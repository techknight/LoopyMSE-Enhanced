// LoopyProbe: measures what LoopyMSE-Enhanced and the hardware reference
// disagree on, on a real Casio Loopy, and logs it to the cartridge's SRAM.
// Copyright (C) 2026 Derek Quenneville
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The run, about two minutes, by itself (README.md has the phase table):
// the frame period, HCOUNT/VCOUNT in both picture heights, fetch and memory
// access costs, display artifacts from VDP accesses during the picture, and
// the mouse. Every result goes to the SRAM log (log.c, decode_probe.py) and
// most also to the screen. The same ROM in the emulator gives the
// emulator's answers.
//
// A finished log is kept at the next boot, so a power cycle before the dump
// loses nothing; holding a mouse button (or any pad button) for three
// seconds on that screen erases it and runs again.

#include "probe.h"

#define BOOT_FRAMES 180

static void Kept(void)
{
	unsigned held = 0;

	Title("LOG COMPLETE");
	Line(2, C_INK, "THE SAVE HOLDS A FINISHED LOG,");
	Line(3, C_INK, "%u RECORDS. TURN OFF AND DUMP IT.", LogKeptRecords());
	Line(5, C_INK, "TO ERASE IT AND RUN AGAIN, HOLD A");
	Line(6, C_INK, "MOUSE BUTTON (OR ANY PAD BUTTON)");
	Line(7, C_INK, "FOR 3 SECONDS.");
	Present();
	for (;;) {
		WaitVblank();
		if (ButtonHeld()) {
			if (held % 60 == 0)
				Line(15, C_RED, "ERASING IN %u", 3 - held / 60);
			if (++held >= 180)
				return;
		} else if (held) {
			held = 0;
			ClearRows(ROW(15), ROW(15) + 8);
		}
		Present();
	}
}

static void Boot(void)
{
	unsigned f;

	WaitVblank();
	LogPhase(PH_BOOT);
	Title("");
	Textf(TEXT_X, ROW(1), 2, C_YELLOW, "LOOPYPROBE");
	Line(3, C_INK, "MEASURES THIS CONSOLE FOR THE");
	Line(4, C_INK, "LOOPYMSE-ENHANCED EMULATOR. IT RUNS");
	Line(5, C_INK, "BY ITSELF FOR ABOUT TWO MINUTES.");
	Line(6, C_INK, "HANDS OFF THE MOUSE UNTIL ASKED;");
	Line(7, C_INK, "PHOTOGRAPH THE SCREEN WHEN ASKED.");
	Line(8, C_GREY, "BUILT " __DATE__ " " __TIME__);
	Line(14, C_INK, "AT THE END: TURN OFF, DUMP THE SAVE,");
	Line(15, C_INK, "RUN DECODE_PROBE.PY ON IT.");
	for (f = 0; f < BOOT_FRAMES; ++f) {
		if (f % 60 == 0)
			Line(17, C_YELLOW, "STARTING IN %u", (BOOT_FRAMES - f) / 60);
		Present();
		WaitVblank();
	}
}

// A test's results stay up for two seconds, for a photograph.
static void Results(void)
{
	unsigned f;

	LogStep(STEP_RESULTS);
	for (f = 0; f < RESULT_FRAMES; ++f) {
		if (f % 60 == 0)
			Line(17, C_GREY, "NEXT TEST IN %u", (RESULT_FRAMES - f) / 60);
		Present();
		WaitVblank();
	}
}

static void Done(void)
{
	WaitVblank();
	LogFlags(LF_DONE);
	LogPhase(PH_DONE);
	Title("DONE");
	Textf(TEXT_X, ROW(1), 2, C_GREEN, "ALL DONE");
	Line(3, C_INK, "%u RECORDS IN THE SAVE.", LogCount());
	Line(5, C_INK, "TURN THE CONSOLE OFF, DUMP THE SAVE");
	Line(6, C_INK, "(.SAV) AND RUN DECODE_PROBE.PY ON IT.");
	Line(7, C_GREY, "THE LOG IS KEPT AT THE NEXT POWER-ON.");
	Present();
	for (;;)
		WaitVblank();
}

int main(void)
{
	// Nothing shares the bus with a measurement: interrupts stay masked
	// (boot/startup.s), the VDP's interrupt sources go off (ScreenInit),
	// and the DMA controller is stopped (the probe uses none).
	sys_setDmaEnabled(false);
	ClockInit();
	ScreenInit();
	ScreenScan(1, 1, 1);    // detects either device (loopy-maniac)

	{
		uint8_t flags = 0;

		if (LogKeptComplete()) {
			Kept();
			flags = LF_RESTARTED;
		}
		LogStart(flags);
	}

	Boot();

	WaitVblank();
	LogPhase(PH_FRAME);
	TestFrame();
	Results();

	WaitVblank();
	LogPhase(PH_HV240);
	TestHV(1);
	Results();

	WaitVblank();
	LogPhase(PH_HV224);
	TestHV(0);
	Results();

	WaitVblank();
	LogPhase(PH_MEM);
	TestMemory();
	Results();

	WaitVblank();
	LogPhase(PH_RASTER);
	TestRaster();

	WaitVblank();
	LogPhase(PH_MOUSE);
	Title("MOUSE");
	Line(2, C_INK, "LOOKING FOR THE MOUSE...");
	Present();
	if (MouseDetect()) {
		TestMouse();
	} else {
		Line(2, C_RED, "NO MOUSE: THE MOUSE TESTS ARE SKIPPED");
		Present();
		for (unsigned f = 0; f < 120; ++f)
			WaitVblank();
	}

	Done();
	return 0;
}
