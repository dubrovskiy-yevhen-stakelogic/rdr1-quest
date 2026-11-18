#pragma once
// GRAB (hand physics): the light link between the hand-physics tick (VrHandPhys.h), the holsters input brain (VrHolsters.h) and the
// melee tick (VrMelee.h). The handphys and melee ticks run on the engine main thread, the holsters brain in the pad filter of the game's
// input update (the input thread the holsters name): every field is an atomic, a press stores its clock before its serial (release),
// the tick loads the serial (acquire) first. All stamps are steady-clock ns; readyAt allows one up to 5 ms ahead of the reader's clock.
//  - ready[h]: the handphys tick found a grab candidate under this hand (a live hand at a prop or a corpse part, open or closing);
//    stamped with the tick's clock and kept for readyMaxNs after the last tick that found one. The holsters take a fresh grip press of
//    that hand for the grab: captured (no LB / RB / cover / block reaches the game), pressed here.
//  - press[h]: the press serial and clock the handphys tick consumes (at most grabPressMaxNs old).
//  - holding[h]: the hand holds a prop: VrMelee gives that hand no fist (no punch), the guard ignores it, the handphys slap / press skip it.
//  - latch[h]: every fresh grip latch of the holsters (the 0.65 crossing): the branch that took it, its clock and serial, the grip then and
//    the age of the ready stamp then (diagnostics: the handphys tick matches a ready that came after a latch it did not get).
//  - holdKind[h]: what the hand holds (HoldKind, set by the handphys grab); join[h]: the other hand holds a ball-jointed prop or a corpse
//    and this hand is within the join distance of it (stamped like ready; the prop is then this hand's grab candidate).
//  - held[h]: this hand's distance to the nearest part of the ball-jointed prop or corpse the other hand holds (stamped like ready by
//    every handphys tick that measured it; expires after readyMaxNs, so a stopped tick never leaves a mask on).
//  - grabFirst / heavyMask / heavyNearM (handphys levers grabfirst / heavymask / heavymaskcm): a ready grab beats the holster sockets for
//    every hand (default off: the sockets first, as before the two-hand grab); a free hand within heavyNearM of the heavy furniture the other
//    hand holds draws nothing unless it has a ready grab of its own (which then beats the sockets).
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace rdrvr::vr::grablink {
inline std::array<std::atomic<std::uint64_t>,2> readyNs{};   // 0 = no candidate
inline std::array<std::atomic<std::uint32_t>,2> pressSerial{};
inline std::array<std::atomic<std::uint64_t>,2> pressNs{};
inline std::array<std::atomic<bool>,2> holdingNow{};
inline std::array<std::atomic<std::uint64_t>,2> holdingNs{}; // the hand-physics tick that last saw the hold (0 = none)
inline std::array<std::atomic<std::uint64_t>,2> pressesTaken{};
inline constexpr std::uint64_t readyMaxNs=100'000'000ull;
// What a hand holds: none, a fixed-rotation light prop, a ball-jointed nav prop (heavy furniture: a chair), a corpse part.
enum HoldKind : std::uint8_t {HoldNone=0,HoldLight,HoldHeavy,HoldCorpse};
inline std::array<std::atomic<std::uint8_t>,2> holdKind{};
inline std::array<std::atomic<std::uint64_t>,2> joinNs{};   // 0 = no join candidate
inline std::array<std::atomic<std::int32_t>,2> joinMm{};    // the last join candidate's distance (mm)
inline std::array<std::atomic<std::uint64_t>,2> heldNs{};   // 0 = not measured
inline std::array<std::atomic<std::int32_t>,2> heldMm{};
inline std::atomic<bool> grabFirst{false},heavyMask{true};inline std::atomic<float> heavyNearM{.30f};
// chordGuard (handphys lever grabchord): a hand holding a prop, at a grab candidate or at a join candidate keeps the manual-theater grip
// chord off (holsters::gripsEngaged): the chord zeroes both grips in the controller sample from the moment both pass 0.85, which the
// hold reads as an open hand. heldLatch (lever grabheldlatch): a fresh latch of a hand that holds a prop is captured (LatchHeld), never
// LB / RB. chordGrabBusy = samples the chord stayed off for a grab.
inline std::atomic<bool> chordGuard{true},heldLatch{true};inline std::atomic<std::uint64_t> chordGrabBusy{};
// HOLD-END GRIP LATCH (handphys lever grabholdlatch): a grip the holsters captured stays theirs until it opens, whatever ends the hold
// (body, detached, safety, teleport, mode): capturedMask (bit h = hand h captured by the holsters, written by the input brain every
// frame) keeps the theater chord off like a grab, and a squeeze the chord zeroed (ControllerSample::chordGrips) is captured by the brain
// instead of read as an open hand. lastEnd[h] = the GrabEnd of hand h's last hold (0xff none), written by the handphys releaseGrab.
inline std::atomic<bool> holdLatch{true};
inline std::atomic<std::uint8_t> capturedMask{};
// capturedNs = the steady clock of the mask's last write (holsters::nowNs): a mask older than capturedFreshNs (the input brain no longer
// runs: loading, input thread stalled) keeps nothing busy
inline std::atomic<std::uint64_t> capturedNs{};inline constexpr std::uint64_t capturedFreshNs=250'000'000ull;
inline bool capturedFresh(std::uint64_t now) noexcept {
    const auto t=capturedNs.load(std::memory_order_relaxed);return t&&(now<t||now-t<=capturedFreshNs);}
inline std::array<std::atomic<std::uint8_t>,2> lastEnd{{0xff,0xff}};
// chordCapturedBusy = theater-chord samples kept off by a captured grip; chordZeroSamples = brain samples whose grip the chord zeroed;
// chordOwnedPresses = such squeezes the brain captured (masked until open); holdEndNativeLeak = a native latch (LB / RB) of a hand after
// its hold ended and before its grip opened (expected 0); holdEndRows = rows written to the ring below.
inline std::atomic<std::uint64_t> chordCapturedBusy{};
inline std::array<std::atomic<std::uint64_t>,2> chordZeroSamples{},chordOwnedPresses{},holdEndNativeLeak{},holdEndRows{};
// The grip-latch ring (16, input thread): one row per hold end seen by the brain (holding true -> false), closed when the raw grip opens
// (<= 0.30 with no chord zero) or after 10 s: clock, hand, end reason, grip at the end, captured then, frames the hand stayed masked, time
// to the open (ms, -1 never), chord-zeroed samples, the first fresh-latch branch before the open (-1 none: expected).
struct GripLatchRec {std::uint64_t ns{};std::uint8_t hand{},end{0xff},captured{};std::int8_t firstLatch{-1};float grip{};std::uint32_t maskedFrames{},chordZeroed{};std::int32_t openMs{-1};};
inline std::mutex gripLatchMutex;inline std::array<GripLatchRec,16> gripLatchRing{};inline unsigned gripLatchNext{};
inline void noteGripLatch(const GripLatchRec& r) noexcept {
    try{std::lock_guard lock(gripLatchMutex);gripLatchRing[gripLatchNext]=r;gripLatchNext=(gripLatchNext+1u)%unsigned(gripLatchRing.size());}catch(...){}
    holdEndRows[r.hand&1u].fetch_add(1,std::memory_order_relaxed);}
inline void setHoldKind(unsigned h,HoldKind k) noexcept {holdKind[h&1u].store(std::uint8_t(k),std::memory_order_relaxed);}
inline HoldKind heldKind(unsigned h) noexcept {const auto k=holdKind[h&1u].load(std::memory_order_relaxed);return k<=HoldCorpse?HoldKind(k):HoldNone;}
inline void setJoin(unsigned h,bool on,std::uint64_t now,float distM) noexcept {
    joinNs[h&1u].store(on?(now?now:1):0,std::memory_order_relaxed);if(on)joinMm[h&1u].store(std::int32_t(distM*1000.f),std::memory_order_relaxed);}
inline bool joinAt(unsigned h,std::uint64_t now) noexcept {
    const auto t=joinNs[h&1u].load(std::memory_order_relaxed);return t&&(now>=t?now-t<=readyMaxNs:t-now<5'000'000ull);}
// This hand's distance to the prop the other hand holds (distM < 0 = none: the stamp cleared).
inline void setHeld(unsigned h,float distM,std::uint64_t now) noexcept {
    const bool on=distM==distM&&distM>=0.f&&distM<1e3f;
    if(on)heldMm[h&1u].store(std::int32_t(distM*1000.f),std::memory_order_relaxed);heldNs[h&1u].store(on?(now?now:1):0,std::memory_order_relaxed);}
// The fresh held distance (mm, -1 none or expired).
inline std::int32_t heldMmAt(unsigned h,std::uint64_t now) noexcept {
    const auto t=heldNs[h&1u].load(std::memory_order_relaxed);
    return t&&(now>=t?now-t<=readyMaxNs:t-now<5'000'000ull)?heldMm[h&1u].load(std::memory_order_relaxed):-1;}
// The mask's distance test: a fresh held distance within nearM.
inline constexpr bool heavyNear(std::int32_t mm,float nearM) noexcept {return mm>=0&&float(mm)<=nearM*1000.f;}
// The other hand holds heavy furniture, the mask is on and this hand is within heavyNearM of it (its draws are masked while it is free).
inline bool heavyOther(unsigned h,std::uint64_t now) noexcept {
    return heavyMask.load(std::memory_order_relaxed)&&heldKind((h&1u)^1u)==HoldHeavy&&heavyNear(heldMmAt(h,now),heavyNearM.load(std::memory_order_relaxed));}
// The holsters' pre-socket decision of a fresh latch (after toss / catch): a join candidate is grabbed (it beats every socket, the mouth,
// the back), a free hand beside heavy furniture with no ready grab of its own is masked (no draw, no LB / RB), else the normal branches
// (PreNone). grabBeforeSocket: a ready grab beats the sockets (after reins) with grabFirst, or for a free hand beside heavy furniture.
enum PreLatch : std::uint8_t {PreNone=0,PreJoin,PreHeavyMask};
inline constexpr PreLatch preLatch(bool joinReady,bool heavyOtherHand,bool freeHand) noexcept {
    return joinReady?PreJoin:heavyOtherHand&&freeHand?PreHeavyMask:PreNone;}
inline constexpr bool grabBeforeSocket(bool grabReady,bool first) noexcept {return grabReady&&first;}
// A join candidate (handphys): the other hand holds a ball-jointed prop or a corpse (HoldHeavy / HoldCorpse) and this hand's nearest grab
// point is within joinM of the prop's nearest part.
inline constexpr bool joinCandidate(HoldKind other,float distM,float joinM) noexcept {
    return (other==HoldHeavy||other==HoldCorpse)&&distM==distM&&distM>=0.f&&distM<=joinM;}
inline void setReady(unsigned h,bool on,std::uint64_t now) noexcept {readyNs[h&1u].store(on?(now?now:1):0,std::memory_order_relaxed);}
inline bool readyAt(unsigned h,std::uint64_t now) noexcept {
    const auto t=readyNs[h&1u].load(std::memory_order_relaxed);return t&&(now>=t?now-t<=readyMaxNs:t-now<5'000'000ull);}
inline void press(unsigned h,std::uint64_t now) noexcept {
    pressNs[h&1u].store(now?now:1,std::memory_order_relaxed);pressSerial[h&1u].fetch_add(1,std::memory_order_release);
    pressesTaken[h&1u].fetch_add(1,std::memory_order_relaxed);}
inline bool holding(unsigned h) noexcept {return holdingNow[h&1u].load(std::memory_order_relaxed);}
// A hold the hand-physics tick saw within holdFreshNs (a tick that stops running never leaves the hand's latches captured).
inline constexpr std::uint64_t holdFreshNs=250'000'000ull;
inline bool holdingAt(unsigned h,std::uint64_t now) noexcept {
    if(!holding(h))return false;const auto t=holdingNs[h&1u].load(std::memory_order_relaxed);
    return t&&(now>=t?now-t<=holdFreshNs:t-now<5'000'000ull);}
// A grab owns the grips: a hand holds a prop, or is at a grab or join candidate (the press that starts the hold is under way).
inline bool grabBusy(std::uint64_t now) noexcept {
    return holdingAt(0,now)||holdingAt(1,now)||readyAt(0,now)||readyAt(1,now)||joinAt(0,now)||joinAt(1,now);}
inline void setHolding(unsigned h,bool on,std::uint64_t now) noexcept {
    holdingNow[h&1u].store(on,std::memory_order_relaxed);holdingNs[h&1u].store(on?(now?now:1):0,std::memory_order_relaxed);}
inline void stampHolding(unsigned h,std::uint64_t now) noexcept {if(holding(h))holdingNs[h&1u].store(now?now:1,std::memory_order_relaxed);}
// The holsters' branches of a fresh latch, in the order they are tried.
// join / heavyMask come right after catch; grab is tried before socket while grabFirst is on or the hand is beside held heavy furniture.
enum LatchBranch : std::uint8_t {LatchToss=0,LatchCatch,LatchBackEmpty,LatchMouth,LatchNotOwned,LatchReins,LatchSocket,LatchForegrip,LatchTransfer,
    LatchHammer,LatchAdopt,LatchPhantom,LatchGrab,LatchGuard,LatchLongGun,LatchReinsMask,LatchNative,LatchJoin,LatchHeavyMask,LatchHeld,LatchCount};
inline constexpr std::array<const char*,LatchCount> latchNames{"toss","catch","backEmpty","mouth","notOwned","reins","socket","foregrip","transfer",
    "hammer","adopt","phantom","grab","guard","longGun","reinsMask","native","join","heavyMask","held"};
// The ready stamp's age at a latch: never set, 0-25, 25-50, 50-100 ms, older (expired); the grip at a latch: <.7, .7-.8, .8-.9, >=.9.
inline constexpr unsigned readyAgeBuckets=5,gripBuckets=4;
inline std::array<std::array<std::atomic<std::uint64_t>,LatchCount>,2> latchBy{};
inline std::array<std::array<std::atomic<std::uint64_t>,readyAgeBuckets>,2> latchReadyAge{};
inline std::array<std::array<std::atomic<std::uint64_t>,gripBuckets>,2> latchGrip{};
inline std::array<std::atomic<std::uint64_t>,2> latchNs{};
inline std::array<std::atomic<std::uint32_t>,2> latchSerial{};
inline std::array<std::atomic<std::uint8_t>,2> latchBranch{};
inline constexpr unsigned readyAgeBucket(std::uint64_t readyNsAtLatch,std::uint64_t now) noexcept {
    if(!readyNsAtLatch)return 0;
    const std::uint64_t age=now>=readyNsAtLatch?now-readyNsAtLatch:0;
    return age<25'000'000ull?1u:age<50'000'000ull?2u:age<=readyMaxNs?3u:4u;}
inline constexpr unsigned gripBucket(float g) noexcept {return g<.7f?0u:g<.8f?1u:g<.9f?2u:3u;}
// The latch ring (16, written on the input thread at each fresh latch): the clock, hand, branch, grip, the ready stamp's age (ms, -1
// none), the join distance (mm, -1 no join candidate), the other hand's HoldKind, the socket the hand was in (-1 none) and its distance
// over the enter radius, the distance between the hands (cm, -1 unknown), the hand's distance to the prop the other hand holds (mm, -1
// none).
struct LatchRec {std::uint64_t ns{};std::uint8_t hand{},branch{},otherKind{};std::int8_t inside{-1};float grip{},insideS{-1.f},handsCm{-1.f};std::int32_t readyMs{-1},joinMm{-1},heldMm{-1};};
inline std::mutex latchRingMutex;inline std::array<LatchRec,16> latchRing{};inline unsigned latchRingNext{};
inline void noteLatch(unsigned h,LatchBranch b,float grip,std::uint64_t now,int inside=-1,float insideS=-1.f,float handsCm=-1.f) noexcept {
    h&=1u;const unsigned k=b<LatchCount?unsigned(b):unsigned(LatchNative);
    {LatchRec r{};r.ns=now;r.hand=std::uint8_t(h);r.branch=std::uint8_t(k);r.otherKind=std::uint8_t(heldKind(h^1u));r.inside=std::int8_t(inside<-1?-1:inside>127?127:inside);
     r.grip=grip;r.insideS=insideS;r.handsCm=handsCm;
     const auto rn=readyNs[h].load(std::memory_order_relaxed);if(rn)r.readyMs=std::int32_t(now>=rn?(now-rn)/1'000'000ull:0u);
     if(joinAt(h,now))r.joinMm=joinMm[h].load(std::memory_order_relaxed);r.heldMm=heldMmAt(h,now);
     try{std::lock_guard lock(latchRingMutex);latchRing[latchRingNext]=r;latchRingNext=(latchRingNext+1u)%unsigned(latchRing.size());}catch(...){}}
    latchBy[h][k].fetch_add(1,std::memory_order_relaxed);
    latchReadyAge[h][readyAgeBucket(readyNs[h].load(std::memory_order_relaxed),now)].fetch_add(1,std::memory_order_relaxed);
    latchGrip[h][gripBucket(grip)].fetch_add(1,std::memory_order_relaxed);
    latchBranch[h].store(std::uint8_t(k),std::memory_order_relaxed);latchNs[h].store(now?now:1,std::memory_order_relaxed);
    latchSerial[h].fetch_add(1,std::memory_order_release);}
inline void resetForTests() noexcept {
    for(unsigned h=0;h<2;++h){readyNs[h].store(0);pressSerial[h].store(0);pressNs[h].store(0);holdingNow[h].store(false);holdingNs[h].store(0);pressesTaken[h].store(0);
        for(auto& x:latchBy[h])x.store(0);for(auto& x:latchReadyAge[h])x.store(0);for(auto& x:latchGrip[h])x.store(0);
        latchNs[h].store(0);latchSerial[h].store(0);latchBranch[h].store(0);holdKind[h].store(0);joinNs[h].store(0);joinMm[h].store(0);
        heldNs[h].store(0);heldMm[h].store(0);}
    grabFirst.store(false);heavyMask.store(true);heavyNearM.store(.30f);chordGuard.store(true);heldLatch.store(true);chordGrabBusy.store(0);
    holdLatch.store(true);capturedMask.store(0);capturedNs.store(0);chordCapturedBusy.store(0);
    for(unsigned h=0;h<2;++h){lastEnd[h].store(0xff);chordZeroSamples[h].store(0);chordOwnedPresses[h].store(0);holdEndNativeLeak[h].store(0);holdEndRows[h].store(0);}
    {std::lock_guard lock(gripLatchMutex);gripLatchRing={};gripLatchNext=0;}}
} // namespace rdrvr::vr::grablink
