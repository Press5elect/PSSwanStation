/*
	PSSwanStation - the updater's test: the frontend's helpers the updater
	uses, as main.cpp and diag.cpp have them, so that update.cpp builds alone.

	SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "fe.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

// The marks made so far, for the test to read.
std::vector<std::string> testMarks();

namespace
{
std::mutex markMutex;
std::vector<std::string> marks;
}

std::vector<std::string> testMarks()
{
	std::lock_guard<std::mutex> lock(markMutex);
	return marks;
}

namespace fe
{

namespace diag
{
void mark(const char *format, ...)
{
	char line[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	std::lock_guard<std::mutex> lock(markMutex);
	marks.push_back(line);
	if (getenv("UPDATE_TEST_VERBOSE") != nullptr)
		fprintf(stderr, "    [mark] %s\n", line);
}
}

std::string format(const char *fmt, ...)
{
	char small[512];
	va_list args;
	va_start(args, fmt);
	va_list copy;
	va_copy(copy, args);
	const int length = vsnprintf(small, sizeof(small), fmt, args);
	va_end(args);
	std::string text;
	if (length < 0)
		text = fmt;
	else if ((size_t)length < sizeof(small))
		text.assign(small, (size_t)length);
	else
	{
		text.resize((size_t)length + 1);
		vsnprintf(text.data(), text.size(), fmt, copy);
		text.resize((size_t)length);
	}
	va_end(copy);
	return text;
}

bool fileExists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dirExists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void makeDir(const std::string& path)
{
	if (path.empty() || dirExists(path))
		return;
	std::string clean = path;
	while (clean.size() > 1 && clean.back() == '/')
		clean.pop_back();
	const size_t slash = clean.rfind('/');
	if (slash != std::string::npos && slash > 0)
		makeDir(clean.substr(0, slash));
	if (mkdir(clean.c_str(), 0777) == 0)
		chmod(clean.c_str(), 0777);
}

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
	out.clear();
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	uint8_t block[65536];
	size_t got;
	while ((got = fread(block, 1, sizeof(block), f)) > 0)
		out.insert(out.end(), block, block + got);
	const bool ok = ferror(f) == 0;
	fclose(f);
	return ok;
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

std::string trim(const std::string& text)
{
	size_t first = 0, last = text.size();
	while (first < last && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r' || text[first] == '\n'))
		first++;
	while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r'
			|| text[last - 1] == '\n'))
		last--;
	return text.substr(first, last - first);
}

double now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

}
