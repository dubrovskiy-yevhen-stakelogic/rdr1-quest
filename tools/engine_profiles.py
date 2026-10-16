"""Engine identity is independent of the Android package/store name.

Profiles contain source edits and verification pins, never a replacement engine.
Edits fail closed if the shared source has changed since a profile was validated.
"""
import hashlib
import json
from pathlib import Path
import shutil

PACKAGES = ("com.rockstargames.rdr", "com.netflix.NGP.Kamo")
PROFILES = {
    "3f80845edec32a663c42a8570e3131446813e695076c4bc96e2142c190bc22f3": {
        "id": "variant-1", "size": 1323108640,
        "notice": "Engine variant 1 detected: supported.",
    },
    "0f288e2f9255d04277190bb5ffca742ec395d33a47fb305ddb22b9f0e904ef72": {
        "id": "variant-2", "size": 49868760,
        "notice": "Engine variant 2 detected: supported, with less testing. VR gameplay has been tested, but some features may behave differently. Please report issues to the author with the engine ID shown below.",
    },
}


def identify(digest, size):
    profile = PROFILES.get(digest.lower())
    if profile is None or profile["size"] != size:
        raise ValueError("unsupported game engine: this engine variant is not supported yet. "
                         "Please report it to the author. "
                         f"Engine SHA-256: {digest}; size: {size} bytes. "
                         "The store name and game version do not identify the engine variant.")
    return dict(profile)


def stage_sources(kit, destination, profile, package):
    if package not in PACKAGES:
        raise ValueError("unsupported Android package")
    destination = Path(destination)
    if destination.exists():
        raise ValueError("engine source staging directory must be empty")
    for name in ("native", "layer", "loader", "assets", "tools"):
        shutil.copytree(kit / name, destination / name,
                        ignore=shutil.ignore_patterns("__pycache__"))
    if profile["id"] == "variant-2":
        spec = json.loads((kit / "tools/engine-profiles/variant-2.json").read_text())
        for row in spec["files"]:
            path = (destination / row["path"]).resolve()
            if destination.resolve() not in path.parents or path.suffix not in (".h", ".cpp", ".inc", ".inl", ".json"):
                raise ValueError("invalid engine profile source path")
            source = path.read_text(encoding="utf-8")
            if hashlib.sha256(source.encode()).hexdigest() != row["sourceSha256"]:
                raise ValueError(f"Engine variant 2 source profile needs updating: {row['path']}. Please report this to the author.")
            lines = source.splitlines(keepends=True)
            for edit in reversed(row["edits"]):
                lines[edit["start"]:edit["end"]] = edit["lines"]
            result = "".join(lines)
            if hashlib.sha256(result.encode()).hexdigest() != row["resultSha256"]:
                raise ValueError("engine profile result checksum mismatch")
            path.write_text(result, encoding="utf-8", newline="\n")
    # Paths belong to the package being installed, not to an engine profile.
    if package != PACKAGES[0]:
        for folder in ("native", "layer"):
            for path in (destination / folder).rglob("*"):
                if path.suffix not in (".h", ".cpp", ".inc", ".inl"):
                    continue
                source = path.read_text(encoding="utf-8")
                result = source
                for prefix in ("/sdcard/Android/data/", "/data/user/0/", "/data/data/"):
                    result = result.replace(prefix + PACKAGES[0], prefix + package)
                if result != source:
                    path.write_text(result, encoding="utf-8", newline="\n")
    return destination
