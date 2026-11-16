# Tools

Small helpers for driving LoopyMSE-Enhanced from other programs. See
[docs/automation.md](../docs/automation.md) for the command language.

- **loopymse_control.py**: a Python client for the control channel
  (`--control`). Step frames, send commands, take screenshots and read the
  status from a test or a notebook.
- **run-quiet.ps1**: starts the emulator on Windows without disturbing the
  desktop (minimised, never activated, output to a log file), waits, and
  returns its exit code. Use it for unattended runs while you keep working.
- **example-script.txt**: a small annotated script.
