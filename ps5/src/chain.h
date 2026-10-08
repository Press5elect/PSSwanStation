/*
	PSSwanStation - libretro slang shader presets, run on the game's picture.

	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include "vulkan_loader.h"

#include <string>

namespace fe::chain
{

// A preset compiled by ps5/tools/make-chains.py: `folder` holds its chain.txt,
// its passes' SPIR-V and its lookup pictures. Null when it cannot be read or
// the driver refuses a part of it (the log says which).
struct Chain;
Chain *load(const std::string& folder);
void destroy(Chain *chain);

// Every parameter to the preset's value, then one by name (false: the preset
// has none of that name).
void resetParameters(Chain *chain);
bool setParameter(Chain *chain, const std::string& name, float value);

// What the passes keep from frame to frame (afterglow, average brightness)
// starts anew: another game.
void forget(Chain *chain);

// Draws the preset over `input` (a `width` x `height` picture, all of the
// image) into a picture of `outWidth` x `outHeight`, recorded into `cmd`.
// Returns that picture's view, left readable by a shader; null when it cannot
// be done. The view stays the same while the sizes do.
VkImageView run(Chain *chain, VkCommandBuffer cmd, VkImageView input, VkImageLayout inputLayout, int width, int height,
		int outWidth, int outHeight);

}
