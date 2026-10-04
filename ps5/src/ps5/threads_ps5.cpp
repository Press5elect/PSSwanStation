/*
	SwanStation for PS5 (from PSFlyCast) - every thread on a 2 MiB stack libkernel allocates.

	Copyright 2026 the PSFlyCast contributors (PSFlyCast, shell/ps5)
	SPDX-License-Identifier: GPL-2.0-or-later

	A thread that asks for no stack size gets 64 KiB on this console (the
	payload SDK fork's platform/docs/PROBE.md, "Threads"), too little for
	Flycast's, libc++'s and RADV's threads. The SDK fork's link wrap gives such
	threads 2 MiB in direct memory; this title asks libkernel for 2 MiB instead,
	as PS5 RetroArch does for its own threads and its cores' (patch 0096 and
	src/core_threads_ps5.cpp there), the route that title has run on: the
	first console run of this title, on the direct-memory stacks, ended with
	two of a frame's saved registers zeroed under the box-art thread
	(chd_open_core_file, 2026-10-01).

	ps5-link.sh binds pthread_create here (--wrap) in place of the SDK's
	wrap. Each thread also starts with its creator's floating-point state
	(the title runs in the IEEE state, ps5_crt.cpp; libkernel starts a thread
	with flush-to-zero on) and runs its C++ thread_local destructors when its
	start routine returns, as the SDK fork's own wrap does.
*/
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <pthread.h>

extern "C"
{
int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
		void *(*start)(void *), void *argument);
// The SDK fork's platform layer (src/cxa.c).
void ps5p_run_thread_destructors(void);
}

namespace
{
constexpr size_t MIN_STACK = 2 * 1024 * 1024;

struct Start
{
	void *(*start)(void *);
	void *argument;
	uint32_t mxcsr;
};

void *trampoline(void *opaque)
{
	const Start run = *static_cast<Start *>(opaque);
	free(opaque);
	__asm__ volatile("ldmxcsr %0" : : "m"(run.mxcsr));
	void *const result = run.start(run.argument);
	ps5p_run_thread_destructors();
	return result;
}
} // namespace

extern "C" int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
		void *(*start)(void *), void *argument)
{
	Start *run = static_cast<Start *>(malloc(sizeof(Start)));
	if (run == nullptr)
		return EAGAIN;
	run->start = start;
	run->argument = argument;
	__asm__ volatile("stmxcsr %0" : "=m"(run->mxcsr));

	pthread_attr_t own;
	pthread_attr_t *use = const_cast<pthread_attr_t *>(attributes);
	if (use == nullptr)
	{
		pthread_attr_init(&own);
		use = &own;
	}
	void *stackAddress = nullptr;
	pthread_attr_getstackaddr(use, &stackAddress);
	size_t size = 0;
	if (stackAddress == nullptr && (pthread_attr_getstacksize(use, &size) != 0 || size < MIN_STACK))
		pthread_attr_setstacksize(use, MIN_STACK);

	const int result = __real_pthread_create(thread, use, trampoline, run);
	if (use == &own)
		pthread_attr_destroy(&own);
	if (result != 0)
		free(run);
	return result;
}
