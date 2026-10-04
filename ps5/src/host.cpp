/*
	SwanStation for PS5 - the libretro host: the emulator core, run by the title.

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

	Pacing. The display paces the emulator: one emulated frame for each
	refresh when the two rates are within one percent (a 59.94 Hz display and
	an NTSC game), and the sound is resampled to follow (audio.cpp). A PAL game
	on the console's 59.94 Hz output gets five frames in six refreshes; the
	console has no 50 Hz mode.
*/
#include "fe.h"
#include "display.h"

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
#include <mutex>
#include <sys/stat.h>

namespace fe::host
{
namespace
{

// --------------------------------------------------------------- the state

bool coreReady;
bool isRunning;
bool isPaused;
GameInfo current;
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

int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
	if (port >= (unsigned)platform::MaxPads)
		return 0;
	const platform::Pad& pad = platform::pad((int)port);
	if (!pad.connected)
		return 0;
	using namespace platform;
	switch (device & RETRO_DEVICE_MASK)
	{
	case RETRO_DEVICE_JOYPAD:
	{
		// The PlayStation's Select and Start are the two halves of the touch
		// pad; OPTIONS belongs to the title (it opens the menu).
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
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		{
			int16_t mask = 0;
			for (unsigned i = 0; i < 16; i++)
				if (buttons & map[i])
					mask |= (int16_t)(1u << i);
			return mask;
		}
		return id < 16 && (buttons & map[id]) != 0 ? 1 : 0;
	}
	case RETRO_DEVICE_ANALOG:
		if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
			return id == RETRO_DEVICE_ID_ANALOG_X ? stick(pad.lx) : stick(pad.ly);
		if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
			return id == RETRO_DEVICE_ID_ANALOG_X ? stick(pad.rx) : stick(pad.ry);
		return 0;
	default:
		return 0;
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

void pushMessage(const std::string& text, double seconds, int progress)
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
	messageList.push_back({ text, now() + seconds, progress });
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

constexpr char StateMagic[8] = { 'S', 'W', 'P', 'S', '5', 'S', 'T', '1' };

void applyControllerTypes()
{
	static const unsigned devices[4] = {
		RETRO_DEVICE_JOYPAD,							// digital controller
		RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0),	// DualShock
		RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1),	// analog joystick
		RETRO_DEVICE_NONE,
	};
	for (unsigned port = 0; port < 2; port++)
		retro_set_controller_port_device(port, devices[std::clamp(options::frontend().controller[port], 0, 3)]);
	// With a multitap the other ports take pads 3 and 4 as DualShocks.
	for (unsigned port = 2; port < 4; port++)
		retro_set_controller_port_device(port, devices[1]);
}

// The serial of each game that ran, by its path: a game's own options are
// then known before it starts the next time (<root>data/game-ids.txt).
std::string knownSerial(const std::string& path)
{
	FILE *f = fopen((rootDir + "data/game-ids.txt").c_str(), "r");
	if (f == nullptr)
		return "";
	char line[2048];
	std::string found;
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		char *tab = strchr(line, '\t');
		if (tab == nullptr)
			continue;
		*tab = 0;
		if (path == trim(tab + 1))
			found = line;
	}
	fclose(f);
	return found;
}

void rememberSerial(const std::string& path, const std::string& serial)
{
	if (path.empty() || serial.empty() || knownSerial(path) == serial)
		return;
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

} // namespace

// ------------------------------------------------------------------- public

bool init()
{
	saveDir = rootDir + "data/saves";
	systemDir = rootDir + "bios";
	cacheDir = rootDir + "data/cache";
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
	return true;
}

void shutdown()
{
	stop();
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
	if (paused)
		for (int i = 0; i < platform::MaxPads; i++)
			platform::padRumble(i, 0, 0);
}

const GameInfo& game()
{
	return current;
}

std::string lastError()
{
	return errorText;
}

bool start(const std::string& path, int stateSlot)
{
	if (!coreReady)
		return false;
	stop();
	errorText.clear();
	diag::mark("game: starting %s", path.empty() ? "(the BIOS)" : path.c_str());

	current = GameInfo();
	current.path = path;
	current.title = path.empty() ? "PlayStation BIOS" : fileTitle(path);
	// A game's own options, when it ran before, are in place before it boots.
	options::loadGame(knownSerial(path));
	applyControllerTypes();

	hwRenderSet = false;
	hwImageValid = false;
	frameWidth = frameHeight = 0;
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
	current.serial = System::GetRunningCode();
	diag::mark("game: running, serial %s, %.2f fps, %u x %u", current.serial.c_str(), av.timing.fps,
			av.geometry.base_width, av.geometry.base_height);
	rememberSerial(path, current.serial);
	options::loadGame(current.serial);
	cheats::loadFor(current.serial);
	cheats::apply();
	pacing = 1.0;
	ranThisSecond = 0;
	secondStarted = now();
	fpsMeasured = 0;
	audio::clear();
	setPaused(false);
	if (stateSlot != -1 && stateExists(stateSlot))
		loadState(stateSlot);
	return true;
}

void stop()
{
	if (!isRunning)
		return;
	diag::mark("game: stopping");
	if (options::frontend().autoSaveOnExit && !current.path.empty())
		saveState(ResumeSlot);
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
}

void reset()
{
	if (!isRunning)
		return;
	retro_reset();
	cheats::apply();
	audio::clear();
}

void runFrame()
{
	if (!isRunning || isPaused)
		return;
	const double displayHz = display::refreshRate();
	const double coreHz = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
	double ratio = coreHz / displayHz;
	if (options::frontend().syncToDisplay && std::fabs(ratio - 1.0) < 0.01)
		ratio = 1.0;
	pacing += ratio;
	int ran = 0;
	while (pacing >= 1.0 && ran < 2)
	{
		pacing -= 1.0;
		retro_run();
		ran++;
	}
	if (pacing > 2.0)
		pacing = 0;
	ranThisSecond += ran;
	const double t = now();
	if (t - secondStarted >= 1.0)
	{
		fpsMeasured = (float)(ranThisSecond / (t - secondStarted));
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

bool saveState(int slot)
{
	if (!isRunning)
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
	std::vector<uint8_t> packed(sizeof(StateMagic) + 8 + packedSize);
	if (mz_compress2(packed.data() + sizeof(StateMagic) + 8, &packedSize, raw.data(), (mz_ulong)size, 1) != MZ_OK)
		return false;
	memcpy(packed.data(), StateMagic, sizeof(StateMagic));
	const uint64_t rawSize = size;
	memcpy(packed.data() + sizeof(StateMagic), &rawSize, 8);
	const bool ok = writeFile(statePath(slot), packed.data(), sizeof(StateMagic) + 8 + packedSize);
	diag::mark("state: slot %d %s (%zu KiB)", slot, ok ? "saved" : "could not be written",
			(sizeof(StateMagic) + 8 + (size_t)packedSize) >> 10);
	if (slot != ResumeSlot)
		addMessage(ok ? format("State saved to slot %d", slot + 1) : "The state could not be written.");
	return ok;
}

bool loadState(int slot)
{
	if (!isRunning)
		return false;
	std::vector<uint8_t> packed;
	if (!readFile(statePath(slot), packed) || packed.size() < sizeof(StateMagic) + 8
			|| memcmp(packed.data(), StateMagic, sizeof(StateMagic)) != 0)
		return false;
	uint64_t rawSize = 0;
	memcpy(&rawSize, packed.data() + sizeof(StateMagic), 8);
	if (rawSize == 0 || rawSize > (64u << 20))
		return false;
	std::vector<uint8_t> raw(rawSize);
	mz_ulong got = (mz_ulong)rawSize;
	if (mz_uncompress(raw.data(), &got, packed.data() + sizeof(StateMagic) + 8,
			(mz_ulong)(packed.size() - sizeof(StateMagic) - 8)) != MZ_OK)
		return false;
	const bool ok = retro_unserialize(raw.data(), got);
	diag::mark("state: slot %d %s", slot, ok ? "loaded" : "refused by the emulator");
	if (ok)
	{
		audio::clear();
		cheats::apply();
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
	{
		char text[64];
		const time_t t = st.st_mtime;
		struct tm tm;
		localtime_r(&t, &tm);
		strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &tm);
		*when = text;
	}
	return true;
}

bool stateExistsFor(const std::string& gamePath, int slot)
{
	return fileExists(statePathFor(gamePath, slot));
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
		addMessage("Inserted " + discLabel(index));
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
