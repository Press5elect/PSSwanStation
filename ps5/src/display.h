/*
	PSSwanStation - the display: Vulkan on RADV, the swapchain, Dear ImGui.

	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fe::display
{

// The instance, the display surface, the device (the emulator's Vulkan
// context, made once for the whole run), the swapchain and ImGui.
bool init();
void shutdown();

int width();
int height();
// How often a picture goes to the screen, in Hz: the display mode's rate (or
// half of it, when each picture is held for two refreshes).
float refreshRate();
// What the output itself refreshes at.
float outputRefreshRate();
// The interface's scale: 1 at 1080 lines.
float scale();
std::string deviceName();

// Starts a frame: waits for a swapchain image, begins ImGui's frame.
bool beginFrame();
// Ends it: ImGui's draw data into the swapchain image, submit, present.
void endFrame();
uint64_t frameCount();

// A picture for ImGui to draw.
struct Texture;
// From RGBA8 pixels (covers, the logo).
Texture *createTexture(int width, int height, const uint8_t *rgba);
// One that is written every frame (the software renderer's picture).
enum PixelFormat { Xrgb8888, Rgb565, Xrgb1555 };
Texture *createDynamicTexture(int width, int height);
void updateTexture(Texture *texture, const void *pixels, int width, int height, size_t pitch, PixelFormat format);
void destroyTexture(Texture *texture);
void *textureId(const Texture *texture);
int textureWidth(const Texture *texture);
int textureHeight(const Texture *texture);

// The hardware renderer's picture: an image view of the emulator's, made
// drawable by ImGui. The id stays the same while the view does.
void *wrapView(void *imageView, int layout);
void releaseWrapped();
// Nearest or linear sampling for what is drawn next into an ImDrawList (the
// game's picture); linear is what everything else is drawn with.
void sampling(void *drawList, bool nearest);

// The picture `texture` (its part up to u, v) alone, drawn `width` x `height`
// and read back as RGBA8. Slow: it waits for the graphics processor. Only
// between a frame's begin and its end.
bool capture(void *texture, float u, float v, int width, int height, std::vector<uint8_t>& rgba);
// A few soft pixels of `texture`, averaged over the last half second, for
// ImGui to stretch over the screen; null when it cannot be made.
void *ambient(void *texture, float u, float v);
// The next one starts anew (another game).
void forgetAmbient();
// The picture `texture` (its part up to u, v, which is `width` x `height`
// pixels) made `outWidth` x `outHeight` with FSR 1: AMD's edge-adaptive
// upscale, then its sharpening (`sharpness`: 0 soft, 1 normal, 2 sharp). The
// result, for ImGui to draw pixel for pixel; null when the picture is not
// smaller than that, or it cannot be done: the picture is then drawn the
// usual way. Only between a frame's begin and its end.
void *upscale(void *texture, int width, int height, float u, float v, int outWidth, int outHeight, int sharpness);
// Frame generation: a picture between the game's last and this one. `texture`
// is the game's picture as it is about to be drawn (its part up to u, v,
// `width` x `height` pixels; FSR's result when that is on). `fresh`: it is a
// new one since the last call, and is kept. `phase`: where the screen is now
// between the picture before (0) and this one (1). Returns what to draw in its
// place, the whole of a texture of the same size: the picture in between, or
// (at phase 1, or with no picture before) this one as it was kept. Null when
// it cannot be done: the picture is then drawn as it is. Only between a
// frame's begin and its end.
// `lighter`: with less work for the graphics processor, and less care.
void *generated(void *texture, int width, int height, float u, float v, bool fresh, float phase, bool lighter = false);
// The next picture has no picture before it (another game, a state loaded).
void forgetGenerated();
// RGBA8 pixels as a PNG file. May be called on any thread.
bool writePng(const std::string& path, const uint8_t *rgba, int width, int height);

// The last frame as a PNG (the test runs' evidence).
bool saveScreenshot(const std::string& path);

// For the emulator's libretro Vulkan interface.
void *vkInstance();

}
