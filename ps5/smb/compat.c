/*
 * PSFlyCast - what libsmb2 asks of libc that a title does not have, or
 * has in another shape.
 *
 * Copyright 2026 the PSFlyCast contributors
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * libsmb2's sources are compiled with these names in place of libc's
 * (shell/ps5/ps5.cmake).
 *
 * getlogin_r, gethostname
 *     the login and host names are fixed.
 *
 * getaddrinfo, freeaddrinfo
 *     no system module gives a title one, and the name is bound to the
 *     payload SDK's, which refuses every request (build 14 on a console:
 *     "Invalid address:192.168.1.10 Can not resolve into IPv4/v6"). libsmb2
 *     asks it for the server's address even when that is written as numbers.
 *     This one answers for an IPv4 address written as numbers, which needs no
 *     look-up; a name is refused (network.cfg takes the server's IP address).
 *
 * fcntl, connect, readv, writev: a socket that stays blocking
 *     libsmb2 makes its socket non-blocking with fcntl and reads from it
 *     until the socket says there is nothing more. On the console that did
 *     not work (build 15: libsmb2 waited for ever in the read after the
 *     server's first answer, as it does on a socket left blocking), and a
 *     socket left blocking on purpose did (build 16). So:
 *       fcntl    does nothing: the socket is left blocking, everywhere, and
 *                what follows makes that work.
 *       connect  gives the socket send and receive time limits, as a last
 *                resort, and connects: it returns when the connection is made
 *                (the caller has just seen the server's port answer).
 *       readv    asks poll, without waiting, whether there is anything to
 *                read, and says EAGAIN when there is not, as a non-blocking
 *                socket would; else it is recv, which then has data and does
 *                not wait.
 *       writev   send, piece by piece: requests are small.
 *     poll itself is the system's: libsmb2 waits in it for the socket, and
 *     the platform's own code uses it in titles.
 */
/* libsmb2's sources see these names as macros for the functions defined here
 * (ps5.cmake); this file means the system's, so the macros go first. */
#undef connect
#undef fcntl
#undef getaddrinfo
#undef freeaddrinfo
#undef gethostname
#undef getlogin_r
#undef readv
#undef writev

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>

int ps5_smb_getlogin_r(char *buf, size_t size)
{
	if (size < 6)
		return ERANGE;
	strcpy(buf, "guest");
	return 0;
}

int ps5_smb_gethostname(char *name, size_t size)
{
	if (size < 4)
		return -1;
	strcpy(name, "PS5");
	return 0;
}

/* libsmb2 calls it twice, F_GETFL and F_SETFL with O_NONBLOCK, on its socket. */
int ps5_smb_fcntl(int fd, int command, ...)
{
	(void)fd;
	(void)command;
	return 0;
}

int ps5_smb_connect(int fd, const struct sockaddr *address, socklen_t length)
{
	/* No read waits here (ps5_smb_readv asks poll first); the limits are for a
	 * server that goes away in the middle of an answer or of a request. */
	struct timeval limit;
	limit.tv_sec = 15;
	limit.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
	return connect(fd, address, length);
}

ssize_t ps5_smb_readv(int fd, const struct iovec *iov, int count)
{
	ssize_t total = 0;
	int i;
	for (i = 0; i < count; i++)
	{
		struct pollfd ready;
		ssize_t n;
		int state;
		if (iov[i].iov_len == 0)
			continue;
		ready.fd = fd;
		ready.events = POLLIN;
		ready.revents = 0;
		state = poll(&ready, 1, 0);
		if (state < 0 && errno != EINTR)
			return total > 0 ? total : -1;
		if (state <= 0 || ready.revents == 0)
		{
			/* Nothing to read now. */
			if (total > 0)
				return total;
			errno = EAGAIN;
			return -1;
		}
		/* Data, the end of the stream, or an error: recv says which, at once. */
		n = recv(fd, iov[i].iov_base, iov[i].iov_len, 0);
		if (n < 0)
			return total > 0 ? total : -1;
		total += n;
		if ((size_t)n < iov[i].iov_len)
			break;	/* a short read, or the end of the stream */
	}
	return total;
}

ssize_t ps5_smb_writev(int fd, const struct iovec *iov, int count)
{
	ssize_t total = 0;
	int i;
	for (i = 0; i < count; i++)
	{
		ssize_t n;
		if (iov[i].iov_len == 0)
			continue;
		n = send(fd, iov[i].iov_base, iov[i].iov_len, 0);
		if (n < 0)
			return total > 0 ? total : -1;
		total += n;
		if ((size_t)n < iov[i].iov_len)
			break;
	}
	return total;
}

/* One block holds the answer and its address, so that freeing the answer
 * frees both. */
struct ps5_smb_address
{
	struct addrinfo info;
	struct sockaddr_in address;
};

int ps5_smb_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **result)
{
	struct in_addr numbers;
	struct ps5_smb_address *block;
	long port = 445;
	(void)hints;
	if (result == NULL)
		return EAI_FAIL;
	*result = NULL;
	if (node == NULL || inet_pton(AF_INET, node, &numbers) != 1)
		return EAI_NONAME;
	if (service != NULL && service[0] != '\0')
	{
		char *end = NULL;
		port = strtol(service, &end, 10);
		if (end == service || *end != '\0' || port <= 0 || port > 65535)
			return EAI_SERVICE;
	}
	block = calloc(1, sizeof(*block));
	if (block == NULL)
		return EAI_MEMORY;
	block->address.sin_family = AF_INET;
	block->address.sin_port = htons((unsigned short)port);
	block->address.sin_addr = numbers;
#if defined(__PROSPERO__) || defined(__FreeBSD__)
	block->address.sin_len = sizeof(block->address);
#endif
	block->info.ai_family = AF_INET;
	block->info.ai_socktype = SOCK_STREAM;
	block->info.ai_protocol = IPPROTO_TCP;
	block->info.ai_addrlen = sizeof(block->address);
	block->info.ai_addr = (struct sockaddr *)&block->address;
	*result = &block->info;
	return 0;
}

void ps5_smb_freeaddrinfo(struct addrinfo *info)
{
	/* A list of one: the block ps5_smb_getaddrinfo made. */
	free(info);
}
