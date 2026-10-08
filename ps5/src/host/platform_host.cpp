/*
	PSSwanStation - the host build's platform: a PC, for test runs.

	SPDX-License-Identifier: GPL-3.0-or-later

	No pad, no sound card and no shell are needed. The pad is a script (the
	SWANSTATION_SCRIPT file: "frame button[+button] [hold-frames]" lines, and
	"frame shot name" to save a screenshot), the sound goes nowhere (a thread
	takes it at the output's pace, as the console's does) and files live in the
	folder SWANSTATION_ROOT names. HTTP is the PC's curl program, so that what
	downloads (covers, the cheat database, updates, achievements) can be run
	against a server on the same PC. SWANSTATION_OUTSIDE names the folder that
	stands for /data/psswanstation, "frame tilt X [Y]" in the script tilts the
	first pad, and SWANSTATION_INPUT_LOG writes what the game is given to the
	log twice a second.
*/
#include "fe.h"
#include "display.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
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
	bool tilt = false;	// "tilt X Y": the pad leans that far, as parts of gravity
	float tiltX = 0, tiltY = 0;
};
std::vector<Step> script;
Pad pads[MaxPads];
std::atomic<bool> audioStop{false};
std::thread audioThread;
bool safeStart, outsideUsed;
std::string outsideWhy;
bool motionOn;

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
	appDir = rootDir;
	options::loadFrontend();
	safeStart = storage::startBegan(appDir);
	const int filesAt = options::frontend().filesAt;
	if (filesAt == 2)
	{
		// A test's "USB drive": the folder SWANSTATION_USB_FILES names.
		const char *drive = getenv("SWANSTATION_USB_FILES");
		if (drive == nullptr || safeStart)
			outsideWhy = safeStart ? "the last start did not finish, so this one stayed in the sandbox"
					: "no USB drive that can be written to is plugged in";
		else
		{
			std::string folder = std::string(drive) + "/" + storage::UsbFolder + "/";
			storage::migrate(appDir, folder);
			rootDir = folder;
			outsideUsed = true;
		}
	}
	else if (filesAt == 3)
		outsideUsed = true;
	else if (filesAt == 1)
	{
		const char *outside = getenv("SWANSTATION_OUTSIDE");
		if (outside == nullptr || safeStart)
			outsideWhy = safeStart ? "the last start did not finish, so this one stayed in the sandbox"
					: "the sandbox could not be left (it needs a resident Lapy service, or the ELF loader on port 9021)";
		else
		{
			std::string folder = outside;
			if (folder.back() != '/')
				folder += '/';
			storage::migrate(appDir, folder);
			rootDir = folder;
			outsideUsed = true;
		}
	}
	diag::open(rootDir);
	diag::mark("%s host build %d; title folder: %s; files: %s", AppName, BuildNumber, appDir.c_str(), rootDir.c_str());
	storage::makeFolders(rootDir);
	if (fileExists(appDir + "sce_sys/param.json"))
		storage::syncDisplayMode(appDir + "sce_sys/param.json", options::frontend().displayMode);
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
				else if (what == "tilt")
				{
					step.tilt = true;
					words >> step.tiltX >> step.tiltY;
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
	storage::startCompleted(appDir);
	fflush(nullptr);
	_Exit(0);
}

void padPoll()
{
	const uint64_t frame = display::frameCount();
	uint32_t held[MaxPads] = {};
	bool present[MaxPads] = { true };
	float tiltX = 0, tiltY = 0;
	for (const Step& step : script)
	{
		if (step.tilt)
		{
			if (frame >= step.frame)
			{
				tiltX = step.tiltX;
				tiltY = step.tiltY;
			}
			continue;
		}
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
	// The first pad lies flat, face up, unless the script leans it.
	pads[0].hasMotion = motionOn;
	pads[0].gravity[0] = motionOn ? tiltX : 0.f;
	pads[0].gravity[2] = motionOn ? tiltY : 0.f;
	pads[0].gravity[1] = motionOn ? std::sqrt(std::max(0.f, 1.f - tiltX * tiltX - tiltY * tiltY)) : 0.f;
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

void padLight(int index, uint8_t red, uint8_t green, uint8_t blue)
{
	static uint32_t sent[MaxPads];
	const uint32_t colour = ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
	if (index < 0 || index >= MaxPads || sent[index] == colour)
		return;
	sent[index] = colour;
	diag::mark("pad: %d: light bar %06x", index + 1, colour);
}

void padMotion(bool on)
{
	motionOn = on;
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

namespace
{
// The PC's curl, when the test asks for HTTP at all (SWANSTATION_HTTP=1): a
// test that does not must not depend on a network.
bool httpWanted()
{
	return getenv("SWANSTATION_HTTP") != nullptr;
}

std::string quoted(const std::string& text)
{
	std::string out = "'";
	for (const char c : text)
		out += c == '\'' ? std::string("'\\''") : std::string(1, c);
	return out + "'";
}
}

int httpGet(const std::string& url, std::vector<uint8_t>& out, unsigned seconds, int *error)
{
	out.clear();
	if (error != nullptr)
		*error = 0;
	if (!httpWanted())
		return -1;
	// The body, then the status on a line of its own.
	const std::string command = format("curl -sS -L --max-time %u -A PSSwanStation/1.0 -w '\\n%%{http_code}' %s 2>/dev/null",
			seconds, quoted(url).c_str());
	FILE *pipe = popen(command.c_str(), "r");
	if (pipe == nullptr)
		return -1;
	uint8_t block[65536];
	size_t got;
	while ((got = fread(block, 1, sizeof(block), pipe)) > 0)
		out.insert(out.end(), block, block + got);
	if (pclose(pipe) != 0)
	{
		out.clear();
		return -1;
	}
	size_t line = out.size();
	while (line > 0 && out[line - 1] != '\n')
		line--;
	const int status = atoi(std::string(out.begin() + (long)line, out.end()).c_str());
	out.resize(line > 0 ? line - 1 : 0);
	return status > 0 ? status : -1;
}

int httpDownload(const std::string& url, const std::string& path, uint64_t limit,
		const std::function<bool(uint64_t done, uint64_t total)>& progress)
{
	if (!httpWanted())
		return -1;
	// curl writes the file; this watches it grow.
	const std::string command = format("curl -sS -L --max-time 3600 -A PSSwanStation/1.0 --max-filesize %llu -o %s "
			"-w '%%{http_code}' %s 2>/dev/null", (unsigned long long)limit, quoted(path).c_str(), quoted(url).c_str());
	std::atomic<bool> done{false};
	std::atomic<bool> stopped{false};
	std::thread watcher([&] {
		while (!done)
		{
			struct stat st;
			if (progress && stat(path.c_str(), &st) == 0 && !progress((uint64_t)st.st_size, 0))
			{
				stopped = true;
				// The only curl this test run has going.
				if (system("pkill -f 'curl -sS -L --max-time 3600' >/dev/null 2>&1") != 0) {}
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	});
	FILE *pipe = popen(command.c_str(), "r");
	char text[32] = "";
	if (pipe != nullptr && fgets(text, sizeof(text), pipe) == nullptr)
		text[0] = 0;
	const int result = pipe != nullptr ? pclose(pipe) : -1;
	done = true;
	watcher.join();
	const int status = atoi(text);
	if (stopped || result != 0 || status != 200)
	{
		unlink(path.c_str());
		return stopped || status <= 0 || status == 200 ? -1 : status;
	}
	chmod(path.c_str(), 0666);
	return 200;
}

bool httpAvailable()
{
	return httpWanted();
}

bool outsideAvailable()
{
	return outsideUsed;
}

std::string outsideProblem()
{
	return outsideWhy;
}

bool startedSafely()
{
	return safeStart;
}

int localTimeOffset()
{
	const time_t t = time(nullptr);
	struct tm local;
	localtime_r(&t, &local);
	return (int)local.tm_gmtoff;
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

std::string fe::shownApp()
{
	return appDir;
}
