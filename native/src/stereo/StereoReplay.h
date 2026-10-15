#pragma once
// Stereo replay: the RIGHT eye is produced by replaying the Vulkan commands
// the engine recorded for the LEFT depth and color stages, with
//  - framebuffers/images of the stock (left) targets replaced by the private
//    right-eye targets,
//  - the bindless texture descriptor set replaced by a mirrored clone whose
//    eye-specific slots (sampled depth, light accumulation, shadow collector)
//    point at the right-eye textures,
//  - a 64-byte vertex push constant holding the clip-space correction
//    C = relClipR * T(cL-cR) * inverse(relClipL) (see EyePositionPatch.h).
// No native DrawCommon/ShaderDraw executes for the right eye. The lighting
// stage keeps its native per-eye execution (it is cheap and needs per-eye
// inverse matrices in effect-local constants).
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>   // ensureDummyDescriptor's queue mutex
#include <string>
#include "StereoBoundary.h"
#include "GpuLite.h" // gpuLiteArmed + the lite frame-span readers (defined in StereoGpuStages.inc)

namespace rdrvr::engine::stereo {
enum class Stage : unsigned {Depth=0,Color=1};
struct RawCommands {
    PFN_vkCmdBindPipeline bindPipeline{};PFN_vkCmdBindDescriptorSets bindDescriptorSets{};
    PFN_vkCmdBindVertexBuffers bindVertexBuffers{};PFN_vkCmdBindIndexBuffer bindIndexBuffer{};
    PFN_vkCmdSetViewport setViewport{};PFN_vkCmdSetScissor setScissor{};PFN_vkCmdSetLineWidth setLineWidth{};
    PFN_vkCmdSetDepthBias setDepthBias{};PFN_vkCmdSetBlendConstants setBlendConstants{};PFN_vkCmdSetDepthBounds setDepthBounds{};
    PFN_vkCmdSetStencilCompareMask setStencilCompareMask{};PFN_vkCmdSetStencilWriteMask setStencilWriteMask{};PFN_vkCmdSetStencilReference setStencilReference{};
    PFN_vkCmdPushConstants pushConstants{};PFN_vkCmdBeginRenderPass beginRenderPass{};PFN_vkCmdEndRenderPass endRenderPass{};PFN_vkCmdNextSubpass nextSubpass{};
    PFN_vkCmdDraw draw{};PFN_vkCmdDrawIndexed drawIndexed{};PFN_vkCmdDrawIndirect drawIndirect{};PFN_vkCmdDrawIndexedIndirect drawIndexedIndirect{};
    PFN_vkCmdPipelineBarrier pipelineBarrier{};PFN_vkCmdCopyImage copyImage{};PFN_vkCmdCopyBuffer copyBuffer{};
    PFN_vkCmdCopyBufferToImage copyBufferToImage{};PFN_vkCmdCopyImageToBuffer copyImageToBuffer{};PFN_vkCmdBlitImage blitImage{};
    PFN_vkCmdClearAttachments clearAttachments{};PFN_vkCmdClearColorImage clearColorImage{};PFN_vkCmdClearDepthStencilImage clearDepthStencilImage{};
    PFN_vkCmdResolveImage resolveImage{};PFN_vkCmdFillBuffer fillBuffer{};PFN_vkCmdUpdateBuffer updateBuffer{};
    PFN_vkCreatePipelineLayout createPipelineLayout{};PFN_vkDestroyPipelineLayout destroyPipelineLayout{};
    PFN_vkCreateFramebuffer createFramebuffer{};PFN_vkDestroyFramebuffer destroyFramebuffer{};
    PFN_vkCreateDescriptorPool createDescriptorPool{};PFN_vkDestroyDescriptorPool destroyDescriptorPool{};
    PFN_vkAllocateDescriptorSets allocateDescriptorSets{};PFN_vkFreeDescriptorSets freeDescriptorSets{};
    PFN_vkUpdateDescriptorSets updateDescriptorSets{};
    // Secondary-command-buffer replay (debug.rdrvr.stereoreplay.async=1) only. Deliberately NOT part of
    // operator bool: a missing slot declines the secondaries path, never the inline replay.
    PFN_vkCmdExecuteCommands executeCommands{};
    explicit operator bool()const noexcept{
        return bindPipeline&&bindDescriptorSets&&bindVertexBuffers&&bindIndexBuffer&&setViewport&&setScissor&&setLineWidth&&setDepthBias&&
            setBlendConstants&&setDepthBounds&&setStencilCompareMask&&setStencilWriteMask&&setStencilReference&&pushConstants&&beginRenderPass&&
            endRenderPass&&nextSubpass&&draw&&drawIndexed&&drawIndirect&&drawIndexedIndirect&&pipelineBarrier&&copyImage&&copyBuffer&&
            copyBufferToImage&&copyImageToBuffer&&blitImage&&clearAttachments&&clearColorImage&&clearDepthStencilImage&&resolveImage&&
            fillBuffer&&updateBuffer&&createPipelineLayout&&destroyPipelineLayout&&createFramebuffer&&destroyFramebuffer&&
            createDescriptorPool&&destroyDescriptorPool&&allocateDescriptorSets&&freeDescriptorSets&&updateDescriptorSets;
    }
};
// ---- cold configuration (TraceEntry) ----
bool bindRaw(const RawCommands&) noexcept;         // once, before the dispatch slots are published
bool enabled() noexcept;                           // debug.rdrvr.stereoreplay != "0"
void deviceCreated(VkDevice,VkPhysicalDevice) noexcept;
void deviceDestroyed(VkDevice) noexcept;           // no Vulkan calls; owner already idle-destroyed
void shutdownAfterIdle(VkDevice) noexcept;         // destroy owned pools/framebuffers/layouts
// ---- creation observers (TraceEntry wrappers) ----
// Vertex shader patch: on success *patched points at owned storage valid until
// the next call on this thread; returns false to use the original info.
bool patchShaderCreate(const VkShaderModuleCreateInfo* original,const VkShaderModuleCreateInfo** patched) noexcept;
// Adds the eye push-constant range; *patched valid until next call on this thread.
bool patchPipelineLayoutCreate(const VkPipelineLayoutCreateInfo* original,const VkPipelineLayoutCreateInfo** patched) noexcept;
void imageViewCreated(VkDevice,const VkImageViewCreateInfo*,VkImageView,VkResult) noexcept;
// Called BEFORE the driver destroys the view (TraceEntry stereoDestroyView). Clone slots that still hold an
// override copy of this view get the engine's own element back (scrub) and, with debug.rdrvr.stereoreplay.scrub != 0,
// the real destroy is deferred when the view was an override source within the last 2 frames or an attachment of a
// mod-owned right framebuffer. Returns true when the module took the destroy over: it calls destroy(device, view,
// allocator) itself once the frame retired (frameHook) or at shutdownAfterIdle. A null destroy never defers.
bool imageViewDestroyed(VkDevice,VkImageView,const VkAllocationCallbacks* allocator=nullptr,PFN_vkDestroyImageView destroy=nullptr) noexcept;
void imageDestroyed(VkDevice,VkImage) noexcept;
// Called by TraceEntry destroyImage (after imageDestroyed, before the driver). The driver's End walk
// also reads a view's image ([view+0xf0] -> [image+0x148]) and vkCreateImageView takes no reference on it, so an image
// with a deferred view still pending is deferred too: true = taken over, destroy(device, image, allocator) is called once
// its views were forwarded and its stamp (the latest of those views) retired, or at shutdownAfterIdle. Memory is not
// deferred (the engine frees it at once).
bool imageDestroyDeferred(VkDevice,VkImage,const VkAllocationCallbacks* allocator,PFN_vkDestroyImage destroy) noexcept;
// GameXr world frame (render thread), before the eye targets are prepared: frame = plan.sceneId (renderFrameId),
// retired = passCapture.retiredFrame() (the watermark of the foveation variant LRU). Deferred destroys are stamped with the
// latest frameHook value and released once stamp + 2 <= retired (the foveation rule). Re-reads the scrub property.
void frameHook(std::uint64_t frame,std::uint64_t retired) noexcept;
void setScrubForTests(int mode) noexcept; // -1: debug.rdrvr.stereoreplay.scrub again (unset = 1)
unsigned deferredPending() noexcept;     // deferred view + framebuffer + image destroys not yet forwarded to the driver
void framebufferCreated(VkDevice,const VkFramebufferCreateInfo*,VkFramebuffer,VkResult) noexcept;
void framebufferDestroyed(VkDevice,VkFramebuffer) noexcept;
// Maps a pass-through attachment view of a recorded LEFT framebuffer (an unregistered view: the foveation map
// appended by the VRS/FDM backends) to the view the resolved RIGHT framebuffer uses (the right-eye map of the same
// level). Called under the replay's cold mutex: must be lock-free. Null = identity.
using PassThroughTranslator=VkImageView(*)(VkImageView) noexcept;
void setPassThroughTranslator(PassThroughTranslator) noexcept;
void descriptorLayoutCreated(VkDevice,const VkDescriptorSetLayoutCreateInfo*,VkDescriptorSetLayout,VkResult) noexcept;
void descriptorLayoutDestroyed(VkDevice,VkDescriptorSetLayout) noexcept;
void descriptorSetsAllocated(VkDevice,const VkDescriptorSetAllocateInfo*,VkDescriptorSet*,VkResult) noexcept;
void descriptorSetsFreed(VkDevice,VkDescriptorPool,std::uint32_t,const VkDescriptorSet*) noexcept;
void descriptorPoolReset(VkDevice,VkDescriptorPool) noexcept;
void descriptorPoolDestroyed(VkDevice,VkDescriptorPool) noexcept;
// Forwards to the original after appending mirrored writes/copies for clones.
void updateDescriptorSets(VkDevice,std::uint32_t,const VkWriteDescriptorSet*,std::uint32_t,const VkCopyDescriptorSet*) noexcept;
// After a successful native vkBeginCommandBuffer: identity eye correction.
void commandBegun(VkDevice,VkCommandBuffer) noexcept;
// ---- hot recording path (dispatch wrappers) ----
class Tape;
struct Recording {Tape* tape{};VkCommandBuffer command{};};
namespace internal {struct BoundState;struct TrackedSet;struct DynCache;}
// ALL per-thread state of the stereo dispatch hooks (and of the replay rewriter on this thread) in ONE
// constant-initialized, trivially destructible thread_local, so a hook resolves TLS once (one TLSDESC sequence) and
// never runs a TLS init/guard. The large or non-trivial parts live behind pointers (heap, allocated on first use
// by that thread, never freed: the render thread and the stereo worker live as long as the process).
struct ThreadHot {
    Recording recording{};                        // tape being recorded on this thread (beginRecording..endRecording)
    VkCommandBuffer trackedCommand{};             // last command buffer resolved by the bound-state tracker ...
    internal::BoundState* trackedState{};         // ... its state (entries[trackedSlot] of trackedSet)
    internal::TrackedSet* trackedSet{};           // 4 tracked command buffers (LRU, ~13 KB)
    unsigned trackedSlot{};
    PFN_vkCmdBindVertexBuffers vertexNext{};      // IndexedDispatch: the next of the stereo-recorded vertex bind
    // replay rewriter (inline replay / takeover on this thread)
    std::uintptr_t topFrame{};std::uint64_t topCursor{};
    std::array<float,16> prevRelClipL{};
    internal::DynCache* dynCache{};unsigned dynCacheGeneration{UINT32_MAX};
    // STEREO TEST 12: the left-eye engine pass open on the recorded command buffer (Begin hook, after the
    // foveation substitution) and its split point; `passes` counts the stage's Begins (= tape pass index).
    struct BoundaryHot {VkCommandBuffer c{};VkRenderPass tail{},empty{};VkFramebuffer fb{};VkRect2D area{};unsigned draws{},splitAt{},queries{},passes{};bool done{},queryWait{};} boundary{};
};
#if defined(_MSC_VER) && !defined(__clang__)
inline thread_local ThreadHot threadHot{}; // host test build: MSVC rejects constinit on thread_local (C2127); still constant-initialized
#else
inline constinit thread_local ThreadHot threadHot{};
#endif
inline Tape* recordingFor(const ThreadHot& h,VkCommandBuffer c) noexcept {
    const auto& r=h.recording;return r.tape&&(r.command==c||!r.command)?r.tape:nullptr;
}
inline Tape* recordingFor(VkCommandBuffer c) noexcept {return recordingFor(threadHot,c);}
// Combined record (when this thread records a tape for c) + bound-state tracking: one TLS resolve per hook.
void hookBindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline) noexcept;
void hookBindDescriptorSets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,std::uint32_t,std::uint32_t,const VkDescriptorSet*,std::uint32_t,const std::uint32_t*) noexcept;
void hookBindIndexBuffer(VkCommandBuffer,VkBuffer,VkDeviceSize,VkIndexType) noexcept;
// records + tracks, then calls threadHot.vertexNext (set by the IndexedDispatch forwarder on this thread)
VKAPI_ATTR void VKAPI_CALL hookBindVertexBuffers(VkCommandBuffer,std::uint32_t,std::uint32_t,const VkBuffer*,const VkDeviceSize*) noexcept;
void recordBindPipeline(Tape&,VkCommandBuffer,VkPipelineBindPoint,VkPipeline) noexcept;
void recordBindDescriptorSets(Tape&,VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,std::uint32_t,std::uint32_t,const VkDescriptorSet*,std::uint32_t,const std::uint32_t*) noexcept;
void recordBindVertexBuffers(Tape&,VkCommandBuffer,std::uint32_t,std::uint32_t,const VkBuffer*,const VkDeviceSize*) noexcept;
void recordBindIndexBuffer(Tape&,VkCommandBuffer,VkBuffer,VkDeviceSize,VkIndexType) noexcept;
void recordSetViewport(Tape&,VkCommandBuffer,std::uint32_t,std::uint32_t,const VkViewport*) noexcept;
void recordSetScissor(Tape&,VkCommandBuffer,std::uint32_t,std::uint32_t,const VkRect2D*) noexcept;
void recordSetLineWidth(Tape&,VkCommandBuffer,float) noexcept;
void recordSetDepthBias(Tape&,VkCommandBuffer,float,float,float) noexcept;
void recordSetBlendConstants(Tape&,VkCommandBuffer,const float*) noexcept;
void recordSetDepthBounds(Tape&,VkCommandBuffer,float,float) noexcept;
void recordSetStencilCompareMask(Tape&,VkCommandBuffer,VkStencilFaceFlags,std::uint32_t) noexcept;
void recordSetStencilWriteMask(Tape&,VkCommandBuffer,VkStencilFaceFlags,std::uint32_t) noexcept;
void recordSetStencilReference(Tape&,VkCommandBuffer,VkStencilFaceFlags,std::uint32_t) noexcept;
void recordPushConstants(Tape&,VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,std::uint32_t,std::uint32_t,const void*) noexcept;
void recordBeginRenderPass(Tape&,VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents) noexcept;
void recordEndRenderPass(Tape&,VkCommandBuffer) noexcept;
void recordNextSubpass(Tape&,VkCommandBuffer,VkSubpassContents) noexcept;
// EYE CULL B: pad = the op header's eye bits (EyeCullTape.h); true = the draw is in the tape (the left eye may then skip it)
bool recordDraw(Tape&,VkCommandBuffer,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t,std::uint16_t pad=0) noexcept;
bool recordDrawIndexed(Tape&,VkCommandBuffer,std::uint32_t,std::uint32_t,std::uint32_t,std::int32_t,std::uint32_t,std::uint16_t pad=0) noexcept;
void recordDrawIndirect(Tape&,VkCommandBuffer,VkBuffer,VkDeviceSize,std::uint32_t,std::uint32_t) noexcept;
void recordDrawIndexedIndirect(Tape&,VkCommandBuffer,VkBuffer,VkDeviceSize,std::uint32_t,std::uint32_t) noexcept;
void recordPipelineBarrier(Tape&,VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,std::uint32_t,const VkMemoryBarrier*,std::uint32_t,const VkBufferMemoryBarrier*,std::uint32_t,const VkImageMemoryBarrier*) noexcept;
void recordCopyImage(Tape&,VkCommandBuffer,VkImage,VkImageLayout,VkImage,VkImageLayout,std::uint32_t,const VkImageCopy*) noexcept;
void recordCopyBuffer(Tape&,VkCommandBuffer,VkBuffer,VkBuffer,std::uint32_t,const VkBufferCopy*) noexcept;
void recordCopyBufferToImage(Tape&,VkCommandBuffer,VkBuffer,VkImage,VkImageLayout,std::uint32_t,const VkBufferImageCopy*) noexcept;
void recordCopyImageToBuffer(Tape&,VkCommandBuffer,VkImage,VkImageLayout,VkBuffer,std::uint32_t,const VkBufferImageCopy*) noexcept;
void recordBlitImage(Tape&,VkCommandBuffer,VkImage,VkImageLayout,VkImage,VkImageLayout,std::uint32_t,const VkImageBlit*,VkFilter) noexcept;
void recordClearAttachments(Tape&,VkCommandBuffer,std::uint32_t,const VkClearAttachment*,std::uint32_t,const VkClearRect*) noexcept;
void recordClearColorImage(Tape&,VkCommandBuffer,VkImage,VkImageLayout,const VkClearColorValue*,std::uint32_t,const VkImageSubresourceRange*) noexcept;
void recordClearDepthStencilImage(Tape&,VkCommandBuffer,VkImage,VkImageLayout,const VkClearDepthStencilValue*,std::uint32_t,const VkImageSubresourceRange*) noexcept;
void recordResolveImage(Tape&,VkCommandBuffer,VkImage,VkImageLayout,VkImage,VkImageLayout,std::uint32_t,const VkImageResolve*) noexcept;
void recordFillBuffer(Tape&,VkCommandBuffer,VkBuffer,VkDeviceSize,VkDeviceSize,std::uint32_t) noexcept;
void recordUpdateBuffer(Tape&,VkCommandBuffer,VkBuffer,VkDeviceSize,VkDeviceSize,const void*) noexcept;
void recordUnsupported(Tape&,VkCommandBuffer,const char* what) noexcept; // invalidates the tape
// Bound-state tracking (always on, cheap): consulted at replay start/end so the
// engine's cached binding state is re-established after the replay's commands.
void trackBindPipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline) noexcept;
void trackBindDescriptorSets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,std::uint32_t,std::uint32_t,const VkDescriptorSet*,std::uint32_t,const std::uint32_t*) noexcept;
void trackBindVertexBuffers(VkCommandBuffer,std::uint32_t,std::uint32_t,const VkBuffer*,const VkDeviceSize*) noexcept;
void trackBindIndexBuffer(VkCommandBuffer,VkBuffer,VkDeviceSize,VkIndexType) noexcept;
void trackSetViewport(VkCommandBuffer,std::uint32_t,std::uint32_t,const VkViewport*) noexcept;
void trackSetScissor(VkCommandBuffer,std::uint32_t,std::uint32_t,const VkRect2D*) noexcept;
void trackSetDepthBias(VkCommandBuffer,float,float,float) noexcept;
void trackSetStencilReference(VkCommandBuffer,VkStencilFaceFlags,std::uint32_t) noexcept;
void trackSetStencilCompareMask(VkCommandBuffer,VkStencilFaceFlags,std::uint32_t) noexcept;
void trackSetStencilWriteMask(VkCommandBuffer,VkStencilFaceFlags,std::uint32_t) noexcept;
void trackSetBlendConstants(VkCommandBuffer,const float*) noexcept;
void trackSetDepthBounds(VkCommandBuffer,float,float) noexcept;
void trackSetLineWidth(VkCommandBuffer,float) noexcept;
// ---- frame integration (GameXr, render thread) ----
struct EyeImagePair {
    VkImage left{},right{};
    std::uintptr_t leftTexture{},rightTexture{}; // engine VkTexture objects (tracked layout +0xa0, data +0x98)
    unsigned role{};                              // EngineTargetRole
    VkExtent3D extent{};VkFormat format{};         // of the right image (resolve copy)
};
struct FrameMaps {
    std::array<EyeImagePair,8> images{};unsigned imageCount{};
    void* context{};                               // engine VkContext (frame uniform ring at context+0x30a0)
    std::array<float,16> relClip[2]{},camera[2]{};  // engine viewport matrices per eye (left,right), engine layout
    std::uint32_t uniformAlignment{256};
    unsigned owner{};                              // right-target owner index (0/1): selects the bindless clone
    std::array<float,16> correction{};            // clipR = correction * clipL, column-major
    VkDevice device{};
    bool valid{};
    // Secondaries path (StereoReplayAsync): queue family of the engine's primary command buffers (secondary
    // pools must match it) and a per-frame serial (pool reset once per frame and owner).
    std::uint32_t queueFamily{UINT32_MAX};
    std::uint64_t frameSerial{};
};
// Recording window around the native LEFT stage. The stock texture objects are
// read at endRecording for tracked-layout snapshots.
void beginRecording(Stage) noexcept;
void endRecording(Stage,const std::uintptr_t* stockTextures,unsigned count) noexcept;
bool tapeReady(Stage) noexcept;
unsigned tapeCommands(Stage) noexcept;
// Diagnostic accessor: Draw/DrawIndexed/indirect ops of the last COMPLETED LEFT tape of this stage
// (written at endRecording on the render thread; UINT32_MAX when that tape was invalid, 0 before the first).
std::uint32_t lastTapeDraws(Stage) noexcept;
// Replay into the current (closed-pass) command buffer. Returns false when the
// replay could not start; the caller falls back to the native path. A failure
// after the first emitted command is reported through replayFaulted() and the
// frame must be cancelled by the caller.
bool replay(Stage,VkCommandBuffer,const FrameMaps&) noexcept;
bool replayFaulted() noexcept;
// ---- worker recording (debug.rdrvr.stereoreplay.async=2 / STEREO TEST "ASYNC WORKER"; StereoReplayAsync.cpp) ----
// GameXr, render thread. A job per stage is launched at beginRecording(stage) (streaming) or endRecording(stage) with the FrameMaps
// predicted for this frame (retired owner, its previous maps, this frame's eye matrices); the worker records its
// secondaries while the render thread continues; replay() splices it (takeover/wait/inline fallback).
bool launchWanted() noexcept;                                                    // worker mode active
// streaming: launched right after beginRecording(stage); the worker records each op as soon as the render thread has
// recorded it (endRecording finalizes the job). Otherwise launched after endRecording with the complete tape.
void launchSecondaries(Stage,const FrameMaps& predicted,std::uint64_t ownerToken,bool streaming=false) noexcept;
bool stageLaunched(Stage) noexcept;                                               // a job exists for this stage of the frame
void noteNotLaunched(Stage,const char* reason) noexcept;                          // static reason strings only
void noteStreamNotLaunched(Stage,const char* reason) noexcept;                    // stage-start (streaming) miss; the stage end retries
void validateLaunched(const FrameMaps& filled,std::uint64_t ownerToken) noexcept; // after the consumer's FrameMaps fill
void finishFrameJobs() noexcept;                                                  // frame end/abort: join or discard jobs
// Render thread, before an eye-target rebuild (or an owner reset): cancel + join this frame's launched jobs (the
// frame context is kept; their stages decline with `why` and replay inline). Returns the jobs cancelled. Static `why` only.
unsigned cancelFrameJobs(const char* why) noexcept;
// Per-STEREO-TEST-mode accumulation (report stereoReplay.async.byMode). GameXr samples the stock stages of the
// frame at FinishPrimary (every mode): `serial` counts measurements so far (a stage that did not run this frame keeps
// its serial and is not counted again); ns UINT64_MAX = no valid value.
struct StageSample {std::uint64_t ns{UINT64_MAX},serial{};};
struct FrameStageSample {StageSample depthBucket,colorBucket,depthStage,colorStage,stereoDraw;std::uint32_t depthObjects{UINT32_MAX},colorObjects{UINT32_MAX};};
void noteFrameStages(const FrameStageSample&) noexcept;                          // [render] frame end
unsigned liveMask() noexcept;
// Light-stage probe: which = 0 off, 1 = left eye, 2 = right eye. Matrices are the eye's current relClip/camera.
// The probe is armed only when debug.rdrvr.stereoreplay.probe=1 (read at most once per 64 setProbe calls); a
// disarmed probeDraw is one relaxed load of a global (no TLS, no errno, no lock).
void setProbe(unsigned which,const float* relClip,const float* camera,void* context,const float* otherRelClip=nullptr,const float* otherCamera=nullptr) noexcept;
inline std::atomic<bool> probeArmed{};
void probeDrawArmed(VkCommandBuffer c) noexcept;
inline void probeDraw(VkCommandBuffer c) noexcept {if(probeArmed.load(std::memory_order_relaxed))probeDrawArmed(c);}
void setProbeEnabledForTests(bool enabled) noexcept;
void shaderCreated(VkShaderModule module,VkResult result) noexcept;   // after vkCreateShaderModule (same thread as patchShaderCreate)
void pipelinesCreated(std::uint32_t count,const VkGraphicsPipelineCreateInfo* infos,const VkPipeline* out,VkResult result) noexcept;
void pipelineLayoutCreated(const VkPipelineLayoutCreateInfo* info,VkPipelineLayout layout,VkResult result) noexcept; // debug.rdrvr.stereoreplay.mask, re-read every call (cheap)
// Applies the eye-specific bindless overrides to every tracked clone:
// textures[i].leftIndex slot <- descriptor of textures[i].rightIndex.
// First restores (engine[l] -> clone_owner[l]) every slot of this owner overridden by an earlier call and not
// overridden again now; count 0 only restores. True when at least one override copy was made.
// rightImage = the right VkImage the pair's right texture must show (maps.images[i].right);
// a copy is made only when engine element r holds a LIVE view (viewRegistry) of that image (null
// rightImage, host tests: copied without validation, except that a known dead view is never copied).
struct BindlessOverride {std::uint32_t leftIndex{},rightIndex{};VkImage rightImage{};};
bool applyBindlessOverrides(VkDevice,const BindlessOverride*,unsigned,unsigned owner) noexcept;
// ---- Adreno descriptor-set companion flag ----
// vulkan.adreno.so BuildId 7635b58b7bb23238a5cb173fd1106751 (Quest 3 and 3S) keeps a sticky per-set flag [set+0x288] bit 2
// ("holds a view with a companion"). A bound set with it walks all [set+0x120] entries on every draw (~0.2 ms per draw: the
// stall) and lists companion views for vkEndCommandBuffer (a freed view in that list crashes it). The offsets are
// private to that build: nothing is read before the driver module's BuildId matched and every probed handle passed a
// self-check through the safe reader ([handle+0] = the driver's descriptor-set vptr; clone [+0]/[+0x120] = the engine
// set's). debug.rdrvr.stereoreplay.setprobe=0 disarms everything below (read every world frame).
using SafeReader=bool(*)(void* context,std::uintptr_t address,void* out,std::size_t size);
void setSafeReader(SafeReader,void* context) noexcept; // TraceEntry readMemory (process_vm_readv); set before bindRaw
// Containment gate [render]: true = an armed clone set of this owner carries the flag (and debug.rdrvr.stereoreplay.contain
// != 0). The caller draws that owner's right eye natively (noteFallback("CLONE_FLAGGED")) or launches no worker job.
// With debug.rdrvr.stereoreplay.clonerebuild != 0 the flagged clone is replaced at once by a new pool + set filled with WRITES
// of the live mirrored elements; the old set is released once its frame retired (frameHook, retired >= stamp + 2).
// frameSerial = FrameMaps::frameSerial of the frame (launch gate: the predicted maps' serial; stage gate:
// stereoFrameMaps.frameSerial) - once contained, the owner stays contained for every gate of that frame.
bool cloneFlagged(VkDevice,unsigned owner,std::uint64_t frameSerial,bool launch=false) noexcept;
// Mod-owned 1x1 sampled view (plain sampled image: no driver companion; freed at shutdownAfterIdle) written
// instead of copying a dead source (revert / scrub) and over engine elements that hold a destroyed companion view.
// ensureDummyDescriptor creates it once [render] (loader exports; one fenced transition on queue `queueIndex` of `family`
// under queueMutex); a failure is final (dummyDescriptorWanted() false, report driverSets.dummy).
bool dummyDescriptorWanted() noexcept;
void ensureDummyDescriptor(VkDevice,std::uint32_t family,std::uint32_t queueIndex,std::mutex* queueMutex) noexcept;
void setDummyDescriptor(VkImageView view,VkSampler sampler,VkImageLayout layout) noexcept; // host tests / external creation
// TraceEntry endCommand brackets the engine's vkEndCommandBuffer; destroys arriving on another thread meanwhile
// are counted (driverSets.engineScrub.destroysDuringPrimaryEnd).
inline std::atomic<unsigned> primaryEndsActive{};
inline thread_local bool primaryEndThisThread{};
struct PrimaryEndScope {
    PrimaryEndScope() noexcept {primaryEndThisThread=true;primaryEndsActive.fetch_add(1,std::memory_order_acq_rel);}
    ~PrimaryEndScope(){primaryEndsActive.fetch_sub(1,std::memory_order_acq_rel);primaryEndThisThread=false;}
    PrimaryEndScope(const PrimaryEndScope&)=delete;PrimaryEndScope& operator=(const PrimaryEndScope&)=delete;
};
// Host tests: arm the probe without a driver module (descriptor-set vptr expected at [handle+0]; 0 = back to the module check)
// and force the properties (-1: the property again). validate: debug.rdrvr.stereoreplay.validate
// (0 = override copies without validation, even from a dead / foreign source).
void setDriverProbeForTests(std::uintptr_t setVtable,int setprobe,int contain,int rebuild,int enginescrub,int validate=-1) noexcept;
unsigned bindlessClones() noexcept;
// Computes the correction from relative clip matrices (P*inverse(R)) and camera
// positions of both eyes, in double precision. Returns false on singularity.
bool eyeCorrection(const float* relClipLeft,const float* relClipRight,const float* leftPosition,const float* rightPosition,std::array<float,16>& out) noexcept;
std::string report();
void noteFallback(const char* reason) noexcept;
// ---- GPU timestamps per render stage ----
// Boundary markers in the engine frame command buffer; a stage is the span between two markers (report
// stereoReplay.gpuStages). GameXr arms it from diagnostics::profilingEnabled() every frame; disarmed, every hook below
// is one relaxed load.
// TailEnd (renLightPrePassLightsMgr::EndMainColorPass hook = end of the RenderScene tail after the capture) and
// QueriesEnd (after sagFxManager::RunQueries = end of the AI/optics occlusion queries); spans sceneTail / queries / framePost.
enum class GpuMark : unsigned {CmdBegin,LeftDepthBegin,LeftDepthEnd,SunBegin,SunEnd,LeftLightBegin,LeftLightEnd,LeftColorBegin,LeftColorEnd,
    StereoBegin,RightDepthBegin,RightDepthEnd,RightLightBegin,RightLightEnd,RightColorBegin,RightColorEnd,StereoEnd,TailEnd,QueriesEnd,CmdEnd,Count};
inline std::atomic<bool> gpuStagesArmed{};
inline std::atomic<VkCommandBuffer> gpuFrameCommand{};        // frame command buffer being measured (null: none)
void gpuMarkSlow(GpuMark) noexcept;
void gpuPassEdge(VkCommandBuffer,const VkRenderPassBeginInfo* begin) noexcept; // begin: before the pass; null: after its end
void gpuCommandEnding(VkCommandBuffer) noexcept;
inline void gpuMark(GpuMark m) noexcept {if(gpuStagesArmed.load(std::memory_order_relaxed))gpuMarkSlow(m);} // [render] stage boundary
inline bool gpuFrameCommandIs(VkCommandBuffer c) noexcept {return gpuStagesArmed.load(std::memory_order_relaxed)&&c&&c==gpuFrameCommand.load(std::memory_order_relaxed);}
inline void gpuPassBeginning(VkCommandBuffer c,const VkRenderPassBeginInfo* b) noexcept {if(b&&gpuFrameCommandIs(c))gpuPassEdge(c,b);} // before vkCmdBeginRenderPass
inline void gpuPassEnded(VkCommandBuffer c) noexcept {if(gpuFrameCommandIs(c))gpuPassEdge(c,nullptr);}                              // after vkCmdEndRenderPass
// eyePassByIndex: engine draws recorded into the measured frame command buffer while a pass is open (per-pass draws).
inline std::atomic<std::uint32_t> gpuOpenPassDraws{};
inline void gpuDrawSeen(VkCommandBuffer c) noexcept {if(gpuFrameCommandIs(c))gpuOpenPassDraws.fetch_add(1,std::memory_order_relaxed);}
// Lite frame span outside DETAIL (GpuLite.h). commandBegun writes one TOP_OF_PIPE timestamp into a
// learned frame command buffer (own 4-slot pool, never waits on a readback); its vkEndCommandBuffer writes the
// BOTTOM_OF_PIPE one. Off (DETAIL, gpuLiteArmed false, debug.rdrvr.gpulite=0): one relaxed load here.
inline std::atomic<VkCommandBuffer> gpuLiteCommand{};       // frame command buffer measured by the lite span (null: none)
void gpuLiteEnding(VkCommandBuffer) noexcept;
inline void gpuLiteEnd(VkCommandBuffer c) noexcept {if(c&&c==gpuLiteCommand.load(std::memory_order_relaxed))gpuLiteEnding(c);}
unsigned gpuLiteRing(std::uint32_t* out,unsigned max) noexcept; // newest spans (us), oldest..newest; returns the count written
// No armed test: a DETAIL frame whose profiler was switched off at FinishPrimary still closes at its own end
// (otherwise the stale slot would be closed in a later command buffer and feed a frame-N..M span to
// AUTO/ADAPTIVE).
inline void gpuCommandEnd(VkCommandBuffer c) noexcept {if(c&&c==gpuFrameCommand.load(std::memory_order_relaxed))gpuCommandEnding(c);else gpuLiteEnd(c);} // before vkEndCommandBuffer
// Host tests: query-pool entry points (default: loader exports).
struct GpuQueryFunctions {PFN_vkCreateQueryPool createQueryPool{};PFN_vkDestroyQueryPool destroyQueryPool{};PFN_vkGetQueryPoolResults getQueryPoolResults{};
    PFN_vkCmdResetQueryPool cmdResetQueryPool{};PFN_vkCmdWriteTimestamp cmdWriteTimestamp{};
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties{};PFN_vkGetPhysicalDeviceQueueFamilyProperties getQueueFamilyProperties{};};
void setGpuQueryFunctionsForTests(const GpuQueryFunctions*) noexcept;
// Test overrides: debug.rdrvr.gpulite (-1: the property again) and debug.rdrvr.gpustages.passes (-1: the property;
// 0 stage markers only, 1 per-pass timestamps every DETAIL frame, 2 alternate every 120 DETAIL frames).
void setGpuLiteForTests(int enabled) noexcept;
void setGpuPassModeForTests(int mode) noexcept;
// Per-state accumulation (report gpuStages.byState), GameXr frame end while armed: the frame's render-thread
// stage CPU and HUD FPS (attributed to the current state), then the settings signature for the next frames
// (e.g. "res100_sh1_gr1_fov0_so0"; the STEREO TEST mode is appended).
void gpuNoteFrameCpu(const FrameStageSample&,double hudHz) noexcept;
// Appends _m<STEREO TEST mode>[a|b] (boundary phase) and _pt<1|0|L> (DETAIL frame with / without the per-pass
// timestamps, or the lite span outside DETAIL); the graphics part is cut so that the suffix always fits (signatureTruncated).
void gpuSetState(const char* graphicsSignature) noexcept;
std::string gpuStagesReport();
// ---- Render-pass boundary test (STEREO TEST 10/11/12; StereoBoundary.h) ----
// PassDesc registry of every engine render pass (key: the handle the engine received, i.e. the
// VRS-converted pass) with its native attachments; twins only for passes the fragment-rate
// bridge converted (TraceEntry rateCreatePass).
void renderPassCreated(VkDevice,VkRenderPass,const VkRenderPassCreateInfo*,const PassTwins& twins) noexcept;
bool renderPassDestroyed(VkDevice,VkRenderPass,PassTwins& twins) noexcept; // true: the pass had twins (the caller destroys them)
void noteTwinFailure() noexcept;
bool twinsFor(VkRenderPass,PassTwins&) noexcept; // lock-free [any]; false: no twins
// GameXr FinishPrimary, once per frame (before gpuSetState): STEREO TEST 10/11/12 and 18/19/20 (testModeAlternating)
// alternate 60 frames on ("a") / 60 off ("b"); the flags hold for the next frame's recordings, splices and gpuStages slot.
// 18/19 only change the phase (flags 0); 20 sets BoundaryMergeR in window a while debug.rdrvr.stereoreplay.merge != 0.
// Every 64th call it also re-reads debug.rdrvr.gpulite and debug.rdrvr.gpustages.passes.
void latchBoundaryPhase() noexcept;
inline std::atomic<unsigned> boundaryFlags{};          // BoundaryFlags armed for the current frame (0: none)
inline std::atomic<unsigned> boundaryPhase{};          // 0 no boundary mode, 1 on ("a"), 2 off ("b")
inline std::atomic<bool> boundaryQueriesObserved{};    // TraceEntry: the engine's vkCmdBeginQuery/EndQuery are hooked
void setBoundaryLeftExtraForTests(int value) noexcept;  // -1: property debug.rdrvr.stereoreplay.leftextra
// Left eye (mode 12): the engine pass is ended after its split draw and a tail twin begun with the stereo raw table
// (no tape, foveation or gpuStages hook sees it); the EXTRA variant (leftextra=1) adds an empty twin after the engine End.
void leftPassBegunSlow(VkCommandBuffer,const VkRenderPassBeginInfo*,VkSubpassContents) noexcept;
void leftDrawSlow(VkCommandBuffer) noexcept;
void leftPassEndedSlow(VkCommandBuffer) noexcept;
void leftQuerySlow(VkCommandBuffer,int delta) noexcept;
constexpr unsigned boundaryLeftFlags=BoundarySplitL|BoundaryExtraL;
inline void leftPassBegun(VkCommandBuffer c,const VkRenderPassBeginInfo* b,VkSubpassContents s) noexcept {if(boundaryFlags.load(std::memory_order_relaxed)&boundaryLeftFlags)leftPassBegunSlow(c,b,s);} // Begin hook
inline void leftAfterDraw(VkCommandBuffer c) noexcept {if(boundaryFlags.load(std::memory_order_relaxed)&BoundarySplitL)leftDrawSlow(c);gpuDrawSeen(c);} // After the forwarded draw (+ per-pass draw count, DETAIL only)
inline void leftPassEnded(VkCommandBuffer c) noexcept {if(boundaryFlags.load(std::memory_order_relaxed)&boundaryLeftFlags)leftPassEndedSlow(c);}     // End hook, before gpuPassEnded
inline void leftQueryEdge(VkCommandBuffer c,int delta) noexcept {if(boundaryFlags.load(std::memory_order_relaxed)&BoundarySplitL)leftQuerySlow(c,delta);} // vkCmdBeginQuery +1 / EndQuery -1
}
