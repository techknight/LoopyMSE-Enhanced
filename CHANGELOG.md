# Changelog

## LoopyMSE-Enhanced 0.5.3-enhanced.1

Based on LoopyMSE 0.5.3.

### Mouse
- XN-110 mouse emulation, modelled on measurements of a real Loopy and
  mouse: 12-bit wrapping counters that only count under mouse-only scan,
  active-low buttons, and pad noise while the mouse is connected. Every
  commercial mouse game detects it. (LoopyMSE pull request #9.)
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
  hardware measurements (`timing.model=console`).

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

### Fixes
- The save file is named by the last dot of the ROM's name: a ROM called
  `Game (v1.0).bin` used to save to `Game (v1.sav`.
- The save file is only rewritten when it has changed.
- An empty value in the ini file no longer discards the whole file.
