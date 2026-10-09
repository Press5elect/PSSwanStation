/*
	PSSwanStation - the updater: a newer build from the releases page, put in
	place of this one.

	SPDX-License-Identifier: GPL-3.0-or-later

	Three steps, each asked for by the interface:

	check()     reads GitHub's "latest release" answer (JSON) and finds in it
	            the build number, the release's text, the title's ZIP and that
	            ZIP's .sha256 file.
	download()  fetches the ZIP into <title folder>/.update/release.zip,
	            compares its size and SHA-256 with the release's, looks at
	            every entry of the archive before anything is unpacked, and
	            unpacks the program's files into .update/stage/. The list of
	            them is .update/plan.
	install()   writes .update/journal, then renames each old file into
	            .update/backup/ and the new one into its place. When all are in
	            place the journal goes first and .update/ after it.

	Only the program's own files are ever unpacked or replaced (isManaged()
	below). The ZIP's other entries are passed over, and a file on the disk
	that the release does not have is left where it is, also inside the
	program's folders.

	An install that is interrupted leaves the journal behind. recover(), at the
	next start, reads it and puts the old files back; it moves a new file back
	into .update/stage/ before the old one returns, so that being interrupted
	itself changes nothing in what the next start reads from the folder.

	.update/ holds, while it exists:
	    release.zip   the download
	    stage/        the new files, unpacked
	    plan          their paths, a line each, eboot.bin last
	    new           what install() adds to the title's folder: the files
	                  that had no older one, and the folders made for them
	                  (those end in '/')
	    journal       the plan again: present from the first rename to the last
	    backup/       the old files while the journal is there
*/
#include "update.h"

#include "fe.h"

#include <miniz.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace fe::update
{
namespace
{
constexpr uint64_t MaxZipBytes = 512ull << 20;
constexpr uint64_t MaxUnpackedBytes = 1ull << 30;
constexpr size_t MaxEntries = 20000;
constexpr size_t MaxAnswerBytes = 4u << 20;		// the releases page's JSON
constexpr size_t MaxNotes = 4000;				// characters of the release's text
constexpr size_t MaxParamJson = 1u << 20;
constexpr size_t MaxComponent = 255;
constexpr int MaxJsonDepth = 48;
constexpr int MaxTreeDepth = 32;
// The console writes large blocks many times faster than small ones.
constexpr size_t WriteBlock = 1u << 20;
constexpr unsigned AnswerSeconds = 20;

// The program's files: these folders whole, and these files of the title's
// folder itself. Nothing else is unpacked, replaced or put back.
constexpr const char *ManagedFolders[] = { "sce_sys/", "sce_module/", "assets/", "licenses/" };
constexpr const char *ManagedFiles[] = { "eboot.bin", "README.txt", "CHANGELOG.txt", "BUILD.txt", "LEGAL.txt",
	"lapy.elf", "lapy-manifest.json" };

// What check() found on the releases page.
struct Release
{
	int build = 0;
	std::string zipName, zipUrl;
	std::string checksumUrl;	// empty: the release has no .sha256 file
	std::string digest;			// the SHA-256 GitHub itself computed, or empty
	uint64_t zipSize = 0;
	bool hasSize = false;
};

// stateMutex: what status() copies, and the set-up. Held for moments only.
std::mutex stateMutex;
Status current;
Setup setup;
Release found;
bool initialised;

// callMutex: one of init(), check(), download(), install() and recover() at a
// time. A state in which the worker runs (Checking, Downloading, Verifying) is
// entered only with it held, and left only by the worker.
std::mutex callMutex;
std::atomic<bool> stopRequested{false};

// The one worker thread, for a check or a download. It is joined before the
// next one starts and when the program ends. Declared after everything it
// uses, so that it ends before any of that does.
struct Worker
{
	std::thread thread;
	void join()
	{
		if (thread.joinable())
			thread.join();
	}
	~Worker()
	{
		stopRequested = true;
		join();
	}
};
Worker worker;

// ------------------------------------------------------------------ paths

bool isManaged(const std::string& path)
{
	for (const char *file : ManagedFiles)
		if (path == file)
			return true;
	for (const char *folder : ManagedFolders)
	{
		const size_t length = strlen(folder);
		if (path.size() > length && path.compare(0, length, folder) == 0)
			return true;
	}
	return false;
}

// What is wrong with a path that is to be added to a folder, or nullptr: it
// must stay below that folder (not absolute, no empty, "." or ".." part), and
// have no backslash, no control character and no part longer than a file name
// may be. A folder's entry in a ZIP ends in '/', which `folder` allows.
const char *pathFault(const std::string& path, bool folder = false)
{
	if (path.empty())
		return "has no name";
	if (path[0] == '/')
		return "is an absolute path";
	for (const unsigned char c : path)
		if (c < 0x20 || c == 0x7f || c == '\\')
			return "has a backslash or a control character in its name";
	size_t start = 0;
	while (start <= path.size())
	{
		size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		const std::string part = path.substr(start, end - start);
		const bool last = end == path.size();
		if (part.empty())
		{
			// Only as what follows a folder's closing '/'.
			if (!(folder && last && start > 0))
				return "has an empty part in its path";
		}
		else if (part == "." || part == "..")
			return "has \"..\" or \".\" in its path";
		else if (part.size() > MaxComponent)
			return "has a name that is too long";
		start = end + 1;
	}
	return nullptr;
}

bool isSafeRelative(const std::string& path)
{
	return pathFault(path) == nullptr;
}

std::string withSlash(std::string dir)
{
	if (!dir.empty() && dir.back() != '/')
		dir += '/';
	return dir;
}

std::string parentOf(const std::string& path)
{
	const size_t slash = path.rfind('/');
	return slash == std::string::npos ? "" : path.substr(0, slash);
}

// Whether the path is a link. open() with O_NOFOLLOW refuses a link (ELOOP,
// or EMLINK as FreeBSD has it) whatever lstat() does; lstat() is asked only
// when open() cannot tell. On the console, build 16's updater, which asked
// lstat() alone, found the list of files it had just written "no longer
// there", where stat() finds every file the title has.
bool isLink(const std::string& path)
{
	const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd >= 0)
	{
		close(fd);
		return false;
	}
	if (errno == ELOOP || errno == EMLINK)
		return true;
	struct stat own;
	return lstat(path.c_str(), &own) == 0 && S_ISLNK(own.st_mode);
}

// What is at a path, a link itself and not what it points to: a link is
// reported as one and never followed, anything else is as stat() sees it.
bool lookAt(const std::string& path, struct stat& st)
{
	if (isLink(path))
	{
		memset(&st, 0, sizeof(st));
		st.st_mode = S_IFLNK | 0777;
		return true;
	}
	return stat(path.c_str(), &st) == 0;
}

// Whether anything is at the path, a link itself and not what it points to.
bool pathExists(const std::string& path)
{
	struct stat st;
	return lookAt(path, st);
}

bool isRegularFile(const std::string& path)
{
	struct stat st;
	return lookAt(path, st) && S_ISREG(st.st_mode);
}

// ------------------------------------------------------------------ files

// A folder and all in it. Links are removed, never followed.
void removeTree(std::string path, int depth = 0)
{
	while (path.size() > 1 && path.back() == '/')
		path.pop_back();
	struct stat st;
	if (path.empty() || !lookAt(path, st))
		return;
	if (!S_ISDIR(st.st_mode))
	{
		unlink(path.c_str());
		return;
	}
	if (depth < MaxTreeDepth)
	{
		std::vector<std::string> names;
		if (DIR *list = opendir(path.c_str()))
		{
			while (const dirent *entry = readdir(list))
			{
				const std::string name = entry->d_name;
				if (name != "." && name != "..")
					names.push_back(name);
			}
			closedir(list);
		}
		for (const std::string& name : names)
			removeTree(path + "/" + name, depth + 1);
	}
	rmdir(path.c_str());
}

bool writeAll(int fd, const void *data, size_t bytes)
{
	const char *at = (const char *)data;
	while (bytes > 0)
	{
		const ssize_t written = write(fd, at, bytes);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return false;
		at += written;
		bytes -= (size_t)written;
	}
	return true;
}

// Asks for a folder's entries (a rename, a new file, a removed one) to be on
// the disk before what follows is done. Where a folder cannot be opened or
// synchronised nothing is lost by it: the order of the steps stays the same.
void syncDir(std::string path)
{
	while (path.size() > 1 && path.back() == '/')
		path.pop_back();
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return;
	fsync(fd);
	close(fd);
}

// A small file that is whole on the disk, or not there: written beside its
// place, synchronised, then renamed.
bool writeDurable(const std::string& path, const std::string& text)
{
	const std::string temporary = path + ".tmp";
	const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return false;
	const bool written = writeAll(fd, text.data(), text.size());
	const bool synced = fsync(fd) == 0;
	const bool closed = close(fd) == 0;
	if (!written || !synced || !closed || rename(temporary.c_str(), path.c_str()) != 0)
	{
		unlink(temporary.c_str());
		return false;
	}
	chmod(path.c_str(), 0666);
	return true;
}

// The lines of a list file (plan, journal, new), empty ones left out.
bool readLines(const std::string& path, std::vector<std::string>& lines)
{
	lines.clear();
	std::vector<uint8_t> data;
	if (!isRegularFile(path) || !readFile(path, data))
		return false;
	size_t start = 0;
	while (start < data.size())
	{
		size_t end = start;
		while (end < data.size() && data[end] != '\n')
			end++;
		std::string line((const char *)data.data() + start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (!line.empty())
			lines.push_back(line);
		start = end + 1;
	}
	return true;
}

// ---------------------------------------------------------------- SHA-256

class Sha256
{
public:
	void add(const uint8_t *data, size_t bytes)
	{
		total += bytes;
		while (bytes > 0)
		{
			const size_t take = std::min(bytes, sizeof(block) - filled);
			memcpy(block + filled, data, take);
			filled += take;
			data += take;
			bytes -= take;
			if (filled == sizeof(block))
			{
				transform();
				filled = 0;
			}
		}
	}

	// The digest as 64 lower-case hex digits. Once only.
	std::string finish()
	{
		const uint64_t bits = total * 8;
		const uint8_t one = 0x80, zero = 0;
		add(&one, 1);
		while (filled != 56)
			add(&zero, 1);
		uint8_t length[8];
		for (int i = 0; i < 8; i++)
			length[i] = (uint8_t)(bits >> (56 - 8 * i));
		add(length, 8);
		static const char hex[] = "0123456789abcdef";
		std::string out;
		for (const uint32_t word : state)
			for (int shift = 28; shift >= 0; shift -= 4)
				out += hex[(word >> shift) & 15];
		return out;
	}

private:
	static uint32_t rotate(uint32_t value, int by)
	{
		return (value >> by) | (value << (32 - by));
	}

	void transform()
	{
		static const uint32_t K[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
		};
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8
					| (uint32_t)block[i * 4 + 3];
		for (int i = 16; i < 64; i++)
		{
			const uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
		uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
		for (int i = 0; i < 64; i++)
		{
			const uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
			const uint32_t choose = (e & f) ^ (~e & g);
			const uint32_t t1 = h + s1 + choose + K[i] + w[i];
			const uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
			const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
			const uint32_t t2 = s0 + majority;
			h = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		state[0] += a;
		state[1] += b;
		state[2] += c;
		state[3] += d;
		state[4] += e;
		state[5] += f;
		state[6] += g;
		state[7] += h;
	}

	uint32_t state[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
		0x5be0cd19 };
	uint8_t block[64] = {};
	size_t filled = 0;
	uint64_t total = 0;
};

// The SHA-256 of a file, read in blocks. False when it cannot be read, or the
// worker was told to stop.
bool hashFile(const std::string& path, std::string& hex)
{
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return false;
	std::vector<uint8_t> buffer(WriteBlock);
	Sha256 sha;
	bool ok = true;
	for (;;)
	{
		const ssize_t got = read(fd, buffer.data(), buffer.size());
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0 || stopRequested)
			ok = false;
		if (got <= 0 || !ok)
			break;
		sha.add(buffer.data(), (size_t)got);
	}
	close(fd);
	if (ok)
		hex = sha.finish();
	return ok;
}

// A .sha256 file: 64 hex digits, alone or followed by white space and the
// file's name (what sha256sum writes).
bool parseChecksum(const std::vector<uint8_t>& data, std::string& hex)
{
	std::string text(data.begin(), data.end());
	if (text.compare(0, 3, "\xef\xbb\xbf") == 0)
		text.erase(0, 3);
	text = trim(text);
	if (text.size() < 64)
		return false;
	for (size_t i = 0; i < 64; i++)
		if (!isxdigit((unsigned char)text[i]))
			return false;
	if (text.size() > 64 && strchr(" \t\r\n", text[64]) == nullptr)
		return false;
	hex = lowercase(text.substr(0, 64));
	return true;
}

// ------------------------------------------------------------------- JSON
// As much of it as the releases page needs: every kind of value is read, an
// answer that is cut short or not quite JSON gives what was read up to there.

struct Json
{
	enum Type { Null, Bool, Number, String, Array, Object };
	Type type = Null;
	bool boolean = false;
	double number = 0;
	std::string text;
	std::vector<std::string> keys;	// an object's, beside its items
	std::vector<Json> items;

	const Json *find(const char *key) const
	{
		if (type == Object)
			for (size_t i = 0; i < keys.size() && i < items.size(); i++)
				if (keys[i] == key)
					return &items[i];
		return nullptr;
	}
	std::string string(const char *key) const
	{
		const Json *value = find(key);
		return value != nullptr && value->type == String ? value->text : "";
	}
};

class JsonReader
{
public:
	JsonReader(const char *data, size_t size) : at(data), end(data + size)
	{
	}

	bool read(Json& out, int depth = 0)
	{
		skipSpace();
		if (at >= end || depth > MaxJsonDepth)
			return false;
		const char c = *at;
		if (c == '{')
			return readObject(out, depth);
		if (c == '[')
			return readArray(out, depth);
		if (c == '"')
		{
			out.type = Json::String;
			return readString(out.text);
		}
		if (c == '-' || (c >= '0' && c <= '9'))
			return readNumber(out);
		if (word("true"))
		{
			out.type = Json::Bool;
			out.boolean = true;
			return true;
		}
		if (word("false"))
		{
			out.type = Json::Bool;
			return true;
		}
		return word("null");
	}

private:
	void skipSpace()
	{
		while (at < end && (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n'))
			at++;
	}

	bool word(const char *text)
	{
		const size_t length = strlen(text);
		if ((size_t)(end - at) < length || memcmp(at, text, length) != 0)
			return false;
		at += length;
		return true;
	}

	bool readNumber(Json& out)
	{
		const char *start = at;
		while (at < end && ((*at >= '0' && *at <= '9') || *at == '-' || *at == '+' || *at == '.' || *at == 'e'
				|| *at == 'E'))
			at++;
		out.type = Json::Number;
		out.number = strtod(std::string(start, (size_t)(at - start)).c_str(), nullptr);
		return true;
	}

	static void appendUtf8(std::string& out, uint32_t code)
	{
		if (code < 0x80)
			out += (char)code;
		else if (code < 0x800)
		{
			out += (char)(0xc0 | (code >> 6));
			out += (char)(0x80 | (code & 0x3f));
		}
		else if (code < 0x10000)
		{
			out += (char)(0xe0 | (code >> 12));
			out += (char)(0x80 | ((code >> 6) & 0x3f));
			out += (char)(0x80 | (code & 0x3f));
		}
		else
		{
			out += (char)(0xf0 | (code >> 18));
			out += (char)(0x80 | ((code >> 12) & 0x3f));
			out += (char)(0x80 | ((code >> 6) & 0x3f));
			out += (char)(0x80 | (code & 0x3f));
		}
	}

	// The four hex digits of a \u escape, the cursor on the first of them.
	bool readHex4(uint32_t& code)
	{
		if (end - at < 4)
			return false;
		code = 0;
		for (int i = 0; i < 4; i++)
		{
			const char c = at[i];
			uint32_t digit;
			if (c >= '0' && c <= '9')
				digit = (uint32_t)(c - '0');
			else if (c >= 'a' && c <= 'f')
				digit = (uint32_t)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				digit = (uint32_t)(c - 'A' + 10);
			else
				return false;
			code = code * 16 + digit;
		}
		at += 4;
		return true;
	}

	bool readString(std::string& out)
	{
		out.clear();
		at++;	// the opening quote
		while (at < end)
		{
			const char c = *at++;
			if (c == '"')
				return true;
			if (c != '\\')
			{
				out += c;
				continue;
			}
			if (at >= end)
				return false;
			const char escape = *at++;
			switch (escape)
			{
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'u':
			{
				uint32_t code;
				if (!readHex4(code))
					return false;
				if (code >= 0xd800 && code < 0xdc00)
				{
					// The first half of a pair: the second follows as its own escape.
					uint32_t low = 0;
					const char *back = at;
					if (end - at >= 6 && at[0] == '\\' && at[1] == 'u' && (at += 2, readHex4(low)) && low >= 0xdc00
							&& low < 0xe000)
						code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
					else
					{
						at = back;
						code = 0xfffd;
					}
				}
				else if (code >= 0xdc00 && code < 0xe000)
					code = 0xfffd;
				appendUtf8(out, code);
				break;
			}
			default:	// '"', '\\', '/', and anything unknown as itself
				out += escape;
				break;
			}
		}
		return false;
	}

	bool readArray(Json& out, int depth)
	{
		out.type = Json::Array;
		at++;
		for (;;)
		{
			skipSpace();
			if (at >= end)
				return false;
			if (*at == ']')
			{
				at++;
				return true;
			}
			if (*at == ',')
			{
				at++;
				continue;
			}
			Json item;
			const bool whole = read(item, depth + 1);
			out.items.push_back(std::move(item));
			if (!whole)
				return false;
		}
	}

	bool readObject(Json& out, int depth)
	{
		out.type = Json::Object;
		at++;
		for (;;)
		{
			skipSpace();
			if (at >= end)
				return false;
			if (*at == '}')
			{
				at++;
				return true;
			}
			if (*at == ',')
			{
				at++;
				continue;
			}
			if (*at != '"')
				return false;
			std::string key;
			if (!readString(key))
				return false;
			skipSpace();
			if (at >= end || *at != ':')
				return false;
			at++;
			Json value;
			const bool whole = read(value, depth + 1);
			out.keys.push_back(std::move(key));
			out.items.push_back(std::move(value));
			if (!whole)
				return false;
		}
	}

	const char *at, *end;
};

// ------------------------------------------------------------------- text

// At most `most` characters of UTF-8 text (not bytes): a cut never lands
// inside a character.
std::string cutCharacters(const std::string& text, size_t most, const char *ending)
{
	size_t characters = 0, keep = 0, at = 0;
	const size_t room = most - std::min(most, strlen(ending));
	for (; at < text.size(); at++)
	{
		if (((unsigned char)text[at] & 0xc0) == 0x80)
			continue;
		if (characters == room)
			keep = at;
		if (characters == most)
			return text.substr(0, keep) + ending;
		characters++;
	}
	return text;
}

// A line that only draws a rule: "---", "***", "* * *".
bool isRule(const std::string& line)
{
	size_t marks = 0;
	for (const char c : line)
	{
		if (c == ' ')
			continue;
		if (c != line[0] || (c != '-' && c != '*' && c != '_'))
			return false;
		marks++;
	}
	return marks >= 3;
}

// A release's text is Markdown: as plain lines for the screen. Headings lose
// their '#', bullets become "- ", bold marks and code quotes go, long runs of
// empty lines shrink to two.
std::string plainText(const std::string& markdown)
{
	std::string out;
	size_t position = 0;
	int emptyLines = 0;
	while (position < markdown.size())
	{
		size_t end = markdown.find('\n', position);
		if (end == std::string::npos)
			end = markdown.size();
		std::string line;
		for (size_t i = position; i < end; i++)
		{
			const unsigned char c = (unsigned char)markdown[i];
			if (c == '\t')
				line += "    ";
			else if (c >= 0x20 && c != 0x7f)
				line += (char)c;
		}
		position = end + 1;
		while (!line.empty() && line.back() == ' ')
			line.pop_back();

		size_t indent = 0;
		while (indent < line.size() && line[indent] == ' ')
			indent++;
		std::string rest = line.substr(indent);
		std::string lead = line.substr(0, indent);
		if (isRule(rest))
			rest.clear();
		else if (!rest.empty() && rest[0] == '#')
		{
			size_t marks = 0;
			while (marks < rest.size() && rest[marks] == '#')
				marks++;
			if (marks == rest.size() || rest[marks] == ' ')
			{
				rest = trim(rest.substr(marks));
				lead.clear();
			}
		}
		else if (!rest.empty() && rest[0] == '>')
			rest = trim(rest.substr(1));
		else if (rest.size() >= 2 && (rest[0] == '*' || rest[0] == '-' || rest[0] == '+') && rest[1] == ' ')
			rest = "- " + trim(rest.substr(2));
		for (const char *mark : { "**", "`" })
			for (size_t at = rest.find(mark); at != std::string::npos; at = rest.find(mark, at))
				rest.erase(at, strlen(mark));
		while (!rest.empty() && rest.back() == ' ')
			rest.pop_back();

		if (rest.empty())
		{
			emptyLines++;
			continue;
		}
		if (!out.empty())
			out.append((size_t)(1 + std::min(emptyLines, 2)), '\n');
		out += lead + rest;
		emptyLines = 0;
	}
	return cutCharacters(out, MaxNotes, "...");
}

// One line for the screen out of a release's name.
std::string oneLine(const std::string& text)
{
	std::string out;
	for (const unsigned char c : text)
		out += c < 0x20 || c == 0x7f ? ' ' : (char)c;
	return cutCharacters(trim(out), 100, "...");
}

bool endsWith(const std::string& text, const char *ending)
{
	const size_t length = strlen(ending);
	return text.size() >= length && text.compare(text.size() - length, length, ending) == 0;
}

// The run of digits at `at` as a number, `at` moved past it. 0 for a run too
// long to be a build number.
int digitRun(const std::string& text, size_t& at)
{
	long long value = 0;
	size_t digits = 0;
	for (; at < text.size() && text[at] >= '0' && text[at] <= '9'; at++, digits++)
		if (digits < 10)
			value = value * 10 + (text[at] - '0');
	return digits > 9 ? 0 : (int)value;
}

// The number written after "build" ("build10", "Build 10", "build-10"): the
// last such in the text. 0 when there is none.
int markedBuild(const std::string& text)
{
	const std::string lower = lowercase(text);
	int build = 0;
	for (size_t at = lower.find("build"); at != std::string::npos; at = lower.find("build", at + 5))
	{
		size_t digits = at + 5;
		while (digits < lower.size() && strchr(" -_.#", lower[digits]) != nullptr)
			digits++;
		if (digits < lower.size() && lower[digits] >= '0' && lower[digits] <= '9')
			build = digitRun(lower, digits);
	}
	return build;
}

// ------------------------------------------------------------------ state

void finish(State state, const std::string& error = "")
{
	if (state == State::Failed)
		diag::mark("update: failed: %s", error.c_str());
	std::lock_guard<std::mutex> lock(stateMutex);
	current.state = state;
	current.error = error;
	current.speed = 0;
	if (state != State::Ready && state != State::Installed)
		current.done = current.total = 0;
}

// Starts the worker. With callMutex held, and the state already one that
// keeps every other call out.
template <typename Function>
void startWorker(Function&& function)
{
	worker.join();
	worker.thread = std::thread(std::forward<Function>(function));
}

// ------------------------------------------------------------------ check

void runCheck(const Setup& use)
{
	diag::mark("update: asking %s", use.latestUrl.c_str());
	std::vector<uint8_t> answer;
	const int code = use.httpGet ? use.httpGet(use.latestUrl, answer, AnswerSeconds) : -1;
	if (code == 404)
	{
		diag::mark("update: the repository has no release yet");
		finish(State::NoRelease);
		return;
	}
	if (code != 200)
	{
		finish(State::Failed, code < 0 ? "The releases page could not be reached."
				: format("The releases page answered %d.", code));
		return;
	}
	if (answer.size() > MaxAnswerBytes)
	{
		finish(State::Failed, "The releases page's answer is too long.");
		return;
	}
	Json page;
	JsonReader((const char *)answer.data(), answer.size()).read(page);
	if (page.type != Json::Object || page.find("tag_name") == nullptr)
	{
		finish(State::Failed, "The releases page's answer could not be read.");
		return;
	}

	// The title's ZIP among the release's files: the one with the highest
	// build number in its name when there are several.
	Release release;
	const Json *chosen = nullptr;
	const Json *assets = page.find("assets");
	if (assets != nullptr && assets->type == Json::Array)
	{
		int best = -1;
		for (const Json& asset : assets->items)
		{
			const std::string name = asset.string("name");
			if (!endsWith(lowercase(name), ".zip") || use.titleId.empty()
					|| name.find(use.titleId) == std::string::npos)
				continue;
			const int build = markedBuild(name);
			if (build > best)
			{
				best = build;
				chosen = &asset;
			}
		}
	}
	const std::string tag = page.string("tag_name");
	release.build = buildNumberIn(tag);
	if (chosen != nullptr)
	{
		release.zipName = chosen->string("name");
		release.zipUrl = chosen->string("browser_download_url");
		if (const Json *size = chosen->find("size"); size != nullptr && size->type == Json::Number && size->number >= 0
				&& size->number < 1e15)
		{
			release.zipSize = (uint64_t)size->number;
			release.hasSize = true;
		}
		// "sha256:<64 hex digits>": GitHub's own, on files uploaded since 2025.
		const std::string digest = lowercase(chosen->string("digest"));
		if (digest.size() == 71 && digest.compare(0, 7, "sha256:") == 0
				&& digest.find_first_not_of("0123456789abcdef", 7) == std::string::npos)
			release.digest = digest.substr(7);
		if (release.build == 0)
			release.build = markedBuild(release.zipName);
		for (const Json& asset : assets->items)
			if (asset.string("name") == release.zipName + ".sha256")
				release.checksumUrl = asset.string("browser_download_url");
	}

	if (release.build <= 0)
	{
		diag::mark("update: the release \"%s\" has no build number", tag.c_str());
		finish(State::NoRelease);
		return;
	}
	Status status;
	status.build = release.build;
	status.name = oneLine(page.string("name"));
	if (status.name.empty())
		status.name = format("Build %d", release.build);
	if (release.build <= use.build)
	{
		diag::mark("update: build %d is running, the newest release is build %d", use.build, release.build);
		status.state = State::UpToDate;
		// It can be put in place again (Install again): a damaged folder
		// mended, or the updater tried.
		status.reinstall = chosen != nullptr
				&& (release.zipUrl.compare(0, 8, "https://") == 0 || release.zipUrl.compare(0, 7, "http://") == 0);
	}
	else if (chosen == nullptr
			|| (release.zipUrl.compare(0, 8, "https://") != 0 && release.zipUrl.compare(0, 7, "http://") != 0))
	{
		// A release whose files are not there (yet).
		diag::mark("update: the release \"%s\" has no ZIP for %s", tag.c_str(), use.titleId.c_str());
		status.state = State::NoRelease;
	}
	else
	{
		status.notes = plainText(page.string("body"));
		status.state = State::Available;
		diag::mark("update: build %d is available (%s, %llu bytes%s)", release.build, release.zipName.c_str(),
				(unsigned long long)release.zipSize,
				release.checksumUrl.empty() && release.digest.empty() ? "" : ", with a checksum");
	}
	std::lock_guard<std::mutex> lock(stateMutex);
	found = release;
	current = status;
}

// ------------------------------------------------------------- the archive

struct Entry
{
	mz_uint index;
	std::string path;	// below the title's folder
	uint64_t size;
};

// Why a ZIP is refused, for the screen and the log.
std::string refusal(const std::string& name, const char *why)
{
	std::string shown;
	for (const unsigned char c : name)
		shown += c < 0x20 || c == 0x7f ? '?' : (char)c;
	return "The release's ZIP was refused: \"" + cutCharacters(shown, 80, "...") + "\" " + why + ".";
}

size_t readArchive(void *opaque, mz_uint64 offset, void *buffer, size_t bytes)
{
	const int fd = *(const int *)opaque;
	size_t got = 0;
	while (got < bytes)
	{
		const ssize_t n = pread(fd, (char *)buffer + got, bytes - got, (off_t)(offset + got));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		got += (size_t)n;
	}
	return got;
}

// Looks at every entry before anything is unpacked. All of them must be
// below the one folder named after the title; none may be a link or a
// special file, or have a name that leads elsewhere. Gives the program's
// files (eboot.bin last) or the reason the archive is refused.
std::string examine(mz_zip_archive& zip, const std::string& titleId, std::vector<Entry>& files)
{
	files.clear();
	const mz_uint count = mz_zip_reader_get_num_files(&zip);
	if (count == 0 || count > MaxEntries)
		return "The release's ZIP was refused: it has no files, or too many.";
	const std::string top = titleId + "/";
	std::set<std::string> names;
	uint64_t unpacked = 0;
	bool eboot = false, param = false;
	for (mz_uint i = 0; i < count; i++)
	{
		mz_zip_archive_file_stat stat;
		if (!mz_zip_reader_file_stat(&zip, i, &stat))
			return "The release's ZIP was refused: its list of files is damaged.";
		const std::string name = stat.m_filename;
		// The name as stored is longer: it was cut, or has a NUL inside it.
		if (mz_zip_reader_get_filename(&zip, i, nullptr, 0) != name.size() + 1)
			return refusal(name, "has a name that cannot be used");
		const bool folder = !name.empty() && name.back() == '/';
		if (const char *fault = pathFault(name, folder))
			return refusal(name, fault);
		if (name.compare(0, top.size(), top) != 0)
			return refusal(name, "is outside the title's folder");
		// The kind of file, where the archive records a Unix mode: only plain
		// files and folders, so no link can send a later entry elsewhere.
		const unsigned kind = (stat.m_external_attr >> 16) & 0170000;
		if (kind != 0 && kind != 0100000 && kind != 0040000)
			return refusal(name, kind == 0120000 ? "is a symbolic link" : "is not a plain file");
		if (!names.insert(folder ? name.substr(0, name.size() - 1) : name).second)
			return refusal(name, "is there twice");
		if (stat.m_uncomp_size > MaxUnpackedBytes || (unpacked += stat.m_uncomp_size) > MaxUnpackedBytes)
			return "The release's ZIP was refused: it unpacks to more than 1 GiB.";
		if (folder || stat.m_is_directory)
			continue;
		const std::string path = name.substr(top.size());
		if (!isManaged(path))
			continue;
		if (!stat.m_is_supported)
			return refusal(name, "is packed in a way that cannot be read");
		if (path == "eboot.bin")
			eboot = true;
		if (path == "sce_sys/param.json")
			param = true;
		files.push_back({ i, path, stat.m_uncomp_size });
	}
	if (!eboot)
		return "The release's ZIP was refused: it has no eboot.bin.";
	if (!param)
		return "The release's ZIP was refused: it has no sce_sys/param.json.";
	// A file where another file's folder would have to be.
	std::set<std::string> paths;
	for (const Entry& file : files)
		paths.insert(file.path);
	for (const Entry& file : files)
		for (std::string above = parentOf(file.path); !above.empty(); above = parentOf(above))
			if (paths.count(above) != 0)
				return refusal(top + file.path, "is below something that is a file");
	std::stable_partition(files.begin(), files.end(), [](const Entry& file) { return file.path != "eboot.bin"; });
	return "";
}

size_t writeToText(void *opaque, mz_uint64, const void *data, size_t bytes)
{
	std::string& text = *(std::string *)opaque;
	if (text.size() + bytes > MaxParamJson)
		return 0;
	text.append((const char *)data, bytes);
	return bytes;
}

// Whether a param.json names this title: "titleId", then the id as the next
// string.
bool namesTitle(const std::string& json, const std::string& titleId)
{
	for (size_t at = json.find("\"titleId\""); at != std::string::npos; at = json.find("\"titleId\"", at + 1))
	{
		size_t value = at + 9;
		while (value < json.size() && strchr(" \t\r\n:", json[value]) != nullptr)
			value++;
		if (json.compare(value, titleId.size() + 2, "\"" + titleId + "\"") == 0)
			return true;
	}
	return false;
}

// One file being unpacked: gathered into large blocks, and never more than
// the archive's list says the file has.
struct Sink
{
	int fd = -1;
	uint64_t size = 0, written = 0;
	std::vector<uint8_t> block;
	bool failed = false;

	bool flush()
	{
		if (!block.empty() && !writeAll(fd, block.data(), block.size()))
			failed = true;
		block.clear();
		return !failed;
	}
};

size_t writeToSink(void *opaque, mz_uint64 offset, const void *data, size_t bytes)
{
	Sink& sink = *(Sink *)opaque;
	if (sink.failed || offset != sink.written || bytes > sink.size - sink.written)
	{
		sink.failed = true;
		return 0;
	}
	sink.block.insert(sink.block.end(), (const uint8_t *)data, (const uint8_t *)data + bytes);
	sink.written += bytes;
	if (sink.block.size() >= WriteBlock && !sink.flush())
		return 0;
	return bytes;
}

// Unpacks one file to its place in the stage, mode 0777: the console does not
// start an eboot.bin with less, and the FTP server is another process.
bool unpack(mz_zip_archive& zip, const Entry& entry, const std::string& path)
{
	makeDir(parentOf(path));
	Sink sink;
	sink.fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
	if (sink.fd < 0)
		return false;
	sink.size = entry.size;
	sink.block.reserve(std::min<uint64_t>(entry.size, WriteBlock + 65536));
	bool ok = mz_zip_reader_extract_to_callback(&zip, entry.index, writeToSink, &sink, 0) != MZ_FALSE;
	ok = sink.flush() && ok && sink.written == entry.size;
	ok = fchmod(sink.fd, 0777) == 0 && ok;
	// On the disk before it is renamed into the title's folder.
	ok = fsync(sink.fd) == 0 && ok;
	ok = close(sink.fd) == 0 && ok;
	return ok;
}

// --------------------------------------------------------------- download

enum class Outcome { Done, Stopped, Failed };

// The steps after the download: size, checksum, the archive, the stage.
Outcome verifyAndUnpack(const Setup& use, const Release& release, const std::string& dir, std::string& error)
{
	const std::string zipPath = dir + "release.zip";
	struct stat st;
	if (stat(zipPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
	{
		error = "The downloaded file is not there.";
		return Outcome::Failed;
	}
	const uint64_t size = (uint64_t)st.st_size;
	if (size > MaxZipBytes || (release.hasSize && size != release.zipSize))
	{
		error = format("The download is %llu bytes; the release says %llu.", (unsigned long long)size,
				(unsigned long long)release.zipSize);
		return Outcome::Failed;
	}

	// Two checksums may come with a release: the .sha256 file beside the ZIP,
	// and the digest GitHub computed when the ZIP was uploaded. Each one that
	// is there must be the file's.
	std::string wanted = release.digest;
	if (!release.checksumUrl.empty())
	{
		std::vector<uint8_t> answer;
		std::string listed;
		const int code = use.httpGet ? use.httpGet(release.checksumUrl, answer, AnswerSeconds) : -1;
		if (stopRequested)
			return Outcome::Stopped;
		if (code != 200 || !parseChecksum(answer, listed))
		{
			error = "The release's checksum could not be read.";
			return Outcome::Failed;
		}
		if (!wanted.empty() && wanted != listed)
		{
			error = "The release's two checksums differ.";
			return Outcome::Failed;
		}
		wanted = listed;
	}
	if (!wanted.empty())
	{
		std::string actual;
		if (!hashFile(zipPath, actual))
		{
			if (stopRequested)
				return Outcome::Stopped;
			error = "The downloaded file could not be read.";
			return Outcome::Failed;
		}
		if (actual != wanted)
		{
			diag::mark("update: SHA-256 %s, the release says %s", actual.c_str(), wanted.c_str());
			error = "The download is damaged: its checksum is not the release's.";
			return Outcome::Failed;
		}
		diag::mark("update: the checksum matches (%s)", actual.c_str());
	}
	else
		diag::mark("update: the release has no checksum; the ZIP's own checks are used");

	int fd = open(zipPath.c_str(), O_RDONLY);
	if (fd < 0)
	{
		error = "The downloaded file could not be read.";
		return Outcome::Failed;
	}
	mz_zip_archive zip;
	mz_zip_zero_struct(&zip);
	zip.m_pRead = readArchive;
	zip.m_pIO_opaque = &fd;
	if (!mz_zip_reader_init(&zip, size, 0))
	{
		error = format("The download is not a ZIP that can be read (%s).",
				mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
		close(fd);
		return Outcome::Failed;
	}

	Outcome outcome = Outcome::Done;
	std::vector<Entry> files;
	error = examine(zip, use.titleId, files);
	if (error.empty())
	{
		std::string param;
		for (const Entry& file : files)
			if (file.path == "sce_sys/param.json"
					&& !mz_zip_reader_extract_to_callback(&zip, file.index, writeToText, &param, 0))
				param.clear();
		if (!namesTitle(param, use.titleId))
			error = "The release's ZIP was refused: its param.json is not " + use.titleId + "'s.";
	}
	if (error.empty())
	{
		std::string plan;
		for (const Entry& file : files)
		{
			if (stopRequested)
			{
				outcome = Outcome::Stopped;
				break;
			}
			if (!unpack(zip, file, dir + "stage/" + file.path))
			{
				// What miniz says, or (no word from it, or only that writing
				// failed) what that comes down to.
				const mz_zip_error reason = mz_zip_get_last_error(&zip);
				const bool writing = reason == MZ_ZIP_NO_ERROR || reason == MZ_ZIP_WRITE_CALLBACK_FAILED;
				error = format("%s could not be unpacked (%s).", file.path.c_str(),
						writing ? "the folder cannot be written, or the storage is full" : mz_zip_get_error_string(reason));
				break;
			}
			plan += file.path + "\n";
		}
		if (error.empty() && outcome == Outcome::Done)
		{
			if (!writeDurable(dir + "plan", plan))
				error = "The update's list of files could not be written.";
			else
				diag::mark("update: %zu files unpacked, ready to install", files.size());
		}
	}
	mz_zip_reader_end(&zip);
	close(fd);
	if (!error.empty())
		outcome = Outcome::Failed;
	return outcome;
}

void runDownload(const Setup& use, const Release& release)
{
	const std::string dir = use.appDir + ".update/";
	const std::string zipPath = dir + "release.zip";
	removeTree(dir);
	makeDir(dir);
	Outcome outcome = Outcome::Failed;
	std::string error;
	if (!dirExists(dir))
		error = "The title's folder cannot be written.";
	else if (release.hasSize && release.zipSize > MaxZipBytes)
		error = "The release's ZIP is larger than an update may be.";
	else if (!use.httpDownload)
		error = "Downloads are not available.";
	else
	{
		diag::mark("update: downloading %s", release.zipUrl.c_str());
		// The speed shown is evened out: a quarter of a second at a time, each
		// new measure given three tenths of the weight.
		double measuredAt = now();
		uint64_t measuredDone = 0;
		const auto progress = [&](uint64_t done, uint64_t total) -> bool
		{
			if (stopRequested)
				return false;
			const double time = now();
			std::lock_guard<std::mutex> lock(stateMutex);
			current.done = done;
			current.total = total != 0 ? total : release.zipSize;
			if (done < measuredDone)
			{
				// Begun again (a redirect, a retry).
				measuredDone = done;
				measuredAt = time;
			}
			else if (time - measuredAt >= 0.25)
			{
				const double speed = (double)(done - measuredDone) / (time - measuredAt);
				current.speed = current.speed <= 0 ? speed : current.speed * 0.7 + speed * 0.3;
				measuredDone = done;
				measuredAt = time;
			}
			return true;
		};
		const int code = use.httpDownload(release.zipUrl, zipPath, MaxZipBytes, progress);
		if (stopRequested)
			outcome = Outcome::Stopped;
		else if (code != 200)
			error = code < 0 ? "The download did not finish." : format("The download's server answered %d.", code);
		else
		{
			chmod(zipPath.c_str(), 0666);
			{
				std::lock_guard<std::mutex> lock(stateMutex);
				current.state = State::Verifying;
				current.speed = 0;
			}
			diag::mark("update: downloaded, checking");
			outcome = verifyAndUnpack(use, release, dir, error);
		}
	}

	if (outcome == Outcome::Done)
	{
		finish(State::Ready);
		return;
	}
	removeTree(dir);
	if (outcome == Outcome::Stopped)
	{
		diag::mark("update: the download was stopped");
		finish(State::Available);
	}
	else
		finish(State::Failed, error);
}

// ---------------------------------------------------------------- install

// The tests' switches (ps5/tests/update): after how many renames an install
// or an undo stops where it is, as a power cut would stop it, and which
// rename of an install fails. Numbers from the environment in the test's
// build; never set (-1) in the title's.
#if defined(PSSWAN_UPDATE_TEST)
int testNumber(const char *name)
{
	const char *value = getenv(name);
	return value != nullptr && *value != '\0' ? atoi(value) : -1;
}
#else
constexpr int testNumber(const char *)
{
	return -1;
}
#endif

// Undoes what an install did, from the journal: the old files return from
// .update/backup/, the new ones go back to .update/stage/, the folders made
// for them are removed, and then .update/ itself. Without a journal nothing
// was begun, and only .update/ is removed. `restored` counts the old files
// put back. False when an old file could not be put back: the journal and
// .update/ then stay for the next try.
//
// A file is taken out of the title's folder only when .update/new says the
// install added it. One with no backup that is not in that list is left
// where it is, whatever else is missing from .update/.
bool undo(const std::string& appDir, bool& hadJournal, int& restored)
{
	const std::string dir = appDir + ".update/";
	const std::string journal = dir + "journal";
	hadJournal = pathExists(journal);
	restored = 0;
	if (!hadJournal)
	{
		removeTree(dir);
		return true;
	}
	std::vector<std::string> paths;
	if (!readLines(journal, paths))
	{
		diag::mark("update: the journal of an interrupted install cannot be read; .update is kept");
		return false;
	}
	const int stopAfter = testNumber("PSSWAN_UPDATE_TEST_UNDO_STOP_AFTER");
	int renames = 0;
	const auto cut = [&] { return stopAfter >= 0 && renames++ >= stopAfter; };
	std::vector<std::string> added;
	readLines(dir + "new", added);
	const std::set<std::string> fresh(added.begin(), added.end());
	bool complete = true;
	std::set<std::string> folders;
	for (size_t i = paths.size(); i-- > 0;)
	{
		const std::string& path = paths[i];
		if (!isSafeRelative(path) || !isManaged(path))
			continue;
		const std::string place = appDir + path;
		const std::string staged = dir + "stage/" + path;
		const std::string backup = dir + "backup/" + path;
		const bool installed = !pathExists(staged) && pathExists(place);
		if (pathExists(backup))
		{
			// The new file goes back to the stage first: were this interrupted
			// between the two renames, the next start would find the backup
			// and a staged file, and finish it.
			if (installed)
			{
				if (cut())
					return false;
				rename(place.c_str(), staged.c_str());
			}
			if (cut())
				return false;
			if (rename(backup.c_str(), place.c_str()) == 0)
				restored++;
			else
			{
				diag::mark("update: %s could not be put back (errno %d)", path.c_str(), errno);
				complete = false;
			}
		}
		else if (installed && fresh.count(path) != 0)
		{
			// A file the old build did not have, put in place. One that is
			// still staged was not reached, and is left.
			if (cut())
				return false;
			if (rename(place.c_str(), staged.c_str()) != 0)
				unlink(place.c_str());
		}
		folders.insert(parentOf(place));
	}
	if (!complete)
		return false;
	// The folders install() made, the deepest first (they were listed the
	// outer ones first). One that has something in it stays.
	for (size_t i = added.size(); i-- > 0;)
		if (added[i].back() == '/' && pathFault(added[i], true) == nullptr && isManaged(added[i] + "x"))
			rmdir((appDir + added[i].substr(0, added[i].size() - 1)).c_str());
	// The journal goes only when the old files are surely in place, and the
	// rest only when the journal is surely gone.
	for (const std::string& folder : folders)
		syncDir(folder);
	unlink(journal.c_str());
	syncDir(dir);
	removeTree(dir);
	return true;
}

} // namespace

// -------------------------------------------------------------------- API

bool recover(const std::string& appDirGiven)
{
	const std::string appDir = withSlash(appDirGiven);
	if (appDir.empty())
		return false;
	std::lock_guard<std::mutex> call(callMutex);
	{
		// Called out of turn, while this very folder's .update is in use:
		// there is nothing to recover, and it must stay.
		std::lock_guard<std::mutex> lock(stateMutex);
		if (initialised && setup.appDir == appDir && (current.state == State::Downloading
				|| current.state == State::Verifying || current.state == State::Ready))
			return false;
	}
	const bool leftover = pathExists(appDir + ".update");
	bool hadJournal = false;
	int restored = 0;
	const bool complete = undo(appDir, hadJournal, restored);
	if (hadJournal)
		diag::mark("update: an install was interrupted: %d old files put back%s", restored,
				complete ? "" : ", not all of them");
	else if (leftover)
		diag::mark("update: removed what an earlier update left in .update");
	return hadJournal;
}

void init(const Setup& given)
{
	std::lock_guard<std::mutex> call(callMutex);
	// A check or a download of an earlier set-up ends first.
	stopRequested = true;
	worker.join();
	stopRequested = false;
	std::lock_guard<std::mutex> lock(stateMutex);
	setup = given;
	setup.appDir = withSlash(setup.appDir);
	found = Release();
	current = Status();
	initialised = true;
}

void check()
{
	std::lock_guard<std::mutex> call(callMutex);
	Setup use;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		if (!initialised)
			return;
		switch (current.state)
		{
		case State::Checking:
		case State::Downloading:
		case State::Verifying:
		case State::Ready:		// unpacked and waiting: asking again would only lose it
		case State::Installed:
			return;
		default:
			break;
		}
		current = Status();
		current.state = State::Checking;
		use = setup;
	}
	startWorker([use] { runCheck(use); });
}

void reinstall()
{
	{
		std::lock_guard<std::mutex> call(callMutex);
		std::lock_guard<std::mutex> lock(stateMutex);
		if (!initialised || current.state != State::UpToDate || !current.reinstall)
			return;
		diag::mark("update: build %d to be put in place again", found.build);
		current.state = State::Available;
	}
	download();
}

void download()
{
	std::lock_guard<std::mutex> call(callMutex);
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		if (!initialised || current.state != State::Available)
			return;
	}
	// The worker that found the release has set its last state: it is over,
	// or all but. Only then is the stop flag this download's.
	worker.join();
	Setup use;
	Release release;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		stopRequested = false;
		current.state = State::Downloading;
		current.error.clear();
		current.done = 0;
		current.total = found.zipSize;
		current.speed = 0;
		use = setup;
		release = found;
	}
	startWorker([use, release] { runDownload(use, release); });
}

void cancel()
{
	std::lock_guard<std::mutex> lock(stateMutex);
	if (current.state == State::Downloading || current.state == State::Verifying)
		stopRequested = true;
}

bool install()
{
	std::lock_guard<std::mutex> call(callMutex);
	Setup use;
	int build;
	{
		std::lock_guard<std::mutex> lock(stateMutex);
		if (!initialised || current.state != State::Ready)
			return false;
		use = setup;
		build = current.build;
	}
	worker.join();
	const std::string dir = use.appDir + ".update/";

	// Everything that can be known beforehand is looked at before the first
	// change: the plan, each staged file, what is at each place now.
	std::string error;
	std::vector<std::string> plan;
	std::string planText, addedText;
	std::set<std::string> made;
	if (!readLines(dir + "plan", plan) || plan.empty())
	{
		// What the console says of it, for the log.
		struct stat st;
		const int byStat = stat((dir + "plan").c_str(), &st) == 0 ? 0 : errno;
		const int byLstat = lstat((dir + "plan").c_str(), &st) == 0 ? 0 : errno;
		diag::mark("update: %splan: stat %d, lstat %d, %zu lines", dir.c_str(), byStat, byLstat, plan.size());
		error = "The unpacked update is no longer there.";
	}
	for (const std::string& path : plan)
	{
		if (!error.empty())
			break;
		struct stat st;
		if (!isSafeRelative(path) || !isManaged(path))
			error = "The update's list of files is damaged.";
		else if (!isRegularFile(dir + "stage/" + path))
			error = "The unpacked update is incomplete (" + path + ").";
		else if (lookAt(use.appDir + path, st) && !S_ISREG(st.st_mode))
			error = path + " in the title's folder is not a file; it was left alone.";
		planText += path + "\n";
		// What the install adds: the folders that are not there yet, the
		// outer ones first, and the file when there is no older one.
		std::vector<std::string> above;
		for (std::string folder = parentOf(path); !folder.empty(); folder = parentOf(folder))
			above.push_back(folder);
		for (size_t i = above.size(); i-- > 0;)
			if (!pathExists(use.appDir + above[i]) && made.insert(above[i]).second)
				addedText += above[i] + "/\n";
		if (!pathExists(use.appDir + path))
			addedText += path + "\n";
	}
	if (!error.empty())
	{
		removeTree(dir);
		finish(State::Failed, error);
		return false;
	}

	diag::mark("update: installing build %d (%zu files)", build, plan.size());
	const int stopAfter = testNumber("PSSWAN_UPDATE_TEST_STOP_AFTER");
	const int failAt = testNumber("PSSWAN_UPDATE_TEST_FAIL_AT");
	int renames = 0;
	const auto step = [&](const std::string& from, const std::string& to) -> bool
	{
		if (renames == failAt || rename(from.c_str(), to.c_str()) != 0)
			return false;
		renames++;
		return true;
	};
	// A test's power cut: everything is left as it is at that moment.
	const auto cut = [&]
	{
		if (stopAfter < 0 || renames < stopAfter)
			return false;
		finish(State::Failed, "test: stopped as by a power cut");
		return true;
	};
	std::set<std::string> folders;
	// The list of what is new before the journal, the journal before the
	// first rename.
	if (!writeDurable(dir + "new", addedText) || !writeDurable(dir + "journal", planText))
		error = "The update's journal could not be written.";
	else
	{
		syncDir(dir);
		for (const std::string& path : plan)
		{
			if (cut())
				return false;
			const std::string place = use.appDir + path;
			const std::string backup = dir + "backup/" + path;
			makeDir(parentOf(backup));
			makeDir(parentOf(place));
			folders.insert(parentOf(place));
			if (pathExists(place) && !step(place, backup))
			{
				error = format("%s could not be moved aside (errno %d).", path.c_str(), errno);
				break;
			}
			if (cut())
				return false;
			if (!step(dir + "stage/" + path, place))
			{
				error = format("%s could not be put in place (errno %d).", path.c_str(), errno);
				break;
			}
		}
	}
	if (!error.empty())
	{
		bool hadJournal = false;
		int restored = 0;
		if (!undo(use.appDir, hadJournal, restored))
			error += " Not every old file could be put back: start the title again.";
		diag::mark("update: the install was undone (%d old files put back)", restored);
		finish(State::Failed, error);
		return false;
	}
	if (cut())
		return false;

	// The new files' names on the disk, then the journal gone, then the rest:
	// at no moment is there a journal beside a stage that is half removed.
	for (const std::string& folder : folders)
		syncDir(folder);
	unlink((dir + "journal").c_str());
	syncDir(dir);
	removeTree(dir);
	diag::mark("update: build %d is installed (%zu files); the title closes", build, plan.size());
	finish(State::Installed);
	return true;
}

Status status()
{
	std::lock_guard<std::mutex> lock(stateMutex);
	return current;
}

int buildNumberIn(const std::string& text)
{
	if (const int build = markedBuild(text); build > 0)
		return build;
	int last = 0;
	for (size_t at = 0; at < text.size();)
	{
		if (text[at] >= '0' && text[at] <= '9')
			last = digitRun(text, at);
		else
			at++;
	}
	return last;
}

}
