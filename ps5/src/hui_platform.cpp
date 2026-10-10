/*
	PSSwanStation - the interface kit's system calls, on the title's own.

	The kit (PS5_VKHomebrewUI) logs, reads the clock and sleeps through five
	functions of its own (platform/ps5/system.hpp there). Here they are the
	title's boot log and clock. The title ends itself through
	platform::quit(), never through the kit, so park and quit only keep the
	kit's contract.

	SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "fe.h"

#include "platform/ps5/system.hpp"

#include <cstdarg>
#include <cstdio>
#include <unistd.h>

namespace hui::sys
{

std::int64_t monotonic_us()
{
	return (std::int64_t)(fe::now() * 1e6);
}

void log(const char *format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	fe::diag::mark("kit: %s", line);
}

bool hide_splash_screen()
{
	return true;	// the title hides it itself, after its first frame
}

void sleep_us(std::uint32_t microseconds)
{
	usleep(microseconds);
}

void park()
{
	for (;;)
		usleep(100000);
}

void quit()
{
	fe::platform::quit();
	park();
}

} // namespace hui::sys
