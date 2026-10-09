/*
	PSSwanStation - recordings of what the players pressed (recorder.cpp).

	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fe::recorder
{

constexpr int Ports = 4;

// What one port gives the emulator in one frame.
struct PortInput
{
	uint16_t buttons = 0;			// libretro's joypad bits
	int16_t lx = 0, ly = 0, rx = 0, ry = 0;
	int16_t l2 = 0, r2 = 0;			// how far the triggers are pressed, 0..32767
	int16_t aimX = 0, aimY = 0;		// a light gun's aim, -32767..32767
	uint8_t connected = 0;			// a pad is there and its buttons are the game's
};

// What a recording is of.
struct Info
{
	std::string serial, title, build;
	uint64_t frames = 0;
};

enum Mode { Off, Recording, Playing };

// A new recording at `path`, beginning from `state` (a state file's contents).
bool startRecording(const std::string& path, const std::vector<uint8_t>& state, const Info& info);
// One frame's inputs, while recording.
void record(const PortInput (&inputs)[Ports]);
// Opens a recording: the state it begins from comes back in `state`.
bool startPlayback(const std::string& path, std::vector<uint8_t>& state, Info& info);
// The next frame's inputs, while playing back. False at the end.
bool next(PortInput (&inputs)[Ports]);
// Ends either.
void stop();
Mode mode();
// Frames recorded, or played back so far, and (playing) how many there are.
uint64_t frame();
uint64_t frames();
const std::string& path();
// What the file at `path` is a recording of; false when it is not one.
bool read(const std::string& path, Info& info);
// The recordings in a folder, the newest first.
std::vector<std::string> list(const std::string& folder);

}
