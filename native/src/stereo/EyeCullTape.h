#pragma once
// EYE CULL B: per-draw eye bits from bodyDraw (EyeCull.h decide) to the left-eye draw hooks
// (CameraIntegration.h cameraDraw*), the stereo tape (StereoReplayInternal.h Header.pad:
// now set by StereoReplay.cpp recordDraw* BEFORE publishTape, so a streaming worker never reads it unset) and the right-eye replay
// loops (StereoReplay.cpp inline, StereoReplayAsync.cpp secondaries / worker). No engine or Vulkan headers: the replay TUs and the
// host tests include it.
// Pad bits (u16):
//   DropLeft   the left eye (native stock stage) skips the live draw: its object lies wholly outside the LEFT eye frustum
//   DropRight  the right eye (tape replay) skips the draw: wholly outside the RIGHT eye. State ops around it are still replayed and
//              set 0 stays pending for the next replayed draw (the same path as the live-mask skips).
//   ColorStage counters only (stock colour stage, else depth); Swap = diagnostic mode 3 (the bits are the OTHER eye's: visible holes).
// `current` = the pad of the DrawCommon object being drawn on THIS thread (Scope from bodyDraw: 0 outside any object, 0 for nested /
// uncounted / excluded objects). cameraDraw* reads it only for a draw recorded into a tape (recordingFor(c): this thread records the
// left stage), so no other thread ever sees or changes it. Never a pad on a draw with instanceCount > 1 (instanced batches are drawn
// by InstanceList::DrawInstances 0x2a27e9c from renDrawBucketContainer::Draw 0x2a294e4 / 0x2a29588, outside DrawCommon, so they carry
// no pad anyway: belt and braces). The left eye drops a draw only when recordDraw* put it into the tape: either the right eye replays
// that tape (the draw kept unless DropRight) or the replay is declined and the right eye is drawn natively (stereo pass: no pads).
#include <array>
#include <atomic>
#include <cstdint>

namespace rdrvr::engine::eyecull::tape {
enum Pad : std::uint16_t {DropLeft=0x1,DropRight=0x2,ColorStage=0x4,Swap=0x8};
inline constexpr std::uint16_t dropBits=DropLeft|DropRight;
#if defined(_MSC_VER) && !defined(__clang__)
inline thread_local std::uint16_t current{}; // host test build: MSVC rejects constinit on thread_local (C2127)
#else
inline constinit thread_local std::uint16_t current{};
#endif
// Draw-level counters (cumulative; [0] depth, [1] colour stage). padDraws = taped draws with a drop bit; leftDropped = live left
// draws skipped; rightDropped = replayed right draws skipped (inline + worker) in replays that were USED: the
// replay loops count into a per-replay Pending (inline: local; worker: RecordCursor) committed only when the replay completes
// (inline `return true` / spliceJob Done), so a cancelled / RING_PRESSURE / CLONE_FLAGGED / declined / faulted job, whose stage is
// then replayed again or drawn natively, adds nothing; multiInstance = draws of a flagged object with instanceCount > 1 (never
// dropped); notRecorded = flagged draws that did not reach the tape (never dropped).
struct Counters {
    std::array<std::atomic<std::uint64_t>,2> padDraws{},leftDropped{},rightDropped{};
    std::atomic<std::uint64_t> multiInstance{},notRecorded{};
};
inline Counters counters;
inline unsigned stageOf(std::uint16_t pad) noexcept {return (pad&ColorStage)?1u:0u;}
// cameraDraw* (the recording thread only): the pad of this draw.
inline std::uint16_t padFor(std::uint32_t instances) noexcept {
    const std::uint16_t p=current;if(!(p&dropBits))return 0;
    if(instances>1){counters.multiInstance.fetch_add(1,std::memory_order_relaxed);return 0;}
    return p;
}
// cameraDraw*, after recordDraw*: true = the left eye skips the live draw (it is in the tape). recorded = recordDraw* result.
inline bool dropLeft(std::uint16_t pad,bool recorded) noexcept {
    if(!pad)return false;
    if(!recorded){counters.notRecorded.fetch_add(1,std::memory_order_relaxed);return false;}
    const unsigned s=stageOf(pad);counters.padDraws[s].fetch_add(1,std::memory_order_relaxed);
    if(!(pad&DropLeft))return false;
    counters.leftDropped[s].fetch_add(1,std::memory_order_relaxed);return true;
}
// Replay loops (any thread), before the rewrite of the draw: true = the right eye skips this tape draw (counted into `pending`).
using Pending=std::array<std::uint32_t,2>; // Right drops of one replay [depth, colour], not yet in counters
inline bool dropRight(std::uint16_t pad,Pending& pending) noexcept {
    if(!(pad&DropRight))return false;
    ++pending[stageOf(pad)];return true;
}
// The replay was used (inline success / spliceJob Done): its drops become rightDropped.
inline void commitRight(const Pending& pending) noexcept {
    for(unsigned s=0;s<2;++s)if(pending[s])counters.rightDropped[s].fetch_add(pending[s],std::memory_order_relaxed);
}
// bodyDraw: the pad of every draw this DrawCommon issues (LIFO restore; on=false: untouched, one branch).
class Scope {
    std::uint16_t saved_{};bool on_{};
public:
    Scope(std::uint16_t pad,bool on) noexcept {if(!on)return;saved_=current;current=pad;on_=true;}
    ~Scope(){if(on_)current=saved_;}
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
inline void resetForTests() noexcept {
    for(auto* a:{&counters.padDraws,&counters.leftDropped,&counters.rightDropped})for(auto& v:*a)v=0;
    counters.multiInstance=0;counters.notRecorded=0;current=0;
}
} // namespace rdrvr::engine::eyecull::tape
