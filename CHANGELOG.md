# Changelog

## LoopyMSE-Enhanced 0.5.3-enhanced.1

Based on LoopyMSE 0.5.3.

### Mouse
- XN-110 mouse emulation, modelled on measurements of a real Loopy and
  mouse (LoopyProbe): signed 12-bit counters that saturate and only count
  under mouse-only scan, active-low buttons, and the mouse's raw pins in
  every controller byte (detect, buttons and the encoders' gray code, which
  changes only with motion). Every commercial mouse game detects it.
  (LoopyMSE pull request #9.)
- `input.device=auto` plugs the mouse in for the known mouse games.
- Autodrive: a cooperating homebrew cartridge can steer the mouse.

### Automation
- A frame-exact command language for scripts (`--script`) and a live control
  channel on stdin (`--control`). It covers pad and mouse input, waits on
  frames, memory, serial text and register writes, asserts, screenshots,
  dumps, savestates, reboot, speed and exit.
- Runs that need no screen: a hidden or minimised window that never takes
  the focus, unlimited speed, scheduled screenshots, a frame limit, and
  defined exit codes.
- Savestates: script and control commands, `--load-state`, and F5-F8 slots
  (Shift to save).
- Recording of video and sound in emulated time (`--record`, `record.*`).

### Development tools
- Serial port logs, write watches, an I/O register trace, raster warnings,
  and a strict mode for unimplemented hardware.
- A CPU profile per program address, and a console timing model built from
  hardware measurements (`timing.model=console`); BIOS code runs at on-chip
  speed in it, as LoopyProbe measured.

### Settings
- One namespace for the ini file and the command line: every
  `section.key` can be given as `--section.key=value`. `--help-config`
  lists them all. The ini file layout changed (see `assets/loopymse.ini`).
- Boost is no longer needed to build.

### Accuracy
- 16-bit and byte reads of the ITU counters and general registers.
- The bus state controller registers (WCR1-3 and others) read back.
- Serial frames take their full length (start, data, parity and stop bits,
  so MIDI no longer runs 25 % fast); TEND and the TXI/TEI interrupts.
- An interrupt held back by the CPU's mask is taken once the mask is
  lowered.
- The DMA transfer-end interrupt; the 16-bit halves of the DMA address
  registers and 32-bit writes of the count and control registers.
- Unimplemented register accesses and modes are reported (and counted for
  strict mode) instead of asserting.

### Hardware documentation (kasami's megadoc, the BIOS disassembly)
- Raster timing as measured on an NTSC console: a line is 1365 VDP cycles,
  a frame 263 lines and 267,892 CPU cycles (LoopyProbe's measurement, 0.17%
  more than the nominal clocks give), 59.73 frames a second (it was
  60.06). The line is drawn and VCOUNT counts up as the active pixels end,
  so register writes in the blanking land on the next line; HCOUNT reads the
  real counter (0-0x101, then 0x1AC-0x1FF); IRQ0 fires at HCMP (and VCMP).
- IRQ2 when a scanline capture or an ADC conversion is done; ITU compare
  match A requests DMA (one unit each), as the BIOS's printing expects.
- Tiles and objects larger than 8x8 wrap their characters within a row of
  8, as the VDP does.
- Cartridge wiring: the detect line follows PB1 on boards wired that way
  (Lupiton no Wonder Palette: the "Magical Shop unlock"), and each board's
  port B pull-ups; `cart.det_wiring` overrides it for homebrew.
- Registers that read back what was written instead of being reported:
  expansion timing, printer motor and head control (with their keys and the
  printer enable), controller output, the ADC channel, PFC pin functions,
  CASCR, VDP interrupt and capture control, BM_MEM_CTRL, sync calibration.
- Blend modes 2, 3, 6 and 7; BM_CTRL aliases; capture mode 0.
- Stores to ROM are ignored. Printing without a cassette or in an invalid
  mode returns 2; mode 4 no longer hangs.

### LoopyProbe
- `probe/`: a homebrew ROM that measures, on a console, the points where
  the documentation and the emulator disagree (frame period, raster
  counters, memory costs, raster artifacts, the mouse), logs them to SRAM
  and decodes them into a report. The results of a console (with
  photographs) and of the emulator are included.
- From the console's results: the frame length (267,892 CPU cycles), BIOS
  fetches at on-chip speed in the console timing model, the mouse's pins
  and saturating counters (see Mouse), and raster artifacts: palette, OAM
  and tile VRAM accesses while a line is drawn disturb the picture as they
  do on a console (`video.raster_artifacts`, on by default).

### Fixes
- The save file is named by the last dot of the ROM's name: a ROM called
  `Game (v1.0).bin` used to save to `Game (v1.sav`.
- The save file is only rewritten when it has changed.
- An empty value in the ini file no longer discards the whole file.
