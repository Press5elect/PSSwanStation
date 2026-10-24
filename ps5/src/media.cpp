/*
	PSSwanStation - the pictures and videos ES-DE themes show, wherever they
	are kept: on the console, on a USB drive, on a network share (SMB, NFS or
	FTP), beside the games.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	ES-DE keeps a system's media in folders of their kind (covers,
	screenshots, titlescreens, marquees, miximages, 3dboxes, backcovers,
	physicalmedia, fanart, videos), each file named after the game's file.
	Such folders are looked for in these places, the first that has a file
	winning:

	  <root>media, <root>media/psx, <root>downloaded_media/psx
	  PSSwanStation/media (and media/psx, downloaded_media/psx) on each USB drive
	  in each games folder (the console's, a USB drive's, every network
	  folder network.cfg names): media, media/psx, downloaded_media/psx,
	  and downloaded_media/psx beside it (ES-DE's own layout, ROMs and
	  ES-DE side by side)

	Asking a share for every file of every game would take seconds a screen,
	so the folders are listed once, on a thread of their own, into a table of
	names; a lookup is then a search in memory. The table is made again when
	the library's lists change, every few minutes, and when asked; a file
	fetched for a game is added to it at once.
*/
#include "fe.h"
#include "tuning.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <dirent.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>

namespace fe::media
{
namespace
{

constexpr const char *Folders[] = { "covers", "screenshots", "titlescreens", "marquees", "miximages", "3dboxes",
	"backcovers", "physicalmedia", "fanart", "videos" };

struct Table
{
	// folder -> game file title, lower case -> file
	std::map<std::string, std::map<std::string, std::string>> files;
	std::vector<std::string> roots;		// the places that had any
	size_t count = 0;
};

std::mutex mutex;
std::condition_variable wake;
Table table;
std::atomic<unsigned> currentGeneration{1};
bool started = false, wanted = true, busy = false;
double madeAt = -1e9;
unsigned librarySeen = ~0u;
std::string statusText = "Not looked for yet";
std::atomic<bool> listedOnce{false};

std::string lower(std::string s)
{
	for (char& c : s)
		c = (char)std::tolower((unsigned char)c);
	return s;
}

std::string withoutExtension(const std::string& name)
{
	const size_t dot = name.rfind('.');
	return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

// A game's file title without its "(Disc 1)": ES-DE names a game on several
// discs after its playlist, which often lacks the tag.
std::string withoutDisc(const std::string& title)
{
	const size_t disc = title.find(" (Disc ");
	if (disc == std::string::npos)
		return title;
	const size_t end = title.find(')', disc);
	return end == std::string::npos ? title : title.substr(0, disc) + title.substr(end + 1);
}

bool wantedFile(const std::string& folder, const std::string& name)
{
	const size_t dot = name.rfind('.');
	if (dot == std::string::npos)
		return false;
	const std::string ext = lower(name.substr(dot));
	if (folder == "videos")
		return ext == ".mp4" || ext == ".m4v";
	return ext == ".png" || ext == ".jpg" || ext == ".jpeg";
}

// The endings Batocera and EmulationStation give a game's pictures in one
// images folder, and the ES-DE folder each stands for. "-image" is the
// main picture: a mix image, or else a screenshot.
const std::vector<std::pair<std::string, const char *>>& imageEndings()
{
	static const std::vector<std::pair<std::string, const char *>> endings = {
		{ "-thumb", "covers" }, { "-boxart", "covers" }, { "-box", "covers" }, { "-marquee", "marquees" },
		{ "-wheel", "marquees" }, { "-image", "miximages" }, { "-image", "screenshots" }, { "-mix", "miximages" },
		{ "-screenshot", "screenshots" }, { "-titleshot", "titlescreens" }, { "-fanart", "fanart" },
		{ "-boxback", "backcovers" }, { "-cartridge", "physicalmedia" }, { "-disc", "physicalmedia" },
		{ "-box3d", "3dboxes" },
	};
	return endings;
}

// A file named with such an ending in its kind's own folder
// (videos/<game>-video.mp4): the game's name without it.
std::string stripEnding(const std::string& stem)
{
	for (const char *ending : { "-video", "-thumb", "-marquee", "-image", "-boxart", "-titleshot", "-fanart" })
	{
		const size_t n = strlen(ending);
		if (stem.size() > n && stem.compare(stem.size() - n, n, ending) == 0)
			return stem.substr(0, stem.size() - n);
	}
	return stem;
}

// The files of one folder (local or on a share), by name.
std::vector<std::pair<std::string, std::string>> listFolder(const std::string& dir)
{
	std::vector<std::pair<std::string, std::string>> out;
	if (smb::isNetworkPath(dir))
	{
		for (const smb::Entry& e : smb::list(dir))
			out.emplace_back(e.name, e.path);
		return out;
	}
	if (DIR *d = opendir(dir.c_str()))
	{
		while (const dirent *entry = readdir(d))
		{
			if (entry->d_name[0] == '.')
				continue;
			out.emplace_back(entry->d_name, dir + "/" + entry->d_name);
		}
		closedir(d);
	}
	return out;
}

std::string parentOf(std::string dir)
{
	while (dir.size() > 1 && dir.back() == '/')
		dir.pop_back();
	const size_t slash = dir.rfind('/');
	// Not above a share's own root (smb://server/share).
	if (slash == std::string::npos || (smb::isNetworkPath(dir) && std::count(dir.begin(), dir.end(), '/') <= 3))
		return "";
	return dir.substr(0, slash);
}

std::vector<std::string> places()
{
	std::vector<std::string> roots;
	const auto add = [&roots](std::string dir) {
		while (dir.size() > 1 && dir.back() == '/')
			dir.pop_back();
		if (!dir.empty() && std::find(roots.begin(), roots.end(), dir) == roots.end())
			roots.push_back(dir);
	};
	const auto withLayouts = [&add](const std::string& base) {
		add(base + "/media");
		add(base + "/media/psx");
		add(base + "/downloaded_media/psx");
	};
	std::string root = rootDir;
	while (root.size() > 1 && root.back() == '/')
		root.pop_back();
	withLayouts(root);
	for (const std::string& drive : platform::usbDrives())
		withLayouts(drive + "/" + storage::UsbFolder);
	std::vector<std::string> gameFolders = { root + "/games" };
	for (const std::string& dir : platform::usbGameDirs())
		gameFolders.push_back(dir);
	for (const std::string& dir : smb::gameFolders())
		gameFolders.push_back(dir);
	for (const std::string& dir : gameFolders)
	{
		// The kinds' folders straight in the games folder (roms/psx/videos,
		// roms/psx/covers), as ES-DE's "media in the ROM folder" and
		// Batocera's roms/psx/images and videos have them.
		add(dir);
		withLayouts(dir);
		const std::string parent = parentOf(dir);
		if (!parent.empty())
			add(parent + "/downloaded_media/psx");
	}
	return roots;
}

void make()
{
	const double started = now();
	Table fresh;
	for (const std::string& base : places())
	{
		bool any = false;
		// The place's own folders first: a share is not asked for what is not there.
		std::set<std::string> present;
		for (const auto& [name, path] : listFolder(base))
			present.insert(lower(name));
		if (present.empty())
			continue;
		// Batocera's and EmulationStation's images folder: one folder, the
		// kind in the name's ending (<game>-thumb.png, <game>-marquee.png).
		if (present.count("images") != 0)
			for (const auto& [name, path] : listFolder(base + "/images"))
			{
				if (!wantedFile("images", name))
					continue;
				const std::string stem = lower(withoutExtension(name));
				for (const auto& [ending, folder] : imageEndings())
					if (stem.size() > ending.size() && stem.compare(stem.size() - ending.size(), ending.size(), ending) == 0)
					{
						if (fresh.files[folder].emplace(stem.substr(0, stem.size() - ending.size()), path).second)
							fresh.count++;
						any = true;
					}
			}
		for (const char *folder : Folders)
		{
			if (present.count(folder) == 0)
				continue;
			std::map<std::string, std::string>& names = fresh.files[folder];
			for (const auto& [name, path] : listFolder(base + "/" + folder))
			{
				if (!wantedFile(folder, name))
					continue;
				if (names.emplace(stripEnding(lower(withoutExtension(name))), path).second)
					fresh.count++;
				any = true;
			}
		}
		if (any)
			fresh.roots.push_back(base);
	}
	std::string text = fresh.count == 0 ? std::string("None found")
			: format("%zu files in %zu place(s)", fresh.count, fresh.roots.size());
	diag::mark("media: %s, looked through in %.1f s", text.c_str(), now() - started);
	for (const std::string& r : fresh.roots)
		diag::mark("media: in %s", smb::isNetworkPath(r) ? r.substr(0, r.find('/', 6)).c_str() : r.c_str());
	std::lock_guard<std::mutex> lock(mutex);
	table = std::move(fresh);
	statusText = text;
	currentGeneration++;
	listedOnce = true;
}

void worker()
{
	for (;;)
	{
		{
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait(lock, [] { return wanted; });
			wanted = false;
			busy = true;
		}
		make();
		std::lock_guard<std::mutex> lock(mutex);
		busy = false;
	}
}

unsigned libraryGenerations()
{
	unsigned sum = 0;
	for (int i = 0; i < library::SourceCount; i++)
		sum = sum * 31 + library::generation(i);
	return sum;
}

} // namespace

void rescan()
{
	std::lock_guard<std::mutex> lock(mutex);
	wanted = true;
	if (!started)
	{
		started = true;
		std::thread(tuning::background(worker)).detach();
	}
	wake.notify_one();
}

std::string find(const library::Game& game, const std::string& folder)
{
	// Looked through again when the library changes, and every five minutes.
	const unsigned libraries = libraryGenerations();
	if (libraries != librarySeen || now() - madeAt > 300)
	{
		librarySeen = libraries;
		madeAt = now();
		rescan();
	}
	std::lock_guard<std::mutex> lock(mutex);
	const auto f = table.files.find(folder);
	if (f == table.files.end())
		return "";
	const std::string key = lower(game.fileTitle);
	auto it = f->second.find(key);
	if (it == f->second.end())
		it = f->second.find(withoutDisc(key));
	if (it == f->second.end() && !game.discs.empty())
		it = f->second.find(lower(withoutExtension(game.discs[0].substr(game.discs[0].rfind('/') + 1))));
	return it == f->second.end() ? "" : it->second;
}

void add(const std::string& folder, const std::string& fileTitle, const std::string& path)
{
	std::lock_guard<std::mutex> lock(mutex);
	table.files[folder][lower(fileTitle)] = path;
	currentGeneration++;
}

bool ready()
{
	return listedOnce.load();
}

unsigned generation()
{
	return currentGeneration.load();
}

std::string status()
{
	std::lock_guard<std::mutex> lock(mutex);
	return busy ? std::string("Looking through the media folders\xe2\x80\xa6") : statusText;
}

} // namespace fe::media
