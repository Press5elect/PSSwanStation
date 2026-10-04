/*
	SwanStation for PS5 - what is known about a game: its description, who made
	it and when, by the disc's serial number.

	SPDX-License-Identifier: GPL-3.0-or-later

	The database is <root>assets/gamedb.zip, made by ps5/tools/make-gamedb.py
	from the libretro database's PlayStation lists (CC BY-SA 4.0): about ten
	thousand games by serial number, and the Redump names of the discs, by
	which a game in the library is known before it has ever run.

	Both lists are kept as the text they are (4 MB), with an index of where
	each line starts.
*/
#include "fe.h"

#include <miniz.h>

#include <cstring>
#include <unordered_map>

namespace fe::gamedb
{
namespace
{
std::string gamesText;
std::unordered_map<std::string, uint32_t> bySerialIndex;	// serial -> the line's start
std::unordered_map<std::string, std::string> byName;		// reduced name -> serial
std::string summaryText = "not installed (assets/gamedb.zip)";

bool extract(mz_zip_archive& zip, const char *name, std::string& out)
{
	const int index = mz_zip_reader_locate_file(&zip, name, nullptr, 0);
	if (index < 0)
		return false;
	size_t bytes = 0;
	void *data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)index, &bytes, 0);
	if (data == nullptr)
		return false;
	out.assign(static_cast<const char *>(data), bytes);
	mz_free(data);
	return true;
}

// As make-gamedb.py reduces a name: its letters and digits, lower case.
std::string reduce(const std::string& name)
{
	std::string out;
	for (const char c : name)
	{
		if (c >= 'A' && c <= 'Z')
			out += (char)(c - 'A' + 'a');
		else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
			out += c;
	}
	return out;
}
}

void init()
{
	std::vector<uint8_t> file;
	mz_zip_archive zip{};
	if (!readFile(rootDir + "assets/gamedb.zip", file) || file.empty()
			|| !mz_zip_reader_init_mem(&zip, file.data(), file.size(), 0))
	{
		diag::mark("gamedb: %s", summaryText.c_str());
		return;
	}
	std::string names;
	const bool ok = extract(zip, "games.tsv", gamesText) && extract(zip, "names.tsv", names);
	mz_zip_reader_end(&zip);
	if (!ok)
	{
		gamesText.clear();
		diag::mark("gamedb: the archive could not be read");
		return;
	}
	for (size_t at = 0; at < gamesText.size();)
	{
		size_t end = gamesText.find('\n', at);
		if (end == std::string::npos)
			end = gamesText.size();
		const size_t tab = gamesText.find('\t', at);
		if (tab != std::string::npos && tab < end)
			bySerialIndex.emplace(gamesText.substr(at, tab - at), (uint32_t)at);
		at = end + 1;
	}
	for (size_t at = 0; at < names.size();)
	{
		size_t end = names.find('\n', at);
		if (end == std::string::npos)
			end = names.size();
		const size_t tab = names.find('\t', at);
		if (tab != std::string::npos && tab < end)
			byName.emplace(names.substr(at, tab - at), names.substr(tab + 1, end - tab - 1));
		at = end + 1;
	}
	summaryText = format("%d games described, %d disc names", (int)bySerialIndex.size(), (int)byName.size());
	diag::mark("gamedb: %s", summaryText.c_str());
}

bool find(const std::string& serial, Info& out)
{
	const auto it = bySerialIndex.find(serial);
	if (it == bySerialIndex.end())
		return false;
	size_t end = gamesText.find('\n', it->second);
	if (end == std::string::npos)
		end = gamesText.size();
	std::string fields[9];
	size_t at = it->second;
	for (int i = 0; i < 9 && at <= end; i++)
	{
		size_t tab = i == 8 ? end : gamesText.find('\t', at);
		if (tab == std::string::npos || tab > end)
			tab = end;
		fields[i] = gamesText.substr(at, tab - at);
		at = tab + 1;
	}
	out.serial = fields[0];
	out.name = fields[1];
	out.developer = fields[2];
	out.publisher = fields[3];
	out.year = atoi(fields[4].c_str());
	out.month = atoi(fields[5].c_str());
	out.players = atoi(fields[6].c_str());
	out.genre = fields[7];
	out.description = fields[8];
	return true;
}

std::string serialByName(const std::string& fileTitle)
{
	const auto it = byName.find(reduce(fileTitle));
	return it == byName.end() ? "" : it->second;
}

std::string summary()
{
	return summaryText;
}

}
