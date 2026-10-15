#pragma once
// QUEST MARKER: a yellow beacon in BOTH eyes at the places the game's radar marks as "go here".
// RDR1 draws NO 3D objective marker in the world (no C3dMarkers / DRAW_MARKER / ShowRadarMarker in librdr; CRadar is 2D only: the yellow
// objective dot and the GPS line live on the minimap, CRadar::Draw / RenderGpsOntoRadar). So nothing is "lost" in the stereo path - the
// beacon is the mod's own, built from the radar's data:
//   * CRadar::RadarTrace[400] (0x4e8511e8, GOT 0x2e0f0f8): CRadarTrace* per blip slot; complex blips (+0x08 == 1, CRadarTraceComplex 0xa0 B):
//     +0x40 u16 flags (0x400 = shown, hudManager::IsMapObjectiveVisible), +0x50 world position (entity blips: refreshed by UpdateEntityBlip),
//     +0x70 type (8/9 = radius area, CRadar::Update -> UpdateRadiusBlip), +0x74 display (SP: coord blips drawn only >= 2 and != 3 / 6,
//     entity blips (+0x94 actor handle != 0) != 3 / 6, areas always), +0x78 scale (area radius), +0x7c Color32 0xAARRGGBB (SetGPSPath),
//     +0x80 alpha, +0x88 CBlipDesc* (sprite; SetBlipParameter(3) stores
//     CRadar::RadarBlips[id]). RadarBlips[i] = CBlipDesc(RadarBlipSpriteFilenames[i]) (PostDLCInit loop): 33/34/35 = BLIP_OBJECTIVE(_UP/_DOWN),
//     58 CHECKPOINT_GENERIC, 115/116 RACE_CHECKPOINT / RACE_FINISH, 2..24 the story contacts (mission starts).
//   * GPS destinations: CWaypoint[i] = hudManager (atSingleton sm_Instance 0x4e6ed0d8) + 0xd5e0 + i * 0x8070 (CRadar at +0xc600, waypoints at
//     CRadar+0xfe0; hudScript::SetGPSPath / ClearGPSPath): +0x00 active, +0x10 destination (the script's full xyz; the GPS radar blip itself
//     has y = 0). i = 1 mission (g_iMissionWaypointID), 0 the player's map waypoint (CFrontEnd), 2 posse leader (MP, unused).
//   * ScriptGlowManager (0x4e829478, one ScriptGlow): the script "glow indicator" optic (OpticsManager::Draw = tail, NOT in the eyes) - report
//     only unless sources bit 8.
// Main thread (ModuleMarker id 10, after TELEPORT - reads only, the per-run arc::Context is shared): guarded batched scan every 250 ms, a
// WORLD arc frame on the hands-pass channel MARKER every tick (eye = this tick's eye midpoint). Beacon: camera-facing yellow ribbon from the
// target's base upward, fading to 0 at the top + a disc at the nearest one's base; beyond `near` (40 m) it is drawn at 40 m along the ray to
// the target (same direction in both eyes, constant angular size: ~11 deg tall, ~0.4 deg wide), fades out inside 4..10 m ("arrived").
// Default: world-tested, no x-ray (never seen through walls); debug.rdrvr.vr.marker.depth 0 = over the world
// (NoWorldTest, the cheapest arc pass). Up to 4 targets (nearest), merged within 3 m. Sources default 1|2|8 (+ the script glow indicator).
// Kill switch debug.rdrvr.vr.marker 0 (Feature::Marker: no checks, no module); menu MOVEMENT > QUEST MARKER ON/OFF; levers (every 64 ticks):
// debug.rdrvr.vr.marker.sources (bits 1 objective blips + yellow radius areas, 2 mission GPS, 4 map waypoint, 8 script glow, 16 mission
// starts; default 3).
#include "HandsArcApi.h"
#include "VrArc.h"
#include "VrMainThread.h"
#include "VrSettings.h"
#include "diagnostics/GuardedReadVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace rdrvr::vr::marker {
namespace hs=::rdrvr::vr::hands;namespace am=::rdrvr::vr::arc::math;using V3=::rdrvr::vr::arc::V3;

// ============================== ELF identities (librdr.so arm64 sha 3f80845e..; file offset = vaddr - 0x4000 for .text) ==============================
namespace elf {
inline constexpr std::uintptr_t radarTraceGotOffset=0x2e0f0f8,radarTraceOffset=0x4e8511e8;  // CRadar::RadarTrace (400 x CRadarTrace*)
inline constexpr std::uintptr_t radarBlipsGotOffset=0x2e0f220,radarBlipsOffset=0x4e851e68;  // CRadar::RadarBlips (CBlipDesc* by sprite id)
inline constexpr std::uintptr_t hudManagerGotOffset=0x2deb900,hudManagerOffset=0x4e6ed0d8;  // atSingleton<hudManager>::sm_Instance
inline constexpr std::uintptr_t glowManagerGotOffset=0x2e0e098,glowManagerOffset=0x4e829478; // ScriptGlowManager::sm_Instance
inline constexpr unsigned traceSlots=400,spriteSlots=270;                                    // 0x18f+1 (CreateGenericBlip), 0x870/8 (PostDLCInit)
inline constexpr std::size_t trComplex=0x08,trFlags=0x40,trPos=0x50,trType=0x70,trDisplay=0x74,trColor=0x7c,trAlpha=0x80,trDesc=0x88,trEntity=0x94,trBytes=0x98;
inline constexpr std::uint16_t flagShown=0x400;
inline constexpr std::size_t descIndex=0x08,waypointBase=0xd5e0,waypointStride=0x8070,wpActive=0x00,wpDest=0x10;
inline constexpr std::size_t glowArray=0x50,glowCount=0x58;                                 // ScriptGlow[] (0x28 B: +0 OpticalEffect*, pos at +0)
// Prints (exact bytes in an executable mapping; TraceEntry checks them before anything is read):
inline constexpr std::uintptr_t traceComplexCtorOffset=0x2a0fffc;   // CRadarTraceComplex::CRadarTraceComplex: +0x08 = 1, +0x40 = 0x1400, +0x74/+0x7c/+0x80/+0x88
inline constexpr std::array<unsigned char,80> traceComplexCtorFingerprint{
#include "gamecode/VrMarker_2a0fffc_80_8d.inc"
};
inline constexpr std::uintptr_t setBlipParamOffset=0x2a14db4;       // CRadar::SetBlipParameter(params, int, int) head .. param 3 (sprite -> +0x88)
inline constexpr std::array<unsigned char,316> setBlipParamFingerprint{
#include "gamecode/VrMarker_2a14db4_316_8d.inc"
};
inline constexpr std::uintptr_t entityBlipStoreOffset=0x2a16f4c;    // CRadar::UpdateEntityBlip: entity position -> +0x50 (complex)
inline constexpr std::array<unsigned char,20> entityBlipStoreFingerprint{
#include "gamecode/VrMarker_2a16f4c_20_8d.inc"
};
inline constexpr std::uintptr_t objectiveVisibleOffset=0x29e8bf8;   // hudManager::IsMapObjectiveVisible: bit 10 of +0x40
inline constexpr std::array<unsigned char,12> objectiveVisibleFingerprint{8,129,64,121,0,41,10,83,253,123,193,168};
inline constexpr std::uintptr_t blipDescCtorOffset=0x2a10b14;       // CBlipDesc::CBlipDesc: +0x08 = iNumCreatedBlipSprites
inline constexpr std::array<unsigned char,52> blipDescCtorFingerprint{
#include "gamecode/VrMarker_2a10b14_52_8d.inc"
};
inline constexpr std::uintptr_t modelLoopOffset=0x2a149f0;          // CRadar::PostDLCInit: RadarBlips[n++] = new CBlipDesc(RadarBlipSpriteFilenames[i]), i < 270
inline constexpr std::array<unsigned char,80> modelLoopFingerprint{
#include "gamecode/VrMarker_2a149f0_80_8d.inc"
};
inline constexpr std::uintptr_t waypointSetOffset=0x2a11544;        // CWaypoint::Set(int, gohGUID, Vector4, Vector3): +0 = 1, dest -> +0x10
inline constexpr std::array<unsigned char,92> waypointSetFingerprint{
#include "gamecode/VrMarker_2a11544_92_8d.inc"
};
inline constexpr std::uintptr_t gpsSlotOffset=0x29fe6e8;            // hudScript::SetGPSPath: hudManager + type * 0x8070 + 0xd5e0 -> CWaypoint::Set
inline constexpr std::array<unsigned char,56> gpsSlotFingerprint{
#include "gamecode/VrMarker_29fe6e8_56_8d.inc"
};
inline constexpr std::uintptr_t scriptGlowCreateOffset=0x297cfac;   // ScriptGlow::Create: OpticalEffect* -> +0, its position -> effect +0 (optional: sources bit 8 / report)
inline constexpr std::array<unsigned char,188> scriptGlowCreateFingerprint{
#include "gamecode/VrMarker_297cfac_188_8d.inc"
};
} // namespace elf

enum Source : std::uint32_t {SrcObjective=1,SrcMission=2,SrcUser=4,SrcGlow=8,SrcContact=16,SrcAll=31};
inline constexpr std::array<int,6> objectiveSprites{33,34,35,58,115,116};
inline constexpr int contactFirst=2,contactLast=24;
inline constexpr unsigned maxTargets=16,maxDrawn=1; // Release: one beam, the nearest target
inline constexpr std::uint64_t scanIntervalNs=250'000'000;
struct Target {V3 pos{};std::uint32_t source{};int sprite{-1};};

// ============================== engine side (TraceEntry injects it after the prints / GOT words) ==============================
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
struct Env {
    ReadFn read{};std::uintptr_t radarTrace{},radarBlips{},hudManager{},glowManager{};
    explicit operator bool() const noexcept {return read&&radarTrace&&radarBlips;}
};
inline Env env{};
enum class Install : int {Unknown,Ok,Property,Prints,Got,NoExecutor};
inline std::atomic<int> installState{int(Install::Unknown)};
inline std::atomic<const char*> installNote{"NOT RUN"};

struct Stats {
    using C=std::atomic<std::uint64_t>;
    C ticks{},scans{},scanUsSum{},scanUsMax{},reads{},readFailures{},slots{},complexBlips{},shown{},objective{},areas{},contacts{},waypoints{},glows{},
      published{},hidden{},exceptions{},leverReads{},rejected{},merged{};
    std::array<C,8> display{}; // display value histogram of the shown objective-sprite blips (7 = >= 7)
};
inline Stats stats;
inline std::atomic<std::uint32_t> sourcesLever{SrcObjective|SrcMission|SrcGlow},depthLever{1},lastCount{},lastSources{},glowCount{};
inline std::atomic<const char*> hideReason{"NOT RUN"};
struct Shown {std::uint32_t count{};std::array<Target,maxDrawn> t{};std::array<float,maxDrawn> distM{};};
inline hs::SeqCell<Shown> shownCell; // main thread writes, report reads

namespace detail {
inline void bump(std::atomic<std::uint64_t>& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
template<class T> inline T at(const unsigned char* b,std::size_t o) noexcept {T v{};std::memcpy(&v,b+o,sizeof v);return v;}
template<class T> inline bool get(std::uintptr_t p,std::size_t o,T& out) noexcept {return env.read&&p&&p<=UINTPTR_MAX-o-sizeof(T)&&env.read(nullptr,p+o,&out,sizeof(T));}
inline bool block(std::uintptr_t p,void* out,std::size_t n) noexcept {return env.read&&p&&p<=UINTPTR_MAX-n&&env.read(nullptr,p,out,n);}
inline bool sane(const V3& p) noexcept {return am::finite(p)&&std::fabs(p[0])<1e5f&&std::fabs(p[1])<1e5f&&std::fabs(p[2])<1e5f;}
struct Scratch {
    std::array<std::uintptr_t,elf::traceSlots> trace{};std::array<std::uintptr_t,elf::spriteSlots> sprites{};
    std::array<unsigned char,elf::traceSlots*elf::trBytes> blocks{};std::array<diagnostics::ReadSegment,elf::traceSlots> segs{};
    std::array<unsigned,elf::traceSlots> slot{};
};
inline Scratch scratch; // main thread only
inline bool yellow(std::uint32_t argb) noexcept {const unsigned r=(argb>>16)&255u,g=(argb>>8)&255u,bl=argb&255u;return argb!=0xffffffffu&&r>=0xc0&&g>=0x90&&bl<=0x80;}
inline int spriteOf(std::uintptr_t desc,std::uint32_t sources) noexcept {
    if(!desc)return -1;const auto& sp=scratch.sprites;
    for(int s:objectiveSprites)if(sp[unsigned(s)]==desc)return s;
    if(sources&SrcContact)for(int s=contactFirst;s<=contactLast;++s)if(sp[unsigned(s)]==desc)return s;
    return -1;
}
inline void add(Target* out,unsigned& n,const V3& p,std::uint32_t src,int sprite,float mergeUnits) noexcept {
    for(unsigned i=0;i<n;++i){const float dx=out[i].pos[0]-p[0],dz=out[i].pos[2]-p[2];
        if(dx*dx+dz*dz<mergeUnits*mergeUnits){out[i].source|=src;if(src&(SrcMission|SrcUser))out[i].pos=p;bump(stats.merged);return;}} // GPS y wins (full xyz)
    if(n<maxTargets)out[n++]={p,src,sprite};
}
} // namespace detail

// Guarded scan (main thread). Returns the target count; never throws.
inline unsigned scan(Target* out,std::uint32_t sources,float upm) noexcept {
    using namespace detail;auto& S=scratch;unsigned n=0,calls=0;const float merge=3.f*std::max(upm,.01f);
    if(!env)return 0;
    if(sources&(SrcObjective|SrcContact)){
        ++calls;++calls;
        if(block(env.radarTrace,S.trace.data(),sizeof S.trace)&&block(env.radarBlips,S.sprites.data(),sizeof S.sprites)){
            unsigned m=0;
            for(unsigned i=0;i<elf::traceSlots;++i)if(const auto p=S.trace[i]){S.slot[m]=i;S.segs[m]={p,S.blocks.data()+std::size_t(m)*elf::trBytes,elf::trBytes};++m;}
            bump(stats.slots,m);
            bool all=false;if(m){++calls;try{all=diagnostics::guardedReadVector.read(env.read,nullptr,S.segs.data(),m);}catch(...){all=false;}}
            if(m&&!all)for(unsigned k=0;k<m;++k){++calls;if(!block(S.segs[k].source,S.segs[k].target,S.segs[k].size)){S.segs[k].size=0;bump(stats.readFailures);}}
            for(unsigned k=0;k<m;++k){
                if(!S.segs[k].size)continue;const unsigned char* b=S.blocks.data()+std::size_t(k)*elf::trBytes;
                if(at<std::uint8_t>(b,elf::trComplex)!=1)continue;bump(stats.complexBlips);
                if(!(at<std::uint16_t>(b,elf::trFlags)&elf::flagShown)||!at<std::uint8_t>(b,elf::trAlpha))continue;
                const int display=at<std::int32_t>(b,elf::trDisplay);const auto colour=at<std::uint32_t>(b,elf::trColor);
                int sprite=-1;bool objective=false;
                if((at<std::int32_t>(b,elf::trType)&~1)==8){ // radius area (RenderRadiusBlips): an objective only in the objective yellow
                    if(!(sources&SrcObjective)||!yellow(colour))continue;sprite=-2;objective=true;bump(stats.areas);}
                else{
                    sprite=spriteOf(at<std::uintptr_t>(b,elf::trDesc),sources);if(sprite<0)continue;
                    objective=sprite<contactFirst||sprite>contactLast;
                    if(!(sources&(objective?SrcObjective:SrcContact)))continue;
                    if(objective)bump(stats.display[unsigned(std::clamp(display,0,7))]);
                    const bool entity=at<std::uint16_t>(b,elf::trEntity)!=0;
                    if(display==3||display==6||(!entity&&display<2))continue; // UpdateEntityBlip / UpdateCoordBlip (SP): not on the radar
                }
                const V3 p{at<float>(b,elf::trPos),at<float>(b,elf::trPos+4),at<float>(b,elf::trPos+8)};if(!sane(p))continue;
                bump(stats.shown);bump(objective?stats.objective:stats.contacts);add(out,n,p,objective?SrcObjective:SrcContact,sprite,merge);
            }
        }else bump(stats.readFailures);
    }
    if((sources&(SrcMission|SrcUser))&&env.hudManager){
        std::uintptr_t hud{};++calls;
        if(get(env.hudManager,0,hud)&&hud)for(unsigned i:{1u,0u}){
            if(!(sources&(i?SrcMission:SrcUser)))continue;
            unsigned char w[0x20]{};++calls;if(!block(hud+elf::waypointBase+std::size_t(i)*elf::waypointStride,w,sizeof w)){bump(stats.readFailures);continue;}
            if(at<std::uint8_t>(w,elf::wpActive)!=1)continue;
            const V3 p{at<float>(w,elf::wpDest),at<float>(w,elf::wpDest+4),at<float>(w,elf::wpDest+8)};if(!sane(p))continue;
            bump(stats.waypoints);add(out,n,p,i?SrcMission:SrcUser,1,merge);
        }
    }
    glowCount.store(0,std::memory_order_relaxed);
    if(env.glowManager){
        std::uintptr_t mgr{},arr{},effect{};std::uint16_t count{};float p[3]{};calls+=4;
        if(get(env.glowManager,0,mgr)&&mgr&&get(mgr,elf::glowCount,count)&&count&&count<=4&&get(mgr,elf::glowArray,arr)&&arr&&get(arr,0,effect)&&effect&&block(effect,p,sizeof p)){
            glowCount.store(count,std::memory_order_relaxed);const V3 v{p[0],p[1],p[2]};
            if((sources&SrcGlow)&&sane(v)){bump(stats.glows);add(out,n,v,SrcGlow,-1,merge);}}
    }
    bump(stats.reads,calls);return n;
}

// ============================== the beacon (pure; host-testable) ==============================
struct Look {float nearM{40.f},arriveM{4.f},fadeM{6.f};bool depthTest{};};
inline constexpr float yr=1.f,yg=.62f,yb=.05f; // the game's objective yellow (linear)
// WORLD frame from up to maxDrawn nearest targets; count 0 = nothing visible (all arrived / invalid). distM[i] = distance of the i-th drawn one.
inline unsigned buildFrame(const Target* t,unsigned n,const V3& eye,V3 up,float upm,std::uint64_t key,const Look& look,hs::ArcFrame& out,
                           std::array<unsigned,maxDrawn>* drawnIdx=nullptr,std::array<float,maxDrawn>* drawnDist=nullptr) noexcept {
    out=hs::ArcFrame{};out.space=std::uint32_t(hs::ArcSpace::World);out.key=key;upm=std::max(upm,.01f);out.unitsPerMetre=upm;
    up=am::normalized(up);if(am::length(up)<.5f)up={0,1,0};
    if(!key||!t||!n||!am::finite(eye))return 0;
    std::array<unsigned,maxTargets> order{};std::array<float,maxTargets> dist{};unsigned m=0;
    for(unsigned i=0;i<n&&i<maxTargets;++i){const float d=am::distance(t[i].pos,eye)/upm;if(std::isfinite(d)&&d>.01f){order[m]=i;dist[i]=d;++m;}}
    std::sort(order.begin(),order.begin()+m,[&](unsigned a,unsigned b){return dist[a]<dist[b];});
    unsigned pts=0,beams=0;
    const auto put=[&](const V3& p,float hw,float a){auto& q=out.points[pts++];q.position[0]=p[0];q.position[1]=p[1];q.position[2]=p[2];
        q.halfWidthM=std::clamp(hw,.0006f,.19f);q.rgba=hs::packRgba(yr,yg,yb,a);q.depthTolM=0;};
    for(unsigned k=0;k<m&&beams<maxDrawn&&pts+4<=hs::arcMaxPoints;++k){
        const unsigned i=order[k];const float D=dist[i];
        const float fade=std::clamp((D-look.arriveM)/std::max(look.fadeM,.01f),0.f,1.f);if(fade<.02f)continue;
        const float Dm=std::min(D,look.nearM);const V3 d=am::sub(t[i].pos,eye);
        const V3 base=D>look.nearM?am::add(eye,am::mul(d,look.nearM/D)):t[i].pos;     // far: same direction, at nearM
        const float H=std::clamp(.20f*Dm,2.f,8.f),hw=std::clamp(.0035f*Dm,.02f,.14f),a=.85f*fade;
        if(pts)put(am::sub(base,am::mul(up,.01f*upm)),hw,0.f);                           // invisible link from the previous beam's top
        put(base,hw,a);put(am::add(base,am::mul(up,.35f*H*upm)),hw,.7f*a);put(am::add(base,am::mul(up,H*upm)),.6f*hw,0.f);
        if(!beams){auto& mk=out.marker;mk.shape=hs::ArcMarkerShape::Disc;mk.rgba=hs::packRgba(yr,yg,yb,.9f*fade);
            mk.centre[0]=base[0];mk.centre[1]=base[1];mk.centre[2]=base[2];mk.radiusM=std::clamp(.011f*Dm,.06f,.45f);mk.liftM=0;mk.depthTolM=.3f;mk.xrayAlpha=0.f;} // Never through walls
        if(drawnIdx)(*drawnIdx)[beams]=i;if(drawnDist)(*drawnDist)[beams]=D;
        ++beams;
    }
    if(pts<2){out.count=0;out.marker=hs::ArcMarker{};return 0;}
    out.count=pts;out.flags=hs::ArcFrame::Valid;
    auto& s=out.style;s.dashPeriodM=0;s.fadeInM=0;s.tube=1;s.core=.3f;s.xrayAlpha=0.f;s.flags=look.depthTest?0u:std::uint32_t(hs::ArcStyle::NoWorldTest);
    if(!hs::validArc(out)){out.count=0;out.marker=hs::ArcMarker{};return 0;}
    return beams;
}

// ============================== ModuleMarker tick ==============================
namespace detail {
struct State {std::array<Target,maxTargets> targets{};unsigned count{};std::uint64_t lastScanNs{},lastLeverIndex{};bool scanned{},shown{},armed{},levers{};};
inline State& state() noexcept {static State s;return s;}
inline void hide(State& s,const char* why) noexcept {
    hideReason.store(why,std::memory_order_relaxed);
    if(s.shown){hs::clearArc(hs::ArcChannel::Marker);s.shown=false;bump(stats.hidden);shownCell.store(Shown{});lastCount.store(0,std::memory_order_relaxed);}
}
inline void arm(State& s,bool on) noexcept {if(s.armed!=on){hs::armArc(hs::ArcChannel::Marker,on);s.armed=on;}}
} // namespace detail
inline void tick(const mainthread::Frame& fr) noexcept {
    using namespace detail;
    try{
        bump(stats.ticks);auto& s=state();
        if(!s.levers||fr.index>=s.lastLeverIndex+64||fr.index<s.lastLeverIndex){s.levers=true;s.lastLeverIndex=fr.index;bump(stats.leverReads);
            sourcesLever.store(std::uint32_t(arc::propertyInt("debug.rdrvr.vr.marker.sources",int(SrcObjective|SrcMission|SrcGlow)))&SrcAll,std::memory_order_relaxed);
            depthLever.store(arc::propertyInt("debug.rdrvr.vr.marker.depth",1)?1u:0u,std::memory_order_relaxed);} // World-tested by default
        // release (user): our cheap beacon is back ON by default (the game's own yellow X is drawn only while the camera is
        // indoors, cause not found yet); debug.rdrvr.vr.marker.custom 0 hides it
        static int custom=1;if(s.lastLeverIndex==fr.index)custom=arc::propertyInt("debug.rdrvr.vr.marker.custom",1);
        if(!custom||!settings().questMarker()||killed(Feature::Marker)||killed(Feature::Arc)||killed(Feature::Hands)){hide(s,custom?"OFF":"CUSTOM_OFF");arm(s,false);s.scanned=false;return;}
        const auto st=hs::arcStatus();
        if(st==hs::ArcStatus::Failed||st==hs::ArcStatus::Killed||st==hs::ArcStatus::Suppressed){hide(s,"ARC_UNAVAILABLE");return;}
        auto& c=arc::frameContext(fr.now);
        if(!c.world()){hide(s,"NOT_WORLD");return;}
        V3 eye{};if(!c.eye(eye)){hide(s,"NO_EYE");return;}
        const float upm=std::max(c.upm(),.01f);const auto sources=sourcesLever.load(std::memory_order_relaxed);
        if(!s.scanned||fr.now<s.lastScanNs||fr.now-s.lastScanNs>=scanIntervalNs){
            const auto t0=nowNs();s.count=scan(s.targets.data(),sources,upm);s.lastScanNs=fr.now;s.scanned=true;bump(stats.scans);
            const auto us=(nowNs()-t0)/1000u;stats.scanUsSum.fetch_add(us,std::memory_order_relaxed);arc::maxOf(stats.scanUsMax,us);
            std::uint32_t mask=0;for(unsigned i=0;i<s.count;++i)mask|=s.targets[i].source;lastSources.store(mask,std::memory_order_relaxed);}
        if(!s.count){hide(s,"NO_TARGETS");arm(s,false);return;}
        arm(s,true);
        Look look{};look.depthTest=depthLever.load(std::memory_order_relaxed)!=0;
        hs::ArcFrame f{};std::array<unsigned,maxDrawn> idx{};Shown sh{};
        const unsigned beams=buildFrame(s.targets.data(),s.count,eye,c.up(),upm,c.key(),look,f,&idx,&sh.distM);
        if(!beams){hide(s,f.key?"ARRIVED":"NO_KEY");return;}
        hs::publishArc(hs::ArcChannel::Marker,f);s.shown=true;bump(stats.published);hideReason.store("",std::memory_order_relaxed);
        sh.count=beams;for(unsigned i=0;i<beams;++i)sh.t[i]=s.targets[idx[i]];
        shownCell.store(sh);lastCount.store(beams,std::memory_order_relaxed);
    }catch(...){bump(stats.exceptions);}
}
// TraceEntry after the prints / GOT words: env first, then the tick. Idempotent.
inline bool install(const Env& e) noexcept {
    env=e;
    if(!mainthread::executorInstalled()){installState.store(int(Install::NoExecutor));installNote.store("NO EXECUTOR");
        status::publish(Feature::Marker,status::Unavailable);return false;}
    if(!mainthread::registerModule(mainthread::ModuleMarker,"marker",&tick,nullptr)){installState.store(int(Install::NoExecutor));installNote.store("MODULE SLOT");
        status::publish(Feature::Marker,status::Unavailable);return false;}
    installState.store(int(Install::Ok));installNote.store("OK");status::publish(Feature::Marker,status::Ready);return true;
}
inline void refuse(Install why,const char* note) noexcept {
    installState.store(int(why));installNote.store(note);status::publish(Feature::Marker,why==Install::Property?status::Property:status::Unavailable);}

inline std::string report() {
    char b[768];std::string o;o.reserve(1536);
    const auto sc=stats.scans.load();const auto ch=hs::arcChannelStats(hs::ArcChannel::Marker);
    std::snprintf(b,sizeof b,"{\"install\":\"%s\",\"menu\":%d,\"sources\":%u,\"depthTest\":%u,\"count\":%u,\"source\":%u,\"hidden\":\"%s\",\"ticks\":%llu,"
        "\"scans\":%llu,\"scanUsAvg\":%.1f,\"scanUsMax\":%llu,\"reads\":%llu,\"blips\":{\"slots\":%llu,\"complex\":%llu,\"shown\":%llu,\"objective\":%llu,"
        "\"areas\":%llu,\"contacts\":%llu,\"waypoints\":%llu,\"glows\":%llu,\"merged\":%llu},\"objectiveDisplay\":[",
        installNote.load(),settings().questMarker()?1:0,sourcesLever.load(),depthLever.load(),lastCount.load(),lastSources.load(),hideReason.load(),
        (unsigned long long)stats.ticks.load(),(unsigned long long)sc,sc?double(stats.scanUsSum.load())/double(sc):0.0,(unsigned long long)stats.scanUsMax.load(),
        (unsigned long long)stats.reads.load(),(unsigned long long)stats.slots.load(),(unsigned long long)stats.complexBlips.load(),
        (unsigned long long)stats.shown.load(),(unsigned long long)stats.objective.load(),(unsigned long long)stats.areas.load(),(unsigned long long)stats.contacts.load(),
        (unsigned long long)stats.waypoints.load(),(unsigned long long)stats.glows.load(),(unsigned long long)stats.merged.load());o+=b;
    for(unsigned i=0;i<8;++i){std::snprintf(b,sizeof b,"%s%llu",i?",":"",(unsigned long long)stats.display[i].load());o+=b;}
    o+="],\"positions\":[";
    Shown sh{};if(shownCell.load(sh))for(unsigned i=0;i<sh.count&&i<maxDrawn;++i){const auto& t=sh.t[i];
        std::snprintf(b,sizeof b,"%s{\"x\":%.1f,\"y\":%.1f,\"z\":%.1f,\"src\":%u,\"sprite\":%d,\"distM\":%.1f}",i?",":"",double(t.pos[0]),double(t.pos[1]),double(t.pos[2]),
            t.source,t.sprite,double(sh.distM[i]));o+=b;}
    std::snprintf(b,sizeof b,"],\"scriptGlow\":%u,\"drawn\":{\"published\":%llu,\"cleared\":%llu,\"hides\":%llu,\"rejected\":%llu,\"wrongThread\":%llu,\"killed\":%llu},"
        "\"errors\":{\"exceptions\":%llu,\"readFailures\":%llu,\"hidden\":%llu},\"leverReads\":%llu}",
        glowCount.load(),(unsigned long long)ch.published,(unsigned long long)ch.cleared,(unsigned long long)ch.hides,(unsigned long long)ch.rejected,
        (unsigned long long)ch.wrongThread,(unsigned long long)ch.killed,(unsigned long long)stats.exceptions.load(),(unsigned long long)stats.readFailures.load(),
        (unsigned long long)stats.hidden.load(),(unsigned long long)stats.leverReads.load());o+=b;
    return o;
}
} // namespace rdrvr::vr::marker
