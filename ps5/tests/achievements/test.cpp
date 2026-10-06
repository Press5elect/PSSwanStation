/*
	PSSwanStation - the achievements module against a stand-in for
	RetroAchievements' server.

	SPDX-License-Identifier: GPL-3.0-or-later

	run.sh builds this with rcheevos' sources and starts mock_server.py; the
	module is pointed at it with SWANSTATION_RA_HOST. The hooks here are a
	2 MiB memory (and the scratchpad) the test pokes, a disc hook over a test
	image from ps5/tools/make-test-disc.py, and HTTP GET through curl. A
	request to any other server ends the test: nothing may reach
	retroachievements.org.

	  test WORK_DIR		(disc.cue, other.cue and disc2.cue are in it)

	TEST_VERBOSE=1 prints the events and the module's log lines;
	TEST_SLOW=1 adds the minute it takes to see a game's load tried again and
	a ping.
*/
#include "achievements.h"
#include "fe.h"
#include "stubs.h"

#include "rc_consoles.h"
#include "rc_hash.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace ach = fe::achievements;
using ach::Event;

namespace
{
// The mock server's user (mock_server.py).
const char *const User = "tester";
const char *const Password = "s3cret p&ss/1";
const char *const Token = "tok3nABC123xyz";

std::string host;		// http://127.0.0.1:PORT
std::string workDir;
std::string discHash;	// of disc.cue
std::vector<Event> seen;
size_t cursor;			// events before it were looked at by expect()

// ------------------------------------------------------------------ checks
const char *kindName(int kind)
{
	static const char *const names[] = { "LoggedIn", "LoginFailed", "GameLoaded", "GameUnknown", "Unlocked",
		"Progress", "Challenge", "LeaderboardStarted", "LeaderboardFailed", "LeaderboardSubmitted", "Completed",
		"ServerError", "Disconnected", "Reconnected" };
	return kind >= 0 && kind < (int)(sizeof(names) / sizeof(names[0])) ? names[kind] : "?";
}

[[noreturn]] void fail(const char *file, int line, const std::string& what)
{
	fprintf(stderr, "FAILED %s:%d: %s\n", file, line, what.c_str());
	for (size_t i = seen.size() > 12 ? seen.size() - 12 : 0; i < seen.size(); i++)
		fprintf(stderr, "    event %s \"%s\" \"%s\"\n", kindName(seen[i].kind), seen[i].title.c_str(), seen[i].text.c_str());
	const std::vector<std::string> log = testLog();
	for (size_t i = log.size() > 25 ? log.size() - 25 : 0; i < log.size(); i++)
		fprintf(stderr, "    log %s\n", log[i].c_str());
	fflush(stderr);
	_exit(1);
}

std::string show(const std::string& value)
{
	return "\"" + value + "\"";
}

template <typename Number>
std::string show(Number value)
{
	return std::to_string(value);
}

#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
			fail(__FILE__, __LINE__, #condition); \
	} while (0)

#define CHECK_EQ(left, right) \
	do \
	{ \
		const auto leftValue = (left); \
		const decltype(leftValue) rightValue = (right); \
		if (!(leftValue == rightValue)) \
			fail(__FILE__, __LINE__, std::string(#left " is ") + show(leftValue) + ", not " + show(rightValue)); \
	} while (0)

void section(const char *name)
{
	printf("  %s\n", name);
	fflush(stdout);
}

bool contains(const std::string& text, const char *part)
{
	return text.find(part) != std::string::npos;
}

bool endsWith(const std::string& text, const std::string& end)
{
	return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

void sleepMs(int milliseconds)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// ------------------------------------------------------------------- hooks
// The emulated memory. The test's second thread reads it through summary()
// (the rich presence), so it has a lock of its own.
std::mutex memoryMutex;
uint8_t ram[0x200000];
uint8_t scratchpad[0x400];

void poke(uint32_t address, uint8_t value)
{
	std::lock_guard<std::mutex> lock(memoryMutex);
	if (address < sizeof(ram))
		ram[address] = value;
	else
		scratchpad[address - sizeof(ram)] = value;
}

void clearMemory()
{
	std::lock_guard<std::mutex> lock(memoryMutex);
	memset(ram, 0, sizeof(ram));
	memset(scratchpad, 0, sizeof(scratchpad));
}

uint32_t readMemory(uint32_t address, uint8_t *buffer, uint32_t bytes)
{
	std::lock_guard<std::mutex> lock(memoryMutex);
	uint32_t done = 0;
	for (; done < bytes; done++, address++)
	{
		if (address < sizeof(ram))
			buffer[done] = ram[address];
		else if (address < sizeof(ram) + sizeof(scratchpad))
			buffer[done] = scratchpad[address - sizeof(ram)];
		else
			break;
	}
	return done;
}

// The disc: a raw MODE2/2352 image, 24 bytes before each sector's user data.
std::atomic<int> discsOpen{0};
std::atomic<int> sectorsRead{0};
std::atomic<int> discDelay{0};	// milliseconds an open takes (a slow share)

void *openDisc(const std::string& path)
{
	if (discDelay > 0)
		sleepMs(discDelay);
	std::string image = path;
	if (endsWith(image, ".cue"))
		image.replace(image.size() - 4, 4, ".bin");
	FILE *file = fopen(image.c_str(), "rb");
	if (file != nullptr)
		discsOpen++;
	return file;
}

bool readSector(void *disc, uint32_t sector, uint8_t *out)
{
	FILE *file = static_cast<FILE *>(disc);
	sectorsRead++;
	return fseek(file, (long)sector * 2352 + 24, SEEK_SET) == 0 && fread(out, 1, 2048, file) == 2048;
}

void closeDisc(void *disc)
{
	fclose(static_cast<FILE *>(disc));
	discsOpen--;
}

// HTTP GET through curl. The status, or -1 when there was no answer.
std::atomic<int> requestNumber{0};
std::atomic<bool> dropErrorBodies{false};	// as the console's client does

int curl(const std::string& url, std::vector<uint8_t>& out, unsigned seconds)
{
	out.clear();
	const std::string file = workDir + "answer-" + std::to_string(requestNumber++) + ".tmp";
	const std::string command = "curl -s --noproxy '*' -m " + std::to_string(seconds) + " -o '" + file
			+ "' -w '%{http_code}' '" + url + "' 2>/dev/null";
	FILE *pipe = popen(command.c_str(), "r");
	if (pipe == nullptr)
		return -1;
	char status[32] = "";
	const bool read = fgets(status, sizeof(status), pipe) != nullptr;
	const int ended = pclose(pipe);
	if (!read || ended != 0 || atoi(status) <= 0)
	{
		unlink(file.c_str());
		return -1;
	}
	fe::readFile(file, out);
	unlink(file.c_str());
	return atoi(status);
}

int httpGet(const std::string& url, std::vector<uint8_t>& out, unsigned seconds)
{
	if (url.compare(0, host.size() + 1, host + "/") != 0 || contains(url, "'"))
	{
		fprintf(stderr, "FAILED: a request that is not for the mock server: %.40s\n", url.c_str());
		_exit(1);
	}
	// Three seconds are plenty for a server on this machine.
	const int status = curl(url, out, std::min(seconds, 3u));
	if (dropErrorBodies && (status < 200 || status >= 300))
		out.clear();
	return status;
}

std::string control(const std::string& query)
{
	std::vector<uint8_t> answer;
	if (curl(host + "/control?" + query, answer, 5) != 200)
		fail(__FILE__, __LINE__, "the mock server does not answer /control?" + query);
	return std::string(answer.begin(), answer.end());
}

std::string get(const std::string& key)
{
	return control("op=get&key=" + key);
}

std::atomic<int> resets{0};

ach::Hooks hooks()
{
	ach::Hooks h;
	h.readMemory = readMemory;
	h.openDisc = openDisc;
	h.readSector = readSector;
	h.closeDisc = closeDisc;
	h.httpGet = httpGet;
	// As the host does: reset the machine, then say so.
	h.reset = [] {
		resets++;
		clearMemory();
		ach::gameReset();
	};
	return h;
}

// ------------------------------------------------------------------ frames
// One frame of the emulator (or of a menu), then the events it brought.
void step(bool running = true)
{
	if (running)
		ach::frame();
	else
		ach::idle();
	for (Event& event : ach::takeEvents())
	{
		if (getenv("TEST_VERBOSE") != nullptr)
			fprintf(stderr, "    [event] %s \"%s\" \"%s\" %d %s\n", kindName(event.kind), event.title.c_str(),
					event.text.c_str(), event.points, event.badge.c_str());
		seen.push_back(std::move(event));
	}
}

void frames(int count)
{
	for (int i = 0; i < count; i++)
		step();
}

// Runs frames until an event of the kind comes, and gives it.
Event expect(Event::Kind kind, double seconds = 10, bool running = true)
{
	const double until = fe::now() + seconds;
	for (;;)
	{
		for (size_t i = cursor; i < seen.size(); i++)
		{
			if (seen[i].kind == kind)
			{
				cursor = i + 1;
				return seen[i];
			}
		}
		if (fe::now() > until)
			fail(__FILE__, __LINE__, std::string("no ") + kindName(kind) + " event came");
		step(running);
		sleepMs(2);
	}
}

// Runs frames until the condition holds.
template <typename Condition>
void waitFor(const char *what, Condition condition, double seconds = 10)
{
	const double until = fe::now() + seconds;
	while (!condition())
	{
		if (fe::now() > until)
			fail(__FILE__, __LINE__, std::string("waited in vain for: ") + what);
		step();
		sleepMs(5);
	}
}

int count(Event::Kind kind, size_t from = 0)
{
	int found = 0;
	for (size_t i = from; i < seen.size(); i++)
		found += seen[i].kind == kind;
	return found;
}

ach::Achievement achievement(uint32_t id)
{
	for (const ach::Achievement& entry : ach::list())
		if (entry.id == id)
			return entry;
	fail(__FILE__, __LINE__, "achievement " + std::to_string(id) + " is not in the list");
}

std::string today()
{
	const time_t now = time(nullptr);
	tm local{};
	localtime_r(&now, &local);
	char date[16];
	strftime(date, sizeof(date), "%Y-%m-%d", &local);
	return date;
}

std::string fileHash(const std::string& cue)
{
	char hash[33] = "";
	CHECK(rc_hash_generate_from_file(hash, RC_CONSOLE_PLAYSTATION, cue.c_str()) != 0);
	return hash;
}

// --------------------------------------------------------------- the tests
std::string disc, otherDisc, secondDisc, cacheDir;
std::string token;
std::vector<uint8_t> savedProgress;

void testHash()
{
	section("the disc's hash");
	// rcheevos' own reading of the image file against the hash's definition
	// worked out in Python (run.sh); the module's own comes with the game.
	discHash = fileHash(disc);
	printf("    rcheevos, from the file:   %s\n", discHash.c_str());
	if (const char *independent = getenv("TEST_DISC_HASH"))
	{
		printf("    from the definition:       %s\n", independent);
		CHECK_EQ(discHash, std::string(independent));
	}
	CHECK(fileHash(otherDisc) != discHash);
	control("op=known&hash=" + discHash);
	control("op=known&hash=" + fileHash(secondDisc));
}

void firstRun()
{
	section("before init: every call is harmless");
	ach::frame();
	ach::idle();
	ach::gameStarted(disc);
	ach::gameReset();
	ach::loginWithToken(User, Token);
	ach::setHardcore(true);
	CHECK(!ach::hardcore());
	CHECK(ach::token().empty());
	CHECK(ach::list().empty());
	CHECK(ach::leaderboards().empty());
	CHECK(ach::takeEvents().empty());
	CHECK(ach::saveProgress().empty());
	CHECK(!ach::summary().loggedIn);
	ach::shutdown();
	CHECK_EQ(resets.load(), 0);

	ach::init(hooks(), cacheDir);
	CHECK(!ach::hardcore());
	CHECK(fe::dirExists(cacheDir + "badges"));

	section("a wrong password");
	ach::loginWithPassword(User, "wr0ng-pw");
	CHECK(ach::summary().loggingIn);
	Event event = expect(Event::LoginFailed, 10, false);
	CHECK(contains(event.text, "Invalid User/Password"));
	ach::Summary summary = ach::summary();
	CHECK(!summary.loggedIn);
	CHECK(!summary.loggingIn);
	CHECK(!summary.lastError.empty());
	CHECK(ach::token().empty());

	section("login with the password");
	ach::loginWithPassword(User, Password);
	event = expect(Event::LoggedIn, 10, false);
	CHECK_EQ(event.title, "Tester");
	CHECK_EQ(event.text, "340 points");
	CHECK_EQ(event.points, 340);
	token = ach::token();
	CHECK_EQ(token, Token);
	summary = ach::summary();
	CHECK(summary.loggedIn);
	CHECK(!summary.loggingIn);
	CHECK_EQ(summary.user, "Tester");
	CHECK_EQ(summary.userPoints, 340);
	CHECK(summary.lastError.empty());
	CHECK(!summary.gameLoaded);
	CHECK(!summary.gameLoading);

	section("a disc the server does not know");
	ach::gameStarted(otherDisc);
	CHECK(ach::summary().gameLoading);
	event = expect(Event::GameUnknown);
	summary = ach::summary();
	CHECK(!summary.gameLoaded);
	CHECK(!summary.gameLoading);
	CHECK(ach::list().empty());
	ach::gameStopped();

	section("a game is loaded, and its slow disc holds nothing up");
	discDelay = 400;
	double before = fe::now();
	ach::gameStarted(disc);
	CHECK(fe::now() - before < 0.2);
	before = fe::now();
	frames(20);
	CHECK(fe::now() - before < 0.2);
	CHECK(ach::summary().gameLoading);
	event = expect(Event::GameLoaded);
	discDelay = 0;
	CHECK_EQ(event.title, "Swan Test Disc");
	CHECK_EQ(event.text, "0 of 3 achievements");
	CHECK_EQ(discsOpen.load(), 0);
	CHECK(sectorsRead > 4);
	// The module's hash of the disc, through the hooks: in its log.
	std::string moduleHash;
	for (const std::string& line : testLog())
	{
		const size_t at = line.find("disc hash ");
		if (at != std::string::npos && contains(line, "(disc.cue)"))
			moduleHash = line.substr(at + 10, 32);
	}
	printf("    the module, through hooks: %s\n", moduleHash.c_str());
	CHECK_EQ(moduleHash, discHash);
	summary = ach::summary();
	CHECK(summary.gameLoaded);
	CHECK(!summary.gameLoading);
	CHECK_EQ(summary.game, "Swan Test Disc");
	CHECK_EQ(summary.gameId, 4242u);
	CHECK_EQ(summary.unlocked, 0);
	CHECK_EQ(summary.total, 3);
	CHECK_EQ(summary.points, 0);
	CHECK_EQ(summary.totalPoints, 40);
	CHECK(!summary.hardcore);
	std::vector<ach::Achievement> list = ach::list();
	CHECK_EQ(list.size(), (size_t)3);	// the unofficial one is not asked for
	for (const ach::Achievement& entry : list)
	{
		CHECK(!entry.unlocked);
		CHECK(!entry.unofficial);
		CHECK(entry.unlockedWhen.empty());
	}
	CHECK_EQ(achievement(1001).title, "First Step");
	CHECK_EQ(achievement(1001).description, "Set the first flag");
	CHECK_EQ(achievement(1002).points, 10);
	std::vector<ach::Leaderboard> boards = ach::leaderboards();
	CHECK_EQ(boards.size(), (size_t)1);
	CHECK_EQ(boards[0].id, 2001u);
	CHECK_EQ(boards[0].title, "Speed Run");
	CHECK(!boards[0].tracking);
	CHECK(boards[0].best.empty());

	section("badges: each fetched once, two at a time at most");
	waitFor("the six badge pictures", [] {
		for (const ach::Achievement& entry : ach::list())
			if (entry.badge.empty())
				return false;
		return get("badge_count") == "6";
	});
	for (const ach::Achievement& entry : ach::list())
	{
		CHECK(endsWith(entry.badge, "_lock.png"));
		CHECK(fe::fileExists(entry.badge));
	}
	for (const char *name : { "90001", "90002", "90003" })
		waitFor("an unlocked badge picture", [name] { return fe::fileExists(cacheDir + "badges/" + name + ".png"); });
	CHECK_EQ(get("badge_repeats"), "0");
	CHECK(atoi(get("badge_most").c_str()) <= 2);
	std::vector<uint8_t> picture;
	CHECK(fe::readFile(cacheDir + "badges/90001.png", picture));
	CHECK(picture.size() > 8 && memcmp(picture.data(), "\x89PNG", 4) == 0);

	// From here on a second thread asks what an interface would, all the time.
	std::atomic<bool> stop{false};
	std::thread interface([&stop] {
		while (!stop)
		{
			ach::summary();
			ach::list();
			ach::leaderboards();
			ach::hardcore();
			ach::token();
			sleepMs(1);
		}
	});

	section("an unlock");
	poke(0x10, 1);
	event = expect(Event::Unlocked);
	CHECK_EQ(event.title, "First Step");
	CHECK_EQ(event.text, "Set the first flag");
	CHECK_EQ(event.points, 5);
	CHECK_EQ(event.badge, cacheDir + "badges/90001.png");
	waitFor("the unlock at the server", [] { return get("award:1001:0") == "1"; });
	ach::Achievement first = achievement(1001);
	CHECK(first.unlocked);
	CHECK_EQ(first.badge, cacheDir + "badges/90001.png");
	CHECK_EQ(first.unlockedWhen, today());
	waitFor("the new score", [] { return ach::summary().userPoints == 345; });
	summary = ach::summary();
	CHECK_EQ(summary.unlocked, 1);
	CHECK_EQ(summary.points, 5);

	section("measured progress");
	poke(0x11, 3);
	event = expect(Event::Progress);
	CHECK_EQ(event.title, "Collector");
	CHECK_EQ(event.text, "3/5");
	ach::Achievement collector = achievement(1002);
	CHECK_EQ(collector.progress, "3/5");
	CHECK(collector.progressPercent > 59.f && collector.progressPercent < 61.f);
	poke(0x11, 5);
	event = expect(Event::Unlocked);
	CHECK_EQ(event.title, "Collector");
	CHECK_EQ(event.points, 10);
	waitFor("the unlock at the server", [] { return get("award:1002:0") == "1"; });

	section("rich presence");
	poke(0x30, 7);
	waitFor("the rich presence", [] { return ach::summary().richPresence == "Level 7"; });

	section("progress saved and loaded");
	// "Triple" counts the frames its flag is set, in the scratchpad.
	const size_t counting = seen.size();
	poke(0x200010, 1);
	frames(4);
	poke(0x200010, 0);
	frames(1);
	CHECK_EQ(achievement(1003).progress, "4/10");
	// A count that moves every frame is not a notice every frame.
	CHECK_EQ(count(Event::Progress, counting), 1);
	CHECK_EQ(seen.back().text, "1/10");
	savedProgress = ach::saveProgress();
	CHECK(!savedProgress.empty());
	poke(0x200010, 1);
	frames(3);
	poke(0x200010, 0);
	frames(1);
	CHECK_EQ(achievement(1003).progress, "7/10");
	ach::loadProgress(savedProgress.data(), savedProgress.size());
	CHECK_EQ(achievement(1003).progress, "4/10");
	CHECK(!achievement(1003).unlocked);
	// A state without progress, and one whose progress is damaged, start afresh.
	ach::loadProgress(nullptr, 0);
	CHECK_EQ(achievement(1003).progress, "");
	ach::loadProgress(savedProgress.data(), savedProgress.size());
	CHECK_EQ(achievement(1003).progress, "4/10");
	std::vector<uint8_t> damaged = savedProgress;
	damaged[damaged.size() / 2] ^= 0x5a;
	ach::loadProgress(damaged.data(), damaged.size());
	CHECK_EQ(achievement(1003).progress, "");
	CHECK_EQ(ach::saveProgress().size(), savedProgress.size());

	section("hardcore mode on: the reset hook is called");
	CHECK_EQ(resets.load(), 0);
	ach::setHardcore(true);
	CHECK_EQ(resets.load(), 1);
	CHECK(ach::hardcore());
	frames(2);
	summary = ach::summary();
	CHECK(summary.hardcore);
	CHECK_EQ(summary.unlocked, 0);		// nothing earned in hardcore mode yet
	CHECK_EQ(summary.userPoints, 1200);
	CHECK(!achievement(1001).unlocked);
	ach::setHardcore(true);
	CHECK_EQ(resets.load(), 1);

	section("leaderboard: started, submitted, cancelled");
	poke(0x21, 42);
	poke(0x20, 1);
	event = expect(Event::LeaderboardStarted);
	CHECK_EQ(event.title, "Speed Run");
	boards = ach::leaderboards();
	CHECK(boards[0].tracking);
	CHECK_EQ(boards[0].value, "42");
	poke(0x21, 57);
	frames(1);
	CHECK_EQ(ach::leaderboards()[0].value, "57");
	poke(0x20, 3);
	event = expect(Event::LeaderboardSubmitted);
	CHECK_EQ(event.title, "Speed Run");
	CHECK_EQ(event.text, "57, rank 2 of 7");
	CHECK_EQ(get("entries"), "57");
	boards = ach::leaderboards();
	CHECK(!boards[0].tracking);
	CHECK(boards[0].value.empty());
	CHECK_EQ(boards[0].best, "57");
	poke(0x20, 0);
	frames(2);
	poke(0x20, 1);
	expect(Event::LeaderboardStarted);
	poke(0x20, 2);
	expect(Event::LeaderboardFailed);
	frames(3);
	CHECK_EQ(count(Event::LeaderboardSubmitted), 1);
	CHECK_EQ(get("entries"), "57");

	section("the server is away at an unlock: told at once, sent when it is back");
	control("op=down&value=1");
	poke(0x10, 1);
	event = expect(Event::Unlocked);
	CHECK_EQ(event.title, "First Step");
	expect(Event::Disconnected, 15);
	CHECK_EQ(get("award:1001:1"), "0");
	CHECK(achievement(1001).unlocked);
	control("op=down&value=0");
	expect(Event::Reconnected, 20);
	CHECK_EQ(get("award:1001:1"), "1");
	CHECK_EQ(get("late:1001"), "1");	// the call said how long ago it was earned
	CHECK_EQ(count(Event::ServerError), 0);
	CHECK_EQ(get("refused"), "0");

	section("logout");
	stop = true;
	interface.join();
	ach::logout();
	summary = ach::summary();
	CHECK(!summary.loggedIn);
	CHECK(!summary.gameLoaded);
	CHECK(ach::token().empty());
	CHECK(ach::list().empty());
	// The game is still running: logging in again loads it again.
	ach::loginWithToken(User, token);
	expect(Event::LoggedIn);
	event = expect(Event::GameLoaded);
	CHECK_EQ(event.text, "1 of 3 achievements");	// hardcore mode is still on
	ach::shutdown();
	CHECK_EQ(discsOpen.load(), 0);

	section("the log holds neither the password nor the token");
	const std::vector<std::string> log = testLog();
	CHECK(log.size() > 20);
	for (const std::string& line : log)
	{
		CHECK(!contains(line, "s3cret"));
		CHECK(!contains(line, "wr0ng"));
		CHECK(!contains(line, "tok3n"));
		CHECK(!contains(line, "dorequest"));
	}
}

void secondRun()
{
	section("second run: login with the token; a login replaces the one before it");
	clearMemory();
	ach::init(hooks(), cacheDir);
	CHECK(!ach::hardcore());	// off again with every start
	ach::setUnofficial(true);
	const size_t from = seen.size();
	ach::loginWithPassword(User, "wr0ng-pw");
	ach::loginWithToken(User, token);
	Event event = expect(Event::LoggedIn, 10, false);
	CHECK_EQ(event.text, "355 points");		// 340 and the two unlocks of the first run
	CHECK_EQ(count(Event::LoginFailed, from), 0);
	CHECK_EQ(ach::token(), token);

	section("progress loaded before the achievements are there");
	ach::gameStarted(disc);
	ach::loadProgress(savedProgress.data(), savedProgress.size());
	CHECK_EQ(ach::saveProgress().size(), savedProgress.size());
	event = expect(Event::GameLoaded);
	CHECK_EQ(event.text, "2 of 3 achievements");	// what the server remembers
	ach::Summary summary = ach::summary();
	CHECK_EQ(summary.unlocked, 2);
	CHECK_EQ(summary.total, 3);
	CHECK_EQ(summary.points, 15);
	CHECK_EQ(summary.totalPoints, 40);
	std::vector<ach::Achievement> list = ach::list();
	CHECK_EQ(list.size(), (size_t)4);
	CHECK_EQ(list[0].id, 1003u);		// what is left to earn comes first
	CHECK_EQ(list[0].progress, "4/10");
	CHECK(list[1].unlocked && list[2].unlocked);
	CHECK_EQ(list[1].unlockedWhen, today());
	CHECK_EQ(list[3].id, 1004u);		// the unofficial one last
	CHECK(list[3].unofficial);
	CHECK(!list[3].unlocked);

	section("an unofficial achievement: its challenge, then earned without the server");
	const std::string awards = get("requests:awardachievement");
	frames(2);
	poke(0x14, 1);
	event = expect(Event::Challenge);
	CHECK_EQ(event.title, "Draft");
	poke(0x15, 1);
	event = expect(Event::Unlocked);
	CHECK_EQ(event.title, "Draft");
	for (int i = 0; i < 50; i++)
	{
		step();
		sleepMs(5);
	}
	CHECK_EQ(get("requests:awardachievement"), awards);
	CHECK(achievement(1004).unlocked);
	CHECK_EQ(ach::summary().unlocked, 2);
	CHECK_EQ(ach::list().back().id, 1004u);		// earned, and still after the official ones

	section("the last achievement: the count went on from the saved four");
	poke(0x200010, 1);
	frames(5);
	CHECK_EQ(achievement(1003).progress, "9/10");
	CHECK(!achievement(1003).unlocked);
	event = expect(Event::Unlocked);
	CHECK_EQ(event.title, "Triple");
	CHECK_EQ(event.points, 25);
	event = expect(Event::Completed);
	CHECK_EQ(event.title, "Swan Test Disc");
	CHECK(contains(event.text, "Completed"));
	CHECK_EQ(get("award:1003:0"), "1");
	CHECK_EQ(ach::summary().unlocked, 3);

	section("another disc of the game is checked with the server");
	ach::discChanged(secondDisc);
	const std::string secondHash = fileHash(secondDisc);
	waitFor("the disc's hash at the server", [&secondHash] { return contains(get("looked_up"), secondHash.c_str()); });
	frames(3);
	CHECK(ach::summary().gameLoaded);
	CHECK_EQ(count(Event::GameUnknown, from), 0);

	section("an unknown disc ends hardcore mode");
	const int before = resets;
	ach::setHardcore(true);
	CHECK_EQ(resets.load(), before + 1);
	ach::discChanged(otherDisc);
	event = expect(Event::GameUnknown);
	CHECK(contains(event.text, "Hardcore"));
	CHECK(!ach::hardcore());
	CHECK(ach::summary().gameLoaded);

	ach::gameStopped();
	CHECK(!ach::summary().gameLoaded);
	CHECK(ach::list().empty());
	CHECK_EQ(get("refused"), "0");
	ach::shutdown();
	CHECK_EQ(discsOpen.load(), 0);
}

void bareTransport()
{
	section("a client that gives no body with an error status (the console's)");
	dropErrorBodies = true;
	ach::init(hooks(), cacheDir);
	ach::loginWithPassword(User, "wr0ng-pw");
	Event event = expect(Event::LoginFailed, 10, false);
	CHECK(contains(event.text, "not accepted"));
	ach::loginWithToken(User, token);
	expect(Event::LoggedIn, 10, false);
	ach::gameStarted(otherDisc);
	expect(Event::GameUnknown);
	ach::gameStopped();
	ach::shutdown();
	dropErrorBodies = false;
}

void shutdownInFlight()
{
	section("shutdown while a login and a game load are on their way");
	control("op=slow&value=1");
	ach::init(hooks(), cacheDir);
	ach::loginWithToken(User, token);
	ach::gameStarted(disc);
	sleepMs(100);
	step(false);
	double before = fe::now();
	ach::shutdown();
	CHECK(fe::now() - before < 4);
	CHECK(!ach::summary().loggedIn);

	section("shutdown while the disc is read");
	control("op=slow&value=0");
	ach::init(hooks(), cacheDir);
	ach::loginWithToken(User, token);
	expect(Event::LoggedIn, 10, false);
	discDelay = 500;
	ach::gameStarted(disc);
	sleepMs(50);
	before = fe::now();
	ach::shutdown();
	CHECK(fe::now() - before < 4);
	discDelay = 0;
	CHECK_EQ(discsOpen.load(), 0);

	section("shutdown while an unlock and badge downloads are on their way: the unlock arrives");
	const std::string freshCache = workDir + "cache2/";
	clearMemory();
	control("op=slow&value=0.3");
	ach::init(hooks(), freshCache);
	ach::setHardcore(true);
	ach::loginWithToken(User, token);
	expect(Event::LoggedIn, 10, false);
	ach::gameStarted(disc);
	expect(Event::GameLoaded, 15);
	// Two unlocks in one frame: the first call is on its way when the title
	// ends, the second has not been sent yet.
	poke(0x200010, 1);
	frames(9);
	CHECK_EQ(achievement(1003).progress, "9/10");
	const size_t from = seen.size();
	poke(0x11, 5);
	step();
	CHECK_EQ(count(Event::Unlocked, from), 2);
	before = fe::now();
	ach::shutdown();
	CHECK(fe::now() - before < 6);
	control("op=slow&value=0");
	CHECK_EQ(get("award:1002:1"), "1");
	CHECK_EQ(get("award:1003:1"), "1");
	CHECK_EQ(get("refused"), "0");
}

// Earns the first two achievements and starts the count of the third.
void earnAllButOne(bool hardcore)
{
	control("op=forget");
	clearMemory();
	ach::setHardcore(hardcore);
	ach::gameStarted(disc);
	Event event = expect(Event::GameLoaded);
	CHECK_EQ(event.text, "0 of 3 achievements");
	frames(2);
	poke(0x10, 1);
	poke(0x11, 5);
	const std::string mode = hardcore ? ":1" : ":0";
	waitFor("two unlocks at the server",
			[&mode] { return get("award:1001" + mode) == "1" && get("award:1002" + mode) == "1"; });
	poke(0x200010, 1);
	frames(9);
}

// rc_client 12.5 reads the loaded game when the last unlock's answer comes,
// and when it tells the completion, without looking whether there is one.
void lastUnlockAndTheGameEnds()
{
	section("the last unlock's answer comes after the game ended");
	ach::init(hooks(), cacheDir);
	ach::loginWithToken(User, token);
	expect(Event::LoggedIn, 10, false);
	earnAllButOne(false);
	control("op=slow&value=0.5");
	size_t from = seen.size();
	step();
	CHECK_EQ(count(Event::Unlocked, from), 1);
	ach::gameStopped();
	for (int i = 0; i < 100; i++)
	{
		step(false);
		sleepMs(10);
	}
	control("op=slow&value=0");
	CHECK_EQ(get("award:1003:0"), "1");
	CHECK_EQ(count(Event::Completed, from), 0);		// the game it completed is no longer there

	section("the last unlock's answer comes in a menu, and the game ends there");
	earnAllButOne(true);
	from = seen.size();
	step();
	CHECK_EQ(count(Event::Unlocked, from), 1);
	Event event = expect(Event::Completed, 10, false);
	CHECK_EQ(event.title, "Swan Test Disc");
	CHECK(contains(event.text, "Mastered"));
	ach::gameStopped();
	step(false);
	CHECK_EQ(count(Event::Completed, from), 1);
	CHECK_EQ(get("refused"), "0");
	ach::shutdown();
}

// The parts that wait on rc_client's and the module's own timers.
void slowTests()
{
	section("the server is away when a game starts: said once, loaded when it is back");
	clearMemory();
	ach::init(hooks(), cacheDir);
	ach::loginWithToken(User, token);
	expect(Event::LoggedIn, 10, false);
	const size_t from = seen.size();
	control("op=down&value=1");
	ach::gameStarted(disc);
	Event event = expect(Event::ServerError);
	CHECK(contains(event.text, "could not be loaded"));
	CHECK(ach::summary().gameLoading);
	CHECK(!ach::summary().lastError.empty());
	control("op=down&value=0");
	event = expect(Event::GameLoaded, 20);
	CHECK_EQ(count(Event::ServerError, from), 1);
	CHECK(ach::summary().lastError.empty());

	section("a ping after half a minute, with the rich presence");
	poke(0x30, 9);
	const double until = fe::now() + 45;
	while (get("requests:ping") == "0")
	{
		CHECK(fe::now() < until);
		for (int i = 0; i < 60; i++)
		{
			step();
			sleepMs(16);
		}
	}
	CHECK_EQ(get("ping"), "Level 9");
	ach::shutdown();
}

} // namespace

int main(int argc, char **argv)
{
	const char *server = getenv("SWANSTATION_RA_HOST");
	if (argc < 2 || server == nullptr)
	{
		fprintf(stderr, "usage: SWANSTATION_RA_HOST=http://127.0.0.1:PORT test WORK_DIR\n");
		return 2;
	}
	host = server;
	CHECK(host.compare(0, 17, "http://127.0.0.1:") == 0);
	workDir = argv[1];
	if (workDir.back() != '/')
		workDir += '/';
	disc = workDir + "disc.cue";
	otherDisc = workDir + "other.cue";
	secondDisc = workDir + "disc2.cue";
	cacheDir = workDir + "cache/";

	testHash();
	firstRun();
	secondRun();
	bareTransport();
	shutdownInFlight();
	lastUnlockAndTheGameEnds();
	if (getenv("TEST_SLOW") != nullptr)
		slowTests();
	printf("  all passed\n");
	return 0;
}
