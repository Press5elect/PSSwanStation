// PSSwanStation - the Sinden Lightgun's console code (src/ps5/sinden_ps5.cpp) run
// on a PC against a stand-in for libSceUsbd with a simulated gun: a UVC camera
// streaming YUY2 320x240 at 60 pictures a second in isochronous packets, a CDC
// serial port answering the gun's commands, a HID mouse. What the boot log
// would say is printed as it comes ("sinden:" lines).
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This is a rehearsal, not the console: the stand-in behaves as libusb 1.0
// does (its transfer layout, its events, its status codes), and as a
// well-behaved UVC camera does. It checks the stages' order and their
// requests, the hand-over (pictures, buttons, calibration, state), a gun
// pulled out and put back, "find only", a camera that sends no picture and
// one whose endpoint stalls, and that every transfer is given back.
#include "../../src/fe.h"
#include "../../src/sinden.h"
#include "../../src/sinden_usb.h"
#include "gun_descriptor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ------------------------------------------------------------------ the title's parts sinden_ps5.cpp uses
namespace
{
std::mutex logLock;
std::vector<std::string> logLines;
const auto started = std::chrono::steady_clock::now();
}

namespace fe
{
std::string rootDir;

double now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	uint8_t b[4096];
	size_t n;
	out.clear();
	while ((n = fread(b, 1, sizeof(b), f)) > 0)
		out.insert(out.end(), b, b + n);
	fclose(f);
	return true;
}

std::string lowercase(std::string text)
{
	for (char& c : text)
		c = (char)tolower((unsigned char)c);
	return text;
}

std::string trim(const std::string& text)
{
	const size_t a = text.find_first_not_of(" \t\r\n"), b = text.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : text.substr(a, b - a + 1);
}

namespace diag
{
void mark(const char *format, ...)
{
	char b[2048];
	va_list a;
	va_start(a, format);
	vsnprintf(b, sizeof(b), format, a);
	va_end(a);
	std::lock_guard<std::mutex> g(logLock);
	printf("     | [%7.3f] %s\n", now(), b);
	logLines.push_back(b);
}
}

namespace platform
{
bool usbAvailable()
{
	return true;
}
}

// FFmpeg is not in this test: the camera here sends YUY2.
namespace sinden::usb
{
Mjpeg::~Mjpeg() {}
bool Mjpeg::open(std::string& error)
{
	error = "no FFmpeg in this test";
	return false;
}
void Mjpeg::close() {}
bool Mjpeg::decode(const uint8_t *, size_t, uint8_t *, int, int, int, std::string& error)
{
	error = "no FFmpeg in this test";
	return false;
}
}
}

// ------------------------------------------------------------------ the stand-in libSceUsbd
namespace sim
{
// libusb 1.0's own transfer layout.
struct IsoDesc
{
	uint32_t length, actual;
	int32_t status;
};
struct Transfer
{
	void *handle;
	uint8_t flags, endpoint, type;
	uint32_t timeout;
	int32_t status, length, actual;
	void (*callback)(void *);
	void *user;
	uint8_t *buffer;
	int32_t packets;
	IsoDesc iso[1];
};
static_assert(offsetof(Transfer, status) == 16 && offsetof(Transfer, iso) == 60, "libusb's layout");

enum class Camera { Streams, Silent, Stalls };

constexpr int Width = 320, Height = 240;
constexpr uint32_t PictureBytes = Width * Height * 2;
constexpr uint32_t Payload = 3000;		// the camera's dwMaxPayloadTransferSize
constexpr double MicroframesPerPicture = 8000.0 / 60.0;

struct Device
{
	std::mutex m;
	bool plugged = true;
	Camera camera = Camera::Streams;
	std::vector<uint8_t> config;
	int alt = 0;
	std::vector<std::pair<int, int>> alts;		// every alternate setting asked for: interface, setting
	uint8_t probe[34] = {};
	bool committed = false;
	uint32_t baud = 9600;
	bool dtr = false;
	int protocol = -1;
	std::vector<uint8_t> tx;
	std::vector<int> commands;
	std::vector<std::string> requests;
	int opens = 0, sonyOpens = 0, allocated = 0, freed = 0, submitted = 0, videoSubmitted = 0;
	std::vector<Transfer *> pending, cancelled;
	bool videoRunning = false;
	double videoBase = 0;
	uint64_t microframe = 0;
	double nextReport = 0;
	unsigned reportNo = 0;
} dev;

int sonyDevice, gunDevice, handleToken;

void put16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

int Init()
{
	return 0;
}

long GetDeviceList(void ***out)
{
	std::lock_guard<std::mutex> g(dev.m);
	void **list = (void **)calloc(3, sizeof(void *));
	long n = 0;
	list[n++] = &sonyDevice;
	if (dev.plugged)
		list[n++] = &gunDevice;
	*out = list;
	return n;
}

void FreeDeviceList(void **list, int)
{
	free(list);
}

int GetDeviceDescriptor(void *d, void *out)
{
	uint8_t dd[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0, 0, 0, 0, 0x00, 0x01, 1, 2, 3, 1 };
	if (d == &sonyDevice)
	{
		put16(dd + 8, 0x054C);
		put16(dd + 10, 0x0CE6);
	}
	else
	{
		dd[4] = 0xEF;
		dd[5] = 0x02;
		dd[6] = 0x01;
		put16(dd + 8, 0x16C0);
		put16(dd + 10, 0x0F38);
	}
	memcpy(out, dd, sizeof(dd));
	return 0;
}

uint8_t GetBusNumber(void *)
{
	return 1;
}

uint8_t GetDeviceAddress(void *d)
{
	return d == &sonyDevice ? 2 : 5;
}

int GetDeviceSpeed(void *)
{
	return 3;		// high speed
}

int Open(void *d, void **handle)
{
	std::lock_guard<std::mutex> g(dev.m);
	if (d == &sonyDevice)
		dev.sonyOpens++;
	if (d == &gunDevice && !dev.plugged)
		return -4;
	dev.opens++;
	*handle = &handleToken;
	return 0;
}

void Close(void *) {}

int KernelDriverActive(void *, int iface)
{
	return iface == 4 ? 1 : 0;		// the console's HID driver has the mouse
}

int DetachKernelDriver(void *, int)
{
	return 0;
}

int ClaimInterface(void *, int)
{
	return 0;
}

int ReleaseInterface(void *, int)
{
	return 0;
}

int SetInterfaceAltSetting(void *, int iface, int alt)
{
	std::lock_guard<std::mutex> g(dev.m);
	dev.alts.push_back({ iface, alt });
	if (iface == 1)
	{
		dev.alt = alt;
		dev.videoRunning = false;
	}
	return 0;
}

int stringDescriptor(uint8_t index, unsigned char *data, uint16_t length)
{
	uint8_t b[64] = {};
	int n;
	if (index == 0)
	{
		b[0] = 4;
		b[1] = 3;
		put16(b + 2, 0x0409);
		n = 4;
	}
	else
	{
		const char *text = index == 1 ? "Sinden" : index == 2 ? "SindenLightgun" : "0001";
		const size_t chars = strlen(text);
		n = (int)(2 + 2 * chars);
		b[0] = (uint8_t)n;
		b[1] = 3;
		for (size_t i = 0; i < chars; i++)
			b[2 + 2 * i] = (uint8_t)text[i];
	}
	n = std::min<int>(n, length);
	memcpy(data, b, (size_t)n);
	return n;
}

// The 50-byte report descriptor of a three-button mouse.
const uint8_t MouseDescriptor[50] = { 0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29,
	0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30,
	0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0 };

int ControlTransfer(void *, uint8_t type, uint8_t request, uint16_t value, uint16_t index, unsigned char *data, uint16_t length,
		unsigned int)
{
	std::lock_guard<std::mutex> g(dev.m);
	char r[48];
	snprintf(r, sizeof(r), "%02x %02x %04x %u", type, request, value, index);
	dev.requests.push_back(r);
	if (!dev.plugged)
		return -4;
	if (type == 0x80 && request == 0x06 && (value >> 8) == 2)
	{
		const int n = std::min<int>(length, (int)dev.config.size());
		memcpy(data, dev.config.data(), (size_t)n);
		return n;
	}
	if (type == 0x80 && request == 0x06 && (value >> 8) == 3)
		return stringDescriptor((uint8_t)value, data, length);
	if (type == 0x81 && request == 0x06 && value == 0x2200 && index == 4)
	{
		const int n = std::min<int>(length, (int)sizeof(MouseDescriptor));
		memcpy(data, MouseDescriptor, (size_t)n);
		return n;
	}
	if (type == 0xA1 && request == 0x21 && index == 2 && length >= 7)
	{
		const uint8_t lc[7] = { (uint8_t)dev.baud, (uint8_t)(dev.baud >> 8), (uint8_t)(dev.baud >> 16), 0, 0, 0, 8 };
		memcpy(data, lc, 7);
		return 7;
	}
	if (type == 0x21 && request == 0x20 && index == 2 && length >= 7)
	{
		dev.baud = (uint32_t)(data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32_t)data[3] << 24));
		return 7;
	}
	if (type == 0x21 && request == 0x22 && index == 2)
	{
		dev.dtr = (value & 1) != 0;
		return 0;
	}
	if (type == 0x21 && request == 0x0B && index == 4)
	{
		dev.protocol = value;
		return 0;
	}
	if (type == 0xA1 && request == 0x81 && value == 0x0100 && index == 1)
	{
		const int n = std::min<int>(length, (int)sizeof(dev.probe));
		memcpy(data, dev.probe, (size_t)n);
		return n;
	}
	if (type == 0x21 && request == 0x01 && value == 0x0100 && index == 1)
	{
		// The camera takes the format, size and interval asked for and says
		// what its pictures and payloads need.
		memcpy(dev.probe, data, std::min<size_t>(length, sizeof(dev.probe)));
		const bool yuy2 = dev.probe[2] == 2 && dev.probe[3] == 2;
		const uint32_t frame = yuy2 ? PictureBytes : 0;
		for (int i = 0; i < 4; i++)
		{
			dev.probe[18 + i] = (uint8_t)(frame >> (8 * i));
			dev.probe[22 + i] = (uint8_t)(Payload >> (8 * i));
		}
		return length;
	}
	if (type == 0x21 && request == 0x01 && value == 0x0200 && index == 1)
	{
		dev.committed = true;
		return length;
	}
	return -9;		// a stall: not a request this camera has
}

void *AllocTransfer(int packets)
{
	std::lock_guard<std::mutex> g(dev.m);
	dev.allocated++;
	return calloc(1, sizeof(Transfer) + (size_t)std::max(packets - 1, 0) * sizeof(IsoDesc));
}

void FreeTransfer(void *t)
{
	std::lock_guard<std::mutex> g(dev.m);
	dev.freed++;
	free(t);
}

void fill(void *t, void *handle, unsigned char endpoint, unsigned char *buffer, int length, uint8_t type, void (*cb)(void *),
		void *user, unsigned int timeout)
{
	Transfer *x = (Transfer *)t;
	x->handle = handle;
	x->endpoint = endpoint;
	x->type = type;
	x->buffer = buffer;
	x->length = length;
	x->callback = cb;
	x->user = user;
	x->timeout = timeout;
}

void FillBulkTransfer(void *t, void *h, unsigned char ep, unsigned char *b, int len, void (*cb)(void *), void *user, unsigned int to)
{
	fill(t, h, ep, b, len, 2, cb, user, to);
}

void FillInterruptTransfer(void *t, void *h, unsigned char ep, unsigned char *b, int len, void (*cb)(void *), void *user,
		unsigned int to)
{
	fill(t, h, ep, b, len, 3, cb, user, to);
}

void FillIsoTransfer(void *t, void *h, unsigned char ep, unsigned char *b, int len, int packets, void (*cb)(void *), void *user,
		unsigned int to)
{
	fill(t, h, ep, b, len, 1, cb, user, to);
	((Transfer *)t)->packets = packets;
}

void SetIsoPacketLengths(void *t, unsigned int length)
{
	Transfer *x = (Transfer *)t;
	for (int i = 0; i < x->packets; i++)
		x->iso[i].length = length;
}

unsigned char *GetIsoPacketBuffer(void *t, unsigned int packet)
{
	Transfer *x = (Transfer *)t;
	size_t at = 0;
	for (unsigned i = 0; i < packet && (int)i < x->packets; i++)
		at += x->iso[i].length;
	return x->buffer + at;
}

int SubmitTransfer(void *t)
{
	std::lock_guard<std::mutex> g(dev.m);
	if (!dev.plugged)
		return -4;
	Transfer *x = (Transfer *)t;
	dev.submitted++;
	if (x->endpoint == 0x81)
		dev.videoSubmitted++;
	x->status = 0;
	x->actual = 0;
	dev.pending.push_back(x);
	return 0;
}

int CancelTransfer(void *t)
{
	std::lock_guard<std::mutex> g(dev.m);
	dev.cancelled.push_back((Transfer *)t);
	return 0;
}

// The picture that one micro-frame's packet belongs to, and the packet: a
// 12-byte header (the picture's frame bit, end of frame on its last), then up
// to the payload's size of the picture, Y = (x + y + picture) & 255.
uint64_t pictureStart(uint64_t k)
{
	return (uint64_t)std::ceil(k * MicroframesPerPicture);
}

uint32_t videoPacket(uint64_t m, uint8_t *out, uint32_t room)
{
	uint64_t k = (uint64_t)(m / MicroframesPerPicture);
	while (k > 0 && pictureStart(k) > m)
		k--;
	while (pictureStart(k + 1) <= m)
		k++;
	const uint64_t sent = (m - pictureStart(k)) * (Payload - 12);
	out[0] = 12;
	out[1] = (uint8_t)(0x80 | 0x0C | (k & 1));
	memset(out + 2, 0, 10);
	if (dev.camera == Camera::Silent || sent >= PictureBytes)
		return 12;		// a header and no data, between pictures
	const uint32_t n = (uint32_t)std::min<uint64_t>(std::min<uint32_t>(room, Payload) - 12, PictureBytes - sent);
	if (sent + n == PictureBytes)
		out[1] |= 0x02;
	for (uint32_t i = 0; i < n; i++)
	{
		const uint64_t o = sent + i;
		const uint64_t pixel = o / 2;
		out[12 + i] = (o & 1) ? 128 : (uint8_t)((pixel % Width) + (pixel / Width) + k);
	}
	return 12 + n;
}

// libusb's event handling: the transfers whose time has come are completed
// and called back, up to the timeout.
int HandleEventsTimeout(struct timeval *tv)
{
	const double until = fe::now() + tv->tv_sec + tv->tv_usec / 1e6;
	std::vector<Transfer *> done;
	for (;;)
	{
		const double t = fe::now();
		{
			std::lock_guard<std::mutex> g(dev.m);
			bool videoSeen = false;
			for (size_t i = 0; i < dev.pending.size();)
			{
				Transfer *x = dev.pending[i];
				bool complete = false;
				if (std::find(dev.cancelled.begin(), dev.cancelled.end(), x) != dev.cancelled.end())
				{
					dev.cancelled.erase(std::remove(dev.cancelled.begin(), dev.cancelled.end(), x), dev.cancelled.end());
					x->status = 3;
					complete = true;
				}
				else if (!dev.plugged)
				{
					x->status = 5;
					complete = true;
				}
				else if (x->endpoint == 0x81 && !videoSeen)
				{
					videoSeen = true;		// in order: the first one waiting first
					if (dev.camera == Camera::Stalls || dev.alt == 0)
					{
						x->status = dev.alt == 0 ? 1 : 4;
						complete = true;
					}
					else
					{
						if (!dev.videoRunning)
						{
							dev.videoRunning = true;
							dev.videoBase = t;
							dev.microframe = 0;
						}
						if (t >= dev.videoBase + (double)(dev.microframe + (uint64_t)x->packets) / 8000.0)
						{
							for (int k = 0; k < x->packets; k++)
							{
								uint8_t *p = x->buffer + (size_t)k * x->iso[0].length;
								x->iso[k].actual = videoPacket(dev.microframe++, p, x->iso[k].length);
								x->iso[k].status = 0;
							}
							x->status = 0;
							complete = true;
						}
					}
				}
				else if (x->endpoint == 0x05)
				{
					for (int k = 0; k + 7 <= x->length; k += 7)
						if (x->buffer[k] == 0xAA && x->buffer[k + 6] == 0xBB)
						{
							const int c = x->buffer[k + 1];
							dev.commands.push_back(c);
							if (c == 101)
								dev.tx.insert(dev.tx.end(), { 1, 9 });
							else if (c == 102)
							{
								const char *name = "SindenCamC     ";
								dev.tx.insert(dev.tx.end(), name, name + 15);
							}
							else if (c == 104)
								dev.tx.insert(dev.tx.end(), { 0x27, 0x8C });
							else if (c == 105)
								dev.tx.insert(dev.tx.end(), { 0x26, 0x94 });
						}
					x->actual = x->length;
					x->status = 0;
					complete = true;
				}
				else if (x->endpoint == 0x86 && !dev.tx.empty())
				{
					const size_t n = std::min<size_t>((size_t)x->length, dev.tx.size());
					memcpy(x->buffer, dev.tx.data(), n);
					dev.tx.erase(dev.tx.begin(), dev.tx.begin() + (long)n);
					x->actual = (int32_t)n;
					x->status = 0;
					complete = true;
				}
				else if (x->endpoint == 0x87 && t >= dev.nextReport)
				{
					// The trigger (left), nothing, the pump action (right), nothing ...
					static const uint8_t pattern[4] = { 1, 0, 2, 0 };
					x->buffer[0] = pattern[dev.reportNo++ % 4];
					x->buffer[1] = x->buffer[2] = 0;
					x->actual = 3;
					x->status = 0;
					dev.nextReport = t + 0.05;
					complete = true;
				}
				if (complete)
				{
					done.push_back(x);
					dev.pending.erase(dev.pending.begin() + (long)i);
				}
				else
					i++;
			}
		}
		if (!done.empty() || t >= until)
			break;
		usleep(500);
	}
	for (Transfer *x : done)
		x->callback(x);
	return 0;
}

struct Symbol
{
	const char *name;
	void *address;
};
// Every call sinden_ps5.cpp looks up but six, so that "not found" is said too.
const Symbol symbols[] = {
	{ "sceUsbdInit", (void *)&Init },
	{ "sceUsbdGetDeviceList", (void *)&GetDeviceList },
	{ "sceUsbdFreeDeviceList", (void *)&FreeDeviceList },
	{ "sceUsbdGetDeviceDescriptor", (void *)&GetDeviceDescriptor },
	{ "sceUsbdGetBusNumber", (void *)&GetBusNumber },
	{ "sceUsbdGetDeviceAddress", (void *)&GetDeviceAddress },
	{ "sceUsbdGetDeviceSpeed", (void *)&GetDeviceSpeed },
	{ "sceUsbdOpen", (void *)&Open },
	{ "sceUsbdClose", (void *)&Close },
	{ "sceUsbdKernelDriverActive", (void *)&KernelDriverActive },
	{ "sceUsbdDetachKernelDriver", (void *)&DetachKernelDriver },
	{ "sceUsbdClaimInterface", (void *)&ClaimInterface },
	{ "sceUsbdReleaseInterface", (void *)&ReleaseInterface },
	{ "sceUsbdSetInterfaceAltSetting", (void *)&SetInterfaceAltSetting },
	{ "sceUsbdControlTransfer", (void *)&ControlTransfer },
	{ "sceUsbdAllocTransfer", (void *)&AllocTransfer },
	{ "sceUsbdFreeTransfer", (void *)&FreeTransfer },
	{ "sceUsbdSubmitTransfer", (void *)&SubmitTransfer },
	{ "sceUsbdCancelTransfer", (void *)&CancelTransfer },
	{ "sceUsbdFillBulkTransfer", (void *)&FillBulkTransfer },
	{ "sceUsbdFillInterruptTransfer", (void *)&FillInterruptTransfer },
	{ "sceUsbdFillIsoTransfer", (void *)&FillIsoTransfer },
	{ "sceUsbdSetIsoPacketLengths", (void *)&SetIsoPacketLengths },
	{ "sceUsbdGetIsoPacketBuffer", (void *)&GetIsoPacketBuffer },
	{ "sceUsbdHandleEventsTimeout", (void *)&HandleEventsTimeout },
};
}

extern "C" int sceKernelLoadStartModule(const char *path, size_t, const void *, uint32_t, void *, int *res)
{
	*res = 0;
	return strcmp(path, "/system/common/lib/libSceUsbd.sprx") == 0 ? 0x4242 : -1;
}

extern "C" int sceKernelDlsym(int handle, const char *symbol, void **addrp)
{
	if (handle != 0x4242)
		return -1;
	for (const sim::Symbol& s : sim::symbols)
		if (strcmp(s.name, symbol) == 0)
		{
			*addrp = s.address;
			return 0;
		}
	return -1;
}

extern "C" int sinden_test_stat(const char *path, struct stat *st)
{
	if (strcmp(path, "/system/common/lib/libSceUsbd.sprx") == 0)
	{
		memset(st, 0, sizeof(*st));
		return 0;
	}
	return stat(path, st);
}

// ------------------------------------------------------------------ the checks
namespace
{
using namespace fe::sinden;

int failures = 0;

void check(bool ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	failures += ok ? 0 : 1;
}

bool waitFor(double seconds, const std::function<bool()>& done)
{
	const double until = fe::now() + seconds;
	while (fe::now() < until)
	{
		if (done())
			return true;
		usleep(20 * 1000);
	}
	return done();
}

bool logged(const char *text)
{
	std::lock_guard<std::mutex> g(logLock);
	for (const std::string& l : logLines)
		if (l.find(text) != std::string::npos)
			return true;
	return false;
}

template <typename F> auto device(F&& f)
{
	std::lock_guard<std::mutex> g(sim::dev.m);
	return f(sim::dev);
}

// Every picture whole and as the camera made it: brightness minus x and y the
// same number everywhere.
bool pictureRight(const Frame& f)
{
	if (f.width != sim::Width || f.height != sim::Height || f.luma == nullptr)
		return false;
	const uint8_t k = (uint8_t)(f.luma[0]);
	for (int y = 0; y < f.height; y++)
		for (int x = 0; x < f.width; x++)
			if ((uint8_t)(f.luma[(size_t)y * f.stride + x] - x - y) != k)
				return false;
	return true;
}

void everyStage()
{
	printf("---- every stage, libSceUsbd\n");
	Options o;
	o.route = 1;
	start(o);
	check(waitFor(10, [] { return state() == State::Streaming; }), "the camera streams within 10 s");
	const Status s = status();
	check(s.serial && s.firmware == "1.9" && s.camera == "SindenCamC", "the serial port's answers: firmware 1.9, camera SindenCamC");
	check(s.format == "YUY2" && s.width == 320 && s.height == 240 && s.route == "libSceUsbd", "YUY2 320x240, through libSceUsbd");
	const Calibration c = calibration();
	check(c.known && c.rawX == 10124 && std::fabs(c.x - 1.24) < 1e-9 && c.rawY == 9876 && std::fabs(c.y + 1.24) < 1e-9,
			"the calibration: 1.24, -1.24");
	check(device([](sim::Device& d) { return d.baud == 115200 && d.dtr && d.protocol == 1; }),
			"115200 baud, DTR on, the mouse in the report protocol");
	check(device([](sim::Device& d) { return d.committed && d.alt == 6; }),
			"the probe committed, alternate setting 6 (3 x 1024 bytes for the 3000-byte payload)");
	check(device([](sim::Device& d) {
		return d.commands == std::vector<int>({ 101, 102, 104, 105 });
	}), "the gun asked 101, 102, 104, 105 and nothing else");

	// Two seconds of pictures, as the frame's thread takes them.
	unsigned taken = 0, right = 0;
	uint64_t firstNumber = 0, lastNumber = 0;
	double firstTime = 0, lastTime = 0;
	const double until = fe::now() + 2;
	while (fe::now() < until)
	{
		Frame f;
		if (latestFrame(f))
		{
			taken++;
			right += pictureRight(f) ? 1 : 0;
			if (firstNumber == 0)
			{
				firstNumber = f.number;
				firstTime = f.time;
			}
			lastNumber = f.number;
			lastTime = f.time;
		}
		usleep(4000);
	}
	const double rate = lastTime > firstTime ? (lastNumber - firstNumber) / (lastTime - firstTime) : 0;
	printf("     | %u pictures taken, %u right, %.1f a second made\n", taken, right, rate);
	check(taken > 90 && right == taken, "about 120 pictures in 2 s, every one whole and right");
	check(rate > 55 && rate < 65, "60 a second");
	Frame none;
	check(!latestFrame(none) || none.number > lastNumber, "no picture twice");
	const Buttons b = buttons();
	check(b.reports > 20, "the mouse's reports come");
	check(logged("libSceUsbd: the transfer's layout: handle +0, endpoint +9, timeout +12, status +16, length +20"),
			"libusb's transfer layout found in the library");
	check(logged("libSceUsbd: not found: sceUsbdGetPortNumbers"), "what the library does not have is said");
	check(logged("camera: the first picture"), "the first picture said");
	check(device([](sim::Device& d) { return d.sonyOpens == 0; }), "Sony's device never opened");
	check(waitFor(5, [] { return logged("pictures a second over"); }), "the rate said 5 s after the first picture");
	const Status later = status();
	check(later.framesPerSecond > 55 && later.framesPerSecond < 65, "and kept in the state");

	// Pulled out, and put back.
	device([](sim::Device& d) { d.plugged = false; return 0; });
	check(waitFor(5, [] { return state() == State::Absent; }), "pulled out: no gun, within 5 s");
	check(device([](sim::Device& d) { return !d.alts.empty() && d.alts.back() == std::make_pair(1, 0); }),
			"its bandwidth given back (alternate setting 0)");
	device([](sim::Device& d) { d.plugged = true; d.commands.clear(); return 0; });
	check(waitFor(10, [] { return state() == State::Streaming; }), "put back: streaming again within 10 s");
	stop();
	check(state() == State::Off, "stopped: off");
	check(device([](sim::Device& d) { return d.pending.empty() && d.allocated == d.freed; }),
			"every transfer back and freed");
	check(logged("the gun's thread stopped"), "the thread said it stopped");
}

void findOnly()
{
	printf("---- find only\n");
	const int submittedBefore = device([](sim::Device& d) { d.requests.clear(); return d.submitted; });
	Options o;
	o.route = 1;
	o.findOnly = true;
	start(o);
	check(waitFor(3, [] { return state() == State::Found; }), "found");
	usleep(2500 * 1000);
	check(device([&](sim::Device& d) {
		for (const std::string& r : d.requests)
			if (r.rfind("80 06", 0) != 0)
				return false;
		return d.submitted == submittedBefore;
	}), "nothing asked of the gun but its descriptors, no transfer");
	stop();
}

void silentCamera()
{
	printf("---- a camera that sends no picture\n");
	device([](sim::Device& d) { d.camera = sim::Camera::Silent; return 0; });
	Options o;
	o.route = 1;
	start(o);
	check(waitFor(12, [] { return state() == State::Failed && status().why.find("no picture") != std::string::npos; }),
			"failed: no picture, and why");
	check(device([](sim::Device& d) { return d.alt == 0; }), "its bandwidth given back");
	const uint64_t before = buttons().reports;
	usleep(600 * 1000);
	check(buttons().reports > before, "the buttons go on");
	stop();
	check(device([](sim::Device& d) { return d.pending.empty() && d.allocated == d.freed; }), "every transfer back and freed");
}

void stallingCamera()
{
	printf("---- a camera whose endpoint stalls\n");
	device([](sim::Device& d) { d.camera = sim::Camera::Stalls; d.videoSubmitted = 0; return 0; });
	Options o;
	o.route = 1;
	start(o);
	check(waitFor(8, [] { return state() == State::Failed && status().why.find("in a row failed") != std::string::npos; }),
			"failed: 20 transfers in a row");
	usleep(500 * 1000);
	const int sent = device([](sim::Device& d) { return d.videoSubmitted; });
	printf("     | %d video transfers sent\n", sent);
	check(sent < 40, "and not sent again and again");
	stop();
}
}

int main()
{
	char dir[] = "/tmp/psswan-sinden-console.XXXXXX";
	if (mkdtemp(dir) == nullptr)
		return 2;
	fe::rootDir = std::string(dir) + "/";
	mkdir((fe::rootDir + "data").c_str(), 0777);
	sim::dev.config = gunConfiguration(sizeof(sim::MouseDescriptor));
	check(state() == State::Off && !claims(0x16C0, 0x0F38), "off before start: the gun not claimed");
	everyStage();
	findOnly();
	silentCamera();
	stallingCamera();
	check(!claims(0x16C0, 0x0F38), "after stop: the gun not claimed");
	std::vector<uint8_t> marker;
	check(fe::readFile(fe::rootDir + "data/sinden-usbd-marker.txt", marker) && std::string(marker.begin(), marker.end()) == "done\n",
			"libSceUsbd's start marker says it came back");
	std::string cleanup = "rm -rf '" + std::string(dir) + "'";
	if (system(cleanup.c_str()) != 0)
		printf("     | %s not removed\n", dir);
	printf("%s: %d failed\n", failures == 0 ? "sinden-usb console rehearsal passed" : "sinden-usb console rehearsal FAILED", failures);
	return failures == 0 ? 0 : 1;
}
