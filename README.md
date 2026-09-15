# LoopyMSE
A Casio Loopy emulator. WIP, plays commercial games with sound.

Features:
- Runs all commercial games
- High-level printer emulation
- Sound emulation
- High-level PCM expansion audio emulation
- Keyboard and controller input, customizable bindings
- Mouse emulation
- Aspect ratio correction
- Available on MacOS, Windows, and Linux

Features still TODO:
- GUI for in-app configuration
- Internal "demo" music used by some games
- Improved / low-level printer emulation

## Builds

Check the [Releases page](../../releases) to get LoopyMSE for Linux, Windows, MacOS Intel, MacOS ARM64.

Rolling builds are available for every commit on [Github Actions](../../actions/).

## Usage
See the [Readme](assets/README.md) (included in builds).

## Dev automation (local development fork only)
Extra command line options for deterministic, scripted runs:

| Option | Meaning |
| ------ | ------- |
| `--screenshot-every N` | Save the raw display buffer (unscaled, like F10) every N emulated frames, i.e. frames 0, N, 2N... (0 = off) |
| `--screenshot-at LIST` | Comma-separated emulated frame numbers to capture (combinable with `--screenshot-every`) |
| `--screenshot-dir DIR` | Output directory, files are named `frame_000123.png` (default: current directory) |
| `--exit-after-frames N` | Quit cleanly after N emulated frames |
| `--input-script FILE` | Drive the gamepad from a frame-based script; real keyboard/controller pad input is ignored |
| `--headless-fast` | No frame throttling or vsync, run as fast as possible (window is still drawn) |
| `--no-sram-write` | Never write SRAM back to the cartridge `.sav` file |

Frame 0 is the first emulated frame after boot. A screenshot for frame F is taken right after frame F is emulated.
When any automation option is given, emulation does not pause when the window loses focus.

Input script format: one entry per line, `FRAME BUTTONS`. FRAME is a decimal frame number, BUTTONS is a `+`-joined set of
`up down left right a b c d start l r`, or `none`. The state applies from the start of FRAME and persists until the next
entry. `#` starts a comment, blank lines are ignored.

```
0   none
120 start   # press start for 6 frames
126 none
200 right+a
```

## Credits and Special Thanks
* PSI - original (upstream) [LoopyMSE](https://github.com/PSI-Rockin/LoopyMSE) project
* kasami - reverse engineering, accuracy improvements, bug fixes, sound & printer implementation, BIOS dumping
* partlyhuman - continued development, testing, automated builds, visual and quality-of-life improvements
* UBCH discord server - documentation and archival, translations, moral support
