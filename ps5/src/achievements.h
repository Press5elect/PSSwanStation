/*
	PSSwanStation - RetroAchievements, through the rcheevos library.

	SPDX-License-Identifier: GPL-3.0-or-later

	rcheevos' rc_client does the work: it logs in, identifies the disc by the
	hash RetroAchievements defines for PlayStation games, fetches the game's
	achievements and leaderboards, watches the emulated memory every frame and
	reports what was earned. This module is what rc_client needs around it
	(memory, the disc, HTTP on threads of its own, events) and a plain view of
	its state for the interface. It knows nothing of the emulator: host.cpp
	gives it the hooks.
*/
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fe::achievements
{

struct Hooks
{
	// Reads emulated memory at a RetroAchievements address for the PlayStation:
	// 0x000000-0x1FFFFF is the main RAM, 0x200000-0x2003FF the scratchpad.
	// Returns how many bytes were read.
	std::function<uint32_t(uint32_t address, uint8_t *buffer, uint32_t bytes)> readMemory;
	// The disc, for the hash: opens the image (a path the emulator can open,
	// network paths included), reads the 2048 bytes of user data of a sector
	// of its first data track by absolute sector number, and closes it.
	// Called on one of this module's own threads, while the emulator may be
	// reading the same image: the handle is this module's alone.
	std::function<void *(const std::string& path)> openDisc;
	std::function<bool(void *disc, uint32_t sector, uint8_t *out2048)> readSector;
	std::function<void(void *disc)> closeDisc;
	// One HTTP(S) GET (platform::httpGet): the status code or -1. Called on
	// this module's own threads, never on the caller's.
	std::function<int(const std::string& url, std::vector<uint8_t>& out, unsigned seconds)> httpGet;
	// The emulator must be reset (hardcore mode was switched on while a game ran).
	std::function<void()> reset;
};

// What happened, for the screen: each is shown once as a notice.
struct Event
{
	enum Kind
	{
		LoggedIn,			// title: the user's name; text: their points
		LoginFailed,		// text: why
		GameLoaded,			// title: the game's name; text: "12 of 40 achievements"
		GameUnknown,		// text: the disc is not in RetroAchievements' database
		Unlocked,			// title, text: the achievement's; points; badge
		Progress,			// title: achievement; text: "12/50"
		Challenge,			// a challenge indicator came on (title: the achievement's)
		LeaderboardStarted,	// title: the leaderboard's
		LeaderboardFailed,
		LeaderboardSubmitted,	// title; text: the score and the rank when the server said it
		Completed,			// title: the game's name; text: every achievement earned / mastered
		ServerError,		// text
		Disconnected,		// an unlock is waiting for the network
		Reconnected,
	} kind;
	std::string title, text;
	int points = 0;
	std::string badge;		// a file with the achievement's picture, or empty (not fetched yet)
};

struct Achievement
{
	uint32_t id = 0;
	std::string title, description;
	int points = 0;
	bool unlocked = false;
	bool unofficial = false;
	std::string progress;		// "12/50" when it is measured, else empty
	float progressPercent = 0;	// 0..100
	std::string badge;			// a file (in <cache>/badges), or empty until it has been fetched
	std::string unlockedWhen;	// "2026-10-04", or empty
};

struct Leaderboard
{
	uint32_t id = 0;
	std::string title, description;
	std::string best;			// the user's best as text, or empty
	bool tracking = false;		// an attempt is running
	std::string value;			// its current value while tracking
};

struct Summary
{
	bool loggedIn = false, loggingIn = false;
	std::string user;
	int userPoints = 0;
	bool gameLoaded = false, gameLoading = false;
	std::string game;				// its name at RetroAchievements
	uint32_t gameId = 0;
	int unlocked = 0, total = 0, points = 0, totalPoints = 0;
	bool hardcore = false;
	std::string richPresence;		// what the game says the player is doing
	std::string lastError;
};

// `cacheDir` (with a trailing '/') is where badges are kept. Call once.
void init(const Hooks& hooks, const std::string& cacheDir);
void shutdown();

// Logging in: with a password the first time (the token the server gives is
// then what is kept: token()), with the token afterwards. Both return at once;
// the result comes as an event and in summary().
void loginWithPassword(const std::string& user, const std::string& password);
void loginWithToken(const std::string& user, const std::string& token);
void logout();
std::string token();

// Hardcore mode: no save states loaded, no cheats, no rewind, no slow motion.
// The frontend enforces that (it asks hardcore()); switching it on while a
// game runs resets the game through the hook.
void setHardcore(bool on);
bool hardcore();
// Whether unofficial achievements (ones still being tested, which the server
// never records) are loaded too. Read when a game is loaded: a change shows
// from the next game on.
void setUnofficial(bool on);

// A game started or ended; `discPath` is the image in the tray.
void gameStarted(const std::string& discPath);
void gameStopped();
// Another disc of the running game went into the tray: the game stays loaded
// and the disc is checked (an unknown disc ends hardcore mode).
void discChanged(const std::string& discPath);
// The emulator was reset, or a state was loaded.
void gameReset();
// Once per emulated frame, on the emulator's thread, after the frame ran.
void frame();
// Once per display refresh while a game is paused or a menu is open (keeps
// the connection's timers going without running achievements).
void idle();

// For save states: achievement progress (hit counts) travels with a state.
// loadProgress() is all a loaded state needs: without data, or with data that
// does not fit, it starts the counts afresh as gameReset() does. A
// gameReset() after it would undo it.
std::vector<uint8_t> saveProgress();
void loadProgress(const uint8_t *data, size_t size);

Summary summary();
std::vector<Achievement> list();
std::vector<Leaderboard> leaderboards();
// The events since the last call, oldest first.
std::vector<Event> takeEvents();

}
