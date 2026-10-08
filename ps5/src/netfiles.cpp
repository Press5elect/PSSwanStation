/*
	PSSwanStation - the title's own files on a network share: copies of the
	memory cards, covers, and everything when the files are kept there.

	SPDX-License-Identifier: GPL-3.0-or-later

	network.cfg's "files" line names a folder on an SMB or NFS share (smb.cpp,
	nfs.cpp). Three things can use it:

	  memory cards   <files>/memory cards holds a copy of each card of
	                 data/saves, both ways: a card played on another console
	                 is brought here before a game starts, and one played here
	                 goes there when the game closes. Several consoles at home
	                 then share their saves.
	  covers         <files>/covers is read: a cover there that this console
	                 has not, or has older, is brought into covers/.
	  everything     with Settings, Games and network, "Where my files are
	                 kept" on the network share, every folder of the user's
	                 (bios, covers, cheats, data, textures, music, borders,
	                 layouts, memcards) is kept there, both ways. The console
	                 keeps a working copy, so that the title starts, plays and
	                 saves when the share is off; it is brought up to date at
	                 the start and sent there when a game closes.

	How a folder is kept the same in two places. Each side's files are named
	by a digest of their bytes (XXH64): the share keeps one list of them,
	<folder>/.psswanstation-sync, written by whichever console sent files
	last; this console keeps another, data/sync/<name>.state, of what both
	sides had the last time they were the same. A file that changed on one
	side only goes to the other. A file that changed on both (two consoles
	played the same game, each on its own copy of the card) is a conflict:
	the copy changed last is kept, and the other is kept beside it as
	<name>.conflict-<date>-<time>, on this console, and said on the screen. A
	file removed on one side is removed on the other; one removed here goes
	to data/sync/removed/ first, not away. A file put on the share by hand
	(covers copied there from a PC) is in no list: it is brought here when
	this console has none, or one of another size.

	Nothing is sent or brought while a game runs: when the title starts (on a
	thread of its own; a game cannot start until it is done), when a game
	closes, and when asked (Settings, Games and network, "Bring my files up to
	date now"). A share that does not answer leaves everything as it is.
*/
#include "fe.h"

#include <xxhash.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fe::netfiles
{
namespace
{
constexpr const char *ManifestName = ".psswanstation-sync";
constexpr int Depth = 8;

// The user's folders kept on the share when everything is.
const char *const everyFolder[] = { "bios", "covers", "cheats", "data", "textures", "music", "borders", "layouts",
		"memcards" };

std::mutex runMutex;			// one sync at a time
std::mutex statusMutex;
std::string statusText, lastText;
std::atomic<bool> busy{false};
std::atomic<int> conflictsSeen{0};
std::thread worker;

void say(const std::string& text)
{
	std::lock_guard<std::mutex> lock(statusMutex);
	statusText = text;
}

std::string hex(uint64_t value)
{
	char text[17];
	snprintf(text, sizeof(text), "%016llx", (unsigned long long)value);
	return text;
}

uint64_t digest(const std::vector<uint8_t>& bytes)
{
	return XXH64(bytes.data(), bytes.size(), 0);
}

// A file that is not the user's: half written, a mark, the lists themselves.
bool skipped(const std::string& name)
{
	const auto endsWith = [&](const char *tail) {
		const size_t n = strlen(tail);
		return name.size() >= n && name.compare(name.size() - n, n, tail) == 0;
	};
	return name.empty() || name[0] == '.' || endsWith(".tmp") || endsWith(".part") || name == ManifestName;
}

// What a folder pair leaves out, by its path relative to the pair's folder:
// for the user's data folder, what the title keeps for itself (its caches,
// these lists, the cards' own backups, which stay on each console).
bool leftOut(const std::string& rel, bool dataFolder)
{
	if (!dataFolder)
		return false;
	for (const char *prefix : { "cache/", "sync/", "saves/backups/", "safe-param.json" })
		if (rel.compare(0, strlen(prefix), prefix) == 0)
			return true;
	return false;
}

struct LocalFile
{
	uint64_t size = 0;
	int64_t mtime = 0;
	std::string hash;
};

// What this console remembers of a pair: for each file, the digest both sides
// had the last time they were the same, and its own file's size, time and
// digest (so that an unchanged file is not read again to know it).
struct Remembered
{
	std::string base;
	uint64_t size = 0;
	int64_t mtime = 0;
	std::string hash;
};

std::map<std::string, Remembered> readState(const std::string& path)
{
	std::map<std::string, Remembered> out;
	std::vector<uint8_t> bytes;
	if (!readFile(path, bytes))
		return out;
	std::string text(bytes.begin(), bytes.end());
	size_t at = 0;
	while (at < text.size())
	{
		const size_t end = std::min(text.find('\n', at), text.size());
		const std::string line = text.substr(at, end - at);
		at = end + 1;
		// base \t size \t mtime \t hash \t rel
		std::vector<std::string> parts;
		size_t from = 0;
		for (int i = 0; i < 4; i++)
		{
			const size_t tab = line.find('\t', from);
			if (tab == std::string::npos)
				break;
			parts.push_back(line.substr(from, tab - from));
			from = tab + 1;
		}
		if (parts.size() != 4 || from >= line.size())
			continue;
		Remembered r;
		r.base = parts[0];
		r.size = strtoull(parts[1].c_str(), nullptr, 10);
		r.mtime = strtol(parts[2].c_str(), nullptr, 10);
		r.hash = parts[3];
		out[line.substr(from)] = r;
	}
	return out;
}

void writeState(const std::string& path, const std::map<std::string, Remembered>& state)
{
	std::string text;
	for (const auto& [rel, r] : state)
		text += r.base + "\t" + std::to_string(r.size) + "\t" + std::to_string(r.mtime) + "\t" + r.hash + "\t" + rel + "\n";
	const size_t slash = path.find_last_of('/');
	makeDir(path.substr(0, slash));
	writeFile(path, text.data(), text.size());
}

// The share's list: digest, size and the time it was sent, by relative path.
struct Listed
{
	std::string hash;
	uint64_t size = 0;
	int64_t time = 0;
};

std::map<std::string, Listed> parseManifest(const std::vector<uint8_t>& bytes)
{
	std::map<std::string, Listed> out;
	std::string text(bytes.begin(), bytes.end());
	size_t at = 0;
	while (at < text.size())
	{
		const size_t end = std::min(text.find('\n', at), text.size());
		const std::string line = text.substr(at, end - at);
		at = end + 1;
		const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1),
				c = b == std::string::npos ? b : line.find('\t', b + 1);
		if (c == std::string::npos)
			continue;
		Listed l;
		l.hash = line.substr(0, a);
		l.size = strtoull(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10);
		l.time = strtol(line.substr(b + 1, c - b - 1).c_str(), nullptr, 10);
		out[line.substr(c + 1)] = l;
	}
	return out;
}

std::string manifestText(const std::map<std::string, Listed>& listed)
{
	std::string text;
	for (const auto& [rel, l] : listed)
		text += l.hash + "\t" + std::to_string(l.size) + "\t" + std::to_string(l.time) + "\t" + rel + "\n";
	return text;
}

// A file's time to the nanosecond, so that one written twice in a second is
// still seen to have changed.
int64_t nanoTime(const struct stat& st)
{
	return (int64_t)st.st_mtim.tv_sec * 1000000000ll + st.st_mtim.tv_nsec;
}

// Every file below `dir` on this console, by path relative to `top`.
void walkLocal(const std::string& top, const std::string& rel, int depth, bool recurse, bool everything,
		std::map<std::string, std::pair<uint64_t, int64_t>>& out)
{
	DIR *dir = opendir((top + rel).c_str());
	if (dir == nullptr)
		return;
	std::vector<std::string> names;
	while (const dirent *entry = readdir(dir))
		if (!skipped(entry->d_name))
			names.push_back(entry->d_name);
	closedir(dir);
	for (const std::string& name : names)
	{
		const std::string child = rel + name;
		struct stat st;
		if (stat((top + child).c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			if (recurse && depth < Depth && !leftOut(child + "/", everything))
				walkLocal(top, child + "/", depth + 1, recurse, everything, out);
		}
		else if (S_ISREG(st.st_mode) && !leftOut(child, everything) && name.find(".conflict-") == std::string::npos)
			out[child] = { (uint64_t)st.st_size, nanoTime(st) };
	}
}

// Every file below `dir` on the share. False when a folder could not be
// listed (then nothing is decided from what is missing).
bool walkRemote(const std::string& top, const std::string& rel, int depth, bool recurse, bool everything,
		std::map<std::string, uint64_t>& out)
{
	const unsigned before = smb::failures();
	const std::vector<smb::Entry> entries = smb::list(top + rel);
	if (smb::failures() != before)
		return false;
	for (const smb::Entry& entry : entries)
	{
		if (skipped(entry.name))
			continue;
		const std::string child = rel + entry.name;
		if (entry.directory)
		{
			if (recurse && depth < Depth && !leftOut(child + "/", everything)
					&& !walkRemote(top, child + "/", depth + 1, recurse, everything, out))
				return false;
		}
		else if (!leftOut(child, everything))
			out[child] = entry.size;
	}
	return true;
}

std::string stamp()
{
	const time_t now = time(nullptr);
	struct tm tm;
	localtime_r(&now, &tm);
	char text[32];
	strftime(text, sizeof(text), "%Y%m%d-%H%M", &tm);
	return text;
}

struct Report
{
	int pulled = 0, pushed = 0, removed = 0, conflicts = 0;
	bool failed = false;
	std::string error;
};

enum class Way { Both, Bring };

// Keeps the folder `local` (a path on this console, ending in '/') and
// `remote` (a share's folder, without the trailing '/') the same.
void syncPair(const std::string& name, const std::string& local, const std::string& remote, Way way, bool recurse,
		bool dataFolder, Report& report)
{
	makeDir(local);
	const std::string statePath = rootDir + "data/sync/" + name + ".state";
	std::map<std::string, Remembered> state = readState(statePath);
	// The share.
	std::map<std::string, uint64_t> remoteFiles;
	if (!walkRemote(remote + "/", "", 0, recurse, dataFolder, remoteFiles))
	{
		report.failed = true;
		report.error = "the share could not be listed (" + smb::lastError() + ")";
		return;
	}
	std::map<std::string, Listed> listed;
	{
		std::vector<uint8_t> bytes;
		if (smb::readWhole(remote + "/" + ManifestName, bytes))
			listed = parseManifest(bytes);
	}
	// This console.
	std::map<std::string, std::pair<uint64_t, int64_t>> localFiles;
	walkLocal(local, "", 0, recurse, dataFolder, localFiles);
	const auto localHash = [&](const std::string& rel) -> std::string {
		const auto it = localFiles.find(rel);
		if (it == localFiles.end())
			return "";
		Remembered& r = state[rel];
		if (!r.hash.empty() && r.size == it->second.first && r.mtime == it->second.second)
			return r.hash;
		std::vector<uint8_t> bytes;
		if (!readFile(local + rel, bytes))
			return "";
		r.size = it->second.first;
		r.mtime = it->second.second;
		r.hash = hex(digest(bytes));
		return r.hash;
	};
	std::set<std::string> names;
	for (const auto& [rel, size] : remoteFiles)
		names.insert(rel);
	for (const auto& [rel, info] : localFiles)
		names.insert(rel);
	for (const auto& [rel, r] : state)
		names.insert(rel);
	bool manifestChanged = false;
	int done = 0;
	const int total = (int)names.size();
	for (const std::string& rel : names)
	{
		done++;
		if (done % 16 == 0)
			say(format("%d of %d files", done, total));
		const std::string lh = localHash(rel);
		const auto onShare = remoteFiles.find(rel);
		std::string rh;
		bool unknown = false;
		if (onShare != remoteFiles.end())
		{
			const auto l = listed.find(rel);
			if (l != listed.end() && l->second.size == onShare->second)
				rh = l->second.hash;
			else
				unknown = true;		// put there by hand, or changed by something else
		}
		const std::string base = state.count(rel) != 0 ? state[rel].base : "";
		const auto pull = [&]() -> bool {
			std::vector<uint8_t> bytes;
			if (!smb::readWhole(remote + "/" + rel, bytes))
			{
				report.failed = true;
				report.error = rel + " could not be read from the share";
				return false;
			}
			const size_t slash = rel.find_last_of('/');
			if (slash != std::string::npos)
				makeDir(local + rel.substr(0, slash));
			if (!writeFile(local + rel, bytes.data(), bytes.size()))
			{
				report.failed = true;
				report.error = local + rel + " could not be written";
				return false;
			}
			struct stat st;
			stat((local + rel).c_str(), &st);
			Remembered& r = state[rel];
			r.hash = r.base = hex(digest(bytes));
			r.size = bytes.size();
			r.mtime = nanoTime(st);
			if (unknown)
			{
				listed[rel] = { r.hash, r.size, (int64_t)time(nullptr) };
				manifestChanged = way == Way::Both;
			}
			report.pulled++;
			return true;
		};
		const auto push = [&]() -> bool {
			std::vector<uint8_t> bytes;
			std::string error;
			if (!readFile(local + rel, bytes) || !smb::writeFile(remote + "/" + rel, bytes.data(), bytes.size(), error))
			{
				report.failed = true;
				report.error = rel + ": " + (error.empty() ? std::string("could not be read here") : error);
				return false;
			}
			state[rel].base = lh;
			// The time the file was changed here, which a conflict is decided by.
			listed[rel] = { lh, bytes.size(), localFiles[rel].second / 1000000000ll };
			manifestChanged = true;
			report.pushed++;
			return true;
		};
		bool ok = true;
		if (!lh.empty() && onShare != remoteFiles.end())
		{
			if (unknown)
			{
				// A file of the share's that no console's list knows.
				if (localFiles[rel].first != onShare->second)
					ok = lh == base || base.empty() ? pull() : push();
				else
				{
					state[rel].base = lh;
					if (way == Way::Both)
					{
						listed[rel] = { lh, onShare->second, (int64_t)time(nullptr) };
						manifestChanged = true;
					}
				}
			}
			else if (lh == rh)
				state[rel].base = lh;
			else if (lh == base)
				ok = pull();
			else if (rh == base || way == Way::Bring)
				ok = way == Way::Both ? push() : pull();
			else
			{
				// Changed on both sides: the one changed last is kept, the other beside it.
				const int64_t localTime = localFiles[rel].second / 1000000000ll, remoteTime = listed[rel].time;
				const std::string aside = local + rel + ".conflict-" + stamp();
				report.conflicts++;
				if (localTime >= remoteTime)
				{
					std::vector<uint8_t> theirs;
					if (smb::readWhole(remote + "/" + rel, theirs))
						writeFile(aside, theirs.data(), theirs.size());
					ok = push();
				}
				else
				{
					rename((local + rel).c_str(), aside.c_str());
					ok = pull();
				}
				diag::mark("files: %s changed here and on the share: the newer kept, the other as %s", rel.c_str(),
						aside.c_str());
			}
		}
		else if (!lh.empty())
		{
			// Here only.
			if (!base.empty() && lh == base && way == Way::Both)
			{
				// Removed from the share since: removed here too, into data/sync/removed.
				const std::string away = rootDir + "data/sync/removed/" + name + "/" + rel;
				makeDir(away.substr(0, away.find_last_of('/')));
				rename((local + rel).c_str(), away.c_str());
				state.erase(rel);
				report.removed++;
			}
			else if (way == Way::Both)
				ok = push();
		}
		else if (onShare != remoteFiles.end())
		{
			// On the share only.
			if (!base.empty() && !unknown && rh == base && way == Way::Both)
			{
				if (smb::remove(remote + "/" + rel))
				{
					listed.erase(rel);
					manifestChanged = true;
					state.erase(rel);
					report.removed++;
				}
			}
			else
				ok = pull();
		}
		else
		{
			// Neither has it any more.
			state.erase(rel);
			if (listed.erase(rel) != 0)
				manifestChanged = true;
		}
		if (!ok)
			break;
	}
	if (manifestChanged && way == Way::Both)
	{
		const std::string text = manifestText(listed);
		std::string error;
		if (!smb::writeFile(remote + "/" + ManifestName, text.data(), text.size(), error) && !report.failed)
		{
			report.failed = true;
			report.error = "the share's list could not be written: " + error;
		}
	}
	writeState(statePath, state);
}

// What to keep the same, as it is set now.
void runOnce(const char *why)
{
	std::lock_guard<std::mutex> lock(runMutex);
	const std::string files = smb::filesFolder();
	const options::Frontend& f = options::frontend();
	if (files.empty() || (f.filesAt != 3 && !f.cardsOnShare && !f.coversFromShare))
		return;
	busy = true;
	say("Asking the share");
	Report report;
	if (f.filesAt == 3)
		for (const char *folder : everyFolder)
		{
			say(std::string("Your ") + folder);
			syncPair(std::string("all-") + folder, rootDir + folder + "/", files + "/" + folder, Way::Both, true,
					strcmp(folder, "data") == 0, report);
			if (report.failed)
				break;
		}
	else
	{
		if (f.cardsOnShare)
		{
			say("Memory cards");
			syncPair("memory-cards", rootDir + "data/saves/", files + "/memory cards", Way::Both, false, false, report);
		}
		if (f.coversFromShare && !report.failed)
		{
			say("Covers");
			syncPair("covers", rootDir + "covers/", files + "/covers", Way::Bring, true, false, report);
		}
	}
	std::string summary;
	if (report.failed)
		summary = "Not brought up to date: " + report.error + ".";
	else if (report.pulled + report.pushed + report.removed == 0)
		summary = "Up to date: nothing had changed.";
	else
		summary = format("Up to date: %d brought here, %d sent to the share%s.", report.pulled, report.pushed,
				report.removed > 0 ? format(", %d removed", report.removed).c_str() : "");
	if (report.conflicts > 0)
	{
		summary += format(" %d file(s) had changed here and on the share: the newer is in use, the other kept beside "
				"it as .conflict-<date>.", report.conflicts);
		conflictsSeen += report.conflicts;
	}
	diag::mark("files: %s (%s)", summary.c_str(), why);
	{
		std::lock_guard<std::mutex> l(statusMutex);
		lastText = summary;
		statusText.clear();
	}
	busy = false;
}

void runAsync(const char *why)
{
	if (busy.exchange(true))
		return;
	if (worker.joinable())
		worker.join();
	worker = std::thread([why] {
		runOnce(why);
		busy = false;
	});
}

} // namespace

bool wanted()
{
	const options::Frontend& f = options::frontend();
	return !smb::filesFolder().empty() && (f.filesAt == 3 || f.cardsOnShare || f.coversFromShare);
}

void startUp()
{
	if (wanted())
		runAsync("start");
}

void gameClosed()
{
	if (wanted())
		runAsync("a game closed");
}

void now()
{
	runAsync("asked for");
}

bool working()
{
	return busy;
}

void finish()
{
	if (worker.joinable())
		worker.join();
}

std::string status()
{
	std::lock_guard<std::mutex> lock(statusMutex);
	if (busy)
		return statusText.empty() ? std::string("Working") : statusText;
	return lastText;
}

int conflicts()
{
	return conflictsSeen;
}

}
