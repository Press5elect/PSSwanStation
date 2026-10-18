#include "texture_replacements.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/platform.h"
#include "common/string_util.h"
#include "common/timer.h"
#include "core/host_interface.h"
#include "host_interface.h"
#include "settings.h"
#include "xxhash.h"
#include <cstdlib>
#ifdef SWANSTATION_STANDALONE
#include <sys/stat.h>
#endif
// PS5: one CPU, compiled for (-march=znver2): no run-time dispatch.
#if (defined(CPU_X86) || defined(CPU_X64)) && !defined(__PROSPERO__)
#include "xxh_x86dispatch.h"
#endif
#include <algorithm>
#include <cinttypes>
#include <file/file_path.h>
#include <cstdio>
#include <cstring>
#include <vector>
Log_SetChannel(TextureReplacements);

TextureReplacements g_texture_replacements;

static constexpr uint32_t VRAMRGBA5551ToRGBA8888(uint16_t color)
{
  uint8_t r = static_cast<uint8_t>(color & 31);
  uint8_t g = static_cast<uint8_t>((color >> 5) & 31);
  uint8_t b = static_cast<uint8_t>((color >> 10) & 31);
  uint8_t a = static_cast<uint8_t>((color >> 15) & 1);

  // 00012345 -> 1234545
  b = (b << 3) | (b & 0b111);
  g = (g << 3) | (g & 0b111);
  r = (r << 3) | (r & 0b111);
  a = a ? 255 : 0;

  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

std::string TextureReplacementHash::ToString() const
{
  return StringUtil::StdStringFromFormat("%" PRIx64 "%" PRIx64, high, low);
}

bool TextureReplacementHash::ParseString(const std::string_view& sv)
{
  if (sv.length() != 32)
    return false;

  std::optional<uint64_t> high_value = StringUtil::FromChars<uint64_t>(sv.substr(0, 16), 16);
  std::optional<uint64_t> low_value = StringUtil::FromChars<uint64_t>(sv.substr(16), 16);
  if (!high_value.has_value() || !low_value.has_value())
    return false;

  low = low_value.value();
  high = high_value.value();
  return true;
}

TextureReplacements::TextureReplacements() = default;

TextureReplacements::~TextureReplacements() = default;

void TextureReplacements::SetGameID(std::string game_id)
{
#ifdef SWANSTATION_STANDALONE
  // The renderer tests (ps5/tests/gpu) run a program as the BIOS, with no
  // disc to name the texture folder.
  if (game_id.empty())
  {
    if (const char* test_id = std::getenv("SWANSTATION_TEXTURE_ID"))
      game_id = test_id;
  }
#endif
  if (m_game_id == game_id)
    return;

  m_game_id = game_id;
  Reload();
}

const TextureReplacementTexture* TextureReplacements::GetVRAMWriteReplacement(uint32_t width, uint32_t height, const void* pixels)
{
  const TextureReplacementHash hash = GetVRAMWriteHash(width, height, pixels);

  const auto it = m_vram_write_replacements.find(hash);
  if (it == m_vram_write_replacements.end())
    return nullptr;

  return LoadTexture(it->second);
}

void TextureReplacements::Shutdown()
{
  m_texture_cache.clear();
  m_vram_write_replacements.clear();
  m_game_id.clear();
}

std::string TextureReplacements::GetSourceDirectory() const
{
#ifdef SWANSTATION_STANDALONE
  // The PS5 title keeps texture packs with the user's other files, or on a
  // USB drive (<folder>/<serial>/; ps5/src/host.cpp names the folders, with
  // ';' between them). The first that has this game's folder is the one.
  if (const char* textures_directories = getenv("SWANSTATION_TEXTURES_DIR"))
  {
    const std::string all(textures_directories);
    std::string first;
    size_t start = 0;
    while (start <= all.size())
    {
      size_t end = all.find(';', start);
      if (end == std::string::npos)
        end = all.size();
      if (end > start)
      {
        const std::string candidate = all.substr(start, end - start) + FS_OSPATH_SEPARATOR_STR + m_game_id;
        if (first.empty())
          first = candidate;
        struct stat st;
        if (stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
          return candidate;
      }
      start = end + 1;
    }
    if (!first.empty())
      return first;
  }
#endif
  // Use the shader cache path as base for the textures folder
  std::string cache_folder = g_host_interface_storage.GetShaderCacheBasePath();
  return g_host_interface->GetUserDirectoryRelativePath("%s" "textures" FS_OSPATH_SEPARATOR_STR "%s", cache_folder.c_str(), m_game_id.c_str());
}

TextureReplacementHash TextureReplacements::GetVRAMWriteHash(uint32_t width, uint32_t height, const void* pixels) const
{
  XXH128_hash_t hash = XXH3_128bits(pixels, width * height * sizeof(uint16_t));
  return {hash.low64, hash.high64};
}

static uint64_t TextureBlockKey(uint32_t bits, uint64_t hash)
{
  return (static_cast<uint64_t>(bits) << 56) ^ hash;
}

bool TextureReplacements::HasTextureBlock(uint32_t bits, uint64_t hash) const
{
  return m_texture_block_replacements.find(TextureBlockKey(bits, hash)) != m_texture_block_replacements.end();
}

bool TextureReplacements::LoadTextureBlock(uint32_t bits, uint64_t hash, TextureReplacementTexture* out) const
{
  const auto it = m_texture_block_replacements.find(TextureBlockKey(bits, hash));
  if (it == m_texture_block_replacements.end())
    return false;
  if (!Common::LoadImageFromFile(out, it->second.c_str()) || out->GetWidth() == 0 || out->GetHeight() == 0)
  {
    Log_ErrorPrintf("Failed to load '%s'", it->second.c_str());
    return false;
  }
  return true;
}

bool TextureReplacements::DumpingTextureBlocks() const
{
  return g_settings.texture_replacements.dump_texture_blocks && !m_game_id.empty();
}

// A PNG of RGBA pixels, its image data in stored (uncompressed) deflate
// blocks: the pictures are small, and every build has this (the libretro
// build's miniz has no deflate).
static std::vector<uint8_t> EncodeStoredPNG(const uint32_t* rgba, uint32_t width, uint32_t height)
{
  static uint32_t crc_table[256];
  if (crc_table[1] == 0)
  {
    for (uint32_t n = 0; n < 256; n++)
    {
      uint32_t c = n;
      for (int k = 0; k < 8; k++)
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      crc_table[n] = c;
    }
  }
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  const auto put32 = [](std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
  };
  const auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    put32(png, static_cast<uint32_t>(data.size()));
    const size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = start; i < png.size(); i++)
      crc = crc_table[(crc ^ png[i]) & 0xFF] ^ (crc >> 8);
    put32(png, crc ^ 0xFFFFFFFFu);
  };

  std::vector<uint8_t> header;
  put32(header, width);
  put32(header, height);
  header.insert(header.end(), {8, 6, 0, 0, 0}); // 8 bits, RGBA, deflate, filter 0, no interlace
  chunk("IHDR", header);

  // The rows, each after its filter byte (none).
  std::vector<uint8_t> raw;
  raw.reserve((width * 4 + 1) * height);
  for (uint32_t y = 0; y < height; y++)
  {
    raw.push_back(0);
    const uint8_t* row = reinterpret_cast<const uint8_t*>(rgba + y * width);
    raw.insert(raw.end(), row, row + width * 4);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw)
  {
    a = (a + c) % 65521u;
    b = (b + a) % 65521u;
  }
  for (size_t pos = 0; pos < raw.size() || pos == 0;)
  {
    const size_t len = std::min<size_t>(raw.size() - pos, 65535);
    const bool last = (pos + len) == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<uint8_t>(len));
    z.push_back(static_cast<uint8_t>(len >> 8));
    z.push_back(static_cast<uint8_t>(~len));
    z.push_back(static_cast<uint8_t>(~len >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
    pos += len;
    if (last)
      break;
  }
  put32(z, (b << 16) | a);
  chunk("IDAT", z);
  chunk("IEND", {});
  return png;
}

void TextureReplacements::DumpTextureBlock(uint32_t bits, uint64_t hash, const uint32_t* rgba, uint32_t width,
                                           uint32_t height)
{
  if (!m_texture_blocks_dumped.emplace(TextureBlockKey(bits, hash), true).second)
    return;
  const std::string dir = GetSourceDirectory() + FS_OSPATH_SEPARATOR_STR "dump";
  if (!path_is_directory(dir.c_str()) && !path_mkdir(dir.c_str()))
    return;
  const std::string path = StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "block-%u-%016" PRIx64 ".png",
                                                           dir.c_str(), bits, hash);
  if (path_is_valid(path.c_str()))
    return;
  const std::vector<uint8_t> png = EncodeStoredPNG(rgba, width, height);
  if (FileSystem::WriteBinaryFile(path.c_str(), png.data(), png.size()))
    Log_InfoPrintf("Wrote out texture block %s", path.c_str());
}

void TextureReplacements::Reload()
{
  m_vram_write_replacements.clear();
  m_texture_block_replacements.clear();
  m_texture_blocks_dumped.clear();
  m_generation++;

  if (g_settings.texture_replacements.AnyReplacementsEnabled())
    FindTextures(GetSourceDirectory());

  if (g_settings.texture_replacements.preload_textures)
    PreloadTextures();

  PurgeUnreferencedTexturesFromCache();
}

void TextureReplacements::PurgeUnreferencedTexturesFromCache()
{
  TextureCache old_map = std::move(m_texture_cache);
  for (const auto& it : m_vram_write_replacements)
  {
    auto it2 = old_map.find(it.second);
    if (it2 != old_map.end())
    {
      m_texture_cache[it.second] = std::move(it2->second);
      old_map.erase(it2);
    }
  }
}

bool TextureReplacements::ParseReplacementFilename(const std::string& filename,
                                                   TextureReplacementHash* replacement_hash,
                                                   ReplacmentType* replacement_type)
{
  const char* extension = std::strrchr(filename.c_str(), '.');
  const char* title = std::strrchr(filename.c_str(), '/');
#ifdef _WIN32
  const char* title2 = std::strrchr(filename.c_str(), '\\');
  if (title2 && (!title || title2 > title))
    title = title2;
#endif

  if (!title || !extension)
    return false;

  title++;

  const char* hashpart;

  if (StringUtil::Strncasecmp(title, "vram-write-", 11) == 0)
  {
    hashpart = title + 11;
    *replacement_type = ReplacmentType::VRAMWrite;
  }
  else
  {
    return false;
  }

  if (!replacement_hash->ParseString(std::string_view(hashpart, static_cast<size_t>(extension - hashpart))))
    return false;

  extension++;

  bool valid_extension = false;
  for (const char* test_extension : {"png", "jpg", "tga", "bmp"})
  {
    if (StringUtil::Strcasecmp(extension, test_extension) == 0)
    {
      valid_extension = true;
      break;
    }
  }

  return valid_extension;
}

void TextureReplacements::FindTextures(const std::string& dir)
{
  FileSystem::FindResultsArray files;
  FileSystem::FindFiles(dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_RECURSIVE, &files);

  for (FILESYSTEM_FIND_DATA& fd : files)
  {
    // block-<bits>-<16 hex digits>.<image>: a texture cache block (GetTextureBlock).
    {
      const char* title = std::strrchr(fd.FileName.c_str(), '/');
      title = title ? title + 1 : fd.FileName.c_str();
      unsigned bits = 0;
      char hex[17] = {};
      if (g_settings.texture_replacements.enable_texture_blocks &&
          StringUtil::Strncasecmp(title, "block-", 6) == 0 && std::sscanf(title + 6, "%u-%16[0-9a-fA-F]", &bits, hex) == 2 &&
          (bits == 4 || bits == 8 || bits == 15) && std::strlen(hex) == 16 &&
          fd.FileName.find(FS_OSPATH_SEPARATOR_STR "dump" FS_OSPATH_SEPARATOR_STR) == std::string::npos)
      {
        const uint64_t block_hash = std::strtoull(hex, nullptr, 16);
        m_texture_block_replacements.emplace(TextureBlockKey(bits, block_hash), fd.FileName);
        continue;
      }
    }

    TextureReplacementHash hash;
    ReplacmentType type;
    if (!ParseReplacementFilename(fd.FileName, &hash, &type))
      continue;

    switch (type)
    {
      case ReplacmentType::VRAMWrite:
      {
        if (!g_settings.texture_replacements.enable_vram_write_replacements)
          continue;
        auto it = m_vram_write_replacements.find(hash);
        if (it != m_vram_write_replacements.end())
        {
          Log_WarningPrintf("Duplicate VRAM write replacement: '%s' and '%s'", it->second.c_str(), fd.FileName.c_str());
          continue;
        }

        m_vram_write_replacements.emplace(hash, std::move(fd.FileName));
      }
      break;
    }
  }

  Log_InfoPrintf("Found %zu replacement VRAM writes and %zu texture blocks for '%s'", m_vram_write_replacements.size(),
                 m_texture_block_replacements.size(), m_game_id.c_str());
}

const TextureReplacementTexture* TextureReplacements::LoadTexture(const std::string& filename)
{
  auto it = m_texture_cache.find(filename);
  if (it != m_texture_cache.end())
    return &it->second;

  Common::RGBA8Image image;
  if (!Common::LoadImageFromFile(&image, filename.c_str()))
  {
    Log_ErrorPrintf("Failed to load '%s'", filename.c_str());
    return nullptr;
  }

  Log_InfoPrintf("Loaded '%s': %ux%u", filename.c_str(), image.GetWidth(), image.GetHeight());
  it = m_texture_cache.emplace(filename, std::move(image)).first;
  return &it->second;
}

void TextureReplacements::PreloadTextures()
{
  const Common::Timer::Value update_interval = Common::Timer::ConvertSecondsToValue(1.0);
  Common::Timer::Value last_update_time = Common::Timer::GetValue();
  uint32_t num_textures_loaded = 0;
  const uint32_t total_textures = static_cast<uint32_t>(m_vram_write_replacements.size());

#define UPDATE_PROGRESS()                                                                                              \
  if ((Common::Timer::GetValue() - last_update_time) >= update_interval)                                              \
  {                                                                                                                    \
    g_host_interface->DisplayLoadingScreen("Preloading replacement textures...", 0, static_cast<int>(total_textures),  \
                                           static_cast<int>(num_textures_loaded));                                     \
    last_update_time = Common::Timer::GetValue();                                                                     \
  }

  for (const auto& it : m_vram_write_replacements)
  {
    UPDATE_PROGRESS();

    LoadTexture(it.second);
    num_textures_loaded++;
  }

#undef UPDATE_PROGRESS
}
