/*
	PSSwanStation - a USB CD or DVD drive on the console (discdrive_io.h).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The console's kernel is FreeBSD's: a drive is the cd(4) device /dev/cdN
	(the console's own drive is /dev/cd0, a USB drive /dev/cd1, as 5S2CD
	found), opened once PSSwanStation has left its sandbox. Its table of
	contents comes from cd(4)'s CDIOREADTOCHEADER and CDIOREADTOCENTRYS, which
	hand the LBA format's addresses back in network byte order; its data
	sectors from pread() at 2048 bytes a sector.

	Whole sectors (2352 bytes) and the sub-channel, which a PlayStation game's
	videos, its XA sound, its CD music and LibCrypt need, have no cd(4) call:
	they are read with the SCSI command READ CD, sent to the drive's pass(4)
	device through CAM. cd(4) names its pass device (CAMGETPASSTHRU), which is
	opened read and write, asked where it is on the bus, and given each command
	as an XPT_SCSI_IO request (CAMIOCOMMAND), as FreeBSD's libcam does. Whether
	the console's kernel has pass(4) and lets a title use it is not known
	until a console runs it: every step that fails says why in the boot log,
	and the disc is then read 2048 bytes a sector.
*/
#include "../discdrive_io.h"
#include "../fe.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <dirent.h>
#include <sys/cdio.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <cstdint>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

namespace fe::discdrive
{
namespace
{

class CamDrive final : public Drive
{
public:
	CamDrive(std::string device, int fd) : device(std::move(device)), fd(fd) { findPass(); }
	~CamDrive() override
	{
		if (passFd >= 0)
			::close(passFd);
		if (fd >= 0)
			::close(fd);
	}

	bool readToc(std::vector<TocEntry>& out, std::string& error) override
	{
		out.clear();
		ioc_toc_header header{};
		if (ioctl(fd, CDIOREADTOCHEADER, &header) != 0)
		{
			// No table of contents through cd(4): the disc as one data track,
			// sized by the medium, as 5S2CD and Crux2 read the drive.
			const int tocErrno = errno;
			off_t bytes = 0;
			if (ioctl(fd, DIOCGMEDIASIZE, &bytes) == 0 && bytes >= (off_t)(DataSector * 32))
			{
				TocEntry track;
				track.track = 1;
				track.control = 0x04;
				track.lba = 0;
				TocEntry end;
				end.track = 0xAA;
				end.lba = (uint32_t)(bytes / DataSector);
				out.push_back(track);
				out.push_back(end);
				if (!toldToc)
				{
					toldToc = true;
					diag::mark("disc drive: %s: no table of contents (%s); %u sectors by the medium's size, as one data track",
							device.c_str(), strerror(tocErrno), end.lba);
				}
				return true;
			}
			error = std::string("no disc the drive can read (table of contents: ") + strerror(tocErrno) + ")";
			return false;
		}
		if (header.ending_track < header.starting_track || header.starting_track == 0)
		{
			error = "the disc's table of contents is empty";
			return false;
		}
		const int count = header.ending_track - header.starting_track + 2;	// and the lead-out
		std::vector<cd_toc_entry> entries((size_t)count);
		ioc_read_toc_entry request{};
		request.address_format = CD_LBA_FORMAT;
		request.starting_track = header.starting_track;
		request.data_len = (u_short)(count * sizeof(cd_toc_entry));
		request.data = entries.data();
		if (ioctl(fd, CDIOREADTOCENTRYS, &request) != 0)
		{
			error = std::string("the table of contents could not be read (") + strerror(errno) + ")";
			return false;
		}
		// Network byte order, as cd(4) gives it: the lead-out read that way is
		// on a disc (under 100 minutes) and read as it is is not. Otherwise
		// this kernel gave it as it is.
		const uint32_t asGiven = (uint32_t)entries.back().addr.lba;
		const bool swap = ntohl(asGiven) <= 450000 && asGiven > 450000;
		for (const cd_toc_entry& e : entries)
		{
			TocEntry entry;
			entry.track = e.track;
			entry.control = (uint8_t)e.control;
			entry.adr = (uint8_t)e.addr_type;
			entry.lba = swap ? ntohl((uint32_t)e.addr.lba) : (uint32_t)e.addr.lba;
			if (e.track == 0xAA || (e.track >= 1 && e.track <= 99))
				out.push_back(entry);
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
		if (passFd < 0 || count == 0)
			return false;
		const uint32_t each = RawSector + subSize(sub);
		uint32_t done = 0;
		while (done < count)
		{
			// As many sectors as one transfer through pass(4) may carry.
			const uint32_t piece = std::min(count - done, std::max<uint32_t>(1, maxTransfer / each));
			uint8_t cdb[12] = {};
			const uint32_t at = lba + done;
			cdb[0] = 0xBE;				// READ CD
			cdb[1] = audio ? 0x04 : 0x00;		// CD-DA sectors, or any type of sector
			cdb[2] = (uint8_t)(at >> 24);
			cdb[3] = (uint8_t)(at >> 16);
			cdb[4] = (uint8_t)(at >> 8);
			cdb[5] = (uint8_t)at;
			cdb[6] = (uint8_t)(piece >> 16);
			cdb[7] = (uint8_t)(piece >> 8);
			cdb[8] = (uint8_t)piece;
			cdb[9] = audio ? 0x10 : 0xF8;		// audio: its 2352 bytes; else sync, all headers, user data, EDC and ECC
			cdb[10] = sub == Sub::Raw ? 0x01 : sub == Sub::Q ? 0x02 : 0x00;
			if (command(cdb, out + (size_t)done * each, piece * each, 12, !audio))
			{
				done += piece;
				continue;
			}
			// E2BIG: more than the kernel maps for one request (build 21's
			// tests: every 26-sector read refused, each block then read a
			// sector at a time). Half as much, remembered for the drive.
			if (lastErrno == E2BIG && piece > 1)
			{
				maxTransfer = std::max<uint32_t>(each, (piece * each) / 2);
				diag::mark("disc drive: %s: a transfer of %u bytes is too big for the kernel; reading %u bytes at a time",
						device.c_str(), piece * each, (maxTransfer / each) * each);
				continue;
			}
			return false;
		}
		return true;
	}

	bool readData(uint32_t lba, uint32_t count, uint8_t *out) override
	{
		const size_t bytes = (size_t)count * DataSector;
		size_t done = 0;
		while (done < bytes)
		{
			const ssize_t got = pread(fd, out + done, bytes - done, (off_t)lba * DataSector + (off_t)done);
			if (got <= 0)
			{
				if (got < 0 && errno == EINTR)
					continue;
				return false;
			}
			done += (size_t)got;
		}
		return true;
	}

	bool setSpeed(unsigned kilobytesPerSecond) override
	{
		if (passFd < 0)
			return false;
		uint8_t cdb[12] = {};
		cdb[0] = 0xBB;					// SET CD SPEED
		cdb[2] = (uint8_t)(kilobytesPerSecond >> 8);
		cdb[3] = (uint8_t)kilobytesPerSecond;
		cdb[4] = 0xFF;					// writing: as it is
		cdb[5] = 0xFF;
		return command(cdb, nullptr, 0);
	}

	std::string route() const override { return passName.empty() ? "no pass device: " + passWhy : passName; }
	int onUsb() const override { return usb; }
	std::string identity() const override { return ident; }

	bool allowRemoval(std::string& how) override
	{
		// cd(4)'s own call first, then PREVENT ALLOW MEDIUM REMOVAL (0x1E,
		// allow) through the pass device.
		if (ioctl(fd, CDIOCALLOW) == 0)
		{
			how = "CDIOCALLOW";
			return true;
		}
		const int first = errno;
		uint8_t cdb[12] = {};
		cdb[0] = 0x1E;
		if (command(cdb, nullptr, 0, 6))
		{
			how = "PREVENT ALLOW MEDIUM REMOVAL";
			return true;
		}
		how = std::string("CDIOCALLOW: ") + strerror(first) + ", and the SCSI command failed";
		return false;
	}

	bool eject(std::string& how) override
	{
		std::string allowed;
		allowRemoval(allowed);
		if (ioctl(fd, CDIOCEJECT) == 0)
		{
			how = "CDIOCEJECT";
			return true;
		}
		const int first = errno;
		uint8_t cdb[12] = {};
		cdb[0] = 0x1B;					// START STOP UNIT
		cdb[4] = 0x02;					// load/eject: eject
		if (command(cdb, nullptr, 0, 6))
		{
			how = "START STOP UNIT";
			return true;
		}
		how = std::string("CDIOCEJECT: ") + strerror(first) + ", and the SCSI command failed";
		return false;
	}

private:
	std::string device;
	int fd = -1;
	int passFd = -1;
	std::string passName, passWhy;
	path_id_t pathId = 0;
	target_id_t targetId = 0;
	lun_id_t targetLun = 0;
	bool told = false;
	bool toldToc = false;
	int usb = -1;
	std::string ident;
	static constexpr size_t PageSize = 4096;
	std::vector<uint8_t> bounce;
	int lastErrno = 0;
	// The most one READ CD carries: 64 KiB to start with (FreeBSD's DFLTPHYS,
	// the least any controller takes), halved if the kernel refuses it.
	uint32_t maxTransfer = 65536;

	void findPass()
	{
		union ccb ccb;
		memset(&ccb, 0, sizeof(ccb));
		ccb.ccb_h.func_code = XPT_GDEVLIST;
		if (ioctl(fd, CAMGETPASSTHRU, &ccb) != 0)
		{
			passWhy = std::string("CAMGETPASSTHRU: ") + strerror(errno);
			return;
		}
		if (ccb.cgdl.status == CAM_GDEVLIST_ERROR || ccb.cgdl.periph_name[0] == '\0')
		{
			passWhy = "CAMGETPASSTHRU found no pass device";
			return;
		}
		const std::string name = std::string(ccb.cgdl.periph_name) + std::to_string(ccb.cgdl.unit_number);
		const int pass = ::open(("/dev/" + name).c_str(), O_RDWR);
		if (pass < 0)
		{
			passWhy = "/dev/" + name + ": " + strerror(errno);
			return;
		}
		// Where it is on the bus, from the pass device itself.
		memset(&ccb, 0, sizeof(ccb));
		ccb.ccb_h.func_code = XPT_GDEVLIST;
		if (ioctl(pass, CAMGETPASSTHRU, &ccb) != 0)
		{
			passWhy = name + " CAMGETPASSTHRU: " + strerror(errno);
			::close(pass);
			return;
		}
		pathId = ccb.ccb_h.path_id;
		targetId = ccb.ccb_h.target_id;
		targetLun = ccb.ccb_h.target_lun;
		passFd = pass;
		passName = name;
		describe();
	}

	// What the drive is and which bus it is on, asked of CAM through the pass
	// device as libcam does when it opens one: the inquiry data (XPT_GDEV_TYPE)
	// and the bus's own (XPT_PATH_INQ). USB drives are on umass's bus.
	void describe()
	{
		union ccb ccb;
		memset(&ccb, 0, sizeof(ccb));
		ccb.ccb_h.func_code = XPT_GDEV_TYPE;
		ccb.ccb_h.path_id = pathId;
		ccb.ccb_h.target_id = targetId;
		ccb.ccb_h.target_lun = targetLun;
		if (ioctl(passFd, CAMIOCOMMAND, &ccb) == 0 && (ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP)
			ident = text(ccb.cgd.inq_data.vendor, SID_VENDOR_SIZE) + " " + text(ccb.cgd.inq_data.product, SID_PRODUCT_SIZE);
		memset(&ccb, 0, sizeof(ccb));
		ccb.ccb_h.func_code = XPT_PATH_INQ;
		ccb.ccb_h.path_id = pathId;
		ccb.ccb_h.target_id = targetId;
		ccb.ccb_h.target_lun = targetLun;
		if (ioctl(passFd, CAMIOCOMMAND, &ccb) == 0 && (ccb.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP)
		{
			const std::string sim = text(ccb.cpi.dev_name, DEV_IDLEN);
			usb = ccb.cpi.transport == XPORT_USB || sim.rfind("umass", 0) == 0 ? 1 : 0;
			ident += (ident.empty() ? "" : ", ") + std::string("on ") + (sim.empty() ? "an unnamed bus" : sim)
					+ std::to_string(ccb.cpi.unit_number) + (usb == 1 ? " (USB)" : "");
		}
		// Trimmed of the spaces SCSI pads its fields with.
		while (!ident.empty() && (ident.front() == ' ' || ident.front() == ','))
			ident.erase(ident.begin());
	}

	static std::string text(const void *field, size_t size)
	{
		const char *c = static_cast<const char *>(field);
		std::string out;
		for (size_t i = 0; i < size && c[i] != '\0'; i++)
			out += (c[i] >= 0x20 && c[i] < 0x7F) ? c[i] : '?';
		while (!out.empty() && out.back() == ' ')
			out.pop_back();
		while (!out.empty() && out.front() == ' ')
			out.erase(out.begin());
		return out;
	}

	bool command(const uint8_t cdb[12], uint8_t *data, uint32_t bytes, uint8_t cdbLength = 12, bool tell = true)
	{
		// The transfer goes through a buffer that starts on a page: the kernel
		// counts a buffer's offset into its first page against the most it
		// maps for one request, so a heap buffer can be refused (E2BIG) at a
		// size an aligned one is taken at.
		uint8_t *buffer = nullptr;
		if (bytes != 0)
		{
			const size_t need = ((size_t)bytes + PageSize - 1) / PageSize * PageSize;
			if (bounce.size() < need + PageSize)
				bounce.assign(need + PageSize, 0);
			buffer = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(bounce.data()) + PageSize - 1) & ~(uintptr_t)(PageSize - 1));
		}
		union ccb ccb;
		memset(&ccb, 0, sizeof(ccb));
		ccb.ccb_h.path_id = pathId;
		ccb.ccb_h.target_id = targetId;
		ccb.ccb_h.target_lun = targetLun;
		cam_fill_csio(&ccb.csio, 1, nullptr, bytes != 0 ? CAM_DIR_IN : CAM_DIR_NONE, MSG_SIMPLE_Q_TAG, buffer, bytes,
				SSD_FULL_SIZE, cdbLength, 10000);
		memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, cdbLength);
		ccb.ccb_h.flags |= CAM_DEV_QFRZDIS;
		const int result = ioctl(passFd, CAMIOCOMMAND, &ccb);
		lastErrno = result == 0 ? 0 : errno;
		const uint32_t status = ccb.ccb_h.status & CAM_STATUS_MASK;
		// All of it, or it did not happen: a short transfer leaves old bytes.
		if (result == 0 && status == CAM_REQ_CMP && ccb.csio.resid == 0)
		{
			if (bytes != 0)
				memcpy(data, buffer, bytes);
			return true;
		}
		// Too big a transfer is the caller's to split, not a failure to tell.
		if (tell && !told && !(result != 0 && lastErrno == E2BIG))
		{
			// The first failure is told in full: the rest of a run repeats it.
			told = true;
			diag::mark("disc drive: %s: SCSI 0x%02X through %s failed: %s, CAM status 0x%x, SCSI status 0x%x, %d bytes short",
					device.c_str(), cdb[0], passName.c_str(), result == 0 ? "answered" : strerror(lastErrno), status,
					ccb.csio.scsi_status, (int)ccb.csio.resid);
		}
		return false;
	}
};

}

std::vector<std::string> deviceNames()
{
	// The names an optical drive's node has had, as Crux2 looks for them:
	// FreeBSD's cd(4) first, then what the rest of the family calls them.
	std::vector<std::string> names;
	std::string passes;
	if (DIR *dir = opendir("/dev"))
	{
		while (const dirent *entry = readdir(dir))
		{
			const std::string name = entry->d_name;
			const bool optical = name.rfind("cd", 0) == 0 || name.rfind("bd", 0) == 0 || name.rfind("dvd", 0) == 0
					|| name.rfind("acd", 0) == 0;
			// cd0, cd1...: not their partitions or control nodes.
			if (optical && name.size() > 2 && std::isdigit((unsigned char)name.back())
					&& name.find_first_of("sp.", 2) == std::string::npos)
				names.push_back("/dev/" + name);
			else if (name.rfind("pass", 0) == 0)
				passes += " " + name;
		}
		closedir(dir);
	}
	for (int i = 0; i < 4; i++)
	{
		const std::string name = "/dev/cd" + std::to_string(i);
		struct stat st;
		if (stat(name.c_str(), &st) == 0 && std::find(names.begin(), names.end(), name) == names.end())
			names.push_back(name);
	}
	std::sort(names.begin(), names.end());
	// Drives the console did not attach, read over USB by the title itself.
	for (const std::string& n : usb::deviceNames())
		names.push_back(n);
	static std::string told;
	std::string list;
	for (const std::string& n : names)
		list += " " + n;
	if (list + "|" + passes != told)
	{
		told = list + "|" + passes;
		diag::mark("disc drive: /dev has drives:%s; pass devices:%s", list.empty() ? " none" : list.c_str(),
				passes.empty() ? " none" : passes.c_str());
	}
	return names;
}

std::unique_ptr<Drive> openDevice(const std::string& device, std::string& error)
{
	if (device.rfind("/dev/ugen", 0) == 0)
		return usb::openDevice(device, error);
	// Not waiting for a disc to be ready, as Crux2 opens it: an empty drive
	// still opens.
	const int fd = ::open(device.c_str(), O_RDONLY | O_NONBLOCK);
	if (fd < 0)
	{
		error = device + ": " + strerror(errno);
		return nullptr;
	}
	return std::make_unique<CamDrive>(device, fd);
}

}
