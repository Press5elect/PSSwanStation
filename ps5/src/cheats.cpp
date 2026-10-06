/*
	PSSwanStation - cheats and patches, from DuckStation's database.

	SPDX-License-Identifier: GPL-3.0-or-later

	The codes are those of github.com/duckstation/chtdb, the cheat and patch
	collection DuckStation ships ("can also be used by other emulators that
	support GameShark codes", its README says): two archives in the title's
	folder, assets/cheats.zip and assets/patches.zip, each holding one
	<serial>.cht per game. A file of the user's own, <root>cheats/<serial>.cht
	in the same format, is read as well.

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
		any = any || cheat.enabled || ((!cheat.choices.empty() || cheat.hasRange) && cheat.value != 0);
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
		if (cheat.enabled || !cheat.choices.empty() || cheat.hasRange)
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
		text = "not installed (assets/cheats.zip, assets/patches.zip)";
	else
		text = format("cheats for %d games, patches for %d", std::max(cheatFiles, 0), std::max(patchFiles, 0));
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
		parse(std::string(own.begin(), own.end()), false, codes);
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
	loadState();
	int on = 0;
	for (const Cheat& cheat : entries)
		on += cheat.enabled;
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
	for (const Cheat& cheat : entries)
	{
		if (!cheat.enabled || !cheat.supported)
			continue;
		for (const auto& [key, value] : cheat.settings)
			options::setOverride(key, value);
		if (cheat.manual)
			continue;
		retro_cheat_set(index++, true, instantiate(cheat).c_str());
	}
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

}
