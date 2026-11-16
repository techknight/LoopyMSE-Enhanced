# LoopyMSE-Enhanced

A Casio Loopy emulator, based on [LoopyMSE](https://github.com/LoopyMSE/LoopyMSE),
with the XN-110 mouse, scripted and remote-controlled runs that need no screen,
CPU profiling and a measured console timing model, aimed at playing the
library and at developing new Loopy software.

Everything LoopyMSE does:
- Runs all commercial games, with sound
- High-level printer emulation
- High-level PCM expansion audio emulation
- Keyboard and controller input, customizable bindings
- Aspect ratio correction
- macOS, Windows and Linux

Plus:
- **XN-110 mouse.** Register behaviour measured on a real Loopy and mouse.
  Every commercial mouse game is detected and playable. Known mouse games get
  the mouse plugged in automatically.
- **Automation.** A frame-exact [command language](docs/automation.md)
  covers pad, mouse, waits on memory, serial text or register writes,
  asserts, screenshots, reboot and exit. It runs from a script file or live
  over stdin, with a defined exit code for every outcome. Runs are
  deterministic, can go faster than real time, and need no visible window,
  focus or desktop input.
- **Homebrew support.** A cartridge can steer the emulated mouse (autodrive).
  There are clean channels for a program to report progress to a test run,
  and a strict mode that fails a run when the program touches unimplemented
  hardware ([details](docs/homebrew-debug-interface.md)).
- **Performance work.** A per-address CPU profile, and a console timing model
  built from hardware measurements, which charges data accesses, DMA and
  wait states that the stock model leaves out.
- **One settings namespace.** Every ini key can be overridden on the command
  line as `--section.key=value`; `--help-config` lists them all.

## Builds

Check the [Releases page](../../releases), or the rolling builds on
[GitHub Actions](../../actions/). To build it yourself, see
[src/BUILDING.md](src/BUILDING.md).

## Usage

See the [Readme](assets/README.md) (included in builds) for playing, and
[docs/automation.md](docs/automation.md) for unattended runs:

```
LoopyMSE game.bin --script route.txt --hidden --fast --out results
```

## Credits and Special Thanks
* PSI - original [LoopyMSE](https://github.com/PSI-Rockin/LoopyMSE) project
* kasami - reverse engineering, accuracy improvements, bug fixes, sound & printer implementation, BIOS dumping
* partlyhuman - continued development, testing, automated builds, visual and quality-of-life improvements
* UBCH discord server - documentation and archival, translations, moral support
