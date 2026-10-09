/*
	PSSwanStation - a speedrun timer with splits (speedrun.cpp).

	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fe::speedrun
{

struct Split
{
	std::string name;
	// When it splits by itself: "0x8007A3B4 u8 == 3" (an address in the main
	// memory, the value's size, a comparison, a number); empty: by hand.
	std::string when;
	int64_t best = -1;		// the personal best's time at this split, in ms from the start
	int64_t gold = -1;		// the fastest this segment was ever done, in ms
	int64_t time = -1;		// this run's time at it, or -1
};

enum State { Idle, Running, Finished };

// The game's splits (<root>data/splits/<serial>.txt): read when a game
// starts, written when they change.
void loadFor(const std::string& serial, const std::string& title);
void unload();
const std::string& file();

// Once an emulated frame: game time, and the conditions that start and split.
void frame(double fps);
// By hand (the shortcuts, the page): start, or split; take the last split
// back; pass a split by; stop and start again.
void startOrSplit();
void undo();
void skip();
void reset();
// Something that a run does not allow happened (a state was loaded, rewind,
// fast forward): this run sets no record.
void markPractice(const char *why);

State state();
// The time to show, in ms.
int64_t elapsed();
// The split being run (== splits().size() when finished).
size_t current();
bool practice();
const std::string& practiceReason();
std::vector<Split>& splits();

// Editing the splits (saved at once).
void addSplit(const std::string& name);
void removeSplit(size_t index);
void renameSplit(size_t index, const std::string& name);
void setWhen(size_t index, const std::string& condition);
const std::string& startWhen();
void setStartWhen(const std::string& condition);
void forgetBest();
// Whether a condition can be read, and whether it holds now.
bool validCondition(const std::string& condition);
bool holds(const std::string& condition);

// "1:23.45" (or "1:02:03.45").
std::string timeText(int64_t ms);
// "+1.23" or "-0.45".
std::string deltaText(int64_t ms);

}
