# Building

## Windows

Run `BUILD_AND_INSTALL.bat`, or prepare a build without a Quest:

```powershell
./tools/build-and-install.ps1 `
  -GamePackage "D:\Backups\RDR-export-20261101-120000" `
  -BuildOnly
```

The builder downloads pinned tools (Python, Temurin JDK 21, Android SDK
command-line tools, NDK r27c, CMake, build-tools 35, platform-tools, the
Khronos OpenXR loader and Vulkan headers), checks their SHA-256, validates the
game package, builds the native libraries and the Java loader, and signs the
assembled APK set. Work files are stored under `C:\RDRVRBuild`, or the
directory passed with `-WorkDir`. `-Trade` selects the Quest CPU/GPU level
trade written to the manifest (`-1`, `0` or `1`; default `0`).

On Windows, the builder remembers the last verified build, original game path
and signing key in `WorkDir/last-build.json`, before starting installation.
Rerunning after a USB or storage failure reuses matching verified APKs. A source,
input, key or trade change triggers a new build. Unchanged data APKs are reused
only after their payload, signature and alignment have been checked. The base
APK and any APK containing changed mod libraries are rebuilt.

For an installed copy with the same signing key, the Windows installer compares
all installed APK hashes and sends only changed APKs, retaining matching splits.
It verifies the full set afterwards. An unavailable or different APK set uses a
full installation. Game data and settings follow the existing update rules.

The game input is requested again only when no usable saved input/build exists.
Older complete archive builds can supply their original extracted `input` folder;
the patched installed APK is never treated as the original game source.
Pass `-GamePackage` to select another source, or `-Rebuild` to disable build reuse.
Keep the work folder and signing key to retain these conveniences.

## Linux and macOS

Install Bash, Python 3.10 or newer, curl, tar and unzip. Run:

```bash
bash BUILD_AND_INSTALL.sh \
  --game-package "$HOME/Backups/RDR-export-20261101-120000" \
  --build-only
```

Omit `--build-only` for installation. Work files are stored under
`~/RDRVRBuild`, or the directory passed with `--work-dir`.

## Options

| Windows | Linux/macOS | Meaning |
| --- | --- | --- |
| `-GamePackage` | `--game-package` | export folder, `base.apk`, single APK, `.apks`, `.zip` or `.xapk` |
| `-WorkDir` | `--work-dir` | tools, downloads, builds, logs and backups |
| `-AndroidSdk`, `-JavaHome` | `--android-sdk`, `--java-home` | use an existing SDK or JDK 21 |
| `-Keystore` | `--keystore` | use another personal signing key |
| `-Trade` | `--trade` | CPU/GPU level trade in the manifest: `-1`, `0` (default) or `1` |
| `-Serial` | `--serial` | ADB serial of the headset when several devices are connected |
| `-LogPath` | `--log-path` | log file location |
| `-BuildOnly` | `--build-only` | build and sign without touching a headset |
| `-Rebuild` | — | Windows: rebuild instead of reusing cached APKs |
| `-NonInteractive` | `--non-interactive` | fail instead of prompting |

The reset script accepts `-Yes` (`--yes`) to skip its confirmation and
`-Serial` (`--serial`); the report script accepts `-Serial`. The GPU boost
script accepts `-Off` (`--off`), `-Status` (`--status`) and `-Serial`
(`--serial`).

## Inputs and signing

The game input is the folder written by `EXPORT_RDR_APKS.bat` (`base.apk` and
its `split_*.apk` files), a `base.apk` next to its splits, a single APK, or an
`.apks`, `.zip` or `.xapk` archive. The package can be `com.rockstargames.rdr`
or `com.netflix.NGP.Kamo`, version `1.58.63226194` (`63226194`). The SHA-256
of `lib/arm64-v8a/librdr.so` selects engine variant 1 or 2 independently of
the package name. Unknown engines are refused with their hash and a request
to contact the author. The original engine is retained. Packages modified by
third parties and incomplete game data are refused.

Variant 2 uses the checked source profile in `tools/engine-profiles/variant-2.json`.
It is applied to a private copy under the build directory; source and result
hashes prevent silently applying stale edits after a shared source change.
Maintainers must revalidate and regenerate the profile when those sources change.

Keep the signing key at `%LOCALAPPDATA%\RDRVRBuilder\signing` on Windows, or
`$XDG_DATA_HOME/rdrvr-builder/signing` (`~/.local/share/rdrvr-builder/signing`
by default) on Linux/macOS. Updates require the same key as the installed
build.

## What the build does

1. `tools/gamecode.py` reads the engine library from the game package and
   writes the engine-code include files listed in `native/gamecode/ranges.json`.
   Hooks compare these bytes with the running engine before they are installed.
   The kit itself contains only their addresses, lengths and SHA-256 values.
2. `tools/hands_assets.py` embeds the UltimateXR hand assets.
3. CMake builds `libVkLayer_RDR2_raw_worker28.so` from `layer/` and
   `librdrvr.so` from `native/` with NDK r27c for arm64-v8a, Android 29.
4. The Java loader in `loader/` is compiled and converted to DEX.
5. `tools/build.py` edits the game manifest for VR, adds the libraries and the
   loader, applies the GameManager null check when the supported Java method
   is present, aligns and signs every APK, and writes `build-manifest.json`.
   Packages declaring `activateGameMode` as a native method have no DEX body
   to patch; their game DEX is preserved. Before launching the game, the loader
   disables that native query if Android provides no GameManager service.
   The compatibility action is recorded
   in the log and manifest. Engine and APK signature checks still apply.

`tools/build.py` can also be run directly; see its `--help`.

## Source layout

| Path | Contents |
| --- | --- |
| `native/src/engine` | engine interop: targets, passes, culling, occlusion |
| `native/src/stereo` | stereo replay of the engine command stream, engine levers |
| `native/src/camera`, `native/src/visibility` | camera split and visibility |
| `native/src/runtime` | OpenXR session, presentation, SpaceWarp, menus, settings |
| `native/src/vr` | body, hands, weapons, holsters, locomotion, HUD, cutscenes, TAA |
| `native/src/*/shaders` | GLSL sources of the embedded SPIR-V |
| `layer/src` | Vulkan layer loaded by the engine |
| `loader/src` | Java application and preflight activity |
| `tools` | build, install and maintenance scripts |

`tools/compile_shaders.py` rebuilds the SPIR-V headers from the GLSL sources
with `glslc` from the Vulkan SDK 1.4.357.0.

## Reset settings

`RESET_VR_SETTINGS.bat` and `RESET_VR_SETTINGS.sh` remove only these files in
`/sdcard/Android/data/com.rockstargames.rdr/files/rdrvr` on the headset:

```text
controls.cfg
controls.cfg.tmp
graphics.cfg
graphics.cfg.tmp
vr.cfg
vr.cfg.tmp
weapons.cfg
weapons.cfg.tmp
```

Saved teleport spots are kept unless `-IncludeTeleportSpots`
(`--include-teleport-spots`) is passed. Both scripts require confirmation and
never start the game.
