/*
	PSSwanStation - videos on the PC, for the tests: the PC's ffmpeg program
	decodes them (it is not part of the title, and the console build has none
	of this; ps5/video_ps5.cpp plays them there).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	Two ffmpeg processes per video: one writes its pictures as NV12, one its
	sound as 48 kHz stereo 16-bit samples. A picture is taken every
	1/frame-rate seconds of the interface's clock (a test run's clock counts
	frames, so the run is the same each time), and a sixtieth of a second of
	sound with each frame.
*/
#include "../fe.h"
#include "../video_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace fe::video::backend
{
namespace
{
struct Host
{
	FILE *pictures = nullptr, *sound = nullptr;
	int width = 0, height = 0;
	double fps = 30, shown = 0;		// pictures due, and taken
	double startedAt = 0;
	bool loop = false, ended = false;
	std::vector<uint8_t> frame;
	std::vector<int16_t> samples;
	double soundDue = 0;
} host;

std::string quoted(const std::string& s)
{
	std::string out = "'";
	for (const char c : s)
		out += c == '\'' ? std::string("'\\''") : std::string(1, c);
	return out + "'";
}

void closePipes()
{
	if (host.pictures != nullptr)
		pclose(host.pictures);
	if (host.sound != nullptr)
		pclose(host.sound);
	host.pictures = host.sound = nullptr;
}
}

const char *name()
{
	return "the PC's ffmpeg (tests)";
}

bool broken()
{
	return false;
}

bool load(std::string& why)
{
	if (system("ffmpeg -version >/dev/null 2>&1") != 0 || system("ffprobe -version >/dev/null 2>&1") != 0)
	{
		why = "this PC has no ffmpeg";
		return false;
	}
	return true;
}

bool open(const std::string& path, bool loop, std::string& why)
{
	close();
	const std::string probe = "ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate "
			"-of csv=p=0 " + quoted(path) + " 2>/dev/null";
	FILE *p = popen(probe.c_str(), "r");
	char line[256] = "";
	if (p == nullptr || fgets(line, sizeof(line), p) == nullptr)
	{
		if (p != nullptr)
			pclose(p);
		why = "ffprobe could not read it";
		return false;
	}
	pclose(p);
	int num = 30, den = 1;
	if (std::sscanf(line, "%d,%d,%d/%d", &host.width, &host.height, &num, &den) < 2 || host.width <= 0)
	{
		why = "no picture in it";
		return false;
	}
	host.width &= ~1;
	host.height &= ~1;
	host.fps = den > 0 && num > 0 ? (double)num / den : 30.0;
	const std::string again = loop ? "-stream_loop -1 " : "";
	const std::string size = std::to_string(host.width) + "x" + std::to_string(host.height);
	host.pictures = popen(("ffmpeg -v error " + again + "-i " + quoted(path) + " -f rawvideo -pix_fmt nv12 -s " + size
			+ " - 2>/dev/null").c_str(), "r");
	host.sound = popen(("ffmpeg -v error " + again + "-i " + quoted(path) + " -vn -f s16le -ac 2 -ar 48000 - 2>/dev/null").c_str(), "r");
	host.frame.assign((size_t)host.width * host.height * 3 / 2, 0);
	host.loop = loop;
	host.ended = false;
	host.shown = 0;
	host.startedAt = now();
	host.soundDue = 0;
	return host.pictures != nullptr;
}

void close()
{
	closePipes();
	host.ended = false;
}

bool take(Picture& out)
{
	if (host.pictures == nullptr || host.ended)
		return false;
	const double due = (now() - host.startedAt) * host.fps;
	if (host.shown > 0 && due < host.shown)
		return false;
	// The pictures due since the last one: the older ones are read and passed over.
	do
	{
		if (fread(host.frame.data(), 1, host.frame.size(), host.pictures) != host.frame.size())
		{
			host.ended = true;
			return false;
		}
		host.shown++;
	} while (host.shown < due);
	out.y = host.frame.data();
	out.uv = host.frame.data() + (size_t)host.width * host.height;
	out.width = host.width;
	out.height = host.height;
	out.pitch = out.uvPitch = host.width;
	return true;
}

void drainAudio(const std::function<void(const int16_t *, size_t, int, int)>& sink)
{
	if (host.sound == nullptr)
		return;
	const double due = (now() - host.startedAt) * 48000.0;
	const size_t frames = due > host.soundDue ? (size_t)(due - host.soundDue) : 0;
	if (frames == 0)
		return;
	host.samples.resize(std::min<size_t>(frames, 48000) * 2);
	const size_t got = fread(host.samples.data(), 4, host.samples.size() / 2, host.sound);
	host.soundDue += (double)frames;
	if (got > 0 && sink)
		sink(host.samples.data(), got, 2, 48000);
}

bool active()
{
	return host.pictures != nullptr && !host.ended;
}

} // namespace fe::video::backend
