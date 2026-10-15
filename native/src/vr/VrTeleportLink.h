#pragma once
// TELEPORT locomotion: the LIGHT shared state between the teleport module (VrTeleportLoco.h, main
// thread) and everything else that must see it without pulling the module in:
//   * input thread (GameXr mapMovement / controlsPadFilter / holsterPadFilter): the stick route (owned / free / escape walk / native),
//     `ownsStick`, `walkModifier`, the stick sample seqlock for the main tick, `externalBusy` (lasso);
//   * warp link (main thread): the warp note -> VrCamera Rig::apply (filter + B snap, warp head key K, psi samples, bSnapWorldDelta),
//     VrBody hook A (head-lock / freeze snap), VrWeapon predictExtra (warp bridge);
//   * fade (BLINK): main -> XR command seqlock with heartbeat, XR -> main ONE 64-bit word {serial, alpha, flags} per xrEndFrame, the XR
//     pair id, the render thread's pair identity (first pair drawn from a head sample at/after K), the XR-side player (curves, watchdog,
//     abort latch) and its counters.
// Header-only, no engine, no Vulkan, no OpenXR (XrHarness chains the colour scale itself from xrFrame()).
#include "VrBodyLink.h" // body::Seqlock
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rdrvr::vr::tploco {
using LinkCounter=std::atomic<std::uint64_t>;
inline void linkBump(LinkCounter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline std::uint64_t linkGet(const LinkCounter& c) noexcept {return c.load(std::memory_order_relaxed);}

// ============================== input thread <-> main tick ==============================
// Where the movement stick goes this pad update:
//   Native   = lasso minigame focus: sticks native, LOCO and TELEPORT overrides suspended;
//   Owned    = TELEPORT aims: engine axes 0, LOCO stick 0, controls / holsters see a neutral stick;
//   Free     = the FREE path (TELEPORT module OFF / UNAVAILABLE / NO ARC, not on foot, lasso busy);
//   FreeWalk = the FREE path x walkScale (L3 escape walk, a long non-transient BLOCKED yields the stick).
enum class StickRoute : std::uint8_t {Native,Owned,Free,FreeWalk};
inline constexpr std::array<const char*,4> stickRouteNames{"NATIVE","OWNED","FREE","WALK"};
struct RouteIn {bool active{};bool ordinaryFoot{};bool l3{};bool lasso{};bool yield{};bool minigame{};};
inline StickRoute stickRoute(const RouteIn& i) noexcept {
    if(i.minigame)return StickRoute::Native;
    if(!i.active||!i.ordinaryFoot)return StickRoute::Free;
    if(i.l3)return StickRoute::FreeWalk;
    if(i.lasso)return StickRoute::Free;
    if(i.yield)return StickRoute::FreeWalk;
    return StickRoute::Owned;
}
// mapMovement -> main tick (pad update k is read by UpdateInputs k of the same Execute iteration).
struct StickSample {float x{},y{};std::uint64_t ns{};std::uint32_t actorGuid{};std::uint8_t l3{},ordinaryFoot{},world{},menu{},route{},teleport{};};
inline body::Seqlock<StickSample> stick;
inline std::atomic<bool> ownsStick{false};           // this pad update: TELEPORT owns the movement stick
inline std::atomic<bool> walkModifier{false};        // this pad update: L3 is the escape-walk modifier (VrControls: no sprint toggle)
inline std::atomic<std::uint32_t> externalBusy{0};   // bit0 lasso rope busy (FLYING / CAUGHT / RECOVER / minigame)
inline std::atomic<float> walkScale{.6f};            // `.walk` %
// main tick -> input thread (the state of the previous tick)
inline std::atomic<bool> moduleActive{false};        // state != OFF (installed, TELEPORT effective, arc usable)
inline std::atomic<bool> lassoGate{false};           // gate 4 closed: stick not owned, no warp
inline std::atomic<bool> minigameFocus{false};       // gate 4 minigame focus word >= 0: sticks native
inline std::atomic<bool> yieldStick{false};          // BLOCKED > 500 ms with a non-transient reason (SETTLING > 1.5 s)
inline LinkCounter stickSamples,ownedSamples,walkSamples,freeSamples,nativeSamples;
// mapMovement: publish one pad update. Returns the route (also stored in ownsStick / walkModifier).
inline StickRoute publishStick(float x,float y,bool l3,bool ordinaryFoot,bool world,bool menu,std::uint32_t actorGuid,bool teleport,std::uint64_t now) noexcept {
    RouteIn r{};r.active=teleport&&moduleActive.load(std::memory_order_relaxed);r.ordinaryFoot=ordinaryFoot;r.l3=l3;
    r.lasso=lassoGate.load(std::memory_order_relaxed);r.yield=yieldStick.load(std::memory_order_relaxed);
    r.minigame=teleport&&minigameFocus.load(std::memory_order_relaxed);
    const auto route=teleport?stickRoute(r):StickRoute::Free;
    StickSample s{};s.x=std::isfinite(x)?x:0.f;s.y=std::isfinite(y)?y:0.f;s.ns=now;s.actorGuid=actorGuid;s.l3=l3;s.ordinaryFoot=ordinaryFoot;
    s.world=world;s.menu=menu;s.route=std::uint8_t(route);s.teleport=teleport;stick.store(s);
    ownsStick.store(route==StickRoute::Owned,std::memory_order_relaxed);
    // walkModifier only while the module is active. In its FREE fallbacks (UNAVAILABLE - FREE, NO ARC, NO PROBE) L3 stays the FREE
    // path's sprint toggle (VrControls); a walk modifier there would swallow L3 and end every sprint.
    walkModifier.store(r.active&&ordinaryFoot&&!r.lasso&&!r.minigame,std::memory_order_relaxed);
    linkBump(stickSamples);
    linkBump(route==StickRoute::Owned?ownedSamples:route==StickRoute::FreeWalk?walkSamples:route==StickRoute::Native?nativeSamples:freeSamples);
    return route;
}
inline void clearStick() noexcept {ownsStick.store(false,std::memory_order_relaxed);walkModifier.store(false,std::memory_order_relaxed);}
// GameXr holsterPadFilter: while TELEPORT owns the movement stick the holsters' InputBrain sees it neutral (a
// copy with sticks[0..1] = 0: the fallback body yaw must not turn while aiming). Template: no ControllerInput.h here.
template<class Sample> inline const Sample* stickNeutralView(const Sample* in,Sample& scratch) noexcept {
    if(!in||!ownsStick.load(std::memory_order_relaxed))return in;
    scratch=*in;scratch.sticks[0]=scratch.sticks[1]=0.f;return &scratch;
}

// ============================== warp link (main thread) ==============================
namespace warp {
// TELEPORT tick, right after TeleportActorWithHeading: serial, time, the pre-warp capsule base C and the warp delta (world units).
struct Note {std::uint64_t serial{},ns{};float from[3]{},delta[3]{};std::uint8_t bsnap{1};};
inline body::Seqlock<Note> note;
// VrCamera Rig::apply (PostCameraUpdate): the snap of `serial` (first apply whose capsule moved >= min(0.5, 0.6 x |delta|) from `from`, toward the target, <= 500 ms), the warp
// head key K (anchor.sampledNs of that apply), psi before the warp (the last pre-warp apply) and the newest psi (+ apply count since the
// snap), the world shift of the LOCAL -> world map caused by the B snap.
struct CameraResult {std::uint64_t serial{},key{},psiNs{};float psiBefore{},psiNow{};float bSnap[3]{};std::uint32_t frames{};
                     std::uint8_t snapped{},psiValid{},bSnapped{};};
inline body::Seqlock<CameraResult> camera;
inline std::atomic<std::uint64_t> bodySerial{0};     // TELEPORT -> VrBody hook A: snap the head lock / freeze for this serial (until the head is post-snap)
inline std::atomic<std::uint64_t> bodySnapped{0};    // VrBody: the serial it snapped
inline LinkCounter cameraSnaps,cameraSnapExpired,bodySnaps,weaponBridged,weaponBridgeRejected;
// The player's own snap turns (GameXr mapHorizontalLook, main thread, the pad update before UpdateInputs; NOT the teleport's
// psi correction). A turn between the commit and FADE_IN is not psi drift: the HOLD neither undoes it nor learns a sign from it.
inline std::atomic<std::uint64_t> userTurns{0};
inline void noteUserTurn() noexcept {userTurns.fetch_add(1,std::memory_order_relaxed);}
inline void publishNote(std::uint64_t serial,std::uint64_t ns,const float from[3],const float delta[3],bool bsnap) noexcept {
    Note n{};n.serial=serial;n.ns=ns;for(unsigned i=0;i<3;++i){n.from[i]=from[i];n.delta[i]=delta[i];}n.bsnap=bsnap;note.store(n);
    bodySerial.store(serial,std::memory_order_release);
}
} // namespace warp

// ============================== fade (BLINK) ==============================
namespace fade {
enum Phase : std::uint8_t {None=0,Out=1,In=2};        // BLACK = the XR thread holds alpha 0 once OUT reached it
struct Command {std::uint64_t serial{},t0{},heartbeatNs{};std::uint32_t outMs{60},inMs{120};std::uint8_t phase{None};};
inline body::Seqlock<Command> command;               // main -> XR (heartbeat rewritten every tick while phase != None)
// XR -> main, ONE word per xrEndFrame: serial:32 | alphaQ:16 (alpha x 65535) | flags:16. A new serial never pairs with a stale alpha.
enum Flag : std::uint16_t {FlagExtension=1,FlagWorld=2,FlagAborted=4};
inline constexpr std::uint64_t pack(std::uint32_t serial,std::uint16_t alphaQ,std::uint16_t flags) noexcept {
    return (std::uint64_t(serial)<<32)|(std::uint64_t(alphaQ)<<16)|flags;}
inline constexpr std::uint32_t wordSerial(std::uint64_t w) noexcept {return std::uint32_t(w>>32);}
inline constexpr std::uint16_t wordAlpha(std::uint64_t w) noexcept {return std::uint16_t(w>>16);}
inline constexpr std::uint16_t wordFlags(std::uint64_t w) noexcept {return std::uint16_t(w);}
inline constexpr bool blackFor(std::uint64_t w,std::uint32_t serial) noexcept {return serial&&wordSerial(w)==serial&&wordAlpha(w)==0;}
inline std::atomic<std::uint64_t> xrWord{0};
inline std::atomic<std::uint64_t> xrPairId{0};       // renderFrameId of the newest stereo pair submitted (monotonic max)
inline std::atomic<int> extensionListed{-1},extensionEnabled{-1}; // XR_KHR_composition_layer_color_scale_bias (GameXr instance creation)
// Pair identity: the render thread (hands hook, every pair, before the pass's own gates) records the first renderFrameId whose
// plan head sample is >= the warp head key K that VrCamera published (warpKey, written before the anchor that carries K is published).
inline std::atomic<std::uint64_t> warpKey{0},firstSceneKey{0},firstScene{0},lastNoteNs{0};
inline LinkCounter pairNotes;
inline void notePair(std::uint64_t headNs,std::uint64_t sceneId,std::uint64_t now) noexcept {
    linkBump(pairNotes);lastNoteNs.store(now,std::memory_order_relaxed);
    const auto K=warpKey.load(std::memory_order_acquire);
    if(!K||!headNs||!sceneId||headNs<K||firstSceneKey.load(std::memory_order_relaxed)==K)return;
    firstScene.store(sceneId,std::memory_order_relaxed);firstSceneKey.store(K,std::memory_order_release);
}
// main: the scene id of the first pair drawn after the warp (0 = not seen yet)
inline std::uint64_t firstPostWarpScene(std::uint64_t K) noexcept {
    if(!K||firstSceneKey.load(std::memory_order_acquire)!=K)return 0;return firstScene.load(std::memory_order_relaxed);}

inline float smoothstep(float t) noexcept {t=std::clamp(t,0.f,1.f);return t*t*(3.f-2.f*t);}
// OUT 1 - smoothstep((now - t0) / out), IN smoothstep((now - t0) / in); `t0` and `now` on the same steady clock (main and XR).
inline float curve(std::uint8_t phase,std::uint64_t t0,std::uint32_t ms,std::uint64_t now) noexcept {
    const float t=ms?float(double(now>t0?now-t0:0)/(double(ms)*1e6)):1.f;
    return phase==Out?1.f-smoothstep(t):phase==In?smoothstep(t):1.f;
}
struct Stats {LinkCounter xrFrames,xrFramesScaled,watchdog,lateCommandIgnored,forcedWorld,cinemaDuringFade,blackFrames,commandReadRetries;};
inline Stats stats;
inline constexpr std::uint64_t heartbeatTimeoutNs=1'500'000'000ull,blackLimitNs=2'000'000'000ull;
struct XrOut {float alpha{1.f};bool active{};std::uint32_t serial{};std::uint16_t flags{};};
// XR thread only (single consumer). Phase None -> alpha 1; OUT -> curve then hold 0 (BLACK); IN -> curve. Presentation not world -> alpha 1.
// Watchdog: the heartbeat older than 1.5 s, or black for more than 2.0 s -> alpha 1 and `abortedSerial` = that serial: every later command
// of the serial is ignored (alpha stays 1: no dim-again flicker from a late IN), flag Aborted in the word.
class XrPlayer {
    std::uint64_t abortedSerial_{},blackSince_{},lateSerial_{},lateT0_{};std::uint8_t latePhase_{};
    Command last_{};bool haveLast_{};
public:
    XrOut frame(std::uint64_t now,bool world,bool extension) noexcept {Command c{};const bool ok=command.load(c);return play(ok?&c:nullptr,now,world,extension);}
    // `loaded` = this frame's command; nullptr = the seqlock read failed (never written, or the main thread's heartbeat rewrite kept racing its
    // 4 tries - e.g. preempted between the odd and the even sequence store). A failed read replays the last command read
    // (fade.commandReadRetries) instead of "no command" = alpha 1 for one frame (a flash of the old / new place mid-blink).
    XrOut play(const Command* loaded,std::uint64_t now,bool world,bool extension) noexcept {
        XrOut o{};linkBump(stats.xrFrames);
        Command c{};
        if(loaded){c=*loaded;last_=c;haveLast_=true;}
        else if(haveLast_){c=last_;linkBump(stats.commandReadRetries);}
        if(!c.serial){blackSince_=0;o.flags=std::uint16_t((extension?FlagExtension:0)|(world?FlagWorld:0));return o;}
        o.serial=std::uint32_t(c.serial);o.flags=std::uint16_t((extension?FlagExtension:0)|(world?FlagWorld:0));
        if(c.serial==abortedSerial_){
            if(c.phase!=None&&(c.serial!=lateSerial_||c.phase!=latePhase_||c.t0!=lateT0_)){
                lateSerial_=c.serial;latePhase_=c.phase;lateT0_=c.t0;if(c.phase==In)linkBump(stats.lateCommandIgnored);}
            o.flags|=FlagAborted;blackSince_=0;return o;}
        if(c.phase==None){blackSince_=0;return o;}
        if(!world){linkBump(stats.forcedWorld);blackSince_=0;return o;}
        const bool beat=c.heartbeatNs&&now>c.heartbeatNs&&now-c.heartbeatNs>heartbeatTimeoutNs;
        const bool longBlack=blackSince_&&now>blackSince_&&now-blackSince_>blackLimitNs;
        if(beat||longBlack){abortedSerial_=c.serial;blackSince_=0;linkBump(stats.watchdog);o.flags|=FlagAborted;return o;}
        float a=curve(c.phase,c.t0,c.phase==Out?c.outMs:c.inMs,now);
        if(c.phase==Out&&a<=1e-4f){a=0.f;if(!blackSince_)blackSince_=now;linkBump(stats.blackFrames);}
        else if(c.phase==In)blackSince_=0;
        o.alpha=std::clamp(a,0.f,1.f);o.active=o.alpha<1.f-1e-4f||c.phase==Out;
        if(o.active)linkBump(stats.xrFramesScaled);
        return o;
    }
    void reset() noexcept {abortedSerial_=blackSince_=lateSerial_=lateT0_=0;latePhase_=0;last_={};haveLast_=false;}
    std::uint64_t abortedSerial() const noexcept {return abortedSerial_;}
};
inline XrPlayer xrPlayer;
// XR thread, once per frame before xrEndFrame: the alpha to apply (colour scale (a, a, a, 1) on every submitted layer while `active`).
inline XrOut xrFrame(std::uint64_t now,bool world) noexcept {return xrPlayer.frame(now,world,extensionEnabled.load(std::memory_order_relaxed)==1);}
// XR thread, after a successful xrEndFrame: the word the main thread confirms black with; the newest stereo pair submitted.
inline void xrSubmitted(const XrOut& o,std::uint64_t pairId) noexcept {
    const auto q=std::uint16_t(std::lround(std::clamp(o.alpha,0.f,1.f)*65535.f));
    xrWord.store(pack(o.serial,q,o.flags),std::memory_order_release);
    if(pairId){auto cur=xrPairId.load(std::memory_order_relaxed);
        while(pairId>cur&&!xrPairId.compare_exchange_weak(cur,pairId,std::memory_order_release,std::memory_order_relaxed)){}}
}
inline void resetForTests() noexcept {
    command.store(Command{});xrWord.store(0);xrPairId.store(0);warpKey.store(0);firstSceneKey.store(0);firstScene.store(0);lastNoteNs.store(0);
    pairNotes.store(0);xrPlayer.reset();
    for(auto* c:{&stats.xrFrames,&stats.xrFramesScaled,&stats.watchdog,&stats.lateCommandIgnored,&stats.forcedWorld,&stats.cinemaDuringFade,&stats.blackFrames,
                 &stats.commandReadRetries})c->store(0);
}
} // namespace fade
} // namespace rdrvr::vr::tploco
