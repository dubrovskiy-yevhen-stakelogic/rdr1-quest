#pragma once
// Body motion link (AppSW motion vectors, runtime/SpaceWarp.h prepareActorPair): VrBody hook A (John, after applyBody) -> VrMotionActors
// (main-thread tick).
//   * the RIG MAP: the OpenXR LOCAL -> world map of the frame hook A just posed (bodyik::Frame::localToWorld, the map the VR arms are
//     placed with). The tick stores it in its ActorSet (ActorSet::rig): SpaceWarp re-places that snapshot's rig actors (player body,
//     ridden horse with parts) onto the camera rig of the frame a pair DREW - a snapshot read one tick off the drawn frame is 0.2-0.3 m
//     away at a run / gallop, more than a limb capsule.
//   * the player BODY (debug.rdrvr.mvbody 1): John's skeleton parts in world (runtime/SpaceWarpBones.h makeParts over the rig's named
//     joints, the head only in THIRD PERSON - first person collapses it around the eyes), the tick's BoneSet entry of the player body
//     (guid playerBodyGuid, layout playerBodyLayout).
// Props (re-read at 1 Hz by the mvactors tick): debug.rdrvr.mvbody (default 0 = off), debug.rdrvr.vr.riding.horseparts (default 0 = off,
// VrRideSteadyLink.h). Hook A publishes only while one of them is on.
#include "runtime/SpaceWarpActors.h" // ActorBones, playerBodyGuid / playerBodyLayout
#include <atomic>
#include <cstdint>
namespace rdrvr::vr::bodylink {
namespace sw=::rdrvr::runtime::spacewarp;
inline std::atomic<int> bodyProp{0},horseProp{0}; // debug.rdrvr.mvbody, debug.rdrvr.vr.riding.horseparts (mvactors tick, 1 Hz)
inline bool wanted() noexcept {return bodyProp.load(std::memory_order_relaxed)!=0||horseProp.load(std::memory_order_relaxed)!=0;}
// map: rows a, b, c, d (ActorPose layout, scale = engine units per metre included); parts: world, count 0 = map only
struct Record {std::uint64_t ns{},serial{};float map[12]{};bool third{};sw::ActorBones parts{};};
inline std::atomic<bool> lock{false};inline Record buf{};
// hook A: published / skipped (the reader held the buffer) / no map this frame / no usable parts; tick: maps + bodies used, stale records,
// NPC part entries given up for the body (full BoneSet)
inline std::atomic<std::uint64_t> published{0},busy{0},noMap{0},noParts{0},mapsUsed{0},bodiesUsed{0},stale{0},dropNpc{0};
inline std::atomic<unsigned> lastParts{0};
// Writer (hook A, any update thread): skips when the reader holds the buffer. Reader (main thread): a few tries.
inline void publish(const Record& r) noexcept {
    bool f=false;if(!lock.compare_exchange_strong(f,true,std::memory_order_acquire)){busy.fetch_add(1,std::memory_order_relaxed);return;}
    buf=r;lock.store(false,std::memory_order_release);published.fetch_add(1,std::memory_order_relaxed);
}
inline bool read(Record& out) noexcept {
    for(int i=0;i<64;++i){bool f=false;if(lock.compare_exchange_weak(f,true,std::memory_order_acquire)){out=buf;lock.store(false,std::memory_order_release);return true;}}
    return false;
}
} // namespace rdrvr::vr::bodylink
