// LoopyProbe: the frame period and the raster counters (questions 1 and 2).
// Copyright (C) 2026 Derek Quenneville
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The clock is ITU channel 1 counting CPU cycles (phi/1), free-running and
// never cleared: 16 bits, so a span is only safe up to 65,535 cycles (4.1 ms)
// unless the code reading it accumulates the differences as it goes, which
// the frame timer does. Interrupts stay masked for the whole run (no tick
// interrupt can be merged or lost; loopy-umihara's lp_conlog.c met that in
// bios_vsync), and the probe never calls bios_vsync: it polls VCOUNT, whose
// bit 8 is set through the vertical blank (the BIOS's own vsync tests the
// same bit).
//
// Channel 1, as in the author's other ports: ITU0 is the BIOS's music timer
// and ITU2-4 its printer's.

#include <string.h>

#include "probe.h"

#define ITU1_START 0x02

uint32_t vblanks;

void ClockInit(void)
{
	ITU_TSTR &= (uint8_t) ~ITU1_START;
	ITU_TIER1 = 0;          // no interrupts
	ITU_TIOR1 = 0;          // compare matches drive no pin
	ITU_TCR1 = 0;           // phi/1, rising edge, never cleared
	ITU_GRA1 = 0xFFFF;
	ITU_GRB1 = 0xFFFF;
	ITU_TCNT1 = 0;
	ITU_TSTR |= ITU1_START;
}

void WaitVblank(void)
{
	while (IN_VBLANK())
		;
	while (!IN_VBLANK())
		;
	++vblanks;
}

void WaitActive(void)
{
	while (IN_VBLANK())
		;
}

// ---------------------------------------------------------------------------
// Question 1: the frame period.
//
// From one vertical blank's start to the start of the `frames`th after it,
// with the cycle count accumulated as it goes (so no wrap is lost), the
// VCOUNT changes seen on the way (lines), and the cycles spent in the
// blank. Runs from on-chip RAM so its loop is short and steady: a pass is a
// VCOUNT read, a TCNT read and a few register operations, and the edge is
// seen within one pass.
// ---------------------------------------------------------------------------

struct FrameTime {
	uint32_t cycles, blank, lines;
};

__attribute__((section(".onchip"), noinline))
static void TimeFrames(unsigned frames, struct FrameTime *out)
{
	volatile uint16_t *vc = &VDP.VCOUNT;
	volatile uint16_t *tc = &ITU_TCNT1;
	uint32_t total = 0, blank = 0, lines = 0;
	uint16_t v, vp, t, tl;

	do
		v = *vc;
	while (v & 0x100);
	do
		v = *vc;
	while (!(v & 0x100));
	tl = *tc;
	vp = v & 0x1FF;
	while (frames) {
		uint16_t dt;

		v = *vc & 0x1FF;        // 9 bits; the rest is not promised
		t = *tc;
		dt = (uint16_t) (t - tl);
		tl = t;
		total += dt;
		if (vp & 0x100)
			blank += dt;
		if (v != vp) {
			++lines;
			if ((v & 0x100) && !(vp & 0x100))
				--frames;
			vp = v;
		}
	}
	out->cycles = total;
	out->blank = blank;
	out->lines = lines;
}

static void FrameRecord(unsigned frames, unsigned rep)
{
	struct FrameTime ft;

	TimeFrames(frames, &ft);
	vblanks += frames + 1;
	LOG(R_FRAME, (uint8_t) frames, HI16(ft.cycles), LO16(ft.cycles),
	    HI16(ft.blank), LO16(ft.blank), (uint16_t) ft.lines,
	    (uint16_t) rep);
	Line(2 + rep, C_INK, "%2u FRAMES: %lu CYCLES, %lu LINES", frames,
	     (unsigned long) ft.cycles, (unsigned long) ft.lines);
	Present();
}

void TestFrame(void)
{
	unsigned i;

	Title("FRAME PERIOD");
	Line(1, C_GREY, "VBLANK TO VBLANK, IN CPU CYCLES (ITU1)");
	Present();
	for (i = 0; i < 4; ++i)
		FrameRecord(1, i);
	FrameRecord(8, 4);
	FrameRecord(8, 5);
	FrameRecord(64, 6);
}

// ---------------------------------------------------------------------------
// Question 2: HCOUNT and VCOUNT.
//
// pb_hvtrace (probes.s, on-chip RAM) samples VCOUNT, HCOUNT and TCNT1 into
// work RAM as fast as it can, about every 25 cycles (8 HCOUNT steps), from
// four lines before the end of the active picture for 1.2 frames, so the
// trace holds both ends of the vertical blank. The trace is then reduced to
// records: which HCOUNT values occur at all, where HCOUNT jumps and wraps,
// where VCOUNT changes relative to HCOUNT, the VCOUNT values and their odd
// transitions, the line period, and a few raw samples around the jump and
// the start of the blank.
// ---------------------------------------------------------------------------

#define TRACE_N 12288
static uint16_t trace[TRACE_N * 3];     // V, H, TCNT per sample
static uint8_t seen_h[64], seen_v[64];

extern void pb_hvtrace(uint16_t *buf, uint32_t n, volatile uint16_t *hcount,
                       volatile uint16_t *tcnt);

#define SEEN(set, x) ((set)[(x) >> 3] & (1u << ((x) & 7)))

static unsigned Runs(uint8_t sub, const uint8_t *set, unsigned tolerance,
                     uint8_t type)
{
	int lo = -1, last = -1;
	unsigned x, seen = 0, runs = 0;

	for (x = 0; x <= 0x200; ++x) {
		int on = x < 0x200 && SEEN(set, x);

		if (on) {
			if (lo < 0 || (int) x - last > (int) tolerance) {
				if (lo >= 0 && runs < 8) {
					LOG(type, sub, (uint16_t) lo, (uint16_t) last,
					    (uint16_t) seen);
					++runs;
				}
				lo = (int) x;
				seen = 0;
			}
			last = (int) x;
			++seen;
		}
	}
	if (lo >= 0 && runs < 8) {
		LOG(type, sub, (uint16_t) lo, (uint16_t) last, (uint16_t) seen);
		++runs;
	}
	return runs;
}

static void Raw(uint8_t sub, unsigned at)
{
	unsigned i;

	if (at < 4)
		at = 4;
	if (at + 6 > TRACE_N)
		at = TRACE_N - 6;
	for (i = at - 4; i < at + 6; i += 2) {
		const uint16_t *s = &trace[i * 3];

		LOG(R_HV_RAW, sub, (uint16_t) i, s[0], s[1], s[2], s[3], s[4],
		    s[5]);
	}
}

static void Analyse(uint8_t sub)
{
	unsigned i, distinct_h = 0, distinct_v = 0, mid = 0;
	unsigned hmin = 0x1FF, hmax = 0, gap = 0, gap_lo = 0, gap_hi = 0;
	unsigned fwd = 0, fwd_before = 0, fwd_after = 0x1FF;
	unsigned wraps = 0, wrap_before = 0, wrap_after = 0x1FF;
	unsigned vchg = 0, odd = 0, raw_jumps = 0, raw_wraps = 0;
	unsigned inc_hb_min = 0x1FF, inc_hb_max = 0, inc_ha_min = 0x1FF,
	         inc_ha_max = 0;
	unsigned first_jump = 0, first_blank = 0, lines = 0;
	uint32_t line_sum = 0;
	uint16_t line_min = 0xFFFF, line_max = 0, t_change = 0;
	int have_change = 0;
	int last = -1;

	memset(seen_h, 0, sizeof seen_h);
	memset(seen_v, 0, sizeof seen_v);
	for (i = 0; i < TRACE_N; ++i) {
		unsigned v = trace[i * 3] & 0x1FF, h = trace[i * 3 + 1] & 0x1FF;

		seen_h[h >> 3] |= (uint8_t) (1u << (h & 7));
		seen_v[v >> 3] |= (uint8_t) (1u << (v & 7));
	}

	for (i = 1; i < TRACE_N; ++i) {
		const uint16_t *p = &trace[(i - 1) * 3], *s = &trace[i * 3];
		unsigned vb = p[0] & 0x1FF, hb = p[1] & 0x1FF;
		unsigned va = s[0] & 0x1FF, ha = s[1] & 0x1FF;
		uint16_t apart = (uint16_t) (s[2] - p[2]);

		// HCOUNT: a jump forward of more than 24 steps (the sampling is
		// about 8), or a step back.
		if (ha > hb + 24) {
			++fwd;
			if (hb > fwd_before)
				fwd_before = hb;
			if (ha < fwd_after)
				fwd_after = ha;
			if (!first_jump)
				first_jump = i;
			if (raw_jumps < 4) {
				LOG(R_HV_HJRAW, sub, 1, (uint16_t) vb, (uint16_t) va,
				    (uint16_t) hb, (uint16_t) ha, apart);
				++raw_jumps;
			}
		} else if (ha < hb) {
			++wraps;
			if (hb > wrap_before)
				wrap_before = hb;
			if (ha < wrap_after)
				wrap_after = ha;
			if (raw_wraps < 4) {
				LOG(R_HV_HJRAW, sub, 2, (uint16_t) vb, (uint16_t) va,
				    (uint16_t) hb, (uint16_t) ha, apart);
				++raw_wraps;
			}
		}

		// VCOUNT: where in the line it changes, and every change that
		// is not a +1.
		if (va != vb) {
			++vchg;
			if (va == ((vb + 1) & 0x1FF)) {
				if (hb < inc_hb_min)
					inc_hb_min = hb;
				if (hb > inc_hb_max)
					inc_hb_max = hb;
				if (ha < inc_ha_min)
					inc_ha_min = ha;
				if (ha > inc_ha_max)
					inc_ha_max = ha;
			} else {
				if (odd < 8)
					LOG(R_HV_VJUMP, sub, (uint16_t) vb, (uint16_t) va,
					    (uint16_t) hb, (uint16_t) ha);
				++odd;
			}
			if ((va & 0x100) && !(vb & 0x100) && !first_blank)
				first_blank = i;
			if (have_change) {
				uint16_t len = (uint16_t) (s[2] - t_change);

				line_sum += len;
				++lines;
				if (len < line_min)
					line_min = len;
				if (len > line_max)
					line_max = len;
			}
			t_change = s[2];
			have_change = 1;
		}
	}

	for (i = 0; i < 0x200; ++i) {
		if (SEEN(seen_v, i))
			++distinct_v;
		if (!SEEN(seen_h, i))
			continue;
		++distinct_h;
		if (i < hmin)
			hmin = i;
		hmax = i;
		if (i >= 0x102 && i <= 0x1AB)
			++mid;
		if (last >= 0 && i - (unsigned) last > gap) {
			gap = i - (unsigned) last;
			gap_lo = (unsigned) last;
			gap_hi = i;
		}
		last = (int) i;
	}

	LOG(R_HV_SUM, sub, TRACE_N, (uint16_t) distinct_h, (uint16_t) hmin,
	    (uint16_t) hmax, (uint16_t) gap_lo, (uint16_t) gap_hi,
	    (uint16_t) mid);
	Runs(sub, seen_h, 16, R_HV_HRUN);
	LOG(R_HV_HJUMP, sub, (uint16_t) fwd, (uint16_t) fwd_before,
	    (uint16_t) fwd_after, (uint16_t) wraps, (uint16_t) wrap_before,
	    (uint16_t) wrap_after);
	LOG(R_HV_VSUM, sub, (uint16_t) vchg, (uint16_t) inc_hb_min,
	    (uint16_t) inc_hb_max, (uint16_t) inc_ha_min, (uint16_t) inc_ha_max,
	    (uint16_t) distinct_v, (uint16_t) odd);
	Runs(sub, seen_v, 1, R_HV_VRUN);
	LOG(R_HV_LINE, sub, (uint16_t) lines, HI16(line_sum), LO16(line_sum),
	    line_min, line_max);
	Raw(sub, first_jump);
	Raw(sub, first_blank);

	Line(2, C_INK, "H SEEN %u VALUES %03X-%03X", distinct_h, hmin, hmax);
	Line(3, C_INK, "WIDEST GAP %03X..%03X, %u IN 102-1AB", gap_lo, gap_hi,
	     mid);
	Line(4, C_INK, "H JUMPS %u: %03X->%03X  WRAPS %u", fwd, fwd_before,
	     fwd_after, wraps);
	Line(5, C_INK, "V +1 AT H %03X-%03X -> %03X-%03X", inc_hb_min,
	     inc_hb_max, inc_ha_min, inc_ha_max);
	Line(6, C_INK, "V VALUES %u, ODD STEPS %u", distinct_v, odd);
	if (lines)
		Line(7, C_INK, "LINE %lu.%02lu CYCLES",
		     (unsigned long) (line_sum / lines),
		     (unsigned long) (line_sum % lines * 100 / lines));
}

// lines240: the 240-line mode (else 224). The mode is switched in the blank
// and given two frames to settle; the trace starts four lines before the
// last active one.
void TestHV(int lines240)
{
	uint8_t sub = lines240 ? 240 : 224;
	unsigned start = sub - 4;
	unsigned spins = 0;

	Title(lines240 ? "HCOUNT/VCOUNT, 240 LINES" : "HCOUNT/VCOUNT, 224 LINES");
	Present();
	WaitVblank();
	ScreenScan(scan_mode & CONTROL_MODE_MOUSE, scan_mode & CONTROL_MODE_GAMEPAD,
	           lines240);
	WaitVblank();
	WaitVblank();
	WaitVblank();

	// The start line, with a limit: a console whose VCOUNT never shows it
	// is itself a result.
	while ((VDP.VCOUNT & 0x1FF) != start)
		if (++spins > 400000u)
			break;
	if (spins > 400000u)
		LOG(R_HV_FAIL, sub, (uint16_t) start, VDP.VCOUNT);
	pb_hvtrace(trace, TRACE_N, &VDP.HCOUNT, &ITU_TCNT1);
	Analyse(sub);

	WaitVblank();
	ScreenScan(scan_mode & CONTROL_MODE_MOUSE, scan_mode & CONTROL_MODE_GAMEPAD,
	           1);
	WaitVblank();
	Present();
}
