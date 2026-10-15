// Render-thread VR hands pass. See HandsPass.h for the threading contract.
//
// Per eye (EngineStereoPass, engine frame command buffer, outside any engine render pass, before the capture copy):
//   1. every fallible step first (pose of the plan's XR frame, matrices, pixel rectangle, pipelines, views, framebuffer,
//      descriptor set); any miss = this eye is skipped with nothing recorded,
//   2. eye colour: received layout -> COLOR_ATTACHMENT_OPTIMAL; world depth: received layout -> DEPTH_STENCIL_READ_ONLY
//      (SAMPLED) or TRANSFER_SRC + a renderArea-sized copy into a private depth (COPY),
//   3. one private render pass, renderArea = scissor = projected bounds of hands + markers, viewport = whole eye:
//      left hand, right hand (UXRH, GPU four-pose blend), holster markers (alpha rings),
//   4. every image back to the layout it was received in, then the engine's bound state restored (EngineRestore).
// Depth modes: SAMPLED = private transient D16/D32 (hand self-occlusion, CLEAR/DONT_CARE) + texelFetch of the eye's world
// depth with discard (preferred); COPY = the world depth rectangle copied into a private depth of the
// world format, hands depth-tested/written against it (used when the world depth is not SAMPLED; no write to the world
// depth either way); NONE = hands over the world (only when the eye depth is unknown / unsupported).
//
// The worker never submits anything: a fenced albedo upload on the session queue would need
// privateQueueMutex, which the XR thread holds for the whole XrHarness::frame() (xrWaitFrame included) and
// re-locks right after the unlock (GameXr.cpp startSession loop), so a try_lock poll practically never gets it.
// The worker only creates objects (staging buffer filled on the CPU) and the copy is recorded by the
// render thread into the engine frame command buffer before the first hands draw (re-recorded each frame until that frame
// retired, the staging buffer freed after the last one retired) - no queue, no mutex, no fence (the TAA module pattern).
// Every build step is visible in report().build (stage, per-stage times, lastError); BUILDING (and a requested pipeline) times
// out after 10 s into FAILED (BUILD_TIMEOUT / PIPELINE_TIMEOUT, build.timeoutStage = the stage it hung in).
//
// ARC: arc channels (ArcBoard per channel, published by the feature ticks) are drawn in the same private render pass
// after hands -> holster markers -> laser: ribbons (one instance per segment
// from a host-coherent vertex ring, 8 slots x arcRingSlotBytes, written once per render frame) -> landing pillars -> end markers. Overlay-source
// table: the early gate keeps the pass alive while ANY source has visible content (arcs, HANDS-OFF holster markers, reload overlay,
// props), arcs draw outside the pose gate (producers clear on invalid input). Arc objects are built
// in their own try blocks: an arc failure (ARC_SHADER / ARC_RING / ARC_PIPELINE) never changes the hands state. Pipelines are prewarmed
// while a channel is armed and the pass is otherwise idle. Arc-only passes whose arcs need no world test (or .arc.depth 0) use NONE depth
// (no engine-depth barrier). GPU time of the 1-in-16 timed frames is bucketed by (contents, depth mode, TAA ran on the eye).
#include "HandsPass.h"
#include "HandMesh.h"
#include "HandsAssets.h"
#include "HandsShaders.h"
#include "TaaHook.h"     // ARC: taa::recordedEyeKey (gpu.split bucket)
#include "VrTeleportLink.h" // TELEPORT: pair identity (fade::notePair) for the blink's hold
#include "VrSettings.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::hands {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
std::uint64_t steadyNs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void logLine(const char* what,const char* detail) noexcept {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_WARN,"RdrVrHands","%s: %s",what,detail?detail:"");
#else
    (void)what;(void)detail;
#endif
}
// Property text (Android) or nothing (host). Returns false when unset.
bool propertyText(const char* name,char* out,std::size_t size) noexcept {
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX]{};
    if(__system_property_get(name,value)<=0||!value[0])return false;
    std::snprintf(out,size,"%s",value);return true;
#else
    (void)name;(void)out;(void)size;return false;
#endif
}
int propertyInt(const char* name,int fallback) noexcept {
    char text[96]{};if(!propertyText(name,text,sizeof text))return fallback;
    char* end{};const long v=std::strtol(text,&end,10);
    return end!=text&&*end==0?int(v):fallback;
}

constexpr VkFormat albedoFormat=VK_FORMAT_R8G8B8A8_SRGB;
constexpr float depthEpsilon=2e-6f; // D24 step 6e-8: margin against z-fighting where a hand touches a surface
constexpr float ambient=0.55f;      // SA / sandbox shading
constexpr std::uint32_t pushBytes=128;
constexpr unsigned pipelineSlots=6,maxCacheEntries=32;
constexpr std::uint64_t maxSampleAgeNs=250'000'000;
constexpr unsigned defaultBuildTimeoutMs=10'000; // BUILDING / a requested pipeline -> FAILED after this long

// observable build steps (report().build.stage / build.stagesMs). Order = execution order of workerMain.
enum class Stage : unsigned {NotStarted,ThreadStart,WorkerEntered,MeshParse,Formats,MeshBuffer,Images,Staging,Views,Descriptors,Shaders,
    Timestamps,Published,Failed,Count};
constexpr const char* stageNames[unsigned(Stage::Count)]{"NOT_STARTED","THREAD_START","WORKER_ENTERED","MESH_PARSE","FORMATS","MESH_BUFFER",
    "IMAGES","STAGING","VIEWS_SAMPLERS","DESCRIPTORS","SHADERS","TIMESTAMPS","PUBLISHED","FAILED"};
// render-thread upload of the albedo + dummy images into the engine command buffer.
enum UploadPhase : unsigned {UploadPending,UploadRecording,UploadDone};
constexpr const char* uploadNames[3]{"PENDING","RECORDING","DONE"};

enum class DepthMode : unsigned {None,Sampled,Copy,Count};
const char* depthName(DepthMode d) noexcept {return d==DepthMode::Sampled?"SAMPLED":d==DepthMode::Copy?"COPY":"NONE";}
enum Skip : unsigned {SkipDisabled,SkipSuppressed,SkipBuilding,SkipDevice,SkipPlan,SkipColor,SkipRestore,SkipNoPose,SkipEpoch,
    SkipStale,SkipHidden,SkipOffscreen,SkipCache,SkipCount};
constexpr const char* skipNames[SkipCount]{"disabled","suppressed","building","device","plan","color","restore","noPose","epoch",
    "stale","hidden","offscreen","cache"};

bool hasStencil(VkFormat f) noexcept {return f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_D32_SFLOAT_S8_UINT||f==VK_FORMAT_D16_UNORM_S8_UINT||f==VK_FORMAT_S8_UINT;}
bool depthFormat(VkFormat f) noexcept {
    return f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_X8_D24_UNORM_PACK32||f==VK_FORMAT_D32_SFLOAT||f==VK_FORMAT_D32_SFLOAT_S8_UINT||
        f==VK_FORMAT_D16_UNORM||f==VK_FORMAT_D16_UNORM_S8_UINT;
}
VkImageAspectFlags depthAspects(VkFormat f) noexcept {return VK_IMAGE_ASPECT_DEPTH_BIT|(hasStencil(f)?VK_IMAGE_ASPECT_STENCIL_BIT:0);}
bool colorLayoutOk(VkImageLayout l) noexcept {
    return l==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL||l==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL||l==VK_IMAGE_LAYOUT_GENERAL||l==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}
bool depthLayoutOk(VkImageLayout l) noexcept {
    return l==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL||l==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL||l==VK_IMAGE_LAYOUT_GENERAL||
        l==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL||l==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL||
        l==VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL||l==VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
}
// Output encode for the eye format (hands.frag encodeOutput): SRGB attachment encodes itself, UNORM needs the curve,
// RGBA16F is the linear HDR path the mailbox Reinhard-maps (tonemap.frag c/(1+c)). -1 = unsupported.
int encodeFor(VkFormat f) noexcept {
    switch(f){
    case VK_FORMAT_B8G8R8A8_SRGB:case VK_FORMAT_R8G8B8A8_SRGB:return 0;
    case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_A2B10G10R10_UNORM_PACK32:return 1;
    case VK_FORMAT_R16G16B16A16_SFLOAT:case VK_FORMAT_B10G11R11_UFLOAT_PACK32:return 2;
    default:return -1;
    }
}
struct VkFailure {const char* what;};
void check(VkResult r,const char* what) {if(r!=VK_SUCCESS)throw VkFailure{what};}

void imageBarrier(VkCommandBuffer c,VkImage image,VkImageAspectFlags aspect,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst,
                  std::uint32_t mips=1) noexcept {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=from;b.newLayout=to;b.srcAccessMask=src;b.dstAccessMask=dst;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={aspect,0,mips,0,1};
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
constexpr VkAccessFlags anyAccess=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;

// ---------------------------------------------------------------------------------------------------------------------
// Shared GPU objects (worker-built, immutable once published).
struct Base {
    VkBuffer mesh{};VkDeviceMemory meshMemory{};
    std::array<VkDeviceSize,2> vertexOffset{},indexOffset{};std::array<std::uint32_t,2> indexCount{};
    std::array<Vec3,2> lo{},hi{};
    VkImage albedo{},dummy{};VkDeviceMemory albedoMemory{},dummyMemory{};VkImageView albedoView{},dummyView{};
    VkSampler linear{},nearest{};
    VkDescriptorSetLayout setLayout{};VkPipelineLayout layout{};VkDescriptorPool pool{};VkDescriptorSet dummySet{};
    VkShaderModule vertex{},fragment{},markerVertex{},markerFragment{},laserVertex{},laserFragment{};
    VkShaderModule cartridgeVertex{},cartridgeFragment{}; // RELOAD: procedural cartridges
    // ARC: shader modules + the vertex ring (8 slots x arcRingSlotBytes, host-visible + coherent, mapped persistently)
    VkShaderModule arcVertex{},arcFragment{},arcMarkerVertex{},arcMarkerFragment{};
    VkBuffer arcRing{};VkDeviceMemory arcRingMemory{};unsigned char* arcRingMapped{};bool arcReady{};
    VkFormat ownDepth{};
    VkQueryPool queries{};double timestampPeriod{};std::uint64_t timestampMask{};
    std::uint64_t buildNs{};
    // albedo mip chain, host-visible, filled by the worker; copied by the render thread (recordUpload), freed once
    // the last frame that recorded the copy retired (render thread) or at shutdownAfterIdle.
    VkBuffer staging{};VkDeviceMemory stagingMemory{};
};
struct PipelineSlot {
    std::atomic<int> status{0}; // 0 free, 1 requested, 2 ready, 3 failed
    VkFormat color{},depth{};bool load{};
    VkRenderPass pass{};VkPipeline hands{},markers{},laser{};std::uint64_t buildNs{};
    VkPipeline cartridge{}; // RELOAD: own try (a failure only hides the cartridges)
    std::atomic<std::uint64_t> requestNs{};std::atomic<const char*> error{""}; // Request time, failure step
    VkPipeline arc{},arcMarker{};std::atomic<int> arcStatus{0};std::atomic<const char*> arcError{""}; // ARC: 0 none, 2 built, 3 failed
};
// Render-thread caches (mutex shared with imageDestroyed only).
struct ColorView {VkImage image{};std::uint64_t generation{};VkFormat format{};VkImageView view{};std::uint64_t lastUse{};};
struct DepthView {VkImage image{};std::uint64_t generation{};VkImageView view{};VkDescriptorSet set{};std::uint64_t lastUse{};};
struct Framebuffer {VkRenderPass pass{};VkImageView color{},depth{};VkImage colorImage{};VkFramebuffer framebuffer{};VkExtent2D extent{};std::uint64_t lastUse{};};
struct OwnImage {VkImage image{};VkDeviceMemory memory{};VkImageView view{};VkFormat format{};VkExtent2D extent{};std::uint64_t lastUse{};};
struct Dead {
    std::uint64_t frame{};VkFramebuffer framebuffer{};VkImageView view{};VkImage image{};VkDeviceMemory memory{};VkDescriptorSet set{};
};
struct FrameHands {
    std::uint64_t renderFrame{};bool computed{},any{};
    std::array<bool,2> visible{};std::array<DMat4,2> model{};std::array<std::array<float,4>,2> weights{};std::array<std::array<float,3>,2> light{};
    MarkerSet markers{};bool markersUngated{}; // LASSO: HANDS-OFF holster markers (no pose gate, overlay channel 2)
    std::array<Marker,16> reload{};std::uint32_t reloadCount{}; // RELOAD: overlay channel 2b (no pose gate, no world test)
    // RELOAD: per reload item (cartridge styles): axis (LOCAL unit, rim -> bullet), the occluder (rear-face centre - base, outer radius,
    // length; radius 0 = none)
    struct Cart {float axis[3]{};float cyl[4]{};float cylLen{};};std::array<Cart,16> reloadCart{};
    PoseMatch match{PoseMatch::None};std::int64_t delta{};Skip skip{SkipCount};
    // VR-held weapon frame of this plan: held hand drawn with the gun's own sample/map, laser sight segment (LOCAL metres)
    bool weapon{},heldSynced{},laser{},laserDot{};Vec3 laserA{},laserB{};float laserHalfWidth{};
    // DUAL: the phantom (second item) frame: its hand synced, its laser (a phantom firearm)
    bool phantomWeapon{},laser2{},laser2Dot{};Vec3 laser2A{},laser2B{};float laser2HalfWidth{};
};
struct Counters {
    std::atomic<std::uint64_t> calls{},drawnEyes{},drawnHands{},drawnMarkers{},frames{};
    std::array<std::atomic<std::uint64_t>,2> drawnPerEye{};
    std::array<std::atomic<std::uint64_t>,SkipCount> skips{};
    std::array<std::atomic<std::uint64_t>,4> matches{};
    std::array<std::atomic<std::uint64_t>,unsigned(DepthMode::Count)> modes{};
    std::atomic<std::int64_t> lastDelta{},maxDelta{};
    std::atomic<std::uint64_t> views{},framebuffers{},sets{},imageDrops{},graveyard{},graveyardReleased{},ownImages{},copyImages{};
    std::atomic<std::uint64_t> pixels{},eyePixels{},lastRect{}; // last rect as (w<<32|h)
    std::atomic<std::uint64_t> timed{},gpuLastNs{},gpuAvgNs{},gpuUnavailable{};
    std::atomic<std::uint64_t> rigidityMicro{};std::atomic<unsigned> lastMode[2]{};
    std::atomic<std::uint64_t> restoreCalls{};
    // upload of the albedo/dummy images in the engine command buffer
    std::atomic<std::uint64_t> uploads{},uploadFirstFrame{},uploadLastFrame{},uploadDoneFrame{};
    std::atomic<unsigned> uploadPhase{UploadPending};std::atomic<bool> stagingFreed{false};
    // weapon frames (held-hand sync, laser) and the residual between the gun's map and the eye plan's LOCAL->world
    std::atomic<std::uint64_t> weaponFrames{},weaponMissing{},weaponSpace{},heldSynced{},laserFrames{},laserEyes{},laserDots{},laserHits{},
        laserMmSum{},residualFrames{},residualUmSum{},residualUmMax{},residualMdegSum{},residualMdegMax{},
        residualBridgedFrames{},residualBridgedUmMax{},residualBridgedMdegMax{},residualSteadyUmMax{},residualSteadyMdegMax{},
        phantomFrames{},phantomSynced{},phantomLaserFrames{};
    std::atomic<std::uint32_t> laserLastMm{};
};
// ---- ARC: render-side types ----
enum class ArcState : int {Off,Killed,Building,Ready,Failed};
const char* arcStateName(ArcState s) noexcept {
    switch(s){case ArcState::Off:return "OFF";case ArcState::Killed:return "KILLED";case ArcState::Building:return "BUILDING";
    case ArcState::Ready:return "READY";case ArcState::Failed:return "FAILED";}return "UNKNOWN";}
// Overlay sources (order 2 HANDS-OFF holster markers, 2a weapon markers, 2b reload overlay, 4 props, 5/6 arcs).
enum class Overlay : unsigned {Arcs,MarkersHandsOff,Reload,Props,WeaponMarkers,Count}; // HAMMER: + weapon markers (channel 2a)
constexpr const char* arcPickNames[ArcBoard::PickCount]{"exact","older","missing","stale","clearedPicks","unanchored","otherSpace","torn"};
struct ArcChannelCounters {
    std::atomic<std::uint64_t> published{},cleared{},hides{},wrongThread{},rejected{},unkeyed{},killed{},frames{},hidden{},drawn{};
    std::atomic<std::uint64_t> liveFrames{},liveExact{}; // render frames while the channel had visible content (device check: liveExact / liveFrames)
    std::array<std::atomic<std::uint64_t>,ArcBoard::PickCount> picks{};
    std::atomic<std::uint32_t> lastPoints{},lastLengthMm{};std::atomic<std::uint64_t> lastLagUs{},maxLagUs{};
};
// gpu.split buckets: contents (ARC_ONLY, MIXED = hands/markers/laser + arcs, HANDS = no arcs) x depth (NONE, SAMPLED, COPY) x TAA (0, 1)
constexpr unsigned splitContents=3,splitBuckets=splitContents*unsigned(DepthMode::Count)*2;
constexpr const char* splitContentNames[splitContents]{"ARC_ONLY","MIXED","HANDS"};
struct ArcCounters {
    std::atomic<std::uint64_t> drawnEyes{},segments{},markers{},pillars{},xrayEyes{},xrayUnavailable{},noDepthEyes{},poseGateBypassed{},
        synthesizingFrames{},ringBusy{},ringWrites{},ringBytes{},pipelinesBuilt{},pipelinesFailed{},arcOnlyPasses{},markerPasses{},reloadPasses{},
        propsPasses{},weaponMarkers{},reloadItems{},prewarmRequests{},prewarmCalls{},noArcPipeline{},computed{},markerNoDepthEyes{};
    std::atomic<std::uint64_t> firstRequestNs{},firstReadyNs{};
    std::array<std::atomic<std::uint64_t>,splitBuckets> splitFrames{},splitAvgNs{};
};
// One render frame's arcs (render thread): the CPU copy of the ring slot (rects are computed from it) + per channel draw data (LOCAL).
struct ArcDraw {
    std::uint64_t renderFrame{~0ull};bool any{},ringOk{},allNoWorldTest{true};unsigned slot{};
    struct Item {
        bool live{};std::uint32_t firstEntry{},entries{};float lengthM{};ArcStyle style{};std::uint32_t flags{};
        bool pillar{};std::uint32_t pillarFirst{};
        bool marker{};ArcMarkerShape shape{};std::uint32_t rgba{};DVec3 centre{},normal{},facing{};bool faceCamera{};
        float radiusM{},liftM{},depthTolM{},xrayAlpha{};
    };
    std::array<Item,arcChannels> items{};
    std::array<ArcVertex,arcChannels*arcRunEntries> verts{};
};
struct Module {
    PoseRing poses;
    SeqCell<MarkerSet> markers;
    WeaponBoard weapons;               // VR-held weapon frames keyed by the plan's head sample
    WeaponBoard phantoms;              // DUAL: the phantom's frames (second item, other hand)
    std::atomic<int> heldSync{1};      // debug.rdrvr.vr.hands.heldsync (default 1)
    std::atomic<int> heldHand{-1};
    std::atomic<unsigned> heldMask{0}; // DUAL: bit h = hand h holds an item (setHeldMask; setHeldHand keeps it in step)
    std::atomic<bool> suppressed{false};
    std::atomic<int> state{int(State::Off)};
    std::atomic<const char*> reason{"NOT_CONFIGURED"};
    std::atomic<bool (*)(VkCommandBuffer) noexcept> restoreReady{nullptr};
    std::atomic<void (*)(VkCommandBuffer) noexcept> restore{nullptr};
    // configuration (render thread writes before the worker starts; read-only afterwards)
    std::atomic<bool> configured{false};
    VkPhysicalDevice physical{};VkDevice device{};std::uint32_t family{},queueIndex{};std::mutex* queueMutex{};
    std::atomic<int> debugDepth{-1},brightPercent{100},gpuTiming{1};bool debugOverride{};
    // worker
    std::thread worker;std::mutex workMutex;std::condition_variable workCv;std::atomic<bool> stop{false};
    Base base{};std::atomic<bool> baseReady{false};
    std::array<PipelineSlot,pipelineSlots> slots{};
    // build observability + timeout (steady ns; 0 = not reached)
    std::atomic<unsigned> stage{unsigned(Stage::NotStarted)},timeoutStage{unsigned(Stage::NotStarted)};
    std::array<std::atomic<std::uint64_t>,unsigned(Stage::Count)> stageNs{};
    std::atomic<std::uint64_t> configureNs{},readyNs{},failedNs{};
    std::atomic<const char*> lastError{""};std::atomic<bool> workerStarted{false};
    std::atomic<unsigned> buildTimeoutMs{defaultBuildTimeoutMs};
    std::atomic<unsigned> testDelayStage{unsigned(Stage::Count)},testDelayMs{0}; // host tests: stall the worker in one stage
    unsigned uploadPhase{UploadPending};std::uint64_t uploadFirst{},uploadLast{};VkCommandBuffer uploadCommand{}; // render thread only
    // render-thread state
    std::mutex cacheMutex;
    std::vector<ColorView> colors;std::vector<DepthView> depths;std::vector<Framebuffer> framebuffers;std::vector<Dead> dead;
    OwnImage own{},copy{};bool copyUnavailable{};
    std::array<std::pair<VkFormat,VkFormatFeatureFlags>,8> formatCache{};
    std::atomic<std::uint64_t> retired{};
    FrameHands frame{};
    // timestamps: 4 slots x {eye0 begin,end, eye1 begin,end}
    std::array<std::uint64_t,4> querySlotFrame{};std::array<bool,4> querySlotPending{};std::uint64_t timedSequence{};unsigned frameQuerySlot{~0u};
    Counters counters;
    std::array<ArcBoard,arcChannels> arcBoards{};
    std::array<ArcChannelCounters,arcChannels> arcCh{};
    ArcCounters arcK{};
    std::atomic<unsigned> arcArmed{0};
    std::atomic<int> arcState{int(ArcState::Off)};
    std::atomic<const char*> arcError{""};
    std::atomic<int> arcLastStatus{-1};std::atomic<std::uint64_t> arcStatusSinceNs{0};
    std::atomic<int> arcLastColor{int(VK_FORMAT_UNDEFINED)};   // eye colour format of the last pass / prewarm (arcStatus slot lookup)
    std::atomic<int> arcXray{1},arcWidthPct{100},arcMinPx{18},arcDepthLever{1},arcAsw{1},arcRects{1}; // live levers (render thread)
    std::atomic<int> arcKillForTests{0},arcFailPipelinesForTests{0};bool arcDebugOverride{};
    std::atomic<bool (*)() noexcept> synthesizing{nullptr};
    // overlay-source table: last VISIBLE content per source (arcs come from the boards)
    std::array<std::atomic<std::uint64_t>,unsigned(Overlay::Count)> overlayVisibleNs{};
    SeqCell<ReloadOverlay> reloadOverlay;std::atomic<std::uint64_t> reloadPublished{};
    ArcDraw arcDraw{};std::array<std::uint64_t,arcRingSlots> ringSlotFrame{};unsigned leverCalls{}; // render thread
    std::array<std::array<unsigned,2>,4> querySlotBucket{};                                          // render thread: gpu.split per query slot / eye
};
Module& module() noexcept {static Module* instance=new Module;return *instance;} // never destroyed: hooks may run during exit

void setState(State s,const char* why) noexcept {
    auto& m=module();m.reason.store(why,std::memory_order_relaxed);m.state.store(int(s),std::memory_order_release);
    if(s==State::Failed){m.lastError.store(why,std::memory_order_relaxed);
        std::uint64_t none=0;m.failedNs.compare_exchange_strong(none,steadyNs(),std::memory_order_relaxed);logLine("hands disabled",why);}
}
// state change only from `from` (the worker finishing after a timeout must not turn FAILED back into READY).
bool transition(State from,State to,const char* why) noexcept {
    auto& m=module();int expected=int(from);
    if(!m.state.compare_exchange_strong(expected,int(to),std::memory_order_acq_rel))return false;
    m.reason.store(why,std::memory_order_relaxed);
    if(to==State::Failed){m.lastError.store(why,std::memory_order_relaxed);
        std::uint64_t none=0;m.failedNs.compare_exchange_strong(none,steadyNs(),std::memory_order_relaxed);logLine("hands disabled",why);}
    return true;
}
std::uint64_t timeoutNs(const Module& m) noexcept {return std::uint64_t(m.buildTimeoutMs.load(std::memory_order_relaxed))*1'000'000ull;}
// BUILDING longer than the timeout -> FAILED BUILD_TIMEOUT (render thread: configure + building skips).
void checkBuildTimeout(Module& m,std::uint64_t now) noexcept {
    if(m.state.load(std::memory_order_acquire)!=int(State::Building))return;
    const auto start=m.configureNs.load(std::memory_order_relaxed);
    if(!start||now<=start||now-start<=timeoutNs(m))return;
    const auto at=m.stage.load(std::memory_order_relaxed);
    if(transition(State::Building,State::Failed,"BUILD_TIMEOUT")){
        m.timeoutStage.store(at,std::memory_order_relaxed);logLine("hands build timed out in stage",stageNames[std::min(at,unsigned(Stage::Count)-1)]);}
}
// worker step marker; host tests may stall one stage (stop-aware) to exercise the timeout.
void enterStage(Module& m,Stage s) {
    m.stageNs[unsigned(s)].store(steadyNs(),std::memory_order_relaxed);m.stage.store(unsigned(s),std::memory_order_release);
    if(m.testDelayStage.load(std::memory_order_relaxed)!=unsigned(s))return;
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(m.testDelayMs.load(std::memory_order_relaxed));
    while(std::chrono::steady_clock::now()<end){
        if(m.stop.load(std::memory_order_acquire))throw VkFailure{"STOPPED"};
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Worker: base objects and pipelines.
std::uint32_t memoryType(VkPhysicalDevice physical,std::uint32_t bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred) noexcept {
    VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(physical,&p);
    for(unsigned pass=0;pass<2;++pass)
        for(std::uint32_t i=0;i<p.memoryTypeCount&&i<VK_MAX_MEMORY_TYPES;++i){
            const auto flags=p.memoryTypes[i].propertyFlags;
            if(!(bits&(1u<<i))||(flags&required)!=required)continue;
            if(pass==0&&(flags&preferred)!=preferred)continue;
            return i;
        }
    return UINT32_MAX;
}
VkDeviceMemory allocateFor(VkDevice device,VkPhysicalDevice physical,const VkMemoryRequirements& req,VkMemoryPropertyFlags required,
                           VkMemoryPropertyFlags preferred,const char* what) {
    const auto type=memoryType(physical,req.memoryTypeBits,required,preferred);
    if(type==UINT32_MAX)throw VkFailure{what};
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=req.size;a.memoryTypeIndex=type;
    VkDeviceMemory memory{};check(vkAllocateMemory(device,&a,nullptr,&memory),what);return memory;
}
void destroyBase(VkDevice d,Base& b) noexcept {
    if(!d){b={};return;}
    if(b.queries)vkDestroyQueryPool(d,b.queries,nullptr);
    for(auto s:{b.vertex,b.fragment,b.markerVertex,b.markerFragment,b.laserVertex,b.laserFragment,
                b.arcVertex,b.arcFragment,b.arcMarkerVertex,b.arcMarkerFragment,b.cartridgeVertex,b.cartridgeFragment})if(s)vkDestroyShaderModule(d,s,nullptr);
    if(b.arcRing)vkDestroyBuffer(d,b.arcRing,nullptr);
    if(b.arcRingMemory)vkFreeMemory(d,b.arcRingMemory,nullptr);
    if(b.pool)vkDestroyDescriptorPool(d,b.pool,nullptr);
    if(b.layout)vkDestroyPipelineLayout(d,b.layout,nullptr);
    if(b.setLayout)vkDestroyDescriptorSetLayout(d,b.setLayout,nullptr);
    if(b.linear)vkDestroySampler(d,b.linear,nullptr);
    if(b.nearest)vkDestroySampler(d,b.nearest,nullptr);
    if(b.albedoView)vkDestroyImageView(d,b.albedoView,nullptr);
    if(b.dummyView)vkDestroyImageView(d,b.dummyView,nullptr);
    if(b.albedo)vkDestroyImage(d,b.albedo,nullptr);
    if(b.dummy)vkDestroyImage(d,b.dummy,nullptr);
    if(b.albedoMemory)vkFreeMemory(d,b.albedoMemory,nullptr);
    if(b.dummyMemory)vkFreeMemory(d,b.dummyMemory,nullptr);
    if(b.mesh)vkDestroyBuffer(d,b.mesh,nullptr);
    if(b.meshMemory)vkFreeMemory(d,b.meshMemory,nullptr);
    if(b.staging)vkDestroyBuffer(d,b.staging,nullptr);
    if(b.stagingMemory)vkFreeMemory(d,b.stagingMemory,nullptr);
    b={};
}
VkShaderModule shaderModule(VkDevice d,const std::uint32_t* code,std::size_t bytes,const char* what) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=bytes;ci.pCode=code;
    VkShaderModule s{};check(vkCreateShaderModule(d,&ci,nullptr,&s),what);return s;
}
// The albedo mip chain goes into a host-visible staging buffer here (worker, CPU only). No queue submit and no queue
// mutex on the worker (see the file header).
void createStaging(Module& m,Base& b) {
    const VkDevice d=m.device;const auto chain=assets::albedo();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=chain.size();bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(d,&bi,nullptr,&b.staging),"STAGING_BUFFER");
    VkMemoryRequirements req{};vkGetBufferMemoryRequirements(d,b.staging,&req);
    b.stagingMemory=allocateFor(d,m.physical,req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,0,"STAGING_MEMORY");
    check(vkBindBufferMemory(d,b.staging,b.stagingMemory,0),"STAGING_BIND");
    void* mapped{};check(vkMapMemory(d,b.stagingMemory,0,VK_WHOLE_SIZE,0,&mapped),"STAGING_MAP");
    std::memcpy(mapped,chain.data(),chain.size());vkUnmapMemory(d,b.stagingMemory);
}
// Render thread, engine command buffer, outside any render pass: albedo mip chain from the staging buffer + the 1x1
// "infinitely far" world-depth stand-in (R8 = 1.0), both ending in SHADER_READ_ONLY_OPTIMAL. UNDEFINED -> TRANSFER_DST with
// ALL_COMMANDS/any-access source: a repeat in a later frame is ordered after every earlier use on the queue.
void recordUpload(const Base& b,VkCommandBuffer cmd) noexcept {
    imageBarrier(cmd,b.albedo,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,anyAccess,VK_ACCESS_TRANSFER_WRITE_BIT,assets::albedoLevels);
    imageBarrier(cmd,b.dummy,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,anyAccess,VK_ACCESS_TRANSFER_WRITE_BIT);
    std::array<VkBufferImageCopy,assets::albedoLevels> regions{};
    for(std::uint32_t i=0;i<assets::albedoLevels;++i){
        auto& r=regions[i];r.bufferOffset=assets::albedoLayout[i].offset;
        r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i,0,1};r.imageExtent={assets::albedoLayout[i].size,assets::albedoLayout[i].size,1};
    }
    vkCmdCopyBufferToImage(cmd,b.staging,b.albedo,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,std::uint32_t(regions.size()),regions.data());
    VkClearColorValue white{};white.float32[0]=white.float32[1]=white.float32[2]=white.float32[3]=1.f;
    const VkImageSubresourceRange one{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(cmd,b.dummy,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&white,1,&one);
    imageBarrier(cmd,b.albedo,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,assets::albedoLevels);
    imageBarrier(cmd,b.dummy,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
}
// ARC: arc failure (arcs only, the hands state is untouched): Building -> Failed with the reason.
void arcFail(Module& m,const char* why) noexcept {
    m.arcError.store(why,std::memory_order_relaxed);int expected=int(ArcState::Building);
    m.arcState.compare_exchange_strong(expected,int(ArcState::Failed),std::memory_order_acq_rel);logLine("arcs disabled",why);
}
void buildArcObjects(Module& m,Base& b) noexcept {
    if(m.arcState.load(std::memory_order_acquire)!=int(ArcState::Building))return;
    const VkDevice d=m.device;
    try{
        b.arcVertex=shaderModule(d,shaders::arcVertex,sizeof(shaders::arcVertex),"ARC_SHADER");
        b.arcFragment=shaderModule(d,shaders::arcFragment,sizeof(shaders::arcFragment),"ARC_SHADER");
        b.arcMarkerVertex=shaderModule(d,shaders::arcMarkerVertex,sizeof(shaders::arcMarkerVertex),"ARC_SHADER");
        b.arcMarkerFragment=shaderModule(d,shaders::arcMarkerFragment,sizeof(shaders::arcMarkerFragment),"ARC_SHADER");
    }catch(...){
        for(auto* s:{&b.arcVertex,&b.arcFragment,&b.arcMarkerVertex,&b.arcMarkerFragment}){if(*s)vkDestroyShaderModule(d,*s,nullptr);*s=VK_NULL_HANDLE;}
        arcFail(m,"ARC_SHADER");return;
    }
    try{
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=VkDeviceSize(arcRingSlotBytes)*arcRingSlots;
        bi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(d,&bi,nullptr,&b.arcRing),"ARC_RING");
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(d,b.arcRing,&req);
        b.arcRingMemory=allocateFor(d,m.physical,req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,"ARC_RING");
        check(vkBindBufferMemory(d,b.arcRing,b.arcRingMemory,0),"ARC_RING");
        void* mapped{};check(vkMapMemory(d,b.arcRingMemory,0,VK_WHOLE_SIZE,0,&mapped),"ARC_RING");
        b.arcRingMapped=static_cast<unsigned char*>(mapped);std::memset(b.arcRingMapped,0,std::size_t(bi.size));
    }catch(...){
        for(auto* s:{&b.arcVertex,&b.arcFragment,&b.arcMarkerVertex,&b.arcMarkerFragment}){if(*s)vkDestroyShaderModule(d,*s,nullptr);*s=VK_NULL_HANDLE;}
        if(b.arcRing)vkDestroyBuffer(d,b.arcRing,nullptr);if(b.arcRingMemory)vkFreeMemory(d,b.arcRingMemory,nullptr);
        b.arcRing={};b.arcRingMemory={};b.arcRingMapped=nullptr;arcFail(m,"ARC_RING");return;
    }
    b.arcReady=true;
}
void buildBase(Module& m) {
    const auto start=steadyNs();
    const VkDevice d=m.device;Base b{};
    try{
        enterStage(m,Stage::MeshParse);
        std::array<HandMesh,2> mesh{};const char* why{};
        if(!mesh[0].parse(assets::leftMesh(),&why)||!mesh[1].parse(assets::rightMesh(),&why))throw VkFailure{why?why:"MESH"};
        enterStage(m,Stage::Formats);
        VkFormatProperties albedoProps{};vkGetPhysicalDeviceFormatProperties(m.physical,albedoFormat,&albedoProps);
        const VkFormatFeatureFlags sampled=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if((albedoProps.optimalTilingFeatures&sampled)!=sampled)throw VkFailure{"ALBEDO_FORMAT"};
        for(VkFormat f:{VK_FORMAT_D16_UNORM,VK_FORMAT_D32_SFLOAT,VK_FORMAT_X8_D24_UNORM_PACK32,VK_FORMAT_D24_UNORM_S8_UINT}){
            VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(m.physical,f,&p);
            if(p.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT){b.ownDepth=f;break;}
        }
        if(!b.ownDepth)throw VkFailure{"OWN_DEPTH_FORMAT"};
        // Mesh: one host-visible buffer {VB left, VB right, IB left, IB right}, written once before publication.
        enterStage(m,Stage::MeshBuffer);
        VkDeviceSize size=0;const auto align=[](VkDeviceSize v){return (v+15)&~VkDeviceSize(15);};
        for(unsigned h=0;h<2;++h){b.vertexOffset[h]=size;size=align(size+mesh[h].vertexBytes.size());}
        for(unsigned h=0;h<2;++h){b.indexOffset[h]=size;size=align(size+mesh[h].indexBytes.size());b.indexCount[h]=mesh[h].indexCount;b.lo[h]=mesh[h].lo;b.hi[h]=mesh[h].hi;}
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(d,&bi,nullptr,&b.mesh),"MESH_BUFFER");
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(d,b.mesh,&req);
        b.meshMemory=allocateFor(d,m.physical,req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,"MESH_MEMORY");
        check(vkBindBufferMemory(d,b.mesh,b.meshMemory,0),"MESH_BIND");
        void* mapped{};check(vkMapMemory(d,b.meshMemory,0,VK_WHOLE_SIZE,0,&mapped),"MESH_MAP");
        for(unsigned h=0;h<2;++h){
            std::memcpy(static_cast<unsigned char*>(mapped)+b.vertexOffset[h],mesh[h].vertexBytes.data(),mesh[h].vertexBytes.size());
            std::memcpy(static_cast<unsigned char*>(mapped)+b.indexOffset[h],mesh[h].indexBytes.data(),mesh[h].indexBytes.size());
        }
        vkUnmapMemory(d,b.meshMemory);
        // Images.
        enterStage(m,Stage::Images);
        const auto image=[&](VkFormat format,std::uint32_t extent,std::uint32_t mips,VkImage& out,VkDeviceMemory& memory,const char* what){
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={extent,extent,1};
            ci.mipLevels=mips;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
            ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
            check(vkCreateImage(d,&ci,nullptr,&out),what);
            VkMemoryRequirements r{};vkGetImageMemoryRequirements(d,out,&r);
            memory=allocateFor(d,m.physical,r,0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,what);check(vkBindImageMemory(d,out,memory,0),what);
        };
        image(albedoFormat,assets::albedoSize,assets::albedoLevels,b.albedo,b.albedoMemory,"ALBEDO_IMAGE");
        image(VK_FORMAT_R8G8B8A8_UNORM,1,1,b.dummy,b.dummyMemory,"DUMMY_IMAGE");
        enterStage(m,Stage::Staging);
        createStaging(m,b); // Copied by the render thread (recordUpload), no queue submit here
        enterStage(m,Stage::Views);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
        vi.image=b.albedo;vi.format=albedoFormat;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,assets::albedoLevels,0,1};
        check(vkCreateImageView(d,&vi,nullptr,&b.albedoView),"ALBEDO_VIEW");
        vi.image=b.dummy;vi.format=VK_FORMAT_R8G8B8A8_UNORM;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        check(vkCreateImageView(d,&vi,nullptr,&b.dummyView),"DUMMY_VIEW");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=float(assets::albedoLevels);
        check(vkCreateSampler(d,&si,nullptr,&b.linear),"SAMPLER");
        si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;si.maxLod=0;
        check(vkCreateSampler(d,&si,nullptr,&b.nearest),"SAMPLER");
        enterStage(m,Stage::Descriptors);
        // Set 0: binding 0 albedo, binding 1 world depth (DEPTH-aspect view of the eye depth, or the dummy).
        const VkDescriptorSetLayoutBinding bindings[2]{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
                                                        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=2;dl.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(d,&dl,nullptr,&b.setLayout),"SET_LAYOUT");
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,pushBytes};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&b.setLayout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(d,&pl,nullptr,&b.layout),"PIPELINE_LAYOUT");
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*(maxCacheEntries+1)};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dp.maxSets=maxCacheEntries+1;dp.poolSizeCount=1;dp.pPoolSizes=&poolSize;
        check(vkCreateDescriptorPool(d,&dp,nullptr,&b.pool),"DESCRIPTOR_POOL");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=b.pool;da.descriptorSetCount=1;da.pSetLayouts=&b.setLayout;
        check(vkAllocateDescriptorSets(d,&da,&b.dummySet),"DUMMY_SET");
        const VkDescriptorImageInfo infos[2]{{b.linear,b.albedoView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{b.nearest,b.dummyView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[2]{};
        for(unsigned i=0;i<2;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=b.dummySet;writes[i].dstBinding=i;writes[i].descriptorCount=1;
            writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&infos[i];}
        vkUpdateDescriptorSets(d,2,writes,0,nullptr);
        enterStage(m,Stage::Shaders);
        b.vertex=shaderModule(d,shaders::vertex,sizeof(shaders::vertex),"VERTEX_SHADER");
        b.fragment=shaderModule(d,shaders::fragment,sizeof(shaders::fragment),"FRAGMENT_SHADER");
        b.markerVertex=shaderModule(d,shaders::markerVertex,sizeof(shaders::markerVertex),"MARKER_VERTEX_SHADER");
        b.markerFragment=shaderModule(d,shaders::markerFragment,sizeof(shaders::markerFragment),"MARKER_FRAGMENT_SHADER");
        b.laserVertex=shaderModule(d,shaders::laserVertex,sizeof(shaders::laserVertex),"LASER_VERTEX_SHADER");
        b.laserFragment=shaderModule(d,shaders::laserFragment,sizeof(shaders::laserFragment),"LASER_FRAGMENT_SHADER");
        b.cartridgeVertex=shaderModule(d,shaders::cartridgeVertex,sizeof(shaders::cartridgeVertex),"CARTRIDGE_SHADER");
        b.cartridgeFragment=shaderModule(d,shaders::cartridgeFragment,sizeof(shaders::cartridgeFragment),"CARTRIDGE_SHADER");
        // Optional GPU timing.
        enterStage(m,Stage::Timestamps);
        if(m.gpuTiming.load(std::memory_order_relaxed)){
            std::uint32_t count{};vkGetPhysicalDeviceQueueFamilyProperties(m.physical,&count,nullptr);
            std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(m.physical,&count,families.data());
            VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(m.physical,&props);
            if(m.family<count&&families[m.family].timestampValidBits&&props.limits.timestampPeriod>0){
                const auto bits=families[m.family].timestampValidBits;b.timestampMask=bits>=64?UINT64_MAX:(1ull<<bits)-1;b.timestampPeriod=props.limits.timestampPeriod;
                VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=16;
                if(vkCreateQueryPool(d,&q,nullptr,&b.queries)!=VK_SUCCESS)b.queries={};
            }
        }
    }catch(const VkFailure&){destroyBase(d,b);throw;}
    catch(...){destroyBase(d,b);throw VkFailure{"EXCEPTION"};}
    buildArcObjects(m,b); // ARC: own try blocks - an arc failure leaves the hands base intact
    b.buildNs=steadyNs()-start;m.base=b;
}
void destroySlot(VkDevice d,PipelineSlot& s) noexcept {
    if(d){if(s.hands)vkDestroyPipeline(d,s.hands,nullptr);if(s.markers)vkDestroyPipeline(d,s.markers,nullptr);if(s.laser)vkDestroyPipeline(d,s.laser,nullptr);
        if(s.arc)vkDestroyPipeline(d,s.arc,nullptr);if(s.arcMarker)vkDestroyPipeline(d,s.arcMarker,nullptr);
        if(s.cartridge)vkDestroyPipeline(d,s.cartridge,nullptr);
        if(s.pass)vkDestroyRenderPass(d,s.pass,nullptr);}
    s.hands=s.markers=s.laser=s.arc=s.arcMarker=s.cartridge={};s.pass={};s.arcStatus.store(0,std::memory_order_relaxed);
}
// Render pass {colour LOAD/STORE, depth} + hands pipeline + marker pipeline for one (colour format, depth format, load).
void buildSlot(Module& m,PipelineSlot& s) {
    const auto start=steadyNs();
    const VkDevice d=m.device;const Base& b=m.base;
    try{
        VkFormatProperties cp{};vkGetPhysicalDeviceFormatProperties(m.physical,s.color,&cp);
        if(!(cp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))throw VkFailure{"COLOR_FORMAT"};
        const bool blend=(cp.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)!=0;
        std::array<VkAttachmentDescription,2> a{};
        a[0].format=s.color;a[0].samples=VK_SAMPLE_COUNT_1_BIT;a[0].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        a[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[0].initialLayout=a[0].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a[1].format=s.depth;a[1].samples=VK_SAMPLE_COUNT_1_BIT;a[1].loadOp=s.load?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;
        a[1].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;a[1].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a[1].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[1].initialLayout=s.load?VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;a[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depth{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&color;sub.pDepthStencilAttachment=&depth;
        std::array<VkSubpassDependency,2> deps{};
        deps[0].srcSubpass=VK_SUBPASS_EXTERNAL;deps[0].dstSubpass=0;deps[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].srcAccessMask=anyAccess;
        deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
        deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;deps[1].dstAccessMask=anyAccess;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=2;rp.pAttachments=a.data();rp.subpassCount=1;rp.pSubpasses=&sub;
        rp.dependencyCount=std::uint32_t(deps.size());rp.pDependencies=deps.data();
        check(vkCreateRenderPass(d,&rp,nullptr,&s.pass),"RENDER_PASS");
        // Hands: UXRH vertex (stride 104): 4 positions, 4 normals, uv.
        const VkVertexInputBindingDescription binding{0,vertexStride,VK_VERTEX_INPUT_RATE_VERTEX};
        std::array<VkVertexInputAttributeDescription,9> attributes{};
        for(std::uint32_t i=0;i<8;++i)attributes[i]={i,0,VK_FORMAT_R32G32B32_SFLOAT,12*i};
        attributes[8]={8,0,VK_FORMAT_R32G32_SFLOAT,96};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&binding;vi.vertexAttributeDescriptionCount=std::uint32_t(attributes.size());vi.pVertexAttributeDescriptions=attributes.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
        // No culling: the private depth resolves self-occlusion, and the left hand is a reflection (det -1).
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;
        rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable=VK_TRUE;ds.depthWriteEnable=VK_TRUE;ds.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
        // The eye's alpha channel is never touched (RGB only).
        VkPipelineColorBlendAttachmentState opaque{};opaque.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&opaque;
        const VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
        VkPipelineShaderStageCreateInfo stages[2]{};
        for(auto& st:stages){st.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=b.vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=b.fragment;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;
        pi.pViewportState=&vp;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pDepthStencilState=&ds;pi.pColorBlendState=&cb;pi.pDynamicState=&dy;
        pi.layout=b.layout;pi.renderPass=s.pass;
        check(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&pi,nullptr,&s.hands),"HANDS_PIPELINE");
        // Markers: procedural quad, alpha-blended (RGB), no hand-depth test or write.
        VkPipelineVertexInputStateCreateInfo none{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineDepthStencilStateCreateInfo off{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};off.depthCompareOp=VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState alpha=opaque;
        if(blend){alpha.blendEnable=VK_TRUE;alpha.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;alpha.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;alpha.colorBlendOp=VK_BLEND_OP_ADD;
            alpha.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;alpha.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;alpha.alphaBlendOp=VK_BLEND_OP_ADD;}
        VkPipelineColorBlendStateCreateInfo mb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};mb.attachmentCount=1;mb.pAttachments=&alpha;
        stages[0].module=b.markerVertex;stages[1].module=b.markerFragment;
        pi.pVertexInputState=&none;pi.pDepthStencilState=&off;pi.pColorBlendState=&mb;
        check(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&pi,nullptr,&s.markers),"MARKER_PIPELINE");
        // laser: procedural quad, alpha-blended (RGB), tested against the private depth (hands occlude it; COPY mode: world
        // depth too) without writing; SAMPLED mode also tests the world depth in the fragment shader like the markers.
        VkPipelineDepthStencilStateCreateInfo test{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        test.depthTestEnable=VK_TRUE;test.depthWriteEnable=VK_FALSE;test.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
        stages[0].module=b.laserVertex;stages[1].module=b.laserFragment;pi.pDepthStencilState=&test;
        check(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&pi,nullptr,&s.laser),"LASER_PIPELINE");
        // RELOAD: procedural cartridges (576 vertices, no vertex input), opaque, tested + written against the private depth (the VR
        // hands occlude them, they occlude each other). Own try: a failure only hides the cartridges (counted), the slot stays READY.
        if(b.cartridgeVertex&&b.cartridgeFragment){
            try{
                stages[0].module=b.cartridgeVertex;stages[1].module=b.cartridgeFragment;
                pi.pVertexInputState=&none;pi.pDepthStencilState=&ds;pi.pColorBlendState=&cb;
                check(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&pi,nullptr,&s.cartridge),"CARTRIDGE_PIPELINE");
            }catch(const VkFailure& f){s.cartridge={};logLine("cartridge pipeline failed",f.what);}
            pi.pDepthStencilState=&test;pi.pColorBlendState=&mb;
        }
        // ARC: ribbon (4 instance-rate bindings on the ring: prev, a, b, next) + end marker (procedural quad); alpha-blended RGB,
        // tested against the private depth without writing (VR hand meshes occlude; COPY: the copied world depth). Own try: a failure
        // leaves the slot READY for the hands (arcError ARC_PIPELINE, arcStatus FAILED).
        if(b.arcReady&&m.arcState.load(std::memory_order_acquire)!=int(ArcState::Killed)){
            try{
                if(m.arcFailPipelinesForTests.load(std::memory_order_relaxed))throw VkFailure{"ARC_PIPELINE"};
                std::array<VkVertexInputBindingDescription,4> ab{};
                for(std::uint32_t i=0;i<4;++i)ab[i]={i,std::uint32_t(sizeof(ArcVertex)),VK_VERTEX_INPUT_RATE_INSTANCE};
                const std::array<VkVertexInputAttributeDescription,8> aa{{
                    {0,0,VK_FORMAT_R32G32B32A32_SFLOAT,0},
                    {1,1,VK_FORMAT_R32G32B32A32_SFLOAT,0},{2,1,VK_FORMAT_R8G8B8A8_UNORM,16},{3,1,VK_FORMAT_R32G32_SFLOAT,20},
                    {4,2,VK_FORMAT_R32G32B32A32_SFLOAT,0},{5,2,VK_FORMAT_R8G8B8A8_UNORM,16},{6,2,VK_FORMAT_R32G32_SFLOAT,20},
                    {7,3,VK_FORMAT_R32G32B32A32_SFLOAT,0}}};
                VkPipelineVertexInputStateCreateInfo avi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
                avi.vertexBindingDescriptionCount=std::uint32_t(ab.size());avi.pVertexBindingDescriptions=ab.data();
                avi.vertexAttributeDescriptionCount=std::uint32_t(aa.size());avi.pVertexAttributeDescriptions=aa.data();
                stages[0].module=b.arcVertex;stages[1].module=b.arcFragment;pi.pVertexInputState=&avi;pi.pDepthStencilState=&test;pi.pColorBlendState=&mb;
                check(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&pi,nullptr,&s.arc),"ARC_PIPELINE");
                stages[0].module=b.arcMarkerVertex;stages[1].module=b.arcMarkerFragment;pi.pVertexInputState=&none;
                check(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&pi,nullptr,&s.arcMarker),"ARC_PIPELINE");
                s.arcStatus.store(2,std::memory_order_release);m.arcK.pipelinesBuilt.fetch_add(1,std::memory_order_relaxed);
                std::uint64_t none0=0;if(m.arcK.firstRequestNs.load(std::memory_order_relaxed))m.arcK.firstReadyNs.compare_exchange_strong(none0,steadyNs());
            }catch(const VkFailure& f){
                if(s.arc)vkDestroyPipeline(d,s.arc,nullptr);if(s.arcMarker)vkDestroyPipeline(d,s.arcMarker,nullptr);s.arc=s.arcMarker={};
                s.arcError.store(f.what,std::memory_order_relaxed);s.arcStatus.store(3,std::memory_order_release);
                m.arcError.store("ARC_PIPELINE",std::memory_order_relaxed);m.arcK.pipelinesFailed.fetch_add(1,std::memory_order_relaxed);
                logLine("arc pipeline failed",f.what);
            }
        }
    }catch(const VkFailure&){destroySlot(d,s);throw;}
    catch(...){destroySlot(d,s);throw VkFailure{"EXCEPTION"};}
    s.buildNs=steadyNs()-start;
}
bool anyRequested(Module& m) noexcept {for(auto& s:m.slots)if(s.status.load(std::memory_order_acquire)==1)return true;return false;}
void workerMain() noexcept {
    auto& m=module();
    m.workerStarted.store(true,std::memory_order_release);
    try{
        enterStage(m,Stage::WorkerEntered);
        buildBase(m);m.baseReady.store(true,std::memory_order_release);
        enterStage(m,Stage::Published);m.readyNs.store(steadyNs(),std::memory_order_relaxed);
        transition(State::Building,State::Ready,"OK"); // A BUILD_TIMEOUT that already fired stays FAILED
        if(m.base.arcReady){int expected=int(ArcState::Building);m.arcState.compare_exchange_strong(expected,int(ArcState::Ready),std::memory_order_acq_rel);}
    }
    catch(const VkFailure& f){m.stage.store(unsigned(Stage::Failed),std::memory_order_release);
        if(!transition(State::Building,State::Failed,f.what))m.lastError.store(f.what,std::memory_order_relaxed);return;}
    catch(...){m.stage.store(unsigned(Stage::Failed),std::memory_order_release);
        if(!transition(State::Building,State::Failed,"BASE_EXCEPTION"))m.lastError.store("BASE_EXCEPTION",std::memory_order_relaxed);return;}
    while(!m.stop.load(std::memory_order_acquire)){
        for(auto& s:m.slots){
            if(m.stop.load(std::memory_order_acquire))break;
            if(s.status.load(std::memory_order_acquire)!=1)continue;
            try{buildSlot(m,s);s.status.store(2,std::memory_order_release);}
            catch(const VkFailure& f){s.error.store(f.what,std::memory_order_relaxed);m.lastError.store(f.what,std::memory_order_relaxed);
                s.status.store(3,std::memory_order_release);logLine("hands pipeline failed",f.what);}
            catch(...){s.error.store("EXCEPTION",std::memory_order_relaxed);m.lastError.store("PIPELINE_EXCEPTION",std::memory_order_relaxed);
                s.status.store(3,std::memory_order_release);}
        }
        std::unique_lock lock(m.workMutex);
        m.workCv.wait_for(lock,std::chrono::milliseconds(50),[&]{return m.stop.load(std::memory_order_acquire)||anyRequested(m);});
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Render thread helpers.
VkFormatFeatureFlags formatFeatures(Module& m,VkFormat f) noexcept {
    for(auto& [format,features]:m.formatCache)if(format==f)return features;
    VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(m.physical,f,&p);
    for(auto& entry:m.formatCache)if(entry.first==VK_FORMAT_UNDEFINED){entry={f,p.optimalTilingFeatures};break;}
    return p.optimalTilingFeatures;
}
// Ready pipelines for the key, or nullptr (requested now / building / failed).
PipelineSlot* pipelines(Module& m,VkFormat color,VkFormat depth,bool load,bool& failed) noexcept {
    failed=false;
    for(auto& s:m.slots){
        const int st=s.status.load(std::memory_order_acquire);
        if(st==0||s.color!=color||s.depth!=depth||s.load!=load)continue;
        if(st==3)failed=true;
        if(st==1){ // A pipeline requested longer than the build timeout ago -> FAILED (the worker is stuck)
            const auto asked=s.requestNs.load(std::memory_order_relaxed),now=steadyNs();
            if(asked&&now>asked&&now-asked>timeoutNs(m)){
                m.timeoutStage.store(m.stage.load(std::memory_order_relaxed),std::memory_order_relaxed);
                transition(State::Ready,State::Failed,"PIPELINE_TIMEOUT");failed=true;}
        }
        return st==2?&s:nullptr;
    }
    for(auto& s:m.slots)if(s.status.load(std::memory_order_acquire)==0){
        s.color=color;s.depth=depth;s.load=load;s.error.store("",std::memory_order_relaxed);s.requestNs.store(steadyNs(),std::memory_order_relaxed);
        s.status.store(1,std::memory_order_release);m.workCv.notify_one();return nullptr;}
    failed=true;return nullptr; // more than pipelineSlots format keys: give up on the extra one
}
void bury(Module& m,const Dead& d) {m.dead.push_back(d);m.counters.graveyard.fetch_add(1,std::memory_order_relaxed);}
void destroyDead(VkDevice d,Module& m,const Dead& x) noexcept {
    if(x.framebuffer)vkDestroyFramebuffer(d,x.framebuffer,nullptr);
    if(x.set&&m.base.pool)vkFreeDescriptorSets(d,m.base.pool,1,&x.set);
    if(x.view)vkDestroyImageView(d,x.view,nullptr);
    if(x.image)vkDestroyImage(d,x.image,nullptr);
    if(x.memory)vkFreeMemory(d,x.memory,nullptr);
}
// Caller holds cacheMutex. Objects last used by a frame the GPU may still run are deferred to the graveyard.
void releaseDeferred(Module& m,std::uint64_t retired) noexcept {
    auto& v=m.dead;std::size_t keep=0;
    for(auto& x:v){if(x.frame<=retired){destroyDead(m.device,m,x);m.counters.graveyardReleased.fetch_add(1,std::memory_order_relaxed);}else v[keep++]=x;}
    v.resize(keep);
}
void dropFramebuffersOf(Module& m,VkImageView view,std::uint64_t retired) {
    auto& v=m.framebuffers;std::size_t keep=0;
    for(auto& f:v){
        if(f.color==view||f.depth==view){Dead x{};x.frame=f.lastUse;x.framebuffer=f.framebuffer;if(x.frame<=retired)destroyDead(m.device,m,x);else bury(m,x);}
        else v[keep++]=f;
    }
    v.resize(keep);
}
VkImageView colorView(Module& m,const EyeTarget& t,std::uint64_t frame) {
    for(auto& c:m.colors)if(c.image==t.image&&c.generation==t.generation&&c.format==t.format){c.lastUse=frame;return c.view;}
    if(m.colors.size()>=maxCacheEntries)return VK_NULL_HANDLE;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=t.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=t.format;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView view{};if(vkCreateImageView(m.device,&vi,nullptr,&view)!=VK_SUCCESS)return VK_NULL_HANDLE;
    m.colors.push_back({t.image,t.generation,t.format,view,frame});m.counters.views.fetch_add(1,std::memory_order_relaxed);return view;
}
// DEPTH-aspect view of the eye's world depth + its descriptor set {albedo, world depth in DEPTH_STENCIL_READ_ONLY_OPTIMAL}.
VkDescriptorSet depthSet(Module& m,const EyeTarget& t,std::uint64_t frame) {
    for(auto& e:m.depths)if(e.image==t.image&&e.generation==t.generation){e.lastUse=frame;return e.set;}
    if(m.depths.size()>=maxCacheEntries)return VK_NULL_HANDLE;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=t.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=t.format;
    vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    VkImageView view{};if(vkCreateImageView(m.device,&vi,nullptr,&view)!=VK_SUCCESS)return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=m.base.pool;da.descriptorSetCount=1;da.pSetLayouts=&m.base.setLayout;
    VkDescriptorSet set{};if(vkAllocateDescriptorSets(m.device,&da,&set)!=VK_SUCCESS){vkDestroyImageView(m.device,view,nullptr);return VK_NULL_HANDLE;}
    const VkDescriptorImageInfo infos[2]{{m.base.linear,m.base.albedoView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{m.base.nearest,view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for(unsigned i=0;i<2;++i){writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;
        writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&infos[i];}
    vkUpdateDescriptorSets(m.device,2,writes,0,nullptr);
    m.depths.push_back({t.image,t.generation,view,set,frame});m.counters.sets.fetch_add(1,std::memory_order_relaxed);return set;
}
// Private depth image (own transient D16/D32 for SAMPLED/NONE, or the world format for COPY) of the eye extent.
bool ownImage(Module& m,OwnImage& o,VkFormat format,VkExtent2D extent,bool copyTarget,std::uint64_t frame,std::uint64_t retired) {
    if(o.image&&o.format==format&&o.extent.width==extent.width&&o.extent.height==extent.height){o.lastUse=frame;return true;}
    if(o.image){ // replaced: framebuffers using the old view and the image itself retire with the last frame that used them
        dropFramebuffersOf(m,o.view,retired);
        Dead x{};x.frame=o.lastUse;x.view=o.view;x.image=o.image;x.memory=o.memory;if(x.frame<=retired)destroyDead(m.device,m,x);else bury(m,x);
        o={};
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={extent.width,extent.height,1};
    ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|(copyTarget?VK_IMAGE_USAGE_TRANSFER_DST_BIT:VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
    OwnImage n{};n.format=format;n.extent=extent;n.lastUse=frame;
    if(vkCreateImage(m.device,&ci,nullptr,&n.image)!=VK_SUCCESS)return false;
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(m.device,n.image,&req);
    auto type=copyTarget?UINT32_MAX:memoryType(m.physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT,0);
    if(type==UINT32_MAX)type=memoryType(m.physical,req.memoryTypeBits,0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=req.size;a.memoryTypeIndex=type;
    if(type==UINT32_MAX||vkAllocateMemory(m.device,&a,nullptr,&n.memory)!=VK_SUCCESS||vkBindImageMemory(m.device,n.image,n.memory,0)!=VK_SUCCESS){
        if(n.memory)vkFreeMemory(m.device,n.memory,nullptr);vkDestroyImage(m.device,n.image,nullptr);return false;}
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=n.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
    vi.subresourceRange={depthAspects(format),0,1,0,1};
    if(vkCreateImageView(m.device,&vi,nullptr,&n.view)!=VK_SUCCESS){vkFreeMemory(m.device,n.memory,nullptr);vkDestroyImage(m.device,n.image,nullptr);return false;}
    o=n;(copyTarget?m.counters.copyImages:m.counters.ownImages).fetch_add(1,std::memory_order_relaxed);return true;
}
VkFramebuffer framebuffer(Module& m,VkRenderPass pass,VkImageView color,VkImageView depth,VkImage colorImage,VkExtent2D extent,std::uint64_t frame) {
    for(auto& f:m.framebuffers)if(f.pass==pass&&f.color==color&&f.depth==depth){f.lastUse=frame;return f.framebuffer;}
    if(m.framebuffers.size()>=maxCacheEntries)return VK_NULL_HANDLE;
    const VkImageView views[2]{color,depth};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=pass;fi.attachmentCount=2;fi.pAttachments=views;
    fi.width=extent.width;fi.height=extent.height;fi.layers=1;
    VkFramebuffer fb{};if(vkCreateFramebuffer(m.device,&fi,nullptr,&fb)!=VK_SUCCESS)return VK_NULL_HANDLE;
    m.framebuffers.push_back({pass,color,depth,colorImage,fb,extent,frame});m.counters.framebuffers.fetch_add(1,std::memory_order_relaxed);return fb;
}

// Per render frame: the controller sample of the plan's XR frame, hand matrices and marker set (shared by both eyes).
// handsOn = menu HANDS (markers and hands); laserOn = menu LASER SIGHT (the laser alone may be drawn with HANDS OFF).
void computeFrame(Module& m,const EyeCall& call,bool handsOn,bool laserOn,bool markersOn) {
    auto& f=m.frame;const auto& plan=*call.plan;
    f=FrameHands{};f.renderFrame=call.renderFrame;f.computed=true;m.frameQuerySlot=~0u;
    // LASSO: holster markers with HANDS OFF (HOLSTERS ON, HOLSTER MARKERS != OFF):
    // no pose gate - the set's own reference space must be the plan's, and it must be fresh (published <= 100 ms ago); drawn in plan LOCAL.
    // HAMMER: weapon markers (Marker.flags & MarkerWeapon: the hammer ring) the same way, whatever HANDS /
    // BODY / HOLSTER MARKERS say (with HANDS ON they are taken here too and skipped by the pose-gated holster marker path below).
    {
        MarkerSet set{};const auto now=steadyNs();
        if(m.markers.load(set)&&set.count&&set.referenceSpace&&set.referenceSpace==plan.referenceSpace&&set.publishedNs&&
           (now<=set.publishedNs||now-set.publishedNs<=arcMaxAgeNs)){
            std::uint32_t weapons=0;
            for(std::uint32_t i=0;i<std::min(set.count,MarkerSet::capacity);++i){const auto& mk=set.markers[i];const bool weapon=(mk.flags&MarkerWeapon)!=0;
                if(!weapon&&(handsOn||!markersOn))continue;
                if(validMarker(mk)){f.markers.markers[f.markers.count++]=mk;weapons+=weapon?1u:0u;}}
            if(f.markers.count){f.any=true;f.markersUngated=true;}
            if(weapons)m.arcK.weaponMarkers.fetch_add(weapons,std::memory_order_relaxed);
        }
    }
    { // RELOAD: chamber mouths / rod tip in the GRIP frame through this plan's WeaponFrame
      // (rigid with the engine-drawn gun), the held round / ammo zones in LOCAL; whatever HANDS / BODY / LASER say, no pose gate, no world test.
        ReloadOverlay ro{};const auto now=steadyNs();
        if(m.reloadOverlay.load(ro)&&ro.count&&ro.referenceSpace&&ro.referenceSpace==plan.referenceSpace&&ro.ns&&(now<=ro.ns||now-ro.ns<=arcMaxAgeNs)){
            WeaponFrame w{};const bool haveW=plan.headSampledNs&&m.weapons.find(plan.headSampledNs,w)&&!(w.referenceSpace&&w.referenceSpace!=plan.referenceSpace);
            DMat4 gripToLocal=identity();
            if(haveW)gripToLocal=multiply(multiply(similarityInverse(localToWorld(plan.world[0],plan.eyes[0],plan.unitsPerMetre)),widen(w.map)),poseMatrix(w.grip,1.0));
            bool cylOk=false;float cylP[3]{},cylR{},cylLen{}; // The open cylinder occluder (LOCAL) for the chambered rounds after it
            for(std::uint32_t i=0;i<std::min<std::uint32_t>(ro.count,std::uint32_t(ro.items.size()))&&f.reloadCount<f.reload.size();++i){
                const auto& it=ro.items[i];Marker mk{};const bool grip=(it.frame&1u)==0;
                if(grip){if(!haveW)continue;const auto p=transform(gripToLocal,it.pos[0],it.pos[1],it.pos[2]);
                    mk.position[0]=float(p[0]);mk.position[1]=float(p[1]);mk.position[2]=float(p[2]);}
                else{mk.position[0]=it.pos[0];mk.position[1]=it.pos[1];mk.position[2]=it.pos[2];}
                mk.radius=it.radius;mk.highlight=it.highlight;mk.flags=(it.style&0xffu)<<8;
                const bool cart=it.style==ReloadStyleCartLive||it.style==ReloadStyleCartSpent;
                if(cart||it.style==ReloadStyleCylinder){ // The axis through the same map as the position
                    auto ax=unpackReloadAxis(it.pad);
                    if(grip){const auto q=transform(gripToLocal,it.pos[0]+ax[0],it.pos[1]+ax[1],it.pos[2]+ax[2]);
                        ax={float(q[0])-mk.position[0],float(q[1])-mk.position[1],float(q[2])-mk.position[2]};
                        const float l=std::sqrt(ax[0]*ax[0]+ax[1]*ax[1]+ax[2]*ax[2]);if(!(l>1e-4f)||!std::isfinite(l))continue;for(float& x:ax)x/=l;}
                    if(it.style==ReloadStyleCylinder){
                        cylOk=std::isfinite(mk.position[0])&&std::isfinite(mk.position[1])&&std::isfinite(mk.position[2])&&it.radius>0.f&&it.radius<.1f&&
                            it.highlight>0.f&&it.highlight<.2f;
                        if(cylOk){for(int k=0;k<3;++k)cylP[k]=mk.position[k];cylR=it.radius;cylLen=it.highlight;}
                        continue;}
                    if(!(it.highlight>0.f&&it.highlight<.2f))continue;
                    if(validMarker(mk)){auto& cd=f.reloadCart[f.reloadCount];cd=FrameHands::Cart{};
                        for(int k=0;k<3;++k)cd.axis[k]=ax[std::size_t(k)];
                        if(cylOk&&(it.frame&ReloadFrameInCylinder)){for(int k=0;k<3;++k)cd.cyl[k]=cylP[k]-mk.position[k];cd.cyl[3]=cylR;cd.cylLen=cylLen;}
                        f.reload[f.reloadCount++]=mk;}
                    continue;}
                if(validMarker(mk))f.reload[f.reloadCount++]=mk;
            }
            if(f.reloadCount){f.any=true;m.arcK.reloadItems.fetch_add(f.reloadCount,std::memory_order_relaxed);}
        }
    }
    HandSample s{};f.match=m.poses.find(plan.trackingFrame,s);
    m.counters.matches[unsigned(f.match)].fetch_add(1,std::memory_order_relaxed);
    if(f.match==PoseMatch::None){f.skip=SkipNoPose;return;}
    f.delta=std::int64_t(plan.trackingFrame)-std::int64_t(s.sequence);
    m.counters.lastDelta.store(f.delta,std::memory_order_relaxed);
    if(std::abs(f.delta)>std::abs(m.counters.maxDelta.load(std::memory_order_relaxed)))m.counters.maxDelta.store(f.delta,std::memory_order_relaxed);
    if((plan.trackingEpoch&&s.epoch!=plan.trackingEpoch)||(plan.referenceSpace&&s.referenceSpace!=plan.referenceSpace)){f.skip=SkipEpoch;return;}
    const auto now=steadyNs();
    if(!s.publishedNs||(now>s.publishedNs&&now-s.publishedNs>maxSampleAgeNs)){f.skip=SkipStale;return;}
    static const DVec3 light=[]{DVec3 l{-0.3,0.8,0.4};normalizeD(l);return l;}();
    const int held=m.heldHand.load(std::memory_order_relaxed);
    const unsigned heldMask=m.heldMask.load(std::memory_order_relaxed)|(held==0||held==1?1u<<unsigned(held):0u); // DUAL: phantom hand too
    if(handsOn)for(unsigned h=0;h<2;++h){
        f.visible[h]=handModel(s.aim[h],s.grip[h],h,f.model[h]);
        if(!f.visible[h])continue;
        f.weights[h]=poseWeights(s.grips[h],s.triggers[h],(heldMask>>h&1u)!=0);
        f.light[h]=modelLight(f.model[h],light);
        f.any=true;
    }
    // the VR-held weapon frame published for this plan's head sample (VrWeapon tags its pose-time placement).
    WeaponFrame w{};
    if(plan.headSampledNs&&m.weapons.find(plan.headSampledNs,w)){
        if(w.referenceSpace&&w.referenceSpace!=s.referenceSpace)m.counters.weaponSpace.fetch_add(1,std::memory_order_relaxed);
        else{
            f.weapon=true;m.counters.weaponFrames.fetch_add(1,std::memory_order_relaxed);
            const double scale=plan.unitsPerMetre;
            const DMat4 toLocal=similarityInverse(localToWorld(plan.world[0],plan.eyes[0],scale));
            const DMat4 K=multiply(toLocal,widen(w.map)); // the gun's LOCAL (its map) -> this plan's LOCAL
            { // residual of the gun's map vs the eyes': grip point displacement and rotation angle
                const auto g=w.grip.position;const auto q=transform(K,g[0],g[1],g[2]);
                const double cm=std::sqrt((q[0]-g[0])*(q[0]-g[0])+(q[1]-g[1])*(q[1]-g[1])+(q[2]-g[2])*(q[2]-g[2]))*100.0;
                const double tr=(K.m[0]+K.m[5]+K.m[10]-1.0)*0.5,deg=std::acos(std::clamp(tr,-1.0,1.0))*57.29577951308232;
                if(std::isfinite(cm)&&std::isfinite(deg)&&cm<1000.0){auto& k=m.counters;k.residualFrames.fetch_add(1,std::memory_order_relaxed);
                    const auto um=std::uint64_t(cm*1e4),md=std::uint64_t(deg*1e3);k.residualUmSum.fetch_add(um,std::memory_order_relaxed);
                    k.residualMdegSum.fetch_add(md,std::memory_order_relaxed);
                    auto x=k.residualUmMax.load(std::memory_order_relaxed);while(um>x&&!k.residualUmMax.compare_exchange_weak(x,um)){}
                    auto y=k.residualMdegMax.load(std::memory_order_relaxed);while(md>y&&!k.residualMdegMax.compare_exchange_weak(y,md)){}
                    // snap-turn bridged frames apart from the steady ones
                    const bool bridged=(w.flags&WeaponFrame::Bridged)!=0;if(bridged)k.residualBridgedFrames.fetch_add(1,std::memory_order_relaxed);
                    auto& um2=bridged?k.residualBridgedUmMax:k.residualSteadyUmMax;auto& md2=bridged?k.residualBridgedMdegMax:k.residualSteadyMdegMax;
                    auto x2=um2.load(std::memory_order_relaxed);while(um>x2&&!um2.compare_exchange_weak(x2,um)){}
                    auto y2=md2.load(std::memory_order_relaxed);while(md>y2&&!md2.compare_exchange_weak(y2,md)){}}
            }
            if(handsOn&&m.heldSync.load(std::memory_order_relaxed)&&(w.hand==0||w.hand==1)){ // held hand rigid with the gun
                const unsigned h=unsigned(w.hand);DMat4 model{};
                if(handModel(w.aim,w.grip,h,model)){
                    f.model[h]=multiply(K,model);f.visible[h]=true;f.weights[h]=poseWeights(s.grips[h],s.triggers[h],true);
                    f.light[h]=modelLight(f.model[h],light);f.any=f.heldSynced=true;m.counters.heldSynced.fetch_add(1,std::memory_order_relaxed);}
            }
            if(laserOn&&(w.flags&WeaponFrame::Laser)&&std::isfinite(w.laserMetres)&&w.laserMetres>0.f){
                const auto a=transform(toLocal,w.muzzle[0],w.muzzle[1],w.muzzle[2]);
                DVec3 d{w.direction[0],w.direction[1],w.direction[2]};
                DVec3 dl{toLocal.m[0]*d[0]+toLocal.m[4]*d[1]+toLocal.m[8]*d[2],toLocal.m[1]*d[0]+toLocal.m[5]*d[1]+toLocal.m[9]*d[2],
                         toLocal.m[2]*d[0]+toLocal.m[6]*d[1]+toLocal.m[10]*d[2]};
                if(normalizeD(dl)&&std::isfinite(a[0])&&std::isfinite(a[1])&&std::isfinite(a[2])){
                    const double len=std::min(double(w.laserMetres),500.0);
                    f.laserA={float(a[0]),float(a[1]),float(a[2])};
                    f.laserB={float(a[0]+dl[0]*len),float(a[1]+dl[1]*len),float(a[2]+dl[2]*len)};
                    f.laserHalfWidth=std::clamp(w.laserWidthMm,.5f,20.f)*.0005f;
                    f.laserDot=(w.flags&WeaponFrame::Hit)||!(w.flags&WeaponFrame::Probed);
                    f.laser=f.any=true;auto& k=m.counters;k.laserFrames.fetch_add(1,std::memory_order_relaxed);
                    if(w.flags&WeaponFrame::Hit)k.laserHits.fetch_add(1,std::memory_order_relaxed);
                    const auto mm=std::uint32_t(std::min(len,4e6/1e3)*1000.0);k.laserMmSum.fetch_add(mm,std::memory_order_relaxed);k.laserLastMm.store(mm,std::memory_order_relaxed);
                }
            }
        }
    }else if(held>=0)m.counters.weaponMissing.fetch_add(1,std::memory_order_relaxed);
    // DUAL: the phantom (second item) - its hand rigid with it, and its own laser when it is a firearm.
    WeaponFrame pw{};
    if(plan.headSampledNs&&m.phantoms.find(plan.headSampledNs,pw)&&!(pw.referenceSpace&&pw.referenceSpace!=s.referenceSpace)){
        f.phantomWeapon=true;m.counters.phantomFrames.fetch_add(1,std::memory_order_relaxed);
        const double scale=plan.unitsPerMetre;
        const DMat4 toLocal=similarityInverse(localToWorld(plan.world[0],plan.eyes[0],scale));
        if(handsOn&&m.heldSync.load(std::memory_order_relaxed)&&(pw.hand==0||pw.hand==1)){
            const unsigned h=unsigned(pw.hand);DMat4 model{};
            if(handModel(pw.aim,pw.grip,h,model)){
                f.model[h]=multiply(multiply(toLocal,widen(pw.map)),model);f.visible[h]=true;f.weights[h]=poseWeights(s.grips[h],s.triggers[h],true);
                f.light[h]=modelLight(f.model[h],light);f.any=true;m.counters.phantomSynced.fetch_add(1,std::memory_order_relaxed);}
        }
        if(laserOn&&(pw.flags&WeaponFrame::Laser)&&std::isfinite(pw.laserMetres)&&pw.laserMetres>0.f){
            const auto a=transform(toLocal,pw.muzzle[0],pw.muzzle[1],pw.muzzle[2]);
            DVec3 d{pw.direction[0],pw.direction[1],pw.direction[2]};
            DVec3 dl{toLocal.m[0]*d[0]+toLocal.m[4]*d[1]+toLocal.m[8]*d[2],toLocal.m[1]*d[0]+toLocal.m[5]*d[1]+toLocal.m[9]*d[2],
                     toLocal.m[2]*d[0]+toLocal.m[6]*d[1]+toLocal.m[10]*d[2]};
            if(normalizeD(dl)&&std::isfinite(a[0])&&std::isfinite(a[1])&&std::isfinite(a[2])){
                const double len=std::min(double(pw.laserMetres),500.0);
                f.laser2A={float(a[0]),float(a[1]),float(a[2])};
                f.laser2B={float(a[0]+dl[0]*len),float(a[1]+dl[1]*len),float(a[2]+dl[2]*len)};
                f.laser2HalfWidth=std::clamp(pw.laserWidthMm,.5f,20.f)*.0005f;
                f.laser2Dot=(pw.flags&WeaponFrame::Hit)||!(pw.flags&WeaponFrame::Probed);
                f.laser2=f.any=true;m.counters.phantomLaserFrames.fetch_add(1,std::memory_order_relaxed);
            }
        }
    }
    MarkerSet markers{};
    if(handsOn&&m.markers.load(markers)&&markers.count&&markers.referenceSpace==s.referenceSpace){
        for(std::uint32_t i=0;i<std::min(markers.count,MarkerSet::capacity)&&f.markers.count<MarkerSet::capacity;++i)
            if(!(markers.markers[i].flags&MarkerWeapon)&&validMarker(markers.markers[i]))f.markers.markers[f.markers.count++]=markers.markers[i]; // Weapon markers above
        if(f.markers.count)f.any=true;
    }
    if(!f.any)f.skip=SkipHidden;
}
// Timestamps (GPU estimate): one timed render frame in 16, ring of 4 slots of 4 queries (eye x begin/end).
void readTimestamps(Module& m) noexcept {
    const auto& b=m.base;if(!b.queries)return;
    for(unsigned slot=0;slot<4;++slot){
        if(!m.querySlotPending[slot])continue;
        struct Value {std::uint64_t ticks,available;};std::array<Value,4> v{};
        const auto r=vkGetQueryPoolResults(m.device,b.queries,slot*4,4,sizeof(v),v.data(),sizeof(Value),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if(r!=VK_SUCCESS&&r!=VK_NOT_READY)continue;
        double ns=0;bool any=false,waiting=false;std::array<double,2> eyeNs{-1.0,-1.0};
        for(unsigned e=0;e<2;++e){
            if(v[2*e].available&&v[2*e+1].available){eyeNs[e]=double((v[2*e+1].ticks-v[2*e].ticks)&b.timestampMask)*b.timestampPeriod;ns+=eyeNs[e];any=true;}
            else if(v[2*e].available||v[2*e+1].available)waiting=true;
        }
        if(waiting||(!any&&m.frame.renderFrame<m.querySlotFrame[slot]+64))continue; // not finished yet (or never submitted: age out)
        m.querySlotPending[slot]=false;
        for(unsigned e=0;e<2;++e){ // ARC: gpu.split - per eye, bucketed by what the eye drew (no extra queries)
            const unsigned bk=m.querySlotBucket[slot][e];if(!(eyeNs[e]>=0.0)||eyeNs[e]>1e8||bk>=splitBuckets)continue;
            const auto val=std::uint64_t(eyeNs[e]);auto& fr=m.arcK.splitFrames[bk];auto& av=m.arcK.splitAvgNs[bk];
            const auto old=av.load(std::memory_order_relaxed);av.store(fr.load(std::memory_order_relaxed)?(old*7+val)/8:val,std::memory_order_relaxed);
            fr.fetch_add(1,std::memory_order_relaxed);}
        if(!any||!(ns>=0)||ns>1e8){m.counters.gpuUnavailable.fetch_add(1,std::memory_order_relaxed);continue;}
        const auto value=std::uint64_t(ns);m.counters.gpuLastNs.store(value,std::memory_order_relaxed);m.counters.timed.fetch_add(1,std::memory_order_relaxed);
        const auto avg=m.counters.gpuAvgNs.load(std::memory_order_relaxed);
        m.counters.gpuAvgNs.store(avg?(avg*7+value)/8:value,std::memory_order_relaxed);
    }
}
// ---------------------------------------------------------------------------------------------------------------------
// ARC: render-thread helpers.
thread_local char arcThreadToken; // the publishing thread's identity (one per thread; ArcBoard writer binding)
std::uint64_t arcToken() noexcept {return reinterpret_cast<std::uint64_t>(&arcThreadToken);}
bool arcsLive(const Module& m,std::uint64_t now) noexcept {for(const auto& b:m.arcBoards)if(b.live(now))return true;return false;}
// Overlay-source table: arcs (when drawable) + the other sources (HANDS-OFF holster markers, reload overlay,
// props): each keeps the pass alive only while it has VISIBLE content stamped within arcMaxAgeNs.
bool overlayLive(const Module& m,std::uint64_t now,bool markersOn) noexcept {
    if(m.arcState.load(std::memory_order_relaxed)==int(ArcState::Ready)&&arcsLive(m,now))return true;
    for(unsigned s=1;s<unsigned(Overlay::Count);++s){
        if(s==unsigned(Overlay::MarkersHandsOff)&&!markersOn)continue; // LASSO: HOLSTERS / HOLSTER MARKERS OFF -> the marker set keeps nothing alive
        const auto v=m.overlayVisibleNs[s].load(std::memory_order_relaxed);
        if(v&&(now<=v||now-v<=arcMaxAgeNs))return true;}
    return false;
}
// Live arc levers (render thread, eye 0, every 64th call; host tests override them).
void liveArcLevers(Module& m) noexcept {
    if(m.arcDebugOverride||(m.leverCalls++%64))return;
    m.arcXray.store(propertyInt("debug.rdrvr.vr.arc.xray",1)?1:0,std::memory_order_relaxed);
    m.arcWidthPct.store(std::clamp(propertyInt("debug.rdrvr.vr.arc.width",100),25,400),std::memory_order_relaxed);
    m.arcMinPx.store(std::clamp(propertyInt("debug.rdrvr.vr.arc.minpx",18),10,40),std::memory_order_relaxed);
    m.arcDepthLever.store(std::clamp(propertyInt("debug.rdrvr.vr.arc.depth",1),0,2),std::memory_order_relaxed);
    m.arcAsw.store(propertyInt("debug.rdrvr.vr.arc.asw",1)?1:0,std::memory_order_relaxed);
    m.arcRects.store(std::clamp(propertyInt("debug.rdrvr.vr.arc.rects",1),1,4),std::memory_order_relaxed);
}
// A slot request for the key; counts it (prewarm) when it is new.
void requestSlot(Module& m,VkFormat color,VkFormat depth,bool load) noexcept {
    bool exists=false;
    for(auto& s:m.slots){if(s.status.load(std::memory_order_acquire)&&s.color==color&&s.depth==depth&&s.load==load){exists=true;break;}}
    bool failed=false;pipelines(m,color,depth,load,failed);
    if(!exists){m.arcK.prewarmRequests.fetch_add(1,std::memory_order_relaxed);
        std::uint64_t none=0;m.arcK.firstRequestNs.compare_exchange_strong(none,steadyNs(),std::memory_order_relaxed);}
}
// Prewarm: while a channel is armed and the pass is otherwise idle, request the arc pipelines of the current eye format
// (the SAMPLED / NONE key; also the COPY key with debug.rdrvr.vr.hands.depth 2). A request only - nothing is recorded.
void prewarm(Module& m,const EyeCall& call) noexcept {
    if(!m.arcArmed.load(std::memory_order_relaxed))return;
    m.arcK.prewarmCalls.fetch_add(1,std::memory_order_relaxed);
    if(m.arcState.load(std::memory_order_acquire)!=int(ArcState::Ready)||!m.baseReady.load(std::memory_order_acquire)||
       m.state.load(std::memory_order_acquire)!=int(State::Ready)||m.suppressed.load(std::memory_order_relaxed))return;
    if(!call.device||call.device!=m.device||call.eye>1)return;
    const auto& color=call.color;
    if(!color.image||color.samples!=VK_SAMPLE_COUNT_1_BIT||!(color.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)||encodeFor(color.format)<0)return;
    m.arcLastColor.store(int(color.format),std::memory_order_relaxed);
    try{
        requestSlot(m,color.format,m.base.ownDepth,false);
        const auto& depth=call.depth;
        if(m.debugDepth.load(std::memory_order_relaxed)==2&&depth.image&&depthFormat(depth.format)&&(depth.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)&&!m.copyUnavailable)
            requestSlot(m,color.format,depth.format,true);
    }catch(...){}
}
// Once per render frame (the first eye past the device / plan / colour checks): every channel's frame for this plan, WORLD -> the plan's
// LOCAL, the padded runs + pillars into the CPU copy, then into ring slot renderFrame % 8 if that slot retired (else ring.busy: no arcs).
void computeArcs(Module& m,const EyeCall& call,std::uint64_t now) noexcept {
    auto& d=m.arcDraw;
    d.renderFrame=call.renderFrame;d.any=d.ringOk=false;d.allNoWorldTest=true;for(auto& it:d.items)it=ArcDraw::Item{};
    if(m.arcState.load(std::memory_order_acquire)!=int(ArcState::Ready)||!m.base.arcReady||!m.base.arcRingMapped)return;
    m.arcK.computed.fetch_add(1,std::memory_order_relaxed);
    const auto& plan=*call.plan;const double scale=plan.unitsPerMetre;
    const DMat4 toLocalWorld=similarityInverse(localToWorld(plan.world[0],plan.eyes[0],scale));
    DVec3 upWorld=transformDirection(toLocalWorld,{0,1,0});if(!normalizeD(upWorld))upWorld={0,1,0}; // world +Y in this plan's LOCAL
    bool synth=false;if(const auto probe=m.synthesizing.load(std::memory_order_acquire)){try{synth=probe();}catch(...){synth=false;}}
    if(synth)m.arcK.synthesizingFrames.fetch_add(1,std::memory_order_relaxed);
    const bool solidAsw=synth&&m.arcAsw.load(std::memory_order_relaxed)!=0;
    const float widthScale=float(std::clamp(m.arcWidthPct.load(std::memory_order_relaxed),25,400))/100.f;
    const auto dim=[](std::uint32_t rgba){const float a=float(rgba>>24)*.8f;return (rgba&0x00ffffffu)|(std::uint32_t(a+.5f)<<24);};
    for(unsigned c=0;c<arcChannels;++c){
        auto& k=m.arcCh[c];ArcFrame fr{};
        const auto pick=m.arcBoards[c].select(plan.headSampledNs,plan.referenceSpace,now,fr);
        k.frames.fetch_add(1,std::memory_order_relaxed);k.picks[pick].fetch_add(1,std::memory_order_relaxed);
        if(m.arcBoards[c].live(now)){k.liveFrames.fetch_add(1,std::memory_order_relaxed);if(pick==ArcBoard::Exact)k.liveExact.fetch_add(1,std::memory_order_relaxed);}
        if(pick!=ArcBoard::Exact&&pick!=ArcBoard::Older)continue;
        if(!fr.count){k.hidden.fetch_add(1,std::memory_order_relaxed);continue;}
        const bool local=fr.space==std::uint32_t(ArcSpace::Local);
        const DMat4 toLocal=local?identity():toLocalWorld;
        auto& it=d.items[c];const std::uint32_t first=c*arcRunEntries;float len=0.f;
        const unsigned n=buildArcVertices(fr,toLocal,&d.verts[first],arcMaxPoints+2,len);
        if(n<4)continue;
        for(unsigned i=0;i<n;++i){auto& v=d.verts[first+i];v.halfWidthM=std::clamp(v.halfWidthM*widthScale,.0002f,.5f);if(solidAsw)v.rgba=dim(v.rgba);}
        it.live=true;it.firstEntry=first;it.entries=n;it.lengthM=len;it.style=fr.style;it.flags=fr.flags;
        if(solidAsw)it.style.dashPeriodM=0.f; // AppSW: solid, slightly dimmer (less visible swim)
        if(buildPillarVertices(fr,toLocal,local?DVec3{0,1,0}:upWorld,&d.verts[first+n],arcRunEntries-n)==4){
            it.pillar=true;it.pillarFirst=first+n;if(solidAsw)for(unsigned i=0;i<4;++i)d.verts[first+n+i].rgba=dim(d.verts[first+n+i].rgba);}
        const auto& mk=fr.marker;
        if(mk.shape!=ArcMarkerShape::None){
            it.centre=transformPoint(toLocal,{mk.centre[0],mk.centre[1],mk.centre[2]});
            DVec3 nrm=transformDirection(toLocal,{mk.normal[0],mk.normal[1],mk.normal[2]});it.faceCamera=!normalizeD(nrm);it.normal=nrm;
            DVec3 fc=transformDirection(toLocal,{mk.facing[0],mk.facing[1],mk.facing[2]});if(!normalizeD(fc))fc={0,0,0};it.facing=fc;
            it.shape=mk.shape;it.rgba=solidAsw?dim(mk.rgba):mk.rgba;it.radiusM=mk.radiusM;it.liftM=mk.liftM;it.depthTolM=mk.depthTolM;it.xrayAlpha=mk.xrayAlpha;
            it.marker=std::isfinite(it.centre[0])&&std::isfinite(it.centre[1])&&std::isfinite(it.centre[2]);
        }
        k.lastPoints.store(n-2,std::memory_order_relaxed);k.lastLengthMm.store(std::uint32_t(std::min(len,4e6f/1e3f)*1000.f),std::memory_order_relaxed);
        const std::uint64_t lag=now>fr.publishedNs?(now-fr.publishedNs)/1000u:0u;k.lastLagUs.store(lag,std::memory_order_relaxed);
        auto mx=k.maxLagUs.load(std::memory_order_relaxed);while(lag>mx&&!k.maxLagUs.compare_exchange_weak(mx,lag)){}
        d.any=true;if(!(fr.style.flags&ArcStyle::NoWorldTest))d.allNoWorldTest=false;
    }
    if(!d.any)return;
    const unsigned slot=unsigned(call.renderFrame%arcRingSlots);
    if(m.ringSlotFrame[slot]>call.renderFrame)m.ringSlotFrame[slot]=0; // render frame ids restarted: the old use is gone
    if(m.ringSlotFrame[slot]&&m.ringSlotFrame[slot]!=call.renderFrame&&m.ringSlotFrame[slot]>call.retiredFrame){
        m.arcK.ringBusy.fetch_add(1,std::memory_order_relaxed);d.any=false;return;}
    std::memcpy(m.base.arcRingMapped+std::size_t(slot)*arcRingSlotBytes,d.verts.data(),arcRingArcBytes); // host-coherent: visible at submit
    m.ringSlotFrame[slot]=call.renderFrame;d.slot=slot;d.ringOk=true;
    m.arcK.ringWrites.fetch_add(1,std::memory_order_relaxed);m.arcK.ringBytes.fetch_add(arcRingArcBytes,std::memory_order_relaxed);
}
// Per eye: a marker's draw data (lift to >= 4 depth steps at its distance, >= 6 px radius, window-depth tolerance, heading).
struct ArcEyeMarker {bool draw{};float centre[4]{},normal[4]{},color[4]{},params[4]{};Vec3 lo{},hi{};};
unsigned splitBucket(unsigned contents,DepthMode mode,bool taa) noexcept {return (contents*unsigned(DepthMode::Count)+unsigned(mode))*2u+(taa?1u:0u);}
bool hookEntry(const EyeCall& call) noexcept {return recordEye(call);}
} // namespace

// =====================================================================================================================
const char* stateName(State s) noexcept {
    switch(s){case State::Off:return "OFF";case State::Killed:return "KILLED";case State::Idle:return "IDLE";case State::Building:return "BUILDING";
    case State::Ready:return "READY";case State::Failed:return "FAILED";default:return "UNKNOWN";}
}
State state() noexcept {return State(module().state.load(std::memory_order_acquire));}
void pushPose(const runtime::ControllerSample& sample) noexcept {
    Errno preserve;auto& m=module();
    if(m.state.load(std::memory_order_relaxed)==int(State::Killed))return;
    m.poses.push(handSample(sample));
}
void setHeldHand(int hand) noexcept {
    auto& m=module();m.heldHand.store(hand==0||hand==1?hand:-1,std::memory_order_relaxed);
    m.heldMask.store(hand==0||hand==1?1u<<unsigned(hand):0u,std::memory_order_relaxed);}
void setHeldMask(unsigned mask) noexcept {module().heldMask.store(mask&3u,std::memory_order_relaxed);} // DUAL: after setHeldHand
void publishMarkers(std::span<const Marker> markers,std::uint64_t referenceSpace,std::uint64_t publishedNs) noexcept {
    MarkerSet s{};s.referenceSpace=referenceSpace;s.publishedNs=publishedNs;
    for(const auto& marker:markers){if(s.count>=MarkerSet::capacity)break;s.markers[s.count++]=marker;}
    auto& m=module();m.markers.store(s);
    // LASSO: the overlay source "HANDS-OFF holster markers" lives only while a set has visible content (render clock)
    m.overlayVisibleNs[unsigned(Overlay::MarkersHandsOff)].store(s.count?steadyNs():0,std::memory_order_relaxed);
    // HAMMER: a set with a weapon marker keeps the pass alive even with HOLSTER MARKERS OFF
    bool weapon=false;for(std::uint32_t i=0;i<s.count;++i)weapon=weapon||(s.markers[i].flags&MarkerWeapon)!=0;
    m.overlayVisibleNs[unsigned(Overlay::WeaponMarkers)].store(weapon?steadyNs():0,std::memory_order_relaxed);
}
void setSuppressed(bool value) noexcept {module().suppressed.store(value,std::memory_order_relaxed);}
void publishWeapon(const WeaponFrame& f) noexcept {Errno preserve;module().weapons.publish(f);}
void publishPhantomWeapon(const WeaponFrame& f) noexcept {Errno preserve;module().phantoms.publish(f);}
void setEngineRestore(EngineRestore r) noexcept {
    auto& m=module();m.restoreReady.store(r.ready,std::memory_order_release);m.restore.store(r.restore,std::memory_order_release);
}
void setBuildTimeoutForTests(unsigned milliseconds,int stallStage,unsigned stallMilliseconds) noexcept {
    auto& m=module();m.buildTimeoutMs.store(milliseconds?milliseconds:defaultBuildTimeoutMs,std::memory_order_relaxed);
    m.testDelayStage.store(stallStage>=0&&stallStage<int(Stage::Count)?unsigned(stallStage):unsigned(Stage::Count),std::memory_order_relaxed);
    m.testDelayMs.store(stallMilliseconds,std::memory_order_relaxed);
}
int stageForTests(const char* name) noexcept {
    for(unsigned i=0;i<unsigned(Stage::Count);++i)if(name&&!std::strcmp(stageNames[i],name))return int(i);
    return -1;
}
void setDebugForTests(int depth,int brightPercent,int gpu) noexcept {
    auto& m=module();m.debugDepth.store(depth,std::memory_order_relaxed);m.brightPercent.store(std::clamp(brightPercent,10,300),std::memory_order_relaxed);
    m.gpuTiming.store(gpu,std::memory_order_relaxed);m.debugOverride=true;
}
void configure(VkPhysicalDevice physical,VkDevice device,std::uint32_t family,std::uint32_t queueIndex,std::mutex* queueMutex) noexcept {
    Errno preserve;auto& m=module();
    if(!m.configured.load(std::memory_order_acquire)){
        if(!physical||!device)return;
        char kill[96]{};
        if(killed(Feature::Hands)||(propertyText("debug.rdrvr.vr.hands",kill,sizeof kill)&&kill[0]=='0')){
            setState(State::Killed,"PROPERTY");m.configured.store(true,std::memory_order_release);
            m.arcState.store(int(ArcState::Killed),std::memory_order_release);m.arcError.store("HANDS_KILLED",std::memory_order_relaxed);
            status::publish(Feature::Hands,status::Unavailable);return;
        }
        { // ARC: Feature::Arc / debug.rdrvr.vr.arc 0 (startup) -> no arc shaders / ring / pipelines; hands / laser / markers unchanged
            char arcKill[96]{};
            const bool arcKilled=killed(Feature::Arc)||m.arcKillForTests.load(std::memory_order_relaxed)||
                (propertyText("debug.rdrvr.vr.arc",arcKill,sizeof arcKill)&&arcKill[0]=='0');
            m.arcState.store(int(arcKilled?ArcState::Killed:ArcState::Building),std::memory_order_release);
            m.arcError.store(arcKilled?"KILLED":"",std::memory_order_relaxed);m.leverCalls=0;
        }
        if(!m.debugOverride){
            char depth[96]{};
            m.debugDepth.store(propertyText("debug.rdrvr.vr.hands.depth",depth,sizeof depth)&&depth[0]>='0'&&depth[0]<='2'?depth[0]-'0':-1,std::memory_order_relaxed);
            m.brightPercent.store(std::clamp(propertyInt("debug.rdrvr.vr.hands.bright",100),10,300),std::memory_order_relaxed);
            m.gpuTiming.store(propertyInt("debug.rdrvr.vr.hands.gpu",1)?1:0,std::memory_order_relaxed);
            m.heldSync.store(propertyInt("debug.rdrvr.vr.hands.heldsync",1)?1:0,std::memory_order_relaxed);
        }
        m.physical=physical;m.device=device;m.family=family;m.queueIndex=queueIndex;
        m.queueMutex=queueMutex; // Kept for the API/report only; the module never takes it (no queue submit)
        m.stop.store(false,std::memory_order_release);
        // build observability reset + start time of the BUILDING timeout
        for(auto& t:m.stageNs)t.store(0,std::memory_order_relaxed);
        m.readyNs.store(0,std::memory_order_relaxed);m.failedNs.store(0,std::memory_order_relaxed);m.lastError.store("",std::memory_order_relaxed);
        m.workerStarted.store(false,std::memory_order_relaxed);m.timeoutStage.store(unsigned(Stage::NotStarted),std::memory_order_relaxed);
        m.uploadPhase=UploadPending;m.uploadFirst=m.uploadLast=0;m.uploadCommand={};m.counters.uploadPhase.store(UploadPending,std::memory_order_relaxed);
        m.counters.stagingFreed.store(false,std::memory_order_relaxed);
        for(auto* c:{&m.counters.uploads,&m.counters.uploadFirstFrame,&m.counters.uploadLastFrame,&m.counters.uploadDoneFrame})c->store(0,std::memory_order_relaxed);
        m.configureNs.store(steadyNs(),std::memory_order_relaxed);
        m.stageNs[unsigned(Stage::ThreadStart)].store(steadyNs(),std::memory_order_relaxed);m.stage.store(unsigned(Stage::ThreadStart),std::memory_order_release);
        setState(State::Building,"BUILDING");
        try{m.worker=std::thread(workerMain);}
        catch(...){setState(State::Failed,"THREAD");m.configured.store(true,std::memory_order_release);status::publish(Feature::Hands,status::Failed);return;}
        eyeHook.store(&hookEntry,std::memory_order_release);
        m.configured.store(true,std::memory_order_release);
    }
    checkBuildTimeout(m,steadyNs()); // BUILDING > 10 s -> FAILED BUILD_TIMEOUT
    { // ARC: the Feature::Arc status slot (no menu row; vr.menu.status.arc) - also keeps arcStatusAgeMs current
        const auto as=arcStatus();
        status::publish(Feature::Arc,as==ArcStatus::Killed?(killed(Feature::Arc)?status::Property:status::Unavailable):
            as==ArcStatus::Failed?status::Failed:status::Ready,arcStatusNote());
    }
    // Menu row suffix (every frame, two relaxed stores).
    const auto st=State(m.state.load(std::memory_order_acquire));
    if(st==State::Killed)status::publish(Feature::Hands,status::Unavailable);
    else if(st==State::Failed)status::publish(Feature::Hands,status::Failed);
    else if(st==State::Building)status::publish(Feature::Hands,status::Ready,"BUILDING");
    else if(!settings().hands())status::publish(Feature::Hands,status::Ready);
    else if(status::bodyHidesHands.load(std::memory_order_relaxed))status::publish(Feature::Hands,status::Ready,"BODY");
    else{
        const auto mode=DepthMode(m.counters.lastMode[0].load(std::memory_order_relaxed));
        status::publish(Feature::Hands,status::Ready,mode==DepthMode::Sampled?"ACTIVE":mode==DepthMode::Copy?"COPY DEPTH":"NO DEPTH");
    }
}
void imageDestroyed(VkDevice device,VkImage image) noexcept {
    Errno preserve;auto& m=module();
    if(!image||!m.configured.load(std::memory_order_acquire)||device!=m.device)return;
    try{
        std::lock_guard lock(m.cacheMutex);
        const auto retired=m.retired.load(std::memory_order_acquire);
        for(std::size_t i=0;i<m.colors.size();){
            auto& c=m.colors[i];if(c.image!=image){++i;continue;}
            dropFramebuffersOf(m,c.view,retired);
            Dead x{};x.frame=c.lastUse;x.view=c.view;if(x.frame<=retired)destroyDead(m.device,m,x);else bury(m,x);
            m.colors.erase(m.colors.begin()+std::ptrdiff_t(i));m.counters.imageDrops.fetch_add(1,std::memory_order_relaxed);
        }
        for(std::size_t i=0;i<m.depths.size();){
            auto& e=m.depths[i];if(e.image!=image){++i;continue;}
            Dead x{};x.frame=e.lastUse;x.view=e.view;x.set=e.set;if(x.frame<=retired)destroyDead(m.device,m,x);else bury(m,x);
            m.depths.erase(m.depths.begin()+std::ptrdiff_t(i));m.counters.imageDrops.fetch_add(1,std::memory_order_relaxed);
        }
        releaseDeferred(m,retired); // Also drains while recordEye skips (HANDS OFF / no pose / not ready)
    }catch(...){}
}
void shutdownAfterIdle(VkDevice device) noexcept {
    Errno preserve;auto& m=module();
    if(!m.configured.load(std::memory_order_acquire)||device!=m.device)return;
    eyeHook.store(nullptr,std::memory_order_release);
    m.stop.store(true,std::memory_order_release);m.workCv.notify_all();
    try{if(m.worker.joinable())m.worker.join();}catch(...){}
    try{
        std::lock_guard lock(m.cacheMutex);const VkDevice d=m.device;
        for(auto& f:m.framebuffers)vkDestroyFramebuffer(d,f.framebuffer,nullptr);
        for(auto& c:m.colors)vkDestroyImageView(d,c.view,nullptr);
        for(auto& e:m.depths){vkDestroyImageView(d,e.view,nullptr);} // sets go with the pool
        for(auto& x:m.dead){Dead y=x;y.set={};destroyDead(d,m,y);}
        for(auto* o:{&m.own,&m.copy})if(o->image){vkDestroyImageView(d,o->view,nullptr);vkDestroyImage(d,o->image,nullptr);vkFreeMemory(d,o->memory,nullptr);*o={};}
        m.framebuffers.clear();m.colors.clear();m.depths.clear();m.dead.clear();m.copyUnavailable=false;
        for(auto& s:m.slots){destroySlot(d,s);s.status.store(0,std::memory_order_release);s.color=s.depth={};s.load=false;}
        destroyBase(d,m.base);m.baseReady.store(false,std::memory_order_release);
        m.uploadPhase=UploadPending;m.uploadFirst=m.uploadLast=0;m.uploadCommand={};
        m.formatCache={};m.frame={};m.querySlotPending={};m.querySlotFrame={};
        m.arcDraw.renderFrame=~0ull;m.arcDraw.any=m.arcDraw.ringOk=false;m.ringSlotFrame={};m.querySlotBucket={};
    }catch(...){}
    m.physical={};m.device={};m.queueMutex=nullptr;
    m.stage.store(unsigned(Stage::NotStarted),std::memory_order_relaxed);m.configureNs.store(0,std::memory_order_relaxed);
    m.arcState.store(int(ArcState::Off),std::memory_order_release);
    m.arcLastColor.store(int(VK_FORMAT_UNDEFINED),std::memory_order_relaxed); // The slots are gone; the next pass / prewarm sets it
    setState(State::Off,"SHUTDOWN");m.configured.store(false,std::memory_order_release);
}
bool waitIdleForTests(unsigned milliseconds) noexcept {
    auto& m=module();const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(std::chrono::steady_clock::now()<end){
        const auto st=State(m.state.load(std::memory_order_acquire));
        if(st==State::Failed)return true;
        if(m.baseReady.load(std::memory_order_acquire)&&!anyRequested(m))return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
bool recordEye(const EyeCall& call) noexcept {
    Errno preserve;auto& m=module();auto& k=m.counters;
    k.calls.fetch_add(1,std::memory_order_relaxed);
    // The GPU-retired watermark and the graveyard drain do not depend on the hands being drawn. Every call from
    // this device advances m.retired (monotonic), and an early skip on eye 0 drains the graveyard if the cache is free
    // (try_lock: never waits on the render thread). Otherwise views of destroyed engine images would stay buried
    // (and outlive their image's driver destroy) for as long as hands are skipped.
    bool cacheLocked=false;
    const auto drainIdle=[&]{
        if(cacheLocked||call.eye!=0||!call.device||call.device!=m.device||!m.configured.load(std::memory_order_acquire))return;
        std::unique_lock idle(m.cacheMutex,std::try_to_lock);if(idle.owns_lock()&&!m.dead.empty())releaseDeferred(m,call.retiredFrame);
    };
    const auto skip=[&](Skip why){k.skips[why].fetch_add(1,std::memory_order_relaxed);try{drainIdle();}catch(...){}return false;};
    try{
        if(call.device&&call.device==m.device){
            auto seen=m.retired.load(std::memory_order_relaxed);
            while(call.retiredFrame>seen&&!m.retired.compare_exchange_weak(seen,call.retiredFrame,std::memory_order_release,std::memory_order_relaxed)){}
        }
        // LASER SIGHT may draw without HANDS. BODY: the character's own arms replace the hand meshes
        // while the BODY module owns them.
        const auto menu=settings();const bool handsOn=menu.hands()&&!status::bodyHidesHands.load(std::memory_order_relaxed),laserOn=menu.laser()&&enabled(Feature::Laser);
        // LASSO: holster markers are drawn with HANDS OFF too when HOLSTERS ON and HOLSTER MARKERS != OFF
        const bool markersOn=menu.holsters()&&menu.holsterMarkers()!=Snapshot::MarkersOff&&enabled(Feature::Holsters);
        const auto nowNs=steadyNs();
        if(call.eye==0)liveArcLevers(m);
        // TELEPORT: every pair, BEFORE the pass's own gates - the first renderFrameId drawn from a head sample at
        // or after the warp key K ends the black hold once the XR thread submitted it (fade::xrPairId).
        if(call.eye==0&&call.plan)::rdrvr::vr::tploco::fade::notePair(call.plan->headSampledNs,call.renderFrame?call.renderFrame:call.plan->sceneId,nowNs);
        // ARC: the overlay-source table keeps the pass alive while a source has VISIBLE content; idle + armed -> prewarm pipelines
        if(!handsOn&&!laserOn&&!overlayLive(m,nowNs,markersOn)){prewarm(m,call);return skip(SkipDisabled);}
        if(m.suppressed.load(std::memory_order_relaxed))return skip(SkipSuppressed);
        if(!m.baseReady.load(std::memory_order_acquire)||m.state.load(std::memory_order_acquire)!=int(State::Ready)){
            checkBuildTimeout(m,steadyNs());return skip(SkipBuilding);}
        if(!call.device||call.device!=m.device||call.family!=m.family||!call.command||call.eye>1)return skip(SkipDevice);
        if(!call.plan||!call.plan->trackingFrame||!(call.plan->unitsPerMetre>0.f)||!std::isfinite(call.plan->unitsPerMetre))return skip(SkipPlan);
        const auto& color=call.color;
        if(!color.image||!color.extent.width||!color.extent.height||color.samples!=VK_SAMPLE_COUNT_1_BIT||!color.layers||
           !(color.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)||!colorLayoutOk(color.layout)||encodeFor(color.format)<0)return skip(SkipColor);
        const auto ready=m.restoreReady.load(std::memory_order_acquire);const auto restore=m.restore.load(std::memory_order_acquire);
        if(!ready||!restore||!ready(call.command))return skip(SkipRestore);
        m.arcLastColor.store(int(color.format),std::memory_order_relaxed); // arcStatus looks up this format's slot
        if(!m.frame.computed||m.frame.renderFrame!=call.renderFrame){
            computeFrame(m,call,handsOn,laserOn,markersOn);k.frames.fetch_add(1,std::memory_order_relaxed);
            computeArcs(m,call,nowNs); // ARC: once per render frame, independent of the pose gate (producers clear instead)
            if(call.eye==0)k.rigidityMicro.store(std::uint64_t(std::min(1e12,planRigidity(call.plan->world,call.plan->eyes,call.plan->unitsPerMetre)*1e6)),std::memory_order_relaxed);
        }
        const auto& f=m.frame;const auto& ad=m.arcDraw;
        // ARC: arcs draw even when the pose gate skipped the hands / holster markers / laser (their fields are empty then)
        const bool arcsHere=ad.any&&ad.ringOk&&ad.renderFrame==call.renderFrame;
        const bool markersHere=f.markersUngated&&f.markers.count; // LASSO: HANDS-OFF markers draw past the pose gate (channel 2)
        const bool reloadHere=f.reloadCount>0; // RELOAD: channel 2b draws past the pose gate too
        if(f.skip!=SkipCount&&!arcsHere&&!markersHere&&!reloadHere)return skip(f.skip);
        const bool poseBypassed=(arcsHere||markersHere||reloadHere)&&f.skip!=SkipCount&&f.skip!=SkipHidden;
        const auto& plan=*call.plan;const unsigned eye=call.eye;
        const double scale=plan.unitsPerMetre;
        const DMat4 localClip=localToClip(plan.projection[eye],plan.eyes[eye],scale);
        const float projX=std::fabs(plan.projection[eye][0]);
        const float arcPixelAngle=projX>1e-6f?2.f/(projX*float(color.extent.width)):.001f; // radians per pixel near the centre
        const double minHalfPerM=double(m.arcMinPx.load(std::memory_order_relaxed))/20.0*double(arcPixelAngle);
        // Pixel rectangle: hands + markers.
        PixelRect rect{};std::array<DMat4,2> clip{};
        for(unsigned h=0;h<2;++h)if(f.visible[h]){clip[h]=multiply(localClip,f.model[h]);
            rect.unite(screenRect(clip[h],m.base.lo[h],m.base.hi[h],color.extent.width,color.extent.height));}
        for(std::uint32_t i=0;i<f.markers.count;++i){const auto& mk=f.markers.markers[i];
            const Vec3 lo{mk.position[0]-mk.radius,mk.position[1]-mk.radius,mk.position[2]-mk.radius},hi{mk.position[0]+mk.radius,mk.position[1]+mk.radius,mk.position[2]+mk.radius};
            rect.unite(screenRect(localClip,lo,hi,color.extent.width,color.extent.height));}
        for(std::uint32_t i=0;i<f.reloadCount;++i){const auto& mk=f.reload[i];
            Vec3 lo{mk.position[0]-mk.radius,mk.position[1]-mk.radius,mk.position[2]-mk.radius},hi{mk.position[0]+mk.radius,mk.position[1]+mk.radius,mk.position[2]+mk.radius};
            const auto st=markerStyle(mk);
            if(st==ReloadStyleCartLive||st==ReloadStyleCartSpent){ // The whole round (rim 1.1 r, length L + 1.2 mm + bullet 1.45 r)
                const float len=mk.highlight+.0012f+1.45f*mk.radius,pad=1.1f*mk.radius;const auto& ax=f.reloadCart[i].axis;
                for(int k=0;k<3;++k){const float e=mk.position[k]+ax[k]*len;
                    lo[std::size_t(k)]=std::min(mk.position[k],e)-pad;hi[std::size_t(k)]=std::max(mk.position[k],e)+pad;}}
            rect.unite(screenRect(localClip,lo,hi,color.extent.width,color.extent.height));}
        if(f.laser)rect.unite(segmentRect(localClip,f.laserA,f.laserB,color.extent.width,color.extent.height,16)); // Beam + end dot
        if(f.laser2)rect.unite(segmentRect(localClip,f.laser2A,f.laser2B,color.extent.width,color.extent.height,16)); // DUAL phantom laser
        // ARC: per-segment rects (drawn width incl. the minimum pixel width), pillars, markers (lifted, >= 6 px)
        std::array<ArcEyeMarker,arcChannels> eyeMarkers{};
        if(arcsHere){
            const DVec3 eyeP{plan.eyes[eye].position[0],plan.eyes[eye].position[1],plan.eyes[eye].position[2]};
            const double nearM=plan.nearClip>0.f?double(plan.nearClip)/scale:.05;
            for(unsigned c=0;c<arcChannels;++c){const auto& it=ad.items[c];if(!it.live)continue;
                for(std::uint32_t s=0;s+3<it.entries;++s)rect.unite(arcSegmentRect(localClip,ad.verts[it.firstEntry+s+1],ad.verts[it.firstEntry+s+2],minHalfPerM,color.extent.width,color.extent.height));
                if(it.pillar)rect.unite(arcSegmentRect(localClip,ad.verts[it.pillarFirst+1],ad.verts[it.pillarFirst+2],minHalfPerM,color.extent.width,color.extent.height));
                if(!it.marker)continue;
                auto& em=eyeMarkers[c];
                DVec3 toEye{eyeP[0]-it.centre[0],eyeP[1]-it.centre[1],eyeP[2]-it.centre[2]};const double dist=std::sqrt(dotD(toEye,toEye));
                DVec3 n=it.normal;if(it.faceCamera){n=toEye;if(!normalizeD(n))continue;}
                const double lift=std::max(double(it.liftM),4.0*std::ldexp(1.0,-24)*dist*dist/std::max(nearM,1e-4));
                const DVec3 C{it.centre[0]+n[0]*lift,it.centre[1]+n[1]*lift,it.centre[2]+n[2]*lift};
                const double radius=std::max(double(it.radiusM),6.0*double(arcPixelAngle)*dist);
                const bool worldTest=!(it.style.flags&ArcStyle::NoWorldTest);
                em.draw=std::isfinite(C[0])&&std::isfinite(C[1])&&std::isfinite(C[2])&&std::isfinite(radius);if(!em.draw)continue;
                em.centre[0]=float(C[0]);em.centre[1]=float(C[1]);em.centre[2]=float(C[2]);em.centre[3]=float(radius);
                em.normal[0]=float(n[0]);em.normal[1]=float(n[1]);em.normal[2]=float(n[2]);
                em.normal[3]=float(unsigned(it.shape))+(it.shape==ArcMarkerShape::Arrow?markerHeadingTurns(n,it.facing):0.f);
                em.color[0]=rgbaChannel(it.rgba,0);em.color[1]=rgbaChannel(it.rgba,1);em.color[2]=rgbaChannel(it.rgba,2);em.color[3]=rgbaChannel(it.rgba,3);
                em.params[0]=m.arcXray.load(std::memory_order_relaxed)?std::clamp(it.xrayAlpha,0.f,1.f):0.f;em.params[1]=worldTest?1.f:0.f;
                em.params[3]=markerDepthEpsilon(localClip,C,eyeP,it.depthTolM,depthEpsilon);
                em.lo={float(C[0]-radius),float(C[1]-radius),float(C[2]-radius)};em.hi={float(C[0]+radius),float(C[1]+radius),float(C[2]+radius)};
                rect.unite(screenRect(localClip,em.lo,em.hi,color.extent.width,color.extent.height));
            }
        }
        if(rect.empty())return skip(SkipOffscreen);
        // Depth mode.
        const auto& depth=call.depth;
        const int forced=m.debugDepth.load(std::memory_order_relaxed);
        const bool depthUsable=depth.image&&depthFormat(depth.format)&&depth.samples==VK_SAMPLE_COUNT_1_BIT&&depthLayoutOk(depth.layout)&&
            depth.extent.width==color.extent.width&&depth.extent.height==color.extent.height;
        const bool canSample=depthUsable&&(depth.usage&VK_IMAGE_USAGE_SAMPLED_BIT)&&(formatFeatures(m,depth.format)&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        const bool canCopy=depthUsable&&(depth.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)&&(formatFeatures(m,depth.format)&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)&&!m.copyUnavailable;
        DepthMode mode=DepthMode::None;
        if(forced==0)mode=DepthMode::None;
        else if(forced==2)mode=canCopy?DepthMode::Copy:canSample?DepthMode::Sampled:DepthMode::None;
        else mode=canSample?DepthMode::Sampled:canCopy?DepthMode::Copy:DepthMode::None;
        // ARC: a pass that draws only arcs whose look needs no world test (or .arc.depth 0) skips the engine depth (2 barriers per eye
        // instead of 4); .arc.depth 2 prefers COPY for arc-only passes.
        const bool handsDrawn=f.visible[0]||f.visible[1]||f.markers.count||f.reloadCount||f.laser||f.laser2;
        const bool arcOnly=arcsHere&&!handsDrawn;
        // A pass that draws only the HANDS-OFF holster markers (+ arcs) - no hand, no laser - gets the same treatment:
        // the markers sit at the player's own hips and need no world test (drawn over the body mesh), so without a world-tested arc it is NONE
        // (the default HANDS OFF + NEAR + BODY ON teach-once lasso marker costs no engine depth); .arc.depth 0 / 2 as for arcs.
        const bool markerOverlay=(markersHere||reloadHere)&&!(f.visible[0]||f.visible[1]||f.laser||f.laser2); // RELOAD: + channel 2b (drawn over the gun)
        if((arcOnly||markerOverlay)&&forced!=0){const int lever=m.arcDepthLever.load(std::memory_order_relaxed);
            if(lever==0||!arcsHere||ad.allNoWorldTest)mode=DepthMode::None;
            else if(lever==2)mode=canCopy?DepthMode::Copy:canSample?DepthMode::Sampled:DepthMode::None;}
        bool failed=false;
        PipelineSlot* slot=mode==DepthMode::Copy?pipelines(m,color.format,depth.format,true,failed):pipelines(m,color.format,m.base.ownDepth,false,failed);
        if(!slot&&failed&&mode==DepthMode::Copy){m.copyUnavailable=true;return skip(SkipBuilding);}
        if(!slot&&failed){transition(State::Ready,State::Failed,"PIPELINE");return skip(SkipBuilding);} // Keeps PIPELINE_TIMEOUT
        if(!slot){if(arcsHere){std::uint64_t none=0;m.arcK.firstRequestNs.compare_exchange_strong(none,steadyNs(),std::memory_order_relaxed);}
            return skip(SkipBuilding);}
        const bool drawArcs=arcsHere&&slot->arc&&slot->arcMarker; // An arc pipeline failure leaves the hands drawn
        if(arcsHere&&!drawArcs){m.arcK.noArcPipeline.fetch_add(1,std::memory_order_relaxed);if(arcOnly)return skip(SkipBuilding);}
        // Views / framebuffer / set (render thread; imageDestroyed may run concurrently on another thread).
        // try_lock (no waiting on the render thread); imageDestroyed() holds this mutex across driver
        // destroys on another thread, so on contention this eye is drawn without hands (one frame) instead of stalling.
        std::unique_lock lock(m.cacheMutex,std::try_to_lock);
        if(!lock.owns_lock())return skip(SkipCache);
        cacheLocked=true;
        if(eye==0||m.dead.size()>16)releaseDeferred(m,call.retiredFrame);
        const VkImageView colorAttachment=colorView(m,color,call.renderFrame);
        if(!colorAttachment)return skip(SkipCache);
        OwnImage& target=mode==DepthMode::Copy?m.copy:m.own;
        if(!ownImage(m,target,mode==DepthMode::Copy?depth.format:m.base.ownDepth,color.extent,mode==DepthMode::Copy,call.renderFrame,call.retiredFrame)){
            if(mode==DepthMode::Copy)m.copyUnavailable=true;
            return skip(SkipCache);
        }
        const VkFramebuffer fb=framebuffer(m,slot->pass,colorAttachment,target.view,color.image,color.extent,call.renderFrame);
        if(!fb)return skip(SkipCache);
        VkDescriptorSet set=m.base.dummySet;
        if(mode==DepthMode::Sampled){set=depthSet(m,depth,call.renderFrame);if(!set)return skip(SkipCache);}
        lock.unlock();cacheLocked=false;
        // ---- recording (no failure past this point) ----
        const VkCommandBuffer c=call.command;
        auto& b=m.base;
        // albedo/dummy upload into this engine command buffer, before the first draw that samples them. Re-recorded in
        // each frame until the first recording frame retired (a discarded pair cannot leave the images undefined); the staging
        // buffer is freed once every frame that recorded the copy retired. Render thread only (as m.frame).
        if(m.uploadPhase==UploadRecording&&call.retiredFrame>=m.uploadFirst){
            m.uploadPhase=UploadDone;k.uploadPhase.store(UploadDone,std::memory_order_relaxed);k.uploadDoneFrame.store(call.renderFrame,std::memory_order_relaxed);}
        if(m.uploadPhase!=UploadDone&&(m.uploadLast!=call.renderFrame||m.uploadCommand!=c)&&b.staging){ // once per frame and command buffer
            recordUpload(b,c);
            if(m.uploadPhase==UploadPending){m.uploadPhase=UploadRecording;m.uploadFirst=call.renderFrame;
                k.uploadPhase.store(UploadRecording,std::memory_order_relaxed);k.uploadFirstFrame.store(call.renderFrame,std::memory_order_relaxed);}
            m.uploadLast=call.renderFrame;m.uploadCommand=c;k.uploads.fetch_add(1,std::memory_order_relaxed);k.uploadLastFrame.store(call.renderFrame,std::memory_order_relaxed);
        }
        if(m.uploadPhase==UploadDone&&b.staging&&call.retiredFrame>=m.uploadLast){
            vkDestroyBuffer(m.device,b.staging,nullptr);vkFreeMemory(m.device,b.stagingMemory,nullptr);b.staging={};b.stagingMemory={};
            k.stagingFreed.store(true,std::memory_order_relaxed);}
        bool timed=false;unsigned qslot=0;
        if(b.queries&&m.gpuTiming.load(std::memory_order_relaxed)){
            if(eye==0){readTimestamps(m);
                if(call.renderFrame%16==0){qslot=unsigned(m.timedSequence%4);
                    if(!m.querySlotPending[qslot]){m.frameQuerySlot=qslot;++m.timedSequence;m.querySlotPending[qslot]=true;m.querySlotFrame[qslot]=call.renderFrame;
                        vkCmdResetQueryPool(c,b.queries,qslot*4,4);}}}
            if(m.frameQuerySlot!=~0u){timed=true;qslot=m.frameQuerySlot;vkCmdWriteTimestamp(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,b.queries,qslot*4+2*eye);}
        }
        const VkRect2D area{{rect.x0,rect.y0},{std::uint32_t(rect.x1-rect.x0),std::uint32_t(rect.y1-rect.y0)}};
        imageBarrier(c,color.image,VK_IMAGE_ASPECT_COLOR_BIT,color.layout,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,anyAccess,
                     VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        const auto aspects=depthAspects(depth.format);
        if(mode==DepthMode::Sampled)
            imageBarrier(c,depth.image,aspects,depth.layout,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,anyAccess,VK_ACCESS_SHADER_READ_BIT);
        else if(mode==DepthMode::Copy){
            imageBarrier(c,depth.image,aspects,depth.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,anyAccess,VK_ACCESS_TRANSFER_READ_BIT);
            imageBarrier(c,target.image,depthAspects(target.format),VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,anyAccess,VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};region.dstSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
            region.srcOffset=region.dstOffset={rect.x0,rect.y0,0};region.extent={area.extent.width,area.extent.height,1};
            vkCmdCopyImage(c,depth.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            imageBarrier(c,target.image,depthAspects(target.format),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            imageBarrier(c,depth.image,aspects,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,depth.layout,VK_ACCESS_TRANSFER_READ_BIT,anyAccess);
        }
        VkClearValue clears[2]{};clears[1].depthStencil={1.f,0};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=slot->pass;begin.framebuffer=fb;begin.renderArea=area;
        begin.clearValueCount=2;begin.pClearValues=clears;
        vkCmdBeginRenderPass(c,&begin,VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0,0,float(color.extent.width),float(color.extent.height),0,1};
        vkCmdSetViewport(c,0,1,&viewport);vkCmdSetScissor(c,0,1,&area);
        vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,b.layout,0,1,&set,0,nullptr);
        const float encode=float(encodeFor(color.format)),bright=float(m.brightPercent.load(std::memory_order_relaxed))/100.f;
        const float worldTest=mode==DepthMode::Sampled?1.f:0.f;
        unsigned drawnHands=0;
        if(f.visible[0]||f.visible[1]){
            vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,slot->hands);
            for(unsigned h=0;h<2;++h){if(!f.visible[h])continue;
                struct Push {float clip[16];float weights[4];float light[4];float params[4];} push{};
                static_assert(sizeof(Push)==112);
                const auto cf=narrow(clip[h]);std::memcpy(push.clip,cf.data(),sizeof push.clip);
                std::memcpy(push.weights,f.weights[h].data(),sizeof push.weights);
                push.light[0]=f.light[h][0];push.light[1]=f.light[h][1];push.light[2]=f.light[h][2];push.light[3]=ambient;
                push.params[0]=bright;push.params[1]=worldTest;push.params[2]=encode;push.params[3]=depthEpsilon;
                vkCmdPushConstants(c,b.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
                vkCmdBindVertexBuffers(c,0,1,&b.mesh,&b.vertexOffset[h]);
                vkCmdBindIndexBuffer(c,b.mesh,b.indexOffset[h],VK_INDEX_TYPE_UINT16);
                vkCmdDrawIndexed(c,b.indexCount[h],1,0,0,0);++drawnHands;
            }
        }
        if(f.markers.count){
            vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,slot->markers);
            const auto right=rotateD(plan.eyes[eye].orientation,{1,0,0}),up=rotateD(plan.eyes[eye].orientation,{0,1,0});
            const auto vp=narrow(localClip);
            for(std::uint32_t i=0;i<f.markers.count;++i){const auto& mk=f.markers.markers[i];
                struct Push {float viewProj[16];float centre[4];float right[4];float up[4];float params[4];} push{};
                static_assert(sizeof(Push)==pushBytes);
                std::memcpy(push.viewProj,vp.data(),sizeof push.viewProj);
                push.centre[0]=mk.position[0];push.centre[1]=mk.position[1];push.centre[2]=mk.position[2];push.centre[3]=mk.radius;
                push.right[0]=float(right[0]);push.right[1]=float(right[1]);push.right[2]=float(right[2]);push.right[3]=std::clamp(mk.highlight,0.f,1.f);
                push.up[0]=float(up[0]);push.up[1]=float(up[1]);push.up[2]=float(up[2]);push.up[3]=bright;
                push.params[0]=float(markerStyle(mk));
                push.params[1]=worldTest;push.params[2]=encode;push.params[3]=depthEpsilon;
                vkCmdPushConstants(c,b.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
                vkCmdDraw(c,6,1,0,0);
            }
            k.drawnMarkers.fetch_add(f.markers.count,std::memory_order_relaxed);
        }
        std::uint32_t reloadMarkers=0;
        for(std::uint32_t i=0;i<f.reloadCount;++i){const auto st=markerStyle(f.reload[i]);reloadMarkers+=st!=ReloadStyleCartLive&&st!=ReloadStyleCartSpent;}
        if(f.reloadCount>reloadMarkers&&slot->cartridge){ // RELOAD: procedural cartridges (opaque, private depth; the cylinder hides them analytically)
            vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,slot->cartridge);
            const auto vp=narrow(localClip);
            for(std::uint32_t i=0;i<f.reloadCount;++i){const auto& mk=f.reload[i];const auto st=markerStyle(mk);
                if(st!=ReloadStyleCartLive&&st!=ReloadStyleCartSpent)continue;const auto& cd=f.reloadCart[i];
                struct Push {float viewProj[16];float base[4];float axis[4];float cyl[4];float params[4];} push{};
                static_assert(sizeof(Push)==pushBytes);
                std::memcpy(push.viewProj,vp.data(),sizeof push.viewProj);
                push.base[0]=mk.position[0];push.base[1]=mk.position[1];push.base[2]=mk.position[2];push.base[3]=mk.radius;
                push.axis[0]=cd.axis[0];push.axis[1]=cd.axis[1];push.axis[2]=cd.axis[2];push.axis[3]=mk.highlight;
                for(int k=0;k<4;++k)push.cyl[k]=cd.cyl[k];
                push.params[0]=float(st);push.params[1]=bright;push.params[2]=encode;push.params[3]=cd.cylLen;
                vkCmdPushConstants(c,b.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
                vkCmdDraw(c,576,1,0,0);
            }
            k.drawnMarkers.fetch_add(f.reloadCount-reloadMarkers,std::memory_order_relaxed);
        }
        if(reloadMarkers){ // RELOAD channel 2b: the same marker pipeline, palette style per item, never world-tested (drawn over the gun)
            vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,slot->markers);
            const auto right=rotateD(plan.eyes[eye].orientation,{1,0,0}),up=rotateD(plan.eyes[eye].orientation,{0,1,0});
            const auto vp=narrow(localClip);
            for(std::uint32_t i=0;i<f.reloadCount;++i){const auto& mk=f.reload[i];
                if(markerStyle(mk)==ReloadStyleCartLive||markerStyle(mk)==ReloadStyleCartSpent)continue;
                struct Push {float viewProj[16];float centre[4];float right[4];float up[4];float params[4];} push{};
                static_assert(sizeof(Push)==pushBytes);
                std::memcpy(push.viewProj,vp.data(),sizeof push.viewProj);
                push.centre[0]=mk.position[0];push.centre[1]=mk.position[1];push.centre[2]=mk.position[2];push.centre[3]=mk.radius;
                push.right[0]=float(right[0]);push.right[1]=float(right[1]);push.right[2]=float(right[2]);push.right[3]=std::clamp(mk.highlight,0.f,1.f);
                push.up[0]=float(up[0]);push.up[1]=float(up[1]);push.up[2]=float(up[2]);push.up[3]=bright;
                push.params[0]=float(markerStyle(mk));push.params[1]=0.f;push.params[2]=encode;push.params[3]=depthEpsilon;
                vkCmdPushConstants(c,b.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
                vkCmdDraw(c,6,1,0,0);
            }
            k.drawnMarkers.fetch_add(reloadMarkers,std::memory_order_relaxed);
        }
        if((f.laser||f.laser2)&&slot->laser){ // Laser sight beam (+ end dot), the same placement as the engine-drawn gun this frame
            vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,slot->laser);
            struct Push {float viewProj[16];float a[4];float b[4];float eye[4];float params[4];} push{};
            static_assert(sizeof(Push)==pushBytes);
            const auto vp=narrow(localClip);std::memcpy(push.viewProj,vp.data(),sizeof push.viewProj);
            const float p0=std::fabs(plan.projection[eye][0]);
            const float pixelAngle=p0>1e-6f?2.f/(p0*float(color.extent.width)):.001f; // radians per pixel near the centre
            const auto& ep=plan.eyes[eye].position;push.eye[0]=ep[0];push.eye[1]=ep[1];push.eye[2]=ep[2];push.eye[3]=bright;
            push.params[0]=.8f*pixelAngle;push.params[1]=worldTest;push.params[2]=encode;push.params[3]=depthEpsilon;
            for(unsigned beam=0;beam<2;++beam){ // DUAL: 1 = the phantom firearm's laser
                if(!(beam?f.laser2:f.laser))continue;
                const Vec3& la=beam?f.laser2A:f.laserA;const Vec3& lb=beam?f.laser2B:f.laserB;
                push.a[0]=la[0];push.a[1]=la[1];push.a[2]=la[2];push.a[3]=beam?f.laser2HalfWidth:f.laserHalfWidth;
                push.b[0]=lb[0];push.b[1]=lb[1];push.b[2]=lb[2];push.b[3]=0.f;
                vkCmdPushConstants(c,b.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
                vkCmdDraw(c,6,1,0,0);
                if(beam?f.laser2Dot:f.laserDot){push.b[3]=1.f;vkCmdPushConstants(c,b.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
                    vkCmdDraw(c,6,1,0,0);k.laserDots.fetch_add(1,std::memory_order_relaxed);}
            }
            k.laserEyes.fetch_add(1,std::memory_order_relaxed);
        }
        // ARC: ribbons -> pillars -> end markers (one bind of the ring per eye: prev / a / b / next = entry offsets 0..3)
        unsigned arcSegments=0,arcPillars=0,arcMarkers=0;const bool xrayOn=m.arcXray.load(std::memory_order_relaxed)!=0;
        if(drawArcs){
            vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,slot->arc);
            const VkDeviceSize base=VkDeviceSize(ad.slot)*arcRingSlotBytes;
            const VkBuffer rings[4]{b.arcRing,b.arcRing,b.arcRing,b.arcRing};
            const VkDeviceSize offsets[4]{base,base+sizeof(ArcVertex),base+2*sizeof(ArcVertex),base+3*sizeof(ArcVertex)};
            vkCmdBindVertexBuffers(c,0,4,rings,offsets);
            struct Push {float viewProj[16];float eye[4];float params[4];float style[4];float shape[4];} push{};
            static_assert(sizeof(Push)==pushBytes);
            const auto vp=narrow(localClip);std::memcpy(push.viewProj,vp.data(),sizeof push.viewProj);
            const auto& ep=plan.eyes[eye].position;push.eye[0]=ep[0];push.eye[1]=ep[1];push.eye[2]=ep[2];push.eye[3]=bright;
            push.params[0]=float(minHalfPerM);push.params[2]=encode;push.params[3]=depthEpsilon;
            const double seconds=double(nowNs%86'400'000'000'000ull)/1e9;
            for(unsigned ch=0;ch<arcChannels;++ch){const auto& it=ad.items[ch];if(!it.live)continue;
                const bool wt=worldTest>0.f&&!(it.style.flags&ArcStyle::NoWorldTest);
                push.params[1]=wt?1.f:0.f;
                const float period=it.style.dashPeriodM>0.f?it.style.dashPeriodM:0.f;
                push.style[0]=period;push.style[1]=std::clamp(it.style.dashDuty,.01f,1.f);
                push.style[2]=period>0.f?float(std::fmod(seconds*double(it.style.dashSpeedMps),double(period))):0.f;
                push.style[3]=xrayOn?std::clamp(it.style.xrayAlpha,0.f,1.f):0.f;
                push.shape[0]=std::max(it.style.fadeInM,0.f);push.shape[1]=std::clamp(it.style.tube,0.f,1.f);push.shape[2]=std::max(it.style.core,0.f);
                push.shape[3]=(it.style.flags&ArcStyle::DashFromEnd)?std::max(it.lengthM,1e-3f):0.f;
                vkCmdPushConstants(c,b.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
                vkCmdDraw(c,6,it.entries-3,0,it.firstEntry);arcSegments+=it.entries-3;
                m.arcCh[ch].drawn.fetch_add(1,std::memory_order_relaxed);
                if(it.pillar){ // the landing pillar: solid, no fade-in, the marker's x-ray
                    push.style[0]=0.f;push.style[2]=0.f;push.style[3]=xrayOn?std::clamp(it.xrayAlpha,0.f,1.f):0.f;
                    push.shape[0]=0.f;push.shape[1]=1.f;push.shape[2]=.3f;push.shape[3]=0.f;
                    vkCmdPushConstants(c,b.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
                    vkCmdDraw(c,6,1,0,it.pillarFirst);++arcPillars;}
            }
            bool markerBound=false;
            for(unsigned ch=0;ch<arcChannels;++ch){const auto& em=eyeMarkers[ch];if(!em.draw)continue;
                if(!markerBound){vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,slot->arcMarker);markerBound=true;}
                struct MPush {float viewProj[16];float centre[4];float normal[4];float color[4];float params[4];} mp{};
                static_assert(sizeof(MPush)==pushBytes);
                std::memcpy(mp.viewProj,vp.data(),sizeof mp.viewProj);std::memcpy(mp.centre,em.centre,sizeof mp.centre);
                std::memcpy(mp.normal,em.normal,sizeof mp.normal);
                mp.color[0]=em.color[0]*bright;mp.color[1]=em.color[1]*bright;mp.color[2]=em.color[2]*bright;mp.color[3]=em.color[3];
                mp.params[0]=em.params[0];mp.params[1]=worldTest>0.f?em.params[1]:0.f;mp.params[2]=encode;mp.params[3]=em.params[3];
                vkCmdPushConstants(c,b.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof mp,&mp);
                vkCmdDraw(c,6,1,0,0);++arcMarkers;
            }
        }
        vkCmdEndRenderPass(c);
        imageBarrier(c,color.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,color.layout,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,anyAccess);
        if(mode==DepthMode::Sampled)
            imageBarrier(c,depth.image,aspects,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,depth.layout,VK_ACCESS_SHADER_READ_BIT,anyAccess);
        if(timed){vkCmdWriteTimestamp(c,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,b.queries,qslot*4+2*eye+1);
            const bool taaRan=taa::recordedEyeKey.load(std::memory_order_relaxed)==((call.renderFrame<<1)|eye);
            m.querySlotBucket[qslot][eye]=splitBucket(drawArcs?(handsDrawn?1u:0u):2u,mode,taaRan);}
        restore(c);k.restoreCalls.fetch_add(1,std::memory_order_relaxed);
        // ---- bookkeeping ----
        k.drawnEyes.fetch_add(1,std::memory_order_relaxed);k.drawnPerEye[eye].fetch_add(1,std::memory_order_relaxed);
        k.drawnHands.fetch_add(drawnHands,std::memory_order_relaxed);k.modes[unsigned(mode)].fetch_add(1,std::memory_order_relaxed);
        k.lastMode[eye].store(unsigned(mode),std::memory_order_relaxed);
        k.pixels.fetch_add(rect.area(),std::memory_order_relaxed);k.eyePixels.fetch_add(std::uint64_t(color.extent.width)*color.extent.height,std::memory_order_relaxed);
        k.lastRect.store((std::uint64_t(area.extent.width)<<32)|area.extent.height,std::memory_order_relaxed);
        if(drawArcs){auto& a=m.arcK;
            a.drawnEyes.fetch_add(1,std::memory_order_relaxed);a.segments.fetch_add(arcSegments,std::memory_order_relaxed);
            a.pillars.fetch_add(arcPillars,std::memory_order_relaxed);a.markers.fetch_add(arcMarkers,std::memory_order_relaxed);
            if(mode==DepthMode::Sampled&&xrayOn)a.xrayEyes.fetch_add(1,std::memory_order_relaxed);
            if(mode==DepthMode::Copy)a.xrayUnavailable.fetch_add(1,std::memory_order_relaxed);
            if(mode==DepthMode::None)a.noDepthEyes.fetch_add(1,std::memory_order_relaxed);
            if(poseBypassed)a.poseGateBypassed.fetch_add(1,std::memory_order_relaxed);
            if(arcOnly)a.arcOnlyPasses.fetch_add(1,std::memory_order_relaxed);}
        if(reloadHere)m.arcK.reloadPasses.fetch_add(1,std::memory_order_relaxed);
        if(markersHere){m.arcK.markerPasses.fetch_add(1,std::memory_order_relaxed);if(poseBypassed&&!drawArcs)m.arcK.poseGateBypassed.fetch_add(1,std::memory_order_relaxed);
            if(markerOverlay&&mode==DepthMode::None)m.arcK.markerNoDepthEyes.fetch_add(1,std::memory_order_relaxed);}
        return true;
    }catch(...){
        setState(State::Failed,"RECORD_EXCEPTION");return false;
    }
}

// ---- RELOAD overlay channel 2b (HandsArcApi.h) ----
void publishReloadOverlay(const ReloadOverlay& overlay) noexcept {
    Errno preserve;auto& m=module();if(m.state.load(std::memory_order_relaxed)==int(State::Killed))return;
    ReloadOverlay o=overlay;o.ns=steadyNs();o.count=std::min<std::uint32_t>(o.count,12u);m.reloadOverlay.store(o);
    m.reloadPublished.fetch_add(1,std::memory_order_relaxed);
    m.overlayVisibleNs[unsigned(Overlay::Reload)].store(o.count?o.ns:0,std::memory_order_relaxed); // only VISIBLE content keeps the pass alive
}
// ---- ARC public API (HandsArcApi.h) ----
void publishArc(ArcChannel channel,const ArcFrame& frame) noexcept {
    Errno preserve;const auto c=unsigned(channel);if(c>=arcChannels)return;
    auto& m=module();auto& k=m.arcCh[c];
    if(killed(Feature::Arc)||killed(Feature::Hands)||m.arcState.load(std::memory_order_acquire)==int(ArcState::Killed)||
       m.state.load(std::memory_order_acquire)==int(State::Killed)){k.killed.fetch_add(1,std::memory_order_relaxed);return;}
    switch(m.arcBoards[c].publish(frame,steadyNs(),arcToken())){
    case ArcBoard::Published:k.published.fetch_add(1,std::memory_order_relaxed);if(!frame.count)k.hides.fetch_add(1,std::memory_order_relaxed);break;
    case ArcBoard::Rejected:k.rejected.fetch_add(1,std::memory_order_relaxed);break;
    case ArcBoard::WrongThread:k.wrongThread.fetch_add(1,std::memory_order_relaxed);break;
    case ArcBoard::Unkeyed:k.unkeyed.fetch_add(1,std::memory_order_relaxed);break;
    }
}
void clearArc(ArcChannel channel) noexcept {
    Errno preserve;const auto c=unsigned(channel);if(c>=arcChannels)return;
    auto& m=module();auto& k=m.arcCh[c];
    if(m.arcBoards[c].clear(steadyNs(),arcToken()))k.cleared.fetch_add(1,std::memory_order_relaxed);else k.wrongThread.fetch_add(1,std::memory_order_relaxed);
}
void armArc(ArcChannel channel,bool armed) noexcept {
    const auto c=unsigned(channel);if(c>=arcChannels)return;auto& m=module();
    if(!armed){m.arcArmed.fetch_and(~(1u<<c),std::memory_order_relaxed);return;}
    // On the 0 -> armed edge a consumer starts waiting NOW. While nothing was armed (idle pass, no eye format / slot) the
    // status has been BUILDING since startup; its age must count from here, else the consumers' "Building > 3 s -> NO ARC" fallback fires
    // on the first ticks after every new selection (prewarm builds the slot ~65 ms later).
    if(!m.arcArmed.fetch_or(1u<<c,std::memory_order_relaxed)&&arcStatus()==ArcStatus::Building)
        m.arcStatusSinceNs.store(steadyNs(),std::memory_order_relaxed);
}
namespace {
// idle (out): BUILDING only because no pipeline slot of the eye format exists and nothing asks for one (no armed channel): the pass has
// been idle (HANDS OFF, LASER OFF, no overlay) since (re)configure. Reported as reason "IDLE"; the status stays BUILDING.
ArcStatus computeArcStatus(Module& m,bool* idle=nullptr) noexcept {
    const auto hs=State(m.state.load(std::memory_order_acquire));const auto as=ArcState(m.arcState.load(std::memory_order_acquire));
    if(hs==State::Killed||as==ArcState::Killed)return ArcStatus::Killed;
    if(hs==State::Off||as==ArcState::Off)return ArcStatus::Off;
    if(hs==State::Failed||as==ArcState::Failed)return ArcStatus::Failed;
    if(m.suppressed.load(std::memory_order_relaxed))return ArcStatus::Suppressed;
    if(hs!=State::Ready||as!=ArcState::Ready||!m.baseReady.load(std::memory_order_acquire))return ArcStatus::Building;
    const bool armed=m.arcArmed.load(std::memory_order_relaxed)!=0;
    const auto fmt=VkFormat(m.arcLastColor.load(std::memory_order_relaxed));
    if(fmt==VK_FORMAT_UNDEFINED){if(idle)*idle=!armed;return ArcStatus::Building;}
    // READY when ANY built slot of the eye format has its arc pipelines - the SAMPLED / NONE key (ownDepth, clear) or the
    // COPY key (engine depth format, load). A pass on COPY depth (.hands.depth 2, or an engine depth that cannot be sampled) with HANDS /
    // LASER ON is never idle, so the SAMPLED key is never prewarmed, yet its arcs draw from the COPY slot.
    bool any=false,failed=false;
    for(auto& s:m.slots){const int st=s.status.load(std::memory_order_acquire);
        if(!st||s.color!=fmt)continue;any=true;
        if(st==2){if(s.arcStatus.load(std::memory_order_acquire)==2)return ArcStatus::Ready;failed=true;} // built, arc pipelines failed
        else if(st==3&&s.depth==m.base.ownDepth&&!s.load)failed=true; // SAMPLED key failed (recordEye fails the hands); a COPY failure falls back
    }
    if(failed)return ArcStatus::Failed;
    if(!any&&idle)*idle=!armed;
    return ArcStatus::Building;
}
} // namespace
ArcStatus arcStatus() noexcept {
    auto& m=module();const auto s=computeArcStatus(m);
    int prev=m.arcLastStatus.load(std::memory_order_relaxed);
    if(prev!=int(s)&&m.arcLastStatus.compare_exchange_strong(prev,int(s),std::memory_order_relaxed))m.arcStatusSinceNs.store(steadyNs(),std::memory_order_relaxed);
    return s;
}
std::uint64_t arcStatusAgeMs() noexcept {
    arcStatus();const auto since=module().arcStatusSinceNs.load(std::memory_order_relaxed),now=steadyNs();
    return since&&now>since?(now-since)/1'000'000ull:0;
}
const char* arcStatusNote() noexcept {
    auto& m=module();
    switch(arcStatus()){
    case ArcStatus::Off:return "OFF";case ArcStatus::Ready:return "OK";
    case ArcStatus::Building:{bool idle=false;computeArcStatus(m,&idle);return idle?"IDLE":"BUILDING";} // IDLE = nothing to build yet
    case ArcStatus::Killed:return "KILLED";case ArcStatus::Suppressed:return "SUPPRESSED";
    case ArcStatus::Failed:{const char* e=m.arcError.load(std::memory_order_relaxed);if(e&&*e)return e;
        if(m.state.load(std::memory_order_acquire)==int(State::Failed)){const char* r=m.reason.load(std::memory_order_relaxed);return r&&*r?r:"HANDS_FAILED";}
        return "ARC_PIPELINE";}
    }
    return "UNKNOWN";
}
ArcChannelStats arcChannelStats(ArcChannel channel) noexcept {
    const auto c=unsigned(channel);if(c>=arcChannels)return {};const auto& k=module().arcCh[c];
    const auto l=[](const std::atomic<std::uint64_t>& a){return a.load(std::memory_order_relaxed);};
    return {l(k.published),l(k.cleared),l(k.hides),l(k.wrongThread),l(k.rejected),l(k.unkeyed),l(k.killed)};
}
void setSynthesizingProbe(bool (*probe)() noexcept) noexcept {module().synthesizing.store(probe,std::memory_order_release);}
void setArcDebugForTests(int kill,int xray,int failArcPipelines,int depthLever) noexcept {
    auto& m=module();m.arcKillForTests.store(kill?1:0,std::memory_order_relaxed);m.arcFailPipelinesForTests.store(failArcPipelines?1:0,std::memory_order_relaxed);
    if(xray>=0)m.arcXray.store(xray?1:0,std::memory_order_relaxed);
    if(depthLever>=0)m.arcDepthLever.store(std::clamp(depthLever,0,2),std::memory_order_relaxed);
    m.arcDebugOverride=true;
}
void resetArcWritersForTests() noexcept {for(auto& b:module().arcBoards)b.resetWriterForTests();}
namespace {
std::string arcReport(Module& m) {
    std::ostringstream o;auto& a=m.arcK;const auto load=[](const std::atomic<std::uint64_t>& x){return x.load(std::memory_order_relaxed);};
    const auto st=arcStatus();const auto now=steadyNs();
    const auto req=load(a.firstRequestNs),rdy=load(a.firstReadyNs);
    o<<"{\"state\":\""<<arcStateName(ArcState(m.arcState.load(std::memory_order_acquire)))<<"\",\"status\":\""<<arcStatusName(st)
     <<"\",\"statusAgeMs\":"<<arcStatusAgeMs()<<",\"reason\":\""<<arcStatusNote()<<"\",\"error\":\""<<m.arcError.load(std::memory_order_relaxed)<<"\""
     <<",\"armed\":"<<m.arcArmed.load(std::memory_order_relaxed)<<",\"xray\":"<<m.arcXray.load(std::memory_order_relaxed)
     <<",\"widthPct\":"<<m.arcWidthPct.load(std::memory_order_relaxed)<<",\"minpx\":"<<m.arcMinPx.load(std::memory_order_relaxed)
     <<",\"depthLever\":"<<m.arcDepthLever.load(std::memory_order_relaxed)<<",\"asw\":"<<m.arcAsw.load(std::memory_order_relaxed)
     <<",\"rects\":"<<m.arcRects.load(std::memory_order_relaxed)<<",\"synthesizingProbe\":"<<(m.synthesizing.load(std::memory_order_relaxed)!=nullptr)
     <<",\"prewarm\":{\"calls\":"<<load(a.prewarmCalls)<<",\"requests\":"<<load(a.prewarmRequests)
     <<",\"firstRequestToReadyMs\":"<<(req&&rdy&&rdy>=req?double(rdy-req)/1e6:-1.0)<<"}"
     <<",\"channels\":[";
    for(unsigned c=0;c<arcChannels;++c){const auto& k=m.arcCh[c];
        o<<(c?",":"")<<"{\"name\":\""<<arcChannelNames[c]<<"\",\"live\":"<<m.arcBoards[c].live(now)<<",\"published\":"<<load(k.published)
         <<",\"cleared\":"<<load(k.cleared)<<",\"hides\":"<<load(k.hides)<<",\"wrongThread\":"<<load(k.wrongThread)<<",\"rejected\":"<<load(k.rejected)
         <<",\"unkeyed\":"<<load(k.unkeyed)<<",\"killed\":"<<load(k.killed)<<",\"frames\":"<<load(k.frames)
         <<",\"liveFrames\":"<<load(k.liveFrames)<<",\"liveExact\":"<<load(k.liveExact);
        for(unsigned i=0;i<ArcBoard::PickCount;++i)o<<",\""<<arcPickNames[i]<<"\":"<<load(k.picks[i]);
        o<<",\"hidden\":"<<load(k.hidden)<<",\"drawnEyes\":"<<load(k.drawn)<<",\"lastPoints\":"<<k.lastPoints.load(std::memory_order_relaxed)
         <<",\"lastLengthM\":"<<double(k.lastLengthMm.load(std::memory_order_relaxed))/1000.0
         <<",\"lastLagMs\":"<<double(load(k.lastLagUs))/1000.0<<",\"maxLagMs\":"<<double(load(k.maxLagUs))/1000.0<<"}";}
    o<<"],\"computedFrames\":"<<load(a.computed)<<",\"drawnEyes\":"<<load(a.drawnEyes)<<",\"segments\":"<<load(a.segments)<<",\"markers\":"<<load(a.markers)
     <<",\"pillars\":"<<load(a.pillars)<<",\"xrayEyes\":"<<load(a.xrayEyes)<<",\"xrayUnavailable\":"<<load(a.xrayUnavailable)
     <<",\"noDepthEyes\":"<<load(a.noDepthEyes)<<",\"poseGateBypassed\":"<<load(a.poseGateBypassed)<<",\"noArcPipeline\":"<<load(a.noArcPipeline)
     <<",\"synthesizingFrames\":"<<load(a.synthesizingFrames)
     <<",\"ring\":{\"slots\":"<<arcRingSlots<<",\"slotBytes\":"<<arcRingSlotBytes<<",\"busy\":"<<load(a.ringBusy)<<",\"writes\":"<<load(a.ringWrites)
     <<",\"bytes\":"<<load(a.ringBytes)<<"}"
     <<",\"pipelines\":{\"built\":"<<load(a.pipelinesBuilt)<<",\"failed\":"<<load(a.pipelinesFailed)<<"}"
     <<",\"overlay\":{\"arcOnlyPasses\":"<<load(a.arcOnlyPasses)<<",\"markerPasses\":"<<load(a.markerPasses)<<",\"reloadPasses\":"<<load(a.reloadPasses)
     <<",\"propsPasses\":"<<load(a.propsPasses)<<",\"weaponMarkers\":"<<load(a.weaponMarkers)<<",\"reloadItems\":"<<load(a.reloadItems)<<",\"reloadPublished\":"<<load(m.reloadPublished)
     <<",\"markerNoDepthEyes\":"<<load(a.markerNoDepthEyes)<<"}"
     <<",\"gpu\":{\"split\":[";
    bool first=true;
    for(unsigned cc=0;cc<splitContents;++cc)for(unsigned d=0;d<unsigned(DepthMode::Count);++d)for(unsigned taa=0;taa<2;++taa){
        const unsigned bk=splitBucket(cc,DepthMode(d),taa!=0);
        o<<(first?"":",")<<"{\"pass\":\""<<splitContentNames[cc]<<"\",\"depth\":\""<<depthName(DepthMode(d))<<"\",\"taa\":"<<taa
         <<",\"frames\":"<<load(a.splitFrames[bk])<<",\"avgUs\":"<<double(load(a.splitAvgNs[bk]))/1000.0<<"}";first=false;}
    o<<"]}}";
    return o.str();
}
} // namespace
std::string report() {
    auto& m=module();auto& k=m.counters;std::ostringstream o;
    const bool baseReady=m.baseReady.load(std::memory_order_acquire);
    const auto load=[](const std::atomic<std::uint64_t>& a){return a.load(std::memory_order_relaxed);};
    o<<"{\"state\":\""<<stateName(state())<<"\",\"reason\":\""<<m.reason.load(std::memory_order_relaxed)<<"\""
     <<",\"menu\":"<<(settings().hands()?1:0)<<",\"suppressed\":"<<m.suppressed.load(std::memory_order_relaxed)
     <<",\"hook\":"<<(eyeHook.load(std::memory_order_relaxed)!=nullptr)
     <<",\"restoreHooks\":"<<(m.restoreReady.load(std::memory_order_relaxed)&&m.restore.load(std::memory_order_relaxed))
     <<",\"debugDepth\":"<<m.debugDepth.load(std::memory_order_relaxed)<<",\"brightPercent\":"<<m.brightPercent.load(std::memory_order_relaxed)
     <<",\"depthMode\":[\""<<depthName(DepthMode(k.lastMode[0].load(std::memory_order_relaxed)))<<"\",\""<<depthName(DepthMode(k.lastMode[1].load(std::memory_order_relaxed)))<<"\"]"
     <<",\"modes\":{\"none\":"<<load(k.modes[0])<<",\"sampled\":"<<load(k.modes[1])<<",\"copy\":"<<load(k.modes[2])<<"}"
     <<",\"calls\":"<<load(k.calls)<<",\"frames\":"<<load(k.frames)<<",\"drawnEyes\":"<<load(k.drawnEyes)
     <<",\"drawsPerEye\":["<<load(k.drawnPerEye[0])<<','<<load(k.drawnPerEye[1])<<"],\"hands\":"<<load(k.drawnHands)<<",\"markers\":"<<load(k.drawnMarkers)
     <<",\"skips\":{";
    for(unsigned i=0;i<SkipCount;++i)o<<(i?",":"")<<'"'<<skipNames[i]<<"\":"<<load(k.skips[i]);
    o<<"},\"poseMatch\":{\"exact\":"<<load(k.matches[0])<<",\"older\":"<<load(k.matches[1])<<",\"newer\":"<<load(k.matches[2])<<",\"none\":"<<load(k.matches[3])<<"}"
     <<",\"frameIdDelta\":{\"last\":"<<k.lastDelta.load(std::memory_order_relaxed)<<",\"max\":"<<k.maxDelta.load(std::memory_order_relaxed)<<"}";
    const auto eyePixels=load(k.eyePixels);const auto rect=load(k.lastRect);
    o<<",\"renderAreaPercent\":"<<(eyePixels?100.0*double(load(k.pixels))/double(eyePixels):0.0)
     <<",\"lastRect\":["<<(rect>>32)<<','<<(rect&0xffffffffu)<<"]"
     <<",\"gpu\":{\"timed\":"<<load(k.timed)<<",\"lastUs\":"<<double(load(k.gpuLastNs))/1000.0<<",\"avgUs\":"<<double(load(k.gpuAvgNs))/1000.0
     <<",\"unavailable\":"<<load(k.gpuUnavailable)<<",\"pool\":"<<(baseReady&&m.base.queries!=VK_NULL_HANDLE)<<"}"
     <<",\"planRigidityMicro\":"<<load(k.rigidityMicro)
     <<",\"objects\":{\"views\":"<<load(k.views)<<",\"framebuffers\":"<<load(k.framebuffers)<<",\"depthSets\":"<<load(k.sets)
     <<",\"imageDrops\":"<<load(k.imageDrops)<<",\"deferred\":"<<load(k.graveyard)<<",\"deferredReleased\":"<<load(k.graveyardReleased)<<",\"ownDepth\":"<<load(k.ownImages)<<",\"copyDepth\":"<<load(k.copyImages)<<"}"
     <<",\"restores\":"<<load(k.restoreCalls);
    { // VR-held weapon frames (held-hand sync, laser sight) and the gun-map vs eye-plan residual
        const auto lf=load(k.laserFrames),rf=load(k.residualFrames);
        o<<",\"weapon\":{\"frames\":"<<load(k.weaponFrames)<<",\"missing\":"<<load(k.weaponMissing)<<",\"otherSpace\":"<<load(k.weaponSpace)
         <<",\"heldSync\":{\"enabled\":"<<m.heldSync.load(std::memory_order_relaxed)<<",\"applied\":"<<load(k.heldSynced)<<"}"
         <<",\"phantom\":{\"frames\":"<<load(k.phantomFrames)<<",\"synced\":"<<load(k.phantomSynced)<<",\"laserFrames\":"<<load(k.phantomLaserFrames)
         <<",\"heldMask\":"<<m.heldMask.load(std::memory_order_relaxed)<<"}"
         <<",\"laser\":{\"menu\":"<<(settings().laser()&&enabled(Feature::Laser)?1:0)<<",\"frames\":"<<lf<<",\"drawnEyes\":"<<load(k.laserEyes)
         <<",\"dots\":"<<load(k.laserDots)<<",\"hitFrames\":"<<load(k.laserHits)
         <<",\"lengthMAvg\":"<<(lf?double(load(k.laserMmSum))/double(lf)/1000.0:0.0)<<",\"lastLengthM\":"<<double(k.laserLastMm.load(std::memory_order_relaxed))/1000.0<<"}"
         <<",\"residual\":{\"frames\":"<<rf<<",\"cmAvg\":"<<(rf?double(load(k.residualUmSum))/double(rf)/1e4:0.0)
         <<",\"cmMax\":"<<double(load(k.residualUmMax))/1e4<<",\"degAvg\":"<<(rf?double(load(k.residualMdegSum))/double(rf)/1e3:0.0)
         <<",\"degMax\":"<<double(load(k.residualMdegMax))/1e3
         <<",\"steady\":{\"cmMax\":"<<double(load(k.residualSteadyUmMax))/1e4<<",\"degMax\":"<<double(load(k.residualSteadyMdegMax))/1e3<<"}"
         <<",\"bridged\":{\"frames\":"<<load(k.residualBridgedFrames)<<",\"cmMax\":"<<double(load(k.residualBridgedUmMax))/1e4
         <<",\"degMax\":"<<double(load(k.residualBridgedMdegMax))/1e3<<"}}}";
    }
    o
     <<",\"buildMs\":"<<(baseReady?double(m.base.buildNs)/1e6:0.0);
    // build steps. stagesMs = start of each reached stage in ms after configure (-1 = not reached); elapsedMs = configure
    // -> READY/FAILED (or now while BUILDING); stageMs = time spent so far in the current stage.
    {const auto now=steadyNs(),start=m.configureNs.load(std::memory_order_relaxed);
     const auto ms=[&](std::uint64_t t){return t&&start&&t>=start?double(t-start)/1e6:-1.0;};
     const auto at=std::min(m.stage.load(std::memory_order_acquire),unsigned(Stage::Count)-1);
     const auto ready=m.readyNs.load(std::memory_order_relaxed),failed=m.failedNs.load(std::memory_order_relaxed);
     const auto end=failed?failed:ready?ready:now;const auto stageAt=m.stageNs[at].load(std::memory_order_relaxed);
     const auto tStage=std::min(m.timeoutStage.load(std::memory_order_relaxed),unsigned(Stage::Count)-1);
     o<<",\"build\":{\"stage\":\""<<stageNames[at]<<"\",\"lastError\":\""<<m.lastError.load(std::memory_order_relaxed)<<"\""
      <<",\"workerStarted\":"<<m.workerStarted.load(std::memory_order_relaxed)<<",\"baseReady\":"<<baseReady
      <<",\"elapsedMs\":"<<(start&&end>=start?double(end-start)/1e6:0.0)<<",\"stageMs\":"<<(stageAt&&now>=stageAt?double(now-stageAt)/1e6:0.0)
      <<",\"readyMs\":"<<ms(ready)<<",\"failedMs\":"<<ms(failed)<<",\"timeoutMs\":"<<m.buildTimeoutMs.load(std::memory_order_relaxed)
      <<",\"timeoutStage\":\""<<(tStage==unsigned(Stage::NotStarted)?"":stageNames[tStage])<<"\""
      <<",\"queueSubmit\":0,\"stagesMs\":{";
     for(unsigned i=1;i<unsigned(Stage::Count);++i)o<<(i>1?",":"")<<'"'<<stageNames[i]<<"\":"<<ms(m.stageNs[i].load(std::memory_order_relaxed));
     o<<"}}";}
    {const auto phase=std::min(k.uploadPhase.load(std::memory_order_relaxed),2u);
     o<<",\"upload\":{\"phase\":\""<<uploadNames[phase]<<"\",\"recorded\":"<<load(k.uploads)<<",\"firstFrame\":"<<load(k.uploadFirstFrame)
      <<",\"lastFrame\":"<<load(k.uploadLastFrame)<<",\"doneFrame\":"<<load(k.uploadDoneFrame)
      <<",\"stagingFreed\":"<<k.stagingFreed.load(std::memory_order_relaxed)<<",\"bytes\":"<<assets::albedo().size()<<"}";}
    o<<",\"arc\":"<<arcReport(m);
    o<<",\"pipelines\":[";
    bool first=true;const auto nowNs=steadyNs();
    for(auto& s:m.slots){const int st=s.status.load(std::memory_order_acquire);if(!st)continue;
        const auto asked=s.requestNs.load(std::memory_order_relaxed);
        o<<(first?"":",")<<"{\"color\":"<<int(s.color)<<",\"depth\":"<<int(s.depth)<<",\"load\":"<<s.load<<",\"status\":"<<st<<",\"buildMs\":"<<(st==2?double(s.buildNs)/1e6:0.0)
         <<",\"ageMs\":"<<(asked&&nowNs>=asked?double(nowNs-asked)/1e6:0.0)<<",\"error\":\""<<s.error.load(std::memory_order_relaxed)<<"\""
         <<",\"arc\":"<<s.arcStatus.load(std::memory_order_relaxed)<<",\"arcError\":\""<<s.arcError.load(std::memory_order_relaxed)<<"\"}";first=false;}
    o<<"]}";
    return o.str();
}
} // namespace rdrvr::vr::hands
