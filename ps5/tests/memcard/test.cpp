/*
	PSSwanStation - the memory card module's test (memcard.cpp), for a PC.

	SPDX-License-Identifier: GPL-3.0-or-later

	Cards are made with blank() and filled with saves built here (as .mcs
	files and the other single-save forms). After every change the card is
	read again by this file's own walk of the directory, which shares nothing
	with memcard.cpp: states, links, sizes, names and every frame's checksum.
	The last part damages the header block twenty thousand times; run.sh
	builds with the address sanitizer, so a read outside the card ends the
	run.

	The one argument is a folder for the files the test writes.
*/
#include "memcard.h"

#include "fe.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace fe;
using memcard::BlockSize;
using memcard::Blocks;
using memcard::Card;
using memcard::CardSize;
using memcard::Save;

namespace
{
int checks, failures;
std::string workDir;

#define CHECK(condition) \
	do \
	{ \
		checks++; \
		if (!(condition)) \
		{ \
			failures++; \
			if (failures <= 40) \
				printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

#define CHECK_TEXT(got, expected) \
	do \
	{ \
		checks++; \
		const std::string gotText = (got), expectedText = (expected); \
		if (gotText != expectedText) \
		{ \
			failures++; \
			if (failures <= 40) \
				printf("FAILED %s:%d: \"%s\" is not \"%s\"\n", __FILE__, __LINE__, gotText.c_str(), expectedText.c_str()); \
		} \
	} while (0)

struct Random
{
	uint64_t state;
	explicit Random(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
	uint32_t next()
	{
		state ^= state >> 12;
		state ^= state << 25;
		state ^= state >> 27;
		return (uint32_t)((state * 0x2545F4914F6CDD1Dull) >> 32);
	}
	uint32_t below(uint32_t limit) { return next() % limit; }
};

// ------------------------------------------------------------ plain files

std::string inWork(const std::string& name)
{
	return workDir + "/" + name;
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	const bool ok = bytes.empty() || fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
	return fclose(f) == 0 && ok;
}

std::vector<uint8_t> readBytes(const std::string& path)
{
	std::vector<uint8_t> bytes;
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return bytes;
	uint8_t part[4096];
	size_t got;
	while ((got = fread(part, 1, sizeof(part), f)) > 0)
		bytes.insert(bytes.end(), part, part + got);
	fclose(f);
	return bytes;
}

// ------------------------------------------------- the format, written out

uint8_t xorOf(const uint8_t *frame)
{
	uint8_t sum = 0;
	for (int i = 0; i < 127; i++)
		sum ^= frame[i];
	return sum;
}

const uint8_t *dirFrame(const Card& card, int block)
{
	return card.data.data() + (size_t)block * 128;
}

uint8_t *dirFrame(Card& card, int block)
{
	return card.data.data() + (size_t)block * 128;
}

unsigned le16(const uint8_t *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8;
}

unsigned le32(const uint8_t *p)
{
	return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

std::vector<uint8_t> bytesOf(const char *text)
{
	return std::vector<uint8_t>(text, text + strlen(text));
}

// A save's data: `blocks` blocks of noise, the first with a title frame.
std::vector<uint8_t> saveData(const std::vector<uint8_t>& title, int blocks, uint32_t seed, uint8_t iconFlag = 0x11)
{
	std::vector<uint8_t> data((size_t)blocks * BlockSize);
	Random random(seed);
	for (uint8_t& byte : data)
		byte = (uint8_t)random.next();
	data[0] = 'S';
	data[1] = 'C';
	data[2] = iconFlag;
	data[3] = (uint8_t)blocks;
	memset(data.data() + 4, 0, 0x5C);
	if (!title.empty())
		memcpy(data.data() + 4, title.data(), title.size() < 64 ? title.size() : 64);
	return data;
}

std::vector<uint8_t> mcsFile(const std::string& name, const std::vector<uint8_t>& data)
{
	std::vector<uint8_t> file(128);
	file[0] = 0x51;
	const uint32_t size = (uint32_t)data.size();
	file[4] = (uint8_t)size;
	file[5] = (uint8_t)(size >> 8);
	file[6] = (uint8_t)(size >> 16);
	file[8] = 0xFF;
	file[9] = 0xFF;
	memcpy(file.data() + 0x0A, name.data(), name.size() < 20 ? name.size() : 20);
	file[127] = xorOf(file.data());
	file.insert(file.end(), data.begin(), data.end());
	return file;
}

// The 54-byte header of Action Replay, Xploder and GameShark files.
std::vector<uint8_t> arFile(const std::string& name, const std::vector<uint8_t>& data)
{
	std::vector<uint8_t> file(54);
	memcpy(file.data(), name.data(), name.size() < 20 ? name.size() : 20);
	memcpy(file.data() + 0x15, "A TITLE IN ASCII", 16);
	file.insert(file.end(), data.begin(), data.end());
	return file;
}

// A PS3's export of a PS1 save.
std::vector<uint8_t> psvFile(const std::string& name, const std::vector<uint8_t>& data, uint8_t type = 1)
{
	std::vector<uint8_t> file(0x84);
	memcpy(file.data(), "\0VSP", 4);
	for (int i = 0x08; i < 0x30; i++)
		file[(size_t)i] = (uint8_t)(i * 7);		// where the seed and the signature are
	file[0x38] = 0x14;
	file[0x3C] = type;
	const uint32_t size = (uint32_t)data.size();
	for (int i = 0; i < 4; i++)
		file[0x40 + (size_t)i] = file[0x5C + (size_t)i] = (uint8_t)(size >> (8 * i));
	file[0x44] = 0x84;
	file[0x49] = 0x02;
	file[0x60] = 0x03;
	file[0x61] = 0x90;
	memcpy(file.data() + 0x64, name.data(), name.size() < 20 ? name.size() : 20);
	file.insert(file.end(), data.begin(), data.end());
	return file;
}

int fileNumber;

// importSave() reads a file: the bytes go through one.
bool importBytes(Card& card, const std::vector<uint8_t>& file, const char *extension = ".mcs", std::string *error = nullptr)
{
	const std::string path = inWork("import-" + std::to_string(fileNumber++ % 8) + extension);
	if (!writeBytes(path, file))
	{
		printf("could not write %s\n", path.c_str());
		return false;
	}
	return memcard::importSave(card, path, error);
}

bool addSave(Card& card, const std::string& name, int blocks, uint32_t seed, const char *title = "TEST SAVE")
{
	return importBytes(card, mcsFile(name, saveData(bytesOf(title), blocks, seed)));
}

const Save *findSave(const Card& card, const std::string& name)
{
	for (const Save& entry : card.saves)
		if (entry.fileName == name)
			return &entry;
	return nullptr;
}

// The same for a test that goes on to read the save: when it is missing the
// check fails and an empty save is read instead.
const Save noSave;

const Save& saveAt(const Card& card, size_t index)
{
	CHECK(index < card.saves.size());
	return index < card.saves.size() ? card.saves[index] : noSave;
}

const Save& lastSave(const Card& card)
{
	CHECK(!card.saves.empty());
	return card.saves.empty() ? noSave : card.saves.back();
}

const Save& found(const Card& card, const std::string& name)
{
	const Save *entry = findSave(card, name);
	CHECK(entry != nullptr);
	return entry == nullptr ? noSave : *entry;
}

// A save's blocks by this file's own reading of the links. Empty when the
// chain is not a sound one.
std::vector<int> walk(const Card& card, int firstBlock)
{
	std::vector<int> chain;
	bool seen[16] = {};
	int block = firstBlock;
	while (true)
	{
		if (block < 1 || block > 15 || seen[block])
			return {};
		seen[block] = true;
		chain.push_back(block);
		const uint8_t *frame = dirFrame(card, block);
		const unsigned next = le16(frame + 8);
		const uint8_t expected = chain.size() == 1 ? 0x51 : next == 0xFFFF ? 0x53 : 0x52;
		if (frame[0] != expected)
			return {};
		if (next == 0xFFFF)
			return chain;
		block = (int)next + 1;
	}
}

std::vector<uint8_t> blocksData(const Card& card, const std::vector<int>& chain)
{
	std::vector<uint8_t> data;
	for (const int block : chain)
		data.insert(data.end(), card.data.begin() + (long)block * (long)BlockSize,
				card.data.begin() + (long)(block + 1) * (long)BlockSize);
	return data;
}

// A directory frame's name as it is listed: bytes that are not text (a
// damaged card's) as question marks.
std::string shownName(const uint8_t *name)
{
	std::string text;
	for (int i = 0; i < 20 && name[i] != 0; i++)
		text += name[i] >= 0x20 && name[i] <= 0x7E ? (char)name[i] : '?';
	return text;
}

// A card that nothing damaged: every frame of the header is sealed, every
// used block belongs to exactly one listed save, and what the module lists
// is what the directory says.
void checkHealthy(const Card& card)
{
	CHECK(card.data.size() == CardSize);
	if (card.data.size() != CardSize)
		return;
	CHECK(card.data[0] == 'M' && card.data[1] == 'C');
	for (int frame = 0; frame < 36; frame++)
		CHECK(card.data[(size_t)frame * 128 + 127] == xorOf(card.data.data() + (size_t)frame * 128));
	CHECK(memcmp(card.data.data(), card.data.data() + 63 * 128, 128) == 0);

	bool used[16] = {};
	int usedBlocks = 0, previous = 0;
	for (const Save& entry : card.saves)
	{
		CHECK(entry.firstBlock > previous);
		previous = entry.firstBlock;
		const std::vector<int> chain = walk(card, entry.firstBlock);
		CHECK(!chain.empty());
		CHECK((int)chain.size() == entry.blocks);
		const uint8_t *first = dirFrame(card, entry.firstBlock);
		CHECK(le32(first + 4) == (unsigned)chain.size() * BlockSize);
		CHECK(shownName(first + 0x0A) == entry.fileName);
		CHECK(first[0x1F] == 0);
		for (size_t i = 0; i < chain.size(); i++)
		{
			CHECK(!used[chain[i]]);
			used[chain[i]] = true;
			usedBlocks++;
			if (i > 0)
			{
				// Only the first frame has the size and the name.
				const uint8_t *frame = dirFrame(card, chain[i]);
				CHECK(le32(frame + 4) == 0 && frame[0x0A] == 0);
			}
		}
	}
	int freeBlocks = 0;
	for (int block = 1; block <= 15; block++)
	{
		const uint8_t state = dirFrame(card, block)[0];
		if (used[block])
			CHECK(state >= 0x51 && state <= 0x53);
		else
		{
			CHECK(state >= 0xA0 && state <= 0xA3);
			freeBlocks++;
		}
	}
	CHECK(freeBlocks == card.freeBlocks);
	CHECK(usedBlocks + freeBlocks == 15);
}

// ------------------------------------------------------------------ tests

void testBlank()
{
	const Card card = memcard::blank();
	checkHealthy(card);
	CHECK(card.saves.empty());
	CHECK(card.freeBlocks == 15);
	CHECK(card.data[127] == 0x0E);		// 'M' ^ 'C'
	for (int block = 1; block <= 15; block++)
	{
		const uint8_t *frame = dirFrame(card, block);
		CHECK(frame[0] == 0xA0 && le16(frame + 8) == 0xFFFF && frame[127] == 0xA0);
		CHECK(le32(frame + 4) == 0 && frame[0x0A] == 0);
	}
	for (int frame = 16; frame < 36; frame++)
		CHECK(le32(card.data.data() + (size_t)frame * 128) == 0xFFFFFFFFu);
	bool blocksEmpty = true;
	for (size_t i = BlockSize; i < CardSize; i++)
		blocksEmpty = blocksEmpty && card.data[i] == 0xFF;
	CHECK(blocksEmpty);
	// refresh() of the same bytes changes nothing.
	Card again = card;
	memcard::refresh(again);
	CHECK(again.data == card.data && again.freeBlocks == 15 && again.saves.empty());
}

void testImportAndList()
{
	Card card = memcard::blank();
	const std::vector<uint8_t> one = saveData(bytesOf("SWAN ADVENTURES 2"), 1, 11);
	CHECK(importBytes(card, mcsFile("BASCUS-9415400000000", one)));
	checkHealthy(card);
	CHECK(card.saves.size() == 1 && card.freeBlocks == 14);
	if (card.saves.size() == 1)
	{
		const Save& entry = saveAt(card, 0);
		CHECK(entry.firstBlock == 1 && entry.blocks == 1);
		CHECK_TEXT(entry.fileName, "BASCUS-9415400000000");	// all 20 characters of it
		CHECK_TEXT(entry.serial, "SCUS-94154");
		CHECK(entry.region == 'A');
		CHECK_TEXT(entry.title, "SWAN ADVENTURES 2");
		CHECK(entry.icon.size() == 1);
	}
	const uint8_t *frame = dirFrame(card, 1);
	CHECK(frame[0] == 0x51 && le32(frame + 4) == 0x2000 && le16(frame + 8) == 0xFFFF);
	CHECK(memcmp(frame + 0x0A, "BASCUS-9415400000000", 20) == 0 && frame[0x1E] == 0);
	CHECK(blocksData(card, {1}) == one);

	// Three blocks: 2, 3 and 4, linked by block number less one.
	const std::vector<uint8_t> three = saveData(bytesOf("SWANS RACING"), 3, 12, 0x13);
	CHECK(importBytes(card, mcsFile("BESCES-00984GAMEDATA", three)));
	checkHealthy(card);
	CHECK(card.saves.size() == 2 && card.freeBlocks == 11);
	if (card.saves.size() == 2)
	{
		const Save& entry = saveAt(card, 1);
		CHECK(entry.firstBlock == 2 && entry.blocks == 3);
		CHECK_TEXT(entry.serial, "SCES-00984");
		CHECK(entry.region == 'E');
		CHECK(entry.icon.size() == 3);
	}
	CHECK(dirFrame(card, 2)[0] == 0x51 && le16(dirFrame(card, 2) + 8) == 2 && le32(dirFrame(card, 2) + 4) == 0x6000);
	CHECK(dirFrame(card, 3)[0] == 0x52 && le16(dirFrame(card, 3) + 8) == 3);
	CHECK(dirFrame(card, 4)[0] == 0x53 && le16(dirFrame(card, 4) + 8) == 0xFFFF);
	CHECK(blocksData(card, {2, 3, 4}) == three);
	CHECK(blocksData(card, {1}) == one);

	// The same name again is refused and changes nothing.
	const Card before = card;
	std::string error;
	CHECK(!importBytes(card, mcsFile("BESCES-00984GAMEDATA", saveData(bytesOf("OTHER"), 1, 13)), ".mcs", &error));
	CHECK(!error.empty() && error.find("BESCES-00984GAMEDATA") != std::string::npos);
	CHECK(card.data == before.data && card.saves.size() == 2);

	// The largest save there is: fifteen blocks, on an empty card only.
	const std::vector<uint8_t> all = saveData(bytesOf("BIG"), 15, 14);
	error.clear();
	CHECK(!importBytes(card, mcsFile("BISLPS-00001BIG", all), ".mcs", &error));
	CHECK(!error.empty() && card.data == before.data);
	Card empty = memcard::blank();
	CHECK(importBytes(empty, mcsFile("BISLPS-00001BIG", all)));
	checkHealthy(empty);
	CHECK(empty.freeBlocks == 0 && empty.saves.size() == 1 && saveAt(empty, 0).blocks == 15);
	CHECK(blocksData(empty, walk(empty, 1)) == all);
	error.clear();
	CHECK(!addSave(empty, "BISLPS-00002", 1, 15));
}

void testNames()
{
	struct Case
	{
		const char *name, *serial;
		char region;
	};
	const Case cases[] = {
		{"BASLUS-00594FF7-S01", "SLUS-00594", 'A'},
		{"BESLES-01234", "SLES-01234", 'E'},
		{"BISLPS-01234ABCDEFGH", "SLPS-01234", 'I'},
		{"BISCPSP10001POCKET", "SCPS-10001", 'I'},		// a PocketStation program
		{"BAHOMEBREW", "", 'A'},
		{"BASLUS-0059", "", 'A'},						// too short for a product code
		{"BASLUS_00594", "", 'A'},
		{"BAslus-00594", "", 'A'},
		{"BASLUS-0O594XX", "", 'A'},
		{"BXSLUS-00594", "", 0},
		{"XASLUS-00594", "", 0},
		{"B", "", 0},
		{"freeform name", "", 0},
	};
	for (const Case& one : cases)
	{
		Card card = memcard::blank();
		CHECK(addSave(card, one.name, 1, 21));
		CHECK(card.saves.size() == 1);
		if (card.saves.size() != 1)
			continue;
		CHECK_TEXT(saveAt(card, 0).fileName, one.name);
		CHECK_TEXT(saveAt(card, 0).serial, one.serial);
		CHECK(saveAt(card, 0).region == one.region);
	}
}

void testTitles()
{
	struct Case
	{
		std::vector<uint8_t> shiftJis;
		const char *shown;
	};
	const Case cases[] = {
		{{0x41, 0x20, 0x53, 0x41, 0x56, 0x45, 0x20, 0x49, 0x4E, 0x20, 0x41, 0x53, 0x43, 0x49, 0x49, 0x20, 0x37}, "A SAVE IN ASCII 7"},	// A SAVE IN ASCII 7
		{{0x82, 0x72, 0x82, 0x76, 0x82, 0x60, 0x82, 0x6D, 0x82, 0x72, 0x82, 0x73, 0x82, 0x60, 0x82, 0x73, 0x82, 0x68, 0x82, 0x6E, 0x82, 0x6D, 0x81, 0x7C, 0x82, 0x50, 0x81, 0x40, 0x81, 0x40, 0x82, 0x65, 0x82, 0x68, 0x82, 0x6D, 0x82, 0x68, 0x82, 0x72, 0x82, 0x67, 0x82, 0x64, 0x82, 0x63, 0x81, 0x40, 0x82, 0x50, 0x82, 0x4F, 0x82, 0x4F, 0x81, 0x93}, "SWANSTATION-1 FINISHED 100%"},	// ＳＷＡＮＳＴＡＴＩＯＮ－１　　ＦＩＮＩＳＨＥＤ　１００％
		{{0x82, 0x72, 0x82, 0x97, 0x82, 0x81, 0x82, 0x8E, 0x81, 0x40, 0x82, 0x72, 0x82, 0x94, 0x82, 0x81, 0x82, 0x94, 0x82, 0x89, 0x82, 0x8F, 0x82, 0x8E, 0x81, 0x69, 0x82, 0x51, 0x81, 0x6A, 0x81, 0x46, 0x82, 0x87, 0x82, 0x81, 0x82, 0x8D, 0x82, 0x85, 0x81, 0x49, 0x81, 0x48}, "Swan Station(2):game!?"},	// Ｓｗａｎ　Ｓｔａｔｉｏｎ（２）：ｇａｍｅ！？
		{{0x81, 0x94, 0x82, 0x50, 0x81, 0x95, 0x82, 0x51, 0x81, 0x7B, 0x82, 0x52, 0x81, 0x81, 0x82, 0x54, 0x81, 0x5E, 0x81, 0x97, 0x81, 0x96, 0x81, 0x90, 0x81, 0x83, 0x81, 0x84, 0x81, 0x6D, 0x81, 0x6E, 0x81, 0x6F, 0x81, 0x70, 0x81, 0x51}, "#1&2+3=5/@*$<>[]{}_"},	// ＃１＆２＋３＝５／＠＊＄＜＞［］｛｝＿
		{{0x82, 0x73, 0x82, 0x6E, 0x82, 0x6C, 0x82, 0x61, 0x81, 0x40, 0x82, 0x71, 0x82, 0x60, 0x82, 0x68, 0x82, 0x63, 0x82, 0x64, 0x82, 0x71, 0x81, 0x40, 0x81, 0x63, 0x81, 0x40, 0x81, 0x65, 0x82, 0x60, 0x81, 0x66, 0x81, 0x40, 0x81, 0x67, 0x82, 0x61, 0x81, 0x68}, "TOMB RAIDER ... 'A' \"B\""},	// ＴＯＭＢ　ＲＡＩＤＥＲ　…　‘Ａ’　“Ｂ”
		{{0x83, 0x5A, 0x81, 0x5B, 0x83, 0x75, 0x83, 0x66, 0x81, 0x5B, 0x83, 0x5E}, "Se-bude-ta"},	// セーブデータ
		{{0x83, 0x74, 0x83, 0x40, 0x83, 0x43, 0x83, 0x69, 0x83, 0x8B, 0x81, 0x45, 0x83, 0x74, 0x83, 0x40, 0x83, 0x93, 0x83, 0x5E, 0x83, 0x57, 0x81, 0x5B}, "Fainaru Fantaji-"},	// ファイナル・ファンタジー
		{{0x83, 0x4C, 0x83, 0x83, 0x83, 0x62, 0x83, 0x58, 0x83, 0x8B}, "Kyassuru"},	// キャッスル
		{{0x83, 0x60, 0x83, 0x87, 0x83, 0x52, 0x83, 0x7B}, "Chokobo"},	// チョコボ
		{{0x83, 0x57, 0x83, 0x83, 0x83, 0x93, 0x83, 0x76}, "Janpu"},	// ジャンプ
		{{0x83, 0x45, 0x83, 0x42, 0x83, 0x55, 0x81, 0x5B, 0x83, 0x68}, "Wiza-do"},	// ウィザード
		{{0x83, 0x7D, 0x83, 0x62, 0x83, 0x60}, "Matchi"},	// マッチ
		{{0x83, 0x65, 0x83, 0x42, 0x83, 0x74, 0x83, 0x40}, "Tifa"},	// ティファ
		{{0x83, 0x56, 0x83, 0x46, 0x83, 0x93, 0x83, 0x80, 0x81, 0x5B}, "Shenmu-"},	// シェンムー
		{{0x83, 0x94, 0x83, 0x40, 0x83, 0x8B, 0x83, 0x4C, 0x83, 0x8A, 0x81, 0x5B}, "Varukiri-"},	// ヴァルキリー
		{{0x82, 0xDA, 0x82, 0xAD, 0x82, 0xCC, 0x82, 0xC8, 0x82, 0xC2, 0x82, 0xE2, 0x82, 0xB7, 0x82, 0xDD}, "Bokunonatsuyasumi"},	// ぼくのなつやすみ
		{{0x82, 0xAB, 0x82, 0xE5, 0x82, 0xA4}, "Kyou"},	// きょう
		{{0x82, 0xAA, 0x82, 0xC1, 0x82, 0xB1, 0x82, 0xA4}, "Gakkou"},	// がっこう
		{{0x82, 0x65, 0x82, 0x65, 0x82, 0x56, 0x81, 0x5E, 0x83, 0x5A, 0x81, 0x5B, 0x83, 0x75, 0x82, 0x4F, 0x82, 0x50}, "FF7/Se-bu01"},	// ＦＦ７／セーブ０１
		{{0x90, 0xB9, 0x8C, 0x95, 0x93, 0x60, 0x90, 0xE0, 0x81, 0x40, 0x82, 0x6B, 0x82, 0x64, 0x82, 0x66, 0x82, 0x64, 0x82, 0x6D, 0x82, 0x63}, "\xC2\xB7" " LEGEND"},	// 聖剣伝説　ＬＥＧＥＮＤ
		{{0x8C, 0xB6, 0x91, 0x7A, 0x90, 0x85, 0x9F, 0xF5, 0x93, 0x60, 0x49, 0x49}, "\xC2\xB7" "II"},	// 幻想水滸伝II
		{{0x88, 0xAB, 0x96, 0x82, 0x8F, 0xE9, 0x83, 0x68, 0x83, 0x89, 0x83, 0x4C, 0x83, 0x85, 0x83, 0x89}, "\xC2\xB7" "Dorakyura"},	// 悪魔城ドラキュラ
		{{0x8C, 0x8E, 0x89, 0xBA, 0x82, 0xCC, 0x96, 0xE9, 0x91, 0x7A, 0x8B, 0xC8}, "\xC2\xB7" "No\xC2\xB7" ""},	// 月下の夜想曲
		{{0x82, 0x60, 0x82, 0x61, 0x82, 0x62, 0x81, 0x40, 0x81, 0x40, 0x81, 0x40}, "ABC"},	// ＡＢＣ　　　
		{{0x81, 0x40, 0x81, 0x40, 0x82, 0x60, 0x82, 0x61, 0x82, 0x62}, "ABC"},	// 　　ＡＢＣ
		{{0x41, 0x42, 0x43, 0x20, 0x20, 0x20}, "ABC"},	// ABC   
		{{0x83, 0x7E, 0x83, 0x80, 0x83, 0x81}, "Mimume"},	// ミムメ
		{{0x83, 0x96, 0x83, 0x95, 0x83, 0x94}, "Kekavu"},	// ヶヵヴ
		{{0x82, 0xF1}, "N"},	// ん
		{{0x82, 0xD9, 0x82, 0xF1, 0x82, 0xE2}, "Honya"},	// ほんや
		{{0xBE, 0xB0, 0xCC, 0xDE, 0x20, 0x82, 0x6E, 0x82, 0x6A}, "\xC2\xB7" " OK"},	// ｾｰﾌﾞ ＯＫ
	};
	for (const Case& one : cases)
	{
		Card card = memcard::blank();
		CHECK(importBytes(card, mcsFile("BISLPS-00001", saveData(one.shiftJis, 1, 31))));
		CHECK(card.saves.size() == 1);
		if (card.saves.size() == 1)
			CHECK_TEXT(saveAt(card, 0).title, one.shown);
	}

	// A title that fills all 64 bytes has no zero after it; the palette that
	// follows must not be read as text.
	{
		std::vector<uint8_t> full;
		for (int i = 0; i < 32; i++)
		{
			full.push_back(0x82);
			full.push_back((uint8_t)(0x60 + i % 26));
		}
		std::vector<uint8_t> data = saveData(full, 1, 32);
		memset(data.data() + 0x44, 'Z', 0x3C);
		// The title field ends at 0x44: put the text there by hand.
		memcpy(data.data() + 4, full.data(), 64);
		Card card = memcard::blank();
		CHECK(importBytes(card, mcsFile("BISLPS-00001", data)));
		CHECK_TEXT(saveAt(card, 0).title, "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF");
		// And one that ends in half a character.
		data[4 + 63] = 0x83;
		data[4 + 62] = 'x';
		card = memcard::blank();
		CHECK(importBytes(card, mcsFile("BISLPS-00001", data)));
		CHECK_TEXT(saveAt(card, 0).title, "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEx\xC2\xB7");
	}

	// No title, or no title frame at all: the file name stands in.
	{
		Card card = memcard::blank();
		CHECK(importBytes(card, mcsFile("BASLUS-00001EMPTY", saveData({}, 1, 33))));
		CHECK_TEXT(saveAt(card, 0).title, "BASLUS-00001EMPTY");
		std::vector<uint8_t> data = saveData(bytesOf("HIDDEN"), 1, 34);
		data[0] = 'X';
		CHECK(importBytes(card, mcsFile("BASLUS-00002NOSC", data)));
		CHECK(card.saves.size() == 2);
		CHECK_TEXT(saveAt(card, 1).title, "BASLUS-00002NOSC");
		CHECK(saveAt(card, 1).icon.empty());
	}
}

void testIcons()
{
	std::vector<uint8_t> data = saveData(bytesOf("ICON"), 1, 41, 0x12);
	const uint16_t palette[16] = {0x0000, 0x8000, 0x001F, 0x03E0, 0x7C00, 0x7FFF, 0xFFFF, 0x0421,
			0x0001, 0x0020, 0x0400, 0x8001, 0x4210, 0x7C1F, 0x03FF, 0x0010};
	const uint32_t expected[16] = {0x00000000, 0xFF000000, 0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF, 0xFFFFFFFF,
			0xFF080808, 0xFF000008, 0xFF000800, 0xFF080000, 0xFF000008, 0xFF848484, 0xFFFF00FF, 0xFF00FFFF, 0xFF000084};
	for (int i = 0; i < 16; i++)
	{
		data[0x60 + (size_t)i * 2] = (uint8_t)palette[i];
		data[0x61 + (size_t)i * 2] = (uint8_t)(palette[i] >> 8);
	}
	// Frame 1: pixel (x, y) has colour (x + y) & 15. Frame 2: colour x, the
	// other way round. Frame 3 is there in the block but not asked for.
	for (int y = 0; y < 16; y++)
	{
		for (int x = 0; x < 16; x += 2)
		{
			data[0x80 + (size_t)(y * 8 + x / 2)] = (uint8_t)(((x + y) & 15) | ((x + 1 + y) & 15) << 4);
			data[0x100 + (size_t)(y * 8 + x / 2)] = (uint8_t)((15 - x) | (15 - (x + 1)) << 4);
		}
	}
	Card card = memcard::blank();
	CHECK(importBytes(card, mcsFile("BASLUS-00003ICON", data)));
	CHECK(card.saves.size() == 1 && saveAt(card, 0).icon.size() == 2);
	if (card.saves.size() != 1 || saveAt(card, 0).icon.size() != 2)
		return;
	const auto& icon = saveAt(card, 0).icon;
	CHECK(icon[0].size() == 256 && icon[1].size() == 256);
	bool first = true, second = true;
	for (int y = 0; y < 16; y++)
	{
		for (int x = 0; x < 16; x++)
		{
			first = first && icon[0][(size_t)(y * 16 + x)] == expected[(x + y) & 15];
			second = second && icon[1][(size_t)(y * 16 + x)] == expected[15 - x];
		}
	}
	CHECK(first);
	CHECK(second);
	// The left pixel is the low half of the byte.
	CHECK(icon[0][0] == expected[0] && icon[0][1] == expected[1]);
	CHECK(icon[1][0] == expected[15] && icon[1][1] == expected[14]);

	const uint8_t flags[] = {0x11, 0x12, 0x13, 0x00, 0x16};
	const size_t frames[] = {1, 2, 3, 1, 1};
	for (int i = 0; i < 5; i++)
	{
		Card other = memcard::blank();
		CHECK(importBytes(other, mcsFile("BASLUS-00004", saveData(bytesOf("FLAG"), 1, 42, flags[i]))));
		CHECK(saveAt(other, 0).icon.size() == frames[i]);
	}
}

// Fifteen saves of one block, then holes at 2, 5, 9 and 14.
Card fragmentedCard()
{
	Card card = memcard::blank();
	for (int i = 1; i <= 15; i++)
		CHECK(addSave(card, "BASLUS-000" + std::to_string(10 + i) + "FILL", 1, 50 + (uint32_t)i));
	CHECK(card.freeBlocks == 0 && card.saves.size() == 15);
	checkHealthy(card);
	for (const int block : {2, 5, 9, 14})
		CHECK(memcard::remove(card, block));
	return card;
}

void testRemove()
{
	Card card = fragmentedCard();
	checkHealthy(card);
	CHECK(card.freeBlocks == 4 && card.saves.size() == 11);
	// A deleted frame keeps its name and its link; only the state changes.
	for (const int block : {2, 5, 9, 14})
	{
		const uint8_t *frame = dirFrame(card, block);
		CHECK(frame[0] == 0xA1 && le32(frame + 4) == 0x2000 && le16(frame + 8) == 0xFFFF);
		CHECK(memcmp(frame + 0x0A, "BASLUS-000", 10) == 0);
		CHECK(findSave(card, std::string(reinterpret_cast<const char *>(frame + 0x0A))) == nullptr);
	}
	// The blocks themselves are left as they were.
	CHECK(blocksData(card, {2}) == saveData(bytesOf("TEST SAVE"), 1, 52));

	const Card before = card;
	CHECK(!memcard::remove(card, 2));		// free already
	CHECK(!memcard::remove(card, 0));
	CHECK(!memcard::remove(card, -3));
	CHECK(!memcard::remove(card, 16));
	CHECK(!memcard::remove(card, 1000000));
	CHECK(card.data == before.data);

	// A chain: first, middle and last become the three deleted states.
	Card chained = memcard::blank();
	CHECK(addSave(chained, "BASLUS-00100A", 1, 61));
	CHECK(addSave(chained, "BASLUS-00100B", 4, 62));
	CHECK(addSave(chained, "BASLUS-00100C", 2, 63));
	const std::vector<uint8_t> blocksBefore(chained.data.begin() + (long)BlockSize, chained.data.end());
	CHECK(!memcard::remove(chained, 3));	// a middle block is not a save
	CHECK(!memcard::remove(chained, 5));	// nor is a last one
	CHECK(memcard::remove(chained, 2));
	checkHealthy(chained);
	CHECK(dirFrame(chained, 2)[0] == 0xA1 && dirFrame(chained, 3)[0] == 0xA2 && dirFrame(chained, 4)[0] == 0xA2
			&& dirFrame(chained, 5)[0] == 0xA3);
	CHECK(chained.freeBlocks == 12 && chained.saves.size() == 2);
	CHECK(saveAt(chained, 0).firstBlock == 1 && saveAt(chained, 1).firstBlock == 6 && saveAt(chained, 1).blocks == 2);
	CHECK(std::vector<uint8_t>(chained.data.begin() + (long)BlockSize, chained.data.end()) == blocksBefore);
	// Deleted blocks are free ones: the next save takes the lowest of them.
	CHECK(addSave(chained, "BASLUS-00100D", 5, 64));
	checkHealthy(chained);
	CHECK(walk(chained, 2) == std::vector<int>({2, 3, 4, 5, 8}));
	CHECK(chained.freeBlocks == 7);
}

void testFragmented()
{
	Card card = fragmentedCard();
	const std::vector<uint8_t> three = saveData(bytesOf("SPREAD OUT"), 3, 71);
	CHECK(importBytes(card, mcsFile("BASLUS-00200SPREAD", three)));
	checkHealthy(card);
	CHECK(card.freeBlocks == 1);
	const Save *entry = findSave(card, "BASLUS-00200SPREAD");
	CHECK(entry != nullptr && entry->firstBlock == 2 && entry->blocks == 3);
	CHECK(walk(card, 2) == std::vector<int>({2, 5, 9}));
	CHECK(dirFrame(card, 2)[0] == 0x51 && le16(dirFrame(card, 2) + 8) == 4 && le32(dirFrame(card, 2) + 4) == 0x6000);
	CHECK(dirFrame(card, 5)[0] == 0x52 && le16(dirFrame(card, 5) + 8) == 8);
	CHECK(dirFrame(card, 9)[0] == 0x53 && le16(dirFrame(card, 9) + 8) == 0xFFFF);
	// The middle and last frames lose what the deleted saves left in them.
	CHECK(dirFrame(card, 5)[0x0A] == 0 && dirFrame(card, 9)[0x0A] == 0);
	CHECK(blocksData(card, {2, 5, 9}) == three);
	// The neighbours are untouched.
	for (const int block : {1, 3, 4, 6, 7, 8, 10, 11, 12, 13, 15})
		CHECK(blocksData(card, {block}) == saveData(bytesOf("TEST SAVE"), 1, 50 + (uint32_t)block));

	// One block left: a save of two does not fit, and nothing is written.
	const Card before = card;
	std::string error;
	CHECK(!importBytes(card, mcsFile("BASLUS-00201TWO", saveData(bytesOf("TWO"), 2, 72)), ".mcs", &error));
	CHECK(!error.empty());
	CHECK(card.data == before.data && card.freeBlocks == 1);
	CHECK(addSave(card, "BASLUS-00202ONE", 1, 73));
	checkHealthy(card);
	CHECK(card.freeBlocks == 0 && found(card, "BASLUS-00202ONE").firstBlock == 14);
}

void testCopy()
{
	Card from = memcard::blank();
	CHECK(addSave(from, "BASLUS-00300ONE", 1, 81, "ONE"));
	CHECK(addSave(from, "BASLUS-00300FOUR", 4, 82, "FOUR"));
	const Card fromBefore = from;

	// Into the holes of a fragmented card: 2, 5, 9, 14.
	Card to = fragmentedCard();
	std::string error = "untouched";
	CHECK(memcard::copy(from, 2, to, &error));
	CHECK_TEXT(error, "untouched");
	checkHealthy(to);
	CHECK(from.data == fromBefore.data);
	CHECK(to.freeBlocks == 0);
	const Save *copied = findSave(to, "BASLUS-00300FOUR");
	CHECK(copied != nullptr && copied->firstBlock == 2 && copied->blocks == 4);
	if (copied != nullptr)
		CHECK_TEXT(copied->title, "FOUR");
	CHECK(walk(to, 2) == std::vector<int>({2, 5, 9, 14}));
	CHECK(blocksData(to, walk(to, 2)) == blocksData(from, walk(from, 2)));

	// No room for the other one.
	const Card toBefore = to;
	CHECK(!memcard::copy(from, 1, to, &error));
	CHECK(error != "untouched" && !error.empty());
	CHECK(to.data == toBefore.data);
	// Without anywhere to put the reason, too.
	CHECK(!memcard::copy(from, 1, to));

	// The same save twice, and onto its own card.
	Card other = memcard::blank();
	CHECK(memcard::copy(from, 1, other, &error));
	CHECK(memcard::copy(from, 2, other, &error));
	checkHealthy(other);
	CHECK(other.saves.size() == 2 && other.freeBlocks == 10);
	const Card otherBefore = other;
	error.clear();
	CHECK(!memcard::copy(from, 2, other, &error));
	CHECK(error.find("BASLUS-00300FOUR") != std::string::npos);
	CHECK(other.data == otherBefore.data);
	CHECK(!memcard::copy(from, 1, from, &error));
	CHECK(from.data == fromBefore.data);

	// What is not a save's first block.
	for (const int block : {0, 3, 5, 6, 15, 16, -1})
		CHECK(!memcard::copy(from, block, other, &error));
	CHECK(other.data == otherBefore.data);

	// After the original is deleted on the other card it can be copied again.
	CHECK(memcard::remove(other, 2));
	CHECK(memcard::copy(from, 2, other, &error));
	checkHealthy(other);
	CHECK(blocksData(other, walk(other, 2)) == blocksData(from, walk(from, 2)));
}

void testExportImport()
{
	Card card = fragmentedCard();
	const std::vector<uint8_t> three = saveData(bytesOf("ROUND TRIP"), 3, 91, 0x12);
	CHECK(importBytes(card, mcsFile("BESLES-00400TRIP", three)));
	const std::string path = inWork("trip.mcs");
	CHECK(memcard::exportSave(card, 2, path));
	const std::vector<uint8_t> file = readBytes(path);
	CHECK(file.size() == 128 + 3 * BlockSize);
	if (file.size() != 128 + 3 * BlockSize)
		return;
	CHECK(file[0] == 0x51 && le32(file.data() + 4) == 0x6000);
	CHECK(memcmp(file.data() + 0x0A, "BESLES-00400TRIP", 17) == 0);
	CHECK(file[127] == xorOf(file.data()));
	CHECK(std::vector<uint8_t>(file.begin() + 128, file.end()) == three);
	// The frame is the card's own.
	CHECK(memcmp(file.data(), dirFrame(card, 2), 128) == 0);

	Card other = memcard::blank();
	CHECK(addSave(other, "BASLUS-00401FIRST", 1, 92));
	CHECK(memcard::importSave(other, path));
	checkHealthy(other);
	CHECK(other.saves.size() == 2);
	CHECK(walk(other, 2) == std::vector<int>({2, 3, 4}));
	CHECK(blocksData(other, {2, 3, 4}) == three);
	CHECK(blocksData(other, {2, 3, 4}) == blocksData(card, {2, 5, 9}));
	if (other.saves.size() == 2)
	{
		CHECK_TEXT(saveAt(other, 1).title, "ROUND TRIP");
		CHECK_TEXT(saveAt(other, 1).serial, "SLES-00400");
		CHECK(saveAt(other, 1).icon == found(card, "BESLES-00400TRIP").icon);
	}
	// Out again: the same file but for the link to the next block.
	const std::string pathAgain = inWork("trip-again.mcs");
	CHECK(memcard::exportSave(other, 2, pathAgain));
	std::vector<uint8_t> again = readBytes(pathAgain);
	CHECK(again.size() == file.size());
	CHECK(std::vector<uint8_t>(again.begin() + 128, again.end()) == three);
	CHECK(le16(file.data() + 8) == 4 && le16(again.data() + 8) == 2);

	// One block, and all of a full card's saves through files.
	Card full = memcard::blank();
	const Card source = fragmentedCard();
	for (const Save& entry : source.saves)
	{
		const std::string one = inWork("one.mcs");
		CHECK(memcard::exportSave(source, entry.firstBlock, one));
		CHECK(readBytes(one).size() == 128 + BlockSize);
		CHECK(memcard::importSave(full, one));
		CHECK(blocksData(full, {lastSave(full).firstBlock}) == blocksData(source, {entry.firstBlock}));
	}
	checkHealthy(full);
	CHECK(full.saves.size() == 11);

	// Not a save's first block; a folder that is not there.
	CHECK(!memcard::exportSave(card, 5, inWork("no.mcs")));
	CHECK(!memcard::exportSave(card, 14, inWork("no.mcs")));
	CHECK(!memcard::exportSave(card, 0, inWork("no.mcs")));
	CHECK(!memcard::exportSave(card, 99, inWork("no.mcs")));
	CHECK(readBytes(inWork("no.mcs")).empty());
	CHECK(!memcard::exportSave(card, 2, inWork("no-such-folder/x.mcs")));
}

void testSingleSaveFormats()
{
	const std::vector<uint8_t> two = saveData(bytesOf("OTHER TOOLS"), 2, 101);
	std::string error;

	for (const char *extension : {".psx", ".mcb", ".mcx", ".pda"})
	{
		Card card = memcard::blank();
		CHECK(importBytes(card, arFile("BASLUS-00500AR", two), extension, &error));
		checkHealthy(card);
		CHECK(card.saves.size() == 1 && saveAt(card, 0).blocks == 2);
		CHECK_TEXT(saveAt(card, 0).fileName, "BASLUS-00500AR");
		CHECK_TEXT(saveAt(card, 0).title, "OTHER TOOLS");
		CHECK(blocksData(card, {1, 2}) == two);
	}
	{
		Card card = memcard::blank();
		CHECK(importBytes(card, psvFile("BESLES-00501PSV", two), ".psv", &error));
		checkHealthy(card);
		CHECK(card.saves.size() == 1 && saveAt(card, 0).blocks == 2);
		CHECK_TEXT(saveAt(card, 0).fileName, "BESLES-00501PSV");
		CHECK_TEXT(saveAt(card, 0).serial, "SLES-00501");
		CHECK(blocksData(card, {1, 2}) == two);
		// The extension does not decide: the content does.
		CHECK(importBytes(card, arFile("BASLUS-00502", two), ".psv", &error));
		CHECK(importBytes(card, mcsFile("BASLUS-00503", two), ".psx", &error));
		CHECK(importBytes(card, psvFile("BASLUS-00504", two), ".mcs", &error));
		checkHealthy(card);
		CHECK(card.saves.size() == 4 && card.freeBlocks == 7);
	}

	// What is refused: each leaves the card as it was and says why.
	Card card = memcard::blank();
	CHECK(addSave(card, "BASLUS-00510", 1, 102));
	const Card before = card;
	const auto refused = [&](const std::vector<uint8_t>& file, const char *extension)
	{
		error.clear();
		const bool ok = importBytes(card, file, extension, &error);
		return !ok && !error.empty() && card.data == before.data && card.saves.size() == 1;
	};
	CHECK(refused(psvFile("BASLUS-00511", two, 2), ".psv"));			// a PS2 save
	std::vector<uint8_t> file = psvFile("BASLUS-00511", two);
	file[0x84] = 'X';
	CHECK(refused(file, ".psv"));										// no title frame
	file = psvFile("BASLUS-00511", two);
	file.pop_back();
	CHECK(refused(file, ".psv"));										// not whole blocks
	file = arFile("BASLUS-00512", two);
	file[54] = 0;
	CHECK(refused(file, ".psx"));										// no title frame
	CHECK(refused(arFile("", two), ".psx"));								// no name
	CHECK(refused(arFile("BAD\x01NAME", two), ".psx"));
	file = mcsFile("BASLUS-00513", two);
	file[0] = 0xA0;
	CHECK(refused(file, ".mcs"));										// not a first block's frame
	file = mcsFile("BASLUS-00513", two);
	file.resize(file.size() - 100);
	CHECK(refused(file, ".mcs"));
	CHECK(refused(mcsFile("", two), ".mcs"));
	CHECK(refused(mcsFile("BASLUS-00513", {}), ".mcs"));					// a frame and no block
	CHECK(refused(mcsFile("BASLUS-00513", saveData(bytesOf("16"), 16, 103)), ".mcs"));
	CHECK(refused({}, ".mcs"));
	CHECK(refused(std::vector<uint8_t>(100, 0x51), ".mcs"));
	CHECK(refused(std::vector<uint8_t>(BlockSize, 'S'), ".mcs"));
	CHECK(refused(std::vector<uint8_t>(4 * 1024 * 1024, 0x51), ".mcs"));	// far too large
	CHECK(refused(card.data, ".mcd"));									// a whole card
	CHECK(refused(mcsFile("BASLUS-00510", two), ".mcs"));				// there already
	CHECK(error.find("BASLUS-00510") != std::string::npos);
	error.clear();
	CHECK(!memcard::importSave(card, inWork("not-there.mcs"), &error) && !error.empty());
	CHECK(!memcard::importSave(card, inWork("not-there.mcs")));
	CHECK(!memcard::importSave(card, workDir, &error));					// a folder
	CHECK(card.data == before.data);
}

std::vector<uint8_t> wrapped(const Card& card, size_t header, const char *magic, size_t magicLength)
{
	std::vector<uint8_t> file(header, 0x5A);
	if (magicLength != 0)
		memcpy(file.data(), magic, magicLength);
	file.insert(file.end(), card.data.begin(), card.data.end());
	return file;
}

void testContainers()
{
	Card card = memcard::blank();
	CHECK(addSave(card, "BASLUS-00600A", 2, 111, "IN A CONTAINER"));
	CHECK(addSave(card, "BESLES-00600B", 1, 112));

	struct Case
	{
		const char *fileName;
		size_t header;
		const char *magic;
		size_t magicLength;
	};
	const Case cases[] = {
		{"card.mcd", 0, "", 0},
		{"card.mcr", 0, "", 0},
		{"card.srm", 0, "", 0},
		{"card.bin", 0, "", 0},
		{"card.gme", 3904, "123-456-STD", 11},
		{"card.vgs", 64, "VgsM", 4},
		{"card.mem", 64, "VgsM", 4},
		{"card.vmp", 128, "\0PMV\x80", 5},
		// The size alone is enough when the mark is not what is expected.
		{"plain.gme", 3904, "", 0},
		{"plain.vgs", 64, "", 0},
		{"plain.vmp", 128, "", 0},
	};
	for (const Case& one : cases)
	{
		const std::vector<uint8_t> file = wrapped(card, one.header, one.magic, one.magicLength);
		Card parsed;
		std::string error = "untouched";
		CHECK(memcard::parse(file, parsed, &error));
		CHECK_TEXT(error, "untouched");
		CHECK(parsed.data == card.data);
		CHECK(parsed.saves.size() == 2 && parsed.freeBlocks == 12);
		checkHealthy(parsed);

		const std::string path = inWork(one.fileName);
		CHECK(writeBytes(path, file));
		Card loaded;
		CHECK(memcard::load(path, loaded, &error));
		CHECK(loaded.data == card.data && loaded.saves.size() == 2);
		if (loaded.saves.size() == 2)
			CHECK_TEXT(saveAt(loaded, 0).title, "IN A CONTAINER");
		CHECK(memcard::kindOf(path, file.size()) == 1);
		CHECK(memcard::kindOf("/a/b/" + lowercase(one.fileName), file.size()) == 1);
	}

	// A marked container with something after the card is still one.
	{
		std::vector<uint8_t> file = wrapped(card, 64, "VgsM", 4);
		file.resize(file.size() + 512);
		Card parsed;
		CHECK(memcard::parse(file, parsed) && parsed.data == card.data);
	}
	// A card's own bytes handed back to it.
	{
		Card same = card;
		CHECK(memcard::parse(same.data, same) && same.data == card.data && same.saves.size() == 2);
	}

	// What is not a card.
	const auto refused = [](const std::vector<uint8_t>& file)
	{
		Card parsed;
		std::string error;
		const bool ok = memcard::parse(file, parsed, &error);
		Card silent;
		return !ok && !error.empty() && !memcard::parse(file, silent);
	};
	CHECK(refused({}));
	CHECK(refused({'M', 'C'}));
	CHECK(refused(std::vector<uint8_t>(CardSize, 0)));				// never formatted
	CHECK(refused(std::vector<uint8_t>(CardSize, 0xFF)));
	CHECK(refused(std::vector<uint8_t>(card.data.begin(), card.data.end() - 1)));
	CHECK(refused(wrapped(card, 100, "", 0)));						// no known header is 100 bytes
	{
		std::vector<uint8_t> file = wrapped(card, 3904, "123-456-STD", 11);
		file.resize(100000);										// cut short
		CHECK(refused(file));
	}
	{
		std::vector<uint8_t> file = wrapped(card, 64, "VgsM", 4);
		file[64] = 'X';												// the card inside is not one
		CHECK(refused(file));
	}

	std::string error;
	Card none;
	CHECK(!memcard::load(inWork("not-there.mcd"), none, &error) && !error.empty());
	CHECK(!memcard::load(inWork("not-there.mcd"), none));
	CHECK(!memcard::load(workDir, none, &error));
	// A large file that begins like a card is not read as one.
	std::vector<uint8_t> large(3 * 1024 * 1024);
	memcpy(large.data(), card.data.data(), CardSize);
	CHECK(writeBytes(inWork("large.bin"), large));
	CHECK(!memcard::load(inWork("large.bin"), none, &error));
	CHECK(memcard::kindOf(inWork("large.bin"), large.size()) == 0);
}

void testSave()
{
	Card card = memcard::blank();
	CHECK(addSave(card, "BASLUS-00700", 3, 121));
	const std::string path = inWork("written.mcd");
	CHECK(writeBytes(path, std::vector<uint8_t>(10, 1)));	// over a file that is there
	CHECK(memcard::save(path, card));
	CHECK(readBytes(path) == card.data);
	Card loaded;
	CHECK(memcard::load(path, loaded));
	CHECK(loaded.data == card.data && loaded.saves.size() == 1 && loaded.freeBlocks == 12);
	// A card read from a container is written as the plain image.
	const std::string gme = inWork("to-raw.gme");
	CHECK(writeBytes(gme, wrapped(card, 3904, "123-456-STD", 11)));
	CHECK(memcard::load(gme, loaded));
	CHECK(memcard::save(inWork("from-gme.mcd"), loaded));
	CHECK(readBytes(inWork("from-gme.mcd")) == card.data);

	Card broken = card;
	broken.data.resize(1000);
	CHECK(!memcard::save(inWork("broken.mcd"), broken));
	CHECK(readBytes(inWork("broken.mcd")).empty());
	CHECK(!memcard::save(inWork("no-such-folder/x.mcd"), card));
}

void testKindOf()
{
	const uint64_t card = CardSize;
	for (const char *name : {"a.mcd", "a.mcr", "a.mc", "a.mci", "a.srm", "a.ps", "a.ddf", "a.psm", "a.bin", "a.mem",
			"a.vgs", "/data/x/Card 1.MCD", "epsxe000.McR"})
		CHECK(memcard::kindOf(name, card) == 1);
	CHECK(memcard::kindOf("a.gme", card + 3904) == 1);
	CHECK(memcard::kindOf("a.vgs", card + 64) == 1);
	CHECK(memcard::kindOf("a.mem", card + 64) == 1);
	CHECK(memcard::kindOf("a.vmp", card + 128) == 1);
	CHECK(memcard::kindOf("a.bin", card + 64) == 0);			// .bin only of exactly a card's size
	CHECK(memcard::kindOf("a.bin", 700ull * 1024 * 1024) == 0);
	CHECK(memcard::kindOf("a.mcd", 8ull * 1024 * 1024) == 0);	// a PS2 card
	CHECK(memcard::kindOf("a.mcd", card - 1) == 0);
	CHECK(memcard::kindOf("a.mcd", 0) == 0);
	CHECK(memcard::kindOf("a.srm", 32768) == 0);
	CHECK(memcard::kindOf("a.txt", card) == 0);
	CHECK(memcard::kindOf("mcd", card) == 0);
	CHECK(memcard::kindOf("", card) == 0);
	CHECK(memcard::kindOf("a.mcd.bak", card) == 0);
	CHECK(memcard::kindOf("a.gme", 0xFFFFFFFFFFFFFFFFull) == 0);

	for (uint64_t blocks = 1; blocks <= 15; blocks++)
	{
		CHECK(memcard::kindOf("a.mcs", 128 + blocks * BlockSize) == 2);
		CHECK(memcard::kindOf("a.psx", 54 + blocks * BlockSize) == 2);
		CHECK(memcard::kindOf("a.mcb", 54 + blocks * BlockSize) == 2);
		CHECK(memcard::kindOf("a.mcx", 54 + blocks * BlockSize) == 2);
		CHECK(memcard::kindOf("a.pda", 54 + blocks * BlockSize) == 2);
		CHECK(memcard::kindOf("A.PSV", 0x84 + blocks * BlockSize) == 2);
	}
	CHECK(memcard::kindOf("a.mcs", 128) == 0);
	CHECK(memcard::kindOf("a.mcs", 128 + 16 * BlockSize) == 0);
	CHECK(memcard::kindOf("a.mcs", 54 + BlockSize) == 0);
	CHECK(memcard::kindOf("a.psx", 128 + BlockSize) == 0);
	CHECK(memcard::kindOf("a.psv", 128 + BlockSize) == 0);
	CHECK(memcard::kindOf("a.psv", 0x84 + BlockSize + 1) == 0);
	CHECK(memcard::kindOf("a.mcs", 0) == 0);
	CHECK(memcard::kindOf("a.mcs", 0xFFFFFFFFFFFFFFFFull) == 0);
}

void setFrame(Card& card, int block, uint8_t state, unsigned next)
{
	uint8_t *frame = dirFrame(card, block);
	frame[0] = state;
	frame[8] = (uint8_t)next;
	frame[9] = (uint8_t)(next >> 8);
	frame[127] = xorOf(frame);
}

// What every card must give, however damaged.
void checkListing(const Card& card)
{
	int previous = 0, usedBlocks = 0;
	for (const Save& entry : card.saves)
	{
		CHECK(entry.firstBlock > previous && entry.firstBlock <= 15);
		previous = entry.firstBlock;
		CHECK(entry.blocks >= 1 && entry.blocks <= 15);
		usedBlocks += entry.blocks;
		CHECK(entry.fileName.size() <= 20);
		bool text = true;
		for (const char c : entry.fileName)
			text = text && c >= 0x20 && c <= 0x7E;
		CHECK(text);
		CHECK(entry.serial.empty() || entry.serial.size() == 10);
		CHECK(entry.region == 0 || entry.region == 'A' || entry.region == 'E' || entry.region == 'I');
		// The title: ASCII and the middle dot, nothing at its end.
		const std::string& title = entry.title;
		bool shown = true;
		for (size_t i = 0; i < title.size(); i++)
		{
			const unsigned char c = (unsigned char)title[i];
			if (c == 0xC2 && i + 1 < title.size() && (unsigned char)title[i + 1] == 0xB7)
				i++;
			else
				shown = shown && c >= 0x20 && c <= 0x7E;
		}
		CHECK(shown);
		// (A save listed by its file name has whatever that ends with.)
		CHECK(title.empty() || title.back() != ' ' || title == entry.fileName);
		CHECK(entry.icon.size() <= 3);
		for (const auto& frame : entry.icon)
			CHECK(frame.size() == 256);
	}
	CHECK(card.freeBlocks >= 0 && usedBlocks + card.freeBlocks <= 15);
}

void testDamaged()
{
	// A chain that comes back to itself.
	Card card = memcard::blank();
	CHECK(addSave(card, "BASLUS-00800LOOP", 3, 131));
	setFrame(card, 2, 0x52, 1);		// block 2 points at block 2
	memcard::refresh(card);
	checkListing(card);
	CHECK(card.saves.size() == 1 && saveAt(card, 0).blocks == 2 && card.freeBlocks == 12);
	setFrame(card, 2, 0x52, 2);
	setFrame(card, 3, 0x52, 1);		// 1 -> 2 -> 3 -> 2
	memcard::refresh(card);
	checkListing(card);
	CHECK(card.saves.size() == 1 && saveAt(card, 0).blocks == 3);
	setFrame(card, 3, 0x52, 0);		// 1 -> 2 -> 3 -> 1: the first block is not a middle one
	memcard::refresh(card);
	CHECK(card.saves.size() == 1 && saveAt(card, 0).blocks == 3);
	// It can still be copied, exported and deleted as the three blocks it has.
	Card to = memcard::blank();
	CHECK(memcard::copy(card, 1, to));
	checkHealthy(to);
	CHECK(to.saves.size() == 1 && saveAt(to, 0).blocks == 3 && blocksData(to, {1, 2, 3}) == blocksData(card, {1, 2, 3}));
	CHECK(memcard::exportSave(card, 1, inWork("loop.mcs")));
	CHECK(readBytes(inWork("loop.mcs")).size() == 128 + 3 * BlockSize);
	CHECK(memcard::remove(card, 1));
	CHECK(card.saves.empty() && card.freeBlocks == 15);

	// Links that leave the card.
	for (const unsigned next : {15u, 16u, 0x7FFFu, 0x8000u, 0xFFFEu, 0x0100u})
	{
		card = memcard::blank();
		CHECK(addSave(card, "BASLUS-00801OUT", 2, 132));
		setFrame(card, 1, 0x51, next);
		memcard::refresh(card);
		checkListing(card);
		CHECK(card.saves.size() == 1 && saveAt(card, 0).blocks == 1);
		CHECK(card.freeBlocks == 13);		// block 2 is used by nothing, but it is not free
	}

	// Two saves that claim the same block: it goes to the first.
	card = memcard::blank();
	CHECK(addSave(card, "BASLUS-00802A", 2, 133));
	CHECK(addSave(card, "BASLUS-00802B", 2, 134));
	setFrame(card, 3, 0x51, 1);			// B's first block points at A's second
	memcard::refresh(card);
	checkListing(card);
	CHECK(card.saves.size() == 2 && saveAt(card, 0).blocks == 2 && saveAt(card, 1).blocks == 1);
	CHECK(memcard::remove(card, 3));
	CHECK(card.saves.size() == 1 && saveAt(card, 0).blocks == 2 && dirFrame(card, 2)[0] == 0x53);

	// A chain that runs into a free block or into another save's first block.
	card = memcard::blank();
	CHECK(addSave(card, "BASLUS-00803A", 1, 135));
	CHECK(addSave(card, "BASLUS-00803B", 1, 136));
	setFrame(card, 1, 0x51, 1);
	memcard::refresh(card);
	CHECK(card.saves.size() == 2 && saveAt(card, 0).blocks == 1 && saveAt(card, 1).blocks == 1);
	setFrame(card, 1, 0x51, 9);
	memcard::refresh(card);
	CHECK(card.saves.size() == 2 && saveAt(card, 0).blocks == 1 && card.freeBlocks == 13);

	// Middle and last blocks that belong to nothing are not listed, and not
	// given to a new save.
	card = memcard::blank();
	setFrame(card, 1, 0x52, 1);
	setFrame(card, 2, 0x53, 0xFFFF);
	memcard::refresh(card);
	checkListing(card);
	CHECK(card.saves.empty() && card.freeBlocks == 13);
	CHECK(addSave(card, "BASLUS-00804", 1, 137));
	CHECK(card.saves.size() == 1 && saveAt(card, 0).firstBlock == 3);

	// A name of bytes that are not text is still shown as text.
	card = memcard::blank();
	CHECK(addSave(card, "BASLUS-00805", 1, 138));
	dirFrame(card, 1)[0x0A + 3] = 0x9C;
	dirFrame(card, 1)[0x0A + 4] = 0x07;
	memcard::refresh(card);
	checkListing(card);
	CHECK_TEXT(saveAt(card, 0).fileName, "BAS??S-00805");
	// And it is copied as the bytes it is.
	to = memcard::blank();
	CHECK(memcard::copy(card, 1, to));
	CHECK(memcmp(dirFrame(to, 1) + 0x0A, dirFrame(card, 1) + 0x0A, 21) == 0);
	CHECK(!memcard::copy(card, 1, to));

	// A card whose bytes are not a card's.
	for (const size_t size : {(size_t)0, (size_t)1, (size_t)127, (size_t)8192, CardSize - 1, CardSize + 1})
	{
		Card wrong = memcard::blank();
		CHECK(addSave(wrong, "BASLUS-00806", 1, 139));
		wrong.data.resize(size);
		memcard::refresh(wrong);
		CHECK(wrong.saves.empty() && wrong.freeBlocks == 0);
		std::string error;
		Card good = memcard::blank();
		CHECK(addSave(good, "BASLUS-00807", 1, 140));
		const Card goodBefore = good;
		CHECK(!memcard::remove(wrong, 1));
		CHECK(!memcard::copy(wrong, 1, good, &error) && !error.empty());
		error.clear();
		CHECK(!memcard::copy(good, 1, wrong, &error) && !error.empty());
		CHECK(!memcard::exportSave(wrong, 1, inWork("wrong.mcs")));
		CHECK(!memcard::save(inWork("wrong.mcd"), wrong));
		CHECK(!importBytes(wrong, mcsFile("BASLUS-00808", saveData(bytesOf("X"), 1, 141)), ".mcs", &error));
		CHECK(wrong.data.size() == size && good.data == goodBefore.data);
	}
}

// The header block damaged in three ways, twenty thousand times: bytes at
// random, a directory of plausible states and links, and all of it noise.
// Whatever is listed must then be within the card, and can be exported,
// copied to an empty card and deleted.
void testFuzz()
{
	Card base = memcard::blank();
	CHECK(addSave(base, "BASLUS-00900A", 1, 151));
	CHECK(addSave(base, "BASLUS-00900B", 3, 152));
	CHECK(addSave(base, "BESLES-00900C", 2, 153));
	CHECK(addSave(base, "BISLPS-00900D", 1, 154));
	CHECK(addSave(base, "BASLUS-00900E", 4, 155));
	CHECK(memcard::remove(base, 5));
	CHECK(addSave(base, "BASLUS-00900F", 3, 156));	// 5, 6, 12
	CHECK(memcard::remove(base, 7));
	checkHealthy(base);

	const uint8_t states[] = {0x51, 0x51, 0x52, 0x52, 0x53, 0x53, 0xA0, 0xA1, 0xA2, 0xA3, 0x00, 0xFF, 0x50, 0x54};
	Random random(20260);
	Card card = base;
	const Card blankCard = memcard::blank();
	int listed = 0, exported = 0, copied = 0, removed = 0, mostBlocks = 0;
	for (int round = 0; round < 20000; round++)
	{
		memcpy(card.data.data(), base.data.data(), BlockSize);
		const uint32_t way = random.below(10);
		if (way < 3)
		{
			const uint32_t count = 1 + random.below(way == 0 ? 4 : 64);
			for (uint32_t i = 0; i < count; i++)
				card.data[random.below((uint32_t)BlockSize)] = (uint8_t)random.next();
		}
		else if (way < 5)
		{
			// The bytes that matter most: states and links.
			const uint32_t count = 1 + random.below(12);
			for (uint32_t i = 0; i < count; i++)
			{
				uint8_t *frame = dirFrame(card, 1 + (int)random.below(15));
				const uint32_t what = random.below(3);
				if (what == 0)
					frame[0] = states[random.below(sizeof(states))];
				else if (what == 1)
				{
					frame[8] = (uint8_t)random.below(17);
					frame[9] = random.below(8) == 0 ? (uint8_t)random.next() : 0;
				}
				else
					frame[random.below(128)] = (uint8_t)random.next();
			}
		}
		else if (way < 7)
		{
			// A sound directory of chains scattered over the card, then a few
			// frames of it changed.
			int order[15];
			for (int i = 0; i < 15; i++)
				order[i] = i + 1;
			for (int i = 14; i > 0; i--)
			{
				const int other = (int)random.below((uint32_t)i + 1), kept = order[i];
				order[i] = order[other];
				order[other] = kept;
			}
			for (int at = 0; at < 15;)
			{
				const int length = 1 + (int)random.below(9);
				const bool unused = random.below(4) == 0;
				for (int i = 0; i < length && at < 15; i++, at++)
				{
					const bool last = i == length - 1 || at == 14;
					setFrame(card, order[at], unused ? 0xA0 : i == 0 ? 0x51 : last ? 0x53 : 0x52,
							unused || last ? 0xFFFF : (unsigned)(order[at + 1] - 1));
				}
			}
			const uint32_t changes = random.below(4);
			for (uint32_t i = 0; i < changes; i++)
			{
				const int block = 1 + (int)random.below(15);
				if (random.below(2) == 0)
					dirFrame(card, block)[0] = states[random.below(sizeof(states))];
				else
					dirFrame(card, block)[8] = (uint8_t)random.below(17);
			}
		}
		else if (way < 9)
		{
			for (int block = 1; block <= 15; block++)
			{
				const uint32_t link = random.below(20);
				setFrame(card, block, states[random.below(sizeof(states))], link < 16 ? link : 0xFFFF);
				if (random.below(4) == 0)
					dirFrame(card, block)[0x0A + random.below(21)] = (uint8_t)random.next();
			}
		}
		else
		{
			for (size_t i = 0; i < BlockSize; i++)
				card.data[i] = (uint8_t)random.next();
		}

		memcard::refresh(card);
		checkListing(card);
		listed += (int)card.saves.size();
		const std::vector<Save> saves = card.saves;
		const int freeBefore = card.freeBlocks;

		// Whatever the links say, a chain only goes on into middle and last
		// blocks.
		for (const Save& entry : saves)
		{
			CHECK(dirFrame(card, entry.firstBlock)[0] == 0x51);
			mostBlocks = entry.blocks > mostBlocks ? entry.blocks : mostBlocks;
		}

		Card to = blankCard;
		int toBlocks = 0;
		for (const Save& entry : saves)
		{
			if (round % 64 == 0)
			{
				const std::string path = inWork("fuzz.mcs");
				CHECK(memcard::exportSave(card, entry.firstBlock, path));
				const std::vector<uint8_t> file = readBytes(path);
				CHECK(file.size() == 128 + (size_t)entry.blocks * BlockSize);
				CHECK(file.size() > 128 + BlockSize - 1 && memcmp(file.data() + 128,
						card.data.data() + (size_t)entry.firstBlock * BlockSize, BlockSize) == 0);
				exported++;
			}
			std::string error;
			const bool ok = memcard::copy(card, entry.firstBlock, to, &error);
			if (ok)
			{
				copied++;
				CHECK((int)to.saves.size() >= 1 && lastSave(to).firstBlock == toBlocks + 1
						&& lastSave(to).blocks == entry.blocks);
				CHECK(memcmp(to.data.data() + (size_t)(toBlocks + 1) * BlockSize,
						card.data.data() + (size_t)entry.firstBlock * BlockSize, BlockSize) == 0);
				toBlocks += entry.blocks;
			}
			else
				// Only a name that is there already can stop it: the saves of
				// one card always fit an empty one.
				CHECK(!error.empty() && toBlocks + entry.blocks <= 15);
		}
		if (round % 16 == 0)
			checkHealthy(to);

		// Deleting one frees its blocks and leaves the others as they were.
		Card cut = card;
		for (size_t i = 0; i < saves.size(); i++)
		{
			CHECK(memcard::remove(cut, saves[i].firstBlock));
			removed++;
			CHECK(cut.saves.size() == saves.size() - i - 1);
			int freed = 0;
			for (size_t k = 0; k <= i; k++)
				freed += saves[k].blocks;
			CHECK(cut.freeBlocks == freeBefore + freed);
			for (size_t k = 0; k < cut.saves.size(); k++)
				CHECK(cut.saves[k].firstBlock == saves[i + 1 + k].firstBlock && cut.saves[k].blocks == saves[i + 1 + k].blocks);
			for (int block = 1; block <= 15; block++)
				CHECK(dirFrame(cut, block)[127] == xorOf(dirFrame(cut, block)) || dirFrame(cut, block)[0] == dirFrame(card, block)[0]);
		}
		checkListing(cut);
		// The blocks themselves are never written by any of this.
		CHECK(memcmp(cut.data.data() + BlockSize, base.data.data() + BlockSize, CardSize - BlockSize) == 0);
	}
	printf("  fuzz: 20000 damaged headers, %d saves listed (the longest of %d blocks), %d exported, %d copied, %d deleted\n",
			listed, mostBlocks, exported, copied, removed);
	CHECK(listed > 20000 && mostBlocks >= 6);

	// Damaged title frames: whatever the bytes, the title is text and the
	// icon is of the right size.
	Random titles(777);
	card = base;
	for (int round = 0; round < 4000; round++)
	{
		for (const Save& entry : base.saves)
		{
			uint8_t *block = card.data.data() + (size_t)entry.firstBlock * BlockSize;
			const uint32_t way = titles.below(4);
			for (size_t i = (way == 0 ? 2 : 4); i < 0x44; i++)
			{
				// Mostly bytes that begin or end a two-byte character.
				const uint32_t pick = titles.below(8);
				block[i] = pick == 0 ? (uint8_t)titles.next() : pick < 4 ? (uint8_t)(0x81 + titles.below(3))
						: pick < 7 ? (uint8_t)(0x40 + titles.below(0xBD)) : (uint8_t)' ';
			}
			if (way == 1)
				block[4 + titles.below(64)] = 0;
		}
		memcard::refresh(card);
		checkListing(card);
		CHECK(card.saves.size() == base.saves.size());
	}

	// Files of noise in every form a card or a save comes in.
	Random files(4242);
	const size_t sizes[] = {0, 1, 53, 54, 127, 128, 0x84, 54 + BlockSize, 128 + BlockSize, 0x84 + BlockSize,
			128 + 15 * BlockSize, CardSize - 1, CardSize, CardSize + 64, CardSize + 128, CardSize + 3904, CardSize + 5000};
	const char *const marks[] = {"MC", "VgsM", "123-456-STD", "Q", "SC"};
	for (int round = 0; round < 600; round++)
	{
		std::vector<uint8_t> file(sizes[files.below(sizeof(sizes) / sizeof(sizes[0]))]);
		const uint32_t way = files.below(4);
		for (uint8_t& byte : file)
			byte = way == 0 ? 0 : (uint8_t)files.next();
		if (files.below(2) == 0)
		{
			const char *mark = marks[files.below(5)];
			if (file.size() >= strlen(mark))
				memcpy(file.data(), mark, strlen(mark));
		}
		else if (file.size() >= 4 && files.below(2) == 0)
			memcpy(file.data(), files.below(2) == 0 ? "\0PMV" : "\0VSP", 4);
		for (const size_t at : {(size_t)54, (size_t)64, (size_t)128, (size_t)0x84, (size_t)3904})
		{
			if (files.below(3) == 0 && file.size() >= at + 2)
				memcpy(file.data() + at, files.below(2) == 0 ? "MC" : "SC", 2);
		}
		if (file.size() > 0x3C && files.below(2) == 0)
			file[0x3C] = 1;
		Card parsed;
		if (memcard::parse(file, parsed))
		{
			CHECK(parsed.data.size() == CardSize && parsed.data[0] == 'M' && parsed.data[1] == 'C');
			checkListing(parsed);
		}
		Card target = memcard::blank();
		if (importBytes(target, file, ".mcs"))
		{
			CHECK(target.saves.size() == 1);
			checkHealthy(target);
		}
		else
			CHECK(target.data == blankCard.data);
	}
}

}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("usage: %s <a folder for the test's files>\n", argv[0]);
		return 2;
	}
	workDir = argv[1];

	struct Test
	{
		const char *name;
		void (*run)();
	};
	const Test tests[] = {
		{"blank card", testBlank},
		{"import and listing", testImportAndList},
		{"file names, serials, regions", testNames},
		{"titles", testTitles},
		{"icons and palette", testIcons},
		{"delete", testRemove},
		{"fragmented free space", testFragmented},
		{"copy between cards", testCopy},
		{"export and import", testExportImport},
		{"single-save files", testSingleSaveFormats},
		{"card containers", testContainers},
		{"writing a card", testSave},
		{"kindOf", testKindOf},
		{"damaged cards", testDamaged},
		{"fuzz", testFuzz},
	};
	for (const Test& test : tests)
	{
		const int checksBefore = checks, failuresBefore = failures;
		test.run();
		printf("%-30s %7d checks  %s\n", test.name, checks - checksBefore, failures == failuresBefore ? "ok" : "FAILED");
	}
	printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
