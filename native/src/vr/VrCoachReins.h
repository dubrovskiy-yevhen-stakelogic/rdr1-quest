#pragma once
// COACH REINS: the reins gestures (VrReins.h) drive a stagecoach / wagon from the driver's seat.
// Engine facts (librdr 1.58 variant 1, disassembly):
//  - sagPlayer+0x80 = the sagPlayerInputWagon (sagPlayer::UpdateStateVehicle 0x26a1c04 picks it for sagVehicleType 7; vtable 0x2d8e490,
//    address point +0x10; slot +0x30 ApplyInput 0x26bb148). +0x9c = the nav state (GetNavState 0x26bbc10, SetNavState 0x26bbc20),
//    +0x98 = the next one (sagActorScript::GetWagonNextNavStateFromPlayer 0x1cad344). The coach scripts read the nav state as the team's
//    pace: 0 = backing (only while held back), 1 = stop, 2..5 = walk, trot, canter, gallop (Reset / the constructor: 1).
//  - input_wagon.xml: Steer = MOVE_X, ReinsBack = MOVE_Y, Whip = HORSE.SPUR (pad A), FootBrake = GENERIC.BRAKE. UpdateAlternateInput
//    0x26ba888: shift = FootBrake ? +1 : Whip ? -1 : MOVE_Y (dead zone 0.5); steer = -sign(x) x^2 of MOVE_X (dead zone 0.25).
//    ApplyInput 0x26bb1a0..: only while [[[actor+0x38]+0xab8]+0x70]+0x18 == 4 (the driving state), next = clamp(nav + step, 1 (0 when
//    stepping down), 5) with step = -sign(shift); the shift timer (+0x90, tune [0] = 2.0 s) is set ready on every frame without a shift,
//    so each new press moves the nav state by one at once and a held press repeats every 2 s. A Whip press also plays the whip
//    (SetupPacket 0x26bb4f0).
//  - The facts are read only on the driver's seat with the row ON: 3 guarded reads a main-thread sample (+ 4 every 4th: the driving state).
// So: SNAP (the brain's gait + 1) = one Whip tap, LIFT / SLOW = one MOVE_Y back tap per step, PULL = MOVE_X. The brain's gait 0..4
// maps to nav 1..5; the taps close the loop on the nav state the riding sample reads (main thread, guarded reads, vtable and code
// words checked once: any mismatch = no taps). Never below nav 1 (no backing). While no rein is held the brain follows the game's nav
// state (the player's own stick / A / a script keep the pace the coach has).
// Menu: BUTTONS > COACH REINS (controls.cfg "coach.reins", needs REINS GESTURES ON). Levers: debug.rdrvr.vr.reins.coach 0 = off,
// .coachlin 0 = the raw rein steer on MOVE_X (default: pre-shaped so the game's dead zone and square give a linear turn).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rdrvr::vr::coach {
inline constexpr std::uintptr_t footTableOffset=0x2d8e1e8;      // sagPlayerInputFoot vtable address point (= movementFootTableOffset)
inline constexpr std::uintptr_t wagonTableOffset=0x2d8e4a0;     // sagPlayerInputWagon vtable address point
inline constexpr std::uintptr_t wagonApplySlot=0x30,wagonApplyOffset=0x26bb148;
inline constexpr std::uintptr_t navGetterOffset=0x26bbc10;      // ldr w0, [x0, #0x9c]; ret
inline constexpr std::uint64_t navGetterPrint=0xd65f03c0b9409c00ull;
inline constexpr std::uintptr_t shiftGateOffset=0x26bb1a0;      // ldr w8, [x8, #0x18]; cmp w8, #0x4; b.ne 0x26bb200
inline constexpr std::array<std::uint32_t,3> shiftGatePrint{0xb9401908u,0x7100111fu,0x540002c1u};
inline constexpr std::size_t playerWagonInput=0x80,navNext=0x98,navNow=0x9c; // read together: [0] next, [1] now
inline constexpr int navStop=1,navMax=5,driveState=4;

inline std::atomic<bool> menuOn{true};   // BUTTONS row (VrControlMap.h storage)
inline std::atomic<bool> killed{false};  // debug.rdrvr.vr.reins.coach 0 (VrReins readProps)
inline std::atomic<bool> linear{true};   // .coachlin
inline bool enabled() noexcept {return menuOn.load(std::memory_order_relaxed)&&!killed.load(std::memory_order_relaxed);}
// Main thread (riding sample, player state 3): the wagon input's nav states and the driving state, stamped; verify -1 = mismatch.
inline std::atomic<int> nav{-1},next{-1},drive{-1},verify{0};inline std::atomic<std::uint64_t> factsNs{0},reads{0},readFails{0};
inline std::atomic<const char*> factsWhy{"NOT_RUN"};
inline void publish(int n,int x,int d,std::uint64_t now) noexcept {
    nav.store(n,std::memory_order_relaxed);next.store(x,std::memory_order_relaxed);drive.store(d,std::memory_order_relaxed);
    factsNs.store(now?now:1,std::memory_order_release);reads.fetch_add(1,std::memory_order_relaxed);}
inline bool factsFresh(std::uint64_t now) noexcept {const auto t=factsNs.load(std::memory_order_acquire);return t&&now>=t&&now-t<300'000'000ull;}
// The brain's gait (0 HALT .. 4 GALLOP) <-> the nav state (1 .. 5)
inline int navOfGait(int gait) noexcept {return std::clamp(gait+1,navStop,navMax);}
inline int gaitOfNav(int n) noexcept {return n<navStop?0:std::min(n,navMax)-1;}
// The rein steer s (-1..1, + = right) on MOVE_X: the game takes ((|x| - .25) / .75)^2, so x = .25 + .75 sqrt(|s|) gives |s| back.
inline float shapeSteer(float s,bool lin) noexcept {
    if(!std::isfinite(s))return 0.f;s=std::clamp(s,-1.f,1.f);
    if(!lin)return s;const float a=std::fabs(s);if(a<.01f)return 0.f;
    return std::copysign(std::min(1.f,.25f+.75f*std::sqrt(a)),s);}

// The tap machine (input thread). A tap is held until the published nav state moved from its value at the press (>= minNs) or
// for maxNs (well under the 2 s repeat), then released for gapNs so the game sees the next press as new. Three taps in a row
// that moved nothing back off for 1 s (the driving state elsewhere / a script owns the pace). Nav 0 (backing) exists only while
// a back input is held (MOVE_Y / FootBrake from the player): no tap then, a Whip would override that input.
struct Tap {
    enum Phase : int {Idle,Press,Gap};
    enum Dir : int {None=0,Up=1,Down=-1};
    std::uint64_t minNs{60'000'000ull},maxNs{220'000'000ull},gapNs{100'000'000ull},backoffNs{1'000'000'000ull};
    Phase phase{Idle};Dir dir{None};int navAtPress{-1};std::uint64_t since{},backoffUntil{};int misses{};
    // TOP WHIP: snaps the brain accepted while the pace is already the top one (want == nav == max); each = one Whip press that is
    // no miss (the nav state cannot move) - the whip crack the rider expects from the stroke
    int kickWant{};bool kickPress{};
    std::atomic<std::uint64_t> ups{},downs{},moved{},missed{},backoffs{},kicks{}; // the report reads them on another thread
    struct Out {bool whip{},back{};};
    void reset() noexcept {phase=Idle;dir=None;navAtPress=-1;since=0;misses=0;kickWant=0;kickPress=false;}
    // want / navNow: nav states (-1 = unknown); live = driver's seat, the brain driving, the facts fresh, the driving state 4
    Out step(std::uint64_t now,bool live,int want,int navNowV) noexcept {
        Out o{};
        if(!live||navNowV<navStop){if(phase==Press){phase=Gap;since=now;}if(phase==Gap&&now-since>=gapNs)phase=Idle;if(!live)misses=0;return o;}
        want=std::clamp(want,navStop,navMax);
        if(phase==Press){
            const bool movedNow=navNowV!=navAtPress;
            if((movedNow&&now-since>=minNs)||now-since>=maxNs){
                if(movedNow){++moved;misses=0;}else if(!kickPress){++missed;if(++misses>=3){misses=0;backoffUntil=now+backoffNs;++backoffs;}}
                phase=Gap;since=now;kickPress=false;}
            else{o.whip=dir==Up;o.back=dir==Down;return o;}
        }
        if(phase==Gap){if(now-since<gapNs)return o;phase=Idle;}
        if(now<backoffUntil)return o;
        const Dir d=want>navNowV?Up:want<navNowV&&navNowV>navStop?Down:None;
        if(d==None){
            if(kickWant>0&&want>=navMax&&navNowV>=navMax){--kickWant;phase=Press;dir=Up;navAtPress=navNowV;since=now;kickPress=true;++kicks;o.whip=true;}
            return o;}
        kickWant=0; // a pace change is under way: its own taps carry the stroke
        phase=Press;dir=d;navAtPress=navNowV;since=now;(d==Up?ups:downs)+=1;
        o.whip=d==Up;o.back=d==Down;return o;
    }
};
inline Tap tap{};
// Report (reins.coach), written on the input thread
inline std::atomic<bool> liveNow{false};inline std::atomic<int> wantNav{-1};
inline std::atomic<std::uint64_t> liveFrames{0},steerFrames{0},whipFrames{0},backFrames{0},notDriving{0},stale{0};
inline std::atomic<const char*> why{"IDLE"};
inline std::string report() {
    char b[640];
    std::snprintf(b,sizeof b,"{\"menu\":%s,\"killed\":%s,\"linear\":%s,\"live\":%s,\"why\":\"%s\",\"nav\":%d,\"next\":%d,\"drive\":%d,\"want\":%d,"
        "\"verify\":%d,\"facts\":\"%s\",\"reads\":%llu,\"readFails\":%llu,\"liveFrames\":%llu,\"steerFrames\":%llu,\"whipFrames\":%llu,\"backFrames\":%llu,"
        "\"notDriving\":%llu,\"stale\":%llu,\"taps\":{\"up\":%llu,\"down\":%llu,\"moved\":%llu,\"missed\":%llu,\"backoffs\":%llu,\"topWhips\":%llu}}",
        menuOn.load()?"true":"false",killed.load()?"true":"false",linear.load()?"true":"false",liveNow.load()?"true":"false",why.load(),
        nav.load(),next.load(),drive.load(),wantNav.load(),verify.load(),factsWhy.load(),(unsigned long long)reads.load(),(unsigned long long)readFails.load(),
        (unsigned long long)liveFrames.load(),(unsigned long long)steerFrames.load(),(unsigned long long)whipFrames.load(),(unsigned long long)backFrames.load(),
        (unsigned long long)notDriving.load(),(unsigned long long)stale.load(),(unsigned long long)tap.ups.load(),(unsigned long long)tap.downs.load(),
        (unsigned long long)tap.moved.load(),(unsigned long long)tap.missed.load(),(unsigned long long)tap.backoffs.load(),(unsigned long long)tap.kicks.load());
    return b;
}
} // namespace rdrvr::vr::coach
