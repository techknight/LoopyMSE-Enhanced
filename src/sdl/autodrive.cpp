/*
Autodrive: closed-loop chase controller for the emulated Loopy mouse.

A cooperating cartridge publishes its cursor state through four render-inert
BG registers (the BG layers are never enabled, so these are spare bits the
game can write without drawing anything):

  BG0_SCROLLX (offset 0x002, 12-bit)  AP_CTL  magic 0xA in bits [11:8],
                                              bit0 target-valid,
                                              bit1 request left button,
                                              bit2 request right button
  BG0_SCROLLY (offset 0x004, 12-bit)  AP_DBG  cartridge-defined debug word
  BG0_PALSEL  (offset 0x00A, 16-bit)  AP_CUR  (cur_y << 8) | cur_x
  BG1_PALSEL  (offset 0x00C, 16-bit)  AP_TGT  (tgt_y << 8) | tgt_x

Coordinates are game pixels (x 0-255, y 0-239); the PALSEL registers are
stored full-width, so one 16-bit write publishes a coordinate pair without
tearing. While the magic is absent this module does nothing at all, which is
what keeps it safe to leave enabled under ROMs (and the BIOS) that do not
speak the protocol.

Motion: inject the full remaining delta as mouse counts (game pixels are
counts / 2, Y sign inverted between here and the game's ISR). The cartridge
publishes at its logic rate (~15 Hz) while this runs per emulated frame
(60 Hz), so inject only when a fresh AP_CUR observation arrives -- injecting
every frame would deal the same delta four times over and oscillate. A
timeout re-arm with a small budget covers deltas lost to the game's own
cursor clamp (the budget bounds the overshoot; the clamp absorbs it and the
next observation corrects).

Buttons are level-held while the request bit is up: the cartridge sees the
press edge through the real mouse path, drops the request, and this releases
-- one edge per request no matter how many frames either side takes.

A script can also name the target itself ("mouse to X Y"): that overrides
the cartridge's AP_TGT until the cursor arrives. AP_DBG is logged (debug
level) whenever it changes, so a cartridge can show its own progress.
*/
#include <input/input.h>
#include <log/log.h>
#include <video/video.h>

#include <cstdint>

#include "autodrive.h"

namespace Autodrive
{

static constexpr uint32_t REG_CTL = 0x002;
static constexpr uint32_t REG_DBG = 0x004;
static constexpr uint32_t REG_CUR = 0x00A;
static constexpr uint32_t REG_TGT = 0x00C;

static constexpr int CTL_MAGIC = 0xA;
static constexpr uint16_t CTL_TGT_VALID = 0x001;
static constexpr uint16_t CTL_REQ_L = 0x002;
static constexpr uint16_t CTL_REQ_R = 0x004;

//One injection may cross the whole screen (2 * 255 = 510 counts); the cap
//only has to keep a few stacked timeout re-arms inside the signed 12-bit
//counter range.
static constexpr int MAX_COUNTS_PER_INJECT = 1000;

//Frames without a fresh AP_CUR before re-injecting anyway, and how many such
//blind injections may stack up before waiting for the cartridge to speak.
static constexpr int REARM_FRAMES = 8;
static constexpr int REARM_BUDGET = 4;

static bool enabled = false;
static bool seen_magic = false;
static bool held_l = false;
static bool held_r = false;
static uint16_t last_cur = 0;
static bool have_cur = false;
static int frames_since_inject = 0;
static int rearm_debt = 0;
static bool override_target = false;
static uint16_t override_tgt = 0;
static int last_dbg = -1;

static void hold_button(int mouse_button, bool* held, bool want)
{
	if (*held == want)
		return;
	Input::inject_mouse_buttons(mouse_button, want);
	*held = want;
	Log::debug("[Autodrive] %s %s", mouse_button == Input::MOUSE_L ? "left" : "right", want ? "press" : "release");
}

static int clamp_counts(int counts)
{
	if (counts > MAX_COUNTS_PER_INJECT)
		return MAX_COUNTS_PER_INJECT;
	if (counts < -MAX_COUNTS_PER_INJECT)
		return -MAX_COUNTS_PER_INJECT;
	return counts;
}

void set_enabled(bool e)
{
	enabled = e;
}

bool is_enabled()
{
	return enabled;
}

void reset()
{
	hold_button(Input::MOUSE_L, &held_l, false);
	hold_button(Input::MOUSE_R, &held_r, false);
	seen_magic = false;
	have_cur = false;
	frames_since_inject = 0;
	rearm_debt = 0;
	override_target = false;
	last_dbg = -1;
}

bool set_target(int x, int y)
{
	if (!enabled || !seen_magic)
		return false;
	override_target = true;
	override_tgt = (uint16_t)(((y & 0xFF) << 8) | (x & 0xFF));
	Log::debug("[Autodrive] script target (%d,%d)", x & 0xFF, y & 0xFF);
	return true;
}

void run_frame()
{
	if (!enabled)
		return;

	uint16_t ctl = Video::bgobj_read16(REG_CTL);
	if (((ctl >> 8) & 0xF) != CTL_MAGIC)
	{
		//Not our cartridge talking (or not yet booted): stay inert.
		if (held_l || held_r || have_cur || seen_magic)
			reset();
		return;
	}

	if (!seen_magic)
	{
		seen_magic = true;
		Log::info("[Autodrive] cartridge speaking APDRIVE, chasing");
	}

	int dbg = Video::bgobj_read16(REG_DBG);
	if (dbg != last_dbg)
	{
		last_dbg = dbg;
		Log::debug("[Autodrive] cartridge debug word %03X", dbg);
	}

	hold_button(Input::MOUSE_L, &held_l, (ctl & CTL_REQ_L) != 0);
	hold_button(Input::MOUSE_R, &held_r, (ctl & CTL_REQ_R) != 0);

	if (!(ctl & CTL_TGT_VALID) && !override_target)
	{
		have_cur = false;
		return;
	}

	uint16_t cur = Video::bgobj_read16(REG_CUR);
	uint16_t tgt = override_target ? override_tgt : Video::bgobj_read16(REG_TGT);
	int dpx = (tgt & 0xFF) - (cur & 0xFF);
	int dpy = (tgt >> 8) - (cur >> 8);

	frames_since_inject++;
	if (dpx == 0 && dpy == 0)
	{
		last_cur = cur;
		have_cur = true;
		rearm_debt = 0;
		if (override_target)
		{
			override_target = false;
			Log::debug("[Autodrive] reached script target");
		}
		return;
	}

	bool fresh = !have_cur || cur != last_cur;
	if (fresh)
		rearm_debt = 0;
	else if (frames_since_inject < REARM_FRAMES || rearm_debt >= REARM_BUDGET)
		return;
	else
		rearm_debt++;

	//Game pixels are counts / 2; the game's ISR negates Y (and the counter
	//read is what un-negates the SDL convention), so send 2*dpx, -2*dpy.
	Input::inject_mouse_move(clamp_counts(2 * dpx), clamp_counts(-2 * dpy));
	Log::debug("[Autodrive] chase (%d,%d) -> (%d,%d)", cur & 0xFF, cur >> 8, tgt & 0xFF, tgt >> 8);

	last_cur = cur;
	have_cur = true;
	frames_since_inject = 0;
}

}  // namespace Autodrive
