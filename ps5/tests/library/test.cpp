// PSSwanStation - the library's disc grouping rules (src/library_names.h), on a PC.
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/library_names.h"

#include <cstdio>

namespace
{
int failures = 0;

void disc(const char *title, int number, const char *base)
{
	std::string got;
	const int n = fe::library::names::discNumber(title, got);
	const bool ok = n == number && (number == 0 || got == base);
	printf("%s \"%s\" -> %d \"%s\"\n", ok ? "ok  " : "FAIL", title, n, n ? got.c_str() : "");
	failures += ok ? 0 : 1;
}

void folder(const char *path, const char *want)
{
	const std::string got = fe::library::names::groupFolder(path);
	const bool ok = got == want;
	printf("%s folder \"%s\" -> \"%s\"\n", ok ? "ok  " : "FAIL", path, got.c_str());
	failures += ok ? 0 : 1;
}
}

int main()
{
	// Dump sets' names, as before.
	disc("Final Fantasy VII (USA) (Disc 2)", 2, "Final Fantasy VII (USA)");
	disc("Game (Europe) (Disc 1) (Rev 1)", 1, "Game (Europe) (Rev 1)");
	disc("Game (USA) (Disc 3 of 4)", 3, "Game (USA)");
	disc("Game [CD 2]", 2, "Game");
	disc("Game (Disc B)", 2, "Game");
	disc("Game (Disc Two)", 2, "Game");
	// Other sets' names: the disc at the end.
	disc("Final Fantasy VIII CD2", 2, "Final Fantasy VIII");
	disc("Metal Gear Solid - Disc 1", 1, "Metal Gear Solid");
	disc("Parasite Eve_disk2", 2, "Parasite Eve");
	disc("Game cd 4", 4, "Game");
	// Not discs.
	disc("Tekken 3 (USA)", 0, "");
	disc("Gran Turismo 2 (USA) (Arcade Disc)", 0, "");
	disc("Discworld", 0, "");
	disc("CD2", 0, "");
	disc("Rayman CDi", 0, "");
	disc("Abcd2", 0, "");
	disc("Game (Disc 0)", 0, "");
	// The folder a disc groups by.
	folder("/data/games/Final Fantasy VII (USA) (Disc 2)/", "/data/games/");
	folder("/data/games/Metal Gear Solid CD1/", "/data/games/");
	folder("/data/games/RPGs/", "/data/games/RPGs/");
	folder("smb://nas/Games/PSX/Game (Disc 1)/", "smb://nas/Games/PSX/");
	folder("/", "/");
	folder("/data/games/Final Fantasy IX/CD3/", "/data/games/Final Fantasy IX/");
	const bool cd = fe::library::names::folderDisc("/g/Game/CD1/") == 1 && fe::library::names::folderDisc("/g/Game/Disc 12/") == 12
			&& fe::library::names::folderDisc("/g/Game/disk_3/") == 3 && fe::library::names::folderDisc("/g/CDs/") == 0
			&& fe::library::names::folderDisc("/g/Discworld/") == 0 && fe::library::names::folderDisc("/g/cd123/") == 0;
	printf("%s folders named after a disc's number\n", cd ? "ok  " : "FAIL");
	failures += cd ? 0 : 1;
	printf("%s\n", failures == 0 ? "all passed" : "FAILED");
	return failures == 0 ? 0 : 1;
}
