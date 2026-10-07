/*
	PSSwanStation - the updater's test.

	SPDX-License-Identifier: GPL-3.0-or-later

	    test <work folder> <port of mock_github.py> [<name of one test>]

	Runs update.cpp against the releases mock_github.py serves on 127.0.0.1
	(HTTP through curl, which follows the redirect an asset has, as GitHub's
	do) and against a "title folder" under the work folder, made anew for each
	case: an older build's program files and a user's own files. The cases
	that are run hundreds of times (an install stopped after every rename)
	read the same releases straight from <work folder>/fixtures instead.
*/
#include "fe.h"
#include "update.h"

#include <miniz.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace fe;
using update::State;

std::vector<std::string> testMarks();

namespace
{
int checks, failures;
std::string work, fixtures, title, base;
const char *const TitleName = "PPSA99248";

#define CHECK(condition) \
	do \
	{ \
		checks++; \
		if (!(condition)) \
		{ \
			failures++; \
			printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

#define CHECK_TEXT(text, part) \
	do \
	{ \
		checks++; \
		const std::string checkedText = (text); \
		if (checkedText.find(part) == std::string::npos) \
		{ \
			failures++; \
			printf("  FAILED %s:%d: \"%s\" has no \"%s\"\n", __FILE__, __LINE__, checkedText.c_str(), part); \
		} \
	} while (0)

const char *stateName(State state)
{
	static const char *const names[] = { "Idle", "Checking", "UpToDate", "Available", "NoRelease", "Downloading",
		"Verifying", "Ready", "Installed", "Failed" };
	return names[(int)state];
}

#define CHECK_STATE(status, wanted) \
	do \
	{ \
		checks++; \
		const update::Status checkedStatus = (status); \
		if (checkedStatus.state != (wanted)) \
		{ \
			failures++; \
			printf("  FAILED %s:%d: the state is %s (%s), not %s\n", __FILE__, __LINE__, \
					stateName(checkedStatus.state), checkedStatus.error.c_str(), stateName(wanted)); \
		} \
	} while (0)

// ------------------------------------------------------------------ files

std::string shellQuote(const std::string& text)
{
	std::string out = "'";
	for (const char c : text)
		out += c == '\'' ? std::string("'\\''") : std::string(1, c);
	return out + "'";
}

void removeAll(const std::string& path)
{
	if (system(("chmod -R u+rwx " + shellQuote(path) + " 2>/dev/null; rm -rf " + shellQuote(path)).c_str()) != 0)
		printf("  could not remove %s\n", path.c_str());
}

void put(const std::string& path, const std::string& content, mode_t mode = 0666)
{
	const size_t slash = path.rfind('/');
	makeDir(path.substr(0, slash));
	FILE *f = fopen(path.c_str(), "wb");
	if (f == nullptr)
	{
		printf("  could not write %s\n", path.c_str());
		exit(2);
	}
	fwrite(content.data(), 1, content.size(), f);
	fclose(f);
	chmod(path.c_str(), mode);
}

std::string contentOf(const std::string& path)
{
	std::vector<uint8_t> data;
	readFile(path, data);
	return std::string(data.begin(), data.end());
}

bool exists(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
}

int modeOf(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0 ? (int)(st.st_mode & 07777) : -1;
}

// Everything in a folder: each file's mode and bytes, each folder's mode.
typedef std::map<std::string, std::string> Tree;

void snapshotInto(const std::string& root, const std::string& relative, Tree& tree)
{
	DIR *list = opendir((root + relative).c_str());
	if (list == nullptr)
		return;
	std::vector<std::string> names;
	while (const dirent *entry = readdir(list))
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			names.push_back(entry->d_name);
	closedir(list);
	for (const std::string& name : names)
	{
		const std::string key = relative + name;
		struct stat st;
		if (lstat((root + key).c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			tree[key + "/"] = format("folder %o", (unsigned)(st.st_mode & 07777));
			snapshotInto(root, key + "/", tree);
		}
		else if (S_ISREG(st.st_mode))
			tree[key] = format("file %o ", (unsigned)(st.st_mode & 07777)) + contentOf(root + key);
		else
			tree[key] = "something else";
	}
}

Tree snapshot(const std::string& root)
{
	Tree tree;
	snapshotInto(root, "", tree);
	return tree;
}

// Says what differs, and whether anything does.
bool sameTree(const Tree& before, const Tree& after, const char *what)
{
	int shown = 0;
	for (const auto& [key, value] : before)
	{
		const auto it = after.find(key);
		if (it == after.end())
			printf("  %s: %s is gone\n", what, key.c_str()), shown++;
		else if (it->second != value)
			printf("  %s: %s changed (%.24s... to %.24s...)\n", what, key.c_str(), value.c_str(),
					it->second.c_str()), shown++;
	}
	for (const auto& [key, value] : after)
		if (before.count(key) == 0)
			printf("  %s: %s appeared\n", what, key.c_str()), shown++;
	return shown == 0;
}

// The title's folder as build 9 and a user left it.
void freshTitle()
{
	removeAll(title);
	const std::string old = "build 9: ";
	put(title + "eboot.bin", "\x7f" "ELF" + old + std::string(50000, 'e'), 0777);
	put(title + "sce_sys/param.json", "{ \"titleId\": \"PPSA99248\", \"contentVersion\": \"old\" }\n");
	put(title + "sce_sys/icon0.png", old + "icon");
	put(title + "sce_module/libc.prx", old + "libc", 0644);
	put(title + "assets/patches.zip", old + "patches");
	put(title + "assets/old-only.bin", old + "a file later builds do not have");
	put(title + "assets/user-theme.png", "the user's own picture, kept in assets", 0600);
	put(title + "licenses/GPL-3.0.txt", old + "GPL");
	put(title + "README.txt", old + "readme");
	put(title + "CHANGELOG.txt", old + "changes");
	put(title + "BUILD.txt", "PSSwanStation, build 9\n");
	put(title + "sandbox-elevator.elf", old + "elevator", 0777);
	put(title + "network.cfg", "server = nas\nuser = me\npassword = secret\n");
	put(title + "frontend.cfg", "view = 1\nvolume = 80\n");
	put(title + "games/x.bin", std::string(300000, 'g') + "the end");
	put(title + "games/Some Game (USA)/Some Game (USA).cue", "FILE \"x.bin\" BINARY\n");
	put(title + "data/saves/card.mcd", std::string(131072, 'm'));
	put(title + "data/history.txt", "1\t2\t0\tgames/x.bin\n");
	put(title + "bios/scph1001.bin", std::string(524288, 'b'));
	put(title + "covers/Some Game (USA).png", "\x89PNG cover");
	put(title + "cheats/SLUS-00594.cht", "cheat\n");
	put(title + "psswanstation-boot.log", "main: display\n");
	makeDir(title + "data/states");
}

// ------------------------------------------------------------- the network

std::atomic<int> requests{0}, downloads{0}, progressCalls{0}, stoppedDownloads{0};
std::atomic<int> serial{0};

// One GET with curl: the body into memory, the status code returned.
int curlGet(const std::string& url, std::vector<uint8_t>& out, unsigned seconds)
{
	requests++;
	const std::string body = work + format("/curl-body-%d", serial++);
	const std::string command = format("curl -s -L --noproxy '*' --max-time %u -o %s -w '%%{http_code}' %s",
			seconds, shellQuote(body).c_str(), shellQuote(url).c_str());
	FILE *pipe = popen(command.c_str(), "r");
	if (pipe == nullptr)
		return -1;
	char text[32] = {};
	const size_t got = fread(text, 1, sizeof(text) - 1, pipe);
	text[got] = '\0';
	pclose(pipe);
	readFile(body, out);
	unlink(body.c_str());
	const int code = atoi(text);
	return code > 0 ? code : -1;
}

// The last answer's status and length in what curl -D wrote.
void readHeaders(const std::string& path, int& status, uint64_t& length)
{
	FILE *f = fopen(path.c_str(), "r");
	if (f == nullptr)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		if (strncmp(line, "HTTP/", 5) == 0)
		{
			const char *space = strchr(line, ' ');
			status = space != nullptr ? atoi(space + 1) : 0;
			length = 0;
		}
		else if (strncasecmp(line, "Content-Length:", 15) == 0)
			length = strtoull(line + 15, nullptr, 10);
	}
	fclose(f);
}

// One GET with curl into a file, following redirects, telling how far it is.
int curlDownload(const std::string& url, const std::string& path, uint64_t limit,
		const std::function<bool(uint64_t done, uint64_t total)>& progress)
{
	downloads++;
	const std::string headers = work + format("/curl-headers-%d", serial++);
	const std::string command = format("curl -s -L --noproxy '*' -D %s %s", shellQuote(headers).c_str(),
			shellQuote(url).c_str());
	FILE *pipe = popen(command.c_str(), "r");
	if (pipe == nullptr)
		return -1;
	const int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	int status = 0;
	uint64_t done = 0, total = 0;
	bool stopped = out < 0;
	char block[16384];
	ssize_t got;
	while (!stopped && (got = read(fileno(pipe), block, sizeof(block))) > 0)
	{
		if (total == 0)
			readHeaders(headers, status, total);
		if (write(out, block, (size_t)got) != got)
			stopped = true;
		done += (uint64_t)got;
		progressCalls++;
		if (done > limit)
			stopped = true;
		else if (!progress(done, total))
		{
			stopped = true;
			stoppedDownloads++;
		}
	}
	if (out >= 0)
		close(out);
	pclose(pipe);
	readHeaders(headers, status, total);
	unlink(headers.c_str());
	if (stopped || status <= 0)
		return -1;
	return status;
}

// The same two, reading the files mock_github.py wrote: no server, no curl.
std::string localPath(const std::string& url)
{
	const std::string host = "http://fixtures.invalid/";
	if (url.compare(0, host.size(), host) != 0)
		return "";
	std::string rest = url.substr(host.size());
	const std::string api = "repos/test/", latest = "/releases/latest", download = "download/";
	if (rest.compare(0, api.size(), api) == 0 && rest.size() > latest.size()
			&& rest.compare(rest.size() - latest.size(), latest.size(), latest) == 0)
		return fixtures + rest.substr(api.size(), rest.size() - api.size() - latest.size()) + "/latest.json";
	if (rest.compare(0, download.size(), download) == 0)
		return fixtures + rest.substr(download.size());
	return "";
}

int localGet(const std::string& url, std::vector<uint8_t>& out, unsigned)
{
	requests++;
	const std::string path = localPath(url);
	if (path.empty() || !readFile(path, out))
		return 404;
	if (path.size() > 12 && path.compare(path.size() - 12, 12, "/latest.json") == 0)
		return atoi(contentOf(path.substr(0, path.size() - 11) + "status").c_str());
	return 200;
}

int localDownload(const std::string& url, const std::string& path, uint64_t limit,
		const std::function<bool(uint64_t done, uint64_t total)>& progress)
{
	downloads++;
	std::vector<uint8_t> data;
	if (!readFile(localPath(url), data))
		return 404;
	if (data.size() > limit)
		return -1;
	const int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (out < 0)
		return -1;
	bool going = true;
	for (size_t at = 0; at < data.size() && going; at += 65536)
	{
		const size_t bytes = std::min<size_t>(65536, data.size() - at);
		going = write(out, data.data() + at, bytes) == (ssize_t)bytes && progress(at + bytes, data.size());
	}
	close(out);
	return going ? 200 : -1;
}

update::Setup setupFor(const std::string& release, int build, bool local = false)
{
	update::Setup setup;
	setup.appDir = title;
	setup.build = build;
	setup.titleId = TitleName;
	setup.latestUrl = (local ? std::string("http://fixtures.invalid") : base) + "/repos/test/" + release
			+ "/releases/latest";
	if (local)
	{
		setup.httpGet = localGet;
		setup.httpDownload = localDownload;
	}
	else
	{
		setup.httpGet = curlGet;
		setup.httpDownload = curlDownload;
	}
	return setup;
}

// Until the worker has nothing more to do.
update::Status settle(double seconds = 60)
{
	const double start = now();
	for (;;)
	{
		const update::Status status = update::status();
		if (status.state != State::Checking && status.state != State::Downloading && status.state != State::Verifying)
			return status;
		if (now() - start > seconds)
		{
			failures++;
			printf("  FAILED: still %s after %.0f seconds\n", stateName(status.state), seconds);
			return status;
		}
		usleep(500);
	}
}

update::Status checkOf(const std::string& release, int build, bool local = false)
{
	update::init(setupFor(release, build, local));
	update::check();
	return settle();
}

// check() and download() of a release, to whatever state that ends in.
update::Status downloadOf(const std::string& release, bool local = false, int build = 9)
{
	const update::Status found = checkOf(release, build, local);
	if (found.state != State::Available)
		return found;
	update::download();
	return settle();
}

// What a release's ZIP holds: path -> (CRC-32, size), from mock_github.py.
struct Listed
{
	uint32_t crc;
	uint64_t size;
};

std::map<std::string, Listed> manifestOf(const std::string& release)
{
	std::map<std::string, Listed> files;
	FILE *f = fopen((fixtures + release + "/manifest.txt").c_str(), "r");
	if (f == nullptr)
		return files;
	char line[2048];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		unsigned crc = 0;
		unsigned long long size = 0;
		int used = 0;
		if (sscanf(line, "%x %llu %n", &crc, &size, &used) >= 2 && used > 0)
			files[trim(line + used)] = { crc, size };
	}
	fclose(f);
	return files;
}

// The program's files, said again here on purpose: the test's own idea of
// what an update may touch.
bool programFile(const std::string& path)
{
	for (const char *file : { "eboot.bin", "README.txt", "CHANGELOG.txt", "BUILD.txt", "LEGAL.txt", "lapy.elf",
			"lapy-manifest.json" })
		if (path == file)
			return true;
	for (const char *folder : { "sce_sys/", "sce_module/", "assets/", "licenses/" })
		if (path.compare(0, strlen(folder), folder) == 0 && path.size() > strlen(folder))
			return true;
	return false;
}

// After an install: every program file of the release is in place, whole and
// 0777; everything else in the folder is as it was, to the byte and the mode.
void checkInstalled(const std::string& release, const Tree& before)
{
	const std::map<std::string, Listed> manifest = manifestOf(release);
	CHECK(!manifest.empty());
	std::set<std::string> replaced, madeFolders;
	size_t program = 0;
	for (const auto& [path, listed] : manifest)
	{
		if (!programFile(path))
			continue;
		program++;
		replaced.insert(path);
		for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1))
			madeFolders.insert(path.substr(0, slash + 1));
		std::vector<uint8_t> data;
		const bool there = readFile(title + path, data);
		const bool whole = there && data.size() == listed.size
				&& (uint32_t)mz_crc32(MZ_CRC32_INIT, data.data(), data.size()) == listed.crc;
		checks++;
		if (!whole || modeOf(title + path) != 0777)
		{
			failures++;
			printf("  FAILED: %s is %s, mode %o\n", path.c_str(),
					!there ? "missing" : whole ? "whole" : "not the release's",
					(unsigned)modeOf(title + path));
		}
	}
	CHECK(program >= 9);
	const Tree after = snapshot(title);
	CHECK(after.count(".update/") == 0);
	for (const auto& [key, value] : before)
	{
		if (replaced.count(key) != 0)
			continue;
		const auto it = after.find(key);
		checks++;
		if (it == after.end() || it->second != value)
		{
			failures++;
			printf("  FAILED: %s %s\n", key.c_str(), it == after.end() ? "is gone" : "changed");
		}
	}
	for (const auto& [key, value] : after)
	{
		if (before.count(key) != 0 || replaced.count(key) != 0)
			continue;
		// Only a folder a new program file needed may be new, and it is 0777.
		checks++;
		if (madeFolders.count(key) == 0 || value != "folder 777")
		{
			failures++;
			printf("  FAILED: %s appeared (%.20s)\n", key.c_str(), value.c_str());
		}
	}
}

bool markedWith(const char *part, size_t from = 0)
{
	const std::vector<std::string> marks = testMarks();
	for (size_t i = from; i < marks.size(); i++)
		if (marks[i].find(part) != std::string::npos)
			return true;
	return false;
}

// ------------------------------------------------------------------ cases

void testBuildNumbers()
{
	CHECK(update::buildNumberIn("build10") == 10);
	CHECK(update::buildNumberIn("v10") == 10);
	CHECK(update::buildNumberIn("PSSwanStation-PS5-PPSA99248-build10.zip") == 10);
	CHECK(update::buildNumberIn("PSSwanStation-PS5-PPSA99248-build9.zip.sha256") == 9);
	CHECK(update::buildNumberIn("Build 12") == 12);
	CHECK(update::buildNumberIn("BUILD-7") == 7);
	CHECK(update::buildNumberIn("build_0031") == 31);
	CHECK(update::buildNumberIn("v1.2-build10-ps5") == 10);
	CHECK(update::buildNumberIn("build9 then build11") == 11);
	CHECK(update::buildNumberIn("v2.5") == 5);
	CHECK(update::buildNumberIn("PPSA99248") == 99248);
	CHECK(update::buildNumberIn("") == 0);
	CHECK(update::buildNumberIn("latest") == 0);
	CHECK(update::buildNumberIn("build") == 0);
	CHECK(update::buildNumberIn("build99999999999999999999") == 0);
	CHECK(update::buildNumberIn("10") == 10);
}

void testBeforeInit()
{
	// Nothing was set up: every call returns, nothing happens.
	CHECK_STATE(update::status(), State::Idle);
	update::check();
	update::download();
	update::cancel();
	CHECK(!update::install());
	CHECK_STATE(update::status(), State::Idle);
	CHECK(requests == 0);
}

void testUpToDate()
{
	freshTitle();
	const Tree before = snapshot(title);
	update::Status status = checkOf("uptodate", 9);
	CHECK_STATE(status, State::UpToDate);
	CHECK(status.build == 9);
	CHECK(status.name == "Build 9");
	// The release is older than what runs.
	status = checkOf("uptodate", 12);
	CHECK_STATE(status, State::UpToDate);
	// And nothing can be downloaded or installed from there.
	update::download();
	CHECK_STATE(update::status(), State::UpToDate);
	CHECK(!update::install());
	CHECK(sameTree(before, snapshot(title), "up to date"));
}

void testNoRelease()
{
	freshTitle();
	const Tree before = snapshot(title);
	CHECK_STATE(checkOf("norelease", 9), State::NoRelease);
	// A repository the server does not know at all answers the same.
	CHECK_STATE(checkOf("never-heard-of-it", 9), State::NoRelease);
	// No build number anywhere; a newer tag whose files are not uploaded.
	CHECK_STATE(checkOf("no-build", 9), State::NoRelease);
	CHECK_STATE(checkOf("no-zip", 9), State::NoRelease);
	update::download();
	CHECK_STATE(update::status(), State::NoRelease);
	CHECK(sameTree(before, snapshot(title), "no release"));
}

void testAnswersThatAreNotReleases()
{
	update::Status status = checkOf("ratelimit", 9);
	CHECK_STATE(status, State::Failed);
	CHECK_TEXT(status.error, "403");
	CHECK_STATE(checkOf("garbage", 9), State::Failed);
	CHECK_STATE(checkOf("truncated", 9), State::Failed);
	CHECK_STATE(checkOf("deep", 9), State::Failed);
	// Nobody listens there.
	update::Setup setup = setupFor("good", 9);
	setup.latestUrl = "http://127.0.0.1:9/repos/test/good/releases/latest";
	update::init(setup);
	update::check();
	status = settle();
	CHECK_STATE(status, State::Failed);
	CHECK(!status.error.empty());
	// No way to ask at all.
	setup.httpGet = nullptr;
	update::init(setup);
	update::check();
	CHECK_STATE(settle(), State::Failed);
	// From Failed a new check is allowed.
	CHECK_STATE(checkOf("uptodate", 9), State::UpToDate);
}

void testAvailable()
{
	update::Status status = checkOf("good", 9);
	CHECK_STATE(status, State::Available);
	CHECK(status.build == 10);
	// The name's \u escapes, a surrogate pair among them.
	CHECK(status.name == "PSSwanStation build 10 \xe2\x80\x94 \xe2\x80\x9c" "faster\xe2\x80\x9d \xf0\x9f\x9a\x80");
	const std::string notes =
		"What's new in build 10\n"
		"\n"
		"- Faster start: the eboot.bin is \xe2\x80\x9csmaller\xe2\x80\x9d \xf0\x9f\x9a\x80\n"
		"- Netplay \xe2\x80\x94 two consoles, one game\n"
		"  - nested: memory cards are shared\n"
		"\n"
		"\n"
		"Fixes\n"
		"Thanks to everyone who sent a log.\n"
		"1. A path with a back\\slash and a \"quote\" and a    tab\n"
		"\n"
		"Full list: https://github.com/Press5elect/PSSwanStation/compare/build9...build10";
	CHECK(status.notes == notes);
	if (status.notes != notes)
		printf("----\n%s\n----\n", status.notes.c_str());
	CHECK(status.error.empty());
	CHECK(markedWith("build 10 is available (PSSwanStation-PS5-PPSA99248-build10.zip"));

	// As GitHub sends it: UTF-8 as it is, and its own digest of the file.
	status = checkOf("good-utf8", 9);
	CHECK_STATE(status, State::Available);
	CHECK(status.name == "Build 10 \xe2\x80\x94 \xe2\x80\x9c" "faster\xe2\x80\x9d");
	CHECK(status.notes == "Test release.");
	CHECK_STATE(checkOf("good-slashes", 9), State::Available);

	// The build number: from "v10"; from the ZIP's name when the tag has none.
	status = checkOf("tag-v", 9);
	CHECK_STATE(status, State::Available);
	CHECK(status.build == 10);
	status = checkOf("tag-latest", 9);
	CHECK_STATE(status, State::Available);
	CHECK(status.build == 10);
	CHECK(status.name == "Build 10");

	// Long notes are cut at 4000 characters, not inside one.
	status = checkOf("long-notes", 9);
	CHECK_STATE(status, State::Available);
	size_t characters = 0;
	for (const unsigned char c : status.notes)
		characters += (c & 0xc0) != 0x80;
	CHECK(characters == 4000);
	CHECK(status.notes.compare(0, 10, "Long\n\xc3\xa9t\xc3\xa9") == 0);
	CHECK(status.notes.size() > 3 && status.notes.compare(status.notes.size() - 3, 3, "...") == 0);
	CHECK(((unsigned char)status.notes[status.notes.size() - 4] & 0xc0) != 0xc0);
}

void testDownloadAndInstall()
{
	freshTitle();
	const Tree before = snapshot(title);
	const int downloadsBefore = downloads, progressBefore = progressCalls;
	update::Status status = downloadOf("good");
	CHECK_STATE(status, State::Ready);
	CHECK(status.build == 10);
	CHECK(status.error.empty());
	CHECK(downloads == downloadsBefore + 1);
	CHECK(progressCalls > progressBefore);
	CHECK(markedWith("the checksum matches"));

	// Ready: the program's files are staged, 0777, and listed, eboot.bin
	// last; nothing of the title is changed yet.
	const std::string dir = title + ".update/";
	CHECK(modeOf(title + ".update") == 0777);
	CHECK(exists(dir + "release.zip"));
	CHECK(modeOf(dir + "stage/eboot.bin") == 0777);
	CHECK(modeOf(dir + "stage/sce_sys/param.json") == 0777);
	CHECK(modeOf(dir + "stage/sce_sys") == 0777);
	CHECK(modeOf(dir + "stage/assets/new/deep") == 0777);
	CHECK(exists(dir + "stage/assets/empty.dat"));
	CHECK(exists(dir + "stage/lapy.elf"));
	CHECK(!exists(dir + "stage/network.cfg"));
	CHECK(!exists(dir + "stage/sandbox-elevator.elf"));
	CHECK(!exists(dir + "stage/games"));
	CHECK(!exists(dir + "stage/bios"));
	CHECK(!exists(dir + "journal"));
	const std::string plan = contentOf(dir + "plan");
	CHECK(plan.size() > 10 && plan.compare(plan.size() - 10, 10, "eboot.bin\n") == 0);
	CHECK_TEXT(plan, "sce_sys/param.json\n");
	CHECK(plan.find("network.cfg") == std::string::npos);
	Tree staged = snapshot(title);
	for (auto it = staged.begin(); it != staged.end();)
		it = it->first.compare(0, 8, ".update/") == 0 ? staged.erase(it) : std::next(it);
	CHECK(sameTree(before, staged, "ready"));

	// Other calls while it waits change nothing.
	update::check();
	update::download();
	update::cancel();
	CHECK(!update::recover(title));
	CHECK_STATE(update::status(), State::Ready);
	CHECK(exists(dir + "stage/eboot.bin"));

	CHECK(update::install());
	CHECK_STATE(update::status(), State::Installed);
	checkInstalled("good", before);
	CHECK(!exists(title + ".update"));
	CHECK(contentOf(title + "BUILD.txt") == "PSSwanStation, build 10, built 2026-10-06\n");
	CHECK(contentOf(title + "network.cfg") == "server = nas\nuser = me\npassword = secret\n");
	CHECK(contentOf(title + "assets/user-theme.png") == "the user's own picture, kept in assets");
	CHECK(modeOf(title + "assets/user-theme.png") == 0600);
	CHECK(exists(title + "assets/old-only.bin"));
	CHECK(contentOf(title + "sandbox-elevator.elf") == "build 9: elevator");
	CHECK(markedWith("build 10 is installed"));

	// Installed: the title closes; nothing more happens here.
	CHECK(!update::install());
	update::check();
	update::download();
	CHECK_STATE(update::status(), State::Installed);
	// The next start finds nothing to recover.
	CHECK(!update::recover(title));
	checkInstalled("good", before);
}

void testInstallIntoEmptyFolder()
{
	// A title folder with none of the program's files (and so no folders).
	removeAll(title);
	put(title + "games/x.bin", "game");
	const Tree before = snapshot(title);
	CHECK_STATE(downloadOf("small", true), State::Ready);
	CHECK(update::install());
	checkInstalled("small", before);
}

void testChecksums()
{
	freshTitle();
	const Tree before = snapshot(title);
	// No checksum of either kind: the ZIP's own checks are what there is.
	size_t marksBefore = testMarks().size();
	CHECK_STATE(downloadOf("no-checksum"), State::Ready);
	CHECK(markedWith("has no checksum", marksBefore));
	// GitHub's digest alone; a bare, upper-case .sha256.
	marksBefore = testMarks().size();
	CHECK_STATE(downloadOf("digest-only"), State::Ready);
	CHECK(markedWith("the checksum matches", marksBefore));
	marksBefore = testMarks().size();
	CHECK_STATE(downloadOf("good-slashes"), State::Ready);
	CHECK(markedWith("the checksum matches", marksBefore));
	CHECK_STATE(downloadOf("good-utf8"), State::Ready);
	CHECK(update::install());
	checkInstalled("good-utf8", before);

	freshTitle();
	const struct
	{
		const char *release, *error;
	} refused[] = {
		{ "bad-checksum", "checksum is not the release's" },
		{ "bad-checksum-file", "checksum could not be read" },
		{ "bad-digest", "checksum is not the release's" },
		{ "bad-size", "the release says" },
	};
	for (const auto& one : refused)
	{
		const update::Status status = downloadOf(one.release);
		CHECK_STATE(status, State::Failed);
		CHECK_TEXT(status.error, one.error);
		CHECK(status.build == 10);
		CHECK(sameTree(before, snapshot(title), one.release));
		// Failed: no download, no install; a check starts over.
		update::download();
		CHECK(!update::install());
		CHECK_STATE(update::status(), State::Failed);
	}
}

void testRefusedArchives()
{
	freshTitle();
	const Tree before = snapshot(title);
	const struct
	{
		const char *release, *error;
	} refused[] = {
		{ "zip-dotdot", "has \"..\" or \".\" in its path" },
		{ "zip-dotdot-deep", "has \"..\" or \".\" in its path" },
		{ "zip-absolute", "is an absolute path" },
		{ "zip-backslash", "has a backslash or a control character" },
		{ "zip-control", "has a backslash or a control character" },
		{ "zip-longname", "has a name that is too long" },
		{ "zip-symlink", "is a symbolic link" },
		{ "zip-twotop", "is outside the title's folder" },
		{ "zip-rootfile", "is outside the title's folder" },
		{ "zip-prefix", "is outside the title's folder" },
		{ "zip-other-top", "is outside the title's folder" },
		{ "zip-duplicate", "is there twice" },
		{ "zip-file-under-file", "is below something that is a file" },
		{ "zip-wrong-title", "param.json is not PPSA99248's" },
		{ "zip-no-eboot", "has no eboot.bin" },
		{ "zip-eboot-folder", "has no eboot.bin" },
		{ "zip-no-param", "has no sce_sys/param.json" },
		{ "zip-not-a-zip", "not a ZIP that can be read" },
		{ "zip-cut-short", "not a ZIP that can be read" },
		{ "zip-empty", "has no files" },
		{ "zip-damaged-data", "sce_sys/icon0.png could not be unpacked" },
		{ "zip-too-large", "more than 1 GiB" },
	};
	for (const auto& one : refused)
	{
		const update::Status status = downloadOf(one.release);
		CHECK_STATE(status, State::Failed);
		CHECK_TEXT(status.error, one.error);
		if (!sameTree(before, snapshot(title), one.release))
			CHECK(!"the title's folder changed");
		CHECK(!exists(title + ".update"));
		CHECK(!update::install());
	}
	CHECK(!exists(work + "/evil.txt"));
	CHECK(!exists("/tmp/psswan-evil.txt"));
	// Nothing beside the title's folder either.
	Tree beside;
	snapshotInto(work + "/title/", "", beside);
	for (const auto& [key, value] : beside)
		CHECK(key.compare(0, 10, "PPSA99248/") == 0);
}

void testProgressAndCancel()
{
	freshTitle();
	const Tree before = snapshot(title);
	CHECK_STATE(checkOf("slow", 9), State::Available);
	const uint64_t size = (uint64_t)contentOf(fixtures + "slow/PSSwanStation-PS5-PPSA99248-build10.zip").size();

	// cancel() with nothing to stop must not stop the download that follows.
	update::cancel();
	update::download();
	CHECK_STATE(update::status(), State::Downloading);
	uint64_t last = 0;
	bool grew = true, totalRight = false;
	double speed = 0;
	int samples = 0;
	const double start = now();
	while (now() - start < 20)
	{
		const update::Status status = update::status();
		if (status.state != State::Downloading)
			break;
		if (status.done < last)
			grew = false;
		if (status.done != last)
			samples++;
		last = status.done;
		totalRight = totalRight || status.total == size;
		speed = std::max(speed, status.speed);
		// A second download(), a check() and an install() while it runs: nothing.
		update::download();
		update::check();
		CHECK(!update::install());
		if (status.done > size / 3 && speed > 0)
			break;
		usleep(2000);
	}
	CHECK(grew);
	CHECK(samples >= 5);
	CHECK(totalRight);
	CHECK(speed > 1000);
	CHECK(last > 0 && last < size);
	CHECK(exists(title + ".update/release.zip"));

	// The download is told to stop through its progress call, and does so at
	// once: it does not run to its end first (two more seconds from here).
	const int stoppedBefore = stoppedDownloads;
	const double cancelled = now();
	update::cancel();
	update::cancel();
	update::Status status = settle();
	CHECK(now() - cancelled < 1.0);
	CHECK(stoppedDownloads == stoppedBefore + 1);
	CHECK_STATE(status, State::Available);
	CHECK(status.build == 10);
	CHECK(status.done == 0 && status.speed == 0);
	CHECK(status.error.empty());
	CHECK(!exists(title + ".update"));
	CHECK(sameTree(before, snapshot(title), "cancelled"));
	CHECK(markedWith("the download was stopped"));

	// And once more, from the start, to the end.
	update::download();
	status = settle();
	CHECK_STATE(status, State::Ready);
	CHECK(status.done == size && status.total == size);
	CHECK(update::install());
	checkInstalled("slow", before);
}

void testInitWhileDownloading()
{
	freshTitle();
	const Tree before = snapshot(title);
	CHECK_STATE(checkOf("slow", 9), State::Available);
	update::download();
	const double start = now();
	while (update::status().done == 0 && now() - start < 20)
		usleep(1000);
	CHECK(update::status().done > 0);
	// A new set-up ends the download first; the worker cleans up after itself.
	update::init(setupFor("uptodate", 9));
	CHECK_STATE(update::status(), State::Idle);
	CHECK(!exists(title + ".update"));
	CHECK(sameTree(before, snapshot(title), "init while downloading"));
	update::check();
	update::check();
	CHECK_STATE(settle(), State::UpToDate);
}

// An install cut off after every possible number of renames, as by a power
// cut: recover() makes the folder what it was, to the byte. With `twice`,
// the recovery itself is cut off after every possible number of its own
// renames, and run again.
void testInterrupted(const char *release, bool twice)
{
	freshTitle();
	const Tree before = snapshot(title);
	int cases = 0;
	bool finished = false;
	for (int k = 0; k < 500 && !finished; k++)
	{
		for (int j = twice ? 0 : -1; j < 500; j++)
		{
			freshTitle();
			if (downloadOf(release, true).state != State::Ready)
			{
				CHECK(!"the release was not made ready");
				return;
			}
			setenv("PSSWAN_UPDATE_TEST_STOP_AFTER", std::to_string(k).c_str(), 1);
			const bool installed = update::install();
			unsetenv("PSSWAN_UPDATE_TEST_STOP_AFTER");
			if (installed)
			{
				// k is more than the install's renames: it went through.
				finished = true;
				checkInstalled(release, before);
				break;
			}
			CHECK(exists(title + ".update/journal"));
			cases++;
			bool cut = false;
			if (j >= 0)
			{
				setenv("PSSWAN_UPDATE_TEST_UNDO_STOP_AFTER", std::to_string(j).c_str(), 1);
				CHECK(update::recover(title));
				unsetenv("PSSWAN_UPDATE_TEST_UNDO_STOP_AFTER");
				cut = exists(title + ".update/journal");
			}
			if (j < 0 || cut)
				CHECK(update::recover(title));
			const Tree after = snapshot(title);
			if (!sameTree(before, after, format("stopped after %d renames, recovery after %d", k, j).c_str()))
			{
				CHECK(!"the folder is not what it was");
				return;
			}
			// Nothing left to recover.
			CHECK(!update::recover(title));
			if (j < 0 || !cut)
				break;
		}
	}
	CHECK(finished);
	printf("  %s: %d interruptions, each recovered to the byte\n", release, cases);
	CHECK(cases >= (twice ? 60 : 20));
}

// A rename that fails in the middle of an install: install() puts everything
// back itself and says so.
void testFailedRename()
{
	freshTitle();
	const Tree before = snapshot(title);
	int cases = 0;
	for (int k = 0; k < 500; k++)
	{
		freshTitle();
		CHECK_STATE(downloadOf("good", true), State::Ready);
		setenv("PSSWAN_UPDATE_TEST_FAIL_AT", std::to_string(k).c_str(), 1);
		const bool installed = update::install();
		unsetenv("PSSWAN_UPDATE_TEST_FAIL_AT");
		if (installed)
			break;
		cases++;
		const update::Status status = update::status();
		CHECK_STATE(status, State::Failed);
		CHECK(status.error.find("could not be") != std::string::npos);
		if (!sameTree(before, snapshot(title), format("rename %d failed", k).c_str()))
		{
			CHECK(!"the folder is not what it was");
			return;
		}
		CHECK(!update::recover(title));
	}
	printf("  %d failed renames, each undone by install() itself\n", cases);
	CHECK(cases >= 20);
	CHECK_STATE(update::status(), State::Installed);
	checkInstalled("good", before);
}

void testInstallObstacles()
{
	// A folder where a program file belongs: refused before anything moves.
	freshTitle();
	unlink((title + "README.txt").c_str());
	put(title + "README.txt/notes.txt", "the user made a folder of this name");
	Tree before = snapshot(title);
	CHECK_STATE(downloadOf("small", true), State::Ready);
	CHECK(!update::install());
	update::Status status = update::status();
	CHECK_STATE(status, State::Failed);
	CHECK_TEXT(status.error, "README.txt in the title's folder is not a file");
	CHECK(sameTree(before, snapshot(title), "a folder in a file's place"));

	// The stage is damaged after Ready (a file gone; the plan gone).
	freshTitle();
	before = snapshot(title);
	CHECK_STATE(downloadOf("small", true), State::Ready);
	unlink((title + ".update/stage/sce_module/libc.prx").c_str());
	CHECK(!update::install());
	CHECK_TEXT(update::status().error, "incomplete");
	CHECK(sameTree(before, snapshot(title), "a staged file gone"));
	CHECK_STATE(downloadOf("small", true), State::Ready);
	unlink((title + ".update/plan").c_str());
	CHECK(!update::install());
	CHECK(sameTree(before, snapshot(title), "the plan gone"));
	// A plan that names what is not the program's.
	CHECK_STATE(downloadOf("small", true), State::Ready);
	put(title + ".update/plan", "games/x.bin\n../outside.txt\neboot.bin\n");
	CHECK(!update::install());
	CHECK_TEXT(update::status().error, "damaged");
	CHECK(sameTree(before, snapshot(title), "a plan with other files"));

	// Renames that really fail (not the test's switch): something is in the
	// way of the backup. At the first file; then some files in.
	for (const char *obstacle : { "backup", "backup/sce_sys" })
	{
		freshTitle();
		before = snapshot(title);
		CHECK_STATE(downloadOf("small", true), State::Ready);
		put(title + ".update/" + obstacle, "a file where a folder has to be");
		CHECK(!update::install());
		status = update::status();
		CHECK_STATE(status, State::Failed);
		CHECK_TEXT(status.error, "could not be moved aside");
		CHECK(sameTree(before, snapshot(title), obstacle));
		CHECK(!update::recover(title));
	}

	// A folder that cannot be written: the first rename fails, all is undone.
	if (geteuid() != 0)
	{
		freshTitle();
		chmod((title + "sce_sys").c_str(), 0555);
		before = snapshot(title);
		CHECK_STATE(downloadOf("small", true), State::Ready);
		CHECK(!update::install());
		CHECK_STATE(update::status(), State::Failed);
		CHECK(sameTree(before, snapshot(title), "a folder that cannot be written"));
		chmod((title + "sce_sys").c_str(), 0777);
	}
	else
		printf("  (run as root: the read-only folder case is skipped)\n");
}

void testRecover()
{
	// Nothing there at all; a folder that does not exist.
	freshTitle();
	Tree before = snapshot(title);
	CHECK(!update::recover(title));
	CHECK(!update::recover(work + "/no-such-folder/"));
	CHECK(!update::recover(""));
	CHECK(sameTree(before, snapshot(title), "nothing to recover"));

	// What an update left behind, without a journal: removed, nothing else.
	put(title + ".update/release.zip", std::string(5000, 'z'));
	put(title + ".update/stage/eboot.bin", "new eboot", 0777);
	put(title + ".update/stage/assets/deep/er/file", "new");
	put(title + ".update/backup/eboot.bin", "not a real backup: there is no journal");
	put(title + ".update/plan", "eboot.bin\n");
	put(title + ".update/new", "assets/\neboot.bin\n");
	chmod((title + ".update/stage/assets/deep").c_str(), 0555);
	size_t marksBefore = testMarks().size();
	// The folder given without its closing '/'.
	CHECK(!update::recover(title.substr(0, title.size() - 1)));
	CHECK(markedWith("removed what an earlier update left", marksBefore));
	if (geteuid() == 0)
		CHECK(!exists(title + ".update"));
	else
	{
		// A folder that cannot be emptied stays, harmlessly, until it can.
		chmod((title + ".update/stage/assets/deep").c_str(), 0777);
		CHECK(!update::recover(title));
	}
	CHECK(!exists(title + ".update"));
	CHECK(sameTree(before, snapshot(title), "a stray .update"));

	// .update as a link to the user's games: the link goes, the games stay.
	CHECK(symlink((title + "games").c_str(), (title + ".update").c_str()) == 0);
	CHECK(!update::recover(title));
	CHECK(!exists(title + ".update"));
	CHECK(sameTree(before, snapshot(title), ".update as a link"));
	// A link inside it likewise.
	makeDir(title + ".update/stage");
	CHECK(symlink((title + "data").c_str(), (title + ".update/stage/data").c_str()) == 0);
	CHECK(!update::recover(title));
	CHECK(!exists(title + ".update"));
	CHECK(sameTree(before, snapshot(title), "a link inside .update"));

	// A journal written by hand: one file half-way (moved aside, the new one
	// not yet in), one done, one new file in place, one not reached; and
	// lines that name what is not the program's, which must not be touched.
	const std::string oldEboot = contentOf(title + "eboot.bin"), oldReadme = contentOf(title + "README.txt");
	const std::string dir = title + ".update/";
	put(dir + "journal", "README.txt\nlapy.elf\nassets/new/file.bin\nCHANGELOG.txt\ngames/x.bin\n../evil.txt\n"
			"/etc/hostname\ndata/saves/card.mcd\n\neboot.bin\n");
	put(dir + "new", "assets/new/\nlapy.elf\nassets/new/file.bin\ngames/\n../../\ndata/saves/\ngames/x.bin\n"
			"data/saves/card.mcd\n/etc/hostname\n");
	put(dir + "backup/README.txt", oldReadme);
	put(title + "README.txt", "the new readme, in place");
	put(title + "lapy.elf", "a new file, in place", 0777);
	put(title + "assets/new/file.bin", "a new file in a new folder", 0777);
	put(dir + "stage/CHANGELOG.txt", "not reached");
	put(dir + "backup/eboot.bin", oldEboot, 0777);
	unlink((title + "eboot.bin").c_str());
	put(dir + "stage/eboot.bin", "the new eboot, never moved", 0777);
	marksBefore = testMarks().size();
	CHECK(update::recover(title));
	CHECK(markedWith("an install was interrupted: 2 old files put back", marksBefore));
	CHECK(sameTree(before, snapshot(title), "a journal by hand"));
	CHECK(!update::recover(title));

	// A journal alone, the rest of .update gone: nothing can be told from
	// it, and nothing of the title may be taken for a file the install added.
	put(dir + "journal", "eboot.bin\nREADME.txt\nsce_sys/param.json\n");
	CHECK(update::recover(title));
	CHECK(!exists(title + ".update"));
	CHECK(sameTree(before, snapshot(title), "a journal alone"));
	// A journal that cannot be read as a list (a folder of that name): the
	// whole of .update is kept, nothing is touched.
	put(dir + "journal/x", "x");
	put(dir + "backup/eboot.bin", "kept");
	update::recover(title);
	CHECK(contentOf(dir + "backup/eboot.bin") == "kept");
	removeAll(title + ".update");
	CHECK(sameTree(before, snapshot(title), "a journal that is a folder"));
}

void testOddOrders()
{
	freshTitle();
	const Tree before = snapshot(title);
	update::init(setupFor("good", 9, true));
	update::init(setupFor("good", 9, true));
	CHECK_STATE(update::status(), State::Idle);
	// From Idle: nothing to download, stop or install.
	update::download();
	update::cancel();
	CHECK(!update::install());
	CHECK_STATE(update::status(), State::Idle);
	// Asked three times while the first answer is on its way: one request.
	static std::atomic<bool> answer;
	answer = false;
	update::Setup held = setupFor("good", 9, true);
	held.httpGet = [](const std::string& url, std::vector<uint8_t>& out, unsigned seconds)
	{
		while (!answer)
			usleep(200);
		return localGet(url, out, seconds);
	};
	update::init(held);
	const int requestsBefore = requests;
	update::check();
	update::check();
	CHECK_STATE(update::status(), State::Checking);
	update::download();
	update::cancel();
	CHECK(!update::install());
	update::check();
	CHECK_STATE(update::status(), State::Checking);
	answer = true;
	CHECK_STATE(settle(), State::Available);
	CHECK(requests == requestsBefore + 1);
	// Available: no install; asking again is allowed and finds the same.
	CHECK(!update::install());
	update::check();
	CHECK_STATE(settle(), State::Available);
	update::download();
	update::download();
	update::check();
	CHECK_STATE(settle(), State::Ready);
	// A new set-up while a release waits: back to the start, and the next
	// download begins with a clean .update.
	update::init(setupFor("small", 9, true));
	CHECK_STATE(update::status(), State::Idle);
	CHECK(!update::install());
	CHECK(exists(title + ".update/stage/eboot.bin"));
	CHECK_STATE(downloadOf("small", true), State::Ready);
	CHECK(!exists(title + ".update/stage/assets/cheats.zip"));
	CHECK(update::install());
	checkInstalled("small", before);
	CHECK(!update::install());

	// Many threads at once, for a second: every call in any order, status()
	// all the while. What is checked is that nothing breaks (the sanitizer
	// builds look closer) and that the folder is sound afterwards.
	freshTitle();
	update::init(setupFor("small", 9, true));
	std::atomic<bool> stop{false};
	std::atomic<int> installs{0}, cancels{0}, reads{0};
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; t++)
		threads.emplace_back([&, t]
		{
			unsigned seed = 12345u + (unsigned)t * 977u;
			while (!stop)
			{
				seed = seed * 1103515245u + 12345u;
				const unsigned pick = (seed >> 16) % 1000;
				if (pick < 250)
					update::check();
				else if (pick < 500)
					update::download();
				else if (pick < 750)
				{
					if (update::install())
						installs++;
				}
				else if (pick < 751)
				{
					update::cancel();
					cancels++;
				}
				else if (pick < 800)
				{
					if (update::status().state == State::Installed)
						update::init(setupFor("small", 9, true));
				}
				else
					usleep(100);
			}
		});
	threads.emplace_back([&]
	{
		while (!stop)
		{
			const update::Status status = update::status();
			if (status.state == State::Available || status.state == State::Ready)
				reads += status.build == 10;
			else
				reads++;
			usleep(50);
		}
	});
	// For a second at least, and until an install has gone through.
	const double hammerStart = now();
	while (now() - hammerStart < 1.0 || (installs == 0 && now() - hammerStart < 30))
		usleep(10000);
	stop = true;
	for (std::thread& thread : threads)
		thread.join();
	settle();
	printf("  threads: %d installs went through, %d cancels, status() read %d times\n", installs.load(),
			cancels.load(), reads.load());
	CHECK(reads > 1000);
	CHECK(installs > 0);
	// Whatever the state: after a recover() the folder is one of the two
	// sound ones, the old build or the new.
	update::init(setupFor("small", 9, true));
	update::recover(title);
	CHECK(!exists(title + ".update"));
	const std::string eboot = contentOf(title + "eboot.bin");
	CHECK(eboot.compare(0, 4, "\x7f" "ELF") == 0);
	CHECK(contentOf(title + "games/x.bin") == std::string(300000, 'g') + "the end");
	CHECK(contentOf(title + "network.cfg") == "server = nas\nuser = me\npassword = secret\n");
}

// The title's real build 9, as it was published: the whole way with it.
void testRealRelease()
{
	if (manifestOf("real").empty())
	{
		printf("  (no real release ZIP was given: skipped)\n");
		return;
	}
	freshTitle();
	for (const char *file : { "eboot.bin", "sce_sys/param.json", "README.txt" })
		unlink((title + file).c_str());
	const Tree before = snapshot(title);
	update::Status status = checkOf("real", 8);
	CHECK_STATE(status, State::Available);
	CHECK(status.build == 9);
	update::download();
	status = settle(300);
	CHECK_STATE(status, State::Ready);
	CHECK(status.total > 10000000 && status.done == status.total);
	CHECK(update::install());
	checkInstalled("real", before);
	// (Any release will do: the test is given build 9, or a later one.)
	CHECK(contentOf(title + "BUILD.txt").compare(0, 21, "PSSwanStation, build ") == 0);
	CHECK_TEXT(contentOf(title + "sce_sys/param.json"), "\"titleId\": \"PPSA99248\"");
	std::vector<uint8_t> eboot;
	readFile(title + "eboot.bin", eboot);
	printf("  the real release: eboot.bin is %zu bytes, mode %o\n", eboot.size(),
			(unsigned)modeOf(title + "eboot.bin"));
	const std::map<std::string, Listed> manifest = manifestOf("real");
	for (const auto& [path, listed] : manifest)
		if (!programFile(path))
			printf("  the real release: %s is in the ZIP and is not one of the program's files\n", path.c_str());
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		printf("usage: test <work folder> <port> [<test>]\n");
		return 2;
	}
	setvbuf(stdout, nullptr, _IOLBF, 0);
	work = argv[1];
	fixtures = work + "/fixtures/";
	title = work + "/title/PPSA99248/";
	base = std::string("http://127.0.0.1:") + argv[2];
	const std::string only = argc > 3 ? argv[3] : "";
	removeAll(work + "/title");
	makeDir(work + "/title");

	const struct
	{
		const char *name;
		std::function<void()> run;
	} tests[] = {
		{ "before-init", testBeforeInit },
		{ "build-numbers", testBuildNumbers },
		{ "up-to-date", testUpToDate },
		{ "no-release", testNoRelease },
		{ "not-releases", testAnswersThatAreNotReleases },
		{ "available", testAvailable },
		{ "download-install", testDownloadAndInstall },
		{ "empty-folder", testInstallIntoEmptyFolder },
		{ "checksums", testChecksums },
		{ "refused-archives", testRefusedArchives },
		{ "progress-cancel", testProgressAndCancel },
		{ "init-while-downloading", testInitWhileDownloading },
		{ "interrupted", [] { testInterrupted("good", false); } },
		{ "interrupted-twice", [] { testInterrupted("small", true); } },
		{ "failed-rename", testFailedRename },
		{ "install-obstacles", testInstallObstacles },
		{ "recover", testRecover },
		{ "odd-orders", testOddOrders },
		{ "real-release", testRealRelease },
	};
	for (const auto& test : tests)
	{
		if (!only.empty() && only != test.name)
			continue;
		const int failedBefore = failures, checkedBefore = checks;
		const double start = now();
		printf("%s\n", test.name);
		test.run();
		printf("  %s (%d checks, %.1f s)\n", failures == failedBefore ? "ok" : "FAILED", checks - checkedBefore,
				now() - start);
	}
	removeAll(work + "/title");
	printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
