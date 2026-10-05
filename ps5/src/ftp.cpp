/*
	PSSwanStation - games on an FTP server.

	SPDX-License-Identifier: GPL-3.0-or-later

	The second kind of network source, beside the SMB share (smb.cpp, through
	whose functions the rest of the title reaches this one). network.cfg names
	the folder:

		path = ftp://192.168.1.10/games/psx
		path = ftp://user:password@192.168.1.10:2121/games

	The idea, and how a file is read, come from the FTP source of the earlier
	DuckStation build for this console, which only spoke to the console's own
	FTP server on 127.0.0.1 as anonymous; this one speaks to any server, with an
	account.

	Plain FTP, passive mode, binary. Each connection is a control connection
	on which commands are sent and three-digit replies read; a listing and a
	file's bytes come on a second, data connection the server names the port
	of (PASV, or EPSV where a server has no PASV), always on the server's own
	address.

	A folder is listed with MLSD, which every field of is defined, and with
	LIST where the server has no MLSD (vsftpd): the listing a Unix "ls -l"
	prints, or the one a Windows server does.

	A file is one RETR, read as a stream. FTP has no "read here": a read
	somewhere else ends the transfer and starts another with REST, except a
	little way ahead, which is reached by reading on. So a file read into
	memory before the game starts ("Load network games into memory", the
	default) is one transfer from its first byte to its last, and a file read
	as the game asks is one transfer for as long as the game reads on, with
	smb.cpp's 256 KiB buffer in front of it. Each open file has a control
	connection of its own, made when it is first read; listings and questions
	about a path share one for each server.

	Sockets stay blocking, as smb.cpp's do and for its reason; every read asks
	poll first, so a server that stops answering costs a time limit and not
	the title. The password travels as FTP sends it: in the clear, on the
	local network.
*/
#include "net.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace fe::ftp
{
namespace
{
using namespace smb::net;
using u8 = uint8_t;
using u64 = uint64_t;
using s64 = int64_t;

constexpr int ReplySeconds = 20;		// for a reply to a command
constexpr int AbortSeconds = 3;			// for the server's word on a transfer that was cut short
// A read this far ahead of the stream is reached by reading on.
constexpr s64 SkipLimit = 512 * 1024;

// A server and the account for it, from network.cfg.
struct Server
{
	std::string host;
	int port = 21;
	std::string user = "anonymous", password = "anonymous@";

	std::string name() const
	{
		return port == 21 ? host : host + ":" + std::to_string(port);
	}
};

// Waits until the socket has something to read: 1, 0 when the time is up,
// -1 when the load was cancelled or the socket is in error.
int waitReadable(int fd, int seconds)
{
	const long long deadline = nowMs() + seconds * 1000ll;
	for (;;)
	{
		if (stopping())
			return -1;
		pollfd ready{};
		ready.fd = fd;
		ready.events = POLLIN;
		const int state = poll(&ready, 1, 200);
		if (state > 0)
			return 1;
		if (state < 0 && errno != EINTR)
			return -1;
		if (nowMs() >= deadline)
			return 0;
	}
}

// Up to `length` bytes: how many, 0 at the end of the stream, -1 for an
// error or no data in the time given.
ssize_t receive(int fd, void *to, size_t length, int seconds)
{
	if (waitReadable(fd, seconds) != 1)
		return -1;
	for (;;)
	{
		const ssize_t got = recv(fd, to, length, 0);
		if (got < 0 && errno == EINTR)
			continue;
		return got;
	}
}

// A connection to the server's port. The control port was asked first on a
// thread of its own (probe); here the connect blocks.
int connectTo(const std::string& host, int port)
{
	sockaddr_in address;
	if (!numericAddress(host, port, address))
		return -1;
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	// A last resort, should a read ever be made that poll did not allow.
	const timeval limit = { RequestSeconds, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
	if (connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
	{
		close(fd);
		return -1;
	}
	return fd;
}

// One control connection.
class Link
{
public:
	~Link()
	{
		shut();
	}

	bool isOpen() const
	{
		return control >= 0;
	}

	const std::string& lastReply() const
	{
		return reply;
	}

	// Connects and logs in. On failure `error` says why, for the screen.
	bool open(const Server& server, std::string& error)
	{
		shut();
		host = server.host;
		const Probe answer = probe(server.host, server.port, "ftp");
		if (answer == Probe::Cancelled)
			return false;
		if (answer == Probe::NoName)
		{
			error = "\"" + server.host + "\" is a name: give the FTP server's IP address in network.cfg (192.168.x.x)";
			return false;
		}
		if (answer == Probe::Closed || (control = connectTo(server.host, server.port)) < 0)
		{
			error = server.name() + " does not answer (FTP): is it switched on, and are the address and the port "
					"in network.cfg right?";
			return false;
		}
		const int one = 1;
		setsockopt(control, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		int code = readReply();
		while (code == 120)		// "service ready in a moment"
			code = readReply();
		if (code == 220)
			code = command("USER %s", server.user.c_str());
		if (code == 331)
			code = command("PASS %s", server.password.c_str());
		if (code != 230 && code != 202)
		{
			error = "ftp://" + server.name() + ": the login as " + server.user + " was refused"
					+ (reply.empty() ? std::string() : " (" + reply + ")") + ": check the user and the password in network.cfg";
			shut();
			return false;
		}
		// Names as UTF-8 where the server needs telling; bytes as they are.
		command("OPTS UTF8 ON");
		if (command("TYPE I") != 200)
		{
			error = "ftp://" + server.name() + " has no binary transfers (" + reply + ")";
			shut();
			return false;
		}
		return true;
	}

	void shut()
	{
		if (control >= 0)
			close(control);
		control = -1;
	}

	// Sends one command and gives the reply's code, or -1 when the connection
	// failed (it is then shut).
	int command(const char *format, ...) __attribute__((format(printf, 2, 3)))
	{
		if (control < 0)
			return -1;
		char text[1400];
		va_list args;
		va_start(args, format);
		const int length = vsnprintf(text, sizeof(text) - 2, format, args);
		va_end(args);
		// A name with a line break in it would be a second command.
		if (length < 0 || (size_t)length >= sizeof(text) - 2 || memchr(text, '\n', (size_t)length) != nullptr
				|| memchr(text, '\r', (size_t)length) != nullptr)
			return 0;
		memcpy(text + length, "\r\n", 2);
		// Anything the server said that nobody asked for (a second word on a
		// transfer that was cut short) is not this command's reply.
		for (;;)
		{
			pollfd waiting{};
			waiting.fd = control;
			waiting.events = POLLIN;
			if (poll(&waiting, 1, 0) <= 0)
				break;
			if (readReply(1) < 0)
				return -1;
		}
		size_t sent = 0;
		while (sent < (size_t)length + 2)
		{
			const ssize_t n = send(control, text + sent, (size_t)length + 2 - sent, 0);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
			{
				shut();
				return -1;
			}
			sent += (size_t)n;
		}
		return readReply();
	}

	// One reply, of one line or of several ("123-" up to "123 ").
	int readReply(int seconds = ReplySeconds)
	{
		std::string line;
		if (!readLine(line, seconds) || line.size() < 3)
		{
			shut();
			return -1;
		}
		const int code = atoi(line.c_str());
		if (line.size() > 3 && line[3] == '-')
		{
			const std::string end = line.substr(0, 3) + " ";
			do
			{
				if (!readLine(line, seconds))
				{
					shut();
					return -1;
				}
			} while (line.compare(0, 4, end) != 0);
		}
		reply = line;
		return code;
	}

	// A data connection with `request` started on it, or -1: lastReply() then
	// says what the server made of the request, if the connection is still open.
	int openData(const std::string& request)
	{
		int port = 0;
		if (!extended)
		{
			int code = command("PASV");
			if (code == 226 || code == 426)		// the end of a transfer before this one
				code = readReply();
			unsigned h[4], p[2];
			const char *numbers = code == 227 ? strchr(reply.c_str(), '(') : nullptr;
			// Some servers leave the brackets out.
			if (code == 227 && numbers == nullptr)
				for (const char *c = reply.c_str() + 4; *c != 0 && numbers == nullptr; c++)
					if (*c >= '0' && *c <= '9')
						numbers = c - 1;
			if (numbers != nullptr && sscanf(numbers + 1, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3], &p[0], &p[1]) == 6)
				port = (int)(p[0] * 256 + p[1]);
			else if (code >= 500 && code < 600)
				extended = true;
			else
				return -1;
		}
		if (extended)
		{
			unsigned number = 0;
			const char *bars = command("EPSV") == 229 ? strstr(reply.c_str(), "(|||") : nullptr;
			if (bars == nullptr || sscanf(bars, "(|||%u|", &number) != 1)
				return -1;
			port = (int)number;
		}
		// The server names any of its addresses, and behind a router the wrong
		// one: its port is on the address the control connection reached.
		const int data = connectTo(host, port);
		if (data < 0)
			return -1;
		const int code = command("%s", request.c_str());
		if (code != 150 && code != 125)
		{
			close(data);
			return -1;
		}
		const int size = 1 << 20;
		setsockopt(data, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
		return data;
	}

private:
	bool readLine(std::string& line, int seconds)
	{
		line.clear();
		for (;;)
		{
			if (inAt >= inFill)
			{
				const ssize_t got = receive(control, in, sizeof(in), seconds);
				if (got <= 0)
					return false;
				inAt = 0;
				inFill = (size_t)got;
			}
			const char c = in[inAt++];
			if (c == '\n')
				return true;
			if (c != '\r' && line.size() < 1024)
				line.push_back(c);
		}
	}

	int control = -1;
	std::string host;
	std::string reply;
	bool extended = false;		// the server has EPSV and no PASV
	char in[512];
	size_t inAt = 0, inFill = 0;
};

// What is kept for each server: its account, and the connection listings
// and questions about paths go over, one at a time.
struct Site
{
	Server server;
	std::mutex mutex;
	Link link;
	long long failedAt = 0;		// nowMs() of the last failure to connect; 0 for none
	bool noMlsd = false, noSize = false;

	// With the mutex held.
	bool connect()
	{
		if (link.isOpen())
			return true;
		if (stopping() || (failedAt != 0 && nowMs() - failedAt < RetrySeconds * 1000))
			return false;
		std::string error;
		if (!link.open(server, error))
		{
			if (!error.empty())
				fail(error);
			if (!stopping())
				failedAt = nowMs();
			return false;
		}
		diag::mark("ftp: connected to %s as %s", server.name().c_str(), server.user.c_str());
		failedAt = 0;
		return true;
	}
};

std::mutex sitesMutex;
std::map<std::string, std::unique_ptr<Site>> sites;		// by server[:port]

// ftp://server[:port]/a/b -> the server and "/a/b".
Site *locate(const std::string& path, std::string& remote)
{
	if (!isPath(path))
		return nullptr;
	const size_t slash = path.find('/', 6);
	const std::string name = path.substr(6, slash == std::string::npos ? std::string::npos : slash - 6);
	remote = slash == std::string::npos ? "/" : path.substr(slash);
	while (remote.size() > 1 && remote.back() == '/')
		remote.pop_back();
	std::lock_guard<std::mutex> lock(sitesMutex);
	const auto it = sites.find(name);
	return it == sites.end() ? nullptr : it->second.get();
}

bool sameWord(const std::string& a, const char *b)
{
	return lowercase(a) == b;
}

// "type=dir;size=0;modify=20260101120000; name" (RFC 3659).
bool parseMlsd(const std::string& line, smb::Entry& entry)
{
	const size_t space = line.find(' ');
	if (space == std::string::npos || space + 1 >= line.size())
		return false;
	std::string type;
	u64 size = 0;
	size_t at = 0;
	while (at < space)
	{
		size_t end = line.find(';', at);
		if (end == std::string::npos || end > space)
			end = space;
		const std::string fact = line.substr(at, end - at);
		const size_t equals = fact.find('=');
		if (equals != std::string::npos)
		{
			const std::string key = fact.substr(0, equals), value = fact.substr(equals + 1);
			if (sameWord(key, "type"))
				type = lowercase(value);
			else if (sameWord(key, "size"))
				size = strtoull(value.c_str(), nullptr, 10);
		}
		at = end + 1;
	}
	// "cdir" and "pdir" are the folder itself and the one above it.
	if (type != "file" && type != "dir")
		return false;
	entry.name = line.substr(space + 1);
	entry.directory = type == "dir";
	entry.size = entry.directory ? 0 : size;
	return entry.name != "." && entry.name != "..";
}

// What a link in a Unix listing most likely is: a file when its name ends
// like one.
bool looksLikeFile(const std::string& name)
{
	const size_t dot = name.find_last_of('.');
	return dot != std::string::npos && dot != 0 && name.size() - dot <= 7;
}

// "drwxr-xr-x 1 owner group 65536 Jan 23 11:44 name", or a Windows server's
// "01-23-26  11:44AM       <DIR>          name".
bool parseList(const std::string& line, smb::Entry& entry)
{
	if (line.empty())
		return false;
	std::vector<std::string> fields;
	size_t at = 0;
	const auto field = [&]() {
		while (at < line.size() && line[at] == ' ')
			at++;
		const size_t start = at;
		while (at < line.size() && line[at] != ' ')
			at++;
		fields.push_back(line.substr(start, at - start));
		return at > start;
	};
	const bool windows = line[0] >= '0' && line[0] <= '9';
	if (!windows && line[0] != '-' && line[0] != 'd' && line[0] != 'l')
		return false;
	for (int i = 0; i < (windows ? 3 : 8); i++)
		if (!field())
			return false;
	while (at < line.size() && line[at] == ' ')
		at++;
	if (at >= line.size())
		return false;
	std::string name = line.substr(at);
	if (windows)
	{
		entry.directory = sameWord(fields[2], "<dir>");
		entry.size = entry.directory ? 0 : strtoull(fields[2].c_str(), nullptr, 10);
	}
	else
	{
		if (line[0] == 'l')
		{
			const size_t arrow = name.find(" -> ");
			if (arrow != std::string::npos)
				name.erase(arrow);
		}
		entry.directory = line[0] == 'd' || (line[0] == 'l' && !looksLikeFile(name));
		entry.size = entry.directory ? 0 : strtoull(fields[4].c_str(), nullptr, 10);
	}
	entry.name = std::move(name);
	return entry.name != "." && entry.name != "..";
}

// Reads a listing to its end from the data connection, which it closes.
bool readListing(Link& link, int data, bool mlsd, const std::string& base, std::vector<smb::Entry>& entries, Busy& busy)
{
	std::string line;
	char block[8192];
	ssize_t got;
	const auto finish = [&]() {
		smb::Entry entry;
		if (mlsd ? parseMlsd(line, entry) : parseList(line, entry))
		{
			entry.path = base + "/" + entry.name;
			entries.push_back(std::move(entry));
		}
		line.clear();
	};
	while ((got = receive(data, block, sizeof(block), RequestSeconds)) > 0)
	{
		busy.answered();
		for (ssize_t i = 0; i < got; i++)
		{
			if (block[i] == '\n')
				finish();
			else if (block[i] != '\r' && line.size() < 2048)
				line.push_back(block[i]);
		}
	}
	if (!line.empty())
		finish();
	close(data);
	if (got < 0)
	{
		link.shut();
		return false;
	}
	const int code = link.readReply();
	return code == 226 || code == 250;
}

// ---------------------------------------------- a file read from the server

class FtpFile : public StreamFile
{
public:
	FtpFile(const Server& server, std::string remote, u64 bytes)
		: StreamFile(bytes), server(server), remote(std::move(remote))
	{
	}

	~FtpFile() override
	{
		if (data >= 0)
			close(data);
		// No goodbye: the server is told by the connection closing.
	}

protected:
	int fetch(u8 *to, u64 offset, size_t want) override
	{
		Busy busy;
		std::string error = "the transfer stopped";
		for (int attempt = 0; attempt < 2 && !stopping(); attempt++)
		{
			if (!place((s64)offset, error))
			{
				dropAll();
				continue;
			}
			size_t total = 0;
			ssize_t got = 0;
			while (total < want)
			{
				got = receive(data, to + total, want - total, RequestSeconds);
				if (got <= 0)
					break;
				total += (size_t)got;
				streamAt += got;
				busy.answered();
			}
			if (total > 0)
				return (int)total;
			if (got == 0 && (u64)streamAt >= bytes)
				return 0;
			// The stream ended early or broke: once more, from here.
			dropAll();
			if (attempt == 0 && !stopping())
				diag::mark("ftp: reading %s: the transfer stopped; connecting again", lastComponent(remote).c_str());
		}
		if (!stopping())
			fail("reading " + lastComponent(remote) + ": " + error);
		hasFailed = true;
		return -1;
	}

private:
	// Brings the stream to `offset`.
	bool place(s64 offset, std::string& error)
	{
		if (data >= 0 && offset >= streamAt && offset - streamAt <= SkipLimit)
		{
			char skipped[16384];
			while (streamAt < offset)
			{
				const ssize_t got = receive(data, skipped, (size_t)std::min<s64>(offset - streamAt, sizeof(skipped)),
						RequestSeconds);
				if (got <= 0)
					return false;
				streamAt += got;
			}
			return true;
		}
		endTransfer();
		if (!link.isOpen())
		{
			std::string why;
			if (!link.open(server, why))
			{
				if (!why.empty())
					error = why;
				return false;
			}
		}
		if (offset != 0)
		{
			int code = link.command("REST %lld", (long long)offset);
			if (code == 226 || code == 426)		// the end of the transfer before this one
				code = link.readReply();
			if (code != 350)
			{
				error = "the server cannot start a file in the middle (" + link.lastReply() + "): switch \"Load network "
						"games into memory\" on";
				return false;
			}
		}
		data = link.openData("RETR " + remote);
		if (data < 0)
		{
			if (link.isOpen())
				error = link.lastReply();
			return false;
		}
		streamAt = offset;
		return true;
	}

	// Ends the transfer in progress and takes the server's word on it (226,
	// or 426 for one cut short), so the next command's reply is its own.
	void endTransfer()
	{
		if (data < 0)
			return;
		close(data);
		data = -1;
		if (!link.isOpen())
			return;
		const int code = link.readReply(AbortSeconds);
		if (code != 226 && code != 250 && code != 426 && code != 451)
			link.shut();
	}

	void dropAll()
	{
		if (data >= 0)
			close(data);
		data = -1;
		link.shut();
	}

	const Server server;
	const std::string remote;
	Link link;
	int data = -1;
	s64 streamAt = 0;
};

// A file's size by the listing of its folder, for servers with no SIZE:
// 1 and `entry`, 0 not there, -1 the server cannot be reached.
int statByListing(const std::string& path, smb::Entry& entry)
{
	const size_t slash = path.find_last_of('/');
	if (slash == std::string::npos || slash < 6)
		return 0;
	const unsigned before = smb::failures();
	for (smb::Entry& candidate : list(path.substr(0, slash)))
		if (candidate.name == path.substr(slash + 1))
		{
			entry = std::move(candidate);
			return 1;
		}
	return smb::failures() != before ? -1 : 0;
}

} // namespace

bool isPath(const std::string& path)
{
	return path.rfind("ftp://", 0) == 0;
}

std::string addFolder(const std::string& value, const std::string& user, const std::string& password)
{
	if (lowercase(value.substr(0, 6)) != "ftp://")
		return "";
	std::string rest = value.substr(6);
	std::string folder;
	const size_t slash = rest.find('/');
	if (slash != std::string::npos)
	{
		folder = rest.substr(slash);
		rest = rest.substr(0, slash);
	}
	while (!folder.empty() && folder.back() == '/')
		folder.pop_back();
	Server server;
	// The account in the line, else network.cfg's; a guest is "anonymous".
	const size_t at = rest.rfind('@');
	if (at != std::string::npos)
	{
		const std::string who = rest.substr(0, at);
		const size_t colon = who.find(':');
		server.user = who.substr(0, colon);
		server.password = colon == std::string::npos ? "" : who.substr(colon + 1);
		rest = rest.substr(at + 1);
	}
	else if (!user.empty() && !(user == "guest" && password.empty()))
	{
		server.user = user;
		server.password = password;
	}
	const size_t colon = rest.find(':');
	if (colon != std::string::npos)
	{
		server.port = atoi(rest.c_str() + colon + 1);
		rest = rest.substr(0, colon);
	}
	if (rest.empty() || server.user.empty() || server.port <= 0 || server.port > 65535)
		return "";
	server.host = rest;
	const std::string name = server.name();
	std::lock_guard<std::mutex> lock(sitesMutex);
	std::unique_ptr<Site>& site = sites[name];
	if (!site)
		site = std::make_unique<Site>();
	site->server = server;
	return "ftp://" + name + folder;
}

std::vector<smb::Entry> list(const std::string& path)
{
	std::vector<smb::Entry> entries;
	std::string remote;
	Site *site = locate(path, remote);
	if (site == nullptr)
		return entries;
	std::string base = path;
	while (!base.empty() && base.back() == '/')
		base.pop_back();
	std::lock_guard<std::mutex> lock(site->mutex);
	Busy busy;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		entries.clear();
		if (!site->connect())
			break;
		// The folder is gone to first and listed with no name: a name with
		// spaces, or one that begins with a dash, is then nobody's problem.
		int code = site->link.command("CWD %s", remote.c_str());
		if (code >= 500 && code < 600)
		{
			note("cannot list " + path.substr(6) + ": " + site->link.lastReply());
			break;
		}
		int data = code == 250 ? site->link.openData(site->noMlsd ? "LIST" : "MLSD") : -1;
		if (data < 0 && !site->noMlsd && site->link.isOpen() && atoi(site->link.lastReply().c_str()) >= 500)
		{
			// No MLSD here.
			site->noMlsd = true;
			diag::mark("ftp: %s has no MLSD: folders are listed with LIST", site->server.name().c_str());
			data = site->link.openData("LIST");
		}
		if (data >= 0 && readListing(site->link, data, !site->noMlsd, base, entries, busy))
			break;
		// The connection had been closed by the server, or broke: once more.
		const std::string said = site->link.isOpen() ? site->link.lastReply() : "the connection was closed";
		site->link.shut();
		if (attempt == 1 && !stopping())
		{
			entries.clear();
			fail("ftp://" + site->server.name() + " stopped answering: " + said);
		}
	}
	return entries;
}

int stat(const std::string& path, smb::Entry& entry)
{
	std::string remote;
	Site *site = locate(path, remote);
	if (site == nullptr)
		return 0;
	bool byListing = false;
	{
		std::lock_guard<std::mutex> lock(site->mutex);
		Busy busy;
		for (int attempt = 0; attempt < 2 && !byListing; attempt++)
		{
			if (!site->connect())
				return -1;
			entry.name = lastComponent(path);
			entry.path = path;
			int code = site->noSize ? 0 : site->link.command("SIZE %s", remote.c_str());
			if (code == 213 && site->link.lastReply().size() > 4)
			{
				entry.directory = false;
				entry.size = strtoull(site->link.lastReply().c_str() + 4, nullptr, 10);
				return 1;
			}
			if (code == 500 || code == 502)
			{
				site->noSize = true;
				diag::mark("ftp: %s has no SIZE: a file's size is read from its folder's listing",
						site->server.name().c_str());
			}
			if (code >= 0)
			{
				// Not a file the server will size: a folder, or not there.
				code = site->link.command("CWD %s", remote.c_str());
				if (code == 250)
				{
					entry.directory = true;
					entry.size = 0;
					return 1;
				}
				if (code >= 0)
				{
					if (!site->noSize)
						return 0;
					byListing = true;
				}
			}
			// A connection the server had closed: once more on a new one.
			if (code < 0 && attempt == 1 && !stopping())
				fail("ftp://" + site->server.name() + " stopped answering");
		}
	}
	return byListing ? statByListing(path, entry) : -1;
}

smb::File *openStream(const std::string& path)
{
	std::string remote;
	Site *site = locate(path, remote);
	smb::Entry entry;
	if (site == nullptr || ftp::stat(path, entry) != 1 || entry.directory)
		return nullptr;
	return new FtpFile(site->server, remote, entry.size);
}

void retryNow()
{
	std::lock_guard<std::mutex> lock(sitesMutex);
	for (auto& [name, site] : sites)
	{
		// Only a server nobody is asking: failedAt is the connection's own.
		std::unique_lock<std::mutex> own(site->mutex, std::try_to_lock);
		if (own.owns_lock())
			site->failedAt = 0;
	}
}

}
