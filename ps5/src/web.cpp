/*
	PSSwanStation - the web panel: the title from a phone or a computer on the
	same network.

	SPDX-License-Identifier: GPL-3.0-or-later

	A small HTTP server on port 3311, on a thread of its own (and one more for
	each connection, a few at a time). It serves one page, and the page asks
	it, as JSON, for what the title is doing, the library, the user's files and
	some of the settings; it can start and close a game, save and load a
	state, change those settings, and put files into the folders games, bios,
	cheats, covers, textures, music, layouts and borders (a disc from the
	computer straight into the library, without FTP).

	Everything but the page asks for the key, which the address in the QR code
	carries (Settings, Games and network): another device on the network, or a
	page in someone's browser, cannot use the panel without it. The key is
	kept in data/web.cfg and a new one can be made. network.cfg (the share's
	password) and RetroAchievements' key are never served.

	What touches the game or the menus is not done on the server's threads:
	it waits in a list that tick() works through on the main thread, which
	also writes what the status answer says.
*/
#include "web.h"
#include "fe.h"
#include "speedrun.h"
#include "ui_internal.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

namespace fe::netplay
{
std::string localAddress();
}

namespace fe::web
{
namespace
{
#if defined(MSG_NOSIGNAL)
constexpr int NoSignal = MSG_NOSIGNAL;
#else
constexpr int NoSignal = 0;
#endif

constexpr int MaxConnections = 6;

std::mutex mutex;					// what follows, between the threads
std::thread server;
std::atomic<bool> stopping{false};
std::atomic<int> connections{0};
int listener = -1;
bool serving = false;
std::string statusText = "Off";
std::string currentKey;
std::string address;
std::string statusJson = "{}";
std::vector<std::function<void()>> pending;

extern const char *const PageHtml;

// ------------------------------------------------------------------ helpers

std::string keyFile()
{
	return rootDir + "data/web.cfg";
}

std::string makeKey()
{
	// Eight letters and digits from the clock, the address space and the
	// system's random numbers where it has them.
	uint64_t seed = (uint64_t)time(nullptr) * 6364136223846793005ull ^ (uint64_t)(uintptr_t)&seed
			^ (uint64_t)(now() * 1e6);
	if (FILE *f = fopen("/dev/urandom", "rb"))
	{
		uint64_t more = 0;
		if (fread(&more, 1, sizeof(more), f) == sizeof(more))
			seed ^= more;
		fclose(f);
	}
	static const char *const letters = "abcdefghijkmnpqrstuvwxyz23456789";
	std::string key;
	for (int i = 0; i < 8; i++)
	{
		seed ^= seed >> 33;
		seed *= 0xff51afd7ed558ccdull;
		seed ^= seed >> 33;
		key += letters[seed % 32];
	}
	return key;
}

void loadKey()
{
	std::vector<uint8_t> raw;
	if (readFile(keyFile(), raw))
	{
		const std::string text = trim(std::string(raw.begin(), raw.end()));
		if (text.compare(0, 6, "key = ") == 0 && text.size() >= 10)
			currentKey = trim(text.substr(6));
	}
	if (currentKey.size() < 6)
	{
		currentKey = makeKey();
		const std::string text = "key = " + currentKey + "\n";
		makeDir(rootDir + "data");
		writeFile(keyFile(), text.data(), text.size());
	}
}

std::string jsonText(const std::string& text)
{
	std::string out = "\"";
	for (const unsigned char c : text)
	{
		if (c == '"' || c == '\\')
		{
			out += '\\';
			out += (char)c;
		}
		else if (c < 0x20)
			out += format("\\u%04x", c);
		else
			out += (char)c;
	}
	return out + "\"";
}

std::string urlDecode(const std::string& text)
{
	std::string out;
	for (size_t i = 0; i < text.size(); i++)
	{
		if (text[i] == '%' && i + 2 < text.size() && isxdigit((unsigned char)text[i + 1]) && isxdigit((unsigned char)text[i + 2]))
		{
			out += (char)strtol(text.substr(i + 1, 2).c_str(), nullptr, 16);
			i += 2;
		}
		else if (text[i] == '+')
			out += ' ';
		else
			out += text[i];
	}
	return out;
}

std::string queryValue(const std::string& query, const std::string& name)
{
	size_t start = 0;
	while (start <= query.size())
	{
		size_t end = query.find('&', start);
		if (end == std::string::npos)
			end = query.size();
		const std::string pair = query.substr(start, end - start);
		const size_t equals = pair.find('=');
		if (equals != std::string::npos && pair.substr(0, equals) == name)
			return urlDecode(pair.substr(equals + 1));
		start = end + 1;
	}
	return "";
}

// A path below the user's folder, as the page names it: no way out of it.
bool safeRelative(const std::string& path)
{
	if (path.empty())
		return true;
	if (path[0] == '/' || path.find('\\') != std::string::npos || path.find('\0') != std::string::npos)
		return false;
	size_t start = 0;
	while (start <= path.size())
	{
		size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		const std::string part = path.substr(start, end - start);
		if (part == ".." || part == ".")
			return false;
		start = end + 1;
	}
	return true;
}

// What is never served: the share's password, RetroAchievements' key, the panel's own key.
bool secret(const std::string& path)
{
	const std::string lower = lowercase(path);
	return lower == "network.cfg" || lower == "data/retroachievements.cfg" || lower == "data/web.cfg"
			|| lower.find(".tmp") == lower.size() - 4;
}

// The folders the page may put files into, and take them out of.
bool writable(const std::string& path)
{
	static const char *const folders[] = { "games/", "bios/", "cheats/", "covers/", "textures/", "music/", "layouts/",
		"borders/", "data/splits/", "data/watch/", "data/recordings/" };
	for (const char *folder : folders)
		if (path.compare(0, strlen(folder), folder) == 0 && path.size() > strlen(folder))
			return true;
	return false;
}

// ------------------------------------------------------------- answering

bool sendAll(int fd, const char *data, size_t bytes)
{
	while (bytes > 0)
	{
		const ssize_t sent = send(fd, data, bytes, NoSignal);
		if (sent <= 0)
		{
			if (sent < 0 && errno == EINTR)
				continue;
			return false;
		}
		data += sent;
		bytes -= (size_t)sent;
	}
	return true;
}

void answer(int fd, int code, const std::string& type, const std::string& body, const std::string& extra = "")
{
	const char *reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden"
			: code == 404 ? "Not Found" : code == 413 ? "Payload Too Large" : code == 503 ? "Service Unavailable" : "Error";
	const std::string head = format("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
			"Connection: close\r\n%s\r\n", code, reason, type.c_str(), body.size(), extra.c_str());
	if (sendAll(fd, head.data(), head.size()))
		sendAll(fd, body.data(), body.size());
}

void answerJson(int fd, const std::string& json)
{
	answer(fd, 200, "application/json; charset=utf-8", json);
}

void answerError(int fd, int code, const std::string& why)
{
	answer(fd, code, "application/json; charset=utf-8", "{\"error\":" + jsonText(why) + "}");
}

// Runs on the main thread at its next frame.
void later(std::function<void()> work)
{
	std::lock_guard<std::mutex> lock(mutex);
	pending.push_back(std::move(work));
}

// --------------------------------------------------------- what is asked for

// The settings the page offers: their names in frontend.cfg, what to call
// them, and what each value means.
struct Setting
{
	const char *key, *label;
	std::vector<std::string> values;	// empty: a number from low to high
	int low, high, step;
};

const std::vector<Setting>& settingsList()
{
	static const std::vector<Setting> list = {
		{ "volume", "Volume", {}, 0, 100, 5 },
		{ "scaler", "Scaling filter", { "Smooth", "Square pixels", "Sharp bilinear", "FSR 1", "NIS", "CAS" }, 0, 5, 1 },
		{ "crt", "Picture tube", { "Off", "Soft scanlines", "Scanlines", "Scanlines and mask", "Home television",
			"Studio monitor", "Arcade monitor", "Soft", "The author's own" }, 0, 8, 1 },
		{ "signal", "Video signal", { "As it is", "Dither smoothed", "S-Video", "Composite" }, 0, 3, 1 },
		{ "frame_generation", "Frame generation", { "Off", "On" }, 0, 1, 1 },
		{ "fg_quality", "Frame generation quality", { "Performance", "Balanced", "Quality" }, 0, 2, 1 },
		{ "pacing", "Frame pacing", { "By the display", "The game's speed", "By the clock" }, 0, 2, 1 },
		{ "show_fps", "Frame counter", { "Off", "On" }, 0, 1, 1 },
		{ "rewind", "Rewind", { "Off", "On" }, 0, 1, 1 },
		{ "fast_forward", "Fast forward speed", { "2x", "3x", "4x", "8x", "As fast as it goes" }, 0, 4, 1 },
		{ "hotkeys", "Shortcuts", { "Off", "On" }, 0, 1, 1 },
		{ "auto_save", "Save when a game is closed", { "Off", "On" }, 0, 1, 1 },
		{ "auto_load", "Continue where I left off", { "Off", "On" }, 0, 1, 1 },
		{ "sleep_safe", "Sleep-safe saving", { "Off", "On" }, 0, 1, 1 },
		{ "speedrun", "Speedrun timer", { "Off", "On" }, 0, 1, 1 },
		{ "watch_overlay", "Watched values over the game", { "Off", "On" }, 0, 1, 1 },
		{ "rumble", "Vibration", { "Off", "On" }, 0, 1, 1 },
		{ "music", "Menu music", { "None", "The title's own", "My file" }, 0, 2, 1 },
		{ "animations", "Animations", { "Full", "Reduced", "Off" }, 0, 2, 1 },
		{ "high_contrast", "High contrast", { "Off", "On" }, 0, 1, 1 },
		{ "colour_blind", "Colour-blind safe colours", { "Off", "On" }, 0, 1, 1 },
	};
	return list;
}

std::string settingsJson()
{
	std::string json = "[";
	bool first = true;
	for (const Setting& setting : settingsList())
	{
		int value = 0;
		if (!options::frontendValue(setting.key, value))
			continue;
		json += std::string(first ? "" : ",") + "{\"key\":" + jsonText(setting.key) + ",\"label\":" + jsonText(setting.label)
				+ format(",\"value\":%d,\"low\":%d,\"high\":%d,\"step\":%d,\"values\":[", value, setting.low, setting.high,
				setting.step);
		for (size_t i = 0; i < setting.values.size(); i++)
			json += (i == 0 ? "" : ",") + jsonText(setting.values[i]);
		json += "]}";
		first = false;
	}
	return json + "]";
}

std::string gamesJson(int source)
{
	std::string json = "[";
	bool first = true;
	if (source >= 0 && source < library::SourceCount)
		for (const library::Game& game : library::games(source))
		{
			json += std::string(first ? "" : ",") + "{\"name\":" + jsonText(game.name) + ",\"path\":" + jsonText(game.path)
					+ ",\"region\":" + jsonText(game.region) + format(",\"size\":%llu,\"discs\":%d}",
					(unsigned long long)game.size, (int)std::max<size_t>(game.discs.size(), 1));
			first = false;
		}
	return json + "]";
}

std::string filesJson(const std::string& relative)
{
	const std::string folder = rootDir + relative;
	DIR *dir = opendir(folder.c_str());
	if (dir == nullptr)
		return "";
	struct Entry
	{
		std::string name;
		bool folder;
		uint64_t size;
		int64_t time;
	};
	std::vector<Entry> entries;
	while (const dirent *entry = readdir(dir))
	{
		const std::string name = entry->d_name;
		if (name == "." || name == "..")
			continue;
		const std::string inside = relative.empty() ? name : relative + "/" + name;
		if (secret(inside))
			continue;
		struct stat st;
		if (stat((folder + "/" + name).c_str(), &st) != 0)
			continue;
		entries.push_back({ name, S_ISDIR(st.st_mode), (uint64_t)st.st_size, (int64_t)st.st_mtime });
	}
	closedir(dir);
	std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
		if (a.folder != b.folder)
			return a.folder;
		return lowercase(a.name) < lowercase(b.name);
	});
	std::string json = "{\"dir\":" + jsonText(relative) + ",\"writable\":"
			+ (writable(relative + "/x") ? "true" : "false") + ",\"entries\":[";
	for (size_t i = 0; i < entries.size(); i++)
	{
		const std::string inside = relative.empty() ? entries[i].name : relative + "/" + entries[i].name;
		json += (i == 0 ? "" : ",") + std::string("{\"name\":") + jsonText(entries[i].name) + ",\"folder\":"
				+ (entries[i].folder ? "true" : "false") + format(",\"size\":%llu,\"time\":%lld",
				(unsigned long long)entries[i].size, (long long)entries[i].time) + ",\"removable\":"
				+ (!entries[i].folder && writable(inside) ? "true" : "false") + "}";
	}
	return json + "]}";
}

// A file to the page, in pieces.
void sendFile(int fd, const std::string& relative)
{
	const std::string path = rootDir + relative;
	FILE *f = fopen(path.c_str(), "rb");
	struct stat st;
	if (f == nullptr || stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
	{
		if (f != nullptr)
			fclose(f);
		answerError(fd, 404, "No such file.");
		return;
	}
	std::string name = baseName(relative);
	for (char& c : name)
		if (c == '"' || c < 0x20)
			c = '_';
	const std::string head = format("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lld\r\n"
			"Content-Disposition: attachment; filename=\"%s\"\r\nConnection: close\r\n\r\n", (long long)st.st_size, name.c_str());
	bool ok = sendAll(fd, head.data(), head.size());
	std::vector<char> block(256 * 1024);
	size_t got;
	while (ok && !stopping && (got = fread(block.data(), 1, block.size(), f)) > 0)
		ok = sendAll(fd, block.data(), got);
	fclose(f);
}

// A file from the page: `length` bytes after what was read with the headers.
void receiveFile(int fd, const std::string& relative, uint64_t length, const std::string& already)
{
	if (!writable(relative) || secret(relative))
	{
		answerError(fd, 403, "Files can be put into games, bios, cheats, covers, textures, music, layouts and borders only.");
		return;
	}
	const std::string path = rootDir + relative;
	const size_t slash = path.rfind('/');
	if (slash != std::string::npos)
		makeDir(path.substr(0, slash));
	const std::string temporary = path + ".upload.tmp";
	FILE *f = fopen(temporary.c_str(), "wb");
	if (f == nullptr)
	{
		answerError(fd, 500, "The file could not be written there.");
		return;
	}
	uint64_t left = length;
	bool ok = true;
	if (!already.empty())
	{
		const size_t use = (size_t)std::min<uint64_t>(already.size(), left);
		ok = fwrite(already.data(), 1, use, f) == use;
		left -= use;
	}
	std::vector<char> block(256 * 1024);
	while (ok && left > 0 && !stopping)
	{
		const ssize_t got = recv(fd, block.data(), (size_t)std::min<uint64_t>(block.size(), left), 0);
		if (got <= 0)
		{
			if (got < 0 && errno == EINTR)
				continue;
			ok = false;
			break;
		}
		ok = fwrite(block.data(), 1, (size_t)got, f) == (size_t)got;
		left -= (uint64_t)got;
	}
	const bool closed = fclose(f) == 0;
	if (!ok || !closed || left != 0 || rename(temporary.c_str(), path.c_str()) != 0)
	{
		unlink(temporary.c_str());
		answerError(fd, 500, left != 0 ? "The file did not arrive whole." : "The file could not be written (the disk may be full).");
		return;
	}
	chmod(path.c_str(), 0666);
	diag::mark("web: received %s (%llu bytes)", relative.c_str(), (unsigned long long)length);
	if (relative.compare(0, 6, "games/") == 0)
		later([] { library::scan(library::Internal, true); });
	answerJson(fd, "{\"ok\":true}");
}

// One connection: one request, one answer.
void handle(int fd)
{
	const timeval limit = { 30, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
#if defined(SO_NOSIGPIPE)
	const int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
	// The request line and the headers.
	std::string data;
	size_t headerEnd = std::string::npos;
	char block[4096];
	while (headerEnd == std::string::npos && data.size() < 16384 && !stopping)
	{
		const ssize_t got = recv(fd, block, sizeof(block), 0);
		if (got <= 0)
		{
			if (got < 0 && errno == EINTR)
				continue;
			return;
		}
		data.append(block, (size_t)got);
		headerEnd = data.find("\r\n\r\n");
	}
	if (headerEnd == std::string::npos)
	{
		answerError(fd, 400, "The request was not understood.");
		return;
	}
	const std::string head = data.substr(0, headerEnd);
	std::string rest = data.substr(headerEnd + 4);
	const size_t lineEnd = head.find("\r\n");
	const std::string requestLine = head.substr(0, lineEnd);
	char methodText[16] = {}, targetText[4096] = {};
	if (sscanf(requestLine.c_str(), "%15s %4095s", methodText, targetText) != 2)
	{
		answerError(fd, 400, "The request was not understood.");
		return;
	}
	const std::string method = methodText, target = targetText;
	uint64_t length = 0;
	std::string keyHeader;
	size_t at = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
	while (at < head.size())
	{
		size_t end = head.find("\r\n", at);
		if (end == std::string::npos)
			end = head.size();
		const std::string line = head.substr(at, end - at);
		at = end + 2;
		const size_t colon = line.find(':');
		if (colon == std::string::npos)
			continue;
		const std::string name = lowercase(trim(line.substr(0, colon))), value = trim(line.substr(colon + 1));
		if (name == "content-length")
			length = strtoull(value.c_str(), nullptr, 10);
		else if (name == "x-key")
			keyHeader = value;
	}
	const size_t question = target.find('?');
	const std::string path = urlDecode(target.substr(0, question));
	const std::string query = question == std::string::npos ? "" : target.substr(question + 1);
	std::string key;
	{
		std::lock_guard<std::mutex> lock(mutex);
		key = currentKey;
	}
	const bool keyed = (!keyHeader.empty() && keyHeader == key) || queryValue(query, "key") == key;

	if (method == "GET" && (path == "/" || path == "/index.html"))
	{
		if (!keyed)
			answer(fd, 403, "text/html; charset=utf-8", "<!doctype html><meta charset=utf-8><meta name=viewport "
					"content='width=device-width'><title>PSSwanStation</title><body style='font-family:sans-serif;"
					"background:#0e121e;color:#eef2fa;padding:24px'><h2>PSSwanStation</h2><p>Open this page from the QR "
					"code PSSwanStation shows (Settings, Games and network, Phone and web control): its address "
					"carries the key.</p>");
		else
			answer(fd, 200, "text/html; charset=utf-8", PageHtml);
		return;
	}
	if (!keyed)
	{
		answerError(fd, 403, "The key is missing or old: scan the QR code again.");
		return;
	}
	// Reading a body that is not a file: small, all of it.
	const auto body = [&]() {
		std::string text = rest;
		while (text.size() < length && text.size() < 65536 && !stopping)
		{
			const ssize_t got = recv(fd, block, std::min(sizeof(block), (size_t)(length - text.size())), 0);
			if (got <= 0)
				break;
			text.append(block, (size_t)got);
		}
		return text.substr(0, (size_t)std::min<uint64_t>(length, text.size()));
	};
	if (method == "GET" && path == "/api/status")
	{
		std::lock_guard<std::mutex> lock(mutex);
		answerJson(fd, statusJson);
	}
	else if (method == "GET" && path == "/api/games")
		answerJson(fd, gamesJson(atoi(queryValue(query, "source").c_str())));
	else if (method == "GET" && path == "/api/settings")
		answerJson(fd, settingsJson());
	else if (method == "POST" && path == "/api/settings")
	{
		const std::string name = queryValue(query, "key");
		const int value = atoi(queryValue(query, "value").c_str());
		bool known = false;
		for (const Setting& setting : settingsList())
			known = known || name == setting.key;
		if (!known)
			answerError(fd, 400, "That setting is not offered here.");
		else
		{
			later([name, value] {
				options::setFrontendValue(name, value);
				if (name == "volume")
					audio::setVolume(options::frontend().volume);
			});
			answerJson(fd, "{\"ok\":true}");
		}
	}
	else if (method == "POST" && path == "/api/start")
	{
		const std::string game = body();
		later([game] {
			for (int source = 0; source < library::SourceCount; source++)
				for (const library::Game& entry : library::games(source))
					if (entry.path == game)
					{
						ui::stack.clear();
						ui::launch(entry, -1);
						return;
					}
		});
		answerJson(fd, "{\"ok\":true}");
	}
	else if (method == "POST" && path == "/api/close")
	{
		later([] {
			if (host::running())
				ui::closeGame = true;
		});
		answerJson(fd, "{\"ok\":true}");
	}
	else if (method == "POST" && path == "/api/reset")
	{
		later([] { host::reset(); });
		answerJson(fd, "{\"ok\":true}");
	}
	else if (method == "POST" && (path == "/api/save" || path == "/api/load"))
	{
		const int slot = std::clamp(atoi(queryValue(query, "slot").c_str()), 0, host::StateSlots - 1);
		const bool save = path == "/api/save";
		later([slot, save] {
			if (save)
				host::saveState(slot);
			else
				host::loadState(slot);
		});
		answerJson(fd, "{\"ok\":true}");
	}
	else if (method == "POST" && path == "/api/speedrun")
	{
		const std::string what = queryValue(query, "do");
		later([what] {
			if (what == "split")
				speedrun::startOrSplit();
			else if (what == "undo")
				speedrun::undo();
			else if (what == "reset")
				speedrun::reset();
		});
		answerJson(fd, "{\"ok\":true}");
	}
	else if (method == "GET" && path == "/api/files")
	{
		std::string dir = queryValue(query, "dir");
		while (!dir.empty() && dir.back() == '/')
			dir.pop_back();
		const std::string json = safeRelative(dir) ? filesJson(dir) : "";
		if (json.empty())
			answerError(fd, 404, "No such folder.");
		else
			answerJson(fd, json);
	}
	else if (method == "GET" && path == "/download")
	{
		const std::string file = queryValue(query, "path");
		if (!safeRelative(file) || file.empty() || secret(file))
			answerError(fd, 403, "That file is not given out.");
		else
			sendFile(fd, file);
	}
	else if (method == "PUT" && path == "/upload")
	{
		const std::string file = queryValue(query, "path");
		if (!safeRelative(file) || file.empty())
			answerError(fd, 403, "Not there.");
		else if (length > (64ull << 30))
			answerError(fd, 413, "That is larger than any disc.");
		else
			receiveFile(fd, file, length, rest);
	}
	else if (method == "POST" && path == "/api/delete")
	{
		const std::string file = queryValue(query, "path");
		if (!safeRelative(file) || !writable(file) || secret(file))
			answerError(fd, 403, "Files can be removed from the folders files can be put into only.");
		else if (unlink((rootDir + file).c_str()) != 0)
			answerError(fd, 404, "It could not be removed.");
		else
		{
			diag::mark("web: removed %s", file.c_str());
			if (file.compare(0, 6, "games/") == 0)
				later([] { library::scan(library::Internal, true); });
			answerJson(fd, "{\"ok\":true}");
		}
	}
	else
		answerError(fd, 404, "Nothing is here.");
}

void serve()
{
	while (!stopping)
	{
		pollfd wait = { listener, POLLIN, 0 };
		const int ready = poll(&wait, 1, 400);
		if (ready <= 0 || stopping)
			continue;
		sockaddr_in from{};
		socklen_t size = sizeof(from);
		const int fd = accept(listener, reinterpret_cast<sockaddr *>(&from), &size);
		if (fd < 0)
			continue;
		if (connections >= MaxConnections)
		{
			answerError(fd, 503, "Busy: try again in a moment.");
			close(fd);
			continue;
		}
		connections++;
		std::thread([fd] {
			handle(fd);
			::shutdown(fd, SHUT_RDWR);
			close(fd);
			connections--;
		}).detach();
	}
}

void start()
{
	loadKey();
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in bindTo{};
	bindTo.sin_family = AF_INET;
	bindTo.sin_port = htons((uint16_t)Port);
	bindTo.sin_addr.s_addr = htonl(INADDR_ANY);
	if (fd >= 0)
	{
		const int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	}
	if (fd < 0 || bind(fd, reinterpret_cast<const sockaddr *>(&bindTo), sizeof(bindTo)) != 0 || listen(fd, 8) != 0)
	{
		const int error = errno;
		if (fd >= 0)
			close(fd);
		statusText = format("Port %d could not be opened (error %d)", Port, error);
		diag::mark("web: %s", statusText.c_str());
		return;
	}
	listener = fd;
	stopping = false;
	address = netplay::localAddress();
	serving = true;
	statusText = address.empty() ? format("Listening on port %d (this console's address is not known)", Port)
			: format("Listening on %s:%d", address.c_str(), Port);
	diag::mark("web: %s", statusText.c_str());
	server = std::thread(serve);
}

void stop()
{
	if (!serving)
		return;
	stopping = true;
	if (server.joinable())
		server.join();
	close(listener);
	listener = -1;
	// The connections end when they next read or write; a moment for them.
	for (int i = 0; i < 40 && connections > 0; i++)
		usleep(50000);
	serving = false;
	statusText = "Off";
	diag::mark("web: stopped");
}
}

void apply()
{
	const bool wanted = options::frontend().web;
	if (wanted && !serving)
		start();
	else if (!wanted && serving)
		stop();
}

void tick()
{
	apply();
	if (!serving)
		return;
	std::vector<std::function<void()>> work;
	{
		std::lock_guard<std::mutex> lock(mutex);
		work.swap(pending);
	}
	for (std::function<void()>& job : work)
		job();
	// What the status answer says, four times a second.
	static double madeAt;
	if (now() - madeAt < 0.25)
		return;
	madeAt = now();
	std::string json = format("{\"build\":%d,\"running\":%s", BuildNumber, host::running() ? "true" : "false");
	if (host::running())
	{
		const host::GameInfo& game = host::game();
		json += ",\"game\":{\"title\":" + jsonText(game.title) + ",\"serial\":" + jsonText(game.serial) + ",\"path\":"
				+ jsonText(game.path) + "}" + format(",\"fps\":%.1f", host::measuredFps());
		std::string slots = "[";
		for (int slot = 0; slot < host::StateSlots; slot++)
		{
			std::string when;
			slots += (slot == 0 ? "" : ",") + (host::stateExists(slot, &when) ? jsonText(when) : std::string("null"));
		}
		json += ",\"slots\":" + slots + "]";
		if (options::frontend().speedrun)
			json += ",\"speedrun\":{\"state\":" + std::to_string((int)speedrun::state()) + ",\"time\":"
					+ jsonText(speedrun::timeText(speedrun::elapsed())) + "}";
	}
	json += "}";
	std::lock_guard<std::mutex> lock(mutex);
	statusJson = json;
}

void shutdown()
{
	stop();
}

bool listening()
{
	return serving;
}

std::string status()
{
	return statusText;
}

std::string url()
{
	if (address.empty())
		address = netplay::localAddress();
	if (address.empty())
		return "";
	if (currentKey.empty())
		loadKey();
	return format("http://%s:%d/?key=%s", address.c_str(), Port, currentKey.c_str());
}

const std::string& key()
{
	if (currentKey.empty())
		loadKey();
	return currentKey;
}

void newKey()
{
	std::lock_guard<std::mutex> lock(mutex);
	currentKey = makeKey();
	const std::string text = "key = " + currentKey + "\n";
	makeDir(rootDir + "data");
	writeFile(keyFile(), text.data(), text.size());
	diag::mark("web: a new key");
}

namespace
{
const char *const PageHtml = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PSSwanStation</title>
<style>
:root{--bg:#0e121e;--panel:#1a2032;--high:#2c3652;--text:#eef2fa;--dim:#a0aac0;--accent:#4f9dff;--bad:#f87171}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px/1.4 system-ui,sans-serif}
header{padding:14px 16px;background:var(--panel);display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap}
h1{font-size:20px;margin:0}#status{color:var(--dim);font-size:14px}
nav{display:flex;gap:4px;padding:8px 16px;overflow-x:auto}nav button{flex:1}
main{padding:8px 16px 40px;max-width:900px;margin:0 auto}
button,select,input{font:inherit;color:var(--text);background:var(--high);border:0;border-radius:10px;padding:10px 14px;min-height:44px}
button:hover{filter:brightness(1.2)}button.on{background:var(--accent);color:#06101f}button.warn{background:#5a2a2a}
.card{background:var(--panel);border-radius:14px;padding:14px;margin:10px 0}
.row{display:flex;align-items:center;justify-content:space-between;gap:10px;padding:8px 0;border-top:1px solid #ffffff12}
.row:first-child{border-top:0}.grow{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis}
.dim{color:var(--dim);font-size:14px}.tools{display:flex;gap:8px;flex-wrap:wrap}
input[type=search]{width:100%}progress{width:100%;height:12px}a{color:var(--accent)}
</style></head><body>
<header><h1>PSSwanStation</h1><span id="status">Connecting…</span></header>
<nav><button data-tab="play" class="on">Playing</button><button data-tab="library">Library</button>
<button data-tab="files">Files</button><button data-tab="settings">Settings</button></nav>
<main>
<section id="play"></section>
<section id="library" hidden><div class="card"><div class="tools">
<select id="source"><option value="0">This console</option><option value="1">USB drives</option><option value="2">Network share</option></select>
<input id="filter" type="search" placeholder="Look for a game"></div></div><div id="games"></div></section>
<section id="files" hidden></section>
<section id="settings" hidden></section>
</main>
<script>
const key=new URLSearchParams(location.search).get('key')||'';
const api=(path,opt={})=>fetch(path,Object.assign({headers:{'X-Key':key}},opt)).then(r=>r.ok?r.json():r.json().then(e=>{throw new Error(e.error||r.status)}));
const post=(path,body)=>api(path,{method:'POST',body:body||''});
const esc=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const size=n=>n>1e9?(n/1e9).toFixed(2)+' GB':n>1e6?(n/1e6).toFixed(1)+' MB':n>1e3?(n/1e3).toFixed(0)+' KB':n+' B';
let tab='play',status={},games=[],dir='';
document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{tab=b.dataset.tab;
document.querySelectorAll('nav button').forEach(x=>x.classList.toggle('on',x===b));
document.querySelectorAll('main section').forEach(s=>s.hidden=s.id!==tab);
if(tab==='library')loadGames();if(tab==='files')loadFiles(dir);if(tab==='settings')loadSettings();});
function drawPlay(){const e=document.getElementById('play');
if(!status.running){e.innerHTML='<div class="card">No game is running. Start one from the Library.</div>';return;}
let slots='';for(let i=0;i<10;i++)slots+=`<option value="${i}">Slot ${i+1}${status.slots&&status.slots[i]?' · '+esc(status.slots[i]):' · empty'}</option>`;
const sr=status.speedrun?`<div class="card"><div class="row"><span class="grow">Speedrun timer</span><b>${esc(status.speedrun.time)}</b></div>
<div class="tools"><button onclick="post('/api/speedrun?do=split')">Start / split</button><button onclick="post('/api/speedrun?do=undo')">Undo split</button>
<button class="warn" onclick="post('/api/speedrun?do=reset')">Reset</button></div></div>`:'';
e.innerHTML=`<div class="card"><div class="row"><div class="grow"><b>${esc(status.game.title)}</b><div class="dim">${esc(status.game.serial)} · ${status.fps} fps</div></div></div>
<div class="tools"><select id="slot">${slots}</select><button onclick="post('/api/save?slot='+slot.value)">Save state</button>
<button onclick="post('/api/load?slot='+slot.value)">Load state</button></div>
<div class="tools" style="margin-top:8px"><button onclick="post('/api/reset')">Reset</button><button class="warn" onclick="if(confirm('Close the game?'))post('/api/close')">Close the game</button></div></div>${sr}`;}
function poll(){api('/api/status').then(s=>{const was=JSON.stringify(status);status=s;
document.getElementById('status').textContent='Build '+s.build+(s.running?' · '+s.game.title:' · in the library');
if(tab==='play'&&(was!==JSON.stringify(s))&&!document.activeElement.matches('select'))drawPlay();}).catch(e=>document.getElementById('status').textContent=e.message);}
function loadGames(){api('/api/games?source='+source.value).then(g=>{games=g;drawGames();}).catch(e=>alert(e.message));}
function drawGames(){const f=filter.value.toLowerCase();const list=games.filter(g=>g.name.toLowerCase().includes(f));
document.getElementById('games').innerHTML='<div class="card">'+(list.length?list.map((g,i)=>`<div class="row"><div class="grow">${esc(g.name)}<div class="dim">${esc(g.region)}${g.discs>1?' · '+g.discs+' discs':''} · ${size(g.size)}</div></div><button data-i="${games.indexOf(g)}">Start</button></div>`).join(''):'No games here.')+'</div>';
document.querySelectorAll('#games button').forEach(b=>b.onclick=()=>post('/api/start',games[b.dataset.i].path).then(()=>{document.querySelector('[data-tab=play]').click();}));}
source.onchange=loadGames;filter.oninput=drawGames;
function loadFiles(d){api('/api/files?dir='+encodeURIComponent(d)).then(r=>{dir=r.dir;const e=document.getElementById('files');
const up=dir.includes('/')?dir.slice(0,dir.lastIndexOf('/')):'';
let h=`<div class="card"><div class="row"><b class="grow">/${esc(dir)}</b>${dir?`<button data-go="${esc(up)}">Up</button>`:''}</div>`;
if(r.writable)h+=`<div class="tools" style="margin:8px 0"><input type="file" id="pick" multiple><button id="send">Put here</button></div><progress id="bar" value="0" max="1" hidden></progress><div id="note" class="dim"></div>`;
else if(!dir)h+=`<div class="dim">Files can be put into games, bios, cheats, covers, textures, music, layouts and borders.</div>`;
h+=r.entries.map(x=>{const p=(dir?dir+'/':'')+x.name;return x.folder?`<div class="row"><span class="grow">📁 ${esc(x.name)}</span><button data-go="${esc(p)}">Open</button></div>`
:`<div class="row"><div class="grow">${esc(x.name)}<div class="dim">${size(x.size)}</div></div><a href="/download?key=${key}&path=${encodeURIComponent(p)}">Get</a>${x.removable?`<button class="warn" data-rm="${esc(p)}">Remove</button>`:''}</div>`}).join('')+'</div>';
e.innerHTML=h;e.querySelectorAll('[data-go]').forEach(b=>b.onclick=()=>loadFiles(b.dataset.go));
e.querySelectorAll('[data-rm]').forEach(b=>b.onclick=()=>{if(confirm('Remove '+b.dataset.rm+'?'))post('/api/delete?path='+encodeURIComponent(b.dataset.rm)).then(()=>loadFiles(dir)).catch(x=>alert(x.message));});
const send=document.getElementById('send');if(send)send.onclick=()=>upload([...document.getElementById('pick').files]);}).catch(e=>alert(e.message));}
function upload(list){if(!list.length)return;const f=list.shift();const bar=document.getElementById('bar'),note=document.getElementById('note');bar.hidden=false;
const x=new XMLHttpRequest();x.open('PUT','/upload?path='+encodeURIComponent((dir?dir+'/':'')+f.name));x.setRequestHeader('X-Key',key);
x.upload.onprogress=e=>{bar.value=e.loaded/e.total;note.textContent=f.name+': '+size(e.loaded)+' of '+size(e.total);};
x.onload=()=>{note.textContent=x.status===200?f.name+' arrived.':f.name+': '+x.responseText;if(list.length)upload(list);else loadFiles(dir);};
x.onerror=()=>{note.textContent=f.name+' did not arrive.';};x.send(f);}
function loadSettings(){api('/api/settings').then(list=>{document.getElementById('settings').innerHTML='<div class="card">'+list.map(s=>{
let c;if(s.values.length){c=`<select data-k="${s.key}">`+s.values.map((v,i)=>`<option value="${s.low+i}"${s.low+i===s.value?' selected':''}>${esc(v)}</option>`).join('')+'</select>';}
else c=`<input type="range" data-k="${s.key}" min="${s.low}" max="${s.high}" step="${s.step}" value="${s.value}">`;
return `<div class="row"><span class="grow">${esc(s.label)}</span>${c}</div>`;}).join('')+'</div>';
document.querySelectorAll('#settings [data-k]').forEach(i=>i.onchange=()=>post('/api/settings?key='+i.dataset.k+'&value='+i.value));}).catch(e=>alert(e.message));}
poll();setInterval(poll,1500);
</script></body></html>
)HTML";
}

}
