/*
	SwanStation for PS5 - the console: folders, the sandbox, the log, the end.

	SPDX-License-Identifier: GPL-3.0-or-later

	Start-up, in the order PSFlyCast's shell/ps5/ps5_main.cpp found to work:
	  1. The crash report and the first marks.
	  2. The frontend's options, read from the title's folder as the sandbox
	     shows it (/app0). With "USB drives" on, the title asks elfldr to run
	     its helper (src/ps5/elevation, from ps5-native-app-boilerplate), which
	     lets this process out of its sandbox so /mnt/usb0-7 can be read. This
	     has to happen before any other thread exists.
	  3. The root folder: /app0 in the sandbox; outside it, the title's real
	     folder. Everything the title reads or writes lives under it.
	  4. The boot log, the folders (0777, so the console's FTP server can reach
	     them), the modes of what earlier runs left, RADV's shader cache
	     folder, and stdout/stderr in logs/swanstation.log.
	A title must not exit(): the shell is asked to close it.
*/
#include "fe.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <ps5platform/exec.h>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C"
{
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
int sceKernelAvailableDirectMemorySize(int64_t searchStart, int64_t searchEnd, size_t alignment, int64_t *physAddrOut,
		size_t *sizeOut);
}

namespace ps5
{
unsigned elevateFilesystem(const char *helperPath);	// elevation/ps5_elevate.cpp
}

namespace fe::diag
{
void setNotifications(bool show);
}

namespace fe::platform
{
void padOpen();

namespace
{
bool elevated;
std::vector<std::string> usbDirs;

bool hasFile(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

bool writableDir(const std::string& dir)
{
	mkdir(dir.c_str(), 0777);
	const std::string probe = dir + "/.write-test";
	FILE *f = fopen(probe.c_str(), "w");
	if (f == nullptr)
		return false;
	fclose(f);
	unlink(probe.c_str());
	return true;
}

// Everything the title keeps in its folder stays reachable over FTP, which
// is another process: folders 0777 and files 0666, whatever the umask gave
// them. Only what differs is changed. The games are the user's own files and
// are left alone, as are the title's.
void repairModes(const std::string& dir, int depth)
{
	DIR *list = opendir(dir.c_str());
	if (list == nullptr)
		return;
	std::vector<std::string> names;
	while (const dirent *entry = readdir(list))
	{
		const std::string name = entry->d_name;
		if (name != "." && name != "..")
			names.push_back(name);
	}
	closedir(list);
	for (const std::string& name : names)
	{
		if (depth == 0 && (name == "games" || name == "sce_sys" || name == "sce_module" || name == "eboot.bin"
				|| name == "sandbox-elevator.elf" || name == "licenses" || name == "assets"))
			continue;
		const std::string path = dir + name;
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			if ((st.st_mode & 0777) != 0777)
				chmod(path.c_str(), 0777);
			if (depth < 4)
				repairModes(path + "/", depth + 1);
		}
		else if (S_ISREG(st.st_mode) && (st.st_mode & 0666) != 0666)
			chmod(path.c_str(), (st.st_mode & 0777) | 0666);
	}
}

// One file, each stream opened on it in append mode, each with a 64 KiB
// buffer a thread of its own writes out every second: a write to the
// console's storage costs about 0.7 ms a call, and a line-buffered stream
// paid it on whichever thread logged (PSFlyCast's finding).
void redirectLogs(const std::string& root)
{
	const std::string log = root + "logs/swanstation.log";
	const std::string prev = root + "logs/swanstation.prev.log";
	rename(log.c_str(), prev.c_str());
	if (freopen(log.c_str(), "a", stdout) == nullptr)
		return;
	chmod(log.c_str(), 0666);
	static char outBuffer[64 * 1024];
	static char errBuffer[64 * 1024];
	setvbuf(stdout, outBuffer, _IOFBF, sizeof(outBuffer));
	fflush(stderr);
	if (freopen(log.c_str(), "a", stderr) != nullptr)
		setvbuf(stderr, errBuffer, _IOFBF, sizeof(errBuffer));
	fprintf(stderr, "[stderr] the emulator's log lines and the graphics driver's messages follow in this file\n");
	std::thread([] {
		for (;;)
		{
			sleep(1);
			fflush(stdout);
			fflush(stderr);
		}
	}).detach();
}

// The game folders on USB drives: a folder named psx, ps1, playstation,
// psone or swanstation (any case) at the top of /mnt/usb0-7. Only those are
// scanned, not whole drives.
void findUsbDirs()
{
	usbDirs.clear();
	for (int i = 0; i < 8; i++)
	{
		const std::string drive = "/mnt/usb" + std::to_string(i);
		DIR *dir = opendir(drive.c_str());
		if (dir == nullptr)
			continue;
		int entries = 0;
		while (const dirent *entry = readdir(dir))
		{
			if (entry->d_name[0] == '.')
				continue;
			entries++;
			const std::string lower = lowercase(entry->d_name);
			for (const char *wanted : { "psx", "ps1", "playstation", "psone", "swanstation" })
				if (lower == wanted)
					usbDirs.push_back(drive + "/" + entry->d_name);
		}
		closedir(dir);
		diag::mark("usb: %s has %d entries", drive.c_str(), entries);
	}
	diag::mark("usb: %d game folder(s)", (int)usbDirs.size());
}

} // namespace

void earlyInit()
{
	diag::installCrashHandler();
	diag::mark("main: %s started (build %d)", AppName, BuildNumber);

	// The options decide whether the sandbox is left; they are read from the
	// title's folder, which the sandbox shows as /app0.
	rootDir = "/app0/";
	options::loadFrontend();
	diag::setNotifications(options::frontend().notifications);
	if (options::frontend().usb)
	{
		// Before any other thread exists, as the helper's protocol asks.
		const unsigned status = ps5::elevateFilesystem("/app0/sandbox-elevator.elf");
		elevated = status == 0;
		diag::mark("usb: sandbox elevation %s (status %u)", elevated ? "granted" : "not granted", status);
	}
	std::vector<std::string> candidates;
	if (elevated)
	{
		// Outside the sandbox /app0 is no longer a path: the title's folder is
		// where the launcher mounted it from, or under the sandbox's mount.
		candidates.push_back(std::string("/data/homebrew/") + TitleId);
		candidates.push_back(std::string("/mnt/sandbox/") + TitleId + "_000/app0");
	}
	candidates.push_back("/app0");
	candidates.push_back("/download0");
	rootDir.clear();
	for (const std::string& candidate : candidates)
		if ((candidate == "/download0" || hasFile(candidate + "/eboot.bin")) && writableDir(candidate))
		{
			rootDir = candidate + "/";
			break;
		}
	if (rootDir.empty())
		rootDir = "/app0/";
	options::loadFrontend();
	diag::setNotifications(options::frontend().notifications);
	diag::open(rootDir);
	diag::mark("%s, build %d; root folder: %s", AppName, BuildNumber, rootDir.c_str());
	for (const char *sub : { "", "bios", "games", "covers", "cheats", "data", "logs", "data/saves", "data/states",
			"data/game-options", "data/cheats", "data/cache" })
		makeDir(rootDir + sub);
	repairModes(rootDir, 0);
	// RADV's shader cache, in the root whichever path that is this run.
	setenv("MESA_SHADER_CACHE_DIR", (rootDir + "radv-shader-cache").c_str(), 1);
	if (options::frontend().usb)
	{
		if (elevated)
			findUsbDirs();
		else
			diag::notify("%s: USB drives need elfldr running (port 9021). Continuing without them.", AppName);
	}
	redirectLogs(rootDir);
	diag::mark("the emulator's log: %slogs/swanstation.log", rootDir.c_str());
}

void lateInit()
{
	diag::mark("hide splash screen: %d", sceSystemServiceHideSplashScreen());
	padOpen();
}

[[noreturn]] void quit()
{
	diag::mark("quit");
	fflush(nullptr);
	const int result = sceSystemServiceLoadExec("exit", nullptr);
	diag::mark("close request: %d", result);
	for (;;)
		usleep(100000);
}

bool usbAvailable()
{
	return elevated;
}

std::vector<std::string> usbGameDirs()
{
	if (elevated)
		findUsbDirs();
	return usbDirs;
}

uint64_t freeMemory()
{
	int64_t start = 0;
	size_t size = 0;
	if (sceKernelAvailableDirectMemorySize(0, (int64_t)1 << 40, 0, &start, &size) == 0)
		return size;
	return 0;
}

bool jitAvailable()
{
	// Asked once, with a small region near the title's code, as the
	// recompiler's own request is.
	static int known = -1;
	if (known < 0)
	{
		void *probe = ps5_exec_allocate(64 * 1024, reinterpret_cast<uintptr_t>(&ps5_exec_allocate));
		known = probe != nullptr ? 1 : 0;
		if (probe != nullptr)
			ps5_exec_release(probe);
		diag::mark("jit: executable memory %s", known ? "available" : "refused: the recompiler is not offered");
	}
	return known == 1;
}

}

std::string fe::shownRoot()
{
	if (rootDir == "/app0/")
		return std::string("/data/homebrew/") + TitleId + "/";
	return rootDir;
}

// _start (ps5/runtime/ps5_crt.cpp) calls this when main returns: the kernel's
// exit() ends a title with SIGSYS, so the shell is asked to close it.
extern "C" void catchReturnFromMain(int status)
{
	fe::diag::mark("main returned (status %d)", status);
	fe::platform::quit();
}
