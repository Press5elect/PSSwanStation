/*
	PSSwanStation - the Sinden Lightgun, read by the console itself: what the
	rest of the title asks of it.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	src/ps5/sinden_ps5.cpp finds the gun on the console's USB, reads its
	serial port (firmware, camera name, calibration), its buttons (the HID
	interface) and its camera (UVC), on a thread of its own, and says every
	step in the boot log as "sinden:" lines (ps5/docs/dev/sinden-lightgun.md,
	"The console side"). What it hands over is here: the newest camera picture
	as brightness, the buttons, the calibration and the gun's state, so the
	frame's thread can find the screen's border in the picture and aim the
	GunCon. On a PC (src/host/sinden_host.cpp) there is no gun: the state stays
	Off.
*/
#pragma once

#include <cstdint>
#include <string>

namespace fe::sinden
{

// What start() is asked to do.
struct Options
{
	// Only the first stage: the USB devices listed and the gun's descriptors
	// decoded in the boot log; nothing is sent to the gun, nothing read from
	// its endpoints.
	bool findOnly = false;
	// The ways tried, in order: 0 libSceUsbd first, then the kernel's ugen
	// nodes; 1 libSceUsbd only; 2 ugen only.
	int route = 0;
};

// Starts the gun's thread; a second call does nothing. Meant to be called
// once at start-up, after the boot log is open (platform::lateInit), when the
// gun's setting is on. The thread looks for a gun every 2 seconds until one is
// plugged in, and again after it is pulled out.
void start(const Options& options = {});
// Stops the thread: the camera's stream is ended, its bandwidth given back
// (alternate setting 0) and the gun's interfaces released. Waits for it
// (a fraction of a second).
void stop();
// Until the setting exists: starts the thread when SWANSTATION_SINDEN is in
// the environment or <root>data/sinden.txt exists, its first word saying how
// ("find": the first stage only; "usbd" or "ugen": that way only; anything
// else: everything). True when started.
bool startIfAsked();
// True while the thread runs and the device is one of the gun's own (vendor
// 0x16c0, products 0x0f01, 0x0f02, 0x0f38, 0x0f39): usbpad_ps5.cpp leaves it
// alone then (in its joystick mode it would look like a gamepad). So start()
// is called before platform::usbPadStart().
bool claims(uint16_t vendor, uint16_t product);

enum class State
{
	Off,		// not started
	Absent,		// started; no gun plugged in
	Found,		// a gun is there and being set up (or "find only")
	Streaming,	// its camera sends pictures
	Failed,		// a gun is there, but its camera does not stream: Status::why
};

struct Status
{
	State state = State::Off;
	std::string why;			// what failed, or the step it is at
	uint16_t vendor = 0, product = 0;
	std::string route;			// "libSceUsbd" or "ugen"
	bool serial = false;		// the serial port answered
	bool buttons = false;		// the HID interface sends reports
	std::string firmware;		// "1.9", from the serial port
	std::string camera;			// the camera's name, from the serial port
	std::string format;			// "YUY2", "MJPEG" ...
	int width = 0, height = 0;
	double framesPerSecond = 0;	// over the last few seconds
};
Status status();
State state();

inline const char *stateName(State state)
{
	switch (state)
	{
	case State::Off: return "off";
	case State::Absent: return "no gun";
	case State::Found: return "found";
	case State::Streaming: return "camera streaming";
	case State::Failed: return "failed";
	}
	return "?";
}

// The newest camera picture, as brightness (Y): `width` x `height` bytes,
// rows `stride` bytes apart.
struct Frame
{
	const uint8_t *luma = nullptr;
	int width = 0, height = 0, stride = 0;
	double time = 0;			// fe::now() when its last packet came
	uint64_t number = 0;		// 1, 2, 3 ... (pictures the camera sent, counted)
};
// True when a picture newer than the one this returned last is there, with
// `out` describing it. For one thread only (the frame's): `out.luma` stays
// valid and unchanged until that thread calls this again. No copy, no lock.
bool latestFrame(Frame& out);

// The buttons, as the gun's HID interface reports them. Which button is
// which is set in Sinden's own software (its defaults: the trigger the left
// mouse button, the pump action the right one; the others keys): the boot
// log's "sinden: buttons:" lines show what each press sends.
struct Buttons
{
	uint32_t mouse = 0;			// bit n: mouse button n + 1 (1 left, 2 right, 3 middle ...)
	uint32_t joystick = 0;		// bit n: joystick button n + 1 (the gun's joystick mode)
	uint8_t modifiers = 0;		// the keyboard's modifier keys (bit n: usage 0xE0 + n)
	uint8_t keys[6] = {};		// keyboard keys held (HID usage page 7), 0 for none
	uint64_t reports = 0;		// reports read since the gun was found
};
Buttons buttons();

// The gun's calibration offsets, read over its serial port (commands 104 and
// 105): (raw - 10000) / 100, as Sinden's gun_serial.py shows them; that tool
// shows 0 for a value over 99 either way (none set).
struct Calibration
{
	bool known = false;
	int rawX = 0, rawY = 0;
	double x = 0, y = 0;
};
Calibration calibration();

}
