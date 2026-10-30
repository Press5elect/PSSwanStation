/*
	PSSwanStation - cover downloads.

	Copyright 2026 the PSFlyCast contributors (PSFlyCast, shell/ps5/ps5_covers.cpp)
	SPDX-FileCopyrightText: 2026 Press5elect
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

	An ES-DE theme also shows title screens and moments of play: the same
	collection has those (Named_Titles, Named_Snaps), fetched the same way
	into media/titlescreens and media/screenshots, the folders ES-DE keeps
	them in, when a theme asks for them and downloads are on.
*/
#include "fe.h"
#include "tuning.h"

#include <atomic>
#include <cerrno>
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

// What the worker fetches: a picture of a kind (Kind) for a file name.
struct Job
{
	std::string base;
	int kind = 0;
};

std::mutex mutex;
std::condition_variable wake;
std::deque<Job> queue;
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

const char *const collectionFolders[KindCount] = { "Named_Boxarts", "Named_Titles", "Named_Snaps" };
const char *const mediaFolders[KindCount] = { "", "titlescreens", "screenshots" };

// Where a kind of picture is kept: covers/, or the ES-DE media folders.
std::string folderFor(int kind)
{
	return kind == BoxArt ? coversDir() : rootDir + "media/" + mediaFolders[kind] + "/";
}

// The name not-found.txt remembers a game by, for each kind.
std::string notFoundKey(const std::string& base, int kind)
{
	return kind == BoxArt ? base : std::string(collectionFolders[kind]) + "/" + base;
}

// The source's address for a kind of picture.
std::string baseUrlFor(const Source& source, int kind)
{
	std::string root = source.baseUrl;
	const size_t folder = root.find("Named_Boxarts");
	if (folder != std::string::npos)
		root.replace(folder, strlen("Named_Boxarts"), collectionFolders[kind]);
	return root;
}

enum FetchResult
{
	FetchSaveFailed = -2,
	FetchUnavailable = -1,
	FetchNotFound = 0,
	FetchSaved = 1,
};

// Save failures are separate from an unavailable server: they must not trigger
// a fallback request or mark the downloader offline.
FetchResult fetch(const Source& source, const std::string& base, int kind)
{
	std::vector<uint8_t> data;
	const std::string root = baseUrlFor(source, kind);
	for (const std::string& name : namesFor(base))
	{
		std::string url = root + urlEncode(thumbnailName(name)) + ".png";
		int status = platform::httpGet(url, data, 20);
		// A regional duplicate is a link in the collection: its body is the
		// name of the picture it stands for.
		if (source.links && status == 200 && !isImage(data) && data.size() < 512)
		{
			std::string target = trim(std::string(data.begin(), data.end()));
			if (target.size() > 4 && target.find('/') == std::string::npos)
			{
				url = root + urlEncode(target);
				status = platform::httpGet(url, data, 20);
			}
		}
		if (status == 200 && isImage(data))
		{
			const std::string folder = folderFor(kind);
			if (kind != BoxArt)
				makeDir(folder);
			const std::string file = folder + base + ".png";
			const std::string temporary = file + ".part";
			if (!writeFile(temporary, data.data(), data.size()))
			{
				diag::mark("covers: could not write %s", file.c_str());
				unlink(temporary.c_str());
				return FetchSaveFailed;
			}
			errno = 0;
			if (rename(temporary.c_str(), file.c_str()) != 0)
			{
				const int error = errno;
				diag::mark("covers: could not finish saving %s (%s)", file.c_str(),
						error == 0 ? "rename failed" : strerror(error));
				unlink(temporary.c_str());
				return FetchSaveFailed;
			}
			static bool first = true;
			if (first)
			{
				first = false;
				diag::mark("covers: downloads work, from %s (%s, %s)", source.name, base.c_str(), collectionFolders[kind]);
			}
			return FetchSaved;
		}
		if (status != 404 && status != 200)
		{
			if (status > 0)
				diag::mark("covers: the server answered %d", status);
			return FetchUnavailable;
		}
	}
	return FetchNotFound;
}

void worker()
{
	int failures = 0;
	int source = 0;
	for (;;)
	{
		Job job;
		{
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait(lock, [] { return !queue.empty(); });
			job = queue.front();
			queue.pop_front();
		}
		int result = offline ? -1 : fetch(Sources[source], job.base, job.kind);
		if (result == FetchUnavailable && !offline && platform::httpAvailable() && source + 1 < SourceCount)
		{
			diag::mark("covers: %s does not answer; trying %s", Sources[source].name, Sources[source + 1].name);
			source++;
			result = fetch(Sources[source], job.base, job.kind);
		}
		if (result == FetchSaved)
		{
			currentGeneration++;
			failures = 0;
		}
		else if (result == FetchNotFound)
		{
			rememberNotFound(notFoundKey(job.base, job.kind));
			failures = 0;
		}
		else if (result == FetchSaveFailed)
			failures = 0;
		else if (++failures >= 3 && !offline)
		{
			// No network, or the collection cannot be reached: stop for this run.
			offline = true;
			diag::mark("covers: downloads stopped for this run (no answer from any source)");
		}
		pending--;
	}
}

void request(const std::string& base, int kind = BoxArt)
{
	if (!options::frontend().covers || base.empty() || offline)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	loadNotFound();
	const std::string key = notFoundKey(base, kind);
	if (!asked.insert(key).second || notFound.count(key) != 0)
		return;
	// A cover before the pictures themes add: the library shows covers first.
	if (kind == BoxArt)
		queue.push_front({ base, kind });
	else
		queue.push_back({ base, kind });
	pending++;
	if (!started)
	{
		started = true;
		std::thread(tuning::background(worker)).detach();
	}
	wake.notify_one();
}

} // namespace

void init()
{
}

namespace
{
std::atomic<int> chooseNow{ChooseIdle};

const char *const *const kindFolders = collectionFolders;

// The collection's picture of one kind for a game, as bytes. 1 found, 0 the
// collection has none, -1 it could not be asked.
int fetchKind(const std::string& base, int kind, std::vector<uint8_t>& data)
{
	bool answered = false;
	for (int source = 0; source < SourceCount; source++)
	{
		std::string root = Sources[source].baseUrl;
		const size_t folder = root.find("Named_Boxarts");
		if (folder == std::string::npos)
			continue;
		root.replace(folder, strlen("Named_Boxarts"), kindFolders[kind]);
		for (const std::string& name : namesFor(base))
		{
			std::string url = root + urlEncode(thumbnailName(name)) + ".png";
			int status = platform::httpGet(url, data, 20);
			if (Sources[source].links && status == 200 && !isImage(data) && data.size() < 512)
			{
				const std::string target = trim(std::string(data.begin(), data.end()));
				if (target.size() > 4 && target.find('/') == std::string::npos)
					status = platform::httpGet(root + urlEncode(target), data, 20);
			}
			if (status == 200 && isImage(data))
				return 1;
			if (status == 404 || status == 200)
				answered = true;
			else
				break;		// this source does not answer: the next one
		}
		if (answered)
			return 0;
	}
	return -1;
}

void removeCoverFiles(const std::string& base)
{
	for (const char *ext : { ".png", ".jpg", ".jpeg" })
		unlink((coversDir() + base + ext).c_str());
}
}

void choose(const library::Game& game, int kind)
{
	if (kind < 0 || kind >= KindCount || chooseNow.exchange(ChooseWorking) == ChooseWorking)
		return;
	const std::string base = game.fileTitle;
	std::thread([base, kind] {
		tuning::setThread(tuning::Role::Background);
		std::vector<uint8_t> data;
		const int result = fetchKind(base, kind, data);
		if (result == 1)
		{
			removeCoverFiles(base);
			if (writeFile(coversDir() + base + ".png", data.data(), data.size()))
			{
				currentGeneration++;
				chooseNow = ChooseDone;
			}
			else
				chooseNow = ChooseFailed;
		}
		else
			chooseNow = result == 0 ? ChooseNotFound : ChooseFailed;
		diag::mark("covers: a %s picture was asked for: %s", kindFolders[kind],
				chooseNow == ChooseDone ? "saved" : chooseNow == ChooseNotFound ? "the collection has none" : "no answer");
	}).detach();
}

ChooseState chooseState()
{
	return (ChooseState)chooseNow.load();
}

void remove(const library::Game& game)
{
	removeCoverFiles(game.fileTitle);
	{
		// The automatic one may be asked for again.
		std::lock_guard<std::mutex> lock(mutex);
		asked.erase(game.fileTitle);
	}
	currentGeneration++;
}

std::string findMedia(const library::Game& game, int kind)
{
	if (kind == BoxArt)
		return find(game);
	if (kind < 0 || kind >= KindCount)
		return "";
	const std::string folder = folderFor(kind);
	for (const char *ext : { ".png", ".jpg", ".jpeg" })
	{
		const std::string file = folder + game.fileTitle + ext;
		if (fileExists(file))
			return file;
	}
	request(game.fileTitle, kind);
	return "";
}

std::string find(const library::Game& game)
{
	for (const char *ext : { ".png", ".jpg", ".jpeg" })
	{
		const std::string file = coversDir() + game.fileTitle + ext;
		if (fileExists(file))
			return file;
	}
	// A box in an ES-DE covers folder (the console's, a USB drive's, a share's).
	const std::string kept = media::find(game, "covers");
	if (!kept.empty())
		return kept;
	request(game.fileTitle);
	return "";
}

void changed()
{
	currentGeneration++;
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
