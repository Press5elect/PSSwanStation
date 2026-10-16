/*
	PSSwanStation - pictures and videos from ScreenScraper (screenscraper.fr).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	ScreenScraper is a community database of games and their media. Its
	interface (api2, jeuInfos.php) is asked about one game at a time, by the
	disc image's file name and size and, when it is known, the disc's serial
	number, for the PlayStation (system 57); its answer lists the game's
	media, each with its kind, region, format and address. The kinds chosen
	(Settings, Games and network, ScreenScraper) are fetched into the folders
	ES-DE keeps them in, <root>media/<kind>/<file name>.<format>, and the box
	also into covers/ when the game has no cover there, so the library shows
	it too.

	Every request carries two keys: the program's (a developer account
	ScreenScraper gives a program; it comes from the environment the title is
	built in, SCREENSCRAPER_DEVID and SCREENSCRAPER_DEVPASSWORD, and is in no
	file of the repository; data/screenscraper.cfg may name another) and the
	user's own account, which sets how many requests a day it may make and
	how fast. One request at a time, as an ordinary account allows; a
	refusal for the day stops it until the next start. The addresses carry
	the keys, so none is written to the boot log.

	Nothing is fetched unless it is switched on with an account. What leaves
	the console is in PRIVACY.md.
*/
#include "fe.h"
#include "json.h"

#include "screenscraper_key.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fe::scraper
{
namespace
{

constexpr const char *Api = "https://api.screenscraper.fr/api2/";
constexpr const char *SoftName = "PSSwanStation";
constexpr int SystemId = 57;	// Sony PlayStation
constexpr uint64_t MaxMedia = 200ull << 20;

Settings current;
bool settingsRead = false;
std::string devId, devPassword;		// from the file, else the build's

std::string settingsFile()
{
	return rootDir + "data/screenscraper.cfg";
}

std::string fromHex(const char *hex)
{
	std::string out;
	for (size_t i = 0; hex[i] != 0 && hex[i + 1] != 0; i += 2)
	{
		const auto digit = [](char c) { return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; };
		out += (char)(digit(hex[i]) * 16 + digit(hex[i + 1]));
	}
	return out;
}

void readSettings()
{
	if (settingsRead)
		return;
	settingsRead = true;
	FILE *f = fopen(settingsFile().c_str(), "r");
	if (f != nullptr)
	{
		char line[512];
		while (fgets(line, sizeof(line), f) != nullptr)
		{
			const std::string text = trim(line);
			const size_t eq = text.find('=');
			if (text.empty() || text[0] == '#' || eq == std::string::npos)
				continue;
			const std::string key = trim(text.substr(0, eq)), value = trim(text.substr(eq + 1));
			const bool on = value == "1";
			Kinds& k = current.kinds;
			if (key == "enabled")
				current.enabled = on;
			else if (key == "user")
				current.user = value;
			else if (key == "password")
				current.password = value;
			else if (key == "automatic")
				current.automatic = on;
			else if (key == "devid")
				devId = value;
			else if (key == "devpassword")
				devPassword = value;
			else if (key == "covers")
				k.covers = on;
			else if (key == "screenshots")
				k.screenshots = on;
			else if (key == "titlescreens")
				k.titlescreens = on;
			else if (key == "marquees")
				k.marquees = on;
			else if (key == "miximages")
				k.miximages = on;
			else if (key == "3dboxes")
				k.boxes3d = on;
			else if (key == "backcovers")
				k.backcovers = on;
			else if (key == "physicalmedia")
				k.physicalmedia = on;
			else if (key == "fanart")
				k.fanart = on;
			else if (key == "videos")
				k.videos = on;
		}
		fclose(f);
	}
	if (devId.empty())
	{
		devId = fromHex(FE_SS_DEVID_HEX);
		devPassword = fromHex(FE_SS_DEVPASSWORD_HEX);
	}
}

// ---- the queue

struct Job
{
	library::Game game;
	bool byHand = false;
};

std::mutex mutex;
std::condition_variable wake;
std::deque<Job> queue;
std::set<std::string> asked;
std::set<std::string> notFound;
bool notFoundRead = false;
// The games asked about before, with the kinds asked for then (a bit each):
// not asked again by themselves while the kinds are the same, so a kind
// ScreenScraper lacks does not cost a request each start.
std::map<std::string, unsigned> done;
bool started = false;
std::atomic<bool> stopNow{false};
std::atomic<int> pendingCount{0};
std::atomic<int> fetchedGames{0}, fetchedFiles{0}, unknownGames{0};
std::string statusText, accountText;
bool closedForToday = false;	// the day's requests are used up, or the keys refused
int unreachable = 0;			// requests in a row that found no server

std::string notFoundFile()
{
	return rootDir + "data/screenscraper-notfound.txt";
}

std::string doneFile()
{
	return rootDir + "data/screenscraper-done.txt";
}

unsigned kindsMask()
{
	const Kinds& k = current.kinds;
	const bool bits[] = { k.covers, k.screenshots, k.titlescreens, k.marquees, k.miximages, k.boxes3d, k.backcovers,
		k.physicalmedia, k.fanart, k.videos };
	unsigned mask = 0;
	for (int i = 0; i < 10; i++)
		mask |= bits[i] ? 1u << i : 0u;
	return mask;
}

void readNotFound()
{
	if (notFoundRead)
		return;
	notFoundRead = true;
	if (FILE *f = fopen(doneFile().c_str(), "r"))
	{
		char line[600];
		while (fgets(line, sizeof(line), f) != nullptr)
		{
			const std::string s = trim(line);
			const size_t bar = s.rfind('|');
			if (bar != std::string::npos)
				done[s.substr(0, bar)] = (unsigned)strtoul(s.c_str() + bar + 1, nullptr, 10);
		}
		fclose(f);
	}
	if (FILE *f = fopen(notFoundFile().c_str(), "r"))
	{
		char line[512];
		while (fgets(line, sizeof(line), f) != nullptr)
		{
			const std::string s = trim(line);
			if (!s.empty())
				notFound.insert(s);
		}
		fclose(f);
	}
}

void setStatus(const std::string& text)
{
	std::lock_guard<std::mutex> lock(mutex);
	statusText = text;
}

std::string encode(const std::string& s)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (const unsigned char c : s)
	{
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += (char)c;
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

// ScreenScraper's address; the PC's test answers from this machine instead.
std::string api()
{
#if defined(SWANSTATION_HOST)
	if (const char *test = getenv("SWANSTATION_SCREENSCRAPER_API"))
		return test;
#endif
	return Api;
}

std::string keys()
{
	return "devid=" + encode(devId) + "&devpassword=" + encode(devPassword) + "&softname=" + encode(SoftName)
			+ "&output=json&ssid=" + encode(current.user) + "&sspassword=" + encode(current.password);
}

// What a refusal means, as ScreenScraper's documentation lists them.
std::string refusal(int status, const std::vector<uint8_t>& body)
{
	const std::string text = trim(std::string(body.begin(), body.begin() + (long)std::min<size_t>(body.size(), 200)));
	switch (status)
	{
	case 400: return "ScreenScraper did not understand the request";
	case 401: return "ScreenScraper is closed to programs while it is busy (try later)";
	case 403: return "ScreenScraper refused the keys: the account's name or password, or this build's developer key";
	case 404: return "not in ScreenScraper";
	case 423: return "ScreenScraper's interface is closed for now";
	case 426: return "ScreenScraper no longer accepts this program's key";
	case 429: return "too many requests at once for this account";
	case 430: return "this account's requests for today are used up";
	case 431: return "too many games ScreenScraper did not know today";
	default: break;
	}
	return status < 0 ? "ScreenScraper could not be reached" : format("ScreenScraper answered %d", status)
			+ (text.empty() ? "" : " (" + text.substr(0, 80) + ")");
}

void readAccount(const Json& response)
{
	const Json *user = response.find("ssuser");
	if (user == nullptr)
		return;
	const std::string today = user->string("requeststoday"), most = user->string("maxrequestsperday");
	const std::string level = user->string("niveau");
	std::lock_guard<std::mutex> lock(mutex);
	accountText = user->string("id") + (level.empty() ? "" : ", level " + level)
			+ (today.empty() ? "" : ", " + today + " of " + most + " requests today");
}

// The regions to prefer for a game, as ScreenScraper names them.
std::vector<std::string> regionsFor(const library::Game& game)
{
	std::vector<std::string> order;
	if (game.region == "Europe")
		order = { "eu", "fr", "uk", "de", "wor", "us", "jp" };
	else if (game.region == "Japan")
		order = { "jp", "wor", "us", "eu" };
	else
		order = { "us", "wor", "eu", "jp" };
	order.push_back("ss");
	order.push_back("");
	return order;
}

struct Media
{
	std::string url, format;
};

// The game's media of the first of `types` there is, in the regions' order.
bool pick(const Json& medias, std::initializer_list<const char *> types, const std::vector<std::string>& regions,
		Media& out)
{
	for (const char *type : types)
		for (const std::string& region : regions)
			for (const Json& m : medias.items)
				if (m.string("type") == type && (region.empty() || m.string("region") == region)
						&& !m.string("url").empty())
				{
					out.url = m.string("url");
					out.format = m.string("format");
					if (out.format.empty())
						out.format = std::string(type).find("video") != std::string::npos ? "mp4" : "png";
					return true;
				}
	return false;
}

bool fetchFile(const std::string& url, const std::string& path)
{
	const size_t slash = path.rfind('/');
	makeDir(path.substr(0, slash));
	const std::string temporary = path + ".part";
	const int status = platform::httpDownload(url, temporary, MaxMedia, [](uint64_t, uint64_t) { return !stopNow.load(); });
	if (status != 200)
	{
		unlink(temporary.c_str());
		return false;
	}
	if (rename(temporary.c_str(), path.c_str()) != 0)
	{
		unlink(temporary.c_str());
		return false;
	}
	return true;
}

bool haveLocal(const std::string& folder, const std::string& title)
{
	for (const char *ext : { ".png", ".jpg", ".jpeg", ".mp4" })
	{
		struct stat st;
		if (stat((rootDir + "media/" + folder + "/" + title + ext).c_str(), &st) == 0)
			return true;
	}
	return false;
}

void scrape(const Job& job)
{
	const library::Game& game = job.game;
	// Nothing asked when every kind chosen is there already.
	{
		const Kinds& k = current.kinds;
		const std::pair<bool, const char *> kinds[] = { { k.covers, "covers" }, { k.screenshots, "screenshots" },
			{ k.titlescreens, "titlescreens" }, { k.marquees, "marquees" }, { k.miximages, "miximages" },
			{ k.boxes3d, "3dboxes" }, { k.backcovers, "backcovers" }, { k.physicalmedia, "physicalmedia" },
			{ k.fanart, "fanart" }, { k.videos, "videos" } };
		bool missing = false;
		for (const auto& [on, folder] : kinds)
			if (on && !haveLocal(folder, game.fileTitle) && media::find(game, folder).empty())
				missing = true;
		if (!missing)
			return;
	}
	const std::string& first = game.discs.empty() ? game.path : game.discs[0];
	const std::string fileName = first.substr(first.rfind('/') + 1);
	std::string serial = host::knownSerial(game.path);
	if (serial.empty())
		serial = gamedb::serialByName(game.fileTitle);
	std::string url = api() + "jeuInfos.php?" + keys() + format("&systemeid=%d&romtype=rom", SystemId)
			+ "&romnom=" + encode(fileName);
	// The size of the image itself (a .cue or a playlist is not what
	// ScreenScraper knows the game by: the name and serial do then).
	const std::string ext = lowercase(extension(fileName));
	if (game.size > 0 && ext != ".cue" && ext != ".m3u" && ext != ".ccd" && ext != ".mds")
		url += format("&romtaille=%llu", (unsigned long long)game.size);
	if (!serial.empty())
		url += "&serialnum=" + encode(serial);
	std::vector<uint8_t> answer;
	setStatus("Asking ScreenScraper about " + game.name + "\xe2\x80\xa6");
	const int status = platform::httpGet(url, answer, 30);
	if (status == 404)
	{
		unknownGames++;
		diag::mark("scraper: %s: not in ScreenScraper", game.fileTitle.c_str());
		std::lock_guard<std::mutex> lock(mutex);
		notFound.insert(game.fileTitle);
		if (FILE *f = fopen(notFoundFile().c_str(), "a"))
		{
			fprintf(f, "%s\n", game.fileTitle.c_str());
			fclose(f);
		}
		return;
	}
	unreachable = status < 0 ? unreachable + 1 : 0;
	if (status < 0 && unreachable >= 3)
	{
		// No network, or the server is down: not asked again this run.
		std::lock_guard<std::mutex> lock(mutex);
		closedForToday = true;
	}
	if (status != 200)
	{
		const std::string why = refusal(status, answer);
		diag::mark("scraper: %s: %s", game.fileTitle.c_str(), why.c_str());
		setStatus(why);
		if (status == 403 || status == 423 || status == 426 || status == 430 || status == 431 || status == 401)
		{
			std::lock_guard<std::mutex> lock(mutex);
			closedForToday = true;
		}
		if (status == 429)
			sleep(5);
		return;
	}
	Json page;
	JsonReader((const char *)answer.data(), answer.size()).read(page);
	const Json *response = page.find("response");
	const Json *gameJson = response != nullptr ? response->find("jeu") : nullptr;
	if (response != nullptr)
		readAccount(*response);
	const Json *medias = gameJson != nullptr ? gameJson->find("medias") : nullptr;
	if (medias == nullptr)
	{
		diag::mark("scraper: %s: an answer without media", game.fileTitle.c_str());
		return;
	}
	const std::vector<std::string> regions = regionsFor(game);
	struct Want
	{
		bool on;
		const char *folder;
		std::initializer_list<const char *> types;
	};
	const Kinds& k = current.kinds;
	const Want wants[] = {
		{ k.covers, "covers", { "box-2D" } },
		{ k.screenshots, "screenshots", { "ss" } },
		{ k.titlescreens, "titlescreens", { "sstitle" } },
		{ k.marquees, "marquees", { "wheel-hd", "wheel" } },
		{ k.miximages, "miximages", { "mixrbv2", "mixrbv1" } },
		{ k.boxes3d, "3dboxes", { "box-3D" } },
		{ k.backcovers, "backcovers", { "box-2D-back" } },
		{ k.physicalmedia, "physicalmedia", { "support-2D" } },
		{ k.fanart, "fanart", { "fanart" } },
		{ k.videos, "videos", { "video-normalized", "video" } },
	};
	int got = 0;
	for (const Want& want : wants)
	{
		if (!want.on || stopNow)
			continue;
		if (haveLocal(want.folder, game.fileTitle) || !media::find(game, want.folder).empty())
			continue;
		Media m;
		if (!pick(*medias, want.types, regions, m))
			continue;
		const std::string path = rootDir + "media/" + want.folder + "/" + game.fileTitle + "." + m.format;
		setStatus(std::string("Fetching ") + want.folder + " for " + game.name + "\xe2\x80\xa6");
		if (!fetchFile(m.url, path))
		{
			diag::mark("scraper: %s: its %s could not be fetched", game.fileTitle.c_str(), want.folder);
			continue;
		}
		got++;
		fetchedFiles++;
		media::add(want.folder, game.fileTitle, path);
		// The box is the library's cover too, when it has none.
		if (std::strcmp(want.folder, "covers") == 0 && covers::find(game).empty())
		{
			std::vector<uint8_t> bytes;
			makeDir(rootDir + "covers");
			if (readFile(path, bytes) && writeFile(rootDir + "covers/" + game.fileTitle + "." + m.format, bytes.data(),
					bytes.size()))
				covers::changed();
		}
	}
	fetchedGames++;
	diag::mark("scraper: %s: %d file(s) fetched", game.fileTitle.c_str(), got);
	const unsigned mask = kindsMask();
	std::lock_guard<std::mutex> lock(mutex);
	done[game.fileTitle] = mask;
	if (FILE *f = fopen(doneFile().c_str(), "a"))
	{
		fprintf(f, "%s|%u\n", game.fileTitle.c_str(), mask);
		fclose(f);
	}
}

void worker()
{
	for (;;)
	{
		Job job;
		{
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait(lock, [] { return !queue.empty(); });
			job = queue.front();
			queue.pop_front();
		}
		if (!stopNow && !closedForToday)
			scrape(job);
		pendingCount--;
		if (pendingCount <= 0)
		{
			stopNow = false;
			std::lock_guard<std::mutex> lock(mutex);
			if (!closedForToday)
				statusText = format("%d game(s) asked about: %d file(s) fetched, %d not in ScreenScraper",
						fetchedGames.load() + unknownGames.load(), fetchedFiles.load(), unknownGames.load());
		}
	}
}

} // namespace

Settings& settings()
{
	readSettings();
	return current;
}

void saveSettings()
{
	readSettings();
	const Kinds& k = current.kinds;
	std::string text = "# PSSwanStation - ScreenScraper (Settings, Games and network). The account is your own\n"
			"# screenscraper.fr account; this file is never served by the web panel.\n";
	text += format("enabled = %d\nautomatic = %d\n", current.enabled, current.automatic);
	text += "user = " + current.user + "\npassword = " + current.password + "\n";
	text += format("covers = %d\nscreenshots = %d\ntitlescreens = %d\nmarquees = %d\nmiximages = %d\n3dboxes = %d\n"
			"backcovers = %d\nphysicalmedia = %d\nfanart = %d\nvideos = %d\n", k.covers, k.screenshots,
			k.titlescreens, k.marquees, k.miximages, k.boxes3d, k.backcovers, k.physicalmedia, k.fanart, k.videos);
	// A developer key of one's own, written by hand, is kept.
	if (devId != fromHex(FE_SS_DEVID_HEX))
		text += "devid = " + devId + "\ndevpassword = " + devPassword + "\n";
	writeFile(settingsFile(), text.data(), text.size());
	chmod(settingsFile().c_str(), 0600);
	std::lock_guard<std::mutex> lock(mutex);
	closedForToday = false;
}

bool usable(std::string *why)
{
	readSettings();
	std::string reason;
	if (devId.empty() || devPassword.empty())
		reason = "This build has no ScreenScraper developer key";
	else if (!current.enabled)
		reason = "ScreenScraper is off";
	else if (current.user.empty() || current.password.empty())
		reason = "No ScreenScraper account is given";
	else if (!platform::httpAvailable())
		reason = "The console is not on a network";
	if (why != nullptr)
		*why = reason;
	return reason.empty();
}

void request(const library::Game& game, bool byHand)
{
	if (game.fileTitle.empty() || !usable())
		return;
	std::lock_guard<std::mutex> lock(mutex);
	readNotFound();
	if (closedForToday || (!byHand && notFound.count(game.fileTitle) != 0))
		return;
	if (!byHand)
	{
		const auto d = done.find(game.fileTitle);
		if (d != done.end() && d->second == kindsMask())
			return;
	}
	if (!asked.insert(game.path).second && !byHand)
		return;
	queue.push_back({ game, byHand });
	pendingCount++;
	if (!started)
	{
		started = true;
		std::thread(worker).detach();
	}
	wake.notify_one();
}

void requestAll()
{
	for (int source = 0; source < library::SourceCount; source++)
		for (const library::Game& game : library::games(source))
			request(game, false);
}

void stop()
{
	if (pendingCount > 0)
		stopNow = true;
	std::lock_guard<std::mutex> lock(mutex);
	pendingCount -= (int)queue.size();
	queue.clear();
	statusText = "Stopped";
}

void checkAccount()
{
	std::string why;
	readSettings();
	if (devId.empty() || current.user.empty())
	{
		std::lock_guard<std::mutex> lock(mutex);
		accountText = devId.empty() ? "This build has no ScreenScraper developer key" : "No account is given";
		return;
	}
	std::thread([] {
		std::vector<uint8_t> answer;
		const int status = platform::httpGet(api() + "ssuserInfos.php?" + keys(), answer, 20);
		if (status == 200)
		{
			Json page;
			JsonReader((const char *)answer.data(), answer.size()).read(page);
			if (const Json *response = page.find("response"))
				readAccount(*response);
		}
		else
		{
			std::lock_guard<std::mutex> lock(mutex);
			accountText = refusal(status, answer);
		}
		std::lock_guard<std::mutex> lock(mutex);
		if (status == 200)
			closedForToday = false;
		diag::mark("scraper: the account: %s", status == 200 ? "accepted" : refusal(status, answer).c_str());
	}).detach();
	std::lock_guard<std::mutex> lock(mutex);
	accountText = "Asking ScreenScraper\xe2\x80\xa6";
}

std::string status()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (pendingCount > 0 && !statusText.empty())
		return statusText + format("  (%d left)", pendingCount.load());
	return statusText;
}

std::string accountStatus()
{
	std::lock_guard<std::mutex> lock(mutex);
	return accountText;
}

int pending()
{
	return pendingCount.load();
}

} // namespace fe::scraper
