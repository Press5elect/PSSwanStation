/*
	PSSwanStation - the Sinden Lightgun's aim: the camera's newest picture
	(sinden.h, read over USB on the gun's own thread) through the title's
	tracker (sinden_track.h) into the GunCon (host::lightgunAim), with the
	gun's buttons.

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	Called once a frame on the frame's thread (host.cpp, before the GunCon's
	aim is moved), only while "Sinden Lightgun" is on and that player has the
	GunCon. The tracker is told the border's thickness and the picture's shape
	as ui.cpp draws them (ui::sindenPicture), so it does not have to guess
	them, and is made again when they change. A picture in which the screen is
	not found, or whose aim is off the picture, is an aim off the screen: a
	shot then reloads, as the GunCon's games expect.

	The buttons, by Sinden's defaults (its software sets them: the boot log's
	"sinden: buttons:" lines say what each press sends): the trigger (the
	first mouse or joystick button) fires; the second is the GunCon's A, the
	third its B. Keys the gun sends are not used yet.

	Every ten seconds of play the boot log has a "sinden: aim:" line: how many
	pictures came, in how many the screen was found, the median quality, the
	border's level in the camera (above about 230 it is clipped: the camera's
	exposure is too high), and the tracker's time.
*/
#include "fe.h"
#include "sinden.h"
#include "sinden_track.h"
#include "ui.h"

#include <libretro.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace fe::sinden
{
namespace
{
std::unique_ptr<track::Tracker> tracker;
float madeFor[3] = { -1, -1, -1 };	// the picture's aspect and the border's fractions the tracker was made for

// For the boot log's line every ten seconds.
struct Tally
{
	double since = -1;
	int pictures = 0, found = 0, offScreen = 0;
	double trackSeconds = 0, worstSeconds = 0;
	std::vector<float> quality, level;
	bool firstSaid = false;
} tally;

void sayTally(double now)
{
	Tally& t = tally;
	if (t.since < 0)
		t.since = now;
	if (now - t.since < 10.0)
		return;
	auto median = [](std::vector<float>& v) {
		if (v.empty())
			return 0.f;
		std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
		return v[v.size() / 2];
	};
	diag::mark("sinden: aim: %d pictures in %.0f s, the screen found in %d (%d off the picture); quality %.2f, the "
			"border's level %.0f of 255; the tracker %.2f ms a picture (worst %.2f)", t.pictures, now - t.since, t.found,
			t.offScreen, median(t.quality), median(t.level), t.pictures > 0 ? t.trackSeconds * 1000.0 / t.pictures : 0.0,
			t.worstSeconds * 1000.0);
	const bool first = t.firstSaid;
	t = Tally();
	t.since = now;
	t.firstSaid = first;
}
}

void aimFrame()
{
	const options::Frontend& f = options::frontend();
	if (f.sinden < 1 || f.sinden > 2)
		return;
	const int port = f.sinden - 1;
	if (f.controller[port] != 5 || state() == State::Off)
		return;

	// The gun's buttons, in the GunCon's (host.cpp's light gun: R2 fires, Y is
	// its A, X its B): the trigger fires, the second button is A, the third B.
	const Buttons b = buttons();
	const uint32_t pressed = b.mouse | b.joystick;
	uint16_t bits = 0;
	if ((pressed & 1u) != 0)
		bits |= (uint16_t)(1u << RETRO_DEVICE_ID_JOYPAD_R2);
	if ((pressed & 2u) != 0)
		bits |= (uint16_t)(1u << RETRO_DEVICE_ID_JOYPAD_Y);
	if ((pressed & 4u) != 0)
		bits |= (uint16_t)(1u << RETRO_DEVICE_ID_JOYPAD_X);
	host::lightgunButtons(port, bits);

	Frame frame;
	if (!latestFrame(frame) || frame.luma == nullptr)
		return;
	float aspect = 4.f / 3.f, borderX = 0, borderY = 0;
	ui::sindenPicture(aspect, borderX, borderY);
	if (!tracker || std::fabs(madeFor[0] - aspect) > 1e-3f || std::fabs(madeFor[1] - borderX) > 1e-4f
			|| std::fabs(madeFor[2] - borderY) > 1e-4f)
	{
		track::Options o;
		o.pictureAspect = aspect;
		o.borderX = borderX;
		o.borderY = borderY;
		tracker = std::make_unique<track::Tracker>(o);
		madeFor[0] = aspect;
		madeFor[1] = borderX;
		madeFor[2] = borderY;
		diag::mark("sinden: aim: the tracker made for a picture %.3f wide to its height, a border of %.4f x %.4f of it",
				aspect, borderX, borderY);
	}
	// The gun's own calibration (its serial port): hundredths of a per cent.
	track::Calibration c;
	const sinden::Calibration gun = calibration();
	if (gun.known && std::abs(gun.rawX - 10000) <= 9900 && std::abs(gun.rawY - 10000) <= 9900)
	{
		c.offsetX = gun.rawX - 10000;
		c.offsetY = gun.rawY - 10000;
	}
	const double began = now();
	const track::Result r = tracker->update(track::Frame{ frame.luma, frame.width, frame.height, frame.stride }, c,
			frame.time);
	const double took = now() - began;
	const bool off = !r.found || r.offScreen;
	host::lightgunAim(port, r.found ? r.x : 0.f, r.found ? r.y : 0.f, off);

	Tally& t = tally;
	t.pictures++;
	t.found += r.found ? 1 : 0;
	t.offScreen += r.found && r.offScreen ? 1 : 0;
	t.trackSeconds += took;
	t.worstSeconds = std::max(t.worstSeconds, took);
	if (r.found)
	{
		t.quality.push_back(r.quality);
		t.level.push_back(r.borderLevel);
		if (!t.firstSaid)
		{
			t.firstSaid = true;
			diag::mark("sinden: aim: the screen found first at picture %llu: aim %.3f, %.3f%s, quality %.2f, turned %.1f "
					"degrees, the border's level %.0f", (unsigned long long)frame.number, r.x, r.y,
					r.offScreen ? " (off the picture)" : "", r.quality, r.roll, r.borderLevel);
		}
	}
	sayTally(began);
}

}
