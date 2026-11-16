/*
	PSSwanStation - where your files are (folders.h).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "folders.h"
#include "fe.h"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <mutex>
#include <unistd.h>

namespace fe::folders
{
namespace
{
struct KindInfo
{
	const char *key, *name, *standard;
	bool written, restart;
};
const KindInfo kinds[KindCount] = {
	{ "bios", "BIOS", "bios/", false, true },
	{ "memory_cards", "Memory Cards", "data/saves/", true, true },
	{ "save_states", "Save States", "data/states/", true, false },
	{ "cheats", "Cheats", "cheats/", true, false },
	{ "covers", "Covers", "covers/", true, false },
	{ "textures", "Texture Packs", "textures/", false, true },
};

std::mutex mutex;
bool loaded;
std::string chosenPath[KindCount];
std::vector<std::string> games;

std::string file()
{
	return appDir + "folders.cfg";
}

std::string withSlash(std::string path)
{
	if (!path.empty() && path.back() != '/')
		path += '/';
	return path;
}

std::string trim(const std::string& s)
{
	const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void readLocked()
{
	if (loaded)
		return;
	loaded = true;
	for (std::string& p : chosenPath)
		p.clear();
	games.clear();
	std::vector<uint8_t> data;
	if (!readFile(file(), data))
		return;
	std::string text(data.begin(), data.end());
	size_t at = 0;
	while (at < text.size())
	{
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = text.substr(at, end - at);
		at = end + 1;
		const size_t eq = line.find('=');
		if (line.empty() || line[0] == '#' || eq == std::string::npos)
			continue;
		const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
		if (value.empty() || value[0] != '/')
			continue;
		if (key == "game_folder")
		{
			if (std::find(games.begin(), games.end(), withSlash(value)) == games.end())
				games.push_back(withSlash(value));
			continue;
		}
		for (int k = 0; k < KindCount; k++)
			if (key == kinds[k].key)
				chosenPath[k] = withSlash(value);
	}
}

void writeLocked()
{
	std::string text = "# PSSwanStation - where your files are (Settings, Library, Folders). A kind with no line is in\n"
			"# its usual place. game_folder: a folder of games, scanned with the folders inside it.\n";
	for (int k = 0; k < KindCount; k++)
		if (!chosenPath[k].empty())
			text += std::string(kinds[k].key) + " = " + chosenPath[k] + "\n";
	for (const std::string& g : games)
		text += "game_folder = " + g + "\n";
	writeFile(file(), text.data(), text.size());
}

// A folder can be written to: a file made in it and taken away again.
bool writable(const std::string& dir, std::string& why)
{
	makeDir(dir);
	const std::string probe = withSlash(dir) + ".psswanstation-probe";
	FILE *f = std::fopen(probe.c_str(), "wb");
	if (f == nullptr)
	{
		why = dir + " cannot be written to.";
		return false;
	}
	const bool ok = std::fputc('p', f) != EOF;
	std::fclose(f);
	unlink(probe.c_str());
	if (!ok)
		why = dir + " cannot be written to (the drive may be full).";
	return ok;
}
}

const char *name(Kind kind)
{
	return kinds[kind].name;
}

std::string standard(Kind kind)
{
	return rootDir + kinds[kind].standard;
}

std::string chosen(Kind kind)
{
	std::lock_guard<std::mutex> lock(mutex);
	readLocked();
	return chosenPath[kind];
}

std::string folder(Kind kind)
{
	const std::string own = chosen(kind);
	return own.empty() ? standard(kind) : own;
}

bool needsRestart(Kind kind)
{
	return kinds[kind].restart;
}

bool choose(Kind kind, const std::string& path, std::string& why)
{
	if (!path.empty() && !dirExists(path))
	{
		why = path + " is not there.";
		return false;
	}
	if (!path.empty() && kinds[kind].written && !writable(path, why))
		return false;
	std::lock_guard<std::mutex> lock(mutex);
	readLocked();
	chosenPath[kind] = withSlash(path);
	writeLocked();
	diag::mark("folders: %s %s", kinds[kind].name, path.empty() ? "in its usual place" : ("in " + path).c_str());
	return true;
}

std::vector<std::string> gameFolders()
{
	std::lock_guard<std::mutex> lock(mutex);
	readLocked();
	return games;
}

bool addGameFolder(const std::string& path, std::string& why)
{
	if (!dirExists(path))
	{
		why = path + " is not there.";
		return false;
	}
	std::lock_guard<std::mutex> lock(mutex);
	readLocked();
	const std::string p = withSlash(path);
	if (std::find(games.begin(), games.end(), p) == games.end())
		games.push_back(p);
	writeLocked();
	diag::mark("folders: game folder added: %s", p.c_str());
	return true;
}

void removeGameFolder(const std::string& path)
{
	std::lock_guard<std::mutex> lock(mutex);
	readLocked();
	games.erase(std::remove(games.begin(), games.end(), withSlash(path)), games.end());
	writeLocked();
	diag::mark("folders: game folder taken off: %s", path.c_str());
}

std::vector<std::pair<std::string, std::string>> places()
{
	std::vector<std::pair<std::string, std::string>> list;
	list.emplace_back("The title's folder", appDir);
	if (rootDir != appDir)
		list.emplace_back("The folder for your files", rootDir);
	if (dirExists("/data"))
		list.emplace_back("The console's storage (/data)", "/data/");
	for (const std::string& drive : platform::usbDrives())
		list.emplace_back("USB drive (" + drive + ")", withSlash(drive));
	return list;
}

std::vector<std::string> subFolders(const std::string& path)
{
	std::vector<std::string> names;
	DIR *dir = opendir(path.c_str());
	if (dir == nullptr)
		return names;
	while (const dirent *entry = readdir(dir))
	{
		const std::string n = entry->d_name;
		if (n.empty() || n[0] == '.')
			continue;
		if (!dirExists(withSlash(path) + n))
			continue;
		names.push_back(n);
		if (names.size() >= 1000)
			break;
	}
	closedir(dir);
	std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
		return lowercase(a) < lowercase(b);
	});
	return names;
}

void load()
{
	std::lock_guard<std::mutex> lock(mutex);
	loaded = false;
	readLocked();
}
}
