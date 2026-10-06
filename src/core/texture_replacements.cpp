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
// PS5: one CPU, compiled for (-march=znver2): no run-time dispatch.
#if (defined(CPU_X86) || defined(CPU_X64)) && !defined(__PROSPERO__)
#include "xxh_x86dispatch.h"
#endif
#include <cinttypes>
#include <file/file_path.h>
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
  // The PS5 title keeps texture packs with the user's other files
  // (<root>textures/<serial>/, ps5/src/host.cpp names the folder).
  if (const char* textures_directory = getenv("SWANSTATION_TEXTURES_DIR"))
    return StringUtil::StdStringFromFormat("%s" FS_OSPATH_SEPARATOR_STR "%s", textures_directory, m_game_id.c_str());
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

void TextureReplacements::Reload()
{
  m_vram_write_replacements.clear();

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
    TextureReplacementHash hash;
    ReplacmentType type;
    if (!ParseReplacementFilename(fd.FileName, &hash, &type))
      continue;

    switch (type)
    {
      case ReplacmentType::VRAMWrite:
      {
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

  Log_InfoPrintf("Found %zu replacement VRAM writes for '%s'", m_vram_write_replacements.size(), m_game_id.c_str());
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
