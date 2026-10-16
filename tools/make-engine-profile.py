#!/usr/bin/env python3
"""Maintainer tool: capture verified native source changes, without engine binaries."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--base", type=Path, required=True)
p.add_argument("--adapted", type=Path, required=True)
p.add_argument("--out", type=Path, required=True)
a = p.parse_args()
rows = []
for folder in ("native", "layer"):
    for path in sorted((a.base / folder).rglob("*")):
        if path.suffix not in (".h", ".cpp", ".inc", ".inl", ".json"):
            continue
        relative = path.relative_to(a.base)
        adapted = a.adapted / relative
        old = path.read_text(encoding="utf-8")
        new = adapted.read_text(encoding="utf-8")
        # Package-specific paths are selected separately at build time.
        for prefix in ("/sdcard/Android/data/", "/data/user/0/", "/data/data/"):
            new = new.replace(prefix + "com.netflix.NGP.Kamo", prefix + "com.rockstargames.rdr")
        if old == new:
            continue
        left, right = old.splitlines(keepends=True), new.splitlines(keepends=True)
        edits = [{"start": i, "end": j, "lines": right[k:l]}
                 for tag, i, j, k, l in difflib.SequenceMatcher(None, left, right, autojunk=False).get_opcodes()
                 if tag != "equal"]
        rows.append({"path": relative.as_posix(), "sourceSha256": hashlib.sha256(old.encode()).hexdigest(),
                     "resultSha256": hashlib.sha256(new.encode()).hexdigest(), "edits": edits})
a.out.parent.mkdir(parents=True, exist_ok=True)
a.out.write_text(json.dumps({"formatVersion": 1, "files": rows}, indent=2) + "\n", encoding="utf-8", newline="\n")
print(f"Profile: {len(rows)} files, {a.out.stat().st_size} bytes")
