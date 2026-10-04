/*
	SwanStation for PS5 (from PSFlyCast) - libc entry points the console's libc does not export.

	Copyright 2026 the PSFlyCast contributors (PSFlyCast, shell/ps5)
	SPDX-License-Identifier: GPL-2.0-or-later

	newlocale and freelocale come from the payload SDK's platform layer (RADV's
	link recipe binds them); the rest are here.

	  uselocale        Flycast's logging formats in the C locale. The console is
	                   always in the C locale, so the handle is only remembered.
	  grantpt, unlockpt, ptsname
	                   the SH4 serial port's pseudo-terminal: refused.
	  if_nametoindex, if_indextoname
	                   miniupnpc's interface lookups (UPnP): refused.
	  fork, link, symlink, readlink, pathconf
	                   only libkernel_sys exports them, and a title's import of
	                   them resolves to nothing; ImGui's URL opener and libc++'s
	                   filesystem code name them, Flycast never calls them here:
	                   refused, so the title imports nothing from libkernel_sys.
	                   They are named ps5_fe_<name> here and bound with
	                   --defsym in ps5-link.sh, as RADV's link binds the
	                   platform layer's replacements.
	  isatty, getcwd, realpath, mkstemp, gai_strerror, gethostbyname,
	  getnameinfo, in6addr_any
	                   imports a title cannot use as they are (PS5 RetroArch's
	                   tools/core-imports.py records why for each): isatty and
	                   the resolver are listed by the SDK but absent at run time,
	                   getcwd needs libkernel_sys, realpath is refused to a title,
	                   in6addr_any no module exports. Flycast's logger calls
	                   isatty at start-up; libc++'s std::filesystem the rest.
	                   Also bound with --defsym (ps5-link.sh).
	  localeconv       the console's gives an empty decimal point, and code that
	                   builds a number for strtod from it (nlohmann::json, which
	                   reads Flycast's JSON) then loses every fraction: 0.62 is
	                   read as 0. This one is POSIX's C locale, as the payload
	                   SDK fork's ps5_localeconv (fa69d00) is. Bound with --defsym.
	  os_DebugBreak    Flycast's debug trap.
*/
#include <cerrno>
#include <cstdlib>
#include <locale.h>
#include <net/if.h>
#include <sys/types.h>
#include <unistd.h>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C"
{

locale_t uselocale(locale_t locale)
{
	static locale_t current = LC_GLOBAL_LOCALE;
	const locale_t previous = current;
	if (locale != nullptr)
		current = locale;
	return previous;
}

int grantpt(int)
{
	errno = EINVAL;
	return -1;
}

int unlockpt(int)
{
	errno = EINVAL;
	return -1;
}

char *ptsname(int)
{
	errno = EINVAL;
	return nullptr;
}

unsigned int if_nametoindex(const char *)
{
	errno = ENXIO;
	return 0;
}

char *if_indextoname(unsigned int, char *)
{
	errno = ENXIO;
	return nullptr;
}

pid_t ps5_fe_fork(void)
{
	errno = ENOSYS;
	return -1;
}

int ps5_fe_link(const char *, const char *)
{
	errno = ENOSYS;
	return -1;
}

int ps5_fe_symlink(const char *, const char *)
{
	errno = ENOSYS;
	return -1;
}

ssize_t ps5_fe_readlink(const char *, char *, size_t)
{
	errno = EINVAL;	// "not a symbolic link"
	return -1;
}

long ps5_fe_pathconf(const char *, int)
{
	errno = EINVAL;
	return -1;
}

int ps5_fe_isatty(int)
{
	errno = ENOTTY;
	return 0;
}

// The title's own folder is its working directory.
char *ps5_fe_getcwd(char *buffer, size_t size)
{
	static const char cwd[] = "/app0";
	if (buffer == nullptr)
	{
		buffer = (char *)malloc(sizeof(cwd));
		if (buffer == nullptr)
			return nullptr;
		size = sizeof(cwd);
	}
	if (size < sizeof(cwd))
	{
		errno = ERANGE;
		return nullptr;
	}
	memcpy(buffer, cwd, sizeof(cwd));
	return buffer;
}

// An absolute path with "." and ".." resolved and no repeated slashes, for a
// file that exists. There are no symbolic links to follow here.
char *ps5_fe_realpath(const char *path, char *resolved)
{
	if (path == nullptr || *path == 0)
	{
		errno = path == nullptr ? EINVAL : ENOENT;
		return nullptr;
	}
	std::string in = path[0] == '/' ? std::string(path) : std::string("/app0/") + path;
	std::vector<std::string> parts;
	size_t i = 0;
	while (i < in.size())
	{
		const size_t j = in.find('/', i);
		const std::string part = in.substr(i, j == std::string::npos ? std::string::npos : j - i);
		if (part == "..")
		{
			if (!parts.empty())
				parts.pop_back();
		}
		else if (!part.empty() && part != ".")
			parts.push_back(part);
		if (j == std::string::npos)
			break;
		i = j + 1;
	}
	std::string out;
	for (const auto& part : parts)
		out += "/" + part;
	if (out.empty())
		out = "/";
	struct stat st;
	if (stat(out.c_str(), &st) != 0)
		return nullptr;	// errno from stat
	if (out.size() >= PATH_MAX)
	{
		errno = ENAMETOOLONG;
		return nullptr;
	}
	if (resolved == nullptr)
	{
		resolved = (char *)malloc(out.size() + 1);
		if (resolved == nullptr)
			return nullptr;
	}
	memcpy(resolved, out.c_str(), out.size() + 1);
	return resolved;
}

int ps5_fe_mkstemp(char *pattern)
{
	const size_t n = pattern != nullptr ? strlen(pattern) : 0;
	if (n < 6 || strcmp(pattern + n - 6, "XXXXXX") != 0)
	{
		errno = EINVAL;
		return -1;
	}
	static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
	static unsigned seed = (unsigned)getpid() * 2654435761u;
	for (int attempt = 0; attempt < 100; attempt++)
	{
		for (size_t k = n - 6; k < n; k++)
		{
			seed = seed * 1103515245u + 12345u;
			pattern[k] = chars[(seed >> 16) % (sizeof(chars) - 1)];
		}
		const int fd = open(pattern, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0 || errno != EEXIST)
			return fd;
	}
	errno = EEXIST;
	return -1;
}

const char *ps5_fe_gai_strerror(int)
{
	return "name resolution is not available";
}

struct hostent *ps5_fe_gethostbyname(const char *)
{
	return nullptr;
}

int ps5_fe_getnameinfo(const struct sockaddr *, socklen_t, char *, socklen_t, char *, socklen_t, int)
{
	return EAI_FAIL;
}

struct lconv *ps5_fe_localeconv(void)
{
	static char empty[] = "";
	static char point[] = ".";
	static struct lconv conventions;
	static bool made;
	if (!made)
	{
		conventions.decimal_point = point;
		conventions.thousands_sep = empty;
		conventions.grouping = empty;
		conventions.int_curr_symbol = empty;
		conventions.currency_symbol = empty;
		conventions.mon_decimal_point = empty;
		conventions.mon_thousands_sep = empty;
		conventions.mon_grouping = empty;
		conventions.positive_sign = empty;
		conventions.negative_sign = empty;
		conventions.int_frac_digits = CHAR_MAX;
		conventions.frac_digits = CHAR_MAX;
		conventions.p_cs_precedes = CHAR_MAX;
		conventions.p_sep_by_space = CHAR_MAX;
		conventions.n_cs_precedes = CHAR_MAX;
		conventions.n_sep_by_space = CHAR_MAX;
		conventions.p_sign_posn = CHAR_MAX;
		conventions.n_sign_posn = CHAR_MAX;
		conventions.int_p_cs_precedes = CHAR_MAX;
		conventions.int_n_cs_precedes = CHAR_MAX;
		conventions.int_p_sep_by_space = CHAR_MAX;
		conventions.int_n_sep_by_space = CHAR_MAX;
		conventions.int_p_sign_posn = CHAR_MAX;
		conventions.int_n_sign_posn = CHAR_MAX;
		made = true;
	}
	return &conventions;
}

extern const struct in6_addr ps5_fe_in6addr_any;
const struct in6_addr ps5_fe_in6addr_any = IN6ADDR_ANY_INIT;

}
