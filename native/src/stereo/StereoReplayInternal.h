#pragma once
// Internals of the stereo replay shared by StereoReplay.cpp (inline replay into the engine's primary command
// buffer) and StereoReplayAsync.cpp (render-pass contents recorded into secondary command buffers).
// Not a public API: include only from those two sources and the host test.
//
// Thread notes for the secondaries path (worker mode, async=2, runs recordJob on a worker thread):
//  [any]    may run off the render thread (takes coldMutex internally where registries are read; writes only the
//           engine uniform ring top region, atomics and its own thread_local caches);
//  [render] render thread only (primary command buffer, thread_local tracked state, engine texture objects
//           +0xa0/+0x98, eye dump).
#include "StereoReplay.h"
#include "CameraMemberScan.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rdrvr::engine::stereo {
namespace internal {
// ---------------------------------------------------------------- counters (instance in StereoReplay.cpp)
struct Stats {
    std::atomic<std::uint64_t> shadersPatched{},shadersDeclined{},shadersNotVertex{},layoutsPatched{},identityPushes{};
    std::atomic<std::uint64_t> viewsTracked{},framebuffersTracked{},framebuffersOwned{},bindlessLayouts{},bindlessSets{},mirroredWrites{},mirroredCopies{},overridesOutOfRange{},overridesUnwritten{},lastBadOverride{},lastUnwrittenOverride{};
    std::atomic<std::uint64_t> recordings{},recordingsInvalid{},replays{},replaysFaulted{},replaysDeclined{},fallbacks{},passesSkipped{},passesReplayed{},framebufferMisses{};
    std::atomic<std::uint64_t> overrideCalls{};
    // debug.rdrvr.stereoreplay.scrub: bookkeeping of
    // the clone slots that hold an override copy, the copies that restore them (revert at the next override call, scrub
    // before the source view is destroyed) and the destroys deferred until their frame retired. Always counted;
    // countOnly* = what scrub=1 would have done while scrub=0.
    struct Scrub {
        std::atomic<std::uint64_t> overrideEntries{},overrideReverts{},overrideScrubs{},staleOverrideViews{},deferredViewDestroys{},deferredViewFlushes{};
        std::atomic<std::uint64_t> staleLive{},staleLiveInWindow{},staleRecent{},entriesMirrorDropped{},entriesUnknownSource{},revertsUnwritten{},scrubsUnwritten{};
        std::atomic<std::uint64_t> deferredFramebufferDestroys{},deferredFramebufferFlushes{},framebuffersDestroyedNow{},teardownViewFlushes{},teardownFramebufferFlushes{},droppedAtDeviceLoss{};
        std::atomic<std::uint64_t> countOnlyReverts{},countOnlyScrubs{},countOnlyDeferrals{},viewDeferUnavailable{},hooks{},frameResets{},maxPending{},lastStaleView{},lastStaleFrame{};
        // Images destroyed while a deferred view of them is pending (deferred with it, forwarded after
        // it); staleEngine*: engine-set elements still holding a view at its destroy (probe only; clone/clone2 mirror them).
        std::atomic<std::uint64_t> deferredImageDestroys{},deferredImageFlushes{},teardownImageFlushes{};
        std::atomic<std::uint64_t> staleEngineViews{},staleEngineSlots{},lastStaleEngineView{},lastStaleEngineSlot{},lastStaleEngineFrame{};
    } scrub;
    std::atomic<std::uint64_t> skipNoFramebuffer{},skipNoView{},skipNoEye{},skipMixed{};
    std::mutex skipMutex;std::array<std::string,6> skipSamples{};unsigned skipNext{};
    std::array<std::string,2> tapeShape{};std::atomic<std::uint64_t> tapeDrawsOutsidePass[2]{},tapeOpenAtEnd[2]{};
    std::atomic<std::uint64_t> passesAutoClosed{},drawsSkippedOutsidePass{},endsSkipped{},faultDisabled{},depthResolves{},depthResolveMisses{};
    std::atomic<std::uint64_t> foreignTargetsLast{},foreignCopiesReplayed{},foreignCopiesSkipped{},foreignBarriersKept{};
    std::atomic<std::uint64_t> maskLast{},maskPassesSkipped{},maskDrawsSkipped{},ownerLast{},startBarriers{},fbCacheHits{};
    struct Probe {std::atomic<std::uint64_t> draws{},withEntries{},noSet0{},noLayout{},wvpMatch{},wvpMismatch{},wvpZero{},wvpMatchPrev{},m3MatchPrev{},wvpMatchOther{},m3MatchOther{},m3Match{},m3Mismatch{},m1Match{},m1Mismatch{},depthL{},depthR{},depthOther{},collL{},collR{},collOther{},lightL{},lightR{},lightOther{};std::string sample;} probe[2];
    std::atomic<std::uint64_t> drawsPatched{},drawsUnpatched{},drawsUnknownPipeline{},drawsUnpatchedPass0{},shadersRegistered{},pipelinesRegistered{};
    std::atomic<std::uint64_t> uboDraws{},uboBinds{},uboBlocks{},uboBytes{},uboFrameInvalid{},uboRingFull{},uboNoLayout{},uboNoDynamic{},uboCursorWrites{},uboCountMismatch{},uboLastOffsets{},uboLastDynamic{};
    std::atomic<std::uint64_t> uboDedupeMismatch{}; // A dedupe hit whose copy had another member offset or was shorter (fresh copy made)
    std::string uboSamples{};std::atomic<bool> uboSampled{};
    std::atomic<std::uint64_t> wvpInvRewritten{},wvpInvLeft{},endBarriers{},prevProjRight{},prevProjLeft{},maxBlockSize{},rageM1AllMatch{},rageM1AllMismatch{},wvpInvRelational{},cacheCleans{};std::string wrapperDump{};std::string pipelineList{};
    std::atomic<std::uint64_t> rageM1Match{},rageM1Transposed{},rageM1Mismatch{},rageM3Match{},rageM3Transposed{},rageM3Mismatch{},wvpInvMatch{},wvpInvMismatch{},cameraMatch{},cameraMismatch{},identityPushesDraw{},uboRingSize{},uboCursorLast{};
    std::string unpatchedSamples{};
    // ReplayRewriter::finish (every colour stage) stores raw rows under skipMutex; report() formats them into
    // pipelineList/unpatchedSamples (formatting on the render thread every frame costs ~0.5 %).
    struct PipeRow {std::uint64_t vsHash{},fsHash{};unsigned draws{},pass{},status{},entries{};bool vsRage{},fsRage{},rewritten{};};
    struct UnpatchedRow {std::uint64_t module{},vsHash{},fsHash{};unsigned status{},members{},set0Bindings{},draws{},words{};};
    std::array<PipeRow,96> pipeRows{};unsigned pipeRowCount{};std::array<UnpatchedRow,8> unpatchedRows{};unsigned unpatchedRowCount{};
    std::array<std::array<std::atomic<std::uint32_t>,33>,2> opCounts{};std::array<std::string,2> foreignSamples{};
    std::atomic<std::uint64_t> replayNs[2]{},replayCommands[2]{},tapeCommands[2]{},tapeBytes[2]{};
    std::atomic<const char*> lastInvalid{"NONE"},lastFallback{"NONE"},lastDecline{"NONE"};
    std::array<std::atomic<std::uint64_t>,12> patchStatus{};
    // Tape-shape sampler per stage (a serial Tape::reset() does not clear; counts only recordings that reach the
    // sampler) and the sampled tape's own size (tapeCommands/tapeBytes are the LAST recording's)
    std::array<std::atomic<std::uint64_t>,2> shapeSerial{},sampledCommands{},sampledBytes{},sampledDraws{},shapeCapped{};
    // Replay barrier filter (emitOutsideOp): calls seen / dropped whole / dropped pure execution barriers, image
    // barriers kept (eye or foreign) / dropped (other images) / layout-preserving (old == new)
    std::atomic<std::uint64_t> barrierCalls{},barrierCallsDropped{},barrierExecOnlyDropped{},imageBarriersKept{},imageBarriersDropped{},imageBarriersNoop{};
    // Render-pass boundary test (STEREO TEST 10/11/12)
    struct Boundary {
        std::atomic<std::uint64_t> framesOn{},framesOff{},extraR{},extraL{},splitsR{},splitsL{};
        std::atomic<std::uint64_t> noTwins{},fewDraws{},inlineSkipped{},leftNotStored{},leftLayout{},leftQuery{},leftContents{},extraLayout{};
        std::atomic<std::uint64_t> twinsCreated{},twinFailures{},passDescs{},tableFull{},twinsDestroyed{};
        std::atomic<unsigned> insertedThisFrame{};                       // boundaries emitted into the current frame (gpuStages slot)
        std::array<std::atomic<std::uint16_t>,2*boundary::prevDrawPasses> prevDraws{}; // [stage][tape pass]: draws replayed last time
        unsigned latchMode{UINT32_MAX};std::uint64_t latchFrames{};       // render thread (latchBoundaryPhase)
        std::atomic<std::uint64_t> mergeAlwaysFrames{};                   // Frames latched with BoundaryMergeR outside STEREO TEST 20
    } boundary;
    // Pass-chain census of the sampled tapes (every 32nd recording of a stage). Sums over samples; divide by samples.
    struct Census {std::atomic<std::uint64_t> samples{},passes{},boundaries{},sameFbLoad{},sameFbClear{},fbSwitch{},attachmentBarrier{},depthWrittenBefore{},draws{},dsWriteDraws{},
        gapBarriers{},gapImageOwn{},gapImageEye{},gapImageOther{},gapImageNoop{},gapMemory{},gapBuffer{},gapCopyBuffer{},gapBufferToImage{},gapImageToBuffer{},
        gapClearColor{},gapClearDepth{},gapFill{},gapUpdate{},gapImageTransfer{},gapState{},gapPush{},
        passStartStencil{},restartNoStencil{},restartUnknown{},restartEmpty{};}; // Same-fb LOAD restarts by the first draw's stencilTestEnable
    std::array<Census,2> census{};
    std::array<std::string,2> passChain{};std::string eyePassDescs{};std::array<VkRenderPass,32> eyePassSeen{};unsigned eyePassCount{}; // skipMutex
};

// ---------------------------------------------------------------- tape format
enum class Op : std::uint16_t {
    BindPipeline,BindDescriptorSets,BindVertexBuffers,BindIndexBuffer,SetViewport,SetScissor,SetLineWidth,SetDepthBias,
    SetBlendConstants,SetDepthBounds,SetStencilCompareMask,SetStencilWriteMask,SetStencilReference,PushConstants,
    BeginRenderPass,EndRenderPass,NextSubpass,Draw,DrawIndexed,DrawIndirect,DrawIndexedIndirect,PipelineBarrier,
    CopyImage,CopyBuffer,CopyBufferToImage,CopyImageToBuffer,BlitImage,ClearAttachments,ClearColorImage,ClearDepthStencilImage,
    ResolveImage,FillBuffer,UpdateBuffer
};
struct Header {std::uint16_t op;std::uint16_t pad;std::uint32_t bytes;};
static_assert(sizeof(Header)==8);
struct BoundSetCall {VkPipelineLayout layout{};VkPipelineBindPoint point{};std::uint32_t count{},offsetCount{};std::array<VkDescriptorSet,8> sets{};std::array<std::uint32_t,64> offsets{};bool valid{};};
struct BoundState {
    VkPipeline pipeline{};VkPipelineBindPoint pipelinePoint{};
    std::array<BoundSetCall,8> setCalls{};
    std::array<VkBuffer,16> vertexBuffers{};std::array<VkDeviceSize,16> vertexOffsets{};std::uint32_t vertexMask{};
    VkBuffer indexBuffer{};VkDeviceSize indexOffset{};VkIndexType indexType{};bool indexKnown{};
    VkViewport viewport{};bool viewportKnown{};VkRect2D scissor{};bool scissorKnown{};
    float depthBias[3]{};bool depthBiasKnown{};float blend[4]{};bool blendKnown{};float bounds[2]{};bool boundsKnown{};float lineWidth{};bool lineWidthKnown{};
    std::array<std::uint32_t,2> stencilRef{},stencilCmp{},stencilWrite{};std::uint32_t stencilRefKnown{},stencilCmpKnown{},stencilWriteKnown{};
};
// payloads
struct PBindPipeline {VkPipelineBindPoint point;VkPipeline pipeline;};
struct PBindSets {VkPipelineBindPoint point;VkPipelineLayout layout;std::uint32_t first,count,offsets;};
struct PBindVertices {std::uint32_t first,count;};
struct PBindIndex {VkBuffer buffer;VkDeviceSize offset;VkIndexType type;};
struct PViewport {std::uint32_t first,count;};
struct PScissor {std::uint32_t first,count;};
struct PFloat1 {float a;};struct PFloat2 {float a,b;};struct PFloat3 {float a,b,c;};struct PFloat4 {float v[4];};
struct PStencil {VkStencilFaceFlags face;std::uint32_t value;};
struct PPush {VkPipelineLayout layout;VkShaderStageFlags stages;std::uint32_t offset,size;};
struct PBeginPass {VkRenderPass pass;VkFramebuffer framebuffer;VkRect2D area;std::uint32_t clears;VkSubpassContents contents;};
struct PSubpass {VkSubpassContents contents;};
struct PDraw {std::uint32_t vertices,instances,firstVertex,firstInstance;};
struct PDrawIndexed {std::uint32_t indices,instances,firstIndex;std::int32_t vertexOffset;std::uint32_t firstInstance;};
struct PIndirect {VkBuffer buffer;VkDeviceSize offset;std::uint32_t count,stride;};
struct PBarrier {VkPipelineStageFlags src,dst;VkDependencyFlags flags;std::uint32_t memory,buffers,images;};
struct PCopyImage {VkImage src;VkImageLayout srcLayout;VkImage dst;VkImageLayout dstLayout;std::uint32_t regions;};
struct PCopyBuffer {VkBuffer src,dst;std::uint32_t regions;};
struct PCopyBufferImage {VkBuffer buffer;VkImage image;VkImageLayout layout;std::uint32_t regions;};
struct PBlit {VkImage src;VkImageLayout srcLayout;VkImage dst;VkImageLayout dstLayout;std::uint32_t regions;VkFilter filter;};
struct PClearAttachments {std::uint32_t attachments,rects;};
struct PClearColor {VkImage image;VkImageLayout layout;VkClearColorValue value;std::uint32_t ranges;};
struct PClearDepth {VkImage image;VkImageLayout layout;VkClearDepthStencilValue value;std::uint32_t ranges;};
struct PResolve {VkImage src;VkImageLayout srcLayout;VkImage dst;VkImageLayout dstLayout;std::uint32_t regions;};
struct PFill {VkBuffer buffer;VkDeviceSize offset,size;std::uint32_t data;};
struct PUpdate {VkBuffer buffer;VkDeviceSize offset,size;};

// ---------------------------------------------------------------- registry value types read by the rewrite
struct ShaderInfo {std::uint32_t status{},members{},set0Bindings{};ShaderUniformScan scan{};std::uint64_t hash{};std::uint32_t words{};};
struct PipelineInfo {VkShaderModule module{};ShaderInfo vs{};std::array<CameraMemberEntry,16> entries{};unsigned entryCount{};bool vsRage{},fsRage{};VkShaderModule fsModule{};std::uint64_t fsHash{};
    bool writesDepth{},writesStencil{};   // pDepthStencilState (depth test+write enabled / stencil test with a non-KEEP op)
    bool testsStencil{};};                // pDepthStencilState->stencilTestEnable (the BindDepthStencilState restart rule)
// Left game set -> bindless clone of the right-target owner (snapshot per replay).
struct SetMap {std::array<std::pair<VkDescriptorSet,VkDescriptorSet>,16> pairs{};unsigned count{};
    VkDescriptorSet operator()(VkDescriptorSet s)const{for(unsigned i=0;i<count;++i)if(pairs[i].first==s)return pairs[i].second;return s;}};
}
class Tape {
public:
    static constexpr std::size_t capacity=24u<<20; // bytes
    std::vector<std::uint8_t> bytes;std::size_t used{};unsigned commands{};
    unsigned draws{}; // Draw/DrawIndexed/indirect ops recorded (diagnostic, read via lastTapeDraws)
    bool invalid{},active{};const char* invalidReason{"NONE"};
    VkCommandBuffer command{};
    internal::BoundState prologue{};
    std::array<std::pair<std::uintptr_t,std::uint32_t>,16> stockLayouts{};unsigned stockLayoutCount{};
    std::uint64_t beganNs{},endedNs{};
    // Streaming (worker mode): while a job reads this tape during the recording, ops below `published` are complete
    // (release/acquire; the storage never moves once reserved, so the reader is never invalidated by later pushes).
    // Render-thread-only fields: publish every `publishEvery` draws/clears and at pass ends; wake a
    // sleeping worker on every `wakeEvery`-th publication (0: never mid-stage, the worker polls).
    std::atomic<bool> streaming{};unsigned sinceNotify{},publishEvery{1},wakeEvery{},sinceWake{};
    // Tape offsets of the CopyImage/BlitImage/ResolveImage ops (render thread), so scanForeignTargets visits
    // only those instead of every op header of the tape; overflow falls back to the full walk.
    std::array<std::uint32_t,64> imageCopies{};unsigned imageCopyCount{};bool imageCopiesOverflow{};
    // The only field the worker reads while the render thread records sits alone on its cache line (the
    // render thread writes used/commands/draws on every push; the worker must not pull that line per op).
    alignas(64) std::atomic<std::size_t> published{};char publishedLine[64-sizeof(std::atomic<std::size_t>)]{};
    void noteImageCopy(std::size_t at){if(imageCopyCount<imageCopies.size())imageCopies[imageCopyCount++]=std::uint32_t(at);else imageCopiesOverflow=true;}
    void reset(){used=0;commands=0;draws=0;invalid=false;invalidReason="NONE";command={};stockLayoutCount=0;active=false;imageCopyCount=0;imageCopiesOverflow=false;
        streaming.store(false,std::memory_order_relaxed);published.store(0,std::memory_order_relaxed);sinceNotify=0;sinceWake=0;}
    bool reserveStorage(){if(bytes.empty()){try{bytes.resize(capacity);}catch(...){fail("TAPE_ALLOC");return false;}}return true;}
    void fail(const char* why){if(!invalid){invalid=true;invalidReason=why;}}
    template<class T> T* push(internal::Op op,std::size_t extra){
        const std::size_t payload=(sizeof(T)+extra+7)&~std::size_t(7);
        if(invalid)return nullptr;
        if(bytes.empty()){try{bytes.resize(capacity);}catch(...){fail("TAPE_ALLOC");return nullptr;}}
        if(used+sizeof(internal::Header)+payload>bytes.size()){fail("TAPE_OVERFLOW");return nullptr;}
        auto* h=reinterpret_cast<internal::Header*>(bytes.data()+used);h->op=std::uint16_t(op);h->pad=0;h->bytes=std::uint32_t(payload);
        auto* p=reinterpret_cast<T*>(bytes.data()+used+sizeof(internal::Header));
        used+=sizeof(internal::Header)+payload;++commands;return p;
    }
};

namespace internal {
// Engine frame uniform ring as seen at replay start (context+0x30a0 -> frame; +0x98 wrapper, +0xa0 mapped, +0xa8 cursor).
struct RingSnapshot {bool uboEnabled{};std::uintptr_t framePtr{},ringMapped{};std::uint64_t ringCursor{};std::uint32_t ringSize{};bool ringValid{};};
// Scratch images the LEFT stage filled from eye images (refilled from the right eye by the replay).
struct ForeignTargets {std::array<VkImage,32> images{};unsigned count{};
    bool contains(VkImage i)const noexcept{for(unsigned k=0;k<count;++k)if(images[k]==i)return true;return false;}};
struct Set0Bind {VkPipelineLayout layout{};VkDescriptorSet set{};std::array<std::uint32_t,16> offsets{};unsigned count{};bool valid{};};
// Worker mode (async=2): one top-down region of the engine frame ring per frame, shared by the depth and color jobs
// and by any inline replay of the same frame (allocations are lock-free; floor = engine cursor at launch + 1 MB).
struct RingRegion {std::uintptr_t framePtr{},mapped{};std::uint32_t size{};std::atomic<std::uint64_t> top{};std::uint64_t floor{},topInit{};
    // Live overlap guard: the engine keeps allocating this frame's ring bottom-up while jobs write right-eye blocks
    // top-down. Every allocation re-reads the engine cursor (framePtr+0xa8); once it passes `floor` the region is
    // under pressure and refuses every further allocation (no right-eye block is ever written below floor).
    std::atomic<bool> pressure{};
    std::uint64_t liveCursor() const noexcept {return *reinterpret_cast<const volatile std::uint64_t*>(framePtr+0xa8);}
};
// Defaults reproduce the inline/secondaries (async 0/1) behavior exactly.
struct RewriteOptions {
    RingRegion* region{};   // nullptr: the render thread's own per-frame region (thread_local cursor)
    bool ownInfos{};        // copy PipelineInfo under coldMutex into the job instead of pointing into the registry
    bool diagnostics{true}; // one-time sample strings in stats (render thread only)
};

// Per-replay right-eye uniform rewrite and draw classification, shared by both replay paths [any].
// Set 0 binds (dynamic uniform offsets) are deferred to the draw, where the camera-dependent blocks of the bound
// pipeline are copied into the top region of the engine frame ring and rewritten for the right eye; the 64-byte
// vertex push constant toggles between the clip-space correction and identity. Every emitting member takes the
// command buffer to record into (the primary for the inline path, the pass's secondary for the secondaries path).
struct ReplayRewriter {
    ReplayRewriter(Stage stage,const FrameMaps& maps,unsigned mask,VkPipelineLayout layout,const SetMap* sets,const RingSnapshot& ring,const RewriteOptions& options=RewriteOptions{}) noexcept;
    ReplayRewriter(const ReplayRewriter&)=delete;ReplayRewriter& operator=(const ReplayRewriter&)=delete;
    void bindPipeline(VkPipelineBindPoint point,VkPipeline pipeline);   // logical state; the caller records the bind
    void bindSets(VkCommandBuffer c,const PBindSets* r);                 // c==VK_NULL_HANDLE: logical state only
    bool classifyDraw(unsigned passIndex) noexcept;                      // true: skip the draw (live mask)
    void rewriteForDraw(VkCommandBuffer c);
    void targetRestarted() noexcept;  // c is a fresh secondary: set 0 must be re-emitted, its push constant is the correction
    void finish() noexcept;           // ring cursor, prevRelClipL, color-stage pipeline/draw statistics
    void threadEntered() noexcept;    // a job continues on this thread: drop this thread's stale layout cache
    // inputs
    Stage stage;const FrameMaps& maps;unsigned mask;VkPipelineLayout layout;const SetMap* sets;
    struct DedupeHit {std::uint32_t dst{},memberOffset{},bytes{};};
    std::unordered_map<std::uint64_t,DedupeHit> dedupe;
    bool uboEnabled;std::uintptr_t framePtr,ringMapped;std::uint64_t ringCursor;std::uint32_t ringSize;bool ringValid;
    std::uint32_t align{};std::uint64_t floorCursor{};
    bool transposedMode{};unsigned checks{};
    float relLInv[16]{},K_LtoR[16]{},G_RtoL[16]{};bool relationValid{};unsigned m1Mismatches{};
    std::array<float,16> relClipRInv{};bool relClipRInvValid{};
    RingRegion* region{};bool ownInfos{},diagnostics{true};std::unordered_map<VkPipeline,PipelineInfo> ownedInfos; // RewriteOptions
    unsigned regionAllocs{}; // The live guard re-reads the engine cursor on every 8th region allocation
    // logical bound state of the replayed stream
    VkPipeline boundPipeline{};const PipelineInfo* boundInfo{};
    Set0Bind set0{};std::array<std::uint32_t,16> emittedOffsets{};bool emittedValid{},pushIdentity{},set0Pending{};
    // statistics
    std::uint64_t drawsPatched{},drawsUnpatched{},drawsUnknown{},drawsUnpatchedPass0{};
    struct PipeSample {const PipelineInfo* info;unsigned draws,pass;bool rewritten;};std::array<PipeSample,96> pipes{};unsigned pipeCount{};
    struct UnpatchedSample {VkShaderModule module;ShaderInfo vs;unsigned draws;std::uint64_t fsHash;};std::array<UnpatchedSample,8> unpatched{};unsigned unpatchedCount{};
private:
    VkDescriptorSet substituteSet(VkDescriptorSet s)const noexcept{return (*sets)(s);}
    const std::vector<std::uint32_t>* dynFor(VkPipelineLayout l);
    const PipelineInfo* infoFor(VkPipeline pipeline);
    std::uint32_t alloc(std::uint32_t bytes) noexcept;
    void writeMat(std::uintptr_t at,const float* m) noexcept;
    std::uint32_t rageBlock(std::uint32_t src,std::uint32_t blockSize);
    std::uint32_t blockCopy(std::uint32_t src,std::uint32_t memberOffset,CameraMember kind,std::uint32_t blockSize);
    void ensurePush(VkCommandBuffer c,bool identity) noexcept;
    void emitSet0(VkCommandBuffer c,const std::array<std::uint32_t,16>& offs) noexcept;
};

// ---- StereoReplay.cpp helpers used by both paths ----
const RawCommands& rawCommands() noexcept;
Stats& replayStats() noexcept;
std::uint64_t replayClockNs() noexcept;
const BoundState* trackedState(VkCommandBuffer c) noexcept;                                    // [render]
RingSnapshot snapshotRing(const FrameMaps& maps,unsigned mask) noexcept;                       // [render] reads the engine frame
ForeignTargets scanForeignTargets(const Tape& t,const FrameMaps& maps,Stage stage);             // [any]
int resolveRightFramebuffer(const FrameMaps& maps,VkRenderPass pass,VkFramebuffer left,VkFramebuffer& out); // [any] 0 skip pass, 1 ok, 2 failure
void emitBoundState(VkCommandBuffer c,const BoundState& s,const SetMap* sets) noexcept;          // [any] sets==nullptr: no substitution
// Transfer/barrier ops of the tape (barrier, copies, blit, resolve, clear images, fill/update), with eye-image
// substitution and foreign scratch targets, exactly as the inline replay emits them.                                  [any]
void emitOutsideOp(VkCommandBuffer c,Op op,const std::uint8_t* payload,const FrameMaps& maps,const ForeignTargets& foreign);
// Start memory barrier + correction push + test clears (mask 25/26).                                                  [render]
void emitReplayHead(VkCommandBuffer c,Stage stage,const FrameMaps& maps,VkPipelineLayout layout,unsigned mask) noexcept;
// Identity push, engine state restore, +0xa0 tracked layouts, end barrier, depth resolve, eye dump, magenta test.      [render]
void emitReplayTail(VkCommandBuffer c,Stage stage,const FrameMaps& maps,VkPipelineLayout layout,unsigned mask,const BoundState& before,const Tape& t,bool dumpNow) noexcept;
// Boundary emission (StereoBoundary.h): the memory dependency between a pass end and a twin that loads its
// attachments (colour/depth writes -> attachment reads/writes; no image barrier, the layouts already match), and an
// empty twin begun and ended right after its pass (clearValueCount 0: the empty twin has no CLEAR).           [any]
void emitBoundaryBarrier(VkCommandBuffer c) noexcept;
void emitEmptyTwin(VkCommandBuffer c,VkRenderPass empty,VkFramebuffer fb,const VkRect2D& area) noexcept;
unsigned boundaryRightFlags() noexcept;                                                         // this frame's BoundaryExtraR/SplitR
bool pollEyeDump();                                                                             // [render]
Tape& tapeFor(Stage stage) noexcept;                                                            // [render]
VkPipelineLayout replayPushLayout(VkDevice device) noexcept;                                    // [render] (thread_local cache)
void snapshotSets(VkDevice device,unsigned owner,SetMap& out);                                  // [any] coldMutex
void resetLegacyRing() noexcept;                                                                // [render] drop the thread_local region cursor

// ---- StereoReplayAsync.cpp ----
enum class SecondaryOutcome {Done,Declined,Faulted};
// Declined: nothing was recorded into the primary; the caller replays inline. Faulted: the primary holds a partial
// replay (state already restored); the caller cancels the frame like an inline fault.
SecondaryOutcome replaySecondaries(Stage stage,VkCommandBuffer primary,const FrameMaps& maps,VkPipelineLayout layout,const SetMap& sets,const Tape& t) noexcept;
void secondaryShutdownAfterIdle(VkDevice device) noexcept;
void secondaryDeviceDestroyed(VkDevice device) noexcept;
// Worker mode (async=2). Jobs are launched by GameXr right after endRecording(stage) with predicted FrameMaps.
bool frameJobsActive() noexcept;                              // a worker-mode frame context exists (jobs of this frame)
RingRegion* sharedRegionFor(const RingSnapshot& ring) noexcept; // the frame's shared region when it covers this ring
SecondaryOutcome replayLaunched(Stage stage,VkCommandBuffer primary,const FrameMaps& maps,VkPipelineLayout layout,const SetMap& sets,const Tape& t) noexcept;
void waitTapeIdle(Stage stage) noexcept;                      // beginRecording: no job may still read this tape
void tapeProgress() noexcept;                                 // [render] new ops were published on a streaming tape
void tapeFinished(Stage stage) noexcept;                      // [render] end of endRecording: the streaming job's tape is complete
// Diagnostics: render-thread CPU / context switches per left stage (beginRecording/endRecording, every mode)
// and render-thread waits on coldMutex (registry hooks that found it held, e.g. by the worker).
void noteRenderStage(Stage stage,bool begin) noexcept;        // [render]
void noteColdWait() noexcept;                                 // [render]
void noteRenderCpu() noexcept;                                // [render] streaming publication: the render thread's current CPU
}
}
