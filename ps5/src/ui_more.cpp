/*
	PSSwanStation - the interface's later pages, and what is drawn over a game.

	SPDX-License-Identifier: GPL-3.0-or-later

	The pages here are made of the same parts as ui.cpp's (ui_internal.h): a
	list worked with the pad, a panel, hints along the bottom. They are the
	keyboard, how the library is sorted and what it shows, choosing a cover,
	the memory cards, the button map, RetroAchievements, netplay, the updater,
	the safe start and the list of shortcuts.

	Over a running game this file has the shortcuts (OPTIONS held with another
	button), the marks for fast forward and rewind, a light gun's aim, the
	notices with a picture, the scanlines and what fills the screen beside a
	picture that does not.
*/
#include "ui_internal.h"
#include "achievements.h"
#include "display.h"
#include "memcard.h"
#include "netplay.h"
#include "update.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace fe::ui
{

using namespace platform;

namespace
{

// A list in a panel in the middle of the screen, over whatever is behind.
const Item *panelList(Frame& f, const std::string& title, std::vector<Item>& items, float panelW, float rowsHigh,
		const std::string& note = "")
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const float noteH = note.empty() ? 0 : fontsHeight(note, panelW - 80, 21) + 22;
	const float panelH = 96 + rowsHigh + noteH + 22;
	const float x0 = (W - panelW) * 0.5f, y0 = std::max((H - panelH) * 0.5f, 70.f);
	draw()->AddRectFilled(ImVec2(0, 0), ImVec2(width(), height()), IM_COL32(4, 6, 12, 150));
	panel(at(x0, y0), at(x0 + panelW, y0 + panelH), IM_COL32(24, 30, 48, 252), 20);
	outline(at(x0, y0), at(x0 + panelW, y0 + panelH), IM_COL32(255, 255, 255, 24), 20, 1.5f);
	textFit(at(x0 + 36, y0 + 28), px(panelW - 72), t.text, title, Bold, 32);
	// (A row's list of choices, while it is open, is over this panel: the note
	// is not drawn across it.)
	const bool choosing = f.picker >= 0;
	const Item *focused = runList(f, items, x0 + 14, y0 + 84, x0 + panelW - 22, y0 + 84 + rowsHigh);
	if (!note.empty() && !choosing)
		textWrapped(at(x0 + 40, y0 + 96 + rowsHigh), px(panelW - 80), t.faint, note, Body, 21);
	return focused;
}

std::vector<std::string> filesIn(const std::string& folder)
{
	std::vector<std::string> names;
	if (DIR *dir = opendir(folder.c_str()))
	{
		while (const dirent *entry = readdir(dir))
			if (entry->d_name[0] != '.')
				names.push_back(entry->d_name);
		closedir(dir);
	}
	std::sort(names.begin(), names.end());
	return names;
}

uint64_t fileSize(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? (uint64_t)st.st_size : 0;
}

// Text from elsewhere (a release's notes) without what the fonts cannot draw:
// the pictures of four bytes (emoji) go, with the space before them.
std::string drawable(const std::string& text)
{
	std::string out;
	for (size_t i = 0; i < text.size();)
	{
		const unsigned char c = (unsigned char)text[i];
		const size_t length = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
		if (length == 4)
		{
			if (!out.empty() && out.back() == ' ')
				out.pop_back();
		}
		else
			out.append(text, i, length);
		i += length;
	}
	return out;
}

std::string dayAndTime(time_t when)
{
	char text[64];
	const time_t local = when + localTimeOffset();
	struct tm tm;
	gmtime_r(&local, &tm);
	strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &tm);
	return text;
}

}

// ------------------------------------------------------------- the keyboard

namespace
{
struct TextState
{
	std::string text;
	int row = 1, column = 0;
	int layer = 0;				// 0 small letters, 1 capitals, 2 signs
	std::function<void(const std::string&)> done;
	double typedAt = -10;		// when the last letter was typed (a hidden text shows it for a moment)
} textState;

constexpr int KeyColumns = 10, KeyRows = 4;
const char *const keyLayers[3][KeyRows] = {
	{ "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@" },
	{ "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._@" },
	{ "1234567890", "!#$%&*()+=", "[]{};:'\"/\\", "<>,?|~^`-_" },
};
// For an address: the digits, the dot and the colon.
const char *const addressKeys[KeyRows] = { "123", "456", "789", ".0:" };
}

void askText(const std::string& title, const std::string& initial, bool hidden, bool address,
		std::function<void(const std::string&)> done)
{
	textState = TextState();
	textState.text = initial;
	textState.done = std::move(done);
	push(Page::Text, hidden ? 1 : 0, address ? 1 : 0, title);
}

void textPage(Frame& f)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	TextState& s = textState;
	const bool hidden = f.a != 0, address = f.b != 0;
	const int columns = address ? 3 : KeyColumns;
	const float key = 84, gap = 8;
	const float kw = KeyColumns * key + (KeyColumns - 1) * gap;
	const float x0 = (W - kw) * 0.5f, y0 = 150;

	text(at(x0, y0 - 70), t.text, f.s, Title, 40);
	// What was typed. A hidden text is dots, but for the letter just typed.
	panel(at(x0, y0), at(x0 + kw, y0 + 76), t.panel, 14);
	outline(at(x0, y0), at(x0 + kw, y0 + 76), withAlpha(t.accent, 0.7f), 14, 2);
	std::string shown = s.text;
	if (hidden)
	{
		shown.clear();
		for (size_t i = 0; i < s.text.size(); i++)
			shown += i + 1 == s.text.size() && clock() - s.typedAt < 0.9 ? std::string(1, s.text[i]) : std::string("\xe2\x80\xa2");
	}
	const float end = textFit(at(x0 + 24, y0 + 20), px(kw - 60), t.text, shown, Bold, 30);
	if (motion() == MotionOff || std::fmod(clock(), 1.0) < 0.6)
		draw()->AddRectFilled(ImVec2(px(x0 + 24) + end + px(3), px(y0 + 20)), ImVec2(px(x0 + 24) + end + px(6), px(y0 + 56)),
				t.accent);

	// The keys.
	const float keysY = y0 + 104;
	const float keysX = address ? x0 + (kw - (3 * key + 2 * gap)) * 0.5f : x0;
	s.row = std::clamp(s.row, 0, KeyRows);
	const int actionRow = KeyRows;
	// The row under the keys: what changes the letters, a space, delete, done.
	static const char *const actions[5] = { "Capitals", "Signs", "Space", "Delete", "Done" };
	const int actionCount = address ? 2 : 5;
	const float actionW = (kw - (actionCount - 1) * gap) / actionCount;
	for (int r = 0; r < KeyRows; r++)
		for (int c = 0; c < columns; c++)
		{
			const float x = keysX + (float)c * (key + gap), y = keysY + (float)r * (key - 10 + gap);
			const bool focused = s.row == r && s.column == c;
			panel(at(x, y), at(x + key, y + key - 10), focused ? t.accent : t.panelHigh, 12);
			const char letter = address ? addressKeys[r][c] : keyLayers[s.layer][r][c];
			textCentred(at(x + key * 0.5f, y + 18), focused ? IM_COL32(8, 12, 22, 255) : t.text, std::string(1, letter), Bold, 30);
		}
	for (int c = 0; c < actionCount; c++)
	{
		const float x = x0 + (float)c * (actionW + gap), y = keysY + (float)actionRow * (key - 10 + gap);
		const int which = address ? c + 3 : c;
		const bool focused = s.row == actionRow && std::clamp(s.column * actionCount / columns, 0, actionCount - 1) == c;
		const bool lit = (which == 0 && s.layer == 1) || (which == 1 && s.layer == 2);
		panel(at(x, y), at(x + actionW, y + key - 10), focused ? t.accent : lit ? t.accentSoft : t.panelHigh, 12);
		textCentred(at(x + actionW * 0.5f, y + 22), focused ? IM_COL32(8, 12, 22, 255) : t.text, actions[which], Bold, 24);
	}

	// The pad.
	const auto type = [&](char letter) {
		if (s.text.size() < 64)
		{
			s.text.push_back(letter);
			s.typedAt = clock();
			sound::play(sound::Key);
		}
	};
	const auto erase = [&] {
		if (!s.text.empty())
		{
			s.text.pop_back();
			sound::play(sound::Key);
		}
	};
	const auto finish = [&] {
		const std::string result = s.text;
		const std::function<void(const std::string&)> done = s.done;
		deferred = [result, done] {
			pop();
			if (done)
				done(result);
		};
	};
	if (nav(Left) && s.column > 0)
		s.column--;
	if (nav(Right) && s.column + 1 < columns)
		s.column++;
	if (nav(Down) && s.row < actionRow)
		s.row++;
	if (nav(Up) && s.row > 0)
		s.row--;
	s.column = std::clamp(s.column, 0, columns - 1);
	if (hit(confirmButton))
	{
		if (s.row < actionRow)
			type(address ? addressKeys[s.row][s.column] : keyLayers[s.layer][s.row][s.column]);
		else
		{
			const int which = std::clamp(s.column * actionCount / columns, 0, actionCount - 1) + (address ? 3 : 0);
			if (which == 0)
				s.layer = s.layer == 1 ? 0 : 1;
			else if (which == 1)
				s.layer = s.layer == 2 ? 0 : 2;
			else if (which == 2)
				type(' ');
			else if (which == 3)
				erase();
			else
				finish();
		}
	}
	else if (hit(Square))
		erase();
	else if (hit(Triangle) && !address)
		type(' ');
	else if (hit(L1) && !address)
		s.layer = s.layer == 1 ? 0 : 1;
	else if (hit(R1) && !address)
		s.layer = s.layer == 2 ? 0 : 2;
	else if (hit(Options))
		finish();
	else if (hit(cancelButton))
	{
		deferred = [] { pop(); };
		consumeInput();
	}
	std::vector<Hint> hints = { { confirmButton, "Type" }, { Square, "Delete" } };
	if (!address)
	{
		hints.push_back({ Triangle, "Space" });
		hints.push_back({ L1, "Capitals" });
		hints.push_back({ R1, "Signs" });
	}
	hints.push_back({ Options, "Done" });
	hints.push_back({ cancelButton, "Cancel" });
	hintBar(hints);
}

// ---------------------------------------------------- the library's options

void libraryOptionsPage(Frame& f)
{
	options::Frontend& o = options::frontend();
	std::vector<Item> items;
	items.push_back(choice("Sort by", o.sort, { "Name", "Last played", "Most played", "Year", "Size" },
			"", [](int i) {
				options::frontend().sort = i;
				libraryChanged();
			}));
	items.push_back(choice("Show", o.filter, { "All games", "Favourites", "Not played yet", "Hidden games" },
			"", [](int i) {
				options::frontend().filter = i;
				libraryChanged();
			}));
	items.push_back(choice("Region", o.regionFilter, { "All", "USA", "Europe", "Japan" },
			"", [](int i) {
				options::frontend().regionFilter = i;
				libraryChanged();
			}));
	{
		// The grid, the list, and the views in space: this title's own and
		// any Aurora layout files in the layouts folder.
		if (f.fresh)
			flowRescan();
		const std::vector<std::string> names = libraryViewNames();
		Item item = choice("View", std::min(o.view, (int)names.size() - 1), names, "",
				[](int i) { options::frontend().view = i; });
		items.push_back(item);
	}
	items.push_back(action(icon::Sync, "Scan for games", "", [] {
		scanEverything();
		pop();
	}));
	const Item *focused = panelList(f, "Sort and filter", items, 760, 5 * 60 + 10,
			"A game becomes a favourite, or is hidden, in its details (Triangle), under More. Hidden games are out of "
			"the library and the search until \"Hidden games\" is shown here. The views after the list stand the "
			"covers in space, as the Xbox 360's Aurora does; an Aurora layout file (.cfljson) put in " + shownRoot()
			+ "layouts is offered here too.");
	standardHints(focused);
}

// --------------------------------------------------------- choosing a cover

void coverPage(Frame& f)
{
	const library::Game game = detailsGame();
	std::vector<Item> items;
	std::string note;
	static const char *const kinds[covers::KindCount] = { "The box", "The title screen", "A moment of the game" };
	const covers::ChooseState state = covers::chooseState();
	for (int kind = 0; kind < covers::KindCount; kind++)
	{
		Item item = action(icon::Download, kinds[kind], "", [game, kind] { covers::choose(game, kind); },
				state != covers::ChooseWorking && httpAvailable());
		item.confirmHint = "Fetch";
		items.push_back(item);
	}
	items.push_back(action(icon::Undo, "Back to the automatic cover", "", [game] {
		covers::remove(game);
		pop();
	}));
	// What the fetching came to, said once it was asked for from this page.
	static bool asked;
	if (f.fresh)
		asked = false;
	if (state == covers::ChooseWorking)
		asked = true;
	note = !httpAvailable() ? "The console is not on a network: pictures cannot be fetched."
			: state == covers::ChooseWorking ? "Fetching the picture\xe2\x80\xa6"
			: !asked ? "The three pictures are from the libretro thumbnails collection, by the game's file name. A "
			"picture of your own goes in the covers folder as <file name>.png or .jpg."
			: state == covers::ChooseDone ? "The picture is the game's cover now."
			: state == covers::ChooseNotFound ? "The collection has no such picture for this game."
			: "The collection did not answer.";
	const Item *focused = panelList(f, "Cover", items, 760, 4 * 60 + 10, note);
	standardHints(focused);
}

// ------------------------------------------------------------ memory cards

namespace
{
// A card read from its file, kept while the manager looks at it.
struct OpenCard
{
	std::string path;
	memcard::Card card;
	bool ok = false;
	std::string error;
	time_t modified = 0;
};
std::map<std::string, OpenCard> openCards;

OpenCard& openCard(const std::string& path)
{
	OpenCard& entry = openCards[path];
	struct stat st{};
	const bool exists = stat(path.c_str(), &st) == 0;
	if (entry.path.empty() || entry.modified != st.st_mtime)
	{
		entry.path = path;
		entry.modified = exists ? st.st_mtime : 0;
		entry.error.clear();
		entry.ok = memcard::load(path, entry.card, &entry.error);
	}
	return entry;
}

std::string savesDir()
{
	return rootDir + "data/saves/";
}

std::string importDir()
{
	return rootDir + "memcards/import/";
}

std::string exportDir()
{
	return rootDir + "memcards/export/";
}

// The emulator's cards: one for each game that saved, named after the game.
std::vector<std::string> cardFiles()
{
	std::vector<std::string> cards;
	for (const std::string& name : filesIn(savesDir()))
		if (extension(name) == ".mcd")
			cards.push_back(name);
	return cards;
}

// "Some Game (Europe)_1.mcd" is "Some Game (Europe)", in slot 1.
std::string cardLabel(const std::string& file)
{
	std::string name = fileTitle(file);
	if (name.size() > 2 && name[name.size() - 2] == '_' && (name.back() == '1' || name.back() == '2'))
		name = name.substr(0, name.size() - 2) + (name.back() == '2' ? "  (slot 2)" : "");
	return name;
}

std::string cardSummary(const OpenCard& open)
{
	if (!open.ok)
		return "Not a card";
	const int saves = (int)open.card.saves.size();
	return format("%d save%s  \xc2\xb7  %d of 15 blocks free", saves, saves == 1 ? "" : "s", open.card.freeBlocks);
}

// A name for a file put out, with nothing in it a file name cannot have.
std::string safeName(std::string name)
{
	for (char& c : name)
		if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|'
				|| (unsigned char)c < 32)
			c = '_';
	return name;
}

Image saveIcon(const std::string& cardPath, const memcard::Save& save)
{
	if (save.icon.empty() || save.icon[0].size() != 256)
		return {};
	// The frames of its icon, a few a second, as the PlayStation shows them.
	const size_t frame = motion() == MotionOff ? 0 : (size_t)(clock() * 3.0) % save.icon.size();
	if (save.icon[frame].size() != 256)
		return {};
	return imageFromPixels(format("%s#%d#%d", cardPath.c_str(), save.firstBlock, (int)frame), save.icon[frame].data(), 16,
			16, 4);
}

// The copies kept of a card, the newest first.
std::vector<std::string> cardCopies(const std::string& cardPath)
{
	std::vector<std::string> copies;
	for (const std::string& name : filesIn(savesDir() + "backups/" + fileTitle(cardPath)))
		if (extension(name) == ".mcd")
			copies.push_back(name);
	std::reverse(copies.begin(), copies.end());
	return copies;
}

// "20261006-183000.mcd" for the screen.
std::string copyLabel(const std::string& name)
{
	if (name.size() >= 15 && name[8] == '-')
		return name.substr(0, 4) + "-" + name.substr(4, 2) + "-" + name.substr(6, 2) + "  " + name.substr(9, 2) + ":"
				+ name.substr(11, 2);
	return fileTitle(name);
}
}

void cardsPage(Frame& f)
{
	std::vector<Item> items;
	const std::vector<std::string> cards = cardFiles();
	if (!cards.empty())
		items.push_back(header("MEMORY CARDS"));
	for (const std::string& name : cards)
	{
		const std::string path = savesDir() + name;
		const OpenCard& open = openCard(path);
		Item item;
		item.icon = icon::Card;
		item.label = cardLabel(name);
		item.value = cardSummary(open);
		item.info = name + "\n\nThe card of this game: the emulator keeps one for each game, in " + shownRoot()
				+ "data/saves, and makes it when the game first runs.";
		if (!open.ok)
			item.info += "\n\n" + open.error;
		item.enabled = open.ok;
		item.confirmHint = "Open";
		item.activate = [path] { push(Page::Card, 0, 0, path); };
		items.push_back(item);
	}
	// What was put in the import folder: whole cards, and single saves.
	std::vector<Item> incoming;
	for (const std::string& name : filesIn(importDir()))
	{
		const std::string path = importDir() + name;
		const int kind = memcard::kindOf(path, fileSize(path));
		if (kind == 0)
			continue;
		Item item;
		item.icon = kind == 1 ? icon::Card : icon::Save;
		item.label = name;
		if (kind == 1)
		{
			const OpenCard& open = openCard(path);
			item.value = cardSummary(open);
			item.enabled = open.ok;
			item.info = "A memory card from somewhere else. Open it to copy its saves to your cards.";
			if (!open.ok)
				item.info += "\n\n" + open.error;
			item.confirmHint = "Open";
			item.activate = [path] { push(Page::Card, 1, 0, path); };
		}
		else
		{
			item.value = "A single save";
			item.info = "One save as a file. Choose the card it goes on.";
			item.confirmHint = "Put on a card";
			item.activate = [path] { push(Page::CardPick, 0, 1, path); };
			item.enabled = !cards.empty();
		}
		incoming.push_back(item);
	}
	if (!incoming.empty())
	{
		items.push_back(header("TO BRING IN"));
		items.insert(items.end(), incoming.begin(), incoming.end());
	}
	if (items.empty())
	{
		const Theme& t = theme();
		const float W = unitsWide(), H = unitsHigh();
		text(at(64, 40), t.text, "Memory cards", Title, 44);
		panel(at(64, 128), at(W - 64, H - 88), t.panel, 16);
		textCentred(at(W * 0.5f, H * 0.36f), withAlpha(t.accent, 0.8f), icon::Card, Title, 72);
		textCentred(at(W * 0.5f, H * 0.36f + 110), t.text, "No memory card yet", Bold, 32);
		const std::string hint = "A game's card is made when the game first runs. Cards and saves from elsewhere go in "
				+ shownRoot() + "memcards/import, and are then listed here.";
		const float w = std::min(toUnits(measure(hint, Body, 24).x), 1100.f);
		textWrapped(at((W - w) * 0.5f, H * 0.36f + 170), px(1100), t.dim, hint, Body, 24);
		hintBar({ { cancelButton, "Back" } });
		return;
	}
	const Item *focused = menuPage(f, "Memory cards", "", items, 1000);
	standardHints(focused);
}

void cardPage(Frame& f)
{
	const std::string path = f.s;
	const bool incoming = f.a == 1;
	OpenCard& open = openCard(path);
	std::vector<Item> items;
	if (!open.ok)
	{
		deferred = [error = open.error] {
			pop();
			message("The card could not be read", error);
		};
		return;
	}
	if (!open.card.saves.empty())
		items.push_back(header(format("SAVES   %d of 15 blocks free", open.card.freeBlocks)));
	for (const memcard::Save& save : open.card.saves)
	{
		Item item;
		item.picture = saveIcon(path, save);
		item.label = save.title.empty() ? save.fileName : save.title;
		item.value = format("%d block%s", save.blocks, save.blocks == 1 ? "" : "s");
		item.info = (save.serial.empty() ? save.fileName : save.serial + "\n" + save.fileName)
				+ format("\n\n%d of the card's 15 blocks.", save.blocks);
		const int block = save.firstBlock;
		const std::string fileName = safeName(save.fileName);
		item.menu = true;
		item.choices = { "Copy to another card", "Put out as a file" };
		if (!incoming)
			item.choices.push_back("Delete");
		item.confirmHint = "Copy, put out" + std::string(incoming ? "" : ", delete");
		item.choose = [path, block, fileName, title = item.label](int what) {
			if (what == 0)
				push(Page::CardPick, block, 0, path);
			else if (what == 1)
			{
				makeDir(exportDir());
				OpenCard& card = openCard(path);
				const std::string out = exportDir() + fileName + ".mcs";
				if (card.ok && memcard::exportSave(card.card, block, out))
					inform("Put out as a file", title + " is in " + shownRoot() + "memcards/export as " + fileName
							+ ".mcs, a form other emulators and card tools read.");
				else
					message("The save was not put out", "The file could not be written.");
			}
			else
				push(Page::Confirm, 0, 0, "Delete this save?", title + " is removed from the card. A copy of the card as it "
						"is now is kept first (Settings, Games and network, Card backups).", [path, block] {
					OpenCard& card = openCard(path);
					if (!card.ok)
						return;
					// The card as it was, among its copies.
					const std::string folder = savesDir() + "backups/" + fileTitle(path);
					makeDir(folder);
					char stamp[32];
					const time_t t = time(nullptr) + localTimeOffset();
					struct tm tm;
					gmtime_r(&t, &tm);
					strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
					memcard::save(folder + "/" + stamp + ".mcd", card.card);
					if (!memcard::remove(card.card, block) || !memcard::save(path, card.card))
						message("The save was not deleted", "The card could not be written.");
					openCards.erase(path);
				});
		};
		items.push_back(item);
	}
	if (open.card.saves.empty())
		items.push_back(fact("Nothing is saved on this card", "", "The card is empty."));
	if (!incoming)
	{
		items.push_back(header("THE CARD"));
		const std::vector<std::string> copies = cardCopies(path);
		{
			Item item;
			item.icon = icon::Undo;
			item.label = "Go back to an earlier copy";
			item.info = "A copy of the card is kept each time it has changed when its game closes, and before a save is "
					"deleted here. Going back to one replaces the card with it; the card as it is now is kept as a copy "
					"first.";
			item.enabled = !copies.empty();
			item.value = copies.empty() ? "None kept" : format("%d kept", (int)copies.size());
			item.menu = true;
			for (const std::string& copy : copies)
				item.choices.push_back(copyLabel(copy));
			item.choose = [path, copies](int index) {
				if (index < 0 || index >= (int)copies.size())
					return;
				const std::string folder = savesDir() + "backups/" + fileTitle(path) + "/";
				memcard::Card earlier;
				std::string error;
				if (!memcard::load(folder + copies[(size_t)index], earlier, &error))
				{
					message("The copy could not be read", error);
					return;
				}
				OpenCard& card = openCard(path);
				if (card.ok)
				{
					char stamp[32];
					const time_t t = time(nullptr) + localTimeOffset();
					struct tm tm;
					gmtime_r(&t, &tm);
					strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
					memcard::save(folder + stamp + ".mcd", card.card);
				}
				if (!memcard::save(path, earlier))
					message("The card was not changed", "The card could not be written.");
				openCards.erase(path);
			};
			items.push_back(item);
		}
		items.push_back(action(icon::Upload, "Put the whole card out as a file", "A copy of the card in " + shownRoot()
				+ "memcards/export, to take to another emulator or to keep.", [path] {
					makeDir(exportDir());
					OpenCard& card = openCard(path);
					const std::string name = safeName(fileTitle(path)) + ".mcd";
					if (card.ok && memcard::save(exportDir() + name, card.card))
						inform("Put out as a file", "The card is in " + shownRoot() + "memcards/export as " + name + ".");
					else
						message("The card was not put out", "The file could not be written.");
				}));
	}
	const Item *focused = menuPage(f, incoming ? baseName(path) : cardLabel(baseName(path)), cardSummary(open), items, 1000);
	standardHints(focused);
}

void cardPickPage(Frame& f)
{
	const std::string from = f.s;
	const int block = f.a;
	const bool single = f.b == 1;
	std::vector<Item> items;
	for (const std::string& name : cardFiles())
	{
		const std::string path = savesDir() + name;
		if (path == from)
			continue;
		const OpenCard& open = openCard(path);
		Item item;
		item.icon = icon::Card;
		item.label = cardLabel(name);
		item.value = cardSummary(open);
		item.enabled = open.ok;
		item.confirmHint = "Put it here";
		item.activate = [from, path, block, single] {
			OpenCard& target = openCard(path);
			std::string error;
			bool ok = false;
			if (!target.ok)
				error = "The card could not be read.";
			else if (single)
				ok = memcard::importSave(target.card, from, &error);
			else
			{
				OpenCard& source = openCard(from);
				ok = source.ok && memcard::copy(source.card, block, target.card, &error);
			}
			if (ok && !memcard::save(path, target.card))
			{
				ok = false;
				error = "The card could not be written.";
			}
			openCards.erase(path);
			pop();
			if (ok)
				inform("The save is on the card", "It is on " + cardLabel(baseName(path)) + " now.");
			else
				message("The save was not copied", error.empty() ? std::string("It could not be copied.") : error);
		};
		items.push_back(item);
	}
	if (items.empty())
		items.push_back(fact("There is no other card", "", "A game's card is made when the game first runs."));
	const Item *focused = menuPage(f, "Which card?", single ? baseName(from) : "", items, 1000);
	standardHints(focused);
}

// ------------------------------------------------------------ the button map

namespace
{
const char *const playStationButtons[16] = { "Cross", "Square", "Select", "Start", "D-pad up", "D-pad down", "D-pad left",
		"D-pad right", "Circle", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3" };
const char *const padButtonNames[16] = { "Cross", "Square", "Touch pad, left half", "Touch pad, right half", "D-pad up",
		"D-pad down", "D-pad left", "D-pad right", "Circle", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3" };
// The order they are listed in: the four shapes, the shoulders, the rest.
const int buttonOrder[16] = { 0, 8, 1, 9, 10, 11, 12, 13, 14, 15, 2, 3, 4, 5, 6, 7 };
}

void buttonsPage(Frame& f)
{
	options::Frontend& o = options::frontend();
	std::vector<Item> items;
	items.push_back(header("THE PLAYSTATION'S BUTTONS"));
	for (const int target : buttonOrder)
	{
		Item item;
		item.label = playStationButtons[target];
		for (int source = 0; source < 16; source++)
			item.choices.push_back(padButtonNames[source]);
		item.choices.push_back("Nothing");
		item.current = o.remap[target] < 0 ? 16 : o.remap[target];
		const bool turbo = (o.turbo & (1u << target)) != 0;
		item.value = item.choices[(size_t)item.current] + (turbo ? "   \xc2\xb7   turbo" : "");
		item.mark = o.remap[target] != target ? 1 : 0;
		item.info = std::string("Which button of the pad presses the PlayStation's ") + playStationButtons[target]
				+ ". Square switches turbo for it: held, it is then pressed again and again by itself.";
		item.choose = [target](int index) {
			options::frontend().remap[target] = index >= 16 ? -1 : index;
			options::saveFrontend();
		};
		// Turbo is for buttons a game wants hammered, not for directions.
		if (target < 4 || target > 7)
		{
			item.alt = [target] {
				options::frontend().turbo ^= 1u << target;
				options::saveFrontend();
			};
			item.altHint = turbo ? "Turbo off" : "Turbo on";
		}
		items.push_back(item);
	}
	items.push_back(header("TURBO"));
	items.push_back(choice("Turbo speed", o.turboRate, { "7 a second", "10 a second", "15 a second" },
			"How fast a button with turbo is pressed while it is held.", [](int i) { options::frontend().turboRate = i; }));
	items.push_back(action(icon::Undo, "Back to the usual", "Every button is itself again, and none has turbo.", [] {
		options::Frontend& o = options::frontend();
		for (int i = 0; i < 16; i++)
			o.remap[i] = i;
		o.turbo = 0;
		options::saveFrontend();
	}));
	const Item *focused = menuPage(f, "Buttons", "for every player, in every game", items, 900);
	standardHints(focused);
}

// ---------------------------------------------------------- RetroAchievements

void achievementsPage(Frame& f)
{
	const achievements::Summary summary = achievements::summary();
	std::vector<Item> items;
	if (!summary.loggedIn)
		items.push_back(fact(summary.loggingIn ? "Signing in\xe2\x80\xa6" : "Not signed in", "",
				"Sign in under Settings, RetroAchievements, Account."));
	else if (summary.gameLoading)
		items.push_back(fact("Asking RetroAchievements about this disc\xe2\x80\xa6", "", ""));
	else if (!summary.gameLoaded)
		items.push_back(fact("This disc has no achievements", "", summary.lastError.empty()
				? std::string("RetroAchievements does not know this disc: achievements are made for particular "
				"releases of a game, and this may be another one, or a changed image.") : summary.lastError));
	else
	{
		const std::vector<achievements::Achievement> list = achievements::list();
		for (int pass = 0; pass < 2; pass++)
		{
			const bool earned = pass == 1;
			bool any = false;
			for (const achievements::Achievement& a : list)
				any = any || a.unlocked == earned;
			if (!any)
				continue;
			items.push_back(header(earned ? "EARNED" : "TO EARN"));
			for (const achievements::Achievement& a : list)
			{
				if (a.unlocked != earned)
					continue;
				Item item;
				if (!a.badge.empty())
					item.picture = image(a.badge);
				if (item.picture.id == nullptr)
					item.icon = earned ? icon::Trophy : icon::Lock;
				item.label = a.title + (a.unofficial ? "   (unofficial)" : "");
				item.value = !a.progress.empty() && !earned ? a.progress : format("%d", a.points);
				item.info = a.description + format("\n\n%d point%s", a.points, a.points == 1 ? "" : "s");
				if (!a.progress.empty() && !earned)
					item.info += "  \xc2\xb7  " + a.progress;
				if (earned && !a.unlockedWhen.empty())
					item.info += "  \xc2\xb7  earned " + a.unlockedWhen;
				items.push_back(item);
			}
		}
		const std::vector<achievements::Leaderboard> boards = achievements::leaderboards();
		if (!boards.empty())
			items.push_back(header("LEADERBOARDS"));
		for (const achievements::Leaderboard& board : boards)
		{
			Item item;
			item.icon = icon::List;
			item.label = board.title;
			item.value = board.tracking ? board.value : board.best;
			item.info = board.description + (board.tracking ? "\n\nAn attempt is running." : "");
			items.push_back(item);
		}
	}
	std::string subtitle;
	if (summary.gameLoaded)
		subtitle = format("%d of %d  \xc2\xb7  %d of %d points", summary.unlocked, summary.total, summary.points,
				summary.totalPoints) + (summary.hardcore ? "  \xc2\xb7  hardcore" : "");
	const Item *focused = menuPage(f, "Achievements", subtitle, items, 1000);
	if (summary.gameLoaded && !summary.richPresence.empty())
	{
		const Theme& t = theme();
		textFit(at(64, unitsHigh() - 82), px(1000), t.faint, summary.richPresence, Body, 20);
	}
	hintBar({ { cancelButton, "Back" } });
	(void)focused;
}

namespace
{
// What was typed for signing in; the password is forgotten once it is sent.
std::string accountUser, accountPassword;
}

void accountPage(Frame& f)
{
	const achievements::Summary summary = achievements::summary();
	std::vector<Item> items;
	if (f.fresh && accountUser.empty())
		accountUser = host::achievementsUser();
	if (summary.loggedIn)
	{
		items.push_back(fact("Signed in as", summary.user, format("%s, with %d points.", summary.user.c_str(),
				summary.userPoints)));
		items.push_back(action(icon::Power, "Sign out", "This console forgets the account. Signing in again needs "
				"the password.", [] {
					host::achievementsLogout();
					accountPassword.clear();
				}));
	}
	else
	{
		Item user = action(icon::User, "User name", "Your name at retroachievements.org.", [] {
			askText("User name", accountUser, false, false, [](const std::string& text) { accountUser = trim(text); });
		});
		user.value = accountUser.empty() ? "Not set" : accountUser;
		user.confirmHint = "Type";
		items.push_back(user);
		Item password = action(icon::Lock, "Password", "Typed once: the server answers with a key, and the key is what "
				"this console keeps (" + shownRoot() + "data/retroachievements.cfg), not the password.", [] {
			askText("Password", "", true, false, [](const std::string& text) { accountPassword = text; });
		});
		password.value = accountPassword.empty() ? "Not typed" : std::string(accountPassword.size(), '*');
		password.confirmHint = "Type";
		items.push_back(password);
		Item sign = action(icon::Check, summary.loggingIn ? "Signing in\xe2\x80\xa6" : "Sign in",
				!httpAvailable() ? "The console is not on a network."
				: summary.lastError.empty() ? "Asks RetroAchievements with the name and the password above."
				: summary.lastError, [] {
					host::achievementsLogin(accountUser, accountPassword);
					accountPassword.clear();
				}, !summary.loggingIn && !accountUser.empty() && !accountPassword.empty() && httpAvailable());
		items.push_back(sign);
	}
	const Item *focused = menuPage(f, "RetroAchievements account", "", items, 900);
	standardHints(focused);
}

// ------------------------------------------------------------------ netplay

void netplayPage(Frame& f)
{
	const Theme& t = theme();
	const netplay::State state = netplay::state();
	const bool busy = netplay::active();
	// Both are there and the host's state is on its way: back to the game,
	// where the rest happens.
	if (busy && (state == netplay::State::Syncing || state == netplay::State::Playing) && f.a == 1)
	{
		deferred = [] { resume(); };
		return;
	}
	std::vector<Item> items;
	std::string subtitle;
	if (!busy)
	{
		f.a = 0;
		const std::string why = state == netplay::State::Failed || state == netplay::State::Ended ? netplay::error() : "";
		items.push_back(action(icon::Server, "Host this game", "This console waits for the other player, who joins it by "
				"its address. The host is player 1.", [] {
					if (host::netplayHost())
					{
						if (!stack.empty())
							stack.back().a = 1;
					}
					else
						message("Netplay did not start", netplay::error().empty()
								? std::string("The game has no serial number to tell the other console.") : netplay::error());
				}, !host::game().serial.empty()));
		items.push_back(action(icon::Network, "Join a game", "Type the address the hosting console shows. This console "
				"is player 2.", [] {
					askText("The host's address", "192.168.", false, true, [](const std::string& address) {
						if (address.empty())
							return;
						if (host::netplayJoin(address))
						{
							if (!stack.empty())
								stack.back().a = 1;
						}
						else
							message("Netplay did not start", netplay::error().empty() ? std::string("That is not an address.")
									: netplay::error());
					});
				}, !host::game().serial.empty()));
		items.push_back(choice("Input delay", options::frontend().netplayDelay - 1,
				{ "1 frame", "2 frames", "3 frames", "4 frames", "5 frames", "6 frames" },
				"How long after a button is pressed the game sees it. Both consoles wait this long so that each has "
				"the other's buttons in time: two frames suit a home network, more a slower one. The host's is used.",
				[](int i) { options::frontend().netplayDelay = i + 1; }));
		items.push_back(fact("How it works", "", "Both consoles run the same game, from the same state, and send each "
				"other only what the players press. For that they need the same disc (the same serial number), the same "
				"BIOS, and this build of PSSwanStation; the host's emulator settings are used on both. Memory cards "
				"and cheats are out while it lasts, and so are save states, fast forward and rewind. Should the two "
				"drift apart, the host's state is sent again.\n\nAn experiment: tried between two PCs, not yet "
				"between two consoles."));
		if (!why.empty())
			subtitle = why;
	}
	else if (state == netplay::State::Playing || state == netplay::State::Syncing)
	{
		items.push_back(fact("Playing", netplay::hosting() ? "You are player 1 (host)" : "You are player 2",
				"The game goes on when this menu is closed; the other player waits meanwhile."));
		const int ping = netplay::ping();
		items.push_back(fact("There and back", ping >= 0 ? format("%d ms", ping) : "Not known yet"));
		items.push_back(fact("Brought back in step", format("%u time%s", netplay::resyncs(), netplay::resyncs() == 1 ? "" : "s"),
				"How often the two consoles had drifted apart and the host's state was sent again."));
		items.push_back(action(icon::Power, "Leave netplay", "The game goes on, on this console alone.", [] {
			host::netplayStop();
			pop();
		}));
	}
	else
	{
		const std::string address = netplay::localAddress();
		if (state == netplay::State::Listening)
			items.push_back(fact("Waiting for the other player", address.empty() ? "Address not known" : address,
					address.empty() ? std::string("This console's address on the network could not be found: look it up in the "
					"console's network settings and tell it to the other player.")
					: "Tell the other player this address: " + address + ". They choose Join a game and type it."));
		else
			items.push_back(fact(state == netplay::State::Connecting ? "Reaching the host\xe2\x80\xa6"
					: "Comparing the two games\xe2\x80\xa6", "", ""));
		items.push_back(action(icon::Cross, "Stop", "", [] { host::netplayStop(); }));
	}
	const Item *focused = menuPage(f, "Netplay", subtitle, items, 800);
	if (busy && state == netplay::State::Listening)
	{
		// The address, large, for reading out.
		const std::string address = netplay::localAddress();
		const float left = 64 + 800 + 24;
		if (!address.empty())
			textCentred(at(left + (unitsWide() - 64 - left) * 0.5f, 300), t.accent, address, Huge, 72);
	}
	standardHints(focused);
	if (f.picker < 0 && hit(cancelButton))
	{
		// Stepping back from a session that has not begun ends it.
		if (busy && state != netplay::State::Playing && state != netplay::State::Syncing)
			host::netplayStop();
		deferred = [] { pop(); };
		consumeInput();
	}
	else if (hit(Options) && host::running())
		deferred = [] { resume(); };
}

// ------------------------------------------------------------------ updates

void updatePage(Frame& f)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const update::Status status = update::status();
	if (f.fresh)
	{
		f.fresh = false;
		if (status.state == update::State::Idle || status.state == update::State::UpToDate
				|| status.state == update::State::NoRelease || status.state == update::State::Failed)
			update::check();
	}
	text(at(64, 40), t.text, "Update", Title, 44);
	const float x0 = 64, x1 = W - 64, top = 128, bottom = H - 88;
	panel(at(x0, top), at(x1, bottom), t.panel, 16);
	const float tx = x0 + 48, tw = x1 - x0 - 96;
	float y = top + 44;
	const auto line = [&](const std::string& value, ImU32 colour, Font font, float size) {
		y += toUnits(textWrapped(at(tx, y), px(tw), colour, value, font, size)) + 14;
	};
	std::vector<Hint> hints;
	bool leave = hit(cancelButton);
	switch (status.state)
	{
	case update::State::Idle:
	case update::State::Checking:
		line("Asking the releases page\xe2\x80\xa6", t.text, Bold, 30);
		progressBar(at(tx, y + 10), at(tx + std::min(tw, 600.f), y + 22), -1.f);
		break;
	case update::State::UpToDate:
		line(format("This is the newest build (%d).", BuildNumber), t.text, Bold, 30);
		line("Nothing to do.", t.dim, Body, 24);
		break;
	case update::State::NoRelease:
		line("The releases page has no build to fetch yet.", t.text, Bold, 30);
		line(std::string("Builds are published at github.com/Press5elect/PSSwanStation, under Releases. This is build ")
				+ std::to_string(BuildNumber) + ".", t.dim, Body, 24);
		break;
	case update::State::Available:
		line(status.name.empty() ? format("Build %d is out", status.build) : drawable(status.name), t.text, Bold, 30);
		line(format("You have build %d. Updating replaces the program's files only: your games, saves, states, covers "
				"and settings stay as they are.", BuildNumber), t.dim, Body, 24);
		if (!status.notes.empty())
		{
			y += 10;
			text(at(tx, y), t.accent, "WHAT IS NEW", Bold, 20);
			y += 36;
			textWrapped(at(tx, y), px(tw), IM_COL32(200, 208, 226, 255), drawable(status.notes), Body, 22,
					px(bottom - y - 30));
		}
		hints.push_back({ confirmButton, "Download" });
		if (hit(confirmButton))
			update::download();
		break;
	case update::State::Downloading:
	{
		line(format("Downloading build %d", status.build), t.text, Bold, 30);
		const float fraction = status.total != 0 ? (float)((double)status.done / (double)status.total) : -1.f;
		progressBar(at(tx, y + 10), at(tx + std::min(tw, 900.f), y + 26), fraction);
		y += 50;
		std::string how = status.total != 0 ? format("%.1f of %.1f MB", status.done / 1048576.0, status.total / 1048576.0)
				: format("%.1f MB", status.done / 1048576.0);
		if (status.speed > 16384)
			how += format("  \xc2\xb7  %.1f MB/s", status.speed / 1048576.0);
		line(how, t.dim, Body, 24);
		if (leave)
		{
			update::cancel();
			leave = false;
		}
		break;
	}
	case update::State::Verifying:
		line(format("Checking and unpacking build %d\xe2\x80\xa6", status.build), t.text, Bold, 30);
		progressBar(at(tx, y + 10), at(tx + std::min(tw, 600.f), y + 22), -1.f);
		leave = false;
		break;
	case update::State::Ready:
		line(format("Build %d is ready to be put in place", status.build), t.text, Bold, 30);
		line("PSSwanStation closes when it is done. Start it again from the home screen: it is then the new build.",
				t.dim, Body, 24);
		hints.push_back({ confirmButton, "Install and close" });
		if (hit(confirmButton))
		{
			if (update::install())
				f.b = 1;
		}
		break;
	case update::State::Installed:
		line(format("Build %d is in place.", status.build), t.text, Bold, 30);
		line("Closing. Start PSSwanStation again from the home screen.", t.dim, Body, 24);
		// The line is on the screen for a moment, then the title closes.
		if (f.b++ > 90)
			quit = true;
		leave = false;
		break;
	case update::State::Failed:
		line("The update did not go through", t.text, Bold, 30);
		line(status.error.empty() ? std::string("Nothing was changed.") : status.error + " Nothing was changed.", t.dim,
				Body, 24);
		hints.push_back({ confirmButton, "Ask again" });
		if (hit(confirmButton))
			update::check();
		break;
	}
	if (status.state != update::State::Installed && status.state != update::State::Verifying)
		hints.push_back({ cancelButton, status.state == update::State::Downloading ? "Stop" : "Back" });
	hintBar(hints, format("Releases: github.com/%s/PSSwanStation", Developer));
	if (leave)
	{
		deferred = [] { pop(); };
		consumeInput();
	}
}

// --------------------------------------------------------------- safe start

void safeStartPage(Frame& f)
{
	std::vector<Item> items;
	items.push_back(action(icon::Play, "Go on as usual", "Nothing is changed.", [] { pop(); }));
	items.push_back(action(icon::Screen, "Display output back to 60 Hz", "Should the screen have stayed dark, or flickered, "
			"after another output was chosen: the console is asked for 59.94 Hz again from the next start, and black "
			"frame insertion is switched off.", [] {
				options::Frontend& o = options::frontend();
				o.displayMode = 0;
				o.blackFrames = false;
				o.pacing = 0;
				options::saveFrontend();
				storage::syncDisplayMode(appDir + "sce_sys/param.json", 0);
				inform("Done", "From the next start the display output is 59.94 Hz. Close PSSwanStation and start it again.");
			}));
	items.push_back(action(icon::Shield, "Stay in the sandbox", "Switches off USB drives and keeping your files outside the "
			"title folder, which are what make PSSwanStation leave its sandbox when it starts. Your files outside stay "
			"where they are.", [] {
				options::Frontend& o = options::frontend();
				o.usb = false;
				o.outside = false;
				options::saveFrontend();
				inform("Done", "From the next start PSSwanStation stays in its sandbox and uses the files in its own folder.");
			}));
	items.push_back(action(icon::Paint, "The interface's settings back to the usual", "Everything under Interface, Picture, "
			"Sound, Controllers and Shortcuts as it was at first: sizes, colours, the button map, the picture's "
			"filters. The emulator's settings and your games' own are kept.", [] {
				const options::Frontend before = options::frontend();
				options::resetFrontend();
				// Where the files are is not an interface setting.
				options::frontend().usb = before.usb;
				options::frontend().outside = before.outside;
				options::saveFrontend();
				storage::syncDisplayMode(appDir + "sce_sys/param.json", 0);
				audio::setVolume(options::frontend().volume);
				inform("Done", "The interface's settings are as they were at first.");
			}));
	items.push_back(action(icon::Chip, "The emulator's settings back to the usual", "Every setting of the emulator for all "
			"games (renderer, resolution, enhancements, the console's) as it was at first. What is set for a single "
			"game is kept.", [] {
				options::resetGlobal();
				inform("Done", "The emulator's settings are as they were at first.");
			}));
	standardHints(menuPage(f, "Safe start", f.a == 1 ? "The display output is 60 Hz again from the next start: close "
			"PSSwanStation and start it again" : "L1 and R1 were held while PSSwanStation started", items, 900));
}

// ---------------------------------------------------------- the shortcuts

void shortcutsPage(Frame& f)
{
	std::vector<Item> items;
	items.push_back(header("WHILE A GAME RUNS: OPTIONS HELD, AND"));
	const auto row = [&items](const char *symbol, const std::string& button, const std::string& does, const std::string& info) {
		Item item = fact(does, button, info);
		item.icon = symbol;
		items.push_back(item);
	};
	row(icon::Forward, "R2", "Fast forward", "While both are held the game runs faster, without sound. How fast is set "
			"under Settings, Shortcuts and rewind.");
	row(icon::Backward, "L2", "Rewind", "While both are held the game goes backwards through the last while of play. It "
			"must be switched on first (Settings, Shortcuts and rewind): the states it steps through are kept in "
			"memory only while it is on.");
	row(icon::Save, "R1", "Save a state", "To the slot the shortcuts use: the one saved to last, or chosen with OPTIONS and "
			"Left or Right.");
	row(icon::Upload, "L1", "Load that state", "");
	row(icon::List, "Left, Right", "Another slot", "Which of the ten slots the two shortcuts above use.");
	items.push_back(header("AND BY ITSELF"));
	row(icon::Gear, "OPTIONS, tapped", "The menu", "With the shortcuts on, the menu opens when OPTIONS is let go, since "
			"holding it is the shortcuts' key.");
	row(icon::Gamepad, "Touch pad, left and right", "Select and Start", "Press the pad down on that side.");
	standardHints(menuPage(f, "Shortcuts", "", items, 900));
}

// ------------------------------------------------------- over a running game

bool gameShortcuts()
{
	static bool optionsDown, usedAsKey;
	const options::Frontend& settings = options::frontend();
	if (optionsSpent)
	{
		// The press that closed the menu: nothing until it is let go.
		optionsSpent = (in.held & Options) != 0;
		optionsDown = false;
		return false;
	}
	if (!settings.hotkeys)
	{
		optionsDown = false;
		host::setInputBlocked(false);
		host::setFastForward(false);
		host::setRewinding(false);
		return hit(Options);
	}
	if ((in.held & Options) == 0)
	{
		// Let go: by itself it was the menu's button.
		const bool tapped = optionsDown && !usedAsKey;
		optionsDown = false;
		host::setInputBlocked(false);
		host::setFastForward(false);
		host::setRewinding(false);
		return tapped;
	}
	if (!optionsDown)
	{
		optionsDown = true;
		usedAsKey = false;
	}
	// While it is held the pads are the shortcuts', not the game's.
	host::setInputBlocked(true);
	const bool forward = (in.held & R2) != 0, back = (in.held & L2) != 0;
	if (forward || back)
		usedAsKey = true;
	if (back && !host::rewinding() && !settings.rewind && hit(L2))
		host::addMessage("Rewind is off: it is switched on under Settings, Shortcuts and rewind.", 4.0);
	host::setRewinding(back);
	host::setFastForward(forward && !back);
	if (hit(R1))
	{
		usedAsKey = true;
		const int slot = host::quickSlot();
		if (host::saveState(slot))
			forgetImage(host::stateThumbPath(slot));
	}
	else if (hit(L1))
	{
		usedAsKey = true;
		if (!host::stateExists(host::quickSlot()))
			host::addMessage(format("Slot %d is empty.", host::quickSlot() + 1));
		else
			host::loadState(host::quickSlot());
	}
	else if (hit(Left | Right))
	{
		usedAsKey = true;
		const int slot = (host::quickSlot() + (hit(Right) ? 1 : host::StateSlots - 1)) % host::StateSlots;
		host::setQuickSlot(slot);
		std::string when;
		host::addMessage(format("The shortcuts use slot %d", slot + 1) + (host::stateExists(slot, &when) ? "  \xc2\xb7  saved "
				+ when : std::string("  \xc2\xb7  empty")), 2.5);
	}
	return false;
}

void gameMarks()
{
	const Theme& t = theme();
	const float W = unitsWide();
	// What the game is doing other than playing: said at the top, in the middle.
	std::string mark;
	const char *symbol = nullptr;
	if (host::rewinding())
	{
		symbol = icon::Backward;
		mark = format("Rewind   %.0f s left", host::rewindSeconds());
	}
	else if (host::fastForward())
	{
		static const char *const speeds[5] = { "2x", "3x", "4x", "8x", "as fast as it goes" };
		symbol = icon::Forward;
		mark = std::string("Fast forward   ") + speeds[std::clamp(options::frontend().fastForward, 0, 4)];
	}
	if (netplay::active())
	{
		const netplay::State state = netplay::state();
		if (state == netplay::State::Syncing)
			mark = "Netplay: the two consoles are being put in step\xe2\x80\xa6";
		else if (netplay::remotePaused())
			mark = "Netplay: the other player has a menu open";
		else if (netplay::silentSeconds() > 0)
			mark = format("Netplay: nothing from the other player for %d s", netplay::silentSeconds());
		if (!mark.empty())
			symbol = icon::Users;
	}
	if (!mark.empty())
	{
		const float w = toUnits(measure(mark, Bold, 24).x) + 100;
		const float x = (W - w) * 0.5f;
		panel(at(x, 30), at(x + w, 82), IM_COL32(10, 12, 20, 215), 26);
		text(at(x + 26, 43), t.accent, symbol, Body, 24);
		text(at(x + 70, 42), t.text, mark, Bold, 24);
	}
	// A light gun's aim, unless the emulator draws one itself.
	const char *own = options::get("swanstation_Controller_ShowCrosshair");
	if (own == nullptr || strcmp(own, "true") != 0)
		for (int port = 0; port < MaxPads; port++)
		{
			float x = 0, y = 0;
			if (!host::gunAim(port, x, y))
				continue;
			const ImVec2 c(gameA.x + (gameB.x - gameA.x) * (x * 0.5f + 0.5f), gameA.y + (gameB.y - gameA.y) * (y * 0.5f + 0.5f));
			static const ImU32 colours[MaxPads] = { IM_COL32(90, 170, 255, 255), IM_COL32(255, 90, 90, 255),
					IM_COL32(90, 230, 120, 255), IM_COL32(255, 110, 220, 255) };
			ImDrawList *list = draw();
			const float r = px(16);
			list->AddCircle(c, r, IM_COL32(0, 0, 0, 200), 32, px(5));
			list->AddCircle(c, r, colours[port], 32, px(2.5f));
			list->AddLine(ImVec2(c.x - r * 1.6f, c.y), ImVec2(c.x - r * 0.5f, c.y), colours[port], px(2.5f));
			list->AddLine(ImVec2(c.x + r * 0.5f, c.y), ImVec2(c.x + r * 1.6f, c.y), colours[port], px(2.5f));
			list->AddLine(ImVec2(c.x, c.y - r * 1.6f), ImVec2(c.x, c.y - r * 0.5f), colours[port], px(2.5f));
			list->AddLine(ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x, c.y + r * 1.6f), colours[port], px(2.5f));
		}
}

float drawNotice(const host::Message& m, float y)
{
	const Theme& t = theme();
	const bool pictured = !m.picture.empty();
	const float textW = std::max(toUnits(measure(m.title, Bold, 26).x), toUnits(measure(m.text, Body, 22).x));
	const float h = 96, pad = pictured ? 104.f : 28.f;
	const float w = std::min(textW + pad + 36, unitsWide() - 96);
	const float top = y - h;
	const float fade = (float)std::clamp((m.until - now()) / 0.4, 0.0, 1.0);
	panel(at(48, top), at(48 + w, top + h), withAlpha(IM_COL32(14, 18, 30, 235), fade), 14);
	panel(at(48, top + 14), at(54, top + h - 14), withAlpha(t.accent, fade), 3);
	if (pictured)
	{
		const Image picture = image(m.picture);
		if (picture.id != nullptr)
			imageFit(picture, at(48 + 20, top + 14), at(48 + 20 + 68, top + 14 + 68), 8,
					IM_COL32(255, 255, 255, (int)(255 * fade)));
		else
			text(at(48 + 34, top + 28), withAlpha(t.accent, fade), icon::Trophy, Body, 40);
	}
	textFit(at(48 + pad, top + 16), px(w - pad - 24), withAlpha(t.text, fade), m.title, Bold, 26);
	textFit(at(48 + pad, top + 54), px(w - pad - 24), withAlpha(t.dim, fade), m.text, Body, 22);
	return h;
}

void drawBorder(ImDrawList *list, void *texture, float u, float v, ImVec2 p0, ImVec2 p1)
{
	const float W = width(), H = height();
	const int kind = options::frontend().border;
	// A picture that fills the screen has nothing beside it.
	if (kind == 0 || (p0.x < 2 && p0.y < 2))
		return;
	if (kind == 1)
	{
		// The picture's own light: a few pixels of it, averaged over the last
		// half second, spread over the whole screen and turned down.
		void *soft = display::ambient(texture, u, v);
		if (soft == nullptr)
			return;
		// Spread so that it covers the screen, keeping the picture's shape.
		const float pictureAspect = (p1.x - p0.x) / std::max(p1.y - p0.y, 1.f);
		float sw = W, sh = W / pictureAspect;
		if (sh < H)
		{
			sh = H;
			sw = H * pictureAspect;
		}
		const ImVec2 a((W - sw) * 0.5f, (H - sh) * 0.5f);
		list->AddImage((ImTextureID)soft, a, ImVec2(a.x + sw, a.y + sh), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(150, 150, 150, 255));
		// Darker towards the screen's edges, so the picture stays what is looked at.
		const ImU32 none = IM_COL32(0, 0, 0, 0), edge = IM_COL32(0, 0, 0, 150);
		list->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(p0.x, H), edge, none, none, edge);
		list->AddRectFilledMultiColor(ImVec2(p1.x, 0), ImVec2(W, H), none, edge, edge, none);
	}
	else if (kind == 2)
	{
		// A quiet gradient in the accent colour.
		const Theme& t = theme();
		const ImU32 top = mix(IM_COL32(6, 8, 14, 255), t.accent, 0.20f), bottom = mix(IM_COL32(2, 3, 6, 255), t.accent, 0.04f);
		list->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), top, top, bottom, bottom);
	}
	else
	{
		// A picture of the user's: one for this game, or one for all.
		static std::string chosenFor, chosenFile;
		const std::string serial = host::game().serial;
		if (chosenFor != serial || chosenFile.empty())
		{
			chosenFor = serial;
			chosenFile = "none";
			for (const std::string& name : { serial, std::string("default") })
				for (const char *ext : { ".png", ".jpg", ".jpeg" })
					if (chosenFile == "none" && !name.empty() && fileExists(rootDir + "borders/" + name + ext))
						chosenFile = rootDir + "borders/" + name + ext;
		}
		const Image picture = chosenFile == "none" ? Image() : image(chosenFile);
		if (picture.id == nullptr || picture.width <= 0)
			return;
		// Over the whole screen, cut to its shape.
		const float aspect = (float)picture.width / (float)picture.height, screen = W / H;
		ImVec2 uv0(0, 0), uv1(1, 1);
		if (aspect > screen)
		{
			const float part = screen / aspect;
			uv0.x = (1 - part) * 0.5f;
			uv1.x = 1 - uv0.x;
		}
		else
		{
			const float part = aspect / screen;
			uv0.y = (1 - part) * 0.5f;
			uv1.y = 1 - uv0.y;
		}
		list->AddImage((ImTextureID)picture.id, ImVec2(0, 0), ImVec2(W, H), uv0, uv1);
	}
	// The picture's own place is black under it (a game's picture is opaque,
	// but its first frames may not be there yet).
	list->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
}

void drawScanlines(ImDrawList *list, ImVec2 p0, ImVec2 p1)
{
	const int kind = options::frontend().crt;
	int lines = host::nativeLines();
	if (kind == 0 || lines < 100)
		return;
	const float high = p1.y - p0.y;
	// A picture of 480 lines is two fields of 240: the tube's lines are the field's.
	if (high / (float)lines < 3.f)
		lines /= 2;
	const float pitch = high / (float)lines;
	if (lines < 100 || pitch < 3.f)
		return;
	// The dark between two lines: how much of a line's height, and how dark.
	static const float part[4] = { 0, 0.30f, 0.42f, 0.42f };
	static const int dark[4] = { 0, 70, 125, 125 };
	const float band = std::max(pitch * part[kind], 1.f);
	const ImU32 colour = IM_COL32(0, 0, 0, dark[kind]), soft = IM_COL32(0, 0, 0, dark[kind] / 3);
	for (int i = 0; i < lines; i++)
	{
		const float y = p0.y + pitch * (float)(i + 1) - band;
		list->AddRectFilled(ImVec2(p0.x, y), ImVec2(p1.x, y + band), colour);
		// Its edge towards the line is softer than its middle.
		if (band >= 3.f)
			list->AddRectFilled(ImVec2(p0.x, y - 1.f), ImVec2(p1.x, y), soft);
	}
	if (kind == 3)
	{
		// The shadow mask: a fine dark stripe every third pixel across.
		const float step = std::max(std::round((p1.y - p0.y) / 720.f), 1.f) * 3.f;
		const ImU32 stripe = IM_COL32(0, 0, 0, 46);
		for (float x = p0.x; x < p1.x; x += step)
			list->AddRectFilled(ImVec2(x, p0.y), ImVec2(x + step / 3.f, p1.y), stripe);
	}
}

// ------------------------------------------------------------ the idle swan

bool idleSwan(bool allowed)
{
	static bool asleep;
	static double since;
	double limit = options::frontend().idleMinutes * 60.0;
#if defined(SWANSTATION_HOST)
	// A test does not wait minutes.
	if (const char *seconds = getenv("SWANSTATION_IDLE_SECONDS"))
		limit = atof(seconds);
#endif
	const double idle = clock() - lastInputAt;
	if (!allowed || limit <= 0)
	{
		asleep = false;
		return false;
	}
	if (!asleep)
	{
		if (idle < limit)
			return false;
		asleep = true;
		since = clock();
	}
	else if (idle < 0.5)
	{
		// A button: awake, and the button is not also something chosen.
		asleep = false;
		consumeInput();
		return false;
	}
	// Night over the water, and the swan across it, there and back.
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const float time = (float)(clock() - since);
	ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(width(), height()), IM_COL32(4, 7, 18, 255),
			IM_COL32(4, 7, 18, 255), IM_COL32(1, 2, 6, 255), IM_COL32(1, 2, 6, 255));
	const float in_ = motion() == MotionOff ? 1.f : std::clamp(time / 2.f, 0.f, 1.f);
	waves(0.5f * in_);
	const float crossing = 46.f;									// seconds from one side to the other
	const float lap = std::fmod(time / crossing, 2.f);				// 0..1 to the left, 1..2 back
	const float along = lap < 1.f ? lap : 2.f - lap;
	const float size = H * 0.20f;
	const float x = W * 0.88f - (W * 0.76f) * (motion() == MotionOff ? 0.5f : along) - size * 0.5f;
	const float y = H * 0.80f - size * 0.78f + std::sin(time * 1.1f) * 4.f;
	SwanPose pose = motion() == MotionFull ? swanIdle(clock()) : SwanPose();
	// It turns round at each side.
	const float turn = std::min(std::fmod(time, crossing), crossing - std::fmod(time, crossing));
	pose.face = (lap < 1.f ? 1.f : -1.f) * (motion() == MotionOff ? 1.f : std::clamp(turn / 0.8f, 0.f, 1.f) * 2.f - 1.f
			+ (turn >= 0.8f ? 0.f : 0.f));
	if (std::fabs(pose.face) < 0.05f)
		pose.face = 0.05f;
	pose.tilt = 0.02f * std::sin(time * 1.3f);
	swan(at(x, y), px(size), pose, in_);
	const std::string now_ = host::clockText();
	if (!now_.empty())
		textCentred(at(W * 0.5f, H * 0.24f), withAlpha(t.dim, 0.55f * in_), now_, Huge, 92);
	textCentred(at(W * 0.5f, H - 70), withAlpha(t.faint, 0.6f * in_), "Any button", Body, 22);
	return true;
}

void startNotices()
{
	static bool said;
	if (said)
		return;
	said = true;
	if (startedSafely())
		inform("A safe start", "The last time PSSwanStation started, it did not get as far as the library. This start was "
				"made as plainly as can be: inside the sandbox (so without USB drives, and with the files in the title's "
				"own folder) and at 59.94 Hz. If the trouble came after a setting was changed, change it back now; "
				"holding L1 and R1 while PSSwanStation starts offers more.");
	if (options::frontend().outside && !outsideAvailable())
		message("Your files are kept outside, and cannot be reached", std::string("Your files are in ") + storage::OutsideDir
				+ ", and this start cannot use them: " + outsideProblem() + ". Until that is put right, PSSwanStation "
				"uses the files in its own folder (" + shownApp() + "), which are as they were when the files moved "
				"out. A game started now saves there, not to your files outside.");
}

// ----------------------------------------------------- rows for other pages

namespace
{
// The emulator settings a picture preset sets, for every game.
struct Preset
{
	const char *key;
	const char *values[3];		// original, sharp, enhanced
};
const Preset presetOptions[] = {
	{ "swanstation_GPU_ResolutionScale", { "1", "8", "8" } },
	{ "swanstation_GPU_TextureFilter", { "Nearest", "Nearest", "xBR" } },
	{ "swanstation_GPU_TrueColor", { "false", "true", "true" } },
	{ "swanstation_GPU_ScaledDithering", { "false", "true", "true" } },
	{ "swanstation_GPU_PGXPEnable", { "false", "true", "true" } },
	{ "swanstation_GPU_PGXPCulling", { "false", "true", "true" } },
	{ "swanstation_GPU_PGXPTextureCorrection", { "false", "true", "true" } },
	{ "swanstation_GPU_MSAA", { "1", "1", "4" } },
	{ "swanstation_GPU_DownsampleMode", { "Disabled", "Disabled", "Disabled" } },
};

// For every game, or for the loaded one alone.
void applyPreset(int preset, bool forGame)
{
	options::setPicture("preset", preset, forGame);
	if (preset >= 1 && preset <= 3)
	{
		for (const Preset& option : presetOptions)
			if (options::find(option.key) != nullptr)
				options::set(option.key, option.values[preset - 1], forGame);
		// The original picture is what a picture tube showed: its lines too.
		options::setPicture("crt", preset == 1 ? 2 : 0, forGame);
		options::setPicture("linear_filter", 1, forGame);
	}
}

// The game goes back to the picture settings every game has.
void clearGamePreset()
{
	for (const Preset& option : presetOptions)
		options::clearGameValue(option.key);
	for (const char *name : { "preset", "crt", "linear_filter" })
		options::clearGamePicture(name);
}

// A row for one of the picture settings a game can have of its own
// (options::setPicture): in Settings it shows and sets the value for every
// game, in Game settings the loaded game's.
Item pictureChoice(bool forGame, const char *name, const std::string& label, std::vector<std::string> names,
		const std::string& info, std::function<void()> changed = {})
{
	Item item;
	item.label = label;
	item.info = info;
	item.choices = std::move(names);
	item.current = std::clamp(options::picture(name, forGame), 0, (int)item.choices.size() - 1);
	item.value = item.choices[item.current];
	const bool own = options::hasGamePicture(name);
	const std::string key = name;
	item.choose = [key, forGame, changed](int index) {
		options::setPicture(key, index, forGame);
		if (changed)
			changed();
	};
	if (forGame)
	{
		if (own)
		{
			item.mark = 1;
			item.alt = [key, changed] {
				options::clearGamePicture(key);
				if (changed)
					changed();
			};
			item.altHint = "Use the general value";
		}
	}
	else if (own)
		item.info += "\n\nThe running game has a value of its own for this (Game settings).";
	return item;
}

}

void pictureItems(bool forGame, std::vector<Item>& items)
{
	const auto now_ = [forGame](const char *name) { return options::picture(name, forGame); };
	const int scaling = now_("scaling");
	items.push_back(pictureChoice(forGame, "scaling", "Scaling",
			{ "Fit the screen", "Whole multiples", "Stretch", "Fit the screen, FSR 1" },
			"Fit keeps the picture's shape and makes it as large as the screen allows. Whole multiples only "
			"enlarges by 2x, 3x and so on, which keeps the software renderer's pixels even. Stretch fills the "
			"screen and distorts. FSR 1 fits the screen too, and enlarges with AMD's FidelityFX Super Resolution 1 "
			"instead of a plain filter: edges stay clean and the picture is sharpened, so a lower Internal "
			"Resolution Scale (Enhancement) looks closer to a high one and costs less. It is the kind of FSR that "
			"works on one finished picture; the later kinds need things an emulated PlayStation does not give. "
			"A picture already as large as the screen is left as it is."));
	{
		Item item = pictureChoice(forGame, "fsr_sharpness", "FSR sharpening", { "Soft", "Normal", "Sharp" },
				"How strongly FSR 1 sharpens the enlarged picture.");
		item.enabled = scaling == 3;
		items.push_back(item);
	}
	{
		Item item = pictureChoice(forGame, "linear_filter", "Smooth scaling", { "Off", "On" },
				"Blends neighbouring pixels when the picture is enlarged to the screen. Off shows them as sharp squares.");
		if (scaling == 3)
		{
			item.enabled = false;
			item.value = "FSR 1";
		}
		items.push_back(item);
	}
	{
		Item item = pictureChoice(forGame, "preset", "Picture preset", { "As set", "Original", "Sharp", "Enhanced" },
				std::string(forGame ? "Sets the emulator's picture settings for this game alone, at once."
				: "Sets the emulator's picture settings for every game at once.") + " Original: the PlayStation's own "
				"resolution and colours, with the lines of a picture tube. Sharp: eight times the resolution, full "
				"colour, steadier polygons (PGXP), nothing smoothed. Enhanced: as Sharp, with smoothed textures (xBR) "
				"and smoothed edges (4x MSAA), which costs speed. Each setting can still be changed by itself (Display, "
				"Enhancement)" + (forGame ? "." : "; a game's own settings stay above these."));
		item.choose = [forGame](int i) { applyPreset(i, forGame); };
		if (forGame && item.alt)
		{
			item.alt = [] { clearGamePreset(); };
			item.altHint = "Use the general values";
		}
		items.push_back(item);
	}
	items.push_back(pictureChoice(forGame, "crt", "Picture tube",
			{ "Off", "Soft scanlines", "Scanlines", "Scanlines and mask" },
			"Draws the dark between the lines a television's picture tube left, over the game's picture: the "
			"look these games were made for. The mask adds the tube's fine vertical stripes. Darkens the picture "
			"somewhat."));
	items.push_back(pictureChoice(forGame, "border", "Beside the picture",
			{ "Black", "The picture's light", "A gradient", "A picture file" },
			"What fills the screen left and right of a 4:3 picture. The picture's light: its own colours, soft and "
			"dim, as a lit screen throws them on a wall. A picture file: " + shownRoot() + "borders/<serial>.png "
			"for one game (SLUS-12345.png), or default.png for all; .jpg works too.",
			[] { display::forgetAmbient(); }));
	items.push_back(pictureChoice(forGame, "pacing", "Frame pacing",
			{ "By the display", "The game's own speed", "The game's own speed, by the clock" },
			"By the display: one frame of the game for each refresh when their rates are within one percent, and "
			"the sound stretched to match; no stutter and no tearing, and the usual choice. The game's own speed: "
			"its exact rate, with a frame dropped or shown twice now and then. By the clock: its exact rate, each "
			"frame handed to the display the moment it is due, which is right for a display with a variable "
			"refresh rate (and for PAL games on one); on other displays it behaves as the second."));
	if (forGame)
		items.push_back(fact("Display output", "For every game",
				"60 Hz or 120 Hz is one setting for every game: the console reads it when PSSwanStation starts, before "
				"any game is chosen. It is in Settings, Picture."));
	else
	{
		options::Frontend& f = options::frontend();
		Item item = choice("Display output", f.displayMode, { "60 Hz", "120 Hz", "120 Hz, variable refresh rate" },
				format("What the console is asked for, from the next start. It is at %.2f Hz now. 120 Hz: menus "
				"move at twice the rate, a PAL game's frames fall more evenly, and black frame insertion "
				"becomes possible; a display that cannot do 120 Hz stays at 60. The third declares, as games "
				"that support it do, that a variable refresh rate may stay on: use it with \"by the clock\" "
				"above. An experiment, not yet run on a console: if the screen stays dark afterwards, hold "
				"L1 and R1 while PSSwanStation starts. This one is for every game: the console reads it when "
				"PSSwanStation starts, before any game is chosen.", display::outputRefreshRate()),
				[](int i) {
					options::frontend().displayMode = i;
					storage::syncDisplayMode(appDir + "sce_sys/param.json", i);
				});
		items.push_back(item);
	}
	const int generation = now_("frame_generation");
	{
		Item item = pictureChoice(forGame, "black_frames", "Black frame insertion", { "Off", "On" },
				"At 120 Hz, every second refresh shows black instead of the same frame again, as a picture tube "
				"went dark between frames: movement is clearer, the picture darker, and some see it flicker. Only "
				"for games at the display's half rate (60 fps games at 119.88 Hz) with pacing by the display.");
		if (display::refreshRate() < 100.f)
		{
			item.enabled = false;
			item.value = "Needs 120 Hz";
		}
		else if (generation != 0)
		{
			item.enabled = false;
			item.value = "Frame generation is on";
		}
		items.push_back(item);
	}
	{
		// What the running game gets of it, in words.
		std::string running;
		if (host::running() && options::frontend().frameGeneration != 0)
		{
			const double pictures = host::picturesPerSecond(), shows = display::refreshRate();
			running = pictures > 0 ? format("\n\nThis game is drawing about %.0f pictures a second now, and the screen shows "
					"%.0f: %s.", pictures, shows, pictures + 1.0 < shows ? "pictures are made between them"
					: "none needs making")
					: format("\n\nThis game does not let its pictures be counted (it draws straight to the screen): each "
					"of its frames is taken for a new one, and the screen shows %.0f.", shows);
		}
		items.push_back(pictureChoice(forGame, "frame_generation", "Frame generation", { "Off", "On", "On, lighter" },
				"Draws pictures of its own between the game's, from how the picture moved, so that movement is "
				"smoother than the game makes it. The title counts how many pictures a second the game really "
				"draws: most PlayStation games draw 30, 20 or fewer, and those are filled up to the screen's 60 "
				"(or 120). A game that already draws as many as the screen shows is left alone; at 120 Hz every "
				"game gains. The made pictures are guesses: where the title cannot tell how something moved it "
				"shows the game's own picture there, a moment's stutter in that place. The game answers the pad "
				"a refresh or two of the screen later. \"Lighter\" asks less of the graphics processor and decides more "
				"coarsely: for when the frame rate drops with it on. Not while fast forwarding or rewinding." + running,
				[] { display::forgetGenerated(); }));
	}
}

void moreSettings(int kind, std::vector<Item>& items)
{
	options::Frontend& f = options::frontend();
	switch (kind)
	{
	case 0:
		items.push_back(toggle("Clock", &f.clock, "The time, in the library's header. It is the console's clock; a console "
				"whose clock was never set shows none."));
		{
			static const int minutes[5] = { 0, 2, 5, 10, 20 };
			int now_ = 0;
			for (int i = 0; i < 5; i++)
				if (minutes[i] == f.idleMinutes)
					now_ = i;
			items.push_back(choice("The swan takes the screen", now_, { "Never", "After 2 minutes", "After 5 minutes",
					"After 10 minutes", "After 20 minutes" },
					"When no button was pressed for this long in the library or a menu, the screen goes dark and the swan "
					"swims across it, which spares a screen that keeps what it shows for long. Any button brings the "
					"menus back. Not while a game is on the screen.",
					[](int i) { options::frontend().idleMinutes = minutes[i]; }));
		}
		items.push_back(toggle("Ask for updates at start", &f.updateCheck, "Asks the releases page, when PSSwanStation "
				"starts, whether a newer build is out, and says so in the library's header. Nothing is fetched until "
				"you say so (Menu, Update)."));
		break;
	case 2:
		items.push_back(choice("Menu music", f.music, { "None", "The title's own", "My file" },
				"Music while the library or a menu has the screen. The title's own is a quiet piece computed when it "
				"is first played, like the other sounds. My file: menu.ogg, menu.mp3 or menu.wav in " + shownRoot()
				+ "music, played round and round (the first six minutes of it).\n\n" + (f.music == 2 ? sound::musicStatus()
				: std::string()),
				[](int i) { options::frontend().music = i; }));
		items.push_back(choice("Music volume", f.musicVolume / 10,
				{ "0%", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%" },
				"How loud the menus' music is, under the Volume above.",
				[](int i) { options::frontend().musicVolume = i * 10; }));
		break;
	case 3:
		items.push_back(action(icon::Gamepad, "Buttons", "Which button of the pad presses which of the PlayStation's, "
				"and which fire again and again while held (turbo).", [] { push(Page::Buttons); }));
		{
			Item item = choice("Tilt steering", f.motion, { "Off", "On" },
					"Player 1's pad, leant to the left or right like a steering wheel, moves the left stick that way: "
					"with a DualShock, an analog joystick or a neGcon chosen above, that steers. An experiment: the "
					"pad's motion sensor has not been read on a console by this title yet.",
					[](int i) { options::frontend().motion = i; });
			items.push_back(item);
			Item range = choice("Tilt for the whole way", f.motionRange, { "20 degrees", "30 degrees", "40 degrees",
					"55 degrees", "70 degrees" },
					"How far the pad is leant for the stick to be all the way over.",
					[](int i) { options::frontend().motionRange = i; });
			range.enabled = f.motion != 0;
			items.push_back(range);
			Item invert = toggle("Tilt the other way round", &f.motionInvert, "Should leaning to the right steer left.");
			invert.enabled = f.motion != 0;
			items.push_back(invert);
		}
		items.push_back(toggle("Player lights", &f.playerLights, "Each pad's light bar in its player's colour: blue, "
				"red, green and pink for players 1 to 4."));
		items.push_back(fact("Battery", "Not shown", "A pad's charge is not shown: of the console's libraries a title "
				"can call, none that this title knows of gives it. The console's own control centre (the PS button) shows it."));
		break;
	case 4:
		{
			Item item = action(icon::Card, "Memory cards", "What is saved on each card: copy, delete, bring in, put out, "
					"and go back to an earlier copy.", [] { push(Page::Cards); }, !host::running());
			if (host::running())
			{
				item.value = "Close the game first";
				item.info += " Not while a game runs: the emulator holds its card.";
			}
			items.push_back(item);
		}
		{
			static const int counts[4] = { 0, 5, 10, 20 };
			int now_ = 2;
			for (int i = 0; i < 4; i++)
				if (counts[i] == f.cardBackups)
					now_ = i;
			items.push_back(choice("Card backups", now_, { "None", "The last 5", "The last 10", "The last 20" },
					"When a game closes and its memory card is not what it was, a copy of the card is kept (in "
					+ shownRoot() + "data/saves/backups), and of each card the newest few. The memory card manager "
					"goes back to one.",
					[](int i) { options::frontend().cardBackups = counts[i]; }));
		}
		{
			const std::string address = smb::wakeAddress();
			Item item = action(icon::Bolt, "Wake the server", address.empty()
					? "A server that sleeps can be woken before it is asked for games (Wake-on-LAN): name its network "
					"card's hardware address in " + shownRoot() + "network.cfg, as  wake = 00:11:32:AA:BB:CC  . It is then "
					"woken when PSSwanStation starts and whenever the share is scanned."
					: "Sends the wake-up packet to " + address + " now. That also happens when PSSwanStation starts and "
					"whenever the share is scanned.",
					[] { host::addMessage(smb::wake() ? "The wake-up packet was sent." : "The wake-up packet could not be sent."); },
					!address.empty());
			item.value = address.empty() ? "Not set" : address;
			items.push_back(item);
		}
		{
			Item item = action(icon::Download, "Fetch the newest cheat database", "The cheats and patches come from the "
					"DuckStation project's chtdb, which grows. A release of PSSwanStation does not carry it (its entries "
					"belong to their authors): this fetches the project's newest release from its own page and uses it "
					"from then on (it is kept in " + shownRoot() + "data). Now: " + cheats::summary() + ".\n\n"
					+ cheats::refreshStatus(),
					[] { cheats::refresh(); }, !cheats::refreshing() && httpAvailable());
			item.value = cheats::refreshing() ? "Fetching\xe2\x80\xa6" : !httpAvailable() ? "No network" : "";
			items.push_back(item);
		}
		{
			Item item = toggle("Keep my files outside the title folder", &f.outside,
					std::string("Keeps BIOS files, covers, cheats, memory cards, states and settings in ")
					+ storage::OutsideDir + " instead of the title's folder, so that replacing or deleting that "
					"folder cannot take them with it. Games may be in either folder's games. From the next start: "
					"what the title's folder holds is copied over once (and left where it is), and PSSwanStation has "
					"to leave its sandbox each time it starts, which needs a resident Lapy service or the ELF loader "
					"on port 9021. Without that it cannot reach the files and says so.");
			item.info += f.outside ? (outsideAvailable() ? "\n\nIn use now." : "\n\nSet, and not in use now: "
					+ (outsideProblem().empty() ? std::string("it takes effect from the next start") : outsideProblem()) + ".")
					: std::string();
			items.push_back(item);
		}
		{
			const std::string serial = host::game().serial;
			std::string where;
			const int files = host::texturePackFiles(serial, &where);
			items.push_back(fact("Texture packs", host::running() ? (files > 0 ? format("%d files for this game", files)
					: std::string("None for this game")) : std::string("By game"),
					"Replacement pictures for a game's backgrounds and other 2D art, as made for DuckStation (files "
					"named vram-write-<number>.png). They go in " + shownRoot() + "textures/<serial>/ (for one game: "
					"textures/SLUS-12345/) or, on a USB drive, in a folder named textures inside its games folder "
					"(psx/textures/SLUS-12345/), and are used when \"Enable VRAM Write Texture Replacement\" is on "
					"(Enhancement). This emulator knows that one kind of pack, not the newer kind that replaces a 3D "
					"game's textures." + (files > 0 ? "\n\nThis game's is " + where + "." : std::string())));
		}
		break;
	case 5:
		items.push_back(toggle("Shortcuts", &f.hotkeys,
				"While a game runs, OPTIONS held with another button does something at once: R2 fast forward, L2 "
				"rewind, R1 and L1 save and load a state. With this on, the menu opens when "
				"OPTIONS is let go; with it off, the moment it is pressed."));
		items.push_back(action(icon::List, "What the shortcuts are", "", [] { push(Page::Shortcuts); }));
		items.push_back(choice("Fast forward speed", f.fastForward, { "2x", "3x", "4x", "8x", "As fast as it goes" },
				"How fast the game runs while OPTIONS and R2 are held. The console may not manage the highest.",
				[](int i) { options::frontend().fastForward = i; }));
		items.push_back(toggle("Rewind", &f.rewind,
				"Keeps the last while of play so that OPTIONS and L2 can go back through it. The states are kept in "
				"memory only, never written to the console's storage, and are gone when the game closes. Taking them "
				"costs a little speed while it is on. Off in netplay and in RetroAchievements' hardcore mode."));
		{
			Item detail = choice("Rewind steps", f.rewindDetail, { "Fine (20 a second)", "Normal (10 a second)",
					"Coarse (5 a second)" },
					"How often a state is kept. Finer steps rewind more smoothly, cost more speed and fill the memory sooner.",
					[](int i) { options::frontend().rewindDetail = i; });
			detail.enabled = f.rewind;
			items.push_back(detail);
			Item memory = choice("Rewind memory", f.rewindMemory, { "128 MB", "256 MB", "512 MB", "1 GB" },
					"How much memory the kept states may take. How long that is depends on the game: a quiet scene "
					"changes little from state to state, a busy one much." + (host::running() && f.rewind
					? format("\n\nKept now: %.0f seconds of play.", host::rewindSeconds()) : std::string()),
					[](int i) { options::frontend().rewindMemory = i; });
			memory.enabled = f.rewind;
			items.push_back(memory);
		}
		break;
	case 6:
	{
		const achievements::Summary summary = achievements::summary();
		items.push_back(toggle("RetroAchievements", &f.achievements,
				"Achievements for PlayStation games from retroachievements.org: goals made by its community, earned "
				"while you play and kept on your account there. Needs an account (free) and the console on the "
				"internet. A game's achievements are in its menu while it runs.",
				[] { host::achievementsApply(); }));
		{
			Item item = action(icon::User, "Account", "Sign in or out.", [] { push(Page::Account); }, f.achievements);
			item.value = summary.loggedIn ? summary.user : summary.loggingIn ? "Signing in\xe2\x80\xa6"
					: host::achievementsUser().empty() ? "Not signed in" : host::achievementsUser() + " (not signed in)";
			if (!summary.lastError.empty() && !summary.loggedIn)
				item.info += "\n\n" + summary.lastError;
			items.push_back(item);
		}
		{
			Item item = toggle("Hardcore mode", &f.hardcore,
					"RetroAchievements' stricter way to play, which earns more: no save states are loaded, no cheats, no "
					"rewind. Switching it on while a game runs resets the game.",
					[] { host::achievementsApply(); });
			item.enabled = f.achievements;
			items.push_back(item);
			Item unofficial = toggle("Unofficial achievements", &f.unofficial,
					"Also shows achievements that are still being made and tested. They earn nothing. From the next game "
					"that starts.", [] { host::achievementsApply(); });
			unofficial.enabled = f.achievements;
			items.push_back(unofficial);
		}
		break;
	}
	}
}

void detailsMoreItems(std::vector<Item>& items)
{
	const library::Game game = detailsGame();
	{
		Item item;
		item.icon = icon::Heart;
		item.label = "Favourite";
		item.choices = { "No", "Yes" };
		item.current = library::favourite(game.path) ? 1 : 0;
		item.value = item.choices[(size_t)item.current];
		item.info = "Favourites have a heart on their cover, and the library can show them alone (Square, in the library).";
		item.choose = [path = game.path](int i) { library::setFavourite(path, i != 0); };
		items.push_back(item);
	}
	{
		Item item;
		item.icon = icon::EyeOff;
		item.label = "Hidden";
		item.choices = { "No", "Yes" };
		item.current = library::hidden(game.path) ? 1 : 0;
		item.value = item.choices[(size_t)item.current];
		item.info = "A hidden game is out of the library and the search. It comes back from \"Hidden games\" (Square, in the library).";
		item.choose = [path = game.path](int i) { library::setHidden(path, i != 0); };
		items.push_back(item);
	}
	items.push_back(action(icon::Picture, "Cover", "Another picture as this game's cover: its box, its title screen or a "
			"moment of the game.", [] { push(Page::Cover); }));
	{
		const std::string serial = detailsSerial_();
		std::string where;
		const int files = host::texturePackFiles(serial, &where);
		items.push_back(fact("Texture pack", serial.empty() ? std::string("Needs the serial") : files > 0
				? format("%d files", files) : std::string("None"),
				serial.empty() ? std::string("The pack's folder is named by the disc's serial number, which is read when "
				"Options or Cheats is opened, or when the game starts.")
				: files > 0 ? "In " + where + ". Used when \"Enable VRAM Write Texture Replacement\" is on (Options)."
				: "Replacement pictures for this game go in " + shownRoot() + "textures/" + serial + "/ or, on a USB "
				"drive, in its games folder's textures/" + serial + "/, and are used when \"Enable VRAM Write Texture "
				"Replacement\" is on (Options)."));
	}
}

void pauseMoreItems(std::vector<Item>& items)
{
	if (options::frontend().achievements)
	{
		const achievements::Summary summary = achievements::summary();
		Item item = action(icon::Trophy, "Achievements", "This game's achievements at RetroAchievements, and what is "
				"earned of them.", [] { push(Page::Achievements); });
		item.value = summary.gameLoaded ? format("%d of %d", summary.unlocked, summary.total)
				: summary.gameLoading ? "Asking\xe2\x80\xa6" : !summary.loggedIn ? "Not signed in" : "None";
		items.push_back(item);
	}
	{
		Item item = action(icon::Users, "Netplay", "Play this game with someone on another console, over the network: "
				"one console hosts, the other joins it.", [] { push(Page::Netplay, netplay::active() ? 1 : 0); });
		if (netplay::active())
			item.value = netplay::state() == netplay::State::Playing ? "Playing" : "Starting";
		items.push_back(item);
	}
	if (options::frontend().hotkeys)
		items.push_back(action(icon::Forward, "Shortcuts", "Fast forward, rewind and states without the "
				"menu: what to hold.", [] { push(Page::Shortcuts); }));
}

}
