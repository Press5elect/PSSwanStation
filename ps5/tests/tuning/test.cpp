// PSSwanStation - the console tuning's arithmetic (src/tuning.h), on a PC.
//
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/tuning.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace
{
int failures = 0;

void check(bool ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	failures += ok ? 0 : 1;
}
}

int main()
{
	using namespace fe::tuning;

	check(priorityOf(Role::Sound) < priorityOf(Role::Input) && priorityOf(Role::Input) < priorityOf(Role::Frame)
			&& priorityOf(Role::Frame) < priorityOf(Role::Background), "sound, controllers, frame, background in order");
	check(priorityOf(Role::Frame) == 700, "the frame's thread stays where every thread starts");
	check(spinMicroseconds(0) == 1500 && sleepMicroseconds(0) == 1000, "the first choices are the driver's own");
	check(spinMicroseconds(9) == 3000 && sleepMicroseconds(-1) == 1000, "choices out of range are clamped");

	{
		Behind b;
		check(!b.note(0.96) && !b.behind, "one window under 97 % is not behind");
		check(!b.note(0.99) && !b.behind, "a good window starts the count again");
		b.note(0.96);
		check(b.note(0.90) && b.behind && b.timesBehind == 1, "two windows in a row under 97 % are behind");
		bool back = false;
		for (int i = 0; i < 19; i++)
			back = back || b.note(1.0);
		check(!back && b.behind, "nine and a half seconds at full speed is not yet enough");
		b.note(0.985);
		for (int i = 0; i < 19; i++)
			back = back || b.note(1.0);
		check(!back && b.behind, "a window under 99 % starts the ten seconds again");
		check(b.note(1.0) && !b.behind, "ten seconds at 99 % or more: keeping up again");
	}

	{
		Session s;
		bool said = false;
		for (int i = 0; i < 3; i++)
			said = said || s.note(0.5, 0.90);
		check(!said, "a second and a half under 95 % says nothing");
		check(s.note(0.5, 0.90), "two seconds under 95 %: a slow: line");
		check(!s.note(0.5, 0.90), "one line a stretch");
		s.note(0.5, 1.0);
		check(std::fabs(s.longest - 2.5) < 1e-9 && std::fabs(s.under - 2.5) < 1e-9, "the stretch and the time under");
		check(std::fabs(s.averageSpeed() - (2.5 * 0.9 + 0.5) / 3.0) < 1e-9, "the average speed is by time");
		unsigned lines = 1;
		for (int stretch = 0; stretch < 40; stretch++)
		{
			for (int i = 0; i < 4; i++)
				lines += s.note(0.5, 0.5) ? 1 : 0;
			s.note(0.5, 1.0);
		}
		check(lines == Session::MostSlowLines, "at most 30 slow: lines a session");
	}

	{
		Window w;
		const double a[Parts + 1] = { 0.0, 0.001, 0.010, 0.020, 0.021, 0.023, 0.030 };
		const double b[Parts + 1] = { 1.0, 1.001, 1.002, 1.003, 1.004, 1.005, 1.006 };
		w.frame(a, 0.025);
		w.frame(b, 0.025);
		w.counts[Draws] = 300;
		check(w.frames == 2 && w.overLimit == 1, "frames over a refresh and a half are counted");
		check(std::fabs(w.worst - 0.030) < 1e-9, "the worst frame");
		check(std::fabs(w.average(2) - 0.0055) < 1e-9, "a part's average");
		check(w.perFrame(Draws) == 150, "counts per frame");
	}

	{
		// A thread's work at the background priority, with arguments and a
		// move-only job; with no setThread linked in (as here) it just runs.
		int got = 0;
		std::thread t(background([&got](int a, int b) { got = a + b; }), 2, 3);
		t.join();
		auto job = std::make_unique<int>(7);
		int moved = 0;
		std::thread u(background([&moved, j = std::move(job)] { moved = *j; }));
		u.join();
		check(got == 5 && moved == 7, "background() runs the work with its arguments");
	}

	printf("%s\n", failures == 0 ? "all passed" : "FAILED");
	return failures == 0 ? 0 : 1;
}
