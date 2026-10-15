#pragma once
// EYE CULL A. The union cull frustum is forced symmetric (StereoVisibility.cpp:104-109) while the
// Quest 3 eye FOV is vertically asymmetric (~44 up / 55 down raw): MAIN objects wholly in the top
// band (or outside both eyes anywhere else) are drawn by the left eye natively and by the right
// eye through the replayed tape, yet reach no pixel of either eye. Per eligible MAIN DrawCommon
// object (bodyDraw, TraceEntry.cpp, right after the SMALL OBJECTS skip; same eligibility as SMALL
// OBJECTS: the SmallObjectCull token was counted = exact stock depth/colour stage, MAIN container,
// pinned caller, outermost DrawCommon, kind 0, drawable, good sphere; plus: no creature,
// DrawCommon int argument 0 (DrawOpaque passes draw-list entry u16 & 0xfff there, meaning
// unproven: such objects are only counted, excluded.instance / instanceOutBoth, never skipped),
// not within r+1 m of the eye midpoint) the root-matrix cull sphere, inflated to 1.1r+0.1 m
// (rigid) / 1.5r+0.25 m (matrix set), is tested against the 4 side planes of each eye's RENDERED
// frustum (EyeViewports::plan(): world[e] / projection[e] of this frame, EDGE MARGIN included;
// un-jittered TAA plan). Counted per stage and class: tested, outL, outR, outBoth. With the S02
// bit 0x400 (EB) in force an object outside BOTH eyes returns before gameBodyDraw (the SMALL
// OBJECTS path: skipped for both eyes, the right eye replays the left tape). Counting runs while
// debug.rdrvr.eyecull.count=1 or 0x400/0x800 is in the S02 / S02AB mask (S02Levers.h
// eyeCullTesting, re-read every 64 scenes), so the CPU cost (~8 plane tests per object) is the
// same with the skip on and off. Kill switch: debug.rdrvr.eyecull 0. Needs the SmallObjectCull
// token: TraceEntry occluderAfterCached asks SmallObjectCull for counts (histogram 1) while
// eyeCullTesting, so SMALL OBJECTS 0 does not silence it; objects SMALL OBJECTS skips never reach
// this test. Sign convention = StereoVisibility.cpp:58/88: the eye-space ray of NDC (sx,sy) is
// ((sx+p8)/p0, (sy+p9)/p5, -1), the eye looks down -z, world columns 0..2 = eye x/y/z axes,
// 12..14 = eye position; p5 may be negative (y flip): edges are sorted. Report
// graphics.eyeCull{testing, skipWanted, frames, stockDepth/stockColor/stereoPass{tested,outL,outR,
// outBoth,skipped,rigid{..}, matrixSet{..}}, perFrame{..} (stock stages only),
// excluded{creature,instance,near,noPlan} (stock stages), instanceOutBoth,
// eyeTan[2]{up,down,outer,inner} (degrees), planeBuilds, planeFailures, stalePlans, midScenePlans,
// staleRefused} (appended after graphics.S02 through S02::eyeCullReport; cumulative counters, so
// window deltas work). Accounting per RenderScene (S02::sceneSerial); a scene still showing the
// previous scene's plan is never skipped. Render thread only, except report().
// The stereo pass (RetailSceneBoundary consumer gameDrawRetailWorld -> EngineStereoPass::drawScene) runs
// INSIDE retailColor's SmallObjectCull Color stage and calls the engine stages directly, so its native eye draws (right-eye
// fallback stages DEPTH_TAPE / COLOR_TAPE / CLONE_FLAGGED / MASK_NATIVE / REPLAY_DECLINED, eye 0 when not reused) carry a Color
// token too. TraceEntry wraps the consumer in stereoPassConsumer<> (one token at retailBoundary.configure): objects drawn while it
// runs are booked as stereoPass{..} (exclusions in stereoPass.excluded), never as stockColor, so the stock ratios stay the
// left eye's. They are still skipped with EB (the decision is eye-independent: the same object is outside both eyes).
//
// EYE CULL B (S02 bit 0x800 EC; debug.rdrvr.eyecull 0 off / 1 A only / 2 A + B / 3 A + B SWAPPED = diagnostic; unset = the
// S02 bits decide, the cfg mask path uses S02Levers.h eyeCullDefaultMode):
// the same tested object, wholly outside ONE eye (outL xor outR; outside both also when A is not in force), gets a pad
// (EyeCullTape.h) for every draw its DrawCommon issues: decide() returns it, bodyDraw holds it in tape::Scope until DrawCommon
// returns (nested / uncounted / excluded DrawCommon calls get pad 0 while testing). The left eye records the draw into the stereo
// tape (Header.pad = pad) but skips the live draw when outside the LEFT eye (CameraIntegration.h); the right replay skips tape
// draws flagged outside the RIGHT eye (StereoReplay.cpp / StereoReplayAsync.cpp; state ops still replayed). Stock stages only (the
// stereo pass draws the right eye natively: no pad). Mode 3 swaps the bits: the left eye drops what the RIGHT eye cannot see (its
// own outer-left wedge) and vice versa, so an eye dump SHOWS the classified objects as holes at the outer edges (the signature
// gets 0x1000). Same eligibility, margins and stale rule as A. Extra report: mode, singleWanted, swap, singleEye{flagged
// [depth,colour]{L,R}, padDraws, leftDropped, rightDropped [depth,colour], multiInstance, notRecorded, perFrame{..}}, byDist
// (stock stages, centre distance from the eye midpoint, edges 10/30/80/200 m = the ablation's dist buckets: tested/outL/outR/outBoth).
#include "S02Levers.h"
#include "SmallObjectCull.h"
#include "EyeCullTape.h" // EYE CULL B: per-draw pad (TLS scope, tape header, left/right drop counters)
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace rdrvr::engine::eyecull {
// ---- pure plane math (host-tested) ----
enum Side : unsigned {Left=0,Right=1,Bottom=2,Top=3};
struct Plane {float n[3]{};float d{};};                        // inside: n.(p-origin)+d >= 0, |n| = 1
struct EyeFrustum {std::array<Plane,4> side{};std::array<float,4> tan{};}; // tan: left/right (x/-z), bottom/top (y/-z) edges
struct Planes {std::array<EyeFrustum,2> eye{};double origin[3]{};bool valid{};};
inline bool rigidWorld(const float* m) noexcept {
    for(unsigned i=0;i<16;++i)if(!std::isfinite(m[i]))return false;
    const auto dot=[m](unsigned a,unsigned b){return double(m[a])*m[b]+double(m[a+1])*m[b+1]+double(m[a+2])*m[b+2];};
    return std::fabs(dot(0,0)-1)<1e-3&&std::fabs(dot(4,4)-1)<1e-3&&std::fabs(dot(8,8)-1)<1e-3&&
        std::fabs(dot(0,4))<1e-3&&std::fabs(dot(0,8))<1e-3&&std::fabs(dot(4,8))<1e-3;
}
// One eye: m = eye->world (column-major), p = its projection (column-major, GL-style -1 at [11]); origin = plane origin.
inline bool buildEye(const float* m,const float* p,const double* origin,EyeFrustum& out) noexcept {
    if(!rigidWorld(m))return false;
    for(unsigned i=0;i<16;++i)if(!std::isfinite(p[i]))return false;
    if(!(p[0]>0.f)||!(std::fabs(p[5])>1e-4f)||!(std::fabs(p[11]+1.f)<1e-4f))return false;
    const double h0=(-1.0+p[8])/p[0],h1=(1.0+p[8])/p[0],v0=(-1.0+p[9])/p[5],v1=(1.0+p[9])/p[5];
    const double t[4]{std::min(h0,h1),std::max(h0,h1),std::min(v0,v1),std::max(v0,v1)};
    if(!(t[Left]<t[Right])||!(t[Bottom]<t[Top]))return false;
    // inward eye-space normals of the planes through the eye: x >= l*D, x <= r*D, y >= b*D, y <= t*D with D = -z
    const double ne[4][3]{{1,0,t[Left]},{-1,0,-t[Right]},{0,1,t[Bottom]},{0,-1,-t[Top]}};
    const double c[3]{double(m[12])-origin[0],double(m[13])-origin[1],double(m[14])-origin[2]};
    for(unsigned k=0;k<4;++k){
        const double len=std::sqrt(ne[k][0]*ne[k][0]+ne[k][1]*ne[k][1]+ne[k][2]*ne[k][2]);
        double n[3];for(unsigned j=0;j<3;++j)n[j]=(ne[k][0]*m[j]+ne[k][1]*m[4+j]+ne[k][2]*m[8+j])/len;
        auto& s=out.side[k];for(unsigned j=0;j<3;++j)s.n[j]=float(n[j]);
        s.d=float(-(n[0]*c[0]+n[1]*c[1]+n[2]*c[2]));out.tan[k]=float(t[k]);
    }
    return true;
}
// Both eyes; the origin is the eye midpoint (world positions of distant towns lose float precision otherwise).
inline bool build(const float* world0,const float* world1,const float* projection0,const float* projection1,Planes& out) noexcept {
    out=Planes{};
    for(unsigned j=0;j<3;++j)out.origin[j]=(double(world0[12+j])+double(world1[12+j]))*0.5;
    out.valid=buildEye(world0,projection0,out.origin,out.eye[0])&&buildEye(world1,projection1,out.origin,out.eye[1]);
    return out.valid;
}
// bit0 = the sphere lies entirely outside eye 0 (left), bit1 = entirely outside eye 1 (right): beyond -radius of a side plane.
inline unsigned outside(const Planes& p,const float* centre,float radius) noexcept {
    const float q[3]{float(double(centre[0])-p.origin[0]),float(double(centre[1])-p.origin[1]),float(double(centre[2])-p.origin[2])};
    unsigned bits=0;
    for(unsigned e=0;e<2;++e)for(const auto& s:p.eye[e].side)
        if(s.n[0]*q[0]+s.n[1]*q[1]+s.n[2]*q[2]+s.d< -radius){bits|=1u<<e;break;}
    return bits;
}
// Test radius (world units; u = units per metre): rigid 1.1r+0.1 m, matrix set (articulated / fragment props) 1.5r+0.25 m.
inline float inflate(float r,bool matrixSet,float u) noexcept {return matrixSet?1.5f*r+0.25f*u:1.1f*r+0.1f*u;}
// Held items / props at the face: never tested when the eye midpoint lies within r+1 m of the centre.
inline bool nearEyes(const Planes& p,const float* c,float r,float u) noexcept {
    const double d[3]{double(c[0])-p.origin[0],double(c[1])-p.origin[1],double(c[2])-p.origin[2]};
    const double lim=double(r)+1.0*u;return d[0]*d[0]+d[1]*d[1]+d[2]*d[2]<lim*lim;
}
// Edge angles in degrees: up/down, outer (temporal: left edge of eye 0, right edge of eye 1) / inner (nasal).
inline std::array<float,4> eyeDegrees(const EyeFrustum& f,unsigned eye) noexcept {
    constexpr double deg=57.29577951308232;
    const float l=float(std::atan(-double(f.tan[Left]))*deg),r=float(std::atan(double(f.tan[Right]))*deg);
    return {float(std::atan(double(f.tan[Top]))*deg),float(std::atan(-double(f.tan[Bottom]))*deg),eye?r:l,eye?l:r};
}
// The pad of an object outside `out` (bit0 left, bit1 right) of its eyes; swap = mode 3 (each eye drops the other's set).
inline std::uint16_t padOf(unsigned out,bool color,bool swap) noexcept {
    const unsigned drop=swap?((out&1u)<<1|(out&2u)>>1):out;
    return std::uint16_t((drop&tape::dropBits)|(color?tape::ColorStage:0u)|(swap?tape::Swap:0u));
}
// Distance buckets of the ablation (CategoryAblation.h distEdges, metres; NaN -> last).
inline constexpr unsigned distBuckets=5;inline constexpr float distEdges[distBuckets-1]{10,30,80,200};
inline unsigned distBucket(float metres) noexcept {unsigned b=0;while(b<distBuckets-1&&!(metres<distEdges[b]))++b;return b;}

// ---- runtime (render thread) ----
enum Exclusion : unsigned {Creature,Instance,Near,NoPlan,ExclusionCount};
inline constexpr const char* exclusionNames[ExclusionCount]{"creature","instance","near","noPlan"};
inline constexpr unsigned stageCount=3;               // stockDepth, stockColor, stereoPass
// [render] > 0 while the RetailSceneBoundary consumer (the stereo pass) runs.
inline unsigned stereoPassDepth{};
template<auto Consumer,class Frame> bool stereoPassConsumer(const Frame& frame) {
    struct Scope {Scope(){++stereoPassDepth;}~Scope(){--stereoPassDepth;}} scope; // unwinds with the consumer's exceptions too
    return Consumer(frame);
}
using Grid=std::array<std::array<std::uint64_t,2>,stageCount>; // [stage depth/colour/stereo pass][class rigid/matrix set]
using ExclusionRows=std::array<std::array<std::uint64_t,ExclusionCount>,2>; // [stock, stereo pass]
using DistRows=std::array<std::array<std::uint64_t,distBuckets>,4>;         // [tested,outL,outR,outBoth][bucket], stock stages
using FlagRows=std::array<std::array<std::uint64_t,2>,2>;                   // [stock depth/colour][L,R] objects given a pad
struct Stats {
    std::array<std::array<std::atomic<std::uint64_t>,2>,stageCount> tested{},outL{},outR{},outBoth{};
    std::array<std::atomic<std::uint64_t>,stageCount> skipped{};
    std::array<std::array<std::atomic<std::uint64_t>,ExclusionCount>,2> excluded{};
    // frames = RenderScenes with valid planes; stalePlans = a scene whose plan was the previous scene's (no skips then);
    // midScenePlans = the plan changed inside one scene (planes rebuilt); staleRefused = outside-both objects not skipped as stale
    std::atomic<std::uint64_t> frames{},planeBuilds{},planeFailures{},stalePlans{},midScenePlans{},staleRefused{},instanceOutBoth{};
    std::array<std::array<std::atomic<float>,4>,2> eyeDeg{};
    std::array<std::array<std::atomic<std::uint64_t>,distBuckets>,4> dist{};
    std::array<std::array<std::atomic<std::uint64_t>,2>,2> flagged{};
    std::atomic<std::uint64_t> staleSingle{};                                 // Single-eye objects not padded (stale plan)
};
inline Stats stats;
struct Frame {                                // accumulators of the current scene, flushed into stats at the next one
    std::uint64_t scene{},planKey{};bool open{},stale{};Planes planes{};float unitsPerMetre{1.f};
    Grid tested{},outL{},outR{},outBoth{};std::array<std::uint64_t,stageCount> skipped{};ExclusionRows excluded{};
    std::uint64_t instanceOutBoth{};
    DistRows dist{};FlagRows flagged{};
    bool midScene{}; // The plan changed inside this scene: no B pads for the rest of it (the eye may render the older plan)
};
inline Frame frame;
inline void flush() noexcept {
    auto& f=frame;if(!f.open)return;
    const auto add=[](auto& a,std::uint64_t v){if(v)a.fetch_add(v,std::memory_order_relaxed);};
    for(unsigned s=0;s<stageCount;++s){for(unsigned c=0;c<2;++c){add(stats.tested[s][c],f.tested[s][c]);add(stats.outL[s][c],f.outL[s][c]);
            add(stats.outR[s][c],f.outR[s][c]);add(stats.outBoth[s][c],f.outBoth[s][c]);}
        add(stats.skipped[s],f.skipped[s]);}
    for(unsigned x=0;x<2;++x)for(unsigned e=0;e<ExclusionCount;++e)add(stats.excluded[x][e],f.excluded[x][e]);
    add(stats.instanceOutBoth,f.instanceOutBoth);
    for(unsigned k=0;k<4;++k)for(unsigned b=0;b<distBuckets;++b)add(stats.dist[k][b],f.dist[k][b]);
    for(unsigned s=0;s<2;++s)for(unsigned e=0;e<2;++e)add(stats.flagged[s][e],f.flagged[s][e]);
    if(f.planes.valid)stats.frames.fetch_add(1,std::memory_order_relaxed);
    f.open=false;
}
// A new RenderScene or a new plan inside one: publish the last scene, build this one's planes. A scene that
// still shows the previous scene's plan is stale (counted, never skipped).
template<class Plan> void beginFrame(const Plan& plan,std::uint64_t planKey,std::uint64_t scene) noexcept {
    auto& f=frame;
    const bool sameScene=f.open&&scene==f.scene,stale=f.open&&!sameScene&&planKey==f.planKey;
    if(sameScene){stats.midScenePlans.fetch_add(1,std::memory_order_relaxed);f.stale=false;f.midScene=true;} // keep the accumulators, new planes
    else{flush();f.tested={};f.outL={};f.outR={};f.outBoth={};f.skipped={};f.excluded={};f.instanceOutBoth=0;f.dist={};f.flagged={};
        f.scene=scene;f.open=true;f.stale=stale;f.midScene=false;
        if(stale)stats.stalePlans.fetch_add(1,std::memory_order_relaxed);}
    f.planKey=planKey;
    f.unitsPerMetre=std::isfinite(plan.unitsPerMetre)&&plan.unitsPerMetre>0.f?plan.unitsPerMetre:1.f;
    if(build(plan.world[0].data(),plan.world[1].data(),plan.projection[0].data(),plan.projection[1].data(),f.planes)){
        stats.planeBuilds.fetch_add(1,std::memory_order_relaxed);
        for(unsigned e=0;e<2;++e){const auto d=eyeDegrees(f.planes.eye[e],e);for(unsigned k=0;k<4;++k)stats.eyeDeg[e][k].store(d[k],std::memory_order_relaxed);}
        if(!f.stale)s02::eyeCullScene.store(scene,std::memory_order_relaxed); // ecGate: live
    }
    else stats.planeFailures.fetch_add(1,std::memory_order_relaxed);
}
// What bodyDraw does with one DrawCommon object. skip = return before gameBodyDraw (A: outside both eyes); pad = the eye
// bits of its draws (B); scoped = hold `pad` in tape::Scope for this DrawCommon (true whenever testing is on: nested / uncounted /
// excluded calls then get pad 0 instead of an enclosing object's).
struct Decision {bool skip{};std::uint16_t pad{};bool scoped{};};
// The tested path (eyeCullTesting on, token counted).
template<class Provider> Decision test(const SmallObjectCull::Token& t,const void* info,int instance,Provider provider) noexcept {
    constexpr Decision none{false,0,true};
    auto& f=frame;
    // Stereo-pass draws (inside the retail consumer) are booked apart from the stock stages
    const unsigned s=stereoPassDepth?2u:t.stage==SmallObjectCull::Color?1u:0u,x=s==2u?1u:0u;
    const auto* views=provider();
    if(!views||!views->ready()){stats.excluded[x][NoPlan].fetch_add(1,std::memory_order_relaxed);return none;}
    const auto& plan=views->plan();
    const std::uint64_t planKey=(plan.sceneId*0x9E3779B97F4A7C15ull)^plan.tracking.frameId,scene=s02::sceneSerial.load(std::memory_order_relaxed);
    if(!f.open||scene!=f.scene||planKey!=f.planKey)beginFrame(plan,planKey,scene);
    if(!f.planes.valid){++f.excluded[x][NoPlan];return none;}
    if(t.cls==SmallObjectCull::Creature){++f.excluded[x][Creature];return none;}
    float c[3],r;if(!SmallObjectCull::sphere(static_cast<const unsigned char*>(info),c,r))return none; // counted => good sphere
    if(nearEyes(f.planes,c,r,f.unitsPerMetre)){++f.excluded[x][Near];return none;}
    const unsigned cls=t.cls==SmallObjectCull::MatrixSet?1u:0u;
    const unsigned out=outside(f.planes,c,inflate(r,cls!=0,f.unitsPerMetre));
    if(instance!=0){++f.excluded[x][Instance];if(out==3u&&!x)++f.instanceOutBoth;return none;} // counted only (see the header)
    ++f.tested[s][cls];if(out&1u)++f.outL[s][cls];if(out&2u)++f.outR[s][cls];
    if(!x){ // byDist (stock stages): centre distance from the eye midpoint
        const double d[3]{double(c[0])-f.planes.origin[0],double(c[1])-f.planes.origin[1],double(c[2])-f.planes.origin[2]};
        const unsigned b=distBucket(float(std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2])/double(f.unitsPerMetre)));
        ++f.dist[0][b];if(out&1u)++f.dist[1][b];if(out&2u)++f.dist[2][b];if(out==3u)++f.dist[3][b];}
    if(!out)return none;
    const unsigned mode=s02::eyeCullMode(); // 0 kills A and B whatever the S02 bits say
    if(out==3u){
        ++f.outBoth[s][cls];
        if(mode>=1&&s02::wants(s02::ECBoth)){
            if(f.stale){stats.staleRefused.fetch_add(1,std::memory_order_relaxed);return none;}
            ++f.skipped[s];return {true,0,true};}
    }
    // EYE CULL B: stock stages only (the stereo pass draws the right eye natively from bodyDraw, no tape)
    if(x||mode<2||!s02::wants(s02::ECRight))return none;
    if(f.stale||f.midScene){stats.staleSingle.fetch_add(1,std::memory_order_relaxed);return none;}
    if(out&1u)++f.flagged[s][0];if(out&2u)++f.flagged[s][1];
    return {false,padOf(out,s==1u,mode==3u),true};
}
// bodyDraw (TraceEntry.cpp), after `if(smallToken.skip)return;`: one relaxed load + branch unless counting/EB/EC is on.
// provider() returns the EyeViewports (gameEyeViewports): ready() and plan() of this frame.
template<class Provider> inline Decision decide(const SmallObjectCull::Token& t,const void* info,int instance,Provider provider) noexcept {
    if(!s02::eyeCullTesting.load(std::memory_order_relaxed))return {};
    if(!t.counted)return {false,0,true};
    return test(t,info,instance,provider);
}
// API (host tests): true = skip the object (outside both eyes and EB in force).
template<class Provider> inline bool object(const SmallObjectCull::Token& t,const void* info,int instance,Provider provider) noexcept {
    return decide(t,info,instance,provider).skip;
}

// ---- report: "eyeCull":{...} ----
inline std::string report() {
    std::ostringstream o;o.setf(std::ios::fixed);o.precision(2);
    const auto u=[](const std::atomic<std::uint64_t>& a){return a.load(std::memory_order_relaxed);};
    const std::uint64_t frames=u(stats.frames);
    const auto per=[frames](std::uint64_t v){return frames?double(v)/double(frames):0.0;};
    const unsigned mode=s02::eyeCullMode();
    o<<"\"eyeCull\":{\"testing\":"<<(s02::eyeCullTesting.load()?1:0)<<",\"skipWanted\":"<<(s02::wants(s02::ECBoth)&&mode>=1?1:0)
     <<",\"mode\":"<<mode<<",\"singleWanted\":"<<(s02::wants(s02::ECRight)&&mode>=2?1:0)<<",\"swap\":"<<(mode==3?1:0)
     <<",\"frames\":"<<frames<<",\"planeBuilds\":"<<u(stats.planeBuilds)<<",\"planeFailures\":"<<u(stats.planeFailures)
     <<",\"stalePlans\":"<<u(stats.stalePlans)<<",\"midScenePlans\":"<<u(stats.midScenePlans)<<",\"staleRefused\":"<<u(stats.staleRefused);
    std::uint64_t all[5]{};
    static constexpr const char* stageNames[stageCount]{"stockDepth","stockColor","stereoPass"};
    static constexpr const char* classNames[2]{"rigid","matrixSet"};
    const auto exclusions=[&](unsigned x){for(unsigned e=0;e<ExclusionCount;++e)o<<(e?",":"")<<'"'<<exclusionNames[e]<<"\":"<<u(stats.excluded[x][e]);};
    for(unsigned s=0;s<stageCount;++s){
        std::uint64_t t=0,l=0,r=0,b=0;
        for(unsigned c=0;c<2;++c){t+=u(stats.tested[s][c]);l+=u(stats.outL[s][c]);r+=u(stats.outR[s][c]);b+=u(stats.outBoth[s][c]);}
        o<<",\""<<stageNames[s]<<"\":{\"tested\":"<<t<<",\"outL\":"<<l<<",\"outR\":"<<r<<",\"outBoth\":"<<b<<",\"skipped\":"<<u(stats.skipped[s]);
        for(unsigned c=0;c<2;++c)o<<",\""<<classNames[c]<<"\":{\"tested\":"<<u(stats.tested[s][c])<<",\"outL\":"<<u(stats.outL[s][c])
            <<",\"outR\":"<<u(stats.outR[s][c])<<",\"outBoth\":"<<u(stats.outBoth[s][c])<<'}';
        if(s==2){o<<",\"excluded\":{";exclusions(1);o<<'}';} // stereo pass apart
        o<<'}';if(s<2){all[0]+=t;all[1]+=l;all[2]+=r;all[3]+=b;all[4]+=u(stats.skipped[s]);}
    }
    o<<",\"perFrame\":{\"tested\":"<<per(all[0])<<",\"outL\":"<<per(all[1])<<",\"outR\":"<<per(all[2])<<",\"outBoth\":"<<per(all[3])
     <<",\"skipped\":"<<per(all[4])<<"},\"excluded\":{";
    exclusions(0);
    o<<"},\"instanceOutBoth\":"<<u(stats.instanceOutBoth)<<",\"eyeTan\":[";
    for(unsigned e=0;e<2;++e){const auto& d=stats.eyeDeg[e];
        o<<(e?",":"")<<"{\"up\":"<<d[0].load()<<",\"down\":"<<d[1].load()<<",\"outer\":"<<d[2].load()<<",\"inner\":"<<d[3].load()<<'}';}
    o<<"],\"margin\":\"rigid 1.1r+0.1m, matrixSet 1.5r+0.25m, none within r+1m of the eye midpoint\"";
    { // EYE CULL B: objects given a pad, draw-level drops (EyeCullTape.h), byDist
        const auto& k=tape::counters;
        const auto pair=[&](const std::array<std::atomic<std::uint64_t>,2>& a){o<<'['<<u(a[0])<<','<<u(a[1])<<']';};
        o<<",\"singleEye\":{\"flagged\":{\"depth\":{\"L\":"<<u(stats.flagged[0][0])<<",\"R\":"<<u(stats.flagged[0][1])<<"},\"color\":{\"L\":"
         <<u(stats.flagged[1][0])<<",\"R\":"<<u(stats.flagged[1][1])<<"}},\"padDraws\":";pair(k.padDraws);
        o<<",\"leftDropped\":";pair(k.leftDropped);o<<",\"rightDropped\":";pair(k.rightDropped);
        o<<",\"multiInstance\":"<<u(k.multiInstance)<<",\"notRecorded\":"<<u(k.notRecorded)<<",\"staleSingle\":"<<u(stats.staleSingle)
         <<",\"perFrame\":{\"flaggedL\":"<<per(u(stats.flagged[0][0])+u(stats.flagged[1][0]))<<",\"flaggedR\":"<<per(u(stats.flagged[0][1])+u(stats.flagged[1][1]))
         <<",\"leftDropped\":"<<per(u(k.leftDropped[0])+u(k.leftDropped[1]))<<",\"rightDropped\":"<<per(u(k.rightDropped[0])+u(k.rightDropped[1]))<<"}}";
        static constexpr const char* distNames[4]{"tested","outL","outR","outBoth"};
        o<<",\"byDist\":{\"edgesM\":[10,30,80,200]";
        for(unsigned r=0;r<4;++r){o<<",\""<<distNames[r]<<"\":[";for(unsigned b=0;b<distBuckets;++b)o<<(b?",":"")<<u(stats.dist[r][b]);o<<']';}
        o<<'}';
    }
    o<<'}';
    return o.str();
}
inline void configure(bool bodyReady) noexcept {
    s02::setInstalled(s02::ECBoth|s02::ECRight,bodyReady);
    for(const unsigned b:{unsigned(s02::ECBoth),unsigned(s02::ECRight)})s02::setFingerprint(b,bodyReady?"ok":"noBodyDraw");
}
inline const bool registered=[]{s02::eyeCullReport.store(&report,std::memory_order_relaxed);configure(false);return true;}();
// Host tests.
inline void resetForTests() noexcept {
    for(auto& s:stats.tested)for(auto& a:s)a=0;for(auto& s:stats.outL)for(auto& a:s)a=0;for(auto& s:stats.outR)for(auto& a:s)a=0;
    for(auto& s:stats.outBoth)for(auto& a:s)a=0;for(auto& a:stats.skipped)a=0;for(auto& x:stats.excluded)for(auto& a:x)a=0;stereoPassDepth=0;
    stats.frames=0;stats.planeBuilds=0;stats.planeFailures=0;stats.stalePlans=0;stats.midScenePlans=0;stats.staleRefused=0;stats.instanceOutBoth=0;
    for(auto& e:stats.eyeDeg)for(auto& a:e)a=0.f;frame=Frame{};
    for(auto& r:stats.dist)for(auto& a:r)a=0;for(auto& r:stats.flagged)for(auto& a:r)a=0;stats.staleSingle=0;tape::resetForTests();
}
} // namespace rdrvr::engine::eyecull
