# Changelog

## 0.2.0

### Weapons and reload

- REALISTIC reload now works on every revolver: Cattleman, Schofield,
  Double-Action, LeMat and any other revolver. The Schofield and LeMat break
  open at the top, the Double-Action cylinder swings out on its crane, the
  LeMat has 9 chambers.
- REALISTIC reload for the double-barrel and sawed-off shotguns: B breaks the
  gun open, you load the shells by hand and close it with B, a push or a flip
  up.
- New row SHELL EJECT: AUTO throws the spent shells out on opening, SHAKE OUT
  keeps them until you tip the muzzle up or shake the gun.
- WEAPON CALIBRATION > SAVE FOR has REVOLVER FIT and SHELLS FIT: line up the
  hammer, rounds and shells for each gun model in 1 mm steps.
- Right stick down cocks the revolver hammer with either hand in REALISTIC.
  Cocking with the free hand from the side is more reliable.
- New row TWO HAND FIRE: long guns fire only while both hands hold them. On in
  the REALISTIC preset, off in CASUAL.
- Two-hand aim: the support hand stays on the barrel and a short grip or
  tracking dip no longer drops the hold.
- The left hand has mirrored fingers and holds the knife correctly.
- A gun held in VR no longer refuses to fire close to a wall or in cover.
- Dead Eye works in VR: R3 with a gun in hand. New rows on the HUD page: DEAD
  EYE MODE (SLOW-MO by default, or the game's own marks), DEAD EYE TIME (HALF
  by default, or FULL) and DEAD EYE FILTER (a warm tint).
- Reload rounds can be taken from the chest and belt while crouched.

### Throwing

- The tomahawk, throwing knives, dynamite and fire bottles are thrown by hand:
  swing and open the grip. The game's own throw does the rest, so damage,
  fuses and item counts stay as in the game.
- Hold the trigger to see the arc, release it to throw along the arc.
- A short trigger tap lights dynamite, then swing to throw it.
- Pass a throwable from hand to hand, or toss it gently and catch it. Tossing
  is not available on a horse.
- New THROWING page: THROWABLES, THROW POWER, AIMED SPEED, THROW GRAVITY, AIM
  ASSIST, MIN SWING, SHOW ARC and THROW SPIN.
- The white aim dot is hidden while you hold a throwable.

### Duels

- Duels are physical: your hands start empty, you draw from the holster and
  shoot. A body hit wins, an arm hit disarms, running out of time loses.
- New cheat DUEL ON LOOK: look at a person, hold the left trigger and press Y
  to start the game's own duel with them.

### Scopes

- Sniper rifles held in VR fire on every trigger pull.
- The full-screen stock scope is off. The rifle's eyepiece shows a zoomed
  picture on a lens, and the rifle's own parts fade out of that picture.
- New SCOPE LENS page: position, size and zoom of the lens, saved per rifle.
- New rows FULL ZOOM (on by default) and FULL ZOOM DIST: bring the scope close
  to your eye and the zoomed picture fills that eye.

### Satchel and items

- VR menu > SATCHEL, or hold X for half a second, opens the game's own satchel
  on the flat screen. Stick to move, A to use, Y for details, B to go back,
  grips to switch tabs.
- The SATCHEL row shows the game's answer when it refuses an item.
- New row GAME MESSAGES (off by default): the game's help text as a note in
  view.

### Body and horse

- Ride with the reins in your hands: grip them, snap them to go and go faster,
  pull one hand toward you to turn, raise both hands and hold to stop. The
  stick and A still work. BUTTONS > REINS GESTURES turns the gestures off.
- The rein rope follows the hands that hold it.
- New HORSE page: HORSE VIEW (first or third person), TURN PULL, HORSE SHAKE,
  and HORSE CAM UP / FWD moved here. Both now default to 0 CM, and settings
  from 0.1.0 are moved to 0 CM, so the reins can be reached.
- A steadier ride: John's body and the horse's head no longer bounce with the
  gallop unless HORSE SHAKE is on. John's upper body stays upright in the saddle
  and follows your head.
- While a hand holds the reins, its grip and trigger do not draw a gun. The
  other hand can still fire.
- The view follows John's head while crouching.

### Cutscenes

- FIRST PERSON cutscenes follow John's head instead of jumping with the shot
  camera.
- Scripted gameplay shots are shown in stereo.
- New row SUN SHADOW CAM: the sun shadows of stereo cutscenes follow your eye.
- New rows CUTSCENE PROPS, LIGHTS and SUN for FIRST PERSON cutscenes. They do
  not solve the shot changes yet, see Known issues.

### HUD and menus

- New row AMMO ON GUN (on by default): the ammo count sits next to the gun.
- Subtitles are shown during stereo cutscenes.
- Menu pages have a colour per category.
- New cheats CLEAR WANTED and NO WANTED.
- The welcome screen shows the version and checks whether GPU BOOST is on.
- The red overheat warning is replaced by a calm note when the system lowers
  the clocks. GRAPHICS > CLOCK NOTICE hides it.
- A tap of the left menu button opens the game's pause menu on the flat screen,
  also with a gun in hand.

### Performance and graphics

- Fixed a lock at 24 fps, most often near water.
- SpaceWarp: the ridden horse, your hands and the held weapon get their own
  motion.

### Stability

- Standing still for a while no longer switches the game to the flat view.
- Less logging during normal play.

### Tools and install

- VR menu > SAVE LOG writes a report to the headset that survives a restart.
  `COLLECT_REPORT.bat` picks up the newest ones.
- New row PERF RECORD with MARK NOW records frame times.
  `COLLECT_REPORT.bat` picks up the recordings and `tools/perf_profile.py`
  summarizes them.

## Known issues

- First-person cutscenes: the game stages each cutscene for its own 2D shots,
  moving lights and hiding objects per shot. Seen from John's eyes, lighting
  can change suddenly and objects can vanish at a shot change. The CUTSCENE
  PROPS, LIGHTS and SUN rows do not solve this yet. A fix is planned.
- The realistic weapon mode (REALISTIC preset, manual reload) works only for
  revolvers and for the double-barrel and sawed-off shotguns. Other weapons use
  the game's reload. We plan to add it for all weapons.
- Engine variant 2: the build stops with a request to report it, because this
  variant has not been updated for 0.2.0 yet. Engine variant 1 is not affected.
- Sun shadows can disappear outdoors outside cutscenes. SUN SHADOW CAM > EYE
  ALWAYS is an untested attempt at this.
- The ridden horse's tail can look doubled.
- In HORSE VIEW THIRD PERSON the holsters and a held gun follow your controllers
  near the camera, not John.

If you find other problems, please report them to the developer on the Discord
server: https://discord.gg/5pUVuZVQj
