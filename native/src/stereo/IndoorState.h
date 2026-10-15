#pragma once
// "Is the camera in a room" for SHADOW UPDATE INDOOR (ShadowUpdate.h level 4), the SUN SHADOWS menu
// suffix (runtime::cameraIndoorMenu, stored by GameXr from flag()) and the gpuStages signature letter _in<0|1|x>.
// librdr 1.58 arm64:
//  * sagEnvironment::UpdateIndoorContribution @0x2a9f6d8 runs on the render thread and writes byte env+0x250fc = "camera
//    point is in a room whose flags +0xa0 lack bit1" (strb w9,[x23,#0xfc] @0x2a9f7e8; strb w8,[x23,#0xfc] @0x2a9fcf0, with
//    x23 = env+0x25000 from add x23,x0,#0x25,lsl #12 @0x2a9f70c) and the float env+0x250ec (indoor contribution 0..1,
//    str s0,[x23,#0xec] @0x2a9fcf4). Callers: sagRenderer::RenderFrame bl @0x2a58458 with x0 = [[sagWorld::smInstance]
//    +0x1d480] (right after WaitForVisibilityList(13), i.e. before RenderScene -> DrawShadowMaps of the same frame) and
//    sagWorld::DrawStart bl @0x2aac024 (MAIN, no camera cut). sagEnvironment::sm_instance holds the same object.
//  * The rooms bool of CachedVisibility::CreateVisibleShaftList: renVisibilityListMgr::CreateVisibilityListShaftGen
//    0x2abe2bc..0x2abe2e8 computes w3 = (request+0x98 | request+0xa0) != 0 (both from rmpWorld::QueryPointIntersectsRoom of
//    the request camera point); the mod sees it in VisibilityCullBoundary::cached -> afterCached (MAIN union scope, on the
//    visibility worker), which calls publishRooms().
// raw signal: both live -> env==1 && rooms; only env live -> env; only rooms live -> rooms; neither -> -1 (unknown).
// Hysteresis: enter after enterFrames consecutive raw==1 samples; any other sample leaves at once and sets exitPending
// (ShadowUpdate.h forces one atlas render on that frame). An unknown signal counts as outside.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace rdrvr::engine::indoor {
// ---- librdr 1.58 arm64 (module-relative) ----
inline constexpr std::uintptr_t worldInstanceOffset=0x4e8a5f58,worldInstanceGotOffset=0x2e015d0; // sagWorld::smInstance
inline constexpr std::uintptr_t worldEnvironmentField=0x1d480;                                   // sagWorld -> sagEnvironment*
inline constexpr std::uintptr_t envInstanceOffset=0x4e8a51a8,envInstanceGotOffset=0x2e01610;     // sagEnvironment::sm_instance
inline constexpr std::uintptr_t indoorByteField=0x250fc,contributionField=0x250ec;
inline constexpr std::uintptr_t updateOffset=0x2a9f6d8; // sagEnvironment::UpdateIndoorContribution (prologue)
inline constexpr std::array<unsigned char,32> updateFingerprint{
#include "gamecode/IndoorState_1bd9170_32_8d.inc"
};
inline constexpr std::uintptr_t envBaseOffset=0x2a9f70c;        // add x23,x0,#0x25,lsl #12
inline constexpr std::array<unsigned char,4> envBaseFingerprint{23,148,64,145};
inline constexpr std::uintptr_t storeByteOffset=0x2a9f7e8;      // strb w9,[x23,#0xfc]
inline constexpr std::array<unsigned char,4> storeByteFingerprint{233,242,3,57};
inline constexpr std::uintptr_t storeByteFloatOffset=0x2a9fcf0; // strb w8,[x23,#0xfc]; str s0,[x23,#0xec]
inline constexpr std::array<unsigned char,8> storeByteFloatFingerprint{232,242,3,57,224,238,0,189};
inline constexpr std::uintptr_t renderFrameCallOffset=0x2a58458; // bl UpdateIndoorContribution (sagRenderer::RenderFrame)
inline constexpr std::array<unsigned char,4> renderFrameCallFingerprint{158,88,9,148};
inline constexpr std::uintptr_t drawStartCallOffset=0x2aac024;   // bl UpdateIndoorContribution (sagWorld::DrawStart)
inline constexpr std::array<unsigned char,4> drawStartCallFingerprint{171,9,8,148};
inline constexpr std::uintptr_t roomsProvenanceOffset=0x2abe2bc; // CreateVisibilityListShaftGen: rooms = +0x98 | +0xa0
inline constexpr std::array<unsigned char,48> roomsProvenanceFingerprint{
#include "gamecode/IndoorState_2abe2bc_48_8d.inc"
};
inline constexpr unsigned enterFrames=6;      // consecutive raw==1 samples before "in a room"
inline constexpr unsigned roomsLiveSamples=4; // the rooms bool is live while its serial moved within the last 4 samples
inline constexpr unsigned ringSize=8;

// ---- pure policy (host-tested) ----
// env/rooms: -1 unknown, 0, 1. When both are live they must agree.
constexpr int combine(int env,int rooms) noexcept {return env>=0?((env==1&&rooms!=0)?1:0):(rooms<0?-1:rooms?1:0);}
struct Filter {
    unsigned run{};bool in{},exitPending{};
    // Returns +1 on entering, -1 on leaving, 0 otherwise.
    int step(int raw) noexcept {
        if(raw==1){if(run<enterFrames)++run;if(!in&&run>=enterFrames){in=true;return 1;}return 0;}
        run=0;if(!in)return 0;
        in=false;exitPending=true;return -1;
    }
};

// ---- runtime state ----
struct Config {
    std::uintptr_t worldVar{},envVar{};          // absolute addresses of the two bss pointers (written before *Ready)
    std::atomic<bool> envReady{},roomsReady{};    // fingerprints proven (release after the addresses)
};
inline Config config;
struct Transition {
    std::atomic<std::uint64_t> sample{};std::atomic<int> in{},env{},rooms{};std::atomic<float> contribution{};
    std::array<std::atomic<float>,3> pos{};
};
struct Stats {
    std::atomic<std::uint64_t> samples{},indoorFrames{},enters{},exits{};
    std::array<std::array<std::atomic<std::uint64_t>,2>,2> agree{}; // [env][rooms] when both are live
    std::atomic<std::uint64_t> envInvalid{},envMismatch{},roomsStale{},roomsPublished{};
    std::atomic<float> contributionLast{-1.f};
    std::atomic<int> envNow{-1},roomsNow{-1},rawNow{-1};
    std::array<Transition,ringSize> ring{};std::atomic<std::uint64_t> transitions{};
};
inline Stats stats;
inline std::atomic<int> flagValue{-1};             // -1 unknown, 0 outside, 1 in a room (filtered)
inline std::atomic<bool> roomsValue{};             // last rooms bool of the MAIN visibility build
inline std::atomic<std::uint64_t> roomsSerial{};   // +1 per publishRooms
struct RenderState {Filter filter{};std::uint64_t lastSerial{};unsigned staleSamples{roomsLiveSamples};};
inline RenderState renderState;                    // render thread only

template<class Code,class Read> void configure(std::uintptr_t base,Code code,Read read) {
    const auto got=[&](std::uintptr_t slot,std::uintptr_t target){
        std::uintptr_t value{};return read(nullptr,base+slot,&value,sizeof value)&&value==base+target;};
    const bool env=code(updateOffset,updateFingerprint)&&code(envBaseOffset,envBaseFingerprint)&&
        code(storeByteOffset,storeByteFingerprint)&&code(storeByteFloatOffset,storeByteFloatFingerprint)&&
        code(renderFrameCallOffset,renderFrameCallFingerprint)&&code(drawStartCallOffset,drawStartCallFingerprint)&&
        got(worldInstanceGotOffset,worldInstanceOffset)&&got(envInstanceGotOffset,envInstanceOffset);
    const bool rooms=code(roomsProvenanceOffset,roomsProvenanceFingerprint);
    config.worldVar=base+worldInstanceOffset;config.envVar=base+envInstanceOffset;
    config.envReady.store(env,std::memory_order_release);config.roomsReady.store(rooms,std::memory_order_release);
}
// Visibility worker, MAIN scope (TraceEntry occluderAfterCached).
inline void publishRooms(bool rooms) noexcept {
    roomsValue.store(rooms,std::memory_order_relaxed);roomsSerial.fetch_add(1,std::memory_order_release);
    stats.roomsPublished.fetch_add(1,std::memory_order_relaxed);
}
inline const unsigned char* loadPointer(std::uintptr_t address) noexcept {
    const unsigned char* p{};std::memcpy(&p,reinterpret_cast<const void*>(address),sizeof p);return p;
}
// Render thread, once per frame (ShadowUpdate.h renderInto, exact path). cameraPos only feeds the transition ring.
inline void sample(const float* cameraPos) noexcept {
    auto& s=stats;auto& r=renderState;
    const auto n=s.samples.fetch_add(1,std::memory_order_relaxed)+1;
    int env=-1;float contribution=-1.f;
    if(config.envReady.load(std::memory_order_acquire)){
        const unsigned char* world=loadPointer(config.worldVar);
        const unsigned char* fromWorld=world?loadPointer(reinterpret_cast<std::uintptr_t>(world)+worldEnvironmentField):nullptr;
        const unsigned char* instance=loadPointer(config.envVar);
        if(!fromWorld||!instance)s.envInvalid.fetch_add(1,std::memory_order_relaxed);
        else if(fromWorld!=instance)s.envMismatch.fetch_add(1,std::memory_order_relaxed);
        else{
            std::uint8_t byte{};float c{};
            std::memcpy(&byte,fromWorld+indoorByteField,1);std::memcpy(&c,fromWorld+contributionField,sizeof c);
            if(byte<=1){env=byte;contribution=std::isfinite(c)?std::clamp(c,0.f,1.f):-1.f;}
            else s.envInvalid.fetch_add(1,std::memory_order_relaxed);
        }
    }
    int rooms=-1;
    if(config.roomsReady.load(std::memory_order_acquire)){
        const auto serial=roomsSerial.load(std::memory_order_acquire);
        if(serial!=r.lastSerial){r.lastSerial=serial;r.staleSamples=0;}else if(r.staleSamples<(1u<<30))++r.staleSamples;
        if(serial&&r.staleSamples<roomsLiveSamples)rooms=roomsValue.load(std::memory_order_relaxed)?1:0;
        else s.roomsStale.fetch_add(1,std::memory_order_relaxed);
    }
    const int raw=combine(env,rooms);
    if(env>=0&&rooms>=0)s.agree[unsigned(env)][unsigned(rooms)].fetch_add(1,std::memory_order_relaxed);
    const int moved=r.filter.step(raw);
    if(r.filter.in)s.indoorFrames.fetch_add(1,std::memory_order_relaxed);
    if(moved){
        (moved>0?s.enters:s.exits).fetch_add(1,std::memory_order_relaxed);
        auto& t=s.ring[unsigned(s.transitions.load(std::memory_order_relaxed)%ringSize)];
        t.sample.store(n,std::memory_order_relaxed);t.in.store(moved>0?1:0,std::memory_order_relaxed);
        t.env.store(env,std::memory_order_relaxed);t.rooms.store(rooms,std::memory_order_relaxed);
        t.contribution.store(contribution,std::memory_order_relaxed);
        for(unsigned i=0;i<3;++i){const float v=cameraPos?cameraPos[i]:0.f;t.pos[i].store(std::isfinite(v)?v:0.f,std::memory_order_relaxed);}
        s.transitions.fetch_add(1,std::memory_order_release);
    }
    s.envNow.store(env,std::memory_order_relaxed);s.roomsNow.store(rooms,std::memory_order_relaxed);s.rawNow.store(raw,std::memory_order_relaxed);
    s.contributionLast.store(contribution,std::memory_order_relaxed);
    flagValue.store(r.filter.in?1:raw<0?-1:0,std::memory_order_relaxed);
}
// -1 unknown, 0 outside, 1 in a room. Any thread (GameXr menu/signature, ShadowUpdate decision).
inline int flag() noexcept {return flagValue.load(std::memory_order_relaxed);}
// Render thread only: true once after the camera left a room (sample() set exitPending).
inline bool consumeExit() noexcept {const bool e=renderState.filter.exitPending;renderState.filter.exitPending=false;return e;}
// Host tests: counters, filter and flag back to the start state (configuration and published rooms stay).
inline void reset() noexcept {
    auto& s=stats;
    for(auto* a:{&s.samples,&s.indoorFrames,&s.enters,&s.exits,&s.envInvalid,&s.envMismatch,&s.roomsStale,&s.roomsPublished,&s.transitions})
        a->store(0,std::memory_order_relaxed);
    for(auto& row:s.agree)for(auto& a:row)a.store(0,std::memory_order_relaxed);
    s.contributionLast.store(-1.f);s.envNow.store(-1);s.roomsNow.store(-1);s.rawNow.store(-1);
    renderState=RenderState{};renderState.lastSerial=roomsSerial.load(std::memory_order_relaxed);flagValue.store(-1);
}

inline std::string report() {
    const auto& s=stats;std::ostringstream o;
    const auto f=[](float x){return std::isfinite(x)?x:-1.f;};
    o<<"\"indoor\":{\"ready\":{\"env\":"<<(config.envReady.load()?1:0)<<",\"rooms\":"<<(config.roomsReady.load()?1:0)<<'}'
     <<",\"flag\":"<<flag()<<",\"samples\":"<<s.samples.load()<<",\"indoorFrames\":"<<s.indoorFrames.load()
     <<",\"enters\":"<<s.enters.load()<<",\"exits\":"<<s.exits.load()
     <<",\"envNow\":"<<s.envNow.load()<<",\"roomsNow\":"<<s.roomsNow.load()<<",\"rawNow\":"<<s.rawNow.load()
     <<",\"contribution\":"<<f(s.contributionLast.load())
     <<",\"agree\":[["<<s.agree[0][0].load()<<','<<s.agree[0][1].load()<<"],["<<s.agree[1][0].load()<<','<<s.agree[1][1].load()<<"]]"
     <<",\"envInvalid\":"<<s.envInvalid.load()<<",\"envMismatch\":"<<s.envMismatch.load()
     <<",\"roomsStale\":"<<s.roomsStale.load()<<",\"roomsPublished\":"<<s.roomsPublished.load()
     <<",\"enterFrames\":"<<enterFrames<<",\"transitions\":[";
    const auto total=s.transitions.load(std::memory_order_acquire);
    for(std::uint64_t i=total>ringSize?total-ringSize:0,first=i;i<total;++i){
        const auto& t=s.ring[unsigned(i%ringSize)];
        o<<(i==first?"":",")<<"{\"n\":"<<t.sample.load()<<",\"in\":"<<t.in.load()<<",\"env\":"<<t.env.load()<<",\"rooms\":"<<t.rooms.load()
         <<",\"c\":"<<f(t.contribution.load())<<",\"pos\":["<<f(t.pos[0].load())<<','<<f(t.pos[1].load())<<','<<f(t.pos[2].load())<<"]}";
    }
    o<<"]}";
    return o.str();
}
} // namespace rdrvr::engine::indoor
