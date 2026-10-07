/*
	PSSwanStation - where the files are: the user's folders, the move outside
	the title's folder, the start marker, the display mode in param.json.

	SPDX-License-Identifier: GPL-3.0-or-later

	The title's folder holds the program and, unless told otherwise, everything
	the user has: games, BIOS, covers, cheats, memory cards, states, settings.
	Replacing that folder with a new build then replaces those too. With "keep
	my files outside the title folder" on they live in /data/psswanstation
	instead, which the console's sandbox hides from a title: it can only be
	used on a start that left the sandbox (ps5/platform_ps5.cpp). The first
	such start copies what the title's folder holds over there; the copies in
	the title's folder stay, untouched from then on, as what a start without
	the way out falls back to.

	The start marker is a file written when a start begins and removed once
	the library has been on the screen for a while. Finding it at a start
	means the last one did not get that far, and this one is made without
	leaving the sandbox and at 59.94 Hz, whatever the settings say.

	The display mode a title gets is what its sce_sys/param.json declares
	(attribute3), read by the console and by the graphics driver when the
	title starts. The setting is kept there: changing it rewrites that one
	number, and the next start has the mode.
*/
#include "fe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fe::storage
{
namespace
{

// What a user keeps, of the title folder's own folders and files.
const char *const userFolders[] = { "bios", "covers", "cheats", "data", "textures", "music", "borders", "layouts",
		"memcards" };
const char *const userFiles[] = { "network.cfg" };

bool copyFile(const std::string& from, const std::string& to)
{
	FILE *in = fopen(from.c_str(), "rb");
	if (in == nullptr)
		return false;
	const std::string temporary = to + ".tmp";
	FILE *out = fopen(temporary.c_str(), "wb");
	if (out == nullptr)
	{
		fclose(in);
		return false;
	}
	static char block[1 << 20];
	bool ok = true;
	size_t got;
	while ((got = fread(block, 1, sizeof(block), in)) > 0)
		if (fwrite(block, 1, got, out) != got)
		{
			ok = false;
			break;
		}
	ok = ferror(in) == 0 && ok;
	fclose(in);
	ok = fclose(out) == 0 && ok;
	if (!ok || rename(temporary.c_str(), to.c_str()) != 0)
	{
		unlink(temporary.c_str());
		return false;
	}
	chmod(to.c_str(), 0666);
	return true;
}

// Copies what `to` does not have yet. Counts the files copied and the ones
// that could not be.
void copyTree(const std::string& from, const std::string& to, int depth, int& copied, int& failed)
{
	DIR *dir = opendir(from.c_str());
	if (dir == nullptr)
		return;
	makeDir(to);
	std::vector<std::string> names;
	while (const dirent *entry = readdir(dir))
	{
		const std::string name = entry->d_name;
		if (name != "." && name != ".." && name.size() < 240)
			names.push_back(name);
	}
	closedir(dir);
	for (const std::string& name : names)
	{
		const std::string source = from + "/" + name, target = to + "/" + name;
		struct stat st;
		if (stat(source.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			if (depth < 6)
				copyTree(source, target, depth + 1, copied, failed);
		}
		else if (S_ISREG(st.st_mode) && !fileExists(target))
		{
			// A file left half written by something else is not a user's file,
			// and neither is the mark of a start under way.
			if ((name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) || name == ".starting")
				continue;
			(copyFile(source, target) ? copied : failed)++;
		}
	}
}

std::string markerPath(const std::string& app)
{
	return app + "data/.starting";
}

// Where the number after "attribute3" is in the file's text: its first
// character and the one after its last. False when the key is not there.
bool findAttribute(const std::string& text, size_t& first, size_t& end)
{
	size_t at = text.find("\"attribute3\"");
	if (at == std::string::npos)
		return false;
	at = text.find(':', at);
	if (at == std::string::npos)
		return false;
	at++;
	while (at < text.size() && (text[at] == ' ' || text[at] == '\t'))
		at++;
	first = at;
	while (at < text.size() && ((text[at] >= '0' && text[at] <= '9') || text[at] == 'x' || text[at] == 'X'
			|| (text[at] >= 'a' && text[at] <= 'f') || (text[at] >= 'A' && text[at] <= 'F')))
		at++;
	end = at;
	return end > first;
}

}

// attribute3 for each display mode: nothing; the 120 Hz bit as the titles
// proven at 119.88 Hz declare it (0x80040); the 120 Hz bit with the one
// titles that take a variable refresh rate declare (0x40040), which the
// graphics driver's notes name and no title on this foundation has run yet.
constexpr unsigned long attributes[3] = { 0, 0x80040, 0x40040 };

void makeFolders(const std::string& root)
{
	for (const char *sub : { "", "bios", "games", "covers", "cheats", "data", "logs", "data/saves", "data/states",
			"data/game-options", "data/cheats", "data/cache", "textures", "music", "borders", "layouts", "memcards",
			"memcards/import", "memcards/export" })
		makeDir(root + sub);
}

int migrate(const std::string& from, const std::string& to)
{
	const std::string done = to + ".migrated";
	if (fileExists(done) || from == to)
		return 0;
	makeDir(to);
	int copied = 0, failed = 0;
	for (const char *folder : userFolders)
		copyTree(from + folder, to + folder, 0, copied, failed);
	for (const char *file : userFiles)
		if (fileExists(from + file) && !fileExists(to + file))
			(copyFile(from + file, to + file) ? copied : failed)++;
	diag::mark("storage: %d file(s) copied from %s to %s, %d could not be", copied, from.c_str(), to.c_str(), failed);
	// Only a copy that was whole is not made again.
	if (failed == 0)
	{
		const std::string note = "The files of " + from + " were copied here.\n";
		writeFile(done, note.data(), note.size());
	}
	return copied;
}

int displayModeIn(const std::string& paramJson)
{
	std::vector<uint8_t> bytes;
	if (!readFile(paramJson, bytes))
		return -1;
	const std::string text(bytes.begin(), bytes.end());
	size_t first = 0, end = 0;
	if (!findAttribute(text, first, end))
		return -1;
	const unsigned long value = strtoul(text.substr(first, end - first).c_str(), nullptr, 0);
	for (int mode = 0; mode < 3; mode++)
		if (value == attributes[mode])
			return mode;
	return -1;
}

bool syncDisplayMode(const std::string& paramJson, int displayMode)
{
	if (displayMode < 0 || displayMode > 2)
		return false;
	std::vector<uint8_t> bytes;
	if (!readFile(paramJson, bytes))
		return false;
	std::string text(bytes.begin(), bytes.end());
	size_t first = 0, end = 0;
	if (!findAttribute(text, first, end))
		return false;
	if (strtoul(text.substr(first, end - first).c_str(), nullptr, 0) == attributes[displayMode])
		return false;
	text.replace(first, end - first, std::to_string(attributes[displayMode]));
	if (!writeFile(paramJson, text.data(), text.size()))
	{
		diag::mark("storage: %s could not be written", paramJson.c_str());
		return false;
	}
	// As the console wants a title's files: readable and runnable by all.
	chmod(paramJson.c_str(), 0777);
	diag::mark("storage: the display mode in %s is now %d (attribute3 %#lx), from the next start", paramJson.c_str(),
			displayMode, attributes[displayMode]);
	return true;
}

bool startBegan(const std::string& app)
{
	const std::string marker = markerPath(app);
	const bool unfinished = fileExists(marker);
	makeDir(app + "data");
	const char note[] = "A start of PSSwanStation is under way. Left behind, it means that start did not finish.\n";
	FILE *f = fopen(marker.c_str(), "w");
	if (f != nullptr)
	{
		fwrite(note, 1, sizeof(note) - 1, f);
		fclose(f);
		chmod(marker.c_str(), 0666);
	}
	return unfinished;
}

void startCompleted(const std::string& app)
{
	unlink(markerPath(app).c_str());
}

}
