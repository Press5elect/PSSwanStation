/*
	PSSwanStation - RetroAchievements: rcheevos' rc_client and what it needs
	around it.

	SPDX-License-Identifier: GPL-3.0-or-later

	rc_client (rcheevos 12.5) keeps the user, the game's achievements and
	leaderboards and their state. This file gives it the emulated memory, the
	server and the disc, and turns what it reports into the events and lists
	of achievements.h.

	Threads. rc_client is used by one thread at a time: every public function
	takes `mutex` first, and rc_client's callbacks run inside those calls.
	What must not hold the emulator up is done by workers, started when first
	needed:
	  - one sends rc_client's server calls, one at a time and in the order
	    they were made. The console's client has HTTP GET only, so a call's
	    POST data goes into the URL's query, which dorequest.php reads as
	    well. The answer waits in a queue until frame(), idle() or
	    takeEvents() hands it to rc_client on the caller's thread;
	  - one reads the disc for the hash that identifies the game (an image on
	    a network share can take seconds);
	  - two fetch badge pictures into <cache>badges/ while no server call is
	    waiting.
	The workers share the queues of `State` under its `queueMutex` and never
	touch rc_client. `mutex` is taken before `queueMutex`, never after.

	Nothing here keeps the password or the token: the frontend asks token()
	after a login and stores it. Neither is written to the log, nor is a
	request's URL, which carries them.
*/
#include "achievements.h"
#include "fe.h"

#include "rc_api_request.h"
#include "rc_client.h"
#include "rc_consoles.h"
#include "rc_hash.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace fe::achievements
{
namespace
{
// Time-outs of a server call and of a badge download, in seconds.
constexpr unsigned CallSeconds = 20;
constexpr unsigned BadgeSeconds = 20;
constexpr int BadgeThreads = 2;
// Events nobody takes are dropped beyond this many, the oldest first.
constexpr size_t MostEvents = 128;
// A leaderboard score's notice waits this long for the rank the server answers.
constexpr double RankSeconds = 4.0;
// At shutdown, unlocks and scores still waiting to be sent are sent for this long.
constexpr double LastCallsSeconds = 5.0;
// How often frame() asks rc_client for the rich presence text.
constexpr double PresenceSeconds = 1.0;
// A count that moves every frame gives a progress event this often at most.
constexpr double ProgressSeconds = 1.0;
// A game whose achievements could not be fetched is tried again after these.
constexpr double LoadRetrySeconds[] = { 10.0, 30.0, 60.0 };
constexpr int LoadRetries = 3;

// ServerCall::status when there is no HTTP status.
constexpr int NoAnswer = -1;	// the request could not be made, or timed out
constexpr int NotSent = -2;		// the module was shut down before it was sent

// A server call of rc_client: the request, then its answer.
struct ServerCall
{
	std::string url;		// with the POST data as its query
	std::string name;		// the request's "r" parameter, for the log
	bool lastCall = false;	// an unlock or a score: still sent while shutting down
	bool unlock = false;	// awardachievement
	bool hardcore = false;	// ...in hardcore mode
	unsigned game = 0;		// State::gameNumber when the call was made
	rc_client_server_callback_t callback = nullptr;
	void *callbackData = nullptr;
	int status = NoAnswer;
	std::string body;
};

struct BadgeJob
{
	std::string url, file, key;
};

struct DiscJob
{
	std::string path;
	unsigned generation = 0;
	bool change = false;	// another disc of the loaded game
};

struct DiscResult
{
	std::string hash;		// empty: the disc could not be read, or is not a PlayStation disc
	unsigned generation = 0;
	bool change = false;
};

struct Login
{
	std::string user, secret;
	bool withToken = false;
};

// A score rc_client submitted, until its notice is shown.
struct Submitted
{
	uint32_t id = 0;
	std::string title, score;
	double until = 0;
};

struct State
{
	Hooks hooks;
	std::string badgeDir;
	rc_client_t *client = nullptr;
	// shutdown() is running: nothing new is started and no event is kept.
	bool stopping = false;

	// The user. A login asked for while another is on its way waits for it.
	bool loggingIn = false;
	std::optional<Login> nextLogin;

	// The running game: its disc, the disc's hash once the worker has it, and
	// how far identifying it is.
	std::string discPath, discHash;
	unsigned gameNumber = 0;	// counts the games started
	bool completedTold[2] = {};	// in softcore, in hardcore: told once a game
	unsigned discGeneration = 0;
	bool hashing = false;
	bool unknown = false;		// not in the database: not asked again
	int retries = 0;
	double retryAt = 0;
	// Progress of a save state loaded before the achievements were there.
	bool haveProgress = false;
	std::vector<uint8_t> progress;
	// What the game says the player is doing, asked once a second in frame():
	// working it out reads memory, which summary() must not do.
	std::string presence;
	double presenceAt = 0;

	std::deque<Event> events;
	std::string lastError;
	bool serverSilent = false;	// the last server call got no answer
	bool resetWanted = false;
	std::map<uint32_t, std::string> best;	// leaderboard: the user's best, as the server said
	std::vector<Submitted> submitted;
	// rc_client's progress indicator: what it shows now, and what of it went
	// out as an event, and when.
	Event progressNow{};
	std::string progressTold;
	double progressToldAt = 0;

	// Shared with the workers.
	std::mutex queueMutex;
	std::condition_variable callWake, badgeWake, discWake;
	std::deque<ServerCall> calls;		// to send, oldest first
	std::deque<ServerCall> answers;		// to hand to rc_client
	bool calling = false;				// a server call is on its way
	std::deque<BadgeJob> badgeJobs;
	// Badge files by key ("00234", "00234_lock"): asked for in this run, there,
	// and failed since the last game was loaded.
	std::set<std::string> badgesAsked, badgesReady, badgesFailed;
	int badgeFailures = 0;
	std::optional<DiscJob> discJob;
	std::deque<DiscResult> discResults;
	bool quit = false;
	double lastCallsUntil = 0;

	std::thread callThread, discThread, badgeThread[BadgeThreads];
};

std::mutex mutex;
State *state;	// from init() to shutdown()

// ------------------------------------------------------------------- events
void addEvent(State& s, Event::Kind kind, const std::string& title, const std::string& text, int points = 0,
		const std::string& badge = "")
{
	if (s.stopping)
		return;
	Event event;
	event.kind = kind;
	event.title = title;
	event.text = text;
	event.points = points;
	event.badge = badge;
	s.events.push_back(std::move(event));
	if (s.events.size() > MostEvents)
		s.events.pop_front();
}

// rc_client's strings may be null.
const char *safe(const char *maybe)
{
	return maybe != nullptr ? maybe : "";
}

// ------------------------------------------------------------------ workers
template <typename Function>
void startThread(std::thread& thread, Function function, State& s)
{
	if (!thread.joinable())
		thread = std::thread(function, &s);
}

void callWorker(State *s)
{
	for (;;)
	{
		ServerCall call;
		{
			std::unique_lock<std::mutex> lock(s->queueMutex);
			s->callWake.wait(lock, [s] { return s->quit || !s->calls.empty(); });
			auto next = s->calls.begin();
			if (s->quit)
			{
				// Shutting down: only unlocks and scores still go out, and
				// only for a short while. shutdown() answers the rest.
				next = std::find_if(s->calls.begin(), s->calls.end(), [](const ServerCall& c) { return c.lastCall; });
				if (next == s->calls.end() || now() >= s->lastCallsUntil)
					return;
			}
			call = std::move(*next);
			s->calls.erase(next);
			s->calling = true;
		}
		std::vector<uint8_t> data;
		call.status = s->hooks.httpGet ? s->hooks.httpGet(call.url, data, CallSeconds) : NoAnswer;
		if (call.status < 0)
			call.status = NoAnswer;
		else
			call.body.assign(data.begin(), data.end());
		bool idle;
		{
			std::lock_guard<std::mutex> lock(s->queueMutex);
			s->answers.push_back(std::move(call));
			s->calling = false;
			idle = s->calls.empty();
		}
		if (idle)
			s->badgeWake.notify_all();
	}
}

bool isPng(const std::vector<uint8_t>& data)
{
	static const uint8_t signature[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
	return data.size() > sizeof(signature) && memcmp(data.data(), signature, sizeof(signature)) == 0;
}

void badgeWorker(State *s)
{
	for (;;)
	{
		BadgeJob job;
		{
			// Server calls go first: the console's client makes one request at a time.
			std::unique_lock<std::mutex> lock(s->queueMutex);
			s->badgeWake.wait(lock, [s] {
				return s->quit || (!s->badgeJobs.empty() && s->calls.empty() && !s->calling);
			});
			if (s->quit)
				return;
			job = std::move(s->badgeJobs.front());
			s->badgeJobs.pop_front();
		}
		bool have = fileExists(job.file);
		bool asked = false;
		if (!have && s->hooks.httpGet)
		{
			std::vector<uint8_t> data;
			asked = true;
			have = s->hooks.httpGet(job.url, data, BadgeSeconds) == 200 && isPng(data)
					&& writeFile(job.file, data.data(), data.size());
		}
		std::lock_guard<std::mutex> lock(s->queueMutex);
		if (have)
		{
			s->badgesReady.insert(job.key);
			if (asked)
				s->badgeFailures = 0;
			continue;
		}
		s->badgesFailed.insert(job.key);
		if (++s->badgeFailures == 3)
		{
			// No network, or the pictures' server is away: the rest waits for
			// the next game to be loaded.
			for (const BadgeJob& left : s->badgeJobs)
				s->badgesFailed.insert(left.key);
			s->badgeJobs.clear();
			diag::mark("achievements: badge downloads stopped (no answer); tried again with the next game");
		}
	}
}

// ---------------------------------------------------------------- the hash
// rcheevos hashes a PlayStation disc from the first data track's SYSTEM.CNF
// and the executable it names (rhash/hash_disc.c, rc_hash_psx). It reads
// through these callbacks: a track, then sectors by absolute number, 2048
// bytes of user data each, of which it may ask for fewer (or, for other
// systems, for more than one sector's worth).
struct DiscAccess
{
	const Hooks *hooks;
	const std::string *path;
};

struct Track
{
	const Hooks *hooks;
	void *disc;
};

void *RC_CCONV openTrack(const char *, uint32_t track, const rc_hash_iterator_t *iterator)
{
	const DiscAccess *access = static_cast<const DiscAccess *>(iterator->userdata);
	// rc_hash_psx asks for track 1, which on a PlayStation disc is the first
	// (and only) data track; the hooks read no other.
	if (track != 1 && track != RC_HASH_CDTRACK_FIRST_DATA)
		return nullptr;
	void *disc = access->hooks->openDisc(*access->path);
	if (disc == nullptr)
		return nullptr;
	return new Track{ access->hooks, disc };
}

size_t RC_CCONV readTrack(void *handle, uint32_t sector, void *buffer, size_t bytes)
{
	const Track *track = static_cast<const Track *>(handle);
	uint8_t data[2048];
	size_t done = 0;
	while (done < bytes)
	{
		if (!track->hooks->readSector(track->disc, sector++, data))
			break;
		const size_t part = std::min(bytes - done, sizeof(data));
		memcpy(static_cast<uint8_t *>(buffer) + done, data, part);
		done += part;
	}
	return done;
}

void RC_CCONV closeTrack(void *handle)
{
	Track *track = static_cast<Track *>(handle);
	track->hooks->closeDisc(track->disc);
	delete track;
}

// The first data track starts the disc: its sectors count from 0.
uint32_t RC_CCONV firstTrackSector(void *)
{
	return 0;
}

// rcheevos must not open anything itself: the path may be one only the
// emulator can open.
void *RC_CCONV openNoFile(const char *)
{
	return nullptr;
}

void RC_CCONV hashError(const char *message, const rc_hash_iterator_t *)
{
	diag::mark("achievements: hash: %s", message);
}

std::string hashDisc(const Hooks& hooks, const std::string& path)
{
	if (!hooks.openDisc || !hooks.readSector || !hooks.closeDisc)
		return "";
	const DiscAccess access{ &hooks, &path };
	rc_hash_iterator_t iterator;
	// The name is not the disc's: rcheevos would read an .m3u itself. It
	// stands for "an image", and the hooks get the real path.
	rc_hash_initialize_iterator(&iterator, "disc.bin", nullptr, 0);
	iterator.userdata = const_cast<DiscAccess *>(&access);
	iterator.callbacks.error_message = hashError;
	iterator.callbacks.filereader.open = openNoFile;
	iterator.callbacks.cdreader.open_track = nullptr;
	iterator.callbacks.cdreader.open_track_iterator = openTrack;
	iterator.callbacks.cdreader.read_sector = readTrack;
	iterator.callbacks.cdreader.close_track = closeTrack;
	iterator.callbacks.cdreader.first_track_sector = firstTrackSector;
	char hash[33] = "";
	const bool made = rc_hash_generate(hash, RC_CONSOLE_PLAYSTATION, &iterator) != 0;
	rc_hash_destroy_iterator(&iterator);
	return made ? hash : "";
}

void discWorker(State *s)
{
	for (;;)
	{
		DiscJob job;
		{
			std::unique_lock<std::mutex> lock(s->queueMutex);
			s->discWake.wait(lock, [s] { return s->quit || s->discJob.has_value(); });
			if (s->quit)
				return;
			job = std::move(*s->discJob);
			s->discJob.reset();
		}
		DiscResult result;
		result.hash = hashDisc(s->hooks, job.path);
		result.generation = job.generation;
		result.change = job.change;
		std::lock_guard<std::mutex> lock(s->queueMutex);
		s->discResults.push_back(std::move(result));
	}
}

// ------------------------------------------------------------------- badges
// The file of an achievement's picture when it is there; asks for it (once)
// when it is not.
std::string badge(State& s, const rc_client_achievement_t *achievement, bool locked)
{
	const std::string name(achievement->badge_name, strnlen(achievement->badge_name, sizeof(achievement->badge_name)));
	// The name comes from the server and becomes a file name.
	if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return isalnum(c) != 0; }))
		return "";
	const std::string key = locked ? name + "_lock" : name;
	const std::string file = s.badgeDir + key + ".png";
	bool queued = false;
	{
		std::lock_guard<std::mutex> lock(s.queueMutex);
		if (s.badgesReady.count(key) != 0)
			return file;
		if (!s.stopping && s.badgesAsked.count(key) == 0)
		{
			char url[512];
			const int which = locked ? RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE : RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
			if (rc_client_achievement_get_image_url(achievement, which, url, sizeof(url)) == RC_OK)
			{
				s.badgesAsked.insert(key);
				s.badgeJobs.push_back({ url, file, key });
				queued = true;
			}
		}
	}
	if (queued)
	{
		for (std::thread& thread : s.badgeThread)
			startThread(thread, badgeWorker, s);
		s.badgeWake.notify_one();
	}
	return "";
}

// A game was loaded: every achievement's picture as it is now, then the
// unlocked picture of those still locked, for the moment they are earned.
void fetchBadges(State& s)
{
	{
		// What failed with an earlier game gets another try.
		std::lock_guard<std::mutex> lock(s.queueMutex);
		for (const std::string& key : s.badgesFailed)
			s.badgesAsked.erase(key);
		s.badgesFailed.clear();
		s.badgeFailures = 0;
	}
	rc_client_achievement_list_t *groups = rc_client_create_achievement_list(s.client,
			RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	if (groups == nullptr)
		return;
	for (int pass = 0; pass < 2; pass++)
	{
		for (uint32_t b = 0; b < groups->num_buckets; b++)
		{
			for (uint32_t i = 0; i < groups->buckets[b].num_achievements; i++)
			{
				const rc_client_achievement_t *achievement = groups->buckets[b].achievements[i];
				const bool locked = achievement->state != RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
				if (pass == 0)
					badge(s, achievement, locked);
				else if (locked)
					badge(s, achievement, false);
			}
		}
	}
	rc_client_destroy_achievement_list(groups);
}

// --------------------------------------------------------- the running game
bool loadInFlight(const State& s)
{
	switch (rc_client_get_load_game_state(s.client))
	{
	case RC_CLIENT_LOAD_GAME_STATE_AWAIT_LOGIN:
	case RC_CLIENT_LOAD_GAME_STATE_IDENTIFYING_GAME:
	case RC_CLIENT_LOAD_GAME_STATE_FETCHING_GAME_DATA:
	case RC_CLIENT_LOAD_GAME_STATE_STARTING_SESSION:
		return true;
	default:
		return false;
	}
}

void hashDiscLater(State& s, bool change)
{
	DiscJob job;
	job.path = s.discPath;
	job.generation = ++s.discGeneration;
	job.change = change;
	s.hashing = true;
	{
		// A job not started yet is for a disc that is no longer in the tray.
		std::lock_guard<std::mutex> lock(s.queueMutex);
		s.discJob = std::move(job);
	}
	startThread(s.discThread, discWorker, s);
	s.discWake.notify_one();
}

void applyProgress(State& s, const uint8_t *data, size_t size)
{
	// A state without progress, or with progress that does not fit this
	// game's achievements, starts their counts afresh.
	if (data == nullptr || size == 0 || rc_client_deserialize_progress_sized(s.client, data, size) != RC_OK)
		rc_client_deserialize_progress_sized(s.client, nullptr, 0);
}

void RC_CCONV loadCallback(int result, const char *error, rc_client_t *client, void *userdata)
{
	State& s = *static_cast<State *>(userdata);
	if (s.stopping || result == RC_ABORTED)
		return;
	if (result == RC_OK)
	{
		const rc_client_game_t *game = rc_client_get_game_info(client);
		rc_client_user_game_summary_t counts;
		rc_client_get_user_game_summary(client, &counts);
		s.retries = 0;
		s.lastError.clear();
		diag::mark("achievements: game %u \"%s\": %u of %u achievements, %u of %u points%s",
				game != nullptr ? game->id : 0, game != nullptr ? safe(game->title) : "",
				counts.num_unlocked_achievements, counts.num_core_achievements, counts.points_unlocked,
				counts.points_core, rc_client_get_hardcore_enabled(client) ? " (hardcore)" : "");
		addEvent(s, Event::GameLoaded, game != nullptr ? safe(game->title) : "",
				format("%u of %u achievements", counts.num_unlocked_achievements, counts.num_core_achievements));
		if (s.haveProgress)
			applyProgress(s, s.progress.data(), s.progress.size());
		s.haveProgress = false;
		s.progress.clear();
		fetchBadges(s);
		return;
	}
	if (result == RC_NO_GAME_LOADED)
	{
		s.unknown = true;
		diag::mark("achievements: the disc (%s) is not in RetroAchievements' database", s.discHash.c_str());
		addEvent(s, Event::GameUnknown, "", "This disc is not in RetroAchievements' database");
		return;
	}
	if (result == RC_LOGIN_REQUIRED)
		return;		// logged out meanwhile: loaded after the next login
	const std::string why = error != nullptr && error[0] != 0 ? error : rc_error_str(result);
	diag::mark("achievements: the game's achievements could not be loaded: %s (%d)", why.c_str(), result);
	s.lastError = why;
	if (s.retries == 0)
		addEvent(s, Event::ServerError, "", "The achievements could not be loaded: " + why);
	if (s.retries < LoadRetries)
		s.retryAt = now() + LoadRetrySeconds[s.retries++];
}

void RC_CCONV changeCallback(int result, const char *error, rc_client_t *, void *userdata)
{
	State& s = *static_cast<State *>(userdata);
	if (s.stopping || result == RC_OK || result == RC_ABORTED)
		return;
	if (result == RC_HARDCORE_DISABLED)
	{
		diag::mark("achievements: hardcore mode off: the disc put in (%s) is not in the database", s.discHash.c_str());
		addEvent(s, Event::GameUnknown, "",
				"Hardcore mode was switched off: this disc is not in RetroAchievements' database");
		return;
	}
	const std::string why = error != nullptr && error[0] != 0 ? error : rc_error_str(result);
	diag::mark("achievements: the disc put in could not be checked: %s (%d)", why.c_str(), result);
	s.lastError = why;
}

// Takes the running game's identification one step further: the hash first,
// then, once somebody is logged in, the game's data.
void identify(State& s)
{
	if (s.stopping || s.discPath.empty() || s.unknown || rc_client_is_game_loaded(s.client) || loadInFlight(s))
		return;
	const bool loggedIn = rc_client_get_user_info(s.client) != nullptr;
	if (!loggedIn && !s.loggingIn)
		return;
	if (s.discHash.empty())
	{
		if (!s.hashing)
			hashDiscLater(s, false);
		return;
	}
	if (loggedIn)
		rc_client_begin_load_game(s.client, s.discHash.c_str(), loadCallback, &s);
}

void discHashed(State& s, const DiscResult& result)
{
	if (result.generation != s.discGeneration || s.discPath.empty())
		return;		// of a disc that is no longer the one in the tray
	s.hashing = false;
	s.discHash = result.hash;
	if (result.hash.empty())
		diag::mark("achievements: the disc could not be identified (%s)", baseName(s.discPath).c_str());
	else
		diag::mark("achievements: disc hash %s (%s)", result.hash.c_str(), baseName(s.discPath).c_str());
	if (result.change && rc_client_is_game_loaded(s.client))
	{
		if (!result.hash.empty())
			rc_client_begin_change_media(s.client, result.hash.c_str(), changeCallback, &s);
		else if (rc_client_get_hardcore_enabled(s.client))
		{
			// rc_client's rule for a disc it cannot place, applied to one
			// that cannot even be read.
			rc_client_set_hardcore_enabled(s.client, 0);
			addEvent(s, Event::GameUnknown, "", "Hardcore mode was switched off: the disc could not be identified");
		}
		return;
	}
	if (result.hash.empty())
	{
		s.unknown = true;
		addEvent(s, Event::GameUnknown, "", "The disc could not be identified for RetroAchievements");
		return;
	}
	identify(s);
}

void startGame(State& s, const std::string& discPath)
{
	rc_client_unload_game(s.client);
	s.gameNumber++;
	s.completedTold[0] = s.completedTold[1] = false;
	s.discPath = discPath;
	s.discHash.clear();
	s.discGeneration++;
	s.hashing = false;
	s.unknown = false;
	s.retries = 0;
	s.retryAt = 0;
	s.haveProgress = false;
	s.progress.clear();
	s.presence.clear();
	s.presenceAt = 0;
	s.best.clear();
	s.submitted.clear();
	s.progressNow = Event();
	s.progressTold.clear();
	identify(s);
}

// ---------------------------------------------------------------- the user
void RC_CCONV loginCallback(int result, const char *error, rc_client_t *client, void *userdata)
{
	State& s = *static_cast<State *>(userdata);
	s.loggingIn = false;
	// Nothing is said of a login that was given up, or that another one
	// replaced before its answer came.
	if (s.stopping || result == RC_ABORTED || s.nextLogin.has_value())
		return;
	if (result != RC_OK)
	{
		const std::string why = error != nullptr && error[0] != 0 ? error : rc_error_str(result);
		diag::mark("achievements: login failed: %s (%d)", why.c_str(), result);
		s.lastError = why;
		addEvent(s, Event::LoginFailed, "", why);
		return;
	}
	const rc_client_user_t *user = rc_client_get_user_info(client);
	if (user == nullptr)
		return;
	const uint32_t points = rc_client_get_hardcore_enabled(client) ? user->score : user->score_softcore;
	diag::mark("achievements: logged in as %s (%u points, %u in softcore)", safe(user->display_name), user->score,
			user->score_softcore);
	s.lastError.clear();
	addEvent(s, Event::LoggedIn, safe(user->display_name), format("%u points", points), (int)points);
	identify(s);
}

void logOut(State& s)
{
	s.nextLogin.reset();
	s.retryAt = 0;
	rc_client_logout(s.client);		// also unloads the game
}

void beginLogin(State& s, const Login& login)
{
	if (rc_client_get_user_info(s.client) != nullptr)
		rc_client_logout(s.client);
	s.loggingIn = true;
	if (login.withToken)
		rc_client_begin_login_with_token(s.client, login.user.c_str(), login.secret.c_str(), loginCallback, &s);
	else
		rc_client_begin_login_with_password(s.client, login.user.c_str(), login.secret.c_str(), loginCallback, &s);
}

void login(const std::string& user, const std::string& secret, bool withToken)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return;
	State& s = *state;
	Login wanted{ user, secret, withToken };
	// rc_client takes one login at a time: this one starts when the answer to
	// the one on its way has come (pump()).
	if (s.loggingIn)
		s.nextLogin = std::move(wanted);
	else
		beginLogin(s, wanted);
}

// ------------------------------------------------------- rc_client's hooks
uint32_t RC_CCONV readMemory(uint32_t address, uint8_t *buffer, uint32_t bytes, rc_client_t *client)
{
	const State *s = static_cast<const State *>(rc_client_get_userdata(client));
	return s->hooks.readMemory ? s->hooks.readMemory(address, buffer, bytes) : 0;
}

// The value of a request's "r" parameter ("awardachievement").
std::string requestName(const char *query)
{
	for (const char *p = query; p != nullptr && *p != 0;)
	{
		const char *end = strchr(p, '&');
		if (p[0] == 'r' && p[1] == '=')
			return end != nullptr ? std::string(p + 2, end) : std::string(p + 2);
		p = end != nullptr ? end + 1 : nullptr;
	}
	return "request";
}

// Runs inside a call into rc_client, so with `mutex` held.
void RC_CCONV serverCall(const rc_api_request_t *request, rc_client_server_callback_t callback, void *callbackData,
		rc_client_t *client)
{
	State& s = *static_cast<State *>(rc_client_get_userdata(client));
	ServerCall call;
	call.url = request->url;
	const char *query = strchr(request->url, '?');
	if (request->post_data != nullptr && request->post_data[0] != 0)
	{
		call.url += query != nullptr ? '&' : '?';
		call.url += request->post_data;
		call.name = requestName(request->post_data);
	}
	else
		call.name = requestName(query != nullptr ? query + 1 : "");
	call.unlock = call.name == "awardachievement";
	call.lastCall = call.unlock || call.name == "submitlbentry";
	call.hardcore = call.unlock && strstr(call.url.c_str(), "&h=1") != nullptr;
	call.game = s.gameNumber;
	call.callback = callback;
	call.callbackData = callbackData;
	if (s.stopping)
	{
		// The workers are gone: shutdown() answers it.
		call.status = NotSent;
		std::lock_guard<std::mutex> lock(s.queueMutex);
		s.answers.push_back(std::move(call));
		return;
	}
	{
		std::lock_guard<std::mutex> lock(s.queueMutex);
		s.calls.push_back(std::move(call));
	}
	startThread(s.callThread, callWorker, s);
	s.callWake.notify_one();
}

void RC_CCONV logMessage(const char *message, const rc_client_t *)
{
	diag::mark("achievements: rcheevos: %s", message);
}

void scoreNotice(State& s, const Submitted& submitted, const std::string& rank)
{
	addEvent(s, Event::LeaderboardSubmitted, submitted.title, rank.empty() ? submitted.score : submitted.score + ", " + rank);
}

void tellProgress(State& s)
{
	addEvent(s, Event::Progress, s.progressNow.title, s.progressNow.text, s.progressNow.points, s.progressNow.badge);
	s.progressTold = s.progressNow.text;
	s.progressToldAt = now();
}

void RC_CCONV eventHandler(const rc_client_event_t *event, rc_client_t *client)
{
	State& s = *static_cast<State *>(rc_client_get_userdata(client));
	if (s.stopping)
		return;
	const rc_client_achievement_t *achievement = event->achievement;
	const rc_client_leaderboard_t *leaderboard = event->leaderboard;
	switch (event->type)
	{
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
		diag::mark("achievements: unlocked %u \"%s\" (%u points%s)", achievement->id, safe(achievement->title),
				achievement->points, rc_client_get_hardcore_enabled(client) ? ", hardcore" : "");
		// Its count is not told again after it.
		if (s.progressNow.title == safe(achievement->title))
		{
			s.progressNow = Event();
			s.progressTold.clear();
		}
		addEvent(s, Event::Unlocked, safe(achievement->title), safe(achievement->description), (int)achievement->points,
				badge(s, achievement, false));
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
		addEvent(s, Event::Challenge, safe(achievement->title), safe(achievement->description), (int)achievement->points,
				badge(s, achievement, false));
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
	{
		// rc_client has one indicator and updates it, every frame for a count
		// of frames. Here each event is a notice: the first count at once,
		// then one a second, and the last one when the indicator goes.
		const bool another = s.progressNow.title != safe(achievement->title);
		s.progressNow.kind = Event::Progress;
		s.progressNow.title = safe(achievement->title);
		s.progressNow.text = achievement->measured_progress;
		s.progressNow.points = (int)achievement->points;
		s.progressNow.badge = badge(s, achievement, false);
		if (another || now() - s.progressToldAt >= ProgressSeconds)
			tellProgress(s);
		break;
	}
	case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
		if (s.progressNow.text != s.progressTold)
			tellProgress(s);
		s.progressNow = Event();
		s.progressTold.clear();
		break;
	case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
		// A notice goes away by itself.
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
		addEvent(s, Event::LeaderboardStarted, safe(leaderboard->title), safe(leaderboard->description));
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
		addEvent(s, Event::LeaderboardFailed, safe(leaderboard->title), safe(leaderboard->description));
		break;
	case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
	{
		// The score goes to the server now; its notice waits a moment for
		// the rank the server answers (the scoreboard event below).
		Submitted submitted;
		submitted.id = leaderboard->id;
		submitted.title = safe(leaderboard->title);
		submitted.score = safe(leaderboard->tracker_value);
		submitted.until = now() + RankSeconds;
		diag::mark("achievements: leaderboard %u \"%s\": %s submitted", leaderboard->id, submitted.title.c_str(),
				submitted.score.c_str());
		s.submitted.push_back(std::move(submitted));
		break;
	}
	case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
	{
		const rc_client_leaderboard_scoreboard_t *board = event->leaderboard_scoreboard;
		s.best[board->leaderboard_id] = board->best_score;
		const auto waiting = std::find_if(s.submitted.begin(), s.submitted.end(),
				[board](const Submitted& entry) { return entry.id == board->leaderboard_id; });
		if (waiting != s.submitted.end())
		{
			Submitted submitted = std::move(*waiting);
			s.submitted.erase(waiting);
			submitted.score = board->submitted_score;
			scoreNotice(s, submitted, board->new_rank != 0 ? format("rank %u of %u", board->new_rank, board->num_entries) : "");
		}
		break;
	}
	case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
	case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
	case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
		// leaderboards() gives an attempt's running value.
		break;
	case RC_CLIENT_EVENT_GAME_COMPLETED:
	case RC_CLIENT_EVENT_SUBSET_COMPLETED:
		// Never raised: answer() tells a completed game itself, and why.
		break;
	case RC_CLIENT_EVENT_RESET:
		// Hardcore mode came on while a game ran: setHardcore() calls the hook.
		s.resetWanted = true;
		break;
	case RC_CLIENT_EVENT_SERVER_ERROR:
	{
		const rc_client_server_error_t *problem = event->server_error;
		const std::string why = safe(problem->error_message);
		diag::mark("achievements: server error (%s %u): %s", safe(problem->api), problem->related_id, why.c_str());
		s.lastError = why;
		// A score the server refused was not submitted.
		if (strcmp(safe(problem->api), "submit_lboard_entry") == 0)
			std::erase_if(s.submitted, [problem](const Submitted& entry) { return entry.id == problem->related_id; });
		addEvent(s, Event::ServerError, "", why);
		break;
	}
	case RC_CLIENT_EVENT_DISCONNECTED:
		diag::mark("achievements: the server does not answer; unlocks wait");
		addEvent(s, Event::Disconnected, "", "An unlock is waiting for the network");
		break;
	case RC_CLIENT_EVENT_RECONNECTED:
		diag::mark("achievements: the server answers again; the waiting unlocks were sent");
		addEvent(s, Event::Reconnected, "", "The waiting unlocks were sent");
		break;
	default:
		break;
	}
}

// ----------------------------------------------------- answers to rc_client
// What rc_client is told when the server answered an error status and the
// client gave no body with it (the console's reads one only on success):
// RetroAchievements' own error body for that status. Without it rc_client
// takes the call as unanswered: it would try an unlock again for ever, and a
// disc the server does not know would look like a network fault.
std::string bodyForStatus(int status)
{
	const char *message = "The server refused the request";
	const char *code = "";
	if (status == 401)
	{
		message = "The user name, password or token was not accepted";
		code = "invalid_credentials";
	}
	else if (status == 403)
	{
		message = "Access denied";
		code = "access_denied";
	}
	else if (status == 404)
	{
		message = "Not found";
		code = "not_found";
	}
	std::string body = format("{\"Success\":false,\"Error\":\"%s (HTTP %d)\",\"Status\":%d", message, status, status);
	if (code[0] != 0)
		body += format(",\"Code\":\"%s\"", code);
	return body + "}";
}

// An unlock's answer says how many achievements are left; at none, rc_client
// 12.5 marks the game completed and raises the event with the next frame.
// Both steps read the loaded game without looking whether there is one
// (rc_client_award_achievement_callback and rc_client_raise_mastery_event,
// called from rc_client_unload_game): a null pointer when the answer comes
// after the game ended, or the game ends before another frame ran. So
// rc_client does not get to see the field (its key is made one it does not
// know), and the module tells the completion. Gives the number, or -1.
long takeRemaining(std::string& body)
{
	static const char key[] = "\"AchievementsRemaining\"";
	const size_t at = body.find(key);
	if (at == std::string::npos)
		return -1;
	size_t value = at + sizeof(key) - 1;
	while (value < body.size() && (body[value] == ':' || isspace((unsigned char)body[value])))
		value++;
	const long remaining = value < body.size() && isdigit((unsigned char)body[value])
			? strtol(body.c_str() + value, nullptr, 10) : -1;
	body[at + 1] = '_';
	return remaining;
}

void completed(State& s, bool mastered)
{
	const rc_client_game_t *game = rc_client_get_game_info(s.client);
	if (game == nullptr || std::exchange(s.completedTold[mastered], true))
		return;
	diag::mark("achievements: \"%s\" %s", safe(game->title), mastered ? "mastered" : "completed");
	addEvent(s, Event::Completed, safe(game->title), mastered ? "Mastered: every achievement earned in hardcore mode"
			: "Completed: every achievement earned");
}

void answer(State& s, ServerCall& call)
{
	rc_api_server_response_t response{};
	long remaining = -1;
	if (call.status == NotSent)
	{
		// Not to be tried again: rc_client frees what it kept for the call.
		call.body = "Shutting down";
		response.http_status_code = RC_API_SERVER_RESPONSE_CLIENT_ERROR;
	}
	else if (call.status == NoAnswer)
	{
		call.body = "The server could not be reached";
		response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
		if (!s.serverSilent && !s.stopping)
			diag::mark("achievements: %s: no answer from the server", call.name.c_str());
		s.serverSilent = true;
	}
	else
	{
		if (s.serverSilent)
			diag::mark("achievements: %s: the server answers again", call.name.c_str());
		s.serverSilent = false;
		if (call.status != 200)
			diag::mark("achievements: %s: HTTP %d", call.name.c_str(), call.status);
		if (call.body.empty() && call.status >= 400 && call.status < 500 && call.status != 429)
			call.body = bodyForStatus(call.status);
		if (call.unlock)
			remaining = takeRemaining(call.body);
		response.http_status_code = call.status;
	}
	response.body = call.body.c_str();
	response.body_length = call.body.size();
	call.callback(&response, call.callbackData);
	// The last achievement of the game that is still the one running.
	if (remaining == 0 && call.game == s.gameNumber && rc_client_is_game_loaded(s.client))
		completed(s, call.hardcore);
}

// Hands rc_client what the workers finished, and does what was waiting for
// it. On the thread the emulator runs on: rc_client reads memory in here.
void pump(State& s)
{
	for (;;)
	{
		ServerCall call;
		{
			std::lock_guard<std::mutex> lock(s.queueMutex);
			if (s.answers.empty())
				break;
			call = std::move(s.answers.front());
			s.answers.pop_front();
		}
		answer(s, call);
	}
	for (;;)
	{
		DiscResult result;
		{
			std::lock_guard<std::mutex> lock(s.queueMutex);
			if (s.discResults.empty())
				break;
			result = std::move(s.discResults.front());
			s.discResults.pop_front();
		}
		discHashed(s, result);
	}
	if (!s.loggingIn && s.nextLogin.has_value())
	{
		const Login login = std::move(*s.nextLogin);
		s.nextLogin.reset();
		beginLogin(s, login);
	}
	if (s.retryAt != 0 && now() >= s.retryAt)
	{
		s.retryAt = 0;
		identify(s);
	}
	// Scores whose rank did not come in time are shown without it.
	while (!s.submitted.empty() && now() >= s.submitted.front().until)
	{
		const Submitted submitted = std::move(s.submitted.front());
		s.submitted.erase(s.submitted.begin());
		scoreNotice(s, submitted, "");
	}
}

} // namespace

void init(const Hooks& hooks, const std::string& cacheDir)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state != nullptr)
		return;
	State *s = new State;
	s->hooks = hooks;
	s->badgeDir = cacheDir + "badges/";
	makeDir(s->badgeDir);
	s->client = rc_client_create(readMemory, serverCall);
	if (s->client == nullptr)
	{
		diag::mark("achievements: rc_client could not be created");
		delete s;
		return;
	}
	rc_client_set_userdata(s->client, s);
	rc_client_set_event_handler(s->client, eventHandler);
	rc_client_enable_logging(s->client, RC_CLIENT_LOG_LEVEL_INFO, logMessage);
	// rc_client starts in hardcore mode; here it is the user's choice.
	rc_client_set_hardcore_enabled(s->client, 0);
#if !defined(SWANSTATION_PS5)
	// PC builds only: another server in RetroAchievements' place (the test's
	// mock), for the calls and for the pictures.
	if (const char *host = getenv("SWANSTATION_RA_HOST"); host != nullptr && host[0] != 0)
	{
		rc_client_set_host(s->client, host);
		rc_api_set_image_host(host);
		diag::mark("achievements: the server is %s", host);
	}
#endif
	// What the HTTP client's User-Agent should end with (RetroAchievements
	// tells clients apart by it); the transport's own is set in the platform
	// layer.
	char clause[64];
	rc_client_get_user_agent_clause(s->client, clause, sizeof(clause));
	diag::mark("achievements: ready (%s)", clause);
	state = s;
}

void shutdown()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return;
	State& s = *state;
	{
		std::lock_guard<std::mutex> queueLock(s.queueMutex);
		s.quit = true;
		s.lastCallsUntil = now() + LastCallsSeconds;
	}
	s.callWake.notify_all();
	s.badgeWake.notify_all();
	s.discWake.notify_all();
	// A worker in the middle of a request or of a disc read ends it first.
	if (s.callThread.joinable())
		s.callThread.join();
	if (s.discThread.joinable())
		s.discThread.join();
	for (std::thread& thread : s.badgeThread)
		if (thread.joinable())
			thread.join();
	// rc_client keeps something for every call until it is answered: the
	// answers that came are handed over, the calls never sent are answered
	// "not sent", and so is whatever those answers set off.
	s.stopping = true;
	for (int round = 0; round < 16; round++)
	{
		std::deque<ServerCall> answers;
		{
			std::lock_guard<std::mutex> queueLock(s.queueMutex);
			answers.swap(s.answers);
			for (ServerCall& call : s.calls)
			{
				call.status = NotSent;
				answers.push_back(std::move(call));
			}
			s.calls.clear();
		}
		if (answers.empty())
			break;
		for (ServerCall& call : answers)
			answer(s, call);
	}
	rc_client_destroy(s.client);
#if !defined(SWANSTATION_PS5)
	rc_api_set_image_host(nullptr);
#endif
	diag::mark("achievements: shut down");
	state = nullptr;
	delete &s;
}

void loginWithPassword(const std::string& user, const std::string& password)
{
	login(user, password, false);
}

void loginWithToken(const std::string& user, const std::string& token)
{
	login(user, token, true);
}

void logout()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return;
	if (rc_client_get_user_info(state->client) != nullptr)
		diag::mark("achievements: logged out");
	logOut(*state);
}

std::string token()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return "";
	const rc_client_user_t *user = rc_client_get_user_info(state->client);
	return user != nullptr ? safe(user->token) : "";
}

void setHardcore(bool on)
{
	std::function<void()> reset;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (state == nullptr)
			return;
		State& s = *state;
		if ((rc_client_get_hardcore_enabled(s.client) != 0) == on)
			return;
		rc_client_set_hardcore_enabled(s.client, on ? 1 : 0);
		diag::mark("achievements: hardcore mode %s", on ? "on" : "off");
		if (std::exchange(s.resetWanted, false))
		{
			// rc_client watches nothing until gameReset() says the reset was done.
			reset = s.hooks.reset;
			if (!reset)
				diag::mark("achievements: no reset hook; achievements wait for the game to be reset");
		}
	}
	// Outside the lock: the reset comes back in through gameReset().
	if (reset)
		reset();
}

bool hardcore()
{
	std::lock_guard<std::mutex> lock(mutex);
	return state != nullptr && rc_client_get_hardcore_enabled(state->client) != 0;
}

void setUnofficial(bool on)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state != nullptr)
		rc_client_set_unofficial_enabled(state->client, on ? 1 : 0);
}

void gameStarted(const std::string& discPath)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state != nullptr)
		startGame(*state, discPath);
}

void gameStopped()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return;
	State& s = *state;
	// The scores sent in the game's last moments are still told.
	for (const Submitted& submitted : s.submitted)
		scoreNotice(s, submitted, "");
	startGame(s, "");
}

void discChanged(const std::string& discPath)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return;
	State& s = *state;
	if (discPath.empty() || discPath == s.discPath)
		return;
	if (!rc_client_is_game_loaded(s.client))
	{
		// Nothing is loaded that the disc could belong to: identified as a game.
		startGame(s, discPath);
		return;
	}
	s.discPath = discPath;
	s.discHash.clear();
	hashDiscLater(s, true);
}

void gameReset()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return;
	// Progress kept for achievements still on their way was the old run's.
	state->haveProgress = false;
	state->progress.clear();
	rc_client_reset(state->client);
}

void frame()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return;
	State& s = *state;
	pump(s);
	rc_client_do_frame(s.client);
	if (now() >= s.presenceAt && rc_client_is_game_loaded(s.client))
	{
		char presence[256] = "";
		rc_client_get_rich_presence_message(s.client, presence, sizeof(presence));
		s.presence = presence;
		s.presenceAt = now() + PresenceSeconds;
	}
}

void idle()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return;
	pump(*state);
	rc_client_idle(state->client);
}

std::vector<uint8_t> saveProgress()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return {};
	State& s = *state;
	if (!rc_client_is_game_loaded(s.client))
		// What a state brought before the achievements were there goes into
		// the next state unchanged.
		return s.haveProgress ? s.progress : std::vector<uint8_t>();
	std::vector<uint8_t> data(rc_client_progress_size(s.client));
	if (data.empty() || rc_client_serialize_progress_sized(s.client, data.data(), data.size()) != RC_OK)
		data.clear();
	return data;
}

void loadProgress(const uint8_t *data, size_t size)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return;
	State& s = *state;
	if (rc_client_is_game_loaded(s.client))
	{
		applyProgress(s, data, size);
		return;
	}
	// The game's achievements are still on their way (a state loaded as the
	// game starts): kept until they are there.
	s.haveProgress = !s.discPath.empty();
	s.progress.clear();
	if (s.haveProgress && data != nullptr)
		s.progress.assign(data, data + size);
}

Summary summary()
{
	std::lock_guard<std::mutex> lock(mutex);
	Summary out;
	if (state == nullptr)
		return out;
	State& s = *state;
	const rc_client_user_t *user = rc_client_get_user_info(s.client);
	out.hardcore = rc_client_get_hardcore_enabled(s.client) != 0;
	out.loggedIn = user != nullptr;
	out.loggingIn = s.loggingIn || s.nextLogin.has_value();
	if (user != nullptr)
	{
		out.user = safe(user->display_name);
		out.userPoints = (int)(out.hardcore ? user->score : user->score_softcore);
	}
	out.gameLoaded = rc_client_is_game_loaded(s.client) != 0;
	if (out.gameLoaded)
	{
		const rc_client_game_t *game = rc_client_get_game_info(s.client);
		rc_client_user_game_summary_t counts;
		rc_client_get_user_game_summary(s.client, &counts);
		out.game = safe(game->title);
		out.gameId = game->id;
		out.unlocked = (int)counts.num_unlocked_achievements;
		out.total = (int)counts.num_core_achievements;
		out.points = (int)counts.points_unlocked;
		out.totalPoints = (int)counts.points_core;
		out.richPresence = s.presence;
	}
	else
		out.gameLoading = !s.discPath.empty() && !s.unknown && (out.loggedIn || out.loggingIn)
				&& (s.hashing || loadInFlight(s) || s.retryAt != 0 || !out.loggedIn);
	out.lastError = s.lastError;
	return out;
}

std::vector<Achievement> list()
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<Achievement> out;
	if (state == nullptr)
		return out;
	State& s = *state;
	const int category = rc_client_get_unofficial_enabled(s.client) ? RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL
			: RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE;
	// By lock state, rc_client gives what is left to earn first, then what
	// this emulator cannot earn, then what was earned.
	rc_client_achievement_list_t *groups = rc_client_create_achievement_list(s.client, category,
			RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	if (groups == nullptr)
		return out;
	for (uint32_t b = 0; b < groups->num_buckets; b++)
	{
		for (uint32_t i = 0; i < groups->buckets[b].num_achievements; i++)
		{
			const rc_client_achievement_t *from = groups->buckets[b].achievements[i];
			Achievement to;
			to.id = from->id;
			to.title = safe(from->title);
			to.description = safe(from->description);
			to.points = (int)from->points;
			to.unlocked = from->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
			to.unofficial = from->category == RC_CLIENT_ACHIEVEMENT_CATEGORY_UNOFFICIAL;
			to.progress = from->measured_progress;
			to.progressPercent = std::clamp(from->measured_percent, 0.f, 100.f);
			to.badge = badge(s, from, !to.unlocked);
			if (to.unlocked && from->unlock_time != 0)
			{
				tm when{};
				char date[16] = "";
				if (localtime_r(&from->unlock_time, &when) != nullptr
						&& strftime(date, sizeof(date), "%Y-%m-%d", &when) != 0)
					to.unlockedWhen = date;
			}
			out.push_back(std::move(to));
		}
	}
	rc_client_destroy_achievement_list(groups);
	// The official ones first, in that order; the unofficial ones after them.
	std::stable_partition(out.begin(), out.end(), [](const Achievement& entry) { return !entry.unofficial; });
	return out;
}

std::vector<Leaderboard> leaderboards()
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<Leaderboard> out;
	if (state == nullptr)
		return out;
	State& s = *state;
	rc_client_leaderboard_list_t *groups = rc_client_create_leaderboard_list(s.client,
			RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE);
	if (groups == nullptr)
		return out;
	for (uint32_t b = 0; b < groups->num_buckets; b++)
	{
		for (uint32_t i = 0; i < groups->buckets[b].num_leaderboards; i++)
		{
			const rc_client_leaderboard_t *from = groups->buckets[b].leaderboards[i];
			Leaderboard to;
			to.id = from->id;
			to.title = safe(from->title);
			to.description = safe(from->description);
			const auto best = s.best.find(from->id);
			if (best != s.best.end())
				to.best = best->second;
			to.tracking = from->state == RC_CLIENT_LEADERBOARD_STATE_TRACKING;
			if (to.tracking)
				to.value = safe(from->tracker_value);
			out.push_back(std::move(to));
		}
	}
	rc_client_destroy_leaderboard_list(groups);
	return out;
}

std::vector<Event> takeEvents()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (state == nullptr)
		return {};
	State& s = *state;
	pump(s);
	std::vector<Event> out(std::make_move_iterator(s.events.begin()), std::make_move_iterator(s.events.end()));
	s.events.clear();
	return out;
}

}
