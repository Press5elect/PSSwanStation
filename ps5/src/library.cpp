/*
	PSSwanStation - the game library: what is in the folders.

	SPDX-License-Identifier: GPL-3.0-or-later

	Three sources, as PSFlyCast has them: the title's own games/ folder, USB
	drives (when the sandbox was left) and the folders of an SMB share named in
	network.cfg. Each is scanned on a thread of its own.

	All three are looked through when the title starts; the interface shows a
	tab only for a source that has games.

	The network list is kept (<root>data/network-games.txt): a start shows the
	kept list without touching the share, so a NAS whose disks sleep is not
	woken by browsing. The share is asked when there is no kept list (or the
	list was made from other folders than network.cfg names now), when the
	user asks for a scan, and when a game is started. A network folder is an
	SMB share's or an FTP server's (smb.cpp, ftp.cpp); both are in the one list.

	Files that are parts of a game are folded into it: the tracks a cue sheet
	names, and the discs of one game named as dumps are ("Game (USA) (Disc
	1).chd", "... (Disc 2).chd"), for which a playlist is written in
	data/cache/playlists/. A playlist is what lets the discs of a game share
	one memory card. Playlists found in the folders (.m3u) are not shown: their
	discs are there themselves and are grouped the same way.
*/
#include "fe.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>

namespace fe::library
{
namespace
{

struct SourceState
{
	std::mutex mutex;
	std::vector<Game> list;
	unsigned generation = 1;
	std::atomic<bool> busy{false};
	std::string status;
	bool scanned = false;
	std::thread thread;
};
SourceState sources[SourceCount];

struct Found
{
	std::string path;
	uint64_t size;
};

bool isGameExtension(const std::string& ext)
{
	// Playlists (.m3u) are left out: the discs they list are in the folder
	// themselves, and are put together here by their "(Disc N)" names.
	static const char *known[] = { ".chd", ".cue", ".pbp", ".iso", ".img", ".bin", ".ecm", ".mds", ".exe",
			".psexe", ".psf", ".minipsf" };
	for (const char *e : known)
		if (ext == e)
			return true;
	return false;
}

void walkLocal(const std::string& dir, int depth, std::vector<Found>& out)
{
	DIR *list = opendir(dir.c_str());
	if (list == nullptr)
		return;
	std::vector<std::string> names;
	while (const dirent *entry = readdir(list))
	{
		const std::string name = entry->d_name;
		if (!name.empty() && name[0] != '.')
			names.push_back(name);
	}
	closedir(list);
	for (const std::string& name : names)
	{
		const std::string path = dir + "/" + name;
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			// A games folder's "textures" holds texture packs, thousands of
			// pictures and no game.
			if (depth < 4 && !(depth == 0 && lowercase(name) == "textures"))
				walkLocal(path, depth + 1, out);
		}
		else if (isGameExtension(extension(name)))
			out.push_back({ path, (uint64_t)st.st_size });
	}
}

void walkShare(const std::string& dir, int depth, std::vector<Found>& out)
{
	for (const smb::Entry& entry : smb::list(dir))
	{
		if (entry.name.empty() || entry.name[0] == '.')
			continue;
		if (entry.directory)
		{
			if (depth < 4)
				walkShare(entry.path, depth + 1, out);
		}
		else if (isGameExtension(extension(entry.name)))
			out.push_back({ entry.path, entry.size });
	}
}

std::string readSmall(const std::string& path)
{
	std::vector<uint8_t> data;
	if (smb::isNetworkPath(path) || !readFile(path, data) || data.size() > (1u << 20))
		return "";
	return std::string(data.begin(), data.end());
}

std::string folderOf(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

// "Game (USA) (Disc 2)" -> "Game (USA)" and 2; 0 when the name has no disc.
int discNumber(const std::string& title, std::string& base)
{
	const std::string lower = lowercase(title);
	for (const char *word : { "(disc ", "(disk ", "(cd ", "[disc ", "[disk ", "[cd " })
	{
		const size_t at = lower.find(word);
		if (at == std::string::npos)
			continue;
		size_t i = at + strlen(word);
		int number = 0;
		if (i < lower.size() && isdigit((unsigned char)lower[i]))
		{
			while (i < lower.size() && isdigit((unsigned char)lower[i]))
				number = number * 10 + (lower[i++] - '0');
		}
		else if (i < lower.size() && lower[i] >= 'a' && lower[i] <= 'h'
				&& (i + 1 >= lower.size() || !isalpha((unsigned char)lower[i + 1])))
			number = lower[i++] - 'a' + 1;
		if (number <= 0)
			continue;
		const size_t end = lower.find_first_of(")]", i);
		if (end == std::string::npos)
			continue;
		base = trim(title.substr(0, at)) + title.substr(end + 1);
		base = trim(base);
		return number;
	}
	return 0;
}

// "Crash Bandicoot (USA) (Rev 1) [SCUS-94900]" -> "Crash Bandicoot", "USA".
void cleanName(const std::string& title, std::string& name, std::string& region)
{
	name = title;
	const size_t tag = title.find_first_of("([");
	if (tag != std::string::npos && tag > 0)
		name = trim(title.substr(0, tag));
	if (name.empty())
		name = title;
	const std::string lower = lowercase(title);
	static const struct
	{
		const char *text, *region;
	} regions[] = {
		{ "(usa", "USA" }, { "(europe", "Europe" }, { "(japan", "Japan" }, { "(world", "World" },
		{ "(uk)", "Europe" }, { "(germany", "Europe" }, { "(france", "Europe" }, { "(spain", "Europe" },
		{ "(italy", "Europe" }, { "(australia", "Europe" }, { "(asia", "Asia" }, { "(korea", "Asia" },
	};
	for (const auto& r : regions)
		if (lower.find(r.text) != std::string::npos)
		{
			region = r.region;
			break;
		}
}

std::string safeName(std::string name)
{
	for (char& c : name)
		if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|')
			c = '_';
	return name;
}

std::vector<Game> build(std::vector<Found> found, int source)
{
	// A .bin (or .img) beside a cue sheet of the same folder belongs to the
	// cue; on a share the cue is not read for this, the names decide.
	std::set<std::string> cueFolders;
	std::set<std::string> cueTitles;
	for (const Found& file : found)
		if (extension(file.path) == ".cue")
		{
			cueFolders.insert(folderOf(file.path));
			cueTitles.insert(lowercase(folderOf(file.path) + fileTitle(file.path)));
		}

	struct Disc
	{
		int number;
		Found file;
	};
	std::map<std::string, std::vector<Disc>> groups;	// by folder + base name
	std::vector<Game> games;
	for (const Found& file : found)
	{
		const std::string ext = extension(file.path);
		// A kept list of an earlier build may still name playlists.
		if (!isGameExtension(ext))
			continue;
		const std::string title = fileTitle(file.path);
		if (ext == ".bin" || ext == ".img")
		{
			const std::string lower = lowercase(title);
			if (cueTitles.count(lowercase(folderOf(file.path) + title)) != 0)
				continue;
			if (cueFolders.count(folderOf(file.path)) != 0 && lower.find("(track") != std::string::npos)
				continue;
		}
		std::string base;
		const int disc = discNumber(title, base);
		if (disc > 0)
		{
			groups[folderOf(file.path) + base].push_back({ disc, file });
			continue;
		}
		Game game;
		game.path = file.path;
		game.fileTitle = title;
		cleanName(title, game.name, game.region);
		game.size = file.size;
		game.source = source;
		games.push_back(std::move(game));
	}
	for (auto& [key, discs] : groups)
	{
		std::sort(discs.begin(), discs.end(), [](const Disc& a, const Disc& b) { return a.number < b.number; });
		Game game;
		const std::string base = baseName(key);
		game.fileTitle = fileTitle(discs[0].file.path);
		cleanName(base, game.name, game.region);
		game.source = source;
		for (const Disc& disc : discs)
		{
			game.discs.push_back(disc.file.path);
			game.size += disc.file.size;
		}
		if (discs.size() == 1)
			game.path = discs[0].file.path;
		else
		{
			// The playlist the emulator starts: the discs by their full
			// paths. Its name is the game's, which names the memory card.
			makeDir(rootDir + "data/cache/playlists");
			game.path = rootDir + "data/cache/playlists/" + safeName(base) + ".m3u";
			std::string text;
			for (const Disc& disc : discs)
				text += disc.file.path + "\n";
			std::vector<uint8_t> existing;
			if (!readFile(game.path, existing) || std::string(existing.begin(), existing.end()) != text)
				writeFile(game.path, text.data(), text.size());
		}
		games.push_back(std::move(game));
	}
	std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
		const std::string x = lowercase(a.name), y = lowercase(b.name);
		return x != y ? x < y : a.fileTitle < b.fileTitle;
	});
	return games;
}

std::string networkListFile()
{
	return rootDir + "data/network-games.txt";
}

// The folders the kept list was made from: its first line. A list made from
// other folders (network.cfg was changed since) is not used.
std::string networkListHeader()
{
	std::string header = "# folders";
	for (const std::string& folder : smb::gameFolders())
		header += "\t" + folder;
	return header;
}

bool loadNetworkList(std::vector<Found>& found)
{
	FILE *f = fopen(networkListFile().c_str(), "r");
	if (f == nullptr)
		return false;
	char line[4096];
	if (fgets(line, sizeof(line), f) == nullptr || trim(line) != trim(networkListHeader()))
	{
		fclose(f);
		return false;
	}
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		char *tab = strchr(line, '\t');
		if (tab == nullptr)
			continue;
		*tab = 0;
		const std::string path = trim(tab + 1);
		if (!path.empty())
			found.push_back({ path, strtoull(line, nullptr, 10) });
	}
	fclose(f);
	return true;
}

void saveNetworkList(const std::vector<Found>& found)
{
	FILE *f = fopen(networkListFile().c_str(), "w");
	if (f == nullptr)
		return;
	fprintf(f, "%s\n", networkListHeader().c_str());
	for (const Found& file : found)
		fprintf(f, "%llu\t%s\n", (unsigned long long)file.size, file.path.c_str());
	fclose(f);
	chmod(networkListFile().c_str(), 0666);
}

void run(int source, bool force)
{
	SourceState& state = sources[source];
	std::vector<Found> found;
	std::string status;
	if (source == Internal)
	{
		walkLocal(rootDir + "games", 0, found);
		// With the user's files outside the title's folder, games may be in either.
		if (appDir != rootDir)
			walkLocal(appDir + "games", 0, found);
	}
	else if (source == Usb)
	{
		for (const std::string& dir : platform::usbGameDirs())
			walkLocal(dir, 0, found);
	}
	else
	{
		const std::vector<std::string>& folders = smb::gameFolders();
		if (folders.empty())
			status = "No network folder is set: edit network.cfg in " + shownRoot();
		else if (force || !loadNetworkList(found))
		{
			found.clear();
			smb::retryNow();
			smb::clearError();
			const unsigned failuresBefore = smb::failures();
			for (const std::string& folder : folders)
				walkShare(folder, 0, found);
			if (smb::failures() != failuresBefore)
			{
				// The share did not answer to the end: the list is not kept,
				// and the scan is made again when the user asks for one.
				status = smb::lastError();
				if (found.empty())
				{
					std::lock_guard<std::mutex> lock(state.mutex);
					state.status = status;
					state.scanned = false;
					state.busy = false;
					return;
				}
			}
			else
			{
				if (!smb::lastError().empty())
					status = smb::lastError();
				saveNetworkList(found);
			}
		}
	}
	std::vector<Game> games = build(std::move(found), source);
	diag::mark("library: %s: %d game(s)", sourceName(source).c_str(), (int)games.size());
	std::lock_guard<std::mutex> lock(state.mutex);
	state.list = std::move(games);
	state.generation++;
	state.status = status;
	state.scanned = true;
	state.busy = false;
}

} // namespace

namespace
{
// Favourites and hidden games: "F<tab>path" and "H<tab>path" lines.
std::mutex marksMutex;
std::set<std::string> favourites, hiddenGames;
bool marksLoaded;
std::atomic<unsigned> marksChanges{1};

void loadMarks()
{
	if (marksLoaded)
		return;
	marksLoaded = true;
	FILE *f = fopen((rootDir + "data/marks.txt").c_str(), "r");
	if (f == nullptr)
		return;
	char line[4096];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		const std::string text = trim(line);
		if (text.size() > 2 && text[1] == '\t')
			(text[0] == 'F' ? favourites : hiddenGames).insert(text.substr(2));
	}
	fclose(f);
}

void saveMarks()
{
	std::string text;
	for (const std::string& path : favourites)
		text += "F\t" + path + "\n";
	for (const std::string& path : hiddenGames)
		text += "H\t" + path + "\n";
	writeFile(rootDir + "data/marks.txt", text.data(), text.size());
	marksChanges++;
}
}

bool favourite(const std::string& gamePath)
{
	std::lock_guard<std::mutex> lock(marksMutex);
	loadMarks();
	return favourites.count(gamePath) != 0;
}

bool hidden(const std::string& gamePath)
{
	std::lock_guard<std::mutex> lock(marksMutex);
	loadMarks();
	return hiddenGames.count(gamePath) != 0;
}

void setFavourite(const std::string& gamePath, bool on)
{
	std::lock_guard<std::mutex> lock(marksMutex);
	loadMarks();
	if (on ? favourites.insert(gamePath).second : favourites.erase(gamePath) != 0)
		saveMarks();
}

void setHidden(const std::string& gamePath, bool on)
{
	std::lock_guard<std::mutex> lock(marksMutex);
	loadMarks();
	if (on ? hiddenGames.insert(gamePath).second : hiddenGames.erase(gamePath) != 0)
		saveMarks();
}

unsigned marksGeneration()
{
	return marksChanges;
}

void init()
{
	smb::loadConfig();
	scan(Internal, false);
	if (platform::usbAvailable())
		scan(Usb, false);
	if (!smb::gameFolders().empty())
		scan(Network, false);
}

void scan(int source, bool force)
{
	if (source < 0 || source >= SourceCount)
		return;
	SourceState& state = sources[source];
	if (state.busy.exchange(true))
		return;
	if (state.thread.joinable())
		state.thread.join();
	{
		std::lock_guard<std::mutex> lock(state.mutex);
		state.status.clear();
	}
	state.thread = std::thread(run, source, force);
}

bool scanning(int source)
{
	return sources[source].busy;
}

bool scanned(int source)
{
	std::lock_guard<std::mutex> lock(sources[source].mutex);
	return sources[source].scanned;
}

std::string scanStatus(int source)
{
	std::lock_guard<std::mutex> lock(sources[source].mutex);
	return sources[source].status;
}

std::vector<Game> games(int source)
{
	std::lock_guard<std::mutex> lock(sources[source].mutex);
	return sources[source].list;
}

unsigned generation(int source)
{
	std::lock_guard<std::mutex> lock(sources[source].mutex);
	return sources[source].generation;
}

std::string sourceName(int source)
{
	return source == Internal ? "Internal" : source == Usb ? "USB" : "Network";
}

bool sourceAvailable(int source)
{
	if (source == Usb)
		return options::frontend().usb;
	return true;
}

std::string sourceHint(int source)
{
	if (source == Internal)
		return "Copy your games into " + shownRoot() + "games/ and press Square to scan.";
	if (source == Usb)
	{
		if (!platform::usbAvailable())
			return "USB drives could not be opened: this needs elfldr listening on port 9021. "
					"Restart PSSwanStation once it runs.";
		return "Put your games in a folder named psx (or ps1, playstation) at the top of the drive, "
				"then press Square to scan.";
	}
	if (smb::gameFolders().empty())
		return "Name your share or FTP server in " + shownRoot() + "network.cfg (path = 192.168.1.10/Games/PSX, "
				"or path = ftp://192.168.1.10/games/psx), then restart PSSwanStation.";
	return "Press Square to scan the share again.";
}

}
