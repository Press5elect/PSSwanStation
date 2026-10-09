/*
	PSSwanStation - the interface: the library, the menus, the settings.

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
#include "ui_internal.h"
#include "netplay.h"
#include "display.h"
#include "update.h"

#include <libretro.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <ctime>
#include <map>

namespace fe::ui
{

using namespace platform;

// ------------------------------------------------------------------- input

Input in;
uint32_t confirmButton = Cross, cancelButton = Circle;
double lastInputAt;

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
	if (held != 0)
		lastInputAt = clock();
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

std::vector<Frame> stack;
// What a page asked for; done after the frame is drawn, when no reference
// into the stack or a page's list is alive.
std::function<void()> deferred;
// Closing the game takes its picture away, and the frame being drawn still
// shows it: that waits for the start of the next frame.
bool closeGame;
bool quit;
library::Game chosen;

void push(Page page, int a, int b, const std::string& s, const std::string& s2, std::function<void()> action)
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

void inform(const std::string& title, const std::string& text)
{
	push(Page::Message, 1, 0, title, text);
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
float fontsHeight(const std::string& value, float wrapUnits, float size)
{
	return toUnits(wrappedHeight(value, px(wrapUnits), Body, size));
}

// ---------------------------------------------------------------- starting

// For launch(): from the beginning, whatever "continue where I left off" says.
constexpr int FromBeginning = -3;

void detailsRestore();
std::string quickSerial(const library::Game& game, int disc);

void begin(const library::Game& game, int slot, int disc)
{
	// The serial of the disc that starts, when it is known without waiting for
	// a share: the game's own settings are then in place before it boots.
	std::string serial = quickSerial(game, disc);
	const std::string& first = game.discs.empty() ? game.path
			: game.discs[(size_t)std::clamp(disc, 0, (int)game.discs.size() - 1)];
	if (!game.path.empty() && !smb::isNetworkPath(first))
	{
		const std::string read = host::readSerial(first);
		if (!read.empty())
			serial = read;
	}
	{
		host::LaunchedGame launched;
		launched.path = game.path;
		launched.name = game.name;
		launched.fileTitle = game.fileTitle;
		launched.region = game.region;
		launched.discs = game.discs;
		launched.source = game.source;
		launched.disc = disc;
		host::rememberGame(launched);
	}
	if (host::start(game.path, slot, disc, serial))
	{
		stack.clear();
		return;
	}
	std::string why = host::lastError();
	// A game on several discs is started through a playlist of them; if that
	// did not open, the disc itself is started (it then keeps a memory card of
	// its own).
	if (game.discs.size() > 1 && disc >= 0 && disc < (int)game.discs.size() && host::start(game.discs[(size_t)disc], slot))
	{
		stack.clear();
		host::addMessage("The discs could not be started together: this disc runs on its own.", 6.0);
		return;
	}
	if (why.empty())
		why = "The emulator could not start this game.";
	detailsRestore();
	message("The game did not start", why);
}

void startBios()
{
	library::Game bios;
	begin(bios, -1, 0);
}

struct LoadingState;
void loadingBegin();
float smooth(float x);
float glide(float x);

// Starts it: a game on a share is read first (the loading page).
void launchNow(const library::Game& game, int slot, int disc)
{
	const std::string& first = game.discs.empty() ? game.path
			: game.discs[(size_t)std::clamp(disc, 0, (int)game.discs.size() - 1)];
	if (smb::isNetworkPath(first) || smb::isNetworkPath(game.path))
	{
		const std::string path = first;
		chosen = game;
		loadingBegin();
		push(Page::Loading, slot, disc, game.path, game.name);
		smb::startPrecache(path);
	}
	else
		begin(game, slot, disc);
}

// `disc`: the one in the tray at the start; -1 for the one last played.
void launch(const library::Game& game, int slot, int disc)
{
	// The memory cards (or all the files) are being brought from the share:
	// a game started now would play on the old ones.
	if (netfiles::working())
	{
		inform("Your files are being brought up to date", "PSSwanStation is bringing your files up to date with the "
				"network share (" + netfiles::status() + "). Start the game again in a moment.");
		return;
	}
	if (slot == -1 && options::frontend().autoLoadOnStart && host::stateExistsFor(game.path, host::ResumeSlot))
		slot = host::ResumeSlot;
	if (slot == FromBeginning)
		slot = -1;
	const int discs = (int)game.discs.size();
	if (disc < 0)
		disc = discs > 1 ? history::get(game.path).disc : 0;
	disc = std::clamp(disc, 0, std::max(discs - 1, 0));
	// With the animations full, the swan sees the game off first. A game on
	// the network is read first, and the swan sees it off from there.
	const std::string& first = game.discs.empty() ? game.path : game.discs[(size_t)disc];
	const bool network = smb::isNetworkPath(first) || smb::isNetworkPath(game.path);
	if (motion() == MotionFull && !network)
	{
		chosen = game;
		push(Page::Launch, slot, disc);
		return;
	}
	launchNow(game, slot, disc);
}

// ------------------------------------------------------------------- lists

Item header(const std::string& label)
{
	Item item;
	item.label = label;
	item.header = true;
	return item;
}

Item action(const char *icon, const std::string& label, const std::string& info, std::function<void()> activate,
		bool enabled)
{
	Item item;
	item.icon = icon;
	item.label = label;
	item.info = info;
	item.activate = std::move(activate);
	item.enabled = enabled;
	return item;
}

Item fact(const std::string& label, const std::string& value, const std::string& info)
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
			else if (!item.choices.empty() && item.choose && !item.menu)
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
				if (item.choices.size() == 2 && !item.menu)
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
		if (item.picture.id != nullptr)
		{
			imageFit(item.picture, at(x - 6, y + 7), at(x + 40, y + h - 7), 6,
					item.enabled ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 110));
			x += 58;
		}
		else if (item.icon != nullptr)
		{
			text(at(x, y + 17), focused && item.enabled ? t.accent : t.faint, item.icon, Body, 24);
			x += 50;
		}
		// The value, at the right.
		float right = x1 - 28;
		const bool steps = item.enabled && focused && !item.menu && (item.adjust || item.choices.size() > 1);
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

ImVec2 gameA, gameB;
bool optionsSpent;

void drawGame(float dim)
{
	ImDrawList *list = ImGui::GetBackgroundDrawList();
	const float W = width(), H = height();
	list->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 255));
	void *texture = host::frameTexture();
	// With black frame insertion every second refresh shows nothing (not under
	// a menu, where the game stands still).
	if (texture != nullptr && !(dim <= 0 && host::blackFrame()))
	{
		int w = 0, h = 0;
		float aspect = 4.f / 3.f;
		host::frameSize(w, h, aspect);
		float u = 1, v = 1;
		host::frameUv(u, v);
		float dw = W, dh = H;
		const options::Frontend& f = options::frontend();
		// (3 was "fit, with FSR 1" before build 15.)
		const int scaling = f.scaling == 3 ? 0 : f.scaling;
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
		const ImVec2 p1(p0.x + dw, p0.y + dh);
		gameA = p0;
		gameB = p1;
		// What is beside the picture, then the picture, then the lines of a
		// picture tube over it.
		drawBorder(list, texture, u, v, p0, p1);
		// The look (display::picture): the signal, the colours, how it is
		// grown to the screen, a picture tube. A picture that arrives the size
		// it has here is drawn pixel for pixel.
		display::Look look;
		static const int scalers[6] = { 0, 0, 1, 2, 3, 4 };
		look.scaler = scalers[std::clamp(f.scaler, 0, 5)];
		look.sharpness = f.fsrSharpness;
		look.signal = f.signal;
		const int lines = host::nativeLines();
		const int scale = lines > 0 ? std::max(h / lines, 1) : 1;
		// The dither is as fine as the picture's pixels unless it is scaled.
		const char *scaledDither = options::get("swanstation_GPU_ScaledDithering");
		look.cell = f.signal == 1 && !(scaledDither != nullptr && !strcmp(scaledDither, "true")) ? 1 : scale;
		look.crt = f.crt >= 4 ? f.crt - 3 : 0;
		look.brightness = 0.5f + 0.05f * (float)f.brightness;
		look.contrast = 0.5f + 0.05f * (float)f.contrast;
		look.saturation = 0.5f + 0.05f * (float)f.saturation;
		look.gamma = 0.5f + 0.05f * (float)f.gamma;
		const int outW = (int)std::lround(dw), outH = (int)std::lround(dh);
		float phase = 1.f;
		bool fresh = false;
		const bool generating = host::generationPhase(phase, fresh);
		void *shown = texture;
		float shownU = u, shownV = v;
		bool full = false;
		// With frame generation the picture is one kept by the display, or one
		// made between the last two (display::generated), at the game's size;
		// the look comes after it.
		if (generating)
		{
			void *made = display::generated(texture, w, h, u, v, fresh, phase, f.fgQuality, f.fgDebug);
			if (made != nullptr)
			{
				shown = made;
				shownU = shownV = 1.f;
			}
		}
		void *looked = display::picture(shown, w, h, shownU, shownV, outW, outH, look, full);
		const bool squares = full || f.scaler == 1;
		if (squares)
			display::sampling(list, true);
		if (looked != nullptr && full)
			list->AddImage((ImTextureID)looked, p0, ImVec2(p0.x + (float)outW, p0.y + (float)outH));
		else if (looked != nullptr)
			list->AddImage((ImTextureID)looked, p0, p1);
		else
			list->AddImage((ImTextureID)shown, p0, p1, ImVec2(0, 0), ImVec2(shownU, shownV));
		if (squares)
			display::sampling(list, false);
		drawScanlines(list, p0, p1);
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
		if (!m.title.empty())
		{
			// A notice with a heading (an achievement).
			y -= drawNotice(m, y) + 10;
			continue;
		}
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
		std::string line = format("%.1f fps  %d%%", got, want > 1 ? (int)std::lround(got / want * 100.0) : 0);
		// With frame generation: the pictures the game draws, and what the
		// screen shows with the made ones.
		float phase = 1.f;
		bool fresh = false;
		if (host::generationPhase(phase, fresh) || options::frontend().frameGeneration != 0)
			line += format("   %.0f \xe2\x86\x92 %.0f", host::picturesPerSecond(), host::shownPerSecond());
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

// What the database says about a game of the library, looked up when the game
// is first under the cursor.
struct Meta
{
	bool tried = false;
	std::string serial;
	bool known = false;
	gamedb::Info info;
};

struct LibraryView
{
	// Every game of the source, by name; and the ones on the screen, as the
	// library is sorted and filtered.
	std::vector<library::Game> all;
	std::vector<library::Game> games;
	unsigned generation = ~0u;
	unsigned orderSeen = ~0u;
	std::vector<std::string> cover;
	std::vector<unsigned> coverSeen;
	std::vector<Meta> meta;
	// The games played lately that this source has, the latest first.
	std::vector<int> recent;
	unsigned historySeen = ~0u;
	bool inShelf = false;
	int shelfCursor = 0;
	int cursor = 0;
	float scroll = 0, scrollTarget = 0;
	bool fresh = true;
};
LibraryView views[library::SourceCount];
// The source on the screen, and the one that was asked for (with L1 and R1,
// kept between starts). A source with no games has no tab: the one asked for
// is shown when it has games, and until then the first that has.
int source, wantedSource;
// Until a button is pressed the library goes to the source asked for when its
// games arrive (a share answers later than the folder does); after that it
// stays where the user is.
bool followWanted = true;
float tabGlow[library::SourceCount];

bool sourceShown(int index)
{
	return !views[index].all.empty();
}

// Counts the changes to how the library is sorted and what it shows.
unsigned orderChanges = 1;

void libraryChanged()
{
	orderChanges++;
}

// A game's release year, for sorting by it: 9999 when the database has none.
int yearOf(const library::Game& game)
{
	static std::map<std::string, int> known;
	const auto it = known.find(game.path);
	if (it != known.end())
		return it->second;
	gamedb::Info info;
	const std::string serial = quickSerial(game, 0);
	const int year = !serial.empty() && gamedb::find(serial, info) && info.year > 0 ? info.year : 9999;
	known[game.path] = year;
	return year;
}

// The games of a source that the filters let through, in the order asked for.
void orderView(LibraryView& view)
{
	const options::Frontend& settings = options::frontend();
	static const char *const regions[4] = { "", "USA", "Europe", "Japan" };
	view.games.clear();
	for (const library::Game& game : view.all)
	{
		const bool hiddenGame = library::hidden(game.path);
		if (settings.filter == 3 ? !hiddenGame : hiddenGame)
			continue;
		if (settings.filter == 1 && !library::favourite(game.path))
			continue;
		if (settings.filter == 2 && history::get(game.path).lastPlayed != 0)
			continue;
		if (settings.regionFilter != 0 && game.region != regions[std::clamp(settings.regionFilter, 0, 3)])
			continue;
		view.games.push_back(game);
	}
	// By name already; the other orders keep that among equals.
	switch (settings.sort)
	{
	case 1:
		std::stable_sort(view.games.begin(), view.games.end(), [](const library::Game& a, const library::Game& b) {
			return history::get(a.path).lastPlayed > history::get(b.path).lastPlayed;
		});
		break;
	case 2:
		std::stable_sort(view.games.begin(), view.games.end(), [](const library::Game& a, const library::Game& b) {
			return history::get(a.path).seconds > history::get(b.path).seconds;
		});
		break;
	case 3:
		std::stable_sort(view.games.begin(), view.games.end(),
				[](const library::Game& a, const library::Game& b) { return yearOf(a) < yearOf(b); });
		break;
	case 4:
		std::stable_sort(view.games.begin(), view.games.end(),
				[](const library::Game& a, const library::Game& b) { return a.size > b.size; });
		break;
	}
}

int shownSources()
{
	int count = 0;
	for (int i = 0; i < library::SourceCount; i++)
		count += sourceShown(i) ? 1 : 0;
	return count;
}

// Every place games can be in is looked through again.
void scanEverything()
{
	for (int i = 0; i < library::SourceCount; i++)
		if (i != library::Usb || options::frontend().usb)
			library::scan(i, true);
}
// The header's mark: its box, the swan sitting in it, and the name. While the
// swan is in the air (it flies in from the splash, and out when a game
// starts) the box is empty.
float headerBoxAlpha = 1, headerNameAlpha = 1;
bool headerSwanAway;

// The header's box, in pixels.
void headerBox(ImVec2& a, ImVec2& b)
{
	a = at(56, 30);
	b = at(120, 94);
}
// The library is only a background (a game's details are over it).
bool libraryBehind;

void refreshView(int index)
{
	LibraryView& view = views[index];
	const unsigned generation = library::generation(index);
	// What the order depends on: the list itself, the marks, the settings,
	// and (for the orders by play) what was played.
	const unsigned order = orderChanges * 7919u + library::marksGeneration() * 104729u
			+ (options::frontend().sort == 1 || options::frontend().sort == 2 || options::frontend().filter == 2
					? history::generation() : 0u);
	if (generation != view.generation || order != view.orderSeen)
	{
		// The cursor stays on its game when the list is replaced.
		const std::string keep = view.cursor < (int)view.games.size() ? view.games[view.cursor].path : "";
		if (generation != view.generation)
			view.all = library::games(index);
		view.generation = generation;
		view.orderSeen = order;
		orderView(view);
		view.cover.assign(view.games.size(), "");
		view.coverSeen.assign(view.games.size(), ~0u);
		view.meta.assign(view.games.size(), Meta());
		view.cursor = std::clamp(view.cursor, 0, std::max((int)view.games.size() - 1, 0));
		for (size_t i = 0; i < view.games.size(); i++)
			if (view.games[i].path == keep)
				view.cursor = (int)i;
		view.historySeen = ~0u;
	}
	if (view.historySeen != history::generation())
	{
		const bool first = view.historySeen == ~0u && view.recent.empty();
		view.historySeen = history::generation();
		view.recent.clear();
		for (const std::string& path : history::recent(64))
			for (size_t i = 0; i < view.games.size(); i++)
				if (view.games[i].path == path)
				{
					view.recent.push_back((int)i);
					break;
				}
		view.shelfCursor = 0;
		// The library opens on what was played last.
		if (first && !view.recent.empty())
			view.inShelf = true;
		if (view.recent.empty())
			view.inShelf = false;
	}
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

// A game's serial without touching its disc: the one it ran under, else the
// one Redump lists for a disc image of that name.
std::string quickSerial(const library::Game& game, int disc)
{
	if (game.discs.size() < 2)
	{
		const std::string known = host::knownSerial(game.path);
		if (!known.empty())
			return known;
	}
	const std::string& file = game.discs.empty() ? game.path
			: game.discs[(size_t)std::clamp(disc, 0, (int)game.discs.size() - 1)];
	return gamedb::serialByName(fileTitle(file));
}

const Meta& metaOf(LibraryView& view, int index)
{
	Meta& meta = view.meta[index];
	if (!meta.tried)
	{
		meta.tried = true;
		meta.serial = quickSerial(view.games[index], 0);
		meta.known = !meta.serial.empty() && gamedb::find(meta.serial, meta.info);
	}
	return meta;
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

std::string playedText(uint64_t seconds)
{
	if (seconds < 60)
		return seconds == 0 ? "" : "under a minute";
	if (seconds < 3600)
		return format("%d min", (int)(seconds / 60));
	return format("%d h %02d min", (int)(seconds / 3600), (int)(seconds % 3600 / 60));
}

std::string agoText(int64_t when)
{
	if (when == 0)
		return "";
	const int64_t passed = (int64_t)time(nullptr) - when;
	if (passed < 0 || when < 1500000000)
		return "";		// the console's clock was not set
	if (passed < 90)
		return "just now";
	if (passed < 3600)
		return format("%d min ago", (int)(passed / 60));
	if (passed < 86400 * 2)
		return format("%d h ago", (int)(passed / 3600));
	return format("%d days ago", (int)(passed / 86400));
}

// "1998  ·  Action  ·  KCE Japan"
std::string factsLine(const gamedb::Info& info, bool makers)
{
	std::string line;
	const auto add = [&line](const std::string& part) {
		if (!part.empty())
			line += (line.empty() ? "" : "  \xc2\xb7  ") + part;
	};
	if (info.year > 0)
		add(format("%d", info.year));
	add(info.genre);
	if (makers)
		add(info.developer);
	return line;
}

std::string usbHint()
{
	if (!options::frontend().usb)
		return "USB drives are off. Turn them on in Settings, Games and network, then restart PSSwanStation.";
	return library::sourceHint(library::Usb);
}

// Which build this is, in the top right corner.
void buildTag(float alpha)
{
	const Theme& t = theme();
	const float W = unitsWide();
	textRight(at(W - 56, 38), withAlpha(t.accent, alpha), format("WIP  \xc2\xb7  BUILD %d", BuildNumber), Bold, 20);
	textRight(at(W - 56, 66), withAlpha(t.faint, alpha), format("%s  \xc2\xb7  %s", BuildDate, Developer), Body, 18);
}

// The title's name as its mark has it: "PS" in the accent colour, the
// emulator's name after it in the text's.
void wordmark(ImVec2 where, float alpha, Font font, float size)
{
	const Theme& t = theme();
	const std::string name = AppName;
	text(where, withAlpha(t.accent, alpha), name.substr(0, 2), font, size);
	text(ImVec2(where.x + measure(name.substr(0, 2), font, size).x, where.y), withAlpha(t.text, alpha), name.substr(2),
			font, size);
}

void drawHeader()
{
	const Theme& t = theme();
	if (headerBoxAlpha > 0.01f)
	{
		ImVec2 a, b, origin;
		float size = 0;
		headerBox(a, b);
		logoBox(a, b, headerBoxAlpha);
		if (!headerSwanAway)
		{
			// It sits there and looks about.
			swanPlace(a, b, origin, size);
			swan(origin, size, motion() == MotionFull ? swanIdle(clock()) : SwanPose(), headerBoxAlpha);
		}
	}
	if (headerNameAlpha > 0.01f)
		wordmark(at(136, 36), headerNameAlpha, Title, 44);
	// The tabs begin where the name ends.
	const float nameEnd = 136 + toUnits(measure(AppName, Title, 44).x);

	// A tab for each place that has games; L1 and R1 when there is a choice.
	static const char *icons[library::SourceCount] = { icon::Drive, icon::Plug, icon::Network };
	const bool choice = shownSources() > 1;
	float x = choice ? nameEnd + 90 : nameEnd + 44;
	for (int i = 0; i < library::SourceCount; i++)
	{
		const bool selected = i == source;
		if (!sourceShown(i))
		{
			tabGlow[i] = 0;
			continue;
		}
		tabGlow[i] = approach(tabGlow[i], selected ? 1.f : 0.f, 14.f);
		const std::string label = library::sourceName(i) + format("  %d", (int)views[i].games.size());
		const float labelW = toUnits(measure(label, Bold, 26).x);
		const float tabW = labelW + 92;
		if (tabGlow[i] > 0.01f)
			panel(at(x, 38), at(x + tabW, 92), withAlpha(t.accentSoft, tabGlow[i]), 27);
		text(at(x + 24, 52), selected ? t.accent : t.faint, icons[i], Body, 24);
		text(at(x + 68, 50), selected ? t.text : t.dim, label, Bold, 26);
		x += tabW + 12;
	}
	if (choice)
	{
		buttonGlyph(at(nameEnd + 50, 65), 30, L1);
		buttonGlyph(at(x + 32, 65), 30, R1);
	}
	buildTag();
	// The time, and a newer build when the releases page has one.
	const float W = unitsWide();
	float right = W - 56 - 250;
	if (options::frontend().clock)
	{
		const std::string time = host::clockText();
		if (!time.empty())
		{
			textRight(at(right, 44), t.dim, time, Bold, 30);
			right -= toUnits(measure(time, Bold, 30).x) + 28;
		}
	}
	const update::Status newer = update::status();
	if (newer.state == update::State::Available || newer.state == update::State::Ready)
	{
		const std::string label = std::string(icon::Download) + format("   Build %d is out", newer.build);
		const float w = toUnits(measure(label, Bold, 20).x) + 36;
		panel(at(right - w, 44), at(right, 84), t.accentSoft, 20);
		text(at(right - w + 18, 53), t.accent, label, Bold, 20);
	}
}

// What is going on besides: a scan, cover downloads.
std::string libraryStatus()
{
	// The share is asked when the title starts, perhaps before this is.
	static bool shareWasAsked = !smb::gameFolders().empty();
	static double shareFailedAt = -1;
	shareWasAsked = shareWasAsked || library::scanning(library::Network);
	// The source on the screen first, then the others (theirs have no tab yet,
	// or are looked through again).
	for (int n = 0; n < library::SourceCount; n++)
	{
		const int i = (source + n) % library::SourceCount;
		if (!library::scanning(i))
			continue;
		const std::string status = library::scanStatus(i);
		if (!status.empty())
			return status;
		return i == source ? "Scanning\xe2\x80\xa6" : "Scanning " + library::sourceName(i) + "\xe2\x80\xa6";
	}
	// A share that did not answer has no tab to say so on: it is said here,
	// for a while.
	if (shareWasAsked && !library::scanning(library::Network))
	{
		if (!library::scanned(library::Network) && !smb::lastError().empty())
			shareFailedAt = clock();
		shareWasAsked = false;
	}
	if (shareFailedAt >= 0 && clock() - shareFailedAt < 8.0)
		return "Network share: " + smb::lastError();
	return covers::status();
}

void emptyLibrary(const std::string& hint, bool busy)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const float cx = W * 0.5f, cy = H * 0.46f;
	textCentred(at(cx, cy - 120), withAlpha(t.accent, 0.8f), busy ? icon::Sync : icon::Disc, Title, 84);
	textCentred(at(cx, cy), t.text, busy ? "Looking for games" : "No games yet", Bold, 34);
	const float wrap = 1000;
	// Centred as a block: wrapped text is left-aligned inside it.
	const ImVec2 extent = measure(hint, Body, 24);
	const float blockW = std::min(toUnits(extent.x), wrap);
	textWrapped(at(cx - blockW * 0.5f, cy + 64), px(wrap), t.dim, hint, Body, 24);
}

// The focused game's cover as the screen's light, one fading into the next.
void libraryWash(const std::string& coverPath)
{
	static std::string current, previous;
	static float fade = 1;
	if (coverPath != current)
	{
		previous = current;
		current = coverPath;
		fade = 0;
	}
	fade = approach(fade, 1.f, 5.f);
	const float strength = 0.34f;
	if (fade < 0.99f && !previous.empty())
		wash(image(previous), strength * (1.f - fade));
	if (!current.empty())
		wash(image(current), strength * fade);
	// Darker towards the bottom, where the names are read.
	ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(width(), height()),
			IM_COL32(8, 10, 18, 40), IM_COL32(8, 10, 18, 40), IM_COL32(8, 10, 18, 170), IM_COL32(8, 10, 18, 170));
}

// One game of the grid: its cover (or a card with its name), the name under
// it and, on the shelf, when it was played.
void drawCell(LibraryView& view, int index, float x, float y, float cell, bool focused, const std::string& under)
{
	const Theme& t = theme();
	const library::Game& game = view.games[index];
	float size = cell;
	if (focused)
	{
		// The cover under the cursor is larger, and breathes when things may move.
		const float grow = 14 + (motion() == MotionFull ? 2 * (float)std::sin(clock() * 3.0) : 0.f);
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
	if (library::favourite(game.path))
	{
		// A favourite: a heart in the corner.
		panel(at(x + 8, y + 8), at(x + 44, y + 44), IM_COL32(0, 0, 0, 170), 18);
		text(at(x + 16, y + 16), IM_COL32(255, 96, 128, 255), icon::Heart, Body, 20);
	}
	const float ty = y + size + (focused ? 6 : 10);
	const float maxW = px(size);
	const float tw = measure(game.name, focused ? Bold : Body, 22).x;
	textFit(ImVec2(tw < maxW ? a.x + (maxW - tw) * 0.5f : a.x, px(ty)), maxW, focused ? t.text : t.dim, game.name,
			focused ? Bold : Body, 22);
	if (!under.empty())
	{
		const float uw = measure(under, Body, 18).x;
		textFit(ImVec2(uw < maxW ? a.x + (maxW - uw) * 0.5f : a.x, px(ty + 30)), maxW, t.faint, under, Body, 18);
	}
}

void openDetails(const library::Game& game);
void openSearch();
struct Frame;
void searchPage(Frame& f);

// The letter a game is filed under: A to Z, and '#' for a name that begins
// with anything else.
char letterOfName(const std::string& name);

// What L2 and R2 jump by. Sorted by name it is the first letter; in any other
// order there are no letters to jump by, and it is a tenth of the list.
bool sortedByName()
{
	return options::frontend().sort == 0;
}

char letterOf(const std::string& name)
{
	return letterOfName(name);
}

char letterOfName(const std::string& name)
{
	for (const unsigned char c : name)
	{
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
			return (char)(c & ~0x20);
		if (c > ' ')
			return '#';
	}
	return '#';
}

// From the game at `from`: the first game of the next letter (forwards), or
// the first of this letter and, from there, the first of the letter before.
int letterJump(const std::vector<library::Game>& games, int from, bool forwards)
{
	const int count = (int)games.size();
	if (count == 0)
		return 0;
	from = std::clamp(from, 0, count - 1);
	if (!sortedByName())
		return std::clamp(from + (forwards ? 1 : -1) * std::max(count / 10, 1), 0, count - 1);
	const char here = letterOf(games[(size_t)from].name);
	if (forwards)
	{
		for (int i = from + 1; i < count; i++)
			if (letterOf(games[(size_t)i].name) != here)
				return i;
		return from;
	}
	int first = from;
	while (first > 0 && letterOf(games[(size_t)first - 1].name) == here)
		first--;
	if (first < from || first == 0)
		return first;
	const char before = letterOf(games[(size_t)first - 1].name);
	first--;
	while (first > 0 && letterOf(games[(size_t)first - 1].name) == before)
		first--;
	return first;
}

// After a jump the letters stand down the right edge for a moment, the one
// arrived at large beside them.
double letterShownAt = -10;

void letterRail(const std::vector<library::Game>& games, int focus)
{
	const float age = (float)(clock() - letterShownAt);
	if (age < 0 || age > 1.5f || games.empty() || !sortedByName())
		return;
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const float alpha = motion() == MotionOff ? 1.f : 1.f - std::clamp((age - 1.1f) / 0.4f, 0.f, 1.f);
	bool has[27] = {};
	for (const library::Game& game : games)
	{
		const char letter = letterOf(game.name);
		has[letter == '#' ? 0 : letter - 'A' + 1] = true;
	}
	const char now = letterOf(games[(size_t)std::clamp(focus, 0, (int)games.size() - 1)].name);
	const float top = 150, step = (H - 64 - top - 30) / 27.f, x = W - 34;
	panel(at(x - 20, top - 14), at(x + 20, top + step * 27 + 4), withAlpha(IM_COL32(10, 12, 20, 200), alpha), 20);
	for (int i = 0; i < 27; i++)
	{
		const char letter = i == 0 ? '#' : (char)('A' + i - 1);
		const bool current = letter == now;
		const float y = top + step * (float)i;
		if (current)
			panel(at(x - 16, y - 2), at(x + 16, y + step - 4), withAlpha(t.accent, alpha), 10);
		textCentred(at(x, y + (step - 26) * 0.5f), withAlpha(current ? IM_COL32(8, 12, 22, 255) : has[i] ? t.text : t.faint,
				alpha * (current || has[i] ? 1.f : 0.45f)), std::string(1, letter), Bold, 19);
	}
	// The letter itself, large.
	const float size = 150;
	panel(at(W - 250, H * 0.5f - size * 0.5f), at(W - 250 + size, H * 0.5f + size * 0.5f),
			withAlpha(IM_COL32(10, 12, 20, 225), alpha), 28);
	textCentred(at(W - 250 + size * 0.5f, H * 0.5f - 58), withAlpha(t.accent, alpha), std::string(1, now), Huge, 92);
}

void libraryPage(bool active)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	backdrop();
	for (int i = 0; i < library::SourceCount; i++)
		refreshView(i);

	// Which source is on the screen: only one that has games.
	const int sourceBefore = source;
	if (active && in.pressed != 0)
		followWanted = false;
	if (followWanted && sourceShown(wantedSource))
		source = wantedSource;
	if (!sourceShown(source))
	{
		source = library::Internal;
		for (int i = library::SourceCount - 1; i >= 0; i--)
			if (sourceShown(i))
				source = i;
		if (sourceShown(wantedSource))
			source = wantedSource;
	}
	if (active && hit(R1 | L1) && shownSources() > 1)
	{
		const int step = hit(R1) ? 1 : library::SourceCount - 1;
		do
			source = (source + step) % library::SourceCount;
		while (!sourceShown(source));
		wantedSource = source;
		options::frontend().source = source;
		options::saveFrontend();
	}
	if (source != sourceBefore)
		views[source].fresh = true;

	LibraryView& view = views[source];
	const int count = (int)view.games.size();
	// 0 the grid, 1 the list, 2 and up a view in space (ui_flow.cpp); one that
	// is no longer there (its file was taken away) is the grid.
	const int viewMode = options::frontend().view;
	const bool flow = viewMode >= 2 && viewMode - 2 < (int)flowNames().size();
	const bool grid = viewMode == 0 || (viewMode >= 2 && !flow);
	const float top = 128, bottom = H - 64;
	coverLookups = 0;
	int focus = -1;		// the game under the cursor

	if (count == 0 && !view.all.empty())
	{
		// Games, but none that the filters let through.
		libraryWash("");
		const Theme& th = theme();
		const float cx = W * 0.5f, cy = H * 0.46f;
		textCentred(at(cx, cy - 120), withAlpha(th.accent, 0.8f), icon::Sliders, Title, 84);
		textCentred(at(cx, cy), th.text, options::frontend().filter == 1 ? "No favourites here yet"
				: options::frontend().filter == 3 ? "No hidden games here" : "No game here passes the filter", Bold, 34);
		textCentred(at(cx, cy + 64), th.dim, options::frontend().filter == 1
				? "A game becomes a favourite in its details (Triangle), under More. Square changes what is shown."
				: "Square changes what the library shows.", Body, 24);
	}
	else if (count == 0)
	{
		// No games anywhere: what is being looked through, or where games go.
		libraryWash("");
		bool busy = false;
		std::string hint;
		for (int i = 0; i < library::SourceCount && !busy; i++)
			if (library::scanning(i))
			{
				busy = true;
				hint = library::scanStatus(i);
			}
		if (!busy)
		{
			hint = "On the console:  " + library::sourceHint(library::Internal)
					+ "\n\nOn a USB drive:  " + usbHint() + "\n\nOn a network share:  ";
			if (!smb::gameFolders().empty() && !smb::lastError().empty())
				hint += smb::lastError() + "  ";
			hint += library::sourceHint(library::Network);
		}
		emptyLibrary(hint, busy);
	}
	else if (flow)
	{
		view.inShelf = false;
		if (active && (nav(R2) || nav(L2)))
		{
			view.cursor = letterJump(view.games, view.cursor, nav(R2));
			letterShownAt = clock();
		}
		FlowGames games;
		games.count = count;
		games.cover = [&view](int i) { return coverOf(view, i); };
		games.game = [&view](int i) -> const library::Game& { return view.games[(size_t)i]; };
		games.facts = [&view](int i) {
			const library::Game& game = view.games[(size_t)i];
			const Meta& meta = metaOf(view, i);
			std::string line = game.region;
			if (meta.known)
				line += (line.empty() ? "" : "  \xc2\xb7  ") + factsLine(meta.info, false);
			if (game.discs.size() > 1)
				line += (line.empty() ? "" : "  \xc2\xb7  ") + format("%d discs", (int)game.discs.size());
			return line;
		};
		games.about = [&view](int i) {
			const Meta& meta = metaOf(view, i);
			return meta.known ? meta.info.description : std::string();
		};
		flowView(viewMode - 2, games, view.cursor, active, view.fresh, top, bottom);
		view.fresh = false;
		focus = view.cursor;
		libraryWash(view.cover[focus]);
	}
	else if (grid)
	{
		const float margin = 64, gap = 26;
		const int columns = std::max((int)((W - margin * 2 + gap) / (232 + gap)), 3);
		const float cell = (W - margin * 2 - gap * (columns - 1)) / columns;
		const float rowH = cell + 74, shelfRowH = cell + 104, labelH = 46;
		const int shelf = std::min((int)view.recent.size(), columns);
		if (shelf == 0)
			view.inShelf = false;
		view.shelfCursor = std::clamp(view.shelfCursor, 0, std::max(shelf - 1, 0));
		if (active)
		{
			int& c = view.cursor;
			if (view.inShelf)
			{
				if (nav(Right) && view.shelfCursor + 1 < shelf)
					view.shelfCursor++;
				if (nav(Left) && view.shelfCursor > 0)
					view.shelfCursor--;
				if (nav(Down))
				{
					view.inShelf = false;
					c = std::min(view.shelfCursor, count - 1);
				}
				// A letter from the shelf: from the game the shelf's cursor is on.
				if (nav(R2) || nav(L2))
				{
					view.inShelf = false;
					c = letterJump(view.games, view.recent[view.shelfCursor], nav(R2));
					letterShownAt = clock();
				}
			}
			else
			{
				if (nav(Right) && c + 1 < count)
					c++;
				if (nav(Left) && c > 0)
					c--;
				if (nav(Down))
					c = c + columns < count ? c + columns : (c / columns < (count - 1) / columns ? count - 1 : c);
				if (nav(Up))
				{
					if (c - columns >= 0)
						c -= columns;
					else if (shelf > 0)
					{
						view.inShelf = true;
						view.shelfCursor = std::min(c, shelf - 1);
					}
				}
				// L2 and R2: the letter before, the letter after.
				if (nav(R2) || nav(L2))
				{
					c = letterJump(view.games, c, nav(R2));
					letterShownAt = clock();
				}
			}
		}
		focus = view.inShelf ? view.recent[view.shelfCursor] : view.cursor;
		libraryWash(view.cover[focus]);

		// Where things are, from the top of what scrolls.
		const float gridTop = shelf > 0 ? labelH + shelfRowH + labelH : 0;
		const int row = view.cursor / columns;
		const int rows = (count + columns - 1) / columns;
		const float viewH = bottom - top;
		float target = view.scrollTarget;
		if (view.inShelf)
			target = -12;
		else
		{
			const float rowTop = gridTop + row * rowH, rowBottom = rowTop + rowH;
			if (rowTop - target < 12)
				target = rowTop - (row == 0 && shelf > 0 ? labelH + 6 : 12);
			if (rowBottom - target > viewH - 12)
				target = rowBottom - viewH + 12;
		}
		target = std::clamp(target, -12.f, std::max(gridTop + rows * rowH - viewH + 12, -12.f));
		view.scrollTarget = target;
		view.scroll = view.fresh ? target : approach(view.scroll, target, 14.f);
		view.fresh = false;

		draw()->PushClipRect(at(0, top - 8), at(W, bottom), true);
		const float origin = top + 12 - view.scroll;
		if (shelf > 0)
		{
			text(at(margin, origin + 6), t.accent, std::string(icon::Clock) + "   CONTINUE PLAYING", Bold, 20);
			for (int pass = 0; pass < 2; pass++)
				for (int i = 0; i < shelf; i++)
				{
					const bool focused = view.inShelf && i == view.shelfCursor;
					if (focused != (pass == 1))
						continue;
					const int index = view.recent[i];
					const history::Entry played = history::get(view.games[index].path);
					std::string under = agoText(played.lastPlayed);
					const std::string time = playedText(played.seconds);
					if (!time.empty())
						under += (under.empty() ? "" : "  \xc2\xb7  ") + time;
					drawCell(view, index, margin + i * (cell + gap), origin + labelH, cell, focused, under);
				}
			text(at(margin, origin + labelH + shelfRowH + 6), t.accent,
					std::string(icon::Grid) + format("   ALL GAMES   %d", count), Bold, 20);
		}
		const float first = view.scroll - gridTop;
		const int firstRow = std::max((int)(first / rowH) - 1, 0);
		const int lastRow = std::min((int)((first + viewH) / rowH) + 1, rows - 1);
		// The one under the cursor is drawn last, over its neighbours.
		for (int pass = 0; pass < 2; pass++)
			for (int r = firstRow; r <= lastRow; r++)
				for (int col = 0; col < columns; col++)
				{
					const int index = r * columns + col;
					if (index >= count)
						break;
					const bool focused = !view.inShelf && index == view.cursor;
					if (focused != (pass == 1))
						continue;
					drawCell(view, index, margin + col * (cell + gap), origin + gridTop + r * rowH, cell, focused, "");
				}
		draw()->PopClipRect();
	}
	else
	{
		// The list: names at the left, the cover and what is known at the right.
		view.inShelf = false;
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
			if (nav(Right))
				c = std::min(c + visible, count - 1);
			if (nav(Left))
				c = std::max(c - visible, 0);
			if (nav(R2) || nav(L2))
			{
				c = letterJump(view.games, c, nav(R2));
				letterShownAt = clock();
			}
		}
		focus = view.cursor;
		libraryWash(view.cover[focus]);
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
		const Meta& meta = metaOf(view, view.cursor);
		const float px0 = x1 + 40, px1 = W - 64;
		const float side = std::min(px1 - px0, 420.f);
		const Image cover = coverOf(view, view.cursor);
		if (cover.id != nullptr)
			imageFit(cover, at(px0, top), at(px0 + side, top + side), 12);
		else
			coverPlaceholder(at(px0, top), at(px0 + side, top + side), game.name, game.region);
		float y = top + side + 22;
		y += toUnits(textWrapped(at(px0, y), px(px1 - px0), t.text, game.name, Bold, 28, px(76))) + 8;
		std::string facts = game.region;
		if (meta.known)
			facts += (facts.empty() ? "" : "  \xc2\xb7  ") + factsLine(meta.info, true);
		text(at(px0, y), t.accent, facts, Bold, 20);
		y += 38;
		if (meta.known)
			textWrapped(at(px0, y), px(px1 - px0), t.dim, meta.info.description, Body, 21, px(bottom - 24 - y));
	}

	// The bottom line: where the cursor is and what the game is, or what the
	// library is busy with.
	std::string left = libraryStatus();
	if (left.empty() && focus >= 0)
	{
		left = view.inShelf ? std::string("Continue playing") : format("%d of %d", view.cursor + 1, count);
		const Meta& meta = metaOf(view, focus);
		if (meta.known)
			left += "     " + factsLine(meta.info, true);
	}
	std::vector<Hint> hints;
	if (count > 0)
	{
		hints.push_back({ confirmButton, "Play" });
		hints.push_back({ Triangle, "Details" });
	}
	if (count > 0)
	{
		hints.push_back({ L2 | R2, "Letter" });
		hints.push_back({ TouchLeft | TouchRight, "Search" });
	}
	hints.push_back({ Square, "Sort and filter" });
	hints.push_back({ Options, "Menu" });
	hints.push_back({ cancelButton, "Close" });
	if (!libraryBehind)
	{
		hintBar(hints, left);
		if (focus >= 0)
			letterRail(view.games, focus);
	}
	drawHeader();

	if (!active)
		return;
	if (hit(Options))
		push(Page::MainMenu);
	else if (hit(TouchLeft | TouchRight) && count > 0)
		deferred = [] { openSearch(); };
	else if (hit(Square))
		push(Page::LibraryOptions);
	else if (hit(cancelButton))
		// Back from the library is out of the title: asked first, as it is one
		// press of the button that goes back everywhere else.
		push(Page::Confirm, 0, 0, "Close PSSwanStation?", "Back to the console's home screen.", [] { quit = true; });
	else if (focus >= 0 && hit(confirmButton))
	{
		const library::Game game = view.games[focus];
		deferred = [game] { launch(game, -1); };
	}
	else if (focus >= 0 && hit(Triangle))
	{
		const library::Game game = view.games[focus];
		deferred = [game] { openDetails(game); };
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
		list.push_back({ "Shortcuts and rewind", "", icon::Forward, 5, "" });
		list.push_back({ "Games and network", "", icon::Server, 4, "" });
		list.push_back({ "RetroAchievements", "", icon::Trophy, 6, "" });
	}
	else
		list.push_back({ "Picture", "", icon::Screen, 1, "How this game is put on the screen: scaling, the picture "
				"tube, what is beside the picture, pacing and frame generation, for this game alone." });
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
	{
		list.push_back({ "Debug", "", icon::Wrench, 7, "" });
		list.push_back({ "About", "", icon::Info, 20, "" });
	}
	return list;
}

Item toggle(const std::string& label, bool *value, const std::string& info, std::function<void()> changed)
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
		{
			// The same list as the library's own (Square, View).
			const std::vector<std::string> names = libraryViewNames();
			items.push_back(choice("Library view", std::min(f.view, (int)names.size() - 1), names,
					"How the library shows your games. Covers: a grid, with what was played lately on a shelf above "
					"it. List: names, with the cover and the description beside them. Flow, Row, Wall, Cascade and "
					"Wheel stand the covers in space as cases, in the manner of Aurora on the Xbox 360: a flow that "
					"leans towards the one under the cursor, a flat row, three rows across the screen, a line going "
					"away to the right, a wheel at the right. Aurora layout files (.cfljson) put in " + shownRoot()
					+ "layouts are in the list too, after those. The same choice is in the library itself, under "
					"Square.",
					[](int i) { options::frontend().view = i; }));
		}
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
		items.push_back(choice("Interface size", std::clamp((f.uiScale - 80) / 10, 0, 6),
				{ "80%", "90%", "100%", "110%", "120%", "130%", "140%" },
				"Makes the text and everything else of the interface larger or smaller. At the largest sizes long "
				"names are cut short.", [](int i) { options::frontend().uiScale = 80 + i * 10; }));
		items.push_back(toggle("High contrast", &f.highContrast,
				"Black behind the menus, white text, the dimmer text brighter and the cursor's row stronger: easier to "
				"read from the sofa or with weaker sight."));
		items.push_back(toggle("Colour-blind safe colours", &f.colourBlind,
				"What is good and what is bad (ahead or behind in the speedrun timer, an achievement earned, a check "
				"that passed) in blue and orange, which are told apart with every common colour blindness, instead "
				"of green and red."));
		items.push_back(choice("Animations", f.animations, { "Full", "Reduced", "Off" },
				"Full: the swan flies from the start-up screen to its corner, looks about while it sits there, "
				"and flies at the screen when a game starts. Reduced: it stays still, the start-up screen only "
				"fades, and a game starts at once. Off: nothing moves at all - no start-up screen, and lists and "
				"pictures jump to their places.",
				[](int i) { options::frontend().animations = i; }));
		{
			Item item = toggle("Start-up animation", &f.splash,
					"How PSSwanStation opens: the swan paddles along the bottom of the screen, takes the lift up to "
					"the middle, and flies to its corner as the library comes in. Any button skips it.");
			if (f.animations == 2)
			{
				item.enabled = false;
				item.value = "Off";
				item.info += " Not shown while Animations is Off.";
			}
			items.push_back(std::move(item));
		}
		{
			Item item = toggle("Start-up sound", &f.splashSound,
					"What the start-up animation sounds like: the water, the lift and its bell, the swan's wings. "
					"It follows the Volume setting (Sound).");
			if (f.animations == 2 || !f.splash)
			{
				item.enabled = false;
				item.value = "Off";
				item.info += f.animations == 2 ? " Not heard while Animations is Off."
						: " Not heard while the start-up animation is off.";
			}
			items.push_back(std::move(item));
		}
		items.push_back(toggle("Interface sounds", &f.uiSounds,
				"Small sounds in the menus: the cursor moving, a choice, a step back, a letter typed. They follow "
				"the Volume setting (Sound)."));
		items.push_back(choice("Confirm button", f.swapConfirm ? 1 : 0, { "Cross", "Circle" },
				"Which button confirms in the menus; the other one goes back. Games are not affected.",
				[](int i) { options::frontend().swapConfirm = i != 0; }));
		items.push_back(toggle("Show frame rate", &f.showFps,
				"Shows, in a corner of the game, how many frames a second the emulator runs and how that compares "
				"with the game's own speed."));
		items.push_back(toggle("Console notices", &f.notifications,
				"Lets PSSwanStation use the console's pop-up notices for things that matter outside its own screen "
				"(a start-up problem, for one).",
				[] { diag::setNotifications(options::frontend().notifications); }));
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
	{
		// Who is holding a controller.
		std::string who;
		int connected = 0;
		for (int i = 0; i < platform::MaxPads; i++)
			if (platform::pad(i).connected)
			{
				connected++;
				who += (who.empty() ? "" : ", ") + std::to_string(i + 1);
			}
		items.push_back(fact("Controllers", connected == 0 ? std::string("None") : connected == 1 ? std::string("Player 1")
				: "Players " + who,
				"Player 1 is whoever started PSSwanStation. Every other player is another user logged in on the console "
				"with a controller of their own (press the PS button on it and choose a user), in the order they "
				"joined; a controller that joins while PSSwanStation runs is taken up within a few seconds. Any of "
				"them moves through these menus."));
		static const char *const modes[4] = { "Disabled", "Port1Only", "Port2Only", "BothPorts" };
		static const char *const tapKey = "swanstation_ControllerPorts_MultitapMode";
		int mode = 0;
		if (const char *now = options::get(tapKey))
			for (int i = 0; i < 4; i++)
				if (!strcmp(now, modes[i]))
					mode = i;
		items.push_back(choice("Multitap", mode, { "Off", "In port 1", "In port 2", "In both ports" },
				"For games made for three or four players. In port 1: the four players are on the multitap, which "
				"is what most of those games expect. In port 2: player 1 is in port 1 and the others on the multitap. "
				"Leave it off for games for one or two: some do not see a controller behind a multitap.",
				[](int i) { options::set(tapKey, modes[i], false); }));
		for (int player = 0; player < 4; player++)
		{
			Item item = choice(format("Player %d's controller", player + 1), f.controller[player],
					{ "Digital controller", "DualShock", "Analog joystick", "None", "neGcon", "GunCon (light gun)" },
					"What the game finds plugged in for this player. DualShock suits most games; a few early ones only "
					"know the digital controller. The neGcon is Namco's twisting controller, which racing games of "
					"the time steer finely with: the left stick (or the pad leant, see Tilt steering) is its twist, "
					"R2 and L2 its two analogue buttons. The GunCon is a light gun: the sticks move its aim, as does "
					"turning the pad when its motion sensor answers; R2 or Cross fires, L2 or Circle fires away "
					"from the screen (which reloads), Square and Triangle are its two buttons.",
					[player](int i) {
						options::frontend().controller[player] = i;
						host::applyControllers();
					});
			if (player >= 2 && mode == 0)
			{
				item.enabled = false;
				item.value = "Needs the multitap";
			}
			items.push_back(std::move(item));
		}
	}
		items.push_back(choice("Stick dead zone", (int)std::lround(f.deadZone * 20.f),
				{ "0%", "5%", "10%", "15%", "20%", "25%", "30%", "35%", "40%" },
				"How far a stick must move before the game sees it.",
				[](int i) { options::frontend().deadZone = (float)i * 0.05f; }));
		items.push_back(toggle("Vibration", &f.rumble, "Passes the game's vibration to the DualSense."));
		items.push_back(fact("Select and Start", "Touch pad",
				"The left half of the touch pad is the PlayStation's Select, the right half is Start: press the pad "
				"down on that side. OPTIONS opens PSSwanStation's menu."));
		break;
	case 4:
		items.push_back(toggle("Save when a game is closed", &f.autoSaveOnExit,
				"Keeps a state of the game as it is when you close it, apart from the ten state slots."));
		items.push_back(toggle("Continue where I left off", &f.autoLoadOnStart,
				"Starts a game from the state kept when it was last closed, when there is one. The game's details "
				"page (Triangle) can still start it from the beginning."));
		items.push_back(toggle("Sleep-safe saving", &f.sleepSafe,
				"Saves the game's resume state every five minutes of play and whenever the console's own menu (the PS "
				"button) opens over it, quietly and without a pause. When the console closes PSSwanStation (rest "
				"mode, or closing it from the console's menu) the next start offers to continue from there. It uses "
				"the same state as \"Save when a game is closed\". Not yet tried on a console: if PSSwanStation "
				"stops when a game starts with this on, switch it off."));
		items.push_back(toggle("Load network games into memory", &f.ramCache,
				"Reads a game from the network share completely before it starts, so the game never waits for the "
				"network while it runs. Off starts sooner and reads as the game asks."));
		items.push_back(toggle("USB drives", &f.usb,
				"Lets PSSwanStation read games from USB drives (a folder named psx, ps1 or playstation at the top "
				"of the drive). For that PSSwanStation has to leave its sandbox when it starts, which needs a resident "
				"Lapy service or the ELF loader listening on port 9021; it takes effect the next time PSSwanStation "
				"starts."));
		{
			std::string folders;
			for (const std::string& folder : smb::gameFolders())
				folders += (folders.empty() ? "" : ", ") + folder;
			items.push_back(fact("Network share", folders.empty() ? "Not set" : folders,
					(folders.empty() ? std::string("No share is named yet. ") : folders + ". ")
					+ "The share is named in " + shownRoot() + "network.cfg (edit it over FTP); the file explains "
					"itself. PSSwanStation reads it when it starts."));
			items.push_back(fact("Folders", shownRoot(),
					"Games go in " + shownRoot() + "games, BIOS files in bios, covers in covers, your own cheat "
					"files in cheats. Memory cards, states and settings are kept in data."));
		}
		break;
	}
	// What later builds added to each page, and the pages they added.
	moreSettings(kind, items);
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
	items.push_back(header("PSSwanStation"));
	items.push_back(fact("Build", format("%d  \xc2\xb7  work in progress  \xc2\xb7  %s", BuildNumber, BuildDate),
			format("Build %d of %s. A work in progress: not everything has been run on a console yet.", BuildNumber,
			BuildDate)));
	items.push_back(fact("Developer", Developer, std::string("The PS5 port and its interface: ") + Developer + "."));
	items.push_back(fact("Emulator", format("%s %s", info.library_name != nullptr ? info.library_name : "SwanStation",
			info.library_version != nullptr ? info.library_version : ""),
			"SwanStation, linked into this title with a frontend of its own."));
	items.push_back(fact("Graphics", display::deviceName()));
	items.push_back(fact("Display", format("%d x %d at %.2f Hz", display::width(), display::height(),
			display::refreshRate())));
	items.push_back(fact("BIOS", host::biosSummary(),
			host::biosSummary() + ". PSSwanStation carries OpenBIOS and needs no BIOS file; games are more "
			"compatible with an original one (scph5500.bin, scph5501.bin, scph5502.bin) in " + shownRoot() + "bios."));
	items.push_back(fact("Game database", gamedb::summary(),
			gamedb::summary() + ". Descriptions, developers, publishers, release years and genres, and the serial "
			"numbers of disc images by their Redump names, from the libretro database."));
	items.push_back(fact("Cheats and patches", cheats::summary(),
			cheats::summary() + ". The database is chtdb, the community cheat collection; a game's entries are in its "
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
			"SwanStation is free software under the GNU General Public License, version 3, kept by the libretro team "
			"and its contributors. This title's source is the SwanStation source plus the ps5 folder."));
	items.push_back(fact("Mesa RADV", "MIT", "The Vulkan driver, from the PS5 Mesa port."));
	items.push_back(fact("Dear ImGui", "MIT", "The interface is drawn with Dear ImGui by Omar Cornut."));
	items.push_back(fact("libsmb2", "LGPL-2.1", "Network shares are read with libsmb2 by Ronnie Sahlberg."));
	items.push_back(fact("Game database", "CC BY-SA 4.0", "The libretro database's PlayStation lists "
			"(github.com/libretro/libretro-database), Creative Commons Attribution-ShareAlike 4.0."));
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
	// As many as fit; when they do not (a large interface size), the list
	// scrolls with the chosen one.
	const int fit = std::max(1, (int)((bottom - top - 28) / 58));
	const int firstShown = count <= fit ? 0 : std::clamp(f.a - fit / 2, 0, count - fit);
	for (int i = firstShown; i < count && i < firstShown + fit; i++)
	{
		const float y = top + 14 + (i - firstShown) * 58;
		const bool selected = i == f.a;
		if (selected)
			panel(at(cx0 + 10, y), at(cx1 - 10, y + 54), t.accentSoft, 12);
		text(at(cx0 + 30, y + 15), selected ? t.accent : t.faint, categories[i].icon, Body, 24);
		textFit(at(cx0 + 80, y + 13), px(cx1 - cx0 - 100), selected ? t.text : t.dim, categories[i].name,
				selected ? Bold : Body, 26);
	}

	std::vector<Item> items;
	if (category.kind == 1)
		pictureItems(forGame, items);
	else if (category.kind < 10)
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
		float listWidth)
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

void standardHints(const Item *focused, const char *confirm)
{
	std::vector<Hint> hints;
	if (focused != nullptr && focused->enabled && (focused->activate || !focused->choices.empty()))
		hints.push_back({ confirmButton, focused->confirmHint.empty() ? confirm : focused->confirmHint });
	if (focused != nullptr && focused->enabled && !focused->menu && (focused->adjust || focused->choices.size() > 1))
		hints.push_back({ Left | Right, "Change" });
	if (focused != nullptr && focused->alt)
		hints.push_back({ Square, focused->altHint });
	hints.push_back({ cancelButton, "Back" });
	hintBar(hints);
}

void resume()
{
	stack.clear();
	// OPTIONS, if that is what did it, is not the next menu's too.
	optionsSpent = true;
}

void mainMenuPage(Frame& f)
{
	std::vector<Item> items;
	items.push_back(action(icon::Gear, "Settings", "The interface, the picture and the sound, controllers, where "
			"games come from, and every setting of the emulator.", [] { push(Page::Settings); }));
	items.push_back(action(icon::Search, "Search", "Finds a game by a part of its name, in the games folder, on the USB "
			"drives and on the network together. From the library: press the touch pad.", [] {
				pop();
				openSearch();
			}));
	items.push_back(action(icon::Card, "Memory cards", "What is saved on each memory card: copy a save to another "
			"card, delete it, take saves in from files and put them out as files, and go back to an earlier copy "
			"of a card.", [] { push(Page::Cards); }));
	items.push_back(action(icon::Chip, "Start the BIOS", "Starts the PlayStation without a disc: the memory card "
			"manager and the CD player of an original BIOS, when one is in the bios folder.", [] { startBios(); }));
	items.push_back(action(icon::Sync, "Scan for games", "Looks through the games folder, the USB drives and the "
			"network share again.", [] {
				scanEverything();
				pop();
			}));
	{
		const update::Status newer = update::status();
		Item item = action(icon::Download, "Update", "Asks the releases page whether a newer build is out, and puts "
				"it in place of this one. Your games, saves and settings are not touched.", [] { push(Page::Update); });
		if (newer.state == update::State::Available || newer.state == update::State::Ready)
			item.value = format("Build %d is out", newer.build);
		items.push_back(item);
	}
	items.push_back(action(icon::Info, "About", "Versions, folders and licences.", [] {
		const int about = (int)settingsCategories(false).size() - 1;
		push(Page::Settings, about);
	}));
	items.push_back(action(icon::Power, "Close PSSwanStation", "Back to the console's home screen.", [] { quit = true; }));
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
				"chtdb database, and your own from the cheats folder.",
				[] { push(Page::Cheats); });
		item.value = cheats::list().empty() ? "None" : on != 0 ? format("%d on", on)
				: format("%d", (int)cheats::list().size());
		items.push_back(item);
	}
	pauseMoreItems(items);
	items.push_back(action(icon::Sliders, "Game settings", "The picture's and the emulator's settings for this game "
			"alone: what is set there is kept with the game and used whenever it runs.",
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
	const Theme& t = theme();
	const bool load = f.a == 1;
	std::vector<Item> items;
	// Which slot each row is.
	std::vector<int> slots;
	if (load)
	{
		std::string when;
		if (host::stateExists(host::ResumeSlot, &when))
		{
			Item item = action(icon::Clock, "Where the game was closed", "",
					[] {
						if (host::loadState(host::ResumeSlot))
							resume();
					});
			item.value = when;
			items.push_back(item);
			slots.push_back(host::ResumeSlot);
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
			{
				// Its picture is another now.
				forgetImage(host::stateThumbPath(slot));
				resume();
			}
		};
		items.push_back(item);
		slots.push_back(slot);
	}
	if (f.fresh && !load)
		f.cursor = host::quickSlot();
	const Item *focused = menuPage(f, load ? "Load state" : "Save state", host::game().title, items, 700);
	// What the game showed when the state under the cursor was saved.
	const float px0 = 64 + 700 + 24, pw = std::min(unitsWide() - 64 - px0, 720.f), ph = pw * 0.75f;
	const int slot = f.cursor >= 0 && f.cursor < (int)slots.size() ? slots[(size_t)f.cursor] : -1;
	panel(at(px0, 128), at(px0 + pw, 128 + ph + 24), withAlpha(t.panel, 0.7f), 16);
	const Image picture = slot != -1 && host::stateExists(slot) ? image(host::stateThumbPath(slot)) : Image();
	if (picture.id != nullptr)
		imageFit(picture, at(px0 + 12, 140), at(px0 + pw - 12, 140 + ph), 10);
	else
		textCentred(at(px0 + pw * 0.5f, 128 + ph * 0.5f), t.faint, slot != -1 && host::stateExists(slot)
				? "No picture was kept with this state" : "Empty", Body, 24);
	if (host::restricted() && load)
		textWrapped(at(px0, 128 + ph + 44), px(pw), t.bad, "Hardcore mode (RetroAchievements) is on: states are not loaded.",
				Body, 22);
	standardHints(focused, load ? "Load" : "Save");
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

// The cheats and patches of the game they were loaded for, as rows. Before the
// game runs (its details) the choices are only kept; a cheat that is applied
// once on request needs the game running.
void cheatItems(std::vector<Item>& items, bool running)
{
	std::vector<cheats::Cheat>& list = cheats::list();
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
			item.enabled = cheat.supported && (running || !cheat.manual);
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
				item.value = !running ? "While playing" : chosenValue.empty() ? "Run once" : chosenValue;
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
			if (cheat.manual && !running)
				item.info += (item.info.empty() ? "" : "\n\n")
						+ std::string("Applied once, on request, from the menu while the game runs.");
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
}

void cheatsPage(Frame& f)
{
	const Theme& t = theme();
	std::vector<Item> items;
	cheatItems(items, true);
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
	// Why none of them does anything just now, where that is so.
	std::string under = host::game().title + "  \xc2\xb7  " + serial;
	if (netplay::active())
		under += "  \xc2\xb7  off while playing with someone over the network";
	else if (host::restricted())
		under += "  \xc2\xb7  off in hardcore mode";
	standardHints(menuPage(f, "Cheats and patches", under, items, 1100));
}

// ------------------------------------------------------------ a game's details

// A game of the library, looked at before it is started: what it is, and its
// own states, settings and cheats.
enum DetailsTab { TabOverview, TabStates, TabOptions, TabCheats, TabMore };

struct Details
{
	library::Game game;
	int disc = 0;				// the one a start puts in the tray
	std::string serial;			// of that disc; empty when not known
	bool serialRead = false;	// the disc itself was asked
	bool serialPending = false;	// and, being on a share, has not answered yet
	bool known = false;
	gamedb::Info info;
	history::Entry played;
	std::string cover;
	int action = 0;				// Play, Load state, Options, Cheats
	std::string cheatsFor;		// the serial the cheat list is loaded for
} det;

const std::string& detailsDiscPath()
{
	return det.game.discs.empty() ? det.game.path
			: det.game.discs[(size_t)std::clamp(det.disc, 0, (int)det.game.discs.size() - 1)];
}

// A disc on a network share is asked for its serial on a thread of its own.
// The share may have to be connected first, or a NAS woken, which takes
// seconds, and the screen does not wait for that: it says the disc is being
// read and goes on. One disc at a time; an answer for a disc nobody asks
// about any more is dropped and the disc asked about now is read instead.
struct SerialReader
{
	std::mutex mutex;
	std::thread thread;
	bool running = false;
	std::string wanted;
	bool done = false;
	std::string donePath, doneSerial;
} serialReader;

void serialAsk(const std::string& path)
{
	SerialReader& r = serialReader;
	std::lock_guard<std::mutex> lock(r.mutex);
	if (r.done && r.donePath == path)
		return;		// the answer is there already
	r.wanted = path;
	r.done = false;
	if (r.running)
		return;
	if (r.thread.joinable())
		r.thread.join();
	r.running = true;
	r.thread = std::thread([] {
		SerialReader& r = serialReader;
		// A game being loaded into memory meanwhile is not this thread's to load.
		smb::streamOnThisThread(true);
		for (;;)
		{
			std::string path;
			{
				std::lock_guard<std::mutex> lock(r.mutex);
				path = r.wanted;
			}
			const double began = now();
			const std::string serial = host::readSerial(path);
			diag::mark("details: a network disc's serial (%s) was read in %.1f s", serial.empty() ? "none" : serial.c_str(),
					now() - began);
			std::lock_guard<std::mutex> lock(r.mutex);
			if (r.wanted != path)
				continue;
			r.donePath = path;
			r.doneSerial = serial;
			r.done = true;
			r.running = false;
			return;
		}
	});
}

bool serialAnswer(const std::string& path, std::string& serial)
{
	SerialReader& r = serialReader;
	std::lock_guard<std::mutex> lock(r.mutex);
	if (!r.done || r.donePath != path)
		return false;
	serial = r.doneSerial;
	return true;
}

// The serial decides what the database, the settings and the cheats are
// about. `ask` reads the disc when nothing quicker knows it: at once from the
// title's own folders, and from a share in the background (detailsPage takes
// the answer when it comes).
void detailsSerial(bool ask)
{
	if (det.serial.empty() && !det.serialRead)
		det.serial = quickSerial(det.game, det.disc);
	if (ask && !det.serialRead)
	{
		const std::string path = detailsDiscPath();
		if (smb::isNetworkPath(path))
		{
			serialAsk(path);
			det.serialPending = true;
		}
		else
		{
			det.serialRead = true;
			const std::string read = host::readSerial(path);
			if (!read.empty())
				det.serial = read;
		}
	}
	gamedb::Info info;
	det.known = !det.serial.empty() && gamedb::find(det.serial, info);
	if (det.known)
		det.info = info;
	// The game's own settings are shown, and kept, under this serial; its
	// cheats and patches are listed by it.
	options::loadGame(det.serial);
	if (det.serial.empty())
	{
		cheats::unload();
		det.cheatsFor.clear();
	}
	else if (det.cheatsFor != det.serial)
	{
		// A later disc without cheats of its own shows the first disc's.
		cheats::loadFor(det.serial, det.disc > 0 ? quickSerial(det.game, 0) : std::string());
		det.cheatsFor = det.serial;
	}
}

// After a start that failed (it puts the settings back to "no game").
void detailsRestore()
{
	if (stack.empty() || det.game.path.empty())
		return;
	for (const Frame& frame : stack)
		if (frame.page == Page::Details)
		{
			det.cheatsFor.clear();
			detailsSerial(false);
			return;
		}
}

const library::Game& detailsGame()
{
	return det.game;
}

const std::string& detailsSerial_()
{
	return det.serial;
}

void closeDetails()
{
	cheats::unload();
	options::loadGame("");
	det = Details();
}

void openDetails(const library::Game& game)
{
	det = Details();
	det.game = game;
	det.played = history::get(game.path);
	det.disc = game.discs.size() > 1 ? std::clamp(det.played.disc, 0, (int)game.discs.size() - 1) : 0;
	for (const char *ext : { ".png", ".jpg", ".jpeg" })
		if (det.cover.empty() && fileExists(rootDir + "covers/" + game.fileTitle + ext))
			det.cover = rootDir + "covers/" + game.fileTitle + ext;
	// A game in the title's own folders is asked for its serial at once: a
	// few sectors of a local file. A share is only asked when its settings
	// or cheats are opened.
	detailsSerial(!smb::isNetworkPath(detailsDiscPath()));
	push(Page::Details);
}

void detailsTab(Frame& f, int tab)
{
	f.a = tab;
	f.cursor = 0;
	f.scroll = f.scrollTarget = 0;
	f.fresh = true;
	f.picker = -1;
	if (tab == TabOptions || tab == TabCheats)
		detailsSerial(true);
}

void detailsPage(Frame& f)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const library::Game& game = det.game;
	const int discs = (int)game.discs.size();
	// A disc on a share that was asked for its serial has answered.
	if (det.serialPending)
	{
		std::string read;
		if (serialAnswer(detailsDiscPath(), read))
		{
			det.serialPending = false;
			det.serialRead = true;
			if (!read.empty())
				det.serial = read;
			detailsSerial(false);
		}
	}
	const float open = motion() == MotionOff ? 1.f : std::clamp((float)((clock() - f.opened) / 0.18), 0.f, 1.f);
	const float ease = 1.f - (1.f - open) * (1.f - open);

	// The panel, over the dimmed library.
	draw()->AddRectFilled(ImVec2(0, 0), ImVec2(width(), height()), IM_COL32(4, 6, 12, (int)(170 * ease)));
	const float w = std::min(W - 160, 1560.f), h = std::min(H - 200, 800.f);
	const float x = (W - w) * 0.5f, y = 96 + (H - 64 - 96 - h) * 0.5f + (1 - ease) * 36;
	draw()->AddRectFilled(at(x - 6, y - 4), at(x + w + 6, y + h + 12), IM_COL32(0, 0, 0, (int)(70 * ease)), px(30));
	panel(at(x, y), at(x + w, y + h), IM_COL32(22, 28, 46, 252), 24);
	outline(at(x, y), at(x + w, y + h), IM_COL32(255, 255, 255, 22), 24, 1.5f);

	// The left column: the cover, the file, the serial, when it was played.
	// Among its saved states, what the game showed at the one under the cursor
	// is there instead.
	const float cx = x + 48, cs = 420;
	Image cover;
	if (f.a == TabStates)
	{
		int row = 0;
		for (const int slot : { (int)host::ResumeSlot, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 })
			if (host::stateExistsFor(game.path, slot) && row++ == f.cursor)
				cover = image(host::stateThumbPathFor(game.path, slot));
	}
	// The cover may have been chosen anew since the details were opened.
	static unsigned coversSeen;
	if (coversSeen != covers::generation())
	{
		coversSeen = covers::generation();
		forgetImage(det.cover);
		det.cover.clear();
		for (const char *ext : { ".png", ".jpg", ".jpeg" })
			if (det.cover.empty() && fileExists(rootDir + "covers/" + game.fileTitle + ext))
				det.cover = rootDir + "covers/" + game.fileTitle + ext;
	}
	if (cover.id == nullptr)
		cover = image(det.cover);
	if (cover.id != nullptr)
		imageFit(cover, at(cx, y + 48), at(cx + cs, y + 48 + cs), 14);
	else
		coverPlaceholder(at(cx, y + 48), at(cx + cs, y + 48 + cs), game.name, game.region);
	float ly = y + 48 + cs + 22;
	textFit(at(cx, ly), px(cs), t.faint, baseName(detailsDiscPath()), Body, 18);
	ly += 32;
	const auto fact = [&](const char *symbol, const std::string& label, const std::string& value) {
		if (value.empty())
			return;
		text(at(cx, ly + 2), t.faint, symbol, Body, 18);
		text(at(cx + 36, ly), t.faint, label, Body, 20);
		textFit(at(cx + 150, ly), px(cs - 150), t.dim, value, Body, 20);
		ly += 34;
	};
	fact(icon::Disc, "ID", !det.serial.empty() ? det.serial : det.serialPending ? "Reading the disc\xe2\x80\xa6"
			: det.serialRead ? "None on the disc" : "");
	fact(icon::Clock, "Played", playedText(det.played.seconds));
	fact(icon::Play, "Last", agoText(det.played.lastPlayed));
	fact(icon::Drive, "Size", sizeText(game.size));
	fact(icon::Server, "From", library::sourceName(game.source));

	// The right column.
	const float tx = x + 48 + cs + 52, tw = x + w - 56 - tx;
	textFit(at(tx, y + 44), px(tw), t.text, game.name, Title, 44);
	std::string meta = "PLAYSTATION";
	const auto add = [&meta](const std::string& part) {
		if (!part.empty())
			meta += "   \xc2\xb7   " + part;
	};
	add(game.region);
	if (det.known)
	{
		if (det.info.year > 0)
			add(format("%d", det.info.year));
		add(det.info.genre);
		if (det.info.players > 0)
			add(det.info.players == 1 ? std::string("1 player") : format("1-%d players", det.info.players));
	}
	if (discs > 1)
		add(format("Disc %d of %d", det.disc + 1, discs));
	textFit(at(tx, y + 108), px(tw), t.accent, meta, Bold, 22);
	if (det.known && (!det.info.developer.empty() || !det.info.publisher.empty()))
	{
		std::string makers = det.info.developer;
		if (!det.info.publisher.empty() && det.info.publisher != det.info.developer)
			makers += (makers.empty() ? "" : "   \xc2\xb7   ") + det.info.publisher;
		textFit(at(tx, y + 142), px(tw), t.dim, makers, Body, 22);
	}
	const float top = y + 196, bottom = y + h - 48;

	// The states kept for it.
	std::vector<std::pair<int, std::string>> states;
	{
		std::string when;
		if (host::stateExistsFor(game.path, host::ResumeSlot, &when))
			states.emplace_back(host::ResumeSlot, when);
		for (int slot = 0; slot < host::StateSlots; slot++)
			if (host::stateExistsFor(game.path, slot, &when))
				states.emplace_back(slot, when);
	}
	const int disc = det.disc;

	if (f.a == TabOverview)
	{
		std::string description = det.known ? det.info.description : std::string();
		if (description.empty())
			description = det.serial.empty() && !det.serialRead
					? "The game's description is found by its serial number, which is read from the disc when "
					"Options or Cheats is opened, and when the game starts."
					: det.serial.empty() ? "This disc has no serial number, so nothing is known about it."
					: "No description of " + det.serial + " in the database.";
		const float pillsY = bottom - 64;
		const float discsY = pillsY - 66;
		const float textBottom = (discs > 1 ? discsY : pillsY) - 28;
		textWrapped(at(tx, top), px(tw), det.known ? IM_COL32(200, 208, 226, 255) : t.faint, description, Body, 24,
				px(textBottom - top));
		if (discs > 1)
		{
			float dx = tx;
			for (int i = 0; i < discs; i++)
			{
				const std::string label = format("Disc %d", i + 1);
				const ImVec2 extent = measure(label, Bold, 20);
				const float pw = toUnits(extent.x) + 36;
				panel(at(dx, discsY), at(dx + pw, discsY + 40), i == det.disc ? t.text : t.panelHigh, 20);
				text(at(dx + 18, discsY + 20 - toUnits(extent.y) * 0.5f), i == det.disc ? IM_COL32(16, 22, 38, 255) : t.dim,
						label, Bold, 20);
				dx += pw + 10;
			}
			buttonGlyph(at(dx + 28, discsY + 20), 28, Square);
			text(at(dx + 52, discsY + 8), t.faint, "Next disc", Body, 20);
		}
		// What can be done, side by side.
		int on = 0;
		for (const cheats::Cheat& cheat : cheats::list())
			on += cheat.enabled;
		const std::string labels[5] = {
			std::string(icon::Play) + "   Play",
			std::string(icon::Upload) + "   Load state",
			std::string(icon::Sliders) + "   Options",
			std::string(icon::Bolt) + (on > 0 ? format("   Cheats   %d on", on) : std::string("   Cheats")),
			std::string(library::favourite(game.path) ? icon::Heart : icon::Star) + "   More",
		};
		const bool enabled[5] = { true, !states.empty(), true, true, true };
		det.action = std::clamp(det.action, 0, 4);
		if (f.picker < 0)
		{
			if (nav(Right))
				det.action = std::min(det.action + 1, 4);
			if (nav(Left))
				det.action = std::max(det.action - 1, 0);
		}
		ImVec2 at0 = at(tx, pillsY);
		for (int i = 0; i < 5; i++)
			at0.x += pill(at0, labels[i], i == det.action, enabled[i], 64, 24) + px(14);
		if (det.action == 1 && states.empty())
			text(at(tx, pillsY - (discs > 1 ? 104 : 38)), t.faint,
					"No state is saved for this game yet: save one from the menu while playing.", Body, 20);

		std::vector<Hint> hints;
		hints.push_back({ confirmButton, det.action == 0 ? "Play" : "Open" });
		if (discs > 1)
			hints.push_back({ Square, "Next disc" });
		hints.push_back({ cancelButton, "Back" });
		hintBar(hints);

		if (discs > 1 && hit(Square))
		{
			det.disc = (det.disc + 1) % discs;
			det.serial.clear();
			det.serialRead = false;
			det.serialPending = false;
			deferred = [] { detailsSerial(!smb::isNetworkPath(detailsDiscPath())); };
		}
		else if (hit(confirmButton) && enabled[det.action])
		{
			const int action = det.action;
			if (action == 0)
				deferred = [game, disc] { launch(game, -1, disc); };
			else
				deferred = [action] {
					if (!stack.empty())
						detailsTab(stack.back(), action);
				};
		}
		else if (hit(cancelButton))
		{
			deferred = [] {
				pop();
				closeDetails();
			};
			consumeInput();
		}
		return;
	}

	// A tab: its name, then its rows.
	static const char *names[] = { "", "Start from a saved state", "Options for this game", "Cheats and patches",
			"More about this game" };
	static const char *symbols[] = { "", icon::Upload, icon::Sliders, icon::Bolt, icon::Star };
	text(at(tx, top + 2), t.accent, symbols[f.a], Body, 26);
	text(at(tx + 44, top), t.text, names[f.a], Bold, 28);
	if (f.a == TabOptions && !det.serial.empty())
		textRight(at(tx + tw, top + 6), t.faint, "kept for " + det.serial, Body, 20);
	else if (f.a == TabCheats && !cheats::serial().empty())
		textRight(at(tx + tw, top + 6), t.faint, "kept for " + cheats::serial(), Body, 20);
	const float listTop = top + 56;

	std::vector<Item> items;
	std::string nothing;
	bool reading = false;
	if (f.a == TabStates)
	{
		for (const auto& [slot, when] : states)
		{
			Item item;
			item.icon = slot == host::ResumeSlot ? icon::Clock : icon::Upload;
			item.label = slot == host::ResumeSlot ? std::string("Where the game was closed") : format("Slot %d", slot + 1);
			item.value = when;
			item.confirmHint = "Play from here";
			item.activate = [game, slot, disc] { launch(game, slot, disc); };
			items.push_back(std::move(item));
		}
		Item fresh = action(icon::Play, "From the beginning", "", [game, disc] { launch(game, FromBeginning, disc); });
		fresh.confirmHint = "Play";
		items.push_back(std::move(fresh));
	}
	else if (f.a == TabMore)
		detailsMoreItems(items);
	else if (det.serial.empty() && det.serialPending)
	{
		// Not known yet: the share is being asked, and may take its time.
		const smb::Status status = smb::status();
		nothing = status.waiting ? status.text + "." : "Reading the disc's serial number from the network share\xe2\x80\xa6";
		nothing += "\n\nThis game's own settings and its cheats are kept by that number. It is read once: after the "
				"game has been played, it is known.";
		reading = true;
	}
	else if (det.serial.empty())
		nothing = "This disc has no serial number that could be read (a PlayStation program, or an image that did "
				"not open). A game's own settings and its cheats are kept by that number.";
	else if (f.a == TabOptions)
	{
		// How the game is put on the screen first, then the emulator's.
		items.push_back(header("PICTURE"));
		pictureItems(true, items);
		for (const options::Category& category : options::categories())
		{
			std::string name = category.name;
			const size_t tail = name.rfind(" Settings");
			if (tail != std::string::npos && tail + 9 == name.size())
				name.erase(tail);
			for (char& c : name)
				if (c >= 'a' && c <= 'z')
					c = (char)(c - 'a' + 'A');
			items.push_back(header(name));
			optionItems(category.key, false, true, items);
		}
	}
	else
	{
		cheatItems(items, false);
		if (items.empty())
			nothing = "The database has no cheats or patches for " + det.serial + ". A file of your own, "
					+ shownRoot() + "cheats/" + det.serial + ".cht, is read when the game starts.";
	}

	if (!nothing.empty())
	{
		const float used = toUnits(textWrapped(at(tx, listTop + 8), px(tw), t.dim, nothing, Body, 24));
		if (reading)
			progressBar(at(tx, listTop + 8 + used + 24), at(tx + std::min(tw, 520.f), listTop + 8 + used + 36), -1.f);
		hintBar({ { cancelButton, "Back" } });
	}
	else
	{
		// The rows, and under them what the one under the cursor is about.
		const float infoH = f.a == TabStates ? 0 : f.a == TabMore ? 110 : 84;
		const Item *focused = runList(f, items, tx - 14, listTop, tx + tw - 4, bottom - infoH);
		if (focused != nullptr && infoH > 0)
		{
			draw()->AddLine(at(tx, bottom - infoH + 10), at(tx + tw, bottom - infoH + 10), IM_COL32(255, 255, 255, 18), 1.f);
			std::string info = focused->info;
			std::replace(info.begin(), info.end(), '\n', ' ');
			textWrapped(at(tx, bottom - infoH + 22), px(tw), t.faint, info, Body, 20, px(infoH - 22));
		}
		standardHints(focused);
	}
	if (f.picker < 0 && hit(cancelButton))
	{
		deferred = [] {
			if (!stack.empty())
				detailsTab(stack.back(), TabOverview);
		};
		consumeInput();
	}
}

// A game on the network is being made ready: read into memory, or its files
// checked. The swan leaves its box in the header for the bar, flies along
// over where the reading has got to, and when the game is ready flies at the
// screen as it does for any game; then the emulator starts it. What is being
// done, how fast and how long it will take are under the bar, where the swan
// never is.
struct LoadingState
{
	double readyAt = -1;			// when the reading ended, by the animations' clock
	bool ready = false, failed = false;
	float shown = 0;				// where the swan is along the bar
	// How fast the reading goes: bytes and the real time they were seen at.
	double sampleAt = 0;
	uint64_t sampleDone = 0;
	double speed = 0;				// bytes a second, smoothed
} loadingState;

void loadingBegin()
{
	loadingState = LoadingState();
}

void loadingPage(Frame& f)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	const bool flies = motion() == MotionFull;
	const smb::Status status = smb::status();
	const int state = smb::precacheState();
	LoadingState& l = loadingState;
	if (state != smb::PrecacheRunning && l.readyAt < 0)
	{
		l.readyAt = clock();
		l.ready = state == smb::PrecacheDone;
		l.failed = !l.ready;
	}
	// Once it is ready the bar stays full: the reading's own figures are gone.
	const float fraction = l.ready ? 1.f : status.progress;
	const float leaving = l.ready ? std::clamp((float)((clock() - l.readyAt) / (flies ? 0.95 : 0.10)), 0.f, 1.f) : 0.f;

	// The speed, from what arrived over the last half second and more.
	const double time = now();
	if (status.total != 0 && !l.ready)
	{
		if (l.sampleAt == 0 || status.done < l.sampleDone)
		{
			l.sampleAt = time;
			l.sampleDone = status.done;
		}
		else if (time - l.sampleAt >= 0.5)
		{
			const double rate = (double)(status.done - l.sampleDone) / (time - l.sampleAt);
			l.speed = l.speed <= 0 ? rate : l.speed * 0.6 + rate * 0.4;
			l.sampleAt = time;
			l.sampleDone = status.done;
		}
	}

	headerSwanAway = flies;
	draw()->AddRectFilled(ImVec2(0, 0), ImVec2(width(), height()), IM_COL32(4, 6, 12, 150));
	const float w = 960, h = flies ? 340 : 240;
	const float x0 = (W - w) * 0.5f, y0 = (H - h) * 0.5f;
	const float left = x0 + 48, right = x0 + w - 48;
	const float barY = y0 + h - 118;
	draw()->AddRectFilled(at(x0 - 6, y0 - 4), at(x0 + w + 6, y0 + h + 12), IM_COL32(0, 0, 0, 70), px(26));
	panel(at(x0, y0), at(x0 + w, y0 + h), IM_COL32(24, 30, 48, 252), 20);
	outline(at(x0, y0), at(x0 + w, y0 + h), IM_COL32(255, 255, 255, 22), 20, 1.5f);
	// The game's name, and how far it is.
	textFit(at(left, y0 + 36), px(right - left - 130), t.text, f.s2, Bold, 32);
	if (fraction >= 0)
		textRight(at(right, y0 + 36), t.accent, format("%d%%", (int)(fraction * 100.f)), Bold, 32);
	progressBar(at(left, barY), at(right, barY + 16), fraction);
	// Under the bar: what is being done; how fast, and how long still.
	std::string doing;
	if (l.ready)
		doing = "Starting the game";
	else if (status.waiting)
		doing = status.text;
	else if (status.total != 0)
		doing = format("Loading into memory   %u of %u MB", (unsigned)(status.done >> 20), (unsigned)(status.total >> 20));
	else
		doing = options::frontend().ramCache ? "Opening the game on the network share" : "Checking the game's files";
	text(at(left, barY + 38), l.ready ? t.text : t.dim, doing, Body, 24);
	if (!l.ready && !status.waiting && status.total != 0 && l.speed > 64 * 1024)
	{
		const double seconds = (double)(status.total - status.done) / l.speed;
		const std::string remaining = seconds < 1.5 ? std::string("a moment")
				: seconds < 90 ? format("%d s", (int)std::lround(seconds)) : format("%d min", (int)std::lround(seconds / 60.0));
		textRight(at(right, barY + 40), t.faint, format("%.0f MB/s  \xc2\xb7  %s left", l.speed / (1 << 20),
				remaining.c_str()), Body, 22);
	}

	if (flies)
	{
		// Along the bar it keeps a lane of its own, over the bar and under the name.
		const float target = fraction >= 0 ? fraction : 0.f;
		l.shown = l.ready ? 1.f : approach(l.shown, target, 7.f);
		const float lane = px(88);
		const ImVec2 onBar(px(left) + px(right - left) * l.shown,
				px(barY - 56) + std::sin((float)clock() * 2.3f) * px(4));
		// From its box in the header to the bar, as the page opens.
		ImVec2 headerA, headerB, from;
		float fromSize = 0;
		headerBox(headerA, headerB);
		swanPlace(headerA, headerB, from, fromSize);
		const float arrive = glide((float)((clock() - f.opened) / 0.60));
		const ImVec2 box(from.x + fromSize * 0.5f, from.y + fromSize * 0.5f);
		ImVec2 c(box.x + (onBar.x - box.x) * arrive, box.y + (onBar.y - box.y) * arrive - std::sin(arrive * 3.14159f) * px(60));
		float size = fromSize + (lane - fromSize) * arrive;
		SwanPose pose;
		pose.fly = smooth(arrive / 0.25f);
		pose.face = 1.f - 2.f * smooth(arrive / 0.30f);		// it turns to fly to the right
		pose.beat = (float)clock() * 6.2832f * 2.6f;
		pose.tilt = -0.06f * pose.fly;
		if (leaving > 0)
		{
			// Ready: larger and larger, towards the middle of the screen.
			const float go = smooth(leaving);
			const float grow = go * go * go;
			size = lane * std::pow(height() * 4.2f / lane, grow);
			const ImVec2 middle(width() * 0.50f, height() * 0.47f);
			const float along = smooth(go * 1.15f);
			c = ImVec2(onBar.x + (middle.x - onBar.x) * along,
					onBar.y + (middle.y - onBar.y) * along - std::sin(along * 3.14159f) * height() * 0.05f);
			pose.beat = (float)clock() * 6.2832f * 3.4f;
			pose.tilt = -0.20f * (1.f - go);
		}
		swan(ImVec2(c.x - size * 0.5f, c.y - size * 0.5f), size, pose);
		// The dark comes over it at the end, as when any game starts.
		const float dark = smooth((leaving - 0.62f) / 0.38f);
		if (dark > 0)
			draw()->AddRectFilled(ImVec2(0, 0), ImVec2(width(), height()), IM_COL32(0, 0, 0, (int)(255 * dark)));
	}
	if (!l.ready && !l.failed)
		hintBar({ { cancelButton, "Cancel" } });

	if (state == smb::PrecacheRunning)
	{
		if (hit(cancelButton))
			smb::cancelLoad();
		consumeInput();
		return;
	}
	consumeInput();
	// Ready: the frame that says so is on the screen before the emulator
	// starts the game, which takes a moment in which nothing is drawn.
	if (l.ready && leaving < 1.f)
		return;
	const int slot = f.a, disc = f.b;
	const bool ok = l.ready;
	deferred = [slot, disc, ok] {
		const std::string error = smb::lastError();
		smb::finishPrecache();
		pop();
		headerSwanAway = false;
		if (ok)
			begin(chosen, slot, disc);
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
	// Something to answer or to know is marked in the accent colour; something
	// that went wrong, in red.
	const bool plain = confirm || f.a == 1;
	text(at(x0 + 44, y0 + 36), plain ? t.accent : t.bad, plain ? icon::Info : icon::Warning, Body, 30);
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

// ----------------------------------------------------------------- search

// A game looked for by its name. On the left a keyboard for the pad, on the
// right every game, from all three places, that has what was typed in its
// name: each word typed must be somewhere in it, and names that begin with
// it come first. Right from the keyboard goes to the games, Left comes back.
struct SearchState
{
	std::string query;
	int column = 0, row = 0;			// on the keyboard
	bool inResults = false;
	int cursor = 0;
	float scroll = 0;
	// The games found: the source and the place in its list.
	std::vector<std::pair<int, int>> found;
	std::string foundFor;
	unsigned generations[library::SourceCount] = {};
	bool built = false;
} searchState;

constexpr int SearchColumns = 6, SearchRows = 6;
const char *const searchKeys = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

void openSearch()
{
	searchState = SearchState();
	push(Page::Search);
}

void searchBuild()
{
	SearchState& s = searchState;
	bool same = s.built && s.foundFor == s.query;
	for (int i = 0; i < library::SourceCount; i++)
		same = same && s.generations[i] == views[i].generation;
	if (same)
		return;
	s.built = true;
	s.foundFor = s.query;
	s.found.clear();
	std::vector<std::string> words;
	{
		std::string word;
		for (const char c : lowercase(s.query) + " ")
		{
			if (c != ' ')
				word.push_back(c);
			else if (!word.empty())
			{
				words.push_back(word);
				word.clear();
			}
		}
	}
	const std::string whole = lowercase(trim(s.query));
	std::vector<std::pair<int, int>> later;
	for (int source = 0; source < library::SourceCount; source++)
	{
		s.generations[source] = views[source].generation;
		for (int i = 0; i < (int)views[source].all.size(); i++)
		{
			// A hidden game is not found either.
			if (library::hidden(views[source].all[(size_t)i].path))
				continue;
			const std::string name = lowercase(views[source].all[(size_t)i].name);
			bool all = true;
			for (const std::string& word : words)
				all = all && name.find(word) != std::string::npos;
			if (!all)
				continue;
			(whole.empty() || name.rfind(whole, 0) == 0 ? s.found : later).emplace_back(source, i);
		}
	}
	const auto byName = [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
		const std::string x = lowercase(views[a.first].all[(size_t)a.second].name);
		const std::string y = lowercase(views[b.first].all[(size_t)b.second].name);
		return x != y ? x < y : a < b;
	};
	std::sort(s.found.begin(), s.found.end(), byName);
	std::sort(later.begin(), later.end(), byName);
	s.found.insert(s.found.end(), later.begin(), later.end());
	s.cursor = 0;
	s.scroll = 0;
}

void searchPage(Frame&)
{
	const Theme& t = theme();
	const float W = unitsWide(), H = unitsHigh();
	SearchState& s = searchState;
	for (int i = 0; i < library::SourceCount; i++)
		refreshView(i);
	searchBuild();
	const int count = (int)s.found.size();
	if (count == 0)
		s.inResults = false;

	text(at(64, 36), t.text, "Search", Title, 44);
	buildTag();

	// The left side: what was typed, and the keyboard.
	const float kx = 64, key = 76, gap = 8, kw = SearchColumns * key + (SearchColumns - 1) * gap;
	const float fieldY = 132;
	panel(at(kx, fieldY), at(kx + kw, fieldY + 68), t.panel, 14);
	outline(at(kx, fieldY), at(kx + kw, fieldY + 68), s.inResults ? IM_COL32(255, 255, 255, 26) : withAlpha(t.accent, 0.7f), 14, 2);
	text(at(kx + 20, fieldY + 20), t.faint, icon::Search, Body, 26);
	if (s.query.empty())
		text(at(kx + 62, fieldY + 19), t.faint, "A part of the name", Body, 26);
	else
	{
		const float end = textFit(at(kx + 62, fieldY + 17), px(kw - 100), t.text, s.query, Bold, 28);
		// The caret, blinking where the next letter goes.
		if (!s.inResults && (motion() == MotionOff || std::fmod(clock(), 1.0) < 0.6))
			draw()->AddRectFilled(ImVec2(px(kx + 62) + end + px(3), px(fieldY + 18)),
					ImVec2(px(kx + 62) + end + px(6), px(fieldY + 50)), t.accent);
	}
	const float keysY = fieldY + 92;
	const int actionRow = SearchRows;		// Space, Delete, Clear
	const float actionW = (kw - 2 * gap) / 3.f;
	static const char *const actions[3] = { "Space", "Delete", "Clear" };
	for (int r = 0; r <= SearchRows; r++)
	{
		const int columns = r == actionRow ? 3 : SearchColumns;
		for (int c = 0; c < columns; c++)
		{
			const float w = r == actionRow ? actionW : key;
			const float x = kx + (float)c * (w + gap), y = keysY + (float)r * (key - 8 + gap);
			const bool focused = !s.inResults && s.row == r && (r == actionRow ? s.column / 2 == c : s.column == c);
			panel(at(x, y), at(x + w, y + key - 8), focused ? t.accent : t.panelHigh, 12);
			const std::string label = r == actionRow ? actions[c] : std::string(1, searchKeys[r * SearchColumns + c]);
			textCentred(at(x + w * 0.5f, y + (r == actionRow ? 21 : 16)), focused ? IM_COL32(8, 12, 22, 255) : t.text, label, Bold,
					r == actionRow ? 22 : 30);
		}
	}

	// The right side: the games.
	const float rx0 = kx + kw + 56, rx1 = W - 64, top = 132, bottom = H - 64 - 24, rowH = 64;
	const int visible = std::max((int)((bottom - top - 60) / rowH), 1);
	text(at(rx0, top + 6), t.dim, count == 0 ? std::string("No game has that in its name")
			: s.query.empty() ? format("All %d games", count) : format("%d game%s", count, count == 1 ? "" : "s"), Bold, 22);
	const float listTop = top + 52;
	if (s.cursor < (int)s.scroll)
		s.scroll = (float)s.cursor;
	if (s.cursor >= (int)s.scroll + visible)
		s.scroll = (float)(s.cursor - visible + 1);
	static const char *const sourceIcons[library::SourceCount] = { icon::Drive, icon::Plug, icon::Network };
	for (int n = 0; n < visible && (int)s.scroll + n < count; n++)
	{
		const int index = (int)s.scroll + n;
		const auto& [source, at2] = s.found[(size_t)index];
		const library::Game& game = views[source].all[(size_t)at2];
		const float y = listTop + (float)n * rowH;
		const bool focused = s.inResults && index == s.cursor;
		if (focused)
		{
			panel(at(rx0 - 12, y + 3), at(rx1, y + rowH - 3), t.panelHigh, 12);
			panel(at(rx0 - 12, y + 14), at(rx0 - 6, y + rowH - 14), t.accent, 3);
		}
		std::string facts = library::sourceName(source);
		if (!game.region.empty())
			facts += "  \xc2\xb7  " + game.region;
		if (game.discs.size() > 1)
			facts += format("  \xc2\xb7  %d discs", (int)game.discs.size());
		const float factsW = toUnits(measure(facts, Body, 20).x);
		textRight(at(rx1 - 24, y + 21), t.faint, facts, Body, 20);
		text(at(rx0 + 10, y + 20), focused ? t.accent : t.faint, sourceIcons[source], Body, 22);
		textFit(at(rx0 + 50, y + 17), px(rx1 - rx0 - 110 - factsW), focused ? t.text : IM_COL32(214, 220, 234, 255), game.name,
				Body, 26);
	}
	if (count > visible)
	{
		// Where in the list this is.
		const float trackH = (float)visible * rowH, barH = std::max(trackH * (float)visible / (float)count, 30.f);
		const float barY = listTop + (trackH - barH) * (s.scroll / (float)(count - visible));
		panel(at(rx1 + 10, barY), at(rx1 + 15, barY + barH), IM_COL32(255, 255, 255, 60), 3);
	}

	// The pad.
	bool changed = false;
	if (!s.inResults)
	{
		const int columns = s.row == actionRow ? 3 : SearchColumns;
		int c = s.row == actionRow ? s.column / 2 : s.column;
		if (nav(Left) && c > 0)
			c--;
		else if (nav(Right))
		{
			if (c + 1 < columns)
				c++;
			else if (count > 0)
				s.inResults = true;
		}
		s.column = s.row == actionRow ? c * 2 : c;
		if (nav(Down) && s.row < actionRow)
			s.row++;
		if (nav(Up) && s.row > 0)
			s.row--;
		const auto erase = [&] {
			// A whole character: a letter of two bytes goes in one piece.
			while (!s.query.empty() && ((unsigned char)s.query.back() & 0xC0) == 0x80)
				s.query.pop_back();
			if (!s.query.empty())
				s.query.pop_back();
			changed = true;
		};
		const auto type = [&](char letter) {
			if (s.query.size() < 40 && !(letter == ' ' && (s.query.empty() || s.query.back() == ' ')))
			{
				s.query.push_back(letter);
				changed = true;
			}
		};
		if (hit(confirmButton))
		{
			if (s.row < actionRow)
				type(searchKeys[s.row * SearchColumns + s.column]);
			else if (s.column / 2 == 0)
				type(' ');
			else if (s.column / 2 == 1)
				erase();
			else
			{
				changed = !s.query.empty();
				s.query.clear();
			}
		}
		else if (hit(Square))
			erase();
		else if (hit(Triangle))
			type(' ');
		if (changed)
			sound::play(sound::Key);
		hintBar({ { confirmButton, "Type" }, { Square, "Delete" }, { Triangle, "Space" }, { Left | Right, "To the games" },
				{ cancelButton, "Back" } });
	}
	else
	{
		if (nav(Down))
			s.cursor = s.cursor + 1 < count ? s.cursor + 1 : (hit(Down) ? 0 : s.cursor);
		if (nav(Up))
			s.cursor = s.cursor > 0 ? s.cursor - 1 : (hit(Up) ? count - 1 : s.cursor);
		if (nav(R2))
			s.cursor = std::min(s.cursor + visible, count - 1);
		if (nav(L2))
			s.cursor = std::max(s.cursor - visible, 0);
		if (nav(Left))
			s.inResults = false;
		const auto& [source, index] = s.found[(size_t)std::clamp(s.cursor, 0, count - 1)];
		if (hit(confirmButton))
		{
			const library::Game game = views[source].all[(size_t)index];
			deferred = [game] { launch(game, -1); };
		}
		else if (hit(Triangle))
		{
			const library::Game game = views[source].all[(size_t)index];
			deferred = [game] { openDetails(game); };
		}
		hintBar({ { confirmButton, "Play" }, { Triangle, "Details" }, { Left | Right, "To the keyboard" }, { cancelButton, "Back" } });
	}
}

// --------------------------------------------------------------- the splash

// The start-up animation, with its sound (sound.cpp; the times of both are
// splashtime's, fe.h).
//
// The swan's head comes up out of the water in the bottom right corner, the
// rest of it under the screen's edge, and paddles along the bottom to the
// middle. There it looks at whoever is watching and goes under: below the
// screen it has got into a lift. The lift is the mark's box. Its doors shut,
// it comes up between its rails to the middle of the screen, its bell rings,
// the doors open, and there is the mark as the console's loading picture
// shows it (sce_sys/pic1.dds): the swan on its water in the box, the name
// under it. Then the swan opens its wings, leaves the box and flies to the box
// in the library's header, while the library comes up underneath.
//
// With the animations limited the splash is the finished picture, which only
// fades into the library.
struct Splash
{
	enum { NotBegun, Showing, Leaving, Over } state = NotBegun;
	double began = 0, leaving = 0;
	int frames = 0;
} splash;

float smooth(float x)
{
	x = std::clamp(x, 0.f, 1.f);
	return x * x * (3.f - 2.f * x);
}

// Sets off gently and arrives gently: how the swan crosses, how the lift travels.
float glide(float x)
{
	x = std::clamp(x, 0.f, 1.f);
	return x * x * x * (x * (x * 6.f - 15.f) + 10.f);
}

// Water thrown up from the bottom edge, `since` seconds ago, at `x` units.
void spray(float since, float x)
{
	if (since < 0 || since > 0.62f)
		return;
	const float H = unitsHigh();
	for (int i = 0; i < 9; i++)
	{
		const float side = ((float)i - 4.f) * 0.25f;						// -1 .. 1
		const float speed = 430.f + 70.f * (float)((i * 37) % 5);			// units a second, upwards
		const float ux = x + side * 210.f * since + side * 26.f;
		const float uy = H + 6.f - (speed * since - 1150.f * since * since);
		if (uy > H + 4.f)
			continue;
		draw()->AddCircleFilled(at(ux, uy), px(3.f + (float)(i % 3) * 1.6f),
				IM_COL32(196, 224, 255, (int)(190 * (1.f - since / 0.62f))), 12);
	}
}

// One of the lift's two doors over the box `a`..`b`: `left` or the right
// one, `open` 0 shut to 1 away in the wall.
void liftDoor(ImVec2 a, ImVec2 b, bool left, float open)
{
	const float side = b.x - a.x, half = side * 0.5f;
	const float w = half * (1.f - open);
	if (w < 1.f)
		return;
	// Its last sliver goes out instead of being drawn square in a round corner.
	const float alpha = 1.f - smooth((open - 0.58f) / 0.36f);
	const float radius = side * 0.19f;
	const float x0 = left ? a.x : b.x - w, x1 = left ? a.x + w : b.x;
	ImDrawList *list = draw();
	list->AddRectFilled(ImVec2(x0, a.y), ImVec2(x1, b.y), withAlpha(IM_COL32(30, 44, 84, 255), alpha), radius,
			left ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersRight);
	// What is on the door slides with it, into the wall.
	const float shift = (left ? -1.f : 1.f) * half * open;
	const float mid = (a.x + b.x) * 0.5f;
	// (Not as far as the box's round corners: a frame's width short of them.)
	const float frame = side * 0.045f;
	list->PushClipRect(ImVec2(left ? a.x + frame : x0, a.y), ImVec2(left ? x1 : b.x - frame, b.y), true);
	const float inset = side * 0.085f;
	const ImVec2 pa(left ? a.x + inset + shift : mid + inset * 0.6f + shift, a.y + inset);
	const ImVec2 pb(left ? mid - inset * 0.6f + shift : b.x - inset + shift, b.y - inset);
	list->AddRectFilled(pa, pb, withAlpha(IM_COL32(40, 58, 108, 255), alpha), side * 0.035f);
	list->AddRect(pa, pb, withAlpha(IM_COL32(120, 160, 230, 70), alpha), side * 0.035f, std::max(side * 0.006f, 1.f));
	// Light along the edge that meets the other door.
	const float edge = left ? x1 : x0;
	const float glint = side * 0.05f;
	const ImU32 lit = withAlpha(IM_COL32(150, 190, 255, 60), alpha), none = IM_COL32(150, 190, 255, 0);
	if (left)
		list->AddRectFilledMultiColor(ImVec2(edge - glint, a.y), ImVec2(edge, b.y), none, lit, lit, none);
	else
		list->AddRectFilledMultiColor(ImVec2(edge, a.y), ImVec2(edge + glint, b.y), lit, none, none, lit);
	list->AddLine(ImVec2(edge, a.y), ImVec2(edge, b.y), withAlpha(IM_COL32(6, 9, 20, 230), alpha), std::max(side * 0.008f, 1.f));
	list->PopClipRect();
}

// `t`: seconds shown; `flight`: 0 at rest, 1 landed in the header.
void drawSplash(float t, float flight)
{
	namespace st = splashtime;
	const Theme& th = theme();
	const float W = unitsWide(), H = unitsHigh();
	const bool flies = motion() == MotionFull;
	// Where the story is: at its end at once when little is to move.
	const float story = flies ? t : st::End;
	const float rest = 1.f - std::clamp(flight / 0.35f, 0.f, 1.f);		// what leaves first
	const float cover = 1.f - flight;									// the splash's own backdrop
	// Its backdrop, over whatever is behind.
	draw()->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(width(), height()), withAlpha(IM_COL32(22, 34, 66, 255), cover),
			withAlpha(IM_COL32(22, 34, 66, 255), cover), withAlpha(IM_COL32(8, 11, 24, 255), cover),
			withAlpha(IM_COL32(8, 11, 24, 255), cover));
	ImDrawList *list = draw();
	// The water: three lines of small waves.
	static const float rows[3] = { 0.800f, 0.850f, 0.900f };
	static const float strength[3] = { 0.27f, 0.19f, 0.12f };
	for (int row = 0; row < 3; row++)
	{
		const float y = height() * rows[row];
		for (float x = 0; x <= width(); x += px(8))
			list->PathLineTo(ImVec2(x, y + std::sin(x / width() * 3.14159f * 9.f + t * (1.3f + 0.3f * (float)row))
					* height() * 0.006f));
		list->PathStroke(IM_COL32(150, 200, 255, (int)(255 * strength[row] * rest)), std::max(height() / 360.f, 2.f));
	}

	// Where the box ends up: the middle of the screen.
	const float side0 = H * 0.36f, x0 = (W - side0) * 0.5f, y0 = H * 0.22f;

	// The lift's rails, from the bottom of the screen to where it stops, and
	// the lamp over its door. They come when the swan has gone under and go
	// when the doors are open.
	const float railAlpha = smooth((story - st::Gone) / 0.25f) * (1.f - smooth((story - st::DoorsOpen - 0.1f) / 0.7f)) * rest;
	if (railAlpha > 0.01f)
	{
		const float thick = std::max(px(3), 1.f);
		for (int side = 0; side < 2; side++)
		{
			const float x = side == 0 ? x0 - 22 : x0 + side0 + 22;
			list->AddRectFilledMultiColor(ImVec2(px(x) - thick * 0.5f, px(y0 - 70)), ImVec2(px(x) + thick * 0.5f, height()),
					IM_COL32(150, 200, 255, 0), IM_COL32(150, 200, 255, 0), IM_COL32(150, 200, 255, (int)(110 * railAlpha)),
					IM_COL32(150, 200, 255, (int)(110 * railAlpha)));
			// The brackets that hold them, a floor apart.
			for (float y = H - 40; y > y0 - 40; y -= 96)
			{
				const float fade = std::clamp((y - (y0 - 70)) / 220.f, 0.f, 1.f);
				list->AddLine(at(x - 9, y), at(x + 9, y), IM_COL32(150, 200, 255, (int)(120 * railAlpha * fade)), thick);
			}
		}
		const bool arrived = story >= st::Ding;
		const ImVec2 lamp = at(W * 0.5f, y0 - 40);
		if (arrived)
			list->AddCircleFilled(lamp, px(22), withAlpha(th.accent, 0.22f * railAlpha), 32);
		list->AddCircleFilled(lamp, px(8), arrived ? withAlpha(th.accent, railAlpha) : IM_COL32(120, 140, 180, (int)(120 * railAlpha)), 24);
	}

	// The box: it comes up from under the screen, and stays where it stops
	// until the swan has left it.
	const float below = (1.f - glide((story - st::LiftStart) / (st::LiftStop - st::LiftStart))) * (H - y0 + 30);
	const ImVec2 boxA = at(x0, y0 + below), boxB = at(x0 + side0, y0 + side0 + below);
	const float boxAlpha = flies ? 1.f - smooth((flight - 0.10f) / 0.45f) : cover;
	const ImVec2 centre((boxA.x + boxB.x) * 0.5f, (boxA.y + boxB.y) * 0.5f);
	// A light behind it, breathing, once it has arrived.
	const float lit = smooth((story - st::Ding) / 0.6f);
	const float breath = 0.5f + 0.5f * std::sin(t * 2.1f);
	for (int ring = 0; ring < 3; ring++)
		list->AddCircleFilled(centre, px(side0 * (0.62f + 0.07f * (float)ring + 0.03f * breath)),
				withAlpha(th.accent, (0.10f - 0.028f * (float)ring) * rest * lit), 96);
	// The bell: a ring of light that widens and goes.
	if (story >= st::Ding && story < st::Ding + 0.8f)
	{
		const float u = (story - st::Ding) / 0.8f;
		list->AddCircle(centre, px(side0 * (0.56f + 0.50f * u)), withAlpha(th.accent, 0.55f * (1.f - u) * (1.f - u)), 96,
				px(5.f * (1.f - u) + 1.f));
	}
	if (below < H)
		logoBox(boxA, boxB, boxAlpha);

	// The swan.
	ImVec2 from, to, headerA, headerB;
	float fromSize = 0, toSize = 0;
	swanPlace(boxA, boxB, from, fromSize);
	headerBox(headerA, headerB);
	swanPlace(headerA, headerB, to, toSize);
	if (!flies)
		swan(from, fromSize, SwanPose(), boxAlpha);
	else if (flight <= 0)
	{
		if (story < st::Gone)
		{
			// Its head and neck over the bottom edge, the rest of it below
			// (the tail too, which stands higher than the back).
			const float size = px(H * 0.62f);
			const float up = smooth((story - st::Rise) / (st::Swim - st::Rise));
			const float down = smooth((story - st::Dive) / (st::Gone - st::Dive));
			const float go = glide((story - st::Swim) / (st::Arrive - st::Swim));
			const float headX = W * (0.90f - 0.40f * go);						// units
			const float paddling = std::sin(3.14159f * std::clamp((story - st::Swim) / (st::Arrive - st::Swim), 0.f, 1.f));
			SwanPose pose;
			// It looks at whoever is watching, then puts its head down.
			const float glance = smooth((story - st::Arrive + 0.10f) / 0.20f) * (1.f - smooth((story - st::Dive + 0.04f) / 0.14f));
			pose.look = 0.58f * glance - down;
			pose.tilt = 0.030f * std::sin(story * 8.5f + 1.f) * paddling;
			const float bob = std::sin(story * 8.5f) * 0.012f * size * paddling;
			swan(ImVec2(px(headX) - 0.335f * size, height() - size * (0.165f + 0.240f * up * (1.f - down)) + bob), size, pose);
			spray(story - st::Rise - 0.03f, W * 0.90f);
			spray(story - st::Dive - 0.14f, W * 0.50f);
		}
		else
		{
			// In its box. When the doors have opened it looks behind it once,
			// then ahead again, before it leaves.
			SwanPose pose;
			pose.look = smooth((story - st::DoorsDone - 0.05f) / 0.30f) * (1.f - smooth((story - st::End + 0.40f) / 0.30f));
			pose.tilt = 0.018f * std::sin(t * 1.3f);
			if (below < H)
				swan(from, fromSize, pose);
		}
	}
	else
	{
		// Up out of the box and over to the header's: an arc, by the middle of
		// the bird, which gets smaller as it goes.
		const float e = smooth(flight);
		const float size = fromSize + (toSize - fromSize) * e;
		const ImVec2 c0(from.x + fromSize * 0.5f, from.y + fromSize * 0.5f);
		const ImVec2 c1(to.x + toSize * 0.5f, to.y + toSize * 0.5f);
		const ImVec2 ctrl(c0.x * 0.72f + c1.x * 0.28f, c1.y + height() * 0.06f);
		const float u = 1.f - e;
		const ImVec2 c(u * u * c0.x + 2 * u * e * ctrl.x + e * e * c1.x, u * u * c0.y + 2 * u * e * ctrl.y + e * e * c1.y);
		SwanPose pose;
		pose.fly = smooth(flight / 0.14f) * (1.f - smooth((flight - 0.86f) / 0.14f));
		pose.beat = t * 6.2832f * 3.0f;
		pose.tilt = 0.30f * std::sin(3.14159f * std::min(flight * 1.25f, 1.f)) * pose.fly;
		swan(ImVec2(c.x - size * 0.5f, c.y - size * 0.5f), size, pose);
	}

	// The lift's doors, shut on the way up.
	const float doorsOpen = smooth((story - st::DoorsOpen) / (st::DoorsDone - st::DoorsOpen));
	if (flies && doorsOpen < 1.f && below < H)
	{
		liftDoor(boxA, boxB, true, doorsOpen);
		liftDoor(boxA, boxB, false, doorsOpen);
	}

	// The name: under the mark, once the doors are open; it goes out on the
	// swan's way, and the header's own comes in where the swan lands.
	const float named = smooth((story - st::DoorsOpen - 0.15f) / 0.50f);
	const float ease = smooth(flight);
	const float size0 = H * 0.085f;
	const float size = size0 + (44 - size0) * ease;
	const float nameW0 = toUnits(measure(AppName, Huge, size0).x);
	const float nameY0 = y0 + side0 + H * 0.035f + 18.f * (1.f - named);
	const float nx = (W - nameW0) * 0.5f + (136 - (W - nameW0) * 0.5f) * ease;
	const float ny = nameY0 + (36 - nameY0) * ease;
	wordmark(at(flies ? nx : (W - nameW0) * 0.5f, flies ? ny : nameY0),
			(flies ? 1.f - std::clamp(flight / 0.55f, 0.f, 1.f) : cover) * named, Huge, flies ? size : size0);
	// What leaves when the flight begins.
	if (rest > 0.01f)
	{
		const float lineY = y0 + side0 + H * 0.145f;
		textCentred(at(W * 0.5f, lineY), withAlpha(th.dim, rest * smooth((story - st::DoorsOpen - 0.40f) / 0.50f)), "for PS5",
				Body, H * 0.030f);
		// Which build this is, in its corner, as in the library.
		buildTag(std::clamp((t - 0.4f) / 0.6f, 0.f, 1.f) * rest);
	}
}

// True while the splash has the screen to itself.
bool runSplash()
{
	namespace st = splashtime;
	if (splash.state == Splash::NotBegun)
		splash.state = options::frontend().splash && motion() != MotionOff && !host::running()
				? Splash::Showing : Splash::Over;
	if (splash.state == Splash::Over)
		return false;
	const bool flies = motion() == MotionFull;
	const double time = clock();
	if (splash.state == Splash::Showing)
	{
		// The first frames wait for the display: the animation's clock, and
		// its sound, start when they are on the screen.
		if (splash.frames < 3)
			splash.began = time;
		else if (splash.frames == 3)
			sound::play(flies ? sound::Splash : sound::Chime);
		splash.frames++;
		const float t = (float)(time - splash.began);
		backdrop();
		drawSplash(t, 0);
		const bool skipped = in.pressed != 0 && t > 0.3f;
		if (t > (flies ? st::End : 1.2f) || skipped)
		{
			if (skipped && flies && t < st::End)
			{
				// Skipped: straight to the end of the story, and its sound stops.
				splash.began = time - st::End;
				sound::stop();
			}
			if (flies)
				sound::play(sound::Flight);
			splash.state = Splash::Leaving;
			splash.leaving = time;
			consumeInput();
		}
		return true;
	}
	const float u = std::clamp((float)((time - splash.leaving) / (flies ? st::Flight : 0.4f)), 0.f, 1.f);
	// The library comes up underneath; its header's box waits for the swan.
	headerSwanAway = flies;
	headerBoxAlpha = flies ? smooth((u - 0.30f) / 0.35f) : u;
	headerNameAlpha = std::clamp((u - 0.5f) / 0.5f, 0.f, 1.f);
	libraryPage(false);
	drawSplash((float)(time - splash.began), u);
	if (u >= 1.f)
	{
		splash.state = Splash::Over;
		headerSwanAway = false;
		headerBoxAlpha = headerNameAlpha = 1;
	}
	return true;
}

// ------------------------------------------------------- a game is starting

// The swan leaves its box in the header, turns round and flies at the
// screen, which goes dark behind it; then the game starts. Only with the
// animations full; otherwise a game starts at once.
void launchPage(Frame& f)
{
	const float u = std::clamp((float)((clock() - f.opened) / 1.05), 0.f, 1.f);
	const float t = (float)(clock() - f.opened);
	headerSwanAway = true;
	ImVec2 headerA, headerB, from;
	float fromSize = 0;
	headerBox(headerA, headerB);
	swanPlace(headerA, headerB, from, fromSize);

	SwanPose pose;
	// It turns round first (the screen's middle is to its right), then goes.
	const float turned = smooth(u / 0.16f);
	pose.face = 1.f - 2.f * turned;
	const float go = smooth((u - 0.10f) / 0.90f);
	pose.fly = smooth((u - 0.06f) / 0.14f);
	pose.beat = t * 6.2832f * 3.4f;
	pose.tilt = -0.22f * pose.fly * (1.f - go);
	// Larger and larger, towards the middle of the screen: at the viewer.
	const float grow = go * go * go;
	const float size = fromSize * std::pow(height() * 4.2f / fromSize, grow);
	const ImVec2 c0(from.x + fromSize * 0.5f, from.y + fromSize * 0.5f);
	const ImVec2 c1(width() * 0.50f, height() * 0.47f);
	const float along = smooth(go * 1.15f);
	const ImVec2 c(c0.x + (c1.x - c0.x) * along, c0.y + (c1.y - c0.y) * along - std::sin(along * 3.14159f) * height() * 0.05f);
	swan(ImVec2(c.x - size * 0.5f, c.y - size * 0.5f), size, pose);
	// The dark comes over it at the end.
	const float dark = smooth((u - 0.62f) / 0.38f);
	if (dark > 0)
		draw()->AddRectFilled(ImVec2(0, 0), ImVec2(width(), height()), IM_COL32(0, 0, 0, (int)(255 * dark)));
	consumeInput();
	if (u >= 1.f)
	{
		const int slot = f.a, disc = f.b;
		deferred = [slot, disc] {
			pop();
			headerSwanAway = false;
			launchNow(chosen, slot, disc);
		};
	}
}

void init()
{
	widgetsInit();
	wantedSource = std::clamp(options::frontend().source, 0, (int)library::SourceCount - 1);
	source = wantedSource;
}

bool quitRequested()
{
	return quit;
}

bool blocksEmulation()
{
	return !stack.empty();
}

// The menus' sounds, by what was pressed while a menu has the pad: not in a
// game, and not while an animation has the screen.
void menuSounds()
{
	if (in.pressed == 0 && in.repeat == 0)
		return;
	if (stack.empty() ? (host::running() || splash.state != Splash::Over)
			: (stack.back().page == Page::Launch || stack.back().page == Page::Loading))
		return;
	// The search's keyboard has its own sound for a letter.
	const bool typing = !stack.empty() && ((stack.back().page == Page::Search && !searchState.inResults)
			|| stack.back().page == Page::Text);
	if (hit(confirmButton))
	{
		if (!typing)
			sound::play(sound::Select);
	}
	else if (hit(cancelButton))
		sound::play(sound::Back);
	else if (hit(L1 | R1 | Options) || (!typing && hit(Triangle | TouchLeft | TouchRight)))
		sound::play(sound::Tab);
	else if (nav(Up | Down | Left | Right | L2 | R2))
		sound::play(sound::Move);
}

#if defined(SWANSTATION_HOST)
// A test of frame generation with pictures of known movement: the files
// f0.png, f1.png... of the folder SWANSTATION_FG_TEST names are a game's
// pictures, each shown for SWANSTATION_FG_STEPS refreshes (2 when not said),
// and the screen shows what frame generation makes of them, top left, pixel
// for pixel. Every refresh is saved as a picture, and the log says which
// showed which step.
bool generationTest()
{
	const char *folder = getenv("SWANSTATION_FG_TEST");
	if (folder == nullptr)
		return false;
	static std::vector<Image> pictures;
	static bool ready;
	static uint64_t started;
	if (!ready)
	{
		pictures.clear();
		ready = true;
		for (int i = 0; i < 64; i++)
		{
			const std::string file = format("%s/f%d.png", folder, i);
			if (!fileExists(file))
				break;
			pictures.push_back(image(file));
			if (pictures.back().id == nullptr)
				ready = false;		// still being read
		}
		started = display::frameCount();
		return true;
	}
	const int steps = getenv("SWANSTATION_FG_STEPS") != nullptr ? std::clamp(atoi(getenv("SWANSTATION_FG_STEPS")), 1, 8) : 2;
	const uint64_t tick = display::frameCount() - started;
	const size_t index = (size_t)(tick / (uint64_t)steps);
	// What the refresh before showed, kept as g<its number>.png.
	if (tick >= 1 && index <= pictures.size())
		display::saveScreenshot(format("%sg%03d.png", rootDir.c_str(), (int)tick - 1));
	if (pictures.empty() || index >= pictures.size())
	{
		quit = true;
		return true;
	}
	const int step = (int)(tick % (uint64_t)steps);
	const float phase = (float)(step + 1) / (float)steps;
	const Image& picture = pictures[index];
	const char *quality = getenv("SWANSTATION_FG_QUALITY");
	void *shown = display::generated(picture.id, picture.width, picture.height, 1.f, 1.f, step == 0,
			getenv("SWANSTATION_FG_AHEAD") != nullptr ? phase + (step == steps - 1 ? 0.f : 1.f) : phase,
			getenv("SWANSTATION_FG_LIGHTER") != nullptr ? 0 : quality != nullptr ? atoi(quality) : 1,
			getenv("SWANSTATION_FG_DEBUG") != nullptr);
	ImDrawList *list = ImGui::GetBackgroundDrawList();
	list->AddRectFilled(ImVec2(0, 0), ImVec2(width(), height()), IM_COL32(0, 0, 0, 255));
	display::sampling(list, true);
	list->AddImage((ImTextureID)(shown != nullptr ? shown : picture.id), ImVec2(0, 0),
			ImVec2((float)picture.width, (float)picture.height));
	display::sampling(list, false);
	diag::mark("generation test: display frame %llu shows picture %d at phase %.2f%s", (unsigned long long)display::frameCount(),
			(int)index, phase, shown == nullptr ? " (not made)" : "");
	return true;
}

// A test of the picture's looks: the picture SWANSTATION_LOOK_TEST names is
// drawn as a game's would be (4:3, fitted to the screen), through each look in
// turn, and each is saved as look-<name>.png.
bool lookTest()
{
	const char *file = getenv("SWANSTATION_LOOK_TEST");
	if (file == nullptr)
		return false;
	static Image picture;
	static bool ready;
	static uint64_t started;
	if (!ready)
	{
		picture = image(file);
		ready = picture.id != nullptr;
		started = display::frameCount();
		return true;
	}
	struct Case
	{
		const char *name;
		int scaler, sharpness, signal, crt;
		float brightness, contrast, saturation, gamma;
	};
	static const Case cases[] = {
		{ "plain", 0, 1, 0, 0, 1, 1, 1, 1 },
		{ "sharp-bilinear", 1, 1, 0, 0, 1, 1, 1, 1 },
		{ "fsr", 2, 1, 0, 0, 1, 1, 1, 1 },
		{ "nis", 3, 1, 0, 0, 1, 1, 1, 1 },
		{ "cas", 4, 2, 0, 0, 1, 1, 1, 1 },
		{ "undither", 1, 1, 1, 0, 1, 1, 1, 1 },
		{ "svideo", 1, 1, 2, 0, 1, 1, 1, 1 },
		{ "composite", 1, 1, 3, 0, 1, 1, 1, 1 },
		{ "colours", 1, 1, 0, 0, 1.1f, 1.2f, 1.3f, 1.2f },
		{ "crt-guest", 0, 1, 0, 1, 1, 1, 1, 1 },
		{ "crt-home", 0, 1, 0, 2, 1, 1, 1, 1 },
		{ "crt-studio", 0, 1, 0, 3, 1, 1, 1, 1 },
		{ "crt-arcade", 0, 1, 0, 4, 1, 1, 1, 1 },
		{ "crt-soft", 0, 1, 0, 5, 1, 1, 1, 1 },
		{ "crt-composite", 0, 1, 3, 2, 1, 1, 1, 1 },
	};
	// Each look for eight refreshes (the tube's afterglow and average settle);
	// the last of them is saved.
	const uint64_t tick = display::frameCount() - started;
	const size_t index = (size_t)(tick / 8);
	if (tick >= 1 && (tick - 1) % 8 == 7 && (tick - 1) / 8 < std::size(cases))
		display::saveScreenshot(format("%slook-%s.png", rootDir.c_str(), cases[(tick - 1) / 8].name));
	if (index >= std::size(cases))
	{
		quit = true;
		return true;
	}
	const Case& c = cases[index];
	display::Look look;
	look.scaler = c.scaler;
	look.sharpness = c.sharpness;
	look.signal = c.signal;
	look.crt = c.crt;
	look.brightness = c.brightness;
	look.contrast = c.contrast;
	look.saturation = c.saturation;
	look.gamma = c.gamma;
	const float H = height(), W = width();
	const float dh = H, dw = std::min(W, dh * 4.f / 3.f);
	const ImVec2 p0(std::floor((W - dw) * 0.5f), 0.f), p1(p0.x + dw, dh);
	ImDrawList *list = ImGui::GetBackgroundDrawList();
	list->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 255));
	bool full = false;
	const int outW = (int)std::lround(dw), outH = (int)std::lround(dh);
	void *looked = display::picture(picture.id, picture.width, picture.height, 1.f, 1.f, outW, outH, look, full);
	display::sampling(list, full);
	if (looked != nullptr && full)
		list->AddImage((ImTextureID)looked, p0, ImVec2(p0.x + (float)outW, p0.y + (float)outH));
	else
		list->AddImage((ImTextureID)(looked != nullptr ? looked : picture.id), p0, p1);
	display::sampling(list, false);
	return true;
}
#endif

void frame()
{
	widgetsFrame();
	imagesFrame();
#if defined(SWANSTATION_HOST)
	if (generationTest() || lookTest())
		return;
#endif
	readInput();
	menuSounds();
	if (closeGame)
	{
		closeGame = false;
		host::stop();
		stack.clear();
		consumeInput();
	}

	const bool game = host::running();
	// L1 and R1 held as the title starts: the safe start page, before anything else.
	static bool safeAsked;
	if (!safeAsked && display::frameCount() < 90 && !game && stack.empty()
			&& (in.held & (L1 | R1)) == (L1 | R1))
	{
		safeAsked = true;
		splash.state = Splash::Over;
		// A display output the screen cannot show leaves this page unseen: that
		// one is put back without being asked for, for the next start.
		options::Frontend& o = options::frontend();
		if (o.displayMode != 0)
		{
			o.displayMode = 0;
			o.blackFrames = false;
			options::saveFrontend();
			storage::syncDisplayMode(appDir + "sce_sys/param.json", 0);
			diag::mark("safe start: the display output is 60 Hz again from the next start");
			push(Page::SafeStart, 1);
		}
		else
			push(Page::SafeStart);
		consumeInput();
	}
	// The menus' music plays while a menu or the library has the screen.
	sound::music(!game && splash.state == Splash::Over);

	if (!game && stack.empty() && runSplash())
	{
		// The animation has the screen.
	}
	else if (!game && idleSwan(stack.empty() || (stack.back().page != Page::Loading && stack.back().page != Page::Launch
			&& stack.back().page != Page::Update)))
	{
		// Nothing was pressed for a while: the swan has the screen.
	}
	else if (!game && stack.empty())
	{
		libraryPage(true);
		startNotices();
	}
	else if (game && stack.empty())
	{
		drawGame(0);
		gameMarks();
		drawGameOverlay();
		if (gameShortcuts())
		{
			push(Page::Pause);
			consumeInput();
		}
	}
	else
	{
		// What is behind the page: the game, dimmed, or the library's backdrop.
		Frame& f = stack.back();
		const Page page = f.page;
		if (game)
			drawGame(0.72f);
		else if (page == Page::Details || page == Page::Launch || page == Page::Loading || page == Page::LibraryOptions
				|| page == Page::Cover)
		{
			libraryBehind = true;
			libraryPage(false);
			libraryBehind = false;
		}
		else
			backdrop();
		switch (page)
		{
		case Page::MainMenu: mainMenuPage(f); break;
		case Page::Settings: settingsPage(f); break;
		case Page::Pause: pausePage(f); break;
		case Page::States: statesPage(f); break;
		case Page::Discs: discsPage(f); break;
		case Page::Cheats: cheatsPage(f); break;
		case Page::Details: detailsPage(f); break;
		case Page::Launch: launchPage(f); break;
		case Page::Loading: loadingPage(f); break;
		case Page::Search: searchPage(f); break;
		case Page::Message: messagePage(f, false); break;
		case Page::Confirm: messagePage(f, true); break;
		case Page::LibraryOptions: libraryOptionsPage(f); break;
		case Page::Cover: coverPage(f); break;
		case Page::Cards: cardsPage(f); break;
		case Page::Card: cardPage(f); break;
		case Page::CardPick: cardPickPage(f); break;
		case Page::Buttons: buttonsPage(f); break;
		case Page::Achievements: achievementsPage(f); break;
		case Page::Account: accountPage(f); break;
		case Page::Netplay: netplayPage(f); break;
		case Page::Text: textPage(f); break;
		case Page::Update: updatePage(f); break;
		case Page::SafeStart: safeStartPage(f); break;
		case Page::Shortcuts: shortcutsPage(f); break;
		case Page::Memory: memoryPage(f); break;
		case Page::Speedrun: speedrunPage(f); break;
		case Page::Web: webPage(f); break;
		case Page::Shaders: shadersPage(f); break;
		}
		if (game)
			drawMessages();
		// Circle goes back; OPTIONS, over a game, returns to it at once. Pages
		// that have a use of their own for Circle, or that must be answered,
		// see to it themselves.
		if (!deferred && page != Page::Loading && page != Page::Launch && page != Page::Text && page != Page::Update
				&& page != Page::Netplay)
		{
			if (hit(cancelButton) && page != Page::Details)
				pop();
			else if (game && hit(Options))
			{
				stack.clear();
				// The same press is not the next menu's.
				optionsSpent = true;
			}
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
