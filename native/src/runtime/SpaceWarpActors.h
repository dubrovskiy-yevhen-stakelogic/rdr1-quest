#pragma once
// Per-actor rigid motion for the AppSW motion vectors (NPCs, animals, horses).
//
// The engine draws actors with closed shaders, so (like the Vice City port's per-atomic root motion) the motion of an
// actor is taken from its root matrix instead of per-vertex history:
//   * main thread (vr/VrMotionActors.h, mainthread module tick after the simulation step): the nearest actors' root
//     matrices (component -> matrix: basis a, b, c + position d, engine world, y up, d at the feet) -> actorChannel;
//   * render thread (GameXr spaceWarpFrame): the latest set is copied into the SpaceWarpFrame of the rendered pair;
//   * SpaceWarp::produce: actors present in both the previous and the current produced frame (same guid, a newer main
//     thread serial) get two matrices per eye - current engine NDC -> actor-relative world offset (membership: an upright
//     cylinder around the root) and current engine NDC -> previous clip (D = Mprev * inverse(Mcur) applied to the pixel's
//     world position, then the previous eye) - written into the slot's uniform block with vkCmdUpdateBuffer;
//   * spacewarp.frag: a pixel whose depth-reconstructed position lies inside an actor's cylinder takes that actor's
//     reprojection instead of the camera-only one. Limbs move with the root (no per-bone history).
// The block builder and a CPU reference of spacewarp.frag live here (the AppSW dump's per-actor trace), as do the
// AppSW dump request counters (EyeDump.h debug.rdrvr.eyedump / debug.rdrvr.spacewarp.dump /
// .depthdual -> GameStereoCapture::setWarpFrame -> SpaceWarp).
// PER-BONE parts for <= 8 near actors (SpaceWarpBones.h; ActorSet::bones, GpuActors::bone). An actor box with all corners
// behind the eye is skipped (ActorBehind); with some behind, its screen rect is clipped at the near plane (boxRect).
// Still / behind / off-screen are decided before any part math (boundBoneSteps / boundBoneBox); blocks with parts come
// first and shape.w = rank: a part of any actor beats every root cylinder (a horse or pedestrian
// listed first never hides a rider's / walker's parts).
#include "SpaceWarpMath.h"
#include "SpaceWarpBones.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <optional>

namespace rdrvr::runtime::spacewarp {
inline constexpr unsigned maxActors=24;
struct ActorPose {
    std::uint32_t guid{};std::int32_t race{-1};
    float m[12]{};                      // a(0..2) b(3..5) c(6..8) d(9..11): world = a*x + b*y + c*z + d (engine units)
    std::int32_t bones{-1};             // Index into the BoneSet of the same serial (-1: root only)
};
struct ActorSet {
    std::uint64_t serial{};             // main-thread tick that read the matrices (0 = none)
    unsigned count{};
    std::array<ActorPose,maxActors> actor{};
    unsigned boneCount{},boneMode{};    // Actors with parts in the BoneSet of this serial; mode = debug.rdrvr.mvbones (0 off, 1 on, 2 pin)
};
// The parts of one main-thread tick. NOT part of ActorSet: an ActorSet travels BY VALUE through the render path (SpaceWarpFrame
// -> GameStereoCapture slot -> SpaceWarpSource on the stack); 8 x 1.3 KB of parts would add ~10 KB to each of those copies on threads
// whose stack size the engine chose. The channel keeps the last boneRing BoneSets by serial; SpaceWarp::produce copies the two it
// needs (cur / prev serial) into heap scratch.
struct BoneSet {std::uint64_t serial{};unsigned count{};std::array<ActorBones,maxBoneActors> bones{};};
inline constexpr unsigned boneRing=16; // ~440 ms of main-thread ticks: a produced pair is never older than a few frames
class ActorChannel {
public:
    void publish(const ActorSet& s) noexcept {std::lock_guard lock(mutex_);set_=s;}
    // the set and its parts (bones may be nullptr / empty: only the serial is recorded, nothing copied)
    void publish(const ActorSet& s,const BoneSet* b) noexcept {std::lock_guard lock(mutex_);set_=s;storeBones(s.serial,b);}
    void publishBones(const BoneSet& b) noexcept {std::lock_guard lock(mutex_);storeBones(b.serial,&b);}
    // the parts of `serial` (false: none recorded / overwritten); copies only the used entries
    bool bones(std::uint64_t serial,BoneSet& out) const noexcept {
        std::lock_guard lock(mutex_);out.serial=serial;out.count=0;if(!serial)return false;
        const auto& r=ring_[serial%boneRing];if(r.serial!=serial)return false;
        out.count=std::min(r.count,maxBoneActors);for(unsigned i=0;i<out.count;++i)out.bones[i]=r.bones[i];return true;}
    void latest(ActorSet& out) const noexcept {std::lock_guard lock(mutex_);out=set_;}
    void clear() noexcept {std::lock_guard lock(mutex_);set_=ActorSet{};for(auto& r:ring_){r.serial=0;r.count=0;}}
    // render thread (spaceWarpFrame): the left eye position of the newest pair, for the scan's distance order
    // (only while SPACEWARP produces pairs: the scan idles when no pair asked for actors within maxIdleNs).
    void setCamera(float x,float y,float z) noexcept {cam_[0].store(x,std::memory_order_relaxed);cam_[1].store(y,std::memory_order_relaxed);
        cam_[2].store(z,std::memory_order_relaxed);camNs_.store(nowNs(),std::memory_order_release);}
    bool camera(float out[3],std::uint64_t now) const noexcept {const auto t=camNs_.load(std::memory_order_acquire);if(!t||now<t||now-t>maxIdleNs)return false;
        for(int i=0;i<3;++i)out[i]=cam_[i].load(std::memory_order_relaxed);return true;}
    static constexpr std::uint64_t maxIdleNs=500'000'000ull;
    static std::uint64_t nowNs() noexcept {return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
private:
    void storeBones(std::uint64_t serial,const BoneSet* b) noexcept {
        if(!serial)return;auto& r=ring_[serial%boneRing];r.serial=serial;r.count=b?std::min(b->count,maxBoneActors):0u;
        for(unsigned i=0;i<r.count;++i)r.bones[i]=b->bones[i];}
    mutable std::mutex mutex_;ActorSet set_{};std::array<BoneSet,boneRing> ring_{};
    std::array<std::atomic<float>,3> cam_{};std::atomic<std::uint64_t> camNs_{};
};
inline ActorChannel actorChannel;
// Shapes (engine units = metres), root at the feet: upright cylinder radius / bottom / top above the root.
// race 0 human, 1 horse (VrMelee / VrLasso sizesOf), anything else a generic animal.
struct ActorShape {float radius,bottom,top;};
inline ActorShape actorShape(std::int32_t race) noexcept {
    if(race==-2)return ActorShape{0.75f,0.15f,2.00f}; // The PLAYER (synthetic, SpaceWarp::buildActors): arms' reach, ground excluded
    return race==0?ActorShape{0.55f,0.10f,2.05f}:race==1?ActorShape{1.50f,0.12f,2.85f}:ActorShape{1.00f,0.06f,1.70f};
}
// Uniform block of the motion-vector pass (std140), one per eye at eyeStride. rect = engine-NDC
// xmin, ymin, xmax, ymax; shape = r^2, bottom, top, 0 of the membership cylinder (with parts: the
// cylinder that bounds them); bones = first part, part count, root cylinder r^2, root cylinder top
// (the cylinder for pixels in no part).
// shape.w = the actor's RANK (index in the ActorSet, nearest first): blocks with parts come first (buildActorBlock), a part of
// ANY actor beats every root cylinder, and among root cylinders the lowest rank wins.
// maxGpuActors blocks per eye (device maxPerEye 4-6): the whole block stays within 16384 B, the Vulkan
// minimum of maxUniformBufferRange (Adreno reports 65536; the bound is for any device).
inline constexpr unsigned maxGpuActors=22;
struct GpuActor {float toLocal[16];float toPrev[16];float rect[4];float shape[4];float bones[4];};
struct GpuActors {float header[4];GpuActor actor[maxGpuActors];GpuBone bone[maxGpuBones];}; // header: actors, parts
static_assert(sizeof(GpuActor)==176&&sizeof(GpuActors)==16+176*maxGpuActors+96*maxGpuBones);
inline constexpr std::uint32_t gpuBoneOffset=16+176*maxGpuActors; // offsetof(GpuActors, bone) (std140: 16-aligned)
inline constexpr std::uint32_t gpuEyeStride=0x4000,gpuSlotStride=2*gpuEyeStride;
static_assert(sizeof(GpuActors)<=16384&&sizeof(GpuActors)<=gpuEyeStride&&gpuEyeStride%256==0&&gpuBoneOffset%16==0&&2*gpuEyeStride<=65536);

// ----------------------------------------------------------------------------------------------------
// AppSW dump requests (one-shot, see SpaceWarp.h SpaceWarpDump). requests: bumped by EyeDump::record (debug.rdrvr.eyedump) and by a
// new value of debug.rdrvr.spacewarp.dump; dual: a new value of debug.rdrvr.spacewarp.depthdual (both depth paths of one pair).
// The capture side claims a request for the pair it is recording (GameStereoCapture::setWarpFrame / recordDepth): taken = the last
// request claimed. Zero cost when nothing is requested (two relaxed loads per pair).
inline std::atomic<std::uint64_t> dumpRequests{},dumpTaken{},dualRequests{},dualTaken{};
inline std::uint64_t claim(std::atomic<std::uint64_t>& requests,std::atomic<std::uint64_t>& taken) noexcept {
    const auto r=requests.load(std::memory_order_relaxed);auto t=taken.load(std::memory_order_relaxed);
    while(r!=t){if(taken.compare_exchange_weak(t,r,std::memory_order_relaxed))return r;}
    return 0;
}
// Per actor of one eye's block: what buildActorBlock did with it (dump JSON, host tests).
enum ActorResult : std::uint8_t {ActorWritten,ActorUnmatched,ActorJump,ActorStill,ActorOffscreen,ActorMath,ActorOverflow,ActorBehind,ActorResultCount};
inline const char* actorResultName(unsigned r) noexcept {
    static constexpr const char* names[ActorResultCount]{"written","unmatched","jump","still","offscreen","math","overflow","behind"};
    return r<ActorResultCount?names[r]:"?";}
// ActorTrace: bones = parts written for this actor (0 root only), boneStepM = the largest part step of the pair
struct ActorTrace {std::uint32_t guid{};std::int32_t race{-1};std::uint8_t result{ActorUnmatched};int block{-1};float stepM{},angleDeg{};int bones{};float boneStepM{};};
// behind = boxes entirely behind the eye, clipped = boxes cut by the near plane (rect clipped),
// stillPinned = standing actors written anyway (debug.rdrvr.mvbones 2), bones = the part counters (left eye).
struct ActorStats {std::uint64_t unmatched{},jump{},still{},offscreen{},math{},behind{},clipped{},stillPinned{};float maxStepM{};BoneStats bones{};};
// The screen rectangle (engine NDC x, y) of a box given relative to the eye (lo / hi = corner - eye position), clipped at the
// near plane clip w >= wMin (w = view depth: every visible surface lies beyond the 5 cm near plane, so 1 cm keeps every claimable
// pixel). The clipped box's vertices are the corners in front + the 12 edges' crossings of w = wMin; projected, they bound the visible
// part. Returns BoxBehind (all corners behind: nothing on screen), BoxFront or BoxClipped; rect padded by 0.02, clamped to +-4.
enum BoxVisibility : int {BoxBehind,BoxFront,BoxClipped};
inline int boxRect(const M4& rel,const double lo[3],const double hi[3],double wMin,float rect[4]) noexcept {
    double q[8][4]{};unsigned front=0;bool finite=true;
    for(int k=0;k<8;++k){const double p[4]{(k&1)?hi[0]:lo[0],(k&2)?hi[1]:lo[1],(k&4)?hi[2]:lo[2],1};
        for(int r=0;r<4;++r){double s=0;for(int c=0;c<4;++c)s+=rel[c*4+r]*p[c];q[k][r]=s;finite=finite&&std::isfinite(s);}
        front+=q[k][3]>=wMin?1u:0u;}
    if(!finite){rect[0]=rect[1]=-4.f;rect[2]=rect[3]=4.f;return BoxClipped;} // non-finite: the conservative clipped rect
    if(!front)return BoxBehind;
    double r0=1e30,r1=1e30,r2=-1e30,r3=-1e30;
    const auto add=[&](double x,double y,double w){x/=w;y/=w;r0=std::min(r0,x);r1=std::min(r1,y);r2=std::max(r2,x);r3=std::max(r3,y);};
    for(int k=0;k<8;++k)if(q[k][3]>=wMin)add(q[k][0],q[k][1],q[k][3]);
    for(int k=0;k<8;++k)for(int bit=1;bit<8;bit<<=1){const int j=k|bit;if(j==k)continue; // the 12 edges (k, k + one bit)
        const bool fk=q[k][3]>=wMin,fj=q[j][3]>=wMin;if(fk==fj)continue;
        const double t=(wMin-q[k][3])/(q[j][3]-q[k][3]);add(q[k][0]+t*(q[j][0]-q[k][0]),q[k][1]+t*(q[j][1]-q[k][1]),wMin);}
    const double pad=0.02;
    rect[0]=float(std::clamp(r0-pad,-4.0,4.0));rect[1]=float(std::clamp(r1-pad,-4.0,4.0));rect[2]=float(std::clamp(r2+pad,-4.0,4.0));rect[3]=float(std::clamp(r3+pad,-4.0,4.0));
    return front==8?BoxFront:BoxClipped;
}
// Eye inputs of buildActorBlock (the SpaceWarpFrame fields it reads): clip = rel * (p - pos), engine units.
struct ActorEye {const float* rel{};const float* pos{};const ActorSet* actors{};const BoneSet* bones{};}; // + the parts of actors->serial
// The actor blocks of one eye. Matching by guid; still / jumped / off-screen actors keep the camera
// vectors. countSkips: count unmatched / jump / still / math (the left eye only); offscreen is counted
// for both eyes. trace (optional, maxActors entries): per current actor (index in cur.actors) what
// happened. Returns the number of blocks written. padSteps = the membership cylinder widening in root
// steps of this pair: 0.5 by default, 1.0 with debug.rdrvr.spacewarp.actorpad 1 (covers the
// extremities of RUNNING actors when the snapshot lags the drawn frame by a tick).
// cache (optional) = the eye-independent half of the parts, computed for the first eye of a pair and reused for the second
// (SpaceWarp::produce passes one; host tests may not: then each eye computes it, same result).
inline unsigned buildActorBlock(const ActorEye& cur,const ActorEye& prev,double unitsPerMetre,bool countSkips,GpuActors& out,ActorStats& st,
                                ActorTrace* trace=nullptr,double padSteps=0.5,BonePairCache* cache=nullptr) noexcept {
    if(!cur.rel||!cur.pos||!prev.rel||!prev.pos||!cur.actors||!prev.actors)return 0;
    const ActorSet& ca=*cur.actors;const ActorSet& pa=*prev.actors;
    if(!ca.count||!pa.count||ca.serial==pa.serial)return 0;
    M4 invRel{};if(!invert(fromFloat(cur.rel),invRel)){++st.math;return 0;}
    const float* cc=cur.pos;const float* cp=prev.pos;
    const M4 relCur=fromFloat(cur.rel),relPrev=fromFloat(prev.rel);
    const M4 base=mul(translation(cc[0],cc[1],cc[2]),invRel);                                  // engine NDC -> world (homogeneous)
    const M4 prevView=mul(relPrev,translation(-double(cp[0]),-double(cp[1]),-double(cp[2]))); // world -> previous clip
    const double upm=unitsPerMetre>0?unitsPerMetre:1.0;
    const auto matrix=[](const ActorPose& a){M4 m=identity();
        for(int c=0;c<4;++c)for(int r=0;r<3;++r)m[c*4+r]=a.m[c*3+r];return m;};
    unsigned n=0,nb=0;const bool pin=ca.boneMode==2,tight=ca.boneMode==3; // nb = parts written (out.bone), pin = standing actors with parts are written; tight = mode 3
    std::optional<PreparedParts> local;              // only without a cache (host tests); SpaceWarp::produce passes one
    for(unsigned i=0;i<ca.count&&i<maxActors;++i){
        const auto& a=ca.actor[i];const ActorPose* b=nullptr;
        ActorTrace t{};t.guid=a.guid;t.race=a.race;
        const auto done=[&](ActorResult r){t.result=r;if(trace)trace[i]=t;};
        if(n>=maxGpuActors){done(ActorOverflow);continue;}
        for(unsigned j=0;j<pa.count&&j<maxActors;++j)if(pa.actor[j].guid==a.guid){b=&pa.actor[j];break;}
        if(!b){if(countSkips)++st.unmatched;done(ActorUnmatched);continue;}
        const double dx=double(a.m[9])-b->m[9],dy=double(a.m[10])-b->m[10],dz=double(a.m[11])-b->m[11];
        const double step=std::sqrt(dx*dx+dy*dy+dz*dz)/upm;t.stepM=float(step);
        if(!(step<=1.5)){if(countSkips)++st.jump;done(ActorJump);continue;}                    // spawn / teleport / slot reuse
        const M4 mc=matrix(a),mp=matrix(*b);M4 inv{};
        if(!invert(mc,inv)){if(countSkips)++st.math;done(ActorMath);continue;}
        const M4 d=mul(mp,inv);                                                                    // current world -> previous world
        const double tr=d[0]+d[5]+d[10],cosA=std::clamp((tr-1.0)/2.0,-1.0,1.0);
        t.angleDeg=float(std::acos(cosA)*57.29577951308232);
        const double mx=(double(a.m[9])+b->m[9])/2,my=(double(a.m[10])+b->m[10])/2,mz=(double(a.m[11])+b->m[11])/2;
        // the actor's parts (both snapshots carry bones with the same layout), into the eye's part array after the written ones
        const BoneSet* cs=cur.bones&&cur.bones->serial==ca.serial?cur.bones:nullptr;const BoneSet* ps=prev.bones&&prev.bones->serial==pa.serial?prev.bones:nullptr;
        const ActorBones* cb=cs&&a.bones>=0&&unsigned(a.bones)<cs->count&&unsigned(a.bones)<maxBoneActors&&cs->bones[unsigned(a.bones)].guid==a.guid?&cs->bones[unsigned(a.bones)]:nullptr;
        const ActorBones* pb=ps&&b->bones>=0&&unsigned(b->bones)<ps->count&&unsigned(b->bones)<maxBoneActors&&ps->bones[unsigned(b->bones)].guid==a.guid?&ps->bones[unsigned(b->bones)]:nullptr;
        const bool withParts=cb&&pb,rootStill=step<0.002&&cosA>0.999994;const double mid[3]{mx,my,mz}; // < 2 mm and < 0.2 deg
        // still / behind / off-screen are decided on a CHEAP bound (SpaceWarpBones.h boundBoneSteps / boundBoneBox, no inverse,
        // once per pair in the cache entry) before any part math, so no parts are prepared, emitted and thrown
        // away for the still / behind ones.
        PreparedParts* pp=withParts?(cache?&cache->entry[unsigned(a.bones)]:&local.emplace()):nullptr; // a.bones < maxBoneActors (checked above)
        bool bounded=false;
        if(pp){if(!(pp->bGuid==a.guid&&pp->bCur==ca.serial&&pp->bPrev==pa.serial)){pp->bounded=boundBoneSteps(*cb,*pb,pp->bound);pp->bGuid=a.guid;pp->bCur=ca.serial;pp->bPrev=pa.serial;}
            bounded=pp->bounded;}
        if(rootStill&&(!withParts||(!pin&&bounded&&pp->bound.step<0.002*upm))){                 // bound.step >= every exact part step
            t.boneStepM=pp?float(pp->bound.step/upm):0.f;if(countSkips)++st.still;done(ActorStill);continue;}
        // membership cylinder centred between the two roots, widened by padSteps root steps (half a step by
        // default; a one-tick snapshot lag leaves the drawn root 1.5 steps from the centre - a walking torso stays inside, only
        // extremities > 0.55 m - 1.0 step from the axis of running actors fall out).
        const auto shape=actorShape(a.race);const double widen=padSteps>0?padSteps:0.0;
        const double radius=shape.radius*upm+widen*std::sqrt(dx*dx+dz*dz),bottom=shape.bottom*upm-widen*std::fabs(dy),top=shape.top*upm+widen*std::fabs(dy);
        // screen rectangle (engine NDC) of the cylinder's box, clipped at the near plane
        float rect[4]{};int vis=BoxFront;
        const auto box=[&](double R,double T){const double lo[3]{mx-R-cc[0],my+bottom-cc[1],mz-R-cc[2]},hi[3]{mx+R-cc[0],my+T-cc[1],mz+R-cc[2]};
            return boxRect(relCur,lo,hi,0.01*upm,rect);};
        const auto offscreen=[&]{return rect[0]>1.f||rect[1]>1.f||rect[2]<-1.f||rect[3]<-1.f;};
        if(!withParts||bounded){ // the root cylinder + every prepared capsule (bound): no part math for an actor behind / off-screen
            if(bounded&&!pp->bound.box)boundBoneBox(*cb,mid,padSteps,pp->bound);
            vis=box(std::max(radius,bounded?pp->bound.radius:0.0),bounded?std::max(top,pp->bound.top):top);
            if(vis==BoxBehind){++st.behind;done(ActorBehind);continue;}
            if(offscreen()){++st.offscreen;done(ActorOffscreen);continue;}}
        bool valid=false;double boundR=0,boundTop=0,boneStep=0;
        if(pp){BoneStats bs{};
            if(!(pp->guid==a.guid&&pp->cur==ca.serial&&pp->prev==pa.serial)){                    // first eye of this pair that needs it
                prepareBoneParts(*cb,*pb,mid,padSteps,upm,*pp,bs,tight);pp->guid=a.guid;pp->cur=ca.serial;pp->prev=pa.serial;}
            valid=pp->valid;if(valid){boundR=pp->boundR;boundTop=pp->boundTop;boneStep=pp->maxStep;}
            if(countSkips){st.bones.actors+=bs.actors;st.bones.parts+=bs.parts;st.bones.unmatched+=bs.unmatched;st.bones.jump+=bs.jump;
                st.bones.math+=bs.math;st.bones.maxStepM=std::max(st.bones.maxStepM,bs.maxStepM);}
            t.boneStepM=float(boneStep/upm);}
        if(rootStill&&(!valid||boneStep<0.002*upm)){                                               // The exact decision
            if(!(pin&&valid)){if(countSkips)++st.still;done(ActorStill);continue;}
            if(countSkips)++st.stillPinned;}                                                       // PIN: written anyway
        if(countSkips)st.maxStepM=std::max(st.maxStepM,float(step));
        const double boxR=std::max(radius,boundR),boxTop=valid?std::max(top,boundTop):top;        // The cylinder bounding the parts
        if(withParts){vis=box(boxR,boxTop);                                                         // the exact box (tighter than the bound)
            if(vis==BoxBehind){++st.behind;done(ActorBehind);continue;}
            if(offscreen()){++st.offscreen;done(ActorOffscreen);continue;}}
        if(vis==BoxClipped)++st.clipped;
        unsigned parts=0;
        if(valid){parts=emitBoneParts(*pp,base,prevView,out.bone+nb,maxGpuBones-nb);if(!parts&&countSkips)++st.bones.overflow;} // only for a written block
        const M4 toLocal=mul(translation(-mx,-my,-mz),base);
        const M4 toPrev=mul(prevView,mul(d,base));
        auto& g=out.actor[n];bool finite=true;
        for(int k=0;k<16;++k){g.toLocal[k]=float(toLocal[k]);g.toPrev[k]=float(toPrev[k]);finite=finite&&std::isfinite(g.toLocal[k])&&std::isfinite(g.toPrev[k]);}
        if(!finite){if(countSkips)++st.math;done(ActorMath);continue;}
        std::memcpy(g.rect,rect,sizeof rect);g.shape[0]=float(boxR*boxR);g.shape[1]=float(bottom);g.shape[2]=float(boxTop);g.shape[3]=float(i);
        g.bones[0]=float(nb);g.bones[1]=float(parts);g.bones[2]=float(radius*radius);g.bones[3]=float(top);
        t.block=int(n);t.bones=int(parts);done(ActorWritten);
        nb+=parts;++n;
    }
    // Blocks WITH parts first (stable), then the root-only ones. The pass takes the first part containing a pixel over every
    // root cylinder (and the lowest-rank root cylinder otherwise), and stops at the first root-only cylinder: a horse (race 1, r 1.5 m)
    // or a pedestrian listed before a rider / walker with parts never hides that actor's parts. std::rotate: in place, no allocation.
    std::array<std::uint8_t,maxGpuActors> where{};unsigned np=0;
    for(unsigned k=0;k<n;++k)if(out.actor[k].bones[1]>0.5f)where[k]=std::uint8_t(np++);
    for(unsigned k=0,r=np;k<n;++k)if(!(out.actor[k].bones[1]>0.5f))where[k]=std::uint8_t(r++);
    for(unsigned k=0,w=0;k<n;++k)if(out.actor[k].bones[1]>0.5f){if(k!=w)std::rotate(out.actor+w,out.actor+k,out.actor+k+1);++w;}
    if(trace)for(unsigned i=0;i<ca.count&&i<maxActors;++i)if(trace[i].result==ActorWritten&&trace[i].block>=0&&unsigned(trace[i].block)<n)trace[i].block=where[unsigned(trace[i].block)];
    out.header[0]=float(n);out.header[1]=float(nb);
    return n;
}
// CPU reference of shaders/spacewarp.frag (camera reprojection + the eye's actor blocks), float math in the shader's order.
// u,v in [0,1] (v = 0 at the TOP image row), depth = the sampled slot depth. claimed = index of the block that took the pixel (-1:
// camera vectors). Returns the RGB the pass writes (A = 0).
// claimedPart = index in block->bone of the part that took the pixel (-1: root or camera); partMargin = the decision margin of
// the part choice (for comparing GPU and CPU tie breaks).
inline std::array<float,3> shaderMotion(const std::array<float,16>& reproject,const GpuActors* block,float u,float v,float depth,const Conventions& c,
                                        int* claimed=nullptr,int* claimedPart=nullptr,float* partMargin=nullptr) noexcept {
    const auto apply=[](const float* m,const float x[4],float o[4]){for(int r=0;r<4;++r){float s=0;for(int k=0;k<4;++k)s+=m[k*4+r]*x[k];o[r]=s;}};
    const float cur[4]{u*2-1,(v*2-1)*c.engineY,depth,1};
    float prev[4]{};apply(reproject.data(),cur,prev);
    if(claimed)*claimed=-1;
    if(claimedPart)*claimedPart=-1;
    if(partMargin)*partMargin=1e30f;
    const int n=block&&depth<1.f?std::min(int(block->header[0]),int(maxGpuActors)):0;
    int root=-1;float rank=1e30f; // The root cylinder that takes the pixel when no part of any actor does (lowest rank)
    for(int i=0;i<n;++i){
        const auto& a=block->actor[i];const float* r=a.rect;
        if(cur[0]<r[0]||cur[1]<r[1]||cur[0]>r[2]||cur[1]>r[3])continue;
        float l[4]{};apply(a.toLocal,cur,l);
        if(l[3]<=1e-9f)continue;
        const float px=l[0]/l[3],py=l[1]/l[3],pz=l[2]/l[3];
        if(py<a.shape[1]||py>a.shape[2]||px*px+pz*pz>a.shape[0])continue;
        // the nearest part containing the point, else the root cylinder
        const int first=std::clamp(int(a.bones[0]),0,int(maxGpuBones)),cnt=std::clamp(int(a.bones[1]),0,int(maxGpuBones)-first);
        const float p[3]{px,py,pz};float margin=1e30f;
        const int k=cnt>0?nearestPart(block->bone+first,cnt,p,&margin):-1;
        if(partMargin)*partMargin=std::min(*partMargin,margin);
        if(k>=0){apply(block->bone[first+k].toPrev,cur,prev);if(claimed)*claimed=i;if(claimedPart)*claimedPart=first+k;root=-1;break;}
        if(py>a.bones[3]||px*px+pz*pz>a.bones[2])continue;
        if(a.shape[3]<rank){rank=a.shape[3];root=i;}
        if(cnt==0)break; // root-only blocks follow every block with parts (buildActorBlock): no part can come
    }
    if(root>=0){apply(block->actor[root].toPrev,cur,prev);if(claimed)*claimed=root;}
    std::array<float,3> d{0,0,0};
    if(prev[3]>1e-6f){
        d={cur[0]-prev[0]/prev[3],cur[1]-prev[1]/prev[3],cur[2]-prev[2]/prev[3]};
        d[1]*=c.engineY;for(float& x:d)x*=c.mvSign;d[1]*=c.mvY;d[2]*=c.mvZ;
    }
    return d;
}
} // namespace rdrvr::runtime::spacewarp
