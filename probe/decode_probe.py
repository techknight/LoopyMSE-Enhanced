#!/usr/bin/env python3
# LoopyProbe: decode the probe's SRAM log.
# Copyright (C) 2026 Derek Quenneville
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode LoopyProbe's SRAM log into a report with a verdict per question.

Usage:
    python decode_probe.py LoopyProbe.sav [-v] [-o report.txt]

The input is a dump of the cartridge's save RAM: the .sav LoopyMSE writes
(beside the ROM, or in --out), or the one the Floopy Drive dumps. A dump
whose 16-bit words are swapped is recognised and swapped back. A partial log
(the run stopped or lost power) is decoded as far as it goes. -v also lists
every record. The layout is in src/probe.h; nothing outside the standard
library is used.
"""

import argparse
import struct
import sys

MAGIC = b"LMPB"
HDR_SIZE = 0x40
REC_SIZE = 16

PHASES = {
    0: "not started", 1: "boot", 2: "frame period", 3: "HCOUNT/VCOUNT 240",
    4: "HCOUNT/VCOUNT 224", 5: "fetch and memory costs", 6: "raster artifacts",
    10: "mouse detection", 11: "mouse (a) idle", 12: "mouse (b) circles",
    13: "mouse (c) left held", 14: "mouse (c) right held",
    15: "mouse (c) both held", 16: "mouse (d) calibration right",
    17: "mouse (d) overflow right", 18: "mouse (d) calibration left",
    19: "mouse (d) overflow left", 20: "mouse (e) pad+mouse scan",
    21: "mouse (e) mouse scan alone", 254: "done",
}
FLAGS = [(0x01, "done"), (0x02, "mouse present"),
         (0x04, "a finished log was erased on request"),
         (0x08, "an unfinished log was found and replaced"),
         (0x10, "a CPU exception stopped the run")]
EXCEPTIONS = {4: "illegal instruction", 6: "illegal slot instruction",
              9: "CPU address error", 10: "DMA address error"}
STEPS = {0: "working", 0xA0: "waiting for a press", 0xA1: "counting down",
         0xA2: "measuring", 0xA3: "waiting for buttons to be let go"}

R_MARK, R_BSC, R_FRAME = 0x01, 0x10, 0x20
R_HV_SUM, R_HV_HRUN, R_HV_HJUMP, R_HV_HJRAW = 0x30, 0x31, 0x32, 0x33
R_HV_VSUM, R_HV_VJUMP, R_HV_VRUN, R_HV_LINE = 0x34, 0x35, 0x36, 0x37
R_HV_RAW, R_HV_FAIL = 0x38, 0x39
R_MEM, R_MEMINFO = 0x40, 0x41
R_MOUSE_DET, R_MOUSE_SMP, R_MOUSE_DXY = 0x50, 0x51, 0x52
R_MOUSE_READ, R_MOUSE_SUM = 0x53, 0x54
R_RASTER, R_RASTER_ACC, R_RASTER_CTL = 0x60, 0x61, 0x62

# The hardware reference (Casio Loopy Hardware Megadoc) and the emulator.
CPU_HZ = 16_000_000
VDP_HZ = 21_477_272
REF_FRAME = 1365 * 263 * CPU_HZ / VDP_HZ          # 267,441.6 CPU cycles
REF_LINE = 1365 * CPU_HZ / VDP_HZ                 # 1,016.9
OLD_FRAME = 266_419                               # LoopyMSE before (60.06 Hz)
OLD_LINE = OLD_FRAME / 263
CONSOLE_FRAME = 267_892.4                         # results/hardware: the author's console,
                                                  # LoopyMSE-Enhanced's timing since

# screen.c's palette, as the capture should return it (RGB555).
def rgb(r, g, b):
    return (r << 10) | (g << 5) | b

BARS = [rgb(31, 0, 0), rgb(0, 31, 0), rgb(0, 0, 31), rgb(31, 31, 0),
        rgb(0, 31, 31), rgb(31, 0, 31), rgb(16, 16, 16), rgb(31, 16, 0)]
BACKDROP = rgb(2, 4, 10)

RASTER_TESTS = {
    1: "no access (the control)",
    2: "palette write, bar 1's entry (on screen)",
    3: "palette write, entry 255 (not on screen)",
    4: "palette read, bar 1's entry",
    5: "OAM write, sprite 127 (parked)",
    6: "tile VRAM read",
    7: "tile VRAM write",
}

MEM_PROBES = {
    1: "nop x16, code in on-chip RAM", 2: "nop x32, code in on-chip RAM",
    3: "nop x16, code in work RAM", 4: "nop x32, code in work RAM",
    5: "nop x16, code in cartridge ROM", 6: "nop x32, code in cartridge ROM",
    7: "work RAM read, one address", 8: "work RAM write, one address",
    9: "work RAM read, two rows", 10: "work RAM write, two rows",
    11: "on-chip RAM read", 12: "cartridge ROM read",
    13: "8 calls to rts;nop in on-chip RAM", 14: "8 calls to rts;nop in work RAM",
    15: "8 calls to rts;nop in cartridge ROM",
    16: "8 calls to rts;nop in the BIOS (0x1158)",
    17: "8 calls to rts;nop in the BIOS (0x1652)",
}


def s12(v):
    v &= 0xFFF
    return v - 0x1000 if v & 0x800 else v


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def u32(hi, lo):
    return (hi << 16) | lo


# ---------------------------------------------------------------- loading

def find_log(data):
    if data[:4] == MAGIC:
        return data, 0, False
    at = data.find(MAGIC)
    if at >= 0:
        return data, at, False
    if len(data) % 2:
        data += b"\0"
    sw = bytearray(len(data))
    sw[0::2] = data[1::2]
    sw[1::2] = data[0::2]
    sw = bytes(sw)
    at = sw.find(MAGIC)
    if at >= 0:
        return sw, at, True
    return data, -1, False


def load(path):
    try:
        data = open(path, "rb").read()
    except OSError as e:
        sys.exit(f"{path}: {e}")
    data, at, swapped = find_log(data)
    if at < 0:
        sys.exit(f"{path}: no LoopyProbe log (magic 'LMPB') in either byte order; "
                 "is this the probe's save?")
    hdr = data[at:at + HDR_SIZE]
    if len(hdr) < HDR_SIZE:
        sys.exit(f"{path}: the log's header is cut off")
    version, count = struct.unpack_from(">HH", hdr, 4)
    log = dict(
        path=path, swapped=swapped, at=at, version=version, count=count,
        phase=hdr[8], flags=hdr[9], prev=hdr[10], step=hdr[11],
        wcr3=struct.unpack_from(">H", hdr, 0x0C)[0],
        cap=struct.unpack_from(">H", hdr, 0x0E)[0],
        date=hdr[0x10:0x1C].split(b"\0")[0].decode("ascii", "replace"),
        time=hdr[0x1C:0x24].split(b"\0")[0].decode("ascii", "replace"),
        frames=struct.unpack_from(">I", hdr, 0x24)[0],
        test=hdr[0x34],
        exc=hdr[0x28], exc_pc=struct.unpack_from(">I", hdr, 0x2C)[0],
        exc_sr=struct.unpack_from(">I", hdr, 0x30)[0])
    recs = []
    for i in range(count):
        off = at + HDR_SIZE + i * REC_SIZE
        if off + REC_SIZE > len(data):
            log["truncated"] = i
            break
        t, sub, *v = struct.unpack_from(">BB7H", data, off)
        recs.append((t, sub, v))
    log["recs"] = recs
    return log


def of(log, rtype, sub=None):
    return [(s, v) for t, s, v in log["recs"] if t == rtype and (sub is None or s == sub)]


# ---------------------------------------------------------------- report

verdicts = []


def verdict(q, text):
    verdicts.append(f"Q{q}: {text}")
    print(f"  => {text}")


def header(log):
    print(f"{log['path']}: LoopyProbe log v{log['version']}, {log['count']} records"
          + (" (16-bit words were swapped; read swapped back)" if log["swapped"] else ""))
    if log["at"]:
        print(f"  (the log starts at offset 0x{log['at']:X} of the file)")
    if log["version"] != 1:
        print(f"  WARNING: this decoder knows version 1, the log is {log['version']}")
    if "truncated" in log:
        print(f"  the file ends after record {log['truncated']}")
    print(f"  built {log['date']} {log['time']}; phase reached: {log['phase']} "
          f"({PHASES.get(log['phase'], '?')}), step 0x{log['step']:02X} "
          f"({STEPS.get(log['step'], '?')})"
          + (f", raster test {log['test']}" if log["test"] else ""))
    flags = [name for bit, name in FLAGS if log["flags"] & bit]
    print(f"  flags: {', '.join(flags) if flags else 'none'}")
    if log["flags"] & 0x08:
        print(f"  the replaced unfinished log had reached phase {log['prev']} "
              f"({PHASES.get(log['prev'], '?')})")
    if log["flags"] & 0x10:
        print(f"  EXCEPTION: {EXCEPTIONS.get(log['exc'], log['exc'])} at PC "
              f"{log['exc_pc']:08X}, SR {log['exc_sr']:08X}")
    print(f"  WCR3 at boot: {log['wcr3']:04X}; vertical blanks counted at the last "
          f"phase change: {log['frames']}")
    marks = [(s, u32(v[0], v[1])) for t, s, v in log["recs"] if t == R_MARK]
    if marks:
        print("  phases logged (at vertical blank): " +
              ", ".join(f"{p} ({f})" for p, f in marks))
    if not log["flags"] & 0x01:
        print("  the run did not finish: sections after the phase reached are missing")


def not_reached(log, phase, q, what):
    print(f"  (not reached: the run ended in phase {log['phase']}, "
          f"{PHASES.get(log['phase'], '?')})")
    verdicts.append(f"Q{q}: {what}: no data")


def q1_frame(log):
    print("\n== Q1: frame period ==")
    recs = of(log, R_FRAME)
    if not recs:
        return not_reached(log, 2, 1, "frame period")
    best = None
    for frames, v in recs:
        cyc, blank, lines = u32(v[0], v[1]), u32(v[2], v[3]), v[4]
        per = cyc / frames
        print(f"  {frames:2} frame(s): {cyc:9,} cycles = {per:11,.1f} a frame "
              f"({CPU_HZ / per:.4f} Hz at 16 MHz), {lines / frames:.2f} lines a frame, "
              f"{blank / frames:8,.0f} cycles a frame in vblank")
        if best is None or frames > best[0]:
            best = (frames, per, lines / frames)
    frames, per, lines = best
    print(f"  reference: {REF_FRAME:,.1f} cycles (1365 x 263 VDP cycles at 21.477272 MHz, "
          f"59.826 Hz); old emulator: {OLD_FRAME:,} (60.06 Hz)")
    ppm = (per - REF_FRAME) / REF_FRAME * 1e6
    if abs(per - REF_FRAME) <= 150:
        what = "matches the reference"
    elif abs(per - OLD_FRAME) <= 150:
        what = "matches the old emulator timing (266,419), not the reference"
    elif abs(per - CONSOLE_FRAME) <= 150:
        what = ("matches the console measured in results/hardware (267,892.4), "
                "not the reference")
    else:
        what = "matches neither the reference nor the old emulator timing"
    verdict(1, f"frame period {per:,.1f} cycles ({ppm:+.0f} ppm from the reference), "
               f"{lines:.2f} lines: {what}")


def hv_mode(log, lines):
    first_v = (lines - 263) & 0x1FF
    print(f"\n-- {lines}-line mode --")
    if of(log, R_HV_FAIL, lines):
        print(f"  the trace's start line ({lines - 4}) never showed in VCOUNT")
    sums = of(log, R_HV_SUM, lines)
    if not sums:
        print("  no trace")
        return None
    v = sums[0][1]
    n, distinct, hmin, hmax, gap_lo, gap_hi, mid = v
    raws = of(log, R_HV_RAW, lines)
    step_cycles = None
    deltas = []
    for _, r in raws:
        deltas.append((r[6] - r[3]) & 0xFFFF)
    if deltas:
        step_cycles = sorted(deltas)[len(deltas) // 2]
    steps = step_cycles * VDP_HZ / CPU_HZ / 4 if step_cycles else 8
    tol = max(4, int(steps * 2 + 1))
    print(f"  {n} samples, about {step_cycles or '?'} cycles (~{steps:.1f} HCOUNT steps) apart")
    print(f"  HCOUNT: {distinct} distinct values, {hmin:03X}..{hmax:03X}; widest gap "
          f"{gap_lo:03X} -> {gap_hi:03X}; {mid} values seen in 102..1AB")
    runs = [r for _, r in of(log, R_HV_HRUN, lines)]
    print("  HCOUNT runs (gaps up to 16 merged): " +
          ", ".join(f"{a:03X}-{b:03X} ({c} seen)" for a, b, c, *_ in runs))
    hj = of(log, R_HV_HJUMP, lines)
    if hj:
        fwd, fb, fa, wraps, wb, wa, _ = hj[0][1]
        print(f"  {fwd} forward jumps (max before {fb:03X}, min after {fa:03X}); "
              f"{wraps} wraps (max before {wb:03X}, min after {wa:03X})")
    for _, r in of(log, R_HV_HJRAW, lines):
        kind = "jump" if r[0] == 1 else "wrap"
        print(f"    {kind}: V {r[1]:03X}->{r[2]:03X}  H {r[3]:03X}->{r[4]:03X}  "
              f"({r[5]} cycles apart)")
    vs = of(log, R_HV_VSUM, lines)
    vchg = hb_min = hb_max = ha_min = ha_max = 0
    if vs:
        vchg, hb_min, hb_max, ha_min, ha_max, vdistinct, odd = vs[0][1]
        print(f"  VCOUNT: {vchg} changes, {vdistinct} distinct values, {odd} not +1")
        print(f"    a +1 change came between samples with HCOUNT {hb_min:03X}-{hb_max:03X} "
              f"before and {ha_min:03X}-{ha_max:03X} after (VCOUNT is read first)")
    vj = [r for _, r in of(log, R_HV_VJUMP, lines)]
    for r in vj:
        print(f"    VCOUNT {r[0]:03X} -> {r[1]:03X} (HCOUNT {r[2]:03X} -> {r[3]:03X})")
    vruns = [r for _, r in of(log, R_HV_VRUN, lines)]
    print("  VCOUNT runs: " + ", ".join(f"{a:03X}-{b:03X}" for a, b, *_ in vruns))
    line_rec = of(log, R_HV_LINE, lines)
    per_line = None
    if line_rec and line_rec[0][1][0]:
        cnt, hi, lo, lmin, lmax = line_rec[0][1][:5]
        per_line = u32(hi, lo) / cnt
        print(f"  line period {per_line:.2f} cycles over {cnt} lines (min {lmin}, max {lmax}); "
              f"reference {REF_LINE:.2f}, old emulator {OLD_LINE:.2f}")
    for _, r in raws:
        print(f"    sample {r[0]:5}: V {r[1] & 0x1FF:03X} H {r[2] & 0x1FF:03X} T {r[3]:04X} | "
              f"V {r[4] & 0x1FF:03X} H {r[5] & 0x1FF:03X} T {r[6]:04X}")

    # Verdicts against the reference. A run of a few values is a stray
    # reading (a read caught mid-change), not part of the count.
    main = [(a, b) for a, b, c, *_ in runs if c >= 8]
    stray = sum(c for a, b, c, *_ in runs if c < 8)
    h_ok = (len(main) == 2 and main[0][0] <= tol and 0x101 - tol <= main[0][1] <= 0x101
            and 0x1AC <= main[1][0] <= 0x1AC + tol and main[1][1] >= 0x1FF - tol)
    if h_ok:
        h_text = "HCOUNT runs 000..101 then 1AC..1FF (reference)"
        if stray:
            h_text += f", with {stray} stray readings between"
    elif mid:
        h_text = f"HCOUNT shows {mid} values in 102..1AB, which the reference says it skips"
    else:
        h_text = (f"HCOUNT values {hmin:03X}..{hmax:03X} with a gap {gap_lo:03X}->{gap_hi:03X} "
                  "(not the reference's 101->1AC)")
    if vs and ha_min >= 0x1AC and ha_max <= 0x1AC + tol * 2 and hb_min <= 0x101:
        v_text = "VCOUNT steps at HCOUNT's jump to 1AC (reference)"
    elif vs and ha_max < 0x40:
        v_text = "VCOUNT steps when HCOUNT wraps to 000 (not the reference)"
    elif vs:
        v_text = f"VCOUNT steps between HCOUNT {hb_min:03X}-{hb_max:03X} and {ha_min:03X}-{ha_max:03X}"
    else:
        v_text = "no VCOUNT data"
    last = lines - 1
    into_blank = [r for r in vj if r[0] == last]
    if into_blank and into_blank[0][1] == first_v:
        b_text = f"after line {last:03X} VCOUNT becomes {first_v:03X} (reference)"
    elif into_blank:
        b_text = (f"after line {last:03X} VCOUNT becomes {into_blank[0][1]:03X} "
                  f"(reference: {first_v:03X})")
    else:
        b_text = f"no jump after line {last:03X} seen (reference: to {first_v:03X})"
    wrap_ok = any(r[0] == 0x1FF and r[1] == 0 for r in vj)
    verdict(2, f"{lines} lines: {h_text}; {v_text}; {b_text}"
               + ("; then 1FF -> 000" if wrap_ok else "")
               + (f"; line {per_line:.1f} cycles" if per_line else ""))
    return per_line


def q2_hv(log):
    print("\n== Q2: HCOUNT and VCOUNT ==")
    if not of(log, R_HV_SUM) and not of(log, R_HV_FAIL):
        return not_reached(log, 3, 2, "HCOUNT/VCOUNT")
    for lines in (240, 224):
        hv_mode(log, lines)


def q3_mem(log):
    print("\n== Q3: fetch and memory access costs ==")
    bsc = dict(of(log, R_BSC))
    if 0 in bsc:
        b = bsc[0]
        print(f"  BCR {b[0]:04X}  WCR1 {b[1]:04X}  WCR2 {b[2]:04X}  WCR3 {b[3]:04X}  "
              f"DCR {b[4]:04X}  PCR {b[5]:04X}  RCR {b[6]:04X}")
    if 1 in bsc:
        b = bsc[1]
        div = {1: 2, 2: 8, 3: 32, 4: 128, 5: 512, 6: 2048, 7: 4096}.get((b[0] >> 3) & 7)
        refresh = f", a refresh about every {b[2] * div} cycles" if div and b[2] else ""
        print(f"  RTCSR {b[0]:04X}  RTCNT {b[1]:04X}  RTCOR {b[2]:04X}{refresh}; "
              f"VDP MODE {b[3]:04X}, BM_MEM_CTRL {b[4]:04X}")
        print(f"  VDP IRQ0_NMI_CTRL as the BIOS left it {b[5]:04X} (the probe sets 0); "
              f"DMAOR {b[6]:04X}")
    recs = dict(of(log, R_MEM))
    if not recs:
        return not_reached(log, 5, 3, "memory costs")

    def per_pass(i):
        if i not in recs:
            return None
        n1, lo1, hi1, n2, lo2, hi2, where = recs[i]
        return (lo2 - lo1) / (n2 - n1)

    print("  id  probe                                   cycles a pass  (spread n1/n2)")
    for i in sorted(recs):
        n1, lo1, hi1, n2, lo2, hi2, where = recs[i]
        print(f"  {i:2}  {MEM_PROBES.get(i, '?'):40} {per_pass(i):8.3f}       "
              f"({hi1 - lo1}/{hi2 - lo2})")

    p = {i: per_pass(i) for i in recs}

    def fetch(a, b):
        return (p[b] - p[a]) / 16 if p.get(a) is not None and p.get(b) is not None else None

    f_on, f_dram, f_rom = fetch(1, 2), fetch(3, 4), fetch(5, 6)
    base = f_on if f_on is not None else 1.0

    def access(i):
        if p.get(i) is None or p.get(1) is None:
            return None
        return (p[i] - p[1]) / 16 + base

    def call(i):
        return p[i] / 8 if p.get(i) is not None else None

    rows = [
        ("fetch, on-chip RAM (per instruction)", f_on, 1, 1),
        ("fetch, work RAM (sequential)", f_dram, 1, 1),
        ("fetch, cartridge ROM", f_rom, 3, 3),
        ("work RAM read, open row", access(7), 1, 1),
        ("work RAM write, open row", access(8), 1, 2),
        ("work RAM read, row change each time", access(9), 3, 3),
        ("work RAM write, row change each time", access(10), 3, 3),
        ("on-chip RAM read", access(11), 1, 1),
        ("cartridge ROM read", access(12), 3, 3),
    ]
    print("\n  item                                     measured  reference  emulator model")
    for name, val, ref, emu in rows:
        if val is None:
            continue
        print(f"  {name:40} {val:8.2f}  {ref:9}  {emu:14}")
    calls = {i: call(i) for i in (13, 14, 15, 16, 17)}
    if all(calls[i] is not None for i in (13, 14, 15, 16)):
        print("\n  a call to rts;nop (jsr, its delay slot, rts, its delay slot), per call:")
        for i, where in ((13, "on-chip RAM"), (14, "work RAM"), (15, "cartridge ROM"),
                         (16, "BIOS 0x1158"), (17, "BIOS 0x1652")):
            if calls.get(i) is not None:
                print(f"    {where:14} {calls[i]:7.2f} cycles ({calls[i] - calls[13]:+.2f} "
                      "against on-chip RAM)")
        k = None
        if f_rom is not None and f_dram is not None and abs(f_rom - f_dram) >= 0.5:
            k = (calls[15] - calls[14]) / (f_rom - f_dram)
        k_used = k if k and 1 <= k <= 4 else 2.0
        f_bios = (f_dram if f_dram is not None else 1.0) + (calls[16] - calls[14]) / k_used
        print(f"    16-bit fetches a call: {k:.2f}" if k else "    16-bit fetches a call: "
              "taken as 2")
        extra = calls[16] - calls[13]
        if extra <= 0.5:
            bios_text = (f"BIOS code runs as fast as on-chip RAM ({extra:+.2f} cycles a call): "
                         "1-cycle fetches, the reference (the old console model charged 3)")
        elif abs(f_bios - 3) <= 0.5:
            bios_text = (f"BIOS fetches about {f_bios:.1f} cycles: the old console model, "
                         "not the reference's 1")
        else:
            bios_text = (f"BIOS fetches about {f_bios:.1f} cycles each "
                         f"({extra:+.2f} cycles a call against on-chip RAM)")
        verdict(3, bios_text)
    w = access(8)
    if w is not None:
        if abs(w - 1) <= 0.4:
            w_text = f"open-row work RAM write {w:.2f} cycles: the reference (1), not the model (2)"
        elif abs(w - 2) <= 0.4:
            w_text = f"open-row work RAM write {w:.2f} cycles: the emulator model (2), not the reference (1)"
        else:
            w_text = f"open-row work RAM write {w:.2f} cycles (reference 1, model 2)"
        verdict(3, w_text)
    r = access(7)
    if r is not None:
        verdict(3, f"open-row work RAM read {r:.2f} cycles (reference and model: 1)")
    if f_on is not None and f_dram is not None and f_rom is not None:
        verdict(3, f"fetch per instruction: on-chip {f_on:.2f}, work RAM {f_dram:.2f}, "
                   f"cartridge ROM {f_rom:.2f} cycles")


def q4_raster(log):
    print("\n== Q4: display artifacts from VDP accesses during the picture ==")
    recs = of(log, R_RASTER)
    if not recs:
        return not_reached(log, 6, 4, "raster artifacts")
    ctl = dict(of(log, R_RASTER_CTL))
    capture_ok = None
    if 120 in ctl:
        got = [c & 0x7FFF for c in ctl[120]]
        want = BARS[:7]
        capture_ok = got == want
        print("  control capture of line 120 at the bars' centres: " +
              " ".join(f"{c:04X}" for c in ctl[120]) +
              (" (the bars' colours: the capture works)" if capture_ok else
               "  -- expected " + " ".join(f"{c:04X}" for c in want)))
    if 60 in ctl:
        print("  control capture of line 60: " + " ".join(f"{c:04X}" for c in ctl[60]) +
              f" (backdrop {BACKDROP:04X} where no text)")
    for line in (190, 211):
        if line in ctl:
            print(f"  control capture of line {line}: " + " ".join(f"{c:04X}" for c in ctl[line]))
    acc = dict(of(log, R_RASTER_ACC))
    seen = []
    for n in sorted(set(s for s, _ in recs)):
        rows = [v for s, v in recs if s == n]
        total = sum(v[2] for v in rows)
        frames = sum(v[1] for v in rows)
        extra = ""
        if n in acc:
            extra = f"; {acc[n][0] * 8:,} accesses a frame" if acc[n][1] else ""
        print(f"  test {n}: {RASTER_TESTS.get(n, '?')}{extra}")
        for v in rows:
            line, fr, px, mx, x, c, g = v
            detail = (f", first at x={x}: {c:04X} -> {g:04X}" if x != 0xFFFF else "")
            print(f"    line {line:3}: {px:5} pixels differ in {fr} of 4 frames "
                  f"(most {mx} in one){detail}")
        if n == 1:
            if total:
                print("    the control's own captures differ: the capture is noisy, "
                      "so differences below are not conclusive")
        elif total:
            seen.append(f"test {n} ({RASTER_TESTS.get(n, '?')}): {total} pixels")
    if capture_ok is False:
        verdict(4, "the capture did not return the expected picture; see the photographs")
    elif seen:
        verdict(4, "the capture saw artifacts in " + "; ".join(seen))
    else:
        verdict(4, "capture did not see any artifacts in any test; compare the photographs")


def stable_bits(values):
    ones, zeros = 0xFFFF, 0xFFFF
    for v in values:
        ones &= v
        zeros &= ~v & 0xFFFF
    return ones, zeros


def q5_mouse(log):
    print("\n== Q5: the XN-110 mouse ==")
    det = of(log, R_MOUSE_DET)
    if not det:
        return not_reached(log, 10, 5, "mouse")
    present, g0, g1, g2, scan, hits = det[0][1][:6]
    print(f"  detection (pad and mouse scan): 0x010 bit 15 on {hits} of 30 frames; last "
          f"010 {g0:04X} 012 {g1:04X} 014 {g2:04X}")
    if not present:
        verdicts.append("Q5: no mouse detected: the mouse tests were skipped")
        print("  => no mouse: skipped")
        return
    smp = {}
    for s, v in of(log, R_MOUSE_SMP):
        smp.setdefault(s, []).append(v)

    idle = smp.get(11, [])
    if idle:
        print(f"\n  (a) idle, {len(idle)} frames:")
        noisy = []
        for k, name in ((1, "010"), (2, "012"), (3, "014"), (4, "050"), (5, "052")):
            vals = [v[k] for v in idle]
            changes = sum(1 for a, b in zip(vals, vals[1:]) if a != b)
            print(f"    {name}: {len(set(vals))} distinct values, changed on {changes} of "
                  f"{len(vals) - 1} frames; first {vals[0]:04X}")
            if k <= 3:
                noisy.append(changes)
        det_bits = []
        for k, name in ((1, "010"), (2, "012"), (3, "014")):
            for half, bit in (("high", 0x8000), ("low", 0x0080)):
                on = sum(1 for v in idle if v[k] & bit)
                det_bits.append(f"{name} {half} {on}/{len(idle)}")
        print("    bit 7 (detect) set, by byte: " + ", ".join(det_bits))
        frac = max(noisy) / max(1, len(idle) - 1)
        all_det = all(v[k] & 0x8080 == 0x8080 for v in idle for k in (1, 2, 3))
        if frac > 0.25:
            text = ("controller bytes change constantly while the mouse is idle "
                    "(the old emulator's garbage model)")
        else:
            text = ("controller bytes stay still while the mouse is idle "
                    "(the reference: raw gray code, changes only with motion)")
        text += ("; bit 7 set in all six bytes (reference)" if all_det
                 else "; bit 7 not set in all six bytes")
        verdict(5, text)

    circ = smp.get(12, [])
    if circ:
        dx = [s12(v[4]) for v in circ]
        dy = [s12(v[5]) for v in circ]
        print(f"\n  (b) circles, {len(circ)} frames: sum |dx| {sum(map(abs, dx))}, "
              f"sum |dy| {sum(map(abs, dy))}, frames with motion "
              f"{sum(1 for a, b in zip(dx, dy) if a or b)}")
        for k, name in ((1, "010"), (2, "012"), (3, "014")):
            vals = [v[k] for v in circ]
            low = [x & 0x0F0F for x in vals]
            print(f"    {name}: {len(set(vals))} distinct values; low nibbles "
                  f"(0x0F0F) {len(set(low))} distinct")

    base = {}
    if idle:
        for k in (1, 2, 3, 4):
            base[k] = stable_bits([v[k] for v in idle])
    for ph, name in ((13, "left"), (14, "right"), (15, "both")):
        rows = smp.get(ph, [])
        if not rows:
            continue
        print(f"\n  (c) {name} held, {len(rows)} frames:")
        for k, reg in ((1, "010"), (2, "012"), (3, "014"), (4, "050")):
            ones, zeros = stable_bits([v[k] for v in rows])
            if k in base:
                b1, b0 = base[k]
                went_low = b1 & zeros
                went_high = b0 & ones
                print(f"    {reg}: bits that went low {went_low:04X}, went high "
                      f"{went_high:04X} (against idle)")
            else:
                print(f"    {reg}: stable ones {ones:04X}, stable zeros {zeros:04X}")
        if ph == 13 and 1 in base:
            ones, zeros = stable_bits([v[1] for v in rows])
            low = base[1][0] & zeros
            o2, z2 = stable_bits([v[2] for v in rows])
            low2 = base[2][0] & z2 if 2 in base else 0
            if low & 0x1000 and low2 & 0x1010:
                text = "left button clears bit 4 of the controller bytes, 012 too (reference)"
            elif low & 0x1000:
                text = (f"left button clears 010 bit 12 (low {low:04X}); 012 does not follow "
                        "(the old emulator model: buttons only in 010)")
            else:
                text = f"left button: 010 bits that went low {low:04X}"
            verdict(5, text)

    sums = {s: v for s, v in of(log, R_MOUSE_SUM)}
    dxy = {}
    for s, v in of(log, R_MOUSE_DXY):
        f0 = v[0]
        for j in range(3):
            dxy.setdefault(s, []).append((f0 + j, s12(v[1 + 2 * j]), s12(v[2 + 2 * j])))
    reads = {}
    for s, v in of(log, R_MOUSE_READ):
        reads.setdefault(s, []).append(v)

    over = []
    for cal, ovf, name in ((16, 17, "right"), (18, 19, "left")):
        if cal not in dxy:
            continue
        xs = [d[1] for d in sorted(dxy[cal])]
        total = sum(xs)
        sign = 1 if total >= 0 else -1
        best = 0
        for i in range(max(1, len(xs) - 59)):
            best = max(best, sign * sum(xs[i:i + 60]))
        print(f"\n  (d) fast {name}, read every frame: {len(xs)} frames, X total {total:+}, "
              f"busiest second {sign * best:+} counts (direction {'+' if sign > 0 else '-'})")
        if ovf not in reads:
            continue
        rows = [r for r in reads[ovf] if r[0]]
        drain = [r for r in reads[ovf] if not r[0]]
        if drain:
            print(f"    window start, emptied: X {drain[0][1]:04X} ({s12(drain[0][1]):+}) "
                  f"Y {drain[0][2]:04X}")
        print(f"  (d) fast {name}, read once a second:")
        for f, x, y, apart, *_ in rows:
            v = s12(x)
            kind = ""
            if v in (2047, -2048):
                kind = "pinned at the limit: SATURATED"
                over.append("saturate")
            elif sign * v < -64:
                kind = f"wrong sign: WRAPPED (true count {v + sign * 4096:+} if once)"
                over.append("wrap")
            print(f"    frame {f:3}: X {x:04X} = {v:+5} after {apart} frames, "
                  f"Y {s12(y):+5}  {kind}")
        if best <= 2047:
            print(f"    the busiest calibration second ({best} counts) did not pass 2047, "
                  "so these readings may not have overflowed")
    if over:
        if "saturate" in over and "wrap" not in over:
            verdict(5, "mouse counters SATURATE at +2047/-2048 (reference)")
        elif "wrap" in over and "saturate" not in over:
            verdict(5, "mouse counters WRAP (12-bit; the old emulator model)")
        else:
            verdict(5, "mouse counters: both pinned and wrapped readings (inconclusive)")
    elif 17 in reads or 19 in reads:
        verdict(5, "mouse counters: no reading overflowed (move faster; inconclusive)")

    if 20 in sums and 21 in sums:
        both = u32(sums[20][0], sums[20][1]) + u32(sums[20][2], sums[20][3])
        alone = u32(sums[21][0], sums[21][1]) + u32(sums[21][2], sums[21][3])
        print(f"\n  (e) |dx|+|dy| with pad and mouse scan both armed: {both}; mouse scan alone: "
              f"{alone} (frames moved {sums[20][6]} / {sums[21][6]})")
        if alone == 0:
            verdict(5, "scan test inconclusive: no motion counted even with the mouse scan alone")
        elif both <= alone * 0.05:
            verdict(5, "no counts accumulate with the pad scan also armed (the emulator's model)")
        else:
            verdict(5, f"counts accumulate with both scans armed ({both} against {alone})")


def dump(log):
    print("\n== all records ==")
    for i, (t, s, v) in enumerate(log["recs"]):
        print(f"  {i:4}  type {t:02X} sub {s:3}  " + " ".join(f"{x:04X}" for x in v))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sav", help="the save RAM dump (.sav)")
    ap.add_argument("-v", "--verbose", action="store_true", help="also list every record")
    ap.add_argument("-o", "--output", help="write the report to this file instead of the console")
    args = ap.parse_args()
    if args.output:
        sys.stdout = open(args.output, "w", encoding="utf-8", newline="\n")
    log = load(args.sav)
    header(log)
    q1_frame(log)
    q2_hv(log)
    q3_mem(log)
    q4_raster(log)
    q5_mouse(log)
    print("\n== verdicts ==")
    for v in verdicts:
        print("  " + v)
    if args.verbose:
        dump(log)
    return 0


if __name__ == "__main__":
    sys.exit(main())
