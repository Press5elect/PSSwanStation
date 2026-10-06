/*
	PSSwanStation - rewind: the last while of play, kept in memory.

	SPDX-License-Identifier: GPL-3.0-or-later

	Every few frames the emulator's state is taken (host.cpp) and handed here.
	Only the newest is kept whole. For each one before it there is what must
	be changed in the one after it to get it back: the bytes that differ, and
	where. Two states a tenth of a second apart differ in a few hundred
	kilobytes of the eleven megabytes a state is, so a fixed amount of memory
	(the setting) holds a minute or several of play. Stepping back puts the
	bytes back, one state at a time; when the memory is full the oldest step
	is forgotten.

	Nothing is written to the console's storage: this is memory only, and gone
	when the game closes.
*/
#include "fe.h"

#include <cstring>
#include <deque>

namespace fe::rewind
{
namespace
{

std::vector<uint8_t> newest;
bool haveNewest;
// What turns a state into the one before it: runs of [bytes to pass over,
// bytes to put back, those bytes].
std::deque<std::vector<uint8_t>> steps;
size_t stepBytes;
size_t budget = 256u << 20;

void put32(std::vector<uint8_t>& out, uint32_t value)
{
	const size_t at = out.size();
	out.resize(at + 4);
	memcpy(out.data() + at, &value, 4);
}

// The step from `next` back to `old` (both the same size).
std::vector<uint8_t> difference(const std::vector<uint8_t>& old, const std::vector<uint8_t>& next)
{
	std::vector<uint8_t> out;
	out.reserve(256 * 1024);
	const size_t size = old.size();
	const uint8_t *a = old.data(), *b = next.data();
	size_t at = 0, passed = 0;
	while (at < size)
	{
		// Eight bytes at a time where they are the same.
		size_t same = at;
		while (same + 8 <= size && memcmp(a + same, b + same, 8) == 0)
			same += 8;
		while (same < size && a[same] == b[same])
			same++;
		if (same >= size)
			break;
		// A run that differs ends where sixteen bytes in a row are the same again
		// (a shorter gap costs more to describe than to copy).
		size_t end = same + 1, equal = 0;
		while (end < size && equal < 16)
		{
			equal = a[end] == b[end] ? equal + 1 : 0;
			end++;
		}
		end -= equal;
		put32(out, (uint32_t)(same - passed));
		put32(out, (uint32_t)(end - same));
		out.insert(out.end(), a + same, a + end);
		passed = at = end;
	}
	return out;
}

void putBack(std::vector<uint8_t>& state, const std::vector<uint8_t>& step)
{
	size_t at = 0, read = 0;
	while (read + 8 <= step.size())
	{
		uint32_t pass, length;
		memcpy(&pass, step.data() + read, 4);
		memcpy(&length, step.data() + read + 4, 4);
		read += 8;
		at += pass;
		if (read + length > step.size() || at + length > state.size())
			return;
		memcpy(state.data() + at, step.data() + read, length);
		at += length;
		read += length;
	}
}

}

void configure(size_t bytes)
{
	budget = bytes;
}

void clear()
{
	steps.clear();
	stepBytes = 0;
	haveNewest = false;
	newest.clear();
	newest.shrink_to_fit();
}

void push(const std::vector<uint8_t>& state)
{
	if (haveNewest && newest.size() == state.size())
	{
		std::vector<uint8_t> step = difference(newest, state);
		step.shrink_to_fit();
		stepBytes += step.size();
		steps.push_back(std::move(step));
	}
	else
	{
		// The first, or a state of another size (another machine): from here anew.
		steps.clear();
		stepBytes = 0;
	}
	newest = state;
	haveNewest = true;
	while (!steps.empty() && stepBytes + newest.size() > budget)
	{
		stepBytes -= steps.front().size();
		steps.pop_front();
	}
}

bool pop(std::vector<uint8_t>& state)
{
	if (!haveNewest)
		return false;
	state = newest;
	if (steps.empty())
	{
		haveNewest = false;
		return true;
	}
	putBack(newest, steps.back());
	stepBytes -= steps.back().size();
	steps.pop_back();
	return true;
}

size_t count()
{
	return haveNewest ? steps.size() + 1 : 0;
}

size_t bytes()
{
	return stepBytes + (haveNewest ? newest.size() : 0);
}

}
