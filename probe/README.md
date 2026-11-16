# LoopyProbe

A Casio Loopy homebrew ROM that measures, on a real console, the behaviour
LoopyMSE-Enhanced and the hardware reference (the *Casio Loopy Hardware
Megadoc*) disagree on. It logs every result to the cartridge's save RAM (so
the `.sav` can be dumped and decoded) and shows its progress on screen. The
same ROM run in the emulator gives the emulator's answers, so the two
reports compare line for line.

## What it measures

| # | Question | Reference says | Emulator (model) says |
| --- | --- | --- | --- |
| 1 | Frame period | 1365 VDP cycles a line at 21.477272 MHz, 263 lines: 267,441.6 CPU cycles (59.826 Hz) | was 266,419 (60.06 Hz) |
| 2 | HCOUNT / VCOUNT (0x0C058002/4) | HCOUNT 0x000-0x101 (4 VDP cycles a step), then 0x1AC-0x1FF; VCOUNT steps at that jump; after the last active line it becomes 0x1D9 (224 lines) or 0x1E9 (240) and counts to 0x1FF | (same in the current source) |
| 3 | Fetch and memory costs | BIOS fetch 1 cycle; work RAM write in the open 1 KB row 1 cycle | console timing model: BIOS fetch 3, open-row write 2 |
| 4 | Raster artifacts | palette writes clean; palette reads, and tile VRAM accesses while a BG/OBJ layer is on, snow | the author's ports saw dots from palette writes on a console |
| 5 | XN-110 mouse | counters saturate at +2047/-2048; all six bytes of 0x0C05D010/12/14 read bit 7 detect, bit 6 right, bit 4 left (active-low), bits 3-0 raw gray code | counters wrap; pad bits of 0x010/12/14 are garbage; bit 15 of 0x010 is presence, bits 12-14 the buttons; no counts while the pad scan is armed too |

How:

1. **Frame period.** ITU channel 1 counts CPU cycles (phi/1, free-running);
   the probe polls VCOUNT bit 8 (set through the vertical blank, as the
   BIOS's own vsync does) from on-chip RAM and times 1, 8 and 64 frames,
   counting VCOUNT changes and the cycles spent in the blank. Interrupts stay
   masked for the whole run and `bios_vsync` is never called.
2. **HCOUNT/VCOUNT.** A loop in on-chip RAM samples VCOUNT, HCOUNT and the
   cycle counter as fast as it can for 1.2 frames from four lines before
   the end of the picture, in 240- and in 224-line mode. The trace is
   reduced to: the HCOUNT values that occur, where HCOUNT jumps and wraps,
   where VCOUNT steps relative to HCOUNT, the VCOUNT values and their odd
   steps, the line period, and raw samples around the jump and the blank.
3. **Costs.** loopy-attorney's probes (one instruction 16 or 32 times in a
   loop), copied to on-chip RAM, work RAM or run in place in the cartridge,
   timed at two loop counts so the call overhead cancels. For the BIOS, a
   loop of `jsr` to an `rts; nop` in the BIOS (0x1158 and 0x1652) against
   the same two instructions in on-chip RAM, work RAM and the cartridge.
   BCR, WCR1-3, DCR, PCR, RCR and the refresh registers are logged too.
4. **Raster.** Seven tests, each hammering one kind of access through the
   whole active picture from on-chip RAM, writing the value already there:
   none (control), palette write to bar 1's entry, palette write to unused
   entry 255, palette read, OAM write (parked sprite 127), tile VRAM read,
   tile VRAM write. A BG band and a row of sprites are on, as well as the
   bitmap. The VDP's scanline capture (mode 0, the displayed output, 15bpp)
   takes lines 60, 120, 190 and 211, four frames each, compared with the
   control. If the capture never differs, the answer is "capture did not see
   any": each test also stays on screen for five seconds, labelled, for a
   photograph.
5. **Mouse.** On a clock (with the mouse plugged in there is no pad to
   press): (a) hands off, (b)
   circles, (c) left, right, both held, (d) fast right and left, read every
   frame (calibration) and then once a second (overflow), (e) moving with
   the pad and mouse scans both armed, then with the mouse scan alone.

## Building

In WSL with the Wonderful toolchain (`gcc-sh-elf` 13.1, picolibc) in
`/opt/wonderful` or `~/wonderful`:

```
wsl -e bash /mnt/g/LoopyMSE-Enhanced/probe/build.sh
```

This makes `probe/LoopyProbe.bin` (big-endian, what LoopyMSE and the Floopy
Drive take; 32 KB of save RAM declared in its header) and
`probe/LoopyProbe.elf`. `build.sh clean` cleans.

## Running it on the console

1. Put `LoopyProbe.bin` on the Floopy Drive **under its own name**, so it
   gets a save file of its own (the probe uses all 32 KB of it and clears
   it when a run starts).
2. Plug the XN-110 mouse into the controller port (it replaces the pad).
   Without a mouse the probe still runs and logs "no mouse".
3. Turn on. The probe runs by itself for about two and a half minutes and
   says on screen what to do; the table below lists it. Keep your hands off
   the mouse until it asks. Photograph each of the seven raster tests while
   it says PHOTOGRAPH THE SCREEN NOW (the test number is in large letters);
   the result screens after the timing tests are worth a photograph too.
4. At ALL DONE, turn the console off and dump the probe's `.sav`. Put it in
   `results/hardware/`.

A finished log survives a power cycle: the probe then shows LOG COMPLETE
and does nothing else. To erase it and run again, hold a mouse button (or
any pad button) for three seconds on that screen. An unfinished log (power
lost, a hang) is replaced at the next boot, but the new log's header keeps
the phase it had reached. A CPU exception stops the run with a red screen
and records the vector, PC and SR in the header.

### Phases

The phase number is in SRAM at 0x02000008 (see *SRAM log*). Times are in
vertical blanks (about 60 a second). Each mouse phase shows its instruction
and a countdown for exactly 240 vertical blanks from the moment its number
is published, then measures for its window; the counters are emptied as the
window opens.

| Phase | What it does | Frames | What you do |
| --- | --- | --- | --- |
| 1 | title | 180 | nothing |
| 2 | bus registers; frame period: 4 x 1 frame, 2 x 8, 1 x 64 | about 95, then 120 of results | hands off |
| 3 | HCOUNT/VCOUNT trace, 240 lines | about 10, then 120 of results | hands off |
| 4 | the same in 224-line mode (the picture shrinks for a moment) | about 10, then 120 | hands off |
| 5 | fetch and memory costs | about 15, then 120 | hands off |
| 6 | raster tests 1-7 (step byte = test): 2 + 16 capture frames, then 300 on show | 7 x 318 | photograph each test |
| 10 | mouse detection (pad and mouse scan) | 30 | hands off |
| 11 | (a) idle: 0x010/12/14 and 0x050/52 every frame | 240 + 120 | hands off |
| 12 | (b) circles: the same | 240 + 120 | move the mouse in circles |
| 13 | (c) left: the same | 240 + 60 | hold the left button |
| 14 | (c) right | 240 + 60 | hold the right button |
| 15 | (c) both | 240 + 60 | hold both buttons |
| 16 | (d) calibration: 0x050/52 every frame | 240 + 180 | move fast to the right, lift, repeat |
| 17 | (d) overflow: 0x050/52 once a second (5 reads) | 240 + 300 | keep moving fast to the right |
| 18 | (d) calibration, left | 240 + 180 | move fast to the left, lift, repeat |
| 19 | (d) overflow, left | 240 + 300 | keep moving fast to the left |
| 20 | (e) pad and mouse scan both armed: 0x050/52 every frame | 240 + 180 | move the mouse around steadily |
| 21 | (e) mouse scan alone (the control) | 240 + 180 | the same |
| 254 | done | | turn off, dump the save |

Phases 1-5 also write 0xEE to the step byte while their results are on
screen. Phases (a)-(d) arm the mouse scan alone.

## Running it in the emulator

From the repository's root, with the BIOS files of your own:

```
LoopyMSE probe/LoopyProbe.bin --bios BIOS.bin --sound-bios SOUNDBIOS.bin --no-ini --hidden --fast --input.device=mouse --script probe/emulator-script.txt --out probe/results/emulator --frames 12000
```

The script (`emulator-script.txt`, generated by `gen_emulator_script.py`;
edit that and run it again) waits on the phase byte with `wait-until`, acts
out every mouse phase (motion of 200 counts every 5 frames, 2400 a second,
for the overflow test), takes a `shot` of each result screen, raster test
and mouse phase, dumps the save RAM to `probe-sram.bin` and ends with
`exit 0`; a run that ends any other way (exit code 3 is a `wait-until`
timeout) did not finish. It holds the left button at boot, so a finished
log already in `results/emulator/LoopyProbe.sav` is erased and the probe
runs again. The run takes about 7,800 frames.

Question 3 is about the emulator's *console* timing model, which is not the
default (the default, `stock`, is fast and optimistic). Run it a second
time with that model for those numbers:

```
LoopyMSE probe/LoopyProbe.bin ... --timing.model=console --out probe/results/emulator-console --frames 12000
```

## Decoding

```
python probe/decode_probe.py probe/results/emulator/LoopyProbe.sav
python probe/decode_probe.py probe/results/hardware/LoopyProbe.sav -v > probe/results/hardware/report.txt
```

Standard library only. It finds the log anywhere in the file, in either
16-bit byte order, decodes a partial log as far as it goes (and says where
the run stopped), and ends with one verdict line per finding, for example
"Q1: frame period 267,441.6 cycles (+0 ppm from the reference), 263.00
lines: matches the reference", "Q5: mouse counters WRAP (12-bit; the
emulator's model)". `-v` also lists every record.

## Results

```
results/
  emulator/           LoopyProbe.sav, probe-sram.bin, the shots (*.png), report.txt
  emulator-console/   the same with --timing.model=console
  hardware/           LoopyProbe.sav from the Floopy Drive, photographs, report.txt
```

Each results folder keeps the raw save (`LoopyProbe.sav`, the measurement
itself), the report made from it (`decode_probe.py -v -o report.txt`) and the
pictures. The built `LoopyProbe.bin` is kept in the repository too, so the
probe can be flashed without building it.

## SRAM log

Save RAM at 0x02000000, 32 KB, big-endian, written a byte at a time; each
record's body is written before the count that covers it.

| Offset | Size | Contents |
| --- | --- | --- |
| 0x00 | 4 | "LMPB" |
| 0x04 | 2 | version, 1 |
| 0x06 | 2 | records written (bumped last) |
| 0x08 | 1 | **current phase** (the table above) |
| 0x09 | 1 | flags: 1 done, 2 mouse present, 4 a finished log was erased on request, 8 an unfinished log was replaced, 0x10 CPU exception |
| 0x0A | 1 | the phase the replaced unfinished log had reached |
| 0x0B | 1 | step within the phase (raster test 1-7; 0xEE results on screen) |
| 0x0C | 2 | WCR3 as the BIOS left it |
| 0x0E | 2 | record capacity |
| 0x10 | 12 | build date |
| 0x1C | 8 | build time |
| 0x24 | 4 | vertical blanks counted, at the last phase change |
| 0x28 | 1 | exception vector (4, 6, 9, 10) |
| 0x2C / 0x30 | 4 / 4 | the exception's PC / SR |
| 0x40 | 16 each | records: type, sub, seven 16-bit values (`src/probe.h` lists them) |

## Files

- `src/` the probe: `main.c` (the run), `log.c` (SRAM log), `timing.c`
  (questions 1 and 2), `memcost.c` (3), `raster.c` (4), `mouse.c` (5),
  `screen.c` (display), `probes.s` (the timing loops), `probe.h` (layout,
  phase numbers, record types)
- `boot/`, `include/`, `tools/` the homebrew template (see NOTICE)
- `Makefile`, `build.sh`
- `decode_probe.py`, `gen_emulator_script.py`, `emulator-script.txt`

## NOTICE

LoopyProbe is free software under the GNU General Public License, version 2
or (at your option) any later version. Copyright (C) 2026 Derek Quenneville.

- `boot/`, `include/`, `tools/*.ld` and `tools/fixrom.py` are Kasami's
  [loopy-homebrew-template](https://github.com/kasamikona/loopy-homebrew-template),
  under the zlib licence; its notice is in `LICENSE-loopy-template.txt`.
  Altered: `boot/vectors.c` (no interrupts taken; exceptions reported into
  the log), `boot/startup.s` (a comment), `boot/header.s` (the header
  text), `include/loopy/vdp.h` (the VDP object is volatile) and
  `tools/loopy.ld` (the on-chip RAM section, libgcc's helpers in work RAM).
- From the author's own Loopy projects (GPL-2.0-or-later): the build set-up
  and script (loopy-computer-fun's Paint); the mouse probe's phase structure,
  its 5x7 font and the video set-up (loopy-maniac's `lp_probe.c` and
  `lp_video.c`); the memory probes and the exception stubs (loopy-attorney's
  `lp_bench_probes.s`, `lp_bench_mem.c`); the ITU1 clock (loopy-shang3's
  `lp_clock.c`); the SRAM log pattern (loopy-umihara's `lp_conlog.c`), and
  keeping a finished log and finding it in a word-swapped dump
  (loopy-prince's `lp_proflog.c` and `proflog.py`).
- The hardware facts it tests come from kasami's *Casio Loopy Hardware
  Megadoc* and the author's annotated BIOS disassembly.
