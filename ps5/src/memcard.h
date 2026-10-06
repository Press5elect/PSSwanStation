/*
	PSSwanStation - PlayStation memory cards, read and changed as files.

	SPDX-License-Identifier: GPL-3.0-or-later

	A card is 128 KiB: a header block (the directory: 15 frames of 128 bytes,
	one for each of the 15 blocks of 8 KiB that hold saves) and those blocks.
	A save takes one block or a chain of them; its first block begins with its
	title (Shift-JIS) and its icon (16 x 16, 16 colours, one to three frames).

	This is what the card manager works with (ui_cards.cpp). Nothing here
	touches a card the emulator has open: the manager is only offered while no
	game runs.
*/
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fe::memcard
{

constexpr size_t CardSize = 128 * 1024;
constexpr size_t BlockSize = 8 * 1024;
constexpr int Blocks = 15;

struct Save
{
	int firstBlock = 0;			// 1..15
	int blocks = 0;				// how many it takes
	std::string title;			// for the screen: UTF-8, what the card's Shift-JIS title can be shown as
	std::string fileName;		// the directory's name of it: "BASLUS-00594FF7-S01"
	std::string serial;			// "SLUS-00594", or empty when the name has none
	char region = 0;			// 'A' America, 'E' Europe, 'I' Japan (the name's second letter)
	// The icon's frames, 16 x 16 pixels each, as 0xAABBGGRR (what ImGui's and
	// display::createTexture's RGBA8 bytes are on a little-endian machine).
	std::vector<std::vector<uint32_t>> icon;
};

struct Card
{
	std::vector<uint8_t> data;	// CardSize bytes
	std::vector<Save> saves;	// by first block
	int freeBlocks = 0;
};

// Reads a card file: a raw image (.mcd .mcr .mc .mci .srm .ps .ddf .mem .vgs
// with its 64-byte header, .gme with its 3904-byte header, .vmp with its
// 128-byte header, .psm, .bin of exactly a card's size). False, with `error`
// said for the screen, when it is not a card.
bool load(const std::string& path, Card& card, std::string *error = nullptr);
// From bytes already in memory (the same formats).
bool parse(const std::vector<uint8_t>& file, Card& card, std::string *error = nullptr);
// Writes the raw 128 KiB image (what the emulator reads), through a
// temporary file renamed over the old one, mode 0666.
bool save(const std::string& path, const Card& card);
// An empty, formatted card.
Card blank();
// Reads the directory again after `data` was changed.
void refresh(Card& card);

// Frees a save's blocks (as the PlayStation's own card manager deletes).
bool remove(Card& card, int firstBlock);
// Copies a save to another card. False, with `error`, when the other card has
// too few free blocks or already holds a save of that file name.
bool copy(const Card& from, int firstBlock, Card& to, std::string *error = nullptr);

// One save as a file of its own: .mcs (the save's 128-byte directory frame,
// then its blocks), which other emulators and card tools read.
bool exportSave(const Card& card, int firstBlock, const std::string& path);
// A single-save file into a card: .mcs, .psx/.mcb/.mcx/.pda (54-byte header
// form), .psv (a PS3 export of a PS1 save). False, with `error`, when there
// is no room, a save of that name is there already, or the file is none of
// these.
bool importSave(Card& card, const std::string& path, std::string *error = nullptr);
// Whether a file name looks like something load() or importSave() reads:
// 1 a whole card, 2 a single save, 0 neither. By extension and size only.
int kindOf(const std::string& path, uint64_t size);

}
