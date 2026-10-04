/*
	SwanStation for PS5 - the interface's own declarations (ui.cpp,
	ui_widgets.cpp, texture.cpp).

	SPDX-License-Identifier: GPL-3.0-or-later

	The interface is drawn with Dear ImGui's draw lists and fonts; its own code
	does the layout and the controller navigation, so every screen behaves the
	same way under a DualSense: the d-pad or the left stick moves, Cross
	confirms, Circle goes back.
*/
#pragma once

#include "fe.h"

#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

namespace fe::ui
{

// ---------------------------------------------------------------- texture.cpp

struct Image
{
	void *id = nullptr;
	int width = 0, height = 0;
	// The same picture a few pixels across and blurred: stretched over the
	// screen it is only its colours. Null for the logo.
	void *soft = nullptr;
};
// The picture in a file. Decoded on a thread of its own: empty until it is
// ready, and for a file that is no picture.
Image image(const std::string& path);
// From memory, decoded at once (the logo).
Image imageFromMemory(const uint8_t *data, size_t size);
// Once a frame: uploads what was decoded, lets go of pictures long unused.
void imagesFrame();

// ------------------------------------------------------------- ui_widgets.cpp

// Icons of the symbol font, as UTF-8.
namespace icon
{
constexpr const char *Gamepad = "\xef\x84\x9b";		// f11b
constexpr const char *Drive = "\xef\x82\xa0";		// f0a0
constexpr const char *Network = "\xef\x9b\xbf";		// f6ff
constexpr const char *Plug = "\xef\x87\xa6";		// f1e6
constexpr const char *Gear = "\xef\x80\x93";		// f013
constexpr const char *Play = "\xef\x81\x8b";		// f04b
constexpr const char *Save = "\xef\x83\x87";		// f0c7
constexpr const char *Folder = "\xef\x81\xbc";		// f07c
constexpr const char *Disc = "\xef\x94\x9f";		// f51f
constexpr const char *Undo = "\xef\x83\xa2";		// f0e2
constexpr const char *Power = "\xef\x80\x91";		// f011
constexpr const char *Info = "\xef\x81\x9a";		// f05a
constexpr const char *Magic = "\xef\x83\x90";		// f0d0
constexpr const char *List = "\xef\x80\xba";		// f03a
constexpr const char *Grid = "\xef\x80\x8a";		// f00a
constexpr const char *Download = "\xef\x80\x99";	// f019
constexpr const char *Sliders = "\xef\x87\x9e";		// f1de
constexpr const char *Screen = "\xef\x89\xac";		// f26c
constexpr const char *Volume = "\xef\x80\xa8";		// f028
constexpr const char *Chip = "\xef\x8b\x9b";		// f2db
constexpr const char *Wrench = "\xef\x82\xad";		// f0ad
constexpr const char *Check = "\xef\x80\x8c";		// f00c
constexpr const char *Cross = "\xef\x80\x8d";		// f00d
constexpr const char *Warning = "\xef\x81\xb1";		// f071
constexpr const char *Upload = "\xef\x82\x93";		// f093
constexpr const char *Clock = "\xef\x80\x97";		// f017
constexpr const char *Paint = "\xef\x94\xbf";		// f53f
constexpr const char *Memory = "\xef\x94\xb8";		// f538
constexpr const char *Chevron = "\xef\x81\x94";		// f054
constexpr const char *Picture = "\xef\x80\xbe";		// f03e
constexpr const char *Card = "\xef\x9f\x82";		// f7c2
constexpr const char *Server = "\xef\x88\xb3";		// f233
constexpr const char *Sync = "\xef\x80\xa1";		// f021
constexpr const char *Bolt = "\xef\x83\xa7";		// f0e7
}

enum Font { Body, Bold, Title, Huge };

struct Theme
{
	ImU32 background, backgroundLow;	// the screen behind everything
	ImU32 panel, panelHigh;				// a panel, a row under the cursor
	ImU32 accent, accentSoft;
	ImU32 text, dim, faint;
	ImU32 good, bad;
};
const Theme& theme();
constexpr int AccentCount = 6;
const char *accentName(int index);

// Loads the fonts (once, before the first frame) and the logo.
void widgetsInit();
// Sets the frame's scale and colours.
void widgetsFrame();

// 1080-line units to pixels (the display's scale times the interface's).
float px(float units);
ImDrawList *draw();
float width();
float height();
// Seconds since the title started, for animations.
double clock();
// Eases towards a target, frame-rate independent.
float approach(float value, float target, float speed);

ImVec2 measure(const std::string& text, Font font = Body, float size = 26);
void text(ImVec2 at, ImU32 colour, const std::string& text, Font font = Body, float size = 26);
// Cut to `maxWidth` with an ellipsis; returns the width drawn.
float textFit(ImVec2 at, float maxWidth, ImU32 colour, const std::string& text, Font font = Body, float size = 26);
void textRight(ImVec2 at, ImU32 colour, const std::string& text, Font font = Body, float size = 26);
void textCentred(ImVec2 at, ImU32 colour, const std::string& text, Font font = Body, float size = 26);
// Wrapped to `wrapWidth`, at most `maxHeight` high (0: any); returns its height.
float textWrapped(ImVec2 at, float wrapWidth, ImU32 colour, const std::string& text, Font font = Body,
		float size = 26, float maxHeight = 0);

// How high it would be, in pixels.
float wrappedHeight(const std::string& text, float wrapWidth, Font font = Body, float size = 26);

void panel(ImVec2 a, ImVec2 b, ImU32 colour, float rounding = 14);
void outline(ImVec2 a, ImVec2 b, ImU32 colour, float rounding = 14, float thickness = 3);
ImU32 withAlpha(ImU32 colour, float alpha);
ImU32 mix(ImU32 a, ImU32 b, float t);

// The screen's background when no game is behind the interface.
void backdrop();
// A picture spread over the whole screen behind everything, faintly: the
// focused game's cover as the library's light.
void wash(const Image& image, float alpha);
// Lines of small waves across the screen (the splash's water).
void waves(float alpha);
// The swan (swan.cpp), drawn from shapes so that it can move.
struct SwanPose
{
	float look = 0;		// 0 ahead, as in the mark; towards 1 it looks behind; below 0 it dips its head
	float fly = 0;		// 0 on the water, 1 in the air: neck out, wings open
	float beat = 0;		// where the wings are in their beat, in radians
	float tilt = 0;		// turned about its body, in radians
	float face = 1;		// 1 facing left, as in the mark; -1 right; between, it is turning round
};
// `origin` is the top left of the bird's square, `size` its side, in pixels.
void swan(ImVec2 origin, float size, const SwanPose& pose, float alpha = 1.f, ImDrawList *list = nullptr);
// The pose of a swan sitting in its box at `time`: it looks about.
SwanPose swanIdle(double time);
// The mark's box (water, light, rounded corners), without the bird.
void logoBox(ImVec2 a, ImVec2 b, float alpha = 1.f);
// Where the bird sits in a box: its square's origin and side.
void swanPlace(ImVec2 boxA, ImVec2 boxB, ImVec2& origin, float& size);
// How much moves (Settings, Interface): everything, little, or nothing.
enum Motion { MotionFull, MotionReduced, MotionOff };
Motion motion();
// A rounded label: filled when `on`.
float pill(ImVec2 at, const std::string& label, bool on, bool enabled = true, float height = 60, float size = 24);
// A soft light.
void glowAt(ImVec2 centre, float radius, ImU32 colour);
// A controller button's symbol, `size` units across, centred on `centre`.
void buttonGlyph(ImVec2 centre, float size, uint32_t button);
// "(x) Start" hints, right-aligned on the bottom bar.
struct Hint
{
	uint32_t button;
	std::string label;
};
void hintBar(const std::vector<Hint>& hints, const std::string& left = "");
void progressBar(ImVec2 a, ImVec2 b, float fraction);
// A cover's placeholder: the game's name on a tinted card.
void coverPlaceholder(ImVec2 a, ImVec2 b, const std::string& name, const std::string& region);
// A picture fitted into a box (letterboxed), with rounded corners.
void imageFit(const Image& image, ImVec2 a, ImVec2 b, float rounding = 10, ImU32 tint = IM_COL32_WHITE);

}
