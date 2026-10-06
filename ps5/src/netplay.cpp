/*
	PSSwanStation - netplay: the connection, the greeting, the state transfer
	and the lockstep (netplay.h says what it is for).

	SPDX-License-Identifier: GPL-3.0-or-later

	Threads. The frame loop's thread calls host(), join(), stop(), step() and
	ran(); a thread of this module's own has the sockets. The two meet in one
	structure behind a mutex that is never held across a socket call, a hook or
	a wait, so step() costs a few list operations and, when it has something to
	send, one byte written to the pipe that wakes the other thread (after the
	mutex is let go; the pipe is made once and kept). The hooks are called from
	step() and ran() only, with the mutex released.

	Sockets. One TCP connection, IPv4, TCP_NODELAY. Nothing here relies on a
	socket that does not wait, for smb.cpp's and ftp.cpp's reason (a title's
	socket on the console did not become non-blocking through fcntl): the
	connection's socket is left blocking, every accept and every read asks
	poll first, and a write is one piece of at most 16 KiB after poll said
	there is room, with MSG_DONTWAIT where the system has it. Send and receive
	time limits are a last resort. The connect of a join is made on a helper
	thread, non-blocking where the system does that and blocking where it does
	not; the network thread waits for it 10 seconds, then shuts the socket
	down and, if that does not end the connect either, leaves the helper to
	end by itself.

	The protocol. Messages, each with an 8-byte header: 'S' 'N', the type, a
	zero, and the length of what follows as 32 bits; numbers are little-endian,
	texts are a 16-bit length and the bytes.

		HELLO     protocol version, build, game, BIOS, role (1 hosts), delay,
		          settings text (the host's; the guest's is empty)
		REFUSE    a text for the other side's screen; the sender ends the session
		RESYNC    epoch: the host starts a new transfer of its state
		STATE     epoch, state size, packed size, state sum, offset, bytes: one
		          piece of the host's state, packed (below)
		STATE_OK  epoch: the guest loaded it
		INPUT     epoch, frame, buttons, lx, ly, rx, ry
		CHECK     epoch, frame, checksum of the emulated memory before that frame
		PAUSE     1 or 0: a menu is open on the sender's side
		PING      the sender's clock; answered by PONG with the same bytes
		BYE       the sender leaves

	Both sides send HELLO when the connection is made and compare the other's
	with their own; the host also sends REFUSE when they differ. Then the state
	travels (epoch 1): the host's step() saves it, the thread packs and sends
	it, the guest's thread unpacks it, the guest's step() loads it and answers
	STATE_OK, and both play from frame 0. Inputs and checksums carry the epoch
	they belong to; when the checksums of a frame differ the host raises the
	epoch, says RESYNC and sends its state again, and everything of an older
	epoch that is still on its way is dropped on arrival.

	The state is mostly zeros, so it is packed as runs: a count of bytes to
	copy, those bytes, a count of zero bytes, and so on to the end; counts are
	7 bits to a byte, low bits first. Only runs of 16 zeros or more are counted
	as zeros.

	Silence. Each side sends PING every second in every state, so no byte from
	the other side for 5 seconds is worth showing (silentSeconds()) and for 20
	seconds means the connection is dead, whether a menu is open or not.
*/
#include "netplay.h"
#include "fe.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

namespace fe::netplay
{
namespace
{
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

constexpr u32 ProtocolVersion = 1;
constexpr u8 Magic0 = 'S', Magic1 = 'N';
constexpr size_t HeaderBytes = 8;
constexpr size_t MaxMessage = 256 * 1024;		// more in one message is not this protocol
constexpr size_t StatePiece = 60 * 1024;		// of the packed state in one STATE message
constexpr size_t MaxStateBytes = 64u * 1024 * 1024;	// a PlayStation's state is 11 MiB
constexpr size_t MaxTextBytes = 1024;			// game and BIOS names
constexpr size_t MaxSettingsBytes = 128 * 1024;
constexpr size_t MinZeroRun = 16;
constexpr size_t SendPiece = 16 * 1024;
constexpr size_t ReceiveBlock = 64 * 1024;
constexpr size_t MaxQueuedInputs = 600;			// the other side is never this far ahead

constexpr double ConnectSeconds = 10;			// for the host to accept a join
constexpr double GreetingSeconds = 10;			// for the other side's HELLO
constexpr double PingSeconds = 1;
constexpr double QuietSeconds = 5;				// silence worth showing
constexpr double SilenceSeconds = 20;			// silence that ends the session
constexpr int SocketLimitSeconds = 2;			// the last resort for a send or a read
constexpr u64 CheckFrames = 120;				// a checksum every so many frames

enum Type : u8
{
	Hello = 1,
	Refuse,
	Resync,
	StateData,
	StateOk,
	InputData,
	Check,
	Pause,
	Ping,
	Pong,
	Bye,
};

// Where the state transfer is.
enum class Sync
{
	None,
	NeedSave,		// host: the next step() saves the state
	Saving,			// host: step() is in the hook
	Saved,			// host: the bytes wait for the thread
	Sending,		// host: the thread has them; STATE_OK is awaited
	Receiving,		// guest: the pieces are arriving
	Arrived,		// guest: the whole state waits for the next step()
	Loading,		// guest: step() is in the hook
};

// A frame's checksum. Frame numbers travel as their low 32 bits.
struct Sum
{
	u32 frame;
	u32 value;
};

// What the two threads share; everything under `mutex`.
struct Shared
{
	std::mutex mutex;

	// Set by host() and join() before the thread starts.
	Session session;
	bool hosting = false;
	sockaddr_in peer{};
	std::string peerName;
	int listenFd = -1;
	int wakeRead = -1, wakeWrite = -1;
	bool wakeUsable = false;		// the pipe is there and behaves
	bool wakePending = false;		// the thread has been woken, or is about to be, and has not looked yet
	bool wakeWanted = false;		// the byte that wakes it is still to be written
	bool quit = false;

	State state = State::Off;
	std::string error;
	std::string hostSettings;
	int delay = 2;
	bool connected = false;			// the thread has the other side: messages can be queued
	std::vector<u8> out;			// messages for the other side, in order

	u32 epoch = 0;
	Sync sync = Sync::None;
	std::vector<u8> stateBytes;		// host: saved, for the thread; guest: arrived, for step()

	u64 frame = 0;
	// The frame the next input recorded here, and the next one received, are
	// for. The lists hold the inputs from frame max(frame, delay) on.
	u64 localNext = 0, remoteNext = 0;
	std::deque<Input> localInputs, remoteInputs;
	bool runGiven = false;			// step() answered Run and ran() has not been called
	std::vector<Sum> localSums, remoteSums;

	bool paused = false, remotePaused = false;
	unsigned resyncs = 0;
};

Shared g;
// host(), join() and stop() one at a time.
std::mutex controlMutex;
std::thread thread;
// Used by the frame loop's thread only.
Hooks hooks;
std::atomic<int> pingMs{-1};
std::atomic<double> lastReceive{0};

// ------------------------------------------------------------------ messages

void put16(std::vector<u8>& to, u16 value)
{
	to.push_back((u8)value);
	to.push_back((u8)(value >> 8));
}

void put32(std::vector<u8>& to, u32 value)
{
	for (int shift = 0; shift < 32; shift += 8)
		to.push_back((u8)(value >> shift));
}

void put64(std::vector<u8>& to, u64 value)
{
	for (int shift = 0; shift < 64; shift += 8)
		to.push_back((u8)(value >> shift));
}

void putText(std::vector<u8>& to, const std::string& text)
{
	const size_t length = std::min<size_t>(text.size(), 0xffff);
	put16(to, (u16)length);
	to.insert(to.end(), text.begin(), text.begin() + (ptrdiff_t)length);
}

// A message's header; endMessage() fills its length in.
size_t beginMessage(std::vector<u8>& to, u8 type)
{
	const size_t start = to.size();
	to.push_back(Magic0);
	to.push_back(Magic1);
	to.push_back(type);
	to.push_back(0);
	put32(to, 0);
	return start;
}

void endMessage(std::vector<u8>& to, size_t start)
{
	const u32 length = (u32)(to.size() - start - HeaderBytes);
	for (int i = 0; i < 4; i++)
		to[start + 4 + (size_t)i] = (u8)(length >> (8 * i));
}

// Reads a message's fields; `ok` goes false when the message is too short.
struct Reader
{
	const u8 *at, *end;
	bool ok = true;

	Reader(const u8 *data, size_t size) : at(data), end(data + size) {}

	size_t left() const
	{
		return (size_t)(end - at);
	}

	u64 number(int bytes)
	{
		if (left() < (size_t)bytes)
		{
			ok = false;
			at = end;
			return 0;
		}
		u64 value = 0;
		for (int i = 0; i < bytes; i++)
			value |= (u64)at[i] << (8 * i);
		at += bytes;
		return value;
	}

	u8 get8() { return (u8)number(1); }
	u16 get16() { return (u16)number(2); }
	u32 get32() { return (u32)number(4); }
	u64 get64() { return number(8); }

	std::string text()
	{
		const size_t length = get16();
		if (left() < length)
		{
			ok = false;
			at = end;
			return "";
		}
		std::string value(reinterpret_cast<const char *>(at), length);
		at += length;
		return value;
	}
};

// ----------------------------------------------------------- the packed state

u64 load64(const u8 *at)
{
	u64 value;
	memcpy(&value, at, sizeof(value));
	return value;
}

void putCount(std::vector<u8>& to, u64 value)
{
	while (value >= 0x80)
	{
		to.push_back((u8)(value | 0x80));
		value >>= 7;
	}
	to.push_back((u8)value);
}

bool getCount(const u8 *& at, const u8 *end, u64& value)
{
	value = 0;
	for (int shift = 0; shift < 64; shift += 7)
	{
		if (at == end)
			return false;
		const u8 byte = *at++;
		value |= (u64)(byte & 0x7f) << shift;
		if ((byte & 0x80) == 0)
			return true;
	}
	return false;
}

// The next run of MinZeroRun zeros or more at or after `from`: where it starts
// and ends, both `size` when there is none. Eight bytes are looked at at a
// time; a run that long has one such step wholly inside it.
void findZeroRun(const u8 *data, size_t size, size_t from, size_t& start, size_t& end)
{
	size_t at = from;
	while (at + 8 <= size)
	{
		if (load64(data + at) != 0)
		{
			at += 8;
			continue;
		}
		start = at;
		while (start > from && data[start - 1] == 0)
			start--;
		end = at + 8;
		while (end + 8 <= size && load64(data + end) == 0)
			end += 8;
		while (end < size && data[end] == 0)
			end++;
		if (end - start >= MinZeroRun)
			return;
		at = end;
	}
	start = end = size;
}

void packZeros(const u8 *data, size_t size, std::vector<u8>& out)
{
	out.clear();
	size_t at = 0;
	while (at < size)
	{
		size_t start, end;
		findZeroRun(data, size, at, start, end);
		putCount(out, start - at);
		out.insert(out.end(), data + at, data + start);
		putCount(out, end - start);
		at = end;
	}
}

// False when the packed bytes do not make exactly `size` bytes.
bool unpackZeros(const u8 *data, size_t packed, size_t size, std::vector<u8>& out)
{
	out.assign(size, 0);
	const u8 *at = data, *const end = data + packed;
	size_t to = 0;
	while (at < end)
	{
		u64 copy, zeros;
		if (!getCount(at, end, copy) || copy > (u64)(end - at) || copy > size - to)
			return false;
		// A count of zero comes with no bytes, and a state of no bytes has no
		// address to copy to.
		if (copy != 0)
			memcpy(out.data() + to, at, (size_t)copy);
		at += copy;
		to += (size_t)copy;
		if (!getCount(at, end, zeros) || zeros > size - to)
			return false;
		to += (size_t)zeros;
	}
	return to == size;
}

// A sum of the state as it was saved, so that a state unpacked wrongly is
// never loaded.
u32 stateSum(const u8 *data, size_t size)
{
	u64 sum = 0x9e3779b97f4a7c15ull ^ size;
	size_t at = 0;
	for (; at + 8 <= size; at += 8)
	{
		sum = (sum ^ load64(data + at)) * 0x9e3779b97f4a7c15ull;
		sum = (sum << 29) | (sum >> 35);
	}
	for (; at < size; at++)
		sum = (sum ^ data[at]) * 0x100000001b3ull;
	return (u32)(sum ^ (sum >> 32));
}

// -------------------------------------------------------------------- sockets

#if defined(MSG_NOSIGNAL)
constexpr int NoSignal = MSG_NOSIGNAL;
#else
constexpr int NoSignal = 0;
#endif
// NETPLAY_PLAIN_SOCKETS (the tests) builds this file as it runs where a
// socket cannot be told not to wait.
#if defined(MSG_DONTWAIT) && !defined(NETPLAY_PLAIN_SOCKETS)
constexpr int NoWait = MSG_DONTWAIT;
#else
constexpr int NoWait = 0;
#endif

bool makeAddress(const std::string& host, int port, sockaddr_in& address)
{
	address = sockaddr_in{};
	if (port <= 0 || port > 65535 || inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
		return false;
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)port);
#if defined(__PROSPERO__) || defined(__FreeBSD__)
	address.sin_len = sizeof(address);
#endif
	return true;
}

std::string addressText(const sockaddr_in& address)
{
	char text[INET_ADDRSTRLEN] = "?";
	inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text));
	return format("%s:%d", text, (int)ntohs(address.sin_port));
}

void setNonBlocking(int fd, bool on)
{
#if defined(NETPLAY_PLAIN_SOCKETS)
	(void)fd;
	(void)on;
#else
	const int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
#endif
}

void closeOnExec(int fd)
{
	const int flags = fcntl(fd, F_GETFD, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

// What the connection to the other side is given once it is made.
void prepareSocket(int fd)
{
	closeOnExec(fd);
	setNonBlocking(fd, false);
	const int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#if defined(SO_NOSIGPIPE)
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	// No read waits here (poll is asked first) and a write is a small piece
	// after poll said there is room; the limits are for what poll got wrong.
	const timeval limit = { SocketLimitSeconds, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
}

// ------------------------------------------- the shared state (mutex held)

bool isActive(State state)
{
	return state != State::Off && state != State::Ended && state != State::Failed;
}

// The thread is to wake from its poll: something was queued or changed. The
// byte is written when the mutex is let go (Locked); the thread itself needs
// none, it looks again after everything it does.
void wakeLocked()
{
	if (g.wakePending)
		return;
	g.wakePending = true;
	g.wakeWanted = true;
}

// The mutex as the frame loop's side takes it. The byte that wakes the thread
// is written after the mutex is let go, so that the thread does not wake only
// to wait for the mutex. At most one byte is written for each time the thread
// looks, so the write never waits.
struct Locked
{
	std::unique_lock<std::mutex> lock;

	Locked() : lock(g.mutex) {}

	~Locked()
	{
		release();
	}

	void release()
	{
		if (!lock.owns_lock())
			return;
		const int fd = g.wakeWanted && g.wakeUsable ? g.wakeWrite : -1;
		g.wakeWanted = false;
		lock.unlock();
		const u8 byte = 1;
		if (fd >= 0)
			(void)!write(fd, &byte, 1);
	}

	void take()
	{
		lock.lock();
	}
};

void queueSimpleLocked(u8 type, u32 value)
{
	if (!g.connected)
		return;
	const size_t start = beginMessage(g.out, type);
	put32(g.out, value);
	endMessage(g.out, start);
	wakeLocked();
}

void queueTextLocked(u8 type, const std::string& text)
{
	if (!g.connected)
		return;
	const size_t start = beginMessage(g.out, type);
	putText(g.out, text);
	endMessage(g.out, start);
	wakeLocked();
}

// The session is over: the first reason given stays.
void finishLocked(State state, const std::string& error)
{
	if (!isActive(g.state))
		return;
	g.state = state;
	g.error = error;
	g.sync = Sync::None;
	g.stateBytes = std::vector<u8>();
	diag::mark("netplay: %s: %s", state == State::Failed ? "failed" : "ended", error.empty() ? "stopped" : error.c_str());
	wakeLocked();
}

// Nothing recorded or received counts any more.
void resetPlayLocked()
{
	g.frame = 0;
	g.localNext = g.remoteNext = (u64)g.delay;
	g.localInputs.clear();
	g.remoteInputs.clear();
	g.runGiven = false;
	g.localSums.clear();
	g.remoteSums.clear();
}

void startPlayingLocked()
{
	resetPlayLocked();
	g.sync = Sync::None;
	g.state = State::Playing;
	diag::mark("netplay: playing (epoch %u, delay %d)", g.epoch, g.delay);
}

// Host: the two are out of step. From here neither runs a frame until the
// guest has loaded the state the next step() saves.
void beginResyncLocked()
{
	g.epoch++;
	g.resyncs++;
	g.state = State::Syncing;
	g.sync = Sync::NeedSave;
	g.stateBytes = std::vector<u8>();
	resetPlayLocked();
	queueSimpleLocked(Resync, g.epoch);
}

// A checksum of this side or of the other was added: compares the two of the
// same frame, once both are there.
void compareSumsLocked()
{
	for (size_t i = 0; i < g.localSums.size(); i++)
		for (size_t j = 0; j < g.remoteSums.size(); j++)
		{
			if (g.localSums[i].frame != g.remoteSums[j].frame)
				continue;
			const Sum mine = g.localSums[i], theirs = g.remoteSums[j];
			g.localSums.erase(g.localSums.begin() + (ptrdiff_t)i);
			g.remoteSums.erase(g.remoteSums.begin() + (ptrdiff_t)j);
			if (mine.value != theirs.value)
			{
				diag::mark("netplay: out of step at frame %u (%08x here, %08x there): %s", mine.frame, mine.value, theirs.value,
						g.hosting ? "sending the state again" : "the host will send its state");
				if (g.hosting)
					beginResyncLocked();
			}
			return;
		}
	// One side far behind the other in its checksums cannot happen in
	// lockstep; the lists stay short all the same.
	if (g.localSums.size() > 16)
		g.localSums.erase(g.localSums.begin());
	if (g.remoteSums.size() > 16)
		g.remoteSums.erase(g.remoteSums.begin());
}

// ------------------------------------------------------- the network thread

// What only the thread touches.
struct Link
{
	int fd = -1;
	bool peerGone = false;			// the other side closed, or said BYE
	bool greeted = false;
	double connectedAt = 0, pingAt = 0;
	double pingAverage = -1;

	std::vector<u8> in;				// received, not yet a whole message
	std::vector<u8> sending;		// to send; `sent` bytes of it are gone
	size_t sent = 0;
	std::vector<u8> block;			// for recv

	// Host: the packed state on its way.
	std::vector<u8> packed;
	size_t packedAt = 0;
	u32 stateEpoch = 0, stateSize = 0, stateSumValue = 0;
	bool packedBusy = false;

	// Guest: the packed state arriving.
	std::vector<u8> arriving;
	u32 arrivingEpoch = 0, arrivingSize = 0, arrivingPacked = 0, arrivingSum = 0;
};

void sendText(Link& link, u8 type, const std::string& text)
{
	const size_t start = beginMessage(link.sending, type);
	putText(link.sending, text);
	endMessage(link.sending, start);
}

void sendHello(Link& link)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	const size_t start = beginMessage(link.sending, Hello);
	put32(link.sending, ProtocolVersion);
	putText(link.sending, format("%d", BuildNumber));
	putText(link.sending, g.session.game);
	putText(link.sending, g.session.bios);
	link.sending.push_back(g.hosting ? 1 : 0);
	link.sending.push_back((u8)g.delay);
	const std::string& settings = g.hosting ? g.session.settings : std::string();
	put32(link.sending, (u32)settings.size());
	link.sending.insert(link.sending.end(), settings.begin(), settings.end());
	endMessage(link.sending, start);
}

// The session fails here and the other side is told why, in its own words.
void failBoth(Link& link, const std::string& here, const std::string& there)
{
	sendText(link, Refuse, there);
	std::lock_guard<std::mutex> lock(g.mutex);
	finishLocked(State::Failed, here);
}

void protocolError(Link& link, const char *what)
{
	diag::mark("netplay: not understood: %s", what);
	failBoth(link, "The other side sent something that is not PSSwanStation netplay.",
			"The other side could not read what this one sent.");
}

std::string shown(const std::string& text)
{
	return text.empty() ? std::string("none") : text;
}

std::string differs(const char *what, const std::string& here, const std::string& there)
{
	return format("The other player has a different %s: %s here, %s there.", what, shown(here).c_str(), shown(there).c_str());
}

void handleHello(Link& link, Reader& r)
{
	const u32 version = r.get32();
	const std::string build = r.text();
	if (!r.ok || link.greeted)
	{
		protocolError(link, "HELLO");
		return;
	}
	std::string ownGame, ownBios;
	bool hosting;
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		ownGame = g.session.game;
		ownBios = g.session.bios;
		hosting = g.hosting;
	}
	const std::string ownBuild = format("%d", BuildNumber);
	if (version != ProtocolVersion)
	{
		// The rest of another version's HELLO is not read.
		const std::string own = build == ownBuild ? format("netplay %u", ProtocolVersion) : "build " + ownBuild;
		const std::string other = build == ownBuild ? format("netplay %u", version) : "build " + build;
		const char *what = "version of PSSwanStation";
		if (hosting)
			failBoth(link, differs(what, own, other), differs(what, other, own));
		else
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			finishLocked(State::Failed, differs(what, own, other));
		}
		return;
	}
	const std::string game = r.text(), bios = r.text();
	const bool otherHosts = r.get8() != 0;
	const int delay = r.get8();
	const size_t settingsBytes = r.get32();
	if (!r.ok || settingsBytes != r.left() || otherHosts == hosting)
	{
		protocolError(link, "HELLO");
		return;
	}
	const std::string settings(reinterpret_cast<const char *>(r.at), settingsBytes);
	const char *what = game != ownGame ? "game" : bios != ownBios ? "BIOS" : nullptr;
	if (what != nullptr)
	{
		const std::string& own = game != ownGame ? ownGame : ownBios;
		const std::string& other = game != ownGame ? game : bios;
		// The guest has the host's HELLO and says the same in its own words;
		// the host's REFUSE is for a guest that did not get that far.
		if (hosting)
			failBoth(link, differs(what, own, other), differs(what, other, own));
		else
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			finishLocked(State::Failed, differs(what, own, other));
		}
		return;
	}
	link.greeted = true;
	std::lock_guard<std::mutex> lock(g.mutex);
	if (g.state != State::Greeting)
		return;
	if (!hosting)
	{
		g.hostSettings = settings;
		g.delay = std::clamp(delay, 1, 10);
	}
	g.epoch = 1;
	g.state = State::Syncing;
	g.sync = hosting ? Sync::NeedSave : Sync::Receiving;
	diag::mark("netplay: greeted (the other side is build %s; game %s, delay %d): the state follows", build.c_str(),
			shown(game).c_str(), g.delay);
}

void handleResync(Link& link, Reader& r)
{
	const u32 epoch = r.get32();
	std::unique_lock<std::mutex> lock(g.mutex);
	if (!r.ok || g.hosting || !link.greeted || epoch <= g.epoch)
	{
		lock.unlock();
		protocolError(link, "RESYNC");
		return;
	}
	if (g.state != State::Syncing && g.state != State::Playing)
		return;
	g.epoch = epoch;
	g.resyncs++;
	g.state = State::Syncing;
	g.sync = Sync::Receiving;
	g.stateBytes = std::vector<u8>();
	resetPlayLocked();
	link.arriving = std::vector<u8>();
	diag::mark("netplay: the host sends its state again (epoch %u)", epoch);
}

void handleState(Link& link, Reader& r)
{
	const u32 epoch = r.get32(), size = r.get32(), packed = r.get32(), sum = r.get32(), offset = r.get32();
	if (!r.ok)
	{
		protocolError(link, "STATE");
		return;
	}
	{
		std::unique_lock<std::mutex> lock(g.mutex);
		if (!isActive(g.state) || (!g.hosting && epoch < g.epoch))
			return;		// the session is over, or the piece is of a transfer the host gave up
		if (g.hosting || g.state != State::Syncing || g.sync != Sync::Receiving || epoch != g.epoch)
		{
			lock.unlock();
			protocolError(link, "a piece of state nobody waits for");
			return;
		}
	}
	if (offset == 0)
	{
		if (size == 0 || size > MaxStateBytes || packed == 0 || packed > MaxStateBytes)
		{
			protocolError(link, "STATE size");
			return;
		}
		link.arriving.clear();
		link.arriving.reserve(packed);
		link.arrivingEpoch = epoch;
		link.arrivingSize = size;
		link.arrivingPacked = packed;
		link.arrivingSum = sum;
	}
	if (epoch != link.arrivingEpoch || size != link.arrivingSize || packed != link.arrivingPacked
			|| offset != link.arriving.size() || r.left() == 0 || r.left() > packed - offset)
	{
		protocolError(link, "STATE piece");
		return;
	}
	link.arriving.insert(link.arriving.end(), r.at, r.end);
	if (link.arriving.size() < packed)
		return;
	const double started = now();
	std::vector<u8> state;
	const bool whole = unpackZeros(link.arriving.data(), link.arriving.size(), size, state)
			&& stateSum(state.data(), state.size()) == sum;
	link.arriving = std::vector<u8>();
	if (!whole)
	{
		failBoth(link, "The host's state did not arrive whole.", "The state did not arrive whole on the other side.");
		return;
	}
	diag::mark("netplay: the state arrived: %u bytes from %u (unpacked in %.0f ms)", size, packed, (now() - started) * 1000.0);
	std::lock_guard<std::mutex> lock(g.mutex);
	if (g.state != State::Syncing || g.sync != Sync::Receiving || g.epoch != epoch)
		return;
	g.stateBytes = std::move(state);
	g.sync = Sync::Arrived;
}

void handleStateOk(Link& link, Reader& r)
{
	const u32 epoch = r.get32();
	std::unique_lock<std::mutex> lock(g.mutex);
	if (!r.ok || !g.hosting)
	{
		lock.unlock();
		protocolError(link, "STATE_OK");
		return;
	}
	if (g.state == State::Syncing && g.sync == Sync::Sending && g.epoch == epoch)
		startPlayingLocked();
}

void handleInput(Link& link, Reader& r)
{
	const u32 epoch = r.get32(), frame = r.get32();
	Input input;
	input.buttons = r.get16();
	input.lx = (int16_t)r.get16();
	input.ly = (int16_t)r.get16();
	input.rx = (int16_t)r.get16();
	input.ry = (int16_t)r.get16();
	std::unique_lock<std::mutex> lock(g.mutex);
	if (!r.ok)
	{
		lock.unlock();
		protocolError(link, "INPUT");
		return;
	}
	// Sent before the other side knew of the resync.
	if (epoch != g.epoch || g.state != State::Playing)
		return;
	if (frame != (u32)g.remoteNext || g.remoteInputs.size() >= MaxQueuedInputs)
	{
		lock.unlock();
		protocolError(link, "INPUT out of order");
		return;
	}
	g.remoteInputs.push_back(input);
	g.remoteNext++;
}

void handleCheck(Link& link, Reader& r)
{
	const u32 epoch = r.get32(), frame = r.get32(), value = r.get32();
	std::unique_lock<std::mutex> lock(g.mutex);
	if (!r.ok)
	{
		lock.unlock();
		protocolError(link, "CHECK");
		return;
	}
	if (epoch != g.epoch || g.state != State::Playing)
		return;
	g.remoteSums.push_back({frame, value});
	compareSumsLocked();
}

// One whole message. False when nothing more of what was received counts.
bool handleMessage(Link& link, u8 type, const u8 *data, size_t size)
{
	Reader r(data, size);
	if (!link.greeted && type != Hello && type != Refuse && type != Bye)
	{
		protocolError(link, "a message before HELLO");
		return false;
	}
	switch (type)
	{
	case Hello:
		handleHello(link, r);
		break;
	case Refuse:
	{
		const std::string text = r.text();
		std::lock_guard<std::mutex> lock(g.mutex);
		finishLocked(State::Failed, r.ok && !text.empty() ? text : "The other side refused.");
		break;
	}
	case Resync:
		handleResync(link, r);
		break;
	case StateData:
		handleState(link, r);
		break;
	case StateOk:
		handleStateOk(link, r);
		break;
	case InputData:
		handleInput(link, r);
		break;
	case Check:
		handleCheck(link, r);
		break;
	case Pause:
	{
		const bool on = r.get8() != 0;
		std::lock_guard<std::mutex> lock(g.mutex);
		g.remotePaused = r.ok && on;
		break;
	}
	case Ping:
	{
		const size_t start = beginMessage(link.sending, Pong);
		link.sending.insert(link.sending.end(), data, data + std::min<size_t>(size, 8));
		endMessage(link.sending, start);
		break;
	}
	case Pong:
	{
		const u64 sentAt = r.get64();
		const double took = now() * 1000.0 - (double)sentAt / 1000.0;
		if (r.ok && took >= 0 && took < SilenceSeconds * 1000.0)
		{
			link.pingAverage = link.pingAverage < 0 ? took : link.pingAverage * 0.75 + took * 0.25;
			pingMs = (int)(link.pingAverage + 0.5);
		}
		break;
	}
	case Bye:
	{
		link.peerGone = true;
		std::lock_guard<std::mutex> lock(g.mutex);
		finishLocked(State::Ended, "The other player left.");
		break;
	}
	default:
		protocolError(link, "an unknown message");
		break;
	}
	std::lock_guard<std::mutex> lock(g.mutex);
	return isActive(g.state);
}

void connectionLost(Link& link)
{
	link.peerGone = true;
	std::lock_guard<std::mutex> lock(g.mutex);
	finishLocked(State::Ended, "The other player left.");
}

// Reads what there is (poll said there is something) and handles the whole
// messages in it.
void receive(Link& link)
{
	link.block.resize(ReceiveBlock);
	const ssize_t got = recv(link.fd, link.block.data(), link.block.size(), NoWait);
	if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
		return;
	if (got <= 0)
	{
		connectionLost(link);
		return;
	}
	lastReceive = now();
	link.in.insert(link.in.end(), link.block.begin(), link.block.begin() + got);
	size_t at = 0;
	while (link.in.size() - at >= HeaderBytes)
	{
		const u8 *header = link.in.data() + at;
		const size_t length = (size_t)header[4] | (size_t)header[5] << 8 | (size_t)header[6] << 16 | (size_t)header[7] << 24;
		if (header[0] != Magic0 || header[1] != Magic1 || header[3] != 0 || length > MaxMessage)
		{
			protocolError(link, "a header");
			at = link.in.size();
			break;
		}
		if (link.in.size() - at - HeaderBytes < length)
			break;
		const bool more = handleMessage(link, header[2], header + HeaderBytes, length);
		at += HeaderBytes + length;
		if (!more)
		{
			at = link.in.size();
			break;
		}
	}
	link.in.erase(link.in.begin(), link.in.begin() + (ptrdiff_t)at);
}

// One piece of what waits (poll said there is room). False when the
// connection broke.
bool sendPiece(Link& link)
{
	if (link.sent >= link.sending.size())
		return true;
	const size_t want = std::min(link.sending.size() - link.sent, SendPiece);
	const ssize_t done = send(link.fd, link.sending.data() + link.sent, want, NoSignal | NoWait);
	if (done < 0)
		return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
	link.sent += (size_t)done;
	if (link.sent == link.sending.size())
	{
		link.sending.clear();
		link.sent = 0;
	}
	else if (link.sent >= 256 * 1024)
	{
		link.sending.erase(link.sending.begin(), link.sending.begin() + (ptrdiff_t)link.sent);
		link.sent = 0;
	}
	return true;
}

// Host: packs the state step() saved.
void packState(Link& link, std::vector<u8>& state)
{
	const double started = now();
	link.stateSize = (u32)state.size();
	link.stateSumValue = stateSum(state.data(), state.size());
	packZeros(state.data(), state.size(), link.packed);
	link.packedAt = 0;
	link.packedBusy = true;
	diag::mark("netplay: sending the state: %zu bytes as %zu (packed in %.0f ms)", state.size(), link.packed.size(),
			(now() - started) * 1000.0);
	state = std::vector<u8>();
}

// Host: the next piece of the packed state, when the one before has gone. The
// pieces go one at a time so that other messages travel between them.
void feedState(Link& link)
{
	if (!link.packedBusy || link.sending.size() - link.sent >= SendPiece)
		return;
	const size_t count = std::min(StatePiece, link.packed.size() - link.packedAt);
	const size_t start = beginMessage(link.sending, StateData);
	put32(link.sending, link.stateEpoch);
	put32(link.sending, link.stateSize);
	put32(link.sending, (u32)link.packed.size());
	put32(link.sending, link.stateSumValue);
	put32(link.sending, (u32)link.packedAt);
	link.sending.insert(link.sending.end(), link.packed.begin() + (ptrdiff_t)link.packedAt,
			link.packed.begin() + (ptrdiff_t)(link.packedAt + count));
	endMessage(link.sending, start);
	link.packedAt += count;
	if (link.packedAt == link.packed.size())
	{
		link.packed = std::vector<u8>();
		link.packedAt = 0;
		link.packedBusy = false;
	}
}

// Closes the connection so that what was sent last arrives: the other side
// reads to the end and closes, which is waited for a moment.
void closeLink(Link& link)
{
	if (link.fd < 0)
		return;
	if (!link.peerGone)
	{
		const double flushBy = now() + 0.3;
		while (link.sent < link.sending.size() && now() < flushBy)
		{
			pollfd room{};
			room.fd = link.fd;
			room.events = POLLOUT;
			if (poll(&room, 1, 50) > 0 && ((room.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 || !sendPiece(link)))
				break;
		}
		shutdown(link.fd, SHUT_WR);
		const double closedBy = now() + 0.15;
		link.block.resize(ReceiveBlock);
		while (now() < closedBy)
		{
			pollfd ready{};
			ready.fd = link.fd;
			ready.events = POLLIN;
			if (poll(&ready, 1, 20) <= 0)
				continue;
			if (recv(link.fd, link.block.data(), link.block.size(), NoWait) <= 0)
				break;
		}
	}
	close(link.fd);
	link.fd = -1;
}

// What poll said of the pipe: empties it, or gives it up when it is not what
// it should be (nobody writes to it again, and the thread looks often instead).
void wakeAnswered(int& wake, short events)
{
	if (wake < 0 || events == 0)
		return;
	if ((events & POLLIN) != 0)
	{
		u8 bytes[16];
		(void)!read(wake, bytes, sizeof(bytes));
		return;
	}
	std::lock_guard<std::mutex> lock(g.mutex);
	g.wakeUsable = false;
	wake = -1;
}

// Host: waits for the other player on the listening socket. The connection,
// or -1 when the session was stopped first.
int acceptPeer()
{
	int listener, wake;
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		listener = g.listenFd;
		wake = g.wakeUsable ? g.wakeRead : -1;
	}
	int fd = -1;
	while (fd < 0)
	{
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.wakePending = g.wakeWanted = false;
			if (g.quit)
				break;
		}
		pollfd fds[2]{};
		fds[0].fd = listener;
		fds[0].events = POLLIN;
		fds[1].fd = wake;
		fds[1].events = POLLIN;
		const int ready = poll(fds, wake >= 0 ? 2 : 1, 100);
		if (ready < 0 && errno != EINTR)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			finishLocked(State::Failed, format("Waiting for the other player failed: %s.", strerror(errno)));
			break;
		}
		if (ready <= 0)
			continue;
		wakeAnswered(wake, fds[1].revents);
		if ((fds[0].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) == 0)
			continue;
		sockaddr_in from{};
		socklen_t length = sizeof(from);
		fd = accept(listener, reinterpret_cast<sockaddr *>(&from), &length);
		if (fd < 0)
		{
			const int error = errno;
			// The one who knocked went away again.
			if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK || error == ECONNABORTED)
				continue;
			std::lock_guard<std::mutex> lock(g.mutex);
			// stop() shuts the listening socket down, which is an error here.
			if (!g.quit)
				finishLocked(State::Failed, format("Waiting for the other player failed: %s.", strerror(error)));
			break;
		}
		diag::mark("netplay: %s joined", addressText(from).c_str());
	}
	// One other player: whoever knocks from now on is refused by the system.
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.listenFd = -1;
	}
	close(listener);
	return fd;
}

// A connect on a thread of its own, so that one the system will not make
// non-blocking costs its caller no more than the time it chose to wait.
struct ConnectAttempt
{
	std::mutex mutex;
	std::condition_variable done;
	bool finished = false;
	bool abandoned = false;		// nobody waits any more: the thread closes its socket
	int trying = -1;			// the socket while the connect is being made
	int fd = -1;				// the socket once it is connected
	int error = 0;				// errno; 0 when connected
};

void connectThread(std::shared_ptr<ConnectAttempt> attempt, sockaddr_in address)
{
	int error = 0;
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		error = errno != 0 ? errno : EIO;
	else
	{
		closeOnExec(fd);
		setNonBlocking(fd, true);
		{
			std::lock_guard<std::mutex> lock(attempt->mutex);
			attempt->trying = fd;
		}
		if (connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
		{
			error = errno != 0 ? errno : EIO;
			if (error == EINPROGRESS || error == EINTR)
			{
				// The non-blocking way: the socket is writable when it is
				// connected, or in error when it is not.
				const double deadline = now() + ConnectSeconds;
				error = ETIMEDOUT;
				for (;;)
				{
					{
						std::lock_guard<std::mutex> lock(attempt->mutex);
						if (attempt->abandoned)
							break;
					}
					pollfd ready{};
					ready.fd = fd;
					ready.events = POLLOUT;
					const int state = poll(&ready, 1, 100);
					if (state > 0)
					{
						int result = 0;
						socklen_t length = sizeof(result);
						if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &length) != 0)
							result = errno != 0 ? errno : EIO;
						sockaddr_in other{};
						socklen_t otherLength = sizeof(other);
						if (result == 0 && getpeername(fd, reinterpret_cast<sockaddr *>(&other), &otherLength) != 0)
							result = ENOTCONN;
						error = result;
						break;
					}
					if (state < 0 && errno != EINTR)
					{
						error = errno;
						break;
					}
					if (now() >= deadline)
						break;
				}
			}
		}
	}
	std::lock_guard<std::mutex> lock(attempt->mutex);
	attempt->trying = -1;
	if (fd >= 0 && (error != 0 || attempt->abandoned))
		close(fd);
	else
		attempt->fd = fd;
	attempt->error = error != 0 ? error : attempt->abandoned ? ECANCELED : 0;
	attempt->finished = true;
	attempt->done.notify_all();
}

// Guest: reaches the host. The connection, or -1 (stopped, or Failed).
int connectPeer()
{
	sockaddr_in address;
	std::string name;
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		address = g.peer;
		name = g.peerName;
	}
	const std::shared_ptr<ConnectAttempt> attempt = std::make_shared<ConnectAttempt>();
	std::thread helper(connectThread, attempt, address);
	const double deadline = now() + ConnectSeconds;
	bool stopped = false;
	std::unique_lock<std::mutex> lock(attempt->mutex);
	while (!attempt->finished && !stopped && now() < deadline)
	{
		attempt->done.wait_for(lock, std::chrono::milliseconds(50));
		lock.unlock();
		{
			std::lock_guard<std::mutex> shared(g.mutex);
			stopped = g.quit;
		}
		lock.lock();
	}
	const bool gaveUp = !attempt->finished;
	if (gaveUp)
	{
		// The helper looks at this ten times a second when its connect does
		// not block. When it does, shutting the socket down ends the connect
		// where the system takes that; where it does not, the helper ends
		// when the system gives up, and closes the socket then.
		attempt->abandoned = true;
		if (attempt->trying >= 0)
			shutdown(attempt->trying, SHUT_RDWR);
		attempt->done.wait_for(lock, std::chrono::milliseconds(300), [&] { return attempt->finished; });
	}
	const bool finished = attempt->finished;
	int fd = attempt->fd;
	const int error = finished && !gaveUp ? attempt->error : ETIMEDOUT;
	attempt->fd = -1;
	lock.unlock();
	if (finished)
		helper.join();
	else
		helper.detach();
	if (stopped)
	{
		if (fd >= 0)
			close(fd);
		return -1;
	}
	if (fd >= 0 && error == 0)
		return fd;
	if (fd >= 0)
		close(fd);
	std::string text;
	if (error == ECONNREFUSED)
		text = format("Nobody is hosting at %s: the connection was refused.", name.c_str());
	else if (error == ETIMEDOUT || error == ECANCELED)
		text = format("The host at %s did not answer in %d seconds.", name.c_str(), (int)ConnectSeconds);
	else
		text = format("The host at %s cannot be reached: %s.", name.c_str(), strerror(error));
	std::lock_guard<std::mutex> shared(g.mutex);
	finishLocked(State::Failed, text);
	return -1;
}

// The connection, from the greeting to its end.
void serve(Link& link)
{
	int wake;
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		wake = g.wakeUsable ? g.wakeRead : -1;
	}
	for (;;)
	{
		std::vector<u8> state;
		bool leaving = false, over = false;
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			g.wakePending = g.wakeWanted = false;
			if (!g.out.empty())
			{
				link.sending.insert(link.sending.end(), g.out.begin(), g.out.end());
				g.out.clear();
			}
			if (g.quit)
				leaving = true;
			else if (!isActive(g.state))
				over = true;
			else if (g.sync == Sync::Saved)
			{
				state.swap(g.stateBytes);
				link.stateEpoch = g.epoch;
				g.sync = Sync::Sending;
			}
		}
		if (leaving)
		{
			const size_t start = beginMessage(link.sending, Bye);
			endMessage(link.sending, start);
			break;
		}
		if (over)
			break;
		if (!state.empty())
			packState(link, state);
		feedState(link);

		pollfd fds[2]{};
		fds[0].fd = link.fd;
		fds[0].events = (short)(POLLIN | (link.sent < link.sending.size() ? POLLOUT : 0));
		fds[1].fd = wake;
		fds[1].events = POLLIN;
		// Without the pipe the thread looks every other millisecond.
		const int ready = poll(fds, wake >= 0 ? 2 : 1, wake >= 0 ? 100 : 2);
		if (ready < 0 && errno != EINTR)
		{
			connectionLost(link);
			break;
		}
		if (ready > 0)
		{
			wakeAnswered(wake, fds[1].revents);
			if ((fds[0].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0)
				receive(link);
			if (!link.peerGone && (fds[0].revents & POLLOUT) != 0 && !sendPiece(link))
				connectionLost(link);
		}

		const double time = now();
		if (time - link.pingAt >= PingSeconds)
		{
			link.pingAt = time;
			const size_t start = beginMessage(link.sending, Ping);
			put64(link.sending, (u64)(time * 1e6));
			endMessage(link.sending, start);
		}
		if (time - lastReceive.load() >= SilenceSeconds)
		{
			link.peerGone = true;
			std::lock_guard<std::mutex> lock(g.mutex);
			finishLocked(State::Ended, "The other player stopped answering.");
		}
		else if (!link.greeted && time - link.connectedAt >= GreetingSeconds)
		{
			std::lock_guard<std::mutex> lock(g.mutex);
			finishLocked(State::Failed, "The other side did not answer as PSSwanStation netplay does.");
		}
	}
}

void run()
{
	bool hosting;
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		hosting = g.hosting;
	}
	Link link;
	link.fd = hosting ? acceptPeer() : connectPeer();
	if (link.fd < 0)
		return;
	prepareSocket(link.fd);
	link.connectedAt = now();
	lastReceive = link.connectedAt;
	sendHello(link);
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		if (isActive(g.state))
		{
			g.state = State::Greeting;
			g.connected = true;
			if (g.paused)
			{
				const size_t start = beginMessage(g.out, Pause);
				g.out.push_back(1);
				endMessage(g.out, start);
			}
		}
	}
	serve(link);
	{
		// What step() queued as the session ended (a REFUSE) goes out too.
		std::lock_guard<std::mutex> lock(g.mutex);
		link.sending.insert(link.sending.end(), g.out.begin(), g.out.end());
		g.out.clear();
		g.connected = false;
	}
	closeLink(link);
}

// --------------------------------------------- starting and stopping (control)

// With controlMutex held: ends the thread and what it had.
void stopLocked()
{
	{
		Locked locked;
		g.quit = true;
		wakeLocked();
		// For an accept that waits although poll said somebody was there (the
		// one who knocked went away in between), where the system ends an
		// accept this way.
		if (g.listenFd >= 0)
			shutdown(g.listenFd, SHUT_RDWR);
	}
	if (thread.joinable())
		thread.join();
	std::lock_guard<std::mutex> lock(g.mutex);
	if (g.listenFd >= 0)
		close(g.listenFd);
	g.listenFd = -1;
	g.connected = false;
	g.quit = false;
	g.wakePending = g.wakeWanted = false;
	g.out = std::vector<u8>();
	g.stateBytes = std::vector<u8>();
	g.sync = Sync::None;
	g.localInputs.clear();
	g.remoteInputs.clear();
	g.runGiven = false;
	if (isActive(g.state))
	{
		g.state = State::Ended;
		g.error.clear();
		diag::mark("netplay: stopped");
	}
}

// With controlMutex held and nothing running: a new session's start.
bool beginLocked(const Session& session, const Hooks& with, bool hosting)
{
	std::lock_guard<std::mutex> lock(g.mutex);
	g.session = session;
	g.session.delay = std::clamp(session.delay, 1, 10);
	g.hosting = hosting;
	g.error.clear();
	g.hostSettings.clear();
	g.delay = g.session.delay;
	g.epoch = 0;
	g.frame = 0;
	g.resyncs = 0;
	g.remotePaused = false;
	g.localSums.clear();
	g.remoteSums.clear();
	pingMs = -1;
	lastReceive = now();
	if (!with.saveState || !with.loadState || !with.checksum)
	{
		g.state = State::Failed;
		g.error = "Netplay was started without the emulator's hooks.";
		return false;
	}
	if (session.game.size() > MaxTextBytes || session.bios.size() > MaxTextBytes || session.settings.size() > MaxSettingsBytes)
	{
		g.state = State::Failed;
		g.error = "The game's name or the settings are too long for netplay.";
		return false;
	}
	hooks = with;
	return true;
}

// The pipe step() wakes the thread with: made for the first session and kept,
// so that a byte written late never goes to some other file that got the
// number. Without a pipe the thread looks often instead.
void openWakeLocked()
{
	if (g.wakeRead >= 0)
		return;
	int ends[2];
	if (pipe(ends) != 0)
	{
		diag::mark("netplay: no pipe here (%s): the network thread looks every 2 ms instead", strerror(errno));
		return;
	}
	for (const int fd : ends)
		closeOnExec(fd);
	g.wakeRead = ends[0];
	g.wakeWrite = ends[1];
	// Tried once before it is relied on: a byte written must show in poll and
	// come back.
	u8 byte = 1;
	pollfd ready{};
	ready.fd = ends[0];
	ready.events = POLLIN;
	g.wakeUsable = write(ends[1], &byte, 1) == 1 && poll(&ready, 1, 100) == 1 && (ready.revents & POLLIN) != 0
			&& read(ends[0], &byte, 1) == 1;
	if (!g.wakeUsable)
		diag::mark("netplay: the pipe does not show in poll here: the network thread looks every 2 ms instead");
}

} // namespace

bool host(const Session& session, const Hooks& with)
{
	std::lock_guard<std::mutex> control(controlMutex);
	stopLocked();
	if (!beginLocked(session, with, true))
		return false;
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons((unsigned short)session.port);
#if defined(__PROSPERO__) || defined(__FreeBSD__)
	address.sin_len = sizeof(address);
#endif
	const int fd = session.port > 0 && session.port <= 65535 ? socket(AF_INET, SOCK_STREAM, 0) : -1;
	const int one = 1;
	if (fd >= 0)
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (fd < 0 || bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 || listen(fd, 1) != 0)
	{
		const std::string why = session.port > 0 && session.port <= 65535 ? strerror(errno) : "not a port number";
		if (fd >= 0)
			close(fd);
		std::lock_guard<std::mutex> lock(g.mutex);
		g.state = State::Failed;
		g.error = format("Port %d cannot be opened for the other player: %s.", session.port, why.c_str());
		diag::mark("netplay: failed: %s", g.error.c_str());
		return false;
	}
	closeOnExec(fd);
	// For the accept, which poll is asked about first, where the system does this.
	setNonBlocking(fd, true);
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.listenFd = fd;
		openWakeLocked();
		g.state = State::Listening;
	}
	diag::mark("netplay: hosting on port %d (game %s, delay %d)", session.port, shown(session.game).c_str(),
			std::clamp(session.delay, 1, 10));
	thread = std::thread(run);
	return true;
}

bool join(const std::string& address, const Session& session, const Hooks& with)
{
	std::lock_guard<std::mutex> control(controlMutex);
	stopLocked();
	if (!beginLocked(session, with, false))
		return false;
	std::string name = address;
	int port = session.port;
	const size_t colon = name.find(':');
	if (colon != std::string::npos)
	{
		port = atoi(name.c_str() + colon + 1);
		name.erase(colon);
	}
	sockaddr_in peer;
	if (!makeAddress(name, port, peer))
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.state = State::Failed;
		g.error = format("\"%s\" is not an address: give the host's IP address, as 192.168.1.20 or 192.168.1.20:%d.",
				address.c_str(), DefaultPort);
		diag::mark("netplay: failed: %s", g.error.c_str());
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		g.peer = peer;
		g.peerName = addressText(peer);
		openWakeLocked();
		g.state = State::Connecting;
	}
	diag::mark("netplay: joining %s (game %s)", addressText(peer).c_str(), shown(session.game).c_str());
	thread = std::thread(run);
	return true;
}

void stop()
{
	std::lock_guard<std::mutex> control(controlMutex);
	stopLocked();
}

State state()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.state;
}

bool active()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	return isActive(g.state);
}

bool hosting()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.hosting;
}

int localPlayer()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.hosting ? 0 : 1;
}

std::string error()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.error;
}

std::string hostSettings()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.hostSettings;
}

std::string localAddress()
{
	// The address the system would send from: a UDP socket "connected" to an
	// address beyond the local network sends nothing and has that address. The
	// others are for a network with no way out, where only a local one routes.
	for (const char *far : {"8.8.8.8", "192.168.1.1", "192.168.0.1", "10.0.0.1", "172.16.0.1"})
	{
		sockaddr_in to;
		if (!makeAddress(far, 53, to))
			continue;
		const int fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			return "";
		sockaddr_in own{};
		socklen_t length = sizeof(own);
		const bool found = connect(fd, reinterpret_cast<const sockaddr *>(&to), sizeof(to)) == 0
				&& getsockname(fd, reinterpret_cast<sockaddr *>(&own), &length) == 0;
		close(fd);
		const u32 number = ntohl(own.sin_addr.s_addr);
		if (!found || number == 0 || (number >> 24) == 127)
			continue;
		char text[INET_ADDRSTRLEN];
		if (inet_ntop(AF_INET, &own.sin_addr, text, sizeof(text)) != nullptr)
			return text;
	}
	return "";
}

int ping()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.connected ? pingMs.load() : -1;
}

int silentSeconds()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	if (!g.connected || !isActive(g.state))
		return 0;
	const double silent = now() - lastReceive.load();
	return silent >= QuietSeconds ? (int)silent : 0;
}

unsigned resyncs()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.resyncs;
}

void setPaused(bool paused)
{
	Locked locked;
	if (g.paused == paused)
		return;
	g.paused = paused;
	if (!g.connected)
		return;
	const size_t start = beginMessage(g.out, Pause);
	g.out.push_back(paused ? 1 : 0);
	endMessage(g.out, start);
	wakeLocked();
}

bool remotePaused()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.remotePaused && isActive(g.state);
}

namespace
{

// The state transfer's part on the frame loop's thread: the host saves, the
// guest loads. The hook runs with the mutex released; what it was for may be
// over when it returns (a resync, the end of the session).
void syncStep(Locked& locked)
{
	const u32 epoch = g.epoch;
	if (g.hosting && g.sync == Sync::NeedSave)
	{
		g.sync = Sync::Saving;
		locked.release();
		std::vector<u8> bytes;
		const bool saved = hooks.saveState(bytes) && !bytes.empty() && bytes.size() <= MaxStateBytes;
		locked.take();
		if (g.state != State::Syncing || g.epoch != epoch || g.sync != Sync::Saving)
			return;
		if (!saved)
		{
			queueTextLocked(Refuse, "The host could not save the game's state.");
			finishLocked(State::Failed, "The game's state could not be saved for the other player.");
			return;
		}
		g.stateBytes = std::move(bytes);
		g.sync = Sync::Saved;
		wakeLocked();
	}
	else if (!g.hosting && g.sync == Sync::Arrived)
	{
		std::vector<u8> bytes = std::move(g.stateBytes);
		g.stateBytes = std::vector<u8>();
		g.sync = Sync::Loading;
		locked.release();
		const bool loaded = hooks.loadState(bytes.data(), bytes.size());
		locked.take();
		if (g.state != State::Syncing || g.epoch != epoch || g.sync != Sync::Loading)
			return;
		if (!loaded)
		{
			queueTextLocked(Refuse, "The other player could not load the game's state.");
			finishLocked(State::Failed, "The host's state could not be loaded.");
			return;
		}
		queueSimpleLocked(StateOk, epoch);
		startPlayingLocked();
	}
}

} // namespace

Step step(const Input& local, Input inputs[Players])
{
	Locked locked;
	if (g.state == State::Syncing)
	{
		syncStep(locked);
		return Step::Idle;
	}
	if (g.state != State::Playing)
		return Step::Idle;
	if (g.paused || g.remotePaused)
		return Step::Wait;
	// One input for each frame number: the next is recorded when the frame
	// `delay` before it is the one to run, however often that frame waits.
	if (g.localNext <= g.frame + (u64)g.delay)
	{
		g.localInputs.push_back(local);
		if (g.connected)
		{
			const size_t start = beginMessage(g.out, InputData);
			put32(g.out, g.epoch);
			put32(g.out, (u32)g.localNext);
			put16(g.out, local.buttons);
			put16(g.out, (u16)local.lx);
			put16(g.out, (u16)local.ly);
			put16(g.out, (u16)local.rx);
			put16(g.out, (u16)local.ry);
			endMessage(g.out, start);
			wakeLocked();
		}
		g.localNext++;
	}
	// The first `delay` frames have nobody's input, on both sides.
	Input mine, theirs;
	if (g.frame >= (u64)g.delay)
	{
		if (g.localInputs.empty() || g.remoteInputs.empty())
			return Step::Wait;
		mine = g.localInputs.front();
		theirs = g.remoteInputs.front();
	}
	inputs[g.hosting ? 0 : 1] = mine;
	inputs[g.hosting ? 1 : 0] = theirs;
	g.runGiven = true;
	return Step::Run;
}

void ran()
{
	u64 frame;
	u32 epoch;
	{
		std::lock_guard<std::mutex> lock(g.mutex);
		// A frame that ran while the two were found out of step is not counted:
		// the state that follows replaces it.
		if (g.state != State::Playing || !g.runGiven)
			return;
		g.runGiven = false;
		if (g.frame >= (u64)g.delay)
		{
			g.localInputs.pop_front();
			g.remoteInputs.pop_front();
		}
		g.frame++;
		if (g.frame % CheckFrames != 0)
			return;
		frame = g.frame;
		epoch = g.epoch;
	}
	const u32 value = hooks.checksum();
	Locked locked;
	if (g.state != State::Playing || g.epoch != epoch || !g.connected)
		return;
	const size_t start = beginMessage(g.out, Check);
	put32(g.out, epoch);
	put32(g.out, (u32)frame);
	put32(g.out, value);
	endMessage(g.out, start);
	wakeLocked();
	g.localSums.push_back({(u32)frame, value});
	compareSumsLocked();
}

uint64_t frame()
{
	std::lock_guard<std::mutex> lock(g.mutex);
	return g.frame;
}

}
