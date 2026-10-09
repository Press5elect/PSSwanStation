/*
	PSSwanStation - libretro shader presets brought in from a USB drive and
	compiled on the console (slangimport.cpp).

	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include <string>
#include <vector>

namespace fe::slangimport
{

// A preset found: its name (the file's, made safe) and where it is.
struct Found
{
	std::string name, path;
	bool imported = false;	// one of that name is brought in already
};
// The .slangp files in PSSwanStation/shaders on each USB drive and in
// <root>shaders, a few folders deep.
std::vector<Found> find();

// Brings one in, on a thread of its own: compiled into <root>data/shaders/<name>/,
// which the picture tube's list then offers. False when one is already being
// brought in.
bool start(const Found& preset);
bool working();
// What it is doing, or what it did last ("" before the first).
std::string status();

// The presets brought in, by name, and the folder each is in.
std::vector<std::string> imported();
std::string folder(const std::string& name);
void remove(const std::string& name);
// Counts the changes to imported().
unsigned generation();

// For the PC tests: brings one in now, on this thread.
bool importNow(const std::string& presetPath, const std::string& name, std::string& error);

}
