/*
	PSSwanStation - audio through libSceAudioOut.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The port is opened as PSFlyCast and PS5 RetroArch open it: the system user
	(255), the main port, 256-frame grains at 48 kHz, 16-bit stereo, with a
	thread of its own feeding sceAudioOutOutput, which blocks until the previous
	grain has been taken. The thread asks audio.cpp for each grain; an empty
	ring plays silence rather than stalling the port. A call that fails does
	not block, so the thread then keeps the grains' own time itself (as
	OGBox's sound output does), instead of spinning and emptying the ring;
	the boot log says so.
*/
#include "fe.h"
#include "../tuning.h"

#include <atomic>
#include <chrono>
#include <thread>

extern "C"
{
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t userId, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioOutOutput(int32_t handle, const void *p);
int sceAudioOutClose(int32_t handle);
}

namespace fe::platform
{
namespace
{
constexpr uint32_t Grain = 256;
int port = -1;
std::atomic<bool> stop{false};
std::thread thread;
}

bool audioOpen()
{
	if (port >= 0)
		return true;
	static bool libraryReady;
	if (!libraryReady)
	{
		const int rc = sceAudioOutInit();
		diag::mark("audio: sceAudioOutInit %#x", (unsigned)rc);
		libraryReady = true;
	}
	port = sceAudioOutOpen(255, 0, 0, Grain, 48000, 1);
	diag::mark("audio: sceAudioOutOpen %#x", (unsigned)port);
	if (port < 0)
		return false;
	stop = false;
	thread = std::thread([] {
		tuning::setThread(tuning::Role::Sound);
		alignas(64) int16_t grain[Grain * 2];
		using Clock = std::chrono::steady_clock;
		const auto period = std::chrono::nanoseconds(1'000'000'000LL * Grain / 48000);
		Clock::time_point due{};
		uint64_t failed = 0;
		Clock::time_point toldAt{};
		while (!stop)
		{
			audio::render(grain, Grain);
			const int rc = sceAudioOutOutput(port, grain);
			if (rc >= 0)
			{
				due = {};
				continue;
			}
			// The grain was not taken and nothing waited: its time is kept here.
			failed++;
			const auto now = Clock::now();
			if (failed <= 3 || now - toldAt >= std::chrono::seconds(10))
			{
				diag::mark("audio: sceAudioOutOutput failed (%#x), %llu time(s) so far; the grains are timed by the clock",
						(unsigned)rc, (unsigned long long)failed);
				toldAt = now;
			}
			due = due == Clock::time_point{} || due < now - period * 4 ? now + period : due + period;
			std::this_thread::sleep_until(due);
		}
	});
	return true;
}

void audioClose()
{
	if (port < 0)
		return;
	stop = true;
	if (thread.joinable())
		thread.join();
	sceAudioOutOutput(port, nullptr);
	sceAudioOutClose(port);
	port = -1;
}

}
