// LoopyProbe: measures the Casio Loopy behaviour LoopyMSE-Enhanced and the
// hardware reference disagree on, and logs it to cartridge SRAM.
// Copyright (C) 2026 Derek Quenneville
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Everything the parts of the probe share: the SRAM log's layout, the phase
// numbers an automation script waits on, the record types, and the screen,
// clock and log calls. decode_probe.py mirrors the numbers in this file.

#ifndef PROBE_H
#define PROBE_H

#include <stdint.h>

#include "loopy.h"

// ---------------------------------------------------------------------------
// SRAM log (src/log.c). Big-endian, byte writes (the SRAM is 8 bits wide).
//
//   0x00  "LMPB"
//   0x04  u16 version (1)
//   0x06  u16 records written; bumped after each record is complete
//   0x08  u8  current phase (PH_*); an automation script waits on it
//   0x09  u8  flags (LF_*)
//   0x0A  u8  the phase a partial log found at boot had reached (else 0)
//   0x0B  u8  step: what the probe is waiting for (STEP_*), 0 while working
//   0x0C  u16 WCR3 as the BIOS left it
//   0x0E  u16 record capacity
//   0x10  char[12] build date (__DATE__)
//   0x1C  char[8]  build time (__TIME__)
//   0x24  u32 vertical blanks the probe has counted, at the last phase change
//   0x28  u8  CPU exception vector (4, 6, 9, 10) if one happened
//   0x2C  u32 the exception's PC, 0x30 u32 its SR
//   0x34  u8  the raster test on screen (1-7), else 0
//   0x40  records of PB_REC_SIZE bytes: u8 type, u8 sub, 7 x u16 (a..g)
// ---------------------------------------------------------------------------

#define PB_SRAM_ADDR    0x02000000u
#define PB_LOG_VERSION  1
#define PB_HDR_SIZE     0x40u
#define PB_REC_SIZE     16u
#define PB_REC_CAP      ((PB_SRAM_SIZE - PB_HDR_SIZE) / PB_REC_SIZE)

#define H_COUNT     0x06
#define H_PHASE     0x08
#define H_FLAGS     0x09
#define H_PREVPHASE 0x0A
#define H_STEP      0x0B
#define H_WCR3      0x0C
#define H_CAP       0x0E
#define H_DATE      0x10
#define H_TIME      0x1C
#define H_FRAMES    0x24
#define H_EXC_KIND  0x28
#define H_EXC_PC    0x2C
#define H_EXC_SR    0x30
#define H_TEST      0x34

#define LF_DONE      0x01   // the run reached its end; kept at the next boot
#define LF_MOUSE     0x02   // a mouse was detected
#define LF_RESTARTED 0x04   // a finished log was erased on request (button held)
#define LF_PARTIAL   0x08   // an unfinished log was found and replaced
#define LF_EXCEPTION 0x10   // a CPU exception stopped the run

// ---------------------------------------------------------------------------
// Phases (the table in README.md). Nothing the person at the console has to
// read or do runs on a timer: the probe waits for a button to be pressed and
// let go before every step. A mouse phase then counts down for
// COUNTDOWN_FRAMES vertical blanks (from the vertical blank it publishes
// STEP_GETREADY at) and measures for its window (STEP_MEASURE).
// ---------------------------------------------------------------------------

enum {
	PH_BOOT = 1,        // title; a finished log is kept unless a button is held
	PH_FRAME = 2,       // bus registers, frame period
	PH_HV240 = 3,       // HCOUNT/VCOUNT trace, 240-line mode
	PH_HV224 = 4,       // the same, 224-line mode
	PH_MEM = 5,         // instruction fetch and memory access costs
	PH_RASTER = 6,      // display artifacts; step = variant
	PH_MOUSE = 10,      // mouse detection
	PH_IDLE = 11,       // (a) hands off
	PH_CIRCLES = 12,    // (b) circles
	PH_LEFT = 13,       // (c) left button held
	PH_RIGHT = 14,      //     right button held
	PH_BOTHBTN = 15,    //     both held
	PH_CAL_RIGHT = 16,  // (d) fast right, read every frame (calibration)
	PH_OVF_RIGHT = 17,  //     fast right, read once a second
	PH_CAL_LEFT = 18,   //     fast left, every frame
	PH_OVF_LEFT = 19,   //     fast left, once a second
	PH_SCAN_BOTH = 20,  // (e) pad and mouse scan both armed, mouse moving
	PH_SCAN_MOUSE = 21, //     mouse scan alone, the same motion (control)
	PH_DONE = 254,
};

// The step byte (0x0B). 0 while the probe works by itself.
#define STEP_PRESS    0xA0  // waiting for a press, every button up
#define STEP_GETREADY 0xA1  // a mouse phase's countdown
#define STEP_MEASURE  0xA2  // measuring: a mouse window, a raster capture
#define STEP_RELEASE  0xA3  // waiting for every button to be let go first

#define COUNTDOWN_FRAMES 180

// ---------------------------------------------------------------------------
// Record types (sub byte and payload a..g as noted).
// ---------------------------------------------------------------------------

#define R_MARK        0x01  // sub=phase; a,b = vblanks counted (hi, lo)
#define R_BSC         0x10  // sub 0: BCR WCR1 WCR2 WCR3 DCR PCR RCR
                            // sub 1: RTCSR RTCNT RTCOR, VDP MODE, BM_MEM_CTRL,
                            // IRQ0_NMI_CTRL at boot, DMAOR
#define R_FRAME       0x20  // sub=frames; a,b cycles; c,d cycles in vblank;
                            // e VCOUNT changes; f repeat
#define R_HV_SUM      0x30  // sub=lines; a samples, b distinct H, c H min,
                            // d H max, e/f the widest gap's edges (last seen
                            // before, first seen after), g distinct H in
                            // 0x102..0x1AB
#define R_HV_HRUN     0x31  // sub=lines; a lo, b hi, c values seen
#define R_HV_HJUMP    0x32  // sub=lines; a fwd jumps, b max H before, c min H
                            // after; d wraps, e max H before, f min H after
#define R_HV_HJRAW    0x33  // sub=lines; a kind (1 jump, 2 wrap), b V before,
                            // c V after, d H before, e H after, f cycles apart
#define R_HV_VSUM     0x34  // sub=lines; a V changes, b/c min/max H before a
                            // +1 change, d/e min/max H after it, f distinct
                            // V, g other changes
#define R_HV_VJUMP    0x35  // sub=lines; a V before, b V after, c H before,
                            // d H after
#define R_HV_VRUN     0x36  // sub=lines; a lo, b hi
#define R_HV_LINE     0x37  // sub=lines; a lines timed, b,c cycles summed,
                            // d min, e max
#define R_HV_RAW      0x38  // sub=lines; a index, b V, c H, d TCNT, e V, f H,
                            // g TCNT (two consecutive samples)
#define R_HV_FAIL     0x39  // sub=lines; the trace's start line never showed
#define R_MEM         0x40  // sub=probe; a n1, b/c min/max cycles at n1,
                            // d n2, e/f min/max at n2, g where it ran
#define R_MEMINFO     0x41  // a,b on-chip scratch address; c,d DRAM buffer
#define R_MOUSE_DET   0x50  // a present, b..d IO 0x010/12/14 at detection,
                            // e scan mode used afterwards
#define R_MOUSE_SMP   0x51  // sub=phase; a frame, b 0x010, c 0x012,
                            // d 0x014, e 0x050, f 0x052
#define R_MOUSE_DXY   0x52  // sub=phase; a first frame, b..g = X,Y raw x3
#define R_MOUSE_READ  0x53  // sub=phase; a frame, b X raw, c Y raw, d frames
                            // since the previous read
#define R_MOUSE_SUM   0x54  // sub=phase; a,b sum|dx|; c,d sum|dy|; e sum dx,
                            // f sum dy (both clamped to s16); g frames moved
#define R_RASTER      0x60  // sub=variant; a line, b frames with differences,
                            // c pixels differing, d most in one frame, e
                            // first x (0xFFFF none), f control, g captured
#define R_RASTER_ACC  0x61  // sub=variant; a rounds of 8 accesses in the last
                            // frame, b access width, c target hi, d target lo
#define R_RASTER_CTL  0x62  // sub=line; a..g pixels of the control capture at
                            // x = 16, 48, ... 208 (step 32)

// ---------------------------------------------------------------------------
// log.c
// ---------------------------------------------------------------------------

int LogKeptComplete(void);              // a finished log is in SRAM
uint16_t LogKeptRecords(void);          // its record count
void LogStart(uint8_t flags);           // erase and start a new log
void LogRec(uint8_t type, uint8_t sub, const uint16_t v[7]);
#define LOG(type, sub, ...) \
	LogRec((type), (sub), (const uint16_t[7]) { __VA_ARGS__ })
void LogPhase(uint8_t phase);           // also clears the step and the test
void LogStep(uint8_t step);
void LogTest(uint8_t test);
void LogFlags(uint8_t set);
uint16_t LogCount(void);
#define HI16(x) ((uint16_t) ((uint32_t) (x) >> 16))
#define LO16(x) ((uint16_t) (uint32_t) (x))

// ---------------------------------------------------------------------------
// timing.c: ITU1 as a free-running CPU-cycle counter, and the raster.
// ---------------------------------------------------------------------------

void ClockInit(void);
#define CYCLES() (ITU_TCNT1)            // 16 bits, CPU cycles, wraps 4.1 ms
extern uint32_t vblanks;                // vertical blank starts seen
void WaitVblank(void);                  // to the start of the next one
void WaitActive(void);                  // out of the blank (if in it)
#define IN_VBLANK() (VDP.VCOUNT & 0x100)
void TestFrame(void);
void TestHV(int lines240);

// ---------------------------------------------------------------------------
// screen.c: one 8bpp bitmap page plus a BG band and a row of sprites.
// ---------------------------------------------------------------------------

#define SCR_W 256
#define SCR_H 240
#define TEXT_X 12                       // a CRT hides about 8 pixels a side
#define ROW(n) (6 + (n) * 10)           // text rows 0-8, 14-17, 21-22

enum {                                  // palette entries
	C_INK = 1, C_YELLOW = 2, C_RED = 3, C_GREEN = 4, C_GREY = 5,
	C_BAR0 = 0x11,                      // 8 bars: 0x11..0x18
	C_BG = 0x21,                        // BG0 bank 2: 0x21..0x23
	C_OBJ = 0x31,                       // OBJ0 bank 3: 0x31..0x33
	C_OFFSCREEN = 0xFF,                 // used by nothing on screen
};
#define BARS_Y0 100                     // the colour bars, lines 100..139
#define BARS_Y1 140
#define BAND_Y0 184                     // BG0's band, lines 184..199
#define BAND_Y1 200
#define OBJ_Y   208                     // the sprites, lines 208..215
#define OBJ_TILE_WORD 0x7FFF            // tile VRAM word the probe pokes

void ScreenInit(void);
void ScreenScan(int mouse, int pad, int lines240);
void ClearRows(int y0, int y1);
void Text(int x, int y, int scale, uint8_t color, const char *s);
void Textf(int x, int y, int scale, uint8_t color, const char *fmt, ...)
	__attribute__((format(printf, 5, 6)));
void Line(int row, uint8_t color, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
void Title(const char *what);
void Present(void);
extern uint8_t scan_mode;               // CONTROL_MODE_* last armed
extern uint16_t irq_ctrl_boot;          // IRQ0_NMI_CTRL as the BIOS left it

// ---------------------------------------------------------------------------
// The tests.
// ---------------------------------------------------------------------------

void TestMemory(void);                  // memcost.c
void TestRaster(void);                  // raster.c
int  MouseDetect(void);                 // mouse.c
void TestMouse(void);

// Buttons (mouse.c): any mouse or pad button, by the presence bit.
int  ButtonHeld(void);
struct Press {
	uint8_t state;                      // 0 (start), 1 all up, 2 pressed
};
// Once a vertical blank: nonzero once a button has been pressed and let go
// (a button already down at the start must be let go first). Publishes
// STEP_RELEASE and STEP_PRESS as it goes.
int  PressStep(struct Press *p);
// Present(), then wait (no time limit) for a press and its release; the
// step is 0 again afterwards.
void WaitPress(void);

// probes.s, copied where they run
typedef void (*Probe)(volatile void *a, uint32_t n, volatile void *b,
                      uint32_t v);
typedef uint32_t (*Access)(volatile void *target, volatile uint16_t *vcount,
                           uint32_t value);

// The CPU's on-chip RAM left after .onchip (tools/loopy.ld).
extern char __onchip_link_end[] __asm("__onchip_link_end");
#define ONCHIP_FREE ((char *) (((uintptr_t) __onchip_link_end + 15u) & ~(uintptr_t) 15u))
#define ONCHIP_END  ((char *) 0x0F0003F0u)

#endif
