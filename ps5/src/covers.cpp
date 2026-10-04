/*
	SwanStation for PS5 - cover downloads.

	Copyright 2026 the PSFlyCast contributors (PSFlyCast, shell/ps5/ps5_covers.cpp)
	SPDX-License-Identifier: GPL-3.0-or-later

	A game with no cover gets one from the libretro thumbnails collection
	(github.com/libretro-thumbnails/Sony_-_PlayStation, the box art RetroArch
	shows), looked up by the game's file name, which is how that collection is
	named (Redump names, "Crash Bandicoot (USA)"). The picture is saved as
	<root>covers/<file name>.png, where the library also looks for the user's
	own covers (.png or .jpg); a name the collection does not have is
	remembered in covers/not-found.txt and not asked for again.

	Two sources hold the same pictures under the same names: the collection on
	GitHub over HTTPS, and libretro's own server over plain HTTP
	(thumbnails.libretro.com), which is tried when the first does not answer.
	One worker thread, one request at a time, through the platform's HTTP
	client (the console's libSceHttp2).
*/
#include "fe.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fe::covers
{
namespace
{
struct Source
{
	const char *name;
	const char *baseUrl;
	bool links;		// a regional duplicate is a text file naming the picture (a git link)
};
constexpr Source Sources[] = {
	{ "GitHub (HTTPS)", "https://raw.githubusercontent.com/libretro-thumbnails/Sony_-_PlayStation/master/Named_Boxarts/", true },
	{ "thumbnails.libretro.com (HTTP)", "http://thumbnails.libretro.com/Sony%20-%20PlayStation/Named_Boxarts/", false },
};
constexpr int SourceCount = 2;

std::mutex mutex;
std::condition_variable wake;
std::deque<std::string> queue;
std::set<std::string> asked;
std::set<std::string> notFound;
bool notFoundLoaded;
bool started;
std::atomic<unsigned> currentGeneration{1};
std::atomic<int> pending{0};
std::atomic<bool> offline{false};

// The collection's file name for a game name: its reserved characters are '_'.
std::string thumbnailName(const std::string& name)
{
	std::string out = name;
	for (char& c : out)
		if (strchr("&*/:`<>?\\|\"", c) != nullptr)
			c = '_';
	return out;
}

std::string urlEncode(const std::string& s)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s)
	{
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += (char)c;
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

bool isImage(const std::vector<uint8_t>& data)
{
	static const uint8_t png[] = { 0x89, 'P', 'N', 'G' };
	return data.size() > 64 && (memcmp(data.data(), png, 4) == 0 || (data[0] == 0xff && data[1] == 0xd8));
}

std::string coversDir()
{
	return rootDir + "covers/";
}

void loadNotFound()
{
	if (notFoundLoaded)
		return;
	notFoundLoaded = true;
	if (FILE *f = fopen((coversDir() + "not-found.txt").c_str(), "r"))
	{
		char line[512];
		while (fgets(line, sizeof(line), f) != nullptr)
		{
			const std::string s = trim(line);
			if (!s.empty())
				notFound.insert(s);
		}
		fclose(f);
	}
}

void rememberNotFound(const std::string& base)
{
	std::lock_guard<std::mutex> lock(mutex);
	notFound.insert(base);
	const std::string file = coversDir() + "not-found.txt";
	if (FILE *f = fopen(file.c_str(), "a"))
	{
		fprintf(f, "%s\n", base.c_str());
		fclose(f);
		chmod(file.c_str(), 0666);
	}
}

// The names to try for a file name: itself, then without a "(Disc N)" tag
// (the collection has the first disc under either), then with each region
// when the name has none.
std::vector<std::string> namesFor(const std::string& base)
{
	std::vector<std::string> names{ base };
	const size_t disc = base.find(" (Disc ");
	if (disc != std::string::npos)
	{
		const size_t end = base.find(')', disc);
		if (end != std::string::npos)
			names.push_back(base.substr(0, disc) + base.substr(end + 1));
	}
	if (base.find('(') == std::string::npos)
		for (const char *region : { " (USA)", " (Europe)", " (Japan)" })
			names.push_back(base + region);
	return names;
}

// 1 saved, 0 not in the collection, -1 could not ask (offline, time-out).
int fetch(const Source& source, const std::string& base)
{
	std::vector<uint8_t> data;
	for (const std::string& name : namesFor(base))
	{
		std::string url = std::string(source.baseUrl) + urlEncode(thumbnailName(name)) + ".png";
		int status = platform::httpGet(url, data, 20);
		// A regional duplicate is a link in the collection: its body is the
		// name of the picture it stands for.
		if (source.links && status == 200 && !isImage(data) && data.size() < 512)
		{
			std::string target = trim(std::string(data.begin(), data.end()));
			if (target.size() > 4 && target.find('/') == std::string::npos)
			{
				url = std::string(source.baseUrl) + urlEncode(target);
				status = platform::httpGet(url, data, 20);
			}
		}
		if (status == 200 && isImage(data))
		{
			const std::string file = coversDir() + base + ".png";
			const std::string temporary = file + ".part";
			if (!writeFile(temporary, data.data(), data.size()) || rename(temporary.c_str(), file.c_str()) != 0)
			{
				unlink(temporary.c_str());
				return -1;
			}
			static bool first = true;
			if (first)
			{
				first = false;
				diag::mark("covers: downloads work, from %s (%s)", source.name, base.c_str());
			}
			return 1;
		}
		if (status != 404 && status != 200)
		{
			if (status > 0)
				diag::mark("covers: the server answered %d", status);
			return -1;
		}
	}
	return 0;
}

void worker()
{
	int failures = 0;
	int source = 0;
	for (;;)
	{
		std::string base;
		{
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait(lock, [] { return !queue.empty(); });
			base = queue.front();
			queue.pop_front();
		}
		int result = offline ? -1 : fetch(Sources[source], base);
		if (result < 0 && !offline && platform::httpAvailable() && source + 1 < SourceCount)
		{
			diag::mark("covers: %s does not answer; trying %s", Sources[source].name, Sources[source + 1].name);
			source++;
			result = fetch(Sources[source], base);
		}
		if (result == 1)
		{
			currentGeneration++;
			failures = 0;
		}
		else if (result == 0)
		{
			rememberNotFound(base);
			failures = 0;
		}
		else if (++failures >= 3 && !offline)
		{
			// No network, or the collection cannot be reached: stop for this run.
			offline = true;
			diag::mark("covers: downloads stopped for this run (no answer from any source)");
		}
		pending--;
	}
}

void request(const std::string& base)
{
	if (!options::frontend().covers || base.empty() || offline)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	loadNotFound();
	if (!asked.insert(base).second || notFound.count(base) != 0)
		return;
	queue.push_back(base);
	pending++;
	if (!started)
	{
		started = true;
		std::thread(worker).detach();
	}
	wake.notify_one();
}

} // namespace

void init()
{
}

std::string find(const library::Game& game)
{
	for (const char *ext : { ".png", ".jpg", ".jpeg" })
	{
		const std::string file = coversDir() + game.fileTitle + ext;
		if (fileExists(file))
			return file;
	}
	request(game.fileTitle);
	return "";
}

unsigned generation()
{
	return currentGeneration.load(std::memory_order_relaxed);
}

std::string status()
{
	const int n = pending.load();
	if (n <= 0 || offline)
		return "";
	return "Downloading covers (" + std::to_string(n) + " left)";
}

}
