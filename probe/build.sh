#!/bin/bash
# LoopyProbe: build the ROM (from loopy-computer-fun Paint's scripts/build.sh).
#
# Finds the Wonderful toolchain (gcc-sh-elf 13.1, picolibc) in either place
# it normally lives, then builds. When the tree is on a Windows drive
# (/mnt/...), the build runs against a WSL-native copy instead: drvfs is
# slow enough that copying the tree twice is still faster than compiling
# on it. From Windows:
#
#     wsl -e bash /mnt/g/LoopyMSE-Enhanced/probe/build.sh
#
# Usage: build.sh [make args...]   (e.g. clean)
set -e

for d in /opt/wonderful "$HOME/wonderful"; do
    if [ -x "$d/toolchain/gcc-sh-elf/bin/sh-elf-gcc" ]; then
        WF="$d"
        break
    fi
done

if [ -z "$WF" ]; then
    echo "Wonderful toolchain not found in /opt/wonderful or \$HOME/wonderful." >&2
    exit 1
fi

SRC="$(cd "$(dirname "$0")" && pwd)"
MAKE="$(command -v gmake || command -v make)"

case "$SRC" in
    /mnt/*)
        WORK="$HOME/.cache/loopy-probe-build"
        mkdir -p "$WORK"
        # --delete takes away a source removed from the tree; obj/, which
        # lives only here and makes the build incremental, is kept.
        rsync -a --delete --exclude obj/ --exclude results/ \
              --exclude '*.bin' --exclude '*.elf' --exclude '*.sav' \
              "$SRC/" "$WORK/"
        "$MAKE" -C "$WORK" WONDERFUL_TOOLCHAIN="$WF" "$@"
        for f in LoopyProbe.bin LoopyProbe.elf; do
            if [ -f "$WORK/$f" ]; then
                cp "$WORK/$f" "$SRC/$f"
            else
                rm -f "$SRC/$f"
            fi
        done
        ;;
    *)
        "$MAKE" -C "$SRC" WONDERFUL_TOOLCHAIN="$WF" "$@"
        ;;
esac
