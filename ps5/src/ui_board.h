/*
	PSSwanStation - the settings board (ui_board.cpp).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "ui_internal.h"

#include <string>
#include <vector>

namespace fe::ui::board
{
// An area of the board: its name, icon and sections, in order.
struct AreaDef
{
	const char *name;
	const char *icon;
	std::vector<std::string> sections;
};
const std::vector<AreaDef>& areas();
// The area numbers callers open directly.
constexpr int LibraryArea = 5, SystemArea = 9;

// A row as its builder made it, and where it came from.
struct Pooled
{
	std::string from, header;
	Item item;
};
std::vector<Pooled> poolRows(bool forGame);

// A setting on the board: its row (named and described for the board), its
// area and section, its default's choice (-2: not a value), whether it is
// not at its default, and whose it is.
struct Row
{
	Item item;
	int area = 0, section = 0;
	std::string id, from;
	bool core = false, restart = false, changed = false;
	int def = -2;
};
// Every setting in its place, in the board's order (the running game's own
// values with `forGame`).
std::vector<Row> rows(bool forGame);

// Page::Settings (a: the area, b: 1 for the running game's own values, s:
// "area" with an area open, s2: a section to open it at).
void page(Frame& f);
// Page::SettingsSearch (s: what is looked for, b: as Page::Settings').
void searchPage(Frame& f);
// The pinned settings, for the menu over a game.
std::vector<Item> quickItems();
// The settings page leaves the game behind it brighter (Graphics, Display).
bool live(const Frame& f);
float gameDim(const Frame& f);

// Every row, a line each (PC test: SWANSTATION_SETTINGS_DUMP=<file>).
void dump(const char *path);
}
