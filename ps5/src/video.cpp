/*
	PSSwanStation - videos for ES-DE themes: one played at a time, its
	pictures made the interface's, its sound mixed into the output.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The decoding is not done here. On the console the console's own video
	player does it (AvPlayer, ps5/video_ps5.cpp): Sony's licensed H.264 and
	AAC decoders, on the console's video hardware, so no decoder of a
	patented format is part of the title. On the PC, for the tests, the PC's
	ffmpeg program does it (host/video_host.cpp). Either gives the newest
	picture as NV12 (a plane of brightness, then one of colour at half the
	size) and the sound as 16-bit samples; this file turns the picture into
	the interface's colours (BT.601, as video of this size is) and hands the
	sound to the output (audio::pushStream).

	A video no one asked for in the last frames is stopped, so leaving the
	library (a game starts) ends it.
*/
#include "fe.h"
#include "display.h"
#include "video_backend.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace fe::video
{
namespace
{

struct State
{
	bool loaded = false, usable = false;
	std::string why;
	std::string path;
	bool loop = false, sound = false, open = false, finished = false;
	uint64_t wantedAt = 0;
	display::Texture *texture = nullptr;
	int width = 0, height = 0;
	bool hasPicture = false;
	double openedAt = 0;
	std::vector<uint32_t> pixels;
	unsigned pictures = 0;
};
State state;

bool ensureLoaded()
{
	if (!state.loaded)
	{
		state.loaded = true;
		state.usable = backend::load(state.why);
		diag::mark("video: %s", state.usable ? backend::name() : ("no videos: " + state.why).c_str());
	}
	return state.usable;
}

inline uint8_t clampByte(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// NV12 to the interface's XRGB8888, BT.601 with video levels (16-235).
void convert(const backend::Picture& p, std::vector<uint32_t>& out)
{
	out.resize((size_t)p.width * p.height);
	for (int y = 0; y < p.height; y++)
	{
		const uint8_t *row = p.y + (size_t)y * p.pitch;
		const uint8_t *uv = p.uv + (size_t)(y / 2) * p.uvPitch;
		uint32_t *dst = &out[(size_t)y * p.width];
		for (int x = 0; x < p.width; x++)
		{
			const int c = ((int)row[x] - 16) * 298;
			const int u = (int)uv[(x & ~1)] - 128, v = (int)uv[(x & ~1) + 1] - 128;
			const uint8_t r = clampByte((c + 409 * v + 128) >> 8);
			const uint8_t g = clampByte((c - 100 * u - 208 * v + 128) >> 8);
			const uint8_t b = clampByte((c + 516 * u + 128) >> 8);
			dst[x] = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
		}
	}
}

void closeNow()
{
	if (state.open)
		backend::close();
	state.open = false;
	state.hasPicture = false;
	state.finished = false;
	audio::clearStream();
}

} // namespace

bool available()
{
	return ensureLoaded();
}

void play(const std::string& path, bool loop, bool sound)
{
	state.wantedAt = display::frameCount();
	if (path == state.path && state.open)
	{
		if (sound != state.sound)
		{
			state.sound = sound;
			if (!sound)
				audio::clearStream();
		}
		return;
	}
	closeNow();
	state.path = path;
	state.loop = loop;
	state.sound = sound;
	if (path.empty() || !ensureLoaded())
		return;
	std::string why;
	state.openedAt = now();
	state.open = backend::open(path, loop, why);
	if (!state.open)
	{
		diag::mark("video: %s cannot be played: %s", path.substr(path.rfind('/') + 1).c_str(), why.c_str());
		state.finished = true;
		// The player itself may be what fails: once it has, no more videos this run.
		if (backend::broken())
			state.usable = false;
	}
}

void stop()
{
	closeNow();
	state.path.clear();
}

const std::string& current()
{
	return state.path;
}

bool ended()
{
	return state.finished;
}

void *picture(int& width, int& height)
{
	state.wantedAt = display::frameCount();
	if (!state.open)
		return nullptr;
	backend::Picture p;
	if (backend::take(p) && p.width > 0 && p.height > 0 && p.y != nullptr && p.uv != nullptr)
	{
		convert(p, state.pixels);
		if (state.texture == nullptr || state.width != p.width || state.height != p.height)
		{
			display::destroyTexture(state.texture);
			state.texture = display::createDynamicTexture(p.width, p.height);
			state.width = p.width;
			state.height = p.height;
		}
		if (state.texture != nullptr)
		{
			display::updateTexture(state.texture, state.pixels.data(), p.width, p.height, (size_t)p.width * 4,
					display::Xrgb8888);
			state.hasPicture = true;
			if (state.pictures++ == 0)
				diag::mark("video: the first picture (%d x %d, %s)", p.width, p.height, backend::name());
		}
	}
	if (state.sound)
		backend::drainAudio([](const int16_t *samples, size_t frames, int channels, int rate) {
			audio::pushStream(samples, frames, channels, rate);
		});
	else
		backend::drainAudio({});
	// Not playing any more: at its end, or (nothing shown after a while) it never started.
	if (!backend::active() && (state.hasPicture || now() - state.openedAt > 8.0))
		state.finished = true;
	width = state.width;
	height = state.height;
	return state.hasPicture && state.texture != nullptr ? display::textureId(state.texture) : nullptr;
}

void frame()
{
	if (state.open && state.wantedAt + 2 < display::frameCount())
		stop();
}

std::string status()
{
	if (!state.loaded)
		return "Not used yet";
	if (!state.usable)
		return "Not available: " + state.why;
	return backend::name();
}

} // namespace fe::video
