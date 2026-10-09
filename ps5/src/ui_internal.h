/*
	PSSwanStation - what the interface's files share (ui.cpp, ui_more.cpp).

	SPDX-License-Identifier: GPL-3.0-or-later

	ui.cpp has the page stack, the lists every menu page is made of, the
	library, the settings and a game's details; ui_more.cpp has the pages that
	came later (memory cards, achievements, netplay, updates, the text
	keyboard and others) and what is drawn over a running game.
*/
#pragma once

#include "ui.h"

#include <functional>
#include <string>
#include <vector>

namespace fe::ui
{

// ------------------------------------------------------------------- input

struct Input
{
	uint32_t held = 0, pressed = 0, repeat = 0;
};
extern Input in;
extern uint32_t confirmButton, cancelButton;
bool hit(uint32_t button);
bool nav(uint32_t button);
void consumeInput();
// When a button was last pressed, by the animations' clock.
extern double lastInputAt;

// ------------------------------------------------------------------- pages

enum class Page
{
	MainMenu,
	Settings,	// a: category, b: 1 for the running game's own values
	Pause,
	States,		// a: 0 save, 1 load
	Discs,
	Cheats,
	Details,	// the game in `chosen`
	Launch,		// the swan sees the game in `chosen` off; a: state slot, b: disc
	Loading,	// a network game being read; a: state slot
	Search,		// a game looked for by its name
	Message,	// s: title, s2: text
	Confirm,	// s: title, s2: text, action
	// ui_more.cpp
	LibraryOptions,	// how the library is sorted and what it shows
	Cover,			// a cover chosen for the game the details are of
	Cards,			// the memory cards
	Card,			// one card (s: its file); a: 1 when it is one to bring in
	CardPick,		// which card a save is copied to (s: from, a: the save's block)
	Buttons,		// which button of the pad is which of the PlayStation's
	Achievements,	// the running game's
	Account,		// the RetroAchievements account
	Netplay,
	Text,			// the keyboard: s: what is asked for, s2: the text so far, a: 1 hides it, b: 1 numbers and dots
	Update,
	SafeStart,
	Shortcuts,		// what the shortcuts are
	Memory,			// values found in the game's memory, and watched
};

struct Frame
{
	Page page;
	int a = 0, b = 0;
	std::string s, s2;
	std::function<void()> action;
	int cursor = 0;
	float scroll = 0, scrollTarget = 0;
	bool fresh = true;
	int picker = -1, pickCursor = 0;
	float pickScroll = 0;
	double opened = 0;
};

extern std::vector<Frame> stack;
// What a page asked for; done after the frame is drawn, when no reference
// into the stack or a page's list is alive.
extern std::function<void()> deferred;
// Closing the game takes its picture away, and the frame being drawn still
// shows it: that waits for the start of the next frame.
extern bool closeGame;
extern bool quit;
extern library::Game chosen;

void push(Page page, int a = 0, int b = 0, const std::string& s = "", const std::string& s2 = "",
		std::function<void()> action = {});
void pop();
// A page that says something went wrong, and one that only says something.
void message(const std::string& title, const std::string& text);
void inform(const std::string& title, const std::string& text);
// Back to the game, from any depth of menus.
void resume();

// Logical size of the screen, in units (1920 x 1080 at the normal size).
float unitsWide();
float unitsHigh();
ImVec2 at(float x, float y);
float toUnits(float pixels);
// How high wrapped body text is, in units.
float fontsHeight(const std::string& value, float wrapUnits, float size = 23);
float smooth(float x);
float glide(float x);

// ------------------------------------------------------------------- lists

struct Item
{
	std::string label, value, info;
	const char *icon = nullptr;
	bool enabled = true, header = false;
	int mark = 0;		// 1: the game's own value, 2: held by a patch
	bool ticked = false;
	// One of several: Left/Right steps through them, confirm opens the list
	// (or flips, when there are two).
	std::vector<std::string> choices;
	int current = -1;
	std::function<void(int)> choose;
	// A number: Left/Right steps it.
	std::function<void(int)> adjust;
	std::function<void()> activate;
	std::function<void()> alt;		// Square
	std::string altHint;
	std::string confirmHint;
	// A small picture at the row's left (a save's icon, an achievement's badge).
	Image picture;
	// The choices are things to do with the row, not values of it: confirm
	// opens them as a list, and Left and Right leave them alone.
	bool menu = false;
};

Item header(const std::string& label);
Item action(const char *icon, const std::string& label, const std::string& info, std::function<void()> activate,
		bool enabled = true);
Item fact(const std::string& label, const std::string& value, const std::string& info = "");
// A switch of the frontend's settings (saved when it is changed).
Item toggle(const std::string& label, bool *value, const std::string& info, std::function<void()> changed = {});
Item choice(const std::string& label, int current, std::vector<std::string> names, const std::string& info,
		std::function<void(int)> set);

// Draws a list in the box and works it with the pad. The item under the
// cursor, or null.
const Item *runList(Frame& f, std::vector<Item>& items, float x0, float y0, float x1, float y1);
// A page that is one list at the left with an explanation at the right.
const Item *menuPage(Frame& f, const std::string& title, const std::string& subtitle, std::vector<Item>& items,
		float listWidth = 780);
void standardHints(const Item *focused, const char *confirm = "Select");
// Which build this is, in the top right corner.
void buildTag(float alpha = 1.f);

// ------------------------------------------------------------- the library

// The game the details page is of.
const library::Game& detailsGame();
const std::string& detailsSerial_();
// Starts a game. `slot`: a state to start from, -1 none; `disc`: the one in
// the tray, -1 the one last played.
void launch(const library::Game& game, int slot, int disc = -1);
void openDetails(const library::Game& game);
void openSearch();
void scanEverything();
std::string agoText(int64_t when);
// The library's lists are made anew (sorting, filters, favourites changed).
void libraryChanged();

// ----------------------------------------------------------- ui_flow.cpp

// The games a view in space shows, as the library has them.
struct FlowGames
{
	int count = 0;
	std::function<Image(int)> cover;
	std::function<const library::Game&(int)> game;
	// One line about a game (its region, year, kind), and what is known of it.
	std::function<std::string(int)> facts, about;
};
// The views in space: this title's own, then the Aurora layout files of
// <root>layouts. The library's "View" setting is 2 and up for these.
std::vector<std::string> flowNames();
// Every view the library has, as the "view" setting counts them: the grid,
// the list, then those.
std::vector<std::string> libraryViewNames();
bool flowIsOwn(int index);
// The layouts folder is read again when the names are next asked for.
void flowRescan();
// Draws view `index` between `top` and `bottom` (in units) and, when `active`,
// moves the cursor with the pad. False when there is no such view.
bool flowView(int index, const FlowGames& games, int& cursor, bool active, bool fresh, float top, float bottom);

// ----------------------------------------------------------- ui_more.cpp

void libraryOptionsPage(Frame& f);
void coverPage(Frame& f);
void cardsPage(Frame& f);
void cardPage(Frame& f);
void cardPickPage(Frame& f);
void buttonsPage(Frame& f);
void achievementsPage(Frame& f);
void accountPage(Frame& f);
void netplayPage(Frame& f);
void textPage(Frame& f);
void updatePage(Frame& f);
void safeStartPage(Frame& f);
void shortcutsPage(Frame& f);
void memoryPage(Frame& f);

// The keyboard: asks for a text and hands it to `done` when it is confirmed.
void askText(const std::string& title, const std::string& initial, bool hidden, bool address,
		std::function<void(const std::string&)> done);

// Over a running game, with no menu open: the shortcuts (OPTIONS held with
// another button), which take the pads from the game while they are pressed.
// True when OPTIONS was tapped by itself: the menu is to open.
bool gameShortcuts();
// What is drawn over the game: the scanlines and the border round it are
// drawGame's (ui.cpp); this is the speed and rewind marks, a light gun's
// aim, netplay's state.
void gameMarks();
// A notice with a heading and a picture (an achievement), bottom left.
// Returns its height in units.
float drawNotice(const host::Message& message, float y);
// Where the game's picture is on the screen, in pixels (drawGame sets it).
extern ImVec2 gameA, gameB;
// OPTIONS closed a menu and is still held: until it is let go it opens none.
extern bool optionsSpent;
// The picture's surroundings and the scanlines over it: `p0`..`p1` is where
// the picture is, in pixels.
void drawBorder(ImDrawList *list, void *texture, float u, float v, ImVec2 p0, ImVec2 p1);
void drawScanlines(ImDrawList *list, ImVec2 p0, ImVec2 p1);

// The swan takes the screen when nothing was pressed for a while. True while
// it has it (nothing else is drawn or worked).
bool idleSwan(bool allowed);
// What must be said once the library is first on the screen: files that are
// kept outside and cannot be reached, a start made safely, an update.
void startNotices();
// The settings rows of the frontend's later pages.
void moreSettings(int kind, std::vector<Item>& items);
// The Picture page's rows: for every game, or the loaded game's own.
void pictureItems(bool forGame, std::vector<Item>& items);
// Rows for a game's details ("More"): favourite, hidden, cover, texture pack.
void detailsMoreItems(std::vector<Item>& items);
// Rows the pause menu gains.
void pauseMoreItems(std::vector<Item>& items);

}
