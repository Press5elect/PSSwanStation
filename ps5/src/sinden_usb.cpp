/*
	PSSwanStation - the Sinden Lightgun's USB, the parts that need no device
	(sinden_usb.h says what and where from).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "sinden_usb.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fe::sinden::usb
{
namespace
{

std::string format(const char *f, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char *f, ...)
{
	char b[512];
	va_list a;
	va_start(a, f);
	vsnprintf(b, sizeof(b), f, a);
	va_end(a);
	return b;
}

uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

void put32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}

std::string bcd(uint16_t v)
{
	return format("%x.%02x", v >> 8, v & 0xFF);
}

double perSecond(uint32_t interval)
{
	return interval != 0 ? 1e7 / interval : 0;
}

// The GUIDs UVC names its uncompressed formats with: a fourcc, then this.
const uint8_t GuidTail[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

std::string guidName(const uint8_t g[16])
{
	if (memcmp(g + 4, GuidTail, sizeof(GuidTail)) == 0)
	{
		std::string s;
		for (int i = 0; i < 4; i++)
			s += g[i] >= 0x20 && g[i] < 0x7F ? (char)g[i] : '?';
		return s;
	}
	return hex(g, 16);
}

Pixels pixelsOfGuid(const uint8_t g[16])
{
	if (memcmp(g + 4, GuidTail, sizeof(GuidTail)) != 0)
		return Pixels::Unknown;
	if (memcmp(g, "YUY2", 4) == 0 || memcmp(g, "YUYV", 4) == 0)
		return Pixels::Yuy2;
	if (memcmp(g, "NV12", 4) == 0)
		return Pixels::Nv12;
	if (memcmp(g, "Y800", 4) == 0 || memcmp(g, "GREY", 4) == 0 || memcmp(g, "Y8  ", 4) == 0)
		return Pixels::Grey;
	if (memcmp(g, "MJPG", 4) == 0)
		return Pixels::Mjpeg;
	return Pixels::Unknown;
}

const char *typeName(int type)
{
	switch (type)
	{
	case Control: return "control";
	case Isochronous: return "isochronous";
	case Bulk: return "bulk";
	default: return "interrupt";
	}
}

std::string describeEndpoint(const Endpoint& e)
{
	std::string s = format("endpoint 0x%02x: %s %s", e.address, typeName(e.type()), e.in() ? "in" : "out");
	if (e.type() == Isochronous)
	{
		static const char *sync[] = { "no synchronisation", "asynchronous", "adaptive", "synchronous" };
		s += format(" (%s)", sync[(e.attributes >> 2) & 3]);
	}
	const unsigned extra = (e.maxPacket >> 11) & 3;
	if (extra != 0)
		s += format(", %u bytes x %u = %u a (micro)frame", e.maxPacket & 0x7FF, extra + 1, e.bytesPerInterval());
	else
		s += format(", %u bytes", e.maxPacket & 0x7FF);
	s += format(", interval %u", e.interval);
	return s;
}

const char *interfaceName(uint8_t cls, uint8_t sub, uint8_t protocol)
{
	if (cls == 0x0E)
		return sub == 1 ? "video control" : sub == 2 ? "video streaming" : "video";
	if (cls == 0x03)
		return sub == 1 ? (protocol == 1 ? "HID, boot keyboard" : protocol == 2 ? "HID, boot mouse" : "HID, boot") : "HID";
	if (cls == 0x02)
		return sub == 2 ? "CDC ACM (serial port)" : "CDC";
	if (cls == 0x0A)
		return "CDC data";
	if (cls == 0x01)
		return sub == 1 ? "audio control" : sub == 2 ? "audio streaming" : "audio";
	return className(cls);
}

std::string describeFrame(const UvcFrame& f, uint8_t subtype)
{
	std::string s = format("frame %u: %ux%u", f.index, f.width, f.height);
	if (f.continuous)
		s += format(", intervals %u to %u step %u (%.1f to %.1f a second)", f.minInterval, f.maxInterval, f.step,
				perSecond(f.minInterval), perSecond(f.maxInterval));
	else
	{
		s += ", intervals";
		for (uint32_t i : f.intervals)
			s += format(" %u (%.1f)", i, perSecond(i));
	}
	s += format(", default %u", f.defaultInterval);
	if (subtype != 0x11)
		s += format(", largest picture %u bytes", f.maxFrameBytes);
	return s;
}

void readFrame(const uint8_t *d, uint8_t len, uint8_t subtype, UvcFrame& f)
{
	f.index = d[3];
	f.width = le16(d + 5);
	f.height = le16(d + 7);
	size_t at;
	uint8_t kinds;
	if (subtype == 0x11)		// frame based: no buffer size, the interval earlier
	{
		f.defaultInterval = le32(d + 17);
		kinds = d[21];
		at = 26;
	}
	else
	{
		f.maxFrameBytes = le32(d + 17);
		f.defaultInterval = le32(d + 21);
		kinds = len > 25 ? d[25] : 0;
		at = 26;
	}
	if (kinds == 0)
	{
		f.continuous = true;
		if (at + 12 <= len)
		{
			f.minInterval = le32(d + at);
			f.maxInterval = le32(d + at + 4);
			f.step = le32(d + at + 8);
		}
	}
	else
		for (unsigned i = 0; i < kinds && at + 4 * (i + 1) <= len; i++)
			f.intervals.push_back(le32(d + at + 4 * i));
}

// Bits from a report, the first at `bit` (HID's order: least significant first).
uint32_t bits(const uint8_t *d, size_t n, uint32_t bit, unsigned size)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < size && i < 32; i++)
	{
		const uint32_t at = bit + i;
		if (at / 8 < n && (d[at / 8] >> (at % 8)) & 1)
			v |= 1u << i;
	}
	return v;
}

} // namespace

std::string hex(const uint8_t *p, size_t n, size_t most)
{
	std::string s;
	char b[4];
	for (size_t i = 0; i < n && i < most; i++)
	{
		snprintf(b, sizeof(b), "%02x", p[i]);
		if (i != 0 && i % 8 == 0)
			s += ' ';
		s += b;
	}
	if (n > most)
		s += " ...";
	return s;
}

const char *className(uint8_t cls)
{
	switch (cls)
	{
	case 0x00: return "per interface";
	case 0x01: return "audio";
	case 0x02: return "CDC";
	case 0x03: return "HID";
	case 0x05: return "physical";
	case 0x06: return "image";
	case 0x07: return "printer";
	case 0x08: return "storage";
	case 0x09: return "hub";
	case 0x0A: return "CDC data";
	case 0x0B: return "smart card";
	case 0x0E: return "video";
	case 0x10: return "audio/video";
	case 0xDC: return "diagnostic";
	case 0xE0: return "wireless";
	case 0xEF: return "miscellaneous";
	case 0xFE: return "application";
	case 0xFF: return "vendor's own";
	default: return "other";
	}
}

const char *pixelsName(Pixels p)
{
	switch (p)
	{
	case Pixels::Yuy2: return "YUY2";
	case Pixels::Nv12: return "NV12";
	case Pixels::Grey: return "grey";
	case Pixels::Mjpeg: return "MJPEG";
	default: return "unknown";
	}
}

bool parseDevice(const uint8_t *d, size_t n, DeviceDescriptor& out)
{
	out = DeviceDescriptor();
	if (n < 18 || d[0] < 18 || d[1] != 0x01)
		return false;
	out.bcdUSB = le16(d + 2);
	out.cls = d[4];
	out.subclass = d[5];
	out.protocol = d[6];
	out.maxPacket0 = d[7];
	out.vendor = le16(d + 8);
	out.product = le16(d + 10);
	out.bcdDevice = le16(d + 12);
	out.iManufacturer = d[14];
	out.iProduct = d[15];
	out.iSerial = d[16];
	out.configurations = d[17];
	out.ok = true;
	return true;
}

std::string describeDevice(const DeviceDescriptor& d)
{
	if (!d.ok)
		return "not read";
	return format("USB %s, class %u/%u/%u (%s), endpoint 0 %u bytes, %04x:%04x release %s, strings %u/%u/%u, "
			"%u configuration%s", bcd(d.bcdUSB).c_str(), d.cls, d.subclass, d.protocol, className(d.cls), d.maxPacket0,
			d.vendor, d.product, bcd(d.bcdDevice).c_str(), d.iManufacturer, d.iProduct, d.iSerial, d.configurations,
			d.configurations == 1 ? "" : "s");
}

uint32_t UvcFrame::shortestInterval() const
{
	if (continuous)
		return minInterval != 0 ? minInterval : defaultInterval;
	uint32_t best = 0;
	for (uint32_t i : intervals)
		if (i != 0 && (best == 0 || i < best))
			best = i;
	return best != 0 ? best : defaultInterval;
}

const Endpoint *AltSetting::endpoint(int type, bool in) const
{
	for (const Endpoint& e : endpoints)
		if (e.type() == type && e.in() == in)
			return &e;
	return nullptr;
}

const AltSetting *Configuration::find(uint8_t number, uint8_t alt) const
{
	for (const AltSetting& a : alts)
		if (a.number == number && a.alt == alt)
			return &a;
	return nullptr;
}

int Configuration::indexOf(uint8_t number) const
{
	std::vector<uint8_t> seen;
	for (const AltSetting& a : alts)
	{
		if (std::find(seen.begin(), seen.end(), a.number) != seen.end())
			continue;
		if (a.number == number)
			return (int)seen.size();
		seen.push_back(a.number);
	}
	return -1;
}

uint16_t Configuration::uvcVersion() const
{
	for (const AltSetting& a : alts)
		if (a.uvcVersion != 0)
			return a.uvcVersion;
	return 0;
}

bool Configuration::hasVideo() const
{
	for (const AltSetting& a : alts)
		if (a.cls == 0x0E && a.subclass == 2)
			return true;
	return false;
}

bool parseConfiguration(const uint8_t *d, size_t n, Configuration& out, std::vector<std::string> *lines)
{
	out = Configuration();
	auto say = [&](const std::string& s) {
		if (lines != nullptr)
			lines->push_back(s);
	};
	if (n < 9 || d[1] != 0x02)
	{
		say(format("not a configuration descriptor (%zu bytes: %s)", n, hex(d, n, 16).c_str()));
		return false;
	}
	int current = -1;		// the alternate setting the descriptors belong to (an index into out.alts)
	size_t i = 0;
	while (i + 2 <= n)
	{
		const uint8_t len = d[i], type = d[i + 1];
		if (len < 2 || i + len > n)
		{
			say(format("a descriptor at byte %zu runs past the end (%u bytes of %zu left): stopped there", i, len, n - i));
			break;
		}
		const uint8_t *p = d + i;
		AltSetting *a = current >= 0 ? &out.alts[(size_t)current] : nullptr;
		if (type == 0x02 && len >= 9)
		{
			out.totalLength = le16(p + 2);
			out.interfaceCount = p[4];
			out.value = p[5];
			out.attributes = p[7];
			out.maxPower = p[8];
			say(format("configuration %u: %u interfaces, %u bytes (%zu read), attributes 0x%02x (%s), %u mA", out.value,
					out.interfaceCount, out.totalLength, n, out.attributes, (out.attributes & 0x40) ? "self-powered" : "bus-powered",
					out.maxPower * 2u));
		}
		else if (type == 0x0B && len >= 8)
			say(format("function: interfaces %u to %u, class %u/%u/%u (%s)", p[2], p[2] + p[3] - 1, p[4], p[5], p[6],
					interfaceName(p[4], p[5], p[6])));
		else if (type == 0x04 && len >= 9)
		{
			AltSetting s;
			s.number = p[2];
			s.alt = p[3];
			s.cls = p[5];
			s.subclass = p[6];
			s.protocol = p[7];
			out.alts.push_back(s);
			current = (int)out.alts.size() - 1;
			say(format("interface %u alternate %u: class %u/%u/%u (%s), %u endpoint%s", s.number, s.alt, s.cls, s.subclass,
					s.protocol, interfaceName(s.cls, s.subclass, s.protocol), p[4], p[4] == 1 ? "" : "s"));
		}
		else if (type == 0x05 && len >= 7)
		{
			Endpoint e;
			e.address = p[2];
			e.attributes = p[3];
			e.maxPacket = le16(p + 4);
			e.interval = p[6];
			if (a != nullptr)
				a->endpoints.push_back(e);
			say("  " + describeEndpoint(e));
		}
		else if (type == 0x21 && len >= 9 && a != nullptr && a->cls == 0x03)
		{
			a->hidReportLength = le16(p + 7);
			say(format("  HID %s, country %u, report descriptor %u bytes", bcd(le16(p + 2)).c_str(), p[4], a->hidReportLength));
		}
		else if (type == 0x24 && len >= 3 && a != nullptr && a->cls == 0x0E && a->subclass == 1)
		{
			const uint8_t sub = p[2];
			if (sub == 0x01 && len >= 12)
			{
				a->uvcVersion = le16(p + 3);
				a->uvcClock = le32(p + 7);
				std::string streams;
				for (unsigned k = 0; k < p[11] && 12u + k < len; k++)
					streams += format(" %u", p[12 + k]);
				say(format("  UVC %s, clock %u Hz, streaming interfaces%s", bcd(a->uvcVersion).c_str(), a->uvcClock,
						streams.c_str()));
			}
			else if (sub == 0x02 && len >= 8)
				say(format("  input terminal %u: type 0x%04x%s", p[3], le16(p + 4), le16(p + 4) == 0x0201 ? " (camera)" : ""));
			else if (sub == 0x03 && len >= 9)
				say(format("  output terminal %u: type 0x%04x%s, from %u", p[3], le16(p + 4),
						le16(p + 4) == 0x0101 ? " (streaming)" : "", p[7]));
			else if (sub == 0x05 && len >= 8)
				say(format("  processing unit %u: from %u, controls %s", p[3], p[4], hex(p + 8, std::min<size_t>(p[7], len - 8)).c_str()));
			else if (sub == 0x06 && len >= 22)
				say(format("  extension unit %u: %s, %u controls", p[3], hex(p + 4, 16).c_str(), p[20]));
			else
				say(format("  video control descriptor 0x%02x (%u bytes): %s", sub, len, hex(p, len).c_str()));
		}
		else if (type == 0x24 && len >= 3 && a != nullptr && a->cls == 0x0E && a->subclass == 2)
		{
			const uint8_t sub = p[2];
			if (sub == 0x01 && len >= 13)
			{
				a->vsEndpoint = p[6];
				say(format("  input header: %u formats, endpoint 0x%02x, still capture method %u", p[3], p[6], p[9]));
			}
			else if ((sub == 0x04 || sub == 0x10) && len >= 23)
			{
				UvcFormat f;
				f.subtype = sub;
				f.index = p[3];
				memcpy(f.guid, p + 5, 16);
				f.bitsPerPixel = p[21];
				f.defaultFrame = p[22];
				f.pixels = pixelsOfGuid(f.guid);
				a->formats.push_back(f);
				say(format("  format %u: %s %s, %u bits a pixel, %u frame sizes, default %u", f.index,
						sub == 0x04 ? "uncompressed" : "frame based", guidName(f.guid).c_str(), f.bitsPerPixel, p[4], f.defaultFrame));
			}
			else if (sub == 0x06 && len >= 11)
			{
				UvcFormat f;
				f.subtype = sub;
				f.index = p[3];
				f.defaultFrame = p[6];
				f.pixels = Pixels::Mjpeg;
				a->formats.push_back(f);
				say(format("  format %u: MJPEG, %u frame sizes, default %u", f.index, p[4], f.defaultFrame));
			}
			else if ((sub == 0x05 || sub == 0x07 || sub == 0x11) && len >= 26 && !a->formats.empty())
			{
				UvcFrame f;
				readFrame(p, len, sub, f);
				a->formats.back().frames.push_back(f);
				say("    " + describeFrame(f, sub));
			}
			else if (sub == 0x0D && len >= 6)
				say(format("  colour: primaries %u, transfer %u, matrix %u", p[3], p[4], p[5]));
			else if (sub == 0x03)
				say(format("  still picture sizes (%u bytes): %s", len, hex(p, len).c_str()));
			else
				say(format("  video streaming descriptor 0x%02x (%u bytes): %s", sub, len, hex(p, len).c_str()));
		}
		else if (type == 0x24 && len >= 3 && a != nullptr && a->cls == 0x02)
		{
			const uint8_t sub = p[2];
			if (sub == 0x00 && len >= 5)
				say(format("  CDC %s", bcd(le16(p + 3)).c_str()));
			else if (sub == 0x01 && len >= 5)
				say(format("  call management: capabilities 0x%02x, data interface %u", p[3], p[4]));
			else if (sub == 0x02 && len >= 4)
				say(format("  ACM: capabilities 0x%02x", p[3]));
			else if (sub == 0x06 && len >= 5)
			{
				a->cdcControl = p[3];
				a->cdcData = p[4];
				say(format("  union: control interface %u, data interface %u", p[3], p[4]));
			}
			else
				say(format("  CDC descriptor 0x%02x (%u bytes): %s", sub, len, hex(p, len).c_str()));
		}
		else if (type == 0x25 && len >= 3)
			say(format("  class endpoint descriptor 0x%02x (%u bytes): %s", p[2], len, hex(p, len).c_str()));
		else
			say(format("  descriptor type 0x%02x (%u bytes): %s", type, len, hex(p, len).c_str()));
		i += len;
	}
	out.ok = !out.alts.empty();
	return out.ok;
}

Parts findParts(const Configuration& c)
{
	Parts parts;
	for (const AltSetting& a : c.alts)
	{
		if (a.alt != 0)
			continue;
		if (a.cls == 0x02 && a.subclass == 2 && parts.serialControl < 0)
		{
			parts.serialControl = a.number;
			if (a.cdcData >= 0)
				parts.serialData = a.cdcData;
		}
		else if (a.cls == 0x03)
		{
			if (const Endpoint *e = a.endpoint(Interrupt, true))
			{
				HidPart h;
				h.iface = a.number;
				h.subclass = a.subclass;
				h.protocol = a.protocol;
				h.reportLength = a.hidReportLength;
				h.in = *e;
				parts.hid.push_back(h);
			}
		}
		else if (a.cls == 0x0E && a.subclass == 1 && parts.videoControl < 0)
			parts.videoControl = a.number;
		else if (a.cls == 0x0E && a.subclass == 2 && parts.videoStreaming < 0)
			parts.videoStreaming = a.number;
	}
	// Without a union descriptor: the first CDC data interface after the control one.
	if (parts.serialControl >= 0 && parts.serialData < 0)
		for (const AltSetting& a : c.alts)
			if (a.alt == 0 && a.cls == 0x0A && a.number > parts.serialControl)
			{
				parts.serialData = a.number;
				break;
			}
	if (parts.serialData >= 0)
		for (const AltSetting& a : c.alts)
			if (a.number == parts.serialData && a.endpoint(Bulk, true) != nullptr && a.endpoint(Bulk, false) != nullptr)
			{
				parts.serialIn = *a.endpoint(Bulk, true);
				parts.serialOut = *a.endpoint(Bulk, false);
				break;
			}
	return parts;
}

StreamChoice chooseStream(const Configuration& c, uint8_t streamingInterface, int wantWidth, int wantHeight, int mostWidth,
		int mostHeight)
{
	StreamChoice best;
	const AltSetting *a = c.find(streamingInterface, 0);
	if (a == nullptr || a->formats.empty())
	{
		best.why = format("interface %u has no formats", streamingInterface);
		return best;
	}
	long bestScore = -1;
	unsigned offered = 0;
	for (const UvcFormat& f : a->formats)
	{
		if (f.pixels == Pixels::Unknown)
			continue;
		for (const UvcFrame& fr : f.frames)
		{
			offered++;
			if (fr.width > mostWidth || fr.height > mostHeight || fr.width == 0 || fr.height == 0)
				continue;
			// The size first (how far from the one wanted), then uncompressed before MJPEG.
			const long distance = std::labs((long)fr.width - wantWidth) + std::labs((long)fr.height - wantHeight);
			const long score = distance * 2 + (f.pixels == Pixels::Mjpeg ? 1 : 0);
			if (bestScore >= 0 && score >= bestScore)
				continue;
			bestScore = score;
			best.ok = true;
			best.formatIndex = f.index;
			best.frameIndex = fr.index;
			best.pixels = f.pixels;
			best.width = fr.width;
			best.height = fr.height;
			best.interval = fr.shortestInterval();
			best.maxFrameBytes = fr.maxFrameBytes;
		}
	}
	if (!best.ok)
		best.why = format("none of the %u frame sizes in a known format is %dx%d or smaller", offered, mostWidth, mostHeight);
	else
		best.why = format("format %u frame %u: %ux%u %s at %.1f a second (interval %u), of %u sizes offered", best.formatIndex,
				best.frameIndex, best.width, best.height, pixelsName(best.pixels), perSecond(best.interval), best.interval, offered);
	return best;
}

AltChoice chooseAlt(const Configuration& c, uint8_t streamingInterface, uint32_t payload)
{
	AltChoice best, largest;
	unsigned settings = 0;
	for (const AltSetting& a : c.alts)
	{
		if (a.number != streamingInterface || a.alt == 0)
			continue;
		const Endpoint *e = a.endpoint(Isochronous, true);
		if (e == nullptr)
			continue;
		settings++;
		const uint32_t bytes = e->bytesPerInterval();
		if (!largest.ok || bytes > largest.endpoint.bytesPerInterval())
		{
			largest.ok = true;
			largest.alt = a.alt;
			largest.endpoint = *e;
		}
		if (payload != 0 && bytes >= payload && (!best.ok || bytes < best.endpoint.bytesPerInterval()))
		{
			best.ok = true;
			best.alt = a.alt;
			best.endpoint = *e;
		}
	}
	if (best.ok)
	{
		best.why = format("alternate %u: endpoint 0x%02x, %u bytes a (micro)frame, the smallest of %u that carries %u", best.alt,
				best.endpoint.address, best.endpoint.bytesPerInterval(), settings, payload);
		return best;
	}
	if (largest.ok)
	{
		largest.why = format("alternate %u: endpoint 0x%02x, %u bytes a (micro)frame, the largest of %u (%s)", largest.alt,
				largest.endpoint.address, largest.endpoint.bytesPerInterval(), settings,
				payload == 0 ? "the camera gave no payload size" : "none carries the payload the camera asked for");
		return largest;
	}
	// No isochronous setting: a camera that streams over bulk does it in setting 0.
	if (const AltSetting *a = c.find(streamingInterface, 0))
		if (const Endpoint *e = a->endpoint(Bulk, true))
		{
			AltChoice bulk;
			bulk.ok = true;
			bulk.bulk = true;
			bulk.alt = 0;
			bulk.endpoint = *e;
			bulk.why = format("alternate 0: bulk endpoint 0x%02x, %u bytes a packet", e->address, e->maxPacket & 0x7FF);
			return bulk;
		}
	AltChoice none;
	none.why = format("interface %u has no isochronous or bulk endpoint in", streamingInterface);
	return none;
}

size_t probeLength(uint16_t bcdUVC)
{
	if (bcdUVC >= 0x0150)
		return 48;
	if (bcdUVC >= 0x0110)
		return 34;
	return 26;
}

void packProbe(const Probe& p, uint8_t *o, size_t n)
{
	uint8_t b[48] = {};
	put16(b + 0, p.hint);
	b[2] = p.formatIndex;
	b[3] = p.frameIndex;
	put32(b + 4, p.interval);
	put16(b + 8, p.keyFrameRate);
	put16(b + 10, p.pFrameRate);
	put16(b + 12, p.compQuality);
	put16(b + 14, p.compWindowSize);
	put16(b + 16, p.delay);
	put32(b + 18, p.maxVideoFrameSize);
	put32(b + 22, p.maxPayloadTransferSize);
	put32(b + 26, p.clockFrequency);
	b[30] = p.framingInfo;
	b[31] = p.preferredVersion;
	b[32] = p.minVersion;
	b[33] = p.maxVersion;
	memcpy(o, b, std::min<size_t>(n, sizeof(b)));
}

Probe unpackProbe(const uint8_t *d, size_t n)
{
	uint8_t b[48] = {};
	memcpy(b, d, std::min<size_t>(n, sizeof(b)));
	Probe p;
	p.hint = le16(b + 0);
	p.formatIndex = b[2];
	p.frameIndex = b[3];
	p.interval = le32(b + 4);
	p.keyFrameRate = le16(b + 8);
	p.pFrameRate = le16(b + 10);
	p.compQuality = le16(b + 12);
	p.compWindowSize = le16(b + 14);
	p.delay = le16(b + 16);
	p.maxVideoFrameSize = le32(b + 18);
	p.maxPayloadTransferSize = le32(b + 22);
	p.clockFrequency = le32(b + 26);
	p.framingInfo = b[30];
	p.preferredVersion = b[31];
	p.minVersion = b[32];
	p.maxVersion = b[33];
	return p;
}

std::string describeProbe(const Probe& p, size_t n)
{
	std::string s = format("format %u, frame %u, interval %u (%.1f a second), largest picture %u bytes, payload %u bytes, "
			"hint 0x%04x, quality %u, delay %u ms", p.formatIndex, p.frameIndex, p.interval, perSecond(p.interval),
			p.maxVideoFrameSize, p.maxPayloadTransferSize, p.hint, p.compQuality, p.delay);
	if (n >= 34)
		s += format(", clock %u Hz, framing 0x%02x, versions %u/%u/%u", p.clockFrequency, p.framingInfo, p.preferredVersion,
				p.minVersion, p.maxVersion);
	return s;
}

// ------------------------------------------------------------------ the stream
void Assembler::begin(Pixels pixels, uint16_t width, uint16_t height, size_t capacity)
{
	pixels_ = pixels;
	width_ = width;
	height_ = height;
	capacity_ = capacity;
	filling_.assign(capacity + Padding, 0);
	done_.assign(capacity + Padding, 0);
	fillBytes_ = doneBytes_ = 0;
	fid_ = -1;
	bad_ = overflow_ = false;
	counts_ = Counts();
	lastDrop_.clear();
}

size_t Assembler::expectedBytes() const
{
	const size_t area = (size_t)width_ * height_;
	switch (pixels_)
	{
	case Pixels::Yuy2: return area * 2;
	case Pixels::Nv12: return area * 3 / 2;
	case Pixels::Grey: return area;
	default: return 0;
	}
}

bool Assembler::payload(const uint8_t *p, size_t n)
{
	counts_.payloads++;
	if (n == 0)
	{
		counts_.empty++;
		return false;
	}
	const size_t header = p[0];
	if (n < 2 || header < 2 || header > n)
	{
		counts_.badHeader++;
		return false;
	}
	const uint8_t info = p[1];
	const int fid = info & 0x01;
	bool ready = false;
	// The frame bit changed: the picture before it ended (some cameras never
	// set the end-of-frame bit).
	if (fid_ >= 0 && fid != fid_)
	{
		counts_.fidChanges++;
		if (fillBytes_ > 0 || bad_)
			ready = finish();
	}
	fid_ = fid;
	if (info & 0x40)
	{
		counts_.errorBit++;
		bad_ = true;
	}
	const size_t data = n - header;
	if (data > 0)
	{
		counts_.bytes += data;
		if (fillBytes_ + data > capacity_)
			overflow_ = bad_ = true;
		else
		{
			memcpy(filling_.data() + fillBytes_, p + header, data);
			fillBytes_ += data;
		}
	}
	if (info & 0x02)
	{
		counts_.endOfFrame++;
		if (finish())
			ready = true;
	}
	return ready;
}

bool Assembler::finish()
{
	const size_t bytes = fillBytes_;
	const bool bad = bad_, overflow = overflow_;
	fillBytes_ = 0;
	bad_ = overflow_ = false;
	if (bytes == 0 && !bad)
		return false;
	char why[160];
	if (overflow)
	{
		counts_.dropped++;
		counts_.droppedOverflow++;
		snprintf(why, sizeof(why), "longer than the %zu bytes set aside", capacity_);
		lastDrop_ = why;
		return false;
	}
	if (bad)
	{
		counts_.dropped++;
		counts_.droppedError++;
		snprintf(why, sizeof(why), "the camera set its error bit (%zu bytes)", bytes);
		lastDrop_ = why;
		return false;
	}
	const size_t expected = expectedBytes();
	if (expected != 0 && bytes < expected)
	{
		counts_.dropped++;
		counts_.droppedShort++;
		snprintf(why, sizeof(why), "%zu bytes of %zu", bytes, expected);
		lastDrop_ = why;
		return false;
	}
	if (pixels_ == Pixels::Mjpeg && (bytes < 4 || filling_[0] != 0xFF || filling_[1] != 0xD8))
	{
		counts_.dropped++;
		counts_.droppedNotJpeg++;
		snprintf(why, sizeof(why), "does not begin as a JPEG picture (%02x %02x, %zu bytes)", filling_[0], filling_[1], bytes);
		lastDrop_ = why;
		return false;
	}
	std::swap(filling_, done_);
	doneBytes_ = bytes;
	memset(done_.data() + bytes, 0, Padding);
	counts_.pictures++;
	return true;
}

bool lumaFrom(Pixels pixels, const uint8_t *picture, size_t bytes, int width, int height, uint8_t *luma, int stride)
{
	if (width <= 0 || height <= 0 || stride < width)
		return false;
	const size_t area = (size_t)width * height;
	switch (pixels)
	{
	case Pixels::Yuy2:
		if (bytes < area * 2)
			return false;
		for (int y = 0; y < height; y++)
		{
			const uint8_t *s = picture + (size_t)y * width * 2;
			uint8_t *o = luma + (size_t)y * stride;
			for (int x = 0; x < width; x++)
				o[x] = s[2 * x];
		}
		return true;
	case Pixels::Nv12:
	case Pixels::Grey:
		if (bytes < area)
			return false;
		for (int y = 0; y < height; y++)
			memcpy(luma + (size_t)y * stride, picture + (size_t)y * width, (size_t)width);
		return true;
	default:
		return false;
	}
}

// ------------------------------------------------------------------ the serial port
namespace serial
{
std::array<uint8_t, 7> frame(uint8_t command, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4)
{
	return { 0xAA, command, p1, p2, p3, p4, 0xBB };
}

size_t answerLength(uint8_t command)
{
	switch (command)
	{
	case Firmware: return 2;
	case CameraName: return 15;
	case CalibrationX:
	case CalibrationY: return 2;
	default: return 0;
	}
}

const char *commandName(uint8_t command)
{
	switch (command)
	{
	case Firmware: return "firmware";
	case CameraName: return "camera name";
	case CalibrationX: return "calibration X";
	case CalibrationY: return "calibration Y";
	default: return "?";
	}
}

std::string firmware(const uint8_t *d, size_t n)
{
	return n >= 2 ? format("%u.%u", d[0], d[1]) : std::string();
}

std::string cameraName(const uint8_t *d, size_t n)
{
	std::string s;
	for (size_t i = 0; i < n && d[i] != 0; i++)
		s += d[i] >= 0x20 && d[i] < 0x7F ? (char)d[i] : '?';
	while (!s.empty() && s.back() == ' ')
		s.pop_back();
	return s;
}

bool calibration(const uint8_t *d, size_t n, int& raw, double& value)
{
	if (n < 2)
		return false;
	raw = (d[0] << 8) | d[1];
	value = (raw - 10000) / 100.0;
	return true;
}

void lineCoding(uint32_t baud, uint8_t out[7])
{
	put32(out, baud);
	out[4] = 0;		// 1 stop bit
	out[5] = 0;		// no parity
	out[6] = 8;		// 8 data bits
}

std::string describeLineCoding(const uint8_t *d, size_t n)
{
	if (n < 7)
		return format("%zu bytes: %s", n, hex(d, n).c_str());
	static const char *stops[] = { "1", "1.5", "2" }, *parity[] = { "none", "odd", "even", "mark", "space" };
	return format("%u baud, %u data bits, parity %s, %s stop bit%s", le32(d), d[6], d[5] < 5 ? parity[d[5]] : "?",
			d[4] < 3 ? stops[d[4]] : "?", d[4] == 0 ? "" : "s");
}
}

// ------------------------------------------------------------------ the buttons
HidLayout parseHid(const uint8_t *d, size_t n)
{
	HidLayout L;
	struct Globals
	{
		uint32_t page = 0;
		int32_t logicalMin = 0;
		uint32_t size = 0, count = 0;
		uint8_t id = 0;
	} g;
	std::vector<Globals> pushed;
	std::vector<uint32_t> usages;
	uint32_t usageMin = 0, usageMax = 0;
	bool range = false;
	std::vector<uint32_t> applications;		// the application collections' usages, innermost last
	std::vector<bool> collectionIsApplication;
	std::vector<std::pair<uint8_t, uint32_t>> nextBit;	// per report ID
	auto bitOf = [&](uint8_t id) -> uint32_t& {
		for (auto& e : nextBit)
			if (e.first == id)
				return e.second;
		nextBit.push_back({ id, 0 });
		return nextBit.back().second;
	};
	size_t i = 0;
	bool broken = false;
	while (i < n)
	{
		const uint8_t prefix = d[i];
		if (prefix == 0xFE)		// a long item: skipped
		{
			if (i + 2 >= n)
				break;
			i += 3 + d[i + 1];
			continue;
		}
		size_t size = prefix & 3;
		if (size == 3)
			size = 4;
		if (i + 1 + size > n)
		{
			broken = true;
			break;
		}
		uint32_t u = 0;
		for (size_t k = 0; k < size; k++)
			u |= (uint32_t)d[i + 1 + k] << (8 * k);
		int32_t s = (int32_t)u;
		if (size == 1)
			s = (int8_t)u;
		else if (size == 2)
			s = (int16_t)u;
		const uint8_t type = (prefix >> 2) & 3, tag = prefix >> 4;
		i += 1 + size;
		if (type == 1)
		{
			switch (tag)
			{
			case 0: g.page = u; break;
			case 1: g.logicalMin = s; break;
			case 7: g.size = u; break;
			case 8:
				g.id = (uint8_t)u;
				L.reportIds = true;
				break;
			case 9: g.count = u; break;
			case 10: pushed.push_back(g); break;
			case 11:
				if (!pushed.empty())
				{
					g = pushed.back();
					pushed.pop_back();
				}
				break;
			default: break;
			}
		}
		else if (type == 2)
		{
			const uint32_t full = size == 4 ? u : (g.page << 16) | u;
			if (tag == 0)
				usages.push_back(full);
			else if (tag == 1)
			{
				usageMin = full;
				range = true;
			}
			else if (tag == 2)
			{
				usageMax = full;
				range = true;
			}
		}
		else if (type == 0)
		{
			if (tag == 10)		// collection
			{
				const bool application = u == 1;
				collectionIsApplication.push_back(application);
				if (application)
					applications.push_back(!usages.empty() ? usages[0] : usageMin);
			}
			else if (tag == 12)		// end of collection
			{
				if (!collectionIsApplication.empty())
				{
					if (collectionIsApplication.back() && !applications.empty())
						applications.pop_back();
					collectionIsApplication.pop_back();
				}
			}
			else if (tag == 8)		// input
			{
				uint32_t& at = bitOf(g.id);
				const bool constant = (u & 1) != 0, variable = (u & 2) != 0;
				const uint32_t first = !usages.empty() ? usages[0] : range ? usageMin : 0;
				const uint16_t page = (uint16_t)(first >> 16);
				const uint32_t application = applications.empty() ? 0 : applications.back();
				if (!constant && g.size > 0 && g.count > 0)
				{
					HidField f;
					f.reportId = g.id;
					f.bit = at;
					f.size = (uint8_t)std::min<uint32_t>(g.size, 32);
					f.count = (uint16_t)std::min<uint32_t>(g.count, 256);
					f.page = page;
					f.usageMin = (uint16_t)first;
					f.usageMax = (uint16_t)(range ? usageMax : !usages.empty() ? usages.back() : first);
					f.logicalMin = g.logicalMin;
					if (variable && page == 0x09 && g.size == 1)
						f.kind = application == 0x00010002 ? HidField::MouseButtons : HidField::JoystickButtons;
					else if (variable && page == 0x07 && f.usageMin >= 0xE0 && f.usageMin <= 0xE7 && g.size == 1)
						f.kind = HidField::KeyModifiers;
					else if (!variable && page == 0x07)
						f.kind = HidField::Keys;
					if (f.kind != HidField::Other)
						L.fields.push_back(f);
				}
				at += g.size * g.count;
			}
			usages.clear();
			range = false;
			usageMin = usageMax = 0;
		}
	}
	for (const HidField& f : L.fields)
	{
		if (!L.summary.empty())
			L.summary += "; ";
		if (L.reportIds)
			L.summary += format("report %u: ", f.reportId);
		switch (f.kind)
		{
		case HidField::MouseButtons:
		case HidField::JoystickButtons:
			L.summary += format("%s buttons %u to %u (bits %u to %u)", f.kind == HidField::MouseButtons ? "mouse" : "joystick",
					f.usageMin, f.usageMin + f.count - 1, f.bit, f.bit + f.count - 1);
			break;
		case HidField::KeyModifiers:
			L.summary += format("modifier keys (bits %u to %u)", f.bit, f.bit + f.count - 1);
			break;
		case HidField::Keys:
			L.summary += format("%u keys of %u bits from bit %u", f.count, f.size, f.bit);
			break;
		default: break;
		}
	}
	if (L.summary.empty())
		L.summary = "no buttons or keys";
	if (broken)
		L.summary += " (the descriptor's last item runs past its end)";
	L.ok = !L.fields.empty();
	return L;
}

HidLayout bootLayout(uint8_t protocol)
{
	HidLayout L;
	if (protocol == 1)
	{
		HidField m;
		m.kind = HidField::KeyModifiers;
		m.size = 1;
		m.count = 8;
		m.page = 0x07;
		m.usageMin = 0xE0;
		m.usageMax = 0xE7;
		L.fields.push_back(m);
		HidField k;
		k.kind = HidField::Keys;
		k.bit = 16;
		k.size = 8;
		k.count = 6;
		k.page = 0x07;
		L.fields.push_back(k);
		L.summary = "boot keyboard: modifier keys (bits 0 to 7), 6 keys of 8 bits from bit 16";
	}
	else if (protocol == 2)
	{
		HidField b;
		b.kind = HidField::MouseButtons;
		b.size = 1;
		b.count = 3;
		b.page = 0x09;
		b.usageMin = 1;
		b.usageMax = 3;
		L.fields.push_back(b);
		L.summary = "boot mouse: buttons 1 to 3 (bits 0 to 2)";
	}
	L.ok = !L.fields.empty();
	return L;
}

bool decodeHid(const HidLayout& layout, const uint8_t *r, size_t n, Buttons& io)
{
	uint8_t id = 0;
	if (layout.reportIds)
	{
		if (n == 0)
			return false;
		id = r[0];
		r++;
		n--;
	}
	bool any = false;
	for (const HidField& f : layout.fields)
	{
		if (f.reportId != id)
			continue;
		any = true;
		switch (f.kind)
		{
		case HidField::MouseButtons:
		case HidField::JoystickButtons:
		{
			uint32_t& word = f.kind == HidField::MouseButtons ? io.mouse : io.joystick;
			for (unsigned j = 0; j < f.count; j++)
			{
				const unsigned button = f.usageMin + j;		// 1 is the first
				if (button == 0 || button > 32)
					continue;
				const uint32_t bit = 1u << (button - 1);
				if (bits(r, n, f.bit + j, 1))
					word |= bit;
				else
					word &= ~bit;
			}
			break;
		}
		case HidField::KeyModifiers:
			for (unsigned j = 0; j < f.count; j++)
			{
				const unsigned usage = f.usageMin + j;
				if (usage < 0xE0 || usage > 0xE7)
					continue;
				const uint8_t bit = (uint8_t)(1u << (usage - 0xE0));
				if (bits(r, n, f.bit + j, 1))
					io.modifiers |= bit;
				else
					io.modifiers &= (uint8_t)~bit;
			}
			break;
		case HidField::Keys:
		{
			memset(io.keys, 0, sizeof(io.keys));
			unsigned held = 0;
			for (unsigned j = 0; j < f.count && held < 6; j++)
			{
				const int32_t v = (int32_t)bits(r, n, f.bit + j * f.size, f.size);
				const int32_t usage = f.usageMin + (v - f.logicalMin);
				// 0 is no key; 1 to 3 are the keyboard's error codes (too many keys).
				if (usage > 3 && usage < 256)
					io.keys[held++] = (uint8_t)usage;
			}
			break;
		}
		default: break;
		}
	}
	return any;
}

std::string describeButtons(const Buttons& b)
{
	return format("mouse %#x, joystick %#x, modifiers 0x%02x, keys %02x %02x %02x %02x %02x %02x", b.mouse, b.joystick,
			b.modifiers, b.keys[0], b.keys[1], b.keys[2], b.keys[3], b.keys[4], b.keys[5]);
}

// ------------------------------------------------------------------ libSceUsbd's transfers
TransferLayout findTransferLayout(const uint8_t *mem, size_t n, const TransferValues& v)
{
	TransferLayout L;
	std::vector<bool> taken(n, false);
	auto mark = [&](int at, size_t bytes) {
		for (size_t k = 0; k < bytes && at >= 0 && (size_t)at + k < n; k++)
			taken[(size_t)at + k] = true;
	};
	// Where a value is, once: -1 not there, -2 there more than once.
	auto find = [&](uint64_t value, size_t bytes, size_t step, size_t from) -> int {
		int at = -1;
		for (size_t o = from; o + bytes <= n; o += step)
		{
			bool free = true;
			for (size_t k = 0; k < bytes; k++)
				free = free && !taken[o + k];
			if (!free)
				continue;
			uint64_t x = 0;
			memcpy(&x, mem + o, bytes);
			if (x != value)
				continue;
			if (at >= 0)
				return -2;
			at = (int)o;
		}
		return at;
	};
	struct
	{
		const char *name;
		int *at;
		uint64_t value;
		size_t bytes, step;
	} wanted[] = {
		{ "handle", &L.handle, v.handle, 8, 8 },
		{ "callback", &L.callback, v.callback, 8, 8 },
		{ "user data", &L.userData, v.userData, 8, 8 },
		{ "buffer", &L.buffer, v.buffer, 8, 8 },
		{ "timeout", &L.timeout, v.timeout, 4, 4 },
		{ "length", &L.length, v.length, 4, 4 },
		{ "packets", &L.packets, v.packets, 4, 4 },
		{ "endpoint", &L.endpoint, v.endpoint, 1, 1 },
	};
	for (auto& w : wanted)
	{
		*w.at = find(w.value, w.bytes, w.step, 0);
		if (*w.at < 0)
		{
			L.why = format("the %s %s", w.name, *w.at == -1 ? "is not in it" : "is in it more than once");
			return L;
		}
		mark(*w.at, w.bytes);
	}
	// libusb's status, length and actual length follow each other in both
	// layouts; a transfer not yet sent has status and actual length 0.
	L.status = L.length - 4;
	L.actual = L.length + 4;
	if (L.status < 0 || (size_t)L.actual + 4 > n || taken[(size_t)L.status] || taken[(size_t)L.actual])
	{
		L.why = format("no room for the status and the actual length around the length at +%d", L.length);
		return L;
	}
	uint32_t status = 0, actual = 0;
	memcpy(&status, mem + L.status, 4);
	memcpy(&actual, mem + L.actual, 4);
	if (status != 0 || actual != 0)
	{
		L.why = format("the words around the length at +%d are not 0 (%#x, %#x)", L.length, status, actual);
		return L;
	}
	mark(L.status, 4);
	mark(L.actual, 4);
	// The packets' descriptors: every one's length set to packetLength; the
	// first two give their size.
	int a = -1, b = -1;
	for (size_t o = (size_t)L.packets + 4; o + 4 <= n; o += 4)
	{
		uint32_t x = 0;
		memcpy(&x, mem + o, 4);
		if (x != v.packetLength || taken[o])
			continue;
		if (a < 0)
			a = (int)o;
		else
		{
			b = (int)o;
			break;
		}
	}
	if (a < 0 || b < 0 || v.packets < 2)
	{
		L.why = a < 0 ? "the packets' lengths are not in it" : "only one packet's length in it";
		return L;
	}
	const int stride = b - a;
	if (stride != 8 && stride != 12 && stride != 16)
	{
		L.why = format("the packets' lengths are %d bytes apart (+%d, +%d)", stride, a, b);
		return L;
	}
	for (uint32_t k = 0; k < v.packets; k++)
	{
		const size_t o = (size_t)a + (size_t)k * (size_t)stride;
		uint32_t x = 0;
		if (o + 4 <= n)
			memcpy(&x, mem + o, 4);
		if (x != v.packetLength)
		{
			L.why = format("packet %u's length is not where the others say (+%zu)", k, o);
			return L;
		}
	}
	L.packetDescriptors = a;
	L.packetStride = stride;
	L.ok = true;
	return L;
}

std::string describeLayout(const TransferLayout& l)
{
	return format("handle +%d, endpoint +%d, timeout +%d, status +%d, length +%d, actual +%d, callback +%d, user data +%d, "
			"buffer +%d, packets +%d, packet descriptors +%d (%d bytes each)", l.handle, l.endpoint, l.timeout, l.status,
			l.length, l.actual, l.callback, l.userData, l.buffer, l.packets, l.packetDescriptors, l.packetStride);
}

}
