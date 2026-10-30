// PSSwanStation - the rules the library groups a game's discs by, header-only
// so tests/library checks them on the PC.
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A disc is known by its name: "(Disc 2)", "(Disc 2 of 3)", "[CD 2]", "(Disc B)"
// or "(Disc Two)" anywhere in it, as dump sets name them, or "CD2", "Disc 2" or
// "Disk2" at its end after a space, a dash or an underscore ("Game CD2",
// "Game - Disc 2"), as other sets do. The discs of one game are those whose
// names are the same without it, in one folder, or each in a folder of its own
// named after the disc beside the others ("Game (Disc 1)/Game (Disc 1).cue"),
// or in folders that are only the disc's number ("Game/CD1/game.cue").
#pragma once

#include <cctype>
#include <cstring>
#include <string>

namespace fe::library::names
{

inline std::string lower(std::string text)
{
	for (char& c : text)
		c = (char)std::tolower((unsigned char)c);
	return text;
}

inline std::string trimmed(const std::string& text)
{
	size_t first = 0, last = text.size();
	while (first < last && (text[first] == ' ' || text[first] == '\t'))
		first++;
	while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t'))
		last--;
	return text.substr(first, last - first);
}

// "Game (USA) (Disc 2)" -> "Game (USA)" and 2; 0 when the name has no disc.
inline int discNumber(const std::string& title, std::string& base)
{
	const std::string low = lower(title);
	// In brackets, anywhere.
	for (const char *word : { "(disc ", "(disk ", "(cd ", "[disc ", "[disk ", "[cd " })
	{
		const size_t at = low.find(word);
		if (at == std::string::npos)
			continue;
		size_t i = at + std::strlen(word);
		int number = 0;
		if (i < low.size() && std::isdigit((unsigned char)low[i]))
		{
			while (i < low.size() && std::isdigit((unsigned char)low[i]))
				number = number * 10 + (low[i++] - '0');
		}
		else if (i < low.size() && low[i] >= 'a' && low[i] <= 'h'
				&& (i + 1 >= low.size() || !std::isalpha((unsigned char)low[i + 1])))
			number = low[i++] - 'a' + 1;
		else
		{
			static const char *const words[] = { "one", "two", "three", "four", "five", "six", "seven", "eight" };
			for (int w = 0; w < 8; w++)
				if (low.compare(i, std::strlen(words[w]), words[w]) == 0
						&& (i + std::strlen(words[w]) >= low.size() || !std::isalpha((unsigned char)low[i + std::strlen(words[w])])))
				{
					number = w + 1;
					i += std::strlen(words[w]);
					break;
				}
		}
		if (number <= 0)
			continue;
		const size_t end = low.find_first_of(")]", i);
		if (end == std::string::npos)
			continue;
		base = trimmed(trimmed(title.substr(0, at)) + title.substr(end + 1));
		return number;
	}
	// At the end, after a separator: "Game CD2", "Game - Disc 2", "Game_disk2".
	size_t digits = low.size();
	while (digits > 0 && std::isdigit((unsigned char)low[digits - 1]))
		digits--;
	if (digits == low.size() || low.size() - digits > 2)
		return 0;
	size_t word = digits;
	if (word > 0 && low[word - 1] == ' ')
		word--;
	for (const char *name : { "disc", "disk", "cd" })
	{
		const size_t length = std::strlen(name);
		if (word < length || low.compare(word - length, length, name) != 0)
			continue;
		const size_t before = word - length;
		if (before == 0 || (low[before - 1] != ' ' && low[before - 1] != '-' && low[before - 1] != '_'))
			return 0;
		const int number = std::stoi(low.substr(digits));
		if (number <= 0)
			return 0;
		size_t cut = before;
		while (cut > 0 && (low[cut - 1] == ' ' || low[cut - 1] == '-' || low[cut - 1] == '_'))
			cut--;
		base = trimmed(title.substr(0, cut));
		return base.empty() ? 0 : number;
	}
	return 0;
}

// A folder that is only a disc's number ("CD1", "Disc 2", "disk_3"): the
// number, the game being the folder above ("Game/CD1/game.cue"). 0 otherwise.
inline int folderDisc(const std::string& folder)
{
	if (folder.size() < 2)
		return 0;
	const size_t slash = folder.find_last_of('/', folder.size() - 2);
	const std::string name = lower(folder.substr(slash == std::string::npos ? 0 : slash + 1,
			folder.size() - 1 - (slash == std::string::npos ? 0 : slash + 1)));
	for (const char *word : { "disc", "disk", "cd" })
	{
		const size_t length = std::strlen(word);
		if (name.compare(0, length, word) != 0)
			continue;
		size_t i = length;
		if (i < name.size() && (name[i] == ' ' || name[i] == '_' || name[i] == '-'))
			i++;
		if (i >= name.size() || name.size() - i > 2)
			return 0;
		for (size_t k = i; k < name.size(); k++)
			if (!std::isdigit((unsigned char)name[k]))
				return 0;
		return std::stoi(name.substr(i));
	}
	return 0;
}

// The folder a disc's group is kept by: its own, or the one above it when its
// folder is named after the disc ("Game (Disc 2)/Game (Disc 2).cue" and its
// sisters). `folder` ends in '/'.
inline std::string groupFolder(const std::string& folder)
{
	if (folder.size() < 2)
		return folder;
	const size_t slash = folder.find_last_of('/', folder.size() - 2);
	const std::string name = folder.substr(slash == std::string::npos ? 0 : slash + 1,
			folder.size() - 1 - (slash == std::string::npos ? 0 : slash + 1));
	std::string base;
	if (discNumber(name, base) > 0 || folderDisc(folder) > 0)
		return slash == std::string::npos ? std::string() : folder.substr(0, slash + 1);
	return folder;
}

}
