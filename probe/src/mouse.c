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
// Whoever is at the console has only the mouse to answer with. Each phase is
// published in the SRAM header and shows its instruction until a mouse
// button is pressed and let go (STEP_PRESS); then, with every button up, a
// countdown of COUNTDOWN_FRAMES vertical blanks (STEP_GETREADY), and then a
// window of a fixed length (STEP_MEASURE; the table below), so an emulator
// script can act each one out in step. The counters are read once at a
// window's start, to empty them. Phases (a)-(d) arm the mouse scan alone;
// (e) arms both, then the mouse alone again as the control.
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

// Any button, by the presence bits of 0x010: a mouse's from 0x050 (left bit
// 12, right bit 14, active-low; the read also empties the X counter, which
// no window is open to care about), pad 1's from the pad registers.
int ButtonHeld(void)
{
	uint16_t g0 = VDP.IO_GAMEPAD[0];

	if (g0 & 0x8000)
		return (~VDP.IO_MOUSEX & 0x5000) != 0;
	if (g0 & 0x0001)
		return (READ_GAMEPAD1_RAW & 0x0FFE) != 0;
	return 0;
}

int PressStep(struct Press *p)
{
	int held = ButtonHeld();

	switch (p->state) {
	case 0:                                 // let go of anything held
		if (held) {
			LogStep(STEP_RELEASE);
			return 0;
		}
		p->state = 1;
		LogStep(STEP_PRESS);
		return 0;
	case 1:                                 // a press
		if (held)
			p->state = 2;
		return 0;
	default:                                // and its release
		if (held)
			return 0;
		p->state = 0;
		return 1;
	}
}

void WaitPress(void)
{
	struct Press p = { 0 };

	Present();
	do
		WaitVblank();
	while (!PressStep(&p));
	LogStep(0);
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

// Publish the phase at the start of a frame and show the instruction until
// a mouse button is pressed and let go; then count down, the instruction
// still up. Returns at the vertical blank that starts the window, the
// COUNTDOWN_FRAMES-th after the one STEP_GETREADY was published at.
static void Prompt(uint8_t phase, const char *what, const char *how,
                   const char *how2)
{
	struct Press p = { 0 };
	unsigned f;

	WaitVblank();
	LogPhase(phase);
	Title("MOUSE");
	Textf(TEXT_X, ROW(1), 2, C_YELLOW, "%s", what);
	Line(4, C_INK, "%s", how);
	Line(5, C_INK, "%s", how2);
	Line(14, C_YELLOW, "WHEN YOU ARE READY, PRESS AND");
	Line(15, C_YELLOW, "RELEASE A MOUSE BUTTON");
	Present();
	for (f = 0;; ++f) {
		WaitVblank();
		if (PressStep(&p))
			break;
		if (f % SHOW_EVERY == 0)
			Status();
		Present();
	}

	ClearRows(ROW(14), ROW(14) + 8);
	WaitVblank();
	LogStep(STEP_GETREADY);
	for (f = 0; f < COUNTDOWN_FRAMES; ++f) {
		if (f % 60 == 0)
			Line(15, C_YELLOW, "GET READY: STARTS IN %u",
			     (COUNTDOWN_FRAMES - f) / 60);
		if (f % SHOW_EVERY == 0)
			Status();
		Present();
		WaitVblank();
	}
	LogStep(STEP_MEASURE);
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
	// Each instruction is for after the start click: the measurement
	// begins as the countdown ends, so motion and held buttons start
	// during the countdown.
	Prompt(PH_IDLE, "HANDS OFF", "AFTER THE CLICK, TAKE YOUR HAND OFF",
	       "THE MOUSE UNTIL THE NEXT SCREEN");
	Sample(PH_IDLE, W_IDLE);

	Prompt(PH_CIRCLES, "CIRCLES", "DURING THE COUNTDOWN, START MOVING IN",
	       "CIRCLES; KEEP GOING TO THE NEXT SCREEN");
	Sample(PH_CIRCLES, W_CIRCLES);

	Prompt(PH_LEFT, "HOLD LEFT", "DURING THE COUNTDOWN, PRESS AND HOLD",
	       "LEFT, STILL, UNTIL THE NEXT SCREEN");
	Sample(PH_LEFT, W_BUTTON);
	Prompt(PH_RIGHT, "HOLD RIGHT", "DURING THE COUNTDOWN, PRESS AND HOLD",
	       "RIGHT, STILL, UNTIL THE NEXT SCREEN");
	Sample(PH_RIGHT, W_BUTTON);
	Prompt(PH_BOTHBTN, "HOLD BOTH", "DURING THE COUNTDOWN, PRESS AND HOLD",
	       "BOTH BUTTONS UNTIL THE NEXT SCREEN");
	Sample(PH_BOTHBTN, W_BUTTON);

	Prompt(PH_CAL_RIGHT, "FAST RIGHT", "DURING THE COUNTDOWN, START MOVING FAST",
	       "RIGHT; LIFT, PUT DOWN LEFT, REPEAT");
	Deltas(PH_CAL_RIGHT, W_CAL);
	Prompt(PH_OVF_RIGHT, "FAST RIGHT", "THE SAME AGAIN: FAST TO THE RIGHT,",
	       "LIFT, PUT DOWN LEFT, REPEAT");
	Blind(PH_OVF_RIGHT, W_OVF);
	Prompt(PH_CAL_LEFT, "FAST LEFT", "DURING THE COUNTDOWN, START MOVING FAST",
	       "LEFT; LIFT, PUT DOWN RIGHT, REPEAT");
	Deltas(PH_CAL_LEFT, W_CAL);
	Prompt(PH_OVF_LEFT, "FAST LEFT", "THE SAME AGAIN: FAST TO THE LEFT,",
	       "LIFT, PUT DOWN RIGHT, REPEAT");
	Blind(PH_OVF_LEFT, W_OVF);

	ScreenScan(1, 1, 1);
	Prompt(PH_SCAN_BOTH, "MOVE AROUND", "DURING THE COUNTDOWN, START MOVING THE",
	       "MOUSE AROUND STEADILY (PAD SCAN ON TOO)");
	Deltas(PH_SCAN_BOTH, W_SCAN);
	ScreenScan(1, 0, 1);
	Prompt(PH_SCAN_MOUSE, "MOVE AROUND", "THE SAME AGAIN, STEADILY",
	       "(MOUSE SCAN ALONE)");
	Deltas(PH_SCAN_MOUSE, W_SCAN);
}
