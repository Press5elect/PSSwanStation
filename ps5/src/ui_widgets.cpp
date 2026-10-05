/*
	PSSwanStation - the interface's drawing: fonts, colours, the pieces
	every screen is made of.

	SPDX-License-Identifier: GPL-3.0-or-later

	Sizes are given in units of a 1080-line screen and scaled to the display
	(two pixels a unit at 2160 lines) and by the interface-size setting.

	The fonts (Roboto, Apache-2.0; Font Awesome Free's solid symbols, SIL OFL
	1.1) and the logo are part of the program file, so the interface does not
	depend on files beside it.
*/
#include "ui.h"
#include "display.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

FE_EMBED(fe_font_medium, "Roboto-Medium.ttf");
FE_EMBED(fe_font_bold, "Roboto-Bold.ttf");
FE_EMBED(fe_font_symbols, "fa-solid-900.ttf");
FE_EMBED(fe_logo_box_png, "logo-box.png");

namespace fe::ui
{
namespace
{

ImFont *fonts[4];
float fontPixels[4];
float unit = 1.f;
Theme current;
Image logoImage;
double started;

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

const ImWchar textRanges[] = {
	0x0020, 0x00FF,		// Basic Latin, Latin-1
	0x0100, 0x017F,		// Latin Extended-A
	0x2010, 0x2027,		// dashes, quotes, the ellipsis
	0x2122, 0x2122,		// trade mark
	0,
};
const ImWchar symbolRanges[] = {
	0xf002, 0xf002, 0xf55a, 0xf55a, 0xf00a, 0xf00a, 0xf00c, 0xf00d, 0xf011, 0xf011, 0xf013, 0xf013, 0xf017, 0xf017, 0xf019, 0xf019, 0xf021, 0xf021,
	0xf028, 0xf028, 0xf03a, 0xf03a, 0xf03e, 0xf03e, 0xf04b, 0xf04b, 0xf054, 0xf054, 0xf05a, 0xf05a, 0xf071, 0xf071,
	0xf07c, 0xf07c, 0xf093, 0xf093, 0xf0a0, 0xf0a0, 0xf0ad, 0xf0ad, 0xf0c7, 0xf0c7, 0xf0d0, 0xf0d0, 0xf0e2, 0xf0e2,
	0xf0e7, 0xf0e7, 0xf11b, 0xf11b, 0xf1de, 0xf1de, 0xf1e6, 0xf1e6, 0xf233, 0xf233, 0xf26c, 0xf26c, 0xf2db, 0xf2db,
	0xf51f, 0xf51f, 0xf538, 0xf538, 0xf53f, 0xf53f, 0xf6ff, 0xf6ff, 0xf7c2, 0xf7c2,
	0,
};

ImFont *addFont(const unsigned char *data, const unsigned char *end, float pixels)
{
	ImGuiIO& io = ImGui::GetIO();
	ImFontConfig config;
	config.FontDataOwnedByAtlas = false;
	config.OversampleH = 2;
	config.OversampleV = 1;
	ImFont *font = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(data), (int)(end - data), pixels,
			&config, textRanges);
	ImFontConfig symbols;
	symbols.FontDataOwnedByAtlas = false;
	symbols.MergeMode = true;
	symbols.OversampleH = 1;
	symbols.OversampleV = 1;
	symbols.GlyphMinAdvanceX = pixels;
	symbols.GlyphOffset = ImVec2(0, pixels * 0.04f);
	io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(fe_font_symbols),
			(int)(fe_font_symbols_end - fe_font_symbols), pixels * 0.82f, &symbols, symbolRanges);
	return font;
}

// A soft light: full colour at the centre, nothing at the edge.
void glow(ImDrawList *list, ImVec2 centre, float radius, ImU32 colour)
{
	constexpr int Segments = 40;
	const ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
	const ImU32 edge = colour & 0x00FFFFFF;
	list->PrimReserve(Segments * 3, Segments + 1);
	const ImDrawIdx base = (ImDrawIdx)list->_VtxCurrentIdx;
	list->PrimWriteVtx(centre, uv, colour);
	for (int i = 0; i < Segments; i++)
	{
		const float angle = (float)i / Segments * 6.2831853f;
		list->PrimWriteVtx(ImVec2(centre.x + std::cos(angle) * radius, centre.y + std::sin(angle) * radius), uv, edge);
	}
	for (int i = 0; i < Segments; i++)
	{
		list->PrimWriteIdx(base);
		list->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
		list->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % Segments));
	}
}

// The end of the longest prefix of `text` that fits `maxWidth`.
size_t fitLength(const std::string& text, float maxWidth, ImFont *font, float pixels)
{
	const char *remaining = nullptr;
	font->CalcTextSizeA(pixels, maxWidth, 0.f, text.c_str(), text.c_str() + text.size(), &remaining);
	size_t length = (size_t)(remaining - text.c_str());
	while (length > 0 && length < text.size() && ((unsigned char)text[length] & 0xC0) == 0x80)
		length--;
	return length;
}

}

const char *accentName(int index)
{
	return accents[std::clamp(index, 0, AccentCount - 1)].name;
}

const Theme& theme()
{
	return current;
}

void widgetsInit()
{
	started = now();
	const float scale = display::scale();
	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();
	// A 4096-wide atlas at most: the console's driver takes it, and so does
	// every PC's.
	io.Fonts->TexDesiredWidth = scale > 1.5f ? 4096 : 2048;
	fontPixels[Body] = std::floor(26.f * scale);
	fontPixels[Bold] = std::floor(26.f * scale);
	fontPixels[Title] = std::floor(44.f * scale);
	fonts[Body] = addFont(fe_font_medium, fe_font_medium_end, fontPixels[Body]);
	fonts[Bold] = addFont(fe_font_bold, fe_font_bold_end, fontPixels[Bold]);
	fonts[Title] = addFont(fe_font_bold, fe_font_bold_end, fontPixels[Title]);
	// The splash's name: the plain letters only, large.
	fontPixels[Huge] = std::floor(92.f * scale);
	{
		static const ImWchar plain[] = { 0x0020, 0x007E, 0 };
		ImFontConfig config;
		config.FontDataOwnedByAtlas = false;
		config.OversampleH = 1;
		config.OversampleV = 1;
		fonts[Huge] = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(fe_font_bold),
				(int)(fe_font_bold_end - fe_font_bold), fontPixels[Huge], &config, plain);
	}
	if (!io.Fonts->Build())
		diag::mark("ui: the font atlas could not be built");
	diag::mark("ui: fonts %d x %d", io.Fonts->TexWidth, io.Fonts->TexHeight);
	logoImage = imageFromMemory(fe_logo_box_png, (size_t)(fe_logo_box_png_end - fe_logo_box_png));
	widgetsFrame();
}

void widgetsFrame()
{
	const options::Frontend& settings = options::frontend();
	unit = display::scale() * (float)std::clamp(settings.uiScale, 70, 140) / 100.f;
	const ImU32 accent = accents[std::clamp(settings.accent, 0, AccentCount - 1)].colour;
	current.background = IM_COL32(14, 18, 30, 255);
	current.backgroundLow = IM_COL32(6, 8, 14, 255);
	current.panel = IM_COL32(26, 32, 50, 235);
	current.panelHigh = IM_COL32(44, 54, 82, 255);
	current.accent = accent;
	current.accentSoft = withAlpha(accent, 0.22f);
	current.text = IM_COL32(238, 242, 250, 255);
	current.dim = IM_COL32(160, 170, 192, 255);
	current.faint = IM_COL32(104, 114, 138, 255);
	current.good = IM_COL32(74, 222, 128, 255);
	current.bad = IM_COL32(248, 113, 113, 255);
}

float px(float units)
{
	return std::floor(units * unit + 0.5f);
}

ImDrawList *draw()
{
	return ImGui::GetForegroundDrawList();
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

float approach(float value, float target, float speed)
{
	// With the animations off, things are where they are going at once.
	if (motion() == MotionOff)
		return target;
	const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
	const float next = value + (target - value) * (1.f - std::exp(-speed * dt));
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

ImVec2 measure(const std::string& text, Font font, float size)
{
	return fonts[font]->CalcTextSizeA(px(size), FLT_MAX, 0.f, text.c_str(), text.c_str() + text.size());
}

void text(ImVec2 at, ImU32 colour, const std::string& text, Font font, float size)
{
	if (!text.empty())
		draw()->AddText(fonts[font], px(size), ImVec2(std::floor(at.x), std::floor(at.y)), colour, text.c_str(),
				text.c_str() + text.size());
}

float textFit(ImVec2 at, float maxWidth, ImU32 colour, const std::string& value, Font font, float size)
{
	const float pixels = px(size);
	const float whole = measure(value, font, size).x;
	if (whole <= maxWidth)
	{
		text(at, colour, value, font, size);
		return whole;
	}
	static const std::string dots = "\xe2\x80\xa6";
	const float dotsWidth = measure(dots, font, size).x;
	std::string cut = value.substr(0, fitLength(value, std::max(maxWidth - dotsWidth, 0.f), fonts[font], pixels));
	while (!cut.empty() && cut.back() == ' ')
		cut.pop_back();
	cut += dots;
	text(at, colour, cut, font, size);
	return measure(cut, font, size).x;
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
	if (value.empty())
		return 0;
	const float pixels = px(size);
	const ImVec2 extent = fonts[font]->CalcTextSizeA(pixels, FLT_MAX, wrapWidth, value.c_str(),
			value.c_str() + value.size());
	const ImVec4 clip(at.x, at.y, at.x + wrapWidth, at.y + (maxHeight > 0 ? maxHeight : extent.y + pixels));
	draw()->AddText(fonts[font], pixels, ImVec2(std::floor(at.x), std::floor(at.y)), colour, value.c_str(),
			value.c_str() + value.size(), wrapWidth, &clip);
	return maxHeight > 0 ? std::min(extent.y, maxHeight) : extent.y;
}

float wrappedHeight(const std::string& value, float wrapWidth, Font font, float size)
{
	if (value.empty())
		return 0;
	return fonts[font]->CalcTextSizeA(px(size), FLT_MAX, wrapWidth, value.c_str(), value.c_str() + value.size()).y;
}

void panel(ImVec2 a, ImVec2 b, ImU32 colour, float rounding)
{
	draw()->AddRectFilled(a, b, colour, px(rounding));
}

void outline(ImVec2 a, ImVec2 b, ImU32 colour, float rounding, float thickness)
{
	draw()->AddRect(a, b, colour, px(rounding), 0, px(thickness));
}

void backdrop()
{
	ImDrawList *list = ImGui::GetBackgroundDrawList();
	const float w = width(), h = height();
	list->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), current.background, current.background,
			current.backgroundLow, current.backgroundLow);
	// Three slow lights in the accent colour (still, with the animations off).
	const float t = motion() == MotionOff ? 0.f : (float)clock();
	const ImU32 light = withAlpha(current.accent, 0.16f);
	const ImU32 cool = IM_COL32(120, 90, 255, 30);
	glow(list, ImVec2(w * (0.18f + 0.06f * std::sin(t * 0.11f)), h * (0.10f + 0.05f * std::cos(t * 0.13f))), h * 0.75f,
			light);
	glow(list, ImVec2(w * (0.86f + 0.05f * std::cos(t * 0.09f)), h * (0.92f + 0.06f * std::sin(t * 0.07f))), h * 0.85f,
			cool);
	glow(list, ImVec2(w * (0.62f + 0.10f * std::sin(t * 0.05f + 1.7f)), h * (0.35f + 0.10f * std::cos(t * 0.06f))),
			h * 0.45f, withAlpha(current.accent, 0.07f));
}

void wash(const Image& image, float alpha)
{
	if (image.soft == nullptr || alpha <= 0.004f)
		return;
	// The blurred miniature, over the whole screen and a little beyond, so
	// its edge pixels are not the screen's edges.
	ImDrawList *list = ImGui::GetBackgroundDrawList();
	const float w = width(), h = height();
	list->AddImage((ImTextureID)image.soft, ImVec2(-w * 0.08f, -h * 0.08f), ImVec2(w * 1.08f, h * 1.08f), ImVec2(0, 0),
			ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(std::clamp(alpha, 0.f, 1.f) * 255)));
}

void waves(float alpha)
{
	ImDrawList *list = ImGui::GetBackgroundDrawList();
	const float w = width(), h = height();
	const float t = (float)clock();
	static const float rows[3] = { 0.800f, 0.850f, 0.900f };
	static const float strength[3] = { 0.27f, 0.19f, 0.12f };
	for (int row = 0; row < 3; row++)
	{
		const float y = h * rows[row];
		for (float x = 0; x <= w; x += px(8))
			list->PathLineTo(ImVec2(x, y + std::sin(x / w * 3.14159f * 9.f + t * (0.9f + 0.25f * (float)row)) * h * 0.006f));
		list->PathStroke(IM_COL32(150, 200, 255, (int)(255 * strength[row] * std::clamp(alpha, 0.f, 1.f))), 0,
				std::max(h / 360.f, 2.f));
	}
}

void glowAt(ImVec2 centre, float radius, ImU32 colour)
{
	glow(ImGui::GetBackgroundDrawList(), centre, radius, colour);
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
	const ImVec2 b(at.x + w, at.y + h);
	if (on)
	{
		draw()->AddRectFilled(ImVec2(at.x - px(5), at.y - px(5)), ImVec2(b.x + px(5), b.y + px(5)),
				withAlpha(current.accent, 0.28f), h * 0.5f + px(5));
		draw()->AddRectFilled(at, b, current.accent, h * 0.5f);
	}
	else
		draw()->AddRectFilled(at, b, current.panelHigh, h * 0.5f);
	const ImU32 ink = !enabled ? current.faint : on ? IM_COL32(8, 14, 28, 255) : current.dim;
	text(ImVec2(at.x + h * 0.5f, at.y + (h - extent.y) * 0.5f), ink, label, Bold, size);
	return w;
}

void buttonGlyph(ImVec2 centre, float size, uint32_t button)
{
	using namespace platform;
	ImDrawList *list = draw();
	const float r = px(size) * 0.5f;
	const float stroke = std::max(px(size * 0.085f), 1.5f);
	const ImU32 body = IM_COL32(36, 42, 60, 255), rim = IM_COL32(96, 108, 136, 255);
	const auto face = [&] {
		list->AddCircleFilled(centre, r, body, 32);
		list->AddCircle(centre, r, rim, 32, std::max(px(1.5f), 1.f));
	};
	const float k = r * 0.44f;
	switch (button)
	{
	case Cross:
		face();
		list->AddLine(ImVec2(centre.x - k, centre.y - k), ImVec2(centre.x + k, centre.y + k), IM_COL32(124, 178, 232, 255),
				stroke);
		list->AddLine(ImVec2(centre.x + k, centre.y - k), ImVec2(centre.x - k, centre.y + k), IM_COL32(124, 178, 232, 255),
				stroke);
		return;
	case Circle:
		face();
		list->AddCircle(centre, r * 0.52f, IM_COL32(255, 102, 102, 255), 24, stroke);
		return;
	case Square:
		face();
		list->AddRect(ImVec2(centre.x - k, centre.y - k), ImVec2(centre.x + k, centre.y + k), IM_COL32(255, 105, 248, 255),
				0, 0, stroke);
		return;
	case Triangle:
	{
		face();
		const float t = r * 0.56f;
		list->AddTriangle(ImVec2(centre.x, centre.y - t), ImVec2(centre.x + t * 0.95f, centre.y + t * 0.68f),
				ImVec2(centre.x - t * 0.95f, centre.y + t * 0.68f), IM_COL32(64, 226, 160, 255), stroke);
		return;
	}
	default:
		break;
	}
	// The others are a label on a key.
	const char *label = button == L1 ? "L1" : button == R1 ? "R1" : button == L2 ? "L2" : button == R2 ? "R2"
			: button == Options ? "OPTIONS" : button == (L1 | R1) ? "L1  R1" : button == (L2 | R2) ? "L2  R2"
			: button == (Left | Right) ? "\xe2\x97\x80  \xe2\x96\xb6" : button == TouchLeft ? "TOUCH L"
			: button == TouchRight ? "TOUCH R" : button == (TouchLeft | TouchRight) ? "TOUCH PAD" : "?";
	// The arrows are not in the font: drawn.
	if (button == (Left | Right))
	{
		const float a = r * 0.5f;
		const ImU32 colour = IM_COL32(200, 208, 224, 255);
		list->AddTriangleFilled(ImVec2(centre.x - r * 0.25f, centre.y - a), ImVec2(centre.x - r * 0.25f, centre.y + a),
				ImVec2(centre.x - r * 1.0f, centre.y), colour);
		list->AddTriangleFilled(ImVec2(centre.x + r * 0.25f, centre.y - a), ImVec2(centre.x + r * 0.25f, centre.y + a),
				ImVec2(centre.x + r * 1.0f, centre.y), colour);
		return;
	}
	const float textSize = size * 0.50f;
	const ImVec2 extent = measure(label, Bold, textSize);
	const float half = extent.x * 0.5f + r * 0.45f;
	list->AddRectFilled(ImVec2(centre.x - half, centre.y - r * 0.82f), ImVec2(centre.x + half, centre.y + r * 0.82f), body,
			r * 0.4f);
	list->AddRect(ImVec2(centre.x - half, centre.y - r * 0.82f), ImVec2(centre.x + half, centre.y + r * 0.82f), rim,
			r * 0.4f, 0, std::max(px(1.5f), 1.f));
	text(ImVec2(centre.x - extent.x * 0.5f, centre.y - extent.y * 0.5f), IM_COL32(220, 226, 238, 255), label, Bold,
			textSize);
}

// How wide a button's symbol is, in pixels.
static float glyphWidth(float size, uint32_t button)
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
	draw()->AddRectFilled(ImVec2(0, h - barHeight), ImVec2(w, h), IM_COL32(8, 10, 18, 215));
	draw()->AddLine(ImVec2(0, h - barHeight), ImVec2(w, h - barHeight), IM_COL32(255, 255, 255, 18), 1.f);
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
	const float radius = (b.y - a.y) * 0.5f;
	draw()->AddRectFilled(a, b, IM_COL32(255, 255, 255, 28), radius);
	if (fraction >= 0)
	{
		const float end = a.x + std::max((b.x - a.x) * std::clamp(fraction, 0.f, 1.f), radius * 2);
		draw()->AddRectFilled(a, ImVec2(end, b.y), current.accent, radius);
	}
	else
	{
		// No figure to show: a piece that runs along the bar.
		const float span = b.x - a.x, piece = span * 0.25f;
		const float at = (float)std::fmod(clock() * 0.6, 1.0) * (span + piece) - piece;
		draw()->AddRectFilled(ImVec2(a.x + std::max(at, 0.f), a.y), ImVec2(a.x + std::min(at + piece, span), b.y),
				current.accent, radius);
	}
}

void coverPlaceholder(ImVec2 a, ImVec2 b, const std::string& name, const std::string& region)
{
	// A tint of its own for each game, from its name.
	uint32_t hash = 2166136261u;
	for (const char c : name)
		hash = (hash ^ (uint8_t)c) * 16777619u;
	float r, g, bl;
	ImGui::ColorConvertHSVtoRGB((float)(hash % 360) / 360.f, 0.42f, 0.38f, r, g, bl);
	const ImU32 top = ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, bl, 1.f));
	const ImU32 bottom = ImGui::ColorConvertFloat4ToU32(ImVec4(r * 0.45f, g * 0.45f, bl * 0.45f, 1.f));
	const float rounding = px(10);
	draw()->AddRectFilled(a, b, bottom, rounding);
	draw()->AddRectFilledMultiColor(ImVec2(a.x, a.y + rounding), ImVec2(b.x, b.y - rounding), top, top, bottom, bottom);
	draw()->AddRectFilled(a, ImVec2(b.x, a.y + rounding * 2), top, rounding, ImDrawFlags_RoundCornersTop);
	const float w = b.x - a.x, h = b.y - a.y;
	const float unitsWide = w / unit;
	textCentred(ImVec2(a.x + w * 0.5f, a.y + h * 0.16f), IM_COL32(255, 255, 255, 70), icon::Disc, Title,
			unitsWide * 0.30f);
	// The name, wrapped and centred line by line.
	const float size = std::clamp(unitsWide * 0.095f, 16.f, 26.f);
	const float wrap = w - px(24);
	float y = a.y + h * 0.56f;
	std::string rest = name;
	for (int line = 0; line < 3 && !rest.empty(); line++)
	{
		const char *end = nullptr;
		fonts[Bold]->CalcTextSizeA(px(size), wrap, 0.f, rest.c_str(), rest.c_str() + rest.size(), &end);
		size_t length = (size_t)(end - rest.c_str());
		if (length < rest.size())
		{
			const size_t space = rest.rfind(' ', length);
			if (space != std::string::npos && space > 0)
				length = space;
		}
		std::string piece = trim(rest.substr(0, length));
		rest = trim(rest.substr(length));
		if (line == 2 && !rest.empty())
			piece += "\xe2\x80\xa6";
		textCentred(ImVec2(a.x + w * 0.5f, y), IM_COL32(255, 255, 255, 235), piece, Bold, size);
		y += px(size * 1.2f);
		if (length == 0)
			break;
	}
	if (!region.empty())
		textCentred(ImVec2(a.x + w * 0.5f, b.y - px(size * 1.6f)), IM_COL32(255, 255, 255, 120), region, Body, size * 0.8f);
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
