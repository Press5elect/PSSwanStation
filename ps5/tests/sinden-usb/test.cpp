// PSSwanStation - the Sinden Lightgun's USB decoders (src/sinden_usb.cpp), on a PC:
// a composite camera, serial port and HID device's configuration descriptor,
// the camera's choice and its UVC stream put together from packets, the serial
// port's frames, the buttons' reports, and libSceUsbd's transfer layout found
// in libusb's and FreeBSD's layouts.
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The configuration descriptor is gun_descriptor.h's.
#include "../../src/sinden_usb.h"
#include "gun_descriptor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace fe::sinden;
using namespace fe::sinden::usb;

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	failures += ok ? 0 : 1;
}

bool contains(const std::vector<std::string>& lines, const char *text)
{
	for (const std::string& l : lines)
		if (l.find(text) != std::string::npos)
			return true;
	return false;
}

// One UVC payload: a 12-byte header (with a presentation time and a clock
// reference, as most cameras send) and `data`.
std::vector<uint8_t> packet(int fid, bool eof, bool error, const uint8_t *data, size_t n)
{
	std::vector<uint8_t> p = { 12, (uint8_t)(0x80 | 0x0C | (fid & 1) | (eof ? 0x02 : 0) | (error ? 0x40 : 0)), 1, 2, 3, 4, 5, 6,
		7, 8, 9, 10 };
	p.insert(p.end(), data, data + n);
	return p;
}

void testDescriptors()
{
	const std::vector<uint8_t> raw = gunConfiguration();
	Configuration c;
	std::vector<std::string> lines;
	check(parseConfiguration(raw.data(), raw.size(), c, &lines), "the configuration descriptor is decoded");
	check(c.value == 1 && c.interfaceCount == 5 && c.totalLength == raw.size() && c.maxPower == 250, "its header");
	check(c.alts.size() == 11, "every interface descriptor (5 interfaces, 6 more alternate settings)");
	check(c.uvcVersion() == 0x0110 && c.hasVideo(), "UVC 1.10, a streaming interface");
	const AltSetting *vs = c.find(1, 0);
	check(vs != nullptr && vs->formats.size() == 2 && vs->vsEndpoint == 0x81, "the streaming interface's two formats and its endpoint");
	if (vs != nullptr && vs->formats.size() == 2)
	{
		check(vs->formats[0].pixels == Pixels::Mjpeg && vs->formats[0].frames.size() == 3, "format 1: MJPEG, 3 sizes");
		check(vs->formats[1].pixels == Pixels::Yuy2 && vs->formats[1].bitsPerPixel == 16 && vs->formats[1].frames.size() == 3,
				"format 2: YUY2, 16 bits, 3 sizes");
		const UvcFrame& f = vs->formats[1].frames[1];
		check(f.width == 320 && f.height == 240 && f.intervals.size() == 2 && f.shortestInterval() == 166666
						&& f.maxFrameBytes == 320 * 240 * 2,
				"YUY2 320x240: its intervals, the shortest 60 a second, its largest picture");
	}
	check(c.indexOf(0) == 0 && c.indexOf(1) == 1 && c.indexOf(2) == 2 && c.indexOf(4) == 4 && c.indexOf(9) == -1,
			"interfaces' indexes (alternate settings counted once)");
	check(contains(lines, "UVC 1.10, clock 48000000 Hz, streaming interfaces 1"), "the boot log's line for the UVC header");
	check(contains(lines, "input terminal 1: type 0x0201 (camera)"), "the camera terminal");
	check(contains(lines, "format 2: uncompressed YUY2, 16 bits a pixel, 3 frame sizes"), "the YUY2 format named");
	check(contains(lines, "frame 2: 320x240, intervals 166666 (60.0) 333333 (30.0)"), "an MJPEG frame size and its rates");
	check(contains(lines, "endpoint 0x81: isochronous in (asynchronous), 1024 bytes x 3 = 3072 a (micro)frame, interval 1"),
			"a high-bandwidth isochronous endpoint");
	check(contains(lines, "union: control interface 2, data interface 3"), "the CDC union");
	check(contains(lines, "HID 1.11, country 0, report descriptor 52 bytes"), "the HID descriptor");
	for (const std::string& l : lines)
		printf("     | %s\n", l.c_str());

	const Parts parts = findParts(c);
	check(parts.serialControl == 2 && parts.serialData == 3 && parts.serialIn.address == 0x86 && parts.serialOut.address == 0x05,
			"the serial port's interfaces and bulk endpoints");
	check(parts.hid.size() == 1 && parts.hid[0].iface == 4 && parts.hid[0].in.address == 0x87 && parts.hid[0].protocol == 2
					&& parts.hid[0].reportLength == 52,
			"the HID interface and its interrupt endpoint");
	check(parts.videoControl == 0 && parts.videoStreaming == 1, "the video interfaces");

	const StreamChoice s = chooseStream(c, 1);
	check(s.ok && s.pixels == Pixels::Yuy2 && s.width == 320 && s.height == 240 && s.formatIndex == 2 && s.frameIndex == 2
					&& s.interval == 166666,
			"320x240 chosen in YUY2 (uncompressed before MJPEG), at 60 a second");
	printf("     | %s\n", s.why.c_str());
	const StreamChoice small = chooseStream(c, 1, 160, 120);
	check(small.ok && small.pixels == Pixels::Mjpeg && small.width == 160, "160x120 only in MJPEG: MJPEG");
	const StreamChoice none = chooseStream(c, 1, 320, 240, 100, 100);
	check(!none.ok, "no size small enough: none, and why");

	AltChoice a = chooseAlt(c, 1, 1500);
	check(a.ok && !a.bulk && a.alt == 4 && a.endpoint.bytesPerInterval() == 1536, "a payload of 1500: 2 x 768, the smallest that carries it");
	printf("     | %s\n", a.why.c_str());
	a = chooseAlt(c, 1, 100);
	check(a.ok && a.alt == 1, "a payload of 100: 128 bytes");
	a = chooseAlt(c, 1, 0);
	check(a.ok && a.alt == 6 && a.endpoint.bytesPerInterval() == 3072, "no payload given: the largest");
	a = chooseAlt(c, 1, 9000);
	check(a.ok && a.alt == 6, "a payload too large for any: the largest");

	// A descriptor cut short is decoded up to where it ends.
	Configuration cut;
	std::vector<std::string> cutLines;
	parseConfiguration(raw.data(), 100, cut, &cutLines);
	check(!cutLines.empty() && cutLines.back().find("runs past the end") != std::string::npos, "a cut descriptor said so");

	DeviceDescriptor d;
	const uint8_t dev[18] = { 18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 0xC0, 0x16, 0x38, 0x0F, 0x00, 0x01, 1, 2, 3, 1 };
	check(parseDevice(dev, sizeof(dev), d) && d.vendor == 0x16C0 && d.product == 0x0F38 && d.cls == 0xEF && d.maxPacket0 == 64,
			"a device descriptor");
	printf("     | %s\n", describeDevice(d).c_str());
}

void testProbe()
{
	check(probeLength(0x0100) == 26 && probeLength(0x0110) == 34 && probeLength(0x0150) == 48, "the probe's length by UVC version");
	Probe p;
	p.hint = 1;
	p.formatIndex = 2;
	p.frameIndex = 3;
	p.interval = 166666;
	p.maxVideoFrameSize = 153600;
	p.maxPayloadTransferSize = 3072;
	p.clockFrequency = 48000000;
	uint8_t b[34];
	packProbe(p, b, sizeof(b));
	check(b[0] == 1 && b[2] == 2 && b[3] == 3 && b[4] == 0x0A && b[5] == 0x8B && b[6] == 0x02 && b[22] == 0x00 && b[23] == 0x0C,
			"the probe packed little-endian at UVC's offsets");
	const Probe q = unpackProbe(b, sizeof(b));
	check(q.interval == 166666 && q.maxVideoFrameSize == 153600 && q.maxPayloadTransferSize == 3072 && q.clockFrequency == 48000000,
			"and read back");
	const Probe shorter = unpackProbe(b, 26);
	check(shorter.maxPayloadTransferSize == 3072 && shorter.clockFrequency == 0, "a UVC 1.0 probe has no clock");
	printf("     | %s\n", describeProbe(q, 34).c_str());
}

void testAssembler()
{
	const int w = 8, h = 4;		// a small YUY2 picture: 64 bytes
	uint8_t picture[w * h * 2];
	for (int i = 0; i < w * h * 2; i++)
		picture[i] = (uint8_t)(i * 3);
	Assembler a;
	a.begin(Pixels::Yuy2, w, h, w * h * 2 + 32);
	check(a.expectedBytes() == 64, "a YUY2 picture's size");

	// A picture in three packets, the last with the end-of-frame bit.
	bool done = false;
	auto p1 = packet(0, false, false, picture, 20), p2 = packet(0, false, false, picture + 20, 30),
		 p3 = packet(0, true, false, picture + 50, 14);
	done = a.payload(p1.data(), p1.size()) || done;
	done = a.payload(p2.data(), p2.size()) || done;
	check(!done, "not whole before its end");
	done = a.payload(p3.data(), p3.size());
	check(done && a.pictureBytes() == 64 && memcmp(a.picture(), picture, 64) == 0, "whole at the end-of-frame bit, byte for byte");

	// The next one without the end-of-frame bit: it ends when the frame bit changes.
	auto q1 = packet(1, false, false, picture, 40), q2 = packet(1, false, false, picture + 40, 24);
	auto header = packet(1, false, false, nullptr, 0);
	check(!a.payload(q1.data(), q1.size()) && !a.payload(header.data(), header.size()) && !a.payload(q2.data(), q2.size()),
			"a picture without the end-of-frame bit, a header-only packet in it");
	auto r1 = packet(0, false, false, picture, 10);
	check(a.payload(r1.data(), r1.size()) && a.pictureBytes() == 64 && memcmp(a.picture(), picture, 64) == 0,
			"whole when the frame bit changes, the next one begun");

	// The rest of that one is lost (an empty packet, a bad header), so it is short.
	const uint8_t badHeader[3] = { 9, 0x80, 0 };
	check(!a.payload(nullptr, 0) && !a.payload(badHeader, sizeof(badHeader)), "an empty packet and a bad header are skipped");
	auto r2 = packet(0, true, false, picture + 10, 20);
	check(!a.payload(r2.data(), r2.size()) && a.counts().droppedShort == 1, "a short picture is dropped");
	check(a.lastDrop() == "30 bytes of 64", "and said in words (\"30 bytes of 64\")");

	// The error bit drops the picture it is in.
	auto e1 = packet(1, false, true, picture, 40), e2 = packet(1, true, false, picture + 40, 24);
	a.payload(e1.data(), e1.size());
	check(!a.payload(e2.data(), e2.size()) && a.counts().droppedError == 1, "a picture with the error bit set is dropped");

	// Too long for the room set aside.
	std::vector<uint8_t> big(200, 7);
	auto o1 = packet(0, false, false, big.data(), big.size()), o2 = packet(0, true, false, picture, 4);
	a.payload(o1.data(), o1.size());
	check(!a.payload(o2.data(), o2.size()) && a.counts().droppedOverflow == 1, "a picture longer than its room is dropped");

	// And the next good one comes through.
	auto g1 = packet(1, true, false, picture, 64);
	check(a.payload(g1.data(), g1.size()) && a.counts().pictures == 3, "then a good one again (3 in all)");
	check(a.counts().empty == 1 && a.counts().badHeader == 1 && a.counts().errorBit == 1, "the counts of what was skipped");

	uint8_t luma[w * h];
	check(lumaFrom(Pixels::Yuy2, a.picture(), a.pictureBytes(), w, h, luma, w) && luma[0] == picture[0] && luma[1] == picture[2]
					&& luma[w] == picture[w * 2] && luma[w * h - 1] == picture[w * h * 2 - 2],
			"YUY2's brightness taken out");
	uint8_t strided[(w + 4) * h];
	memset(strided, 0xEE, sizeof(strided));
	check(lumaFrom(Pixels::Yuy2, a.picture(), a.pictureBytes(), w, h, strided, w + 4) && strided[w] == 0xEE
					&& strided[w + 4] == picture[w * 2],
			"into rows further apart");
	check(!lumaFrom(Pixels::Yuy2, a.picture(), 10, w, h, luma, w), "not from too few bytes");

	// MJPEG: whatever length, but it must begin as a JPEG picture.
	Assembler m;
	m.begin(Pixels::Mjpeg, 320, 240, 1000);
	const uint8_t jpeg[] = { 0xFF, 0xD8, 0xFF, 0xDB, 1, 2, 3, 0xFF, 0xD9 }, notJpeg[] = { 1, 2, 3, 4, 5 };
	auto j1 = packet(0, true, false, jpeg, sizeof(jpeg)), j2 = packet(1, true, false, notJpeg, sizeof(notJpeg));
	check(m.payload(j1.data(), j1.size()) && m.pictureBytes() == sizeof(jpeg), "an MJPEG picture");
	bool padded = true;
	for (size_t k = 0; k < Assembler::Padding; k++)
		padded = padded && m.picture()[sizeof(jpeg) + k] == 0;
	check(padded, "with zeroed room after it for FFmpeg");
	check(!m.payload(j2.data(), j2.size()) && m.counts().droppedNotJpeg == 1, "one that is not a JPEG picture is dropped");

	Rate r;
	for (int i = 0; i <= 60; i++)
		r.note(1.0 + i / 60.0);
	check(std::fabs(r.perSecond() - 60.0) < 1e-9 && std::fabs(r.seconds() - 1.0) < 1e-9, "61 pictures over a second: 60 a second");
}

void testSerial()
{
	const auto f = serial::frame(serial::Firmware);
	const uint8_t want[7] = { 0xAA, 101, 0, 0, 0, 0, 0xBB };
	check(memcmp(f.data(), want, 7) == 0, "command 101's frame: AA 65 00 00 00 00 BB");
	check(serial::answerLength(101) == 2 && serial::answerLength(102) == 15 && serial::answerLength(104) == 2
					&& serial::answerLength(105) == 2 && serial::answerLength(103) == 0,
			"the answers' lengths (103, the name's write, is not one of them)");
	const uint8_t fw[2] = { 1, 9 };
	check(serial::firmware(fw, 2) == "1.9" && serial::firmware(fw, 1).empty(), "the firmware's version");
	const uint8_t name[15] = { 'S', 'i', 'n', 'd', 'e', 'n', 'C', 'a', 'm', 'C', ' ', ' ', ' ', ' ', ' ' };
	check(serial::cameraName(name, 15) == "SindenCamC", "the camera's name, its spaces cut");
	int raw = 0;
	double v = 0;
	const uint8_t zero[2] = { 0x27, 0x10 }, plus[2] = { 0x27, 0x8C }, minus[2] = { 0x26, 0x94 };
	check(serial::calibration(zero, 2, raw, v) && raw == 10000 && v == 0.0, "calibration 10000: 0");
	check(serial::calibration(plus, 2, raw, v) && raw == 10124 && std::fabs(v - 1.24) < 1e-9, "10124: 1.24");
	check(serial::calibration(minus, 2, raw, v) && raw == 9876 && std::fabs(v + 1.24) < 1e-9, "9876: -1.24");
	check(!serial::calibration(minus, 1, raw, v), "one byte is not an answer");
	uint8_t lc[7];
	serial::lineCoding(115200, lc);
	const uint8_t wantLc[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };
	check(memcmp(lc, wantLc, 7) == 0 && serial::describeLineCoding(lc, 7) == "115200 baud, 8 data bits, parity none, 1 stop bit",
			"115200 8N1's line coding");
}

void testHid()
{
	// A mouse (report 1: five buttons, X, Y, the wheel) and a keyboard (report
	// 2: modifiers, a reserved byte, six keys), as a Sinden in its default mode.
	const uint8_t descriptor[] = {
		0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xA1, 0x00,
		0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
		0x95, 0x01, 0x75, 0x03, 0x81, 0x03,
		0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02,
		0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
		0xC0, 0xC0,
		0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x02,
		0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
		0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
		0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
		0xC0,
	};
	const HidLayout L = parseHid(descriptor, sizeof(descriptor));
	printf("     | %s\n", L.summary.c_str());
	check(L.ok && L.reportIds && L.fields.size() == 3, "a mouse and a keyboard with report IDs: three fields");
	Buttons b;
	const uint8_t trigger[] = { 1, 0x01, 0, 0, 0, 0, 0 }, pump[] = { 1, 0x02, 0x10, 0, 0, 0, 0 };
	check(decodeHid(L, trigger, sizeof(trigger), b) && b.mouse == 1, "the left button (Sinden's trigger by default)");
	check(decodeHid(L, pump, sizeof(pump), b) && b.mouse == 2, "the right button alone, the motion ignored");
	const uint8_t keys[] = { 2, 0x02, 0, 0x29, 0x1E, 0, 0, 0, 0 };
	check(decodeHid(L, keys, sizeof(keys), b) && b.modifiers == 0x02 && b.keys[0] == 0x29 && b.keys[1] == 0x1E && b.keys[2] == 0
					&& b.mouse == 2,
			"a keyboard report: left shift, Escape and 1; the mouse buttons kept");
	const uint8_t unknown[] = { 3, 1, 2 };
	check(!decodeHid(L, unknown, sizeof(unknown), b), "a report the layout does not have");
	printf("     | %s\n", describeButtons(b).c_str());

	// A joystick: sixteen buttons and two axes, no report IDs.
	const uint8_t joystick[] = {
		0x05, 0x01, 0x09, 0x04, 0xA1, 0x01,
		0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
		0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
		0xC0,
	};
	const HidLayout J = parseHid(joystick, sizeof(joystick));
	Buttons jb;
	const uint8_t jr[] = { 0x05, 0x80, 0x40, 0x40 };
	check(J.ok && !J.reportIds && decodeHid(J, jr, sizeof(jr), jb) && jb.joystick == 0x8005 && jb.mouse == 0,
			"a joystick's buttons 1, 3 and 16");

	const HidLayout bm = bootLayout(2), bk = bootLayout(1);
	Buttons bb;
	const uint8_t mouseReport[] = { 0x05, 0x01, 0xFF }, keyReport[] = { 0x01, 0, 0x04, 0, 0, 0, 0, 0 };
	check(decodeHid(bm, mouseReport, sizeof(mouseReport), bb) && bb.mouse == 0x05, "a boot mouse's report");
	check(decodeHid(bk, keyReport, sizeof(keyReport), bb) && bb.modifiers == 1 && bb.keys[0] == 0x04, "a boot keyboard's report");

	const uint8_t cut[] = { 0x05, 0x01, 0x09 };
	const HidLayout C = parseHid(cut, sizeof(cut));
	check(!C.ok && C.summary.find("past its end") != std::string::npos, "a cut report descriptor said so");
}

// A transfer as libusb lays it out, filled as libusb_fill_iso_transfer and
// libusb_set_iso_packet_lengths would.
std::vector<uint8_t> transfer(bool freebsd, const TransferValues& v, size_t& statusAt, size_t& descAt)
{
	std::vector<uint8_t> t(256, 0);
	auto put = [&](size_t at, const void *p, size_t n) { memcpy(t.data() + at, p, n); };
	put(0, &v.handle, 8);
	size_t endpoint, timeout, status, length, actual, callback, user, buffer, packets, desc;
	if (freebsd)	// FreeBSD's libusb.h: the endpoint an unsigned int
	{
		endpoint = 12, timeout = 20, status = 24, length = 28, actual = 32, callback = 40, user = 48, buffer = 56, packets = 64,
		desc = 68;
		const uint32_t ep = v.endpoint;
		put(endpoint, &ep, 4);
		t[16] = 1;		// type
	}
	else
	{
		endpoint = 9, timeout = 12, status = 16, length = 20, actual = 24, callback = 32, user = 40, buffer = 48, packets = 56,
		desc = 60;
		t[endpoint] = v.endpoint;
		t[10] = 1;
	}
	put(timeout, &v.timeout, 4);
	put(length, &v.length, 4);
	put(callback, &v.callback, 8);
	put(user, &v.userData, 8);
	put(buffer, &v.buffer, 8);
	put(packets, &v.packets, 4);
	for (uint32_t k = 0; k < v.packets; k++)
		put(desc + 12 * k, &v.packetLength, 4);
	(void)actual;
	statusAt = status;
	descAt = desc;
	return t;
}

void testLayout()
{
	TransferValues v;
	v.handle = 0x00000008A0B1C2D0ull;
	v.callback = 0x0000000812345670ull;
	v.userData = 0x5153494E44454E31ull;
	v.buffer = 0x0000000D00C0FFE0ull;	// its upper half 13, as the packet count: not to be taken for it
	v.length = 0x000A5A5A;
	v.timeout = 0x00C0FFEE;
	v.packets = 13;
	v.packetLength = 0xBEEF;
	v.endpoint = 0x8D;
	for (bool freebsd : { false, true })
	{
		size_t status = 0, desc = 0;
		const std::vector<uint8_t> t = transfer(freebsd, v, status, desc);
		const TransferLayout L = findTransferLayout(t.data(), t.size(), v);
		printf("     | %s: %s%s\n", freebsd ? "FreeBSD's" : "libusb's", L.ok ? describeLayout(L).c_str() : "",
				L.ok ? "" : L.why.c_str());
		check(L.ok && L.handle == 0 && (size_t)L.status == status && (size_t)L.packetDescriptors == desc && L.packetStride == 12
						&& L.actual == L.status + 8 && L.packets + 4 == L.packetDescriptors,
				freebsd ? "FreeBSD's libusb layout found" : "libusb's own layout found");
	}
	// A structure the values are not all in is refused, with the reason.
	size_t s = 0, d = 0;
	std::vector<uint8_t> t = transfer(false, v, s, d);
	memset(t.data() + 32, 0, 8);
	const TransferLayout L = findTransferLayout(t.data(), t.size(), v);
	check(!L.ok && L.why == "the callback is not in it", "a layout without the callback is refused, and why");
}

}

int main()
{
	testDescriptors();
	testProbe();
	testAssembler();
	testSerial();
	testHid();
	testLayout();
	printf("%s: %d failed\n", failures == 0 ? "sinden-usb passed" : "sinden-usb FAILED", failures);
	return failures == 0 ? 0 : 1;
}
