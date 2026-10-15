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
//    Android key -> d11 pad button (AndroidGamepad_c::GamepadButtonEvent 0x198ca7c): 96->0 A, 97->1 B, 99->2 X, 100->3 Y,
//    19->10 D-pad up, 106->18 L3, 107->19 R3.
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
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::controls {

// Pad bits: identical to rdrvr::runtime::PadButton (static_assert in the GameXr glue). DpadUp is output-only.
namespace bit {
inline constexpr std::uint32_t A=1u<<0,B=1u<<1,X=1u<<2,Y=1u<<3,Menu=1u<<4,L3=1u<<5,R3=1u<<6,LGrip=1u<<7,RGrip=1u<<8,
    LT=1u<<9,RT=1u<<10,DpadUp=1u<<11;
inline constexpr std::uint32_t mapped=A|B|X|Y|L3|R3|DpadUp; // the only bits the mapper owns
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
//            L3 sprint toggle, R3 crouch toggle, LT+R3 Dead Eye.
// Mounted  = any other player state (horse / wagon / vehicle): A = spur/accelerate (pad A, one press per tap; user: repeated taps
//            like the stock game), B = jump, the manual reload keeps B while it owns it, L3 nothing, X = X, LT+R3 Dead Eye.
inline Layout layoutFor(bool menuOrUnknown,bool playerRead,std::uint32_t playerMode) noexcept {
    if(menuOrUnknown)return Layout::Native;
    if(!playerRead)return Layout::Gameplay;
    return playerMode==2?Layout::Foot:Layout::Mounted;
}

struct Tuning {
    float moveMin{0.25f};            // stick deflection that counts as "moving" (sprint arm / pump)
    float neutralMax{0.20f};         // below this the stick is neutral
    std::uint64_t neutralHold{ms(200)};  // sprint ends after the stick is neutral this long
    std::uint64_t armWindow{ms(1500)};   // L3 clicked with a neutral stick: sprint waits this long for a deflection
    unsigned pumpDown{2},pumpUp{1};  // sprint A pump train in pad updates
    unsigned crouchPulse{2};         // L3 (CROUCH) pulse length in pad updates
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
    reloadErrors{},notReady{},sprintTeleportIgnored{},stickOwnedFrames{},lassoRemapFrames{},lassoMinigameFrames{},lassoRemapPresses{};
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
    void endSprint(std::atomic<std::uint64_t>* reason) noexcept {if(sprint_!=Sprint::Off&&reason)reason->fetch_add(1,std::memory_order_relaxed);sprint_=Sprint::Off;pump_=0;neutralSince_=0;}
public:
    Tuning tuning{};
    Sprint sprint() const noexcept {return sprint_;}
    void reset() noexcept {latch_={};endSprint(nullptr);crouch_=0;jumpKnown_=false;}
    // Returns the new button word: bits outside bit::mapped are passed unchanged.
    std::uint32_t apply(const MapIn& in,std::uint32_t buttons) noexcept {
        const auto frame=updateFrame.fetch_add(1,std::memory_order_relaxed)+1;
        const unsigned li=unsigned(in.layout)<4?unsigned(in.layout):0u;
        lastLayout.store(li,std::memory_order_relaxed);
        if(in.jumping>=0){const bool j=in.jumping>0;if(jumpKnown_&&j&&!lastJumping_)noteImpulse(frame,in.now);lastJumping_=j;jumpKnown_=true;}
        else jumpKnown_=false;
        if(!in.ready){
            if(sprint_!=Sprint::Off)endSprint(&sprintEndsContext);
            latch_={};crouch_=0;notReady.fetch_add(1,std::memory_order_relaxed);
            sprintState.store(0,std::memory_order_relaxed);sprintWanted.store(false,std::memory_order_relaxed);
            return buttons&~bit::mapped;
        }
        layoutFrames[li].fetch_add(1,std::memory_order_relaxed);
        const auto L=in.layout;const bool lt=(in.buttons&bit::LT)!=0;
        const float mag=!in.stickOwned&&std::isfinite(in.stickX)&&std::isfinite(in.stickY)?std::hypot(in.stickX,in.stickY):0.f;
        if(in.stickOwned)stickOwnedFrames.fetch_add(1,std::memory_order_relaxed);
        static constexpr std::uint32_t touch[6]{bit::A,bit::B,bit::X,bit::Y,bit::L3,bit::R3};
        bool rising[6]{};
        for(unsigned i=0;i<6;++i){
            const bool down=(in.buttons&touch[i])!=0;auto& l=latch_[i];
            if(down&&!l.held){l.held=true;rising[i]=true;l.out=0;}
            else if(!down&&l.held){l.held=false;l.out=0;}
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
            const bool jump=!bOwned&&L==Layout::Mounted&&!lassoFace; // B on a horse = jump (pad X) A = spur
            latch_[1].out=bOwned?0:jump?bit::X:bit::B;
            if(!jump&&gameplayMap)(bOwned?reloadConsumed:reloadPassed).fetch_add(1,std::memory_order_relaxed);
        }
        // X: whistle (D-pad up) on foot; X while a catch is on the rope / in a lasso minigame (LASSO)
        if(rising[2]&&lassoFace){latch_[2].out=bit::X;lassoRemapPresses.fetch_add(1,std::memory_order_relaxed);}
        else if(rising[2]){latch_[2].out=L==Layout::Foot?bit::DpadUp:bit::X;if(L==Layout::Foot)whistlePresses.fetch_add(1,std::memory_order_relaxed);}
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
        // R3: crouch on foot, Dead Eye with LT, look-back never in gameplay
        if(rising[5]){
            if(L==Layout::Native||lassoStock)latch_[5].out=bit::R3; // LASSO minigame: stock R3 (prompt code 10)
            else if(lt){latch_[5].out=bit::R3;deadEyePresses.fetch_add(1,std::memory_order_relaxed);}
            else if(L==Layout::Foot){latch_[5].out=0;crouch_=tuning.crouchPulse;crouchPulses.fetch_add(1,std::memory_order_relaxed);}
            else{latch_[5].out=0;r3Suppressed.fetch_add(1,std::memory_order_relaxed);}
        }
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
        "\"sprintTeleportIgnored\":%llu,\"stickOwnedFrames\":%llu,\"lasso\":{\"remapFrames\":%llu,\"minigameFrames\":%llu,\"presses\":%llu},",
        (unsigned long long)clicksL.load(),(unsigned long long)clicksR.load(),(unsigned long long)chords.load(),(unsigned long long)recenters.load(),
        (unsigned long long)chordsAborted.load(),(unsigned long long)clickResets.load(),(unsigned long long)sprintTeleportIgnored.load(),
        (unsigned long long)stickOwnedFrames.load(),(unsigned long long)lassoRemapFrames.load(),(unsigned long long)lassoMinigameFrames.load(),
        (unsigned long long)lassoRemapPresses.load());
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
    pressToImpulse.json(out,"pressToImpulse");out+="},";pressToImpulseMs.json(out,"pressToImpulseMs");out+="}}";
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
// debug.rdrvr.vr.fastjump 0 = no SetupNewJump hook; debug.rdrvr.vr.jumplaunch <ms> (0..500, 0 = native LaunchTime).
inline void loadProperties() noexcept {
    controlsEnabled.store(propertyOn("debug.rdrvr.vr.controls"));fastJumpEnabled.store(propertyOn("debug.rdrvr.vr.fastjump"));
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
