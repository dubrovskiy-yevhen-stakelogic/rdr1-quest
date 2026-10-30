#pragma once
// REINS: immersive riding - grab the reins with the hands, snap them down / up to make the
// horse walk, again faster, keep doing it to gallop, pull the right hand toward you = right, the left = left, both raised = stop.
// The reached pace is held (A held at CANTER+), x / y output independent, lighter / two-style steering, a
// standing turn only on a deliberate 300 ms rein, and a steering trace against the real horse yaw rate.
// Rules behind levers (default on, 0 = the base rule in the sections below):
//  a) RELEASE (.relopen): grip < .relgrip (8%) for .relms (500 ms), so a wobbling squeeze does not drop the rein;
//     .relidle: an opened hand also lets go during an inactivity hold / suspension;
//     a hand let go there re-grabs on a squeeze still held at the resume. At a holster socket the holsters' own open level
//     (.relout 30% for .reloutms 100 ms; default OFF = sticky reins) lets go too - else the draw squeeze is captured as REINS;
//     .relfreeze: an opening hand (the timer running) keeps its pose - its way to the hip does not swerve the horse.
//  b) ONE HAND (.onepull): one held hand steers by its own pull toward the rider: s = +pullR / -pullL (no neck rein / inward rule),
//     signed (.pushturn default 1 - pushed forward = the other way: one hand on a gun still turns both ways, and a forward-
//     arcing snap leaves no lasting turn); the second grab / 2 -> 1 handoff / snap re-zero keep that hand's turn. Two hands = direct rein.
//  c) STOP = LIFT (.lift; a stopping rider LIFTS both hands 15-35 cm, often arcing forward, no back pull): both hands up
//     >= .brakeup (15 cm) over their neutral height and alike (|upR - upL| < max(.alike 6 cm, .alikepct 30% of the larger)), or one
//     held hand up >= .liftone (18 cm), HELD .liftms (300 ms) -> the slow ladder (TROT+ -> WALK -> HALT, one step per .slowms 600 ms).
//     Held = the hands were also vertically still (< .liftv 0.5 m/s) for .liftstill (200 ms) of it - a go-snap's high wind-up
//     (tops 46 cm over the rest) passes the pose but never rests there; .liftfull: a lift stop finishes the ladder to HALT unless
//     a snap is accepted (a 0.4 s lift = SLOW then HALT); .spotlift: no standing turn arms while the pose is held.
//     Height leak .hleak (tau 4 s): the neutral HEIGHT follows hands held still (< 5 cm/s) while no stop pose is held, so a rider who
//     carries the reins higher than at the grab does not sit in a permanent stop pose (down only at .hleakdn 30 s - a lap rest
//     does not turn the riding height into a lift; at HALT after a stop the held pose blocks it for .hleakhold 5 s).
//  d) LOWERING (.lowsnap): a down-stroke whose top lies within .lowms (1.5 s) of a slow step and > .lowcm (8 cm) over the neutral height
//     is no snap (rejLift), by the top's sample time (the stroke start); a stop lift held (armed) at HALT after a stop keeps the
//     window open (its lowering, whenever it comes, a repeated stop lift) - at most .lowmax (3) such extra rejLifts per slow step; otherwise
//     no wait after a stop.
//
// Input thread only (GameXr controlsPadFilter, after the VrControls Mapper, MOUNTED layout while a riding sample is fresh).
//  - FRAME: pull / side / zone are measured in the HORSE frame in OpenXR LOCAL: yaw = head yaw (LOCAL) + horseMinusHead (riding sample,
//    fresh < 500 ms; else the frame yaw of the first grab), never the raw head yaw.
//  - GRAB: grip >= .55 while the hand is in the reins zone (forward 15..160 cm, down -10..120 cm, |side| <= 70 cm of the head) and holds
//    no weapon / support / phantom / lasso, is not at the holsters' foregrip / transfer / catch target, and the press did not begin in a
//    socket outside the zone (VrHolsters holsterTarget). RELEASE: grip < .30 for 100 ms, pose lost > 300 ms or the hand became busy; a
//    forced release re-arms a still-squeezed hand; a squeeze already down when the brain (re)activates is never a fresh press.
//  - NEUTRAL = the grab point; a hand that moves off it within 350 ms of its grab and comes to rest within 1 s re-zeroes there (settle;
//    also armed for 1.5 s by a > 10 deg step of the horse frame in LOCAL: snap turn / recenter). The second grab re-zeroes the first hand,
//    a release of one of two re-zeroes the other horizontally; a reference-space / input-epoch / continuity change or a resume after a
//    long suspension re-zeroes the held hands where they are. After a snap or a slow only the COMMON horizontal displacement is taken
//    out (the steer differential / a one-hand side offset stay), never the height; with .lat2 only the common FORWARD /
//    BACK part (the common side offset is the two-hand neck rein; one inside the dead zone = drift still goes), the second grab keeps
//    the first hand's neck-rein side offset and the 2 -> 1 handoff keeps the common side part (at most the remaining hand's own, the
//    same way) - no re-zero turns a held turn into the reverse one when the hands return (report rezeroSteer: the steer cm a re-zero
//    took away). Leak: 3 s horizontal while the rein is quiet; 10 s on a hand held still > 2.5 s whatever the steer (a wrong neutral
//    can never steer forever).
//  - SNAP (go / faster): the held hands' mean height drops >= .flick (5 cm) at >= .snapspeed (0.5 m/s) within 350 ms, then rises >= 2 cm
//    (or 40% of the drop); controller sample timestamps, refractory 450 ms, ignored 250 ms after a grab / release / busy change and while
//    slowing; a stroke that moved back toward the rider by > 0.6 x its drop is a yank (rejected). Gait 0 HALT, 1 WALK (stick y .walk),
//    2 TROT, 3 CANTER (+ one A spur), 4 GALLOP (+ one spur per further snap). The gait the rider reached STAYS until a slow / stop,
//    the loose-rein timeout, the manual stick or a reset - nothing decays it while a rein is held.
//  - A: the stock spur input is a
//    sagInputCombinerTapHold (bump detector, binary default tuning ctor 0x26d1bf4: press level .8, hold time .2 s, tap on the RELEASE):
//    a press released within .2 s = a tap = one spur (ApplyInput 0x26b8a44, tap edge), a press held past .2 s = the hold channel (index
//    0x11 -> +0xcc0 hold timer -> the keep-pace / speed-match flag). .hold_a 1: at CANTER+ A stays held; a spur = (A up 70 ms if down)
//    A 120 ms, A up 70 ms (the tap), then A held again. The player's own A merges, never swallowed: their press while the brain holds A
//    clears A for one update (their edge reaches the game), their release clears it 70 ms (the release half of their tap) before the
//    hold resumes. A is released on a slow step / HALT / gait < CANTER, while no rein is held, on the manual stick or a reset; never
//    pressed while the player holds A. .hold_a 2 = the hold press itself is the spur (a bump-on-PRESS tuning); 0 = taps only.
//  - STEER (a turn rate: held = keeps turning): two hands = direct rein pullR - pullL + .lat2 (0.6) x the mean side displacement of both
//    hands (neck-reining both hands sideways turns too; the pull difference alone is not enough, a both-hands-right move pulls the
//    crossing LEFT hand back toward the chest = a LEFT steer); one hand = neck rein, its side displacement (hand right = right), its
//    pull does not steer and a pull toward the body never counts its inward part. Dead 1.5 cm .dead (0.5 cm hysteresis), full
//    lock at .steer 8 cm, 60 ms smoothing .smooth; none while a hand settles. s > 0 = the rider wants RIGHT.
//  - SLOW: both hands back >= .brake (8 cm) AND raised >= .brakeup (8 cm) for 250 ms (one hand: the same) -> TROT+ to WALK, WALK to
//    HALT, one step per 600 ms while held; never armed at HALT (nothing to slow) or by a settling hand.
//  - OUTPUT (pad stick, x right +, y forward +, never back): x and y independent:
//    x = clamp(s * .gain, -1, 1), y = the gait's forward value (WALK .walk 0.40, TROT+ 1.0) - the throttle (keyed by max(local y, 0))
//    does not drop in a turn and the yaw reaches atan2(1, .4) 68 deg at WALK, atan2(1, 1) 45 deg at TROT+ (|v| <= 1.41: the normal
//    UpdateSim path passes it raw, atan2 keeps the direction); at TROT+ the last 20% of the lock (|x| .8 -> 1) lowers y
//    to cot(.yawmax) (55 deg: y .70), so the full lock turns 55 deg and the mid range keeps y 1;
//    .xy 0 = polar output (sin a, cos a), a = s * .yaw. HALT (0,0), or the STANDING TURN (+-1, 0: yaw 90 deg > the sim's
//    75 deg on-spot threshold) once |s| > .spotin 0.6 for .spotms 300 ms (exit < .35: a rider settling the reins does not spin the
//    horse back and forth). Sign chain: LOCAL yaw = atan2(-f.x, -f.z) (left +), horse yaw = head yaw + horseMinusHead (world headings,
//    left + too), pull = -(d . fwd), side = d . right; x > 0 -> pad sticks[0] > 0 = the Touch stick right -> horse-relative local x' =
//    -x -> yaw atan2(-x, y) < 0 = a RIGHT turn (heading + = left). Taken raw by the game's HORSE-RELATIVE mode (horseRelActive,
//    camGlueCMXGame+0x638 forced by the riding code; a 'false' without a new stamp = that code's read failure, +0x638 stays forced:
//    kept); else rotated into the native control frame with horseMinusCam; neither = nothing.
//  - TRACE: 10 Hz x 64 rows [t ms, gait, s, pullL, pullR, latL, latR cm, x, y, A (1 brain, 2 player, 4 cleared for the player's
//    edge), horse yaw rate deg/s (+ left, from horseHeadingWorld over >= 50 ms; a pair whose stamp moved between the loads is skipped),
//    head yaw relative to the horse deg (+ left; does a glance - shoulders and both hands turning with the head - steer?)];
//    frames steering left / right vs the horse turning left / right (> 10 deg/s) and agree / disagree (|s| > .3 and |rate| > 10 deg/s;
//    no horse speed reaches the brain: not speed-gated).
//  - LOOSE (no hand): steer 0, no taps, A up, the gait holds 3 s, then WALK, after 6 s HALT (a re-grab within that resumes the A hold).
//    A Touch stick > .3 = manual: gait 0, no output until the next snap.
//  - INACTIVITY: the FOOT layout (a dismount) or a menu / cutscene resets at once. A gap < .hold (300 ms) keeps everything and repeats
//    the last output - never into the stock pad (menu / Unknown); while the layout stays MOUNTED (optic, camera override, no riding
//    sample) the state is kept for as long as it lasts with no output and only the held hands masked; anything else > .hold resets.
//  - Arms: while REINS is on, both arms follow the controllers on a horse (VrBody armsFollow); holds(h) = the hands on the rope.
// Menu: CONTROLS > REINS GESTURES OFF / ON (controls.cfg "horse.reins"). Kill: debug.rdrvr.vr.reins 0.
// Props debug.rdrvr.vr.reins.* (read every ~256 updates): walk (stick %, 40) flick (cm, 5) snapspeed (cm/s, 50) steer (cm, 8) brake
// (cm, 8) brakeup (cm, 8) yaw (deg, 55; .xy 0 only) hold (ms, 300; 0 = reset at once) loose (s, 3; 0 = keep the gait); hold_a
// (1 tap + hold, 2 hold press = spur, 0 taps only) gain (x, 1.0) lat2 (x, 0.6; 0 = pull difference only) dead (cm, 1.5) smooth (ms,
// 60) spotin (%, 60) spotms (ms, 300; 0 = at once) xy (0 = polar output) yawmax (deg, 55; 45..75, TROT+ full lock);
// defaults are Tuning{} in the final unit, only a set prop is scaled; 0 = off: camrel (camera-relative fallback)
// spur (all A: taps and the hold) spot (standing turn) slow (slow / stop) leak (neutral leak) manual (stick resets the gait) haptics; settle
// (grab settle re-zero) step (frame step arms the settle) space (space / epoch re-zero) common (snap / slow re-zero common mode only;
// 0 = full) handoff (2 -> 1 hand re-zero) stillleak (still-hand leak) slowfast (TROT+ slows to WALK) yank (yank reject) inward (one-hand
// inward pull ignored) holster (holster targets block grabs / arming; VrHolsters prefers foregrip / transfer at the latch) maskheld
// (VrHolsters masks a held rein's grip bit) relkeep (horse-relative kept on a failure frame) suspend (MOUNTED-layout suspension).
// relopen relidle relgrip (%, 8) relms (ms, 500) onepull lift brakeup (cm: 15 for .lift; unset = 8 for the back + up rule)
// liftone (cm, 18) liftms (ms, 300) alike (cm, 6) alikepct (%, 30) hleak (s, 4; 0 = off) lowsnap lowms (ms, 1500) lowcm (cm, 8) slowms
// (ms, 600: the ladder step) pushturn (0 = only a pull turns); liftstill (ms, 200; 0 = off) liftv (cm/s, 50) liftfull
// spotlift hleakdn (s, 30; 0 = never down) hleakhold (ms, 5000) lowmax (count, 3; 0 = no cap) relout (grip %, 30; 0 = off) reloutms
// (ms, 100) relfreeze.
#include "VrHaptics.h"
#include "VrSettings.h" // HORSE > TURN PULL
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::reins {
inline std::atomic<bool> menuOn{true};         // CONTROLS row (VrControlMap.h storage)
inline std::atomic<bool> killed{false};        // debug.rdrvr.vr.reins 0
inline std::atomic<std::uint32_t> zoneMask{};  // hands whose grip press belongs to the reins this update (holster filter masks it)
inline std::atomic<std::uint32_t> heldMask{};
inline std::atomic<std::uint64_t> ridingNs{};  // main thread: last good riding camera sample
inline std::atomic<float> horseMinusHead{};    // main thread: rider heading - head yaw world (atan2(f.x, f.z) convention, radians)
inline std::atomic<std::uint64_t> horseMinusHeadNs{};
// (GameXr riding camera, main thread): the game reads the horse stick horse-relative (camGlueCMXGame+0x638 != 0) / the rider
// heading minus the native control-vector heading (sagPlayer+0x30, atan2(v.x, v.z); radians, + = the horse left of the camera frame).
// Defaults = not active / never published: the brain outputs nothing (NONE).
inline std::atomic<bool> horseRelActive{false}; inline std::atomic<std::uint64_t> horseRelNs{};
inline std::atomic<float> horseMinusCam{}; inline std::atomic<std::uint64_t> horseMinusCamNs{};
// the rider/horse forward heading in the WORLD (atan2(f.x, f.z) of the rider forward -c, heading increases to the LEFT), published
// by the riding camera sample (GameXr ridingAnchorSample) each riding frame; the reins trace derives the real horse turn rate from it.
inline std::atomic<float> horseHeadingWorld{}; inline std::atomic<std::uint64_t> horseHeadingNs{};
inline bool stampFresh(std::uint64_t stamp,std::uint64_t now,std::uint64_t maxAge) noexcept {return stamp&&(now>=stamp?now-stamp<maxAge:stamp-now<50'000'000ull);}
inline void publishHorseRel(std::uint64_t now,bool active) noexcept {horseRelActive.store(active,std::memory_order_relaxed);horseRelNs.store(now,std::memory_order_relaxed);}
inline void publishHorseMinusCam(std::uint64_t now,float rad) noexcept {
    if(std::isfinite(rad)){horseMinusCam.store(rad,std::memory_order_relaxed);horseMinusCamNs.store(now,std::memory_order_relaxed);}}
// a reach for the rifle barrel / a hip draw swinging forward must not take the rein: VrHolsters' pad filter (it runs right after
// the brain's, so the brain reads the previous update) publishes per hand what a press there means to the holsters: bit 0 = a socket
// or an empty-back / not-owned dead zone, bit 1 = the two-hand foregrip / a DUAL transfer / a THROW catch / a reload-owned free hand.
// Packed (ns & ~3) | bits, 0 = nothing; stale after 120 ms (holsters suspended / gated).
inline std::array<std::atomic<std::uint64_t>,2> holsterTargets{};
inline void publishHolsterTarget(unsigned hand,bool socket,bool target,std::uint64_t now) noexcept {
    holsterTargets[hand&1u].store(socket||target?(now&~3ull)|(socket?1ull:0ull)|(target?2ull:0ull):0ull,std::memory_order_relaxed);}
inline unsigned holsterTarget(unsigned hand,std::uint64_t now) noexcept {const auto v=holsterTargets[hand&1u].load(std::memory_order_relaxed);
    return stampFresh(v&~3ull,now,120'000'000ull)?unsigned(v&3ull):0u;}
// levers the holsters read (debug.rdrvr.vr.reins.holster / .maskheld, polled by the brain)
inline std::atomic<bool> holsterFirst{true},maskHeld{true};
// HARD STOP: librdr rdrHorseScriptControl::MakeHardStop(sagActor*)
// sets bit 0 of the player horse sim's +0xb74, which rdrHorseSim::UpdateSim ORs with the player's brake input and clears every sim frame
// (& 0xf0) - the game's own hard brake for one frame. The brain asks for it until this steady_clock time; VrBody's main-thread tick calls
// it on the mount (steadylink::mountActor) every frame until then.
// the MOUNTED layout was seen (any brain state, e.g. the mount animation): VrHolsters masks native grips on a horse
inline std::atomic<std::uint64_t> layoutMountedNs{0};
inline bool mountedLayout(std::uint64_t now) noexcept {const auto t=layoutMountedNs.load(std::memory_order_relaxed);return t&&now>=t&&now-t<300'000'000ull;}
// BRAKE PULL: the game's own pad brake = the stick pulled BACK at speed (sagPlayerInputHorse::UpdateBrakeStick:
// |stick| > 0.5 within 22.5 deg of straight back = full brake, steering zeroed) - in the CAMERA-relative Horse Control only (the forced
// horse-relative +0x638 = 1 reads back as 'turn around': y -1 spins the horse at -204 deg/s). For .brakepullms (600 ms) after a
// stop from TROT+ GameXr writes +0x638 = 0 and the brain sends the stick (0, -1); ended early once the horse yaws > 90 deg/s.
inline std::atomic<std::uint64_t> brakePullUntil{0};inline std::atomic<std::uint64_t> brakePullFrames{0},brakePullSpinEnds{0};
inline bool brakePullActive(std::uint64_t now) noexcept {const auto u=brakePullUntil.load(std::memory_order_relaxed);return u&&now<u;}
inline std::atomic<std::uint64_t> hardStopUntil{0};inline std::atomic<std::uint64_t> hardStopCalls{0},hardStopNoHorse{0},hardStopBitSeen{0};
// report: the horse sim's two guids (+0xad8 / +0xadc) vs the mount / player guids MakeHardStop compares, the sim pointer
inline std::atomic<std::uint32_t> hsSimAd8{0},hsSimAdc{0},hsHorseGuid{0},hsPlayerGuid{0};inline std::atomic<std::uintptr_t> hsSim{0};
// the mount's hrsBrakeControl tune (hrsHorseComponent = [horse+0x68], vtable 0x2d24f10; tune = [comp+0x1170] = hrsSimTune+0x420:
// [0] fSoftBrakeDecel, [1] fHardBrakeDecel - hrsBrakeControl::Update takes the HARD decel when the hard-stop bool is set and the brake
// amount <= 0). The hard-stop bit alone gives no felt change -> while the hard stop runs the HARD decel is x .hardmult (4),
// put back after. Report: the game's values.
inline std::atomic<float> hsDecayDelay{-1.f},hsDecayTimerSeen{-1.f};inline std::atomic<std::uint64_t> hsDecayTimerZeroed{0};
inline std::atomic<float> hsSoftDecel{-1.f},hsHardDecel{-1.f},hsHardUsed{-1.f};inline std::atomic<std::uint64_t> hsTuneBoosts{0},hsTuneBad{0};
inline bool enabled() noexcept {return menuOn.load(std::memory_order_relaxed)&&!killed.load(std::memory_order_relaxed);}
// VrBody: on a horse both arms follow the controllers while REINS is on (not just the gun arm).
inline bool ridingFresh(std::uint64_t now) noexcept {const auto r=ridingNs.load(std::memory_order_relaxed);
    return r&&(now>=r?now-r<400'000'000ull:r-now<50'000'000ull);}
inline bool armsFollow(std::uint64_t now) noexcept {return enabled()&&ridingFresh(now);}
inline bool claims(unsigned hand) noexcept {return (zoneMask.load(std::memory_order_relaxed)>>(hand&1u))&1u;}
// The hands that hold the reins (the arms still follow the controllers; the rope mode follows these hands).
inline bool holds(unsigned hand) noexcept {return (heldMask.load(std::memory_order_relaxed)>>(hand&1u))&1u;}
// while REINS runs on a horse an empty-hand grip that
// no socket / reins took never reaches the game as LB / RB (RB mounted = draw), also during a short inactivity hold.
inline std::atomic<bool> activeNow{false};
inline bool masksNativeGrip() noexcept {return activeNow.load(std::memory_order_relaxed);}
inline void publishRiding(std::uint64_t now,float horseMinusHeadRad,bool deltaValid) noexcept {
    ridingNs.store(now,std::memory_order_relaxed);
    if(deltaValid&&std::isfinite(horseMinusHeadRad)){horseMinusHead.store(horseMinusHeadRad,std::memory_order_relaxed);horseMinusHeadNs.store(now,std::memory_order_relaxed);}
}

struct Tuning {
    float walk{.40f},flick{.05f},snapSpeed{.50f},steer{.10f}, // steer: full lock 10 cm
    brake{.08f},brakeUp{.10f},yaw{.9599311f}; // yaw: 55 deg; brakeUp 10 cm
    // still-hand neutral re-learn (ms still before, tau s) and the WHIP snap (up then down: cm each way, window ms, m/s average)
    // .relsteer = the two-hand side term from the hands' spread (R away from L = right), .level = stop needs both hands at one height
    // .stopdirect: an armed lift stop goes straight to HALT. The lift arms after
    // .liftms 200 ms with .liftstill 150 ms under .liftv 0.25 m/s (a go-whip's top stays under 0.25 m/s for ~0.1 s only)
    // a real lift can be uneven (L 25 / R 43 cm): .level 0 =
    // the world level within .level (12 cm) OR the neutral-relative alike rule (10 cm / 50%: the grab heights differ), the symmetric
    // check = the STEER input (a turn) < .steer instead of raw diffs
    // HOLD STOP: go-whips reach 50-76 cm with a ~0.2 s flat top, so a stillness rule either misses the raised hands or fires on a whip.
    // On recorded rides no whip stays raised that long, the stop holds do. Both hands raised (mean >= .stopup 35 cm, each >= .stopmin 25 cm) for
    // .stopms (550 ms) = HALT, no stillness / level / symmetry. .dropstop: the reins opened while raised (mean >= .stopup) = HALT at once
    // (otherwise the loose rule keeps the GALLOP 3 s)
    // Two hands stop ONLY by this rule.
    // .brakeback: after a stop from TROT+ the stick is pulled back (y -1) for .brakebackms (500 ms),
    // stick 0 = the game's own coast-down
    // .brakeback OFF by default (y -1 after the stop spins the horse at -204 deg/s: horse-relative back = turn around, no brake)
    std::uint64_t relHighNs{800'000'000ull}; // .relhighms
    // FAST path = raised (the same mean / each) AND pulled back toward
    // the rider (mean pull >= .stoppull 6 cm) for .stopfastms 300 ms (recorded stops pull 8-14 cm back, the go-whips <= 4 cm); else
    // .stopms 550 ms (500 fires on a whip)
    float stopPull{.06f};std::uint64_t stopFastNs{300'000'000ull};
    std::uint64_t brakePullNs{0}; // .brakepullms (0 = off): (0,-1) spins the horse in 4 frames (-198 deg/s)
    bool holdStop{true},dropStop{true},brakeBack{false},hardStop{true};std::uint64_t hardStopNs{2'500'000'000ull};float stopUp{.35f},stopMin{.25f};std::uint64_t stopHoldNs{550'000'000ull},brakeBackNs{500'000'000ull}; // .hardstop / .hardstopms
    bool relSteer{true},letGo{true},relInvalid{false},symStop{true},stopDirect{true};float level{.12f},symCm{.12f}; // .symcm: a 5 cm lock rejects real lift frames
    std::uint64_t stillNs{2'500'000'000ull};float stillTau{10.f};bool whip{true};float whipUp{.04f},whipDown{.04f},whipSpeed{.35f};std::uint64_t whipNs{700'000'000ull};
    std::uint64_t holdNs{300'000'000ull},looseNs{3'000'000'000ull};
    bool camRel{true},spur{true},spot{true},slow{true},leak{true},manual{true},haptics{true};
    bool settle{true},step{true},space{true},common{true},handoff{true},stillLeak{true},slowFast{true},yank{true},inward{true},holster{true},
        relKeep{true},suspend{true};
    // A hold mode, steering law / input shaping, standing-turn dwell
    int holdA{1};float gain{1.f},lat2{1.f},dead{.03f}, // dead 3 cm
    smooth{.06f},spotIn{.45f};std::uint64_t spotNs{200'000'000ull};bool xy{true};
    // TROT+ full-lock yaw (55 deg; 45 = plain (1, 1))
    float yawMax{.9599311f};
    // release, one-hand pull steering, stop = lift + height leak, no snap from the lowering after a slow step
    bool relOpen{true},relIdle{true},onePull{true},lift{true},lowSnap{true},pushTurn{true};
    float relGrip{.08f},liftOne{.18f},alike{.10f},alikePct{.50f},hLeak{4.f},lowCm{.08f},brakeUp25{.08f};
    std::uint64_t relNs{500'000'000ull}, // both grips can dip to 0.00 for 0.1-0.3 s while held (the holsters see every dip as a new press): a shorter time drops the reins
    liftNs{200'000'000ull},lowNs{1'500'000'000ull},slowStepNs{600'000'000ull};
    // a HELD lift (vertically still), the ladder completes, the held stop at HALT, the lap-rest leak, the socket release
    float liftV{.25f},hLeakDn{30.f},relOut{0.f};int lowMax{3};
    std::uint64_t liftStillNs{150'000'000ull},hLeakHoldNs{5'000'000'000ull},relOutNs{100'000'000ull};
    bool liftFull{true},relFreeze{true},spotLift{true};
};
struct In {
    std::uint64_t now{},sampleNs{},sequence{};
    std::uint64_t space{},epoch{},continuity{}; // input identity (reference space / input epoch / mailbox continuity): a change re-zeroes
    bool mounted{};                       // MOUNTED layout on a horse (player state 5) + fresh riding sample (+ pad ready)
    bool layoutMounted{},onFoot{},stockPad{}; // the MOUNTED layout alone (suspend, never reset) / FOOT (a dismount: reset at once) /
                                          // NATIVE (menu / Unknown: the stock pad - never a held stick or A)
    bool haveHead{};std::array<float,3> head{},headFwd{};
    std::array<bool,2> valid{};std::array<std::array<float,3>,2> pos{};std::array<float,2> grip{};
    bool gripLive{};                      // grip / pos / busy come from a live controller sample (also while the brain is not live)
    std::array<bool,2> busy{};            // weapon / support / phantom / lasso owns that hand
    float userX{},userY{};                // Touch left stick
    bool userA{};                         // the player's own pad A (after the Mapper) is down
    bool menu{};                          // a real menu / cutscene: reset at once
};
struct Out {bool stick{};float x{},y{};bool a{};
    bool aUp{}; // clear pad A this update (the player's own press while the brain holds A: their edge reaches the game)
};
enum Mode : int {ModeNone=0,ModeHorseRel=1,ModeCamRel=2};
inline const char* modeName(int m) noexcept {return m==ModeHorseRel?"HORSE_REL":m==ModeCamRel?"CAM_REL":"NONE";}
inline const char* gaitName(int g) noexcept {static constexpr const char* n[5]{"HALT","WALK","TROT","CANTER","GALLOP"};return n[std::clamp(g,0,4)];}

struct Stats {
    std::atomic<std::uint64_t> updates{},activeFrames{},grabs{},releases{},flicks{},spurs{},brakes{},steerFrames{},userOverride{},
        rotated{},rejectedSlow{},downStarts{},relOpen{},relBusy{},relInvalid{},pressOutside{},
        relReset{},forcedReleases{},rejShallow{},rejNoUp{},rejRefractory{},rejSlowing{},slowSteps{},halts{},spurUserA{},spurOff{},
        standingTurns{},inactivityHolds{},resets{},looseDrops{},horseRelFrames{},noneFrames{},hapticPosts{},hapticDrops{},rezeros{},leakFrames{},
        rejYank{},settleRezeros{},spaceRezeros{},resumeRezeros{},handoffRezeros{},frameSteps{},stillLeakFrames{},inwardFrames{},
        dismountResets{},suspendFrames{},relKept{},holsterBlocked{},armBlocked{},
        // A hold / merge, standing-turn dwell, steer-vs-horse-turn sign check
        aHoldFrames{},aTaps{},aUserEdges{},aUserFollow{},aReleases{},spotShort{},
        turnFramesLeft{},turnFramesRight{},horseTurnLeft{},horseTurnRight{},signAgree{},signDisagree{},rateSamples{},rateRejected{},
        rateTorn{},rezeroSteerHits{}, // a heading pair changed between its loads / a re-zero removed > 1 cm of steer input
        idleReleases{},liftPoseFrames{},rejLift{},hLeakFrames{},
        relSocket{},stopLow{},stopLevel{},stopSym{},holdStops{},dropStops{},brakeBackFrames{},fastStops{},liftArms{},liftWindups{},liftFulls{},haltLiftFrames{},rejLiftCapped{},freezeFrames{},
        nowNs{};
    std::atomic<int> level{},held{},mode{},frameSrc{};
    std::atomic<float> steer{},pullL{},pullR{},latL{},latR{},lastDrop{},lastPeak{},maxDrop{},outX{},outY{};
    std::atomic<bool> outStick{},outA{},manual{},slowing{},settling{};
    // report: A state, the horse yaw rate, the steering input (cm), the live tuning
    std::atomic<int> aPhase{},holdA{1};std::atomic<bool> aHold{},aWant{},outAUp{},rateValid{};
    std::atomic<float> rateDegS{},steerInCm{},cfgSteer{.08f},cfgDead{.015f},cfgSmooth{.06f},cfgGain{1.f},cfgLat2{.6f},cfgSpotIn{.6f};
    std::atomic<std::uint64_t> headingNs{};std::atomic<int> cfgSpotMs{300};std::atomic<bool> cfgXy{true};
    // the steer input (cm, signed: + = a right steer taken away) the last re-zero removed / the largest |..|; the head yaw
    // relative to the horse (deg, + = looking left); TROT+ full-lock yaw, loose timeout
    std::atomic<float> rezeroSteerCm{},rezeroSteerMaxCm{},headRelDeg{},cfgYawMax{.9599311f};std::atomic<int> cfgLooseMs{3000};
    // the held hands' rise over their neutral height (m), the last rejLift top over the neutral (m), the live tuning
    std::atomic<float> upL{},upR{},lastLiftTop{},cfgRelGrip{.15f},cfgBrakeUp{.15f},cfgLiftOne{.18f},cfgAlike{.06f},cfgAlikePct{.30f},
        cfgHLeak{4.f},cfgLowCm{.08f};
    std::atomic<int> cfgRelMs{250},cfgLiftMs{300},cfgLowMs{1500},cfgSlowMs{600},cfgBits{0x1ff};
    // bits: relopen relidle onepull lift lowsnap pushturn liftfull relfreeze spotlift
    // the held-lift stillness, the height leak down / hold at HALT, the rejLift cap, the socket release
    std::atomic<float> cfgLiftV{.50f},cfgHLeakDn{30.f},cfgRelOut{.30f},vyL{},vyR{};
    std::atomic<int> cfgLiftStillMs{200},cfgHLeakHoldMs{5000},cfgLowMax{3},cfgRelOutMs{100};
    std::array<std::array<std::atomic<float>,3>,2> pressRel{}; // forward / down / right (m) of the last press per hand
    std::array<std::atomic<bool>,2> pressInZone{};
};
inline Stats stats{};
inline void bump(std::atomic<std::uint64_t>& c) noexcept {c.fetch_add(1,std::memory_order_relaxed);}

// the last 32 brain events for the report (t ms, kind, a, b). Single writer (input thread); a torn read only mixes two events.
// rezero a: 1 settle (b = hand), 2 space / epoch change, 3 resume after a long suspension, 4 one of two hands released (b = held bits);
// slow a = old gait * 10 + new gait, b = up cm (.lift: the lower hand's rise; a step of a completing ladder = 0) / pull cm (.lift 0);
// step a = deg, b = frame source; rejYank a = drop cm, b = back cm.
// spur a = gait, b = .hold_a; turn (standing) a = s, b = dwell ms; aRelease a = 1 slow / gait < CANTER, 2 HALT, 3 no rein held,
// 4 manual stick, 5 no stick output (mode NONE), b = gait.
enum Ev : std::uint8_t {EvNone,EvGrab,EvRelOpen,EvRelInvalid,EvRelBusy,EvRelReset,EvSnap,EvRejWeak,EvRejShallow,EvRejNoUp,EvRejRefractory,
    EvRejSlowing,EvSlow,EvHalt,EvSpur,EvLoose,EvManual,EvTurn,EvHold,EvReset,EvRejYank,EvRezero,EvStep,EvDismount,EvARelease,EvRejLift,
    EvRelSocket,EvCount};
inline constexpr const char* evNames[EvCount]{"none","grab","relOpen","relInvalid","relBusy","relReset","snap","rejWeak","rejShallow","rejNoUp",
    "rejRefractory","rejSlowing","slow","halt","spur","loose","manual","turn","hold","reset","rejYank","rezero","step","dismount","aRelease",
    "rejLift", // rejLift a = the stroke top over the neutral height (cm), b = ms from the window start (the last slow step / held
               // stop lift at HALT) to the stroke top (< 0: the top came first)
    "relSocket"}; // relSocket a = hand, b = 1 while inactive
struct EventRing {
    std::array<std::atomic<std::uint64_t>,32> head{},vals{};std::atomic<std::uint32_t> n{};
    void push(std::uint64_t now,Ev k,float a,float b) noexcept {
        const std::uint32_t i=n.load(std::memory_order_relaxed);std::uint32_t ua{},ub{};std::memcpy(&ua,&a,4);std::memcpy(&ub,&b,4);
        vals[i&31u].store((std::uint64_t(ua)<<32)|ub,std::memory_order_relaxed);
        head[i&31u].store(((now/1'000'000ull)<<8)|std::uint64_t(k),std::memory_order_relaxed);n.store(i+1,std::memory_order_release);
    }
};
inline EventRing events{};
// STEERING TRACE: 10 Hz while the brain is live, 64 rows
// (6.4 s). v = gait, s, pullL, pullR, latL, latR (cm), out x, out y, A (1 brain, 2 player, 4 cleared for the player's edge), horse yaw rate
// (deg/s, + = left, NaN = no fresh heading), head yaw relative to the horse (deg, + = looking left: do glances steer?).
// Single writer (input thread); a torn read only mixes two rows.
struct TraceRow {std::atomic<std::uint64_t> t{};std::array<std::atomic<float>,16> v{};}; // + upL, upR (cm), gripL, gripR, held bits
// CAPTURES: the trace window frozen 1 s after an event (a rein let go = 1, a wrong-way turn = 2), the last 4 - the 6.4 s ring
// alone rarely covers the moment of a problem
struct Capture {std::uint64_t at{};int kind{};unsigned n{};std::array<std::array<float,17>,64> rows{};};
inline std::array<Capture,4> captures{};inline std::atomic<unsigned> captureN{0};
inline std::array<TraceRow,64> trace{};inline std::atomic<std::uint32_t> traceN{};

class Brain {
    static constexpr std::uint64_t ms=1'000'000ull;
    enum Rz : int {RzSettle=1,RzSpace=2,RzResume=3,RzHandoff=4,RzLetGo=5};
    struct Hand {bool held{},pressed{},posValid{},zeroPending{};std::uint64_t armUntil{},openNs{},badNs{},posNs{},settleUntil{},settleStartBy{},stillNs{};
        std::array<float,3> neutral{},pos{},settleRef{};float speed{};
        float vy{};bool idleRel{};std::uint64_t outNs{},busyNs{},openHighNs{}; // |vertical speed| (m/s, 50 ms), let go by .relidle, socket timer
    };
    std::array<Hand,2> h_{};
    float headYaw_{},grabYaw_{};bool grabYawValid_{};
    float prevYaw_{},prevHeadYaw_{};bool frameValid_{};          // frame step detector
    std::uint64_t space_{},epoch_{},continuity_{},layoutNs_{},relTrueNs_{};
    int gait_{};bool manual_{},spot_{};Out last_{};int lastMode_{};
    // snap detector: mean height (y) and mean back coordinate (b, along -horse forward) of the held hands per controller sample
    struct S {std::uint64_t t{};float y{},b{};};
    std::array<S,128> ring_{};unsigned ringN_{},ringAt_{};
    float minY_{},minB_{};std::uint64_t minT_{};bool haveMin_{};
    std::uint64_t lastSeq_{},ignoreUntil_{},lastSnapT_{};unsigned prevHeld_{},prevBusy_{};
    // slow / stop
    bool slowCond_{},slowing_{},rezero_{};std::uint64_t slowNext_{},slowEnd_{};
    std::uint64_t lowRef_{}; // the lowering-is-no-snap window start = the last slow step (sample clock; + a held stop at HALT)
    // the lift-pose episode (pose time / vertically still time, s), armed = held; the ladder completing to HALT; a slow
    // step since the last accepted snap; rejLifts since the last slow step; the stop pose time at HALT since then (s)
    bool liftEp_{},liftArmed_{},pendHalt_{},stopped_{};float liftStillS_{},haltPoseS_{};int rejLiftN_{};std::uint64_t liftStart_{};
    std::uint64_t stepTs_{}; // the last slow step (sample clock): the base window; lowRef_ beyond it = a held stop at HALT (capped)
    // steer
    float steerS_{},letPeak_{},letAvg_{};bool letEased_{};std::uint64_t hiSince_{},hiPullSince_{},brakeBackUntil_{},lastStopNs_{},wrongSince_{},lastWrongCap_{},capPendNs_{},capPendAt_{};int capPendKind_{};float upPrev_[2]{};std::uint64_t letCalmNs_{};bool engaged_{},lock_{};std::uint64_t stepNs_{},buzzNs_{}; // hiSince_ / upPrev_ = the hold stop / drop stop
    float latPrev_[2]{}; // the held hands' side offsets of the previous update (the 2 -> 1 hand handoff)
    float pullPrev_[2]{}; // .onepull: their pulls of the previous update (the handoff keeps a direct-rein turn as the one-hand pull)
    std::uint64_t spotArmNs_{}; // |s| > .spotin since (the standing turn's dwell)
    // loose reins
    std::uint64_t looseSince_{};
    // A: AIdle = A follows the hold (want), AGap / AGap2 = A up (before a tap / the tap's release), ATap = A down (the press)
    enum APh : int {AIdle,AGap,ATap,AGap2};APh aPh_{AIdle};std::uint64_t aUntil_{};
    bool aHold_{},aOut_{},prevWant_{},prevUserA_{};
    // horse yaw rate (trace): the heading baseline >= 50 ms back
    float hdg_{},rate_{};std::uint64_t hdgNs_{},rateNs_{},traceNs_{};
    // inactivity hold
    std::uint64_t lastActive_{};bool holding_{};
    // haptics schedule (ticks are posted when due: the haptic queue drops pulses older than 100 ms)
    struct Hap {std::uint64_t due{};unsigned mask{};float amp{};std::uint32_t us{};std::uint16_t src{};};
    std::array<Hap,12> hq_{};std::array<std::uint64_t,2> hapNs_{};
    Tuning t_{};unsigned propTick_{};
    static float yawOf(const std::array<float,3>& f) noexcept {return std::atan2(-f[0],-f[2]);}
    static std::array<float,3> fwdOf(float y) noexcept {return {-std::sin(y),0.f,-std::cos(y)};}
    static std::array<float,3> rightOf(float y) noexcept {return {std::cos(y),0.f,-std::sin(y)};}
    static float dot(const std::array<float,3>& a,const std::array<float,3>& b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
    static std::array<float,3> sub(const std::array<float,3>& a,const std::array<float,3>& b) noexcept {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
    void readProps() noexcept {
#if defined(__ANDROID__)
        char v[PROP_VALUE_MAX]{};
        auto get=[&](const char* name)->bool{v[0]=0;return __system_property_get(name,v)>0&&v[0];};
        // def is in the FINAL unit and returned as is; only a SET prop is scaled (prop x scale). The defaults come from
        // Tuning{}.
        const Tuning d{};
        auto num=[&](const char* name,float def,float scale,float lo,float hi,bool zero=false){
            if(get(name)){const float x=float(std::atof(v));if(std::isfinite(x)&&(x>0||(zero&&x==0)))return std::clamp(x*scale,zero?0.f:lo,hi);}return def;};
        auto on=[&](const char* name){return !(get(name)&&v[0]=='0');};
        killed.store(!on("debug.rdrvr.vr.reins"),std::memory_order_relaxed);
        t_.walk=num("debug.rdrvr.vr.reins.walk",d.walk,.01f,.15f,.80f);
        t_.flick=num("debug.rdrvr.vr.reins.flick",d.flick,.01f,.03f,.30f);
        t_.snapSpeed=num("debug.rdrvr.vr.reins.snapspeed",d.snapSpeed,.01f,.15f,3.f);
        t_.stillNs=std::uint64_t(num("debug.rdrvr.vr.reins.stillms",float(d.stillNs/ms),1.f,100.f,10000.f))*ms;
        t_.stillTau=num("debug.rdrvr.vr.reins.stilltau",d.stillTau,.001f,.2f,60.f); // prop in ms
        t_.relSteer=on("debug.rdrvr.vr.reins.relsteer");t_.letGo=on("debug.rdrvr.vr.reins.letgo");t_.relInvalid=get("debug.rdrvr.vr.reins.relinvalid")&&v[0]=='1';t_.symStop=on("debug.rdrvr.vr.reins.symstop");t_.stopDirect=on("debug.rdrvr.vr.reins.stopdirect");t_.holdStop=on("debug.rdrvr.vr.reins.holdstop");t_.relHighNs=std::uint64_t(num("debug.rdrvr.vr.reins.relhighms",float(d.relHighNs/ms),1.f,0.f,3000.f,true))*ms;t_.brakeBack=get("debug.rdrvr.vr.reins.brakeback")&&v[0]=='1';
        t_.brakePullNs=std::uint64_t(num("debug.rdrvr.vr.reins.brakepullms",float(d.brakePullNs/ms),1.f,0.f,3000.f,true))*ms;t_.hardStop=on("debug.rdrvr.vr.reins.hardstop");
        t_.hardStopNs=std::uint64_t(num("debug.rdrvr.vr.reins.hardstopms",float(d.hardStopNs/ms),1.f,50.f,5000.f))*ms;
        t_.brakeBackNs=std::uint64_t(num("debug.rdrvr.vr.reins.brakebackms",float(d.brakeBackNs/ms),1.f,50.f,3000.f))*ms;t_.dropStop=on("debug.rdrvr.vr.reins.dropstop");
        t_.stopUp=num("debug.rdrvr.vr.reins.stopup",d.stopUp,.01f,.05f,1.f);t_.stopMin=num("debug.rdrvr.vr.reins.stopmin",d.stopMin,.01f,0.f,1.f,true);
        t_.stopHoldNs=std::uint64_t(num("debug.rdrvr.vr.reins.stopms",float(d.stopHoldNs/ms),1.f,100.f,3000.f))*ms;t_.stopPull=num("debug.rdrvr.vr.reins.stoppull",d.stopPull,.01f,-1.f,.5f,true);
        t_.stopFastNs=std::uint64_t(num("debug.rdrvr.vr.reins.stopfastms",float(d.stopFastNs/ms),1.f,50.f,3000.f))*ms;t_.symCm=num("debug.rdrvr.vr.reins.symcm",d.symCm,.01f,.03f,.50f);t_.level=num("debug.rdrvr.vr.reins.level",d.level,.01f,0.f,.40f,true);
        t_.whip=on("debug.rdrvr.vr.reins.whip");t_.whipUp=num("debug.rdrvr.vr.reins.whipup",d.whipUp,.01f,.02f,.40f);
        t_.whipDown=num("debug.rdrvr.vr.reins.whipdown",d.whipDown,.01f,.02f,.40f);t_.whipSpeed=num("debug.rdrvr.vr.reins.whipspeed",d.whipSpeed,.01f,.1f,3.f);
        t_.whipNs=std::uint64_t(num("debug.rdrvr.vr.reins.whipms",float(d.whipNs/ms),1.f,150.f,2000.f))*ms;
        t_.steer=num("debug.rdrvr.vr.reins.steer",::rdrvr::vr::settings().rideTurnMeters(),.01f,.03f,.40f); // HORSE > TURN PULL
        t_.dead=std::fmin(t_.dead,.6f*t_.steer); // the dead zone stays below the lock
        // levers
        t_.holdA=get("debug.rdrvr.vr.reins.hold_a")?std::clamp(std::atoi(v),0,2):d.holdA;
        t_.gain=num("debug.rdrvr.vr.reins.gain",d.gain,1.f,.2f,3.f);
        t_.lat2=num("debug.rdrvr.vr.reins.lat2",d.lat2,1.f,0.f,2.f,true);
        t_.dead=num("debug.rdrvr.vr.reins.dead",d.dead,.01f,.006f,.06f);
        t_.smooth=num("debug.rdrvr.vr.reins.smooth",d.smooth,.001f,0.f,.5f,true);
        t_.spotIn=num("debug.rdrvr.vr.reins.spotin",d.spotIn,.01f,.4f,.95f);
        t_.spotNs=std::uint64_t(num("debug.rdrvr.vr.reins.spotms",float(d.spotNs/ms),1.f,0.f,2000.f,true))*ms;
        t_.xy=on("debug.rdrvr.vr.reins.xy");
        t_.yawMax=num("debug.rdrvr.vr.reins.yawmax",d.yawMax,.01745329f,.7853982f,1.3089969f); // deg 45..75 (75 = the on-spot threshold)
        t_.brake=num("debug.rdrvr.vr.reins.brake",d.brake,.01f,.04f,.40f);
        t_.brakeUp=num("debug.rdrvr.vr.reins.brakeup",d.brakeUp,.01f,.02f,.40f); // a stop = the lift (or back AND up)
        t_.brakeUp25=num("debug.rdrvr.vr.reins.brakeup",d.brakeUp25,.01f,.02f,.40f); // .lift 0 = the back + up rule (8 cm unless set)
        // levers (header)
        t_.relOpen=on("debug.rdrvr.vr.reins.relopen");t_.relIdle=on("debug.rdrvr.vr.reins.relidle");t_.onePull=on("debug.rdrvr.vr.reins.onepull");
        t_.lift=on("debug.rdrvr.vr.reins.lift");t_.lowSnap=on("debug.rdrvr.vr.reins.lowsnap");
        t_.pushTurn=on("debug.rdrvr.vr.reins.pushturn"); // default 1 = the signed law (0 = only a pull turns)
        t_.liftFull=on("debug.rdrvr.vr.reins.liftfull");t_.relFreeze=on("debug.rdrvr.vr.reins.relfreeze");t_.spotLift=on("debug.rdrvr.vr.reins.spotlift");
        t_.liftV=num("debug.rdrvr.vr.reins.liftv",d.liftV,.01f,.10f,3.f); // cm/s
        t_.liftStillNs=std::uint64_t(num("debug.rdrvr.vr.reins.liftstill",float(d.liftStillNs/ms),1.f,0.f,2000.f,true))*ms;
        t_.hLeakDn=num("debug.rdrvr.vr.reins.hleakdn",d.hLeakDn,1.f,0.f,600.f,true);
        t_.hLeakHoldNs=std::uint64_t(num("debug.rdrvr.vr.reins.hleakhold",float(d.hLeakHoldNs/ms),1.f,0.f,60000.f,true))*ms;
        t_.lowMax=get("debug.rdrvr.vr.reins.lowmax")?std::clamp(std::atoi(v),0,100):d.lowMax;
        t_.relOut=num("debug.rdrvr.vr.reins.relout",d.relOut,.01f,0.f,.60f,true);
        t_.relOutNs=std::uint64_t(num("debug.rdrvr.vr.reins.reloutms",float(d.relOutNs/ms),1.f,0.f,1000.f,true))*ms;
        t_.relGrip=num("debug.rdrvr.vr.reins.relgrip",d.relGrip,.01f,.05f,.50f);
        t_.relNs=std::uint64_t(num("debug.rdrvr.vr.reins.relms",float(d.relNs/ms),1.f,0.f,2000.f,true))*ms;
        t_.liftOne=num("debug.rdrvr.vr.reins.liftone",d.liftOne,.01f,.05f,.50f);
        t_.liftNs=std::uint64_t(num("debug.rdrvr.vr.reins.liftms",float(d.liftNs/ms),1.f,0.f,2000.f,true))*ms;
        t_.alike=num("debug.rdrvr.vr.reins.alike",d.alike,.01f,0.f,.40f,true);
        t_.alikePct=num("debug.rdrvr.vr.reins.alikepct",d.alikePct,.01f,0.f,1.f,true);
        t_.hLeak=num("debug.rdrvr.vr.reins.hleak",d.hLeak,1.f,0.f,60.f,true);
        t_.lowNs=std::uint64_t(num("debug.rdrvr.vr.reins.lowms",float(d.lowNs/ms),1.f,0.f,5000.f,true))*ms;
        t_.lowCm=num("debug.rdrvr.vr.reins.lowcm",d.lowCm,.01f,0.f,.50f,true);
        t_.slowStepNs=std::uint64_t(num("debug.rdrvr.vr.reins.slowms",float(d.slowStepNs/ms),1.f,50.f,3000.f))*ms;
        t_.yaw=num("debug.rdrvr.vr.reins.yaw",d.yaw,.01745329f,.1745329f,1.4835299f); // deg 10..85
        t_.holdNs=std::uint64_t(num("debug.rdrvr.vr.reins.hold",float(d.holdNs/ms),1.f,0.f,2000.f,true))*ms;
        t_.looseNs=std::uint64_t(num("debug.rdrvr.vr.reins.loose",float(d.looseNs/ms),1000.f,0.f,30000.f,true))*ms; // prop s, def ms
        t_.camRel=on("debug.rdrvr.vr.reins.camrel");t_.spur=on("debug.rdrvr.vr.reins.spur");t_.spot=on("debug.rdrvr.vr.reins.spot");
        t_.slow=on("debug.rdrvr.vr.reins.slow");t_.leak=on("debug.rdrvr.vr.reins.leak");t_.manual=on("debug.rdrvr.vr.reins.manual");
        t_.haptics=on("debug.rdrvr.vr.reins.haptics");
        t_.settle=on("debug.rdrvr.vr.reins.settle");t_.step=on("debug.rdrvr.vr.reins.step");t_.space=on("debug.rdrvr.vr.reins.space");
        t_.common=on("debug.rdrvr.vr.reins.common");t_.handoff=on("debug.rdrvr.vr.reins.handoff");t_.stillLeak=on("debug.rdrvr.vr.reins.stillleak");
        t_.slowFast=on("debug.rdrvr.vr.reins.slowfast");t_.yank=on("debug.rdrvr.vr.reins.yank");t_.inward=on("debug.rdrvr.vr.reins.inward");
        t_.holster=on("debug.rdrvr.vr.reins.holster");t_.relKeep=on("debug.rdrvr.vr.reins.relkeep");t_.suspend=on("debug.rdrvr.vr.reins.suspend");
        holsterFirst.store(t_.holster,std::memory_order_relaxed);maskHeld.store(on("debug.rdrvr.vr.reins.maskheld"),std::memory_order_relaxed);
#endif
        stats.holdA.store(t_.holdA,std::memory_order_relaxed);stats.cfgSteer.store(t_.steer,std::memory_order_relaxed);
        stats.cfgDead.store(t_.dead,std::memory_order_relaxed);stats.cfgSmooth.store(t_.smooth,std::memory_order_relaxed);
        stats.cfgGain.store(t_.gain,std::memory_order_relaxed);stats.cfgLat2.store(t_.lat2,std::memory_order_relaxed);
        stats.cfgSpotIn.store(t_.spotIn,std::memory_order_relaxed);stats.cfgSpotMs.store(int(t_.spotNs/ms),std::memory_order_relaxed);
        stats.cfgXy.store(t_.xy,std::memory_order_relaxed);stats.cfgYawMax.store(t_.yawMax,std::memory_order_relaxed);
        stats.cfgLooseMs.store(int(t_.looseNs/ms),std::memory_order_relaxed);
        stats.cfgRelGrip.store(t_.relGrip,std::memory_order_relaxed);stats.cfgBrakeUp.store(t_.brakeUp,std::memory_order_relaxed);
        stats.cfgLiftOne.store(t_.liftOne,std::memory_order_relaxed);stats.cfgAlike.store(t_.alike,std::memory_order_relaxed);
        stats.cfgAlikePct.store(t_.alikePct,std::memory_order_relaxed);stats.cfgHLeak.store(t_.hLeak,std::memory_order_relaxed);
        stats.cfgLowCm.store(t_.lowCm,std::memory_order_relaxed);stats.cfgRelMs.store(int(t_.relNs/ms),std::memory_order_relaxed);
        stats.cfgLiftMs.store(int(t_.liftNs/ms),std::memory_order_relaxed);stats.cfgLowMs.store(int(t_.lowNs/ms),std::memory_order_relaxed);
        stats.cfgSlowMs.store(int(t_.slowStepNs/ms),std::memory_order_relaxed);
        stats.cfgBits.store((t_.relOpen?1:0)|(t_.relIdle?2:0)|(t_.onePull?4:0)|(t_.lift?8:0)|(t_.lowSnap?16:0)|(t_.pushTurn?32:0)|
            (t_.liftFull?64:0)|(t_.relFreeze?128:0)|(t_.spotLift?256:0),std::memory_order_relaxed);
        stats.cfgBrakeUp.store(t_.lift?t_.brakeUp:t_.brakeUp25,std::memory_order_relaxed); // the effective threshold
        stats.cfgLiftV.store(t_.liftV,std::memory_order_relaxed);stats.cfgHLeakDn.store(t_.hLeakDn,std::memory_order_relaxed);
        stats.cfgRelOut.store(t_.relOut,std::memory_order_relaxed);stats.cfgLiftStillMs.store(int(t_.liftStillNs/ms),std::memory_order_relaxed);
        stats.cfgHLeakHoldMs.store(int(t_.hLeakHoldNs/ms),std::memory_order_relaxed);stats.cfgLowMax.store(t_.lowMax,std::memory_order_relaxed);
        stats.cfgRelOutMs.store(int(t_.relOutNs/ms),std::memory_order_relaxed);
    }
    // .relopen: a held hand lets go after grip < .relgrip for .relms; 0 = grip < .30 for 100 ms
    // the two-hand steer input (m, + = right): pullR - pullL + .lat2 x the side term (.relsteer: the spread signed by the side that moved)
    // The hands coming CLOSER must turn too (the right hand moved toward the left one = left): the side moves minus the part both
    // hands share (both carried the same way = a lean / torso turn = 0; one hand out OR in = its own side move; spread apart = ~0)
    float twoSteer(const float* pull,const float* lat) const noexcept {float side=.5f*(lat[0]+lat[1]);
        if(t_.relSteer){const float c=lat[0]*lat[1]>0.f?std::copysign(std::fmin(std::fabs(lat[0]),std::fabs(lat[1])),lat[0]):0.f;
            side=lat[0]+lat[1]-2.f*c;}
        // only a pull TOWARD the rider steers - a hand pushed forward while it moves out
        // sideways (the right arm reaching forward-right) would read as 'the left rein drawn' and beat the side term
        return (std::fmax(pull[1],0.f)-std::fmax(pull[0],0.f))+t_.lat2*side;}
    float relGrip() const noexcept {return t_.relOpen?t_.relGrip:.30f;}
    std::uint64_t relNs() const noexcept {return t_.relOpen?t_.relNs:100*ms;}
    // .onepull: the one-hand steer (m, + = right) of hand i at pull p, the signed law s = +pullR / -pullL
    // (.pushturn default 1 - a rein hand pushed forward turns the other way, so a rider with a gun in one hand still turns both ways, and
    // the snap / slow re-zero keeps any one-hand displacement beyond the dead zone: a forward-arcing stroke leaves no lasting turn);
    // .pushturn 0 = only a pull toward the rider turns
    float oneSteer(unsigned i,float p) const noexcept {const float q=t_.pushTurn?p:std::fmax(p,0.f);return i==1u?q:-q;}
    unsigned heldBits() const noexcept {return (h_[0].held?1u:0u)|(h_[1].held?2u:0u);}
    // ---- haptics: at most one post per hand per update and 12 ms apart (never floods the 32-entry queue) ----
    void hap(std::uint64_t due,unsigned mask,float amp,std::uint32_t us,std::uint16_t src=haptics::SourceOther) noexcept {
        if(!t_.haptics||!(mask&3u))return;
        for(auto& q:hq_)if(!q.mask){q={due,mask&3u,amp,us,src};return;}
        bump(stats.hapticDrops);
    }
    void hapRun(std::uint64_t now) noexcept {
        std::array<bool,2> posted{};
        for(auto& q:hq_){if(!q.mask||now<q.due)continue;
            if(now-q.due>150*ms){q.mask=0;bump(stats.hapticDrops);continue;} // late (after a hold): dropped, never a burst
            for(unsigned i=0;i<2;++i)if(((q.mask>>i)&1u)&&!posted[i]&&(!hapNs_[i]||now<hapNs_[i]||now-hapNs_[i]>=12*ms)){
                bump(haptics::pulse(i,q.amp,q.us,now,q.src)?stats.hapticPosts:stats.hapticDrops);hapNs_[i]=now;posted[i]=true;q.mask&=~(1u<<i);}
        }
    }
    // ---- spur: the stock tap/hold combiner taps on a release within .2 s and holds past .2 s (header). hold_a 1: (up 70 ms if
    // the brain's A is down) A 120 ms, up 70 ms, then A held; 2: (up 70 ms if down) then A held - the press is the spur; 0: (up 70 ms if
    // down) A 120 ms, then up. From the first spur at CANTER+ on, A stays held (aHold_) until the gait drops below CANTER ----
    void requestSpur(std::uint64_t now,bool userA,int mode) noexcept {
        if(!t_.spur||mode==ModeNone){bump(stats.spurOff);return;}
        if(t_.holdA>0)aHold_=true;               // the pace the rider reached is kept (stock: keep A held)
        if(userA){bump(stats.spurUserA);return;} // the player's own A passes untouched (their release resumes the hold, aStep)
        if(aPh_==AGap){bump(stats.spurs);events.push(now,EvSpur,float(gait_),float(t_.holdA));return;} // a fresh edge already on its way
        if(aOut_||aPh_==ATap){aPh_=AGap;aUntil_=now+70*ms;}  // A down (held / tapping): up first, else the game sees no new press
        else if(t_.holdA==2)aPh_=AIdle;                      // A up: the hold press itself is the edge
        else{aPh_=ATap;aUntil_=now+120*ms;bump(stats.aTaps);}
        bump(stats.spurs);events.push(now,EvSpur,float(gait_),float(t_.holdA));
    }
    bool aStep(std::uint64_t now,bool want) noexcept {
        if(aPh_==AGap&&now>=aUntil_){if(t_.holdA==2&&want)aPh_=AIdle;else{aPh_=ATap;aUntil_=now+120*ms;bump(stats.aTaps);}}
        if(aPh_==ATap&&now>=aUntil_){if(t_.holdA==1&&want){aPh_=AGap2;aUntil_=now+70*ms;}else aPh_=AIdle;} // the release = the stock tap
        if(aPh_==AGap2&&now>=aUntil_)aPh_=AIdle;
        const bool hold=aPh_==AIdle&&want&&t_.holdA>0;if(hold)bump(stats.aHoldFrames);
        return aPh_==ATap||hold;
    }
    void captureAt(std::uint64_t now,int kind) noexcept {if(!capPendNs_){capPendNs_=now+1'000*ms;capPendAt_=now/ms;capPendKind_=kind;}}
    void release(unsigned i,Ev why,std::uint64_t now,bool idle=false) noexcept {
        auto& h=h_[i];if(!h.held)return;h.held=false;h.openNs=h.badNs=h.outNs=0;h.settleUntil=h.stillNs=0;h.zeroPending=false;bump(stats.releases);
        bump(why==EvRelOpen?stats.relOpen:why==EvRelSocket?stats.relSocket:why==EvRelInvalid?stats.relInvalid:why==EvRelBusy?stats.relBusy:stats.relReset);
        if(why!=EvRelOpen&&why!=EvRelSocket){bump(stats.forcedReleases);if(h.pressed)h.armUntil=~0ull;} // a still-squeezed hand re-grabs at once
        events.push(now,why,float(i),idle?1.f:0.f);if(why!=EvRelReset)captureAt(now,1);
        // .dropstop: the hand opened while the held hands were raised (the last update's mean height) = HALT now, not the loose GALLOP
        if(why==EvRelOpen&&t_.dropStop&&gait_>0){const unsigned o=i^1u;const float m=h_[o].held?.5f*(upPrev_[0]+upPrev_[1]):upPrev_[i];
            if(m>=t_.stopUp){if(t_.brakeBack)brakeBackUntil_=now+t_.brakeBackNs;
                if(t_.hardStop&&gait_>=2)hardStopUntil.store(now+t_.hardStopNs,std::memory_order_relaxed);gait_=0;stopped_=true;aPh_=AIdle;aHold_=false;looseSince_=0;slowing_=false;bump(stats.dropStops);bump(stats.halts);
                events.push(now,EvHalt,1.f,m*100.f);}}
    }
    // at a holster socket (holsterTarget bit 0) a held rein lets go at the holsters' own open
    // level (grip <= .relout 30% for .reloutms 100 ms): VrHolsters re-arms its press at <= 30% and latches the next one at >= 65%, and a
    // rein still held there (claims) would capture that squeeze as REINS - the draw is swallowed and the hand at the hip reads as a full pull.
    // A fast draw squeezes before the 100 ms: a hand that opened there and closes again (>= .55, the press the holsters latch this
    // update - they run after the brain) lets go at once too.
    bool socketOpen(Hand& h,float g,bool socket,std::uint64_t now) const noexcept {
        if(t_.relOut<=0.f||!socket){h.outNs=0;return false;}
        if(g<=t_.relOut){if(!h.outNs)h.outNs=now;return now>=h.outNs&&now-h.outNs>=t_.relOutNs;}
        return h.outNs&&g>=.55f;}
    void clearMasks() noexcept {zoneMask.store(0,std::memory_order_relaxed);heldMask.store(0,std::memory_order_relaxed);activeNow.store(false,std::memory_order_relaxed);}
    void resetAll(std::uint64_t now,bool log) noexcept {
        for(unsigned i=0;i<2;++i)release(i,EvRelReset,now);
        gait_=0;manual_=spot_=false;steerS_=0;engaged_=lock_=false;slowCond_=slowing_=rezero_=false;looseSince_=0;spotArmNs_=0;lowRef_=0;
        liftEp_=liftArmed_=pendHalt_=stopped_=false;liftStillS_=haltPoseS_=0.f;rejLiftN_=0;stepTs_=liftStart_=0;for(auto& h:h_)h.idleRel=false;
        aPh_=AIdle;aUntil_=0;aHold_=aOut_=prevWant_=false;traceNs_=0;
        ringN_=0;haveMin_=false;grabYawValid_=false;holding_=false;last_={};lastMode_=ModeNone;prevHeld_=0;frameValid_=false;
        for(auto& q:hq_)q.mask=0;
        if(log){bump(stats.resets);events.push(now,EvReset,0.f,0.f);}
    }
    // the hands did not move but their LOCAL coordinates may have (space / epoch / continuity change, a long suspension):
    // the held hands re-zero on their next valid sample; detector / steer / slow restart, 250 ms settle
    void rebase(std::uint64_t now,int why) noexcept {
        for(auto& h:h_){h.posValid=false;h.speed=h.vy=0.f;h.stillNs=h.settleUntil=0;if(h.held)h.zeroPending=true;}
        ringN_=0;haveMin_=false;ignoreUntil_=now+250*ms;steerS_=0.f;engaged_=spot_=lock_=false;slowCond_=slowing_=rezero_=false;frameValid_=false;
        liftEp_=liftArmed_=pendHalt_=false;
        spotArmNs_=0;bump(stats.rezeros);events.push(now,EvRezero,float(why),float(heldBits()));
    }
    // the raw steer input (m, no inward rule / dead zone) of the held hands now: two = direct rein + .lat2 x the mean side, one = its side
    // (.onepull: one = its pull, + right hand / - left hand)
    float steerIn(const std::array<float,3>& zf,const std::array<float,3>& zr) const noexcept {
        float p[2]{},l[2]{};for(unsigned i=0;i<2;++i)if(h_[i].held){const auto dd=sub(h_[i].pos,h_[i].neutral);p[i]=-dot(dd,zf);l[i]=dot(dd,zr);}
        if(h_[0].held&&h_[1].held)return (p[1]-p[0])+t_.lat2*.5f*(l[0]+l[1]);
        if(t_.onePull)return h_[1].held?oneSteer(1u,p[1]):h_[0].held?oneSteer(0u,p[0]):0.f;
        return h_[0].held?l[0]:h_[1].held?l[1]:0.f;}
    // the steer input a re-zero took away (a held turn the rider must now redo, or the reverse turn when the hands return)
    void noteRezeroSteer(float before,float after) noexcept {const float cm=(before-after)*100.f;stats.rezeroSteerCm.store(cm,std::memory_order_relaxed);
        if(std::fabs(cm)>std::fabs(stats.rezeroSteerMaxCm.load(std::memory_order_relaxed)))stats.rezeroSteerMaxCm.store(cm,std::memory_order_relaxed);
        if(std::fabs(cm)>1.f)bump(stats.rezeroSteerHits);}
    // set a held hand's horizontal neutral so that it reads pull 0 and side = keep (m) now; the height is never touched
    static void zeroKeepSide(Hand& h,const std::array<float,3>& zr,float keep) noexcept {h.neutral[0]=h.pos[0]-zr[0]*keep;h.neutral[2]=h.pos[2]-zr[2]*keep;}
    // .onepull: the same, reading pull = keep (m, + = toward the rider) and side 0 now
    static void zeroKeepPull(Hand& h,const std::array<float,3>& zf,float keep) noexcept {h.neutral[0]=h.pos[0]+zf[0]*keep;h.neutral[2]=h.pos[2]+zf[2]*keep;}
    // after a snap / a slow only the COMMON horizontal displacement is taken out - two hands: their mean (pullR - pullL
    // stays), one hand: its forward / back part (its side offset = the neck-rein steer stays); never the height (the one-hand slow's
    // 'raised' keeps its rest). .reins.common 0 = the full re-zero. With .lat2 > 0 the two hands'
    // common SIDE offset is the neck-rein steer, so only their common forward / back part goes (the full mean wipes a held neck-rein
    // turn on every snap and reads the hands' return to the centre as a LEFT turn). A common side
    // offset whose lat2 term stays inside the dead zone is drift, not a turn: it still goes. The stroke's own sideways
    // wobble above that is not taken out: it is transient (the hands come back), a re-zero at the accept would turn it into a reverse
    // steer once they do.
    void rezeroAfter(const std::array<float,3>& zf,const std::array<float,3>& zr) noexcept {
        const int n=int(h_[0].held)+int(h_[1].held);if(!n)return;bump(stats.rezeros);const float before=steerIn(zf,zr);
        if(!t_.common){for(auto& h:h_)if(h.held)h.neutral=h.pos;}
        else if(n==2){float mx=.5f*((h_[0].pos[0]-h_[0].neutral[0])+(h_[1].pos[0]-h_[1].neutral[0])),mz=.5f*((h_[0].pos[2]-h_[0].neutral[2])+(h_[1].pos[2]-h_[1].neutral[2]));
            if(t_.lat2>0.f&&t_.lat2*std::fabs(mx*zr[0]+mz*zr[2])>=t_.dead){const float c=mx*zf[0]+mz*zf[2];mx=zf[0]*c;mz=zf[2]*c;} // only the common fwd / back
            for(auto& h:h_){h.neutral[0]+=mx;h.neutral[2]+=mz;}}
        else for(auto& h:h_)if(h.held){const float c=(h.pos[0]-h.neutral[0])*zf[0]+(h.pos[2]-h.neutral[2])*zf[2];
            if(t_.onePull&&std::fabs(oneSteer(1u,-c))>=t_.dead)continue; // that pull IS the one-hand turn: kept (drift inside the dead zone /
                                                                          // a forward push goes; |steer| is the same for either hand)
            h.neutral[0]+=zf[0]*c;h.neutral[2]+=zf[2]*c;}
        noteRezeroSteer(before,steerIn(zf,zr));
    }
    void publish(const Out& o,int mode) noexcept {
        stats.level.store(gait_,std::memory_order_relaxed);stats.held.store(int(h_[0].held)+int(h_[1].held),std::memory_order_relaxed);
        stats.mode.store(mode,std::memory_order_relaxed);stats.outStick.store(o.stick,std::memory_order_relaxed);stats.outA.store(o.a,std::memory_order_relaxed);
        stats.outX.store(o.x,std::memory_order_relaxed);stats.outY.store(o.y,std::memory_order_relaxed);
        stats.manual.store(manual_,std::memory_order_relaxed);stats.slowing.store(slowing_,std::memory_order_relaxed);
        stats.outAUp.store(o.aUp,std::memory_order_relaxed);stats.aPhase.store(int(aPh_),std::memory_order_relaxed);
        stats.aHold.store(aHold_,std::memory_order_relaxed);stats.aWant.store(prevWant_,std::memory_order_relaxed);
    }
    // ---- not live: FOOT = a dismount, reset at once; MOUNTED layout = suspend; a short gap = hold ----
    Out inactive(const In& in,std::uint64_t now) noexcept {
        if(lastActive_&&(in.onFoot||!t_.holdNs)){
            if(in.onFoot){bump(stats.dismountResets);events.push(now,EvDismount,float(heldBits()),float(gait_));}
            resetAll(now,heldBits()||gait_>0);lastActive_=0;}
        if(lastActive_){
            // .relidle: a hand opened during a short hold / a suspension lets go
            // there as well; the timer runs on across the gap
            if(t_.relIdle&&in.gripLive){unsigned gone=0;
                for(unsigned i=0;i<2;++i){auto& h=h_[i];if(!h.held)continue;const float g=in.grip[i];Ev why=EvNone;
                    if(g<relGrip()){if(!h.openNs)h.openNs=now;if(now-h.openNs>=relNs())why=EvRelOpen;}else h.openNs=0;
                    // the socket rule here too; a hand let go here re-grabs on a squeeze held at the resume (idleRel)
                    if(why==EvNone&&socketOpen(h,g,t_.holster&&(holsterTarget(i,now)&1u),now))why=EvRelSocket;
                    if(why!=EvNone){release(i,why,now,true);h.pressed=false;h.idleRel=true;gone|=1u<<i;bump(stats.idleReleases);}}
                if(gone){zoneMask.fetch_and(~gone,std::memory_order_relaxed);heldMask.fetch_and(~gone,std::memory_order_relaxed);}}
            const bool shortGap=now>=lastActive_&&now-lastActive_<=t_.holdNs;
            const std::uint64_t ref=t_.suspend?std::max(lastActive_,layoutNs_):lastActive_; // the layout left MOUNTED .hold ago at the latest
            if((t_.suspend&&in.layoutMounted)||(now>=ref&&now-ref<=t_.holdNs)){
                if(!holding_){holding_=true;bump(stats.inactivityHolds);events.push(now,EvHold,float(heldBits()),float(gait_));}
                hapRun(now);Out o{};
                // the last output (stick + an in-flight spur tap) is repeated only for .hold and only into the MOUNTED / GAMEPLAY (no
                // player read, pad not ready) layout: never into the stock pad (menu / Unknown: UI navigation, A = confirm), never for a
                // long suspension (optic, camera override: the player's own stick)
                // the A hold (and an in-flight spur) continues through a short gap; never the one-update clear (aUp)
                if(shortGap&&(in.layoutMounted||!in.stockPad)){o=last_;o.aUp=false;o.a=aStep(now,prevWant_)&&o.stick;}
                else{aPh_=AIdle;bump(stats.suspendFrames);}
                aOut_=o.a;
                if(!shortGap){const auto hb=heldBits();zoneMask.store(hb,std::memory_order_relaxed);heldMask.store(hb,std::memory_order_relaxed);
                    activeNow.store(false,std::memory_order_relaxed);} // a long suspension: only the held reins stay masked
                publish(o,o.stick?lastMode_:ModeNone);return o;
            }
            resetAll(now,heldBits()||gait_>0);lastActive_=0;
        }
        clearMasks();publish({},ModeNone);return {};
    }
    // ---- snap detector (one controller sample): the down-stroke = the 350 ms before the bottom ----
    void ringPush(std::uint64_t t,float y,float b) noexcept {ring_[ringAt_]={t,y,b};ringAt_=(ringAt_+1)%unsigned(ring_.size());ringN_=std::min<unsigned>(ringN_+1,unsigned(ring_.size()));}
    const S& ringAt(unsigned k) const noexcept {return ring_[(ringAt_+unsigned(ring_.size())-ringN_+k)%unsigned(ring_.size())];} // k = 0 oldest
    void stroke(float& top,float& peak,float& topB,std::uint64_t& topT) const noexcept { // top = highest point <= 350 ms before the bottom
        top=minY_;topB=minB_;topT=minT_;peak=0.f;                                          // (topT: its sample time); peak = downward m/s over >= 25 ms
        for(unsigned k=0;k<ringN_;++k){const auto& s=ringAt(k);if(s.t>minT_)break;if(minT_-s.t<=350*ms&&s.y>=top){top=s.y;topT=s.t;topB=s.b;}}
        for(unsigned k=0;k<ringN_;++k){const auto& a=ringAt(k);if(a.t<topT)continue;if(a.t>=minT_)break;
            for(unsigned j=k+1;j<ringN_;++j){const auto& b=ringAt(j);if(b.t>minT_)break;
                if(b.t>=a.t+25*ms){peak=std::fmax(peak,(a.y-b.y)/(float(b.t-a.t)*1e-9f));break;}}} // 25 ms baseline: tracking noise at 72+ Hz
    }
public:
    int level() const noexcept {return gait_;}
    Out step(const In& in) noexcept {
        bump(stats.updates);if((propTick_++&255u)==0)readProps();
        const std::uint64_t now=in.now;stats.nowNs.store(now,std::memory_order_relaxed);
        const bool uRise=in.userA&&!prevUserA_,uFall=!in.userA&&prevUserA_;prevUserA_=in.userA; // the player's own A edges
        if(!enabled()||in.menu){ // off / a menu: everything resets at once
            if(lastActive_){resetAll(now,heldBits()||gait_>0);lastActive_=0;}
            clearMasks();publish({},ModeNone);return {};
        }
        if(in.layoutMounted){layoutNs_=now;layoutMountedNs.store(now,std::memory_order_relaxed);}
        if(!(in.mounted&&in.haveHead))return inactive(in,now);
        // ---- (re)activation: grips seeded - a squeeze already down (latched by the holsters on foot / in a menu, its grip
        // bit unmasked) is never a fresh rise, no arm survives a reset / forced release; a resume after a long suspension re-zeroes ----
        const bool fresh=!lastActive_,resumed=!fresh&&holding_&&(now<lastActive_||now-lastActive_>t_.holdNs);
        if(fresh||resumed){ // never for a hand .relidle let go in this suspension - its squeeze held now is a fresh press
            for(unsigned i=0;i<2;++i){auto& h=h_[i];if(!h.idleRel)h.pressed=in.grip[i]>=.55f||(h.held&&in.grip[i]>=.30f);h.armUntil=0;}
            frameValid_=false;space_=in.space;epoch_=in.epoch;continuity_=in.continuity;
            if(resumed){rebase(now,RzResume);bump(stats.resumeRezeros);}
        }
        bool spaceChanged=false; // an OS recenter / new LOCAL epoch / continuity break moved every LOCAL coordinate
        if(in.space!=space_||in.epoch!=epoch_||in.continuity!=continuity_){
            space_=in.space;epoch_=in.epoch;continuity_=in.continuity;
            if(t_.space){rebase(now,RzSpace);bump(stats.spaceRezeros);spaceChanged=true;}}
        holding_=false;lastActive_=now;activeNow.store(true,std::memory_order_relaxed);bump(stats.activeFrames);for(auto& h:h_)h.idleRel=false;
        const float dt=stepNs_&&now>stepNs_?std::fmin(float(now-stepNs_)*1e-9f,.1f):0.f;stepNs_=now;
        const std::uint64_t ts=in.sampleNs?in.sampleNs:now;
        // ---- the real horse yaw rate (trace / sign check) from the riding sample's world heading (+ = left), wrap-safe, >= 50 ms
        // baseline; a gap > 500 ms restarts it, an implausible step (> 720 deg/s: a mount / teleport) is dropped. The pair is
        // read stamp (acquire, pairs with the riding sample's release) - heading (acquire) - stamp again; a stamp that moved = a newer
        // sample landed in between (heading k+1 with stamp k = one frame of turn on a 50 ms baseline): skipped, the next update takes it.
        // Residual: the writer stores heading then stamp, so a heading read in the few ns before its own stamp is visible to the second
        // load still pairs with the old stamp (closing that needs a packed / sequence-stamped writer: ridingAnchorSample) ----
        {const auto hns=horseHeadingNs.load(std::memory_order_acquire);
            if(hns&&hns!=hdgNs_){const float h=horseHeadingWorld.load(std::memory_order_acquire);
                if(horseHeadingNs.load(std::memory_order_relaxed)!=hns)bump(stats.rateTorn);
                else{stats.headingNs.store(hns,std::memory_order_relaxed);
                    if(!std::isfinite(h)){}
                    else if(!hdgNs_||hns<hdgNs_||hns-hdgNs_>500*ms){hdg_=h;hdgNs_=hns;}
                    else if(hns-hdgNs_>=50*ms){const float r=std::remainder(h-hdg_,6.2831853f)/(float(hns-hdgNs_)*1e-9f);
                        if(std::fabs(r)<12.566f){rate_=r;rateNs_=hns;bump(stats.rateSamples);}else bump(stats.rateRejected);hdg_=h;hdgNs_=hns;}}}}
        const bool rateOk=rateNs_&&stampFresh(rateNs_,now,300*ms);const float rateDeg=rateOk?rate_*57.29578f:0.f;
        stats.rateValid.store(rateOk,std::memory_order_relaxed);stats.rateDegS.store(rateDeg,std::memory_order_relaxed);
        // ---- frame: the horse yaw in LOCAL (yaw left +) ----
        if(in.headFwd[0]*in.headFwd[0]+in.headFwd[2]*in.headFwd[2]>.04f)headYaw_=yawOf(in.headFwd);
        float yaw=headYaw_;int src=0;
        if(stampFresh(horseMinusHeadNs.load(std::memory_order_relaxed),now,500*ms)){
            yaw=std::remainder(headYaw_+horseMinusHead.load(std::memory_order_relaxed),6.2831853f);src=2;}
        else if(grabYawValid_){if(spaceChanged)grabYaw_=headYaw_;yaw=grabYaw_;src=1;}
        if(spaceChanged&&grabYawValid_&&src==2)grabYaw_=yaw;
        stats.frameSrc.store(src,std::memory_order_relaxed);
        // (a glance turns the shoulders and both held hands with them = a lat2 + direct-rein steer): the head yaw relative to
        // the horse frame, + = looking left (0 in the HEAD frame) - logged only (trace / report)
        const float headRelDeg=std::remainder(headYaw_-yaw,6.2831853f)*57.29578f;stats.headRelDeg.store(headRelDeg,std::memory_order_relaxed);
        // ---- frame step: a VR snap turn / recenter step rotates the horse frame inside LOCAL. The LOCAL neutrals stay right
        // for a hand that does not move; a rider who turns body + hands back onto the reins moves both - that motion settles (re-zero at
        // rest) instead of reading as a pull difference. A step = > 10 deg in one update with the head turning < 1 deg (no sample skew).
        if(frameValid_&&t_.step&&(h_[0].held||h_[1].held)){
            const float dy=std::remainder(yaw-prevYaw_,6.2831853f),dh=std::remainder(headYaw_-prevHeadYaw_,6.2831853f);
            if(std::fabs(dy)>.1745f&&std::fabs(dh)<.0175f){bump(stats.frameSteps);events.push(now,EvStep,dy*57.29578f,float(src));
                for(auto& h:h_)if(h.held&&h.posValid){h.settleRef=h.pos;h.settleStartBy=now+800*ms;h.settleUntil=now+1500*ms;}
                ringN_=0;haveMin_=false;ignoreUntil_=now+250*ms;}
        }
        prevYaw_=yaw;prevHeadYaw_=headYaw_;frameValid_=true;
        const auto zf=fwdOf(yaw),zr=rightOf(yaw);
        // ---- per hand: pose, grip (on .55 / off .30), zone, grab / release ----
        const unsigned busyBits=(in.busy[0]?1u:0u)|(in.busy[1]?2u:0u);
        std::uint32_t zone=0;
        for(unsigned i=0;i<2;++i){
            auto& h=h_[i];const float g=in.grip[i];
            const bool was=h.pressed;h.pressed=g>=.55f||(was&&g>=.30f);
            const bool rise=h.pressed&&!was;
            const unsigned ht=t_.holster?holsterTarget(i,now):0u; // bit 0 socket, bit 1 foregrip / transfer / catch / reload
            bool inZone=false;float f=0,r=0,d=0;
            if(in.valid[i]){
                // .relfreeze: a held hand that is opening (grip < .relgrip, the .relms release timer running) keeps the pose
                // it had when it opened - its way to a holster / the lap does not steer, lift or turn for the release delay
                const bool frz=t_.relFreeze&&h.held&&h.openNs&&h.posValid;if(frz)bump(stats.freezeFrames);
                if(!frz&&h.posValid&&ts>h.posNs){const float sdt=float(ts-h.posNs)*1e-9f;
                    if(sdt>.002f&&sdt<.2f){const auto dp=sub(in.pos[i],h.pos);const float sp=std::sqrt(dot(dp,dp))/sdt;h.speed+=(sp-h.speed)*(1.f-std::exp(-sdt/.1f));
                        h.vy+=(std::fabs(dp[1])/sdt-h.vy)*(1.f-std::exp(-sdt/.05f));}} // the held-lift stillness (50 ms)
                if(!frz&&(!h.posValid||ts!=h.posNs)){h.pos=in.pos[i];h.posNs=ts;}h.posValid=true;h.badNs=0;
                if(h.held&&h.zeroPending){h.neutral=h.pos;h.zeroPending=false;}
                if(h.speed<.03f){if(!h.stillNs)h.stillNs=now;}else h.stillNs=0;
                const auto rel=sub(in.pos[i],in.head);f=dot(rel,zf);r=dot(rel,zr);d=-rel[1];
                inZone=!in.busy[i]&&f>=.15f&&f<=1.6f&&d>=-.10f&&d<=1.2f&&std::fabs(r)<=.7f; // in front of the body, up to the horse's head
                if(inZone&&(ht&2u)){inZone=false;if(rise)bump(stats.holsterBlocked);} // the barrel / a transfer: the holsters' press
            }else if(h.held&&!h.badNs)h.badNs=now;
            // a squeeze just before the zone still grabs (300 ms) - never one that began in a socket outside the zone (a hip draw)
            if(rise){if((ht&1u)&&!inZone){h.armUntil=0;bump(stats.armBlocked);}else h.armUntil=now+300*ms;}else if(!h.pressed)h.armUntil=0;
            if(rise){stats.pressRel[i][0].store(f,std::memory_order_relaxed);stats.pressRel[i][1].store(d,std::memory_order_relaxed);
                stats.pressRel[i][2].store(r,std::memory_order_relaxed);stats.pressInZone[i].store(inZone,std::memory_order_relaxed);if(!inZone)bump(stats.pressOutside);}
            if(inZone)zone|=1u<<i;
            if(h.held){
                // STICKY: a hand holding the reins lets go ONLY when the grip opens (< .relgrip for .relms; a relaxed hold dips under higher values)
                // or at a dismount / menu reset. The socket rule (.relout) is off by default (0: a relaxed
                // grip at the hip holster would drop the reins and the re-squeeze draw the revolver / the lasso), a tracking loss keeps the
                // reins (.relinvalid 1 = a 300 ms drop) and a 'busy' hand (the weapon / lasso brain owns it) must stay so 300 ms
                if(in.busy[i]){if(!h.busyNs)h.busyNs=now;}else h.busyNs=0;
                if(h.busyNs&&now-h.busyNs>=300*ms){h.busyNs=0;release(i,EvRelBusy,now);}
                // the grip is a button, not a pose: an opened hand lets go with an invalid pose too (hands raised out of the tracking)
                else if(!in.valid[i]){ // the last valid pose decides 'raised' (both raised out of the tracking -> let go)
                    if(h.pos[1]-h.neutral[1]>=.20f)h.openNs=0;
                    else if(g<relGrip()){if(!h.openNs)h.openNs=now;if(now-h.openNs>=relNs())release(i,EvRelOpen,now);}else h.openNs=0;
                    if(h.held&&t_.relInvalid&&now-h.badNs>300*ms)release(i,EvRelInvalid,now);}
                else{bool gone=false; // .relopen; + the socket rule (.relout)
                    // the grip relaxes while the arms go up: a hand raised >= 20 cm over its neutral lets go
                    // only after .relhighms (800 ms) open; lowered it lets go after .relms
                    // a RAISED hand (>= 20 cm over its neutral) never lets go by an open grip - the stop gesture opens the fingers; it lets go
                    // once it is lowered (the open timer restarts there)
                    // a hand drawn back >= 10 cm counts as
                    // in the stop / turn gesture too - no open-grip release there either
                    const bool high=in.pos[i][1]-h.neutral[1]>=.20f||-dot(sub(in.pos[i],h.neutral),zf)>=.10f;
                    // ... and a raised hand held open .25 s IS the stop: HALT + the hard stop, the reins stay in the hand
                    if(high){h.openNs=0;
                        if(g<relGrip()){if(!h.openHighNs)h.openHighNs=now;
                            if(t_.dropStop&&gait_>0&&now-h.openHighNs>=250*ms){const bool fast=gait_>=2;gait_=0;stopped_=true;aPh_=AIdle;aHold_=false;looseSince_=0;
                                slowing_=false;if(t_.hardStop&&fast)hardStopUntil.store(now+t_.hardStopNs,std::memory_order_relaxed);
                                if(fast&&t_.brakePullNs)brakePullUntil.store(now+t_.brakePullNs,std::memory_order_relaxed);
                                lastStopNs_=now;bump(stats.dropStops);bump(stats.halts);events.push(now,EvHalt,2.f,(in.pos[i][1]-h.neutral[1])*100.f);
                                hap(now,heldBits(),.6f,60'000);}}
                        else h.openHighNs=0;}
                    else if(g<relGrip()){if(!h.openNs)h.openNs=now;if(now-h.openNs>=relNs()){release(i,EvRelOpen,now);gone=true;}}else h.openNs=0;
                    if(!gone&&socketOpen(h,g,(ht&1u)!=0u,now))release(i,EvRelSocket,now);}
            }
            if(!h.held&&h.pressed&&g>=.55f&&inZone&&now<=h.armUntil){
                const bool first=!h_[0].held&&!h_[1].held;
                if(first){grabYaw_=yaw;grabYawValid_=true;} // the frame yaw in use at the grab (horse if fresh, else head)
                h.held=true;h.armUntil=0;h.openNs=h.badNs=h.outNs=0;h.neutral=in.pos[i];h.zeroPending=false;h.stillNs=0;bump(stats.grabs);
                // reach - grab - settle back to rest: a hand that leaves the grab point within 350 ms and comes to rest
                // within 1 s re-zeroes there (else an uneven settle is a constant steer, a settle back a permanent slow)
                if(t_.settle){h.settleRef=in.pos[i];h.settleStartBy=now+350*ms;h.settleUntil=now+1000*ms;}else h.settleUntil=0;
                // the second hand re-zeroes the first too (it moved since its own grab: a constant direct-rein steer otherwise).
                // with .lat2 a one-hand neck rein (its side offset, not one the inward rule zeroed) is kept - it stays a
                // (.lat2 / 2 weighted) turn the same way instead of becoming a reverse turn when that hand returns to its grab point
                if(!first&&h_[i^1u].posValid&&t_.onePull){auto& o=h_[i^1u]; // the first hand's pull turn (beyond the dead
                    // zone) stays the same direct-rein turn (pullR - pullL with the new hand at 0); its side offset (no one-hand steer) goes
                    const float oPull=-dot(sub(o.pos,o.neutral),zf),st=oneSteer(i^1u,oPull),sg=(i^1u)==1u?1.f:-1.f,
                        keep=t_.common&&std::fabs(st)>=t_.dead?sg*st:0.f; // the pull that steered (never a forward push)
                    o.neutral[1]=o.pos[1];zeroKeepPull(o,zf,keep);bump(stats.rezeros);noteRezeroSteer(st,sg*keep);}
                else if(!first&&h_[i^1u].posValid){auto& o=h_[i^1u];
                    const auto od=sub(o.pos,o.neutral);const float oPull=-dot(od,zf),oLat=dot(od,zr);
                    const bool inwardZeroed=t_.inward&&oPull>std::fabs(oLat)&&((i^1u)==0u?oLat>0.f:oLat<0.f);
                    const float keep=t_.common&&t_.lat2>0.f&&!inwardZeroed?oLat:0.f;
                    o.neutral[1]=o.pos[1];zeroKeepSide(o,zr,keep);bump(stats.rezeros);noteRezeroSteer(oLat,keep);} // one-hand terms
                events.push(now,EvGrab,float(i),f*100.f);hap(now,1u<<i,.5f,30'000,haptics::SourceGrab);
            }
        }
        for(unsigned i=0;i<2;++i)if(h_[i].held)zone|=1u<<i; // a held hand keeps its grip masked even if it left the zone
        const unsigned held2=heldBits();const int held=int(h_[0].held)+int(h_[1].held);
        zoneMask.store(zone,std::memory_order_relaxed);heldMask.store(held2,std::memory_order_relaxed);
        if(!held)grabYawValid_=false;
        if(held2!=prevHeld_||busyBits!=prevBusy_){ // grab / release / busy change: restart the detector, 250 ms settle
            if(prevHeld_&&!held2){looseSince_=now;hap(now,prevHeld_,.2f,20'000);events.push(now,EvLoose,float(gait_),0.f);}
            if(held2)looseSince_=0;
            // one of two hands let go - the other one's side offset (never used by the direct rein) would become a neck-rein
            // steer: it re-zeroes horizontally where it is. With .lat2 the two hands' COMMON side offset is a neck-rein
            // turn; zeroing it reads the remaining hand's return to the centre as a full opposite lock (both 8 cm right, let the left go to
            // draw: back to the centre = a LEFT turn). It keeps that common part, at most its own side offset and only the same way; its
            // own excess (the other hand's share of a direct rein / a cross-over) still goes. The common part = the mean of both hands'
            // side offsets of the last two-hand update (the released hand's pose may already be on its way to a holster).
            if(t_.handoff&&prevHeld_==3u&&(held2==1u||held2==2u)){const unsigned ki=held2==1u?0u:1u;auto& k=h_[ki];
                // .onepull: the remaining hand now steers by its own pull (+ R / - L): it keeps the part of that pull that carries
                // on the two-hand direct-rein turn (pullR - pullL of the last two-hand update), at most its own and only the same way
                // (R drawn 8 for a right turn, L let go: still right; both drawn 10 alike: no turn), its side offset goes
                if(k.posValid&&t_.onePull){const float kPull=-dot(sub(k.pos,k.neutral),zf),sg=ki==1u?1.f:-1.f,own=oneSteer(ki,kPull),
                        d=pullPrev_[1]-pullPrev_[0],keepS=own*d>0.f?std::copysign(std::fmin(std::fabs(own),std::fabs(d)),own):0.f;
                    zeroKeepPull(k,zf,sg*keepS);noteRezeroSteer(d+t_.lat2*.5f*(latPrev_[0]+latPrev_[1]),keepS);}
                else if(k.posValid){const float kLat=dot(sub(k.pos,k.neutral),zr),mean=.5f*(latPrev_[0]+latPrev_[1]);
                    const float keep=t_.lat2>0.f&&mean*kLat>0.f?std::copysign(std::fmin(std::fabs(kLat),std::fabs(mean)),kLat):0.f;
                    zeroKeepSide(k,zr,keep);noteRezeroSteer(kLat,keep);}
                bump(stats.handoffRezeros);bump(stats.rezeros);events.push(now,EvRezero,float(RzHandoff),float(held2));}
            prevHeld_=held2;prevBusy_=busyBits;ringN_=0;haveMin_=false;letPeak_=0.f;letCalmNs_=0;ignoreUntil_=now+250*ms;
        }
        // ---- settle: off the grab / step point by >= 3 cm and at rest (< 8 cm/s) = the new neutral; on the way = settling ----
        bool settling=false;
        for(unsigned i=0;i<2;++i){auto& h=h_[i];if(!h.held||!h.settleUntil)continue;
            if(now>=h.settleUntil||!in.valid[i]){h.settleUntil=0;continue;}
            const auto dd=sub(h.pos,h.settleRef);
            if(dot(dd,dd)<.0009f){if(now>=h.settleStartBy)h.settleUntil=0;continue;} // it never left: the grab point is the rest
            if(h.speed<.08f){h.neutral=h.pos;h.settleUntil=0;bump(stats.settleRezeros);bump(stats.rezeros);events.push(now,EvRezero,float(RzSettle),float(i));}
            else settling=true;
        }
        stats.settling.store(settling,std::memory_order_relaxed);
        // ---- pull (back toward the rider) / side / rise of each held hand from its neutral, horse frame ----
        float pull[2]{},lat[2]{},up[2]{};
        for(unsigned i=0;i<2;++i)if(h_[i].held){const auto dd=sub(h_[i].pos,h_[i].neutral);pull[i]=-dot(dd,zf);lat[i]=dot(dd,zr);up[i]=dd[1];}
        stats.pullL.store(pull[0],std::memory_order_relaxed);stats.pullR.store(pull[1],std::memory_order_relaxed);
        stats.latL.store(lat[0],std::memory_order_relaxed);stats.latR.store(lat[1],std::memory_order_relaxed);latPrev_[0]=lat[0];latPrev_[1]=lat[1];
        pullPrev_[0]=pull[0];pullPrev_[1]=pull[1];upPrev_[0]=up[0];upPrev_[1]=up[1];stats.upL.store(up[0],std::memory_order_relaxed);stats.upR.store(up[1],std::memory_order_relaxed);
        const unsigned one=h_[0].held?0u:1u;
        // ---- mode: the game's horse-relative stick, else camera-relative compensation, else nothing. The riding code
        // stamps horseRelNs only after a good read; a 'false' with no new stamp is its identity / read failure while +0x638 stays forced
        // (horse-relative): kept until the stamp is 500 ms old ----
        const auto relNs=horseRelNs.load(std::memory_order_relaxed);const bool relAct=horseRelActive.load(std::memory_order_relaxed);
        if(relAct&&relNs)relTrueNs_=relNs;
        const bool relKept=!relAct&&t_.relKeep&&relTrueNs_&&relNs==relTrueNs_;
        int mode=ModeNone;
        if((relAct||relKept)&&stampFresh(relNs,now,500*ms)){mode=ModeHorseRel;if(relKept)bump(stats.relKept);}
        else if(t_.camRel&&stampFresh(horseMinusCamNs.load(std::memory_order_relaxed),now,500*ms))mode=ModeCamRel;
        // ---- slow / stop: both back >= .brake 250 ms (one hand: back + raised 6 cm) -> TROT+ to WALK, WALK to HALT, every 600 ms.
        // never armed at HALT (there is nothing to slow) or by a hand
        // still settling; .reins.slowfast 0 = one gait per step (CANTER / GALLOP -> TROT changes nothing the game sees) ----
        // .lift (a rider stops by LIFTING both hands 15-35 cm, often arcing forward - the horizontal 'back' is ~0 or
        // negative): both up >= .brakeup (15 cm) over their neutral height and alike (|upR - upL| < max(.alike 6 cm, .alikepct 30% of the
        // larger)) / one held hand up >= .liftone (18 cm), held .liftms (300 ms); no pull back. A one-rein lift is no stop.
        bool pose;
        // .level: the two held hands' WORLD heights within
        // .level (8 cm) instead of the neutral-relative .alike rule (0 = .alike alone); one hand higher than the other is a turn, never a stop
        const bool levelOk=held==2&&((t_.level>0.f&&std::fabs(h_[1].pos[1]-h_[0].pos[1])<=t_.level)||
            std::fabs(up[1]-up[0])<std::fmax(t_.alike,t_.alikePct*std::fmax(up[0],up[1])));
        // .symstop (a big turn at GALLOP must not read as a lift stop):
        // two hands stop only when they also sit alike horizontally (|pullR - pullL| < 6 cm, |latR - latL| < 10 cm
        // over the grab) - one rein drawn out / back is a turn, never a stop
        const bool symOk=!t_.symStop||held!=2||std::fabs(twoSteer(pull,lat))<t_.symCm;
        // report: why a two-hand lift (one hand >= .brakeup) was no stop pose (frames): too low (the other hand), not level, a turn
        if(t_.lift&&held==2&&std::fmax(up[0],up[1])>=t_.brakeUp){if(std::fmin(up[0],up[1])<t_.brakeUp)bump(stats.stopLow);
            else if(!levelOk)bump(stats.stopLevel);else if(!symOk)bump(stats.stopSym);}
        if(t_.lift)pose=held==2?std::fmin(up[0],up[1])>=t_.brakeUp&&levelOk&&symOk:
            held==1&&up[one]>=t_.liftOne;
        else pose=held==2?std::fmin(pull[0],pull[1])>=t_.brake&&std::fmin(up[0],up[1])>=t_.brakeUp25:held==1&&pull[one]>=t_.brake&&up[one]>=t_.brakeUp25;
        // the stop is a HELD lift. A go-snap's wind-up passes through the same pose (tops 46 cm over the rest, 0.5-1 s
        // apart) but never rests there: the pose arms only once it lasted .liftms (300 ms) AND
        // the held hands were vertically still (|vy| < .liftv 0.5 m/s, 50 ms) for .liftstill (200 ms) of it (0 = .liftms alone;
        // vertical only: this rider's stop lifts arc forward). Eligible at TROT+ / WALK (the ladder) and, at HALT after a slow step since
        // the last accepted snap (stopped_), as the held stop that keeps the lowering window open (below). .lift 0 = 250 ms.
        bool still=true;
        if(t_.lift&&t_.liftStillNs){for(unsigned i=0;i<2;++i)if(h_[i].held&&h_[i].vy>=t_.liftV)still=false;}
        stats.vyL.store(h_[0].vy,std::memory_order_relaxed);stats.vyR.store(h_[1].vy,std::memory_order_relaxed);
        const bool elig=t_.slow&&held&&pose&&!(settling&&!liftEp_)&&(gait_>0||(t_.lift&&t_.lowSnap&&stopped_));
        if(elig){
            if(!liftEp_){liftEp_=true;liftArmed_=false;liftStillS_=0.f;liftStart_=now;}else if(still)liftStillS_+=dt;
            const std::uint64_t poseNs=now>=liftStart_?now-liftStart_:0; // the timer runs from the pose's first update
            // with .holdstop two hands arm only by the HOLD STOP below
            if(!liftArmed_&&!(t_.holdStop&&held==2)&&poseNs>=(t_.lift?t_.liftNs:250*ms)&&(!t_.lift||!t_.liftStillNs||liftStillS_>=float(t_.liftStillNs)*1e-9f)){
                liftArmed_=true;bump(stats.liftArms);if(!pendHalt_)slowNext_=now;}
        }else{if(liftEp_&&!liftArmed_&&t_.lift&&now>=liftStart_&&now-liftStart_>=t_.liftNs)bump(stats.liftWindups);liftEp_=liftArmed_=false;}
        { // HOLD STOP (Tuning): raised long enough = armed, whatever the stillness / level / symmetry rules above said
            const bool hi=held==2&&.5f*(up[0]+up[1])>=t_.stopUp&&std::fmin(up[0],up[1])>=t_.stopMin;
            if(hi&&!settling){if(!hiSince_)hiSince_=now;}else hiSince_=0;
            const bool pulled=hi&&.5f*(pull[0]+pull[1])>=t_.stopPull;
            if(pulled&&!settling){if(!hiPullSince_)hiPullSince_=now;}else hiPullSince_=0;
            const bool fast=hiPullSince_&&now-hiPullSince_>=t_.stopFastNs;
            if(t_.holdStop&&hiSince_&&gait_>0&&(fast||now-hiSince_>=t_.stopHoldNs)&&!liftArmed_){if(fast)bump(stats.fastStops);
                liftEp_=true;liftArmed_=true;if(!liftStart_||liftStart_>now)liftStart_=now;bump(stats.liftArms);bump(stats.holdStops);if(!pendHalt_)slowNext_=now;}}
        // the slow condition (.lift: once armed; .lift 0: from the pose's start - it blocks snaps / steer / leaks)
        const bool cond=liftEp_&&gait_>0&&(liftArmed_||!t_.lift);
        if(cond)bump(stats.liftPoseFrames);
        // a direct-rein turn pulls BOTH hands back too (9 slows seen at
        // 22-36 cm common pull), so a stop needs both hands back >= .brake AND raised >= .reins.brakeup (8 / 8 cm)
        if(cond){if(!slowCond_)slowCond_=true;}
        else if(slowCond_){slowCond_=false;if(slowing_){slowing_=false;slowEnd_=now;rezero_=true;
            // .liftfull (a 0.4 s lift at GALLOP = SLOW then HALT): a lift stop that took a step finishes the ladder to
            // HALT, one step per .slowms, unless a snap is accepted first
            if(t_.lift&&t_.liftFull&&gait_>0){pendHalt_=true;bump(stats.liftFulls);}}}
        if(gait_>0&&((cond&&liftArmed_)||pendHalt_)&&now>=slowNext_){
            if(cond&&!slowing_){slowing_=true;bump(stats.brakes);}
            slowNext_=now+t_.slowStepNs;aPh_=AIdle;aHold_=false; // a slow step lets A go (the stock hold ends: the pace drops)
            const int from=gait_;gait_=t_.lift&&t_.stopDirect&&cond?0:t_.slowFast&&gait_>=2?1:gait_-1;if(!gait_)lastStopNs_=now;
            if(gait_==0&&from>=2&&t_.brakeBack)brakeBackUntil_=now+t_.brakeBackNs; // .brakeback
            if(gait_==0&&from>=2&&t_.hardStop)hardStopUntil.store(now+t_.hardStopNs,std::memory_order_relaxed); // the game's hard brake
            if(gait_==0&&from>=2&&t_.brakePullNs)brakePullUntil.store(now+t_.brakePullNs,std::memory_order_relaxed); // the pad brake
            bump(stats.slowSteps);
            lowRef_=stepTs_=ts;stopped_=true;rejLiftN_=0; // the window in the stroke clock (sample time)
            events.push(now,EvSlow,float(from*10+gait_),!cond?0.f:(t_.lift?(held==2?std::fmin(up[0],up[1]):up[one]):held==2?std::fmin(pull[0],pull[1]):pull[one])*100.f);
            if(gait_==0){bump(stats.halts);events.push(now,EvHalt,0.f,0.f);hap(now,held2,.6f,60'000);hap(now+120*ms,held2,.6f,60'000);}
            else hap(now,held2,.5f,50'000);
        }
        if(gait_==0)pendHalt_=false;
        // (the stop held at HALT, then lowered / a stop lift repeated at HALT): at HALT after a slow
        // step a held (armed) lift keeps the lowering window open - lowRef_ follows it, so its lowering is no snap whenever it comes; a
        // go-snap from raised hands is no held lift (never arms) and these extra rejLifts are capped (.lowmax 3: no deadlock)
        if(t_.lift&&liftArmed_&&gait_==0&&stopped_){lowRef_=ts;bump(stats.haltLiftFrames);}
        // the stop pose held at HALT since the last snap from rest (the height leak waits .hleakhold; a rider who keeps the reins up
        // after a stop and snaps from there adds up over the stop cycles - the leak learns that posture)
        if(gait_==0&&stopped_&&pose)haltPoseS_+=dt;
        const bool slowBlock=slowCond_||slowing_||(slowEnd_&&now>=slowEnd_&&now-slowEnd_<300*ms);
        if(rezero_&&!slowCond_){ // after a slow: re-zero (common mode) once the hands came to rest (or 500 ms)
            bool calm=true;for(unsigned i=0;i<2;++i)if(h_[i].held&&h_[i].speed>.08f)calm=false;
            if(calm||now-slowEnd_>500*ms){rezeroAfter(zf,zr);rezero_=false;}
        }
        // ---- steer: direct rein + both hands' side (two hands) / neck rein (one hand); s > 0 = RIGHT.
        // dead .dead 3 cm, engaged down to .dead - 0.5 cm, full lock .steer 10 cm, .smooth 60 ms.
        // Two hands also count .lat2 x their mean side displacement - moving both hands right
        // (neck-reining) would read 0 or even LEFT otherwise (the left hand crossing toward the chest comes back more than the right one); a
        // one-rein pull back and a rotation of both hands to the right (right hand back, left hand forward, both right) all agree ----
        float raw=0.f,dv=0.f;
        if(held&&!slowBlock&&!settling){
            // .onepull: one held hand = its own pull, + right / - left
            // .relsteer: the side term is the hands' SPREAD (R - L sideways over the grab) signed by the side that moved (the midpoint shift) - both
            // hands carried together (a body lean / a torso turn / a stale common offset) is 0, hands brought together is 0, R out = right,
            // L out = left. pullR - pullL already is the R-vs-L back difference.
            dv=held==2?twoSteer(pull,lat):t_.onePull?oneSteer(one,pull[one]):lat[one];
            // a one-hand pull back AND inward must not be a neck-rein turn the other way: one hand pulled back more
            // than it moved sideways never counts the inward part (its outward part still turns toward that hand)
            if(held==1&&!t_.onePull&&t_.inward&&pull[one]>std::fabs(dv)&&(one==0u?dv>0.f:dv<0.f)){dv=0.f;bump(stats.inwardFrames);}
            const float dz=t_.dead,lo=std::fmax(dz-.005f,.003f),ad=std::fabs(dv);
            if(!engaged_&&ad>=dz)engaged_=true;else if(engaged_&&ad<lo)engaged_=false;
            if(engaged_)raw=std::copysign(std::clamp((ad-lo)/std::fmax(t_.steer-lo,.01f),0.f,1.f),dv);
            // .letgo: a turn eased back to <= 60% of its peak (peak >= 3 cm)
            // with the held hands calm (< 8 cm/s) for 250 ms re-zeroes the hands where they are - the horse keeps the heading it has, no
            // leftover offset pulls it round. A turn HELD at its peak keeps turning.
            // on the 100 ms mean of |turn| and only for a real ease-off (>= 3 cm off
            // the peak) - a held turn that sags a little (or the still leak bleeding it) is not re-zeroed to straight
            // and only after the hands MOVED back (the mean falling >= 5 cm/s once since the peak; the 10 s still leak bleeds far slower)
            if(t_.letGo){const float av0=letAvg_;letAvg_+=(ad-letAvg_)*(1.f-std::exp(-dt/.1f));const float av=letAvg_;
                if(av>letPeak_){letPeak_=av;letEased_=false;}else if(dt>0.f&&(av0-av)/dt>=.05f)letEased_=true;
                bool calm=true;for(unsigned i=0;i<2;++i)if(h_[i].held&&h_[i].speed>.08f)calm=false;
                if(letPeak_>=.04f&&letEased_&&av<=.6f*letPeak_&&letPeak_-av>=.03f&&calm){if(!letCalmNs_)letCalmNs_=now;
                    if(now-letCalmNs_>=250*ms){for(unsigned i=0;i<2;++i)if(h_[i].held){h_[i].neutral[0]=h_[i].pos[0];h_[i].neutral[2]=h_[i].pos[2];}
                        events.push(now,EvRezero,float(RzLetGo),letPeak_*100.f);bump(stats.rezeros);letPeak_=letAvg_=0.f;letCalmNs_=0;raw=0.f;engaged_=false;}}
                else letCalmNs_=0;}
        }else{engaged_=false;letCalmNs_=0;}
        if(!held)letPeak_=letAvg_=0.f;
        steerS_+=(raw-steerS_)*(t_.smooth>.001f?1.f-std::exp(-dt/t_.smooth):1.f);if(!held)steerS_=0.f; // loose reins: steer 0
        const float s=std::fabs(steerS_)<.01f?0.f:steerS_;
        stats.steer.store(s,std::memory_order_relaxed);stats.steerInCm.store(dv*100.f,std::memory_order_relaxed);
        // ---- neutral leak (horizontal): 3 s while the rein is quiet; 10 s on a hand held still > 2.5 s whatever the steer ----
        {const bool quiet=t_.leak&&held&&!slowCond_&&std::fabs(s)<.15f;bool any=false,still=false;
            for(unsigned i=0;i<2;++i){auto& h=h_[i];if(!h.held||!in.valid[i]||h.settleUntil)continue;
                float tau=0.f;
                if(quiet&&h.speed<.05f)tau=3.f;
                // a neutral 6.5 cm off is a constant steer, and the quiet leak needs |s| < .15 so it never runs: a hand held still for .stillms (2500 ms) re-learns its
                // neutral with .stilltau (10 s) WHATEVER the steer - let go of the pull and the horse runs straight where it points
                else if(t_.stillLeak&&!slowCond_&&h.stillNs&&now>=h.stillNs&&now-h.stillNs>=t_.stillNs){tau=t_.stillTau;still=true;}
                if(tau<=0.f)continue;
                const float k=1.f-std::exp(-dt/tau);h.neutral[0]+=(h.pos[0]-h.neutral[0])*k;h.neutral[2]+=(h.pos[2]-h.neutral[2])*k;any=true;}
            if(any)bump(stats.leakFrames);if(still)bump(stats.stillLeakFrames);}
        // ---- .hleak: the neutral HEIGHT follows a held hand that is still (< 5 cm/s) with tau 4 s while no stop pose is held (the
        // pose at a gait / its ladder): reins carried higher than at the grab are no permanent stop pose. DOWN only with
        // .hleakdn (30 s: a lap rest must not drag the neutral down, or back at the riding height is a lift);
        // at HALT after a stop the held stop pose blocks it for .hleakhold (5 s of pose; longer = a new carried-high posture) ----
        {const bool block=slowCond_||(pose&&gait_>0)||(t_.lift&&pose&&gait_==0&&stopped_&&haltPoseS_<float(t_.hLeakHoldNs)*1e-9f);
            if(t_.hLeak>0.f&&held&&!block){bool any=false;
                for(unsigned i=0;i<2;++i){auto& h=h_[i];if(!h.held||!in.valid[i]||h.settleUntil||h.speed>=.05f)continue;
                    const float d=h.pos[1]-h.neutral[1],tau=d>=0.f?t_.hLeak:t_.hLeakDn;if(tau<=0.f)continue;
                    h.neutral[1]+=d*(1.f-std::exp(-dt/tau));any=true;}
                if(any)bump(stats.hLeakFrames);}}
        // ---- manual Touch stick: gait 0, no output until the next snap ----
        const bool userStick=std::hypot(in.userX,in.userY)>.3f;
        if(userStick){bump(stats.userOverride);
            if(t_.manual){if(!manual_){manual_=true;events.push(now,EvManual,float(gait_),0.f);}gait_=0;aPh_=AIdle;aHold_=false;spot_=false;spotArmNs_=0;
                stopped_=pendHalt_=false;}}
        // ---- snap: each new controller sample once, mean height (+ back coordinate) of the held hands ----
        if(held&&in.sequence&&in.sequence!=lastSeq_){
            lastSeq_=in.sequence;
            bool ok=true;float y=0,b=0;for(unsigned i=0;i<2;++i)if(h_[i].held){ok=ok&&in.valid[i];y+=in.pos[i][1];b-=dot(in.pos[i],zf);}y/=float(held);b/=float(held);
            if(!ok||now<ignoreUntil_){ringN_=0;haveMin_=false;}
            else if(!ringN_||ts>ringAt(ringN_-1).t){
                if(ringN_&&ts-ringAt(ringN_-1).t>150*ms){ringN_=0;haveMin_=false;} // a sample gap: restart
                ringPush(ts,y,b);
                if(t_.whip){ // WHIP: within .whipms (700 ms) the held hands' mean height rose >= .whipup (4 cm) from a
                    // low to a peak and then fell >= .whipdown (4 cm), both at an average >= .whipspeed (0.35 m/s). A slow lift, a held stop
                    // lift and the lowering after it never match (too slow / no quick rise first).
                    int ip=-1;float py=-1e9f;
                    for(int k=int(ringN_)-1;k>=0;--k){const auto& e=ringAt(unsigned(k));if(ts-e.t>t_.whipNs)break;if(e.y>py){py=e.y;ip=k;}}
                    const float fall=ip>=0?py-y:0.f;
                    if(ip>=0&&fall>=t_.whipDown){const auto& pk=ringAt(unsigned(ip));int il=-1;float ly=1e9f;
                        for(int k=ip-1;k>=0;--k){const auto& e=ringAt(unsigned(k));if(ts-e.t>t_.whipNs)break;if(e.y<ly){ly=e.y;il=k;}}
                        const float rise=il>=0?py-ly:0.f;
                        if(il>=0&&rise>=t_.whipUp){const auto& lo=ringAt(unsigned(il));
                            const float vu=rise/std::fmax(float(pk.t-lo.t)*1e-9f,.02f),vd=fall/std::fmax(float(ts-pk.t)*1e-9f,.02f);
                            bump(stats.downStarts);stats.lastDrop.store(fall,std::memory_order_relaxed);stats.lastPeak.store(std::fmin(vu,vd),std::memory_order_relaxed);
                            Ev rej=EvNone;
                            if(vu<t_.whipSpeed||vd<t_.whipSpeed)rej=EvRejWeak;
                            else if(slowBlock)rej=EvRejSlowing;
                            else if(lastStopNs_&&now>=lastStopNs_&&now-lastStopNs_<1'000'000'000ull)rej=EvRejSlowing; // lowering after a stop is no whip
                            else if(lastSnapT_&&ts>=lastSnapT_&&ts-lastSnapT_<300*ms)rej=EvRejRefractory; // 300 ms (whips can be 445 ms apart)
                            if(rej!=EvNone){bump(rej==EvRejWeak?stats.rejectedSlow:rej==EvRejSlowing?stats.rejSlowing:stats.rejRefractory);
                                events.push(now,rej,rise*100.f,std::fmin(vu,vd));}
                            else{bump(stats.flicks);lastSnapT_=ts;manual_=false;gait_=std::min(gait_+1,4);looseSince_=0;
                                stopped_=pendHalt_=liftEp_=liftArmed_=false;haltPoseS_=0.f;rezeroAfter(zf,zr);
                                hardStopUntil.store(0,std::memory_order_relaxed);brakePullUntil.store(0,std::memory_order_relaxed);events.push(now,EvSnap,rise*100.f,std::fmin(vu,vd));
                                hap(now,held2,.8f,25'000);for(int k=1;k<=gait_;++k)hap(now+std::uint64_t(k)*90*ms,held2,.3f,15'000);
                                if(gait_>=3)requestSpur(now,in.userA,mode);}
                            ringN_=0;ringPush(ts,y,b);}}
                }else
                if(!haveMin_||y<minY_){minY_=y;minB_=b;minT_=ts;haveMin_=true;}
                else{
                    const float riseNow=y-minY_;const bool late=ts-minT_>350*ms;
                    if(riseNow>=std::fmin(.02f,.4f*t_.flick)||late){
                        float top=0,peak=0,topB=0;std::uint64_t topT=0;stroke(top,peak,topB,topT);const float drop=top-minY_,back=minB_-topB;
                        const bool upOk=riseNow>=std::fmin(.02f,.4f*drop);
                        if(drop>=t_.flick&&!upOk&&!late){} // a candidate: wait for the rest of the up-stroke (bottom kept)
                        else if(drop>=t_.flick){
                            bump(stats.downStarts);stats.lastDrop.store(drop,std::memory_order_relaxed);stats.lastPeak.store(peak,std::memory_order_relaxed);
                            if(drop>stats.maxDrop.load(std::memory_order_relaxed))stats.maxDrop.store(drop,std::memory_order_relaxed);
                            Ev rej=EvNone;
                            if(peak<t_.snapSpeed)rej=EvRejWeak;else if(!upOk)rej=EvRejNoUp;
                            else if(t_.yank&&back>.6f*drop)rej=EvRejYank; // back-and-down = a check, not a go
                            else if(slowBlock)rej=EvRejSlowing;
                            else if(lastStopNs_&&now>=lastStopNs_&&now-lastStopNs_<1'000'000'000ull)rej=EvRejSlowing; // lowering after a stop is no whip
                            // .lowsnap: a stroke whose top (its sample time, the stroke's START) lies within .lowms
                            // (1.5 s) of the last slow step and > .lowcm (8 cm) over the held hands' neutral height is the hands coming
                            // down from the stop lift, not a go. A stop lift held at HALT extends the window (lowRef_); those
                            // extended rejections are capped at .lowmax (3) per slow step - a go from raised hands is never held off longer
                            float ny=0.f;for(unsigned i=0;i<2;++i)if(h_[i].held)ny+=h_[i].neutral[1];const float topOver=top-ny/float(held);
                            bool lowWin=false;
                            if(t_.lowSnap&&lowRef_&&topT<lowRef_+t_.lowNs&&topOver>t_.lowCm){
                                const bool plain=stepTs_&&topT<stepTs_+t_.lowNs; // the base window: uncapped
                                lowWin=plain||!t_.lowMax||rejLiftN_<t_.lowMax;
                                if(lowWin&&!plain&&rej==EvNone)++rejLiftN_;else if(!lowWin&&rej==EvNone)bump(stats.rejLiftCapped);}
                            if(rej==EvNone&&lowWin){rej=EvRejLift;stats.lastLiftTop.store(topOver,std::memory_order_relaxed);}
                            else if(rej==EvNone&&lastSnapT_&&ts>=lastSnapT_&&ts-lastSnapT_<450*ms)rej=EvRejRefractory;
                            if(rej!=EvNone){bump(rej==EvRejWeak?stats.rejectedSlow:rej==EvRejNoUp?stats.rejNoUp:rej==EvRejYank?stats.rejYank:
                                    rej==EvRejSlowing?stats.rejSlowing:rej==EvRejLift?stats.rejLift:stats.rejRefractory);
                                events.push(now,rej,rej==EvRejLift?topOver*100.f:drop*100.f,rej==EvRejYank?back*100.f:
                                    rej==EvRejLift?float((double(topT)-double(lowRef_))*1e-6):peak);}
                            else{ // ---- accepted snap: gait + 1 (max 4), spur at >= 3, re-zero (common mode), haptics ----
                                bump(stats.flicks);lastSnapT_=ts;manual_=false;gait_=std::min(gait_+1,4);looseSince_=0;
                                stopped_=pendHalt_=liftEp_=liftArmed_=false; // a go ends the stop (and a ladder still completing)
                                if(topOver<(held==2?t_.brakeUp:t_.liftOne))haltPoseS_=0.f; // a snap from below the lift pose: a fresh posture
                                rezeroAfter(zf,zr);
                                hardStopUntil.store(0,std::memory_order_relaxed);brakePullUntil.store(0,std::memory_order_relaxed);events.push(now,EvSnap,drop*100.f,peak);
                                hap(now,held2,.8f,25'000);for(int k=1;k<=gait_;++k)hap(now+std::uint64_t(k)*90*ms,held2,.3f,15'000);
                                if(gait_>=3)requestSpur(now,in.userA,mode);
                            }
                            ringN_=0;ringPush(ts,y,b);minY_=y;minB_=b;minT_=ts; // the next stroke starts here
                        }else{ // no stroke: restart the bottom (a fast but short one is counted once)
                            if(drop>=.6f*t_.flick&&upOk&&peak>=t_.snapSpeed){bump(stats.rejShallow);events.push(now,EvRejShallow,drop*100.f,peak);ringN_=0;ringPush(ts,y,b);}
                            minY_=y;minB_=b;minT_=ts;
                        }
                    }
                }
            }
        }
        // ---- loose reins: the gait holds .loose (3 s), then WALK, after 2x HALT ----
        if(!held&&gait_>0&&looseSince_&&t_.looseNs&&now>=looseSince_){const auto age=now-looseSince_;
            if(age>=2*t_.looseNs){gait_=0;bump(stats.looseDrops);events.push(now,EvLoose,0.f,float(age/ms));}
            else if(age>=t_.looseNs&&gait_>1){gait_=1;bump(stats.looseDrops);events.push(now,EvLoose,1.f,float(age/ms));}}
        // ---- output ----
        Out out{};
        const bool braking=brakeBackUntil_&&now<brakeBackUntil_&&gait_==0;if(!braking)brakeBackUntil_=0;
        if((held||gait_>0||braking)&&!manual_&&!userStick){
            float vx=0,vy=0;
            if(gait_==0){ // HALT: (0,0), or the standing turn (yaw 90 deg > the sim's 75 deg on-spot threshold)
                // (seen: 12 standing turns alternating +.57 / -.59 / +.55 / -.56 after a HALT - the rider settling the
                // reins spins the horse back and forth): only after |s| > .spotin (0.45) held for .spotms (200 ms); exit < .35
                const float as=std::fabs(s);
                if(held&&t_.spot&&!slowBlock){
                    if(!spot_){ // .spotlift: never armed while the lift pose is held (the uneven forward arcs of a stop lift
                                // read as a full direct-rein lock)
                        if(as>t_.spotIn&&!(t_.lift&&t_.spotLift&&pose)){if(!spotArmNs_)spotArmNs_=now;
                            if(now-spotArmNs_>=t_.spotNs){spot_=true;bump(stats.standingTurns);events.push(now,EvTurn,s,float((now-spotArmNs_)/ms));spotArmNs_=0;}}
                        else{if(spotArmNs_)bump(stats.spotShort);spotArmNs_=0;}
                    }else if(as<.35f)spot_=false;
                }else{spot_=false;spotArmNs_=0;}
                if(spot_){vx=s>0?1.f:-1.f;vy=0.f;}
            }else if(t_.xy){ // x = the turn, y = the gait's throttle, independent (|v| <= 1.41; the sim keeps the direction)
                spot_=false;spotArmNs_=0;vx=std::clamp(s*t_.gain,-1.f,1.f);vy=gait_==1?t_.walk:1.f;
                // (1, 1) caps a TROT+ turn at 45 deg: the last 20% of the lock trades
                // throttle for yaw up to .yawmax (55 deg: y 1 -> cot 55 = .70 at full lock; a polar full lock gives cos 55 = .57),
                // so the mid range keeps y = 1. .yawmax 45 = plain (1, 1)
                if(gait_>=2){const float ax=std::fabs(vx),ymin=1.f/std::tan(t_.yawMax);if(ax>.8f&&ymin<1.f)vy=1.f-(1.f-ymin)*(ax-.8f)*5.f;}
            }else{ // .xy 0: polar output
                spot_=false;spotArmNs_=0;const float a=s*t_.yaw;vx=std::sin(a);
                if(gait_==1){vy=t_.walk;const float m=std::hypot(vx,vy);if(m>.8f){vx/=m;vy/=m;}} // asin law below 0.8, atan2 above
                else vy=std::cos(a);
                const float m=std::hypot(vx,vy);if(m>1.f){vx/=m;vy/=m;} // |v| <= 1
            }
            vy=std::fmax(vy,0.f); // never back (= a turn-around)
            if(braking&&mode==ModeHorseRel){vx=0.f;vy=-1.f;bump(stats.brakeBackFrames);} // the reins pulled - only horse-relative (back = back)
            if(mode==ModeHorseRel){out.stick=true;out.x=vx;out.y=vy;bump(stats.horseRelFrames);}
            else if(mode==ModeCamRel){ // game stick heading h = atan2(-x, y) (left +); the game adds the control-vector heading
                const float m=std::hypot(vx,vy),hh=std::atan2(-vx,vy)+horseMinusCam.load(std::memory_order_relaxed);
                float rx=-std::sin(hh)*m,ry=std::cos(hh)*m;const float k=std::fmax(1.f,std::fmax(std::fabs(rx),std::fabs(ry)));rx/=k;ry/=k; // |v| up to
                out.stick=true;out.x=rx;out.y=ry;bump(stats.rotated);} // 1.41 - scaled into the pad square, the direction kept (no per-axis clip)
            else bump(stats.noneFrames);
            if(s!=0.f)bump(stats.steerFrames);
        }else{spot_=false;spotArmNs_=0;}
        { // BRAKE PULL: the pad brake (camera-relative while GameXr holds +0x638 = 0), raw (0, -1); a spin ends it
            bool pulling=brakePullActive(now)&&gait_==0&&held&&!manual_&&!userStick;
            if(pulling&&std::fabs(stats.rateDegS.load(std::memory_order_relaxed))>90.f){brakePullUntil.store(0,std::memory_order_relaxed);pulling=false;bump(brakePullSpinEnds);}
            if(pulling){bump(brakePullFrames);out.stick=true;out.x=0.f;out.y=-1.f;}}
        // ---- A: held from the first CANTER+ spur while a rein is held and the brain drives the stick; the player's A merges ----
        if(gait_<3)aHold_=false; // a slow step / HALT / the loose-rein WALK / manual / reset
        const bool want=aHold_&&t_.holdA>0&&t_.spur&&held&&out.stick;
        if(prevWant_&&!want){bump(stats.aReleases); // the manual stick first (it clears aHold_ / gait_ itself: logged 2 before)
            events.push(now,EvARelease,float((manual_||userStick)?4:!held?3:!aHold_?(gait_==0?2:1):5),float(gait_));}
        if(uFall&&want&&t_.holdA==1&&aPh_==AIdle){aPh_=AGap2;aUntil_=now+70*ms;bump(stats.aUserFollow);} // the release half of their tap
        out.a=aStep(now,want)&&out.stick;
        if(uRise&&aOut_){out.a=false;out.aUp=true;bump(stats.aUserEdges);} // their press over the brain's A: one update up = their edge
        prevWant_=want;aOut_=out.a;
        // ---- sign check (frames the stick carries a steer: moving or the standing turn) + the 10 Hz trace ----
        if(out.stick&&(gait_>0||spot_)){
            if(s>0.f)bump(stats.turnFramesRight);else if(s<0.f)bump(stats.turnFramesLeft);
            if(rateOk){if(rateDeg>10.f)bump(stats.horseTurnLeft);else if(rateDeg<-10.f)bump(stats.horseTurnRight);
                if(std::fabs(s)>.3f&&std::fabs(rateDeg)>10.f)bump((s>0.f)==(rateDeg<0.f)?stats.signAgree:stats.signDisagree);} // right = rate < 0
        }
        if(!traceNs_||now<traceNs_||now-traceNs_>=100*ms){traceNs_=now;
            const std::uint32_t ti=traceN.load(std::memory_order_relaxed);auto& row=trace[ti&63u];row.t.store(now/ms,std::memory_order_relaxed);
            const float tv[16]{float(gait_),s,pull[0]*100.f,pull[1]*100.f,lat[0]*100.f,lat[1]*100.f,out.x,out.y,
                float((out.a?1:0)|(in.userA?2:0)|(out.aUp?4:0)),rateOk?rateDeg:std::nanf(""),headRelDeg,up[0]*100.f,up[1]*100.f,
                in.grip[0],in.grip[1],float(held2)};
            for(unsigned k=0;k<16;++k)row.v[k].store(tv[k],std::memory_order_relaxed);
            traceN.store(ti+1,std::memory_order_release);
            // wrong-way detector: |s| > .5 against the horse's own yaw (> 15 deg/s the other way) for 600 ms
            if(rateOk&&std::fabs(s)>.5f&&((s>0.f&&rateDeg>15.f)||(s<0.f&&rateDeg<-15.f))){if(!wrongSince_)wrongSince_=now;
                if(now-wrongSince_>=600*ms&&(!lastWrongCap_||now-lastWrongCap_>5'000*ms)){lastWrongCap_=now;captureAt(now,2);}}else wrongSince_=0;
            if(capPendNs_&&now>=capPendNs_){const unsigned c=captureN.load(std::memory_order_relaxed);auto& cp=captures[c&3u];cp.at=capPendAt_;cp.kind=capPendKind_;
                const std::uint32_t tn=traceN.load(std::memory_order_relaxed);unsigned n=0;
                for(std::uint32_t k=tn>64u?tn-64u:0u;k<tn;++k){const auto& r=trace[k&63u];cp.rows[n][0]=float(r.t.load(std::memory_order_relaxed)%100000000ull);
                    for(unsigned q=0;q<16;++q)cp.rows[n][q+1]=r.v[q].load(std::memory_order_relaxed);++n;}
                cp.n=n;captureN.store(c+1,std::memory_order_release);capPendNs_=0;}}
        // ---- steering haptics: tension on the pulled / rein hand every 120 ms, a tick at full lock ----
        if(held&&!slowBlock){const float as=std::fabs(s);const unsigned hand=held==2?(s>0?1u:0u):one;
            if(as>.15f&&(!buzzNs_||now<buzzNs_||now-buzzNs_>=120*ms)){buzzNs_=now;hap(now,1u<<hand,.08f+.25f*as,30'000);}
            if(!lock_&&as>=.98f){lock_=true;hap(now,1u<<hand,.5f,20'000);}else if(lock_&&as<.9f)lock_=false;}
        hapRun(now);
        last_=out;lastMode_=mode;publish(out,mode);
        return out;
    }
};
inline Brain brain{};

inline std::string report() {
    auto u=[](const std::atomic<std::uint64_t>& c){return (unsigned long long)c.load(std::memory_order_relaxed);};
    auto f=[](const std::atomic<float>& x){return double(x.load(std::memory_order_relaxed));};
    const auto nowNs=stats.nowNs.load(std::memory_order_relaxed);
    auto age=[nowNs](const std::atomic<std::uint64_t>& stamp){const auto t=stamp.load(std::memory_order_relaxed);return t&&nowNs>=t?(long long)((nowNs-t)/1'000'000ull):-1ll;};
    std::string out;out.reserve(12288);char b[3072];
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"killed\":%s,\"updates\":%llu,\"activeFrames\":%llu,\"held\":%d,\"zone\":%u,\"level\":%d,"
        "\"grabs\":%llu,\"releases\":%llu,\"flicks\":%llu,\"downStarts\":%llu,\"rejectedSlow\":%llu,\"lastDropCm\":%.1f,\"spurs\":%llu,\"brakes\":%llu,"
        "\"steer\":%.2f,\"pullCm\":[%.1f,%.1f],\"steerFrames\":%llu,\"userOverride\":%llu,\"rotated\":%llu,\"horseMinusHeadDeg\":%.1f,"
        "\"release\":{\"open\":%llu,\"socket\":%llu,\"busy\":%llu,\"invalid\":%llu,\"reset\":%llu},\"pressOutside\":%llu,\"maxDropCm\":%.1f,"
        "\"pressCm\":[{\"fwd\":%.0f,\"down\":%.0f,\"right\":%.0f,\"zone\":%s},{\"fwd\":%.0f,\"down\":%.0f,\"right\":%.0f,\"zone\":%s}],",
        menuOn.load()?"true":"false",killed.load()?"true":"false",u(stats.updates),u(stats.activeFrames),
        stats.held.load(),zoneMask.load(),stats.level.load(),u(stats.grabs),u(stats.releases),u(stats.flicks),u(stats.downStarts),u(stats.rejectedSlow),
        f(stats.lastDrop)*100.0,u(stats.spurs),u(stats.brakes),f(stats.steer),f(stats.pullL)*100.0,f(stats.pullR)*100.0,u(stats.steerFrames),
        u(stats.userOverride),u(stats.rotated),double(horseMinusHead.load())*57.29578,
        u(stats.relOpen),u(stats.relSocket),u(stats.relBusy),u(stats.relInvalid),u(stats.relReset),u(stats.pressOutside),f(stats.maxDrop)*100.0,
        f(stats.pressRel[0][0])*100.0,f(stats.pressRel[0][1])*100.0,f(stats.pressRel[0][2])*100.0,stats.pressInZone[0].load()?"true":"false",
        f(stats.pressRel[1][0])*100.0,f(stats.pressRel[1][1])*100.0,f(stats.pressRel[1][2])*100.0,stats.pressInZone[1].load()?"true":"false");
    out+=b;
    static constexpr const char* frames[3]{"HEAD","GRAB","HORSE"};
    std::snprintf(b,sizeof b,"\"gait\":\"%s\",\"manual\":%s,\"slowing\":%s,\"latCm\":[%.1f,%.1f],\"frame\":\"%s\",\"lastPeakMps\":%.2f,"
        "\"out\":{\"mode\":\"%s\",\"stick\":%s,\"x\":%.3f,\"y\":%.3f,\"a\":%s},\"modeFrames\":{\"horseRel\":%llu,\"camRel\":%llu,\"none\":%llu},"
        "\"horseRel\":{\"active\":%s,\"ageMs\":%lld},\"horseMinusCamDeg\":%.1f,\"horseMinusCamAgeMs\":%lld,\"horseMinusHeadAgeMs\":%lld,"
        "\"snaps\":{\"accepted\":%llu,\"rejected\":{\"weak\":%llu,\"shallow\":%llu,\"noUp\":%llu,\"refractory\":%llu,\"slowing\":%llu,\"yank\":%llu}},"
        "\"slowSteps\":%llu,\"halts\":%llu,\"spurTaps\":%llu,\"spurSkipped\":{\"userA\":%llu,\"off\":%llu},\"standingTurns\":%llu,"
        "\"inactivityHolds\":%llu,\"forcedReleases\":%llu,\"resets\":%llu,\"looseDrops\":%llu,\"rezeros\":%llu,\"leakFrames\":%llu,",
        gaitName(stats.level.load()),stats.manual.load()?"true":"false",stats.slowing.load()?"true":"false",f(stats.latL)*100.0,f(stats.latR)*100.0,
        frames[std::clamp(stats.frameSrc.load(),0,2)],f(stats.lastPeak),
        modeName(stats.mode.load()),stats.outStick.load()?"true":"false",f(stats.outX),f(stats.outY),stats.outA.load()?"true":"false",
        u(stats.horseRelFrames),u(stats.rotated),u(stats.noneFrames),horseRelActive.load()?"true":"false",age(horseRelNs),
        double(horseMinusCam.load())*57.29578,age(horseMinusCamNs),age(horseMinusHeadNs),
        u(stats.flicks),u(stats.rejectedSlow),u(stats.rejShallow),u(stats.rejNoUp),u(stats.rejRefractory),u(stats.rejSlowing),u(stats.rejYank),
        u(stats.slowSteps),u(stats.halts),u(stats.spurs),u(stats.spurUserA),u(stats.spurOff),u(stats.standingTurns),
        u(stats.inactivityHolds),u(stats.forcedReleases),u(stats.resets),u(stats.looseDrops),u(stats.rezeros),u(stats.leakFrames));
    out+=b;
    // A hold, steering law, standing-turn dwell, steer vs the real horse turn (rate + = left: a RIGHT steer should pair with < 0)
    {static constexpr const char* aPhases[4]{"IDLE","GAP","TAP","GAP2"};
        char rate[24];if(stats.rateValid.load())std::snprintf(rate,sizeof rate,"%.1f",f(stats.rateDegS));else std::strcpy(rate,"null");
        std::snprintf(b,sizeof b,"\"a\":{\"holdMode\":%d,\"hold\":%s,\"want\":%s,\"phase\":\"%s\",\"clearedForUser\":%s,\"holdFrames\":%llu,\"taps\":%llu,"
            "\"userEdges\":%llu,\"userFollow\":%llu,\"releases\":%llu},"
            "\"steerCfg\":{\"xy\":%s,\"gain\":%.2f,\"lat2\":%.2f,\"steerCm\":%.1f,\"deadCm\":%.1f,\"smoothMs\":%.0f,\"spotIn\":%.2f,\"spotMs\":%d,"
            "\"yawMaxDeg\":%.0f,\"looseMs\":%d},"
            "\"steerInCm\":%.1f,\"spotShort\":%llu,\"headRelDeg\":%.1f,\"rezeroSteer\":{\"lastCm\":%.1f,\"maxCm\":%.1f,\"hits\":%llu},"
            "\"turnSign\":{\"steerLeft\":%llu,\"steerRight\":%llu,\"horseLeft\":%llu,\"horseRight\":%llu,\"agree\":%llu,\"disagree\":%llu,"
            "\"rateDegS\":%s,\"headingAgeMs\":%lld,\"rateSamples\":%llu,\"rateRejected\":%llu,\"rateTorn\":%llu},",
            stats.holdA.load(),stats.aHold.load()?"true":"false",stats.aWant.load()?"true":"false",aPhases[std::clamp(stats.aPhase.load(),0,3)],
            stats.outAUp.load()?"true":"false",u(stats.aHoldFrames),u(stats.aTaps),u(stats.aUserEdges),u(stats.aUserFollow),u(stats.aReleases),
            stats.cfgXy.load()?"true":"false",f(stats.cfgGain),f(stats.cfgLat2),f(stats.cfgSteer)*100.0,f(stats.cfgDead)*100.0,f(stats.cfgSmooth)*1000.0,
            f(stats.cfgSpotIn),stats.cfgSpotMs.load(),f(stats.cfgYawMax)*57.29578,stats.cfgLooseMs.load(),
            f(stats.steerInCm),u(stats.spotShort),f(stats.headRelDeg),f(stats.rezeroSteerCm),f(stats.rezeroSteerMaxCm),u(stats.rezeroSteerHits),
            u(stats.turnFramesLeft),u(stats.turnFramesRight),u(stats.horseTurnLeft),u(stats.horseTurnRight),u(stats.signAgree),u(stats.signDisagree),
            rate,age(stats.headingNs),u(stats.rateSamples),u(stats.rateRejected),u(stats.rateTorn));
        out+=b;}
    // release / one-hand pull / lift stop / height leak / lowering levers and counters (+ pushTurn;
    // brakeUpCm = the effective threshold: .brakeup with .lift, the back + up rule's up (8) with .lift 0)
    {const int bits=stats.cfgBits.load();
        std::snprintf(b,sizeof b,"\"rules\":{\"relOpen\":%d,\"relIdle\":%d,\"onePull\":%d,\"lift\":%d,\"lowSnap\":%d,\"pushTurn\":%d,\"liftFull\":%d,"
            "\"relFreeze\":%d,\"spotLift\":%d,\"relGrip\":%.2f,\"relMs\":%d,\"relOut\":%.2f,\"relOutMs\":%d,"
            "\"brakeUpCm\":%.1f,\"liftOneCm\":%.1f,\"liftMs\":%d,\"liftStillMs\":%d,\"liftVCmS\":%.0f,\"alikeCm\":%.1f,\"alikePct\":%.0f,"
            "\"hLeakS\":%.1f,\"hLeakDnS\":%.1f,\"hLeakHoldMs\":%d,\"lowMs\":%d,\"lowCm\":%.1f,\"lowMax\":%d,"
            "\"slowMs\":%d,\"upCm\":[%.1f,%.1f],\"vyCmS\":[%.0f,%.0f],\"idleReleases\":%llu,\"liftPoseFrames\":%llu,\"liftArms\":%llu,"
            "\"liftWindups\":%llu,\"liftFulls\":%llu,\"haltLiftFrames\":%llu,\"rejLift\":%llu,\"rejLiftCapped\":%llu,\"lastLiftTopCm\":%.1f,"
            "\"hLeakFrames\":%llu,\"freezeFrames\":%llu},",
            bits&1,(bits>>1)&1,(bits>>2)&1,(bits>>3)&1,(bits>>4)&1,(bits>>5)&1,(bits>>6)&1,(bits>>7)&1,(bits>>8)&1,
            f(stats.cfgRelGrip),stats.cfgRelMs.load(),f(stats.cfgRelOut),stats.cfgRelOutMs.load(),
            f(stats.cfgBrakeUp)*100.0,f(stats.cfgLiftOne)*100.0,stats.cfgLiftMs.load(),stats.cfgLiftStillMs.load(),f(stats.cfgLiftV)*100.0,
            f(stats.cfgAlike)*100.0,f(stats.cfgAlikePct)*100.0,
            f(stats.cfgHLeak),f(stats.cfgHLeakDn),stats.cfgHLeakHoldMs.load(),stats.cfgLowMs.load(),f(stats.cfgLowCm)*100.0,stats.cfgLowMax.load(),
            stats.cfgSlowMs.load(),f(stats.upL)*100.0,f(stats.upR)*100.0,f(stats.vyL)*100.0,f(stats.vyR)*100.0,
            u(stats.idleReleases),u(stats.liftPoseFrames),u(stats.liftArms),u(stats.liftWindups),u(stats.liftFulls),u(stats.haltLiftFrames),
            u(stats.rejLift),u(stats.rejLiftCapped),f(stats.lastLiftTop)*100.0,u(stats.hLeakFrames),u(stats.freezeFrames));
        out+=b;}
    // counters
    const auto ht0=holsterTargets[0].load(std::memory_order_relaxed),ht1=holsterTargets[1].load(std::memory_order_relaxed);
    std::snprintf(b,sizeof b,"\"rezero\":{\"settle\":%llu,\"space\":%llu,\"resume\":%llu,\"handoff\":%llu},\"settling\":%s,\"frameSteps\":%llu,"
        "\"stillLeakFrames\":%llu,\"inwardFrames\":%llu,\"dismountResets\":%llu,\"suspendFrames\":%llu,\"relKept\":%llu,"
        "\"stopReject\":{\"low\":%llu,\"level\":%llu,\"sym\":%llu,\"arms\":%llu,\"windups\":%llu,\"holdStops\":%llu,\"fastStops\":%llu,\"dropStops\":%llu,\"brakeBackFrames\":%llu,\"brakePullFrames\":%llu,\"brakePullSpinEnds\":%llu,\"hardStopCalls\":%llu,\"hardStopNoHorse\":%llu,\"hardStopBitSeen\":%llu,\"simAd8\":%u,\"simAdc\":%u,\"horseGuid\":%u,\"playerGuid\":%u,\"sim\":%s,\"softDecel\":%.2f,\"hardDecel\":%.2f,\"hardUsed\":%.2f,\"tuneBoosts\":%llu,\"tuneBad\":%llu,\"decayDelay\":%.2f,\"decayTimerSeen\":%.2f,\"decayTimerZeroed\":%llu},\"holster\":{\"blocked\":%llu,\"armBlocked\":%llu,\"target\":[%u,%u],\"first\":%s,\"maskHeld\":%s},"
        "\"haptics\":{\"posted\":%llu,\"dropped\":%llu},\"nowMs\":%llu,\"events\":[",
        u(stats.settleRezeros),u(stats.spaceRezeros),u(stats.resumeRezeros),u(stats.handoffRezeros),stats.settling.load()?"true":"false",u(stats.frameSteps),
        u(stats.stillLeakFrames),u(stats.inwardFrames),u(stats.dismountResets),u(stats.suspendFrames),u(stats.relKept),
        u(stats.stopLow),u(stats.stopLevel),u(stats.stopSym),u(stats.liftArms),u(stats.liftWindups),u(stats.holdStops),u(stats.fastStops),u(stats.dropStops),u(stats.brakeBackFrames),u(brakePullFrames),u(brakePullSpinEnds),u(hardStopCalls),u(hardStopNoHorse),u(hardStopBitSeen),unsigned(hsSimAd8.load()),unsigned(hsSimAdc.load()),unsigned(hsHorseGuid.load()),
        unsigned(hsPlayerGuid.load()),hsSim.load()?"true":"false",hsSoftDecel.load(),hsHardDecel.load(),hsHardUsed.load(),u(hsTuneBoosts),u(hsTuneBad),hsDecayDelay.load(),hsDecayTimerSeen.load(),u(hsDecayTimerZeroed),
        u(stats.holsterBlocked),u(stats.armBlocked),unsigned(ht0&3ull),unsigned(ht1&3ull),holsterFirst.load()?"true":"false",maskHeld.load()?"true":"false",
        u(stats.hapticPosts),u(stats.hapticDrops),(unsigned long long)(nowNs/1'000'000ull));
    out+=b;
    const std::uint32_t n=events.n.load(std::memory_order_acquire);
    for(std::uint32_t i=n>32u?n-32u:0u,first=i;i<n;++i){
        const auto hd=events.head[i&31u].load(std::memory_order_relaxed),vv=events.vals[i&31u].load(std::memory_order_relaxed);
        const std::uint32_t ua=std::uint32_t(vv>>32),ub=std::uint32_t(vv);float a{},bb{};std::memcpy(&a,&ua,4);std::memcpy(&bb,&ub,4);
        const unsigned k=unsigned(hd&0xffu);
        std::snprintf(b,sizeof b,"%s[%llu,\"%s\",%.2f,%.2f]",i==first?"":",",(unsigned long long)(hd>>8),evNames[k<EvCount?k:0],double(a),double(bb));
        out+=b;
    }
    out+="]";
    // steering trace (10 Hz, oldest first)
    out+=",\"trace\":{\"cols\":\"t,gait,s,pullL,pullR,latL,latR,x,y,A(1 brain 2 user 4 cleared),rateDegS(+left),headRelDeg(+left),upL,upR,gripL,gripR,held\",\"rows\":[";
    const std::uint32_t tn=traceN.load(std::memory_order_acquire);
    for(std::uint32_t i=tn>64u?tn-64u:0u,first=i;i<tn;++i){
        const auto& r=trace[i&63u];float v[16];for(unsigned k=0;k<16;++k)v[k]=r.v[k].load(std::memory_order_relaxed);
        char rate[24];if(std::isfinite(v[9]))std::snprintf(rate,sizeof rate,"%.1f",double(v[9]));else std::strcpy(rate,"null");
        std::snprintf(b,sizeof b,"%s[%llu,%.0f,%.2f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.0f,%s,%.0f,%.1f,%.1f,%.2f,%.2f,%.0f]",i==first?"":",",
            (unsigned long long)r.t.load(std::memory_order_relaxed),double(v[0]),double(v[1]),double(v[2]),double(v[3]),double(v[4]),double(v[5]),
            double(v[6]),double(v[7]),double(v[8]),rate,std::isfinite(v[10])?double(v[10]):0.0,double(v[11]),double(v[12]),double(v[13]),
            double(v[14]),double(v[15]));
        out+=b;
    }
    out+="]}";
    { // captures (t = ms mod 1e8, then the trace columns)
        out+=",\"captures\":[";const unsigned cn=captureN.load(std::memory_order_acquire);bool firstC=true;
        for(unsigned c=cn>4u?cn-4u:0u;c<cn;++c){const auto& cp=captures[c&3u];if(!firstC)out+=',';firstC=false;
            std::snprintf(b,sizeof b,"{\"at\":%llu,\"kind\":%d,\"rows\":[",(unsigned long long)cp.at,cp.kind);out+=b;
            for(unsigned k=0;k<cp.n;++k){out+=k?",[":"[";for(unsigned q=0;q<17;++q){std::snprintf(b,sizeof b,q?",%.1f":"%.0f",double(std::isfinite(cp.rows[k][q])?cp.rows[k][q]:0.f));out+=b;}out+=']';}
            out+="]}";}
        out+="]}";}
    return out;
}
} // namespace rdrvr::vr::reins
