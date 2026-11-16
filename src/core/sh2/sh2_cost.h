#pragma once

#include <cstdint>
#include <string>

// Console timing model ([timing] model = console). Stock LoopyMSE charges an
// instruction its fetch alone (3 cycles from the cartridge, 1 elsewhere) and
// nothing for a data access, a literal-pool load or a DMA. With this model on,
// every executed instruction is charged what a real console was measured to
// charge. The constants come from a memory benchmark photographed on
// hardware, and the rules below reproduce that photograph within 3 %:
//
//   - fetch: cartridge ROM and BIOS 3, work RAM 1 in the open 1 KB row and
//     3 in another, on-chip RAM nothing (it leaves the bus);
//   - data: work RAM reads 1 / 3 and writes 2 / 3 by row, a 32-bit access
//     two units (the bus is 16 bits), ROM and SRAM 3 a unit, on-chip 1, the
//     VDP by area (bitmap, tile, OAM, palette, registers; the bitmap's fast
//     mode, 0x0C05E000 bit 0, followed);
//   - an instruction takes max(states, fetch + data), a taken branch one
//     more fetch per extra state, a load used by the next instruction a
//     stall where the fetch is a single cycle;
//   - a DMA on the CPU's own controller stalls the CPU by the measured rate
//     a 16-bit unit for its source and destination.
//
// Work RAM's open row is shared by fetches, data and DMA. Interrupts run as
// code, so no flat interrupt share is added. DRAM refresh is not modelled.

namespace SH2::Cost
{

extern bool enabled;

// Forget the open row, the bitmap mode and any owed cycles (power-on).
void reset();

// Cycles a cartridge ROM access takes (fetches, literals and data alike):
// one state plus the long waits WCR3 sets for area 6 (3 at the BIOS's
// 0xA800, 2 at 0xA000), followed from the program's writes to WCR3. A
// what-if value given here overrides them; 0 = follow WCR3.
void set_cart_rom_cycles(double cycles);
void wcr3_written(double cycles);

// Price the fetch of the instruction at pc, moving the open row. Returns its
// cycles; *onchip is set when the code runs from on-chip RAM.
double fetch(uint32_t pc, bool* onchip);

// Data accesses between begin() and end() are summed for the instruction op.
void begin(uint16_t op);
double end();
bool collecting();
void access(uint32_t addr, int size, bool write, uint32_t value);

// The executed instruction's whole time, from its fetch and data cost.
// taken: a non-delayed branch went (BT/BF); delayed: it set up a delay slot.
double price(uint16_t op, double fetch_cost, bool onchip, double data, bool taken, bool delayed);

// A transfer of `units` items of `size` bytes by the CPU's DMA controller.
void dma(uint32_t src, uint32_t dst, int size, uint32_t units);
// Cycles owed outside any instruction (DMA started by a request), taken by
// the next one.
double take_pending();

// Exception entry: the states beyond its own accesses.
constexpr double EXCEPTION_STATES = 5.0;

// Cycles by what they were spent on, while the profile runs.
void split_enable(bool on);
void split_add_exec(double cycles);
std::string split_text();

}  // namespace SH2::Cost
