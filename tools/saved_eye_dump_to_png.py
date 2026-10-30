#!/usr/bin/env python3
"""Convert an eye picture kept by menu ROOT > SAVE LOG (native/src/diagnostics/SavedLog.h) to PNG, from a local folder.

With debug.rdrvr.savelog.eyes 1 set before the press, SAVE LOG keeps saved-logs/log-<stamp>.json + log-<stamp>-eye-dump.json,
log-<stamp>-eye-dump-*.raw (and the AppSW half, -eye-dump-warp*).
Pull the folder first (the 6 newest pictures are about 110 MB each):
  adb pull /sdcard/Android/data/com.rockstargames.rdr/files/rdrvr/saved-logs

Usage: saved_eye_dump_to_png.py <folder> [log-<stamp>] [--all] [--tool <eye_dump_to_png.py>]
  no stamp: the newest picture in the folder; --all: every picture.
Output: <folder>/log-<stamp>-png/ (R-color.png, L-color.png, ...). The raw files are hard-linked (or copied) under the names
eye_dump_to_png.py --local expects, for the time of the conversion, and removed again. The converter is looked for in --tool,
$RDRVR_EYE_DUMP_TOOL, then next to this script. Needs numpy (and Pillow for PNG).
"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

TAG = re.compile(r"^(log-[0-9-]+)-(eye-dump.*\.(?:json|raw))$")


def find_tool(explicit):
    here = Path(__file__).resolve().parent
    candidates = [explicit, os.environ.get("RDRVR_EYE_DUMP_TOOL"), here / "eye_dump_to_png.py"]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    sys.exit("eye_dump_to_png.py not found: pass --tool or set RDRVR_EYE_DUMP_TOOL")


def pictures(folder):
    found = {}
    for path in folder.iterdir():
        match = TAG.match(path.name)
        if match:
            found.setdefault(match.group(1), []).append((path, match.group(2)))
    return found


def place(source, target):
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)


def convert(tool, folder, stamp, files):
    out = folder / f"{stamp}-png"
    out.mkdir(exist_ok=True)
    placed = []
    try:
        for source, name in files:
            target = out / name
            target.unlink(missing_ok=True)
            place(source, target)
            placed.append(target)
        print(f"== {stamp}: {len(files)} files -> {out}", flush=True)
        return subprocess.run([sys.executable, str(tool), str(out), "--local"], check=False).returncode
    finally:
        for target in placed:
            target.unlink(missing_ok=True)


def main():
    argv = sys.argv[1:]
    every = "--all" in argv
    if every:
        argv.remove("--all")
    explicit = None
    if "--tool" in argv:
        at = argv.index("--tool")
        if at + 1 >= len(argv):
            sys.exit(__doc__)
        explicit = argv[at + 1]
        del argv[at:at + 2]
    if not 1 <= len(argv) <= 2 or argv[0].startswith("--"):
        sys.exit(__doc__)
    folder = Path(argv[0])
    if not folder.is_dir():
        sys.exit(f"no such folder: {folder}")
    found = pictures(folder)
    if not found:
        sys.exit(f"no log-*-eye-dump* files in {folder}")
    if len(argv) == 2:
        if argv[1] not in found:
            sys.exit(f"no picture {argv[1]} in {folder}; there are: {', '.join(sorted(found))}")
        chosen = [argv[1]]
    else:
        chosen = sorted(found) if every else [max(found)]
    tool = find_tool(explicit)
    failed = 0
    for stamp in chosen:
        if not any(name == "eye-dump.json" for _, name in found[stamp]):
            print(f"{stamp}: no eye-dump.json (the picture is incomplete), converting what is there")
        failed += convert(tool, folder, stamp, found[stamp]) != 0
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
