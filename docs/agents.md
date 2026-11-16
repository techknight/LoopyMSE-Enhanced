# LoopyMSE-Enhanced: reference for AI agents

This page is written for AI agents that run Casio Loopy programs in
LoopyMSE-Enhanced to build, test or study them. It is dense on purpose:
exact syntax, exact output formats, and the rules that decide whether a run
means what you think it means. The human-oriented guide is
[automation.md](automation.md). The authoritative list of settings is the
program itself: `LoopyMSE --help-config` (every key, its current value and
its meaning); `LoopyMSE --help` lists the command-line shortcuts.

## Contract

- **No screen, no focus, no desktop input.** Never take screenshots of the
  window or send keystrokes or mouse events to it. Drive input with a script
  (`--script`) or the control channel (`--control`), and take pictures with
  `shot`, which saves the emulator's own framebuffer. Run with `--hidden` so
  no window exists at all.
- **Deterministic.** The same ROM, BIOS, save file, settings and script give
  byte-identical frames, sound, logs and savestates, at any speed, hidden or
  not. Use `--no-ini --no-sram-write` so neither a local ini file nor a
  leftover save changes the result.
- **Frame-exact.** Time is counted in emulated frames from power-on (59.73
  per emulated second, 267,892 CPU cycles each as measured on a console). Wall-clock time means
  nothing; never sleep and hope.
- **Every run ends with an exit code** that says how it ended (below).
  Always check it.

## Minimal recipes

Boot, run 600 frames, keep the last picture:
```
LoopyMSE game.bin --bios bios.bin --sound-bios soundbios.bin --no-ini --no-sram-write \
    --hidden --fast --frames 600 --automation.shot_at=600 --out run1 --log log.txt
# picture: run1/frame_000600.png   log: run1/log.txt   exit code: 0
```

Scripted test with checks:
```
# test.txt
@300 press start
wait-until mem.b 0x09001234 == 2 timeout 1800   # program state in RAM
shot level2
assert mem.w 0x09001240 >= 100 "score too low"
exit 0
```
```
LoopyMSE game.bin ... --hidden --fast --script test.txt --frames 20000 --out run2
```
Exit code 3 means the wait timed out, 4 that the assert failed, 0 that the
script reached `exit 0`, and 0 with "frame limit reached" in the log that the
script never got there (pair `--frames` with an explicit `exit` and treat a
frame-limit exit as a failure if you expected the script to finish).

Interactive session (`tools/loopymse_control.py` wraps this):
```
LoopyMSE game.bin ... --control --paused --hidden
> step 300            < ok 300 step 300
> press start         < ok 300 press start
> step 60             < ok 360 step 60
> shot title          < shot 360 /abs/path/title.png
                      < ok 360 shot title
> status              < status frame=360 paused=1 device=pad speed=1 unimplemented=0
> exit 0              < ok 360 exit 0
                      < exit 360 0
```

Skip a long boot in many runs: save once, then start every later run from
the state.
```
run A: script "@3000 save-state boot.state"   (written in paths.output)
run B: LoopyMSE game.bin ... --load-state runA/boot.state --script test.txt
```

## Frame semantics (get these right)

- `frame` = number of frames emulated so far. A command scheduled at frame F
  runs before frame F+1 is emulated.
- Input set at F is what the program sees from the next emulated frame on.
- `shot` at F saves the picture of the F-th frame. `automation.shot_at=F`
  and `shot_every=N` use the same numbering (`shot_every` fires at N, 2N...).
- `--frames N` (`automation.exit_after_frames`) stops once N frames are
  done, after running the commands due at N.
- The count goes on across `reboot`/F12. `load-state` sets it to the frame
  the state was saved at.

## Command language

One command per line; `#` starts a comment (not inside quotes); numbers are
decimal or `0x` hex; `"..."` quotes text (`\"`, `\n` escapes). Script commands
run in order; a command waits for the one before it.

```
line      := [prefix] command
prefix    := "@" FRAME        # wait until FRAME frames are done, then run
           | "+" N            # wait N frames, then run
```
A past `@FRAME` runs immediately.

| Command | Blocks? | Effect |
| --- | --- | --- |
| `press BTNS [N=6]` | no | hold for N frames, then release |
| `hold BTNS` / `release BTNS` / `release all` | no | |
| `set BTNS` | no | exactly these held (`set none`); cancels pending presses |
| `plug mouse` / `plug pad` | no | swap the controller (only one is connected) |
| `mouse move DX DY` | no | counts; +DY is up |
| `mouse down\|up left\|right` | no | |
| `mouse click left\|right [N=6]` | no | |
| `mouse to X Y` | no | autodrive cartridges only (see homebrew-debug-interface.md); otherwise a warning |
| `wait N` | yes | |
| `wait-until COND [timeout N]` | yes | timeout: exit code 3 |
| `assert mem[.b\|.w\|.l] ADDR OP VALUE ["msg"]` | no | false: exit code 4 |
| `shot [NAME]` | no | PNG of the raw picture (256 x 224 or 240) |
| `dump WHAT FILE` | no | raw bytes; WHAT = ram sram bitmap tiles oam palette onchip |
| `save-state FILE` / `load-state FILE` | no | failure: exit code 1 |
| `reboot` | no | |
| `speed X` | no | 0 = unlimited |
| `echo TEXT` | no | to the log (control channel: an `echo` reply) |
| `exit [CODE=0]` | — | ends the run |
| `pause` / `resume` / `step [N=1]` / `status` | step blocks | control channel only |

BTNS: `up down left right a b c d start l r` joined with `+`, or `none`.

Conditions:
- `write ADDR [VALUE]`: any CPU or DMA write touching ADDR; with VALUE, a
  write that starts at ADDR and stores exactly VALUE (compare the full
  written width). Mirrors are folded (`0x0C05xxxx` = `0x0405xxxx`).
- `serial PORT "TEXT"`: TEXT appeared in what serial port PORT (0 or 1) sent
  since the wait began.
- `mem[.b|.w|.l] ADDR OP VALUE`: big-endian read at the frame boundary,
  default 32 bits; OP is `== != < > <= >= &` (`&`: any bit set). Read RAM,
  not I/O registers.

## Control channel protocol

- Commands on stdin, one per line, executed in arrival order (one waiting
  command holds back the rest). Replies on stdout, one line each:
  `ok F <line>` when a line's command has finished, `error F <message>`,
  `shot F <path>`, `echo F <text>`, `status frame=F paused=0|1
  device=pad|mouse speed=S unimplemented=N` (no `ok` after it),
  `exit F CODE` when the run ends.
- The log goes to stderr (or `--log FILE`), so stdout carries only replies.
- `--paused` holds at frame 0. Waits only progress while emulation runs:
  `resume` or `step` first, or the wait never ends.
- Closing stdin ends the run with code 0 after the queued commands finish.

## Exit codes

0 ok (`exit`, frame limit, window closed, stdin closed) · 1 bad command
line, settings, ROM, script, savestate or dump · 2 `--strict` and the program
used unimplemented hardware or faulted (address error) · 3 `wait-until`
timeout · 4 `assert` failed · other: the code given to `exit`.

## Settings that matter for agents

Any setting: `--section.key=value` (or `--set section.key=value`); the
command line beats the ini file; `--no-ini` ignores the file.

| Shortcut | Setting | Note |
| --- | --- | --- |
| `--hidden` | `automation.window=hidden` | no window at all |
| `--fast` | `emulator.speed=0` | unlimited; use for every unattended run |
| `--frames N` | `automation.exit_after_frames` | always set one as a safety net |
| `--script F` | `automation.script` | |
| `--control` / `--paused` | `automation.control=stdin` / `automation.start_paused` | |
| `--out DIR` | `paths.output` | relative output files go here |
| `--log F` | `debug.log_file` | |
| `--no-sram-write` | `cart.sram_write=false` | reproducible runs |
| `--mouse` | `input.device=mouse` | `auto` plugs it for known mouse games |
| `--strict` | `debug.strict` | fail on unimplemented hardware or faults |
| `--load-state F` | `automation.load_state` | |
| `--record F` | `record.video` | needs ffmpeg |
| `--profile F` | `profile.file` | with `profile.from` / `profile.to` |

Unattended runs (a script, the control channel, a non-normal window, a frame
limit, scheduled shots or a profile) default to: no pausing when unfocused,
prints not opened in a viewer, and no sound output when the window is hidden
or speed is unlimited. While a script or the control channel runs, the real
keyboard, controller and mouse are ignored.

Relative output paths (log, profile, serial and watch logs, recordings,
dumps, `save-state`, `cart.sram`) go into `paths.output` when it is set.
`load-state` with a relative name looks in `paths.output` first.

## Observing the program

| Want | Use |
| --- | --- |
| The program's printf (SCI0) | `--debug.serial0_log=out.txt`, or `wait-until serial 0 "TEXT"` |
| The MIDI stream with timing | `--debug.serial1_log=midi.txt --debug.serial_log_timed=true` (lines `FRAME CYCLE BYTE`) |
| Every write to an address | `--debug.watch_writes=0x0C059050,0x09000000:16 --debug.watch_log=w.txt` (frame, line, PC, value) |
| Pad/mouse register accesses | `--debug.trace_io=true` |
| Palette/OAM writes during display | `--debug.raster_warnings=true` (approximate; hints only) |
| The picture without raster snow | `--video.raster_artifacts=false` (default true: palette, OAM and tile VRAM accesses during the active picture disturb it, as on a console) |
| Unimplemented hardware used | grep the log for `[UNIMPLEMENTED]` and `[FAULT]`, or `--strict` |
| Where CPU time goes | `--profile p.txt --profile.from=A --profile.to=B` (`ADDR cycles` lines); add `--timing.model=console` for console-like costs |
| Memory contents | `dump ram ram.bin` (work RAM at 0x09000000, 512 KB) |
| Video and sound | `--record out.mp4`, or `--record.audio=a.wav`, `--record.frames=DIR` |

## Pitfalls

- A wait never progresses while paused (control channel).
- Mouse motion only counts while the program has armed mouse-only scanning;
  motion sent earlier is lost, as on hardware. Plug the mouse in before the
  program checks for it (most check at boot): `input.device=mouse` or `auto`.
- Most games treat one pixel of cursor motion as two counts.
- `press` lasts 6 frames by default; a game that polls slowly may need more.
- `shot` pictures are raw (256 pixels wide, 224 or 240 lines), not aspect
  corrected.
- A recording is finished (video and sound muxed) only when the run ends
  normally: end it with `exit`, `--frames` or by closing stdin, never by
  killing the process (that leaves `FILE.video.mkv` and `FILE.audio.wav`).
- The save file (`<rom>.sav` beside the ROM) is written while the program
  runs; use `--no-sram-write` or `--out` so test runs do not change it.
- Savestates load only into the same build and the same cartridge; the
  synthesizer restarts silent after a load (the program is unaffected).
- The default timing model is fast and optimistic; judge performance with
  `--timing.model=console`, and judge final timing on hardware.
- Dots, streaks, dashes or missing sprites in a `shot` can be raster
  artifacts: the program touched the palette, OAM or tile VRAM while a line
  was drawn, which disturbs a console's picture too. Check with
  `--debug.raster_warnings=true`; move those accesses into the blanking.
- Exit code 0 with "frame limit reached" means the script did not finish.
