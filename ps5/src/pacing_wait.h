// PSSwanStation - how late the console's sleeps wake, measured once at start,
// and the frame pacing's wait for a frame due by the clock (from Crux2 v1.0.1,
// its PS5Timing.h).
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pacing by the clock sleeps for most of what is left of a frame and waits out
// the rest. How much "the rest" must be depends on how late the kernel wakes a
// sleeping thread: PSWin measured short timed waits on this console rounded up
// to a tick of about 10 ms, which a fixed 0.3 ms margin does not cover, and a
// frame that wakes 8 ms late is a hitch. So at start a background thread asks
// for short sleeps with each of the two calls the console has (nanosleep and
// sceKernelUsleep), notes how late each woke, takes the call that woke closer
// to time, and sets the margin to its 90th percentile of lateness plus 0.3 ms,
// between 0.5 and 12 ms. In the margin the wait gives its core away
// (sched_yield) and spins only the last quarter of a millisecond, so the
// emulator's other threads and the graphics driver's get the core.
//
// The arithmetic is here, header-only, so tests/pacing checks it on the PC
// with a simulated console.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fe::pacingwait
{

inline constexpr uint64_t MARGIN_MIN_NS = 500000;
inline constexpr uint64_t MARGIN_MAX_NS = 12000000;
inline constexpr uint64_t MARGIN_EXTRA_NS = 300000;
inline constexpr uint64_t MARGIN_DEFAULT_NS = 1500000;
inline constexpr unsigned SAMPLES_PER_REQUEST = 12;
inline constexpr uint64_t REQUESTS_NS[] = { 1000000, 3000000 };
// While more than this is left the wait yields; only the end is spun.
inline constexpr uint64_t SPIN_LAST_NS = 250000;

// The p-th percentile (0..100) of the values, nearest rank; 0 for none.
inline uint64_t percentile(std::vector<uint64_t> values, unsigned p)
{
	if (values.empty())
		return 0;
	std::sort(values.begin(), values.end());
	size_t rank = (values.size() * p + 99) / 100;
	if (rank == 0)
		rank = 1;
	return values[std::min(rank, values.size()) - 1];
}

inline uint64_t marginFor(const std::vector<uint64_t>& lateness)
{
	if (lateness.empty())
		return MARGIN_DEFAULT_NS;
	return std::clamp(percentile(lateness, 90) + MARGIN_EXTRA_NS, MARGIN_MIN_NS, MARGIN_MAX_NS);
}

struct Measured
{
	std::vector<uint64_t> lateness;	// how late each sleep woke, ns
	uint64_t p50 = 0, p90 = 0, worst = 0;
	bool ok = false;			// every sleep returned
};

// sleep(ns) sleeps and returns false if the call failed; now() is a
// nanosecond clock.
template <typename SleepFn, typename NowFn>
Measured measure(SleepFn sleep, NowFn now)
{
	Measured m;
	m.ok = true;
	for (const uint64_t request : REQUESTS_NS)
		for (unsigned i = 0; i < SAMPLES_PER_REQUEST; i++)
		{
			const uint64_t from = now();
			if (!sleep(request))
			{
				m.ok = false;
				return m;
			}
			const uint64_t took = now() - from;
			m.lateness.push_back(took > request ? took - request : 0);
		}
	m.p50 = percentile(m.lateness, 50);
	m.p90 = percentile(m.lateness, 90);
	m.worst = percentile(m.lateness, 100);
	return m;
}

// Which of two measured calls to sleep with: the one whose 90th percentile is
// lower, the first on a tie (within 0.1 ms) or when the second failed.
inline bool preferSecond(const Measured& first, const Measured& second)
{
	if (!second.ok)
		return false;
	if (!first.ok)
		return true;
	return second.p90 + 100000 < first.p90;
}

// What the waits cost: asleep, awake, and how often a sleep woke past the end
// it was meant for.
struct Waits
{
	uint64_t waits = 0;
	uint64_t asleepNs = 0;
	uint64_t awakeNs = 0;
	uint64_t late = 0;		// waits whose sleep woke after the end
	uint64_t latestNs = 0;	// the most a sleep overshot

	void add(uint64_t asleep, uint64_t awake, uint64_t overshot)
	{
		waits++;
		asleepNs += asleep;
		awakeNs += awake;
		if (overshot > 0)
		{
			late++;
			latestNs = std::max(latestNs, overshot);
		}
	}
};

// The wait until `end`: sleep for what is left less the margin, then wait out
// the rest awake, yielding while more than SPIN_LAST_NS is left and spinning
// the end. What it cost is added to `waits`.
template <typename Now, typename Sleep, typename Yield, typename Spin>
inline void waitUntil(uint64_t end, uint64_t margin, Now now, Sleep sleep, Yield yield, Spin spin, Waits& waits)
{
	const uint64_t start = now();
	if (end <= start)
		return;
	if (end - start > margin)
		sleep(end - start - margin);
	const uint64_t woke = now();
	for (;;)
	{
		const uint64_t t = now();
		if (t >= end)
			break;
		if (end - t > SPIN_LAST_NS)
			yield();
		else
			spin();
	}
	const uint64_t awakeUntil = std::max(woke, end);
	waits.add(woke - start, awakeUntil - woke, woke > end ? woke - end : 0);
}

// The runtime (pacing_wait.cpp).
// Starts the probe once, on a thread of its own; its line goes to the boot log.
void startProbe();
// Waits until `end` (seconds on fe::now()'s clock).
void waitUntilSeconds(double end);
// The waits since the last call as one line ("" with none), and cleared.
std::string takeLine();
// The clock's line ("" before it runs, or on the PC).
std::string clockLine();

}
