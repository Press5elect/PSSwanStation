/*
	PSSwanStation - the title's main: start-up order, the frame loop.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	One thread does everything a frame needs, in this order: the pads, the
	next swapchain image, the emulator (one display refresh of it, unless a
	menu is over the game), the interface, the present. The present waits for
	the display (FIFO), and that wait is what paces the emulator.
*/
#include "fe.h"
#include "display.h"
#include "update.h"
#include "web.h"
#if defined(SWANSTATION_HOST)
#include "slangimport.h"
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

namespace fe
{

std::string rootDir;
std::string appDir;
// Counts the builds handed over; the About page and the boot log show it.
const int BuildNumber = 24;
// The day the build was configured (ps5/CMakeLists.txt).
const char *const BuildDate = FE_BUILD_DATE;

// ------------------------------------------------------------------ helpers

std::string format(const char *fmt, ...)
{
	char small[512];
	va_list args;
	va_start(args, fmt);
	va_list copy;
	va_copy(copy, args);
	const int length = vsnprintf(small, sizeof(small), fmt, args);
	va_end(args);
	std::string text;
	if (length < 0)
		text = fmt;
	else if ((size_t)length < sizeof(small))
		text.assign(small, (size_t)length);
	else
	{
		text.resize((size_t)length + 1);
		vsnprintf(text.data(), text.size(), fmt, copy);
		text.resize((size_t)length);
	}
	va_end(copy);
	return text;
}

bool fileExists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dirExists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void makeDir(const std::string& path)
{
	if (path.empty() || dirExists(path))
		return;
	std::string clean = path;
	while (clean.size() > 1 && clean.back() == '/')
		clean.pop_back();
	const size_t slash = clean.rfind('/');
	if (slash != std::string::npos && slash > 0)
		makeDir(clean.substr(0, slash));
	if (mkdir(clean.c_str(), 0777) == 0)
		// Whatever the umask is: FTP clients must be able to write here.
		chmod(clean.c_str(), 0777);
}

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
	out.clear();
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	uint8_t block[65536];
	size_t got;
	while ((got = fread(block, 1, sizeof(block), f)) > 0)
		out.insert(out.end(), block, block + got);
	const bool ok = ferror(f) == 0;
	fclose(f);
	return ok;
}

bool writeFile(const std::string& path, const void *data, size_t bytes)
{
	// Written beside the file and renamed over it: a crash or a full disk
	// leaves the old one whole.
	const std::string temporary = path + ".tmp";
	FILE *f = fopen(temporary.c_str(), "wb");
	if (f == nullptr)
		return false;
	const bool written = bytes == 0 || fwrite(data, 1, bytes, f) == bytes;
	const bool closed = fclose(f) == 0;
	if (!written || !closed || rename(temporary.c_str(), path.c_str()) != 0)
	{
		unlink(temporary.c_str());
		return false;
	}
	chmod(path.c_str(), 0666);
	return true;
}

std::string lowercase(std::string text)
{
	for (char& c : text)
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	return text;
}

std::string baseName(const std::string& path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string fileTitle(const std::string& path)
{
	const std::string name = baseName(path);
	const size_t dot = name.rfind('.');
	return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string extension(const std::string& path)
{
	const std::string name = baseName(path);
	const size_t dot = name.rfind('.');
	return dot == std::string::npos || dot == 0 ? "" : lowercase(name.substr(dot));
}

std::string trim(const std::string& text)
{
	size_t first = 0, last = text.size();
	while (first < last && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' || text[first] == '\n'))
		first++;
	while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r'
			|| text[last - 1] == '\n'))
		last--;
	return text.substr(first, last - first);
}

double now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

}

using namespace fe;

extern "C" const char *rc_version_string(void);

namespace fe
{
std::string userAgent()
{
#if defined(SWANSTATION_PS5)
	const char *system = "PS5";
#else
	const char *system = "PC";
#endif
	return format("PSSwanStation/%d (%s) rcheevos/%s", BuildNumber, system, rc_version_string());
}
}

// Where the updater asks for the newest release: the repository this title's
// source is in.
constexpr const char *ReleasesUrl = "https://api.github.com/repos/Press5elect/PSSwanStation/releases/latest";

int main(int argc, char **argv)
{
	platform::earlyInit();
	// A home-screen forwarder starts the title with --rom <game> (and
	// --exit-after-game): PS5 Forwarder Format, section 10.
	std::string launchRom;
	bool exitAfterGame = false;
	for (int i = 0; i < argc && argv != nullptr; i++)
	{
		if (argv[i] == nullptr)
			continue;
		const std::string arg = argv[i];
		if ((arg == "--rom" || arg == "-rom") && i + 1 < argc && argv[i + 1] != nullptr)
			launchRom = argv[++i];
		else if (arg.rfind("--rom=", 0) == 0)
			launchRom = arg.substr(6);
		else if (arg == "--exit-after-game")
			exitAfterGame = true;
	}
	if (argc > 1)
		diag::mark("main: started with %d argument(s)%s%s%s", argc - 1, launchRom.empty() ? "" : ", --rom ",
				launchRom.c_str(), exitAfterGame ? ", --exit-after-game" : "");
	// An update that a power cut interrupted is undone before anything reads
	// the title's files.
	if (update::recover(appDir))
		diag::mark("update: an interrupted install was undone");
	{
		update::Setup setup;
		setup.appDir = appDir;
		setup.build = BuildNumber;
		setup.titleId = TitleId;
		setup.latestUrl = ReleasesUrl;
#if defined(SWANSTATION_HOST)
		if (const char *url = getenv("SWANSTATION_UPDATE_URL"))
			setup.latestUrl = url;
		// A test can be an older build than it is, to be offered this one.
		if (const char *build = getenv("SWANSTATION_UPDATE_BUILD"))
			setup.build = atoi(build);
#endif
		setup.httpGet = [](const std::string& url, std::vector<uint8_t>& out, unsigned seconds) {
			return platform::httpGet(url, out, seconds);
		};
		setup.httpDownload = platform::httpDownload;
		update::init(setup);
	}
	diag::mark("main: display");
	if (!display::init())
	{
		diag::notify("%s: the display could not be opened (see %spsswanstation-boot.log)", AppName, shownRoot().c_str());
		platform::quit();
	}
	platform::lateInit();
	diag::mark("main: %d x %d at %.2f Hz on %s", display::width(), display::height(), display::refreshRate(),
			display::deviceName().c_str());

	audio::init();
	sound::init();
	audio::setVolume(options::frontend().volume);

	diag::mark("main: emulator");
	// The emulator's disc reader opens the discs in USB drives too.
	discdrive::installOpener();
	if (!host::init())
	{
		diag::notify("%s: the emulator could not be started", AppName);
		platform::quit();
	}
	cheats::init();
	gamedb::init();
	history::init();
	library::init();
	discdrive::start();
	// network.cfg is read: the files kept on the share are brought up to date.
	netfiles::startUp();
#if defined(SWANSTATION_HOST)
	// The PC test: a shader preset brought in as on the console, then out.
	if (const char *preset = getenv("SWANSTATION_IMPORT_TEST"))
	{
		std::string error;
		const double began = now();
		const bool ok = slangimport::importNow(preset, fileTitle(preset), error);
		printf("import %s: %s in %.2f s\n", preset, ok ? "brought in" : error.c_str(), now() - began);
		fflush(stdout);
		_exit(ok ? 0 : 1);
	}
	if (const char *spec = getenv("SWANSTATION_NET_TEST"))
	{
		const int result = smb::selfTest(spec);
		fflush(stdout);
		_exit(result);
	}
#endif
	covers::init();
	ui::init();
	if (!launchRom.empty())
		ui::startFromArguments(launchRom, exitAfterGame);
	if (options::frontend().updateCheck && platform::httpAvailable())
		update::check();
	diag::mark("main: running");

#ifdef SWANSTATION_HOST
	// Test runs: a game to start at once, and a frame to stop at.
	const char *testGame = getenv("SWANSTATION_GAME");
	const char *testFrames = getenv("SWANSTATION_FRAMES");
	const uint64_t lastFrame = testFrames != nullptr ? strtoull(testFrames, nullptr, 10) : 0;
	if (testGame != nullptr && !host::start(strcmp(testGame, "bios") == 0 ? "" : testGame))
		diag::mark("main: the test game did not start: %s", host::lastError().c_str());
#endif

	// A frame that takes long is a screen that stood still: the log says when
	// and for how long, and the marks before it say what was being done.
	double frameBegan = now();
	// Frames that took longer than a refresh and a half, counted with what
	// each part of them took and whether a button was held, and said in the
	// boot log every ten seconds there were any.
	struct Slow
	{
		int frames = 0, slow = 0, held = 0, slowHeld = 0;
		double worst = 0, part[6] = {}, began = 0;
	} slow;
	slow.began = now();
	while (!ui::quitRequested())
	{
		const double time = now();
		const double took = time - frameBegan;
		if (took > 0.75)
			diag::mark("main: the screen stood still for %.1f s", took);
		frameBegan = time;
		double at[7];
		at[0] = now();
		platform::padPoll();
		at[1] = now();
		if (!display::beginFrame())
		{
			usleep(10000);
			continue;
		}
		at[2] = now();
		const bool playing = !ui::blocksEmulation() && host::running();
		if (!ui::blocksEmulation())
			host::runFrame();
		at[3] = now();
		host::tick();
		at[4] = now();
		ui::frame();
		ui::widgetsSubmit();
		at[5] = now();
		display::endFrame();
		at[6] = now();
#ifdef SWANSTATION_HOST
		// A test: what each refresh's frame of the game took.
		static FILE *times = getenv("SWANSTATION_TIMES") != nullptr ? fopen(getenv("SWANSTATION_TIMES"), "w") : nullptr;
		if (times != nullptr && playing)
			fprintf(times, "%.3f\n", (at[3] - at[2]) * 1e3);
#endif
		if (playing)
		{
			const float rate = display::refreshRate();
			const double limit = rate > 1.f ? 1.5 / rate : 0.025;
			const bool held = platform::pad(0).buttons != 0;
			const double length = at[6] - at[0];
			slow.frames++;
			slow.held += held;
			if (length > limit)
			{
				slow.slow++;
				slow.slowHeld += held;
				if (length > slow.worst)
				{
					slow.worst = length;
					for (int i = 0; i < 6; i++)
						slow.part[i] = at[i + 1] - at[i];
				}
			}
			if (time - slow.began >= 10.0)
			{
				if (slow.slow > 0)
					diag::mark("main: %d of %d frames slow (%d of them with a button held; a button was held in %d frames); "
							"worst %.1f ms: pad %.1f, wait %.1f, game %.1f, tick %.1f, interface %.1f, present %.1f",
							slow.slow, slow.frames, slow.slowHeld, slow.held, slow.worst * 1e3, slow.part[0] * 1e3,
							slow.part[1] * 1e3, slow.part[2] * 1e3, slow.part[3] * 1e3, slow.part[4] * 1e3,
							slow.part[5] * 1e3);
				slow = Slow();
				slow.began = time;
			}
		}
		// The web panel's requests, between frames.
		web::tick();
		// The library has been on the screen for a while: this start worked.
		if (display::frameCount() == 180)
			storage::startCompleted(appDir);
#ifdef SWANSTATION_HOST
		if (lastFrame != 0 && display::frameCount() >= lastFrame)
			break;
#endif
	}

	diag::mark("main: closing");
	web::shutdown();
	host::shutdown();
	discdrive::stop();
	netfiles::finish();
	options::saveFrontend();
	audio::shutdown();
	platform::quit();
}
