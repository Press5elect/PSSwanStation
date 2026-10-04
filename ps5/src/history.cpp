/*
	SwanStation for PS5 - what was played: when, for how long, and which disc
	of a game on several was in the tray.

	SPDX-License-Identifier: GPL-3.0-or-later

	<root>data/history.txt, a line a game: its path in the library, when it was
	last played, the seconds played and the disc. The library's "Continue
	playing" shelf, the details' "last played" and the disc a game starts from
	all come from here.
*/
#include "fe.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <sstream>

namespace fe::history
{
namespace
{
std::map<std::string, Entry> entries;
std::string playing;
double playingSince;
unsigned changes;

std::string file()
{
	return rootDir + "data/history.txt";
}

void save()
{
	std::string text;
	for (const auto& [path, entry] : entries)
		text += format("%lld\t%llu\t%d\t%s\n", (long long)entry.lastPlayed, (unsigned long long)entry.seconds,
				entry.disc, path.c_str());
	writeFile(file(), text.data(), text.size());
	changes++;
}
}

void init()
{
	std::vector<uint8_t> data;
	if (!readFile(file(), data))
		return;
	std::istringstream lines(std::string(data.begin(), data.end()));
	std::string line;
	while (std::getline(lines, line))
	{
		long long when = 0;
		unsigned long long seconds = 0;
		int disc = 0, used = 0;
		if (sscanf(line.c_str(), "%lld\t%llu\t%d\t%n", &when, &seconds, &disc, &used) < 3 || used <= 0
				|| (size_t)used >= line.size())
			continue;
		Entry& entry = entries[trim(line.substr((size_t)used))];
		entry.lastPlayed = when;
		entry.seconds = seconds;
		entry.disc = std::max(disc, 0);
	}
}

Entry get(const std::string& gamePath)
{
	const auto it = entries.find(gamePath);
	return it == entries.end() ? Entry() : it->second;
}

void begin(const std::string& gamePath)
{
	end();
	if (gamePath.empty())
		return;
	playing = gamePath;
	playingSince = now();
	entries[gamePath].lastPlayed = (int64_t)time(nullptr);
	save();
}

void end()
{
	if (playing.empty())
		return;
	Entry& entry = entries[playing];
	entry.seconds += (uint64_t)std::max(now() - playingSince, 0.0);
	entry.lastPlayed = (int64_t)time(nullptr);
	playing.clear();
	save();
}

void setDisc(const std::string& gamePath, int disc)
{
	if (gamePath.empty() || entries[gamePath].disc == disc)
		return;
	entries[gamePath].disc = disc;
	save();
}

std::vector<std::string> recent(size_t most)
{
	std::vector<std::pair<int64_t, std::string>> played;
	for (const auto& [path, entry] : entries)
		if (entry.lastPlayed != 0)
			played.emplace_back(entry.lastPlayed, path);
	std::sort(played.begin(), played.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	std::vector<std::string> paths;
	for (size_t i = 0; i < played.size() && i < most; i++)
		paths.push_back(played[i].second);
	return paths;
}

unsigned generation()
{
	return changes;
}

}
