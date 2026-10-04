/*
	SwanStation for PS5 (from PSFlyCast) - fgetpos and fsetpos.

	Copyright 2026 the PSFlyCast contributors (PSFlyCast, shell/ps5)
	SPDX-License-Identifier: GPL-2.0-or-later

	The payload SDK's stdio.h declares fpos_t as an off_t, 8 bytes, as FreeBSD
	does. The console's libc writes a larger fpos_t: hostfs::StdFile::size()
	keeps its fpos_t on the stack, and fgetpos wrote past it into its caller's
	saved registers (rbx, r12, r14 came back zero; the box-art thread then
	faulted in libchdr's chd_open_core_file, console runs of 2026-10-01).

	ps5-link.sh binds both here (--wrap): the position is the stream's offset,
	read and set with ftello and fseeko, whose off_t the console and the SDK
	agree on.
*/
#include <cerrno>
#include <cstdio>
#include <sys/types.h>

extern "C" int __wrap_fgetpos(FILE *stream, fpos_t *position)
{
	const off_t offset = ftello(stream);
	if (offset < 0)
		return -1;
	*position = offset;
	return 0;
}

extern "C" int __wrap_fsetpos(FILE *stream, const fpos_t *position)
{
	return fseeko(stream, *position, SEEK_SET);
}
