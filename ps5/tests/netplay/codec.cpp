/*
	PSSwanStation - netplay's tests: the packing of the state.

	SPDX-License-Identifier: GPL-3.0-or-later

	netplay.cpp is part of this file, so that what it keeps to itself can be
	called: buffers of every shape are packed and unpacked and must come back
	as they were, and packed bytes that are damaged must be refused, not
	followed out of the buffer (the address sanitizer watches).
*/
#include "../../src/netplay.cpp"

#include <cstdio>
#include <cstdlib>

using namespace fe::netplay;

namespace
{
uint64_t seed = 0x1234567890abcdefull;

uint64_t next()
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return seed;
}

int failures = 0;

void roundTrip(const std::vector<uint8_t>& data, const char *what)
{
	std::vector<uint8_t> packed, back;
	packZeros(data.data(), data.size(), packed);
	if (!unpackZeros(packed.data(), packed.size(), data.size(), back) || back != data)
	{
		printf("FAIL: %s (%zu bytes) did not come back as it was\n", what, data.size());
		failures++;
	}
	// One byte more or less than it is must be refused.
	if (unpackZeros(packed.data(), packed.size(), data.size() + 1, back)
			|| (!data.empty() && unpackZeros(packed.data(), packed.size(), data.size() - 1, back)))
	{
		printf("FAIL: %s (%zu bytes) unpacked to another size\n", what, data.size());
		failures++;
	}
}

// Zero runs and other bytes in turn, each of a length up to the limits given.
std::vector<uint8_t> shaped(size_t size, size_t maxZeros, size_t maxOther)
{
	std::vector<uint8_t> data(size, 0);
	size_t at = 0;
	while (at < size)
	{
		at += next() % (maxZeros + 1);
		const size_t other = next() % (maxOther + 1);
		for (size_t i = 0; i < other && at < size; i++, at++)
			data[at] = (uint8_t)(next() | 1);
	}
	return data;
}
}

int main()
{
	roundTrip({}, "nothing");
	for (size_t size = 1; size <= 80; size++)
	{
		roundTrip(std::vector<uint8_t>(size, 0), "zeros");
		roundTrip(std::vector<uint8_t>(size, 7), "sevens");
		// A run of every length at every place of a small buffer.
		for (size_t start = 0; start < size; start++)
			for (size_t length = 1; start + length <= size; length += length < 20 ? 1 : 7)
			{
				std::vector<uint8_t> data(size, 0xaa);
				std::fill(data.begin() + (ptrdiff_t)start, data.begin() + (ptrdiff_t)(start + length), 0);
				roundTrip(data, "one run");
			}
	}
	for (int i = 0; i < 3000; i++)
	{
		const size_t size = next() % 5000;
		roundTrip(shaped(size, 1 + next() % 64, 1 + next() % 64), "a small mix");
	}
	for (int i = 0; i < 20; i++)
		roundTrip(shaped(1 + next() % (4u << 20), 1 + next() % 200000, 1 + next() % 300), "a large mix");

	// A state like the emulator's: 11 MiB, mostly zeros.
	{
		std::vector<uint8_t> data = shaped(11u << 20, 60000, 40), packed, back;
		const double started = fe::now();
		packZeros(data.data(), data.size(), packed);
		const double middle = fe::now();
		const bool whole = unpackZeros(packed.data(), packed.size(), data.size(), back);
		const double ended = fe::now();
		const uint32_t sum = stateSum(data.data(), data.size());
		printf("11 MiB: packed to %zu bytes in %.1f ms, unpacked in %.1f ms, summed in %.1f ms\n", packed.size(),
				(middle - started) * 1000.0, (ended - middle) * 1000.0, (fe::now() - ended) * 1000.0);
		if (!whole || back != data || stateSum(back.data(), back.size()) != sum)
		{
			printf("FAIL: the large state did not come back\n");
			failures++;
		}
		back[back.size() / 2] ^= 1;
		if (stateSum(back.data(), back.size()) == sum)
		{
			printf("FAIL: the sum did not notice a changed bit\n");
			failures++;
		}
	}

	// Damaged packed bytes: cut short, changed, or noise. They may unpack to
	// something or be refused; they must not be followed out of the buffer.
	unsigned refused = 0, taken = 0;
	for (int i = 0; i < 20000; i++)
	{
		const std::vector<uint8_t> data = shaped(next() % 3000, 1 + next() % 100, 1 + next() % 30);
		std::vector<uint8_t> packed, back;
		packZeros(data.data(), data.size(), packed);
		switch (next() % 3)
		{
		case 0:
			packed.resize(next() % (packed.size() + 1));
			break;
		case 1:
			for (int n = 1 + (int)(next() % 4); n > 0 && !packed.empty(); n--)
				packed[next() % packed.size()] = (uint8_t)next();
			break;
		default:
			packed.resize(next() % 200);
			for (uint8_t& byte : packed)
				byte = (uint8_t)next();
			break;
		}
		if (unpackZeros(packed.data(), packed.size(), data.size(), back))
			taken++;
		else
			refused++;
	}
	printf("damaged: %u refused, %u unpacked to the right size\n", refused, taken);

	printf(failures == 0 ? "codec: ok\n" : "codec: FAILED\n");
	return failures == 0 ? 0 : 1;
}
