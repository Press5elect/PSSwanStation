/*
	PSSwanStation - videos played by the console's own player, AvPlayer
	(libSceAvPlayer), for ES-DE themes.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	AvPlayer opens an MP4 file, decodes its H.264 pictures on the console's
	video hardware and its AAC sound with Sony's decoders, and keeps them to
	its own clock: the title asks for the picture that is due now
	(sceAvPlayerGetVideoData, NV12) and the sound that is due
	(sceAvPlayerGetAudioData, 16-bit samples). Nothing of a decoder is part of
	the title.

	The library is loaded at the first video (sceKernelLoadStartModule, its
	functions by name with sceKernelDlsym, as usbpad_ps5.cpp loads
	libSceUsbd). The structures and calls are those of the PS4 SDK's
	sceAvPlayer, which the console keeps; how they behave on the PS5 under
	this title has not been measured yet, so every step says in the boot log
	what it got ("video:" lines), and a start that never came back is not
	tried again (data/avplayer-marker.txt, as libSceUsbd has).

	The player's pictures go in memory it asks the title for
	(allocateTexture): direct memory the video hardware can write, mapped for
	the processor too, as RADV maps its own (type 12). Where the colour plane
	starts is not said by the old interface: it is worked out from the size
	of the memory the player asked for (a picture's rows and their pitch).
*/
#include "../fe.h"
#include "../video_backend.h"

#include <ps5platform/kernel.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>
#include <vector>

extern "C" {
int sceKernelLoadStartModule(const char *path, size_t args, const void *argp, uint32_t flags, void *opt, int *res);
int sceKernelDlsym(int handle, const char *symbol, void **addrp);
}

namespace fe::video::backend
{
namespace
{

// ---- sceAvPlayer, as the PS4 SDK declares it

typedef void *(*AllocateFn)(void *object, uint32_t alignment, uint32_t size);
typedef void (*DeallocateFn)(void *object, void *memory);
typedef int (*OpenFileFn)(void *object, const char *path);
typedef int (*CloseFileFn)(void *object);
typedef int (*ReadOffsetFn)(void *object, uint8_t *buffer, uint64_t position, uint32_t length);
typedef uint64_t (*SizeFileFn)(void *object);
typedef void (*EventFn)(void *object, int32_t event, int32_t source, void *data);

struct MemAllocator
{
	void *object;
	AllocateFn allocate;
	DeallocateFn deallocate;
	AllocateFn allocateTexture;
	DeallocateFn deallocateTexture;
};

struct FileReplacement
{
	void *object;
	OpenFileFn open;
	CloseFileFn close;
	ReadOffsetFn readOffset;
	SizeFileFn size;
};

struct EventReplacement
{
	void *object;
	EventFn callback;
};

struct InitData
{
	MemAllocator memory;
	FileReplacement file;
	EventReplacement events;
	int32_t debugLevel;
	uint32_t basePriority;
	int32_t videoBuffers;
	bool autoStart;
	uint8_t reserved[3];
	const char *language;
	uint8_t spare[64];		// room, should the console's be longer
};

struct AudioDetails
{
	uint16_t channels;
	uint8_t reserved[2];
	uint32_t rate;
	uint32_t size;
	uint8_t language[4];
};

struct VideoDetails
{
	uint32_t width, height;
	float aspect;
	uint8_t language[4];
};

struct FrameInfo
{
	uint8_t *data;
	uint8_t reserved[4];
	uint64_t timeStamp;
	union
	{
		uint8_t raw[16];
		AudioDetails audio;
		VideoDetails video;
	} details;
	uint8_t spare[64];
};

typedef void *Handle;
struct Api
{
	Handle (*Init)(InitData *) = nullptr;
	int32_t (*AddSource)(Handle, const char *) = nullptr;
	int32_t (*SetLooping)(Handle, bool) = nullptr;
	int32_t (*Start)(Handle) = nullptr;
	int32_t (*Stop)(Handle) = nullptr;
	int32_t (*Close)(Handle) = nullptr;
	bool (*IsActive)(Handle) = nullptr;
	bool (*GetVideoData)(Handle, FrameInfo *) = nullptr;
	bool (*GetAudioData)(Handle, FrameInfo *) = nullptr;
};
Api api;

// ---- memory

struct Block
{
	uint8_t *address;
	size_t length;
	int64_t physical;	// -1: from the heap
};
std::mutex blocksMutex;
std::vector<Block> blocks;

void *allocateHeap(void *, uint32_t alignment, uint32_t size)
{
	void *memory = nullptr;
	if (posix_memalign(&memory, std::max<size_t>(alignment, sizeof(void *)), std::max<uint32_t>(size, 1)) != 0)
		return nullptr;
	return memory;
}

void freeHeap(void *, void *memory)
{
	free(memory);
}

constexpr int DirectType = 12;
constexpr int Protection = PS5_KERNEL_PROT_CPU_READ | PS5_KERNEL_PROT_CPU_WRITE | PS5_KERNEL_PROT_GPU_READ
		| PS5_KERNEL_PROT_GPU_WRITE;

void *allocateTexture(void *, uint32_t alignment, uint32_t size)
{
	const size_t align = std::max<size_t>(alignment, PS5_KERNEL_DIRECT_ALIGNMENT);
	const size_t length = (size + PS5_KERNEL_DIRECT_ALIGNMENT - 1) & ~(PS5_KERNEL_DIRECT_ALIGNMENT - 1);
	int64_t physical = -1;
	if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), length, align, DirectType, &physical) != 0)
	{
		diag::mark("video: the player's picture memory (%u bytes) could not be had", size);
		return nullptr;
	}
	void *address = nullptr;
	if (sceKernelMapDirectMemory(&address, length, Protection, 0, physical, align) != 0)
	{
		sceKernelReleaseDirectMemory(physical, length);
		diag::mark("video: the player's picture memory (%u bytes) could not be mapped", size);
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(blocksMutex);
	blocks.push_back({ (uint8_t *)address, (size_t)size, physical });
	return address;
}

void freeTexture(void *, void *memory)
{
	std::lock_guard<std::mutex> lock(blocksMutex);
	for (size_t i = 0; i < blocks.size(); i++)
		if (blocks[i].address == memory)
		{
			const size_t length = (blocks[i].length + PS5_KERNEL_DIRECT_ALIGNMENT - 1) & ~(PS5_KERNEL_DIRECT_ALIGNMENT - 1);
			sceKernelMunmap(memory, length);
			sceKernelReleaseDirectMemory(blocks[i].physical, length);
			blocks.erase(blocks.begin() + (long)i);
			return;
		}
}

// The bytes from `p` to the end of the picture memory it is in (0: not in any).
size_t bytesFrom(const uint8_t *p)
{
	std::lock_guard<std::mutex> lock(blocksMutex);
	for (const Block& b : blocks)
		if (p >= b.address && p < b.address + b.length)
			return (size_t)(b.address + b.length - p);
	return 0;
}

// ---- files, when the player is to read through the title

// A local file, or one on a network share (SMB, NFS, FTP) through the
// title's own readers: the player cannot reach a share by itself.
struct FileState
{
	std::mutex lock;
	FILE *file = nullptr;
	smb::File *remote = nullptr;
	uint64_t size = 0;
};
FileState fileState;

int openFile(void *, const char *path)
{
	std::lock_guard<std::mutex> guard(fileState.lock);
	if (smb::isNetworkPath(path))
	{
		fileState.remote = smb::open(path);
		if (fileState.remote == nullptr || fileState.remote->failed())
		{
			delete fileState.remote;
			fileState.remote = nullptr;
			return -1;
		}
		fileState.size = (uint64_t)std::max<int64_t>(fileState.remote->size(), 0);
		return 0;
	}
	fileState.file = fopen(path, "rb");
	if (fileState.file == nullptr)
		return -1;
	struct stat st{};
	fileState.size = stat(path, &st) == 0 ? (uint64_t)st.st_size : 0;
	return 0;
}

int closeFile(void *)
{
	std::lock_guard<std::mutex> guard(fileState.lock);
	if (fileState.file != nullptr)
		fclose(fileState.file);
	delete fileState.remote;
	fileState.file = nullptr;
	fileState.remote = nullptr;
	return 0;
}

int readFileAt(void *, uint8_t *buffer, uint64_t position, uint32_t length)
{
	std::lock_guard<std::mutex> guard(fileState.lock);
	if (fileState.remote != nullptr)
	{
		if (fileState.remote->seek((int64_t)position, SEEK_SET) < 0)
			return -1;
		return (int)fileState.remote->read(buffer, length);
	}
	if (fileState.file == nullptr || fseek(fileState.file, (long)position, SEEK_SET) != 0)
		return -1;
	return (int)fread(buffer, 1, length, fileState.file);
}

uint64_t fileSize(void *)
{
	return fileState.size;
}

// ---- events

std::atomic<int> lastEvent{0};
std::atomic<int> eventsLogged{0};

void onEvent(void *, int32_t event, int32_t source, void *)
{
	lastEvent = event;
	if (eventsLogged++ < 24)
		diag::mark("video: the player says %#x (source %d)", (unsigned)event, source);
}

// ---- the player

struct Player
{
	Handle handle = nullptr;
	bool loop = false;
	bool throughTitle = false;		// reads the file through the title's callbacks
	std::string marker;
	bool proven = false;			// a picture came: the marker is cleared
	// How the pictures are laid out, once worked out.
	int layoutWidth = 0, layoutHeight = 0, pitch = 0, rows = 0;
	unsigned audioPackets = 0;
} player;

bool brokenNow = false;

void writeMarker(const char *text)
{
	if (player.marker.empty())
		return;
	if (FILE *f = fopen(player.marker.c_str(), "wb"))
	{
		fputs(text, f);
		fclose(f);
	}
}

template <typename F> void symbol(int module, const char *name, F& fn, bool& ok)
{
	void *address = nullptr;
	if (sceKernelDlsym(module, name, &address) == 0 && address != nullptr)
		fn = reinterpret_cast<F>(address);
	else
	{
		diag::mark("video: libSceAvPlayer: %s not found", name);
		ok = false;
	}
}

Handle makePlayer(bool throughTitle)
{
	InitData init;
	memset(&init, 0, sizeof(init));
	init.memory = { nullptr, allocateHeap, freeHeap, allocateTexture, freeTexture };
	if (throughTitle)
		init.file = { nullptr, openFile, closeFile, readFileAt, fileSize };
	init.events = { nullptr, onEvent };
	init.debugLevel = 0;
	init.basePriority = 700;
	init.videoBuffers = 2;
	init.autoStart = true;
	init.language = "en";
	return api.Init(&init);
}

// Where the colour plane is: the picture memory holds whole pictures of
// pitch x rows brightness bytes and half as many colour bytes; the pitch
// and rows that fill it exactly are the ones the player used.
void workOutLayout(const uint8_t *data, int width, int height)
{
	if (player.layoutWidth == width && player.layoutHeight == height)
		return;
	player.layoutWidth = width;
	player.layoutHeight = height;
	player.pitch = width;
	player.rows = height;
	const size_t available = bytesFrom(data);
	const auto up = [](int v, int a) { return (v + a - 1) / a * a; };
	bool found = false;
	for (int pitchAlign : { 1, 16, 32, 64, 128, 256 })
	{
		for (int rowAlign : { 1, 16, 32, 64 })
		{
			const size_t frame = (size_t)up(width, pitchAlign) * up(height, rowAlign) * 3 / 2;
			for (int count = 1; count <= 8 && !found; count++)
				if (frame * count == available || (count == 1 && frame <= available && available - frame < 65536
						&& pitchAlign == 1 && rowAlign == 1))
				{
					player.pitch = up(width, pitchAlign);
					player.rows = up(height, rowAlign);
					found = true;
				}
			if (found)
				break;
		}
		if (found)
			break;
	}
	diag::mark("video: pictures %d x %d, rows of %d bytes, colour after %d rows (%zu bytes of picture memory%s)",
			width, height, player.pitch, player.rows, available, found ? "" : ", guessed");
}

// The memory the last picture taken lives in stays the player's: its planes
// are read straight from it.
} // namespace

const char *name()
{
	return player.throughTitle ? "the console's video player (AvPlayer, files read by the title)"
			: "the console's video player (AvPlayer)";
}

bool broken()
{
	return brokenNow;
}

bool load(std::string& why)
{
	player.marker = rootDir + "data/avplayer-marker.txt";
	if (FILE *f = fopen(player.marker.c_str(), "rb"))
	{
		char line[16] = {};
		const size_t n = fread(line, 1, sizeof(line) - 1, f);
		fclose(f);
		line[n] = 0;
		if (strncmp(line, "started", 7) == 0)
		{
			why = "the last video never came back from the console's player (delete data/avplayer-marker.txt to try again)";
			return false;
		}
	}
	int module = -1;
	for (const char *dir : { "/system/common/lib/", "/system/priv/lib/", "/system_ex/common_ex/lib/" })
	{
		const std::string path = std::string(dir) + "libSceAvPlayer.sprx";
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
			continue;
		int result = 0;
		writeMarker("started load");
		module = sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, &result);
		diag::mark("video: libSceAvPlayer: %s: load %#x (start %d)", path.c_str(), (unsigned)module, result);
		if (module >= 0)
			break;
	}
	if (module < 0)
	{
		writeMarker("not loadable");
		why = "the console's video player (libSceAvPlayer) could not be loaded";
		return false;
	}
	bool ok = true;
	symbol(module, "sceAvPlayerInit", api.Init, ok);
	symbol(module, "sceAvPlayerAddSource", api.AddSource, ok);
	symbol(module, "sceAvPlayerSetLooping", api.SetLooping, ok);
	symbol(module, "sceAvPlayerStart", api.Start, ok);
	symbol(module, "sceAvPlayerStop", api.Stop, ok);
	symbol(module, "sceAvPlayerClose", api.Close, ok);
	symbol(module, "sceAvPlayerIsActive", api.IsActive, ok);
	symbol(module, "sceAvPlayerGetVideoData", api.GetVideoData, ok);
	symbol(module, "sceAvPlayerGetAudioData", api.GetAudioData, ok);
	writeMarker(ok ? "loaded" : "incomplete");
	if (!ok)
		why = "the console's video player lacks a function it needs";
	return ok;
}

bool open(const std::string& path, bool loop, std::string& why)
{
	close();
	// Until a first picture shows it works, a crash in the player is remembered.
	if (!player.proven)
		writeMarker("started");
	// A file on a share is read through the title; a local one as the player
	// last managed, then the other way.
	const bool remote = smb::isNetworkPath(path);
	std::vector<bool> ways = { player.throughTitle, !player.throughTitle };
	if (remote)
		ways = { true };
	for (const bool throughTitle : ways)
	{
		player.handle = makePlayer(throughTitle);
		if (player.handle == nullptr)
		{
			why = "the player could not be made";
			brokenNow = true;
			writeMarker("init failed");
			return false;
		}
		const int added = api.AddSource(player.handle, path.c_str());
		if (added >= 0)
		{
			if (!remote)
				player.throughTitle = throughTitle;
			break;
		}
		diag::mark("video: AddSource %s %#x", throughTitle ? "(files read by the title)" : "(the player's own files)",
				(unsigned)added);
		api.Close(player.handle);
		player.handle = nullptr;
		closeFile(nullptr);
	}
	if (player.handle == nullptr)
	{
		why = "the player would not open the file";
		if (!player.proven)
			writeMarker("loaded");
		return false;
	}
	player.loop = loop;
	if (api.SetLooping != nullptr)
		api.SetLooping(player.handle, loop);
	player.layoutWidth = player.layoutHeight = 0;
	player.audioPackets = 0;
	return true;
}

void close()
{
	if (player.handle != nullptr)
	{
		api.Stop(player.handle);
		api.Close(player.handle);
		player.handle = nullptr;
	}
	closeFile(nullptr);
}

bool take(Picture& out)
{
	if (player.handle == nullptr)
		return false;
	FrameInfo info;
	memset(&info, 0, sizeof(info));
	bool got = false;
	FrameInfo newest;
	// The newest picture due: older ones still waiting are passed over.
	for (int i = 0; i < 4 && api.GetVideoData(player.handle, &info); i++)
	{
		newest = info;
		got = true;
	}
	if (!got || newest.data == nullptr)
		return false;
	const int width = (int)newest.details.video.width, height = (int)newest.details.video.height;
	if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
		return false;
	workOutLayout(newest.data, width, height);
	out.width = width & ~1;
	out.height = height & ~1;
	out.y = newest.data;
	out.pitch = player.pitch;
	out.uv = newest.data + (size_t)player.pitch * player.rows;
	out.uvPitch = player.pitch;
	if (!player.proven)
	{
		player.proven = true;
		writeMarker("works");
	}
	return true;
}

void drainAudio(const std::function<void(const int16_t *, size_t, int, int)>& sink)
{
	if (player.handle == nullptr)
		return;
	FrameInfo info;
	for (int i = 0; i < 16; i++)
	{
		memset(&info, 0, sizeof(info));
		if (!api.GetAudioData(player.handle, &info) || info.data == nullptr)
			break;
		const int channels = info.details.audio.channels;
		const int rate = (int)info.details.audio.rate;
		const size_t bytes = info.details.audio.size;
		if (player.audioPackets++ == 0)
			diag::mark("video: sound %d channel(s) at %d Hz, %zu bytes a packet", channels, rate, bytes);
		if (sink && channels > 0 && channels <= 8 && rate > 0 && bytes > 0)
			sink((const int16_t *)info.data, bytes / (2 * (size_t)channels), channels, rate);
	}
}

bool active()
{
	return player.handle != nullptr && api.IsActive(player.handle);
}

} // namespace fe::video::backend
