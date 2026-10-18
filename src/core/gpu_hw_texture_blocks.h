// PSSwanStation - HD texture packs by the texture cache's blocks: the
// renderer's state (gpu_hw_texture_blocks.cpp).
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gpu_hw.h"
#include <array>
#include <unordered_map>
#include <vector>

struct GPU_HW::TextureBlockState
{
  // Video memory is followed in tiles of 32 x 32 halfwords.
  static constexpr uint32_t TILE = 32, TILES_X = VRAM_WIDTH / TILE, TILES_Y = VRAM_HEIGHT / TILE;

  // Video memory as the CPU wrote it (uploads, fills, copies, read backs).
  std::vector<uint16_t> vram = std::vector<uint16_t>(VRAM_WIDTH * VRAM_HEIGHT, 0);
  // When each tile last changed (a counter), and whether the GPU drew into it
  // since its contents were last known here.
  std::array<uint32_t, TILES_X * TILES_Y> tile_epoch{};
  std::array<bool, TILES_X * TILES_Y> tile_drawn{};
  uint32_t epoch = 1;

  // A block as the GPU reads it: its mode and place, and its palette's.
  struct Block
  {
    uint32_t epoch;   // when the hash was taken
    uint64_t hash;
    bool valid;       // the block was known (not drawn by the GPU)
    bool has_pack;    // the pack has a picture of it
  };
  std::unordered_map<uint64_t, Block> blocks;

  // The atlas: which picture each slot holds, when it was last used.
  struct Slot
  {
    uint64_t key = 0; // (bits << 56) ^ hash, 0 when empty
    uint64_t last_use = 0;
    uint32_t flush_serial = ~0u;
  };
  std::array<Slot, TEXTURE_BLOCK_SLOTS> slots{};
  std::unordered_map<uint64_t, uint8_t> resident;
  std::unordered_map<uint64_t, bool> failed; // pictures that would not load
  uint64_t use_counter = 0;
};
