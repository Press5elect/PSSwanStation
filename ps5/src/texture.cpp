/*
	PSSwanStation - pictures for the interface: covers, the logo.

	SPDX-License-Identifier: GPL-3.0-or-later

	A cover is a PNG or JPEG file; decoding one takes a few milliseconds, too
	long for the thread that also runs the frame, so a thread of its own
	decodes them and the frame only uploads the pixels (two pictures a frame at
	most). A picture is kept while it is shown and for a while after; the ones
	longest unused go when there are many.
*/
#include "ui.h"
#include "display.h"

#include <stb_image.h>
#include <stb_image_resize.h>

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

struct Decoded
{
	std::string path;
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
std::deque<std::string> wanted;
std::deque<Decoded> ready;
std::thread worker;
bool workerStarted;

// Decodes to RGBA and makes it no larger than a cover is drawn.
bool decode(const uint8_t *data, size_t size, Decoded& out)
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
	if (longest > MaxSide)
	{
		const int nw = std::max(1, w * MaxSide / longest), nh = std::max(1, h * MaxSide / longest);
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

void work()
{
	for (;;)
	{
		std::string path;
		{
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait(lock, [] { return !wanted.empty(); });
			// The newest request first: it is what the screen shows now.
			path = wanted.back();
			wanted.pop_back();
		}
		Decoded decoded;
		decoded.path = path;
		std::vector<uint8_t> file;
		if (readFile(path, file) && !file.empty() && decode(file.data(), file.size(), decoded))
			soften(decoded);
		std::lock_guard<std::mutex> lock(mutex);
		ready.push_back(std::move(decoded));
	}
}

}

Image image(const std::string& path)
{
	if (path.empty())
		return {};
	Entry& entry = entries[path];
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
			worker = std::thread(work);
			worker.detach();
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			wanted.push_back(path);
		}
		wake.notify_one();
	}
	return {};
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
