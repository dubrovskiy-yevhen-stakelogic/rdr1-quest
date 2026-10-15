# Red Dead Redemption VR for Quest

Version `0.1.0 alpha`. Source code and build tools for the Quest VR mod.

An unofficial VR mod for the Android version of Red Dead Redemption. It runs
standalone on Meta Quest 3 and Quest 3S, without a PC or Quest Link.

## Requirements

- Red Dead Redemption for Android from Google Play, version `1.58.63226194`
  (arm64), exported from an Android phone or tablet with `EXPORT_RDR_APKS.bat`.
- Meta Quest 3 or Quest 3S with Developer Mode enabled and a USB-C data cable.
- Windows 10/11, Linux x86_64 or macOS, an internet connection for the build
  tools, and about 20 GB of free disk space.
- About 8 GB of free storage on the headset.

## Build and install

The easiest way: download this repository once (Code > Download ZIP, or
`git clone`), put it in a folder of its own and run `UPDATE.bat` on Windows or
`bash UPDATE.sh` on Linux/macOS. It gets Git if the PC has none (a private
portable copy on Windows, no admin rights), brings the folder to the latest
version from GitHub and starts the build and install wizard below. Run the
same file again whenever a new version is out: your exported game, the
personal signing key and build caches are kept; only the mod's own files are
replaced.

1. Export the game from the phone with `EXPORT_RDR_APKS.bat`. Keep the whole
   export folder: `base.apk`, every `split_*.apk` and the `external` folder.
2. Run `BUILD_AND_INSTALL.bat` on Windows or `bash BUILD_AND_INSTALL.sh` on
   Linux/macOS.
3. Select the export folder.
4. Connect the Quest, allow USB debugging in the headset, and follow the
   prompts.

The installer downloads and verifies every build tool, builds the mod from
these sources, signs the APK set with a personal key and installs it. The game
is left stopped. Start it from **Library > Unknown sources > RDR** while wearing
the headset in a well-lit room.

Keep the personal signing key (see [BUILDING.md](BUILDING.md)). Updates must be
signed with the same key; otherwise Android requires uninstalling the game,
which deletes its saved games. Copy them to the PC first with
`TRANSFER_SAVES.bat -FromHeadset` (see [Saves](#saves)).

## Features

- Full 6DoF stereo rendering of the game world with head-tracked camera.
- 36 fps rendering shown at 72 Hz, with optional Application SpaceWarp that
  uses per-character motion vectors.
- TAA anti-aliasing with adjustable sharpness, foveated rendering, and
  resolution, shadow, grass and sharpening options.
- The player's own body with arm IK; weapons sit in the hands.
- Body holsters for every weapon slot, physical aiming and firing, two-hand
  aim, dual wield and hand-to-hand transfer.
- Manual revolver reload, hammer cocking and fanning, gun spin, throwing and
  catching the revolver.
- Physical knife and fist melee, fist block, two-handed lasso.
- Smooth, native or teleport locomotion, snap or smooth turn, sprint and
  crouch toggles.
- Horseback riding with a stable seat camera (adjustable height and
  distance), spurring by tapping A, jumping with B.
- Weapon presets: REALISTIC (gun spin, manual reload one round at a time,
  hammer cocking) or CASUAL (the trigger just fires, B reloads, no spin).
- HOLSTERS page: choose which of your revolvers, rifles, shotguns, thrown
  and melee weapons sits in each holster.
- Whistle for the horse by holding an empty hand to the mouth and squeezing
  the grip.
- HUD on a head-locked panel or with the minimap on the left wrist.
- Cutscenes on a flat screen, in stereo from the cutscene camera, or in first
  person.
- In-headset VR menu with graphics, movement, weapons, holsters, HUD, cheat,
  button remapping and controls help pages. Cheats include giving any weapon,
  every weapon of a type or all weapons.

## Controls

| Action | Control |
| --- | --- |
| Move | Left stick |
| Sprint toggle | Left stick click (L3) |
| Turn (snap or smooth) | Right stick left / right |
| Crouch toggle | Right stick click (R3) |
| Jump (on a horse: spur, tap repeatedly) | A |
| Reload, interact; open the revolver cylinder (on a horse: jump) | B |
| Whistle for the horse | X |
| Use, mount, dismount | Y |
| Dead Eye | Left trigger + R3 |
| Recenter | Hold L3 + R3 |
| Draw from a holster | Grip at the holster |
| Fire | Trigger of the hand holding the weapon |
| Pause menu | Left menu button |
| VR menu | Hold the left menu button |

Holsters: revolver on the right hip, lasso on the left hip, knife on the left
chest, throwables at the chest centre, rifles behind the right shoulder,
shotguns behind the left shoulder and sniper rifles behind the head. During a
cutscene R3 switches between the cutscene modes.

In the VR menu the stick moves up and down, the right trigger steps a value
forward and the left trigger back, A opens or applies, B goes back and X / Y
switch pages. Settings are saved immediately. Every button above can be moved
to another button on the BUTTONS page, separately on foot and on a horse; the
CONTROLS page in the headset describes all of them.

## GPU boost

Android apps cannot raise the headset GPU clock themselves, so the kit includes
a small helper that runs on the headset over ADB. `BUILD_AND_INSTALL` starts it
after the install. A headset restart stops it: run `GPU_BOOST.bat` or
`bash GPU_BOOST.sh` once after every headset restart, with the headset
connected and USB debugging allowed.

- **GRAPHICS > GPU BOOST** in the VR menu chooses the level: `OFF`, `640` or
  `690` MHz (default `690`). `690` also raises the CPU to its top clock
  (2.36 GHz), which keeps busy places such as towns smooth. The row shows
  `NO HELPER` when the helper is not running.
- The boost applies only while the game runs. Home and other apps keep their
  normal clocks.
- Higher clocks drain the battery faster and warm the headset. When the
  headset gets hot, the game shows a red overheat warning and lowers `690` to
  `640`.
- `-Off` (`--off`) stops the helper and restores the normal clocks; `-Status`
  (`--status`) shows its state.

## Saves

The game keeps its saves in private storage that ADB cannot read. Every time
the game starts, the mod copies them to its `rdrvr` folder on the headset and
imports the saves staged there. `TRANSFER_SAVES.bat` or
`bash TRANSFER_SAVES.sh` moves saves between the PC and the headset; connect
the headset and allow USB debugging first.

- **PC to headset:** `TRANSFER_SAVES.bat` takes the saves of the newest profile
  of the PC game (`Documents\Rockstar Games\Red Dead Redemption\Profiles`).
  `-From <folder>` (`--from <folder>`) or dropping a folder on the `.bat` file
  uses another folder; Linux and macOS ask for it. `-SkipExtras` leaves
  `RDR2EXTRAS.SAV` out. The tool lists which headset saves will be replaced,
  closes the game and stages the files. Start the game once: the saves are
  imported while it starts.
- **Headset to PC:** `-FromHeadset` (`--from-headset`) copies the headset saves
  as `RDR2SAVE0.SAV` and so on to `saves-from-headset` in the work folder
  (`-To <folder>` chooses another empty folder). Close the PC game and copy
  them into its profile folder. They are the saves as of the last game start:
  start and close the game once to include newer ones.
- **Backups:** the headset backs up every save that an import replaces in
  `rdrvr/save-backup/<time>`. Before each import the tool also copies the
  current headset saves to `save-backups` in the work folder.
  `-FromHeadset -IncludeBackups` copies the headset backups to the PC.
- **Continue or Load Game:** after an import, **Continue** loads the save with
  the newest in-game save date, which may be another slot. Use **Load Game** to
  pick the imported slot, or replace every slot that is in use.
- `-Status` (`--status`) shows the headset saves, saves waiting for import and
  the end of the transfer log.

Saves of the PC version load unchanged. Saves from consoles are untested.

## Settings and reports

- In the headset: VR menu > **RESET ALL**, press A twice. Graphics, VR and
  button settings go back to the defaults. Saved games are not touched.
- `RESET_VR_SETTINGS.bat` or `bash RESET_VR_SETTINGS.sh` restores the default
  VR settings after confirmation. Saved games are not touched.
- `COLLECT_REPORT.bat` collects a diagnostic report from the headset into the
  work folder. Nothing is uploaded.

## Known limitations

- The game needs 6DoF tracking. In a dark room the headset cannot track and the
  game does not start.
- On battery the headset lowers its clocks; busy towns at night can drop below
  36 fps.
- The pause menu, front end and loading screens are shown on a flat screen.
- HUD text and subtitles are hidden during stereo cutscenes.
- On horseback only the revolver and the lasso can be drawn from holsters.
  Duels, mounted guns and scripted scenes use the game's own weapon handling.
- Room-scale movement is experimental and off by default.
- Installing the 3 GB APK set over Wi-Fi ADB can fail; use a USB cable.

## License

See [LICENSE](LICENSE), [NOTICE.md](NOTICE.md), and the licenses alongside
third-party files. Game APKs, game code and assets, and signing keys are
provided locally and are not included in this source kit.
