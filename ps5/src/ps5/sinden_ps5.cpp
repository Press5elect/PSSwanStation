/*
	PSSwanStation - the Sinden Lightgun, read by the console itself over USB.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	Nothing of this has run on a console with a gun yet. Every step goes to the
	boot log as a "sinden:" line, so that one test run says what the console
	and the gun did (ps5/docs/dev/sinden-lightgun.md, "The console side", says
	what each line means). A thread of its own, in stages, each stopping
	cleanly when it fails:

	  1. Find: the USB devices listed (the kernel's /dev/ugenB.A nodes with
	     their device-info ioctl; libSceUsbd's device list), the gun's device
	     and configuration descriptors decoded line by line, with their bytes.
	     Descriptors only: the kernel's own copies, or GET_DESCRIPTOR on
	     endpoint 0.
	  2. Serial: the CDC ACM port set to 115200 8N1 with DTR on; the gun asked
	     its firmware (101), its camera's name (102) and its calibration (104,
	     105), as MrLightgun's gun_serial.py asks them.
	  3. Buttons: each HID interface's report descriptor, then its interrupt
	     endpoint read, the first reports and each change logged raw.
	  4. Camera: UVC's probe and commit for 320x240 (uncompressed first, else
	     MJPEG); the alternate setting whose isochronous endpoint carries the
	     payload the camera asks for; the stream read with isochronous (or bulk)
	     transfers, put together into pictures, and its rate measured.
	  5. Hand-over: the newest picture's brightness, the buttons, the
	     calibration and the state, for the frame's thread (sinden.h).

	The two ways, in the order tried (Options::route): libSceUsbd, the
	console's libusb-like library, loaded at run time by name; then the
	kernel's ugen nodes with FreeBSD's USB_FS. When the camera sends no
	picture one way, the other way is tried.

	What is never done: a device is never opened that the kernel or the
	library has not said what it is first, and none is read from but the
	gun (PS5SX2's lesson, as usbpad_ps5.cpp keeps it: reading unknown
	devices' endpoints can take a DualSense on a cable from the system, or
	stall the USB drive a game is read from). Sony's devices, hubs and
	storage are not opened beyond the kernel's device-info ioctl. A 0x16c0
	device that is not one of the gun's products is described and left (0x16c0
	is shared by many hobby boards). Only the gun's own interfaces are taken
	from their drivers. Nothing is written into the gun (command 103 is never
	sent), and the line coding is never 1200 baud (that restarts an
	ATmega32u4 into its boot loader). Every wait has a timeout; nothing spins.
*/
#include "fe.h"
#include "sinden.h"
#include "sinden_usb.h"
#include "tuning.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" {
int sceKernelLoadStartModule(const char *path, size_t args, const void *argp, uint32_t flags, void *opt, int *res);
int sceKernelDlsym(int handle, const char *symbol, void **addrp);
}

namespace fe::sinden
{
namespace
{
using usb::hex;

constexpr uint16_t GunVendor = 0x16C0;
constexpr uint16_t SonyVendor = 0x054C;
// The products MrLightgun's MiSTer input maps name (Config/inputs/*.map).
constexpr uint16_t GunProducts[] = { 0x0F01, 0x0F02, 0x0F38, 0x0F39 };
constexpr int MostWidth = 640, MostHeight = 480;
constexpr double LookEvery = 2.0;				// seconds between looks for a gun
constexpr double FirstPictureWithin = 5.0;		// or the camera did not stream this way
constexpr double SerialSettle = 2.0;			// gun_serial.py waits this long after opening the port
constexpr double SerialAnswerWithin = 1.5;		// and this long for an answer
constexpr int VideoTransfers = 8;

bool gunProduct(uint16_t vendor, uint16_t product)
{
	return vendor == GunVendor && std::find(std::begin(GunProducts), std::end(GunProducts), product) != std::end(GunProducts);
}

// ------------------------------------------------------------------ what is said
void say(const char *f, ...) __attribute__((format(printf, 1, 2)));
void say(const char *f, ...)
{
	char b[1400];
	va_list a;
	va_start(a, f);
	vsnprintf(b, sizeof(b), f, a);
	va_end(a);
	diag::mark("sinden: %s", b);
}

std::string format(const char *f, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char *f, ...)
{
	char b[600];
	va_list a;
	va_start(a, f);
	vsnprintf(b, sizeof(b), f, a);
	va_end(a);
	return b;
}

// Bytes in the boot log, 64 a line.
void sayBytes(const std::string& what, const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i += 64)
		say("%s, bytes %zu to %zu: %s", what.c_str(), i, std::min(n, i + 64) - 1, hex(p + i, std::min<size_t>(64, n - i)).c_str());
}

// What has been said once (a device seen, a failure): not said again.
std::vector<std::string> saidOnce;

bool firstTime(const std::string& key)
{
	if (std::find(saidOnce.begin(), saidOnce.end(), key) != saidOnce.end())
		return false;
	if (saidOnce.size() < 512)
		saidOnce.push_back(key);
	return true;
}

// ------------------------------------------------------------------ what is handed over
std::mutex lock;
Status shared;						// under lock
Buttons sharedButtons;				// under lock
Calibration sharedCalibration;		// under lock
Options options;
std::atomic<bool> running{ false }, stopping{ false }, finished{ true };

// The pictures: three buffers, one the camera's thread writes, one the
// frame's thread reads, one between them (the newest whole one, with Fresh
// set until it is taken). Set aside once, at the largest size the camera may
// be given; none per picture.
struct Picture
{
	std::vector<uint8_t> luma;
	int width = 0, height = 0, stride = 0;
	double time = 0;
	uint64_t number = 0;
};
Picture pictures[3];
constexpr unsigned Fresh = 4;
std::atomic<unsigned> middle{ 1 };
unsigned back = 2;			// the camera's thread's
unsigned front = 0;			// the frame's thread's
uint64_t picturesMade = 0;	// the camera's thread's

void setState(State s, const std::string& why)
{
	std::lock_guard<std::mutex> g(lock);
	shared.state = s;
	shared.why = why;
}

// ------------------------------------------------------------------ libSceUsbd
// libusb 1.0's calls under Sony's names, as on the PS4 (no context argument).
// None is declared for the console: each is looked up by name, and the boot
// log says which were found.
using TransferCallback = void (*)(void *transfer);
struct Usbd
{
	bool tried = false, ok = false;
	int (*Init)() = nullptr;
	long (*GetDeviceList)(void ***list) = nullptr;
	void (*FreeDeviceList)(void **list, int unref) = nullptr;
	int (*GetDeviceDescriptor)(void *dev, void *desc) = nullptr;
	uint8_t (*GetBusNumber)(void *dev) = nullptr;
	uint8_t (*GetDeviceAddress)(void *dev) = nullptr;
	int (*GetDeviceSpeed)(void *dev) = nullptr;
	int (*GetPortNumbers)(void *dev, uint8_t *ports, int length) = nullptr;
	int (*GetMaxIsoPacketSize)(void *dev, unsigned char endpoint) = nullptr;
	int (*GetConfigDescriptor)(void *dev, uint8_t index, void **config) = nullptr;
	void (*FreeConfigDescriptor)(void *config) = nullptr;
	int (*Open)(void *dev, void **handle) = nullptr;
	void (*Close)(void *handle) = nullptr;
	int (*KernelDriverActive)(void *handle, int iface) = nullptr;
	int (*DetachKernelDriver)(void *handle, int iface) = nullptr;
	int (*ClaimInterface)(void *handle, int iface) = nullptr;
	int (*ReleaseInterface)(void *handle, int iface) = nullptr;
	int (*SetInterfaceAltSetting)(void *handle, int iface, int alt) = nullptr;
	int (*ControlTransfer)(void *handle, uint8_t type, uint8_t request, uint16_t value, uint16_t index, unsigned char *data,
			uint16_t length, unsigned int timeout) = nullptr;
	int (*BulkTransfer)(void *handle, unsigned char endpoint, unsigned char *data, int length, int *done,
			unsigned int timeout) = nullptr;
	int (*InterruptTransfer)(void *handle, unsigned char endpoint, unsigned char *data, int length, int *done,
			unsigned int timeout) = nullptr;
	void *(*AllocTransfer)(int packets) = nullptr;
	void (*FreeTransfer)(void *transfer) = nullptr;
	int (*SubmitTransfer)(void *transfer) = nullptr;
	int (*CancelTransfer)(void *transfer) = nullptr;
	void (*FillBulkTransfer)(void *transfer, void *handle, unsigned char endpoint, unsigned char *buffer, int length,
			TransferCallback callback, void *user, unsigned int timeout) = nullptr;
	void (*FillInterruptTransfer)(void *transfer, void *handle, unsigned char endpoint, unsigned char *buffer, int length,
			TransferCallback callback, void *user, unsigned int timeout) = nullptr;
	void (*FillIsoTransfer)(void *transfer, void *handle, unsigned char endpoint, unsigned char *buffer, int length,
			int packets, TransferCallback callback, void *user, unsigned int timeout) = nullptr;
	void (*SetIsoPacketLengths)(void *transfer, unsigned int length) = nullptr;
	unsigned char *(*GetIsoPacketBuffer)(void *transfer, unsigned int packet) = nullptr;
	int (*HandleEventsTimeout)(struct timeval *tv) = nullptr;
	bool layoutTried = false;
	usb::TransferLayout layout;
} usbd;

// libusb's error codes, and Sony's own (0x8xxxxxxx) as they are.
std::string usbdError(int rc)
{
	static const char *names[] = { "", "I/O error", "invalid parameter", "access denied", "no device", "not found", "busy",
		"timeout", "overflow", "pipe error (stall)", "interrupted", "no memory", "not supported" };
	if (rc < 0 && rc >= -12)
		return format("%d (%s)", rc, names[-rc]);
	return format("%d (%#x)", rc, (unsigned)rc);
}

const char *usbdStatus(int s)
{
	static const char *names[] = { "completed", "error", "timed out", "cancelled", "stalled", "no device", "overflow" };
	return s >= 0 && s <= 6 ? names[s] : "?";
}

const char *ugenStatus(int s)
{
	// USB_ERR_* of dev/usb/usbdi.h.
	static const char *names[] = { "done", "pending requests", "not started", "invalid", "no memory", "cancelled",
		"bad address", "bad buffer size", "bad flag", "no callback", "in use", "no address", "no pipe", "no frames",
		"no packet size", "set address failed", "no power", "too deep", "I/O error", "not configured", "timeout",
		"short transfer", "stalled", "interrupted", "DMA load failed", "bad context", "no root hub", "no interrupt thread",
		"not locked" };
	return s >= 0 && s < (int)(sizeof(names) / sizeof(names[0])) ? names[s] : "?";
}

template <typename F> void symbol(int module, const char *name, F& fn, std::string& found, std::string& missing)
{
	void *address = nullptr;
	if (sceKernelDlsym(module, name, &address) == 0 && address != nullptr)
	{
		fn = reinterpret_cast<F>(address);
		found += found.empty() ? name : std::string(", ") + name;
	}
	else
		missing += missing.empty() ? name : std::string(", ") + name;
}

bool usbdLoad()
{
	if (usbd.tried)
		return usbd.ok;
	usbd.tried = true;
	// A start that never came back from the library is not repeated (as
	// usbpad_ps5.cpp does, with a marker of its own).
	const std::string marker = rootDir + "data/sinden-usbd-marker.txt";
	if (FILE *f = fopen(marker.c_str(), "rb"))
	{
		char line[16] = {};
		const size_t n = fread(line, 1, sizeof(line) - 1, f);
		fclose(f);
		line[n] = 0;
		if (strncmp(line, "started", 7) == 0)
		{
			say("libSceUsbd: not tried: the last start never came back from it (delete data/sinden-usbd-marker.txt to try again)");
			return false;
		}
	}
	int module = -1;
	for (const char *dir : { "/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/" })
	{
		const std::string path = std::string(dir) + "libSceUsbd.sprx";
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
			continue;
		int result = 0;
		module = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, &result);
		say("libSceUsbd: %s: loaded %#x (start %d)", path.c_str(), (unsigned)module, result);
		if (module >= 0)
			break;
	}
	if (module < 0)
	{
		say("libSceUsbd: not loadable (none of its three places has it, or the load failed)");
		return false;
	}
	std::string found, missing;
	symbol(module, "sceUsbdInit", usbd.Init, found, missing);
	symbol(module, "sceUsbdGetDeviceList", usbd.GetDeviceList, found, missing);
	symbol(module, "sceUsbdFreeDeviceList", usbd.FreeDeviceList, found, missing);
	symbol(module, "sceUsbdGetDeviceDescriptor", usbd.GetDeviceDescriptor, found, missing);
	symbol(module, "sceUsbdGetBusNumber", usbd.GetBusNumber, found, missing);
	symbol(module, "sceUsbdGetDeviceAddress", usbd.GetDeviceAddress, found, missing);
	symbol(module, "sceUsbdGetDeviceSpeed", usbd.GetDeviceSpeed, found, missing);
	symbol(module, "sceUsbdGetPortNumbers", usbd.GetPortNumbers, found, missing);
	symbol(module, "sceUsbdGetMaxIsoPacketSize", usbd.GetMaxIsoPacketSize, found, missing);
	symbol(module, "sceUsbdGetConfigDescriptor", usbd.GetConfigDescriptor, found, missing);
	symbol(module, "sceUsbdFreeConfigDescriptor", usbd.FreeConfigDescriptor, found, missing);
	symbol(module, "sceUsbdOpen", usbd.Open, found, missing);
	symbol(module, "sceUsbdClose", usbd.Close, found, missing);
	symbol(module, "sceUsbdKernelDriverActive", usbd.KernelDriverActive, found, missing);
	symbol(module, "sceUsbdDetachKernelDriver", usbd.DetachKernelDriver, found, missing);
	symbol(module, "sceUsbdClaimInterface", usbd.ClaimInterface, found, missing);
	symbol(module, "sceUsbdReleaseInterface", usbd.ReleaseInterface, found, missing);
	symbol(module, "sceUsbdSetInterfaceAltSetting", usbd.SetInterfaceAltSetting, found, missing);
	symbol(module, "sceUsbdControlTransfer", usbd.ControlTransfer, found, missing);
	symbol(module, "sceUsbdBulkTransfer", usbd.BulkTransfer, found, missing);
	symbol(module, "sceUsbdInterruptTransfer", usbd.InterruptTransfer, found, missing);
	symbol(module, "sceUsbdAllocTransfer", usbd.AllocTransfer, found, missing);
	symbol(module, "sceUsbdFreeTransfer", usbd.FreeTransfer, found, missing);
	symbol(module, "sceUsbdSubmitTransfer", usbd.SubmitTransfer, found, missing);
	symbol(module, "sceUsbdCancelTransfer", usbd.CancelTransfer, found, missing);
	symbol(module, "sceUsbdFillBulkTransfer", usbd.FillBulkTransfer, found, missing);
	symbol(module, "sceUsbdFillInterruptTransfer", usbd.FillInterruptTransfer, found, missing);
	symbol(module, "sceUsbdFillIsoTransfer", usbd.FillIsoTransfer, found, missing);
	symbol(module, "sceUsbdSetIsoPacketLengths", usbd.SetIsoPacketLengths, found, missing);
	symbol(module, "sceUsbdGetIsoPacketBuffer", usbd.GetIsoPacketBuffer, found, missing);
	symbol(module, "sceUsbdHandleEventsTimeout", usbd.HandleEventsTimeout, found, missing);
	say("libSceUsbd: found: %s", found.empty() ? "none" : found.c_str());
	say("libSceUsbd: not found: %s", missing.empty() ? "none" : missing.c_str());
	if (usbd.Init == nullptr || usbd.GetDeviceList == nullptr || usbd.FreeDeviceList == nullptr
			|| usbd.GetDeviceDescriptor == nullptr || usbd.Open == nullptr || usbd.Close == nullptr
			|| usbd.ControlTransfer == nullptr)
	{
		say("libSceUsbd: without the calls to list and open devices: not used");
		return false;
	}
	if (FILE *f = fopen(marker.c_str(), "wb"))
	{
		fputs("started\n", f);
		fclose(f);
	}
	const int rc = usbd.Init();
	if (FILE *f = fopen(marker.c_str(), "wb"))
	{
		fputs("done\n", f);
		fclose(f);
	}
	say("libSceUsbd: sceUsbdInit: %s", usbdError(rc).c_str());
	usbd.ok = rc >= 0;
	return usbd.ok;
}

bool usbdTransfers()
{
	return usbd.ok && usbd.AllocTransfer != nullptr && usbd.FreeTransfer != nullptr && usbd.SubmitTransfer != nullptr
			&& usbd.CancelTransfer != nullptr && usbd.HandleEventsTimeout != nullptr;
}

// The library's device list. Its count is libusb's ssize_t, or perhaps an
// int: a negative int read as a long looks like a large count, so it is
// taken as the int it would be, and no console has more than 256 devices.
long usbdList(void ***list)
{
	*list = nullptr;
	long count = usbd.GetDeviceList(list);
	if (count > 0x7FFFFFFFL)
		count = (int32_t)(uint32_t)count;
	if (count > 256)
		count = 256;
	return count;
}

void usbdEvents(int timeoutMs)
{
	timeval tv = {};
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;
	usbd.HandleEventsTimeout(&tv);
}

std::string usbdString(void *handle, uint8_t index)
{
	if (index == 0)
		return {};
	unsigned char buffer[256] = {};
	const int n = usbd.ControlTransfer(handle, 0x80, 0x06, (uint16_t)(0x0300 | index), 0x0409, buffer, sizeof(buffer), 500);
	std::string s;
	for (int i = 2; i + 1 < n && i < (int)sizeof(buffer) - 1; i += 2)
		s += buffer[i] >= 0x20 && buffer[i] < 0x7F && buffer[i + 1] == 0 ? (char)buffer[i] : '?';
	return s;
}

// ------------------------------------------------------------------ the devices
enum class Way { Usbd, Ugen };

const char *wayName(Way w)
{
	return w == Way::Usbd ? "libSceUsbd" : "ugen";
}

struct Listed
{
	Way way = Way::Ugen;
	std::string name;			// "/dev/ugen0.3", "libSceUsbd device 3"
	void *dev = nullptr;		// libSceUsbd: the device, while its list is held
	uint16_t vid = 0, pid = 0;
	uint8_t cls = 0;
	int bus = -1, address = -1, hubAddress = -1, speed = -1;	// -1: not said
	std::vector<uint8_t> ports;	// libSceUsbd: the port path, when the library gives it
	std::string product, manufacturer, serial;
	bool sony = false, hub = false, described = false;
	usb::DeviceDescriptor device;
	std::vector<uint8_t> raw;	// the configuration descriptor
	usb::Configuration config;
	bool video() const { return described && config.hasVideo(); }
	bool gun() const { return gunProduct(vid, pid); }
};

// What one look found. A libSceUsbd list is held while the devices in it are
// used, and let go with the look.
struct Scan
{
	std::vector<Listed> devices;
	void **usbdList = nullptr;
	Scan() = default;
	Scan(const Scan&) = delete;
	Scan& operator=(const Scan&) = delete;
	~Scan()
	{
		if (usbdList != nullptr && usbd.FreeDeviceList != nullptr)
			usbd.FreeDeviceList(usbdList, 1);
	}
	const Listed *gun(Way w) const
	{
		for (const Listed& d : devices)
			if (d.way == w && d.gun())
				return &d;
		return nullptr;
	}
};

std::string portsText(const std::vector<uint8_t>& p)
{
	if (p.empty())
		return "not said";
	std::string s;
	for (uint8_t x : p)
		s += (s.empty() ? "" : ".") + std::to_string(x);
	return s;
}

// The descriptors of a device in the boot log, once for each different set.
std::vector<std::vector<uint8_t>> dumped;

void dumpDescriptors(const Listed& d, const char *what)
{
	say("%s: %s: %04x:%04x, manufacturer \"%s\", product \"%s\", serial \"%s\", bus %d address %d, speed %d", what,
			d.name.c_str(), d.vid, d.pid, d.manufacturer.c_str(), d.product.c_str(), d.serial.c_str(), d.bus, d.address, d.speed);
	say("%s: device descriptor: %s", what, usb::describeDevice(d.device).c_str());
	if (!d.described)
	{
		say("%s: its configuration descriptor was not read", what);
		return;
	}
	for (const std::vector<uint8_t>& before : dumped)
		if (before == d.raw)
		{
			say("%s: its configuration descriptor (%zu bytes) is the one above", what, d.raw.size());
			return;
		}
	if (dumped.size() < 8)
		dumped.push_back(d.raw);
	sayBytes(format("%s: configuration descriptor (%zu bytes)", what, d.raw.size()), d.raw.data(), d.raw.size());
	usb::Configuration c;
	std::vector<std::string> lines;
	usb::parseConfiguration(d.raw.data(), d.raw.size(), c, &lines);
	for (const std::string& l : lines)
		say("%s: descriptor: %s", what, l.c_str());
}

bool mightBeCamera(uint8_t cls)
{
	return cls == 0x00 || cls == 0x0E || cls == 0xEF;
}

// Sony's devices and hubs as the kernel said what they are: not opened again
// while their node stays the same one (its inode and time), for 30 s at most.
struct KnownNode
{
	std::string path;
	ino_t inode = 0;
	time_t changed = 0;
	double until = 0;
	Listed device;
};
std::vector<KnownNode> knownNodes;

void listUgen(Scan& s, bool loud, bool describe)
{
	std::vector<std::string> nodes;
	std::string all;
	DIR *dir = opendir("/dev");
	if (dir == nullptr)
	{
		if (firstTime("ugen /dev"))
			say("ugen: /dev cannot be read (errno %d): the title is in its sandbox?", errno);
		return;
	}
	while (dirent *e = readdir(dir))
	{
		const std::string n = e->d_name;
		if (n.rfind("ugen", 0) == 0)
			nodes.push_back(n);
		// The kernel drivers' nodes say which drivers took which device.
		for (const char *kind : { "ugen", "uhid", "ums", "ukbd", "kbd", "cuaU", "ttyU", "umodem", "video", "usb" })
			if (loud && n.rfind(kind, 0) == 0)
			{
				all += " " + n;
				break;
			}
	}
	closedir(dir);
	if (loud)
		say("ugen: /dev's USB nodes:%s", all.empty() ? " none" : all.c_str());
	std::sort(nodes.begin(), nodes.end());
	const double t = now();
	knownNodes.erase(std::remove_if(knownNodes.begin(), knownNodes.end(), [&](const KnownNode& k) { return k.until < t; }),
			knownNodes.end());
	for (const std::string& n : nodes)
	{
		const std::string path = "/dev/" + n;
		struct stat st = {};
		const bool stated = stat(path.c_str(), &st) == 0;
		bool known = false;
		for (const KnownNode& k : knownNodes)
			if (stated && k.path == path && k.inode == st.st_ino && k.changed == st.st_ctime)
			{
				s.devices.push_back(k.device);
				known = true;
				break;
			}
		if (known)
			continue;
		const int fd = open(path.c_str(), O_RDWR);
		if (fd < 0)
		{
			if (firstTime(format("open %s %d", path.c_str(), errno)))
				say("ugen: %s: open: errno %d", path.c_str(), errno);
			continue;
		}
		usb_device_info info = {};
		if (ioctl(fd, USB_GET_DEVICEINFO, &info) != 0)
		{
			// Not known, so not read (PS5SX2's lesson).
			if (firstTime("info " + path))
				say("ugen: %s: the kernel does not say what it is (errno %d): left alone", path.c_str(), errno);
			close(fd);
			continue;
		}
		Listed d;
		d.way = Way::Ugen;
		d.name = path;
		d.vid = info.udi_vendorNo;
		d.pid = info.udi_productNo;
		d.cls = info.udi_class;
		d.bus = info.udi_bus;
		d.address = info.udi_addr;
		d.hubAddress = info.udi_hubaddr;
		d.speed = info.udi_speed;
		d.product = info.udi_product;
		d.manufacturer = info.udi_vendor;
		d.serial = info.udi_serial;
		d.sony = d.vid == SonyVendor;
		d.hub = d.cls == 9;
		if (firstTime(format("%s %04x:%04x", path.c_str(), d.vid, d.pid)))
			say("ugen: %s: %04x:%04x class %u, bus %u address %u, hub at address %u port %u, speed %u, \"%s\"%s", path.c_str(),
					d.vid, d.pid, d.cls, info.udi_bus, info.udi_addr, info.udi_hubaddr, info.udi_hubport, info.udi_speed,
					d.product.c_str(), d.sony ? ": Sony's, left to the system" : d.hub ? ": a hub" : "");
		// The descriptors the kernel keeps (no request goes to the device), for
		// the gun and for what may be its camera.
		if (describe && !d.sony && !d.hub && (d.vid == GunVendor || mightBeCamera(d.cls)))
		{
			usb_device_descriptor dd = {};
			if (ioctl(fd, USB_GET_DEVICE_DESC, &dd) == 0)
				usb::parseDevice(reinterpret_cast<const uint8_t *>(&dd), sizeof(dd), d.device);
			std::vector<uint8_t> buffer(8192);
			usb_gen_descriptor g = {};
			g.ugd_data = buffer.data();
			g.ugd_maxlen = (uint16_t)buffer.size();
			g.ugd_config_index = 0xFF;		// USB_UNCONFIG_INDEX: the configuration in use, as the kernel keeps it
			if (ioctl(fd, USB_GET_FULL_DESC, &g) == 0)
			{
				d.raw.assign(buffer.begin(), buffer.begin() + std::min<size_t>(g.ugd_actlen, buffer.size()));
				d.described = usb::parseConfiguration(d.raw.data(), d.raw.size(), d.config);
			}
			else if (d.vid == GunVendor)
				say("ugen: %s: its configuration descriptor: errno %d", path.c_str(), errno);
		}
		close(fd);
		if (stated && (d.sony || d.hub))
		{
			KnownNode k;
			k.path = path;
			k.inode = st.st_ino;
			k.changed = st.st_ctime;
			k.until = t + 30;
			k.device = d;
			knownNodes.push_back(k);
		}
		s.devices.push_back(d);
	}
	if (loud && nodes.empty())
		say("ugen: no ugen nodes");
}

// What libSceUsbd devices' descriptors were (by vendor, product, bus and
// address): a device is opened for them once.
std::vector<std::pair<std::string, Listed>> usbdDescribed;

void describeUsbd(Listed& d)
{
	const std::string key = format("%04x:%04x %d.%d", d.vid, d.pid, d.bus, d.address);
	for (const auto& e : usbdDescribed)
		if (e.first == key)
		{
			d.raw = e.second.raw;
			d.config = e.second.config;
			d.described = e.second.described;
			d.product = e.second.product;
			d.manufacturer = e.second.manufacturer;
			d.serial = e.second.serial;
			return;
		}
	void *handle = nullptr;
	const int rc = usbd.Open(d.dev, &handle);
	if (rc < 0 || handle == nullptr)
	{
		if (firstTime(format("usbd open %04x:%04x %d", d.vid, d.pid, rc)))
			say("libSceUsbd: %s (%04x:%04x): not opened: %s", d.name.c_str(), d.vid, d.pid, usbdError(rc).c_str());
		return;
	}
	uint8_t head[9] = {};
	int n = usbd.ControlTransfer(handle, 0x80, 0x06, 0x0200, 0, head, sizeof(head), 1000);
	if (n >= 4)
	{
		const uint16_t total = std::min<uint16_t>((uint16_t)(head[2] | (head[3] << 8)), 8192);
		std::vector<uint8_t> buffer(std::max<uint16_t>(total, 9));
		n = usbd.ControlTransfer(handle, 0x80, 0x06, 0x0200, 0, buffer.data(), (uint16_t)buffer.size(), 1000);
		if (n > 0)
		{
			d.raw.assign(buffer.begin(), buffer.begin() + n);
			d.described = usb::parseConfiguration(d.raw.data(), d.raw.size(), d.config);
		}
	}
	if (!d.described && firstTime(format("usbd config %04x:%04x", d.vid, d.pid)))
		say("libSceUsbd: %s (%04x:%04x): its configuration descriptor: %s", d.name.c_str(), d.vid, d.pid, usbdError(n).c_str());
	d.manufacturer = usbdString(handle, d.device.iManufacturer);
	d.product = usbdString(handle, d.device.iProduct);
	d.serial = usbdString(handle, d.device.iSerial);
	usbd.Close(handle);
	if (usbdDescribed.size() < 64)
		usbdDescribed.push_back({ key, d });
}

// Behind the same hub as another device (the libSceUsbd port paths): the
// same path but the last port, two levels down at least.
bool sameHub(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
	return a.size() >= 2 && a.size() == b.size() && std::equal(a.begin(), a.end() - 1, b.begin());
}

void listUsbd(Scan& s, bool loud, bool describe)
{
	if (!usbdLoad())
		return;
	void **list = nullptr;
	const long count = usbdList(&list);
	if (count < 0 || list == nullptr)
	{
		if (firstTime(format("usbd list %ld", count)))
			say("libSceUsbd: sceUsbdGetDeviceList: %s", usbdError((int)count).c_str());
		return;
	}
	s.usbdList = list;
	if (loud)
		say("libSceUsbd: %ld devices", count);
	const size_t first = s.devices.size();
	for (long i = 0; i < count; i++)
	{
		uint8_t dd[64] = {};	// libusb_device_descriptor: the descriptor's 18 bytes in their order
		const int rc = usbd.GetDeviceDescriptor(list[i], dd);
		if (rc < 0)
		{
			if (firstTime(format("usbd descriptor %ld %d", i, rc)))
				say("libSceUsbd: device %ld: its descriptor: %s", i, usbdError(rc).c_str());
			continue;
		}
		Listed d;
		d.way = Way::Usbd;
		d.name = format("libSceUsbd device %ld", i);
		d.dev = list[i];
		usb::parseDevice(dd, 18, d.device);
		d.vid = (uint16_t)(dd[8] | (dd[9] << 8));
		d.pid = (uint16_t)(dd[10] | (dd[11] << 8));
		d.cls = dd[4];
		if (usbd.GetBusNumber != nullptr)
			d.bus = usbd.GetBusNumber(list[i]);
		if (usbd.GetDeviceAddress != nullptr)
			d.address = usbd.GetDeviceAddress(list[i]);
		if (usbd.GetDeviceSpeed != nullptr)
			d.speed = usbd.GetDeviceSpeed(list[i]);
		if (usbd.GetPortNumbers != nullptr)
		{
			uint8_t ports[8] = {};
			const int n = usbd.GetPortNumbers(list[i], ports, (int)sizeof(ports));
			if (n > 0)
				d.ports.assign(ports, ports + n);
		}
		d.sony = d.vid == SonyVendor;
		d.hub = d.cls == 9;
		if (firstTime(format("usbd %04x:%04x %d.%d", d.vid, d.pid, d.bus, d.address)))
			say("libSceUsbd: device %ld: %04x:%04x class %u, bus %d address %d, ports %s, speed %d%s", i, d.vid, d.pid, d.cls,
					d.bus, d.address, portsText(d.ports).c_str(), d.speed,
					d.sony ? ": Sony's, left to the system" : d.hub ? ": a hub" : "");
		s.devices.push_back(d);
	}
	// The configuration descriptors of the gun's devices and of what may be its
	// camera: a device of the video or composite classes, or one behind the
	// gun's own hub. Never Sony's, a hub or a storage device.
	std::vector<std::vector<uint8_t>> gunPorts;
	bool gunHere = false;
	for (size_t i = first; i < s.devices.size(); i++)
		if (s.devices[i].vid == GunVendor)
		{
			gunPorts.push_back(s.devices[i].ports);
			gunHere = gunHere || s.devices[i].gun();
		}
	for (size_t i = first; i < s.devices.size() && describe; i++)
	{
		Listed& d = s.devices[i];
		if (d.sony || d.hub || d.cls == 8)
			continue;
		bool want = d.vid == GunVendor || (gunHere && (d.cls == 0x0E || d.cls == 0xEF));
		for (const auto& p : gunPorts)
			want = want || (gunHere && sameHub(p, d.ports));
		if (!want)
			continue;
		// Already read through ugen: the same device, not asked again.
		for (const Listed& u : s.devices)
			if (u.way == Way::Ugen && u.described && u.vid == d.vid && u.pid == d.pid && u.bus == d.bus && u.address == d.address)
			{
				d.raw = u.raw;
				d.config = u.config;
				d.described = true;
				d.product = u.product;
				d.manufacturer = u.manufacturer;
				d.serial = u.serial;
			}
		if (!d.described)
			describeUsbd(d);
	}
}

// One look: the ways the options allow, the gun's descriptors in the log
// (`describe`: read them; without it, only the list).
void list(Scan& s, bool loud, bool describe = true)
{
	if (options.route != 1)
		listUgen(s, loud, describe);
	if (options.route != 2)
		listUsbd(s, loud, describe);
	for (const Listed& d : s.devices)
	{
		if (d.vid != GunVendor || !describe)
			continue;
		if (!firstTime(format("described %s %04x:%04x %zu", d.name.c_str(), d.vid, d.pid, d.raw.size())))
			continue;
		if (d.gun())
			dumpDescriptors(d, "find");
		else
		{
			say("find: %s: %04x:%04x is not one of the gun's products (0f01, 0f02, 0f38, 0f39): described, nothing more",
					d.name.c_str(), d.vid, d.pid);
			dumpDescriptors(d, "find");
		}
	}
}

// The gun's camera, from the devices one way lists: the gun's own device; a
// camera named as the gun calls its camera; one behind the gun's hub; the
// only camera there is that is not Sony's.
const Listed *findCamera(const Scan& s, Way w, const Listed& gun, const std::string& cameraName, std::string& how)
{
	if (gun.video())
	{
		how = "the gun's own device has the video interfaces";
		return &gun;
	}
	std::vector<const Listed *> cameras;
	for (const Listed& d : s.devices)
		if (d.way == w && !d.sony && !d.hub && d.video() && &d != &gun)
			cameras.push_back(&d);
	if (!cameraName.empty())
		for (const Listed *c : cameras)
			if (c->product == cameraName)
			{
				how = "its name is the camera's name the gun gave";
				return c;
			}
	std::vector<const Listed *> behind;
	for (const Listed *c : cameras)
	{
		if (w == Way::Usbd && sameHub(gun.ports, c->ports))
			behind.push_back(c);
		if (w == Way::Ugen && c->bus == gun.bus && c->hubAddress == gun.hubAddress)
		{
			// Not the root hub (the hub's own hub address is 0): a hub of the gun's own.
			for (const Listed& h : s.devices)
				if (h.way == w && h.hub && h.bus == gun.bus && h.address == gun.hubAddress && h.hubAddress > 0)
					behind.push_back(c);
		}
	}
	if (behind.size() == 1)
	{
		how = "behind the same hub as the gun";
		return behind[0];
	}
	if (cameras.size() == 1)
	{
		how = "the only camera listed that is not Sony's";
		return cameras[0];
	}
	how = format("%zu cameras listed, none known to be the gun's (named \"%s\")", cameras.size(), cameraName.c_str());
	return nullptr;
}

// ------------------------------------------------------------------ transfers, either way
struct Completion
{
	class Link *link = nullptr;
	int pipe = -1, slot = -1;
	int status = 0;
	bool ok = false, timedOut = false, cancelled = false, gone = false;
	uint32_t length = 0;		// bulk and interrupt: the bytes
	uint32_t packets = 0;		// isochronous: the packets done
	const uint32_t *lengths = nullptr;	// their lengths
	const uint8_t *data = nullptr;
	uint32_t per = 0;			// isochronous: the room for each packet
	uint32_t badPackets = 0;	// isochronous packets with an error of their own (libSceUsbd says)
};

class Link
{
public:
	virtual ~Link() = default;
	virtual Way way() const = 0;
	// A request on endpoint 0: the bytes moved, or -1 (error says why).
	virtual int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t *data, uint16_t length,
			unsigned timeoutMs = 1000) = 0;
	// The driver that has an interface: its name, "" for none.
	virtual std::string driver(uint8_t iface) = 0;
	virtual bool detach(uint8_t iface) = 0;
	virtual bool claim(uint8_t iface) = 0;
	virtual bool setAlt(uint8_t iface, uint8_t alt) = 0;
	// An endpoint's transfers: `slots` of them, `bytes` each (isochronous:
	// `packets` packets of bytes / packets). Its number, or -1.
	virtual int openPipe(const usb::Endpoint& ep, uint32_t bytes, uint32_t packets, int slots, unsigned timeoutMs) = 0;
	virtual uint8_t *buffer(int pipe, int slot) = 0;
	virtual bool submit(int pipe, int slot, uint32_t length) = 0;
	virtual bool busy(int pipe, int slot) const = 0;
	virtual void closePipe(int pipe) = 0;
	// Waiting: the pollable descriptors (ugen), and what has come back.
	virtual int fds(pollfd *out, int most) = 0;
	virtual int collect(Completion *out, int most) = 0;
	virtual const char *statusName(int status) const = 0;
	virtual void close() = 0;

	std::string error;
	std::string opened;			// the last pipe as the route set it up, for the log
	bool gone = false;
	const usb::Configuration *config = nullptr;
	std::vector<uint8_t> claimed;

protected:
	// The alternate setting's index among the interface's, as FreeBSD counts them.
	uint8_t altIndex(uint8_t iface, uint8_t alt) const
	{
		uint8_t k = 0;
		if (config != nullptr)
			for (const usb::AltSetting& a : config->alts)
				if (a.number == iface)
				{
					if (a.alt == alt)
						return k;
					k++;
				}
		return alt;
	}
};

// ---- the kernel's ugen nodes, with FreeBSD's USB_FS
class UgenLink final : public Link
{
public:
	UgenLink(const std::string& path, const usb::Configuration *c) : path_(path)
	{
		config = c;
	}
	~UgenLink() override
	{
		close();
	}

	bool open()
	{
		ctl_ = ::open(path_.c_str(), O_RDWR);
		if (ctl_ < 0)
			error = format("open %s: errno %d", path_.c_str(), errno);
		return ctl_ >= 0;
	}

	Way way() const override
	{
		return Way::Ugen;
	}

	int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t *data, uint16_t length,
			unsigned) override
	{
		// The kernel's own timeout (USB_DEFAULT_TIMEOUT) applies.
		usb_ctl_request r = {};
		r.ucr_data = data;
		r.ucr_flags = 0x0004;	// USB_SHORT_XFER_OK
		r.ucr_request.bmRequestType = type;
		r.ucr_request.bRequest = request;
		r.ucr_request.wValue[0] = value & 0xFF;
		r.ucr_request.wValue[1] = value >> 8;
		r.ucr_request.wIndex[0] = index & 0xFF;
		r.ucr_request.wIndex[1] = index >> 8;
		r.ucr_request.wLength[0] = length & 0xFF;
		r.ucr_request.wLength[1] = length >> 8;
		if (ioctl(ctl_, USB_DO_REQUEST, &r) != 0)
		{
			error = format("errno %d", errno);
			return -1;
		}
		return r.ucr_actlen;
	}

	std::string driver(uint8_t iface) override
	{
		char name[64] = {};
		usb_gen_descriptor g = {};
		g.ugd_data = name;
		g.ugd_maxlen = sizeof(name) - 1;
		g.ugd_iface_index = (uint8_t)index(iface);
		if (ioctl(ctl_, USB_GET_IFACE_DRIVER, &g) == 0)
			return name[0] != 0 ? std::string(name) : std::string("a driver");
		const int first = errno;
		if (first == ENOENT)
			return "";
		int i = index(iface);
		if (ioctl(ctl_, USB_IFACE_DRIVER_ACTIVE, &i) == 0)
			return format("a driver (its name not given: errno %d)", first);
		return errno == ENXIO ? std::string() : format("not known (errno %d, %d)", first, errno);
	}

	bool detach(uint8_t iface) override
	{
		int i = index(iface);
		if (ioctl(ctl_, USB_IFACE_DRIVER_DETACH, &i) == 0)
			return true;
		error = format("errno %d", errno);
		return false;
	}

	bool claim(uint8_t iface) override
	{
		int i = index(iface);
		if (ioctl(ctl_, USB_CLAIM_INTERFACE, &i) != 0)
		{
			error = format("errno %d", errno);
			return false;
		}
		claimed.push_back(iface);
		return true;
	}

	bool setAlt(uint8_t iface, uint8_t alt) override
	{
		usb_alt_interface a = {};
		a.uai_interface_index = (uint8_t)index(iface);
		a.uai_alt_index = altIndex(iface, alt);
		if (ioctl(ctl_, USB_SET_ALTINTERFACE, &a) == 0)
			return true;
		error = format("errno %d", errno);
		return false;
	}

	int openPipe(const usb::Endpoint& ep, uint32_t bytes, uint32_t packets, int slots, unsigned timeoutMs) override
	{
		// Each endpoint on a descriptor of its own: the control node can be
		// opened more than once (FreeBSD's libusb does it), each open with its
		// own USB_FS, so setting an alternate on the first touches none.
		Pipe p;
		p.ep = ep;
		p.iso = ep.type() == usb::Isochronous;
		p.packets = p.iso ? std::max<uint32_t>(packets, 1) : 1;
		p.timeout = timeoutMs;
		p.fd = ::open(path_.c_str(), O_RDWR);
		if (p.fd < 0)
		{
			error = format("a second open of %s: errno %d", path_.c_str(), errno);
			return -1;
		}
		p.fs.assign((size_t)slots, usb_fs_endpoint());
		usb_fs_init init = {};
		init.pEndpoints = p.fs.data();
		init.ep_index_max = (uint8_t)slots;
		if (ioctl(p.fd, USB_FS_INIT, &init) != 0)
		{
			error = format("USB_FS_INIT: errno %d", errno);
			::close(p.fd);
			return -1;
		}
		usb_fs_open first = {};
		for (int s = 0; s < slots; s++)
		{
			usb_fs_open op = {};
			op.max_bufsize = bytes;
			op.max_frames = p.packets;
			op.ep_index = (uint8_t)s;
			op.ep_no = ep.address;
			if (ioctl(p.fd, USB_FS_OPEN, &op) != 0)
			{
				error = format("USB_FS_OPEN of endpoint 0x%02x (transfer %d): errno %d", ep.address, s, errno);
				closeFs(p, s);
				return -1;
			}
			if (s == 0)
				first = op;
		}
		if (p.iso)
		{
			p.per = bytes / p.packets;
			if (first.max_packet_length != 0 && first.max_packet_length < p.per)
				p.per = first.max_packet_length;
			// The kernel may give fewer frames a transfer than asked for.
			if (first.max_frames != 0 && first.max_frames < p.packets)
				p.packets = first.max_frames;
			p.bytes = p.per * p.packets;
		}
		else
		{
			p.per = 0;
			p.bytes = first.max_bufsize != 0 ? std::min(bytes, first.max_bufsize) : bytes;
		}
		opened = format("USB_FS: packets of up to %u bytes, buffers of %u, %u frames a transfer", first.max_packet_length,
				first.max_bufsize, first.max_frames);
		p.slots = slots;
		p.buffers.assign((size_t)slots, std::vector<uint8_t>(std::max<uint32_t>(p.bytes, 1), 0));
		p.pointers.assign((size_t)slots, std::vector<void *>(p.packets, nullptr));
		p.lengths.assign((size_t)slots, std::vector<uint32_t>(p.packets, 0));
		p.busy.assign((size_t)slots, 0);
		pipes_.push_back(std::move(p));
		return (int)pipes_.size() - 1;
	}

	uint8_t *buffer(int pipe, int slot) override
	{
		return pipes_[(size_t)pipe].buffers[(size_t)slot].data();
	}

	bool busy(int pipe, int slot) const override
	{
		return pipe >= 0 && pipes_[(size_t)pipe].busy[(size_t)slot] != 0;
	}

	bool submit(int pipe, int slot, uint32_t length) override
	{
		Pipe& p = pipes_[(size_t)pipe];
		if (p.fd < 0)
			return false;
		uint8_t *b = p.buffers[(size_t)slot].data();
		std::vector<void *>& ptr = p.pointers[(size_t)slot];
		std::vector<uint32_t>& len = p.lengths[(size_t)slot];
		if (p.iso)
			for (uint32_t k = 0; k < p.packets; k++)
			{
				ptr[k] = b + (size_t)k * p.per;
				len[k] = p.per;
			}
		else
		{
			ptr[0] = b;
			len[0] = std::min(length, p.bytes);
		}
		usb_fs_endpoint& e = p.fs[(size_t)slot];
		e.ppBuffer = ptr.data();
		e.pLength = len.data();
		e.nFrames = p.packets;
		e.aFrames = 0;
		e.flags = p.ep.in() ? (USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK) : 0;
		e.timeout = (uint16_t)std::min<unsigned>(p.timeout, 65535);
		e.status = 0;
		usb_fs_start st = {};
		st.ep_index = (uint8_t)slot;
		if (ioctl(p.fd, USB_FS_START, &st) != 0)
		{
			error = format("USB_FS_START: errno %d", errno);
			if (errno == ENXIO || errno == ENODEV)
				gone = true;
			return false;
		}
		p.busy[(size_t)slot] = 1;
		return true;
	}

	void closePipe(int pipe) override
	{
		if (pipe < 0 || (size_t)pipe >= pipes_.size())
			return;
		Pipe& p = pipes_[(size_t)pipe];
		if (p.fd >= 0)
			closeFs(p, p.slots);
	}

	int fds(pollfd *out, int most) override
	{
		int n = 0;
		for (const Pipe& p : pipes_)
			if (p.fd >= 0 && n < most && std::find(p.busy.begin(), p.busy.end(), 1) != p.busy.end())
				out[n++] = { p.fd, (short)(POLLIN | POLLOUT), 0 };
		return n;
	}

	int collect(Completion *out, int most) override
	{
		int n = 0;
		for (size_t i = 0; i < pipes_.size() && n < most; i++)
		{
			Pipe& p = pipes_[i];
			while (p.fd >= 0 && n < most)
			{
				usb_fs_complete c = {};
				if (ioctl(p.fd, USB_FS_COMPLETE, &c) != 0)
				{
					if (errno == ENXIO || errno == ENODEV)
						gone = true;
					break;		// EBUSY: nothing more done
				}
				const size_t s = c.ep_index;
				if (s >= p.fs.size())
					continue;
				p.busy[s] = 0;
				const usb_fs_endpoint& e = p.fs[s];
				Completion& o = out[n++];
				o = Completion();
				o.link = this;
				o.pipe = (int)i;
				o.slot = (int)s;
				o.status = e.status;
				o.ok = e.status == 0;		// USB_ERR_NORMAL_COMPLETION
				o.timedOut = e.status == 20;	// USB_ERR_TIMEOUT
				o.cancelled = e.status == 5;	// USB_ERR_CANCELLED
				o.data = p.buffers[s].data();
				o.lengths = p.lengths[s].data();
				o.per = p.per;
				if (p.iso)
				{
					o.packets = std::min(e.aFrames, p.packets);
					for (uint32_t k = 0; k < o.packets; k++)
						o.length += p.lengths[s][k];
				}
				else
				{
					o.packets = 1;
					o.length = p.lengths[s][0];
				}
			}
		}
		return n;
	}

	const char *statusName(int status) const override
	{
		return ugenStatus(status);
	}

	void close() override
	{
		for (Pipe& p : pipes_)
			if (p.fd >= 0)
				closeFs(p, p.slots);
		pipes_.clear();
		if (ctl_ >= 0)
		{
			for (uint8_t iface : claimed)
			{
				int i = index(iface);
				ioctl(ctl_, USB_RELEASE_INTERFACE, &i);
			}
			claimed.clear();
			::close(ctl_);
			ctl_ = -1;
		}
	}

private:
	struct Pipe
	{
		int fd = -1;
		usb::Endpoint ep;
		bool iso = false;
		uint32_t packets = 1, per = 0, bytes = 0;
		unsigned timeout = 0;
		int slots = 0;
		std::vector<usb_fs_endpoint> fs;
		std::vector<std::vector<uint8_t>> buffers;
		std::vector<std::vector<void *>> pointers;
		std::vector<std::vector<uint32_t>> lengths;
		std::vector<uint8_t> busy;
	};

	int index(uint8_t iface) const
	{
		const int i = config != nullptr ? config->indexOf(iface) : -1;
		return i >= 0 ? i : iface;
	}

	// Closes the first `slots` transfers (each waits for its own to stop), then USB_FS.
	static void closeFs(Pipe& p, int slots)
	{
		for (int s = 0; s < slots; s++)
		{
			usb_fs_close c = {};
			c.ep_index = (uint8_t)s;
			ioctl(p.fd, USB_FS_CLOSE, &c);
		}
		usb_fs_uninit u = {};
		ioctl(p.fd, USB_FS_UNINIT, &u);
		::close(p.fd);
		p.fd = -1;
		std::fill(p.busy.begin(), p.busy.end(), 0);
	}

	std::string path_;
	int ctl_ = -1;
	std::vector<Pipe> pipes_;
};

// ---- libSceUsbd, with libusb's asynchronous transfers
class UsbdLink;
std::vector<UsbdLink *> usbdLinks;		// the open ones, for the transfers' callback

class UsbdLink final : public Link
{
public:
	explicit UsbdLink(const usb::Configuration *c)
	{
		config = c;
	}
	~UsbdLink() override
	{
		close();
	}

	bool open(void *dev)
	{
		const int rc = usbd.Open(dev, &handle_);
		if (rc < 0 || handle_ == nullptr)
		{
			error = "sceUsbdOpen: " + usbdError(rc);
			handle_ = nullptr;
			return false;
		}
		usbdLinks.push_back(this);
		return true;
	}

	void *handle() const
	{
		return handle_;
	}

	Way way() const override
	{
		return Way::Usbd;
	}

	int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t *data, uint16_t length,
			unsigned timeoutMs) override
	{
		const int rc = usbd.ControlTransfer(handle_, type, request, value, index, data, length, timeoutMs);
		if (rc < 0)
		{
			error = usbdError(rc);
			return -1;
		}
		return rc;
	}

	std::string driver(uint8_t iface) override
	{
		if (usbd.KernelDriverActive == nullptr)
			return "not known (sceUsbdKernelDriverActive not found)";
		const int rc = usbd.KernelDriverActive(handle_, iface);
		return rc == 1 ? std::string("a driver") : rc == 0 ? std::string() : "not known (" + usbdError(rc) + ")";
	}

	bool detach(uint8_t iface) override
	{
		if (usbd.DetachKernelDriver == nullptr)
		{
			error = "sceUsbdDetachKernelDriver not found";
			return false;
		}
		const int rc = usbd.DetachKernelDriver(handle_, iface);
		if (rc < 0)
			error = usbdError(rc);
		return rc >= 0;
	}

	bool claim(uint8_t iface) override
	{
		if (usbd.ClaimInterface == nullptr)
		{
			error = "sceUsbdClaimInterface not found";
			return false;
		}
		const int rc = usbd.ClaimInterface(handle_, iface);
		if (rc < 0)
		{
			error = usbdError(rc);
			return false;
		}
		claimed.push_back(iface);
		return true;
	}

	bool setAlt(uint8_t iface, uint8_t alt) override
	{
		if (usbd.SetInterfaceAltSetting == nullptr)
		{
			error = "sceUsbdSetInterfaceAltSetting not found";
			return false;
		}
		const int rc = usbd.SetInterfaceAltSetting(handle_, iface, alt);
		if (rc < 0)
			error = usbdError(rc);
		return rc >= 0;
	}

	int openPipe(const usb::Endpoint& ep, uint32_t bytes, uint32_t packets, int slots, unsigned timeoutMs) override
	{
		Pipe p;
		p.ep = ep;
		p.iso = ep.type() == usb::Isochronous;
		p.packets = p.iso ? std::max<uint32_t>(packets, 1) : 1;
		p.per = p.iso ? bytes / p.packets : 0;
		p.bytes = p.iso ? p.per * p.packets : bytes;
		p.timeout = timeoutMs;
		const bool fill = p.iso ? usbd.FillIsoTransfer != nullptr && usbd.SetIsoPacketLengths != nullptr
				: ep.type() == usb::Bulk ? usbd.FillBulkTransfer != nullptr
				: usbd.FillInterruptTransfer != nullptr;
		if (!fill || !usbdTransfers() || !usbd.layout.ok)
		{
			error = !usbd.layout.ok ? "the library's transfer layout is not known"
					: p.iso ? "sceUsbdFillIsoTransfer or sceUsbdSetIsoPacketLengths not found"
					: ep.type() == usb::Bulk ? "sceUsbdFillBulkTransfer not found"
					: "sceUsbdFillInterruptTransfer not found";
			return -1;
		}
		for (int s = 0; s < slots; s++)
		{
			void *t = usbd.AllocTransfer(p.iso ? (int)p.packets : 0);
			if (t == nullptr)
			{
				error = "sceUsbdAllocTransfer gave none";
				for (void *made : p.transfers)
					usbd.FreeTransfer(made);
				return -1;
			}
			p.transfers.push_back(t);
		}
		p.buffers.assign((size_t)slots, std::vector<uint8_t>(std::max<uint32_t>(p.bytes, 1), 0));
		p.lengths.assign((size_t)slots, std::vector<uint32_t>(p.packets, 0));
		p.bad.assign((size_t)slots, 0);
		p.busy.assign((size_t)slots, 0);
		p.open = true;
		opened = format("libSceUsbd: %d asynchronous transfer%s", slots, slots == 1 ? "" : "s");
		pipes_.push_back(std::move(p));
		return (int)pipes_.size() - 1;
	}

	uint8_t *buffer(int pipe, int slot) override
	{
		return pipes_[(size_t)pipe].buffers[(size_t)slot].data();
	}

	bool busy(int pipe, int slot) const override
	{
		return pipe >= 0 && pipes_[(size_t)pipe].busy[(size_t)slot] != 0;
	}

	bool submit(int pipe, int slot, uint32_t length) override
	{
		Pipe& p = pipes_[(size_t)pipe];
		if (!p.open)
			return false;
		void *t = p.transfers[(size_t)slot];
		uint8_t *b = p.buffers[(size_t)slot].data();
		if (p.iso)
		{
			usbd.FillIsoTransfer(t, handle_, p.ep.address, b, (int)p.bytes, (int)p.packets, &UsbdLink::callback, this, p.timeout);
			usbd.SetIsoPacketLengths(t, p.per);
		}
		else if (p.ep.type() == usb::Bulk)
			usbd.FillBulkTransfer(t, handle_, p.ep.address, b, (int)std::min(length, p.bytes), &UsbdLink::callback, this, p.timeout);
		else
			usbd.FillInterruptTransfer(t, handle_, p.ep.address, b, (int)std::min(length, p.bytes), &UsbdLink::callback, this,
					p.timeout);
		const int rc = usbd.SubmitTransfer(t);
		if (rc < 0)
		{
			error = "sceUsbdSubmitTransfer: " + usbdError(rc);
			if (rc == -4)	// LIBUSB_ERROR_NO_DEVICE
				gone = true;
			return false;
		}
		p.busy[(size_t)slot] = 1;
		return true;
	}

	void closePipe(int pipe) override
	{
		if (pipe < 0 || (size_t)pipe >= pipes_.size() || !pipes_[(size_t)pipe].open)
			return;
		Pipe& p = pipes_[(size_t)pipe];
		for (size_t s = 0; s < p.transfers.size(); s++)
			if (p.busy[s])
				usbd.CancelTransfer(p.transfers[s]);
		// The cancelled transfers come back through the library's events.
		for (int round = 0; round < 40 && std::find(p.busy.begin(), p.busy.end(), 1) != p.busy.end(); round++)
			usbdEvents(25);
		unsigned left = 0;
		for (size_t s = 0; s < p.transfers.size(); s++)
		{
			if (p.busy[s])
				left++;		// still the library's: not freed
			else
				usbd.FreeTransfer(p.transfers[s]);
		}
		if (left != 0)
		{
			say("libSceUsbd: %u transfers on endpoint 0x%02x did not come back in a second: left to the library", left,
					p.ep.address);
			p.abandoned = true;
		}
		p.open = false;
	}

	int fds(pollfd *, int) override
	{
		return 0;
	}

	int collect(Completion *out, int most) override
	{
		int n = 0, i = 0;
		for (; i < queued_ && n < most; i++)
		{
			const Done& d = queue_[i];
			Pipe& p = pipes_[(size_t)d.pipe];
			if (!p.open)
				continue;
			Completion& o = out[n++];
			o = Completion();
			o.link = this;
			o.pipe = d.pipe;
			o.slot = d.slot;
			o.status = d.status;
			o.ok = d.status == 0;			// LIBUSB_TRANSFER_COMPLETED
			o.timedOut = d.status == 2;
			o.cancelled = d.status == 3;
			o.gone = d.status == 5;			// LIBUSB_TRANSFER_NO_DEVICE
			o.data = p.buffers[(size_t)d.slot].data();
			o.lengths = p.lengths[(size_t)d.slot].data();
			o.per = p.per;
			o.packets = p.packets;
			o.badPackets = p.bad[(size_t)d.slot];
			for (uint32_t k = 0; k < p.packets; k++)
				o.length += p.lengths[(size_t)d.slot][k];
			if (o.gone)
				gone = true;
		}
		std::memmove(queue_, queue_ + i, (size_t)(queued_ - i) * sizeof(Done));
		queued_ -= i;
		return n;
	}

	const char *statusName(int status) const override
	{
		return usbdStatus(status);
	}

	void close() override
	{
		for (size_t i = 0; i < pipes_.size(); i++)
		{
			closePipe((int)i);
			// The library may still write into a transfer it did not give
			// back: its memory is kept.
			if (pipes_[i].abandoned)
				abandonedPipes().push_back(std::move(pipes_[i]));
		}
		pipes_.clear();
		if (handle_ != nullptr)
		{
			for (uint8_t iface : claimed)
				if (usbd.ReleaseInterface != nullptr)
					usbd.ReleaseInterface(handle_, iface);
			claimed.clear();
			usbd.Close(handle_);
			handle_ = nullptr;
		}
		usbdLinks.erase(std::remove(usbdLinks.begin(), usbdLinks.end(), this), usbdLinks.end());
	}

	// Called by the library from sceUsbdHandleEventsTimeout, on the gun's thread.
	static void callback(void *transfer)
	{
		for (UsbdLink *l : usbdLinks)
			for (size_t i = 0; i < l->pipes_.size(); i++)
			{
				Pipe& p = l->pipes_[i];
				for (size_t s = 0; s < p.transfers.size(); s++)
					if (p.transfers[s] == transfer)
					{
						l->done((int)i, (int)s, transfer);
						return;
					}
			}
	}

private:
	struct Pipe
	{
		usb::Endpoint ep;
		bool iso = false, open = false, abandoned = false;
		uint32_t packets = 1, per = 0, bytes = 0;
		unsigned timeout = 0;
		std::vector<void *> transfers;
		std::vector<std::vector<uint8_t>> buffers;
		std::vector<std::vector<uint32_t>> lengths;
		std::vector<uint32_t> bad;
		std::vector<uint8_t> busy;
	};
	struct Done
	{
		int pipe, slot, status;
	};
	static std::vector<Pipe>& abandonedPipes()
	{
		static std::vector<Pipe> kept;
		return kept;
	}

	// A transfer back: its status and lengths read at the offsets found for
	// this library (usbdLayout).
	void done(int pipe, int slot, void *transfer)
	{
		const usb::TransferLayout& L = usbd.layout;
		const uint8_t *m = static_cast<const uint8_t *>(transfer);
		Pipe& p = pipes_[(size_t)pipe];
		p.busy[(size_t)slot] = 0;
		int32_t status = 0, actual = 0;
		std::memcpy(&status, m + L.status, 4);
		std::memcpy(&actual, m + L.actual, 4);
		std::vector<uint32_t>& len = p.lengths[(size_t)slot];
		if (p.iso)
		{
			uint32_t bad = 0;
			for (uint32_t k = 0; k < p.packets; k++)
			{
				const uint8_t *d = m + L.packetDescriptors + (size_t)k * (size_t)L.packetStride;
				uint32_t got = 0;
				int32_t packetStatus = 0;
				std::memcpy(&got, d + 4, 4);
				std::memcpy(&packetStatus, d + 8, 4);
				len[k] = std::min(got, p.per);
				if (packetStatus != 0)
					bad++;
			}
			p.bad[(size_t)slot] = bad;
		}
		else
			len[0] = actual > 0 ? std::min((uint32_t)actual, p.bytes) : 0;
		if (queued_ < (int)(sizeof(queue_) / sizeof(queue_[0])))
			queue_[queued_++] = { pipe, slot, status };
	}

	void *handle_ = nullptr;
	std::vector<Pipe> pipes_;
	Done queue_[64] = {};
	int queued_ = 0;
};

// libSceUsbd's transfer layout, found once with a real handle: a transfer
// filled by the library's own fill functions with values that cannot be
// mistaken (usb::findTransferLayout). It is allocated with more packets than
// it is filled with, so the bytes looked at are all its own.
bool usbdLayout(void *handle)
{
	if (usbd.layoutTried)
		return usbd.layout.ok;
	usbd.layoutTried = true;
	if (!usbdTransfers() || usbd.FillIsoTransfer == nullptr || usbd.SetIsoPacketLengths == nullptr)
	{
		usbd.layout.why = "the asynchronous and isochronous calls are not all there";
		say("libSceUsbd: transfers: %s: not used for the gun", usbd.layout.why.c_str());
		return false;
	}
	constexpr int Allocated = 32, Filled = 13;
	constexpr size_t Looked = 288;		// 64 + 4 + 13 x 16 + 12, within 32 descriptors of 12 bytes after any header
	void *t = usbd.AllocTransfer(Allocated);
	if (t == nullptr)
	{
		usbd.layout.why = "sceUsbdAllocTransfer gave none";
		say("libSceUsbd: transfers: %s", usbd.layout.why.c_str());
		return false;
	}
	static uint8_t buffer[16];
	usb::TransferValues v;
	v.handle = (uint64_t)(uintptr_t)handle;
	v.callback = (uint64_t)(uintptr_t)&UsbdLink::callback;
	v.userData = 0x5153494E44454E31ull;		// never followed: only stored
	v.buffer = (uint64_t)(uintptr_t)buffer;
	v.length = 0x000A5A5A;
	v.timeout = 0x00C0FFEE;
	v.packets = Filled;
	v.packetLength = 0xBEEF;
	v.endpoint = 0x8D;
	usbd.FillIsoTransfer(t, handle, v.endpoint, buffer, (int)v.length, Filled, &UsbdLink::callback,
			reinterpret_cast<void *>((uintptr_t)v.userData), v.timeout);
	usbd.SetIsoPacketLengths(t, v.packetLength);
	uint8_t copy[Looked];
	std::memcpy(copy, t, sizeof(copy));
	usb::TransferLayout L = usb::findTransferLayout(copy, sizeof(copy), v);
	if (L.ok && usbd.GetIsoPacketBuffer != nullptr)
	{
		const unsigned char *fifth = usbd.GetIsoPacketBuffer(t, 5);
		if (fifth != buffer + 5 * v.packetLength)
		{
			L.ok = false;
			L.why = format("sceUsbdGetIsoPacketBuffer(5) is buffer + %td, not + %u", fifth - buffer, 5 * v.packetLength);
		}
	}
	usbd.FreeTransfer(t);
	usbd.layout = L;
	if (L.ok)
		say("libSceUsbd: the transfer's layout: %s", usb::describeLayout(L).c_str());
	else
	{
		say("libSceUsbd: the transfer's layout not found (%s): its transfers not used", L.why.c_str());
		sayBytes("libSceUsbd: a filled transfer", copy, sizeof(copy));
	}
	return L.ok;
}

// Waits up to timeoutMs for any of the links' transfers. Never returns at
// once with nothing done, whatever the route answers: no busy loop.
int waitLinks(Link *const *links, int count, int timeoutMs, Completion *out, int most)
{
	const double began = now();
	bool events = false;
	pollfd fds[32];
	int nf = 0;
	for (int i = 0; i < count; i++)
	{
		if (links[i] == nullptr)
			continue;
		if (links[i]->way() == Way::Usbd)
			events = true;
		else
			nf += links[i]->fds(fds + nf, 32 - nf);
	}
	if (events)
		usbdEvents(timeoutMs);
	else if (nf > 0)
		poll(fds, (nfds_t)nf, timeoutMs);
	int n = 0;
	for (int i = 0; i < count && n < most; i++)
		if (links[i] != nullptr)
			n += links[i]->collect(out + n, most - n);
	const double waited = (now() - began) * 1000;
	if (n == 0 && waited < timeoutMs / 2.0)
		usleep((useconds_t)((timeoutMs - waited) * 1000));
	return n;
}

// ------------------------------------------------------------------ the gun, one way
enum class Outcome { Stopped, Gone, CameraFailed };

class Session
{
public:
	Session(Way way, Scan& scan, const Listed& gun) : way_(way), scan_(scan), gunDev_(gun) {}
	~Session()
	{
		close();
	}

	bool open();
	void serialStage();
	void buttonsStage();
	bool cameraStage();
	Outcome run(bool mayGiveUp);
	void close();

	std::string cameraFailure;
	bool cameraTransport = false;	// the camera failed in a way the other route may not

private:
	struct Hid
	{
		usb::HidPart part;
		usb::HidLayout layout;
		bool boot = false;
		int pipe = -1;
		uint32_t bytes = 8;
		Buttons state;
		uint64_t reports = 0;
		unsigned changesSaid = 0;
		uint8_t last[64] = {};
		size_t lastBytes = 0;
		unsigned emptyInRow = 0;
		bool stoppedSaid = false;
	};
	enum class Cam { None, Starting, Streaming, Stalled, Failed };

	Link *openLink(const Listed& d);
	void take(Link *l, uint8_t iface, const char *what);
	void pump(int timeoutMs);
	void pumpFor(double seconds);
	void dispatch(const Completion& c);
	size_t query(uint8_t command, uint8_t *answer, size_t room);
	void hidDone(Hid& h, const Completion& c);
	void videoDone(const Completion& c);
	void picture();
	void cameraNoPicture();
	void cameraLines(double t);
	void cameraStop(const std::string& why);
	bool stillThere();

	Way way_;
	Scan& scan_;
	const Listed& gunDev_;
	const Listed *camDev_ = nullptr;
	Link *gun_ = nullptr, *cam_ = nullptr;
	usb::Parts parts_;

	// The serial port.
	int serialIn_ = -1, serialOut_ = -1;
	uint32_t serialInBytes_ = 64;
	uint8_t rx_[256] = {};
	size_t rxBytes_ = 0;
	unsigned rxEmptyInRow_ = 0;
	bool rxStoppedSaid_ = false;
	std::string cameraName_;

	// The buttons.
	std::vector<Hid> hids_;

	// The camera.
	Cam camState_ = Cam::None;
	int video_ = -1;
	uint8_t vs_ = 0;
	bool altSet_ = false;
	usb::StreamChoice stream_;
	usb::AltChoice alt_;
	usb::Assembler assembler_;
	usb::Mjpeg mjpeg_;
	uint32_t videoBytes_ = 0;
	double streamStarted_ = 0, firstPicture_ = -1, lastPicture_ = 0, nextLine_ = 0;
	unsigned linesSaid_ = 0, packetsSaid_ = 0, errorsSaid_ = 0, decodeInRow_ = 0, failedInRow_ = 0;
	uint64_t transfers_ = 0, transferErrors_ = 0, packets_ = 0, emptyPackets_ = 0, badPackets_ = 0, decodeFailed_ = 0;
	int lastStatus_ = 0;
	usb::Rate rate_, window_;
	double windowStart_ = 0;
};

Link *Session::openLink(const Listed& d)
{
	if (way_ == Way::Usbd)
	{
		UsbdLink *l = new UsbdLink(&d.config);
		if (!l->open(d.dev))
		{
			say("%s: %s: %s", wayName(way_), d.name.c_str(), l->error.c_str());
			delete l;
			return nullptr;
		}
		if (!usbdLayout(l->handle()))
		{
			delete l;
			return nullptr;
		}
		return l;
	}
	UgenLink *l = new UgenLink(d.name, &d.config);
	if (!l->open())
	{
		say("%s: %s: %s", wayName(way_), d.name.c_str(), l->error.c_str());
		delete l;
		return nullptr;
	}
	return l;
}

bool Session::open()
{
	if (!gunDev_.described)
	{
		say("%s: the gun's configuration descriptor was not read: not set up this way", wayName(way_));
		return false;
	}
	parts_ = usb::findParts(gunDev_.config);
	say("%s: the gun's parts: serial port %s, %zu HID interface%s, video %s", wayName(way_),
			parts_.serialControl >= 0 ? format("interfaces %d and %d", parts_.serialControl, parts_.serialData).c_str() : "none",
			parts_.hid.size(), parts_.hid.size() == 1 ? "" : "s",
			parts_.videoStreaming >= 0 ? format("interfaces %d and %d", parts_.videoControl, parts_.videoStreaming).c_str()
									   : "not on this device");
	gun_ = openLink(gunDev_);
	return gun_ != nullptr;
}

void Session::take(Link *l, uint8_t iface, const char *what)
{
	const std::string d = l->driver(iface);
	if (d.empty())
		say("%s: interface %u: no driver has it", what, iface);
	else
	{
		const bool ok = l->detach(iface);
		say("%s: interface %u: %s has it; let go: %s", what, iface, d.c_str(), ok ? "yes" : ("no, " + l->error).c_str());
	}
	if (!l->claim(iface))
		say("%s: interface %u: not claimed (%s)", what, iface, l->error.c_str());
}

void Session::pump(int timeoutMs)
{
	Link *links[2] = { gun_, cam_ != gun_ ? cam_ : nullptr };
	Completion done[48];
	const int n = waitLinks(links, 2, timeoutMs, done, 48);
	for (int i = 0; i < n; i++)
		dispatch(done[i]);
}

void Session::pumpFor(double seconds)
{
	const double until = now() + seconds;
	while (now() < until && !stopping && !(gun_ != nullptr && gun_->gone))
		pump(20);
}

void Session::dispatch(const Completion& c)
{
	if (c.link == gun_ && c.pipe == serialIn_ && serialIn_ >= 0)
	{
		if (c.ok && c.length > 0)
		{
			const size_t n = std::min<size_t>(c.length, sizeof(rx_) - rxBytes_);
			std::memcpy(rx_ + rxBytes_, c.data, n);
			rxBytes_ += n;
		}
		rxEmptyInRow_ = c.ok && c.length == 0 ? rxEmptyInRow_ + 1 : 0;
		if (rxEmptyInRow_ >= 50)
		{
			if (!rxStoppedSaid_)
				say("serial: 50 empty reads in a row: reading stopped");
			rxStoppedSaid_ = true;
			return;
		}
		if (c.ok || c.timedOut)
		{
			if (!stopping && !gun_->submit(serialIn_, 0, serialInBytes_) && !rxStoppedSaid_)
			{
				say("serial: reading stopped: %s", gun_->error.c_str());
				rxStoppedSaid_ = true;
			}
		}
		else if (!c.cancelled && !rxStoppedSaid_)
		{
			say("serial: reading stopped: %s (%d)", gun_->statusName(c.status), c.status);
			rxStoppedSaid_ = true;
		}
		return;
	}
	if (c.link == gun_ && c.pipe == serialOut_ && serialOut_ >= 0)
	{
		if (!c.ok)
			say("serial: a command was not sent: %s (%d)", gun_->statusName(c.status), c.status);
		return;
	}
	if (c.link == cam_ && c.pipe == video_ && video_ >= 0)
	{
		videoDone(c);
		return;
	}
	for (Hid& h : hids_)
		if (c.link == gun_ && c.pipe == h.pipe)
		{
			hidDone(h, c);
			return;
		}
}

// ---- 2. the serial port
size_t Session::query(uint8_t command, uint8_t *answer, size_t room)
{
	for (int i = 0; i < 50 && gun_->busy(serialOut_, 0); i++)
		pump(20);
	rxBytes_ = 0;		// what came before is not the answer (gun_serial.py's reset_input_buffer)
	const auto f = usb::serial::frame(command);
	std::memcpy(gun_->buffer(serialOut_, 0), f.data(), f.size());
	if (!gun_->submit(serialOut_, 0, (uint32_t)f.size()))
	{
		say("serial: %u (%s): not sent: %s", command, usb::serial::commandName(command), gun_->error.c_str());
		return 0;
	}
	const size_t want = usb::serial::answerLength(command);
	const double until = now() + SerialAnswerWithin;
	while (now() < until && rxBytes_ < want && !stopping && !gun_->gone)
		pump(20);
	const size_t n = std::min(rxBytes_, room);
	std::memcpy(answer, rx_, n);
	return n;
}

void Session::serialStage()
{
	if (parts_.serialControl < 0 || parts_.serialData < 0 || parts_.serialIn.address == 0 || parts_.serialOut.address == 0)
	{
		say("serial: no CDC ACM serial port with bulk endpoints in the gun's descriptors: skipped");
		return;
	}
	const uint8_t control = (uint8_t)parts_.serialControl;
	take(gun_, control, "serial");
	take(gun_, (uint8_t)parts_.serialData, "serial");
	uint8_t lc[7] = {};
	int n = gun_->control(0xA1, 0x21, 0, control, lc, sizeof(lc));		// GET_LINE_CODING
	say("serial: line coding before: %s", n < 0 ? ("not read, " + gun_->error).c_str() : usb::serial::describeLineCoding(lc, (size_t)n).c_str());
	usb::serial::lineCoding(115200, lc);
	n = gun_->control(0x21, 0x20, 0, control, lc, sizeof(lc));		// SET_LINE_CODING
	say("serial: line coding 115200 8N1: %s", n < 0 ? ("not set, " + gun_->error).c_str() : "set");
	n = gun_->control(0x21, 0x22, 0x0003, control, nullptr, 0);		// SET_CONTROL_LINE_STATE: DTR and RTS
	say("serial: DTR and RTS on: %s", n < 0 ? ("no, " + gun_->error).c_str() : "yes");
	serialInBytes_ = std::max<uint32_t>(64, parts_.serialIn.maxPacket & 0x7FF);
	serialIn_ = gun_->openPipe(parts_.serialIn, serialInBytes_, 1, 1, 0);
	if (serialIn_ < 0)
	{
		say("serial: endpoint 0x%02x not opened: %s", parts_.serialIn.address, gun_->error.c_str());
		return;
	}
	serialOut_ = gun_->openPipe(parts_.serialOut, 64, 1, 1, 1000);
	if (serialOut_ < 0)
	{
		say("serial: endpoint 0x%02x not opened: %s", parts_.serialOut.address, gun_->error.c_str());
		return;
	}
	say("serial: endpoints 0x%02x and 0x%02x open (%s)", parts_.serialIn.address, parts_.serialOut.address, gun_->opened.c_str());
	if (!gun_->submit(serialIn_, 0, serialInBytes_))
	{
		say("serial: reading not started: %s", gun_->error.c_str());
		return;
	}
	// The gun's microcontroller settles first (gun_serial.py waits 2 s after
	// opening the port).
	pumpFor(SerialSettle);
	if (rxBytes_ > 0)
		say("serial: %zu bytes came unasked: %s", rxBytes_, hex(rx_, rxBytes_).c_str());
	bool any = false;
	Calibration cal;
	bool haveX = false, haveY = false;
	std::string firmware;
	for (uint8_t command : { usb::serial::Firmware, usb::serial::CameraName, usb::serial::CalibrationX, usb::serial::CalibrationY })
	{
		if (stopping || gun_->gone)
			return;
		uint8_t answer[64] = {};
		const size_t got = query(command, answer, sizeof(answer));
		const size_t want = usb::serial::answerLength(command);
		const auto f = usb::serial::frame(command);
		const std::string sent = hex(f.data(), f.size());
		if (got < want)
		{
			say("serial: %u (%s): sent %s; %s", command, usb::serial::commandName(command), sent.c_str(),
					got == 0 ? "no answer in 1.5 s" : format("only %zu bytes back: %s", got, hex(answer, got).c_str()).c_str());
			continue;
		}
		any = true;
		std::string meaning;
		if (command == usb::serial::Firmware)
		{
			firmware = usb::serial::firmware(answer, got);
			meaning = "firmware " + firmware;
		}
		else if (command == usb::serial::CameraName)
		{
			cameraName_ = usb::serial::cameraName(answer, want);
			meaning = "camera \"" + cameraName_ + "\"";
		}
		else
		{
			int raw = 0;
			double value = 0;
			usb::serial::calibration(answer, got, raw, value);
			if (command == usb::serial::CalibrationX)
			{
				cal.rawX = raw;
				cal.x = value;
				haveX = true;
			}
			else
			{
				cal.rawY = raw;
				cal.y = value;
				haveY = true;
			}
			meaning = format("raw %d, %.2f%s", raw, value, value > 99 || value < -99 ? " (over 99: none set)" : "");
		}
		say("serial: %u (%s): sent %s; answer (%zu bytes) %s: %s", command, usb::serial::commandName(command), sent.c_str(),
				got, hex(answer, got).c_str(), meaning.c_str());
	}
	cal.known = haveX && haveY;
	std::lock_guard<std::mutex> g(lock);
	shared.serial = any;
	shared.firmware = firmware;
	shared.camera = cameraName_;
	sharedCalibration = cal;
}

// ---- 3. the buttons
void Session::buttonsStage()
{
	if (parts_.hid.empty())
	{
		say("buttons: no HID interface with an interrupt endpoint in the gun's descriptors");
		return;
	}
	for (const usb::HidPart& part : parts_.hid)
	{
		Hid h;
		h.part = part;
		uint8_t rd[1024] = {};
		const uint16_t want = part.reportLength != 0 ? std::min<uint16_t>(part.reportLength, sizeof(rd)) : 512;
		const int n = gun_->control(0x81, 0x06, 0x2200, part.iface, rd, want);		// GET_DESCRIPTOR, report
		if (n > 0)
		{
			sayBytes(format("buttons: interface %u: report descriptor (%d bytes)", part.iface, n), rd, (size_t)n);
			h.layout = usb::parseHid(rd, (size_t)n);
		}
		else
			say("buttons: interface %u: report descriptor not read (%s)", part.iface, gun_->error.c_str());
		if (!h.layout.ok && part.subclass == 1)
		{
			h.layout = usb::bootLayout(part.protocol);
			h.boot = true;
		}
		say("buttons: interface %u (subclass %u, protocol %u): %s", part.iface, part.subclass, part.protocol,
				h.layout.summary.c_str());
		take(gun_, part.iface, "buttons");
		if (part.subclass == 1)
		{
			// A boot interface may have been put in either protocol by the driver
			// that had it: the one whose reports the layout above reads.
			const int rc = gun_->control(0x21, 0x0B, h.boot ? 0 : 1, part.iface, nullptr, 0);	// SET_PROTOCOL
			say("buttons: interface %u: %s protocol: %s", part.iface, h.boot ? "boot" : "report",
					rc < 0 ? ("not set, " + gun_->error).c_str() : "set");
		}
		h.bytes = std::max<uint32_t>(part.in.maxPacket & 0x7FF, 8);
		h.pipe = gun_->openPipe(part.in, h.bytes, 1, 2, 0);
		if (h.pipe < 0)
		{
			say("buttons: interface %u: endpoint 0x%02x not opened: %s", part.iface, part.in.address, gun_->error.c_str());
			continue;
		}
		const bool ok = gun_->submit(h.pipe, 0, h.bytes) && gun_->submit(h.pipe, 1, h.bytes);
		say("buttons: interface %u: reading endpoint 0x%02x (%u bytes a report)%s", part.iface, part.in.address, h.bytes,
				ok ? "" : (": " + gun_->error).c_str());
		hids_.push_back(h);
	}
}

void Session::hidDone(Hid& h, const Completion& c)
{
	if (c.cancelled)
		return;
	if (!c.ok && !c.timedOut)
	{
		if (!h.stoppedSaid)
			say("buttons: interface %u: reading stopped: %s (%d)", h.part.iface, gun_->statusName(c.status), c.status);
		h.stoppedSaid = true;
		return;
	}
	h.emptyInRow = c.ok && c.length == 0 ? h.emptyInRow + 1 : 0;
	if (h.emptyInRow >= 50)
	{
		say("buttons: interface %u: 50 empty reports in a row: reading stopped", h.part.iface);
		h.stoppedSaid = true;
		return;
	}
	if (c.ok && c.length > 0)
	{
		h.reports++;
		const size_t n = std::min<size_t>(c.length, sizeof(h.last));
		const bool changed = n != h.lastBytes || std::memcmp(h.last, c.data, n) != 0;
		usb::decodeHid(h.layout, c.data, c.length, h.state);
		// The first reports, then each change (the first 100), so that a log
		// shows what each press sends.
		if (h.reports <= 4 || (changed && h.changesSaid < 100))
		{
			say("buttons: interface %u: report %llu (%u bytes): %s: %s", h.part.iface, (unsigned long long)h.reports, c.length,
					hex(c.data, c.length).c_str(), usb::describeButtons(h.state).c_str());
			if (changed)
				h.changesSaid++;
		}
		std::memcpy(h.last, c.data, n);
		h.lastBytes = n;
		Buttons all;
		unsigned keys = 0;
		for (const Hid& x : hids_)
		{
			all.mouse |= x.state.mouse;
			all.joystick |= x.state.joystick;
			all.modifiers |= x.state.modifiers;
			all.reports += x.reports;
			for (uint8_t k : x.state.keys)
				if (k != 0 && keys < 6)
					all.keys[keys++] = k;
		}
		std::lock_guard<std::mutex> g(lock);
		sharedButtons = all;
		shared.buttons = true;
	}
	if (!stopping && !gun_->submit(c.pipe, c.slot, h.bytes) && !h.stoppedSaid)
	{
		say("buttons: interface %u: reading stopped: %s", h.part.iface, gun_->error.c_str());
		h.stoppedSaid = true;
	}
}

// ---- 4. the camera
bool Session::cameraStage()
{
	std::string how;
	camDev_ = findCamera(scan_, way_, gunDev_, cameraName_, how);
	if (camDev_ == nullptr)
	{
		cameraFailure = "no camera found: " + how;
		say("camera: %s", cameraFailure.c_str());
		return false;
	}
	say("camera: %s (%04x:%04x \"%s\"): %s", camDev_->name.c_str(), camDev_->vid, camDev_->pid, camDev_->product.c_str(),
			how.c_str());
	if (camDev_ != &gunDev_)
	{
		if (firstTime(format("camera described %s %zu", camDev_->name.c_str(), camDev_->raw.size())))
			dumpDescriptors(*camDev_, "camera");
		cam_ = openLink(*camDev_);
		if (cam_ == nullptr)
		{
			cameraFailure = "the camera's device was not opened";
			cameraTransport = true;
			return false;
		}
	}
	else
		cam_ = gun_;
	const usb::Configuration& config = camDev_->config;
	const usb::Parts parts = usb::findParts(config);
	if (parts.videoStreaming < 0)
	{
		cameraFailure = "no video streaming interface";
		say("camera: %s", cameraFailure.c_str());
		return false;
	}
	vs_ = (uint8_t)parts.videoStreaming;
	stream_ = usb::chooseStream(config, vs_, 320, 240, MostWidth, MostHeight);
	say("camera: chosen: %s", stream_.why.c_str());
	if (!stream_.ok)
	{
		cameraFailure = stream_.why;
		return false;
	}
	if (parts.videoControl >= 0)
		take(cam_, (uint8_t)parts.videoControl, "camera");
	take(cam_, vs_, "camera");

	// UVC's probe and commit (UVC 1.1 4.3.1.1): what the camera has, what is
	// asked, what it answers, and that answer committed.
	const size_t length = usb::probeLength(config.uvcVersion());
	uint8_t b[48] = {};
	int n = cam_->control(0xA1, 0x81, 0x0100, vs_, b, (uint16_t)length);	// GET_CUR, VS_PROBE_CONTROL
	usb::Probe asked;
	if (n > 0)
	{
		asked = usb::unpackProbe(b, (size_t)n);
		say("camera: probe as it was (%d bytes): %s", n, usb::describeProbe(asked, (size_t)n).c_str());
	}
	else
		say("camera: probe not read (%s)", cam_->error.c_str());
	asked.hint = 0x0001;		// the frame interval is the one asked for
	asked.formatIndex = stream_.formatIndex;
	asked.frameIndex = stream_.frameIndex;
	asked.interval = stream_.interval;
	usb::packProbe(asked, b, length);
	n = cam_->control(0x21, 0x01, 0x0100, vs_, b, (uint16_t)length);		// SET_CUR, VS_PROBE_CONTROL
	if (n < 0)
	{
		cameraFailure = "the camera refused the probe (" + cam_->error + ")";
		cameraTransport = true;
		say("camera: %s", cameraFailure.c_str());
		return false;
	}
	std::memset(b, 0, sizeof(b));
	n = cam_->control(0xA1, 0x81, 0x0100, vs_, b, (uint16_t)length);
	if (n < 26)
	{
		cameraFailure = format("the probe's answer was not read (%d bytes, %s)", n, cam_->error.c_str());
		cameraTransport = true;
		say("camera: %s", cameraFailure.c_str());
		return false;
	}
	const usb::Probe answer = usb::unpackProbe(b, (size_t)n);
	say("camera: probe answered (%d bytes): %s", n, usb::describeProbe(answer, (size_t)n).c_str());
	if (answer.formatIndex != stream_.formatIndex || answer.frameIndex != stream_.frameIndex)
	{
		cameraFailure = format("the camera answered format %u frame %u for format %u frame %u", answer.formatIndex,
				answer.frameIndex, stream_.formatIndex, stream_.frameIndex);
		say("camera: %s", cameraFailure.c_str());
		return false;
	}
	n = cam_->control(0x21, 0x01, 0x0200, vs_, b, (uint16_t)length);		// SET_CUR, VS_COMMIT_CONTROL: its own answer
	say("camera: committed: %s", n < 0 ? ("no, " + cam_->error).c_str() : "yes");
	if (n < 0)
	{
		cameraFailure = "the camera refused the commit (" + cam_->error + ")";
		cameraTransport = true;
		return false;
	}
	alt_ = usb::chooseAlt(config, vs_, answer.maxPayloadTransferSize);
	say("camera: %s", alt_.why.c_str());
	if (!alt_.ok)
	{
		cameraFailure = alt_.why;
		return false;
	}
	if (!alt_.bulk)
	{
		if (!cam_->setAlt(vs_, alt_.alt))
		{
			cameraFailure = format("alternate setting %u refused (%s)", alt_.alt, cam_->error.c_str());
			cameraTransport = true;
			say("camera: %s", cameraFailure.c_str());
			return false;
		}
		altSet_ = true;
		say("camera: interface %u alternate setting %u set", vs_, alt_.alt);
		// FreeBSD probes drivers again for an interface whose setting changed.
		const std::string again = cam_->driver(vs_);
		if (!again.empty())
		{
			const bool ok = cam_->detach(vs_);
			say("camera: interface %u: %s took it again; let go: %s", vs_, again.c_str(), ok ? "yes" : ("no, " + cam_->error).c_str());
		}
	}

	// Room for one picture as the camera says it can be, at least what its
	// size needs, at most 4 MiB.
	const size_t area = (size_t)stream_.width * stream_.height;
	size_t capacity = std::max<size_t>(answer.maxVideoFrameSize, std::max<size_t>(stream_.maxFrameBytes, area * 2));
	capacity = std::min<size_t>(capacity, 4u << 20);
	assembler_.begin(stream_.pixels, stream_.width, stream_.height, capacity);
	if (stream_.pixels == usb::Pixels::Mjpeg)
	{
		std::string e;
		if (!mjpeg_.open(e))
		{
			cameraFailure = "MJPEG: " + e;
			say("camera: %s", cameraFailure.c_str());
			return false;
		}
	}
	if (alt_.bulk)
	{
		videoBytes_ = std::clamp<uint32_t>(answer.maxPayloadTransferSize != 0 ? answer.maxPayloadTransferSize : 16384, 512,
				512 * 1024);
		video_ = cam_->openPipe(alt_.endpoint, videoBytes_, 1, 4, 0);
	}
	else
	{
		if (way_ == Way::Usbd && usbd.GetMaxIsoPacketSize != nullptr)
			say("camera: the library's largest packet for endpoint 0x%02x: %d bytes (the descriptor's: %u)", alt_.endpoint.address,
					usbd.GetMaxIsoPacketSize(camDev_->dev, alt_.endpoint.address), alt_.endpoint.bytesPerInterval());
		// 32 packets a transfer (4 ms at high speed), 8 at full speed (8 ms).
		const uint32_t packets = camDev_->speed == 2 ? 8 : 32;
		videoBytes_ = alt_.endpoint.bytesPerInterval() * packets;
		video_ = cam_->openPipe(alt_.endpoint, videoBytes_, packets, VideoTransfers, 1000);
	}
	if (video_ < 0)
	{
		cameraFailure = format("endpoint 0x%02x not opened (%s)", alt_.endpoint.address, cam_->error.c_str());
		cameraTransport = true;
		say("camera: %s", cameraFailure.c_str());
		return false;
	}
	say("camera: reading endpoint 0x%02x: %d transfers of %u bytes (%s)", alt_.endpoint.address, alt_.bulk ? 4 : VideoTransfers,
			videoBytes_, cam_->opened.c_str());
	for (int s = 0; s < (alt_.bulk ? 4 : VideoTransfers); s++)
		if (!cam_->submit(video_, s, videoBytes_))
		{
			cameraFailure = "a transfer was not started (" + cam_->error + ")";
			cameraTransport = true;
			say("camera: %s", cameraFailure.c_str());
			return false;
		}
	camState_ = Cam::Starting;
	streamStarted_ = now();
	std::lock_guard<std::mutex> g(lock);
	shared.format = usb::pixelsName(stream_.pixels);
	shared.width = stream_.width;
	shared.height = stream_.height;
	shared.why = "the camera is starting";
	return true;
}

void Session::videoDone(const Completion& c)
{
	if (c.cancelled)
		return;
	transfers_++;
	if (!c.ok || (!alt_.bulk && c.packets == 0))
	{
		transferErrors_++;
		failedInRow_++;
		lastStatus_ = c.status;
		if (errorsSaid_ < 3)
			say("camera: transfer %llu: %s (%d), %u packets", (unsigned long long)transfers_, cam_->statusName(c.status),
					c.status, c.packets);
		errorsSaid_++;
		// A transfer that fails as soon as it is sent would be sent again as
		// soon as it fails: the stream is stopped instead.
		if (failedInRow_ >= 20)
		{
			cameraTransport = true;
			cameraStop(format("20 transfers in a row failed (the last: %s)", cam_->statusName(c.status)));
			return;
		}
	}
	else
		failedInRow_ = 0;
	if (alt_.bulk)
	{
		if (c.ok && c.length > 0)
		{
			packets_++;
			if (packetsSaid_ < 3)
				say("camera: payload %u (%u bytes): %s", ++packetsSaid_, c.length, hex(c.data, std::min<uint32_t>(c.length, 16)).c_str());
			if (assembler_.payload(c.data, c.length))
				picture();
		}
	}
	else
	{
		badPackets_ += c.badPackets;
		for (uint32_t k = 0; k < c.packets; k++)
		{
			const uint32_t n = c.lengths[k];
			packets_++;
			if (n == 0)
			{
				emptyPackets_++;
				continue;
			}
			const uint8_t *p = c.data + (size_t)k * c.per;
			if (packetsSaid_ < 3)
				say("camera: packet %u that is not empty (%u bytes): %s", ++packetsSaid_, n, hex(p, std::min<uint32_t>(n, 16)).c_str());
			if (assembler_.payload(p, n))
				picture();
		}
	}
	if (stopping || video_ < 0 || camState_ == Cam::Failed)
		return;
	if (!cam_->submit(video_, c.slot, videoBytes_) && errorsSaid_ < 6)
	{
		errorsSaid_++;
		say("camera: a transfer was not started again: %s", cam_->error.c_str());
	}
}

void Session::picture()
{
	const double t = now();
	Picture& slot = pictures[back];
	const int w = stream_.width, h = stream_.height;
	bool ok;
	if (stream_.pixels == usb::Pixels::Mjpeg)
	{
		std::string e;
		ok = mjpeg_.decode(assembler_.picture(), assembler_.pictureBytes(), slot.luma.data(), w, h, w, e);
		if (!ok)
		{
			decodeFailed_++;
			decodeInRow_++;
			if (decodeFailed_ == 1)
				say("camera: an MJPEG picture not decoded (%zu bytes, begins %s): %s", assembler_.pictureBytes(),
						hex(assembler_.picture(), std::min<size_t>(assembler_.pictureBytes(), 16)).c_str(), e.c_str());
			if (decodeInRow_ == 30)
				cameraStop("30 MJPEG pictures in a row did not decode (" + e + ")");
			return;
		}
		decodeInRow_ = 0;
	}
	else
		ok = usb::lumaFrom(stream_.pixels, assembler_.picture(), assembler_.pictureBytes(), w, h, slot.luma.data(), w);
	if (!ok)
		return;
	slot.width = w;
	slot.height = h;
	slot.stride = w;
	slot.time = t;
	slot.number = ++picturesMade;
	back = middle.exchange(back | Fresh, std::memory_order_acq_rel) & 3;
	rate_.note(t);
	window_.note(t);
	lastPicture_ = t;
	if (firstPicture_ < 0)
	{
		firstPicture_ = t;
		nextLine_ = t + 5;
		windowStart_ = t;
		say("camera: the first picture, %.2f s after the stream began: %zu bytes, %dx%d %s, begins %s", t - streamStarted_,
				assembler_.pictureBytes(), w, h, usb::pixelsName(stream_.pixels),
				hex(assembler_.picture(), std::min<size_t>(assembler_.pictureBytes(), 32)).c_str());
	}
	if (camState_ != Cam::Streaming)
	{
		if (camState_ == Cam::Stalled)
			say("camera: pictures again");
		camState_ = Cam::Streaming;
		setState(State::Streaming, format("the camera sends %dx%d %s through %s", w, h, usb::pixelsName(stream_.pixels),
				wayName(way_)));
	}
}

void Session::cameraLines(double t)
{
	if (t - windowStart_ >= 1.0)
	{
		const double fps = window_.perSecond();
		window_ = usb::Rate();
		windowStart_ = t;
		std::lock_guard<std::mutex> g(lock);
		shared.framesPerSecond = fps;
	}
	if (t < nextLine_)
		return;
	// At 5 s and 30 s after the first picture, then every 10 minutes.
	linesSaid_++;
	nextLine_ = t + (linesSaid_ == 1 ? 25 : 600);
	const usb::Assembler::Counts& k = assembler_.counts();
	say("camera: %.1f pictures a second over %.1f s (%llu), %dx%d %s through %s; dropped %llu (%llu short, %llu with the error bit, "
		"%llu too long, %llu not JPEG; last: %s), %llu MJPEG not decoded; packets %llu (%llu empty, %llu bad headers, %llu with "
		"errors), transfers %llu (%llu failed)",
			rate_.perSecond(), rate_.seconds(), (unsigned long long)k.pictures, stream_.width, stream_.height,
			usb::pixelsName(stream_.pixels), wayName(way_), (unsigned long long)k.dropped, (unsigned long long)k.droppedShort,
			(unsigned long long)k.droppedError, (unsigned long long)k.droppedOverflow, (unsigned long long)k.droppedNotJpeg,
			assembler_.lastDrop().empty() ? "none" : assembler_.lastDrop().c_str(), (unsigned long long)decodeFailed_,
			(unsigned long long)packets_, (unsigned long long)emptyPackets_, (unsigned long long)k.badHeader,
			(unsigned long long)badPackets_, (unsigned long long)transfers_, (unsigned long long)transferErrors_);
}

void Session::cameraNoPicture()
{
	const usb::Assembler::Counts& k = assembler_.counts();
	cameraFailure = format("no picture in %.0f s through %s", FirstPictureWithin, wayName(way_));
	say("camera: %s: transfers %llu (%llu failed, the last %s), packets %llu (%llu empty, %llu with errors), payload "
		"bytes %llu, bad headers %llu, error bits %llu, frame bit changes %llu, end-of-frame bits %llu, dropped %llu (last: %s)",
			cameraFailure.c_str(), (unsigned long long)transfers_, (unsigned long long)transferErrors_,
			transferErrors_ != 0 ? cam_->statusName(lastStatus_) : "none", (unsigned long long)packets_,
			(unsigned long long)emptyPackets_, (unsigned long long)badPackets_, (unsigned long long)k.bytes,
			(unsigned long long)k.badHeader, (unsigned long long)k.errorBit, (unsigned long long)k.fidChanges,
			(unsigned long long)k.endOfFrame, (unsigned long long)k.dropped,
			assembler_.lastDrop().empty() ? "none" : assembler_.lastDrop().c_str());
	cameraTransport = true;
	cameraStop(cameraFailure);
}

// The stream ended (the pictures stop; the buttons go on): its transfers
// closed, its bandwidth given back.
void Session::cameraStop(const std::string& why)
{
	if (camState_ == Cam::Failed)
		return;
	camState_ = Cam::Failed;
	cameraFailure = why;
	if (cam_ != nullptr && video_ >= 0)
		cam_->closePipe(video_);
	video_ = -1;
	if (cam_ != nullptr && altSet_)
	{
		const bool ok = cam_->setAlt(vs_, 0);
		say("camera: back to alternate setting 0 (its bandwidth given back): %s", ok ? "yes" : cam_->error.c_str());
		altSet_ = false;
	}
	setState(State::Failed, "the camera: " + why);
}

bool Session::stillThere()
{
	for (const Listed *d : { &gunDev_, camDev_ })
	{
		if (d == nullptr)
			continue;
		if (d->way == Way::Ugen)
		{
			struct stat st;
			if (stat(d->name.c_str(), &st) != 0)
				return false;
			continue;
		}
		void **list = nullptr;
		const long count = usbdList(&list);
		if (count < 0 || list == nullptr)
			continue;		// the library cannot say: taken as there
		bool there = false;
		for (long i = 0; i < count && !there; i++)
		{
			uint8_t dd[64] = {};
			if (usbd.GetDeviceDescriptor(list[i], dd) < 0)
				continue;
			const uint16_t vid = (uint16_t)(dd[8] | (dd[9] << 8)), pid = (uint16_t)(dd[10] | (dd[11] << 8));
			const int bus = usbd.GetBusNumber != nullptr ? usbd.GetBusNumber(list[i]) : -1;
			const int address = usbd.GetDeviceAddress != nullptr ? usbd.GetDeviceAddress(list[i]) : -1;
			there = vid == d->vid && pid == d->pid && bus == d->bus && address == d->address;
		}
		usbd.FreeDeviceList(list, 1);
		if (!there)
			return false;
	}
	return true;
}

Outcome Session::run(bool mayGiveUp)
{
	double nextCheck = now() + LookEvery;
	for (;;)
	{
		if (stopping)
			return Outcome::Stopped;
		pump(20);
		const double t = now();
		if ((gun_ != nullptr && gun_->gone) || (cam_ != nullptr && cam_->gone))
		{
			say("the gun's USB device is gone");
			return Outcome::Gone;
		}
		if (t >= nextCheck)
		{
			nextCheck = t + LookEvery;
			if (!stillThere())
			{
				say("the gun was pulled out");
				return Outcome::Gone;
			}
		}
		if (camState_ == Cam::Starting && t - streamStarted_ > FirstPictureWithin)
		{
			cameraNoPicture();
			if (mayGiveUp)
				return Outcome::CameraFailed;
		}
		if (camState_ == Cam::Streaming || camState_ == Cam::Stalled)
			cameraLines(t);
		if (camState_ == Cam::Streaming && t - lastPicture_ > 5)
		{
			camState_ = Cam::Stalled;
			say("camera: no picture for 5 s (%llu in all)", (unsigned long long)assembler_.counts().pictures);
			setState(State::Failed, "the camera stopped sending pictures");
		}
	}
}

void Session::close()
{
	if (cam_ != nullptr && video_ >= 0)
		cam_->closePipe(video_);
	video_ = -1;
	if (cam_ != nullptr && altSet_)
	{
		const bool ok = cam_->setAlt(vs_, 0);
		say("camera: back to alternate setting 0 (its bandwidth given back): %s", ok ? "yes" : cam_->error.c_str());
		altSet_ = false;
	}
	mjpeg_.close();
	if (cam_ != nullptr && cam_ != gun_)
	{
		cam_->close();
		delete cam_;
	}
	if (gun_ != nullptr)
	{
		gun_->close();
		delete gun_;
	}
	cam_ = gun_ = nullptr;
}

// ------------------------------------------------------------------ the thread
void sleepFor(double seconds)
{
	const double until = now() + seconds;
	while (!stopping && now() < until)
		usleep(100 * 1000);
}

void clearShared(State state, const std::string& why)
{
	std::lock_guard<std::mutex> g(lock);
	shared = Status();
	shared.state = state;
	shared.why = why;
	sharedButtons = Buttons();
	sharedCalibration = Calibration();
}

// Until the gun is no longer listed (or the thread stops), looking every 2 s.
void waitGone(uint16_t vid, uint16_t pid)
{
	for (;;)
	{
		sleepFor(LookEvery);
		if (stopping)
			return;
		Scan s;
		list(s, false, false);
		bool there = false;
		for (const Listed& d : s.devices)
			there = there || (d.vid == vid && d.pid == pid);
		if (!there)
		{
			say("the gun was pulled out");
			return;
		}
	}
}

// The gun's stages, one way after the other while its camera sends no picture.
void runGun(Scan& scan, const Listed& found)
{
	std::vector<std::pair<Way, const Listed *>> ways;
	for (Way w : { Way::Usbd, Way::Ugen })
	{
		if ((w == Way::Usbd && options.route == 2) || (w == Way::Ugen && options.route == 1))
			continue;
		if (const Listed *g = scan.gun(w))
			ways.push_back({ w, g });
		else
			say("%s: the gun is not in its list: not set up this way", wayName(w));
	}
	for (size_t i = 0; i < ways.size(); i++)
	{
		const bool last = i + 1 == ways.size();
		Session s(ways[i].first, scan, *ways[i].second);
		setState(State::Found, format("setting the gun up through %s", wayName(ways[i].first)));
		if (!s.open())
		{
			say("%s: the gun was not opened%s", wayName(ways[i].first), last ? "" : "; trying the next way");
			continue;
		}
		{
			std::lock_guard<std::mutex> g(lock);
			shared.route = wayName(ways[i].first);
		}
		s.serialStage();
		if (stopping)
			return;
		s.buttonsStage();
		if (stopping)
			return;
		const bool camera = s.cameraStage();
		if (!camera)
		{
			setState(State::Failed, "the camera: " + s.cameraFailure);
			if (s.cameraTransport && !last)
			{
				say("camera: not set up through %s (%s); trying the next way", wayName(ways[i].first), s.cameraFailure.c_str());
				continue;
			}
		}
		const Outcome o = s.run(camera && !last);
		if (o == Outcome::CameraFailed)
		{
			say("camera: nothing through %s; trying the next way", wayName(ways[i].first));
			continue;
		}
		return;
	}
	setState(State::Failed, "the gun could not be set up either way");
	say("the gun could not be set up either way; waiting for it to be pulled out");
	waitGone(found.vid, found.pid);
}

void *thread(void *)
{
	if (tuning::setThread != nullptr)
		tuning::setThread(tuning::Role::Input);
	say("looking for a Sinden Lightgun: %s, %s; the title is %s its sandbox", options.findOnly ? "its descriptors only" : "every stage",
			options.route == 1 ? "libSceUsbd only" : options.route == 2 ? "ugen only" : "libSceUsbd, then ugen",
			platform::usbAvailable() ? "out of" : "in");
	bool first = true, saidNone = false;
	while (!stopping)
	{
		Scan scan;
		list(scan, first);
		first = false;
		const Listed *gun = scan.gun(Way::Usbd);
		if (gun == nullptr)
			gun = scan.gun(Way::Ugen);
		if (gun == nullptr)
		{
			if (!saidNone)
				say("no Sinden Lightgun among the %zu devices listed; looking again every %.0f seconds", scan.devices.size(),
						LookEvery);
			saidNone = true;
			clearShared(State::Absent, "no gun plugged in");
			sleepFor(LookEvery);
			continue;
		}
		saidNone = false;
		say("the gun: %04x:%04x \"%s\" (%s)", gun->vid, gun->pid, gun->product.c_str(), gun->name.c_str());
		clearShared(State::Found, "found");
		{
			std::lock_guard<std::mutex> g(lock);
			shared.vendor = gun->vid;
			shared.product = gun->pid;
		}
		if (options.findOnly)
		{
			setState(State::Found, "find only: its descriptors are in the boot log");
			const uint16_t vid = gun->vid, pid = gun->pid;
			waitGone(vid, pid);
		}
		else
			runGun(scan, *gun);
		clearShared(State::Absent, "no gun plugged in");
	}
	say("the gun's thread stopped");
	finished = true;
	return nullptr;
}

} // namespace

void start(const Options& o)
{
	if (running.exchange(true))
		return;
	if (!finished)
	{
		say("not started: the last thread has not stopped yet");
		running = false;
		return;
	}
	options = o;
	stopping = false;
	finished = false;
	for (Picture& p : pictures)
		if (p.luma.empty())
			p.luma.assign((size_t)MostWidth * MostHeight, 0);
	setState(State::Absent, "looking");
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 512 * 1024);
	const int rc = pthread_create(&t, &attr, thread, nullptr);
	pthread_attr_destroy(&attr);
	if (rc != 0)
	{
		say("no thread (%d)", rc);
		finished = true;
		running = false;
		setState(State::Off, "no thread");
		return;
	}
	pthread_detach(t);
}

void stop()
{
	if (!running)
		return;
	stopping = true;
	const double until = now() + 2.0;
	while (!finished && now() < until)
		usleep(20 * 1000);
	if (!finished)
		say("the gun's thread did not stop within 2 s (in the USB library?): left");
	running = false;
	clearShared(State::Off, "stopped");
}

bool startIfAsked()
{
	std::string word, where;
	if (const char *e = getenv("SWANSTATION_SINDEN"))
	{
		word = e;
		where = "SWANSTATION_SINDEN";
	}
	else
	{
		std::vector<uint8_t> text;
		if (!readFile(rootDir + "data/sinden.txt", text))
			return false;
		word.assign(text.begin(), text.end());
		where = "data/sinden.txt";
	}
	word = lowercase(trim(word));
	const size_t space = word.find_first_of(" \t\r\n");
	if (space != std::string::npos)
		word.resize(space);
	Options o;
	o.findOnly = word == "find";
	o.route = word == "usbd" ? 1 : word == "ugen" ? 2 : 0;
	say("asked for by %s (\"%s\")", where.c_str(), word.c_str());
	start(o);
	return true;
}

bool claims(uint16_t vendor, uint16_t product)
{
	return running && gunProduct(vendor, product);
}

Status status()
{
	std::lock_guard<std::mutex> g(lock);
	return shared;
}

State state()
{
	std::lock_guard<std::mutex> g(lock);
	return shared.state;
}

bool latestFrame(Frame& out)
{
	if ((middle.load(std::memory_order_acquire) & Fresh) == 0)
		return false;
	front = middle.exchange(front, std::memory_order_acq_rel) & 3;
	const Picture& p = pictures[front];
	out.luma = p.luma.data();
	out.width = p.width;
	out.height = p.height;
	out.stride = p.stride;
	out.time = p.time;
	out.number = p.number;
	return true;
}

Buttons buttons()
{
	std::lock_guard<std::mutex> g(lock);
	return sharedButtons;
}

Calibration calibration()
{
	std::lock_guard<std::mutex> g(lock);
	return sharedCalibration;
}

}
