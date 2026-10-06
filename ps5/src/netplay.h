/*
	PSSwanStation - netplay: two consoles play one game over the network.

	SPDX-License-Identifier: GPL-3.0-or-later

	Lockstep with an input delay. Both sides run the same emulator on the same
	disc from the same state; each frame both send what their player pressed,
	for the frame `delay` frames ahead, and a frame only runs once both
	players' buttons for it are there. Nothing else is exchanged while it
	plays, except a checksum of the emulated memory now and then: if the two
	differ, the hosting side sends its state again and both go on from it.

	One side hosts (listens on a TCP port), the other joins it by address. The
	hosting side is player 1.

	The module knows nothing about the emulator: host.cpp gives it the three
	things it needs (Hooks) and asks it each frame what to do (step).
*/
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fe::netplay
{

constexpr int DefaultPort = 28800;
constexpr int Players = 2;

// What one player holds in one frame.
struct Input
{
	uint16_t buttons = 0;			// libretro's joypad bits (RETRO_DEVICE_ID_JOYPAD_*)
	int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

struct Hooks
{
	// The emulator's whole state, as retro_serialize gives it.
	std::function<bool(std::vector<uint8_t>& out)> saveState;
	std::function<bool(const uint8_t *data, size_t size)> loadState;
	// A checksum of the emulated memory (the same on both sides while they
	// are in step).
	std::function<uint32_t()> checksum;
};

// What must be the same on both sides before a game can be played together.
struct Session
{
	std::string game;			// the disc's serial
	std::string bios;			// which BIOS image runs (a name or a checksum)
	std::string settings;		// the emulator settings that matter, "key=value" lines; the host's are sent to the guest
	int delay = 2;				// frames between pressing and seeing (1..10); the host's is used
	int port = DefaultPort;
};

enum class State
{
	Off,
	Listening,		// hosting: waiting for the other player
	Connecting,		// joining: reaching the host
	Greeting,		// both there: versions, game and BIOS are compared
	Syncing,		// the host's state travels to the guest
	Playing,
	Ended,			// the other side left, or stop() was called
	Failed,			// see error()
};

// Starts hosting. The session's settings text is what the guest will be given.
bool host(const Session& session, const Hooks& hooks);
// Starts joining the host at `address` ("192.168.1.20" or "192.168.1.20:28800").
bool join(const std::string& address, const Session& session, const Hooks& hooks);
// Ends it and tells the other side. Safe to call in any state.
void stop();

State state();
bool active();					// anything but Off, Ended and Failed
bool hosting();
int localPlayer();				// 0 or 1
std::string error();			// for the screen, when Failed (or Ended by the other side)
// The settings text the host sent (the guest applies it before it loads the
// state); empty on the host and before Greeting is over.
std::string hostSettings();
// This machine's address on the local network ("192.168.1.20"), or empty.
std::string localAddress();
// Milliseconds a packet takes there and back, averaged; -1 when not known.
int ping();
// Seconds since anything came from the other side, once that is 5 or more
// (for the status line); 0 while it answers. At 20 the session ends.
int silentSeconds();
// How many times the two were brought back in step.
unsigned resyncs();
// A menu is open on this side: the other side is told, and waits.
void setPaused(bool paused);
bool remotePaused();

// What the frame loop must do now.
enum class Step
{
	Idle,		// nothing to run: not playing yet (show state()), or it is over
	Wait,		// playing, but the other player's buttons for the next frame have not come: run nothing this refresh
	Run,		// run one emulated frame with `inputs`
};
// Called once per display refresh by the emulator's thread with what the
// local player holds now. When it answers Run, `inputs[0]` and `inputs[1]`
// are players 1 and 2 for the frame to run; call ran() after running it.
// While Syncing it also does the state transfer through the hooks (on the
// caller's thread, which is the emulator's).
Step step(const Input& local, Input inputs[Players]);
void ran();
// The emulated frame count since the two were last in step from frame 0.
uint64_t frame();

}
