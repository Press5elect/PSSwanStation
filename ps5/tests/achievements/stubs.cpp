/*
	PSSwanStation - the achievements test: the frontend's helpers the module
	uses, without the frontend.

	SPDX-License-Identifier: GPL-3.0-or-later

	The helpers are main.cpp's, word for word where it matters (a file is
	written beside its place and renamed). diag::mark keeps every line, so the
	test can look for what must and must not be in the log.
*/
#include "fe.h"
#include "stubs.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
std::mutex logMutex;
std::vector<std::string> logLines;
}

std::vector<std::string> testLog()
{
	std::lock_guard<std::mutex> lock(logMutex);
	return logLines;
}

namespace fe
{

std::string rootDir;

void diag::mark(const char *format, ...)
{
	char text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	std::lock_guard<std::mutex> lock(logMutex);
	logLines.push_back(text);
	if (getenv("TEST_VERBOSE") != nullptr)
		fprintf(stderr, "    [log] %s\n", text);
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
	uint8_t chunk[65536];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
		out.insert(out.end(), chunk, chunk + n);
	fclose(f);
	return true;
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

std::string baseName(const std::string& path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

double now()
{
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

}
