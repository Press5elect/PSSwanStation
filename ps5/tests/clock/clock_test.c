/* PSSwanStation - the title's clock from the processor's counter (src/ps5/clock_ps5.c), on the PC (from Crux2).
 * SPDX-License-Identifier: GPL-3.0+
 *
 *   clock_test <case>     one case a process: the clock starts once in a process
 *
 * The console's calls are stood in for: the counter is a number this test moves on, its
 * reported rate is given, and the kernel's monotonic clock is the counter at its true rate,
 * which may differ from the reported one. What is checked: the clock never goes back; it stays
 * within the stated distance of the kernel's; the kernel is asked about once a simulated second,
 * not at each read; other clocks are the kernel's; threads reading at once each see time go on
 * (run with the thread sanitizer); a rate that is not believable leaves the kernel's clock. */
#define _GNU_SOURCE
#include "../../src/ps5/clock_ps5.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int __wrap_clock_gettime(clockid_t clock, struct timespec *now);

static unsigned s_checks, s_failed;
#define CHECK(cond, ...)                                                                                              \
   do {                                                                                                               \
      s_checks++;                                                                                                     \
      if (!(cond)) {                                                                                                  \
         s_failed++;                                                                                                  \
         if (s_failed <= 20) {                                                                                        \
            printf("  FAILED %s:%d: ", __FILE__, __LINE__);                                                           \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
         }                                                                                                            \
      }                                                                                                               \
   } while (0)

/* The stand-ins. */
static _Atomic uint64_t s_tsc = UINT64_C(5000000000000); /* an hour of uptime and more */
static uint64_t s_reported_hz = UINT64_C(1596300232);
static double s_true_hz = 1596300232.0;
static _Atomic uint64_t s_tsc_step; /* added at each read (the threads case) */
static _Atomic unsigned s_kernel_monotonic_calls, s_kernel_other_calls;
static uint64_t s_tsc_at_boot = 0;
static _Atomic uint64_t s_sleep_ns; /* the kernel's clock ran on this much without the counter */

uint64_t sceKernelReadTsc(void)
{
   const uint64_t step = atomic_load(&s_tsc_step);
   return step ? atomic_fetch_add(&s_tsc, step) + step : atomic_load(&s_tsc);
}

uint64_t sceKernelGetTscFrequency(void)
{
   return s_reported_hz;
}

static uint64_t true_kernel_ns(void)
{
   const double seconds = (double)(atomic_load(&s_tsc) - s_tsc_at_boot) / s_true_hz;
   return (uint64_t)(seconds * 1e9) + atomic_load(&s_sleep_ns);
}

int __real_clock_gettime(clockid_t clock, struct timespec *now)
{
   if (clock != CLOCK_MONOTONIC) {
      atomic_fetch_add(&s_kernel_other_calls, 1);
      return clock_gettime(clock, now);
   }
   atomic_fetch_add(&s_kernel_monotonic_calls, 1);
   if (!now)
      return -1; /* EFAULT, as the kernel answers */
   const uint64_t ns = true_kernel_ns();
   now->tv_sec = (time_t)(ns / 1000000000u);
   now->tv_nsec = (long)(ns % 1000000000u);
   return 0;
}

static uint64_t read_ns(void)
{
   struct timespec now;
   __wrap_clock_gettime(CLOCK_MONOTONIC, &now);
   return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static uint64_t rng = 0x9e3779b97f4a7c15u;
static uint64_t next_random(void)
{
   rng ^= rng << 13;
   rng ^= rng >> 7;
   rng ^= rng << 17;
   return rng;
}

static void case_math(void)
{
   const uint64_t mult = ps5_clock_mult(s_reported_hz);
   const struct ps5_clock_base base = {1000, 7, mult};
   const uint64_t one_second = ps5_clock_ns(&base, 1000 + s_reported_hz) - 7;
   CHECK(one_second >= 999999999u && one_second <= 1000000001u, "a second of ticks is %llu ns", (unsigned long long)one_second);
   const uint64_t day = ps5_clock_ns(&base, 1000 + s_reported_hz * 86400) - 7;
   CHECK(day > UINT64_C(86399999990000) && day < UINT64_C(86400000010000), "a day of ticks is %llu ns (within 10 us)", (unsigned long long)day);
   CHECK(ps5_clock_ns(&base, 500) == 7, "a counter read before the base is the base's moment");
   CHECK(ps5_clock_mult(0) == 0 && ps5_clock_mult(1000) == 0 && ps5_clock_mult(UINT64_C(20000000000)) == 0, "rates not believable");

   /* The rebase: the clock goes on from where it is, and the pace makes up the difference. */
   int64_t difference = 0;
   struct ps5_clock_base next = ps5_clock_rebase(&base, mult, 1000 + s_reported_hz, 7 + 1000000000u + 400000, &difference);
   CHECK(difference == 400000 || difference == 400001 || difference == 399999, "difference %lld", (long long)difference);
   CHECK(next.mono0 == ps5_clock_ns(&base, 1000 + s_reported_hz), "no jump");
   const uint64_t ahead = ps5_clock_ns(&next, next.tsc0 + s_reported_hz) - next.mono0;
   CHECK(ahead > 1000399000u && ahead < 1000401000u, "the next second is %llu ns", (unsigned long long)ahead);
   /* A difference beyond what a second can make up: 1 ms a second at most. */
   next = ps5_clock_rebase(&base, mult, 1000 + s_reported_hz, 7 + 1000000000u - 30000000u, &difference);
   const uint64_t slowed = ps5_clock_ns(&next, next.tsc0 + s_reported_hz) - next.mono0;
   CHECK(slowed > 998999000u && slowed < 999001000u, "slowed to %llu ns a second", (unsigned long long)slowed);
   /* The kernel's clock far ahead (a console that slept): taken at once. */
   next = ps5_clock_rebase(&base, mult, 1000 + s_reported_hz, 7 + 5000000000u, &difference);
   CHECK(next.mono0 == 7 + 5000000000u, "a sleep is taken at once");
}

/* Simulated hours of reads, the counter's true rate off by ppm from the reported one. */
static void case_drift(double ppm, double hours, double bound_ms)
{
   s_true_hz = (double)s_reported_hz * (1.0 + ppm / 1e6);
   s_tsc_at_boot = atomic_load(&s_tsc) - (uint64_t)(3600.0 * s_true_hz);
   uint64_t last = read_ns();
   const uint64_t start_tsc = atomic_load(&s_tsc);
   const uint64_t end_tsc = start_tsc + (uint64_t)(hours * 3600.0 * s_true_hz);
   double worst_ms = 0;
   unsigned reads = 0, backwards = 0;
   while (atomic_load(&s_tsc) < end_tsc) {
      /* Reads 0.05 to 5 ms apart, and now and then a gap of up to 3 s with no read. */
      const uint64_t r = next_random();
      const double gap_ms = (r % 1000 == 0) ? (double)(r % 3000) : 0.05 + (double)(r % 4950) / 1000.0;
      atomic_fetch_add(&s_tsc, (uint64_t)(gap_ms / 1000.0 * s_true_hz));
      const uint64_t now = read_ns();
      reads++;
      if (now < last)
         backwards++;
      last = now;
      if (atomic_load(&s_tsc) - start_tsc > (uint64_t)(10.0 * s_true_hz)) {
         const double ms = ((double)now - (double)true_kernel_ns()) / 1e6;
         const double size = ms < 0 ? -ms : ms;
         if (size > worst_ms)
            worst_ms = size;
      }
   }
   const double seconds = hours * 3600.0;
   const unsigned asked = atomic_load(&s_kernel_monotonic_calls);
   printf("  counter %+.0f ppm from its reported rate, %.1f h: %u reads, the kernel asked %u times, "
          "%.3f ms from the kernel's clock at most\n",
          ppm, hours, reads, asked, worst_ms);
   CHECK(backwards == 0, "%u reads went back", backwards);
   CHECK(worst_ms <= bound_ms, "%.3f ms from the kernel's clock (bound %.3f)", worst_ms, bound_ms);
   CHECK(asked <= (unsigned)(seconds * 1.05) + 10, "the kernel asked %u times in %.0f s", asked, seconds);
   CHECK(asked >= (unsigned)(seconds * 0.5), "the kernel asked only %u times in %.0f s", asked, seconds);
}

/* The kernel's clock runs on while the counter does not move (a console at rest): taken at once. */
static void case_sleep(void)
{
   s_true_hz = (double)s_reported_hz;
   s_tsc_at_boot = atomic_load(&s_tsc);
   uint64_t last = read_ns();
   for (int i = 0; i < 5000; i++) {
      atomic_fetch_add(&s_tsc, s_reported_hz / 1000);
      if (i == 2000)
         atomic_fetch_add(&s_sleep_ns, UINT64_C(600000000000)); /* ten minutes */
      const uint64_t now = read_ns();
      CHECK(now >= last, "went back at %d", i);
      last = now;
   }
   const double ms = ((double)read_ns() - (double)true_kernel_ns()) / 1e6;
   printf("  after ten minutes at rest: %.3f ms from the kernel's clock\n", ms);
   CHECK(ms > -2.0 && ms < 2.0, "%.3f ms from the kernel's clock after a rest", ms);
}

static void case_other_clocks(void)
{
   struct timespec now;
   read_ns();
   const unsigned before = atomic_load(&s_kernel_other_calls);
   CHECK(__wrap_clock_gettime(CLOCK_REALTIME, &now) == 0 && now.tv_sec > 1700000000, "the realtime clock is the kernel's");
   CHECK(__wrap_clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) == 0, "a cpu-time clock is the kernel's");
   CHECK(atomic_load(&s_kernel_other_calls) == before + 2, "two calls of the kernel's");
   CHECK(__wrap_clock_gettime(CLOCK_MONOTONIC, NULL) != 0 || 1, "no crash on a null pointer");
   char line[256];
   ps5px_clock_line(line, sizeof(line));
   printf("  %s\n", line);
   CHECK(strstr(line, "1596300232 Hz") != NULL, "the boot log's line: %s", line);
}

static void case_not_believable(void)
{
   s_reported_hz = 0;
   const unsigned before = atomic_load(&s_kernel_monotonic_calls);
   for (int i = 0; i < 100; i++) {
      atomic_fetch_add(&s_tsc, 1000000);
      read_ns();
   }
   CHECK(atomic_load(&s_kernel_monotonic_calls) >= before + 100, "the kernel answers every read");
   char line[256];
   ps5px_clock_line(line, sizeof(line));
   printf("  %s\n", line);
   CHECK(strstr(line, "not believable") != NULL, "the boot log's line: %s", line);
}

#define THREADS 8
#define READS 200000
static _Atomic unsigned s_thread_backwards;
static void *reader(void *arg)
{
   (void)arg;
   uint64_t last = 0;
   for (int i = 0; i < READS; i++) {
      const uint64_t now = read_ns();
      if (now < last)
         atomic_fetch_add(&s_thread_backwards, 1);
      last = now;
   }
   return NULL;
}

static void case_threads(void)
{
   s_true_hz = (double)s_reported_hz * (1.0 + 200 / 1e6);
   s_tsc_at_boot = atomic_load(&s_tsc);
   /* Each read moves the counter on by 2 ms' worth, so the base is checked and published often
    * while the others read. */
   atomic_store(&s_tsc_step, s_reported_hz / 500);
   pthread_t threads[THREADS];
   for (int i = 0; i < THREADS; i++)
      pthread_create(&threads[i], NULL, reader, NULL);
   for (int i = 0; i < THREADS; i++)
      pthread_join(threads[i], NULL);
   atomic_store(&s_tsc_step, 0);
   const double seconds = (double)(atomic_load(&s_tsc) - s_tsc_at_boot) / s_true_hz;
   printf("  %d threads, %d reads each, %.0f simulated s: the kernel asked %u times, %u reads went back\n",
          THREADS, READS, seconds, atomic_load(&s_kernel_monotonic_calls), atomic_load(&s_thread_backwards));
   CHECK(atomic_load(&s_thread_backwards) == 0, "a thread saw time go back");
   CHECK(atomic_load(&s_kernel_monotonic_calls) <= (unsigned)(seconds * 1.05) + 10, "asked too often");
}

int main(int argc, char **argv)
{
   const char *which = argc > 1 ? argv[1] : "";
   if (!strcmp(which, "math"))
      case_math();
   else if (!strcmp(which, "exact"))
      case_drift(0, 4, 0.05);
   else if (!strcmp(which, "fast"))
      case_drift(300, 4, 1.5); /* the counter faster than reported: 0.3 ms a second to take back */
   else if (!strcmp(which, "slow"))
      case_drift(-300, 4, 1.5);
   else if (!strcmp(which, "far-fast"))
      case_drift(2000, 0.5, 12); /* beyond the 1 ms a second made up, until the pace is learned */
   else if (!strcmp(which, "far-slow"))
      case_drift(-2000, 0.5, 12); /* the same, slow */
   else if (!strcmp(which, "sleep"))
      case_sleep();
   else if (!strcmp(which, "other"))
      case_other_clocks();
   else if (!strcmp(which, "not-believable"))
      case_not_believable();
   else if (!strcmp(which, "threads"))
      case_threads();
   else {
      fprintf(stderr, "usage: clock_test math|exact|fast|slow|far-fast|far-slow|sleep|other|not-believable|threads\n");
      return 2;
   }
   printf("clock %s: %u checks, %u failed\n", which, s_checks, s_failed);
   return s_failed ? 1 : 0;
}
