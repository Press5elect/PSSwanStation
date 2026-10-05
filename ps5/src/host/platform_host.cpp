/*
	PSSwanStation - the host build's platform: a PC, for test runs.

	SPDX-License-Identifier: GPL-3.0-or-later

	No pad, no sound card and no shell are needed. The pad is a script (the
	SWANSTATION_SCRIPT file: "frame button[+button] [hold-frames]" lines, and
	"frame shot name" to save a screenshot), the sound goes nowhere (a thread
	takes it at the output's pace, as the console's does) and files live in the
	folder SWANSTATION_ROOT names.
*/
#include "fe.h"
#include "display.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <thread>

namespace fe::platform
{
namespace
{
struct Step
{
	uint64_t frame;
	uint32_t buttons;
	uint64_t hold;
	std::string shot;
	float lx = 0, ly = 0;
	int pad = 0;		// "2:cross" is the second player's
};
std::vector<Step> script;
Pad pads[MaxPads];
std::atomic<bool> audioStop{false};
std::thread audioThread;

uint32_t buttonByName(const std::string& name)
{
	static const std::map<std::string, uint32_t> names = {
		{ "cross", Cross }, { "circle", Circle }, { "square", Square }, { "triangle", Triangle },
		{ "l1", L1 }, { "r1", R1 }, { "l2", L2 }, { "r2", R2 }, { "l3", L3 }, { "r3", R3 },
		{ "up", Up }, { "down", Down }, { "left", Left }, { "right", Right }, { "options", Options },
		{ "touchleft", TouchLeft }, { "touchright", TouchRight }, { "select", TouchLeft }, { "start", TouchRight },
	};
	const auto it = names.find(lowercase(name));
	return it == names.end() ? 0 : it->second;
}
}

void earlyInit()
{
	diag::installCrashHandler();
	const char *root = getenv("SWANSTATION_ROOT");
	rootDir = root != nullptr ? root : "./swanstation-root";
	if (rootDir.back() != '/')
		rootDir += '/';
	makeDir(rootDir);
	options::loadFrontend();
	diag::open(rootDir);
	diag::mark("%s host build %d; root folder: %s", AppName, BuildNumber, rootDir.c_str());
	for (const char *sub : { "bios", "games", "covers", "cheats", "data", "logs", "data/saves", "data/states",
			"data/game-options", "data/cheats", "data/cache" })
		makeDir(rootDir + sub);
	if (const char *file = getenv("SWANSTATION_SCRIPT"))
	{
		std::vector<uint8_t> text;
		if (readFile(file, text))
		{
			std::istringstream in(std::string(text.begin(), text.end()));
			std::string line;
			while (std::getline(in, line))
			{
				std::istringstream words(line);
				Step step{};
				std::string what;
				if (!(words >> step.frame >> what) || what.empty() || what[0] == '#')
					continue;
				if (what == "shot")
				{
					words >> step.shot;
				}
				else
				{
					if (what.size() > 2 && what[1] == ':' && what[0] >= '1' && what[0] <= '0' + MaxPads)
					{
						step.pad = what[0] - '1';
						what = what.substr(2);
					}
					std::istringstream parts(what);
					std::string part;
					while (std::getline(parts, part, '+'))
						step.buttons |= buttonByName(part);
					step.hold = 2;
					words >> step.hold;
				}
				script.push_back(step);
			}
		}
		diag::mark("script: %d steps", (int)script.size());
	}
}

void lateInit()
{
}

[[noreturn]] void quit()
{
	diag::mark("quit");
	fflush(nullptr);
	_Exit(0);
}

void padPoll()
{
	const uint64_t frame = display::frameCount();
	uint32_t held[MaxPads] = {};
	bool present[MaxPads] = { true };
	for (const Step& step : script)
	{
		// A pad the script names is connected from its first step on.
		if (step.shot.empty() && frame + 60 >= step.frame)
			present[step.pad] = true;
		if (!step.shot.empty())
		{
			if (step.frame == frame)
			{
				const std::string path = rootDir + step.shot + ".png";
				diag::mark("shot %s: %s", step.shot.c_str(), display::saveScreenshot(path) ? "saved" : "failed");
			}
			continue;
		}
		if (frame >= step.frame && frame < step.frame + step.hold)
			held[step.pad] |= step.buttons;
	}
	for (int i = 0; i < MaxPads; i++)
	{
		Pad& pad = pads[i];
		const uint32_t before = pad.buttons;
		pad.connected = present[i];
		pad.buttons = held[i];
		pad.pressed = held[i] & ~before;
		pad.released = before & ~held[i];
	}
}

const Pad& pad(int index)
{
	static const Pad none;
	return index >= 0 && index < MaxPads ? pads[index] : none;
}

int padCount()
{
	int n = 0;
	for (const Pad& pad : pads)
		n += pad.connected;
	return n;
}

void padRumble(int, float, float)
{
}

bool audioOpen()
{
	audioStop = false;
	audioThread = std::thread([] {
		int16_t grain[256 * 2];
		auto next = std::chrono::steady_clock::now();
		// A test can ask for what would have been heard: raw 48 kHz stereo.
		const char *dump = getenv("SWANSTATION_AUDIO_DUMP");
		FILE *heard = dump != nullptr ? fopen(dump, "wb") : nullptr;
		while (!audioStop)
		{
			audio::render(grain, 256);
			if (heard != nullptr)
				fwrite(grain, 4, 256, heard);
			next += std::chrono::microseconds(256 * 1000000ll / 48000);
			std::this_thread::sleep_until(next);
		}
		if (heard != nullptr)
			fclose(heard);
	});
	return true;
}

void audioClose()
{
	audioStop = true;
	if (audioThread.joinable())
		audioThread.join();
}

int httpGet(const std::string&, std::vector<uint8_t>& out, unsigned, int *error)
{
	out.clear();
	if (error != nullptr)
		*error = 0;
	return -1;
}

bool httpAvailable()
{
	return false;
}

bool usbAvailable()
{
	return getenv("SWANSTATION_USB") != nullptr;
}

std::vector<std::string> usbGameDirs()
{
	std::vector<std::string> dirs;
	if (const char *dir = getenv("SWANSTATION_USB"))
		dirs.push_back(dir);
	return dirs;
}

uint64_t freeMemory()
{
	return 0;
}

bool jitAvailable()
{
	return getenv("SWANSTATION_NO_JIT") == nullptr;
}

}

std::string fe::shownRoot()
{
	return rootDir;
}
