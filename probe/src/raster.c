// LoopyProbe: display artifacts from VDP accesses during the picture
// (question 4).
// Copyright (C) 2026 Derek Quenneville
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The author's ports saw dots on a console when they wrote the palette while
// a line was drawn (loopy-maniac's lp_video.c); the reference says palette
// writes are clean and palette reads and tile VRAM accesses (with a BG or
// OBJ layer on) snow. Each test hammers one kind of access through the
// whole active picture, from on-chip RAM (probes.s, pb_acc_*), writing the
// value already there so the picture itself does not change:
//
//   1  nothing (the control)
//   2  palette write, the entry bar 1 shows
//   3  palette write, entry 255 (nothing on screen uses it)
//   4  palette read, bar 1's entry
//   5  OAM write, sprite 127 (parked below the picture)
//   6  tile VRAM read, the last word (unused)
//   7  tile VRAM write, the same word
//
// The VDP's scanline capture (CAPTURE_CTRL mode 0: the displayed output,
// 15bpp) takes one line a frame while the accesses run: lines 60 (text and
// backdrop), 120 (the bars), 190 (the BG band) and 211 (the sprites), four
// frames each. Every capture is compared with the control's first capture
// of the same line. If the capture sees nothing, that is the result
// "capture did not see any"; each test also stays on screen for five
// seconds, labelled, for a photograph.

#include <string.h>

#include "probe.h"

#define ACC_DECL(n) extern const char pb_acc_##n[], pb_acc_##n##_end[]
ACC_DECL(none);
ACC_DECL(wr);
ACC_DECL(rd);

#define TESTS 7
#define CAP_FRAMES 4
#define SHOW_FRAMES 300

static const uint8_t cap_lines[4] = { 60, 120, 190, 211 };
static uint16_t control[4][256];

struct Test {
	const char *name;
	int kind;                       // 0 none, 1 write, 2 read
	volatile void *target;
};

static const struct Test tests[TESTS] = {
	{ "NO ACCESS (CONTROL)", 0, 0 },
	{ "PALETTE WRITE, BAR 1'S ENTRY", 1, &VDP.PALETTE[C_BAR0] },
	{ "PALETTE WRITE, UNUSED ENTRY 255", 1, &VDP.PALETTE[C_OFFSCREEN] },
	{ "PALETTE READ, BAR 1'S ENTRY", 2, &VDP.PALETTE[C_BAR0] },
	{ "OAM WRITE, SPRITE 127", 1, (volatile uint16_t *) &VDP.OAM[127] + 1 },
	{ "TILE VRAM READ", 2, &VDP.TILE_VRAM[OBJ_TILE_WORD] },
	{ "TILE VRAM WRITE", 1, &VDP.TILE_VRAM[OBJ_TILE_WORD] },
};

static Access Place(int kind)
{
	const char *s = kind == 1 ? pb_acc_wr : kind == 2 ? pb_acc_rd : pb_acc_none;
	const char *e = kind == 1 ? pb_acc_wr_end : kind == 2 ? pb_acc_rd_end
	              : pb_acc_none_end;

	memcpy(ONCHIP_FREE, s, (size_t) (e - s));
	return (Access) (void *) ONCHIP_FREE;
}

static void RunTest(int n)
{
	const struct Test *t = &tests[n - 1];
	Access acc = Place(t->kind);
	uint32_t value = 0, rounds = 0;
	unsigned li, f, total_diff = 0;

	LogStep((uint8_t) n);
	Title("RASTER ARTIFACTS");
	Textf(TEXT_X, ROW(1), 2, C_YELLOW, "TEST %d OF %d", n, TESTS);
	Line(3, C_INK, "%s", t->name);
	Line(4, C_GREY, "CAPTURING LINES 60 120 190 211");
	Present();
	WaitVblank();
	WaitVblank();           // in the blank, with the picture settled

	if (t->target)
		value = *(volatile uint16_t *) t->target;

	for (li = 0; li < 4; ++li) {
		unsigned diff_frames = 0, diff_total = 0, diff_max = 0;
		uint16_t first_x = 0xFFFF, first_ctl = 0, first_got = 0;

		for (f = 0; f <= CAP_FRAMES; ++f) {
			if (f > 0) {
				// The last frame's capture: compare it.
				unsigned x, diffs = 0;

				for (x = 0; x < 256; ++x) {
					uint16_t got = VDP.CAPTURE_DATA[x];

					if (n == 1 && f == 1) {
						control[li][x] = got;
						continue;
					}
					if (got != control[li][x]) {
						if (first_x == 0xFFFF) {
							first_x = (uint16_t) x;
							first_ctl = control[li][x];
							first_got = got;
						}
						++diffs;
					}
				}
				if (diffs)
					++diff_frames;
				diff_total += diffs;
				if (diffs > diff_max)
					diff_max = diffs;
			}
			if (f == CAP_FRAMES)
				break;
			VDP.CAPTURE_CTRL = CAPTURE_MODE_OUTPUT_15BPP | cap_lines[li];
			VDP.TRIGGER = TRIGGER_CAPTURE;
			WaitActive();
			rounds = acc(t->target, &VDP.VCOUNT, value);
			++vblanks;
		}
		LOG(R_RASTER, (uint8_t) n, cap_lines[li], (uint16_t) diff_frames,
		    (uint16_t) diff_total, (uint16_t) diff_max, first_x, first_ctl,
		    first_got);
		total_diff += diff_total;
		if (n == 1) {
			const uint16_t *c = control[li];

			LOG(R_RASTER_CTL, cap_lines[li], c[16], c[48], c[80], c[112],
			    c[144], c[176], c[208]);
		}
	}

	Line(4, total_diff ? C_RED : C_GREEN, "CAPTURE: %u PIXELS DIFFER",
	     total_diff);
	for (f = 0; f < SHOW_FRAMES; ++f) {
		if (f % 60 == 0) {
			Line(15, C_YELLOW, "PHOTOGRAPH THE SCREEN NOW: %u S",
			     (SHOW_FRAMES - f) / 60);
			Present();
		}
		WaitActive();
		rounds = acc(t->target, &VDP.VCOUNT, value);
		++vblanks;
	}
	LOG(R_RASTER_ACC, (uint8_t) n, (uint16_t) rounds, (uint16_t) t->kind,
	    HI16(t->target), LO16(t->target));
}

void TestRaster(void)
{
	int n;

	for (n = 1; n <= TESTS; ++n)
		RunTest(n);
}
