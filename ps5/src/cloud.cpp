/*
	PSSwanStation - cloud saves: the memory cards (and, if asked, the save
	states) kept in the user's Google Drive or Dropbox, as they are kept on a
	network share (netfiles.cpp does the keeping the same; this is the storage).

	SPDX-FileCopyrightText: 2026 Press5elect
	SPDX-License-Identifier: GPL-3.0-or-later

	Signing in, without typing a password on the console:

	  Google Drive  OAuth 2.0 for TVs and limited-input devices: the console
	                asks Google for a code, shows it with the address to type
	                it at (google.com/device, a QR code for the phone), and
	                asks every few seconds until it has been allowed there.
	                The scope is drive.appdata: a folder of the app's own,
	                hidden in Drive, and nothing else of the user's.
	  Dropbox       OAuth 2.0 with PKCE and no redirect: a QR code opens
	                Dropbox's page on the phone, which, once allowed, shows a
	                code; it is typed on the console (or pasted through the
	                phone and web panel). The app has an "App folder" of its
	                own (Apps/PSSwanStation), nothing else.

	What is kept on the console is the refresh token, in data/cloud.cfg (never
	served by the web panel); an access token is asked for with it when needed.

	The app's own registration with Google (a client ID and secret of the
	"TVs and limited input devices" kind) and Dropbox (an app key) is built in
	from the environment (PSS_GOOGLE_CLIENT_ID, PSS_GOOGLE_CLIENT_SECRET,
	PSS_DROPBOX_APP_KEY, ps5/CMakeLists.txt), or read from <root>cloud-keys.cfg
	(google_client_id = ..., google_client_secret = ..., dropbox_app_key = ...),
	which wins. A build with neither says so and offers nothing. For an
	installed app neither is secret, as Google's documentation says.

	Everything here runs on the caller's thread except the sign-in, which has
	one of its own; the storage is used by netfiles.cpp's thread.
*/
#include "cloud.h"
#include "fe.h"
#include "json.h"
#include "tls_client.h"

#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <thread>

extern "C" int mbedtls_hardware_poll(void *, unsigned char *output, size_t length, size_t *written);

#ifndef PSS_GOOGLE_CLIENT_ID
#define PSS_GOOGLE_CLIENT_ID ""
#endif
#ifndef PSS_GOOGLE_CLIENT_SECRET
#define PSS_GOOGLE_CLIENT_SECRET ""
#endif
#ifndef PSS_DROPBOX_APP_KEY
#define PSS_DROPBOX_APP_KEY ""
#endif

namespace fe::cloud
{
namespace
{
using Headers = std::vector<std::pair<std::string, std::string>>;
constexpr unsigned Seconds = 30;
constexpr size_t Largest = 64u << 20;		// a save state is a few megabytes

// Where Google and Dropbox are. The renderer tests point them at a local
// server (SWANSTATION_CLOUD_TEST: "http://127.0.0.1:port").
std::string base(const char *real)
{
#if defined(SWANSTATION_HOST)
	if (const char *test = getenv("SWANSTATION_CLOUD_TEST"))
		return test;
#endif
	return real;
}
std::string googleAuth() { return base("https://oauth2.googleapis.com"); }
std::string googleApi() { return base("https://www.googleapis.com"); }
std::string dropboxApi() { return base("https://api.dropboxapi.com"); }
std::string dropboxContent() { return base("https://content.dropboxapi.com"); }

struct Keys
{
	std::string googleId, googleSecret, dropboxKey;
};

std::string trimmed(const std::string& text)
{
	size_t a = 0, b = text.size();
	while (a < b && isspace((unsigned char)text[a]))
		a++;
	while (b > a && isspace((unsigned char)text[b - 1]))
		b--;
	return text.substr(a, b - a);
}

// "name = value" lines.
std::map<std::string, std::string> readLines(const std::string& path)
{
	std::map<std::string, std::string> out;
	std::vector<uint8_t> bytes;
	if (!readFile(path, bytes))
		return out;
	const std::string text(bytes.begin(), bytes.end());
	size_t at = 0;
	while (at < text.size())
	{
		const size_t end = std::min(text.find('\n', at), text.size());
		const std::string line = text.substr(at, end - at);
		at = end + 1;
		const size_t equals = line.find('=');
		if (equals == std::string::npos || trimmed(line).empty() || trimmed(line)[0] == '#')
			continue;
		out[trimmed(line.substr(0, equals))] = trimmed(line.substr(equals + 1));
	}
	return out;
}

Keys keys()
{
	Keys k{ PSS_GOOGLE_CLIENT_ID, PSS_GOOGLE_CLIENT_SECRET, PSS_DROPBOX_APP_KEY };
	const auto own = readLines(rootDir + "cloud-keys.cfg");
	if (own.count("google_client_id"))
		k.googleId = own.at("google_client_id");
	if (own.count("google_client_secret"))
		k.googleSecret = own.at("google_client_secret");
	if (own.count("dropbox_app_key"))
		k.dropboxKey = own.at("dropbox_app_key");
	return k;
}

std::string urlEncode(const std::string& text)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : text)
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += (char)c;
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	return out;
}

std::string form(const std::vector<std::pair<std::string, std::string>>& fields)
{
	std::string out;
	for (const auto& [name, value] : fields)
		out += (out.empty() ? "" : "&") + urlEncode(name) + "=" + urlEncode(value);
	return out;
}

// A JSON string; with `ascii`, everything past ASCII as \u escapes (an HTTP
// header, which Dropbox's arguments are, takes ASCII only).
std::string jsonString(const std::string& text, bool ascii = false)
{
	std::string out = "\"";
	for (size_t i = 0; i < text.size(); i++)
	{
		const unsigned char c = (unsigned char)text[i];
		if (c == '"' || c == '\\')
		{
			out += '\\';
			out += (char)c;
		}
		else if (c < 0x20)
			out += format("\\u%04x", c);
		else if (c >= 0x80 && ascii)
		{
			// UTF-8 to UTF-16.
			uint32_t code = 0;
			int extra = 0;
			if ((c & 0xE0) == 0xC0)
				code = c & 0x1F, extra = 1;
			else if ((c & 0xF0) == 0xE0)
				code = c & 0x0F, extra = 2;
			else
				code = c & 0x07, extra = 3;
			for (int k = 0; k < extra && i + 1 < text.size(); k++)
				code = (code << 6) | ((unsigned char)text[++i] & 0x3F);
			if (code >= 0x10000)
			{
				code -= 0x10000;
				out += format("\\u%04x\\u%04x", 0xD800 + (code >> 10), 0xDC00 + (code & 0x3FF));
			}
			else
				out += format("\\u%04x", code);
		}
		else
			out += (char)c;
	}
	return out + "\"";
}

bool parse(const std::vector<uint8_t>& bytes, Json& out)
{
	if (bytes.empty())
		return false;
	JsonReader reader((const char *)bytes.data(), bytes.size());
	return reader.read(out) && out.type == Json::Object;
}

// ------------------------------------------------------------ the account

std::mutex mutex;
Service signedService = None;
std::string refreshToken, accountName;
std::string accessToken;
double accessUntil = 0;
bool loaded = false;

std::string accountFile()
{
	return rootDir + "data/cloud.cfg";
}

void load()
{
	if (loaded)
		return;
	loaded = true;
	const auto values = readLines(accountFile());
	if (values.count("service") && values.count("refresh"))
	{
		const int service = atoi(values.at("service").c_str());
		if (service == GoogleDrive || service == Dropbox)
		{
			signedService = (Service)service;
			refreshToken = values.at("refresh");
			accountName = values.count("account") ? values.at("account") : "";
		}
	}
}

void save()
{
	const std::string text = signedService == None ? std::string()
			: format("# PSSwanStation's cloud saves: the account signed in to. Keep this file to yourself.\n"
					"service = %d\nrefresh = %s\naccount = %s\n", (int)signedService, refreshToken.c_str(),
					accountName.c_str());
	if (text.empty())
		::remove(accountFile().c_str());
	else
	{
		writeFile(accountFile(), text.data(), text.size());
		chmod(accountFile().c_str(), 0600);
	}
}

// A new access token from the refresh token. With the mutex held.
bool refreshAccess(std::string& why)
{
	const Keys k = keys();
	std::vector<uint8_t> answer;
	int status;
	if (signedService == GoogleDrive)
		status = tls::send("POST", googleAuth() + "/token", { { "Content-Type", "application/x-www-form-urlencoded" } },
				form({ { "client_id", k.googleId }, { "client_secret", k.googleSecret }, { "refresh_token", refreshToken },
					{ "grant_type", "refresh_token" } }),
				answer, Seconds, 1u << 20, &why);
	else
		status = tls::send("POST", dropboxApi() + "/oauth2/token", { { "Content-Type", "application/x-www-form-urlencoded" } },
				form({ { "client_id", k.dropboxKey }, { "refresh_token", refreshToken }, { "grant_type", "refresh_token" } }),
				answer, Seconds, 1u << 20, &why);
	Json json;
	if (status == 200 && parse(answer, json) && !json.string("access_token").empty())
	{
		accessToken = json.string("access_token");
		const Json *expires = json.find("expires_in");
		accessUntil = now() + (expires != nullptr && expires->type == Json::Number ? expires->number : 3600.0) - 60.0;
		return true;
	}
	if (status >= 400 && status < 500 && parse(answer, json))
	{
		const std::string error = json.string("error");
		why = error == "invalid_grant" ? "the sign-in was withdrawn or has expired: sign in again"
				: "the service refused (" + (error.empty() ? std::to_string(status) : error) + ")";
	}
	else if (why.empty())
		why = format("the service answered %d", status);
	return false;
}

bool access(std::string& token, std::string& why)
{
	std::lock_guard<std::mutex> lock(mutex);
	load();
	if (signedService == None)
	{
		why = "not signed in";
		return false;
	}
	if (accessToken.empty() || now() > accessUntil)
		if (!refreshAccess(why))
			return false;
	token = accessToken;
	return true;
}

void dropAccess()
{
	std::lock_guard<std::mutex> lock(mutex);
	accessToken.clear();
}

// One authorised request: asked again once with a new token after a 401.
int call(const std::string& method, const std::string& url, Headers headers, const std::string& body,
		std::vector<uint8_t>& out, std::string& why)
{
	for (int attempt = 0; attempt < 2; attempt++)
	{
		std::string token;
		if (!access(token, why))
			return -1;
		Headers all = headers;
		all.push_back({ "Authorization", "Bearer " + token });
		const int status = tls::send(method, url, all, body, out, Seconds, Largest, &why);
		if (status == 401 && attempt == 0)
		{
			dropAccess();
			continue;
		}
		if (status < 0)
			return status;
		if (status >= 400 && why.empty())
		{
			Json json;
			std::string detail;
			if (parse(out, json))
			{
				detail = json.string("error_summary");
				if (detail.empty())
					if (const Json *error = json.find("error"))
						detail = error->type == Json::String ? error->text : error->string("message");
			}
			why = format("the service answered %d", status) + (detail.empty() ? "" : " (" + detail + ")");
		}
		return status;
	}
	return -1;
}

// ----------------------------------------------------------- Google Drive

// The app's folder in Drive (appDataFolder) has no folders here: a file's
// name is its whole path, "memory cards/Game_1.mcd".
class DriveStore : public Store
{
public:
	bool walk(const std::string& top, bool, bool, std::map<std::string, uint64_t>& out) override
	{
		if (!listAll())
			return false;
		const std::string prefix = top + "/";
		for (const auto& [name, file] : files)
			if (name.compare(0, prefix.size(), prefix) == 0)
				out[name.substr(prefix.size())] = file.size;
		return true;
	}

	bool read(const std::string& path, std::vector<uint8_t>& out) override
	{
		const auto it = files.find(path);
		if (it == files.end())
		{
			why = path + " is not in Drive";
			return false;
		}
		return call("GET", googleApi() + "/drive/v3/files/" + it->second.id + "?alt=media", {}, "", out, why) == 200;
	}

	bool write(const std::string& path, const void *data, size_t size, std::string& error) override
	{
		const std::string bytes((const char *)data, size);
		std::vector<uint8_t> answer;
		int status;
		const auto it = files.find(path);
		if (it != files.end())
			status = call("PATCH", googleApi() + "/upload/drive/v3/files/" + it->second.id + "?uploadType=media&fields=id",
					{ { "Content-Type", "application/octet-stream" } }, bytes, answer, why);
		else
		{
			const std::string boundary = "psswanstation-part-boundary-7f3a9c";
			const std::string body = "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n{\"name\":"
					+ jsonString(path) + ",\"parents\":[\"appDataFolder\"]}\r\n--" + boundary
					+ "\r\nContent-Type: application/octet-stream\r\n\r\n" + bytes + "\r\n--" + boundary + "--\r\n";
			status = call("POST", googleApi() + "/upload/drive/v3/files?uploadType=multipart&fields=id",
					{ { "Content-Type", "multipart/related; boundary=" + boundary } }, body, answer, why);
		}
		Json json;
		if (status == 200 && parse(answer, json) && !json.string("id").empty())
		{
			files[path] = { json.string("id"), (uint64_t)size };
			return true;
		}
		error = why;
		return false;
	}

	bool remove(const std::string& path) override
	{
		const auto it = files.find(path);
		if (it == files.end())
			return true;
		std::vector<uint8_t> answer;
		const int status = call("DELETE", googleApi() + "/drive/v3/files/" + it->second.id, {}, "", answer, why);
		if (status == 204 || status == 200 || status == 404)
		{
			files.erase(it);
			return true;
		}
		return false;
	}

	std::string error() override { return why; }
	std::string name() override { return "Google Drive"; }

private:
	struct File
	{
		std::string id;
		uint64_t size = 0;
	};

	// Every file of the app's folder, once a run.
	bool listAll()
	{
		if (listed)
			return true;
		files.clear();
		std::string page;
		for (int pages = 0; pages < 100; pages++)
		{
			std::vector<uint8_t> answer;
			const std::string url = googleApi() + "/drive/v3/files?spaces=appDataFolder&pageSize=1000"
					"&fields=nextPageToken,files(id,name,size)" + (page.empty() ? "" : "&pageToken=" + urlEncode(page));
			if (call("GET", url, {}, "", answer, why) != 200)
				return false;
			Json json;
			if (!parse(answer, json))
			{
				why = "Drive's answer could not be read";
				return false;
			}
			if (const Json *list = json.find("files"))
				for (const Json& file : list->items)
				{
					// Two files of one name (an upload asked twice): the first stays.
					const std::string name = file.string("name");
					if (!name.empty() && !files.count(name))
						files[name] = { file.string("id"), (uint64_t)strtoull(file.string("size").c_str(), nullptr, 10) };
				}
			page = json.string("nextPageToken");
			if (page.empty())
				break;
		}
		listed = true;
		return true;
	}

	std::map<std::string, File> files;
	bool listed = false;
	std::string why;
};

// ---------------------------------------------------------------- Dropbox

// The app folder holds the folders as they are: "/memory cards/Game_1.mcd".
class DropboxStore : public Store
{
public:
	bool walk(const std::string& top, bool recurse, bool, std::map<std::string, uint64_t>& out) override
	{
		std::vector<uint8_t> answer;
		std::string body = "{\"path\":" + jsonString("/" + top) + ",\"recursive\":" + (recurse ? "true" : "false")
				+ ",\"limit\":2000}";
		std::string url = dropboxApi() + "/2/files/list_folder";
		for (int pages = 0; pages < 200; pages++)
		{
			const int status = call("POST", url, { { "Content-Type", "application/json" } }, body, answer, why);
			if (status == 409 && pages == 0 && std::string(answer.begin(), answer.end()).find("not_found") != std::string::npos)
				return true;		// not made yet: empty
			if (status != 200)
				return false;
			Json json;
			if (!parse(answer, json))
			{
				why = "Dropbox's answer could not be read";
				return false;
			}
			const std::string prefix = "/" + lower(top) + "/";
			if (const Json *entries = json.find("entries"))
				for (const Json& entry : entries->items)
				{
					if (entry.string(".tag") != "file")
						continue;
					// Dropbox names are case-blind: the lower-case path decides where, the shown one what.
					const std::string shown = entry.string("path_display"), key = entry.string("path_lower");
					if (key.compare(0, prefix.size(), prefix) != 0 || shown.size() < prefix.size())
						continue;
					const Json *size = entry.find("size");
					out[shown.substr(prefix.size())] = size != nullptr ? (uint64_t)size->number : 0;
				}
			const Json *more = json.find("has_more");
			if (more == nullptr || !more->boolean)
				break;
			url = dropboxApi() + "/2/files/list_folder/continue";
			body = "{\"cursor\":" + jsonString(json.string("cursor")) + "}";
		}
		return true;
	}

	bool read(const std::string& path, std::vector<uint8_t>& out) override
	{
		return call("POST", dropboxContent() + "/2/files/download",
				{ { "Dropbox-API-Arg", "{\"path\":" + jsonString("/" + path, true) + "}" } }, "", out, why) == 200;
	}

	bool write(const std::string& path, const void *data, size_t size, std::string& error) override
	{
		std::vector<uint8_t> answer;
		const int status = call("POST", dropboxContent() + "/2/files/upload",
				{ { "Content-Type", "application/octet-stream" },
					{ "Dropbox-API-Arg", "{\"path\":" + jsonString("/" + path, true) + ",\"mode\":\"overwrite\",\"mute\":true}" } },
				std::string((const char *)data, size), answer, why);
		if (status == 200)
			return true;
		error = why;
		return false;
	}

	bool remove(const std::string& path) override
	{
		std::vector<uint8_t> answer;
		const int status = call("POST", dropboxApi() + "/2/files/delete_v2", { { "Content-Type", "application/json" } },
				"{\"path\":" + jsonString("/" + path) + "}", answer, why);
		return status == 200 || (status == 409 && std::string(answer.begin(), answer.end()).find("not_found") != std::string::npos);
	}

	std::string error() override { return why; }
	std::string name() override { return "Dropbox"; }

private:
	static std::string lower(std::string text)
	{
		for (char& c : text)
			c = (char)tolower((unsigned char)c);
		return text;
	}
	std::string why;
};

// ---------------------------------------------------------------- sign in

std::mutex signMutex;
SignIn signing;
std::atomic<bool> signCancel{false};
std::thread signThread;
std::string dropboxVerifier;

void setSigning(const SignIn& state)
{
	std::lock_guard<std::mutex> lock(signMutex);
	signing = state;
}

void signedInAs(Service service, const std::string& refresh, const std::string& account, const std::string& access,
		double expiresIn)
{
	{
		std::lock_guard<std::mutex> lock(mutex);
		loaded = true;
		signedService = service;
		refreshToken = refresh;
		accountName = account;
		accessToken = access;
		accessUntil = now() + expiresIn - 60.0;
		save();
	}
	diag::mark("cloud: signed in to %s%s", serviceName(service).c_str(), account.empty() ? "" : (" as " + account).c_str());
}

// Who it is, for the settings page: the account's address.
std::string whoIsIt(Service service, const std::string& access)
{
	std::vector<uint8_t> answer;
	Json json;
	if (service == GoogleDrive)
	{
		if (tls::send("GET", googleApi() + "/drive/v3/about?fields=user(emailAddress)",
				{ { "Authorization", "Bearer " + access } }, "", answer, Seconds) == 200 && parse(answer, json))
			if (const Json *user = json.find("user"))
				return user->string("emailAddress");
	}
	else if (tls::send("POST", dropboxApi() + "/2/users/get_current_account", { { "Authorization", "Bearer " + access } },
			"", answer, Seconds) == 200 && parse(answer, json))
		return json.string("email");
	return "";
}

void googleSignIn()
{
	const Keys k = keys();
	SignIn state;
	state.service = GoogleDrive;
	state.stage = SignIn::Asking;
	setSigning(state);
	std::vector<uint8_t> answer;
	std::string why;
	const int status = tls::send("POST", googleAuth() + "/device/code",
			{ { "Content-Type", "application/x-www-form-urlencoded" } },
			form({ { "client_id", k.googleId }, { "scope", "https://www.googleapis.com/auth/drive.appdata" } }), answer,
			Seconds, 1u << 20, &why);
	Json json;
	if (status != 200 || !parse(answer, json) || json.string("device_code").empty())
	{
		state.stage = SignIn::Failed;
		state.why = "Google did not give a code: " + (why.empty() ? format("it answered %d", status) : why) + ".";
		setSigning(state);
		return;
	}
	const std::string device = json.string("device_code");
	state.code = json.string("user_code");
	state.url = json.string("verification_url");
	if (state.url.empty())
		state.url = json.string("verification_uri");
	const Json *every = json.find("interval");
	const Json *within = json.find("expires_in");
	double interval = every != nullptr && every->type == Json::Number ? every->number : 5.0;
	const double until = now() + (within != nullptr && within->type == Json::Number ? within->number : 900.0);
	state.stage = SignIn::Waiting;
	setSigning(state);
	while (!signCancel && now() < until)
	{
		for (double waited = 0; waited < interval && !signCancel; waited += 0.25)
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		if (signCancel)
			break;
		answer.clear();
		why.clear();
		const int s = tls::send("POST", googleAuth() + "/token", { { "Content-Type", "application/x-www-form-urlencoded" } },
				form({ { "client_id", k.googleId }, { "client_secret", k.googleSecret }, { "device_code", device },
					{ "grant_type", "urn:ietf:params:oauth:grant-type:device_code" } }),
				answer, Seconds, 1u << 20, &why);
		Json token;
		if (!parse(answer, token))
			continue;		// no answer this time: asked again
		if (s == 200 && !token.string("refresh_token").empty())
		{
			const std::string access = token.string("access_token");
			const Json *expires = token.find("expires_in");
			signedInAs(GoogleDrive, token.string("refresh_token"), whoIsIt(GoogleDrive, access), access,
					expires != nullptr ? expires->number : 3600.0);
			state.stage = SignIn::Done;
			setSigning(state);
			return;
		}
		const std::string error = token.string("error");
		if (error == "authorization_pending")
			continue;
		if (error == "slow_down")
		{
			interval += 5;
			continue;
		}
		state.stage = SignIn::Failed;
		state.why = error == "access_denied" ? "It was not allowed on the phone." : error == "expired_token"
				? "The code expired before it was used." : "Google refused (" + (error.empty() ? std::to_string(s) : error) + ").";
		setSigning(state);
		return;
	}
	state.stage = signCancel ? SignIn::Idle : SignIn::Failed;
	state.why = signCancel ? "" : "The code expired before it was used.";
	setSigning(state);
}

std::string base64Url(const unsigned char *data, size_t size)
{
	unsigned char out[256];
	size_t written = 0;
	mbedtls_base64_encode(out, sizeof(out), &written, data, size);
	std::string text((const char *)out, written);
	for (char& c : text)
		c = c == '+' ? '-' : c == '/' ? '_' : c;
	while (!text.empty() && text.back() == '=')
		text.pop_back();
	return text;
}

void dropboxExchange(std::string code)
{
	const Keys k = keys();
	SignIn state;
	state.service = Dropbox;
	state.stage = SignIn::Asking;
	setSigning(state);
	std::vector<uint8_t> answer;
	std::string why;
	const int status = tls::send("POST", dropboxApi() + "/oauth2/token",
			{ { "Content-Type", "application/x-www-form-urlencoded" } },
			form({ { "code", trimmed(code) }, { "grant_type", "authorization_code" }, { "code_verifier", dropboxVerifier },
				{ "client_id", k.dropboxKey } }),
			answer, Seconds, 1u << 20, &why);
	Json json;
	if (status == 200 && parse(answer, json) && !json.string("refresh_token").empty())
	{
		const std::string access = json.string("access_token");
		const Json *expires = json.find("expires_in");
		signedInAs(Dropbox, json.string("refresh_token"), whoIsIt(Dropbox, access), access,
				expires != nullptr ? expires->number : 14400.0);
		state.stage = SignIn::Done;
	}
	else
	{
		std::string error = parse(answer, json) ? json.string("error_description") : "";
		state.stage = SignIn::Failed;
		state.why = "Dropbox did not take the code: " + (!error.empty() ? error : !why.empty() ? why
				: format("it answered %d", status)) + ".";
	}
	setSigning(state);
}

void runSigning(std::function<void()> job)
{
	signCancel = true;
	if (signThread.joinable())
		signThread.join();
	signCancel = false;
	signThread = std::thread(std::move(job));
}
}

std::string serviceName(Service service)
{
	return service == GoogleDrive ? "Google Drive" : service == Dropbox ? "Dropbox" : "None";
}

bool available(Service service)
{
	const Keys k = keys();
	return service == GoogleDrive ? !k.googleId.empty() && !k.googleSecret.empty()
			: service == Dropbox ? !k.dropboxKey.empty() : false;
}

Service service()
{
	std::lock_guard<std::mutex> lock(mutex);
	load();
	return signedService;
}

std::string account()
{
	std::lock_guard<std::mutex> lock(mutex);
	load();
	return accountName;
}

void startSignIn(Service which)
{
	if (!available(which))
	{
		SignIn state;
		state.service = which;
		state.stage = SignIn::Failed;
		state.why = "This build has no registration with " + serviceName(which) + ".";
		setSigning(state);
		return;
	}
	if (which == GoogleDrive)
		runSigning(googleSignIn);
	else
	{
		// PKCE: a verifier of 48 random bytes (64 characters), and its SHA-256 for the address.
		unsigned char random[48], hash[32];
		size_t written = 0;
		mbedtls_hardware_poll(nullptr, random, sizeof(random), &written);
		dropboxVerifier = base64Url(random, sizeof(random));
		mbedtls_sha256((const unsigned char *)dropboxVerifier.data(), dropboxVerifier.size(), hash, 0);
		SignIn state;
		state.service = Dropbox;
		state.stage = SignIn::Waiting;
		state.url = "https://www.dropbox.com/oauth2/authorize?client_id=" + urlEncode(keys().dropboxKey)
				+ "&response_type=code&token_access_type=offline&code_challenge_method=S256&code_challenge="
				+ base64Url(hash, sizeof(hash));
		setSigning(state);
	}
}

void dropboxCode(const std::string& code)
{
	if (dropboxVerifier.empty() || trimmed(code).empty())
		return;
	runSigning([code] { dropboxExchange(code); });
}

SignIn signIn()
{
	std::lock_guard<std::mutex> lock(signMutex);
	return signing;
}

void cancelSignIn()
{
	signCancel = true;
	if (signThread.joinable())
		signThread.join();
	setSigning(SignIn());
}

void signOut()
{
	cancelSignIn();
	std::string token;
	Service was;
	{
		std::lock_guard<std::mutex> lock(mutex);
		load();
		was = signedService;
		token = refreshToken;
		signedService = None;
		refreshToken.clear();
		accountName.clear();
		accessToken.clear();
		save();
	}
	// The token is given back, so it stops working there too (best effort).
	std::vector<uint8_t> answer;
	if (was == GoogleDrive && !token.empty())
		tls::send("POST", googleAuth() + "/revoke", { { "Content-Type", "application/x-www-form-urlencoded" } },
				form({ { "token", token } }), answer, 10);
	diag::mark("cloud: signed out of %s", serviceName(was).c_str());
}

std::unique_ptr<Store> openStore()
{
	switch (service())
	{
	case GoogleDrive: return std::make_unique<DriveStore>();
	case Dropbox: return std::make_unique<DropboxStore>();
	default: return nullptr;
	}
}

void finish()
{
	signCancel = true;
	if (signThread.joinable())
		signThread.join();
}

}
