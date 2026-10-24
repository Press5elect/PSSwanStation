/*
	PSSwanStation - pictures for the interface: covers, the logo, a theme's
	pictures.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	A cover is a PNG or JPEG file; decoding one takes a few milliseconds, too
	long for the thread that also runs the frame, so a thread of its own
	decodes them and the frame only uploads the pixels (two pictures a frame at
	most). A picture is kept while it is shown and for a while after; the ones
	longest unused go when there are many.

	A cover is made no larger than it is drawn (512 pixels); a picture asked
	for at a size of its own (an ES-DE theme's background, as wide as the
	screen) is kept at that size, under its own name. An SVG file (the
	drawings ES-DE themes are made of) is drawn into pixels at the size asked
	for, by nanosvg.
*/
#include "ui.h"
#include "tuning.h"
#include "fe.h"
#include "display.h"

#include <stb_image.h>
#include <stb_image_resize.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include <cstdio>
#include <cstring>
#include <cmath>
#include <nanosvg.h>
#include <nanosvgrast.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace fe::ui
{
namespace
{

// Covers are drawn about 460 pixels across on a 2160-line display.
constexpr int MaxSide = 512;
// A theme's picture: never larger than this.
constexpr int LargestSide = 4096;
constexpr size_t MaxKept = 220;

constexpr int SoftSide = 24;

struct Entry
{
	display::Texture *texture = nullptr;
	display::Texture *soft = nullptr;
	int width = 0, height = 0;
	uint64_t used = 0;
	bool pending = false;
};

struct Request
{
	std::string key, path;
	int side = MaxSide;
	bool padded = false;	// a clear border round it (pictures turned on the screen)
};

struct Decoded
{
	std::string path;	// the entry's key
	int width = 0, height = 0;
	std::vector<uint8_t> pixels;
	std::vector<uint8_t> soft;		// SoftSide x SoftSide
};

// The picture as a few blurred pixels.
void soften(Decoded& out)
{
	std::vector<uint8_t> small((size_t)SoftSide * SoftSide * 4);
	stbir_resize_uint8(out.pixels.data(), out.width, out.height, 0, small.data(), SoftSide, SoftSide, 0, 4);
	std::vector<uint8_t> blurred(small.size());
	for (int pass = 0; pass < 3; pass++)
	{
		for (int y = 0; y < SoftSide; y++)
			for (int x = 0; x < SoftSide; x++)
				for (int c = 0; c < 4; c++)
				{
					int sum = 0;
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++)
							sum += small[((size_t)std::clamp(y + dy, 0, SoftSide - 1) * SoftSide
									+ (size_t)std::clamp(x + dx, 0, SoftSide - 1)) * 4 + c];
					blurred[((size_t)y * SoftSide + x) * 4 + c] = (uint8_t)(sum / 9);
				}
		small.swap(blurred);
	}
	for (size_t i = 3; i < small.size(); i += 4)
		small[i] = 255;
	out.soft = std::move(small);
}

std::map<std::string, Entry> entries;
std::mutex mutex;
std::condition_variable wake;
std::deque<Request> wanted;
std::deque<Decoded> ready;
std::thread worker;
bool workerStarted;

bool isSvg(const std::string& path)
{
	if (path.size() < 4)
		return false;
	std::string ext = path.substr(path.size() - 4);
	for (char& c : ext)
		c = (char)std::tolower((unsigned char)c);
	return ext == ".svg";
}

// Decodes to RGBA and makes it no larger than `side` on its longest side.
bool decode(const uint8_t *data, size_t size, Decoded& out, int side = MaxSide)
{
	int w = 0, h = 0, channels = 0;
	stbi_uc *pixels = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
	if (pixels == nullptr || w <= 0 || h <= 0)
	{
		if (pixels != nullptr)
			stbi_image_free(pixels);
		return false;
	}
	const int longest = std::max(w, h);
#if defined(SWANSTATION_HOST)
	// The frame generation test's pictures are a game's: as large as they are.
	static const bool whole = getenv("SWANSTATION_FG_TEST") != nullptr;
#else
	const bool whole = false;
#endif
	if (longest > side && !whole)
	{
		const int nw = std::max(1, (int)((int64_t)w * side / longest)), nh = std::max(1, (int)((int64_t)h * side / longest));
		out.pixels.resize((size_t)nw * nh * 4);
		stbir_resize_uint8(pixels, w, h, 0, out.pixels.data(), nw, nh, 0, 4);
		out.width = nw;
		out.height = nh;
	}
	else
	{
		out.pixels.assign(pixels, pixels + (size_t)w * h * 4);
		out.width = w;
		out.height = h;
	}
	stbi_image_free(pixels);
	return true;
}

// An SVG drawing in pixels, `side` pixels on its longest side.
bool decodeSvg(std::vector<uint8_t>& file, Decoded& out, int side)
{
	file.push_back(0);
	NSVGimage *svg = nsvgParse(reinterpret_cast<char *>(file.data()), "px", 96.f);
	if (svg == nullptr)
		return false;
	bool ok = false;
	if (svg->width > 0 && svg->height > 0)
	{
		const float scale = (float)side / std::max(svg->width, svg->height);
		const int w = std::max(1, (int)std::lround(svg->width * scale));
		const int h = std::max(1, (int)std::lround(svg->height * scale));
		if (NSVGrasterizer *rast = nsvgCreateRasterizer())
		{
			out.pixels.assign((size_t)w * h * 4, 0);
			nsvgRasterize(rast, svg, 0, 0, scale, out.pixels.data(), w, h, w * 4);
			nsvgDeleteRasterizer(rast);
			out.width = w;
			out.height = h;
			ok = true;
		}
	}
	nsvgDelete(svg);
	return ok;
}

// A clear border of PictureBorder pixels round the picture: drawn turned
// inside a larger box, what lies past its edges samples the border (the
// display clamps to the edge), so nothing of the picture is smeared outwards.
void pad(Decoded& d)
{
	const int b = PictureBorder;
	const int w = d.width + b * 2, h = d.height + b * 2;
	std::vector<uint8_t> out((size_t)w * h * 4, 0);
	for (int y = 0; y < d.height; y++)
		memcpy(&out[((size_t)(y + b) * w + b) * 4], &d.pixels[(size_t)y * d.width * 4], (size_t)d.width * 4);
	d.pixels.swap(out);
	d.width = w;
	d.height = h;
}

void work()
{
	for (;;)
	{
		Request request;
		{
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait(lock, [] { return !wanted.empty(); });
			// The newest request first: it is what the screen shows now.
			request = wanted.back();
			wanted.pop_back();
		}
		Decoded decoded;
		decoded.path = request.key;
		std::vector<uint8_t> file;
		// A theme's picture may be on a network share (media.cpp).
		const bool read = smb::isNetworkPath(request.path) ? smb::readWhole(request.path, file)
				: readFile(request.path, file);
		if (read && !file.empty())
		{
			const bool done = isSvg(request.path) ? decodeSvg(file, decoded, request.side)
					: decode(file.data(), file.size(), decoded, request.side);
			if (done)
			{
				soften(decoded);
				if (request.padded)
					pad(decoded);
			}
			else
				decoded.pixels.clear();
		}
		std::lock_guard<std::mutex> lock(mutex);
		ready.push_back(std::move(decoded));
	}
}

Image lookup(const std::string& key, const std::string& path, int side, bool padded = false)
{
	Entry& entry = entries[key];
	entry.used = display::frameCount();
	if (entry.texture != nullptr)
		return { display::textureId(entry.texture), entry.width, entry.height,
				entry.soft != nullptr ? display::textureId(entry.soft) : nullptr };
	if (!entry.pending && entry.width == 0)
	{
		entry.pending = true;
		if (!workerStarted)
		{
			workerStarted = true;
			worker = std::thread(tuning::background(work));
			worker.detach();
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			wanted.push_back({ key, path, side, padded });
		}
		wake.notify_one();
	}
	return {};
}

}

Image image(const std::string& path)
{
	if (path.empty())
		return {};
	return lookup(path, path, MaxSide);
}

Image image(const std::string& path, int side)
{
	if (path.empty())
		return {};
	// Sizes in steps, so a picture drawn a little larger or smaller is not read again.
	int step = 64;
	while (step < side && step < LargestSide)
		step *= 2;
	side = std::clamp(step, 32, LargestSide);
	if (side == MaxSide && !isSvg(path))
		return lookup(path, path, MaxSide);
	return lookup(path + "@" + std::to_string(side), path, side);
}

Image imagePadded(const std::string& path, int side)
{
	if (path.empty())
		return {};
	int step = 64;
	while (step < side && step < LargestSide)
		step *= 2;
	side = std::clamp(step, 32, LargestSide);
	return lookup(path + "@" + std::to_string(side) + "+border", path, side, true);
}

bool imageFailed(const std::string& path)
{
	for (auto it = entries.lower_bound(path); it != entries.end() && it->first.compare(0, path.size(), path) == 0; ++it)
		if ((it->first.size() == path.size() || it->first[path.size()] == '@') && it->second.width < 0)
			return true;
	return false;
}

void forgetImage(const std::string& path)
{
	for (auto it = entries.begin(); it != entries.end();)
	{
		if (it->first == path || (it->first.size() > path.size() && it->first.compare(0, path.size(), path) == 0
				&& it->first[path.size()] == '@'))
		{
			display::destroyTexture(it->second.texture);
			display::destroyTexture(it->second.soft);
			it = entries.erase(it);
		}
		else
			++it;
	}
}

Image imageFromPixels(const std::string& key, const uint32_t *pixels, int width, int height, int enlarge)
{
	if (key.empty() || width <= 0 || height <= 0)
		return {};
	// No pixels: the picture made before under this key, if there is one.
	if (pixels == nullptr)
	{
		const auto found = entries.find("pixels:" + key);
		if (found == entries.end() || found->second.texture == nullptr)
			return {};
		found->second.used = display::frameCount();
		return { display::textureId(found->second.texture), found->second.width, found->second.height, nullptr };
	}
	Entry& entry = entries["pixels:" + key];
	entry.used = display::frameCount();
	if (entry.texture == nullptr)
	{
		// Enlarged by whole pixels first: the display's sampling then leaves
		// the picture's squares as squares.
		enlarge = std::clamp(enlarge, 1, 16);
		std::vector<uint32_t> large((size_t)width * enlarge * height * enlarge);
		for (int y = 0; y < height * enlarge; y++)
			for (int x = 0; x < width * enlarge; x++)
				large[(size_t)y * width * enlarge + x] = pixels[(size_t)(y / enlarge) * width + x / enlarge];
		entry.texture = display::createTexture(width * enlarge, height * enlarge,
				reinterpret_cast<const uint8_t *>(large.data()));
		entry.width = width * enlarge;
		entry.height = height * enlarge;
	}
	return { display::textureId(entry.texture), entry.width, entry.height, nullptr };
}

Image imageFromMemory(const uint8_t *data, size_t size)
{
	Decoded decoded;
	if (data == nullptr || !decode(data, size, decoded))
		return {};
	display::Texture *texture = display::createTexture(decoded.width, decoded.height, decoded.pixels.data());
	if (texture == nullptr)
		return {};
	return { display::textureId(texture), decoded.width, decoded.height, nullptr };
}

void imagesFrame()
{
	for (int uploads = 0; uploads < 2; uploads++)
	{
		Decoded decoded;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (ready.empty())
				break;
			decoded = std::move(ready.front());
			ready.pop_front();
		}
		const auto it = entries.find(decoded.path);
		if (it == entries.end())
			continue;
		Entry& entry = it->second;
		entry.pending = false;
		if (decoded.pixels.empty())
		{
			// No picture: -1 keeps it from being asked for again.
			entry.width = entry.height = -1;
			continue;
		}
		// A file asked for again while its first reading was under way.
		display::destroyTexture(entry.texture);
		display::destroyTexture(entry.soft);
		entry.soft = nullptr;
		entry.texture = display::createTexture(decoded.width, decoded.height, decoded.pixels.data());
		if (!decoded.soft.empty())
			entry.soft = display::createTexture(SoftSide, SoftSide, decoded.soft.data());
		entry.width = decoded.width;
		entry.height = decoded.height;
	}

	if (entries.size() <= MaxKept)
		return;
	const uint64_t frame = display::frameCount();
	// The oldest quarter of what the screen has not shown for two seconds.
	std::vector<std::pair<uint64_t, std::string>> idle;
	for (const auto& [path, entry] : entries)
		if (!entry.pending && entry.used + 120 < frame)
			idle.emplace_back(entry.used, path);
	std::sort(idle.begin(), idle.end());
	const size_t drop = std::min(idle.size(), entries.size() - MaxKept + MaxKept / 4);
	for (size_t i = 0; i < drop; i++)
	{
		const auto it = entries.find(idle[i].second);
		display::destroyTexture(it->second.texture);
		display::destroyTexture(it->second.soft);
		entries.erase(it);
	}
}

}
