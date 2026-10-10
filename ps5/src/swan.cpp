/*
	PSSwanStation - the swan, drawn so that it can move.

	SPDX-License-Identifier: GPL-3.0-or-later

	The title's mark is a swan on water in a rounded box (ps5/tools/make-art.py
	draws the icon and the loading picture). In the interface the box is a
	picture and the bird is drawn here, from the same shapes, each frame: a
	body, a tail, a neck that is a curve, a head with a beak and an eye, and
	wings. So it can do what a picture cannot: turn its head to look behind it,
	dip it, preen, tuck it in to sleep, open its wings, stretch its neck and
	fly.

	Everything is in the mark's own coordinates: a unit square whose (0, 0) is
	the bird's top left corner, as make-art.py's swan() has it. Two poses are
	kept as numbers, swimming (the mark exactly) and flying, and a pose in
	between is every number in between; the other moves bend the neck from
	there.

	It dresses for the theme: an inked outline where the theme draws with ink
	(Brutal, Sketch, Hazard, Contrast, and a thin one on light pages, where a
	white bird would vanish), a wireframe in Blueprint, pastel feathers in
	Candy and Clay, and in Pixel it is a sprite of square pixels. And for the
	season: a scarf in winter, a hat at Halloween, Christmas and New Year.

	The swan in the header lives (swanLive): it looks about, feeds, preens,
	stretches its wings, drifts, dozes off when nothing is pressed for a while,
	turns to the game under the cursor, starts at fast scrolling and cheers when
	a game starts.
*/
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace fe::ui
{
namespace
{

struct Point
{
	float x, y;
};

Point mixed(Point a, Point b, float t)
{
	return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

float mixed(float a, float b, float t)
{
	return a + (b - a) * t;
}

Point bezier(Point p0, Point p1, Point p2, Point p3, float t)
{
	const float u = 1 - t;
	return { u * u * u * p0.x + 3 * u * u * t * p1.x + 3 * u * t * t * p2.x + t * t * t * p3.x,
			u * u * u * p0.y + 3 * u * u * t * p1.y + 3 * u * t * t * p2.y + t * t * t * p3.y };
}

constexpr float Pi = 3.14159265f;

float ramp(double t, double from, double to)
{
	const float x = std::clamp((float)((t - from) / (to - from)), 0.f, 1.f);
	return x * x * (3.f - 2.f * x);
}

// How the bird is drawn in the theme of the moment.
enum class Style { Plain, Ink, Wire, Pixel };

struct Look
{
	Style style = Style::Plain;
	ImU32 body = IM_COL32(255, 255, 255, 255), shade = IM_COL32(190, 214, 245, 255);
	ImU32 beak = IM_COL32(255, 170, 60, 255), eye = IM_COL32(20, 30, 56, 255);
	ImU32 ink = IM_COL32(17, 17, 17, 255);
	float line = 0;		// the outline's width, in sides of the square
	bool wobble = false;	// a hand-drawn line
};

Look lookNow(bool plain)
{
	Look look;
	if (plain || !options::frontend().swanThemed)
		return look;
	const Theme& t = theme();
	const char *id = themeId();
	const auto is = [id](const char *name) { return std::strcmp(id, name) == 0; };
	// The folded wing takes a little of the theme's colour.
	look.shade = mix(IM_COL32(190, 214, 245, 255), t.accent | 0xff000000u, 0.25f);
	if (is("pixel"))
	{
		look.style = Style::Pixel;
		look.ink = IM_COL32(33, 37, 41, 255);
	}
	else if (is("brutal"))
	{
		look.style = Style::Ink;
		look.line = 0.022f;
		look.shade = mix(IM_COL32(255, 255, 255, 255), t.accent | 0xff000000u, 0.6f);
	}
	else if (is("sketch"))
	{
		look.style = Style::Ink;
		look.line = 0.012f;
		look.wobble = true;
		look.ink = IM_COL32(41, 41, 41, 255);
		look.shade = IM_COL32(236, 236, 236, 255);
	}
	else if (is("hazard"))
	{
		look.style = Style::Ink;
		look.line = 0.016f;
		look.body = IM_COL32(232, 232, 226, 255);
		look.shade = IM_COL32(255, 196, 0, 255);
		look.ink = IM_COL32(10, 10, 10, 255);
	}
	else if (is("contrast"))
	{
		look.style = Style::Ink;
		look.line = 0.016f;
		look.ink = IM_COL32(0, 0, 0, 255);
		look.shade = IM_COL32(255, 255, 0, 255);
		look.beak = IM_COL32(255, 140, 0, 255);
	}
	else if (is("blueprint"))
	{
		look.style = Style::Wire;
		look.line = 0.008f;
		look.ink = IM_COL32(220, 236, 255, 255);
		look.body = IM_COL32(40, 90, 170, 70);
		look.shade = IM_COL32(120, 170, 240, 120);
		look.beak = IM_COL32(220, 236, 255, 160);
		look.eye = IM_COL32(220, 236, 255, 255);
	}
	else if (is("candy"))
	{
		look.shade = IM_COL32(255, 186, 220, 255);
		look.beak = IM_COL32(255, 128, 170, 255);
		look.style = Style::Ink;
		look.line = 0.008f;
		look.ink = IM_COL32(200, 120, 170, 255);
	}
	else if (is("clay"))
	{
		look.body = IM_COL32(246, 244, 250, 255);
		look.shade = mix(IM_COL32(220, 214, 236, 255), t.accent | 0xff000000u, 0.3f);
	}
	else if (!t.dark)
	{
		// A white bird on a white page: a thin line round it.
		look.style = Style::Ink;
		look.line = 0.008f;
		look.ink = withAlpha(t.pageText, 0.55f);
	}
	return look;
}

// The season's touch: by the date, or as chosen.
enum class Season { None, Winter, Halloween, Christmas, NewYear };

Season seasonNow(bool plain)
{
	if (plain)
		return Season::None;
	switch (options::frontend().swanSeason)
	{
	case 1: return Season::None;
	case 2: return Season::Winter;
	case 3: return Season::Halloween;
	case 4: return Season::Christmas;
	case 5: return Season::NewYear;
	default: break;
	}
	const time_t now = time(nullptr);
	struct tm local;
	if (now < 1500000000 || localtime_r(&now, &local) == nullptr)
		return Season::None;		// the console's clock was not set
	const int month = local.tm_mon + 1, day = local.tm_mday;
	if ((month == 12 && day == 31) || (month == 1 && day == 1))
		return Season::NewYear;
	if (month == 12 && day <= 26)
		return Season::Christmas;
	if ((month == 10 && day >= 24) || (month == 11 && day == 1))
		return Season::Halloween;
	if (month == 12 || month == 1 || month == 2)
		return Season::Winter;
	return Season::None;
}

// ------------------------------------------------------------- the sprite
//
// The swan of the Pixel theme: frames of 20 by 17 pixels, facing left as the
// mark does. W white, S the folded wing, O the beak, K the outline and the
// eye. Each frame says where its head's top is (for a hat) and its neck (for
// a scarf).

struct Frame
{
	const char *rows[17];
	int headX, headY, neckX, neckY;
};

const Frame Sitting = { {
		"....................",
		"......KKK...........",
		".....KWWWK..........",
		"..OOOWKWWK..........",
		".OOOOWWWWK..........",
		".....KKWWK..........",
		".......KWWK.........",
		"........KWK.........",
		"........KWK.........",
		".......KWWK......K..",
		".......KWWK.....KWK.",
		"......KWWWKKKKKKWWK.",
		".....KWWWWWWSSSSWWK.",
		".....KWWWWWSSSSSSWK.",
		".....KWWWWWWSSSSWK..",
		"......KWWWWWWWWWWK..",
		".......KKKKKKKKKK...",
	}, 7, 1, 9, 8 };

const Frame Looking = { {
		"....................",
		".........KKK........",
		"........KWWWK.......",
		"........KWWKWOOO....",
		"........KWWWWOOOO...",
		"........KWWKK.......",
		".......KWWK.........",
		"........KWK.........",
		"........KWK.........",
		".......KWWK......K..",
		".......KWWK.....KWK.",
		"......KWWWKKKKKKWWK.",
		".....KWWWWWWSSSSWWK.",
		".....KWWWWWSSSSSSWK.",
		".....KWWWWWWSSSSWK..",
		"......KWWWWWWWWWWK..",
		".......KKKKKKKKKK...",
	}, 10, 1, 9, 8 };

const Frame Sleeping = { {
		"....................",
		"....................",
		"....................",
		"....................",
		"....................",
		"....................",
		"....................",
		"....................",
		"...........KKKK.....",
		"..........KWWWWK.K..",
		".........OKWKKWWKWK.",
		"......KKKKWWWWWWKWK.",
		".....KWWWWWWSSSSWWK.",
		".....KWWWWWSSSSSSWK.",
		".....KWWWWWWSSSSWK..",
		"......KWWWWWWWWWWK..",
		".......KKKKKKKKKK...",
	}, 12, 8, 9, 11 };

const Frame Flapping = { {
		"...............K....",
		"......KKK.....KWK...",
		".....KWWWK...KWWK...",
		"..OOOWKWWK..KWWSK...",
		".OOOOWWWWK.KWWSSK...",
		".....KKWWK.KWWSSK...",
		".......KWWKKWWSK....",
		"........KWKKWSK.....",
		"........KWKKSK......",
		".......KWWKKK....K..",
		".......KWWK.....KWK.",
		"......KWWWKKKKKKWWK.",
		".....KWWWWWWSSSSWWK.",
		".....KWWWWWSSSSSSWK.",
		".....KWWWWWWSSSSWK..",
		"......KWWWWWWWWWWK..",
		".......KKKKKKKKKK...",
	}, 7, 1, 9, 8 };

// The hats, 5 wide, their last row on the head's top, leaning back (to the
// right of a bird facing left). R red, W white, K black, P purple, Y yellow.
const char *const ChristmasHat[4] = { "...RW", "..RRR", ".RRR.", "WWWWW" };
const char *const WitchHat[4] = { "...K.", "..KP.", ".KPP.", "KKKKK" };
const char *const PartyHat[4] = { "..Y..", ".YRY.", ".YYY.", "YRYRY" };

ImU32 pixelColour(char c, const Look& look, float alpha)
{
	ImU32 colour = 0;
	switch (c)
	{
	case 'W': colour = look.body; break;
	case 'S': colour = look.shade; break;
	case 'O': colour = look.beak; break;
	case 'K': colour = look.ink; break;
	case 'R': colour = IM_COL32(214, 40, 52, 255); break;
	case 'P': colour = IM_COL32(112, 52, 164, 255); break;
	case 'Y': colour = IM_COL32(250, 200, 40, 255); break;
	default: return 0;
	}
	return withAlpha(colour, alpha);
}

// Draws the sprite; returns where its head's top is, in pixels.
ImVec2 pixelSwan(ImVec2 origin, float size, const SwanPose& pose, float alpha, ImDrawList *list, const Look& look,
		Season season)
{
	const float fly = std::clamp(pose.fly, 0.f, 1.f);
	const Frame *frame = &Sitting;
	if (pose.sleep > 0.5f)
		frame = &Sleeping;
	else if (std::max(pose.wings, fly) > 0.4f && (fly < 0.4f || std::sin(pose.beat) > 0.f || pose.wings > 0.4f))
		frame = (fly > 0.4f && std::sin(pose.beat) <= 0.f) ? &Sitting : &Flapping;
	else if (pose.look > 0.5f || pose.preen > 0.5f)
		frame = &Looking;
	// Pixel art does not turn about: it only flips, and narrows as it does.
	const float face = std::clamp(pose.face, -1.f, 1.f);
	const float wide = std::max(std::fabs(face), 0.15f) * (face < 0 ? -1.f : 1.f);
	const float cell = size * 0.045f;
	const float pivot = 0.53f * size;
	const float left = 0.08f * size, top = 0.78f * size - 17 * cell - pose.stretch * cell * 2;
	const auto rect = [&](int column, int row, ImU32 colour) {
		if (colour == 0)
			return;
		float x0 = left + column * cell - pivot, x1 = x0 + cell;
		x0 *= wide;
		x1 *= wide;
		if (x0 > x1)
			std::swap(x0, x1);
		// Whole pixels of the screen, so the squares stay square-edged.
		const float y = std::floor(origin.y + top + row * cell);
		list->AddRectFilled(ImVec2(std::floor(origin.x + pivot + x0), y),
				ImVec2(std::floor(origin.x + pivot + x1) + 1, std::floor(origin.y + top + (row + 1) * cell) + 1), colour);
	};
	for (int row = 0; row < 17; row++)
		for (int column = 0; column < 20; column++)
		{
			const char c = frame->rows[row][column];
			if (c != '.')
				rect(column, row, pixelColour(c, look, alpha));
		}
	const char *const *hat = season == Season::Christmas ? ChristmasHat : season == Season::Halloween ? WitchHat
			: season == Season::NewYear ? PartyHat : nullptr;
	if (hat != nullptr)
		for (int row = 0; row < 4; row++)
			for (int column = 0; column < 5; column++)
				rect(frame->headX - 2 + column, frame->headY - 3 + row, pixelColour(hat[row][column], look, alpha));
	if (season == Season::Winter)
	{
		// A scarf: red and white round the neck, its end hanging behind.
		for (int i = -1; i <= 1; i++)
			rect(frame->neckX + i, frame->neckY, pixelColour(i == 0 ? 'W' : 'R', look, alpha));
		rect(frame->neckX + 2, frame->neckY + 1, pixelColour('R', look, alpha));
		rect(frame->neckX + 2, frame->neckY + 2, pixelColour('W', look, alpha));
	}
	return ImVec2(origin.x + pivot + (left + frame->headX * cell - pivot) * wide, origin.y + top + frame->headY * cell);
}

} // namespace

void swan(ImVec2 origin, float size, const SwanPose& pose, float alpha, ImDrawList *list, bool plain)
{
	if (alpha <= 0.004f || size <= 1.f)
		return;
	if (list == nullptr)
		list = draw();
	origin.x += pose.drift * size;
	origin.y -= pose.lift * size;
	const Look look = lookNow(plain);
	const Season season = seasonNow(plain);
	const double now = clock();

	// What floats about it: a heart, the letters of a doze.
	const auto extras = [&](ImVec2 head) {
		if (pose.heart > 0.f && pose.heart < 1.f)
		{
			const float h = pose.heart;
			const float rise = size * (0.10f + 0.35f * h);
			const float fade = std::sin(Pi * std::min(h * 1.4f, 1.f)) * alpha;
			const float r = size * (0.05f + 0.03f * std::min(h * 3.f, 1.f));
			const ImVec2 c(head.x, head.y - rise);
			const ImU32 red = withAlpha(IM_COL32(255, 86, 120, 255), fade);
			list->AddCircleFilled(ImVec2(c.x - r * 0.5f, c.y), r * 0.56f, red, 20);
			list->AddCircleFilled(ImVec2(c.x + r * 0.5f, c.y), r * 0.56f, red, 20);
			list->AddTriangleFilled(ImVec2(c.x - r * 1.04f, c.y + r * 0.18f), ImVec2(c.x + r * 1.04f, c.y + r * 0.18f),
					ImVec2(c.x, c.y + r * 1.25f), red);
		}
		if (pose.sleep > 0.8f && size > 24.f)
		{
			// Z, z, z, rising and fading, one after another.
			for (int i = 0; i < 3; i++)
			{
				const float t = (float)std::fmod(now * 0.45 + i / 3.0, 1.0);
				const float a = std::sin(Pi * t) * (pose.sleep - 0.8f) * 5.f * alpha;
				const float s = size * (0.07f + 0.06f * t);
				text(ImVec2(head.x + size * (0.05f + 0.10f * t), head.y - size * (0.10f + 0.30f * t)),
						withAlpha(theme().pageText, a), "z", Bold, s / std::max(px(1), 0.001f));
			}
		}
	};

	if (look.style == Style::Pixel)
	{
		extras(pixelSwan(origin, size, pose, alpha, list, look, season));
		return;
	}

	const float fly = std::clamp(pose.fly, 0.f, 1.f);
	const float swim = 1.f - fly;
	const float open = std::max(fly, std::clamp(pose.wings, 0.f, 1.f));
	const float cosT = std::cos(pose.tilt), sinT = std::sin(pose.tilt);
	// The body's middle is what the bird turns about.
	const Point pivot = { 0.585f, 0.640f };
	const float face = std::clamp(pose.face, -1.f, 1.f);
	const float wide = std::max(std::fabs(face), 0.06f);		// how wide a round part is, seen turning
	const auto place = [&](Point p) {
		const float dx = (p.x - pivot.x) * face, dy = p.y - pivot.y;
		return ImVec2(origin.x + (pivot.x + dx * cosT - dy * sinT) * size,
				origin.y + (pivot.y + dx * sinT + dy * cosT) * size);
	};
	const auto tone = [alpha](ImU32 c, float a = 1.f) { return withAlpha(c, std::clamp(alpha * a, 0.f, 1.f)); };

	// Drawn twice when the theme inks it: first every shape a line wider in
	// the ink, then the shapes themselves over it, so only the outside edge of
	// the whole bird shows. A wireframe shows every shape's edge instead.
	const bool inked = look.style != Style::Plain;
	const float line = std::max(look.line * size, 1.f);
	int pass = inked ? 0 : 1;
	const auto wobble = [&](ImVec2 p, int i) {
		if (!look.wobble || pass != 0)
			return p;
		return ImVec2(p.x + line * 0.35f * std::sin(i * 2.7f + (float)now * 0.0f), p.y + line * 0.35f * std::cos(i * 1.9f));
	};
	const auto poly = [&](const ImVec2 *points, int count, ImU32 fill) {
		if (pass == 0)
		{
			ImVec2 shifted[64];
			count = std::min(count, 64);
			for (int i = 0; i < count; i++)
				shifted[i] = wobble(points[i], i);
			list->AddPolyline(shifted, count, tone(look.ink, ((fill >> 24) & 0xff) / 255.f > 0.05f ? 1.f : 0.f),
					ImDrawFlags_Closed, line * 2.f);
			if (look.style != Style::Wire)
				list->AddConcavePolyFilled(points, count, tone(look.ink, ((fill >> 24) & 0xff) / 255.f));
		}
		else
			list->AddConcavePolyFilled(points, count, fill);
	};
	const auto circle = [&](ImVec2 c, float r, ImU32 fill, int segments) {
		if (pass == 0)
		{
			if (look.style == Style::Wire)
				list->AddCircle(c, r, tone(look.ink), segments, line);
			else
				list->AddCircleFilled(wobble(c, (int)r), r + line, tone(look.ink, ((fill >> 24) & 0xff) / 255.f), segments);
		}
		else
			list->AddCircleFilled(c, r, fill, segments);
	};
	const auto ellipse = [&](ImVec2 c, ImVec2 r, ImU32 fill, int segments) {
		if (pass == 0)
		{
			if (look.style == Style::Wire)
			{
				ImVec2 points[48];
				const int n = std::min(segments, 48);
				for (int i = 0; i < n; i++)
				{
					const float a = 2 * Pi * i / n;
					const float x = std::cos(a) * r.x, y = std::sin(a) * r.y;
					points[i] = ImVec2(c.x + x * cosT - y * sinT, c.y + x * sinT + y * cosT);
				}
				list->AddPolyline(points, n, tone(look.ink), ImDrawFlags_Closed, line);
			}
			else
				list->AddEllipseFilled(wobble(c, (int)r.x), ImVec2(r.x + line, r.y + line),
						tone(look.ink, ((fill >> 24) & 0xff) / 255.f), pose.tilt, segments);
		}
		else
			list->AddEllipseFilled(c, r, fill, pose.tilt, segments);
	};

	const ImU32 white = tone(look.body);
	const ImU32 shade = tone(look.shade);

	// The head's turn: 0 looks ahead, as the mark does; towards 1 it turns to
	// look behind (the beak shortens, points at the viewer, then the other
	// way); below 0 it dips forward.
	const float back = std::clamp(pose.look, 0.f, 1.f) * swim;
	const float dip = std::clamp(-pose.look, 0.f, 1.f) * swim;
	const float preen = std::clamp(pose.preen, 0.f, 1.f) * swim;
	const float sleep = std::clamp(pose.sleep, 0.f, 1.f) * swim;
	const float stretch = std::clamp(pose.stretch, 0.f, 1.f);
	const float lean = std::clamp(pose.lean, 0.f, 1.f) * swim;
	const float turn = back * 0.86f * Pi;
	float facing = std::cos(turn);		// 1 ahead, 0 at the viewer, -1 behind
	facing = mixed(facing, -0.55f, preen);
	facing = mixed(facing, -1.f, sleep);

	// The neck: an S from the chest to the head; stretched out ahead in flight.
	Point n0 = mixed(Point{ 0.415f, 0.640f }, Point{ 0.400f, 0.612f }, fly);
	Point n1 = mixed(Point{ 0.200f, 0.560f }, Point{ 0.300f, 0.598f }, fly);
	Point n2 = mixed(Point{ 0.520f, 0.360f }, Point{ 0.200f, 0.584f }, fly);
	Point n3 = mixed(Point{ 0.345f, 0.235f }, Point{ 0.095f, 0.566f }, fly);
	// Looking behind brings the head back over the body; a dip lowers it.
	const float over = std::sin(turn * 0.5f);
	n3.x += 0.085f * over * over - 0.035f * dip;
	n3.y += -0.012f * over + 0.070f * dip;
	n2.x += -0.030f * over - 0.030f * dip;
	n2.y += 0.030f * dip;
	n1.x += -0.012f * dip;
	// Tall, startled; forward and down, watching something below.
	n3.y -= 0.090f * stretch;
	n2.y -= 0.060f * stretch;
	n3.x += 0.015f * stretch;
	n3.x -= 0.070f * lean;
	n3.y += 0.090f * lean;
	n2.x -= 0.040f * lean;
	n2.y += 0.040f * lean;
	// Preening: the head goes round to the wing on its back.
	n3 = mixed(n3, Point{ 0.600f, 0.505f }, preen);
	n2 = mixed(n2, Point{ 0.580f, 0.300f }, preen);
	n1 = mixed(n1, Point{ 0.330f, 0.390f }, preen);
	// Asleep: the neck folded down, the head on the back.
	n3 = mixed(n3, Point{ 0.640f, 0.520f }, sleep);
	n2 = mixed(n2, Point{ 0.520f, 0.470f }, sleep);
	n1 = mixed(n1, Point{ 0.330f, 0.560f }, sleep);
	const Point head = { n3.x - 0.010f * facing, n3.y - 0.003f };

	// A wing, open: from the shoulder to a tip that the beat moves, and back
	// to the body behind it. `beat` is 1 with the wing up, -1 down.
	const auto wing = [&](float beat, Point shift, ImU32 fill, bool edge) {
		const Point shoulder = { 0.47f + shift.x, 0.600f + shift.y };
		const Point rear = { 0.72f + shift.x, 0.615f + shift.y };
		const Point tip = { 0.640f + 0.060f * beat + shift.x, 0.600f - 0.500f * beat + shift.y };
		const Point lead1 = { shoulder.x - 0.070f, shoulder.y - 0.260f * beat };
		const Point lead2 = { tip.x - 0.120f, tip.y + 0.100f * beat };
		const Point trail1 = { tip.x + 0.150f, tip.y + 0.130f * beat };
		const Point trail2 = { rear.x + 0.110f, rear.y - 0.170f * beat };
		ImVec2 points[26];
		int count = 0;
		for (int i = 0; i <= 12; i++)
			points[count++] = place(bezier(shoulder, lead1, lead2, tip, (float)i / 12.f));
		for (int i = 1; i <= 12; i++)
			points[count++] = place(bezier(tip, trail1, trail2, rear, (float)i / 12.f));
		poly(points, count, fill);
		if (edge && pass == 1)
		{
			// Its trailing edge, so a white wing shows against the white body.
			for (int i = 0; i <= 12; i++)
				list->PathLineTo(place(bezier(tip, trail1, trail2, rear, (float)i / 12.f)));
			list->PathStroke(shade, std::max(size * 0.012f, 1.f));
		}
	};
	// On the water the wings rise from the back rather than the shoulders'
	// full beat: a stretch, or a flap that splashes.
	const float beatOf = fly > 0.5f ? 1.f : 0.75f;

	for (; pass < 2; pass++)
	{
		// The far wing, behind everything, a beat ahead of the near one.
		if (open > 0.02f)
			wing(std::sin(pose.beat + 0.55f) * beatOf * (fly > 0.02f ? 1.f : open), { 0.035f, -0.012f },
					tone(look.shade, std::min(open * 4.f, 1.f)), false);

		// The tail: raised on the water, flat behind in the air.
		{
			const Point swimTail[4] = { { 0.700f, 0.560f }, { 0.905f, 0.435f }, { 0.835f, 0.660f }, { 0.740f, 0.740f } };
			const Point flyTail[4] = { { 0.740f, 0.575f }, { 0.975f, 0.605f }, { 0.900f, 0.668f }, { 0.760f, 0.690f } };
			ImVec2 points[4];
			for (int i = 0; i < 4; i++)
				points[i] = place(mixed(swimTail[i], flyTail[i], fly));
			poly(points, 4, white);
			circle(place(mixed(Point{ 0.800f, 0.585f }, Point{ 0.800f, 0.622f }, fly)), size * mixed(0.075f, 0.046f, fly),
					white, 32);
		}

		// The body.
		{
			const Point centre = mixed(Point{ 0.585f, 0.640f }, Point{ 0.565f, 0.625f }, fly);
			ellipse(place(centre), ImVec2(size * mixed(0.250f, 0.262f, fly) * wide, size * mixed(0.135f, 0.098f, fly)), white,
					48);
		}

		// The neck.
		const float thick0 = mixed(0.062f, 0.056f, fly), thick1 = mixed(0.042f, 0.037f, fly);
		constexpr int Links = 30;
		for (int i = 0; i <= Links; i++)
		{
			const float t = (float)i / Links;
			// In a wireframe a neck of thirty circles is a tangle: its two
			// ends only, and a line along it.
			if (look.style == Style::Wire && i != 0 && i != Links)
				continue;
			circle(place(bezier(n0, n1, n2, n3, t)), size * mixed(thick0, thick1, t), white, 20);
		}
		if (look.style == Style::Wire)
		{
			for (int i = 0; i <= Links; i++)
				list->PathLineTo(place(bezier(n0, n1, n2, n3, (float)i / Links)));
			if (pass == 0)
				list->PathStroke(tone(look.ink), line);
			else
				list->PathStroke(white, size * (thick0 + thick1));
		}

		// A scarf, low on the neck, its end hanging behind.
		if ((season == Season::Winter || season == Season::Christmas) && pass == 1)
		{
			const ImU32 red = tone(IM_COL32(214, 40, 52, 255)), stripe = tone(IM_COL32(255, 255, 255, 255));
			for (int i = 0; i <= 8; i++)
			{
				const float t = 0.48f + 0.012f * i;
				circle(place(bezier(n0, n1, n2, n3, t)), size * (mixed(thick0, thick1, t) + 0.006f), (i / 3) % 2 ? stripe : red,
						20);
			}
			const Point knot = bezier(n0, n1, n2, n3, 0.53f);
			const ImVec2 end[4] = { place({ knot.x + 0.02f, knot.y - 0.01f }), place({ knot.x + 0.075f, knot.y + 0.07f }),
					place({ knot.x + 0.045f, knot.y + 0.095f }), place({ knot.x - 0.005f, knot.y + 0.02f }) };
			list->AddConvexPolyFilled(end, 4, red);
		}

		// The head, the beak and the eye.
		ellipse(place(head), ImVec2(size * 0.052f, size * 0.047f), white, 28);
		const float away = 1.f - std::fabs(facing);		// 1 when the beak points at the viewer
		if (std::fabs(facing) > 0.04f)
		{
			const float base = head.x - 0.035f * facing;
			// Preening and asleep the beak points down into the feathers.
			const float down = 0.05f * std::max(preen, sleep);
			const ImVec2 points[3] = { place({ base, head.y - 0.020f }),
					place({ head.x - 0.140f * facing, head.y + 0.036f + down }), place({ base, head.y + 0.036f }) };
			poly(points, 3, tone(look.beak));
		}
		if (pass == 1)
		{
			// Seen from the front the beak is a small oval under the eyes.
			const float front = std::clamp((away - 0.45f) / 0.55f, 0.f, 1.f);
			if (front > 0.02f)
				list->AddEllipseFilled(place({ head.x - 0.018f * facing, head.y + 0.020f }),
						ImVec2(size * 0.022f * front, size * 0.026f * front), tone(look.beak), pose.tilt, 16);
			const ImU32 eye = tone(look.eye);
			const float eyeY = head.y - 0.010f;
			const float spread = 0.024f * front;		// both eyes show when it faces the viewer
			const float r = std::max(size * 0.0095f, 0.8f);
			if (sleep > 0.5f)
			{
				// Shut: a short curve.
				const ImVec2 e = place({ head.x - 0.008f * facing, eyeY });
				list->AddLine(ImVec2(e.x - r * 1.4f, e.y), ImVec2(e.x + r * 1.4f, e.y + r * 0.4f), eye, std::max(r * 0.7f, 1.f));
			}
			else
			{
				list->AddCircleFilled(place({ head.x - 0.008f * facing - spread, eyeY }), r, eye, 12);
				if (front > 0.02f)
					list->AddCircleFilled(place({ head.x - 0.008f * facing + spread, eyeY }), r, tone(look.eye, front), 12);
			}
		}

		// The folded wing: a crescent of feathers along the back, on the water,
		// while the wings are shut.
		const float folded = swim * (1.f - open);
		if (folded > 0.02f)
		{
			const Point centre = { 0.660f, 0.598f };
			constexpr int Steps = 18;
			ImVec2 points[(Steps + 1) * 2];
			for (int i = 0; i <= Steps; i++)
			{
				const float along = (float)i / Steps;
				const float angle = (0.10f + 0.74f * along) * Pi;
				const Point outer = { centre.x + 0.200f * std::cos(angle), centre.y + 0.124f * std::sin(angle) };
				// Thick in the middle, to nothing at its ends.
				const float thick = 0.052f * std::pow(std::sin(Pi * along), 0.75f);
				const float dx = centre.x - outer.x, dy = centre.y - 0.060f - outer.y;
				const float length = std::max(std::sqrt(dx * dx + dy * dy), 0.001f);
				points[i] = place(outer);
				points[(Steps + 1) * 2 - 1 - i] = place({ outer.x + dx / length * thick, outer.y + dy / length * thick });
			}
			if (pass == 1)
				list->AddConcavePolyFilled(points, (Steps + 1) * 2, tone(look.shade, folded));
		}

		// The near wing, over the body.
		if (open > 0.02f)
			wing(std::sin(pose.beat) * beatOf * (fly > 0.02f ? 1.f : open), { 0, 0 }, tone(look.body, std::min(open * 4.f, 1.f)), true);

		// A hat on the head, its tip leaning back.
		if (season == Season::Christmas || season == Season::Halloween || season == Season::NewYear)
		{
			const float hr = 0.050f;
			const Point brimL = { head.x - hr * 1.1f, head.y - hr * 0.55f }, brimR = { head.x + hr * 1.1f, head.y - hr * 0.55f };
			if (season == Season::Christmas)
			{
				const ImVec2 cone[3] = { place({ head.x - hr * 0.95f, brimL.y }), place({ head.x + hr * 0.95f, brimL.y }),
						place({ head.x + hr * 1.9f, head.y - hr * 2.4f }) };
				poly(cone, 3, tone(IM_COL32(214, 40, 52, 255)));
				if (pass == 1)
				{
					list->AddLine(place(brimL), place(brimR), tone(IM_COL32(255, 255, 255, 255)), size * 0.030f);
					list->AddCircleFilled(place({ head.x + hr * 1.9f, head.y - hr * 2.4f }), size * 0.020f,
							tone(IM_COL32(255, 255, 255, 255)), 16);
				}
			}
			else if (season == Season::Halloween)
			{
				const ImVec2 cone[3] = { place({ head.x - hr * 0.7f, brimL.y }), place({ head.x + hr * 0.7f, brimL.y }),
						place({ head.x + hr * 1.3f, head.y - hr * 3.0f }) };
				poly(cone, 3, tone(IM_COL32(40, 24, 60, 255)));
				if (pass == 1)
				{
					list->AddLine(place({ brimL.x - hr * 0.4f, brimL.y }), place({ brimR.x + hr * 0.4f, brimR.y }),
							tone(IM_COL32(40, 24, 60, 255)), size * 0.022f);
					list->AddLine(place({ head.x - hr * 0.62f, brimL.y - hr * 0.35f }),
							place({ head.x + hr * 0.62f, brimL.y - hr * 0.35f }), tone(IM_COL32(255, 140, 20, 255)),
							size * 0.014f);
				}
			}
			else
			{
				const ImVec2 cone[3] = { place({ head.x - hr * 0.75f, brimL.y }), place({ head.x + hr * 0.75f, brimL.y }),
						place({ head.x + hr * 0.6f, head.y - hr * 2.6f }) };
				poly(cone, 3, tone(IM_COL32(250, 200, 40, 255)));
				if (pass == 1)
				{
					for (int i = 1; i <= 2; i++)
					{
						const float t = i / 3.f;
						const Point a = { mixed(head.x - hr * 0.75f, head.x + hr * 0.6f, t), mixed(brimL.y, head.y - hr * 2.6f, t) };
						const Point b = { mixed(head.x + hr * 0.75f, head.x + hr * 0.6f, t), a.y };
						list->AddLine(place(a), place(b), tone(IM_COL32(214, 40, 52, 255)), size * 0.012f);
					}
					list->AddCircleFilled(place({ head.x + hr * 0.6f, head.y - hr * 2.6f }), size * 0.016f,
							tone(IM_COL32(214, 40, 52, 255)), 12);
				}
			}
		}
	}
	extras(place({ head.x, head.y - 0.04f }));
}

// What the swan does while it only sits in its box: now and then it looks
// behind it, and once in a while dips its head. A slow cycle, by the clock.
SwanPose swanIdle(double time)
{
	SwanPose pose;
	const double t = std::fmod(time, 13.0);
	// Looks behind, twice: a glance, then a longer look.
	const float look = ramp(t, 3.2, 3.75) * (1.f - ramp(t, 4.9, 5.45)) + ramp(t, 6.3, 6.8) * (1.f - ramp(t, 8.6, 9.2));
	const float dip = ramp(t, 10.6, 11.0) * (1.f - ramp(t, 11.5, 12.0));
	pose.look = look - dip * 0.8f;
	// It is on water: it rocks a little.
	pose.tilt = 0.018f * (float)std::sin(time * 1.3);
	return pose;
}

// --------------------------------------------------------------- living

namespace
{
struct Life
{
	double cueAt[5] = { -100, -100, -100, -100, -100 };
	double moves[8] = {};
	int nextMove = 0;
	double touchedAt = 0;
	double watchSince = -100;
	ImVec2 watch;
	bool started = false;
} life;

uint32_t hashOf(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// A bump that rises over `in` seconds, holds, and falls over `out` by `end`.
float bump(double t, double in, double end, double out)
{
	if (t < 0 || t > end)
		return 0;
	return ramp(t, 0, in) * (1.f - ramp(t, end - out, end));
}

// The idle moves, one after another by the clock: each stretch of time does
// one thing, chosen by its number.
void idleMoves(double time, bool lively, SwanPose& pose)
{
	const double length = lively ? 7.5 : 11.0;
	const double k = std::floor(time / length);
	const double t = time - k * length;
	const uint32_t h = hashOf((uint32_t)(int64_t)k * 2654435761u + 17u);
	const int kinds = lively ? 7 : 3;
	const int kind = (int)(h % (uint32_t)kinds);
	const float e = ramp(t, 1.0, 1.6) * (1.f - ramp(t, length - 2.0, length - 1.3));
	switch (kind)
	{
	case 0:		// a glance behind, and back
		pose.look = bump(t - 1.5, 0.5, 2.0, 0.5);
		break;
	case 1:		// a long look behind
		pose.look = e;
		break;
	case 2:		// feeding: the head down twice
		pose.look = -(bump(t - 1.2, 0.35, 1.3, 0.4) + bump(t - 3.0, 0.35, 1.5, 0.4)) * 0.9f;
		break;
	case 3:		// preening, with a little shake of the head in the feathers
		pose.preen = e;
		pose.tilt += 0.02f * (float)std::sin(time * 14.0) * e;
		break;
	case 4:		// it stretches its wings and flaps them twice
		pose.wings = bump(t - 1.2, 0.4, 2.6, 0.6);
		pose.beat = (float)(t * 2 * Pi * 1.6) + Pi * 0.5f;
		pose.stretch = 0.4f * pose.wings;
		break;
	case 5:		// it drifts out to one side, turns, and comes back
	{
		const float dir = (h >> 8) & 1 ? 1.f : -1.f;
		const float p = std::clamp((float)(t / length), 0.f, 1.f);
		const float s = std::sin(Pi * p);
		pose.drift = 0.06f * dir * s * s;
		// Facing the way it goes: away (left, as it faces) needs no turn.
		const float velocity = dir * std::sin(2 * Pi * p);
		const float right = std::clamp(velocity * 4.f, 0.f, 1.f);
		pose.face = 1.f - 2.f * right;
		pose.tilt += 0.03f * velocity;
		break;
	}
	default:	// it rests, looking ahead
		break;
	}
}
} // namespace

void swanCue(SwanCue cue)
{
	const double now = clock();
	if (cue == SwanCue::Move)
	{
		// Many moves in a moment are fast scrolling: it starts.
		life.moves[life.nextMove] = now;
		life.nextMove = (life.nextMove + 1) % 8;
		int recent = 0;
		for (const double at : life.moves)
			recent += now - at < 1.0 ? 1 : 0;
		if (recent >= 7 && now - life.cueAt[(int)SwanCue::Startle] > 2.5)
			life.cueAt[(int)SwanCue::Startle] = now;
		life.touchedAt = now;
		return;
	}
	if (cue == SwanCue::Touch)
	{
		// Woken: it starts a little.
		if (now - life.touchedAt > 40.0 && options::frontend().swanMoves == 0)
			life.cueAt[(int)SwanCue::Startle] = now;
		life.touchedAt = now;
		return;
	}
	life.cueAt[(int)cue] = now;
	life.touchedAt = now;
}

void swanWatch(ImVec2 target)
{
	if (std::fabs(target.x - life.watch.x) > 1.f || std::fabs(target.y - life.watch.y) > 1.f)
	{
		life.watch = target;
		life.watchSince = clock();
	}
}

float swanCheerTime()
{
	return options::frontend().swanMoves == 0 && motion() == MotionFull ? 0.55f : 0.f;
}

SwanPose swanLive(double time, ImVec2 origin, float size)
{
	const int moves = motion() == MotionFull ? options::frontend().swanMoves : 2;
	if (!life.started)
	{
		life.started = true;
		life.touchedAt = time;
	}
	SwanPose pose;
	if (moves == 2)
		return pose;
	const bool lively = moves == 0;
	pose.tilt = 0.018f * (float)std::sin(time * 1.3);
	idleMoves(time, lively, pose);

	// Nothing pressed for a while: it dozes off.
	const double quiet = time - life.touchedAt;
	if (lively && quiet > 40.0)
	{
		const float doze = ramp(quiet, 40.0, 41.6);
		pose = SwanPose();
		pose.sleep = doze;
		pose.tilt = 0.012f * (float)std::sin(time * 0.9);
		return pose;
	}

	// The game under the cursor: it turns its head to it.
	const double watching = time - life.watchSince;
	if (watching >= 0 && watching < 3.0)
	{
		const float w = bump(watching, 0.25, 3.0, 0.6);
		const ImVec2 me(origin.x + size * 0.4f, origin.y + size * 0.3f);
		const float right = life.watch.x > me.x ? 1.f : 0.f;
		pose.look *= 1.f - w;
		pose.preen *= 1.f - w;
		pose.wings *= 1.f - w;
		pose.drift *= 1.f - w;
		pose.face = mixed(pose.face, 1.f - 2.f * right, w);
		const float below = std::clamp((life.watch.y - me.y) / std::max(height() * 0.6f, 1.f), 0.f, 1.f);
		pose.lean = w * (0.25f + 0.6f * below);
	}

	// What happened.
	const double startled = time - life.cueAt[(int)SwanCue::Startle];
	if (startled >= 0 && startled < 0.9)
	{
		const float s = bump(startled, 0.08, 0.9, 0.5);
		pose.stretch = std::max(pose.stretch, s);
		pose.wings = std::max(pose.wings, 0.55f * s);
		pose.beat = (float)(startled * 2 * Pi * 4.0);
		pose.lift = 0.05f * std::sin(Pi * std::min((float)startled / 0.35f, 1.f));
		pose.lean = 0;
	}
	const double cheered = time - life.cueAt[(int)SwanCue::Cheer];
	if (cheered >= 0 && cheered < 1.4)
	{
		const float c = bump(cheered, 0.12, 1.4, 0.4);
		pose.wings = std::max(pose.wings, c);
		pose.beat = (float)(cheered * 2 * Pi * 3.2) + Pi * 0.5f;
		pose.stretch = std::max(pose.stretch, 0.6f * c);
		pose.lift = 0.06f * std::fabs(std::sin((float)cheered * 2 * Pi * 1.6f)) * c;
		pose.lean = 0;
		pose.look = 0;
	}
	const double loved = time - life.cueAt[(int)SwanCue::Love];
	if (loved >= 0 && loved < 1.6)
	{
		pose.heart = (float)(loved / 1.6);
		// It looks round at the viewer, pleased.
		pose.look = std::max(pose.look, 0.55f * bump(loved, 0.2, 1.6, 0.4));
	}
	return pose;
}

std::vector<std::string> swanMoveNames()
{
	return { "Lively", "Calm", "Still" };
}

std::vector<std::string> swanSeasonNames()
{
	return { "By the date", "None", "Winter scarf", "Halloween hat", "Christmas hat", "New Year hat" };
}

} // namespace fe::ui
