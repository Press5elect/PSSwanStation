#pragma once
#include "bus.h"
#include "cpu_core.h"

namespace CPU {

// exceptions
void RaiseException(Exception excode);
void RaiseException(uint32_t CAUSE_bits, uint32_t EPC);

ALWAYS_INLINE bool HasPendingInterrupt()
{
  return g_state.cop0_regs.sr.IEc &&
         (((g_state.cop0_regs.cause.bits & g_state.cop0_regs.sr.bits) & (UINT32_C(0xFF) << 8)) != 0);
}

ALWAYS_INLINE void CheckForPendingInterrupt()
{
  if (HasPendingInterrupt())
    g_state.downcount = 0;
}

void DispatchInterrupt();
void UpdateDebugDispatcherFlag();

// icache stuff
ALWAYS_INLINE bool IsCachedAddress(VirtualMemoryAddress address)
{
  // KUSEG, KSEG0
  return (address >> 29) <= 4;
}
ALWAYS_INLINE uint32_t GetICacheLine(VirtualMemoryAddress address)
{
  return ((address >> 4) & 0xFFu);
}
ALWAYS_INLINE uint32_t GetICacheLineOffset(VirtualMemoryAddress address)
{
  return (address & (ICACHE_LINE_SIZE - 1));
}
// The tag holds the physical address (psx-spx, Memory Map, i-Cache, "Tag
// format"): KUSEG and KSEG0 reach the same physical code through the same tag.
ALWAYS_INLINE uint32_t GetICacheTagForAddress(VirtualMemoryAddress address)
{
  return (address & ICACHE_TAG_ADDRESS_MASK & 0x1FFFFFFFu);
}
ALWAYS_INLINE uint32_t GetICacheTagMaskForAddress(VirtualMemoryAddress address)
{
  static const uint32_t mask[4] = {ICACHE_TAG_ADDRESS_MASK | 1, ICACHE_TAG_ADDRESS_MASK | 2, ICACHE_TAG_ADDRESS_MASK | 4,
                              ICACHE_TAG_ADDRESS_MASK | 8};
  return mask[(address >> 2) & 0x03u];
}

ALWAYS_INLINE bool CompareICacheTag(VirtualMemoryAddress address)
{
  const uint32_t line = GetICacheLine(address);
  return ((g_state.icache_tags[line] & GetICacheTagMaskForAddress(address)) == GetICacheTagForAddress(address));
}

TickCount GetInstructionReadTicks(VirtualMemoryAddress address);
TickCount GetICacheFillTicks(VirtualMemoryAddress address);
uint32_t FillICache(VirtualMemoryAddress address);
void CheckAndUpdateICacheTags(uint32_t line_count, TickCount uncached_ticks);

ALWAYS_INLINE Segment GetSegmentForAddress(VirtualMemoryAddress address)
{
  switch ((address >> 29))
  {
    case 0x00: // KUSEG 0M-512M
    case 0x01: // KUSEG 512M-1024M
    case 0x02: // KUSEG 1024M-1536M
    case 0x03: // KUSEG 1536M-2048M
      return Segment::KUSEG;

    case 0x04: // KSEG0 - physical memory cached
      return Segment::KSEG0;

    case 0x05: // KSEG1 - physical memory uncached
      return Segment::KSEG1;

    case 0x06: // KSEG2
    case 0x07: // KSEG2
    default:
      break;
  }
  return Segment::KSEG2;
}

ALWAYS_INLINE PhysicalMemoryAddress VirtualAddressToPhysical(VirtualMemoryAddress address)
{
  return (address & PHYSICAL_MEMORY_ADDRESS_MASK);
}

// defined in bus.cpp - memory access functions which return false if an exception was thrown.
bool FetchInstruction();
bool FetchInstructionForInterpreterFallback();
bool SafeReadInstruction(VirtualMemoryAddress addr, uint32_t* value);
bool ReadMemoryByte(VirtualMemoryAddress addr, uint8_t* value);
bool ReadMemoryHalfWord(VirtualMemoryAddress addr, uint16_t* value);
bool ReadMemoryWord(VirtualMemoryAddress addr, uint32_t* value);
bool WriteMemoryByte(VirtualMemoryAddress addr, uint32_t value);
bool WriteMemoryHalfWord(VirtualMemoryAddress addr, uint32_t value);
bool WriteMemoryWord(VirtualMemoryAddress addr, uint32_t value);
void* GetDirectReadMemoryPointer(VirtualMemoryAddress address, MemoryAccessSize size, TickCount* read_ticks);
void* GetDirectWriteMemoryPointer(VirtualMemoryAddress address, MemoryAccessSize size);

ALWAYS_INLINE void AddGTETicks(TickCount ticks)
{
  g_state.gte_completion_tick = g_state.pending_ticks + ticks + 1;
}

ALWAYS_INLINE void StallUntilGTEComplete()
{
  g_state.pending_ticks =
    (g_state.gte_completion_tick > g_state.pending_ticks) ? g_state.gte_completion_tick : g_state.pending_ticks;
}

} // namespace CPU
