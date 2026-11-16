/*
	PSSwanStation - where your files are: game folders anywhere, and a folder
	of your choice for the BIOS, memory cards, save states, cheats, covers and
	texture packs (after Crux2's Settings > Folders, 369a544; Crux2 by
	Press5elect).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	Kept in <title folder>folders.cfg, a line each:
	  bios = /mnt/usb0/PSX/bios
	  memory_cards = /data/psx/cards
	  game_folder = /mnt/usb1/Games        (as many as you like)
	A kind with no line is in its usual place under the folder for your files
	(rootDir: bios/, data/saves/, data/states/, cheats/, covers/, textures/).
	The BIOS, memory cards and texture packs are handed to the emulator as
	PSSwanStation starts, so a change to them is used from the next start;
	save states, cheats and covers are looked up each time.
*/
#pragma once

#include <string>
#include <vector>

namespace fe::folders
{
enum Kind { Bios, MemoryCards, SaveStates, Cheats, Covers, Textures, KindCount };

// "BIOS", "Memory Cards"...
const char *name(Kind kind);
// The usual place, with a trailing '/'.
std::string standard(Kind kind);
// The folder chosen ("" for the usual place), and the one in use, with a
// trailing '/'.
std::string chosen(Kind kind);
std::string folder(Kind kind);
// Whether it is used only from the next start.
bool needsRestart(Kind kind);
// Chooses a folder (empty: back to the usual place). A folder written to is
// tried for writing first; false, with why, when it cannot be.
bool choose(Kind kind, const std::string& path, std::string& why);

// The game folders added, scanned with the folders inside them.
std::vector<std::string> gameFolders();
bool addGameFolder(const std::string& path, std::string& why);
void removeGameFolder(const std::string& path);

// Where a folder can be chosen from: the title's folder, the folder for your
// files, the console's storage and each USB drive, as {name, path}.
std::vector<std::pair<std::string, std::string>> places();
// The folders in `path`, by name (hidden ones left out; at most 1000).
std::vector<std::string> subFolders(const std::string& path);

// Reads folders.cfg (once; again after a change).
void load();
}
