/*
	PSSwanStation - PlayStation memory cards, read and changed as files.

	SPDX-License-Identifier: GPL-3.0-or-later

	The card's layout, as the psx-spx documentation ("Memory Card Data Format")
	describes it. Block 0 is 64 frames of 128 bytes:

		frame 0			"MC", and the XOR of bytes 0..126 in byte 127
		frames 1..15	the directory, a frame for each of blocks 1..15
		frames 16..35	the list of broken sectors
		frame 63		a copy of frame 0 (the console's write test)

	A directory frame: the block's state at +0 (0x51 the first block of a save,
	0x52 a middle one, 0x53 the last; 0xA0 free, 0xA1..0xA3 the same three
	after the save was deleted), the save's size in bytes at +4 and its file
	name at +0x0A (both in the first block's frame only), the next block of the
	chain at +8 (its number less one, 0xFFFF for none), the checksum at +127.

	A save's first block: "SC", the icon's frame count at +2 (0x11..0x13), the
	title at +4 (64 bytes of Shift-JIS), 16 colours at +0x60 and the icon's
	frames at +0x80, +0x100 and +0x180 (16 x 16, four bits a pixel).

	Nothing here trusts a card: a chain that loops, points outside the card or
	runs into another save ends where it stops making sense.
*/
#include "memcard.h"

#include "fe.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fe::memcard
{
namespace
{
constexpr size_t FrameSize = 128;
constexpr size_t NameOffset = 0x0A;
constexpr size_t NameLength = 20;

enum : uint8_t
{
	StateFirst = 0x51,
	StateMiddle = 0x52,
	StateLast = 0x53,
	StateFree = 0xA0,
	// What the console's card manager leaves of a deleted save: the frames
	// keep their names and links, only the state changes.
	StateDeletedFirst = 0xA1,
	StateDeletedMiddle = 0xA2,
	StateDeletedLast = 0xA3,
};

// The headers other programs put before a card or a single save.
constexpr size_t VgsHeader = 64;		// Virtual Game Station: .vgs, .mem ("VgsM")
constexpr size_t VmpHeader = 128;		// PSP and PS3 virtual cards: .vmp ("\0PMV")
constexpr size_t GmeHeader = 3904;		// DexDrive: .gme ("123-456-STD")
constexpr size_t McsHeader = 128;		// .mcs: the save's directory frame
constexpr size_t ArHeader = 54;			// Action Replay, Xploder, GameShark: .psx .mcb .mcx .pda
constexpr size_t PsvHeader = 0x84;		// a PS3's export of a PS1 save: .psv
constexpr size_t PsvType = 0x3C;		// 1 a PS1 save, 2 a PS2 save
constexpr size_t PsvName = 0x64;

// No file read here is larger than these: a disc image that happens to be
// named .bin is not read whole to find that out.
constexpr size_t MostCardFile = CardSize + GmeHeader;
constexpr size_t MostSaveFile = PsvHeader + (size_t)Blocks * BlockSize;

void say(std::string *error, const std::string& text)
{
	if (error != nullptr)
		*error = text;
}

// A file of at most `most` bytes. False when it is larger or cannot be read.
bool readSmallFile(const std::string& path, std::vector<uint8_t>& out, size_t most)
{
	out.clear();
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	out.resize(most + 1);
	const size_t got = fread(out.data(), 1, out.size(), f);
	const bool ok = ferror(f) == 0 && got <= most;
	fclose(f);
	out.resize(ok ? got : 0);
	return ok;
}

uint8_t checksum(const uint8_t *frame)
{
	uint8_t sum = 0;
	for (size_t i = 0; i < FrameSize - 1; i++)
		sum ^= frame[i];
	return sum;
}

void seal(uint8_t *frame)
{
	frame[FrameSize - 1] = checksum(frame);
}

// The directory frame of a block (1..15) of a card of the full size.
const uint8_t *frameOf(const std::vector<uint8_t>& data, int block)
{
	return data.data() + (size_t)block * FrameSize;
}

uint8_t *frameOf(std::vector<uint8_t>& data, int block)
{
	return data.data() + (size_t)block * FrameSize;
}

bool isFree(uint8_t state)
{
	return state >= StateFree && state <= StateDeletedLast;
}

// The name in a directory frame (or anywhere else 21 bytes hold one), as the
// bytes it is.
std::string nameAt(const uint8_t *name)
{
	size_t length = 0;
	while (length < NameLength && name[length] != 0)
		length++;
	return std::string(reinterpret_cast<const char *>(name), length);
}

// A name a save file may bring: something, in printable ASCII.
bool usableName(const std::string& name)
{
	if (name.empty())
		return false;
	for (const char c : name)
		if (c < 0x20 || c > 0x7E)
			return false;
	return true;
}

// The saves of a card: each one's blocks in the order of its chain. A chain
// goes on only into a block marked as a middle or last one that no chain has
// taken yet, so it has an end whatever the card says, and no block belongs to
// two saves.
std::vector<std::vector<int>> chainsOf(const std::vector<uint8_t>& data)
{
	std::vector<std::vector<int>> chains;
	if (data.size() != CardSize)
		return chains;
	bool taken[Blocks + 1] = {};
	for (int first = 1; first <= Blocks; first++)
	{
		if (frameOf(data, first)[0] != StateFirst)
			continue;
		std::vector<int> chain{first};
		for (int at = first; (int)chain.size() < Blocks;)
		{
			const uint8_t *frame = frameOf(data, at);
			const unsigned next = (unsigned)frame[8] | (unsigned)frame[9] << 8;
			if (next >= (unsigned)Blocks)
				break;
			const int block = (int)next + 1;
			const uint8_t state = frameOf(data, block)[0];
			if (taken[block] || (state != StateMiddle && state != StateLast))
				break;
			taken[block] = true;
			chain.push_back(block);
			at = block;
			if (state == StateLast)
				break;
		}
		chains.push_back(std::move(chain));
	}
	return chains;
}

// The chain of the save that begins at a block, or nullptr.
const std::vector<int> *chainFrom(const std::vector<std::vector<int>>& chains, int firstBlock)
{
	for (const std::vector<int>& chain : chains)
		if (chain.front() == firstBlock)
			return &chain;
	return nullptr;
}

// ------------------------------------------------------------------- titles

// The first row of Shift-JIS symbols (0x8140..0x819A) as what a Latin font
// has of them; nullptr where it has nothing alike.
const char *const Symbols[] = {
	/* 40 */ " ", ",", ".", ",", ".", " ", ":", ";", "?", "!", nullptr, nullptr, "'", "`", nullptr, "^",
	/* 50 */ "~", "_", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "-", "-", "-", "/", "\\",
	/* 60 */ "~", nullptr, "|", "...", "..", "'", "'", "\"", "\"", "(", ")", "[", "]", "[", "]", "{",
	/* 70 */ "}", "<", ">", "<", ">", "\"", "\"", "\"", "\"", "[", "]", "+", "-", nullptr, "x", nullptr,
	/* 80 */ nullptr, "=", nullptr, "<", ">", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "'", "\"", nullptr, nullptr,
	/* 90 */ "$", nullptr, nullptr, "%", "#", "&", "*", "@", nullptr, "*", "*",
};

// Hiragana from 0x829F and katakana from 0x8340 come in this one order (as
// they do from U+3041 and U+30A1). The interface's font has Latin letters
// only, so a kana is shown as its reading.
const char *const Kana[] = {
	"a", "a", "i", "i", "u", "u", "e", "e", "o", "o",
	"ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go",
	"sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo",
	"ta", "da", "chi", "ji", "", "tsu", "zu", "te", "de", "to", "do",
	"na", "ni", "nu", "ne", "no",
	"ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu", "he", "be", "pe", "ho", "bo", "po",
	"ma", "mi", "mu", "me", "mo",
	"ya", "ya", "yu", "yu", "yo", "yo",
	"ra", "ri", "ru", "re", "ro",
	"wa", "wa", "wi", "we", "wo", "n",
	"vu", "ka", "ke",
};
constexpr int KanaCount = (int)(sizeof(Kana) / sizeof(Kana[0]));
static_assert(KanaCount == 86, "the kana table follows U+3041..U+3096");
static_assert(sizeof(Symbols) / sizeof(Symbols[0]) == 0x9B - 0x40, "the symbol table covers 0x8140..0x819A");
constexpr int Hiragana = 83;	// the last three are katakana only
constexpr int KanaSmallTsu = 34;
constexpr int KanaN = 82;

bool isSmallVowel(int kana)
{
	return kana < 10 && kana % 2 == 0;
}

bool isSmallY(int kana)
{
	return kana == 66 || kana == 68 || kana == 70;
}

bool isVowel(char c)
{
	return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o';
}

// A title being put together from kana: where the run of them began (its
// first letter is written as a capital), and the syllable a small kana after
// it would change.
struct KanaRun
{
	bool open = false;
	size_t start = 0;
	bool hasLast = false;	// the text ends with a syllable that ends in a vowel
	size_t last = 0;		// where it begins
	int lastKana = 0;
	bool doubled = false;	// a small tsu came: the next consonant is doubled
};

void addKana(std::string& out, KanaRun& run, int kana)
{
	if (!run.open)
	{
		run = KanaRun();
		run.open = true;
		run.start = out.size();
	}
	// The run's capital is put back at the end: the rules below look at
	// small letters.
	if (out.size() > run.start && out[run.start] >= 'A' && out[run.start] <= 'Z')
		out[run.start] = (char)(out[run.start] - 'A' + 'a');
	const char *letters = Kana[kana];
	const char vowel = kana == KanaSmallTsu ? 0 : letters[strlen(letters) - 1];
	if (kana == KanaSmallTsu)
	{
		run.doubled = true;
		run.hasLast = false;
	}
	else if (run.hasLast && isSmallVowel(kana))
	{
		// fu + small a is "fa", te + small i is "ti", u + small i is "wi"; a
		// repeated vowel only lengthens the syllable.
		if (out.back() == vowel)
			out += vowel;
		else if (out.size() - run.last == 1 && out.back() == 'u')
		{
			out.back() = 'w';
			out += vowel;
		}
		else
			out.back() = vowel;
		run.hasLast = false;
	}
	else if (run.hasLast && isSmallY(kana) && (out.back() == 'i' || out.back() == 'e'))
	{
		// ki + small ya is "kya"; shi, chi and ji take the vowel alone ("sha").
		out.pop_back();
		const bool plain = run.lastKana == 22 || run.lastKana == 23 || run.lastKana == 32 || run.lastKana == 33;
		if (!plain)
			out += 'y';
		out += vowel;
		run.hasLast = false;
	}
	else
	{
		if (run.doubled && !isVowel(letters[0]))
			out += letters[0] == 'c' ? 't' : letters[0];
		run.doubled = false;
		run.last = out.size();
		run.lastKana = kana;
		run.hasLast = kana != KanaN;
		out += letters;
	}
	if (out.size() > run.start && out[run.start] >= 'a' && out[run.start] <= 'z')
		out[run.start] = (char)(out[run.start] - 'a' + 'A');
}

bool isLeadByte(uint8_t c)
{
	return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

bool isTrailByte(uint8_t c)
{
	return (c >= 0x40 && c <= 0x7E) || (c >= 0x80 && c <= 0xFC);
}

// A save's Shift-JIS title as the interface's font can show it: the wide
// Latin letters, digits and signs become the ASCII ones, kana their reading,
// and whatever is left (kanji mostly) one middle dot for each run of it.
std::string shownTitle(const uint8_t *text, size_t bytes)
{
	std::string out;
	KanaRun run;
	bool dotted = false;	// the text ends with the dot for what cannot be shown
	for (size_t i = 0; i < bytes && text[i] != 0;)
	{
		const uint8_t c = text[i];
		char single[2] = {0, 0};
		const char *ascii = nullptr;
		int kana = -1;
		if (c >= 0x20 && c <= 0x7E)
		{
			single[0] = (char)c;
			ascii = single;
			i++;
		}
		else if (isLeadByte(c) && i + 1 < bytes && isTrailByte(text[i + 1]))
		{
			const uint8_t d = text[i + 1];
			i += 2;
			if (c == 0x81)
			{
				if (d == 0x5B && run.open)
				{
					// The long vowel mark inside a word.
					out += '-';
					run.hasLast = false;
					run.doubled = false;
					continue;
				}
				if (d >= 0x40 && (size_t)(d - 0x40) < sizeof(Symbols) / sizeof(Symbols[0]))
					ascii = Symbols[d - 0x40];
			}
			else if (c == 0x82)
			{
				if (d >= 0x4F && d <= 0x58)
					single[0] = (char)('0' + (d - 0x4F));
				else if (d >= 0x60 && d <= 0x79)
					single[0] = (char)('A' + (d - 0x60));
				else if (d >= 0x81 && d <= 0x9A)
					single[0] = (char)('a' + (d - 0x81));
				else if (d >= 0x9F && d <= 0x9F + Hiragana - 1)
					kana = d - 0x9F;
				if (single[0] != 0)
					ascii = single;
			}
			else if (c == 0x83 && d >= 0x40 && d <= 0x96 && d != 0x7F)
				kana = d - 0x40 - (d > 0x7F ? 1 : 0);
		}
		else
			// A control code, a half-width kana or a byte that is no character.
			i++;

		if (kana >= 0 && kana < KanaCount)
		{
			addKana(out, run, kana);
			dotted = false;
			continue;
		}
		run.open = false;
		if (ascii == nullptr)
		{
			if (!dotted)
				out += "\xC2\xB7";
			dotted = true;
			continue;
		}
		dotted = false;
		// Titles are padded with spaces to fill the console's two lines.
		if (ascii[0] == ' ' && (out.empty() || out.back() == ' '))
			continue;
		out += ascii;
	}
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	return out;
}

// --------------------------------------------------------------- one save

// "BASLUS-00594FF7-S01": B, the region, the game's product code, the game's
// own part.
void readName(Save& entry, const std::string& name)
{
	if (name.size() < 2 || name[0] != 'B' || (name[1] != 'A' && name[1] != 'E' && name[1] != 'I'))
		return;
	entry.region = name[1];
	if (name.size() < 12)
		return;
	for (size_t i = 2; i < 6; i++)
		if (name[i] < 'A' || name[i] > 'Z')
			return;
	// A PocketStation program has a P where the hyphen is.
	if (name[6] != '-' && name[6] != 'P')
		return;
	for (size_t i = 7; i < 12; i++)
		if (name[i] < '0' || name[i] > '9')
			return;
	entry.serial = name.substr(2, 4) + "-" + name.substr(7, 5);
}

// A 15-bit colour of the console as 0xAABBGGRR. 0x0000 is the transparent one.
uint32_t colour(unsigned value)
{
	if (value == 0)
		return 0;
	const uint32_t r = value & 31, g = (value >> 5) & 31, b = (value >> 10) & 31;
	// Five bits to eight: the top bits again at the bottom, so 31 is 255.
	const uint32_t r8 = (r << 3) | (r >> 2), g8 = (g << 3) | (g >> 2), b8 = (b << 3) | (b >> 2);
	return 0xFF000000u | (b8 << 16) | (g8 << 8) | r8;
}

// The title and the icon, from a save's first block.
void readTitleFrame(Save& entry, const uint8_t *block)
{
	if (block[0] != 'S' || block[1] != 'C')
		return;
	entry.title = shownTitle(block + 4, 64);
	uint32_t palette[16];
	for (int i = 0; i < 16; i++)
		palette[i] = colour((unsigned)block[0x60 + i * 2] | (unsigned)block[0x61 + i * 2] << 8);
	// Anything but 0x12 and 0x13 is shown as a still picture.
	const int frames = block[2] == 0x12 ? 2 : block[2] == 0x13 ? 3 : 1;
	for (int frame = 0; frame < frames; frame++)
	{
		const uint8_t *pixels = block + 0x80 + (size_t)frame * 0x80;
		std::vector<uint32_t> picture(16 * 16);
		for (size_t i = 0; i < 128; i++)
		{
			// The low half of a byte is the pixel on the left.
			picture[i * 2] = palette[pixels[i] & 15];
			picture[i * 2 + 1] = palette[pixels[i] >> 4];
		}
		entry.icon.push_back(std::move(picture));
	}
}

// Puts a save into a card: the lowest free blocks, chained in that order.
bool place(Card& to, const std::string& name, const uint8_t *blocks, int count, std::string *error)
{
	if (to.data.size() != CardSize || count < 1 || count > Blocks)
	{
		say(error, "The save could not be read.");
		return false;
	}
	for (const std::vector<int>& chain : chainsOf(to.data))
	{
		if (nameAt(frameOf(to.data, chain.front()) + NameOffset) == name)
		{
			say(error, format("The card already holds this save (%s).", name.c_str()));
			return false;
		}
	}
	std::vector<int> room;
	int freeBlocks = 0;
	for (int block = 1; block <= Blocks; block++)
	{
		if (!isFree(frameOf(to.data, block)[0]))
			continue;
		freeBlocks++;
		if ((int)room.size() < count)
			room.push_back(block);
	}
	if ((int)room.size() < count)
	{
		say(error, format("The card has too little room: the save takes %d %s, %d %s free.", count,
				count == 1 ? "block" : "blocks", freeBlocks, freeBlocks == 1 ? "is" : "are"));
		return false;
	}
	for (int i = 0; i < count; i++)
	{
		uint8_t *frame = frameOf(to.data, room[(size_t)i]);
		memset(frame, 0, FrameSize);
		frame[0] = i == 0 ? StateFirst : i == count - 1 ? StateLast : StateMiddle;
		if (i == 0)
		{
			const uint32_t bytes = (uint32_t)count * (uint32_t)BlockSize;
			frame[4] = (uint8_t)bytes;
			frame[5] = (uint8_t)(bytes >> 8);
			frame[6] = (uint8_t)(bytes >> 16);
			frame[7] = (uint8_t)(bytes >> 24);
			memcpy(frame + NameOffset, name.data(), std::min(name.size(), NameLength));
		}
		const unsigned next = i == count - 1 ? 0xFFFFu : (unsigned)(room[(size_t)i + 1] - 1);
		frame[8] = (uint8_t)next;
		frame[9] = (uint8_t)(next >> 8);
		seal(frame);
		memcpy(to.data.data() + (size_t)room[(size_t)i] * BlockSize, blocks + (size_t)i * BlockSize, BlockSize);
	}
	refresh(to);
	return true;
}

// A header's length and whole blocks after it: how many, or 0.
int blocksAfter(size_t fileSize, size_t header)
{
	if (fileSize <= header || (fileSize - header) % BlockSize != 0 || (fileSize - header) / BlockSize > (size_t)Blocks)
		return 0;
	return (int)((fileSize - header) / BlockSize);
}

bool hasExtension(const std::string& ext, const char *const *list)
{
	for (; *list != nullptr; list++)
		if (ext == *list)
			return true;
	return false;
}
}

bool parse(const std::vector<uint8_t>& file, Card& card, std::string *error)
{
	// Where the card begins: at the start, or after one of the headers. A
	// header counts when its own mark is there or the size is exactly its
	// length and a card's; the card's "MC" must be there in any case.
	struct Container
	{
		size_t header;
		const char *magic;
		size_t magicLength;
	};
	static const Container containers[] = {
		{0, "", 0},
		{GmeHeader, "123-456-STD", 11},
		{VgsHeader, "VgsM", 4},
		{VmpHeader, "\0PMV", 4},
	};
	for (const Container& container : containers)
	{
		if (file.size() < container.header + CardSize)
			continue;
		const bool marked = container.header == 0 || memcmp(file.data(), container.magic, container.magicLength) == 0;
		if (!marked && file.size() != container.header + CardSize)
			continue;
		const uint8_t *start = file.data() + container.header;
		if (start[0] != 'M' || start[1] != 'C')
			continue;
		// Through a copy: `file` may be the card's own bytes.
		std::vector<uint8_t> image(start, start + CardSize);
		card.data = std::move(image);
		refresh(card);
		return true;
	}
	say(error, file.size() == CardSize ? "This card is not formatted." : "This file is not a memory card.");
	return false;
}

bool load(const std::string& path, Card& card, std::string *error)
{
	std::vector<uint8_t> file;
	if (!readSmallFile(path, file, MostCardFile))
	{
		say(error, "This file could not be read as a memory card.");
		return false;
	}
	return parse(file, card, error);
}

bool save(const std::string& path, const Card& card)
{
	return card.data.size() == CardSize && writeFile(path, card.data.data(), card.data.size());
}

Card blank()
{
	// As the emulator formats a card (MemoryCardImage::Format).
	Card card;
	card.data.assign(CardSize, 0xFF);
	uint8_t *header = card.data.data();
	memset(header, 0, BlockSize);
	header[0] = 'M';
	header[1] = 'C';
	seal(header);
	for (int block = 1; block <= Blocks; block++)
	{
		uint8_t *frame = frameOf(card.data, block);
		frame[0] = StateFree;
		frame[8] = 0xFF;
		frame[9] = 0xFF;
		seal(frame);
	}
	// The broken sectors: none.
	for (size_t i = 16; i < 36; i++)
	{
		uint8_t *frame = header + i * FrameSize;
		memset(frame, 0xFF, 4);
		frame[8] = 0xFF;
		frame[9] = 0xFF;
		seal(frame);
	}
	memcpy(header + 63 * FrameSize, header, FrameSize);
	refresh(card);
	return card;
}

void refresh(Card& card)
{
	card.saves.clear();
	card.freeBlocks = 0;
	if (card.data.size() != CardSize)
		return;
	for (int block = 1; block <= Blocks; block++)
		if (isFree(frameOf(card.data, block)[0]))
			card.freeBlocks++;
	for (const std::vector<int>& chain : chainsOf(card.data))
	{
		Save entry;
		entry.firstBlock = chain.front();
		entry.blocks = (int)chain.size();
		entry.fileName = nameAt(frameOf(card.data, entry.firstBlock) + NameOffset);
		// For the screen: a damaged name must still be text.
		for (char& c : entry.fileName)
			if (c < 0x20 || c > 0x7E)
				c = '?';
		readName(entry, entry.fileName);
		readTitleFrame(entry, card.data.data() + (size_t)entry.firstBlock * BlockSize);
		// A save without a title frame (or with an empty title) is listed by
		// its file name.
		if (entry.title.empty())
			entry.title = entry.fileName;
		card.saves.push_back(std::move(entry));
	}
}

bool remove(Card& card, int firstBlock)
{
	const std::vector<std::vector<int>> chains = chainsOf(card.data);
	const std::vector<int> *chain = chainFrom(chains, firstBlock);
	if (chain == nullptr)
		return false;
	for (const int block : *chain)
	{
		uint8_t *frame = frameOf(card.data, block);
		frame[0] = (uint8_t)(frame[0] - StateFirst + StateDeletedFirst);
		seal(frame);
	}
	refresh(card);
	return true;
}

bool copy(const Card& from, int firstBlock, Card& to, std::string *error)
{
	const std::vector<std::vector<int>> chains = chainsOf(from.data);
	const std::vector<int> *chain = chainFrom(chains, firstBlock);
	if (chain == nullptr || to.data.size() != CardSize)
	{
		say(error, "The save could not be read.");
		return false;
	}
	std::vector<uint8_t> blocks;
	for (const int block : *chain)
	{
		const uint8_t *start = from.data.data() + (size_t)block * BlockSize;
		blocks.insert(blocks.end(), start, start + BlockSize);
	}
	return place(to, nameAt(frameOf(from.data, firstBlock) + NameOffset), blocks.data(), (int)chain->size(), error);
}

bool exportSave(const Card& card, int firstBlock, const std::string& path)
{
	const std::vector<std::vector<int>> chains = chainsOf(card.data);
	const std::vector<int> *chain = chainFrom(chains, firstBlock);
	if (chain == nullptr)
		return false;
	// The directory frame as the card has it, with the size the file really
	// holds.
	std::vector<uint8_t> file(frameOf(card.data, firstBlock), frameOf(card.data, firstBlock) + FrameSize);
	const uint32_t bytes = (uint32_t)chain->size() * (uint32_t)BlockSize;
	file[4] = (uint8_t)bytes;
	file[5] = (uint8_t)(bytes >> 8);
	file[6] = (uint8_t)(bytes >> 16);
	file[7] = (uint8_t)(bytes >> 24);
	seal(file.data());
	for (const int block : *chain)
	{
		const uint8_t *start = card.data.data() + (size_t)block * BlockSize;
		file.insert(file.end(), start, start + BlockSize);
	}
	return writeFile(path, file.data(), file.size());
}

bool importSave(Card& card, const std::string& path, std::string *error)
{
	std::vector<uint8_t> file;
	if (!readSmallFile(path, file, MostSaveFile))
	{
		say(error, "This file could not be read as a save.");
		return false;
	}
	// Told apart by what they hold, not by the extension: the three headers
	// have different lengths, so only one of them leaves whole blocks.
	std::string name;
	size_t header = 0;
	int count = 0;
	if (file.size() > PsvHeader && memcmp(file.data(), "\0VSP", 4) == 0)
	{
		if (file[PsvType] == 1 && (count = blocksAfter(file.size(), PsvHeader)) != 0 && file[PsvHeader] == 'S'
				&& file[PsvHeader + 1] == 'C')
		{
			header = PsvHeader;
			name = nameAt(file.data() + PsvName);
		}
		else
			count = 0;
	}
	else if ((count = blocksAfter(file.size(), McsHeader)) != 0 && file[0] == StateFirst)
	{
		header = McsHeader;
		name = nameAt(file.data() + NameOffset);
	}
	else if ((count = blocksAfter(file.size(), ArHeader)) != 0 && file[ArHeader] == 'S' && file[ArHeader + 1] == 'C')
	{
		header = ArHeader;
		name = nameAt(file.data());
	}
	else
		count = 0;
	if (count == 0 || !usableName(name))
	{
		say(error, "This file is not a save this card manager reads.");
		return false;
	}
	return place(card, name, file.data() + header, count, error);
}

int kindOf(const std::string& path, uint64_t size)
{
	static const char *const cards[] = {".mcd", ".mcr", ".mc", ".mci", ".srm", ".ps", ".ddf", ".psm", ".mem", ".vgs",
			".gme", ".vmp", nullptr};
	static const char *const withFrame[] = {".mcs", ".ps1", nullptr};
	static const char *const actionReplay[] = {".psx", ".mcb", ".mcx", ".pda", nullptr};
	const std::string ext = extension(path);
	if (size > MostCardFile)
		return 0;
	if (ext == ".bin")
		return size == CardSize ? 1 : 0;
	if (hasExtension(ext, cards))
		return size == CardSize || size == CardSize + VgsHeader || size == CardSize + VmpHeader
				|| size == CardSize + GmeHeader ? 1 : 0;
	if (hasExtension(ext, withFrame))
		return blocksAfter((size_t)size, McsHeader) != 0 ? 2 : 0;
	if (hasExtension(ext, actionReplay))
		return blocksAfter((size_t)size, ArHeader) != 0 ? 2 : 0;
	if (ext == ".psv")
		return blocksAfter((size_t)size, PsvHeader) != 0 ? 2 : 0;
	return 0;
}

}
