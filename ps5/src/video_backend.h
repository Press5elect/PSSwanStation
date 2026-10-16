/*
	PSSwanStation - what plays a video for video.cpp: the console's player
	(ps5/video_ps5.cpp) or, on the PC, its ffmpeg (host/video_host.cpp).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	All of it is called from the frame's thread.
*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace fe::video::backend
{

// A decoded picture: NV12, a brightness plane and a colour plane (U and V
// side by side) at half the size.
struct Picture
{
	const uint8_t *y = nullptr, *uv = nullptr;
	int width = 0, height = 0;
	int pitch = 0, uvPitch = 0;		// bytes from one row to the next
};

// Once: whether videos can be played, and why not.
bool load(std::string& why);
const char *name();
// The player failed in a way that will not mend this run.
bool broken();
bool open(const std::string& path, bool loop, std::string& why);
void close();
// The newest picture since the last call (valid until the next call or close).
bool take(Picture& out);
// The sound decoded since the last call, given to `sink` (when it is set)
// as interleaved 16-bit samples; dropped when it is not.
void drainAudio(const std::function<void(const int16_t *samples, size_t frames, int channels, int rate)>& sink);
// False once it has played to its end (and does not loop).
bool active();

} // namespace fe::video::backend
