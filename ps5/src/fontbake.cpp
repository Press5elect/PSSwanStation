/*
	PSSwanStation - a TrueType or OpenType font file made into the interface
	kit's font (a signed distance field atlas with its metrics), on the
	console, for an ES-DE theme's fonts.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	The interface kit draws text from fonts baked before the build
	(PS5_VKHomebrewUI's tools/font-baker, from BlackBearReloaded's
	ps5-homebrew-ui). A theme brings its own .ttf and .otf files, so they are
	baked here the same way, into the same format (gfx::Font reads it): the
	printable ASCII letters, the letters of western and central European
	languages and basic Cyrillic, and a few signs, with stb_truetype (public
	domain or MIT, Sean Barrett), at 48 pixels with a spread of 6.
*/
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>
#include <cmath>
#include <cstdlib>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include "gfx/font_format.hpp"

namespace fe::fontbake
{
namespace
{
namespace ff = hui::gfx::font_format;

struct Baked
{
	int codepoint = 0;
	int w = 0, h = 0;
	float xoff = 0, yoff = 0;
	float advance = 0;
	unsigned char *bitmap = nullptr;
	bool own = false;	// made by sdfFromPicture (free), not stb_truetype
	int x = 0, y = 0;
};

void release(Baked& g)
{
	if (g.own)
		free(g.bitmap);
	else
		stbtt_FreeSDF(g.bitmap, nullptr);
	g.bitmap = nullptr;
}

std::vector<int> codepoints()
{
	std::vector<int> result;
	for (int c = 32; c < 127; ++c)
		result.push_back(c);
	for (int c = 0x00A1; c <= 0x017F; ++c)
		result.push_back(c);
	for (int c = 0x0410; c <= 0x044F; ++c)
		result.push_back(c);
	const int more[] = { 0x0401, 0x0451, 0x2018, 0x2019, 0x201C, 0x201D, 0x201E, 0x20AC, 0x2122, 0x2013, 0x2014,
		0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193, 0x2605, 0x2606 };
	result.insert(result.end(), std::begin(more), std::end(more));
	return result;
}

// Felzenszwalb and Huttenlocher's distance transform of a sampled function,
// in one dimension: d[i] = min over j of (i - j)^2 + f[j].
void distance1d(const float *f, float *d, int n, int *v, float *z)
{
	int k = 0;
	v[0] = 0;
	z[0] = -1e20f;
	z[1] = 1e20f;
	for (int q = 1; q < n; q++)
	{
		float s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.f * (float)(q - v[k]));
		while (s <= z[k])
		{
			k--;
			s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.f * (float)(q - v[k]));
		}
		k++;
		v[k] = q;
		z[k] = s;
		z[k + 1] = 1e20f;
	}
	k = 0;
	for (int q = 0; q < n; q++)
	{
		while (z[k + 1] < (float)q)
			k++;
		d[q] = (float)(q - v[k]) * (float)(q - v[k]) + f[v[k]];
	}
}

// The squared distance of every pixel to the nearest pixel that is zero in `grid`.
void distance2d(std::vector<float>& grid, int w, int h)
{
	const int n = std::max(w, h);
	std::vector<float> f((size_t)n), d((size_t)n), z((size_t)n + 1);
	std::vector<int> v((size_t)n);
	for (int x = 0; x < w; x++)
	{
		for (int y = 0; y < h; y++)
			f[(size_t)y] = grid[(size_t)y * w + x];
		distance1d(f.data(), d.data(), h, v.data(), z.data());
		for (int y = 0; y < h; y++)
			grid[(size_t)y * w + x] = d[(size_t)y];
	}
	for (int y = 0; y < h; y++)
	{
		distance1d(&grid[(size_t)y * w], d.data(), w, v.data(), z.data());
		for (int x = 0; x < w; x++)
			grid[(size_t)y * w + x] = d[(size_t)x];
	}
}

// A glyph's distance field made from its picture drawn four times as large:
// for fonts with cubic curves (OpenType's CFF outlines), which stb_truetype's
// own distance field leaves out. Freed with free().
unsigned char *sdfFromPicture(const stbtt_fontinfo *font, float scale, int codepoint, int range, int *w, int *h,
		float *xoff, float *yoff)
{
	const int up = 4;
	int bw = 0, bh = 0, bx = 0, by = 0;
	unsigned char *picture = stbtt_GetCodepointBitmap(font, scale * up, scale * up, codepoint, &bw, &bh, &bx, &by);
	*w = *h = 0;
	if (picture == nullptr || bw <= 0 || bh <= 0)
	{
		if (picture != nullptr)
			stbtt_FreeBitmap(picture, nullptr);
		return nullptr;
	}
	const int pad = range * up;
	// A whole number of output pixels: the large picture's origin on one of them.
	const int ox0 = (int)std::floor((float)bx / up) - range, oy0 = (int)std::floor((float)by / up) - range;
	const int ox1 = (int)std::ceil((float)(bx + bw) / up) + range, oy1 = (int)std::ceil((float)(by + bh) / up) + range;
	const int ow = ox1 - ox0, oh = oy1 - oy0;
	const int gw = ow * up, gh = oh * up;
	const int sx = bx - ox0 * up, sy = by - oy0 * up;	// where the picture sits in the grid
	(void)pad;
	std::vector<float> outside((size_t)gw * gh), inside((size_t)gw * gh);
	const float far = 1e10f;
	for (int y = 0; y < gh; y++)
		for (int x = 0; x < gw; x++)
		{
			const int px = x - sx, py = y - sy;
			const bool in = px >= 0 && py >= 0 && px < bw && py < bh && picture[(size_t)py * bw + px] >= 128;
			outside[(size_t)y * gw + x] = in ? 0.f : far;
			inside[(size_t)y * gw + x] = in ? far : 0.f;
		}
	stbtt_FreeBitmap(picture, nullptr);
	distance2d(outside, gw, gh);
	distance2d(inside, gw, gh);
	unsigned char *out = (unsigned char *)malloc((size_t)ow * oh);
	for (int y = 0; y < oh; y++)
		for (int x = 0; x < ow; x++)
		{
			// The middle of the output pixel, in the large grid.
			const int gx = std::min(x * up + up / 2, gw - 1), gy = std::min(y * up + up / 2, gh - 1);
			const size_t i = (size_t)gy * gw + gx;
			const float d = (outside[i] > 0 ? std::sqrt(outside[i]) - 0.5f : -(std::sqrt(inside[i]) - 0.5f)) / (float)up;
			const float value = 128.f - d * 128.f / (float)range;
			out[(size_t)y * ow + x] = (unsigned char)std::clamp(value + 0.5f, 0.f, 255.f);
		}
	*w = ow;
	*h = oh;
	*xoff = (float)ox0;
	*yoff = (float)oy0;
	return out;
}

template <typename T> void put(std::string& out, const T& value)
{
	out.append(reinterpret_cast<const char *>(&value), sizeof(T));
}

// Shelf packing, tallest first; false when the atlas is too small.
bool pack(std::vector<Baked>& glyphs, int atlas, int& height)
{
	std::vector<Baked *> order;
	for (Baked& g : glyphs)
		if (g.bitmap != nullptr)
			order.push_back(&g);
	std::sort(order.begin(), order.end(), [](const Baked *a, const Baked *b) { return a->h > b->h; });
	int x = 1, y = 1, shelf = 0;
	for (Baked *g : order)
	{
		if (x + g->w + 1 > atlas)
		{
			x = 1;
			y += shelf + 1;
			shelf = 0;
		}
		if (y + g->h + 1 > atlas)
			return false;
		g->x = x;
		g->y = y;
		x += g->w + 1;
		shelf = std::max(shelf, g->h);
	}
	height = std::min(atlas, y + shelf + 1);
	return true;
}

} // namespace

bool bake(const std::vector<uint8_t>& ttf, std::string& out, std::string& why)
{
	out.clear();
	const float pixelSize = 48.f;
	const int range = 6;
	stbtt_fontinfo font;
	const int offset = ttf.empty() ? -1 : stbtt_GetFontOffsetForIndex(ttf.data(), 0);
	if (offset < 0 || !stbtt_InitFont(&font, ttf.data(), offset))
	{
		why = "not a TrueType or OpenType font stb_truetype can read";
		return false;
	}
	const float scale = stbtt_ScaleForMappingEmToPixels(&font, pixelSize);
	// OpenType with CFF outlines: cubic curves.
	const bool cubic = font.cff.size > 0;
	std::vector<Baked> glyphs;
	for (const int cp : codepoints())
	{
		if (cp != ' ' && stbtt_FindGlyphIndex(&font, cp) == 0)
			continue;
		Baked g;
		g.codepoint = cp;
		int advance = 0, bearing = 0;
		stbtt_GetCodepointHMetrics(&font, cp, &advance, &bearing);
		g.advance = (float)advance * scale;
		if (cubic)
		{
			g.bitmap = sdfFromPicture(&font, scale, cp, range, &g.w, &g.h, &g.xoff, &g.yoff);
			g.own = true;
		}
		else
		{
			int xoff = 0, yoff = 0;
			g.bitmap = stbtt_GetCodepointSDF(&font, scale, cp, range, 128, 128.f / (float)range, &g.w, &g.h, &xoff,
					&yoff);
			g.xoff = (float)xoff;
			g.yoff = (float)yoff;
		}
		glyphs.push_back(g);
	}
	if (glyphs.size() < 10)
	{
		for (Baked& g : glyphs)
			release(g);
		why = "the font has no Latin letters";
		return false;
	}
	int atlas = 1024, height = 0;
	while (!pack(glyphs, atlas, height) && atlas < 4096)
		atlas *= 2;
	if (height == 0)
	{
		for (Baked& g : glyphs)
			release(g);
		why = "its letters do not fit an atlas";
		return false;
	}
	std::vector<unsigned char> pixels((size_t)atlas * height, 0);
	for (const Baked& g : glyphs)
		for (int row = 0; g.bitmap != nullptr && row < g.h; ++row)
			std::memcpy(&pixels[(size_t)(g.y + row) * atlas + g.x], g.bitmap + row * g.w, (size_t)g.w);

	// Kerning from the font's table (or its GPOS pairs, which stb_truetype reads too).
	std::vector<ff::Kern> kerns;
	if (stbtt_GetKerningTableLength(&font) > 0 || font.gpos != 0)
		for (const Baked& a : glyphs)
			for (const Baked& b : glyphs)
			{
				const int kern = stbtt_GetCodepointKernAdvance(&font, a.codepoint, b.codepoint);
				if (kern != 0)
					kerns.push_back(ff::Kern{ (uint32_t)a.codepoint, (uint32_t)b.codepoint, (float)kern * scale });
			}

	int ascent = 0, descent = 0, lineGap = 0;
	stbtt_GetFontVMetrics(&font, &ascent, &descent, &lineGap);
	ff::Header header{};
	header.magic = ff::kMagic;
	header.version = ff::kVersion;
	header.atlas_width = (uint16_t)atlas;
	header.atlas_height = (uint16_t)height;
	header.pixel_size = pixelSize;
	header.sdf_range = (float)range;
	header.ascent = (float)ascent * scale;
	header.descent = (float)descent * scale;
	header.line_gap = (float)lineGap * scale;
	header.glyph_count = (uint32_t)glyphs.size();
	header.kern_count = (uint32_t)kerns.size();
	put(out, header);
	std::sort(glyphs.begin(), glyphs.end(), [](const Baked& a, const Baked& b) { return a.codepoint < b.codepoint; });
	for (const Baked& g : glyphs)
	{
		ff::Glyph record{};
		record.codepoint = (uint32_t)g.codepoint;
		record.x = (uint16_t)g.x;
		record.y = (uint16_t)g.y;
		record.w = (uint16_t)g.w;
		record.h = (uint16_t)g.h;
		record.offset_x = g.xoff;
		record.offset_y = g.yoff;
		record.advance = g.advance;
		put(out, record);
	}
	std::sort(kerns.begin(), kerns.end(), [](const ff::Kern& a, const ff::Kern& b) {
		return a.first != b.first ? a.first < b.first : a.second < b.second;
	});
	for (const ff::Kern& k : kerns)
		put(out, k);
	out.append(reinterpret_cast<const char *>(pixels.data()), pixels.size());
	for (Baked& g : glyphs)
		release(g);
	return true;
}

} // namespace fe::fontbake
