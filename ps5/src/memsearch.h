/*
	PSSwanStation - finding values in the emulated memory, and watching them
	(memsearch.cpp).

	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fe::memsearch
{

// How a search narrows its candidates: against a value, or against what each
// was at the search's last step.
enum Compare
{
	Equal,
	NotEqual,
	Greater,
	Less,
	Changed,
	Unchanged,
	Increased,
	Decreased,
	CompareCount
};
const char *compareName(Compare how);
bool compareNeedsValue(Compare how);

// The size of the values looked for: 1, 2 or 4 bytes. Changing it ends the search.
void setSize(int bytes);
int size();
// A new search: every place in memory is a candidate, as it is now.
void begin();
bool searching();
// Keeps the candidates that compare so; what they are now is what the next
// step compares against.
void narrow(Compare how, uint32_t value);
// How many are left, and the first of them.
uint64_t count();
struct Result
{
	uint32_t address;	// from the start of the main memory (0x80000000 to the game)
	uint32_t value, previous;
};
std::vector<Result> results(size_t most);
// Steps taken since the search began, for the screen.
int steps();

// A value in the main memory, little-endian as the PlayStation keeps it.
uint32_t read(uint32_t address, int bytes);
void write(uint32_t address, int bytes, uint32_t value);
uint32_t memorySize();

// Values kept in sight, and held at one value when frozen.
struct Watch
{
	uint32_t address = 0;
	int size = 1;
	std::string name;
	bool frozen = false;
	uint32_t frozenValue = 0;
};
std::vector<Watch>& watches();
void addWatch(uint32_t address, int bytes, const std::string& name = "");
void removeWatch(size_t index);
void setFrozen(size_t index, bool frozen);
void setWatchValue(size_t index, uint32_t value);
// The watch list is the game's (<root>data/watch/<serial>.txt).
void loadFor(const std::string& serial);
void save();
void unload();
// Once a frame, before the emulator runs: frozen values are written.
void frame();

// A GameShark code that keeps `value` at `address`: 30aaaaaa 00vv for a byte,
// 80aaaaaa vvvv for two, two such lines for four.
std::string gameSharkCode(uint32_t address, int bytes, uint32_t value);
// "0x8001A2B4".
std::string addressText(uint32_t address);

}
