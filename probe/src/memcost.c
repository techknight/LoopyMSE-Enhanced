// LoopyProbe: instruction fetch and memory access costs (question 3).
// Copyright (C) 2026 Derek Quenneville
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The reference says a BIOS fetch costs 1 cycle (the BIOS is the CPU's own
// 32-bit ROM) and a work RAM write within the open 1 KB row 1 cycle; the
// emulator's console timing model, from the author's earlier benchmark
// (loopy-attorney BENCH=mem), charges 3 and 2. Each probe (probes.s) is
// timed on ITU1 at two loop counts, five times each; the decoder takes the
// difference of the fastest times, which cancels the call and the timer
// reads, divided by the difference in passes: cycles a pass. Everything
// runs with interrupts masked and nothing else on the bus but the DRAM
// refresh (logged with the bus registers).
//
//   id  probe   code in    data
//    1  nop16   on-chip    -          16 nops a pass: the loop's baseline
//    2  nop32   on-chip    -          (nop32 - nop16) / 16 = one fetch
//    3  nop16   work RAM
//    4  nop32   work RAM
//    5  nop16   cartridge
//    6  nop32   cartridge
//    7  rd16    on-chip    work RAM, one address (the open row)
//    8  wr16    on-chip    work RAM, one address
//    9  rd16x2  on-chip    work RAM, two rows alternately
//   10  wr16x2  on-chip    work RAM, two rows alternately
//   11  rd16    on-chip    on-chip RAM
//   12  rd16    on-chip    cartridge ROM
//   13  jsr8    on-chip    -> rts;nop in on-chip RAM
//   14  jsr8    on-chip    -> rts;nop in work RAM
//   15  jsr8    on-chip    -> rts;nop in the cartridge
//   16  jsr8    on-chip    -> rts;nop in the BIOS at 0x1158 (32-bit aligned)
//   17  jsr8    on-chip    -> rts;nop in the BIOS at 0x1652 (not aligned)
//
// The BIOS's two `rts; nop` (loopy_bios_disassembly.txt): 0x1158 ends
// _B_SplitLongs, 0x1652 is the return _B_Itu3Delay runs into. Both return
// at once and touch nothing.

#include <string.h>

#include "probe.h"

#define PROBE_DECL(n) extern const char pb_##n[], pb_##n##_end[]
PROBE_DECL(nop16);
PROBE_DECL(nop32);
PROBE_DECL(rd16);
PROBE_DECL(wr16);
PROBE_DECL(rd16x2);
PROBE_DECL(wr16x2);
PROBE_DECL(jsr8);
PROBE_DECL(stub);

enum { AT_ONCHIP = 1, AT_DRAM = 2, AT_ROM = 3 };

#define BIOS_RTS_ALIGNED   ((volatile void *) 0x1158u)
#define BIOS_RTS_UNALIGNED ((volatile void *) 0x1652u)
#define CART_DATA          ((volatile void *) 0x0E000100u)   // the vector table
#define N1 32u
#define N2 160u
#define TRIES 5

// Copies of the code: a probe and a stub in work RAM, the same in on-chip
// RAM (after .onchip), and a data buffer a DRAM row (1 KB) apart, placed at
// run time because the linker script aligns .bss to 4 only.
static uint32_t code_dram[64];
static uint32_t stub_dram[2];
static uint8_t dram_area[3072];

#define ONCHIP_PROBE ((void *) ONCHIP_FREE)
#define ONCHIP_STUB  ((void *) (ONCHIP_FREE + 0x80))
#define ONCHIP_DATA  ((volatile void *) (ONCHIP_FREE + 0x90))

static Probe Place(const char *start, const char *end, int where)
{
	size_t n = (size_t) (end - start);

	if (where == AT_DRAM) {
		memcpy(code_dram, start, n);
		return (Probe) (void *) code_dram;
	}
	if (where == AT_ONCHIP) {
		memcpy(ONCHIP_PROBE, start, n);
		return (Probe) ONCHIP_PROBE;
	}
	return (Probe) (const void *) start;
}

static void Measure(uint8_t id, const char *start, const char *end, int where,
                    volatile void *a, volatile void *b, uint32_t v)
{
	Probe p = Place(start, end, where);
	uint16_t lo[2] = { 0xFFFF, 0xFFFF }, hi[2] = { 0, 0 };
	static const uint32_t n[2] = { N1, N2 };
	int i, k;

	for (i = 0; i < TRIES; ++i)
		for (k = 0; k < 2; ++k) {
			uint16_t t0 = CYCLES(), t;

			p(a, n[k], b, v);
			t = (uint16_t) (CYCLES() - t0);
			if (t < lo[k])
				lo[k] = t;
			if (t > hi[k])
				hi[k] = t;
		}
	LOG(R_MEM, id, N1, lo[0], hi[0], N2, lo[1], hi[1], (uint16_t) where);
	Line(1 + (id - 1) % 8, C_INK, "%2u: %5u CYCLES / %u PASSES", id,
	     (unsigned) (uint16_t) (lo[1] - lo[0]), (unsigned) (N2 - N1));
}

void TestMemory(void)
{
	volatile uint16_t *row_a =
	    (volatile uint16_t *) (((uintptr_t) dram_area + 1023u) & ~(uintptr_t) 1023u);
	volatile uint16_t *row_b = row_a + 512;          // 1 KB on: the next row
	volatile void *rom = CART_DATA;

	Title("FETCH AND MEMORY COSTS");
	Present();

	LOG(R_BSC, 0, BSC_BCR, BSC_WCR1, BSC_WCR2, BSC_WCR3, BSC_DCR, BSC_PCR,
	    BSC_RCR);
	LOG(R_BSC, 1, BSC_RTCSR, BSC_RTCNT, BSC_RTCOR, VDP.MODE, VDP.UNK_5E000,
	    irq_ctrl_boot, DMAC_DMAOR);
	LOG(R_MEMINFO, 0, HI16(ONCHIP_FREE), LO16(ONCHIP_FREE), HI16(row_a),
	    LO16(row_a));
	if (ONCHIP_FREE + 0x94 > ONCHIP_END) {
		Line(2, C_RED, "NO ROOM IN ON-CHIP RAM: SKIPPED");
		Present();
		return;
	}

	// The stubs: one in each place the BIOS is compared with.
	memcpy(ONCHIP_STUB, pb_stub, 4);
	memcpy(stub_dram, pb_stub, 4);
	*(volatile uint16_t *) ONCHIP_DATA = 0x1234;

	Measure(1, pb_nop16, pb_nop16_end, AT_ONCHIP, 0, 0, 0);
	Measure(2, pb_nop32, pb_nop32_end, AT_ONCHIP, 0, 0, 0);
	Measure(3, pb_nop16, pb_nop16_end, AT_DRAM, 0, 0, 0);
	Measure(4, pb_nop32, pb_nop32_end, AT_DRAM, 0, 0, 0);
	Measure(5, pb_nop16, pb_nop16_end, AT_ROM, 0, 0, 0);
	Measure(6, pb_nop32, pb_nop32_end, AT_ROM, 0, 0, 0);
	Present();
	Measure(7, pb_rd16, pb_rd16_end, AT_ONCHIP, row_a, 0, 0);
	Measure(8, pb_wr16, pb_wr16_end, AT_ONCHIP, row_a, 0, 0x5A5A);
	Measure(9, pb_rd16x2, pb_rd16x2_end, AT_ONCHIP, row_a, row_b, 0);
	Measure(10, pb_wr16x2, pb_wr16x2_end, AT_ONCHIP, row_a, row_b, 0x5A5A);
	Measure(11, pb_rd16, pb_rd16_end, AT_ONCHIP, ONCHIP_DATA, 0, 0);
	Measure(12, pb_rd16, pb_rd16_end, AT_ONCHIP, rom, 0, 0);
	Present();
	Measure(13, pb_jsr8, pb_jsr8_end, AT_ONCHIP, ONCHIP_STUB, 0, 0);
	Measure(14, pb_jsr8, pb_jsr8_end, AT_ONCHIP, stub_dram, 0, 0);
	Measure(15, pb_jsr8, pb_jsr8_end, AT_ONCHIP, (volatile void *) pb_stub, 0, 0);
	Measure(16, pb_jsr8, pb_jsr8_end, AT_ONCHIP, BIOS_RTS_ALIGNED, 0, 0);
	Measure(17, pb_jsr8, pb_jsr8_end, AT_ONCHIP, BIOS_RTS_UNALIGNED, 0, 0);
	Present();
}
