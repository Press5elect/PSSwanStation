/*
	SwanStation for PS5 - the interface: the library, the menus, the settings.

	SPDX-License-Identifier: GPL-3.0-or-later

	Screens are pages on a stack. With the stack empty the screen is the
	library (no game) or the game itself; OPTIONS puts the first page on it
	and Circle takes the top one off. A page is drawn from the current state
	every frame (the lists are built anew each time), so what the screen shows
	is always what is set.

	The controls: the d-pad or the left stick moves, Cross confirms and Circle
	goes back (or the other way round, a setting), Square and Triangle are a
	page's second and third action, L1/R1 change tab. In a game the two halves
	of the touch pad are the PlayStation's Select and Start, and OPTIONS opens
	the menu.
*/
#include "ui.h"
#include "display.h"

#include <libretro.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace fe::ui
{
namespace
{

using namespace platform;

// ------------------------------------------------------------------- input

struct Input
{
	uint32_t held = 0, pressed = 0, repeat = 0;
};
Input in;
uint32_t confirmButton = Cross, cancelButton = Circle;

void readInput()
{
	static uint32_t before;
	static double repeatAt;
	uint32_t held = 0;
	for (int i = 0; i < MaxPads; i++)
	{
		const Pad& p = pad(i);
		if (!p.connected)
			continue;
		held |= p.buttons;
		if (p.lx < -0.6f)
			held |= Left;
		if (p.lx > 0.6f)
			held |= Right;
		if (p.ly < -0.6f)
			held |= Up;
		if (p.ly > 0.6f)
			held |= Down;
		if (p.l2 > 0.5f)
			held |= L2;
		if (p.r2 > 0.5f)
			held |= R2;
	}
	in.held = held;
	in.pressed = held & ~before;
	in.repeat = in.pressed;
	const uint32_t repeating = Up | Down | Left | Right | L2 | R2;
	const double t = now();
	if ((held & repeating) != (before & repeating))
		repeatAt = t + 0.36;
	else if ((held & repeating) != 0 && t >= repeatAt)
	{
		in.repeat |= held & repeating;
		repeatAt = t + 0.07;
	}
	before = held;
	const bool swap = options::frontend().swapConfirm;
	confirmButton = swap ? Circle : Cross;
	cancelButton = swap ? Cross : Circle;
}

bool hit(uint32_t button)
{
	return (in.pressed & button) != 0;
}

bool nav(uint32_t button)
{
	return (in.repeat & button) != 0;
}

void consumeInput()
{
	in.pressed = in.repeat = 0;
}

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
	Loading,	// a network game being read; a: state slot
	Message,	// s: title, s2: text
	Confirm,	// s: title, s2: text, action
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

std::vector<Frame> stack;
// What a page asked for; done after the frame is drawn, when no reference
// into the stack or a page's list is alive.
std::function<void()> deferred;
// Closing the game takes its picture away, and the frame being drawn still
// shows it: that waits for the start of the next frame.
bool closeGame;
bool quit;
library::Game chosen;

void push(Page page, int a = 0, int b = 0, const std::string& s = "", const std::string& s2 = "",
		std::function<void()> action = {})
{
	Frame frame;
	frame.page = page;
	frame.a = a;
	frame.b = b;
	frame.s = s;
	frame.s2 = s2;
	frame.action = std::move(action);
	frame.opened = clock();
	stack.push_back(std::move(frame));
}

void pop()
{
	if (!stack.empty())
		stack.pop_back();
}

void message(const std::string& title, const std::string& text)
{
	push(Page::Message, 0, 0, title, text);
}

// Logical size of the screen, in units (1920 x 1080 at the normal size).
float unitsWide()
{
	return width() / px(1000) * 1000.f;
}

float unitsHigh()
{
	return height() / px(1000) * 1000.f;
}

ImVec2 at(float x, float y)
{
	return ImVec2(px(x), px(y));
}

float toUnits(float pixels)
{
	return pixels * 1000.f / px(1000);
}

// How high wrapped body text is, in units.
float fontsHeight(const std::string& value, float wrapUnits, float size = 23)
{
	return toUnits(wrappedHeight(value, px(wrapUnits), Body, size));
}

// ---------------------------------------------------------------- starting

void begin(const std::string& path, int slot)
{
	if (host::start(path, slot))
	{
		stack.clear();
		return;
	}
	std::string why = host::lastError();
	if (why.empty())
		why = "The emulator could not start this game.";
	message("The game did not start", why);
}

// For launch(): from the beginning, whatever "continue where I left off" says.
constexpr int FromBeginning = -3;

void launch(const library::Game& game, int slot)
{
	if (slot == -1 && options::frontend().autoLoadOnStart && host::stateExistsFor(game.path, host::ResumeSlot))
		slot = host::ResumeSlot;
	if (slot == FromBeginning)
		slot = -1;
	const bool network = smb::isNetworkPath(game.path)
			|| (!game.discs.empty() && smb::isNetworkPath(game.discs.front()));
	if (network)
	{
		chosen = game;
		push(Page::Loading, slot, 0, game.path, game.name);
		smb::startPrecache(game.path);
	}
	else
		begin(game.path, slot);
}

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
};

Item header(const std::string& label)
{
	Item item;
	item.label = label;
	item.header = true;
	return item;
}

Item action(const char *icon, const std::string& label, const std::string& info, std::function<void()> activate,
		bool enabled = true)
{
	Item item;
	item.icon = icon;
	item.label = label;
	item.info = info;
	item.activate = std::move(activate);
	item.enabled = enabled;
	return item;
}

Item fact(const std::string& label, const std::string& value, const std::string& info = "")
{
	Item item;
	item.label = label;
	item.value = value;
	item.info = info.empty() ? value : info;
	return item;
}

void pickerPanel(Frame& f, const Item& item)
{
	const int count = (int)item.choices.size();
	f.pickCursor = std::clamp(f.pickCursor, 0, count - 1);
	if (nav(Down))
		f.pickCursor = f.pickCursor + 1 < count ? f.pickCursor + 1 : (hit(Down) ? 0 : f.pickCursor);
	if (nav(Up))
		f.pickCursor = f.pickCursor > 0 ? f.pickCursor - 1 : (hit(Up) ? count - 1 : f.pickCursor);
	if (nav(R2))
		f.pickCursor = std::min(f.pickCursor + 8, count - 1);
	if (nav(L2))
		f.pickCursor = std::max(f.pickCursor - 8, 0);
	if (hit(confirmButton))
	{
		const int index = f.pickCursor;
		if (item.choose)
			deferred = [choose = item.choose, index] { choose(index); };
		f.picker = -1;
		consumeInput();
		return;
	}
	if (hit(cancelButton))
	{
		f.picker = -1;
		consumeInput();
		return;
	}

	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const float rowH = 56;
	const int visible = std::min(count, 12);
	const float panelW = 760, panelH = 96 + visible * rowH + 20;
	const float x0 = (W - panelW) * 0.5f, y0 = (H - panelH) * 0.5f;
	draw()->AddRectFilled(ImVec2(0, 0), ImVec2(width(), height()), IM_COL32(0, 0, 0, 150));
	panel(at(x0, y0), at(x0 + panelW, y0 + panelH), IM_COL32(24, 30, 48, 252), 18);
	outline(at(x0, y0), at(x0 + panelW, y0 + panelH), IM_COL32(255, 255, 255, 26), 18, 1.5f);
	textFit(at(x0 + 32, y0 + 26), px(panelW - 64), t.text, item.label, Bold, 30);

	const float listTop = y0 + 88;
	float target = f.pickScroll;
	if (f.pickCursor * rowH < target)
		target = f.pickCursor * rowH;
	if ((f.pickCursor + 1) * rowH > target + visible * rowH)
		target = (f.pickCursor + 1) * rowH - visible * rowH;
	f.pickScroll = target;
	draw()->PushClipRect(at(x0, listTop), at(x0 + panelW, listTop + visible * rowH), true);
	for (int i = 0; i < count; i++)
	{
		const float y = listTop + i * rowH - f.pickScroll;
		if (y + rowH < listTop || y > listTop + visible * rowH)
			continue;
		const bool focused = i == f.pickCursor;
		if (focused)
		{
			panel(at(x0 + 14, y + 3), at(x0 + panelW - 14, y + rowH - 3), t.panelHigh, 12);
			panel(at(x0 + 14, y + 12), at(x0 + 20, y + rowH - 12), t.accent, 3);
		}
		if (i == item.current)
			text(at(x0 + 36, y + 14), t.accent, icon::Check, Body, 24);
		textFit(at(x0 + 84, y + 13), px(panelW - 130), focused ? t.text : t.dim, item.choices[i], Body, 26);
	}
	draw()->PopClipRect();
	if (count > visible)
	{
		const float trackH = visible * rowH;
		const float barH = std::max(trackH * visible / count, 30.f);
		const float barY = listTop + (trackH - barH) * (f.pickScroll / ((count - visible) * rowH));
		panel(at(x0 + panelW - 10, barY), at(x0 + panelW - 5, barY + barH), IM_COL32(255, 255, 255, 60), 3);
	}
}

// Draws a list in the box and works it with the pad. The item under the
// cursor, or null.
const Item *runList(Frame& f, std::vector<Item>& items, float x0, float y0, float x1, float y1)
{
	const int count = (int)items.size();
	if (count == 0)
		return nullptr;
	const auto selectable = [&](int i) { return i >= 0 && i < count && !items[i].header; };
	f.cursor = std::clamp(f.cursor, 0, count - 1);
	if (!selectable(f.cursor))
	{
		int i = f.cursor;
		while (i < count && !selectable(i))
			i++;
		if (i >= count)
		{
			i = f.cursor;
			while (i >= 0 && !selectable(i))
				i--;
		}
		if (i < 0)
			return nullptr;
		f.cursor = i;
	}

	const bool picking = f.picker >= 0 && f.picker < count && !items[f.picker].choices.empty();
	if (!picking)
	{
		f.picker = -1;
		const auto step = [&](int direction, bool wrap) {
			int i = f.cursor + direction;
			while (i >= 0 && i < count && !selectable(i))
				i += direction;
			if (i >= 0 && i < count)
			{
				f.cursor = i;
				return true;
			}
			if (wrap)
			{
				i = direction > 0 ? 0 : count - 1;
				while (!selectable(i))
					i += direction;
				f.cursor = i;
			}
			return false;
		};
		if (nav(Down))
			step(1, hit(Down));
		if (nav(Up))
			step(-1, hit(Up));
		if (nav(R2))
			for (int n = 0; n < 8 && step(1, false); n++) {}
		if (nav(L2))
			for (int n = 0; n < 8 && step(-1, false); n++) {}

		const Item& item = items[f.cursor];
		const int direction = nav(Right) ? 1 : nav(Left) ? -1 : 0;
		if (direction != 0 && item.enabled)
		{
			if (item.adjust)
				deferred = [adjust = item.adjust, direction] { adjust(direction); };
			else if (!item.choices.empty() && item.choose)
			{
				const int last = (int)item.choices.size() - 1;
				const int index = item.current < 0 ? 0 : std::clamp(item.current + direction, 0, last);
				if (index != item.current)
					deferred = [choose = item.choose, index] { choose(index); };
			}
		}
		if (hit(confirmButton) && item.enabled)
		{
			if (!item.choices.empty() && item.choose)
			{
				if (item.choices.size() == 2)
					deferred = [choose = item.choose, index = item.current == 0 ? 1 : 0] { choose(index); };
				else
				{
					f.picker = f.cursor;
					f.pickCursor = std::max(item.current, 0);
					f.pickScroll = 0;
					consumeInput();
				}
			}
			else if (item.activate)
				deferred = item.activate;
		}
		if (hit(Square) && item.alt && item.enabled)
			deferred = item.alt;
	}

	// The rows.
	const Theme& t = theme();
	const float rowH = 60, headH = 50;
	std::vector<float> top((size_t)count + 1);
	float total = 0;
	for (int i = 0; i < count; i++)
	{
		top[i] = total;
		total += items[i].header ? headH : rowH;
	}
	top[count] = total;
	const float viewH = y1 - y0;
	float target = f.scrollTarget;
	if (top[f.cursor] - target < rowH)
		target = top[f.cursor] - rowH;
	if (top[f.cursor + 1] - target > viewH - rowH)
		target = top[f.cursor + 1] - viewH + rowH;
	target = std::clamp(target, 0.f, std::max(total - viewH, 0.f));
	f.scrollTarget = target;
	f.scroll = f.fresh ? target : approach(f.scroll, target, 20.f);
	f.fresh = false;

	draw()->PushClipRect(at(x0, y0), at(x1, y1), true);
	for (int i = 0; i < count; i++)
	{
		const Item& item = items[i];
		const float y = y0 + top[i] - f.scroll;
		const float h = item.header ? headH : rowH;
		if (y + h < y0 || y > y1)
			continue;
		if (item.header)
		{
			text(at(x0 + 22, y + 20), t.accent, item.label, Bold, 20);
			continue;
		}
		const bool focused = i == f.cursor;
		if (focused)
		{
			panel(at(x0 + 6, y + 3), at(x1 - 6, y + h - 3), t.panelHigh, 12);
			panel(at(x0 + 6, y + 14), at(x0 + 12, y + h - 14), t.accent, 3);
		}
		const ImU32 colour = !item.enabled ? t.faint : focused ? t.text : IM_COL32(214, 220, 234, 255);
		float x = x0 + 30;
		if (item.icon != nullptr)
		{
			text(at(x, y + 17), focused && item.enabled ? t.accent : t.faint, item.icon, Body, 24);
			x += 50;
		}
		// The value, at the right.
		float right = x1 - 28;
		const bool steps = item.enabled && focused && (item.adjust || item.choices.size() > 1);
		if (!item.value.empty() || item.ticked)
		{
			const float maxValue = (x1 - x0) * 0.46f;
			std::string value = item.value;
			const ImU32 valueColour = !item.enabled ? t.faint : item.mark == 2 ? t.good : focused ? t.accent : t.dim;
			if (steps)
			{
				// Arrows either side say Left/Right changes it.
				const float cy = y + h * 0.5f;
				draw()->AddTriangleFilled(at(right, cy), at(right - 9, cy - 8), at(right - 9, cy + 8), t.faint);
				right -= 22;
			}
			if (item.ticked)
			{
				text(at(right - 26, y + 17), t.accent, icon::Check, Body, 24);
				right -= 40;
			}
			const float valueWidth = std::min(toUnits(measure(value, Body, 24).x), maxValue);
			textFit(at(right - valueWidth, y + 16), px(maxValue), valueColour, value, Body, 24);
			right -= valueWidth;
			if (steps)
			{
				const float cy = y + h * 0.5f;
				right -= 14;
				draw()->AddTriangleFilled(at(right - 9, cy), at(right, cy - 8), at(right, cy + 8), t.faint);
				right -= 12;
			}
			if (item.mark == 1)
			{
				draw()->AddCircleFilled(at(right - 16, y + h * 0.5f), px(5), t.accent);
				right -= 26;
			}
			right -= 18;
		}
		else if (item.activate && item.choices.empty() && item.enabled && focused)
		{
			text(at(right - 18, y + 19), t.faint, icon::Chevron, Body, 20);
			right -= 40;
		}
		textFit(at(x, y + 15), px(std::max(right - x, 40.f)), colour, item.label, Body, 26);
	}
	draw()->PopClipRect();
	if (total > viewH)
	{
		const float barH = std::max(viewH * viewH / total, 40.f);
		const float barY = y0 + (viewH - barH) * (f.scroll / (total - viewH));
		panel(at(x1 + 6, barY), at(x1 + 11, barY + barH), IM_COL32(255, 255, 255, 50), 3);
	}

	const Item *focusedItem = &items[f.cursor];
	if (picking)
		pickerPanel(f, items[f.picker]);
	return focusedItem;
}

// ------------------------------------------------------------ the game view

void drawGame(float dim)
{
	ImDrawList *list = ImGui::GetBackgroundDrawList();
	const float W = width(), H = height();
	list->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 255));
	void *texture = host::frameTexture();
	if (texture != nullptr)
	{
		int w = 0, h = 0;
		float aspect = 4.f / 3.f;
		host::frameSize(w, h, aspect);
		float u = 1, v = 1;
		host::frameUv(u, v);
		float dw = W, dh = H;
		const int scaling = options::frontend().scaling;
		if (scaling != 2)
		{
			dh = H;
			if (scaling == 1 && h > 0 && (int)H / h >= 1)
				dh = (float)(((int)H / h) * h);
			dw = dh * aspect;
			if (dw > W)
			{
				dw = W;
				dh = W / aspect;
			}
		}
		const ImVec2 p0(std::floor((W - dw) * 0.5f), std::floor((H - dh) * 0.5f));
		list->AddImage((ImTextureID)texture, p0, ImVec2(p0.x + dw, p0.y + dh), ImVec2(0, 0), ImVec2(u, v));
	}
	if (dim > 0)
		list->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(6, 8, 14, (int)(dim * 255)));
}

void drawMessages()
{
	const Theme& t = theme();
	const std::vector<host::Message> messages = host::messages();
	float y = unitsHigh() - 60;
	for (size_t i = messages.size(); i-- > 0;)
	{
		const host::Message& m = messages[i];
		const float textW = toUnits(measure(m.text, Body, 24).x);
		const float w = std::min(textW + 48, unitsWide() - 96);
		const float h = m.progress >= 0 ? 70.f : 52.f;
		y -= h + 10;
		const float fade = (float)std::clamp((m.until - now()) / 0.4, 0.0, 1.0);
		panel(at(48, y), at(48 + w, y + h), withAlpha(IM_COL32(14, 18, 30, 225), fade), 12);
		textFit(at(72, y + 13), px(w - 48), withAlpha(t.text, fade), m.text, Body, 24);
		if (m.progress >= 0)
			progressBar(at(72, y + 50), at(48 + w - 24, y + 58), (float)m.progress / 100.f);
	}
}

void drawGameOverlay()
{
	const Theme& t = theme();
	drawMessages();
	if (options::frontend().showFps)
	{
		const double want = host::coreFps();
		const float got = host::measuredFps();
		const std::string line = format("%.1f fps  %d%%", got, want > 1 ? (int)std::lround(got / want * 100.0) : 0);
		const float w = toUnits(measure(line, Bold, 22).x) + 32;
		const float x = unitsWide() - w - 32;
		panel(at(x, 28), at(x + w, 68), IM_COL32(10, 12, 20, 190), 10);
		text(at(x + 16, 36), got + 0.6f >= (float)want ? t.good : t.bad, line, Bold, 22);
	}
	// A streamed network game waiting for the share.
	const smb::Status status = smb::status();
	if (!status.text.empty())
	{
		const float w = toUnits(measure(status.text, Body, 24).x) + 56;
		const float x = (unitsWide() - w) * 0.5f;
		panel(at(x, 36), at(x + w, 88), IM_COL32(10, 12, 20, 215), 14);
		text(at(x + 28, 48), t.text, status.text, Body, 24);
	}
}

// --------------------------------------------------------------- the library

struct LibraryView
{
	std::vector<library::Game> games;
	unsigned generation = ~0u;
	std::vector<std::string> cover;
	std::vector<unsigned> coverSeen;
	int cursor = 0;
	float scroll = 0;
	bool fresh = true;
};
LibraryView views[library::SourceCount];
int source;
float tabGlow[library::SourceCount];

void refreshView(int index)
{
	LibraryView& view = views[index];
	const unsigned generation = library::generation(index);
	if (generation == view.generation)
		return;
	// The cursor stays on its game when the list is replaced.
	const std::string keep = view.cursor < (int)view.games.size() ? view.games[view.cursor].path : "";
	view.games = library::games(index);
	view.generation = generation;
	view.cover.assign(view.games.size(), "");
	view.coverSeen.assign(view.games.size(), ~0u);
	view.cursor = std::clamp(view.cursor, 0, std::max((int)view.games.size() - 1, 0));
	for (size_t i = 0; i < view.games.size(); i++)
		if (view.games[i].path == keep)
			view.cursor = (int)i;
}

// The cover of a game on the screen, looked for on disk when it is first
// shown and again when the downloader says something arrived.
int coverLookups;
Image coverOf(LibraryView& view, int index)
{
	if (view.cover[index].empty() && view.coverSeen[index] != covers::generation() && coverLookups < 6)
	{
		coverLookups++;
		view.coverSeen[index] = covers::generation();
		view.cover[index] = covers::find(view.games[index]);
	}
	return view.cover[index].empty() ? Image() : image(view.cover[index]);
}

std::string sizeText(uint64_t bytes)
{
	if (bytes == 0)
		return "";
	if (bytes >= (1ull << 30))
		return format("%.2f GB", (double)bytes / (double)(1ull << 30));
	// A cue sheet's own size says nothing about the game's.
	if (bytes < (1ull << 20))
		return "";
	return format("%.0f MB", (double)bytes / (double)(1 << 20));
}

std::string usbHint()
{
	if (!options::frontend().usb)
		return "USB drives are off. Turn them on in Settings, Games and network, then restart SwanStation.";
	return library::sourceHint(library::Usb);
}

void drawHeader()
{
	const Theme& t = theme();
	const float W = unitsWide();
	logo(at(56, 30), 64);
	text(at(136, 36), t.text, AppName, Title, 44);

	static const char *icons[library::SourceCount] = { icon::Drive, icon::Plug, icon::Network };
	float x = 470;
	for (int i = 0; i < library::SourceCount; i++)
	{
		const bool selected = i == source;
		tabGlow[i] = approach(tabGlow[i], selected ? 1.f : 0.f, 14.f);
		std::string label = library::sourceName(i);
		if (library::scanned(i) && !views[i].games.empty())
			label += format("  %d", (int)views[i].games.size());
		const float labelW = toUnits(measure(label, Bold, 26).x);
		const float tabW = labelW + 92;
		if (tabGlow[i] > 0.01f)
			panel(at(x, 38), at(x + tabW, 92), withAlpha(t.accentSoft, tabGlow[i]), 27);
		text(at(x + 24, 52), selected ? t.accent : t.faint, icons[i], Body, 24);
		text(at(x + 68, 50), selected ? t.text : t.dim, label, Bold, 26);
		x += tabW + 12;
	}
	buttonGlyph(at(430, 65), 30, L1);
	buttonGlyph(at(x + 32, 65), 30, R1);

	// What is going on, at the right: a scan, cover downloads.
	std::string status;
	if (library::scanning(source))
	{
		status = library::scanStatus(source);
		if (status.empty())
			status = "Scanning\xe2\x80\xa6";
	}
	else
		status = covers::status();
	if (!status.empty())
	{
		const float spin = (float)clock() * 5.f;
		const ImVec2 c = at(W - 72, 65);
		draw()->PathArcTo(c, px(11), spin, spin + 4.4f, 20);
		draw()->PathStroke(t.accent, 0, px(3));
		textRight(at(W - 100, 52), t.dim, status, Body, 22);
	}
}

void emptyLibrary(const std::string& hint, bool busy)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const float cx = W * 0.5f, cy = H * 0.46f;
	textCentred(at(cx, cy - 120), withAlpha(t.accent, 0.8f), busy ? icon::Sync : icon::Disc, Title, 84);
	textCentred(at(cx, cy), t.text, busy ? "Looking for games" : "No games here yet", Bold, 34);
	const float wrap = 1000;
	// Centred as a block: wrapped text is left-aligned inside it.
	const ImVec2 extent = measure(hint, Body, 24);
	const float blockW = std::min(toUnits(extent.x), wrap);
	textWrapped(at(cx - blockW * 0.5f, cy + 64), px(wrap), t.dim, hint, Body, 24);
}

void libraryPage(bool active)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	backdrop();
	for (int i = 0; i < library::SourceCount; i++)
		refreshView(i);

	if (active)
	{
		if (hit(R1))
			source = (source + 1) % library::SourceCount;
		if (hit(L1))
			source = (source + library::SourceCount - 1) % library::SourceCount;
		if (hit(R1 | L1))
		{
			options::frontend().source = source;
			options::saveFrontend();
			views[source].fresh = true;
		}
	}
	// The share is asked the first time its tab is opened.
	if (source == library::Network && !library::scanned(source) && !library::scanning(source)
			&& !smb::gameFolders().empty())
		library::scan(source, false);

	LibraryView& view = views[source];
	const int count = (int)view.games.size();
	const bool grid = options::frontend().view == 0;
	drawHeader();

	const float top = 128, bottom = H - 64;
	coverLookups = 0;
	if (count == 0)
	{
		const bool busy = library::scanning(source);
		std::string hint = source == library::Usb ? usbHint() : library::sourceHint(source);
		if (busy)
			hint = library::scanStatus(source);
		else if (source == library::Network && !smb::lastError().empty())
			hint = smb::lastError() + "\n" + hint;
		emptyLibrary(hint, busy);
	}
	else if (grid)
	{
		const float margin = 64, gap = 26;
		const int columns = std::max((int)((W - margin * 2 + gap) / (232 + gap)), 3);
		const float cell = (W - margin * 2 - gap * (columns - 1)) / columns;
		const float rowH = cell + 74;
		if (active)
		{
			int& c = view.cursor;
			if (nav(Right) && c + 1 < count)
				c++;
			if (nav(Left) && c > 0)
				c--;
			if (nav(Down))
				c = c + columns < count ? c + columns : (c / columns < (count - 1) / columns ? count - 1 : c);
			if (nav(Up) && c - columns >= 0)
				c -= columns;
			if (nav(R2))
				c = std::min(c + columns * 3, count - 1);
			if (nav(L2))
				c = std::max(c - columns * 3, 0);
		}
		const int row = view.cursor / columns;
		const int rows = (count + columns - 1) / columns;
		const float viewH = bottom - top;
		float target = view.scroll;
		// Kept as the screen was, moved only as far as the cursor needs.
		static float targets[library::SourceCount];
		target = targets[source];
		if (row * rowH - target < 12)
			target = row * rowH - 12;
		if ((row + 1) * rowH - target > viewH - 12)
			target = (row + 1) * rowH - viewH + 12;
		target = std::clamp(target, -12.f, std::max(rows * rowH - viewH + 12, -12.f));
		targets[source] = target;
		view.scroll = view.fresh ? target : approach(view.scroll, target, 14.f);
		view.fresh = false;

		draw()->PushClipRect(at(0, top - 8), at(W, bottom), true);
		const int firstRow = std::max((int)(view.scroll / rowH) - 1, 0);
		const int lastRow = std::min((int)((view.scroll + viewH) / rowH) + 1, rows - 1);
		for (int pass = 0; pass < 2; pass++)
			for (int r = firstRow; r <= lastRow; r++)
				for (int col = 0; col < columns; col++)
				{
					const int index = r * columns + col;
					if (index >= count)
						break;
					const bool focused = index == view.cursor;
					// The one under the cursor is drawn last, over its neighbours.
					if (focused != (pass == 1))
						continue;
					const library::Game& game = view.games[index];
					float x = margin + col * (cell + gap);
					float y = top + 12 + r * rowH - view.scroll;
					float size = cell;
					if (focused)
					{
						const float grow = 14 + 2 * (float)std::sin(clock() * 3.0);
						x -= grow * 0.5f;
						y -= grow * 0.5f;
						size += grow;
					}
					const ImVec2 a = at(x, y), b = at(x + size, y + size);
					const Image cover = coverOf(view, index);
					if (focused)
						draw()->AddRectFilled(ImVec2(a.x - px(10), a.y - px(10)), ImVec2(b.x + px(10), b.y + px(10)),
								withAlpha(t.accent, 0.20f), px(18));
					if (cover.id != nullptr)
						imageFit(cover, a, b, 10);
					else
						coverPlaceholder(a, b, game.name, game.region);
					if (focused)
						outline(ImVec2(a.x - px(4), a.y - px(4)), ImVec2(b.x + px(4), b.y + px(4)), t.accent, 13, 4);
					if (game.discs.size() > 1)
					{
						const std::string discs = format("%d discs", (int)game.discs.size());
						const float w = toUnits(measure(discs, Bold, 18).x) + 20;
						panel(at(x + size - w - 8, y + 8), at(x + size - 8, y + 38), IM_COL32(0, 0, 0, 170), 8);
						text(at(x + size - w + 2, y + 13), t.text, discs, Bold, 18);
					}
					const float ty = y + size + (focused ? 6 : 10);
					const float tw = measure(game.name, focused ? Bold : Body, 22).x;
					const float maxW = px(size);
					const float tx = tw < maxW ? a.x + (maxW - tw) * 0.5f : a.x;
					textFit(ImVec2(tx, px(ty)), maxW, focused ? t.text : t.dim, game.name, focused ? Bold : Body, 22);
				}
		draw()->PopClipRect();
	}
	else
	{
		// The list: names at the left, the cover and the facts at the right.
		const float rowH = 62, x0 = 64, x1 = W - 620;
		const float viewH = bottom - top - 16;
		const int visible = std::max((int)(viewH / rowH), 1);
		if (active)
		{
			int& c = view.cursor;
			if (nav(Down))
				c = c + 1 < count ? c + 1 : (hit(Down) ? 0 : c);
			if (nav(Up))
				c = c > 0 ? c - 1 : (hit(Up) ? count - 1 : c);
			if (nav(R2) || nav(Right))
				c = std::min(c + visible, count - 1);
			if (nav(L2) || nav(Left))
				c = std::max(c - visible, 0);
		}
		static float targets[library::SourceCount];
		float target = targets[source];
		if (view.cursor * rowH - target < rowH)
			target = view.cursor * rowH - rowH;
		if ((view.cursor + 1) * rowH - target > viewH - rowH)
			target = (view.cursor + 1) * rowH - viewH + rowH;
		target = std::clamp(target, 0.f, std::max(count * rowH - viewH, 0.f));
		targets[source] = target;
		view.scroll = view.fresh ? target : approach(view.scroll, target, 18.f);
		view.fresh = false;
		panel(at(x0 - 8, top), at(x1 + 8, bottom - 12), t.panel, 16);
		draw()->PushClipRect(at(x0, top + 8), at(x1, bottom - 20), true);
		const int first = std::max((int)(view.scroll / rowH) - 1, 0);
		const int last = std::min(first + visible + 3, count - 1);
		for (int i = first; i <= last; i++)
		{
			const library::Game& game = view.games[i];
			const float y = top + 8 + i * rowH - view.scroll;
			const bool focused = i == view.cursor;
			if (focused)
			{
				panel(at(x0, y + 3), at(x1, y + rowH - 3), t.panelHigh, 12);
				panel(at(x0, y + 14), at(x0 + 6, y + rowH - 14), t.accent, 3);
			}
			const std::string facts = game.region + (game.region.empty() || game.discs.size() < 2 ? "" : "  \xc2\xb7  ")
					+ (game.discs.size() > 1 ? format("%d discs", (int)game.discs.size()) : "");
			const float factsW = toUnits(measure(facts, Body, 20).x);
			textRight(at(x1 - 24, y + 20), t.faint, facts, Body, 20);
			textFit(at(x0 + 28, y + 16), px(x1 - x0 - 70 - factsW), focused ? t.text : IM_COL32(214, 220, 234, 255),
					game.name, Body, 26);
		}
		draw()->PopClipRect();

		const library::Game& game = view.games[view.cursor];
		const float px0 = x1 + 40, px1 = W - 64;
		const float side = px1 - px0;
		const Image cover = coverOf(view, view.cursor);
		if (cover.id != nullptr)
			imageFit(cover, at(px0, top), at(px1, top + side), 12);
		else
			coverPlaceholder(at(px0, top), at(px1, top + side), game.name, game.region);
		float y = top + side + 24;
		y += toUnits(textWrapped(at(px0, y), px(side), t.text, game.name, Bold, 28, px(76))) + 10;
		const std::string facts = game.region + (game.region.empty() || game.size == 0 ? "" : "  \xc2\xb7  ")
				+ sizeText(game.size);
		text(at(px0, y), t.dim, facts, Body, 22);
	}

	std::string left;
	if (count > 0)
		left = format("%d of %d", view.cursor + 1, count);
	std::vector<Hint> hints;
	if (count > 0)
	{
		hints.push_back({ confirmButton, "Start" });
		hints.push_back({ Triangle, "Details" });
	}
	hints.push_back({ Square, "Scan" });
	hints.push_back({ Options, "Menu" });
	hintBar(hints, left);

	if (!active)
		return;
	if (hit(Options))
		push(Page::MainMenu);
	else if (hit(Square))
		library::scan(source, true);
	else if (count > 0 && hit(confirmButton))
	{
		const library::Game game = view.games[view.cursor];
		deferred = [game] { launch(game, -1); };
	}
	else if (count > 0 && hit(Triangle))
	{
		chosen = view.games[view.cursor];
		push(Page::Details);
	}
}

// ---------------------------------------------------------------- settings

struct SettingsCategory
{
	std::string name, key;
	const char *icon;
	int kind;	// 0..4 the frontend's pages, 10 a category of the core, 11 its uncategorised, 20 about
	std::string info;
};

std::vector<SettingsCategory> settingsCategories(bool forGame)
{
	std::vector<SettingsCategory> list;
	if (!forGame)
	{
		list.push_back({ "Interface", "", icon::Paint, 0, "" });
		list.push_back({ "Picture", "", icon::Screen, 1, "" });
		list.push_back({ "Sound", "", icon::Volume, 2, "" });
		list.push_back({ "Controllers", "", icon::Gamepad, 3, "" });
		list.push_back({ "Games and network", "", icon::Server, 4, "" });
	}
	bool uncategorised = false;
	for (const options::Option& option : options::all())
		if (option.category.empty())
			uncategorised = true;
	if (uncategorised)
		list.push_back({ "Emulator", "", icon::Chip, 11, "" });
	for (const options::Category& category : options::categories())
	{
		std::string name = category.name;
		const size_t tail = name.rfind(" Settings");
		if (tail != std::string::npos && tail + 9 == name.size())
			name.erase(tail);
		const char *symbol = category.key == "console" ? icon::Chip : category.key == "enhancement" ? icon::Magic
				: category.key == "display" ? icon::Picture : category.key == "port" ? icon::Card : icon::Wrench;
		list.push_back({ name, category.key, symbol, 10, category.info });
	}
	if (!forGame)
		list.push_back({ "About", "", icon::Info, 20, "" });
	return list;
}

Item toggle(const std::string& label, bool *value, const std::string& info, std::function<void()> changed = {})
{
	Item item;
	item.label = label;
	item.info = info;
	item.choices = { "Off", "On" };
	item.current = *value ? 1 : 0;
	item.value = item.choices[item.current];
	item.choose = [value, changed](int index) {
		*value = index != 0;
		options::saveFrontend();
		if (changed)
			changed();
	};
	return item;
}

Item choice(const std::string& label, int current, std::vector<std::string> names, const std::string& info,
		std::function<void(int)> set)
{
	Item item;
	item.label = label;
	item.info = info;
	item.choices = std::move(names);
	item.current = std::clamp(current, 0, (int)item.choices.size() - 1);
	item.value = item.choices[item.current];
	item.choose = [set](int index) {
		set(index);
		options::saveFrontend();
	};
	return item;
}

void frontendItems(int kind, std::vector<Item>& items)
{
	options::Frontend& f = options::frontend();
	switch (kind)
	{
	case 0:
		items.push_back(choice("Library view", f.view, { "Covers", "List" },
				"How the library shows your games: a wall of covers, or a list of names with the cover beside it.",
				[](int i) { options::frontend().view = i; }));
		items.push_back(toggle("Download covers", &f.covers,
				"Box art for games that have none in the covers folder is fetched from the libretro thumbnails "
				"collection, by the game's file name. Your own pictures (covers/<file name>.png or .jpg) are never "
				"replaced."));
		items.push_back(choice("Accent colour", f.accent,
				[] {
					std::vector<std::string> names;
					for (int i = 0; i < AccentCount; i++)
						names.push_back(accentName(i));
					return names;
				}(),
				"The colour of the cursor and the highlights.", [](int i) { options::frontend().accent = i; }));
		items.push_back(choice("Interface size", (f.uiScale - 80) / 10,
				{ "80%", "90%", "100%", "110%", "120%", "130%" },
				"Makes the text and everything else of the interface larger or smaller.",
				[](int i) { options::frontend().uiScale = 80 + i * 10; }));
		items.push_back(choice("Confirm button", f.swapConfirm ? 1 : 0, { "Cross", "Circle" },
				"Which button confirms in the menus; the other one goes back. Games are not affected.",
				[](int i) { options::frontend().swapConfirm = i != 0; }));
		items.push_back(toggle("Show frame rate", &f.showFps,
				"Shows, in a corner of the game, how many frames a second the emulator runs and how that compares "
				"with the game's own speed."));
		items.push_back(toggle("Console notices", &f.notifications,
				"Lets SwanStation use the console's pop-up notices for things that matter outside its own screen "
				"(a start-up problem, for one).",
				[] { diag::setNotifications(options::frontend().notifications); }));
		break;
	case 1:
		items.push_back(choice("Scaling", f.scaling, { "Fit the screen", "Whole multiples", "Stretch" },
				"Fit keeps the picture's shape and makes it as large as the screen allows. Whole multiples only "
				"enlarges by 2x, 3x and so on, which keeps the software renderer's pixels even. Stretch fills the "
				"screen and distorts.",
				[](int i) { options::frontend().scaling = i; }));
		items.push_back(toggle("Smooth scaling", &f.linearFilter,
				"Blends neighbouring pixels when the picture is enlarged to the screen. Off shows them as sharp squares.",
				[] { display::setLinear(options::frontend().linearFilter); }));
		items.push_back(toggle("Follow the display", &f.syncToDisplay,
				"Runs one frame of the game for each refresh of the display when their rates are within one percent, "
				"and stretches the sound to match: no stutter and no tearing. Off keeps the game's exact speed and "
				"drops or repeats a frame now and then."));
		break;
	case 2:
		items.push_back(choice("Volume", f.volume / 5,
				[] {
					std::vector<std::string> names;
					for (int v = 0; v <= 100; v += 5)
						names.push_back(format("%d%%", v));
					return names;
				}(),
				"The loudness of the game's sound.", [](int i) {
					options::frontend().volume = i * 5;
					audio::setVolume(i * 5);
				}));
		break;
	case 3:
		for (int port = 0; port < 2; port++)
			items.push_back(choice(format("Controller in port %d", port + 1), f.controller[port],
					{ "Digital controller", "DualShock", "Analog joystick", "None" },
					"What the game finds plugged into this port. DualShock suits most games; a few early ones only "
					"know the digital controller. Port 2 is the second DualSense.",
					[port](int i) {
						options::frontend().controller[port] = i;
						host::applyControllers();
					}));
		items.push_back(choice("Stick dead zone", (int)std::lround(f.deadZone * 20.f),
				{ "0%", "5%", "10%", "15%", "20%", "25%", "30%", "35%", "40%" },
				"How far a stick must move before the game sees it.",
				[](int i) { options::frontend().deadZone = (float)i * 0.05f; }));
		items.push_back(toggle("Vibration", &f.rumble, "Passes the game's vibration to the DualSense."));
		items.push_back(fact("Select and Start", "Touch pad",
				"The left half of the touch pad is the PlayStation's Select, the right half is Start: press the pad "
				"down on that side. OPTIONS opens SwanStation's menu."));
		break;
	case 4:
		items.push_back(toggle("Save when a game is closed", &f.autoSaveOnExit,
				"Keeps a state of the game as it is when you close it, apart from the ten state slots."));
		items.push_back(toggle("Continue where I left off", &f.autoLoadOnStart,
				"Starts a game from the state kept when it was last closed, when there is one. The game's details "
				"page (Triangle) can still start it from the beginning."));
		items.push_back(toggle("Load network games into memory", &f.ramCache,
				"Reads a game from the network share completely before it starts, so the game never waits for the "
				"network while it runs. Off starts sooner and reads as the game asks."));
		items.push_back(toggle("USB drives", &f.usb,
				"Lets SwanStation read games from USB drives (a folder named psx, ps1 or playstation at the top "
				"of the drive). This needs the ELF loader listening on port 9021 and takes effect the next time "
				"SwanStation starts."));
		{
			std::string folders;
			for (const std::string& folder : smb::gameFolders())
				folders += (folders.empty() ? "" : ", ") + folder;
			items.push_back(fact("Network share", folders.empty() ? "Not set" : folders,
					(folders.empty() ? std::string("No share is named yet. ") : folders + ". ")
					+ "The share is named in " + shownRoot() + "network.cfg (edit it over FTP); the file explains "
					"itself. SwanStation reads it when it starts."));
			items.push_back(fact("Folders", shownRoot(),
					"Games go in " + shownRoot() + "games, BIOS files in bios, covers in covers, your own cheat "
					"files in cheats. Memory cards, states and settings are kept in data."));
		}
		break;
	}
}

void optionItems(const std::string& category, bool uncategorised, bool forGame, std::vector<Item>& items)
{
	for (const options::Option& option : options::all())
	{
		if (!option.visible || option.values.empty())
			continue;
		if (uncategorised ? !option.category.empty() : option.category != category)
			continue;
		Item item;
		item.label = option.name;
		const char *set = options::get(option.key);
		const std::string value = set != nullptr ? set : option.defaultValue;
		for (size_t i = 0; i < option.values.size(); i++)
		{
			item.choices.push_back(option.values[i].label);
			if (option.values[i].value == value)
				item.current = (int)i;
		}
		item.value = options::label(option, value);
		item.info = option.info;
		if (!item.info.empty())
			item.info += "\n\n";
		item.info += "Default: " + options::label(option, option.defaultValue);
		if (option.key == "swanstation_GPU_Renderer" && host::running())
			item.info += "\n\nA running game keeps its renderer; the change is used from the next start.";
		const std::string key = option.key;
		const bool own = options::hasGameValue(key);
		if (options::hasOverride(key))
		{
			item.mark = 2;
			item.info += "\n\nSet by a patch that is on for this game; the value chosen here returns when the patch "
					"is switched off.";
		}
		else if (forGame && own)
			item.mark = 1;
		else if (!forGame && own)
			item.info += "\n\nThe running game has a value of its own for this (Game settings).";
		std::vector<std::string> values;
		for (const options::Value& v : option.values)
			values.push_back(v.value);
		item.choose = [key, values, forGame](int index) {
			if (index >= 0 && index < (int)values.size())
				options::set(key, values[index], forGame);
		};
		if (forGame)
		{
			if (own)
			{
				item.alt = [key] { options::clearGameValue(key); };
				item.altHint = "Use the general value";
			}
		}
		else if (value != option.defaultValue)
		{
			item.alt = [key, standard = option.defaultValue] { options::set(key, standard, false); };
			item.altHint = "Default";
		}
		items.push_back(std::move(item));
	}
}

void aboutItems(std::vector<Item>& items)
{
	retro_system_info info{};
	retro_get_system_info(&info);
	items.push_back(header("SwanStation for PS5"));
	items.push_back(fact("Build", format("%d", BuildNumber)));
	items.push_back(fact("Emulator", format("%s %s", info.library_name != nullptr ? info.library_name : "SwanStation",
			info.library_version != nullptr ? info.library_version : ""),
			"SwanStation, the libretro fork of DuckStation, linked into this title with a frontend of its own."));
	items.push_back(fact("Graphics", display::deviceName()));
	items.push_back(fact("Display", format("%d x %d at %.2f Hz", display::width(), display::height(),
			display::refreshRate())));
	items.push_back(fact("BIOS", host::biosSummary(),
			host::biosSummary() + ". SwanStation carries OpenBIOS and needs no BIOS file; games are more "
			"compatible with an original one (scph5500.bin, scph5501.bin, scph5502.bin) in " + shownRoot() + "bios."));
	items.push_back(fact("Cheats and patches", cheats::summary(),
			cheats::summary() + ". The database is the DuckStation project's chtdb; a game's entries are in its "
			"menu while it runs."));
	items.push_back(fact("Recompiler", jitAvailable() ? "Available" : "Not available",
			jitAvailable() ? "The console gives the emulator memory it can run generated code from, so the "
			"recompiler, the fast way to emulate the PlayStation's processor, is used."
			: "The console refused memory that generated code can run from. The cached interpreter is used "
			"instead; it is slower, and enough for most games."));
	const uint64_t memory = freeMemory();
	if (memory != 0)
		items.push_back(fact("Free memory", format("%u MB", (unsigned)(memory >> 20))));
	items.push_back(fact("Folder", shownRoot()));
	items.push_back(header("Licences"));
	items.push_back(fact("SwanStation", "GPL-3.0",
			"SwanStation is free software under the GNU General Public License, version 3. It is a fork of "
			"DuckStation by Connor McLaughlin (stenzek) and its contributors, kept by the libretro team. This "
			"title's source is the SwanStation source plus the ps5 folder."));
	items.push_back(fact("Mesa RADV", "MIT", "The Vulkan driver, from the PS5 Mesa port."));
	items.push_back(fact("Dear ImGui", "MIT", "The interface is drawn with Dear ImGui by Omar Cornut."));
	items.push_back(fact("libsmb2", "LGPL-2.1", "Network shares are read with libsmb2 by Ronnie Sahlberg."));
	items.push_back(fact("Fonts", "Roboto, Font Awesome",
			"Roboto (Apache License 2.0) and the solid symbols of Font Awesome Free (SIL OFL 1.1)."));
}

void settingsPage(Frame& f)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const bool forGame = f.b != 0;
	const std::vector<SettingsCategory> categories = settingsCategories(forGame);
	const int count = (int)categories.size();
	if (f.picker < 0)
	{
		const int before = f.a;
		if (hit(R1))
			f.a = (f.a + 1) % count;
		if (hit(L1))
			f.a = (f.a + count - 1) % count;
		if (f.a != before)
		{
			f.cursor = 0;
			f.scroll = f.scrollTarget = 0;
			f.fresh = true;
		}
	}
	f.a = std::clamp(f.a, 0, count - 1);
	const SettingsCategory& category = categories[f.a];

	text(at(64, 40), t.text, forGame ? "Game settings" : "Settings", Title, 44);
	if (forGame)
		textFit(at(64 + toUnits(measure("Game settings", Title, 44).x) + 28, 58), px(W - 700), t.dim,
				host::game().title + (host::game().serial.empty() ? "" : "  \xc2\xb7  " + host::game().serial), Body, 26);

	// The categories.
	const float top = 128, bottom = H - 88;
	const float cx0 = 64, cx1 = 404;
	panel(at(cx0, top), at(cx1, bottom), t.panel, 16);
	for (int i = 0; i < count; i++)
	{
		const float y = top + 14 + i * 58;
		const bool selected = i == f.a;
		if (selected)
			panel(at(cx0 + 10, y), at(cx1 - 10, y + 54), t.accentSoft, 12);
		text(at(cx0 + 30, y + 15), selected ? t.accent : t.faint, categories[i].icon, Body, 24);
		textFit(at(cx0 + 80, y + 13), px(cx1 - cx0 - 100), selected ? t.text : t.dim, categories[i].name,
				selected ? Bold : Body, 26);
	}

	std::vector<Item> items;
	if (category.kind < 10)
		frontendItems(category.kind, items);
	else if (category.kind == 20)
		aboutItems(items);
	else
		optionItems(category.key, category.kind == 11, forGame, items);

	const float lx0 = 428, lx1 = W - 560;
	panel(at(lx0, top), at(lx1, bottom), t.panel, 16);
	const Item *focused = runList(f, items, lx0 + 8, top + 10, lx1 - 16, bottom - 10);

	// What the option under the cursor does.
	const float ix0 = lx1 + 24, ix1 = W - 64;
	panel(at(ix0, top), at(ix1, bottom), withAlpha(t.panel, 0.7f), 16);
	if (focused != nullptr)
	{
		float y = top + 28;
		y += toUnits(textWrapped(at(ix0 + 28, y), px(ix1 - ix0 - 56), t.text, focused->label, Bold, 28)) + 16;
		if (focused->mark == 1)
		{
			text(at(ix0 + 28, y), t.accent, "This game's own value", Bold, 20);
			y += 36;
		}
		textWrapped(at(ix0 + 28, y), px(ix1 - ix0 - 56), t.dim, focused->info, Body, 23, px(bottom - y - 24));
	}
	else if (forGame)
		textWrapped(at(ix0 + 28, top + 28), px(ix1 - ix0 - 56), t.dim, category.info, Body, 23);

	std::vector<Hint> hints;
	hints.push_back({ L1 | R1, "Section" });
	if (focused != nullptr && !focused->choices.empty())
		hints.push_back({ Left | Right, "Change" });
	if (focused != nullptr && focused->alt)
		hints.push_back({ Square, focused->altHint });
	hints.push_back({ cancelButton, "Back" });
	hintBar(hints, forGame ? "What is set here is used for this game only and kept with it." : "");
}

// ------------------------------------------------------------- menu pages

// A page that is one list at the left with an explanation at the right.
const Item *menuPage(Frame& f, const std::string& title, const std::string& subtitle, std::vector<Item>& items,
		float listWidth = 780)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	text(at(64, 40), t.text, title, Title, 44);
	if (!subtitle.empty())
		textFit(at(64 + toUnits(measure(title, Title, 44).x) + 28, 58), px(W - 700), t.dim, subtitle, Body, 26);
	const float top = 128, bottom = H - 88;
	const float x0 = 64, x1 = x0 + listWidth;
	panel(at(x0, top), at(x1, bottom), t.panel, 16);
	const Item *focused = runList(f, items, x0 + 8, top + 10, x1 - 16, bottom - 10);
	if (focused != nullptr && !focused->info.empty())
	{
		const float ix0 = x1 + 24, ix1 = std::min(x1 + 24 + 700, W - 64);
		const float wrap = ix1 - ix0 - 56;
		const float h = fontsHeight(focused->info, wrap);
		panel(at(ix0, top), at(ix1, top + h + 56), withAlpha(t.panel, 0.7f), 16);
		textWrapped(at(ix0 + 28, top + 28), px(wrap), t.dim, focused->info, Body, 23);
	}
	return focused;
}

void standardHints(const Item *focused, const char *confirm = "Select")
{
	std::vector<Hint> hints;
	if (focused != nullptr && focused->enabled && (focused->activate || !focused->choices.empty()))
		hints.push_back({ confirmButton, focused->confirmHint.empty() ? confirm : focused->confirmHint });
	if (focused != nullptr && focused->enabled && (focused->adjust || focused->choices.size() > 1))
		hints.push_back({ Left | Right, "Change" });
	if (focused != nullptr && focused->alt)
		hints.push_back({ Square, focused->altHint });
	hints.push_back({ cancelButton, "Back" });
	hintBar(hints);
}

void resume()
{
	stack.clear();
}

void mainMenuPage(Frame& f)
{
	std::vector<Item> items;
	items.push_back(action(icon::Gear, "Settings", "The interface, the picture and the sound, controllers, where "
			"games come from, and every setting of the emulator.", [] { push(Page::Settings); }));
	items.push_back(action(icon::Chip, "Start the BIOS", "Starts the PlayStation without a disc: the memory card "
			"manager and the CD player of an original BIOS, when one is in the bios folder.", [] { begin("", -1); }));
	items.push_back(action(icon::Sync, "Scan for games", "Looks through the games folder, the USB drives and the "
			"network share again.", [] {
				for (int i = 0; i < library::SourceCount; i++)
					if (i != library::Usb || options::frontend().usb)
						library::scan(i, true);
				pop();
			}));
	items.push_back(action(icon::Info, "About", "Versions, folders and licences.", [] {
		const int about = (int)settingsCategories(false).size() - 1;
		push(Page::Settings, about);
	}));
	items.push_back(action(icon::Power, "Close SwanStation", "Back to the console's home screen.", [] { quit = true; }));
	standardHints(menuPage(f, "Menu", "", items, 620));
}

void pausePage(Frame& f)
{
	const host::GameInfo& game = host::game();
	std::vector<Item> items;
	items.push_back(action(icon::Play, "Resume", "", [] { resume(); }));
	items.push_back(action(icon::Save, "Save state", "Keeps the game exactly as it is now in one of ten slots.",
			[] { push(Page::States, 0); }, !game.path.empty()));
	items.push_back(action(icon::Upload, "Load state", "Returns the game to a state saved before.",
			[] { push(Page::States, 1); }, !game.path.empty()));
	if (host::discCount() > 1)
	{
		Item item = action(icon::Disc, "Change disc", "Takes the disc out and puts another of this game in.",
				[] { push(Page::Discs); });
		item.value = format("%d of %d", host::discIndex() + 1, host::discCount());
		items.push_back(item);
	}
	{
		int on = 0;
		for (const cheats::Cheat& cheat : cheats::list())
			on += cheat.enabled;
		Item item = action(icon::Magic, "Cheats and patches",
				"Cheat codes and patches (widescreen, 60 frames a second and others) for this game from the "
				"DuckStation database, and your own from the cheats folder.",
				[] { push(Page::Cheats); });
		item.value = cheats::list().empty() ? "None" : on != 0 ? format("%d on", on)
				: format("%d", (int)cheats::list().size());
		items.push_back(item);
	}
	items.push_back(action(icon::Sliders, "Game settings", "The emulator's settings for this game alone: what is "
			"set there is kept with the game and used whenever it runs.",
			[] { push(Page::Settings, 0, 1); }, !game.serial.empty()));
	items.push_back(action(icon::Gear, "Settings", "The settings for every game.", [] { push(Page::Settings); }));
	items.push_back(action(icon::Undo, "Reset", "Restarts the game, as the console's reset button does.", [] {
		push(Page::Confirm, 0, 0, "Reset the game?", "What was not saved to a memory card or a state is lost.", [] {
			host::reset();
			resume();
		});
	}));
	items.push_back(action(icon::Power, "Close the game", "Back to the library.", [] {
		if (options::frontend().autoSaveOnExit)
		{
			closeGame = true;
			return;
		}
		push(Page::Confirm, 0, 0, "Close the game?", "What was not saved to a memory card or a state is lost.",
				[] { closeGame = true; });
	}));
	const std::string subtitle = game.serial.empty() ? "" : game.serial;
	standardHints(menuPage(f, game.title, subtitle, items, 700));
}

void statesPage(Frame& f)
{
	const bool load = f.a == 1;
	std::vector<Item> items;
	if (load)
	{
		std::string when;
		if (host::stateExists(host::ResumeSlot, &when))
		{
			Item item = action(icon::Clock, "Where the game was closed", "The state kept when this game was last closed.",
					[] {
						if (host::loadState(host::ResumeSlot))
							resume();
					});
			item.value = when;
			items.push_back(item);
		}
	}
	for (int slot = 0; slot < host::StateSlots; slot++)
	{
		std::string when;
		const bool exists = host::stateExists(slot, &when);
		Item item;
		item.icon = load ? icon::Upload : icon::Save;
		item.label = format("Slot %d", slot + 1);
		item.value = exists ? when : "Empty";
		item.enabled = !load || exists;
		item.activate = [slot, load] {
			if (load ? host::loadState(slot) : host::saveState(slot))
				resume();
		};
		items.push_back(item);
	}
	standardHints(menuPage(f, load ? "Load state" : "Save state", host::game().title, items, 700),
			load ? "Load" : "Save");
}

void discsPage(Frame& f)
{
	std::vector<Item> items;
	const int current = host::discIndex();
	for (int i = 0; i < host::discCount(); i++)
	{
		Item item;
		item.icon = icon::Disc;
		item.label = host::discLabel(i);
		item.ticked = i == current;
		item.activate = [i] {
			if (host::setDisc(i))
				resume();
		};
		items.push_back(item);
	}
	if (f.fresh)
		f.cursor = current;
	standardHints(menuPage(f, "Change disc", host::game().title, items, 900), "Insert");
}

void cheatsPage(Frame& f)
{
	const Theme& t = theme();
	std::vector<cheats::Cheat>& list = cheats::list();
	std::vector<Item> items;
	bool anyPatch = false, anyCheat = false;
	for (const cheats::Cheat& cheat : list)
		(cheat.patch ? anyPatch : anyCheat) = true;
	for (int pass = 0; pass < 2; pass++)
	{
		const bool patches = pass == 0;
		if (patches ? !anyPatch : !anyCheat)
			continue;
		items.push_back(header(patches ? "PATCHES" : "CHEATS"));
		for (size_t i = 0; i < list.size(); i++)
		{
			const cheats::Cheat& cheat = list[i];
			if (cheat.patch != patches)
				continue;
			Item item;
			item.label = cheat.group.empty() ? cheat.name : cheat.group + ":  " + cheat.name;
			item.enabled = cheat.supported;
			std::string chosenValue;
			if (!cheat.choices.empty())
			{
				for (const auto& [name, value] : cheat.choices)
					if (value == cheat.value)
						chosenValue = name;
				if (chosenValue.empty())
					chosenValue = cheat.choices.front().first;
			}
			else if (cheat.hasRange)
				chosenValue = format("%u", cheat.value);
			if (!cheat.supported)
				item.value = "Not supported";
			else if (cheat.manual)
				item.value = chosenValue.empty() ? "Run once" : chosenValue;
			else if (!chosenValue.empty())
				item.value = cheat.enabled ? chosenValue : "Off  (" + chosenValue + ")";
			else
				item.value = cheat.enabled ? "On" : "Off";
			item.ticked = cheat.enabled && !cheat.manual;
			item.info = cheat.description;
			if (!cheat.author.empty())
				item.info += (item.info.empty() ? "" : "\n\n") + std::string("By ") + cheat.author;
			if (!cheat.settings.empty())
				item.info += (item.info.empty() ? "" : "\n\n")
						+ std::string("While it is on, it also sets the emulator's options it needs.");
			if (!cheat.supported)
				item.info += (item.info.empty() ? "" : "\n\n")
						+ std::string("It uses a code type this emulator does not have.");
			item.confirmHint = cheat.manual ? "Apply once" : cheat.enabled ? "Switch off" : "Switch on";
			if (cheat.manual)
				item.activate = [i] {
					cheats::runOnce(i);
					host::addMessage("Applied: " + cheats::list()[i].name);
				};
			else
				item.activate = [i] { cheats::setEnabled(i, !cheats::list()[i].enabled); };
			if (!cheat.choices.empty())
				item.adjust = [i](int direction) {
					const cheats::Cheat& c = cheats::list()[i];
					int index = 0;
					for (size_t n = 0; n < c.choices.size(); n++)
						if (c.choices[n].second == c.value)
							index = (int)n;
					index = std::clamp(index + direction, 0, (int)c.choices.size() - 1);
					cheats::setValue(i, c.choices[(size_t)index].second);
				};
			else if (cheat.hasRange)
				item.adjust = [i](int direction) {
					const cheats::Cheat& c = cheats::list()[i];
					const int64_t next = std::clamp<int64_t>((int64_t)c.value + direction, c.rangeLow, c.rangeHigh);
					cheats::setValue(i, (uint32_t)next);
				};
			items.push_back(std::move(item));
		}
	}
	const std::string serial = host::game().serial;
	if (items.empty())
	{
		text(at(64, 40), t.text, "Cheats and patches", Title, 44);
		const float W = unitsWide(), H = unitsHigh();
		panel(at(64, 128), at(W - 64, H - 88), t.panel, 16);
		textCentred(at(W * 0.5f, H * 0.36f), withAlpha(t.accent, 0.8f), icon::Magic, Title, 72);
		textCentred(at(W * 0.5f, H * 0.36f + 110), t.text, "Nothing for this game", Bold, 32);
		const std::string hint = serial.empty()
				? std::string("The disc has no serial number the database could be asked with.")
				: "The database has no entry for " + serial + ". A file of your own, " + shownRoot() + "cheats/"
				+ serial + ".cht, is read when the game starts.";
		const float w = std::min(toUnits(measure(hint, Body, 24).x), 1100.f);
		textWrapped(at((W - w) * 0.5f, H * 0.36f + 170), px(1100), t.dim, hint, Body, 24);
		hintBar({ { cancelButton, "Back" } });
		return;
	}
	standardHints(menuPage(f, "Cheats and patches", host::game().title + "  \xc2\xb7  " + serial, items, 1100));
}

void detailsPage(Frame& f)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const library::Game game = chosen;
	std::vector<Item> items;
	const bool resumable = host::stateExistsFor(game.path, host::ResumeSlot);
	if (resumable)
		items.push_back(action(icon::Clock, "Continue", "Starts from the state kept when the game was last closed.",
				[game] { launch(game, host::ResumeSlot); }));
	items.push_back(action(icon::Play, resumable ? "Start from the beginning" : "Start", "",
			[game] { launch(game, FromBeginning); }));
	for (int slot = 0; slot < host::StateSlots; slot++)
		if (host::stateExistsFor(game.path, slot))
			items.push_back(action(icon::Upload, format("Start from state %d", slot + 1), "",
					[game, slot] { launch(game, slot); }));

	text(at(64, 40), t.text, "Game", Title, 44);
	const float top = 128, bottom = H - 88;
	// The cover and the facts at the left, what can be done at the right.
	const float side = 520;
	const std::string coverFile = [&] {
		for (const char *ext : { ".png", ".jpg", ".jpeg" })
			if (fileExists(rootDir + "covers/" + game.fileTitle + ext))
				return rootDir + "covers/" + game.fileTitle + ext;
		return std::string();
	}();
	static std::string coverFor, coverPath;
	if (coverFor != game.path || f.fresh)
	{
		coverFor = game.path;
		coverPath = coverFile;
	}
	const Image cover = image(coverPath);
	if (cover.id != nullptr)
		imageFit(cover, at(64, top), at(64 + side, top + side), 14);
	else
		coverPlaceholder(at(64, top), at(64 + side, top + side), game.name, game.region);

	const float x0 = 64 + side + 48, x1 = W - 64;
	float y = top;
	y += toUnits(textWrapped(at(x0, y), px(x1 - x0), t.text, game.name, Title, 40, px(100))) + 14;
	std::string facts = game.region;
	const std::string size = sizeText(game.size);
	if (!size.empty())
		facts += (facts.empty() ? "" : "  \xc2\xb7  ") + size;
	if (game.discs.size() > 1)
		facts += (facts.empty() ? "" : "  \xc2\xb7  ") + format("%d discs", (int)game.discs.size());
	facts += (facts.empty() ? "" : "  \xc2\xb7  ") + library::sourceName(game.source);
	text(at(x0, y), t.dim, facts, Body, 24);
	y += 44;
	textFit(at(x0, y), px(x1 - x0), t.faint, game.discs.size() > 1 ? game.discs.front() : game.path, Body, 20);
	y += 52;
	panel(at(x0, y), at(x1, bottom), t.panel, 16);
	const Item *focused = runList(f, items, x0 + 8, y + 10, x1 - 16, bottom - 10);
	standardHints(focused, "Start");
}

void loadingPage(Frame& f)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const smb::Status status = smb::status();
	const float w = 900, h = 250;
	const float x0 = (W - w) * 0.5f, y0 = (H - h) * 0.5f;
	panel(at(x0, y0), at(x0 + w, y0 + h), IM_COL32(24, 30, 48, 250), 20);
	textFit(at(x0 + 44, y0 + 36), px(w - 88), t.text, f.s2, Bold, 32);
	const std::string line = !status.text.empty() ? status.text
			: options::frontend().ramCache ? "Opening the game on the network share" : "Checking the game's files";
	text(at(x0 + 44, y0 + 100), t.dim, line, Body, 24);
	progressBar(at(x0 + 44, y0 + 160), at(x0 + w - 44, y0 + 176), status.progress);
	hintBar({ { cancelButton, "Cancel" } });

	const int state = smb::precacheState();
	if (state == smb::PrecacheRunning)
	{
		if (hit(cancelButton))
			smb::cancelLoad();
		consumeInput();
		return;
	}
	consumeInput();
	const std::string path = f.s;
	const int slot = f.a;
	const bool ok = state == smb::PrecacheDone;
	deferred = [path, slot, ok] {
		const std::string error = smb::lastError();
		smb::finishPrecache();
		pop();
		if (ok)
			begin(path, slot);
		else if (!error.empty())
			message("The game could not be read", error);
	};
}

void messagePage(Frame& f, bool confirm)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const float w = 920;
	const float textH = fontsHeight(f.s2, w - 88, 24);
	const float h = 190 + textH;
	const float x0 = (W - w) * 0.5f, y0 = (H - h) * 0.5f;
	draw()->AddRectFilled(ImVec2(0, 0), ImVec2(width(), height()), IM_COL32(0, 0, 0, 130));
	panel(at(x0, y0), at(x0 + w, y0 + h), IM_COL32(24, 30, 48, 252), 20);
	outline(at(x0, y0), at(x0 + w, y0 + h), IM_COL32(255, 255, 255, 26), 20, 1.5f);
	text(at(x0 + 44, y0 + 36), confirm ? t.accent : t.bad, confirm ? icon::Info : icon::Warning, Body, 30);
	textFit(at(x0 + 96, y0 + 34), px(w - 140), t.text, f.s, Bold, 32);
	textWrapped(at(x0 + 44, y0 + 98), px(w - 88), t.dim, f.s2, Body, 24);
	// The buttons.
	const float by = y0 + h - 72;
	if (confirm)
	{
		buttonGlyph(at(x0 + w - 420, by + 20), 34, confirmButton);
		text(at(x0 + w - 392, by + 7), t.text, "Yes", Bold, 24);
		buttonGlyph(at(x0 + w - 220, by + 20), 34, cancelButton);
		text(at(x0 + w - 192, by + 7), t.text, "No", Bold, 24);
		if (hit(confirmButton))
		{
			deferred = [action = f.action] {
				pop();
				if (action)
					action();
			};
			consumeInput();
		}
	}
	else
	{
		buttonGlyph(at(x0 + w - 180, by + 20), 34, confirmButton);
		text(at(x0 + w - 152, by + 7), t.text, "OK", Bold, 24);
		if (hit(confirmButton))
		{
			deferred = [] { pop(); };
			consumeInput();
		}
	}
}

} // namespace

void init()
{
	widgetsInit();
	source = std::clamp(options::frontend().source, 0, (int)library::SourceCount - 1);
	if (source == library::Usb && !options::frontend().usb)
		source = library::Internal;
}

bool quitRequested()
{
	return quit;
}

bool blocksEmulation()
{
	return !stack.empty();
}

void frame()
{
	widgetsFrame();
	imagesFrame();
	readInput();
	if (closeGame)
	{
		closeGame = false;
		host::stop();
		stack.clear();
		consumeInput();
	}

	const bool game = host::running();
	if (!game && stack.empty())
		libraryPage(true);
	else if (game && stack.empty())
	{
		drawGame(0);
		drawGameOverlay();
		if (hit(Options))
		{
			push(Page::Pause);
			consumeInput();
		}
	}
	else
	{
		// What is behind the page: the game, dimmed, or the library's backdrop.
		if (game)
			drawGame(0.72f);
		else
			backdrop();
		Frame& f = stack.back();
		const Page page = f.page;
		switch (page)
		{
		case Page::MainMenu: mainMenuPage(f); break;
		case Page::Settings: settingsPage(f); break;
		case Page::Pause: pausePage(f); break;
		case Page::States: statesPage(f); break;
		case Page::Discs: discsPage(f); break;
		case Page::Cheats: cheatsPage(f); break;
		case Page::Details: detailsPage(f); break;
		case Page::Loading: loadingPage(f); break;
		case Page::Message: messagePage(f, false); break;
		case Page::Confirm: messagePage(f, true); break;
		}
		if (game)
			drawMessages();
		// Circle goes back; OPTIONS, over a game, returns to it at once.
		if (!deferred && page != Page::Loading)
		{
			if (hit(cancelButton))
				pop();
			else if (game && hit(Options))
				stack.clear();
		}
	}

	if (deferred)
	{
		const std::function<void()> run = std::move(deferred);
		deferred = nullptr;
		run();
	}
	const bool wantPause = host::running() && !stack.empty();
	if (host::running() && host::paused() != wantPause)
		host::setPaused(wantPause);
}

}
