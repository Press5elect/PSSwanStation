/*
	PSSwanStation - USB controllers the console does not take as its own.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The console's controller library (ScePad) reports only Sony's and the
	licensed controllers. A PS3 USB pad, a DualShock 3 on a cable, an Xbox 360
	pad or another USB gamepad gets power from the console and nothing else.
	This thread reads such a controller itself, over USB, the first way that
	works:

	  1. /dev/uhidN: the kernel's HID driver took the device; read() gives its
	     reports, an ioctl its report descriptor.
	  2. /dev/ugenB.A: FreeBSD's generic USB access, when the kernel answers
	     its device-info ioctl (so the device is known before anything is read
	     from it): the gamepad interface's driver lets go, and its interrupt
	     endpoint is read through its own node, or through USB_FS on the
	     control node.
	  3. libSceUsbd, the console's libusb-like library, loaded at run time:
	     only when "USB controllers" is set to look harder, and only a device
	     that is not Sony's and whose configuration has a gamepad interface.

	What is never done: reading a device that could not be identified first.
	PS5SX2 found that reading the endpoints of unknown devices can take a
	DualSense on a cable from the system (until the console is switched off)
	or stall the USB drive a game is read from. Sony's PS4 and PS5
	controllers are always left to the system; keyboards, mice, hubs and
	storage are not touched.

	It follows PS5SX2's ProsperoUsbPad.cpp (swordpdf, GPL-3.0-or-later), and
	decodes the reports with its OrbisUsbPadDecode.h (third_party/
	ps5sx2-usbpad). Nothing of this has been seen to work on a console yet:
	everything it finds and every report of the first seconds goes to the boot
	log as "usbpad:" lines, so a log shows what a console offers and what a
	controller sends.
*/
#include "fe.h"
#include "../tuning.h"

#include "OrbisUsbPadDecode.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
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

namespace fe::platform
{
namespace
{
using namespace orbis_usbpad;

constexpr int ReadTimeoutMs = 250;

std::mutex lock;
Pad state;							// under lock
std::string stateName;				// under lock
bool connected = false;				// under lock
std::atomic<bool> started{ false };
int lookHarder = 0;

std::string hex(const uint8_t *p, size_t n, size_t most = 64)
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

enum class Route { None, Uhid, UgenNode, UgenFs, Usbd };

const char *routeName(Route r)
{
	switch (r)
	{
	case Route::Uhid: return "uhid";
	case Route::UgenNode: return "ugen, endpoint node";
	case Route::UgenFs: return "ugen, USB_FS";
	case Route::Usbd: return "libSceUsbd";
	default: return "none";
	}
}

// libSceUsbd: libusb 1.0's calls under Sony's names (as on the PS4).
struct Usbd
{
	bool tried = false, ok = false;
	int (*Init)() = nullptr;
	long (*GetDeviceList)(void ***list) = nullptr;
	void (*FreeDeviceList)(void **list, int unref) = nullptr;
	int (*GetDeviceDescriptor)(void *dev, void *desc) = nullptr;
	int (*Open)(void *dev, void **handle) = nullptr;
	void (*Close)(void *handle) = nullptr;
	int (*KernelDriverActive)(void *handle, int iface) = nullptr;
	int (*DetachKernelDriver)(void *handle, int iface) = nullptr;
	int (*ClaimInterface)(void *handle, int iface) = nullptr;
	int (*ControlTransfer)(void *handle, uint8_t type, uint8_t req, uint16_t value, uint16_t index, unsigned char *data,
			uint16_t len, unsigned int timeout) = nullptr;
	int (*InterruptTransfer)(void *handle, unsigned char ep, unsigned char *data, int len, int *done,
			unsigned int timeout) = nullptr;
} usbd;

struct Device
{
	Route route = Route::None;
	std::string name, product;
	int ctl = -1, ep = -1;
	void *usbd = nullptr;
	uint16_t vid = 0, pid = 0;
	Target target;
	Format format = Format::None;
	HidLayout layout;
	bool fsOpen = false;
	usb_fs_endpoint fsEp[1] = {};
	std::vector<uint8_t> fsBuffer;
	void *fsPointer = nullptr;
	uint32_t fsLength = 0;
};

void closeDevice(Device& d)
{
	if (d.fsOpen && d.ctl >= 0)
	{
		usb_fs_close c = {};
		ioctl(d.ctl, USB_FS_CLOSE, &c);
		usb_fs_uninit u = {};
		ioctl(d.ctl, USB_FS_UNINIT, &u);
	}
	if (d.ep >= 0)
		close(d.ep);
	if (d.ctl >= 0)
		close(d.ctl);
	if (d.usbd != nullptr && usbd.Close != nullptr)
		usbd.Close(d.usbd);
	d = Device();
}

// Which decoding a device gets. False: not a controller of ours.
bool classify(Device& d, const uint8_t *reportDescriptor, size_t length)
{
	if (SystemOwned(d.vid, d.pid))
	{
		diag::mark("usbpad: %s: %04x:%04x is one of Sony's PS4/PS5 controllers: the system's", d.name.c_str(), d.vid, d.pid);
		return false;
	}
	if (d.target.xinput)
		d.format = Format::XInput;
	else if (d.vid == 0x054C && d.pid == 0x0268)
		d.format = Format::Ds3;
	else
	{
		if (length > 0)
			d.layout = ParseHid(reportDescriptor, length);
		if (d.layout.keyboard && !d.layout.gamepad)
		{
			diag::mark("usbpad: %s: a keyboard or mouse: left alone", d.name.c_str());
			return false;
		}
		if (d.layout.ok && d.layout.gamepad)
			d.format = Format::Hid;
		else if (d.vid == 0x12BA)
			d.format = Format::Ps3Fixed;	// a PS3 controller with no readable descriptor: the layout is known
		else
		{
			diag::mark("usbpad: %s: %04x:%04x \"%s\": no gamepad in its report descriptor (%zu bytes): left alone",
					d.name.c_str(), d.vid, d.pid, d.product.c_str(), length);
			return false;
		}
	}
	return true;
}

void sayTaken(const Device& d)
{
	diag::mark("usbpad: taken: %s by way of %s: %04x:%04x \"%s\", interface %u, endpoint %#x (%u bytes), read as %s",
			d.name.c_str(), routeName(d.route), d.vid, d.pid, d.product.c_str(), d.target.iface, d.target.ep_in,
			d.target.max_packet, FormatName(d.format));
}

// ---- 1. uhid

bool tryUhid(const std::string& node, Device& d)
{
	const std::string path = "/dev/" + node;
	const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
	if (fd < 0)
	{
		diag::mark("usbpad: %s: open: errno %d", path.c_str(), errno);
		return false;
	}
	d = Device();
	d.name = path;
	usb_device_info info = {};
	if (ioctl(fd, USB_GET_DEVICEINFO, &info) == 0)
	{
		d.vid = info.udi_vendorNo;
		d.pid = info.udi_productNo;
		d.product = info.udi_product;
	}
	std::vector<uint8_t> descriptor(4096);
	usb_gen_descriptor gd = {};
	gd.ugd_data = descriptor.data();
	gd.ugd_maxlen = (uint16_t)descriptor.size();
	const int rc = ioctl(fd, USB_GET_REPORT_DESC, &gd);
	const size_t length = rc == 0 ? std::min<size_t>(gd.ugd_actlen, descriptor.size()) : 0;
	diag::mark("usbpad: %s: %04x:%04x \"%s\", report descriptor %s (%zu bytes)", path.c_str(), d.vid, d.pid,
			d.product.c_str(), rc == 0 ? "read" : "not read", length);
	d.target.max_packet = 64;
	if (!classify(d, descriptor.data(), length))
	{
		close(fd);
		d = Device();
		return false;
	}
	d.route = Route::Uhid;
	d.ep = fd;
	sayTaken(d);
	return true;
}

// ---- 2. ugen

bool ugenRequest(int fd, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *data, uint16_t length,
		uint16_t *actual)
{
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
	if (ioctl(fd, USB_DO_REQUEST, &r) != 0)
		return false;
	if (actual != nullptr)
		*actual = (uint16_t)r.ucr_actlen;
	return true;
}

// A DualShock 3 sends nothing until it is told to (feature report 0xf4).
void ds3Enable(Device& d)
{
	uint8_t on[4] = { 0x42, 0x0C, 0x00, 0x00 };
	bool ok = false;
	if (d.ctl >= 0)
		ok = ugenRequest(d.ctl, 0x21, 0x09, 0x03F4, d.target.iface, on, sizeof(on), nullptr);
	else if (d.usbd != nullptr && usbd.ControlTransfer != nullptr)
		ok = usbd.ControlTransfer(d.usbd, 0x21, 0x09, 0x03F4, d.target.iface, on, sizeof(on), 500) >= 0;
	diag::mark("usbpad: DualShock 3: reports turned on: %s", ok ? "yes" : "no");
}

bool ugenFsOpen(Device& d)
{
	usb_fs_init init = {};
	init.pEndpoints = d.fsEp;
	init.ep_index_max = 1;
	if (ioctl(d.ctl, USB_FS_INIT, &init) != 0)
	{
		diag::mark("usbpad: %s: USB_FS_INIT: errno %d", d.name.c_str(), errno);
		return false;
	}
	usb_fs_open op = {};
	op.max_bufsize = std::max<uint32_t>(d.target.max_packet, 64);
	op.max_frames = 1;
	op.ep_index = 0;
	op.ep_no = d.target.ep_in;
	if (ioctl(d.ctl, USB_FS_OPEN, &op) != 0)
	{
		diag::mark("usbpad: %s: USB_FS_OPEN endpoint %#x: errno %d", d.name.c_str(), d.target.ep_in, errno);
		usb_fs_uninit u = {};
		ioctl(d.ctl, USB_FS_UNINIT, &u);
		return false;
	}
	d.fsOpen = true;
	d.fsBuffer.assign(op.max_bufsize, 0);
	return true;
}

bool tryUgen(const std::string& node, Device& d)
{
	const std::string path = "/dev/" + node;
	const int fd = open(path.c_str(), O_RDWR);
	if (fd < 0)
	{
		diag::mark("usbpad: %s: open: errno %d", path.c_str(), errno);
		return false;
	}
	usb_device_info info = {};
	if (ioctl(fd, USB_GET_DEVICEINFO, &info) != 0)
	{
		// Not known, so not read (PS5SX2's lesson).
		diag::mark("usbpad: %s: the kernel does not say what it is (errno %d): left alone", path.c_str(), errno);
		close(fd);
		return false;
	}
	d = Device();
	d.name = path;
	d.vid = info.udi_vendorNo;
	d.pid = info.udi_productNo;
	d.product = info.udi_product;
	if (info.udi_class == 9 || info.udi_class == 8 || SystemOwned(d.vid, d.pid))
	{
		diag::mark("usbpad: %s: %04x:%04x \"%s\", class %u: %s", path.c_str(), d.vid, d.pid, d.product.c_str(),
				info.udi_class, SystemOwned(d.vid, d.pid) ? "Sony's, the system's" : "a hub or storage");
		close(fd);
		d = Device();
		return false;
	}
	std::vector<uint8_t> configuration(4096);
	usb_gen_descriptor gd = {};
	gd.ugd_data = configuration.data();
	gd.ugd_maxlen = (uint16_t)configuration.size();
	gd.ugd_config_index = 0xFF;		// USB_CURRENT_CONFIG_INDEX
	const bool haveConfiguration = ioctl(fd, USB_GET_FULL_DESC, &gd) == 0;
	const size_t configurationLength = haveConfiguration ? std::min<size_t>(gd.ugd_actlen, configuration.size()) : 0;
	d.target = FindTarget(configuration.data(), configurationLength);
	diag::mark("usbpad: %s: %04x:%04x \"%s\", class %u, bus %u address %u, configuration %s (%zu bytes): %s", path.c_str(),
			d.vid, d.pid, d.product.c_str(), info.udi_class, info.udi_bus, info.udi_addr, haveConfiguration ? "read" : "not read",
			configurationLength, d.target.ok ? "a gamepad interface" : "no gamepad interface");
	if (!d.target.ok)
	{
		close(fd);
		d = Device();
		return false;
	}
	std::vector<uint8_t> descriptor(4096);
	uint16_t descriptorLength = 0;
	if (!d.target.xinput)
	{
		const uint16_t want = d.target.report_desc_len != 0 ? d.target.report_desc_len : 1024;
		if (!ugenRequest(fd, 0x81, 0x06, 0x2200, d.target.iface, descriptor.data(), std::min<uint16_t>(want, 4096),
				&descriptorLength))
			diag::mark("usbpad: %s: report descriptor: errno %d", path.c_str(), errno);
	}
	if (!classify(d, descriptor.data(), descriptorLength))
	{
		close(fd);
		d = Device();
		return false;
	}
	d.ctl = fd;
	// The gamepad interface's driver lets go of it; nothing else of the device
	// is touched.
	int iface = d.target.iface;
	if (ioctl(fd, USB_IFACE_DRIVER_ACTIVE, &iface) == 0)
	{
		iface = d.target.iface;
		const int rc = ioctl(fd, USB_IFACE_DRIVER_DETACH, &iface);
		diag::mark("usbpad: %s: interface %u had a driver: let go %s (errno %d)", path.c_str(), d.target.iface,
				rc == 0 ? "done" : "refused", rc == 0 ? 0 : errno);
	}
	iface = d.target.iface;
	ioctl(fd, USB_CLAIM_INTERFACE, &iface);
	char endpoint[64];
	snprintf(endpoint, sizeof(endpoint), "/dev/usb/%u.%u.%u", info.udi_bus, info.udi_addr, d.target.ep_in & 0x0F);
	const int ep = open(endpoint, O_RDONLY | O_NONBLOCK);
	if (ep >= 0)
	{
		int one = 1;
		ioctl(ep, USB_SET_RX_SHORT_XFER, &one);
		d.ep = ep;
		d.route = Route::UgenNode;
	}
	else
	{
		diag::mark("usbpad: %s: open: errno %d; trying USB_FS", endpoint, errno);
		if (!ugenFsOpen(d))
		{
			closeDevice(d);
			return false;
		}
		d.route = Route::UgenFs;
	}
	if (d.format == Format::Ds3)
		ds3Enable(d);
	sayTaken(d);
	return true;
}

// ---- 3. libSceUsbd

template <typename F> void symbol(int module, const char *name, F& fn)
{
	void *address = nullptr;
	if (sceKernelDlsym(module, name, &address) == 0 && address != nullptr)
		fn = reinterpret_cast<F>(address);
	else
		diag::mark("usbpad: libSceUsbd: %s not found", name);
}

bool usbdLoad()
{
	if (usbd.tried)
		return usbd.ok;
	usbd.tried = true;
	// A start that never came back from the library is not repeated.
	const std::string marker = rootDir + "data/usbd-marker.txt";
	if (FILE *f = fopen(marker.c_str(), "rb"))
	{
		char line[16] = {};
		const size_t n = fread(line, 1, sizeof(line) - 1, f);
		fclose(f);
		line[n] = 0;
		if (strncmp(line, "started", 7) == 0)
		{
			diag::mark("usbpad: libSceUsbd: not tried: the last start never came back from it (delete data/usbd-marker.txt to try again)");
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
		diag::mark("usbpad: libSceUsbd: %s: load %#x (start %d)", path.c_str(), (unsigned)module, result);
		if (module >= 0)
			break;
	}
	if (module < 0)
	{
		diag::mark("usbpad: libSceUsbd: not loadable");
		return false;
	}
	symbol(module, "sceUsbdInit", usbd.Init);
	symbol(module, "sceUsbdGetDeviceList", usbd.GetDeviceList);
	symbol(module, "sceUsbdFreeDeviceList", usbd.FreeDeviceList);
	symbol(module, "sceUsbdGetDeviceDescriptor", usbd.GetDeviceDescriptor);
	symbol(module, "sceUsbdOpen", usbd.Open);
	symbol(module, "sceUsbdClose", usbd.Close);
	symbol(module, "sceUsbdKernelDriverActive", usbd.KernelDriverActive);
	symbol(module, "sceUsbdDetachKernelDriver", usbd.DetachKernelDriver);
	symbol(module, "sceUsbdClaimInterface", usbd.ClaimInterface);
	symbol(module, "sceUsbdControlTransfer", usbd.ControlTransfer);
	symbol(module, "sceUsbdInterruptTransfer", usbd.InterruptTransfer);
	if (usbd.Init == nullptr || usbd.GetDeviceList == nullptr || usbd.FreeDeviceList == nullptr
			|| usbd.GetDeviceDescriptor == nullptr || usbd.Open == nullptr || usbd.Close == nullptr
			|| usbd.ControlTransfer == nullptr || usbd.InterruptTransfer == nullptr)
		return false;
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
	diag::mark("usbpad: libSceUsbd: sceUsbdInit %#x", (unsigned)rc);
	usbd.ok = rc >= 0;
	return usbd.ok;
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

bool tryUsbd(Device& out, std::vector<std::string>& seen)
{
	if (!usbdLoad())
		return false;
	void **list = nullptr;
	const long count = usbd.GetDeviceList(&list);
	if (count <= 0 || list == nullptr)
		return false;
	bool taken = false;
	for (long i = 0; i < count && !taken; i++)
	{
		uint8_t dd[64] = {};	// libusb_device_descriptor (18 bytes), with room to spare
		if (usbd.GetDeviceDescriptor(list[i], dd) < 0)
			continue;
		const uint16_t vid = (uint16_t)(dd[8] | (dd[9] << 8)), pid = (uint16_t)(dd[10] | (dd[11] << 8));
		char key[48];
		snprintf(key, sizeof(key), "libSceUsbd %04x:%04x #%ld", vid, pid, i);
		const bool firstTime = std::find(seen.begin(), seen.end(), key) == seen.end();
		// Hubs (class 9), storage (8) and Sony's controllers are never opened.
		if (dd[4] == 9 || dd[4] == 8 || SystemOwned(vid, pid))
			continue;
		if (!firstTime)
			continue;
		seen.push_back(key);
		void *handle = nullptr;
		if (usbd.Open(list[i], &handle) < 0 || handle == nullptr)
		{
			diag::mark("usbpad: %s: could not be opened", key);
			continue;
		}
		Device d;
		d.name = key;
		d.usbd = handle;
		d.vid = vid;
		d.pid = pid;
		d.product = usbdString(handle, dd[15]);
		unsigned char configuration[1024] = {};
		const int length = usbd.ControlTransfer(handle, 0x80, 0x06, 0x0200, 0, configuration, sizeof(configuration), 500);
		d.target = length > 0 ? FindTarget(configuration, (size_t)length) : Target();
		diag::mark("usbpad: %s \"%s\", configuration %d bytes: %s", key, d.product.c_str(), length,
				d.target.ok ? "a gamepad interface" : "no gamepad interface");
		if (!d.target.ok)
		{
			usbd.Close(handle);
			continue;
		}
		unsigned char descriptor[4096] = {};
		int descriptorLength = 0;
		if (!d.target.xinput)
		{
			const uint16_t want = d.target.report_desc_len != 0 ? d.target.report_desc_len : 1024;
			descriptorLength = usbd.ControlTransfer(handle, 0x81, 0x06, 0x2200, d.target.iface, descriptor,
					std::min<uint16_t>(want, 4096), 500);
		}
		if (!classify(d, descriptor, descriptorLength > 0 ? (size_t)descriptorLength : 0))
		{
			usbd.Close(handle);
			continue;
		}
		if (usbd.KernelDriverActive != nullptr && usbd.DetachKernelDriver != nullptr
				&& usbd.KernelDriverActive(handle, d.target.iface) == 1)
			diag::mark("usbpad: libSceUsbd: driver let go: %d", usbd.DetachKernelDriver(handle, d.target.iface));
		if (usbd.ClaimInterface != nullptr)
		{
			const int rc = usbd.ClaimInterface(handle, d.target.iface);
			if (rc < 0)
				diag::mark("usbpad: libSceUsbd: claim interface %u: %d", d.target.iface, rc);
		}
		d.route = Route::Usbd;
		if (d.format == Format::Ds3)
			ds3Enable(d);
		sayTaken(d);
		out = d;
		taken = true;
	}
	usbd.FreeDeviceList(list, 1);
	return taken;
}

// ---- looking

bool scan(Device& d, std::vector<std::string>& seen, bool first)
{
	std::vector<std::string> uhid, ugen;
	std::string all;
	if (DIR *dir = opendir("/dev"))
	{
		while (dirent *e = readdir(dir))
		{
			const std::string n = e->d_name;
			if (n.rfind("uhid", 0) == 0)
				uhid.push_back(n);
			else if (n.rfind("ugen", 0) == 0)
				ugen.push_back(n);
			if (first && (n.find("usb") != std::string::npos || n.find("ugen") != std::string::npos
					|| n.find("hid") != std::string::npos))
				all += " " + n;
		}
		closedir(dir);
	}
	else if (first)
		diag::mark("usbpad: /dev cannot be read (errno %d): the title is in its sandbox?", errno);
	if (first)
		diag::mark("usbpad: /dev's USB nodes:%s", all.empty() ? " none" : all.c_str());
	std::sort(uhid.begin(), uhid.end());
	std::sort(ugen.begin(), ugen.end());
	for (const std::string& n : uhid)
	{
		if (std::find(seen.begin(), seen.end(), n) != seen.end())
			continue;
		seen.push_back(n);
		if (tryUhid(n, d))
			return true;
	}
	for (const std::string& n : ugen)
	{
		if (std::find(seen.begin(), seen.end(), n) != seen.end())
			continue;
		seen.push_back(n);
		if (tryUgen(n, d))
			return true;
	}
	return lookHarder != 0 && tryUsbd(d, seen);
}

// One report: its length, 0 for nothing within the timeout, -1 for a device
// that is gone.
int readReport(Device& d, uint8_t *buffer, size_t capacity)
{
	if (d.route == Route::Uhid || d.route == Route::UgenNode)
	{
		pollfd p = { d.ep, POLLIN, 0 };
		const int pr = poll(&p, 1, ReadTimeoutMs);
		if (pr == 0)
			return 0;
		if (pr < 0)
			return errno == EINTR ? 0 : -1;
		if ((p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
			return -1;
		const ssize_t n = read(d.ep, buffer, capacity);
		if (n < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
		return (int)n;
	}
	if (d.route == Route::UgenFs)
	{
		d.fsPointer = d.fsBuffer.data();
		d.fsLength = (uint32_t)d.fsBuffer.size();
		d.fsEp[0].ppBuffer = &d.fsPointer;
		d.fsEp[0].pLength = &d.fsLength;
		d.fsEp[0].nFrames = 1;
		d.fsEp[0].aFrames = 0;
		d.fsEp[0].flags = USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK;
		d.fsEp[0].timeout = ReadTimeoutMs;
		d.fsEp[0].status = 0;
		usb_fs_start st = {};
		if (ioctl(d.ctl, USB_FS_START, &st) != 0)
			return -1;
		for (int tries = 0; tries < 4; tries++)
		{
			pollfd p = { d.ctl, POLLIN | POLLOUT, 0 };
			poll(&p, 1, ReadTimeoutMs + 50);
			usb_fs_complete c = {};
			if (ioctl(d.ctl, USB_FS_COMPLETE, &c) == 0)
			{
				if (d.fsEp[0].status == 0)
				{
					const size_t n = std::min<size_t>(d.fsLength, capacity);
					memcpy(buffer, d.fsBuffer.data(), n);
					return (int)n;
				}
				return d.fsEp[0].status == 20 ? 0 : -1;	// USB_ERR_TIMEOUT
			}
			if (errno != EBUSY)
				return -1;
		}
		usb_fs_stop sp = {};
		ioctl(d.ctl, USB_FS_STOP, &sp);
		return 0;
	}
	if (d.route == Route::Usbd)
	{
		int done = 0;
		const int rc = usbd.InterruptTransfer(d.usbd, d.target.ep_in, buffer,
				(int)std::min<size_t>(capacity, d.target.max_packet != 0 ? d.target.max_packet : 64), &done, ReadTimeoutMs);
		if (rc == 0)
			return done;
		return rc == -7 ? 0 : -1;	// LIBUSB_ERROR_TIMEOUT
	}
	return -1;
}

bool decode(const Device& d, const uint8_t *r, size_t n, Common& c)
{
	switch (d.format)
	{
	case Format::Hid: return DecodeHid(d.layout, r, n, c);
	case Format::Ps3Fixed: return DecodePs3Fixed(r, n, c);
	case Format::Ds3: return DecodeDs3(r, n, c);
	case Format::XInput: return DecodeXInput(r, n, c);
	default: return false;
	}
}

// The controller as a DualSense would be: its Select and Start are the touch
// pad's halves (the PlayStation's Select and Start), its PS or Guide button
// is OPTIONS (PSSwanStation's menu).
Pad toPad(const Common& c)
{
	Pad p;
	p.connected = true;
	const uint16_t b = c.buttons;
	const struct
	{
		uint16_t from;
		uint32_t to;
	} map[] = {
		{ B_CROSS, Cross }, { B_CIRCLE, Circle }, { B_SQUARE, Square }, { B_TRIANGLE, Triangle }, { B_L1, L1 }, { B_R1, R1 },
		{ B_L2, L2 }, { B_R2, R2 }, { B_L3, L3 }, { B_R3, R3 }, { B_SELECT, TouchLeft }, { B_START, TouchRight },
		{ B_PS, Options },
	};
	for (const auto& entry : map)
		if ((b & entry.from) != 0)
			p.buttons |= entry.to;
	switch (c.hat)
	{
	case 0: p.buttons |= Up; break;
	case 1: p.buttons |= Up | Right; break;
	case 2: p.buttons |= Right; break;
	case 3: p.buttons |= Down | Right; break;
	case 4: p.buttons |= Down; break;
	case 5: p.buttons |= Down | Left; break;
	case 6: p.buttons |= Left; break;
	case 7: p.buttons |= Up | Left; break;
	default: break;
	}
	p.lx = (c.lx - 128) / 127.5f;
	p.ly = (c.ly - 128) / 127.5f;
	p.rx = (c.rx - 128) / 127.5f;
	p.ry = (c.ry - 128) / 127.5f;
	p.l2 = c.l2 / 255.f;
	p.r2 = c.r2 / 255.f;
	return p;
}

void *thread(void *)
{
	tuning::setThread(tuning::Role::Input);
	Device d;
	std::vector<std::string> seen;
	bool first = true;
	unsigned reports = 0, changesSaid = 0, rounds = 0;
	std::vector<uint8_t> last;
	for (;;)
	{
		if (d.route == Route::None)
		{
			const bool found = scan(d, seen, first);
			if (first && !found)
				diag::mark("usbpad: no USB controller found; looking again every 2 seconds");
			first = false;
			if (!found)
			{
				// A device plugged in later has a new node; the ones looked at
				// are looked at again now and then (a re-plug at the same place).
				if (++rounds % 15 == 0)
					seen.clear();
				sleep(2);
				continue;
			}
			reports = changesSaid = 0;
			last.clear();
			std::lock_guard<std::mutex> guard(lock);
			stateName = d.product.empty() ? std::string("USB controller") : d.product;
			diag::notify("USB controller: %s", stateName.c_str());
		}
		uint8_t buffer[1024];
		const int n = readReport(d, buffer, sizeof(buffer));
		if (n < 0)
		{
			diag::mark("usbpad: %s: read failed (errno %d) after %u reports: gone", d.name.c_str(), errno, reports);
			{
				std::lock_guard<std::mutex> guard(lock);
				connected = false;
				state = Pad();
			}
			closeDevice(d);
			seen.clear();
			diag::notify("USB controller disconnected");
			sleep(1);
			continue;
		}
		if (n == 0)
			continue;
		reports++;
		// The first reports, then each change (the first 100), so that a log
		// shows what each press sends.
		const bool changed = last.size() != (size_t)n || memcmp(last.data(), buffer, (size_t)n) != 0;
		if (reports <= 4 || (changed && changesSaid < 100))
		{
			diag::mark("usbpad: report %u (%d bytes): %s", reports, n, hex(buffer, (size_t)n).c_str());
			if (changed)
				changesSaid++;
		}
		if (changed)
			last.assign(buffer, buffer + n);
		Common c;
		if (!decode(d, buffer, (size_t)n, c))
			continue;
		std::lock_guard<std::mutex> guard(lock);
		state = toPad(c);
		connected = true;
	}
	return nullptr;
}
} // namespace

void usbPadStart(int mode)
{
	if (mode <= 0 || started.exchange(true))
		return;
	lookHarder = mode >= 2 ? 1 : 0;
	diag::mark("usbpad: looking for USB controllers%s", lookHarder ? ", through libSceUsbd too" : "");
	pthread_t t;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	const int rc = pthread_create(&t, &attr, thread, nullptr);
	pthread_attr_destroy(&attr);
	if (rc != 0)
		diag::mark("usbpad: no thread (%d)", rc);
	else
		pthread_detach(t);
}

bool usbPad(Pad& out, std::string *name)
{
	std::lock_guard<std::mutex> guard(lock);
	if (!connected)
		return false;
	out = state;
	if (name != nullptr)
		*name = stateName;
	return true;
}

}
