/*
	SwanStation for PS5 - the display: Vulkan on RADV, the swapchain, Dear ImGui.

	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace fe::display
{

// The instance, the display surface, the device (the emulator's Vulkan
// context, made once for the whole run), the swapchain and ImGui.
bool init();
void shutdown();

int width();
int height();
// What the display's mode says, in Hz.
float refreshRate();
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
// Nearest or linear sampling for the game's picture.
void setLinear(bool linear);

// The last frame as a PNG (the test runs' evidence).
bool saveScreenshot(const std::string& path);

// For the emulator's libretro Vulkan interface.
void *vkInstance();

}
