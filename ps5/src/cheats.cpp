/*
	PSSwanStation - cheats and patches, from DuckStation's database.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The codes are those of github.com/duckstation/chtdb, the cheat and patch
	collection DuckStation ships ("can also be used by other emulators that
	support GameShark codes", its README says): two archives in the title's
	folder, assets/cheats.zip and assets/patches.zip, each holding one
	<serial>.cht per game. A file of the user's own, <root>cheats/<serial>.cht
	in the same format, is read as well. For a game neither has codes for,
	assets/libretro-cheats.zip, the libretro database's PlayStation cheats
	matched to discs by name (CC BY-SA 4.0, tools/make-libretro-cheats.py).

	The format (the collection's cheat-format.txt):

		[Group\Name]            a section for each entry; a group's entries
		                        exclude each other (the widescreen ratios)
		Type = Gameshark
		Activation = EndFrame   or Manual: applied once, on request
		Author = ...  Description = ...
		Option = Name:0x1F      named values for the code's '?' digits
		OptionRange = 0:15      or a range of them
		OverrideAspectRatio = 16:9, DisableWidescreenRendering = True,
		OverrideCPUOverclock = 200, Enable8MBRAM = True
		                        emulator settings the entry asks for
		80012345 0001           the code lines

	SwanStation's cheat engine runs GameShark codes and DuckStation's
	extension types (src/core/cheats.cpp). An entry using a type it does not
	have is shown, marked, and cannot be switched on. The enabled entries are
	handed to the core through libretro's cheat calls; the settings they ask
	for are laid over the game's options while they are on.

	Which entries are on is kept per game in <root>data/cheats/<serial>.txt.
*/
#include "fe.h"
#include "tuning.h"
#include "netplay.h"

#include <libretro.h>
#include <miniz.h>

#include "core/cheats.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <dirent.h>
#include <memory>
#include <sstream>
#include <sys/stat.h>

namespace fe::cheats
{
namespace
{
std::vector<Cheat> entries;
std::string loadedSerial;
std::string summaryText;
std::mutex summaryMutex;

// The database the title came with (assets/), or a newer one fetched since
// (data/, see refresh()).
std::string cheatsArchive()
{
	const std::string fetched = rootDir + "data/cheats.zip";
	return fileExists(fetched) ? fetched : appDir + "assets/cheats.zip";
}

std::string patchesArchive()
{
	const std::string fetched = rootDir + "data/patches.zip";
	return fileExists(fetched) ? fetched : appDir + "assets/patches.zip";
}

std::mutex archiveMutex;
using Archive = std::shared_ptr<const std::vector<uint8_t>>;
std::map<std::string, Archive>& keptArchives()
{
	static std::map<std::string, Archive> kept;
	return kept;
}

// The two archives are small (2.5 MB and 80 KB): each is read once and kept,
// until a newer one is fetched. Whoever reads one holds on to it meanwhile.
Archive archiveData(const std::string& archive)
{
	std::lock_guard<std::mutex> lock(archiveMutex);
	std::map<std::string, Archive>& kept = keptArchives();
	auto it = kept.find(archive);
	if (it == kept.end())
	{
		auto bytes = std::make_shared<std::vector<uint8_t>>();
		readFile(archive, *bytes);
		it = kept.emplace(archive, std::move(bytes)).first;
	}
	return it->second;
}

bool readFromArchive(const std::string& archive, const std::string& name, std::string& text)
{
	const Archive held = archiveData(archive);
	const std::vector<uint8_t>& data = *held;
	mz_zip_archive zip{};
	if (data.empty() || !mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0))
		return false;
	bool found = false;
	const int index = mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, 0);
	if (index >= 0)
	{
		size_t bytes = 0;
		void *content = mz_zip_reader_extract_to_heap(&zip, (mz_uint)index, &bytes, 0);
		if (content != nullptr)
		{
			text.assign(static_cast<const char *>(content), bytes);
			mz_free(content);
			found = true;
		}
	}
	mz_zip_reader_end(&zip);
	return found;
}

int countArchive(const std::string& archive)
{
	const Archive held = archiveData(archive);
	const std::vector<uint8_t>& data = *held;
	mz_zip_archive zip{};
	if (data.empty() || !mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0))
		return -1;
	const int files = (int)mz_zip_reader_get_num_files(&zip);
	mz_zip_reader_end(&zip);
	return files;
}

bool isHex(const std::string& text, bool placeholders)
{
	if (text.empty())
		return false;
	for (char c : text)
		if (!isxdigit((unsigned char)c) && !(placeholders && c == '?'))
			return false;
	return true;
}

// The code types SwanStation's engine has (CheatCode::InstructionCode).
bool knownType(unsigned type)
{
	static const uint8_t known[] = {
		0x00, 0x30, 0x80, 0x1F, 0x10, 0x11, 0x20, 0x21, 0xC1, 0xC0, 0xD5, 0xD6, 0xD4, 0xD0, 0xD1, 0xD2, 0xD3,
		0xE0, 0xE1, 0xE2, 0xE3, 0x50, 0xC2, 0x53, 0x90, 0xA5, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0x60, 0x61, 0xA6,
		0xA7, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0x31, 0x32, 0x81, 0x82, 0x91, 0x92, 0xD7, 0xC3, 0xC4, 0xC5,
		0xC6, 0xF6, 0x51, 0x52, 0xE4, 0xE5,
	};
	for (uint8_t k : known)
		if (k == type)
			return true;
	return false;
}

// Types whose following lines are data, not codes of their own.
bool hasDataLines(unsigned type)
{
	return type == 0x50 || type == 0x53 || type == 0xF4 || type == 0xF6 || type == 0xC2;
}

uint32_t parseNumber(const std::string& text)
{
	return (uint32_t)strtoul(text.c_str(), nullptr, 0);
}

void finish(Cheat& cheat, bool sawUnknown, bool sawData, bool ignore, std::vector<Cheat>& out)
{
	if (cheat.name.empty() || cheat.code.empty() || ignore)
		return;
	cheat.supported = !sawUnknown || sawData;
	if (!cheat.choices.empty())
		cheat.value = cheat.choices.front().second;
	else if (cheat.hasRange)
		cheat.value = cheat.rangeLow;
	out.push_back(cheat);
}

void parse(const std::string& text, bool patch, std::vector<Cheat>& out)
{
	std::istringstream lines(text);
	std::string line;
	Cheat cheat;
	bool open = false, sawUnknown = false, sawData = false, ignore = false;
	while (std::getline(lines, line))
	{
		line = trim(line);
		if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == ':')
			continue;
		if (line[0] == '[' && line.back() == ']')
		{
			if (open)
				finish(cheat, sawUnknown, sawData, ignore, out);
			cheat = Cheat();
			cheat.patch = patch;
			open = true;
			sawUnknown = sawData = ignore = false;
			std::string name = line.substr(1, line.size() - 2);
			const size_t slash = name.find('\\');
			if (slash != std::string::npos)
			{
				cheat.group = trim(name.substr(0, slash));
				cheat.name = trim(name.substr(slash + 1));
			}
			else
				cheat.name = trim(name);
			continue;
		}
		if (!open)
			continue;
		const size_t equals = line.find('=');
		if (equals != std::string::npos && !isxdigit((unsigned char)line[0]) == false && line.find(' ') > equals)
		{
			// "A = b" where A happens to start with a hex digit (Author,
			// Activation, Description...): a key, not a code.
		}
		if (equals != std::string::npos)
		{
			const std::string key = lowercase(trim(line.substr(0, equals)));
			const std::string value = trim(line.substr(equals + 1));
			const bool yes = lowercase(value) == "true";
			if (key == "type")
			{
				if (lowercase(value) != "gameshark")
					sawUnknown = true;
			}
			else if (key == "activation")
				cheat.manual = lowercase(value) == "manual";
			else if (key == "author")
				cheat.author = value;
			else if (key == "description")
				cheat.description = value;
			else if (key == "ignore")
				ignore = yes;
			else if (key == "option")
			{
				const size_t colon = value.rfind(':');
				if (colon != std::string::npos)
					cheat.choices.push_back({ trim(value.substr(0, colon)), parseNumber(trim(value.substr(colon + 1))) });
			}
			else if (key == "optionrange")
			{
				const size_t colon = value.find(':');
				if (colon != std::string::npos)
				{
					cheat.hasRange = true;
					cheat.rangeLow = parseNumber(trim(value.substr(0, colon)));
					cheat.rangeHigh = parseNumber(trim(value.substr(colon + 1)));
					if (cheat.rangeHigh < cheat.rangeLow)
						std::swap(cheat.rangeLow, cheat.rangeHigh);
				}
			}
			else if (key == "overrideaspectratio")
			{
				static const char *listed[] = { "4:3", "16:9", "19:9", "20:9" };
				bool known = false;
				for (const char *ratio : listed)
					known = known || value == ratio;
				if (known)
					cheat.settings.push_back({ "swanstation_Display_AspectRatio", value });
				else
				{
					const size_t colon = value.find(':');
					if (colon != std::string::npos)
					{
						cheat.settings.push_back({ "swanstation_Display_AspectRatio", "Custom" });
						cheat.settings.push_back({ "swanstation_Display_CustomAspectRatioNumerator",
								trim(value.substr(0, colon)) });
						cheat.settings.push_back({ "swanstation_Display_CustomAspectRatioDenominator",
								trim(value.substr(colon + 1)) });
					}
				}
			}
			else if (key == "disablewidescreenrendering")
			{
				if (yes)
					cheat.settings.push_back({ "swanstation_GPU_WidescreenHack", "false" });
			}
			else if (key == "overridecpuoverclock")
				cheat.settings.push_back({ "swanstation_CPU_Overclock", value });
			else if (key == "enable8mbram")
			{
				if (yes)
					cheat.settings.push_back({ "swanstation_Console_Enable8MBRAM", "true" });
			}
			continue;
		}
		// A code line: eight hex digits, a space, four to eight more.
		const size_t space = line.find(' ');
		if (space == std::string::npos)
		{
			sawUnknown = true;
			continue;
		}
		const std::string first = line.substr(0, space);
		std::string second = trim(line.substr(space + 1));
		const size_t comment = second.find_first_of(" \t;");
		if (comment != std::string::npos)
			second = second.substr(0, comment);
		if (first.size() != 8 || !isHex(first, true) || second.size() < 2 || second.size() > 8 || !isHex(second, true))
		{
			sawUnknown = true;
			continue;
		}
		if (isHex(first.substr(0, 2), false))
		{
			const unsigned type = (unsigned)strtoul(first.substr(0, 2).c_str(), nullptr, 16);
			if (hasDataLines(type))
				sawData = true;
			else if (!knownType(type))
				sawUnknown = true;
		}
		if (!cheat.code.empty())
			cheat.code += '+';
		cheat.code += first + ' ' + second;
	}
	if (open)
		finish(cheat, sawUnknown, sawData, ignore, out);
}

std::string stateFile()
{
	return rootDir + "data/cheats/" + loadedSerial + ".txt";
}

std::string keyOf(const Cheat& cheat)
{
	if (!cheat.mod.empty())
		return "mod|" + cheat.mod + "|" + cheat.name;
	return (cheat.patch ? "patch|" : "cheat|") + cheat.group + "|" + cheat.name;
}

void loadState()
{
	FILE *f = fopen(stateFile().c_str(), "r");
	if (f == nullptr)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		char *tab = strchr(line, '\t');
		if (tab == nullptr)
			continue;
		*tab = 0;
		const std::string key = line;
		unsigned on = 0, value = 0;
		if (sscanf(tab + 1, "%u\t%u", &on, &value) < 1)
			continue;
		for (Cheat& cheat : entries)
			if (keyOf(cheat) == key)
			{
				cheat.enabled = on != 0 && cheat.supported;
				if (!cheat.choices.empty() || cheat.hasRange)
					cheat.value = value;
			}
	}
	fclose(f);
}

void saveState()
{
	if (loadedSerial.empty())
		return;
	bool any = false;
	for (const Cheat& cheat : entries)
		any = any || cheat.enabled || !cheat.mod.empty() || ((!cheat.choices.empty() || cheat.hasRange) && cheat.value != 0);
	const std::string file = stateFile();
	if (!any)
	{
		remove(file.c_str());
		return;
	}
	FILE *f = fopen(file.c_str(), "w");
	if (f == nullptr)
		return;
	for (const Cheat& cheat : entries)
		// A mod's feature is kept either way: one on by default stays off when switched off.
		if (cheat.enabled || !cheat.mod.empty() || !cheat.choices.empty() || cheat.hasRange)
			fprintf(f, "%s\t%u\t%u\n", keyOf(cheat).c_str(), cheat.enabled ? 1u : 0u, cheat.value);
	fclose(f);
	chmod(file.c_str(), 0666);
}

// The code with the chosen value in place of its '?' digits: the value's
// hexadecimal digits, the lowest at the right-most '?' of each word.
std::string instantiate(const Cheat& cheat)
{
	std::string code = cheat.code;
	size_t end = code.size();
	while (end > 0)
	{
		// One word: from the separator before it to `end`.
		size_t begin = end;
		while (begin > 0 && code[begin - 1] != ' ' && code[begin - 1] != '+')
			begin--;
		uint32_t value = cheat.value;
		for (size_t i = end; i > begin; i--)
			if (code[i - 1] == '?')
			{
				code[i - 1] = "0123456789ABCDEF"[value & 15];
				value >>= 4;
			}
		end = begin > 0 ? begin - 1 : 0;
	}
	return code;
}

} // namespace

void init()
{
	const int cheatFiles = countArchive(cheatsArchive());
	const int patchFiles = countArchive(patchesArchive());
	std::string text;
	if (cheatFiles < 0 && patchFiles < 0)
		text = "not fetched yet (Settings, Games and saving, \"Fetch the newest cheat database\")";
	else
		text = format("cheats for %d games, patches for %d", std::max(cheatFiles, 0), std::max(patchFiles, 0));
	const int libretroFiles = countArchive(appDir + "assets/libretro-cheats.zip");
	if (libretroFiles > 0)
		text += format("; libretro's codes for %d more discs", libretroFiles);
	if (fileExists(rootDir + "data/cheats.zip"))
	{
		struct stat st;
		char day[32] = "";
		if (stat((rootDir + "data/cheats.zip").c_str(), &st) == 0)
		{
			struct tm tm;
			const time_t when = st.st_mtime;
			gmtime_r(&when, &tm);
			strftime(day, sizeof(day), "%Y-%m-%d", &tm);
		}
		text += std::string(", fetched ") + day;
	}
	diag::mark("cheats: database: %s", text.c_str());
	std::lock_guard<std::mutex> lock(summaryMutex);
	summaryText = text;
}

namespace
{
std::mutex refreshMutex;
std::string refreshText;
std::atomic<bool> refreshBusy{false};

void say(const std::string& text)
{
	std::lock_guard<std::mutex> lock(refreshMutex);
	refreshText = text;
}

// An archive of the database as it should be: a ZIP with at least `least`
// files in it.
bool plausible(const std::vector<uint8_t>& data, int least)
{
	mz_zip_archive zip{};
	if (data.size() < 1024 || !mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0))
		return false;
	const int files = (int)mz_zip_reader_get_num_files(&zip);
	mz_zip_reader_end(&zip);
	return files >= least;
}
}

void refresh()
{
	if (refreshBusy.exchange(true))
		return;
	say("Fetching the cheat database\xe2\x80\xa6");
	std::thread([] {
		tuning::setThread(tuning::Role::Background);
		// The chtdb project's release that always holds the newest of both.
		static const char *const base = "https://github.com/duckstation/chtdb/releases/download/latest/";
		std::vector<uint8_t> cheatData, patchData;
		const int a = platform::httpGet(std::string(base) + "cheats.zip", cheatData, 60);
		const int b = a == 200 ? platform::httpGet(std::string(base) + "patches.zip", patchData, 60) : -1;
		if (a != 200 || b != 200)
			say(a < 0 || (a == 200 && b < 0) ? "The database could not be fetched: no answer from GitHub."
					: format("The database could not be fetched: the server answered %d.", a != 200 ? a : b));
		else if (!plausible(cheatData, 1000) || !plausible(patchData, 20))
			say("What came is not the database: nothing was changed.");
		else if (!writeFile(rootDir + "data/cheats.zip", cheatData.data(), cheatData.size())
				|| !writeFile(rootDir + "data/patches.zip", patchData.data(), patchData.size()))
			say("The database could not be saved.");
		else
		{
			{
				std::lock_guard<std::mutex> lock(archiveMutex);
				keptArchives().clear();
			}
			init();
			say("The database is up to date: " + summary() + ".");
		}
		diag::mark("cheats: refresh: %s", refreshStatus().c_str());
		refreshBusy = false;
	}).detach();
}

bool refreshing()
{
	return refreshBusy;
}

std::string refreshStatus()
{
	std::lock_guard<std::mutex> lock(refreshMutex);
	return refreshText;
}

// ------------------------------------------------------------------- mods
//
// A mod is a text file (mod.txt in a folder of its own under mods/, or any
// <name>.mod file there) that names the games it is for by serial and gives
// features that can be switched on and off one by one:
//
//   name = Faster start
//   version = 1.0
//   author = Someone
//   description = What the mod does.
//   games = SLUS-00594, SCES-00344
//
//   [feature]
//   name = Skip the logos
//   description = What this part does.
//   on = yes
//   patch = 80041234: 2a 00 02 24 > 00 00 00 00
//   code = D0041238 1234
//   code = 80041238 0000
//   setting = swanstation_GPU_WidescreenHack = false
//
// A patch writes the new bytes where the old ones are found, and only then:
// code the game loads later (from the disc, into the same place) is patched
// when it arrives, and a different version of the game is left alone. It
// becomes GameShark conditional codes (D0 then 80, a halfword at a time),
// which the emulator's cheat engine applies every frame. code lines are
// GameShark codes as they are; setting lines are emulator options the feature
// needs while it is on. The disc image itself is never changed.
namespace
{
std::string modSummaryText;

std::string trim(const std::string& text)
{
	size_t a = 0, b = text.size();
	while (a < b && isspace((unsigned char)text[a]))
		a++;
	while (b > a && isspace((unsigned char)text[b - 1]))
		b--;
	return text.substr(a, b - a);
}

std::string upper(std::string text)
{
	for (char& c : text)
		c = (char)toupper((unsigned char)c);
	return text;
}

// A serial as the library writes it (SLUS-00594), from "SLUS-00594",
// "SLUS_005.94" or "slus00594".
std::string normalSerial(std::string text)
{
	std::string letters, digits;
	for (char c : upper(text))
		if (isalpha((unsigned char)c) && digits.empty())
			letters += c;
		else if (isdigit((unsigned char)c))
			digits += c;
	return letters.empty() || digits.empty() ? std::string() : letters + "-" + digits;
}

bool hexBytes(const std::string& text, std::vector<uint8_t>& out)
{
	out.clear();
	std::string digits;
	for (char c : text)
		if (isxdigit((unsigned char)c))
			digits += c;
		else if (!isspace((unsigned char)c))
			return false;
	if (digits.empty() || digits.size() % 2 != 0)
		return false;
	for (size_t i = 0; i < digits.size(); i += 2)
		out.push_back((uint8_t)strtoul(digits.substr(i, 2).c_str(), nullptr, 16));
	return true;
}

// "80041234: 2a 00 02 24 > 00 00 00 00" as GameShark codes, or "" with `why`.
std::string patchCodes(const std::string& text, std::string& why)
{
	const size_t colon = text.find(':'), arrow = text.find('>');
	if (colon == std::string::npos || arrow == std::string::npos || arrow < colon)
	{
		why = "a patch is \"address: old bytes > new bytes\"";
		return std::string();
	}
	uint32_t address = (uint32_t)strtoul(trim(text.substr(0, colon)).c_str(), nullptr, 16);
	std::vector<uint8_t> before, after;
	if (!hexBytes(text.substr(colon + 1, arrow - colon - 1), before) || !hexBytes(text.substr(arrow + 1), after))
	{
		why = "a patch's bytes are hexadecimal pairs";
		return std::string();
	}
	if (before.size() != after.size() || before.size() % 2 != 0)
	{
		why = "a patch's old and new bytes are as many, and an even number";
		return std::string();
	}
	const uint32_t region = address & 0xE0000000u;
	if ((region != 0x80000000u && region != 0xA0000000u && region != 0) || (address & 0x1FFFFFFFu) >= 0x200000u ||
			(address & 1) != 0)
	{
		why = "a patch's address is in the PlayStation's 2 MB of memory (80000000 to 801FFFFF), and even";
		return std::string();
	}
	address &= 0x1FFFFFu;
	std::string code;
	char line[64];
	for (size_t i = 0; i < before.size(); i += 2)
	{
		// Halfwords as they are in memory: little endian.
		const unsigned old = before[i] | (unsigned)before[i + 1] << 8, now = after[i] | (unsigned)after[i + 1] << 8;
		if (old == now)
			continue;
		snprintf(line, sizeof(line), "%sD0%06X %04X+80%06X %04X", code.empty() ? "" : "+", address + (unsigned)i, old,
				address + (unsigned)i, now);
		code += line;
	}
	if (code.empty())
		why = "a patch changes nothing";
	return code;
}

struct ModFile
{
	std::string path, name, version, author, description;
	std::vector<std::string> games;
	std::vector<Cheat> features;
	std::vector<std::string> problems;
};

ModFile readMod(const std::string& path, const std::string& text)
{
	ModFile mod;
	mod.path = path;
	Cheat *feature = nullptr;
	std::istringstream in(text);
	std::string raw;
	int number = 0;
	while (std::getline(in, raw))
	{
		number++;
		const std::string line = trim(raw);
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;
		if (line == "[feature]")
		{
			mod.features.emplace_back();
			feature = &mod.features.back();
			feature->patch = true;
			continue;
		}
		const size_t equals = line.find('=');
		if (equals == std::string::npos)
		{
			mod.problems.push_back(format("line %d is not \"name = value\"", number));
			continue;
		}
		const std::string key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
		if (feature == nullptr)
		{
			if (key == "name")
				mod.name = value;
			else if (key == "version")
				mod.version = value;
			else if (key == "author")
				mod.author = value;
			else if (key == "description")
				mod.description += (mod.description.empty() ? "" : " ") + value;
			else if (key == "games" || key == "game")
			{
				std::string piece;
				std::istringstream list(value);
				while (std::getline(list, piece, ','))
				{
					const std::string serial = normalSerial(trim(piece));
					if (!serial.empty())
						mod.games.push_back(serial);
				}
			}
			else
				mod.problems.push_back(format("line %d: \"%s\" is not a mod's own field", number, key.c_str()));
			continue;
		}
		if (key == "name")
			feature->name = value;
		else if (key == "description")
			feature->description += (feature->description.empty() ? "" : " ") + value;
		else if (key == "on")
			feature->enabled = value == "yes" || value == "true" || value == "1" || value == "on";
		else if (key == "patch")
		{
			std::string why;
			const std::string codes = patchCodes(value, why);
			if (codes.empty())
				mod.problems.push_back(format("line %d: %s", number, why.c_str()));
			else
				feature->code += (feature->code.empty() ? "" : "+") + codes;
		}
		else if (key == "code")
		{
			std::vector<uint8_t> bytes;
			std::string compact = value;
			const size_t space = compact.find(' ');
			if (space == std::string::npos || !hexBytes(compact, bytes) || bytes.size() != 6)
				mod.problems.push_back(format("line %d: a code is \"XXXXXXXX YYYY\"", number));
			else
			{
				if (!knownType(bytes[0]))
					feature->supported = false;
				feature->code += (feature->code.empty() ? "" : "+") + upper(value);
			}
		}
		else if (key == "setting")
		{
			const size_t second = value.find('=');
			if (second == std::string::npos)
				mod.problems.push_back(format("line %d: a setting is \"option = value\"", number));
			else
				feature->settings.emplace_back(trim(value.substr(0, second)), trim(value.substr(second + 1)));
		}
		else
			mod.problems.push_back(format("line %d: \"%s\" is not a feature's field", number, key.c_str()));
	}
	if (mod.name.empty())
		mod.problems.push_back("it has no name");
	if (mod.games.empty())
		mod.problems.push_back("it names no game (games = SLUS-00594, ...)");
	const std::string label = mod.version.empty() ? mod.name : mod.name + " " + mod.version;
	std::vector<Cheat> kept;
	for (Cheat& f : mod.features)
	{
		if (f.name.empty())
		{
			mod.problems.push_back("a feature has no name");
			continue;
		}
		if (f.code.empty() && f.settings.empty())
		{
			mod.problems.push_back("feature \"" + f.name + "\" has no patch, code or setting");
			continue;
		}
		f.mod = label;
		f.author = mod.author;
		if (f.description.empty())
			f.description = mod.description;
		kept.push_back(f);
	}
	mod.features = kept;
	return mod;
}

// Every mod file in the mod folders: <folder>/<any>/mod.txt and <folder>/*.mod.
std::vector<ModFile> readAllMods()
{
	std::vector<ModFile> mods;
	for (const std::string& folder : modFolders())
	{
		DIR *dir = opendir(folder.c_str());
		if (dir == nullptr)
			continue;
		std::vector<std::string> files;
		while (const dirent *entry = readdir(dir))
		{
			const std::string name = entry->d_name;
			if (name.empty() || name[0] == '.')
				continue;
			const std::string path = folder + "/" + name;
			if (name.size() > 4 && upper(name.substr(name.size() - 4)) == ".MOD")
				files.push_back(path);
			else if (fileExists(path + "/mod.txt"))
				files.push_back(path + "/mod.txt");
		}
		closedir(dir);
		std::sort(files.begin(), files.end());
		for (const std::string& file : files)
		{
			std::vector<uint8_t> bytes;
			if (!readFile(file, bytes) || bytes.size() > (256u << 10))
				continue;
			mods.push_back(readMod(file, std::string(bytes.begin(), bytes.end())));
		}
	}
	return mods;
}

// The running game's mod features, after its patches.
void addMods(const std::string& gameSerial, std::vector<Cheat>& out)
{
	int packages = 0, problems = 0;
	std::set<std::string> games;
	for (const ModFile& mod : readAllMods())
	{
		packages++;
		problems += (int)mod.problems.size();
		for (const std::string& problem : mod.problems)
			diag::mark("mods: %s: %s", mod.path.c_str(), problem.c_str());
		for (const std::string& game : mod.games)
			games.insert(game);
		if (std::find(mod.games.begin(), mod.games.end(), normalSerial(gameSerial)) == mod.games.end())
			continue;
		for (const Cheat& feature : mod.features)
			out.push_back(feature);
	}
	modSummaryText = packages == 0 ? std::string()
			: format("%d mod%s for %d game%s%s", packages, packages == 1 ? "" : "s", (int)games.size(),
					games.size() == 1 ? "" : "s", problems == 0 ? "" : format(", %d problem%s (boot log, \"mods:\")",
					problems, problems == 1 ? "" : "s").c_str());
}
}

std::vector<std::string> modFolders()
{
	std::vector<std::string> folders;
	folders.push_back(rootDir + "mods");
	for (const std::string& drive : platform::usbDrives())
	{
		const std::string folder = drive + "/" + storage::UsbFolder + "/mods";
		if (std::find(folders.begin(), folders.end(), folder) == folders.end())
			folders.push_back(folder);
	}
	return folders;
}

std::string modsSummary()
{
	if (modSummaryText.empty())
	{
		// Not asked for a game yet: counted now.
		std::vector<Cheat> none;
		addMods("", none);
	}
	return modSummaryText.empty() ? std::string("None found") : modSummaryText;
}

void loadFor(const std::string& gameSerial, const std::string& firstDisc)
{
	unload();
	loadedSerial = gameSerial;
	if (loadedSerial.empty())
		return;
	std::string text;
	std::vector<Cheat> patches, codes;
	if (readFromArchive(patchesArchive(), loadedSerial + ".cht", text))
		parse(text, true, patches);
	if (readFromArchive(cheatsArchive(), loadedSerial + ".cht", text))
		parse(text, false, codes);
	std::vector<uint8_t> own;
	if (readFile(rootDir + "cheats/" + loadedSerial + ".cht", own))
	{
		std::vector<Cheat> mine;
		parse(std::string(own.begin(), own.end()), false, mine);
		for (Cheat& cheat : mine)
		{
			cheat.own = true;
			codes.push_back(std::move(cheat));
		}
	}
	// A game neither has codes for: the libretro database's, matched to the
	// disc by name (tools/make-libretro-cheats.py), each saying so.
	if (codes.empty() && readFromArchive(appDir + "assets/libretro-cheats.zip", loadedSerial + ".cht", text))
		parse(text, false, codes);
	// The cheat archive's files end with the game's patches again (under
	// "PATCHES LISTED BELOW"): an entry both have is kept once, as a patch.
	entries = patches;
	for (Cheat& cheat : codes)
	{
		const bool duplicate = std::any_of(patches.begin(), patches.end(), [&](const Cheat& patch) {
			return patch.name == cheat.name && patch.group == cheat.group && patch.code == cheat.code;
		});
		if (!duplicate)
			entries.push_back(std::move(cheat));
	}
	// The mods for this game, after the database's.
	const size_t beforeMods = entries.size();
	addMods(loadedSerial, entries);
	const int mods = (int)(entries.size() - beforeMods);
	loadState();
	int on = 0;
	for (const Cheat& cheat : entries)
		on += cheat.enabled;
	if (mods > 0)
		diag::mark("mods: %s: %d feature(s) from the mod folders", loadedSerial.c_str(), mods);
	diag::mark("cheats: %s: %d entries (%d patches), %d on", loadedSerial.c_str(), (int)entries.size(), (int)patches.size(), on);
	if (entries.empty() && !firstDisc.empty() && firstDisc != gameSerial)
		loadFor(firstDisc);
}

const std::string& serial()
{
	return loadedSerial;
}

void unload()
{
	entries.clear();
	loadedSerial.clear();
	options::clearOverrides();
}

std::vector<Cheat>& list()
{
	return entries;
}

void apply()
{
	// From a game's details, before it runs, the choice is only kept.
	if (!host::running())
		return;
	retro_cheat_reset();
	// In netplay the settings that are held are the session's, and a cheat on
	// one console would part the two: none run, and nothing held is let go.
	if (netplay::active())
		return;
	options::clearOverrides();
	// RetroAchievements' hardcore mode allows none.
	if (host::restricted())
		return;
	unsigned index = 0;
	const int automatic = automaticWidescreen();
	for (size_t i = 0; i < entries.size(); i++)
	{
		const Cheat& cheat = entries[i];
		if ((!cheat.enabled && (int)i != automatic) || !cheat.supported)
			continue;
		for (const auto& [key, value] : cheat.settings)
			options::setOverride(key, value);
		if (cheat.manual)
			continue;
		retro_cheat_set(index++, true, instantiate(cheat).c_str());
	}
	static std::string said;
	const std::string now = automatic >= 0 ? loadedSerial + ": " + entries[(size_t)automatic].name : std::string();
	if (now != said && !now.empty())
		diag::mark("cheats: %s, used by itself for the wide picture", now.c_str());
	said = now;
}

void followWidescreen()
{
	static int applied = -2;
	static std::string appliedFor;
	if (!host::running())
	{
		applied = -2;
		return;
	}
	const int now = automaticWidescreen();
	if (now != applied || appliedFor != loadedSerial)
	{
		const bool first = applied == -2;
		applied = now;
		appliedFor = loadedSerial;
		if (!first)
			apply();
	}
}

int automaticWidescreen()
{
	const options::Frontend& f = options::frontend();
	if (!f.widescreenPatch || entries.empty())
		return -1;
	// What the player asked for: a wide aspect ratio, or the widescreen hack.
	const char *aspect = options::own("swanstation_Display_AspectRatio");
	const char *hack = options::own("swanstation_GPU_WidescreenHack");
	const std::string ratio = aspect != nullptr ? aspect : "";
	static const char *const wide[] = { "16:9", "16:10", "19:9", "20:9", "21:9", "32:9" };
	std::string wanted;
	for (const char *w : wide)
		if (ratio == w)
			wanted = w;
	if (wanted.empty() && hack != nullptr && strcmp(hack, "true") == 0)
		wanted = "16:9";
	if (wanted.empty())
		return -1;
	int best = -1, bestScore = 0;
	for (size_t i = 0; i < entries.size(); i++)
	{
		const Cheat& c = entries[i];
		const std::string name = upper(c.name + " " + c.group);
		if (!c.patch || !c.mod.empty() || !c.supported || c.manual || name.find("WIDESCREEN") == std::string::npos)
			continue;
		// One the player switched on themselves is theirs: none is added.
		if (c.enabled)
			return -1;
		if (name.find("CRASH") != std::string::npos || name.find("MENU") != std::string::npos ||
				name.find("VERT-") != std::string::npos)
			continue;
		const int score = name.find(wanted) != std::string::npos ? 3 : name.find("16:9") != std::string::npos ? 2 : 1;
		if (score > bestScore)
		{
			best = (int)i;
			bestScore = score;
		}
	}
	return best;
}

void setEnabled(size_t index, bool enabled)
{
	if (index >= entries.size() || !entries[index].supported)
		return;
	if (enabled && !entries[index].group.empty())
		for (Cheat& other : entries)
			if (other.group == entries[index].group && other.patch == entries[index].patch)
				other.enabled = false;
	entries[index].enabled = enabled;
	saveState();
	apply();
}

void setValue(size_t index, uint32_t value)
{
	if (index >= entries.size())
		return;
	entries[index].value = value;
	saveState();
	if (entries[index].enabled)
		apply();
}

void runOnce(size_t index)
{
	if (index >= entries.size() || !entries[index].supported || !host::running())
		return;
	CheatCode code;
	code.description = entries[index].name;
	code.enabled = true;
	if (CheatList::ParseLibretroCheat(&code, instantiate(entries[index]).c_str()))
		code.Apply();
}

std::string summary()
{
	std::lock_guard<std::mutex> lock(summaryMutex);
	return summaryText;
}

// ------------------------------------------------------- codes typed in

namespace
{
std::string ownFile()
{
	return rootDir + "cheats/" + loadedSerial + ".cht";
}

// A section's name as the format has it: no brackets, no backslash (that
// would make a group), no line breaks.
std::string sectionName(const std::string& name)
{
	std::string out;
	for (char c : name)
		if (c != '[' && c != ']' && c != '\\' && c != '\n' && c != '\r')
			out += c;
	return trim(out);
}

// The file's text with the section `name` taken out ("" when it is not there).
std::string withoutSection(const std::string& text, const std::string& name, bool& found)
{
	std::istringstream in(text);
	std::string line, out;
	bool skipping = false;
	found = false;
	while (std::getline(in, line))
	{
		const std::string t = trim(line);
		if (!t.empty() && t.front() == '[' && t.back() == ']')
		{
			skipping = t.substr(1, t.size() - 2) == name;
			found = found || skipping;
		}
		if (!skipping)
			out += line + "\n";
	}
	return out;
}
}

std::string codeLines(const Cheat& cheat)
{
	std::string out;
	std::string code = cheat.code;
	size_t start = 0;
	while (start <= code.size())
	{
		size_t end = code.find('+', start);
		if (end == std::string::npos)
			end = code.size();
		std::string part = trim(code.substr(start, end - start));
		if (!part.empty())
			out += (out.empty() ? "" : "\n") + part;
		start = end + 1;
	}
	return out;
}

bool addOwn(const std::string& rawName, const std::string& lines, std::string& why)
{
	if (loadedSerial.empty())
	{
		why = "The game's serial number is not known, and a game's own codes are kept under it.";
		return false;
	}
	const std::string name = sectionName(rawName);
	if (name.empty())
	{
		why = "The code needs a name.";
		return false;
	}
	// The hexadecimal digits, twelve a code: eight of address, four of value.
	std::vector<std::string> codes;
	std::string digits;
	for (char c : lines)
	{
		if (isxdigit((unsigned char)c))
			digits += (char)toupper((unsigned char)c);
		else if (c == '+' || c == '\n')
		{
			if (!digits.empty() && digits.size() != 12)
			{
				why = "Each code has 12 digits (8, a space, 4): \"" + digits + "\" has " + std::to_string(digits.size()) + ".";
				return false;
			}
			if (!digits.empty())
				codes.push_back(digits.substr(0, 8) + " " + digits.substr(8));
			digits.clear();
		}
		else if (c != ' ')
		{
			why = std::string("A GameShark code has only the digits 0 to 9 and the letters A to F, not '") + c + "'.";
			return false;
		}
		if (digits.size() == 12)
		{
			codes.push_back(digits.substr(0, 8) + " " + digits.substr(8));
			digits.clear();
		}
	}
	if (!digits.empty())
	{
		why = "The last code has " + std::to_string(digits.size()) + " digits; each has 12 (8, a space, 4).";
		return false;
	}
	if (codes.empty())
	{
		why = "No code was typed.";
		return false;
	}
	// The emulator must know each line's type.
	bool sawData = false, sawUnknown = false;
	for (const std::string& code : codes)
	{
		const unsigned type = (unsigned)strtoul(code.substr(0, 2).c_str(), nullptr, 16);
		if (!knownType(type))
			sawUnknown = true;
		if (hasDataLines(type))
			sawData = true;
	}
	if (sawUnknown && !sawData)
	{
		why = "A code type this emulator does not run (the first two digits of a line). Check the code against where it "
				"was found.";
		return false;
	}

	std::vector<uint8_t> old;
	std::string text = readFile(ownFile(), old) ? std::string(old.begin(), old.end()) : std::string();
	bool replaced = false;
	text = withoutSection(text, name, replaced);
	if (!text.empty() && text.back() != '\n')
		text += "\n";
	if (!text.empty())
		text += "\n";
	text += "[" + name + "]\nType = Gameshark\nActivation = EndFrame\nAuthor = Typed in PSSwanStation\n";
	for (const std::string& code : codes)
		text += code + "\n";
	makeDir(rootDir + "cheats");
	if (!writeFile(ownFile(), text.data(), text.size()))
	{
		why = "The file " + ownFile() + " could not be written.";
		return false;
	}
	diag::mark("cheats: %s: %s \"%s\" (%d line(s)) in cheats/%s.cht", loadedSerial.c_str(), replaced ? "replaced" : "added",
			name.c_str(), (int)codes.size(), loadedSerial.c_str());
	// Listed again, with it switched on.
	const std::string serialNow = loadedSerial;
	loadFor(serialNow);
	for (size_t i = 0; i < entries.size(); i++)
		if (entries[i].own && entries[i].group.empty() && entries[i].name == name)
			setEnabled(i, true);
	return true;
}

bool removeOwn(size_t index)
{
	if (index >= entries.size() || !entries[index].own)
		return false;
	const std::string name = entries[index].group.empty() ? entries[index].name
			: entries[index].group + "\\" + entries[index].name;
	std::vector<uint8_t> old;
	if (!readFile(ownFile(), old))
		return false;
	bool found = false;
	const std::string text = withoutSection(std::string(old.begin(), old.end()), name, found);
	if (!found || !writeFile(ownFile(), text.data(), text.size()))
		return false;
	setEnabled(index, false);
	const std::string serialNow = loadedSerial;
	loadFor(serialNow);
	apply();
	return true;
}

}
