/*
 * PSSwanStation - what libnfs asks of libc that a title does not have, or has
 * in another shape. libnfs's sources are compiled with these names in place
 * of libc's (ps5/CMakeLists.txt).
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * The console's sockets as libsmb2 has them (ps5/smb/compat.c, where the
 * reasons are): left blocking, with every read asking poll first. libnfs is
 * given libsmb2's own getaddrinfo, connect, readv and writev from there, and
 * these:
 *
 * fcntl, ioctl   do nothing: libnfs makes its socket non-blocking with one of
 *                them, and on the console that did not work for libsmb2.
 * recv           with MSG_DONTWAIT, which libnfs asks for: poll first, and
 *                EAGAIN when there is nothing, as a non-blocking socket says.
 * getservbyport  no service database: no port is a well-known one, and libnfs
 *                tries the reserved ports for its source port (a NAS that
 *                accepts only those needs the export's "insecure" option when
 *                the console refuses them).
 * getprotobyname no protocol database either: "tcp" is IPPROTO_TCP, for
 *                setting TCP_NODELAY and the keep-alive.
 * getuid, getgid a title has no user of its own to give: 0, which the title
 *                replaces with the uid and gid the path names.
 * strndup        here, not trusted to the console's libc, which I have not
 *                seen export it.
 */
#undef fcntl
#undef ioctl
#undef recv
#undef getservbyport
#undef getprotobyname
#undef getuid
#undef getgid
#undef strndup

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

int ps5_nfs_fcntl(int fd, int command, ...)
{
	(void)fd;
	(void)command;
	return 0;
}

int ps5_nfs_ioctl(int fd, unsigned long request, ...)
{
	(void)fd;
	(void)request;
	return 0;
}

ssize_t ps5_nfs_recv(int fd, void *buffer, size_t length, int flags)
{
	struct pollfd ready;
	int state;
	if (flags & MSG_DONTWAIT)
	{
		ready.fd = fd;
		ready.events = POLLIN;
		ready.revents = 0;
		state = poll(&ready, 1, 0);
		if (state < 0 && errno != EINTR)
			return -1;
		if (state <= 0 || ready.revents == 0)
		{
			errno = EAGAIN;
			return -1;
		}
	}
	return recv(fd, buffer, length, flags & ~MSG_DONTWAIT);
}

struct servent *ps5_nfs_getservbyport(int port, const char *protocol)
{
	(void)port;
	(void)protocol;
	return NULL;
}

struct protoent *ps5_nfs_getprotobyname(const char *name)
{
	static char tcpName[] = "tcp";
	static char *noAliases[] = { NULL };
	static struct protoent tcp = { tcpName, noAliases, IPPROTO_TCP };
	return name != NULL && strcmp(name, "tcp") == 0 ? &tcp : NULL;
}

unsigned int ps5_nfs_getuid(void)
{
	return 0;
}

unsigned int ps5_nfs_getgid(void)
{
	return 0;
}

char *ps5_nfs_strndup(const char *text, size_t most)
{
	size_t length = 0;
	char *copy;
	while (length < most && text[length] != '\0')
		length++;
	copy = malloc(length + 1);
	if (copy == NULL)
		return NULL;
	memcpy(copy, text, length);
	copy[length] = '\0';
	return copy;
}
