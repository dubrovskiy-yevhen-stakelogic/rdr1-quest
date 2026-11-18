#pragma once
// Controls: Touch -> game button layout, stick-click arbitration (sprint / crouch / recenter),
// fast jump.
//
// Engine facts (librdr.so 1.58 arm64, .text file offset = vaddr - 0x4000):
//  - Default bindings of the Android port (rage::GameActionInput, static initializer 0x1ac7018..0x1acb0e4, decoded by
//    emulating every rage::Binding(int,BindingType) / DigitalGameAction ctor call; type 0 = d11 pad button):
//      FOOT group 5: SPRINT = pad 0 (A), JUMP = pad 2 (X), INTERACT = pad 1 (B), CROUCH = pad 18 (L3), COVER = pad 15 (RB),
//      MELEE_BLOCK = pad 15, MELEE_SPECIAL = pad 3 (Y);  group 6: RELOAD = pad 1 (B), DEADEYE = pad 19 (R3), FIRE = 17,
//      TARGET = 16, USE = 3, USE2 = 2, WHISTLE = 10 (D-pad up), LOOK_AT = 1;  camera group 2: LOOK_BACK = pad 19 (R3);
//      HORSE group 7: JUMP = pad 2 (X), SPUR = pad 0 (A);  menus group 19: ACCEPT = 0, CANCEL = 1, ACTION_4 = 19.
//  - DEADEYE (player input 29) is read only as a PRESS edge (value byte 0 vs byte 2 and byte 3, the same test as the sprint press
//    0x26b3a84..0x26b3ab8): sagPlayer::UpdateFoot 0x269dfa8..0x269dfcc and sagPlayer::UpdateTargetingState 0x269e3b0..0x269e3d0 are its only
//    readers. Camera LOOK_BACK (camera input 2) is read as HELD (byte 0 vs byte 2 only) every update by camGlueCMXGame::UpdateInputParams
//    0x2634c20..0x2634c2c (gated by player-derived +0x104 / +0x10c / +0x10d, 0x2634bb4..0x2634bd0). So a held R3 is LOOK_BACK for the whole
//    hold wherever that gate lets it, while Dead Eye needs only the press: the mapper sends Dead Eye presses as a short pulse.
//    Android key -> d11 pad button (AndroidGamepad_c::GamepadButtonEvent 0x198ca7c): 96->0 A, 97->1 B, 99->2 X, 100->3 Y,
//    19->10 D-pad up, 106->18 L3, 107->19 R3, 108->12 Start, 109->13 Select (4 BACK -> 13 too).
//    INVENTORY = pad 13 (Select; decoded binding "INVENTORY = 13/20"): the Touch controllers have no Select, so it is the HOLD
//    of the logical X (whistle) >= Tuning::inventoryHold (bit::Select, Android key 109).
//  - Sprint: sagPlayerInputFoot::UpdateParameters reads input 0x18 (0x26b3a78) - press edges pump the sprint machine
//    foot+0x184 (states 2..4, SprintPumpTimeout foot+0x80), foot+0x18a selects "held" instead of "pressed" (0x26b396c..0x26b3998).
//    A pump train (down 2 updates / up 1 update) drives both variants.
//  - Crouch: IsCrouching = mover+0xb30 & 0xf00 == 0x100 (sagPlayerInputFoot::IsCrouching 0x26b641c).
//  - Jump: action tree -> JumpSequenceAnimTask::Begin (0x22cf194) -> SetupNewJump(ctx, PhaseIn, false) (only caller,
//    PLT 0x2c6ea60 from 0x22cf2b0) plays the GETREADY clip (state 1, track GetreadyTime +0x50) and sequences LAUNCH;
//    Sequencer state 1->2 plays LAUNCH (LaunchTime +0x54); Sequencer state 2 (clip end) broadcasts sagMsgSetVelocity and calls
//    mvrMoverComponent::Jump(JumpHeight +0x48, ArcTime) (0x22cd268) -> mvrMoverInst::StartJump (ballistic vertical velocity,
//    inst+0x82c |= 0x8000 at 0x288c820) -> state 3 ARC. The physical jump starts only after getready + launch clips.
//    SetupNewJump's own bool (w2) = "start at LAUNCH" branch (0x22cca04: state 2, SetJumpSegment(2), launch clip) - never
//    used by the engine. FAST JUMP = GOT hook of SetupNewJump (JUMP_SLOT 0x2e3fb90) that passes true for the local on-foot
//    player and then retimes the launch clip with the engine's own animAnimatorComponent::RetimeAnimPlayer (0x1e19cd8, the
//    call SetupNewJump itself makes with LaunchTime). Landing/fall/arc stay native.
//  - sagActionIntentions::Update (0x1e5eee4) only accumulates held time: no debounce between the pad and the JUMP intention.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include "VrDeadEye.h" // DEAD EYE: press counters (light header, no engine)
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::controls {

// Pad bits: identical to rdrvr::runtime::PadButton (static_assert in the GameXr glue). DpadUp / Select are output-only.
namespace bit {
inline constexpr std::uint32_t A=1u<<0,B=1u<<1,X=1u<<2,Y=1u<<3,Menu=1u<<4,L3=1u<<5,R3=1u<<6,LGrip=1u<<7,RGrip=1u<<8,
    LT=1u<<9,RT=1u<<10,DpadUp=1u<<11,Select=1u<<12;
inline constexpr std::uint32_t mapped=A|B|X|Y|L3|R3|DpadUp|Select; // the only bits the mapper owns
}

inline constexpr std::uint64_t ms(std::uint64_t v) noexcept {return v*1'000'000ull;}

// ======================================= XR thread: stick clicks =======================================
// L3 / R3 single clicks and the recenter chord share the same two buttons:
//  - first click from rest -> PENDING for `window` (150 ms);
//  - the other stick pressed while the first is still PENDING -> CHORD: both consumed (nothing reaches the game) until each
//    is released; both held >= recenterHold (600 ms) after the second press -> one recenter;
//  - released inside the window -> a click pulse of minPulse (120 ms, >= 2 game updates even at 20 fps);
//  - held past the window -> ACTIVE while held (and >= minPulse).
// invalidate() (focus / action loss) blocks both sides until they are physically released (no repeat
// recenter after a reference-space reset while still held).
struct ClickTiming {std::uint64_t window{ms(150)},recenterHold{ms(600)},minPulse{ms(120)};};
struct ClickResult {std::uint32_t bits{};bool recenter{};};
inline std::atomic<std::uint64_t> clicksL{},clicksR{},chords{},recenters{},chordsAborted{},clickResets{};
class StickClickArbiter {
public:
    enum class Side : std::uint8_t {Up,Pending,Active,Consumed,Blocked};
private:
    struct State {Side st{Side::Blocked};std::uint64_t t0{},until{};};
    std::array<State,2> s_{};std::uint64_t chordStart_{};bool chordFired_{},chordLive_{};std::uint64_t last_{};
public:
    void invalidate() noexcept {for(auto& s:s_){s.st=Side::Blocked;s.t0=s.until=0;}chordStart_=0;chordFired_=chordLive_=false;last_=0;
        clickResets.fetch_add(1,std::memory_order_relaxed);}
    Side side(unsigned i) const noexcept {return s_[i&1].st;}
    ClickResult sample(bool left,bool right,std::uint64_t now,const ClickTiming& t={}) noexcept {
        ClickResult out{};
        if(!now||(last_&&now<last_)){invalidate();return out;}
        last_=now;
        const bool phys[2]{left,right};
        for(unsigned i=0;i<2;++i)if(s_[i].st==Side::Up&&phys[i]){s_[i].st=Side::Pending;s_[i].t0=now;}
        if(s_[0].st==Side::Pending&&s_[1].st==Side::Pending&&phys[0]&&phys[1]){ // second press inside the first one's window
            s_[0].st=s_[1].st=Side::Consumed;chordStart_=now;chordFired_=false;chordLive_=true;chords.fetch_add(1,std::memory_order_relaxed);
        }
        for(unsigned i=0;i<2;++i){
            auto& s=s_[i];
            switch(s.st){
            case Side::Blocked:case Side::Consumed:if(!phys[i])s.st=Side::Up;break;
            case Side::Pending:
                if(!phys[i]||now-s.t0>=t.window){
                    s.st=Side::Active;s.until=now+t.minPulse;(i?clicksR:clicksL).fetch_add(1,std::memory_order_relaxed);
                }
                break;
            case Side::Active:if(!phys[i]&&now>=s.until)s.st=Side::Up;break;
            case Side::Up:break;
            }
        }
        if(chordLive_){
            const bool both=s_[0].st==Side::Consumed&&s_[1].st==Side::Consumed&&phys[0]&&phys[1];
            if(both&&!chordFired_&&now-chordStart_>=t.recenterHold){chordFired_=true;out.recenter=true;recenters.fetch_add(1,std::memory_order_relaxed);}
            if(!both){if(!chordFired_)chordsAborted.fetch_add(1,std::memory_order_relaxed);chordLive_=false;}
        }
        if(s_[0].st==Side::Active)out.bits|=bit::L3;
        if(s_[1].st==Side::Active)out.bits|=bit::R3;
        return out;
    }
};

// ======================================= game input thread: layout mapper =======================================
// Layout of the moment a Touch button is PRESSED; a held button keeps the mapping it was pressed with (a context change can
// never create a new press edge on another game button, e.g. A held from a jump into a menu ACCEPT).
enum class Layout : std::uint8_t {Native,Gameplay,Foot,Mounted,Count};
inline constexpr std::array<const char*,4> layoutNames{"NATIVE","GAMEPLAY","FOOT","MOUNTED"};
// Native   = menus / frontend / unknown presentation: the Touch buttons are the stock pad (incl. R3 = menu ACTION_4).
// Gameplay = world / optic / cutscene / camera override with no readable player context: stock pad, R3 only with LT.
// Foot     = player state 2 (on foot): A jump, B reload (manual reload may consume it), X whistle, Y use,
//            L3 sprint toggle, R3 crouch toggle, LT+R3 Dead Eye (LT reaching the game, MapIn::ltGame); R3 with a firearm in a VR hand /
//            while Dead Eye runs = Dead Eye.
// INVENTORY (every layout, not in a lasso catch / minigame): X held >= inventoryHold = pad Select (d11 13) while it stays held.
//            Foot / Mounted: X's own action (whistle / pad X) is sent as a pulse on RELEASE of a shorter press, so a hold never
//            whistles; Native / Gameplay: X goes down at once (menus) and the hold adds Select.
// Mounted  = any other player state (horse / wagon / vehicle): A = spur/accelerate (pad A, one press per tap,
//            like the stock game), B = jump, the manual reload keeps B while it owns it, L3 nothing, X = X, LT+R3 Dead Eye.
//            A vehicle seat with VR weapons (MapIn::vehicleSeat): B = pad B (@GENERIC.RELOAD); pad X there is @WAGON.SWITCH_SEAT, on X alone.
inline Layout layoutFor(bool menuOrUnknown,bool playerRead,std::uint32_t playerMode) noexcept {
    if(menuOrUnknown)return Layout::Native;
    if(!playerRead)return Layout::Gameplay;
    return playerMode==2?Layout::Foot:Layout::Mounted;
}

// ---- R3 + LT: the LT the game gets ----
// LT + R3 starts Dead Eye only while the game aims, i.e. while LT reaches the game. The holster filter runs AFTER this mapper
// (NativePadDriver: controls, then filter) and keeps LT from the game while the VR hands are empty (VrHolsters.h routeEmpty), so the
// physical LT alone is no aim: R3 then reaches the game without aim, DEADEYE is refused and pad 19 acts as camera LOOK_BACK
// (default bindings above) for as long as R3 is held. The LT bit of the last SUBMITTED pad (noteDelivered, pad observer) tells.
// NativePadDriver::beforeUpdate runs controls -> filter -> observer on every update that reaches submission, all on the native input
// thread, so "the observer ran since the previous mapper sample" (padNotes advanced) means the delivered LT belongs to the previous
// update, however long that update took.
enum class LtGame : std::uint8_t {Physical,Off,On,Pending};
inline constexpr std::array<const char*,4> ltGameNames{"PHYSICAL","OFF","ON","PENDING"};
inline std::atomic<bool> r3DeliveredLt{true}; // debug.rdrvr.vr.r3lt 0 = the physical LT decides LT + R3 (no delivered-LT check)
inline std::atomic<bool> ltDelivered{false};inline std::atomic<std::uint64_t> ltDeliveredNs{0},padNotes{};
// Pad observer (native input thread): the buttons exactly as submitted.
inline void noteDelivered(std::uint64_t now,std::uint32_t buttons,bool ready) noexcept {
    ltDelivered.store(ready&&(buttons&bit::LT)!=0,std::memory_order_relaxed);ltDeliveredNs.store(now?now:1,std::memory_order_release);
    padNotes.fetch_add(1,std::memory_order_relaxed);
}
// Pure. physLtPrev = the physical LT of the previous mapper update (the one the delivered sample was submitted in);
// fresh = the pad observer ran after the previous mapper update (LtTrack: padNotes advanced).
//  - no observer note since the previous mapper update (no observer, a failed submission): Physical;
//  - LT up: Off;  LT went down after the last submitted pad: Pending (that pad cannot tell yet);  else the delivered LT.
inline LtGame ltForDeadEye(bool physLt,bool physLtPrev,bool delivered,bool fresh) noexcept {
    if(!fresh)return LtGame::Physical;
    if(!physLt)return LtGame::Off;
    if(!physLtPrev)return LtGame::Pending;
    return delivered?LtGame::On:LtGame::Off;
}
// Keeps the previous physical LT and the observer count of the previous mapper update (input thread).
struct LtTrack {
    bool prev{};std::uint64_t notesSeen{};
    LtGame sample(bool physLt,bool delivered,std::uint64_t notes) noexcept {
        const bool fresh=notes!=0&&notes!=notesSeen;notesSeen=notes;
        const auto g=ltForDeadEye(physLt,prev,delivered,fresh);prev=physLt;return g;}
};
// The LT rule of this update: the delivered LT only with the lever on and no engine item in a VR hand (holdHand < 0: the holster
// filter's empty-hand route withholds LT); a held item keeps the physical LT (its own LT routes: ltNative, the throwable force aim).
inline LtGame ltGameFor(bool lever,int holdHand,LtGame delivered) noexcept {return lever&&holdHand<0?delivered:LtGame::Physical;}
// LT counts as aim for R3 / the SLOW-MO arm.
inline bool ltAims(LtGame g,bool physLt) noexcept {return g==LtGame::Physical?physLt:g==LtGame::On;}

// R3 route of a press (pure). Order: stock layouts, a running Dead Eye (end it), LT aiming, a gun in a VR hand, LT not known yet
// (Wait: decided on the next update), then crouch on foot / nothing elsewhere; *_LT_WITHHELD = LT was held but never reached the game.
enum class R3Route : std::uint8_t {Native,Lasso,DeadEyeEnd,DeadEyeLt,DeadEyeGun,Crouch,LtWithheldCrouch,Suppressed,LtWithheldSuppressed,Wait,Count};
inline constexpr std::array<const char*,10> r3RouteNames{"NATIVE","LASSO","DEADEYE_END","DEADEYE_LT","DEADEYE_GUN","CROUCH",
    "LT_WITHHELD_CROUCH","SUPPRESSED","LT_WITHHELD_SUPPRESSED","WAIT"};
inline R3Route r3Route(Layout L,bool lassoStock,bool physLt,LtGame g,bool gun,bool active,bool mayWait=true) noexcept {
    if(L==Layout::Native)return R3Route::Native;
    if(lassoStock)return R3Route::Lasso;
    if(active)return R3Route::DeadEyeEnd;
    if(ltAims(g,physLt))return R3Route::DeadEyeLt;
    if(gun)return R3Route::DeadEyeGun;
    if(g==LtGame::Pending&&mayWait)return R3Route::Wait;
    const bool withheld=physLt;
    if(L==Layout::Foot)return withheld?R3Route::LtWithheldCrouch:R3Route::Crouch;
    return withheld?R3Route::LtWithheldSuppressed:R3Route::Suppressed;
}

// vr.controls.r3: one entry per decided R3 press (input thread writes, report reads under r3Seq).
struct R3Event {
    std::uint64_t ns{};std::uint8_t layout{},route{},ltGame{};bool ltPhys{},waited{},gun{},active{};std::int8_t holdHand{-1};
    std::int8_t crouchedAtPress{-1},crouchedAfter{-1};std::int32_t heldMs{-1};
    float psiMaxDevDeg{-1.f},psiEndDevDeg{-1.f},stickMaxX{0.f};std::uint32_t psiSamples{};
    std::int8_t ctxAllowed{-1};std::int16_t aimBits{-1};const char* block{}; // holster gate and engine aim bits at the press
    std::uint8_t pulseUpdates{}; // updates the R3 bit was sent (Dead Eye pulse; held routes count up to 255)
};
inline constexpr std::uint64_t r3CrouchCheckDelay=ms(500);
inline std::array<R3Event,16> r3Ring{};inline unsigned r3Next{};inline std::atomic<std::uint64_t> r3Total{};
inline std::atomic<std::uint32_t> r3Seq{}; // odd while the input thread writes the ring
inline std::atomic<std::uint64_t> ltWithheldR3{},r3Waits{},r3WaitDropped{},lastApplyNs{};
inline void r3WriteBegin() noexcept {r3Seq.fetch_add(1,std::memory_order_acq_rel);}
inline void r3WriteEnd() noexcept {r3Seq.fetch_add(1,std::memory_order_release);}
inline float wrapPi(float a) noexcept {
    constexpr float pi=3.14159265358979f;
    if(!std::isfinite(a))return 0.f;
    a=std::fmod(a+pi,2.f*pi);if(a<0.f)a+=2.f*pi;return a-pi;
}

struct Tuning {
    float moveMin{0.25f};           // stick deflection that counts as "moving" (sprint arm / pump)
    float neutralMax{0.20f};         // below this the stick is neutral
    std::uint64_t neutralHold{ms(200)};  // sprint ends after the stick is neutral this long
    std::uint64_t armWindow{ms(1500)};   // L3 clicked with a neutral stick: sprint waits this long for a deflection
    unsigned pumpDown{2},pumpUp{1};  // sprint A pump train in pad updates
    unsigned crouchPulse{2};         // L3 (CROUCH) pulse length in pad updates
    unsigned deadEyePulse{3};        // R3 (DEADEYE) pulse length in pad updates: the game reads only the press edge of input 29
    std::uint64_t inventoryHold{ms(500)}; // X held this long = INVENTORY (pad Select)
    unsigned tapPulse{3};            // a released short X press: whistle / pad X pulse length in pad updates
};
struct MapIn {
    bool ready{};std::uint32_t buttons{};float stickX{},stickY{};Layout layout{Layout::Native};std::uint64_t now{};
    int jumping{-1}; // mover jump flag: -1 unknown, 0/1
    // TELEPORT: stickOwned = TELEPORT owns the movement stick this update (the mapper sees it
    // neutral: sprint never arms / pumps); teleportWalk = L3 is the escape-walk modifier (no sprint toggle, never forwarded).
    bool stickOwned{},teleportWalk{};
    // LASSO: lassoRemap = a catch is on the rope (CAUGHT): Touch A -> pad A (FOOT and MOUNTED),
    // Touch X -> pad X on foot (whistle off meanwhile) so the HUD prompts match; lassoMinigame = a lasso minigame has the focus: every face
    // button and the stick clicks are the stock pad (prompt codes 4-7 = the labelled button; 9 / 10 = L3 / R3), no sprint, no crouch toggle.
    bool lassoRemap{},lassoMinigame{};int promptCode{32};
    // DEAD EYE (VrDeadEye.h): deadEyeGun = a firearm is in a VR hand (holsters heldFirearm) -> R3 = the game's DEADEYE press
    // (the gun in hand is the aim; LT is the free hand's trigger in VR); deadEyeActive = Dead Eye runs -> R3 = the stock "end it".
    bool deadEyeGun{},deadEyeActive{};
    bool vehicleSeat{}; // the player sits in a vehicle seat where VR weapons work (vehicleseat::weaponSeatNow, player state 3)
    // R3 + LT: the LT the game gets (ltForDeadEye). Physical = the physical LT decides (the default, and the r3lt lever off).
    LtGame ltGame{LtGame::Physical};
    // R3 ring only (never part of a decision): the VR holding hand, the mover's crouch posture (-1 unknown), the world yaw psi
    // (VisibilityCameraBridge::worldYaw, NaN unknown) and its origin serial.
    int holdHand{-1},crouched{-1};float psi{std::numeric_limits<float>::quiet_NaN()};std::uint64_t psiOrigin{};
    // R3 ring only: right stick X (snap / smooth turn input, NaN unknown), the holster context gate (-1 unknown, 0 refused, 1 allowed)
    // with its reason (static string or nullptr), the engine aim bits (player+0x3bc, -1 unknown).
    float lookX{std::numeric_limits<float>::quiet_NaN()};int ctxAllowed{-1},aimBits{-1};const char* blockReason{};
};
enum class Sprint : std::uint8_t {Off,Armed,On};
inline constexpr std::array<const char*,3> sprintNames{"OFF","ARMED","ON"};

// Reload consumer = rdrvr::vr::reload::consumeB(bool pressedEdge) (vr/VrReload.h), linked by the
// GameXr glue only when that header exists (__has_include); this header stays free of the
// holster/weapon headers (ControllerActions includes it on the XR thread). Contract: called on
// EVERY ready pad update in the Foot/Mounted layouts with pressedEdge = B went down this update;
// true = the manual reload owns B -> B is dropped this update, and a press it took stays dropped
// until B is released (no late native RELOAD/INTERACT edge if the ownership ends mid-press).
// Menus / cutscenes: not called, B is stock.
using ReloadConsumer=bool(*)(bool pressedEdge);
inline std::atomic<ReloadConsumer> reloadConsumer{nullptr};

// ---- counters (report) ----
inline std::atomic<std::uint64_t> updateFrame{};         // pad updates seen by the mapper (one per game frame)
inline std::atomic<std::uint64_t> jumpPresses{},sprintToggles{},sprintPumps{},sprintOnFrames{},sprintEndsNeutral{},sprintEndsContext{},
    sprintArmExpired{},crouchPulses{},deadEyePresses{},r3Suppressed{},spurPresses{},whistlePresses{},reloadConsumed{},reloadPassed{},
    reloadErrors{},notReady{},sprintTeleportIgnored{},stickOwnedFrames{},lassoRemapFrames{},lassoMinigameFrames{},lassoRemapPresses{},
    inventoryPresses{},xTaps{};
inline std::array<std::atomic<std::uint64_t>,4> layoutFrames{};
inline std::atomic<bool> whistleRequest{false}; // The holsters' mouth gesture -> one D-pad up (whistle) pulse on foot
inline std::atomic<unsigned> lastLayout{0};inline std::atomic<unsigned> sprintState{0};inline std::atomic<bool> sprintWanted{false};

// ---- jump latency (frames = pad updates; 1 = the very next game update, the minimum possible) ----
struct LatencyStat {
    std::atomic<std::uint64_t> n{},sum{},last{},min{~0ull},max{};
    void add(std::uint64_t v) noexcept {n.fetch_add(1,std::memory_order_relaxed);sum.fetch_add(v,std::memory_order_relaxed);last.store(v,std::memory_order_relaxed);
        auto m=min.load(std::memory_order_relaxed);while(v<m&&!min.compare_exchange_weak(m,v,std::memory_order_relaxed)){}
        m=max.load(std::memory_order_relaxed);while(v>m&&!max.compare_exchange_weak(m,v,std::memory_order_relaxed)){}}
    void json(std::string& out,const char* name) const {
        char b[160];const auto c=n.load();
        std::snprintf(b,sizeof b,"\"%s\":{\"n\":%llu,\"last\":%llu,\"min\":%llu,\"max\":%llu,\"avg\":%.2f}",name,(unsigned long long)c,
            (unsigned long long)last.load(),(unsigned long long)(c?min.load():0),(unsigned long long)max.load(),c?double(sum.load())/double(c):0.0);
        out+=b;}
};
inline LatencyStat pressToBegin,pressToImpulse,beginToImpulse,pressToImpulseMs;
inline std::atomic<std::uint64_t> pressFrame{},pressNs{},beginFrame{};inline std::atomic<bool> waitBegin{},waitImpulse{};
inline std::atomic<std::uint64_t> beginsUnmatched{},impulsesUnmatched{},jumpTimeouts{},impulses{};
inline constexpr std::uint64_t latencyWindowFrames=90;
inline void notePress(std::uint64_t frame,std::uint64_t now) noexcept {
    if(waitImpulse.load(std::memory_order_relaxed))jumpTimeouts.fetch_add(1,std::memory_order_relaxed); // previous press never jumped
    pressFrame.store(frame,std::memory_order_relaxed);pressNs.store(now,std::memory_order_relaxed);beginFrame.store(0,std::memory_order_relaxed);
    waitBegin.store(true,std::memory_order_relaxed);waitImpulse.store(true,std::memory_order_release);
}
// Called from the SetupNewJump hook (engine thread). `frame` = updateFrame now (the press of update f is read by game update f+1).
inline void noteBegin(std::uint64_t frame) noexcept {
    if(!waitBegin.exchange(false,std::memory_order_acq_rel)){beginsUnmatched.fetch_add(1,std::memory_order_relaxed);return;}
    const auto p=pressFrame.load(std::memory_order_relaxed);
    if(frame<p||frame-p>latencyWindowFrames){beginsUnmatched.fetch_add(1,std::memory_order_relaxed);return;}
    pressToBegin.add(frame-p+1);beginFrame.store(frame+1,std::memory_order_relaxed);
}
// Mover jump flag rising edge observed at pad update `frame` (the jump happened in the game update before it).
inline void noteImpulse(std::uint64_t frame,std::uint64_t now) noexcept {
    impulses.fetch_add(1,std::memory_order_relaxed);
    if(!waitImpulse.exchange(false,std::memory_order_acq_rel)){impulsesUnmatched.fetch_add(1,std::memory_order_relaxed);return;}
    waitBegin.store(false,std::memory_order_relaxed);
    const auto p=pressFrame.load(std::memory_order_relaxed);
    if(frame<=p||frame-p>latencyWindowFrames){impulsesUnmatched.fetch_add(1,std::memory_order_relaxed);return;}
    pressToImpulse.add(frame-p);
    const auto b=beginFrame.load(std::memory_order_relaxed);if(b&&frame>=b)beginToImpulse.add(frame-b); // 0 = same game update
    const auto pn=pressNs.load(std::memory_order_relaxed);if(pn&&now>pn)pressToImpulseMs.add((now-pn)/1'000'000ull);
}

class Mapper {
    struct Latch {bool held{};std::uint32_t out{};};
    std::array<Latch,6> latch_{}; // A B X Y L3 R3
    Sprint sprint_{Sprint::Off};std::uint64_t armedUntil_{},neutralSince_{};unsigned pump_{},crouch_{},whistle_{};
    bool lastJumping_{};bool jumpKnown_{};
    // INVENTORY: the X press that may still become a hold (pending), its press time, whether its own action waits for the release
    // (Foot / Mounted) and which bit that release pulses (DpadUp whistle on foot / pad X).
    bool xPending_{},xDeferred_{};std::uint64_t xSince_{};std::uint32_t xTapBit_{},tapBit_{};unsigned tap_{};
    // R3: a press waiting one update for the delivered LT (r3Wait_); the ring entry of the current press (held tracking, 500 ms crouch check)
    bool r3Wait_{};std::uint64_t r3PressNs_{};int r3Slot_{-1},r3CheckSlot_{-1};std::uint64_t r3CheckNs_{};
    bool r3PsiBase_{};float r3Psi0_{};std::uint64_t r3PsiOrigin_{};
    unsigned r3Pulse_{}; // Dead Eye routes: updates left with the R3 bit (0 = the bit follows the held latch)
    void r3Release(std::uint64_t now) noexcept {
        if(r3Slot_>=0){auto& e=r3Ring[unsigned(r3Slot_)];r3WriteBegin();
            e.heldMs=std::int32_t(std::min<std::uint64_t>(now>=e.ns?(now-e.ns)/1'000'000ull:0,0x7fffffffull));r3WriteEnd();}
        r3Slot_=-1;r3PsiBase_=false;
    }
    void r3Record(const MapIn& in,R3Route route,bool waited) noexcept {
        if(r3Slot_>=0)r3Release(in.now);
        const unsigned i=r3Next;r3WriteBegin();
        auto& e=r3Ring[i];e=R3Event{};e.ns=r3PressNs_?r3PressNs_:in.now;e.layout=std::uint8_t(in.layout);e.route=std::uint8_t(route);
        e.ltGame=std::uint8_t(in.ltGame);e.ltPhys=(in.buttons&bit::LT)!=0;e.waited=waited;e.gun=in.deadEyeGun;e.active=in.deadEyeActive;
        e.holdHand=std::int8_t(std::clamp(in.holdHand,-1,1));e.crouchedAtPress=std::int8_t(std::clamp(in.crouched,-1,1));
        e.ctxAllowed=std::int8_t(std::clamp(in.ctxAllowed,-1,1));e.aimBits=std::int16_t(std::clamp(in.aimBits,-1,0x7fff));e.block=in.blockReason;
        r3Next=(i+1u)%unsigned(r3Ring.size());r3WriteEnd();r3Total.fetch_add(1,std::memory_order_relaxed);
        r3Slot_=int(i);r3CheckSlot_=int(i);r3CheckNs_=e.ns+r3CrouchCheckDelay;r3PsiBase_=false;
    }
    // Every ready update: the yaw deviation of the held press (max and latest = at the release; a 180 deg flip that comes back ends
    // near 0, a stick turn stays), the largest right-stick X while held (a turn needs a deflection), the updates the R3 bit was sent,
    // its release, the crouch posture 500 ms after the press.
    void r3Track(const MapIn& in,bool held,bool sentR3) noexcept {
        if(r3Slot_>=0&&held){
            auto& e=r3Ring[unsigned(r3Slot_)];r3WriteBegin();
            if(sentR3&&e.pulseUpdates<255u)++e.pulseUpdates;
            if(std::isfinite(in.lookX)&&std::fabs(in.lookX)>e.stickMaxX)e.stickMaxX=std::fmin(std::fabs(in.lookX),1.f);
            if(std::isfinite(in.psi)){
                if(!r3PsiBase_||in.psiOrigin!=r3PsiOrigin_){r3PsiBase_=true;r3Psi0_=in.psi;r3PsiOrigin_=in.psiOrigin;}
                const float dev=std::fabs(wrapPi(in.psi-r3Psi0_))*57.2957795f;
                ++e.psiSamples;if(dev>e.psiMaxDevDeg)e.psiMaxDevDeg=dev;e.psiEndDevDeg=dev;
            }
            r3WriteEnd();
        }else if(r3Slot_>=0)r3Release(in.now);
        if(r3CheckSlot_>=0&&in.now>=r3CheckNs_){
            auto& e=r3Ring[unsigned(r3CheckSlot_)];r3WriteBegin();e.crouchedAfter=std::int8_t(std::clamp(in.crouched,-1,1));r3WriteEnd();
            r3CheckSlot_=-1;
        }
    }
    void endSprint(std::atomic<std::uint64_t>* reason) noexcept {if(sprint_!=Sprint::Off&&reason)reason->fetch_add(1,std::memory_order_relaxed);sprint_=Sprint::Off;pump_=0;neutralSince_=0;}
public:
    Tuning tuning{};
    Sprint sprint() const noexcept {return sprint_;}
    void reset() noexcept {latch_={};endSprint(nullptr);crouch_=0;jumpKnown_=false;xPending_=xDeferred_=false;tap_=0;
        r3Wait_=false;r3Slot_=r3CheckSlot_=-1;r3PsiBase_=false;r3Pulse_=0;}
    // Returns the new button word: bits outside bit::mapped are passed unchanged.
    std::uint32_t apply(const MapIn& in,std::uint32_t buttons) noexcept {
        const auto frame=updateFrame.fetch_add(1,std::memory_order_relaxed)+1;
        lastApplyNs.store(in.now,std::memory_order_relaxed);
        const unsigned li=unsigned(in.layout)<4?unsigned(in.layout):0u;
        lastLayout.store(li,std::memory_order_relaxed);
        if(in.jumping>=0){const bool j=in.jumping>0;if(jumpKnown_&&j&&!lastJumping_)noteImpulse(frame,in.now);lastJumping_=j;jumpKnown_=true;}
        else jumpKnown_=false;
        if(!in.ready){
            if(sprint_!=Sprint::Off)endSprint(&sprintEndsContext);
            latch_={};crouch_=0;xPending_=xDeferred_=false;tap_=0;notReady.fetch_add(1,std::memory_order_relaxed);
            // A press waiting for the delivered LT is dropped with the latches: counted and kept in the ring as route WAIT.
            if(r3Wait_){r3Wait_=false;r3WaitDropped.fetch_add(1,std::memory_order_relaxed);r3Record(in,R3Route::Wait,true);}
            r3Pulse_=0;if(r3Slot_>=0)r3Release(in.now);
            sprintState.store(0,std::memory_order_relaxed);sprintWanted.store(false,std::memory_order_relaxed);
            return buttons&~bit::mapped;
        }
        layoutFrames[li].fetch_add(1,std::memory_order_relaxed);
        const auto L=in.layout;const bool lt=(in.buttons&bit::LT)!=0;
        const float mag=!in.stickOwned&&std::isfinite(in.stickX)&&std::isfinite(in.stickY)?std::hypot(in.stickX,in.stickY):0.f;
        if(in.stickOwned)stickOwnedFrames.fetch_add(1,std::memory_order_relaxed);
        static constexpr std::uint32_t touch[6]{bit::A,bit::B,bit::X,bit::Y,bit::L3,bit::R3};
        bool rising[6]{},falling[6]{};
        for(unsigned i=0;i<6;++i){
            const bool down=(in.buttons&touch[i])!=0;auto& l=latch_[i];
            if(down&&!l.held){l.held=true;rising[i]=true;l.out=0;}
            else if(!down&&l.held){l.held=false;falling[i]=true;l.out=0;}
        }
        const bool gameplayMap=L==Layout::Foot||L==Layout::Mounted;
        // LASSO: face remap while a catch is on the rope / stock face buttons in a lasso minigame (pressed buttons keep their mapping)
        const bool lassoStock=gameplayMap&&in.lassoMinigame,lassoFace=gameplayMap&&(in.lassoRemap||in.lassoMinigame);
        if(lassoFace)(in.lassoMinigame?lassoMinigameFrames:lassoRemapFrames).fetch_add(1,std::memory_order_relaxed);
        // A: jump
        if(rising[0]&&lassoFace){latch_[0].out=bit::A;lassoRemapPresses.fetch_add(1,std::memory_order_relaxed);}
        else if(rising[0]&&L==Layout::Mounted){latch_[0].out=bit::A;spurPresses.fetch_add(1,std::memory_order_relaxed);} // A on a horse = spur (one per tap)
        else if(rising[0]){latch_[0].out=gameplayMap?bit::X:bit::A;if(L==Layout::Foot){jumpPresses.fetch_add(1,std::memory_order_relaxed);notePress(frame,in.now);}}
        // B: reload / interact; the manual reload may own it (see ReloadConsumer)
        bool bOwned=false;
        if(gameplayMap)if(const auto f=reloadConsumer.load(std::memory_order_relaxed)){
            try{bOwned=f(rising[1]);}catch(...){reloadErrors.fetch_add(1,std::memory_order_relaxed);}
        }
        if(rising[1]){
            const bool jump=!bOwned&&L==Layout::Mounted&&!lassoFace&&!in.vehicleSeat; // B on a horse = jump (pad X) A = spur
            latch_[1].out=bOwned?0:jump?bit::X:bit::B;
            if(!jump&&gameplayMap)(bOwned?reloadConsumed:reloadPassed).fetch_add(1,std::memory_order_relaxed);
        }
        // X: whistle (D-pad up) on foot; X while a catch is on the rope / in a lasso minigame (LASSO); HOLD = INVENTORY (pad Select)
        if(rising[2]&&lassoFace){latch_[2].out=bit::X;xPending_=false;lassoRemapPresses.fetch_add(1,std::memory_order_relaxed);}
        else if(rising[2]){
            xPending_=true;xDeferred_=gameplayMap;xSince_=in.now;xTapBit_=L==Layout::Foot?bit::DpadUp:bit::X;
            latch_[2].out=xDeferred_?0:bit::X; // Native / Gameplay: stock X at once (menus)
        }
        if(xPending_&&latch_[2].held&&in.now>=xSince_&&in.now-xSince_>=tuning.inventoryHold){
            xPending_=false;latch_[2].out=xDeferred_?bit::Select:bit::X|bit::Select; // Select held while X stays held
            inventoryPresses.fetch_add(1,std::memory_order_relaxed);
        }else if(xPending_&&falling[2]){ // a short press: X's own action now (deferred) or already sent (stock)
            xPending_=false;xTaps.fetch_add(1,std::memory_order_relaxed);
            if(xDeferred_){tapBit_=xTapBit_;tap_=tuning.tapPulse;if(xTapBit_==bit::DpadUp)whistlePresses.fetch_add(1,std::memory_order_relaxed);}
        }
        if(rising[3])latch_[3].out=bit::Y;
        // L3: sprint toggle on foot, spur when mounted
        if(rising[4]&&lassoStock)latch_[4].out=bit::L3; // LASSO minigame: stock L3 (prompt code 9)
        else if(rising[4]&&in.teleportWalk&&L==Layout::Foot){latch_[4].out=0;sprintTeleportIgnored.fetch_add(1,std::memory_order_relaxed);} // TELEPORT walk modifier
        else if(rising[4]){
            if(L==Layout::Foot){
                sprintToggles.fetch_add(1,std::memory_order_relaxed);
                if(sprint_==Sprint::Off){sprint_=mag>=tuning.moveMin?Sprint::On:Sprint::Armed;armedUntil_=in.now+tuning.armWindow;neutralSince_=0;pump_=0;}
                else endSprint(nullptr);
                latch_[4].out=0;
            }else if(L==Layout::Mounted)latch_[4].out=0; // No L3 gallop on a horse (B taps)
            else latch_[4].out=bit::L3;
        }
        // R3: crouch on foot, Dead Eye with LT reaching the game / a gun in a VR hand / while it runs, look-back never in gameplay.
        // The route is fixed when the press is decided (its rising edge, or one update later while the delivered LT is Pending).
        if(rising[5]||falling[5]){r3Pulse_=0;}
        if(rising[5]){r3PressNs_=in.now;r3Wait_=false;}
        if(rising[5]||r3Wait_){
            const bool waited=!rising[5];r3Wait_=false;
            const auto route=r3Route(L,lassoStock,lt,in.ltGame,in.deadEyeGun,in.deadEyeActive,!waited);
            switch(route){
            case R3Route::Native:case R3Route::Lasso:latch_[5].out=bit::R3;break; // LASSO minigame: stock R3 (prompt code 10)
            case R3Route::DeadEyeEnd:case R3Route::DeadEyeLt:case R3Route::DeadEyeGun:
                // A pulse, not the hold: the rest of a held R3 would be camera LOOK_BACK (pad 19) whenever its gate allows
                latch_[5].out=bit::R3;r3Pulse_=std::max(1u,tuning.deadEyePulse);deadEyePresses.fetch_add(1,std::memory_order_relaxed);
                deadeye::bump(route==R3Route::DeadEyeEnd?deadeye::endPresses:route==R3Route::DeadEyeLt?deadeye::ltPresses:deadeye::gunPresses);
                break;
            case R3Route::Crouch:case R3Route::LtWithheldCrouch:
                latch_[5].out=0;crouch_=tuning.crouchPulse;crouchPulses.fetch_add(1,std::memory_order_relaxed);
                if(route==R3Route::LtWithheldCrouch)ltWithheldR3.fetch_add(1,std::memory_order_relaxed);
                break;
            case R3Route::Wait:latch_[5].out=0;r3Wait_=true;r3Waits.fetch_add(1,std::memory_order_relaxed);break;
            default:
                latch_[5].out=0;r3Suppressed.fetch_add(1,std::memory_order_relaxed);
                if(route==R3Route::LtWithheldSuppressed)ltWithheldR3.fetch_add(1,std::memory_order_relaxed);
                break;
            }
            if(route!=R3Route::Wait)r3Record(in,route,waited);
        }
        // R3 bit of this update (the output loop below): the held latch, a Dead Eye pulse ends after deadEyePulse updates
        const bool sendR3=latch_[5].held&&latch_[5].out!=0;
        if(sendR3&&r3Pulse_&&--r3Pulse_==0)latch_[5].out=0; // this update still sends it (sendR3 captured above)
        r3Track(in,latch_[5].held,sendR3);
        // Sprint state
        if(sprint_!=Sprint::Off&&(L!=Layout::Foot||in.teleportWalk||lassoStock))endSprint(&sprintEndsContext); // Never sprinting in TELEPORT / a lasso minigame
        if(sprint_==Sprint::Armed){
            if(mag>=tuning.moveMin)sprint_=Sprint::On;
            else if(in.now>=armedUntil_){sprint_=Sprint::Off;sprintArmExpired.fetch_add(1,std::memory_order_relaxed);}
        }
        if(sprint_==Sprint::On){
            if(mag<tuning.neutralMax){
                if(!neutralSince_)neutralSince_=in.now?in.now:1;
                else if(in.now-neutralSince_>=tuning.neutralHold)endSprint(&sprintEndsNeutral);
            }else neutralSince_=0;
        }
        std::uint32_t out=buttons&~bit::mapped;
        for(const auto& l:latch_)if(l.held)out|=l.out;
        if(sendR3)out|=bit::R3; // also the last pulse update (latch_[5].out already cleared)
        if(bOwned)out&=~bit::B; // the reload owns B this update (whatever the press mapped it to)
        if(sprint_==Sprint::On){
            sprintOnFrames.fetch_add(1,std::memory_order_relaxed);
            if(mag>=tuning.moveMin){
                const unsigned period=tuning.pumpDown+tuning.pumpUp;
                if(pump_%period<tuning.pumpDown){out|=bit::A;if(pump_%period==0)sprintPumps.fetch_add(1,std::memory_order_relaxed);}
                ++pump_;
            }else pump_=0;
        }
        if(crouch_){out|=bit::L3;--crouch_;}
        if(whistleRequest.exchange(false,std::memory_order_relaxed)&&L==Layout::Foot){whistle_=3;whistlePresses.fetch_add(1,std::memory_order_relaxed);}
        if(whistle_){out|=bit::DpadUp;--whistle_;}
        if(tap_){out|=tapBit_;--tap_;}
        sprintState.store(unsigned(sprint_),std::memory_order_relaxed);
        sprintWanted.store(sprint_==Sprint::On,std::memory_order_relaxed);
        return out;
    }
};
inline Mapper mapper; // game input thread only

// ======================================= fast jump (engine) =======================================
inline constexpr std::uintptr_t setupNewJumpOffset=0x22cc950,setupNewJumpGotOffset=0x2e3fb90;  // JumpSequenceAnimTask::SetupNewJump
inline constexpr std::uintptr_t setupNewJumpReturnOffset=0x22cf2b4;  // its only call (Begin 0x22cf2b0, via PLT 0x2c6ea60)
inline constexpr std::uintptr_t beginSiteOffset=0x22cf298,skipBranchOffset=0x22cca04,sequencerJumpSiteOffset=0x22cd258;
inline constexpr std::uintptr_t retimeOffset=0x1e19cd8,retimeGotOffset=0x2e3d5b0;      // animAnimatorComponent::RetimeAnimPlayer(float)
inline constexpr std::uintptr_t playerManagerOffset=0x4e7e7c90;
namespace field {
inline constexpr std::size_t ctxActor=0x28,ctxAnimator=0x30;            // sagActionContext (Begin 0x22cf2e0, SetupNewJump 0x22cc980)
inline constexpr std::size_t taskLaunchClip=0x28,taskState=0x74;        // JumpSequenceAnimTask (0x22cca1c, 0x22cca14)
inline constexpr std::size_t actorGuid=0x8,actorFlags=0x118,playerState=0x5d4,playerGuid=0x5f4,playerActive=0x5f1,pmPlayer=0xa20;
inline constexpr std::size_t moverInstFlags=0x84c,moverPosture=0xb30;   // jump flag bit15 (StartJump 0x288c820); IsCrouching
inline constexpr std::uint16_t jumpingBit=0x8000;
}
inline constexpr std::array<unsigned char,64> setupNewJumpFingerprint{
#include "gamecode/VrControls_22cc950_64_8d.inc"
};
inline constexpr std::array<unsigned char,48> skipBranchFingerprint{
#include "gamecode/VrControls_22cca04_48_8d.inc"
};
inline constexpr std::array<unsigned char,40> beginSiteFingerprint{
#include "gamecode/VrControls_22cf298_40_8d.inc"
};
inline constexpr std::array<unsigned char,132> retimeFingerprint{
#include "gamecode/VrControls_1e19cd8_132_8d.inc"
};
inline constexpr std::array<unsigned char,40> sequencerJumpFingerprint{
#include "gamecode/VrControls_22cd258_40_8d.inc"
};

inline constexpr float defaultLaunchSeconds=0.03f;
// FAST JUMP decision (pure): only the engine's own Begin call, never an engine "true", the local on-foot player, a loaded
// launch clip (the skip branch returns false without one - the jump would be lost).
enum class JumpVerdict : std::uint8_t {Native,Fast,WrongCaller,NotLocal,NoLaunch,Disabled};
inline JumpVerdict jumpVerdict(bool enabled,bool pinnedCaller,bool engineSkip,bool localFoot,bool haveLaunch) noexcept {
    if(!enabled)return JumpVerdict::Disabled;
    if(!pinnedCaller)return JumpVerdict::WrongCaller;
    if(engineSkip)return JumpVerdict::Native;
    if(!localFoot)return JumpVerdict::NotLocal;
    if(!haveLaunch)return JumpVerdict::NoLaunch;
    return JumpVerdict::Fast;
}
inline float sanitizeLaunchSeconds(float s) noexcept {return std::isfinite(s)&&s>=0.f&&s<=0.5f?s:defaultLaunchSeconds;}
inline std::atomic<bool> fastJumpEnabled{true},controlsEnabled{true};inline std::atomic<float> launchSeconds{defaultLaunchSeconds};
inline std::atomic<bool> fastJumpInstalled{};inline std::atomic<const char*> fastJumpInstall{"NOT ATTEMPTED"};
inline std::atomic<std::uint64_t> jumpCalls{},jumpFast{},jumpFastFailed{},jumpNative{},jumpNotLocal{},jumpNoLaunch{},jumpWrongCaller{},
    jumpRetimed{},jumpRetimeSkipped{},jumpOk{},jumpFailed{};
inline std::atomic<bool> crouching{};inline std::atomic<std::uint64_t> crouchChanges{};inline std::atomic<int> jumpFlag{-1};

// "r3": the LT + R3 check (lever, last delivered LT, counters) and the ring, oldest first; ageMs = before the latest mapper update.
inline std::string r3Report() {
    std::array<R3Event,16> ring{};unsigned next=0;bool torn=true;
    for(int attempt=0;attempt<8&&torn;++attempt){
        const auto s0=r3Seq.load(std::memory_order_acquire);if(s0&1u)continue;
        ring=r3Ring;next=r3Next;std::atomic_thread_fence(std::memory_order_acquire);
        torn=r3Seq.load(std::memory_order_relaxed)!=s0;
    }
    const auto now=lastApplyNs.load(std::memory_order_relaxed);const auto dn=ltDeliveredNs.load(std::memory_order_relaxed);
    std::string out;char b[768];
    std::snprintf(b,sizeof b,"\"r3\":{\"deliveredLtCheck\":%s,\"ltDelivered\":%s,\"deliveredAgeMs\":%lld,\"padNotes\":%llu,"
        "\"ltWithheld\":%llu,\"waits\":%llu,\"waitDropped\":%llu,\"deadEyePulse\":%u,\"total\":%llu,\"torn\":%s,\"ring\":[",
        r3DeliveredLt.load()?"true":"false",ltDelivered.load()?"true":"false",
        dn&&now?(long long)((std::int64_t)(now-dn)/1'000'000):-1LL,(unsigned long long)padNotes.load(),
        (unsigned long long)ltWithheldR3.load(),(unsigned long long)r3Waits.load(),(unsigned long long)r3WaitDropped.load(),
        mapper.tuning.deadEyePulse,(unsigned long long)r3Total.load(),torn?"true":"false");
    out+=b;bool first=true;
    for(unsigned k=0;k<ring.size();++k){
        const auto& e=ring[(next+k)%ring.size()];if(!e.ns)continue;
        const unsigned lay=e.layout<4?e.layout:0,rt=e.route<r3RouteNames.size()?e.route:0,lg=e.ltGame<4?e.ltGame:0;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%lld,\"layout\":\"%s\",\"route\":\"%s\",\"ltPhys\":%s,\"ltGame\":\"%s\",\"ltFresh\":%s,"
            "\"waited\":%s,\"deadEyeGun\":%s,\"deadEyeActive\":%s,\"holdHand\":%d,\"crouchedAtPress\":%d,\"crouchedAfter500ms\":%d,"
            "\"heldMs\":%d,\"r3Updates\":%u,\"psiMaxDevDeg\":%.1f,\"psiEndDevDeg\":%.1f,\"psiSamples\":%u,\"stickMaxX\":%.2f,"
            "\"ctxAllowed\":%d,\"blockReason\":\"%s\",\"aimBits\":%d}",
            first?"":",",now>=e.ns?(long long)((now-e.ns)/1'000'000ull):0LL,layoutNames[lay],r3RouteNames[rt],e.ltPhys?"true":"false",
            ltGameNames[lg],lg!=unsigned(LtGame::Physical)?"true":"false",e.waited?"true":"false",e.gun?"true":"false",
            e.active?"true":"false",int(e.holdHand),int(e.crouchedAtPress),int(e.crouchedAfter),int(e.heldMs),unsigned(e.pulseUpdates),
            double(e.psiMaxDevDeg),double(e.psiEndDevDeg),unsigned(e.psiSamples),double(e.stickMaxX),int(e.ctxAllowed),e.block?e.block:"?",
            int(e.aimBits));
        out+=b;first=false;
    }
    out+="]}";
    return out;
}

inline std::string report() {
    std::string out;char b[1024];
    const unsigned li=lastLayout.load()<4?lastLayout.load():0;const unsigned ss=sprintState.load()<3?sprintState.load():0;
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"layout\":\"%s\",\"frames\":%llu,\"layoutFrames\":{\"native\":%llu,\"gameplay\":%llu,\"foot\":%llu,\"mounted\":%llu},"
        "\"notReady\":%llu,\"sprint\":{\"state\":\"%s\",\"toggles\":%llu,\"pumps\":%llu,\"onFrames\":%llu,\"endNeutral\":%llu,\"endContext\":%llu,\"armExpired\":%llu},"
        "\"crouch\":{\"pulses\":%llu,\"crouching\":%s,\"changes\":%llu},\"deadEye\":%llu,\"r3Suppressed\":%llu,\"spur\":%llu,\"whistle\":%llu,"
        "\"reload\":{\"linked\":%s,\"consumed\":%llu,\"passed\":%llu,\"errors\":%llu},",
        controlsEnabled.load()?"true":"false",layoutNames[li],(unsigned long long)updateFrame.load(),
        (unsigned long long)layoutFrames[0].load(),(unsigned long long)layoutFrames[1].load(),(unsigned long long)layoutFrames[2].load(),
        (unsigned long long)layoutFrames[3].load(),(unsigned long long)notReady.load(),sprintNames[ss],(unsigned long long)sprintToggles.load(),
        (unsigned long long)sprintPumps.load(),(unsigned long long)sprintOnFrames.load(),(unsigned long long)sprintEndsNeutral.load(),
        (unsigned long long)sprintEndsContext.load(),(unsigned long long)sprintArmExpired.load(),(unsigned long long)crouchPulses.load(),
        crouching.load()?"true":"false",(unsigned long long)crouchChanges.load(),(unsigned long long)deadEyePresses.load(),
        (unsigned long long)r3Suppressed.load(),(unsigned long long)spurPresses.load(),(unsigned long long)whistlePresses.load(),
        reloadConsumer.load()?"true":"false",(unsigned long long)reloadConsumed.load(),
        (unsigned long long)reloadPassed.load(),(unsigned long long)reloadErrors.load());
    out+=b;
    std::snprintf(b,sizeof b,"\"clicks\":{\"l3\":%llu,\"r3\":%llu,\"chords\":%llu,\"recenters\":%llu,\"chordsAborted\":%llu,\"resets\":%llu},"
        "\"sprintTeleportIgnored\":%llu,\"stickOwnedFrames\":%llu,\"lasso\":{\"remapFrames\":%llu,\"minigameFrames\":%llu,\"presses\":%llu},"
        "\"inventory\":%llu,\"xTaps\":%llu,",
        (unsigned long long)clicksL.load(),(unsigned long long)clicksR.load(),(unsigned long long)chords.load(),(unsigned long long)recenters.load(),
        (unsigned long long)chordsAborted.load(),(unsigned long long)clickResets.load(),(unsigned long long)sprintTeleportIgnored.load(),
        (unsigned long long)stickOwnedFrames.load(),(unsigned long long)lassoRemapFrames.load(),(unsigned long long)lassoMinigameFrames.load(),
        (unsigned long long)lassoRemapPresses.load(),(unsigned long long)inventoryPresses.load(),(unsigned long long)xTaps.load());
    out+=b;
    std::snprintf(b,sizeof b,"\"jump\":{\"presses\":%llu,\"fastInstalled\":%s,\"install\":\"%s\",\"fastEnabled\":%s,\"launchMs\":%.1f,\"calls\":%llu,"
        "\"fast\":%llu,\"fastFailed\":%llu,\"native\":%llu,\"notLocal\":%llu,\"noLaunch\":%llu,\"wrongCaller\":%llu,\"retimed\":%llu,"
        "\"retimeSkipped\":%llu,\"setupOk\":%llu,\"setupFailed\":%llu,\"impulses\":%llu,\"flag\":%d,\"timeouts\":%llu,\"beginsUnmatched\":%llu,"
        "\"impulsesUnmatched\":%llu,",
        (unsigned long long)jumpPresses.load(),fastJumpInstalled.load()?"true":"false",fastJumpInstall.load(),fastJumpEnabled.load()?"true":"false",
        double(launchSeconds.load())*1000.0,(unsigned long long)jumpCalls.load(),(unsigned long long)jumpFast.load(),(unsigned long long)jumpFastFailed.load(),
        (unsigned long long)jumpNative.load(),(unsigned long long)jumpNotLocal.load(),(unsigned long long)jumpNoLaunch.load(),
        (unsigned long long)jumpWrongCaller.load(),(unsigned long long)jumpRetimed.load(),(unsigned long long)jumpRetimeSkipped.load(),
        (unsigned long long)jumpOk.load(),(unsigned long long)jumpFailed.load(),(unsigned long long)impulses.load(),jumpFlag.load(),
        (unsigned long long)jumpTimeouts.load(),(unsigned long long)beginsUnmatched.load(),(unsigned long long)impulsesUnmatched.load());
    out+=b;
    out+="\"latencyFrames\":{";pressToBegin.json(out,"pressToBegin");out+=",";beginToImpulse.json(out,"beginToImpulse");out+=",";
    pressToImpulse.json(out,"pressToImpulse");out+="},";pressToImpulseMs.json(out,"pressToImpulseMs");out+="},";
    out+=r3Report();out+="}";
    return out;
}

#if defined(__ANDROID__)
// ======================================= engine side (Android only) =======================================
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using SetupNewJumpFn=bool(*)(void* task,void* ctx,float phase,bool startAtLaunch);
using RetimeFn=void(*)(void* animator,float seconds);
struct EngineApi {ReadFn read{};std::uintptr_t base{};std::uintptr_t playerManager{},playerManagerTable{},playerTable{};};
namespace detail {
inline EngineApi api{};inline SetupNewJumpFn setupOriginal{};inline RetimeFn retime{};
template<class T> inline bool get(std::uintptr_t p,std::size_t o,T& out) noexcept {
    return api.read&&p&&p<=UINTPTR_MAX-o&&p+o<=UINTPTR_MAX-sizeof(T)&&api.read(nullptr,p+o,&out,sizeof(T));}
// The jumping actor is the local player (flags & 3 == 3, GUID == PLAYERMGR player GUID) in player state 2 (on foot).
inline bool localFootPlayer(std::uintptr_t ctx,std::uintptr_t& animator) noexcept {
    std::uintptr_t actor{},table{},player{};std::uint32_t flags{},guid{},playerGuid{},state{};std::uint8_t active{};
    return get(ctx,field::ctxActor,actor)&&actor&&get(ctx,field::ctxAnimator,animator)&&animator&&
        get(actor,field::actorFlags,flags)&&(flags&3u)==3u&&get(actor,field::actorGuid,guid)&&
        get(api.playerManager,0,table)&&table==api.playerManagerTable&&get(api.playerManager,field::pmPlayer,player)&&player&&
        get(player,0,table)&&table==api.playerTable&&get(player,field::playerActive,active)&&active==1&&
        get(player,field::playerGuid,playerGuid)&&playerGuid==guid&&get(player,field::playerState,state)&&state==2;
}
}
__attribute__((noinline)) inline bool setupNewJumpHook(void* task,void* ctx,float phase,bool startAtLaunch) noexcept {
    const auto original=detail::setupOriginal;
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    jumpCalls.fetch_add(1,std::memory_order_relaxed);
    std::uintptr_t animator{},launch{};
    const bool pinned=caller==detail::api.base+setupNewJumpReturnOffset;
    const bool enabled=fastJumpEnabled.load(std::memory_order_relaxed)&&controlsEnabled.load(std::memory_order_relaxed);
    const bool local=enabled&&pinned&&!startAtLaunch&&detail::localFootPlayer(reinterpret_cast<std::uintptr_t>(ctx),animator);
    const bool haveLaunch=local&&detail::get(reinterpret_cast<std::uintptr_t>(task),field::taskLaunchClip,launch)&&launch;
    const auto verdict=jumpVerdict(enabled,pinned,startAtLaunch,local,haveLaunch);
    if(local)noteBegin(updateFrame.load(std::memory_order_relaxed));
    switch(verdict){
        case JumpVerdict::Fast:break;
        case JumpVerdict::NotLocal:jumpNotLocal.fetch_add(1,std::memory_order_relaxed);return original(task,ctx,phase,startAtLaunch);
        case JumpVerdict::NoLaunch:jumpNoLaunch.fetch_add(1,std::memory_order_relaxed);return original(task,ctx,phase,startAtLaunch);
        case JumpVerdict::WrongCaller:jumpWrongCaller.fetch_add(1,std::memory_order_relaxed);return original(task,ctx,phase,startAtLaunch);
        default:jumpNative.fetch_add(1,std::memory_order_relaxed);return original(task,ctx,phase,startAtLaunch);
    }
    jumpFast.fetch_add(1,std::memory_order_relaxed);
    const bool ok=original(task,ctx,phase,true); // engine branch 0x22cca04: state 2, SetJumpSegment(2), LAUNCH clip + LaunchTime
    (ok?jumpOk:jumpFailed).fetch_add(1,std::memory_order_relaxed);
    if(!ok){jumpFastFailed.fetch_add(1,std::memory_order_relaxed);return ok;}
    std::uint32_t state{};const float seconds=launchSeconds.load(std::memory_order_relaxed);
    if(seconds>0.f&&detail::retime&&detail::get(reinterpret_cast<std::uintptr_t>(task),field::taskState,state)&&state==2){
        detail::retime(reinterpret_cast<void*>(animator),seconds); // same call SetupNewJump made with LaunchTime (0x22cca78)
        jumpRetimed.fetch_add(1,std::memory_order_relaxed);
    }else jumpRetimeSkipped.fetch_add(1,std::memory_order_relaxed);
    return ok;
}
inline bool propertyOn(const char* name) noexcept {char v[PROP_VALUE_MAX]{};return __system_property_get(name,v)<=0||v[0]!='0';}
// debug.rdrvr.vr.controls 0 = stock pad layout (mapper off; the stick-click arbiter / 0.6 s recenter chord stay);
// debug.rdrvr.vr.r3lt 0 = LT + R3 decided by the physical LT (no check that LT reaches the game);
// debug.rdrvr.vr.fastjump 0 = no SetupNewJump hook; debug.rdrvr.vr.jumplaunch <ms> (0..500, 0 = native LaunchTime).
inline void loadProperties() noexcept {
    controlsEnabled.store(propertyOn("debug.rdrvr.vr.controls"));fastJumpEnabled.store(propertyOn("debug.rdrvr.vr.fastjump"));
    r3DeliveredLt.store(propertyOn("debug.rdrvr.vr.r3lt"));
    char v[PROP_VALUE_MAX]{};
    if(__system_property_get("debug.rdrvr.vr.jumplaunch",v)>0){char* end{};const long msv=std::strtol(v,&end,10);
        if(end!=v)launchSeconds.store(sanitizeLaunchSeconds(float(msv)/1000.f));}
}
// code(offset, fingerprint) = executable bytes match; got(slot, target) = *(base+slot) == base+target;
// replace(address, expected, replacement) = TraceEntry replacePointer.
template<class Code,class Got,class Replace>
inline void install(const EngineApi& api,Code&& code,Got&& got,Replace&& replace) noexcept {
    loadProperties();
    if(!fastJumpEnabled.load()){fastJumpInstall.store("KILLED");return;}
    if(!api.read||!api.base||!api.playerManager||!api.playerManagerTable||!api.playerTable){fastJumpInstall.store("NO ENGINE API");return;}
    detail::api=api;const auto b=api.base;
    if(!(code(setupNewJumpOffset,setupNewJumpFingerprint)&&code(skipBranchOffset,skipBranchFingerprint)&&code(beginSiteOffset,beginSiteFingerprint)&&
         code(sequencerJumpSiteOffset,sequencerJumpFingerprint)&&code(retimeOffset,retimeFingerprint)&&
         got(setupNewJumpGotOffset,setupNewJumpOffset)&&got(retimeGotOffset,retimeOffset))){fastJumpInstall.store("ENGINE MISMATCH");return;}
    detail::setupOriginal=reinterpret_cast<SetupNewJumpFn>(b+setupNewJumpOffset);detail::retime=reinterpret_cast<RetimeFn>(b+retimeOffset);
    if(replace(b+setupNewJumpGotOffset,b+setupNewJumpOffset,reinterpret_cast<std::uintptr_t>(&setupNewJumpHook))){
        fastJumpInstalled.store(true,std::memory_order_release);fastJumpInstall.store("INSTALLED");}
    else fastJumpInstall.store("HOOK NOT INSTALLED");
}
#endif // __ANDROID__
} // namespace rdrvr::vr::controls
