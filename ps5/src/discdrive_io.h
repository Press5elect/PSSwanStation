/*
	PSSwanStation - a CD or DVD drive, as the disc code reads it.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	What a drive is asked: its table of contents, whole sectors with the SCSI
	command READ CD (0xBE), the 2048 bytes of a data sector when that command
	is not let through, and a reading speed (SET CD SPEED, 0xBB). On the
	console a USB drive is FreeBSD's cd(4) device /dev/cdN, and the SCSI
	commands go to its pass(4) device through CAM (ps5/drive_ps5.cpp); on the PC
	test build a disc image stands in for it (discdrive.cpp).

	The commands and the order the read modes are tried in follow DuckStation's
	CD-ROM device reader as it was under the GPL (src/util/cd_image_device.cpp,
	commit 7f4e5d55^, July 2021 to May 2024, Connor McLaughlin), written again
	for FreeBSD's CAM; nothing of DuckStation after its change of licence.
*/
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fe::discdrive
{

constexpr uint32_t RawSector = 2352;
constexpr uint32_t DataSector = 2048;
constexpr uint32_t SubQSize = 16;		// READ CD's formatted sub-channel Q
constexpr uint32_t SubRawSize = 96;		// READ CD's raw P-W sub-channels, interleaved

// One entry of the table of contents; track 0xAA is the lead-out.
struct TocEntry
{
	uint8_t track = 0;
	uint8_t control = 0;	// bit 2: a data track
	uint8_t adr = 1;
	uint32_t lba = 0;		// the drive's address: 0 is where track 1 begins
};

// The sub-channel READ CD is asked for with each sector.
enum class Sub { None, Q, Raw };

constexpr uint32_t subSize(Sub sub)
{
	return sub == Sub::Q ? SubQSize : sub == Sub::Raw ? SubRawSize : 0;
}

class Drive
{
public:
	virtual ~Drive() = default;
	// The tracks and the lead-out. False with no disc, or one the drive cannot read.
	virtual bool readToc(std::vector<TocEntry>& out, std::string& error) = 0;
	// `count` sectors from `lba`, each RawSector bytes and then the sub-channel
	// asked for, into `out`. False when the command fails or is not let through.
	// `audio`: the sectors are CD audio by the table of contents, and are asked
	// for as such (READ CD's expected sector type CD-DA, user data only): the
	// surest way to a drive's audio, where "any type" with every field asked
	// for has given some drives' silence or an error. A failure of that is
	// not told in the boot log (the caller asks again as "any type").
	virtual bool readCd(uint32_t lba, uint32_t count, Sub sub, uint8_t *out, bool audio = false) = 0;
	// The user data of `count` data sectors (DataSector each).
	virtual bool readData(uint32_t lba, uint32_t count, uint8_t *out) = 0;
	// Kilobytes a second; 0xFFFF for as fast as the drive goes.
	virtual bool setSpeed(unsigned kilobytesPerSecond) = 0;
	// What the commands go through, for the boot log ("pass1", "no pass device").
	virtual std::string route() const = 0;
	// Lets the drive's button open the tray again (the console locks it when it
	// starts with a disc in a USB drive), and opens the tray.
	virtual bool allowRemoval(std::string& how) = 0;
	virtual bool eject(std::string& how) = 0;
	// Whether the drive is on USB, as its bus says: 1 it is, 0 it is not (the
	// console's own drive), -1 unknown (no pass device to ask).
	virtual int onUsb() const { return -1; }
	// The drive's maker and model and its bus, for the boot log; empty when unknown.
	virtual std::string identity() const { return std::string(); }
};

// The drives there are now (/dev/cd0, /dev/cd1...).
std::vector<std::string> deviceNames();
// Opens one; null with `error` set when it cannot be.
std::unique_ptr<Drive> openDevice(const std::string& device, std::string& error);

}
