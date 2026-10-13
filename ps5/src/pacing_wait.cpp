// PSSwanStation - the runtime of pacing_wait.h: the start-up probe of how late
// sleeps wake, the frame pacing's wait, and their lines for the boot log (from
// Crux2 v1.0.1, its PS5Misc.cpp).
//
// SPDX-FileCopyrightText: 2026 Jacob (Press5elect)
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pacing_wait.h"

#include "fe.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <sched.h>
#include <thread>

#if defined(SWANSTATION_PS5)
extern "C" int sceKernelUsleep(uint32_t microseconds);
#include "ps5/clock_ps5.h"
#endif

namespace fe::pacingwait
{
namespace
{

std::atomic<uint64_t> marginNs{ MARGIN_DEFAULT_NS };
std::atomic<bool> withUsleep{ false };
std::mutex waitsLock;
Waits waitsSoFar;

uint64_t nowNs()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// nanosleep for ns, going on after an interruption until the time is up.
bool nanosleepFor(uint64_t ns)
{
	const uint64_t until = nowNs() + ns;
	for (;;)
	{
		timespec ts;
		ts.tv_sec = (time_t)(ns / 1000000000ULL);
		ts.tv_nsec = (long)(ns % 1000000000ULL);
		if (nanosleep(&ts, nullptr) == 0)
			return true;
		if (errno != EINTR)
			return false;
		const uint64_t t = nowNs();
		if (t >= until)
			return true;
		ns = until - t;
	}
}

#if defined(SWANSTATION_PS5)
// The same with sceKernelUsleep, which takes microseconds and answers with an
// error code.
bool usleepFor(uint64_t ns)
{
	const uint64_t until = nowNs() + ns;
	for (;;)
	{
		const uint64_t us = (ns + 999) / 1000;
		const int r = sceKernelUsleep((uint32_t)std::min<uint64_t>(us, 0xffffffffULL));
		if (r == 0)
			return true;
		const uint64_t t = nowNs();
		if (t >= until)
			return true;
		// An interrupted sleep goes on; any other failure falls back to
		// nanosleep for the rest.
		if (r != EINTR && r != (int)0x80020004)
			return nanosleepFor(until - t);
		ns = until - t;
	}
}
#endif

void sleepFor(uint64_t ns)
{
#if defined(SWANSTATION_PS5)
	if (withUsleep.load(std::memory_order_relaxed))
	{
		usleepFor(ns);
		return;
	}
#endif
	nanosleepFor(ns);
}

}

void startProbe()
{
	static std::atomic<bool> started{ false };
	if (started.exchange(true))
		return;
	std::thread([] {
		const Measured nano = measure([](uint64_t ns) { return nanosleepFor(ns); }, nowNs);
#if defined(SWANSTATION_PS5)
		const Measured usl = measure([](uint64_t ns) { return usleepFor(ns); }, nowNs);
#else
		const Measured usl;	// the PC has nanosleep alone
#endif
		const bool second = preferSecond(nano, usl);
		const Measured& taken = second ? usl : nano;
		const uint64_t margin = taken.ok ? marginFor(taken.lateness) : MARGIN_DEFAULT_NS;
		withUsleep.store(second, std::memory_order_relaxed);
		marginNs.store(margin, std::memory_order_relaxed);
		diag::mark("timing: sleeps wake late by %.2f ms typically, %.2f ms at worst (nanosleep %.2f/%.2f, "
			"sceKernelUsleep %.2f/%.2f ms at 90%%/worst, %s taken); pacing by the clock waits the last %.2f ms awake",
			taken.p50 / 1e6, taken.worst / 1e6, nano.p90 / 1e6, nano.worst / 1e6, usl.p90 / 1e6, usl.worst / 1e6,
			second ? "sceKernelUsleep" : "nanosleep", margin / 1e6);
	}).detach();
}

void waitUntilSeconds(double end)
{
	if (end <= 0)
		return;
	Waits w;
	waitUntil((uint64_t)(end * 1e9), marginNs.load(std::memory_order_relaxed), nowNs, sleepFor,
		[] { sched_yield(); }, [] {
#if defined(__x86_64__)
			__builtin_ia32_pause();
#endif
		}, w);
	if (w.waits == 0)
		return;
	std::lock_guard<std::mutex> lock(waitsLock);
	waitsSoFar.add(w.asleepNs, w.awakeNs, w.late ? w.latestNs : 0);
}

std::string takeLine()
{
	Waits w;
	{
		std::lock_guard<std::mutex> lock(waitsLock);
		w = waitsSoFar;
		waitsSoFar = Waits();
	}
	if (w.waits == 0)
		return std::string();
	const double waited = (double)(w.asleepNs + w.awakeNs);
	char line[320];
	std::snprintf(line, sizeof(line),
		"pacing by the clock: %llu waits, %.2f ms asleep and %.2f ms awake each (%.0f%% awake, yielding but "
		"for the last %.2f ms); %llu woke late, by %.2f ms at most",
		(unsigned long long)w.waits, w.asleepNs / 1e6 / w.waits, w.awakeNs / 1e6 / w.waits,
		waited > 0 ? 100.0 * w.awakeNs / waited : 0.0, SPIN_LAST_NS / 1e6, (unsigned long long)w.late,
		w.latestNs / 1e6);
	return line;
}

std::string clockLine()
{
#if defined(SWANSTATION_PS5)
	char line[256];
	ps5px_clock_line(line, sizeof(line));
	return line;
#else
	return std::string();
#endif
}

}
