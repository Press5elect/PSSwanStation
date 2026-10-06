/*
	PSSwanStation - the memory card test's stand-ins for the frontend.

	SPDX-License-Identifier: GPL-3.0-or-later

	memcard.cpp uses four of fe.h's helpers. They are in main.cpp, which needs
	the whole frontend; these do the same, so the test builds from memcard.cpp
	alone.
*/
#include "fe.h"

#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace fe
{

std::string format(const char *fmt, ...)
{
	char text[1024];
	va_list args;
	va_start(args, fmt);
	const int length = vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	return length < 0 ? std::string(fmt) : std::string(text);
}

bool writeFile(const std::string& path, const void *data, size_t bytes)
{
	const std::string temporary = path + ".tmp";
	FILE *f = fopen(temporary.c_str(), "wb");
	if (f == nullptr)
		return false;
	const bool written = bytes == 0 || fwrite(data, 1, bytes, f) == bytes;
	const bool closed = fclose(f) == 0;
	if (!written || !closed || rename(temporary.c_str(), path.c_str()) != 0)
	{
		unlink(temporary.c_str());
		return false;
	}
	chmod(path.c_str(), 0666);
	return true;
}

std::string lowercase(std::string text)
{
	for (char& c : text)
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	return text;
}

std::string extension(const std::string& path)
{
	const size_t slash = path.find_last_of("/\\");
	const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
	const size_t dot = name.rfind('.');
	return dot == std::string::npos || dot == 0 ? "" : lowercase(name.substr(dot));
}

}
