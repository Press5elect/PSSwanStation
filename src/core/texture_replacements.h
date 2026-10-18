#pragma once
#include "common/hash_combine.h"
#include "common/image.h"
#include "types.h"
#include <string>
#include <unordered_map>
#include <vector>

struct TextureReplacementHash
{
  uint64_t low;
  uint64_t high;

  std::string ToString() const;
  bool ParseString(const std::string_view& sv);

  bool operator==(const TextureReplacementHash& rhs) const { return low == rhs.low && high == rhs.high; }
};

namespace std {
template<>
struct hash<TextureReplacementHash>
{
  size_t operator()(const TextureReplacementHash& h) const
  {
    size_t hash_hash = std::hash<uint64_t>{}(h.low);
    hash_combine(hash_hash, h.high);
    return hash_hash;
  }
};
} // namespace std

using TextureReplacementTexture = Common::RGBA8Image;

class TextureReplacements
{
public:
  enum class ReplacmentType
  {
    VRAMWrite
  };

  TextureReplacements();
  ~TextureReplacements();

  void SetGameID(std::string game_id);

  void Reload();

  const TextureReplacementTexture* GetVRAMWriteReplacement(uint32_t width, uint32_t height, const void* pixels);

  // PSSwanStation: HD texture packs by the GPU texture cache's blocks (see
  // GPU_HW, gpu_hw_texture_blocks.cpp). A block is a texture page's unit of
  // the console's texture cache (psx-spx, "GPU Texture Caching"): 64x64
  // texels of a 4-bit texture, 32x64 of an 8-bit one, 32x32 of a 15-bit one.
  // A pack's picture of one is named block-<4|8|15>-<16 hex digits>.png (or
  // .jpg, .tga, .bmp), anywhere in the game's texture folder; the digits are
  // the block's hash (its texels, then its palette for 4 and 8 bits).
  bool HasTextureBlocks() const { return !m_texture_block_replacements.empty(); }
  bool HasTextureBlock(uint32_t bits, uint64_t hash) const;
  // Reads the pack's picture of a block (not kept here: the renderer keeps
  // it in its replacement atlas).
  bool LoadTextureBlock(uint32_t bits, uint64_t hash, TextureReplacementTexture* out) const;
  // Writes a block the game uses as <folder>/dump/block-<bits>-<hash>.png,
  // once (RGBA, its native size), for pack makers.
  bool DumpingTextureBlocks() const;
  void DumpTextureBlock(uint32_t bits, uint64_t hash, const uint32_t* rgba, uint32_t width, uint32_t height);
  // Changes whenever the replacements are looked for again (a new game, a setting).
  uint32_t GetGeneration() const { return m_generation; }

  void Shutdown();

private:
  using VRAMWriteReplacementMap = std::unordered_map<TextureReplacementHash, std::string>;
  using TextureCache = std::unordered_map<std::string, TextureReplacementTexture>;

  static bool ParseReplacementFilename(const std::string& filename, TextureReplacementHash* replacement_hash,
                                       ReplacmentType* replacement_type);

  std::string GetSourceDirectory() const;

  TextureReplacementHash GetVRAMWriteHash(uint32_t width, uint32_t height, const void* pixels) const;

  void FindTextures(const std::string& dir);

  const TextureReplacementTexture* LoadTexture(const std::string& filename);
  void PreloadTextures();
  void PurgeUnreferencedTexturesFromCache();

  std::string m_game_id;

  TextureCache m_texture_cache;

  VRAMWriteReplacementMap m_vram_write_replacements;
  std::unordered_map<uint64_t, std::string> m_texture_block_replacements; // (bits << 56) ^ hash
  std::unordered_map<uint64_t, bool> m_texture_blocks_dumped;
  uint32_t m_generation = 1;
};

extern TextureReplacements g_texture_replacements;