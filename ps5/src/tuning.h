// PSSwanStation - the console's own tuning (from Crux2, "PS5 homebrew stack:
// what Crux2 fixed and learned"): which threads run first, the graphics
// driver's settings, how far the frames may run ahead of the screen, and the
// boot log's measuring lines.
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Thread priorities. Every thread of a title starts at 700 (of 256 to 767,
// lower runs first), and the console's scheduler gives a processor to the
// higher priority strictly: a thread that never waits keeps its processor
// from every thread at its priority or below (PS5_MihawkOS's scheduler probe,
// as Crux2 measured it). With "Sound and controls before the emulator" on, the
// sound output runs at 690 and the USB controllers' thread at 692, ahead of
// the frame's thread (700: the pads, the emulator, the interface, the
// present), and the background jobs (covers, videos, the library, the share's
// and the cloud's files, RetroAchievements' requests) run at 730, after it.
// The disc drive's reading thread keeps the lowest, 767, as before. Off by
// default, as in Crux2: a setting that has not run on the console ships off.
//
// The frames ahead of the screen. The swapchain has four images; while the
// game keeps up, at most two of them wait to be shown (as the three images did
// before), so what is shown follows the controls as closely as before. When
// the game falls behind (two half-second windows under 97 % of its speed), a
// third may wait, so a frame that waits for its vblank does not hold the next
// one back; after ten seconds at 99 % or more it is two again (Crux2's
// PS5AutoFrameQueue, with RADV's wsi_videoout_set_queue_depth, Crux2 0011).
//
// The boot log. Every five seconds of play, with "Measure the game" on (or
// verbose logging): a "test: window" line with the speed, the worst frame,
// what each part of a frame took and, per frame, the draws, render passes,
// barriers, copies and clears recorded (all of the title's: the game's
// renderer and the interface's). A "slow:" line whenever the game has run
// under 95 % for two seconds (at most 30 a session), and a "session:" line
// when a game closes: its average speed, the time under 95 % and the longest
// stretch of it. Those two are always written.
//
// The arithmetic is here, header-only, so tests/tuning checks it on the PC.
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>

namespace fe::tuning
{

// ------------------------------------------------------------------ priorities
enum class Role
{
	Sound,		// 690: the sound output
	Input,		// 692: the USB controllers' thread
	Frame,		// 700: the frame's thread, where every thread starts
	Background,	// 730: covers, videos, the library, files kept elsewhere, requests
};

inline int priorityOf(Role role)
{
	switch (role)
	{
	case Role::Sound: return 690;
	case Role::Input: return 692;
	case Role::Frame: return 700;
	case Role::Background: return 730;
	}
	return 700;
}

inline const char *nameOf(Role role)
{
	switch (role)
	{
	case Role::Sound: return "sound";
	case Role::Input: return "controllers";
	case Role::Frame: return "frame";
	case Role::Background: return "background job";
	}
	return "?";
}

// The calling thread runs at its role's priority, when "Sound and controls
// before the emulator" is on. Said once per role in the boot log. (Weak: the
// PC tests that build one of the files that call it alone go without it.)
void setThread(Role role) __attribute__((weak));
// A thread's work, run at the background jobs' priority:
// std::thread(tuning::background(work), arguments...).
template <typename F>
auto background(F&& work)
{
	return [w = std::forward<F>(work)](auto&&... arguments) mutable {
		if (setThread != nullptr)
			setThread(Role::Background);
		return w(std::forward<decltype(arguments)>(arguments)...);
	};
}

// ------------------------------------------------------------------ the driver
// The GPU wait's spin and sleep (RADV_PS5_WAIT_SPIN_US, RADV_PS5_WAIT_SLEEP_US,
// Crux2 0009): the settings' choices, the first the driver's own.
inline constexpr int SpinChoices[] = { 1500, 500, 250, 3000 };
inline constexpr int SleepChoices[] = { 1000, 500, 250, 100 };

inline int spinMicroseconds(int choice)
{
	return SpinChoices[std::clamp(choice, 0, 3)];
}

inline int sleepMicroseconds(int choice)
{
	return SleepChoices[std::clamp(choice, 0, 3)];
}

// The driver's settings, put in its environment before the device is made:
// the driver's own thread (RADV_THREADED_RECORDING) and the GPU wait's spin
// and sleep. From the next start; said in the boot log.
void applyDriverSettings();
// The driver can be told how many images may wait (it has Crux2 0011): the
// swapchain is then made with four images, three without it.
bool queueDepthAvailable();
// A swapchain was made: the driver is given the frames ahead again.
void swapchainMade();

// ------------------------------------------------------------------ the frames ahead
// Images that may wait to be shown: two while the game keeps up, three while
// it is behind (the swapchain has four).
inline constexpr int DepthKeptUp = 2;
inline constexpr int DepthBehind = 3;

// Whether the game is behind, from its speed in half-second windows.
struct Behind
{
	static constexpr double Window = 0.5;			// seconds
	static constexpr double FallsUnder = 0.97;		// two windows under this: behind
	static constexpr double KeepsUpAt = 0.99;		// back when this long at this or more
	static constexpr double KeepsUpFor = 10.0;

	bool behind = false;
	int under = 0;				// windows in a row under FallsUnder
	double keptUpFor = 0;		// seconds in a row at KeepsUpAt or more, while behind
	unsigned timesBehind = 0;

	// One window's speed (1 is full speed). True when `behind` changed.
	bool note(double speed)
	{
		if (!behind)
		{
			under = speed < FallsUnder ? under + 1 : 0;
			if (under >= 2)
			{
				behind = true;
				under = 0;
				keptUpFor = 0;
				timesBehind++;
				return true;
			}
			return false;
		}
		keptUpFor = speed >= KeepsUpAt ? keptUpFor + Window : 0;
		if (keptUpFor >= KeepsUpFor)
		{
			behind = false;
			keptUpFor = 0;
			return true;
		}
		return false;
	}
};

// ------------------------------------------------------------------ the measuring
// What the title recorded into its command buffers.
enum Count
{
	Draws,
	Passes,
	Barriers,
	Copies,		// image and buffer copies, blits, uploads
	Clears,
	CountKinds,
};

// The parts of a frame main.cpp times: pad, wait (the next image), game,
// tick, interface, present.
inline constexpr int Parts = 6;

// A five-second window of play.
struct Window
{
	double began = 0, last = 0;
	uint64_t ranAtStart = 0;
	unsigned frames = 0, overLimit = 0;
	double worst = 0;
	double part[Parts] = {};
	uint64_t counts[CountKinds] = {};

	void frame(const double at[Parts + 1], double limit)
	{
		const double length = at[Parts] - at[0];
		frames++;
		worst = std::max(worst, length);
		if (length > limit)
			overLimit++;
		for (int i = 0; i < Parts; i++)
			part[i] += at[i + 1] - at[i];
	}
	double average(int i) const
	{
		return frames != 0 ? part[i] / frames : 0;
	}
	double perFrame(int kind) const
	{
		return frames != 0 ? (double)counts[kind] / frames : 0;
	}
};

// The speed over a game: the time under 95 %, its longest stretch, and when a
// "slow:" line is due (two seconds under, at most 30 a session).
struct Session
{
	static constexpr double SlowUnder = 0.95;
	static constexpr double SlowAfter = 2.0;
	static constexpr unsigned MostSlowLines = 30;

	double played = 0, gameSeconds = 0;		// wall time, and the game's own time at full speed
	double under = 0, stretch = 0, longest = 0;
	unsigned slowLines = 0;
	bool saidThisStretch = false;

	// A stretch of `seconds` at `speed`. True when a "slow:" line is due now.
	bool note(double seconds, double speed)
	{
		played += seconds;
		gameSeconds += seconds * speed;
		if (speed >= SlowUnder)
		{
			stretch = 0;
			saidThisStretch = false;
			return false;
		}
		under += seconds;
		stretch += seconds;
		longest = std::max(longest, stretch);
		if (stretch >= SlowAfter && !saidThisStretch && slowLines < MostSlowLines)
		{
			saidThisStretch = true;
			slowLines++;
			return true;
		}
		return false;
	}
	double averageSpeed() const
	{
		return played > 0 ? gameSeconds / played : 0;
	}
};

// Each frame of the main loop: `at` the times main.cpp took, `playing` when
// the game ran this frame. The windows, the "slow:" lines and the frames ahead
// of the screen follow from it.
void frame(const double at[Parts + 1], bool playing);
// A game started, and one closed (its "session:" line).
void gameStarted();
void gameEnded();

}
