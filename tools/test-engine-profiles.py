import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import engine_profiles as profiles

ROOT = Path(__file__).resolve().parent.parent


class Profiles(unittest.TestCase):
    def test_identification_and_unknown(self):
        for digest, spec in profiles.PROFILES.items():
            self.assertEqual(profiles.identify(digest.upper(), spec["size"])["id"], spec["id"])
            with self.assertRaisesRegex(ValueError, "report.*author"):
                profiles.identify(digest, spec["size"] + 1)
        with self.assertRaisesRegex(ValueError, "SHA-256: " + "0" * 64):
            profiles.identify("0" * 64, 49868760)

    def test_profile_reconstructs_verified_sources_and_separates_package(self):
        profile = next(p for p in profiles.PROFILES.values() if p["id"] == "variant-2")
        spec = json.loads((ROOT / "tools/engine-profiles/variant-2.json").read_text())
        with tempfile.TemporaryDirectory() as temp:
            destination = profiles.stage_sources(ROOT, Path(temp) / "staged", profile, profiles.PACKAGES[0])
            for row in spec["files"]:
                self.assertEqual(hashlib.sha256((destination / row["path"]).read_text().encode()).hexdigest(), row["resultSha256"])
            # A profile identifies the engine, even when the APK package is Rockstar.
            ranges = json.loads((destination / "native/gamecode/ranges.json").read_text())
            self.assertEqual(profiles.identify(ranges["engine"]["sha256"], ranges["engine"]["size"])["id"], "variant-2")
        with tempfile.TemporaryDirectory() as temp:
            destination = profiles.stage_sources(ROOT, Path(temp) / "staged", profile, profiles.PACKAGES[1])
            files = [destination / r["path"] for r in spec["files"] if r["path"].endswith((".h", ".cpp", ".inc"))]
            self.assertFalse(any('/sdcard/Android/data/com.rockstargames.rdr' in p.read_text() for p in files))

    def test_stale_profile_refused(self):
        profile = next(p for p in profiles.PROFILES.values() if p["id"] == "variant-2")
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "source"
            import shutil
            for folder in ("native", "layer", "loader", "assets", "tools"):
                shutil.copytree(ROOT / folder, source / folder)
            row = json.loads((source / "tools/engine-profiles/variant-2.json").read_text())["files"][0]
            p = source / row["path"]
            p.write_text(p.read_text() + "\n// source changed\n")
            with self.assertRaisesRegex(ValueError, "needs updating"):
                profiles.stage_sources(source, Path(temp) / "staged", profile, profiles.PACKAGES[0])


if __name__ == "__main__":
    unittest.main()
