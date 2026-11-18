#pragma once
// KEEP GUN: a gun held in a VR hand stays in that hand when Dead Eye or a duel ends.
//
// Engine facts (librdr.so 1.58 arm64, vaddr, all.s):
//  - sagPlayer::UpdateWeaponSelection 0x269f320, select state 1 with an item in hand: two RDR1-mode auto-holsters, both skipped while
//    player+0x3bc bit1 (the mod's force aim) is set. 0x269fb78..0x269fbc8: with player input 8 in one state, RDR1 mode (bit3) set, bit1
//    clear and zoom (+0x244) not 1 or 2, it reaches PutAwayItem (0x269fc08 -> 0x269fd48) without reading the hold timer. 0x269fbd0:
//    bits & 0xa == 8 and the hold timer +0x3e0 <= 0. SetRDR1HoldMode 0x2698da4 (the keep) restarts +0x3e0 only while [[player+0x410]+9]
//    == 0 and does nothing for the first branch: an item in hand without bit1 can go back on the next UpdateWeaponSelection.
//    [[player+0x410]+9] is written by UpdateFoot 0x269df88 / UpdateTargetingState 0x269e38c: nonzero while input 25 is down or
//    sagPlayerStates+0x14 (written by UpdateZoomState 0x269baa0) is nonzero, so the keep VrHolsters.h makes before a bit1 drop is a no-op
//    on such a frame. With bit1 set a positive +0x3e0 is zeroed (0x269f6cc..0x269f6dc), at <= 0 it keeps counting down: an item held
//    under bit1 always meets an expired timer the frame bit1 drops. A third PutAwayItem (0x269f798, select state 4 with input 8 held)
//    ignores bit1 and the timer.
//  - sagPlayer::EndDeadEyeCommon 0x269733c, aniDuel::DuelCleanUp 0x216806c and sagPlayer::SetDeadEyeDueling 0x269d1e4 call neither
//    PutAwayItem nor HolsterItems; the duel machine 0x21658c0 only HolsterItems at RequestDuel 0x2166ccc (model only).
// The mod's side (VrHolsters.h): the poll drops bit1 when its gates read closed. When Dead Eye ends the fresh flag sagPlayerStates+0x20
// clears first, while the context block (re-read every 10th poll) and VrWeapon's force gate (published once per frame) can still read
// Dead Eye: the gates read closed for 1..10 polls exactly when the game leaves Dead Eye, and bit1 drops with the gun in the hand.
// What this header adds (state + pure decisions; the engine work stays in VrHolsters.h):
//  - TAIL: the mod's own bit1 stays set for tailNs after Dead Eye or a duel ends while the item stays in the VR hand (never newly set,
//    never while control is locked, dead or out of game); the poll re-reads the context block on those edges.
//  - RESTORE: an engine put-away of the VR-held firearm while Dead Eye runs or just ended, or in a duel's result phase or just after
//    the duel, while the holding grip never opened -> the same slot is drawn back into the same hand (instant draw, the usual OpDraw
//    gates). An opened grip, another item in that hand, a hand that took another job or restoreWaitNs without an open gate cancel it;
//    one restore per windowNs (the item put away again right after its restore stays in the holster); never against a script's force
//    holster (inventory flags 0x20 / 0x40); a put-away in the duel's result phase waits for the VR duel route to end (clockStart).
//  - WAITED INTENT: a HOLD release intent that had to wait for a closed gate (Dead Eye, cover, ...) runs only if the grip still reads
//    open when the gate opens; a grip back above the open threshold keeps the gun (RESQUEEZE needs gripClose).
// Levers (read at startup): debug.rdrvr.vr.holsters.keepgun 0 = all three off; .keepgun.restore 0 = no redraw;
// .keepgun.tailms <0..3000> (default 750, 0 = no tail). Report vr.holsters.keepGun.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace rdrvr::vr::keepgun {
using Counter=std::atomic<std::uint64_t>;
inline void bump(Counter& c) noexcept {c.fetch_add(1,std::memory_order_relaxed);}

inline std::atomic<bool> enabled{true},restoreOn{true};
inline std::atomic<std::uint64_t> tailNs{750'000'000ull};
inline constexpr std::uint64_t windowNs=2'000'000'000ull;      // an engine put-away this long after an end still counts as caused by it
inline constexpr std::uint64_t restoreWaitNs=4'000'000'000ull; // a restore waits this long for an open gate, then gives up
inline constexpr std::uint64_t supportWindowNs=1'000'000'000ull; // a support grip lost this long before the put-away comes back with the gun
inline constexpr std::uint64_t duelHoldCapNs=20'000'000'000ull; // the VR duel route holds a duel request's wait clock at most this long

enum Context : int {ContextNone,ContextDeadEye,ContextDeadEyeEnd,ContextDuel,ContextDuelEnd,ContextCount};
inline constexpr std::array<const char*,ContextCount> contextNames{"NONE","DEAD_EYE","DEAD_EYE_END","DUEL","DUEL_END"};
inline const char* contextName(int c) noexcept {return c>=0&&c<ContextCount?contextNames[std::size_t(c)]:"?";}

inline constexpr bool within(std::uint64_t now,std::uint64_t t,std::uint64_t span) noexcept {return t&&span&&now>=t&&now-t<span;}
// The context of a put-away at now: Dead Eye running, a duel in its result phase, or the more recent end within window.
inline constexpr Context contextAt(std::uint64_t now,bool deadEye,std::uint64_t eyeEndNs,bool duelResult,std::uint64_t duelEndNs,std::uint64_t window) noexcept {
    if(deadEye)return ContextDeadEye;
    if(duelResult)return ContextDuel;
    const bool e=within(now,eyeEndNs,window),d=within(now,duelEndNs,window);
    if(e&&d)return eyeEndNs>=duelEndNs?ContextDeadEyeEnd:ContextDuelEnd;
    return e?ContextDeadEyeEnd:d?ContextDuelEnd:ContextNone;
}
// TAIL: keep the mod's force aim this poll although the gates read closed.
inline constexpr bool tailKeeps(bool on,bool forceOurs,bool inHand,bool inGame,bool alive,bool locked,std::uint64_t now,std::uint64_t eyeEndNs,
                                std::uint64_t duelEndNs,std::uint64_t tail) noexcept {
    return on&&forceOurs&&inHand&&inGame&&alive&&!locked&&(within(now,eyeEndNs,tail)||within(now,duelEndNs,tail));
}
// WAITED INTENT: the release intent of a hand whose gate was closed keeps the gun when the grip reads above gripOpen once it opens.
inline constexpr bool waitedIntentKeeps(bool on,bool waited,float grip,float gripOpen) noexcept {return on&&waited&&grip>gripOpen;}

// RESTORE request: written by the main thread at the engine put-away, consumed by the input brain.
struct Request {std::uint32_t id{};int slot{-1},hand{-1};std::uint32_t holdSequence{};std::uint64_t ns{};int context{ContextNone};};
// What the brain sees of the hand the gun left.
struct HandFacts {
    bool holdMode{true};         // GripMode::Hold
    bool latched{},captured{};   // the press is still the one that held the gun (release() clears both on an open grip)
    std::uint32_t pressSequence{};
    bool otherJob{};             // the hand holds an item again, is the phantom / support hand
    bool poseValid{true};
};
enum Decision : int {DecideWait,DecidePost,DecideOpened,DecideBusy,DecideExpired,DecideCount};
inline constexpr std::array<const char*,DecideCount> decisionNames{"WAIT","POST","OPENED","BUSY","EXPIRED"};
// gateOpen = holsters context allowed, the slot grabbable, no Dead Eye, no duel route.
inline constexpr Decision decide(const Request& r,const HandFacts& f,bool gateOpen,std::uint64_t now,std::uint64_t wait) noexcept {
    if(f.holdMode&&(!f.latched||!f.captured||!f.pressSequence||f.pressSequence!=r.holdSequence))return DecideOpened;
    if(f.otherJob)return DecideBusy;
    if(now<r.ns||now-r.ns>wait)return DecideExpired;
    if(!gateOpen||!f.poseValid)return DecideWait;
    return DecidePost;
}

// The start of a request's wait clock: a put-away in the duel's result phase waits for the VR duel route to end (the result phase and the
// Dueling script's outro can outlast restoreWaitNs), so heldNs (the last frame the route still ran for that request, 0 = never) restarts
// the clock; cap bounds the whole wait from the put-away.
inline constexpr std::uint64_t clockStart(const Request& r,std::uint64_t heldNs,std::uint64_t now,std::uint64_t cap) noexcept {
    return r.context==ContextDuel&&heldNs>r.ns&&now>=r.ns&&now-r.ns<=cap?heldNs:r.ns;
}

// ---- the request slot (one at a time; a newer put-away replaces an older one) ----
inline std::mutex requestMutex;inline Request request{};inline std::atomic<std::uint32_t> requestId{},consumedId{};
inline void post(Request r) noexcept {
    std::lock_guard lock(requestMutex);r.id=requestId.load(std::memory_order_relaxed)+1;request=r;requestId.store(r.id,std::memory_order_release);}
inline bool pending(Request& out) noexcept {
    if(requestId.load(std::memory_order_acquire)==consumedId.load(std::memory_order_relaxed))return false;
    std::lock_guard lock(requestMutex);out=request;return out.id&&out.id!=consumedId.load(std::memory_order_relaxed);}
inline void consume(std::uint32_t id) noexcept {consumedId.store(id,std::memory_order_relaxed);}

// ---- counters + the last time a VR-held item left the hand (report) ----
inline Counter tailPolls,tailEdgeReads,deadEyeEnds,duelEnds,engineLeaves,engineLeavesDeadEye,engineLeavesDuel,requests,posted,
    opened,busy,expired,waitFrames,supportsRestored,waitedKept,offRequests,repeats,forcedHolsters,duelHeldFrames;
struct Leave {
    std::uint64_t ns{};const char* cause{"NONE"};const char* release{""};int context{ContextNone};int slot{-1},hand{-1};
    float grip{-1.f};double sinceReleaseMs{-1},sinceForceDropMs{-1},sinceDeadEyeEndMs{-1},sinceDuelEndMs{-1};int aimBits{-1};float timer{0.f};
    bool engine{},restore{};
};
inline std::mutex leaveMutex;inline Leave lastLeave{};inline std::atomic<const char*> lastDecision{"NONE"};
inline void noteLeave(const Leave& l) noexcept {std::lock_guard lock(leaveMutex);lastLeave=l;}
// Input brain: why the holding hand let go last (release / open-hand reason, its grip, when), per hand.
inline std::array<std::atomic<const char*>,2> releaseWhy{{{""},{""}}};
inline std::array<std::atomic<float>,2> releaseGrip{{{-1.f},{-1.f}}};
inline std::array<std::atomic<std::uint64_t>,2> releaseNs{};
inline std::array<std::atomic<float>,2> gripNow{{{-1.f},{-1.f}}}; // the brain's last grip value per hand (engine put-away forensics)
inline void noteRelease(int hand,const char* why,float grip,std::uint64_t now) noexcept {
    if(hand<0||hand>1)return;releaseWhy[std::size_t(hand)].store(why?why:"",std::memory_order_relaxed);
    releaseGrip[std::size_t(hand)].store(grip,std::memory_order_relaxed);releaseNs[std::size_t(hand)].store(now,std::memory_order_relaxed);}
inline double msSince(std::uint64_t now,std::uint64_t t) noexcept {return t&&now>=t?double(now-t)/1e6:-1.0;}

inline std::string report(std::uint64_t now) {
    Leave l{};{std::lock_guard lock(leaveMutex);l=lastLeave;}
    const auto u=[](const Counter& c){return (unsigned long long)c.load(std::memory_order_relaxed);};
    char b[1600];
    std::snprintf(b,sizeof b,"{\"on\":%s,\"restore\":%s,\"tailMs\":%.0f,\"tailPolls\":%llu,\"edgeReads\":%llu,\"deadEyeEnds\":%llu,\"duelEnds\":%llu,"
        "\"engineLeaves\":%llu,\"engineLeavesDeadEye\":%llu,\"engineLeavesDuel\":%llu,\"restores\":{\"requests\":%llu,\"posted\":%llu,\"opened\":%llu,"
        "\"busy\":%llu,\"expired\":%llu,\"off\":%llu,\"repeats\":%llu,\"forcedHolster\":%llu,\"duelHeldFrames\":%llu,\"waitFrames\":%llu,\"supports\":%llu,\"last\":\"%s\"},\"waitedKept\":%llu,"
        "\"lastLeave\":{\"cause\":\"%s\",\"release\":\"%s\",\"context\":\"%s\",\"engine\":%s,\"restore\":%s,\"ageMs\":%.0f,\"slot\":%d,\"hand\":%d,"
        "\"grip\":%.2f,\"sinceReleaseMs\":%.0f,\"sinceForceDropMs\":%.0f,\"sinceDeadEyeEndMs\":%.0f,\"sinceDuelEndMs\":%.0f,\"aimBits\":%d,\"timer\":%.3f}}",
        enabled.load()?"true":"false",restoreOn.load()?"true":"false",double(tailNs.load())/1e6,u(tailPolls),u(tailEdgeReads),u(deadEyeEnds),u(duelEnds),
        u(engineLeaves),u(engineLeavesDeadEye),u(engineLeavesDuel),u(requests),u(posted),u(opened),u(busy),u(expired),u(offRequests),u(repeats),u(forcedHolsters),u(duelHeldFrames),u(waitFrames),
        u(supportsRestored),lastDecision.load(),u(waitedKept),l.cause?l.cause:"",l.release?l.release:"",contextName(l.context),l.engine?"true":"false",
        l.restore?"true":"false",msSince(now,l.ns),l.slot,l.hand,double(l.grip),l.sinceReleaseMs,l.sinceForceDropMs,l.sinceDeadEyeEndMs,
        l.sinceDuelEndMs,l.aimBits,double(l.timer));
    return b;
}
} // namespace rdrvr::vr::keepgun
