/*
	PSSwanStation - a PlayStation disc in a USB CD or DVD drive.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The drive (discdrive_io.h) is read in the best way it allows, tried in
	this order at the start of track 1 and checked there:

	  1. whole sectors with the sub-channel Q (READ CD, Q formatted), the Q's
	     checksum and its position on the disc both right;
	  2. whole sectors with the raw sub-channels, Q taken out of them;
	  3. whole sectors alone (the sync pattern right): the Q is made up from
	     the table of contents, so LibCrypt-protected discs do not run;
	  4. 2048 bytes of each data sector: the rest of a sector is made up
	     around them (Mode 2 Form 1), so a game's videos and XA sound, which
	     are Form 2 sectors, and its CD music tracks are lost.

	The order and the checks follow DuckStation's CD-ROM device reader as it
	was under the GPL (commit 7f4e5d55^); the sector building in 4 follows
	5S2CD (GPL-3.0), which proved on the console that cd(4) reads PlayStation
	CDs 2048 bytes at a time.

	Three ways to play, asked before the game starts:

	  - a copy to a folder: the disc is read into memory, then written as a
	    .bin and .cue (and an .sbi with the sectors whose Q is not what it
	    should be, which is what LibCrypt checks) to the games folder, a USB
	    drive, the SMB share or the NFS share; the copy is then played;
	  - a copy in memory: the disc is read into memory and played from there
	    (driveram://), and the memory given back when the game ends;
	  - from the drive (drive://): sectors are read as the game asks, a
	    thread of its own reading ahead of it 32 sectors at a time.

	The core opens those two kinds of path through CDImage's open hook. On the
	PC test build a disc image stands in for the drive: SWANSTATION_FAKE_DRIVE
	names it, and SWANSTATION_FAKE_DRIVE_MODE=data|raw takes away what a
	weaker drive (or a console that refuses the SCSI commands) does not give.
*/
#include "fe.h"
#include "discdrive_io.h"

#include "common/cd_image.h"
#include "common/error.h"
#include "core/system.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <new>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#if defined(SWANSTATION_PS5)
#include <pthread.h>
extern "C" int scePthreadSetprio(pthread_t thread, int priority);
#endif

namespace fe::discdrive
{
namespace
{

constexpr uint32_t Pregap = 150;		// track 1's two seconds before the drive's address 0
// Sectors a SCSI command reads at once: 26 x 2448 bytes (with the raw
// sub-channels) stays under the 64 KiB a USB bus moves in one transfer.
constexpr uint32_t BlockSectors = 26;	// read at once while streaming
constexpr uint32_t ChunkSectors = 26;	// read at once while copying

uint8_t bcd(uint32_t value)
{
	return (uint8_t)(((value / 10) << 4) | (value % 10));
}

void msfOf(uint32_t lba, uint8_t& m, uint8_t& s, uint8_t& f)
{
	m = (uint8_t)(lba / (75 * 60));
	s = (uint8_t)((lba / 75) % 60);
	f = (uint8_t)(lba % 75);
}

// ------------------------------------------------------------- the reading

enum class Mode { SubQ, SubRaw, Raw, Data };

const char *modeText(Mode mode)
{
	switch (mode)
	{
	case Mode::SubQ: return "whole sectors with their sub-channel Q";
	case Mode::SubRaw: return "whole sectors with their raw sub-channels";
	case Mode::Raw: return "whole sectors, no sub-channel (LibCrypt discs will not run)";
	default: return "2048 bytes a sector (videos, XA sound and CD music are lost)";
	}
}

bool hasQ(Mode mode)
{
	return mode == Mode::SubQ || mode == Mode::SubRaw;
}

bool fullSectors(Mode mode)
{
	return mode != Mode::Data;
}

// Q out of the 96 interleaved bytes of P-W: bit 6 of each byte, eight to a byte.
void deinterleaveQ(const uint8_t *raw, uint8_t *q)
{
	for (int i = 0; i < 12; i++)
	{
		uint8_t value = 0;
		for (int b = 0; b < 8; b++)
			value = (uint8_t)((value << 1) | ((raw[i * 8 + b] >> 6) & 1));
		q[i] = value;
	}
}

// The Q says where it is (not a catalogue number or an ISRC, ADR 2 and 3).
// Its CRC is not asked for: some drives give the Q with the CRC left zero,
// and the emulator wants a CRC that matches (makeCrc gives it one, as the
// .sbi files of LibCrypt discs do).
bool qIsRight(const uint8_t *q12, uint32_t lba)
{
	CDImage::SubChannelQ subq;
	std::memcpy(subq.data.data(), q12, 12);
	const uint8_t adr = subq.control_bits & 0x0F;
	if (adr == 2 || adr == 3)
		return false;
	uint8_t m, s, f;
	msfOf(lba + Pregap, m, s, f);
	return subq.absolute_minute_bcd == bcd(m) && subq.absolute_second_bcd == bcd(s) && subq.absolute_frame_bcd == bcd(f);
}

// The CRC of a Q's ten bytes written over its last two, when it is not right
// already (a drive that gives it zero, or altered as LibCrypt alters it: the
// emulator's .sbi handling makes those sectors' CRCs good too, since the
// games check the position, not the CRC). Done as a Q goes to the emulator;
// what the drive gave is kept as it is, for the .sbi of a copy.
// A Q that is not even made of BCD digits (a drive's misread) is left with
// its wrong CRC, which the emulator ignores as a PlayStation would.
void makeCrc(uint8_t *q12)
{
	CDImage::SubChannelQ subq;
	std::memcpy(subq.data.data(), q12, 12);
	if (subq.IsCRCValid())
		return;
	for (int i = 1; i < 10; i++)
		if (i != 6 && ((q12[i] & 0x0F) > 9 || (q12[i] >> 4) > 9))
			return;
	const uint16_t crc = CDImage::SubChannelQ::ComputeCRC(subq.data);
	std::memcpy(q12 + 10, &crc, 2);
}

bool syncIsRight(const uint8_t *sector)
{
	static const uint8_t sync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
	return std::memcmp(sector, sync, sizeof(sync)) == 0;
}

// A whole data sector made up around its 2048 bytes: sync, header (its
// position, mode 2), the XA subheader of Form 1 twice; EDC and ECC left 0.
void buildDataSector(uint32_t lba, const uint8_t *data, uint8_t *out)
{
	std::memset(out, 0, RawSector);
	out[0] = 0x00;
	std::memset(out + 1, 0xFF, 10);
	out[11] = 0x00;
	uint8_t m, s, f;
	msfOf(lba + Pregap, m, s, f);
	out[12] = bcd(m);
	out[13] = bcd(s);
	out[14] = bcd(f);
	out[15] = 0x02;
	out[18] = out[22] = 0x08;	// submode: data
	std::memcpy(out + 24, data, DataSector);
}

// A drive and the way it is read.
struct Reader
{
	std::string device;
	std::unique_ptr<Drive> drive;
	std::vector<TocEntry> toc;	// the tracks, then the lead-out
	Mode mode = Mode::Data;
	uint32_t sectors = 0;		// the lead-out's address
	// Set by whoever no longer wants the reading (a game closing, Cancel):
	// retries stop at once.
	const std::atomic<bool> *abort = nullptr;

	bool aborted() const { return abort != nullptr && abort->load(); }

	bool open(const std::string& name, std::string& error)
	{
		device = name;
		drive = openDevice(name, error);
		if (!drive || !drive->readToc(toc, error))
			return false;
		sectors = toc.back().lba;
		if (sectors == 0 || sectors > 450000)
		{
			error = "the disc is not a CD";
			return false;
		}
		return chooseMode(error);
	}

	bool isData(uint32_t lba) const
	{
		for (size_t i = toc.size() - 1; i-- > 0;)
			if (lba >= toc[i].lba)
				return (toc[i].control & 0x04) != 0;
		return true;
	}

	bool chooseMode(std::string& error)
	{
		const uint32_t first = toc.front().lba;
		const bool data = (toc.front().control & 0x04) != 0;
		std::vector<uint8_t> buffer(RawSector + SubRawSize);
		// A few sectors: one Q frame in a hundred may be a catalogue number or
		// an ISRC, which has no position to check.
		auto qGiven = [&](Sub sub) {
			for (uint32_t k = 0; k < 4 && first + k < sectors; k++)
			{
				if (!drive->readCd(first + k, 1, sub, buffer.data()))
					return false;
				uint8_t q[12];
				if (sub == Sub::Raw)
					deinterleaveQ(buffer.data() + RawSector, q);
				else
					std::memcpy(q, buffer.data() + RawSector, 12);
				if (qIsRight(q, first + k) && (!data || syncIsRight(buffer.data())))
					return true;
			}
			return false;
		};
		if (qGiven(Sub::Q))
			mode = Mode::SubQ;
		else if (qGiven(Sub::Raw))
			mode = Mode::SubRaw;
		if (!hasQ(mode) && drive->readCd(first, 1, Sub::None, buffer.data()) && (!data || syncIsRight(buffer.data())))
			mode = Mode::Raw;
		else if (!hasQ(mode))
		{
			if (!data || !drive->readData(first + 16, 1, buffer.data()))
			{
				error = "the drive did not give the disc's sectors in any way";
				return false;
			}
			mode = Mode::Data;
		}
		diag::mark("disc drive: %s: %u sectors, %d track(s), read as %s (commands through %s)", device.c_str(),
				sectors, (int)toc.size() - 1, modeText(mode), drive->route().c_str());
		return true;
	}

	// `count` sectors from `lba`: RawSector bytes each into raw, the Q's 12
	// bytes each into q (when the mode has it and q is not null).
	bool read(uint32_t lba, uint32_t count, uint8_t *raw, uint8_t *q)
	{
		if (lba + count > sectors)
			return false;
		for (int attempt = 0; attempt < 3 && !aborted(); attempt++)
			if (readOnce(lba, count, raw, q))
				return true;
		// One at a time, so a scratch costs only the sectors under it; given
		// up at the first sector that will not read, so a scratched stretch
		// does not hold the game for minutes.
		if (count > 1)
		{
			for (uint32_t i = 0; i < count; i++)
				if (aborted() || !readOnce(lba + i, 1, raw + (size_t)i * RawSector, q != nullptr ? q + (size_t)i * 12 : nullptr))
					return false;
			return true;
		}
		return false;
	}

	bool readOnce(uint32_t lba, uint32_t count, uint8_t *raw, uint8_t *q)
	{
		if (mode == Mode::Data)
		{
			// Data sectors 2048 bytes each, made whole; audio sectors are silence.
			std::vector<uint8_t> data((size_t)count * DataSector);
			uint32_t i = 0;
			while (i < count)
			{
				uint32_t run = 1;
				const bool d = isData(lba + i);
				while (i + run < count && isData(lba + i + run) == d)
					run++;
				if (d)
				{
					// Many drives refuse a Form 2 sector (a video's, XA sound's)
					// to a 2048-byte read: that one is lost, the rest are read.
					if (!drive->readData(lba + i, run, data.data() + (size_t)i * DataSector))
						for (uint32_t k = 0; k < run; k++)
							if (!drive->readData(lba + i + k, 1, data.data() + (size_t)(i + k) * DataSector))
								std::memset(data.data() + (size_t)(i + k) * DataSector, 0, DataSector);
					for (uint32_t k = 0; k < run; k++)
						buildDataSector(lba + i + k, data.data() + (size_t)(i + k) * DataSector,
								raw + (size_t)(i + k) * RawSector);
				}
				else
					std::memset(raw + (size_t)i * RawSector, 0, (size_t)run * RawSector);
				i += run;
			}
			return true;
		}
		const Sub sub = mode == Mode::SubQ ? Sub::Q : mode == Mode::SubRaw ? Sub::Raw : Sub::None;
		const uint32_t each = RawSector + subSize(sub);
		std::vector<uint8_t> buffer((size_t)count * each);
		// Data and audio in runs of their own, as the table of contents has
		// them: audio asked for as CD audio (and as "any type" if the drive
		// refuses that: a table that puts the border elsewhere than the disc).
		uint32_t i = 0;
		while (i < count)
		{
			uint32_t run = 1;
			const bool d = isData(lba + i);
			while (i + run < count && isData(lba + i + run) == d)
				run++;
			uint8_t *at = buffer.data() + (size_t)i * each;
			if (!(!d && drive->readCd(lba + i, run, sub, at, true)) && !drive->readCd(lba + i, run, sub, at, false))
				return false;
			i += run;
		}
		for (uint32_t i = 0; i < count; i++)
		{
			const uint8_t *sector = buffer.data() + (size_t)i * each;
			std::memcpy(raw + (size_t)i * RawSector, sector, RawSector);
			if (q != nullptr && sub == Sub::Q)
				std::memcpy(q + (size_t)i * 12, sector + RawSector, 12);
			else if (q != nullptr && sub == Sub::Raw)
				deinterleaveQ(sector + RawSector, q + (size_t)i * 12);
		}
		return true;
	}
};

// ------------------------------------------------- the disc as an image

// The table of contents as the emulator keeps it: track 1's two seconds of
// pregap before it, each track's sectors at their place on the drive.
class DriveImage : public CDImage
{
public:
	DriveImage() : CDImage(OpenFlags::None) {}
	// A drive is read as the game asks or copied into memory by the title
	// itself; the emulator's own copy into memory would lose the sub-channel.
	bool AllowsPreload() const override { return false; }

protected:
	// The index a position on the disc is in; only reads the table, so any
	// thread may ask.
	const Index *indexAt(LBA lba) const
	{
		for (const Index& index : m_indices)
			if (lba >= index.start_lba_on_disc && lba - index.start_lba_on_disc < index.length)
				return &index;
		return nullptr;
	}

	void buildToc(const std::vector<TocEntry>& toc, const std::string& name)
	{
		m_filename = name;
		LBA disc = 0;
		for (size_t i = 0; i + 1 < toc.size(); i++)
		{
			const TocEntry& e = toc[i];
			SubChannelQ::Control control{};
			control.bits = (uint8_t)((e.adr & 0x0F) | (e.control << 4));
			const TrackMode mode = (e.control & 0x04) != 0 ? TrackMode::Mode2Raw : TrackMode::Audio;
			const uint32_t length = toc[i + 1].lba - e.lba;
			if (i == 0)
			{
				Index pregap{};
				pregap.start_lba_on_disc = disc;
				pregap.start_lba_in_track = static_cast<LBA>(-static_cast<int32_t>(Pregap));
				pregap.length = Pregap;
				pregap.track_number = e.track;
				pregap.index_number = 0;
				pregap.mode = mode;
				pregap.control.bits = control.bits;
				pregap.is_pregap = true;
				m_indices.push_back(pregap);
				disc += Pregap;
			}
			Track track{};
			track.track_number = e.track;
			track.start_lba = disc;
			track.first_index = static_cast<uint32_t>(m_indices.size());
			track.length = length;
			track.mode = mode;
			track.control.bits = control.bits;
			m_tracks.push_back(track);
			Index index{};
			index.file_offset = e.lba;
			index.file_index = 0;
			index.file_sector_size = RawSector;
			index.start_lba_on_disc = disc;
			index.track_number = e.track;
			index.index_number = 1;
			index.start_lba_in_track = 0;
			index.length = length;
			index.mode = mode;
			index.control.bits = control.bits;
			index.is_pregap = false;
			m_indices.push_back(index);
			disc += length;
		}
		AddLeadOutIndex();
		m_lba_count = disc;
	}
};

// Streaming: sectors as the game asks for them, read ahead by a thread of
// its own, which alone talks to the drive.
class Stream
{
public:
	Reader reader;

	bool open(const std::string& device, std::string& error)
	{
		reader.abort = &stopping;
		if (!reader.open(device, error))
			return false;
		// As fast as the drive goes: the whole disc is read into memory behind
		// the game, and the sooner that is done the sooner the drive rests.
		reader.drive->setSpeed(0xFFFF);
		started = now();
		worker = std::thread([this] { run(); });
#if defined(SWANSTATION_PS5)
		// The reading is the console's least pressing work: the emulator's,
		// the sound's and the graphics driver's threads come first.
		const int r = scePthreadSetprio(worker.native_handle(), 767);
		diag::mark("disc drive: the reading thread's priority set to the lowest (%d)", r);
#endif
		return true;
	}

	~Stream()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		wake.notify_all();
		if (worker.joinable())
			worker.join();
		if (asks > 0)
			diag::mark("disc drive: %llu of %u blocks were read into memory (%s); reads waited for the drive %llu time(s), "
					"%.0f ms at most, %.1f s in all (the emulated drive waits with them; the game goes on)",
					(unsigned long long)blocks.size(), lastBlock, allRead ? "the whole disc" : "the game closed before the rest",
					(unsigned long long)waits, longestWait * 1000.0, waited);
	}

	bool get(uint32_t lba, uint8_t *raw, uint8_t *q)
	{
		std::unique_lock<std::mutex> lock(mutex);
		const uint32_t block = lba / BlockSectors;
		// A block the reading could not read is read again for this ask.
		failed.erase(block);
		if (want != block)
		{
			want = block;
			wake.notify_all();
		}
		asks++;
		if (blocks.count(block) == 0)
		{
			// Not read yet: this read waits for the drive (the emulated drive
			// waits with it, the game goes on). Counted, and told.
			const double from = now();
			arrived.wait(lock, [&] { return blocks.count(block) != 0 || failed.count(block) != 0 || stopping; });
			const double took = now() - from;
			if (took > 0.040)
			{
				waits++;
				waited += took;
				longestWait = std::max(longestWait, took);
				if (from - lastTold > 10.0)
				{
					lastTold = from;
					diag::mark("disc drive: a read waited %.0f ms for sector %u (%u blocks were ready ahead of it)", took * 1000.0,
							lba, aheadOf(block));
				}
			}
		}
		auto it = blocks.find(block);
		if (it == blocks.end())
			return false;
		const uint32_t at = lba - block * BlockSectors;
		if (raw != nullptr)
			std::memcpy(raw, it->second.raw.data() + (size_t)at * RawSector, RawSector);
		if (q != nullptr)
			std::memcpy(q, it->second.q.data() + (size_t)at * 12, 12);
		return true;
	}

	// A sector's Q if its block is in already: never waits, never moves the
	// reading (the emulated drive asking where its head is).
	bool peekQ(uint32_t lba, uint8_t *q)
	{
		std::lock_guard<std::mutex> lock(mutex);
		const uint32_t block = lba / BlockSectors;
		auto it = blocks.find(block);
		if (it == blocks.end())
			return false;
		std::memcpy(q, it->second.q.data() + (size_t)(lba - block * BlockSectors) * 12, 12);
		return true;
	}

private:
	struct Block
	{
		std::vector<uint8_t> raw, q;
	};
	std::mutex mutex;
	std::condition_variable wake, arrived;
	std::map<uint32_t, Block> blocks;	// every block read, for as long as the game runs
	std::map<uint32_t, int> failed;
	uint32_t want = 0;
	uint32_t lastBlock = 0;
	bool allRead = false;
	std::atomic<bool> stopping{ false };
	std::thread worker;
	// What the game is about to read comes first: the first block missing
	// from the sixteen seconds ahead of it (96 blocks of 26 sectors; a
	// PlayStation reads 150 sectors a second). Then the rest of the disc, in
	// order from its start, as fast as the drive goes, until all of it is in
	// memory and the drive has nothing more to do.
	static constexpr uint32_t Ahead = 96;
	// Blocks of each track's start read early: three blocks, about half a
	// second of CD music, enough to cover the drive's seek to the rest.
	static constexpr uint32_t TrackStartBlocks = 3;
	std::vector<uint32_t> trackStarts;
	uint64_t asks = 0, waits = 0;
	double waited = 0, longestWait = 0, lastTold = -100, started = 0;
	int audioBlocksSeen = 0;
	bool audioTold = false;

	uint32_t aheadOf(uint32_t block) const
	{
		uint32_t n = 0;
		while (blocks.count(block + 1 + n) != 0)
			n++;
		return n;
	}

	// Whether the drive gives sound for the disc's audio sectors, told once
	// (the first twenty audio blocks may be a pregap's silence).
	void noteAudio(uint32_t lba, const Block& block)
	{
		if (audioTold || reader.isData(lba))
			return;
		bool sound = false;
		for (size_t i = 0; i < block.raw.size() && !sound; i += 2)
			sound = block.raw[i] != 0 || block.raw[i + 1] != 0;
		audioBlocksSeen++;
		if (sound)
		{
			audioTold = true;
			diag::mark("disc drive: the drive gives the disc's CD audio (sound in the audio sectors from %u)", lba);
		}
		else if (audioBlocksSeen >= 20)
		{
			audioTold = true;
			diag::mark("disc drive: the drive gives silence for the disc's CD audio (%d audio blocks of zeros, up to sector %u)",
					audioBlocksSeen, lba);
		}
	}

	void run()
	{
		std::unique_lock<std::mutex> lock(mutex);
		lastBlock = (reader.sectors + BlockSectors - 1) / BlockSectors;
		for (size_t t = 0; t + 1 < reader.toc.size(); t++)
			for (uint32_t k = 0; k < TrackStartBlocks; k++)
			{
				const uint32_t b = reader.toc[t].lba / BlockSectors + k;
				if (b < lastBlock && reader.toc[t].lba > 0)
					trackStarts.push_back(b);
			}
		uint32_t sweep = 0;	// the rest of the disc is read from here on
		while (!stopping)
		{
			// The block the game waits for, or the first missing ahead of it;
			// else the next of the sweep.
			uint32_t next = UINT32_MAX;
			for (uint32_t b = want; b < std::min(want + Ahead + 1, lastBlock); b++)
				if (blocks.count(b) == 0 && failed.count(b) == 0)
				{
					next = b;
					break;
				}
			// Then the start of every track (a few seconds of each), so a game
			// that starts CD music or jumps to a track does not wait for the
			// drive's seek; then the rest of the disc in order.
			if (next == UINT32_MAX)
				for (uint32_t b : trackStarts)
					if (blocks.count(b) == 0 && failed.count(b) == 0)
					{
						next = b;
						break;
					}
			if (next == UINT32_MAX)
			{
				while (sweep < lastBlock && (blocks.count(sweep) != 0 || failed.count(sweep) != 0))
					sweep++;
				if (sweep < lastBlock)
					next = sweep;
			}
			if (next == UINT32_MAX)
			{
				if (!allRead)
				{
					allRead = true;
					diag::mark("disc drive: the whole disc is in memory (%u blocks, %llu MB) after %.0f s; the drive has "
							"nothing more to read", lastBlock, (unsigned long long)(blocks.size() * BlockSectors * RawSector / 1000000),
							now() - started);
				}
				wake.wait(lock);
				continue;
			}
			lock.unlock();
			const uint32_t lba = next * BlockSectors;
			const uint32_t count = std::min(BlockSectors, reader.sectors - lba);
			Block block;
			block.raw.assign((size_t)BlockSectors * RawSector, 0);
			block.q.assign((size_t)BlockSectors * 12, 0);
			const bool ok = reader.read(lba, count, block.raw.data(), hasQ(reader.mode) ? block.q.data() : nullptr);
			lock.lock();
			if (ok)
			{
				noteAudio(lba, block);
				blocks[next] = std::move(block);
			}
			else
			{
				failed[next] = 1;
				diag::mark("disc drive: sectors %u to %u could not be read", lba, lba + count - 1);
			}
			arrived.notify_all();
		}
		arrived.notify_all();
	}
};

class StreamImage final : public DriveImage
{
public:
	bool open(const std::string& device, const std::string& name, Common::Error *error)
	{
		std::string why;
		stream = std::make_unique<Stream>();
		if (!stream->open(device, why))
		{
			if (error != nullptr)
				error->SetFormattedMessage("The disc in the drive could not be read: %s.", why.c_str());
			return false;
		}
		buildToc(stream->reader.toc, name);
		return Seek(1, Position{ 0, 0, 0 });
	}

	bool ReadSubChannelQ(SubChannelQ *subq, const Index& index, LBA lbaInIndex) override
	{
		if (index.file_sector_size == 0 || !hasQ(stream->reader.mode))
			return CDImage::ReadSubChannelQ(subq, index, lbaInIndex);
		if (!stream->get(static_cast<uint32_t>(index.file_offset) + lbaInIndex, nullptr, subq->data.data()))
			return false;
		makeCrc(subq->data.data());
		return true;
	}

	bool HasNonStandardSubchannel() const override { return hasQ(stream->reader.mode); }
	Mode readMode() const { return stream->reader.mode; }

	// A real drive: its seeks take a few hundred milliseconds, which the
	// emulated drive takes too, rather than the emulator stopping for them.
	bool ReadsSlowly() const override { return true; }

	bool PeekSubChannelQ(LBA lba, SubChannelQ *subq) const override
	{
		const Index *index = indexAt(lba);
		if (index == nullptr)
			return false;
		const LBA offset = lba - index->start_lba_on_disc;
		if (index->file_sector_size == 0 || !hasQ(stream->reader.mode))
		{
			// Made from the table of contents, as every read of it is.
			const_cast<StreamImage *>(this)->GenerateSubChannelQ(subq, *index, offset);
			return true;
		}
		if (!stream->peekQ(static_cast<uint32_t>(index->file_offset) + offset, subq->data.data()))
		{
			// Not read yet: the position from the table of contents. Waiting
			// for the drive here would stop the whole emulator (test 9: every
			// new CD music track, while the game asked where the head was
			// during the drive's seek to it). A LibCrypt check reads its own
			// sectors, which are in memory by then.
			const_cast<StreamImage *>(this)->GenerateSubChannelQ(subq, *index, offset);
			return true;
		}
		makeCrc(subq->data.data());
		return true;
	}

protected:
	bool ReadSectorFromIndex(void *buffer, const Index& index, LBA lbaInIndex) override
	{
		if (index.file_sector_size == 0)
			return false;
		return stream->get(static_cast<uint32_t>(index.file_offset) + lbaInIndex, static_cast<uint8_t *>(buffer), nullptr);
	}

private:
	std::unique_ptr<Stream> stream;
};

// A whole disc read into memory.
struct RamDisc
{
	std::string device;
	std::vector<TocEntry> toc;
	Mode mode = Mode::Data;
	uint32_t sectors = 0;
	std::vector<uint8_t> raw, q;	// q: 12 bytes a sector, when the mode has it
};

class RamImage final : public DriveImage
{
public:
	explicit RamImage(std::shared_ptr<const RamDisc> disc, const std::string& name) : disc(std::move(disc))
	{
		buildToc(this->disc->toc, name);
		Seek(1, Position{ 0, 0, 0 });
	}

	bool ReadSubChannelQ(SubChannelQ *subq, const Index& index, LBA lbaInIndex) override
	{
		if (index.file_sector_size == 0 || disc->q.empty())
			return CDImage::ReadSubChannelQ(subq, index, lbaInIndex);
		const uint64_t lba = index.file_offset + lbaInIndex;
		if (lba >= disc->sectors)
			return false;
		std::memcpy(subq->data.data(), disc->q.data() + lba * 12, 12);
		makeCrc(subq->data.data());
		return true;
	}

	bool HasNonStandardSubchannel() const override { return !disc->q.empty(); }

protected:
	bool ReadSectorFromIndex(void *buffer, const Index& index, LBA lbaInIndex) override
	{
		const uint64_t lba = index.file_offset + lbaInIndex;
		if (index.file_sector_size == 0 || lba >= disc->sectors)
			return false;
		std::memcpy(buffer, disc->raw.data() + lba * RawSector, RawSector);
		return true;
	}

private:
	std::shared_ptr<const RamDisc> disc;
};

// ---------------------------------------------------------------- state

std::mutex stateMutex;
std::vector<Disc> present;					// the discs in the drives
std::map<std::string, std::string> signatures;	// per device: what its table of contents was
std::atomic<unsigned> discGeneration{ 1 };
std::atomic<bool> watching{ false }, playing{ false };
std::thread watcher;
// For the entry that stands in for a drive that is not found: whether
// "USB disc drive" was on when PSSwanStation started, the watcher's passes so
// far, and what /dev listed at the last one.
bool wantedAtStart = false;
std::atomic<unsigned> passes{ 0 };
std::string seen;					// under stateMutex
std::shared_ptr<const RamDisc> ramDisc;		// the copy in memory being played
// One thing at a time talks to the drives: the watcher identifying a disc,
// a copy reading one, a game's stream opening one.
std::mutex driveMutex;

std::mutex jobMutex;
std::thread jobThread;
std::atomic<int> job{ JobIdle };
std::atomic<bool> jobCancel{ false };
std::atomic<uint64_t> jobDone{ 0 }, jobTotal{ 0 };
std::string jobText, jobError, jobCopied;
int jobSource = library::Internal;

void setError(const std::string& text)
{
	std::lock_guard<std::mutex> lock(jobMutex);
	jobError = text;
}

std::string shortDevice(const std::string& device)
{
	return device.rfind("/dev/", 0) == 0 ? device.substr(5) : device;
}

std::string safeName(const std::string& name)
{
	std::string out;
	for (char c : name)
		out += std::strchr("/\\:*?\"<>|", c) != nullptr || (unsigned char)c < 32 ? ' ' : c;
	out = trim(out);
	return out.empty() ? std::string("PlayStation disc") : out;
}

// What is in a drive now: its table of contents, read again; "" when no disc.
std::string signatureOf(Drive& drive, std::string& error)
{
	std::vector<TocEntry> toc;
	if (!drive.readToc(toc, error))
		return "";
	std::string text;
	for (const TocEntry& e : toc)
		text += format("%u:%u:%u;", e.track, e.control, e.lba);
	return text;
}

// The disc's serial and name, read through the emulator's own reader.
bool identify(const std::string& device, Disc& out, std::string& why)
{
	StreamImage image;
	Common::Error error;
	if (!image.open(device, "disc", &error))
	{
		why = error.GetCodeAndMessage().GetCharArray();
		return false;
	}
	out.device = device;
	out.serial = System::GetGameCodeForImage(&image, false);
	gamedb::Info info;
	if (!out.serial.empty() && gamedb::find(out.serial, info) && !info.name.empty())
		out.name = info.name;
	else
		out.name = out.serial.empty() ? std::string("PlayStation disc") : "PlayStation disc " + out.serial;
	out.name = safeName(out.name);
	// The cover is found by the disc's Redump name, as the collection names it.
	const std::string redump = out.serial.empty() ? std::string() : gamedb::redumpName(out.serial);
	out.coverName = redump.empty() ? out.name : safeName(redump);
	out.bytes = (uint64_t)image.GetLBACount() * RawSector;
	out.tracks = (int)image.GetTrackCount();
	out.reading = modeText(image.readMode());
	out.fullSectors = fullSectors(image.readMode());
	out.playable = true;
	return true;
}

// What the library shows for a drive whose disc cannot be played (or that has
// none), so that a drive that was found is seen: chosen, it says why.
Disc driveEntry(const std::string& device, const std::string& problem, bool empty)
{
	Disc d;
	d.device = device;
	d.playable = false;
	d.problem = problem;
	d.name = empty ? "USB disc drive - no disc" : "USB disc drive - disc not readable";
	return d;
}

// The entry for "USB disc drive" when no USB drive is found: why, as far as
// PSSwanStation can tell (called with stateMutex held).
Disc notFound()
{
	Disc d;
	d.device = NoDrive;
	d.name = "USB disc drive - not found";
	d.playable = false;
	if (!wantedAtStart)
	{
		d.problem = "\"USB disc drive\" was turned on after PSSwanStation started. It takes effect the next time "
				"PSSwanStation starts: close it and start it again.";
		return d;
	}
	const std::string platformWhy = platform::drivesProblem();
	if (!watching)
	{
		d.problem = platformWhy.empty() ? std::string("PSSwanStation could not leave its sandbox, which it needs to see "
				"USB drives. Run the ELF loader on port 9021, then start PSSwanStation again.") : platformWhy;
		return d;
	}
	if (passes == 0)
	{
		d.problem = "Looking for drives...";
		return d;
	}
	d.problem = "The console lists no USB disc drive"
			+ (seen.empty() ? std::string(".") : " (it lists" + seen + ").");
	if (!platformWhy.empty())
		d.problem += " " + platformWhy;
	d.problem += " If a drive is plugged in, the console has not recognised it, and PSSwanStation could not read it "
			"over USB itself either. A full-size drive needs its own power adapter, plugged in and switched on. Try "
			"another USB port on the console and the drive's own cable, and plug it in while PSSwanStation is running: "
			"it is looked for every two seconds. The boot log's \"disc drive:\" and \"usb drive:\" lines say what the "
			"console lists and what each USB device answered.";
	return d;
}

void watch()
{
	std::map<std::string, int> tries;	// per device: identifications that failed for this disc
	std::set<std::string> unlocked;		// drives whose tray was let go
	std::set<std::string> seenOnce, failedOnce;	// drives told in the boot log
	while (watching)
	{
		if (!playing && job != JobRunning)
		{
			bool changed = false;
			std::string listedAs;
			const std::vector<std::string> devices = deviceNames();
			for (const std::string& device : devices)
			{
				// A game or a copy may have begun since the pass did.
				if (playing || job == JobRunning)
					break;
				std::unique_lock<std::mutex> busy(driveMutex);
				std::string error;
				std::unique_ptr<Drive> drive = openDevice(device, error);
				// The console's own drive (it reads no CD) by its bus when the
				// drive says, else by its name: cd0. A USB drive on a console
				// without a drive of its own may be cd0 too.
				const int usb = drive ? drive->onUsb() : -1;
				const bool own = usb == 0 || (usb < 0 && device == "/dev/cd0");
				if (drive && seenOnce.insert(device).second)
				{
					const std::string what = drive->identity();
					diag::mark("disc drive: %s: %s; %s", device.c_str(), what.empty() ? "maker and model not known" : what.c_str(),
							usb == 1 ? "a USB drive" : usb == 0 ? "the console's own drive" : own
							? "taken for the console's own drive (by its name; the bus did not say)" : "taken for a USB drive (the bus did not say)");
				}
				if (!drive && failedOnce.insert(device).second)
					diag::mark("disc drive: %s could not be opened: %s", device.c_str(), error.c_str());
				listedAs += " " + shortDevice(device) + (own ? " (the console's own drive)" : drive ? "" : " (could not be opened)");
				// A console started with a disc in a USB drive locks the tray:
				// it is let go the first time a drive is seen.
				if (drive && unlocked.insert(device).second)
				{
					std::string how;
					const bool ok = drive->allowRemoval(how);
					diag::mark("disc drive: %s: the tray %s (%s)", device.c_str(), ok ? "is unlocked" : "could not be unlocked", how.c_str());
				}
				const std::string signature = drive ? signatureOf(*drive, error) : std::string();
				drive.reset();
				// "-": looked at, no disc; "": not looked at yet.
				const std::string now_ = signature.empty() ? std::string("-") : signature;
				std::string before;
				{
					std::lock_guard<std::mutex> lock(stateMutex);
					before = signatures[device];
				}
				if (now_ == before)
					continue;
				Disc disc;
				std::string why;
				const bool known = !signature.empty() && identify(device, disc, why);
				busy.unlock();
				if (!signature.empty() && !known && ++tries[device] < 5)
					continue;	// a drive still spinning up: asked again at the next pass
				tries.erase(device);
				// The console's own drive reads no CD: it is listed only with a
				// disc that plays. A USB drive is listed whatever is in it.
				bool listed = known;
				if (known)
					diag::mark("disc drive: %s: %s (%s) went in", device.c_str(), disc.name.c_str(),
							disc.serial.empty() ? "no serial" : disc.serial.c_str());
				else if (!signature.empty())
				{
					diag::mark("disc drive: %s: a disc is in, but it could not be read as a PlayStation disc: %s",
							device.c_str(), why.c_str());
					disc = driveEntry(device, "A disc is in the drive, but it could not be read as a PlayStation disc: " + why
							+ " PlayStation discs are CDs; the console's own drive cannot read them.", false);
					listed = !own;
				}
				else
				{
					if (!before.empty() && before != "-")
						diag::mark("disc drive: %s: the disc came out", device.c_str());
					else
						diag::mark("disc drive: %s: no disc in it (%s)", device.c_str(), error.empty() ? "nothing to read" : error.c_str());
					disc = driveEntry(device, "The drive is there, with no disc it can read in it. Put a PlayStation disc in: "
							"it is read within a few seconds.", true);
					listed = !own;
				}
				std::lock_guard<std::mutex> lock(stateMutex);
				signatures[device] = now_;
				present.erase(std::remove_if(present.begin(), present.end(),
						[&](const Disc& d) { return d.device == device; }), present.end());
				if (listed)
					present.push_back(disc);
				changed = true;
			}
			// A drive that was unplugged: its disc goes with it.
			{
				std::lock_guard<std::mutex> lock(stateMutex);
				for (auto it = signatures.begin(); it != signatures.end();)
				{
					if (std::find(devices.begin(), devices.end(), it->first) != devices.end())
					{
						++it;
						continue;
					}
					if (!it->second.empty())
						diag::mark("disc drive: %s was unplugged", it->first.c_str());
					const std::string gone = it->first;
					present.erase(std::remove_if(present.begin(), present.end(),
							[&](const Disc& d) { return d.device == gone; }), present.end());
					it = signatures.erase(it);
					changed = true;
				}
			}
			{
				std::lock_guard<std::mutex> lock(stateMutex);
				seen = listedAs;
			}
			// The first pass: the entry for a drive not found says so now.
			if (passes++ == 0)
				changed = true;
			if (changed)
			{
				discGeneration++;
				library::scan(library::Optical, true);
			}
		}
		for (int i = 0; i < 20 && watching; i++)
			usleep(100000);
	}
}

// ------------------------------------------------------------ the copies

bool readWhole(const Disc& disc, std::shared_ptr<RamDisc>& out)
{
	std::lock_guard<std::mutex> busy(driveMutex);
	Reader reader;
	reader.abort = &jobCancel;
	std::string why;
	if (!reader.open(disc.device, why))
	{
		setError("The disc could not be read: " + why + ".");
		return false;
	}
	reader.drive->setSpeed(0xFFFF);
	auto ram = std::make_shared<RamDisc>();
	ram->device = disc.device;
	ram->toc = reader.toc;
	ram->mode = reader.mode;
	ram->sectors = reader.sectors;
	try
	{
		ram->raw.resize((size_t)reader.sectors * RawSector);
		if (hasQ(reader.mode))
			ram->q.resize((size_t)reader.sectors * 12);
	}
	catch (const std::bad_alloc&)
	{
		setError(format("There is not enough memory for the disc (%u MB).", (unsigned)((uint64_t)reader.sectors * RawSector >> 20)));
		return false;
	}
	jobTotal = (uint64_t)reader.sectors * RawSector;
	const double began = now();
	for (uint32_t lba = 0; lba < reader.sectors; lba += ChunkSectors)
	{
		if (jobCancel)
			return false;
		const uint32_t count = std::min(ChunkSectors, reader.sectors - lba);
		if (!reader.read(lba, count, ram->raw.data() + (size_t)lba * RawSector,
				ram->q.empty() ? nullptr : ram->q.data() + (size_t)lba * 12))
		{
			setError(format("Sectors %u to %u of the disc could not be read (a scratch, or dirt?).", lba, lba + count - 1));
			return false;
		}
		jobDone = (uint64_t)(lba + count) * RawSector;
	}
	diag::mark("disc drive: %s read into memory in %.1f s (%u MB)", disc.device.c_str(), now() - began,
			(unsigned)(jobTotal.load() >> 20));
	out = std::move(ram);
	return true;
}

// The .cue of the copy: each track at its place in the .bin; INDEX 00 where
// the sub-channel says a track's pregap begins.
std::string cueOf(const RamDisc& disc, const std::string& binName)
{
	std::string cue = "FILE \"" + binName + "\" BINARY\n";
	for (size_t i = 0; i + 1 < disc.toc.size(); i++)
	{
		const TocEntry& e = disc.toc[i];
		const bool data = (e.control & 0x04) != 0;
		cue += format("  TRACK %02u %s\n", e.track, data ? "MODE2/2352" : "AUDIO");
		if (i > 0 && !disc.q.empty())
		{
			// Back from the track's start while the Q says this track, index 0.
			// Frames that do not give a position (a catalogue number, an
			// ISRC: ADR 2 or 3) are passed over, as are bad ones.
			uint32_t start = e.lba, misses = 0;
			for (uint32_t at = e.lba; at > disc.toc[i - 1].lba && misses < 8; at--)
			{
				CDImage::SubChannelQ subq;
				std::memcpy(subq.data.data(), disc.q.data() + (size_t)(at - 1) * 12, 12);
				const int adr = subq.control_bits & 0x0F;
				if (!subq.IsCRCValid() || adr == 2 || adr == 3)
				{
					misses++;
					continue;
				}
				if (subq.track_number_bcd != bcd(e.track) || subq.index_number_bcd != 0)
					break;
				start = at - 1;
				misses = 0;
			}
			if (start < e.lba)
			{
				uint8_t m, s, f;
				msfOf(start, m, s, f);
				cue += format("    INDEX 00 %02u:%02u:%02u\n", m, s, f);
			}
			else
				cue += "    PREGAP 00:00:00\n";
		}
		else if (i > 0)
		{
			// No sub-channel to say where the pregap begins: it is in the .bin,
			// at the end of the track before, and none is to be added.
			cue += "    PREGAP 00:00:00\n";
		}
		uint8_t m, s, f;
		msfOf(e.lba, m, s, f);
		cue += format("    INDEX 01 %02u:%02u:%02u\n", m, s, f);
	}
	return cue;
}

// The .sbi: each sector whose Q's checksum is wrong (LibCrypt's), its
// position on the disc and its first ten bytes. Empty when there are none or
// so many that they are read errors, not protection.
std::vector<uint8_t> sbiOf(const RamDisc& disc, int& count)
{
	std::vector<uint8_t> out;
	count = 0;
	if (disc.q.empty())
		return out;
	const char header[4] = { 'S', 'B', 'I', '\0' };
	out.insert(out.end(), header, header + 4);
	// An altered sector: its CRC is not its ten bytes' (as LibCrypt leaves
	// them), or it says it is elsewhere than it is. A drive that gives every
	// CRC wrong (zero) is judged by the position alone.
	uint32_t badCrcs = 0;
	for (uint32_t lba = 0; lba < disc.sectors && badCrcs <= 512; lba++)
	{
		CDImage::SubChannelQ subq;
		std::memcpy(subq.data.data(), disc.q.data() + (size_t)lba * 12, 12);
		if (!subq.IsCRCValid())
			badCrcs++;
	}
	const bool byCrc = badCrcs <= 512;
	for (uint32_t lba = 0; lba < disc.sectors; lba++)
	{
		CDImage::SubChannelQ subq;
		std::memcpy(subq.data.data(), disc.q.data() + (size_t)lba * 12, 12);
		const uint8_t adr = subq.control_bits & 0x0F;
		uint8_t m, s, f;
		msfOf(lba + Pregap, m, s, f);
		const bool misplaced = adr != 2 && adr != 3
				&& !(subq.absolute_minute_bcd == bcd(m) && subq.absolute_second_bcd == bcd(s) && subq.absolute_frame_bcd == bcd(f));
		if ((!byCrc || subq.IsCRCValid()) && !misplaced)
			continue;
		const uint8_t entry[4] = { bcd(m), bcd(s), bcd(f), 1 };
		out.insert(out.end(), entry, entry + 4);
		out.insert(out.end(), subq.data.data(), subq.data.data() + 10);
		count++;
	}
	if (count == 0 || count > 512)
		out.clear();
	return out;
}

bool isNetwork(int destination)
{
	return destination == ToShare || destination == ToNfs;
}

bool writeLocal(const std::string& path, const uint8_t *data, size_t bytes, bool counted)
{
	const std::string part = path + ".part";
	FILE *f = fopen(part.c_str(), "wb");
	if (f == nullptr)
		return false;
	size_t done = 0;
	bool ok = true;
	while (done < bytes && ok)
	{
		if (jobCancel)
			ok = false;
		const size_t n = std::min<size_t>(bytes - done, 4 << 20);
		ok = ok && fwrite(data + done, 1, n, f) == n;
		done += n;
		if (counted)
			jobDone = done;
	}
	ok = fclose(f) == 0 && ok;
	// The name is new (a copy never takes the place of a file), so the
	// renaming is safe on exFAT too.
	if (ok)
		ok = rename(part.c_str(), path.c_str()) == 0;
	if (!ok)
		unlink(part.c_str());
	else
		chmod(path.c_str(), 0666);
	return ok;
}

bool exists(int destination, const std::string& path)
{
	if (isNetwork(destination))
	{
		smb::Entry entry;
		return smb::stat(path, entry) == 1;
	}
	return fileExists(path);
}

bool writeCopy(const RamDisc& disc, const Disc& about, int destination)
{
	std::string why;
	std::string folder = destinationFolder(destination, &why);
	if (folder.empty())
	{
		setError(why);
		return false;
	}
	if (folder.back() != '/')
		folder += '/';
	// A name not taken yet.
	std::string base = about.name;
	for (int n = 2; exists(destination, folder + base + ".cue") || exists(destination, folder + base + ".bin"); n++)
		base = about.name + format(" (%d)", n);
	const std::string bin = folder + base + ".bin", cue = folder + base + ".cue", sbi = folder + base + ".sbi";
	const std::string cueText = cueOf(disc, base + ".bin");
	int bad = 0;
	const std::vector<uint8_t> sbiData = sbiOf(disc, bad);
	const uint64_t binBytes = disc.raw.size();
	{
		std::lock_guard<std::mutex> lock(jobMutex);
		jobText = "Writing the copy to " + destinationName(destination);
	}
	jobDone = 0;
	jobTotal = binBytes;
	// The .cue last: the library finds a copy by it, so it is never seen
	// before the rest is there. A copy cut short takes away what it wrote.
	bool ok = true;
	std::string error;
	std::vector<std::string> written;
	const bool network = isNetwork(destination);
	auto put = [&](const std::string& path, const uint8_t *data, size_t bytes, bool counted) {
		if (!ok || jobCancel)
		{
			ok = false;
			return;
		}
		if (network)
		{
			ok = smb::writeFile(path, data, bytes, error);
			if (counted)
				jobDone = bytes;
		}
		else
		{
			ok = writeLocal(path, data, bytes, counted);
			if (!ok)
				error = "the folder could not be written (full, or read only?)";
		}
		if (ok)
			written.push_back(path);
	};
	if (network)
		smb::makeFolders(folder);
	else
		makeDir(folder.substr(0, folder.size() - 1));
	if (!sbiData.empty())
		put(sbi, sbiData.data(), sbiData.size(), false);
	put(bin, disc.raw.data(), disc.raw.size(), true);
	put(cue, reinterpret_cast<const uint8_t *>(cueText.data()), cueText.size(), false);
	if (!ok || jobCancel)
	{
		for (const std::string& path : written)
		{
			if (network)
				smb::remove(path);
			else
				unlink(path.c_str());
		}
		if (!jobCancel)
			setError("The copy could not be written to " + destinationName(destination) + ": " + error + ".");
		return false;
	}
	diag::mark("disc drive: copied to %s (%s%s)", bin.c_str(), disc.q.empty() ? "no sub-channel" : "with sub-channel",
			sbiData.empty() ? "" : format(", %d LibCrypt sectors in the .sbi", bad).c_str());
	std::lock_guard<std::mutex> lock(jobMutex);
	jobCopied = cue;
	jobSource = destination == ToGames ? library::Internal : destination == ToUsb ? library::Usb : library::Network;
	return true;
}

void runJob(Disc disc, int destination)
{
	std::shared_ptr<RamDisc> ram;
	{
		std::lock_guard<std::mutex> lock(jobMutex);
		jobText = destination >= 0 ? "Reading the disc" : "Reading the disc into memory";
	}
	bool ok = readWhole(disc, ram);
	if (ok && destination >= 0)
	{
		ok = writeCopy(*ram, disc, destination);
		ram.reset();
		if (ok)
			library::scan(jobSource, true);
	}
	else if (ok)
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		ramDisc = std::move(ram);
	}
	if (!ok && jobCancel)
	{
		std::lock_guard<std::mutex> lock(jobMutex);
		jobError.clear();
	}
	job = ok ? JobDone : JobFailed;
}

void startJob(const Disc& disc, int destination)
{
	if (job == JobRunning)
		return;
	if (jobThread.joinable())
		jobThread.join();
	jobCancel = false;
	jobDone = 0;
	jobTotal = disc.bytes;
	{
		std::lock_guard<std::mutex> lock(jobMutex);
		jobError.clear();
		jobCopied.clear();
		jobText = "Reading the disc";
	}
	releaseRam();
	job = JobRunning;
	jobThread = std::thread(runJob, disc, destination);
}

// ------------------------------------------------------- the open hook

std::string nameOfPath(const std::string& rest)
{
	const size_t slash = rest.find('/');
	return slash == std::string::npos ? std::string() : rest.substr(slash + 1);
}

bool openHook(const char *filename, std::unique_ptr<CDImage> *out, Common::Error *error)
{
	const std::string path = filename != nullptr ? filename : "";
	const bool ram = isRamPath(path);
	if (!ram && !isDrivePath(path))
		return false;
	const std::string rest = path.substr(ram ? 11 : 8);
	const std::string device = "/dev/" + rest.substr(0, rest.find('/'));
	if (ram)
	{
		std::shared_ptr<const RamDisc> disc;
		{
			std::lock_guard<std::mutex> lock(stateMutex);
			disc = ramDisc;
		}
		if (!disc || disc->device != device)
		{
			if (error != nullptr)
				error->SetMessage("The copy of the disc in memory is no longer there.");
			out->reset();
			return true;
		}
		*out = std::make_unique<RamImage>(disc, path);
		return true;
	}
	auto image = std::make_unique<StreamImage>();
	std::lock_guard<std::mutex> busy(driveMutex);
	if (!image->open(device, path, error))
		out->reset();
	else
		*out = std::move(image);
	return true;
}

} // namespace

// ------------------------------------------------------------------ API

void start()
{
	// The drives are watched whenever PSSwanStation is out of its sandbox for
	// USB: with "USB disc drive" on, or "USB drives".
	const options::Frontend& o = options::frontend();
	wantedAtStart = o.discDrive;
	// The Disc Drive tab: with "USB disc drive" on it is there from the start,
	// with the drive's disc or with why no drive is found.
	if (o.discDrive)
		library::scan(library::Optical, true);
	if (!o.discDrive && !o.usb)
	{
		diag::mark("disc drive: not watched (USB disc drive and USB drives are off)");
		return;
	}
	if (watching.exchange(true))
		return;
#if defined(SWANSTATION_PS5)
	const bool inSandbox = !platform::usbAvailable();
#else
	// The PC test: the sandbox as the console's would keep the title in it.
	const bool inSandbox = getenv("SWANSTATION_FAKE_SANDBOX") != nullptr;
#endif
	if (inSandbox)
	{
		const std::string why = platform::drivesProblem();
		diag::mark("disc drive: PSSwanStation is in its sandbox, so the drives cannot be read (%s)",
				why.empty() ? "leaving it was not granted" : why.c_str());
		watching = false;
		return;
	}
	diag::mark("disc drive: watching the drives");
	watcher = std::thread(watch);
}

void stop()
{
	watching = false;
	if (watcher.joinable())
		watcher.join();
	jobCancel = true;
	if (jobThread.joinable())
		jobThread.join();
}

std::vector<Disc> discs()
{
	std::lock_guard<std::mutex> lock(stateMutex);
	if (present.empty() && options::frontend().discDrive)
		return { notFound() };
	return present;
}

unsigned generation()
{
	return discGeneration;
}

bool isDrivePath(const std::string& path)
{
	return path.rfind("drive://", 0) == 0;
}

bool isRamPath(const std::string& path)
{
	return path.rfind("driveram://", 0) == 0;
}

std::string keptPath(const std::string& path)
{
	return isRamPath(path) ? "drive://" + path.substr(11) : path;
}

std::string streamPath(const Disc& disc)
{
	return "drive://" + shortDevice(disc.device) + "/" + disc.name + ".disc";
}

std::string ramPath(const Disc& disc)
{
	return "driveram://" + shortDevice(disc.device) + "/" + disc.name + ".disc";
}

bool discOf(const std::string& path, Disc& out)
{
	const bool ram = isRamPath(path);
	if (!ram && !isDrivePath(path))
		return false;
	const std::string rest = path.substr(ram ? 11 : 8);
	const std::string device = "/dev/" + rest.substr(0, rest.find('/'));
	std::lock_guard<std::mutex> lock(stateMutex);
	if (device == NoDrive)
	{
		if (!present.empty() || !options::frontend().discDrive)
			return false;
		out = notFound();
		return true;
	}
	for (const Disc& disc : present)
		if (disc.device == device)
		{
			out = disc;
			return true;
		}
	return false;
}

void installOpener()
{
	CDImage::SetOpenHook(openHook);
}

void setPlaying(bool on)
{
	playing = on;
}

bool eject(const std::string& device, std::string& why)
{
	if (playing || job == JobRunning)
	{
		why = "The disc is in use: close the game, or wait for the copy to end.";
		return false;
	}
	std::lock_guard<std::mutex> busy(driveMutex);
	std::string error;
	std::unique_ptr<Drive> drive = openDevice(device, error);
	if (!drive)
	{
		why = "The drive could not be opened: " + error + ".";
		return false;
	}
	std::string how;
	const bool ok = drive->eject(how);
	diag::mark("disc drive: %s: eject %s (%s)", device.c_str(), ok ? "done" : "failed", how.c_str());
	if (!ok)
		why = "The drive did not open its tray (" + how + "). Its own button may work now that the tray is unlocked.";
	return ok;
}

std::string destinationName(int destination)
{
	switch (destination)
	{
	case ToGames: return "the games folder";
	case ToUsb: return "the USB drive";
	case ToShare: return "the network share (SMB)";
	default: return "the NFS share";
	}
}

std::string destinationFolder(int destination, std::string *why)
{
	std::string reason;
	std::string folder;
	if (destination == ToGames)
		folder = rootDir + "games/";
	else if (destination == ToUsb)
	{
		const std::vector<std::string> dirs = platform::usbGameDirs();
		if (!dirs.empty())
			folder = dirs.front() + "/";
		else
		{
			const std::vector<std::string> drives = platform::usbDrives();
			if (!drives.empty())
				folder = drives.front() + "/psx/";
			else
				reason = platform::usbAvailable() ? "No USB drive is plugged in." : "USB drives are not on (Settings, Games and network).";
		}
	}
	else
	{
		const char *scheme = destination == ToShare ? "smb://" : "nfs://";
		for (const std::string& f : smb::gameFolders())
			if (f.rfind(scheme, 0) == 0 && smb::canWrite(f))
			{
				folder = f + "/";
				break;
			}
		if (folder.empty())
			reason = destination == ToShare ? "network.cfg names no SMB games folder that can be written."
					: "network.cfg names no NFS games folder that can be written.";
	}
	if (why != nullptr)
		*why = reason;
	return folder;
}

void startRam(const Disc& disc)
{
	startJob(disc, -1);
}

void startCopy(const Disc& disc, int destination)
{
	startJob(disc, destination);
}

int jobState()
{
	return job;
}

Status status()
{
	Status s;
	{
		std::lock_guard<std::mutex> lock(jobMutex);
		s.text = jobText;
	}
	s.done = jobDone;
	s.total = jobTotal;
	if (s.total != 0)
		s.progress = std::clamp((float)((double)s.done / (double)s.total), 0.f, 1.f);
	return s;
}

void cancel()
{
	jobCancel = true;
}

std::string lastError()
{
	std::lock_guard<std::mutex> lock(jobMutex);
	return jobError;
}

std::string copiedPath()
{
	std::lock_guard<std::mutex> lock(jobMutex);
	return jobCopied;
}

int copiedSource()
{
	std::lock_guard<std::mutex> lock(jobMutex);
	return jobSource;
}

void finishJob()
{
	if (job == JobRunning)
		return;
	if (jobThread.joinable())
		jobThread.join();
	job = JobIdle;
}

void releaseRam()
{
	std::lock_guard<std::mutex> lock(stateMutex);
	ramDisc.reset();
}

// ------------------------------------------------- the PC's stand-in drive

#if defined(SWANSTATION_HOST)
namespace
{

class FakeDrive final : public Drive
{
public:
	explicit FakeDrive(std::unique_ptr<CDImage> image) : image(std::move(image))
	{
		const char *m = getenv("SWANSTATION_FAKE_DRIVE_MODE");
		mode = m != nullptr ? m : "";
	}

	bool readToc(std::vector<TocEntry>& out, std::string& error) override
	{
		out.clear();
		if (mode == "empty")
		{
			error = "the stand-in drive is empty";
			return false;
		}
		for (const CDImage::Track& t : image->GetTracks())
		{
			TocEntry e;
			e.track = (uint8_t)t.track_number;
			e.control = (uint8_t)(t.control.bits >> 4);
			e.adr = 1;
			e.lba = t.start_lba - Pregap;
			out.push_back(e);
		}
		TocEntry end;
		end.track = 0xAA;
		end.lba = image->GetLBACount() - Pregap;
		out.push_back(end);
		return true;
	}

	bool readCd(uint32_t lba, uint32_t count, Sub sub, uint8_t *out, bool audio = false) override
	{
		(void)audio;
		if (dataOnly() || (mode == "raw" && sub != Sub::None))
			return false;
		// SWANSTATION_FAKE_DRIVE_DELAY_MS: a slow drive, that many ms a command,
		// and 1.5 s to spin up again after two seconds idle.
		if (const char *delay = getenv("SWANSTATION_FAKE_DRIVE_DELAY_MS"))
		{
			const double t = now();
			if (lastRead > 0 && t - lastRead > 2.0)
				usleep(1500000);
			usleep((useconds_t)(atoi(delay) * 1000));
			lastRead = now();
		}
		const uint32_t each = RawSector + subSize(sub);
		for (uint32_t i = 0; i < count; i++)
		{
			CDImage::SubChannelQ q;
			uint8_t *sector = out + (size_t)i * each;
			if (!image->Seek(lba + i + Pregap) || !image->ReadRawSector(sector, &q))
				return false;
			if (sub == Sub::Q)
			{
				std::memset(sector + RawSector, 0, SubQSize);
				std::memcpy(sector + RawSector, q.data.data(), 12);
			}
			else if (sub == Sub::Raw)
			{
				// Q into bit 6 of the 96 bytes, as a drive interleaves it.
				uint8_t *raw = sector + RawSector;
				std::memset(raw, 0, SubRawSize);
				for (int k = 0; k < 96; k++)
					if ((q.data[(size_t)(k / 8)] >> (7 - k % 8)) & 1)
						raw[k] |= 0x40;
			}
		}
		return true;
	}

	bool readData(uint32_t lba, uint32_t count, uint8_t *out) override
	{
		std::vector<uint8_t> sector(RawSector);
		for (uint32_t i = 0; i < count; i++)
		{
			if (!image->Seek(lba + i + Pregap) || !image->ReadRawSector(sector.data(), nullptr))
				return false;
			// "data-strict": a Form 2 sector refused, as many drives do.
			if (mode == "data-strict" && (sector[18] & 0x20) != 0)
				return false;
			std::memcpy(out + (size_t)i * DataSector, sector.data() + 24, DataSector);
		}
		return true;
	}

	bool setSpeed(unsigned) override { return !dataOnly(); }
	// SWANSTATION_FAKE_DRIVE_USB: 1 or 0, what the bus says; unset, it does not say.
	int onUsb() const override
	{
		const char *u = getenv("SWANSTATION_FAKE_DRIVE_USB");
		return u == nullptr ? -1 : atoi(u) != 0 ? 1 : 0;
	}
	std::string identity() const override { return "PC STAND-IN DRIVE"; }
	bool allowRemoval(std::string& how) override { how = "the PC's stand-in"; return true; }
	bool eject(std::string& how) override { how = "the PC's stand-in"; return true; }
	std::string route() const override { return dataOnly() ? "no pass device (the PC's stand-in)" : "the PC's stand-in"; }

private:
	double lastRead = 0;
	bool dataOnly() const { return mode.rfind("data", 0) == 0; }

	std::unique_ptr<CDImage> image;
	std::string mode;
};

}

// SWANSTATION_FAKE_DRIVE_NODE: the stand-in's name in /dev (cd1 unless set).
std::string fakeNode()
{
	const char *node = getenv("SWANSTATION_FAKE_DRIVE_NODE");
	return std::string("/dev/") + (node != nullptr ? node : "cd1");
}

std::vector<std::string> deviceNames()
{
	std::vector<std::string> names;
	// The drive is there while its image is: taking the file away unplugs it.
	const char *path = getenv("SWANSTATION_FAKE_DRIVE");
	if (path != nullptr && fileExists(path))
		names.push_back(fakeNode());
	return names;
}

std::unique_ptr<Drive> openDevice(const std::string& device, std::string& error)
{
	const char *path = getenv("SWANSTATION_FAKE_DRIVE");
	if (path == nullptr || device != fakeNode() || !fileExists(path))
	{
		error = device + ": no such drive";
		return nullptr;
	}
	Common::Error e;
	std::unique_ptr<CDImage> image = CDImage::Open(path, CDImage::OpenFlags::None, &e);
	if (!image)
	{
		error = "the stand-in disc image could not be opened";
		return nullptr;
	}
	return std::make_unique<FakeDrive>(std::move(image));
}
#endif

}
