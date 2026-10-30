#pragma once
// SEAT: holds the rider still in the saddle. Without it the native idle animations and the gallop crouch leave the rider without a head in view.
// The riding eye is fixed to the saddle (GameXr ridingAnchorSample) and the neck subtree is collapsed for the eyes (VrBody hook C), so every
// native riding idle / lean / gallop crouch that carries the torso away from the fixed eye shows the collapsed neck stump in front of the
// view (measured: the head joint 0.28..0.52 m forward / up to 0.36 m sideways of the rider root at a STANDSTILL).
// The seat hold (VrBody hook A, mounted path, BEFORE the arm IK, under VrBody's busy guard; pure math here, the memory IO in VrBody.h):
//  - chain = the head joint's ancestors below it down to (excluding) the root bone 0 / the pelvis: spine00..spine03 + neck on the human rig
//    (0 character_root, 1..4 spine00..03, 5 neck, 6 head; the pelvis 93 expected a SIBLING of spine00 under character_root - taken from the
//    NPC name order, unverified on the player rig, so BuildChain checks it: a pelvis / hip under a chain bone starts the chain ABOVE it, hips
//    and legs stay native either way). Resolved per crSkeletonData from its parent indices (VrBody resolveRig), reported as
//    vr.body.seat.chain {bones, cut, pelvis, pelvisParent}.
//  - upright frame F = the skeleton root with its pitch / roll removed: the root's b axis (the rider matrix's up, as GameXr's riding eye
//    reads it) turned onto world up by the minimal rotation - a FIXED axis (picking the axis nearest to up each frame jumps
//    90 deg past a 45 deg tilt); past a 70 deg tilt (a rear / a buck) the last F of this mount is held (tiltFrames).
//  - reference = an upright seated spine: the BIND pose (crSkeletonData+0x20 default locals, stride 0x40 = what crSkeleton::Reset 0x1b04434
//    copies into the locals) with spine00 turned so the bind spine00 -> head line is VERTICAL (a tilted bind would stay for the
//    whole ride). .seat.ref 1 (or a bind more than 26 deg off the root up) = the native pose captured once per mount at a calm, centred moment
//    (speed < 0.3 m/s, the head within 3 cm of the highest CALM head, over the saddle's centre line, shoulders square, 0.5 s; its spine line
//    made vertical too); no capture after .seat.wait -> the bind. A ramp out with the reference gone keeps the last one applied.
//  - follow: the torso follows the VR head moderately - a twist toward the HMD heading shared by the spine bones (soft-limited .seat.twist,
//    held at the limit up to 180 deg with a side hysteresis past 150 deg, so a look over the shoulder never flips or un-twists it) and a lean
//    (pitch / roll, shared by the spine bones) that puts the HEAD joint horizontally on VrBody's neck-model target (neckTarget: BODY BACK
//    behind the eye = the on-foot head-lock rule, minus HORSE CAM FWD), soft-limited .seat.lean. .seat.tau smooths the TARGET in the rider
//    frame (it turns / moves with the horse); the lean is solved every frame against the native spine base, so the gallop's hip bounce is
//    cancelled without lag (smoothing the solved lean passes ~60 % of a 2 Hz base motion to the head). The HEAD joint (top of
//    the neck = the pivot the HMD neck model and the riding eye calibrate from), not the neck joint 10 cm lower. .seat.follow 0 = rigid.
//  - weight ramps in .seat.ramp after the mount animation and out on a dismount / gate change; ragdoll / cutscene / death / a stale gate cut
//    at once (the native pose, nothing written). The chain LOCALS are written (rotation; the native translation) and the globals of the
//    whole spine00 subtree (neck, head + face, clavicles + arms + fingers, *_Attachment) are carried rigidly with their nearest chain bone -
//    a later full crSkeleton::Update of this frame (G = G[parent] * L) reproduces the pose; the arm IK then solves from the new shoulders and
//    the rein rope reads the IK'd wrists; the head collapse (hook C) collapses about the new neck joint. Hook B: drift of the chain
//    after the engine IK (aim / rein 'spine+' chains) -> counted and re-asserted.
//  - GameXr's riding eye calibrates its forward seat from the head joint (lf), which is now OUR pose -> while the hold is active it takes the
//    reference head's forward of the rider root + BODY BACK, LATCHED once per mount (the mean over the first 1 s at full weight; a
//    running mean lets the camera creep with the native hip motion), side 0 and no gallop lean (holding()); no feedback loop.
// No engine address (struct fields of the skeleton VrBody already resolves). Levers debug.rdrvr.vr.body.seat 0 (off), .seat.ref 0 / 1,
// .seat.follow <0..1>, .seat.lean <deg 0..60>, .seat.twist <deg 0..90>, .seat.tau <ms>, .seat.ramp <ms>, .seat.wait <ms, 0 = forever>,
// .seat.reassert 0, .seat.eye 0 (the riding eye keeps a fixed 0.10 m forward seat while holding). Report vr.body.seat.
#include "VrArmIk.h"
#include "VrMainThread.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rdrvr::vr::seat {
namespace A=::rdrvr::vr::armik;
inline constexpr int maxChain=8,maxBones=256;
inline constexpr float tiltHoldDot=.34f;                 // cos 70 deg: past it the last upright frame of this mount is held
using Counter=std::atomic<std::uint64_t>;
inline void bump(Counter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline void maxOf(std::atomic<float>& a,float v) noexcept {float m=a.load(std::memory_order_relaxed);while(v>m&&!a.compare_exchange_weak(m,v)){}}
inline void put(std::atomic<float>& a,float v) noexcept {if(std::isfinite(v))a.store(v,std::memory_order_relaxed);}

// ============================== configuration (VrBody startup fills it from the debug.rdrvr.vr.body.seat* properties) ==============================
struct Config {bool on{true},capture{false},reassert{true},eye{true};float follow{1.f},leanDeg{30.f},twistDeg{40.f},tauMs{80.f},rampS{.3f},waitS{3.f};};
inline Config config{};

// ============================== chain (main thread, per crSkeletonData) ==============================
struct Chain {
    bool ok{};const char* why{"NOT_RUN"};
    int n{},spineN{},head{-1},baseParent{-1},subCount{},clavicles{},cut{},pelvis{-1},pelvisParent{-3};
    std::array<int,maxChain> bone{};                     // bone[0] = the lowest spine bone (spine00) .. bone[n-1] = the head's parent (neck)
    A::Quat bindBase{};std::array<A::Quat,maxChain> bindLocal{}; // bind rotation of bone[0] in the skeleton-root frame (spine line vertical); bind locals (k >= 1)
    A::Vec3 bindSpine{};float bindLenCm{},bindUpDot{},bindPitchDeg{},bindSideDeg{};bool bindUpright{}; // bind bone[0] -> head (unit, root frame) and its tilt
    std::array<std::int16_t,maxBones> sub{};std::array<std::int8_t,maxBones> anchor{}; // every other descendant of bone[0] + its nearest chain bone
};
inline int chainIndex(const Chain& c,int b) noexcept {for(int k=0;k<c.n;++k)if(c.bone[std::size_t(k)]==b)return k;return -1;}
// parentAt(i) -> int (parent(0) is never followed: bone 0 is the root bone whatever its parent word says); defaults = every bone's bind local.
template<class ParentAt>
inline bool BuildChain(int count,int head,int neck,int pelvis,int hipR,int hipL,int clavicleR,int clavicleL,ParentAt&& parentAt,const A::Mat34* defaults,
                       Chain& c) noexcept {
    c=Chain{};c.pelvis=pelvis;c.pelvisParent=pelvis>=0&&pelvis<count?parentAt(pelvis):-3;
    if(head<=0||head>=count||count>maxBones||!defaults){c.why="NO_HEAD";return false;}
    std::array<int,maxChain> upw{};int n=0;int b=parentAt(head);
    while(b>0&&b<count&&b!=pelvis){if(n==maxChain){c.why="CHAIN_LONG";return false;}upw[std::size_t(n++)]=b;b=parentAt(b);}
    if(b<0||b>=count){c.why="CHAIN_PARENT";return false;}
    if(n<2){c.why="CHAIN_SHORT";return false;}
    if(neck>=0&&upw[0]!=neck){c.why="HEAD_NOT_UNDER_NECK";return false;}
    std::array<int,maxChain> low{};for(int k=0;k<n;++k)low[std::size_t(k)]=upw[std::size_t(n-1-k)];
    // hips / legs stay native only if no chain bone carries the pelvis or a hip (the engine's leg IK names its chain "spine,
    // pelvis, hip..."; the sibling layout came from NPC names without parents) - else the chain starts above the deepest chain bone that does
    int cut=0;
    for(const int x:{pelvis,hipR,hipL}){if(x<0||x>=count)continue;
        for(int k=n-1;k>=0;--k)if(A::IsAncestor(low[std::size_t(k)],x,parentAt)){cut=std::max(cut,k+1);break;}}
    c.cut=cut;
    if(cut>0){if(n-cut<2){c.why="PELVIS_UNDER_SPINE";return false;}b=low[std::size_t(cut-1)];}
    c.n=n-cut;c.spineN=c.n-1;c.head=head;c.baseParent=b;
    for(int k=0;k<c.n;++k)c.bone[std::size_t(k)]=low[std::size_t(k+cut)];
    // bind of bone[0] in the skeleton-root frame (crSkeleton::Update: G0 = Root * L0, Gi = G[parent] * Li)
    A::Mat34 m{};int x=c.bone[0];
    for(int guard=0;;++guard){if(guard>=16||x<0||x>=count){c.why="BIND_PATH";return false;}m=defaults[x]*m;if(x==0)break;x=parentAt(x);}
    A::Mat34 mh=m;for(int k=1;k<c.n;++k)mh=mh*defaults[c.bone[std::size_t(k)]];mh=mh*defaults[head];
    const A::Vec3 v=mh.d-m.d;const float len=A::Length(v);
    if(!A::Finite(m)||!A::Finite(mh)||!(len>.2f&&len<1.5f)){c.why="BIND_LENGTH";return false;}
    c.bindSpine=v*(1.f/len);c.bindLenCm=len*100.f;
    // the bind tilt against the root up (b = +y; forward = -c, side = up x forward = -x: GameXr's riding-eye axes); > 0.9 = upright enough
    // to be the reference (26 deg), its residual tilt removed below
    c.bindUpDot=c.bindSpine.y;c.bindPitchDeg=std::atan2(-c.bindSpine.z,c.bindSpine.y)/A::kDegToRad;c.bindSideDeg=std::atan2(-c.bindSpine.x,c.bindSpine.y)/A::kDegToRad;
    c.bindUpright=c.bindUpDot>.9f;
    c.bindBase=A::Normalize(A::FromTo(c.bindSpine,{0,1,0})*A::ToQuat(A::Orthonormalize(m.Rot()))); // the spine00 -> head line vertical
    for(int k=1;k<c.n;++k)c.bindLocal[std::size_t(k)]=A::ToQuat(A::Orthonormalize(defaults[c.bone[std::size_t(k)]].Rot()));
    // the subtree: every non-chain descendant of bone[0] with its nearest chain ancestor (the rigid carry needs no index order)
    for(int i=0;i<count;++i){
        if(i==c.bone[0]||chainIndex(c,i)>=0)continue;
        int p=i,anc=-1;
        for(int guard=0;guard<64&&p>0;++guard){p=parentAt(p);const int k=chainIndex(c,p);if(k>=0){anc=k;break;}if(p==pelvis)break;}
        if(anc<0)continue;
        c.sub[std::size_t(c.subCount)]=std::int16_t(i);c.anchor[std::size_t(c.subCount)]=std::int8_t(anc);++c.subCount;
        if(i==clavicleR||i==clavicleL)++c.clavicles;
    }
    c.ok=true;c.why="OK";return true;
}

// ============================== pure math ==============================
// the lock's soft knee (VrBody softLimit): linear to knee * max, then a tanh saturation to max
inline float SoftLimit(float len,float maxLen,float knee) noexcept {
    if(!(maxLen>0.f))return 0.f;const float k=std::clamp(knee,0.f,.99f)*maxLen;
    if(len<=k)return len;const float r=maxLen-k;return k+r*std::tanh((len-k)/r);
}
// The skeleton-root rotation with its pitch / roll removed: b = up, c = the root's c made horizontal (the heading GameXr's riding eye uses,
// -c; exact for any roll and up to ~78 deg of pitch, then the root's b turned onto up by the minimal rotation), a = b x c. upDot = cos(tilt)
// of the root's b.
inline A::Mat33 UprightFrame(const A::Mat34& root,A::Vec3 up,float& upDot) noexcept {
    const A::Vec3 rb=A::Normalize(root.b,up);upDot=A::Dot(rb,up);
    const A::Vec3 rc=A::Normalize(root.c,{0,0,1});A::Vec3 c=rc-up*A::Dot(rc,up);
    if(A::Length(c)<.2f){c=A::Rotate(A::FromTo(rb,up),rc);c=c-up*A::Dot(c,up);}
    c=A::Normalize(c,{0,0,1});return A::Mat33{A::Cross(up,c),up,c};
}
// The rider's forward as GameXr's riding eye reads it (-c, horizontal) - of the upright frame, so it stays valid through any tilt.
inline A::Vec3 RiderForward(const A::Mat33& F,A::Vec3 up) noexcept {A::Vec3 f=-F.c;f=f-up*A::Dot(f,up);return A::Normalize(f,{0,0,1});}
// The HMD heading: horizontal(forward), the fallback within 6 deg of straight up / down. (horizontal(forward + head up) with the
// head up made from world up flips 180 deg looking UP past 45 deg; this is the same direction everywhere else.)
inline A::Vec3 HeadHeading(A::Vec3 headFwd,A::Vec3 up,A::Vec3 fallback) noexcept {
    const A::Vec3 fw=A::Normalize(headFwd,fallback);const A::Vec3 h=fw-up*A::Dot(fw,up);
    return A::Length(h)<.1f?fallback:A::Normalize(h,fallback);
}
// the torso twist for a head yaw relative to the rider: soft-limited to maxTwist and HELD there up to 180 deg (a fade
// 100..170 deg un-twists the chest on a look back); past 150 deg the side of the last frame is kept (sideMem), so a head passing 180
// never flips it - it switches once the yaw is back under 150 deg on the other side.
inline float TwistFor(float yaw,float maxTwist,float& sideMem,bool& clamped) noexcept {
    if(!std::isfinite(yaw)){clamped=false;return 0.f;}
    const float a=std::fabs(yaw),sgn=a>150.f*A::kDegToRad&&sideMem!=0.f?sideMem:yaw<0.f?-1.f:1.f;
    sideMem=sgn;clamped=a>maxTwist;return sgn*SoftLimit(a,maxTwist,.8f);
}
// native inputs: bone[0]'s parent global, the chain locals (memory), the head joint in the neck frame (from the globals)
struct Pose {A::Mat34 parent{};std::array<A::Mat34,maxChain> local{};A::Vec3 headLocal{};};
struct Out {std::array<A::Mat34,maxChain> global{},local{};A::Vec3 head{};};
// Forward kinematics of the chain: bone[0] rotated to `base` (world), bone[k >= 1] to ref[k]; the twist (rad about up) and the lean
// (horizontal vector: direction the head moves, length rad) applied in world at every spine joint (share 1 / spineN each, the neck none);
// weight blends each local from the native one (0) to that pose (1). Translations stay native (the bones keep their lengths).
inline void Fk(const Chain& c,const Pose& p,const A::Mat33& base,const A::Quat* ref,A::Vec3 up,float twist,A::Vec3 lean,float weight,Out& o) noexcept {
    const float ll=A::Length(lean),share=c.spineN>0?1.f/float(c.spineN):0.f;const bool rotate=ll>1e-6f||std::fabs(twist)>1e-6f;
    const A::Vec3 axis=ll>1e-6f?A::Normalize(A::Cross(up,lean),{1,0,0}):A::Vec3{1,0,0};
    const A::Mat33 dm=A::FromQuat(A::AxisAngle(axis,ll*share)*A::AxisAngle(up,twist*share)); // twist first, then the lean
    A::Mat34 gp=p.parent;
    for(int k=0;k<c.n;++k){
        const A::Mat33 rp=A::Orthonormalize(gp.Rot()),rt=A::Transpose(rp);
        A::Mat33 lr=k==0?rt*base:A::FromQuat(ref[k]);
        if(rotate&&k<c.spineN)lr=rt*(dm*(rp*lr));
        lr=weight<1.f?A::FromQuat(A::Slerp(A::ToQuat(A::Orthonormalize(p.local[std::size_t(k)].Rot())),A::ToQuat(A::Orthonormalize(lr)),std::max(weight,0.f))):
                       A::Orthonormalize(lr);
        o.local[std::size_t(k)]=A::MakeMat34(lr,p.local[std::size_t(k)].d);o.global[std::size_t(k)]=gp*o.local[std::size_t(k)];gp=o.global[std::size_t(k)];
    }
    o.head=A::TransformPoint(gp,p.headLocal);
}
// The lean that puts the head joint horizontally on `target`: 3 Gauss-Seidel steps over the lever of the shared bend (mean height of the head
// above the spine joints), each limited to maxLean (soft knee 0.8). clamped = the wanted lean went past maxLean.
inline A::Vec3 SolveLean(const Chain& c,const Pose& p,const A::Mat33& base,const A::Quat* ref,A::Vec3 up,float twist,A::Vec3 target,float maxLean,
                         bool& clamped) noexcept {
    clamped=false;Out o{};Fk(c,p,base,ref,up,twist,{},1.f,o);
    float lever=0.f;for(int k=0;k<c.spineN;++k)lever+=A::Dot(o.head-o.global[std::size_t(k)].d,up);
    lever/=float(std::max(c.spineN,1));if(!(lever>1e-3f)||!(maxLean>0.f))return {};
    A::Vec3 lean{};
    for(int it=0;it<3;++it){
        A::Vec3 e=target-o.head;e=e-up*A::Dot(e,up);lean=lean+e*(1.f/lever);
        const float l=A::Length(lean);if(!std::isfinite(l))return {};
        if(l>maxLean)clamped=true;
        if(l>maxLean*.8f){lean=lean*(SoftLimit(l,maxLean,.8f)/l);}
        if(it<2)Fk(c,p,base,ref,up,twist,lean,1.f,o);
    }
    return lean;
}
inline float AngleDeg(const A::Mat33& a,const A::Mat33& b) noexcept {
    const float d=std::fabs(A::Dot(A::ToQuat(A::Orthonormalize(a)),A::ToQuat(A::Orthonormalize(b))));
    return 2.f*std::acos(std::clamp(d,0.f,1.f))/A::kDegToRad;
}

// ============================== state (one writer: the thread that runs the player's hook A, under VrBody busy) ==============================
struct State {
    std::uintptr_t data{};std::uint32_t generation{};float weight{};std::uint64_t lastWantNs{},lastHeadNs{},offNs{}; // offNs: first frame seen off since the last hold
    bool haveFollow{};float twist{},twistSide{};A::Vec3 lean{};                    // twist smoothed (rider-relative); lean = the last applied (report)
    bool haveAim{};A::Vec3 aimR{};float followK{1.f};                              // the smoothed neck-model target in the rider frame (F about the root)
    bool haveF{};A::Quat lastF{};                                                  // the last upright frame within the tilt limit (this mount)
    bool haveUse{};A::Quat useBase{};std::array<A::Quat,maxChain> useLocal{};      // the reference last applied (a ramp out without one keeps it)
    bool captured{},bindFallback{};A::Quat capBase{};std::array<A::Quat,maxChain> capLocal{};float calmS{},maxHead{-1e9f},speedS{},waitS{};
    bool haveRoot{};A::Vec3 prevRoot{};
    bool eyeLatched{};float eyeSum{},eyeS{},eyeRef{};                              // the reference head forward of the root (m), latched per mount
};
inline State state{};
struct Saved {bool valid{};std::array<A::Mat34,maxChain> local{};}; // the chain locals hook A wrote (hook B re-derives them under the live parent)
struct Stats {
    Counter frames{},wantedFrames{},holdFrames{},noHead{},rampOutFrames{},cuts{},mounts{},captures{},captureWait{},captureRejects{},captureTimeouts{},
        clampFrames{},leanClamped{},twistClamped{},tiltFrames{},lastRefFrames{},eyeLatches{},skipped{},driftFrames{},driftOver1cm{},reasserted{},dryFrames{};
    std::atomic<float> weight{},leanDeg{},twistDeg{},neckErrCm{},neckErrCmMax{},driftCmMax{},driftDegMax{},usMax{},tiltDeg{},tiltDegMax{};
    std::atomic<const char*> refSource{"NONE"},skipWhy{""};std::atomic<bool> active{},wanted{},refOk{};
    // rider frame (cm; forward = GameXr's -c, side = up x forward): the native head / spine base vs the rider root, the reference head, the
    // final head, the neck joint (the collapse stump) vs the VR eye (below along up, behind along the HMD heading)
    std::atomic<float> nativeFwd{},nativeSide{},nativeUp{},baseFwd{},baseSide{},refFwd{},refUp{},headFwd{},headSide{},headUp{},stumpBelow{},stumpBehind{};
};
inline Stats stats{};

// ============================== GameXr riding eye <-> seat (any thread) ==============================
inline std::atomic<std::uint64_t> activeNs{};                  // the last hook-A frame the hold wrote with weight > 0
inline std::atomic<float> eyeFwdM{.10f};inline std::atomic<bool> eyeFwdOk{}; // latched reference head forward of the rider root + BODY BACK (m)
inline std::atomic<std::uint64_t> eyeHoldSamples{};inline std::atomic<float> eyeFwdSeatUsed{-1.f}; // written by the riding eye
// True while the hold drives the rider's upper body (written < 250 ms ago): the riding eye must not calibrate from the head joint.
inline bool holding() noexcept {
    const auto t=activeNs.load(std::memory_order_acquire);if(!t)return false;const auto now=::rdrvr::vr::nowNs();
    return now>=t?now-t<250'000'000ull:t-now<250'000'000ull;
}
// The forward offset the riding eye should keep from the rider root while holding (.seat.eye 0 = none: a fixed seat).
inline bool eyeForward(float& out) noexcept {
    if(!config.eye||!eyeFwdOk.load(std::memory_order_acquire))return false;const float v=eyeFwdM.load(std::memory_order_relaxed);
    if(!std::isfinite(v))return false;out=v;return true;
}

// ============================== report "vr":{"body":{"seat":...}} (any thread) ==============================
inline std::string report(const Chain* c) {
    std::string out;out.reserve(2560);char b[2048];const auto& s=stats;const auto& k=config;
    const auto u=[](const Counter& x){return (unsigned long long)x.load(std::memory_order_relaxed);};
    const auto f=[](const std::atomic<float>& x){return double(x.load(std::memory_order_relaxed));};
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"config\":{\"ref\":\"%s\",\"follow\":%.2f,\"leanDeg\":%.0f,\"twistDeg\":%.0f,\"tauMs\":%.0f,\"rampMs\":%.0f,"
        "\"waitMs\":%.0f,\"reassert\":%s,\"eye\":%s},\"chain\":{\"ok\":%s,\"why\":\"%s\",\"bones\":[",k.on?"true":"false",k.capture?"CAPTURE":"BIND",double(k.follow),
        double(k.leanDeg),double(k.twistDeg),double(k.tauMs),double(k.rampS*1000.f),double(k.waitS*1000.f),k.reassert?"true":"false",k.eye?"true":"false",
        c&&c->ok?"true":"false",c?c->why:"NO_RIG");out+=b;
    if(c)for(int i=0;i<c->n;++i){std::snprintf(b,sizeof b,"%s%d",i?",":"",c->bone[std::size_t(i)]);out+=b;}
    std::snprintf(b,sizeof b,"],\"head\":%d,\"baseParent\":%d,\"spineBones\":%d,\"cut\":%d,\"pelvis\":%d,\"pelvisParent\":%d,\"subtree\":%d,\"clavicles\":%d,"
        "\"bindLenCm\":%.1f,\"bindSpine\":[%.2f,%.2f,%.2f],\"bindUpDot\":%.3f,\"bindPitchDeg\":%.1f,\"bindSideDeg\":%.1f,\"bindUpright\":%s},",
        c?c->head:-1,c?c->baseParent:-1,c?c->spineN:0,c?c->cut:0,c?c->pelvis:-1,c?c->pelvisParent:-3,c?c->subCount:0,c?c->clavicles:0,c?double(c->bindLenCm):0.0,
        c?double(c->bindSpine.x):0.0,c?double(c->bindSpine.y):0.0,c?double(c->bindSpine.z):0.0,c?double(c->bindUpDot):0.0,c?double(c->bindPitchDeg):0.0,
        c?double(c->bindSideDeg):0.0,c&&c->bindUpright?"true":"false");out+=b;
    std::snprintf(b,sizeof b,"\"active\":%s,\"wanted\":%s,\"weight\":%.2f,\"refSource\":\"%s\",\"refOk\":%s,\"leanDeg\":%.1f,\"twistDeg\":%.1f,"
        "\"neckErrCm\":%.1f,\"neckErrCmMax\":%.1f,\"clampFrames\":%llu,\"leanClamped\":%llu,\"twistClamped\":%llu,\"frames\":%llu,\"wantedFrames\":%llu,"
        "\"holdFrames\":%llu,\"noHead\":%llu,\"rampOutFrames\":%llu,\"lastRefFrames\":%llu,\"cuts\":%llu,\"mounts\":%llu,\"captures\":%llu,\"captureWait\":%llu,"
        "\"captureRejects\":%llu,\"captureTimeouts\":%llu,\"tiltFrames\":%llu,\"tiltDeg\":%.1f,\"tiltDegMax\":%.1f,\"dryFrames\":%llu,"
        "\"skipped\":%llu,\"skipWhy\":\"%s\",\"usMax\":%.1f,",
        s.active.load()?"true":"false",s.wanted.load()?"true":"false",f(s.weight),s.refSource.load(),s.refOk.load()?"true":"false",
        f(s.leanDeg),f(s.twistDeg),f(s.neckErrCm),f(s.neckErrCmMax),u(s.clampFrames),u(s.leanClamped),u(s.twistClamped),u(s.frames),u(s.wantedFrames),
        u(s.holdFrames),u(s.noHead),u(s.rampOutFrames),u(s.lastRefFrames),u(s.cuts),u(s.mounts),u(s.captures),u(s.captureWait),u(s.captureRejects),
        u(s.captureTimeouts),u(s.tiltFrames),f(s.tiltDeg),f(s.tiltDegMax),u(s.dryFrames),u(s.skipped),s.skipWhy.load(),f(s.usMax));out+=b;
    std::snprintf(b,sizeof b,"\"riderCm\":{\"nativeHead\":[%.1f,%.1f,%.1f],\"base\":[%.1f,%.1f],\"refHead\":[%.1f,%.1f],\"head\":[%.1f,%.1f,%.1f],"
        "\"stumpBelowEye\":%.1f,\"stumpBehindEye\":%.1f},\"drift\":{\"frames\":%llu,\"over1cm\":%llu,\"cmMax\":%.2f,\"degMax\":%.1f,\"reasserted\":%llu},"
        "\"eye\":{\"fwdM\":%.3f,\"ok\":%s,\"latches\":%llu,\"holding\":%s,\"holdSamples\":%llu,\"fwdSeatUsed\":%.3f}}",
        f(s.nativeFwd),f(s.nativeSide),f(s.nativeUp),f(s.baseFwd),f(s.baseSide),f(s.refFwd),f(s.refUp),f(s.headFwd),f(s.headSide),f(s.headUp),
        f(s.stumpBelow),f(s.stumpBehind),u(s.driftFrames),u(s.driftOver1cm),f(s.driftCmMax),f(s.driftDegMax),u(s.reasserted),
        double(eyeFwdM.load()),eyeFwdOk.load()?"true":"false",u(s.eyeLatches),holding()?"true":"false",(unsigned long long)eyeHoldSamples.load(),
        double(eyeFwdSeatUsed.load()));
    out+=b;return out;
}

} // namespace rdrvr::vr::seat
