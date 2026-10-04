/*
	SwanStation for PS5 - the frontend's shared declarations.

	SPDX-License-Identifier: GPL-3.0-or-later

	The frontend is a small libretro host: it owns the display, the pads, the
	sound and the files, and runs SwanStation's core, linked into the title,
	through the libretro interface the core already has. One thread does the
	interface, the emulator and the drawing; the sound output, the library
	scan, cover downloads and network reads have threads of their own.
*/
#pragma once

#include <cstdarg>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fe
{

// The title's folder, with a trailing '/': /app0/ on the console (which is
// /data/homebrew/PPSA99248/ over FTP), the test folder on a PC.
extern std::string rootDir;
// The root as a person reaches it over FTP.
std::string shownRoot();

constexpr const char *AppName = "SwanStation";
constexpr const char *TitleId = "PPSA99248";
extern const int BuildNumber;

// ------------------------------------------------------------------ diag.cpp
namespace diag
{
// Opens <dir>swanstation-boot.log for this run (the two before it are kept as
// .1.log and .2.log) and writes the marks made before it was open.
bool open(const std::string& dir);
// One line of the boot log, written straight to the file.
void mark(const char *format, ...) __attribute__((format(printf, 1, 2)));
// A line of the boot log and, where the platform has them and they are on, a
// pop-up notice.
void notify(const char *format, ...) __attribute__((format(printf, 1, 2)));
void setNotifications(bool show);
// Fatal signals and std::terminate write a report to the boot log.
void installCrashHandler();
const char *lastMark();
}

// ------------------------------------------------------------- platform layer
// (src/ps5/*.cpp on the console, src/host/platform_host.cpp on a PC)
namespace platform
{
enum Button : uint32_t
{
	Cross = 1u << 0,
	Circle = 1u << 1,
	Square = 1u << 2,
	Triangle = 1u << 3,
	L1 = 1u << 4,
	R1 = 1u << 5,
	L2 = 1u << 6,
	R2 = 1u << 7,
	L3 = 1u << 8,
	R3 = 1u << 9,
	Up = 1u << 10,
	Down = 1u << 11,
	Left = 1u << 12,
	Right = 1u << 13,
	Options = 1u << 14,
	// The touch pad's click, by the half of the pad the finger is on.
	TouchLeft = 1u << 15,
	TouchRight = 1u << 16,
};

struct Pad
{
	bool connected = false;
	uint32_t buttons = 0;	// held now
	uint32_t pressed = 0;	// went down since the previous poll
	uint32_t released = 0;	// went up since the previous poll
	float lx = 0, ly = 0;	// -1..1
	float rx = 0, ry = 0;
	float l2 = 0, r2 = 0;	// 0..1
};

constexpr int MaxPads = 4;

// Before anything else: the folders, the sandbox, the log.
void earlyInit();
// After the display is up: dismiss the splash, open the pads.
void lateInit();
// Ends the title. Does not return.
[[noreturn]] void quit();

void padPoll();
const Pad& pad(int index);
int padCount();
// 0..1 for the large and the small motor.
void padRumble(int index, float strong, float weak);

// The sound output: opens it and starts the thread that feeds it from
// audio::render(). False when there is none.
bool audioOpen();
void audioClose();

// One HTTP(S) GET. The status code, or -1 when the request could not be made.
int httpGet(const std::string& url, std::vector<uint8_t>& out, unsigned seconds, int *error = nullptr);
bool httpAvailable();

// USB drives: whether they can be read at all, and the folders found on them.
bool usbAvailable();
std::vector<std::string> usbGameDirs();

// Free memory, for the About page: bytes, or 0 when unknown.
uint64_t freeMemory();
// Whether memory can be made executable (the recompiler needs it).
bool jitAvailable();
}

// ----------------------------------------------------------------- audio.cpp
namespace audio
{
void init();
void shutdown();
// From the core: 44.1 kHz stereo frames.
void push(const int16_t *samples, size_t frames);
// From the output thread: `frames` 48 kHz stereo frames; silence when dry.
void render(int16_t *out, size_t frames);
// Empties the ring (a game ended, a state was loaded).
void clear();
void setPaused(bool paused);
// 0..100.
void setVolume(int percent);
// How full the ring is, 0..1, for the statistics line.
float fill();
unsigned underruns();
}

// --------------------------------------------------------------- options.cpp
namespace options
{
// The frontend's own settings (<root>frontend.cfg).
struct Frontend
{
	int view = 0;				// library: 0 grid, 1 list
	int source = 0;				// the library tab last open: 0 internal, 1 USB, 2 network
	bool covers = true;			// download missing covers
	bool usb = false;			// leave the sandbox at start to read USB drives
	bool ramCache = true;		// read a network game whole into memory before it starts
	bool notifications = false;	// the console's pop-up notices
	int scaling = 0;			// 0 fit (keep aspect), 1 integer, 2 stretch
	bool linearFilter = true;	// how the picture is stretched to the screen
	int volume = 100;
	bool showFps = false;
	bool syncToDisplay = true;	// the sound follows the display's pace
	bool autoSaveOnExit = false;	// save a resume state when a game is closed
	bool autoLoadOnStart = false;	// and start from it
	int controller[2] = {1, 1};	// port 1 and 2: 0 digital, 1 DualShock, 2 analog joystick, 3 none
	float deadZone = 0.10f;
	bool rumble = true;
	bool swapConfirm = false;	// Circle confirms in the menus
	int uiScale = 100;			// percent
	int accent = 0;
};
Frontend& frontend();
void loadFrontend();
void saveFrontend();

// The core's options (libretro core options v2), as the core defined them.
struct Value
{
	std::string value, label;
};
struct Option
{
	std::string key, name, info, category;
	std::vector<Value> values;
	std::string defaultValue;
	bool visible = true;
};
struct Category
{
	std::string key, name, info;
};
// Called by the host when the core hands its definitions over.
void define(const void *optionsV2);
const std::vector<Category>& categories();
std::vector<Option>& all();
Option *find(const std::string& key);

// Values: the global set (<root>data/options.cfg) and, over it, the running
// game's own (<root>data/game-options/<serial>.cfg).
const char *get(const std::string& key);
std::string label(const Option& option, const std::string& value);
void set(const std::string& key, const std::string& value, bool forGame);
void clearGameValue(const std::string& key);
bool hasGameValue(const std::string& key);
void loadGlobal();
void loadGame(const std::string& serial);	// empty: no game
const std::string& gameSerial();
// True once after a value changed (RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE).
bool takeChanged();
// Counts the changes.
unsigned generation();
// Values that hold while something else asks for them (a widescreen patch's
// aspect ratio): over the game's and the global ones, never saved.
void setOverride(const std::string& key, const std::string& value);
void clearOverrides();
bool hasOverride(const std::string& key);
void setVisible(const std::string& key, bool visible);
}

// ------------------------------------------------------------------ host.cpp
namespace host
{
bool init();		// the core: retro_init, its options
void shutdown();

struct GameInfo
{
	std::string path;		// the file, or smb://...
	std::string title;		// for the screen
	std::string serial;		// the disc's ID once it runs (SLUS-00594)
};

bool running();
bool paused();
void setPaused(bool paused);
const GameInfo& game();
// Starts a game; an empty path boots the BIOS. `stateSlot` >= 0 loads that
// state as it starts, -2 the resume state. False when the core refused.
bool start(const std::string& path, int stateSlot = -1);
void stop();
void reset();
// Runs the core for one display refresh (none, one or two emulated frames).
void runFrame();
std::string lastError();

// The picture: an ImGui texture and its size, or 0 when there is none.
void *frameTexture();
void frameSize(int& width, int& height, float& aspect);
// The part of the texture the picture fills (1, 1 for the hardware renderer).
void frameUv(float& u, float& v);
double coreFps();
float measuredFps();

// Save states: slots 0..9, and the resume state (slot -2).
constexpr int StateSlots = 10;
constexpr int ResumeSlot = -2;
bool saveState(int slot);
bool loadState(int slot);
bool stateExists(int slot, std::string *when = nullptr);
bool stateExistsFor(const std::string& gamePath, int slot);
std::string statePath(int slot);
std::string stateThumbPath(int slot);

// Discs of the running game (an .m3u, or the files of one game).
int discCount();
int discIndex();
std::string discLabel(int index);
bool setDisc(int index);

// Messages from the core, for the screen.
struct Message
{
	std::string text;
	double until;
	int progress;	// -1: none
};
std::vector<Message> messages();
void addMessage(const std::string& text, double seconds = 3.0);

// The controller types changed (settings): tell the core.
void applyControllers();
// Which BIOS files are in <root>bios/, for the About page.
std::string biosSummary();
// Memory card 1 as libretro save RAM, when that card type is chosen.
void flushSaveRam();
}

// ------------------------------------------------------------------- vfs.cpp
namespace vfs
{
// The retro_vfs_interface the core gets: its URI-shaped paths (smb://) come
// here; local files stay with libretro-common's own implementation.
void *interface();
}

// ------------------------------------------------------------------- smb.cpp
namespace smb
{
struct Entry
{
	std::string name, path;
	bool directory = false;
	uint64_t size = 0;
};
class File
{
public:
	virtual ~File() = default;
	virtual size_t read(void *out, size_t bytes) = 0;
	virtual int seek(int64_t offset, int whence) = 0;
	virtual int64_t tell() = 0;
	virtual int64_t size() = 0;
	virtual bool failed() = 0;
};
// Reads <root>network.cfg (and writes a template when there is none).
void loadConfig();
// The folders it names, as smb://server/share/folder.
const std::vector<std::string>& gameFolders();
bool isNetworkPath(const std::string& path);
std::vector<Entry> list(const std::string& path);
File *open(const std::string& path);
// 1 there, 0 not there, -1 the share cannot be reached.
int stat(const std::string& path, Entry& entry);
// A network game is about to be loaded: until endLoad(), with "load into
// memory" on, each file opened is read whole into memory.
void beginLoad();
void endLoad();
void cancelLoad();
void retryNow();
// Gives back the memory of the game that ended.
void releaseImages();
// Reads a network game's files into memory on a thread of its own (what
// "load into memory" does), or just checks they open when that is off.
enum { PrecacheIdle, PrecacheRunning, PrecacheDone, PrecacheFailed };
void startPrecache(const std::string& gamePath);
int precacheState();
void finishPrecache();
struct Status
{
	std::string text;
	float progress = -1.f;
};
Status status();
unsigned failures();
std::string lastError();
void clearError();
}

// --------------------------------------------------------------- library.cpp
namespace library
{
enum Source { Internal, Usb, Network, SourceCount };
struct Game
{
	std::string path;			// the file to start: an image, or an .m3u
	std::string name;			// cleaned for the screen
	std::string fileTitle;		// file name without extension (covers are named so)
	std::string region;			// "USA", "Europe", "Japan" or empty
	std::vector<std::string> discs;	// more than one for a grouped game
	uint64_t size = 0;
	int source = Internal;
};
void init();
// Starts a scan of a source on a thread of its own; the list is replaced when
// it ends. `force` asks the share even when a kept list exists.
void scan(int source, bool force);
bool scanning(int source);
bool scanned(int source);
std::string scanStatus(int source);
// The games of a source, by name. The generation changes with the list.
std::vector<Game> games(int source);
unsigned generation(int source);
std::string sourceName(int source);
bool sourceAvailable(int source);
// What the source's folder is, for the empty-list hint.
std::string sourceHint(int source);
}

// ---------------------------------------------------------------- covers.cpp
namespace covers
{
void init();
// The cover file for a game (<root>covers/<file title>.png or .jpg), or
// empty; asks for a download when there is none and downloads are on.
std::string find(const library::Game& game);
unsigned generation();
std::string status();
}

// ---------------------------------------------------------------- cheats.cpp
namespace cheats
{
struct Cheat
{
	std::string name;			// "Infinite Lives", or the part after the group
	std::string group;			// "Select Widescreen Aspect Ratio" for group\\name entries
	std::string author, description;
	std::string code;			// GameShark lines, '+' separated; '?' where a choice goes
	bool manual = false;		// applied once on request, not every frame
	bool patch = false;			// from the patch database (widescreen, 60 fps...)
	bool supported = true;		// every code type is one the emulator has
	bool enabled = false;
	// A value the user picks, put in place of the code's '?' digits: named
	// choices, or a range.
	std::vector<std::pair<std::string, uint32_t>> choices;
	bool hasRange = false;
	uint32_t rangeLow = 0, rangeHigh = 0;
	uint32_t value = 0;
	// Emulator settings the entry asks for while it is on (a widescreen
	// patch's aspect ratio), as core option key and value.
	std::vector<std::pair<std::string, std::string>> settings;
};
void init();
// Loads the running game's cheats and patches from the databases (and the
// user's own <root>cheats/<serial>.cht), with what was switched on before.
void loadFor(const std::string& serial);
void unload();
std::vector<Cheat>& list();
// Switches one on or off (others of its group go off), saves and applies.
void setEnabled(size_t index, bool enabled);
// The value a cheat with choices uses (saved, applied when it is on).
void setValue(size_t index, uint32_t value);
// Runs a manual cheat once.
void runOnce(size_t index);
// Hands the enabled set to the emulator (after a start, a reset, a state).
void apply();
// How many cheats and patches the databases hold, for the About page.
std::string summary();
}

// ----------------------------------------------------------------- ui.cpp
namespace ui
{
void init();
// One frame of the interface: input, then ImGui windows. `gameVisible` says
// the emulator's picture is behind it.
void frame();
// Whether the title should end.
bool quitRequested();
// The emulator must not run this frame (a menu is over the game).
bool blocksEmulation();
}

// ------------------------------------------------------------------ helpers
std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
void makeDir(const std::string& path);		// 0777, with its parents
bool readFile(const std::string& path, std::vector<uint8_t>& out);
bool writeFile(const std::string& path, const void *data, size_t bytes);	// 0666
std::string lowercase(std::string text);
std::string baseName(const std::string& path);	// after the last '/'
std::string fileTitle(const std::string& path);	// base name without extension
std::string extension(const std::string& path);	// lower case, with the dot
std::string trim(const std::string& text);
double now();	// seconds, monotonic

}
