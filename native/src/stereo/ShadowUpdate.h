#pragma once
// GRAPHICS row SHADOW UPDATE (EVERY / 1/2 / 1/3 / ADAPTIVE: how often the sun-shadow cascade atlas is
// re-rendered), the atlas skip while SUN SHADOWS is OFF, STEREO TEST 13 FREEZE SHADOW ATLAS, 14 RECEIVER NO CLOUDS,
// 15 RECEIVER RANGE CAP, and the sun-shadow diagnostics (receiver arguments, atlas CPU, skipped atlas frames).
// librdr 1.58 arm64 (llvm-objdump / llvm-readelf -r of librdr.so):
//  * sagRenderer::DrawShadowMaps @0x2a53ecc (render thread, inside RenderScene_RenderThread, before
//    DrawShadowMapsIntoCollector) calls rage::CShadowMap::RenderIntoShadowMaps(bool,Vector3&,bool,uint,bool) @0x2947168
//    through PLT 0x2c38900 = GOT 0x2e24ae0 (bl @0x2a54014, return 0x2a54018; x0 = the rmlShadowMap, vptr 0x2dbfe38 =
//    vtable 0x2dbfe28+0x10; x2 = sun direction). The only other PLT caller is the overload @0x29470f4 (bl @0x2947140),
//    passed through. After the call DrawShadowMaps still waits for every prepared cascade list / tree task
//    (renderer+0x6f4 bits -> WaitForVisibilityList 8..11), so a skipped call leaves no work dangling.
//  * RenderIntoShadowMaps: atlas Lock + Clear(1.0) + Unlock, +0x90 = sun dir, +0xf0..0x12f = camera of
//    grcViewport::sm_Current (+0x40, NormalizeSafe), then per tile (tile 0 skipped while +0x74 == 1.0):
//    CalcOrthoViewportAroundSphere (tile sphere tile+0x70, tile viewport), BeginTileAroundSphere @0x2946c08 through
//    GOT 0x2e24ad0 (single caller, return 0x2947464: saves sm_Current in +0x1d8, copies the four tile spheres into
//    CShadowMap+0x130..0x16f as {xyz, r^2}, writes the tile light matrix tile+0x10..0x4f = its only writer, locks the
//    atlas), the virtual RenderShadowCasters, Unlock, SetCurrent(+0x1d8).
//  * The main thread (CreateSceneVisibility -> CalculateShadowSpheres @0x2946b30, GetOrthoViewport -> tile+0x70)
//    rewrites the sphere block +0x130..0x16f every frame, concurrently with the render thread.
//  * Render-thread readers of +0x130..0x16f and of the tile matrices: BeginRenderShadowsIntoShadowCollector @0x2947890
//    (GOT 0x2e24af8; exact bl @0x2a54594 in DrawShadowMapsIntoCollector, return 0x2a54598; also writes the receiver
//    range +0x8c from the chosen sphere and the current camera) and SetShadowMapConstants @0x2947e58 (GOT 0x2e24b20;
//    exact bl @0x2a4fd94 in PreTransparentDrawCallback, return 0x2a4fd98). Other callers pass through.
// A skipped frame keeps the atlas and the tile matrices of the last rendered frame. That frame's 64-byte sphere block,
// copied right after its last BeginTileAroundSphere (exactly what the atlas was rendered with), is written back right
// before both uploads on the render thread: the same window in which the engine itself races the main thread.
// Receiver (renLightPrePassLightsMgr::RenderDirectionalShadow, exact bl @0x2a54940): s0 range = far-100 m when the
// cloud flag w2 is set, else CShadowMap::GetRange() = +0x8c; w5 tiles = [+0x28]-[+0x1c4].
//  * SHADOW UPDATE INDOOR (cfg value 4): while IndoorState.h says "in a room" the atlas renders only on camera
//    movement > 1 m, sun > 1 deg, view turn > 30 deg or every 500 ms (cut > 5 m, noPose, FREEZE, SUN OFF unchanged);
//    leaving the room forces a render on that frame; outside (or unknown) level 4 is exactly ADAPTIVE.
//  * CASCADE CULL NATIVE/x2/x4: CachedVisibility::sm_ShadowScreenSpaceThresholdCS2/CS3 (.data 0x2f7cfd0/0x2f7cfd4,
//    0.04/0.26; the only readers are the GOT loads of CachedVisibility::GetThreshold @0x2a6ec58 list 11 and @0x2a6ecb0
//    list 10, read live on every cascade visibility build; no writer in the binary) scaled x{1,2,4} by compare-and-restore
//    at the start of the exact RenderIntoShadowMaps (affects the next cascade visibility build). CS0/CS1 never touched.
//  * Tile measurement only: per-tile CPU (BeginTileAroundSphere timestamps inside the exact render) and caster draws per
//    tile (casterDraw() from the DrawCommon hook while insideAtlas), indexed by the cascade-cull level in force.
// Levels 5 STAGGER / 6 STAGGER FAR (one atlas tile per frame, per-tile snapshots, quadrant clears) live in
//    SunStagger.h (included below, before renderInto); report key shadowUpdate.stagger, byState _su5/_su6[x|a|n|l|o].
#include "IndoorState.h"
#include "StereoTestMode.h"
#include "SunNight.h" // SUN SHADOWS 7 STAGGER / NIGHT OFF (clock gate, report shadowUpdate.sunNight, signature "z")
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace rdrvr::engine::shadowupdate {
// ---- librdr 1.58 arm64 (module-relative) ----
inline constexpr std::uintptr_t renderIntoOffset=0x2947168,renderIntoGotOffset=0x2e24ae0,renderIntoReturnOffset=0x2a54018;
inline constexpr std::uintptr_t beginTileOffset=0x2946c08,beginTileGotOffset=0x2e24ad0,beginTileReturnOffset=0x2947464;
inline constexpr std::uintptr_t collectorOffset=0x2947890,collectorGotOffset=0x2e24af8,collectorReturnOffset=0x2a54598;
inline constexpr std::uintptr_t constantsOffset=0x2947e58,constantsGotOffset=0x2e24b20,constantsReturnOffset=0x2a4fd98;
inline constexpr std::uintptr_t shadowMapVtableOffset=0x2dbfe38;                                   // rage::rmlShadowMap
inline constexpr std::uintptr_t currentViewportOffset=0x4e6e14d8,currentViewportGotOffset=0x2de0958; // grcViewport::sm_Current
inline constexpr std::array<unsigned char,32> renderIntoFingerprint{
#include "gamecode/ShadowUpdate_1c75e0c_32_8d.inc"
};
inline constexpr std::array<unsigned char,32> beginTileFingerprint{
#include "gamecode/ShadowUpdate_2946c08_32_8d.inc"
};
inline constexpr std::array<unsigned char,32> collectorFingerprint{
#include "gamecode/ShadowUpdate_2947890_32_8d.inc"
};
inline constexpr std::array<unsigned char,32> constantsFingerprint{
#include "gamecode/ShadowUpdate_2947e58_32_8d.inc"
};
inline constexpr std::array<unsigned char,4> renderIntoCallFingerprint{59,146,7,148};  // bl 0x2c38900 @0x2a54014
inline constexpr std::array<unsigned char,4> beginTileCallFingerprint{32,197,11,148};  // bl 0x2c388e0 @0x2947460
inline constexpr std::array<unsigned char,4> collectorCallFingerprint{231,144,7,148};  // bl 0x2c38930 @0x2a54594
inline constexpr std::array<unsigned char,4> constantsCallFingerprint{251,162,7,148};  // bl 0x2c38980 @0x2a4fd94
inline constexpr std::uintptr_t sphereField=0x130,rangeField=0x8c,viewportCameraField=0x40;
inline constexpr std::size_t sphereBytes=0x40;
inline constexpr float rangeCapM=60.f; // STEREO TEST 15 RECEIVER RANGE CAP

// ---- levels (graphics.cfg shadowUpdate) ----
inline constexpr unsigned levels=5; // 4 INDOOR. decide() handles the skip levels 0..4 only.
inline constexpr unsigned menuLevels=8;
inline constexpr unsigned levelAdaptive=3,levelIndoor=4;
// 7 STAGGER / NIGHT OFF (SunNight.h) is stored as 7 in stats.level (report, _su7 signature) and acts as 5 STAGGER here and
// in SunStagger.h; at night VisibleTrims.h switches SUN SHADOWS off through GraphicsDrawPolicy (SUN OFF skip below).
inline constexpr unsigned levelNightOff=sunnight::rowValue;
inline constexpr unsigned actingLevel(unsigned v) noexcept {return v==levelNightOff?sunnight::dayLevel:v;}
inline const char* levelName(unsigned v) noexcept {
    return v==1?"1/2":v==2?"1/3":v==3?"ADAPTIVE":v==4?"INDOOR":v==5?"STAGGER":v==6?"STAGGER FAR":v==7?"STAGGER / NIGHT OFF":"EVERY";}
inline constexpr unsigned levelInterval(unsigned v) noexcept {return v==1?2u:v==2?3u:1u;}

// ---- pure policy (host-tested) ----
// indoor* apply to level 4 while the camera is in a room (indoorRotationDeg 0 disables the rotation rule).
struct Thresholds {float translationM{.25f},rotationDeg{2.f},sunDeg{.5f},cutM{5.f};unsigned adaptiveFrames{3};
                   float indoorTranslationM{1.f},indoorSunDeg{1.f},indoorRotationDeg{30.f};std::uint64_t indoorIntervalUs{500000};};
// Camera of grcViewport::sm_Current (Matrix34 rows b = up, c = forward axis, d = position) and the sun direction.
struct Pose {float position[3]{},forward[3]{},up[3]{},sun[3]{};bool valid{};};
// TrigStagger = a partial (one/two tile) render, TrigStaggerFull = a full render at a stagger level (the reason
// is in shadowUpdate.stagger.full); SkipStagger = an idle frame of the stagger indoor gate.
enum Trigger : unsigned {TrigEvery,TrigNoSnapshot,TrigForced,TrigInterval,TrigTranslation,TrigRotation,TrigSun,TrigCut,TrigNoPose,
                         TrigTimer,TrigIndoorExit,TrigStagger,TrigStaggerFull,TriggerCount};
enum SkipKind : unsigned {SkipInterval,SkipAdaptive,SkipFreeze,SkipSunOff,SkipIndoor,SkipStagger,SkipCount};
inline constexpr const char* triggerNames[TriggerCount]{"every","noSnapshot","forced","interval","translation","rotation","sun","cut","noPose",
                                                         "timer","indoorExit","stagger","staggerFull"};
inline constexpr const char* skipNames[SkipCount]{"interval","adaptive","freeze","sunOff","indoor","stagger"};
// indoor: the camera is in a room (IndoorState flag()==1).
struct Request {unsigned level{};bool freeze{},sunOff{};bool indoor{};};
struct Decision {bool render{};unsigned reason{};}; // Trigger when render, SkipKind otherwise
struct Delta {float translationM{},rotationDeg{},sunDeg{};bool valid{};};
inline float angleDeg(const float* a,const float* b) noexcept {
    const float la=std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]),lb=std::sqrt(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]);
    if(!(la>1e-6f)||!(lb>1e-6f))return 180.f;
    const float c=std::clamp((a[0]*b[0]+a[1]*b[1]+a[2]*b[2])/(la*lb),-1.f,1.f);
    return std::acos(c)*57.2957795f;
}
inline Delta delta(const Pose& from,const Pose& to) noexcept {
    if(!from.valid||!to.valid)return {};
    const float d[3]{to.position[0]-from.position[0],to.position[1]-from.position[1],to.position[2]-from.position[2]};
    Delta out{std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]),std::max(angleDeg(from.forward,to.forward),angleDeg(from.up,to.up)),angleDeg(from.sun,to.sun),true};
    out.valid=std::isfinite(out.translationM)&&std::isfinite(out.rotationDeg)&&std::isfinite(out.sunDeg);
    return out;
}
// skippedSince = atlas frames skipped since the last render. FREEZE and SUN OFF skip whenever a snapshot exists; a cut
// (> cutM) renders at 1/2, 1/3, ADAPTIVE and INDOOR as well; ADAPTIVE renders on movement or every adaptiveFrames-th
// frame. INDOOR (level 4) with r.indoor renders on indoor movement or once sinceRenderUs reaches indoorIntervalUs;
// level 4 without r.indoor is ADAPTIVE. sinceRenderUs (us since the last render, ~0 unknown) is read only for level 4.
inline Decision decide(const Request& r,bool snapshot,bool forced,unsigned skippedSince,const Pose& last,const Pose& now,const Thresholds& t={},
                       std::uint64_t sinceRenderUs=~0ull) noexcept {
    if(!snapshot)return {true,TrigNoSnapshot};
    if(r.freeze)return {false,SkipFreeze};
    if(r.sunOff)return {false,SkipSunOff};
    if(!r.level||r.level>=levels)return {true,TrigEvery};
    if(forced)return {true,TrigForced};
    const auto d=delta(last,now);
    if(!d.valid)return {true,TrigNoPose};
    if(d.translationM>t.cutM)return {true,TrigCut};
    if(r.level==levelIndoor&&r.indoor){
        if(d.translationM>t.indoorTranslationM)return {true,TrigTranslation};
        if(d.sunDeg>t.indoorSunDeg)return {true,TrigSun};
        if(t.indoorRotationDeg>0.f&&d.rotationDeg>t.indoorRotationDeg)return {true,TrigRotation};
        if(sinceRenderUs>=t.indoorIntervalUs)return {true,TrigTimer};
        return {false,SkipIndoor};
    }
    if(r.level!=levelAdaptive&&r.level!=levelIndoor)return skippedSince+1>=levelInterval(r.level)?Decision{true,TrigInterval}:Decision{false,SkipInterval};
    if(d.translationM>t.translationM)return {true,TrigTranslation};
    if(d.rotationDeg>t.rotationDeg)return {true,TrigRotation};
    if(d.sunDeg>t.sunDeg)return {true,TrigSun};
    if(skippedSince+1>=t.adaptiveFrames)return {true,TrigInterval};
    return {false,SkipAdaptive};
}
// Render-thread bookkeeping of the atlas snapshot (no atomics: every user runs on the render thread, checked by token).
struct Tracker {
    std::array<unsigned char,sphereBytes> spheres{},pending{};
    const void* owner{};const void* pendingMap{};
    Pose pose{};
    unsigned skippedSince{};
    bool valid{},pendingValid{},skippedFrame{},forceNext{};
    std::uint64_t renderedUs{}; // clockUs() of the last render (0 unknown), INDOOR timer
    void beginRender() noexcept {pendingValid=false;}
    void tileBegun(const void* map,const void* block) noexcept {std::memcpy(pending.data(),block,sphereBytes);pendingMap=map;pendingValid=true;}
    // Returns false when the block had to be read after the whole render (no BeginTileAroundSphere was seen).
    bool rendered(const void* map,const void* blockNow,const Pose& p,std::uint64_t us=0) noexcept {
        const bool exact=pendingValid&&pendingMap==map;
        std::memcpy(spheres.data(),exact?pending.data():static_cast<const unsigned char*>(blockNow),sphereBytes);
        owner=map;valid=true;pose=p;skippedSince=0;skippedFrame=false;forceNext=false;pendingValid=false;renderedUs=us;return exact;
    }
    void skipped(bool forceAfter) noexcept {if(skippedSince<(1u<<30))++skippedSince;skippedFrame=true;forceNext=forceNext||forceAfter;}
    bool restore(const void* map,void* block) const noexcept {
        if(!skippedFrame||!valid||map!=owner)return false;
        std::memcpy(block,spheres.data(),sphereBytes);return true;
    }
    void invalidate() noexcept {valid=false;skippedFrame=false;pendingValid=false;}
};

// Eighth-octave buckets of an unsigned value (+-4.5 % around the reported midpoint; exact below 7); add() is one
// relaxed fetch_add (hot path once per frame). Index 7 is never used (x = v+1 = 8 starts the octave buckets at 8).
class LogHistogram {
public:
    static constexpr unsigned buckets=160; // up to ~2^21 (us: ~4 s, dm: ~400 km); larger values land in the last bucket
    static constexpr unsigned bucket(std::uint64_t v) noexcept {
        const std::uint64_t x=v+1;const unsigned msb=unsigned(std::bit_width(x))-1;
        const unsigned i=msb<3?unsigned(x)-1:8*(msb-2)+unsigned((x>>(msb-3))&7u);
        return i<buckets?i:buckets-1;
    }
    static constexpr std::uint64_t lower(unsigned i) noexcept {
        if(i<8)return i;
        const unsigned msb=i/8+2,q=i%8;return ((std::uint64_t(8+q))<<(msb-3))-1;
    }
    static constexpr unsigned next(unsigned i) noexcept {return i==6?8:i+1;}
    void add(std::uint64_t v) noexcept {counts_[bucket(v)].fetch_add(1,std::memory_order_relaxed);}
    std::uint64_t count() const noexcept {std::uint64_t n{};for(const auto& c:counts_)n+=c.load(std::memory_order_relaxed);return n;}
    // Nearest-rank percentile, reported as the bucket midpoint (value units); -1 when empty.
    double percentile(double p) const noexcept {
        std::array<std::uint64_t,buckets> c{};std::uint64_t n{};
        for(unsigned i=0;i<buckets;++i){c[i]=counts_[i].load(std::memory_order_relaxed);n+=c[i];}
        if(!n)return -1.;
        const auto rank=std::max<std::uint64_t>(1,std::uint64_t(std::ceil(p*double(n))));std::uint64_t seen{};
        for(unsigned i=0;i<buckets;++i){seen+=c[i];if(seen>=rank){
            if(next(i)>=buckets)return double(lower(i));
            return (double(lower(i))+double(lower(next(i))))*.5;}}
        return double(lower(buckets-1));
    }
private:
    std::array<std::atomic<std::uint32_t>,buckets> counts_{};
};

// ---- runtime state ----
struct Config {std::uintptr_t shadowVtable{},currentViewport{},renderReturn{},beginTileReturn{},collectorReturn{},constantsReturn{};};
inline Config config; // written once while the hooks are installed (before the slots go live)
using RenderInto=void(*)(void*,bool,void*,bool,unsigned,bool);
using BeginTile=void(*)(void*,int,bool);
using Collector=void(*)(void*,bool,bool,bool);
using Constants=void(*)(void*);
inline RenderInto originalRenderInto{};
inline BeginTile originalBeginTile{};
inline Collector originalCollector{};
inline Constants originalConstants{};
// EVERY, 1/2, 1/3, ADAPTIVE, INDOOR, STAGGER, STAGGER FAR, FREEZE (STEREO TEST 13), SUN OFF
inline constexpr unsigned stateCount=9;
inline const char* stateName(unsigned s) noexcept {return s==7?"FREEZE":s==8?"SUN OFF":levelName(s);}
inline constexpr unsigned stateOf(const Request& r) noexcept {return r.freeze?7u:r.sunOff?8u:std::min(r.level,6u);}
// indoor*: the frames of this state on which the camera was in a room (Request::indoor), whatever the level.
struct StateStats {std::atomic<std::uint64_t> frames{},rendered{},cpuUsSum{},indoorFrames{},indoorRendered{},indoorCpuUsSum{};LogHistogram cpuUs;};
inline constexpr std::uintptr_t cs2Offset=0x2f7cfd0,cs3Offset=0x2f7cfd4;       // CachedVisibility::sm_ShadowScreenSpaceThresholdCS2/CS3 (.data)
inline constexpr std::uintptr_t cs2GotOffset=0x2e10e08,cs3GotOffset=0x2e10de8; // R_AARCH64_GLOB_DAT
inline constexpr std::array<std::uint32_t,2> cascadeCullNativeBits{0x3d23d70au,0x3e851eb8u}; // 0.04f, 0.26f
inline constexpr std::uintptr_t cullList11CodeOffset=0x2a6ec54; // GetThreshold list 11: adrp x9; ldr x9,[x9,#0xde8] (GOT CS3)
inline constexpr std::array<unsigned char,8> cullList11CodeFingerprint{9,29,0,208,41,245,70,249};
inline constexpr std::uintptr_t cullList10CodeOffset=0x2a6ecac; // GetThreshold list 10: adrp x9; ldr x9,[x9,#0xe08] (GOT CS2); ldr s1,[x9]
inline constexpr std::array<unsigned char,12> cullList10CodeFingerprint{9,29,0,208,41,5,71,249,33,1,64,189};
inline constexpr unsigned cascadeCullLevels=3;
inline const char* cascadeCullName(unsigned l) noexcept {return l==1?"x2":l==2?"x4":"NATIVE";}
inline constexpr float cascadeCullScale(unsigned l) noexcept {return l==1?2.f:l==2?4.f:1.f;}
// Value the level writes into CS2 (i 0) / CS3 (i 1): native x {1,2,4} (exact: power-of-two scale).
inline constexpr std::uint32_t cascadeCullBits(unsigned i,unsigned level) noexcept {
    return std::bit_cast<std::uint32_t>(std::bit_cast<float>(cascadeCullNativeBits[i&1u])*cascadeCullScale(level));
}
struct CullState {
    std::array<std::uintptr_t,2> address{};             // absolute CS2/CS3 addresses (written before ready)
    std::atomic<bool> ready{};                           // GOT + code + native values proven
    std::atomic<const char*> fingerprint{"unconfigured"};// "ok", "got", "code" or "value"
    std::atomic<std::uint64_t> applyCalls{},writes{},restores{},foreign{};
    std::atomic<bool> suspended{};                       // a foreign value is in place: nothing written until it is gone
    std::atomic<unsigned> effective{};                   // level whose values are in memory now (0 when suspended)
    std::array<std::atomic<std::uint32_t>,2> appliedBits{}; // CS2/CS3 as last seen/written by apply (0 = never)
    std::array<std::uint32_t,2> lastWritten=cascadeCullNativeBits; // render thread only
};
inline CullState cull;
// ---- tile measurement ----
struct TileStats {std::atomic<std::uint64_t> begins{},usSum{},casters{};std::atomic<std::uint32_t> castersMax{};LogHistogram us;};
struct TileProbe { // render thread only, reset for each exact atlas render
    std::array<std::uint64_t,4> tileStartUs{};std::array<std::uint32_t,4> tileCasters{};
    std::uint32_t preTileCasters{};int currentTile{-1};unsigned begunMask{};
};
struct ReceiverStats {
    std::atomic<std::uint64_t> calls{},cloud{},noCloud{},forcedNoCloud{},capped{};
    std::array<std::atomic<std::uint64_t>,6> tiles{}; // tiles argument 0..4, [5] = other
    LogHistogram rangeCloudDm,rangeNoCloudDm;         // range argument as the engine passed it (decimetres)
    std::atomic<float> lastRange{-1.f},cascadeRange{-1.f};
};
struct Stats {
    std::atomic<unsigned> level{};                    // SHADOW UPDATE, stored at every RenderScene
    std::atomic<bool> hooked{};
    std::atomic<std::uint64_t> calls{},declined{},foreignCalls{},foreignRenders{},rendered{},skipped{},tileBegins{},snapshotFallback{};
    std::atomic<std::uint64_t> collectorCalls{},constantsCalls{},restoresCollector{},restoresConstants{},restoreMisses{},foreignThread{},nativeRace{};
    std::array<std::atomic<std::uint64_t>,TriggerCount> triggers{};
    std::array<std::atomic<std::uint64_t>,SkipCount> skips{};
    std::array<StateStats,stateCount> byState{};
    LogHistogram cpuUs;std::atomic<std::uint64_t> cpuUsSum{};
    std::atomic<std::uint64_t> receiverOffScene{};    // scene serial in which the receiver was last suppressed (SUN SHADOWS OFF)
    std::atomic<float> lastTranslationM{-1.f},lastRotationDeg{-1.f},lastSunDeg{-1.f};
    ReceiverStats receiver;
    std::atomic<unsigned> cullLevel{};                // CASCADE CULL request (0..2), stored by TraceEntry
    std::array<std::array<TileStats,4>,cascadeCullLevels> tiles{}; // Per-tile stats [cull level in force][tile]
    std::atomic<std::uint64_t> probedRenders{},setupRenders{},setupUsSum{},preTileCasters{};
};
inline Stats stats;
inline Tracker tracker;                               // render thread only
inline Thresholds thresholds;                         // read on the render thread (defaults; no writer)
inline TileProbe tileProbe;                           // render thread only
inline std::atomic<bool> insideAtlas{};               // set around the exact native RenderIntoShadowMaps (render thread)
inline thread_local char threadToken{};               // its address identifies the calling thread
inline std::atomic<const void*> renderThread{};       // &threadToken of the thread of the exact RenderIntoShadowMaps call
inline std::atomic<bool> foreignRender{};             // the atlas was rendered on another thread: the snapshot is stale
inline bool onRenderThread() noexcept {return renderThread.load(std::memory_order_relaxed)==&threadToken;}
inline bool shadowMap(const void* map) noexcept {
    if(!map||!config.shadowVtable)return false;std::uintptr_t v{};std::memcpy(&v,map,sizeof v);return v==config.shadowVtable;
}
inline unsigned char* sphereBlock(void* map) noexcept {return static_cast<unsigned char*>(map)+sphereField;}
inline Pose readPose(const void* sun) noexcept {
    Pose p{};const void* viewport{};
    if(config.currentViewport)std::memcpy(&viewport,reinterpret_cast<const void*>(config.currentViewport),sizeof viewport);
    if(!viewport||!sun)return p;
    float m[16]{};std::memcpy(m,static_cast<const unsigned char*>(viewport)+viewportCameraField,sizeof m);
    for(int i=0;i<3;++i){p.up[i]=m[4+i];p.forward[i]=m[8+i];p.position[i]=m[12+i];}
    std::memcpy(p.sun,sun,sizeof p.sun);
    bool finite=true;for(int i=0;i<3;++i)finite=finite&&std::isfinite(p.up[i])&&std::isfinite(p.forward[i])&&std::isfinite(p.position[i])&&std::isfinite(p.sun[i]);
    p.valid=finite;return p;
}
// SUN SHADOWS OFF skips the atlas only while the receiver really is suppressed (this or the previous RenderScene).
inline void receiverSuppressed(std::uint64_t scene) noexcept {stats.receiverOffScene.store(scene,std::memory_order_relaxed);}
inline bool receiverRecentlyOff(std::uint64_t scene) noexcept {const auto off=stats.receiverOffScene.load(std::memory_order_relaxed);return off&&off+1>=scene;}
inline std::uint64_t nowUs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline std::uint64_t(*clockUs)()=nowUs; // Every timestamp of renderInto/beginTile (host tests inject a clock)

// ---- CASCADE CULL ----
// Both GOT slots must point at CS2/CS3, both GetThreshold loads must be in place and both values must be the native ones.
template<class Code,class Read> void configureCascadeCull(std::uintptr_t base,Code code,Read read) {
    const auto got=[&](std::uintptr_t slot,std::uintptr_t target){
        std::uintptr_t value{};return read(nullptr,base+slot,&value,sizeof value)&&value==base+target;};
    const auto native=[&](std::uintptr_t offset,std::uint32_t bits){
        std::uint32_t value{};return read(nullptr,base+offset,&value,sizeof value)&&value==bits;};
    const char* result="ok";
    if(!got(cs2GotOffset,cs2Offset)||!got(cs3GotOffset,cs3Offset))result="got";
    else if(!code(cullList11CodeOffset,cullList11CodeFingerprint)||!code(cullList10CodeOffset,cullList10CodeFingerprint))result="code";
    else if(!native(cs2Offset,cascadeCullNativeBits[0])||!native(cs3Offset,cascadeCullNativeBits[1]))result="value";
    const bool ok=result[0]=='o';
    cull.address={base+cs2Offset,base+cs3Offset};cull.lastWritten=cascadeCullNativeBits;
    cull.fingerprint.store(result,std::memory_order_relaxed);cull.ready.store(ok,std::memory_order_release);
}
// Menu availability (runtime::cascadeCullReady): proven and the exact RenderIntoShadowMaps hook is live.
inline bool cascadeCullConfigured() noexcept {return cull.ready.load(std::memory_order_acquire)&&stats.hooked.load(std::memory_order_relaxed);}
// Level whose CS2/CS3 values are in memory now (gpuStages signature _cc<n>); 0 when not ready, suspended or NATIVE.
inline unsigned cascadeCullEffective() noexcept {return cull.effective.load(std::memory_order_relaxed);}
// 32-bit word of engine .data (CS2/CS3): relaxed atomic access. std::atomic_ref where the library has it (MSVC host
// tests); the NDK 27 libc++ does not, so the compiler builtins there.
inline std::uint32_t loadWord(std::uintptr_t address) noexcept {
#if defined(__cpp_lib_atomic_ref)
    return std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(address)).load(std::memory_order_relaxed);
#else
    return __atomic_load_n(reinterpret_cast<std::uint32_t*>(address),__ATOMIC_RELAXED);
#endif
}
inline bool exchangeWord(std::uintptr_t address,std::uint32_t& expected,std::uint32_t desired) noexcept {
#if defined(__cpp_lib_atomic_ref)
    return std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(address)).compare_exchange_strong(expected,desired,std::memory_order_relaxed);
#else
    return __atomic_compare_exchange_n(reinterpret_cast<std::uint32_t*>(address),&expected,desired,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
#endif
}
// Start of the exact RenderIntoShadowMaps (render thread). Compare-and-restore: a slot is written only while it holds
// the native value or the mod's last write; anything else is foreign -> suspended (the mod's own
// values go back to native, the foreign one is never touched) until the foreign value is gone.
// Returns the level in force.
inline unsigned applyCascadeCull() noexcept {
    auto& c=cull;
    if(!c.ready.load(std::memory_order_acquire))return 0;
    c.applyCalls.fetch_add(1,std::memory_order_relaxed);
    unsigned level=stats.cullLevel.load(std::memory_order_relaxed);if(level>=cascadeCullLevels)level=0;
    std::array<std::uint32_t,2> now{};bool foreign=false;
    for(unsigned i=0;i<2;++i){
        now[i]=loadWord(c.address[i]);
        if(now[i]!=cascadeCullNativeBits[i]&&now[i]!=c.lastWritten[i]&&now[i]!=cascadeCullBits(i,level))foreign=true;
    }
    const auto write=[&](unsigned i,std::uint32_t want){
        std::uint32_t expected=now[i];
        if(exchangeWord(c.address[i],expected,want)){
            c.lastWritten[i]=want;now[i]=want;(want==cascadeCullNativeBits[i]?c.restores:c.writes).fetch_add(1,std::memory_order_relaxed);
        }else now[i]=expected; // changed concurrently: judged again next frame
    };
    if(foreign){
        if(!c.suspended.exchange(true,std::memory_order_relaxed))c.foreign.fetch_add(1,std::memory_order_relaxed);
        for(unsigned i=0;i<2;++i)if(now[i]==c.lastWritten[i]&&now[i]!=cascadeCullNativeBits[i])write(i,cascadeCullNativeBits[i]);
    }else{
        c.suspended.store(false,std::memory_order_relaxed);
        for(unsigned i=0;i<2;++i)if(now[i]!=cascadeCullBits(i,level))write(i,cascadeCullBits(i,level));
    }
    for(unsigned i=0;i<2;++i)c.appliedBits[i].store(now[i],std::memory_order_relaxed);
    const unsigned effective=!foreign&&now[0]==cascadeCullBits(0,level)&&now[1]==cascadeCullBits(1,level)?level:0;
    c.effective.store(effective,std::memory_order_relaxed);
    return effective;
}

// ---- tile measurement ----
// DrawCommon hook (TraceEntry bodyDraw, every thread): one relaxed load unless the exact atlas render is running; the
// thread-local check runs only then, and only the render thread counts (per tile begun so far).
inline void casterDraw() noexcept {
    if(!insideAtlas.load(std::memory_order_relaxed))return;
    if(!onRenderThread())return;
    auto& p=tileProbe;
    if(p.currentTile>=0&&p.currentTile<4)++p.tileCasters[unsigned(p.currentTile)];else ++p.preTileCasters;
}
// After the exact native render: tile duration = next started tile's start (or the end) - its own start.
// drawMask = the tiles a partial render really drew (masked tiles still bound the others' durations).
inline void foldTiles(unsigned level,std::uint64_t entryUs,std::uint64_t endUs,unsigned drawMask=0xfu) noexcept {
    auto& p=tileProbe;auto& s=stats;
    s.probedRenders.fetch_add(1,std::memory_order_relaxed);s.preTileCasters.fetch_add(p.preTileCasters,std::memory_order_relaxed);
    if(!p.begunMask)return;
    auto& row=s.tiles[level<cascadeCullLevels?level:0];
    std::uint64_t first=~0ull;
    for(unsigned t=0;t<4;++t){
        if(!(p.begunMask&(1u<<t)))continue;
        const auto start=p.tileStartUs[t];first=std::min(first,start);
        if(!(drawMask&(1u<<t)))continue;
        std::uint64_t next=endUs;
        for(unsigned u=0;u<4;++u){
            if(u==t||!(p.begunMask&(1u<<u)))continue;
            const auto other=p.tileStartUs[u];
            if(other<start||(other==start&&u<t))continue;
            next=std::min(next,other);
        }
        const std::uint64_t us=next>start?next-start:0;
        auto& ts=row[t];ts.begins.fetch_add(1,std::memory_order_relaxed);ts.usSum.fetch_add(us,std::memory_order_relaxed);ts.us.add(us);
        ts.casters.fetch_add(p.tileCasters[t],std::memory_order_relaxed);
        if(p.tileCasters[t]>ts.castersMax.load(std::memory_order_relaxed))ts.castersMax.store(p.tileCasters[t],std::memory_order_relaxed);
    }
    s.setupRenders.fetch_add(1,std::memory_order_relaxed);s.setupUsSum.fetch_add(first>entryUs?first-entryUs:0,std::memory_order_relaxed);
}

} // namespace rdrvr::engine::shadowupdate
#include "SunStagger.h" // Levels 5 STAGGER / 6 STAGGER FAR (needs everything above)
namespace rdrvr::engine::shadowupdate {

// GOT replacement of RenderIntoShadowMaps (caller = return address of the call, request built by the caller).
inline void renderInto(void* map,bool a,void* sun,bool b,unsigned c,bool d,std::uintptr_t caller,const Request& request) noexcept {
    auto& s=stats;s.calls.fetch_add(1,std::memory_order_relaxed);
    const bool isMap=shadowMap(map);
    if(!isMap||caller!=config.renderReturn){
        // Other callers keep native behaviour; their render replaces the atlas, so the snapshot follows it.
        (isMap?s.foreignCalls:s.declined).fetch_add(1,std::memory_order_relaxed);
        const bool mine=isMap&&onRenderThread();
        if(mine)tracker.beginRender();
        originalRenderInto(map,a,sun,b,c,d);
        if(mine){if(!tracker.rendered(map,sphereBlock(map),readPose(sun),clockUs()))s.snapshotFallback.fetch_add(1,std::memory_order_relaxed);
            stagger::foreignRendered();} // The tile snapshots are stale
        else if(isMap){foreignRender.store(true,std::memory_order_relaxed);s.foreignRenders.fetch_add(1,std::memory_order_relaxed);}
        return;
    }
    renderThread.store(&threadToken,std::memory_order_relaxed);
    if(foreignRender.exchange(false,std::memory_order_relaxed)){tracker.invalidate();stagger::foreignRendered();}
    const unsigned cullLevel=applyCascadeCull(); // CS2/CS3 for the next cascade visibility builds
    const Pose now=readPose(sun);
    // One indoor sample per frame at every level (menu label, signature, agreement stats); INDOOR acts
    // only with the hooks live and flag()==1. A room exit forces the render of this frame at level 4.
    indoor::sample(now.valid?now.position:nullptr);
    Request req=request;req.indoor=s.hooked.load(std::memory_order_relaxed)&&indoor::flag()==1;
    req.level=actingLevel(req.level); // 7 NIGHT OFF renders as STAGGER (night = SUN OFF via the draw policy)
    // STAGGER / STAGGER FAR. Idle (indoor gate) and partial frames end here; a full frame falls through with the
    // render forced (TrigStaggerFull) and learning on; unavailable / disabled / OFF renders EVERY. Stagger consumes the
    // room exit itself (its gate ends at once, no forced render).
    bool staggerFull=false;
    if(stagger::isLevel(req.level)&&!req.freeze&&!req.sunOff){
        const auto k=stagger::route(map,req,now);
        if(k==stagger::Idle||k==stagger::Partial){
            auto& st=s.byState[stateOf(req)];st.frames.fetch_add(1,std::memory_order_relaxed);
            if(req.indoor)st.indoorFrames.fetch_add(1,std::memory_order_relaxed);
            if(k==stagger::Idle){
                stagger::idle();
                s.skipped.fetch_add(1,std::memory_order_relaxed);s.skips[SkipStagger].fetch_add(1,std::memory_order_relaxed);return;}
            const auto us=stagger::renderPartial(map,a,sun,b,c,d,cullLevel);
            s.rendered.fetch_add(1,std::memory_order_relaxed);s.triggers[TrigStagger].fetch_add(1,std::memory_order_relaxed);
            s.cpuUs.add(us);s.cpuUsSum.fetch_add(us,std::memory_order_relaxed);
            st.rendered.fetch_add(1,std::memory_order_relaxed);st.cpuUs.add(us);st.cpuUsSum.fetch_add(us,std::memory_order_relaxed);
            if(req.indoor){st.indoorRendered.fetch_add(1,std::memory_order_relaxed);st.indoorCpuUsSum.fetch_add(us,std::memory_order_relaxed);}
            return;
        }
        if(k==stagger::Native)req.level=0;else staggerFull=true;
    }else if(stagger::leave())tracker.invalidate(); // the atlas was mixed-time: the first frame here renders
    const bool exitForce=indoor::consumeExit()&&req.level==levelIndoor;
    const auto delta_=delta(tracker.pose,now);
    if(delta_.valid){s.lastTranslationM.store(delta_.translationM,std::memory_order_relaxed);s.lastRotationDeg.store(delta_.rotationDeg,std::memory_order_relaxed);s.lastSunDeg.store(delta_.sunDeg,std::memory_order_relaxed);}
    const bool snapshot=tracker.valid&&tracker.owner==map;
    const bool forceNext=tracker.forceNext&&!req.sunOff&&!req.freeze;
    const auto decisionUs=clockUs();
    const std::uint64_t since=tracker.renderedUs&&decisionUs>=tracker.renderedUs?decisionUs-tracker.renderedUs:~0ull;
    auto decision=decide(req,snapshot,forceNext||exitForce,tracker.skippedSince,tracker.pose,now,thresholds,since);
    if(decision.render&&decision.reason==TrigForced&&exitForce&&!forceNext)decision.reason=TrigIndoorExit;
    if(staggerFull)decision={true,TrigStaggerFull}; // route() decided (reason in stagger.full)
    auto& state=s.byState[stateOf(req)];state.frames.fetch_add(1,std::memory_order_relaxed);
    if(req.indoor)state.indoorFrames.fetch_add(1,std::memory_order_relaxed);
    if(!decision.render){
        tracker.skipped(req.freeze||req.sunOff);
        s.skipped.fetch_add(1,std::memory_order_relaxed);s.skips[decision.reason<SkipCount?decision.reason:0].fetch_add(1,std::memory_order_relaxed);
        return;
    }
    tracker.beginRender();
    tileProbe=TileProbe{};
    if(staggerFull)stagger::fullBegin(map); // Learning (extent, clear value, quadrants) + snapshot of every tile
    const auto t0=clockUs();
    insideAtlas.store(true,std::memory_order_relaxed);
    originalRenderInto(map,a,sun,b,c,d);
    insideAtlas.store(false,std::memory_order_relaxed);
    const auto t1=clockUs();const auto us=t1>t0?t1-t0:0;
    if(staggerFull)stagger::fullEnd(map);
    foldTiles(cullLevel,t0,t1);
    if(!tracker.rendered(map,sphereBlock(map),now,t0?t0:1))s.snapshotFallback.fetch_add(1,std::memory_order_relaxed);
    s.rendered.fetch_add(1,std::memory_order_relaxed);s.triggers[decision.reason<TriggerCount?decision.reason:0].fetch_add(1,std::memory_order_relaxed);
    s.cpuUs.add(us);s.cpuUsSum.fetch_add(us,std::memory_order_relaxed);
    state.rendered.fetch_add(1,std::memory_order_relaxed);state.cpuUs.add(us);state.cpuUsSum.fetch_add(us,std::memory_order_relaxed);
    if(req.indoor){state.indoorRendered.fetch_add(1,std::memory_order_relaxed);state.indoorCpuUsSum.fetch_add(us,std::memory_order_relaxed);}
}
// GOT replacement of BeginTileAroundSphere: the sphere block it just wrote is what this tile pass renders with.
// Inside the exact atlas render the tile start is stamped before the native call.
// stagger::before/afterBeginTile (exact caller, render thread) mask a tile (old matrix back) or snapshot it.
inline void beginTile(void* map,int tile,bool b,std::uintptr_t caller) noexcept {
    if(insideAtlas.load(std::memory_order_relaxed)&&tile>=0&&tile<4&&onRenderThread()){
        auto& p=tileProbe;p.tileStartUs[unsigned(tile)]=clockUs();p.begunMask|=1u<<unsigned(tile);p.currentTile=tile;}
    const bool exact=caller==config.beginTileReturn&&map&&onRenderThread();
    if(exact)stagger::beforeBeginTile(map,tile);
    originalBeginTile(map,tile,b);
    if(!exact)return;
    tracker.tileBegun(map,sphereBlock(map));stats.tileBegins.fetch_add(1,std::memory_order_relaxed);
    stagger::afterBeginTile(map,tile);
}
// Before an upload of the sphere block: restore it on a skipped frame; on a rendered frame only observe whether the
// main thread already replaced it (the engine's own race, counted as nativeRace).
inline void beforeUpload(void* map,std::atomic<std::uint64_t>& restored) noexcept {
    auto& s=stats;if(!renderThread.load(std::memory_order_relaxed))return;
    if(!onRenderThread()){s.foreignThread.fetch_add(1,std::memory_order_relaxed);return;}
    if(stagger::compose(map))return; // Per-tile composite while the atlas is staggered
    auto* block=sphereBlock(map);
    if(tracker.skippedFrame){
        if(!foreignRender.load(std::memory_order_relaxed)&&tracker.restore(map,block))restored.fetch_add(1,std::memory_order_relaxed);
        else s.restoreMisses.fetch_add(1,std::memory_order_relaxed);
    }else if(tracker.valid&&tracker.owner==map&&std::memcmp(block,tracker.spheres.data(),sphereBytes))s.nativeRace.fetch_add(1,std::memory_order_relaxed);
}
// GOT replacement of BeginRenderShadowsIntoShadowCollector (exact: DrawShadowMapsIntoCollector, per native eye).
inline void collector(void* map,bool a,bool b,bool c,std::uintptr_t caller) noexcept {
    const bool exact=caller==config.collectorReturn&&shadowMap(map);
    if(exact){stats.collectorCalls.fetch_add(1,std::memory_order_relaxed);beforeUpload(map,stats.restoresCollector);}
    // Any other reader of this map on the render thread (RenderTopDownShadowMap) gets the composite too
    else if(renderThread.load(std::memory_order_relaxed)&&onRenderThread()&&shadowMap(map))stagger::composeOther(map);
    originalCollector(map,a,b,c);
    if(exact){float range{};std::memcpy(&range,static_cast<const unsigned char*>(map)+rangeField,sizeof range);
        if(std::isfinite(range)&&range>0.f)stats.receiver.cascadeRange.store(range,std::memory_order_relaxed);}
}
// GOT replacement of SetShadowMapConstants (exact: PreTransparentDrawCallback).
inline void constants(void* map,std::uintptr_t caller) noexcept {
    if(caller==config.constantsReturn&&shadowMap(map)){stats.constantsCalls.fetch_add(1,std::memory_order_relaxed);beforeUpload(map,stats.restoresConstants);}
    originalConstants(map);
}
// Receiver arguments of the exact RenderDirectionalShadow call: histogram, then the STEREO TEST 14/15 change.
struct ReceiverArgs {float range{};bool cloud{};};
inline ReceiverArgs receiver(float range,bool cloud,int tiles,unsigned testMode) noexcept {
    auto& r=stats.receiver;r.calls.fetch_add(1,std::memory_order_relaxed);
    (cloud?r.cloud:r.noCloud).fetch_add(1,std::memory_order_relaxed);
    r.tiles[tiles>=0&&tiles<5?unsigned(tiles):5u].fetch_add(1,std::memory_order_relaxed);
    if(std::isfinite(range)&&range>=0.f&&range<1e6f)(cloud?r.rangeCloudDm:r.rangeNoCloudDm).add(std::uint64_t(range*10.f+.5f));
    r.lastRange.store(range,std::memory_order_relaxed);
    ReceiverArgs out{range,cloud};
    if(testMode==stereo::testModeReceiverNoClouds&&cloud){
        // Native non-cloud path: WithoutClouds technique, range = CShadowMap::GetRange() (+0x8c of this frame).
        out.cloud=false;const float cascade=r.cascadeRange.load(std::memory_order_relaxed);
        if(cascade>0.f&&cascade<out.range)out.range=cascade;
        r.forcedNoCloud.fetch_add(1,std::memory_order_relaxed);
    }
    if(testMode==stereo::testModeReceiverRangeCap&&out.range>rangeCapM){out.range=rangeCapM;r.capped.fetch_add(1,std::memory_order_relaxed);}
    return out;
}

inline std::string report() {
    const auto& s=stats;std::ostringstream o;
    const auto ms=[](double us){return us<0?-1.:us/1000.;};
    const auto m=[](double dm){return dm<0?-1.:dm/10.;};
    const auto f=[](float x){return std::isfinite(x)?x:-1.f;};
    const auto level=s.level.load();const auto rendered=s.rendered.load(),skipped=s.skipped.load();
    o<<"\"shadowUpdate\":{\"level\":"<<level<<",\"name\":\""<<levelName(level)<<"\",\"hooked\":"<<s.hooked.load()
     <<",\"calls\":"<<s.calls.load()<<",\"rendered\":"<<rendered<<",\"skippedCascadeFrames\":"<<skipped
     <<",\"renderedPct\":"<<(rendered+skipped?100.*double(rendered)/double(rendered+skipped):-1.)
     <<",\"declined\":"<<s.declined.load()<<",\"foreignCalls\":"<<s.foreignCalls.load()<<",\"foreignRenders\":"<<s.foreignRenders.load()
     <<",\"tileBegins\":"<<s.tileBegins.load()<<",\"snapshotFallback\":"<<s.snapshotFallback.load()
     <<",\"restores\":{\"collectorCalls\":"<<s.collectorCalls.load()<<",\"collector\":"<<s.restoresCollector.load()
     <<",\"constantsCalls\":"<<s.constantsCalls.load()<<",\"constants\":"<<s.restoresConstants.load()
     <<",\"misses\":"<<s.restoreMisses.load()<<",\"foreignThread\":"<<s.foreignThread.load()<<",\"nativeRace\":"<<s.nativeRace.load()<<'}';
    o<<",\"triggers\":{";for(unsigned i=0;i<TriggerCount;++i)o<<(i?",":"")<<'"'<<triggerNames[i]<<"\":"<<s.triggers[i].load();
    o<<"},\"skips\":{";for(unsigned i=0;i<SkipCount;++i)o<<(i?",":"")<<'"'<<skipNames[i]<<"\":"<<s.skips[i].load();
    const auto cpuN=s.cpuUs.count();
    o<<"},\"atlasCpuMs\":{\"calls\":"<<cpuN<<",\"p50\":"<<ms(s.cpuUs.percentile(.5))<<",\"p95\":"<<ms(s.cpuUs.percentile(.95))
     <<",\"mean\":"<<(cpuN?double(s.cpuUsSum.load())/double(cpuN)/1000.:-1.)<<'}';
    o<<",\"byState\":{";
    for(unsigned i=0;i<stateCount;++i){const auto& b=s.byState[i];const auto frames=b.frames.load(),r=b.rendered.load();
        o<<(i?",":"")<<'"'<<stateName(i)<<"\":{\"frames\":"<<frames<<",\"rendered\":"<<r
         <<",\"cpuMsPerFrame\":"<<(frames?double(b.cpuUsSum.load())/double(frames)/1000.:-1.)
         <<",\"cpuMsP50\":"<<ms(b.cpuUs.percentile(.5))<<",\"cpuMsP95\":"<<ms(b.cpuUs.percentile(.95))
         <<",\"indoorFrames\":"<<b.indoorFrames.load()<<",\"indoorRendered\":"<<b.indoorRendered.load()
         <<",\"indoorCpuMsPerFrame\":"<<(b.indoorFrames.load()?double(b.indoorCpuUsSum.load())/double(b.indoorFrames.load())/1000.:-1.)<<'}';}
    o<<"},\"lastDelta\":{\"translationM\":"<<f(s.lastTranslationM.load())<<",\"rotationDeg\":"<<f(s.lastRotationDeg.load())<<",\"sunDeg\":"<<f(s.lastSunDeg.load())<<'}';
    { // INDOOR thresholds (ADAPTIVE ones under "adaptive"), per-tile stats, cascade cull
        const auto& t=thresholds;
        o<<",\"thresholds\":{\"translationM\":"<<t.indoorTranslationM<<",\"sunDeg\":"<<t.indoorSunDeg<<",\"rotationDeg\":"<<t.indoorRotationDeg
         <<",\"intervalMs\":"<<double(t.indoorIntervalUs)/1000.<<",\"adaptive\":{\"translationM\":"<<t.translationM<<",\"rotationDeg\":"<<t.rotationDeg
         <<",\"sunDeg\":"<<t.sunDeg<<",\"cutM\":"<<t.cutM<<",\"frames\":"<<t.adaptiveFrames<<"}}";
        o<<",\"tiles\":{";
        for(unsigned l=0;l<cascadeCullLevels;++l){
            o<<(l?",":"")<<'"'<<cascadeCullName(l)<<"\":[";
            for(unsigned i=0;i<4;++i){const auto& ts=s.tiles[l][i];const auto n=ts.begins.load();
                o<<(i?",":"")<<"{\"tile\":"<<i<<",\"begins\":"<<n<<",\"cpuMsMean\":"<<(n?double(ts.usSum.load())/double(n)/1000.:-1.)
                 <<",\"cpuMsP50\":"<<ms(ts.us.percentile(.5))<<",\"cpuMsP95\":"<<ms(ts.us.percentile(.95))
                 <<",\"castersMean\":"<<(n?double(ts.casters.load())/double(n):-1.)<<",\"castersMax\":"<<ts.castersMax.load()<<'}';}
            o<<']';
        }
        const auto setupN=s.setupRenders.load();
        o<<"},\"probedRenders\":"<<s.probedRenders.load()<<",\"setupMsMean\":"<<(setupN?double(s.setupUsSum.load())/double(setupN)/1000.:-1.)
         <<",\"preTileCasters\":"<<s.preTileCasters.load();
        const auto& c=cull;const auto bitsFloat=[&](std::uint32_t v){return v?f(std::bit_cast<float>(v)):-1.f;};
        const unsigned cl=s.cullLevel.load();
        o<<",\"cascadeCull\":{\"level\":"<<cl<<",\"name\":\""<<cascadeCullName(cl)<<"\",\"ready\":"<<(c.ready.load()?1:0)
         <<",\"configured\":"<<(cascadeCullConfigured()?1:0)<<",\"effective\":"<<c.effective.load()
         <<",\"fingerprint\":\""<<c.fingerprint.load()<<"\",\"native\":["<<bitsFloat(cascadeCullNativeBits[0])<<','<<bitsFloat(cascadeCullNativeBits[1])
         <<"],\"applied\":["<<bitsFloat(c.appliedBits[0].load())<<','<<bitsFloat(c.appliedBits[1].load())<<']'
         <<",\"applyCalls\":"<<c.applyCalls.load()<<",\"writes\":"<<c.writes.load()<<",\"restores\":"<<c.restores.load()
         <<",\"foreign\":"<<c.foreign.load()<<",\"suspended\":"<<(c.suspended.load()?1:0)<<'}';
    }
    const auto& r=s.receiver;
    o<<",\"receiver\":{\"calls\":"<<r.calls.load()<<",\"cloudTrue\":"<<r.cloud.load()<<",\"cloudFalse\":"<<r.noCloud.load()<<",\"tiles\":[";
    for(unsigned i=0;i<6;++i)o<<(i?",":"")<<r.tiles[i].load();
    o<<"],\"rangeCloudM\":{\"p50\":"<<m(r.rangeCloudDm.percentile(.5))<<",\"p95\":"<<m(r.rangeCloudDm.percentile(.95))<<'}'
     <<",\"rangeNoCloudM\":{\"p50\":"<<m(r.rangeNoCloudDm.percentile(.5))<<",\"p95\":"<<m(r.rangeNoCloudDm.percentile(.95))<<'}'
     <<",\"lastRangeM\":"<<f(r.lastRange.load())<<",\"cascadeRangeM\":"<<f(r.cascadeRange.load())
     <<",\"forcedNoCloud\":"<<r.forcedNoCloud.load()<<",\"capped\":"<<r.capped.load()<<",\"capM\":"<<rangeCapM<<'}';
    o<<',';stagger::report(o);
    o<<',';sunnight::report(o);
    o<<'}';
    return o.str();
}
}
