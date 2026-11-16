/*
	PSSwanStation - games on a network share (SMB), and what every
	network source goes through (an FTP server is ftp.cpp's, an NFS share
	nfs.cpp's).

	Copyright 2026 the PSFlyCast contributors (PSFlyCast, shell/ps5/ps5_smb.cpp)
	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-2.0-or-later

	PSFlyCast's network code, with the emulator's side changed: SwanStation
	reads every game file through libretro's VFS, and its hybrid VFS sends a
	URI-shaped path to the frontend (vfs.cpp), which opens

		smb://server/share/folder/file

	here: SMB 2/3 through libsmb2 (github.com/sahlberg/libsmb2, LGPL-2.1, built
	into the title). <root>network.cfg names the folders to scan and the
	account to use:

		path = 192.168.1.10/Games/PSX
		user = guest
		password =

	Settings' "Network shares" page writes the file too (saveConfig, below),
	and finds the SMB servers on the console's network for it (findServers).

	Sockets. On the console a title's socket did not behave as a non-blocking
	one after fcntl(F_SETFL, O_NONBLOCK): libsmb2, which reads until the socket
	says "nothing more", waited for ever on the server's first answer. With the
	socket left blocking on purpose PSFlyCast connected, listed the share and
	read games at 69 MB/s on a console. So libsmb2 is built that way
	(ps5/smb/compat.c): its reads ask poll first whether there is anything to
	read, and a connect is made only after the server's port has answered a
	connection tried on a thread of its own, which the caller waits for five
	seconds.

	The connection. One per share, made the first time it is needed and shared
	by every thread, one request at a time (a libsmb2 context is not
	thread-safe). A server that is off or a wrong address fails after five
	seconds. A server that is on gets a minute for each request, because a NAS
	that let its disks sleep answers the first one only when they are spinning
	again. A share that could not be reached is tried again after 30 seconds,
	or at once when the user asks. A request that fails on a connection that is
	gone is made once more on a new one. A game started while its server does
	not answer waits for it, asking each second (awaitShare), as OGBox does.

	A game's files, two ways (the "Load network games into memory" option):

	  in memory   before a game starts, precache() reads its files whole into
	              memory on a thread of its own, in 16 MiB blocks from the
	              title's heap, while the screen shows how far it is; the
	              emulator then opens them from memory and the share is not
	              touched again: the NAS can sleep while the game runs.
	  streamed    the file is read from the share as the game asks, through a
	              256 KiB buffer. A request that fails is made once more on a
	              new connection.
*/
#include "fe.h"
#include "net.h"
#include "netplay.h"
#include "tuning.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C"
{
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#if defined(SWANSTATION_PS5)
extern int ps5_net_buffer_kib;	// ps5/smb/compat.c
#endif
}

namespace fe::smb
{
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s64 = int64_t;

namespace
{

using namespace net;

constexpr size_t RamBlock = 16u << 20;	// a file in memory is made of these
constexpr size_t RamPiece = 4u << 20;	// read at a time; the cancel flag is looked at between two
constexpr u64 RamBudget = (u64)3 << 30;	// for all files in memory together

struct Account
{
	std::string user = "guest";
	std::string password;
	std::string domain;
};
Account account;
std::vector<std::string> folders;	// smb://, ftp:// and nfs:// folders, from network.cfg
std::string filesPlace;				// network.cfg's "files" folder, or ""
// What network.cfg says (the three above and the wake address) is read again
// when the share form changes it, while other threads use it: they take
// copies under this.
std::mutex configMutex;

Account currentAccount()
{
	std::lock_guard<std::mutex> lock(configMutex);
	return account;
}

// ---------------------------------------------------- what the share is doing

std::atomic<bool> loading{false};		// a network game is being loaded
std::atomic<bool> cancelled{false};		// and the user gave up
std::atomic<u64> ramTotal{0};			// the file being read into memory; 0 when none
std::atomic<u64> ramDone{0};
std::atomic<u64> gameTotal{0};			// all the files of the game being read into memory
std::atomic<u64> gameDone{0};
thread_local bool streamOnly;			// this thread's files are not read into memory
std::atomic<u64> ramInUse{0};			// all files in memory
std::atomic<int> busyDepth{0};			// requests in flight
std::atomic<long long> busySince{0};	// when the oldest started, in ms; 0 when none
std::atomic<long long> awaitedSince{0};	// a share waited for before a game starts (awaitShare), in ms; 0 when none
// Probes on this thread that settled whether a server is there: it answered,
// or it is named so that the console cannot ask it. A share waited for is no
// longer waited for after a try that made one of these.
thread_local unsigned settledHere;
std::atomic<unsigned> failureCount{0};
std::mutex errorMutex;
std::string errorText;

} // namespace

namespace net
{

long long nowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Busy::Busy()
{
	if (busyDepth.fetch_add(1) == 0)
		busySince = nowMs();
}

Busy::~Busy()
{
	if (busyDepth.fetch_sub(1) == 1)
		busySince = 0;
}

void Busy::answered()
{
	busySince = nowMs();
}

// The share answers, but not with what was asked for (a folder that cannot be
// listed): said on the screen and in the log.
void note(const std::string& text)
{
	{
		std::lock_guard<std::mutex> lock(errorMutex);
		errorText = text;
	}
	diag::mark("share: %s", text.c_str());
}

// The share could not be reached, or stopped answering: counted, so that a
// scan knows its list is not whole.
void fail(const std::string& text)
{
	failureCount++;
	note(text);
}

// The load in progress was cancelled: nothing more is asked of the share.
bool stopping()
{
	return loading && cancelled;
}

std::string stripped(std::string text)
{
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
		text.pop_back();
	return text;
}

namespace
{
// A connection tried on a thread of its own. Non-blocking sockets did not
// work on the console (see the top of this file), so the connect blocks, for
// as long as the system lets it when nothing answers, and the caller waits
// for it only so long. The thread ends by itself and closes its socket.
struct ConnectAttempt
{
	std::mutex mutex;
	std::condition_variable done;
	bool finished = false;
	int error = 0;			// errno of socket() or connect(); 0 when connected
	bool noSocket = false;
	long long tookMs = 0;
	// For the log, once: what fcntl says about a socket here (F_GETFL, then
	// F_SETFL with O_NONBLOCK on a socket of its own, then F_GETFL again).
	int fcntlResult = 0, fcntlErrno = 0;
	int setResult = 0, setErrno = 0, flagsAfter = 0;
};

std::shared_ptr<ConnectAttempt> startConnect(const sockaddr_in& address)
{
	std::shared_ptr<ConnectAttempt> attempt = std::make_shared<ConnectAttempt>();
	std::thread([attempt, address] {
		const long long started = nowMs();
		int error = 0;
		bool noSocket = false;
		int flags = 0, flagsErrno = 0;
		const int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
		{
			error = errno;
			noSocket = true;
		}
		else
		{
			errno = 0;
			flags = fcntl(fd, F_GETFL, 0);
			flagsErrno = errno;
			if (connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
				error = errno != 0 ? errno : EIO;
			close(fd);
		}
		static std::atomic<bool> tried{false};
		int setResult = 0, setErrno = 0, flagsAfter = 0;
		if (!tried.exchange(true))
		{
			const int other = socket(AF_INET, SOCK_STREAM, 0);
			if (other >= 0)
			{
				errno = 0;
				setResult = fcntl(other, F_SETFL, fcntl(other, F_GETFL, 0) | O_NONBLOCK);
				setErrno = errno;
				flagsAfter = fcntl(other, F_GETFL, 0);
				close(other);
			}
		}
		std::lock_guard<std::mutex> lock(attempt->mutex);
		attempt->setResult = setResult;
		attempt->setErrno = setErrno;
		attempt->flagsAfter = flagsAfter;
		attempt->error = error;
		attempt->noSocket = noSocket;
		attempt->fcntlResult = flags;
		attempt->fcntlErrno = flagsErrno;
		attempt->tookMs = nowMs() - started;
		attempt->finished = true;
		attempt->done.notify_all();
	}).detach();
	return attempt;
}

} // namespace

std::string lastComponent(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool numericAddress(const std::string& server, int port, sockaddr_in& address)
{
	address = sockaddr_in{};
	if (inet_pton(AF_INET, server.c_str(), &address.sin_addr) != 1)
		return false;
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)port);
#if defined(__PROSPERO__) || defined(__FreeBSD__)
	address.sin_len = sizeof(address);
#endif
	return true;
}

Probe probe(const std::string& server, int port, const char *what)
{
	sockaddr_in address;
	if (!numericAddress(server, port, address))
	{
		settledHere++;
		return Probe::NoName;
	}
	const std::shared_ptr<ConnectAttempt> attempt = startConnect(address);
	const long long deadline = nowMs() + ProbeSeconds * 1000;
	std::unique_lock<std::mutex> lock(attempt->mutex);
	while (!attempt->finished && nowMs() < deadline)
	{
		if (stopping())
			return Probe::Cancelled;
		attempt->done.wait_for(lock, std::chrono::milliseconds(100));
	}
	if (!attempt->finished)
	{
		diag::mark("%s: %s port %d: no answer in %d s", what, server.c_str(), port, ProbeSeconds);
		return Probe::Closed;
	}
	// Once, for the log: how this console's sockets take fcntl.
	static bool said;
	if (!said && !attempt->noSocket)
	{
		said = true;
		diag::mark("share: sockets here: fcntl(F_GETFL) gives %#x (errno %d); F_SETFL O_NONBLOCK (%#x) gives %d (errno %d), flags then %#x",
				attempt->fcntlResult, attempt->fcntlErrno, (int)O_NONBLOCK, attempt->setResult, attempt->setErrno,
				attempt->flagsAfter);
	}
	if (attempt->error == 0)
	{
		diag::mark("%s: %s port %d answers (%lld ms)", what, server.c_str(), port, attempt->tookMs);
		settledHere++;
		return Probe::Open;
	}
	diag::mark("%s: %s port %d: %s failed: %s (errno %d)", what, server.c_str(), port,
			attempt->noSocket ? "socket" : "connect", strerror(attempt->error), attempt->error);
	if (attempt->noSocket)
		return Probe::Unknown;
	switch (attempt->error)
	{
	case ECONNREFUSED:
	case ENETUNREACH:
	case EHOSTUNREACH:
	case ETIMEDOUT:
	case EHOSTDOWN:
	case ENETDOWN:
		return Probe::Closed;
	default:
		return Probe::Unknown;
	}
}

} // namespace net

namespace
{

// One share's connection. Every libsmb2 call on it holds the mutex.
struct Share
{
	std::recursive_mutex mutex;
	std::string server, share;
	smb2_context *context = nullptr;
	unsigned generation = 0;	// changes when the connection is made again
	long long failedAt = 0;		// nowMs() of the last failure to connect; 0 for none
	int preferred = 0;			// the login that worked last time

	bool connect()
	{
		if (context != nullptr)
			return true;
		if (stopping() || (failedAt != 0 && nowMs() - failedAt < RetrySeconds * 1000))
			return false;
		Busy busy;
#if defined(SWANSTATION_HOST)
		// A test can have the share take its time, as a NAS waking its disks does.
		if (const char *wait = getenv("SWANSTATION_NET_WAKE_MS"))
			std::this_thread::sleep_for(std::chrono::milliseconds(atoi(wait)));
#endif
		const Probe answer = probe(server, 445, "smb");
		if (answer == Probe::Cancelled)
			return false;
		if (answer == Probe::Closed)
		{
			fail(server + " does not answer: is it switched on, and is its address right (Settings, Library, Network Shares)?");
			failedAt = nowMs();
			return false;
		}
		if (answer == Probe::NoName)
		{
			fail("\"" + server + "\" is a name: give the server's IP address (192.168.x.x), not its name");
			failedAt = nowMs();
			return false;
		}
		// The ways to log in, most capable first. A guest (no password) logs
		// in with no password at all: libsmb2 signs an SMB 3.1.1 session it
		// opened with an empty one, and a server that mapped it to its guest
		// account refuses the signature. SMB 3.0.2 and 2.1 are the fallbacks
		// for servers that take neither.
		struct Attempt
		{
			bool password;
			int version;
			const char *name;
		};
		static const Attempt withPassword[] = { { true, 0, "SMB 3" }, { true, SMB2_VERSION_0302, "SMB 3.0.2" },
				{ true, SMB2_VERSION_0210, "SMB 2.1" } };
		static const Attempt asGuest[] = { { false, 0, "SMB 3, no password" }, { true, SMB2_VERSION_0302, "SMB 3.0.2" },
				{ true, SMB2_VERSION_0210, "SMB 2.1" } };
		const Account account = currentAccount();
		const Attempt *attempts = account.password.empty() ? asGuest : withPassword;
		smb2_context *c = nullptr;
		std::string error;
		for (int i = 0; i < 3 && !stopping(); i++)
		{
			const Attempt& attempt = attempts[(i + preferred) % 3];
			c = smb2_init_context();
			if (c == nullptr)
				return false;
			smb2_set_timeout(c, RequestSeconds);
			if (attempt.version != 0)
				smb2_set_version(c, (smb2_negotiate_version)attempt.version);
			smb2_set_user(c, account.user.c_str());
			if (attempt.password)
				smb2_set_password(c, account.password.c_str());
			if (!account.domain.empty())
				smb2_set_domain(c, account.domain.c_str());
			const time_t started = time(nullptr);
			if (smb2_connect_share(c, server.c_str(), share.c_str(), account.user.c_str()) == 0)
			{
				preferred = (i + preferred) % 3;
				diag::mark("smb: connected to //%s/%s as %s (%s) in %d s", server.c_str(), share.c_str(),
						account.user.c_str(), attempt.name, (int)(time(nullptr) - started));
#if defined(SWANSTATION_PS5)
				static bool saidBuffers;
				if (!saidBuffers)
				{
					saidBuffers = true;
					diag::mark("smb: the connection's TCP buffers: %d KiB (receive and send)%s", ps5_net_buffer_kib,
							ps5_net_buffer_kib == 0 ? ", the console's own: none larger was taken" : "");
				}
#endif
				break;
			}
			error = stripped(smb2_get_error(c));
			diag::mark("smb: //%s/%s as %s (%s): %s", server.c_str(), share.c_str(), account.user.c_str(), attempt.name,
					error.c_str());
			smb2_destroy_context(c);
			c = nullptr;
			// A server that let a request time out is not asked two more times.
			if (time(nullptr) - started >= RequestSeconds - 1)
				break;
		}
		if (c == nullptr)
		{
			if (stopping())
				return false;
			fail("//" + server + "/" + share + ": " + error
					+ " (check the share's name, the user and the password under Network shares)");
			failedAt = nowMs();
			return false;
		}
		context = c;
		generation++;
		failedAt = 0;
		return true;
	}

	// After a request failed: the connection may be gone (the NAS restarted,
	// the console's network was reset). The next request makes it again.
	void drop()
	{
		if (context != nullptr)
		{
			smb2_destroy_context(context);
			context = nullptr;
		}
	}

	// After a request that began at `started` failed: whether the connection
	// is what failed - the request timed out, or the share's root cannot be
	// read either. It is dropped then, and the caller asks once more on a new
	// one; otherwise the share answers and what was asked for is not there.
	bool lost(long long started)
	{
		if (context == nullptr)
			return true;
		smb2_stat_64 st{};
		if (nowMs() - started >= (RequestSeconds - 1) * 1000 || smb2_stat(context, "", &st) != 0)
		{
			drop();
			return true;
		}
		return false;
	}
};

std::mutex sharesMutex;
std::map<std::string, std::unique_ptr<Share>> shares;

// smb://server/share/a/b -> the share's connection and "a/b".
Share *locate(const std::string& path, std::string& relative)
{
	if (path.rfind("smb://", 0) != 0)
		return nullptr;
	const size_t serverEnd = path.find('/', 6);
	if (serverEnd == std::string::npos)
		return nullptr;
	size_t shareEnd = path.find('/', serverEnd + 1);
	if (shareEnd == std::string::npos)
		shareEnd = path.size();
	const std::string server = path.substr(6, serverEnd - 6);
	const std::string share = path.substr(serverEnd + 1, shareEnd - serverEnd - 1);
	if (server.empty() || share.empty())
		return nullptr;
	relative = shareEnd < path.size() ? path.substr(shareEnd + 1) : "";
	while (!relative.empty() && relative.back() == '/')
		relative.pop_back();
	std::lock_guard<std::mutex> lock(sharesMutex);
	std::unique_ptr<Share>& entry = shares[server + "/" + share];
	if (!entry)
	{
		entry = std::make_unique<Share>();
		entry->server = server;
		entry->share = share;
	}
	return entry.get();
}

// ------------------------------------------------- a file read from the share

class SmbFile : public StreamFile
{
public:
	SmbFile(Share *share, const std::string& relative, smb2fh *handle, u64 bytes)
		: StreamFile(bytes), share(share), relative(relative), handle(handle), generation(share->generation)
	{
	}

	~SmbFile() override
	{
		std::lock_guard<std::recursive_mutex> lock(share->mutex);
		if (handle != nullptr && share->context != nullptr && generation == share->generation)
			smb2_close(share->context, handle);
	}

private:
	// Reads up to `want` bytes at `offset`, making the connection and opening
	// the file again once if the request fails.
	int fetch(u8 *to, u64 offset, size_t want) override
	{
		std::lock_guard<std::recursive_mutex> lock(share->mutex);
		Busy busy;
		for (int attempt = 0; attempt < 2; attempt++)
		{
			if (!share->connect())
				break;
			if (generation != share->generation || handle == nullptr)
			{
				handle = smb2_open(share->context, relative.c_str(), O_RDONLY);
				generation = share->generation;
				if (handle == nullptr)
					break;
			}
			// At most 1 MiB a request: each answer shows the share is alive
			// (status() counts the time since the last one).
			const u32 most = std::clamp<u32>(smb2_get_max_read_size(share->context), 4096, 1u << 20);
			size_t total = 0;
			int n = 0;
			while (total < want)
			{
				const u32 chunk = (u32)std::min<size_t>(want - total, most);
				n = smb2_pread(share->context, handle, to + total, chunk, offset + total);
				if (n <= 0)
					break;
				total += (size_t)n;
				busy.answered();
			}
			if (total > 0 || n == 0)
				return (int)total;
			const std::string error = stripped(smb2_get_error(share->context));
			share->drop();
			handle = nullptr;
			if (attempt == 1 || stopping())
			{
				fail("reading " + lastComponent(relative) + ": " + error);
				break;
			}
			diag::mark("smb: reading %s: %s; connecting again", lastComponent(relative).c_str(), error.c_str());
		}
		hasFailed = true;
		return -1;
	}

	Share *share;
	std::string relative;
	smb2fh *handle;
	unsigned generation;
};

// -------------------------------------------------- a file read into memory

// A file's bytes, in blocks from the title's heap. The emulator opens a
// game's files more than once (a .cue's track file for each track): every
// open of a path gets the one image.
struct RamImage
{
	std::vector<u8 *> blocks;
	u64 bytes = 0;

	~RamImage()
	{
		for (u8 *block : blocks)
			free(block);
		ramInUse -= bytes;
	}
};

std::mutex imagesMutex;
// The images of the game being loaded or running, by path.
std::map<std::string, std::shared_ptr<RamImage>> images;
std::mutex imageLoadMutex;	// one file is read into memory at a time

std::shared_ptr<RamImage> findImage(const std::string& path)
{
	std::lock_guard<std::mutex> lock(imagesMutex);
	const auto it = images.find(path);
	return it == images.end() ? nullptr : it->second;
}

// The whole of `from` in memory, or nullptr: `stopped` says whether the read
// failed or was cancelled (then the game cannot start); otherwise the file
// does not fit and is to be streamed.
std::shared_ptr<RamImage> loadImage(const std::string& path, File& from, bool& stopped)
{
	stopped = false;
	std::lock_guard<std::mutex> oneAtATime(imageLoadMutex);
	if (std::shared_ptr<RamImage> image = findImage(path))
		return image;
	const std::string name = lastComponent(path);
	const u64 bytes = (u64)from.size();
	if (ramInUse.load() + bytes > RamBudget)
	{
		gameDone += bytes;		// it is streamed: nothing more of it is waited for
		diag::mark("share: %s (%llu MB) is streamed: more than %llu MB would be in memory", name.c_str(),
				(unsigned long long)(bytes >> 20), (unsigned long long)(RamBudget >> 20));
		return nullptr;
	}
	std::shared_ptr<RamImage> image = std::make_shared<RamImage>();
	image->bytes = bytes;
	ramInUse += bytes;
	for (u64 at = 0; at < bytes; at += RamBlock)
	{
		u8 *block = static_cast<u8 *>(malloc((size_t)std::min<u64>(RamBlock, bytes - at)));
		if (block == nullptr)
		{
			diag::mark("share: %s (%llu MB) is streamed: not enough memory", name.c_str(),
					(unsigned long long)(bytes >> 20));
			gameDone += bytes;
			return nullptr;
		}
		image->blocks.push_back(block);
	}
	// Small files (a .cue, an .sbi) come and go without a progress bar.
	const bool shown = bytes >= (1u << 20);
	if (shown)
	{
		ramDone = 0;
		ramTotal = bytes;
	}
	const long long started = nowMs();
	u64 done = 0;
	while (done < bytes)
	{
		if (cancelled)
		{
			stopped = true;
			break;
		}
		const size_t inBlock = (size_t)(done % RamBlock);
		const size_t n = (size_t)std::min<u64>({ (u64)RamPiece, bytes - done, (u64)(RamBlock - inBlock) });
		if (from.read(image->blocks[(size_t)(done / RamBlock)] + inBlock, n) != n)
		{
			stopped = true;
			break;
		}
		done += n;
		gameDone += n;
		if (shown)
			ramDone = done;
#if defined(SWANSTATION_HOST)
		// And be as slow as a real network: a wait for each piece read.
		if (const char *wait = getenv("SWANSTATION_NET_PIECE_MS"))
			std::this_thread::sleep_for(std::chrono::milliseconds(atoi(wait)));
#endif
	}
	if (shown)
		ramTotal = 0;
	if (stopped)
	{
		diag::mark("share: %s: %s after %llu of %llu MB", name.c_str(), cancelled ? "cancelled" : "the read failed",
				(unsigned long long)(done >> 20), (unsigned long long)(bytes >> 20));
		return nullptr;
	}
	if (shown)
	{
		const double seconds = std::max(0.001, (double)(nowMs() - started) / 1000.0);
		diag::mark("share: %s in memory: %llu MB in %.1f s (%.0f MB/s)", name.c_str(),
				(unsigned long long)(bytes >> 20), seconds, (double)bytes / (1 << 20) / seconds);
	}
	std::lock_guard<std::mutex> lock(imagesMutex);
	images[path] = image;
	return image;
}

class RamFile : public File
{
public:
	explicit RamFile(std::shared_ptr<RamImage> image) : image(std::move(image)), bytes(this->image->bytes)
	{
	}

	size_t read(void *out, size_t want) override
	{
		u8 *to = static_cast<u8 *>(out);
		const u64 available = position < bytes ? bytes - position : 0;
		u64 left = std::min<u64>(want, available);
		const size_t total = (size_t)left;
		while (left > 0)
		{
			const size_t inBlock = (size_t)(position % RamBlock);
			const size_t n = (size_t)std::min<u64>(left, RamBlock - inBlock);
			memcpy(to, image->blocks[(size_t)(position / RamBlock)] + inBlock, n);
			to += n;
			position += n;
			left -= n;
		}
		return total;
	}

	int64_t tell() override
	{
		return (s64)position;
	}

	int seek(int64_t offset, int whence) override
	{
		const s64 target = whence == SEEK_SET ? offset : whence == SEEK_CUR ? (s64)position + offset : (s64)bytes + offset;
		if (target < 0)
			return -1;
		position = (u64)target;
		return 0;
	}

	int64_t size() override
	{
		return (s64)bytes;
	}

	bool failed() override
	{
		return false;
	}

private:
	std::shared_ptr<RamImage> image;
	u64 bytes;
	u64 position = 0;
};

// The file, open for streaming, or nullptr.
SmbFile *openStream(Share *share, const std::string& relative)
{
	std::lock_guard<std::recursive_mutex> lock(share->mutex);
	Busy busy;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!share->connect())
			return nullptr;
		const long long started = nowMs();
		smb2fh *handle = smb2_open(share->context, relative.c_str(), O_RDONLY);
		if (handle != nullptr)
		{
			smb2_stat_64 st{};
			if (smb2_fstat(share->context, handle, &st) != 0)
			{
				smb2_close(share->context, handle);
				return nullptr;
			}
			return new SmbFile(share, relative, handle, st.smb2_size);
		}
		// Not there (the emulator tries names: an .sbi beside the image), or
		// the connection is gone: then once more on a new one.
		const std::string error = stripped(smb2_get_error(share->context));
		if (!share->lost(started))
			return nullptr;
		if (attempt == 1)
			fail("//" + share->server + "/" + share->share + " stopped answering: " + error);
	}
	return nullptr;
}

// 1 the path is there (`st` says what it is), 0 it is not, -1 the share
// cannot be reached. A connection that is gone is made again once.
int statShare(Share *share, const std::string& relative, smb2_stat_64& st)
{
	std::lock_guard<std::recursive_mutex> lock(share->mutex);
	Busy busy;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!share->connect())
			return -1;
		const long long started = nowMs();
		if (smb2_stat(share->context, relative.c_str(), &st) == 0)
			return 1;
		const std::string error = stripped(smb2_get_error(share->context));
		if (!share->lost(started))
			return 0;
		if (attempt == 1)
			fail("//" + share->server + "/" + share->share + " stopped answering: " + error);
	}
	return -1;
}

// "192.168.1.10/Share/Folder", "//server/share", "\\server\share\folder" and
// "smb://server/share" all name the same kind of place.
std::string normalize(std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	if (path.find("://") != std::string::npos && path.rfind("smb://", 0) != 0)
		return "";		// some other kind of place
	if (path.rfind("smb://", 0) == 0)
		path = path.substr(6);
	while (!path.empty() && path.front() == '/')
		path.erase(path.begin());
	while (!path.empty() && path.back() == '/')
		path.pop_back();
	if (path.find('/') == std::string::npos)
		return "";		// a server with no share
	return "smb://" + path;
}

} // namespace

std::vector<Entry> list(const std::string& path)
{
	if (ftp::isPath(path))
		return ftp::list(path);
	if (nfs::isPath(path))
		return nfs::list(path);
	std::vector<Entry> entries;
	std::string relative;
	Share *share = locate(path, relative);
	if (share == nullptr)
		return entries;
	std::lock_guard<std::recursive_mutex> lock(share->mutex);
	Busy busy;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!share->connect())
			break;
		const long long started = nowMs();
		smb2dir *dir = smb2_opendir(share->context, relative.c_str());
		if (dir == nullptr)
		{
			const std::string error = stripped(smb2_get_error(share->context));
			if (!share->lost(started))
			{
				// The share answers: it is this folder that cannot be listed.
				note("cannot list " + path.substr(6) + ": " + error);
				break;
			}
			if (attempt == 0)
				continue;
			fail("//" + share->server + "/" + share->share + " stopped answering: " + error);
			break;
		}
		std::string base = path;
		while (!base.empty() && base.back() == '/')
			base.pop_back();
		while (smb2dirent *entry = smb2_readdir(share->context, dir))
		{
			const std::string name = entry->name;
			if (name == "." || name == "..")
				continue;
			Entry item;
			item.name = name;
			item.path = base + "/" + name;
			item.directory = entry->st.smb2_type == SMB2_TYPE_DIRECTORY;
			item.size = entry->st.smb2_size;
			entries.push_back(std::move(item));
		}
		smb2_closedir(share->context, dir);
		break;
	}
	return entries;
}

File *open(const std::string& path)
{
	const bool onFtp = ftp::isPath(path), onNfs = nfs::isPath(path);
	std::string relative;
	Share *share = onFtp || onNfs ? nullptr : locate(path, relative);
	if (share == nullptr && !onFtp && !onNfs)
		return nullptr;
	// A file that is in memory already is read from there, whoever asks.
	if (std::shared_ptr<RamImage> image = findImage(path))
		return new RamFile(std::move(image));
	if (stopping())
		return nullptr;
	File *file = onFtp ? ftp::openStream(path) : onNfs ? nfs::openStream(path) : openStream(share, relative);
	if (file == nullptr || !loading || streamOnly || !options::frontend().ramCache || file->size() == 0)
		return file;
	// A game is being loaded into memory: this file, whole, now. The share's
	// lock is taken for each piece, not for the whole file.
	bool stopped = false;
	std::shared_ptr<RamImage> image = loadImage(path, *file, stopped);
	if (image != nullptr || stopped)
	{
		delete file;
		return image != nullptr ? new RamFile(std::move(image)) : nullptr;
	}
	file->seek(0, SEEK_SET);
	return file;
}

int stat(const std::string& path, Entry& entry)
{
	const bool onFtp = ftp::isPath(path), onNfs = nfs::isPath(path);
	std::string relative;
	Share *share = onFtp || onNfs ? nullptr : locate(path, relative);
	if (share == nullptr && !onFtp && !onNfs)
		return 0;
	if (std::shared_ptr<RamImage> image = findImage(path))
	{
		entry.name = lastComponent(path);
		entry.path = path;
		entry.directory = false;
		entry.size = image->bytes;
		return 1;
	}
	if (onFtp)
		return ftp::stat(path, entry);
	if (onNfs)
		return nfs::stat(path, entry);
	smb2_stat_64 st{};
	const int found = statShare(share, relative, st);
	if (found == 1)
	{
		entry.name = lastComponent(path);
		entry.path = path;
		entry.directory = st.smb2_type == SMB2_TYPE_DIRECTORY;
		entry.size = st.smb2_size;
	}
	return found;
}

namespace
{
// The hardware address of a server that sleeps (network.cfg's "wake"), as
// text and as its six bytes.
std::string wakeText;
uint8_t wakeBytes[6];
bool wakeSet;

bool parseHardwareAddress(const std::string& text, uint8_t out[6])
{
	unsigned parts[6];
	char end = 0;
	if (sscanf(text.c_str(), "%2x%*1[:-]%2x%*1[:-]%2x%*1[:-]%2x%*1[:-]%2x%*1[:-]%2x%c", &parts[0], &parts[1], &parts[2],
			&parts[3], &parts[4], &parts[5], &end) != 6)
		return false;
	for (int i = 0; i < 6; i++)
		out[i] = (uint8_t)parts[i];
	return true;
}

// The servers the folders are on, as IPv4 addresses in network order.
std::vector<uint32_t> serverAddresses()
{
	std::vector<uint32_t> out;
	for (const std::string& folder : gameFolders())
	{
		size_t begin = folder.find("://");
		begin = begin == std::string::npos ? 0 : begin + 3;
		const size_t at = folder.find('@', begin);
		const size_t slash = folder.find('/', begin);
		if (at != std::string::npos && (slash == std::string::npos || at < slash))
			begin = at + 1;
		std::string host = folder.substr(begin, folder.find_first_of("/:", begin) - begin);
		in_addr address{};
		if (inet_pton(AF_INET, host.c_str(), &address) == 1
				&& std::find(out.begin(), out.end(), address.s_addr) == out.end())
			out.push_back(address.s_addr);
	}
	return out;
}
}

std::string wakeAddress()
{
	std::lock_guard<std::mutex> lock(configMutex);
	return wakeSet ? wakeText : std::string();
}

// Wake-on-LAN: six bytes of 0xFF and the hardware address sixteen times, as
// a UDP packet to everyone on the network (the server is asleep and has no
// address of its own to send to). To the network's broadcast address, and to
// the last address of each server's own /24, where most home networks have it.
bool wake()
{
	uint8_t packet[6 + 16 * 6];
	std::string wakeText;
	{
		std::lock_guard<std::mutex> lock(configMutex);
		if (!wakeSet)
			return false;
		wakeText = smb::wakeText;
		memset(packet, 0xff, 6);
		for (int i = 0; i < 16; i++)
			memcpy(packet + 6 + i * 6, wakeBytes, 6);
	}
	const int handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (handle < 0)
	{
		diag::mark("share: wake: no socket (%d)", errno);
		return false;
	}
	const int yes = 1;
	setsockopt(handle, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
	std::vector<std::pair<uint32_t, uint16_t>> targets;
	targets.emplace_back(htonl(INADDR_BROADCAST), 9);
	for (const uint32_t server : serverAddresses())
	{
		targets.emplace_back((server & htonl(0xffffff00u)) | htonl(0xffu), 9);
		targets.emplace_back((server & htonl(0xffffff00u)) | htonl(0xffu), 7);
	}
#if defined(SWANSTATION_HOST)
	// A test listens on this PC instead.
	if (const char *to = getenv("SWANSTATION_WAKE_TO"))
	{
		targets.clear();
		targets.emplace_back(htonl(INADDR_LOOPBACK), (uint16_t)atoi(to));
	}
#endif
	int sent = 0;
	for (const auto& [address, port] : targets)
	{
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_port = htons(port);
		to.sin_addr.s_addr = address;
		if (sendto(handle, packet, sizeof(packet), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to))
				== (ssize_t)sizeof(packet))
			sent++;
	}
	close(handle);
	diag::mark("share: wake: the packet for %s went out %d time(s) of %d", wakeText.c_str(), sent, (int)targets.size());
	return sent > 0;
}

namespace
{
constexpr u64 LargeFile = 8u << 20;		// a disc image, for waking a share's disks

// The server a network folder is on, and the port its kind answers on.
bool serverOf(const std::string& folder, std::string& host, int& port)
{
	const size_t scheme = folder.find("://");
	if (scheme == std::string::npos)
		return false;
	const std::string kind = folder.substr(0, scheme);
	const size_t begin = scheme + 3;
	host = folder.substr(begin, folder.find('/', begin) - begin);
	port = kind == "ftp" ? 21 : kind == "nfs" ? 2049 : 445;
	const size_t colon = host.find(':');
	if (colon != std::string::npos)
	{
		port = atoi(host.c_str() + colon + 1);
		host.resize(colon);
	}
	return !host.empty() && port > 0;
}

// The first file of LargeFile or more in `folder`, or within `depth` folders
// below it; "" for none. At most 400 names of a folder and 16 folders below
// each are looked at.
std::string firstLargeFile(const std::string& folder, int depth)
{
	std::vector<std::string> below;
	unsigned seen = 0;
	for (const Entry& entry : list(folder))
	{
		if (++seen > 400)
			break;
		if (entry.name.empty() || entry.name[0] == '.')
			continue;
		if (entry.directory)
			below.push_back(entry.path);
		else if (entry.size >= LargeFile)
			return entry.path;
	}
	for (size_t i = 0; depth > 0 && i < below.size() && i < 16; i++)
	{
		const std::string found = firstLargeFile(below[i], depth - 1);
		if (!found.empty())
			return found;
	}
	return "";
}

// A little of the file, at its start and somewhere inside it (what a NAS
// keeps in its memory does not wake its disks): the bytes read.
size_t readALittle(const std::string& path)
{
	File *file = open(path);
	if (file == nullptr)
		return 0;
	std::vector<u8> buffer(64 * 1024);
	size_t got = file->read(buffer.data(), buffer.size());
	const int64_t bytes = file->size();
	if (bytes > (int64_t)(4 * buffer.size()))
	{
		std::mt19937_64 chance((u64)nowMs());
		const u64 at = (chance() % (u64)(bytes - (int64_t)buffer.size())) & ~(u64)4095;
		if (file->seek((int64_t)at, SEEK_SET) == 0)
			got += file->read(buffer.data(), buffer.size());
	}
	delete file;
	return got;
}
}

void wakeShares()
{
	const std::vector<std::string> folders = gameFolders();
	if (folders.empty())
		return;
	// The games played lately, the latest first: on each share, the latest
	// played from it is the file read.
	const std::vector<std::string> recent = history::recent(64);
	std::thread(tuning::background([folders, recent] {
		streamOnThisThread(true);
		// Not while the title's files are brought from the share or the
		// Network tab is scanned: those judge the share by the failures
		// counted meanwhile.
		while (netfiles::working() || library::scanning(library::Network))
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		std::vector<std::string> asked;
		for (const std::string& folder : folders)
		{
			if (loading)
			{
				diag::mark("share: the shares are not woken further: a game is being read from the network");
				return;
			}
			// One read for each SMB share (smb://server/share), whichever of
			// its folders the games are in.
			std::string share = folder;
			if (share.rfind("smb://", 0) == 0)
			{
				const size_t serverEnd = share.find('/', 6);
				share = share.substr(0, serverEnd == std::string::npos ? serverEnd : share.find('/', serverEnd + 1));
			}
			if (std::find(asked.begin(), asked.end(), share) != asked.end())
				continue;
			asked.push_back(share);
			std::string host;
			int port = 0;
			if (!serverOf(folder, host, port))
				continue;
			const long long began = nowMs();
			// Asked first whether it answers at all: a probe leaves nothing
			// for the title's own requests to find (no failure counted, no
			// wait before the next try).
			if (probe(host, port, "wake") != Probe::Open)
			{
				diag::mark("share: %s does not answer: not woken", share.c_str());
				continue;
			}
			std::string file;
			for (const std::string& game : recent)
				if (game.rfind(share + "/", 0) == 0)
				{
					file = game;
					break;
				}
			if (!file.empty())
			{
				// A cue sheet or a playlist is small: a large file beside it
				// (its track, a disc).
				Entry entry;
				if (stat(file, entry) != 1 || entry.size < LargeFile)
					file = firstLargeFile(file.substr(0, file.find_last_of('/')), 0);
			}
			if (file.empty())
				file = firstLargeFile(folder, 2);
			if (file.empty())
			{
				diag::mark("share: %s: no large file on it to read (to wake its disks)", share.c_str());
				continue;
			}
			const size_t got = readALittle(file);
			diag::mark("share: %s woken: %zu KiB of %s read in %.1f s", share.c_str(), got / 1024,
					lastComponent(file).c_str(), (double)(nowMs() - began) / 1000.0);
		}
	})).detach();
}

void loadConfig()
{
	// Read into these, then put in place at once.
	std::vector<std::string> folders;
	std::string filesPlace;
	Account account;
	bool wakeSet = false;
	std::string wakeText;
	uint8_t wakeBytes[6] = {};
	const auto install = [&] {
		std::lock_guard<std::mutex> lock(configMutex);
		smb::folders = folders;
		smb::filesPlace = filesPlace;
		smb::account = account;
		smb::wakeSet = wakeSet;
		smb::wakeText = wakeText;
		memcpy(smb::wakeBytes, wakeBytes, sizeof(wakeBytes));
	};
	const std::string file = rootDir + "network.cfg";
	FILE *f = fopen(file.c_str(), "r");
	if (f == nullptr)
	{
		// A template to fill in.
		if ((f = fopen(file.c_str(), "w")) != nullptr)
		{
			fputs("# PSSwanStation - games on the network: an SMB share (Windows sharing), an\n"
					"# NFS share or an FTP server.\n"
					"#\n"
					"# One \"path\" line for each folder to scan, the server by its IP address.\n"
					"# An SMB share is server/share/folder; an NFS share is nfs://server/path;\n"
					"# an FTP server is ftp://server/folder (ftp://server:2121/folder for a\n"
					"# port other than 21). For example:\n"
					"#   path = 192.168.1.10/Games/PSX\n"
					"#   path = nfs://192.168.1.10/volume1/games/psx\n"
					"#   path = ftp://192.168.1.10/games/psx\n"
					"# An NFS share is written to as the user and group the line names:\n"
					"#   path = nfs://192.168.1.10/volume1/games/psx?uid=1026&gid=100\n"
					"# (0 when not given; add &version=4 for NFS 4).\n"
					"#\n"
					"# A folder on an SMB or NFS share for PSSwanStation's own files: copies\n"
					"# of the memory cards, covers, and everything when Settings, Games and\n"
					"# network keeps your files there. For example:\n"
					"#   files = 192.168.1.10/Games/PSSwanStation\n"
					"# Remove the # in front of a path line to use it. The folder and the\n"
					"# folders inside it are scanned for games when PSSwanStation first starts\n"
					"# with it, and again with Square in the library; the list is kept, so\n"
					"# the share is not scanned again until you ask (as PSSwanStation starts\n"
					"# each share is only read a little, to wake its disks). The library has\n"
					"# a Network tab once games were found.\n"
					"#\n"
					"# The account: leave it as guest with no password for an open share (an\n"
					"# FTP server is then asked as \"anonymous\"). An FTP server with an account\n"
					"# of its own: ftp://user:password@server/folder.\n"
					"#\n"
					"# A server that sleeps can be woken (Wake-on-LAN) before it is asked:\n"
					"# name its network card's hardware address on a \"wake\" line, as\n"
					"#   wake = 00:11:32:AA:BB:CC\n"
					"\n"
					"# path = server/share/folder\n"
					"user = guest\n"
					"password =\n"
					"# domain = WORKGROUP\n"
					"# wake = 00:11:32:AA:BB:CC\n", f);
			fclose(f);
			chmod(file.c_str(), 0666);
		}
		install();
		return;
	}
	// FTP folders wait for the whole file: the account may come after them.
	std::vector<std::pair<size_t, std::string>> ftpLines;
	char line[1024];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		const std::string text = trim(line);
		if (text.empty() || text[0] == '#' || text[0] == ';')
			continue;
		const size_t equals = text.find('=');
		if (equals == std::string::npos)
			continue;
		const std::string key = trim(text.substr(0, equals));
		const std::string value = trim(text.substr(equals + 1));
		if (key == "path" && lowercase(value).rfind("ftp://", 0) == 0)
		{
			ftpLines.emplace_back(folders.size(), value);
			folders.emplace_back();
		}
		else if (key == "path" && lowercase(value).rfind("nfs://", 0) == 0)
		{
			const std::string folder = nfs::addFolder(value);
			if (!folder.empty())
				folders.push_back(folder);
			else
				diag::mark("share: network.cfg: \"%s\" is not nfs://server/path", value.c_str());
		}
		else if (key == "files" && !value.empty())
		{
			filesPlace = lowercase(value).rfind("nfs://", 0) == 0 ? nfs::addFolder(value)
					: lowercase(value).rfind("ftp://", 0) == 0 ? std::string() : normalize(value);
			if (filesPlace.empty())
				diag::mark("share: network.cfg: \"files = %s\" is not an SMB or NFS folder (an FTP server is only read)",
						value.c_str());
		}
		else if (key == "path")
		{
			const std::string folder = normalize(value);
			if (!folder.empty())
				folders.push_back(folder);
			else
				diag::mark("smb: network.cfg: \"%s\" is not server/share/folder", value.c_str());
		}
		else if (key == "user")
			account.user = value.empty() ? "guest" : value;
		else if (key == "password")
			account.password = value;
		else if (key == "domain")
			account.domain = value;
		else if (key == "wake" && !value.empty())
		{
			wakeSet = parseHardwareAddress(value, wakeBytes);
			wakeText = value;
			if (!wakeSet)
				diag::mark("share: network.cfg: \"%s\" is not a hardware address (00:11:22:33:44:55)", value.c_str());
		}
	}
	fclose(f);
	for (const auto& [index, value] : ftpLines)
	{
		folders[index] = ftp::addFolder(value, account.user, account.password);
		if (folders[index].empty())
			diag::mark("share: network.cfg: a path line is not ftp://server/folder");
	}
	folders.erase(std::remove(folders.begin(), folders.end(), std::string()), folders.end());
	install();
	for (const std::string& folder : folders)
		diag::mark("share: games folder %s%s", folder.c_str(),
				ftp::isPath(folder) || nfs::isPath(folder) ? "" : (" (user " + account.user + ")").c_str());
	if (!filesPlace.empty())
		diag::mark("share: the title's files folder %s", filesPlace.c_str());
	// A server that sleeps is woken as the title starts: it is up by the time
	// its games are asked for.
	if (wakeSet && !folders.empty())
		wake();
}

std::vector<std::string> gameFolders()
{
	std::lock_guard<std::mutex> lock(configMutex);
	return folders;
}

bool isNetworkPath(const std::string& path)
{
	return path.rfind("smb://", 0) == 0 || ftp::isPath(path) || nfs::isPath(path);
}

namespace
{
// The servers that could not be reached are asked again at the next request,
// not RetrySeconds after the failure.
void forgetFailures()
{
	ftp::retryNow();
	nfs::retryNow();
	std::lock_guard<std::mutex> lock(sharesMutex);
	for (auto& [name, share] : shares)
	{
		// Only a share nobody is using: failedAt is the connection's own.
		std::unique_lock<std::recursive_mutex> own(share->mutex, std::try_to_lock);
		if (own.owns_lock())
			share->failedAt = 0;
	}
}
}

void retryNow()
{
	// Asked for again by hand: a server that went back to sleep is woken first.
	wake();
	forgetFailures();
}

bool awaitShare(const std::string& path)
{
	const long long began = nowMs();
	unsigned settled = settledHere;
	Entry entry;
	int found = stat(path, entry);
	// There, not there, or the server answered and refused (the account, the
	// share's name): what follows says so, as before.
	if (found != -1 || settledHere != settled)
		return true;
	awaitedSince = began;
	const std::string where = path.substr(0, path.find_last_of('/'));
	diag::mark("share: %s does not answer: the game waits for it, asking each second", where.c_str());
	int tries = 1;
	long long tried = began;
	while (!stopping())
	{
		// A try that took a second or more (five for a server that is off) is
		// followed at once.
		while (!stopping() && nowMs() - tried < 1000)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (stopping())
			break;
		forgetFailures();
		tried = nowMs();
		settled = settledHere;
		found = stat(path, entry);
		tries++;
		if (found != -1 || settledHere != settled)
			break;
	}
	awaitedSince = 0;
	const double seconds = (double)(nowMs() - began) / 1000.0;
	if (stopping())
	{
		diag::mark("share: waiting for %s given up after %.1f s (%d tries)", where.c_str(), seconds, tries);
		return false;
	}
	if (found == -1)
		diag::mark("share: %s answers after %.1f s, but does not let the game be read", where.c_str(), seconds);
	else
	{
		diag::mark("share: %s answered after %.1f s (%d tries): the game is read", where.c_str(), seconds, tries);
		clearError();
	}
	return true;
}

void streamOnThisThread(bool only)
{
	streamOnly = only;
}

void setLoadTotal(uint64_t bytes)
{
	gameDone = 0;
	gameTotal = bytes;
}

void clearLoad()
{
	gameTotal = 0;
	gameDone = 0;
}

void beginLoad()
{
	retryNow();
	cancelled = false;
	ramTotal = 0;
	ramDone = 0;
	clearLoad();
	loading = true;
	diag::mark("share: a network game is starting (%s)", options::frontend().ramCache ? "read into memory" : "streamed");
}

void endLoad()
{
	loading = false;
	cancelled = false;
	ramTotal = 0;
}

void cancelLoad()
{
	cancelled = true;
}

void releaseImages()
{
	std::map<std::string, std::shared_ptr<RamImage>> done;
	{
		std::lock_guard<std::mutex> lock(imagesMutex);
		done.swap(images);
	}
	if (!done.empty())
		diag::mark("share: %d file(s) released from memory", (int)done.size());
}

Status status()
{
	Status current;
	// The whole game when its size is known; else the file being read.
	const bool whole = gameTotal.load() != 0;
	const u64 total = whole ? gameTotal.load() : ramTotal.load();
	const u64 done = std::min(whole ? gameDone.load() : ramDone.load(), total);
	if (total != 0)
		current.progress = std::clamp((float)((double)done / (double)total), 0.f, 1.f);
	if (whole)
	{
		current.done = done;
		current.total = total;
	}
	// A share waited for before a game starts counts from the first try.
	const long long awaited = awaitedSince.load();
	const long long since = awaited != 0 ? awaited : busySince.load();
	const long long waited = since != 0 ? nowMs() - since : 0;
	char text[128];
	if (awaited != 0 || waited >= 2500)
	{
		// No answer for a while: a NAS waking its disks, or a server that is gone.
		snprintf(text, sizeof(text), "Waiting for the network share (%d s)", (int)(waited / 1000));
		current.text = text;
		current.waiting = true;
	}
	else if (total != 0)
	{
		snprintf(text, sizeof(text), "Loading into memory   %u / %u MB", (unsigned)(done >> 20), (unsigned)(total >> 20));
		current.text = text;
	}
	return current;
}

unsigned failures()
{
	return failureCount.load();
}

void clearError()
{
	std::lock_guard<std::mutex> lock(errorMutex);
	errorText.clear();
}

std::string lastError()
{
	std::lock_guard<std::mutex> lock(errorMutex);
	return errorText;
}

#if defined(SWANSTATION_HOST)
// A test run can ask for a network file to be read every way the emulator
// would and compared with the same file on the PC:
// SWANSTATION_NET_TEST="<network path>|<local path>".
int selfTest(const char *spec)
{
	const std::string text = spec;
	const size_t bar = text.find('|');
	if (bar == std::string::npos)
		return 2;
	const std::string remote = text.substr(0, bar), local = text.substr(bar + 1);
	std::vector<uint8_t> want;
	if (!readFile(local, want) || want.empty())
	{
		printf("net-test: cannot read %s\n", local.c_str());
		return 2;
	}
	Entry entry;
	const int found = stat(remote, entry);
	printf("net-test: stat %d, size %llu (local %zu)\n", found, (unsigned long long)entry.size, want.size());
	if (found != 1 || entry.size != want.size())
		return 1;
	if (stat(remote + ".missing", entry) != 0)
	{
		printf("net-test: a file that is not there was found\n");
		return 1;
	}
	uint32_t seed = 12345;
	const auto random = [&seed](uint32_t below) {
		seed = seed * 1664525u + 1013904223u;
		return (uint32_t)(((uint64_t)(seed >> 8) * below) >> 24);
	};
	std::vector<uint8_t> got(2u << 20);
	for (int pass = 0; pass < 2; pass++)
	{
		// The second pass is from memory, as a game being loaded is.
		if (pass == 1)
			beginLoad();
		File *file = open(remote);
		if (pass == 1)
			endLoad();
		if (file == nullptr || (uint64_t)file->size() != want.size())
		{
			printf("net-test: pass %d: open failed\n", pass);
			return 1;
		}
		// From the first byte to the last, in pieces of every size.
		size_t at = 0;
		while (at < want.size())
		{
			const size_t n = std::min<size_t>(1 + random(700000), want.size() - at);
			if (file->read(got.data(), n) != n || memcmp(got.data(), want.data() + at, n) != 0)
			{
				printf("net-test: pass %d: reading on failed at %zu\n", pass, at);
				return 1;
			}
			at += n;
		}
		if (file->read(got.data(), 16) != 0)
		{
			printf("net-test: pass %d: read past the end\n", pass);
			return 1;
		}
		// Here and there: back, a little ahead, far ahead, across the end.
		for (int i = 0; i < 400; i++)
		{
			const int kind = (int)random(4);
			size_t where = kind == 0 ? random((uint32_t)want.size())
					: kind == 1 ? std::min<size_t>(at + random(300000), want.size() - 1)
					: kind == 2 ? (at > 200000 ? at - random(200000) : 0) : want.size() - 1 - random(100000);
			const size_t n = std::min<size_t>(1 + random(i % 7 == 0 ? 1500000 : 9000), got.size());
			const size_t expect = std::min(n, want.size() - where);
			if (file->seek((int64_t)where, SEEK_SET) != 0 || file->read(got.data(), n) != expect
					|| memcmp(got.data(), want.data() + where, expect) != 0)
			{
				printf("net-test: pass %d: read %d of %zu at %zu failed\n", pass, i, n, where);
				return 1;
			}
			at = where + expect;
		}
		delete file;
		printf("net-test: pass %d (%s) agrees\n", pass, pass == 0 ? "streamed" : "from memory");
	}
	releaseImages();
	return 0;
}
#endif

std::string filesFolder()
{
	std::lock_guard<std::mutex> lock(configMutex);
	return filesPlace;
}

bool canWrite(const std::string& path)
{
	return path.rfind("smb://", 0) == 0 || nfs::isPath(path);
}

namespace
{
// Each folder of `relative` ("a/b/c") on the share, made where it is not there.
bool makeShareFolders(Share *share, const std::string& relative)
{
	for (size_t at = 0; at <= relative.size(); at++)
		if (at == relative.size() || relative[at] == '/')
		{
			if (at == 0)
				continue;
			const std::string part = relative.substr(0, at);
			smb2_stat_64 st{};
			if (smb2_stat(share->context, part.c_str(), &st) == 0)
			{
				if (st.smb2_type != SMB2_TYPE_DIRECTORY)
					return false;
				continue;
			}
			if (smb2_mkdir(share->context, part.c_str()) != 0)
				return false;
		}
	return true;
}
}

bool makeFolders(const std::string& path)
{
	if (nfs::isPath(path))
		return nfs::makeFolders(path);
	std::string relative;
	Share *share = locate(path, relative);
	if (share == nullptr)
		return false;
	std::lock_guard<std::recursive_mutex> lock(share->mutex);
	Busy busy;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!share->connect())
			return false;
		const long long started = nowMs();
		if (makeShareFolders(share, relative))
			return true;
		if (!share->lost(started))
			return false;
	}
	return false;
}

bool writeFile(const std::string& path, const void *data, size_t bytes, std::string& error)
{
	if (nfs::isPath(path))
		return nfs::writeFile(path, data, bytes, error);
	std::string relative;
	Share *share = ftp::isPath(path) ? nullptr : locate(path, relative);
	if (share == nullptr)
	{
		error = ftp::isPath(path) ? "an FTP folder is only read" : "not on a share of network.cfg";
		return false;
	}
	std::lock_guard<std::recursive_mutex> lock(share->mutex);
	Busy busy;
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!share->connect())
		{
			error = "the share does not answer";
			return false;
		}
		const long long started = nowMs();
		const size_t slash = relative.find_last_of('/');
		if (slash != std::string::npos && !makeShareFolders(share, relative.substr(0, slash)))
		{
			if (!share->lost(started))
			{
				error = "its folder could not be made on the share (" + stripped(smb2_get_error(share->context)) + ")";
				return false;
			}
			continue;
		}
		// Written beside it and then put in its place.
		const std::string part = relative + ".part";
		smb2fh *handle = smb2_open(share->context, part.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
		if (handle == nullptr)
		{
			if (!share->lost(started))
			{
				error = "the share does not let it be written (" + stripped(smb2_get_error(share->context)) + ")";
				return false;
			}
			continue;
		}
		const uint8_t *from = static_cast<const uint8_t *>(data);
		const size_t piece = std::max<size_t>(std::min<size_t>(smb2_get_max_write_size(share->context), 1 << 20), 4096);
		size_t done = 0;
		bool ok = true;
		while (done < bytes)
		{
			const int wrote = smb2_pwrite(share->context, handle, from + done, (uint32_t)std::min(bytes - done, piece), done);
			if (wrote <= 0)
			{
				ok = false;
				break;
			}
			done += (size_t)wrote;
		}
		ok = smb2_close(share->context, handle) == 0 && ok;
		if (ok)
		{
			// SMB renames over nothing: what was there goes first.
			smb2_unlink(share->context, relative.c_str());
			ok = smb2_rename(share->context, part.c_str(), relative.c_str()) == 0;
		}
		if (ok)
			return true;
		error = "the share could not be written (" + stripped(smb2_get_error(share->context)) + ")";
		smb2_unlink(share->context, part.c_str());
		if (!share->lost(started))
			return false;
	}
	return false;
}

bool remove(const std::string& path)
{
	if (nfs::isPath(path))
		return nfs::remove(path);
	std::string relative;
	Share *share = locate(path, relative);
	if (share == nullptr)
		return false;
	std::lock_guard<std::recursive_mutex> lock(share->mutex);
	Busy busy;
	if (!share->connect())
		return false;
	smb2_stat_64 st{};
	return smb2_unlink(share->context, relative.c_str()) == 0 || smb2_stat(share->context, relative.c_str(), &st) != 0;
}

bool readWhole(const std::string& path, std::vector<uint8_t>& out)
{
	out.clear();
	Entry entry;
	if (stat(path, entry) != 1 || entry.directory || entry.size > (256ull << 20))
		return false;
	File *file = nullptr;
	if (ftp::isPath(path))
		file = ftp::openStream(path);
	else if (nfs::isPath(path))
		file = nfs::openStream(path);
	else
	{
		std::string relative;
		Share *share = locate(path, relative);
		file = share != nullptr ? openStream(share, relative) : nullptr;
	}
	if (file == nullptr)
		return false;
	out.resize((size_t)entry.size);
	const size_t got = out.empty() ? 0 : file->read(out.data(), out.size());
	const bool ok = got == out.size() && !file->failed();
	delete file;
	if (!ok)
		out.clear();
	return ok;
}


// ------------------------------------------------------- the share form

Config readConfig()
{
	Config config;
	FILE *f = fopen((rootDir + "network.cfg").c_str(), "r");
	if (f == nullptr)
		return config;
	char line[1024];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		const std::string text = trim(line);
		if (text.empty() || text[0] == '#' || text[0] == ';')
			continue;
		const size_t equals = text.find('=');
		if (equals == std::string::npos)
			continue;
		const std::string key = trim(text.substr(0, equals)), value = trim(text.substr(equals + 1));
		if (key == "path" && !value.empty())
			config.paths.push_back(value);
		else if (key == "files")
			config.files = value;
		else if (key == "user")
			config.user = value;
		else if (key == "password")
			config.password = value;
	}
	fclose(f);
	return config;
}

namespace
{
std::string oneLine(std::string value)
{
	value.erase(std::remove_if(value.begin(), value.end(), [](char c) { return c == '\n' || c == '\r'; }), value.end());
	return trim(value);
}

// The key of a line that is in use (not a comment), or "".
std::string activeKey(const std::string& line)
{
	const std::string text = trim(line);
	if (text.empty() || text[0] == '#' || text[0] == ';')
		return "";
	const size_t equals = text.find('=');
	return equals == std::string::npos ? "" : trim(text.substr(0, equals));
}
}

// The file keeps what the form does not change: its comments, the domain,
// the wake address, any line it does not know. The folders take the place
// of the first path line (or come after the template's "# path =" example),
// and the others are changed where they are.
bool saveConfig(const Config& config, std::string& error)
{
	const std::string file = rootDir + "network.cfg";
	std::vector<std::string> lines;
	{
		FILE *f = fopen(file.c_str(), "r");
		if (f == nullptr)
		{
			loadConfig();	// the template, to fill in
			f = fopen(file.c_str(), "r");
		}
		if (f != nullptr)
		{
			char line[1024];
			while (fgets(line, sizeof(line), f) != nullptr)
			{
				std::string text = line;
				while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
					text.pop_back();
				lines.push_back(text);
			}
			fclose(f);
		}
	}
	std::vector<std::string> paths;
	for (const std::string& path : config.paths)
		if (!oneLine(path).empty())
			paths.push_back("path = " + oneLine(path));
	const std::string files = oneLine(config.files);
	const std::string user = oneLine(config.user).empty() ? "guest" : oneLine(config.user);
	const std::string password = oneLine(config.password);

	std::vector<std::string> out;
	bool pathsDone = false, filesDone = false, userDone = false, passwordDone = false;
	for (const std::string& line : lines)
	{
		const std::string key = activeKey(line);
		if (key == "path")
		{
			if (!pathsDone)
				out.insert(out.end(), paths.begin(), paths.end());
			pathsDone = true;
		}
		else if (key == "files")
		{
			if (!filesDone && !files.empty())
				out.push_back("files = " + files);
			filesDone = true;
		}
		else if (key == "user")
		{
			if (!userDone)
				out.push_back("user = " + user);
			userDone = true;
		}
		else if (key == "password")
		{
			if (!passwordDone)
				out.push_back("password = " + password);
			passwordDone = true;
		}
		else
			out.push_back(line);
	}
	if (!pathsDone && !paths.empty())
	{
		// After the template's last "# path = ..." example, or at the end.
		size_t at = out.size();
		for (size_t i = 0; i < out.size(); i++)
		{
			const std::string text = trim(out[i]);
			if (text.rfind("#", 0) == 0 && trim(text.substr(1)).rfind("path", 0) == 0 && text.find('=') != std::string::npos)
				at = i + 1;
		}
		out.insert(out.begin() + (std::ptrdiff_t)at, paths.begin(), paths.end());
	}
	if (!filesDone && !files.empty())
		out.push_back("files = " + files);
	if (!userDone)
		out.push_back("user = " + user);
	if (!passwordDone)
		out.push_back("password = " + password);

	// Written beside it and put in its place: a file cut short is never read.
	const std::string next = file + ".new";
	FILE *f = fopen(next.c_str(), "w");
	if (f == nullptr)
	{
		error = std::string("network.cfg could not be written: ") + strerror(errno);
		return false;
	}
	bool ok = true;
	for (const std::string& line : out)
		ok = ok && fputs(line.c_str(), f) >= 0 && fputc('\n', f) != EOF;
	ok = fflush(f) == 0 && ok;
	fsync(fileno(f));
	fclose(f);
	if (!ok || rename(next.c_str(), file.c_str()) != 0)
	{
		error = std::string("network.cfg could not be written: ") + strerror(errno);
		::remove(next.c_str());
		return false;
	}
	chmod(file.c_str(), 0666);
	diag::mark("share: network.cfg written by the share form (%d folder(s))", (int)paths.size());
	loadConfig();
	// A connection made with the account before is made again with this one,
	// the next time it is needed (one in use now is left to finish).
	{
		std::lock_guard<std::mutex> lock(sharesMutex);
		for (auto& [name, share] : shares)
		{
			std::unique_lock<std::recursive_mutex> own(share->mutex, std::try_to_lock);
			if (own.owns_lock())
			{
				share->drop();
				share->failedAt = 0;
			}
		}
	}
	ftp::retryNow();
	nfs::retryNow();
	clearError();
	return true;
}

namespace
{
// A NetBIOS node status answer: the machine's own name (its unique 0x20
// "file server" name, else its 0x00 workstation name).
std::string nodeStatusName(const uint8_t *b, size_t n)
{
	if (n < 57)
		return "";
	const unsigned count = b[56];
	std::string first;
	for (unsigned i = 0; i < count && 57 + (i + 1) * 18 <= n; i++)
	{
		const uint8_t *e = b + 57 + i * 18;
		const bool group = (e[16] & 0x80) != 0;
		std::string name(reinterpret_cast<const char *>(e), 15);
		while (!name.empty() && (name.back() == ' ' || name.back() == 0))
			name.pop_back();
		if (group || name.empty())
			continue;
		bool printable = true;
		for (char c : name)
			printable = printable && (unsigned char)c >= 32 && (unsigned char)c < 127;
		if (!printable)
			continue;
		if (e[15] == 0x20)
			return name;
		if (first.empty() && e[15] == 0x00)
			first = name;
	}
	return first;
}
}

// After OGBox's share form (OGBox by Press5elect): the hosts of the console's
// own /24 that answer on port 445, 64 asked at once with 400 ms for each
// batch, then each asked for its NetBIOS name on port 137 (half a second for
// all the answers). A connect that never comes back is what a blocking
// socket would do with a host that is not there, so the scan stops at once
// should the console not take O_NONBLOCK on a socket.
std::vector<FoundServer> findServers(std::string& problem)
{
	std::vector<FoundServer> found;
	problem.clear();
	std::string own = netplay::localAddress();
#if defined(SWANSTATION_HOST)
	// A test looks on another network (127.0.0.x, where it answers).
	if (const char *from = getenv("SWANSTATION_SCAN_FROM"))
		own = from;
#endif
	in_addr mine{};
	if (own.empty() || inet_pton(AF_INET, own.c_str(), &mine) != 1)
	{
		problem = "The console has no address on a network.";
		return found;
	}
	const uint32_t me = ntohl(mine.s_addr), net = me & 0xffffff00u;
	const long long started = nowMs();
	std::vector<uint32_t> open;
	for (uint32_t base = 1; base < 255 && !stopping(); base += 64)
	{
		std::vector<std::pair<int, uint32_t>> sockets;
		for (uint32_t h = base; h < base + 64 && h < 255; h++)
		{
			const uint32_t ip = net | h;
			if (ip == me)
				continue;
			const int fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd < 0)
				continue;
			const int flags = fcntl(fd, F_GETFL, 0);
			if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 || !(fcntl(fd, F_GETFL, 0) & O_NONBLOCK))
			{
				close(fd);
				for (const auto& s : sockets)
					close(s.first);
				diag::mark("share: find servers: this console's sockets do not take O_NONBLOCK; not scanned");
				problem = "This console cannot ask many addresses at once. Type the server's address instead.";
				return found;
			}
			char text[INET_ADDRSTRLEN];
			snprintf(text, sizeof(text), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
			sockaddr_in address;
			numericAddress(text, 445, address);
			const int rc = connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
			if (rc == 0)
			{
				open.push_back(ip);
				close(fd);
			}
			else if (errno == EINPROGRESS)
				sockets.push_back({ fd, ip });
			else
				close(fd);
		}
		std::vector<pollfd> polled;
		for (const auto& s : sockets)
			polled.push_back({ s.first, POLLOUT, 0 });
		for (int waited = 0; waited < 400 && !polled.empty(); waited += 50)
		{
			if (poll(polled.data(), polled.size(), 50) <= 0)
				continue;
			for (size_t i = 0; i < polled.size(); i++)
				if (polled[i].fd >= 0 && (polled[i].revents & (POLLOUT | POLLERR | POLLHUP)))
				{
					int error = 1;
					socklen_t length = sizeof(error);
					if ((polled[i].revents & POLLOUT)
							&& getsockopt(polled[i].fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0)
						open.push_back(sockets[i].second);
					polled[i].fd = -1;	// answered: not asked again
				}
		}
		for (const auto& s : sockets)
			close(s.first);
	}
	std::sort(open.begin(), open.end());
	open.erase(std::unique(open.begin(), open.end()), open.end());

	// Their names: a node status request for "*".
	std::vector<std::string> names(open.size());
	const int udp = socket(AF_INET, SOCK_DGRAM, 0);
	if (udp >= 0 && !open.empty())
	{
		uint8_t query[50] = { 0x50, 0x53, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 'C', 'K' };
		for (int i = 15; i < 45; i++)
			query[i] = 'A';
		query[45] = 0x00;
		query[46] = 0x00;
		query[47] = 0x21;	// NBSTAT
		query[48] = 0x00;
		query[49] = 0x01;	// IN
		for (const uint32_t ip : open)
		{
			sockaddr_in to{};
			to.sin_family = AF_INET;
			to.sin_port = htons(137);
			to.sin_addr.s_addr = htonl(ip);
#if defined(__PROSPERO__) || defined(__FreeBSD__)
			to.sin_len = sizeof(to);
#endif
			sendto(udp, query, sizeof(query), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
		}
		for (int waited = 0; waited < 500; waited += 50)
		{
			pollfd p{ udp, POLLIN, 0 };
			if (poll(&p, 1, 50) <= 0)
				continue;
			uint8_t answer[1500];
			sockaddr_in from{};
			socklen_t length = sizeof(from);
			const ssize_t n = recvfrom(udp, answer, sizeof(answer), 0, reinterpret_cast<sockaddr *>(&from), &length);
			if (n <= 0)
				continue;
			const auto it = std::find(open.begin(), open.end(), ntohl(from.sin_addr.s_addr));
			if (it != open.end())
				names[(size_t)(it - open.begin())] = nodeStatusName(answer, (size_t)n);
		}
	}
	if (udp >= 0)
		close(udp);
	for (size_t i = 0; i < open.size(); i++)
	{
		char text[INET_ADDRSTRLEN];
		snprintf(text, sizeof(text), "%u.%u.%u.%u", open[i] >> 24, (open[i] >> 16) & 255, (open[i] >> 8) & 255, open[i] & 255);
		found.push_back({ text, names[i] });
	}
	// The log has how many and how long, not the addresses.
	diag::mark("share: find servers: %d answer on port 445 in the console's /24 (%lld ms)", (int)found.size(),
			nowMs() - started);
	if (found.empty())
		problem = "No server answered on the console's network.";
	return found;
}

} // namespace fe::smb
