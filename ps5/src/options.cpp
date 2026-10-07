/*
	PSSwanStation - settings: the frontend's own, and every option of the core.

	SPDX-License-Identifier: GPL-3.0-or-later

	The core describes its settings through libretro's core options (version 2:
	113 options in five categories). The frontend keeps the definitions as the
	core gave them and shows every one of them in the Settings, so nothing the
	emulator can be told is left out, and a new option of a later core appears
	by itself.

	Values are text, as libretro has them: <root>data/options.cfg for every
	game, and <root>data/game-options/<serial>.cfg for the ones a game has of
	its own, laid over the first while that game runs.

	A few lists are narrowed for the console, and a few defaults differ from
	the core's; both are in `adapt` below, with the reason for each.
*/
#include "fe.h"

#include <libretro.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sys/stat.h>

namespace fe::options
{
namespace
{
Frontend current;
std::vector<Option> definitions;
std::vector<Category> cats;
// Values live in map nodes, whose addresses do not move: the core keeps the
// pointers RETRO_ENVIRONMENT_GET_VARIABLE hands it.
std::map<std::string, std::string> globalValues, gameValues, overrides;
std::string serial;
bool changed;
unsigned changes;
std::mutex mutex;

std::string globalFile()
{
	return rootDir + "data/options.cfg";
}

std::string gameFile(const std::string& id)
{
	return rootDir + "data/game-options/" + id + ".cfg";
}

void readValues(const std::string& path, std::map<std::string, std::string>& values)
{
	values.clear();
	FILE *f = fopen(path.c_str(), "r");
	if (f == nullptr)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		const char *equals = strchr(line, '=');
		if (equals == nullptr || line[0] == '#')
			continue;
		const std::string key = trim(std::string(line, equals - line));
		std::string value = trim(equals + 1);
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
			value = value.substr(1, value.size() - 2);
		if (!key.empty())
			values[key] = value;
	}
	fclose(f);
}

void writeValues(const std::string& path, const std::map<std::string, std::string>& values)
{
	if (values.empty())
	{
		remove(path.c_str());
		return;
	}
	FILE *f = fopen(path.c_str(), "w");
	if (f == nullptr)
		return;
	for (const auto& [key, value] : values)
		fprintf(f, "%s = \"%s\"\n", key.c_str(), value.c_str());
	fclose(f);
	chmod(path.c_str(), 0666);
}

void keepOnly(Option& option, std::initializer_list<const char *> wanted)
{
	std::vector<Value> kept;
	for (const Value& value : option.values)
		for (const char *name : wanted)
			if (value.value == name)
				kept.push_back(value);
	if (!kept.empty())
		option.values = kept;
}

void drop(Option& option, const char *unwanted)
{
	option.values.erase(std::remove_if(option.values.begin(), option.values.end(),
			[unwanted](const Value& value) { return value.value == unwanted; }), option.values.end());
}

// What differs from the core's own definitions in this title.
void adapt(Option& option)
{
	const std::string& key = option.key;
	if (key == "swanstation_GPU_Renderer")
	{
		// The title has one graphics API, Vulkan (RADV), and the software
		// renderer.
		keepOnly(option, { "Vulkan", "Software" });
		option.defaultValue = "Vulkan";
	}
	else if (key == "swanstation_GPU_ResolutionScale")
	{
		// 8x: 2560 x 1920 for a 320 x 240 game, the closest to the display's
		// 2160 lines below it. Not measured on a console yet (README).
		option.defaultValue = "8";
	}
	else if (key == "swanstation_MemoryCards_Card1Type")
	{
		// "Libretro" hands the card to a frontend as save RAM; here the
		// emulator keeps its cards itself, one per game title.
		drop(option, "Libretro");
		option.defaultValue = "PerGameTitle";
	}
	else if (key == "swanstation_CPU_FastmemMode")
	{
		// The mmap fastmem is not built for the console (16 KiB pages). The
		// lookup-table one reaches an unmapped PlayStation address through the
		// same address of the title itself and counts on that faulting, which
		// holds on a PC, where nothing is mapped below 4 GiB; whether it holds
		// on the console is not established, so it is there to try, and off
		// by default.
		drop(option, "MMap");
		drop(option, "true");
		drop(option, "false");
		option.defaultValue = "Disabled";
		option.info += " On the console this is experimental: leave it off unless a game needs the speed.";
	}
	else if (key == "swanstation_CPU_ExecutionMode")
	{
		// The recompiler needs memory it can execute.
		if (!platform::jitAvailable())
		{
			drop(option, "Recompiler");
			option.defaultValue = "CachedInterpreter";
			option.info += " The recompiler is not offered: this console refused executable memory.";
		}
	}
	else if (key == "swanstation_Main_RunaheadFrameCount" || key == "swanstation_Audio_FastHook")
	{
		// Both are for a libretro frontend's own audio and run-ahead handling.
	}
	if (std::none_of(option.values.begin(), option.values.end(),
			[&](const Value& value) { return value.value == option.defaultValue; }) && !option.values.empty())
		option.defaultValue = option.values.front().value;
}

void addDefinitions(const retro_core_options_v2 *v2)
{
	if (v2 == nullptr)
		return;
	definitions.clear();
	cats.clear();
	if (v2->categories != nullptr)
		for (const retro_core_option_v2_category *c = v2->categories; c->key != nullptr; c++)
			cats.push_back({ c->key, c->desc != nullptr ? c->desc : c->key, c->info != nullptr ? c->info : "" });
	for (const retro_core_option_v2_definition *d = v2->definitions; d != nullptr && d->key != nullptr; d++)
	{
		Option option;
		option.key = d->key;
		option.name = d->desc_categorized != nullptr ? d->desc_categorized : (d->desc != nullptr ? d->desc : d->key);
		const char *info = d->info_categorized != nullptr ? d->info_categorized : d->info;
		option.info = info != nullptr ? info : "";
		option.category = d->category_key != nullptr ? d->category_key : "";
		for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value != nullptr; i++)
			option.values.push_back({ d->values[i].value,
					d->values[i].label != nullptr ? d->values[i].label : d->values[i].value });
		option.defaultValue = d->default_value != nullptr ? d->default_value : "";
		adapt(option);
		definitions.push_back(std::move(option));
	}
	diag::mark("options: %d options in %d categories", (int)definitions.size(), (int)cats.size());
}

} // namespace

Frontend& frontend()
{
	return current;
}

namespace
{
// The frontend's settings by their names in frontend.cfg, each a whole
// number (a switch is 0 or 1) but the dead zone.
struct Field
{
	const char *name;
	int *number;
	bool *flag;
	int low, high;
};

std::vector<Field> fields()
{
	Frontend& c = current;
	std::vector<Field> list = {
		{ "view", &c.view, nullptr, 0, 61 },
		{ "source", &c.source, nullptr, 0, 2 },
		{ "covers", nullptr, &c.covers, 0, 1 },
		{ "usb", nullptr, &c.usb, 0, 1 },
		{ "ram_cache", nullptr, &c.ramCache, 0, 1 },
		{ "notifications", nullptr, &c.notifications, 0, 1 },
		{ "scaling", &c.scaling, nullptr, 0, 3 },
		{ "fsr_sharpness", &c.fsrSharpness, nullptr, 0, 2 },
		{ "linear_filter", nullptr, &c.linearFilter, 0, 1 },
		{ "volume", &c.volume, nullptr, 0, 100 },
		{ "show_fps", nullptr, &c.showFps, 0, 1 },
		{ "pacing", &c.pacing, nullptr, 0, 2 },
		{ "display_mode", &c.displayMode, nullptr, 0, 2 },
		{ "black_frames", nullptr, &c.blackFrames, 0, 1 },
		{ "frame_generation", &c.frameGeneration, nullptr, 0, 2 },
		{ "crt", &c.crt, nullptr, 0, 3 },
		{ "border", &c.border, nullptr, 0, 3 },
		{ "preset", &c.preset, nullptr, 0, 3 },
		{ "auto_save", nullptr, &c.autoSaveOnExit, 0, 1 },
		{ "auto_load", nullptr, &c.autoLoadOnStart, 0, 1 },
		{ "controller1", &c.controller[0], nullptr, 0, 5 },
		{ "controller2", &c.controller[1], nullptr, 0, 5 },
		{ "controller3", &c.controller[2], nullptr, 0, 5 },
		{ "controller4", &c.controller[3], nullptr, 0, 5 },
		{ "rumble", nullptr, &c.rumble, 0, 1 },
		{ "player_lights", nullptr, &c.playerLights, 0, 1 },
		{ "turbo_rate", &c.turboRate, nullptr, 0, 2 },
		{ "hotkeys", nullptr, &c.hotkeys, 0, 1 },
		{ "fast_forward", &c.fastForward, nullptr, 0, 4 },
		{ "rewind", nullptr, &c.rewind, 0, 1 },
		{ "rewind_detail", &c.rewindDetail, nullptr, 0, 2 },
		{ "rewind_memory", &c.rewindMemory, nullptr, 0, 3 },
		{ "motion", &c.motion, nullptr, 0, 2 },
		{ "motion_range", &c.motionRange, nullptr, 0, 4 },
		{ "motion_invert", nullptr, &c.motionInvert, 0, 1 },
		{ "swap_confirm", nullptr, &c.swapConfirm, 0, 1 },
		{ "splash", nullptr, &c.splash, 0, 1 },
		{ "splash_sound", nullptr, &c.splashSound, 0, 1 },
		{ "ui_sounds", nullptr, &c.uiSounds, 0, 1 },
		{ "music", &c.music, nullptr, 0, 2 },
		{ "music_volume", &c.musicVolume, nullptr, 0, 100 },
		{ "animations", &c.animations, nullptr, 0, 2 },
		{ "ui_scale", &c.uiScale, nullptr, 75, 150 },
		{ "accent", &c.accent, nullptr, 0, 7 },
		{ "sort", &c.sort, nullptr, 0, 4 },
		{ "filter", &c.filter, nullptr, 0, 3 },
		{ "region_filter", &c.regionFilter, nullptr, 0, 3 },
		{ "idle_minutes", &c.idleMinutes, nullptr, 0, 60 },
		{ "clock", nullptr, &c.clock, 0, 1 },
		{ "outside", nullptr, &c.outside, 0, 1 },
		{ "card_backups", &c.cardBackups, nullptr, 0, 50 },
		{ "update_check", nullptr, &c.updateCheck, 0, 1 },
		{ "achievements", nullptr, &c.achievements, 0, 1 },
		{ "hardcore", nullptr, &c.hardcore, 0, 1 },
		{ "unofficial", nullptr, &c.unofficial, 0, 1 },
		{ "netplay_delay", &c.netplayDelay, nullptr, 1, 10 },
	};
	static const char *const mapNames[16] = { "map_cross", "map_square", "map_select", "map_start", "map_up", "map_down",
			"map_left", "map_right", "map_circle", "map_triangle", "map_l1", "map_r1", "map_l2", "map_r2", "map_l3",
			"map_r3" };
	for (int i = 0; i < 16; i++)
		list.push_back({ mapNames[i], &c.remap[i], nullptr, -1, 15 });
	return list;
}
}

// frontend.cfg is the title folder's, wherever the user's other files are:
// it is read before the sandbox is left, to know whether to leave it.
void loadFrontend()
{
	FILE *f = fopen((appDir + "frontend.cfg").c_str(), "r");
	if (f == nullptr)
		return;
	const std::vector<Field> known = fields();
	char key[64];
	float value;
	while (fscanf(f, " %63[^= ] = %f", key, &value) == 2)
	{
		const int i = (int)value;
		if (!strcmp(key, "dead_zone"))
			current.deadZone = std::clamp(value, 0.f, 0.5f);
		else if (!strcmp(key, "turbo"))
			current.turbo = (unsigned)i & 0xffffu;
		// What "Follow the display" was before the pacing had three kinds.
		else if (!strcmp(key, "sync_to_display"))
			current.pacing = i != 0 ? 0 : 1;
		else
			for (const Field& field : known)
				if (!strcmp(key, field.name))
				{
					if (field.flag != nullptr)
						*field.flag = i != 0;
					else
						*field.number = std::clamp(i, field.low, field.high);
					break;
				}
	}
	fclose(f);
}

void saveFrontend()
{
	std::string text;
	for (const Field& field : fields())
		text += format("%s = %d\n", field.name, field.flag != nullptr ? (int)*field.flag : *field.number);
	text += format("dead_zone = %.2f\nturbo = %u\n", current.deadZone, current.turbo);
	writeFile(appDir + "frontend.cfg", text.data(), text.size());
}

void define(const void *optionsV2)
{
	std::lock_guard<std::mutex> lock(mutex);
	// Once: the core hands the same definitions over each time it is
	// initialised (between games), and what was shown or hidden stays.
	if (!definitions.empty())
		return;
	addDefinitions(static_cast<const retro_core_options_v2 *>(optionsV2));
}

const std::vector<Category>& categories()
{
	return cats;
}

std::vector<Option>& all()
{
	return definitions;
}

Option *find(const std::string& key)
{
	for (Option& option : definitions)
		if (option.key == key)
			return &option;
	return nullptr;
}

const char *get(const std::string& key)
{
	std::lock_guard<std::mutex> lock(mutex);
	const Option *option = nullptr;
	for (const Option& candidate : definitions)
		if (candidate.key == key)
		{
			option = &candidate;
			break;
		}
	if (option == nullptr)
		return nullptr;
	// A stored value the option no longer has (an older build's, or a list
	// narrowed here) is passed over.
	const auto valid = [option](const std::string& value) {
		return std::any_of(option->values.begin(), option->values.end(),
				[&](const Value& v) { return v.value == value; });
	};
	if (const auto it = overrides.find(key); it != overrides.end() && valid(it->second))
		return it->second.c_str();
	if (const auto it = gameValues.find(key); it != gameValues.end() && valid(it->second))
		return it->second.c_str();
	if (const auto it = globalValues.find(key); it != globalValues.end() && valid(it->second))
		return it->second.c_str();
	return option->defaultValue.c_str();
}

std::string label(const Option& option, const std::string& value)
{
	for (const Value& v : option.values)
		if (v.value == value)
			return v.label;
	return value;
}

void set(const std::string& key, const std::string& value, bool forGame)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (forGame && !serial.empty())
	{
		gameValues[key] = value;
		writeValues(gameFile(serial), gameValues);
	}
	else
	{
		globalValues[key] = value;
		writeValues(globalFile(), globalValues);
	}
	changed = true;
	changes++;
}

void clearGameValue(const std::string& key)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (gameValues.erase(key) != 0 && !serial.empty())
	{
		writeValues(gameFile(serial), gameValues);
		changed = true;
	changes++;
	}
}

bool hasGameValue(const std::string& key)
{
	std::lock_guard<std::mutex> lock(mutex);
	return gameValues.count(key) != 0;
}

void loadGlobal()
{
	std::lock_guard<std::mutex> lock(mutex);
	readValues(globalFile(), globalValues);
	changed = true;
	changes++;
}

void loadGame(const std::string& id)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (id == serial)
		return;
	serial = id;
	if (id.empty())
		gameValues.clear();
	else
		readValues(gameFile(id), gameValues);
	changed = true;
	changes++;
}

const std::string& gameSerial()
{
	return serial;
}

bool takeChanged()
{
	std::lock_guard<std::mutex> lock(mutex);
	const bool was = changed;
	changed = false;
	return was;
}

unsigned generation()
{
	return changes;
}

void setOverride(const std::string& key, const std::string& value)
{
	std::lock_guard<std::mutex> lock(mutex);
	const auto it = overrides.find(key);
	if (it != overrides.end() && it->second == value)
		return;
	overrides[key] = value;
	changed = true;
	changes++;
}

void clearOverrides()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (overrides.empty())
		return;
	overrides.clear();
	changed = true;
	changes++;
}

bool hasOverride(const std::string& key)
{
	std::lock_guard<std::mutex> lock(mutex);
	return overrides.count(key) != 0;
}

void resetGlobal()
{
	std::lock_guard<std::mutex> lock(mutex);
	globalValues.clear();
	writeValues(globalFile(), globalValues);
	changed = true;
	changes++;
}

void resetFrontend()
{
	current = Frontend();
	saveFrontend();
}

void setVisible(const std::string& key, bool visible)
{
	for (Option& option : definitions)
		if (option.key == key)
			option.visible = visible;
}

}
