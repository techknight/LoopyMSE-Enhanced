// LoopyProbe: the XN-110 mouse (question 5).
// Copyright (C) 2026 Derek Quenneville
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The emulator's model (from loopy-maniac's mouse probe on a console): the
// 12-bit delta counters wrap when left unread; with a mouse on the port the
// pad-matrix bits of 0x0C05D010/12/14 read constantly-changing garbage, bit
// 15 of 0x010 is presence and bits 12-14 mirror the buttons, active-low; no
// counts accumulate while the pad scan is armed too. The reference: the
// counters saturate at +2047/-2048, and all six bytes of 0x010/12/14 read
// bit 7 detect, bit 6 right button and bit 4 left (active-low), bits 3-0
// the encoders' raw gray code, which changes only when the mouse moves.
//
// The user has only the mouse, so every phase runs on a clock: it is
// published in the SRAM header, shows its instruction and a countdown for
// PROMPT_FRAMES vertical blanks, and then measures for a fixed window (the
// table below), so an emulator script can act each one out in step. The
// counters are read once at a window's start, to empty them. Phases (a)-(d)
// arm the mouse scan alone; (e) arms both, then the mouse alone again as
// the control.
//
//   phase  window  reads
//   11      120    (a) hands off: 010/12/14 and 050/52 every frame
//   12      120    (b) circles: the same
//   13-15    60    (c) left, right, both held: the same
//   16      180    (d) fast right: 050/52 every frame (calibration)
//   17      300        fast right: 050/52 once a second
//   18      180        fast left, every frame
//   19      300        fast left, once a second
//   20      180    (e) pad and mouse scan, moving: 050/52 every frame
//   21      180        mouse scan alone, the same motion

#include "probe.h"

#define W_IDLE 120
#define W_CIRCLES 120
#define W_BUTTON 60
#define W_CAL 180
#define W_OVF 300
#define W_SCAN 180
#define OVF_EVERY 60
// The live values are logged every frame but drawn every sixth, so a
// frame's work stays well inside the frame (the windows are counted in
// vertical blanks, and a missed one would stretch them).
#define SHOW_EVERY 6

int ButtonHeld(void)
{
	uint16_t g0 = VDP.IO_GAMEPAD[0];

	if (g0 & 0x8000)                        // mouse: left bit 12, right 14
		return (~g0 & 0x5000) != 0;
	if (g0 & 0x0001)                        // pad 1 present: any button
		return (READ_GAMEPAD1_RAW & 0x0FFE) != 0;
	return 0;
}

// With both scans armed for half a second (loopy-maniac's detection mode):
// bit 15 of 0x010 on most frames.
int MouseDetect(void)
{
	unsigned i, hits = 0;
	uint16_t g0 = 0, g1 = 0, g2 = 0;
	int present;

	ScreenScan(1, 1, 1);
	for (i = 0; i < 30; ++i) {
		WaitVblank();
		g0 = VDP.IO_GAMEPAD[0];
		g1 = VDP.IO_GAMEPAD[1];
		g2 = VDP.IO_GAMEPAD[2];
		if (g0 & 0x8000)
			++hits;
	}
	present = hits >= 15;
	ScreenScan(present, !present, 1);
	LOG(R_MOUSE_DET, 0, (uint16_t) present, g0, g1, g2, scan_mode,
	    (uint16_t) hits);
	if (present)
		LogFlags(LF_MOUSE);
	return present;
}

static void Status(void)
{
	Line(21, C_GREY, "010:%04X 012:%04X 014:%04X", VDP.IO_GAMEPAD[0],
	     VDP.IO_GAMEPAD[1], VDP.IO_GAMEPAD[2]);
}

// Publish the phase at the start of a frame, then count the prompt down.
// Returns at the vertical blank that starts the window.
static void Prompt(uint8_t phase, const char *what, const char *how,
                   const char *how2)
{
	unsigned f;

	WaitVblank();
	LogPhase(phase);
	Title("MOUSE");
	Textf(TEXT_X, ROW(1), 2, C_YELLOW, "%s", what);
	Line(4, C_INK, "%s", how);
	Line(5, C_INK, "%s", how2);
	for (f = 0; f < PROMPT_FRAMES; ++f) {
		if (f % 60 == 0)
			Line(15, C_YELLOW, "GET READY: STARTS IN %u",
			     (PROMPT_FRAMES - f) / 60);
		if (f % SHOW_EVERY == 0)
			Status();
		Present();
		WaitVblank();
	}
	Line(15, C_GREEN, "NOW!");
	Present();
}

// Empty the counters at a window's start; what had built up is logged.
static void Drain(uint8_t phase)
{
	uint16_t x = VDP.IO_MOUSEX, y = VDP.IO_MOUSEY;

	LOG(R_MOUSE_READ, phase, 0, x, y, 0);
}

static void Countdown(unsigned f, unsigned window)
{
	if (f % 60 == 0)
		Line(16, C_YELLOW, "%u S LEFT", (window - f + 59) / 60);
}

// (a)-(c): every register every frame.
static void Sample(uint8_t phase, unsigned window)
{
	unsigned f;

	Drain(phase);
	for (f = 0; f < window; ++f) {
		uint16_t g0, g1, g2, x, y;

		WaitVblank();
		g0 = VDP.IO_GAMEPAD[0];
		g1 = VDP.IO_GAMEPAD[1];
		g2 = VDP.IO_GAMEPAD[2];
		x = VDP.IO_MOUSEX;
		y = VDP.IO_MOUSEY;
		LOG(R_MOUSE_SMP, phase, (uint16_t) f, g0, g1, g2, x, y);
		Countdown(f, window);
		if (f % SHOW_EVERY == 0) {
			Line(21, C_GREY, "010:%04X 012:%04X 014:%04X", g0, g1, g2);
			Line(22, C_GREY, "050:%04X 052:%04X", x, y);
		}
		Present();
	}
}

static int S12(uint16_t v)
{
	return (int) ((int16_t) (uint16_t) (v << 4) >> 4);
}

static uint16_t Clamp16(int32_t v)
{
	return (uint16_t) (int16_t) (v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

// (d) calibration and (e): the counters every frame, three frames to a
// record, and the sums.
static void Deltas(uint8_t phase, unsigned window)
{
	uint16_t pack[6];
	uint32_t ax = 0, ay = 0;
	int32_t sx = 0, sy = 0;
	unsigned f, moved = 0;

	Drain(phase);
	for (f = 0; f < window; ++f) {
		uint16_t x, y;
		int dx, dy;

		WaitVblank();
		x = VDP.IO_MOUSEX;
		y = VDP.IO_MOUSEY;
		dx = S12(x);
		dy = S12(y);
		sx += dx;
		sy += dy;
		ax += (uint32_t) (dx < 0 ? -dx : dx);
		ay += (uint32_t) (dy < 0 ? -dy : dy);
		if (dx || dy)
			++moved;
		pack[(f % 3) * 2] = x;
		pack[(f % 3) * 2 + 1] = y;
		if (f % 3 == 2)
			LOG(R_MOUSE_DXY, phase, (uint16_t) (f - 2), pack[0], pack[1],
			    pack[2], pack[3], pack[4], pack[5]);
		Countdown(f, window);
		if (f % SHOW_EVERY == 0) {
			Line(21, C_GREY, "050:%04X 052:%04X", x, y);
			Line(22, C_INK, "SUM X %ld Y %ld", (long) sx, (long) sy);
		}
		Present();
	}
	LOG(R_MOUSE_SUM, phase, HI16(ax), LO16(ax), HI16(ay), LO16(ay),
	    Clamp16(sx), Clamp16(sy), (uint16_t) moved);
}

// (d) overflow: the counters left alone for a second at a time.
static void Blind(uint8_t phase, unsigned window)
{
	static const uint8_t rows[5] = { 6, 7, 8, 14, 17 };
	unsigned f, last = 0, reads = 0;

	Drain(phase);
	for (f = 1; f <= window; ++f) {
		WaitVblank();
		if (f % OVF_EVERY == 0) {
			uint16_t x = VDP.IO_MOUSEX, y = VDP.IO_MOUSEY;

			LOG(R_MOUSE_READ, phase, (uint16_t) f, x, y,
			    (uint16_t) (f - last));
			last = f;
			if (reads < 5)
				Line(rows[reads], C_INK, "READ %u: X %04X (%d) Y %04X",
				     reads + 1, x, S12(x), y);
			++reads;
		}
		Countdown(f - 1, window);
		Present();
	}
}

void TestMouse(void)
{
	Prompt(PH_IDLE, "HANDS OFF", "KEEP YOUR HANDS OFF THE MOUSE",
	       "(THE REGISTERS ARE READ IDLE)");
	Sample(PH_IDLE, W_IDLE);

	Prompt(PH_CIRCLES, "CIRCLES", "MOVE THE MOUSE IN CIRCLES",
	       "AND KEEP GOING UNTIL TOLD TO STOP");
	Sample(PH_CIRCLES, W_CIRCLES);

	Prompt(PH_LEFT, "HOLD LEFT", "STOP MOVING. PRESS AND HOLD",
	       "THE LEFT BUTTON");
	Sample(PH_LEFT, W_BUTTON);
	Prompt(PH_RIGHT, "HOLD RIGHT", "RELEASE LEFT. PRESS AND HOLD",
	       "THE RIGHT BUTTON");
	Sample(PH_RIGHT, W_BUTTON);
	Prompt(PH_BOTHBTN, "HOLD BOTH", "PRESS AND HOLD BOTH BUTTONS", "");
	Sample(PH_BOTHBTN, W_BUTTON);

	Prompt(PH_CAL_RIGHT, "FAST RIGHT", "RELEASE. MOVE FAST TO THE RIGHT,",
	       "LIFT, PUT DOWN LEFT, REPEAT");
	Deltas(PH_CAL_RIGHT, W_CAL);
	Prompt(PH_OVF_RIGHT, "FAST RIGHT", "KEEP MOVING FAST TO THE RIGHT,",
	       "LIFT, PUT DOWN LEFT, REPEAT");
	Blind(PH_OVF_RIGHT, W_OVF);
	Prompt(PH_CAL_LEFT, "FAST LEFT", "NOW MOVE FAST TO THE LEFT,",
	       "LIFT, PUT DOWN RIGHT, REPEAT");
	Deltas(PH_CAL_LEFT, W_CAL);
	Prompt(PH_OVF_LEFT, "FAST LEFT", "KEEP MOVING FAST TO THE LEFT,",
	       "LIFT, PUT DOWN RIGHT, REPEAT");
	Blind(PH_OVF_LEFT, W_OVF);

	ScreenScan(1, 1, 1);
	Prompt(PH_SCAN_BOTH, "MOVE AROUND", "MOVE THE MOUSE AROUND STEADILY",
	       "(PAD AND MOUSE SCAN BOTH ON)");
	Deltas(PH_SCAN_BOTH, W_SCAN);
	ScreenScan(1, 0, 1);
	Prompt(PH_SCAN_MOUSE, "MOVE AROUND", "THE SAME AGAIN, STEADILY",
	       "(MOUSE SCAN ALONE)");
	Deltas(PH_SCAN_MOUSE, W_SCAN);
}
