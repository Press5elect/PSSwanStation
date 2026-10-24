/*
	PSSwanStation - the interface's drawing: fonts, themes, the frame's
	layers, the pieces every screen is made of.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	Everything is drawn by the interface kit, BlackBearReloaded's
	ps5-homebrew-ui through mihawk-99's PS5_VKHomebrewUI (GPL-3.0-or-later):
	its shapes are signed distance fields, its text an SDF atlas, its panels
	and focus rings drawn by its Painter in the chosen theme's style (glass,
	soft shadows, hard outlines, bevels, pixels, a pen...). A frame is queued as
	the kit's layers: the theme's backdrop, the background list (washes, the
	game's dimming), the frosted-glass copy, the foreground list (everything
	else). The game's own picture is under all of them (display::present).

	Sizes are given in units of a 1080-line screen and scaled to the display
	(two pixels a unit at 2160 lines) and by the interface-size setting. Text
	is placed by the top of its line, as the pages were written.

	The fonts (Inter, Montserrat, Press Start 2P and Patrick Hand under the SIL
	OFL 1.1, DejaVu Sans Mono under the Bitstream Vera licence, Font Awesome
	Free's solid icons under the SIL OFL 1.1), baked by ps5/tools/bake-fonts.sh,
	and the logo are part of the program file, so the interface does not depend
	on files beside it.
*/
#include "ui.h"
#include "tuning.h"
#include "display.h"
#include "fe.h"

#include "gfx/draw_list.hpp"
#include "gfx/font.hpp"
#include "gfx/vk/vk_renderer.hpp"
#include "ui/fonts.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include <cstring>

// The files in ps5/assets, in the program's read-only data.
#define FE_EMBED(symbol, file) \
	__asm__(".pushsection .rodata\n" \
			".balign 16\n" \
			".globl " #symbol "\n" \
			".hidden " #symbol "\n" \
			#symbol ":\n" \
			".incbin \"" FE_ASSET_DIR "/" file "\"\n" \
			".globl " #symbol "_end\n" \
			".hidden " #symbol "_end\n" \
			#symbol "_end:\n" \
			".byte 0\n" \
			".popsection\n"); \
	extern "C" const unsigned char symbol[], symbol##_end[]

FE_EMBED(fe_hui_regular, "hui/fonts/inter-regular.huifont");
FE_EMBED(fe_hui_semibold, "hui/fonts/inter-semibold.huifont");
FE_EMBED(fe_hui_display, "hui/fonts/montserrat-medium.huifont");
FE_EMBED(fe_hui_mono, "hui/fonts/dejavu-sans-mono.huifont");
FE_EMBED(fe_hui_pixel, "hui/fonts/press-start-2p.huifont");
FE_EMBED(fe_hui_hand, "hui/fonts/patrick-hand.huifont");
FE_EMBED(fe_hui_icons, "hui/fonts/icons.huifont");
FE_EMBED(fe_logo_box_png, "logo-box.png");

namespace fe::ui
{

namespace gfx = hui::gfx;

namespace
{

// ------------------------------------------------------------------ state

gfx::Font faces[7];
hui::ui::Fonts kitFonts;
hui::ui::FontRef iconFont;
bool fontsReady = false;

float unit = 1.f;
double started;
double lastFrameAt = 0;
float frameDt = 1.f / 60.f;
Theme current;
Image logoImage;

// The frame's lists, and the draw lists the pages are given for them.
gfx::DrawList backList, frontList;
ImDrawList backShim, frontShim, sceneShim;
Scene sceneMesh;
bool inScene = false;
bool wantBackdrop = false;

// The kit's theme the frame is drawn in: the chosen one, or the title's own.
hui::ui::Theme swanTheme;
const hui::ui::Theme *kitTheme = nullptr;

struct Accent
{
	const char *name;
	ImU32 colour;
};
const Accent accents[AccentCount] = {
	{ "Swan blue", IM_COL32(74, 163, 255, 255) },
	{ "Teal", IM_COL32(45, 212, 191, 255) },
	{ "Violet", IM_COL32(167, 139, 250, 255) },
	{ "Amber", IM_COL32(251, 191, 36, 255) },
	{ "Rose", IM_COL32(251, 113, 133, 255) },
	{ "Green", IM_COL32(74, 222, 128, 255) },
};

// ---------------------------------------------------------------- colours

gfx::Color colourOf(ImU32 c)
{
	return { (float)(c & 0xff) / 255.f, (float)((c >> 8) & 0xff) / 255.f, (float)((c >> 16) & 0xff) / 255.f,
		(float)(c >> 24) / 255.f };
}

ImU32 u32Of(gfx::Color c)
{
	const auto b = [](float v) { return (ImU32)(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
	return IM_COL32(b(c.r), b(c.g), b(c.b), b(c.a));
}

gfx::Rect rectOf(ImVec2 a, ImVec2 b)
{
	return { std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x), std::fabs(b.y - a.y) };
}

// ----------------------------------------------------------------- themes

// The title's own look as a kit theme: deep blue panels with soft shadows on
// slow clouds of the accent colour, as PSSwanStation looked before the kit.
void makeSwanTheme(ImU32 accent)
{
	const hui::ui::Theme *base = nullptr;
	for (const hui::ui::Theme& t : hui::ui::themes())
		if (std::strcmp(t.id, "acrylic") == 0)
			base = &t;
	if (base != nullptr)
		swanTheme = *base;
	swanTheme.id = "psswanstation";
	swanTheme.name = "PSSwanStation";
	swanTheme.family = "The title's own";
	swanTheme.summary = "Deep blue panels on the water, the accent of your choice";
	swanTheme.page = gfx::Color::rgb(0x0e121e);
	swanTheme.page_text = gfx::Color::rgb(0xeef2fa);
	swanTheme.page_text_muted = gfx::Color::rgb(0xa0aac0);
	swanTheme.surface = gfx::Color::rgb(0x1a2032, 0.92f);
	swanTheme.surface_high = gfx::Color::rgb(0x2c3652);
	swanTheme.text = gfx::Color::rgb(0xeef2fa);
	swanTheme.text_muted = gfx::Color::rgb(0xa0aac0);
	swanTheme.accent = colourOf(accent);
	swanTheme.primary = colourOf(accent);
	swanTheme.on_primary = gfx::Color::rgb(0x080e1c);
	swanTheme.secondary = gfx::Color::rgb(0x2c3652);
	swanTheme.on_secondary = gfx::Color::rgb(0xeef2fa);
	swanTheme.outline = gfx::Color::rgb(0xffffff, 0.10f);
	swanTheme.focus = colourOf(accent);
	swanTheme.shadow = gfx::Color::rgb(0x000000, 0.45f);
	swanTheme.light = gfx::Color::rgb(0xffffff, 0.08f);
	swanTheme.style = hui::ui::SurfaceStyle::soft;
	swanTheme.corner = hui::ui::Corner::round;
	swanTheme.radius = 12.f;
	swanTheme.radius_card = 16.f;
	swanTheme.border = 0.f;
	swanTheme.heading = hui::ui::FontRole::semibold;
	swanTheme.label = hui::ui::FontRole::semibold;
	swanTheme.caps = false;
	swanTheme.tracking = 0.f;
	swanTheme.dark = true;
	swanTheme.backdrop.mode = gfx::BackdropMode::aurora;
	swanTheme.backdrop.colors[0] = gfx::Color::rgb(0x0e121e);
	swanTheme.backdrop.colors[1] = gfx::Color::rgb(0x06080e);
	swanTheme.backdrop.colors[2] = colourOf(accent).with_alpha(0.55f);
	swanTheme.backdrop.colors[3] = gfx::Color::rgb(0x785aff, 0.45f);
}

// What a page's text at this size is drawn with, and at what kit size: the
// kit's sizes are an em, the pages' a line's height (top to bottom).
struct Face
{
	const hui::ui::FontRef *ref;
	float size;		// for the kit
	float pixels;	// the line's height
};

const hui::ui::FontRef& roleFont(hui::ui::FontRole role)
{
	switch (role)
	{
	case hui::ui::FontRole::regular: return kitFonts.regular;
	case hui::ui::FontRole::semibold: return kitFonts.semibold;
	case hui::ui::FontRole::display: return kitFonts.display;
	case hui::ui::FontRole::mono: return kitFonts.mono;
	case hui::ui::FontRole::pixel: return kitFonts.pixel;
	case hui::ui::FontRole::hand: return kitFonts.hand;
	}
	return kitFonts.regular;
}

Face faceOf(Font font, float units)
{
	const hui::ui::Theme& t = *kitTheme;
	const hui::ui::FontRef *ref = &kitFonts.regular;
	// Running text is the regular face; a handwritten theme writes it by hand.
	// Labels and titles are the theme's own faces.
	switch (font)
	{
	case Body: ref = t.heading == hui::ui::FontRole::hand ? &kitFonts.hand : &kitFonts.regular; break;
	case Bold: ref = &roleFont(t.label); break;
	case Title: ref = &roleFont(t.heading); break;
	case Huge: ref = &roleFont(t.heading == hui::ui::FontRole::semibold ? hui::ui::FontRole::display : t.heading); break;
	}
	const float pixels = px(units);
	const gfx::Font& f = *ref->font;
	const float span = std::max(f.ascent(1.f) + f.descent(1.f), 0.5f);
	// The square pixel face is wide: a little smaller, so lines still fit.
	// Handwriting is drawn small for its line: a little larger, to read alike.
	const float shrink = ref == &kitFonts.pixel ? 0.72f : ref == &kitFonts.hand ? 1.22f : 1.f;
	return { ref, pixels / span * shrink, pixels };
}

// The icons are Font Awesome's, in the private use area: drawn with the icon
// font, the rest with the face asked for.
bool isIcon(uint32_t c)
{
	return c >= 0xE000 && c <= 0xF8FF;
}

uint32_t nextCodepoint(const std::string& s, size_t& i)
{
	const unsigned char c = (unsigned char)s[i];
	uint32_t cp = c;
	int more = 0;
	if (c >= 0xF0)
	{
		cp = c & 0x07;
		more = 3;
	}
	else if (c >= 0xE0)
	{
		cp = c & 0x0F;
		more = 2;
	}
	else if (c >= 0xC0)
	{
		cp = c & 0x1F;
		more = 1;
	}
	i++;
	while (more-- > 0 && i < s.size())
		cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
	return cp;
}

// Runs of text and of icons.
template <typename Fn>
void eachRun(const std::string& text, Fn fn)
{
	size_t i = 0, runStart = 0;
	bool runIcon = false, first = true;
	while (i < text.size())
	{
		const size_t at = i;
		const bool icon = isIcon(nextCodepoint(text, i));
		if (first)
		{
			runIcon = icon;
			first = false;
		}
		else if (icon != runIcon)
		{
			fn(std::string_view(text).substr(runStart, at - runStart), runIcon);
			runStart = at;
			runIcon = icon;
		}
	}
	if (!first)
		fn(std::string_view(text).substr(runStart), runIcon);
}

// An icon is drawn a little smaller than the text's line, and takes at least
// the line's height of width, centred in it.
float iconSize(const Face& face)
{
	const gfx::Font& f = *iconFont.font;
	return face.pixels * 0.82f / std::max(f.ascent(1.f) + f.descent(1.f), 0.5f);
}

float runWidth(std::string_view run, bool icon, const Face& face)
{
	if (!icon)
		return face.ref->font->measure(run, face.size);
	float width = 0;
	const std::string s(run);
	size_t i = 0;
	while (i < s.size())
	{
		const size_t at = i;
		nextCodepoint(s, i);
		width += std::max(iconFont.font->measure(std::string_view(s).substr(at, i - at), iconSize(face)), face.pixels);
	}
	return width;
}

float textWidth(const std::string& text, const Face& face)
{
	float width = 0;
	eachRun(text, [&](std::string_view run, bool icon) { width += runWidth(run, icon, face); });
	return width;
}

// Draws a line with its top at y; returns its width.
float drawLine(gfx::DrawList& list, float x, float y, ImU32 colour, const std::string& text, const Face& face)
{
	const gfx::Color c = colourOf(colour);
	const float baseline = y + face.ref->font->ascent(face.size);
	float pen = x;
	eachRun(text, [&](std::string_view run, bool icon) {
		if (!icon)
		{
			pen += hui::ui::text(list, *face.ref, run, pen, baseline, face.size, c);
			return;
		}
		const float size = iconSize(face);
		const std::string s(run);
		size_t i = 0;
		while (i < s.size())
		{
			const size_t at = i;
			nextCodepoint(s, i);
			const std::string_view glyph = std::string_view(s).substr(at, i - at);
			const float w = iconFont.font->measure(glyph, size);
			const float advance = std::max(w, face.pixels);
			// Set on the line's middle.
			const float iconBaseline = y + (face.pixels + iconFont.font->ascent(size) - iconFont.font->descent(size)) * 0.5f;
			hui::ui::text(list, iconFont, glyph, pen + (advance - w) * 0.5f, iconBaseline, size, c);
			pen += advance;
		}
	});
	return pen - x;
}

// The lines of text wrapped to a width.
std::vector<std::string> wrapLines(const std::string& text, float wrap, const Face& face)
{
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= text.size())
	{
		const size_t newline = text.find('\n', start);
		const std::string paragraph = text.substr(start, newline == std::string::npos ? std::string::npos : newline - start);
		std::string line;
		size_t i = 0;
		while (i < paragraph.size())
		{
			size_t end = paragraph.find(' ', i);
			if (end == std::string::npos)
				end = paragraph.size();
			const std::string word = paragraph.substr(i, end - i);
			const std::string trial = line.empty() ? word : line + " " + word;
			if (!line.empty() && textWidth(trial, face) > wrap)
			{
				lines.push_back(line);
				line = word;
			}
			else
				line = trial;
			// A word longer than the line is cut where it fills it.
			while (textWidth(line, face) > wrap && line.size() > 1)
			{
				size_t cut = line.size();
				while (cut > 1 && textWidth(line.substr(0, cut), face) > wrap)
				{
					cut--;
					while (cut > 0 && ((unsigned char)line[cut] & 0xC0) == 0x80)
						cut--;
				}
				if (cut == 0)
					break;
				lines.push_back(line.substr(0, cut));
				line = line.substr(cut);
			}
			i = end + 1;
		}
		lines.push_back(line);
		if (newline == std::string::npos)
			break;
		start = newline + 1;
	}
	return lines;
}

// The longest start of `text` that fits `maxWidth`, whole letters.
size_t fitLength(const std::string& text, float maxWidth, const Face& face)
{
	size_t length = text.size();
	while (length > 0 && textWidth(text.substr(0, length), face) > maxWidth)
	{
		length--;
		while (length > 0 && ((unsigned char)text[length] & 0xC0) == 0x80)
			length--;
	}
	return length;
}

hui::ui::Painter painter()
{
	return hui::ui::Painter(frontList, kitFonts, *kitTheme, display::interfaceRenderer().glass_texture());
}

} // namespace

} // namespace fe::ui

// ------------------------------------------------------------ the draw list

using fe::ui::colourOf;
using fe::ui::rectOf;
namespace gfx = hui::gfx;


void ImDrawList::AddRectFilled(ImVec2 a, ImVec2 b, ImU32 col, float rounding, ImDrawFlags flags)
{
	if (scene != nullptr)
	{
		AddQuadFilled(a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y), col);
		return;
	}
	const gfx::Rect r = rectOf(a, b);
	const float radius = std::min(rounding, std::min(r.w, r.h) * 0.5f);
	kit->rounded_rect(r, radius, colourOf(col));
	// Only some corners round: the others are filled square again.
	const int corners = flags & ImDrawFlags_RoundCornersAll;
	if (radius > 0 && corners != 0 && corners != ImDrawFlags_RoundCornersAll)
	{
		const gfx::Color c = colourOf(col);
		if (!(corners & ImDrawFlags_RoundCornersTopLeft))
			kit->rounded_rect({ r.x, r.y, radius, radius }, 0, c);
		if (!(corners & ImDrawFlags_RoundCornersTopRight))
			kit->rounded_rect({ r.x + r.w - radius, r.y, radius, radius }, 0, c);
		if (!(corners & ImDrawFlags_RoundCornersBottomLeft))
			kit->rounded_rect({ r.x, r.y + r.h - radius, radius, radius }, 0, c);
		if (!(corners & ImDrawFlags_RoundCornersBottomRight))
			kit->rounded_rect({ r.x + r.w - radius, r.y + r.h - radius, radius, radius }, 0, c);
	}
}

void ImDrawList::AddRect(ImVec2 a, ImVec2 b, ImU32 col, float rounding, ImDrawFlags, float thickness)
{
	if (scene != nullptr)
	{
		const ImVec2 p[4] = { a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y) };
		AddPolyline(p, 4, col, ImDrawFlags_Closed, thickness);
		return;
	}
	const gfx::Rect r = rectOf(a, b);
	kit->bordered_rect(r, std::min(rounding, std::min(r.w, r.h) * 0.5f), gfx::Color{ 0, 0, 0, 0 }, thickness,
			colourOf(col));
}

void ImDrawList::AddRectFilledMultiColor(ImVec2 a, ImVec2 b, ImU32 ul, ImU32 ur, ImU32 br, ImU32 bl)
{
	if (scene != nullptr)
	{
		AddQuadFilled(a, ImVec2(b.x, a.y), b, ImVec2(a.x, b.y), ul);
		return;
	}
	const gfx::Rect r = rectOf(a, b);
	if (ul == bl && ur == br && ul != ur)
		kit->gradient_rect_h(r, 0, colourOf(ul), colourOf(ur));
	else
		kit->gradient_rect(r, 0, gfx::mix(colourOf(ul), colourOf(ur), 0.5f), gfx::mix(colourOf(bl), colourOf(br), 0.5f));
}

void ImDrawList::AddLine(ImVec2 a, ImVec2 b, ImU32 col, float thickness)
{
	if (scene != nullptr)
	{
		const float dx = b.x - a.x, dy = b.y - a.y, len = std::max(std::sqrt(dx * dx + dy * dy), 0.001f);
		const float nx = -dy / len * thickness * 0.5f, ny = dx / len * thickness * 0.5f;
		AddQuadFilled(ImVec2(a.x + nx, a.y + ny), ImVec2(b.x + nx, b.y + ny), ImVec2(b.x - nx, b.y - ny),
				ImVec2(a.x - nx, a.y - ny), col);
		return;
	}
	kit->line(a.x, a.y, b.x, b.y, thickness, colourOf(col));
}

void ImDrawList::AddPolyline(const ImVec2 *points, int count, ImU32 col, ImDrawFlags flags, float thickness)
{
	for (int i = 0; i + 1 < count; i++)
		AddLine(points[i], points[i + 1], col, thickness);
	if ((flags & ImDrawFlags_Closed) && count > 2)
		AddLine(points[count - 1], points[0], col, thickness);
}

void ImDrawList::AddCircle(ImVec2 centre, float radius, ImU32 col, int, float thickness)
{
	if (scene != nullptr)
	{
		constexpr int Steps = 32;
		ImVec2 p[Steps];
		for (int i = 0; i < Steps; i++)
			p[i] = ImVec2(centre.x + std::cos(i * 6.2831853f / Steps) * radius, centre.y + std::sin(i * 6.2831853f / Steps) * radius);
		AddPolyline(p, Steps, col, ImDrawFlags_Closed, thickness);
		return;
	}
	kit->ring(centre.x, centre.y, radius, thickness, colourOf(col));
}

void ImDrawList::AddCircleFilled(ImVec2 centre, float radius, ImU32 col, int)
{
	if (scene != nullptr)
	{
		constexpr int Steps = 32;
		ImVec2 p[Steps];
		for (int i = 0; i < Steps; i++)
			p[i] = ImVec2(centre.x + std::cos(i * 6.2831853f / Steps) * radius, centre.y + std::sin(i * 6.2831853f / Steps) * radius);
		AddConvexPolyFilled(p, Steps, col);
		return;
	}
	kit->circle(centre.x, centre.y, radius, colourOf(col));
}

void ImDrawList::AddTriangle(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 col, float thickness)
{
	const ImVec2 p[3] = { a, b, c };
	AddPolyline(p, 3, col, ImDrawFlags_Closed, thickness);
}

void ImDrawList::AddTriangleFilled(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 col)
{
	const ImVec2 p[3] = { a, b, c };
	AddConvexPolyFilled(p, 3, col);
}

void ImDrawList::AddQuadFilled(ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImU32 col)
{
	const ImVec2 p[4] = { a, b, c, d };
	AddConvexPolyFilled(p, 4, col);
}

void ImDrawList::AddConvexPolyFilled(const ImVec2 *points, int count, ImU32 col)
{
	if (count < 3)
		return;
	if (scene != nullptr)
	{
		for (int i = 1; i + 1 < count; i++)
			sceneTriangle(points[0], points[i], points[i + 1], col);
		return;
	}
	std::vector<float> xy((size_t)count * 2);
	for (int i = 0; i < count; i++)
	{
		xy[(size_t)i * 2] = points[i].x;
		xy[(size_t)i * 2 + 1] = points[i].y;
	}
	kit->polygon(xy.data(), count, colourOf(col));
}

void ImDrawList::AddConcavePolyFilled(const ImVec2 *points, int count, ImU32 col)
{
	if (count < 3)
		return;
	if (scene != nullptr)
	{
		AddConvexPolyFilled(points, count, col);
		return;
	}
	// The kit's polygons are triangulated by ear clipping: concave is fine.
	std::vector<float> xy((size_t)count * 2);
	for (int i = 0; i < count; i++)
	{
		xy[(size_t)i * 2] = points[i].x;
		xy[(size_t)i * 2 + 1] = points[i].y;
	}
	kit->polygon(xy.data(), count, colourOf(col));
}

void ImDrawList::AddEllipseFilled(ImVec2 centre, ImVec2 radius, ImU32 col, float rotation, int)
{
	constexpr int Steps = 40;
	ImVec2 p[Steps];
	const float c = std::cos(rotation), sn = std::sin(rotation);
	for (int i = 0; i < Steps; i++)
	{
		const float a = (float)i / Steps * 6.2831853f;
		const float x = std::cos(a) * radius.x, y = std::sin(a) * radius.y;
		p[i] = ImVec2(centre.x + x * c - y * sn, centre.y + x * sn + y * c);
	}
	AddConvexPolyFilled(p, Steps, col);
}

void ImDrawList::AddImage(ImTextureID texture, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1, ImU32 col)
{
	AddImageRounded(texture, a, b, uv0, uv1, col, 0.f);
}

void ImDrawList::AddImageRounded(ImTextureID texture, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1, ImU32 col,
		float rounding, ImDrawFlags)
{
	if (texture == nullptr)
		return;
	if (scene != nullptr)
	{
		PushTexture(texture);
		PrimReserve(6, 4);
		const unsigned first = _VtxCurrentIdx;
		PrimWriteVtx(a, uv0, col);
		PrimWriteVtx(ImVec2(b.x, a.y), ImVec2(uv1.x, uv0.y), col);
		PrimWriteVtx(b, uv1, col);
		PrimWriteVtx(ImVec2(a.x, b.y), ImVec2(uv0.x, uv1.y), col);
		for (unsigned i : { 0u, 1u, 2u, 0u, 2u, 3u })
			PrimWriteIdx(first + i);
		PopTexture();
		return;
	}
	const uint32_t handle = fe::display::kitTexture(texture);
	if (handle == 0)
		return;
	kit->image(handle, rectOf(a, b), { uv0.x, uv0.y, uv1.x - uv0.x, uv1.y - uv0.y }, colourOf(col), rounding);
}

void ImDrawList::AddImageRotated(ImTextureID texture, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1, ImU32 col,
		float rounding, float angle)
{
	if (kit == nullptr)
	{
		AddImageRounded(texture, a, b, uv0, uv1, col, rounding);
		return;
	}
	const uint32_t handle = fe::display::kitTexture(texture);
	if (handle == 0)
		return;
	kit->image(handle, rectOf(a, b), { uv0.x, uv0.y, uv1.x - uv0.x, uv1.y - uv0.y }, colourOf(col), rounding, angle);
}

void ImDrawList::AddImageGradient(ImTextureID texture, ImVec2 a, ImVec2 b, ImVec2 uv0, ImVec2 uv1, ImU32 top,
		ImU32 bottom, float rounding)
{
	if (kit == nullptr)
	{
		AddImageRounded(texture, a, b, uv0, uv1, top, rounding);
		return;
	}
	const uint32_t handle = fe::display::kitTexture(texture);
	if (handle == 0)
		return;
	kit->image_gradient(handle, rectOf(a, b), { uv0.x, uv0.y, uv1.x - uv0.x, uv1.y - uv0.y }, colourOf(top),
			colourOf(bottom), rounding);
}

void ImDrawList::PushClipRect(ImVec2 a, ImVec2 b, bool)
{
	if (kit != nullptr)
		kit->push_clip(rectOf(a, b));
}

void ImDrawList::PopClipRect()
{
	if (kit != nullptr)
		kit->pop_clip();
}

void ImDrawList::PathLineTo(ImVec2 point)
{
	path_.push_back(point);
}

void ImDrawList::PathStroke(ImU32 col, float thickness, ImDrawFlags flags)
{
	AddPolyline(path_.data(), (int)path_.size(), col, flags, thickness);
	path_.clear();
}

void ImDrawList::PushTexture(ImTextureID texture)
{
	if (scene != nullptr)
		scene->textures.push_back(texture);
}

void ImDrawList::PopTexture()
{
	if (scene != nullptr && !scene->textures.empty())
		scene->textures.pop_back();
}

void ImDrawList::PrimReserve(int, int)
{
	if (scene == nullptr)
		return;
	_VtxCurrentIdx = (unsigned)scene->vertices.size();
	void *texture = scene->textures.empty() ? nullptr : scene->textures.back();
	if (scene->runs.empty() || scene->runs.back().texture != texture)
		scene->runs.push_back({ texture, (uint32_t)scene->indices.size(), 0 });
}

void ImDrawList::PrimWriteVtx(ImVec2 pos, ImVec2 uv, ImU32 col)
{
	if (scene != nullptr)
		scene->vertices.push_back({ pos, uv, col });
}

void ImDrawList::PrimWriteIdx(ImDrawIdx index)
{
	if (scene == nullptr)
		return;
	scene->indices.push_back(index);
	scene->runs.back().count++;
}

void ImDrawList::sceneTriangle(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 col)
{
	if (scene->runs.empty() || scene->runs.back().texture != nullptr)
		scene->runs.push_back({ nullptr, (uint32_t)scene->indices.size(), 0 });
	const uint32_t first = (uint32_t)scene->vertices.size();
	scene->vertices.push_back({ a, ImVec2(0, 0), col });
	scene->vertices.push_back({ b, ImVec2(0, 0), col });
	scene->vertices.push_back({ c, ImVec2(0, 0), col });
	for (uint32_t i = 0; i < 3; i++)
		scene->indices.push_back(first + i);
	scene->runs.back().count += 3;
}

namespace fe::ui
{

// ------------------------------------------------------------------ themes

const char *accentName(int index)
{
	return accents[std::clamp(index, 0, AccentCount - 1)].name;
}

std::vector<std::string> themeNames()
{
	std::vector<std::string> names = { "PSSwanStation" };
	for (const hui::ui::Theme& t : hui::ui::themes())
		names.push_back(t.name);
	return names;
}

std::string themeAbout(int index)
{
	const auto all = hui::ui::themes();
	if (index <= 0 || index > (int)all.size())
		return "The title's own: deep blue panels on the water, in the accent colour you choose.";
	const hui::ui::Theme& t = all[(size_t)index - 1];
	return std::string(t.family) + ". " + t.summary + ".";
}

int themeSounds()
{
	if (kitTheme == nullptr || kitTheme == &swanTheme)
		return 1;
	return kitTheme->sounds == hui::audio::SoundSet::paper ? 3 : 2;
}

const char *themeId()
{
	return kitTheme != nullptr ? kitTheme->id : "psswanstation";
}

const Theme& theme()
{
	return current;
}

const hui::ui::Theme& kitThemeNow()
{
	return *kitTheme;
}

const hui::ui::Fonts& kitFontsNow()
{
	return kitFonts;
}

// --------------------------------------------------------------- the frame

void widgetsInit()
{
	started = now();
	const std::pair<const unsigned char *, const unsigned char *> files[7] = {
		{ fe_hui_regular, fe_hui_regular_end }, { fe_hui_semibold, fe_hui_semibold_end },
		{ fe_hui_display, fe_hui_display_end }, { fe_hui_mono, fe_hui_mono_end },
		{ fe_hui_pixel, fe_hui_pixel_end }, { fe_hui_hand, fe_hui_hand_end }, { fe_hui_icons, fe_hui_icons_end },
	};
	hui::ui::FontRef *refs[7] = { &kitFonts.regular, &kitFonts.semibold, &kitFonts.display, &kitFonts.mono,
		&kitFonts.pixel, &kitFonts.hand, &iconFont };
	gfx::VkRenderer& renderer = display::interfaceRenderer();
	fontsReady = true;
	for (int i = 0; i < 7; i++)
	{
		const std::string bytes((const char *)files[i].first, (size_t)(files[i].second - files[i].first));
		if (!faces[i].load(bytes))
		{
			diag::mark("ui: font %d: %s", i, faces[i].error().c_str());
			fontsReady = false;
			continue;
		}
		refs[i]->font = &faces[i];
		refs[i]->texture = renderer.create_font_texture(faces[i]);
	}
	backShim.kit = &backList;
	frontShim.kit = &frontList;
	sceneShim.scene = &sceneMesh;
	diag::mark("ui: fonts ready (the interface kit's, %d themes)", (int)hui::ui::themes().size() + 1);
	logoImage = imageFromMemory(fe_logo_box_png, (size_t)(fe_logo_box_png_end - fe_logo_box_png));
	widgetsFrame();
}

void widgetsFrame()
{
	const double t = now();
	frameDt = lastFrameAt > 0 ? (float)std::clamp(t - lastFrameAt, 0.0, 0.1) : 1.f / 60.f;
	lastFrameAt = t;
	backList.clear();
	frontList.clear();
	sceneMesh.clear();
	inScene = false;
	wantBackdrop = false;

	const options::Frontend& settings = options::frontend();
	unit = display::scale() * (float)std::clamp(settings.uiScale, 70, 140) / 100.f;
	const ImU32 accent = accents[std::clamp(settings.accent, 0, AccentCount - 1)].colour;
	makeSwanTheme(accent);
	const auto all = hui::ui::themes();
	const int chosen = settings.theme;
	kitTheme = chosen > 0 && chosen <= (int)all.size() ? &all[(size_t)chosen - 1] : &swanTheme;
	const hui::ui::Theme& k = *kitTheme;

	current.background = u32Of(k.page);
	current.backgroundLow = u32Of(gfx::mix(k.page, gfx::Color{ 0, 0, 0, 1 }, k.dark ? 0.45f : 0.08f));
	current.panel = u32Of(k.surface.a < 0.85f ? k.surface : k.surface.with_alpha(0.94f));
	current.panelHigh = u32Of(k.surface_high);
	current.accent = u32Of(k.accent);
	current.accentSoft = u32Of(k.accent.with_alpha(0.22f));
	current.onAccent = u32Of(hui::ui::Painter::on(k.accent));
	current.text = u32Of(k.text);
	current.dim = u32Of(k.text_muted);
	current.faint = u32Of(gfx::mix(k.text_muted, k.page, 0.35f));
	current.pageText = u32Of(k.page_text.a > 0 ? k.page_text : k.text);
	current.pageDim = u32Of(k.page_text_muted.a > 0 ? k.page_text_muted : k.text_muted);
	current.good = u32Of(k.success);
	current.bad = u32Of(k.danger);
	current.dark = k.dark;
	if (settings.highContrast)
	{
		current.background = k.dark ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
		current.backgroundLow = current.background;
		current.panel = k.dark ? IM_COL32(10, 12, 18, 250) : IM_COL32(245, 245, 245, 250);
		current.accentSoft = withAlpha(current.accent, 0.42f);
		current.text = current.pageText = k.dark ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
		current.dim = current.pageDim = k.dark ? IM_COL32(214, 220, 232, 255) : IM_COL32(30, 30, 30, 255);
		current.faint = k.dark ? IM_COL32(168, 176, 196, 255) : IM_COL32(70, 70, 70, 255);
	}
	// Okabe and Ito's sky blue and orange, told apart with every common
	// colour blindness.
	if (settings.colourBlind)
	{
		current.good = IM_COL32(86, 180, 233, 255);
		current.bad = IM_COL32(230, 159, 0, 255);
	}
}

void widgetsSubmit()
{
	gfx::VkRenderer& renderer = display::interfaceRenderer();
	renderer.begin();
	if (wantBackdrop && kitTheme->backdrop.mode != gfx::BackdropMode::none && motion() != MotionOff)
	{
		gfx::BackdropSpec spec = kitTheme->backdrop;
		spec.time = (float)clock();
		renderer.backdrop(spec);
	}
	else if (wantBackdrop && kitTheme->backdrop.mode != gfx::BackdropMode::none)
	{
		gfx::BackdropSpec spec = kitTheme->backdrop;
		spec.time = 0;
		renderer.backdrop(spec);
	}
	renderer.draw(backList);
	// The frosted copy only for themes whose panels are glass.
	if (kitTheme->style == hui::ui::SurfaceStyle::glass)
		renderer.glass();
	renderer.draw(frontList);
}

float px(float units)
{
	return std::floor(units * unit + 0.5f);
}

ImDrawList *draw()
{
	return inScene ? &sceneShim : &frontShim;
}

ImDrawList *background()
{
	return &backShim;
}

void beginScene()
{
	sceneMesh.clear();
	inScene = true;
}

const Scene& endScene()
{
	inScene = false;
	return sceneMesh;
}

float width()
{
	return (float)display::width();
}

float height()
{
	return (float)display::height();
}

double clock()
{
#if defined(SWANSTATION_HOST)
	// A test run can ask for the animations' clock to count frames, so a
	// screenshot at a frame shows the same moment however slow the PC draws.
	static const bool byFrame = getenv("SWANSTATION_FRAME_CLOCK") != nullptr;
	if (byFrame)
		return (double)display::frameCount() / 60.0;
#endif
	return now() - started;
}

float frameTime()
{
#if defined(SWANSTATION_HOST)
	static const bool byFrame = getenv("SWANSTATION_FRAME_CLOCK") != nullptr;
	if (byFrame)
		return 1.f / 60.f;
#endif
	return frameDt;
}

float approach(float value, float target, float speed)
{
	// With the animations off, things are where they are going at once.
	if (motion() == MotionOff)
		return target;
	const float next = value + (target - value) * (1.f - std::exp(-speed * frameTime()));
	return std::fabs(target - next) < 0.01f ? target : next;
}

ImU32 withAlpha(ImU32 colour, float alpha)
{
	const ImU32 a = (ImU32)(std::clamp(alpha, 0.f, 1.f) * (float)(colour >> 24) + 0.5f);
	return (colour & 0x00FFFFFF) | (a << 24);
}

ImU32 mix(ImU32 a, ImU32 b, float t)
{
	t = std::clamp(t, 0.f, 1.f);
	ImU32 out = 0;
	for (int shift = 0; shift < 32; shift += 8)
	{
		const float ca = (float)((a >> shift) & 0xFF), cb = (float)((b >> shift) & 0xFF);
		out |= (ImU32)(ca + (cb - ca) * t + 0.5f) << shift;
	}
	return out;
}

// -------------------------------------------------------------------- text

ImVec2 measure(const std::string& text, Font font, float size)
{
	if (!fontsReady)
		return ImVec2(0, px(size));
	const Face face = faceOf(font, size);
	int lines = 1;
	float widest = 0;
	size_t start = 0;
	for (;;)
	{
		const size_t end = text.find('\n', start);
		widest = std::max(widest, textWidth(text.substr(start, end == std::string::npos ? std::string::npos : end - start), face));
		if (end == std::string::npos)
			break;
		lines++;
		start = end + 1;
	}
	return ImVec2(widest, face.pixels * (float)lines);
}

void text(ImVec2 at, ImU32 colour, const std::string& value, Font font, float size)
{
	if (value.empty() || !fontsReady)
		return;
	if (inScene)
		return;	// a scene is a mesh; its words are drawn flat over it
	const Face face = faceOf(font, size);
	float y = std::floor(at.y);
	size_t start = 0;
	for (;;)
	{
		const size_t end = value.find('\n', start);
		drawLine(frontList, std::floor(at.x), y, colour, value.substr(start, end == std::string::npos ? std::string::npos : end - start),
				face);
		if (end == std::string::npos)
			break;
		y += face.pixels;
		start = end + 1;
	}
}

float textFit(ImVec2 at, float maxWidth, ImU32 colour, const std::string& value, Font font, float size)
{
	const float whole = measure(value, font, size).x;
	if (whole <= maxWidth)
	{
		text(at, colour, value, font, size);
		return whole;
	}
	static const std::string dots = "\xe2\x80\xa6";
	const Face face = faceOf(font, size);
	const float dotsWidth = textWidth(dots, face);
	std::string cut = value.substr(0, fitLength(value, std::max(maxWidth - dotsWidth, 0.f), face));
	while (!cut.empty() && cut.back() == ' ')
		cut.pop_back();
	cut += dots;
	text(at, colour, cut, font, size);
	return textWidth(cut, face);
}

void textRight(ImVec2 at, ImU32 colour, const std::string& value, Font font, float size)
{
	text(ImVec2(at.x - measure(value, font, size).x, at.y), colour, value, font, size);
}

void textCentred(ImVec2 at, ImU32 colour, const std::string& value, Font font, float size)
{
	text(ImVec2(at.x - measure(value, font, size).x * 0.5f, at.y), colour, value, font, size);
}

float textWrapped(ImVec2 at, float wrapWidth, ImU32 colour, const std::string& value, Font font, float size,
		float maxHeight)
{
	if (value.empty() || !fontsReady)
		return 0;
	const Face face = faceOf(font, size);
	const std::vector<std::string> lines = wrapLines(value, wrapWidth, face);
	float y = std::floor(at.y);
	const float bottom = maxHeight > 0 ? at.y + maxHeight : 1e9f;
	for (const std::string& line : lines)
	{
		if (y + face.pixels > bottom + 0.5f)
			break;
		if (!inScene)
			drawLine(frontList, std::floor(at.x), y, colour, line, face);
		y += face.pixels;
	}
	const float total = face.pixels * (float)lines.size();
	return maxHeight > 0 ? std::min(total, maxHeight) : total;
}

float wrappedHeight(const std::string& value, float wrapWidth, Font font, float size)
{
	if (value.empty() || !fontsReady)
		return 0;
	const Face face = faceOf(font, size);
	return face.pixels * (float)wrapLines(value, wrapWidth, face).size();
}

std::vector<std::string> wrapText(const std::string& value, float wrapWidth, Font font, float size)
{
	if (!fontsReady)
		return { value };
	return wrapLines(value, wrapWidth, faceOf(font, size));
}

// ------------------------------------------------------------------ pieces

void panel(ImVec2 a, ImVec2 b, ImU32 colour, float rounding)
{
	if (inScene)
	{
		draw()->AddRectFilled(a, b, colour, px(rounding));
		return;
	}
	const gfx::Rect r = rectOf(a, b);
	const float radius = std::min(px(rounding), std::min(r.w, r.h) * 0.5f);
	// A page's own panel colour is a surface in the theme's style (glass, a
	// soft shadow, a hard outline...); any other colour is filled as it is.
	if (colour == current.panel)
	{
		hui::ui::Painter paint = painter();
		paint.surface(r, radius, kitTheme->surface, kitTheme->outline, 1.f);
		return;
	}
	if (colour == current.panelHigh)
	{
		hui::ui::Painter paint = painter();
		paint.fill(r, radius, kitTheme->surface_high);
		return;
	}
	frontList.rounded_rect(r, radius, colourOf(colour));
}

void outline(ImVec2 a, ImVec2 b, ImU32 colour, float rounding, float thickness)
{
	draw()->AddRect(a, b, colour, px(rounding), 0, px(thickness));
}

void focusRing(ImVec2 a, ImVec2 b, float rounding, float amount)
{
	if (inScene || amount <= 0.004f)
		return;
	hui::ui::Painter paint = painter();
	paint.focus_ring(rectOf(a, b), px(rounding), std::clamp(amount, 0.f, 1.f));
}

void heading(ImVec2 at, const std::string& value, float size, ImU32 colour)
{
	text(at, colour, value, Title, size);
}

void backdrop()
{
	// The theme's backdrop, a layer of the kit's under everything; the page's
	// colour under it in case it has none.
	wantBackdrop = true;
	backList.rounded_rect({ 0, 0, width(), height() }, 0, colourOf(current.background));
	if (kitTheme->backdrop.mode == gfx::BackdropMode::none)
		backList.gradient_rect({ 0, 0, width(), height() }, 0, colourOf(current.background), colourOf(current.backgroundLow));
}

bool backdropDrawn()
{
	return wantBackdrop;
}

void wash(const Image& image, float alpha)
{
	if (image.soft == nullptr || alpha <= 0.004f)
		return;
	// The blurred miniature, over the whole screen and a little beyond, so
	// its edge pixels are not the screen's edges.
	const float w = width(), h = height();
	backShim.AddImage((ImTextureID)image.soft, ImVec2(-w * 0.08f, -h * 0.08f), ImVec2(w * 1.08f, h * 1.08f), ImVec2(0, 0),
			ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(std::clamp(alpha, 0.f, 1.f) * 255)));
}

void waves(float alpha)
{
	const float w = width(), h = height();
	const float t = (float)clock();
	static const float rows[3] = { 0.800f, 0.850f, 0.900f };
	static const float strength[3] = { 0.27f, 0.19f, 0.12f };
	const ImU32 ink = kitTheme == &swanTheme ? IM_COL32(150, 200, 255, 255) : current.accent;
	for (int row = 0; row < 3; row++)
	{
		const float y = h * rows[row];
		for (float x = 0; x <= w; x += px(8))
			backShim.PathLineTo(ImVec2(x, y + std::sin(x / w * 3.14159f * 9.f + t * (0.9f + 0.25f * (float)row)) * h * 0.006f));
		backShim.PathStroke(withAlpha(ink, strength[row] * std::clamp(alpha, 0.f, 1.f)), std::max(h / 360.f, 2.f));
	}
}

void glowAt(ImVec2 centre, float radius, ImU32 colour)
{
	backList.shadow({ centre.x - radius * 0.35f, centre.y - radius * 0.35f, radius * 0.7f, radius * 0.7f }, radius * 0.35f,
			radius * 0.65f, colourOf(colour));
}

void logoBox(ImVec2 a, ImVec2 b, float alpha)
{
	if (logoImage.id == nullptr || alpha <= 0.004f)
		return;
	draw()->AddImage((ImTextureID)logoImage.id, a, b, ImVec2(0, 0), ImVec2(1, 1),
			IM_COL32(255, 255, 255, (int)(std::clamp(alpha, 0.f, 1.f) * 255)));
}

void swanPlace(ImVec2 boxA, ImVec2 boxB, ImVec2& origin, float& size)
{
	// As ps5/tools/make-art.py puts the bird into the icon.
	const float side = boxB.x - boxA.x;
	origin = ImVec2(boxA.x + side * 0.04f, boxA.y + side * 0.04f);
	size = side * 0.92f;
}

Motion motion()
{
	return (Motion)std::clamp(options::frontend().animations, 0, 2);
}

float pill(ImVec2 at, const std::string& label, bool on, bool enabled, float height, float size)
{
	const ImVec2 extent = measure(label, Bold, size);
	const float h = px(height);
	const float w = extent.x + h;
	const gfx::Rect r{ at.x, at.y, w, h };
	hui::ui::Painter paint = painter();
	hui::ui::Look look;
	look.disabled = !enabled;
	look.focus = on ? 1.f : 0.f;
	paint.chip(r, "", on ? 1.f : 0.f, look);
	const ImU32 ink = !enabled ? current.faint : on ? u32Of(hui::ui::Painter::on(kitTheme->accent)) : current.dim;
	text(ImVec2(at.x + h * 0.5f, at.y + (h - extent.y) * 0.5f), ink, label, Bold, size);
	return w;
}

void buttonGlyph(ImVec2 centre, float size, uint32_t button)
{
	using namespace platform;
	gfx::DrawList& list = frontList;
	const float r = px(size) * 0.5f;
	const float stroke = std::max(px(size * 0.085f), 1.5f);
	const bool dark = kitTheme->dark;
	const gfx::Color body = dark ? gfx::Color::rgb(0x242a3c) : gfx::Color::rgb(0xffffff);
	const gfx::Color rim = dark ? gfx::Color::rgb(0x606c88) : gfx::Color::rgb(0x9aa2b4);
	const auto face = [&] {
		list.circle(centre.x, centre.y, r, body);
		list.ring(centre.x, centre.y, r, std::max(px(1.5f), 1.f), rim);
	};
	const float k = r * 0.44f;
	switch (button)
	{
	case Cross:
		face();
		list.line(centre.x - k, centre.y - k, centre.x + k, centre.y + k, stroke, gfx::Color::rgb(0x7cb2e8));
		list.line(centre.x + k, centre.y - k, centre.x - k, centre.y + k, stroke, gfx::Color::rgb(0x7cb2e8));
		return;
	case Circle:
		face();
		list.ring(centre.x, centre.y, r * 0.52f, stroke, gfx::Color::rgb(0xff6666));
		return;
	case Square:
		face();
		list.bordered_rect({ centre.x - k, centre.y - k, k * 2, k * 2 }, 0, gfx::Color{ 0, 0, 0, 0 }, stroke,
				gfx::Color::rgb(0xff69f8));
		return;
	case Triangle:
		face();
		list.triangle({ centre.x - r * 0.55f, centre.y - r * 0.56f, r * 1.1f, r * 0.98f }, gfx::Color::rgb(0x40e2a0), stroke);
		return;
	default:
		break;
	}
	// The others are a label on a key.
	const char *label = button == L1 ? "L1" : button == R1 ? "R1" : button == L2 ? "L2" : button == R2 ? "R2"
			: button == Options ? "OPTIONS" : button == (L1 | R1) ? "L1  R1" : button == (L2 | R2) ? "L2  R2"
			: button == (Left | Right) ? "" : button == TouchLeft ? "TOUCH L"
			: button == TouchRight ? "TOUCH R" : button == (TouchLeft | TouchRight) ? "TOUCH PAD" : "?";
	if (button == (Left | Right))
	{
		const float a = r * 0.5f;
		const ImU32 colour = dark ? IM_COL32(200, 208, 224, 255) : IM_COL32(60, 66, 80, 255);
		frontShim.AddTriangleFilled(ImVec2(centre.x - r * 0.25f, centre.y - a), ImVec2(centre.x - r * 0.25f, centre.y + a),
				ImVec2(centre.x - r * 1.0f, centre.y), colour);
		frontShim.AddTriangleFilled(ImVec2(centre.x + r * 0.25f, centre.y - a), ImVec2(centre.x + r * 0.25f, centre.y + a),
				ImVec2(centre.x + r * 1.0f, centre.y), colour);
		return;
	}
	const float textSize = size * 0.50f;
	const ImVec2 extent = measure(label, Bold, textSize);
	const float half = extent.x * 0.5f + r * 0.45f;
	list.bordered_rect({ centre.x - half, centre.y - r * 0.82f, half * 2, r * 1.64f }, r * 0.4f, body,
			std::max(px(1.5f), 1.f), rim);
	text(ImVec2(centre.x - extent.x * 0.5f, centre.y - extent.y * 0.5f), dark ? IM_COL32(220, 226, 238, 255) : IM_COL32(40, 44, 56, 255),
			label, Bold, textSize);
}

// How wide a button's symbol is, in pixels.
float glyphWidth(float size, uint32_t button)
{
	using namespace platform;
	if (button == Cross || button == Circle || button == Square || button == Triangle)
		return px(size);
	if (button == (Left | Right))
		return px(size);
	const char *label = button == L1 ? "L1" : button == R1 ? "R1" : button == L2 ? "L2" : button == R2 ? "R2"
			: button == Options ? "OPTIONS" : button == (L1 | R1) ? "L1  R1" : button == (L2 | R2) ? "L2  R2"
			: button == TouchLeft ? "TOUCH L" : button == TouchRight ? "TOUCH R"
			: button == (TouchLeft | TouchRight) ? "TOUCH PAD" : "?";
	return measure(label, Bold, size * 0.50f).x + px(size) * 0.45f;
}

void hintBar(const std::vector<Hint>& hints, const std::string& left)
{
	const float w = width(), h = height();
	const float barHeight = px(64);
	const gfx::Color bar = kitTheme->dark ? gfx::Color::rgb(0x080a12, 0.84f) : kitTheme->surface.with_alpha(0.92f);
	frontList.rounded_rect({ 0, h - barHeight, w, barHeight }, 0, bar);
	frontList.line(0, h - barHeight, w, h - barHeight, 1.f, kitTheme->outline.a > 0 ? kitTheme->outline : gfx::Color::rgb(0xffffff, 0.07f));
	const float middle = h - barHeight * 0.5f;
	const float glyph = 34, textSize = 22;
	float x = w - px(48);
	for (size_t i = hints.size(); i-- > 0;)
	{
		const Hint& hint = hints[i];
		const ImVec2 extent = measure(hint.label, Body, textSize);
		x -= extent.x;
		text(ImVec2(x, middle - extent.y * 0.5f), current.dim, hint.label, Body, textSize);
		const float gw = glyphWidth(glyph, hint.button);
		x -= px(10) + gw;
		buttonGlyph(ImVec2(x + gw * 0.5f, middle), glyph, hint.button);
		x -= px(34);
	}
	if (!left.empty())
		textFit(ImVec2(px(48), middle - measure(left, Body, textSize).y * 0.5f), std::max(x - px(96), 0.f), current.faint,
				left, Body, textSize);
}

void progressBar(ImVec2 a, ImVec2 b, float fraction)
{
	const gfx::Rect r = rectOf(a, b);
	hui::ui::Painter paint = painter();
	if (fraction >= 0)
	{
		paint.progress(r, std::clamp(fraction, 0.f, 1.f));
		return;
	}
	// No figure to show: a piece that runs along the bar.
	const float radius = r.h * 0.5f;
	paint.well(r, radius, kitTheme->surface_high);
	const float span = r.w, piece = span * 0.25f;
	const float at = (float)std::fmod(clock() * 0.6, 1.0) * (span + piece) - piece;
	const float x0 = r.x + std::max(at, 0.f), x1 = r.x + std::min(at + piece, span);
	if (x1 > x0)
		frontList.rounded_rect({ x0, r.y, x1 - x0, r.h }, std::min(radius, (x1 - x0) * 0.5f), kitTheme->accent);
}

// ------------------------------------------------------- the disc drive, alive

namespace
{
ImU32 rgba(float r, float g, float b, float a)
{
	const auto c = [](float v) { return (unsigned)std::clamp(v * 255.f + 0.5f, 0.f, 255.f); };
	return IM_COL32(c(r), c(g), c(b), c(a));
}

// A rainbow's colour at h (0 to 1), as a CD's surface throws it.
ImU32 rainbow(float h, float alpha)
{
	h = h - std::floor(h);
	const float r = std::clamp(std::fabs(h * 6.f - 3.f) - 1.f, 0.f, 1.f);
	const float g = std::clamp(2.f - std::fabs(h * 6.f - 2.f), 0.f, 1.f);
	const float b = std::clamp(2.f - std::fabs(h * 6.f - 4.f), 0.f, 1.f);
	return rgba(0.55f + 0.45f * r, 0.55f + 0.45f * g, 0.55f + 0.45f * b, alpha);
}

// A piece of a ring, from angle a0 to a1 (radians), as quads.
void ringPiece(ImDrawList *list, ImVec2 c, float r0, float r1, float a0, float a1, ImU32 col, int steps = 6)
{
	for (int i = 0; i < steps; i++)
	{
		const float s0 = a0 + (a1 - a0) * (float)i / (float)steps, s1 = a0 + (a1 - a0) * (float)(i + 1) / (float)steps;
		const ImVec2 p0(c.x + std::cos(s0) * r0, c.y + std::sin(s0) * r0), p1(c.x + std::cos(s0) * r1, c.y + std::sin(s0) * r1);
		const ImVec2 p2(c.x + std::cos(s1) * r1, c.y + std::sin(s1) * r1), p3(c.x + std::cos(s1) * r0, c.y + std::sin(s1) * r0);
		list->AddQuadFilled(p0, p1, p2, p3, col);
	}
}

// When each disc was first seen (by its path), for the drop it arrives with.
std::map<std::string, double> discArrived;
// How far each drive's disc has turned, and how fast it turns now (turns a
// second), so a change of pace eases instead of jumping.
struct Turning
{
	double angle = 0, rate = 0, last = 0;
};
std::map<std::string, Turning> turning;
}

bool driveArt(ImVec2 a, ImVec2 b, const std::string& path, const std::string& name, const Image& cover, float alpha,
		bool withName)
{
	if (inScene || !discdrive::isDrivePath(path))
		return false;
	discdrive::Disc disc;
	const bool known = discdrive::discOf(path, disc);
	enum { NoDrive, Empty, HasDisc } state = !known ? Empty : disc.device == discdrive::NoDrive ? NoDrive
			: disc.playable ? HasDisc : Empty;
	const Theme& t = theme();
	ImDrawList *list = draw();
	const double now = clock();
	const bool moving = motion() == MotionFull;
	const bool some = motion() != MotionOff;
	const float w = b.x - a.x, h = b.y - a.y, s = std::min(w, h);
	const float rounding = std::min(px(12), s * 0.06f);

	// Behind: the cover, softly, or the theme's dark gradient with the accent in it.
	const gfx::Rect r = rectOf(a, b);
	if (cover.id != nullptr && state == HasDisc)
	{
		list->AddImageRounded(cover.soft != nullptr ? cover.soft : cover.id, a, b, ImVec2(0, 0), ImVec2(1, 1),
				withAlpha(IM_COL32_WHITE, alpha), rounding);
		list->AddRectFilled(a, b, withAlpha(IM_COL32(6, 8, 14, 255), 0.55f * alpha), rounding);
	}
	else
	{
		gfx::Color top = colourOf(t.accent), bottom = colourOf(t.background);
		top = { top.r * 0.30f + 0.06f, top.g * 0.30f + 0.06f, top.b * 0.30f + 0.09f, alpha };
		bottom = { bottom.r * 0.8f, bottom.g * 0.8f, bottom.b * 0.8f, alpha };
		frontList.gradient_rect(r, rounding, top, bottom);
	}
	list->PushClipRect(a, b, true);

	// The drive: a slim face across the lower part, its slot and its light.
	const float faceH = s * 0.16f;
	const float faceY0 = a.y + h * 0.70f, faceY1 = faceY0 + faceH;
	const float faceX0 = a.x + w * 0.5f - s * 0.40f, faceX1 = a.x + w * 0.5f + s * 0.40f;
	const float slotY = faceY0 + faceH * 0.40f;
	const int activity = state == HasDisc ? discdrive::activity() : discdrive::Resting;
	const float cx = a.x + w * 0.5f;

	// The disc's place: above the slot, half out of it.
	const float discR = s * 0.27f;
	float discY = slotY - discR * 0.92f;
	float discAlpha = 1.f;
	if (state == HasDisc)
	{
		// Arriving: it drops in from above, overshoots and settles.
		auto it = discArrived.find(path);
		if (it == discArrived.end())
			it = discArrived.emplace(path, now).first;
		const float since = (float)(now - it->second);
		if (moving && since < 1.2f)
		{
			const float k = since / 1.2f;
			const float spring = 1.f - std::exp(-6.f * k) * std::cos(10.f * k);
			discY -= (1.f - spring) * s * 0.75f;
			discAlpha = std::clamp(k * 3.f, 0.f, 1.f);
		}
		else if (moving)
			discY += std::sin((float)now * 1.3f) * s * 0.012f;	// it floats a little while it rests
	}

	// The search pulses of a drive not found, from where the drive would be.
	if (state == NoDrive && some)
		for (int i = 0; i < 3; i++)
		{
			const float k = (float)std::fmod(now * 0.45 + i / 3.0, 1.0);
			list->AddCircle(ImVec2(cx, slotY), s * (0.08f + 0.55f * k), withAlpha(t.accent, (1.f - k) * 0.45f * alpha),
					48, px(2.f));
		}

	// An empty drive: the tray comes out for a disc, and goes back in, with the
	// place for a disc drawn in it.
	if (state == Empty)
	{
		const float out = some ? 0.5f + 0.5f * std::sin((float)now * 1.1f) : 1.f;
		const float trayH = s * 0.48f * (0.25f + 0.75f * (out * out * (3.f - 2.f * out)));
		const float trayX0 = faceX0 + s * 0.08f, trayX1 = faceX1 - s * 0.08f;
		list->AddRectFilled(ImVec2(trayX0, slotY - trayH), ImVec2(trayX1, slotY), withAlpha(IM_COL32(30, 34, 44, 255), alpha),
				px(6));
		list->AddRect(ImVec2(trayX0, slotY - trayH), ImVec2(trayX1, slotY), withAlpha(IM_COL32(255, 255, 255, 40), alpha), px(6),
				0, px(1.5f));
		const ImVec2 well(cx, slotY - trayH * 0.5f);
		const float wellR = std::min(trayH * 0.42f, (trayX1 - trayX0) * 0.42f);
		const float pulse = some ? 0.5f + 0.5f * std::sin((float)now * 3.0f) : 0.6f;
		list->AddCircle(well, wellR, withAlpha(t.accent, (0.35f + 0.45f * pulse) * alpha), 48, px(2.5f));
		list->AddCircle(well, wellR * 0.22f, withAlpha(t.accent, 0.5f * alpha), 24, px(2.f));
		// An arrow above it, bobbing: a disc goes in here.
		const float bob = some ? std::sin((float)now * 3.0f) * s * 0.02f : 0.f;
		const float ay = slotY - trayH - s * 0.10f + bob, aw = s * 0.05f;
		list->AddTriangleFilled(ImVec2(cx - aw, ay), ImVec2(cx + aw, ay), ImVec2(cx, ay + aw * 1.1f),
				withAlpha(t.accent, (0.5f + 0.5f * pulse) * alpha));
		list->AddRectFilled(ImVec2(cx - aw * 0.35f, ay - aw * 1.2f), ImVec2(cx + aw * 0.35f, ay), withAlpha(t.accent,
				(0.5f + 0.5f * pulse) * alpha), px(2));
	}

	// The disc: a PlayStation disc's black side, the light's rainbow on it,
	// its marks turning with it.
	if (state == HasDisc)
	{
		Turning& turn = turning[path];
		const double dt = turn.last > 0 ? std::clamp(now - turn.last, 0.0, 0.1) : 0.0;
		turn.last = now;
		const double want = !moving ? 0.0 : activity == discdrive::Reading ? 2.6 : activity == discdrive::Spinning ? 0.6 : 0.08;
		turn.rate += (want - turn.rate) * std::min(1.0, dt * 2.5);
		turn.angle = std::fmod(turn.angle + turn.rate * dt * 2.0 * M_PI, 2.0 * M_PI);
		const float spin = (float)turn.angle, fast = (float)std::clamp(turn.rate / 2.6, 0.0, 1.0);
		const ImVec2 c(cx, discY);
		const float da = discAlpha * alpha;
		// Its shadow on the drive.
		list->AddEllipseFilled(ImVec2(cx, slotY + faceH * 0.05f), ImVec2(discR * 0.95f, discR * 0.12f),
				withAlpha(IM_COL32(0, 0, 0, 255), 0.45f * da));
		const bool label = cover.id != nullptr && cover.width > 0 && cover.height > 0;
		list->AddCircleFilled(c, discR, withAlpha(label ? IM_COL32(200, 204, 214, 255) : IM_COL32(16, 16, 22, 255), da), 64);
		if (label)
		{
			// The cover printed on the disc, turning with it: the middle square of
			// the picture, inside the disc's edge.
			const float aspect = (float)cover.width / (float)cover.height;
			const ImVec2 uv0 = aspect > 1.f ? ImVec2(0.5f - 0.5f / aspect, 0.f) : ImVec2(0.f, 0.5f - 0.5f * aspect);
			const ImVec2 uv1 = aspect > 1.f ? ImVec2(0.5f + 0.5f / aspect, 1.f) : ImVec2(1.f, 0.5f + 0.5f * aspect);
			const float lr = discR * 0.965f;
			list->AddImageRotated(cover.id, ImVec2(c.x - lr, c.y - lr), ImVec2(c.x + lr, c.y + lr), uv0, uv1,
					withAlpha(IM_COL32_WHITE, da), lr, spin);
			// Printed ink is matte: a little darker toward the edge, as the light falls off.
			list->AddCircle(c, lr * 0.985f, withAlpha(IM_COL32(0, 0, 0, 255), 0.25f * da), 64, px(3.f));
		}
		else
			// The data area's faint rings.
			for (float k : { 0.93f, 0.80f, 0.66f, 0.52f })
				list->AddCircle(c, discR * k, withAlpha(IM_COL32(255, 255, 255, 14), da), 64, px(1.f));
		// The rainbow stays where the light is, swaying a little; brighter as it spins.
		const float light = -0.75f + (some ? 0.12f * std::sin((float)now * 0.7f) : 0.f);
		for (int side = 0; side < 2; side++)
		{
			const float centre = light + (float)side * (float)M_PI;
			for (int i = -5; i < 5; i++)
			{
				const float a0 = centre + (float)i * 0.06f, a1 = a0 + 0.06f;
				const float fall = 1.f - std::fabs(((float)i + 0.5f) / 5.f);
				ringPiece(list, c, discR * 0.40f, discR * 0.97f, a0, a1,
						rainbow((float)(i + 5) / 10.f + (float)side * 0.5f, (label ? 0.07f : 0.16f + 0.16f * fast) * fall * da), 2);
			}
		}
		// The marks that turn with it (pressing marks near the middle, a nick in
		// the rim), drawn longer and fainter as it spins faster.
		static const float marks[][3] = { { 0.0f, 0.36f, 0.35f }, { 2.1f, 0.38f, 0.20f }, { 3.9f, 0.35f, 0.28f } };
		for (const auto& m : marks)
		{
			if (label)
				break;	// the cover shows the turning
			const float a0 = spin + m[0], len = m[2] * (1.f + fast * 4.f);
			ringPiece(list, c, discR * m[1], discR * (m[1] + 0.035f), a0, a0 + len,
					withAlpha(IM_COL32(220, 225, 235, 255), (0.30f - fast * 0.18f) * da), 5);
		}
		if (!label)
			ringPiece(list, c, discR * 0.90f, discR * 0.99f, spin + 1.2f, spin + 1.2f + 0.05f + fast * 0.9f,
					withAlpha(IM_COL32(255, 255, 255, 255), (0.35f - fast * 0.2f) * da), 4);
		// Speed: arcs around the rim while it is read.
		if (activity == discdrive::Reading && some)
			for (int i = 0; i < 3; i++)
			{
				const float a0 = -spin * 1.0f + (float)i * 2.094f;
				ringPiece(list, c, discR * 1.05f, discR * 1.09f, a0, a0 + 0.9f, withAlpha(t.accent, 0.55f * fast * da), 8);
			}
		// The middle: the clear ring, the stacking ring, the hole.
		list->AddCircleFilled(c, discR * 0.30f, withAlpha(IM_COL32(190, 198, 212, 255), 0.30f * da), 48);
		list->AddCircle(c, discR * 0.22f, withAlpha(IM_COL32(255, 255, 255, 255), 0.30f * da), 48, px(1.5f));
		list->AddCircleFilled(c, discR * 0.12f, withAlpha(IM_COL32(6, 8, 12, 255), da), 32);
		list->AddCircle(c, discR, withAlpha(IM_COL32(255, 255, 255, 50), da), 64, px(1.5f));
		// The laser: a glint that runs along a radius while the disc is read.
		if (activity == discdrive::Reading && some)
		{
			const float k = 0.5f + 0.5f * std::sin((float)now * 2.2f);
			const float rr = discR * (0.38f + 0.55f * k);
			const ImVec2 g(c.x + std::cos(1.9f) * rr, c.y + std::sin(1.9f) * rr);
			list->AddCircleFilled(g, s * 0.022f, withAlpha(IM_COL32(255, 90, 90, 255), 0.35f * da), 16);
			list->AddCircleFilled(g, s * 0.009f, withAlpha(IM_COL32(255, 220, 220, 255), 0.9f * da), 12);
		}
		// A copy's progress: a ring of the accent around the disc.
		const discdrive::Status status = discdrive::status();
		if (discdrive::jobState() == discdrive::JobRunning && status.progress >= 0.f)
		{
			list->AddCircle(c, discR * 1.14f, withAlpha(IM_COL32(255, 255, 255, 255), 0.12f * da), 64, px(3.f));
			ringPiece(list, c, discR * 1.12f, discR * 1.16f, -(float)M_PI * 0.5f,
					-(float)M_PI * 0.5f + 2.f * (float)M_PI * std::clamp(status.progress, 0.f, 1.f), withAlpha(t.accent, da), 48);
		}
	}

	// The face of the drive over the disc's lower half: the disc is in it.
	list->AddRectFilled(ImVec2(faceX0, faceY0), ImVec2(faceX1, faceY1), withAlpha(IM_COL32(24, 27, 36, 255),
			(state == NoDrive ? 0.55f : 1.f) * alpha), px(8));
	list->AddRectFilledMultiColor(ImVec2(faceX0 + px(2), faceY0 + px(2)), ImVec2(faceX1 - px(2), faceY0 + faceH * 0.35f),
			withAlpha(IM_COL32(255, 255, 255, 255), 0.07f * alpha), withAlpha(IM_COL32(255, 255, 255, 255), 0.07f * alpha),
			withAlpha(IM_COL32(255, 255, 255, 255), 0.f), withAlpha(IM_COL32(255, 255, 255, 255), 0.f));
	list->AddRect(ImVec2(faceX0, faceY0), ImVec2(faceX1, faceY1), withAlpha(IM_COL32(255, 255, 255, 255), 0.10f * alpha), px(8),
			0, px(1.f));
	list->AddRectFilled(ImVec2(faceX0 + s * 0.06f, slotY - px(1.5f)), ImVec2(faceX1 - s * 0.06f, slotY + px(1.5f)),
			withAlpha(IM_COL32(0, 0, 0, 255), 0.85f * alpha), px(2));
	// The eject button.
	{
		const float ex = faceX0 + s * 0.07f, ey = faceY0 + faceH * 0.72f, e = s * 0.018f;
		list->AddTriangleFilled(ImVec2(ex - e, ey), ImVec2(ex + e, ey), ImVec2(ex, ey - e * 1.1f),
				withAlpha(IM_COL32(255, 255, 255, 255), 0.30f * alpha));
		list->AddRectFilled(ImVec2(ex - e, ey + e * 0.35f), ImVec2(ex + e, ey + e * 0.65f),
				withAlpha(IM_COL32(255, 255, 255, 255), 0.30f * alpha));
	}
	// The light: green blinking while read, green while a game plays, the
	// accent with a disc, amber for an empty tray, red with no drive.
	{
		ImU32 led = t.accent;
		float on = 1.f;
		if (state == NoDrive)
		{
			led = IM_COL32(240, 70, 70, 255);
			on = some ? 0.35f + 0.65f * (0.5f + 0.5f * std::sin((float)now * 2.0f)) : 0.8f;
		}
		else if (state == Empty)
		{
			led = IM_COL32(255, 176, 50, 255);
			on = some ? 0.4f + 0.6f * (0.5f + 0.5f * std::sin((float)now * 1.1f)) : 0.8f;
		}
		else if (activity == discdrive::Reading)
		{
			led = IM_COL32(80, 230, 120, 255);
			on = some ? (std::fmod(now * 7.0, 1.0) < 0.55 ? 1.f : 0.25f) : 1.f;
		}
		else if (activity == discdrive::Spinning)
			led = IM_COL32(80, 230, 120, 255);
		const ImVec2 lc(faceX1 - s * 0.07f, faceY0 + faceH * 0.70f);
		list->AddCircleFilled(lc, s * 0.040f, withAlpha(led, 0.18f * on * alpha), 24);
		list->AddCircleFilled(lc, s * 0.016f, withAlpha(led, (0.35f + 0.65f * on) * alpha), 16);
	}
	list->PopClipRect();

	// What it is: the disc's name (or the drive's state), over the top.
	if (withName && s > px(110))
	{
		const float unitsWide = w / unit;
		const float size = std::clamp(unitsWide * 0.075f, 14.f, 26.f);
		const std::vector<std::string> lines = wrapText(name, w - px(24), Bold, size);
		float y = a.y + h * 0.05f;
		for (size_t line = 0; line < lines.size() && line < 2; line++)
		{
			textCentred(ImVec2(cx, y), withAlpha(IM_COL32(255, 255, 255, 230), alpha), lines[line], Bold, size);
			y += px(size * 1.15f);
		}
	}
	return true;
}

void coverPlaceholder(ImVec2 a, ImVec2 b, const std::string& name, const std::string& region)
{
	// A tint of its own for each game, from its name.
	uint32_t hash = 2166136261u;
	for (const char c : name)
		hash = (hash ^ (uint8_t)c) * 16777619u;
	const float hue = (float)(hash % 360) / 360.f;
	const auto hsv = [](float h, float s, float v) {
		const float i = std::floor(h * 6.f), f = h * 6.f - i;
		const float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
		switch ((int)i % 6)
		{
		case 0: return gfx::Color{ v, t, p, 1 };
		case 1: return gfx::Color{ q, v, p, 1 };
		case 2: return gfx::Color{ p, v, t, 1 };
		case 3: return gfx::Color{ p, q, v, 1 };
		case 4: return gfx::Color{ t, p, v, 1 };
		default: return gfx::Color{ v, p, q, 1 };
		}
	};
	const gfx::Color top = hsv(hue, 0.42f, 0.38f);
	const gfx::Color bottom{ top.r * 0.45f, top.g * 0.45f, top.b * 0.45f, 1.f };
	if (inScene)
	{
		draw()->AddRectFilled(a, b, u32Of(top));
		return;
	}
	const gfx::Rect r = rectOf(a, b);
	frontList.gradient_rect(r, px(10), top, bottom);
	const float w = r.w, h = r.h;
	const float unitsWide = w / unit;
	textCentred(ImVec2(a.x + w * 0.5f, a.y + h * 0.16f), IM_COL32(255, 255, 255, 70), icon::Disc, Title,
			unitsWide * 0.30f);
	// The name, wrapped and centred line by line.
	const float size = std::clamp(unitsWide * 0.095f, 16.f, 26.f);
	const float wrap = w - px(24);
	float y = a.y + h * 0.56f;
	const std::vector<std::string> lines = wrapText(name, wrap, Bold, size);
	for (size_t line = 0; line < lines.size() && line < 3; line++)
	{
		std::string piece = lines[line];
		if (line == 2 && lines.size() > 3)
			piece += "\xe2\x80\xa6";
		textCentred(ImVec2(a.x + w * 0.5f, y), IM_COL32(255, 255, 255, 235), piece, Bold, size);
		y += px(size * 1.2f);
	}
	if (!region.empty())
		textCentred(ImVec2(a.x + w * 0.5f, b.y - px(size * 1.6f)), IM_COL32(255, 255, 255, 120), region, Body, size * 0.8f);
}

namespace
{
// Text into RGBA pixels, from a font's distance field: each glyph's square of
// the atlas sampled, the edge where the field crosses the middle.
void blitText(std::vector<uint32_t>& pixels, int w, int h, const hui::ui::FontRef& ref, const std::string& value,
		float cx, float baseline, float size, uint32_t colour)
{
	if (ref.font == nullptr)
		return;
	const gfx::Font& f = *ref.font;
	std::vector<gfx::GlyphQuad> quads;
	f.layout(value, cx, baseline, size, gfx::Align::center, quads);
	const std::vector<uint8_t>& atlas = f.atlas();
	const int aw = f.atlas_width(), ah = f.atlas_height();
	const float soft = std::max(0.5f / std::max(f.sdf_range(size), 0.5f), 0.02f);
	for (const gfx::GlyphQuad& q : quads)
	{
		for (int y = std::max((int)q.y0, 0); y < std::min((int)std::ceil(q.y1), h); y++)
			for (int x = std::max((int)q.x0, 0); x < std::min((int)std::ceil(q.x1), w); x++)
			{
				const float u = q.u0 + (q.u1 - q.u0) * (((float)x + 0.5f - q.x0) / (q.x1 - q.x0));
				const float v = q.v0 + (q.v1 - q.v0) * (((float)y + 0.5f - q.y0) / (q.y1 - q.y0));
				const int ax = std::clamp((int)(u * (float)aw), 0, aw - 1), ay = std::clamp((int)(v * (float)ah), 0, ah - 1);
				const float d = (float)atlas[(size_t)ay * (size_t)aw + (size_t)ax] / 255.f;
				const float cover = std::clamp((d - 0.5f) / soft + 0.5f, 0.f, 1.f) * (float)(colour >> 24) / 255.f;
				if (cover <= 0.f)
					continue;
				uint32_t& out = pixels[(size_t)y * (size_t)w + (size_t)x];
				uint32_t blended = 0xff000000u;
				for (int shift = 0; shift < 24; shift += 8)
				{
					const float under = (float)((out >> shift) & 0xff), over = (float)((colour >> shift) & 0xff);
					blended |= (uint32_t)(under + (over - under) * cover + 0.5f) << shift;
				}
				out = blended;
			}
	}
}
}

Image placeholderImage(const std::string& name, const std::string& region, float aspect)
{
	const int h = 320, w = std::clamp((int)(320.f * aspect), 160, 420);
	const std::string key = "placeholder:" + std::to_string(w) + ":" + name;
	Image cached = imageFromPixels(key, nullptr, w, h);
	if (cached.id != nullptr || !fontsReady)
		return cached;
	uint32_t hash = 2166136261u;
	for (const char c : name)
		hash = (hash ^ (uint8_t)c) * 16777619u;
	const float hue = (float)(hash % 360) / 360.f;
	const float i = std::floor(hue * 6.f), f = hue * 6.f - i;
	const float v = 0.38f, sat = 0.42f, p = v * (1 - sat), q = v * (1 - f * sat), t = v * (1 - (1 - f) * sat);
	float r = v, g = t, b = p;
	switch ((int)i % 6)
	{
	case 1: r = q; g = v; b = p; break;
	case 2: r = p; g = v; b = t; break;
	case 3: r = p; g = q; b = v; break;
	case 4: r = t; g = p; b = v; break;
	case 5: r = v; g = p; b = q; break;
	default: break;
	}
	std::vector<uint32_t> pixels((size_t)w * h);
	for (int y = 0; y < h; y++)
	{
		const float k = 1.f - 0.55f * (float)y / (float)h;
		const uint32_t c = 0xff000000u | ((uint32_t)(b * k * 255.f) << 16) | ((uint32_t)(g * k * 255.f) << 8) | (uint32_t)(r * k * 255.f);
		std::fill(pixels.begin() + (size_t)y * w, pixels.begin() + (size_t)(y + 1) * w, c);
	}
	blitText(pixels, w, h, iconFont, icon::Disc, (float)w * 0.5f, (float)h * 0.36f, (float)w * 0.34f, 0x46ffffffu);
	const Face face{ &kitFonts.semibold, 30.f, 30.f };
	const std::vector<std::string> lines = wrapLines(name, (float)w - 28.f, face);
	float y = (float)h * 0.62f;
	for (size_t line = 0; line < lines.size() && line < 3; line++)
	{
		std::string piece = lines[line];
		if (line == 2 && lines.size() > 3)
			piece += "\xe2\x80\xa6";
		blitText(pixels, w, h, kitFonts.semibold, piece, (float)w * 0.5f, y, 30.f, 0xebffffffu);
		y += 34.f;
	}
	if (!region.empty())
		blitText(pixels, w, h, kitFonts.regular, region, (float)w * 0.5f, (float)h - 18.f, 22.f, 0x78ffffffu);
	return imageFromPixels(key, pixels.data(), w, h);
}

void imageFit(const Image& image, ImVec2 a, ImVec2 b, float rounding, ImU32 tint)
{
	if (image.id == nullptr || image.width <= 0 || image.height <= 0)
		return;
	const float boxW = b.x - a.x, boxH = b.y - a.y;
	const float scale = std::min(boxW / (float)image.width, boxH / (float)image.height);
	const float w = (float)image.width * scale, h = (float)image.height * scale;
	// Sat on the bottom of its box, as a case stands on a shelf.
	const ImVec2 p0(std::floor(a.x + (boxW - w) * 0.5f), std::floor(b.y - h));
	draw()->AddImageRounded((ImTextureID)image.id, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1), tint,
			px(rounding));
}

}

// ------------------------------------------------------------ theme fonts

namespace fe::fontbake
{
bool bake(const std::vector<uint8_t>& ttf, std::string& out, std::string& why);
}

namespace fe::ui
{
namespace
{

// A theme's font file, baked on a thread of its own (a few hundred
// milliseconds), then uploaded by the frame.
struct FileFace
{
	std::string path;
	std::string blob, why;
	gfx::Font font;
	hui::ui::FontRef ref;
	std::atomic<int> state{0};	// 0 baking, 1 baked, 2 ready, -1 failed
};
std::vector<std::unique_ptr<FileFace>> fileFaces;

const hui::ui::FontRef *fileRef(int font)
{
	if (font >= 0 && font < (int)fileFaces.size() && fileFaces[(size_t)font]->state.load() == 2)
		return &fileFaces[(size_t)font]->ref;
	return &kitFonts.regular;
}

Face fileFace(int font, float em, float lineHeight)
{
	return { fileRef(font), em, lineHeight > 0 ? lineHeight : em };
}

}

int fontFile(const std::string& path)
{
	if (path.empty() || !fontsReady)
		return -1;
	for (size_t i = 0; i < fileFaces.size(); i++)
	{
		FileFace& f = *fileFaces[i];
		if (f.path != path)
			continue;
		const int state = f.state.load(std::memory_order_acquire);
		if (state == 1)
		{
			if (f.font.load(f.blob))
			{
				f.ref.font = &f.font;
				f.ref.texture = display::interfaceRenderer().create_font_texture(f.font);
				f.state = f.ref.texture != 0 ? 2 : -1;
				diag::mark("themes: font %s ready (%u by %u atlas)", path.substr(path.rfind('/') + 1).c_str(),
						(unsigned)f.font.atlas_width(), (unsigned)f.font.atlas_height());
			}
			else
			{
				diag::mark("themes: font %s: %s", path.c_str(), f.font.error().c_str());
				f.state = -1;
			}
			std::string().swap(f.blob);
		}
		return f.state.load() == 2 ? (int)i : -1;
	}
	auto fresh = std::make_unique<FileFace>();
	fresh->path = path;
	FileFace *f = fresh.get();
	fileFaces.push_back(std::move(fresh));
	std::thread([f] {
		tuning::setThread(tuning::Role::Background);
		std::vector<uint8_t> file;
		bool ok = readFile(f->path, file) && fontbake::bake(file, f->blob, f->why);
		if (!ok)
			diag::mark("themes: font %s cannot be used: %s", f->path.c_str(),
					f->why.empty() ? "it cannot be read" : f->why.c_str());
		f->state.store(ok ? 1 : -1, std::memory_order_release);
	}).detach();
	return -1;
}

bool fontFilePending(const std::string& path)
{
	for (const auto& f : fileFaces)
		if (f->path == path)
			return f->state.load() == 0 || f->state.load() == 1;
	return false;
}

float fontTextWidth(int font, const std::string& value, float em)
{
	if (!fontsReady || value.empty())
		return 0;
	return textWidth(value, fileFace(font, em, em));
}

float fontAscent(int font, float em)
{
	return fontsReady ? fileRef(font)->font->ascent(em) : em * 0.8f;
}

float fontDescent(int font, float em)
{
	return fontsReady ? fileRef(font)->font->descent(em) : em * 0.2f;
}

void fontText(int font, ImVec2 at, ImU32 colour, const std::string& value, float em, float lineHeight)
{
	if (!fontsReady || value.empty() || inScene)
		return;
	const Face face = fileFace(font, em, lineHeight);
	const gfx::Font& f = *face.ref->font;
	// Set on the middle of its line, as tall as the line is.
	const float top = at.y + (face.pixels - (f.ascent(em) + f.descent(em))) * 0.5f;
	drawLine(frontList, at.x, top, colour, value, face);
}

std::vector<std::string> fontWrap(int font, const std::string& value, float em, float width)
{
	if (!fontsReady)
		return { value };
	return wrapLines(value, width, fileFace(font, em, em));
}

std::string fontFit(int font, const std::string& value, float em, float width)
{
	if (!fontsReady)
		return value;
	const Face face = fileFace(font, em, em);
	if (textWidth(value, face) <= width)
		return value;
	static const std::string dots = "\xe2\x80\xa6";
	std::string cut = value.substr(0, fitLength(value, std::max(width - textWidth(dots, face), 0.f), face));
	while (!cut.empty() && cut.back() == ' ')
		cut.pop_back();
	return cut + dots;
}

} // namespace fe::ui
