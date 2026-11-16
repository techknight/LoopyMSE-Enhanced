"""Drive LoopyMSE-Enhanced over its control channel (stdin/stdout).

    from loopymse_control import LoopyMSE

    with LoopyMSE("LoopyMSE", "game.bin", "--bios", "bios.bin") as emu:
        emu.step(300)              # run 300 frames, then pause
        emu.send("press start")
        emu.step(120)
        print(emu.shot("title"))   # path of the saved screenshot
        print(emu.status())        # {'frame': '420', 'paused': '1', ...}

Every command of docs/automation.md can be sent with send(); it returns the
reply line once the command has finished, and raises CommandError on an
"error" reply. The emulator runs with no window (--hidden) and starts paused
(--paused), so nothing happens until you step or resume.

Run this file directly for a small demo:
    python loopymse_control.py PATH/TO/LoopyMSE game.bin [more emulator options]
"""

import subprocess
import sys


class CommandError(RuntimeError):
    pass


class LoopyMSE:
    def __init__(self, exe, rom, *options, hidden=True, paused=True):
        args = [exe, rom, "--control"]
        if hidden:
            args.append("--hidden")
        if paused:
            args.append("--paused")
        args.extend(options)
        self.process = subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1
        )
        self.events = []  # shot/echo lines received while waiting

    def send(self, command):
        """Send one command and return its final reply line."""
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise CommandError("the emulator exited (code %s)" % self.process.wait())
            line = line.rstrip("\n")
            kind = line.split(" ", 1)[0]
            if kind in ("ok", "status", "exit"):
                return line
            if kind == "error":
                raise CommandError(line)
            self.events.append(line)

    def step(self, frames=1):
        return self.send("step %d" % frames)

    def shot(self, name=""):
        self.send(("shot " + name).strip())
        for line in reversed(self.events):
            if line.startswith("shot "):
                return line.split(" ", 2)[2]
        return None

    def status(self):
        fields = self.send("status").split()[1:]
        return dict(f.split("=", 1) for f in fields)

    def close(self, code=0):
        if self.process.poll() is None:
            try:
                self.send("exit %d" % code)
            except CommandError:
                pass
            self.process.stdin.close()
            self.process.wait()
        return self.process.returncode

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    with LoopyMSE(sys.argv[1], sys.argv[2], *sys.argv[3:]) as emu:
        print(emu.status())
        emu.step(300)
        emu.send("press start")
        emu.step(120)
        print("screenshot:", emu.shot("demo"))
        print(emu.status())
