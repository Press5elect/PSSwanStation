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
	When neither has the game's box art, xlenore's psx-covers collection is
	asked by the disc's serial (raw.githubusercontent.com/xlenore/psx-covers/
	main/covers/default/<serial>.jpg), the serial the game ran under before,
	the one the game database gives its file name, or, for a game on the
	console or a USB drive under a name of its own, the one read from its
	disc image; the picture is saved as <file name>.jpg. A name the libretro
	collection does not have does not keep a game from psx-covers: a serial
	psx-covers has no cover for is remembered by itself ("serial:<serial>"
	in not-found.txt).
	One worker thread, one request at a time, through the platform's HTTP
	client (the console's libSceHttp2).

	An ES-DE theme also shows title screens and moments of play: the same
	collection has those (Named_Titles, Named_Snaps), fetched the same way
	into media/titlescreens and media/screenshots, the folders ES-DE keeps
	them in, when a theme asks for them and downloads are on.

	Not only the games in sight: when nothing is asked for, the worker goes
	through every game of the library in its order (the console's games, the
	USB drives', the share's) and fetches the covers they lack, one after
	another, as OGBox does with a folder's games (OGBox by Press5elect).
	What the library shows meanwhile goes first. Not while a game runs.
*/
#include "fe.h"
#include "folders.h"
#include "tuning.h"

#include <atomic>
#include <chrono>
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
#include <vector>

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
	std::string serial;		// for the fallback by serial (box art only), or empty
	bool serialOnly = false;	// the libretro collection is known not to have it
};

// The fallback for box art: xlenore's psx-covers, by the disc's serial.
constexpr const char *SerialCoverUrl = "https://raw.githubusercontent.com/xlenore/psx-covers/main/covers/default/";

// The serial a game's cover can be asked for by: the one it ran under before,
// else the one the game database gives its file name (Redump's names), else,
// with `readDisc`, the one in the disc image itself (a game on the console or
// a USB drive; a share's is not read, so that it is not woken for a cover).
std::string serialFor(const std::string& path, const std::string& fileTitle, bool readDisc = false)
{
	std::string serial = path.empty() ? std::string() : host::knownSerial(path);
	if (serial.empty())
		serial = gamedb::serialByName(fileTitle);
	if (serial.empty() && readDisc && !path.empty() && path.compare(0, 6, "smb://") != 0
			&& path.compare(0, 6, "nfs://") != 0 && path.compare(0, 6, "ftp://") != 0
			&& lowercase(path).find(".m3u") == std::string::npos)
		serial = host::readSerial(path);
	return serial;
}

std::string serialKey(const std::string& serial)
{
	return "serial:" + serial;
}

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
// The library's games, gone through for the covers they lack (worker only),
// and how far, for status().
std::vector<library::Game> walk;
std::atomic<size_t> walkAt{0}, walkSize{0};
unsigned walkMade;	// the library's generations it was made from

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
	return folders::folder(folders::Covers);
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

// The fallback: the box art by the serial, saved as <file name>.jpg.
FetchResult fetchBySerial(const std::string& serial, const std::string& base)
{
	if (serial.empty())
		return FetchNotFound;
	std::vector<uint8_t> data;
	const int status = platform::httpGet(SerialCoverUrl + serial + ".jpg", data, 20);
	if (status == 200 && isImage(data))
	{
		const std::string file = coversDir() + base + ".jpg";
		const std::string temporary = file + ".part";
		if (!writeFile(temporary, data.data(), data.size()) || rename(temporary.c_str(), file.c_str()) != 0)
		{
			diag::mark("covers: could not write %s", file.c_str());
			unlink(temporary.c_str());
			return FetchSaveFailed;
		}
		static bool first = true;
		if (first)
		{
			first = false;
			diag::mark("covers: a cover by its serial from psx-covers (%s, %s)", base.c_str(), serial.c_str());
		}
		return FetchSaved;
	}
	if (status != 404 && status != 200)
	{
		static bool said;
		if (!said)
		{
			said = true;
			diag::mark("covers: psx-covers did not answer for %s (%d); asked again at the next start", serial.c_str(), status);
		}
		return FetchUnavailable;
	}
	return FetchNotFound;
}

bool haveCover(const library::Game& game)
{
	for (const char *ext : { ".png", ".jpg", ".jpeg" })
		if (fileExists(coversDir() + game.fileTitle + ext))
			return true;
	return !media::find(game, "covers").empty();
}

void request(const std::string& base, int kind, const std::string& serial = "");

// What a walk through the library found, said once at its end.
struct WalkTally
{
	int withCover = 0, without = 0, withSerial = 0;
	bool said = false;
} walkTally;

// Nothing is asked for: the library's next game without a cover is. The
// list is made again when the library changes.
void walkOn()
{
	unsigned made = 0;
	for (int source : { library::Internal, library::Usb, library::Network })
		made = made * 31 + library::generation(source);
	if (made != walkMade)
	{
		walkMade = made;
		walk.clear();
		std::set<std::string> seen;
		for (int source : { library::Internal, library::Usb, library::Network })
			for (library::Game& game : library::games(source))
				if (!game.fileTitle.empty() && seen.insert(game.fileTitle).second)
					walk.push_back(std::move(game));
		walkAt = 0;
		walkSize = walk.size();
		walkTally = WalkTally();
	}
	// ES-DE's folders are listed first: a cover there is not fetched again.
	if (!options::frontend().covers || offline || host::running() || !media::ready())
	{
		static bool offSaid;
		if (!offSaid && !options::frontend().covers)
		{
			offSaid = true;
			diag::mark("covers: Download Covers is disabled: none is fetched");
		}
		return;
	}
	while (walkAt < walk.size())
	{
		const library::Game& game = walk[walkAt];
		walkAt++;
		if (haveCover(game))
		{
			walkTally.withCover++;
			continue;
		}
		const int before = pending.load();
		// The serial it ran under, the game database's, or its disc's (no game
		// runs while the walk goes on).
		const std::string serial = serialFor(game.discs.empty() ? game.path : game.discs.front(), game.fileTitle, true);
		walkTally.without++;
		walkTally.withSerial += !serial.empty();
		request(game.fileTitle, BoxArt, serial);
		if (pending.load() != before)
			return;	// one at a time: what the library asks for meanwhile goes first
	}	if (!walkTally.said && !walk.empty())
	{
		walkTally.said = true;
		diag::mark("covers: the library looked through: %d game(s) with a cover, %d without (%d of them with a serial to "
				"ask psx-covers by)", walkTally.withCover, walkTally.without, walkTally.withSerial);
	}

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
			while (queue.empty())
			{
				lock.unlock();
				walkOn();
				lock.lock();
				if (queue.empty())
					wake.wait_for(lock, std::chrono::seconds(2));
			}
			job = queue.front();
			queue.pop_front();
		}
		if (job.serialOnly)
		{
			// The libretro collection is known not to have it: psx-covers alone.
			const FetchResult bySerial = offline ? FetchUnavailable : fetchBySerial(job.serial, job.base);
			if (bySerial == FetchSaved)
				currentGeneration++;
			else if (bySerial == FetchNotFound)
				rememberNotFound(serialKey(job.serial));
			pending--;
			continue;
		}
		int result = offline ? -1 : fetch(Sources[source], job.base, job.kind);
		if (result == FetchUnavailable && !offline && platform::httpAvailable() && source + 1 < SourceCount)
		{
			diag::mark("covers: %s does not answer; trying %s", Sources[source].name, Sources[source + 1].name);
			source++;
			result = fetch(Sources[source], job.base, job.kind);
		}
		// Neither libretro source has the box art: psx-covers, by the serial.
		// Its not answering does not stop the downloads.
		if (result == FetchNotFound && job.kind == BoxArt && !job.serial.empty())
		{
			const FetchResult bySerial = fetchBySerial(job.serial, job.base);
			if (bySerial == FetchSaved || bySerial == FetchSaveFailed)
				result = bySerial;
			else if (bySerial == FetchNotFound)
				rememberNotFound(serialKey(job.serial));
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

void request(const std::string& base, int kind, const std::string& serial)
{
	if (!options::frontend().covers || base.empty() || offline)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	loadNotFound();
	const std::string key = notFoundKey(base, kind);
	// A name the libretro collection does not have may still have a cover by
	// its serial, which psx-covers is asked for once.
	const bool byNameMissed = notFound.count(key) != 0;
	const bool serialOnly = byNameMissed && kind == BoxArt && !serial.empty() && notFound.count(serialKey(serial)) == 0;
	if (byNameMissed && !serialOnly)
		return;
	if (!asked.insert(key).second)
		return;
	// A cover before the pictures themes add: the library shows covers first.
	if (kind == BoxArt)
		queue.push_front({ base, kind, serial, serialOnly });
	else
		queue.push_back({ base, kind, std::string(), false });
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
	// The worker starts at once: the library's covers are fetched without
	// the library being looked at first.
	if (!options::frontend().covers)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	if (!started)
	{
		started = true;
		std::thread(tuning::background(worker)).detach();
	}
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
	const std::string serial = kind == BoxArt ? serialFor(game.path, game.fileTitle) : std::string();
	std::thread([base, kind, serial] {
		tuning::setThread(tuning::Role::Background);
		std::vector<uint8_t> data;
		int result = fetchKind(base, kind, data);
		// Not in the libretro collection: psx-covers, by the serial (a JPEG).
		if (result != 1 && !serial.empty())
		{
			std::vector<uint8_t> jpeg;
			const int status = platform::httpGet(SerialCoverUrl + serial + ".jpg", jpeg, 20);
			if (status == 200 && isImage(jpeg))
			{
				removeCoverFiles(base);
				const bool saved = writeFile(coversDir() + base + ".jpg", jpeg.data(), jpeg.size());
				if (saved)
					currentGeneration++;
				chooseNow = saved ? ChooseDone : ChooseFailed;
				diag::mark("covers: a box art was asked for: %s (psx-covers, %s)", saved ? "saved" : "not saved",
						serial.c_str());
				return;
			}
			if (status == 404 && result == -1)
				result = 0;
		}
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
	request(game.fileTitle, BoxArt, serialFor(game.path, game.fileTitle));
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
	const size_t at = walkAt.load(), size = walkSize.load();
	if (size > 0 && at < size)
		return "Downloading covers (game " + std::to_string(at) + " of " + std::to_string(size) + ")";
	return "Downloading covers (" + std::to_string(n) + " left)";
}

}
