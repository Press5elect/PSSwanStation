/*
	PSSwanStation - the libretro host: the emulator core, run by the title.

	SPDX-License-Identifier: GPL-3.0-or-later

	SwanStation is a libretro core, and it is linked into the title as it is:
	this file is the other side of that interface, the part RetroArch plays on
	a PC. It answers the core's environment calls (options, folders, messages,
	the disc control and the hardware renderer's Vulkan interface), takes its
	picture and its sound, and gives it the pads.

	The picture. With the hardware renderer the core draws with the Vulkan
	device the title made at start-up (display.cpp): the context negotiation
	libretro has for that is accepted and never used, because the device is
	already there, and the core's own context destruction is compiled out
	(SWANSTATION_STANDALONE in src/core/gpu_hw_vulkan.cpp). Each frame the core
	hands over an image view (set_image), which ImGui draws under the menus.
	With the software renderer it hands over pixels, which go into a texture.

	Pacing, three ways (the setting). By the display: one emulated frame for
	each refresh when the two rates are within one percent (a 59.94 Hz display
	and an NTSC game; every second refresh at 119.88 Hz), and the sound is
	resampled to follow (audio.cpp). At the game's own speed on the display's
	refreshes: a PAL game on the console's 59.94 Hz output gets five frames in
	six refreshes; the console has no 50 Hz mode. And at the game's own speed by
	the clock, each frame handed over the moment it is due: on a display with
	a variable refresh rate that is when it is shown.

	Fast forward runs more frames a refresh, unheard. Rewind steps back through
	states kept in memory (rewind.cpp), which this file takes every few frames
	while "Rewind" is on. Both are held from a shortcut (ui.cpp).

	The pads reach the game through a map (which button of the pad presses
	which of the PlayStation's, and which fire repeatedly while held), with the
	first pad's tilt as its left stick when that is on. In netplay the two
	players' buttons come from netplay.cpp instead, the same on both consoles.
*/
#include "fe.h"
#include "achievements.h"
#include "display.h"
#include "memcard.h"
#include "netplay.h"

#include <libretro.h>
#include <libretro_vulkan.h>

#include "common/vulkan/context.h"
#include "core/system.h"

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fe::host
{
// discid.cpp: the disc, and the emulated memory.
void *discOpen(const std::string& imagePath);
bool discRead(void *disc, uint32_t sector, uint8_t *out2048);
void discClose(void *disc);
uint8_t *coreRam(uint32_t& size);
uint8_t *coreScratchpad();
const uint8_t *coreBios(uint32_t& size);
uint32_t coreDisplayChanges();
uint32_t coreDrawCommands();
uint32_t coreVideoBlocks();
uint32_t coreDrawResolutionScale();
void nameTextureFolders();

namespace
{

// --------------------------------------------------------------- the state

bool coreReady;
bool isRunning;
bool isPaused;
GameInfo current;
// The first disc's serial, of a game on several: its cheats serve the discs
// that have none of their own.
std::string firstDiscSerial;
std::string errorText;
retro_system_av_info av{};
double pacing;
// Frames the emulator ran in the last second.
int ranThisSecond;
double secondStarted;
float fpsMeasured;

retro_hw_render_callback hwRender{};
bool hwRenderSet;
retro_hw_render_interface_vulkan vulkanInterface{};
retro_vulkan_image hwImage{};
bool hwImageValid;
unsigned frameWidth, frameHeight;
bool frameIsHardware;
retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
display::Texture *softwareTexture;

retro_disk_control_ext_callback disk{};
bool diskSet;
retro_core_options_update_display_callback_t updateDisplay;
float rumble[platform::MaxPads][2];

// Fast forward and rewind, held from the shortcuts.
bool ffOn, rewindOn;
bool rewindDry;				// rewinding found nothing more to step back to
double rewindDue;			// refreshes until the next step back
int sinceKept;				// emulated frames since a state was kept for rewinding
std::vector<uint8_t> stateBuffer;
bool inputBlocked;
bool blackNow;
// Frame generation: whether a picture is to be made this refresh, where it
// is between the game's last two, and whether the latest is new.
bool genOn, genFresh;
float genPhase = 1.f, lastShownPhase = 1.f;
// The pictures the screen showed in the last second, the game's and made ones.
int shownThisSecond;
float shownMeasured;
// How often the game shows a new picture. Most games draw into a second
// buffer and then move the display's start to it: that is counted (the
// emulator's GPU says how often it happened), and a game that draws 30 or 20
// pictures a second is seen to. A game that never does that is counted by
// whether it drew anything in a frame: a frame it drew nothing in shows the
// picture before again.
struct Cadence
{
	uint32_t changesSeen = 0, drawsSeen = 0, blocksSeen = 0;
	uint64_t videoAtFrame = ~0ull;		// the emulated frame a video was last decoded in
	// The last few gaps between pictures, in refreshes, and when the latest
	// came by the clock (any-rate generation).
	int gaps[4] = { 1, 1, 1, 1 };
	unsigned gapIndex = 0;
	double pictureAtTime = 0, gapSeconds = 0;
	uint64_t changedAtFrame = ~0ull;	// the emulated frame the display's start last moved in
	bool pictureNow = false;			// a new picture came in this refresh
	uint64_t refreshes = 0, pictureAtRefresh = 0, pictureAtFrame = 0;
	int gapRefreshes = 1, gapFrames = 1;	// between the last two pictures
	int sincePicture = 0;				// refreshes since the last one
	bool counted = false;				// by the display's start, not by frames
} cadence;
uint64_t emulatedFrames;
double clockNext;			// pacing by the clock: when the next frame is due
int lastSlot = -1;			// the state slot used last, for the shortcuts

// Each player's light gun aim, -1..1 across and down the picture.
struct Aim
{
	float x = 0, y = 0;
};
Aim aims[platform::MaxPads];

// Netplay: the two players' buttons for the frame being run, and what the
// session changed that is put back when it ends.
bool netOn;
netplay::Input netInputs[netplay::Players];
bool netSettingsTaken;
int netController[2] = { -1, -1 };

std::mutex messageMutex;
std::vector<Message> messageList;

std::string saveDir, systemDir, cacheDir;

// -------------------------------------------------------------- the picture

void setImage(void *, const retro_vulkan_image *image, uint32_t, const VkSemaphore *, uint32_t)
{
	if (image != nullptr)
	{
		hwImage = *image;
		hwImageValid = true;
	}
	else
		hwImageValid = false;
}

uint32_t getSyncIndex(void *)
{
	return 0;
}

uint32_t getSyncIndexMask(void *)
{
	return 1;
}

void setCommandBuffers(void *, uint32_t, const VkCommandBuffer *)
{
}

void waitSyncIndex(void *)
{
}

void lockQueue(void *)
{
}

void unlockQueue(void *)
{
}

void setSignalSemaphore(void *, VkSemaphore)
{
}

void fillVulkanInterface()
{
	vulkanInterface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
	vulkanInterface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
	vulkanInterface.handle = &vulkanInterface;
	vulkanInterface.instance = static_cast<VkInstance>(display::vkInstance());
	vulkanInterface.gpu = g_vulkan_context->GetPhysicalDevice();
	vulkanInterface.device = g_vulkan_context->GetDevice();
	vulkanInterface.get_device_proc_addr = vkGetDeviceProcAddr;
	vulkanInterface.get_instance_proc_addr = vkGetInstanceProcAddr;
	vulkanInterface.queue = g_vulkan_context->GetGraphicsQueue();
	vulkanInterface.queue_index = g_vulkan_context->GetGraphicsQueueFamilyIndex();
	vulkanInterface.set_image = setImage;
	vulkanInterface.get_sync_index = getSyncIndex;
	vulkanInterface.get_sync_index_mask = getSyncIndexMask;
	vulkanInterface.set_command_buffers = setCommandBuffers;
	vulkanInterface.wait_sync_index = waitSyncIndex;
	vulkanInterface.lock_queue = lockQueue;
	vulkanInterface.unlock_queue = unlockQueue;
	vulkanInterface.set_signal_semaphore = setSignalSemaphore;
}

void videoRefresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (data == nullptr)
		return;		// the same frame again
	if (data == RETRO_HW_FRAME_BUFFER_VALID)
	{
		frameIsHardware = true;
		frameWidth = width;
		frameHeight = height;
		return;
	}
	if (width == 0 || height == 0)
		return;
	if (softwareTexture == nullptr || display::textureWidth(softwareTexture) < (int)width
			|| display::textureHeight(softwareTexture) < (int)height)
	{
		display::destroyTexture(softwareTexture);
		// Room for the largest picture the console draws, so a change of video
		// mode does not make a new texture.
		softwareTexture = display::createDynamicTexture(std::max(width, 1024u), std::max(height, 512u));
	}
	display::updateTexture(softwareTexture, data, (int)width, (int)height, pitch,
			pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? display::Xrgb8888
			: pixelFormat == RETRO_PIXEL_FORMAT_RGB565 ? display::Rgb565 : display::Xrgb1555);
	frameIsHardware = false;
	frameWidth = width;
	frameHeight = height;
}

// ---------------------------------------------------------------- the sound

size_t audioBatch(const int16_t *data, size_t frames)
{
	audio::push(data, frames);
	return frames;
}

void audioSample(int16_t left, int16_t right)
{
	const int16_t frame[2] = { left, right };
	audio::push(frame, 1);
}

// ----------------------------------------------------------------- the pads

void inputPoll()
{
}

int16_t stick(float value)
{
	const float dead = options::frontend().deadZone;
	const float magnitude = std::fabs(value);
	if (magnitude < dead)
		return 0;
	const float scaled = std::copysign((magnitude - dead) / (1.f - dead), value);
	return (int16_t)std::lround(std::clamp(scaled, -1.f, 1.f) * 32767.f);
}

// The PlayStation's sixteen buttons, in libretro's order, as the pad's own
// buttons stand for them before any remapping. Select and Start are the two
// halves of the touch pad; OPTIONS belongs to the title (it opens the menu).
uint16_t padButtons(const platform::Pad& pad)
{
	using namespace platform;
	static const uint32_t map[16] = {
		Cross,		// B
		Square,		// Y
		TouchLeft,	// Select
		TouchRight,	// Start
		Up, Down, Left, Right,
		Circle,		// A
		Triangle,	// X
		L1, R1, L2, R2, L3, R3,
	};
	uint32_t buttons = pad.buttons;
	if (pad.l2 > 0.25f)
		buttons |= L2;
	if (pad.r2 > 0.25f)
		buttons |= R2;
	uint16_t mask = 0;
	for (unsigned i = 0; i < 16; i++)
		if (buttons & map[i])
			mask |= (uint16_t)(1u << i);
	return mask;
}

// What a player presses as the game is to see it: the map (Settings,
// Controllers, Buttons), and the buttons that fire by themselves while held.
uint16_t gameButtons(const platform::Pad& pad)
{
	const options::Frontend& settings = options::frontend();
	const uint16_t held = padButtons(pad);
	static const int periods[3] = { 4, 3, 2 };		// frames on, and as many off
	const bool turboOff = (emulatedFrames / (uint64_t)periods[std::clamp(settings.turboRate, 0, 2)]) % 2 == 1;
	uint16_t out = 0;
	for (unsigned target = 0; target < 16; target++)
	{
		const int source = settings.remap[target];
		if (source < 0 || source > 15 || (held & (1u << source)) == 0)
			continue;
		if ((settings.turbo & (1u << target)) != 0 && turboOff)
			continue;
		out |= (uint16_t)(1u << target);
	}
	return out;
}

// The first pad leant to one side, as a steering wheel is turned: how far,
// -1..1 at the angle the setting names. Whichever way up the pad is held,
// leaning it moves gravity along its left-right axis, so that axis alone is
// read. Which sign is "right" follows libScePad's convention as documented;
// "Invert" is there should a console say otherwise.
float tiltSteer(const platform::Pad& pad)
{
	const options::Frontend& settings = options::frontend();
	if (settings.motion == 0 || !pad.hasMotion)
		return 0;
	const float g = std::sqrt(pad.gravity[0] * pad.gravity[0] + pad.gravity[1] * pad.gravity[1]
			+ pad.gravity[2] * pad.gravity[2]);
	if (g < 0.5f || g > 1.6f)
		return 0;		// thrown about: not a lean
	static const float ranges[5] = { 20.f, 30.f, 40.f, 55.f, 70.f };
	const float angle = std::asin(std::clamp(-pad.gravity[0] / g, -1.f, 1.f)) * 57.29578f;
	const float steer = std::clamp(angle / ranges[std::clamp(settings.motionRange, 0, 4)], -1.f, 1.f);
	return settings.motionInvert ? -steer : steer;
}

// One player's pad as the emulator is given it.
netplay::Input localInput(int port)
{
	netplay::Input in;
	const platform::Pad& pad = platform::pad(port);
	if (!pad.connected || inputBlocked)
		return in;
	in.buttons = gameButtons(pad);
	in.lx = stick(pad.lx);
	in.ly = stick(pad.ly);
	in.rx = stick(pad.rx);
	in.ry = stick(pad.ry);
	if (port == 0)
	{
		// The lean adds to the stick, so either steers.
		const float steer = tiltSteer(pad);
		if (steer != 0)
			in.lx = (int16_t)std::clamp((int)in.lx + (int)std::lround(steer * 32767.f), -32767, 32767);
	}
	return in;
}

int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
	if (port >= (unsigned)platform::MaxPads)
		return 0;
	// In netplay players 1 and 2 are the two consoles, the same on both.
	const netplay::Input in = netOn ? (port < netplay::Players ? netInputs[port] : netplay::Input())
			: localInput((int)port);
	const platform::Pad& pad = platform::pad((int)port);
	switch (device & RETRO_DEVICE_MASK)
	{
	case RETRO_DEVICE_JOYPAD:
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
			return (int16_t)in.buttons;
		return id < 16 && (in.buttons & (1u << id)) != 0 ? 1 : 0;
	case RETRO_DEVICE_ANALOG:
		if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
			return id == RETRO_DEVICE_ID_ANALOG_X ? in.lx : in.ly;
		if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
			return id == RETRO_DEVICE_ID_ANALOG_X ? in.rx : in.ry;
		if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON && !netOn && pad.connected && !inputBlocked)
		{
			// How far a button is pressed (the neGcon's I, II and L): the
			// triggers say; any other button is all the way down or not at all.
			if (id == RETRO_DEVICE_ID_JOYPAD_R2)
				return (int16_t)std::lround(pad.r2 * 32767.f);
			if (id == RETRO_DEVICE_ID_JOYPAD_L2)
				return (int16_t)std::lround(pad.l2 * 32767.f);
			return id < 16 && (in.buttons & (1u << id)) != 0 ? 32767 : 0;
		}
		return 0;
	case RETRO_DEVICE_LIGHTGUN:
	{
		if (netOn || !pad.connected || inputBlocked)
			return 0;
		const Aim& aim = aims[port];
		const uint16_t held = in.buttons;
		const auto pressed = [held](unsigned button) { return (held & (1u << button)) != 0; };
		switch (id)
		{
		case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X: return (int16_t)std::lround(aim.x * 32767.f);
		case RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y: return (int16_t)std::lround(aim.y * 32767.f);
		case RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN: return 0;
		case RETRO_DEVICE_ID_LIGHTGUN_TRIGGER:
			return pressed(RETRO_DEVICE_ID_JOYPAD_R2) || pressed(RETRO_DEVICE_ID_JOYPAD_B) ? 1 : 0;
		// Shooting away from the screen, which is how these games reload.
		case RETRO_DEVICE_ID_LIGHTGUN_RELOAD:
			return pressed(RETRO_DEVICE_ID_JOYPAD_L2) || pressed(RETRO_DEVICE_ID_JOYPAD_A) ? 1 : 0;
		case RETRO_DEVICE_ID_LIGHTGUN_AUX_A:
			return pressed(RETRO_DEVICE_ID_JOYPAD_Y) || pressed(RETRO_DEVICE_ID_JOYPAD_L) ? 1 : 0;
		case RETRO_DEVICE_ID_LIGHTGUN_AUX_B:
			return pressed(RETRO_DEVICE_ID_JOYPAD_X) || pressed(RETRO_DEVICE_ID_JOYPAD_R) ? 1 : 0;
		default: return 0;
		}
	}
	default:
		return 0;
	}
}

// A light gun is aimed with the right stick, which moves the aim about the
// picture, and, when the pad's motion sensor answers, by turning the pad.
void moveAims()
{
	for (int port = 0; port < platform::MaxPads; port++)
	{
		if (options::frontend().controller[port] != 5)
			continue;
		const platform::Pad& pad = platform::pad(port);
		Aim& aim = aims[port];
		const float dead = options::frontend().deadZone;
		const float rx = std::fabs(pad.rx) > dead ? pad.rx : 0.f, ry = std::fabs(pad.ry) > dead ? pad.ry : 0.f;
		const float lx = std::fabs(pad.lx) > dead ? pad.lx : 0.f, ly = std::fabs(pad.ly) > dead ? pad.ly : 0.f;
		aim.x += (rx + lx) * 0.030f;
		aim.y += (ry + ly) * 0.040f;
		if (pad.hasMotion)
		{
			// Radians a second, a frame's worth: a quarter turn crosses the picture.
			aim.x -= pad.turning[1] / 60.f * 1.3f;
			aim.y -= pad.turning[0] / 60.f * 1.7f;
		}
		aim.x = std::clamp(aim.x, -0.98f, 0.98f);
		aim.y = std::clamp(aim.y, -0.98f, 0.98f);
	}
}

bool setRumbleState(unsigned port, retro_rumble_effect effect, uint16_t strength)
{
	if (port >= (unsigned)platform::MaxPads)
		return false;
	rumble[port][effect == RETRO_RUMBLE_STRONG ? 0 : 1] = strength / 65535.f;
	return true;
}

// ---------------------------------------------------------------- messages

void logCallback(retro_log_level level, const char *format, ...)
{
	static const char *names[] = { "debug", "info", "warn", "error" };
	char text[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	fprintf(stderr, "[%s] %s", names[std::min<unsigned>(level, 3)], text);
	const size_t length = strlen(text);
	if (length == 0 || text[length - 1] != '\n')
		fputc('\n', stderr);
	if (level == RETRO_LOG_ERROR)
	{
		while (!errorText.empty() && errorText.back() == '\n')
			errorText.pop_back();
		errorText = text;
		while (!errorText.empty() && (errorText.back() == '\n' || errorText.back() == ' '))
			errorText.pop_back();
		// "[OpenAndParse] Failed to open..." is shown without the function's name.
		if (errorText.size() > 2 && errorText[0] == '[')
		{
			const size_t close = errorText.find("] ");
			if (close != std::string::npos)
				errorText.erase(0, close + 2);
		}
	}
}

void pushMessage(const std::string& text, double seconds, int progress, const std::string& title = "",
		const std::string& picture = "")
{
	std::lock_guard<std::mutex> lock(messageMutex);
	// A progress line replaces the one before it.
	if (progress >= 0)
		for (Message& message : messageList)
			if (message.progress >= 0)
			{
				message.text = text;
				message.until = now() + seconds;
				message.progress = progress;
				return;
			}
	for (Message& message : messageList)
		if (message.text == text)
		{
			message.until = now() + seconds;
			return;
		}
	messageList.push_back({ text, now() + seconds, progress, title, picture });
	if (messageList.size() > 6)
		messageList.erase(messageList.begin());
}

// ------------------------------------------------------------- environment

bool environment(unsigned command, void *data)
{
	switch (command)
	{
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*static_cast<unsigned *>(data) = 2;
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
		options::define(data);
		// The frontend shows categories.
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
		options::define(static_cast<const retro_core_options_v2_intl *>(data)->us);
		return true;
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
	{
		const retro_core_option_display *display = static_cast<const retro_core_option_display *>(data);
		if (display != nullptr && display->key != nullptr)
			options::setVisible(display->key, display->visible);
		return true;
	}
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
		updateDisplay = data != nullptr
				? static_cast<const retro_core_options_update_display_callback *>(data)->callback : nullptr;
		return true;
	case RETRO_ENVIRONMENT_GET_LANGUAGE:
		*static_cast<unsigned *>(data) = RETRO_LANGUAGE_ENGLISH;
		return true;
	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		static_cast<retro_log_callback *>(data)->log = logCallback;
		return true;
	case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
		*static_cast<unsigned *>(data) = 1;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE:
	{
		const retro_message *message = static_cast<const retro_message *>(data);
		if (message != nullptr && message->msg != nullptr)
		{
			if (strncmp(message->msg, "ERROR: ", 7) == 0)
				errorText = message->msg + 7;
			pushMessage(message->msg, std::clamp(message->frames / 60.0, 1.5, 10.0), -1);
		}
		return true;
	}
	case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
	{
		const retro_message_ext *message = static_cast<const retro_message_ext *>(data);
		if (message != nullptr && message->msg != nullptr)
			pushMessage(message->msg, std::clamp(message->duration / 1000.0, 1.0, 10.0),
					message->type == RETRO_MESSAGE_TYPE_PROGRESS ? std::max<int>(message->progress, 0) : -1);
		return true;
	}
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*static_cast<const char **>(data) = saveDir.c_str();
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*static_cast<const char **>(data) = systemDir.c_str();
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
		*static_cast<const char **>(data) = cacheDir.c_str();
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE:
	{
		retro_variable *variable = static_cast<retro_variable *>(data);
		if (variable == nullptr || variable->key == nullptr)
			return false;
		variable->value = options::get(variable->key);
		return variable->value != nullptr;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*static_cast<bool *>(data) = options::takeChanged();
		return true;
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		av = *static_cast<const retro_system_av_info *>(data);
		return true;
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
		av.geometry = *static_cast<const retro_game_geometry *>(data);
		return true;
	case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
		*static_cast<int *>(data) = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
		return true;
	case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
		*static_cast<retro_savestate_context *>(data) = RETRO_SAVESTATE_CONTEXT_NORMAL;
		return true;
	case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
		static_cast<retro_rumble_interface *>(data)->set_rumble_state = setRumbleState;
		return true;
	case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
		*static_cast<unsigned *>(data) = 1;
		return true;
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
		disk = *static_cast<const retro_disk_control_ext_callback *>(data);
		diskSet = true;
		return true;
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
	{
		const retro_disk_control_callback *basic = static_cast<const retro_disk_control_callback *>(data);
		disk = {};
		disk.set_eject_state = basic->set_eject_state;
		disk.get_eject_state = basic->get_eject_state;
		disk.get_image_index = basic->get_image_index;
		disk.set_image_index = basic->set_image_index;
		disk.get_num_images = basic->get_num_images;
		disk.replace_image_index = basic->replace_image_index;
		disk.add_image_index = basic->add_image_index;
		diskSet = true;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
		*static_cast<unsigned *>(data) = RETRO_HW_CONTEXT_VULKAN;
		return true;
	case RETRO_ENVIRONMENT_SET_HW_RENDER:
	{
		const retro_hw_render_callback *callback = static_cast<const retro_hw_render_callback *>(data);
		if (callback == nullptr || callback->context_type != RETRO_HW_CONTEXT_VULKAN)
			return false;
		hwRender = *callback;
		hwRenderSet = true;
		return true;
	}
	case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
		// Accepted and not used: the device exists already (display.cpp).
		return true;
	case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
		fillVulkanInterface();
		*static_cast<const retro_hw_render_interface **>(data)
				= reinterpret_cast<const retro_hw_render_interface *>(&vulkanInterface);
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		pixelFormat = *static_cast<const retro_pixel_format *>(data);
		return true;
	case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
	{
		retro_vfs_interface_info *info = static_cast<retro_vfs_interface_info *>(data);
		if (info == nullptr || info->required_interface_version > 3)
			return false;
		info->required_interface_version = 3;
		info->iface = static_cast<retro_vfs_interface *>(vfs::interface());
		return true;
	}
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*static_cast<bool *>(data) = true;
		return true;
	default:
		return false;
	}
}

// ------------------------------------------------------------ save states

std::string stateBase(const std::string& gamePath)
{
	std::string name = fileTitle(gamePath);
	for (char& c : name)
		if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|')
			c = '_';
	return rootDir + "data/states/" + name;
}

std::string statePathFor(const std::string& gamePath, int slot)
{
	return stateBase(gamePath) + (slot == ResumeSlot ? std::string(".resume") : format(".%d", slot)) + ".state";
}

// A state file: the mark, how large the emulator's state is unpacked, then
// (the second form) how large it is packed and how large what follows it is,
// the packed state, and RetroAchievements' progress at that moment.
constexpr char StateMagic[8] = { 'S', 'W', 'P', 'S', '5', 'S', 'T', '1' };
constexpr char StateMagic2[8] = { 'S', 'W', 'P', 'S', '5', 'S', 'T', '2' };

std::string timeText(time_t t)
{
	char text[64];
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &tm);
	return text;
}

void applyControllerTypes()
{
	static const unsigned devices[6] = {
		RETRO_DEVICE_JOYPAD,							// digital controller
		RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0),	// DualShock
		RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1),	// analog joystick
		RETRO_DEVICE_NONE,
		RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 2),	// neGcon
		RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0),	// GunCon
	};
	// The emulator's controllers 1 to 4 are the four pads, one for each
	// person logged in. Without a multitap it plugs in the first two; with
	// one in port 1 all four are on it (1A to 1D); with one in port 2 the
	// first is in port 1 and the others on the multitap. The emulator has
	// four more, for a second multitap: nobody holds those.
	for (unsigned port = 0; port < 4; port++)
	{
		// In netplay both consoles plug in what the hosting one has.
		const int type = port < 2 && netController[port] >= 0 ? netController[port] : options::frontend().controller[port];
		retro_set_controller_port_device(port, devices[std::clamp(type, 0, 5)]);
	}
	for (unsigned port = 4; port < 8; port++)
		retro_set_controller_port_device(port, RETRO_DEVICE_NONE);
}

// The serial of each game that ran, by its path: a game's own options are
// then known before it starts the next time (<root>data/game-ids.txt).
// Read once; the library asks for every game it shows.
std::map<std::string, std::string>& serialsByPath()
{
	static std::map<std::string, std::string> known;
	static bool loaded;
	if (!loaded)
	{
		loaded = true;
		FILE *f = fopen((rootDir + "data/game-ids.txt").c_str(), "r");
		if (f != nullptr)
		{
			char line[2048];
			while (fgets(line, sizeof(line), f) != nullptr)
			{
				char *tab = strchr(line, '\t');
				if (tab == nullptr)
					continue;
				*tab = 0;
				known[trim(tab + 1)] = line;
			}
			fclose(f);
		}
	}
	return known;
}

std::string lookupSerial(const std::string& path)
{
	const auto& known = serialsByPath();
	const auto it = known.find(path);
	return it == known.end() ? "" : it->second;
}

void rememberSerial(const std::string& path, const std::string& serial)
{
	if (path.empty() || serial.empty() || lookupSerial(path) == serial)
		return;
	serialsByPath()[path] = serial;
	const std::string file = rootDir + "data/game-ids.txt";
	FILE *f = fopen(file.c_str(), "a");
	if (f == nullptr)
		return;
	fprintf(f, "%s\t%s\n", serial.c_str(), path.c_str());
	fclose(f);
	chmod(file.c_str(), 0666);
}

// A libretro frontend unloads the core between games, and the core counts on
// it: its list of discs, for one, is only emptied in retro_deinit, so the next
// game would list the last one's discs before its own.
void reinitCore()
{
	retro_deinit();
	retro_set_environment(environment);
	retro_init();
}

// ---------------------------------------------------------------- rewinding

constexpr int rewindIntervals[3] = { 3, 6, 12 };
constexpr size_t rewindBudgets[4] = { 128u << 20, 256u << 20, 512u << 20, 1024u << 20 };

int rewindInterval()
{
	return rewindIntervals[std::clamp(options::frontend().rewindDetail, 0, 2)];
}

bool rewindAllowed()
{
	return options::frontend().rewind && !netOn && !restricted();
}

// After an emulated frame: every few, the state is kept to step back to.
void keepForRewind()
{
	if (!rewindAllowed())
	{
		if (rewind::count() != 0)
			rewind::clear();
		return;
	}
	if (++sinceKept < rewindInterval())
		return;
	sinceKept = 0;
	rewind::configure(rewindBudgets[std::clamp(options::frontend().rewindMemory, 0, 3)]);
	const size_t size = retro_serialize_size();
	stateBuffer.resize(size);
	if (retro_serialize(stateBuffer.data(), size))
		rewind::push(stateBuffer);
}

// -------------------------------------------------------- memory card copies

// When a game closes: each card that is not what its newest copy holds gets a
// new copy, <saves>/backups/<card>/<date>-<time>.mcd, and of each card the
// newest few are kept (the setting).
void backUpCards()
{
	const int keep = options::frontend().cardBackups;
	if (keep <= 0)
		return;
	DIR *dir = opendir(saveDir.c_str());
	if (dir == nullptr)
		return;
	std::vector<std::string> cards;
	while (const dirent *entry = readdir(dir))
		if (extension(entry->d_name) == ".mcd")
			cards.push_back(entry->d_name);
	closedir(dir);
	for (const std::string& card : cards)
	{
		std::vector<uint8_t> now_, before;
		if (!readFile(saveDir + "/" + card, now_) || now_.size() != 128 * 1024)
			continue;
		const std::string folder = saveDir + "/backups/" + fileTitle(card);
		std::vector<std::string> copies;
		if (DIR *list = opendir(folder.c_str()))
		{
			while (const dirent *entry = readdir(list))
				if (extension(entry->d_name) == ".mcd")
					copies.push_back(entry->d_name);
			closedir(list);
		}
		std::sort(copies.begin(), copies.end());
		if (!copies.empty() && readFile(folder + "/" + copies.back(), before) && before == now_)
			continue;
		// A card nothing was ever saved to is not worth a copy.
		memcard::Card parsed;
		if (copies.empty() && (!memcard::parse(now_, parsed) || parsed.saves.empty()))
			continue;
		makeDir(folder);
		char stamp[32];
		const time_t t = time(nullptr) + platform::localTimeOffset();
		struct tm tm;
		gmtime_r(&t, &tm);
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
		if (writeFile(folder + "/" + stamp + ".mcd", now_.data(), now_.size()))
		{
			copies.push_back(std::string(stamp) + ".mcd");
			diag::mark("cards: %s changed: a copy was kept (%d in all)", card.c_str(), (int)copies.size());
		}
		while ((int)copies.size() > keep)
		{
			unlink((folder + "/" + copies.front()).c_str());
			copies.erase(copies.begin());
		}
	}
}

// ------------------------------------------------------------------ netplay

// The emulator's settings both consoles must have the same for the two to
// stay in step: what changes how the PlayStation behaves, not how it looks.
const char *const netKeys[] = {
	"swanstation_BIOS_PatchFastBoot", "swanstation_CDROM_LoadImagePatches", "swanstation_CDROM_ReadSpeedup",
	"swanstation_CDROM_SeekSpeedup", "swanstation_CDROM_RegionCheck", "swanstation_CDROM_MuteCDAudio",
	"swanstation_CPU_ExecutionMode", "swanstation_CPU_Overclock", "swanstation_CPU_RecompilerICache",
	"swanstation_Console_Enable8MBRAM", "swanstation_Console_Region", "swanstation_GPU_ForceNTSCTimings",
	"swanstation_GPU_DisableInterlacing", "swanstation_GPU_WidescreenHack", "swanstation_Hacks_OldMDECRoutines",
	"swanstation_Main_ApplyGameSettings", "swanstation_Controller1_ForceAnalog", "swanstation_Controller2_ForceAnalog",
	"swanstation_Controller1_AnalogDPadInDigitalMode", "swanstation_Controller2_AnalogDPadInDigitalMode",
	"swanstation_Controller_AnalogCombo",
};
// And what a session sets on both: no memory cards (each console has its own,
// and they would differ), no multitap, no run-ahead.
const char *const netForced[][2] = {
	{ "swanstation_MemoryCards_Card1Type", "None" }, { "swanstation_MemoryCards_Card2Type", "None" },
	{ "swanstation_ControllerPorts_MultitapMode", "Disabled" }, { "swanstation_Main_RunaheadFrameCount", "0" },
};

std::string netSettingsText()
{
	std::string text;
	for (const char *key : netKeys)
		if (const char *value = options::get(key))
			text += std::string(key) + "=" + value + "\n";
	for (int player = 0; player < 2; player++)
		text += format("controller%d=%d\n", player + 1, options::frontend().controller[player]);
	return text;
}

// What a session needs of this console, the host's settings (`theirs`) over
// its own on the joining side.
void netApply(const std::string& theirs)
{
	// A cheat on one console and not the other would part them at once. (This
	// also lets go of the settings a patch held, before the session's are set.)
	cheats::apply();
	for (const auto& forced : netForced)
		options::setOverride(forced[0], forced[1]);
	size_t at = 0;
	while (at < theirs.size())
	{
		const size_t end = std::min(theirs.find('\n', at), theirs.size());
		const std::string line = theirs.substr(at, end - at);
		at = end + 1;
		const size_t equals = line.find('=');
		if (equals == std::string::npos)
			continue;
		const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
		if (key == "controller1" || key == "controller2")
			netController[key == "controller1" ? 0 : 1] = std::clamp(atoi(value.c_str()), 0, 5);
		else
			for (const char *known : netKeys)
				if (key == known)
					options::setOverride(key, value);
	}
	applyControllerTypes();
}

netplay::Hooks netHooks()
{
	netplay::Hooks hooks;
	hooks.saveState = [](std::vector<uint8_t>& out) {
		const size_t size = retro_serialize_size();
		out.resize(size);
		return retro_serialize(out.data(), size);
	};
	hooks.loadState = [](const uint8_t *data, size_t size) {
		const bool ok = retro_unserialize(data, size);
		audio::clear();
		return ok;
	};
	hooks.checksum = []() -> uint32_t {
		uint32_t size = 0;
		const uint8_t *ram = coreRam(size);
		return ram != nullptr ? (uint32_t)mz_crc32(MZ_CRC32_INIT, ram, size) : 0;
	};
	return hooks;
}

netplay::Session netSession()
{
	netplay::Session session;
	session.game = current.serial;
	uint32_t size = 0;
	const uint8_t *bios = coreBios(size);
	session.bios = format("%08x", (unsigned)mz_crc32(MZ_CRC32_INIT, bios, size));
	session.settings = netSettingsText();
	session.delay = std::clamp(options::frontend().netplayDelay, 1, 10);
	return session;
}

void netBegin()
{
	netOn = true;
	netSettingsTaken = false;
	netInputs[0] = netInputs[1] = netplay::Input();
	ffOn = rewindOn = false;
	audio::setMuted(false);
	rewind::clear();
}

// The session is over, by either side or by a fault: this console is its own again.
void netEnd()
{
	if (!netOn)
		return;
	const std::string why = netplay::error();
	diag::mark("netplay: over after %llu frames in step, brought back in step %u time(s)",
			(unsigned long long)netplay::frame(), netplay::resyncs());
	netplay::stop();
	netOn = false;
	netController[0] = netController[1] = -1;
	options::clearOverrides();
	if (isRunning)
	{
		// The cheats that were on are on again.
		applyControllerTypes();
		cheats::apply();
	}
	addMessage(why.empty() ? std::string("Netplay has ended.") : "Netplay has ended. " + why, 6.0);
	diag::mark("netplay: ended: %s", why.c_str());
}

// ----------------------------------------------------------- RetroAchievements

std::string keptUser, keptToken;
bool achievementsStarted;

void loadKeptLogin()
{
	keptUser.clear();
	keptToken.clear();
	FILE *f = fopen((rootDir + "data/retroachievements.cfg").c_str(), "r");
	if (f == nullptr)
		return;
	char line[512];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		const std::string text = trim(line);
		const size_t equals = text.find('=');
		if (equals == std::string::npos)
			continue;
		const std::string key = trim(text.substr(0, equals)), value = trim(text.substr(equals + 1));
		if (key == "user")
			keptUser = value;
		else if (key == "token")
			keptToken = value;
	}
	fclose(f);
}

void saveKeptLogin()
{
	const std::string path = rootDir + "data/retroachievements.cfg";
	if (keptUser.empty() || keptToken.empty())
	{
		unlink(path.c_str());
		return;
	}
	const std::string text = "# PSSwanStation - the RetroAchievements account this console is signed in to.\n"
			"# The token is the key the server gave when the password was typed; the\n"
			"# password itself is not kept. Delete this file to sign out.\n"
			"user = " + keptUser + "\ntoken = " + keptToken + "\n";
	writeFile(path, text.data(), text.size());
}

// The image in the tray: for a game on several discs, the disc itself.
std::string trayPath()
{
	if (diskSet && disk.get_image_path != nullptr && disk.get_num_images != nullptr && disk.get_num_images() > 0)
	{
		char path[4096] = "";
		if (disk.get_image_path(disk.get_image_index(), path, sizeof(path)) && path[0] != 0)
			return path;
	}
	return current.path;
}

void achievementEvents()
{
	for (const achievements::Event& event : achievements::takeEvents())
	{
		using E = achievements::Event;
		switch (event.kind)
		{
		case E::LoggedIn:
			keptUser = event.title;
			keptToken = achievements::token();
			saveKeptLogin();
			addNotice("RetroAchievements", "Signed in as " + event.title + (event.text.empty() ? "" : "  \xc2\xb7  " + event.text));
			break;
		case E::LoginFailed:
			addNotice("RetroAchievements", "Signing in failed: " + event.text, "", 7.0);
			break;
		case E::GameLoaded:
			addNotice(event.title, event.text + (achievements::hardcore() ? "  \xc2\xb7  hardcore" : ""));
			break;
		case E::GameUnknown:
			addMessage("RetroAchievements has no achievements for this disc.", 4.0);
			break;
		case E::Unlocked:
			addNotice(event.title, event.text + (event.points > 0 ? format("  \xc2\xb7  %d points", event.points) : ""),
					event.badge, 6.0);
			sound::unlock();
			break;
		case E::Progress:
			addMessage(event.title + ":  " + event.text, 2.5);
			break;
		case E::Challenge:
			addMessage("Challenge:  " + event.title, 3.0);
			break;
		case E::LeaderboardStarted:
			addMessage("Leaderboard attempt started:  " + event.title, 3.0);
			break;
		case E::LeaderboardFailed:
			addMessage("Leaderboard attempt ended:  " + event.title, 3.0);
			break;
		case E::LeaderboardSubmitted:
			addNotice(event.title, event.text.empty() ? std::string("Score sent") : event.text);
			break;
		case E::Completed:
			addNotice(event.title, event.text, "", 8.0);
			sound::unlock();
			break;
		case E::ServerError:
			addMessage("RetroAchievements: " + event.text, 5.0);
			break;
		case E::Disconnected:
			addMessage("RetroAchievements cannot be reached: what you earn is sent when it can be.", 5.0);
			break;
		case E::Reconnected:
			addMessage("RetroAchievements is reachable again.", 3.0);
			break;
		}
	}
}

void achievementsInit()
{
	achievements::Hooks hooks;
	hooks.readMemory = [](uint32_t address, uint8_t *buffer, uint32_t bytes) -> uint32_t {
		if (!isRunning)
			return 0;
		uint32_t ramSize = 0;
		const uint8_t *ram = coreRam(ramSize);
		// The main RAM is 2 MiB to RetroAchievements, whatever the emulator holds.
		const uint32_t mainSize = std::min<uint32_t>(ramSize, 0x200000);
		if (ram != nullptr && address < mainSize)
		{
			const uint32_t count = std::min(bytes, mainSize - address);
			memcpy(buffer, ram + address, count);
			return count;
		}
		if (address >= 0x200000 && address < 0x200400)
		{
			const uint32_t count = std::min(bytes, 0x200400 - address);
			memcpy(buffer, coreScratchpad() + (address - 0x200000), count);
			return count;
		}
		return 0;
	};
	hooks.openDisc = [](const std::string& path) { return discOpen(path); };
	hooks.readSector = [](void *disc, uint32_t sector, uint8_t *out) { return discRead(disc, sector, out); };
	hooks.closeDisc = [](void *disc) { discClose(disc); };
	hooks.httpGet = [](const std::string& url, std::vector<uint8_t>& out, unsigned seconds) {
		return platform::httpGet(url, out, seconds);
	};
	hooks.reset = [] { reset(); };
	achievements::init(hooks, rootDir + "data/cache/");
	achievementsStarted = true;
	loadKeptLogin();
}

} // namespace

// ------------------------------------------------------------------- public

bool init()
{
	saveDir = rootDir + "data/saves";
	systemDir = rootDir + "bios";
	cacheDir = rootDir + "data/cache";
	nameTextureFolders();
	options::loadGlobal();
	retro_set_environment(environment);
	retro_set_video_refresh(videoRefresh);
	retro_set_audio_sample(audioSample);
	retro_set_audio_sample_batch(audioBatch);
	retro_set_input_poll(inputPoll);
	retro_set_input_state(inputState);
	diag::mark("core: retro_init");
	retro_init();
	retro_system_info info{};
	retro_get_system_info(&info);
	diag::mark("core: %s %s", info.library_name != nullptr ? info.library_name : "?",
			info.library_version != nullptr ? info.library_version : "?");
	coreReady = true;
	achievementsInit();
	achievementsApply();
	return true;
}

void shutdown()
{
	stop();
	if (achievementsStarted)
		achievements::shutdown();
	achievementsStarted = false;
	if (coreReady)
		retro_deinit();
	coreReady = false;
}

bool running()
{
	return isRunning;
}

bool paused()
{
	return isPaused;
}

void setPaused(bool paused)
{
	isPaused = paused;
	audio::setPaused(paused || !isRunning);
	if (netOn)
		netplay::setPaused(paused);
	if (paused)
	{
		// What was held when the menu opened is not held when it closes.
		ffOn = rewindOn = false;
		audio::setMuted(false);
		clockNext = 0;
		for (int i = 0; i < platform::MaxPads; i++)
			platform::padRumble(i, 0, 0);
	}
}

const GameInfo& game()
{
	return current;
}

std::string lastError()
{
	return errorText;
}

std::string knownSerial(const std::string& gamePath)
{
	return lookupSerial(gamePath);
}

namespace
{
// The serial of the disc in the tray. For a playlist the emulator keeps the
// first disc's as the running game's, whichever disc is in.
std::string traySerial()
{
	if (diskSet && disk.get_num_images != nullptr && disk.get_num_images() > 1 && disk.get_image_path != nullptr)
	{
		char path[4096] = "";
		if (disk.get_image_path(disk.get_image_index(), path, sizeof(path)) && path[0] != 0)
		{
			const std::string serial = readSerial(path);
			if (!serial.empty())
				return serial;
		}
	}
	return System::GetRunningCode();
}
}

bool start(const std::string& path, int stateSlot, int disc, const std::string& serialHint)
{
	if (!coreReady)
		return false;
	stop();
	errorText.clear();
	diag::mark("game: starting %s", path.empty() ? "(the BIOS)" : path.c_str());
	const double startBegan = now();

	current = GameInfo();
	current.path = path;
	current.title = path.empty() ? "PlayStation BIOS" : fileTitle(path);
	// A game's own options, when it ran before, are in place before it boots.
	options::loadGame(!serialHint.empty() ? serialHint : lookupSerial(path));
	applyControllerTypes();
	// The disc in the tray at the start, of a playlist's.
	if (disc > 0 && diskSet && disk.set_initial_image != nullptr)
		disk.set_initial_image((unsigned)disc, path.c_str());

	hwRenderSet = false;
	hwImageValid = false;
	frameWidth = frameHeight = 0;
	nameTextureFolders();
	retro_game_info info{};
	info.path = path.c_str();
	bool loaded = retro_load_game(path.empty() ? nullptr : &info);
	// The core starts its BIOS when a disc image cannot be opened, as a console
	// with an empty tray would; a game that was asked for and is not there is
	// a failure to say so about.
	const std::string ext = extension(path);
	const bool program = ext == ".exe" || ext == ".psexe" || ext == ".psf" || ext == ".minipsf";
	if (loaded && !path.empty() && !program && !System::HasMedia())
	{
		// The renderer it asked for was never made; told so, the core asks
		// again for the next game.
		if (hwRenderSet && hwRender.context_destroy != nullptr)
			hwRender.context_destroy();
		retro_unload_game();
		loaded = false;
		if (errorText.empty())
			errorText = "The disc image could not be opened.";
	}
	if (!loaded)
	{
		diag::mark("game: the emulator refused it: %s", errorText.c_str());
		if (errorText.empty())
			errorText = "The emulator could not start this game.";
		const std::string why = errorText;
		reinitCore();
		errorText = why;
		hwRenderSet = false;
		options::loadGame("");
		smb::releaseImages();
		current = GameInfo();
		return false;
	}
	isRunning = true;
	if (hwRenderSet && hwRender.context_reset != nullptr)
	{
		diag::mark("game: hardware renderer");
		hwRender.context_reset();
	}
	retro_get_system_av_info(&av);
	firstDiscSerial = System::GetRunningCode();
	current.serial = traySerial();
	diag::mark("game: running, serial %s, disc %d of %d, %.2f fps, %u x %u; the start took %.1f s", current.serial.c_str(),
			discIndex() + 1, std::max(discCount(), 1), av.timing.fps, av.geometry.base_width, av.geometry.base_height,
			now() - startBegan);
	rememberSerial(path, current.serial);
	if (!path.empty())
	{
		history::begin(path);
		if (discCount() > 1)
			history::setDisc(path, discIndex());
	}
	options::loadGame(current.serial);
	cheats::loadFor(current.serial, firstDiscSerial);
	cheats::apply();
	pacing = 1.0;
	ranThisSecond = 0;
	secondStarted = now();
	fpsMeasured = 0;
	ffOn = rewindOn = rewindDry = false;
	sinceKept = 0;
	emulatedFrames = 0;
	clockNext = 0;
	rewind::clear();
	for (Aim& aim : aims)
		aim = Aim();
	display::forgetAmbient();
	display::forgetGenerated();
	display::forgetPicture();
	cadence = Cadence();
	audio::setMuted(false);
	audio::clear();
	setPaused(false);
	// The slot the shortcuts use: the one saved to last, or the first.
	lastSlot = 0;
	{
		time_t newest = 0;
		for (int slot = 0; slot < StateSlots; slot++)
		{
			struct stat st;
			if (stat(statePath(slot).c_str(), &st) == 0 && st.st_mtime > newest)
			{
				newest = st.st_mtime;
				lastSlot = slot;
			}
		}
	}
	if (options::frontend().achievements && !path.empty())
		achievements::gameStarted(trayPath());
	if (stateSlot != -1 && stateExists(stateSlot))
		loadState(stateSlot);
	return true;
}

void stop()
{
	if (!isRunning)
		return;
	diag::mark("game: stopping");
	netEnd();
	if (options::frontend().autoSaveOnExit && !current.path.empty() && !restricted())
		saveState(ResumeSlot);
	achievements::gameStopped();
	ffOn = rewindOn = false;
	audio::setMuted(false);
	rewind::clear();
	if (!current.path.empty() && discCount() > 1)
		history::setDisc(current.path, discIndex());
	history::end();
	setPaused(true);
	// As a libretro frontend does: the hardware context goes first (the
	// emulator falls back to its software renderer and lets go of the
	// device's objects), then the game.
	if (hwRenderSet && hwRender.context_destroy != nullptr)
		hwRender.context_destroy();
	hwRenderSet = false;
	retro_unload_game();
	if (g_vulkan_context)
		g_vulkan_context->ExecuteCommandBuffer(true);
	reinitCore();
	// The emulator has written its cards by now.
	backUpCards();
	isRunning = false;
	hwImageValid = false;
	frameWidth = frameHeight = 0;
	display::releaseWrapped();
	display::destroyTexture(softwareTexture);
	softwareTexture = nullptr;
	audio::clear();
	cheats::unload();
	options::loadGame("");
	smb::releaseImages();
	current = GameInfo();
	{
		std::lock_guard<std::mutex> lock(messageMutex);
		messageList.clear();
	}
	diag::mark("game: stopped");
	// What the game wrote (its memory card, a state) goes to the share.
	netfiles::gameClosed();
}

void reset()
{
	if (!isRunning)
		return;
	retro_reset();
	if (!restricted() && !netOn)
		cheats::apply();
	audio::clear();
	rewind::clear();
	achievements::gameReset();
}

namespace
{
// One emulated frame, and what goes with each.
void runOne(bool keep)
{
	moveAims();
#if defined(SWANSTATION_HOST)
	// A test reads what the game is given.
	if (getenv("SWANSTATION_INPUT_LOG") != nullptr && emulatedFrames % 30 == 0)
	{
		const netplay::Input in = localInput(0);
		diag::mark("input: frame %llu buttons %04x left %d %d aim %.2f %.2f trigger %d analog R2 %d",
				(unsigned long long)emulatedFrames, in.buttons, in.lx, in.ly, aims[0].x, aims[0].y,
				inputState(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER),
				inputState(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_R2));
	}
#endif
	retro_run();
	emulatedFrames++;
	{
		// Was that a new picture?
		const uint32_t changes = coreDisplayChanges();
		const bool changed = changes != cadence.changesSeen;
		cadence.changesSeen = changes;
		if (changed)
			cadence.changedAtFrame = emulatedFrames;
		const uint32_t draws = coreDrawCommands();
		const bool drew = draws != cadence.drawsSeen;
		cadence.drawsSeen = draws;
		const uint32_t blocks = coreVideoBlocks();
		if (blocks != cadence.blocksSeen)
		{
			cadence.blocksSeen = blocks;
			cadence.videoAtFrame = emulatedFrames;
		}
		// A game that moved its display's start within the last second is
		// counted by that; another by whether it drew.
		cadence.counted = cadence.changedAtFrame != ~0ull && emulatedFrames - cadence.changedAtFrame < 60;
		if (changed || (!cadence.counted && drew))
			cadence.pictureNow = true;
	}
	if (keep)
		keepForRewind();
	if (!netOn)
		achievements::frame();
}

// Netplay's part of a refresh: as many frames as are due and both players'
// buttons have come for.
int runNetplay(int due)
{
	if (!netplay::active())
	{
		netEnd();
		return 0;
	}
	// The joining side takes the host's settings before the host's state.
	if (!netSettingsTaken && netplay::state() >= netplay::State::Syncing)
	{
		netSettingsTaken = true;
		netApply(netplay::hosting() ? std::string() : netplay::hostSettings());
	}
	int ran = 0;
	do
	{
		netplay::Input inputs[netplay::Players];
		if (netplay::step(localInput(0), inputs) != netplay::Step::Run)
			break;
		netInputs[0] = inputs[0];
		netInputs[1] = inputs[1];
		runOne(false);
		netplay::ran();
		ran++;
		// Now and then the log says how it goes.
		if (netplay::frame() % 1800 == 0)
			diag::mark("netplay: frame %llu, there and back in %d ms, brought back in step %u time(s)",
					(unsigned long long)netplay::frame(), netplay::ping(), netplay::resyncs());
	} while (ran < due);
	return ran;
}
}

void runFrame()
{
	blackNow = false;
	// A game that stands still shows its latest picture.
	genFresh = false;
	genPhase = 1.f;
	genOn = isRunning && options::frontend().frameGeneration != 0;
	cadence.pictureNow = false;
	if (!isRunning || isPaused)
		return;
	const options::Frontend& settings = options::frontend();
	const double displayHz = display::refreshRate();
	const double coreHz = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
	double ratio = coreHz / displayHz;
	// By the display: a game whose rate is the display's, or half of it, gets
	// exactly one frame a refresh, or one every second refresh.
	const bool even = settings.pacing == 0 && std::fabs(ratio - 0.5) < 0.005;
	if (settings.pacing == 0 && std::fabs(ratio - 1.0) < 0.01)
		ratio = 1.0;
	else if (even)
		ratio = 0.5;
	if (rewindOn && !rewindAllowed())
		rewindOn = false;

	int ran = 0;
	bool anyRate = false;
	if (netOn)
	{
		pacing += ratio;
		const int due = std::min((int)pacing, 2);
		if (due > 0)
			ran = runNetplay(due);
		pacing = std::min(pacing - ran, 2.0);
	}
	else if (rewindOn)
	{
		// Back through the kept states, twice as fast as they were played.
		rewindDue -= 1.0;
		if (rewindDue <= 0)
		{
			rewindDue += std::max(rewindInterval() * 0.5 / ratio, 1.0);
			if (rewind::pop(stateBuffer) && retro_unserialize(stateBuffer.data(), stateBuffer.size()))
			{
				// A frame from there, for its picture.
				runOne(false);
				ran = 1;
				rewindDry = false;
			}
			else if (!rewindDry)
			{
				rewindDry = true;
				addMessage("That is as far back as the rewind memory goes.");
			}
		}
		pacing = 1.0;
	}
	else if (ffOn)
	{
		static const double speeds[4] = { 2, 3, 4, 8 };
		if (settings.fastForward >= 4)
		{
			// As fast as it goes: frames for most of a refresh's time.
			const double until = now() + 0.75 / display::outputRefreshRate();
			do
			{
				runOne(true);
				ran++;
			} while (ran < 40 && now() < until);
		}
		else
		{
			pacing += ratio * speeds[std::clamp(settings.fastForward, 0, 3)];
			while (pacing >= 1.0 && ran < 16)
			{
				pacing -= 1.0;
				runOne(true);
				ran++;
			}
		}
		if (pacing > 2.0)
			pacing = 0;
	}
	else if (settings.pacing == 2 && settings.frameGeneration != 0)
	{
		// By the clock, with frame generation: the screen is not waited for a
		// frame of the game, it refreshes at its own rate and each refresh
		// shows where the game is at that moment (any rate, with a variable
		// refresh rate or without).
		const double period = 1.0 / coreHz;
		const double t = now();
		if (clockNext == 0 || t - clockNext > 0.1)
			clockNext = t;
		while (clockNext <= t && ran < 2)
		{
			runOne(true);
			ran++;
			clockNext += period;
		}
		anyRate = true;
	}
	else if (settings.pacing == 2)
	{
		// By the clock: the frame runs when it is due, and what is drawn then
		// goes to the display at once.
		const double period = 1.0 / coreHz;
		double t = now();
		if (clockNext == 0 || t - clockNext > 0.1)
			clockNext = t;
		if (clockNext > t)
		{
			// Sleeping is coarse: the last third of a millisecond is waited out.
			const double left = clockNext - t;
			if (left > 0.0006)
				usleep((useconds_t)((left - 0.0003) * 1e6));
			while ((t = now()) < clockNext) {}
		}
		const int due = std::min(1 + (int)((t - clockNext) / period), 2);
		for (int i = 0; i < due; i++)
			runOne(true);
		ran = due;
		clockNext += period * due;
	}
	else
	{
		pacing += ratio;
		while (pacing >= 1.0 && ran < 2)
		{
			pacing -= 1.0;
			runOne(true);
			ran++;
		}
		if (pacing > 2.0)
			pacing = 0;
		// Black frame insertion: at twice the game's rate, the refresh between
		// two of its frames shows nothing, as a picture tube does.
		blackNow = even && settings.blackFrames && settings.frameGeneration == 0 && ran == 0;
	}
	// The game's pictures, counted in refreshes and in its own frames.
	cadence.refreshes++;
	const double clock = now();
	if (cadence.pictureNow)
	{
		cadence.gapRefreshes = (int)std::min<uint64_t>(cadence.refreshes - cadence.pictureAtRefresh, 99);
		cadence.gapFrames = (int)std::min<uint64_t>(emulatedFrames - cadence.pictureAtFrame, 99);
		cadence.pictureAtRefresh = cadence.refreshes;
		cadence.pictureAtFrame = emulatedFrames;
		cadence.sincePicture = 0;
		cadence.gaps[cadence.gapIndex++ % 4] = cadence.gapRefreshes;
		cadence.gapSeconds = std::clamp(clock - cadence.pictureAtTime, 0.0, 0.5);
		cadence.pictureAtTime = clock;
	}
	// Frame generation: which picture this refresh is to show. Ahead (fgMode
	// 1): from the game's latest picture on, along its movement, instead of
	// between the last two, which waits for the latest.
	const bool ahead = settings.fgMode == 1;
	const bool capped = settings.fgCap == 1 && displayHz > 100.0;
	const bool video = cadence.videoAtFrame != ~0ull && emulatedFrames - cadence.videoAtFrame < 30;
	if (settings.frameGeneration == 0)
		genOn = false;
	else if (ffOn || rewindOn || ran > 1 || (ratio > 1.0 && !anyRate))
	{
		// Run fast or backwards, or catching up, or a game faster than the
		// display: the frames as they are.
		genOn = false;
		if (ran > 0)
			display::forgetGenerated();
	}
	else if (video && settings.fgVideos == 1)
	{
		// A video plays, and is to be left as it is.
		genOn = false;
		if (ran > 0)
			display::forgetGenerated();
	}
	else if (anyRate)
	{
		// By the clock: where the screen is now between the game's last two
		// pictures (or past the latest), from the time each came.
		genFresh = cadence.pictureNow;
		const double gap = cadence.gapSeconds > 0.004 ? cadence.gapSeconds : (double)cadence.gapFrames / coreHz;
		const float part = (float)std::clamp((clock - cadence.pictureAtTime) / gap, 0.0, 1.0);
		genPhase = ahead ? 1.f + std::min(part, 0.95f) : part;
		if (!ahead && cadence.pictureNow)
			genPhase = 0.f;
	}
	else if (cadence.gapFrames <= 1)
	{
		// A new picture every frame. At half the display's rate the picture
		// half way to each is shown the refresh it is made in, and the frame
		// itself the refresh after (ahead: the frame first, then half a frame
		// past it). At the display's own rate there is no room between two
		// frames, and nothing is made. At any other rate (a PAL game on a
		// 60 Hz screen) the screen is a frame behind the game and shows where
		// the game was at that moment, between two frames nearly always.
		genFresh = ran > 0;
		if (even)
		{
			if (capped)
				genOn = false;
			else if (ahead)
				genPhase = ran > 0 ? 1.f : 1.5f;
			else
				genPhase = ran > 0 ? 0.5f : 1.f;
		}
		else if (settings.pacing == 2 || ratio == 1.0)
			genOn = false;
		else
			genPhase = ahead ? 1.f + (float)std::clamp(pacing, 0.0, 0.95) : (float)std::clamp(pacing, 0.0, 1.0);
	}
	else
	{
		// A new picture every second, third... frame, as most PlayStation
		// games draw: the refreshes until the next one is due show even steps
		// from the picture before to this one, the last of them this one
		// itself. How many is the average of the last four gaps, so that a
		// game whose pictures come unevenly (two refreshes, then three) moves
		// evenly too. Capped at 60: the steps go two refreshes at a time.
		genFresh = cadence.pictureNow;
		double steps = 0;
		for (int gap : cadence.gaps)
			steps += gap;
		steps = std::clamp(steps / 4.0, 1.0, 8.0);
		if (cadence.gapRefreshes > 8)
			steps = 1;
		int since = cadence.sincePicture;
		if (capped)
			since = since / 2 * 2 + 1;
		const float part = std::min((float)((since + 1) / steps), 1.f);
		genPhase = ahead ? 1.f + std::min((float)(since / steps), 0.95f) : part;
	}
	// Taking back frame generation's delay: one frame of run-ahead while it is
	// on, unless the user set some of their own.
	{
		const char *own = options::own("swanstation_Main_RunaheadFrameCount");
		// (By the settings, not by this refresh: a setting that changed every
		// refresh would set the emulator up anew every refresh.)
		const bool take = settings.frameGeneration != 0 && settings.fgRunAhead && !netOn && (own == nullptr || !strcmp(own, "0"));
		options::hold("swanstation_Main_RunaheadFrameCount", take ? "1" : "");
	}
	// The pictures the screen showed this second: the game's and the made ones.
	if (genOn ? (genFresh || std::fabs(genPhase - lastShownPhase) > 0.001f) : cadence.pictureNow)
		shownThisSecond++;
	lastShownPhase = genPhase;
	cadence.sincePicture++;
#if defined(SWANSTATION_HOST)
	// A test reads how the frames fell.
	if (getenv("SWANSTATION_PACING_LOG") != nullptr)
		diag::mark("pacing: display frame %llu ran %d ratio %.4f picture %d every %d frames (%d refreshes) phase %.2f generation %d",
				(unsigned long long)display::frameCount(), ran, ratio, (int)cadence.pictureNow, cadence.gapFrames,
				cadence.gapRefreshes, genPhase, (int)genOn);
#endif
	ranThisSecond += ran;
	const double t = now();
	if (t - secondStarted >= 1.0)
	{
		fpsMeasured = (float)(ranThisSecond / (t - secondStarted));
		shownMeasured = (float)(shownThisSecond / (t - secondStarted));
		shownThisSecond = 0;
		ranThisSecond = 0;
		secondStarted = t;
	}
	// Options that hide or show others: the core says which after a change.
	static unsigned seen;
	if (updateDisplay != nullptr && seen != options::generation())
	{
		seen = options::generation();
		updateDisplay();
	}
	if (options::frontend().rumble)
		for (int i = 0; i < platform::MaxPads; i++)
			platform::padRumble(i, rumble[i][0], rumble[i][1]);
}

void setFastForward(bool on)
{
	on = on && isRunning && !netOn;
	if (on == ffOn)
		return;
	ffOn = on;
	audio::setMuted(ffOn || rewindOn);
	pacing = 1.0;
	clockNext = 0;
}

bool fastForward()
{
	return ffOn;
}

void setRewinding(bool on)
{
	on = on && isRunning && rewindAllowed();
	if (on == rewindOn)
		return;
	rewindOn = on;
	rewindDue = 0;
	rewindDry = false;
	audio::setMuted(ffOn || rewindOn);
	pacing = 1.0;
	clockNext = 0;
	if (!on)
	{
		// Play goes on from where the rewinding stopped.
		sinceKept = 0;
		cheats::apply();
	}
}

bool rewinding()
{
	return rewindOn;
}

float rewindSeconds()
{
	const double fps = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
	return (float)(rewind::count() * rewindInterval() / fps);
}

void setInputBlocked(bool blocked)
{
	inputBlocked = blocked;
}

bool blackFrame()
{
	return blackNow;
}

bool generationPhase(float& phase, bool& fresh)
{
	phase = genPhase;
	fresh = genFresh;
	return genOn;
}

bool newPicture()
{
	return isRunning && cadence.pictureNow;
}

float pictureMs()
{
	const double pictures = picturesPerSecond();
	return pictures > 0.5 ? (float)(1000.0 / pictures) : 16.7f;
}

float shownPerSecond()
{
	return isRunning ? shownMeasured : 0.f;
}

double picturesPerSecond()
{
	if (!isRunning)
		return 0;
	return (av.timing.fps > 1.0 ? av.timing.fps : 60.0) / std::max(cadence.gapFrames, 1);
}

int nativeLines()
{
	if (!isRunning || frameHeight == 0)
		return 0;
	// By the scale the renderer really drew at (a changed setting takes a
	// frame or two to reach it).
	const int scale = frameIsHardware ? std::max((int)coreDrawResolutionScale(), 1) : 1;
	return (int)frameHeight / scale;
}

void *frameTexture()
{
	if (!isRunning || frameWidth == 0)
		return nullptr;
	if (frameIsHardware)
		return hwImageValid ? display::wrapView(hwImage.image_view, (int)hwImage.image_layout) : nullptr;
	return display::textureId(softwareTexture);
}

void frameSize(int& width, int& height, float& aspect)
{
	width = (int)frameWidth;
	height = (int)frameHeight;
	aspect = av.geometry.aspect_ratio > 0 ? av.geometry.aspect_ratio
			: (frameHeight != 0 ? (float)frameWidth / (float)frameHeight : 4.f / 3.f);
}

// The part of the software renderer's texture the picture fills.
void frameUv(float& u, float& v)
{
	if (frameIsHardware || softwareTexture == nullptr)
	{
		u = v = 1.f;
		return;
	}
	u = (float)frameWidth / (float)display::textureWidth(softwareTexture);
	v = (float)frameHeight / (float)display::textureHeight(softwareTexture);
}

double coreFps()
{
	return av.timing.fps;
}

float measuredFps()
{
	return fpsMeasured;
}

std::string statePath(int slot)
{
	return statePathFor(current.path, slot);
}

std::string stateThumbPath(int slot)
{
	return statePath(slot) + ".png";
}

std::string stateThumbPathFor(const std::string& gamePath, int slot)
{
	return statePathFor(gamePath, slot) + ".png";
}

int quickSlot()
{
	return std::clamp(lastSlot, 0, StateSlots - 1);
}

void setQuickSlot(int slot)
{
	lastSlot = std::clamp(slot, 0, StateSlots - 1);
}

bool restricted()
{
	return isRunning && options::frontend().achievements && achievements::hardcore()
			&& achievements::summary().gameLoaded;
}

bool capture(int maxHeight, std::vector<uint8_t>& rgba, int& width, int& height)
{
	void *texture = frameTexture();
	if (texture == nullptr || frameHeight == 0)
		return false;
	int w = 0, h = 0;
	float aspect = 4.f / 3.f;
	frameSize(w, h, aspect);
	float u = 1, v = 1;
	frameUv(u, v);
	height = std::clamp(h, 120, std::max(maxHeight, 120));
	width = std::max((int)std::lround(height * aspect), 16);
	return display::capture(texture, u, v, width, height, rgba);
}

std::string clockText()
{
	const time_t t = time(nullptr);
	// A console whose clock was never set says it is 1970, or 2000.
	if (t < 1700000000)
		return "";
	const time_t local = t + platform::localTimeOffset();
	struct tm tm;
	gmtime_r(&local, &tm);
	return format("%02d:%02d", tm.tm_hour, tm.tm_min);
}

// Texture packs are with the user's files, or on a USB drive: the emulator
// (src/core/texture_replacements.cpp) takes the first folder that has the
// game's.
void nameTextureFolders()
{
	std::string folders;
	for (const std::string& folder : textureFolders())
		folders += (folders.empty() ? "" : ";") + folder;
	setenv("SWANSTATION_TEXTURES_DIR", folders.c_str(), 1);
}

std::vector<std::string> textureFolders()
{
	std::vector<std::string> folders = { rootDir + "textures" };
	// On a USB drive: a folder named textures in its games folder.
	for (const std::string& games : platform::usbGameDirs())
		folders.push_back(games + "/textures");
	return folders;
}

int texturePackFiles(const std::string& serial, std::string *where)
{
	if (serial.empty())
		return 0;
	// As the emulator chooses: the first folder that has this game's.
	for (const std::string& folder : textureFolders())
	{
		DIR *dir = opendir((folder + "/" + serial).c_str());
		if (dir == nullptr)
			continue;
		int files = 0;
		while (const dirent *entry = readdir(dir))
			if (extension(entry->d_name) == ".png")
				files++;
		closedir(dir);
		if (where != nullptr)
			*where = folder + "/" + serial;
		return files;
	}
	return 0;
}

bool gunAim(int port, float& x, float& y)
{
	if (!isRunning || netOn || port < 0 || port >= platform::MaxPads || options::frontend().controller[port] != 5
			|| !platform::pad(port).connected)
		return false;
	x = aims[port].x;
	y = aims[port].y;
	return true;
}

bool saveState(int slot)
{
	if (!isRunning || netOn)
		return false;
	const size_t size = retro_serialize_size();
	std::vector<uint8_t> raw(size);
	if (!retro_serialize(raw.data(), size))
	{
		addMessage("The state could not be saved.");
		return false;
	}
	// The emulator's buffer is a fixed 11 MiB, mostly unused: compressed.
	mz_ulong packedSize = mz_compressBound((mz_ulong)size);
	const std::vector<uint8_t> extra = achievements::saveProgress();
	constexpr size_t Header = sizeof(StateMagic2) + 24;
	std::vector<uint8_t> packed(Header + packedSize + extra.size());
	if (mz_compress2(packed.data() + Header, &packedSize, raw.data(), (mz_ulong)size, 1) != MZ_OK)
		return false;
	memcpy(packed.data(), StateMagic2, sizeof(StateMagic2));
	const uint64_t sizes[3] = { size, packedSize, extra.size() };
	memcpy(packed.data() + sizeof(StateMagic2), sizes, 24);
	if (!extra.empty())
		memcpy(packed.data() + Header + packedSize, extra.data(), extra.size());
	const bool ok = writeFile(statePath(slot), packed.data(), Header + packedSize + extra.size());
	diag::mark("state: slot %d %s (%zu KiB)", slot, ok ? "saved" : "could not be written",
			(Header + (size_t)packedSize + extra.size()) >> 10);
	if (ok)
	{
		// What the game showed, kept beside the state for the lists.
		std::vector<uint8_t> rgba;
		int width = 0, height = 0;
		const std::string picture = stateThumbPath(slot);
		if (!capture(360, rgba, width, height) || !display::writePng(picture, rgba.data(), width, height))
			unlink(picture.c_str());
		if (slot != ResumeSlot)
			lastSlot = slot;
	}
	if (slot != ResumeSlot)
		addMessage(ok ? format("State saved to slot %d", slot + 1) : "The state could not be written.");
	return ok;
}

bool loadState(int slot)
{
	if (!isRunning || netOn)
		return false;
	if (restricted())
	{
		addMessage("States are not loaded in hardcore mode (RetroAchievements).", 4.0);
		return false;
	}
	std::vector<uint8_t> packed;
	if (!readFile(statePath(slot), packed) || packed.size() < sizeof(StateMagic) + 8)
		return false;
	uint64_t rawSize = 0, packedSize = 0, extraSize = 0;
	size_t header = sizeof(StateMagic) + 8;
	memcpy(&rawSize, packed.data() + sizeof(StateMagic), 8);
	if (memcmp(packed.data(), StateMagic2, sizeof(StateMagic2)) == 0 && packed.size() >= sizeof(StateMagic2) + 24)
	{
		header = sizeof(StateMagic2) + 24;
		memcpy(&packedSize, packed.data() + sizeof(StateMagic2) + 8, 8);
		memcpy(&extraSize, packed.data() + sizeof(StateMagic2) + 16, 8);
		if (packedSize > packed.size() - header || extraSize > packed.size() - header - packedSize)
			return false;
	}
	else if (memcmp(packed.data(), StateMagic, sizeof(StateMagic)) == 0)
		packedSize = packed.size() - header;
	else
		return false;
	if (rawSize == 0 || rawSize > (64u << 20))
		return false;
	std::vector<uint8_t> raw(rawSize);
	mz_ulong got = (mz_ulong)rawSize;
	if (mz_uncompress(raw.data(), &got, packed.data() + header, (mz_ulong)packedSize) != MZ_OK)
		return false;
	const bool ok = retro_unserialize(raw.data(), got);
	diag::mark("state: slot %d %s", slot, ok ? "loaded" : "refused by the emulator");
	if (ok)
	{
		audio::clear();
		cheats::apply();
		rewind::clear();
		// Achievements in progress are as they were at that moment (a state
		// from before they were kept starts them afresh).
		if (extraSize != 0)
			achievements::loadProgress(packed.data() + header + packedSize, (size_t)extraSize);
		else
			achievements::gameReset();
		if (slot != ResumeSlot)
			lastSlot = slot;
	}
	if (slot != ResumeSlot)
		addMessage(ok ? format("State loaded from slot %d", slot + 1) : "The state could not be loaded.");
	return ok;
}

bool stateExists(int slot, std::string *when)
{
	struct stat st;
	if (stat(statePath(slot).c_str(), &st) != 0)
		return false;
	if (when != nullptr)
		*when = timeText(st.st_mtime);
	return true;
}

bool stateExistsFor(const std::string& gamePath, int slot, std::string *when)
{
	struct stat st;
	if (stat(statePathFor(gamePath, slot).c_str(), &st) != 0)
		return false;
	if (when != nullptr)
		*when = timeText(st.st_mtime);
	return true;
}

int discCount()
{
	return isRunning && diskSet && disk.get_num_images != nullptr ? (int)disk.get_num_images() : 0;
}

int discIndex()
{
	return isRunning && diskSet && disk.get_image_index != nullptr ? (int)disk.get_image_index() : 0;
}

std::string discLabel(int index)
{
	char label[512] = "";
	if (diskSet && disk.get_image_label != nullptr && disk.get_image_label((unsigned)index, label, sizeof(label)))
		return label;
	return format("Disc %d", index + 1);
}

bool setDisc(int index)
{
	if (!isRunning || !diskSet || index < 0 || index >= discCount())
		return false;
	if (index == discIndex() && !disk.get_eject_state())
		return true;
	if (!disk.get_eject_state() && !disk.set_eject_state(true))
		return false;
	const bool chosen = disk.set_image_index((unsigned)index);
	const bool inserted = disk.set_eject_state(false);
	diag::mark("disc: %d %s", index + 1, chosen && inserted ? "inserted" : "could not be inserted");
	if (chosen && inserted)
	{
		addMessage("Inserted " + discLabel(index));
		history::setDisc(current.path, index);
		// The other disc has a serial of its own, and with it its own cheats
		// and settings.
		const std::string serial = traySerial();
		if (!serial.empty() && serial != current.serial)
		{
			current.serial = serial;
			options::loadGame(serial);
			cheats::loadFor(serial, firstDiscSerial);
			cheats::apply();
		}
		if (options::frontend().achievements)
			achievements::discChanged(trayPath());
	}
	return chosen && inserted;
}

std::vector<Message> messages()
{
	std::lock_guard<std::mutex> lock(messageMutex);
	const double t = now();
	messageList.erase(std::remove_if(messageList.begin(), messageList.end(),
			[t](const Message& message) { return message.until < t; }), messageList.end());
	return messageList;
}

void addMessage(const std::string& text, double seconds)
{
	pushMessage(text, seconds, -1);
}

void addNotice(const std::string& title, const std::string& text, const std::string& picture, double seconds)
{
	pushMessage(text, seconds, -1, title, picture);
}

// ------------------------------------------------------------------ netplay

bool netplayHost()
{
	if (!isRunning || current.serial.empty() || netOn)
		return false;
	if (!netplay::host(netSession(), netHooks()))
		return false;
	netBegin();
	diag::mark("netplay: hosting %s", current.serial.c_str());
	return true;
}

bool netplayJoin(const std::string& address)
{
	if (!isRunning || current.serial.empty() || netOn)
		return false;
	if (!netplay::join(address, netSession(), netHooks()))
		return false;
	netBegin();
	diag::mark("netplay: joining with %s", current.serial.c_str());
	return true;
}

void netplayStop()
{
	netEnd();
}

// --------------------------------------------------------- RetroAchievements

void achievementsApply()
{
	if (!achievementsStarted)
		return;
	const options::Frontend& settings = options::frontend();
	achievements::setHardcore(settings.achievements && settings.hardcore);
	achievements::setUnofficial(settings.unofficial);
	const achievements::Summary now_ = achievements::summary();
	if (!settings.achievements)
	{
		if (now_.loggedIn || now_.loggingIn)
			achievements::logout();
		return;
	}
	if (!now_.loggedIn && !now_.loggingIn && !keptUser.empty() && !keptToken.empty())
	{
		achievements::loginWithToken(keptUser, keptToken);
		if (isRunning && !current.path.empty())
			achievements::gameStarted(trayPath());
	}
}

void achievementsLogin(const std::string& user, const std::string& password)
{
	if (!achievementsStarted || user.empty() || password.empty())
		return;
	achievements::loginWithPassword(user, password);
	if (isRunning && !current.path.empty())
		achievements::gameStarted(trayPath());
}

void achievementsLogout()
{
	if (achievementsStarted)
		achievements::logout();
	keptUser.clear();
	keptToken.clear();
	saveKeptLogin();
}

std::string achievementsUser()
{
	return keptUser;
}

void tick()
{
	const options::Frontend& settings = options::frontend();
	// Each player's light in their colour: blue, red, green, pink, as the
	// PlayStation 4 gave them.
	static const uint8_t colours[platform::MaxPads][3] = { { 0, 70, 255 }, { 255, 30, 30 }, { 30, 220, 70 }, { 255, 50, 190 } };
	bool gun = false;
	for (int i = 0; i < platform::MaxPads; i++)
	{
		if (settings.playerLights && platform::pad(i).connected)
			platform::padLight(i, colours[i][0], colours[i][1], colours[i][2]);
		else
			platform::padLight(i, 0, 0, 0);
		gun = gun || settings.controller[i] == 5;
	}
	platform::padMotion(settings.motion != 0 || gun);
	if (achievementsStarted)
	{
		// With no frame of a game running, the server's answers still come.
		if (!isRunning || isPaused)
			achievements::idle();
		achievementEvents();
	}
}

void applyControllers()
{
	if (coreReady)
		applyControllerTypes();
}

std::string biosSummary()
{
	std::string found;
	for (const char *name : { "scph5500.bin", "scph5501.bin", "scph5502.bin", "psxonpsp660.bin", "ps1_rom.bin" })
		if (fileExists(rootDir + "bios/" + name))
			found += (found.empty() ? "" : ", ") + std::string(name);
	return found.empty() ? "none: the built-in OpenBIOS is used" : found;
}

void flushSaveRam()
{
}

}
