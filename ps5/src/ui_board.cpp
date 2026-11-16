/*
	PSSwanStation - the settings board: every setting of the title on one
	board of areas, each area's sections in the order the game's picture,
	sound or a press travels.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The rows themselves are made where they always were (ui.cpp's
	frontendItems and optionItems, ui_more.cpp's pictureItems and
	moreSettings), so each keeps what it does. This file gathers them, gives
	each its place (an area and a section, ui_board_table.inc) and, for the
	title's own rows, the name and description in SwanStation's manner;
	SwanStation's options keep their own names and descriptions word for word.

	The board shows every area as a tile with a light for each of its
	settings, lit when the setting is not at its default. An area lists its
	sections in order (L2/R2 between them) beside what the setting under the
	cursor does, its default, when it applies and whose it is. Changes made
	here are listed and can be undone (L3); R3 pins a setting to the quick
	settings, which the menu over a game shows; Triangle searches every
	setting by its name and description.
*/
#include "ui_internal.h"
#include "ui_board.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace fe::ui
{
void frontendItems(int kind, std::vector<Item>& items);
void optionItems(const std::string& category, bool uncategorised, bool forGame, std::vector<Item>& items);
void aboutItems(std::vector<Item>& items);
void pictureItems(bool forGame, std::vector<Item>& items);

using namespace platform;

namespace board
{
namespace
{
enum AreaId { Console, Graphics, Display, Audio, Controllers, Library, Saving, Interface, Network, System };

struct Place
{
	const char *match;
	int area;
	const char *section, *name, *desc;
	int def;
};

const Place places[] = {
#include "ui_board_table.inc"
};

const std::vector<AreaDef>& areaList()
{
	static const std::vector<AreaDef> list = {
		{ "Console", icon::Chip, { "Disc", "BIOS", "CPU", "Memory Cards" } },
		{ "Graphics", icon::Paint, { "Renderer", "Geometry", "Textures", "Colour" } },
		{ "Display", icon::Screen, { "Preset", "Frame", "Scaling", "Filters", "Adjustments", "Frame Generation",
				"Output" } },
		{ "Audio", icon::Volume, { "Mix", "Output", "Interface Sounds" } },
		{ "Controllers", icon::Gamepad, { "Ports", "Controller 1", "Controller 2", "Controller 3", "Controller 4",
				"Controller 5", "Controller 6", "Controller 7", "Controller 8", "Pad", "Hotkeys", "Lightgun", "USB" } },
		{ "Library", icon::Grid, { "Sources", "View", "Media" } },
		{ "Saving", icon::Save, { "Save States", "Rewind", "Storage" } },
		{ "Interface", icon::Sliders, { "Theme", "Accessibility", "Mascot", "Startup" } },
		{ "Network", icon::Network, { "RetroAchievements", "Remote", "Updates" } },
		{ "System", icon::Wrench, { "Logging", "Performance", "Input Recording", "About", "Licences", "Other" } },
	};
	return list;
}

int sectionIndex(int area, const std::string& name)
{
	const std::vector<std::string>& sections = areaList()[(size_t)area].sections;
	for (size_t i = 0; i < sections.size(); i++)
		if (sections[i] == name)
			return (int)i;
	return (int)sections.size() - 1;
}

bool matches(const Place& place, const Item& item)
{
	const size_t n = std::strlen(place.match);
	if (n > 0 && place.match[n - 1] == '*')
		return !item.key.empty() && item.key.compare(0, n - 1, place.match, n - 1) == 0;
	if (!item.key.empty())
		return item.key == place.match;
	return item.label == place.match;
}

std::string expand(const char *text)
{
	std::string out = text;
	for (size_t at; (at = out.find("{root}")) != std::string::npos;)
		out.replace(at, 6, shownRoot());
	return out;
}

std::string lower(std::string s)
{
	for (char& c : s)
		c = (char)std::tolower((unsigned char)c);
	return s;
}

// ------------------------------------------------------------------ changes and pins

struct Change
{
	std::string id, name, from, to;
	int fromIndex;
	bool forGame;
};
std::vector<Change> trail;

std::vector<std::string> pins;
bool pinsRead;

std::string pinsFile()
{
	return appDir + "quick-settings.txt";
}

void readPins()
{
	if (pinsRead)
		return;
	pinsRead = true;
	std::vector<uint8_t> data;
	if (!readFile(pinsFile(), data))
	{
		// At first: what is changed most while a game runs.
		pins = { "picture:preset", "swanstation_GPU_ResolutionScale", "picture:crt", "Fast Forward Speed",
			"Vibration Strength" };
		return;
	}
	std::string line;
	for (uint8_t c : data)
	{
		if (c == '\n')
		{
			if (!line.empty())
				pins.push_back(line);
			line.clear();
		}
		else if (c != '\r')
			line += (char)c;
	}
	if (!line.empty())
		pins.push_back(line);
}

void writePins()
{
	std::string text;
	for (const std::string& pin : pins)
		text += pin + "\n";
	writeFile(pinsFile(), text.data(), text.size());
}

bool pinned(const std::string& id)
{
	readPins();
	return std::find(pins.begin(), pins.end(), id) != pins.end();
}

void togglePin(const std::string& id)
{
	readPins();
	const auto at = std::find(pins.begin(), pins.end(), id);
	if (at != pins.end())
		pins.erase(at);
	else
		pins.push_back(id);
	writePins();
}

// Records a change made on the board before it is made, to be undone.
void wrapForTrail(Row& row, bool forGame)
{
	if (!row.item.choose || row.item.current < 0)
		return;
	const std::function<void(int)> choose = row.item.choose;
	const int from = row.item.current;
	const std::string id = row.id, name = row.item.label;
	const std::vector<std::string> choices = row.item.choices;
	row.item.choose = [choose, from, id, name, choices, forGame](int index) {
		if (index != from && index >= 0 && index < (int)choices.size())
		{
			trail.push_back({ id, name, choices[(size_t)from], choices[(size_t)index], from, forGame });
			if (trail.size() > 50)
				trail.erase(trail.begin());
		}
		choose(index);
	};
}

void undoLast(bool forGame)
{
	for (size_t i = trail.size(); i-- > 0;)
	{
		if (trail[i].forGame != forGame)
			continue;
		const Change change = trail[i];
		trail.erase(trail.begin() + (long)i);
		for (Row& row : rows(forGame))
			if (row.id == change.id && row.item.choose)
			{
				row.item.choose(change.fromIndex);
				cue(sound::Toggle);
				break;
			}
		return;
	}
}

// The quick settings' rows for the menu over a game, from the pins.
std::vector<Row> pinnedRows()
{
	readPins();
	std::vector<Row> out;
	std::vector<Row> all = rows(false);
	for (const std::string& pin : pins)
		for (Row& row : all)
			if (row.id == pin)
			{
				out.push_back(row);
				break;
			}
	return out;
}

std::string sectionsLine(int area, const std::vector<Row>& all)
{
	std::string line;
	const std::vector<std::string>& sections = areaList()[(size_t)area].sections;
	for (size_t s = 0; s < sections.size(); s++)
	{
		bool used = false;
		for (const Row& row : all)
			if (row.area == area && row.section == (int)s)
				used = true;
		if (!used)
			continue;
		line += (line.empty() ? "" : "  \xc2\xb7  ") + sections[s];
	}
	return line;
}

// The rows of an area as a list, under their sections' headers.
std::vector<Item> areaItems(std::vector<Row>& all, int area, bool forGame, std::vector<const Row *> *order)
{
	std::vector<Item> items;
	int section = -1;
	for (Row& row : all)
	{
		if (row.area != area)
			continue;
		if (row.section != section)
		{
			section = row.section;
			items.push_back(header(areaList()[(size_t)area].sections[(size_t)section]));
			if (order != nullptr)
				order->push_back(nullptr);
		}
		wrapForTrail(row, forGame);
		Item item = row.item;
		if (pinned(row.id))
			item.label += std::string("  ") + icon::Star;
		items.push_back(std::move(item));
		if (order != nullptr)
			order->push_back(&row);
	}
	return items;
}

// What the setting under the cursor is: its description, current value,
// default, when it applies and whose it is.
void infoPanel(const Row *row, float x0, float y0, float x1, float y1, bool forGame)
{
	const Theme& t = theme();
	panel(at(x0, y0), at(x1, y1), withAlpha(t.panel, 0.86f), 16);
	if (row == nullptr)
		return;
	const Item& item = row->item;
	const float wrap = x1 - x0 - 56;
	float y = y0 + 26;
	y += toUnits(textWrapped(at(x0 + 28, y), px(wrap), t.text, item.label, Bold, 28)) + 12;
	if (item.mark == 1)
	{
		text(at(x0 + 28, y), t.accent, "This game's own value", Bold, 20);
		y += 34;
	}
	const float metaH = 150;
	const float infoMax = std::max(y1 - metaH - y - 20, 60.f);
	y += toUnits(textWrapped(at(x0 + 28, y), px(wrap), t.dim, item.info, Body, 23, px(infoMax))) + 18;
	float my = std::max(y, y1 - metaH);
	const auto meta = [&](const char *label, const std::string& value, ImU32 colour) {
		text(at(x0 + 28, my), t.faint, label, Body, 20);
		textFit(at(x0 + 178, my), px(wrap - 150), colour, value, Bold, 20);
		my += 30;
	};
	if (!item.choices.empty() && item.current >= 0)
	{
		meta("Current", item.value, row->changed ? t.accent : t.text);
		if (row->def >= 0 && row->def < (int)item.choices.size())
			meta("Default", item.choices[(size_t)row->def], t.text);
		meta("Applies", row->restart ? "After restart" : "Immediately", t.text);
	}
	meta("Source", row->core ? "SwanStation" : "PSSwanStation", t.text);
	if (!item.note.empty())
	{
		textWrapped(at(x0 + 28, my + 6), px(wrap), t.faint, item.note, Body, 19, px(y1 - my - 12));
	}
	(void)forGame;
}

// The last changes, along the bottom.
void trailLine(float y, bool forGame)
{
	const Theme& t = theme();
	float x = 64;
	text(at(x, y), t.faint, "CHANGES", Bold, 18);
	x += 110;
	int shown = 0;
	for (size_t i = trail.size(); i-- > 0 && shown < 3;)
	{
		if (trail[i].forGame != forGame)
			continue;
		const std::string label = trail[i].name + ":  " + trail[i].from + "  \xe2\x86\x92  " + trail[i].to;
		const float w = std::min(toUnits(measure(label, Body, 20).x) + 12, 560.f);
		textFit(at(x, y - 2), px(w), shown == 0 ? t.pageText : t.pageDim, label, Body, 20);
		x += w + 36;
		shown++;
	}
	if (shown == 0)
		text(at(x, y - 2), t.pageDim, "Changes made here are listed here; L3 undoes the last.", Body, 20);
}

void boardView(Frame& f, std::vector<Row>& all, bool forGame)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	std::vector<int> shown;
	for (int a = 0; a < (int)areaList().size(); a++)
		if (std::any_of(all.begin(), all.end(), [a](const Row& r) { return r.area == a; }))
			shown.push_back(a);
	if (shown.empty())
		return;
	const int columns = 5, count = (int)shown.size();
	int focus = 0;
	for (int i = 0; i < count; i++)
		if (shown[(size_t)i] == f.a)
			focus = i;
	if (f.picker < 0)
	{
		if (nav(Right) || hit(R1))
			focus = (focus + 1) % count;
		if (nav(Left) || hit(L1))
			focus = (focus + count - 1) % count;
		if (nav(Down))
			focus = std::min(focus + columns, count - 1);
		if (nav(Up))
			focus = focus >= columns ? focus - columns : focus;
	}
	f.a = shown[(size_t)focus];

	int changedAll = 0, settingsAll = 0;
	for (const Row& row : all)
		if (row.def != -2)
		{
			settingsAll++;
			changedAll += row.changed;
		}
	text(at(64, 40), t.pageText, forGame ? "Game Settings" : "Settings", Title, 44);
	if (forGame)
		textFit(at(64 + toUnits(measure("Game Settings", Title, 44).x) + 28, 58), px(W - 900), t.pageDim,
				host::game().title, Body, 26);
	textRight(at(W - 64, 56), t.pageDim, format("%d changed of %d settings", changedAll, settingsAll), Body, 22);

	const float top = 128, bottom = H - 200, gap = 20;
	const int rowsOfTiles = (count + columns - 1) / columns;
	const float tw = (W - 128 - gap * (columns - 1)) / columns;
	const float th = std::min((bottom - top - gap * (rowsOfTiles - 1)) / rowsOfTiles, 380.f);
	for (int i = 0; i < count; i++)
	{
		const int area = shown[(size_t)i];
		const float x = 64 + (i % columns) * (tw + gap), y = top + (i / columns) * (th + gap);
		const bool on = i == focus;
		panel(at(x, y), at(x + tw, y + th), on ? t.panelHigh : t.panel, 16);
		if (on)
			focusRing(at(x, y), at(x + tw, y + th), 16);
		int settings = 0, changed = 0;
		for (const Row& row : all)
			if (row.area == area && row.def != -2)
			{
				settings++;
				changed += row.changed;
			}
		text(at(x + 24, y + 26), on ? t.accent : t.faint, areaList()[(size_t)area].icon, Body, 26);
		textFit(at(x + 68, y + 22), px(tw - 92), t.text, areaList()[(size_t)area].name, Bold, 30);
		text(at(x + 24, y + 70), changed > 0 ? t.accent : t.dim,
				changed > 0 ? format("%d changed of %d", changed, settings) : format("%d settings", settings), Body, 20);
		textWrapped(at(x + 24, y + 104), px(tw - 48), t.dim, sectionsLine(area, all), Body, 19, px(th * 0.42f));
		// A light for each setting, lit when it is not at its default.
		float lx = x + 26, ly = y + th - 30;
		int section = -1;
		std::vector<std::pair<float, float>> lit, dark;
		std::vector<const Row *> list;
		for (const Row& row : all)
			if (row.area == area && row.def != -2)
				list.push_back(&row);
		const float step = 15;
		const int perLine = std::max(1, (int)((tw - 52) / step));
		const int linesNeeded = ((int)list.size() + perLine - 1) / perLine;
		ly -= (linesNeeded - 1) * step;
		for (const Row *row : list)
		{
			if (section >= 0 && row->section != section)
				lx += 6;
			section = row->section;
			if (lx > x + tw - 26)
			{
				lx = x + 26;
				ly += step;
			}
			(row->changed ? lit : dark).push_back({ lx, ly });
			lx += step;
		}
		for (const auto& [px_, py] : dark)
			draw()->AddCircleFilled(at(px_, py), px(4.5f), withAlpha(t.faint, 0.55f));
		for (const auto& [px_, py] : lit)
		{
			glowAt(at(px_, py), px(12), withAlpha(t.accent, 0.35f));
			draw()->AddCircleFilled(at(px_, py), px(5), t.accent);
		}
	}

	// The quick settings: what the menu over a game offers at once.
	if (!forGame)
	{
		float x = 64;
		const float y = H - 176;
		text(at(x, y + 12), t.pageDim, "QUICK SETTINGS", Bold, 18);
		x += 190;
		for (const Row& row : pinnedRows())
		{
			const std::string label = std::string(icon::Star) + "  " + row.item.label + ":  " + row.item.value;
			if (x + toUnits(measure(label, Body, 20).x) + 40 > W - 64)
				break;
			x += pill(at(x, y), label, false, true, 44, 20) + 12;
		}
	}
	if (f.picker < 0 && hit(confirmButton))
	{
		f.s = "area";
		f.cursor = 0;
		f.scroll = f.scrollTarget = 0;
		f.fresh = true;
		consumeInput();
	}
	hintBar({ { confirmButton, "Open" }, { Left | Right, "Area" }, { Triangle, "Search" },
			{ cancelButton, "Back" } },
			forGame ? "What is set here is used for this game only and kept with it." : "");
}

void areaView(Frame& f, std::vector<Row>& all, bool forGame)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	std::vector<int> shown;
	for (int a = 0; a < (int)areaList().size(); a++)
		if (std::any_of(all.begin(), all.end(), [a](const Row& r) { return r.area == a; }))
			shown.push_back(a);
	if (shown.empty())
		return;
	if (std::find(shown.begin(), shown.end(), f.a) == shown.end())
		f.a = shown.front();
	if (f.picker < 0)
	{
		if (hit(cancelButton))
		{
			f.s.clear();
			f.cursor = 0;
			f.fresh = true;
			consumeInput();
			return;
		}
		const int at_ = (int)(std::find(shown.begin(), shown.end(), f.a) - shown.begin());
		const int n = (int)shown.size();
		const int next = hit(R1) ? (at_ + 1) % n : hit(L1) ? (at_ + n - 1) % n : at_;
		if (next != at_)
		{
			f.a = shown[(size_t)next];
			f.cursor = 0;
			f.scroll = f.scrollTarget = 0;
			f.fresh = true;
		}
		if (hit(L3))
			deferred = [forGame] { undoLast(forGame); };
	}
	const int area = f.a;
	std::vector<const Row *> order;
	std::vector<Item> items = areaItems(all, area, forGame, &order);
	// A section asked for (About from the menu): its first row.
	if (f.fresh && !f.s2.empty())
	{
		for (size_t i = 0; i < items.size(); i++)
			if (items[i].header && items[i].label == f.s2 && i + 1 < items.size())
				f.cursor = (int)i + 1;
		f.s2.clear();
	}

	text(at(64, 40), t.pageDim, forGame ? "Game Settings" : "Settings", Title, 44);
	const float sx = 64 + toUnits(measure(forGame ? "Game Settings" : "Settings", Title, 44).x);
	text(at(sx + 14, 40), t.pageDim, "/", Title, 44);
	text(at(sx + 50, 40), t.pageText, areaList()[(size_t)area].name, Title, 44);

	// The sections, in order, with the one under the cursor lit.
	int cursorSection = -1;
	if (f.cursor >= 0 && f.cursor < (int)order.size() && order[(size_t)f.cursor] != nullptr)
		cursorSection = order[(size_t)f.cursor]->section;
	{
		float x = 64;
		const float y = 112;
		const std::vector<std::string>& sections = areaList()[(size_t)area].sections;
		for (size_t s = 0; s < sections.size(); s++)
		{
			int settings = 0, changed = 0;
			for (const Row& row : all)
				if (row.area == area && row.section == (int)s)
				{
					settings++;
					changed += row.changed;
				}
			if (settings == 0)
				continue;
			const std::string label = changed > 0 ? format("%s  %d", sections[s].c_str(), changed) : sections[s];
			const float w = toUnits(measure(label, Body, 20).x) + 40;
			if (x + w > W - 64)
				break;
			if (x > 64)
				text(at(x - 19, y + 9), t.pageDim, "\xe2\x86\x92", Body, 20);
			x += pill(at(x, y), label, (int)s == cursorSection, true, 42, 20) + 24;
		}
	}

	const float top = 174, bottom = H - 140;
	const float lx0 = 64, lx1 = W * 0.56f;
	panel(at(lx0, top), at(lx1, bottom), t.panel, 16);
	const Item *focused = runList(f, items, lx0 + 8, top + 10, lx1 - 16, bottom - 10);
	const Row *row = nullptr;
	if (focused != nullptr && f.cursor >= 0 && f.cursor < (int)order.size())
		row = order[(size_t)f.cursor];

	// Over a running game, Graphics and Display leave a window on it: the
	// game behind is the setting's effect, live.
	const float ix0 = lx1 + 24, ix1 = W - 64;
	if (live(f))
	{
		const float wy1 = top + (ix1 - ix0) * 9.f / 16.f * 0.8f;
		outline(at(ix0, top), at(ix1, wy1), withAlpha(t.text, 0.35f), 14, 2);
		text(at(ix0 + 16, top + 12), t.pageText, "LIVE", Bold, 18);
		infoPanel(row, ix0, wy1 + 18, ix1, bottom, forGame);
	}
	else
		infoPanel(row, ix0, top, ix1, bottom, forGame);

	trailLine(H - 124, forGame);

	if (row != nullptr && f.picker < 0 && hit(R3))
	{
		const std::string id = row->id;
		deferred = [id] { togglePin(id); };
	}
	std::vector<Hint> hints;
	if (focused != nullptr && focused->enabled && !focused->menu && focused->choices.size() > 1)
		hints.push_back({ Left | Right, "Change" });
	else if (focused != nullptr && focused->enabled && focused->activate)
		hints.push_back({ confirmButton, "Open" });
	hints.push_back({ L2 | R2, "Section" });
	hints.push_back({ L1 | R1, "Area" });
	if (focused != nullptr && focused->alt)
		hints.push_back({ Square, focused->altHint });
	if (row != nullptr && !forGame)
		hints.push_back({ R3, pinned(row->id) ? "Unpin" : "Pin" });
	hints.push_back({ L3, "Undo" });
	hints.push_back({ Triangle, "Search" });
	hints.push_back({ cancelButton, "Board" });
	hintBar(hints);
}
}

// ------------------------------------------------------------------ the rows

const std::vector<AreaDef>& areas()
{
	return areaList();
}

std::vector<Pooled> poolRows(bool forGame)
{
	std::vector<Pooled> pool;
	auto take = [&](const char *from, std::vector<Item>& items) {
		std::string head;
		for (Item& item : items)
		{
			if (item.header)
			{
				head = item.label;
				continue;
			}
			pool.push_back({ from, head, std::move(item) });
		}
	};
	{
		std::vector<Item> items;
		pictureItems(forGame, items);
		if (!forGame)
			items.push_back(toggle("Show frame rate", &options::frontend().showFps, ""));
		take("picture", items);
	}
	if (!forGame)
		for (const auto& [kind, name] : std::vector<std::pair<int, const char *>>{
				{ 0, "library" }, { 8, "look" }, { 2, "sound" }, { 3, "controls" }, { 4, "games" }, { 7, "system" } })
		{
			std::vector<Item> items;
			frontendItems(kind, items);
			take(name, items);
		}
	{
		std::vector<Item> items;
		optionItems("", true, forGame, items);
		take("emulator", items);
		for (const options::Category& category : options::categories())
		{
			items.clear();
			optionItems(category.key, false, forGame, items);
			take(category.key.c_str(), items);
		}
	}
	if (!forGame)
	{
		std::vector<Item> items;
		aboutItems(items);
		take("about", items);
	}
	return pool;
}

std::vector<Row> rows(bool forGame)
{
	static std::set<std::string> unplacedSaid;
	std::vector<Pooled> pool = poolRows(forGame);
	struct Placed
	{
		Row row;
		int place, order;
	};
	std::vector<Placed> placed;
	int order = 0;
	for (Pooled& p : pool)
	{
		Row row;
		row.from = p.from;
		row.core = !p.item.key.empty() && p.item.key.compare(0, 12, "swanstation_") == 0;
		int placeIndex = -1;
		if (p.from == "about")
		{
			row.area = System;
			row.section = sectionIndex(System, p.header == "Licences" ? "Licences" : "About");
			row.def = -2;
			placeIndex = 100000;
		}
		else
		{
			for (int i = 0; i < (int)(sizeof(places) / sizeof(places[0])); i++)
				if (matches(places[i], p.item))
				{
					placeIndex = i;
					break;
				}
			if (placeIndex < 0)
			{
				row.area = System;
				row.section = sectionIndex(System, "Other");
				row.def = p.item.defaultChoice >= 0 ? p.item.defaultChoice : -2;
				placeIndex = 200000;
				if (unplacedSaid.insert(p.item.label).second)
					diag::mark("board: no place for \"%s\" (%s)", p.item.label.c_str(), p.from.c_str());
			}
			else
			{
				const Place& place = places[placeIndex];
				if (place.area < 0)
					continue;
				row.area = place.area;
				row.section = sectionIndex(place.area, place.section);
				row.def = place.def == -1 ? p.item.defaultChoice : place.def;
				if (place.name != nullptr)
					p.item.label = place.name;
				if (place.desc != nullptr)
				{
					// What is so now (a reason it is not offered, a status) stays.
					std::string now;
					const size_t at_ = p.item.info.find("\n\nNow: ");
					if (at_ != std::string::npos)
						now = p.item.info.substr(at_);
					p.item.info = expand(place.desc) + now;
				}
			}
		}
		row.id = !p.item.key.empty() ? p.item.key : p.item.label;
		row.restart = p.item.label.find("(Restart)") != std::string::npos;
		row.changed = row.def >= 0 && p.item.current >= 0 && p.item.choices.size() > 1 && p.item.current != row.def;
		p.item.lit = row.changed;
		// The title's own settings go back to their default with Square, as the
		// emulator's do.
		if (!row.core && row.changed && !p.item.alt && p.item.choose && p.item.enabled
				&& row.def < (int)p.item.choices.size())
		{
			p.item.alt = [choose = p.item.choose, def = row.def] { choose(def); };
			p.item.altHint = "Default";
		}
		row.item = std::move(p.item);
		placed.push_back({ std::move(row), placeIndex, order++ });
	}
	std::stable_sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) {
		if (a.row.area != b.row.area)
			return a.row.area < b.row.area;
		if (a.row.section != b.row.section)
			return a.row.section < b.row.section;
		if (a.place != b.place)
			return a.place < b.place;
		return a.order < b.order;
	});
	std::vector<Row> out;
	out.reserve(placed.size());
	for (Placed& p : placed)
		out.push_back(std::move(p.row));
	return out;
}

bool live(const Frame& f)
{
	return host::running() && f.page == Page::Settings && f.s == "area" && (f.a == Graphics || f.a == Display);
}

float gameDim(const Frame& f)
{
	return live(f) ? 0.2f : 0.72f;
}

void page(Frame& f)
{
	const bool forGame = f.b != 0;
	std::vector<Row> all = rows(forGame);
	if (f.picker < 0 && hit(Triangle))
	{
		deferred = [forGame] {
			askText("Search Settings", "", false, false, [forGame](const std::string& query) {
				if (!query.empty())
					push(Page::SettingsSearch, 0, forGame ? 1 : 0, query);
			});
		};
		consumeInput();
	}
	if (f.s == "area")
		areaView(f, all, forGame);
	else
		boardView(f, all, forGame);
}

void searchPage(Frame& f)
{
	const bool forGame = f.b != 0;
	std::vector<Row> all = rows(forGame);
	std::vector<std::string> words;
	{
		std::string word;
		for (char c : lower(f.s) + " ")
		{
			if (c == ' ')
			{
				if (!word.empty())
					words.push_back(word);
				word.clear();
			}
			else
				word += c;
		}
	}
	struct Hit
	{
		Row *row;
		int score;
	};
	std::vector<Hit> hits;
	for (Row& row : all)
	{
		const std::string name = lower(row.item.label), body = lower(row.item.info);
		int score = 0;
		bool every = true;
		for (const std::string& w : words)
		{
			if (name.find(w) != std::string::npos)
				score += 3;
			else if (body.find(w) != std::string::npos)
				score += 1;
			else
				every = false;
		}
		if (every && score > 0)
			hits.push_back({ &row, score });
	}
	std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
	std::vector<Item> items;
	std::vector<const Row *> order;
	for (const Hit& h : hits)
	{
		wrapForTrail(*h.row, forGame);
		Item item = h.row->item;
		item.info = std::string(areaList()[(size_t)h.row->area].name) + "  \xe2\x86\x92  "
				+ areaList()[(size_t)h.row->area].sections[(size_t)h.row->section] + "\n\n" + item.info;
		items.push_back(std::move(item));
		order.push_back(h.row);
	}
	if (items.empty())
	{
		Item none = fact("No setting matches", "", "Search looks in the name and the description of every setting.");
		items.push_back(none);
	}
	const Item *focused = menuPage(f, "Search Settings", "\"" + f.s + "\"  \xc2\xb7  " + format("%d found", (int)hits.size()),
			items, 900);
	const Row *row = !order.empty() && f.cursor >= 0 && f.cursor < (int)order.size() ? order[(size_t)f.cursor] : nullptr;
	if (row != nullptr && f.picker < 0 && hit(R1))
	{
		const int area = row->area;
		const std::string id = row->id;
		const bool game = forGame;
		deferred = [area, id, game] {
			pop();
			if (!stack.empty() && stack.back().page == Page::Settings)
			{
				Frame& s = stack.back();
				s.a = area;
				s.s = "area";
				s.fresh = true;
				std::vector<Row> list = rows(game);
				std::vector<const Row *> order_;
				areaItems(list, area, game, &order_);
				for (size_t i = 0; i < order_.size(); i++)
					if (order_[i] != nullptr && order_[i]->id == id)
						s.cursor = (int)i;
			}
		};
	}
	std::vector<Hint> hints;
	if (focused != nullptr && focused->enabled && focused->choices.size() > 1 && !focused->menu)
		hints.push_back({ Left | Right, "Change" });
	if (row != nullptr)
		hints.push_back({ R1, "Show in its area" });
	if (focused != nullptr && focused->alt)
		hints.push_back({ Square, focused->altHint });
	hints.push_back({ cancelButton, "Back" });
	hintBar(hints);
}

std::vector<Item> quickItems()
{
	std::vector<Item> items;
	for (Row& row : pinnedRows())
	{
		wrapForTrail(row, false);
		items.push_back(row.item);
	}
	return items;
}

// ------------------------------------------------------------------ PC test

void dump(const char *path)
{
	FILE *out = std::fopen(path, "w");
	if (out == nullptr)
		return;
	for (const Row& row : rows(false))
	{
		std::string info = row.item.info;
		for (char& c : info)
			if (c == '\n' || c == '\t')
				c = ' ';
		std::fprintf(out, "%s\t%s\t%s\t%s\t%d/%d\t%d\t%d\t%s\t%s\n", areaList()[(size_t)row.area].name,
				areaList()[(size_t)row.area].sections[(size_t)row.section].c_str(), row.item.label.c_str(),
				row.id.c_str(), row.item.current, (int)row.item.choices.size(), row.def, row.changed ? 1 : 0,
				row.from.c_str(), info.c_str());
	}
	std::fclose(out);
}
}
}
