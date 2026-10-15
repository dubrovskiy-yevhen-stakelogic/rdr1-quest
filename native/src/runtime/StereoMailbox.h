#pragma once
#include "StereoTargets.h"
#include "GpuDuration.h"
#include "HdrTransfer.h"
#include "SpaceWarp.h"
#include "StereoAntiAliasing.h"
#include "contracts/SubmissionPolicy.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <vector>

namespace rdrvr::runtime {
struct StereoLayer {
    std::array<XrSwapchain,2> swapchains{};
    FrameContext frame{};
    std::array<Pose,2> poses{};
    std::array<Fov,2> fov{};
    std::array<Extent,2> extent{};
    SpaceWarpLayer warp{}; // XR_FB_space_warp data of this pair (invalid when SPACEWARP is off)
};
enum class SourceEncoding { Linear, SrgbEncoded };
// Ticket of one producer copy in the mailbox ring. It is complete once that
// copy's fence signalled, its ring slot was reused (reuse first observes the
// fence) or the ring was rebuilt (rebuild waits every fence).
struct StereoCaptureTicket {std::uint64_t generation{},sequence{};unsigned slot{};};
// MAILBOX DIRECT: a ring slot reserved for one in-frame capture (StereoMailbox::reserveDirect). The pass copies both eyes straight
// into images (initialized = the slot image is in GENERAL, else UNDEFINED); capture() of that pair then skips its copy loop. token 0 = none.
struct StereoDirectSlot {std::uint64_t generation{},token{};unsigned slot{};std::array<VkImage,2> images{};std::array<bool,2> initialized{};bool valid{};};
// Ring of slotCount pair images. The producer never waits for its own copy;
// the XR consumer only reads copies whose fence it observed signalled and
// never waits (except the bounded late latch, see render()). Binary semaphores: ready (producer -> consumer) and done
// (consumer -> next producer copy into the slot); each signal is waited once,
// by the consumer/producer or by a drain batch when the consumer skipped it.
class StereoMailbox {
    friend struct StereoMailboxTest; // the host GPU test inspects the ring
public:
    static constexpr unsigned slotCount=3,consumerCount=3;
private:
    struct Eye {
        XrSwapchain swapchain{};
        std::vector<XrSwapchainImageVulkan2KHR> images;
        // A second swapchain used on alternate blits (debug.rdrvr.xrchains 2, default; not for HDR). The compositor holds an
        // image until it picked up the next frame, so with ~3 frames queued (pdt 70-95 ms ahead) all 3 images of one chain can be
        // busy and xrWaitSwapchainImage would block the XR thread.
        XrSwapchain swapchain2{};
        std::vector<XrSwapchainImageVulkan2KHR> images2;
        XrSwapchain chain(unsigned set) const noexcept {return set?swapchain2:swapchain;}
        const std::vector<XrSwapchainImageVulkan2KHR>& imagesOf(unsigned set) const noexcept {return set?images2:images;}
        Extent extent{};VkFormat format{},xrFormat{};
    };
    struct Slot {
        std::array<OwnedImage,2> images{};
        std::array<HdrTransfer,2> hdr;
        VkCommandBuffer command{};VkFence fence{};VkSemaphore ready{},done{};
        RenderSubmission frame{};StereoReticle reticle{};
        std::uint64_t sequence{},publishedNs{};
        std::uint64_t readyNs{};bool readyExact{}; // Copy completion (GPU end on CLOCK_MONOTONIC when exact, else observation time)
        bool submitted{},readyPending{},donePending{},initialized{};
        bool antiAliased{}; // This slot's producer pass ran FXAA (GPU time attribution)
        VkQueue queue{};    // The producer queue of this capture (submit gate: only that queue waits)
        bool direct{},directWarp{}; // Also written into an XR ticket's eye images (+ its AppSW images) by this capture
        std::uint64_t mdToken{};    // Reserved by an in-frame capture (sequence 0 = out of the ring until its capture() or release)
    };
    struct Consumer {VkCommandBuffer command{};VkFence fence{};bool submitted{};
        int slot{-1};std::uint64_t seq{};bool link{};}; // blitQueueDelay link resolved at completion (the pair may be latched in flight)
    mutable std::mutex mutex_;
    GpuDuration gpuTiming_;
    VkDevice device_{};VkPhysicalDevice physical_{};std::uint32_t family_{};
    std::vector<VkQueue> producerQueues_;
    VkQueue consumerQueue_{};
    std::array<Eye,2> eyes_{};
    std::array<Slot,slotCount> slots_;
    std::array<Consumer,consumerCount> consumers_{};
    VkCommandPool pool_{};
    bool accepting_{},published_{},screenValid_{},poisoned_{},screensDirty_{};
    std::uint64_t generation_{1},sequence_{},minSequence_{},displayedSequence_{};
    int displayedSlot_{-1};Epochs displayedEpochs_{};
    RenderSubmission latest_{};
    bool displayedReticle_{};
    StereoLayer displayed_{};
    std::uint64_t publishedNs_{},captured_{},copied_{},rejected_{},epochRejected_{},lastFrame_{},reticleCopied_{},reticleExpired_{};
    std::uint64_t inFlightCaptures_{},notReadyRejected_{},slotWaits_{},slotTimeouts_{},readyDrains_{},doneWaits_{};
    std::uint64_t consumerNotReady_{},consumerBusy_{},consumerSkipped_{},framesInFlightMax_{},redrawSkipped_{};
    // FIFO latch (debug.rdrvr.latchfifo, default 1, AppSW synthesizing only): show the OLDEST finished unshown pair instead of the newest,
    // at most one pair behind the newest finished (bounded latency); a late pair then costs one slot of latency instead of a repeat + a skip.
    std::uint64_t fifoPicks_{},fifoOlder_{},fifoSkipAhead_{},fifoNextPropNs_{};int fifoMode_{1};
    // Latch classification; XR thread under mutex_, measurement pack on (KgslClock.h measurementPack).
    // [0] every latch, [1] latches while spaceWarpControl.synthesizing(). Ages are CPU ns since the pair's capture submit
    // (Slot::publishedNs, same steady clock as render()'s nowNs).
    struct LatchCounts {std::uint64_t latches{},newPairs{},newNewerInFlight{},repeats{},repeatsNewerNotReady{},repeatsNothingNewer{},repeatsConsumerBusy{};};
    struct AgeWindow { // last 256 ages (us) + session sum for delta(sumMs)/delta(total) means
        static constexpr unsigned capacity=256;std::array<std::uint32_t,capacity> v{};unsigned n{},next{};std::uint64_t total{};double sumMs{};std::uint32_t last{},max{};
        void add(std::uint32_t us) noexcept {v[next]=us;next=(next+1)%capacity;if(n<capacity)++n;++total;sumMs+=double(us)/1e3;last=us;max=std::max(max,us);}
    };
    struct LatchRow {std::uint64_t ns{},pair{};std::uint32_t ageUs{},pendingUs{};std::uint8_t kind{},synth{};};
    static constexpr unsigned latchRows=512; // = XrHistory::capacity: rows join xrHistory by time (wake <= tNs < next wake)
    std::array<LatchCounts,2> latchCounts_{};std::array<AgeWindow,2> latchedAge_{},pendingAge_{};
    std::array<LatchRow,latchRows> latchRing_{};unsigned latchN_{},latchNext_{};std::uint64_t latchTotal_{},displayedPublishedNs_{};
    std::atomic<std::uint8_t> lastLatch_{};std::atomic<std::uint32_t> lastLatchAgeUs_{};
    void noteLatch(std::uint8_t kind,std::uint64_t nowNs,std::uint64_t pair,std::uint64_t ageNs,std::uint64_t pendingNs) noexcept;
    // XR thread / producer under mutex_: per-pair deadline samples for the pacer's phase controller
    // (FramePacing.h DeadlineTracker -> pairDeadlines) and the bounded late-latch wait (LateLatchPolicy, debug.rdrvr.latchus).
    // The late-latch wait is ONE vkWaitForFences of at most the budget, holding mutex_ (the fence cannot be reset or
    // destroyed meanwhile, and the XR thread never re-acquires the lock after it). A render-thread capture()/copyComplete() that
    // arrives meanwhile blocks for up to the budget: counted by lateWaitSeq_ (odd while the XR thread waits).
    std::atomic<std::uint64_t> lateWaitSeq_{};
    struct LateLatchStats {
        std::array<std::uint64_t,LateLatchPolicy::whyCount> why{};      // policy outcome of every NewerNotReady candidate
        std::uint64_t waits{},hits{},timeouts{},clockNegative{},producerBlocked{};
        AgeWindow waitUs,hitWaitUs,pdtLeadUs,clockUs,producerBlockedUs; // wait (all / hits), pdt - wait end, signal - GPU end,
                                                                        // render-thread lock wait that overlapped a late wait
    };
    DeadlineTracker deadline_{};LateLatchStats late_{};std::uint64_t readyExact_{},readyInexact_{};
    // In-flight latch (debug.rdrvr.latchinflight, default 1, AppSW synthesizing only; re-read with latchfifo at 1 Hz): pick the newest
    // SUBMITTED pair; the consumer batch already waits the slot's ready semaphore, so the XR copy is queued before frame N+1 and runs
    // right behind pair N on the GPU. Always-on XR-thread costs: xrWaitSwapchainImage (eye images) and the consumer vkQueueSubmit.
    // FIXED LATCH DEPTH (debug.rdrvr.latch.depth D, 1..3, default 1): show the newest pair whose views came from XR frame <= now - D
    // (finished or in flight). D 2 gives every pair two XR frames to finish (GPU jitter does not drop a slot) at one frame more
    // latency; XrHarness renders the views D frames ahead (view-ahead = D) so the content matches the frame that shows it.
    int latchDepth_{1};std::uint64_t xrFrameNow_{},depthPicks_{},depthNone_{};
    int inflightMode_{1};std::int64_t inflightBiasNs_{2'000'000};bool lastInflight_{};
    std::uint64_t inflightPicks_{},inflightUnfinished_{},inflightNothing_{};
    AgeWindow imageWaitUs_,consumerSubmitUs_,producerLockUs_;
    AgeWindow warpWaitUs_;unsigned chainSets_{1},nextSet_{},chainsProperty_{2};std::uint64_t setSwaps_{}; // AppSW image waits; eye chain sets
    // Eye image PREFETCH (debug.rdrvr.xrprefetch, default 1; one eye chain set only): right after xrEndFrame the XR thread acquires and
    // waits the next image of both eye swapchains (prefetchImages), so render() - between the latch and xrEndFrame - does not block in
    // xrWaitSwapchainImage (the compositor can hold an image for ~3 frames). The wait lands
    // inside the XR thread's idle time before xrWaitFrame. Swapchain calls stay on the XR thread; held_/heldIndex_ change under mutex_.
    // Diagnostic (debug.rdrvr.xrnoblit 1, default 0; the picture is GARBAGE): the XR frame acquires / releases its images and chains
    // AppSW as usual but records and submits NO GPU work - measures the runtime's phase sync / Prd / cadence when the binding queue never
    // waits behind the game frame.
    int noBlit_{0};std::uint64_t noBlitFrames_{};
    int prefetchMode_{1};std::uint64_t prefetches_{},prefetchUsed_{};AgeWindow prefetchWaitUs_;
    // DIRECT XR WRITE (debug.rdrvr.xrdirect; one eye set, blit path): the runtime's phase sync measures
    // the XR frame to its GPU completion, and an XR blit can queue behind the game frame.
    // TICKETS: image sets (2 eyes + the 4 AppSW images when they can be chained) acquired + waited
    // by the XR thread after xrEndFrame (prefetchImages; a wait that would reach the next XR wake
    // times out and resumes next frame). capture() blits the pair into the oldest ready free
    // ticket and records the MV pass into its AppSW images on the GAME queue; render() of a
    // finished direct pair then only releases images. FIFO: xrReleaseSwapchainImage releases the
    // OLDEST waited image, so tickets go back in order - every assigned ticket up to the shown
    // pair first; a pair older than an assigned ticket cannot be shown any more (blocked =
    // repeat). A pair without a ticket takes the XR blit path, into the oldest free ticket's
    // images when there is one.
    struct Ticket {std::array<std::uint32_t,2> eye{};std::array<bool,2> waited{};bool wantWarp{};SpaceWarp::Images warp{};std::uint64_t seq{};}; // seq 0 = free
    // ONE ticket. OpenXR xrWaitSwapchainImage: "Once a swapchain image has been successfully waited on without timeout,
    // the app must release before waiting on the next acquired swapchain image" - two waited tickets per swapchain are illegal.
    static constexpr unsigned maxTickets=1;
    // xrdirect defaults to 0. With one legal ticket the pipeline is one pair deep: the next pair is captured before the previous one
    // is latched (lockstep: capture ~9 ms before the latch, ready ~18 ms after capture), so every other pair misses the ticket and
    // release order blocks it: about half the pair rate of lockstep AUTO without direct writes.
    std::array<Ticket,maxTickets> tickets_{};unsigned ticketCount_{};int directMode_{0};
    std::uint64_t directCaptures_{},directNoTicket_{},directLatches_{},directWarpLatches_{},directBlocked_{},directRescued_{},ticketTimeouts_{},ticketBlits_{};
    static bool ticketReady(const Ticket& t) noexcept {return t.waited[0]&&t.waited[1];}
    void releaseTickets(unsigned n); // XR thread under mutex_: tickets [0, n) back to the runtime (eyes + AppSW), FIFO shifted
    // SUBMIT GATE (waitNewestCopy): render-thread waits outside mutex_ on the newest capture's fence; discardGpu() waits for
    // externalWaits_ to drain before destroying fences (the waiter decrements before it re-locks for its statistics).
    std::atomic<unsigned> externalWaits_{};std::uint64_t gatedSeq_{},gateChecks_{},gateWaits_{},gateTimeouts_{};AgeWindow gateWaitUs_; // producerLock: render-thread capture() wait for mutex_ (XR render() holds it)
    void noteProducerLock(std::uint64_t lateSeqBefore,std::uint64_t lockStartNs) noexcept;
    // MAILBOX DIRECT (debug.rdrvr.mailbox.direct, read by the render thread in StereoCaptureSlot::recordEye; default 0 = off).
    // Without it: recordEye copies each eye into the capture slot's images_ and capture() copies those into a ring slot (two full-eye copies
    // per pair, the 'capture' GPU lane). reserveDirect (render thread, recording eye 0, try_lock): a ring slot that is not displayed, not
    // reserved, whose producer fence AND every consumer read (XR blit) were observed complete and whose pair cannot be latched
    // (sequence <= displayedSequence_ or < minSequence_, no XR ticket) leaves the ring (sequence 0, mdToken). The pass copies the eyes
    // into it; capture() of that pair skips the copy loop (warp_.produce, the ready/done batch, the slot fence = submit gate target and the
    // in-flight latch are unchanged: ready is signalled behind the pass on the same queue). The ring is never rebuilt for a direct pair
    // (stale = reject). A reservation capture() did not consume is returned by releaseDirect once its pass is GPU-proven complete
    // (StereoCaptureSlot::retire); a ring rebuild meanwhile (discardGpu) keeps its images alive as orphans until that release.
    struct DirectOrphan {std::uint64_t token{};std::array<OwnedImage,2> images{};};
    std::vector<DirectOrphan> mdOrphans_;std::uint64_t mdNextToken_{};
    enum MdWhy : unsigned {MdState,MdConfig,MdAa,MdNoSlot,MdWhyCount};
    std::array<std::uint64_t,MdWhyCount> mdRefused_{};std::atomic<std::uint64_t> mdBusy_{};
    std::uint64_t mdRequests_{},mdReserved_{},mdCaptures_{},mdStale_{},mdAaSkips_{},mdReleased_{},mdOrphaned_{},mdOrphansFreed_{};
    // BUSY ATTRIBUTION (always on, counters only): a reserveDirect try_lock failure reads which instrumented hold owns mutex_ now
    // (mdHold_ = hold id << 8 | phase; 0 = none: an uninstrumented site such as capture() / freshCompatible(), or released meanwhile -> other)
    // and marks it (mdBusyMark_ = hold id). That hold measures itself at its unlock (MdHold::end, under mutex_): full hold, and busy -> unlock
    // (= the wait a retry needs). Exactly one side clears a mark (seq_cst CAS): the holder (measured) or the render thread (missed).
    // mode 2: a busy reservation retries try_lock with yields for <= mdRetryNs.
    enum MdPhase : unsigned {MdpOther,MdpRenderLatch,MdpRenderLateWait,MdpRenderImages,MdpRenderSubmit,MdpPrefetch,MdpReport,MdpCount};
    static constexpr std::uint64_t mdRetryNs=200'000;
    static constexpr unsigned mdHoldBuckets=8; // hold / remain buckets: < edge, the last one = >= 5 ms
    static constexpr std::array<std::uint32_t,mdHoldBuckets-1> mdHoldEdgesUs{50,100,200,500,1000,2000,5000};
    struct MdHoldStats {std::array<std::uint64_t,MdpCount> measured{};std::array<AgeWindow,MdpCount> holdUs{};AgeWindow allHoldUs,remainUs;
        std::array<std::uint64_t,mdHoldBuckets> holdBuckets{},remainBuckets{};};
    struct WaitHistogram { // lock-free (the render thread writes it outside mutex_): 10 us buckets, the last one = >= 250 us
        static constexpr unsigned buckets=26,stepUs=10;std::array<std::atomic<std::uint32_t>,buckets> n{};std::atomic<std::uint64_t> total{},sumUs{};std::atomic<std::uint32_t> max{};
        void add(std::uint32_t us) noexcept;
    };
    struct MdHold { // an XR-thread / report() hold of mutex_: construct right after the lock, end() (or destroy) before the unlock
        const StereoMailbox& m;std::uint64_t id{},startNs{};
        MdHold(const StereoMailbox&,unsigned phase) noexcept;~MdHold(){end();}
        MdHold(const MdHold&)=delete;MdHold& operator=(const MdHold&)=delete;
        void phase(unsigned p) const noexcept {m.mdHold_.store(id<<8|p,std::memory_order_relaxed);}
        void end() noexcept;
    };
    mutable std::atomic<std::uint64_t> mdHold_{},mdBusyMark_{},mdBusyNs_{};mutable std::atomic<unsigned> mdBusyPhaseSeen_{};
    mutable std::uint64_t mdHoldIds_{};mutable MdHoldStats mdHoldStats_{}; // mutex_ held (report() is const and a holder too)
    std::array<std::atomic<std::uint64_t>,MdpCount> mdBusyPhase_{};std::atomic<std::uint64_t> mdBusyMissed_{},mdBusyRetried_{},mdBusyWon_{};WaitHistogram mdBusyWaitUs_;
    void noteBusy(std::uint64_t nowNs) noexcept; // render thread, try_lock failed (outside mutex_)
    const char* status_{"WAITING_FOR_STEREO_SOURCE"};
    SpaceWarp warp_; // AppSW: per-slot depth + reprojection (producer), motion-vector pass (consumer)
    // Post anti-aliasing (FXAA) in place of the producer copy. Requested by
    // GRAPHICS -> ANTI-ALIASING, debug.rdrvr.aa=<0..3> overrides; any unsupported case copies (counted).
    // 2 FXAA EDGES / 3 FXAA SPLIT run the same pass with aaParams_.debug = mode-1 (debug views).
    // The format-pair pipeline objects are built by aaBuild_ on a background thread (the
    // submit hook only polls it; frames copy meanwhile, counted "building") and survive ring
    // rebuilds; views/framebuffers follow the ring; shutdown waits the build and destroys all.
    StereoAntiAliasing aa_;StereoAntiAliasing::Params aaParams_{};
    std::future<StereoAntiAliasing::Shared> aaBuild_;VkFormat aaBuildSource_{},aaBuildTarget_{},aaBrokenSource_{},aaBrokenTarget_{};
    unsigned aaMenu_{},aaApplied_{};int aaProperty_{-1};std::uint64_t aaPoll_{};
    std::uint64_t aaFrames_{},aaHdrFrames_{},aaCopyFrames_{},aaFallbacks_{},aaCreates_{},aaCreateNs_{},aaBuildFailures_{},aaTargetCreates_{},aaSourceReleases_{};
    std::array<std::uint64_t,unsigned(StereoAntiAliasing::Fallback::Count)> aaFallbackReasons_{};
    diagnostics::DurationWindow aaPassGpu_,copyPassGpu_;
    unsigned antiAliasingRequest();
    void antiAliasingCollect() noexcept;
    void antiAliasingWaitBuild() noexcept;
    void commands();
    void begin(VkCommandBuffer,unsigned lane);
    void submit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence);
    bool observeSlot(unsigned);
    bool observeConsumer(unsigned);
    int acquireSlot();
    int acquireConsumer();
    unsigned framesInFlight() const;
    void waitAll();
    void discardScreens();
    void discardGpu();
    void screens(XrSession);
public:
    static constexpr std::uint64_t maxAgeNs=150'000'000;
    static constexpr std::uint64_t slotWaitNs=50'000'000;
    StereoMailbox()=default;
    StereoMailbox(const StereoMailbox&)=delete;
    StereoMailbox& operator=(const StereoMailbox&)=delete;
    void bind(const XrGraphicsBindingVulkan2KHR&);
    // Caller serializes the known game queue. Both sources are single-sample,
    // TRANSFER_SRC-capable and stay alive/unchanged until the returned ticket is
    // complete. Source layouts are restored. The completion fence must be
    // signalled, unless queueOrdered: then every source write was submitted
    // earlier on this queue, or behind semaphores consumed earlier on this
    // queue, so queue order puts the copy after it (the fence may be pending).
    // sampledSources = both sources also have SAMPLED usage; required for the
    // anti-aliasing pass (otherwise the pair is copied and counted as a fallback).
    bool capture(VkQueue,const RenderSubmission&,std::uint64_t nowNs,SourceEncoding,const StereoReticle& reticle={},
                 StereoCaptureTicket* ticket=nullptr,bool queueOrdered=false,bool sampledSources=false,
                 const SpaceWarpSource* warp=nullptr, // Small per-eye depth + matrices of this pair (optional)
                 const StereoDirectSlot* direct=nullptr); // The pass already wrote this reserved slot; true = reservation consumed
    // MAILBOX DIRECT: render thread, while recording eye 0 of a pair whose published eye i has extents[i] / formats[i] (SWAP EYES
    // already applied). Never waits (try_lock); false = the two-copy path (counted). Never fails a ring invariant: see mdToken.
    bool reserveDirect(VkQueue,const std::array<Extent,2>& extents,const std::array<VkFormat,2>& formats,SourceEncoding,
                       std::uint64_t resourcesEpoch,StereoDirectSlot& out,bool retry=false) noexcept; // Retry = mode 2 (<= mdRetryNs)
    // Gives back a reservation capture() did not consume, once nothing can write it any more (its pass GPU-complete or never submitted).
    void releaseDirect(const StereoDirectSlot&) noexcept;
    // Non-blocking: has the copy behind this ticket finished reading its sources?
    bool copyComplete(const StereoCaptureTicket&) noexcept;
    // Bounded wait for the same (source destruction on a resize only; holds the
    // mailbox lock while waiting). False on timeout or device loss.
    bool waitCopy(const StereoCaptureTicket&,std::uint64_t timeoutNs) noexcept;
    // The caller is about to destroy these source images. Afterwards no mailbox
    // object (anti-aliasing source view) refers to them; a slot still reading them is
    // waited for (bounded, holds the mailbox lock). False on timeout.
    bool releaseSources(const std::array<VkImage,2>& images,std::uint64_t timeoutNs) noexcept;
    bool render(XrSession,std::uint64_t trackingEpoch,std::uint64_t spaceEpoch,std::uint64_t nowNs,StereoLayer&);
    // XR thread, right after xrEndFrame of a world stereo frame: acquire + wait the next ticket ahead (see Ticket).
    void prefetchImages();
    void setXrFrame(std::uint64_t frame) noexcept {xrFrameNow_=frame;} // XR thread, before render() (the frame that publishes views)
private:
    static bool e0NotHdr(const std::array<Eye,2>& eyes) noexcept;
public:
    // SUBMIT GATE (render thread, before a game submit on `queue`): once per captured pair, if the
    // newest pair's capture copy was submitted on this queue and has not finished, wait for its
    // fence (<= timeoutNs) so the next game frame reaches the GPU after it - the XR copy of that
    // pair (waiting its ready semaphore) then runs before the next frame instead of behind it.
    // Returns true when it waited.
    bool waitNewestCopy(VkQueue queue,std::uint64_t timeoutNs) noexcept;
    // What the last render() did, for xrHistory flag 16 (XR thread, read right after render()).
    // None = early-out (no screen / epoch / age / stale resubmit / no layer) or measurement pack off. A repeat re-shows the
    // displayed pair: NewerNotReady = a newer pair was captured but its copy had not finished (GPU-late), NothingNewer = no
    // newer pair captured yet (render-thread / pacer-late), ConsumerBusy = a finished pair but all consumer buffers in flight.
    enum class Latch : std::uint8_t {None,New,NewNewerInFlight,RepeatNewerNotReady,RepeatNothingNewer,RepeatConsumerBusy};
    Latch lastLatch() const noexcept {return Latch(lastLatch_.load(std::memory_order_relaxed));}
    std::uint32_t lastLatchAgeUs() const noexcept {return lastLatchAgeUs_.load(std::memory_order_relaxed);} // pair age at this latch
    bool freshCompatible(std::uint64_t trackingEpoch,std::uint64_t spaceEpoch,std::uint64_t nowNs) const noexcept;
    void invalidate();
    void detachSession();
    void shutdown();
    std::string report() const;
    std::string spaceWarpReport() const;
};
} // namespace rdrvr::runtime
