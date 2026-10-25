/* The CPU's instruction cache, timed, run in place of the BIOS.
 *
 * A block of 64 instructions (16 cache lines) is copied to RAM and called
 * through KSEG0 with the cache flushed (every line a miss), again (every line
 * a hit), through KUSEG (the same physical code: psx-spx says the tag holds
 * the physical address, so still hits) and through KSEG1 (never cached).
 * Root counter 2 counts the system clock for each call.
 *
 * RAM[0x100 + n * 4]: the cycles of case n (0 cold KSEG0, 1 warm KSEG0,
 * 2 KUSEG after KSEG0, 3 KSEG1). RAM[0] = 0xC0FFEE when done. */
typedef unsigned int u32;

__attribute__((section(".params"))) const volatile u32 param_seed = 1;
__attribute__((section(".params"))) const volatile u32 param_mode = 0;

#define IO32(a) (*(volatile u32*)(0xBF800000u + (a)))
#define RAM32(a) (*(volatile u32*)(0xA0000000u + (a)))

#define BLOCK 0x10000u
#define BLOCK_WORDS 64u
#define BLOCK2 0x20000u /* a second block, of 128 words (32 lines) */

/* A store to a cache address (0 included), out of the compiler's sight. */
static inline void store_zero(u32 address)
{
  __asm__ volatile("sw $zero, 0(%0)" : : "r"(address) : "memory");
}

static inline void set_sr(u32 v)
{
  __asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop" : : "r"(v));
}

/* The BIOS's flush (psx-spx, Memory Control, BCC): the tags cleared with the
 * cache isolated and TAG set, then every code word. Runs from the ROM, which
 * is never cached. */
static void flush_icache(void)
{
  volatile u32* const bcc = (volatile u32*)0xFFFE0130u;
  *bcc = 0x804u; /* TAG | IS1 */
  set_sr(0x10000u); /* IsC */
  for (u32 i = 0; i < 0x1000u; i += 16)
    store_zero(i);
  set_sr(0);
  *bcc = 0x800u; /* IS1 */
  set_sr(0x10000u);
  for (u32 i = 0; i < 0x1000u; i += 4)
    store_zero(i);
  set_sr(0);
  *bcc = 0x0001E988u; /* as the BIOS leaves it */
}

__attribute__((noinline)) static u32 timed(u32 address)
{
  void (*const call)(void) = (void (*)(void))address;
  IO32(0x1124) = 0; /* counter 2: system clock; the write starts it at 0 */
  call();
  return IO32(0x1120) & 0xFFFFu;
}

void test_main(void)
{
  /* 62 nops, jr ra, nop */
  for (u32 i = 0; i < BLOCK_WORDS - 2; i++)
    RAM32(BLOCK + i * 4) = 0;
  RAM32(BLOCK + (BLOCK_WORDS - 2) * 4) = 0x03E00008u;
  RAM32(BLOCK + (BLOCK_WORDS - 1) * 4) = 0;

  /* Each case four times over, to see the spread. */
  for (u32 r = 0; r < 4; r++)
  {
    flush_icache();
    (void)timed(0xA0000000u | BLOCK);
    RAM32(0x100 + r * 16) = timed(0x80000000u | BLOCK);
    RAM32(0x104 + r * 16) = timed(0x80000000u | BLOCK);
    RAM32(0x108 + r * 16) = timed(BLOCK);
    RAM32(0x10C + r * 16) = timed(0xA0000000u | BLOCK);
  }
  for (u32 i = 0; i < 126; i++)
    RAM32(BLOCK2 + i * 4) = 0;
  RAM32(BLOCK2 + 126 * 4) = 0x03E00008u;
  RAM32(BLOCK2 + 127 * 4) = 0;
  flush_icache();
  (void)timed(0xA0000000u | BLOCK);
  RAM32(0x180) = timed(0x80000000u | BLOCK2);
  RAM32(0x184) = timed(0x80000000u | BLOCK2);
  RAM32(0) = 0xC0FFEEu;
  for (;;)
    ;
}
