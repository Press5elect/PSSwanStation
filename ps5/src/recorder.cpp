/*
	PSSwanStation - recordings of what the players pressed, and playing them back.

	SPDX-License-Identifier: GPL-3.0-or-later

	A recording is the state of the game when it began, then, for each frame
	the emulator ran, what each of the four ports gave it: the buttons, the
	sticks, how far the triggers were pressed, where a light gun aimed, and
	whether a pad was there at all. Started from that state and given the same,
	the emulator runs the same frames again, so a recording shows a run to
	someone else, or brings back the moment a fault happened.

	The file (<root>data/recordings/<serial>/<date and time>.psrec):

	  "PSWREC01"                     8 bytes
	  u32 version (1), u32 ports (4), u32 bytes a port takes in a frame (19)
	  u32 length, the disc's serial
	  u32 length, the game's name
	  u32 length, the title's build that made it, as text
	  u64 length, the state it begins from (a state file's contents: host.cpp)
	  then the frames, one after another, until the file ends

	Everything little-endian, as the console and a PC are.
*/
#include "recorder.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <algorithm>

namespace fe::recorder
{
namespace
{
constexpr char Magic[8] = { 'P', 'S', 'W', 'R', 'E', 'C', '0', '1' };
constexpr uint32_t Version = 1;
constexpr uint32_t PortBytes = 19;
constexpr size_t FrameBytes = PortBytes * Ports;

Mode current = Off;
FILE *file = nullptr;
uint64_t position = 0, total = 0;
std::string currentPath;

void put16(uint8_t *out, uint16_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
}

uint16_t get16(const uint8_t *in)
{
	return (uint16_t)(in[0] | (in[1] << 8));
}

void encode(const PortInput& in, uint8_t *out)
{
	put16(out + 0, in.buttons);
	put16(out + 2, (uint16_t)in.lx);
	put16(out + 4, (uint16_t)in.ly);
	put16(out + 6, (uint16_t)in.rx);
	put16(out + 8, (uint16_t)in.ry);
	put16(out + 10, (uint16_t)in.l2);
	put16(out + 12, (uint16_t)in.r2);
	put16(out + 14, (uint16_t)in.aimX);
	put16(out + 16, (uint16_t)in.aimY);
	out[18] = in.connected;
}

void decode(const uint8_t *in, PortInput& out)
{
	out.buttons = get16(in + 0);
	out.lx = (int16_t)get16(in + 2);
	out.ly = (int16_t)get16(in + 4);
	out.rx = (int16_t)get16(in + 6);
	out.ry = (int16_t)get16(in + 8);
	out.l2 = (int16_t)get16(in + 10);
	out.r2 = (int16_t)get16(in + 12);
	out.aimX = (int16_t)get16(in + 14);
	out.aimY = (int16_t)get16(in + 16);
	out.connected = in[18];
}

bool writeU32(FILE *f, uint32_t value)
{
	const uint8_t bytes[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
	return fwrite(bytes, 1, 4, f) == 4;
}

bool writeString(FILE *f, const std::string& text)
{
	return writeU32(f, (uint32_t)text.size()) && (text.empty() || fwrite(text.data(), 1, text.size(), f) == text.size());
}

bool readU32(FILE *f, uint32_t& value)
{
	uint8_t bytes[4];
	if (fread(bytes, 1, 4, f) != 4)
		return false;
	value = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
	return true;
}

bool readString(FILE *f, std::string& text, uint32_t most)
{
	uint32_t length;
	if (!readU32(f, length) || length > most)
		return false;
	text.resize(length);
	return length == 0 || fread(text.data(), 1, length, f) == length;
}

// The parts of the file before the frames. False when it is not a recording.
bool readHeader(FILE *f, Info& info, std::vector<uint8_t> *state)
{
	char magic[8];
	uint32_t version, ports, portBytes;
	if (fread(magic, 1, 8, f) != 8 || memcmp(magic, Magic, 8) != 0 || !readU32(f, version) || version != Version
			|| !readU32(f, ports) || ports != (uint32_t)Ports || !readU32(f, portBytes) || portBytes != PortBytes)
		return false;
	if (!readString(f, info.serial, 64) || !readString(f, info.title, 1024) || !readString(f, info.build, 64))
		return false;
	uint8_t sizeBytes[8];
	if (fread(sizeBytes, 1, 8, f) != 8)
		return false;
	uint64_t stateSize = 0;
	for (int i = 7; i >= 0; i--)
		stateSize = (stateSize << 8) | sizeBytes[i];
	if (stateSize == 0 || stateSize > (64u << 20))
		return false;
	if (state != nullptr)
	{
		state->resize((size_t)stateSize);
		if (fread(state->data(), 1, (size_t)stateSize, f) != stateSize)
			return false;
	}
	else if (fseek(f, (long)stateSize, SEEK_CUR) != 0)
		return false;
	const long framesAt = ftell(f);
	if (fseek(f, 0, SEEK_END) != 0)
		return false;
	const long end = ftell(f);
	if (framesAt < 0 || end < framesAt)
		return false;
	info.frames = (uint64_t)(end - framesAt) / FrameBytes;
	return fseek(f, framesAt, SEEK_SET) == 0;
}
}

bool startRecording(const std::string& path, const std::vector<uint8_t>& state, const Info& info)
{
	stop();
	FILE *f = fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	uint8_t sizeBytes[8];
	uint64_t size = state.size();
	for (int i = 0; i < 8; i++, size >>= 8)
		sizeBytes[i] = (uint8_t)size;
	const bool ok = fwrite(Magic, 1, 8, f) == 8 && writeU32(f, Version) && writeU32(f, (uint32_t)Ports)
			&& writeU32(f, PortBytes) && writeString(f, info.serial) && writeString(f, info.title)
			&& writeString(f, info.build) && fwrite(sizeBytes, 1, 8, f) == 8
			&& fwrite(state.data(), 1, state.size(), f) == state.size();
	if (!ok)
	{
		fclose(f);
		remove(path.c_str());
		return false;
	}
	file = f;
	current = Recording;
	position = total = 0;
	currentPath = path;
	return true;
}

void record(const PortInput (&inputs)[Ports])
{
	if (current != Recording || file == nullptr)
		return;
	uint8_t frame[FrameBytes];
	for (int port = 0; port < Ports; port++)
		encode(inputs[port], frame + port * PortBytes);
	if (fwrite(frame, 1, FrameBytes, file) != FrameBytes)
	{
		// A full disk: what was written stays a recording that ends here.
		stop();
		return;
	}
	position++;
	total = position;
	// A second at a time reaches the file, so a crash loses little.
	if (position % 60 == 0)
		fflush(file);
}

bool startPlayback(const std::string& path, std::vector<uint8_t>& state, Info& info)
{
	stop();
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	if (!readHeader(f, info, &state))
	{
		fclose(f);
		return false;
	}
	file = f;
	current = Playing;
	position = 0;
	total = info.frames;
	currentPath = path;
	return true;
}

bool next(PortInput (&inputs)[Ports])
{
	if (current != Playing || file == nullptr)
		return false;
	uint8_t frame[FrameBytes];
	if (fread(frame, 1, FrameBytes, file) != FrameBytes)
		return false;
	for (int port = 0; port < Ports; port++)
		decode(frame + port * PortBytes, inputs[port]);
	position++;
	return true;
}

void stop()
{
	if (file != nullptr)
		fclose(file);
	file = nullptr;
	current = Off;
	currentPath.clear();
}

Mode mode()
{
	return current;
}

uint64_t frame()
{
	return position;
}

uint64_t frames()
{
	return total;
}

const std::string& path()
{
	return currentPath;
}

bool read(const std::string& path, Info& info)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	const bool ok = readHeader(f, info, nullptr);
	fclose(f);
	return ok;
}

std::vector<std::string> list(const std::string& folder)
{
	std::vector<std::string> files;
	DIR *dir = opendir(folder.c_str());
	if (dir == nullptr)
		return files;
	while (const dirent *entry = readdir(dir))
	{
		const std::string name = entry->d_name;
		if (name.size() > 6 && name.compare(name.size() - 6, 6, ".psrec") == 0)
			files.push_back(folder + "/" + name);
	}
	closedir(dir);
	// Named by when they were made: the newest first.
	std::sort(files.begin(), files.end(), std::greater<std::string>());
	return files;
}

}
