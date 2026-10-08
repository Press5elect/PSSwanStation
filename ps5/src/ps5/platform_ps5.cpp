/*
	PSSwanStation - the console: folders, the sandbox, the log, the end.

	SPDX-License-Identifier: GPL-3.0-or-later

	Start-up, in the order PSFlyCast's shell/ps5/ps5_main.cpp found to work:
	  1. The crash report and the first marks.
	  2. The frontend's options, read from the title's folder as the sandbox
	     shows it (/app0), and the start marker (storage.cpp): after a start
	     that did not finish, this one is made as plainly as can be.
	  3. With "USB drives" or "keep my files outside the title folder" on, the
	     title asks to be let out of its sandbox, so that /mnt/usb0-7 and /data
	     can be reached. The asking is ps5/elevation's (BlackBearReloaded's
	     Lapy client, see its README.txt): a resident Lapy service first, else
	     the packaged one-shot helper through the ELF loader on port 9021. It
	     has to happen before any other thread exists.
	  4. The title's folder as this process now sees it, and the root folder
	     the user's files are in: the title's folder, or /data/psswanstation.
	  5. The boot log, the folders (0777, so the console's FTP server can reach
	     them), the modes of what earlier runs left, RADV's shader cache
	     folder, the display mode in param.json, and stdout/stderr in
	     logs/psswanstation.log.
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
#include <time.h>
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

#include "elevation/elevation.hpp"

extern "C"
{
int sceKernelConvertUtcToLocaltime(int64_t utc, int64_t *local, void *zone, uint64_t *summerTime);
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
bool safeStart;
bool outsideUsed;
std::string outsideWhy;
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
				|| name == "lapy.elf" || name == "lapy-manifest.json" || name == ".update" || name == "licenses"
				|| name == "assets"))
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
	const std::string log = root + "logs/psswanstation.log";
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
			for (const char *wanted : { "psx", "ps1", "playstation", "psone", "psswanstation", "swanstation" })
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
	appDir = rootDir = "/app0/";
	options::loadFrontend();
	diag::setNotifications(options::frontend().notifications);
	safeStart = storage::startBegan(appDir);
	if (safeStart)
		diag::mark("main: the last start did not finish: this one stays in the sandbox, at 59.94 Hz");
	const int filesAt = options::frontend().filesAt;
	const bool wantOut = (options::frontend().usb || filesAt == 1 || filesAt == 2 || filesAt == 3) && !safeStart;
	if (wantOut)
	{
		// Before any other thread exists, as the helper's protocol asks.
		const elevation::Status status = elevation::request(elevation::Capability::filesystem, "/app0/lapy.elf");
		elevated = status == elevation::Status::ok;
		diag::mark("sandbox: leaving it %s (status %u, by way of %s)", elevated ? "granted" : "not granted",
				(unsigned)status, elevation::path());
	}
	// The title's folder as this process sees it now. Outside the sandbox
	// /app0 may no longer be a path: the folder is where the launcher mounted
	// it from, or where the console mounts the title it runs.
	std::vector<std::string> candidates;
	if (elevated)
	{
		candidates.push_back(std::string("/data/homebrew/") + TitleId);
		candidates.push_back(std::string("/system_ex/app/") + TitleId);
		candidates.push_back(std::string("/mnt/sandbox/") + TitleId + "_000/app0");
	}
	candidates.push_back("/app0");
	appDir.clear();
	for (const std::string& candidate : candidates)
		if (hasFile(candidate + "/eboot.bin") && writableDir(candidate))
		{
			appDir = candidate + "/";
			break;
		}
	if (appDir.empty())
		appDir = "/app0/";
	rootDir = appDir;
	options::loadFrontend();
	diag::setNotifications(options::frontend().notifications);

	// The user's files: where "Where my files are kept" says, when that can
	// be reached. What is copied there the first time comes from wherever
	// they were kept before (the folder outside, else the title's).
	int migrated = 0;
	const std::string outside = storage::OutsideDir;
	const bool outsideUsable = elevated && writableDir(outside.substr(0, outside.size() - 1));
	const std::string before = outsideUsable && hasFile(outside + ".migrated") ? outside : appDir;
	const std::string sandboxWhy = safeStart ? "the last start did not finish, so this one stayed in the sandbox"
			: "the sandbox could not be left (it needs a resident Lapy service, or the ELF loader on port 9021)";
	if (filesAt == 1)
	{
		if (!elevated)
			outsideWhy = sandboxWhy;
		else if (!outsideUsable)
			outsideWhy = outside + " cannot be written to";
		else
		{
			migrated = storage::migrate(appDir, outside);
			rootDir = outside;
			outsideUsed = true;
		}
	}
	else if (filesAt == 2)
	{
		// The first USB drive that has the folder, else the first one that can be written to.
		std::string drive;
		for (int pass = 0; pass < 2 && drive.empty() && elevated; pass++)
			for (int i = 0; i < 8 && drive.empty(); i++)
			{
				const std::string mount = "/mnt/usb" + std::to_string(i);
				if (pass == 0 ? writableDir(mount + "/" + storage::UsbFolder) : writableDir(mount))
					drive = mount + "/" + storage::UsbFolder + "/";
			}
		if (!elevated)
			outsideWhy = sandboxWhy;
		else if (drive.empty())
			outsideWhy = "no USB drive that can be written to is plugged in";
		else
		{
			migrated = storage::migrate(before, drive);
			rootDir = drive;
			outsideUsed = true;
		}
	}
	else if (filesAt == 3)
	{
		// The console's copy of what the share keeps: outside where it can be.
		if (outsideUsable)
		{
			migrated = storage::migrate(appDir, outside);
			rootDir = outside;
		}
		outsideUsed = true;
	}
	diag::open(rootDir);
	diag::mark("%s, build %d; title folder: %s; files: %s", AppName, BuildNumber, appDir.c_str(), rootDir.c_str());
	if (migrated > 0)
		diag::mark("storage: first start with the files outside: %d copied", migrated);
	if (!outsideWhy.empty())
		diag::mark("storage: the files are kept in %s, which this start cannot use: %s", storage::OutsideDir,
				outsideWhy.c_str());
	storage::makeFolders(rootDir);
	if (rootDir != appDir)
		makeDir(appDir + "games");
	repairModes(rootDir, 0);
	// RADV's shader cache, with the user's files (it is theirs to delete).
	setenv("MESA_SHADER_CACHE_DIR", (rootDir + "radv-shader-cache").c_str(), 1);

	// The display mode: what param.json declares is what the console grants
	// and what the driver asks for, so the setting is kept there. A start
	// after one that failed shows the driver a file that declares nothing.
	const std::string param = appDir + "sce_sys/param.json";
	const int declared = storage::displayModeIn(param);
	if (storage::syncDisplayMode(param, options::frontend().displayMode))
		diag::mark("display: mode %d was declared, %d is set", declared, options::frontend().displayMode);
	std::string driverParam = param;
	if (safeStart)
	{
		driverParam = rootDir + "data/safe-param.json";
		const char plain[] = "{ \"attribute3\": 0 }\n";
		writeFile(driverParam, plain, sizeof(plain) - 1);
	}
	// The driver looks in /app0, which a process outside the sandbox has not.
	setenv("PS5_VIDEOOUT_PARAM_JSON", driverParam.c_str(), 1);

	if (options::frontend().usb)
	{
		if (elevated)
			findUsbDirs();
		else if (!safeStart)
			diag::notify("%s: USB drives need a resident Lapy service or the ELF loader (port 9021). Continuing "
					"without them.", AppName);
	}
	// Whether the console gives the title a folder of its own for downloads,
	// noted for later builds.
	diag::mark("storage: /download0 %s", writableDir("/download0") ? "can be written to" : "cannot be written to");
	redirectLogs(rootDir);
	diag::mark("the emulator's log: %slogs/psswanstation.log", rootDir.c_str());
}

void lateInit()
{
	diag::mark("hide splash screen: %d", sceSystemServiceHideSplashScreen());
	padOpen();
}

[[noreturn]] void quit()
{
	diag::mark("quit");
	storage::startCompleted(appDir);
	for (int i = 0; i < MaxPads; i++)
		padLight(i, 0, 0, 0);
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
	// Asked once a minute: summer time begins and ends while a title runs.
	static int offset;
	static double askedAt = -1000;
	if (now() - askedAt < 60)
		return offset;
	askedAt = now();
	const int64_t utc = (int64_t)time(nullptr);
	int64_t local = utc;
	// Room for more than the two answers are documented to take.
	uint64_t zone[8] = {}, summer[2] = {};
	if (sceKernelConvertUtcToLocaltime(utc, &local, zone, summer) == 0 && local - utc > -86400 && local - utc < 86400)
		offset = (int)(local - utc);
	return offset;
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

std::string fe::shownApp()
{
	if (appDir == "/app0/")
		return std::string("/data/homebrew/") + TitleId + "/";
	return appDir;
}

// _start (ps5/runtime/ps5_crt.cpp) calls this when main returns: the kernel's
// exit() ends a title with SIGSYS, so the shell is asked to close it.
extern "C" void catchReturnFromMain(int status)
{
	fe::diag::mark("main returned (status %d)", status);
	fe::platform::quit();
}
