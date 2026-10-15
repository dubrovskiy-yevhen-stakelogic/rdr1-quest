#pragma once
// Vulkan-free part of the VR hands pass (host-testable).
//  - HandSample / PoseRing: the XR thread's controller samples keyed by XR frame id; the render thread picks the sample
//    of the eye plan's own XR frame. Seqlock slots: the reader never blocks.
//  - MarkerSet / markerBoard: up to 8 holster markers in LOCAL (HOLSTERS -> HANDS), seqlock.
//  - handModel(): the SA / UltimateXR hand basis: forward = aim -Z, up = grip +X made
//    orthogonal to forward, right = up x forward, then the LEFT palm normal (up) is reflected;
//    mesh X -> forward with the wrist 5.5 cm behind the grip, mesh Y -> -up, mesh Z -> right.
//  - clip = F * P_eye * inverse(E_eye) * M (head anchor, origin and yaw of the engine cancel exactly);
//    F flips clip Y when the plan carries the engine's GL-style projection (P[5] > 0) so that a POSITIVE Vulkan viewport
//    maps a pixel exactly onto the same texel of the eye's world depth.
//  - screenRect(): pixel rectangle of projected bounds (renderArea / scissor of the pass).
#include "runtime/ControllerInput.h"
#include "runtime/Math.h"
#include "contracts/SubmissionPolicy.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace rdrvr::vr::hands {
using runtime::Vec3;
using runtime::Quat;
using runtime::Mat4;

// ---- single-writer seqlock over relaxed atomic words (no data race, no reader lock) ----
template<class T>
class SeqCell {
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr std::size_t words=(sizeof(T)+7)/8;
    std::atomic<std::uint64_t> sequence_{0};
    std::array<std::atomic<std::uint64_t>,words> data_{};
public:
    // One writer at a time (the owning thread).
    void store(const T& value) noexcept {
        std::array<std::uint64_t,words> buffer{};std::memcpy(buffer.data(),&value,sizeof(T));
        const auto s=sequence_.load(std::memory_order_relaxed);
        sequence_.store(s+1,std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for(std::size_t i=0;i<words;++i)data_[i].store(buffer[i],std::memory_order_relaxed);
        sequence_.store(s+2,std::memory_order_release);
    }
    // false = never written or a write kept overlapping (bounded retries).
    bool load(T& out) const noexcept {
        for(unsigned attempt=0;attempt<4;++attempt){
            const auto before=sequence_.load(std::memory_order_acquire);
            if(!before)return false;
            if(before&1)continue;
            std::array<std::uint64_t,words> buffer{};
            for(std::size_t i=0;i<words;++i)buffer[i]=data_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(sequence_.load(std::memory_order_relaxed)!=before)continue;
            std::memcpy(&out,buffer.data(),sizeof(T));return true;
        }
        return false;
    }
};

// ---- controller samples ----
struct HandSample {
    std::uint64_t sequence{},publishedNs{},epoch{},referenceSpace{};
    std::array<Pose,2> aim{},grip{};
    std::array<float,2> grips{},triggers{};
};
inline HandSample handSample(const runtime::ControllerSample& c) noexcept {
    HandSample s{};s.sequence=c.sequence;s.publishedNs=c.publishedNs;s.epoch=c.epoch;s.referenceSpace=c.referenceSpace;
    s.aim=c.aim;s.grip=c.grip;s.grips=c.grips;s.triggers=c.triggers;return s;
}
enum class PoseMatch : unsigned {Exact,Older,Newer,None};
class PoseRing {
public:
    static constexpr unsigned size=32; // > 0.4 s of XR frames at 72 Hz
private:
    std::array<SeqCell<HandSample>,size> cells_{};
public:
    // XR thread only.
    void push(const HandSample& s) noexcept {if(s.sequence)cells_[s.sequence%size].store(s);}
    // Any thread. Exact frame, else the newest older one, else the oldest newer one.
    PoseMatch find(std::uint64_t frame,HandSample& out) const noexcept {
        HandSample s{};
        if(frame&&cells_[frame%size].load(s)&&s.sequence==frame){out=s;return PoseMatch::Exact;}
        bool older=false,newer=false;HandSample bestOlder{},bestNewer{};
        for(const auto& cell:cells_){
            if(!cell.load(s)||!s.sequence)continue;
            if(s.sequence<=frame){if(!older||s.sequence>bestOlder.sequence){bestOlder=s;older=true;}}
            else if(!newer||s.sequence<bestNewer.sequence){bestNewer=s;newer=true;}
        }
        if(older){out=bestOlder;return PoseMatch::Older;}
        if(newer){out=bestNewer;return PoseMatch::Newer;}
        return PoseMatch::None;
    }
};

// ---- holster markers (LOCAL metres of the XR reference space epoch `referenceSpace`) ----
struct Marker {
    float position[3]{};
    float radius{};     // metres (visual ring radius)
    float highlight{};  // 0 = idle, 1 = a hand is inside the grab radius
    // bit0 MarkerWeapon (hammer ring), bits 8-15 palette style (0 = the default ring)
    std::uint32_t flags{};
};
inline constexpr std::uint32_t MarkerWeapon=1u;
inline constexpr std::uint32_t markerStyle(const Marker& m) noexcept {return (m.flags>>8)&0xffu;}
struct MarkerSet {
    static constexpr unsigned capacity=8;
    std::uint64_t referenceSpace{},publishedNs{};
    std::uint32_t count{};
    std::array<Marker,capacity> markers{};
};
// ---- RELOAD overlay (producer reload::Brain on the input thread, same layout as reload::Overlay) ----
// frame 0 = GRIP: metres in the holding controller's grip frame, drawn through the plan's WeaponFrame (map * pose(grip)) = rigid with the
// engine-drawn gun; frame 1 = LOCAL of `referenceSpace`. style = the marker palette (1 EMPTY, 2 SPENT, 3 LIVE, 4 held tip, 5 zone, 6 debug).
struct ReloadOverlayItem {float pos[3]{};float radius{};float highlight{};std::uint32_t style{};std::uint32_t frame{};std::uint32_t pad{};};
struct ReloadOverlay {std::uint64_t referenceSpace{},ns{};std::int32_t hand{-1};std::uint32_t count{};std::array<ReloadOverlayItem,16> items{};};
static_assert(sizeof(ReloadOverlayItem)==32&&sizeof(ReloadOverlay)==24+16*32);
// Procedural cartridges (reload::StyleCartLive / StyleCartSpent; StyleCylinder = the open cylinder, an analytic occluder for the
// chambered rounds after it, frame bit ReloadFrameInCylinder). pos = the rim's rear point / the rear-face centre, radius = the case / outer
// radius, highlight = the case / cylinder length (m), pad = the axis packed 3 x 10-bit snorm. frame bit 0: 0 GRIP, 1 LOCAL.
inline constexpr std::uint32_t ReloadStyleCartLive=8,ReloadStyleCartSpent=9,ReloadStyleCylinder=10,ReloadFrameInCylinder=0x100u;
inline std::array<float,3> unpackReloadAxis(std::uint32_t w) noexcept {
    std::array<float,3> a{};for(int i=0;i<3;++i)a[std::size_t(i)]=float((w>>(10*i))&1023u)/511.5f-1.f;
    const float l=std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]);if(l>1e-4f)for(float& x:a)x/=l;return a;}
inline bool validMarker(const Marker& m) noexcept {
    for(float x:m.position)if(!std::isfinite(x)||std::abs(x)>100.f)return false;
    return std::isfinite(m.radius)&&m.radius>0.005f&&m.radius<0.5f&&std::isfinite(m.highlight);
}

// ---- double precision column-major 4x4 (same layout as runtime::Mat4: m[column*4+row]) ----
struct DMat4 {std::array<double,16> m{};};
inline DMat4 identity() noexcept {DMat4 r{};r.m[0]=r.m[5]=r.m[10]=r.m[15]=1;return r;}
inline DMat4 multiply(const DMat4& a,const DMat4& b) noexcept {
    DMat4 r{};
    for(int c=0;c<4;++c)for(int row=0;row<4;++row){double v=0;for(int k=0;k<4;++k)v+=a.m[k*4+row]*b.m[c*4+k];r.m[c*4+row]=v;}
    return r;
}
inline DMat4 widen(const Mat4& a) noexcept {DMat4 r{};for(unsigned i=0;i<16;++i)r.m[i]=a[i];return r;}
inline Mat4 narrow(const DMat4& a) noexcept {Mat4 r{};for(unsigned i=0;i<16;++i)r[i]=float(a.m[i]);return r;}
inline std::array<double,4> transform(const DMat4& a,double x,double y,double z,double w=1) noexcept {
    std::array<double,4> r{};for(int row=0;row<4;++row)r[row]=a.m[row]*x+a.m[4+row]*y+a.m[8+row]*z+a.m[12+row]*w;return r;
}
using DVec3=std::array<double,3>;
inline DVec3 rotateD(const Quat& qf,const DVec3& v) noexcept {
    const double q[4]{qf[0],qf[1],qf[2],qf[3]};
    const DVec3 t{2*(q[1]*v[2]-q[2]*v[1]),2*(q[2]*v[0]-q[0]*v[2]),2*(q[0]*v[1]-q[1]*v[0])};
    return {v[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1],v[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2],v[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0]};
}
inline double dotD(const DVec3& a,const DVec3& b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline DVec3 crossD(const DVec3& a,const DVec3& b) noexcept {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline bool normalizeD(DVec3& v) noexcept {const double n=std::sqrt(dotD(v,v));if(!(n>1e-9))return false;for(double& x:v)x/=n;return true;}

// [R(q) | position*scale]: the LOCAL pose as the plan composes it (CameraAdapter::prepare, unitsPerMetre = scale).
inline DMat4 poseMatrix(const Pose& p,double scale) noexcept {
    const auto x=rotateD(p.orientation,{1,0,0}),y=rotateD(p.orientation,{0,1,0}),z=rotateD(p.orientation,{0,0,1});
    return {{x[0],x[1],x[2],0, y[0],y[1],y[2],0, z[0],z[1],z[2],0, p.position[0]*scale,p.position[1]*scale,p.position[2]*scale,1}};
}
// inverse([R | t]) for an orthonormal R.
inline DMat4 rigidInverse(const DMat4& a) noexcept {
    DMat4 r{};
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)r.m[j*4+i]=a.m[i*4+j];
    for(int i=0;i<3;++i)r.m[12+i]=-(r.m[i]*a.m[12]+r.m[4+i]*a.m[13]+r.m[8+i]*a.m[14]);
    r.m[15]=1;return r;
}
// F * P: back to Vulkan's y-down clip for a positive-height viewport when the engine projection is GL-style.
inline DMat4 vulkanProjection(const Mat4& projection) noexcept {
    auto p=widen(projection);
    if(projection[5]>0)for(int c=0;c<4;++c)p.m[c*4+1]=-p.m[c*4+1];
    return p;
}
// LOCAL (metres) -> clip of one eye: F * P_e * inverse(E_e) * S(scale).
inline DMat4 localToClip(const Mat4& projection,const Pose& eye,double scale) noexcept {
    DMat4 s=identity();s.m[0]=s.m[5]=s.m[10]=scale;
    return multiply(multiply(vulkanProjection(projection),rigidInverse(poseMatrix(eye,scale))),s);
}

inline constexpr double wristOffsetMetres=0.055; // SA/UXRH: mesh origin sits 5.5 cm in front of the wrist
// Model matrix of hand h (0 left, 1 right) in LOCAL metres. Columns: forward, -up, right, grip - 0.055 * forward.
// Returns false for invalid poses or a degenerate basis (grip +X parallel to aim forward).
inline bool handModel(const Pose& aim,const Pose& grip,unsigned hand,DMat4& out) noexcept {
    if(hand>1||!validPose(aim)||!validPose(grip))return false;
    DVec3 forward=rotateD(aim.orientation,{0,0,-1});if(!normalizeD(forward))return false;
    DVec3 up=rotateD(grip.orientation,{1,0,0});
    const double d=dotD(up,forward);for(int i=0;i<3;++i)up[i]-=d*forward[i];
    if(dotD(up,up)<1e-4||!normalizeD(up))return false;
    DVec3 right=crossD(up,forward);if(!normalizeD(right))return false; // thumb axis BEFORE the palm reflection
    if(hand==0)for(double& x:up)x=-x;
    const DVec3 origin{grip.position[0]-wristOffsetMetres*forward[0],grip.position[1]-wristOffsetMetres*forward[1],grip.position[2]-wristOffsetMetres*forward[2]};
    out={{forward[0],forward[1],forward[2],0, -up[0],-up[1],-up[2],0, right[0],right[1],right[2],0, origin[0],origin[1],origin[2],1}};
    return true;
}
// Light direction (unit, LOCAL) expressed in model space: M3 is orthonormal (a reflection for the left hand), so
// dot(M3 n, L) = dot(n, M3^T L).
inline std::array<float,3> modelLight(const DMat4& model,const DVec3& lightLocal) noexcept {
    std::array<float,3> r{};for(int c=0;c<3;++c)r[c]=float(model.m[c*4]*lightLocal[0]+model.m[c*4+1]*lightLocal[1]+model.m[c*4+2]*lightLocal[2]);
    return r;
}
// Bilinear four-pose weights {open, grip, trigger, both} (SA Xr.cpp wO/wG/wT/wB). A hand holding a weapon is a fist.
inline std::array<float,4> poseWeights(float grip,float trigger,bool holding) noexcept {
    float g=std::isfinite(grip)?std::clamp(grip,0.f,1.f):0.f,t=std::isfinite(trigger)?std::clamp(trigger,0.f,1.f):0.f;
    if(holding)g=1.f;
    return {(1-g)*(1-t),g*(1-t),(1-g)*t,g*t};
}

// ---- screen rectangle ----
struct PixelRect {
    std::int32_t x0{},y0{},x1{},y1{}; // half-open [x0,x1) x [y0,y1)
    bool empty() const noexcept {return x1<=x0||y1<=y0;}
    std::uint64_t area() const noexcept {return empty()?0:std::uint64_t(x1-x0)*std::uint64_t(y1-y0);}
    void unite(const PixelRect& o) noexcept {if(o.empty())return;if(empty()){*this=o;return;}x0=std::min(x0,o.x0);y0=std::min(y0,o.y0);x1=std::max(x1,o.x1);y1=std::max(y1,o.y1);}
};
// Projects the 8 corners of [lo,hi] (model space) with clip; positive Vulkan viewport (0,0,W,H). Any corner at or behind
// the eye plane (w <= 1e-4) makes the result the whole eye (conservative). Off-screen or wholly in front of the near
// plane -> empty. pad: pixels added on every side (rasterization rounding).
inline PixelRect screenRect(const DMat4& clip,const Vec3& lo,const Vec3& hi,std::uint32_t width,std::uint32_t height,int pad=2) noexcept {
    PixelRect full{0,0,std::int32_t(width),std::int32_t(height)};
    double minX=1e30,minY=1e30,maxX=-1e30,maxY=-1e30;bool anyBeyondNear=false;
    for(unsigned i=0;i<8;++i){
        const auto p=transform(clip,(i&1)?hi[0]:lo[0],(i&2)?hi[1]:lo[1],(i&4)?hi[2]:lo[2]);
        if(!(p[3]>1e-4))return full;
        if(!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2]))return full;
        if(p[2]>=0)anyBeyondNear=true; // forward Z: z/w < 0 is in front of the near plane
        const double x=(p[0]/p[3]*0.5+0.5)*width,y=(p[1]/p[3]*0.5+0.5)*height;
        minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);
    }
    if(!anyBeyondNear)return {};
    PixelRect r{};
    r.x0=std::int32_t(std::clamp(std::floor(minX)-pad,0.0,double(width)));r.x1=std::int32_t(std::clamp(std::ceil(maxX)+pad,0.0,double(width)));
    r.y0=std::int32_t(std::clamp(std::floor(minY)-pad,0.0,double(height)));r.y1=std::int32_t(std::clamp(std::ceil(maxY)+pad,0.0,double(height)));
    return r.empty()?PixelRect{}:r;
}
// ---- the VR-held weapon of a frame (VrWeapon tags its placement with the head sample of the plan its eyes render) ----
// The engine draws the gun in WORLD with the placement computed at weapWeapon::Update from map (LOCAL metres -> world).
// The pass draws in LOCAL of the eye plan: worldToLocal(plan) * map * (hand pose) puts the held
// hand exactly where the gun is (rigid), and the laser (muzzle + direction, world) lands exactly
// on the drawn barrel, whatever the residual between map and this plan.
struct WeaponFrame {
    enum : std::uint32_t {Laser=1,Hit=2,Firearm=4,Probed=8, // Probed: laserMetres came from an engine probe (no Hit = clear ray)
        Bridged=16}; // Placed through the snap-turn bridge (pre-turn plan rotated by the learned sign)
    std::uint64_t headNs{},referenceSpace{},epoch{},sequence{};
    std::int32_t hand{-1};std::uint32_t flags{};
    Pose aim{},grip{};               // LOCAL poses of the physical holding hand used for the placement
    Mat4 map{};                      // LOCAL metres -> world of that placement (uniform scale = unitsPerMetre)
    Vec3 muzzle{},direction{};       // world, direction unit
    float laserMetres{},laserWidthMm{},unitsPerMetre{1};
};
class WeaponBoard {
    static constexpr unsigned size=8;
    std::array<SeqCell<WeaponFrame>,size> cells_{};std::atomic<unsigned> next_{0};
public:
    void publish(const WeaponFrame& f) noexcept { // one writer (engine main thread)
        if(!f.headNs)return;const auto i=next_.load(std::memory_order_relaxed);cells_[i%size].store(f);next_.store(i+1,std::memory_order_relaxed);}
    bool find(std::uint64_t headNs,WeaponFrame& out) const noexcept {
        if(!headNs)return false;WeaponFrame f{};
        for(const auto& c:cells_)if(c.load(f)&&f.headNs==headNs){out=f;return true;}
        return false;
    }
};
// LOCAL metres -> world of an eye plan: W = world_e * inverse(E_e(scale)) * S(scale) (the hands' own composition).
inline DMat4 localToWorld(const Mat4& world,const Pose& eye,double scale) noexcept {
    DMat4 s=identity();s.m[0]=s.m[5]=s.m[10]=scale;return multiply(multiply(widen(world),rigidInverse(poseMatrix(eye,scale))),s);
}
// Inverse of [R*s | t] (R orthonormal, s > 0).
inline DMat4 similarityInverse(const DMat4& a) noexcept {
    const double s2=a.m[0]*a.m[0]+a.m[1]*a.m[1]+a.m[2]*a.m[2];DMat4 r{};if(!(s2>1e-12))return identity();
    for(int i=0;i<3;++i)for(int j=0;j<3;++j)r.m[j*4+i]=a.m[i*4+j]/s2;
    for(int i=0;i<3;++i)r.m[12+i]=-(r.m[i]*a.m[12]+r.m[4+i]*a.m[13]+r.m[8+i]*a.m[14]);
    r.m[15]=1;return r;
}
// Pixel rectangle of the segment a..b (LOCAL) clipped to w > 1e-3 (positive viewport, same conventions as screenRect) plus a pad
// in pixels; empty when wholly behind the eye.
inline PixelRect segmentRect(const DMat4& clip,const Vec3& a,const Vec3& b,std::uint32_t width,std::uint32_t height,int pad) noexcept {
    auto pa=transform(clip,a[0],a[1],a[2]),pb=transform(clip,b[0],b[1],b[2]);
    constexpr double nearW=1e-3;
    if(!(pa[3]>nearW)&&!(pb[3]>nearW))return {};
    const auto cut=[&](std::array<double,4>& behind,const std::array<double,4>& front){const double t=(nearW-behind[3])/(front[3]-behind[3]);
        for(int i=0;i<4;++i)behind[i]+=(front[i]-behind[i])*t;};
    if(!(pa[3]>nearW))cut(pa,pb);else if(!(pb[3]>nearW))cut(pb,pa);
    double minX=1e30,minY=1e30,maxX=-1e30,maxY=-1e30;
    for(const auto* p:{&pa,&pb}){const auto& q=*p;if(!std::isfinite(q[0])||!std::isfinite(q[1])||!(q[3]>0))return {};
        const double x=(q[0]/q[3]*0.5+0.5)*width,y=(q[1]/q[3]*0.5+0.5)*height;
        minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);}
    PixelRect r{};
    r.x0=std::int32_t(std::clamp(std::floor(minX)-pad,0.0,double(width)));r.x1=std::int32_t(std::clamp(std::ceil(maxX)+pad,0.0,double(width)));
    r.y0=std::int32_t(std::clamp(std::floor(minY)-pad,0.0,double(height)));r.y1=std::int32_t(std::clamp(std::ceil(maxY)+pad,0.0,double(height)));
    return r.empty()?PixelRect{}:r;
}
// Rigidity of the plan's LOCAL->world map: W_e = world_e * inverse(E_e) must be the same for both eyes (the hands are
// drawn in LOCAL; a camera module that composes the eyes differently would show up here). Max abs element difference
// of W_0 vs W_1 (rotation part unitless, translation in world units).
inline double planRigidity(const std::array<Mat4,2>& world,const std::array<Pose,2>& eyes,double scale) noexcept {
    std::array<DMat4,2> w{};
    for(unsigned e=0;e<2;++e)w[e]=multiply(widen(world[e]),rigidInverse(poseMatrix(eyes[e],scale)));
    double worst=0;for(unsigned i=0;i<16;++i)worst=std::max(worst,std::abs(w[0].m[i]-w[1].m[i]));
    return std::isfinite(worst)?worst:1e30;
}

// =====================================================================================================================
// ARC: the shared arc / trajectory channels of the hands pass.
// Producers publish WORLD frames from their main-thread ticks (key = the head anchor of the same Execute iteration,
// arc::Context::key()) or LOCAL frames (THROW channel, input brain); the render thread picks the frame of its plan's head sample,
// converts WORLD -> the plan's LOCAL and expands the polyline into a camera-facing ribbon on the GPU (ArcVertex runs in a
// host-coherent ring, one instance per segment). Vulkan-free, host-testable.
inline constexpr unsigned arcMaxPoints=64,arcChannels=5,arcRingSlots=8,arcRunEntries=arcMaxPoints+2+4; // ribbon run + pillar run + MARKER
inline constexpr unsigned arcPropsEntries=32;                    // props region per ring slot (reserved)
inline constexpr std::uint64_t arcMaxAgeNs=100'000'000,arcMaxLagNs=60'000'000; // 100 ms, shared with the teleport module
// ONE writer thread per channel (bound on the first publish/clear; another thread's publish/clear is DROPPED and counted).
// LASSO / TELEPORT / DEBUG: engine main thread (their ticks). THROW: the holsters input brain (Local frames only).
enum class ArcChannel : unsigned {Lasso=0,Teleport=1,Throw=2,Debug=3,Marker=4}; // MARKER = VrMarker.h (ModuleMarker tick)
inline constexpr std::array<const char*,arcChannels> arcChannelNames{"LASSO","TELEPORT","THROW","DEBUG","MARKER"};
enum class ArcSpace : std::uint32_t {World=0,Local=1};
enum class ArcMarkerShape : std::uint32_t {None=0,Ring=1,Disc=2,Cross=3,Dot=4,Arrow=5};
// linear 0..1 -> R8G8B8A8 (R in the low byte = R8G8B8A8_UNORM byte order); NaN -> 0
inline constexpr std::uint32_t packRgba(float r,float g,float b,float a) noexcept {
    const auto q=[](float v){v=v==v?v:0.f;v=v<0.f?0.f:v>1.f?1.f:v;return std::uint32_t(v*255.f+.5f);};
    return q(r)|(q(g)<<8)|(q(b)<<16)|(q(a)<<24);
}
inline constexpr float rgbaChannel(std::uint32_t rgba,unsigned c) noexcept {return float((rgba>>(8*c))&0xffu)/255.f;}

struct ArcPoint {                 // 24 bytes
    float position[3]{};          // World: engine world units; Local: LOCAL metres of frame.referenceSpace
    float halfWidthM{.006f};      // metres; drawn at least minHalfPx (render side), alpha scaled by true/drawn width
    std::uint32_t rgba{};         // packRgba (linear colour, A = opacity)
    float depthTolM{0};           // world-depth test tolerance (m) toward the eye; toFrame ramps it up near a landing (grass)
};
struct ArcStyle {                 // 32 bytes
    enum : std::uint32_t {DashFromEnd=1,NoWorldTest=2};
    float dashPeriodM{0};         // 0 = solid
    float dashDuty{.55f};
    float dashSpeedMps{0};        // dash scroll toward the end; 0 = static (comfort default)
    float xrayAlpha{0};           // ribbon alpha multiplier where the world hides it (SAMPLED only); 0 = hidden there
    float fadeInM{.10f};          // alpha 0 -> 1 over the first fadeInM metres from point 0 (the hand)
    float tube{1};                // 1 = cylindrical shading, 0 = flat
    float core{.30f};             // hot-core boost
    std::uint32_t flags{};        // DashFromEnd (phase anchored at the target), NoWorldTest (needs no engine depth)
};
struct ArcMarker {                // 72 bytes
    ArcMarkerShape shape{ArcMarkerShape::None};
    std::uint32_t rgba{};
    float centre[3]{};            // same space as the points
    float radiusM{.25f};          // per frame (teleport ring 0.30 m); the render thread keeps >= 6 px
    float normal[3]{};            // surface normal (same space); all zero = camera-facing
    float liftM{.02f};            // drawn this far along the normal; raised to 4 depth steps at its distance
    float facing[3]{};            // Arrow: unit direction in the marker plane (same space); zero = none
    float depthTolM{.30f};        // world-depth tolerance of the marker (grass, rocks): CPU converts to a window-depth epsilon
    float xrayAlpha{.45f};        // marker alpha where the world still hides it after the tolerance (0 = hidden)
    float pillarHeightM{0};       // > 0: a camera-facing vertical beam centre -> centre + up * height (ribbon pipeline)
    std::uint32_t pillarRgba{};   // its colour at the base (alpha fades to 0 at the top)
    float reserved{};
};
struct ArcFrame {                 // 1680 bytes, trivially copyable, multiple of 8
    enum : std::uint32_t {Valid=1,Probed=2,Pending=4,Truncated=8,HandBlocked=16};
    std::uint64_t key{};          // World: arc::Context::key() = HeadCameraAnchor::sampledNs (never 0). Local: ignored
    std::uint64_t referenceSpace{}; // Local only (XR reference space epoch); World: 0
    std::uint64_t publishedNs{};  // STAMPED by publishArc with the render clock; the producer's value is ignored
    std::uint32_t space{};        // ArcSpace
    std::uint32_t count{};        // 0 = hidden for this key, else 2..arcMaxPoints
    std::uint32_t flags{};        // Valid, Probed, Pending (unresolved chord before the landing), Truncated, HandBlocked
    float unitsPerMetre{1};       // World: engine units per metre at publication (report only; conversion uses the plan's)
    ArcStyle style{};
    ArcMarker marker{};
    std::array<ArcPoint,arcMaxPoints> points{};
};
static_assert(sizeof(ArcPoint)==24&&sizeof(ArcStyle)==32&&sizeof(ArcMarker)==72&&sizeof(ArcFrame)==1680);
static_assert(std::is_trivially_copyable_v<ArcFrame>&&sizeof(ArcFrame)%8==0);

namespace detail {
inline bool finite3(const float* v) noexcept {return std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]);}
inline bool within(float v,float lo,float hi) noexcept {return std::isfinite(v)&&v>=lo&&v<=hi;}
inline float length3(const float* v) noexcept {return std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);}
}
// finite; count 0 or 2..64; |coord| < 1e5; halfWidth 0.5 mm..0.2 m; depthTol 0..2 m; marker radius <= 3 m, pillar <= 3 m; normal zero or
// 0.5..2 long (normalized by the render thread); facing zero or unit; World => key != 0.
inline bool validArc(const ArcFrame& f) noexcept {
    using namespace detail;
    if(f.space>1||(f.count!=0&&(f.count<2||f.count>arcMaxPoints)))return false;
    if(f.space==std::uint32_t(ArcSpace::World)&&!f.key)return false;
    if(!within(f.unitsPerMetre,1e-3f,1e4f))return false;
    for(std::uint32_t i=0;i<f.count;++i){const auto& p=f.points[i];
        if(!finite3(p.position)||std::abs(p.position[0])>=1e5f||std::abs(p.position[1])>=1e5f||std::abs(p.position[2])>=1e5f)return false;
        if(!within(p.halfWidthM,.0005f,.2f)||!within(p.depthTolM,0.f,2.f))return false;}
    const auto& s=f.style;
    if(!within(s.dashPeriodM,0.f,100.f)||!within(s.dashDuty,.01f,1.f)||!within(s.dashSpeedMps,-100.f,100.f)||!within(s.xrayAlpha,0.f,1.f)||
       !within(s.fadeInM,0.f,10.f)||!within(s.tube,0.f,1.f)||!within(s.core,0.f,4.f))return false;
    const auto& m=f.marker;
    if(std::uint32_t(m.shape)>std::uint32_t(ArcMarkerShape::Arrow))return false;
    if(m.shape!=ArcMarkerShape::None||m.pillarHeightM!=0.f){
        if(!finite3(m.centre)||std::abs(m.centre[0])>=1e5f||std::abs(m.centre[1])>=1e5f||std::abs(m.centre[2])>=1e5f)return false;
        if(!within(m.radiusM,1e-4f,3.f)||!within(m.liftM,0.f,1.f)||!within(m.depthTolM,0.f,2.f)||!within(m.xrayAlpha,0.f,1.f)||
           !within(m.pillarHeightM,0.f,3.f))return false;
        if(!finite3(m.normal)||!finite3(m.facing))return false;
        const float nl=length3(m.normal),fl=length3(m.facing);
        if(nl!=0.f&&(nl<.5f||nl>2.f))return false;
        if(fl!=0.f&&std::abs(fl-1.f)>.01f)return false;
    }
    return true;
}

// Per channel. Single writer (thread-bound); lock-free readers. 8 cells (~220 ms of main frames) keyed by `key`.
class ArcBoard {
    static constexpr unsigned size=8;
    static constexpr std::uint64_t localKey=~std::uint64_t(0); // keys_ marker of a Local frame (scanned by reference space)
    std::array<std::atomic<std::uint64_t>,size> keys_{};std::array<SeqCell<ArcFrame>,size> cells_{};std::atomic<unsigned> next_{0};
    std::atomic<std::uint64_t> clearedNs_{0},lastVisibleNs_{0},writer_{0},lastPublishedNs_{0};
    bool bind(std::uint64_t thread) noexcept {
        std::uint64_t expected=0;
        if(writer_.compare_exchange_strong(expected,thread,std::memory_order_acq_rel))return true;
        return expected==thread;
    }
public:
    enum Result : unsigned {Published,Rejected,WrongThread,Unkeyed};
    enum Pick : unsigned {Exact,Older,Missing,Stale,Cleared,Unanchored,OtherSpace,Torn,PickCount};
    // f.publishedNs = max(now, last clear + 1, last publish + 1) (strictly after a clear of the same instant). count > 0 -> lastVisibleNs = now.
    Result publish(ArcFrame f,std::uint64_t now,std::uint64_t thread) noexcept {
        if(f.space==std::uint32_t(ArcSpace::World)&&!f.key)return Unkeyed;
        if(!validArc(f))return Rejected;
        if(!thread||!bind(thread))return WrongThread;
        std::uint64_t stamp=now;
        stamp=std::max(stamp,clearedNs_.load(std::memory_order_relaxed)+1);
        stamp=std::max(stamp,lastPublishedNs_.load(std::memory_order_relaxed)+1);
        f.publishedNs=stamp;lastPublishedNs_.store(stamp,std::memory_order_relaxed);
        const unsigned i=next_.load(std::memory_order_relaxed)%size;
        keys_[i].store(0,std::memory_order_release);
        cells_[i].store(f);
        keys_[i].store(f.space==std::uint32_t(ArcSpace::World)?f.key:localKey,std::memory_order_release);
        next_.store(next_.load(std::memory_order_relaxed)+1,std::memory_order_relaxed);
        if(f.count)lastVisibleNs_.store(stamp,std::memory_order_release);
        return Published;
    }
    // Every frame published before this call is never drawn again (whatever its key); nothing keeps the pass alive afterwards.
    bool clear(std::uint64_t now,std::uint64_t thread) noexcept {
        if(!thread||!bind(thread))return false;
        const auto stamp=std::max(now,lastPublishedNs_.load(std::memory_order_relaxed));
        clearedNs_.store(std::max(stamp,clearedNs_.load(std::memory_order_relaxed)),std::memory_order_release);
        lastVisibleNs_.store(0,std::memory_order_release);return true;
    }
    // A visible frame was published within arcMaxAgeNs and not cleared since (hide frames never count).
    bool live(std::uint64_t now) const noexcept {
        const auto v=lastVisibleNs_.load(std::memory_order_acquire);
        return v&&(now<=v||now-v<=arcMaxAgeNs);
    }
    std::uint64_t lastVisibleNs() const noexcept {return lastVisibleNs_.load(std::memory_order_acquire);}
    // World: the largest key <= headNs and >= headNs - arcMaxLagNs (same key twice: the newer publishedNs); headNs == 0 -> no World pick.
    // Local: the newest frame whose referenceSpace == referenceSpace. Both: publishedNs > clearedNs (else Cleared) and now - publishedNs
    // <= arcMaxAgeNs (else Stale); a World and a Local candidate -> the newer publishedNs. A loaded cell whose key changed meanwhile ->
    // one retry, else Torn. Exact = World key == headNs, or a Local frame of the plan's reference space.
    Pick select(std::uint64_t headNs,std::uint64_t referenceSpace,std::uint64_t now,ArcFrame& out) const noexcept {
        for(unsigned attempt=0;attempt<2;++attempt){
            bool torn=false,otherSpace=false,anyWorld=false;
            bool haveW=false,haveL=false;ArcFrame w{},l{};
            for(unsigned i=0;i<size;++i){
                const auto k=keys_[i].load(std::memory_order_acquire);
                if(!k)continue;
                if(k==localKey){
                    ArcFrame f{};
                    if(!cells_[i].load(f)||keys_[i].load(std::memory_order_acquire)!=k||f.space!=std::uint32_t(ArcSpace::Local)){torn=true;continue;}
                    if(f.referenceSpace!=referenceSpace||!referenceSpace){otherSpace=true;continue;}
                    if(!haveL||f.publishedNs>l.publishedNs){l=f;haveL=true;}
                    continue;
                }
                anyWorld=true;
                if(!headNs||k>headNs||headNs-k>arcMaxLagNs)continue;
                if(haveW&&k<w.key)continue;
                ArcFrame f{};
                if(!cells_[i].load(f)||keys_[i].load(std::memory_order_acquire)!=k||f.key!=k){torn=true;continue;}
                if(!haveW||f.key>w.key||(f.key==w.key&&f.publishedNs>w.publishedNs)){w=f;haveW=true;}
            }
            if(torn&&!haveW&&!haveL&&attempt==0)continue; // one retry
            if(!haveW&&!haveL){
                if(torn)return Torn;
                if(!headNs&&!otherSpace)return Unanchored;
                if(otherSpace&&!anyWorld)return OtherSpace;
                return headNs?Missing:(otherSpace?OtherSpace:Unanchored);
            }
            const bool useW=haveW&&(!haveL||w.publishedNs>=l.publishedNs);
            const ArcFrame& f=useW?w:l;
            if(f.publishedNs<=clearedNs_.load(std::memory_order_acquire))return Cleared;
            if(now>f.publishedNs&&now-f.publishedNs>arcMaxAgeNs)return Stale;
            out=f;return useW?(f.key==headNs?Exact:Older):Exact;
        }
        return Torn;
    }
    void resetWriterForTests() noexcept {writer_.store(0,std::memory_order_release);}
};

// GPU instance data: one entry per point, the run padded [p0, p0, p1 .. pn-1, pn-1] (n + 2 entries); pillar run [c, c, t, t].
struct ArcVertex {float position[3];float halfWidthM;std::uint32_t rgba;float sM;float depthTolM;float reserved;};
static_assert(sizeof(ArcVertex)==32&&std::is_trivially_copyable_v<ArcVertex>);
inline constexpr std::uint32_t arcRingSlotBytes=std::uint32_t(arcChannels*arcRunEntries*sizeof(ArcVertex)+arcPropsEntries*sizeof(ArcVertex));
inline constexpr std::uint32_t arcRingArcBytes=std::uint32_t(arcChannels*arcRunEntries*sizeof(ArcVertex)); // 11200: props start here
static_assert(arcRingSlotBytes==12224&&arcRingArcBytes==11200);

inline DVec3 transformPoint(const DMat4& m,const DVec3& p) noexcept {const auto r=transform(m,p[0],p[1],p[2]);return {r[0],r[1],r[2]};}
inline DVec3 transformDirection(const DMat4& m,const DVec3& v) noexcept {
    return {m.m[0]*v[0]+m.m[4]*v[1]+m.m[8]*v[2],m.m[1]*v[0]+m.m[5]*v[1]+m.m[9]*v[2],m.m[2]*v[0]+m.m[6]*v[1]+m.m[10]*v[2]};}
// WORLD (or LOCAL with toLocal = identity) points -> the padded LOCAL run. Points closer than 1 mm to the previous kept point are
// dropped; s = arc length from point 0 (LOCAL metres). Returns the entry count (points + 2), 0 when < 2 points remain / no room.
inline unsigned buildArcVertices(const ArcFrame& f,const DMat4& toLocal,ArcVertex* out,unsigned capacity,float& lengthM) noexcept {
    lengthM=0.f;
    if(!out||f.count<2||f.count>arcMaxPoints||capacity<4)return 0;
    unsigned n=0;DVec3 last{};double s=0;
    for(std::uint32_t i=0;i<f.count;++i){
        const auto& p=f.points[i];
        const DVec3 q=transformPoint(toLocal,{p.position[0],p.position[1],p.position[2]});
        if(!std::isfinite(q[0])||!std::isfinite(q[1])||!std::isfinite(q[2]))return 0;
        if(n){const double d=std::sqrt((q[0]-last[0])*(q[0]-last[0])+(q[1]-last[1])*(q[1]-last[1])+(q[2]-last[2])*(q[2]-last[2]));
            if(d<1e-3)continue;s+=d;}
        if(n+3>capacity)return 0;
        auto& v=out[n+1];v.position[0]=float(q[0]);v.position[1]=float(q[1]);v.position[2]=float(q[2]);
        v.halfWidthM=p.halfWidthM;v.rgba=p.rgba;v.sM=float(s);v.depthTolM=p.depthTolM;v.reserved=0.f;
        last=q;++n;
    }
    if(n<2)return 0;
    out[0]=out[1];out[n+1]=out[n];
    lengthM=float(s);return n+2;
}
// The landing pillar [c, c, t, t]: t = c + upLocal * height (LOCAL metres), half width 7 mm, the base colour fading to alpha 0 at the top,
// the marker's depth tolerance. Returns 4, or 0 (no pillar / no room / not finite).
inline unsigned buildPillarVertices(const ArcFrame& f,const DMat4& toLocal,const DVec3& upLocal,ArcVertex* out,unsigned capacity) noexcept {
    const auto& m=f.marker;
    if(!out||capacity<4||!(m.pillarHeightM>0.f))return 0;
    DVec3 up=upLocal;if(!normalizeD(up))return 0;
    const DVec3 c=transformPoint(toLocal,{m.centre[0],m.centre[1],m.centre[2]});
    const double h=m.pillarHeightM;const DVec3 t{c[0]+up[0]*h,c[1]+up[1]*h,c[2]+up[2]*h};
    for(double x:c)if(!std::isfinite(x))return 0;
    for(double x:t)if(!std::isfinite(x))return 0;
    const std::uint32_t top=m.pillarRgba&0x00ffffffu;
    const auto put=[&](ArcVertex& v,const DVec3& p,std::uint32_t rgba,double sM){
        v.position[0]=float(p[0]);v.position[1]=float(p[1]);v.position[2]=float(p[2]);v.halfWidthM=.007f;v.rgba=rgba;v.sM=float(sM);
        v.depthTolM=m.depthTolM;v.reserved=0.f;};
    put(out[0],c,m.pillarRgba,0);put(out[1],c,m.pillarRgba,0);put(out[2],t,top,h);put(out[3],t,top,h);
    return 4;
}
// Pixel rectangle of the segment a..b (LOCAL) widened by its drawn half width (max(halfWidth, minHalfPerM * distance), the shader's rule)
// in pixels at each endpoint, + 2 px; positive viewport, same conventions as segmentRect.
inline PixelRect arcSegmentRect(const DMat4& localClip,const ArcVertex& a,const ArcVertex& b,double minHalfPerM,std::uint32_t width,std::uint32_t height) noexcept {
    const double rx=std::sqrt(localClip.m[0]*localClip.m[0]+localClip.m[4]*localClip.m[4]+localClip.m[8]*localClip.m[8]);
    const double ry=std::sqrt(localClip.m[1]*localClip.m[1]+localClip.m[5]*localClip.m[5]+localClip.m[9]*localClip.m[9]);
    const double rw=std::sqrt(localClip.m[3]*localClip.m[3]+localClip.m[7]*localClip.m[7]+localClip.m[11]*localClip.m[11]);
    double pad=2;
    for(const ArcVertex* v:{&a,&b}){
        const auto c=transform(localClip,v->position[0],v->position[1],v->position[2]);
        const double w=std::max(c[3],1e-3);
        const double dist=rw>1e-12?w/rw:1.0;                           // metres (clip w = scale * view distance)
        const double half=std::max(double(v->halfWidthM),minHalfPerM*dist);
        const double pxPerM=std::max(rx*width,ry*height)*.5/w;
        if(std::isfinite(half*pxPerM))pad=std::max(pad,std::ceil(half*pxPerM)+2);else pad=double(std::max(width,height));
    }
    pad=std::min(pad,double(std::max(width,height)));
    return segmentRect(localClip,{a.position[0],a.position[1],a.position[2]},{b.position[0],b.position[1],b.position[2]},width,height,int(pad));
}
// The shader's anyPerpendicular (hands_arc_marker.vert) in double.
inline DVec3 anyPerpendicularD(const DVec3& v) noexcept {
    DVec3 r=std::abs(v[1])<.99?crossD(v,{0,1,0}):crossD(v,{1,0,0});normalizeD(r);return r;}
// Heading of `facing` in the marker shader's (t1 = anyPerpendicular(n), t2 = n x t1) basis, as a fraction of 2 pi in [0, 1).
inline float markerHeadingTurns(const DVec3& normalLocal,const DVec3& facingLocal) noexcept {
    DVec3 n=normalLocal;if(!normalizeD(n))return 0.f;
    const DVec3 t1=anyPerpendicularD(n),t2=crossD(n,t1);
    const double x=dotD(facingLocal,t1),y=dotD(facingLocal,t2);
    if(!(std::abs(x)+std::abs(y)>1e-9))return 0.f;
    double turns=std::atan2(y,x)/6.283185307179586;if(turns<0)turns+=1.0;
    return float(std::clamp(turns,0.0,.99998));
}
// Window-depth epsilon of a marker: base + z_w(C) - z_w(C pulled min(tol, 0.9 |eye - C|) toward the eye), with the plan's own matrix
// (forward Z, convention-independent otherwise). >= base.
inline float markerDepthEpsilon(const DMat4& localClip,const DVec3& centre,const DVec3& eye,float tolM,float baseEpsilon) noexcept {
    DVec3 d{eye[0]-centre[0],eye[1]-centre[1],eye[2]-centre[2]};const double L=std::sqrt(dotD(d,d));
    if(!(L>1e-6)||!(tolM>0.f))return baseEpsilon;
    const double t=std::min(double(tolM),.9*L);
    const DVec3 p{centre[0]+d[0]/L*t,centre[1]+d[1]/L*t,centre[2]+d[2]/L*t};
    const auto a=transform(localClip,centre[0],centre[1],centre[2]),b=transform(localClip,p[0],p[1],p[2]);
    if(!(a[3]>1e-9)||!(b[3]>1e-9))return baseEpsilon;
    const double e=a[2]/a[3]-b[2]/b[3];
    return std::isfinite(e)&&e>0?float(double(baseEpsilon)+e):baseEpsilon;
}
} // namespace rdrvr::vr::hands
