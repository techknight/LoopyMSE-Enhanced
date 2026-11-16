// LoopyProbe: the screen.
// Copyright (C) 2026 Derek Quenneville
// SPDX-License-Identifier: GPL-2.0-or-later
//
// One 8bpp bitmap page (BM0) holds the text and eight colour bars; a band of
// background tiles (BG0) and a row of eight sprites (OBJ0) are on as well,
// because the reference says tile VRAM accesses snow only while a BG or OBJ
// layer is enabled, and the raster test needs both kinds of picture to look
// at. Text is drawn into a copy of the page in work RAM and the rows that
// changed are copied to bitmap VRAM (dual-ported: writable at any time) by
// Present(). The VDP set-up follows loopy-maniac's lp_video.c, whose
// comments explain each register; the 5x7 font is its probe's.
//
// Layout (lines): text rows 0-8 (6..93), bars 100..139, text rows 14-17
// (146..183), BG band 184..199, sprites 208..215, text rows 21-22
// (216..233; row 22 only in 240-line mode).

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "probe.h"

static uint8_t fb[SCR_H][SCR_W] __attribute__((aligned(4)));
static int dirty_lo = SCR_H, dirty_hi = -1;
uint8_t scan_mode;

// A classic 5x7 font, column-major, LSB at the top: ASCII 0x20-0x5F.
static const uint8_t font5x7[0x40][5] = {
	{0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00},
	{0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
	{0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
	{0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
	{0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00},
	{0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
	{0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08},
	{0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
	{0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
	{0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
	{0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
	{0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
	{0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
	{0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
	{0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
	{0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
	{0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E},
	{0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
	{0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41},
	{0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
	{0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
	{0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
	{0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F},
	{0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
	{0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E},
	{0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
	{0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
	{0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
	{0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07},
	{0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
	{0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00},
	{0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
};

// The palette. decode_probe.py knows the bars' colours, to check what the
// scanline capture returns.
static const struct { uint8_t index; uint16_t rgb; } palette[] = {
	{ C_INK, RGB555(31, 31, 31) },   { C_YELLOW, RGB555(31, 31, 0) },
	{ C_RED, RGB555(31, 8, 8) },     { C_GREEN, RGB555(8, 31, 8) },
	{ C_GREY, RGB555(20, 20, 20) },
	{ C_BAR0 + 0, RGB555(31, 0, 0) }, { C_BAR0 + 1, RGB555(0, 31, 0) },
	{ C_BAR0 + 2, RGB555(0, 0, 31) }, { C_BAR0 + 3, RGB555(31, 31, 0) },
	{ C_BAR0 + 4, RGB555(0, 31, 31) }, { C_BAR0 + 5, RGB555(31, 0, 31) },
	{ C_BAR0 + 6, RGB555(16, 16, 16) }, { C_BAR0 + 7, RGB555(31, 16, 0) },
	{ C_BG + 0, RGB555(6, 20, 6) },   { C_BG + 1, RGB555(12, 28, 12) },
	{ C_BG + 2, RGB555(24, 31, 24) },
	{ C_OBJ + 0, RGB555(31, 20, 4) }, { C_OBJ + 1, RGB555(31, 28, 12) },
	{ C_OBJ + 2, RGB555(31, 31, 24) },
	{ C_OFFSCREEN, RGB555(31, 0, 15) },
};
#define BACKDROP RGB555(2, 4, 10)

// Tile VRAM with BG_CTRL = 0: map 0 at word 0 (64 x 64 entries), map 1 at
// word 0x1000, character data from word 0x2000, 16 words a 4bpp character.
// Character 0 stays blank (every unused map entry points at it).
#define CHAR_WORD(n) (0x2000u + (n) * 16u)
#define BG_CHAR  1
#define OBJ_CHAR 2
#define OAM_PARKED 0x00000200u   // y = 256: below the picture (lp_video.c)

static void Dirty(int y0, int y1)
{
	if (y0 < dirty_lo)
		dirty_lo = y0;
	if (y1 > dirty_hi)
		dirty_hi = y1;
}

void ClearRows(int y0, int y1)
{
	if (y0 < 0)
		y0 = 0;
	if (y1 > SCR_H)
		y1 = SCR_H;
	if (y0 >= y1)
		return;
	memset(fb[y0], 0, (size_t) (y1 - y0) * SCR_W);
	Dirty(y0, y1 - 1);
}

void Text(int x, int y, int scale, uint8_t color, const char *s)
{
	int step = 6 * scale;

	for (; *s; s++, x += step) {
		unsigned ch = (unsigned char) *s;
		const uint8_t *glyph;
		int cx, cy, px, py;

		if (ch >= 'a' && ch <= 'z')
			ch -= 0x20;
		if (ch < 0x20 || ch > 0x5F)
			ch = '?';
		glyph = font5x7[ch - 0x20];
		if (x < 0 || x + step > SCR_W || y < 0 || y + 8 * scale > SCR_H)
			continue;
		// Eight rows of six (the glyphs' bit 7 is clear, so the eighth row
		// and the sixth column are the gaps).
		for (cy = 0; cy < 8; cy++) {
			if (scale == 1) {
				uint8_t *row = &fb[y + cy][x];

				for (cx = 0; cx < 5; cx++)
					row[cx] = (glyph[cx] >> cy & 1) ? color : 0;
				row[5] = 0;
				continue;
			}
			for (cx = 0; cx < 6; cx++) {
				uint8_t c = (cx < 5 && (glyph[cx] >> cy & 1)) ? color : 0;

				for (py = 0; py < scale; py++)
					for (px = 0; px < scale; px++)
						fb[y + cy * scale + py][x + cx * scale + px] = c;
			}
		}
		Dirty(y, y + 8 * scale - 1);
	}
}

void Textf(int x, int y, int scale, uint8_t color, const char *fmt, ...)
{
	char buf[48];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	Text(x, y, scale, color, buf);
}

// A whole text row: cleared, then the line.
void Line(int row, uint8_t color, const char *fmt, ...)
{
	char buf[48];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	ClearRows(ROW(row), ROW(row) + 8);
	Text(TEXT_X, ROW(row), 1, color, buf);
}

// A new screen: every text row cleared, the heading on row 0.
void Title(const char *what)
{
	ClearRows(0, BARS_Y0);
	ClearRows(BARS_Y1, BAND_Y0);
	ClearRows(OBJ_Y + 8, SCR_H);
	Textf(TEXT_X, ROW(0), 1, C_YELLOW, "LOOPYPROBE P%u %s",
	      ((volatile uint8_t *) PB_SRAM_ADDR)[H_PHASE], what);
}

// The rows that changed, to bitmap VRAM in 32-bit stores (two bus cycles
// each on the VDP's 16-bit bus).
void Present(void)
{
	int y;

	for (y = dirty_lo; y <= dirty_hi; ++y) {
		const uint32_t *src = (const uint32_t *) (const void *) fb[y];
		volatile uint32_t *dst =
		    (volatile uint32_t *) (volatile void *) &VDP.BITMAP_VRAM[y * (SCR_W / 2)];
		int i;

		for (i = 0; i < SCR_W / 4; ++i)
			dst[i] = src[i];
	}
	dirty_lo = SCR_H;
	dirty_hi = -1;
}

void ScreenScan(int mouse, int pad, int lines240)
{
	scan_mode = (uint8_t) ((mouse ? CONTROL_MODE_MOUSE : 0) |
	                       (pad ? CONTROL_MODE_GAMEPAD : 0));
	bios_vdpMode(scan_mode, lines240 ? VIDEO_HEIGHT_240P : VIDEO_HEIGHT_224P);
}

uint16_t irq_ctrl_boot;

void ScreenInit(void)
{
	unsigned i, x, y;

	// No VDP interrupt sources: the vblank NMI cannot be masked by SR, and
	// would land in the middle of measurements. What the BIOS left is
	// logged with the bus registers.
	irq_ctrl_boot = VDP.IRQ0_NMI_CTRL;
	VDP.IRQ0_NMI_CTRL = 0;

	// Blank while reprogramming.
	VDP.LAYER_CTRL = 0;
	VDP.SCREENPRIO = 0;
	VDP.BACKDROP_A = 0;
	VDP.BACKDROP_B = 0;

	// Tile VRAM: all clear, then one character for the BG band and one
	// for the sprites, and the band's map entries (map 0, rows 23-24).
	for (i = 0; i < 0x8000u; ++i)
		VDP.TILE_VRAM[i] = 0;
	for (y = 0; y < 8; ++y) {
		uint16_t bg0 = 0, bg1 = 0, ob0 = 0, ob1 = 0;

		for (x = 0; x < 8; ++x) {
			unsigned bg = (x + y) % 3 + 1;
			unsigned ob = (x == 0 || x == 7 || y == 0 || y == 7) ? 1
			            : (x >= 3 && x <= 4 && y >= 3 && y <= 4) ? 3 : 2;
			unsigned shift = (3 - (x & 3)) * 4;

			if (x < 4) {
				bg0 |= (uint16_t) (bg << shift);
				ob0 |= (uint16_t) (ob << shift);
			} else {
				bg1 |= (uint16_t) (bg << shift);
				ob1 |= (uint16_t) (ob << shift);
			}
		}
		VDP.TILE_VRAM[CHAR_WORD(BG_CHAR) + y * 2] = bg0;
		VDP.TILE_VRAM[CHAR_WORD(BG_CHAR) + y * 2 + 1] = bg1;
		VDP.TILE_VRAM[CHAR_WORD(OBJ_CHAR) + y * 2] = ob0;
		VDP.TILE_VRAM[CHAR_WORD(OBJ_CHAR) + y * 2 + 1] = ob1;
	}
	for (y = BAND_Y0 / 8; y < BAND_Y1 / 8; ++y)
		for (x = 0; x < 32; ++x)
			VDP.TILE_VRAM[y * 64 + x] = BG_CHAR;   // screen A, palette 0

	VDP.BG_CTRL = 0;
	for (i = 0; i < 4; ++i)
		VDP.BG_SCROLL[i] = 0;
	VDP.BG_SUBPAL[0] = 2;   // BG0 palette descriptor 0 -> entries 0x2n
	VDP.BG_SUBPAL[1] = 0;
	VDP.OBJ_CTRL = OBJ_FORMAT_4BPP;
	VDP.OBJ_SUBPAL[0] = 3;  // OBJ0 palette descriptor 0 -> entries 0x3n
	VDP.OBJ_SUBPAL[1] = 0;
	VDP.CHARBASE = 0;

	// Every OAM slot parked (real hardware powers up with garbage there),
	// then eight 8x8 sprites in a row.
	for (i = 0; i < 0x80u; ++i)
		VDP.OAM[i] = OAM_PARKED;
	for (i = 0; i < 8; ++i)
		VDP.OAM[i] = ((uint32_t) OBJ_CHAR << 24) | ((uint32_t) OBJ_Y << 16) |
		             (24u + i * 28u);

	VDP.BLEND = BLEND_MATH;
	VDP.BM_SCROLLX[0] = 0;
	VDP.BM_SCROLLY[0] = 0;
	VDP.BM_SCREENX[0] = 0;
	VDP.BM_SCREENY[0] = 0;
	VDP.BM_WIDTH[0] = SCR_W - 1;
	VDP.BM_HEIGHT[0] = SCR_H - 1;
	VDP.BM_CTRL = BM_MODE_8BPP_SHARED;
	VDP.BM_SUBPAL = BM_SUBPAL(0, 0, 0, 0);
	VDP.BM_COL_LATCH[0] = 0;

	for (i = 0; i < 0x100u; ++i)
		VDP.PALETTE[i] = 0;
	for (i = 0; i < sizeof palette / sizeof palette[0]; ++i)
		VDP.PALETTE[palette[i].index] = palette[i].rgb;

	memset(fb, 0, sizeof fb);
	for (y = BARS_Y0; y < BARS_Y1; ++y)
		for (x = 0; x < SCR_W; ++x)
			fb[y][x] = (uint8_t) (C_BAR0 + x / 32);
	Dirty(0, SCR_H - 1);
	Present();

	VDP.BACKDROP_A = BACKDROP;
	VDP.BACKDROP_B = 0;
	VDP.SCREENPRIO = SCREEN_A_ENABLE | PRIORITY_BM_A | PRIORITY_BG0_A |
	                 PRIORITY_OBJ0_A;
	VDP.LAYER_CTRL = LAYER_SCREEN(LAYER_SCREEN_A, LAYER_SCREEN_A,
	                              LAYER_SCREEN_A, LAYER_SCREEN_A) |
	                 LAYER_ENABLE_BM0 | LAYER_ENABLE_BG0 | LAYER_ENABLE_OBJ0;
}
