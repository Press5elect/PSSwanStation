// PSSwanStation - HD texture packs by the texture cache's blocks.
//
// The console's GPU reads textures through a small cache that holds a texture
// page in blocks (psx-spx, "GPU Texture Caching"): 64x64 texels of a 4-bit
// texture (16 halfwords x 64 rows of video memory), 32x64 texels of an 8-bit
// one (16 x 64 halfwords), 32x32 texels of a 15-bit one (32 x 32). A block is
// the unit a pack replaces here: its hash is XXH3-64 of the block's
// halfwords, row by row, followed by its palette's (16 halfwords for 4 bits,
// 256 for 8 bits; none for 15 bits), all little-endian. A pack's picture of it
// is block-<4|8|15>-<16 hex digits of the hash>.png (TextureReplacements).
//
// To hash a block the renderer needs its texels as the CPU sees them, so it
// keeps a copy of video memory beside the GPU's: CPU uploads, fills, copies
// and read backs go into it, and what the GPU draws marks the 32x32 tiles it
// touches as unknown until the CPU writes them again. A block in an unknown
// tile (a texture the game rendered) is never replaced.
//
// A primitive reading at most 2 x 2 blocks gets the pictures' atlas slots in
// its vertices (BatchVertex::replacement, replacement_slots), and the
// fragment shader samples them (batch_texture_blocks.glslinc):
//
//   replacement        bit 31     some block has a picture
//                      bit 30     the primitive reads two columns of blocks
//                      bit 29     two rows
//                      bits 8-15  the first block's row in the page
//                      bits 0-7   its column
//   replacement_slots  a byte a block (column, then row: 0 1 / 2 3), the
//                      atlas slot of its picture, 255 for none
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gpu_hw_texture_blocks.h"
#include "common/log.h"
#include "stb_image_resize.h"
#include "texture_replacements.h"
#include "xxhash.h"
#include <algorithm>
#include <cstring>
Log_SetChannel(GPU_HW);

namespace {
struct BlockShape
{
  uint32_t bits;       // 4, 8 or 15
  uint32_t texels_x;   // texels across a block
  uint32_t texels_y;   // texels down
  uint32_t halfwords;  // halfwords across a block
  uint32_t palette;    // palette entries (0 for 15 bits)
};

BlockShape ShapeOf(GPUTextureMode mode)
{
  switch (static_cast<uint32_t>(mode) & 3u)
  {
    case 0:
      return {4, 64, 64, 16, 16};
    case 1:
      return {8, 32, 64, 16, 256};
    default:
      return {15, 32, 32, 32, 0};
  }
}

uint32_t RGBA5551ToRGBA8(uint16_t c)
{
  if (c == 0)
    return 0;
  const uint32_t r = ((c & 31u) * 255u + 15u) / 31u;
  const uint32_t g = (((c >> 5) & 31u) * 255u + 15u) / 31u;
  const uint32_t b = (((c >> 10) & 31u) * 255u + 15u) / 31u;
  return r | (g << 8) | (b << 16) | 0xFF000000u;
}
} // namespace

void GPU_HW::UpdateTextureBlocksActive()
{
  const uint32_t generation = g_texture_replacements.GetGeneration();
  const bool want = SupportsTextureBlocks() &&
                    (g_texture_replacements.HasTextureBlocks() || g_texture_replacements.DumpingTextureBlocks());
  if (!want)
  {
    if (m_texture_blocks)
    {
      m_texture_blocks_active = false;
      m_texture_blocks.reset();
    }
    return;
  }
  if (m_texture_blocks_active && generation == m_texture_blocks_generation)
    return;

  FlushRender();
  if (!m_texture_blocks || generation != m_texture_blocks_generation)
  {
    // Another game or pack: nothing of the old one is kept.
    m_texture_blocks = std::make_unique<TextureBlockState>();
  }
  m_texture_blocks->blocks.clear();
  m_texture_blocks_generation = generation;
  m_texture_blocks_active = true;

  // Learn video memory as it is now (ReadVRAM ends in TextureBlocksReadBack).
  ReadVRAM(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
  Log_InfoPrintf("Texture blocks on (%s)", g_texture_replacements.HasTextureBlocks() ? "a pack" : "writing out");
}

namespace {
// Marks the tiles a rectangle (wrapping at the edges of video memory)
// touches as changed; those it covers whole get known (or unknown) as asked.
template<typename State>
void TouchTiles(State& s, uint32_t x, uint32_t y, uint32_t width, uint32_t height, int whole_known)
{
  if (width == 0 || height == 0)
    return;
  const uint32_t T = State::TILE;
  const uint32_t epoch = ++s.epoch;
  for (uint32_t ty = y / T; ty <= (y + height - 1) / T; ty++)
  {
    const uint32_t y0 = std::max(y, ty * T), y1 = std::min(y + height, ty * T + T);
    for (uint32_t tx = x / T; tx <= (x + width - 1) / T; tx++)
    {
      const uint32_t x0 = std::max(x, tx * T), x1 = std::min(x + width, tx * T + T);
      const uint32_t index = (ty % State::TILES_Y) * State::TILES_X + (tx % State::TILES_X);
      s.tile_epoch[index] = epoch;
      if (whole_known >= 0 && (x1 - x0) == T && (y1 - y0) == T)
        s.tile_drawn[index] = (whole_known == 0);
    }
  }
}
} // namespace

void GPU_HW::MarkTextureBlockTilesDrawnSlow(uint32_t left, uint32_t right, uint32_t top, uint32_t bottom)
{
  TextureBlockState& s = *m_texture_blocks;
  if (right <= left || bottom <= top)
    return;
  right = std::min(right, VRAM_WIDTH);
  bottom = std::min(bottom, VRAM_HEIGHT);
  const uint32_t T = TextureBlockState::TILE;
  const uint32_t epoch = ++s.epoch;
  for (uint32_t ty = top / T; ty <= (bottom - 1) / T; ty++)
  {
    for (uint32_t tx = left / T; tx <= (right - 1) / T; tx++)
    {
      const uint32_t index = ty * TextureBlockState::TILES_X + tx;
      s.tile_epoch[index] = epoch;
      s.tile_drawn[index] = true;
    }
  }
}

void GPU_HW::TextureBlocksFill(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color)
{
  if (!m_texture_blocks_active)
    return;
  TextureBlockState& s = *m_texture_blocks;
  uint16_t* const vram = m_vram_ptr;
  m_vram_ptr = s.vram.data();
  GPU::FillVRAM(x, y, width, height, color);
  m_vram_ptr = vram;
  // An interlaced fill leaves every other line as it was.
  TouchTiles(s, x, y, width, height, IsInterlacedRenderingEnabled() ? -1 : 1);
}

void GPU_HW::TextureBlocksUpdate(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* data,
                                 bool set_mask, bool check_mask)
{
  if (!m_texture_blocks_active)
    return;
  TextureBlockState& s = *m_texture_blocks;
  uint16_t* const vram = m_vram_ptr;
  m_vram_ptr = s.vram.data();
  GPU::UpdateVRAM(x, y, width, height, data, set_mask, check_mask);
  m_vram_ptr = vram;
  // With the mask test, pixels the GPU drew may have been kept.
  TouchTiles(s, x, y, width, height, check_mask ? -1 : 1);
}

void GPU_HW::TextureBlocksCopy(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t width,
                               uint32_t height)
{
  if (!m_texture_blocks_active || width == 0 || height == 0)
    return;
  TextureBlockState& s = *m_texture_blocks;
  const uint16_t mask_and = m_GPUSTAT.GetMaskAND();
  const uint16_t mask_or = m_GPUSTAT.GetMaskOR();

  // As GPU::CopyVRAM: right to left when the copy moves right.
  const bool reverse = src_x < dst_x || ((src_x + width - 1) % VRAM_WIDTH) < ((dst_x + width - 1) % VRAM_WIDTH);
  for (uint32_t row = 0; row < height; row++)
  {
    const uint16_t* src_row = &s.vram[((src_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
    uint16_t* dst_row = &s.vram[((dst_y + row) % VRAM_HEIGHT) * VRAM_WIDTH];
    for (uint32_t i = 0; i < width; i++)
    {
      const uint32_t col = reverse ? (width - 1 - i) : i;
      uint16_t& dst = dst_row[(dst_x + col) % VRAM_WIDTH];
      if ((dst & mask_and) == 0)
        dst = src_row[(src_x + col) % VRAM_WIDTH] | mask_or;
    }
  }

  // The copy is known where its source was.
  bool source_drawn = false;
  const uint32_t T = TextureBlockState::TILE;
  for (uint32_t ty = src_y / T; ty <= (src_y + height - 1) / T && !source_drawn; ty++)
  {
    for (uint32_t tx = src_x / T; tx <= (src_x + width - 1) / T && !source_drawn; tx++)
      source_drawn = s.tile_drawn[(ty % TextureBlockState::TILES_Y) * TextureBlockState::TILES_X +
                                  (tx % TextureBlockState::TILES_X)];
  }
  if (source_drawn)
  {
    // Every tile it touches is unknown now, not only those it covers.
    for (uint32_t ty = dst_y / T; ty <= (dst_y + height - 1) / T; ty++)
    {
      for (uint32_t tx = dst_x / T; tx <= (dst_x + width - 1) / T; tx++)
        s.tile_drawn[(ty % TextureBlockState::TILES_Y) * TextureBlockState::TILES_X + (tx % TextureBlockState::TILES_X)] =
          true;
    }
    TouchTiles(s, dst_x, dst_y, width, height, -1);
  }
  else
  {
    TouchTiles(s, dst_x, dst_y, width, height, mask_and ? -1 : 1);
  }
}

void GPU_HW::TextureBlocksReadBack(const Common::Rectangle<uint32_t>& rect)
{
  if (!m_texture_blocks_active || !rect.Valid())
    return;
  TextureBlockState& s = *m_texture_blocks;
  const uint32_t right = std::min(rect.right, VRAM_WIDTH), bottom = std::min(rect.bottom, VRAM_HEIGHT);
  if (right <= rect.left || bottom <= rect.top)
    return;
  for (uint32_t y = rect.top; y < bottom; y++)
    std::memcpy(&s.vram[y * VRAM_WIDTH + rect.left], &m_vram_ptr[y * VRAM_WIDTH + rect.left],
                (right - rect.left) * sizeof(uint16_t));
  TouchTiles(s, rect.left, rect.top, right - rect.left, bottom - rect.top, 1);
}

bool GPU_HW::LookupTextureBlocks(uint32_t min_u, uint32_t max_u, uint32_t min_v, uint32_t max_v,
                                 uint32_t* replacement, uint32_t* replacement_slots)
{
  if (!m_texture_blocks_active)
    return false;
  const GPUTextureWindow& window = m_draw_mode.texture_window;
  if (window.and_x != 0xFF || window.and_y != 0xFF || window.or_x != 0 || window.or_y != 0)
    return false;

  TextureBlockState& s = *m_texture_blocks;
  const BlockShape shape = ShapeOf(m_draw_mode.mode_reg.texture_mode);
  const uint32_t bx0 = min_u / shape.texels_x, bx1 = max_u / shape.texels_x;
  const uint32_t by0 = min_v / shape.texels_y, by1 = max_v / shape.texels_y;
  if (bx1 < bx0 || by1 < by0 || bx1 - bx0 > 1 || by1 - by0 > 1)
    return false;

  const uint32_t page_x = m_draw_mode.texture_page_x, page_y = m_draw_mode.texture_page_y;
  const uint32_t clut_x = shape.palette ? m_draw_mode.texture_palette_x : 0;
  const uint32_t clut_y = shape.palette ? m_draw_mode.texture_palette_y : 0;
  const bool dumping = g_texture_replacements.DumpingTextureBlocks();
  const uint32_t T = TextureBlockState::TILE;

  uint32_t slots = 0xFFFFFFFFu;
  bool any = false;
  for (uint32_t by = by0; by <= by1; by++)
  {
    for (uint32_t bx = bx0; bx <= bx1; bx++)
    {
      const uint32_t vx = (page_x + bx * shape.halfwords) % VRAM_WIDTH;
      const uint32_t vy = page_y + by * shape.texels_y;
      if (vy + shape.texels_y > VRAM_HEIGHT)
        continue;

      // Where the block and its palette lie; when they last changed; known?
      uint32_t latest = 0;
      bool drawn = false;
      for (uint32_t ty = vy / T; ty <= (vy + shape.texels_y - 1) / T; ty++)
      {
        const uint32_t index = ty * TextureBlockState::TILES_X + vx / T;
        latest = std::max(latest, s.tile_epoch[index]);
        drawn |= s.tile_drawn[index];
      }
      if (shape.palette)
      {
        for (uint32_t tx = clut_x / T; tx <= (clut_x + shape.palette - 1) / T; tx++)
        {
          const uint32_t index = (clut_y / T) * TextureBlockState::TILES_X + (tx % TextureBlockState::TILES_X);
          latest = std::max(latest, s.tile_epoch[index]);
          drawn |= s.tile_drawn[index];
        }
      }

      const uint64_t key = (static_cast<uint64_t>(shape.bits) << 56) | (static_cast<uint64_t>(vx) << 40) |
                           (static_cast<uint64_t>(vy) << 28) | (static_cast<uint64_t>(clut_x) << 12) | clut_y;
      TextureBlockState::Block& block = s.blocks[key];
      if (block.epoch == 0 || block.epoch < latest)
      {
        block.epoch = s.epoch;
        block.valid = !drawn;
        block.has_pack = false;
        if (block.valid)
        {
          // Its halfwords row by row, then its palette's.
          uint16_t data[32 * 32 + 256];
          uint32_t n = 0;
          for (uint32_t row = 0; row < shape.texels_y; row++)
          {
            const uint16_t* src = &s.vram[(vy + row) * VRAM_WIDTH];
            for (uint32_t col = 0; col < shape.halfwords; col++)
              data[n++] = src[(vx + col) % VRAM_WIDTH];
          }
          for (uint32_t i = 0; i < shape.palette; i++)
            data[n++] = s.vram[clut_y * VRAM_WIDTH + (clut_x + i) % VRAM_WIDTH];
          block.hash = XXH3_64bits(data, n * sizeof(uint16_t));
          block.has_pack = g_texture_replacements.HasTextureBlock(shape.bits, block.hash);

          if (dumping)
          {
            // Its texels as RGBA, for pack makers (once a hash).
            std::vector<uint32_t> rgba(shape.texels_x * shape.texels_y);
            for (uint32_t y = 0; y < shape.texels_y; y++)
            {
              const uint16_t* row = &data[y * shape.halfwords];
              for (uint32_t x = 0; x < shape.texels_x; x++)
              {
                uint16_t c;
                if (shape.bits == 4)
                  c = data[shape.texels_y * shape.halfwords + ((row[x / 4] >> ((x & 3) * 4)) & 15u)];
                else if (shape.bits == 8)
                  c = data[shape.texels_y * shape.halfwords + ((row[x / 2] >> ((x & 1) * 8)) & 255u)];
                else
                  c = row[x];
                rgba[y * shape.texels_x + x] = RGBA5551ToRGBA8(c);
              }
            }
            g_texture_replacements.DumpTextureBlock(shape.bits, block.hash, rgba.data(), shape.texels_x,
                                                    shape.texels_y);
          }
        }
      }
      if (!block.valid || !block.has_pack)
        continue;

      // Its picture's slot in the atlas, uploading it there when it is not.
      const uint64_t pkey = (static_cast<uint64_t>(shape.bits) << 56) ^ block.hash;
      uint32_t slot;
      const auto resident = s.resident.find(pkey);
      if (resident != s.resident.end())
      {
        slot = resident->second;
      }
      else
      {
        if (s.failed.count(pkey))
          continue;
        TextureReplacementTexture picture;
        if (!g_texture_replacements.LoadTextureBlock(shape.bits, block.hash, &picture))
        {
          s.failed.emplace(pkey, true);
          continue;
        }
        std::vector<uint32_t> scaled(TEXTURE_BLOCK_SLOT_SIZE * TEXTURE_BLOCK_SLOT_SIZE);
        if (picture.GetWidth() == TEXTURE_BLOCK_SLOT_SIZE && picture.GetHeight() == TEXTURE_BLOCK_SLOT_SIZE)
        {
          for (uint32_t y = 0; y < TEXTURE_BLOCK_SLOT_SIZE; y++)
            std::memcpy(&scaled[y * TEXTURE_BLOCK_SLOT_SIZE],
                        reinterpret_cast<const uint8_t*>(picture.GetPixels()) + y * picture.GetByteStride(),
                        TEXTURE_BLOCK_SLOT_SIZE * sizeof(uint32_t));
        }
        else if (!stbir_resize_uint8(reinterpret_cast<const unsigned char*>(picture.GetPixels()),
                                     static_cast<int>(picture.GetWidth()), static_cast<int>(picture.GetHeight()),
                                     static_cast<int>(picture.GetByteStride()),
                                     reinterpret_cast<unsigned char*>(scaled.data()), TEXTURE_BLOCK_SLOT_SIZE,
                                     TEXTURE_BLOCK_SLOT_SIZE, 0, 4))
        {
          s.failed.emplace(pkey, true);
          continue;
        }

        // A free slot, or the one used longest ago.
        slot = 0;
        for (uint32_t i = 0; i < TEXTURE_BLOCK_SLOTS; i++)
        {
          if (s.slots[i].key == 0)
          {
            slot = i;
            break;
          }
          if (s.slots[i].last_use < s.slots[slot].last_use)
            slot = i;
        }
        // The batch being built may still read the old picture there.
        if (s.slots[slot].key != 0 && s.slots[slot].flush_serial == m_flush_serial)
        {
          FlushRender();
          EnsureVertexBufferSpaceForCurrentCommand();
        }
        const bool uploaded = UploadTextureBlock(slot, scaled.data());
        // (Making the atlas the first time sends the batch to the GPU.)
        if (!m_batch_current_vertex_ptr)
          EnsureVertexBufferSpaceForCurrentCommand();
        if (!uploaded)
          continue;
        if (s.slots[slot].key != 0)
          s.resident.erase(s.slots[slot].key);
        s.slots[slot].key = pkey;
        s.resident.emplace(pkey, static_cast<uint8_t>(slot));
      }
      s.slots[slot].last_use = ++s.use_counter;
      s.slots[slot].flush_serial = m_flush_serial;

      const uint32_t index = (bx - bx0) + 2 * (by - by0);
      slots = (slots & ~(0xFFu << (index * 8))) | (slot << (index * 8));
      any = true;
    }
  }
  if (!any)
    return false;

  *replacement = 0x80000000u | ((bx1 > bx0) ? (1u << 30) : 0u) | ((by1 > by0) ? (1u << 29) : 0u) | (by0 << 8) | bx0;
  *replacement_slots = slots;
  return true;
}
