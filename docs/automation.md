# Automation

LoopyMSE-Enhanced can run a Loopy program with nobody at the keyboard: a
**script** file, or a **control channel** on stdin, drives the pad and the
mouse frame by frame, waits for things to happen inside the machine, takes
screenshots and ends the run with an exit code. No window has to be visible
or focused, and no keystrokes or mouse events are sent to the desktop, so you
can keep using the computer while runs go on in the background.

```
LoopyMSE game.bin --script walkthrough.txt --hidden --fast --out run1
```

## Frames

Everything is counted in emulated frames (59.826 a second, the console's
own rate) from power-on.
**Frame F** is the moment F frames have been emulated:

- a command due at frame F runs before the next frame is emulated, so input
  set at frame F is what the game sees from then on;
- a screenshot taken at frame F shows the picture of the F-th frame.

A reboot (F12 or the `reboot` command) does not reset the count. Runs are
deterministic: the same ROM, save file and script give the same frames, at
any speed and with or without a window.

## Running unattended

| Option | Meaning |
| --- | --- |
| `--script FILE` | Run a command script (`automation.script`) |
| `--control` | Read commands from stdin (`automation.control=stdin`) |
| `--paused` | With `--control`: hold at frame 0 until `resume` or `step` |
| `--hidden` | No window at all (`automation.window=hidden`); `minimized` is the other choice |
| `--fast` | As fast as possible (`emulator.speed=0`); any other number is a speed factor |
| `--frames N` | Quit after N frames (`automation.exit_after_frames`) |
| `--out DIR` | Put everything the run writes in DIR: screenshots, prints, the save file, the log, the profile (`paths.output`) |
| `--no-sram-write` | Never write the save file (`cart.sram_write=false`) |
| `--automation.shot_every=N` | A screenshot every N frames |
| `--automation.shot_at=F,F,...` | Screenshots at these frames |
| `--log FILE` | Write the log to FILE (`debug.log_file`) |
| `--strict` | Stop with exit code 2 when the program touches hardware the emulator does not implement |
| `--no-ini` | Ignore the ini file: only built-in defaults and the command line count |

A run with a script, the control channel, a hidden or minimised window,
scheduled screenshots, a frame limit or a profile counts as **unattended**:
it keeps running in the background, never takes the focus, never opens
prints in a viewer, and with a hidden window or at unlimited speed it makes
no sound. Settings given on the command line still win. While a script or
the control channel is active, the real keyboard, game controller and mouse
are ignored, so nothing you do at the computer can disturb the run.

Screenshots are the raw 256-pixel-wide picture, as PNG by default
(`screenshot.image_type`), named `frame_NNNNNN.png` or by the name given to
`shot`.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Finished: `exit`, the frame limit, the window closed, or stdin closed on the control channel |
| 1 | Bad command line, settings, ROM or script |
| 2 | The program used unimplemented hardware and `debug.strict` is on |
| 3 | A `wait-until` ran out of time |
| 4 | An `assert` failed |
| other | The code given to `exit` |

## Scripts

One command per line; `#` starts a comment. Commands run in order. A line may
start with a frame prefix:

- `@F command` waits until frame F, then runs the command;
- `+N command` waits N frames, then runs the command.

Without a prefix a command runs straight after the one before it, in the same
frame. Numbers may be decimal or hexadecimal (`0x...`).

```
# Start the game and save a picture of the first room
@120 press start
+90  press a
wait-until mem.b 0x09001234 == 1 timeout 600   # the room number the game keeps in RAM
shot first-room
exit
```

### Pad

Buttons: `up down left right a b c d start l r`, joined with `+`
(`a+right`), or `none`.

| Command | Effect |
| --- | --- |
| `press BUTTONS [FRAMES]` | Hold the buttons for FRAMES frames (default 6), then let go |
| `hold BUTTONS` | Hold the buttons until released |
| `release BUTTONS` / `release all` | Let go |
| `set BUTTONS` | Make exactly these buttons held (`set none` lets go of everything) |

### Mouse

| Command | Effect |
| --- | --- |
| `plug mouse` / `plug pad` | Plug in the XN-110 mouse or the pad (like F2 / F1) |
| `mouse move DX DY` | Move the mouse by DX, DY counts (positive DY is up) |
| `mouse down left\|right`, `mouse up left\|right` | Press or release a button |
| `mouse click left\|right [FRAMES]` | Press for FRAMES frames (default 6) |
| `mouse to X Y` | Steer the cursor to game pixel X, Y; needs a cartridge that publishes its cursor (`input.autodrive`, see [homebrew-debug-interface.md](homebrew-debug-interface.md)) |

The mouse goes through the real XN-110 registers, so it behaves as on the
console: the counters only count while the game has armed mouse scanning, and
most games treat one game pixel as two counts. Plug the mouse in before the
game looks for it (most games check at boot), or start with
`input.device=mouse` or `auto`.

### Waiting

| Command | Effect |
| --- | --- |
| `wait FRAMES` | Wait |
| `wait-until CONDITION [timeout FRAMES]` | Wait until the condition holds; on timeout the run ends with code 3 |

Conditions:

- `write ADDR [VALUE]`: the program wrote to ADDR (with VALUE: a write
  starting at ADDR that stored exactly VALUE). Mirrors are folded, so
  `0x0C059050` and `0x04059050` are the same register.
- `serial PORT "TEXT"`: serial port 0 or 1 has sent TEXT since the wait began
  (port 0 is where homebrew usually prints).
- `mem ADDR OP VALUE`, `mem.b`, `mem.w`, `mem.l`: compare memory (default 32
  bits) at the frame boundary. OP is `==`, `!=`, `<`, `>`, `<=`, `>=`, or `&`
  (any of the bits set). Read RAM this way, not I/O registers: a register
  read can have side effects.

### Other commands

| Command | Effect |
| --- | --- |
| `shot [NAME]` | Save a screenshot (in `paths.screenshots`) |
| `dump WHAT FILE` | Write raw memory: `ram` (work RAM), `sram` (the save), `bitmap` (bitmap VRAM), `tiles` (tile VRAM), `oam`, `palette`, `onchip` |
| `save-state FILE` / `load-state FILE` | Save or load a savestate (see below) |
| `assert mem ADDR OP VALUE ["message"]` | End the run with code 4 if the comparison fails |
| `reboot` | Power-cycle the Loopy (the frame count goes on) |
| `speed FACTOR` | Change speed (0 = as fast as possible) |
| `echo TEXT` | Write TEXT to the log (or reply with it on the control channel) |
| `exit [CODE]` | End the run |

## The control channel

With `--control`, the emulator reads the same commands from stdin, one per
line, and answers on stdout. The log moves to stderr (or to `--log FILE`),
so stdout carries only the replies:

| Reply | When |
| --- | --- |
| `ok FRAME COMMAND` | a command finished (a `wait` or `wait-until` finishes when its wait is over) |
| `error FRAME MESSAGE` | a command could not be parsed or failed |
| `shot FRAME PATH` | a screenshot was saved |
| `echo FRAME TEXT` | answer to `echo` |
| `status frame=F paused=0\|1 device=pad\|mouse speed=S unimplemented=N` | answer to `status` |
| `exit FRAME CODE` | the run is ending |

Commands run in the order they arrive; a command that waits holds back the
ones after it. Four more commands only exist here:

| Command | Effect |
| --- | --- |
| `pause` | Stop emulating |
| `resume` | Go on |
| `step [N]` | Emulate N frames (default 1), then pause; answers once they are done |
| `status` | Report the frame number and state |

Waits only make progress while emulation runs: `resume` before a
`wait-until`, or `step` through it. When stdin closes, the run ends with
code 0.

A typical driver starts the emulator with `--control --paused --hidden`,
then steps and inspects, for example from Python:

```python
import subprocess
emu = subprocess.Popen(["LoopyMSE", "game.bin", "--control", "--paused", "--hidden"],
                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
def send(cmd):
    emu.stdin.write(cmd + "\n"); emu.stdin.flush()
    while True:
        reply = emu.stdout.readline().strip()
        if reply.split(" ")[0] in ("ok", "error", "status", "exit"):
            return reply
send("step 300")
send("press start")
send("step 120")
print(send("shot title"))
send("exit")
```

## Savestates

`save-state FILE` in a script (or on the control channel) saves the whole
machine at that frame boundary; `load-state FILE` or `--load-state FILE` (at
startup) brings it back, frame number included, so a run started from a state
continues exactly as the run that saved it. In the window, Shift+F5 to
Shift+F8 save to slots 1-4 and F5 to F8 load them (files named
`<rom>.state1` to `.state4`, in `paths.states`, `paths.output` or beside the
ROM).

A state only loads into the same build of the emulator with the same
cartridge. The sound synthesizer is not saved: after a load it starts silent
and picks up from the next notes the game sends, which never affects the game
itself.

## Recording

| Setting | Meaning |
| --- | --- |
| `record.video=FILE` (`--record FILE`) | Video with sound, through ffmpeg: `.mp4`, `.mkv`, ... |
| `record.audio=FILE` | The sound as a 48 kHz 16-bit stereo `.wav` |
| `record.frames=DIR` | Every frame as a PNG (no ffmpeg needed) |
| `record.scale=N` | Video pixel scale (default 3: 768x720) |
| `record.quality=N` | x264 CRF, lower is better (default 16) |
| `record.ffmpeg=PATH` | The ffmpeg program (default: `ffmpeg` on the PATH) |

Everything is recorded in emulated time: every emulated frame is one video
frame at the Loopy's real frame rate, and the sound is rendered in step with
emulation instead of by the sound card. A recording made hidden at
`--fast` therefore plays back at normal speed, and two runs of the same
script give identical files.

```
LoopyMSE game.bin --script demo.txt --hidden --fast --record demo.mp4
```

## Debugging

| Setting | Meaning |
| --- | --- |
| `debug.serial0_log=FILE`, `debug.serial1_log=FILE` | Everything a serial port sends (port 0: the program's printf; port 1: the MIDI stream to the sound chip) |
| `debug.serial_log_timed=true` | Write one `FRAME CYCLE BYTE` line per byte instead of raw bytes |
| `debug.watch_writes=ADDR[:LEN],...` | Log every write to these addresses with the frame, line and program counter |
| `debug.watch_log=FILE` | Where the watch lines go (default: the log) |
| `debug.trace_io=true` | Log every pad, mouse and sensor register access |
| `debug.raster_warnings=true` | Report palette and OAM writes that land on a line being drawn: on a console they show as dots. The emulator's line timing is approximate (DMA, for one, is instant), so treat reports as pointers, not proof |
| `debug.log_frames=true` | Prefix every log line with the frame number |
| `debug.strict=true` (`--strict`) | End with exit code 2 at the first use of hardware the emulator does not implement (each kind is logged once as `[UNIMPLEMENTED]`) |

## CPU profile and timing model

`--profile FILE` (`profile.file`) with `profile.from` and `profile.to` writes
the CPU cycles spent at every program address over those frames, one
`ADDR cycles` line each, for cartridge ROM and work RAM.
`profile.watch=ADDR` records a 32-bit value (say, the game's own frame
counter) at the start and the end of the window, so two builds can be
compared over the same amount of game progress.

`timing.model=console` charges every instruction what a real console was
measured to charge (data accesses, literal loads, DMA, work-RAM rows, the
VDP areas, cartridge wait states), instead of the stock model, which charges
instruction fetches only and makes data-heavy code look up to twice as fast
as it really is. `timing.cart_rom_cycles` overrides the cartridge ROM cost
for what-if experiments. With the console model, a profile also lists what
the cycles were spent on.
