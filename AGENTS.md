# Notes for AI agents

**Using the emulator** to run or test Loopy programs: read
[docs/agents.md](docs/agents.md). It is the compact reference for scripts,
the control channel, exit codes and the pitfalls. Run unattended and hidden
(`--hidden --fast --no-ini --no-sram-write --frames N`), take pictures with
`shot`, and never screenshot the window or send it keystrokes.

**Changing the emulator:**
- Build: `cmake -S . -B out/build -DCMAKE_BUILD_TYPE=Release` and
  `cmake --build out/build --config Release`. There are no dependencies
  besides CMake and a C++17 compiler; SDL2 is fetched. See src/BUILDING.md.
- Line endings are LF everywhere (`.gitattributes`); tabs in C++
  (`src/.clang-format`, `.editorconfig`).
- New settings go in `register_settings()` in `src/sdl/options.cpp` (one
  namespace for the ini file and the command line); new script and control
  commands go in `parse_line()` and `execute()` in
  `src/automation/automation.cpp`. Document both in docs/automation.md and
  docs/agents.md, and settings also in assets/loopymse.ini when users would
  set them.
- Keep emulation deterministic: no wall-clock time, threads or host state in
  the emulated machine. Check a change with fixed-input hidden runs (frames
  from `shot`, profiles) before and after; if a change alters emulation on
  purpose, say which change and why in the commit message.
- Report unimplemented hardware with `Log::unimplemented`, not `assert`.
- New per-module state belongs in that module's `save_state`/`load_state`
  (savestates), and scheduled events must carry plain values, not pointers.
- Never commit BIOS files, game ROMs or game save files. The exception is
  `probe/`: its own ROM (`LoopyProbe.bin`) and the save files under
  `probe/results/`, which are the measurements.
