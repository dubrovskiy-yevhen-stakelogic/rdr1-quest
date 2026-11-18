# Changelog

## 0.3.0

### Physical interaction

- Hand physics, ON by default (row HANDS > HAND PHYSICS): your empty hands
  are physical objects in the world. The feature is experimental; if it
  causes problems, turn it off with HANDS > HAND PHYSICS > OFF (the welcome
  panel says the same). MEASURE only records the cost in the report.
- Walls and furniture: your hands stop at them instead of passing through.
- Doors: push a door with your palm from either side and it moves with your
  hand and stops when your hand stops. Walking through a door with your body
  still opens it.
- Glass: punch a window with a bare hand to break it. The hand bleeds (the
  game's own wound on John's hand).
- Small props: cups, bottles and similar items are knocked over by your
  hands. Pick a light prop up with one hand; it stays in the palm until you
  open the hand, and you can throw it. A thrown cup does no damage.
- Chairs and heavy props: lift them with both hands, carry them while
  walking, hit people with them or throw them. A chair hit counts as an
  attack. A heavy prop stays in your hands until you open them, also while
  walking or swinging.
- Pushing people: an open hand pushes people. A light push makes them step
  back, a firm push makes them stagger, a hard push with one or both hands
  knocks them down along the push, also people who normally only stagger.
  Pushes never hurt anyone or make them hostile.
- Walking up to someone with your hands down no longer pushes them.

### Weapons and controls

- The game's weapon wheel no longer opens. Weapons are chosen with the
  holsters.
- People react when you aim a gun at them.
- The stock auto-aim no longer turns John or the camera when you take a
  weapon or throwable.
- A gun stays in your hand after Dead Eye and after a duel while you keep
  holding it.
- The gun fires on the first trigger press after a close-kill scene.
- New row BUTTONS > GUN DEAD EYE (ON by default). OFF: R3 always crouches on
  foot and Dead Eye is left trigger + R3.
- Left trigger + R3 with empty hands crouches instead of turning the camera
  around.
- Dead Eye keeps the support hand on the barrel.
- Steep overhand knife and tomahawk throws are thrown instead of put away.
- Guns can be used from a stagecoach-type seat.
- The REALISTIC weapon preset no longer turns TWO HAND FIRE on. The row stays
  in the HANDS menu.

### Body, horse and coach

- Ride with one hand: move the rein hand to the side to turn, snaps with one
  hand do not steer. The other hand is free for a gun.
- Open the hand to drop the reins: half open is enough, the reins fall in
  about 0.35 s and the horse keeps its pace, so you can shoot on the move.
  An open hand never stops the horse; to stop, lift both hands with closed
  fists.
- Lifting one rein hand no longer stops the horse.
- A squeeze close to the body in the middle takes the reins instead of the
  lasso or a holster. Draw from the side as before.
- Taking the second rein again starts straight.
- The rein rope stays in your hand when the game lets go of it, for example
  while you reach for the gun on your back.
- Sitting in a coach no longer puts the reins in John's hand: take them
  yourself. All reins of the coach rest until you grab them.
- New row HORSE > COACH TURN: how far a hand moves for a full coach turn
  (12 to 45 cm, 28 by default). TURN PULL stays for the horse.
- Coach steering no longer sticks after a turn, and the reins drop as fast
  as on a horse.
- A snap at the top coach speed cracks the whip.
- The coach no longer doubles at speed with SpaceWarp, and the view follows
  the seat height instead of sinking into the coach.
- John stays upright under the view on the coach seat: the driving and whip
  animations no longer lean him into the camera, and his head no longer
  pops into view.
- New row BUTTONS > COACH REINS: drive a coach with the reins in your hands.
  Snap them to go faster, pull one hand to turn, lift both to stop.
- A steadier coach seat; HORSE SHAKE also brings back the seat bounce.
- John's head is shown when the view is away from it, for example when you
  are knocked down, so you no longer see a headless body.
- The arms keep following the controllers when a mission locks movement.
- Close-kill scenes hold the stereo view instead of flashing to the flat
  screen.

### HUD

- The player arrow on the minimap points where you look, relative to the
  map. The map itself still turns as in the game.

### Graphics

- New row HORSE > HORSE TAIL FIX (OFF by default): if a horse shows a second,
  frozen tail, switch it ON. Tails and manes of horses within 40 m are then
  simulated even when the game's own camera does not see them.
- Object glows and the beam on flagged items stay visible with SMALL
  OBJECTS on.
- SpaceWarp: people falling over no longer smear.

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
server: https://discord.gg/rFUacmVfW
