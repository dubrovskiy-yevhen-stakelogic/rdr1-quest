#pragma once
// RIDE STEADY link: VrMotionActors (main thread) publishes the horse under the eye; VrBody's hook A shifts that actor's skeleton
// with the smoothed saddle height (VrBody.h, namespace steady).
// HORSE PARTS: AppSW gives the mount the camera rig's motion
// (SpaceWarp::prepareActorPair), so the animated neck / head nod is extrapolated as rigid and doubles.
// The parts (debug.rdrvr.vr.riding.horseparts 1, VrBody steady::horseChains): the 4 legs, the neck and the head as up to 16
// capsules along the skeleton's own CHAINS (parents, chosen once per skeleton data while the horse stands), WORLD space of the hook call
// (the drawn pose after the steady shifts), each carried by its real upper bone (bone = engine index: a re-selection is a layout change).
// VrMotionActors attaches them to the mount's ActorBones; SpaceWarp re-places them on the drawn rig (ActorSet::rig) and keeps their motion
// relative to the root.
#include <array>
#include <atomic>
#include <cstdint>
namespace rdrvr::vr::steadylink {
inline std::atomic<std::uintptr_t> mountActor{0}; // the race-1 actor under the eye (0 = none)
// HORSE VIEW THIRD PERSON: the riding camera's offset (engine world, metres) written by GameXr ridingAnchorSample; the BODY frame
// takes it back off the controllers / HMD so John's arms and seat stay on John (not 3 m behind with the camera)
inline std::atomic<float> thirdX{0.f},thirdY{0.f},thirdZ{0.f};inline std::atomic<std::uint64_t> thirdNs{0};
// the first stamp (anchor key) of the current third-person run; GameXr's eyes show John only for a plan whose anchor key
// is in [thirdSinceNs, thirdNs] (written before thirdNs's release store)
inline std::atomic<std::uint64_t> thirdSinceNs{0};
inline bool thirdActive(std::uint64_t now) noexcept {const auto t=thirdNs.load(std::memory_order_relaxed);return t&&now>=t&&now-t<300'000'000ull;}
inline constexpr unsigned maxHorseParts=16;
struct HorsePart {float a[3]{},b[3]{};float radius{};float m[12]{};std::int16_t bone{-1};}; // world (engine units); m rows a, b, c, d (ActorPose layout)
struct HorseParts {std::uint64_t ns{};std::uintptr_t actor{};unsigned count{};std::array<HorsePart,maxHorseParts> part{};};
inline constexpr std::uint32_t horsePartLayout=0xFFFF0002u; // = runtime/SpaceWarpActors.h horsePartLayout (handPartLayout = 0xFFFF0001)
inline std::atomic<bool> partsLock{false};inline HorseParts partsBuf{};
inline std::atomic<std::uint64_t> partsPublished{0},partsBusy{0},partsUsed{0},partsStale{0};
// Writer (hook A, any update thread): skips when the reader holds the buffer. Reader (main thread): a few tries.
inline void publishParts(const HorseParts& p) noexcept {
    bool f=false;if(!partsLock.compare_exchange_strong(f,true,std::memory_order_acquire)){partsBusy.fetch_add(1,std::memory_order_relaxed);return;}
    partsBuf=p;partsLock.store(false,std::memory_order_release);partsPublished.fetch_add(1,std::memory_order_relaxed);
}
inline bool readParts(HorseParts& out) noexcept {
    for(int i=0;i<64;++i){bool f=false;if(partsLock.compare_exchange_weak(f,true,std::memory_order_acquire)){out=partsBuf;partsLock.store(false,std::memory_order_release);return true;}}
    return false;
}
}
