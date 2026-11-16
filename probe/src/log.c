// LoopyProbe: the SRAM log.
// Copyright (C) 2026 Derek Quenneville
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The layout is in probe.h. Each record's body is written before the count
// that covers it, so a run that loses power mid-way still decodes up to its
// last whole record (the pattern of loopy-umihara's lp_conlog.c). A log that
// reached its end is kept at the next boot, so turning the console off and
// on again does not lose it before it is dumped (loopy-prince's
// lp_proflog.c); it is erased only on request: a button held at power-on (main.c).

#include <string.h>

#include "probe.h"

#define SRAM ((volatile uint8_t *) PB_SRAM_ADDR)

static uint16_t count;

static void Put16(unsigned off, uint16_t v)
{
	SRAM[off] = (uint8_t) (v >> 8);
	SRAM[off + 1] = (uint8_t) v;
}

static void Put32(unsigned off, uint32_t v)
{
	Put16(off, (uint16_t) (v >> 16));
	Put16(off + 2, (uint16_t) v);
}

static int HaveMagic(void)
{
	return SRAM[0] == 'L' && SRAM[1] == 'M' && SRAM[2] == 'P' &&
	       SRAM[3] == 'B' && SRAM[4] == 0 && SRAM[5] == PB_LOG_VERSION;
}

int LogKeptComplete(void)
{
	return HaveMagic() && (SRAM[H_FLAGS] & LF_DONE);
}

uint16_t LogKeptRecords(void)
{
	return (uint16_t) (SRAM[H_COUNT] << 8 | SRAM[H_COUNT + 1]);
}

void LogStart(uint8_t flags)
{
	static const char date[] = __DATE__, time[] = __TIME__;
	uint8_t prev = 0;
	unsigned i;

	if (HaveMagic() && !(SRAM[H_FLAGS] & LF_DONE)) {
		// An unfinished run: start over, but say how far it got (a
		// hang on the console shows up as the phase it stopped in).
		prev = SRAM[H_PHASE];
		flags |= LF_PARTIAL;
	}

	// The magic goes last, once the rest is consistent.
	SRAM[0] = 0;
	for (i = 1; i < PB_SRAM_SIZE; ++i)
		SRAM[i] = 0;
	Put16(4, PB_LOG_VERSION);
	SRAM[H_FLAGS] = flags;
	SRAM[H_PREVPHASE] = prev;
	Put16(H_WCR3, BSC_WCR3);
	Put16(H_CAP, (uint16_t) PB_REC_CAP);
	for (i = 0; i < 12 && date[i]; ++i)
		SRAM[H_DATE + i] = (uint8_t) date[i];
	for (i = 0; i < 8 && time[i]; ++i)
		SRAM[H_TIME + i] = (uint8_t) time[i];
	count = 0;
	SRAM[1] = 'M';
	SRAM[2] = 'P';
	SRAM[3] = 'B';
	SRAM[0] = 'L';
}

void LogRec(uint8_t type, uint8_t sub, const uint16_t v[7])
{
	unsigned off, i;

	if (count >= PB_REC_CAP)
		return;
	off = PB_HDR_SIZE + (unsigned) count * PB_REC_SIZE;
	SRAM[off] = type;
	SRAM[off + 1] = sub;
	for (i = 0; i < 7; ++i)
		Put16(off + 2 + i * 2, v[i]);
	++count;
	Put16(H_COUNT, count);
}

uint16_t LogCount(void)
{
	return count;
}

void LogPhase(uint8_t phase)
{
	Put32(H_FRAMES, vblanks);
	SRAM[H_STEP] = 0;
	SRAM[H_TEST] = 0;
	SRAM[H_PHASE] = phase;
	LOG(R_MARK, phase, HI16(vblanks), LO16(vblanks));
}

void LogStep(uint8_t step)
{
	SRAM[H_STEP] = step;
}

void LogTest(uint8_t test)
{
	SRAM[H_TEST] = test;
}

void LogFlags(uint8_t set)
{
	SRAM[H_FLAGS] |= set;
}

// A CPU exception (boot/vectors.c): into the header, a red screen, a halt.
void PB_Exception(unsigned kind, const unsigned long *frame)
	__attribute__((noreturn));
void PB_Exception(unsigned kind, const unsigned long *frame)
{
	SRAM[H_EXC_KIND] = (uint8_t) kind;
	Put32(H_EXC_PC, frame[0]);
	Put32(H_EXC_SR, frame[1]);
	LogFlags(LF_EXCEPTION);
	VDP.BACKDROP_A = RGB555(31, 0, 0);
	for (;;)
		;
}
