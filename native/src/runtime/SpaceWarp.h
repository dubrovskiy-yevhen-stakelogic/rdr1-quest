#pragma once
// Application SpaceWarp (XR_FB_space_warp) for the stereo mailbox.
//
// Data flow per world frame N (SPACEWARP on):
//  1. Render thread, stereo pass N: right after each eye's colour copy, the eye's depth (D24S8,
//     full resolution) is copied (blitted to the motion-vector rect where the format allows a blit;
//     Quest 3 cannot blit D24S8, so it is a full-size copy) into a per-capture-slot depth image
//     (StereoCaptureSlot::recordDepth), left in SHADER_READ_ONLY_OPTIMAL (SAMPLED usage + a
//     DEPTH-aspect view). Transfer commands only: no engine pipeline/descriptor state is touched.
//  2. Render thread, mailbox copy C_N (queued right behind pass N): a depth-only pass per eye writes
//     the mailbox ring slot's depth at the motion-vector resolution (farthest of a 2x2 gather,
//     shaders/spacewarp_depth.frag) from the sampled capture image ("slotDepthPath":"downsample");
//     a device whose depth formats cannot be both attachment and sampled keeps a full-size copy
//     ("copy"). Same slot as the colour pair, same ready/done chain. The camera-only reprojection
//     current -> previous produced frame is computed per eye from the engine's relative clip
//     matrices and eye positions (SpaceWarpMath.h).
//  3. XR thread, consumer: when the slot is displayed, a fragment pass per eye renders the motion
//     vectors (R16G16B16A16_SFLOAT) and depth into the XR motion-vector/depth swapchain images (both at
//     the recommended motion-vector rect, the rect the layer references);
//     XrHarness chains XrCompositionLayerSpaceWarpInfoFB onto each projection view.
// Motion vectors are camera-only (head motion + locomotion). Per-object velocity is not
// available: moving NPCs/animals/vehicles get the vectors of a static object at their depth.
//
// History only across exactly one rendered frame (renderGaps otherwise), cuts at > 0.5 m / > 5 deg
// (snap turn), SAMPLED usage on the MV/depth swapchains, AppSW budget guard (SpaceWarpControl::noteCadence:
// > 7 re-submits in 72 half-rate XR frames suspend the info 5 s, back-off doubling to 40 s), per-mode report,
// warpDepth GPU lane (capture copy + slot depth pass).
//
// Conventions (defaults; every one can be flipped on device):
//   debug.rdrvr.spacewarp=<0..5>: overrides the menu (OFF, ON, ON ZERO MV, ON FAR DEPTH, ON INVERT,
//     ON FLIP Y).
//   debug.rdrvr.spacewarp.mvy=<1|-1>: 1 Vulkan NDC y (down +); -1 GL NDC y (up +) [default].
//   debug.rdrvr.spacewarp.mvsign=<1|-1>: 1 current - previous (Meta XrSpaceWarp sample) [default].
//   debug.rdrvr.spacewarp.mvz=<0|1|2>: z: 0 none, 1 delta of stored depth [default], 2 GL NDC (2x).
//   debug.rdrvr.spacewarp.enginey=<0|1|-1>: engine NDC y vs image rows: 0 auto from P[5] [default].
//   debug.rdrvr.spacewarp.depth=<0|1|2>: 0 forward [0,1], nearZ=near, farZ=far [default];
//     1 farZ=+inf; 2 reversed (nearZ=far, farZ=near).
//   debug.rdrvr.spacewarp.delta=<0|1|2>: appSpaceDeltaPose 0 identity, 1 inv(prev)*cur [default],
//     2 inv(cur)*prev.
//   debug.rdrvr.spacewarp.skip=<0..3>: FRAME_SKIP_BIT 0 only on cuts/no history, 1 also on
//     re-submitted (not re-rendered) frames [default], 2 re-submits carry no space-warp info,
//     3 = 1 + pose-compensated re-submits: the re-submitted pair's projection poses are moved by
//     the locomotion of the elapsed pairs (inverse appSpaceDeltaPose ^ n), so positional timewarp
//     keeps the world moving while the pair holds; NPC motion still holds.
//   debug.rdrvr.spacewarp.guard=<0|1>: AppSW budget guard [default 0].
//   debug.rdrvr.spacewarp.cutcm=<n>: cut translation per pair in cm [default 50].
//   debug.rdrvr.spacewarp.cutdeg=<n>: cut rotation per pair in degrees [default 5].
//   debug.rdrvr.spacewarp.depthdirect=<0|1>: 1 = step 1 records the farthest-of-2x2 downsample
//     straight from the engine eye depth into a 420x440 per-capture-slot depth (SpaceWarpDirect)
//     and step 2 only copies that into the ring slot [default]; 0 = the full-size copy + mailbox
//     downsample above. Re-read at 1 Hz, applied from the next pair. Report:
//     spaceWarp.gpu.depthDirect.
// AppSW dump (verification on the picture, one-shot, nothing recorded or allocated until requested):
//   debug.rdrvr.eyedump <nonce> (EyeDump.h) or a new value of debug.rdrvr.spacewarp.dump: the next produced pair writes, for both eyes,
//     rdrvr/eye-dump-warp-{L,R}-slotDepth.raw (the 420x440 depth the MV pass samples), -mv.raw (R16G16B16A16_SFLOAT motion vectors) and
//     -xrDepth.raw (the depth handed to the compositor) + eye-dump-warp.json (matrices, conventions, push constants, the actor blocks
//     and a per-actor trace). The consumer side re-renders the MV pass with the same pipeline, descriptor set and push constants into
//     private images right after the real pass (SpaceWarp::dumpConsumer); debug.rdrvr.spacewarp.dumpxr=1 at swapchain creation also
//     gives the swapchains TRANSFER_SRC usage and copies the real images (-mvXr / -xrDepthXr) for a bit-exact comparison.
//   debug.rdrvr.spacewarp.depthdual <new value>: the next depth-direct pair ALSO records the full-size
//     copy (GameStereoCapture) and the mailbox runs the copy path's downsample into a private image:
//     -depthCopyPath.raw vs -slotDepth.raw (= the direct path), compared on the device ("dualCompare"
//     in the JSON and spaceWarp.gpu.dump.lastDual).
//   * the dump spans TWO consecutive produced pairs: the request's first pair only copies its slot depth (-prevDepth.raw, "prevDepth"
//     in the JSON), the next pair (its MV history = that pair) is the dumped one. That allows checking the motion
//     vectors against the PICTURE: each pixel's surface, moved back by the dumped MV, must land on the previous pair's depth
//     (actor vectors vs camera-only on the actor pixels, camera vectors on the rest).
//   * debug.rdrvr.spacewarp.actorpad=<0|1> actor cylinder widening: 0 = half a root step [default], 1 = a full step
//     (SpaceWarpActors.h buildActorBlock padSteps). Re-read at 1 Hz. Report: spaceWarp.gpu.actors.pad.
// Per-bone motion (SpaceWarpBones.h): parts of <= 8 near actors after the actor array of each eye's
// block (two vkCmdUpdateBuffer per eye: header + actors, then the parts at gpuBoneOffset; eye stride 16384 B); the actor rect is
// clipped at the near plane (actors wholly behind the eye are skipped). Mode debug.rdrvr.mvbones (main thread, VrMotionBones.h) travels
// with the ActorSet. Report: spaceWarp.gpu.actors.{skipBehind,clipped,stillPinned,bones{...}}; dump JSON: per block "bones",
// the eye's "part" list (blocks with parts first, shape.w = rank), per actor snapshot "bones".
#include "StereoTargets.h"
#include "SpaceWarpMath.h"
#include "SpaceWarpActors.h" // Per-actor motion
#include "FramePacing.h" // XR frame counter / intervals for synthesizing() and the verdict
#include "diagnostics/FrameTimings.h"
#include <atomic>
#include <array>
#include <chrono> // SpaceWarpDirectPipeline (background build)
#include <future>
#include <memory>
#include <mutex>
#include <thread> // SpaceWarpDirectPipeline::wait (host tests)
#include <string>
#include <vector>

namespace rdrvr::runtime {
// Per rendered world frame (GameXr, render thread), engine units.
struct SpaceWarpFrame {
    std::uint64_t frame{};
    std::array<std::array<float,16>,2> relClip{};       // clip = relClip * (p - position)
    std::array<std::array<float,3>,2> position{};       // eye positions
    std::array<double,16> worldFromLocal{};             // OpenXR LOCAL (metres) -> engine world (units)
    float unitsPerMetre{1},nearClip{},farClip{};        // engine units
    float projectionY{};                                // eye projection m[5]: > 0 = engine NDC y up
    bool valid{};
    spacewarp::ActorSet actors{};                       // Newest main-thread actor matrices at record time
};
// StereoCaptureSlot -> StereoMailbox::capture: per-eye depth written by commands queued before the
// capture on the same queue, in `layout`.
struct SpaceWarpSource {
    SpaceWarpFrame frame{};
    std::array<VkImage,2> depth{};VkFormat format{};Extent extent{};
    std::array<VkImageView,2> views{};                          // DEPTH-aspect views (SAMPLED usage); null = copy only
    VkImageLayout layout{VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}; // SHADER_READ_ONLY_OPTIMAL when sampled
    bool direct{}; // Depth = SpaceWarpDirect images (MV rect, slot depth format, TRANSFER_SRC_OPTIMAL): the mailbox only copies
    // depthdual: a direct pair that ALSO carries the copy path's full-size depth (SHADER_READ_ONLY_OPTIMAL, DEPTH view) for the dump
    std::array<VkImage,2> dualDepth{};std::array<VkImageView,2> dualViews{};VkFormat dualFormat{};Extent dualExtent{};
};
namespace spacewarp { // Why an eye that wanted the depth-direct path took the full-size copy (spaceWarp.gpu.depthDirect)
// DirectBuilding = the shared pipeline is still being built in the background (DirectBuild = that build failed).
enum DirectReason : unsigned {DirectUsage,DirectFormat,DirectRestore,DirectBuild,DirectImages,DirectView,DirectMailbox,DirectPair,DirectBuilding,DirectReasonCount};
inline const char* directReasonName(unsigned r) noexcept {
    static constexpr const char* names[DirectReasonCount]{"usage","format","restore","build","images","view","mailbox","pair","building"};
    return r<DirectReasonCount?names[r]:"none";}
}
// StereoMailbox -> XrHarness (part of StereoLayer).
struct SpaceWarpLayer {
    bool valid{},skip{};std::uint64_t frame{};
    std::array<XrSwapchain,2> motion{},depth{};Extent extent{};
    XrPosef delta{{0,0,0,1},{0,0,0}};
    float minDepth{0},maxDepth{1},nearZ{},farZ{};
};
class SpaceWarpControl {
public:
    static constexpr unsigned modes=spacewarp::variantCount; // OFF, ON, ON ZERO MV, ON FAR DEPTH, ON INVERT, ON FLIP Y
    static const char* modeName(unsigned m) noexcept {return spacewarp::variantName(m);}
    // XR bootstrap (GameXr): extension enabled at xrCreateInstance + system properties.
    std::atomic<bool> extension{};std::atomic<std::uint32_t> width{},height{};std::atomic<int> propertiesResult{1};
    bool available() const noexcept {return extension.load(std::memory_order_relaxed)&&width.load(std::memory_order_relaxed)&&height.load(std::memory_order_relaxed);}
    unsigned mode() const noexcept {const int p=property_.load(std::memory_order_relaxed);return p>=0?unsigned(p):menu_.load(std::memory_order_relaxed);}
    bool active() const noexcept {return available()&&mode()!=0;}
    Extent extent() const noexcept {return {width.load(std::memory_order_relaxed),height.load(std::memory_order_relaxed)};}
    // XR thread, every frame (properties re-read once per second).
    void configure(unsigned menuMode,std::uint64_t now=diagnostics::monotonicNs()) noexcept;
    // Shader conventions for a slot whose engine projection had m[5] = projectionY.
    spacewarp::Conventions conventions(float projectionY) const noexcept;
    int depthMode() const noexcept {return depthMode_.load(std::memory_order_relaxed);}
    int deltaMode() const noexcept {return deltaMode_.load(std::memory_order_relaxed);}
    int skipPolicy() const noexcept {return skip_.load(std::memory_order_relaxed);}
    // Cut thresholds per produced pair (debug.rdrvr.spacewarp.cutcm / cutdeg).
    double cutMetres() const noexcept {return double(cutCm_.load(std::memory_order_relaxed))/100.0;}
    double cutDegrees() const noexcept {return double(cutDeg_.load(std::memory_order_relaxed));}
    // XR thread: /perfmetrics_meta/compositor/spacewarp_mode once per second while active.
    void pollRuntime(XrInstance,XrSession,std::uint64_t now=diagnostics::monotonicNs()) noexcept;
    // XR thread, per xrEndFrame with a stereo layer.
    void noteSubmit(bool withInfo,bool skipBit,bool resubmit,const XrPosef& delta,std::int64_t predictedPeriodNs) noexcept;
    double runtimeMode() const noexcept {return runtimeMode_.load(std::memory_order_relaxed);}
    // Space-warp info was really chained within the last synthesisWindow XR frames. Only then does
    // SPACEWARP force LOCK 36 (a source/GPU failure must not leave a plain 36 lock behind).
    static constexpr std::uint64_t synthesisWindow=72;
    bool synthesizing() const noexcept {
        const auto last=lastInfoXrFrame_.load(std::memory_order_relaxed);
        return active()&&last&&framePacer.xrFrames()-last<=synthesisWindow;}
    // AppSW budget guard: while suspended no space-warp info is chained (the runtime returns to its
    // full-rate loop), the producer does no depth work, and LOCK 36 stays forced (lockForced()).
    bool overBudget(std::uint64_t now=diagnostics::monotonicNs()) const noexcept {return now<suspendUntil_.load(std::memory_order_relaxed);}
    bool producing() const noexcept {return active()&&!overBudget();}
    bool lockForced() const noexcept {return synthesizing()||(active()&&overBudget());}
    // XR thread, every XR frame in which space-warp info would be chained (not suspended): resubmit = the pair was
    // already shown; halfRate = the runtime runs xrWaitFrame at >= 1.5 display periods (only those are counted).
    void noteCadence(bool resubmit,bool halfRate,std::uint64_t now=diagnostics::monotonicNs()) noexcept;
    // XR thread, every XR frame with a stereo layer while SPACEWARP is active (per-mode cadence counters).
    void noteFrame(std::uint64_t pairFrame) noexcept;
    // Render thread (mailbox mutex), per produced pair: history gap, cut, CPU-side centre motion (< 0 = none).
    void noteProduced(bool renderGap,bool cut,double centreMotion) noexcept;
    // GPU warpDepth lane (DETAIL): part 0 = capture-side copy (both eyes, pass command buffer), 1 = slot depth
    // (downsample pass or copy, both eyes, mailbox command buffer). A frame with both parts adds a total.
    // direct = the pair took the depth-direct path (also kept in depthDirect.gpu.{capture,slot}).
    void noteDepthGpu(unsigned part,std::uint64_t frame,std::uint64_t ns,bool direct=false) noexcept;
    // Depth direct: property (default 1), the mailbox's refusal (a direct source it cannot consume: copy path until the
    // next GPU generation), per-eye result (capture side) and per-pair path. JSON fields of spaceWarp.gpu.depthDirect.
    bool depthDirect() const noexcept {return depthDirect_.load(std::memory_order_relaxed)!=0;}
    bool directRefused() const noexcept {return directRefused_.load(std::memory_order_relaxed);}
    void refuseDirect(bool refused) noexcept {directRefused_.store(refused,std::memory_order_relaxed);}
    void noteDirectEye(bool direct,unsigned reason) noexcept;
    void noteDirectPair(bool direct) noexcept;
    std::string directFields() const;
    // Why an active SPACEWARP is not synthesizing (menu text), nullptr when it is or when it is off.
    const char* issue() const noexcept {
        if(!active())return nullptr;
        if(overBudget())return "GPU OVER BUDGET"; // shown immediately, not after the 72-frame window
        if(synthesizing())return nullptr;
        if(const char* gpu=gpuIssue_.load(std::memory_order_relaxed))return gpu;
        const int produce=lastProduce_.load(std::memory_order_relaxed);
        return produce==2?"NO DEPTH SOURCE":produce==3?"DEPTH MISMATCH":"STARTING";}
    // SpaceWarp (mailbox mutex): short GPU failure label (nullptr = none) and the last producer result
    // (1 data, 2 no depth source from the capture slot, 3 slot format/extent mismatch).
    void setGpuIssue(const char* label) noexcept {gpuIssue_.store(label,std::memory_order_relaxed);}
    void noteProduce(int result) noexcept {lastProduce_.store(result,std::memory_order_relaxed);}
    std::uint64_t lastSubmittedFrame{}; // XR thread only: renderFrameId of the last pair submitted with space-warp info
    std::atomic<std::uint64_t> compensatedResubmits{}; // Re-submits shown with locomotion-compensated poses (skip policy 3)
    std::atomic<int> mvPlayer{1}; // debug.rdrvr.mvplayer (default 1): synthetic actor block for the player's own body
    std::string report() const;
private:
    std::atomic<unsigned> menu_{};std::atomic<int> property_{-1};
    std::atomic<int> mvY_{-1},mvSign_{1},mvZ_{1},engineY_{0},depthMode_{0},deltaMode_{1},skip_{1};
    std::atomic<int> guardEnabled_{0},cutCm_{50},cutDeg_{5};
    std::uint64_t nextProperties_{},nextPoll_{};
    XrInstance pollInstance_{};XrPath metricPath_{};PFN_xrQueryPerformanceMetricsCounterMETA query_{};
    std::atomic<int> metricResult_{1};std::atomic<double> runtimeMode_{-1};std::atomic<std::uint64_t> metricPolls_{};
    std::atomic<std::uint64_t> submitted_{},withInfo_{},skipped_{},resubmits_{},withoutInfo_{};
    std::atomic<std::int64_t> predictedPeriod_{};
    std::atomic<std::uint64_t> lastInfoXrFrame_{};std::atomic<const char*> gpuIssue_{};std::atomic<int> lastProduce_{};
    std::atomic<std::int64_t> intervalWithInfo_{},periodWithInfo_{},predictedWithInfo_{}; // XR timing while info is chained
    // Budget guard (guard_ under mutex_, written by the XR thread), per-mode counters, warpDepth lane.
    std::atomic<std::uint64_t> suspendUntil_{};unsigned guardMode_{}; // guardMode_: XR thread
    struct ModeStats {std::atomic<std::uint64_t> xrFrames{},newPairs{},resubmits{},renderGaps{},cuts{},suspensions{};};
    std::array<ModeStats,modes> perMode_{};std::uint64_t lastPairFrame_{}; // lastPairFrame_: XR thread
    diagnostics::DurationWindow depthCapture_,depthSlot_,depthTotal_;
    struct DepthPending {std::uint64_t frame{};std::array<std::uint64_t,2> ns{};unsigned mask{};};
    mutable std::mutex mutex_;
    struct Window {std::array<double,256> v{};unsigned n{},next{};std::uint64_t total{};
        void add(double x) noexcept {v[next]=x;next=(next+1)%v.size();if(n<v.size())++n;++total;}
        std::string json() const;};
    Window translationMm_,rotationDeg_;
    spacewarp::CadenceGuard guard_{};                   // mutex_
    std::array<Window,modes> mvCentre_{};               // mutex_
    std::array<DepthPending,8> depthPending_{};         // mutex_
    // Depth direct (property: XR thread; counters: render thread; read by SpaceWarp::report).
    std::atomic<int> depthDirect_{1},directLastReason_{-1};std::atomic<bool> directRefused_{},directLast_{};
    std::atomic<std::uint64_t> directEyes_{},directPairs_{},copyPairs_{},directFallbacks_{};
    std::array<std::atomic<std::uint64_t>,spacewarp::DirectReasonCount> directReasons_{};
    diagnostics::DurationWindow directCapture_,directSlot_;
    // Dump triggers (XR thread, configure): a new non-zero value of the property bumps spacewarp::dumpRequests / dualRequests.
    int dumpProp_{},dualProp_{};bool propsSeen_{};
    std::atomic<int> actorPad_{0}; // debug.rdrvr.spacewarp.actorpad (0 half a root step, 1 a full step)
public:
    double actorPadSteps() const noexcept {return actorPad_.load(std::memory_order_relaxed)?1.0:0.5;}
    std::atomic<int> dumpXr{};       // debug.rdrvr.spacewarp.dumpxr (read by SpaceWarp::screens at swapchain creation)
    void enableGuardForTest(bool on) noexcept {guardEnabled_.store(on?1:0,std::memory_order_relaxed);} // host GPU test (property-less)
};
inline SpaceWarpControl spaceWarpControl;

// AppSW depth direct (debug.rdrvr.spacewarp.depthdirect, default 1). One per StereoCaptureSlot, every call under that slot's
// mutex (render thread). Records, in the engine's frame command buffer right after the eye's colour capture copy, the mailbox's
// farthest-of-2x2 downsample (spacewarp.vert + spacewarp_depth.frag: the same rule and the same 420x440 result) sampling the engine
// eye depth through a DEPTH-aspect view in DEPTH_STENCIL_READ_ONLY_OPTIMAL (as TAA LITE samples it) into this slot's eye image at
// the motion-vector rect (slot depth format, left TRANSFER_SRC_OPTIMAL for the mailbox copy). Replaces the full-size depth copy per
// eye (Quest 3: D24S8 cannot be blitted) and the mailbox's downsample of that copy. The engine depth goes back to the layout the
// engine tracks; the engine's bound command state is re-emitted with the TAA/hands restore (spaceWarpRestore, from GameXr.cpp).
struct SpaceWarpRestore {std::atomic<bool(*)(VkCommandBuffer) noexcept> ready{};std::atomic<void(*)(VkCommandBuffer) noexcept> restore{};};
inline SpaceWarpRestore spaceWarpRestore; // GameStereoCapture::setEngineRestore; null = never direct (fallback "restore")
// The device/format-level objects of the depth-direct pass (sampler, set layout, pipeline layout, render pass,
// pipeline) are shared by both capture slots and built ONCE on a background thread (std::async, as the mailbox's FXAA
// objects): no vkCreateGraphicsPipelines inside engine command recording or under the GameStereoCapture mutex. Requested at
// GameStereoCapture::bind (session start) and again by the first direct eye if none exists; until it lands every eye takes the
// copy path with reason "building" ("build" = the build failed: sticky until destroy). Handles are copied out (plain values);
// the objects live until destroy(device) at device teardown (after the slots, device idle). Own leaf mutex, never held while
// compiling or waiting. Report: spaceWarp.gpu.depthDirect.pipeline.
struct SpaceWarpDirectShared {
    enum State : unsigned {None,Building,Ready,Failed};
    unsigned state{None};std::uint64_t id{};VkFormat format{};std::uint64_t buildNs{};
    VkSampler sampler{};VkDescriptorSetLayout setLayout{};VkPipelineLayout layout{};VkRenderPass pass{};VkPipeline pipeline{};
};
class SpaceWarpDirectPipeline {
public:
    using Shared=SpaceWarpDirectShared;
    static Shared build(VkDevice,VkFormat);              // any thread; throws (partial objects destroyed)
    static void destroyObjects(VkDevice,Shared&) noexcept;
    void request(VkDevice,VkFormat) noexcept;            // start the background build unless built / pending / failed; never waits
    unsigned get(VkDevice,VkFormat,Shared& out) noexcept; // non-blocking: adopt a finished build, request when none; = out.state
    bool wait(std::uint64_t timeoutNs) noexcept;         // host tests: a pending build landed (true also when none is pending)
    void destroy(VkDevice) noexcept;                     // device idle: waits for a pending build, destroys; a later request rebuilds
    std::string json() const;
private:
    mutable std::mutex mutex_;VkDevice device_{};VkFormat format_{};Shared shared_{};std::future<Shared> build_;bool failed_{};
    std::uint64_t requests_{},builds_{},failures_{},nextId_{},lastBuildNs_{};
    void collect() noexcept;                             // mutex_ held
    void requestLocked(VkDevice,VkFormat) noexcept;      // mutex_ held
};
inline SpaceWarpDirectPipeline spaceWarpDirectPipeline;
class SpaceWarpDirect {
public:
    struct Source {VkImage image{};VkFormat format{};Extent extent{};VkImageLayout layout{};VkImageUsageFlags usage{};std::uint32_t layer{};};
    using Ready=bool(*)(VkCommandBuffer) noexcept;using Restore=void(*)(VkCommandBuffer) noexcept;
    // Target format: the mailbox's slot depth format (attachment + sampled) that can also be copied; UNDEFINED = none.
    static VkFormat targetFormat(VkPhysicalDevice) noexcept;
    VkFormat targetFor(VkPhysicalDevice physical) noexcept { // cached per slot (a device constant)
        if(physical!=targetPhysical_){targetPhysical_=physical;target_=targetFormat(physical);}
        return target_;}
    // Outside any render pass. Every fallible step first: false = nothing recorded, why = spacewarp::DirectReason. shared: the
    // SpaceWarpDirectPipeline objects (null / not Ready = "building", Failed = "build"). queries: timestamps first / first+1
    // around the eye's barriers + pass (DETAIL, capture part of the warpDepth lane) or null.
    bool record(VkPhysicalDevice,VkDevice,VkCommandBuffer,unsigned eye,const Source&,Extent target,const SpaceWarpDirectShared* shared,Ready,Restore,
                VkQueryPool queries,std::uint32_t first,unsigned& why) noexcept;
    bool hasImages() const noexcept {return eyes_[0].image.image!=VK_NULL_HANDLE;}
    bool matches(VkFormat f,Extent e) const noexcept {return hasImages()&&format_==f&&extent_==e;}
    VkImage image(unsigned eye) const noexcept {return eye<2?eyes_[eye].image.image:VK_NULL_HANDLE;}
    VkFormat format() const noexcept {return format_;}
    Extent extent() const noexcept {return extent_;}
    std::uint64_t creates() const noexcept {return creates_;}
    void retired(VkDevice) noexcept;        // the pass that sampled through the per-frame engine depth views is GPU-complete
    void destroyImages(VkDevice) noexcept;  // caller proved that no mailbox copy still reads them
    void destroy(VkDevice) noexcept;        // device idle (per-slot objects only; the shared ones: SpaceWarpDirectPipeline::destroy)
private:
    struct Eye {OwnedImage image{};VkImageView attach{};VkFramebuffer framebuffer{};VkDescriptorSet set{};VkImageView source{};};
    std::array<Eye,2> eyes_{};VkFormat format_{};Extent extent_{};
    // Per slot (cheap, no compilation): the descriptor pool/sets and framebuffers made against shared generation sharedId_.
    VkDescriptorPool pool_{};std::uint64_t sharedId_{};VkRenderPass pass_{};std::uint64_t creates_{};
    VkPhysicalDevice targetPhysical_{};VkFormat target_{};
    bool bindShared(VkDevice,const SpaceWarpDirectShared&) noexcept;
    bool images(VkPhysicalDevice,VkDevice,VkFormat,Extent) noexcept;
    void destroyFramebuffers(VkDevice) noexcept;
    void destroySets(VkDevice) noexcept;
};

namespace spacewarp { // AppSW dump output (host tests point it elsewhere and write synchronously)
inline std::string dumpDir="/sdcard/Android/data/com.rockstargames.rdr/files/rdrvr";
inline std::atomic<bool> dumpSynchronous{false};
inline std::atomic<std::uint64_t> dumpFilesWritten{}; // dumps whose files were written (writer thread)
}
// GPU side, owned by StereoMailbox; every call is made under the mailbox mutex.
class SpaceWarp {
    friend struct SpaceWarpTest; // the host GPU test inspects the slots
public:
    static constexpr unsigned slotCount=3;
    void bind(VkPhysicalDevice physical,VkDevice device,std::uint32_t family) noexcept {physical_=physical;device_=device;family_=family;}
    // Producer (render thread, StereoMailbox::capture, after slot acquisition and before the
    // producer command buffer ends). Records the slot depth (downsample pass or copy); null source,
    // an inactive control or a suspended one (budget guard) invalidates the slot's motion data.
    // Throws only on Vulkan failures.
    void produce(VkCommandBuffer,unsigned slot,const SpaceWarpSource*,const FrameContext&);
    // The slot's producer command buffer completed (fence observed): read its warpDepth timestamps.
    void completed(unsigned slot) noexcept;
    void noteConsumerSkips(std::uint64_t n) noexcept {consumerSkips_+=n;} // Pairs skipped by the consumer while ON
    // Consumer (XR thread, StereoMailbox::render).
    void screens(XrSession);                    // swapchains + pipeline when active (idempotent)
    bool acquire(unsigned slot) noexcept;       // all four XR images acquired and waited, or nothing held
    void record(VkCommandBuffer,unsigned slot) noexcept; // motion-vector/depth pass for both eyes (after acquire)
    void release() noexcept;                    // release the images acquired by acquire()
    SpaceWarpLayer layer(unsigned slot) const noexcept;
    // DIRECT XR WRITE (StereoMailbox tickets): the four images of one pair acquired + waited ahead by the XR thread, written by
    // whoever records the MV pass (the producer for a direct pair, the consumer for a ring pair), released by the XR thread in acquire
    // order. generation = swapchainCreates_ at acquisition (a recreated set is never released / written through stale indices).
    struct Images {bool valid{};std::array<std::uint32_t,2> motion{},depth{};std::uint64_t generation{};};
    struct Chains {std::array<XrSwapchain,2> motion{},depth{};std::array<std::size_t,2> motionCount{},depthCount{};std::uint64_t generation{};};
    bool ticketChains(Chains&) const noexcept;  // mailbox lock: the handles, when tickets may carry AppSW images (one set, chains + pipeline ready, producing)
    static bool acquireImages(const Chains&,Images&) noexcept; // XR thread, NO lock (blocks): acquire + wait all four; false = none held
    bool recordImages(VkCommandBuffer,unsigned slot,const Images&) noexcept; // mailbox lock: MV pass of the slot into these images
    void releaseImages(const Images&) noexcept; // mailbox lock, XR thread
    void noteAcquireFailure() noexcept {++acquireFailures_;fail("ACQUIRE_FAILED","GPU FAILED: ACQUIRE");}
    void discardScreens() noexcept;             // after every consumer submission completed
    void discardGpu() noexcept;                 // after every producer/consumer submission completed
    std::string report() const;
private:
    struct EyeChain {
        XrSwapchain motion{},depth{};
        std::vector<XrSwapchainImageVulkan2KHR> motionImages,depthImages;
        std::vector<VkImageView> motionViews,depthViews;
        std::vector<VkFramebuffer> framebuffers;   // [motionIndex * depthImages.size() + depthIndex]
        std::uint32_t motionIndex{},depthIndex{};
    };
    struct Slot {
        std::array<OwnedImage,2> depth{};VkFormat format{};Extent extent{};
        bool downsample{};                          // Depth written by the downsample pass (else copied)
        std::array<VkImageView,2> attach{};         // DEPTH|STENCIL attachment view (stencil formats only)
        std::array<VkFramebuffer,2> downFramebuffer{};
        std::array<VkDescriptorSet,2> sets{},downSets{}; // sets: MV pass samples the slot depth; downSets: the capture depth
        bool valid{},skip{};std::uint64_t frame{};
        std::array<std::array<float,16>,2> reproject{};
        spacewarp::PoseD delta{};bool deltaValid{};
        float nearZ{},farZ{},projectionY{};
        bool timed{};std::uint64_t timedTicket{},timedFrame{}; // warpDepth timestamps recorded
        bool timedDirect{}; // The timed pair was a depth-direct copy
    };
    struct Previous {
        bool valid{};std::uint64_t frame{};Epochs epochs{};SpaceWarpFrame data{};
    };
    VkPhysicalDevice physical_{};VkDevice device_{};std::uint32_t family_{};
    std::array<Slot,slotCount> slots_{};Previous previous_{};
    std::array<EyeChain,2> eyes_{};Extent extent_{};VkFormat depthFormat_{};bool chains_{},broken_{},acquired_{};
    std::array<EyeChain,2> alt_{};bool altReady_{};std::uint64_t warpSwaps_{};
    std::array<VkFramebuffer,2> current_{}; // framebuffers of the images held by acquire()
    VkSampler sampler_{};VkDescriptorSetLayout setLayout_{};VkDescriptorPool pool_{};
    VkPipelineLayout layout_{};VkRenderPass pass_{};VkPipeline pipeline_{};
    // Slot depth downsample (path_: 0 undecided, 1 downsample, 2 copy) and the warpDepth timestamps.
    VkRenderPass downPass_{};VkPipeline downPipeline_{};VkFormat downFormat_{};int path_{};const char* downStatus_{"UNDECIDED"};
    VkQueryPool queries_{};double tsPeriod_{};std::uint64_t tsMask_{};bool queriesTried_{};
    // Per-actor motion: one uniform buffer, slot m / eye e at m*gpuSlotStride + e*gpuEyeStride (vkCmdUpdateBuffer in produce).
    VkBuffer actorBuffer_{};VkDeviceMemory actorMemory_{};
    std::uint64_t playerBlocks_{}; // Synthetic player actors inserted (per eye)
    std::uint64_t actorPairs_{},actorsWritten_{},actorSameSerial_{},actorSkipUnmatched_{},actorSkipJump_{},actorSkipStill_{},actorSkipOffscreen_{},
        actorSkipMath_{},actorPairsOff_{};
    unsigned actorsMaxEye_{},actorsLastEye_[2]{};float actorMaxStepM_{};
    std::array<std::uint64_t,4> actorSerialDelta_{}; // Main-thread ticks between the snapshots of consecutive pairs: 0, 1, 2, 3+
    // Bone motion: behind / clipped rects, pinned standing actors, part counters (left eye), parts written (both eyes), the eye blocks
    // (heap, made with the actor buffer: 2 x 16.5 KB never on the render thread's stack)
    std::uint64_t actorSkipBehind_{},actorClipped_{},actorStillPinned_{},boneActors_{},boneParts_{},boneUnmatched_{},boneJump_{},boneOverflow_{},boneMath_{},
        bonePartsWritten_{},bonePairs_{};
    unsigned bonesMaxEye_{},bonesLastEye_[2]{};float boneMaxStepM_{};std::array<std::uint64_t,4> boneModePairs_{};
    std::unique_ptr<std::array<spacewarp::GpuActors,2>> blocks_;
    std::unique_ptr<std::array<spacewarp::BoneSet,2>> boneSets_;std::uint64_t boneMissing_{}; // This pair's parts (cur, prev) from the channel
    std::unique_ptr<spacewarp::BonePairCache> boneCache_; // The eye-independent half of the parts (left eye computes, right eye reuses)
    void createActorBuffer();
    // spacewarp::buildActorBlock (SpaceWarpActors.h, host-tested) + the counters; trace (dump) optional.
    unsigned buildActors(const SpaceWarpFrame& cur,const SpaceWarpFrame& prev,unsigned eye,spacewarp::GpuActors& out,spacewarp::ActorTrace* trace=nullptr) noexcept;
    // AppSW dump (see the header comment). Every member under the mailbox mutex; GPU objects made on the first dump only.
    struct DumpEntry {std::string name;VkFormat format{};Extent extent{};bool depth{};VkDeviceSize offset{},bytes{};bool recorded{};};
    struct Dump {
        std::uint64_t request{};int slot{-1};std::uint64_t frame{};unsigned tries{};bool dual{};
        bool producerRecorded{},producerDone{},consumerRecorded{},consumerDone{};std::uint64_t startNs{},consumerNs{};
        std::string meta,consumerMeta;std::vector<DumpEntry> entries;VkDeviceSize used{};
        VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};VkDeviceSize capacity{};VkEvent event{};
        std::array<OwnedImage,2> mv{},depth{};std::array<VkFramebuffer,2> fb{};Extent mirrorExtent{};VkFormat mirrorDepth{};
        std::array<OwnedImage,2> dualImage{};std::array<VkImageView,2> dualAttach{};std::array<VkFramebuffer,2> dualFb{};Extent dualExtent{};VkFormat dualFormat{};
        std::uint64_t started{},written{},failed{},abandoned{},busy{},retries{},timeouts{};std::string lastError,lastDual,lastFiles;
        // Pre-roll = the request's first pair (prevSlot) only copies its slot depth ("prevDepth"); the next consecutive pair
        // is the dumped one (slot). preroll: waiting for that pair; prevRecorded / prevDone: the copy recorded / its slot fence observed.
        bool preroll{},prevRecorded{},prevDone{};int prevSlot{-1};std::uint64_t prevFrame{},prerolls{};Extent planExtent{};VkFormat planFormat{};
    } dump_{};
    bool xrCopy_{}; // the swapchains carry TRANSFER_SRC (dumpxr 1 at creation): the dump also copies the real images
    static VkDeviceSize texelBytes(VkFormat) noexcept;
    VkDeviceSize dumpPlan(const char* name,VkFormat,Extent,bool depth); // appends an entry, returns its offset
    bool dumpBuffer(VkDeviceSize) noexcept;
    bool dumpStart(unsigned m,std::uint64_t request,bool dual,bool preroll=false) noexcept;
    void dumpProducer(VkCommandBuffer,unsigned m,const SpaceWarpSource&,bool preroll=false) noexcept;
    void dumpConsumer(VkCommandBuffer,unsigned m) noexcept;
    void dumpPoll() noexcept;
    void dumpFinish(const char* consumerState) noexcept;
    void dumpCopy(VkCommandBuffer,VkImage,VkImageAspectFlags copyAspect,VkImageAspectFlags barrierAspect,VkImageLayout from,VkImageLayout back,
                  VkPipelineStageFlags srcStage,VkAccessFlags srcAccess,DumpEntry&) noexcept;
    void dumpDestroyImages() noexcept;
    void dumpDestroy() noexcept;
    std::array<float,20> pushFor(const Slot&,unsigned eye) const noexcept;
    void createPass(VkFormat depth); // the MV pass objects; throws
    const char* status_{"OFF"};
    std::uint64_t produced_{},noSource_{},noHistory_{},cuts_{},formatMismatch_{},slotImages_{},consumed_{},acquireFailures_{},swapchainCreates_{};
    std::uint64_t renderGaps_{},suspendedPairs_{},downsamples_{},slotCopies_{},downsampleFailures_{},consumerSkips_{},timingUnavailable_{};
    std::uint64_t directCopies_{},directMismatches_{}; // Depth-direct pairs copied into the slot / refused (format, extent, path)
    diagnostics::DurationWindow passCpu_;
    void base();
    void createSlotImages(Slot&,VkFormat,Extent,bool downsample);
    void destroySlot(Slot&) noexcept;
    bool buildDownsample() noexcept;
    bool timeBegin(VkCommandBuffer,unsigned slot,std::uint64_t frame) noexcept;
    void destroyPipeline() noexcept;
    // Every failure disables space warp until the next discardScreens/discardGpu (ring rebuild,
    // session change) and is shown in the menu through spaceWarpControl.issue().
    void fail(const char* status,const char* label) noexcept {broken_=true;status_=status;spaceWarpControl.setGpuIssue(label);}
    void clearFailure() noexcept {broken_=false;status_="OFF";spaceWarpControl.setGpuIssue(nullptr);}
    VkFramebuffer framebuffer(unsigned eye);
    VkFramebuffer framebufferFor(unsigned eye,std::uint32_t motionIndex,std::uint32_t depthIndex);
    void recordPass(VkCommandBuffer,unsigned slot,const std::array<VkFramebuffer,2>&) noexcept;
};
} // namespace rdrvr::runtime

// ---------------------------------------------------------------------------------------------------------
// SpaceWarpImplementation: header-only on purpose (no new translation unit; StereoMailbox.cpp is also
// #included by the host GPU test).
#include "SpaceWarpShaders.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
namespace rdrvr::runtime {
namespace swdetail {
inline bool hasStencil(VkFormat f) noexcept {return f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_D32_SFLOAT_S8_UINT||f==VK_FORMAT_D16_UNORM_S8_UINT;}
inline VkImageAspectFlags depthAspects(VkFormat f) noexcept {return VK_IMAGE_ASPECT_DEPTH_BIT|(hasStencil(f)?VK_IMAGE_ASPECT_STENCIL_BIT:0);}
inline void barrier(VkCommandBuffer c,VkImage image,VkImageAspectFlags aspect,VkImageLayout old,VkImageLayout next,VkAccessFlags src,VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=old;b.newLayout=next;b.srcAccessMask=src;b.dstAccessMask=dst;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={aspect,0,1,0,1};
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
// One image barrier for a batched vkCmdPipelineBarrier with explicit stages.
inline VkImageMemoryBarrier imageBarrier(VkImage image,VkImageAspectFlags aspect,VkImageLayout old,VkImageLayout next,VkAccessFlags src,VkAccessFlags dst,std::uint32_t layer=0) noexcept {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=old;b.newLayout=next;b.srcAccessMask=src;b.dstAccessMask=dst;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={aspect,0,1,layer,1};return b;
}
// Slot depth format (shared by SpaceWarp::buildDownsample and SpaceWarpDirect): a sampleable depth attachment.
inline VkFormat slotDepthFormat(VkPhysicalDevice physical,VkFormatFeatureFlags extra=0) noexcept {
    const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    for(auto f:{VK_FORMAT_D32_SFLOAT,VK_FORMAT_X8_D24_UNORM_PACK32,VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_D32_SFLOAT_S8_UINT}){
        VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(physical,f,&p);
        if((p.optimalTilingFeatures&need)==need)return (p.optimalTilingFeatures&extra)==extra?f:VK_FORMAT_UNDEFINED;}
    return VK_FORMAT_UNDEFINED;
}
inline int intProperty([[maybe_unused]] const char* name,int fallback) noexcept {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX]{};if(__system_property_get(name,v)<=0)return fallback;
    char* end{};const long x=std::strtol(v,&end,10);return end&&end!=v&&!*end?int(x):fallback;
#else
    return fallback;
#endif
}
inline XrPosef toXr(const spacewarp::PoseD& p) noexcept {
    XrPosef out{};out.orientation={float(p.q[0]),float(p.q[1]),float(p.q[2]),float(p.q[3])};out.position={float(p.p[0]),float(p.p[1]),float(p.p[2])};return out;
}
inline spacewarp::PoseD inverse(const spacewarp::PoseD& p) noexcept {
    spacewarp::PoseD out{};out.q[0]=-p.q[0];out.q[1]=-p.q[1];out.q[2]=-p.q[2];out.q[3]=p.q[3];
    const auto r=spacewarp::rotation(out.q);
    for(int i=0;i<3;++i)out.p[i]=-(r[0*4+i]*p.p[0]+r[1*4+i]*p.p[1]+r[2*4+i]*p.p[2]);
    return out;
}
} // namespace swdetail
// ------------------------------------------------------------------------------------------ control
inline void SpaceWarpControl::configure(unsigned menuMode,std::uint64_t now) noexcept {
    menu_.store(menuMode<modes?menuMode:0u,std::memory_order_relaxed);
    if(now>=nextProperties_){
        nextProperties_=now+1'000'000'000ull;
        const int p=swdetail::intProperty("debug.rdrvr.spacewarp",-1);property_.store(p>=0&&p<int(modes)?p:-1,std::memory_order_relaxed);
        // mvy defaults to -1: the runtime wants motion-vector y up = +; with 1 every head pitch shakes
        // (synthesized frame up, real frame back).
        mvY_.store(swdetail::intProperty("debug.rdrvr.spacewarp.mvy",-1)>0?1:-1,std::memory_order_relaxed);
        mvSign_.store(swdetail::intProperty("debug.rdrvr.spacewarp.mvsign",1)<0?-1:1,std::memory_order_relaxed);
        mvZ_.store(std::clamp(swdetail::intProperty("debug.rdrvr.spacewarp.mvz",1),0,2),std::memory_order_relaxed);
        engineY_.store(std::clamp(swdetail::intProperty("debug.rdrvr.spacewarp.enginey",0),-1,1),std::memory_order_relaxed);
        depthMode_.store(std::clamp(swdetail::intProperty("debug.rdrvr.spacewarp.depth",0),0,2),std::memory_order_relaxed);
        deltaMode_.store(std::clamp(swdetail::intProperty("debug.rdrvr.spacewarp.delta",1),0,2),std::memory_order_relaxed);
        skip_.store(std::clamp(swdetail::intProperty("debug.rdrvr.spacewarp.skip",1),0,3),std::memory_order_relaxed);
        mvPlayer.store(swdetail::intProperty("debug.rdrvr.mvplayer",1)!=0?1:0,std::memory_order_relaxed);
        guardEnabled_.store(swdetail::intProperty("debug.rdrvr.spacewarp.guard",0)!=0?1:0,std::memory_order_relaxed);
        cutCm_.store(std::clamp(swdetail::intProperty("debug.rdrvr.spacewarp.cutcm",50),1,100000),std::memory_order_relaxed);
        cutDeg_.store(std::clamp(swdetail::intProperty("debug.rdrvr.spacewarp.cutdeg",5),1,180),std::memory_order_relaxed);
        depthDirect_.store(swdetail::intProperty("debug.rdrvr.spacewarp.depthdirect",1)!=0?1:0,std::memory_order_relaxed);
        // Dump triggers: a CHANGE to a non-zero value requests one dump (the value seen at the first read is only the baseline)
        const int dumpP=swdetail::intProperty("debug.rdrvr.spacewarp.dump",0),dualP=swdetail::intProperty("debug.rdrvr.spacewarp.depthdual",0);
        if(propsSeen_){
            if(dumpP!=dumpProp_&&dumpP)spacewarp::dumpRequests.fetch_add(1,std::memory_order_relaxed);
            if(dualP!=dualProp_&&dualP)spacewarp::dualRequests.fetch_add(1,std::memory_order_relaxed);
        }
        dumpProp_=dumpP;dualProp_=dualP;propsSeen_=true;
        dumpXr.store(swdetail::intProperty("debug.rdrvr.spacewarp.dumpxr",0)!=0?1:0,std::memory_order_relaxed);
        actorPad_.store(swdetail::intProperty("debug.rdrvr.spacewarp.actorpad",0)!=0?1:0,std::memory_order_relaxed);
    }
    // No AppSW pair is produced while OFF: a dump requested now would fire much later - drop it (two relaxed atomics per frame)
    if(!active()){spacewarp::dumpTaken.store(spacewarp::dumpRequests.load(std::memory_order_relaxed),std::memory_order_relaxed);
        spacewarp::dualTaken.store(spacewarp::dualRequests.load(std::memory_order_relaxed),std::memory_order_relaxed);}
    // A mode change (menu, property, OFF) or a disabled guard starts the guard over (no carried suspension/back-off).
    const unsigned m=active()?mode():0u;
    if(m!=guardMode_||(!guardEnabled_.load(std::memory_order_relaxed)&&suspendUntil_.load(std::memory_order_relaxed))){
        guardMode_=m;std::lock_guard lock(mutex_);guard_.reset();suspendUntil_.store(0,std::memory_order_relaxed);}
}
inline spacewarp::Conventions SpaceWarpControl::conventions(float projectionY) const noexcept {
    const unsigned m=mode();spacewarp::Conventions c{};
    const int ey=engineY_.load(std::memory_order_relaxed);
    c.engineY=ey?float(ey):(projectionY>0?-1.f:1.f);
    c.mvY=float(mvY_.load(std::memory_order_relaxed))*spacewarp::variantY(m);
    c.mvSign=float(mvSign_.load(std::memory_order_relaxed))*spacewarp::variantSign(m);
    c.mvZ=float(mvZ_.load(std::memory_order_relaxed));
    return c;
}
inline void SpaceWarpControl::pollRuntime(XrInstance instance,XrSession session,std::uint64_t now) noexcept {
    if(!instance||!session||!active()||now<nextPoll_)return;
    nextPoll_=now+1'000'000'000ull;
    if(pollInstance_!=instance){
        pollInstance_=instance;query_=nullptr;metricPath_=XR_NULL_PATH;
        PFN_xrVoidFunction f{};
        if(XR_SUCCEEDED(xrGetInstanceProcAddr(instance,"xrQueryPerformanceMetricsCounterMETA",&f))&&f)query_=reinterpret_cast<PFN_xrQueryPerformanceMetricsCounterMETA>(f);
        XrPath path{};if(XR_SUCCEEDED(xrStringToPath(instance,"/perfmetrics_meta/compositor/spacewarp_mode",&path)))metricPath_=path;
    }
    if(!query_||metricPath_==XR_NULL_PATH){metricResult_.store(XR_ERROR_FUNCTION_UNSUPPORTED,std::memory_order_relaxed);return;}
    XrPerformanceMetricsCounterMETA v{XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};
    const auto r=query_(session,metricPath_,&v);metricResult_.store(r,std::memory_order_relaxed);metricPolls_.fetch_add(1,std::memory_order_relaxed);
    if(XR_FAILED(r))return;
    if(v.counterFlags&XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META)runtimeMode_.store(double(v.floatValue),std::memory_order_relaxed);
    else if(v.counterFlags&XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META)runtimeMode_.store(double(v.uintValue),std::memory_order_relaxed);
}
inline void SpaceWarpControl::noteSubmit(bool info,bool skipBit,bool resubmit,const XrPosef& d,std::int64_t period) noexcept {
    submitted_.fetch_add(1,std::memory_order_relaxed);predictedPeriod_.store(period,std::memory_order_relaxed);
    if(resubmit)resubmits_.fetch_add(1,std::memory_order_relaxed);
    if(!info){withoutInfo_.fetch_add(1,std::memory_order_relaxed);return;}
    withInfo_.fetch_add(1,std::memory_order_relaxed);if(skipBit)skipped_.fetch_add(1,std::memory_order_relaxed);
    lastInfoXrFrame_.store(std::max<std::uint64_t>(1,framePacer.xrFrames()),std::memory_order_relaxed);
    intervalWithInfo_.store(framePacer.xrIntervalNs(),std::memory_order_relaxed);periodWithInfo_.store(framePacer.periodNs(),std::memory_order_relaxed);
    predictedWithInfo_.store(period,std::memory_order_relaxed);
    if(resubmit)return;
    const double t=std::sqrt(double(d.position.x)*d.position.x+double(d.position.y)*d.position.y+double(d.position.z)*d.position.z)*1000;
    const double w=std::min(1.0,std::fabs(double(d.orientation.w)));const double a=2*std::acos(w)*57.29577951308232;
    std::lock_guard lock(mutex_);translationMm_.add(t);rotationDeg_.add(a);
}
inline void SpaceWarpControl::noteCadence(bool resubmit,bool halfRate,std::uint64_t now) noexcept {
    if(!guardEnabled_.load(std::memory_order_relaxed))return;
    std::lock_guard lock(mutex_);
    if(!guard_.note(resubmit,halfRate,now))return;
    suspendUntil_.store(guard_.until,std::memory_order_relaxed);
    const unsigned m=mode();if(m<modes)perMode_[m].suspensions.fetch_add(1,std::memory_order_relaxed);
}
inline void SpaceWarpControl::noteFrame(std::uint64_t pair) noexcept {
    const unsigned m=mode();if(m>=modes||!pair)return;
    auto& s=perMode_[m];s.xrFrames.fetch_add(1,std::memory_order_relaxed);
    (pair!=lastPairFrame_?s.newPairs:s.resubmits).fetch_add(1,std::memory_order_relaxed);lastPairFrame_=pair;
}
inline void SpaceWarpControl::noteProduced(bool gap,bool cut,double centre) noexcept {
    const unsigned m=mode();if(m>=modes)return;auto& s=perMode_[m];
    if(gap)s.renderGaps.fetch_add(1,std::memory_order_relaxed);
    if(cut)s.cuts.fetch_add(1,std::memory_order_relaxed);
    if(centre>=0&&std::isfinite(centre)){std::lock_guard lock(mutex_);mvCentre_[m].add(centre);}
}
inline void SpaceWarpControl::noteDirectEye(bool direct,unsigned reason) noexcept {
    if(direct){directEyes_.fetch_add(1,std::memory_order_relaxed);return;}
    directFallbacks_.fetch_add(1,std::memory_order_relaxed);
    if(reason<spacewarp::DirectReasonCount){directReasons_[reason].fetch_add(1,std::memory_order_relaxed);directLastReason_.store(int(reason),std::memory_order_relaxed);}
}
inline void SpaceWarpControl::noteDirectPair(bool direct) noexcept {
    directLast_.store(direct,std::memory_order_relaxed);(direct?directPairs_:copyPairs_).fetch_add(1,std::memory_order_relaxed);
}
// Fields of spaceWarp.gpu.depthDirect (SpaceWarp::report adds the mailbox side). on = property 1 and the newest recorded
// pair took the direct path; passes = eyes recorded direct; fallbacks = eyes that wanted it but took the full-size copy.
inline std::string SpaceWarpControl::directFields() const {
    std::ostringstream o;const int last=directLastReason_.load(std::memory_order_relaxed);
    o<<"\"on\":"<<(depthDirect()&&directLast_.load(std::memory_order_relaxed)?"true":"false")<<",\"property\":"<<depthDirect_.load()
     <<",\"passes\":"<<directEyes_.load()<<",\"fallbacks\":"<<directFallbacks_.load()<<",\"directPairs\":"<<directPairs_.load()<<",\"copyPairs\":"<<copyPairs_.load()
     <<",\"refused\":"<<(directRefused()?"true":"false")<<",\"lastFallback\":\""<<(last>=0?spacewarp::directReasonName(unsigned(last)):"")<<"\",\"fallbackReasons\":{";
    for(unsigned r=0;r<spacewarp::DirectReasonCount;++r)o<<(r?",":"")<<'"'<<spacewarp::directReasonName(r)<<"\":"<<directReasons_[r].load();
    o<<"},\"gpu\":{\"capture\":"<<directCapture_.snapshot().json()<<",\"slot\":"<<directSlot_.snapshot().json()<<'}';
    o<<",\"pipeline\":"<<spaceWarpDirectPipeline.json(); // The shared background-built pass objects
    return o.str();
}
inline void SpaceWarpControl::noteDepthGpu(unsigned part,std::uint64_t frame,std::uint64_t ns,bool direct) noexcept {
    if(part>1||!frame||!ns)return;
    (part?depthSlot_:depthCapture_).elapsed(1,1+ns);
    if(direct)(part?directSlot_:directCapture_).elapsed(1,1+ns);
    std::lock_guard lock(mutex_);
    DepthPending* hit=nullptr;DepthPending* oldest=&depthPending_[0];
    for(auto& d:depthPending_){if(d.frame==frame){hit=&d;break;}if(d.frame<oldest->frame)oldest=&d;}
    if(!hit){hit=oldest;*hit=DepthPending{};hit->frame=frame;}
    hit->ns[part]=ns;hit->mask|=1u<<part;
    if(hit->mask==3u){depthTotal_.elapsed(1,1+hit->ns[0]+hit->ns[1]);*hit=DepthPending{};}
}
inline std::string SpaceWarpControl::Window::json() const {
    std::ostringstream o;o<<"{\"samples\":"<<n<<",\"total\":"<<total;
    if(n){auto s=v;std::sort(s.begin(),s.begin()+n);o<<",\"p50\":"<<s[(n*50+99)/100-1]<<",\"p95\":"<<s[(n*95+99)/100-1]<<",\"max\":"<<s[n-1];}
    o<<'}';return o.str();
}
inline std::string SpaceWarpControl::report() const {
    const auto m=mode();std::ostringstream o;
    o<<"{\"supported\":"<<(available()?"true":"false")<<",\"extension\":"<<(extension.load()?"true":"false")<<",\"propertiesResult\":"<<propertiesResult.load()
     <<",\"recommendedMotionVector\":["<<width.load()<<','<<height.load()<<"],\"mode\":\""<<modeName(m)<<"\",\"modeIndex\":"<<m
     <<",\"propertyOverride\":"<<property_.load()<<",\"enabled\":"<<(active()?"true":"false")
     <<",\"conventions\":{\"mvY\":"<<mvY_.load()<<",\"mvSign\":"<<mvSign_.load()<<",\"mvZ\":"<<mvZ_.load()<<",\"engineY\":"<<engineY_.load()
     <<",\"depthMode\":"<<depthMode_.load()<<",\"deltaMode\":"<<deltaMode_.load()<<",\"skipPolicy\":"<<skip_.load()<<'}'
     <<",\"compensatedResubmits\":"<<compensatedResubmits.load()
     <<",\"variant\":{\"zeroMotion\":"<<(spacewarp::variantZeroMotion(m)?"true":"false")<<",\"farDepth\":"<<(spacewarp::variantFarDepth(m)?"true":"false")
     <<",\"mvSign\":"<<spacewarp::variantSign(m)<<",\"mvY\":"<<spacewarp::variantY(m)<<'}'
     <<",\"cut\":{\"metres\":"<<cutMetres()<<",\"degrees\":"<<cutDegrees()<<'}'
     <<",\"framesSubmitted\":"<<submitted_.load()<<",\"withInfo\":"<<withInfo_.load()<<",\"skipped\":"<<skipped_.load()
     <<",\"resubmits\":"<<resubmits_.load()<<",\"withoutInfo\":"<<withoutInfo_.load()<<",\"predictedDisplayPeriodNs\":"<<predictedPeriod_.load()
     <<",\"runtimeSpacewarpMode\":"<<runtimeMode_.load()<<",\"metricResult\":"<<metricResult_.load()<<",\"metricPolls\":"<<metricPolls_.load();
    { // Proof of the half-rate assumption, measured while space-warp info was chained
        const auto xi=intervalWithInfo_.load(),p=periodWithInfo_.load(),pdp=predictedWithInfo_.load();
        const char* verdict=!withInfo_.load()||p<=0||xi<=0?"\"unknown\"":(xi>p+p/2||pdp>p+p/2)?"true":xi<p+p/4?"false":"\"unknown\"";
        const char* why=issue();
        o<<",\"synthesizing\":"<<(synthesizing()?"true":"false")<<",\"lockForced\":"<<(lockForced()?"true":"false")
         <<",\"issue\":\""<<(why?why:"")<<"\",\"lastProduce\":"<<lastProduce_.load()
         <<",\"xr\":{\"intervalNs\":"<<xi<<",\"displayPeriodNs\":"<<p<<",\"predictedDisplayPeriodNs\":"<<pdp
         <<",\"currentIntervalNs\":"<<framePacer.xrIntervalNs()<<",\"currentPredictedDisplayPeriodNs\":"<<framePacer.predictedPeriodNs()<<'}'
         <<",\"halfRateByRuntime\":"<<verdict;
    }
    { // GPU warpDepth lane (DETAIL): capture copy, slot depth (downsample/copy), per-frame total
        const auto capture=depthCapture_.snapshot().json(),slot=depthSlot_.snapshot().json(),total=depthTotal_.snapshot().json();
        o<<",\"warpDepthGpu\":{\"capture\":"<<capture<<",\"slot\":"<<slot<<",\"total\":"<<total<<'}';
    }
    const auto now=diagnostics::monotonicNs();const auto until=suspendUntil_.load(std::memory_order_relaxed);
    std::uint64_t suspensionsTotal=0;for(const auto& s:perMode_)suspensionsTotal+=s.suspensions.load(std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    o<<",\"budget\":{\"guard\":"<<(guardEnabled_.load()?"true":"false")<<",\"overBudget\":"<<(now<until?"true":"false")
     <<",\"remainingMs\":"<<(now<until?double(until-now)/1e6:0.)<<",\"suspensions\":"<<suspensionsTotal<<",\"suspensionsSinceModeChange\":"<<guard_.trips
     <<",\"lastSuspendMs\":"<<double(guard_.lastSuspendNs)/1e6
     <<",\"nextBackoffMs\":"<<double(guard_.backoff)/1e6
     <<",\"cadence\":{\"window\":"<<spacewarp::CadenceGuard::window<<",\"maxRepeats\":"<<spacewarp::CadenceGuard::maxRepeats
     <<",\"samples\":"<<guard_.samples<<",\"repeats\":"<<guard_.repeats<<",\"counted\":"<<guard_.counted<<"}}";
    o<<",\"byMode\":{";
    for(unsigned k=1;k<modes;++k){const auto& s=perMode_[k];
        o<<(k>1?",":"")<<'"'<<modeName(k)<<"\":{\"xrFrames\":"<<s.xrFrames.load()<<",\"newPairs\":"<<s.newPairs.load()<<",\"resubmits\":"<<s.resubmits.load()
         <<",\"renderGaps\":"<<s.renderGaps.load()<<",\"cuts\":"<<s.cuts.load()<<",\"suspensions\":"<<s.suspensions.load()
         <<",\"mvCentreNdc\":"<<mvCentre_[k].json()<<'}';}
    o<<'}';
    o<<",\"deltaPose\":{\"translationMm\":"<<translationMm_.json()<<",\"rotationDeg\":"<<rotationDeg_.json()<<"}}";
    return o.str();
}
// ------------------------------------------------------------------------------------------ GPU
inline void SpaceWarp::base() {
    if(setLayout_)return;
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    checkVk(vkCreateSampler(device_,&sampler,nullptr,&sampler_),"spacewarp sampler");
    // Binding 1 = the actor block of the motion-vector pass (the depth downsample never reads it).
    const VkDescriptorSetLayoutBinding binding[2]{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ds.bindingCount=2;ds.pBindings=binding;
    checkVk(vkCreateDescriptorSetLayout(device_,&ds,nullptr,&setLayout_),"spacewarp descriptor layout");
    // Per slot and eye one set for the MV pass and one for the depth downsample.
    const VkDescriptorPoolSize size[2]{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4*slotCount},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2*slotCount}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=4*slotCount;pool.poolSizeCount=2;pool.pPoolSizes=size;
    checkVk(vkCreateDescriptorPool(device_,&pool,nullptr,&pool_),"spacewarp descriptor pool");
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,80};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&setLayout_;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
    checkVk(vkCreatePipelineLayout(device_,&pl,nullptr,&layout_),"spacewarp pipeline layout");
    createActorBuffer();
}
// Device-local uniform buffer for every slot's actor block (written by vkCmdUpdateBuffer, read by the MV pass).
inline void SpaceWarp::createActorBuffer() {
    if(actorBuffer_)return;
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};b.size=VkDeviceSize(spacewarp::gpuSlotStride)*slotCount;
    b.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(!blocks_)blocks_=std::make_unique<std::array<spacewarp::GpuActors,2>>();
    if(!boneSets_)boneSets_=std::make_unique<std::array<spacewarp::BoneSet,2>>();
    if(!boneCache_)boneCache_=std::make_unique<spacewarp::BonePairCache>();
    checkVk(vkCreateBuffer(device_,&b,nullptr,&actorBuffer_),"spacewarp actor buffer");
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device_,actorBuffer_,&req);
    VkPhysicalDeviceMemoryProperties mem{};vkGetPhysicalDeviceMemoryProperties(physical_,&mem);
    std::uint32_t type=UINT32_MAX;
    for(VkMemoryPropertyFlags want:{VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),VkMemoryPropertyFlags(0)}){
        for(std::uint32_t i=0;i<mem.memoryTypeCount&&type==UINT32_MAX;++i)
            if((req.memoryTypeBits&(1u<<i))&&(mem.memoryTypes[i].propertyFlags&want)==want)type=i;
        if(type!=UINT32_MAX)break;}
    if(type==UINT32_MAX){vkDestroyBuffer(device_,actorBuffer_,nullptr);actorBuffer_={};throw std::runtime_error("spacewarp actor buffer memory type");}
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=req.size;a.memoryTypeIndex=type;
    if(vkAllocateMemory(device_,&a,nullptr,&actorMemory_)!=VK_SUCCESS){vkDestroyBuffer(device_,actorBuffer_,nullptr);actorBuffer_={};throw std::runtime_error("spacewarp actor buffer memory");}
    if(vkBindBufferMemory(device_,actorBuffer_,actorMemory_,0)!=VK_SUCCESS){vkDestroyBuffer(device_,actorBuffer_,nullptr);vkFreeMemory(device_,actorMemory_,nullptr);
        actorBuffer_={};actorMemory_={};throw std::runtime_error("spacewarp actor buffer bind");}
}
inline unsigned SpaceWarp::buildActors(const SpaceWarpFrame& cur,const SpaceWarpFrame& prev,unsigned e,spacewarp::GpuActors& out,spacewarp::ActorTrace* trace) noexcept {
    spacewarp::ActorStats st{};
    // Player body: the engine flags the player, so the actor scan never returns him (not even with
    // debug.rdrvr.mvactors.player 1) and his body would get the static-world vectors: a body that walks
    // WITH the camera would be extrapolated as if the camera walked past it. Synthetic actor riding the
    // app space: Mcur = feet under the eyes, Mprev = WfL_prev * inv(WfL_cur) * Mcur, so the block's
    // D = WfL_prev * inv(WfL_cur) (head motion only, no locomotion). Standing still it is 'still' (no
    // block: static-world vectors are right then). Inserted first (rank 0) in copies of both sets.
    spacewarp::ActorSet curSet=cur.actors,prevSet=prev.actors;const spacewarp::ActorSet* ca=&cur.actors;const spacewarp::ActorSet* pa=&prev.actors;
    if(spaceWarpControl.mvPlayer.load(std::memory_order_relaxed)&&cur.actors.serial&&prev.actors.serial&&cur.actors.serial!=prev.actors.serial){
        spacewarp::M4 wc{},wp{},iwc{};for(int i=0;i<16;++i){wc[i]=cur.worldFromLocal[i];wp[i]=prev.worldFromLocal[i];}
        const double upm=cur.unitsPerMetre>0?cur.unitsPerMetre:1.0;
        if(wc[15]!=0&&wp[15]!=0&&spacewarp::invert(wc,iwc)){
            spacewarp::M4 mc=spacewarp::identity();
            mc[12]=(double(cur.position[0][0])+cur.position[1][0])/2;mc[13]=(double(cur.position[0][1])+cur.position[1][1])/2-1.5*upm;mc[14]=(double(cur.position[0][2])+cur.position[1][2])/2;
            const spacewarp::M4 mp=spacewarp::mul(wp,spacewarp::mul(iwc,mc));bool ok=true;
            spacewarp::ActorPose pc{},pp{};pc.guid=pp.guid=0xFFFFFF01u;pc.race=pp.race=-2;
            for(int c=0;c<4;++c)for(int r=0;r<3;++r){pc.m[c*3+r]=float(mc[c*4+r]);pp.m[c*3+r]=float(mp[c*4+r]);ok=ok&&std::isfinite(pp.m[c*3+r]);}
            const auto put=[](spacewarp::ActorSet& s,const spacewarp::ActorPose& a){
                const unsigned n=std::min<unsigned>(s.count,spacewarp::maxActors-1);for(unsigned i=n;i>0;--i)s.actor[i]=s.actor[i-1];s.actor[0]=a;s.count=n+1;};
            if(ok){put(curSet,pc);put(prevSet,pp);ca=&curSet;pa=&prevSet;++playerBlocks_;}
        }
    }
    const spacewarp::BoneSet* cb=boneSets_&&(*boneSets_)[0].count?&(*boneSets_)[0]:nullptr;const spacewarp::BoneSet* pb=boneSets_&&(*boneSets_)[1].count?&(*boneSets_)[1]:nullptr;
    const unsigned n=spacewarp::buildActorBlock({cur.relClip[e].data(),cur.position[e].data(),ca,cb},{prev.relClip[e].data(),prev.position[e].data(),pa,pb},
        cur.unitsPerMetre,e==0,out,st,trace,spaceWarpControl.actorPadSteps(),boneCache_.get());
    actorSkipUnmatched_+=st.unmatched;actorSkipJump_+=st.jump;actorSkipStill_+=st.still;actorSkipOffscreen_+=st.offscreen;actorSkipMath_+=st.math;
    actorMaxStepM_=std::max(actorMaxStepM_,st.maxStepM);
    actorSkipBehind_+=st.behind;actorClipped_+=st.clipped;actorStillPinned_+=st.stillPinned;
    boneActors_+=st.bones.actors;boneParts_+=st.bones.parts;boneUnmatched_+=st.bones.unmatched;boneJump_+=st.bones.jump;boneOverflow_+=st.bones.overflow;
    boneMath_+=st.bones.math;boneMaxStepM_=std::max(boneMaxStepM_,st.bones.maxStepM);
    return n;
}
// Depth-only pass writing the slot depth at the motion-vector rect from the sampled capture depth.
// Chooses a slot format that is both a depth attachment and sampleable; false = keep the full-size copy.
inline bool SpaceWarp::buildDownsample() noexcept {
    if(downPipeline_)return true;
    const auto mv=spaceWarpControl.extent();if(!mv.width||!mv.height){downStatus_="NO_EXTENT";return false;}
    const VkFormat format=swdetail::slotDepthFormat(physical_); // Same choice as SpaceWarpDirect::targetFormat
    if(format==VK_FORMAT_UNDEFINED){downStatus_="NO_FORMAT";return false;}
    VkShaderModule vs{},fs{};
    try{
        VkAttachmentDescription att{};att.format=format;att.samples=VK_SAMPLE_COUNT_1_BIT;att.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;att.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;att.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference ds{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.pDepthStencilAttachment=&ds;
        std::array<VkSubpassDependency,2> dep{};
        dep[0].srcSubpass=VK_SUBPASS_EXTERNAL;dep[0].dstSubpass=0;dep[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dep[0].dstStageMask=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        dep[0].dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
        dep[1].srcSubpass=0;dep[1].dstSubpass=VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;dep[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dep[1].srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;dep[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_MEMORY_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&att;rp.subpassCount=1;rp.pSubpasses=&sub;
        rp.dependencyCount=std::uint32_t(dep.size());rp.pDependencies=dep.data();
        checkVk(vkCreateRenderPass(device_,&rp,nullptr,&downPass_),"spacewarp depth pass");
        VkShaderModuleCreateInfo mod{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mod.codeSize=sizeof(spacewarp::shaders::vertex);mod.pCode=spacewarp::shaders::vertex;
        checkVk(vkCreateShaderModule(device_,&mod,nullptr,&vs),"spacewarp depth vertex shader");
        mod.codeSize=sizeof(spacewarp::shaders::depthDownsample);mod.pCode=spacewarp::shaders::depthDownsample;
        checkVk(vkCreateShaderModule(device_,&mod,nullptr,&fs),"spacewarp depth fragment shader");
        VkPipelineShaderStageCreateInfo stages[2]{};stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[0].pName="main";
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;stages[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable=VK_TRUE;dss.depthWriteEnable=VK_TRUE;dss.depthCompareOp=VK_COMPARE_OP_ALWAYS; // gl_FragDepth for every pixel
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; // no colour attachment
        const VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;
        pi.pViewportState=&vp;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pDepthStencilState=&dss;pi.pColorBlendState=&cb;pi.pDynamicState=&dy;pi.layout=layout_;pi.renderPass=downPass_;
        checkVk(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&pi,nullptr,&downPipeline_),"spacewarp depth pipeline");
    }catch(...){
        if(downPipeline_)vkDestroyPipeline(device_,downPipeline_,nullptr);
        if(downPass_)vkDestroyRenderPass(device_,downPass_,nullptr);
        downPipeline_={};downPass_={};++downsampleFailures_;downStatus_="BUILD_FAILED";
    }
    if(vs)vkDestroyShaderModule(device_,vs,nullptr);
    if(fs)vkDestroyShaderModule(device_,fs,nullptr);
    if(!downPipeline_)return false;
    downFormat_=format;downStatus_="READY";return true;
}
inline void SpaceWarp::createSlotImages(Slot& s,VkFormat format,Extent extent,bool down) {
    VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(physical_,format,&p);
    if(!(p.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))throw std::runtime_error("spacewarp depth format not sampleable");
    // Downsample target = depth attachment at the MV rect; copy target = transfer destination (full size).
    // TRANSFER_SRC: read-back by the host GPU test / depth dumps. TRANSFER_DST also on the downsample target (a
    // depth-direct pair is copied in at the MV rect; both kinds of pair share the slot images, so depthdirect can
    // flip without a rebuild).
    const VkImageUsageFlags usage=(down?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:0)|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    s.format=format;s.extent=extent;s.downsample=down;
    for(unsigned e=0;e<2;++e)s.depth[e]=createOwnedImage(physical_,device_,extent,format,usage,VK_IMAGE_ASPECT_DEPTH_BIT,true);
    ++slotImages_;
    if(!s.sets[0]){
        std::array<VkDescriptorSetLayout,4> layouts{setLayout_,setLayout_,setLayout_,setLayout_};std::array<VkDescriptorSet,4> sets{};
        VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};a.descriptorPool=pool_;a.descriptorSetCount=4;a.pSetLayouts=layouts.data();
        checkVk(vkAllocateDescriptorSets(device_,&a,sets.data()),"spacewarp descriptor sets");
        s.sets={sets[0],sets[1]};s.downSets={sets[2],sets[3]};
    }
    const auto slotIndex=VkDeviceSize(&s-slots_.data());
    for(unsigned e=0;e<2;++e){
        VkDescriptorImageInfo info{sampler_,s.depth[e].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo actors{actorBuffer_,slotIndex*spacewarp::gpuSlotStride+e*spacewarp::gpuEyeStride,sizeof(spacewarp::GpuActors)};
        std::array<VkWriteDescriptorSet,2> w{};
        w[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w[0].dstSet=s.sets[e];w[0].descriptorCount=1;w[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[0].pImageInfo=&info;
        w[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w[1].dstSet=s.sets[e];w[1].dstBinding=1;w[1].descriptorCount=1;w[1].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;w[1].pBufferInfo=&actors;
        vkUpdateDescriptorSets(device_,2,w.data(),0,nullptr);
    }
    if(!down)return;
    for(unsigned e=0;e<2;++e){
        VkImageView view=s.depth[e].view;
        if(swdetail::hasStencil(format)){ // a combined depth/stencil attachment view covers both aspects
            VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=s.depth[e].image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=format;
            v.subresourceRange={swdetail::depthAspects(format),0,1,0,1};
            checkVk(vkCreateImageView(device_,&v,nullptr,&s.attach[e]),"spacewarp depth attachment view");view=s.attach[e];
        }
        VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};f.renderPass=downPass_;f.attachmentCount=1;f.pAttachments=&view;
        f.width=extent.width;f.height=extent.height;f.layers=1;checkVk(vkCreateFramebuffer(device_,&f,nullptr,&s.downFramebuffer[e]),"spacewarp depth framebuffer");
    }
}
inline void SpaceWarp::destroySlot(Slot& s) noexcept {
    for(auto& fb:s.downFramebuffer){if(fb)vkDestroyFramebuffer(device_,fb,nullptr);fb={};}
    for(auto& v:s.attach){if(v)vkDestroyImageView(device_,v,nullptr);v={};}
    for(auto& image:s.depth)destroyOwnedImage(device_,image);
    s=Slot{}; // descriptor sets are freed with the pool
}
// warpDepth timestamps (slot part) around the slot depth commands; DETAIL only (profiler ticket).
inline bool SpaceWarp::timeBegin(VkCommandBuffer cmd,unsigned m,std::uint64_t frame) noexcept {
    const auto ticket=diagnostics::profilerTicket();if(!(ticket&1))return false;
    if(!queriesTried_){
        queriesTried_=true;
        std::uint32_t count{};vkGetPhysicalDeviceQueueFamilyProperties(physical_,&count,nullptr);
        std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(physical_,&count,families.data());
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical_,&props);
        if(family_<count&&families[family_].timestampValidBits&&props.limits.timestampPeriod>0){
            const auto bits=families[family_].timestampValidBits;tsMask_=bits>=64?UINT64_MAX:(1ull<<bits)-1;tsPeriod_=props.limits.timestampPeriod;
            VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=2*slotCount;
            if(vkCreateQueryPool(device_,&q,nullptr,&queries_)!=VK_SUCCESS)queries_={};
        }
    }
    if(!queries_)return false;
    vkCmdResetQueryPool(cmd,queries_,2*m,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries_,2*m);
    slots_[m].timedTicket=ticket;slots_[m].timedFrame=frame;return true;
}
inline void SpaceWarp::completed(unsigned m) noexcept {
    if(m>=slotCount)return;auto& s=slots_[m];
    if(dump_.request&&dump_.slot==int(m)&&dump_.producerRecorded&&!dump_.producerDone){dump_.producerDone=true;dumpPoll();}
    if(dump_.request&&dump_.prevSlot==int(m)&&dump_.prevRecorded&&!dump_.prevDone){dump_.prevDone=true;dumpPoll();} // Pre-roll copy landed
    if(!s.timed)return;s.timed=false;
    if(!queries_||!diagnostics::profilerTicketValid(s.timedTicket))return;
    struct Value {std::uint64_t ticks,available;};std::array<Value,2> v{};
    const auto r=vkGetQueryPoolResults(device_,queries_,2*m,2,sizeof(v),v.data(),sizeof(Value),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if(r!=VK_SUCCESS||!v[0].available||!v[1].available){++timingUnavailable_;return;}
    const double ns=double((v[1].ticks-v[0].ticks)&tsMask_)*tsPeriod_;
    if(!(ns>=0)||ns>1e9){++timingUnavailable_;return;}
    spaceWarpControl.noteDepthGpu(1,s.timedFrame,std::max<std::uint64_t>(1,std::uint64_t(ns)),s.timedDirect);
}
inline void SpaceWarp::produce(VkCommandBuffer cmd,unsigned m,const SpaceWarpSource* src,const FrameContext& frame) {
    if(m>=slotCount)return;
    auto& s=slots_[m];s.valid=s.skip=s.deltaValid=false;s.timed=false;
    // OFF, or suspended by the budget guard: no producer work, no history across the gap.
    if(!spaceWarpControl.producing()){if(spaceWarpControl.active())++suspendedPairs_;previous_.valid=false;return;}
    if(!src||!src->frame.valid||!src->depth[0]||!src->depth[1]||!src->extent.width||!src->extent.height||
       src->frame.frame!=frame.renderFrameId){++noSource_;previous_.valid=false;spaceWarpControl.noteProduce(2);return;}
    if(!device_||broken_){previous_.valid=false;return;}
    const bool direct=src->direct; // Already the MV-rect slot depth (SpaceWarpDirect): copy only
    const bool sampledSource=!direct&&src->views[0]&&src->views[1]&&src->layout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    try{
        base();
        if(!path_)path_=(sampledSource||direct)&&buildDownsample()?1:2; // decided once per GPU generation (discardGpu)
        const bool down=path_==1;
        const VkFormat format=down?downFormat_:src->format;const Extent extent=down?spaceWarpControl.extent():src->extent;
        // A direct pair needs the downsample slot images at its own format/extent; otherwise the capture side is told to
        // take the full-size copy from the next pair on (refused until discardGpu), and this pair goes without motion data.
        if(direct&&(!down||src->format!=format||!(src->extent==extent)||src->layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)){
            ++directMismatches_;++formatMismatch_;spaceWarpControl.refuseDirect(true);previous_.valid=false;spaceWarpControl.noteProduce(3);return;}
        if((down&&!sampledSource&&!direct)||(s.depth[0].image&&(s.format!=format||!(s.extent==extent)||s.downsample!=down))){
            ++formatMismatch_;previous_.valid=false;spaceWarpControl.noteProduce(3);return;}
        if(!s.depth[0].image)createSlotImages(s,format,extent,down);
    }catch(const std::exception&){fail("SLOT_IMAGES_FAILED","GPU FAILED: SLOT IMAGES");previous_.valid=false;return;}
    const auto& cur=src->frame;const auto& prev=previous_.data;
    // Motion vectors only across exactly one rendered frame; a gap takes the no-history / FRAME_SKIP path.
    const bool sameSpace=previous_.valid&&previous_.epochs.tracking==frame.epochs.tracking&&previous_.epochs.referenceSpace==frame.epochs.referenceSpace;
    const bool gap=sameSpace&&!spacewarp::consecutiveFrames(previous_.frame,cur.frame);
    if(gap)++renderGaps_;
    bool ok=sameSpace&&!gap,cut=false;double centre=-1;
    for(unsigned e=0;e<2&&ok;++e)ok=spacewarp::reprojection(cur.relClip[e].data(),cur.position[e].data(),prev.relClip[e].data(),prev.position[e].data(),s.reproject[e]);
    if(ok){
        spacewarp::M4 a{},b{};for(int i=0;i<16;++i){a[i]=prev.worldFromLocal[i];b[i]=cur.worldFromLocal[i];}
        s.deltaValid=spacewarp::deltaPose(a,b,cur.unitsPerMetre>0?cur.unitsPerMetre:1.0,s.delta);
        // A camera cut / teleport / respawn / snap turn (beyond cutcm / cutdeg): no extrapolation from this frame.
        if(s.deltaValid&&spacewarp::cameraCut(s.delta,spaceWarpControl.cutMetres(),spaceWarpControl.cutDegrees())){s.skip=true;++cuts_;cut=true;}
        centre=spacewarp::centreMotion(s.reproject[0],spaceWarpControl.conventions(cur.projectionY)); // camera motion, before ZERO MV
    }else{
        ++noHistory_;s.skip=true;
        for(auto& r:s.reproject)r=spacewarp::identityReprojection(); // identity: zero motion
        s.delta={};s.deltaValid=false;
    }
    spaceWarpControl.noteProduced(gap,cut,centre);
    // AppSW dump: a request (eye dump / debug.rdrvr.spacewarp.dump) starts a one-shot dump; a depthdual pair dumps THIS pair.
    // A request first pre-rolls on this pair (its slot depth = the next pair's history, "prevDepth"); the next consecutive
    // pair is dumped. A gap, a cut or a slot format change restarts the pre-roll (8 tries).
    const bool dualPair=direct&&src->dualDepth[0]&&src->dualDepth[1]&&src->dualViews[0]&&src->dualViews[1]&&src->dualExtent.width&&src->dualExtent.height;
    bool dumping=false,prerolling=false;
    const auto restart=[&]{const auto r=dump_.request;const unsigned tries=dump_.tries+1;dump_.request=0;prerolling=dumpStart(m,r,false,true);dump_.tries=tries;++dump_.retries;};
    if(dump_.request&&dump_.slot==int(m)&&!dump_.consumerRecorded){ // the dumped pair was never displayed: its slot is reused now
        ++dump_.abandoned;
        if(dump_.dual||dump_.tries>=8)dumpFinish("skipped");       // depthdual: the producer half is the point
        else restart();                                            // A new pre-roll on this pair
    }else if(dump_.request&&dump_.preroll){
        if(ok&&!cut&&dump_.prevRecorded&&previous_.frame==dump_.prevFrame&&s.format==dump_.planFormat&&s.extent==dump_.planExtent){
            dump_.preroll=false;dump_.slot=int(m);dumping=true;}  // this pair's history is the pre-rolled pair
        else if(dump_.tries>=8)dumpFinish("skipped");
        else restart();
    }
    if(const auto want=spacewarp::claim(spacewarp::dumpRequests,spacewarp::dumpTaken);!dumping&&!prerolling&&(want||dualPair)){
        if(dump_.request)++dump_.busy;
        else if(dualPair)dumping=dumpStart(m,want?want:spacewarp::dualTaken.load(std::memory_order_relaxed),true);
        else prerolling=dumpStart(m,want,false,true);
    }
    if(prerolling)dump_.prevFrame=cur.frame;
    // This slot's actor blocks (both eyes), GPU-ordered with the slot depth (same producer command buffer).
    static std::array<spacewarp::GpuActors,2> noBlocks{}; // Only without an actor buffer (never written)
    auto& blocks=blocks_?*blocks_:noBlocks;blocks[0].header[0]=blocks[1].header[0]=blocks[0].header[1]=blocks[1].header[1]=0;unsigned blocksOn=0;
    std::vector<spacewarp::ActorTrace> traces;if(dumping)try{traces.resize(2*spacewarp::maxActors);}catch(...){} // Only for a dump
    if(actorBuffer_){
        const bool actors=ok&&!cut&&spacewarp::variantActors(spaceWarpControl.mode());blocksOn=actors?1u:0u;
        if(!actors&&ok)++actorPairsOff_;
        if(actors&&cur.actors.serial&&cur.actors.serial==prev.actors.serial)++actorSameSerial_;
        if(actors&&cur.actors.serial&&prev.actors.serial) // Main-thread ticks between the two snapshots (1 = exact pairing)
            ++actorSerialDelta_[std::min<std::uint64_t>(cur.actors.serial>=prev.actors.serial?cur.actors.serial-prev.actors.serial:3,3)];
        bool any=false,anyParts=false;
        if(actors)++boneModePairs_[std::min(cur.actors.boneMode,3u)]; // Pairs per debug.rdrvr.mvbones mode
        if(boneSets_){auto& bs=*boneSets_;bs[0].count=bs[1].count=0;bs[0].serial=bs[1].serial=0; // This pair's parts, both snapshots
            if(actors&&cur.actors.boneCount&&prev.actors.boneCount&&cur.actors.serial!=prev.actors.serial&&
               !(spacewarp::actorChannel.bones(cur.actors.serial,bs[0])&&spacewarp::actorChannel.bones(prev.actors.serial,bs[1]))){++boneMissing_;bs[0].count=bs[1].count=0;}}
        for(unsigned e=0;e<2;++e){
            auto& block=blocks[e];
            const unsigned n=actors?buildActors(cur,prev,e,block,traces.empty()?nullptr:traces.data()+e*spacewarp::maxActors):0u;block.header[0]=float(n);actorsLastEye_[e]=n;any=any||n;
            actorsWritten_+=n;actorsMaxEye_=std::max(actorsMaxEye_,n);
            const unsigned parts=n?std::min(unsigned(block.header[1]),spacewarp::maxGpuBones):0u;if(!n)block.header[1]=0;
            bonePartsWritten_+=parts;bonesLastEye_[e]=parts;bonesMaxEye_=std::max(bonesMaxEye_,parts);anyParts=anyParts||parts;
            const VkDeviceSize bytes=16+VkDeviceSize(n)*sizeof(spacewarp::GpuActor),base=VkDeviceSize(m)*spacewarp::gpuSlotStride+e*spacewarp::gpuEyeStride;
            vkCmdUpdateBuffer(cmd,actorBuffer_,base,bytes,&block);
            if(parts)vkCmdUpdateBuffer(cmd,actorBuffer_,base+spacewarp::gpuBoneOffset,VkDeviceSize(parts)*sizeof(spacewarp::GpuBone),block.bone);
        }
        if(any)++actorPairs_;
        if(anyParts)++bonePairs_;
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};bb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask=VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_MEMORY_READ_BIT;bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        bb.buffer=actorBuffer_;bb.offset=VkDeviceSize(m)*spacewarp::gpuSlotStride;bb.size=spacewarp::gpuSlotStride;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,1,&bb,0,nullptr);
    }
    const float scale=cur.unitsPerMetre>0?cur.unitsPerMetre:1.f;
    s.nearZ=cur.nearClip/scale;s.farZ=cur.farClip/scale;s.projectionY=cur.projectionY;
    if(dumping)try{ // The pair's CPU side, before previous_ (= `prev`) is overwritten below
        namespace sw=spacewarp;std::ostringstream o;o.precision(9);
        const auto num=[&](double v){if(std::isfinite(v))o<<v;else o<<"null";};
        const auto arr=[&](const float* v,unsigned n){o<<'[';for(unsigned i=0;i<n;++i){if(i)o<<',';num(v[i]);}o<<']';};
        const auto conv=spaceWarpControl.conventions(cur.projectionY);const unsigned variant=spaceWarpControl.mode();
        o<<"\"frame\":"<<cur.frame<<",\"slot\":"<<m<<",\"prevFrame\":"<<previous_.frame<<",\"history\":"<<(ok?"true":"false")<<",\"cut\":"<<(cut?"true":"false")
         <<",\"skip\":"<<(s.skip?"true":"false")<<",\"path\":\""<<(direct?"direct":s.downsample?"downsample":"copy")<<"\",\"dual\":"<<(dump_.dual?"true":"false")
         <<",\"variant\":"<<variant<<",\"variantName\":\""<<sw::variantName(variant)<<"\",\"actorsOn\":"<<blocksOn<<",\"actorPad\":"<<spaceWarpControl.actorPadSteps()
         <<",\"conventions\":{\"engineY\":"<<conv.engineY<<",\"mvY\":"<<conv.mvY<<",\"mvSign\":"<<conv.mvSign<<",\"mvZ\":"<<conv.mvZ<<'}'
         <<",\"projectionY\":";num(cur.projectionY);o<<",\"unitsPerMetre\":";num(cur.unitsPerMetre);o<<",\"nearZ\":";num(s.nearZ);o<<",\"farZ\":";num(s.farZ);
        o<<",\"slotFormat\":"<<int(s.format)<<",\"slotExtent\":["<<s.extent.width<<','<<s.extent.height<<']';
        o<<",\"reproject\":[";arr(s.reproject[0].data(),16);o<<',';arr(s.reproject[1].data(),16);o<<']';
        o<<",\"pushedReproject\":[";arr(sw::pushedReprojection(variant,s.reproject[0]).data(),16);o<<',';arr(sw::pushedReprojection(variant,s.reproject[1]).data(),16);o<<']';
        o<<",\"relClipCur\":[";arr(cur.relClip[0].data(),16);o<<',';arr(cur.relClip[1].data(),16);o<<"],\"relClipPrev\":[";arr(prev.relClip[0].data(),16);o<<',';arr(prev.relClip[1].data(),16);
        o<<"],\"posCur\":[";arr(cur.position[0].data(),3);o<<',';arr(cur.position[1].data(),3);o<<"],\"posPrev\":[";arr(prev.position[0].data(),3);o<<',';arr(prev.position[1].data(),3);
        o<<"],\"deltaPose\":{\"valid\":"<<(s.deltaValid?"true":"false")<<",\"q\":["<<s.delta.q[0]<<','<<s.delta.q[1]<<','<<s.delta.q[2]<<','<<s.delta.q[3]
         <<"],\"p\":["<<s.delta.p[0]<<','<<s.delta.p[1]<<','<<s.delta.p[2]<<"]}";
        const auto poses=[&](const sw::ActorSet& set,const sw::BoneSet* bset){o<<'[';for(unsigned i=0;i<set.count&&i<sw::maxActors;++i){const auto& a=set.actor[i];
            o<<(i?",":"")<<"{\"guid\":"<<a.guid<<",\"race\":"<<a.race<<",\"m\":";arr(a.m,12);
            if(bset&&bset->serial==set.serial&&a.bones>=0&&unsigned(a.bones)<bset->count&&unsigned(a.bones)<sw::maxBoneActors){const auto& bs=bset->bones[unsigned(a.bones)];
                o<<",\"bones\":{\"layout\":"<<bs.layout<<",\"parts\":[";
                for(unsigned k=0;k<bs.count&&k<sw::maxActorBones;++k){const auto& q=bs.part[k];o<<(k?",":"")<<"{\"cls\":\""<<sw::boneClassName(q.cls)<<"\",\"bone\":"<<q.bone<<",\"a\":";
                    arr(q.a,3);o<<",\"b\":";arr(q.b,3);o<<",\"r\":";num(q.radius);o<<",\"m\":";arr(q.m,12);o<<'}';}
                o<<"]}";}
            o<<'}';}o<<']';};
        // why no actor got a block at all (else ""): off = no history / cut / ZERO MV / CAM ONLY; sameSerial = the main thread did not tick
        const char* none=!blocksOn?"off":!cur.actors.count?"noActors":!prev.actors.count?"noPrevActors":cur.actors.serial==prev.actors.serial?"sameSerial":"";
        o<<",\"actors\":{\"serialCur\":"<<cur.actors.serial<<",\"serialPrev\":"<<prev.actors.serial<<",\"none\":\""<<none<<"\",\"boneMode\":"<<cur.actors.boneMode<<",\"boneMissing\":"<<boneMissing_<<",\"cur\":";
        poses(cur.actors,boneSets_?&(*boneSets_)[0]:nullptr);
        o<<",\"prev\":";poses(prev.actors,boneSets_?&(*boneSets_)[1]:nullptr);o<<",\"eyes\":[";
        for(unsigned e=0;e<2;++e){const auto& b=blocks[e];const unsigned n=unsigned(b.header[0]);
            o<<(e?",":"")<<"{\"blocks\":"<<n<<",\"trace\":[";bool first=true;
            if(blocksOn&&!traces.empty())for(unsigned i=0;i<cur.actors.count&&i<sw::maxActors;++i){const auto& t=traces[e*sw::maxActors+i];if(!t.guid)continue; // not reached (early out)
                o<<(first?"":",")<<"{\"index\":"<<i<<",\"guid\":"<<t.guid<<",\"race\":"<<t.race<<",\"result\":\""<<sw::actorResultName(t.result)<<"\",\"block\":"<<t.block<<",\"stepM\":";
                num(t.stepM);o<<",\"angleDeg\":";num(t.angleDeg);o<<",\"bones\":"<<t.bones<<",\"boneStepM\":";num(t.boneStepM);o<<'}';first=false;}
            o<<"],\"block\":[";
            for(unsigned i=0;i<n&&i<sw::maxGpuActors;++i){const auto& g=b.actor[i];o<<(i?",":"")<<"{\"rect\":";arr(g.rect,4);o<<",\"shape\":";arr(g.shape,4);
                o<<",\"toLocal\":";arr(g.toLocal,16);o<<",\"toPrev\":";arr(g.toPrev,16);o<<",\"bones\":";arr(g.bones,4);o<<'}';}
            const unsigned np=n?std::min(unsigned(b.header[1]),sw::maxGpuBones):0u; // The eye's parts (block "bones" = first, count, root r^2, top)
            o<<"],\"part\":[";for(unsigned k=0;k<np;++k){const auto& g=b.bone[k];o<<(k?",":"")<<"{\"a\":";arr(g.a,4);o<<",\"b\":";arr(g.b,4);o<<",\"toPrev\":";arr(g.toPrev,16);o<<'}';}
            o<<"]}";}
        o<<"]}";
        dump_.frame=cur.frame;dump_.meta=o.str();
    }catch(...){dump_.meta="\"metaError\":true";}
    previous_.valid=true;previous_.frame=cur.frame;previous_.epochs=frame.epochs;previous_.data=cur;
    const bool timed=timeBegin(cmd,m,cur.frame);
    const auto aspect=swdetail::depthAspects(src->format);
    if(direct){
        // Pass N (earlier submission, same queue) wrote the 420x440 source in its depth-only render pass and left it in
        // TRANSFER_SRC_OPTIMAL (external dependency late tests -> transfer). One batched barrier before and after the two copies.
        const auto slotAspect=swdetail::depthAspects(s.format);
        std::array<VkImageMemoryBarrier,4> pre{};std::array<VkImageMemoryBarrier,2> post{};
        for(unsigned e=0;e<2;++e){
            pre[2*e]=swdetail::imageBarrier(src->depth[e],aspect,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
            pre[2*e+1]=swdetail::imageBarrier(s.depth[e].image,slotAspect,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
            post[e]=swdetail::imageBarrier(s.depth[e].image,slotAspect,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        }
        // The slot's previous readers (MV pass) are behind the batch's done-semaphore wait (ALL_COMMANDS); its previous writers
        // (downsample pass / copy) are fragment tests / transfer.
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,std::uint32_t(pre.size()),pre.data());
        for(unsigned e=0;e<2;++e){
            VkImageCopy c{};c.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};c.dstSubresource=c.srcSubresource;c.extent={s.extent.width,s.extent.height,1};
            vkCmdCopyImage(cmd,src->depth[e],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.depth[e].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
        }
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,std::uint32_t(post.size()),post.data());
        ++directCopies_;
    }else if(s.downsample){
        // Farthest-of-2x2 downsample into the slot depth at the MV rect (render pass: UNDEFINED -> SHADER_READ_ONLY).
        // The slot's previous producer command buffer completed (acquireSlot), so its downSets are free to update.
        std::array<VkDescriptorImageInfo,2> info{};std::array<VkWriteDescriptorSet,2> writes{};
        for(unsigned e=0;e<2;++e){info[e]={sampler_,src->views[e],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            auto& w=writes[e];w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=s.downSets[e];w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&info[e];}
        vkUpdateDescriptorSets(device_,2,writes.data(),0,nullptr);
        for(unsigned e=0;e<2;++e){
            // Pass N (earlier submission, same queue) wrote the source with transfer commands and left it sampled.
            swdetail::barrier(cmd,src->depth[e],aspect,src->layout,src->layout,VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
            VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=downPass_;b.framebuffer=s.downFramebuffer[e];b.renderArea.extent={s.extent.width,s.extent.height};
            vkCmdBeginRenderPass(cmd,&b,VK_SUBPASS_CONTENTS_INLINE);
            const VkViewport viewport{0,0,float(s.extent.width),float(s.extent.height),0,1};const VkRect2D scissor{{0,0},{s.extent.width,s.extent.height}};
            vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,downPipeline_);
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&s.downSets[e],0,nullptr);
            vkCmdDraw(cmd,3,1,0,0);
            vkCmdEndRenderPass(cmd);
        }
        ++downsamples_;
    }else{
        const auto slotAspect=swdetail::depthAspects(s.format);
        for(unsigned e=0;e<2;++e){
            // Pass N (earlier submission, same queue) wrote the source with transfer commands.
            swdetail::barrier(cmd,src->depth[e],aspect,src->layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
            swdetail::barrier(cmd,s.depth[e].image,slotAspect,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy c{};c.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};c.dstSubresource=c.srcSubresource;c.extent={src->extent.width,src->extent.height,1};
            vkCmdCopyImage(cmd,src->depth[e],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.depth[e].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
            swdetail::barrier(cmd,s.depth[e].image,slotAspect,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        }
        ++slotCopies_;
    }
    if(timed){vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries_,2*m+1);s.timed=true;s.timedDirect=direct;}
    if(dumping)dumpProducer(cmd,m,*src); // After the timed range (the warpDepth lane stays clean)
    else if(prerolling)dumpProducer(cmd,m,*src,true); // Only this slot's depth (the dumped pair's history)
    s.valid=true;s.frame=cur.frame;++produced_;spaceWarpControl.noteProduce(1);
}
inline void SpaceWarp::destroyPipeline() noexcept {
    if(pipeline_)vkDestroyPipeline(device_,pipeline_,nullptr);
    if(pass_)vkDestroyRenderPass(device_,pass_,nullptr);
    pipeline_={};pass_={};
}
// The motion-vector pass objects (render pass: R16G16B16A16_SFLOAT + `depth`, pipeline). Separate from screens(): the host GPU
// test builds it without OpenXR to run the real spacewarp.frag through the dump's mirror pass. Throws (partial objects: destroyPipeline).
inline void SpaceWarp::createPass(VkFormat depth) {
    if(pass_)return;
    base();
    std::array<VkAttachmentDescription,2> att{};
    att[0].format=VK_FORMAT_R16G16B16A16_SFLOAT;att[0].samples=VK_SAMPLE_COUNT_1_BIT;att[0].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;att[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;att[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;att[0].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1]=att[0];att[1].format=depth;att[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},ds{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&color;sub.pDepthStencilAttachment=&ds;
    VkSubpassDependency dep{};dep.srcSubpass=VK_SUBPASS_EXTERNAL;dep.dstSubpass=0;
    dep.srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=2;rp.pAttachments=att.data();rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=1;rp.pDependencies=&dep;
    checkVk(vkCreateRenderPass(device_,&rp,nullptr,&pass_),"spacewarp render pass");
    VkShaderModule vs{},fs{};
    try{
        VkShaderModuleCreateInfo mod{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mod.codeSize=sizeof(spacewarp::shaders::vertex);mod.pCode=spacewarp::shaders::vertex;
        checkVk(vkCreateShaderModule(device_,&mod,nullptr,&vs),"spacewarp vertex shader");
        mod.codeSize=sizeof(spacewarp::shaders::fragment);mod.pCode=spacewarp::shaders::fragment;
        checkVk(vkCreateShaderModule(device_,&mod,nullptr,&fs),"spacewarp fragment shader");
        VkPipelineShaderStageCreateInfo stages[2]{};stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[0].pName="main";
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;stages[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable=VK_TRUE;dss.depthWriteEnable=VK_TRUE;dss.depthCompareOp=VK_COMPARE_OP_ALWAYS; // gl_FragDepth written for every pixel
        VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&blend;
        const VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;
        pi.pViewportState=&vp;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pDepthStencilState=&dss;pi.pColorBlendState=&cb;pi.pDynamicState=&dy;pi.layout=layout_;pi.renderPass=pass_;
        checkVk(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline_),"spacewarp pipeline");
    }catch(...){if(vs)vkDestroyShaderModule(device_,vs,nullptr);if(fs)vkDestroyShaderModule(device_,fs,nullptr);throw;}
    vkDestroyShaderModule(device_,vs,nullptr);vkDestroyShaderModule(device_,fs,nullptr);
}
inline void SpaceWarp::screens(XrSession session) {
    if(dump_.request)dumpPoll(); // Every XR frame (StereoMailbox::render) while a dump is in flight
    if(!spaceWarpControl.active()||broken_||!device_||!session)return;
    const auto wanted=spaceWarpControl.extent();
    if(chains_&&pipeline_)return; // swapchains are fixed per session (recommended rect)
    try{
        base();
        if(!chains_){
            std::uint32_t n{};checkXr(xrEnumerateSwapchainFormats(session,0,&n,nullptr),"spacewarp format count");
            if(!n||n>256)throw std::runtime_error("spacewarp format count invalid");
            std::vector<std::int64_t> formats(n);checkXr(xrEnumerateSwapchainFormats(session,n,&n,formats.data()),"spacewarp formats");
            // A format must be offered by the runtime and support every required feature (the swapchains
            // are also SAMPLED by the compositor).
            const auto offered=[&](VkFormat f,VkFormatFeatureFlags feature){VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(physical_,f,&p);
                return std::find(formats.begin(),formats.end(),std::int64_t(f))!=formats.end()&&(p.optimalTilingFeatures&feature)==feature;};
            if(!offered(VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)){fail("MV_FORMAT_UNAVAILABLE","GPU FAILED: MV FORMAT");return;}
            VkFormat depth=VK_FORMAT_UNDEFINED;
            for(auto f:{VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_X8_D24_UNORM_PACK32,VK_FORMAT_D32_SFLOAT,VK_FORMAT_D16_UNORM,VK_FORMAT_D32_SFLOAT_S8_UINT})
                if(offered(f,VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)){depth=f;break;}
            if(depth==VK_FORMAT_UNDEFINED){fail("DEPTH_FORMAT_UNAVAILABLE","GPU FAILED: DEPTH FORMAT");return;}
            if(pass_&&depthFormat_!=depth)destroyPipeline();
            depthFormat_=depth;
        }
        const VkFormat depth=depthFormat_;
        if(!pass_)createPass(depth); // Shared with the host GPU test (dump mirror pass)
        if(!chains_)xrCopy_=spaceWarpControl.dumpXr.load(std::memory_order_relaxed)!=0; // dumpxr 1 = TRANSFER_SRC on both swapchains
        // ONE swapchain set by default: with alternating sets the compositor synthesizes no
        // frames (VrApi log "ASW=<frames/s incl. synthesized>,Type=App" shows ASW=0 although the
        // info is chained on every frame); with one set it synthesizes. Second set only on request:
        // debug.rdrvr.xrchains.warp 2 (read at creation).
        unsigned sets=1;
#if defined(__ANDROID__)
        {char v[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.xrchains.warp",v)>0&&v[0]=='2'&&!v[1])sets=2;}
#endif
        if(!chains_)for(unsigned k=0;k<2*sets;++k){
            const unsigned e=k&1u;auto& c=(k>=2?alt_:eyes_)[e];
            XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};sc.sampleCount=sc.faceCount=sc.arraySize=sc.mipCount=1;sc.width=wanted.width;sc.height=wanted.height;
            // SAMPLED as the spec / Meta guide require (the compositor samples both images).
            const XrSwapchainUsageFlags copy=xrCopy_?XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT:0;
            sc.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT|copy;sc.format=VK_FORMAT_R16G16B16A16_SFLOAT;
            if(copy&&XR_FAILED(xrCreateSwapchain(session,&sc,&c.motion))){c.motion={};xrCopy_=false;sc.usageFlags&=~copy;} // refused: created without TRANSFER_SRC
            if(!c.motion)checkXr(xrCreateSwapchain(session,&sc,&c.motion),"spacewarp motion swapchain");
            sc.usageFlags=XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT|(xrCopy_?copy:0);sc.format=depth;
            if(xrCopy_&&XR_FAILED(xrCreateSwapchain(session,&sc,&c.depth))){c.depth={};xrCopy_=false;sc.usageFlags&=~copy;}
            if(!c.depth)checkXr(xrCreateSwapchain(session,&sc,&c.depth),"spacewarp depth swapchain");
            const auto images=[&](XrSwapchain chain,std::vector<XrSwapchainImageVulkan2KHR>& out){
                std::uint32_t count{};checkXr(xrEnumerateSwapchainImages(chain,0,&count,nullptr),"spacewarp image count");
                if(!count||count>16)throw std::runtime_error("spacewarp image count invalid");
                out.assign(count,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
                checkXr(xrEnumerateSwapchainImages(chain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(out.data())),"spacewarp images");};
            images(c.motion,c.motionImages);images(c.depth,c.depthImages);
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.viewType=VK_IMAGE_VIEW_TYPE_2D;
            for(const auto& image:c.motionImages){view.image=image.image;view.format=VK_FORMAT_R16G16B16A16_SFLOAT;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                VkImageView v{};checkVk(vkCreateImageView(device_,&view,nullptr,&v),"spacewarp motion view");c.motionViews.push_back(v);}
            for(const auto& image:c.depthImages){view.image=image.image;view.format=depth;view.subresourceRange={swdetail::depthAspects(depth),0,1,0,1};
                VkImageView v{};checkVk(vkCreateImageView(device_,&view,nullptr,&v),"spacewarp depth view");c.depthViews.push_back(v);}
            c.framebuffers.assign(c.motionImages.size()*c.depthImages.size(),VK_NULL_HANDLE);
        }
        if(!chains_){extent_=wanted;chains_=true;++swapchainCreates_;altReady_=alt_[0].motion&&alt_[1].motion&&alt_[0].depth&&alt_[1].depth;}
        status_="READY";
    }catch(const std::exception&){discardScreens();fail("SWAPCHAIN_FAILED","GPU FAILED: SWAPCHAIN");}
}
inline VkFramebuffer SpaceWarp::framebuffer(unsigned eye) {return framebufferFor(eye,eyes_[eye].motionIndex,eyes_[eye].depthIndex);}
inline VkFramebuffer SpaceWarp::framebufferFor(unsigned eye,std::uint32_t motionIndex,std::uint32_t depthIndex) {
    auto& c=eyes_[eye];
    if(motionIndex>=c.motionViews.size()||depthIndex>=c.depthViews.size())throw std::runtime_error("spacewarp image index");
    const auto index=std::size_t(motionIndex)*c.depthImages.size()+depthIndex;
    if(index>=c.framebuffers.size())throw std::runtime_error("spacewarp framebuffer index");
    auto& fb=c.framebuffers[index];
    if(!fb){
        const std::array<VkImageView,2> views{c.motionViews[motionIndex],c.depthViews[depthIndex]};
        VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};f.renderPass=pass_;f.attachmentCount=2;f.pAttachments=views.data();
        f.width=extent_.width;f.height=extent_.height;f.layers=1;checkVk(vkCreateFramebuffer(device_,&f,nullptr,&fb),"spacewarp framebuffer");
    }
    return fb;
}
inline bool SpaceWarp::acquire(unsigned m) noexcept {
    acquired_=false;
    if(altReady_&&chains_){std::swap(eyes_,alt_);++warpSwaps_;} // Write the other set (the compositor still holds the last one)
    // No motion-vector pass while the budget guard suspends the info (it would not be chained).
    if(!chains_||broken_||!pipeline_||m>=slotCount||!slots_[m].valid||!spaceWarpControl.producing())return false;
    std::array<XrSwapchain,4> held{};unsigned count=0;bool good=true;
    for(unsigned e=0;e<2&&good;++e)for(unsigned k=0;k<2&&good;++k){
        auto& c=eyes_[e];const auto chain=k?c.depth:c.motion;std::uint32_t index{};
        XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if(XR_FAILED(xrAcquireSwapchainImage(chain,&a,&index))||index>=(k?c.depthImages.size():c.motionImages.size())){good=false;break;}
        held[count++]=chain;(k?c.depthIndex:c.motionIndex)=index;
        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=XR_INFINITE_DURATION;
        const auto r=xrWaitSwapchainImage(chain,&w);if(r!=XR_SUCCESS&&r!=XR_SESSION_LOSS_PENDING)good=false;
    }
    // Framebuffers are created here (not while recording) so a failure releases the images and only
    // disables space warp; record() itself never throws.
    if(good&&(!slots_[m].sets[0]||!slots_[m].sets[1]))good=false;
    if(good){try{for(unsigned e=0;e<2;++e)current_[e]=framebuffer(e);}catch(...){good=false;}}
    if(!good){
        for(unsigned i=0;i<count;++i){XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrReleaseSwapchainImage(held[i],&r);}
        ++acquireFailures_;fail("ACQUIRE_FAILED","GPU FAILED: ACQUIRE");return false;
    }
    acquired_=true;return true;
}
// The fragment push constants of one eye's MV pass: pushed reprojection (ON ZERO MV pushes identity) + conventions.
inline std::array<float,20> SpaceWarp::pushFor(const Slot& s,unsigned e) const noexcept {
    const auto conv=spaceWarpControl.conventions(s.projectionY);const unsigned variant=spaceWarpControl.mode();
    const auto reproject=spacewarp::pushedReprojection(variant,s.reproject[e&1]);
    std::array<float,20> push{};std::memcpy(push.data(),reproject.data(),64);push[16]=conv.engineY;push[17]=conv.mvY;push[18]=conv.mvSign;push[19]=conv.mvZ;
    return push;
}
inline void SpaceWarp::record(VkCommandBuffer cmd,unsigned m) noexcept {
    if(!acquired_||m>=slotCount||!current_[0]||!current_[1])return;
    recordPass(cmd,m,current_);
}
inline bool SpaceWarp::ticketChains(Chains& out) const noexcept {
    if(!chains_||broken_||!pipeline_||altReady_||!spaceWarpControl.producing())return false;
    for(unsigned e=0;e<2;++e){const auto& c=eyes_[e];if(!c.motion||!c.depth||c.motionViews.empty()||c.depthViews.empty())return false;
        out.motion[e]=c.motion;out.depth[e]=c.depth;out.motionCount[e]=c.motionImages.size();out.depthCount[e]=c.depthImages.size();}
    out.generation=swapchainCreates_;return true;
}
inline bool SpaceWarp::acquireImages(const Chains& c,Images& out) noexcept {
    out={};std::array<XrSwapchain,4> held{};unsigned count=0;bool good=true;
    for(unsigned e=0;e<2&&good;++e)for(unsigned k=0;k<2&&good;++k){
        const auto chain=k?c.depth[e]:c.motion[e];std::uint32_t index{};
        XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if(XR_FAILED(xrAcquireSwapchainImage(chain,&a,&index))||index>=(k?c.depthCount[e]:c.motionCount[e])){good=false;break;}
        held[count++]=chain;(k?out.depth[e]:out.motion[e])=index;
        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=XR_INFINITE_DURATION;
        const auto r=xrWaitSwapchainImage(chain,&w);if(r!=XR_SUCCESS&&r!=XR_SESSION_LOSS_PENDING)good=false;
    }
    if(!good){for(unsigned i=0;i<count;++i){XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrReleaseSwapchainImage(held[i],&r);}out={};return false;}
    out.valid=true;out.generation=c.generation;return true;
}
inline bool SpaceWarp::recordImages(VkCommandBuffer cmd,unsigned m,const Images& img) noexcept {
    if(!img.valid||img.generation!=swapchainCreates_||!chains_||broken_||!pipeline_||m>=slotCount||!slots_[m].valid||!slots_[m].sets[0]||!slots_[m].sets[1])return false;
    std::array<VkFramebuffer,2> fb{};
    try{for(unsigned e=0;e<2;++e)fb[e]=framebufferFor(e,img.motion[e],img.depth[e]);}catch(...){return false;}
    // In the producer's own command buffer (not behind a semaphore as on the XR queue): make the slot depth / actor block writes
    // recorded by produce() visible to the MV pass explicitly.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_UNIFORM_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,nullptr,0,nullptr);
    recordPass(cmd,m,fb);return true;
}
inline void SpaceWarp::releaseImages(const Images& img) noexcept {
    if(!img.valid||img.generation!=swapchainCreates_||!chains_)return; // a recreated set: the old handles are gone with their images
    for(auto& c:eyes_)for(auto chain:{c.motion,c.depth}){XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrReleaseSwapchainImage(chain,&r);}
}
inline void SpaceWarp::recordPass(VkCommandBuffer cmd,unsigned m,const std::array<VkFramebuffer,2>& fb) noexcept {
    const auto start=diagnostics::monotonicNs();
    const auto& s=slots_[m];
    for(unsigned e=0;e<2;++e){
        VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=pass_;b.framebuffer=fb[e];b.renderArea.extent={extent_.width,extent_.height};
        vkCmdBeginRenderPass(cmd,&b,VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0,0,float(extent_.width),float(extent_.height),0,1};const VkRect2D scissor{{0,0},{extent_.width,extent_.height}};
        vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&s.sets[e],0,nullptr);
        const auto push=pushFor(s,e);
        vkCmdPushConstants(cmd,layout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(float)*20,push.data());
        vkCmdDraw(cmd,3,1,0,0);
        vkCmdEndRenderPass(cmd);
    }
    // AppSW dump: the dumped pair is displayed now -> the same pass into private images (+ the real images with dumpxr)
    if(fb==current_&&dump_.request&&dump_.slot==int(m)&&dump_.producerRecorded&&!dump_.consumerRecorded&&s.frame==dump_.frame)dumpConsumer(cmd,m); // acquire() path only
    ++consumed_;passCpu_.elapsed(start,diagnostics::monotonicNs());
}
inline void SpaceWarp::release() noexcept {
    if(!acquired_)return;
    for(auto& c:eyes_)for(auto chain:{c.motion,c.depth}){XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xrReleaseSwapchainImage(chain,&r);}
    acquired_=false;current_={};
}
inline SpaceWarpLayer SpaceWarp::layer(unsigned m) const noexcept {
    SpaceWarpLayer out{};
    if(!chains_||broken_||m>=slotCount||!slots_[m].valid)return out;
    const auto& s=slots_[m];
    out.valid=true;out.skip=s.skip;out.frame=s.frame;out.extent=extent_;
    for(unsigned e=0;e<2;++e){out.motion[e]=eyes_[e].motion;out.depth[e]=eyes_[e].depth;}
    const int delta=spaceWarpControl.deltaMode();
    if(s.deltaValid&&delta==1)out.delta=swdetail::toXr(s.delta);
    else if(s.deltaValid&&delta==2)out.delta=swdetail::toXr(swdetail::inverse(s.delta));
    // ON FAR DEPTH = nearZ 1e6, farZ +inf (positional timewarp rotation-only); else debug.rdrvr.spacewarp.depth.
    const auto d=spacewarp::layerDepth(spaceWarpControl.mode(),spaceWarpControl.depthMode(),s.nearZ,s.farZ);
    out.minDepth=d.minDepth;out.maxDepth=d.maxDepth;out.nearZ=d.nearZ;out.farZ=d.farZ;
    return out;
}
inline void SpaceWarp::discardScreens() noexcept {
    altReady_=false;
    for(auto* set:{&eyes_,&alt_})for(auto& c:*set){
        for(auto fb:c.framebuffers)if(fb)vkDestroyFramebuffer(device_,fb,nullptr);
        for(auto v:c.motionViews)if(v)vkDestroyImageView(device_,v,nullptr);
        for(auto v:c.depthViews)if(v)vkDestroyImageView(device_,v,nullptr);
        if(c.motion)xrDestroySwapchain(c.motion);
        if(c.depth)xrDestroySwapchain(c.depth);
        c=EyeChain{};
    }
    chains_=false;acquired_=false;current_={};xrCopy_=false;
    if(dump_.request)dumpPoll();dumpDestroyImages(); // The mirror targets (consumer fences waited by the caller)
    clearFailure(); // Swapchains are recreated on the next render (ring rebuild / session change)
}
inline void SpaceWarp::discardGpu() noexcept {
    if(!device_)return;
    dumpDestroy(); // Every producer/consumer submission completed (caller); before the passes its framebuffers were made for
    for(auto* set:{&eyes_,&alt_})for(auto& c:*set){for(auto& fb:c.framebuffers){if(fb)vkDestroyFramebuffer(device_,fb,nullptr);fb={};}}
    for(auto& s:slots_)destroySlot(s);
    destroyPipeline();
    if(downPipeline_)vkDestroyPipeline(device_,downPipeline_,nullptr);
    if(downPass_)vkDestroyRenderPass(device_,downPass_,nullptr);
    if(queries_)vkDestroyQueryPool(device_,queries_,nullptr);
    downPipeline_={};downPass_={};downFormat_={};path_=0;downStatus_="UNDECIDED";queries_={};queriesTried_=false;
    if(layout_)vkDestroyPipelineLayout(device_,layout_,nullptr);
    if(pool_)vkDestroyDescriptorPool(device_,pool_,nullptr);
    if(setLayout_)vkDestroyDescriptorSetLayout(device_,setLayout_,nullptr);
    if(sampler_)vkDestroySampler(device_,sampler_,nullptr);
    if(actorBuffer_)vkDestroyBuffer(device_,actorBuffer_,nullptr);
    if(actorMemory_)vkFreeMemory(device_,actorMemory_,nullptr);
    actorBuffer_={};actorMemory_={};
    layout_={};pool_={};setLayout_={};sampler_={};previous_={};
    spaceWarpControl.refuseDirect(false); // A new GPU generation decides path_ again and may accept direct pairs
    clearFailure();
}
inline std::string SpaceWarp::report() const {
    std::ostringstream o;
    const char* path=path_==1?"downsample":path_==2?"copy":"undecided";
    o<<"{\"status\":\""<<status_<<"\",\"broken\":"<<(broken_?"true":"false")<<",\"swapchains\":"<<(chains_?"true":"false")
     <<",\"mvExtent\":["<<extent_.width<<','<<extent_.height<<"],\"motionFormat\":"<<int(VK_FORMAT_R16G16B16A16_SFLOAT)<<",\"depthFormat\":"<<int(depthFormat_)
     <<",\"slotDepthFormat\":"<<int(slots_[0].format)<<",\"slotDepthExtent\":["<<slots_[0].extent.width<<','<<slots_[0].extent.height<<"]"
     <<",\"slotDepthPath\":\""<<path<<"\",\"downsampleStatus\":\""<<downStatus_<<"\",\"downsampleFormat\":"<<int(downFormat_)
     <<",\"downsamples\":"<<downsamples_<<",\"slotCopies\":"<<slotCopies_<<",\"downsampleFailures\":"<<downsampleFailures_
     <<",\"depthDirect\":{"<<spaceWarpControl.directFields()<<",\"slotDirectCopies\":"<<directCopies_<<",\"mismatches\":"<<directMismatches_<<'}'
     <<",\"produced\":"<<produced_<<",\"noSource\":"<<noSource_<<",\"noHistory\":"<<noHistory_<<",\"renderGaps\":"<<renderGaps_<<",\"cuts\":"<<cuts_<<",\"formatMismatch\":"<<formatMismatch_
     <<",\"suspendedPairs\":"<<suspendedPairs_<<",\"consumerSkipsWhileOn\":"<<consumerSkips_
     <<",\"slotImageSets\":"<<slotImages_<<",\"passes\":"<<consumed_<<",\"acquireFailures\":"<<acquireFailures_<<",\"warpSets\":"<<(altReady_?2:1)<<",\"warpSwaps\":"<<warpSwaps_<<",\"swapchainCreates\":"<<swapchainCreates_
     <<",\"warpDepthTimestamps\":"<<(queries_?"true":"false")<<",\"timingUnavailable\":"<<timingUnavailable_
     <<",\"passCpuNs\":"<<passCpu_.snapshot().json()
     <<",\"actors\":{\"buffer\":"<<(actorBuffer_?"true":"false")<<",\"pairs\":"<<actorPairs_<<",\"pairsOff\":"<<actorPairsOff_<<",\"written\":"<<actorsWritten_<<",\"playerBlocks\":"<<playerBlocks_
     <<",\"maxPerEye\":"<<actorsMaxEye_<<",\"lastPerEye\":["<<actorsLastEye_[0]<<','<<actorsLastEye_[1]<<"],\"sameSerial\":"<<actorSameSerial_
     <<",\"skipUnmatched\":"<<actorSkipUnmatched_<<",\"skipJump\":"<<actorSkipJump_<<",\"skipStill\":"<<actorSkipStill_
     <<",\"skipOffscreen\":"<<actorSkipOffscreen_<<",\"skipMath\":"<<actorSkipMath_<<",\"maxStepM\":"<<actorMaxStepM_
     <<",\"serialDelta\":["<<actorSerialDelta_[0]<<','<<actorSerialDelta_[1]<<','<<actorSerialDelta_[2]<<','<<actorSerialDelta_[3]<<']'
     <<",\"pad\":"<<spaceWarpControl.actorPadSteps() // Cylinder widening in root steps (debug.rdrvr.spacewarp.actorpad)
     <<",\"skipBehind\":"<<actorSkipBehind_<<",\"clipped\":"<<actorClipped_<<",\"stillPinned\":"<<actorStillPinned_
     <<",\"bones\":{\"pairs\":"<<bonePairs_<<",\"missing\":"<<boneMissing_<<",\"modePairs\":["<<boneModePairs_[0]<<','<<boneModePairs_[1]<<','<<boneModePairs_[2]<<','<<boneModePairs_[3]<<"],\"actors\":"<<boneActors_
     <<",\"parts\":"<<boneParts_<<",\"written\":"<<bonePartsWritten_<<",\"maxPerEye\":"<<bonesMaxEye_<<",\"lastPerEye\":["<<bonesLastEye_[0]<<','<<bonesLastEye_[1]
     <<"],\"unmatched\":"<<boneUnmatched_<<",\"jump\":"<<boneJump_<<",\"overflow\":"<<boneOverflow_<<",\"math\":"<<boneMath_<<",\"maxStepM\":"<<boneMaxStepM_<<"}}"
     <<",\"dump\":{\"inFlight\":"<<(dump_.request?"true":"false")<<",\"started\":"<<dump_.started<<",\"written\":"<<dump_.written<<",\"failed\":"<<dump_.failed
     <<",\"abandoned\":"<<dump_.abandoned<<",\"retries\":"<<dump_.retries<<",\"busy\":"<<dump_.busy<<",\"timeouts\":"<<dump_.timeouts
     <<",\"prerolls\":"<<dump_.prerolls<<",\"waitingForPair\":"<<(dump_.preroll?"true":"false")
     <<",\"xrCopy\":"<<(xrCopy_?"true":"false")<<",\"lastError\":\""<<dump_.lastError<<"\",\"lastFiles\":\""<<dump_.lastFiles<<"\",\"lastDual\":"
     <<(dump_.lastDual.empty()?std::string("null"):dump_.lastDual)<<"}}";
    return o.str();
}
inline VkDeviceSize SpaceWarp::texelBytes(VkFormat f) noexcept {
    switch(f){case VK_FORMAT_R16G16B16A16_SFLOAT:return 8;case VK_FORMAT_D16_UNORM:case VK_FORMAT_D16_UNORM_S8_UINT:return 2;default:return 4;} // depth aspect copies: 4
}
inline VkDeviceSize SpaceWarp::dumpPlan(const char* name,VkFormat f,Extent e,bool depth) {
    DumpEntry d{};d.name=name;d.format=f;d.extent=e;d.depth=depth;d.offset=(dump_.used+255)&~VkDeviceSize(255);
    d.bytes=VkDeviceSize(e.width)*e.height*texelBytes(f);dump_.used=d.offset+d.bytes;dump_.entries.push_back(d);return d.offset;
}
inline bool SpaceWarp::dumpBuffer(VkDeviceSize need) noexcept {
    auto& d=dump_;
    if(!d.event){VkEventCreateInfo ec{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};if(vkCreateEvent(device_,&ec,nullptr,&d.event)!=VK_SUCCESS){d.event={};d.lastError="event";return false;}}
    if(d.buffer&&d.capacity>=need)return true;
    if(d.buffer){vkDestroyBuffer(device_,d.buffer,nullptr);vkFreeMemory(device_,d.memory,nullptr);d.buffer={};d.memory={};d.mapped={};d.capacity=0;} // no dump in flight
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};b.size=need;b.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;b.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(device_,&b,nullptr,&d.buffer)!=VK_SUCCESS){d.buffer={};d.lastError="buffer";return false;}
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(device_,d.buffer,&req);
    VkPhysicalDeviceMemoryProperties mem{};vkGetPhysicalDeviceMemoryProperties(physical_,&mem);std::uint32_t type=UINT32_MAX;
    const VkMemoryPropertyFlags want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(std::uint32_t i=0;i<mem.memoryTypeCount&&type==UINT32_MAX;++i)if((req.memoryTypeBits&(1u<<i))&&(mem.memoryTypes[i].propertyFlags&want)==want)type=i;
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=req.size;a.memoryTypeIndex=type;
    if(type==UINT32_MAX||vkAllocateMemory(device_,&a,nullptr,&d.memory)!=VK_SUCCESS||vkBindBufferMemory(device_,d.buffer,d.memory,0)!=VK_SUCCESS||
       vkMapMemory(device_,d.memory,0,VK_WHOLE_SIZE,0,&d.mapped)!=VK_SUCCESS){
        vkDestroyBuffer(device_,d.buffer,nullptr);if(d.memory)vkFreeMemory(device_,d.memory,nullptr);d.buffer={};d.memory={};d.mapped={};d.lastError="buffer memory";return false;}
    d.capacity=need;return true;
}
// One dump starts on mailbox slot m (produce, before the pair's actor blocks): plan every region (producer: slot depth [+ the copy
// path's depth]; consumer: MV + XR depth [+ the real swapchain images]) and make the host-visible buffer + event. False = no dump.
// preroll = slot m is the history pair (only "prevDepth" now, every other region on the next consecutive pair).
inline bool SpaceWarp::dumpStart(unsigned m,std::uint64_t request,bool dual,bool preroll) noexcept {
    auto& d=dump_;const auto& s=slots_[m];
    d.request=request?request:1;d.frame=0;d.tries=0;d.dual=!preroll&&dual&&downPipeline_&&s.downsample;
    d.preroll=preroll;d.slot=preroll?-1:int(m);d.prevSlot=preroll?int(m):-1;d.prevFrame=0;d.prevRecorded=d.prevDone=false;
    d.planFormat=s.format;d.planExtent=s.extent;
    d.producerRecorded=d.producerDone=d.consumerRecorded=d.consumerDone=false;d.startNs=diagnostics::monotonicNs();d.consumerNs=0;
    d.meta.clear();d.consumerMeta.clear();d.entries.clear();d.used=0;
    try{
        if(preroll)for(unsigned e=0;e<2;++e)dumpPlan(e?"R-prevDepth":"L-prevDepth",s.format,s.extent,true);
        for(unsigned e=0;e<2;++e)dumpPlan(e?"R-slotDepth":"L-slotDepth",s.format,s.extent,true);
        if(d.dual)for(unsigned e=0;e<2;++e)dumpPlan(e?"R-depthCopyPath":"L-depthCopyPath",downFormat_,s.extent,true);
        const Extent mv=chains_?extent_:spaceWarpControl.extent();const VkFormat df=depthFormat_?depthFormat_:VK_FORMAT_D24_UNORM_S8_UINT;
        for(unsigned e=0;e<2;++e){dumpPlan(e?"R-mv":"L-mv",VK_FORMAT_R16G16B16A16_SFLOAT,mv,false);dumpPlan(e?"R-xrDepth":"L-xrDepth",df,mv,true);}
        if(xrCopy_)for(unsigned e=0;e<2;++e){dumpPlan(e?"R-mvXr":"L-mvXr",VK_FORMAT_R16G16B16A16_SFLOAT,mv,false);dumpPlan(e?"R-xrDepthXr":"L-xrDepthXr",df,mv,true);}
    }catch(...){d.lastError="plan";++d.failed;d.request=0;d.slot=d.prevSlot=-1;d.preroll=false;return false;}
    if(!s.depth[0].image||!dumpBuffer(d.used)){++d.failed;d.request=0;d.slot=d.prevSlot=-1;d.preroll=false;return false;}
    vkResetEvent(device_,d.event); // its last vkCmdSetEvent (previous dump) executed long ago
    ++d.started;if(preroll)++d.prerolls;return true;
}
// Copy one image (layout `from`) into its buffer region and back to `back` (TRANSFER_SRC_OPTIMAL = leave it there).
inline void SpaceWarp::dumpCopy(VkCommandBuffer cmd,VkImage image,VkImageAspectFlags copyAspect,VkImageAspectFlags barrierAspect,VkImageLayout from,VkImageLayout back,
                                VkPipelineStageFlags srcStage,VkAccessFlags srcAccess,DumpEntry& e) noexcept {
    const auto pre=swdetail::imageBarrier(image,barrierAspect,from,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,srcAccess,VK_ACCESS_TRANSFER_READ_BIT);
    vkCmdPipelineBarrier(cmd,srcStage,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&pre);
    VkBufferImageCopy r{};r.bufferOffset=e.offset;r.imageSubresource={copyAspect,0,0,1};r.imageExtent={e.extent.width,e.extent.height,1};
    vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dump_.buffer,1,&r);e.recorded=true;
    if(back==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)return;
    const auto post=swdetail::imageBarrier(image,barrierAspect,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,back,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&post);
}
// Producer half (end of produce, same command buffer): [depthdual: the copy path's farthest-of-2x2
// downsample of the full-size copy into private images] + copies of the slot depth (and those
// images) into the buffer. Completion: the slot fence (completed()).
inline void SpaceWarp::dumpProducer(VkCommandBuffer cmd,unsigned m,const SpaceWarpSource& src,bool preroll) noexcept {
    auto& d=dump_;const auto& s=slots_[m];
    const auto entry=[&](const std::string& n)->DumpEntry*{for(auto& e:d.entries)if(e.name==n)return &e;return nullptr;};
    // A retried dump writes the same buffer regions as copies of earlier submissions (same queue): order the writes
    VkMemoryBarrier waw{VK_STRUCTURE_TYPE_MEMORY_BARRIER};waw.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;waw.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&waw,0,nullptr,0,nullptr);
    if(preroll){ // The history pair: its slot depth only (completion: this slot's fence -> completed() -> prevDone)
        for(unsigned e=0;e<2;++e)if(auto* r=entry(e?"R-prevDepth":"L-prevDepth"))
            dumpCopy(cmd,s.depth[e].image,VK_IMAGE_ASPECT_DEPTH_BIT,swdetail::depthAspects(s.format),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_WRITE_BIT,*r);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        d.prevRecorded=true;return;
    }
    if(d.dual){
        try{
            if(!d.dualImage[0].image||d.dualFormat!=downFormat_||!(d.dualExtent==s.extent)){
                for(auto& fb:d.dualFb){if(fb)vkDestroyFramebuffer(device_,fb,nullptr);fb={};}
                for(auto& v:d.dualAttach){if(v)vkDestroyImageView(device_,v,nullptr);v={};}
                for(auto& i:d.dualImage)destroyOwnedImage(device_,i);
                for(unsigned e=0;e<2;++e){
                    d.dualImage[e]=createOwnedImage(physical_,device_,s.extent,downFormat_,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,true);
                    VkImageView view=d.dualImage[e].view;
                    if(swdetail::hasStencil(downFormat_)){VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=d.dualImage[e].image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
                        v.format=downFormat_;v.subresourceRange={swdetail::depthAspects(downFormat_),0,1,0,1};checkVk(vkCreateImageView(device_,&v,nullptr,&d.dualAttach[e]),"dump dual view");view=d.dualAttach[e];}
                    VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};f.renderPass=downPass_;f.attachmentCount=1;f.pAttachments=&view;f.width=s.extent.width;f.height=s.extent.height;f.layers=1;
                    checkVk(vkCreateFramebuffer(device_,&f,nullptr,&d.dualFb[e]),"dump dual framebuffer");
                }
                d.dualFormat=downFormat_;d.dualExtent=s.extent;
            }
            // the slot's downSets are free (its previous producer command buffer completed: acquireSlot) and unused by a direct pair
            std::array<VkDescriptorImageInfo,2> info{};std::array<VkWriteDescriptorSet,2> writes{};
            for(unsigned e=0;e<2;++e){info[e]={sampler_,src.dualViews[e],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                auto& w=writes[e];w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=s.downSets[e];w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&info[e];}
            vkUpdateDescriptorSets(device_,2,writes.data(),0,nullptr);
            for(unsigned e=0;e<2;++e){
                swdetail::barrier(cmd,src.dualDepth[e],swdetail::depthAspects(src.dualFormat),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
                VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=downPass_;b.framebuffer=d.dualFb[e];b.renderArea.extent={s.extent.width,s.extent.height};
                vkCmdBeginRenderPass(cmd,&b,VK_SUBPASS_CONTENTS_INLINE);
                const VkViewport viewport{0,0,float(s.extent.width),float(s.extent.height),0,1};const VkRect2D scissor{{0,0},{s.extent.width,s.extent.height}};
                vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
                vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,downPipeline_);
                vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&s.downSets[e],0,nullptr);
                vkCmdDraw(cmd,3,1,0,0);
                vkCmdEndRenderPass(cmd);
            }
        }catch(...){d.dual=false;d.lastError="dual images";}
    }
    for(unsigned e=0;e<2;++e){
        if(auto* r=entry(e?"R-slotDepth":"L-slotDepth"))
            dumpCopy(cmd,s.depth[e].image,VK_IMAGE_ASPECT_DEPTH_BIT,swdetail::depthAspects(s.format),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_WRITE_BIT,*r);
        if(d.dual)if(auto* r=entry(e?"R-depthCopyPath":"L-depthCopyPath"))
            dumpCopy(cmd,d.dualImage[e].image,VK_IMAGE_ASPECT_DEPTH_BIT,swdetail::depthAspects(d.dualFormat),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_ACCESS_MEMORY_WRITE_BIT,*r);
    }
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    d.producerRecorded=true;
}
// Consumer half (record, right after the real pass of the dumped slot): the same pipeline / descriptor set / push constants into
// private R16G16B16A16_SFLOAT + depth images (pass_ compatible), copied out; with dumpxr also the real swapchain images. Completion:
// the event (dumpPoll), set behind the copies in this command buffer.
inline void SpaceWarp::dumpConsumer(VkCommandBuffer cmd,unsigned m) noexcept {
    auto& d=dump_;const auto& s=slots_[m];
    const auto entry=[&](const std::string& n)->DumpEntry*{for(auto& e:d.entries)if(e.name==n)return &e;return nullptr;};
    if(!pass_||!pipeline_||!d.event||!extent_.width||!extent_.height)return;
    try{
        if(!d.mv[0].image||!(d.mirrorExtent==extent_)||d.mirrorDepth!=depthFormat_){
            dumpDestroyImages();
            for(unsigned e=0;e<2;++e){
                d.mv[e]=createOwnedImage(physical_,device_,extent_,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_COLOR_BIT,true);
                d.depth[e]=createOwnedImage(physical_,device_,extent_,depthFormat_,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,swdetail::depthAspects(depthFormat_),true);
                const std::array<VkImageView,2> views{d.mv[e].view,d.depth[e].view};
                VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};f.renderPass=pass_;f.attachmentCount=2;f.pAttachments=views.data();f.width=extent_.width;f.height=extent_.height;f.layers=1;
                checkVk(vkCreateFramebuffer(device_,&f,nullptr,&d.fb[e]),"dump mirror framebuffer");
            }
            d.mirrorExtent=extent_;d.mirrorDepth=depthFormat_;
        }
    }catch(...){dumpDestroyImages();d.lastError="mirror images";return;}
    std::ostringstream o;o.precision(9);
    o<<"\"consumer\":{\"extent\":["<<extent_.width<<','<<extent_.height<<"],\"depthFormat\":"<<int(depthFormat_)<<",\"xrCopy\":"<<(xrCopy_?"true":"false")<<",\"push\":[";
    for(unsigned e=0;e<2;++e){
        VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=pass_;b.framebuffer=d.fb[e];b.renderArea.extent={extent_.width,extent_.height};
        vkCmdBeginRenderPass(cmd,&b,VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0,0,float(extent_.width),float(extent_.height),0,1};const VkRect2D scissor{{0,0},{extent_.width,extent_.height}};
        vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&s.sets[e],0,nullptr);
        const auto push=pushFor(s,e);
        vkCmdPushConstants(cmd,layout_,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(float)*20,push.data());
        vkCmdDraw(cmd,3,1,0,0);
        vkCmdEndRenderPass(cmd);
        o<<(e?",":"")<<'[';for(unsigned i=0;i<20;++i)o<<(i?",":"")<<push[i];o<<']';
    }
    o<<"],\"motionIndex\":["<<eyes_[0].motionIndex<<','<<eyes_[1].motionIndex<<"],\"depthIndex\":["<<eyes_[0].depthIndex<<','<<eyes_[1].depthIndex<<"]}";
    const auto depthAspect=swdetail::depthAspects(depthFormat_);
    for(unsigned e=0;e<2;++e){
        if(auto* r=entry(e?"R-mv":"L-mv");r&&r->extent==extent_)
            dumpCopy(cmd,d.mv[e].image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,*r);
        if(auto* r=entry(e?"R-xrDepth":"L-xrDepth");r&&r->extent==extent_&&r->format==depthFormat_)
            dumpCopy(cmd,d.depth[e].image,VK_IMAGE_ASPECT_DEPTH_BIT,depthAspect,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,*r);
        if(!xrCopy_||!acquired_)continue; // the real images: only while held (record) and created with TRANSFER_SRC
        const auto& c=eyes_[e];
        if(auto* r=entry(e?"R-mvXr":"L-mvXr");r&&r->extent==extent_&&c.motionIndex<c.motionImages.size())
            dumpCopy(cmd,c.motionImages[c.motionIndex].image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,*r);
        if(auto* r=entry(e?"R-xrDepthXr":"L-xrDepthXr");r&&r->extent==extent_&&r->format==depthFormat_&&c.depthIndex<c.depthImages.size())
            dumpCopy(cmd,c.depthImages[c.depthIndex].image,VK_IMAGE_ASPECT_DEPTH_BIT,depthAspect,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,*r);
    }
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    vkCmdSetEvent(cmd,d.event,VK_PIPELINE_STAGE_TRANSFER_BIT);
    d.consumerMeta=o.str();d.consumerRecorded=true;d.consumerNs=diagnostics::monotonicNs();
}
// Mailbox mutex: the consumer half landed (event) / the producer half landed (completed()) -> write; 5 s without both -> partial files.
inline void SpaceWarp::dumpPoll() noexcept {
    auto& d=dump_;if(!d.request)return;
    if(d.consumerRecorded&&!d.consumerDone&&d.event&&vkGetEventStatus(device_,d.event)==VK_EVENT_SET)d.consumerDone=true;
    if(d.producerDone&&d.consumerDone&&(d.prevSlot<0||d.prevDone)){dumpFinish("ok");return;} // + the pre-roll copy
    if(diagnostics::monotonicNs()-d.startNs>5'000'000'000ull){++d.timeouts;dumpFinish("timeout");}
}
// Files: <dumpDir>/eye-dump-warp-<name>.raw for every region that landed, then eye-dump-warp.json (entries in EyeDump's format +
// the producer / consumer JSON + the depthdual comparison). Written on a detached thread from a copy (no XR-thread file I/O).
inline void SpaceWarp::dumpFinish(const char* state) noexcept {
    auto& d=dump_;
    try{
        const auto landed=[&](const DumpEntry& e){const bool consumer=e.name.find("mv")!=std::string::npos||e.name.find("xrDepth")!=std::string::npos;
            if(e.name.find("prevDepth")!=std::string::npos)return e.recorded&&d.prevDone; // The pre-roll pair
            return e.recorded&&(consumer?d.consumerDone:d.producerDone);};
        std::vector<DumpEntry> keep;for(const auto& e:d.entries)if(landed(e)&&d.mapped&&e.offset+e.bytes<=d.capacity)keep.push_back(e);
        std::vector<std::uint8_t> bytes;VkDeviceSize total=0;for(const auto& e:keep)total+=e.bytes;bytes.reserve(std::size_t(total));
        std::vector<std::pair<std::string,std::pair<std::size_t,std::size_t>>> files;
        for(const auto& e:keep){const auto* p=static_cast<const std::uint8_t*>(d.mapped)+e.offset;const std::size_t at=bytes.size();
            bytes.insert(bytes.end(),p,p+e.bytes);files.push_back({e.name,{at,std::size_t(e.bytes)}});}
        // depthdual: the copy path's 420x440 (downsample of the full-size copy) vs the direct path's
        // (= the slot depth), both eyes
        std::string dual;
        if(d.dual&&d.producerDone){
            std::ostringstream c;c.precision(9);c<<"{\"frame\":"<<d.frame<<",\"eyes\":[";
            for(unsigned e=0;e<2;++e){
                const DumpEntry* a=nullptr;const DumpEntry* b=nullptr;
                for(const auto& k:keep){if(k.name==(e?"R-depthCopyPath":"L-depthCopyPath"))a=&k;if(k.name==(e?"R-slotDepth":"L-slotDepth"))b=&k;}
                std::size_t texels=0,differ=0;double maxDiff=0;
                if(a&&b&&a->bytes==b->bytes&&a->format==b->format){
                    const auto* pa=static_cast<const std::uint8_t*>(d.mapped)+a->offset;const auto* pb=static_cast<const std::uint8_t*>(d.mapped)+b->offset;
                    texels=std::size_t(a->bytes/4);const bool f32=a->format==VK_FORMAT_D32_SFLOAT||a->format==VK_FORMAT_D32_SFLOAT_S8_UINT;
                    for(std::size_t i=0;i<texels;++i){std::uint32_t x{},y{};std::memcpy(&x,pa+4*i,4);std::memcpy(&y,pb+4*i,4);
                        float fx{},fy{};if(f32){std::memcpy(&fx,&x,4);std::memcpy(&fy,&y,4);}else{fx=float(x&0xffffffu)/16777215.f;fy=float(y&0xffffffu)/16777215.f;}
                        if((f32?x!=y:(x&0xffffffu)!=(y&0xffffffu))){++differ;maxDiff=std::max(maxDiff,double(std::fabs(fx-fy)));}}
                }
                c<<(e?",":"")<<"{\"texels\":"<<texels<<",\"differ\":"<<differ<<",\"maxAbsDiff\":"<<maxDiff<<'}';
            }
            c<<"]}";dual=c.str();d.lastDual=dual;
        }
        std::ostringstream j;j.precision(9);
        j<<"{\"kind\":\"appsw\",\"state\":\""<<state<<"\",\"request\":"<<d.request<<",\"tries\":"<<d.tries<<",\"producer\":"<<(d.producerDone?"true":"false")
         <<",\"consumerRecorded\":"<<(d.consumerRecorded?"true":"false")<<",\"consumerDone\":"<<(d.consumerDone?"true":"false")<<",\"entries\":[";
        for(std::size_t i=0;i<keep.size();++i){const auto& e=keep[i];
            j<<(i?",":"")<<"{\"name\":\"warp-"<<e.name<<"\",\"format\":"<<int(e.format)<<",\"width\":"<<e.extent.width<<",\"height\":"<<e.extent.height
             <<",\"depth\":"<<(e.depth?1:0)<<",\"bytes\":"<<e.bytes<<'}';}
        j<<"],"<<(d.meta.empty()?std::string("\"meta\":null"):d.meta);
        if(!d.consumerMeta.empty())j<<','<<d.consumerMeta;
        if(!dual.empty())j<<",\"dualCompare\":"<<dual;
        if(d.prevSlot>=0)j<<",\"prevDepth\":{\"slot\":"<<d.prevSlot<<",\"frame\":"<<d.prevFrame<<",\"recorded\":"<<(d.prevRecorded?"true":"false")
         <<",\"done\":"<<(d.prevDone?"true":"false")<<",\"waitingForPair\":"<<(d.preroll?"true":"false")<<'}';
        j<<'}';
        auto json=j.str();const auto dir=spacewarp::dumpDir;
        auto write=[dir,files=std::move(files),bytes=std::move(bytes),json=std::move(json)]() noexcept {
            try{
                for(const auto& [name,range]:files){const auto path=dir+"/eye-dump-warp-"+name+".raw";
                    if(FILE* f=std::fopen(path.c_str(),"wb")){std::fwrite(bytes.data()+range.first,1,range.second,f);std::fclose(f);}}
                if(FILE* f=std::fopen((dir+"/eye-dump-warp.json").c_str(),"wb")){std::fwrite(json.data(),1,json.size(),f);std::fclose(f);}
                spacewarp::dumpFilesWritten.fetch_add(1,std::memory_order_release);
            }catch(...){}
        };
        if(spacewarp::dumpSynchronous.load(std::memory_order_relaxed))write();
        else std::thread(std::move(write)).detach();
        ++d.written;d.lastFiles=std::to_string(keep.size())+" files, "+state;
    }catch(...){++d.failed;d.lastError="finish";}
    d.request=0;d.slot=-1;d.producerRecorded=d.producerDone=d.consumerRecorded=d.consumerDone=false;d.meta.clear();d.consumerMeta.clear();
    d.preroll=d.prevRecorded=d.prevDone=false;d.prevSlot=-1;
}
inline void SpaceWarp::dumpDestroyImages() noexcept {
    auto& d=dump_;
    for(auto& fb:d.fb){if(fb&&device_)vkDestroyFramebuffer(device_,fb,nullptr);fb={};}
    for(auto& i:d.mv)destroyOwnedImage(device_,i);
    for(auto& i:d.depth)destroyOwnedImage(device_,i);
    d.mirrorExtent={};d.mirrorDepth={};
}
inline void SpaceWarp::dumpDestroy() noexcept {
    auto& d=dump_;if(!device_)return;
    if(d.request)dumpPoll();
    if(d.request)dumpFinish("discarded"); // every submission completed (caller): what landed is written
    dumpDestroyImages();
    for(auto& fb:d.dualFb){if(fb)vkDestroyFramebuffer(device_,fb,nullptr);fb={};}
    for(auto& v:d.dualAttach){if(v)vkDestroyImageView(device_,v,nullptr);v={};}
    for(auto& i:d.dualImage)destroyOwnedImage(device_,i);
    d.dualFormat={};d.dualExtent={};
    if(d.buffer){vkDestroyBuffer(device_,d.buffer,nullptr);vkFreeMemory(device_,d.memory,nullptr);}
    if(d.event)vkDestroyEvent(device_,d.event,nullptr);
    d.buffer={};d.memory={};d.mapped={};d.capacity=0;d.event={};
}
// ------------------------------------------------------------------------------------------ depth direct (capture side)
inline VkFormat SpaceWarpDirect::targetFormat(VkPhysicalDevice physical) noexcept {
    return physical?swdetail::slotDepthFormat(physical,VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT):VK_FORMAT_UNDEFINED;
}
// Builds the shared objects: a depth-only pass into the target (DONT_CARE -> STORE, UNDEFINED -> TRANSFER_SRC_OPTIMAL), the
// mailbox's downsample pipeline state. Runs on the std::async thread: no lock held, no engine command buffer involved.
inline SpaceWarpDirectShared SpaceWarpDirectPipeline::build(VkDevice d,VkFormat format) {
    const auto start=diagnostics::monotonicNs();
    Shared s{};s.format=format;VkShaderModule vs{},fs{};
    try{
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
        sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checkVk(vkCreateSampler(d,&sampler,nullptr,&s.sampler),"spacewarp direct sampler");
        const VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ds.bindingCount=1;ds.pBindings=&binding;
        checkVk(vkCreateDescriptorSetLayout(d,&ds,nullptr,&s.setLayout),"spacewarp direct descriptor layout");
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&s.setLayout; // no push constants
        checkVk(vkCreatePipelineLayout(d,&pl,nullptr,&s.layout),"spacewarp direct pipeline layout");
        VkAttachmentDescription att{};att.format=format;att.samples=VK_SAMPLE_COUNT_1_BIT;att.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;att.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;att.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        const VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.pDepthStencilAttachment=&ref;
        std::array<VkSubpassDependency,2> dep{};
        // in: the previous mailbox copy of this image (transfer read, earlier submission) before the UNDEFINED transition + writes
        dep[0].srcSubpass=VK_SUBPASS_EXTERNAL;dep[0].dstSubpass=0;dep[0].srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
        dep[0].dstStageMask=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;dep[0].srcAccessMask=0;
        dep[0].dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // out: depth writes + the final TRANSFER_SRC transition visible to the mailbox copy (later submission, same queue)
        dep[1].srcSubpass=0;dep[1].dstSubpass=VK_SUBPASS_EXTERNAL;dep[1].srcStageMask=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dep[1].srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;dep[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&att;rp.subpassCount=1;rp.pSubpasses=&sub;
        rp.dependencyCount=std::uint32_t(dep.size());rp.pDependencies=dep.data();
        checkVk(vkCreateRenderPass(d,&rp,nullptr,&s.pass),"spacewarp direct render pass");
        VkShaderModuleCreateInfo mod{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mod.codeSize=sizeof(spacewarp::shaders::vertex);mod.pCode=spacewarp::shaders::vertex;
        checkVk(vkCreateShaderModule(d,&mod,nullptr,&vs),"spacewarp direct vertex shader");
        mod.codeSize=sizeof(spacewarp::shaders::depthDownsample);mod.pCode=spacewarp::shaders::depthDownsample;
        checkVk(vkCreateShaderModule(d,&mod,nullptr,&fs),"spacewarp direct fragment shader");
        VkPipelineShaderStageCreateInfo stages[2]{};stages[0].sType=stages[1].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[0].pName="main";
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;stages[1].pName="main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable=VK_TRUE;dss.depthWriteEnable=VK_TRUE;dss.depthCompareOp=VK_COMPARE_OP_ALWAYS; // gl_FragDepth for every pixel
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; // no colour attachment
        const VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;
        pi.pViewportState=&vp;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pDepthStencilState=&dss;pi.pColorBlendState=&cb;pi.pDynamicState=&dy;pi.layout=s.layout;pi.renderPass=s.pass;
        checkVk(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&pi,nullptr,&s.pipeline),"spacewarp direct pipeline");
    }catch(...){
        if(vs)vkDestroyShaderModule(d,vs,nullptr);
        if(fs)vkDestroyShaderModule(d,fs,nullptr);
        destroyObjects(d,s);throw;
    }
    vkDestroyShaderModule(d,vs,nullptr);vkDestroyShaderModule(d,fs,nullptr);
    s.buildNs=std::max<std::uint64_t>(1,diagnostics::monotonicNs()-start);s.state=Shared::Ready;return s;
}
inline void SpaceWarpDirectPipeline::destroyObjects(VkDevice d,Shared& s) noexcept {
    if(d){
        if(s.pipeline)vkDestroyPipeline(d,s.pipeline,nullptr);
        if(s.pass)vkDestroyRenderPass(d,s.pass,nullptr);
        if(s.layout)vkDestroyPipelineLayout(d,s.layout,nullptr);
        if(s.setLayout)vkDestroyDescriptorSetLayout(d,s.setLayout,nullptr);
        if(s.sampler)vkDestroySampler(d,s.sampler,nullptr);
    }
    s=Shared{};
}
inline void SpaceWarpDirectPipeline::collect() noexcept {
    try{
        if(!build_.valid()||build_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        try{shared_=build_.get();shared_.id=++nextId_;++builds_;lastBuildNs_=shared_.buildNs;}
        catch(...){failed_=true;++failures_;} // the copy path from now on (fallback "build") until destroy()
    }catch(...){}
}
inline void SpaceWarpDirectPipeline::requestLocked(VkDevice d,VkFormat format) noexcept {
    if(!d||format==VK_FORMAT_UNDEFINED)return;
    collect();
    if(device_&&device_!=d){ // a new game device: the old one is gone (one device per process), its handles are only forgotten
        if(build_.valid())return;
        shared_=Shared{};failed_=false;device_={};format_={};
    }
    if(shared_.pipeline||build_.valid()||failed_)return;
    device_=d;format_=format;++requests_;
    try{build_=std::async(std::launch::async,[d,format]{return SpaceWarpDirectPipeline::build(d,format);});}
    catch(...){failed_=true;++failures_;} // no thread: copy path ("build")
}
inline void SpaceWarpDirectPipeline::request(VkDevice d,VkFormat format) noexcept {std::lock_guard lock(mutex_);requestLocked(d,format);}
inline unsigned SpaceWarpDirectPipeline::get(VkDevice d,VkFormat format,Shared& out) noexcept {
    std::lock_guard lock(mutex_);
    requestLocked(d,format); // collects a finished build; starts one when none exists (bind did not run, or after destroy)
    if(d&&device_==d&&shared_.pipeline&&shared_.format==format){out=shared_;return out.state;}
    out=Shared{};
    out.state=(d&&device_==d&&(failed_||shared_.pipeline))?Shared::Failed:build_.valid()?Shared::Building:Shared::None; // pipeline: another format
    return out.state;
}
inline bool SpaceWarpDirectPipeline::wait(std::uint64_t timeoutNs) noexcept {
    const auto end=diagnostics::monotonicNs()+timeoutNs;
    for(;;){
        {std::lock_guard lock(mutex_);collect();if(!build_.valid())return true;}
        if(diagnostics::monotonicNs()>=end)return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
inline void SpaceWarpDirectPipeline::destroy(VkDevice d) noexcept {
    std::future<Shared> pending;Shared s{};
    {
        std::lock_guard lock(mutex_);
        if(!d||d!=device_)return;
        pending=std::move(build_);s=shared_;shared_=Shared{};failed_=false;device_={};format_={};
    }
    if(pending.valid()){try{auto late=pending.get();destroyObjects(d,late);}catch(...){}} // outside mutex_ (report() never waits for it)
    destroyObjects(d,s);
}
inline std::string SpaceWarpDirectPipeline::json() const {
    std::lock_guard lock(mutex_);std::ostringstream o;
    const char* state=shared_.pipeline?"ready":failed_?"failed":build_.valid()?"building":"none";
    o<<"{\"state\":\""<<state<<"\",\"requests\":"<<requests_<<",\"builds\":"<<builds_<<",\"failures\":"<<failures_
     <<",\"buildMs\":"<<double(lastBuildNs_)/1e6<<'}';
    return o.str();
}
// ------------------------------------------------------------------------------------------ depth direct (per capture slot)
inline void SpaceWarpDirect::destroyFramebuffers(VkDevice d) noexcept {
    for(auto& e:eyes_){if(e.framebuffer)vkDestroyFramebuffer(d,e.framebuffer,nullptr);e.framebuffer={};}
}
inline void SpaceWarpDirect::destroySets(VkDevice d) noexcept {
    if(pool_)vkDestroyDescriptorPool(d,pool_,nullptr); // frees both sets
    pool_={};sharedId_=0;pass_={};for(auto& e:eyes_)e.set={};
}
// This slot's 2-set pool (shared set layout) and, through images(), framebuffers against the shared render pass. Re-made only for
// another shared generation (never within one device); record()'s precondition (the slot's previous pass retired) covers the old ones.
inline bool SpaceWarpDirect::bindShared(VkDevice d,const SpaceWarpDirectShared& s) noexcept {
    if(pool_&&sharedId_==s.id)return true;
    destroySets(d);destroyFramebuffers(d);
    try{
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=2;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        checkVk(vkCreateDescriptorPool(d,&pool,nullptr,&pool_),"spacewarp direct descriptor pool");
        const std::array<VkDescriptorSetLayout,2> layouts{s.setLayout,s.setLayout};std::array<VkDescriptorSet,2> sets{};
        VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};a.descriptorPool=pool_;a.descriptorSetCount=2;a.pSetLayouts=layouts.data();
        checkVk(vkAllocateDescriptorSets(d,&a,sets.data()),"spacewarp direct descriptor sets");
        eyes_[0].set=sets[0];eyes_[1].set=sets[1];
    }catch(...){destroySets(d);return false;}
    sharedId_=s.id;pass_=s.pass;return true;
}
inline void SpaceWarpDirect::destroyImages(VkDevice d) noexcept {
    destroyFramebuffers(d);
    for(auto& e:eyes_){
        if(e.attach)vkDestroyImageView(d,e.attach,nullptr);
        destroyOwnedImage(d,e.image);e.attach={};
    }
    format_={};extent_={};
}
inline bool SpaceWarpDirect::images(VkPhysicalDevice physical,VkDevice d,VkFormat format,Extent extent) noexcept {
    if(!matches(format,extent)){
        destroyImages(d); // the caller waited for the last mailbox copy before any replacement
        try{
            for(auto& e:eyes_){
                // TRANSFER_SRC: the mailbox copy (and the host GPU test read-back). DEPTH view = the attachment view without stencil.
                e.image=createOwnedImage(physical,d,extent,format,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,true);
                if(swdetail::hasStencil(format)){ // a combined depth/stencil attachment view covers both aspects
                    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=e.image.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=format;
                    v.subresourceRange={swdetail::depthAspects(format),0,1,0,1};
                    checkVk(vkCreateImageView(d,&v,nullptr,&e.attach),"spacewarp direct attachment view");
                }
            }
        }catch(...){destroyImages(d);return false;}
        format_=format;extent_=extent;++creates_;
    }
    try{
        for(auto& e:eyes_)if(!e.framebuffer){ // Against the shared render pass (bindShared)
            const VkImageView view=e.attach?e.attach:e.image.view;
            VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};f.renderPass=pass_;f.attachmentCount=1;f.pAttachments=&view;
            f.width=extent_.width;f.height=extent_.height;f.layers=1;checkVk(vkCreateFramebuffer(d,&f,nullptr,&e.framebuffer),"spacewarp direct framebuffer");
        }
    }catch(...){destroyFramebuffers(d);return false;}
    return true;
}
inline void SpaceWarpDirect::retired(VkDevice d) noexcept {
    for(auto& e:eyes_){if(e.source&&d)vkDestroyImageView(d,e.source,nullptr);e.source={};}
}
inline void SpaceWarpDirect::destroy(VkDevice d) noexcept {
    if(!d)return;
    retired(d);destroyImages(d);destroySets(d);
}
inline bool SpaceWarpDirect::record(VkPhysicalDevice physical,VkDevice d,VkCommandBuffer cmd,unsigned eye,const Source& src,Extent target,
                                    const SpaceWarpDirectShared* shared,Ready ready,Restore restore,VkQueryPool queries,std::uint32_t first,unsigned& why) noexcept {
    namespace sw=spacewarp;
    if(eye>1||!physical||!d||!cmd||!src.image){why=sw::DirectUsage;return false;}
    // The pass binds a pipeline, a descriptor set and dynamic state in the engine's command buffer: only with the engine restore.
    if(!ready||!restore||!ready(cmd)){why=sw::DirectRestore;return false;}
    VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(physical,src.format,&p);
    if(!(src.usage&VK_IMAGE_USAGE_SAMPLED_BIT)||!(p.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)){why=sw::DirectUsage;return false;}
    const VkFormat format=targetFor(physical);
    if(format==VK_FORMAT_UNDEFINED||!target.width||!target.height||target.width>src.extent.width||target.height>src.extent.height){why=sw::DirectFormat;return false;}
    // The shared objects are built in the background (SpaceWarpDirectPipeline), never here.
    if(!shared||shared->state!=SpaceWarpDirectShared::Ready||!shared->pipeline||shared->format!=format){
        why=shared&&(shared->state==SpaceWarpDirectShared::Failed||shared->state==SpaceWarpDirectShared::Ready)?sw::DirectBuild:sw::DirectBuilding;return false;}
    if(!bindShared(d,*shared)){why=sw::DirectBuild;return false;}
    if(!images(physical,d,format,target)){why=sw::DirectImages;return false;}
    auto& e=eyes_[eye];
    if(e.source){vkDestroyImageView(d,e.source,nullptr);e.source={};} // normally released at the slot's (GPU-proven) retirement
    // Per frame: the engine may destroy and re-create its depth under the same handle, so no view is cached across pairs.
    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=src.image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=src.format;
    v.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,src.layer,1};
    if(vkCreateImageView(d,&v,nullptr,&e.source)!=VK_SUCCESS){e.source={};why=sw::DirectView;return false;}
    const VkDescriptorImageInfo info{shared->sampler,e.source,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=e.set;w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w.pImageInfo=&info;
    vkUpdateDescriptorSets(d,1,&w,0,nullptr); // this eye's set: its last use (the slot's previous pass) has retired
    // ---- recording (no failure past this point) ----
    const auto aspect=swdetail::depthAspects(src.format);
    if(queries)vkCmdResetQueryPool(cmd,queries,first,2);
    // Engine depth -> sampled (DEPTH_STENCIL_READ_ONLY, as TAA LITE). First scope: its writers (depth/stencil attachment of the engine
    // and the hands, a depth-resolve copy) and readers (TAA LITE; TAA/hands restores end in ALL_COMMANDS, so they chain). The eye's
    // colour capture copy just before touches another image and is not waited for.
    const auto in=swdetail::imageBarrier(src.image,aspect,src.layout,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,src.layer);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&in);
    if(queries)vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,first);
    VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=pass_;b.framebuffer=e.framebuffer;b.renderArea.extent={extent_.width,extent_.height};
    vkCmdBeginRenderPass(cmd,&b,VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0,0,float(extent_.width),float(extent_.height),0,1};const VkRect2D scissor{{0,0},{extent_.width,extent_.height}};
    vkCmdSetViewport(cmd,0,1,&viewport);vkCmdSetScissor(cmd,0,1,&scissor);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,shared->pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,shared->layout,0,1,&e.set,0,nullptr);
    vkCmdDraw(cmd,3,1,0,0);
    vkCmdEndRenderPass(cmd);
    // Back to the layout the engine tracks; what the engine records next is unknown, so the second scope is ALL_COMMANDS (the first
    // scope is only this pass's sampling: a write-after-read, no availability needed).
    const auto out=swdetail::imageBarrier(src.image,aspect,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,src.layout,0,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,src.layer);
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&out);
    if(queries)vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,first+1);
    restore(cmd); // the engine's bound pipeline / sets / dynamic state (GameXr.cpp handsRestore, as after TAA and the hands)
    return true;
}
} // namespace rdrvr::runtime
