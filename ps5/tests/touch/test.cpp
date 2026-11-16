// PSSwanStation: which half of the touch pad a click is on (src/touch_click.h),
// the cases of Crux2's touch_click_test.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
#include "touch_click.h"

#include <cstdio>

static int failures = 0, checks = 0;
static void check(bool ok, const char *what)
{
	checks++;
	if (!ok)
	{
		failures++;
		std::printf("FAIL %s\n", what);
	}
}

int main()
{
	using fe::touch::Click;
	{
		Click c;
		uint16_t x[1] = { 300 };
		uint8_t id[1] = { 1 };
		c.touches(1, x, id, 1000);
		check(c.leftHalf(1000), "one finger on the left: Select");
	}
	{
		Click c;
		uint16_t x[1] = { 1500 };
		uint8_t id[1] = { 1 };
		c.touches(1, x, id, 1000);
		check(!c.leftHalf(1000), "one finger on the right: Start");
	}
	{
		// A thumb resting on the left, then a finger clicks on the right: the
		// right finger is second in the report but touched last.
		Click c;
		uint16_t x1[1] = { 200 };
		uint8_t id1[1] = { 7 };
		c.touches(1, x1, id1, 1000);
		uint16_t x2[2] = { 200, 1700 };
		uint8_t id2[2] = { 7, 8 };
		c.touches(2, x2, id2, 2000);
		check(!c.leftHalf(2000), "thumb resting left, click right: Start");
	}
	{
		Click c;
		uint16_t x1[1] = { 1700 };
		uint8_t id1[1] = { 3 };
		c.touches(1, x1, id1, 1000);
		uint16_t x2[2] = { 1700, 100 };
		uint8_t id2[2] = { 3, 4 };
		c.touches(2, x2, id2, 2000);
		check(c.leftHalf(2000), "thumb resting right, click left: Select");
	}
	{
		// The report may put the newer finger first.
		Click c;
		uint16_t x1[1] = { 1700 };
		uint8_t id1[1] = { 3 };
		c.touches(1, x1, id1, 1000);
		uint16_t x2[2] = { 100, 1700 };
		uint8_t id2[2] = { 4, 3 };
		c.touches(2, x2, id2, 2000);
		check(c.leftHalf(2000), "newer finger listed first: still the newer one");
	}
	{
		// The click arrives in a reading with no finger: the one seen 50 ms before.
		Click c;
		uint16_t x[1] = { 400 };
		uint8_t id[1] = { 1 };
		c.touches(1, x, id, 1000000);
		c.touches(0, x, id, 1050000);
		check(c.leftHalf(1050000), "no finger in the click's reading: the one seen 50 ms before");
		check(!c.leftHalf(1300000), "a finger seen 300 ms before no longer counts: Start");
	}
	{
		Click c;
		check(!c.leftHalf(5), "no finger ever: Start, as before");
	}
	{
		// The resting thumb lifts; the finger that stays is the newest.
		Click c;
		uint16_t x1[1] = { 200 };
		uint8_t id1[1] = { 1 };
		c.touches(1, x1, id1, 1000);
		uint16_t x2[2] = { 200, 1600 };
		uint8_t id2[2] = { 1, 2 };
		c.touches(2, x2, id2, 2000);
		uint16_t x3[1] = { 1600 };
		uint8_t id3[1] = { 2 };
		c.touches(1, x3, id3, 3000);
		check(!c.leftHalf(3000), "thumb lifted, the other finger stays: Start");
		// A new touch on the left with the same id the thumb had.
		uint16_t x4[2] = { 1600, 300 };
		uint8_t id4[2] = { 2, 1 };
		c.touches(2, x4, id4, 4000);
		check(c.leftHalf(4000), "a new finger reusing an old id is the newest: Select");
	}
	{
		Click c;
		uint16_t x[1] = { 959 };
		uint8_t id[1] = { 1 };
		c.touches(1, x, id, 1);
		check(c.leftHalf(1), "959 is the left half");
		uint16_t y[1] = { 960 };
		c.touches(1, y, id, 2);
		check(!c.leftHalf(2), "960 is the right half");
	}
	{
		Click c;
		uint16_t x[2] = { 100, 1800 };
		uint8_t id[2] = { 1, 2 };
		c.touches(2, x, id, 1);
		uint16_t nx = 0;
		check(c.newestX(nx) && nx == 1800, "two fingers at once: the second in the report is the newer");
	}
	std::printf("touch: %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
