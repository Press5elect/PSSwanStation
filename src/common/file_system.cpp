#include <config.h>

#include "file_system.h"
#include "byte_stream.h"
#include "string_util.h"
#include <cstring>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/param.h>
#else
// alloca() on Microsoft Windows. Be careful because the header differs wildly
// between operating systems.
#  ifdef HAVE_MALLOC_H
#    include <malloc.h>
#  endif
#endif

#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif

#if defined(_WIN32)
#include <shlobj.h>
#include "windows_headers.h"
#else
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <compat/strl.h>
#include <encodings/utf.h>
#include <file/file_path.h>
#include <streams/file_stream.h>

extern "C" int rfeof(RFILE* stream)
{
   return filestream_eof(stream);
}

extern "C" char *rfgets(char *buffer, int maxCount, RFILE* stream)
{
   if (!stream)
      return NULL;

   return filestream_gets(stream, buffer, maxCount);
}

extern "C" int rfgetc(RFILE* stream)
{
   if (!stream)
      return EOF;

   return filestream_getc(stream);
}

extern "C" RFILE* rfopen(const char *path, const char *mode)
{
   RFILE          *output  = NULL;
   unsigned int retro_mode = RETRO_VFS_FILE_ACCESS_READ;
   bool position_to_end    = false;

   if (strstr(mode, "r"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_READ;
      if (strstr(mode, "+"))
      {
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE |
            RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      }
   }
   else if (strstr(mode, "w"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_WRITE;
      if (strstr(mode, "+"))
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE;
   }
   else if (strstr(mode, "a"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_WRITE |
         RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      position_to_end = true;
      if (strstr(mode, "+"))
      {
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE |
            RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      }
   }

   output = filestream_open(path, retro_mode,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (output && position_to_end)
      filestream_seek(output, 0, RETRO_VFS_SEEK_POSITION_END);

   return output;
}

extern "C" int rfclose(RFILE* stream)
{
   if (!stream)
      return EOF;

   return filestream_close(stream);
}

extern "C" int64_t rftell(RFILE* stream)
{
   if (!stream)
      return -1;

   return filestream_tell(stream);
}

extern "C" int64_t rfread(void* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream)
{
   if (!stream || (elem_size == 0) || (elem_count == 0))
      return 0;

   return (filestream_read(stream, buffer, elem_size * elem_count) / elem_size);
}

extern "C" int64_t rfseek(RFILE* stream, int64_t offset, int origin)
{
   int seek_position = -1;

   if (!stream)
      return -1;

   switch (origin)
   {
      case SEEK_SET:
         seek_position = RETRO_VFS_SEEK_POSITION_START;
         break;
      case SEEK_CUR:
         seek_position = RETRO_VFS_SEEK_POSITION_CURRENT;
         break;
      case SEEK_END:
         seek_position = RETRO_VFS_SEEK_POSITION_END;
         break;
   }

   return filestream_seek(stream, offset, seek_position);
}

extern "C" int64_t rfwrite(void const* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream)
{
   if (!stream || (elem_size == 0) || (elem_count == 0))
      return 0;

   return (filestream_write(stream, buffer, elem_size * elem_count) / elem_size);
}

namespace FileSystem {

bool IsAbsolutePath(const std::string_view& path)
{
#ifdef _WIN32
  return (path.length() >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
          path[1] == ':' && (path[2] == '/' || path[2] == '\\'));
#else
  // A URI ("smb://server/share/file", which a frontend's VFS opens) names its
  // file whole, as a path from the root does: it is not relative to the
  // playlist or cue sheet that mentions it.
  if (path.find("://") != std::string_view::npos)
    return true;
  return (path.length() >= 1 && path[0] == '/');
#endif
}

std::string_view StripExtension(const std::string_view& path)
{
  std::string_view::size_type pos = path.rfind('.');
  if (pos == std::string::npos)
    return path;

  return path.substr(0, pos);
}

std::string ReplaceExtension(const std::string_view& path, const std::string_view& new_extension)
{
  std::string_view::size_type pos = path.rfind('.');
  if (pos == std::string::npos)
    return std::string(path);

  std::string ret(path, 0, pos + 1);
  ret.append(new_extension);
  return ret;
}

static std::string_view::size_type GetLastSeperatorPosition(const std::string_view& filename, bool include_separator)
{
  std::string_view::size_type last_separator = filename.rfind('/');
  if (include_separator && last_separator != std::string_view::npos)
    last_separator++;

#if defined(_WIN32)
  std::string_view::size_type other_last_separator = filename.rfind('\\');
  if (other_last_separator != std::string_view::npos)
  {
    if (include_separator)
      other_last_separator++;
    if (last_separator == std::string_view::npos || other_last_separator > last_separator)
      last_separator = other_last_separator;
  }
#endif

  return last_separator;
}

static std::string_view GetFileNameFromPath(const std::string_view& path)
{
  std::string_view::size_type pos = GetLastSeperatorPosition(path, true);
  if (pos == std::string_view::npos)
    return path;

  return path.substr(pos);
}

std::string GetDisplayNameFromPath(const std::string_view& path)
{
  return std::string(GetFileNameFromPath(path));
}

std::string_view GetFileTitleFromPath(const std::string_view& path)
{
  std::string_view filename(GetFileNameFromPath(path));
  std::string::size_type pos = filename.rfind('.');
  if (pos == std::string_view::npos)
    return filename;

  return filename.substr(0, pos);
}

std::string BuildRelativePath(const std::string_view& filename, const std::string_view& new_filename)
{
  std::string new_string;
  std::string_view::size_type pos = GetLastSeperatorPosition(filename, true);
  if (pos != std::string_view::npos)
    new_string.assign(filename, 0, pos);
  new_string.append(new_filename);
  return new_string;
}

std::unique_ptr<ByteStream> OpenFile(const char* FileName, uint32_t Flags)
{
  // TODO: Handle Android content URIs here.

  // forward to local file wrapper
  return ByteStream_OpenFileStream(FileName, Flags);
}


std::optional<std::vector<uint8_t>> ReadBinaryFile(const char* filename)
{
  RFILE *fp = OpenRFile(filename, "rb");
  if (!fp)
    return std::nullopt;

  rfseek(fp, 0, SEEK_END);
  int64_t size = rftell(fp);
  rfseek(fp, 0, SEEK_SET);
  if (size < 0)
  {
    rfclose(fp);
    return std::nullopt;
  }

  std::vector<uint8_t> res(static_cast<size_t>(size));
  if (size > 0 && rfread(res.data(), 1u, static_cast<size_t>(size), fp) != static_cast<int64_t>(size))
  {
    rfclose(fp);
    return std::nullopt;
  }
  rfclose(fp);
  return res;
}

std::optional<std::vector<uint8_t>> ReadBinaryFile(RFILE* fp)
{
  rfseek(fp, 0, SEEK_END);
  int64_t size = rftell(fp);
  rfseek(fp, 0, SEEK_SET);
  if (size < 0)
    return std::nullopt;

  std::vector<uint8_t> res(static_cast<size_t>(size));
  if (size > 0 && rfread(res.data(), 1u, static_cast<size_t>(size), fp) != static_cast<int64_t>(size))
    return std::nullopt;

  return res;
}

std::optional<std::string> ReadFileToString(RFILE* fp)
{
  rfseek(fp, 0, SEEK_END);
  int64_t size = rftell(fp);
  rfseek(fp, 0, SEEK_SET);
  if (size < 0)
    return std::nullopt;
  std::string res;
  res.resize(static_cast<size_t>(size));
  if (size > 0 && rfread(res.data(), 1u, static_cast<size_t>(size), fp) != static_cast<int64_t>(size))
    return std::nullopt;
  return res;
}

bool WriteBinaryFile(const char* filename, const void* data, size_t data_length)
{
  RFILE *fp = OpenRFile(filename, "wb");
  if (!fp)
    return false;
  if (data_length > 0 && rfwrite(data, 1u, data_length, fp) != static_cast<int64_t>(data_length))
  {
    rfclose(fp);
    return false;
  }
  rfclose(fp);
  return true;
}

#ifdef _WIN32
static uint32_t RecursiveFindFiles(const char* OriginPath, const char* ParentPath, const char* Path, const char* Pattern,
                              uint32_t Flags, FileSystem::FindResultsArray* pResults)
{
  std::string tempStr;
  if (Path)
  {
    if (ParentPath)
      tempStr = StringUtil::StdStringFromFormat("%s\\%s\\%s\\*", OriginPath, ParentPath, Path);
    else
      tempStr = StringUtil::StdStringFromFormat("%s\\%s\\*", OriginPath, Path);
  }
  else
  {
    tempStr = StringUtil::StdStringFromFormat("%s\\*", OriginPath);
  }

  // holder for utf-8 conversion
  WIN32_FIND_DATAW wfd;

  wchar_t *a   = utf8_to_utf16_string_alloc(tempStr.c_str());
  HANDLE hFind = FindFirstFileW(a, &wfd);
  free(a);
  if (hFind == INVALID_HANDLE_VALUE)
    return 0;

  // small speed optimization for '*' case
  bool hasWildCards = false;
  bool wildCardMatchAll = false;
  uint32_t nFiles = 0;
  if (std::strpbrk(Pattern, "*?") != nullptr)
  {
    hasWildCards = true;
    wildCardMatchAll = !(std::strcmp(Pattern, "*"));
  }

  // iterate results
  do
  {
    if (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN && !(Flags & FILESYSTEM_FIND_HIDDEN_FILES))
      continue;

    if (wfd.cFileName[0] == L'.')
    {
      if (wfd.cFileName[1] == L'\0' || (wfd.cFileName[1] == L'.' && wfd.cFileName[2] == L'\0'))
        continue;
    }

    char *utf8_filename = utf16_to_utf8_string_alloc(wfd.cFileName);
    if (!utf8_filename)
      continue;

    FILESYSTEM_FIND_DATA outData;

    if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
      if (Flags & FILESYSTEM_FIND_RECURSIVE)
      {
        // recurse into this directory
        if (ParentPath != nullptr)
        {
          std::string recursiveDir = StringUtil::StdStringFromFormat("%s\\%s", ParentPath, Path);
          nFiles += RecursiveFindFiles(OriginPath, recursiveDir.c_str(), utf8_filename, Pattern, Flags, pResults);
        }
        else
          nFiles += RecursiveFindFiles(OriginPath, Path, utf8_filename, Pattern, Flags, pResults);
      }

      free(utf8_filename);
      continue;
    }
    else
    {
      if (!(Flags & FILESYSTEM_FIND_FILES))
      {
	free(utf8_filename);
        continue;
      }
    }

    // match the filename
    if (hasWildCards)
    {
      if (!wildCardMatchAll && !StringUtil::WildcardMatch(utf8_filename, Pattern))
      {
	free(utf8_filename);
        continue;
      }
    }
    else
    {
      if (std::strcmp(utf8_filename, Pattern) != 0)
      {
	free(utf8_filename);
        continue;
      }
    }

    // add file to list
    // TODO string formatter, clean this mess..
    if (!(Flags & FILESYSTEM_FIND_RELATIVE_PATHS))
    {
      if (ParentPath != nullptr)
        outData.FileName =
          StringUtil::StdStringFromFormat("%s\\%s\\%s\\%s", OriginPath, ParentPath, Path, utf8_filename);
      else if (Path != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s\\%s", OriginPath, Path, utf8_filename);
      else
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s", OriginPath, utf8_filename);
    }
    else
    {
      if (ParentPath != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s\\%s", ParentPath, Path, utf8_filename);
      else if (Path != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s", Path, utf8_filename);
      else
        outData.FileName = std::string(utf8_filename);
    }

    outData.Size = (uint64_t)wfd.nFileSizeHigh << 32 | (uint64_t)wfd.nFileSizeLow;

    nFiles++;
    pResults->push_back(std::move(outData));
    free(utf8_filename);
  } while (FindNextFileW(hFind, &wfd) == TRUE);
  FindClose(hFind);

  return nFiles;
}

#else
static uint32_t RecursiveFindFiles(const char* OriginPath, const char* ParentPath, const char* Path, const char* Pattern,
                              uint32_t Flags, FindResultsArray* pResults)
{
  std::string tempStr;
  if (Path)
  {
    if (ParentPath)
      tempStr = StringUtil::StdStringFromFormat("%s/%s/%s", OriginPath, ParentPath, Path);
    else
      tempStr = StringUtil::StdStringFromFormat("%s/%s", OriginPath, Path);
  }
  else
  {
    tempStr = StringUtil::StdStringFromFormat("%s", OriginPath);
  }

  DIR* pDir = opendir(tempStr.c_str());
  if (pDir == nullptr)
    return 0;

  // small speed optimization for '*' case
  bool hasWildCards = false;
  bool wildCardMatchAll = false;
  uint32_t nFiles = 0;
  if (std::strpbrk(Pattern, "*?"))
  {
    hasWildCards = true;
    wildCardMatchAll = (std::strcmp(Pattern, "*") == 0);
  }

  // iterate results
  PathString full_path;
  struct dirent* pDirEnt;
  while ((pDirEnt = readdir(pDir)) != nullptr)
  {
    //        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN && !(Flags & FILESYSTEM_FIND_HIDDEN_FILES))
    //            continue;
    //
    if (pDirEnt->d_name[0] == '.')
    {
      if (pDirEnt->d_name[1] == '\0' || (pDirEnt->d_name[1] == '.' && pDirEnt->d_name[2] == '\0'))
        continue;

      if (!(Flags & FILESYSTEM_FIND_HIDDEN_FILES))
        continue;
    }

    if (ParentPath != nullptr)
      full_path.Format("%s/%s/%s/%s", OriginPath, ParentPath, Path, pDirEnt->d_name);
    else if (Path != nullptr)
      full_path.Format("%s/%s/%s", OriginPath, Path, pDirEnt->d_name);
    else
      full_path.Format("%s/%s", OriginPath, pDirEnt->d_name);

    FILESYSTEM_FIND_DATA outData;

    int32_t sdir_size = path_get_size(full_path);

    if (sdir_size == -1)
      continue;

    if (path_is_directory(full_path))
    {
      if (Flags & FILESYSTEM_FIND_RECURSIVE)
      {
        // recurse into this directory
        if (ParentPath != nullptr)
        {
          std::string recursiveDir = StringUtil::StdStringFromFormat("%s/%s", ParentPath, Path);
          nFiles += RecursiveFindFiles(OriginPath, recursiveDir.c_str(), pDirEnt->d_name, Pattern, Flags, pResults);
        }
        else
        {
          nFiles += RecursiveFindFiles(OriginPath, Path, pDirEnt->d_name, Pattern, Flags, pResults);
        }
      }

      continue;
    }
    else
    {
      if (!(Flags & FILESYSTEM_FIND_FILES))
        continue;
    }

    outData.Size = static_cast<uint64_t>(sdir_size);

    // match the filename
    if (hasWildCards)
    {
      if (!wildCardMatchAll && !StringUtil::WildcardMatch(pDirEnt->d_name, Pattern))
        continue;
    }
    else
    {
      if (std::strcmp(pDirEnt->d_name, Pattern) != 0)
        continue;
    }

    // add file to list
    // TODO string formatter, clean this mess..
    if (!(Flags & FILESYSTEM_FIND_RELATIVE_PATHS))
    {
      outData.FileName = std::string(full_path.GetCharArray());
    }
    else
    {
      if (ParentPath != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s\\%s", ParentPath, Path, pDirEnt->d_name);
      else if (Path != nullptr)
        outData.FileName = StringUtil::StdStringFromFormat("%s\\%s", Path, pDirEnt->d_name);
      else
        outData.FileName = pDirEnt->d_name;
    }

    nFiles++;
    pResults->push_back(std::move(outData));
  }

  closedir(pDir);
  return nFiles;
}
#endif

bool FindFiles(const char* Path, const char* Pattern, uint32_t Flags, FindResultsArray* pResults)
{
  // has a path
  if (Path[0] == '\0')
    return false;

  // clear result array
  pResults->clear();

  // enter the recursive function
  return (RecursiveFindFiles(Path, nullptr, nullptr, Pattern, Flags, pResults) > 0);
}

RFILE* OpenMappableRFile(const char* filename)
{
   return filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS);
}

const uint8_t* GetMappedView(RFILE* fp, int64_t* size)
{
   if (!fp)
      return nullptr;
   return filestream_get_mapped_ptr(fp, size);
}

RFILE* OpenRFile(const char *filename, const char *mode)
{
   RFILE          *output  = NULL;
   unsigned int retro_mode = RETRO_VFS_FILE_ACCESS_READ;
   bool position_to_end    = false;

   if (strstr(mode, "r"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_READ;
      if (strstr(mode, "+"))
      {
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE |
            RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      }
   }
   else if (strstr(mode, "w"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_WRITE;
      if (strstr(mode, "+"))
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE;
   }
   else if (strstr(mode, "a"))
   {
      retro_mode = RETRO_VFS_FILE_ACCESS_WRITE |
         RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      position_to_end = true;
      if (strstr(mode, "+"))
      {
         retro_mode = RETRO_VFS_FILE_ACCESS_READ_WRITE |
            RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
      }
   }

   output = filestream_open(filename, retro_mode,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (output && position_to_end)
      filestream_seek(output, 0, RETRO_VFS_SEEK_POSITION_END);

   return output;
}

int64_t FSeek64(RFILE* fp, int64_t offset, int whence)
{
   int seek_position = -1;

   if (!fp)
      return -1;

   switch (whence)
   {
      case SEEK_SET:
         seek_position = RETRO_VFS_SEEK_POSITION_START;
         break;
      case SEEK_CUR:
         seek_position = RETRO_VFS_SEEK_POSITION_CURRENT;
         break;
      case SEEK_END:
         seek_position = RETRO_VFS_SEEK_POSITION_END;
         break;
   }

   return filestream_seek(fp, offset, seek_position);
}

int64_t FTell64(RFILE* fp)
{
	return filestream_tell(fp);
}

} // namespace FileSystem
