# Red Dead Redemption VR for Quest

Version `0.2.0 alpha`. Source code and build tools for the Quest VR mod. See
[CHANGELOG.md](CHANGELOG.md) for what changed and for the known issues.

An unofficial VR mod for the Android version of Red Dead Redemption. It runs
standalone on Meta Quest 3 and Quest 3S, without a PC or Quest Link.

## Requirements

- Red Dead Redemption for Android, version `1.58.63226194`
  (arm64), exported from an Android phone or tablet with `EXPORT_RDR_APKS.bat`.
- Meta Quest 3 or Quest 3S with Developer Mode enabled and a USB-C data cable.
- Windows 10/11, Linux x86_64 or macOS, an internet connection for the build
  tools, and about 20 GB of free disk space.
- About 18 GB of free storage on the headset during installation; the installer
  calculates the requirement with a reserve from the APK set size.

## Build and install

The easiest way: download this repository once (Code > Download ZIP, or
`git clone`), put it in a folder of its own and run `UPDATE.bat` on Windows or
`bash UPDATE.sh` on Linux/macOS. It gets Git if the PC has none (a private
portable copy on Windows, no admin rights), brings the folder to the latest
version from GitHub and starts the build and install wizard below. Run the
same file again whenever a new version is out: your exported game, the
personal signing key and build caches are kept; only the mod's own files are
replaced.

1. **Download the full game on your phone first.** Open it, wait for all
   additional data to download, and check that you can enter the game world.
   Then export it with `EXPORT_RDR_APKS.bat`. Keep the whole export folder:
   `base.apk`, every `split_*.apk` and the `asset-packs` and `external`
   folders. A small APK or incomplete XAPK is not enough; do not rely on the
   Quest to download missing data.
2. Run `BUILD_AND_INSTALL.bat` on Windows or `bash BUILD_AND_INSTALL.sh` on
   Linux/macOS.
3. Select the export folder.
4. Connect the Quest, allow USB debugging in the headset, and follow the
   prompts.

The installer downloads and verifies every build tool, builds the mod from
these sources, signs the APK set with a personal key and installs it. The game
is left stopped. Start it from **Library > Unknown sources > RDR** while wearing
the headset in a well-lit room.

The installer identifies the engine variant by the SHA-256 of `librdr.so`,
independently of the Rockstar/Netflix package name, and shows the variant and
engine ID. Version 0.2.0 builds engine variant 1. Engine variant 2 has not been
updated for 0.2.0 yet, so its build stops with a request to report it. An
unknown variant is stopped with a request to report the ID to the author; the
installer never applies another variant's hooks or replaces the game engine.

Use one complete export of your game. APK, APKS, ZIP and XAPK inputs are
accepted. Some XAPKs contain the engine and initial data but omit the world and
audio packs. If the installer reports missing resources, let that same game
finish downloading on the phone and export it again. A second edition is not
required, and the installer does not supply game files.

The Google Play edition installs all of its data as APK files, so it exports
completely from any phone. The Netflix edition installs only part of the data
as APK files and downloads the world, cutscene and audio packs (about 3.7 GB)
into the app's private storage, which Android lets only a rooted phone copy
over USB. On a rooted phone `EXPORT_RDR_APKS.bat` asks for superuser access for
Shell, copies these packs into the `asset-packs` folder of the export and
checks every file; the builder turns them into APK files for the Quest. Without
root the Netflix edition cannot be exported completely.

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
- Manual reload for every revolver and for the double-barrel and sawed-off
  shotguns, hammer cocking and fanning, gun spin, throwing and catching the
  revolver.
- Dead Eye with a gun in hand: slow motion with free shooting by default, or the
  game's own marks.
- The tomahawk, throwing knives, dynamite and fire bottles thrown by hand, with
  an aim arc on the trigger, hand-to-hand passing and tossing.
- Duels with a real draw from the holster.
- Sniper rifles with a zoomed picture on the scope's lens, adjustable per rifle,
  and a full-zoom view when the scope is close to the eye.
- Physical knife and fist melee, fist block, two-handed lasso.
- Smooth, native or teleport locomotion, snap or smooth turn, sprint and
  crouch toggles.
- Horseback riding with reins in your hands or the stick, a steady seat camera
  (adjustable height and distance), first or third person, spurring by tapping
  A, jumping with B.
- Weapon presets: REALISTIC (gun spin, manual reload one round at a time,
  hammer cocking, two hands on long guns) or CASUAL (default: the trigger just
  fires, B reloads, no spin). REALISTIC reload covers revolvers and the
  double-barrel and sawed-off shotguns; other weapons use the game's reload.
  CASUAL forwards the held firearm trigger without VR hammer or cylinder gates;
  hand gestures do not cock or fan the hammer. The game controls ammo and firing cadence.
  Existing saved settings are preserved; resetting VR settings restores CASUAL.
- The game's own satchel on the flat screen, from the VR menu or by holding X.
- HOLSTERS page: choose which of your revolvers, rifles, shotguns, thrown
  and melee weapons sits in each holster.
- Whistle for the horse by holding an empty hand to the mouth and squeezing
  the grip.
- HUD on a head-locked panel or with the minimap on the left wrist, and the
  ammo count next to the gun.
- Cutscenes on a flat screen, in stereo from the cutscene camera, or in first
  person.
- In-headset VR menu with graphics, movement, weapons, cheats, weapon
  calibration, scope lens, HUD, horse, throwing, controls help, button remapping
  and holsters pages. Cheats include giving any weapon, every weapon of a type
  or all weapons, starting a duel and clearing the wanted level.

## Controls

| Action | Control |
| --- | --- |
| Move | Left stick |
| Sprint toggle | Left stick click (L3) |
| Turn (snap or smooth) | Right stick left / right |
| Crouch toggle | Right stick click (R3) |
| Jump (on a horse: spur, tap repeatedly) | A |
| Reload, interact; open the revolver cylinder or double-barrel in REALISTIC (on a horse: jump) | B |
| Whistle for the horse | X |
| Satchel (items, skins, outfits) | VR menu > SATCHEL, or hold X for half a second; stick and A inside, B goes back, grips switch tabs |
| Use, mount, dismount | Y |
| Dead Eye | R3 with a gun in hand, or left trigger + R3 |
| Cock the hammer (REALISTIC) | Right stick down, or drag the hammer back with the free hand |
| Recenter | Hold L3 + R3 |
| Draw from a holster | Grip at the holster |
| Fire | Trigger of the hand holding the weapon |
| Throw | Swing and open the grip, or hold the trigger for the arc and release |
| Game menu (pause, map, journal, options, save) | Tap the left menu button; B or the left menu button goes back |
| VR menu | Hold the left menu button |

Holsters: revolver on the right hip, lasso on the left hip, knife on the left
chest, throwables at the chest centre, rifles behind the right shoulder,
shotguns behind the left shoulder and sniper rifles behind the head. During a
cutscene R3 switches between the cutscene modes.

Reins: grip the reins in front of you, snap them to go and go faster, pull one
hand toward you to turn and raise both hands and hold to stop. The stick and A
keep working; **BUTTONS > REINS GESTURES** turns the gestures off.

REALISTIC reload: B opens the gun, tip the muzzle up and shake to eject (**SHELL
EJECT** decides for the shotguns), take a round with the free hand at the chest
or belt, put it in an empty chamber and close with a short B, a push or a flick.
A long B runs the game's reload.

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
- Higher clocks drain the battery faster and warm the headset, as with any
  boost tool. When the system lowers the clocks, which is normal, the game shows
  a short note and lowers `690` to `640`. **GRAPHICS > CLOCK NOTICE** hides the
  note.
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

On Windows, running `UPDATE.bat` again remembers your game source and reuses a
verified build when its inputs have not changed. After a failed USB installation
or insufficient headset storage, free space/reconnect and rerun it; keep
`C:\RDRVRBuild` and your signing key. When the mod changes, unchanged data APKs can
be reused, and only APKs differing from the installed set are sent to the Quest.
File checks still take time. Use `BUILD_AND_INSTALL.bat -GamePackage "path"` to
choose a different input, or add `-Rebuild` to force a fresh build.

To report a bug, open the VR menu and press A on **SAVE LOG** right after the
problem. It writes a report to `rdrvr/saved-logs` on the headset that survives a
game restart; the 40 newest stay. Then run `COLLECT_REPORT.bat` and send the
result to the developer on the Discord server: https://discord.gg/5pUVuZVQj

New rows in the VR menu and their defaults:

| Page | Row | Default |
| --- | --- | --- |
| ROOT | SATCHEL, SAVE LOG | |
| MOVEMENT | SUN SHADOW CAM | EYE IN CUTSCENES |
| MOVEMENT | CUTSCENE PROPS, LIGHTS, SUN (FIRST PERSON cutscenes) | STEADY |
| HANDS / WEAPONS | TWO HAND FIRE | OFF |
| HANDS / WEAPONS | SHELL EJECT | AUTO |
| HUD | AMMO ON GUN | ON |
| HUD | GAME MESSAGES | OFF |
| HUD | DEAD EYE MODE | SLOW-MO |
| HUD | DEAD EYE TIME | HALF |
| HUD | DEAD EYE FILTER | ON |
| HUD | PERF RECORD, MARK NOW | OFF |
| HORSE | HORSE VIEW | FIRST PERSON |
| HORSE | TURN PULL | 22 CM |
| HORSE | HORSE SHAKE | OFF |
| THROWING | THROWABLES | VR |
| THROWING | THROW POWER, THROW GRAVITY | 100% |
| THROWING | AIMED SPEED | 14 M/S |
| THROWING | AIM ASSIST | OFF |
| THROWING | MIN SWING | 2.0 M/S |
| THROWING | SHOW ARC | ON |
| THROWING | THROW SPIN | NORMAL |
| SCOPE LENS | FULL ZOOM | ON |
| SCOPE LENS | FULL ZOOM DIST | 12 CM |
| BUTTONS | REINS GESTURES | ON |
| CHEATS | DUEL ON LOOK, NO WANTED | OFF |
| CHEATS | CLEAR WANTED | |

**WEAPON CALIBRATION > SAVE FOR** also has REVOLVER FIT, SHELLS FIT and SCOPE
LENS for the held gun.

- In the headset: VR menu > **RESET ALL**, press A twice. Graphics, VR and
  button settings go back to the defaults. Saved games are not touched.
- `RESET_VR_SETTINGS.bat` or `bash RESET_VR_SETTINGS.sh` restores the default
  VR settings after confirmation. Saved games are not touched.
- `COLLECT_REPORT.bat` collects a diagnostic report from the headset into the
  work folder. Nothing is uploaded.
  While the game is running it requests a fresh diagnostic snapshot, including
  with the FPS counter off. It does not change settings or start/stop the game.
  The report also includes the newest **SAVE LOG** files and **PERF RECORD**
  recordings.
  `summary.txt` states whether that snapshot was confirmed; if not, the saved
  JSON can contain only startup state and is not evidence of gameplay counters.
  If a weapon stops firing, try the trigger several times and collect the report
  while the game is still running, before restarting it. Include the weapon name
  and whether the problem began after holstering, dropping or changing weapons.
  The game records shot attempts and holster events as `RdrVrFire` in the log,
  with the last 64 events also saved in `vr.weapon.diagnostics` in the JSON report.
  The report ZIP includes a separate `fire-diagnostics.txt` with these log lines.
  `NATIVE_PAD` events and `gameBootstrap.nativeInput.delivery` record trigger delivery
  after the native input update: requested/submitted RT, selected controller,
  native held/pressed buttons, and rejected filters. `ROUTED` alone only confirms
  that the VR weapon filter forwarded the press, not that the game consumed it.
  Input blocks have an explicit reason; `NO_ENGINE_FIRE` only means no projectile
  was observed within 700 ms. Its context hint is not a confirmed engine cause.

## Known limitations

See also the known issues in [CHANGELOG.md](CHANGELOG.md).

- The game needs 6DoF tracking. In a dark room the headset cannot track and the
  game does not start.
- On battery the headset lowers its clocks; busy towns at night can drop below
  36 fps.
- The pause menu, satchel, front end and loading screens are shown on a flat
  screen.
- During stereo cutscenes only the subtitles of the HUD are shown.
- On horseback the knife cannot be drawn from its holster, and throwables cannot
  be tossed. Mounted guns and scripted scenes use the game's own weapon handling.
- Room-scale movement is experimental and off by default.
- Installing the 3 GB APK set over Wi-Fi ADB can fail; use a USB cable.

## License

See [LICENSE](LICENSE), [NOTICE.md](NOTICE.md), and the licenses alongside
third-party files. Game APKs, game code and assets, and signing keys are
provided locally and are not included in this source kit.
