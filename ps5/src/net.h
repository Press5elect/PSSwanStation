/*
	SwanStation for PS5 - what the network sources share (smb.cpp, ftp.cpp).

	SPDX-License-Identifier: GPL-2.0-or-later

	Not for the rest of the frontend: fe.h's smb namespace is what that uses,
	for a game on an SMB share and on an FTP server alike.
*/
#pragma once

#include "fe.h"

#include <algorithm>
#include <cstring>
#include <netinet/in.h>

namespace fe::smb::net
{

constexpr int ProbeSeconds = 5;			// for a server's port to accept a connection
constexpr int RequestSeconds = 60;		// for each request: a NAS waking its disks
constexpr int RetrySeconds = 30;		// before a server that failed is tried again
constexpr size_t StreamBuffer = 256 * 1024;

long long nowMs();

// Around every request to a server: the screen says "waiting for the network
// share" when one takes long.
struct Busy
{
	Busy();
	~Busy();
	// The server answered and the next request follows at once.
	void answered();
};

// The server answers, but not with what was asked for (a folder that cannot be
// listed): said on the screen and in the log.
void note(const std::string& text);
// The server could not be reached, or stopped answering: counted, so that a
// scan knows its list is not whole.
void fail(const std::string& text);
// The load in progress was cancelled: nothing more is asked of a server.
bool stopping();

std::string stripped(std::string text);
std::string lastComponent(const std::string& path);
// False when `server` is a name: the console cannot look names up.
bool numericAddress(const std::string& server, int port, sockaddr_in& address);

// Whether the server accepts a connection on a port.
enum class Probe
{
	Open,
	Closed,		// refused, unreachable, or no answer in the time given
	Unknown,	// could not tell (no socket, an error that says nothing)
	Cancelled,
	NoName,		// the server is given by name, which the console cannot look up
};
// `what` is for the log: "smb", "ftp".
Probe probe(const std::string& server, int port, const char *what);

// A file read from a server as the game asks, through a buffer: what is
// read from the server is fetch()'s to say.
class StreamFile : public File
{
public:
	explicit StreamFile(uint64_t bytes) : bytes(bytes), buffer(StreamBuffer) {}

	size_t read(void *out, size_t want) override
	{
		uint8_t *to = static_cast<uint8_t *>(out);
		size_t done = 0;
		while (want > 0)
		{
			if (position >= bufferStart && position < bufferStart + bufferFill)
			{
				const size_t offset = (size_t)(position - bufferStart);
				const size_t n = std::min(want, bufferFill - offset);
				memcpy(to + done, buffer.data() + offset, n);
				done += n;
				want -= n;
				position += n;
				continue;
			}
			if (position >= bytes)
				break;
			// A large read goes straight to the caller; a small one fills the buffer.
			if (want >= buffer.size())
			{
				const int n = fetch(to + done, position, want);
				if (n <= 0)
					break;
				done += n;
				want -= n;
				position += n;
			}
			else
			{
				const int n = fetch(buffer.data(), position, buffer.size());
				if (n <= 0)
					break;
				bufferStart = position;
				bufferFill = (size_t)n;
			}
		}
		return done;
	}

	int64_t tell() override
	{
		return (int64_t)position;
	}

	int seek(int64_t offset, int whence) override
	{
		const int64_t target = whence == SEEK_SET ? offset
				: whence == SEEK_CUR ? (int64_t)position + offset : (int64_t)bytes + offset;
		if (target < 0)
			return -1;
		position = (uint64_t)target;
		return 0;
	}

	int64_t size() override
	{
		return (int64_t)bytes;
	}

	bool failed() override
	{
		return hasFailed;
	}

protected:
	// Reads up to `want` bytes at `offset`: how many, 0 at the end, -1 (and
	// hasFailed) when the server cannot be read.
	virtual int fetch(uint8_t *to, uint64_t offset, size_t want) = 0;

	uint64_t bytes;
	bool hasFailed = false;

private:
	uint64_t position = 0;
	std::vector<uint8_t> buffer;
	uint64_t bufferStart = 0;
	size_t bufferFill = 0;
};

}

// FTP servers (ftp.cpp), reached through fe.h's smb functions by their
// ftp:// paths.
namespace fe::ftp
{
bool isPath(const std::string& path);
// A "path" line of network.cfg that names an FTP folder,
// ftp://[user[:password]@]server[:port]/folder: remembers the account (the
// one in the line, else `user` and `password`; guest with no password logs
// in as anonymous) and gives the folder as ftp://server[:port]/folder, or ""
// when the line does not name one.
std::string addFolder(const std::string& value, const std::string& user, const std::string& password);
std::vector<smb::Entry> list(const std::string& path);
// 1 there, 0 not there, -1 the server cannot be reached.
int stat(const std::string& path, smb::Entry& entry);
// The file, to be read as asked, or nullptr.
smb::File *openStream(const std::string& path);
void retryNow();
}
