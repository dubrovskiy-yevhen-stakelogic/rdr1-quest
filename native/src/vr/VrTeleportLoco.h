#pragma once
// TELEPORT locomotion:
// LOCOMOTION TELEPORT = left stick forward aims a ballistic arc from the left hand (the shared arc tracer of VrArc.h, published from THIS
// module's main-thread tick, mainthread::ModuleTeleport = 9 = last), the landing is validated with engine queries (down-only fixed ground,
// slope / distance / height / water, standing capsule, low obstacle, edge), releasing the stick commits the arc that was SHOWN ~50 ms before
// the release (History + Tracer::validateShown), the screen blinks (OpenXR colour scale, VrTeleportLink.h fade) and the player is warped
// with sagActorScript::TeleportActorWithHeading (0x1caafb8) from the UpdateInputs wrapper; the black is held until the
// mover recovery ended, the capsule settled, the XR thread submitted a pair drawn after the warp and the world yaw psi is stable.
//
//  * Pure, host-testable: Config (properties), StickGate, Validator (decision table on injected probes), History, Module::tick (the state
//    machine) with an injected Env (engine side) - the host test drives it with a fake world, the arc fakes of vr_arc_test and the fade word.
//  * Engine side (#ifdef __ANDROID__): EngineApi (print-checked function pointers, TraceEntry), the Env implementation, install().
// Kill switch debug.rdrvr.vr.tploco 0 (Feature::TpLoco): nothing checked, rows PROP, LOCOMOTION TELEPORT acts FREE. Report vr.tploco.
// (Not debug.rdrvr.teleport: that is the saved-spot teleport of stereo/Teleport.h.)
#include "VrTeleportLink.h"
#include "VrArc.h"
#include "VrHaptics.h"
#include "VrMainThread.h"
#include "VrSettings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

namespace rdrvr::vr::tploco {
namespace hs=::rdrvr::vr::hands;
using arc::V3;
namespace am=arc::math;
using Counter=std::atomic<std::uint64_t>;
inline void bump(Counter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline std::uint64_t u64(const Counter& c) noexcept {return c.load(std::memory_order_relaxed);}
inline void maxOf(Counter& c,std::uint64_t v) noexcept {auto m=c.load(std::memory_order_relaxed);while(v>m&&!c.compare_exchange_weak(m,v,std::memory_order_relaxed)){}}
inline constexpr std::uint64_t msNs(std::uint64_t ms) noexcept {return ms*1'000'000ull;}

// ============================== ELF identities (librdr.so arm64 sha 3f80845e..22f3; .text file offset = vaddr - 0x4000) ==============================
// The WHOLE TeleportActorToMatrix is fingerprinted (the 25.0 compare, the g_playerTeleporting / streamer stores, the p3 branch
// and the flag words tploco relies on). Any mismatch -> TELEPORT UNAVAILABLE (FREE path, row "UNAVAILABLE - FREE"), nothing
// else affected.
namespace elf {
inline constexpr std::uintptr_t toMatrixOffset=0x1caaa8c;                                   // sagActorScript::TeleportActorToMatrix (0x404 B)
inline constexpr std::uintptr_t groundOffset=0x245d99c,groundGotOffset=0x2e4b7a0;          // rdr2scripting::world::FindIntersectionWithMaterial
inline constexpr std::uintptr_t levelInstanceGotOffset=0x2de2030,levelInstanceOffset=0x4e6f6e98; // GLOB_DAT rage::phLevelNew::sm_ActiveInstance
inline constexpr std::uintptr_t capsuleOffset=0x268b300,capsuleGotOffset=0x2e475f0;        // sTestCapsule
inline constexpr std::uintptr_t liquidInstanceOffset=0x28a5fd4,liquidInstanceGotOffset=0x2e480a8; // EnvLiquidManager::GetInstance
inline constexpr std::uintptr_t liquidGlobalGotOffset=0x2e0c770,liquidGlobalOffset=0x4e8088d0;   // GLOB_DAT EnvLiquidManager::m_Instance
inline constexpr std::uintptr_t waterOffset=0x28a700c,waterGotOffset=0x2e480b0;            // EnvLiquidManager::GetWaterElevation
inline constexpr std::uintptr_t indoorsOffset=0x245e078;                                   // IsPositionIndoors (optional, report only)
inline constexpr std::uintptr_t worldGotOffset=0x2de1948,worldOffset=0x4e6f6cd8;           // GLOB_DAT rage::rmpWorld::sm_Instance
inline constexpr std::uintptr_t excludeOffset=0x217bc70,excludeGotOffset=0x2e46a68;        // ConfigureActorExcludeInsts (reticle print)
inline constexpr std::uintptr_t minigameGotOffset=0x2e01f60,minigameOffset=0x4e75ebe0;     // GLOB_DAT atSingleton<sagMiniGameSever>::sm_Instance
inline constexpr std::array<unsigned char,336> capsuleFingerprint{
#include "gamecode/VrTeleportLoco_268b300_336_8d.inc"
};
inline constexpr std::array<unsigned char,348> groundFingerprint{
#include "gamecode/VrTeleportLoco_245d99c_348_8d.inc"
};
inline constexpr std::array<unsigned char,16> liquidInstanceFingerprint{
#include "gamecode/VrTeleportLoco_28a5fd4_16_8d.inc"
};
inline constexpr std::array<unsigned char,304> waterElevationFingerprint{
#include "gamecode/VrTeleportLoco_28a700c_304_8d.inc"
};
inline constexpr std::array<unsigned char,100> indoorsFingerprint{
#include "gamecode/VrTeleportLoco_245e078_100_8d.inc"
};
inline constexpr std::array<unsigned char,1028> toMatrixFullFingerprint{
#include "gamecode/VrTeleportLoco_1caaa8c_1028_8d.inc"
};
} // namespace elf

// ============================== config (properties, read once at install; menu values every tick) ==============================
struct Config {
    bool fade{true};                                             // .fade 0 = never the colour scale (INSTANT ONLY)
    std::uint32_t outMs{60},inMs{120},cooldownMs{0},holdCapMs{400},holdFrames{3};   // .timing
    float maxUpM{2.6f},maxDownM{3.0f},slopeDeg{35.f},waterM{.35f},minM{.30f};          // .limits (cm)
    float capsuleR{.28f},kneeM{.45f},topM{1.75f};std::uint32_t capsuleMask{0x96337f7du}; // .capsule
    float lowR{.20f};std::uint32_t lowMask{0x96337f7du};std::uint8_t lowState{3};       // .low
    std::uint32_t budgetUs{400};unsigned casts{3};                // .budget (validation wall clock per tick, arc cast share - reported)
    float walk{.6f};std::uint32_t lookbackMs{50};bool arcSurface{},streamFlag{},psi{true},edge{true},bsnap{true};unsigned hand{0};
    bool probeOnly{},facing{};
    std::uint32_t instantCooldownMs{0};
};
namespace detail {
// "a,b,c" -> up to n fields; hexMask bit i = field i is hexadecimal (masks). Missing / bad fields keep their defaults.
inline unsigned parseFields(const char* text,long long* out,unsigned n,unsigned hexMask) noexcept {
    if(!text)return 0;unsigned k=0;const char* s=text;
    while(k<n&&*s){
        while(*s==' ')++s;char* end{};
        const long long v=(hexMask>>k)&1u?(long long)std::strtoull(s,&end,16):std::strtoll(s,&end,10);
        if(end!=s)out[k]=v;
        ++k;s=end;while(*s&&*s!=',')++s;if(*s==',')++s;else break;
    }
    return k;
}
inline bool propertyText(const char* name,char* out,std::size_t n) noexcept {
    const auto fn=arc::propertyReader.load(std::memory_order_acquire);out[0]=0;return fn&&fn(name,out,n)&&out[0];}
} // namespace detail
inline Config readConfig() noexcept {
    Config c{};char t[96];
    const auto one=[&](const char* name,long fallback,long lo,long hi){
        if(!detail::propertyText(name,t,sizeof t))return fallback;char* e{};const long v=std::strtol(t,&e,10);
        return e!=t&&*e==0?std::clamp(v,lo,hi):fallback;};
    c.fade=one("debug.rdrvr.vr.tploco.fade",1,0,1)!=0;
    if(detail::propertyText("debug.rdrvr.vr.tploco.timing",t,sizeof t)){long long f[5]{60,120,250,400,3};detail::parseFields(t,f,5,0);
        c.outMs=std::uint32_t(std::clamp(f[0],0ll,1000ll));c.inMs=std::uint32_t(std::clamp(f[1],0ll,1000ll));c.cooldownMs=std::uint32_t(std::clamp(f[2],0ll,3000ll));
        c.holdCapMs=std::uint32_t(std::clamp(f[3],50ll,1500ll));c.holdFrames=std::uint32_t(std::clamp(f[4],1ll,20ll));}
    if(detail::propertyText("debug.rdrvr.vr.tploco.limits",t,sizeof t)){long long f[5]{260,300,35,35,30};detail::parseFields(t,f,5,0);
        c.maxUpM=float(std::clamp(f[0],0ll,2000ll))/100.f;c.maxDownM=float(std::clamp(f[1],0ll,2000ll))/100.f;c.slopeDeg=float(std::clamp(f[2],1ll,89ll));
        c.waterM=float(std::clamp(f[3],0ll,500ll))/100.f;c.minM=float(std::clamp(f[4],0ll,500ll))/100.f;}
    if(detail::propertyText("debug.rdrvr.vr.tploco.capsule",t,sizeof t)){long long f[4]{28,45,175,0x96337f7dll};detail::parseFields(t,f,4,8u);
        c.capsuleR=float(std::clamp(f[0],5ll,100ll))/100.f;c.kneeM=float(std::clamp(f[1],0ll,150ll))/100.f;c.topM=float(std::clamp(f[2],60ll,300ll))/100.f;
        c.capsuleMask=std::uint32_t(f[3]);if(c.topM-c.capsuleR<c.kneeM+c.capsuleR)c.topM=c.kneeM+2.f*c.capsuleR+.01f;}
    if(detail::propertyText("debug.rdrvr.vr.tploco.low",t,sizeof t)){long long f[3]{20,0x96337f7dll,3};detail::parseFields(t,f,3,2u);
        c.lowR=float(std::clamp(f[0],5ll,100ll))/100.f;c.lowMask=std::uint32_t(f[1]);c.lowState=std::uint8_t(std::clamp(f[2],1ll,7ll));}
    if(detail::propertyText("debug.rdrvr.vr.tploco.budget",t,sizeof t)){long long f[2]{400,3};detail::parseFields(t,f,2,0);
        c.budgetUs=std::uint32_t(std::clamp(f[0],50ll,5000ll));c.casts=unsigned(std::clamp(f[1],1ll,12ll));}
    c.walk=float(one("debug.rdrvr.vr.tploco.walk",60,20,100))/100.f;
    c.lookbackMs=std::uint32_t(one("debug.rdrvr.vr.tploco.lookback",50,0,200));
    c.arcSurface=one("debug.rdrvr.vr.tploco.arcsurface",0,0,1)!=0;c.streamFlag=one("debug.rdrvr.vr.tploco.streamflag",0,0,1)!=0;
    c.psi=one("debug.rdrvr.vr.tploco.psi",1,0,1)!=0;c.edge=one("debug.rdrvr.vr.tploco.edge",1,0,1)!=0;c.bsnap=one("debug.rdrvr.vr.tploco.bsnap",1,0,1)!=0;
    c.hand=unsigned(one("debug.rdrvr.vr.tploco.hand",0,0,1));c.probeOnly=one("debug.rdrvr.vr.tploco.probe",0,0,1)!=0;
    c.facing=one("debug.rdrvr.vr.tploco.facing",0,0,1)!=0;
    return c;
}
// v0 so that a 45 deg throw from `height` above the feet lands at `range`: v^2 = g R^2 / (R + h).
inline float launchSpeed(float rangeM,float heightM=1.2f,float g=9.81f) noexcept {
    return rangeM>0.f?rangeM*std::sqrt(g/std::max(rangeM+heightM,.01f)):0.f;}

// ============================== stick gate (re-arm, engage, aim samples) ==============================
struct StickGate {
    static constexpr float neutral=.35f,engageY=.60f,engageMag=.65f,backY=-.60f;
    static constexpr std::uint64_t rearmNs=msNs(100);
    bool armed{};std::uint64_t neutralSince{};bool waited{};
    void disarm() noexcept {armed=false;neutralSince=0;waited=false;}
    static float magnitude(float x,float y) noexcept {return std::isfinite(x)&&std::isfinite(y)?std::hypot(x,y):0.f;}
    // Every sample while not aiming: armed after >= 100 ms neutral; true = it had to wait for a deflected stick (stick.rearmWaits).
    bool sample(float x,float y,std::uint64_t now) noexcept {
        if(armed)return false;
        if(magnitude(x,y)<neutral){if(!neutralSince)neutralSince=now?now:1;
            if(now>=neutralSince&&now-neutralSince>=rearmNs){armed=true;const bool w=waited;waited=false;return w;}}
        else{neutralSince=0;waited=true;}
        return false;
    }
    static bool engage(float x,float y) noexcept {return y>=engageY&&magnitude(x,y)>=engageMag;}
    enum class Aim : std::uint8_t {Hold,Back,Neutral};
    static Aim classify(float x,float y) noexcept {
        if(magnitude(x,y)<neutral)return Aim::Neutral;
        if(y<=backY)return Aim::Back;
        return Aim::Hold;
    }
};

// ============================== validation ==============================
enum class Reason : std::uint8_t {Ok,HandBlocked,NoHit,NoGround,Obstacle,Steep,TooClose,TooFar,TooHigh,TooLow,Water,Blocked,Edge,ProbeFailed,Count};
inline constexpr unsigned reasonCount=unsigned(Reason::Count);
inline constexpr std::array<const char*,reasonCount> reasonNames{"OK","HAND_BLOCKED","NO_HIT","NO_GROUND","OBSTACLE","STEEP","TOO_CLOSE",
    "TOO_FAR","TOO_HIGH","TOO_LOW","WATER","BLOCKED","EDGE","PROBE_FAILED"};
// panel glyphs for the LOCOMOTION row note
inline constexpr std::array<const char*,reasonCount> reasonNotes{"READY","HAND BLOCKED","NO HIT","NO GROUND","OBSTACLE","STEEP","TOO CLOSE",
    "TOO FAR","TOO HIGH","TOO LOW","WATER","BLOCKED","EDGE","PROBE FAILED"};
// marker look group: Cross = blocked-like, Dot = out of limits, None = no landing
inline constexpr bool outOfLimits(Reason r) noexcept {return r==Reason::TooFar||r==Reason::TooHigh||r==Reason::TooLow||r==Reason::TooClose;}
inline constexpr bool noLanding(Reason r) noexcept {return r==Reason::NoHit||r==Reason::NoGround;}
enum class Cls : std::uint8_t {Floor,Wall,Ceiling};
inline constexpr std::array<const char*,3> clsNames{"FLOOR","WALL","CEILING"};
enum class ProbeOut : std::uint8_t {Miss,Hit,Failed};
// The engine queries of the validator (the engine on the device, or a fake world). All main thread.
struct Probes {
    void* ctx{};
    ProbeOut (*ground)(void*,const V3& a,const V3& b,V3& point,V3& normal) noexcept{};   // FindIntersectionWithMaterial, fixed only, down segment
    int (*capsule)(void*,const V3& a,const V3& b,float radius,std::uint32_t include,std::uint8_t state) noexcept{}; // sTestCapsule hits, < 0 failed
    int (*water)(void*,float x,float z,float& height) noexcept{};                         // 1 = a water surface at (x, z), 0 none, < 0 unavailable
    std::uint64_t (*clock)(void*) noexcept{};                                              // ns (validation budget)
    explicit operator bool() const noexcept {return ground&&capsule;}
};
struct Limits {                         // metres (scaled by upm where they are lengths); guardUnits is engine units
    float rangeM{9.f},maxUpM{2.6f},maxDownM{3.f},slopeCos{.819f},waterM{.35f},minM{.30f};
    float capsuleR{.28f},kneeM{.45f},topM{1.75f};std::uint32_t capsuleMask{0x96337f7du};
    float lowR{.20f};std::uint32_t lowMask{0x96337f7du};std::uint8_t lowState{3};bool edge{true};
    float guardUnits{24.f};
};
inline Limits limitsFrom(const Config& c,float rangeM) noexcept {
    Limits l{};l.rangeM=rangeM;l.maxUpM=c.maxUpM;l.maxDownM=c.maxDownM;l.slopeCos=std::cos(c.slopeDeg*3.14159265f/180.f);l.waterM=c.waterM;l.minM=c.minM;
    l.capsuleR=c.capsuleR;l.kneeM=c.kneeM;l.topM=c.topM;l.capsuleMask=c.capsuleMask;l.lowR=c.lowR;l.lowMask=c.lowMask;l.lowState=c.lowState;l.edge=c.edge;
    return l;
}
struct Geometry {V3 up{0,1,0};float upm{1.f};V3 C{};V3 arcDir{};};   // C = capsule base (player matrix d); arcDir = the arc's direction (any length)
struct Validation {
    Reason reason{Reason::NoHit};Cls cls{Cls::Floor};V3 H{},n{},G{},nG{};
    float distM{},dyM{},waterDepthM{-1.f};int capsuleHits{-1},lowHits{-1},edgeFound{-1};
    bool ground{},cheap{},full{};       // stages done (ground: G known; cheap: slope / distance / height / water; full: capsule, low, edge)
    unsigned probes{};
    bool ok() const noexcept {return reason==Reason::Ok&&full;}
};
struct ValidatorStats {Counter groundCalls{},groundNsSum{},capsuleCalls{},capsuleNsSum{},capsuleNsMax{},waterCalls{},failures{},budgetHits{},fullRuns{};};
inline ValidatorStats validatorStats;

namespace vdetail {
inline float along(const V3& v,const V3& up) noexcept {return am::dot(v,up);}
inline V3 horizontal(const V3& v,const V3& up) noexcept {return am::sub(v,am::mul(up,am::dot(v,up)));}
inline std::uint64_t clockOf(const Probes& p) noexcept {return p.clock?p.clock(p.ctx):0;}
inline ProbeOut ground(const Probes& p,const V3& a,const V3& b,V3& pt,V3& n,Validation& v) noexcept {
    ++v.probes;const auto t0=clockOf(p);ProbeOut r=ProbeOut::Failed;
    try{r=p.ground?p.ground(p.ctx,a,b,pt,n):ProbeOut::Failed;}catch(...){r=ProbeOut::Failed;}
    const auto dt=clockOf(p)-t0;bump(validatorStats.groundCalls);validatorStats.groundNsSum.fetch_add(dt,std::memory_order_relaxed);
    if(r==ProbeOut::Hit&&(!am::finite(pt)||!am::finite(n)))r=ProbeOut::Failed;
    if(r==ProbeOut::Failed)bump(validatorStats.failures);
    return r;
}
inline int capsule(const Probes& p,const V3& a,const V3& b,float r,std::uint32_t include,std::uint8_t state,Validation& v) noexcept {
    ++v.probes;const auto t0=clockOf(p);int n=-1;
    try{n=p.capsule?p.capsule(p.ctx,a,b,r,include,state):-1;}catch(...){n=-1;}
    const auto dt=clockOf(p)-t0;bump(validatorStats.capsuleCalls);validatorStats.capsuleNsSum.fetch_add(dt,std::memory_order_relaxed);maxOf(validatorStats.capsuleNsMax,dt);
    if(n<0)bump(validatorStats.failures);
    return n;
}
} // namespace vdetail

// Step 2 (floor / wall / ceiling -> the fixed ground G under the landing; never a roof / slab top above H) + steps 3-7 (slope, distance,
// height, water). True = G known (the reason may still be invalid).
inline bool validateGround(const Limits& L,const Geometry& g,const V3& H,const V3& nIn,bool nValid,const Probes& P,Validation& v) noexcept {
    using namespace vdetail;
    v=Validation{};v.H=H;v.n=nIn;
    const V3 up=g.up;const float u=std::max(g.upm,.01f);
    const V3 n=nValid?am::normalized(nIn):up;
    const float ny=along(n,up);const V3 hn=horizontal(n,up);const float hl=am::length(hn);
    V3 a{},b{},pt{},nn{};
    const float downTo=along(g.C,up)-(L.maxDownM+.5f)*u;   // the floor search never goes deeper than the TOO_LOW limit + 0.5 m
    const auto downEnd=[&](const V3& s){const float sy=along(s,up);const float ey=std::min(sy-.2f*u,downTo);return am::add(s,am::mul(up,ey-sy));};
    bool wall=false;V3 hdir{};
    if(ny>=.5f){                                                         // 2a floor
        v.cls=Cls::Floor;a=am::add(H,am::mul(up,.35f*u));b=am::sub(H,am::mul(up,.35f*u));
        const auto r=ground(P,a,b,pt,nn,v);
        if(r==ProbeOut::Failed){v.reason=Reason::ProbeFailed;return false;}
        if(r==ProbeOut::Miss){v.reason=Reason::NoGround;return false;}
        if(std::fabs(along(am::sub(pt,H),up))>.10f*u){v.G=pt;v.nG=nn;v.ground=true;v.reason=Reason::Obstacle;return true;} // 2b
    }else{
        if(ny<-.3f||hl<.3f){                                             // 2d ceiling
            v.cls=Cls::Ceiling;
            const V3 S=am::add(H,am::mul(n,.30f*u));
            if(along(am::sub(S,H),up)<=-.15f*u){a=S;b=downEnd(S);}
            else{wall=true;hdir=am::mul(am::normalized(horizontal(g.arcDir,up)),-1.f);}
        }else{wall=true;hdir=am::mul(hn,1.f/std::max(hl,1e-6f));v.cls=Cls::Wall;}
        if(wall){                                                         // 2c wall: the floor in front of the wall's foot
            if(am::length(hdir)<.5f)hdir=am::normalized(horizontal(n,up));
            const V3 S=am::sub(am::add(H,am::mul(hdir,.40f*u)),am::mul(up,.15f*u));a=S;b=downEnd(S);
        }
        const auto r=ground(P,a,b,pt,nn,v);
        if(r==ProbeOut::Failed){v.reason=Reason::ProbeFailed;return false;}
        if(r==ProbeOut::Miss){v.reason=Reason::NoGround;return false;}
    }
    v.G=pt;v.nG=am::normalized(nn);v.ground=true;
    // 3..6
    const V3 d=am::sub(v.G,g.C);const float hd=am::length(horizontal(d,up))/u,dy=along(d,up)/u;v.distM=hd;v.dyM=dy;
    if(along(v.nG,up)<L.slopeCos)v.reason=Reason::Steep;
    else if(hd<L.minM)v.reason=Reason::TooClose;
    else if(hd>L.rangeM||am::length(d)>=L.guardUnits)v.reason=Reason::TooFar;   // 3D < 24 engine units (independent of upm)
    else if(dy>L.maxUpM)v.reason=Reason::TooHigh;
    else if(-dy>L.maxDownM)v.reason=Reason::TooLow;
    else{
        v.reason=Reason::Ok;
        if(P.water){float h{};int w=-1;++v.probes;try{w=P.water(P.ctx,v.G[0],v.G[2],h);}catch(...){w=-1;}bump(validatorStats.waterCalls);
            if(w>0&&std::isfinite(h)){v.waterDepthM=(h-along(v.G,up))/u;if(v.waterDepthM>L.waterM)v.reason=Reason::Water;}}
    }
    v.cheap=true;
    return true;
}
// Steps 8-10 from `step` (0 capsule, 1 low capsule, 2..5 edge probes); stops when `deadline` (clock ns, 0 = none) passed BETWEEN probes.
// Returns the next step (6 = done -> v.full).
inline unsigned validateFull(const Limits& L,const Geometry& g,const Probes& P,Validation& v,unsigned step,std::uint64_t deadline,int& edgeOk) noexcept {
    using namespace vdetail;
    if(!v.ground||v.reason!=Reason::Ok)return 6;
    const V3 up=g.up;const float u=std::max(g.upm,.01f);
    const auto over=[&]{return deadline&&clockOf(P)>=deadline;};
    for(;step<6;++step){
        if(over()){bump(validatorStats.budgetHits);return step;}
        if(step==0){                                                     // 8 standing capsule (knee .. top)
            const V3 a=am::add(v.G,am::mul(up,(L.kneeM+L.capsuleR)*u)),b=am::add(v.G,am::mul(up,(L.topM-L.capsuleR)*u));
            const int n=capsule(P,a,b,L.capsuleR*u,L.capsuleMask,7,v);v.capsuleHits=n;
            if(n<0){v.reason=Reason::ProbeFailed;return 6;}
            if(n>0){v.reason=Reason::Blocked;return 6;}
        }else if(step==1){                                               // 9 low obstacle (non-fixed, below the knee)
            const V3 a=am::add(v.G,am::mul(up,.24f*u)),b=am::add(v.G,am::mul(up,.26f*u));
            const int n=capsule(P,a,b,L.lowR*u,L.lowMask,L.lowState,v);v.lowHits=n;
            if(n<0){v.reason=Reason::ProbeFailed;return 6;}
            if(n>0){v.reason=Reason::Blocked;return 6;}
            edgeOk=0;
            if(!L.edge){v.edgeFound=-1;step=5;}                          // edge test off
        }else{                                                           // 10 edge / beam: 4 x ground at +-0.25 m along two horizontal axes
            V3 x=horizontal(V3{1,0,0},up);if(am::length(x)<.5f)x=horizontal(V3{0,0,1},up);x=am::normalized(x);const V3 z=am::cross(x,up);
            const unsigned k=step-2;const V3 off=am::mul(k<2?x:z,(k%2?-.25f:.25f)*u);
            const V3 c=am::add(v.G,off);V3 pt{},nn{};
            const auto r=ground(P,am::add(c,am::mul(up,.30f*u)),am::sub(c,am::mul(up,.30f*u)),pt,nn,v);
            if(r==ProbeOut::Failed){v.reason=Reason::ProbeFailed;return 6;}
            if(r==ProbeOut::Hit&&std::fabs(along(am::sub(pt,v.G),up))<=.30f*u)++edgeOk;
            if(step==5){v.edgeFound=edgeOk;if(edgeOk<3){v.reason=Reason::Edge;return 6;}}
        }
    }
    v.full=true;bump(validatorStats.fullRuns);return 6;
}
// Synchronous full validation (commit / warp): no budget.
inline Validation validateAll(const Limits& L,const Geometry& g,const V3& H,const V3& n,bool nValid,const Probes& P) noexcept {
    Validation v{};if(!validateGround(L,g,H,n,nValid,P,v))return v;
    int e=0;validateFull(L,g,P,v,0,0,e);return v;
}

// Preview (aiming): ground when the landing moved > 10 cm (at most every 2nd tick), the full checks once it is stable >= 100 ms; a wall-clock
// budget checked between probes, the rest continues next tick (the marker stays "validating").
class Previewer {
    Validation v_{};bool have_{};V3 groundH_{};std::uint64_t groundTick_{},stableSince_{};unsigned fullStep_{6};int edgeOk_{};bool stale_{};
public:
    void reset() noexcept {*this=Previewer{};}
    const Validation& last() const noexcept {return v_;}
    bool stale() const noexcept {return stale_;}
    // returns the result to show; `fresh` = this tick computed something new
    const Validation& run(const Limits& L,const Geometry& g,const arc::Trace& t,const Probes& P,std::uint64_t now,std::uint64_t tick,
                          std::uint64_t budgetNs) noexcept {
        stale_=false;
        if(t.handBlocked){v_=Validation{};v_.reason=Reason::HandBlocked;have_=false;return v_;}
        if(!t.hit){v_=Validation{};v_.reason=Reason::NoHit;have_=false;return v_;}
        const float u=std::max(g.upm,.01f);const V3 H=t.landing.point;
        const std::uint64_t start=vdetail::clockOf(P),deadline=budgetNs?start+budgetNs:0;
        if(!have_||am::distance(H,groundH_)>.10f*u){
            if(have_&&tick==groundTick_+1){stale_=true;return v_;}       // at most every 2nd tick: keep showing "validating"
            validateGround(L,g,H,t.landing.normal,t.landing.normalValid,P,v_);
            have_=true;groundH_=H;groundTick_=tick;stableSince_=now;fullStep_=v_.reason==Reason::Ok?0u:6u;edgeOk_=0;
            return v_;
        }
        if(v_.reason==Reason::Ok&&!v_.full&&fullStep_<6&&now>=stableSince_&&now-stableSince_>=msNs(100))
            fullStep_=validateFull(L,g,P,v_,fullStep_,deadline,edgeOk_);
        return v_;
    }
};

// ============================== History (last 4 shown traces, for the lookback) ==============================
enum class Shown : std::uint8_t {None,Pending,Validating,Valid,Invalid};
inline constexpr std::array<const char*,5> shownNames{"NONE","PENDING","VALIDATING","VALID","INVALID"};
struct Entry {arc::Trace trace{};Validation v{};std::uint64_t ns{};Shown shown{Shown::None};V3 launchOrigin{},launchVelocity{};};
struct History {
    std::array<Entry,4> e{};unsigned count{},head{};
    void clear() noexcept {count=0;head=0;}
    void push(const Entry& x) noexcept {e[head]=x;head=(head+1)%e.size();count=std::min<unsigned>(count+1,unsigned(e.size()));}
    const Entry* newest(unsigned back=0) const noexcept {if(back>=count)return nullptr;return &e[(head+unsigned(e.size())-1-back)%e.size()];}
    // the newest entry shown at or before `target`; else the oldest one kept
    const Entry* pick(std::uint64_t target) const noexcept {
        for(unsigned k=0;k<count;++k){const auto* x=newest(k);if(x->ns<=target)return x;}
        return count?newest(count-1):nullptr;
    }
};

// ============================== engine side (injected) ==============================
struct PlayerFacts {
    const char* reason{"NO SAMPLE"};std::uint32_t guid{};V3 C{};float headingDeg{};
    bool moverRead{},recovering{},jumping{};float vy{};bool velocityKnown{};
};
struct Env {
    void* ctx{};
    bool (*player)(void*,bool strict,PlayerFacts&) noexcept{};                 // samplePlayer (+ mover recovery / jump bits, velocity)
    int (*lasso)(void*,std::uint32_t& userState) noexcept{};                   // 1 busy (aniLasso user state), 0 free, -1 unreadable
    int (*minigame)(void*) noexcept{};                                          // 1 focus, 0 none, -1 unknown
    int (*locoGate)(void*,std::uint64_t now,const char*& name) noexcept{};     // 1 open, 2 transient (SETTLING), 0 closed (name), -1 no mover hook
    bool (*warp)(void*,std::uint32_t guid,const V3& pos,float headingDeg) noexcept{}; // TeleportActorWithHeading(false, false, false)
    int (*viewTurn)(float radians,std::uint64_t& turnNs) noexcept{};           // teleportViewTurn (1 applied, 0 retry, -1 declined)
    int (*streamFlag)(void*,int write) noexcept{};                              // write < 0: read the streamer's one-frame flag; else write it
    int (*playerTeleporting)(void*,bool clear) noexcept{};                      // g_playerTeleporting (read, clear)
    int (*indoors)(void*,const V3& p) noexcept{};                               // 1 / 0, -1 no world (report only)
    int (*crouching)(void*) noexcept{};                                         // 1 / 0 / -1 (report only)
    Probes probes{};
    explicit operator bool() const noexcept {return player&&warp&&bool(probes);}
};

// ============================== the module (main thread) ==============================
enum class State : std::uint8_t {Off,Idle,Blocked,Walk,Aiming,ReleaseHold,CommitPending,FadeOut,WarpPending,Hold,FadeIn,Cooldown,Count};
inline constexpr unsigned stateCount=unsigned(State::Count);
inline constexpr std::array<const char*,stateCount> stateNames{"OFF","IDLE","BLOCKED","WALK","AIMING","RELEASE_HOLD","COMMIT_PENDING","FADE_OUT",
    "WARP_PENDING","HOLD","FADE_IN","COOLDOWN"};
enum Gate : unsigned {GateMenu,GateNotWorld,GateNative,GateLasso,GateSettling,GateLoco,GatePlayer,GateCount};
inline constexpr std::array<const char*,GateCount> gateNames{"MENU","NOT_WORLD","NATIVE","LASSO","SETTLING","LOCO","PLAYER"};
enum Abort : unsigned {AbortConfirmTimeout,AbortGate,AbortCapsule,AbortPlayer,AbortWatchdog,AbortCommit,AbortCount};
inline constexpr std::array<const char*,AbortCount> abortNames{"CONFIRM_TIMEOUT","GATE","CAPSULE","PLAYER","WATCHDOG","COMMIT"};
enum Cancel : unsigned {CancelBack,CancelFlick,CancelGate,CancelMenu,CancelWalk,CancelAirborne,CancelUntracked,CancelStale,CancelCount};
inline constexpr std::array<const char*,CancelCount> cancelNames{"back","flick","gate","menu","walk","airborne","untracked","stale"};
enum Reject : unsigned {RejectChanged,RejectFailed,RejectTimeout,RejectNoContext,RejectValidation,RejectCount};
inline constexpr std::array<const char*,RejectCount> rejectNames{"changed","failed","timeout","noContext","validation"};

struct Stats {
    Counter ticks{},exceptions{};std::array<Counter,stateCount> stateTicks{};std::array<Counter,GateCount> gates{};
    Counter stickStale{},stickEngage{},rearmWaits{},yieldedMinigame{},walkFrames{},walkSessions{},blockedYieldFrames{},hintsWalk{},fallbackFree{};
    Counter aimSessions{},aimFrames{},aimCasts{},aimChordsStale{},aimHits{},aimPendingFrames{},aimValidatingFrames{},guardTrips{},budgetShrinks{};
    std::array<Counter,reasonCount> reasons{};
    Counter aimUsSum{},aimUsMax{},aimUsFrames{};
    Counter commitRequests{},commitAccepted{},commitCasts{},commitUsSum{},commitUsMax{};std::array<Counter,RejectCount> commitRejected{};
    Counter releasesValid{},releasesOvershoot{},releasesCommitChanged{},releasesProbeOnly{};std::array<Counter,reasonCount> releasesInvalid{};
    std::array<Counter,CancelCount> cancels{};
    Counter warpsRequested{},warpsExecuted{},warpsOk{},warpsMissed{},warpsFell{},flagOwned{},streamFlagRestored{},indoorChanges{},crouchLost{};
    Counter restOffsetFallback{},recoveryMsSum{},recoveryN{},settleMsSum{},settleN{},settleDyUmMax{},holdCapHits{},psiDeltaUdegMax{},psiCorrected{},
        psiTurnDeclined{},psiSignFlips{},pairFallbacks{},indoorsNoWorld{};
    Counter psiTurnUserSkipped{};   // Warps whose HOLD saw a player snap turn since the commit (no psi correction / sign learning)
    std::array<Counter,AbortCount> aborts{};
    Counter fadeStarts{},fadeConfirms{},confirmWaitUsSum{},blackUsSum{},blackUsMax{},totalUsSum{},fadeIns{},instantOnly{},fadeFailures{};
    Counter blinkWarps{};           // BLINK warps whose black / total time is in blackUsSum / totalUsSum (the averages' denominator)
    Counter refusals{},validHaptics{},arcPublished{},arcHidden{};
    Counter arcArms{},noArcTicks{};   // Channel arm edges, ticks spent in "NO ARC" (FREE)
    Counter frameUsAimSum{},frameAimN{},frameUsIdleSum{},frameIdleN{};
};
inline Stats stats;
struct LastWarp {float from[3]{},target[3]{},after[3]{};float dist{},errorM{},dyM{},restOffsetM{},ageS{},frameMsBefore{},frameMsAfter{},psiDeltaDeg{};
                 std::uint64_t serial{},ns{};bool ok{},done{};};
struct LastAim {Reason reason{Reason::NoHit};Cls cls{};V3 H{},n{},G{},nG{};float dist{},dy{},waterDepth{-1};int capsuleHits{-1},lowHits{-1},edgeFound{-1};};

class Module {
public:
    Config cfg{};bool configured{};bool installed{};Env env{};
    // published (report; any thread reads through atomics below)
    std::atomic<unsigned> stateNow{0};std::atomic<const char*> noteNow{""};std::atomic<float> lastX{},lastY{};
    std::atomic<float> lastRestOffset{-1.f},lastDy{0.f},settleDyMaxM{0.f};std::atomic<float> v0Now{0.f},upmNow{1.f},rangeNow{9.f};
    std::atomic<bool> blinkNow{true},instantOnlyNow{false};
    body::Seqlock<LastWarp> lastWarp;body::Seqlock<LastAim> lastAim;
private:
    State st_{State::Off};StickGate gate_{};arc::Tracer tracer_{};arc::DirectionFilter filter_{};Previewer preview_{};History history_{};
    std::uint64_t enterNs_{},aimStartNs_{},blockedSinceNs_{};const char* blockedNote_{""};bool blockedTransient_{};unsigned backCount_{};
    bool arcShown_{},armedArc_{};unsigned validStreak_{};std::uint64_t lastValidHapticNs_{};bool wasValid_{};
    std::uint64_t lastValidShownNs_{};
    // refusal display
    hs::ArcFrame refuseFrame_{};std::uint64_t refuseUntilNs_{};bool refuseShown_{};
    // commit
    std::uint64_t serial_{};Entry picked_{};bool accepted_{};arc::Hit committed_{};Validation commitV_{};hs::ArcFrame frozen_{};
    std::uint64_t commitStartNs_{},releaseHoldNs_{};unsigned commitTicks_{};bool fromValid_{},blink_{true};
    // fade / warp / hold
    std::uint64_t aimGateSinceNs_{};
    std::uint64_t fadeStartNs_{},blackNs_{},warpNs_{},warpFrame_{},holdStartNs_{},fadeInNs_{},cooldownNs_{};std::uint32_t cooldownMs_{250};
    unsigned fadeFailStreak_{};bool warped_{};V3 target_{},before_{};float restOffset_{};
    unsigned settleTicks_{};bool recovered_{};std::uint64_t recoveredNs_{};bool settled_{};bool postOk_{};bool pairOk_{};bool pairFallback_{};
    std::array<float,3> psiHist_{};unsigned psiCount_{};std::uint32_t psiFrames_{};unsigned psiAttempts_{};float psiDeltaBefore_{};std::uint64_t psiWaitFrame_{};
    int psiSign_{1};bool psiDone_{};
    std::uint64_t userTurns0_{};bool psiUserSkipped_{};   // warp::userTurns at the commit; the HOLD saw a player snap turn (counted once)
    bool aimLost_{};                                       // The last aim tick hid the arc (hand untracked / no input / no world)
    // post-warp tracker (1 s)
    bool post_{};std::uint64_t postSerial_{};int crouchBefore_{-1},indoorsBefore_{-1};float postDyMax_{};bool fell_{};
    // misc
    unsigned failStreak_{};std::uint64_t hintUntilNs_{},lastHintNs_{},secondPulseNs_{};bool fallbackCued_{};
    std::uint64_t lastTickNs_{},guardUntilFrame_{};double aimFrameMs_{},idleFrameMs_{};bool shrink_{},aimHinted_{};
    V3 warpUp_{0,1,0};float warpUpm_{1.f};std::uint64_t lastBlackUs_{};unsigned gateReadTick_{};bool lassoBusy_{},minigame_{};
    const char* note_{""};
public:
    State state() const noexcept {return st_;}
    unsigned failStreak() const noexcept {return failStreak_;}
    bool arcArmed() const noexcept {return armedArc_;}
    std::uint64_t lastBlackUs() const noexcept {return lastBlackUs_;}
    std::uint64_t serial() const noexcept {return serial_;}
    int psiSign() const noexcept {return psiSign_;}
    void resetForTests() noexcept {
        const auto e=env;const bool inst=installed;const auto c=cfg;
        st_=State::Off;gate_={};tracer_.reset();filter_.reset();preview_.reset();history_.clear();enterNs_=aimStartNs_=blockedSinceNs_=0;blockedNote_="";
        blockedTransient_=false;backCount_=0;arcShown_=armedArc_=false;validStreak_=0;lastValidHapticNs_=0;wasValid_=false;lastValidShownNs_=0;
        refuseFrame_={};refuseUntilNs_=0;refuseShown_=false;serial_=0;picked_={};accepted_=false;committed_={};commitV_={};frozen_={};
        commitStartNs_=releaseHoldNs_=0;commitTicks_=0;fromValid_=false;blink_=true;fadeStartNs_=blackNs_=warpNs_=warpFrame_=holdStartNs_=fadeInNs_=cooldownNs_=0;
        fadeFailStreak_=0;warped_=false;target_=before_={};restOffset_=0;settleTicks_=0;recovered_=false;recoveredNs_=0;settled_=postOk_=pairOk_=pairFallback_=false;
        psiHist_={};psiCount_=0;psiFrames_=0;psiAttempts_=0;psiDeltaBefore_=0;psiWaitFrame_=0;psiSign_=1;psiDone_=false;post_=false;postSerial_=0;
        userTurns0_=0;psiUserSkipped_=false;aimLost_=false;
        crouchBefore_=indoorsBefore_=-1;postDyMax_=0;fell_=false;failStreak_=0;hintUntilNs_=lastHintNs_=secondPulseNs_=0;fallbackCued_=false;lastTickNs_=0;
        guardUntilFrame_=0;aimFrameMs_=idleFrameMs_=0;shrink_=aimHinted_=false;warpUp_={0,1,0};warpUpm_=1.f;lastBlackUs_=0;gateReadTick_=0;lassoBusy_=minigame_=false;note_="";
        env=e;installed=inst;cfg=c;
    }
    void tick(const mainthread::Frame& fr) noexcept;
private:
    // ---- helpers ----
    void pulse(float a,std::uint32_t us,std::uint64_t now) noexcept {haptics::pulse(cfg.hand&1u,a,us,now,haptics::SourceTeleport);}
    void enter(State s,std::uint64_t now) noexcept {st_=s;enterNs_=now;}
    void hideArc() noexcept {if(arcShown_){hs::clearArc(hs::ArcChannel::Teleport);arcShown_=false;bump(stats.arcHidden);}}
    void publish(hs::ArcFrame& f,const arc::Context& c) noexcept {f.key=c.key();if(!f.key){bump(stats.arcHidden);return;} // A frame without an anchor key keeps the last arc (hiding it would flicker)
   hs::publishArc(hs::ArcChannel::Teleport,f);arcShown_=f.count>0;bump(stats.arcPublished);}
    float v0() const noexcept {return launchSpeed(rangeNow.load(std::memory_order_relaxed));}
    arc::TraceConfig traceConfig() const noexcept {
        arc::TraceConfig t{};t.chordDtS=.12f;t.maxChords=16;t.pointsPerChord=4;t.losFromEye=true;t.losSelect=arc::Select::AnySurface;
        t.query.include=0x500208;t.query.select=cfg.arcSurface?arc::Select::AnySurface:arc::Select::BulletStopper;t.user=arc::ProbeUser::Teleport;return t;}
    Limits limits() const noexcept {return limitsFrom(cfg,rangeNow.load(std::memory_order_relaxed));}
    bool playerFacts(bool strict,PlayerFacts& p) noexcept {p=PlayerFacts{};if(!env.player)return false;try{return env.player(env.ctx,strict,p);}catch(...){return false;}}
    static bool airborne(const PlayerFacts& p) noexcept {return p.jumping||(p.velocityKnown&&std::fabs(p.vy)>1.5f);}
    arc::Look lookFor(const Validation& v,Shown s) const noexcept {
        if(s==Shown::Valid)return arc::teleportLook(true);
        if(s==Shown::Validating||s==Shown::Pending)return arc::teleportValidatingLook();
        arc::Look l=arc::teleportLook(false);
        if(outOfLimits(v.reason))l.marker.shape=hs::ArcMarkerShape::Dot;
        else if(noLanding(v.reason)||v.reason==Reason::HandBlocked)l.marker.shape=hs::ArcMarkerShape::None;
        return l;
    }
    // the marker sits on G (the floor found under / in front of the hit), not on the raw arc hit
    static void markerOnGround(hs::ArcFrame& f,const Validation& v) noexcept {
        if(f.marker.shape==hs::ArcMarkerShape::None||!v.ground)return;
        for(unsigned i=0;i<3;++i){f.marker.centre[i]=v.G[i];f.marker.normal[i]=v.nG[i];}
    }
    void buildFrame(const arc::Context& c,const Entry& e,hs::ArcFrame& out,bool refused=false) noexcept {
        const Shown s=refused?Shown::Invalid:e.shown;
        Validation v=e.v;if(refused&&v.reason==Reason::Ok)v.reason=Reason::Blocked;
        arc::Look l=lookFor(v,s);if(refused&&l.marker.shape==hs::ArcMarkerShape::None&&e.trace.hit)l.marker.shape=hs::ArcMarkerShape::Cross;
        arc::toFrame(c,tracer_,e.trace,l,s==Shown::Valid,out);markerOnGround(out,v);
    }
    void startRefusal(const arc::Context& c,const Entry& e,std::uint64_t now) noexcept {
        buildFrame(c,e,refuseFrame_,true);refuseUntilNs_=now+msNs(300);refuseShown_=refuseFrame_.count>0;
        if(refuseShown_)publish(refuseFrame_,c);else hideArc();
        pulse(.25f,60'000,now);bump(stats.refusals);
        ++failStreak_;
        if(failStreak_>=3)hint(now);
    }
    void hint(std::uint64_t now) noexcept {
        hintUntilNs_=now+msNs(5000);
        if(!lastHintNs_||now-lastHintNs_>=msNs(30000)){lastHintNs_=now;pulse(.5f,40'000,now);secondPulseNs_=now+msNs(120);bump(stats.hintsWalk);}
    }
    void fadeCommand(std::uint8_t phase,std::uint64_t t0,std::uint64_t now) noexcept {
        fade::Command c{};c.serial=serial_;c.phase=phase;c.t0=t0;c.heartbeatNs=now;c.outMs=cfg.outMs;c.inMs=cfg.inMs;fade::command.store(c);}
    void heartbeat(std::uint64_t now) noexcept {fade::Command c{};if(fade::command.load(c)&&c.phase!=fade::None&&c.serial==serial_){c.heartbeatNs=now;fade::command.store(c);}}
    void fadeNone(std::uint64_t now) noexcept {fadeCommand(fade::None,now,now);}
    // IN from the current alpha (an abort mid-OUT never darkens first)
    void fadeInFromCurrent(std::uint64_t now) noexcept {
        fade::Command c{};float a=0.f;
        if(fade::command.load(c)&&c.serial==serial_&&c.phase==fade::Out)a=fade::curve(fade::Out,c.t0,c.outMs,now);
        else if(c.serial==serial_&&c.phase==fade::In)a=fade::curve(fade::In,c.t0,c.inMs,now);
        float lo=0.f,hi=1.f;for(int i=0;i<24;++i){const float m=.5f*(lo+hi);(fade::smoothstep(m)<a?lo:hi)=m;}
        const std::uint64_t back=std::uint64_t(double(lo)*double(cfg.inMs)*1e6);
        fadeCommand(fade::In,now>back?now-back:0,now);fadeInNs_=now>back?now-back:0;bump(stats.fadeIns);
    }
    bool blinkAvailable() const noexcept {return cfg.fade&&fade::extensionEnabled.load(std::memory_order_relaxed)==1&&fadeFailStreak_<3;}
    // countTarget false: the caller shows / counts the refused target itself (startRefusal), so it is not counted twice
    void abortTo(Abort why,std::uint64_t now,bool fadeActive,bool countTarget=true) noexcept {
        bump(stats.aborts[why]);hideArc();
        if(fadeActive){fadeInFromCurrent(now);enter(State::FadeIn,now);}
        else{fadeNone(now);cooldownNs_=now;cooldownMs_=cfg.cooldownMs;enter(State::Cooldown,now);}
        if(countTarget&&!warped_&&(why==AbortCapsule||why==AbortCommit)){++failStreak_;if(failStreak_>=3)hint(now);}
    }
    void finishRefused(const arc::Context& c,std::uint64_t now) noexcept {startRefusal(c,picked_,now);gate_.disarm();enter(State::Idle,now);}
    void cancelAim(Cancel why,std::uint64_t now,State to) noexcept {bump(stats.cancels[why]);arc::noteCommitCancelled();hideArc();gate_.disarm();enter(to,now);}
    // ---- per-state work ----
    void aimTick(const mainthread::Frame& fr,arc::Context& c,const PlayerFacts& p,bool haveP) noexcept;
    void release(const mainthread::Frame& fr,arc::Context& c,const StickSample& s,bool overshoot) noexcept;
    void commit(const mainthread::Frame& fr,arc::Context& c) noexcept;
    bool commitTick(const mainthread::Frame& fr,arc::Context& c) noexcept;   // true while still pending
    void doWarp(const mainthread::Frame& fr,arc::Context& c,bool instant) noexcept;
    void holdTick(const mainthread::Frame& fr,arc::Context& c) noexcept;
    void postTick(std::uint64_t now) noexcept;
    struct Gates {bool open{true};unsigned gate{GateCount};const char* note{""};bool transient{};bool lasso{};bool minigame{};bool menu{};bool foot{};bool l3{};};
    Gates gates(const StickSample& s,bool fresh,const arc::Context& c,std::uint64_t now) noexcept;
};
inline Module module;

// ============================== the tick ==============================
inline Module::Gates Module::gates(const StickSample& s,bool fresh,const arc::Context& c,std::uint64_t now) noexcept {
    Gates g{};g.foot=fresh&&s.ordinaryFoot;g.l3=fresh&&s.l3;g.menu=fresh&&s.menu;
    // gate 4 first (it decides the stick route even when other gates are closed). The engine reads are guarded (process_vm_readv on the
    // device): every tick while an aim / commit / blink runs, every 4th tick otherwise (<= 110 ms late for a lasso catch or a minigame).
    const bool busy=st_!=State::Idle&&st_!=State::Blocked&&st_!=State::Walk&&st_!=State::Cooldown;
    if(busy||++gateReadTick_>=4){gateReadTick_=0;
        std::uint32_t lassoState{};int lasso=0;if(env.lasso){try{lasso=env.lasso(env.ctx,lassoState);}catch(...){lasso=-1;}}
        int mg=0;if(env.minigame){try{mg=env.minigame(env.ctx);}catch(...){mg=-1;}}
        lassoBusy_=lasso>0;minigame_=mg>0;}
    g.minigame=minigame_;g.lasso=lassoBusy_||g.minigame||(externalBusy.load(std::memory_order_relaxed)&1u);
    const auto close=[&](unsigned which,const char* note,bool transient){if(g.open){g.open=false;g.gate=which;g.note=note;g.transient=transient;}};
    if(g.menu)close(GateMenu,"MENU",true);
    else if(!fresh||!s.world||!c.world())close(GateNotWorld,"NATIVE",true);
    if(!g.foot)close(GateNative,"NATIVE",true);
    if(g.lasso)close(GateLasso,"LASSO",true);
    if(g.open&&env.locoGate){
        const char* name="LOCKED";int r=-1;try{r=env.locoGate(env.ctx,now,name);}catch(...){r=0;name="LOCKED";}
        if(r==2)close(GateSettling,"SETTLING",true);
        else if(r==0)close(GateLoco,name&&*name?name:"LOCKED",false);
    }
    return g;
}


inline void Module::tick(const mainthread::Frame& fr) noexcept {
    try{
        bump(stats.ticks);const auto now=fr.now;const auto t0=nowNs();
        if(!configured){cfg=readConfig();configured=true;walkScale.store(cfg.walk,std::memory_order_relaxed);}
        // frame-interval monitor (UpdateInputs -> UpdateInputs): aiming vs idle (aim.frameMsAiming / frameMsIdle)
        if(lastTickNs_&&now>lastTickNs_){const double ms=double(now-lastTickNs_)/1e6;
            if(ms<200.){if(st_==State::Aiming){bump(stats.frameAimN);stats.frameUsAimSum.fetch_add(std::uint64_t(ms*1000.),std::memory_order_relaxed);
                    aimFrameMs_=aimFrameMs_>0.?aimFrameMs_+(ms-aimFrameMs_)*.05:ms;}
                else{bump(stats.frameIdleN);stats.frameUsIdleSum.fetch_add(std::uint64_t(ms*1000.),std::memory_order_relaxed);
                    idleFrameMs_=idleFrameMs_>0.?idleFrameMs_+(ms-idleFrameMs_)*.02:ms;}}}
        lastTickNs_=now;
        if(secondPulseNs_&&now>=secondPulseNs_){secondPulseNs_=0;pulse(.5f,40'000,now);}
        const auto vs=settings();
        const bool teleport=vs.teleportLocomotion();
        rangeNow.store(vs.teleportRangeMetres(),std::memory_order_relaxed);v0Now.store(launchSpeed(vs.teleportRangeMetres()),std::memory_order_relaxed);
        blink_=vs.teleportBlink();blinkNow.store(blink_,std::memory_order_relaxed);
        instantOnlyNow.store(blink_&&!blinkAvailable(),std::memory_order_relaxed);
        if(post_)postTick(now);
        // ---- gate 1: mode / kill / install / probe API / arc status (Failed / Killed / Building > 3 s -> FREE "NO ARC") ----
        const char* offNote=nullptr;bool noArc=false;
        const bool deselected=!teleport||killed(Feature::TpLoco);
        if(deselected)offNote="";
        else if(!installed||!env)offNote="UNAVAILABLE - FREE";
        else{
            const auto* api=arc::engineApi.load(std::memory_order_acquire);
            if(!api||!*api)offNote="NO PROBE";
            else{
                // Arm the TELEPORT channel BEFORE the arc status test. With HANDS / LASER OFF the idle
                // pass reports BUILDING ("IDLE") since the session started; only an armed channel
                // prewarms the slot, and the 0 -> armed edge restarts the BUILDING age (HandsPass
                // armArc). Testing first would let "Building > 3 s -> NO ARC" fire on the first tick of
                // a selection made > 3 s into the session, and the channel would never be armed.
                // Disarmed only on deselect / kill.
                if(!armedArc_){hs::armArc(hs::ArcChannel::Teleport,true);armedArc_=true;bump(stats.arcArms);}
                const auto st=hs::arcStatus();
                if(st==hs::ArcStatus::Failed||st==hs::ArcStatus::Killed||(st==hs::ArcStatus::Building&&hs::arcStatusAgeMs()>3000)){offNote="NO ARC";noArc=true;}
            }
        }
        if(offNote){
            // a running blink always ends through IN (never stuck in black)
            if(st_==State::FadeOut||st_==State::WarpPending||st_==State::Hold){bump(stats.aborts[AbortGate]);hideArc();fadeInFromCurrent(now);enter(State::FadeIn,now);}
            if(st_==State::FadeIn){heartbeat(now);if(now>=fadeInNs_&&now-fadeInNs_>=msNs(cfg.inMs)){fadeNone(now);enter(State::Off,now);}}
            else if(st_!=State::Off){hideArc();fadeNone(now);enter(State::Off,now);}
            if(armedArc_&&deselected){hs::armArc(hs::ArcChannel::Teleport,false);armedArc_=false;}
            if(noArc)bump(stats.noArcTicks);
            moduleActive.store(false,std::memory_order_relaxed);lassoGate.store(false,std::memory_order_relaxed);
            minigameFocus.store(false,std::memory_order_relaxed);yieldStick.store(false,std::memory_order_relaxed);
            if(teleport&&*offNote&&!fallbackCued_){fallbackCued_=true;bump(stats.fallbackFree);pulse(.5f,40'000,now);secondPulseNs_=now+msNs(120);}
            if(!teleport)fallbackCued_=false;
            note_=offNote;noteNow.store(offNote,std::memory_order_relaxed);stateNow.store(unsigned(st_),std::memory_order_relaxed);
            bump(stats.stateTicks[unsigned(st_)]);
            if(killed(Feature::TpLoco))status::publish(Feature::TpLoco,status::Property);
            else if(!installed)status::publish(Feature::TpLoco,status::Unavailable);   // row "TELEPORT - UNAVAILABLE - FREE"
            else status::publish(Feature::TpLoco,status::Ready,*offNote?offNote:nullptr);
            return;
        }
        moduleActive.store(true,std::memory_order_relaxed);
        if(st_==State::Off){gate_.disarm();enter(State::Idle,now);fallbackCued_=false;}
        // ---- inputs ----
        StickSample s{};const bool haveS=stick.load(s);
        const bool fresh=haveS&&s.ns&&(now>=s.ns?now-s.ns<msNs(150):s.ns-now<msNs(150));
        if(!fresh)bump(stats.stickStale);
        lastX.store(s.x,std::memory_order_relaxed);lastY.store(s.y,std::memory_order_relaxed);
        auto& c=arc::frameContext(now);
        const auto g=gates(s,fresh,c,now);
        lassoGate.store(g.lasso,std::memory_order_relaxed);minigameFocus.store(g.minigame,std::memory_order_relaxed);
        if(g.minigame)bump(stats.yieldedMinigame);
        heartbeat(now);
        PlayerFacts p{};bool haveP=false;
        if(st_==State::Aiming||st_==State::ReleaseHold||(st_==State::Idle&&g.open&&gate_.armed&&StickGate::engage(s.x,s.y)))
            haveP=playerFacts(false,p)&&!p.reason;
        // ---- state machine ----
        switch(st_){
        case State::Off:case State::Count:break;
        case State::Idle:case State::Blocked:case State::Walk:{
            if(g.l3&&g.foot&&!g.lasso&&!g.menu){                         // escape walk (FREE path x .walk, mapMovement)
                if(st_!=State::Walk){bump(stats.walkSessions);hideArc();refuseUntilNs_=0;}
                enter(State::Walk,now);bump(stats.walkFrames);yieldStick.store(false,std::memory_order_relaxed);break;}
            if(st_==State::Walk){gate_.disarm();enter(State::Idle,now);}
            if(!g.open){
                if(st_!=State::Blocked||blockedNote_!=g.note){blockedSinceNs_=now;blockedNote_=g.note;blockedTransient_=g.transient;}
                if(st_!=State::Blocked){hideArc();refuseUntilNs_=0;}
                enter(State::Blocked,now);bump(stats.gates[g.gate<GateCount?g.gate:GateLoco]);if(!g.transient)gate_.disarm();
                const auto age=now-blockedSinceNs_;
                const bool yield=(!g.transient&&age>msNs(500))||(g.gate==GateSettling&&age>msNs(1500));
                yieldStick.store(yield,std::memory_order_relaxed);if(yield)bump(stats.blockedYieldFrames);
                break;}
            yieldStick.store(false,std::memory_order_relaxed);
            if(st_==State::Blocked){if(!blockedTransient_)gate_.disarm();enter(State::Idle,now);} // A transient gate keeps the stick armed
            if(refuseUntilNs_){if(now<refuseUntilNs_&&refuseShown_)publish(refuseFrame_,c);else{refuseUntilNs_=0;hideArc();}}
            if(gate_.sample(s.x,s.y,now))bump(stats.rearmWaits);
            if(gate_.armed&&StickGate::engage(s.x,s.y)){
                bump(stats.aimSessions);bump(stats.stickEngage);refuseUntilNs_=0;shrink_=false;
                tracer_.reset();filter_.reset();preview_.reset();history_.clear();backCount_=0;validStreak_=0;wasValid_=false;
                aimStartNs_=now;lastValidShownNs_=0;enter(State::Aiming,now);
                arc::ledger.demand(arc::ProbeUser::Teleport,8); // the next window grants the teleport tracer its share (arc newcomer rule)
                aimTick(fr,c,p,haveP);
            }
            break;}
        case State::Aiming:{
            if(g.l3&&g.foot){cancelAim(CancelWalk,now,State::Walk);bump(stats.walkSessions);break;}
            if(!g.open){
                // A transient gate closing for < 400 ms keeps the aim and the last arc
                if(g.transient&&!g.menu){if(!aimGateSinceNs_)aimGateSinceNs_=now;
                    if(now-aimGateSinceNs_<msNs(400)){if(const auto* e=history_.newest();e&&e->shown!=Shown::None){hs::ArcFrame f{};buildFrame(c,*e,f);publish(f,c);}break;}}
                aimGateSinceNs_=0;cancelAim(g.menu?CancelMenu:CancelGate,now,State::Idle);break;}
            aimGateSinceNs_=0;
            const auto a=StickGate::classify(s.x,s.y);
            if(a==StickGate::Aim::Neutral){
                if(now-aimStartNs_<msNs(120)&&backCount_==0){cancelAim(CancelFlick,now,State::Idle);break;}
                release(fr,c,s,backCount_==1);break;}
            if(a==StickGate::Aim::Back){
                if(++backCount_>=2){cancelAim(CancelBack,now,State::Idle);break;}
                // frozen on the last shown arc; nothing when the newest record is "hidden" (a lost hand) - never re-shows a stale arc
                if(const auto* e=history_.newest();e&&e->shown!=Shown::None){hs::ArcFrame f{};buildFrame(c,*e,f);publish(f,c);}else hideArc();
                break;}
            backCount_=0;
            aimTick(fr,c,p,haveP);
            break;}
        case State::ReleaseHold:{
            if(!g.open&&g.gate!=GateSettling){bump(stats.cancels[CancelGate]);hideArc();gate_.disarm();enter(State::Idle,now);break;}
            if(now-releaseHoldNs_>msNs(1000)){bump(stats.cancels[CancelAirborne]);arc::noteCommitExpired();hideArc();gate_.disarm();enter(State::Idle,now);break;}
            publish(frozen_,c);
            if(haveP&&!airborne(p))commit(fr,c);
            break;}
        case State::CommitPending:{
            if(!g.open){bump(stats.aborts[AbortGate]);arc::noteCommitCancelled();hideArc();gate_.disarm();enter(State::Idle,now);break;}
            publish(frozen_,c);
            commitTick(fr,c);
            break;}
        case State::FadeOut:{
            if(!g.open){abortTo(AbortGate,now,true);break;}
            const auto w=fade::xrWord.load(std::memory_order_acquire);
            if(fade::wordSerial(w)==std::uint32_t(serial_)&&(fade::wordFlags(w)&fade::FlagAborted)){++fadeFailStreak_;abortTo(AbortWatchdog,now,false);break;}
            if(!accepted_){commitTick(fr,c);if(st_!=State::FadeOut)break;}   // a rejection aborted through IN already
            if(fade::blackFor(w,std::uint32_t(serial_))){
                if(!blackNs_){blackNs_=now;bump(stats.fadeConfirms);stats.confirmWaitUsSum.fetch_add((now-fadeStartNs_)/1000,std::memory_order_relaxed);}
                fadeFailStreak_=0;
                if(accepted_){enter(State::WarpPending,now);doWarp(fr,c,false);}
                break;}
            if(now-fadeStartNs_>msNs(cfg.outMs+150)){++fadeFailStreak_;bump(stats.fadeFailures);abortTo(AbortConfirmTimeout,now,true);}
            break;}
        case State::WarpPending:{
            if(!g.open){abortTo(AbortGate,now,true);break;}
            doWarp(fr,c,false);break;}
        case State::Hold:{
            const auto w=fade::xrWord.load(std::memory_order_acquire);
            if(fade::wordSerial(w)==std::uint32_t(serial_)&&(fade::wordFlags(w)&fade::FlagAborted)){bump(stats.aborts[AbortWatchdog]);
                fadeNone(now);cooldownNs_=now;cooldownMs_=cfg.cooldownMs;enter(State::Cooldown,now);break;}
            if(g.menu||!c.world()){bump(stats.aborts[AbortGate]);fadeInFromCurrent(now);enter(State::FadeIn,now);break;}
            holdTick(fr,c);
            break;}
        case State::FadeIn:{
            if(now>=fadeInNs_&&now-fadeInNs_>=msNs(cfg.inMs)){
                // Both sums and their denominator (blinkWarps) cover the same BLINK warps (warpsExecuted also counts INSTANT ones)
                if(blackNs_&&fadeStartNs_&&warped_){const auto b=(fadeInNs_>blackNs_?fadeInNs_-blackNs_:0)/1000;stats.blackUsSum.fetch_add(b,std::memory_order_relaxed);
                    maxOf(stats.blackUsMax,b);lastBlackUs_=b;stats.totalUsSum.fetch_add((now-fadeStartNs_)/1000,std::memory_order_relaxed);bump(stats.blinkWarps);}
                if(warped_){failStreak_=0;hintUntilNs_=0;}
                fadeNone(now);cooldownNs_=now;cooldownMs_=cfg.cooldownMs;enter(State::Cooldown,now);}
            break;}
        case State::Cooldown:{
            // An INSTANT warp refused at the re-validation shows its refusal through the cooldown
            if(refuseUntilNs_){if(g.open&&now<refuseUntilNs_&&refuseShown_)publish(refuseFrame_,c);else{refuseUntilNs_=0;hideArc();}}
            if(now-cooldownNs_>=msNs(cooldownMs_)){gate_.armed=true;enter(State::Idle,now);} // No re-centre needed for the next teleport
            break;}
        }
        // hints: 3 s of aiming without a valid result
        if(st_==State::Aiming&&!lastValidShownNs_&&now-aimStartNs_>msNs(3000)&&!aimHinted_){aimHinted_=true;hint(now);}
        if(st_!=State::Aiming)aimHinted_=false;
        // frame monitor: aiming exceeds idle by > 2 ms over 2 s -> budget 200 us until the next aim session
        if(st_==State::Aiming&&now-aimStartNs_>msNs(2000)&&aimFrameMs_>0.&&idleFrameMs_>0.&&aimFrameMs_>idleFrameMs_+2.&&!shrink_){shrink_=true;bump(stats.budgetShrinks);}
        // the row note (panel glyphs)
        const char* n="READY";
        switch(st_){
        case State::Aiming:case State::ReleaseHold:case State::CommitPending:n="AIMING";break;
        case State::FadeOut:case State::WarpPending:case State::Hold:case State::FadeIn:n="BLINK";break;
        case State::Blocked:n=blockedNote_&&*blockedNote_?blockedNote_:"BLOCKED";break;
        case State::Walk:n="WALK";break;
        default:n=blink_&&!blinkAvailable()?"INSTANT ONLY":"READY";break;
        }
        if(hintUntilNs_&&now<hintUntilNs_&&(st_==State::Idle||st_==State::Aiming))n="HOLD L3 TO WALK"; // a gate's own note wins
        note_=n;noteNow.store(n,std::memory_order_relaxed);stateNow.store(unsigned(st_),std::memory_order_relaxed);bump(stats.stateTicks[unsigned(st_)]);
        status::publish(Feature::TpLoco,status::Ready,n);
        const auto dt=nowNs()-t0;
        if(st_==State::Aiming||st_==State::CommitPending){bump(stats.aimUsFrames);stats.aimUsSum.fetch_add(dt/1000,std::memory_order_relaxed);maxOf(stats.aimUsMax,dt/1000);}
        if(dt>msNs(2)&&(st_==State::Aiming||st_==State::CommitPending)){bump(stats.guardTrips);guardUntilFrame_=fr.index+8;}
    }catch(...){bump(stats.exceptions);}
}

inline void Module::aimTick(const mainthread::Frame& fr,arc::Context& c,const PlayerFacts& p,bool haveP) noexcept {
    const auto now=fr.now;bump(stats.aimFrames);
    V3 o{},dir{};
    if(!c.world()||!c.inputValid()||!c.aimRay(cfg.hand,o,dir)){                // untracked hand / no world: nothing shown, the aim survives
        hideArc();validStreak_=0;aimLost_=true;
        // The History records the gap ("hidden since now"): a release whose lookback falls into it commits nothing
        if(const auto* n=history_.newest();n&&n->shown!=Shown::None){Entry h{};h.ns=now;h.shown=Shown::None;history_.push(h);}
        return;}
    aimLost_=false;
    V3 start{};if(c.startPoint(cfg.hand,start)==arc::StartSource::None)start=o;
    if(arc::filterEnabled.load(std::memory_order_relaxed))dir=filter_.filter(dir,now);
    const float upm=c.upm();upmNow.store(upm,std::memory_order_relaxed);
    arc::Launch L{};L.origin=am::add(start,am::mul(dir,.05f*upm));L.velocity=am::mul(dir,v0()*upm);L.gravityMps2=9.81f;
    L.maxTimeS=2.f;L.maxRangeM=rangeNow.load(std::memory_order_relaxed)+1.f;
    // maxDown + 1 m below the FEET (maxDown + 1 m below the hand could not reach the TOO_LOW floor:
    // the hand is ~1.2 m up)
    {const float handUp=haveP?std::max(0.f,am::dot(am::sub(L.origin,p.C),c.up())/std::max(upm,.01f)):1.2f;L.maxDropM=cfg.maxDownM+1.f+handUp;}
    const auto tc=traceConfig();
    const arc::Trace t=tracer_.run(c,L,tc);
    // a steady aim reuses every chord (demand 0 -> no grant next window); the chords cast in one window all expire together after
    // reuseMaxTicks -> declare the ones expiring next window as demand now, so the re-cast is granted instead of a Pending blip.
    {unsigned expiring=0;const auto w=t.window;const unsigned last=t.hit?t.landingChord:(t.chordCount?t.chordCount-1:0);
     for(unsigned k=0;k<=last&&k<t.chordCount;++k)if(w>=t.chordTick[k]&&w-t.chordTick[k]+1>=tc.reuseMaxTicks)++expiring;
     if(t.losUsed&&w>=t.losTick&&w-t.losTick+1>=tc.reuseMaxTicks)++expiring;
     if(expiring)arc::ledger.demand(arc::ProbeUser::Teleport,expiring+t.casts+t.unknown);}
    bump(stats.aimCasts,t.casts);bump(stats.aimChordsStale,t.unknown+t.failed);if(t.hit)bump(stats.aimHits);
    Entry e{};e.trace=t;e.ns=now;e.launchOrigin=L.origin;e.launchVelocity=L.velocity;
    if(t.pending){e.shown=Shown::Pending;bump(stats.aimPendingFrames);}
    else if(!haveP){e.shown=Shown::Validating;bump(stats.aimValidatingFrames);}   // no capsule base this tick: nothing judged
    else{
        Geometry g{};g.up=c.up();g.upm=upm;g.C=p.C;g.arcDir=L.velocity;
        const std::uint64_t budgetUs=(guardUntilFrame_>fr.index||shrink_)?std::min<std::uint32_t>(cfg.budgetUs,200):cfg.budgetUs;
        const Validation& v=preview_.run(limits(),g,t,env.probes,now,fr.index,budgetUs*1000ull);
        e.v=v;
        if(preview_.stale())e.shown=Shown::Validating;
        else if(v.reason!=Reason::Ok)e.shown=Shown::Invalid;
        else e.shown=v.full?Shown::Valid:Shown::Validating;
        if(e.shown==Shown::Validating)bump(stats.aimValidatingFrames);
        if(!preview_.stale())bump(stats.reasons[unsigned(v.reason)]);
        LastAim la{};la.reason=v.reason;la.cls=v.cls;la.H=v.H;la.n=v.n;la.G=v.G;la.nG=v.nG;la.dist=v.distM;la.dy=v.dyM;la.waterDepth=v.waterDepthM;
        la.capsuleHits=v.capsuleHits;la.lowHits=v.lowHits;la.edgeFound=v.edgeFound;lastAim.store(la);
    }
    // a light tick when the target becomes valid (2 consecutive valid results, >= 250 ms between ticks)
    if(e.shown==Shown::Valid){lastValidShownNs_=now;
        if(++validStreak_>=2&&!wasValid_){wasValid_=true;if(!lastValidHapticNs_||now-lastValidHapticNs_>=msNs(250)){lastValidHapticNs_=now;pulse(.15f,15'000,now);bump(stats.validHaptics);}}}
    else if(e.shown==Shown::Invalid){validStreak_=0;wasValid_=false;}
    hs::ArcFrame f{};buildFrame(c,e,f);publish(f,c);
    history_.push(e);
}

inline void Module::release(const mainthread::Frame& fr,arc::Context& c,const StickSample& s,bool overshoot) noexcept {
    const auto now=fr.now;
    if(overshoot)bump(stats.releasesOvershoot);
    const std::uint64_t at=s.ns?s.ns:now,back=msNs(cfg.lookbackMs);
    const Entry* e=history_.pick(at>back?at-back:0);
    // Only an arc that is on screen now AND was shown ~lookback before the release is committed.
    // The aim hand untracked at the release tick, the arc hidden by the last aim tick (lost hand), or
    // the lookback falling into such a gap (History "hidden" record) -> commit.cancelled,
    // cancels.untracked, no warp: an arc shown before the hand was lost is never warped to. An entry
    // far older than the lookback (only a hitch causes that) -> cancels.stale.
    {V3 ro{},rd{};
     if(!c.aimRay(cfg.hand,ro,rd)||aimLost_||(e&&e->shown==Shown::None)){aimLost_=false;cancelAim(CancelUntracked,now,State::Idle);return;}}
    if(!e){cancelAim(CancelFlick,now,State::Idle);return;}
    if(at>e->ns&&at-e->ns>back+msNs(250)){cancelAim(CancelStale,now,State::Idle);return;}
    picked_=*e;
    if(cfg.probeOnly){                                                   // .probe 1: arc + reasons, never a warp
        bump(stats.releasesProbeOnly);buildFrame(c,picked_,refuseFrame_);refuseUntilNs_=now+msNs(300);refuseShown_=refuseFrame_.count>0;
        if(picked_.shown==Shown::Invalid)bump(stats.releasesInvalid[unsigned(picked_.v.reason)]);
        gate_.disarm();enter(State::Idle,now);return;}
    // stable invalid = the picked entry and the one shown right before it are both invalid -> refused at once (red Cross 300 ms)
    if(picked_.shown==Shown::Invalid){
        const Entry* before=nullptr;for(unsigned k=0;k+1<history_.count;++k)if(history_.newest(k)->ns==picked_.ns){before=history_.newest(k+1);break;}
        if(!before||before->shown==Shown::Invalid){bump(stats.releasesInvalid[unsigned(picked_.v.reason)]);finishRefused(c,now);return;}
    }
    PlayerFacts p{};const bool haveP=playerFacts(false,p)&&!p.reason;
    if(haveP&&airborne(p)){                                              // released in a jump: committed on landing (<= 1 s)
        buildFrame(c,picked_,frozen_);publish(frozen_,c);releaseHoldNs_=now;enter(State::ReleaseHold,now);return;}
    commit(fr,c);
}

inline void Module::commit(const mainthread::Frame& fr,arc::Context& c) noexcept {
    const auto now=fr.now;
    buildFrame(c,picked_,frozen_);publish(frozen_,c);                   // markShown(the picked trace): validateShown judges what was SHOWN
    ++serial_;bump(stats.commitRequests);
    accepted_=false;committed_={};commitV_={};commitStartNs_=now;commitTicks_=0;blackNs_=0;fadeStartNs_=0;warped_=false;
    userTurns0_=warp::userTurns.load(std::memory_order_relaxed);         // user snap turns from here on are not psi drift (holdTick)
    fromValid_=picked_.shown==Shown::Valid;
    if(fromValid_)bump(stats.releasesValid);
    if(fromValid_&&blink_&&blinkAvailable()){                            // A Valid entry fades at once, validateShown meanwhile
        fadeStartNs_=now;fadeCommand(fade::Out,now,now);bump(stats.fadeStarts);enter(State::FadeOut,now);
        commitTick(fr,c);return;}
    if(blink_&&!blinkAvailable())bump(stats.instantOnly);
    enter(State::CommitPending,now);commitTick(fr,c);
}

inline bool Module::commitTick(const mainthread::Frame& fr,arc::Context& c) noexcept {
    const auto now=fr.now;const auto t0=nowNs();++commitTicks_;
    const auto r=tracer_.validateShown(c,traceConfig());
    bump(stats.commitCasts,r.casts);
    {const auto us=(nowNs()-t0)/1000;stats.commitUsSum.fetch_add(us,std::memory_order_relaxed);maxOf(stats.commitUsMax,us);}
    bool rejected=r.state==arc::CommitState::Rejected;unsigned why=RejectChanged;
    if(rejected)switch(r.reason){case arc::CommitReject::Failed:why=RejectFailed;break;case arc::CommitReject::Timeout:why=RejectTimeout;break;
        case arc::CommitReject::NoContext:why=RejectNoContext;break;default:why=RejectChanged;break;}
    if(r.state==arc::CommitState::Pending){
        if(commitTicks_>=3&&now-commitStartNs_>msNs(150)){rejected=true;why=RejectTimeout;}
        else return true;
    }
    // The shown landing was valid, so use IT; validateAll below
    // still checks ground + capsule at that point before the warp.
    arc::Hit landing=r.landing;
    if(rejected&&(why==RejectChanged||why==RejectTimeout)&&picked_.shown==Shown::Valid&&picked_.trace.hit){
        bump(stats.commitRejected[why]);if(why==RejectChanged)bump(stats.releasesCommitChanged);
        rejected=false;landing=picked_.trace.landing;}
    // Take whichever landing exists:
    // the shown one, else the re-traced one; validateAll below still checks ground + capsule before the warp.
    else if(rejected&&why==RejectChanged&&(picked_.trace.hit||r.landing.distance>0.f||r.landing.normalValid)){
        bump(stats.commitRejected[why]);bump(stats.releasesCommitChanged);
        rejected=false;landing=picked_.trace.hit?picked_.trace.landing:r.landing;}
    if(rejected){
        bump(stats.commitRejected[why]);if(why==RejectChanged)bump(stats.releasesCommitChanged);
        if(st_==State::FadeOut){pulse(.25f,60'000,now);bump(stats.refusals);abortTo(AbortCommit,now,true);}
        else finishRefused(c,now);
        return false;
    }
    accepted_=true;committed_=landing;bump(stats.commitAccepted);
    if(st_==State::CommitPending){
        PlayerFacts p{};
        // A player-gate abort is not a refused target - no red Cross, no refusal buzz, failStreak unchanged
        if(!playerFacts(false,p)||p.reason){bump(stats.aborts[AbortPlayer]);hideArc();gate_.disarm();enter(State::Idle,now);return false;}
        Geometry g{};g.up=c.up();g.upm=c.upm();g.C=p.C;g.arcDir=picked_.launchVelocity;
        commitV_=validateAll(limits(),g,committed_.point,committed_.normal,committed_.normalValid,env.probes);
        if(!commitV_.ok()){bump(stats.commitRejected[RejectValidation]);bump(stats.releasesInvalid[unsigned(commitV_.reason)]);
            picked_.v=commitV_;picked_.shown=Shown::Invalid;finishRefused(c,now);return false;}
        if(blink_&&blinkAvailable()){fadeStartNs_=now;fadeCommand(fade::Out,now,now);bump(stats.fadeStarts);enter(State::FadeOut,now);return false;}
        enter(State::WarpPending,now);doWarp(fr,c,true);
    }
    return false;
}

inline void Module::doWarp(const mainthread::Frame& fr,arc::Context& c,bool instant) noexcept {
    const auto now=fr.now;bump(stats.warpsRequested);
    if(!c.world()){abortTo(AbortGate,now,!instant);return;}
    PlayerFacts p{};
    if(!playerFacts(true,p)||p.reason){abortTo(AbortPlayer,now,!instant);return;}         // Gate 6: strict player gate
    const V3 up=c.up();const float upm=std::max(c.upm(),.01f);
    Geometry g{};g.up=up;g.upm=upm;g.C=p.C;g.arcDir=picked_.launchVelocity;
    const auto L=limits();
    // a stale producer result is never trusted at the warp: ground / water / capsule / low / edge again at the committed landing
    const Validation v=validateAll(L,g,committed_.point,committed_.normal,committed_.normalValid,env.probes);
    const Validation& ref=commitV_.ground?commitV_:picked_.v;
    if(!v.ok()||(ref.ground&&am::distance(v.G,ref.G)>.5f*upm)){
        bump(stats.releasesInvalid[unsigned(v.ok()?Reason::Blocked:v.reason)]);
        // INSTANT shows the red refusal AFTER the abort (abortTo hides the arc; COOLDOWN keeps re-publishing the refusal frame
        // for its 300 ms) and counts the target once (startRefusal), not twice toward the HOLD L3 TO WALK hint
        if(instant){abortTo(AbortCapsule,now,false,false);picked_.v=v;startRefusal(c,picked_,now);}
        else{pulse(.25f,60'000,now);bump(stats.refusals);abortTo(AbortCapsule,now,true);}   // BLINK refusal buzz
        return;}
    // the capsule base's rest offset above the fixed ground (answers whether C sits on the float height)
    float rest=.03f*upm;
    {V3 pt{},nn{};Validation scratch{};
     const auto r=vdetail::ground(env.probes,am::add(p.C,am::mul(up,.30f*upm)),am::sub(p.C,am::mul(up,.60f*upm)),pt,nn,scratch);
     if(r==ProbeOut::Hit&&am::dot(am::normalized(nn),up)>=.8f)rest=std::clamp(am::dot(am::sub(p.C,pt),up),0.f,.30f*upm);
     else bump(stats.restOffsetFallback);}
    const V3 target=am::add(v.G,am::mul(up,rest+.01f*upm));
    if(am::distance(target,p.C)>=L.guardUnits){bump(stats.releasesInvalid[unsigned(Reason::TooFar)]);                         // 24-unit warp guard
        if(instant){abortTo(AbortCapsule,now,false,false);picked_.v=v;picked_.v.reason=Reason::TooFar;startRefusal(c,picked_,now);}
        else{pulse(.25f,60'000,now);bump(stats.refusals);abortTo(AbortCapsule,now,true);}   // BLINK refusal buzz
        return;}
    int streamBefore=-1;if(cfg.streamFlag&&env.streamFlag){try{streamBefore=env.streamFlag(env.ctx,-1);}catch(...){streamBefore=-1;}}
    indoorsBefore_=-1;if(env.indoors){try{indoorsBefore_=env.indoors(env.ctx,p.C);}catch(...){indoorsBefore_=-1;}if(indoorsBefore_<0)bump(stats.indoorsNoWorld);}
    crouchBefore_=-1;if(env.crouching){try{crouchBefore_=env.crouching(env.ctx);}catch(...){crouchBefore_=-1;}}
    hideArc();                                                           // clearArc BEFORE the warp (arc contract)
    bool ok=false;try{ok=env.warp(env.ctx,p.guid,target,p.headingDeg);}catch(...){ok=false;}
    if(!ok){abortTo(AbortPlayer,now,!instant);return;}
    if(cfg.streamFlag&&streamBefore>=0&&env.streamFlag){try{env.streamFlag(env.ctx,streamBefore);bump(stats.streamFlagRestored);}catch(...){}}
    if(env.playerTeleporting){try{if(env.playerTeleporting(env.ctx,false)==1){env.playerTeleporting(env.ctx,true);bump(stats.flagOwned);}}catch(...){}}
    const float from[3]{p.C[0],p.C[1],p.C[2]},delta[3]{target[0]-p.C[0],target[1]-p.C[1],target[2]-p.C[2]};
    warp::publishNote(serial_,now,from,delta,cfg.bsnap);
    pulse(.35f,25'000,now);
    bump(stats.warpsExecuted);warped_=true;warpNs_=now;warpFrame_=fr.index;holdStartNs_=now;target_=target;before_=p.C;restOffset_=rest;
    lastRestOffset.store(rest/upm,std::memory_order_relaxed);
    {LastWarp lw{};for(unsigned i=0;i<3;++i){lw.from[i]=from[i];lw.target[i]=target[i];}lw.dist=am::distance(target,p.C)/upm;lw.restOffsetM=rest/upm;
     lw.serial=serial_;lw.ns=now;lw.frameMsBefore=float(idleFrameMs_);lastWarp.store(lw);}
    post_=true;postSerial_=serial_;postDyMax_=0;fell_=false;recovered_=false;recoveredNs_=0;settleTicks_=0;settled_=false;postOk_=false;pairOk_=false;pairFallback_=false;
    psiHist_={};psiCount_=0;psiFrames_=0;psiAttempts_=0;psiDeltaBefore_=0;psiWaitFrame_=0;psiDone_=false;psiUserSkipped_=false;warpUp_=up;warpUpm_=upm;
    if(instant){failStreak_=0;hintUntilNs_=0;fadeNone(now);cooldownNs_=now;cooldownMs_=cfg.instantCooldownMs;enter(State::Cooldown,now);}
    else enter(State::Hold,now);
}

inline void Module::postTick(std::uint64_t now) noexcept {
    if(!post_)return;
    PlayerFacts p{};const bool have=playerFacts(false,p)&&!p.reason;
    const float u=std::max(warpUpm_,.01f);
    float dy=0.f,herr=0.f;
    if(have){
        const V3 d=am::sub(p.C,target_);dy=am::dot(d,warpUp_)/u;herr=am::length(vdetail::horizontal(d,warpUp_))/u;
        postDyMax_=std::max(postDyMax_,std::fabs(dy));if(dy<-1.f)fell_=true;
        postOk_=herr<=.30f&&std::fabs(dy)<=.40f;                         // the post-check (HOLD waits for it; the result is taken at 1 s)
        lastDy.store(dy,std::memory_order_relaxed);
        if(!recovered_&&(!p.moverRead?now-warpNs_>=msNs(150):!p.recovering)){recovered_=true;recoveredNs_=now;bump(stats.recoveryN);
            stats.recoveryMsSum.fetch_add((now-warpNs_)/1'000'000ull,std::memory_order_relaxed);}
        if(recovered_&&!settled_){
            if(!p.velocityKnown||std::fabs(p.vy)<.2f){if(++settleTicks_>=2){settled_=true;bump(stats.settleN);
                stats.settleMsSum.fetch_add((now-warpNs_)/1'000'000ull,std::memory_order_relaxed);maxOf(stats.settleDyUmMax,std::uint64_t(postDyMax_*1e6f));
                settleDyMaxM.store(postDyMax_,std::memory_order_relaxed);}}
            else settleTicks_=0;
        }
    }
    if(!recovered_&&now-warpNs_>=msNs(1000)){recovered_=true;settled_=true;}   // recovery cap 1 s
    if(now-warpNs_<msNs(1000))return;
    // post-check result (1 s after the warp)
    const bool ok=have&&herr<=.30f&&std::fabs(dy)<=.40f&&!fell_;
    bump(ok?stats.warpsOk:stats.warpsMissed);if(fell_)bump(stats.warpsFell);
    if(env.crouching&&crouchBefore_>=0){int after=-1;try{after=env.crouching(env.ctx);}catch(...){}if(crouchBefore_==1&&after==0)bump(stats.crouchLost);}
    if(env.indoors&&indoorsBefore_>=0&&have){int after=-1;try{after=env.indoors(env.ctx,p.C);}catch(...){}
        if(after<0)bump(stats.indoorsNoWorld);else if(after!=indoorsBefore_)bump(stats.indoorChanges);}
    LastWarp lw{};if(lastWarp.load(lw)&&lw.serial==postSerial_){
        for(unsigned i=0;i<3;++i)lw.after[i]=p.C[i];lw.errorM=herr;lw.dyM=dy;lw.ok=ok;lw.done=true;lw.ageS=float(double(now-warpNs_)/1e9);
        lw.frameMsAfter=float(idleFrameMs_);lastWarp.store(lw);}
    post_=false;
}

inline void Module::holdTick(const mainthread::Frame& fr,arc::Context& c) noexcept {
    (void)c;const auto now=fr.now;
    warp::CameraResult cr{};const bool haveCr=warp::camera.load(cr)&&cr.serial==serial_&&cr.snapped;
    // pair identity: the XR thread submitted a pair drawn from a head sample at/after the warp key K (fallback: frames + 50 ms)
    if(!pairOk_){
        if(haveCr&&cr.key){const auto first=fade::firstPostWarpScene(cr.key);if(first&&fade::xrPairId.load(std::memory_order_acquire)>=first)pairOk_=true;}
        const auto lastNote=fade::lastNoteNs.load(std::memory_order_relaxed);
        const bool noNotes=!lastNote||(now>lastNote&&now-lastNote>msNs(200));
        const bool noKey=!haveCr&&now-warpNs_>msNs(150);
        if(!pairOk_&&(noNotes||noKey)&&fr.index>=warpFrame_+cfg.holdFrames&&now-warpNs_>=msNs(50)){pairOk_=true;pairFallback_=true;bump(stats.pairFallbacks);}
    }
    // psi: stable (< 0.5 deg) for 2 frames; no camera result (camera killed / no snap) -> not measurable after 150 ms
    constexpr float deg=3.14159265f/180.f;
    const auto wrap=[](float a){while(a>3.14159265f)a-=6.28318531f;while(a<-3.14159265f)a+=6.28318531f;return a;};
    if(haveCr&&cr.psiValid&&cr.frames!=psiFrames_){psiFrames_=cr.frames;psiHist_[2]=psiHist_[1];psiHist_[1]=psiHist_[0];psiHist_[0]=cr.psiNow;psiCount_=std::min(psiCount_+1,3u);}
    bool psiStable=!cfg.psi||(!haveCr&&now-warpNs_>msNs(150))||(haveCr&&!cr.psiValid);
    if(!psiStable&&psiCount_>=3)psiStable=std::fabs(wrap(psiHist_[0]-psiHist_[1]))<.5f*deg&&std::fabs(wrap(psiHist_[1]-psiHist_[2]))<.5f*deg;
    const float dPsi=haveCr&&cr.psiValid?wrap(cr.psiNow-cr.psiBefore):0.f;
    // dPsi = psi now - psi before the warp also holds every snap turn made meanwhile (right stick in the dark, a likely
    // action right after releasing the left stick). That is not drift: no correction (it would undo the turn), no sign
    // learning, not in psiDeltaDegMax. Counted once per warp (warps.psiTurnUserSkipped).
    const bool userTurned=warp::userTurns.load(std::memory_order_relaxed)!=userTurns0_;
    if(userTurned&&!psiUserSkipped_){psiUserSkipped_=true;bump(stats.psiTurnUserSkipped);}
    if(!userTurned)maxOf(stats.psiDeltaUdegMax,std::uint64_t(std::fabs(dPsi)/deg*1e6f));
    {LastWarp lw{};if(lastWarp.load(lw)&&lw.serial==serial_){lw.psiDeltaDeg=dPsi/deg;lastWarp.store(lw);}}
    if(psiWaitFrame_){
        if(fr.index<=psiWaitFrame_)return;
        psiWaitFrame_=0;if(!userTurned&&std::fabs(dPsi)>std::fabs(psiDeltaBefore_)+.2f*deg){psiSign_=-psiSign_;bump(stats.psiSignFlips);}
    }
    const bool capped=now-warpNs_>=msNs(cfg.holdCapMs);
    const bool ready=settled_&&postOk_&&pairOk_&&psiStable;
    if(!capped&&!ready)return;
    if(capped&&!ready&&!psiDone_)bump(stats.holdCapHits);
    if(cfg.psi&&!userTurned&&haveCr&&cr.psiValid&&std::fabs(dPsi)>1.f*deg&&psiAttempts_<(capped?1u:2u)&&env.viewTurn&&!psiDone_){
        std::uint64_t ns{};int r=-1;try{r=env.viewTurn(-dPsi*float(psiSign_),ns);}catch(...){r=-1;}
        if(r==1){++psiAttempts_;bump(stats.psiCorrected);psiDeltaBefore_=dPsi;psiWaitFrame_=fr.index+1;psiCount_=0;if(capped)psiDone_=true;return;}
        if(r<0){bump(stats.psiTurnDeclined);psiDone_=true;}
        else if(!capped)return;                                          // not eligible this frame: retry (the cap bounds it)
    }
    fadeCommand(fade::In,now,now);fadeInNs_=now;bump(stats.fadeIns);enter(State::FadeIn,now);
}

// ============================== registration / install ==============================
inline void moduleTick(const mainthread::Frame& f) noexcept {module.tick(f);}
inline std::atomic<const char*> installState{"NOT ATTEMPTED"};
// Fail closed: nothing of the engine is used. Unless killed, a status-only tick is still registered (when the executor runs) so that
// selecting TELEPORT gives the FREE path with the row note and the one-time double-pulse cue (fallback.free) - module.installed stays false.
inline void unavailable(const char* why) noexcept {
    installState.store(why);
    status::publish(Feature::TpLoco,killed(Feature::TpLoco)?status::Property:status::Unavailable);
    if(!killed(Feature::TpLoco)&&mainthread::executorInstalled())mainthread::registerModule(mainthread::ModuleTeleport,"teleport",&moduleTick,nullptr);
}
// Startup (TraceEntry, after LOCO and the arc probe binding): register ModuleTeleport (9, the last tick) with a ready Env.
inline bool installWith(const Env& e) noexcept {
    if(killed(Feature::TpLoco)){installState.store("KILLED");status::publish(Feature::TpLoco,status::Property);return false;}
    if(!e){unavailable("NO PROBE");return false;}
    module.env=e;module.cfg=readConfig();module.configured=true;walkScale.store(module.cfg.walk,std::memory_order_relaxed);
    if(!mainthread::registerModule(mainthread::ModuleTeleport,"teleport",&moduleTick,nullptr)){
        module.env=Env{};installState.store("DUPLICATE MODULE");status::publish(Feature::TpLoco,status::Unavailable);return false;}
    module.installed=true;installState.store("INSTALLED");status::publish(Feature::TpLoco,status::Ready);return true;
}
inline void resetForTests() noexcept {
    module.resetForTests();module.installed=false;module.env=Env{};module.configured=false;module.cfg=Config{};
    mainthread::registerModule(mainthread::ModuleTeleport,"teleport",nullptr,nullptr);installState.store("NOT ATTEMPTED");
    moduleActive.store(false);lassoGate.store(false);minigameFocus.store(false);yieldStick.store(false);ownsStick.store(false);walkModifier.store(false);
    externalBusy.store(0);fade::resetForTests();
    stats.~Stats();::new(&stats) Stats{};validatorStats.~ValidatorStats();::new(&validatorStats) ValidatorStats{};
    module.lastWarp.store(LastWarp{});module.lastAim.store(LastAim{});
}

// ============================== report (vr.tploco) ==============================
inline std::string report() {
    std::string o;o.reserve(6144);char b[1400];
    const auto& m=module;const auto& c=m.cfg;const auto& s=stats;
    const auto vs=settings();const unsigned eff=vs[Key::Locomotion];
    const auto st=std::min<unsigned>(m.stateNow.load(),stateCount-1);
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"killed\":%s,\"mode\":\"%s\",\"state\":\"%s\",\"note\":\"%s\",\"install\":\"%s\",\"ticks\":%llu,\"exceptions\":%llu,"
        "\"api\":{\"env\":%s,\"player\":%s,\"warp\":%s,\"ground\":%s,\"capsule\":%s,\"water\":%s,\"indoors\":%s,\"lasso\":%s,\"minigame\":%s,\"viewTurn\":%s,"
        "\"castBetween\":%s,\"arcCapsule\":%s,\"executor\":%s},",
        m.installed?"true":"false",killed(Feature::TpLoco)?"true":"false",eff<3?keyInfo[0].names[eff]:"?",stateNames[st],m.noteNow.load(),installState.load(),
        (unsigned long long)u64(s.ticks),(unsigned long long)u64(s.exceptions),
        bool(m.env)?"true":"false",m.env.player?"true":"false",m.env.warp?"true":"false",m.env.probes.ground?"true":"false",
        m.env.probes.capsule?"true":"false",m.env.probes.water?"true":"false",m.env.indoors?"true":"false",m.env.lasso?"true":"false",
        m.env.minigame?"true":"false",m.env.viewTurn?"true":"false",
        arc::engineApi.load()&&*arc::engineApi.load()?"true":"false",arc::engineApi.load()&&arc::engineApi.load()->capsule?"true":"false",
        mainthread::executorInstalled()?"true":"false");o+=b;
    std::snprintf(b,sizeof b,"\"config\":{\"range\":%.0f,\"v0\":%.2f,\"upm\":%.3f,\"blink\":%s,\"instantOnly\":%s,\"outMs\":%u,\"inMs\":%u,\"cooldownMs\":%u,"
        "\"holdCapMs\":%u,\"holdFrames\":%u,\"maxUp\":%.2f,\"maxDown\":%.2f,\"slopeDeg\":%.0f,\"waterCm\":%.0f,\"minCm\":%.0f,"
        "\"capsule\":{\"r\":%.2f,\"knee\":%.2f,\"top\":%.2f,\"mask\":\"%x\"},\"low\":{\"r\":%.2f,\"mask\":\"%x\",\"state\":%u},\"budgetUs\":%u,\"casts\":%u,"
        "\"walk\":%.2f,\"lookbackMs\":%u,\"arcSurface\":%d,\"streamFlag\":%d,\"psi\":%d,\"edge\":%d,\"bsnap\":%d,\"hand\":%u,\"probeOnly\":%d,\"facing\":%d},",
        double(m.rangeNow.load()),double(m.v0Now.load()),double(m.upmNow.load()),m.blinkNow.load()?"true":"false",m.instantOnlyNow.load()?"true":"false",
        c.outMs,c.inMs,c.cooldownMs,c.holdCapMs,c.holdFrames,double(c.maxUpM),double(c.maxDownM),double(c.slopeDeg),double(c.waterM*100.f),double(c.minM*100.f),
        double(c.capsuleR),double(c.kneeM),double(c.topM),c.capsuleMask,double(c.lowR),c.lowMask,unsigned(c.lowState),c.budgetUs,c.casts,double(c.walk),
        c.lookbackMs,int(c.arcSurface),int(c.streamFlag),int(c.psi),int(c.edge),int(c.bsnap),c.hand,int(c.probeOnly),int(c.facing));o+=b;
    std::snprintf(b,sizeof b,"\"stick\":{\"samples\":%llu,\"owned\":%llu,\"walk\":%llu,\"free\":%llu,\"native\":%llu,\"stale\":%llu,\"engage\":%llu,"
        "\"rearmWaits\":%llu,\"yieldedMinigame\":%llu,\"ownsNow\":%s,\"lastX\":%.2f,\"lastY\":%.2f},\"gates\":{",
        (unsigned long long)linkGet(stickSamples),(unsigned long long)linkGet(ownedSamples),(unsigned long long)linkGet(walkSamples),
        (unsigned long long)linkGet(freeSamples),(unsigned long long)linkGet(nativeSamples),(unsigned long long)u64(s.stickStale),
        (unsigned long long)u64(s.stickEngage),(unsigned long long)u64(s.rearmWaits),(unsigned long long)u64(s.yieldedMinigame),
        ownsStick.load()?"true":"false",double(m.lastX.load()),double(m.lastY.load()));o+=b;
    for(unsigned i=0;i<GateCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",gateNames[i],(unsigned long long)u64(s.gates[i]));o+=b;}
    std::snprintf(b,sizeof b,"},\"escape\":{\"walkFrames\":%llu,\"walkSessions\":%llu,\"blockedYieldFrames\":%llu},\"hints\":{\"walk\":%llu},"
        "\"fallback\":{\"free\":%llu},\"states\":{",
        (unsigned long long)u64(s.walkFrames),(unsigned long long)u64(s.walkSessions),(unsigned long long)u64(s.blockedYieldFrames),
        (unsigned long long)u64(s.hintsWalk),(unsigned long long)u64(s.fallbackFree));o+=b;
    for(unsigned i=0;i<stateCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",stateNames[i],(unsigned long long)u64(s.stateTicks[i]));o+=b;}
    const auto& vst=validatorStats;const auto gc=u64(vst.groundCalls),cc=u64(vst.capsuleCalls),af=u64(s.aimUsFrames),fa=u64(s.frameAimN),fi=u64(s.frameIdleN);
    std::snprintf(b,sizeof b,"},\"aim\":{\"sessions\":%llu,\"frames\":%llu,\"casts\":%llu,\"chordsStale\":%llu,\"hits\":%llu,\"pendingFrames\":%llu,"
        "\"validatingFrames\":%llu,\"validTicks\":%llu,\"usAvg\":%.1f,\"usMax\":%llu,\"groundUsAvg\":%.1f,\"capsuleUsAvg\":%.1f,\"capsuleUsMax\":%.1f,"
        "\"probeFailures\":%llu,\"budgetHits\":%llu,\"fullRuns\":%llu,\"guardTrips\":%llu,\"budgetShrinks\":%llu,\"frameMsAiming\":%.2f,\"frameMsIdle\":%.2f,\"reasons\":{",
        (unsigned long long)u64(s.aimSessions),(unsigned long long)u64(s.aimFrames),(unsigned long long)u64(s.aimCasts),(unsigned long long)u64(s.aimChordsStale),
        (unsigned long long)u64(s.aimHits),(unsigned long long)u64(s.aimPendingFrames),(unsigned long long)u64(s.aimValidatingFrames),
        (unsigned long long)u64(s.validHaptics),af?double(u64(s.aimUsSum))/double(af):0.0,(unsigned long long)u64(s.aimUsMax),
        gc?double(u64(vst.groundNsSum))/double(gc)/1e3:0.0,cc?double(u64(vst.capsuleNsSum))/double(cc)/1e3:0.0,double(u64(vst.capsuleNsMax))/1e3,
        (unsigned long long)u64(vst.failures),(unsigned long long)u64(vst.budgetHits),(unsigned long long)u64(vst.fullRuns),
        (unsigned long long)u64(s.guardTrips),(unsigned long long)u64(s.budgetShrinks),fa?double(u64(s.frameUsAimSum))/double(fa)/1e3:0.0,
        fi?double(u64(s.frameUsIdleSum))/double(fi)/1e3:0.0);o+=b;
    for(unsigned i=0;i<reasonCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",reasonNames[i],(unsigned long long)u64(s.reasons[i]));o+=b;}
    {LastAim la{};m.lastAim.load(la);
     std::snprintf(b,sizeof b,"},\"last\":{\"reason\":\"%s\",\"cls\":\"%s\",\"H\":[%.2f,%.2f,%.2f],\"n\":[%.2f,%.2f,%.2f],\"G\":[%.2f,%.2f,%.2f],"
        "\"nG\":[%.2f,%.2f,%.2f],\"dist\":%.2f,\"dy\":%.2f,\"waterDepth\":%.2f,\"capsuleHits\":%d,\"lowHits\":%d,\"edgeFound\":%d}},",
        reasonNames[std::min<unsigned>(unsigned(la.reason),reasonCount-1)],clsNames[std::min<unsigned>(unsigned(la.cls),2)],
        double(la.H[0]),double(la.H[1]),double(la.H[2]),double(la.n[0]),double(la.n[1]),double(la.n[2]),double(la.G[0]),double(la.G[1]),double(la.G[2]),
        double(la.nG[0]),double(la.nG[1]),double(la.nG[2]),double(la.dist),double(la.dy),double(la.waterDepth),la.capsuleHits,la.lowHits,la.edgeFound);o+=b;}
    const auto cr=u64(s.commitRequests);
    std::snprintf(b,sizeof b,"\"commit\":{\"requests\":%llu,\"accepted\":%llu,\"rejected\":{",(unsigned long long)cr,(unsigned long long)u64(s.commitAccepted));o+=b;
    for(unsigned i=0;i<RejectCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",rejectNames[i],(unsigned long long)u64(s.commitRejected[i]));o+=b;}
    std::snprintf(b,sizeof b,"},\"castsAvg\":%.2f,\"usAvg\":%.1f,\"usMax\":%llu},\"releases\":{\"valid\":%llu,\"overshoot\":%llu,\"commitChanged\":%llu,"
        "\"probeOnly\":%llu,\"invalid\":{",
        cr?double(u64(s.commitCasts))/double(cr):0.0,cr?double(u64(s.commitUsSum))/double(cr):0.0,(unsigned long long)u64(s.commitUsMax),
        (unsigned long long)u64(s.releasesValid),(unsigned long long)u64(s.releasesOvershoot),(unsigned long long)u64(s.releasesCommitChanged),
        (unsigned long long)u64(s.releasesProbeOnly));o+=b;
    {bool first=true;for(unsigned i=0;i<reasonCount;++i){const auto n=u64(s.releasesInvalid[i]);if(!n)continue;
        std::snprintf(b,sizeof b,"%s\"%s\":%llu",first?"":",",reasonNames[i],(unsigned long long)n);o+=b;first=false;}}
    o+="}},\"cancels\":{";
    for(unsigned i=0;i<CancelCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",cancelNames[i],(unsigned long long)u64(s.cancels[i]));o+=b;}
    const auto rn=u64(s.recoveryN),sn=u64(s.settleN);
    std::snprintf(b,sizeof b,"},\"warps\":{\"requested\":%llu,\"executed\":%llu,\"ok\":%llu,\"missed\":%llu,\"fell\":%llu,\"flagOwned\":%llu,"
        "\"streamFlagRestored\":%llu,\"indoorChanges\":%llu,\"crouchLost\":%llu,\"restOffset\":%.3f,\"restOffsetFallback\":%llu,\"recoveryMs\":%.0f,"
        "\"settleMs\":%.0f,\"settleDyMax\":%.3f,\"holdCapHits\":%llu,\"psiDeltaDegMax\":%.2f,\"psiCorrected\":%llu,\"psiTurnDeclined\":%llu,"
        "\"psiTurnUserSkipped\":%llu,\"psiSignFlips\":%llu,\"psiSign\":%d,\"lastDy\":%.3f,\"aborted\":{",
        (unsigned long long)u64(s.warpsRequested),(unsigned long long)u64(s.warpsExecuted),(unsigned long long)u64(s.warpsOk),(unsigned long long)u64(s.warpsMissed),
        (unsigned long long)u64(s.warpsFell),(unsigned long long)u64(s.flagOwned),(unsigned long long)u64(s.streamFlagRestored),
        (unsigned long long)u64(s.indoorChanges),(unsigned long long)u64(s.crouchLost),double(m.lastRestOffset.load()),(unsigned long long)u64(s.restOffsetFallback),
        rn?double(u64(s.recoveryMsSum))/double(rn):-1.0,sn?double(u64(s.settleMsSum))/double(sn):-1.0,double(u64(s.settleDyUmMax))/1e6,
        (unsigned long long)u64(s.holdCapHits),double(u64(s.psiDeltaUdegMax))/1e6,(unsigned long long)u64(s.psiCorrected),
        (unsigned long long)u64(s.psiTurnDeclined),(unsigned long long)u64(s.psiTurnUserSkipped),(unsigned long long)u64(s.psiSignFlips),m.psiSign(),
        double(m.lastDy.load()));o+=b;
    for(unsigned i=0;i<AbortCount;++i){std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",abortNames[i],(unsigned long long)u64(s.aborts[i]));o+=b;}
    {LastWarp lw{};m.lastWarp.load(lw);
     std::snprintf(b,sizeof b,"},\"last\":{\"serial\":%llu,\"from\":[%.2f,%.2f,%.2f],\"target\":[%.2f,%.2f,%.2f],\"after\":[%.2f,%.2f,%.2f],\"dist\":%.2f,"
        "\"errorM\":%.3f,\"dy\":%.3f,\"restOffset\":%.3f,\"ok\":%s,\"done\":%s,\"psiDeltaDeg\":%.2f,\"frameMsBefore\":%.2f,\"frameMsAfter\":%.2f}},",
        (unsigned long long)lw.serial,double(lw.from[0]),double(lw.from[1]),double(lw.from[2]),double(lw.target[0]),double(lw.target[1]),double(lw.target[2]),
        double(lw.after[0]),double(lw.after[1]),double(lw.after[2]),double(lw.dist),double(lw.errorM),double(lw.dyM),double(lw.restOffsetM),
        lw.ok?"true":"false",lw.done?"true":"false",double(lw.psiDeltaDeg),double(lw.frameMsBefore),double(lw.frameMsAfter));o+=b;}
    const auto fc=u64(s.fadeConfirms),fin=u64(s.blinkWarps);   // BLINK warps only (warpsExecuted also counts INSTANT ones)
    std::snprintf(b,sizeof b,"\"indoors\":{\"noWorld\":%llu},\"fade\":{\"extensionListed\":%d,\"extensionEnabled\":%d,\"mode\":\"%s\",\"starts\":%llu,"
        "\"confirms\":%llu,\"confirmWaitMsAvg\":%.1f,\"blinkWarps\":%llu,\"blackMsAvg\":%.1f,\"blackMsMax\":%.1f,\"lastBlackMs\":%.1f,\"totalMsAvg\":%.1f,\"ins\":%llu,"
        "\"failures\":%llu,\"instantOnly\":%llu,\"watchdog\":%llu,\"lateCommandIgnored\":%llu,\"forcedWorld\":%llu,\"cinemaDuringFade\":%llu,"
        "\"commandReadRetries\":%llu,\"pairIdentityFallbacks\":%llu,\"xrFrames\":%llu,\"xrFramesScaled\":%llu,\"blackFrames\":%llu,\"pairNotes\":%llu,\"xrPairId\":%llu},",
        (unsigned long long)u64(s.indoorsNoWorld),fade::extensionListed.load(),fade::extensionEnabled.load(),!m.blinkNow.load()?"INSTANT":m.instantOnlyNow.load()?"INSTANT_ONLY":"BLINK",
        (unsigned long long)u64(s.fadeStarts),(unsigned long long)fc,fc?double(u64(s.confirmWaitUsSum))/double(fc)/1e3:0.0,(unsigned long long)fin,
        fin?double(u64(s.blackUsSum))/double(fin)/1e3:0.0,double(u64(s.blackUsMax))/1e3,double(m.lastBlackUs())/1e3,
        fin?double(u64(s.totalUsSum))/double(fin)/1e3:0.0,(unsigned long long)u64(s.fadeIns),(unsigned long long)u64(s.fadeFailures),
        (unsigned long long)u64(s.instantOnly),(unsigned long long)linkGet(fade::stats.watchdog),(unsigned long long)linkGet(fade::stats.lateCommandIgnored),
        (unsigned long long)linkGet(fade::stats.forcedWorld),(unsigned long long)linkGet(fade::stats.cinemaDuringFade),
        (unsigned long long)linkGet(fade::stats.commandReadRetries),(unsigned long long)u64(s.pairFallbacks),
        (unsigned long long)linkGet(fade::stats.xrFrames),(unsigned long long)linkGet(fade::stats.xrFramesScaled),(unsigned long long)linkGet(fade::stats.blackFrames),
        (unsigned long long)linkGet(fade::pairNotes),(unsigned long long)fade::xrPairId.load());o+=b;
    std::snprintf(b,sizeof b,"\"link\":{\"cameraSnaps\":%llu,\"cameraSnapExpired\":%llu,\"bodySnaps\":%llu,\"weaponBridged\":%llu,\"weaponBridgeRejected\":%llu},"
        "\"arc\":{\"published\":%llu,\"hidden\":%llu,\"armed\":%s,\"arms\":%llu,\"noArcTicks\":%llu},\"refusals\":%llu,\"failStreak\":%u}",
        (unsigned long long)linkGet(warp::cameraSnaps),(unsigned long long)linkGet(warp::cameraSnapExpired),(unsigned long long)linkGet(warp::bodySnaps),
        (unsigned long long)linkGet(warp::weaponBridged),(unsigned long long)linkGet(warp::weaponBridgeRejected),
        (unsigned long long)u64(s.arcPublished),(unsigned long long)u64(s.arcHidden),m.arcArmed()?"true":"false",(unsigned long long)u64(s.arcArms),
        (unsigned long long)u64(s.noArcTicks),(unsigned long long)u64(s.refusals),m.failStreak());o+=b;
    return o;
}

#ifdef __ANDROID__
} // namespace rdrvr::vr::tploco
#include "Teleport.h"     // EngineApi, detail::samplePlayer (the strict player gate), TeleportActorWithHeading
#include "VrLocomotion.h" // gate 5: LOCO's mover gate (recovery -> SETTLING)
#include "VrControls.h"   // crouch state (warps.crouchLost, report only)
namespace rdrvr::vr::tploco {
// ============================== engine side ==============================
// ABIs (AArch64): bool FindIntersectionWithMaterial(const V4* a, const V4* b, V4* out, V4* normal, bool fixedOnly, int* mat,
// int include, int typeExclude) 0x245d99c; int sTestCapsule(a, b, r, phIntersection* hits, max, include, type, state, swept, excl, n) 0x268b300;
// void* EnvLiquidManager::GetInstance() 0x28a5fd4; bool GetWaterElevation(mgr, x, z, float* h) 0x28a700c; bool IsPositionIndoors(x, y, z) 0x245e078.
using GroundFn=bool(*)(const arc::EngineVec*,const arc::EngineVec*,arc::EngineVec*,arc::EngineVec*,bool,int*,int,int);
using CapsuleFn=int(*)(const arc::EngineVec*,const arc::EngineVec*,float,arc::EngineIntersection*,int,unsigned,unsigned,unsigned char,bool,const void* const*,int);
using LiquidInstanceFn=void*(*)();
using WaterFn=bool(*)(void*,float,float,float*);
using IndoorsFn=bool(*)(float,float,float);
struct EngineApi {
    ::rdrvr::teleport::EngineApi tp{};                       // Teleport.h's verified API (samplePlayer, TeleportActorWithHeading, globals)
    GroundFn ground{};CapsuleFn capsule{};LiquidInstanceFn liquid{};WaterFn water{};IndoorsFn indoors{};
    std::uintptr_t worldInstance{},minigameInstance{};       // &rmpWorld::sm_Instance, &atSingleton<sagMiniGameSever>::sm_Instance
};
namespace engine {
inline EngineApi api{};
inline std::atomic<std::uint32_t> playerGuid{0};             // the last sampled player GUID (capsule exclusion list)
template<class T> inline bool get(std::uintptr_t p,std::size_t o,T& out) noexcept {
    return api.tp.read&&p&&p<=UINTPTR_MAX-o&&p+o<=UINTPTR_MAX-sizeof(T)&&api.tp.read(nullptr,p+o,&out,sizeof(T));}
inline arc::EngineVec vec(const V3& p) noexcept {return {{p[0],p[1],p[2],0.f}};}
inline std::uint64_t clockNs(void*) noexcept {return nowNs();}
inline ProbeOut ground(void*,const V3& a,const V3& b,V3& point,V3& normal) noexcept {
    if(!api.ground)return ProbeOut::Failed;
    arc::ledger.take(arc::ProbeUser::Validate);const auto t0=nowNs();
    alignas(16) arc::EngineVec va=vec(a),vb=vec(b),out{},n{};int mat=-1;bool hit=false;
    try{hit=api.ground(&va,&vb,&out,&n,true,&mat,int(0x96337f7du),0x400000);}catch(...){arc::ledger.charge(arc::ProbeUser::Validate,nowNs()-t0,am::distance(a,b),true);return ProbeOut::Failed;}
    arc::ledger.charge(arc::ProbeUser::Validate,nowNs()-t0,am::distance(a,b),false);
    if(!hit)return ProbeOut::Miss;
    point={out.v[0],out.v[1],out.v[2]};normal={n.v[0],n.v[1],n.v[2]};return ProbeOut::Hit;
}
inline int capsule(void*,const V3& a,const V3& b,float r,std::uint32_t include,std::uint8_t state) noexcept {
    if(!api.capsule)return -1;
    int count=0;const void* const* list=arc::ledger.excluded(playerGuid.load(std::memory_order_relaxed),count);
    if(!list||count<=0)return -1;
    arc::ledger.take(arc::ProbeUser::Validate);const auto t0=nowNs();
    alignas(16) arc::EngineVec va=vec(a),vb=vec(b);std::array<arc::EngineIntersection,1> hits{};
    {std::uint32_t pre=0xffff;std::memcpy(hits[0].bytes+8,&pre,4);std::memcpy(hits[0].bytes+0x64,&pre,4);}
    int n=-1;try{n=api.capsule(&va,&vb,r,hits.data(),1,include,0xffffffffu,state,false,list,count);}catch(...){n=-1;}
    arc::ledger.charge(arc::ProbeUser::Validate,nowNs()-t0,am::distance(a,b),n<0);
    return n;
}
inline int water(void*,float x,float z,float& h) noexcept {
    if(!api.liquid||!api.water)return -1;
    try{void* mgr=api.liquid();if(!mgr)return -1;float out=0.f;const bool ok=api.water(mgr,x,z,&out);if(!ok)return 0;h=out;return 1;}catch(...){return -1;}
}
// One non-strict sample (~14 guarded reads) per tick is shared by player() and lasso() (2 ms cache); strict samples are never cached.
struct SampleCache {std::uint64_t ns{};::rdrvr::teleport::detail::Player p{};const char* reason{};};
inline SampleCache sampleCache{};
inline const char* sample(::rdrvr::teleport::detail::Player& s,bool strict) noexcept {
    const auto now=nowNs();
    if(!strict&&sampleCache.ns&&now>=sampleCache.ns&&now-sampleCache.ns<2'000'000ull){s=sampleCache.p;return sampleCache.reason;}
    const char* why=::rdrvr::teleport::detail::samplePlayer(api.tp,s,strict);
    if(!strict){sampleCache.ns=now;sampleCache.p=s;sampleCache.reason=why;}
    return why;
}
inline bool player(void*,bool strict,PlayerFacts& p) noexcept {
    if(!api.tp.read||!api.tp.playerManager)return false;
    ::rdrvr::teleport::detail::Player s{};p.reason=sample(s,strict);
    p.guid=s.guid;p.C={s.x,s.y,s.z};p.headingDeg=s.heading;const std::uintptr_t actor=s.actor;
    if(s.guid)playerGuid.store(s.guid,std::memory_order_relaxed);
    if(p.reason)return true;
    std::uintptr_t mover{};std::uint16_t flags{};float vel[3]{};
    if(get(actor,0x88,mover)&&mover&&get(mover,0x84c,flags)){p.moverRead=true;p.recovering=flags&1u;p.jumping=flags&0x8000u;
        if(get(mover,0x7b0,vel)&&std::isfinite(vel[1])){p.velocityKnown=true;p.vy=vel[1];}}
    return true;
}
// aniLasso user state (sagPlayerStates::CheckIsLassoAttached 0x26ab340: [[[actor+0x38]+0xab8]+0x40]+0x18); busy states {2,3,5..10,12,15}
inline int lasso(void*,std::uint32_t& stateOut) noexcept {
    ::rdrvr::teleport::detail::Player s{};
    if(!api.tp.read||!api.tp.playerManager||sample(s,false)||!s.actor)return -1;
    const std::uintptr_t actor=s.actor;
    std::uintptr_t a38{},aab8{},lasso{};std::uint32_t user{};
    if(!get(actor,0x38,a38)||!a38||!get(a38,0xab8,aab8)||!aab8||!get(aab8,0x40,lasso))return -1;
    if(!lasso)return 0;
    if(!get(lasso,0x18,user))return -1;
    stateOut=user;return user==2||user==3||(user>=5&&user<=10)||user==12||user==15?1:0;
}
// atSingleton<sagMiniGameSever>::sm_Instance -> +0x58 >= 0 (CheckIsMiniGameFocus 0x26ab414)
inline int minigame(void*) noexcept {
    std::uintptr_t inst{};std::int32_t focus{-1};
    if(!api.minigameInstance||!get(api.minigameInstance,0,inst))return -1;
    if(!inst)return 0;
    if(!get(inst,0x58,focus))return -1;
    return focus>=0?1:0;
}
inline bool warpFn(void*,std::uint32_t guid,const V3& pos,float heading) noexcept {
    if(!api.tp.teleport||!guid)return false;
    try{api.tp.teleport(guid,pos[0],pos[1],pos[2],heading,false,false,false);return true;}catch(...){return false;}
}
inline int streamFlag(void*,int write) noexcept {
    std::uintptr_t streamer{};std::uint8_t v{};
    if(!api.tp.streamer||!get(api.tp.streamer,0,streamer)||!streamer||!get(streamer,0x11d,v))return -1;
    if(write>=0)*reinterpret_cast<volatile std::uint8_t*>(streamer+0x11d)=std::uint8_t(write?1:0);
    return v;
}
inline int playerTeleporting(void*,bool clear) noexcept {
    std::uint8_t v{};if(!api.tp.playerTeleporting||!get(api.tp.playerTeleporting,0,v))return -1;
    if(clear&&v)*reinterpret_cast<volatile std::uint8_t*>(api.tp.playerTeleporting)=0;   // Own and clear it (cannot happen < 24 units)
    return v;
}
// IsPositionIndoors dereferences rmpWorld::sm_Instance->+8 without a null check: both read and non-zero first (report only)
inline int indoors(void*,const V3& p) noexcept {
    std::uintptr_t world{},rooms{};
    if(!api.indoors||!api.worldInstance||!get(api.worldInstance,0,world)||!world||!get(world,0x8,rooms)||!rooms)return -1;
    try{return api.indoors(p[0],p[1],p[2])?1:0;}catch(...){return -1;}
}
// teleportViewTurn (GameXr registers it with the snap-turn API, after this module installs): read at call time
inline int viewTurnFn(float radians,std::uint64_t& turnNs) noexcept {
    const auto fn=::rdrvr::teleport::viewTurn.load(std::memory_order_acquire);return fn?fn(radians,turnNs):-1;}
inline int crouchingFn(void*) noexcept {return ::rdrvr::vr::controls::crouching.load(std::memory_order_relaxed)?1:0;}
// gate 5: LOCO's mover gate of the newest player substep (fresh < 250 ms): Ok open, Skipped (mover+0x84c bit0 = the gravity-0 recovery after
// a warp) transient SETTLING, anything else closed with its name; a stale gate = the mover is not stepped (NO PHYSICS); no hook = skipped.
inline int locoGateFn(void*,std::uint64_t now,const char*& name) noexcept {
    namespace lc=::rdrvr::vr::loco;
    if(!lc::moverInstalled.load(std::memory_order_acquire))return -1;
    const auto last=lc::lastCallNs.load(std::memory_order_relaxed);
    if(!last||(now>last&&now-last>250'000'000ull)){name="NO PHYSICS";return 0;}
    const auto g=lc::lastGate.load(std::memory_order_relaxed);
    if(g==unsigned(lc::Gate::Ok))return 1;
    if(g==unsigned(lc::Gate::Skipped))return 2;
    name=g<lc::gateCount?lc::gateNames[g]:"LOCKED";return 0;
}
} // namespace engine
// TraceEntry: after the prints - bind the shared capsule print into the arc probe API (copy-on-write), then register.
inline void bindArcCapsule(CapsuleFn fn) noexcept {
    static arc::EngineProbeApi withCapsule{};
    const auto* cur=arc::engineApi.load(std::memory_order_acquire);
    if(!cur||!fn)return;
    withCapsule=*cur;withCapsule.capsule=fn;arc::engineApi.store(&withCapsule,std::memory_order_release);
}
inline bool install(const EngineApi& e) noexcept {
    engine::api=e;
    Env env{};env.player=&engine::player;env.lasso=&engine::lasso;env.minigame=&engine::minigame;env.locoGate=&engine::locoGateFn;
    env.warp=&engine::warpFn;env.viewTurn=&engine::viewTurnFn;env.streamFlag=&engine::streamFlag;env.playerTeleporting=&engine::playerTeleporting;
    env.indoors=e.indoors?&engine::indoors:nullptr;env.crouching=&engine::crouchingFn;
    env.probes.ground=&engine::ground;env.probes.capsule=&engine::capsule;env.probes.water=e.liquid&&e.water?&engine::water:nullptr;
    env.probes.clock=&engine::clockNs;
    if(!e.tp.read||!e.tp.teleport||!e.tp.playerManager||!e.ground||!e.capsule){unavailable("NO TELEPORT API");return false;}
    return installWith(env);
}
#endif
} // namespace rdrvr::vr::tploco
