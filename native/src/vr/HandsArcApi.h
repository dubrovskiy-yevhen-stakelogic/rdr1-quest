#pragma once
// ARC: the producer side of the hands-pass arc channels.
// Vulkan-free on purpose (VrArc.h and the feature ticks include it without <vulkan/vulkan.h>); HandsPass.h includes it, the definitions
// live in HandsPass.cpp.
//
//  * World frames: only from the channel's main-thread TICK (VrMainThread), key = arc::frameContext(now).key() of the same tick (never 0).
//    Local frames: the THROW channel's input brain (reserved; no producer).
//  * ONE writer thread per channel: the first publishArc / clearArc binds it; another thread's calls are dropped and counted (wrongThread).
//  * publishArc stamps frame.publishedNs itself (render clock); invalid frames are dropped and counted (rejected). A frame is drawn at most
//    arcMaxAgeNs (100 ms) after its publication.
//  * clearArc is an immediate cut: nothing published before it is ever drawn again, whatever its key. Call it on commit, cancel, mode
//    change, BEFORE a warp, and whenever the world / input / hand tracking is lost.
//  * Gate on arcStatus(): Failed / Killed, or Building for more than 3 s -> the feature's fallback (TELEPORT -> FREE "NO ARC", LASSO ->
//    native aim "NO ARC"); Suppressed -> hide only.
#include "HandsMath.h"
#include <cstdint>

namespace rdrvr::vr::hands {
void publishArc(ArcChannel channel,const ArcFrame& frame) noexcept;
void clearArc(ArcChannel channel) noexcept;
// Any thread: this channel may show an arc soon -> the pass prewarms the arc pipelines of the current eye format while it is otherwise idle.
void armArc(ArcChannel channel,bool armed) noexcept;
// RELOAD: the reload overlay (input thread, one writer). Count 0 hides it; the pass keeps itself alive only
// while the last publication had visible items (stamped with the render clock, <= 100 ms).
void publishReloadOverlay(const ReloadOverlay& overlay) noexcept;
enum class ArcStatus : std::uint8_t {Off,Building,Ready,Failed,Killed,Suppressed};
inline const char* arcStatusName(ArcStatus s) noexcept {
    switch(s){case ArcStatus::Off:return "OFF";case ArcStatus::Building:return "BUILDING";case ArcStatus::Ready:return "READY";
    case ArcStatus::Failed:return "FAILED";case ArcStatus::Killed:return "KILLED";case ArcStatus::Suppressed:return "SUPPRESSED";}
    return "UNKNOWN";
}
ArcStatus arcStatus() noexcept;           // any thread, atomics only
std::uint64_t arcStatusAgeMs() noexcept;  // time spent in the current status
const char* arcStatusNote() noexcept;     // "OK", "BUILDING", "ARC_PIPELINE", "ARC_SHADER", "ARC_RING", "PIPELINE_TIMEOUT", "KILLED", ...
// Producer-side counters of a channel (vr.arc.channels mirrors them).
struct ArcChannelStats {std::uint64_t published{},cleared{},hides{},wrongThread{},rejected{},unkeyed{},killed{};};
ArcChannelStats arcChannelStats(ArcChannel channel) noexcept;
// AppSW (runtime::spaceWarpControl.synthesizing(), set by GameXr): synthesized frames are counted and drawn with solid, dimmer arcs.
void setSynthesizingProbe(bool (*probe)() noexcept) noexcept;
// Host tests. kill: 1 = arcs killed at the next configure; xray: -1 property / 0 / 1; failArcPipelines: 1 = every arc pipeline build
// throws (ARC_PIPELINE); depthLever: -1 property / 0 NONE for arc-only passes / 1 auto / 2 COPY.
void setArcDebugForTests(int kill,int xray,int failArcPipelines,int depthLever) noexcept;
void resetArcWritersForTests() noexcept;
} // namespace rdrvr::vr::hands
