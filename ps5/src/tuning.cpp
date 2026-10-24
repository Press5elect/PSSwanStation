/*
	PSSwanStation - the console's own tuning: the runtime of tuning.h.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "tuning.h"
#include "display.h"
#include "fe.h"

#include "vulkan_loader.h"

#include <atomic>
#include <cstdlib>
#include <pthread.h>

#if defined(SWANSTATION_PS5)
extern "C" int scePthreadSetprio(pthread_t thread, int priority);
// RADV's VideoOut WSI (PS5 Mesa with Crux2 0011): the most images presented
// and not yet shown before an acquire waits. Weak: a driver without it leaves
// the swapchain at three images, as before.
extern "C" int wsi_videoout_set_queue_depth(int depth) __attribute__((weak));
#endif

namespace fe::tuning
{
namespace
{
std::atomic<unsigned> saidRoles{0};

// ------------------------------------------------------------------ counting
// The title's Vulkan calls go through the loader's table (dep/vulkan-loader):
// while the game is measured, the commands counted are pointed at a count and
// then at the driver's own. A device made again (another renderer) fills the
// table again, and the next frame points it here again.
std::atomic<uint64_t> counted[CountKinds];

inline void add(Count kind)
{
	counted[kind].fetch_add(1, std::memory_order_relaxed);
}

PFN_vkCmdDraw realDraw;
PFN_vkCmdDrawIndexed realDrawIndexed;
PFN_vkCmdBeginRenderPass realBeginRenderPass;
PFN_vkCmdPipelineBarrier realPipelineBarrier;
PFN_vkCmdCopyImage realCopyImage;
PFN_vkCmdBlitImage realBlitImage;
PFN_vkCmdCopyBufferToImage realCopyBufferToImage;
PFN_vkCmdCopyImageToBuffer realCopyImageToBuffer;
PFN_vkCmdClearColorImage realClearColorImage;
PFN_vkCmdClearDepthStencilImage realClearDepthStencilImage;
PFN_vkCmdClearAttachments realClearAttachments;

VKAPI_ATTR void VKAPI_CALL countDraw(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	add(Draws);
	realDraw(cb, a, b, c, d);
}
VKAPI_ATTR void VKAPI_CALL countDrawIndexed(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e)
{
	add(Draws);
	realDrawIndexed(cb, a, b, c, d, e);
}
VKAPI_ATTR void VKAPI_CALL countBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *info,
		VkSubpassContents contents)
{
	add(Passes);
	realBeginRenderPass(cb, info, contents);
}
VKAPI_ATTR void VKAPI_CALL countPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
		VkDependencyFlags flags, uint32_t memoryCount, const VkMemoryBarrier *memory, uint32_t bufferCount,
		const VkBufferMemoryBarrier *buffers, uint32_t imageCount, const VkImageMemoryBarrier *images)
{
	add(Barriers);
	realPipelineBarrier(cb, src, dst, flags, memoryCount, memory, bufferCount, buffers, imageCount, images);
}
VKAPI_ATTR void VKAPI_CALL countCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst,
		VkImageLayout dstLayout, uint32_t count, const VkImageCopy *regions)
{
	add(Copies);
	realCopyImage(cb, src, srcLayout, dst, dstLayout, count, regions);
}
VKAPI_ATTR void VKAPI_CALL countBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout srcLayout, VkImage dst,
		VkImageLayout dstLayout, uint32_t count, const VkImageBlit *regions, VkFilter filter)
{
	add(Copies);
	realBlitImage(cb, src, srcLayout, dst, dstLayout, count, regions, filter);
}
VKAPI_ATTR void VKAPI_CALL countCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout,
		uint32_t count, const VkBufferImageCopy *regions)
{
	add(Copies);
	realCopyBufferToImage(cb, src, dst, layout, count, regions);
}
VKAPI_ATTR void VKAPI_CALL countCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout layout, VkBuffer dst,
		uint32_t count, const VkBufferImageCopy *regions)
{
	add(Copies);
	realCopyImageToBuffer(cb, src, layout, dst, count, regions);
}
VKAPI_ATTR void VKAPI_CALL countClearColorImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
		const VkClearColorValue *colour, uint32_t count, const VkImageSubresourceRange *ranges)
{
	add(Clears);
	realClearColorImage(cb, image, layout, colour, count, ranges);
}
VKAPI_ATTR void VKAPI_CALL countClearDepthStencilImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
		const VkClearDepthStencilValue *value, uint32_t count, const VkImageSubresourceRange *ranges)
{
	add(Clears);
	realClearDepthStencilImage(cb, image, layout, value, count, ranges);
}
VKAPI_ATTR void VKAPI_CALL countClearAttachments(VkCommandBuffer cb, uint32_t count, const VkClearAttachment *attachments,
		uint32_t rects, const VkClearRect *rect)
{
	add(Clears);
	realClearAttachments(cb, count, attachments, rects, rect);
}

// Points one entry of the table at its count, keeping the driver's own.
template <typename F>
void point(F& entry, F& real, F count)
{
	if (entry != nullptr && entry != count)
	{
		real = entry;
		entry = count;
	}
}

void countCommands()
{
	point(vkCmdDraw, realDraw, &countDraw);
	point(vkCmdDrawIndexed, realDrawIndexed, &countDrawIndexed);
	point(vkCmdBeginRenderPass, realBeginRenderPass, &countBeginRenderPass);
	point(vkCmdPipelineBarrier, realPipelineBarrier, &countPipelineBarrier);
	point(vkCmdCopyImage, realCopyImage, &countCopyImage);
	point(vkCmdBlitImage, realBlitImage, &countBlitImage);
	point(vkCmdCopyBufferToImage, realCopyBufferToImage, &countCopyBufferToImage);
	point(vkCmdCopyImageToBuffer, realCopyImageToBuffer, &countCopyImageToBuffer);
	point(vkCmdClearColorImage, realClearColorImage, &countClearColorImage);
	point(vkCmdClearDepthStencilImage, realClearDepthStencilImage, &countClearDepthStencilImage);
	point(vkCmdClearAttachments, realClearAttachments, &countClearAttachments);
}

// ------------------------------------------------------------------ the state
struct State
{
	Behind behind;
	int depth = 0;				// what the driver was given (0: nothing yet)
	double windowBegan = 0;		// the behind watch's half second
	uint64_t ranAtWindow = 0;
	Window half;				// the same half second's frames, measured or not
	Window window;
	unsigned windowNumber = 0;
	uint64_t countsAtWindow[CountKinds] = {};
	unsigned underrunsAtWindow = 0;
	Session session;
	bool inGame = false;
	double lastFrame = 0;
};
State state;

bool measuring()
{
	const options::Frontend& f = options::frontend();
	return f.measure || f.verboseLog;
}

void setDepth(int depth)
{
#if defined(SWANSTATION_PS5)
	if (wsi_videoout_set_queue_depth == nullptr || depth == state.depth)
		return;
	const int r = wsi_videoout_set_queue_depth(depth);
	if (r != 0)
		diag::mark("tuning: the driver did not take %d frames ahead of the screen (%d)", depth, r);
	state.depth = depth;
#else
	state.depth = depth;
#endif
}

void startWindow(double t)
{
	state.window = Window();
	state.window.began = t;
	state.window.ranAtStart = host::ranFrames();
	for (int i = 0; i < CountKinds; i++)
		state.countsAtWindow[i] = counted[i].load(std::memory_order_relaxed);
	state.underrunsAtWindow = audio::underruns();
}

void sayWindow(double t)
{
	Window& w = state.window;
	const double seconds = t - w.began;
	const double fps = host::coreFps() > 1.0 ? host::coreFps() : 60.0;
	const double speed = seconds > 0 ? (double)(host::ranFrames() - w.ranAtStart) / (seconds * fps) : 0;
	for (int i = 0; i < CountKinds; i++)
		w.counts[i] = counted[i].load(std::memory_order_relaxed) - state.countsAtWindow[i];
	state.windowNumber++;
	diag::mark("test: window %u: %.0f%% speed, %.1f frames a second, worst frame %.1f ms, %u over a refresh and a "
			"half, %u sound underruns, %d frames ahead%s; a frame: pad %.2f, wait %.2f, game %.2f, tick %.2f, "
			"interface %.2f, present %.2f ms; draws %.0f, passes %.1f, barriers %.1f, copies %.1f, clears %.1f",
			state.windowNumber, speed * 100.0, seconds > 0 ? w.frames / seconds : 0.0, w.worst * 1e3, w.overLimit,
			audio::underruns() - state.underrunsAtWindow, state.depth != 0 ? state.depth : 2,
			state.behind.behind ? " (behind)" : "", w.average(0) * 1e3, w.average(1) * 1e3, w.average(2) * 1e3,
			w.average(3) * 1e3, w.average(4) * 1e3, w.average(5) * 1e3, w.perFrame(Draws), w.perFrame(Passes),
			w.perFrame(Barriers), w.perFrame(Copies), w.perFrame(Clears));
}
}

// ------------------------------------------------------------------ priorities
void setThread(Role role)
{
	if (!options::frontend().threadPriorities)
		return;
	const int wanted = priorityOf(role);
#if defined(SWANSTATION_PS5)
	const int result = scePthreadSetprio(pthread_self(), wanted);
#else
	const int result = 0;
#endif
	const unsigned bit = 1u << (unsigned)role;
	if ((saidRoles.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
		diag::mark("tuning: a %s thread runs at %d (%#x)", nameOf(role), wanted, (unsigned)result);
}

// ------------------------------------------------------------------ the driver
void applyDriverSettings()
{
	const options::Frontend& f = options::frontend();
	if (f.driverThread)
		setenv("RADV_THREADED_RECORDING", "1", 1);
	const int spin = spinMicroseconds(f.gpuWaitSpin), sleep = sleepMicroseconds(f.gpuWaitSleep);
	if (f.gpuWaitSpin != 0)
		setenv("RADV_PS5_WAIT_SPIN_US", format("%d", spin).c_str(), 1);
	if (f.gpuWaitSleep != 0)
		setenv("RADV_PS5_WAIT_SLEEP_US", format("%d", sleep).c_str(), 1);
	diag::mark("tuning: the graphics driver %s; the GPU wait spins %d us, then sleeps %d us between checks; "
			"sound and controls %s", f.driverThread ? "records on its own thread" : "records on the frame's thread",
			spin, sleep, f.threadPriorities ? "run before the emulator" : "at the emulator's priority");
}

bool queueDepthAvailable()
{
#if defined(SWANSTATION_PS5)
	return wsi_videoout_set_queue_depth != nullptr;
#else
	return false;
#endif
}

void swapchainMade()
{
	state.depth = 0;
	setDepth(state.behind.behind ? DepthBehind : DepthKeptUp);
}

// ------------------------------------------------------------------ each frame
void frame(const double at[Parts + 1], bool playing)
{
	const double t = at[Parts];
	if (measuring())
		countCommands();
	// Fast forward, a menu over the game and no game are not the game's speed.
	if (!playing || !state.inGame || host::fastForward())
	{
		state.windowBegan = 0;
		state.window.began = 0;
		return;
	}
	const float rate = display::refreshRate();
	const double limit = rate > 1.f ? 1.5 / rate : 0.025;
	// The half-second watch: behind, and the session's speed.
	if (state.windowBegan == 0)
	{
		state.windowBegan = t;
		state.ranAtWindow = host::ranFrames();
		state.half = Window();
	}
	state.half.frame(at, limit);
	if (t - state.windowBegan >= Behind::Window)
	{
		const double seconds = t - state.windowBegan;
		const double fps = host::coreFps() > 1.0 ? host::coreFps() : 60.0;
		const double speed = (double)(host::ranFrames() - state.ranAtWindow) / (seconds * fps);
		if (state.behind.note(speed))
		{
			if (state.behind.timesBehind <= 5 || !state.behind.behind)
				diag::mark("tuning: the game is %s (%.0f%% in the last half second): %d frames ahead of the screen",
						state.behind.behind ? "behind" : "keeping up again", speed * 100.0,
						state.behind.behind ? DepthBehind : DepthKeptUp);
			setDepth(state.behind.behind ? DepthBehind : DepthKeptUp);
		}
		if (state.session.note(seconds, speed))
			diag::mark("slow: the game has run under %.0f%% for %.1f s (%.0f%% in the last half second); a frame: game "
					"%.2f, interface %.2f, present %.2f ms", Session::SlowUnder * 100.0, state.session.stretch,
					speed * 100.0, state.half.average(2) * 1e3, state.half.average(4) * 1e3,
					state.half.average(5) * 1e3);
		state.windowBegan = t;
		state.ranAtWindow = host::ranFrames();
		state.half = Window();
	}
	if (!measuring())
		return;
	if (state.window.began == 0)
		startWindow(t);
	state.window.frame(at, limit);
	if (t - state.window.began >= 5.0)
	{
		sayWindow(t);
		startWindow(t);
	}
}

void gameStarted()
{
	state.inGame = true;
	state.session = Session();
	state.behind = Behind();
	state.windowBegan = 0;
	state.window.began = 0;
	state.windowNumber = 0;
	setDepth(DepthKeptUp);
}

void gameEnded()
{
	if (!state.inGame)
		return;
	state.inGame = false;
	const Session& s = state.session;
	if (s.played > 0)
		diag::mark("session: %.0f s played at %.1f%% speed on average; %.1f s under %.0f%%, the longest stretch %.1f s; "
				"behind the screen %u time(s)", s.played, s.averageSpeed() * 100.0, s.under, Session::SlowUnder * 100.0,
				s.longest, state.behind.timesBehind);
	state.behind = Behind();
	setDepth(DepthKeptUp);
}

}
