/*
	PSSwanStation - netplay's tests: the few things of the frontend that
	netplay.cpp uses, so that it builds without the rest of the title.

	SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "../../src/fe.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

namespace fe
{

const int BuildNumber = 9;

std::string format(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	va_list copy;
	va_copy(copy, args);
	const int length = vsnprintf(nullptr, 0, fmt, args);
	va_end(args);
	std::string text;
	if (length > 0)
	{
		text.resize((size_t)length + 1);
		vsnprintf(text.data(), text.size(), fmt, copy);
		text.resize((size_t)length);
	}
	va_end(copy);
	return text;
}

double now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

namespace diag
{
// The marks go to stderr when NETPLAY_TEST_LOG is set, with the time and
// NETPLAY_TEST_NAME in front.
void mark(const char *format, ...)
{
	static const char *const wanted = getenv("NETPLAY_TEST_LOG");
	if (wanted == nullptr)
		return;
	static const char *const name = getenv("NETPLAY_TEST_NAME");
	static std::mutex mutex;
	static const double started = now();
	char text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	std::lock_guard<std::mutex> lock(mutex);
	fprintf(stderr, "[%8.3f %s] %s\n", now() - started, name != nullptr ? name : "-", text);
}
}

}
