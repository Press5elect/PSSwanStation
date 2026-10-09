/*
	PSSwanStation - the frontend's shared declarations.

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
#include <memory>
#include <string>
#include <vector>

namespace fe
{

// Where the user's files are, with a trailing '/': games, BIOS, covers,
// cheats, data, the logs. The title's folder (/app0/ on the console, which is
// /data/homebrew/PPSA99248/ over FTP; the test folder on a PC), unless "keep
// my files outside the title folder" is on and the folder outside
// (/data/psswanstation/) can be reached.
extern std::string rootDir;
// The title's own folder, with a trailing '/': the program, its assets,
// sce_sys, frontend.cfg. The updater replaces files here and nowhere else.
extern std::string appDir;
// The root as a person reaches it over FTP, and the title's folder.
std::string shownRoot();
std::string shownApp();

// The title's name; the emulator inside it is SwanStation.
constexpr const char *AppName = "PSSwanStation";
constexpr const char *TitleId = "PPSA99248";
extern const int BuildNumber;
// "2026-10-04", and who makes the builds.
extern const char *const BuildDate;
constexpr const char *Developer = "Press5elect";

// ------------------------------------------------------------------ diag.cpp
namespace diag
{
// Opens <dir>psswanstation-boot.log for this run (the two before it are kept as
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
	// The motion sensor, when the pad has one and it is on (padMotion): which
	// way gravity pulls, in g, in the pad's own axes (x to its right, y up
	// out of its face, z towards the player), and how fast it turns about
	// those axes, in radians a second.
	bool hasMotion = false;
	float gravity[3] = { 0, 0, 0 };
	float turning[3] = { 0, 0, 0 };
	// A finger on the touch pad: where, 0..1 across and down.
	bool touching = false;
	float touchX = 0, touchY = 0;
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
// The pad's light bar: a colour, or (0, 0, 0) to give it back to the console.
void padLight(int index, uint8_t red, uint8_t green, uint8_t blue);
// Switches the motion sensors of every pad on or off.
void padMotion(bool on);

// The sound output: opens it and starts the thread that feeds it from
// audio::render(). False when there is none.
bool audioOpen();
void audioClose();

// One HTTP(S) GET. The status code, or -1 when the request could not be made.
// The answer's body is given for error statuses too (a server's reason).
int httpGet(const std::string& url, std::vector<uint8_t>& out, unsigned seconds, int *error = nullptr);
// One HTTP(S) GET written to a file as it arrives, for what is too large for
// memory. `progress` is told how far it is (total 0 when the server did not
// say) and stops it by answering false. The status code, or -1; anything but
// 200 leaves no file.
int httpDownload(const std::string& url, const std::string& path, uint64_t limit,
		const std::function<bool(uint64_t done, uint64_t total)>& progress);
bool httpAvailable();

// Seconds to add to UTC for the console's local time (its time zone and
// summer time), for clocks on the screen.
int localTimeOffset();
// Whether the place the files are kept in (Settings, "Where my files are
// kept") could be used this run, and why not when it could not.
bool outsideAvailable();
std::string outsideProblem();
// The previous start did not get as far as the library (it is tried without
// leaving the sandbox this time).
bool startedSafely();

// USB drives: whether they can be read at all, and the folders found on them.
bool usbAvailable();
std::vector<std::string> usbGameDirs();
// The top of each USB drive that is plugged in and can be read (/mnt/usb0),
// without a trailing '/'.
std::vector<std::string> usbDrives();

// Whether the console's own menu (the PS button's) is over the title: 1 it
// is, 0 it is not, -1 the console does not say.
int systemUiOverlaid();
// Free memory, for the About page: bytes, or 0 when unknown.
uint64_t freeMemory();
// Whether memory can be made executable (the recompiler needs it).
bool jitAvailable();
}

// -------------------------------------------------------------- netfiles.cpp
namespace netfiles
{
// Whether the share's "files" folder is set and something is kept there.
bool wanted();
// Brings the files up to date with the share, on a thread of its own: as the
// title starts, when a game closed, and when asked.
void startUp();
void gameClosed();
void now();
// True while that runs (a game is not started meanwhile).
bool working();
// Waits for it (the title closes).
void finish();
// What it is doing, or what it did last.
std::string status();
// How many files were found changed on both sides, this run.
int conflicts();
}

// --------------------------------------------------------------- storage.cpp
namespace storage
{
// The folder a user's files are kept in when they are kept outside the
// title's folder.
constexpr const char *OutsideDir = "/data/psswanstation/";
// The folder on a USB drive they are kept in (at the top of the drive).
constexpr const char *UsbFolder = "PSSwanStation";
// The folders a user's files go in, made under `root` (0777).
void makeFolders(const std::string& root);
// The first start with the files outside: what the title's folder holds of
// them is copied to `to`, where it is not there yet. How many files were copied.
int migrate(const std::string& from, const std::string& to);
// The display mode sce_sys/param.json declares (0 59.94 Hz, 1 119.88 Hz,
// 2 119.88 Hz with a variable refresh rate), or -1; and making it what the
// setting says, true when the file was changed (it counts from the next start).
int displayModeIn(const std::string& paramJson);
bool syncDisplayMode(const std::string& paramJson, int displayMode);
// A start begins: true when the one before it never finished. And it has
// finished: the library has been on the screen for a while, or the title closes.
bool startBegan(const std::string& appDir);
void startCompleted(const std::string& appDir);
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
// One of the interface's sounds: 48 kHz stereo frames, mixed into the output
// from now on, whether a game runs or not.
void playSound(std::shared_ptr<const std::vector<int16_t>> frames);
// Fades out what is playing of them.
void stopSounds();
// The menus' music: 48 kHz stereo frames played round and round, faded in;
// null fades out what plays. `volume` 0..100.
void setMusic(std::shared_ptr<const std::vector<int16_t>> frames);
void setMusicVolume(int percent);
// The game's sound is not heard (fast forward, rewind); the ring is emptied.
void setMuted(bool muted);
}

// ----------------------------------------------------------------- sound.cpp
// The interface's sounds. None is a recording: each is computed when the
// title starts.
namespace sound
{
enum Id
{
	Splash,		// the start-up animation, from its first moment to the open doors
	Flight,		// the swan's flight to its corner
	Chime,		// the lift's bell alone
	// The menus: the cursor moved, something was chosen, a step back, another
	// tab or section, a letter typed.
	Move,
	Select,
	Back,
	Tab,
	Key,
	Unlock,		// an achievement was earned
	Count
};
void init();
// The first three are not heard when "Start-up sound" is off, the others
// when "Interface sounds" is.
void play(Id id);
void stop();
// The menus' music, by the setting: the title's own piece (computed, as the
// sounds are), or the user's file (<root>music/menu.wav, .ogg or .flac).
// `wanted` says a menu has the screen; in a game it fades out.
void music(bool wanted);
// What the music setting found, for the settings page.
std::string musicStatus();
// An achievement was earned.
void unlock();
}

// When what happens in the start-up animation, in seconds from its start: the
// picture (ui.cpp) and the sound (sound.cpp) keep to the same times.
namespace splashtime
{
constexpr float Rise = 0.05f;		// the swan's head comes up in the bottom right corner
constexpr float Swim = 0.45f;		// and sets off along the bottom edge
constexpr float Arrive = 1.85f;		// at the middle: it looks at the viewer
constexpr float Dive = 2.10f;		// the head goes down
constexpr float Gone = 2.45f;		// it is in the lift, below the screen
constexpr float Clunk = 2.55f;		// the lift's doors shut
constexpr float LiftStart = 2.70f;	// it sets off upwards
constexpr float LiftStop = 3.90f;	// and stops in the middle of the screen
constexpr float Ding = 3.95f;
constexpr float DoorsOpen = 4.05f;
constexpr float DoorsDone = 4.55f;
constexpr float End = 5.40f;		// the swan leaves for the library's corner
constexpr float Flight = 1.50f;		// how long that takes
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
	int scaling = 0;			// 0 fit (keep aspect), 1 integer, 2 stretch (3, before build 15: fit with FSR 1)
	// How the picture is grown to the screen: 0 smooth (bilinear), 1 square
	// pixels (nearest), 2 sharp bilinear, 3 FSR 1, 4 NIS, 5 CAS.
	int scaler = 0;
	int fsrSharpness = 1;		// the sharpening of FSR, NIS and CAS: 0 soft, 1 normal, 2 sharp
	bool linearFilter = true;	// before build 15: square pixels when false (scaler took its place)
	int signal = 0;				// 0 as it is, 1 the dither undone, 2 S-Video, 3 composite
	// The colours, each in steps of 5% from 50% (0) to 150% (20); 10 unchanged.
	int brightness = 10, contrast = 10, saturation = 10, gamma = 10;
	int volume = 100;
	bool showFps = false;
	// How frames are timed: 0 by the display (one frame of the game for each
	// refresh when the rates are within one percent), 1 the game's own speed
	// on the display's refreshes, 2 the game's own speed by the clock, each
	// frame shown when it is ready (for a display with a variable refresh rate).
	int pacing = 0;
	// What the console is asked for (sce_sys/param.json, from the next start):
	// 0 59.94 Hz, 1 119.88 Hz, 2 119.88 Hz and a variable refresh rate.
	int displayMode = 0;
	bool blackFrames = false;	// at 119.88 Hz: a black refresh after each of the game's frames
	int frameGeneration = 0;	// pictures made between the game's (display::generated): 0 off, 1 on (2, before build 15: lighter)
	int fgQuality = 1;			// 0 performance, 1 balanced, 2 quality (with the two-way check)
	int fgCap = 0;				// 0 up to the screen's rate, 1 up to 60 a second
	int fgVideos = 0;			// 0 videos too, 1 not while a video plays (the PlayStation's MDEC decoding)
	int fgMode = 0;				// 0 between the game's last two pictures, 1 ahead of the last (no waiting)
	bool fgRunAhead = false;	// one frame of run-ahead while it is on, to take back its delay
	bool fgDebug = false;		// the movement shown in colours
	// 0 off, 1 soft scanlines, 2 scanlines, 3 scanlines and a shadow mask; 4 on,
	// a picture tube preset: display::crtPresetNames()[crt - 4] (crt-guest-advanced's
	// kinds, then the presets brought in from a USB drive)
	int crt = 0;
	int border = 0;				// beside a 4:3 picture: 0 black, 1 the picture's own light, 2 a gradient, 3 a picture file
	int preset = 0;				// the picture preset last chosen: 0 none, 1 original, 2 sharp, 3 enhanced, 4 speedrun
	bool autoSaveOnExit = false;	// save a resume state when a game is closed
	bool autoLoadOnStart = false;	// and start from it
	// Sleep-safe saving: the resume state is saved every five minutes of play
	// and when the console's menu opens over the game, so a title closed by the
	// console (rest mode) can be continued at the next start.
	bool sleepSafe = false;
	bool verboseLog = false;
	bool watchOverlay = true;	// the values watched (memsearch) shown over the game
	bool web = false;			// the web panel for a phone or a computer (web.cpp), on port 3311
	bool speedrun = false;		// the speedrun timer over the game, its shortcuts and its conditions
	int speedrunClock = 0;		// 0 game time (the emulator's frames), 1 real time	// everything the emulator says into the boot log, and a line about speed every ten seconds
	// Players 1 to 4: 0 digital, 1 DualShock, 2 analog joystick, 3 none,
	// 4 neGcon, 5 GunCon.
	int controller[4] = {1, 1, 1, 1};
	float deadZone = 0.10f;
	bool rumble = true;
	bool playerLights = true;	// each player's light bar in their colour
	// The PlayStation's sixteen buttons (libretro's order: Cross, Square,
	// Select, Start, Up, Down, Left, Right, Circle, Triangle, L1, R1, L2, R2,
	// L3, R3): which button of the pad presses each (the same order; -1 none).
	int remap[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
	unsigned turbo = 0;			// the PlayStation buttons that fire repeatedly while held, as bits
	int turboRate = 1;			// 0 slow (7 a second), 1 normal (10), 2 fast (15)
	bool hotkeys = true;		// OPTIONS held with another button: fast forward, rewind, quick save and load
	int fastForward = 1;		// its speed: 0 2x, 1 3x, 2 4x, 3 8x, 4 as fast as it goes
	bool rewind = false;		// keep the last while of play in memory, to go back
	int rewindDetail = 1;		// a state every 0: 3 frames, 1: 6, 2: 12
	int rewindMemory = 1;		// 0: 128 MB, 1: 256, 2: 512, 3: 1024
	int motion = 0;				// player 1's pad, tilted: 0 nothing, 1 steers (left stick, left and right), 2 is the left stick
	int motionRange = 2;		// how far it must be tilted for the stick's whole way: 0 20, 1 30, 2 40, 3 55, 4 70 degrees
	bool motionInvert = false;
	bool swapConfirm = false;	// Circle confirms in the menus
	bool splash = true;			// the start-up animation
	bool splashSound = true;	// and its sound
	bool uiSounds = true;		// the menus' sounds
	int music = 0;				// in the menus: 0 none, 1 the title's own, 2 the file in the music folder
	int musicVolume = 60;
	int animations = 0;			// 0 everything moves, 1 little does, 2 nothing does
	int uiScale = 100;			// percent
	bool highContrast = false;	// black behind the menus, white text, brighter dim text
	bool colourBlind = false;	// good and bad in blue and orange instead of green and red
	int accent = 0;
	int sort = 0;				// library: 0 name, 1 last played, 2 most played, 3 year, 4 size
	int filter = 0;				// 0 all, 1 favourites, 2 not played yet, 3 hidden
	int regionFilter = 0;		// 0 all, 1 USA, 2 Europe, 3 Japan
	int idleMinutes = 5;		// the swan takes the screen after this long without a button; 0 never
	bool clock = true;			// the time in the library's header
	// Where the user's files are kept: 0 the title's folder, 1 /data/psswanstation,
	// 2 a USB drive, 3 the network share's "files" folder (with a working copy on
	// the console). 1 and 2 need the sandbox left; 3's copy is kept as 1 is where
	// it can be, else in the title's folder.
	int filesAt = 0;
	bool cardsOnShare = false;		// copies of the memory cards in the share's "files" folder
	bool coversFromShare = false;	// covers brought from the share's "files" folder
	int cardBackups = 10;		// copies kept of each memory card that changed; 0 none
	bool updateCheck = true;	// ask the releases page at start-up
	bool achievements = false;	// RetroAchievements
	bool hardcore = false;
	bool unofficial = false;
	int netplayDelay = 2;		// frames
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
// The frontend's picture settings (scaling, fsr_sharpness, linear_filter,
// pacing, black_frames, frame_generation, crt, border, preset: their names in
// frontend.cfg) can be the loaded game's own as well. frontend() has what is
// in force, the game's where it has one. picture(name, false) is the value
// for every game, picture(name, true) the one in force.
int picture(const std::string& name, bool forGame);
void setPicture(const std::string& name, int value, bool forGame);
bool hasGamePicture(const std::string& name);
void clearGamePicture(const std::string& name);
const std::string& gameSerial();
// True once after a value changed (RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE).
bool takeChanged();
// Counts the changes.
unsigned generation();
// Values that hold while something else asks for them (a widescreen patch's
// aspect ratio): over the game's and the global ones, never saved.
void setOverride(const std::string& key, const std::string& value);
// A value the frontend holds for a while, over the user's own (an empty one
// lets go); own() is the user's value, past it.
void hold(const std::string& key, const std::string& value);
const char *own(const std::string& key);
void clearOverrides();
bool hasOverride(const std::string& key);
void setVisible(const std::string& key, bool visible);
// Every setting of the emulator back to what it is at first (the games' own
// values stay), and the frontend's own likewise.
void resetGlobal();
void resetFrontend();
// One of the frontend's settings by its name in frontend.cfg, as a whole
// number (a switch 0 or 1; a picture setting's value for every game). False
// when there is no such setting. Setting it saves frontend.cfg.
bool frontendValue(const std::string& name, int& value);
bool setFrontendValue(const std::string& name, int value);
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
// `disc` is the one in the tray at the start, of a game on several; `serial`
// is that disc's, when the caller knows it, so the game's own settings are in
// place before it boots.
bool start(const std::string& path, int stateSlot = -1, int disc = 0, const std::string& serial = "");
void stop();
void reset();
// Runs the core for one display refresh (none, one or more emulated frames).
void runFrame();
std::string lastError();

// Fast forward and rewind, while they are held. Rewind steps back through
// the states kept in memory (settings); it is refused when none are kept.
void setFastForward(bool on);
bool fastForward();
void setRewinding(bool on);
bool rewinding();
// How much play the rewind memory holds, in seconds.
float rewindSeconds();
// The pads are not the game's for now (a shortcut is being pressed).
void setInputBlocked(bool blocked);
// With black frame insertion: this refresh shows black.
bool blackFrame();
// With frame generation: where this refresh is between the game's picture
// before (0) and its latest (1), and whether the latest is new this refresh.
// False when no picture is to be made (it is off, or the game is being run
// fast or backwards).
bool generationPhase(float& phase, bool& fresh);
// How many new pictures a second the running game shows, when that can be
// counted (it moves its display from one buffer to another); else 0.
double picturesPerSecond();
// How many pictures the screen showed in the last second, the game's and the
// ones frame generation made.
float shownPerSecond();
// A new picture of the game came in this refresh; and how long the game takes
// between two, in milliseconds.
bool newPicture();
float pictureMs();
// The lines the PlayStation draws (240, 480...), for the scanlines.
int nativeLines();
// The game's picture as it is now, without the interface: RGBA8, at most
// `maxHeight` lines. Only between a frame's begin and its end.
bool capture(int maxHeight, std::vector<uint8_t>& rgba, int& width, int& height);
// The time of the screen's clock ("21:07"), or empty when the console's is not set.
std::string clockText();

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
bool stateExistsFor(const std::string& gamePath, int slot, std::string *when = nullptr);
// The serial a game ran under before (<root>data/game-ids.txt), or empty.
std::string knownSerial(const std::string& gamePath);
// Read from a disc image now, without starting it; empty when it has none.
std::string readSerial(const std::string& imagePath);
std::string statePath(int slot);
// The picture saved with a state (what the game showed).
std::string stateThumbPath(int slot);
std::string stateThumbPathFor(const std::string& gamePath, int slot);
// The slot the shortcuts save to and load from: the one used last, or chosen.
int quickSlot();
void setQuickSlot(int slot);

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
	// A notice with a heading and perhaps a picture (an achievement).
	std::string title, picture;
};
std::vector<Message> messages();
void addMessage(const std::string& text, double seconds = 3.0);
void addNotice(const std::string& title, const std::string& text, const std::string& picture = "",
		double seconds = 5.0);

// RetroAchievements (achievements.h does the work; these are the frontend's
// side of it). The settings changed, or the title started: sign in with the
// key kept from before, or out.
void achievementsApply();
// With a password, the first time: the key the server answers with is kept
// (<root>data/retroachievements.cfg), never the password.
void achievementsLogin(const std::string& user, const std::string& password);
void achievementsLogout();
// The name kept, or empty.
std::string achievementsUser();
// Once a frame of the interface, game or no game: the players' lights, the
// motion sensors, and RetroAchievements' answers (what happened is put on
// the screen).
void tick();
// A light gun's aim for the screen, when player `port` holds one: -1..1
// across and down the picture. False when they hold none.
bool gunAim(int port, float& x, float& y);

// The controller types changed (settings): tell the core.
void applyControllers();
// Hardcore mode of RetroAchievements is on with a game loaded: no states are
// loaded, no cheats, no rewind.
bool restricted();
// Netplay: starts hosting the running game, or joins a host with it; the
// state it is in is netplay::state().
bool netplayHost();
bool netplayJoin(const std::string& address);
void netplayStop();
// Where texture packs are looked for: <root>textures, then PSSwanStation/textures
// on each USB drive, then (where build 15 and before looked) a folder named
// textures in a USB drive's games folder. A game's pack is <folder>/<serial>/.
std::vector<std::string> textureFolders();
// How many files a game's pack holds, in the first of those folders that has
// one (`where`: which).
int texturePackFiles(const std::string& serial, std::string *where = nullptr);
// Which BIOS files are in <root>bios/, for the About page.
std::string biosSummary();
// Memory card 1 as libretro save RAM, when that card type is chosen.
void flushSaveRam();
// Sleep-safe saving: the game that is being started, as the library has it
// (kept with the resume state, to start it again).
struct LaunchedGame
{
	std::string path, name, fileTitle, region;
	std::vector<std::string> discs;
	int source = 0, disc = 0;
};
void rememberGame(const LaunchedGame& game);
// The title was closed while a game ran, after a sleep-safe save of it (and
// so not by closing the game): that game, once; false when there is none.
bool interruptedGame(LaunchedGame& game);
void forgetInterruptedGame();
// Debug: recordings of what the players pressed (recorder.cpp). A recording
// begins from a state of the running game, taken as it starts; playing one
// back loads that state and gives the game the recorded presses, frame by
// frame, while the pads are left out. Loading a state, a reset, rewinding,
// netplay or closing the game ends either.
bool recordStart();
void recordStop();
bool playbackStart(const std::string& file);
void playbackStop();
// The folder the running game's recordings are in (<root>data/recordings/<serial>).
std::string recordingsFolder();
}

// ---------------------------------------------------------------- rewind.cpp
// The states rewinding steps back through, in memory.
namespace rewind
{
// How much memory they may take, in bytes.
void configure(size_t bytes);
void clear();
// The emulator's state now: the newest.
void push(const std::vector<uint8_t>& state);
// The newest kept, which is then forgotten: the one before it is the newest.
// False when there is none.
bool pop(std::vector<uint8_t>& state);
size_t count();
size_t bytes();
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
// The folders it names, as smb://server/share/folder, ftp://server/folder or
// nfs://server/path.
const std::vector<std::string>& gameFolders();
// network.cfg's "files" line: the folder on a share for the title's own files
// (memory card copies, covers, everything when the files are kept there), or
// "" when it names none.
const std::string& filesFolder();
// Writing to a network folder: an SMB or NFS share (an FTP folder is only
// read). A file is written beside its place and then put there, so that one
// cut short is never taken for it; the folders it is in are made.
bool canWrite(const std::string& path);
bool writeFile(const std::string& path, const void *data, size_t bytes, std::string& error);
bool makeFolders(const std::string& path);
bool remove(const std::string& path);
// A whole file, read as it is now (never from memory): false when it cannot be.
bool readWhole(const std::string& path, std::vector<uint8_t>& out);
bool isNetworkPath(const std::string& path);
std::vector<Entry> list(const std::string& path);
File *open(const std::string& path);
// 1 there, 0 not there, -1 the share cannot be reached.
int stat(const std::string& path, Entry& entry);
// A network game is about to be loaded: until endLoad(), with "load into
// memory" on, each file opened is read whole into memory.
void beginLoad();
// How much the game being loaded into memory is, all its files together:
// status() then counts through the whole of it. Kept until clearLoad().
void setLoadTotal(uint64_t bytes);
void clearLoad();
void endLoad();
void cancelLoad();
void retryNow();
// Gives back the memory of the game that ended.
void releaseImages();
#if defined(SWANSTATION_HOST)
// The PC test build only: reads a network file every way and compares it.
int selfTest(const char *spec);
#endif
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
	// The game being read into memory, all its files together: 0 when none is.
	uint64_t done = 0, total = 0;
	bool waiting = false;		// the server has not answered for a while
};
// The calling thread's files are never read into memory (it only looks at a
// disc while something else may be loading one).
void streamOnThisThread(bool only);
Status status();
unsigned failures();
std::string lastError();
void clearError();
// Waking a server that sleeps: the hardware address network.cfg names
// ("wake = 00:11:32:AA:BB:CC"), or empty.
std::string wakeAddress();
// Sends the wake-up packet (Wake-on-LAN) to it. False when none is named or
// it could not be sent.
bool wake();
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

// Favourites and hidden games, by the game's path (<root>data/marks.txt).
bool favourite(const std::string& gamePath);
bool hidden(const std::string& gamePath);
void setFavourite(const std::string& gamePath, bool on);
void setHidden(const std::string& gamePath, bool on);
unsigned marksGeneration();
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
// Choosing a cover. The three kinds of picture the collection has for a
// game: its box, its title screen, a moment of play.
enum Kind { BoxArt, TitleScreen, InGame, KindCount };
enum ChooseState { ChooseIdle, ChooseWorking, ChooseDone, ChooseNotFound, ChooseFailed };
// Fetches that picture for the game and makes it its cover (on a thread).
void choose(const library::Game& game, int kind);
ChooseState chooseState();
// Removes the game's cover: the automatic one comes back when downloads are on.
void remove(const library::Game& game);
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
// A later disc of a game often has no file of its own in the database: the
// first disc's (`firstDisc`) is then used.
void loadFor(const std::string& serial, const std::string& firstDisc = "");
void unload();
// The serial the list was found under.
const std::string& serial();
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
// Fetches the newest database from the chtdb project's releases into
// <root>data/, where it is used instead of the one the title came with (on a
// thread; refreshStatus says how it goes, and ends with a full stop).
void refresh();
bool refreshing();
std::string refreshStatus();
}

// ---------------------------------------------------------------- gamedb.cpp
namespace gamedb
{
struct Info
{
	std::string serial, name, developer, publisher, genre, description;
	int year = 0, month = 0, players = 0;
};
void init();
bool find(const std::string& serial, Info& out);
// The serial of the disc whose image file is named so (Redump's names), or empty.
std::string serialByName(const std::string& fileTitle);
std::string summary();
}

// --------------------------------------------------------------- history.cpp
namespace history
{
struct Entry
{
	int64_t lastPlayed = 0;		// seconds since 1970; 0: never
	uint64_t seconds = 0;		// played in all
	int disc = 0;				// the disc last in the tray (of a game on several)
};
void init();
// By the game's path in the library.
Entry get(const std::string& gamePath);
void begin(const std::string& gamePath);
void end();
void setDisc(const std::string& gamePath, int disc);
// The paths of the games played, the latest first.
std::vector<std::string> recent(size_t most);
unsigned generation();
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
