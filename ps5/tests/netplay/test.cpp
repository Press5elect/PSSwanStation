/*
	PSSwanStation - netplay's test: one side of a session, with a made-up
	emulator in place of the real one.

	SPDX-License-Identifier: GPL-3.0-or-later

	The module is one per process, so a session is two of this program: one
	run as "host" and one as "join" (run.sh starts them and compares what they
	print). The emulator is a few megabytes of memory, mostly zeros, that each
	frame mixes with both players' inputs and the frame's number, so one input
	that differs, or comes a frame early, shows in the checksum from then on.
	Each side presses made-up buttons, different ones at every step() call.

	What a side checks itself (a violation ends it with exit code 1):
	- the first `delay` frames have nobody's input;
	- its own input for a frame is the one it gave at the first step() call
	  made while the frame `delay` before was the one to run, and no other;
	- no frame runs while a menu is open on either side;
	- after stop() the process has the threads it had before the session, and
	  the open files and two (the pipe that wakes the network thread, which
	  is kept).

	What it prints for run.sh to compare with the other side:
	  RESULT ... frames= checksum= in0= in1=    at frame --frames: the memory's
	                                            checksum and a sum of each
	                                            player's inputs so far
	  FINAL ... state= error= resyncs= ...      when the session is over

	Exit codes: 0 a RESULT was printed and nothing was violated, 1 a
	violation, 2 the session ended before --frames, 3 the time ran out.

	"raw" is not a side of a session but a stranger on the port: it sends what
	--send names and reads until the connection is closed.
*/
#include "../../src/fe.h"
#include "../../src/netplay.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace fe;
using netplay::Input;

namespace
{

struct Options
{
	std::string role;
	std::string address = "127.0.0.1";
	int port = 21500;
	std::string game = "SLUS-00594", bios = "scph5501";
	int delay = 2;
	uint64_t frames = 3000;
	uint64_t extra = 60;			// the host runs this many frames past --frames, then stops
	int jitterMs = 0;				// a sleep of up to so long between step() calls
	int refreshUs = 0;				// a sleep of this long between step() calls, as a display's refresh; 0: as fast as it goes
	int64_t corruptAt = -1;			// changes a byte of the memory at this frame, once
	int64_t pauseAt = -1;			// opens a menu at this frame
	int pauseMs = 2000;
	double retry = 0;				// join: tries again for so long when nobody is hosting
	double timeout = 60;
	int sessions = 1;				// host: so many sessions one after the other
	uint64_t salt = 77;
	uint64_t seed = 1;
	std::string send = "garbage";	// raw: what to send
	int count = 200;				// raw: how many messages of noise
};

uint64_t mix(uint64_t value)
{
	value ^= value >> 33;
	value *= 0xff51afd7ed558ccdull;
	value ^= value >> 33;
	value *= 0xc4ceb9fe1a85ec53ull;
	value ^= value >> 33;
	return value;
}

struct Random
{
	uint64_t state;

	explicit Random(uint64_t seed) : state(mix(seed) | 1) {}

	uint64_t next()
	{
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		return state;
	}
};

uint64_t inputBits(const Input& input)
{
	return ((uint64_t)input.buttons | (uint64_t)(uint16_t)input.lx << 16 | (uint64_t)(uint16_t)input.ly << 32
			| (uint64_t)(uint16_t)input.rx << 48) ^ (uint64_t)(uint16_t)input.ry * 0x9e3779b97f4a7c15ull;
}

bool same(const Input& a, const Input& b)
{
	return a.buttons == b.buttons && a.lx == b.lx && a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
}

bool neutral(const Input& input)
{
	return same(input, Input{});
}

// ------------------------------------------------------- the made-up emulator

constexpr size_t MemoryBytes = (3u << 20) + 123;

struct Emulator
{
	std::vector<uint8_t> memory;
	// A setting that changes what a frame does: the guest must have taken the
	// host's before the first frame.
	uint64_t salt = 0;

	uint64_t word(size_t at) const
	{
		uint64_t value;
		memcpy(&value, memory.data() + at, sizeof(value));
		return value;
	}

	void setWord(size_t at, uint64_t value)
	{
		memcpy(memory.data() + at, &value, sizeof(value));
	}

	// The host's memory has something of every kind in it; the guest's is
	// different, so only the transfer can make them equal.
	void reset(bool rich, uint64_t seed)
	{
		memory.assign(MemoryBytes, 0);
		Random random(seed);
		setWord(0, random.next());
		if (!rich)
			return;
		// Bytes that do not pack.
		for (size_t at = 1u << 20; at < (1u << 20) + 96 * 1024; at++)
			memory[at] = (uint8_t)random.next();
		// Lone bytes far apart.
		for (size_t at = 4096; at < MemoryBytes; at += 4096 + random.next() % 4096)
			memory[at] = (uint8_t)(random.next() | 1);
		// Zero runs around the length where they begin to count as runs.
		for (size_t at = 2u << 20; at < (2u << 20) + 64 * 1024; at += 1 + random.next() % 24)
			memory[at] = (uint8_t)(random.next() | 1);
		// The end is not zero.
		for (size_t at = MemoryBytes - 5; at < MemoryBytes; at++)
			memory[at] = 0xee;
	}

	void run(const Input inputs[netplay::Players])
	{
		uint64_t sum = word(0);
		const uint64_t count = word(8);
		sum = mix(sum ^ salt ^ (count * 0x9e3779b97f4a7c15ull));
		for (int player = 0; player < netplay::Players; player++)
			sum = mix(sum ^ inputBits(inputs[player]) ^ ((uint64_t)(player + 1) << 60));
		memory[16 + sum % (MemoryBytes - 16)] ^= (uint8_t)(sum >> 32);
		setWord(0, sum);
		setWord(8, count + 1);
	}

	uint32_t checksum() const
	{
		uint64_t sum = 1;
		size_t at = 0;
		for (; at + 8 <= memory.size(); at += 8)
			sum = (sum ^ word(at)) * 0x100000001b3ull + (sum >> 29);
		for (; at < memory.size(); at++)
			sum = (sum ^ memory[at]) * 0x100000001b3ull;
		return (uint32_t)(sum ^ (sum >> 32));
	}
};

// ------------------------------------------------------------ what is counted

int countDir(const char *path)
{
	DIR *dir = opendir(path);
	if (dir == nullptr)
		return -1;
	int count = 0;
	while (readdir(dir) != nullptr)
		count++;
	closedir(dir);
	return count;
}

int threadCount()
{
	return countDir("/proc/self/task") - 2;
}

int fileCount()
{
	// Without ".", ".." and the directory being read.
	return countDir("/proc/self/fd") - 3;
}

const char *stateName(netplay::State state)
{
	switch (state)
	{
	case netplay::State::Off: return "Off";
	case netplay::State::Listening: return "Listening";
	case netplay::State::Connecting: return "Connecting";
	case netplay::State::Greeting: return "Greeting";
	case netplay::State::Syncing: return "Syncing";
	case netplay::State::Playing: return "Playing";
	case netplay::State::Ended: return "Ended";
	case netplay::State::Failed: return "Failed";
	}
	return "?";
}

int violations = 0;

void violation(const char *format, ...) __attribute__((format(printf, 1, 2)));
void violation(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	printf("VIOLATION ");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	fflush(stdout);
	violations++;
}

void sleepMs(double ms)
{
	usleep((useconds_t)(ms * 1000.0));
}

// ---------------------------------------------------------------- one session

// 0, 1, 2 or 3 as the program's exit codes.
int runSession(const Options& options, int session)
{
	const bool hosting = options.role == "host";
	const int local = hosting ? 0 : 1;
	Emulator emulator;
	emulator.reset(hosting, options.seed * 1000 + (hosting ? 1 : 2));
	emulator.salt = hosting ? options.salt : 0;
	unsigned saves = 0, loads = 0;

	netplay::Hooks hooks;
	hooks.saveState = [&](std::vector<uint8_t>& out) {
		out = emulator.memory;
		saves++;
		return true;
	};
	hooks.loadState = [&](const uint8_t *data, size_t size) {
		if (size != MemoryBytes)
			return false;
		emulator.memory.assign(data, data + size);
		loads++;
		return true;
	};
	hooks.checksum = [&] { return emulator.checksum(); };

	netplay::Session mine;
	mine.game = options.game;
	mine.bios = options.bios;
	mine.port = options.port;
	// The guest asks for another delay than the host's, which is the one used.
	mine.delay = hosting ? options.delay : options.delay % 10 + 1;
	if (hosting)
		mine.settings = format("salt=%" PRIu64 "\nname=netplay test\n", options.salt);
	const std::string target = format("%s:%d", options.address.c_str(), options.port);

	const double started = now();
	const auto begin = [&] {
		return hosting ? netplay::host(mine, hooks) : netplay::join(target, mine, hooks);
	};
	bool begun = begin();
	if (hosting && begun)
	{
		printf("LISTENING session=%d port=%d address=%s\n", session, options.port, netplay::localAddress().c_str());
		fflush(stdout);
	}

	Random random(options.seed * 7919 + (uint64_t)local + (uint64_t)session * 104729);
	uint64_t sums[netplay::Players] = {1, 2};
	std::unordered_map<uint64_t, Input> given;	// by the frame they are for
	// Frames whose input this side cannot tell: while a menu is open on the
	// other side a step() call records nothing, and the word of the menu
	// may have come just before the call or just after.
	std::unordered_set<uint64_t> unsure;
	std::vector<float> stepTimes;
	stepTimes.reserve(1 << 20);
	unsigned seenResyncs = 0;
	bool settingsTaken = false, resultPrinted = false, corrupted = false, timedOut = false;
	bool pausing = false, paused = false, sawRemotePause = false, sawSilence = false;
	double pauseUntil = 0, lastRun = 0, longestGap = 0;
	uint64_t steps = 0, runs = 0, waits = 0;
	float slowest = 0, slowestNothing = 0;
	uint64_t slowestFrame = 0;

	for (;;)
	{
		const double time = now();
		if (time - started > options.timeout)
		{
			timedOut = true;
			break;
		}
		netplay::State state = begun ? netplay::state() : netplay::State::Failed;
		if (!netplay::active())
		{
			// Nobody hosting yet: once more, while --retry lasts.
			if (!hosting && state == netplay::State::Failed && time - started < options.retry
					&& netplay::error().find("refused") != std::string::npos)
			{
				netplay::stop();
				sleepMs(50);
				begun = begin();
				continue;
			}
			break;
		}
		if (netplay::silentSeconds() >= 5)
			sawSilence = true;
		if (state == netplay::State::Syncing && !hosting && !settingsTaken)
		{
			// As the frontend does: the host's settings, before the next step().
			unsigned long long salt = 0;
			if (sscanf(netplay::hostSettings().c_str(), "salt=%llu", &salt) != 1)
				violation("the host's settings did not arrive: \"%s\"", netplay::hostSettings().c_str());
			emulator.salt = salt;
			settingsTaken = true;
		}
		if (netplay::resyncs() != seenResyncs)
		{
			// The inputs so far were of a game that is gone.
			seenResyncs = netplay::resyncs();
			sums[0] = 1;
			sums[1] = 2;
			given.clear();
			unsure.clear();
		}

		// A menu, once.
		if (options.pauseAt >= 0 && !pausing && !paused && state == netplay::State::Playing
				&& netplay::frame() >= (uint64_t)options.pauseAt)
		{
			netplay::setPaused(true);
			pausing = true;
			pauseUntil = time + options.pauseMs / 1000.0;
		}
		if (pausing && time >= pauseUntil)
		{
			netplay::setPaused(false);
			pausing = false;
			paused = true;
		}
		const bool remoteBefore = netplay::remotePaused();
		sawRemotePause = sawRemotePause || remoteBefore;

		Input offered;
		const uint64_t bits = random.next();
		offered.buttons = (uint16_t)(bits | 1);
		offered.lx = (int16_t)(bits >> 16);
		offered.ly = (int16_t)(bits >> 32);
		offered.rx = (int16_t)(bits >> 48);
		offered.ry = (int16_t)random.next();

		Input inputs[netplay::Players];
		timespec before, after;
		// What the machine does to a stretch of time with nothing in it, to
		// read step()'s slowest call against: the tests share two processors.
		clock_gettime(CLOCK_MONOTONIC, &before);
		clock_gettime(CLOCK_MONOTONIC, &after);
		slowestNothing = std::max(slowestNothing,
				(float)((after.tv_sec - before.tv_sec) * 1e6 + (after.tv_nsec - before.tv_nsec) * 1e-3));
		clock_gettime(CLOCK_MONOTONIC, &before);
		const netplay::Step answer = netplay::step(offered, inputs);
		clock_gettime(CLOCK_MONOTONIC, &after);
		steps++;
		if (answer != netplay::Step::Idle)
		{
			stepTimes.push_back((float)((after.tv_sec - before.tv_sec) * 1e6 + (after.tv_nsec - before.tv_nsec) * 1e-3));
			if (stepTimes.back() > slowest)
			{
				slowest = stepTimes.back();
				slowestFrame = netplay::frame();
			}
		}

		if (answer != netplay::Step::Idle && !pausing && netplay::resyncs() == seenResyncs)
		{
			// The first input given while this frame is the one to run, and
			// no menu is open, is the one for the frame `delay` later.
			const uint64_t forFrame = netplay::frame() + (uint64_t)options.delay;
			if (remoteBefore || netplay::remotePaused())
			{
				if (given.find(forFrame) == given.end())
					unsure.insert(forFrame);
			}
			else if (given.find(forFrame) == given.end() && unsure.find(forFrame) == unsure.end())
				given[forFrame] = offered;
		}
		if (answer == netplay::Step::Run)
		{
			const uint64_t frame = netplay::frame();
			if (pausing)
				violation("frame %" PRIu64 " ran while this side's menu was open", frame);
			if (remoteBefore && netplay::remotePaused())
				violation("frame %" PRIu64 " ran while the other side's menu was open", frame);
			if (netplay::resyncs() == seenResyncs)
			{
				if (frame < (uint64_t)options.delay)
				{
					if (!neutral(inputs[0]) || !neutral(inputs[1]))
						violation("frame %" PRIu64 " is before the delay and has an input", frame);
				}
				else
				{
					if (neutral(inputs[0]) || neutral(inputs[1]))
						violation("frame %" PRIu64 " has nobody's input", frame);
					const auto it = given.find(frame);
					if (unsure.erase(frame) == 0 && (it == given.end() || !same(it->second, inputs[local])))
						violation("frame %" PRIu64 " did not get this side's input for it", frame);
					if (it != given.end())
						given.erase(it);
				}
				emulator.run(inputs);
				for (int player = 0; player < netplay::Players; player++)
					sums[player] = mix(sums[player] ^ inputBits(inputs[player]));
				netplay::ran();
				runs++;
				if (lastRun != 0)
					longestGap = std::max(longestGap, now() - lastRun);
				lastRun = now();
				const uint64_t reached = netplay::frame();
				if (netplay::resyncs() == seenResyncs && reached == frame + 1)
				{
					if (options.corruptAt >= 0 && !corrupted && reached == (uint64_t)options.corruptAt)
					{
						emulator.memory[MemoryBytes / 3] ^= 0x5a;
						corrupted = true;
					}
					if (reached == options.frames)
					{
						printf("RESULT role=%s session=%d frames=%" PRIu64 " checksum=%08x in0=%016" PRIx64 " in1=%016" PRIx64
								" resyncs=%u\n", options.role.c_str(), session, reached, emulator.checksum(), sums[0], sums[1],
								netplay::resyncs());
						fflush(stdout);
						resultPrinted = true;
					}
					// The other side is at most the delay and a frame behind:
					// it has passed --frames too when this one is `extra` past it.
					if (hosting && reached >= options.frames + options.extra)
						break;
				}
			}
			else
				netplay::ran();
		}
		else
		{
			waits++;
			if (options.refreshUs == 0)
				usleep(300);
		}
		if (options.refreshUs > 0)
			usleep((useconds_t)options.refreshUs);
		if (options.jitterMs > 0)
			sleepMs((double)(random.next() % ((uint64_t)options.jitterMs * 1000 + 1)) / 1000.0);
	}

	const netplay::State endState = netplay::state();
	const std::string endError = netplay::error();
	const unsigned endResyncs = netplay::resyncs();
	const int ping = netplay::ping();
	const double stopStarted = now();
	netplay::stop();
	const double stopTook = now() - stopStarted;

	std::sort(stepTimes.begin(), stepTimes.end());
	const auto part = [&](double share) {
		return stepTimes.empty() ? 0.f : stepTimes[std::min(stepTimes.size() - 1, (size_t)(share * (double)stepTimes.size()))];
	};
	double total = 0;
	for (const float took : stepTimes)
		total += took;
	printf("FINAL role=%s session=%d state=%s error=\"%s\" resyncs=%u saves=%u loads=%u steps=%" PRIu64 " runs=%" PRIu64
			" waits=%" PRIu64 " ping=%d remotepause=%d silence=%d gapms=%.0f elapsed=%.2f stopms=%.1f timeout=%d\n",
			options.role.c_str(), session, stateName(endState), endError.c_str(), endResyncs, saves, loads, steps, runs, waits, ping,
			sawRemotePause ? 1 : 0, sawSilence ? 1 : 0, longestGap * 1000.0, now() - started, stopTook * 1000.0, timedOut ? 1 : 0);
	printf("STEP role=%s session=%d calls=%zu meanus=%.2f p50us=%.2f p99us=%.2f p999us=%.2f maxus=%.2f maxframe=%" PRIu64
			" nothingmaxus=%.2f\n",
			options.role.c_str(), session, stepTimes.size(), stepTimes.empty() ? 0.0 : total / (double)stepTimes.size(), part(0.5),
			part(0.99), part(0.999), slowest, slowestFrame, slowestNothing);
	fflush(stdout);
	if (netplay::active())
		violation("still active after stop()");
	if (violations != 0)
		return 1;
	if (timedOut)
		return 3;
	return resultPrinted ? 0 : 2;
}

// ---------------------------------------------------------------- a stranger

void rawPut32(std::vector<uint8_t>& to, uint32_t value)
{
	for (int shift = 0; shift < 32; shift += 8)
		to.push_back((uint8_t)(value >> shift));
}

void rawText(std::vector<uint8_t>& to, const std::string& text)
{
	to.push_back((uint8_t)text.size());
	to.push_back((uint8_t)(text.size() >> 8));
	to.insert(to.end(), text.begin(), text.end());
}

void rawMessage(std::vector<uint8_t>& to, uint8_t type, const std::vector<uint8_t>& payload)
{
	to.push_back('S');
	to.push_back('N');
	to.push_back(type);
	to.push_back(0);
	rawPut32(to, (uint32_t)payload.size());
	to.insert(to.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> rawHello(const Options& options, uint32_t version, const char *build)
{
	std::vector<uint8_t> payload;
	rawPut32(payload, version);
	rawText(payload, build);
	rawText(payload, options.game);
	rawText(payload, options.bios);
	payload.push_back(0);		// a guest
	payload.push_back(2);
	rawPut32(payload, 0);
	return payload;
}

int runRaw(const Options& options)
{
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)options.port);
	inet_pton(AF_INET, options.address.c_str(), &address.sin_addr);
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
	{
		printf("RAW connect failed: %s\n", strerror(errno));
		return 2;
	}
	Random random(options.seed);
	std::vector<uint8_t> out;
	if (options.send == "garbage")
	{
		const char *text = "GET / HTTP/1.1\r\nHost: console\r\n\r\n";
		out.assign(text, text + strlen(text));
	}
	else if (options.send == "version")
		rawMessage(out, 1, rawHello(options, 99, "42"));
	else if (options.send == "short")
	{
		// A header that promises more than comes.
		rawMessage(out, 1, std::vector<uint8_t>(100, 0));
		out.resize(40);
	}
	else if (options.send == "noise")
	{
		// A proper HELLO, then messages of every type with anything in them.
		rawMessage(out, 1, rawHello(options, 1, "9"));
		for (int i = 0; i < options.count; i++)
		{
			std::vector<uint8_t> payload(random.next() % 40);
			for (uint8_t& byte : payload)
				byte = (uint8_t)random.next();
			// Often the epoch a session starts with, so that some get further.
			if (payload.size() >= 4 && random.next() % 2 == 0)
			{
				payload[0] = 1;
				payload[1] = payload[2] = payload[3] = 0;
			}
			rawMessage(out, (uint8_t)(2 + random.next() % 11), payload);
		}
	}
	size_t sent = 0;
	while (sent < out.size())
	{
		const ssize_t done = send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
		if (done <= 0)
			break;
		sent += (size_t)done;
	}
	if (options.send == "short")
		shutdown(fd, SHUT_WR);
	// What comes back, until the other side closes or five seconds pass.
	size_t received = 0;
	const double until = now() + 5;
	bool closed = false;
	while (!closed && now() < until)
	{
		pollfd ready{};
		ready.fd = fd;
		ready.events = POLLIN;
		if (poll(&ready, 1, 100) <= 0)
			continue;
		uint8_t block[4096];
		const ssize_t got = recv(fd, block, sizeof(block), 0);
		if (got <= 0)
			closed = true;
		else
			received += (size_t)got;
	}
	close(fd);
	printf("RAW send=%s sent=%zu received=%zu closed=%d\n", options.send.c_str(), sent, received, closed ? 1 : 0);
	return 0;
}

} // namespace

int main(int argc, char **argv)
{
	Options options;
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s host|join|raw [--port N] [--addr A] [--game S] [--bios S] [--delay N] [--frames N] ...\n", argv[0]);
		return 64;
	}
	options.role = argv[1];
	for (int i = 2; i < argc; i++)
	{
		const std::string name = argv[i];
		const char *value = i + 1 < argc ? argv[i + 1] : "";
		i++;
		if (name == "--port") options.port = atoi(value);
		else if (name == "--addr") options.address = value;
		else if (name == "--game") options.game = value;
		else if (name == "--bios") options.bios = value;
		else if (name == "--delay") options.delay = atoi(value);
		else if (name == "--frames") options.frames = strtoull(value, nullptr, 10);
		else if (name == "--extra") options.extra = strtoull(value, nullptr, 10);
		else if (name == "--jitter") options.jitterMs = atoi(value);
		else if (name == "--refresh-us") options.refreshUs = atoi(value);
		else if (name == "--corrupt") options.corruptAt = atoll(value);
		else if (name == "--pause-at") options.pauseAt = atoll(value);
		else if (name == "--pause-ms") options.pauseMs = atoi(value);
		else if (name == "--retry") options.retry = atof(value);
		else if (name == "--timeout") options.timeout = atof(value);
		else if (name == "--sessions") options.sessions = atoi(value);
		else if (name == "--salt") options.salt = strtoull(value, nullptr, 10);
		else if (name == "--seed") options.seed = strtoull(value, nullptr, 10);
		else if (name == "--send") options.send = value;
		else if (name == "--count") options.count = atoi(value);
		else
		{
			fprintf(stderr, "unknown option %s\n", name.c_str());
			return 64;
		}
	}
	if (options.role == "raw")
		return runRaw(options);
	if (options.role != "host" && options.role != "join")
	{
		fprintf(stderr, "unknown role %s\n", options.role.c_str());
		return 64;
	}

	// A thread made and ended first: what a sanitizer starts with the first
	// thread of a process is then part of the count.
	std::thread([] {}).join();
	const int threadsBefore = threadCount(), filesBefore = fileCount();
	int worst = 0;
	for (int session = 1; session <= options.sessions; session++)
	{
		if (options.role == "host" && options.sessions > 1)
		{
			// Hosting, stopped before anyone came, then hosted again on the
			// same port: the session below.
			netplay::Session nobody;
			nobody.game = options.game;
			nobody.bios = options.bios;
			nobody.port = options.port;
			netplay::Hooks hooks;
			hooks.saveState = [](std::vector<uint8_t>&) { return false; };
			hooks.loadState = [](const uint8_t *, size_t) { return false; };
			hooks.checksum = [] { return 0u; };
			if (!netplay::host(nobody, hooks) || netplay::state() != netplay::State::Listening)
				violation("hosting again failed: %s", netplay::error().c_str());
			sleepMs(session * 20);
			netplay::stop();
			if (netplay::state() != netplay::State::Ended || netplay::active())
				violation("not Ended after stop()");
		}
		const int code = runSession(options, session);
		worst = std::max(worst, code);
		// A helper thread whose connect was given up ends a moment later.
		for (int i = 0; i < 50 && threadCount() != threadsBefore; i++)
			sleepMs(10);
		const int threads = threadCount(), files = fileCount();
		printf("AFTER session=%d threads=%d (before %d) files=%d (before %d)\n", session, threads, threadsBefore, files, filesBefore);
		if (threads != threadsBefore)
			violation("%d threads after stop(), %d before the session", threads, threadsBefore);
		// The pipe that wakes the network thread is made once and kept (by a
		// session that got as far as having a thread).
		if (files != filesBefore + 2 && files != filesBefore)
			violation("%d open files after stop(), %d before the session", files, filesBefore);
	}
	fflush(stdout);
	return violations != 0 ? 1 : worst;
}
