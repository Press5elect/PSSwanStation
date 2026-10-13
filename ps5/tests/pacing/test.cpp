// PSSwanStation - pacing_wait.h on the PC, with a simulated console.
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The clock is a number this test moves on: a sleep wakes late by what the
// simulated kernel adds (a tick of up to 10 ms, as measured on the console), a
// yield costs 20 us and a spin 1 us. Checked: the wait never ends before its
// end, ends within a spin of it whatever the lateness when the margin covers
// it, spends little of its time spinning, and the margin follows the measured
// lateness inside its bounds.
#include "../../src/pacing_wait.h"

#include <cstdio>
#include <cstdlib>

using namespace fe::pacingwait;

static int failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAILED line %d: %s\n", __LINE__, #c); failed++; } } while (0)

int main()
{
	// Margins.
	CHECK(marginFor({}) == MARGIN_DEFAULT_NS);
	CHECK(marginFor({ 0, 0, 0 }) == MARGIN_MIN_NS);
	CHECK(marginFor({ 50000000 }) == MARGIN_MAX_NS);
	{
		std::vector<uint64_t> late(20, 2000000);
		late[19] = 9000000;	// one outlier above the 90th percentile
		CHECK(marginFor(late) == 2000000 + MARGIN_EXTRA_NS);
	}
	// The measuring, with a sleep that wakes 3 ms late.
	{
		uint64_t clock = 0;
		const Measured m = measure([&](uint64_t ns) { clock += ns + 3000000; return true; }, [&] { return clock; });
		CHECK(m.ok && m.p90 == 3000000 && m.worst == 3000000);
		Measured better = m;
		better.p90 = 1000000;
		CHECK(preferSecond(m, better) && !preferSecond(better, m));
		Measured broken;
		CHECK(!preferSecond(m, broken) && preferSecond(broken, m));
	}
	// The wait, against lateness up to the margin.
	unsigned rounds = 0;
	uint64_t spinsTotal = 0, yieldsTotal = 0;
	for (uint64_t lateness = 0; lateness <= 9000000; lateness += 250000)
		for (uint64_t left = 100000; left <= 33000000; left += 1700000)
		{
			uint64_t clock = 1000000000ULL;
			uint64_t spins = 0, yields = 0;
			const uint64_t end = clock + left;
			const uint64_t margin = std::clamp(lateness + MARGIN_EXTRA_NS, MARGIN_MIN_NS, MARGIN_MAX_NS);
			Waits w;
			waitUntil(end, margin, [&] { return clock; }, [&](uint64_t ns) { clock += ns + lateness; },
				[&] { clock += 20000; yields++; }, [&] { clock += 1000; spins++; }, w);
			CHECK(clock >= end);
			CHECK(clock <= end + 20000);	// at most one yield past it: never a frame late
			CHECK(spins <= SPIN_LAST_NS / 1000 + 1);
			CHECK(w.waits == 1 && w.late == 0);
			spinsTotal += spins;
			yieldsTotal += yields;
			rounds++;
		}
	// A sleep that wakes later than the margin: counted as late.
	{
		uint64_t clock = 0;
		Waits w;
		waitUntil(10000000, 1000000, [&] { return clock; }, [&](uint64_t ns) { clock += ns + 4000000; },
			[&] { clock += 20000; }, [&] { clock += 1000; }, w);
		CHECK(w.late == 1 && w.latestNs == 3000000);
	}
	std::printf("pacing: %u waits simulated, %.1f spins and %.1f yields each; %d failed\n", rounds,
		(double)spinsTotal / rounds, (double)yieldsTotal / rounds, failed);
	return failed ? 1 : 0;
}
