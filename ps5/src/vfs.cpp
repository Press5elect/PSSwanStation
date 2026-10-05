/*
	SwanStation for PS5 - the file system the frontend gives the core.

	SPDX-License-Identifier: GPL-3.0-or-later

	The core reads files through libretro's VFS. Its hybrid VFS
	(dep/libretro-common/vfs/vfs_hybrid.c) opens plain paths itself and sends
	URI-shaped ones to the frontend: those are smb:// paths, read from the
	share (smb.cpp), streamed or from memory. The share is only read.

	Also here: reading a network game's files into memory before it starts, on
	a thread of its own, so the screen can show how far it is and Circle can
	stop it.
*/
#include "fe.h"

#include <libretro.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>

namespace fe::vfs
{
namespace
{

struct Handle
{
	smb::File *file;
	std::string path;
};

struct Dir
{
	std::vector<smb::Entry> entries;
	size_t at = 0;		// 1-based after the first readdir
};

const char *getPath(retro_vfs_file_handle *stream)
{
	return reinterpret_cast<Handle *>(stream)->path.c_str();
}

retro_vfs_file_handle *open(const char *path, unsigned mode, unsigned)
{
	if (path == nullptr || (mode & RETRO_VFS_FILE_ACCESS_WRITE) != 0 || !smb::isNetworkPath(path))
		return nullptr;
	smb::File *file = smb::open(path);
	if (file == nullptr)
		return nullptr;
	return reinterpret_cast<retro_vfs_file_handle *>(new Handle{ file, path });
}

int close(retro_vfs_file_handle *stream)
{
	Handle *handle = reinterpret_cast<Handle *>(stream);
	if (handle == nullptr)
		return -1;
	delete handle->file;
	delete handle;
	return 0;
}

int64_t size(retro_vfs_file_handle *stream)
{
	return reinterpret_cast<Handle *>(stream)->file->size();
}

int64_t tell(retro_vfs_file_handle *stream)
{
	return reinterpret_cast<Handle *>(stream)->file->tell();
}

int64_t seek(retro_vfs_file_handle *stream, int64_t offset, int position)
{
	smb::File *file = reinterpret_cast<Handle *>(stream)->file;
	const int whence = position == RETRO_VFS_SEEK_POSITION_START ? SEEK_SET
			: position == RETRO_VFS_SEEK_POSITION_CURRENT ? SEEK_CUR : SEEK_END;
	// 0, as fseek gives: the core's readers test for that, and libretro-common's
	// own files (fseeko underneath) answer the same way.
	return file->seek(offset, whence) != 0 ? -1 : 0;
}

int64_t read(retro_vfs_file_handle *stream, void *out, uint64_t bytes)
{
	smb::File *file = reinterpret_cast<Handle *>(stream)->file;
	const size_t n = file->read(out, (size_t)bytes);
	if (n == 0 && bytes != 0 && file->failed())
		return -1;
	return (int64_t)n;
}

int64_t write(retro_vfs_file_handle *, const void *, uint64_t)
{
	return -1;
}

int flush(retro_vfs_file_handle *)
{
	return 0;
}

int remove(const char *)
{
	return -1;
}

int rename(const char *, const char *)
{
	return -1;
}

int64_t truncate(retro_vfs_file_handle *, int64_t)
{
	return -1;
}

int stat(const char *path, int32_t *bytes)
{
	if (path == nullptr || !smb::isNetworkPath(path))
		return 0;
	smb::Entry entry;
	if (smb::stat(path, entry) != 1)
		return 0;
	if (bytes != nullptr)
		*bytes = entry.size > 0x7fffffffu ? 0x7fffffff : (int32_t)entry.size;
	return RETRO_VFS_STAT_IS_VALID | (entry.directory ? RETRO_VFS_STAT_IS_DIRECTORY : 0);
}

int makeDir(const char *)
{
	return -1;
}

retro_vfs_dir_handle *openDir(const char *path, bool)
{
	if (path == nullptr || !smb::isNetworkPath(path))
		return nullptr;
	Dir *dir = new Dir();
	dir->entries = smb::list(path);
	return reinterpret_cast<retro_vfs_dir_handle *>(dir);
}

bool readDir(retro_vfs_dir_handle *stream)
{
	Dir *dir = reinterpret_cast<Dir *>(stream);
	if (dir->at >= dir->entries.size())
		return false;
	dir->at++;
	return true;
}

const char *direntName(retro_vfs_dir_handle *stream)
{
	Dir *dir = reinterpret_cast<Dir *>(stream);
	return dir->at >= 1 && dir->at <= dir->entries.size() ? dir->entries[dir->at - 1].name.c_str() : nullptr;
}

bool direntIsDir(retro_vfs_dir_handle *stream)
{
	Dir *dir = reinterpret_cast<Dir *>(stream);
	return dir->at >= 1 && dir->at <= dir->entries.size() && dir->entries[dir->at - 1].directory;
}

int closeDir(retro_vfs_dir_handle *stream)
{
	delete reinterpret_cast<Dir *>(stream);
	return 0;
}

retro_vfs_interface table = {
	getPath, open, close, size, tell, seek, read, write, flush, remove, rename,
	truncate,
	stat, makeDir, openDir, readDir, direntName, direntIsDir, closeDir,
};

} // namespace

void *interface()
{
	return &table;
}

}

// ------------------------------------------- a network game, into memory first

namespace fe::smb
{
namespace
{
std::atomic<int> precache{PrecacheIdle};
std::thread precacheThread;

std::string folderOf(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

std::string readText(const std::string& path)
{
	std::string text;
	if (isNetworkPath(path))
	{
		File *file = open(path);
		if (file == nullptr)
			return text;
		const int64_t bytes = file->size();
		if (bytes > 0 && bytes < (1 << 20))
		{
			text.resize((size_t)bytes);
			text.resize(file->read(text.data(), (size_t)bytes));
		}
		delete file;
	}
	else
	{
		std::vector<uint8_t> data;
		if (readFile(path, data))
			text.assign(data.begin(), data.end());
	}
	return text;
}

// The files a game file stands for: itself, a cue sheet's tracks, a
// playlist's first disc.
void collect(const std::string& path, std::vector<std::string>& files, int depth)
{
	const std::string ext = extension(path);
	if (ext == ".m3u" && depth < 2)
	{
		std::istringstream lines(readText(path));
		std::string line;
		while (std::getline(lines, line))
		{
			line = trim(line);
			if (line.empty() || line[0] == '#')
				continue;
			const std::string entry = line.find("://") != std::string::npos || line[0] == '/' ? line
					: folderOf(path) + line;
			collect(entry, files, depth + 1);
			break;		// the first disc; the others when they are inserted
		}
		return;
	}
	if (!isNetworkPath(path))
		return;
	files.push_back(path);
	if (ext == ".cue")
	{
		std::istringstream lines(readText(path));
		std::string line;
		while (std::getline(lines, line))
		{
			const size_t file = line.find("FILE");
			const size_t first = line.find('"');
			const size_t last = line.rfind('"');
			if (file == std::string::npos || first == std::string::npos || last <= first)
				continue;
			std::string name = line.substr(first + 1, last - first - 1);
			for (char& c : name)
				if (c == '\\')
					c = '/';
			const std::string track = folderOf(path) + baseName(name);
			if (std::find(files.begin(), files.end(), track) == files.end())
				files.push_back(track);
		}
	}
}
}

void startPrecache(const std::string& gamePath)
{
	if (precacheThread.joinable())
		precacheThread.join();
	precache = PrecacheRunning;
	clearError();
	precacheThread = std::thread([gamePath] {
		beginLoad();
		std::vector<std::string> files;
		collect(gamePath, files, 0);
		bool ok = true;
		for (const std::string& path : files)
		{
			File *file = open(path);
			if (file == nullptr)
			{
				ok = false;
				break;
			}
			delete file;
		}
		const bool wasCancelled = !ok && lastError().empty();
		endLoad();
		if (!ok)
			releaseImages();
		diag::mark("share: %d file(s) for %s: %s", (int)files.size(), baseName(gamePath).c_str(),
				ok ? "ready" : wasCancelled ? "cancelled" : "failed");
		precache = ok ? PrecacheDone : PrecacheFailed;
	});
}

int precacheState()
{
	return precache;
}

void finishPrecache()
{
	if (precacheThread.joinable())
		precacheThread.join();
	precache = PrecacheIdle;
}

}
