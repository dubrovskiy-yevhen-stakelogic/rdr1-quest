#pragma once
#include "StereoMailbox.h"
#include "TrackedViews.h"
#include "SubmitCompletion.h"
#include "SpaceWarp.h"
#include <thread>
namespace rdrvr::runtime {
struct CaptureFrame {
    FrameContext frame{};
    std::array<Pose,2> poses{};
    std::array<Fov,2> fov{};
    std::array<Extent,2> extents{};
    std::array<VkFormat,2> formats{};
    SourceEncoding encoding{SourceEncoding::Linear};
    StereoReticle reticle{};
};
struct PassImage {
    VkImage image{};
    Extent extent{};
    VkFormat format{};
    VkImageLayout layout{};
    VkImageUsageFlags usage{};
    VkSampleCountFlagBits samples{};
    std::uint32_t family{},arrayLayer{},arrayLayers{};
};
// Copies each finished eye before the engine may reuse its render target. No
// engine layout/state writes, queue submits, command-buffer begin/end or waits
// occur in recordEye. The caller MUST be outside a render pass and own a primary
// recording command buffer on the declared game queue family. The exact-engine
// adapter must supply verified usage/sample/layer metadata, never guessed flags.
//
// A pair is normally published IN FLIGHT from submitted(): the
// mailbox copy is queued on the pass's own queue right behind the pass, then
// the early marker, so the marker fence covers pass + copy. The slot stays
// pending (retiredFrame unchanged) until that marker (or the copy ticket) is
// observed signalled: retirement is always GPU-proven.
class StereoCaptureSlot {
    mutable std::mutex mutex_;
    VkDevice device_{};VkPhysicalDevice physical_{};std::uint32_t family_{};
    std::vector<VkQueue> queues_;
    VkQueue queue_{};
    std::thread::id owner_{};
    std::array<OwnedImage,2> images_{};
    bool sampled_{}; // images_ also carry SAMPLED usage (mailbox anti-aliasing source)
    CaptureFrame latest_{};
    std::array<bool,2> initialized_{};
    unsigned recorded_{},submitted_{};
    std::array<VkCommandBuffer,2> commands_{};
    VkCommandBuffer passCommand_{};
    std::array<SubmitCompletion,3> completion_{}; // two eyes and enclosing pass
    std::uint64_t crossQueueSubmissions_{},unprovenPresents_{};
    bool passSubmitted_{},passDiscarded_{};
    bool active_{},accepting_{},poisoned_{},cancelled_{};
    std::uint64_t lastFrame_{},retiredFrame_{},resourceEpoch_{},prepared_{},published_{},discarded_{},rejected_{};
    bool earlyCompletion_{},externalEpoch_{};VkFence earlyFence_{};VkQueue earlyQueue_{};
    std::thread::id earlyThread_{};std::uint64_t earlyFrame_{},earlyMarkers_{},earlyPublished_{},earlyNotReady_{},earlyWrongThread_{},earlyFailed_{},earlyPolls_{},earlyReady_{},earlyWrongQueue_{};
    // In-flight publication state.
    StereoMailbox* mailbox_{};StereoCaptureTicket ticket_{},lastTicket_{};
    bool inFlight_{},markerCoversCopy_{},earlySubmitted_{},publishRejected_{},lastTicketValid_{};
    std::uint64_t inFlightPublished_{},inFlightRejected_{},retiredInFlight_{},throttleWaits_{},throttleTimeouts_{},discardWaits_{},reuseWaits_{};
    // Slot-owned fence queued on the present queue behind the batch
    // that consumed the covering waits, for pairs that cannot be published and
    // have no marker (retired by polls instead of a render-thread wait).
    VkFence retireFence_{};std::uint64_t retireFenceFrame_{};bool retireFenceSubmitted_{};
    std::uint64_t retireFences_{},retiredDiscarded_{},partialMarkers_{};
    // AppSW: small per-eye depth (recommended motion-vector rect) blitted from the eye depth in the pass.
    // SAMPLED usage + DEPTH-aspect view, left in SHADER_READ_ONLY_OPTIMAL for the mailbox downsample pass.
    std::array<OwnedImage,2> warpDepth_{};VkFormat warpFormat_{};Extent warpExtent_{};unsigned warpRecorded_{};
    SpaceWarpFrame warpFrame_{};bool warpFrameSet_{};
    std::uint64_t warpBlits_{},warpCopies_{},warpRejected_{},warpCreates_{},warpFrames_{};
    // warpDepth GPU lane, capture part (timestamps around each eye's depth copy, read at retirement).
    VkQueryPool warpQueries_{};double warpPeriod_{};std::uint64_t warpMask_{},warpTicket_{},warpTimingUnavailable_{};unsigned warpTimed_{};bool warpQueryTried_{};
    // Depth direct: per-slot 420x440 depth downsampled straight from the engine eye depth (SpaceWarp.h SpaceWarpDirect).
    // warpDirect_: this pair's path, decided at its first recorded eye (both eyes alike). warpFullTicket_: the last publication
    // whose warp source was the full-size warpDepth_ pair; once that copy completed, direct pairs release those images.
    SpaceWarpDirect direct_{};bool warpDirect_{};StereoCaptureTicket warpFullTicket_{};bool warpFullTicketValid_{};
    std::uint64_t warpDirectEyes_{},warpDirectFallbacks_{},warpFullReleases_{};int warpDirectLastReason_{-1};
    bool recordDirect(VkCommandBuffer,unsigned eye,const PassImage&,unsigned& why);
    // depthdual (debug.rdrvr.spacewarp.depthdual, one pair): a direct pair that also records the full-size copy (warpDepth_)
    // so the mailbox can dump both 420x440 results of the same frame (SpaceWarp.h dumpProducer). warpDualRecorded_: eyes copied.
    bool warpDual_{};unsigned warpDualRecorded_{};std::uint64_t warpDualPairs_{},warpDualFailures_{};
    bool recordCopy(VkCommandBuffer,unsigned eye,const PassImage&,bool time); // the full-size copy path of one eye (caller: mutex_)
    // MAILBOX DIRECT (debug.rdrvr.mailbox.direct 1, re-read ~1 Hz in recordEye; 0 = images_ exactly as before; 2 = 1 + bounded
    // busy retry in reserveDirect, A/B only): recordEye(eye 0)
    // reserves a mailbox ring slot (StereoMailbox::reserveDirect) and both eyes are copied straight into it, so StereoMailbox::capture skips
    // its own full-eye copy. md_: this pair's reservation, until capture() consumed it (publishInFlight / finish) or, unconsumed, until
    // retire() hands it back (the pass is GPU-proven complete or never submitted). mdEyes_: eyes written into the slot (images_ untouched,
    // initialized_ unchanged). mdSwap_: SWAP EYES test fixed at the reservation (slot eye = swapped eye; submission()/warpSource() follow it).
    StereoDirectSlot md_{};StereoMailbox* mdMailbox_{};unsigned mdEyes_{};bool mdSwap_{};int mdMode_{};
    std::uint64_t mdPairs_{},mdRefused_{},mdNoMailbox_{},mdConsumed_{},mdReleases_{};
    bool swapEyes() const noexcept;
    const StereoDirectSlot* directPair() const noexcept {return md_.valid&&mdEyes_==3u?&md_:nullptr;}
    void releaseDirect() noexcept;
    void notePublished(const StereoCaptureTicket&) noexcept;
    bool warpQueryPool() noexcept;
    void readWarpTiming() noexcept;
    const SpaceWarpSource* warpSource(SpaceWarpSource&) const noexcept;
    RenderSubmission submission(VkFence) const;
    bool sameQueue(VkQueue) const;
    void retire() noexcept;
    void discardRetire() noexcept;
    bool retireReady() noexcept;
    bool retireDiscarded() noexcept;
    bool queueRetireFence(VkQueue) noexcept;
    bool publishInFlight(VkQueue,VkFence,StereoMailbox&,std::uint64_t,bool markerAfter);
    bool finish(VkQueue,VkFence,StereoMailbox&,std::uint64_t);
public:
    static constexpr std::uint64_t discardWaitNs=100'000'000,reuseWaitNs=100'000'000;
    // The declared present queue and actual source submission queue can differ.
    // A marker is retired only by completeSubmitted on its own serialized queue.
    enum class ArmReason {Inactive,Ready,Unhealthy,PresentQueue,Recording,Partial,Command,Marker,SubmitQueue,Thread};
    struct PresentState {FrameContext frame{};bool healthy{},active{},armed{};VkQueue presentQueue{},markerQueue{};ArmReason reason{ArmReason::Inactive};};
    PresentState presentState(VkQueue) const noexcept;
    void bind(const XrGraphicsBindingVulkan2KHR&,bool earlyCompletion=false,bool externalEpoch=false);
    std::uint64_t pendingFrame()const noexcept {std::lock_guard guard(mutex_);return active_?latest_.frame.renderFrameId:0;}
    // Called before recording either eye. Resources cannot be replaced until
    // the preceding frame's copies have retired through completePresent.
    // Supplying passCommand retains the engine pass even if drawing fails before
    // its first copy. It must already be recording and cover ALL target use.
    bool beginFrame(VkQueue,const CaptureFrame&,VkCommandBuffer passCommand=VK_NULL_HANDLE);
    void cancelFrame(std::uint64_t renderFrameId) noexcept;
    std::uint64_t retiredFrame() const noexcept;
    bool recordEye(VkCommandBuffer,unsigned,const PassImage&,StereoMailbox* mailbox=nullptr); // mailbox = the MAILBOX DIRECT target
    // AppSW: after recordEye(eye) in the same pass command buffer: the eye depth (layout as
    // tracked by the engine) is blitted (nearest) or copied into warpDepth_[eye] and restored.
    // Transfer commands only; a rejection never affects the colour pair. Skipped while the AppSW
    // budget guard suspends the space-warp info (spaceWarpControl.producing()); timestamps around
    // the copy (DETAIL).
    // With debug.rdrvr.spacewarp.depthdirect 1 (default) the eye depth (SAMPLED usage) is instead downsampled in a
    // depth-only pass into direct_ (MV rect) and the engine's bound state is re-emitted (setEngineRestore); any miss takes the
    // copy above for the whole pair (a right eye that cannot follow a direct left eye leaves the pair without a source).
    bool recordDepth(VkCommandBuffer,unsigned eye,const PassImage& depth);
    void setWarpFrame(const SpaceWarpFrame&) noexcept;
    // Invoked after the intercepted stock vkQueueSubmit returns. A present
    // fence from an older frame cannot retire commands that were only recorded.
    // Flags: passObserved (this call carried the pass), publishedNow (in flight).
    static constexpr unsigned passObserved=1,publishedNow=2;
    unsigned submitted(VkQueue,std::uint32_t,const VkSubmitInfo*,VkResult,StereoMailbox* mailbox=nullptr,std::uint64_t nowNs=0) noexcept;
    // Does this submission carry this slot's recorded (not yet submitted) stereo pass command buffer? (read-only)
    bool carriesPass(VkQueue,std::uint32_t,const VkSubmitInfo*) noexcept;
    // Begin/reset/free/pool destruction invalidates an earlier recording even
    // when the VkCommandBuffer handle is reused unchanged. Drop the pair; retain
    // submitted copies until the present fence establishes their GPU completion.
    void commandInvalidated(VkCommandBuffer) noexcept;
    // Caller has consumed the current stock present semaphores exactly once on
    // q in the batch that signals fence. A signalled fence publishes and retires
    // (classic). A pending fence publishes in flight on q (the copy is queued
    // behind that batch) and retires once the copy is observed complete.
    // Cross-queue commands additionally require a matching semaphore in
    // completedWaits from their exact successful submission.
    bool completePresent(VkQueue,VkFence,StereoMailbox&,std::uint64_t nowNs,std::span<const VkSemaphore> completedWaits={});
    // Optional private fence ordered after the exact pass submission, with no
    // stock semaphore consumption. Poll BEFORE forwarding the next intercepted
    // submit on serializedQueue, while the caller holds external queue ownership.
    // Poll only on the recording/submitting thread;
    // publication uses that same game queue and retains all image lifetimes.
    bool completeSubmitted(VkQueue serializedQueue,StereoMailbox&,std::uint64_t nowNs);
    // Non-blocking retirement of an in-flight publication (marker/copy observed).
    bool pollRetire() noexcept;
    // Throttle (render thread, inside the submit hook of the NEXT pass on
    // serializedQueue): bounded wait on this slot's own marker, then retire
    // (publishing first through the early path if it was not published yet).
    enum class WaitResult {NotPending,Retired,Timeout,Unavailable};
    WaitResult waitRetire(VkQueue serializedQueue,StereoMailbox*,std::uint64_t nowNs,std::uint64_t timeoutNs,bool& published) noexcept;
    // Device teardown only: caller must have stopped ALL future recording and
    // submissions referencing these images and serialized every device queue.
    void shutdownRetired();
    std::string report() const;
};
// Two bounded owners allow the CPU to record the next pair while the GPU is
// finishing the previous one. retiredFrame is a conservative contiguous watermark.
// When a pass submission leaves no free owner for the next frame,
// submitted() waits (bounded) for the older pair while the new pass is queued,
// so a GPU-bound frame waits instead of skipping the next frame's stereo pass.
class GameStereoCapture {
    mutable std::mutex mutex_;std::array<StereoCaptureSlot,2> slots_;
    unsigned count_{1},recording_{};std::uint64_t lastFrame_{},generation_{};CaptureFrame config_{};
    FrameContext published_{};VkQueue publishedQueue_{};std::uint64_t publishedNs_{};
    std::uint64_t bypassChecks_{},bypassAllowed_{},bypassCrossQueue_{},bypassNoFresh_{},bypassEpoch_{},bypassUnhealthy_{},bypassPressure_{},bypassUnarmed_{},slotPressure_{};
    std::uint64_t bypassRecording_{},bypassPartial_{},bypassMarker_{},bypassQueue_{},bypassThread_{},bypassCommand_{},bypassLatestMissing_{};
    StereoMailbox* mailbox_{};
    VkDevice directDevice_{}; // Device of the shared depth-direct pipeline (spaceWarpDirectPipeline), destroyed at shutdown
    diagnostics::DurationWindow throttleWait_;
    std::uint64_t inFlightPublished_{},throttleWaits_{},throttleImmediate_{},throttleRetired_{},throttleTimeouts_{},throttleUnavailable_{};
    void published(unsigned,std::uint64_t) noexcept;
    void poll() noexcept;
public:
    static constexpr std::uint64_t throttleNs=100'000'000;
    // Throttle calls shorter than realWaitNs are counted as throttleImmediate and
    // kept out of the throttleWait window.
    static constexpr std::uint64_t realWaitNs=50'000;
    void bind(const XrGraphicsBindingVulkan2KHR&,bool earlyCompletion=false,bool buffered=false);
    bool beginFrame(VkQueue,const CaptureFrame&,VkCommandBuffer passCommand=VK_NULL_HANDLE);
    void cancelFrame(std::uint64_t)noexcept;
    std::uint64_t retiredFrame()const noexcept;
    bool recordEye(VkCommandBuffer,unsigned,const PassImage&);
    bool recordDepth(VkCommandBuffer,unsigned eye,const PassImage& depth);
    void setWarpFrame(const SpaceWarpFrame&) noexcept;                     // Matrices of the recording pair
    static bool warpWanted() noexcept;                                     // SPACEWARP active and not suspended
    // The engine bound-state restore used after the depth-direct pass (GameXr.cpp: the TAA/hands handsRestoreReady/handsRestore).
    static void setEngineRestore(bool(*ready)(VkCommandBuffer) noexcept,void(*restore)(VkCommandBuffer) noexcept) noexcept;
    // true when this submission carried a stereo pass (the frame-lock point; call
    // framePacer.pace() after this returns, outside the capture lock).
    bool submitted(VkQueue,std::uint32_t,const VkSubmitInfo*,VkResult)noexcept;
    bool carriesPass(VkQueue,std::uint32_t,const VkSubmitInfo*)noexcept; // The submit gate waits only before the stereo pass submit
    void commandInvalidated(VkCommandBuffer)noexcept;
    bool completePresent(VkQueue,VkFence,StereoMailbox&,std::uint64_t,std::span<const VkSemaphore> waits={});
    bool completeSubmitted(VkQueue,StereoMailbox&,std::uint64_t);
    // No Vulkan calls besides non-blocking fence polls. The caller separately
    // qualifies live XR/tracking and StereoMailbox::freshCompatible. Declines
    // unless the latest capture has a proven independent completion path (early
    // marker or in-flight publication) and at least one slot is free.
    bool canBypassPresent(VkQueue,std::uint64_t trackingEpoch,std::uint64_t spaceEpoch,std::uint64_t nowNs) noexcept;
    void shutdownRetired();
    std::string report()const;
};
}
