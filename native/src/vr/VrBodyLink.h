#pragma once
// LOCO <-> CAMERA: the tiny shared contract between free locomotion (VrLocomotion.h, owner LOCO) and the body
// camera / roomscale (VrCamera.h, owner CAMERA). Header-only, lock-free, no engine access.
//
// Conventions (engine world space, metres, Y up):
//   yaw psi (radians)   forward = ( sin psi, 0, cos psi ),  right = ( -cos psi, 0, sin psi )  (right = forward x up)
//                       = the view-yaw convention (f = -column 2 of the eye-to-world matrix, psi = atan2(f.x, f.z)).
//   XZ vectors          {x, z} pairs in world metres.
//
// Data flow per game frame (all on the engine main thread except the report reads):
//   CAMERA  publishHeadYaw(psi, ns)        whenever it has a current rendered view: camera yaw + HMD yaw (pitch/roll dropped).
//   LOCO    reads headYaw() when NativePadDriver builds the stick (mapMovement) -> stick velocity v_vr (world XZ).
//   CAMERA  requestHead(x, z) in PostCameraUpdate of frame N: roomscale displacement for frame N+1's physics (world metres,
//           already rotated, gain/per-frame/per-second limited by CAMERA; hard cap headRequestMaxMeters here). One request per
//           frame; a newer request replaces an unconsumed one.
//   LOCO    consumes it once (by id) at the FIRST physics substep of frame N+1 and spreads it evenly over the N substeps (a
//           velocity, like the stick), then publishes a FrameRecord (what was commanded, flags, which head id was applied).
//   CAMERA  after physics (PostCameraUpdate of frame N+1) credits the capsule's TOTAL measured XZ motion since its previous
//           apply to the body point B (B += R^-1 * dC) while roomscale owns the capsule (record applied, no stick).
// split() below (per-substep attribution) is diagnostic only: it assumes a substep's command moves the capsule within that
// substep, which is false on this engine:
// mvrMoverInst::Update's impulse shows in the capsule position only one substep later, so split() credited the head part
// with the WRONG SIGN -> B walked away from the neck -> lean pinned at the 0.30 cap -> endless requests (34 m run-away).
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace rdrvr::vr::body {

inline constexpr std::uint64_t headYawMaxAgeNs=200'000'000ull; // 200 ms: older yaw = no direction (LOCO goes transparent)
inline constexpr float headRequestMaxMeters=0.10f;              // hard cap per frame (CAMERA sends <= 0.05)

// ---- generic single-writer seqlock over a trivially copyable struct (atomic words: no data race, never blocks) ----
template<class T> class Seqlock {
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr std::size_t words=(sizeof(T)+7)/8;
    std::atomic<std::uint32_t> seq_{0};
    std::array<std::atomic<std::uint64_t>,words> data_{};
public:
    void store(const T& value) noexcept { // one writer at a time (main thread)
        std::array<std::uint64_t,words> raw{};std::memcpy(raw.data(),&value,sizeof(T));
        const auto s=seq_.load(std::memory_order_relaxed);
        seq_.store(s+1,std::memory_order_relaxed);std::atomic_thread_fence(std::memory_order_release);
        for(std::size_t i=0;i<words;++i)data_[i].store(raw[i],std::memory_order_relaxed);
        seq_.store(s+2,std::memory_order_release);
    }
    bool load(T& out) const noexcept { // false = never written or a write kept racing (4 tries)
        for(int attempt=0;attempt<4;++attempt){
            const auto a=seq_.load(std::memory_order_acquire);
            if(a&1u)continue;
            std::array<std::uint64_t,words> raw{};
            for(std::size_t i=0;i<words;++i)raw[i]=data_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(seq_.load(std::memory_order_relaxed)!=a)continue;
            if(!a)return false;
            std::memcpy(&out,raw.data(),sizeof(T));return true;
        }
        return false;
    }
};

// ---- CAMERA -> LOCO: head yaw ----
struct HeadYaw {float yaw{};std::uint64_t ns{};};
inline Seqlock<HeadYaw> headYawWorld;
inline void publishHeadYaw(float yawRadians,std::uint64_t nowNs) noexcept {
    if(!std::isfinite(yawRadians)||!nowNs)return;
    headYawWorld.store({yawRadians,nowNs});
}
// true = a yaw no older than maxAge exists (a yaw stamped slightly after `now` by another thread counts as fresh).
inline bool headYaw(float& yaw,std::uint64_t nowNs,std::uint64_t maxAgeNs=headYawMaxAgeNs) noexcept {
    HeadYaw h{};if(!headYawWorld.load(h)||!h.ns)return false;
    if(nowNs>h.ns&&nowNs-h.ns>maxAgeNs)return false;
    yaw=h.yaw;return true;
}

// ---- CAMERA -> LOCO: roomscale head displacement for the next physics frame ----
struct HeadRequest {float x{},z{};std::uint32_t id{};std::uint64_t ns{};};
inline Seqlock<HeadRequest> headRequest;
inline std::atomic<std::uint32_t> headRequestId{0};
// Returns the id of this request (non-zero). Non-finite input is ignored (returns 0); the length is clamped to headRequestMaxMeters.
inline std::uint32_t requestHead(float x,float z,std::uint64_t nowNs) noexcept {
    if(!std::isfinite(x)||!std::isfinite(z))return 0;
    const float l=std::hypot(x,z);if(l>headRequestMaxMeters){x*=headRequestMaxMeters/l;z*=headRequestMaxMeters/l;}
    std::uint32_t id=headRequestId.fetch_add(1,std::memory_order_relaxed)+1;if(!id)id=headRequestId.fetch_add(1,std::memory_order_relaxed)+1;
    headRequest.store({x,z,id,nowNs});return id;
}
// LOCO: the newest request whose id differs from `lastId` (then lastId = its id); false = nothing new.
inline bool takeHead(std::uint32_t& lastId,HeadRequest& out) noexcept {
    HeadRequest r{};if(!headRequest.load(r)||!r.id||r.id==lastId)return false;
    lastId=r.id;out=r;return true;
}

// ---- LOCO -> CAMERA: what one physics frame commanded ----
struct FrameRecord {
    std::uint32_t frame{};       // LOCO physics-frame counter (1, 2, ...)
    std::uint32_t headId{};      // HeadRequest id applied on the last substep (0 = none)
    std::int32_t steps{};        // N = sagWorld::sm_PhysicsStepCount seen this frame
    std::uint32_t flags{};       // record* bits below
    float c0[2]{};               // capsule XZ at entry of substep 0
    float c1[2]{};               // capsule XZ at entry of the last substep (== c0 when N == 1)
    float stickSub[2]{};         // stick displacement commanded per substep (v_vr * dt_sub)
    float head[2]{};             // head displacement commanded over the whole frame (spread over the N substeps)
    std::uint64_t ns{};          // monotonic stamp of the last substep
};
// applied: LOCO owned the capsule's XZ this frame (stick/head motion or the zeroed release hold), integrated: substep entry
// positions are integrated (diagnostic), stick: a non-zero stick displacement was commanded, head: a head part was applied.
inline constexpr std::uint32_t recordApplied=1u,recordIntegrated=2u,recordStick=4u,recordHead=8u;
inline Seqlock<FrameRecord> frameRecord;
inline void publishRecord(const FrameRecord& r) noexcept {frameRecord.store(r);}
inline bool latestRecord(FrameRecord& r) noexcept {return frameRecord.load(r);}

struct Achieved {float stick[2]{};float head[2]{};bool proportional{};};
// DIAGNOSTIC ONLY - nothing credits B with it (see the data-flow note at the top: the same-substep effect is
// false, the engine applies a substep's command one substep later).
// Split the frame's achieved capsule motion (C2 = capsule XZ after the last physics substep) into the stick part and the
// head part. N >= 2 with integrated substeps: the stick efficacy measured on substeps 0..N-2 ((C1-C0)/(N-1), walls included)
// is removed from the last substep's motion; the rest is head, length-limited to what was requested (external pushes are
// never credited to the head). Otherwise (N == 1 or entry positions not integrated): proportional, f = clamp(a.c/|c|^2, 0, 1).
inline Achieved split(const FrameRecord& r,float c2x,float c2z) noexcept {
    Achieved out{};
    const float ax=c2x-r.c0[0],az=c2z-r.c0[1];
    if(!std::isfinite(ax)||!std::isfinite(az))return out;
    const float hx=r.head[0],hz=r.head[1],hl=std::hypot(hx,hz);
    if(r.steps>=2&&(r.flags&recordIntegrated)){
        const float n=float(r.steps-1);
        const float px=(r.c1[0]-r.c0[0])/n,pz=(r.c1[1]-r.c0[1])/n; // achieved stick per substep
        float lx=(c2x-r.c1[0])-px,lz=(c2z-r.c1[1])-pz;
        const float ll=std::hypot(lx,lz);
        if(hl<1e-6f)lx=lz=0;
        else if(ll>hl){lx*=hl/ll;lz*=hl/ll;}
        out.head[0]=lx;out.head[1]=lz;
    }else{
        out.proportional=true;
        const float n=float(r.steps>0?r.steps:1);
        const float cx=r.stickSub[0]*n+hx,cz=r.stickSub[1]*n+hz,cc=cx*cx+cz*cz;
        if(hl>=1e-6f&&cc>1e-12f){
            float f=(ax*cx+az*cz)/cc;f=f<0?0:f>1?1:f;
            out.head[0]=hx*f;out.head[1]=hz*f;
        }
    }
    out.stick[0]=ax-out.head[0];out.stick[1]=az-out.head[1];
    return out;
}

// Yaw helpers (shared convention).
inline void yawBasis(float yaw,float forward[2],float right[2]) noexcept {
    const float s=std::sin(yaw),c=std::cos(yaw);
    forward[0]=s;forward[1]=c;right[0]=-c;right[1]=s;
}

} // namespace rdrvr::vr::body
