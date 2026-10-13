/* PSSwanStation - the monotonic clock from the processor's own counter (from Crux2 v1.0.1).
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0+
 *
 * On the console clock_gettime is a system call of about 20 us, while reading the processor's
 * time-stamp counter (sceKernelReadTsc) takes about 12 ns (PS5_Vulkan's R34 cost probe, 10,000
 * calls each on the console; PSWin's "Fast clock" is the same remedy for Windows games). The
 * emulator and the frame pacing read the clock many times a frame, and the graphics driver reads it in every wait for the GPU. So the title's clock_gettime
 * (CLOCK_MONOTONIC) is answered from the counter (clock_ps5.c, the link wraps the name for the
 * whole title, the driver included).
 *
 * The counter's rate is what the kernel reports (sceKernelGetTscFrequency, 1,596,300,232 Hz on
 * my console, which matched a measured sleep in the same probe). So that a rate a little off
 * cannot make the clock drift from the kernel's (a deadline built from it and handed to the
 * kernel must mean the same moment), once a second the kernel's clock is read and the clock's
 * pace for the next second is set to make up the difference, within 1 ms a second: it never
 * jumps back and never stops. From ten seconds on, the pace itself is the one the kernel's clock
 * measured over the whole run. A difference of more than 50 ms ahead of the clock (a console
 * that slept) is taken at once.
 *
 * The arithmetic is here, header-only, so the host tests check it on the PC. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The clock from a point on: ns = mono0 + (tsc - tsc0) * mult / 2^32. */
struct ps5_clock_base
{
   uint64_t tsc0;
   uint64_t mono0;
   uint64_t mult;
};

#define PS5_CLOCK_NS_PER_S UINT64_C(1000000000)
/* The most the pace is changed by to make up a difference: 1 ms a second (1000 ppm). */
#define PS5_CLOCK_MAX_SLEW_PPM 1000
/* A difference larger than this, with the kernel's clock ahead, is taken at once. */
#define PS5_CLOCK_JUMP_NS UINT64_C(50000000)

/* The pace that turns counter ticks at hz into nanoseconds; 0 for a rate that is not believable
 * (below 100 MHz or above 10 GHz). */
static inline uint64_t
ps5_clock_mult(uint64_t hz)
{
   if (hz < UINT64_C(100000000) || hz > UINT64_C(10000000000))
      return 0;
   return (uint64_t)(((unsigned __int128)PS5_CLOCK_NS_PER_S << 32) / hz);
}

static inline uint64_t
ps5_clock_ns(const struct ps5_clock_base *base, uint64_t tsc)
{
   /* A counter read on another core a hair before the base's: the base's moment. */
   const uint64_t ticks = tsc > base->tsc0 ? tsc - base->tsc0 : 0;
   return base->mono0 + (uint64_t)(((unsigned __int128)ticks * base->mult) >> 32);
}

/* The base for the next second: the clock goes on from where it is at tsc (no jump back), at
 * the nominal pace changed by what the kernel's clock (kernel_ns, read at tsc) says it is off,
 * so the difference is made up over the next second. *difference_ns gets kernel - clock. */
static inline struct ps5_clock_base
ps5_clock_rebase(const struct ps5_clock_base *base, uint64_t nominal_mult, uint64_t tsc, uint64_t kernel_ns,
                 int64_t *difference_ns)
{
   const uint64_t now = ps5_clock_ns(base, tsc);
   const int64_t difference = (int64_t)(kernel_ns - now);
   if (difference_ns)
      *difference_ns = difference;

   struct ps5_clock_base next = {tsc, now, nominal_mult};
   if (difference > (int64_t)PS5_CLOCK_JUMP_NS) {
      next.mono0 = kernel_ns; /* the kernel's clock ran on without the counter: forward at once */
      return next;
   }
   int64_t slew = difference; /* ns to make up over the next second */
   const int64_t limit = (int64_t)(PS5_CLOCK_NS_PER_S / 1000000 * PS5_CLOCK_MAX_SLEW_PPM);
   if (slew > limit)
      slew = limit;
   if (slew < -limit)
      slew = -limit;
   next.mult = (uint64_t)(((__int128)nominal_mult * ((__int128)PS5_CLOCK_NS_PER_S + slew)) /
                          (__int128)PS5_CLOCK_NS_PER_S);
   return next;
}

/* The counter's pace as the kernel's clock measures it, from the first check (start_tsc,
 * start_kernel_ns) to this one, once at least ten seconds apart (the kernel's clock is read to
 * about 20 us, a few parts a million over ten seconds); the reported pace before, and kept within
 * 1% of it. A reported rate a little off is then corrected for, not only made up for each second. */
#define PS5_CLOCK_LEARN_NS (UINT64_C(10) * PS5_CLOCK_NS_PER_S)
static inline uint64_t
ps5_clock_learned_mult(uint64_t reported_mult, uint64_t start_tsc, uint64_t start_kernel_ns, uint64_t tsc,
                       uint64_t kernel_ns)
{
   if (tsc <= start_tsc || kernel_ns < start_kernel_ns + PS5_CLOCK_LEARN_NS)
      return reported_mult;
   const uint64_t learned =
      (uint64_t)(((unsigned __int128)(kernel_ns - start_kernel_ns) << 32) / (unsigned __int128)(tsc - start_tsc));
   const uint64_t low = reported_mult - reported_mult / 100, high = reported_mult + reported_mult / 100;
   return learned < low ? low : learned > high ? high : learned;
}

/* The runtime (clock_ps5.c). */
/* Nanoseconds on the title's monotonic clock: the counter's when it is running, else the
 * kernel's. */
uint64_t ps5px_monotonic_ns(void);
/* One line for the boot log: the counter's rate, how many times the clock was checked against
 * the kernel's and the largest difference found; "" before the clock runs. */
void ps5px_clock_line(char *line, unsigned size);

#ifdef __cplusplus
}
#endif
