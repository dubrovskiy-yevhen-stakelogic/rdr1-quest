#pragma once
// PER-BONE motion vectors for the nearest moving creatures (AppSW; extends the per-actor root motion of
// SpaceWarpActors.h). A root transform alone leaves the legs, arms and head of a walking NPC at the app's
// render rate under AppSW, so the parts get their own motion.
// How (the skinning is closed engine code, so - like the root path - motion comes from matrices,
// not from vertex history):
//   * main thread (vr/VrMotionBones.h, inside the mvactors tick): for <= maxBoneActors (8) of the published actors within
//     boneRangeM (25 m), moving ones first, the world matrices of ~19 named joints of the actor's crSkeleton (skeleton globals,
//     stride 0x40, world space - VrMelee / VrBody [V]) in ONE guarded vector read together with the roots; makeParts() turns them into
//     <= maxActorBones (16) rigid PARTS: a capsule (segment joint -> child joint or tip, radius per bone class) + the matrix of the
//     bone that carries it (ActorBones, published with the ActorSet);
//   * SpaceWarp::produce -> buildActorBlock -> buildBoneParts(): per part of an actor present in both snapshots with the same layout:
//     D = Gprev * inverse(Gcur) (current world -> previous world of a point riding on that bone), the capsule centred between its
//     two poses and widened by padSteps of its step (like the root cylinder), toPrev = prevView * D * base (engine NDC -> previous
//     clip) -> GpuBone (96 B, std140) after the actor array of the eye's uniform block;
//   * spacewarp.frag: inside an actor's rect and bounding cylinder, the NEAREST part axis whose capsule contains the pixel's
//     depth-reconstructed position gives the reprojection; no part -> the root cylinder -> else camera vectors.
//     A part of ANY actor wins over every root cylinder (blocks with parts first; root cylinders by rank).
// Debug: debug.rdrvr.mvbones 0 = off (root motion only), 1 = on,
// 3 = on with TIGHT capsules (prepareBoneParts), 2 = PIN (bone blocks also for standing actors - every
// near NPC shows its capsules in an AppSW eye dump).
#include "SpaceWarpMath.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rdrvr::runtime::spacewarp {
inline constexpr unsigned maxBoneActors=8,maxActorBones=16,maxGpuBones=maxBoneActors*maxActorBones;
inline constexpr float boneRangeM=25.f;                   // main thread: only actors this close get bones
enum BoneKey : std::uint8_t {KeyHead,KeyNeck,KeySpine1,KeySpine3,KeyPelvis,KeyArmL,KeyElbowL,KeyWristL,KeyArmR,KeyElbowR,KeyWristR,
    KeyHipL,KeyKneeL,KeyAnkleL,KeyToeL,KeyHipR,KeyKneeR,KeyAnkleR,KeyToeR,KeyCount};
inline constexpr std::array<std::array<const char*,3>,KeyCount> boneKeyAliases{{ // exact names (case-insensitive), priority order
    {"head","head01",nullptr},{"neck","neck01","neck00"},{"spine01","spine1","spine02"},{"spine03","spine3","spine02"},{"pelvis","pelvis01","hips"},
    {"arm_l","upperarm_l",nullptr},{"elbow_l","forearm_l",nullptr},{"wrist_l","hand_l",nullptr},
    {"arm_r","upperarm_r",nullptr},{"elbow_r","forearm_r",nullptr},{"wrist_r","hand_r",nullptr},
    {"hip_l","thigh_l",nullptr},{"knee_l","calf_l",nullptr},{"ankle_l","foot_l",nullptr},{"toe_l","ball_l",nullptr},
    {"hip_r","thigh_r",nullptr},{"knee_r","calf_r",nullptr},{"ankle_r","foot_r",nullptr},{"toe_r","ball_r",nullptr}}};
inline const char* boneKeyName(unsigned k) noexcept {return k<KeyCount?boneKeyAliases[k][0]:"?";}
inline char boneLower(char c) noexcept {return c>='A'&&c<='Z'?char(c-'A'+'a'):c;}
// Rank of `name` for key k: 0..2 = alias index, -1 = no match (exact only: "hip_l_Attachment" is NOT hip_l).
inline int boneKeyRank(const char* name,unsigned k) noexcept {
    if(!name||k>=KeyCount)return -1;
    for(int i=0;i<3;++i){const char* a=boneKeyAliases[k][std::size_t(i)];if(!a)continue;
        std::size_t n=0;while(a[n]&&name[n]&&boneLower(name[n])==a[n])++n;
        if(!a[n]&&!name[n])return i;}
    return -1;
}
// key -> bone index (-1 missing) from the skeleton's bone names (names[i] may be nullptr). Each key takes its best-ranked bone.
struct BoneKeyMap {std::array<std::int16_t,KeyCount> index{};
    BoneKeyMap(){index.fill(-1);}
    bool has(unsigned k) const noexcept {return k<KeyCount&&index[k]>=0;}
    // humanoid enough for parts: pelvis, the spine top, both arms and legs down to wrist / ankle, and the head
    bool humanoid() const noexcept {
        for(unsigned k:{KeyHead,KeySpine3,KeyPelvis,KeyArmL,KeyElbowL,KeyWristL,KeyArmR,KeyElbowR,KeyWristR,KeyHipL,KeyKneeL,KeyAnkleL,KeyHipR,KeyKneeR,KeyAnkleR})
            if(!has(k))return false;
        return true;}
};
inline BoneKeyMap resolveBoneKeys(const char* const* names,unsigned count) noexcept {
    BoneKeyMap m{};std::array<int,KeyCount> best{};best.fill(99);
    for(unsigned i=0;i<count&&i<32767;++i)for(unsigned k=0;k<KeyCount;++k){const int r=boneKeyRank(names[i],k);if(r>=0&&r<best[k]){best[k]=r;m.index[k]=std::int16_t(i);}}
    return m;
}
enum BoneClass : std::uint8_t {ClassHead,ClassChest,ClassTorso,ClassPelvis,ClassUpperArm,ClassForearm,ClassHand,ClassThigh,ClassShin,ClassFoot,ClassCount};
inline const char* boneClassName(unsigned c) noexcept {
    static constexpr const char* n[ClassCount]{"head","chest","torso","pelvis","upperArm","forearm","hand","thigh","shin","foot"};return c<ClassCount?n[c]:"?";}
// A part: segment a -> b (b = KeyCount: the tip a + extend * unit(a - from)), carried by `motion` (missing: by a), radius in metres.
struct PartSpec {BoneKey a,b,from,motion;float radius,extend;BoneClass cls;};
inline constexpr std::array<PartSpec,maxActorBones> partSpecs{{
    {KeyHead,KeyCount,KeyNeck,KeyHead,.13f,.16f,ClassHead},         // skull + face above the head joint
    {KeySpine3,KeyNeck,KeyCount,KeySpine3,.18f,0,ClassChest},
    {KeyPelvis,KeySpine3,KeyCount,KeySpine1,.17f,0,ClassTorso},
    {KeyHipL,KeyHipR,KeyCount,KeyPelvis,.15f,0,ClassPelvis},
    {KeyArmL,KeyElbowL,KeyCount,KeyArmL,.075f,0,ClassUpperArm},{KeyElbowL,KeyWristL,KeyCount,KeyElbowL,.065f,0,ClassForearm},
    {KeyWristL,KeyCount,KeyElbowL,KeyWristL,.075f,.10f,ClassHand},
    {KeyArmR,KeyElbowR,KeyCount,KeyArmR,.075f,0,ClassUpperArm},{KeyElbowR,KeyWristR,KeyCount,KeyElbowR,.065f,0,ClassForearm},
    {KeyWristR,KeyCount,KeyElbowR,KeyWristR,.075f,.10f,ClassHand},
    {KeyHipL,KeyKneeL,KeyCount,KeyHipL,.10f,0,ClassThigh},{KeyKneeL,KeyAnkleL,KeyCount,KeyKneeL,.08f,0,ClassShin},
    {KeyAnkleL,KeyToeL,KeyCount,KeyAnkleL,.07f,0,ClassFoot},
    {KeyHipR,KeyKneeR,KeyCount,KeyHipR,.10f,0,ClassThigh},{KeyKneeR,KeyAnkleR,KeyCount,KeyKneeR,.08f,0,ClassShin},
    {KeyAnkleR,KeyToeR,KeyCount,KeyAnkleR,.07f,0,ClassFoot}}};
// One rigid part of a skinned creature (main-thread snapshot, engine world units).
struct BonePart {float a[3]{},b[3]{};float radius{};std::uint8_t cls{},spec{};std::uint16_t bone{};float m[12]{};}; // m: rows a, b, c, d (ActorPose)
struct ActorBones {std::uint32_t guid{},layout{},count{};std::array<BonePart,maxActorBones> part{};};
// The parts of one actor from its joint matrices (m[k] = 12 floats rows a,b,c,d, or nullptr), bones[k] = engine bone index of key k.
// Sanity (a stale / foreign skeleton never publishes): every joint finite and within maxReachM of the root, every segment
// <= maxSegmentM. Returns the part count (0 = unusable: the actor keeps root-only vectors). upm = engine units per metre.
inline constexpr float maxReachM=3.f,maxSegmentM=1.2f;
inline unsigned makeParts(const std::array<const float*,KeyCount>& m,const std::array<std::int16_t,KeyCount>& bones,const float root[3],float upm,ActorBones& out) noexcept {
    out.count=0;const float u=upm>0?upm:1.f;
    const auto pos=[&](unsigned k,float p[3]){if(k>=KeyCount||!m[k])return false;for(int i=0;i<3;++i)p[i]=m[k][9+i];
        float d2=0;for(int i=0;i<3;++i){if(!std::isfinite(p[i]))return false;const float d=p[i]-root[i];d2+=d*d;}
        return d2<=maxReachM*maxReachM*u*u;};
    for(unsigned s=0;s<partSpecs.size();++s){
        const auto& sp=partSpecs[s];float a[3]{},b[3]{},f[3]{};
        if(!pos(sp.a,a))continue;
        if(sp.b<KeyCount){if(!pos(sp.b,b))continue;}
        else{if(!pos(sp.from,f))continue;float d[3]{a[0]-f[0],a[1]-f[1],a[2]-f[2]};const float l=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
            if(!(l>1e-4f*u))continue;for(int i=0;i<3;++i)b[i]=a[i]+d[i]/l*sp.extend*u;}
        float seg2=0;for(int i=0;i<3;++i)seg2+=(b[i]-a[i])*(b[i]-a[i]);if(!(seg2<=maxSegmentM*maxSegmentM*u*u))continue;
        const unsigned mk=m[sp.motion]?unsigned(sp.motion):unsigned(sp.a);
        bool finite=true;for(int i=0;i<12;++i)finite=finite&&std::isfinite(m[mk][i])&&std::fabs(m[mk][i])<1e6f;if(!finite)continue;
        auto& p=out.part[out.count++];std::memcpy(p.a,a,sizeof a);std::memcpy(p.b,b,sizeof b);p.radius=sp.radius*u;p.cls=std::uint8_t(sp.cls);p.spec=std::uint8_t(s);
        p.bone=std::uint16_t(bones[mk]>=0?bones[mk]:0);std::memcpy(p.m,m[mk],sizeof p.m);
    }
    return out.count;
}
// Uniform block entry of one part (std140): a.xyz / b.xyz = the segment in the actor block's local frame (world axes, offset from the
// block centre = toLocal space), a.w = radius^2, b.w = 1 / |b - a|^2 (0: a sphere); toPrev = engine NDC -> previous clip of a point on
// this part.
struct GpuBone {float a[4];float b[4];float toPrev[16];};
static_assert(sizeof(GpuBone)==96);
struct BoneStats {std::uint64_t actors{},parts{},unmatched{},jump{},overflow{},math{};float maxStepM{};};
inline M4 poseMatrix(const float* m12) noexcept {M4 m=identity();for(int c=0;c<4;++c)for(int r=0;r<3;++r)m[c*4+r]=m12[c*3+r];return m;}
// a CHEAP upper bound of the prepared values (no inverse, no 4x4 product), for the decisions
// buildActorBlock takes BEFORE prepareBoneParts (still / behind / off-screen: standing NPCs and
// walkers behind the viewer can fill the 8 part slots and would otherwise be prepared + emitted every
// pair, only to be thrown away). Point q riding on a part bone, G = (B | d), x = G^-1 q:
// step = |Gp x - Gc x| <= |dp - dc| + ||Bp - Bc||_F |x|, |x| <= |q - dc| / sqrt(1 - ||Bc^T Bc - I||_F).
// So step <= bound.step (still: "bound.step < 2 mm" implies the exact decision). boundBoneBox: every
// prepared capsule (centre within step / 2 of q, radius + padSteps * step) lies inside bound.radius
// (horizontal, around mid) / bound.top - only computed for an actor that is not still.
// false: layouts differ or a basis is far from orthonormal (no early decision; prepareBoneParts
// decides).
struct PartBound {double step{},radius{},top{-1e30};bool box{};std::array<double,maxActorBones> partStep{};};
inline bool boundBoneSteps(const ActorBones& cb,const ActorBones& pb,PartBound& out) noexcept {
    out.step=0;out.radius=0;out.top=-1e30;out.box=false;
    if(!cb.count||cb.count!=pb.count||cb.layout!=pb.layout||cb.count>maxActorBones)return false;
    for(unsigned k=0;k<cb.count;++k){
        const auto& c=cb.part[k];const auto& p=pb.part[k];if(c.spec!=p.spec||c.bone!=p.bone)return false;
        double dB=0,dT=0,e=0,l=0;
        for(int i=0;i<9;++i){const double x=double(p.m[i])-double(c.m[i]);dB+=x*x;}
        for(int i=9;i<12;++i){const double x=double(p.m[i])-double(c.m[i]);dT+=x*x;}
        for(int i=0;i<3;++i)for(int j=i;j<3;++j){double s=i==j?-1.0:0.0;for(int r=0;r<3;++r)s+=double(c.m[i*3+r])*double(c.m[j*3+r]);e+=(i==j?1.0:2.0)*s*s;}
        if(!(e<0.25))return false;                                                                 // ||E||_F < 0.5
        for(const float* q:{c.a,c.b}){double v=0;for(int i=0;i<3;++i){const double x=double(q[i])-double(c.m[9+i]);v+=x*x;}l=std::max(l,v);}
        const double step=std::sqrt(dT)+std::sqrt(dB*l/(1.0-std::sqrt(e)));
        if(!std::isfinite(step))return false;
        out.partStep[k]=step;out.step=std::max(out.step,step);
    }
    return true;
}
inline void boundBoneBox(const ActorBones& cb,const double mid[3],double padSteps,PartBound& out) noexcept {
    const double widen=padSteps>0?padSteps:0.0;out.radius=0;out.top=-1e30;
    for(unsigned k=0;k<cb.count&&k<maxActorBones;++k){const auto& c=cb.part[k];const double r=double(c.radius)+(widen+0.5)*out.partStep[k];
        for(const float* q:{c.a,c.b}){const double x=double(q[0])-mid[0],y=double(q[1])-mid[1],z=double(q[2])-mid[2];
            out.radius=std::max(out.radius,std::sqrt(x*x+z*z)+r);out.top=std::max(out.top,y+r);}}
    out.box=true;
}
// the EYE-INDEPENDENT half of one actor's parts for one produced pair: D = Gprev * inverse(Gcur) per part, the capsule centred
// between its two poses in the block frame (mid = block centre, world), widened by padSteps of its step; bounds (horizontal radius /
// top around mid) and the largest endpoint step (engine units). Computed for the first eye, reused for the second (BonePairCache).
// Plus the cheap bound of the same pair (bGuid / bCur / bPrev: its own key; computed once per pair, reused by the second eye).
struct PreparedParts {std::uint32_t guid{};std::uint64_t cur{},prev{};unsigned count{};bool valid{};double boundR{},boundTop{},maxStep{};
    std::array<M4,maxActorBones> D{};std::array<std::array<float,8>,maxActorBones> ab{};
    std::uint32_t bGuid{};std::uint64_t bCur{},bPrev{};bool bounded{};PartBound bound{};};
struct BonePairCache {std::array<PreparedParts,maxBoneActors> entry{};};
// tight (debug.rdrvr.mvbones 3) = each capsule at the CURRENT pose with its own radius (no centring between the two
// poses, no padSteps widening). The half-step widening of a walker's parts (~2 cm) lets thighs /
// hands / arms take coat and torso pixels; tight keeps the limbs and frees most of those, but loses
// limb pixels if the snapshot lags the drawn frame by a tick.
inline bool prepareBoneParts(const ActorBones& cb,const ActorBones& pb,const double mid[3],double padSteps,double upm,PreparedParts& out,BoneStats& st,bool tight=false) noexcept {
    out.valid=false;out.count=0;out.boundR=0;out.boundTop=-1e30;out.maxStep=0;
    if(!cb.count||cb.count!=pb.count||cb.layout!=pb.layout||cb.count>maxActorBones){++st.unmatched;return false;}
    for(unsigned k=0;k<cb.count;++k)if(cb.part[k].spec!=pb.part[k].spec||cb.part[k].bone!=pb.part[k].bone){++st.unmatched;return false;}
    const double u=upm>0?upm:1.0,widen=padSteps>0?padSteps:0.0;
    std::array<std::array<double,2>,maxActorBones> stepOf{};std::array<std::array<std::array<double,3>,2>,maxActorBones> moved{};
    for(unsigned k=0;k<cb.count;++k){
        M4 inv{};if(!invert(poseMatrix(cb.part[k].m),inv)){++st.math;return false;}
        out.D[k]=mul(poseMatrix(pb.part[k].m),inv);const auto& D=out.D[k];
        for(int e=0;e<2;++e){const float* q=e?cb.part[k].b:cb.part[k].a;auto& w=moved[k][std::size_t(e)];
            for(int r=0;r<3;++r)w[std::size_t(r)]=D[0*4+r]*q[0]+D[1*4+r]*q[1]+D[2*4+r]*q[2]+D[3*4+r];
            stepOf[k][std::size_t(e)]=std::sqrt((w[0]-q[0])*(w[0]-q[0])+(w[1]-q[1])*(w[1]-q[1])+(w[2]-q[2])*(w[2]-q[2]));
            out.maxStep=std::max(out.maxStep,stepOf[k][std::size_t(e)]);}
    }
    if(!(out.maxStep<=1.5*u)){++st.jump;return false;}                                      // a skeleton swap / teleport: root only
    for(unsigned k=0;k<cb.count;++k){
        const auto& p=cb.part[k];double a[3]{},b[3]{};const double step=std::max(stepOf[k][0],stepOf[k][1]);
        for(int r=0;r<3;++r){a[r]=(p.a[r]+moved[k][0][std::size_t(r)])/2-mid[r];b[r]=(p.b[r]+moved[k][1][std::size_t(r)])/2-mid[r];} // centred between the two poses
        if(tight)for(int r=0;r<3;++r){a[r]=p.a[r]-mid[r];b[r]=p.b[r]-mid[r];}                       // The current pose
        const double r=p.radius+(tight?0.0:widen*step),l2=(b[0]-a[0])*(b[0]-a[0])+(b[1]-a[1])*(b[1]-a[1])+(b[2]-a[2])*(b[2]-a[2]);
        for(const double* q:{a,b}){out.boundR=std::max(out.boundR,std::sqrt(q[0]*q[0]+q[2]*q[2])+r);out.boundTop=std::max(out.boundTop,q[1]+r);}
        auto& g=out.ab[k];for(int i=0;i<3;++i){g[std::size_t(i)]=float(a[i]);g[std::size_t(4+i)]=float(b[i]);}g[3]=float(r*r);g[7]=l2>1e-12?float(1.0/l2):0.f;
        for(float x:g)if(!std::isfinite(x)){++st.math;return false;}
    }
    out.count=cb.count;out.valid=true;
    ++st.actors;st.parts+=cb.count;st.maxStepM=std::max(st.maxStepM,float(out.maxStep/u));
    return true;
}
// The eye half: toPrev = prevView * D * base per part (base = engine NDC -> world of this eye, prevView = world -> previous clip).
// Returns the parts written (0: no room / math).
inline unsigned emitBoneParts(const PreparedParts& pp,const M4& base,const M4& prevView,GpuBone* out,unsigned room) noexcept {
    if(!pp.valid||!pp.count||pp.count>room)return 0;
    for(unsigned k=0;k<pp.count;++k){
        auto& g=out[k];const M4 toPrev=mul(prevView,mul(pp.D[k],base));bool finite=true;
        for(int i=0;i<4;++i){g.a[i]=pp.ab[k][std::size_t(i)];g.b[i]=pp.ab[k][std::size_t(4+i)];}
        for(int i=0;i<16;++i){g.toPrev[i]=float(toPrev[i]);finite=finite&&std::isfinite(g.toPrev[i]);}
        if(!finite)return 0;
    }
    return pp.count;
}
// CPU reference of the shader's part choice (float, the shader's order): the part whose capsule contains p with the nearest axis.
// Returns the index in [0, n) or -1. margin (optional): the smallest |d2 - r2| / |d2 - second best d2| of the decision (ties).
inline int nearestPart(const GpuBone* parts,int n,const float p[3],float* margin=nullptr) noexcept {
    int pick=-1;float best=1e30f,second=1e30f,edge=1e30f;
    for(int k=0;k<n;++k){
        const auto& g=parts[k];
        const float ab[3]{g.b[0]-g.a[0],g.b[1]-g.a[1],g.b[2]-g.a[2]},ap[3]{p[0]-g.a[0],p[1]-g.a[1],p[2]-g.a[2]};
        const float t=std::clamp((ap[0]*ab[0]+ap[1]*ab[1]+ap[2]*ab[2])*g.b[3],0.f,1.f);
        const float q[3]{ap[0]-ab[0]*t,ap[1]-ab[1]*t,ap[2]-ab[2]*t};const float d2=q[0]*q[0]+q[1]*q[1]+q[2]*q[2];
        edge=std::min(edge,std::fabs(d2-g.a[3]));
        if(d2<=g.a[3]){if(d2<best){second=best;best=d2;pick=k;}else second=std::min(second,d2);}
    }
    if(margin)*margin=std::min(edge,pick>=0&&second<1e29f?second-best:1e30f);
    return pick;
}
} // namespace rdrvr::runtime::spacewarp
