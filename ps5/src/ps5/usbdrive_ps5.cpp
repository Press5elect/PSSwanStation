/*
	PSSwanStation - a USB disc drive the console did not take as a drive.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	A USB CD, DVD or Blu-ray drive the console attaches as a disc drive is
	/dev/cdN and read through cd(4) and CAM (drive_ps5.cpp). One the console
	does not attach (build 21's tests: an ASUS BW-16D1X-U, listed by the
	console only as a generic USB device, ugenB.A) has no cd node and no pass
	device: nothing the console offers reads it. This file reads such a drive
	itself, over USB, as the console's own driver would: the USB mass storage
	bulk-only transport (a 31-byte command block out, the data, a 13-byte
	status block in), carrying the same SCSI commands drive_ps5.cpp sends
	(READ TOC, READ CD, READ(10), SET CD SPEED, PREVENT ALLOW MEDIUM REMOVAL,
	START STOP UNIT).

	The USB access is FreeBSD's ugen(4): the device's control node
	/dev/ugenB.A for its descriptors and control requests, and its bulk
	endpoints through their nodes /dev/usb/B.A.E, or through the USB_FS
	requests on the control node when the endpoint nodes cannot be opened.
	Which of those the console's kernel allows a title is not known until a
	console runs it (its ugen refuses USB_GET_DEVICEINFO, the first request
	PSSwanStation's USB controller reader makes): every step is tried in turn
	and says in the boot log ("usb drive:" lines) what the console answered.

	What is never done, as PS5SX2 found reading unknown USB devices can take a
	controller from the system or stall a USB drive: a device is read only
	once its descriptors say it is a SCSI mass storage device on the bulk-only
	transport, and only when the console has no driver on it. When the console
	will not say whether it has a driver, a device is taken only when it can
	be told apart from the drives the console did attach (by their names) and
	there is at least one more mass storage device on the bus than the console
	attached; otherwise it is left alone and the boot log says so.
*/
#include "../discdrive_io.h"
#include "../fe.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <set>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <vector>

#include <dev/usb/usb.h>
#include <dev/usb/usb_endian.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usbdi.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_pass.h>

namespace fe::discdrive::usb
{
namespace
{

constexpr uint32_t ChunkBytes = 65536;		// the most one data phase carries (ugen's bulk buffer)
constexpr int CommandTimeoutMs = 10000;		// a seek of a slow drive
constexpr int ControlTimeoutMs = 2000;

std::string trim(std::string s)
{
	while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
		s.pop_back();
	while (!s.empty() && s.front() == ' ')
		s.erase(s.begin());
	return s;
}

std::string ascii(const void *field, size_t size)
{
	const char *c = static_cast<const char *>(field);
	std::string out;
	for (size_t i = 0; i < size && c[i] != '\0'; i++)
		out += (c[i] >= 0x20 && c[i] < 0x7F) ? c[i] : '?';
	return trim(out);
}

std::string upper(std::string s)
{
	for (char& c : s)
		c = (char)std::toupper((unsigned char)c);
	return s;
}

// The words of a name, four letters or more, for telling devices apart.
std::vector<std::string> words(const std::string& text)
{
	std::vector<std::string> out;
	std::string word;
	for (char c : upper(text) + " ")
	{
		if (std::isalnum((unsigned char)c))
			word += c;
		else
		{
			if (word.size() >= 4)
				out.push_back(word);
			word.clear();
		}
	}
	return out;
}

bool shareWord(const std::string& a, const std::string& b)
{
	const std::vector<std::string> wa = words(a), wb = words(b);
	for (const std::string& x : wa)
		if (std::find(wb.begin(), wb.end(), x) != wb.end())
			return true;
	return false;
}

// ------------------------------------------------- what the console attached

struct Attached
{
	std::string periph;		// da0, cd1
	std::string name;		// "SanDisk Cruzer Blade"
};

// The devices the console's own USB storage driver attached, from their pass
// devices: a drive on umass-sim's bus is one the console reads itself.
std::vector<Attached> attachedByConsole()
{
	std::vector<Attached> out;
	DIR *dir = opendir("/dev");
	if (dir == nullptr)
		return out;
	std::vector<std::string> passes;
	while (const dirent *entry = readdir(dir))
		if (std::strncmp(entry->d_name, "pass", 4) == 0 && std::isdigit((unsigned char)entry->d_name[4]))
			passes.push_back(entry->d_name);
	closedir(dir);
	for (const std::string& pass : passes)
	{
		const int fd = ::open(("/dev/" + pass).c_str(), O_RDWR);
		if (fd < 0)
			continue;
		union ccb ccb;
		memset(&ccb, 0, sizeof(ccb));
		ccb.ccb_h.func_code = XPT_GDEVLIST;
		if (ioctl(fd, CAMGETPASSTHRU, &ccb) != 0)
		{
			::close(fd);
			continue;
		}
		const path_id_t path = ccb.ccb_h.path_id;
		const target_id_t target = ccb.ccb_h.target_id;
		const lun_id_t lun = ccb.ccb_h.target_lun;
		memset(&ccb, 0, sizeof(ccb));
		ccb.ccb_h.func_code = XPT_PATH_INQ;
		ccb.ccb_h.path_id = path;
		ccb.ccb_h.target_id = target;
		ccb.ccb_h.target_lun = lun;
		bool onUsb = false;
		if (ioctl(fd, CAMIOCOMMAND, &ccb) == 0 && (ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP)
			onUsb = ccb.cpi.transport == XPORT_USB || ascii(ccb.cpi.dev_name, DEV_IDLEN).rfind("umass", 0) == 0;
		if (onUsb)
		{
			Attached a;
			memset(&ccb, 0, sizeof(ccb));
			ccb.ccb_h.func_code = XPT_GDEV_TYPE;
			ccb.ccb_h.path_id = path;
			ccb.ccb_h.target_id = target;
			ccb.ccb_h.target_lun = lun;
			if (ioctl(fd, CAMIOCOMMAND, &ccb) == 0 && (ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP)
				a.name = trim(ascii(ccb.cgd.inq_data.vendor, SID_VENDOR_SIZE) + " "
						+ ascii(ccb.cgd.inq_data.product, SID_PRODUCT_SIZE));
			// Which device it is: the pass device's own list entry names its peer.
			memset(&ccb, 0, sizeof(ccb));
			ccb.ccb_h.func_code = XPT_GDEVLIST;
			ccb.ccb_h.path_id = path;
			ccb.ccb_h.target_id = target;
			ccb.ccb_h.target_lun = lun;
			a.periph = pass;
			for (int i = 0; i < 8 && ioctl(fd, CAMIOCOMMAND, &ccb) == 0 && ccb.cgdl.status != CAM_GDEVLIST_ERROR; i++)
			{
				if (std::strncmp(ccb.cgdl.periph_name, "pass", 4) != 0 && ccb.cgdl.periph_name[0] != '\0')
				{
					a.periph = ascii(ccb.cgdl.periph_name, DEV_IDLEN) + std::to_string(ccb.cgdl.unit_number);
					break;
				}
				if (ccb.cgdl.status != CAM_GDEVLIST_MORE_DEVS)
					break;
			}
			out.push_back(a);
		}
		::close(fd);
	}
	return out;
}

// --------------------------------------------------------- the USB device

struct Found
{
	std::string node;		// ugen2.3
	int bus = -1, addr = -1;
	uint16_t vid = 0, pid = 0;
	std::string maker, product;
	bool massStorage = false;	// a SCSI mass storage interface on the bulk-only transport
	bool uasOnly = false;		// only USB Attached SCSI, which this cannot drive
	int iface = -1, altBot = -1;
	uint8_t epIn = 0, epOut = 0;
	uint16_t packetIn = 512, packetOut = 512;
	int driver = -1;		// 1 the console has a driver on it, 0 none, -1 it would not say
	std::string driverName;
	std::string descriptorsBy;	// how the descriptors were read
	std::string why;		// why it is left alone
};

bool getDeviceDescriptor(int fd, const std::string& node, usb_device_descriptor& dd, std::string& by)
{
	if (ioctl(fd, USB_GET_DEVICE_DESC, &dd) == 0)
	{
		by = "USB_GET_DEVICE_DESC";
		return true;
	}
	const int first = errno;
	// A control request, GET DESCRIPTOR (device): what enumeration itself asks.
	usb_ctl_request req = {};
	memset(&dd, 0, sizeof(dd));
	req.ucr_data = &dd;
	req.ucr_request.bmRequestType = 0x80;
	req.ucr_request.bRequest = 0x06;
	USETW(req.ucr_request.wValue, 0x0100);
	USETW(req.ucr_request.wIndex, 0);
	USETW(req.ucr_request.wLength, sizeof(dd));
	if (ioctl(fd, USB_DO_REQUEST, &req) == 0 && req.ucr_actlen >= 8)
	{
		by = "USB_DO_REQUEST";
		return true;
	}
	diag::mark("usb drive: %s: the device descriptor could not be read (USB_GET_DEVICE_DESC: %s; USB_DO_REQUEST: %s)",
			node.c_str(), strerror(first), strerror(errno));
	return false;
}

bool getConfiguration(int fd, std::vector<uint8_t>& out, std::string& by)
{
	out.assign(4096, 0);
	usb_gen_descriptor gd = {};
	gd.ugd_data = out.data();
	gd.ugd_maxlen = (uint16_t)out.size();
	gd.ugd_config_index = 0xFF;		// the current configuration
	if (ioctl(fd, USB_GET_FULL_DESC, &gd) == 0 && gd.ugd_actlen >= 9)
	{
		out.resize(gd.ugd_actlen);
		by += "+USB_GET_FULL_DESC";
		return true;
	}
	const int first = errno;
	usb_ctl_request req = {};
	req.ucr_data = out.data();
	req.ucr_request.bmRequestType = 0x80;
	req.ucr_request.bRequest = 0x06;
	USETW(req.ucr_request.wValue, 0x0200);		// configuration 0
	USETW(req.ucr_request.wIndex, 0);
	USETW(req.ucr_request.wLength, (uint16_t)out.size());
	if (ioctl(fd, USB_DO_REQUEST, &req) == 0 && req.ucr_actlen >= 9)
	{
		out.resize(req.ucr_actlen);
		by += "+USB_DO_REQUEST";
		return true;
	}
	diag::mark("usb drive: the configuration could not be read (USB_GET_FULL_DESC: %s; USB_DO_REQUEST: %s)",
			strerror(first), strerror(errno));
	out.clear();
	return false;
}

std::string getString(int fd, uint8_t index)
{
	if (index == 0)
		return "";
	std::vector<uint8_t> buffer(256, 0);
	usb_gen_descriptor gd = {};
	gd.ugd_data = buffer.data();
	gd.ugd_maxlen = (uint16_t)buffer.size();
	gd.ugd_lang_id = 0x0409;
	gd.ugd_string_index = index;
	size_t length = 0;
	if (ioctl(fd, USB_GET_STRING_DESC, &gd) == 0)
		length = gd.ugd_actlen;
	else
	{
		usb_ctl_request req = {};
		req.ucr_data = buffer.data();
		req.ucr_request.bmRequestType = 0x80;
		req.ucr_request.bRequest = 0x06;
		USETW(req.ucr_request.wValue, 0x0300 | index);
		USETW(req.ucr_request.wIndex, 0x0409);
		USETW(req.ucr_request.wLength, (uint16_t)buffer.size());
		if (ioctl(fd, USB_DO_REQUEST, &req) == 0)
			length = req.ucr_actlen;
	}
	// A string descriptor: length, type 3, UTF-16LE.
	std::string s;
	if (length >= 2 && buffer[1] == 0x03)
	{
		const size_t n = std::min<size_t>(buffer[0], length);
		for (size_t i = 2; i + 1 < n; i += 2)
		{
			const uint16_t c = (uint16_t)(buffer[i] | (buffer[i + 1] << 8));
			s += (c >= 0x20 && c < 0x7F) ? (char)c : '?';
		}
	}
	return trim(s);
}

// The mass storage interface in a configuration descriptor, with its bulk
// endpoints: SCSI transparent command set (6) on the bulk-only transport
// (0x50). A device offering only USB Attached SCSI (0x62) is noted.
void findInterface(const std::vector<uint8_t>& c, Found& f)
{
	int iface = -1, alt = -1;
	uint8_t cls = 0, sub = 0, proto = 0;
	bool inThis = false;
	size_t i = 0;
	while (i + 2 <= c.size() && c[i] >= 2 && i + c[i] <= c.size())
	{
		const uint8_t length = c[i], type = c[i + 1];
		if (type == 0x04 && length >= 9)
		{
			iface = c[i + 2];
			alt = c[i + 3];
			cls = c[i + 5];
			sub = c[i + 6];
			proto = c[i + 7];
			inThis = false;
			if (cls == 0x08)
			{
				if (sub == 0x06 && proto == 0x50 && !f.massStorage)
				{
					f.massStorage = true;
					f.iface = iface;
					f.altBot = alt;
					f.epIn = f.epOut = 0;
					inThis = true;
				}
				else if (proto == 0x62)
					f.uasOnly = !f.massStorage;
			}
		}
		else if (type == 0x05 && length >= 7 && inThis && (c[i + 3] & 0x03) == 0x02)
		{
			const uint8_t address = c[i + 2];
			const uint16_t packet = (uint16_t)(c[i + 4] | (c[i + 5] << 8));
			if ((address & 0x80) != 0 && f.epIn == 0)
			{
				f.epIn = address;
				f.packetIn = packet != 0 ? packet : 512;
			}
			else if ((address & 0x80) == 0 && f.epOut == 0)
			{
				f.epOut = address;
				f.packetOut = packet != 0 ? packet : 512;
			}
		}
		i += length;
	}
	if (f.massStorage && (f.epIn == 0 || f.epOut == 0))
	{
		f.massStorage = false;
		f.why = "its mass storage interface has no bulk endpoints";
	}
	if (f.massStorage)
		f.uasOnly = false;
}

// ----------------------------------------------------------- the transport

// Bulk transfers to the drive: its endpoint nodes, else USB_FS on the
// control node.
class Transport
{
public:
	Transport(int ctl, const Found& f) : ctl(ctl), f(f) {}
	~Transport()
	{
		if (fsOpen)
		{
			for (uint8_t i = 0; i < 2; i++)
			{
				usb_fs_close c = {};
				c.ep_index = i;
				ioctl(ctl, USB_FS_CLOSE, &c);
			}
			usb_fs_uninit u = {};
			ioctl(ctl, USB_FS_UNINIT, &u);
		}
		if (in >= 0)
			::close(in);
		if (out >= 0)
			::close(out);
	}

	bool open(std::string& how)
	{
		char path[64];
		snprintf(path, sizeof(path), "/dev/usb/%d.%d.%u", f.bus, f.addr, f.epIn & 0x0F);
		in = ::open(path, O_RDONLY | O_NONBLOCK);
		const int inErrno = errno;
		snprintf(path, sizeof(path), "/dev/usb/%d.%d.%u", f.bus, f.addr, f.epOut & 0x0F);
		out = in >= 0 ? ::open(path, O_WRONLY) : -1;
		const int outErrno = errno;
		if (in >= 0 && out >= 0)
		{
			int one = 1;
			ioctl(in, USB_SET_RX_SHORT_XFER, &one);
			int size = (int)ChunkBytes;
			ioctl(in, USB_SET_RX_BUFFER_SIZE, &size);
			ioctl(out, USB_SET_TX_BUFFER_SIZE, &size);
			how = "endpoint nodes";
			return true;
		}
		std::string nodes = std::string("endpoint nodes: ") + strerror(in < 0 ? inErrno : outErrno);
		if (in >= 0)
			::close(in);
		in = out = -1;
		// USB_FS: endpoint index 0 out, 1 in.
		memset(fsEp, 0, sizeof(fsEp));
		usb_fs_init init = {};
		init.pEndpoints = fsEp;
		init.ep_index_max = 2;
		if (ioctl(ctl, USB_FS_INIT, &init) != 0)
		{
			how = nodes + "; USB_FS_INIT: " + strerror(errno);
			return false;
		}
		for (uint8_t i = 0; i < 2; i++)
		{
			usb_fs_open op = {};
			op.max_bufsize = ChunkBytes;
			op.max_frames = 1;
			op.ep_index = i;
			op.ep_no = i == 0 ? f.epOut : f.epIn;
			if (ioctl(ctl, USB_FS_OPEN, &op) != 0)
			{
				how = nodes + "; USB_FS_OPEN endpoint " + std::to_string((unsigned)op.ep_no) + ": " + strerror(errno);
				usb_fs_uninit u = {};
				ioctl(ctl, USB_FS_UNINIT, &u);
				return false;
			}
		}
		fsOpen = true;
		how = "USB_FS";
		return true;
	}

	// A control request to the device (the console's ugen does it on the
	// control node).
	bool control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *data, uint16_t length,
			uint16_t *actual = nullptr)
	{
		usb_ctl_request req = {};
		req.ucr_data = data;
		req.ucr_request.bmRequestType = type;
		req.ucr_request.bRequest = request;
		USETW(req.ucr_request.wValue, value);
		USETW(req.ucr_request.wIndex, index);
		USETW(req.ucr_request.wLength, length);
		req.ucr_flags = USB_SHORT_XFER_OK;
		if (ioctl(ctl, USB_DO_REQUEST, &req) != 0)
		{
			lastErrno = errno;
			return false;
		}
		if (actual != nullptr)
			*actual = req.ucr_actlen;
		return true;
	}

	bool clearHalt(uint8_t endpoint)
	{
		if (fsOpen)
		{
			usb_fs_clear_stall_sync cs = {};
			cs.ep_index = (endpoint & 0x80) != 0 ? 1 : 0;
			if (ioctl(ctl, USB_FS_CLEAR_STALL_SYNC, &cs) == 0)
				return true;
		}
		return control(0x02, 0x01, 0x0000, endpoint, nullptr, 0);		// CLEAR FEATURE, ENDPOINT HALT
	}

	bool bulkOut(const uint8_t *data, uint32_t length)
	{
		if (!fsOpen)
		{
			uint32_t done = 0;
			while (done < length)
			{
				const ssize_t n = ::write(out, data + done, length - done);
				if (n < 0)
				{
					if (errno == EINTR)
						continue;
					lastErrno = errno;
					return false;
				}
				done += (uint32_t)n;
			}
			return true;
		}
		return fsTransfer(0, const_cast<uint8_t *>(data), length, CommandTimeoutMs) == (int)length;
	}

	// Up to `length` bytes in; the count, or -1.
	int bulkIn(uint8_t *data, uint32_t length, int timeoutMs)
	{
		if (!fsOpen)
		{
			uint32_t done = 0;
			while (done < length)
			{
				pollfd p = { in, POLLIN, 0 };
				const int pr = poll(&p, 1, timeoutMs);
				if (pr == 0)
				{
					lastErrno = ETIMEDOUT;
					return done != 0 ? (int)done : -1;
				}
				if (pr < 0)
				{
					if (errno == EINTR)
						continue;
					lastErrno = errno;
					return -1;
				}
				const ssize_t n = ::read(in, data + done, length - done);
				if (n < 0)
				{
					if (errno == EINTR || errno == EAGAIN)
						continue;
					lastErrno = errno;
					return -1;
				}
				if (n == 0)
					break;
				done += (uint32_t)n;
				// A short packet ends a bulk transfer.
				if ((uint32_t)n % f.packetIn != 0)
					break;
			}
			return (int)done;
		}
		return fsTransfer(1, data, length, timeoutMs);
	}

	int lastErrno = 0;

private:
	int ctl;
	Found f;
	int in = -1, out = -1;
	bool fsOpen = false;
	usb_fs_endpoint fsEp[2];

	// One USB_FS transfer on endpoint index `index`: the bytes moved, or -1.
	int fsTransfer(uint8_t index, uint8_t *data, uint32_t length, int timeoutMs)
	{
		void *pointer = data;
		uint32_t moved = length;
		fsEp[index].ppBuffer = &pointer;
		fsEp[index].pLength = &moved;
		fsEp[index].nFrames = 1;
		fsEp[index].aFrames = 0;
		fsEp[index].flags = index == 1 ? (USB_FS_FLAG_SINGLE_SHORT_OK | USB_FS_FLAG_MULTI_SHORT_OK) : 0;
		fsEp[index].timeout = (uint16_t)std::min(timeoutMs, 65000);
		fsEp[index].status = 0;
		usb_fs_start st = {};
		st.ep_index = index;
		if (ioctl(ctl, USB_FS_START, &st) != 0)
		{
			lastErrno = errno;
			return -1;
		}
		for (int tries = 0; tries < 64; tries++)
		{
			pollfd p = { ctl, POLLIN | POLLOUT, 0 };
			poll(&p, 1, timeoutMs + 50);
			usb_fs_complete c = {};
			if (ioctl(ctl, USB_FS_COMPLETE, &c) == 0)
			{
				if (c.ep_index != index)
					continue;
				if (fsEp[index].status == 0)
					return (int)moved;
				lastErrno = fsEp[index].status == USB_ERR_TIMEOUT ? ETIMEDOUT : EIO;
				return -1;
			}
			if (errno != EBUSY)
			{
				lastErrno = errno;
				return -1;
			}
		}
		usb_fs_stop sp = {};
		sp.ep_index = index;
		ioctl(ctl, USB_FS_STOP, &sp);
		lastErrno = ETIMEDOUT;
		return -1;
	}
};

// -------------------------------------------------------------- the drive

class BotDrive final : public Drive
{
public:
	BotDrive(int ctl, const Found& f, std::unique_ptr<Transport> t, std::string how)
		: ctl(ctl), f(f), transport(std::move(t)), how(std::move(how))
	{
		device = "/dev/" + f.node;
	}
	~BotDrive() override
	{
		transport.reset();
		if (ctl >= 0)
			::close(ctl);
	}

	bool readToc(std::vector<TocEntry>& out, std::string& error) override
	{
		out.clear();
		// TEST UNIT READY: an empty drive says so by its sense.
		uint8_t tur[6] = {};
		if (!command(tur, 6, nullptr, 0, true, false))
		{
			if (senseKey == 0x02 && senseAsc == 0x3A)
				error = "no disc in the drive";
			else if (senseKey == 0x02)
				error = "the drive is not ready (sense " + senseText() + ")";
			else
				error = "the drive did not answer (" + lastFailure + ")";
			return false;
		}
		std::vector<uint8_t> toc(4 + 8 * 101, 0);
		uint8_t cdb[10] = {};
		cdb[0] = 0x43;					// READ TOC
		cdb[6] = 0;						// from track 0
		cdb[7] = (uint8_t)(toc.size() >> 8);
		cdb[8] = (uint8_t)toc.size();
		if (!command(cdb, 10, toc.data(), (uint32_t)toc.size(), true))
		{
			// No table of contents: one data track, sized by the capacity (a
			// DVD, or a drive that keeps the TOC to itself).
			uint8_t capacity[8] = {};
			uint8_t rc[10] = {};
			rc[0] = 0x25;				// READ CAPACITY
			if (command(rc, 10, capacity, 8, true))
			{
				const uint32_t last = (uint32_t)((capacity[0] << 24) | (capacity[1] << 16) | (capacity[2] << 8) | capacity[3]);
				TocEntry track;
				track.track = 1;
				track.control = 0x04;
				track.lba = 0;
				TocEntry end;
				end.track = 0xAA;
				end.lba = last + 1;
				out.push_back(track);
				out.push_back(end);
				if (!toldToc)
				{
					toldToc = true;
					diag::mark("usb drive: %s: no table of contents (%s); %u sectors by the capacity, as one data track",
							device.c_str(), lastFailure.c_str(), end.lba);
				}
				return true;
			}
			error = "no disc the drive can read (table of contents: " + lastFailure + ")";
			return false;
		}
		const size_t length = std::min<size_t>(toc.size(), 2 + (size_t)((toc[0] << 8) | toc[1]));
		for (size_t i = 4; i + 8 <= length; i += 8)
		{
			TocEntry e;
			e.adr = (uint8_t)(toc[i + 1] >> 4);
			e.control = (uint8_t)(toc[i + 1] & 0x0F);
			e.track = toc[i + 2];
			e.lba = (uint32_t)((toc[i + 4] << 24) | (toc[i + 5] << 16) | (toc[i + 6] << 8) | toc[i + 7]);
			if (e.track == 0xAA || (e.track >= 1 && e.track <= 99))
				out.push_back(e);
		}
		if (out.size() < 2 || out.back().track != 0xAA)
		{
			error = "the table of contents has no end";
			return false;
		}
		return true;
	}

	bool readCd(uint32_t lba, uint32_t count, Sub sub, uint8_t *out, bool audio = false) override
	{
		const uint32_t each = RawSector + subSize(sub);
		uint32_t done = 0;
		while (done < count)
		{
			const uint32_t piece = std::min(count - done, std::max<uint32_t>(1, maxTransfer / each));
			uint8_t cdb[12] = {};
			const uint32_t at = lba + done;
			cdb[0] = 0xBE;				// READ CD
			cdb[1] = audio ? 0x04 : 0x00;
			cdb[2] = (uint8_t)(at >> 24);
			cdb[3] = (uint8_t)(at >> 16);
			cdb[4] = (uint8_t)(at >> 8);
			cdb[5] = (uint8_t)at;
			cdb[6] = (uint8_t)(piece >> 16);
			cdb[7] = (uint8_t)(piece >> 8);
			cdb[8] = (uint8_t)piece;
			cdb[9] = audio ? 0x10 : 0xF8;
			cdb[10] = sub == Sub::Raw ? 0x01 : sub == Sub::Q ? 0x02 : 0x00;
			if (command(cdb, 12, out + (size_t)done * each, piece * each, true, !audio))
			{
				done += piece;
				continue;
			}
			// A transfer the bus did not carry whole: half as much, remembered.
			if (shortTransfer && piece > 1)
			{
				maxTransfer = std::max<uint32_t>(each, (piece * each) / 2);
				diag::mark("usb drive: %s: a transfer of %u bytes was cut short; reading %u bytes at a time",
						device.c_str(), piece * each, (maxTransfer / each) * each);
				continue;
			}
			return false;
		}
		return true;
	}

	bool readData(uint32_t lba, uint32_t count, uint8_t *out) override
	{
		uint32_t done = 0;
		while (done < count)
		{
			const uint32_t piece = std::min(count - done, std::max<uint32_t>(1, maxTransfer / DataSector));
			uint8_t cdb[10] = {};
			const uint32_t at = lba + done;
			cdb[0] = 0x28;				// READ(10)
			cdb[2] = (uint8_t)(at >> 24);
			cdb[3] = (uint8_t)(at >> 16);
			cdb[4] = (uint8_t)(at >> 8);
			cdb[5] = (uint8_t)at;
			cdb[7] = (uint8_t)(piece >> 8);
			cdb[8] = (uint8_t)piece;
			if (command(cdb, 10, out + (size_t)done * DataSector, piece * DataSector, true))
			{
				done += piece;
				continue;
			}
			if (shortTransfer && piece > 1)
			{
				maxTransfer = std::max<uint32_t>(DataSector, (piece * DataSector) / 2);
				continue;
			}
			return false;
		}
		return true;
	}

	bool setSpeed(unsigned kilobytesPerSecond) override
	{
		uint8_t cdb[12] = {};
		cdb[0] = 0xBB;					// SET CD SPEED
		cdb[2] = (uint8_t)(kilobytesPerSecond >> 8);
		cdb[3] = (uint8_t)kilobytesPerSecond;
		cdb[4] = 0xFF;
		cdb[5] = 0xFF;
		return command(cdb, 12, nullptr, 0, true, false);
	}

	std::string route() const override { return "USB bulk-only transport by PSSwanStation itself (" + how + ")"; }
	int onUsb() const override { return 1; }
	std::string identity() const override
	{
		return (f.maker + " " + f.product + format(", %04x:%04x on USB bus %d address %d; no driver of the console's on it, "
				"read by PSSwanStation itself", f.vid, f.pid, f.bus, f.addr));
	}

	bool allowRemoval(std::string& how) override
	{
		uint8_t cdb[6] = {};
		cdb[0] = 0x1E;					// PREVENT ALLOW MEDIUM REMOVAL: allow
		if (command(cdb, 6, nullptr, 0, true))
		{
			how = "PREVENT ALLOW MEDIUM REMOVAL";
			return true;
		}
		how = "PREVENT ALLOW MEDIUM REMOVAL failed: " + lastFailure;
		return false;
	}

	bool eject(std::string& how) override
	{
		std::string allowed;
		allowRemoval(allowed);
		uint8_t cdb[6] = {};
		cdb[0] = 0x1B;					// START STOP UNIT: eject
		cdb[4] = 0x02;
		if (command(cdb, 6, nullptr, 0, true))
		{
			how = "START STOP UNIT";
			return true;
		}
		how = "START STOP UNIT failed: " + lastFailure;
		return false;
	}

	// INQUIRY: the peripheral type (5 a CD/DVD drive) and the drive's name.
	bool inquire(uint8_t& type, std::string& name)
	{
		uint8_t data[36] = {};
		uint8_t cdb[6] = {};
		cdb[0] = 0x12;
		cdb[4] = sizeof(data);
		if (!command(cdb, 6, data, sizeof(data), true))
			return false;
		type = (uint8_t)(data[0] & 0x1F);
		name = trim(ascii(data + 8, 8) + " " + ascii(data + 16, 16));
		return true;
	}

private:
	int ctl;
	Found f;
	std::unique_ptr<Transport> transport;
	std::string how, device;
	uint32_t tag = 1;
	uint32_t maxTransfer = ChunkBytes;
	bool shortTransfer = false;
	bool told = false, toldToc = false;
	uint8_t senseKey = 0, senseAsc = 0, senseAscq = 0;
	std::string lastFailure;

	std::string senseText() const
	{
		return format("%x/%02x/%02x", senseKey, senseAsc, senseAscq);
	}

	// A bulk-only mass storage reset, then the endpoints cleared: the
	// recovery the transport asks for after a phase error.
	void reset()
	{
		transport->control(0x21, 0xFF, 0, (uint16_t)f.iface, nullptr, 0);
		transport->clearHalt(f.epIn);
		transport->clearHalt(f.epOut);
	}

	bool requestSense()
	{
		uint8_t data[18] = {};
		uint8_t cdb[6] = {};
		cdb[0] = 0x03;
		cdb[4] = sizeof(data);
		if (!transact(cdb, 6, data, sizeof(data), true))
			return false;
		senseKey = (uint8_t)(data[2] & 0x0F);
		senseAsc = data[12];
		senseAscq = data[13];
		return true;
	}

	// One command with its reply: false with the sense read when the drive
	// refused it (a unit attention after a disc change is retried once).
	bool command(const uint8_t *cdb, uint8_t length, uint8_t *data, uint32_t bytes, bool dataIn, bool tell = true)
	{
		for (int attempt = 0; attempt < 2; attempt++)
		{
			senseKey = senseAsc = senseAscq = 0;
			shortTransfer = false;
			const int result = transactWithStatus(cdb, length, data, bytes, dataIn);
			if (result == 0)
				return true;
			if (result == 1)
			{
				requestSense();
				lastFailure = "refused, sense " + senseText();
				if (senseKey == 0x06 && attempt == 0)
					continue;		// unit attention: the disc changed, ask again
			}
			else
				lastFailure = result == 2 ? "phase error, reset" : std::string("transport: ") + strerror(transport->lastErrno);
			if (tell && !told && !(senseKey == 0x02 && senseAsc == 0x3A))
			{
				told = true;
				diag::mark("usb drive: %s: SCSI 0x%02X failed: %s", device.c_str(), cdb[0], lastFailure.c_str());
			}
			return false;
		}
		return false;
	}

	bool transact(const uint8_t *cdb, uint8_t length, uint8_t *data, uint32_t bytes, bool dataIn)
	{
		return transactWithStatus(cdb, length, data, bytes, dataIn) == 0;
	}

	// The bulk-only transport: command block out, data, status block in.
	// 0 passed, 1 the drive refused the command, 2 a phase error (reset done),
	// -1 the transport failed.
	int transactWithStatus(const uint8_t *cdb, uint8_t length, uint8_t *data, uint32_t bytes, bool dataIn)
	{
		uint8_t cbw[31] = {};
		const uint32_t thisTag = tag++;
		memcpy(cbw, "USBC", 4);
		cbw[4] = (uint8_t)thisTag;
		cbw[5] = (uint8_t)(thisTag >> 8);
		cbw[6] = (uint8_t)(thisTag >> 16);
		cbw[7] = (uint8_t)(thisTag >> 24);
		cbw[8] = (uint8_t)bytes;
		cbw[9] = (uint8_t)(bytes >> 8);
		cbw[10] = (uint8_t)(bytes >> 16);
		cbw[11] = (uint8_t)(bytes >> 24);
		cbw[12] = dataIn ? 0x80 : 0x00;
		cbw[13] = 0;					// LUN 0
		cbw[14] = length;
		memcpy(cbw + 15, cdb, length);
		if (!transport->bulkOut(cbw, sizeof(cbw)))
		{
			transport->clearHalt(f.epOut);
			return -1;
		}
		uint32_t moved = 0;
		if (bytes != 0)
		{
			if (dataIn)
			{
				const int n = transport->bulkIn(data, bytes, CommandTimeoutMs);
				if (n < 0)
				{
					transport->clearHalt(f.epIn);
					if (transport->lastErrno == ETIMEDOUT)
						return -1;
				}
				else
					moved = (uint32_t)n;
			}
			else if (!transport->bulkOut(data, bytes))
				transport->clearHalt(f.epOut);
			else
				moved = bytes;
		}
		uint8_t csw[13] = {};
		int n = transport->bulkIn(csw, sizeof(csw), CommandTimeoutMs);
		if (n != 13)
		{
			// A stalled IN endpoint after a short data phase: cleared, the
			// status is asked for once more.
			transport->clearHalt(f.epIn);
			n = transport->bulkIn(csw, sizeof(csw), CommandTimeoutMs);
		}
		if (n != 13 || memcmp(csw, "USBS", 4) != 0)
		{
			reset();
			return 2;
		}
		const uint32_t cswTag = (uint32_t)(csw[4] | (csw[5] << 8) | (csw[6] << 16) | (csw[7] << 24));
		const uint8_t status = csw[12];
		if (cswTag != thisTag || status == 2)
		{
			reset();
			return 2;
		}
		if (status == 1)
			return 1;
		if (moved != bytes)
		{
			shortTransfer = true;
			lastFailure = format("%u of %u bytes came", moved, bytes);
			return -1;
		}
		return 0;
	}
};

// --------------------------------------------------------------- looking

std::map<std::string, Found> known;		// per ugen node, as last looked at
std::set<std::string> taken;			// nodes read as drives
unsigned passes = 0;

// Looks at one USB device the console lists: what it is, and whether it is
// a mass storage device nobody drives. Told once per node.
void look(const std::string& node, const std::vector<Attached>& attached, size_t massStorageCount, Found& f)
{
	f = Found();
	f.node = node;
	sscanf(node.c_str(), "ugen%d.%d", &f.bus, &f.addr);
	const int fd = ::open(("/dev/" + node).c_str(), O_RDWR);
	if (fd < 0)
	{
		// ENXIO: a hub, which has no generic node to open. Not told.
		if (errno != ENXIO)
			diag::mark("usb drive: /dev/%s could not be opened: %s", node.c_str(), strerror(errno));
		f.why = errno == ENXIO ? "a hub" : "could not be opened";
		return;
	}
	usb_device_descriptor dd = {};
	if (!getDeviceDescriptor(fd, node, dd, f.descriptorsBy))
	{
		f.why = "the console will not say what it is";
		::close(fd);
		return;
	}
	f.vid = UGETW(dd.idVendor);
	f.pid = UGETW(dd.idProduct);
	f.maker = getString(fd, dd.iManufacturer);
	f.product = getString(fd, dd.iProduct);
	std::vector<uint8_t> configuration;
	if (getConfiguration(fd, configuration, f.descriptorsBy))
		findInterface(configuration, f);
	else
		f.why = "its configuration could not be read";
	if (f.massStorage)
	{
		int iface = f.iface;
		const int active = ioctl(fd, USB_IFACE_DRIVER_ACTIVE, &iface);
		if (active == 0)
			f.driver = 1;
		else if (errno == ENXIO || errno == ENOENT)
			f.driver = 0;
		else
			f.driver = -1;
		std::vector<uint8_t> name(64, 0);
		usb_gen_descriptor gd = {};
		gd.ugd_data = name.data();
		gd.ugd_maxlen = (uint16_t)name.size();
		gd.ugd_iface_index = (uint8_t)f.iface;
		if (ioctl(fd, USB_GET_IFACE_DRIVER, &gd) == 0)
		{
			f.driverName = ascii(name.data(), name.size());
			if (!f.driverName.empty())
				f.driver = 1;
		}
	}
	::close(fd);
	const std::string what = format("%04x:%04x \"%s\" \"%s\"", f.vid, f.pid, f.maker.c_str(), f.product.c_str());
	if (!f.massStorage)
	{
		if (f.why.empty())
			f.why = f.uasOnly ? "USB Attached SCSI only, which PSSwanStation cannot drive" : "not a mass storage device";
		diag::mark("usb drive: %s: %s (descriptors by %s): %s", node.c_str(), what.c_str(), f.descriptorsBy.c_str(), f.why.c_str());
		return;
	}
	if (f.driver == 1)
	{
		f.why = "the console drives it" + (f.driverName.empty() ? std::string() : " (" + f.driverName + ")");
		diag::mark("usb drive: %s: %s, mass storage, interface %d: %s", node.c_str(), what.c_str(), f.iface, f.why.c_str());
		return;
	}
	if (f.driver < 0)
	{
		// The console will not say. It is taken only when it is plainly not
		// one of the drives the console attached, and there is one to spare.
		// Without the count (a first look) nothing is decided or told.
		if (massStorageCount == 0)
			return;
		bool named = false;
		for (const Attached& a : attached)
			if (shareWord(a.name, f.maker + " " + f.product))
				named = true;
		if (named)
			f.why = "the console will not say whether it drives it, and it is named like a drive the console attached";
		else if (attached.size() >= massStorageCount)
			f.why = format("the console will not say whether it drives it, and it attached %zu USB storage device(s) for "
					"%zu on the bus", attached.size(), massStorageCount);
		if (!f.why.empty())
		{
			diag::mark("usb drive: %s: %s, mass storage, interface %d: left alone: %s", node.c_str(), what.c_str(), f.iface,
					f.why.c_str());
			return;
		}
	}
	diag::mark("usb drive: %s: %s, mass storage (SCSI, bulk-only) on interface %d alternate %d, endpoints in %#x out %#x; "
			"%s: PSSwanStation reads it itself", node.c_str(), what.c_str(), f.iface, f.altBot, f.epIn, f.epOut,
			f.driver == 0 ? "no driver of the console's on it" : "the console will not say whether it drives it");
}

std::unique_ptr<BotDrive> openFound(const Found& f, std::string& error)
{
	const int fd = ::open(("/dev/" + f.node).c_str(), O_RDWR);
	if (fd < 0)
	{
		error = "/dev/" + f.node + ": " + strerror(errno);
		return nullptr;
	}
	if (f.altBot > 0)
	{
		usb_alt_interface alt = {};
		alt.uai_interface_index = (uint8_t)f.iface;
		alt.uai_alt_index = (uint8_t)f.altBot;
		if (ioctl(fd, USB_SET_ALTINTERFACE, &alt) != 0)
			diag::mark("usb drive: %s: the bulk-only alternate setting %d could not be chosen: %s", f.node.c_str(), f.altBot,
					strerror(errno));
	}
	int iface = f.iface;
	ioctl(fd, USB_CLAIM_INTERFACE, &iface);
	auto transport = std::make_unique<Transport>(fd, f);
	std::string how;
	if (!transport->open(how))
	{
		error = "the drive's endpoints could not be opened (" + how + ")";
		transport.reset();
		::close(fd);
		return nullptr;
	}
	return std::make_unique<BotDrive>(fd, f, std::move(transport), how);
}

}

// ------------------------------------------------------- the ports' state

// A hub's ports, asked of the hub itself (hub class requests through its
// control endpoint): what is electrically on each port, enabled or not. A
// drive the console never lists may still show as a connection on a port
// that was not enabled (enumeration failed, or the console refused it), or
// as an over-current. A port that holds a connection with no device on it is
// reset once, which makes the console try to enumerate it again. Told once
// per set of ports; the open of each hub's node is tried every way, as the
// console's ugen refused the root hubs read-write.
std::set<std::string> portsTold;
std::set<std::string> portsReset;

int openHub(const std::string& node, std::string& how)
{
	const std::string ugen = "/dev/" + node;
	int bus = -1, addr = -1;
	sscanf(node.c_str(), "ugen%d.%d", &bus, &addr);
	const std::string ep0 = format("/dev/usb/%d.%d.0", bus, addr);
	const std::string paths[2] = { ugen, ep0 };
	const int flags[3] = { O_RDWR, O_RDONLY, O_WRONLY };
	const char *names[3] = { "read-write", "read-only", "write-only" };
	std::string tried;
	for (const std::string& path : paths)
		for (int i = 0; i < 3; i++)
		{
			const int fd = ::open(path.c_str(), flags[i]);
			if (fd >= 0)
			{
				how = path + " " + names[i];
				return fd;
			}
			tried += format("%s%s %s: %s", tried.empty() ? "" : "; ", path.c_str(), names[i], strerror(errno));
		}
	how = tried;
	return -1;
}

void reportPorts(const std::vector<std::string>& nodes)
{
	// Where each device sits: its port path, so that a port with a device is
	// told apart from one with a bare connection.
	std::map<std::string, std::string> atPort;		// "bus:port" -> node
	for (const std::string& node : nodes)
	{
		const int fd = ::open(("/dev/" + node).c_str(), O_RDWR);
		if (fd < 0)
			continue;
		usb_device_port_path path = {};
		if (ioctl(fd, USB_GET_DEV_PORT_PATH, &path) == 0 && path.udp_port_level >= 1)
		{
			// The root hub's port is the first level; deeper levels are a hub's.
			std::string key = format("%u", path.udp_bus);
			for (uint8_t i = 0; i < path.udp_port_level && i < USB_DEVICE_PORT_PATH_MAX; i++)
				key += format(":%u", path.udp_port_no[i]);
			atPort[key] = node;
		}
		::close(fd);
	}
	for (const std::string& node : nodes)
	{
		int bus = -1, addr = -1;
		sscanf(node.c_str(), "ugen%d.%d", &bus, &addr);
		// Hubs: the root hub (address 1) and any device of class 9.
		bool hub = addr == 1;
		int fd = -1;
		std::string how;
		if (!hub)
		{
			fd = ::open(("/dev/" + node).c_str(), O_RDWR);
			usb_device_descriptor dd = {};
			if (fd >= 0 && ioctl(fd, USB_GET_DEVICE_DESC, &dd) == 0 && dd.bDeviceClass == 0x09)
				hub = true;
			if (!hub)
			{
				if (fd >= 0)
					::close(fd);
				continue;
			}
			how = "/dev/" + node + " read-write";
		}
		else
			fd = openHub(node, how);
		if (fd < 0)
		{
			if (portsTold.insert(node + "|closed").second)
				diag::mark("usb bus: %s (a hub): its node could not be opened (%s); its ports cannot be asked", node.c_str(), how.c_str());
			continue;
		}
		// The hub descriptor: USB 2 hubs give it as type 0x29, USB 3 hubs as 0x2A.
		uint8_t hd[64] = {};
		usb_ctl_request req = {};
		req.ucr_data = hd;
		req.ucr_flags = USB_SHORT_XFER_OK;
		req.ucr_request.bmRequestType = 0xA0;
		req.ucr_request.bRequest = 0x06;
		USETW(req.ucr_request.wIndex, 0);
		USETW(req.ucr_request.wLength, sizeof(hd));
		bool superSpeed = false;
		USETW(req.ucr_request.wValue, 0x2900);
		bool got = ioctl(fd, USB_DO_REQUEST, &req) == 0 && req.ucr_actlen >= 3;
		if (!got)
		{
			USETW(req.ucr_request.wValue, 0x2A00);
			got = ioctl(fd, USB_DO_REQUEST, &req) == 0 && req.ucr_actlen >= 3;
			superSpeed = got;
		}
		if (!got)
		{
			if (portsTold.insert(node + "|nodesc").second)
				diag::mark("usb bus: %s (a hub, opened as %s): the hub descriptor could not be read: %s", node.c_str(), how.c_str(),
						strerror(errno));
			::close(fd);
			continue;
		}
		const int ports = hd[2];
		std::string report;
		for (int port = 1; port <= ports && port <= 16; port++)
		{
			uint8_t status[4] = {};
			usb_ctl_request r = {};
			r.ucr_data = status;
			r.ucr_request.bmRequestType = 0xA3;
			r.ucr_request.bRequest = 0x00;			// GET STATUS, port
			USETW(r.ucr_request.wValue, 0);
			USETW(r.ucr_request.wIndex, (uint16_t)port);
			USETW(r.ucr_request.wLength, 4);
			if (ioctl(fd, USB_DO_REQUEST, &r) != 0)
			{
				report += format(" %d: not answered (%s)", port, strerror(errno));
				continue;
			}
			const uint16_t st = (uint16_t)(status[0] | (status[1] << 8));
			const uint16_t change = (uint16_t)(status[2] | (status[3] << 8));
			const bool connected = (st & 0x0001) != 0, enabled = (st & 0x0002) != 0, overCurrent = (st & 0x0008) != 0;
			const bool powered = superSpeed ? (st & 0x0200) != 0 : (st & 0x0100) != 0;
			std::string speed;
			if (superSpeed)
				speed = format("SuperSpeed link state %u", (st >> 5) & 0x0F);
			else
				speed = (st & 0x0400) != 0 ? "high speed" : (st & 0x0200) != 0 ? "low speed" : "full speed";
			const std::string key = format("%d:%d", bus, port);
			const auto it = atPort.find(key);
			report += format(" %d: %s%s%s%s%s (status %04x change %04x)", port,
					connected ? "connected" : "empty", enabled ? ", enabled" : connected ? ", NOT enabled" : "",
					overCurrent ? ", OVER-CURRENT" : "", powered ? "" : ", unpowered",
					it != atPort.end() ? (", " + it->second).c_str() : connected ? (", no device listed; " + speed).c_str() : "",
					st, change);
			// A connection the console did not enumerate: the port reset once,
			// so the console looks at the device again.
			if (connected && it == atPort.end() && addr == 1 && portsReset.insert(key).second)
			{
				usb_ctl_request rs = {};
				rs.ucr_request.bmRequestType = 0x23;
				rs.ucr_request.bRequest = 0x03;		// SET FEATURE
				USETW(rs.ucr_request.wValue, powered ? 4 : 8);	// PORT_RESET, or PORT_POWER first
				USETW(rs.ucr_request.wIndex, (uint16_t)port);
				USETW(rs.ucr_request.wLength, 0);
				const int rc = ioctl(fd, USB_DO_REQUEST, &rs);
				diag::mark("usb bus: %s port %d holds a connection the console did not list: %s asked for (%s)", node.c_str(), port,
						powered ? "a port reset" : "port power", rc == 0 ? "accepted" : strerror(errno));
			}
		}
		::close(fd);
		if (portsTold.insert(node + "|" + report).second)
			diag::mark("usb bus: %s (%s hub, opened as %s), %d port(s):%s", node.c_str(), superSpeed ? "a USB 3" : "a USB 2",
					how.c_str(), ports, report.c_str());
	}
}

// ------------------------------------------------- the kernel's own words

// The kernel's message buffer (kern.msgbuf), which FreeBSD's USB stack writes
// to when a device comes, goes or fails to enumerate ("uhub2: port 3: device
// problem (SET_ADDR_FAILED), disabling port"): the one account of a device
// that never got as far as a node. Its USB lines go to the boot log, each
// once; read at the first look and again while no drive is found.
std::set<std::string> kernelTold;
bool kernelRefused = false;

bool usbLine(const std::string& line)
{
	std::string l = line;
	for (char& c : l)
		c = (char)std::tolower((unsigned char)c);
	static const char *keys[] = { "usb", "uhub", "umass", "ugen", "xhci", "ehci", "ohci", "set_addr", "enumerat",
			"over-current", "overcurrent", "port reset", "disabling port", "cd1", "cd2", "cd3", "scsi" };
	for (const char *k : keys)
		if (l.find(k) != std::string::npos)
			return true;
	return false;
}

void readKernelLog()
{
	if (kernelRefused)
		return;
	size_t length = 0;
	if (sysctlbyname("kern.msgbuf", nullptr, &length, nullptr, 0) != 0 || length == 0)
	{
		kernelRefused = true;
		diag::mark("usb bus: the kernel's message buffer cannot be read (kern.msgbuf: %s)", length == 0 && errno == 0
				? "empty" : strerror(errno));
		return;
	}
	std::vector<char> buffer(length + 4096, 0);
	length = buffer.size();
	if (sysctlbyname("kern.msgbuf", buffer.data(), &length, nullptr, 0) != 0)
	{
		kernelRefused = true;
		diag::mark("usb bus: the kernel's message buffer cannot be read (kern.msgbuf: %s)", strerror(errno));
		return;
	}
	std::vector<std::string> lines;
	std::string line;
	for (size_t i = 0; i < length; i++)
	{
		const char c = buffer[i];
		if (c == '\n' || c == '\0')
		{
			if (!line.empty() && usbLine(line))
				lines.push_back(line);
			line.clear();
		}
		else if (c >= 0x20 && c < 0x7F)
			line += c;
	}
	// The newest eighty: the buffer is a ring of the console's whole uptime.
	const size_t from = lines.size() > 80 ? lines.size() - 80 : 0;
	int told = 0;
	for (size_t i = from; i < lines.size(); i++)
		if (kernelTold.insert(lines[i]).second && kernelTold.size() < 2000)
		{
			diag::mark("usb bus: kernel: %s", lines[i].c_str());
			told++;
		}
	if (kernelTold.empty() && told == 0)
	{
		kernelTold.insert("");
		diag::mark("usb bus: the kernel's message buffer (%zu bytes) holds no USB lines", length);
	}
}

std::vector<std::string> deviceNames()
{
	std::vector<std::string> out;
	std::vector<std::string> nodes;
	if (DIR *dir = opendir("/dev"))
	{
		while (const dirent *entry = readdir(dir))
			if (std::strncmp(entry->d_name, "ugen", 4) == 0 && std::isdigit((unsigned char)entry->d_name[4]))
				nodes.push_back(entry->d_name);
		closedir(dir);
	}
	std::sort(nodes.begin(), nodes.end());
	// Nodes that went away: forgotten, so that a device plugged in again at
	// the same address is looked at afresh.
	for (auto it = known.begin(); it != known.end();)
	{
		if (std::find(nodes.begin(), nodes.end(), it->first) == nodes.end())
		{
			taken.erase(it->first);
			it = known.erase(it);
		}
		else
			++it;
	}
	std::vector<std::string> fresh;
	for (const std::string& node : nodes)
		if (known.find(node) == known.end())
			fresh.push_back(node);
	if (!fresh.empty())
	{
		reportPorts(nodes);
		readKernelLog();
	}
	if (!fresh.empty())
	{
		// Everything the console attached, and how many mass storage devices
		// are on the bus: the guard for a device the console will not vouch for.
		const std::vector<Attached> attached = attachedByConsole();
		if (passes++ == 0)
		{
			std::string list;
			for (const Attached& a : attached)
				list += " " + a.periph + " (" + a.name + ")";
			diag::mark("usb drive: the console attached %zu USB storage device(s):%s", attached.size(),
					list.empty() ? " none" : list.c_str());
		}
		// A first look at each, for the count; then the decision.
		std::vector<Found> looked;
		for (const std::string& node : fresh)
		{
			Found f;
			look(node, attached, 0, f);
			looked.push_back(f);
		}
		size_t massStorage = 0;
		for (const Found& f : looked)
			massStorage += f.massStorage ? 1 : 0;
		for (const Found& f : looked)
		{
			Found decided = f;
			if (f.massStorage && f.driver < 0)
			{
				// Looked at again with the count, which decides; told then.
				look(f.node, attached, massStorage, decided);
			}
			known[f.node] = decided;
			if (decided.massStorage && decided.why.empty())
			{
				// Only a drive: INQUIRY says what kind of device it is. A disk
				// (the console's USB stick, were it ever here) is left alone.
				std::string error;
				std::unique_ptr<BotDrive> drive = openFound(decided, error);
				uint8_t type = 0xFF;
				std::string name;
				if (drive && drive->inquire(type, name))
				{
					if (type == 0x05)
					{
						diag::mark("usb drive: %s: INQUIRY: \"%s\", a CD/DVD drive: listed as a disc drive", f.node.c_str(),
								name.c_str());
						taken.insert(f.node);
					}
					else
					{
						known[f.node].why = format("INQUIRY says it is device type %u (\"%s\"), not a CD/DVD drive", type, name.c_str());
						diag::mark("usb drive: %s: %s: left alone", f.node.c_str(), known[f.node].why.c_str());
					}
				}
				else
				{
					known[f.node].why = drive ? "INQUIRY failed" : error;
					diag::mark("usb drive: %s: not read: %s", f.node.c_str(), known[f.node].why.c_str());
				}
			}
		}
	}
	static unsigned idle = 0;
	if (fresh.empty() && taken.empty() && ++idle % 5 == 0)
		readKernelLog();
	for (const std::string& node : nodes)
		if (taken.count(node) != 0)
			out.push_back("/dev/" + node);
	return out;
}

std::unique_ptr<Drive> openDevice(const std::string& device, std::string& error)
{
	const std::string node = device.rfind("/dev/", 0) == 0 ? device.substr(5) : device;
	const auto it = known.find(node);
	if (it == known.end() || taken.count(node) == 0)
	{
		error = device + ": not a drive PSSwanStation reads";
		return nullptr;
	}
	return openFound(it->second, error);
}

}
