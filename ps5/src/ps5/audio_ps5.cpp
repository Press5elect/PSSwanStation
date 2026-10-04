/*
	SwanStation for PS5 - audio through libSceAudioOut.

	SPDX-License-Identifier: GPL-3.0-or-later

	The port is opened as PSFlyCast and PS5 RetroArch open it: the system user
	(255), the main port, 256-frame grains at 48 kHz, 16-bit stereo, with a
	thread of its own feeding sceAudioOutOutput, which blocks until the previous
	grain has been taken. The thread asks audio.cpp for each grain; an empty
	ring plays silence rather than stalling the port.
*/
#include "fe.h"

#include <atomic>
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
		alignas(64) int16_t grain[Grain * 2];
		while (!stop)
		{
			audio::render(grain, Grain);
			sceAudioOutOutput(port, grain);
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
