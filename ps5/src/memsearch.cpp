/*
	PSSwanStation - finding values in the emulated memory, and watching them.

	SPDX-License-Identifier: GPL-3.0-or-later

	The way cheats are found: a search starts with every place in the main
	memory (2 MiB, 8 with the larger memory) as a candidate, and each step
	keeps those that compare as asked, against a number or against what they
	were at the step before: the lives went down by one, so the place that
	holds them decreased while most did not. A handful left, each can be
	watched over the game, frozen at a value, or made a GameShark code in the
	game's own cheat file.

	A candidate is a flag for each place of the search's size, and the memory
	as it was at the last step is kept beside them; both are as large as the
	memory itself, at most.
*/
#include "memsearch.h"
#include "fe.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fe::host
{
uint8_t *coreRam(uint32_t& size);
}

namespace fe::memsearch
{
namespace
{
int searchSize = 1;
bool begun = false;
int stepCount = 0;
std::vector<uint8_t> alive;		// one flag for each place of searchSize
std::vector<uint8_t> before;	// the memory at the last step
uint64_t aliveCount = 0;

std::vector<Watch> watchList;
std::string watchSerial;

uint32_t readFrom(const uint8_t *memory, uint32_t limit, uint32_t address, int bytes)
{
	if (memory == nullptr || address + (uint32_t)bytes > limit)
		return 0;
	uint32_t value = 0;
	for (int i = bytes - 1; i >= 0; i--)
		value = (value << 8) | memory[address + (uint32_t)i];
	return value;
}

bool matches(Compare how, uint32_t now, uint32_t was, uint32_t value)
{
	switch (how)
	{
	case Equal: return now == value;
	case NotEqual: return now != value;
	case Greater: return now > value;
	case Less: return now < value;
	case Changed: return now != was;
	case Unchanged: return now == was;
	case Increased: return now > was;
	case Decreased: return now < was;
	default: return false;
	}
}

std::string watchFile(const std::string& serial)
{
	return rootDir + "data/watch/" + serial + ".txt";
}
}

const char *compareName(Compare how)
{
	static const char *const names[CompareCount] = { "Equal to the value", "Not equal to the value",
		"Greater than the value", "Less than the value", "Changed", "Unchanged", "Increased", "Decreased" };
	return how >= 0 && how < CompareCount ? names[how] : "";
}

bool compareNeedsValue(Compare how)
{
	return how == Equal || how == NotEqual || how == Greater || how == Less;
}

uint32_t memorySize()
{
	uint32_t size = 0;
	host::coreRam(size);
	return size;
}

void setSize(int bytes)
{
	bytes = bytes >= 4 ? 4 : bytes >= 2 ? 2 : 1;
	if (bytes == searchSize)
		return;
	searchSize = bytes;
	begun = false;
	alive.clear();
	before.clear();
	aliveCount = 0;
	stepCount = 0;
}

int size()
{
	return searchSize;
}

void begin()
{
	uint32_t ramSize = 0;
	const uint8_t *ram = host::coreRam(ramSize);
	if (ram == nullptr || ramSize == 0)
	{
		begun = false;
		return;
	}
	const uint32_t places = ramSize / (uint32_t)searchSize;
	alive.assign(places, 1);
	before.assign(ram, ram + ramSize);
	aliveCount = places;
	begun = true;
	stepCount = 0;
}

bool searching()
{
	return begun;
}

void narrow(Compare how, uint32_t value)
{
	if (!begun)
		begin();
	uint32_t ramSize = 0;
	const uint8_t *ram = host::coreRam(ramSize);
	if (ram == nullptr || ramSize != before.size())
	{
		// The memory changed size (another game, the larger memory): anew.
		begin();
		return;
	}
	const uint32_t step = (uint32_t)searchSize;
	uint64_t left = 0;
	for (size_t i = 0; i < alive.size(); i++)
	{
		if (!alive[i])
			continue;
		const uint32_t address = (uint32_t)i * step;
		const uint32_t now = readFrom(ram, ramSize, address, searchSize);
		const uint32_t was = readFrom(before.data(), (uint32_t)before.size(), address, searchSize);
		if (matches(how, now, was, value))
			left++;
		else
			alive[i] = 0;
	}
	aliveCount = left;
	before.assign(ram, ram + ramSize);
	stepCount++;
}

uint64_t count()
{
	return begun ? aliveCount : 0;
}

int steps()
{
	return stepCount;
}

std::vector<Result> results(size_t most)
{
	std::vector<Result> out;
	if (!begun)
		return out;
	uint32_t ramSize = 0;
	const uint8_t *ram = host::coreRam(ramSize);
	const uint32_t step = (uint32_t)searchSize;
	for (size_t i = 0; i < alive.size() && out.size() < most; i++)
	{
		if (!alive[i])
			continue;
		const uint32_t address = (uint32_t)i * step;
		out.push_back({ address, readFrom(ram, ramSize, address, searchSize),
			readFrom(before.data(), (uint32_t)before.size(), address, searchSize) });
	}
	return out;
}

uint32_t read(uint32_t address, int bytes)
{
	uint32_t ramSize = 0;
	const uint8_t *ram = host::coreRam(ramSize);
	return readFrom(ram, ramSize, address, bytes);
}

void write(uint32_t address, int bytes, uint32_t value)
{
	uint32_t ramSize = 0;
	uint8_t *ram = host::coreRam(ramSize);
	if (ram == nullptr || address + (uint32_t)bytes > ramSize)
		return;
	for (int i = 0; i < bytes; i++)
		ram[address + (uint32_t)i] = (uint8_t)(value >> (8 * i));
}

std::vector<Watch>& watches()
{
	return watchList;
}

void addWatch(uint32_t address, int bytes, const std::string& name)
{
	for (const Watch& watch : watchList)
		if (watch.address == address && watch.size == bytes)
			return;
	Watch watch;
	watch.address = address;
	watch.size = bytes;
	watch.name = name;
	watchList.push_back(watch);
	save();
}

void removeWatch(size_t index)
{
	if (index < watchList.size())
		watchList.erase(watchList.begin() + (long)index);
	save();
}

void setFrozen(size_t index, bool frozen)
{
	if (index >= watchList.size())
		return;
	watchList[index].frozen = frozen;
	if (frozen)
		watchList[index].frozenValue = read(watchList[index].address, watchList[index].size);
	save();
}

void setWatchValue(size_t index, uint32_t value)
{
	if (index >= watchList.size())
		return;
	write(watchList[index].address, watchList[index].size, value);
	if (watchList[index].frozen)
		watchList[index].frozenValue = value;
	save();
}

void loadFor(const std::string& serial)
{
	watchList.clear();
	watchSerial = serial;
	setSize(1);
	begun = false;
	alive.clear();
	before.clear();
	if (serial.empty())
		return;
	std::vector<uint8_t> raw;
	if (!readFile(watchFile(serial), raw))
		return;
	const std::string text(raw.begin(), raw.end());
	size_t start = 0;
	while (start < text.size())
	{
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = trim(text.substr(start, end - start));
		start = end + 1;
		if (line.empty() || line[0] == '#')
			continue;
		// "address size frozen value name", the numbers in hexadecimal.
		unsigned address = 0, bytes = 1, frozen = 0, value = 0;
		int used = 0;
		if (sscanf(line.c_str(), "%x %u %u %x %n", &address, &bytes, &frozen, &value, &used) < 4)
			continue;
		Watch watch;
		watch.address = address;
		watch.size = bytes >= 4 ? 4 : bytes >= 2 ? 2 : 1;
		watch.frozen = frozen != 0;
		watch.frozenValue = value;
		watch.name = used > 0 ? trim(line.substr((size_t)used)) : std::string();
		watchList.push_back(watch);
	}
}

void save()
{
	if (watchSerial.empty())
		return;
	std::string text = "# PSSwanStation's watched values for " + watchSerial
			+ ": address size frozen value name (address and value in hexadecimal)\n";
	for (const Watch& watch : watchList)
		text += format("%06x %d %d %x %s\n", watch.address, watch.size, watch.frozen ? 1 : 0, watch.frozenValue,
				watch.name.c_str());
	makeDir(rootDir + "data/watch");
	writeFile(watchFile(watchSerial), text.data(), text.size());
}

void unload()
{
	watchList.clear();
	watchSerial.clear();
	begun = false;
	alive.clear();
	before.clear();
	aliveCount = 0;
	stepCount = 0;
}

void frame()
{
	for (const Watch& watch : watchList)
		if (watch.frozen)
			write(watch.address, watch.size, watch.frozenValue);
}

std::string gameSharkCode(uint32_t address, int bytes, uint32_t value)
{
	const uint32_t at = address & 0xFFFFFF;
	if (bytes == 1)
		return format("30%06X 00%02X", at, value & 0xFF);
	if (bytes == 2)
		return format("80%06X %04X", at, value & 0xFFFF);
	return format("80%06X %04X\n80%06X %04X", at, value & 0xFFFF, (at + 2) & 0xFFFFFF, (value >> 16) & 0xFFFF);
}

std::string addressText(uint32_t address)
{
	return format("0x%08X", 0x80000000u | address);
}

}
