#include "StereoReplay.h"
#include "StereoReplayInternal.h"
#include "StereoReplayAsync.h"
#include "EyePositionPatch.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#include <dlfcn.h>  // Driver module identity (dladdr / dl_iterate_phdr, GNU build-id note)
#include <elf.h>
#include <link.h>
#include <unistd.h> // gettid (P1 log lines)
#endif
#include "StereoTestMode.h"
#include "CameraMemberScan.h"
#include "EyeDump.h"
#include "S02Levers.h"          // Lever mask of the running frame / latch at the frame end
#include "EngineColorRestart.h" // COLOUR RESTARTS OFF (left colour stage scope)
#include "StereoScissor.h"      // Moon: right-eye remap of sub-rectangle scissors (SkyDome::SetMoonExtents)
#include "EyeCullTape.h"        // EYE CULL B: tape draws flagged outside the right eye are not replayed

namespace rdrvr::engine::stereo {
namespace internal { // heap parts of ThreadHot (StereoReplay.h)
struct Tracked {VkCommandBuffer command{};BoundState state{};std::uint64_t touched{};};
struct TrackedSet {std::array<Tracked,4> entries{};std::uint64_t clock{};};
struct DynCache {std::unordered_map<VkPipelineLayout,std::vector<std::uint32_t>> map;};
}
using namespace internal; // tape format, counters and rewrite types (StereoReplayInternal.h)
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
RawCommands raw;
std::atomic<bool> rawBound{};
std::mutex coldMutex;
std::uint64_t nowNs(){return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}

// ---------------------------------------------------------------- counters (Stats: StereoReplayInternal.h)
Stats stats;
// debug.rdrvr.stereoreplay.sdalias (0: the replayed depth stage copies right Depth -> SampledDepth).
// 1: right-eye shader reads of SampledDepth resolve to the right Depth image (EngineTextureMap::texture asks sampledDepthAliased),
// i.e. one image for both roles like the stock left eye; the replayed tapes already sample the right Depth (first bindless override,
// GameXr stereoPrepareFrame: the in-stage sun-shadow collector reads the CURRENT right depth), so emitDepthResolve drops the copy
// and keeps only the Depth transition to DEPTH_STENCIL_READ_ONLY. `active` lives from that tail to the frame end
// (latchBoundaryPhase) / the next depth recording or replay; a natively drawn right depth stage never sets it, so its native copy
// (EngineDepthResolve) and SampledDepth routing stay native. Render thread; report stereoReplay.sdAlias.
struct SdAlias {std::atomic<bool> active{};std::atomic<unsigned> on{},calls{};std::atomic<std::uint64_t> skipped{},routed{};};
SdAlias sdAlias;
inline void sdAliasClear() noexcept {sdAlias.active.store(false,std::memory_order_relaxed);}

// ---------------------------------------------------------------- tape (format: StereoReplayInternal.h)
// Bound state of the last 4 command buffers begun on this thread (LRU by last access). The entries
// live on the heap (threadHot.trackedSet, allocated by the first commandBegun of the thread, never
// freed) and the last command resolved is cached in threadHot, so the per-bind hit is a pointer
// compare. A hit through the cache does not stamp the clock; the cached entry is stamped when the
// slow path runs next, before anything else, which keeps exactly the relative LRU order of stamping
// every access (the cached entry always holds the newest stamp at that point).
#if defined(__GNUC__)||defined(__clang__)
#define RDRVR_STEREO_COLD __attribute__((noinline,cold))
#define RDRVR_STEREO_HOT inline __attribute__((always_inline))
#else
#define RDRVR_STEREO_COLD __declspec(noinline)
#define RDRVR_STEREO_HOT __forceinline
#endif
RDRVR_STEREO_COLD BoundState* trackedSlow(ThreadHot& h,VkCommandBuffer c,bool create) noexcept {
    TrackedSet* set=h.trackedSet;
    if(!set){if(!create&&c)return nullptr;set=h.trackedSet=new(std::nothrow) TrackedSet{};if(!set)return nullptr;} // (a null handle matches an empty entry)
    if(h.trackedState)set->entries[h.trackedSlot].touched=++set->clock;
    for(unsigned i=0;i<set->entries.size();++i){auto& t=set->entries[i];if(t.command!=c)continue;
        t.touched=++set->clock;
        if(c){h.trackedCommand=c;h.trackedState=&t.state;h.trackedSlot=i;}
        return &t.state;}
    if(!create)return nullptr;
    unsigned victim=0;
    for(unsigned i=0;i<set->entries.size();++i){const auto& t=set->entries[i];if(!t.command){victim=i;break;}else if(t.touched<set->entries[victim].touched)victim=i;}
    auto& v=set->entries[victim];v.command=c;v.state=BoundState{};v.touched=++set->clock;
    h.trackedCommand=c;h.trackedState=c?&v.state:nullptr;h.trackedSlot=victim;if(!c)h.trackedCommand={};
    return &v.state;
}
RDRVR_STEREO_HOT BoundState* tracked(ThreadHot& h,VkCommandBuffer c,bool create) noexcept { // hit: inlined pointer compare
    if(!create&&c&&c==h.trackedCommand)[[likely]]return h.trackedState;
    return trackedSlow(h,c,create);
}
inline BoundState* tracked(VkCommandBuffer c,bool create) noexcept {return tracked(threadHot,c,create);}
}
namespace {
Tape tapes[2];
std::atomic<std::uint32_t> lastDraws[2]{}; // Draws of the last completed tape per stage (UINT32_MAX = invalid)
// Census: draws of every pass of the stage's current recording (recordBegin/EndRenderPass, render thread),
// published by endRecording into drawHist (report stereoReplay.census.drawHist) and, for the depth stage's pass 0, into
// the DETAIL slot (gpuStages.depthByDraws).
struct TapePassDraws {std::array<std::uint16_t,64> draws{};unsigned passes{};std::uint32_t start{};bool open{};};
std::array<TapePassDraws,2> tapePassDraws;
inline TapePassDraws& passDrawsOf(const Tape& t) noexcept {return tapePassDraws[&t==&tapes[1]?1u:0u];}
inline void notePassBegin(const Tape& t) noexcept {auto& d=passDrawsOf(t);d.start=t.draws;d.open=true;}
inline void notePassEnd(const Tape& t) noexcept {
    auto& d=passDrawsOf(t);if(!d.open)return; // a leading End closes the engine pass begun before the recording window
    d.open=false;if(d.passes<d.draws.size())d.draws[d.passes]=std::uint16_t(std::min<std::uint32_t>(t.draws-d.start,65535u));++d.passes;
}
struct DrawHist { // per stage; written by the render thread (endRecording), read by report()
    std::array<std::atomic<std::uint64_t>,4> cls{};std::atomic<std::uint64_t> passes{},frames{},overflow{};
    std::array<std::atomic<std::uint64_t>,65> bins{};                      // 32 draws per bin, the last one >= 2048
    std::array<std::atomic<std::uint32_t>,256> frameMax{};std::atomic<std::uint64_t> frameCount{};std::atomic<std::uint32_t> maxEver{};
};
std::array<DrawHist,2> drawHist;
bool sameCommand(Tape& t,VkCommandBuffer c){
    if(!t.command)[[unlikely]]{t.command=c;if(const auto* s=tracked(c,false))t.prologue=*s;else t.prologue=BoundState{};return true;}
    if(t.command!=c){t.fail("TAPE_MULTIPLE_COMMANDS");return false;}
    return true;
}
template<class T> T* after(void* p,std::size_t count=0){(void)count;return reinterpret_cast<T*>(p);}

// ---------------------------------------------------------------- registries
struct ViewRecord {VkDevice device{};VkImage image{};VkImageViewType type{};VkFormat format{};VkImageSubresourceRange range{};};
struct FramebufferRecord {VkDevice device{};VkRenderPass pass{};std::uint32_t width{},height{},layers{},count{};std::array<VkImageView,8> views{};bool owned{};};
std::unordered_map<VkImageView,ViewRecord> viewRegistry;
std::unordered_map<VkFramebuffer,FramebufferRecord> framebufferRegistry;std::atomic<unsigned> framebufferGeneration{};
std::atomic<PassThroughTranslator> passThroughTranslator{}; // Foveation map of the left eye -> right eye (resolveFramebuffer)
struct LayoutRecord {VkDevice device{};bool bindless{};VkDescriptorSetLayoutCreateFlags flags{};std::vector<VkDescriptorSetLayoutBinding> bindings{};std::vector<VkDescriptorBindingFlags> bindingFlags{};std::uint32_t textureBinding{UINT32_MAX};std::vector<std::uint32_t> dynamicBindings{};};
std::unordered_map<VkPipelineLayout,VkDescriptorSetLayout> pipelineLayoutRegistry;
std::unordered_map<VkDescriptorSetLayout,LayoutRecord> layoutRegistry;
struct SetRecord {VkDevice device{};VkDescriptorPool gamePool{};VkDescriptorSetLayout layout{};VkDescriptorSet clone{};VkDescriptorSet clone2{};VkDescriptorPool clonePool{};std::uint32_t textureBinding{};std::uint32_t textureCount{};std::vector<bool> written{};std::uint32_t variableCount{},layoutCount{},writtenCount{},maxWritten{};std::array<std::uint32_t,8> writesPerBinding{};
    std::array<std::uint32_t,8> bindingCounts{};std::array<std::vector<bool>,8> bindingWritten{};std::array<std::uint32_t,8> bindingWrittenCount{};
    std::array<std::vector<VkImage>,8> elementImage{};std::array<std::vector<std::uint32_t>,8> elementSlot{};
    std::unordered_map<VkImage,std::vector<std::uint32_t>> elementsOf{};std::uint32_t aliasTracked{};
    // The engine's view last written to every element (same index space and cap as elementImage; null =
    // unknown: never written with a view, or copied from an untracked set) and, per owner (0: clone, 1: clone2), the
    // elements whose clone slot holds an override copy: {binding, element, view of the copied right element, frame stamp}.
    // Exact: replaced by the next override of the slot, removed by a revert / scrub / mirrored engine write of it.
    std::array<std::vector<VkImageView>,8> elementView{};
    struct OverrideEntry {std::uint32_t binding{},element{};VkImageView source{};std::uint64_t frame{};};
    std::array<std::vector<OverrideEntry>,2> overrides{};
    // imageLayout (and sampler, COMBINED_IMAGE_SAMPLER bindings) of the engine's last write per element (same
    // index space and cap as elementView; a clone rebuild re-WRITES them), descriptor type and binding flags per binding,
    // `untracked`: the layout has a binding the mirror cannot re-write (not a >= 32-element sampled binding) -> no rebuild.
    std::array<std::vector<VkImageLayout>,8> elementLayout{};std::array<std::vector<VkSampler>,8> elementSampler{};
    std::array<VkDescriptorType,8> bindingTypes{};std::array<VkDescriptorBindingFlags,8> bindingFlagBits{};bool untracked{};
    // Dedicated pool of a rebuilt clone (null: the clone still lives in clonePool, shared with the other one)
    std::array<VkDescriptorPool,2> ownPool{};
    // Driver probe of this record: [0] engine set, [1] clone, [2] clone2. checked/armed per set (a rebuilt
    // clone is re-checked); allocFlag = [clone+0x288] read right after vkAllocateDescriptorSets (-1: not read).
    struct Probe {std::array<bool,3> checked{},armed{},vtableOk{};std::array<std::uint8_t,3> flag{};std::array<std::uint32_t,3> count{};
        std::array<std::uint64_t,3> flaggedFrames{};std::array<int,2> allocFlag{-1,-1};std::array<std::uintptr_t,3> vtable{};
        std::array<std::uint32_t,2> rebuilds{};std::array<std::uint64_t,2> lastRebuild{};
        std::array<std::uint8_t,3> unread{};} probe{};}; // Failed safe reads per set (retried; latched as refused after 64)
std::unordered_map<VkDescriptorSet,SetRecord> setRegistry;
// ---- override sources, deferred destroys (coldMutex unless atomic) ----
// A clone slot may have handed an override source to a command buffer that is still recording or
// pending after the slot was restored: the source counts as "recent" for scrubWindow frames after
// it left the slot. A destroy of a recent (or still installed) source, or of an attachment of a
// mod-owned right framebuffer, is forwarded to the driver only once its frame retired (stamp +
// scrubWindow <= passCapture.retiredFrame(), the foveation LRU rule), or at shutdownAfterIdle.
constexpr std::uint64_t scrubWindow=2;
struct RecentSource {VkImageView view{};std::uint64_t frame{};};
std::vector<RecentSource> recentSources;
// The engine's allocation callbacks are copied (a compatible set: same functions and pUserData), so a caller-owned
// struct need not outlive its vkDestroyImageView call.
struct DeferredView {VkDevice device{};VkImageView view{};VkAllocationCallbacks callbacks{};bool hasCallbacks{};PFN_vkDestroyImageView destroy{};std::uint64_t frame{};
    VkImage image{}; // The view's image (viewRegistry at its destroy; null = untracked)
    void forward() const noexcept {if(destroy)destroy(device,view,hasCallbacks?&callbacks:nullptr);}};
struct DeferredFramebuffer {VkDevice device{};VkFramebuffer framebuffer{};std::uint64_t frame{};};
// An image destroyed while a deferred view of it is pending (the End walk reads [view+0xf0] = the image and
// [image+0x148]); forwarded after every pending view of it, once its stamp (the latest of those views) retired.
struct DeferredImage {VkDevice device{};VkImage image{};VkAllocationCallbacks callbacks{};bool hasCallbacks{};PFN_vkDestroyImage destroy{};std::uint64_t frame{};
    void forward() const noexcept {if(destroy)destroy(device,image,hasCallbacks?&callbacks:nullptr);}};
std::vector<DeferredView> deferredViews;std::vector<DeferredFramebuffer> deferredFramebuffers;std::vector<DeferredImage> deferredImages;
std::atomic<std::uint64_t> scrubFrame{},scrubRetired{};            // latest frameHook values (render thread writes)
std::atomic<int> scrubForced{-1};std::atomic<bool> scrubProperty{true},scrubPropertyRead{};
void refreshScrubProperty() noexcept {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX]{};scrubProperty.store(!(__system_property_get("debug.rdrvr.stereoreplay.scrub",v)>0&&v[0]=='0'&&!v[1]),std::memory_order_relaxed);
#endif
    scrubPropertyRead.store(true,std::memory_order_release);
}
bool scrubOn() noexcept { // debug.rdrvr.stereoreplay.scrub: unset/1 = revert + scrub + deferred destroy, 0 = count only
    const int forced=scrubForced.load(std::memory_order_relaxed);if(forced>=0)return forced!=0;
    if(!scrubPropertyRead.load(std::memory_order_acquire))refreshScrubProperty();
    return scrubProperty.load(std::memory_order_relaxed);
}
void noteRecentSource(VkImageView view,std::uint64_t frame){ // caller holds coldMutex
    if(!view)return;
    for(auto& s:recentSources)if(s.view==view){s.frame=std::max(s.frame,frame);return;}
    recentSources.push_back({view,frame});
}
bool recentSource(VkImageView view,std::uint64_t frame) noexcept { // caller holds coldMutex
    for(const auto& s:recentSources)if(s.view==view&&s.frame+scrubWindow>=frame)return true;
    return false;
}
void notePending() noexcept { // caller holds coldMutex
    const std::uint64_t n=deferredViews.size()+deferredFramebuffers.size()+deferredImages.size();
    if(n>stats.scrub.maxPending.load(std::memory_order_relaxed))stats.scrub.maxPending.store(n,std::memory_order_relaxed);
}
// A mirrored engine write/copy of [first, first+count) at `binding` replaces the clone and clone2 slots: their override
// entries end there (the source stays recent). Caller holds coldMutex.
void dropOverridesAt(SetRecord& r,std::uint32_t binding,std::uint32_t first,std::uint32_t count,std::uint64_t frame){
    for(auto& entries:r.overrides){
        for(std::size_t k=0;k<entries.size();){const auto& e=entries[k];
            if(e.binding!=binding||e.element<first||e.element-first>=count){++k;continue;}
            noteRecentSource(e.source,frame);++stats.scrub.entriesMirrorDropped;entries[k]=entries.back();entries.pop_back();}}
}
struct DeviceRecord {VkPhysicalDevice physical{};VkPipelineLayout pushLayout{};};
std::unordered_map<VkShaderModule,ShaderInfo> shaderRegistry;
std::unordered_map<VkPipeline,PipelineInfo> pipelineRegistry;std::atomic<unsigned> pipelineGeneration{};
// Bindless texture references of every module (patchShaderCreate scan; coldMutex) and their union per
// graphics pipeline (merge rule M8); erased at device teardown, overwritten when a handle is reused.
std::unordered_map<VkShaderModule,merge::TexScan> texScanByModule;std::unordered_map<VkPipeline,merge::TexScan> texScanByPipeline;
thread_local merge::TexScan lastTexScan{};
merge::Stats mergeStatsValue;
void setElementImage(SetRecord& r,std::uint32_t binding,std::uint32_t element,VkImage image,VkImageView view){ // caller holds coldMutex
    if(binding>=r.elementImage.size()||element>=r.elementImage[binding].size())return;
    if(element<r.elementView[binding].size())r.elementView[binding][element]=view; // The view shadow (also when the image is unchanged)
    auto& images=r.elementImage[binding];auto& slots=r.elementSlot[binding];const VkImage old=images[element];if(old==image)return;
    const std::uint32_t key=(binding<<24)|element; // element < 2^24 (capped at 65536)
    if(old){auto it=r.elementsOf.find(old);
        if(it!=r.elementsOf.end()){auto& keys=it->second;const std::uint32_t at=slots[element];
            if(at<keys.size()&&keys[at]==key){const std::uint32_t moved=keys.back();keys[at]=moved;keys.pop_back();
                if(moved!=key&&(moved>>24)<r.elementSlot.size()&&(moved&0xffffffu)<r.elementSlot[moved>>24].size())r.elementSlot[moved>>24][moved&0xffffffu]=at;
                if(r.aliasTracked)--r.aliasTracked;}
            if(keys.empty())r.elementsOf.erase(it);}}
    images[element]=VK_NULL_HANDLE;
    if(image){auto& keys=r.elementsOf[image];slots[element]=std::uint32_t(keys.size());keys.push_back(key);images[element]=image;++r.aliasTracked;}
}
namespace drv {
// vulkan.adreno.so BuildId 7635b58b7bb23238a5cb173fd1106751 (Quest 3 and Quest 3S).
constexpr unsigned char buildId[16]{0x76,0x35,0xb5,0x8b,0x7b,0xb2,0x32,0x38,0xa5,0xcb,0x17,0x3f,0xd1,0x10,0x67,0x51};
// The VkDescriptorSet handle IS the driver object (vkCmdBindDescriptorSets 0x29f220 stores the handle into [cb+0x240][i]; the
// draw-time flush 0x21e3d0 reads [handle+0x288] directly). Its vptr = module base + 0x51efd0, the vtable's address point in
// .data.rel.ro (+0x28 storage release 0x367bf8, +0x38 init 0x2c7070, +0x40 write 0x29ffd0, +0x48 copy 0x23ed80).
constexpr std::uintptr_t setVtableOffset=0x51efd0;
constexpr std::uintptr_t setFlagOffset=0x288;   // ldrb. Bit 2 = "holds a view with a companion": ORed by the write paths
constexpr std::uint8_t companionBit=4;          // (0x2a0b18, 0x2a1488, 0x23f8a8, 0x23fa0c) and by the copy path 0x23f294 from the
                                                // SOURCE entry's view; cleared only by the storage release 0x367cf0 (set freed)
constexpr std::uintptr_t setCountOffset=0x120;  // ldr w: 64-byte slot count = the per-draw scan bound (65536 for 4 x 16384)
constexpr std::uintptr_t viewCompanionOffset=0x1b8; // companion view: created by vkCreateImageView iff [image+0x458]
// The only layer allowed between the loader and the ICD: it intercepts vkUpdateDescriptorSets / vkAllocateDescriptorSets /
// vkCmdBindDescriptorSets but forwards the driver's handles unchanged.
constexpr const char* workerLayer="libVkLayer_RDR2_raw_worker28.so";
constexpr std::uint32_t maxRebuildsPerClone=16,rebuildSpacing=8; // per engine set and owner: session cap / frames between rebuilds
}
struct CopySource {std::uint64_t frame{};std::uint8_t owner{},kind{};std::uint32_t binding{},l{},r{};VkImageView view{};bool live{},imageMatches{},copied{},companionRead{};std::uintptr_t companion{};};
constexpr const char* copyKindName[3]{"override","revert","scrub"};
struct DeferredSet {VkDevice device{};VkDescriptorPool pool{};VkDescriptorSet set{};bool ownPool{};std::uint64_t frame{};};
struct T2State {
    std::atomic<int> forcedSetprobe{-1},forcedContain{-1},forcedRebuild{-1},forcedEngineScrub{-1},forcedValidate{-1}; // host tests (-1: property)
    std::atomic<bool> setprobe{true},contain{true},rebuild{true},engineScrub{true},validate{true}; // frameHook re-reads the properties
    std::atomic<std::uint64_t> unvalidatedCopies{}; // validate=0: override copies the validation would have refused (made anyway)
    std::atomic<int> identity{};std::atomic<const char*> why{"NOT_TRIED"};std::uintptr_t base{},setVtable{};std::atomic<std::uintptr_t> testVtable{}; // 1 armed, -1 refused
    char dispatchModule[64]{};int dispatchIcd{-1};
    std::atomic<std::uint64_t> selfCheckRefused{},probeFrames{},copySourcesDropped{};
    std::uint64_t firstFlagFrame{};int firstFlagOwner{-1},firstFlagSet{-1};
    std::array<CopySource,24> latest{},first{};unsigned latestCount{},firstCount{};
    // (b) source validation
    std::atomic<std::uint64_t> overridesStaleSource{},overridesOtherImage{},overridesNoSource{},overridePairsStale{},revertsStaleSource{},scrubsStaleSource{},dummyWrites{},dummyMissing{};
    // (c) containment
    // Containment is keyed by FrameMaps::frameSerial (the same value at a frame's launches and at its stages;
    // monotonic, never reset); cloneFlaggedFrames / launchRefused count frames (serials), stageFallbacks counts stages.
    std::array<std::atomic<std::uint64_t>,2> cloneFlaggedFrames{},launchRefused{},stageFallbacks{},containOff{};std::array<std::uint64_t,2> containedSerial{UINT64_MAX,UINT64_MAX},launchRefusedSerial{UINT64_MAX,UINT64_MAX};
    std::atomic<std::uint64_t> engineFlaggedChecks{},rebuildContainedStages{}; // contain=0 but a stage-gate rebuild contains its frame
    // Latest frame (scrubFrame) in which any armed engine / clone / clone2 byte had the companion bit (coldMutex)
    std::uint64_t lastAnyFlagFrame{};
    // (d) recovery
    std::array<std::atomic<std::uint64_t>,2> cloneRebuilds{};
    std::atomic<std::uint64_t> rebuildReflagged{},rebuildRefused{},rebuildCapped{},rebuildSpaced{},rebuildFailed{},rebuildWrites{},rebuildDeadSkipped{},setReleases{},teardownSetReleases{};
    std::atomic<std::uint64_t> rebuildLayoutSkipped{}; // Live elements with an untracked layout / sampler, not written
    const char* lastRebuildRefusal{"NONE"};std::array<std::uint64_t,16> rebuildFrames{};unsigned rebuildFrameCount{};
    // (e) probes P1-P3 and the engine scrub
    std::atomic<std::uint64_t> staleEngineCompanionViews{},companionUnread{},engineScrubWrites{},engineScrubSlots{},engineScrubRefusedFlags{},engineScrubRefusedNoDummy{},engineScrubDeferredViews{},engineScrubDeferredImages{};
    std::array<std::atomic<std::uint64_t>,2> endOverlapViews{};std::atomic<std::uint64_t> endOverlapImages{}; // P2 [companion, none]
    std::atomic<std::uint64_t> reusedStaleHandles{},staleHoldersReset{};
    std::array<std::atomic<std::uint32_t>,64> burst{};std::atomic<unsigned> burstFrames{};std::atomic<std::uint64_t> burstFirstFrame{};std::atomic<std::uint64_t> burstBefore{};
    std::atomic<long> renderTid{};
    // Failed self-check reads (retried), P1 companion views not deferred (no recent flag), dummy writes
    // refused (binding without UPDATE_AFTER_BIND), dummy objects destroyed at shutdownAfterIdle
    std::atomic<std::uint64_t> selfCheckUnread{},engineScrubUnflagged{},dummyRefusedFlags{},dummyDestroyed{};
} t2;
std::vector<DeferredSet> deferredSets;
std::mutex identityMutex; // ensureIdentity only (never taken under coldMutex: dl_iterate_phdr takes the loader lock)
struct Dummy {VkDevice device{};VkImageView view{};VkSampler sampler{};VkImageLayout layout{VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};VkImage image{};VkDeviceMemory memory{};const char* status{"NOT_CREATED"};} dummy;
std::atomic<bool> dummyReady{},dummyFailed{};
std::atomic<SafeReader> safeReader{};std::atomic<void*> safeReaderContext{};
bool safeRead(std::uintptr_t p,void* out,std::size_t n) noexcept {
    const auto r=safeReader.load(std::memory_order_acquire);return r&&p&&r(safeReaderContext.load(std::memory_order_relaxed),p,out,n);}
bool propOn(const std::atomic<int>& forced,const std::atomic<bool>& prop) noexcept {const int f=forced.load(std::memory_order_relaxed);return f>=0?f!=0:prop.load(std::memory_order_relaxed);}
bool setprobeOn() noexcept {return propOn(t2.forcedSetprobe,t2.setprobe);}               // debug.rdrvr.stereoreplay.setprobe (default 1)
bool containOn() noexcept {return setprobeOn()&&propOn(t2.forcedContain,t2.contain);}      // debug.rdrvr.stereoreplay.contain (default 1)
bool rebuildOn() noexcept {return setprobeOn()&&propOn(t2.forcedRebuild,t2.rebuild);}      // debug.rdrvr.stereoreplay.clonerebuild (default 1)
bool engineScrubOn() noexcept {return setprobeOn()&&propOn(t2.forcedEngineScrub,t2.engineScrub);} // debug.rdrvr.stereoreplay.enginescrub (default 1)
// debug.rdrvr.stereoreplay.validate (default 1): 0 = the override copy is made without validation,
// even from a dead / foreign source (counted unvalidatedCopies): a diagnostic mode that lets the probe
// see which copy sets the flag
bool validateOn() noexcept {return propOn(t2.forcedValidate,t2.validate);}
void refreshT2Properties() noexcept {
#ifdef __ANDROID__
    auto read=[](const char* name,std::atomic<bool>& out){char v[PROP_VALUE_MAX]{};out.store(!(__system_property_get(name,v)>0&&v[0]=='0'&&!v[1]),std::memory_order_relaxed);};
    read("debug.rdrvr.stereoreplay.setprobe",t2.setprobe);read("debug.rdrvr.stereoreplay.contain",t2.contain);
    read("debug.rdrvr.stereoreplay.clonerebuild",t2.rebuild);read("debug.rdrvr.stereoreplay.enginescrub",t2.engineScrub);
    read("debug.rdrvr.stereoreplay.validate",t2.validate);
#endif
}
#ifdef __ANDROID__
struct ModuleSearch {std::uintptr_t base{};bool found{},matched{};};
int matchDriverModule(dl_phdr_info* info,std::size_t,void* data) noexcept {
    auto& s=*static_cast<ModuleSearch*>(data);
    const char* path=info->dlpi_name?info->dlpi_name:"";const char* name=std::strrchr(path,'/');name=name?name+1:path;
    if(std::strcmp(name,"vulkan.adreno.so"))return 0;
    s.found=true;s.base=std::uintptr_t(info->dlpi_addr);
    for(ElfW(Half) i=0;i<info->dlpi_phnum;++i){const auto& note=info->dlpi_phdr[i];if(note.p_type!=PT_NOTE||!note.p_memsz)continue;
        bool readable=false; // only a note inside a readable load segment of the module
        for(ElfW(Half) j=0;j<info->dlpi_phnum&&!readable;++j){const auto& load=info->dlpi_phdr[j];
            readable=load.p_type==PT_LOAD&&(load.p_flags&PF_R)&&note.p_vaddr>=load.p_vaddr&&note.p_vaddr-load.p_vaddr<=load.p_memsz&&note.p_memsz<=load.p_memsz-(note.p_vaddr-load.p_vaddr);}
        if(!readable)continue;
        const auto* p=reinterpret_cast<const unsigned char*>(info->dlpi_addr+note.p_vaddr);std::size_t left=note.p_memsz;
        while(left>=sizeof(ElfW(Nhdr))){ElfW(Nhdr) n{};std::memcpy(&n,p,sizeof n);p+=sizeof n;left-=sizeof n;
            const std::size_t names=(std::size_t(n.n_namesz)+3u)&~std::size_t(3),desc=(std::size_t(n.n_descsz)+3u)&~std::size_t(3);
            if(names>left||desc>left-names)break;
            if(n.n_type==NT_GNU_BUILD_ID&&n.n_namesz==4&&!std::memcmp(p,"GNU",4)&&n.n_descsz==sizeof drv::buildId&&!std::memcmp(p+names,drv::buildId,sizeof drv::buildId))s.matched=true;
            p+=names+desc;left-=names+desc;}}
    return 1;
}
#endif
// Once per process (outside coldMutex): the loaded vulkan.adreno.so must carry BuildId 7635b58b..., and the replay's raw
// entries must reach the ICD itself or only the known non-wrapping worker layer (vkGetDeviceProcAddr through the loader
// returns the top of the device chain; the engine's slots are loader trampolines into that same chain). Every handle is
// additionally self-checked (armRecord), so a handle-wrapping layer can never arm the probe.
void ensureIdentity(VkDevice d) noexcept {
    if(t2.identity.load(std::memory_order_acquire))return;
    try{std::lock_guard g(identityMutex);if(t2.identity.load(std::memory_order_relaxed))return;
        if(const auto v=t2.testVtable.load(std::memory_order_relaxed)){t2.setVtable=v;t2.why="TEST";t2.identity.store(1,std::memory_order_release);return;}
#ifdef __ANDROID__
        if(!d)return; // retried once a device exists
        ModuleSearch s{};dl_iterate_phdr(matchDriverModule,&s);
        Dl_info info{};const auto f=::vkGetDeviceProcAddr(d,"vkCmdBindDescriptorSets");
        if(f&&dladdr(reinterpret_cast<void*>(f),&info)&&info.dli_fname){const char* b=std::strrchr(info.dli_fname,'/');b=b?b+1:info.dli_fname;
            std::snprintf(t2.dispatchModule,sizeof t2.dispatchModule,"%s",b);t2.dispatchIcd=s.found&&reinterpret_cast<std::uintptr_t>(info.dli_fbase)==s.base?1:0;}
        const char* why=!s.found?"NO_ADRENO_MODULE":!s.matched?"BUILD_ID":t2.dispatchIcd<0?"NO_DISPATCH":
            (!t2.dispatchIcd&&std::strcmp(t2.dispatchModule,drv::workerLayer))?"DISPATCH_LAYER":nullptr;
        t2.base=s.base;
        if(why){t2.why=why;t2.identity.store(-1,std::memory_order_release);return;}
        t2.setVtable=s.base+drv::setVtableOffset;t2.why="ARMED";t2.identity.store(1,std::memory_order_release);
#else
        (void)d;t2.why="NO_DRIVER";t2.identity.store(-1,std::memory_order_release);
#endif
    }catch(...){}
}
bool driverArmed() noexcept {return t2.identity.load(std::memory_order_acquire)==1&&setprobeOn();}
VkDescriptorSet probeSet(VkDescriptorSet engine,const SetRecord& r,unsigned k) noexcept {return k==0?engine:k==1?r.clone:r.clone2;}
// Self-check of every probed handle through the safe reader (nothing is dereferenced directly before it passed): [set+0] is
// the driver's descriptor-set vptr, and a clone's [+0] / [+0x120] equal the engine set's (same class, same layout). Caller
// holds coldMutex, identity armed. A rebuilt clone is re-checked (checked[k] reset).
void armRecord(VkDescriptorSet engine,SetRecord& r) noexcept {
    auto& p=r.probe;
    for(unsigned k=0;k<3;++k){if(p.checked[k])continue;
        if(k&&!p.checked[0])continue; // A clone is compared with the engine set - decided only after it
        const auto s=reinterpret_cast<std::uintptr_t>(probeSet(engine,r,k));std::uintptr_t vt{};std::uint32_t n{};
        const bool read=s&&safeRead(s,&vt,sizeof vt)&&safeRead(s+drv::setCountOffset,&n,sizeof n);
        // A failed read (reader not set yet, a quarantined channel) is not a mismatch: not latched, retried at the
        // next frameHook / gate (selfCheckUnread); 64 failures in a row latch it as refused (a permanently unreadable handle)
        if(!read&&s&&p.unread[k]<64){++p.unread[k];++t2.selfCheckUnread;continue;}
        p.checked[k]=true;p.vtable[k]=vt;p.count[k]=n;
        bool ok=read&&vt==t2.setVtable&&n>0&&n<=(1u<<20);
        if(k)ok=ok&&p.vtableOk[0]&&vt==p.vtable[0]&&n==p.count[0];
        p.vtableOk[k]=ok;p.armed[k]=ok;if(!ok)++t2.selfCheckRefused;}
}
// Armed handles only: the sets are alive while their record is in setRegistry under coldMutex (the free / reset / destroy
// hooks erase the record under coldMutex BEFORE the driver releases the engine set; the clones are the mod's own).
inline std::uint8_t liveFlag(VkDescriptorSet s) noexcept {return *reinterpret_cast<const volatile std::uint8_t*>(reinterpret_cast<std::uintptr_t>(s)+drv::setFlagOffset);}
inline std::uint32_t liveCount(VkDescriptorSet s) noexcept {return *reinterpret_cast<const volatile std::uint32_t*>(reinterpret_cast<std::uintptr_t>(s)+drv::setCountOffset);}
std::uintptr_t readCompanion(VkImageView v,bool& read) noexcept { // P1 / copy sources: only meaningful for the armed driver build
    std::uintptr_t c{};read=v&&driverArmed()&&safeRead(reinterpret_cast<std::uintptr_t>(v)+drv::viewCompanionOffset,&c,sizeof c);return read?c:0;}
void noteFlag(unsigned k,std::uint64_t frame) noexcept { // caller holds coldMutex: the first flag of the session freezes the copy sources
    if(t2.firstFlagSet>=0)return;
    t2.firstFlagSet=int(k);t2.firstFlagOwner=k?int(k-1):-1;t2.firstFlagFrame=frame;t2.first=t2.latest;t2.firstCount=t2.latestCount;
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_WARN,"RdrVrStereo","companion flag first seen: %s, frame %llu, %u copy source(s) of the last override call",
        k==0?"engine set":k==1?"clone (owner 0)":"clone2 (owner 1)",static_cast<unsigned long long>(frame),t2.latestCount);
#endif
}
void noteAnyFlag(std::uint64_t frame) noexcept {t2.lastAnyFlagFrame=std::max(t2.lastAnyFlagFrame,frame);} // caller holds coldMutex
// The engine companion-view / image deferral (e) protects only command buffers that may have LISTED the view, and
// the driver lists views only at draws with a FLAGGED set bound (scan gate 0x21fa5c/0x21fa60). So it acts only while a flag is or
// was recently present: an armed engine / clone / clone2 byte of the device has bit 2 now, or the last flag was seen in a frame
// that has not retired with margin (stamp + scrubWindow + 1 > retired: the launch gate stamps the previous frame id). A probe set
// that does not arm (self-check refused: its flag is unknown) keeps the deferral on. Without a flag destroys are
// not deferred; P1-P3 still count. Caller holds coldMutex, identity armed.
bool flagRecent(VkDevice d) noexcept {
    if(t2.lastAnyFlagFrame&&t2.lastAnyFlagFrame+scrubWindow+1>scrubRetired.load(std::memory_order_relaxed))return true;
    for(auto& [set,record]:setRegistry){if(record.device!=d)continue;armRecord(set,record);const auto& p=record.probe;
        for(unsigned k=0;k<3;++k){if(!p.armed[k])return true;
            if(liveFlag(probeSet(set,record,k))&drv::companionBit){noteAnyFlag(scrubFrame.load(std::memory_order_relaxed));return true;}}}
    return false;
}
void noteCopySource(unsigned owner,unsigned kind,std::uint32_t b,std::uint32_t l,std::uint32_t r,VkImageView v,bool live,bool matches,bool copied,std::uint64_t frame) noexcept {
    if(!v&&!copied)return; // caller holds coldMutex
    if(t2.latestCount>=t2.latest.size()){++t2.copySourcesDropped;return;}
    CopySource c{frame,std::uint8_t(owner),std::uint8_t(kind),b,l,r,v,live,matches,copied,false,0};
    if(v&&(copied||!live))c.companion=readCompanion(v,c.companionRead); // a source that was (or would have been) copied
    t2.latest[t2.latestCount++]=c;
}
// Images whose vkDestroyImage reached the hook: a view of such an image
// is still in viewRegistry for a moment but must never be WRITTEN (rebuild) or copied as a "live" source. A handle reused by a
// new image leaves the set at its first view's creation. coldMutex.
std::unordered_set<VkImage> destroyedImages;
std::unordered_map<VkImageView,std::vector<std::pair<VkDescriptorSet,std::uint32_t>>> staleHolders;
bool liveView(VkImageView v) noexcept { // caller holds coldMutex
    if(!v)return false;if(v==dummy.view&&dummyReady.load(std::memory_order_relaxed))return true;
    const auto it=viewRegistry.find(v);return it!=viewRegistry.end()&&!destroyedImages.count(it->second.image);}
void noteElementLayout(SetRecord& r,std::uint32_t b,std::uint32_t e,VkImageLayout layout,VkSampler sampler) noexcept {
    if(b<8&&e<r.elementLayout[b].size())r.elementLayout[b][e]=layout;
    if(b<8&&e<r.elementSampler[b].size())r.elementSampler[b][e]=sampler;}
// Writes of the dummy view (pImageInfo fixed up at issue time: the vectors may reallocate while collecting). Caller holds coldMutex.
struct DummyWrites {
    std::vector<VkWriteDescriptorSet> writes;std::vector<VkDescriptorImageInfo> infos;
    void clear() noexcept {writes.clear();infos.clear();}
    bool add(VkDescriptorSet dst,std::uint32_t binding,std::uint32_t element,VkDescriptorType type){
        if(!dummyReady.load(std::memory_order_acquire)||!dummy.view)return false;
        const bool combined=type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;if(combined&&!dummy.sampler)return false;
        if(!combined&&type!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)return false;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=dst;w.dstBinding=binding;w.dstArrayElement=element;w.descriptorCount=1;w.descriptorType=type;
        writes.push_back(w);infos.push_back({combined?dummy.sampler:VK_NULL_HANDLE,dummy.view,dummy.layout});return true;}
    const VkWriteDescriptorSet* data() noexcept {for(std::size_t k=0;k<writes.size();++k)writes[k].pImageInfo=&infos[k];return writes.data();}
};
// One rule for every mod write into a set that a recording or pending command buffer may have bound: the binding
// must carry UPDATE_AFTER_BIND (VUID-vkUpdateDescriptorSets-None-03047 accepts it alone; RDR1's bindless bindings report flags 5 =
// UPDATE_AFTER_BIND | PARTIALLY_BOUND). The clone dummy writes (revert / scrub) are gated by it; the engine scrub keeps its
// stricter rule (UPDATE_AFTER_BIND + UPDATE_UNUSED_WHILE_PENDING), so on RDR1 it never writes (refusedFlags = slots).
inline bool updateAfterBind(const SetRecord& r,std::uint32_t b) noexcept {return b<8&&(r.bindingFlagBits[b]&VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT)!=0;}
void noteRebuildFrame(std::uint64_t frame) noexcept {t2.rebuildFrames[t2.rebuildFrameCount++%t2.rebuildFrames.size()]=frame;}
// (d) recovery. Replaces clone `o` of the engine set by a new set from a NEW pool (the old pool may hold the other clone),
// filled with WRITES of the live mirrored elements only (elements whose engine view was destroyed stay unwritten: PARTIALLY_
// BOUND content, never sampled - the texture is gone). Writes never OR a dead pointer's state into the set (the flag bit is
// set from the WRITTEN live view's own companion); the new storage starts with a cleared bit (verified below: rebuildReflagged).
// The old set is not freed here: a primary or worker secondary of this frame may still have it bound (and its End walk may
// still list its views), so it is released by frameHook once stamp + scrubWindow <= retired, i.e. after every command buffer
// that bound it completed (VUID-vkFreeDescriptorSets-pDescriptorSets-00309). Discarded worker secondaries that bound it are
// never submitted; they become invalid, which is legal, and are reset with their pool. Caller holds coldMutex [render].
bool rebuildClone(VkDescriptorSet engine,SetRecord& r,unsigned o,std::uint64_t frame) noexcept {
    auto& p=r.probe;
    auto refuse=[&](const char* why){++t2.rebuildRefused;t2.lastRebuildRefusal=why;return false;};
    if(r.untracked)return refuse("UNTRACKED_BINDING");
    if(p.rebuilds[o]>=drv::maxRebuildsPerClone){++t2.rebuildCapped;return false;}
    if(p.rebuilds[o]&&frame<p.lastRebuild[o]+drv::rebuildSpacing){++t2.rebuildSpaced;return false;}
    auto L=layoutRegistry.find(r.layout);if(L==layoutRegistry.end())return refuse("NO_LAYOUT");
    try{
        std::vector<VkDescriptorPoolSize> sizes;
        for(const auto& b:L->second.bindings){bool merged=false;for(auto& s:sizes)if(s.type==b.descriptorType){s.descriptorCount+=b.descriptorCount;merged=true;}if(!merged)sizes.push_back({b.descriptorType,b.descriptorCount});}
        if(sizes.empty())return refuse("NO_BINDINGS");
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT|((L->second.flags&VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT)?VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT:0);
        pci.maxSets=1;pci.poolSizeCount=std::uint32_t(sizes.size());pci.pPoolSizes=sizes.data();
        VkDescriptorPool pool{};if(raw.createDescriptorPool(r.device,&pci,nullptr,&pool)!=VK_SUCCESS||!pool){++t2.rebuildFailed;return refuse("POOL_CREATE");}
        std::uint32_t variable=r.variableCount;VkDescriptorSetVariableDescriptorCountAllocateInfo vinfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};vinfo.descriptorSetCount=1;vinfo.pDescriptorCounts=&variable;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.pNext=variable?&vinfo:nullptr;ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&r.layout;
        VkDescriptorSet set{};if(raw.allocateDescriptorSets(r.device,&ai,&set)!=VK_SUCCESS||!set){raw.destroyDescriptorPool(r.device,pool,nullptr);++t2.rebuildFailed;return refuse("SET_ALLOCATE");}
        std::vector<VkDescriptorImageInfo> infos;std::vector<VkWriteDescriptorSet> writes;std::uint64_t dead=0;
        for(std::uint32_t b=0;b<8;++b){if(!r.bindingCounts[b])continue;const auto& views=r.elementView[b];const auto& bw=r.bindingWritten[b];
            const VkDescriptorType type=r.bindingTypes[b];const bool combined=type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            for(std::uint32_t e=0;e<views.size()&&e<bw.size();++e){const VkImageView v=views[e];if(!v||!bw[e])continue;
                if(!liveView(v)){++dead;continue;}
                // An element mirrored from an engine COPY of an untracked source has no known imageLayout (UNDEFINED)
                // and, COMBINED, no sampler: writing it would be invalid (VUID-VkWriteDescriptorSet-descriptorType-04149/04150)
                const VkImageLayout layout=e<r.elementLayout[b].size()?r.elementLayout[b][e]:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                const VkSampler sampler=combined&&e<r.elementSampler[b].size()?r.elementSampler[b][e]:VK_NULL_HANDLE;
                if(layout==VK_IMAGE_LAYOUT_UNDEFINED||(combined&&!sampler)){++dead;++t2.rebuildLayoutSkipped;continue;}
                infos.push_back({sampler,v,layout});
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=set;w.dstBinding=b;w.dstArrayElement=e;w.descriptorCount=1;w.descriptorType=type;writes.push_back(w);}}
        for(std::size_t k=0;k<writes.size();++k)writes[k].pImageInfo=&infos[k];
        if(!writes.empty())raw.updateDescriptorSets(r.device,std::uint32_t(writes.size()),writes.data(),0,nullptr); // a fresh set: bound nowhere yet
        t2.rebuildWrites.fetch_add(writes.size(),std::memory_order_relaxed);t2.rebuildDeadSkipped.fetch_add(dead,std::memory_order_relaxed);
        VkDescriptorSet& slot=o?r.clone2:r.clone;const VkDescriptorSet old=slot;const VkDescriptorPool oldOwn=r.ownPool[o];
        slot=set;r.ownPool[o]=pool;
        deferredSets.push_back({r.device,oldOwn?oldOwn:r.clonePool,old,oldOwn!=VK_NULL_HANDLE,frame});
        for(const auto& e:r.overrides[o])noteRecentSource(e.source,frame); // the new set holds the engine elements; the next override call re-applies
        r.overrides[o].clear();
        p.checked[1+o]=false;p.armed[1+o]=false;p.flag[1+o]=0;p.unread[1+o]=0;armRecord(engine,r);
        if(p.armed[1+o]&&(liveFlag(set)&drv::companionBit)){++t2.rebuildReflagged;p.flag[1+o]=1;}
        ++p.rebuilds[o];p.lastRebuild[o]=frame;++t2.cloneRebuilds[o];noteRebuildFrame(frame);
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_WARN,"RdrVrStereo","flagged clone%s rebuilt at frame %llu: %zu live writes, %llu dead elements skipped, new set %sflagged, old set released after retire",
            o?"2":"",static_cast<unsigned long long>(frame),writes.size(),static_cast<unsigned long long>(dead),p.flag[1+o]?"":"not ");
#endif
        return true;
    }catch(...){++t2.rebuildFailed;return refuse("EXCEPTION");}
}
// Deferred clone sets (caller holds coldMutex: a concurrent forgetSet may destroy the shared pool, so the release and the
// pool's destroy are serialized). due: frameHook (stamp + scrubWindow <= retired); all of a device: shutdownAfterIdle.
void releaseDeferredSets(VkDevice device,bool all,std::uint64_t retired) noexcept {
    for(std::size_t k=0;k<deferredSets.size();){const auto x=deferredSets[k];
        if((all&&x.device!=device)||(!all&&x.frame+scrubWindow>retired)){++k;continue;}
        if(x.ownPool)raw.destroyDescriptorPool(x.device,x.pool,nullptr);else raw.freeDescriptorSets(x.device,x.pool,1,&x.set);
        ++t2.setReleases;if(all)++t2.teardownSetReleases;deferredSets[k]=deferredSets.back();deferredSets.pop_back();}
}
thread_local EyeDump eyeDump;thread_local bool sampled_M1mismatch{};thread_local bool sampled_PrevViewProjL{};thread_local bool sampled_WVPInvL{};thread_local bool sampled_WVPInvNZ{};thread_local bool sampled_gCameraMatrixL{};
thread_local PatchReport lastPatchReport{};thread_local bool lastPatchValid{};thread_local ShaderUniformScan lastScan{};thread_local std::uint64_t lastHash{};thread_local std::uint32_t lastWords{};
using D16=std::array<double,16>;bool invert(const D16& in,D16& out);
std::unordered_map<VkDevice,DeviceRecord> deviceRegistry;
std::atomic<bool> enabledValue{true},enabledRead{};
const float identityMatrix[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
// PassDesc of every engine render pass (coldMutex) and a lock-free table of the passes that have twins
// (open addressing, 256 slots, written under coldMutex, read by the worker per replayed pass and by the left Begin hook;
// each slot is a seqlock: odd seq = being written, the reader retries/fails).
struct AttachmentDesc {VkFormat format{};VkAttachmentLoadOp load{},stencilLoad{};VkAttachmentStoreOp store{},stencilStore{};VkImageLayout initial{},final{},reference{};bool referenced{};};
struct PassDesc {VkDevice device{};std::uint32_t count{},colors{},dependencies{};int depth{-1};std::array<AttachmentDesc,boundary::maxAttachments> att{};PassTwins twins{};bool shapeKnown{};};
std::unordered_map<VkRenderPass,PassDesc> passRegistry;
struct TwinSlot {std::atomic<VkRenderPass> key{},empty{},head{},tail{};std::atomic<std::uint32_t> flags{},seq{};};
constexpr unsigned twinTableSize=256;
std::array<TwinSlot,twinTableSize> twinTable;
VkRenderPass twinTombstone() noexcept {return reinterpret_cast<VkRenderPass>(std::uintptr_t(1));}
unsigned twinHash(VkRenderPass p) noexcept {return unsigned(((std::uint64_t(reinterpret_cast<std::uintptr_t>(p))>>4)*0x9E3779B97F4A7C15ull)>>56)&(twinTableSize-1);}
void twinSlotWrite(TwinSlot& s,VkRenderPass key,const PassTwins& t) noexcept { // caller holds coldMutex
    s.seq.fetch_add(1,std::memory_order_relaxed);std::atomic_thread_fence(std::memory_order_release); // odd: readers back off
    s.empty.store(t.empty,std::memory_order_relaxed);s.head.store(t.head,std::memory_order_relaxed);s.tail.store(t.tail,std::memory_order_relaxed);s.flags.store(t.flags,std::memory_order_relaxed);
    s.key.store(key,std::memory_order_relaxed);
    s.seq.fetch_add(1,std::memory_order_release);
}
bool twinTablePut(VkRenderPass pass,const PassTwins& t) noexcept { // caller holds coldMutex
    const unsigned h=twinHash(pass);int slot=-1;
    for(unsigned i=0;i<twinTableSize;++i){const unsigned k=(h+i)&(twinTableSize-1);const auto key=twinTable[k].key.load(std::memory_order_relaxed);
        if(key==pass){slot=int(k);break;}
        if(!key||key==twinTombstone()){if(slot<0)slot=int(k);if(!key)break;}}
    if(slot<0)return false;
    twinSlotWrite(twinTable[unsigned(slot)],pass,t);return true;
}
void twinTableErase(VkRenderPass pass) noexcept { // caller holds coldMutex
    const unsigned h=twinHash(pass);
    for(unsigned i=0;i<twinTableSize;++i){auto& s=twinTable[(h+i)&(twinTableSize-1)];const auto key=s.key.load(std::memory_order_relaxed);
        if(!key)return;if(key==pass){twinSlotWrite(s,twinTombstone(),PassTwins{});return;}}
}

VkPipelineLayout pushLayoutFor(VkDevice d){
    // caller holds coldMutex
    auto it=deviceRegistry.find(d);if(it==deviceRegistry.end())return VK_NULL_HANDLE;
    if(it->second.pushLayout)return it->second.pushLayout;
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT,0,64};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};ci.pushConstantRangeCount=1;ci.pPushConstantRanges=&range;
    VkPipelineLayout layout{};
    if(raw.createPipelineLayout(d,&ci,nullptr,&layout)!=VK_SUCCESS)return VK_NULL_HANDLE;
    it->second.pushLayout=layout;return layout;
}
thread_local VkDevice cachedPushDevice{};thread_local VkPipelineLayout cachedPushLayout{};
VkPipelineLayout cachedPushLayoutFor(VkDevice d){
    if(cachedPushDevice==d&&cachedPushLayout)return cachedPushLayout;
    std::lock_guard lock(coldMutex);cachedPushLayout=pushLayoutFor(d);cachedPushDevice=d;return cachedPushLayout;
}
bool isBindless(const VkDescriptorSetLayoutCreateInfo& ci,std::uint32_t& textureBinding){
    textureBinding=UINT32_MAX;
    for(std::uint32_t i=0;i<ci.bindingCount;++i){
        const auto& b=ci.pBindings[i];
        if((b.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER||b.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)&&b.descriptorCount>=32){
            if(textureBinding==UINT32_MAX||b.descriptorCount>ci.pBindings[textureBinding].descriptorCount)textureBinding=i;
        }
    }
    if(textureBinding==UINT32_MAX)return false;
    textureBinding=ci.pBindings[textureBinding].binding;return true;
}
}

// ================================================================= cold API
bool bindRaw(const RawCommands& r) noexcept {
    if(!r)return false;
    std::lock_guard lock(coldMutex);
    if(rawBound.load())return true;
    raw=r;rawBound.store(true,std::memory_order_release);return true;
}
bool enabled() noexcept {
    if(!enabledRead.load(std::memory_order_acquire)){
#ifdef __ANDROID__
        char value[PROP_VALUE_MAX]{};__system_property_get("debug.rdrvr.stereoreplay",value);
        enabledValue.store(!(value[0]=='0'&&value[1]==0),std::memory_order_release);
#endif
        enabledRead.store(true,std::memory_order_release);
    }
    return enabledValue.load(std::memory_order_acquire)&&rawBound.load(std::memory_order_acquire);
}
void gpuFrameBegin(VkDevice d,VkCommandBuffer c) noexcept;void gpuStageCommand(VkCommandBuffer c) noexcept;void gpuDeviceGone(VkDevice d,bool idle) noexcept;
void gpuNoteEyeImages(const FrameMaps& maps) noexcept;
void gpuLiteBegin(VkDevice d,VkCommandBuffer c) noexcept;void gpuLiteStageCommand(VkCommandBuffer c) noexcept;void gpuNoteDepthPass0(VkCommandBuffer c,std::uint32_t draws) noexcept;
#include "StereoGpuStages.inc"
void deviceCreated(VkDevice d,VkPhysicalDevice p) noexcept {
    Errno preserve;std::lock_guard lock(coldMutex);deviceRegistry[d]={p,VK_NULL_HANDLE};
}
void deviceDestroyed(VkDevice d) noexcept {
    Errno preserve;secondaryDeviceDestroyed(d); // forget secondary-path command pools (no Vulkan calls)
    gpuDeviceGone(d,false);                     // forget the GPU-stage query pool (no Vulkan calls)
    std::lock_guard lock(coldMutex);
    deviceRegistry.erase(d);
    { // Destroys still deferred went with the device (shutdownAfterIdle did not run or failed its idle)
        const auto before=deferredViews.size()+deferredFramebuffers.size()+deferredImages.size();
        deferredViews.erase(std::remove_if(deferredViews.begin(),deferredViews.end(),[&](const DeferredView& v){return v.device==d;}),deferredViews.end());
        deferredFramebuffers.erase(std::remove_if(deferredFramebuffers.begin(),deferredFramebuffers.end(),[&](const DeferredFramebuffer& f){return f.device==d;}),deferredFramebuffers.end());
        deferredImages.erase(std::remove_if(deferredImages.begin(),deferredImages.end(),[&](const DeferredImage& i){return i.device==d;}),deferredImages.end());
        stats.scrub.droppedAtDeviceLoss.fetch_add(before-deferredViews.size()-deferredFramebuffers.size()-deferredImages.size(),std::memory_order_relaxed);
        recentSources.clear();}
    // Deferred clone sets and the dummy went with the device (no Vulkan calls here)
    deferredSets.erase(std::remove_if(deferredSets.begin(),deferredSets.end(),[&](const DeferredSet& s){return s.device==d;}),deferredSets.end());
    if(dummy.device==d){dummyReady.store(false,std::memory_order_release);dummy=Dummy{};dummyFailed.store(false,std::memory_order_relaxed);}
    destroyedImages.clear();staleHolders.clear();
    for(auto it=setRegistry.begin();it!=setRegistry.end();)if(it->second.device==d)it=setRegistry.erase(it);else ++it;
    for(auto it=framebufferRegistry.begin();it!=framebufferRegistry.end();)if(it->second.device==d)it=framebufferRegistry.erase(it);else ++it;
    for(auto it=viewRegistry.begin();it!=viewRegistry.end();)if(it->second.device==d)it=viewRegistry.erase(it);else ++it;
    for(auto it=layoutRegistry.begin();it!=layoutRegistry.end();)if(it->second.device==d)it=layoutRegistry.erase(it);else ++it;
    for(auto it=passRegistry.begin();it!=passRegistry.end();)if(it->second.device==d){twinTableErase(it->first);it=passRegistry.erase(it);}else ++it; // (twins went with the device)
    texScanByModule.clear();texScanByPipeline.clear(); // (modules and pipelines went with the device)
    cachedPushDevice={};cachedPushLayout={};
}
void shutdownAfterIdle(VkDevice d) noexcept {
    Errno preserve;secondaryShutdownAfterIdle(d); // destroy secondary-path command pools
    gpuDeviceGone(d,true);                        // destroy the GPU-stage query pool (device idle)
    { // The device is idle - every deferred destroy of this device now (framebuffers before their views, views before their images)
        std::vector<DeferredView> views;std::vector<DeferredFramebuffer> framebuffers;std::vector<DeferredImage> images;
        try{std::lock_guard lock(coldMutex);
            for(std::size_t k=0;k<deferredFramebuffers.size();)if(deferredFramebuffers[k].device==d){framebuffers.push_back(deferredFramebuffers[k]);deferredFramebuffers[k]=deferredFramebuffers.back();deferredFramebuffers.pop_back();}else ++k;
            for(std::size_t k=0;k<deferredViews.size();)if(deferredViews[k].device==d){views.push_back(deferredViews[k]);deferredViews[k]=deferredViews.back();deferredViews.pop_back();}else ++k;
            for(std::size_t k=0;k<deferredImages.size();)if(deferredImages[k].device==d){images.push_back(deferredImages[k]);deferredImages[k]=deferredImages.back();deferredImages.pop_back();}else ++k;
            recentSources.clear();
        }catch(...){}
        for(const auto& f:framebuffers){raw.destroyFramebuffer(f.device,f.framebuffer,nullptr);++stats.scrub.teardownFramebufferFlushes;++stats.scrub.deferredFramebufferFlushes;}
        for(const auto& v:views){v.forward();++stats.scrub.teardownViewFlushes;++stats.scrub.deferredViewFlushes;}
        for(const auto& i:images){i.forward();++stats.scrub.teardownImageFlushes;++stats.scrub.deferredImageFlushes;}
    }
    std::lock_guard lock(coldMutex);
    releaseDeferredSets(d,true,0); // The device is idle - old clone sets now (before their shared pools go below)
    for(auto it=setRegistry.begin();it!=setRegistry.end();){
        if(it->second.device==d){if(it->second.clonePool)raw.destroyDescriptorPool(d,it->second.clonePool,nullptr);
            for(const auto pool:it->second.ownPool)if(pool)raw.destroyDescriptorPool(d,pool,nullptr);
            it=setRegistry.erase(it);}else ++it;
    }
    if(dummy.device==d){ // The 1x1 dummy dies before the device (VUID-vkDestroyDevice-device-05137). The
        // device is idle and the clones that held it are gone; the engine sets that may still name it are never bound again.
        // Destroyed through the loader exports that created it (a new device creates a new one: dummyDescriptorWanted()).
        const Dummy x=dummy;dummyReady.store(false,std::memory_order_release);dummy=Dummy{};dummy.status="DESTROYED";dummyFailed.store(false,std::memory_order_release);
        if(x.sampler)vkDestroySampler(d,x.sampler,nullptr);if(x.view)vkDestroyImageView(d,x.view,nullptr);
        if(x.image)vkDestroyImage(d,x.image,nullptr);if(x.memory)vkFreeMemory(d,x.memory,nullptr);++t2.dummyDestroyed;}
    for(auto it=framebufferRegistry.begin();it!=framebufferRegistry.end();){
        if(it->second.device==d&&it->second.owned){raw.destroyFramebuffer(d,it->first,nullptr);it=framebufferRegistry.erase(it);}else ++it;
    }
    auto dev=deviceRegistry.find(d);
    if(dev!=deviceRegistry.end()&&dev->second.pushLayout){raw.destroyPipelineLayout(d,dev->second.pushLayout,nullptr);dev->second.pushLayout={};}
    cachedPushDevice={};cachedPushLayout={};
}

// ================================================================= creation observers
bool patchShaderCreate(const VkShaderModuleCreateInfo* original,const VkShaderModuleCreateInfo** patched) noexcept {
    Errno preserve;
    thread_local std::vector<std::uint32_t> storage;thread_local VkShaderModuleCreateInfo info;
    if(!enabled()||!original||!patched||original->sType!=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO||original->pNext||original->flags||
       !original->pCode||original->codeSize<20||original->codeSize%4)return false;
    try{
        PatchReport report{};
        const auto status=patchEyePosition(original->pCode,original->codeSize/4,storage,report);lastPatchReport=report;lastPatchReport.status=status;lastPatchValid=true;
        scanCameraMembers(original->pCode,original->codeSize/4,lastScan);
        merge::scanTextureRefs(original->pCode,original->codeSize/4,lastTexScan); // Bindless texture indices read (M8)
        {std::uint64_t h=1469598103934665603ull;const std::uint32_t n=std::uint32_t(original->codeSize/4);for(std::uint32_t i=0;i<n;++i){h^=original->pCode[i];h*=1099511628211ull;}lastHash=h;lastWords=n;}
        ++stats.patchStatus[unsigned(status)<stats.patchStatus.size()?unsigned(status):0];
        if(status==PatchStatus::NotVertex){++stats.shadersNotVertex;return false;}
        if(status!=PatchStatus::Patched){++stats.shadersDeclined;return false;}
        info=*original;info.pCode=storage.data();info.codeSize=storage.size()*4;*patched=&info;++stats.shadersPatched;return true;
    }catch(...){return false;}
}
bool patchPipelineLayoutCreate(const VkPipelineLayoutCreateInfo* original,const VkPipelineLayoutCreateInfo** patched) noexcept {
    Errno preserve;
    thread_local VkPipelineLayoutCreateInfo info;thread_local VkPushConstantRange range;
    if(!enabled()||!original||!patched||original->sType!=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO||original->pushConstantRangeCount)return false;
    range={VK_SHADER_STAGE_VERTEX_BIT,0,64};
    info=*original;info.pushConstantRangeCount=1;info.pPushConstantRanges=&range;*patched=&info;++stats.layoutsPatched;return true;
}
void imageViewCreated(VkDevice d,const VkImageViewCreateInfo* c,VkImageView view,VkResult result) noexcept {
    Errno preserve;if(result!=VK_SUCCESS||!c||!view)return;
    try{std::lock_guard lock(coldMutex);viewRegistry[view]={d,c->image,c->viewType,c->format,c->subresourceRange};++stats.viewsTracked;
        if(!destroyedImages.empty())destroyedImages.erase(c->image); // A reused image handle is alive again
        if(!staleHolders.empty()){auto held=staleHolders.find(view); // A destroyed view's handle comes back
            if(held!=staleHolders.end()){bool still=false;
                for(const auto& [set,key]:held->second){auto rec=setRegistry.find(set);const std::uint32_t b=key>>24,e=key&0xffffffu;
                    still|=rec!=setRegistry.end()&&b<rec->second.elementView.size()&&e<rec->second.elementView[b].size()&&rec->second.elementView[b][e]==view;}
                if(still)++t2.reusedStaleHandles;staleHolders.erase(held);}}}catch(...){}
}
bool imageViewDestroyed(VkDevice d,VkImageView view,const VkAllocationCallbacks* allocator,PFN_vkDestroyImageView destroy) noexcept {
    Errno preserve;if(!view)return false;
    bool deferred=false;char line[200]{},engineLine[200]{}; // Logcat lines of a stale override view / engine slot (written under the lock, logged after it)
    char p1Line[256]{}; // Stale engine view with a companion (first 64, then every 256th)
    ensureIdentity(d);  // (outside coldMutex; once)
    framebufferGeneration.fetch_add(1);try{
        thread_local std::vector<VkCopyDescriptorSet> copies;copies.clear();
        thread_local DummyWrites dw;dw.clear(); // Dummy writes (scrub of a dead source, engine scrub)
        thread_local std::vector<std::pair<VkDescriptorSet,std::uint32_t>> holders;holders.clear(); // engine set, binding<<24|element
        const bool act=scrubOn();const std::uint64_t frame=scrubFrame.load(std::memory_order_relaxed);
        const bool endOverlap=primaryEndsActive.load(std::memory_order_acquire)&&!primaryEndThisThread; // P2 (before the lock)
        std::lock_guard lock(coldMutex);
        VkImage image{};VkFormat viewFormat{};{auto vr=viewRegistry.find(view);if(vr!=viewRegistry.end()){image=vr->second.image;viewFormat=vr->second.format;viewRegistry.erase(vr);}} // Kept with a deferred view
        bool engineDefer=false; // A destroyed companion view still in engine elements
        // Probe (count only): engine-set elements still holding this view. Clone / clone2 mirror them one to one
        // (unless overridden there) and the driver's End walk covers every element of every bound set, so an End-walk fault with
        // staleOverrideViews == 0 but staleEngineSlots > 0 is a stale engine / mirrored slot, not a stale override view.
        // O(elements of the view's image) per record via elementsOf (the 65536-element cap of elementImage applies).
        {unsigned engineSlots=0;std::uint32_t lastBinding=0,lastElement=0;
            if(image)for(const auto& entry:setRegistry){const auto& record=entry.second;if(record.device!=d)continue;
                const auto keys=record.elementsOf.find(image);if(keys==record.elementsOf.end())continue;
                for(const std::uint32_t key:keys->second){const std::uint32_t b=key>>24,e=key&0xffffffu;
                    if(b<record.elementView.size()&&e<record.elementView[b].size()&&record.elementView[b][e]==view){++engineSlots;lastBinding=b;lastElement=e;holders.push_back({entry.first,key});}}}
            if(engineSlots){const auto n=++stats.scrub.staleEngineViews;stats.scrub.staleEngineSlots.fetch_add(engineSlots,std::memory_order_relaxed);
                stats.scrub.lastStaleEngineView.store(std::uint64_t(reinterpret_cast<std::uintptr_t>(view)),std::memory_order_relaxed);
                stats.scrub.lastStaleEngineSlot.store((std::uint64_t(lastBinding)<<32)|lastElement,std::memory_order_relaxed);stats.scrub.lastStaleEngineFrame.store(frame,std::memory_order_relaxed);
                if(n<=16||(n&1023u)==0)std::snprintf(engineLine,sizeof engineLine,"stale engine slot view %llx (image %llx): frame %llu, %u engine slot(s), last binding %u element %u (#%llu)",
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(view)),static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(image)),
                    static_cast<unsigned long long>(frame),engineSlots,lastBinding,lastElement,static_cast<unsigned long long>(n));
                // The view's companion, read through the safe reader BEFORE the destroy is
                // forwarded (the driver object is still alive here; only the armed driver build
                // gives +0x1b8 a meaning). The Adreno draw-time scan lists only companion views,
                // so only those can reach the End walk.
                bool companionRead=false;const std::uintptr_t companion=readCompanion(view,companionRead);
                if(!companionRead)++t2.companionUnread;
                else if(companion){const auto c=++t2.staleEngineCompanionViews;
                    if(c<=64||(c&255u)==0){
#ifdef __ANDROID__
                        const long tid=long(gettid());
#else
                        const long tid=0;
#endif
                        std::snprintf(p1Line,sizeof p1Line,"stale engine companion view %llx (image %llx, format %d, companion %llx): tid %ld%s, frame %llu, binding %u element %u, %u slot(s), end overlap %d (#%llu)",
                            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(view)),static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(image)),int(viewFormat),
                            static_cast<unsigned long long>(companion),tid,tid&&tid==t2.renderTid.load(std::memory_order_relaxed)?" (render)":"",static_cast<unsigned long long>(frame),
                            lastBinding,lastElement,engineSlots,endOverlap?1:0,static_cast<unsigned long long>(c));}}
                // P3: per world frame (frameHook count) for the first 64 world frames; before the first one: burstBefore
                {const unsigned f=t2.burstFrames.load(std::memory_order_relaxed);if(!f)++t2.burstBefore;else if(f<=t2.burst.size())++t2.burst[f-1];}
                // Engine scrub: overwrite every engine element holding this companion view (and
                // the clone / clone2 mirror of it, unless that clone slot holds an override copy)
                // with the dummy view. Vulkan: writing an element of a set that a command buffer
                // being recorded has bound needs UPDATE_AFTER_BIND on the binding; one bound in a
                // PENDING command buffer needs UPDATE_UNUSED_WHILE_PENDING and the element unused
                // by that command buffer - true here: the engine destroys a texture only after its
                // last use retired (d11 DeferredDeleteList), and FreeDescriptor runs after
                // vkDestroyImageView, so no engine write of the index can race this one (both
                // under coldMutex). Without both flags on the binding nothing is written
                // (counted). Command buffers recorded earlier may have listed the view at earlier
                // draws (the write cannot take that back), so the destroy itself is deferred past
                // retire too. RDR1's bindless bindings have flags 5 (no UNUSED_WHILE_PENDING): the
                // write is refused there (refusedFlags = slots, writes 0) and only the deferral
                // acts. Both act only while flagRecent().
                if(companionRead&&companion&&engineScrubOn()&&!flagRecent(d))++t2.engineScrubUnflagged; // No flag now or recently - nothing listed it
                else if(companionRead&&companion&&engineScrubOn()){engineDefer=true;
                    for(const auto& [engineSet,key]:holders){auto rec=setRegistry.find(engineSet);if(rec==setRegistry.end())continue;auto& record=rec->second;
                        const std::uint32_t b=key>>24,e=key&0xffffffu;if(b>=8)continue;
                        constexpr VkDescriptorBindingFlags need=VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT|VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
                        if((record.bindingFlagBits[b]&need)!=need){++t2.engineScrubRefusedFlags;continue;}
                        if(!dw.add(engineSet,b,e,record.bindingTypes[b])){++t2.engineScrubRefusedNoDummy;continue;}
                        ++t2.engineScrubSlots;
                        for(unsigned o=0;o<2;++o){bool overridden=false;for(const auto& x:record.overrides[o])overridden|=x.binding==b&&x.element==e;
                            if(!overridden&&dw.add(o?record.clone2:record.clone,b,e,record.bindingTypes[b]))++t2.engineScrubWrites;}
                        ++t2.engineScrubWrites;
                        setElementImage(record,b,e,VK_NULL_HANDLE,dummy.view);noteElementLayout(record,b,e,dummy.layout,record.bindingTypes[b]==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER?dummy.sampler:VK_NULL_HANDLE);}}
                if(staleHolders.size()>=65536u){staleHolders.clear();++t2.staleHoldersReset;}
                {auto& list=staleHolders[view];list.clear();
                 for(const auto& hk:holders){auto rec=setRegistry.find(hk.first);const std::uint32_t b=hk.second>>24,e=hk.second&0xffffffu;
                     if(rec!=setRegistry.end()&&b<rec->second.elementView.size()&&e<rec->second.elementView[b].size()&&rec->second.elementView[b][e]==view)list.push_back(hk);}
                 if(list.empty())staleHolders.erase(view);}
            }}
        if(endOverlap){bool read=false;const auto c=readCompanion(view,read);++t2.endOverlapViews[c?0:1];} // P2: destroy on another thread during an engine vkEndCommandBuffer
        // (3) scrub: every clone slot still holding an override copy of this view gets the engine's own element
        // back (legal while the clone is bound: UPDATE_AFTER_BIND pool), so no command buffer recorded from now on binds it.
        // The entry is dropped in both modes (the handle becomes reusable once the driver destroyed it).
        bool live=false,liveInWindow=false;unsigned slots=0;std::uint64_t applied=0;
        for(auto& [set,record]:setRegistry){if(record.device!=d)continue;
            for(unsigned o=0;o<2;++o){auto& entries=record.overrides[o];
                for(std::size_t k=0;k<entries.size();){const auto e=entries[k];if(e.source!=view){++k;continue;}
                    live=true;liveInWindow|=e.frame+scrubWindow>=frame;++slots;applied=std::max(applied,e.frame);entries[k]=entries.back();entries.pop_back();
                    if(!act){++stats.scrub.countOnlyScrubs;continue;}
                    const auto& bw=record.bindingWritten[e.binding];
                    // The entry is popped already, so the slot must not keep the dying view
                    // untracked. The scrub copies engine[element] only when that is a LIVE view other than this one (this view
                    // left viewRegistry above); otherwise the dummy is written (no companion, never destroyed) - never the
                    // dead element. Without a dummy (not created / failed): no copy, counted.
                    const VkImageView src=e.element<record.elementView[e.binding].size()?record.elementView[e.binding][e.element]:VK_NULL_HANDLE;
                    const bool written=e.element<bw.size()&&bw[e.element],srcLive=written&&src!=view&&liveView(src);
                    noteCopySource(o,2,e.binding,e.element,e.element,src,srcLive,true,srcLive,frame);
                    if(!srcLive){if(!written)++stats.scrub.scrubsUnwritten;else ++t2.scrubsStaleSource;
                        if(!updateAfterBind(record,e.binding))++t2.dummyRefusedFlags; // Binding without UPDATE_AFTER_BIND: not written (counted)
                        else if(dw.add(o?record.clone2:record.clone,e.binding,e.element,record.bindingTypes[e.binding]))++t2.dummyWrites;else ++t2.dummyMissing;
                        continue;}
                    VkCopyDescriptorSet cp{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};cp.srcSet=set;cp.srcBinding=e.binding;cp.srcArrayElement=e.element;
                    cp.dstSet=o?record.clone2:record.clone;cp.dstBinding=e.binding;cp.dstArrayElement=e.element;cp.descriptorCount=1;copies.push_back(cp);++stats.scrub.overrideScrubs;}}}
        const bool recent=recentSource(view,frame);
        if(live||recent){const auto n=++stats.scrub.staleOverrideViews;if(live)++stats.scrub.staleLive;else ++stats.scrub.staleRecent;if(liveInWindow)++stats.scrub.staleLiveInWindow;
            stats.scrub.lastStaleView.store(std::uint64_t(reinterpret_cast<std::uintptr_t>(view)),std::memory_order_relaxed);stats.scrub.lastStaleFrame.store(frame,std::memory_order_relaxed);
            if(n<=32||(n&255u)==0)std::snprintf(line,sizeof line,"stale override view %llx: frame %llu, %s, %u clone slot(s), last applied %llu, scrub %d (#%llu)",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(view)),static_cast<unsigned long long>(frame),live?"installed":"recent",slots,
                static_cast<unsigned long long>(applied),act?1:0,static_cast<unsigned long long>(n));}
        recentSources.erase(std::remove_if(recentSources.begin(),recentSources.end(),[&](const RecentSource& s){return s.view==view;}),recentSources.end());
        // (5) mod-owned right framebuffers on this view: destroyed once their frame retired (scrub), at once otherwise
        bool attachment=false;
        for(auto it=framebufferRegistry.begin();it!=framebufferRegistry.end();){
            bool uses=false;for(std::uint32_t i=0;i<it->second.count;++i)uses|=it->second.views[i]==view;
            if(uses&&it->second.owned){attachment=true;
                if(act){deferredFramebuffers.push_back({d,it->first,frame});++stats.scrub.deferredFramebufferDestroys;}
                else{raw.destroyFramebuffer(d,it->first,nullptr);++stats.scrub.framebuffersDestroyedNow;}
                it=framebufferRegistry.erase(it);}else ++it;
        }
        // (4) the view itself: kept alive while a command buffer that bound it (or a deferred framebuffer using it)
        // may still be recording / pending; the driver's vkEndCommandBuffer walk dereferences every view of the bound sets.
        // A destroyed companion view still held by engine elements is deferred as well
        // (enginescrub, any scrub mode): command buffers recorded before the engine scrub may have
        // listed it for their End walk. Only while flagRecent(); otherwise forwarded at once (counted,
        // engineScrub.unflagged).
        if(live||recent||attachment||engineDefer){
            if(!act&&!engineDefer)++stats.scrub.countOnlyDeferrals;
            else if(!destroy)++stats.scrub.viewDeferUnavailable;
            else{DeferredView v{d,view,{},allocator!=nullptr,destroy,frame,image};if(allocator)v.callbacks=*allocator;deferredViews.push_back(v);++stats.scrub.deferredViewDestroys;deferred=true;
                if(engineDefer)++t2.engineScrubDeferredViews;}
        }
        notePending();
        // under coldMutex (serialized with applyBindlessOverrides and the engine's own writes); writes before copies (Vulkan
        // order), and no copy here reads an element written here (copies read override slots l, writes hit the view's slots)
        if(!copies.empty()||!dw.writes.empty())raw.updateDescriptorSets(d,std::uint32_t(dw.writes.size()),dw.data(),std::uint32_t(copies.size()),copies.data());
    }catch(...){}
#ifdef __ANDROID__
    if(line[0])__android_log_print(ANDROID_LOG_WARN,"RdrVrStereo","%s%s",line,deferred?", destroy deferred":"");
    if(engineLine[0])__android_log_print(ANDROID_LOG_WARN,"RdrVrStereo","%s%s",engineLine,deferred?", destroy deferred":"");
    if(p1Line[0])__android_log_print(ANDROID_LOG_WARN,"RdrVrStereo","%s%s",p1Line,deferred?", destroy deferred":"");
#else
    (void)p1Line;
#endif
    return deferred;
}
void imageDestroyed(VkDevice d,VkImage image) noexcept {
    Errno preserve;if(!image)return;
    if(primaryEndsActive.load(std::memory_order_acquire)&&!primaryEndThisThread)++t2.endOverlapImages;
    try{const bool act=scrubOn();const std::uint64_t frame=scrubFrame.load(std::memory_order_relaxed);bool removed=false;
        std::lock_guard lock(coldMutex);
        if(destroyedImages.size()>=65536u)destroyedImages.clear(); // bound (a cleared entry only relaxes liveView: no destroyed-image check)
        destroyedImages.insert(image); // From here on its views are not written / copied as live sources (liveView)
        for(auto it=framebufferRegistry.begin();it!=framebufferRegistry.end();){
            bool uses=false;
            for(std::uint32_t i=0;i<it->second.count;++i){auto v=viewRegistry.find(it->second.views[i]);uses|=v!=viewRegistry.end()&&v->second.image==image;}
            if(uses&&it->second.owned){ // (5): deferred like imageViewDestroyed's
                if(act){deferredFramebuffers.push_back({d,it->first,frame});++stats.scrub.deferredFramebufferDestroys;}
                else{raw.destroyFramebuffer(d,it->first,nullptr);++stats.scrub.framebuffersDestroyedNow;}
                removed=true;it=framebufferRegistry.erase(it);}else ++it;
        }
        if(removed){framebufferGeneration.fetch_add(1);notePending();} // The replay's framebuffer caches drop the handle
    }catch(...){}
}
bool imageDestroyDeferred(VkDevice d,VkImage image,const VkAllocationCallbacks* allocator,PFN_vkDestroyImage destroy) noexcept {
    // A deferred view keeps the driver's pointer to its image ([view+0xf0]; the End walk reads [image+0x148] and
    // [image+0x105]); vkCreateImageView takes no reference, so the image is deferred with its pending views (any scrub mode:
    // views are only pending when scrub=1 deferred them). Not deferred: the image's memory (vkFreeMemory is not hooked).
    Errno preserve;if(!d||!image||!destroy)return false;
    ensureIdentity(d); // (outside coldMutex; once)
    try{std::lock_guard lock(coldMutex);
        bool any=false,engine=false;std::uint64_t stamp=0;
        for(const auto& v:deferredViews)if(v.device==d&&v.image==image){any=true;stamp=std::max(stamp,v.frame);}
        // The engine destroys the image BEFORE its view. When a still-live view of this image sits
        // in an engine element and has a companion, the view's later destroy is deferred (engine scrub) and the End walk of a
        // command buffer that listed it reads [view+0xf0] = this image: defer the image with it (released after its views).
        if(!any&&engineScrubOn()&&driverArmed()&&flagRecent(d)) // Only while a flag is or was recently present
            for(const auto& entry:setRegistry){const auto& record=entry.second;if(record.device!=d||any)continue;
                const auto keys=record.elementsOf.find(image);if(keys==record.elementsOf.end())continue;
                for(const std::uint32_t key:keys->second){const std::uint32_t b=key>>24,e=key&0xffffffu;
                    const VkImageView v=b<record.elementView.size()&&e<record.elementView[b].size()?record.elementView[b][e]:VK_NULL_HANDLE;
                    if(!v||!viewRegistry.count(v))continue;
                    bool read=false;if(readCompanion(v,read)){any=engine=true;stamp=scrubFrame.load(std::memory_order_relaxed);break;}}}
        if(!any)return false;
        DeferredImage x{d,image,{},allocator!=nullptr,destroy,stamp};if(allocator)x.callbacks=*allocator;deferredImages.push_back(x);
        ++stats.scrub.deferredImageDestroys;if(engine)++t2.engineScrubDeferredImages;notePending();return true;
    }catch(...){return false;}
}
void frameHook(std::uint64_t frame,std::uint64_t retired) noexcept {
    Errno preserve;refreshScrubProperty();++stats.scrub.hooks;
    refreshT2Properties(); // setprobe / contain / clonerebuild / enginescrub: 4 property reads per world frame
#ifdef __ANDROID__
    t2.renderTid.store(long(gettid()),std::memory_order_relaxed); // P1 log: "(render)" marks a destroy on this thread
#endif
    {const unsigned f=t2.burstFrames.load(std::memory_order_relaxed);if(f<=t2.burst.size()){if(!f)t2.burstFirstFrame=frame;t2.burstFrames.store(f+1,std::memory_order_relaxed);}} // P3
    std::vector<DeferredView> views;std::vector<DeferredFramebuffer> framebuffers;std::vector<DeferredImage> images;
    {VkDevice any{};try{std::lock_guard lock(coldMutex);if(!setRegistry.empty())any=setRegistry.begin()->second.device;}catch(...){}ensureIdentity(any);}
    try{std::lock_guard lock(coldMutex);
        const std::uint64_t previous=scrubFrame.exchange(frame,std::memory_order_relaxed);scrubRetired.store(retired,std::memory_order_relaxed);
        if(frame<previous){ // a new frame-id epoch (session restart): restamp so nothing waits for an id that never retires
            ++stats.scrub.frameResets;for(auto& v:deferredViews)v.frame=frame;for(auto& f:deferredFramebuffers)f.frame=frame;for(auto& s:recentSources)s.frame=frame;for(auto& i:deferredImages)i.frame=frame;
            for(auto& s:deferredSets)s.frame=frame;
            for(auto& [s,r]:setRegistry)r.probe.lastRebuild={}; // Rebuild spacing restarts with the new ids
            if(t2.lastAnyFlagFrame)t2.lastAnyFlagFrame=frame;}   // A flag seen in the old epoch stays recent until this frame retires
        // Probe: [set+0x288] bit 2 and [set+0x120] of every bindless engine set, its clone and clone2
        // (volatile loads of armed handles only; a flagged clone is contained and rebuilt by cloneFlagged at the gates)
        if(driverArmed()){++t2.probeFrames;
            for(auto& [set,record]:setRegistry){armRecord(set,record);
                for(unsigned k=0;k<3;++k){auto& p=record.probe;if(!p.armed[k])continue;const VkDescriptorSet s=probeSet(set,record,k);
                    const bool f=(liveFlag(s)&drv::companionBit)!=0;p.count[k]=liveCount(s);p.flag[k]=f?1:0;if(f){++p.flaggedFrames[k];noteFlag(k,frame);noteAnyFlag(frame);}}}}
        releaseDeferredSets(VK_NULL_HANDLE,false,retired); // Old clone sets whose frame retired (under coldMutex: see there)
        auto due=[&](std::uint64_t stamp){return stamp+scrubWindow<=retired;};
        for(std::size_t k=0;k<deferredFramebuffers.size();)if(due(deferredFramebuffers[k].frame)){framebuffers.push_back(deferredFramebuffers[k]);deferredFramebuffers[k]=deferredFramebuffers.back();deferredFramebuffers.pop_back();}else ++k;
        for(std::size_t k=0;k<deferredViews.size();)if(due(deferredViews[k].frame)){views.push_back(deferredViews[k]);deferredViews[k]=deferredViews.back();deferredViews.pop_back();}else ++k;
        // An image goes only after every view of it (also one deferred after the image's own destroy) was forwarded
        for(std::size_t k=0;k<deferredImages.size();){const DeferredImage i=deferredImages[k];bool held=!due(i.frame);
            for(std::size_t j=0;!held&&j<deferredViews.size();++j)held=deferredViews[j].device==i.device&&deferredViews[j].image==i.image;
            if(held){++k;continue;}images.push_back(i);deferredImages[k]=deferredImages.back();deferredImages.pop_back();}
        recentSources.erase(std::remove_if(recentSources.begin(),recentSources.end(),[&](const RecentSource& s){return s.frame+scrubWindow<frame;}),recentSources.end());
    }catch(...){}
    for(const auto& f:framebuffers){raw.destroyFramebuffer(f.device,f.framebuffer,nullptr);++stats.scrub.deferredFramebufferFlushes;} // framebuffers before their views
    for(const auto& v:views){v.forward();++stats.scrub.deferredViewFlushes;}
    for(const auto& i:images){i.forward();++stats.scrub.deferredImageFlushes;} // Images after their views
}
void setScrubForTests(int mode) noexcept {scrubForced.store(mode<0?-1:(mode?1:0),std::memory_order_relaxed);}
unsigned deferredPending() noexcept {try{std::lock_guard lock(coldMutex);return unsigned(deferredViews.size()+deferredFramebuffers.size()+deferredImages.size());}catch(...){return 0;}}
void framebufferCreated(VkDevice d,const VkFramebufferCreateInfo* c,VkFramebuffer fb,VkResult result) noexcept {
    Errno preserve;if(result!=VK_SUCCESS||!c||!fb||c->attachmentCount>8||(c->flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))return;
    try{std::lock_guard lock(coldMutex);FramebufferRecord r{d,c->renderPass,c->width,c->height,c->layers,c->attachmentCount,{},false};
        for(std::uint32_t i=0;i<c->attachmentCount;++i)r.views[i]=c->pAttachments[i];
        framebufferRegistry[fb]=r;++stats.framebuffersTracked;}catch(...){}
}
void setPassThroughTranslator(PassThroughTranslator t) noexcept {passThroughTranslator.store(t,std::memory_order_release);framebufferGeneration.fetch_add(1);}
void framebufferDestroyed(VkDevice,VkFramebuffer fb) noexcept {
    Errno preserve;if(!fb)return;framebufferGeneration.fetch_add(1);try{std::lock_guard lock(coldMutex);framebufferRegistry.erase(fb);}catch(...){}
}
void renderPassCreated(VkDevice d,VkRenderPass pass,const VkRenderPassCreateInfo* c,const PassTwins& twins) noexcept {
    Errno preserve;if(!pass||!c)return;
    try{PassDesc desc{};desc.device=d;desc.twins=twins;desc.dependencies=c->dependencyCount;
        std::array<VkImageLayout,boundary::maxAttachments> ref{};std::array<bool,boundary::maxAttachments> used{};
        desc.shapeKnown=boundary::referenceLayouts(*c,ref,used);
        desc.count=c->pAttachments?std::min<std::uint32_t>(c->attachmentCount,boundary::maxAttachments):0;
        if(c->subpassCount&&c->pSubpasses){const auto& s=c->pSubpasses[0];desc.colors=s.colorAttachmentCount;
            if(s.pDepthStencilAttachment&&s.pDepthStencilAttachment->attachment!=VK_ATTACHMENT_UNUSED)desc.depth=int(s.pDepthStencilAttachment->attachment);}
        for(std::uint32_t i=0;i<desc.count;++i){const auto& a=c->pAttachments[i];
            desc.att[i]={a.format,a.loadOp,a.stencilLoadOp,a.storeOp,a.stencilStoreOp,a.initialLayout,a.finalLayout,used[i]?ref[i]:VK_IMAGE_LAYOUT_UNDEFINED,used[i]};}
        const bool hasTwins=twins.empty||twins.head||twins.tail;
        std::lock_guard lock(coldMutex);passRegistry[pass]=desc;++stats.boundary.passDescs;
        if(hasTwins){if(twinTablePut(pass,twins))++stats.boundary.twinsCreated;else ++stats.boundary.tableFull;} // table full: the twins stay in the registry (destroyed with P)
    }catch(...){}
}
bool renderPassDestroyed(VkDevice,VkRenderPass pass,PassTwins& twins) noexcept {
    Errno preserve;twins=PassTwins{};if(!pass)return false;
    try{std::lock_guard lock(coldMutex);auto it=passRegistry.find(pass);if(it==passRegistry.end())return false;
        twins=it->second.twins;passRegistry.erase(it);twinTableErase(pass); // readers stop finding the twins before the caller destroys them
        const bool any=twins.empty||twins.head||twins.tail;if(any)++stats.boundary.twinsDestroyed;return any;
    }catch(...){return false;}
}
void noteTwinFailure() noexcept {++stats.boundary.twinFailures;}
bool twinsFor(VkRenderPass pass,PassTwins& out) noexcept {
    if(!pass||pass==twinTombstone())return false;
    const unsigned h=twinHash(pass);
    for(unsigned i=0;i<twinTableSize;++i){auto& s=twinTable[(h+i)&(twinTableSize-1)];
        const auto key=s.key.load(std::memory_order_acquire);if(!key)return false;if(key!=pass)continue;
        for(unsigned attempt=0;attempt<4;++attempt){ // seqlock read (see twinSlotWrite)
            const auto s1=s.seq.load(std::memory_order_acquire);if(s1&1u)continue;
            PassTwins t{s.empty.load(std::memory_order_relaxed),s.head.load(std::memory_order_relaxed),s.tail.load(std::memory_order_relaxed),s.flags.load(std::memory_order_relaxed)};
            const auto again=s.key.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(s.seq.load(std::memory_order_relaxed)!=s1)continue;
            if(again!=pass)return false;
            out=t;return t.empty||t.head||t.tail;}
        return false;}
    return false;
}
void descriptorLayoutCreated(VkDevice d,const VkDescriptorSetLayoutCreateInfo* c,VkDescriptorSetLayout layout,VkResult result) noexcept {
    Errno preserve;if(result!=VK_SUCCESS||!c||!layout||!c->pBindings)return;
    try{
        std::uint32_t textureBinding{};const bool bindless=isBindless(*c,textureBinding);
        LayoutRecord r{d,bindless,c->flags,{},{},textureBinding};
        r.bindings.assign(c->pBindings,c->pBindings+c->bindingCount);
        {std::vector<std::pair<std::uint32_t,std::uint32_t>> dyn;for(const auto& b:r.bindings)if(b.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC||b.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC||b.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||b.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)dyn.push_back({b.binding,b.descriptorCount}); // DUD converts the engine's uniform bindings to dynamic ones
         std::sort(dyn.begin(),dyn.end());for(const auto& [binding,count]:dyn)for(std::uint32_t k=0;k<count;++k)r.dynamicBindings.push_back(binding);}
        pipelineGeneration.fetch_add(1);
        if(bindless){
            for(const auto* next=static_cast<const VkBaseInStructure*>(c->pNext);next;next=next->pNext){
                if(next->sType==VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO){
                    const auto* f=reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(next);
                    if(f->bindingCount==c->bindingCount&&f->pBindingFlags)r.bindingFlags.assign(f->pBindingFlags,f->pBindingFlags+f->bindingCount);
                }
            }
            ++stats.bindlessLayouts;
        }
        std::lock_guard lock(coldMutex);layoutRegistry[layout]=std::move(r);
    }catch(...){}
}
void descriptorLayoutDestroyed(VkDevice,VkDescriptorSetLayout layout) noexcept {
    Errno preserve;try{std::lock_guard lock(coldMutex);layoutRegistry.erase(layout);}catch(...){}
}
void descriptorSetsAllocated(VkDevice d,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* out,VkResult result) noexcept {
    Errno preserve;if(result!=VK_SUCCESS||!info||!out||!enabled())return;
    ensureIdentity(d); // The allocFlag read below needs the driver identity (outside coldMutex; once)
    try{
        std::unique_lock lock(coldMutex,std::try_to_lock);if(!lock.owns_lock()){noteColdWait();lock.lock();}
        const VkDescriptorSetVariableDescriptorCountAllocateInfo* variable{};
        for(const auto* next=static_cast<const VkBaseInStructure*>(info->pNext);next;next=next->pNext)
            if(next->sType==VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO)variable=reinterpret_cast<const VkDescriptorSetVariableDescriptorCountAllocateInfo*>(next);
        for(std::uint32_t i=0;i<info->descriptorSetCount;++i){
            auto layout=layoutRegistry.find(info->pSetLayouts[i]);
            if(layout==layoutRegistry.end()||!layout->second.bindless||!out[i])continue;
            const auto& L=layout->second;
            std::vector<VkDescriptorPoolSize> sizes;
            for(const auto& b:L.bindings){bool merged=false;for(auto& s:sizes)if(s.type==b.descriptorType){s.descriptorCount+=b.descriptorCount;merged=true;}if(!merged)sizes.push_back({b.descriptorType,b.descriptorCount});}
            if(sizes.empty())continue;
            VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pci.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT|((L.flags&VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT)?VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT:0);
            for(auto& sz:sizes)sz.descriptorCount*=2;pci.maxSets=2;pci.poolSizeCount=std::uint32_t(sizes.size());pci.pPoolSizes=sizes.data();
            VkDescriptorPool pool{};if(raw.createDescriptorPool(d,&pci,nullptr,&pool)!=VK_SUCCESS||!pool)continue;
            VkDescriptorSetVariableDescriptorCountAllocateInfo vinfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO};
            std::uint32_t count{};
            if(variable&&variable->descriptorSetCount==info->descriptorSetCount&&variable->pDescriptorCounts){count=variable->pDescriptorCounts[i];vinfo.descriptorSetCount=1;vinfo.pDescriptorCounts=&count;}
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.pNext=variable?&vinfo:nullptr;ai.descriptorPool=pool;ai.descriptorSetCount=1;ai.pSetLayouts=&info->pSetLayouts[i];
            VkDescriptorSet clone{},clone2{};
            if(raw.allocateDescriptorSets(d,&ai,&clone)!=VK_SUCCESS||!clone){raw.destroyDescriptorPool(d,pool,nullptr);continue;}
            if(raw.allocateDescriptorSets(d,&ai,&clone2)!=VK_SUCCESS||!clone2){raw.destroyDescriptorPool(d,pool,nullptr);continue;}
            std::uint32_t textureCount=0;for(const auto& b:L.bindings)if(b.binding==L.textureBinding)textureCount=b.descriptorCount;
            std::uint32_t highest=0;for(const auto& b:L.bindings)highest=std::max(highest,b.binding);
            std::uint32_t variableCount=0;
            if(variable&&variable->descriptorSetCount==info->descriptorSetCount&&variable->pDescriptorCounts)variableCount=count;
            bool textureIsVariable=false;
            for(std::size_t k=0;k<L.bindings.size()&&k<L.bindingFlags.size();++k)if(L.bindings[k].binding==L.textureBinding&&(L.bindingFlags[k]&VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT))textureIsVariable=true;
            if(textureIsVariable&&L.textureBinding==highest&&variableCount&&variableCount<textureCount)textureCount=variableCount;
            SetRecord rec{d,info->descriptorPool,info->pSetLayouts[i],clone,clone2,pool,L.textureBinding,textureCount,{}};rec.written.assign(textureCount,false);rec.variableCount=variableCount;rec.layoutCount=0;for(const auto& b:L.bindings)if(b.binding==L.textureBinding)rec.layoutCount=b.descriptorCount;
            for(const auto& b:L.bindings){
                if(b.binding>=8)continue;
                if(!((b.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER||b.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)&&b.descriptorCount>=32))continue;
                std::uint32_t n=b.descriptorCount;
                bool isVar=false;for(std::size_t k=0;k<L.bindings.size()&&k<L.bindingFlags.size();++k)if(L.bindings[k].binding==b.binding&&(L.bindingFlags[k]&VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT))isVar=true;
                if(isVar&&b.binding==highest&&variableCount&&variableCount<n)n=variableCount;
                rec.bindingCounts[b.binding]=n;rec.bindingWritten[b.binding].assign(n,false);
                rec.elementImage[b.binding].assign(std::min<std::uint32_t>(n,65536u),VK_NULL_HANDLE);rec.elementSlot[b.binding].assign(std::min<std::uint32_t>(n,65536u),0u);
                rec.elementView[b.binding].assign(std::min<std::uint32_t>(n,65536u),VK_NULL_HANDLE);
                rec.elementLayout[b.binding].assign(std::min<std::uint32_t>(n,65536u),VK_IMAGE_LAYOUT_UNDEFINED);
                if(b.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)rec.elementSampler[b.binding].assign(std::min<std::uint32_t>(n,65536u),VK_NULL_HANDLE);
            }
            // Descriptor type and binding flags per binding (the engine scrub needs UPDATE_AFTER_BIND +
            // UPDATE_UNUSED_WHILE_PENDING); a binding the mirror does not track blocks a clone rebuild (it could not re-write it)
            for(std::size_t k=0;k<L.bindings.size();++k){const auto& b=L.bindings[k];
                const bool tracked=b.binding<8&&(b.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER||b.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)&&b.descriptorCount>=32&&b.descriptorCount<=65536u;
                if(!tracked&&b.descriptorCount)rec.untracked=true;
                if(b.binding<8){rec.bindingTypes[b.binding]=b.descriptorType;rec.bindingFlagBits[b.binding]=k<L.bindingFlags.size()?L.bindingFlags[k]:0;}}
            // The clones' flag byte right after allocation (raw byte through the safe reader - report driverSets.clone.allocFlag).
            // Read only with the driver identity armed (setprobe) and the clone's [+0] = the driver's descriptor-set
            // vptr (self-check through the safe reader); otherwise -1
            if(driverArmed())for(unsigned o=0;o<2;++o){const auto s=reinterpret_cast<std::uintptr_t>(o?clone2:clone);std::uintptr_t vt{};std::uint8_t f{};
                if(safeRead(s,&vt,sizeof vt)&&vt==t2.setVtable&&safeRead(s+drv::setFlagOffset,&f,1))rec.probe.allocFlag[o]=f;}
            setRegistry[out[i]]=std::move(rec);++stats.bindlessSets;
        }
    }catch(...){}
}
namespace {
void forgetSet(std::unordered_map<VkDescriptorSet,SetRecord>::iterator it){
    {const auto frame=scrubFrame.load(std::memory_order_relaxed);for(const auto& entries:it->second.overrides)for(const auto& e:entries)noteRecentSource(e.source,frame);}
    // An old clone set still deferred in the shared pool goes with that pool's destroy below
    deferredSets.erase(std::remove_if(deferredSets.begin(),deferredSets.end(),[&](const DeferredSet& s){return !s.ownPool&&s.pool==it->second.clonePool;}),deferredSets.end());
    for(const auto pool:it->second.ownPool)if(pool)raw.destroyDescriptorPool(it->second.device,pool,nullptr); // rebuilt clones
    if(it->second.clonePool)raw.destroyDescriptorPool(it->second.device,it->second.clonePool,nullptr);
    setRegistry.erase(it);
}
}
void descriptorSetsFreed(VkDevice,VkDescriptorPool,std::uint32_t n,const VkDescriptorSet* sets) noexcept {
    Errno preserve;if(!sets)return;
    try{std::lock_guard lock(coldMutex);for(std::uint32_t i=0;i<n;++i){auto it=setRegistry.find(sets[i]);if(it!=setRegistry.end())forgetSet(it);}}catch(...){}
}
void descriptorPoolReset(VkDevice d,VkDescriptorPool pool) noexcept {
    Errno preserve;try{std::lock_guard lock(coldMutex);for(auto it=setRegistry.begin();it!=setRegistry.end();){if(it->second.device==d&&it->second.gamePool==pool){auto next=std::next(it);forgetSet(it);it=next;}else ++it;}}catch(...){}
}
void descriptorPoolDestroyed(VkDevice d,VkDescriptorPool pool) noexcept {descriptorPoolReset(d,pool);}
void updateDescriptorSets(VkDevice d,std::uint32_t writeCount,const VkWriteDescriptorSet* writes,std::uint32_t copyCount,const VkCopyDescriptorSet* copies) noexcept {
    Errno preserve;
    thread_local std::vector<VkWriteDescriptorSet> allWrites;thread_local std::vector<VkCopyDescriptorSet> allCopies;
    bool mirrored=false;
    // The driver update below is issued while coldMutex is still held,
    // so the mirror's element tracking (source validation, clone rebuild, engine scrub) never runs ahead of the driver sets,
    // and the mod's own writes to the engine set (engine scrub) are externally synchronized with the engine's (dstSet).
    std::unique_lock<std::mutex> lock(coldMutex,std::defer_lock);
    try{
        allWrites.clear();allCopies.clear();
        if(!lock.try_lock()){noteColdWait();lock.lock();}
        if(!setRegistry.empty()){
            for(std::uint32_t i=0;i<writeCount;++i){
                auto it=setRegistry.find(writes[i].dstSet);
                if(it==setRegistry.end())continue;
                if(!mirrored){allWrites.assign(writes,writes+writeCount);allCopies.assign(copies,copies+copyCount);mirrored=true;}
                auto w=writes[i];w.dstSet=it->second.clone;allWrites.push_back(w);w.dstSet=it->second.clone2;allWrites.push_back(w);++stats.mirroredWrites;++it->second.writesPerBinding[std::min<std::uint32_t>(w.dstBinding,7)];
                if(w.dstBinding==it->second.textureBinding)for(std::uint32_t e=w.dstArrayElement;e<w.dstArrayElement+w.descriptorCount;++e){it->second.maxWritten=std::max(it->second.maxWritten,e);if(e<it->second.textureCount&&!it->second.written[e]){it->second.written[e]=true;++it->second.writtenCount;}}
                if(w.dstBinding<8&&it->second.bindingCounts[w.dstBinding]){auto& bw=it->second.bindingWritten[w.dstBinding];const auto n=it->second.bindingCounts[w.dstBinding];
                    for(std::uint32_t e=w.dstArrayElement;e<w.dstArrayElement+w.descriptorCount&&e<n;++e)if(!bw[e]){bw[e]=true;++it->second.bindingWrittenCount[w.dstBinding];}}
                if(w.dstBinding<8&&it->second.bindingCounts[w.dstBinding]&&writes[i].pImageInfo&&(w.descriptorType==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE||w.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER))
                    try{for(std::uint32_t k=0;k<w.descriptorCount;++k){VkImage image{};
                            const VkImageView view=writes[i].pImageInfo[k].imageView; // Element -> image and view (override sources)
                            if(view){auto v=viewRegistry.find(view);if(v!=viewRegistry.end())image=v->second.image;}
                            setElementImage(it->second,w.dstBinding,w.dstArrayElement+k,image,view);
                            noteElementLayout(it->second,w.dstBinding,w.dstArrayElement+k,writes[i].pImageInfo[k].imageLayout,writes[i].pImageInfo[k].sampler);}}catch(...){}
                if(w.dstBinding<8&&(!it->second.overrides[0].empty()||!it->second.overrides[1].empty()))dropOverridesAt(it->second,w.dstBinding,w.dstArrayElement,w.descriptorCount,scrubFrame.load(std::memory_order_relaxed)); // The mirror rewrote clone/clone2 there
            }
            for(std::uint32_t i=0;i<copyCount;++i){
                auto it=setRegistry.find(copies[i].dstSet);
                if(it==setRegistry.end())continue;
                if(!mirrored){allWrites.assign(writes,writes+writeCount);allCopies.assign(copies,copies+copyCount);mirrored=true;}
                auto c=copies[i];c.dstSet=it->second.clone;allCopies.push_back(c);c.dstSet=it->second.clone2;allCopies.push_back(c);++stats.mirroredCopies;
                if(c.dstBinding==it->second.textureBinding)for(std::uint32_t e=c.dstArrayElement;e<c.dstArrayElement+c.descriptorCount;++e){it->second.maxWritten=std::max(it->second.maxWritten,e);if(e<it->second.textureCount&&!it->second.written[e]){it->second.written[e]=true;++it->second.writtenCount;}}
                if(c.dstBinding<8&&it->second.bindingCounts[c.dstBinding]){auto& bw=it->second.bindingWritten[c.dstBinding];const auto n=it->second.bindingCounts[c.dstBinding];
                    for(std::uint32_t e=c.dstArrayElement;e<c.dstArrayElement+c.descriptorCount&&e<n;++e)if(!bw[e]){bw[e]=true;++it->second.bindingWrittenCount[c.dstBinding];}}
                if(c.dstBinding<8&&it->second.bindingCounts[c.dstBinding])try{
                    auto src=setRegistry.find(copies[i].srcSet);const std::uint32_t sb=copies[i].srcBinding;
                    for(std::uint32_t k=0;k<c.descriptorCount;++k){VkImage image{};VkImageView view{};
                        if(src!=setRegistry.end()&&sb<8&&copies[i].srcArrayElement+k<src->second.elementImage[sb].size())image=src->second.elementImage[sb][copies[i].srcArrayElement+k];
                        if(src!=setRegistry.end()&&sb<8&&copies[i].srcArrayElement+k<src->second.elementView[sb].size())view=src->second.elementView[sb][copies[i].srcArrayElement+k];
                        setElementImage(it->second,c.dstBinding,c.dstArrayElement+k,image,view);
                        VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;VkSampler sampler{};
                        if(src!=setRegistry.end()&&sb<8&&copies[i].srcArrayElement+k<src->second.elementLayout[sb].size())layout=src->second.elementLayout[sb][copies[i].srcArrayElement+k];
                        if(src!=setRegistry.end()&&sb<8&&copies[i].srcArrayElement+k<src->second.elementSampler[sb].size())sampler=src->second.elementSampler[sb][copies[i].srcArrayElement+k];
                        noteElementLayout(it->second,c.dstBinding,c.dstArrayElement+k,layout,sampler);}}catch(...){}
                if(c.dstBinding<8&&(!it->second.overrides[0].empty()||!it->second.overrides[1].empty()))dropOverridesAt(it->second,c.dstBinding,c.dstArrayElement,c.descriptorCount,scrubFrame.load(std::memory_order_relaxed));
            }
        }
    }catch(...){mirrored=false;}
    if(mirrored)raw.updateDescriptorSets(d,std::uint32_t(allWrites.size()),allWrites.data(),std::uint32_t(allCopies.size()),allCopies.data());
    else raw.updateDescriptorSets(d,writeCount,writes,copyCount,copies);
} // coldMutex (when taken) released here, after the driver update
void commandBegun(VkDevice d,VkCommandBuffer c) noexcept {
    Errno preserve;
    if(auto* s=tracked(c,true))*s=BoundState{};
    if(gpuStagesArmed.load(std::memory_order_relaxed)){if(d&&c)gpuFrameBegin(d,c);} // DETAIL: reset this frame's query slot + first marker
    else if(gpuLiteArmed.load(std::memory_order_relaxed)&&gpu::liteEnabled()&&d&&c)gpuLiteBegin(d,c); // Lite frame span (learned frame command buffers only)
    if(!enabled()||!d||!c)return;
    const auto layout=cachedPushLayoutFor(d);if(!layout)return;
    raw.pushConstants(c,layout,VK_SHADER_STAGE_VERTEX_BIT,0,64,identityMatrix);++stats.identityPushes;
}

// ================================================================= tracking
namespace {
inline void trackPipeline(BoundState* s,VkPipelineBindPoint p,VkPipeline pipeline) noexcept {if(s&&p==VK_PIPELINE_BIND_POINT_GRAPHICS){s->pipeline=pipeline;s->pipelinePoint=p;}}
inline void trackSets(BoundState* s,VkPipelineBindPoint p,VkPipelineLayout l,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t n,const std::uint32_t* offsets) noexcept {
    if(!s||p!=VK_PIPELINE_BIND_POINT_GRAPHICS||first>=8||count>8||first+count>8||n>64)return;
    auto& call=s->setCalls[first];call.layout=l;call.point=p;call.count=count;call.offsetCount=n;call.valid=true;
    for(std::uint32_t i=0;i<count;++i)call.sets[i]=sets[i];
    for(std::uint32_t i=0;i<n;++i)call.offsets[i]=offsets[i];
    for(std::uint32_t i=first+1;i<first+count;++i)s->setCalls[i].valid=false;
}
inline void trackVertices(BoundState* s,std::uint32_t first,std::uint32_t count,const VkBuffer* b,const VkDeviceSize* o) noexcept {
    if(!s||first>=16||count>16||first+count>16)return;
    for(std::uint32_t i=0;i<count;++i){s->vertexBuffers[first+i]=b[i];s->vertexOffsets[first+i]=o[i];s->vertexMask|=1u<<(first+i);}
}
inline void trackIndex(BoundState* s,VkBuffer b,VkDeviceSize o,VkIndexType t) noexcept {if(s){s->indexBuffer=b;s->indexOffset=o;s->indexType=t;s->indexKnown=true;}}
}
void hookBindPipeline(VkCommandBuffer c,VkPipelineBindPoint p,VkPipeline pipeline) noexcept {
    auto& h=threadHot;if(auto* t=recordingFor(h,c))recordBindPipeline(*t,c,p,pipeline);trackPipeline(tracked(h,c,false),p,pipeline);
}
void hookBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t n,const std::uint32_t* offsets) noexcept {
    auto& h=threadHot;if(auto* t=recordingFor(h,c))recordBindDescriptorSets(*t,c,p,l,first,count,sets,n,offsets);trackSets(tracked(h,c,false),p,l,first,count,sets,n,offsets);
}
void hookBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType type) noexcept {
    auto& h=threadHot;if(auto* t=recordingFor(h,c))recordBindIndexBuffer(*t,c,b,o,type);trackIndex(tracked(h,c,false),b,o,type);
}
VKAPI_ATTR void VKAPI_CALL hookBindVertexBuffers(VkCommandBuffer c,std::uint32_t first,std::uint32_t n,const VkBuffer* b,const VkDeviceSize* o) noexcept {
    auto& h=threadHot;if(auto* t=recordingFor(h,c))recordBindVertexBuffers(*t,c,first,n,b,o);trackVertices(tracked(h,c,false),first,n,b,o);
    if(const auto next=h.vertexNext)next(c,first,n,b,o);
}
void trackBindPipeline(VkCommandBuffer c,VkPipelineBindPoint p,VkPipeline pipeline) noexcept {trackPipeline(tracked(c,false),p,pipeline);}
void trackBindDescriptorSets(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t n,const std::uint32_t* offsets) noexcept {
    trackSets(tracked(c,false),p,l,first,count,sets,n,offsets);
}
void trackBindVertexBuffers(VkCommandBuffer c,std::uint32_t first,std::uint32_t count,const VkBuffer* b,const VkDeviceSize* o) noexcept {trackVertices(tracked(c,false),first,count,b,o);}
void trackBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType t) noexcept {trackIndex(tracked(c,false),b,o,t);}
void trackSetViewport(VkCommandBuffer c,std::uint32_t first,std::uint32_t count,const VkViewport* v) noexcept {if(auto* s=tracked(c,false)){if(first==0&&count>=1&&v){s->viewport=v[0];s->viewportKnown=true;}}}
void trackSetScissor(VkCommandBuffer c,std::uint32_t first,std::uint32_t count,const VkRect2D* v) noexcept {if(auto* s=tracked(c,false)){if(first==0&&count>=1&&v){s->scissor=v[0];s->scissorKnown=true;}}}
void trackSetDepthBias(VkCommandBuffer c,float a,float b,float d) noexcept {if(auto* s=tracked(c,false)){s->depthBias[0]=a;s->depthBias[1]=b;s->depthBias[2]=d;s->depthBiasKnown=true;}}
void trackSetStencilReference(VkCommandBuffer c,VkStencilFaceFlags f,std::uint32_t v) noexcept {if(auto* s=tracked(c,false)){if(f&VK_STENCIL_FACE_FRONT_BIT){s->stencilRef[0]=v;s->stencilRefKnown|=1;}if(f&VK_STENCIL_FACE_BACK_BIT){s->stencilRef[1]=v;s->stencilRefKnown|=2;}}}
void trackSetStencilCompareMask(VkCommandBuffer c,VkStencilFaceFlags f,std::uint32_t v) noexcept {if(auto* s=tracked(c,false)){if(f&VK_STENCIL_FACE_FRONT_BIT){s->stencilCmp[0]=v;s->stencilCmpKnown|=1;}if(f&VK_STENCIL_FACE_BACK_BIT){s->stencilCmp[1]=v;s->stencilCmpKnown|=2;}}}
void trackSetStencilWriteMask(VkCommandBuffer c,VkStencilFaceFlags f,std::uint32_t v) noexcept {if(auto* s=tracked(c,false)){if(f&VK_STENCIL_FACE_FRONT_BIT){s->stencilWrite[0]=v;s->stencilWriteKnown|=1;}if(f&VK_STENCIL_FACE_BACK_BIT){s->stencilWrite[1]=v;s->stencilWriteKnown|=2;}}}
void trackSetBlendConstants(VkCommandBuffer c,const float* v) noexcept {if(auto* s=tracked(c,false)){if(v){std::memcpy(s->blend,v,sizeof s->blend);s->blendKnown=true;}}}
void trackSetDepthBounds(VkCommandBuffer c,float a,float b) noexcept {if(auto* s=tracked(c,false)){s->bounds[0]=a;s->bounds[1]=b;s->boundsKnown=true;}}
void trackSetLineWidth(VkCommandBuffer c,float v) noexcept {if(auto* s=tracked(c,false)){s->lineWidth=v;s->lineWidthKnown=true;}}

// ================================================================= recording
#define TAPE_GUARD(t,c) if(!sameCommand(t,c))return;
// Worker mode streaming: after a draw/clear (and at every pass end) everything recorded so far is
// complete for the worker. The length is published every publishEvery draws/clears and at every pass
// end (the worker reads whole batches instead of following each draw); a sleeping worker is woken
// only at pass ends and on every wakeEvery-th publication when wakeEvery > 0 (0, the default: never
// mid-stage, the worker polls; endRecording always wakes it). One relaxed load when not streaming.
inline void publishTape(Tape& t,bool boundary) noexcept {
    if(!t.streaming.load(std::memory_order_relaxed))return;
    if(!boundary&&++t.sinceNotify<t.publishEvery)return;
    t.sinceNotify=0;
    // The render thread's CPU is sampled at stage begin/end only (a getcpu syscall every 16 draws costs about 0.3 %)
    if(!t.wakeEvery){t.published.store(t.used,std::memory_order_release);return;}
    t.published.store(t.used,std::memory_order_seq_cst); // pairs with the worker's sleeping flag (tapeProgress)
    if(boundary||++t.sinceWake>=t.wakeEvery){t.sinceWake=0;tapeProgress();}
}
void recordBindPipeline(Tape& t,VkCommandBuffer c,VkPipelineBindPoint p,VkPipeline pipeline) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PBindPipeline>(Op::BindPipeline,0))*r={p,pipeline};}
void recordBindDescriptorSets(Tape& t,VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t n,const std::uint32_t* offsets) noexcept {
    TAPE_GUARD(t,c);if(count>64||n>256){t.fail("BIND_SETS_TOO_MANY");return;}
    if(auto* r=t.push<PBindSets>(Op::BindDescriptorSets,count*sizeof(VkDescriptorSet)+n*sizeof(std::uint32_t))){
        *r={p,l,first,count,n};auto* s=reinterpret_cast<VkDescriptorSet*>(r+1);for(std::uint32_t i=0;i<count;++i)s[i]=sets[i];
        auto* o=reinterpret_cast<std::uint32_t*>(s+count);for(std::uint32_t i=0;i<n;++i)o[i]=offsets[i];
    }
}
void recordBindVertexBuffers(Tape& t,VkCommandBuffer c,std::uint32_t first,std::uint32_t count,const VkBuffer* b,const VkDeviceSize* o) noexcept {
    TAPE_GUARD(t,c);if(count>32){t.fail("VERTEX_TOO_MANY");return;}
    if(auto* r=t.push<PBindVertices>(Op::BindVertexBuffers,count*(sizeof(VkBuffer)+sizeof(VkDeviceSize)))){
        *r={first,count};auto* buffers=reinterpret_cast<VkBuffer*>(r+1);auto* offsets=reinterpret_cast<VkDeviceSize*>(buffers+count);
        for(std::uint32_t i=0;i<count;++i){buffers[i]=b[i];offsets[i]=o[i];}
    }
}
void recordBindIndexBuffer(Tape& t,VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType type) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PBindIndex>(Op::BindIndexBuffer,0))*r={b,o,type};}
void recordSetViewport(Tape& t,VkCommandBuffer c,std::uint32_t first,std::uint32_t count,const VkViewport* v) noexcept {
    TAPE_GUARD(t,c);if(count>16){t.fail("VIEWPORT_TOO_MANY");return;}
    if(auto* r=t.push<PViewport>(Op::SetViewport,count*sizeof(VkViewport))){*r={first,count};std::memcpy(r+1,v,count*sizeof(VkViewport));}
}
void recordSetScissor(Tape& t,VkCommandBuffer c,std::uint32_t first,std::uint32_t count,const VkRect2D* v) noexcept {
    TAPE_GUARD(t,c);if(count>16){t.fail("SCISSOR_TOO_MANY");return;}
    if(auto* r=t.push<PScissor>(Op::SetScissor,count*sizeof(VkRect2D))){*r={first,count};std::memcpy(r+1,v,count*sizeof(VkRect2D));}
}
void recordSetLineWidth(Tape& t,VkCommandBuffer c,float v) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PFloat1>(Op::SetLineWidth,0))*r={v};}
void recordSetDepthBias(Tape& t,VkCommandBuffer c,float a,float b,float d) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PFloat3>(Op::SetDepthBias,0))*r={a,b,d};}
void recordSetBlendConstants(Tape& t,VkCommandBuffer c,const float* v) noexcept {TAPE_GUARD(t,c);if(!v){t.fail("BLEND_NULL");return;}if(auto* r=t.push<PFloat4>(Op::SetBlendConstants,0))std::memcpy(r->v,v,sizeof r->v);}
void recordSetDepthBounds(Tape& t,VkCommandBuffer c,float a,float b) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PFloat2>(Op::SetDepthBounds,0))*r={a,b};}
void recordSetStencilCompareMask(Tape& t,VkCommandBuffer c,VkStencilFaceFlags f,std::uint32_t v) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PStencil>(Op::SetStencilCompareMask,0))*r={f,v};}
void recordSetStencilWriteMask(Tape& t,VkCommandBuffer c,VkStencilFaceFlags f,std::uint32_t v) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PStencil>(Op::SetStencilWriteMask,0))*r={f,v};}
void recordSetStencilReference(Tape& t,VkCommandBuffer c,VkStencilFaceFlags f,std::uint32_t v) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PStencil>(Op::SetStencilReference,0))*r={f,v};}
void recordPushConstants(Tape& t,VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags s,std::uint32_t o,std::uint32_t n,const void* b) noexcept {
    TAPE_GUARD(t,c);if(n>256||(n&&!b)){t.fail("PUSH_TOO_LARGE");return;}
    if(auto* r=t.push<PPush>(Op::PushConstants,n)){*r={l,s,o,n};if(n)std::memcpy(r+1,b,n);}
}
void recordBeginRenderPass(Tape& t,VkCommandBuffer c,const VkRenderPassBeginInfo* b,VkSubpassContents s) noexcept {
    TAPE_GUARD(t,c);if(!b||b->pNext||b->clearValueCount>16||(b->clearValueCount&&!b->pClearValues)){t.fail("BEGIN_PASS_UNSUPPORTED");return;}
    if(auto* r=t.push<PBeginPass>(Op::BeginRenderPass,b->clearValueCount*sizeof(VkClearValue))){
        *r={b->renderPass,b->framebuffer,b->renderArea,b->clearValueCount,s};if(b->clearValueCount)std::memcpy(r+1,b->pClearValues,b->clearValueCount*sizeof(VkClearValue));
        notePassBegin(t);
    }
}
void recordEndRenderPass(Tape& t,VkCommandBuffer c) noexcept {TAPE_GUARD(t,c);t.push<PSubpass>(Op::EndRenderPass,0);notePassEnd(t);publishTape(t,true);}
void recordNextSubpass(Tape& t,VkCommandBuffer c,VkSubpassContents s) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PSubpass>(Op::NextSubpass,0))*r={s};}
// EYE CULL B: the eye pad goes into the op header BEFORE publishTape (a streaming worker reads only published ops)
inline void padLast(void* payload,std::uint16_t pad) noexcept {reinterpret_cast<Header*>(static_cast<std::uint8_t*>(payload)-sizeof(Header))->pad=pad;}
bool recordDraw(Tape& t,VkCommandBuffer c,std::uint32_t v,std::uint32_t i,std::uint32_t fv,std::uint32_t fi,std::uint16_t pad) noexcept {if(!sameCommand(t,c))return false;
    auto* r=t.push<PDraw>(Op::Draw,0);if(r){*r={v,i,fv,fi};++t.draws;if(pad)padLast(r,pad);}publishTape(t,false);return r!=nullptr;}
bool recordDrawIndexed(Tape& t,VkCommandBuffer c,std::uint32_t n,std::uint32_t i,std::uint32_t fi,std::int32_t vo,std::uint32_t fin,std::uint16_t pad) noexcept {if(!sameCommand(t,c))return false;
    auto* r=t.push<PDrawIndexed>(Op::DrawIndexed,0);if(r){*r={n,i,fi,vo,fin};++t.draws;if(pad)padLast(r,pad);}publishTape(t,false);return r!=nullptr;}
void recordDrawIndirect(Tape& t,VkCommandBuffer c,VkBuffer b,VkDeviceSize o,std::uint32_t n,std::uint32_t s) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PIndirect>(Op::DrawIndirect,0)){*r={b,o,n,s};++t.draws;}publishTape(t,false);}
void recordDrawIndexedIndirect(Tape& t,VkCommandBuffer c,VkBuffer b,VkDeviceSize o,std::uint32_t n,std::uint32_t s) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PIndirect>(Op::DrawIndexedIndirect,0)){*r={b,o,n,s};++t.draws;}publishTape(t,false);}
void recordPipelineBarrier(Tape& t,VkCommandBuffer c,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags f,std::uint32_t nm,const VkMemoryBarrier* m,std::uint32_t nb,const VkBufferMemoryBarrier* b,std::uint32_t ni,const VkImageMemoryBarrier* im) noexcept {
    TAPE_GUARD(t,c);if(nm>64||nb>64||ni>64){t.fail("BARRIER_TOO_MANY");return;}
    for(std::uint32_t i=0;i<nm;++i)if(m[i].pNext){t.fail("BARRIER_PNEXT");return;}
    for(std::uint32_t i=0;i<nb;++i)if(b[i].pNext){t.fail("BARRIER_PNEXT");return;}
    for(std::uint32_t i=0;i<ni;++i)if(im[i].pNext){t.fail("BARRIER_PNEXT");return;}
    if(auto* r=t.push<PBarrier>(Op::PipelineBarrier,nm*sizeof(VkMemoryBarrier)+nb*sizeof(VkBufferMemoryBarrier)+ni*sizeof(VkImageMemoryBarrier))){
        *r={src,dst,f,nm,nb,ni};auto* pm=reinterpret_cast<VkMemoryBarrier*>(r+1);if(nm)std::memcpy(pm,m,nm*sizeof(VkMemoryBarrier));
        auto* pb=reinterpret_cast<VkBufferMemoryBarrier*>(pm+nm);if(nb)std::memcpy(pb,b,nb*sizeof(VkBufferMemoryBarrier));
        auto* pi=reinterpret_cast<VkImageMemoryBarrier*>(pb+nb);if(ni)std::memcpy(pi,im,ni*sizeof(VkImageMemoryBarrier));
    }
}
void recordCopyImage(Tape& t,VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,std::uint32_t n,const VkImageCopy* regions) noexcept {
    TAPE_GUARD(t,c);if(n>32){t.fail("COPY_TOO_MANY");return;}
    const std::size_t at=t.used;if(auto* r=t.push<PCopyImage>(Op::CopyImage,n*sizeof(VkImageCopy))){*r={s,sl,d,dl,n};std::memcpy(r+1,regions,n*sizeof(VkImageCopy));t.noteImageCopy(at);}
}
void recordCopyBuffer(Tape& t,VkCommandBuffer c,VkBuffer s,VkBuffer d,std::uint32_t n,const VkBufferCopy* regions) noexcept {
    TAPE_GUARD(t,c);if(n>64){t.fail("COPY_TOO_MANY");return;}
    if(auto* r=t.push<PCopyBuffer>(Op::CopyBuffer,n*sizeof(VkBufferCopy))){*r={s,d,n};std::memcpy(r+1,regions,n*sizeof(VkBufferCopy));}
}
void recordCopyBufferToImage(Tape& t,VkCommandBuffer c,VkBuffer s,VkImage d,VkImageLayout l,std::uint32_t n,const VkBufferImageCopy* regions) noexcept {
    TAPE_GUARD(t,c);if(n>64){t.fail("COPY_TOO_MANY");return;}
    if(auto* r=t.push<PCopyBufferImage>(Op::CopyBufferToImage,n*sizeof(VkBufferImageCopy))){*r={s,d,l,n};std::memcpy(r+1,regions,n*sizeof(VkBufferImageCopy));}
}
void recordCopyImageToBuffer(Tape& t,VkCommandBuffer c,VkImage s,VkImageLayout l,VkBuffer d,std::uint32_t n,const VkBufferImageCopy* regions) noexcept {
    TAPE_GUARD(t,c);if(n>64){t.fail("COPY_TOO_MANY");return;}
    if(auto* r=t.push<PCopyBufferImage>(Op::CopyImageToBuffer,n*sizeof(VkBufferImageCopy))){*r={d,s,l,n};std::memcpy(r+1,regions,n*sizeof(VkBufferImageCopy));}
}
void recordBlitImage(Tape& t,VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,std::uint32_t n,const VkImageBlit* regions,VkFilter f) noexcept {
    TAPE_GUARD(t,c);if(n>32){t.fail("BLIT_TOO_MANY");return;}
    const std::size_t at=t.used;if(auto* r=t.push<PBlit>(Op::BlitImage,n*sizeof(VkImageBlit))){*r={s,sl,d,dl,n,f};std::memcpy(r+1,regions,n*sizeof(VkImageBlit));t.noteImageCopy(at);}
}
void recordClearAttachments(Tape& t,VkCommandBuffer c,std::uint32_t na,const VkClearAttachment* a,std::uint32_t nr,const VkClearRect* rects) noexcept {
    TAPE_GUARD(t,c);if(na>16||nr>16){t.fail("CLEAR_TOO_MANY");return;}
    if(auto* r=t.push<PClearAttachments>(Op::ClearAttachments,na*sizeof(VkClearAttachment)+nr*sizeof(VkClearRect))){
        *r={na,nr};auto* pa=reinterpret_cast<VkClearAttachment*>(r+1);std::memcpy(pa,a,na*sizeof(VkClearAttachment));std::memcpy(pa+na,rects,nr*sizeof(VkClearRect));
    }
    publishTape(t,false);
}
void recordClearColorImage(Tape& t,VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearColorValue* v,std::uint32_t n,const VkImageSubresourceRange* ranges) noexcept {
    TAPE_GUARD(t,c);if(n>16||!v){t.fail("CLEAR_TOO_MANY");return;}
    if(auto* r=t.push<PClearColor>(Op::ClearColorImage,n*sizeof(VkImageSubresourceRange))){*r={i,l,*v,n};std::memcpy(r+1,ranges,n*sizeof(VkImageSubresourceRange));}
}
void recordClearDepthStencilImage(Tape& t,VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearDepthStencilValue* v,std::uint32_t n,const VkImageSubresourceRange* ranges) noexcept {
    TAPE_GUARD(t,c);if(n>16||!v){t.fail("CLEAR_TOO_MANY");return;}
    if(auto* r=t.push<PClearDepth>(Op::ClearDepthStencilImage,n*sizeof(VkImageSubresourceRange))){*r={i,l,*v,n};std::memcpy(r+1,ranges,n*sizeof(VkImageSubresourceRange));}
}
void recordResolveImage(Tape& t,VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,std::uint32_t n,const VkImageResolve* regions) noexcept {
    TAPE_GUARD(t,c);if(n>16){t.fail("RESOLVE_TOO_MANY");return;}
    const std::size_t at=t.used;if(auto* r=t.push<PResolve>(Op::ResolveImage,n*sizeof(VkImageResolve))){*r={s,sl,d,dl,n};std::memcpy(r+1,regions,n*sizeof(VkImageResolve));t.noteImageCopy(at);}
}
void recordFillBuffer(Tape& t,VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize s,std::uint32_t d) noexcept {TAPE_GUARD(t,c);if(auto* r=t.push<PFill>(Op::FillBuffer,0))*r={b,o,s,d};}
void recordUpdateBuffer(Tape& t,VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize s,const void* d) noexcept {
    TAPE_GUARD(t,c);if(s>65536||!d){t.fail("UPDATE_TOO_LARGE");return;}
    if(auto* r=t.push<PUpdate>(Op::UpdateBuffer,std::size_t(s))){*r={b,o,s};std::memcpy(r+1,d,std::size_t(s));}
}
void recordUnsupported(Tape& t,VkCommandBuffer c,const char* what) noexcept {(void)c;t.fail(what);}
#undef TAPE_GUARD

// ================================================================= tape sample + pass-chain census (render thread)
namespace {
// Eye images of the last replay (left and right): the census tells barriers on eye images from other images.
std::array<VkImage,16> censusEyeImages{};unsigned censusEyeCount{};
void noteCensusEyes(const FrameMaps& m) noexcept {
    unsigned n=0;for(unsigned k=0;k<m.imageCount&&k<8;++k){if(m.images[k].left)censusEyeImages[n++]=m.images[k].left;if(m.images[k].right)censusEyeImages[n++]=m.images[k].right;}censusEyeCount=n;
}
bool censusEye(VkImage i) noexcept {for(unsigned k=0;k<censusEyeCount;++k)if(censusEyeImages[k]==i)return true;return false;}
std::string hexHandle(const void* p){char b[24];std::snprintf(b,sizeof b,"%llx",static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(p)));return b;}
// "C:L D:L S:C" (load) / "C:S D:D S:S" (store): colour, depth and stencil aspect of every native attachment
std::string opSignature(const PassDesc& d,bool store){
    std::string s;
    for(std::uint32_t i=0;i<d.count;++i){const auto& a=d.att[i];const bool depth=boundary::hasDepth(a.format),stencil=boundary::hasStencil(a.format);
        auto add=[&](char kind,char op){if(!s.empty())s+=' ';s+=kind;s+=':';s+=op;};
        if(!depth&&!stencil)add('C',store?boundary::opLetter(a.store):boundary::opLetter(a.load));
        if(depth)add('D',store?boundary::opLetter(a.store):boundary::opLetter(a.load));
        if(stencil)add('S',store?boundary::opLetter(a.stencilStore):boundary::opLetter(a.stencilLoad));}
    return s;
}
bool passClears(const PassDesc& d) noexcept {
    for(std::uint32_t i=0;i<d.count;++i){const auto& a=d.att[i];
        if(a.load==VK_ATTACHMENT_LOAD_OP_CLEAR&&!(boundary::hasStencil(a.format)&&!boundary::hasDepth(a.format)))return true;
        if(boundary::hasStencil(a.format)&&a.stencilLoad==VK_ATTACHMENT_LOAD_OP_CLEAR)return true;}
    return false;
}
std::string passDescJson(VkRenderPass rp,const PassDesc& d){
    std::ostringstream o;o<<"{\"rp\":\""<<hexHandle(rp)<<"\",\"colors\":"<<d.colors<<",\"depth\":"<<d.depth<<",\"dependencies\":"<<d.dependencies<<",\"shape\":"<<(d.shapeKnown?1:0)
        <<",\"twins\":\""<<(d.twins.empty?"E":"")<<(d.twins.head?"H":"")<<(d.twins.tail?"T":"")<<"\",\"flags\":"<<d.twins.flags<<",\"load\":\""<<opSignature(d,false)<<"\",\"store\":\""<<opSignature(d,true)<<"\",\"att\":[";
    for(std::uint32_t i=0;i<d.count;++i){const auto& a=d.att[i];
        o<<(i?",":"")<<"{\"format\":"<<unsigned(a.format)<<",\"load\":\""<<boundary::opLetter(a.load)<<"\",\"store\":\""<<boundary::opLetter(a.store)<<"\",\"stencilLoad\":\""<<boundary::opLetter(a.stencilLoad)
         <<"\",\"stencilStore\":\""<<boundary::opLetter(a.stencilStore)<<"\",\"initial\":"<<unsigned(a.initial)<<",\"final\":"<<unsigned(a.final)<<",\"reference\":"<<(a.referenced?int(a.reference):-1)<<"}";}
    o<<"]}";return o.str();
}
// Every 32nd recording of a stage: run-length shape (capped at 600 characters), op counts / draws outside passes / open
// pass at the end over the WHOLE tape, and the pass chain: per Begin {framebuffer index, render pass, load/store ops,
// clears, draws, depth/stencil-writing draws} and per gap (End .. next Begin) every outside-pass op kind; image barriers
// split into the adjacent passes' attachments ("own"), other eye images and non-eye images.
void sampleTape(const Tape& t,unsigned si){
    std::string shape;char last=0;unsigned run=0;bool inPass=false,capped=false;std::uint64_t outside=0;std::array<std::uint32_t,33> counts{};
    auto flush=[&](){if(!last)return;if(shape.size()<=600){shape+=last;if(run>1)shape+=std::to_string(run);}else capped=true;last=0;run=0;};
    struct CPass {VkFramebuffer fb{};VkRenderPass rp{};std::uint32_t clears{},draws{},dsw{},gap{};int firstStencil{-1};}; // stencilTestEnable of the first draw (-1 no draw, -2 unknown)
    struct CGap {int prev{-1},next{-1};std::uint32_t barriers{},memory{},buffer{},own{},eye{},other{},noop{},copyBuffer{},bufferToImage{},imageToBuffer{},clearColor{},clearDepth{},fill{},update{},imageTransfer{},state{},push{};};
    struct CImage {std::uint32_t gap{};VkImage image{};bool noop{};};
    std::vector<CPass> passes;std::vector<CGap> gaps(1);std::vector<CImage> images;
    std::unordered_map<VkPipeline,std::uint8_t> dsCache; // bit0 depth write, bit1 stencil write, bit2 unknown pipeline, bit3 stencil test
    auto dsFlags=[&](VkPipeline p)->std::uint8_t{if(!p)return 4;auto f=dsCache.find(p);if(f!=dsCache.end())return f->second;std::uint8_t v=4;
        {std::lock_guard lock(coldMutex);auto it=pipelineRegistry.find(p);if(it!=pipelineRegistry.end())v=std::uint8_t((it->second.writesDepth?1:0)|(it->second.writesStencil?2:0)|(it->second.testsStencil?8:0));}
        dsCache.emplace(p,v);return v;};
    std::uint8_t boundDs=dsFlags(t.prologue.pipeline);
    std::size_t at=0;
    while(at<t.used){
        const auto* h=reinterpret_cast<const Header*>(t.bytes.data()+at);const auto* p=t.bytes.data()+at+sizeof(Header);at+=sizeof(Header)+h->bytes;
        const auto op=static_cast<Op>(h->op);char ch='S';if(unsigned(op)<counts.size())++counts[unsigned(op)];
        auto& gap=gaps.back();
        switch(op){
        case Op::BeginRenderPass:{ch='B';inPass=true;const auto* r=reinterpret_cast<const PBeginPass*>(p);
            gap.next=int(passes.size());passes.push_back({r->framebuffer,r->pass,r->clears,0,0,std::uint32_t(gaps.size()-1)});break;}
        case Op::EndRenderPass:ch='E';if(inPass){gaps.emplace_back();gaps.back().prev=int(passes.size())-1;}inPass=false;break; // a leading End closes a pass begun before the window
        case Op::NextSubpass:ch='N';break;
        case Op::Draw:case Op::DrawIndexed:case Op::DrawIndirect:case Op::DrawIndexedIndirect:ch='D';
            if(!inPass)++outside;else if(!passes.empty()){auto& ps=passes.back();++ps.draws;if(boundDs&3)++ps.dsw;
                if(ps.firstStencil==-1)ps.firstStencil=(boundDs&4)?-2:(boundDs&8)?1:0;}break;
        case Op::PipelineBarrier:{ch='X';if(inPass)break;const auto* r=reinterpret_cast<const PBarrier*>(p);++gap.barriers;gap.memory+=r->memory;gap.buffer+=r->buffers;
            const auto* pm=reinterpret_cast<const VkMemoryBarrier*>(r+1);const auto* pb=reinterpret_cast<const VkBufferMemoryBarrier*>(pm+r->memory);const auto* pi=reinterpret_cast<const VkImageMemoryBarrier*>(pb+r->buffers);
            for(std::uint32_t i=0;i<r->images;++i)images.push_back({std::uint32_t(gaps.size()-1),pi[i].image,pi[i].oldLayout==pi[i].newLayout});break;}
        case Op::CopyBuffer:ch='C';if(!inPass)++gap.copyBuffer;break;
        case Op::CopyBufferToImage:ch='C';if(!inPass)++gap.bufferToImage;break;
        case Op::CopyImageToBuffer:ch='C';if(!inPass)++gap.imageToBuffer;break;
        case Op::ClearColorImage:ch='C';if(!inPass)++gap.clearColor;break;
        case Op::ClearDepthStencilImage:ch='C';if(!inPass)++gap.clearDepth;break;
        case Op::FillBuffer:ch='C';if(!inPass)++gap.fill;break;
        case Op::UpdateBuffer:ch='C';if(!inPass)++gap.update;break;
        case Op::CopyImage:case Op::BlitImage:case Op::ResolveImage:ch='C';if(!inPass)++gap.imageTransfer;break;
        case Op::ClearAttachments:ch='C';break;
        case Op::BindPipeline:{const auto* r=reinterpret_cast<const PBindPipeline*>(p);if(r->point==VK_PIPELINE_BIND_POINT_GRAPHICS)boundDs=dsFlags(r->pipeline);if(!inPass)++gap.state;break;}
        case Op::PushConstants:if(!inPass)++gap.push;break;
        default:if(!inPass)++gap.state;break;}
        if(ch==last)++run;else{flush();last=ch;run=1;}
    }
    flush();
    for(unsigned k=0;k<counts.size();++k)stats.opCounts[si][k].store(counts[k]);
    stats.tapeDrawsOutsidePass[si].store(outside);stats.tapeOpenAtEnd[si].store(inPass?1:0);
    stats.sampledCommands[si].store(t.commands);stats.sampledBytes[si].store(t.used);stats.sampledDraws[si].store(t.draws);stats.shapeCapped[si].store(capped?1:0);
    // ---- census: framebuffer images and PassDescs (one coldMutex section)
    struct Fb {VkFramebuffer fb{};std::array<VkImage,8> images{};unsigned count{};bool eye{};};
    struct Rp {VkRenderPass rp{};PassDesc desc{};bool known{};bool eye{};};
    std::vector<Fb> fbs;std::vector<Rp> rps;std::vector<unsigned> fbOf(passes.size()),rpOf(passes.size());
    for(std::size_t k=0;k<passes.size();++k){
        unsigned f=0;while(f<fbs.size()&&fbs[f].fb!=passes[k].fb)++f;if(f==fbs.size())fbs.push_back({passes[k].fb});fbOf[k]=f;
        unsigned r=0;while(r<rps.size()&&rps[r].rp!=passes[k].rp)++r;if(r==rps.size())rps.push_back({passes[k].rp});rpOf[k]=r;}
    {std::lock_guard lock(coldMutex);
        for(auto& f:fbs){auto it=framebufferRegistry.find(f.fb);if(it==framebufferRegistry.end())continue;
            for(std::uint32_t i=0;i<it->second.count&&i<8;++i){auto v=viewRegistry.find(it->second.views[i]);if(v==viewRegistry.end())continue;f.images[f.count++]=v->second.image;f.eye|=censusEye(v->second.image);}}
        for(auto& r:rps){auto it=passRegistry.find(r.rp);if(it!=passRegistry.end()){r.desc=it->second;r.known=true;}}}
    for(std::size_t k=0;k<passes.size();++k)rps[rpOf[k]].eye|=fbs[fbOf[k]].eye;
    auto owns=[&](int pass,VkImage image){if(pass<0||std::size_t(pass)>=passes.size())return false;const auto& f=fbs[fbOf[std::size_t(pass)]];for(unsigned i=0;i<f.count;++i)if(f.images[i]==image)return true;return false;};
    for(const auto& im:images){auto& g=gaps[im.gap];if(im.noop)++g.noop;
        if(owns(g.prev,im.image)||owns(g.next,im.image))++g.own;else if(censusEye(im.image))++g.eye;else ++g.other;}
    auto& cs=stats.census[si];
    std::uint64_t sameFbLoad=0,sameFbClear=0,fbSwitch=0,attachmentBarrier=0,depthWrittenBefore=0,draws=0,dsw=0;
    std::uint64_t startStencil=0,restartNoStencil=0,restartUnknown=0,restartEmpty=0;
    for(std::size_t k=0;k<passes.size();++k){draws+=passes[k].draws;dsw+=passes[k].dsw;if(!k)continue;
        const bool same=passes[k].fb==passes[k-1].fb;const auto& r=rps[rpOf[k]];const bool clears=r.known?passClears(r.desc):passes[k].clears>0;
        if(!same)++fbSwitch;else if(clears)++sameFbClear;else{++sameFbLoad;
            const int st=passes[k].firstStencil;if(st==1)++startStencil;else if(st==0)++restartNoStencil;else if(st==-2)++restartUnknown;else ++restartEmpty;}
        if(gaps[passes[k].gap].own)++attachmentBarrier;if(passes[k-1].dsw)++depthWrittenBefore;}
    ++cs.samples;cs.passes+=passes.size();cs.boundaries+=passes.empty()?0:passes.size()-1;cs.sameFbLoad+=sameFbLoad;cs.sameFbClear+=sameFbClear;cs.fbSwitch+=fbSwitch;
    cs.attachmentBarrier+=attachmentBarrier;cs.depthWrittenBefore+=depthWrittenBefore;cs.draws+=draws;cs.dsWriteDraws+=dsw;
    cs.passStartStencil+=startStencil;cs.restartNoStencil+=restartNoStencil;cs.restartUnknown+=restartUnknown;cs.restartEmpty+=restartEmpty;
    for(const auto& g:gaps){cs.gapBarriers+=g.barriers;cs.gapImageOwn+=g.own;cs.gapImageEye+=g.eye;cs.gapImageOther+=g.other;cs.gapImageNoop+=g.noop;cs.gapMemory+=g.memory;cs.gapBuffer+=g.buffer;
        cs.gapCopyBuffer+=g.copyBuffer;cs.gapBufferToImage+=g.bufferToImage;cs.gapImageToBuffer+=g.imageToBuffer;cs.gapClearColor+=g.clearColor;cs.gapClearDepth+=g.clearDepth;
        cs.gapFill+=g.fill;cs.gapUpdate+=g.update;cs.gapImageTransfer+=g.imageTransfer;cs.gapState+=g.state;cs.gapPush+=g.push;}
    auto gapJson=[&](std::ostringstream& o,const CGap& g){bool first=true;auto f=[&](const char* n,std::uint32_t v){if(!v)return;o<<(first?"":",")<<"\""<<n<<"\":"<<v;first=false;};
        o<<"{";f("barriers",g.barriers);f("own",g.own);f("eye",g.eye);f("other",g.other);f("noop",g.noop);f("memory",g.memory);f("buffer",g.buffer);f("copyBuffer",g.copyBuffer);
        f("bufferToImage",g.bufferToImage);f("imageToBuffer",g.imageToBuffer);f("clearColor",g.clearColor);f("clearDepth",g.clearDepth);f("fill",g.fill);f("update",g.update);
        f("imageTransfer",g.imageTransfer);f("state",g.state);f("push",g.push);o<<"}";};
    std::ostringstream o;o<<"{\"recording\":"<<stats.shapeSerial[si].load()<<",\"passes\":[";
    for(std::size_t k=0;k<passes.size()&&k<64;++k){const auto& ps=passes[k];const auto& r=rps[rpOf[k]];
        o<<(k?",":"")<<"{\"fb\":"<<fbOf[k]<<",\"eye\":"<<(fbs[fbOf[k]].eye?1:0)<<",\"rp\":\""<<hexHandle(ps.rp)<<"\",\"load\":\""<<(r.known?opSignature(r.desc,false):std::string("?"))
         <<"\",\"store\":\""<<(r.known?opSignature(r.desc,true):std::string("?"))<<"\",\"clears\":"<<ps.clears<<",\"draws\":"<<ps.draws<<",\"dsWrites\":"<<ps.dsw
         <<",\"stencilStart\":"<<ps.firstStencil<<",\"gapBefore\":";
        gapJson(o,gaps[ps.gap]);o<<"}";}
    o<<"],\"after\":";gapJson(o,gaps.back().next<0&&gaps.size()>1?gaps.back():CGap{});o<<",\"passCount\":"<<passes.size()<<"}";
    std::lock_guard g(stats.skipMutex);stats.tapeShape[si]=shape;stats.passChain[si]=o.str();
    for(const auto& r:rps){if(!r.eye||!r.known||stats.eyePassCount>=stats.eyePassSeen.size())continue;
        bool seen=false;for(unsigned i=0;i<stats.eyePassCount;++i)if(stats.eyePassSeen[i]==r.rp)seen=true;if(seen)continue;
        stats.eyePassSeen[stats.eyePassCount++]=r.rp;stats.eyePassDescs+=(stats.eyePassDescs.empty()?"":",")+passDescJson(r.rp,r.desc);}
}
}

// ================================================================= frame integration
namespace {
void censusNoteFrame(unsigned si) noexcept { // render thread (endRecording)
    const auto& d=tapePassDraws[si&1u];auto& h=drawHist[si&1u];
    const unsigned stored=std::min<unsigned>(d.passes,unsigned(d.draws.size()));std::uint32_t frameMax=0;
    for(unsigned i=0;i<stored;++i){const unsigned n=d.draws[i];h.cls[boundary::drawClass(n)].fetch_add(1,std::memory_order_relaxed);
        h.bins[std::min(n/32u,64u)].fetch_add(1,std::memory_order_relaxed);frameMax=std::max<std::uint32_t>(frameMax,n);}
    h.passes.fetch_add(d.passes,std::memory_order_relaxed);if(d.passes>stored)h.overflow.fetch_add(d.passes-stored,std::memory_order_relaxed);
    h.frames.fetch_add(1,std::memory_order_relaxed);
    const auto k=h.frameCount.load(std::memory_order_relaxed);h.frameMax[unsigned(k%h.frameMax.size())].store(frameMax,std::memory_order_relaxed);h.frameCount.store(k+1,std::memory_order_release);
    if(frameMax>h.maxEver.load(std::memory_order_relaxed))h.maxEver.store(frameMax,std::memory_order_relaxed);
}
std::string drawHistJson(unsigned si){
    const auto& h=drawHist[si&1u];std::ostringstream o;
    std::uint64_t total=0;for(const auto& b:h.bins)total+=b.load(std::memory_order_relaxed);
    std::uint32_t p95=0;if(total){const std::uint64_t rank=(total*95+99)/100;std::uint64_t seen=0;
        for(unsigned k=0;k<h.bins.size();++k){seen+=h.bins[k].load(std::memory_order_relaxed);if(seen>=rank){p95=k<64?k*32u+31u:2048u;break;}}}
    const auto frames=h.frameCount.load(std::memory_order_acquire);const unsigned n=unsigned(std::min<std::uint64_t>(frames,h.frameMax.size()));
    std::array<std::uint32_t,256> v{};for(unsigned i=0;i<n;++i)v[i]=h.frameMax[i].load(std::memory_order_relaxed);std::sort(v.begin(),v.begin()+n);
    auto pct=[&](unsigned p)->std::uint32_t{return n?v[std::min<unsigned>(n-1,(n*p+99)/100-1)]:0;};
    o<<"{\"lt50\":"<<h.cls[0].load()<<",\"b50_1399\":"<<h.cls[1].load()<<",\"b1400_1549\":"<<h.cls[2].load()<<",\"ge1550\":"<<h.cls[3].load()
     <<",\"passes\":"<<h.passes.load()<<",\"frames\":"<<h.frames.load()<<",\"overflow\":"<<h.overflow.load()<<",\"perPassP95\":"<<p95
     <<",\"frameMax\":{\"p50\":"<<pct(50)<<",\"p95\":"<<pct(95)<<",\"max\":"<<(n?v[n-1]:0)<<"},\"maxEver\":"<<h.maxEver.load()<<"}";
    return o.str();
}
}
void beginRecording(Stage stage) noexcept {
    Errno preserve;
    if(stage==Stage::Color)colorrestart::begin(s02::wants(s02::CR)); // VkDevice+0x13d6 = 1 for the left colour stage (no-op unless CR)
    else sdAliasClear(); // A new frame's depth stage: the right SampledDepth routing starts native again
    waitTapeIdle(stage); // worker mode: a job of the previous frame may still read this tape
    noteRenderStage(stage,true); // Diagnostics (CPU, context switches of the left stage)
    gpuMark(stage==Stage::Depth?GpuMark::LeftDepthBegin:GpuMark::LeftColorBegin); // DETAIL GPU stage markers
    auto& t=tapes[unsigned(stage)];t.reset();
    tapePassDraws[unsigned(stage)&1u]=TapePassDraws{};
    threadHot.boundary={}; // Left pass index restarts with the stage's tape
    if(!enabled())return;
    t.active=true;t.beganNs=nowNs();
    threadHot.recording={&t,VK_NULL_HANDLE};
}
void endRecording(Stage stage,const std::uintptr_t* stockTextures,unsigned count) noexcept {
    Errno preserve;
    const bool colorRestartOn=stage==Stage::Color&&colorrestart::end(); // Compare-and-restore first, on every path
    noteRenderStage(stage,false);
    auto& t=tapes[unsigned(stage)];
    const bool detail=gpuStagesArmed.load(std::memory_order_relaxed);
    if(detail){gpuStageCommand(t.command);gpuMark(stage==Stage::Depth?GpuMark::LeftDepthEnd:GpuMark::LeftColorEnd);}
    else if(stage==Stage::Depth)gpuLiteStageCommand(t.command); // Learn the frame command buffer for the lite span
    const bool wasActive=threadHot.recording.tape==&t;
    threadHot.recording={};threadHot.boundary.c={}; // No left split/extra outside the recording window
    if(!wasActive||!t.active){t.fail("NOT_RECORDING");lastDraws[unsigned(stage)].store(UINT32_MAX,std::memory_order_relaxed);tapeFinished(stage);return;}
    t.active=false;t.endedNs=nowNs();
    // The prologue (bound state before the first recorded command) was captured
    // by the first recorded command; the engine may rely on bindings made before
    // the window (e.g. the bindless texture set bound once per command buffer).
    t.stockLayoutCount=0;
    if(stockTextures){
        for(unsigned i=0;i<count&&t.stockLayoutCount<t.stockLayouts.size();++i){
            const auto p=stockTextures[i];if(!p||p%4)continue;
            std::uint32_t layout{};std::memcpy(&layout,reinterpret_cast<const void*>(p+0xa0),sizeof layout);
            t.stockLayouts[t.stockLayoutCount++]={p,layout};
        }
    }
    if(!t.commands)t.fail("TAPE_EMPTY");
    if(!t.invalid){ // Per-pass draws of this recording -> census.drawHist; depth pass 0 -> gpuStages.depthByDraws
        auto& d=tapePassDraws[unsigned(stage)&1u];
        if(d.open){d.open=false;if(d.passes<d.draws.size())d.draws[d.passes]=std::uint16_t(std::min<std::uint32_t>(t.draws-d.start,65535u));++d.passes;} // the pass still open at the window end
        censusNoteFrame(unsigned(stage)&1u);
        if(detail&&stage==Stage::Depth&&d.passes)gpuNoteDepthPass0(t.command,d.draws[0]);
        if(stage==Stage::Color)colorrestart::notePasses(colorRestartOn,d.passes); // colorRestart.colorPassesPerFrame{on,off}
    }
    // Compressed shape of the tape for the report (B/E/N pass ops, D draws, X barriers, C
    // copies/clears, S state) and the pass-chain census, every 32nd recording OF THIS STAGE.
    if((stats.shapeSerial[unsigned(stage)&1u].fetch_add(1,std::memory_order_relaxed)&31u)==0)try{sampleTape(t,unsigned(stage)&1u);}catch(...){}
    ++stats.recordings;if(t.invalid){++stats.recordingsInvalid;stats.lastInvalid.store(t.invalidReason);}
    stats.tapeCommands[unsigned(stage)].store(t.commands);stats.tapeBytes[unsigned(stage)].store(t.used);
    lastDraws[unsigned(stage)].store(t.invalid?UINT32_MAX:t.draws,std::memory_order_relaxed);
    tapeFinished(stage); // worker mode: a job streaming this tape gets its final length (and decides on t.invalid)
}
bool tapeReady(Stage stage) noexcept {const auto& t=tapes[unsigned(stage)];return !t.active&&!t.invalid&&t.commands>0;}
unsigned tapeCommands(Stage stage) noexcept {return tapes[unsigned(stage)].commands;}
std::uint32_t lastTapeDraws(Stage stage) noexcept {return lastDraws[unsigned(stage)&1u].load(std::memory_order_relaxed);}

namespace internal {
void emitBoundaryBarrier(VkCommandBuffer c) noexcept {
    // Store -> load across the inserted boundary: the ended pass's attachment writes (its storeOp runs in
    // COLOR_ATTACHMENT_OUTPUT / LATE_FRAGMENT_TESTS as *_ATTACHMENT_WRITE) before the twin's loadOp (attachment reads in
    // COLOR_ATTACHMENT_OUTPUT / EARLY_FRAGMENT_TESTS) and its writes (WAW). No image barrier: the twin begins in exactly
    // the layout the previous pass ended in (StereoBoundary.h rules), so there is no transition to order. The VRS map
    // is read-only (no hazard). Outside any render pass instance, so no subpass self-dependency is involved.
    constexpr VkPipelineStageFlags stages=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    raw.pipelineBarrier(c,stages,stages,0,1,&mb,0,nullptr,0,nullptr);
}
void emitEmptyTwin(VkCommandBuffer c,VkRenderPass empty,VkFramebuffer fb,const VkRect2D& area) noexcept {
    VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=empty;b.framebuffer=fb;b.renderArea=area; // clearValueCount 0: no CLEAR in the empty twin
    raw.beginRenderPass(c,&b,VK_SUBPASS_CONTENTS_INLINE);raw.endRenderPass(c);
}
unsigned boundaryRightFlags() noexcept {return boundaryFlags.load(std::memory_order_relaxed)&(BoundaryExtraR|BoundarySplitR|BoundaryMergeR);}
}
namespace {
std::atomic<int> leftExtraOverride{-1}; // host tests
std::atomic<bool> mergeAlwaysProperty{}; // debug.rdrvr.stereoreplay.merge=2 = R EYE MERGE ALWAYS
bool mergeAlways() noexcept {return mergeAlwaysProperty.load(std::memory_order_relaxed);}
std::atomic<const char*> leftVariant{"SPLIT"};
bool leftExtraWanted() noexcept { // debug.rdrvr.stereoreplay.leftextra=1: mode 12 inserts empty twins after the left passes instead of splitting
    const int forced=leftExtraOverride.load(std::memory_order_relaxed);if(forced>=0)return forced!=0;
#ifdef __ANDROID__
    static unsigned calls{};static bool wanted{}; // render thread (latchBoundaryPhase)
    if((calls++&63u)==0){char v[PROP_VALUE_MAX]{};wanted=__system_property_get("debug.rdrvr.stereoreplay.leftextra",v)>0&&v[0]=='1';}
    return wanted;
#else
    return false;
#endif
}
}
void setBoundaryLeftExtraForTests(int value) noexcept {leftExtraOverride.store(value,std::memory_order_relaxed);}
void latchBoundaryPhase() noexcept {
    Errno preserve;auto& b=stats.boundary;const unsigned mode=testMode.load(std::memory_order_relaxed);
    sdAliasClear(); // Frame end (FinishPrimary), after this frame's right-eye stages
    {static unsigned calls{};if((calls++&63u)==0)gpu::refreshProperties();} // debug.rdrvr.gpulite / gpustages.passes (render thread)
    const unsigned levers=s02::latchFrame(); // Lever mask of the next frame
    if(mode!=b.latchMode){b.latchMode=mode;b.latchFrames=0;} // a mode starts with its "on" window
    if(!testModeAlternating(mode)||(mode==testModeMergeR&&!merge::enabled())){
        // R EYE MERGE ALWAYS: bit 0x2 (or debug.rdrvr.stereoreplay.merge=2) merges the right-eye colour replay every
        // frame while no alternating STEREO TEST owns the boundary flags (phase stays 0: no _m..a/_m..b split).
        const bool always=!testModeAlternating(mode)&&merge::enabled()&&((levers&s02::MG)||mergeAlways());
        boundaryFlags.store(always?unsigned(BoundaryMergeR):0u,std::memory_order_relaxed);boundaryPhase.store(0,std::memory_order_relaxed);
        if(always)++b.mergeAlwaysFrames;
        return;} // Merge off -> no _m20a/_m20b pair
    const bool on=((b.latchFrames++/60)&1u)==0;
    unsigned flags=0; // Test modes 18/19 (TEXTURE FILTER / MIP BIAS) only change the phase
    if(on){if(mode==testModeBoundaryExtraR)flags=BoundaryExtraR;else if(mode==testModeBoundarySplitR)flags=BoundarySplitR;
        else if(mode==testModeBoundarySplitL){const bool extra=leftExtraWanted();flags=extra?BoundaryExtraL:BoundarySplitL;leftVariant.store(extra?"EXTRA":"SPLIT",std::memory_order_relaxed);}
        else if(mode==testModeMergeR&&merge::enabled())flags=BoundaryMergeR;}
    boundaryFlags.store(flags,std::memory_order_relaxed);boundaryPhase.store(on?1u:2u,std::memory_order_relaxed);
    ++(on?b.framesOn:b.framesOff);
}
// Left eye (mode 12), render thread: Begin hook after the foveation substitution (b = what the driver begins).
void leftPassBegunSlow(VkCommandBuffer c,const VkRenderPassBeginInfo* b,VkSubpassContents s) noexcept {
    auto& h=threadHot;auto& L=h.boundary;L.c={};
    Tape* const t=recordingFor(h,c);if(!t||!b||t->command!=c)return; // only the passes of a recorded left stage (= the tape's passes; the Begin was just taped)
    const unsigned index=L.passes++;
    auto& bs=stats.boundary;const unsigned flags=boundaryFlags.load(std::memory_order_relaxed);
    PassTwins tw{};if(!twinsFor(b->renderPass,tw)){++bs.noTwins;return;}
    L.fb=b->framebuffer;L.area=b->renderArea;L.draws=0;L.queries=0;L.done=false;L.queryWait=false;L.splitAt=0;L.tail={};L.empty={};
    if(flags&BoundaryExtraL){if(!tw.empty){++bs.noTwins;return;}if(!(tw.flags&PassFinalIsReference)){++bs.extraLayout;return;}L.empty=tw.empty;L.c=c;return;}
    // Split: P itself is the head, so it must store everything and end in its subpass layouts (the tail starts there).
    if(s!=VK_SUBPASS_CONTENTS_INLINE){++bs.leftContents;return;}
    if(!(tw.flags&PassStoresAll)){++bs.leftNotStored;return;}
    if(!(tw.flags&PassFinalIsReference)){++bs.leftLayout;return;}
    if(!tw.tail){++bs.noTwins;return;}
    if(!boundaryQueriesObserved.load(std::memory_order_relaxed)){++bs.leftQuery;return;} // engine queries not visible: an active one could straddle the split
    const unsigned si=t==&tapes[0]?0u:1u;
    const unsigned prev=index<boundary::prevDrawPasses?bs.prevDraws[si*boundary::prevDrawPasses+index].load(std::memory_order_relaxed):0u;
    if(!(L.splitAt=boundary::splitPoint(prev))){++bs.fewDraws;return;}
    L.tail=tw.tail;L.c=c;
}
// After the forwarded engine draw: at the split draw end P and begin its tail twin with the raw entry points; the
// engine keeps drawing into the tail and its own vkCmdEndRenderPass ends the tail (final = P.final). Bound pipeline,
// descriptor sets, push constants and dynamic state of a primary command buffer persist across render pass instances.
void leftDrawSlow(VkCommandBuffer c) noexcept {
    auto& L=threadHot.boundary;if(L.c!=c||!L.splitAt||L.done)return;
    if(++L.draws<L.splitAt)return;
    // A query begun inside this pass must end inside the same subpass of the same render pass instance, and none may be
    // active at vkCmdEndRenderPass (vkCmdEndQuery / vkCmdEndRenderPass valid usage): wait for a draw with no active query.
    if(L.queries){if(!L.queryWait){L.queryWait=true;++stats.boundary.leftQuery;}return;}
    if(!rawBound.load(std::memory_order_acquire))return;
    Errno preserve;
    raw.endRenderPass(c);emitBoundaryBarrier(c);
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};bi.renderPass=L.tail;bi.framebuffer=L.fb;bi.renderArea=L.area; // no CLEAR in a tail: clearValueCount 0
    raw.beginRenderPass(c,&bi,VK_SUBPASS_CONTENTS_INLINE);
    L.done=true;++stats.boundary.splitsL;stats.boundary.insertedThisFrame.fetch_add(1,std::memory_order_relaxed);
}
// End hook, after the engine's vkCmdEndRenderPass and before gpuPassEnded (deferred stage markers flush after the twin).
void leftPassEndedSlow(VkCommandBuffer c) noexcept {
    auto& L=threadHot.boundary;if(L.c!=c)return;
    const VkRenderPass empty=L.empty;const VkFramebuffer fb=L.fb;const VkRect2D area=L.area;L.c={};
    if(!empty||!rawBound.load(std::memory_order_acquire))return;
    Errno preserve;emitBoundaryBarrier(c);emitEmptyTwin(c,empty,fb,area);
    ++stats.boundary.extraL;stats.boundary.insertedThisFrame.fetch_add(1,std::memory_order_relaxed);
}
void leftQuerySlow(VkCommandBuffer c,int delta) noexcept {
    auto& L=threadHot.boundary;if(L.c!=c)return;
    if(delta>0)++L.queries;else if(L.queries)--L.queries;
}
namespace {
thread_local bool faulted{};
struct Substitution {
    const FrameMaps* maps{};
    VkImage image(VkImage i,bool* mapped=nullptr)const{for(unsigned k=0;k<maps->imageCount;++k)if(maps->images[k].left==i){if(mapped)*mapped=true;return maps->images[k].right;}if(mapped)*mapped=false;return i;}
};
// Resolve the right-eye framebuffer for a recorded left framebuffer. Result:
// 0 = not an eye framebuffer (skip the pass), 1 = resolved, 2 = failure.
bool passThroughAttachment(const ViewRecord& v){return v.format==VK_FORMAT_R8_UINT;}
int resolveFramebuffer(const Substitution& sub,VkDevice device,VkRenderPass pass,VkFramebuffer left,VkFramebuffer& out){
    std::lock_guard lock(coldMutex);
    auto sample=[&](const char* why,const FramebufferRecord* L){
        try{std::ostringstream o;o<<why<<" fb="<<reinterpret_cast<std::uint64_t>(left)<<" pass="<<reinterpret_cast<std::uint64_t>(pass);
            if(L){o<<" att=[";for(std::uint32_t i=0;i<L->count;++i){auto v=viewRegistry.find(L->views[i]);o<<(i?",":"")<<reinterpret_cast<std::uint64_t>(L->views[i])<<":"<<(v==viewRegistry.end()?std::uint64_t(0):reinterpret_cast<std::uint64_t>(v->second.image))<<"/"<<(v==viewRegistry.end()?0:unsigned(v->second.format));}o<<"] "<<L->width<<"x"<<L->height;}
            o<<" map=[";for(unsigned k=0;k<sub.maps->imageCount;++k)o<<(k?",":"")<<reinterpret_cast<std::uint64_t>(sub.maps->images[k].left)<<">"<<reinterpret_cast<std::uint64_t>(sub.maps->images[k].right)<<"/r"<<sub.maps->images[k].role;o<<"]";
            std::lock_guard g(stats.skipMutex);if(stats.skipNext<stats.skipSamples.size())stats.skipSamples[stats.skipNext++]=o.str();}catch(...){}};
    auto it=framebufferRegistry.find(left);if(it==framebufferRegistry.end()){if(++stats.skipNoFramebuffer<8)sample("NO_FRAMEBUFFER",nullptr);return 0;}
    const auto& L=it->second;
    std::array<VkImage,8> want{};std::array<ViewRecord,8> leftViews{};std::array<bool,8> passThrough{};unsigned eyeAttachments=0;
    for(std::uint32_t i=0;i<L.count;++i){
        auto v=viewRegistry.find(L.views[i]);if(v==viewRegistry.end()){++stats.skipNoView;passThrough[i]=true;want[i]=reinterpret_cast<VkImage>(1);continue;}
        leftViews[i]=v->second;bool m=false;want[i]=sub.image(v->second.image,&m);
        if(m)++eyeAttachments;else if(passThroughAttachment(v->second))passThrough[i]=true;else want[i]=VK_NULL_HANDLE;
    }
    if(!eyeAttachments){if(++stats.skipNoEye<8)sample("NO_EYE_ATTACHMENT",&L);return 0;}
    for(std::uint32_t i=0;i<L.count;++i)if(!want[i]){if(++stats.skipMixed<8)sample("MIXED",&L);return 2;} // foreign non-eye attachment mixed into an eye pass
    // A pass-through attachment that is a left-eye foveation map (VRS rate map / FDM density map, appended last
    // to the framebuffer) becomes the right-eye map of the same level; anything else passes through unchanged.
    std::array<VkImageView,8> through{};const auto translate=passThroughTranslator.load(std::memory_order_acquire);
    for(std::uint32_t i=0;i<L.count;++i)if(passThrough[i])through[i]=translate?translate(L.views[i]):L.views[i];
    for(auto& f:framebufferRegistry){
        const auto& R=f.second;
        if(f.first==left||R.device!=device||R.count!=L.count||R.width!=L.width||R.height!=L.height||R.layers!=L.layers)continue;
        bool same=true;
        for(std::uint32_t i=0;i<L.count&&same;++i){auto v=viewRegistry.find(R.views[i]);
            if(passThrough[i]){same=R.views[i]==through[i]||(v!=viewRegistry.end()&&viewRegistry.count(L.views[i])&&v->second.format==leftViews[i].format);continue;}
            if(v==viewRegistry.end()){same=false;break;}
            else same=v->second.image==want[i]&&v->second.range.aspectMask==leftViews[i].range.aspectMask&&
                v->second.range.baseMipLevel==leftViews[i].range.baseMipLevel&&v->second.range.baseArrayLayer==leftViews[i].range.baseArrayLayer;}
        if(same){out=f.first;return 1;}
    }
    // Create an owned framebuffer from registered views on the right images.
    std::array<VkImageView,8> views{};
    for(std::uint32_t i=0;i<L.count;++i){
        if(passThrough[i]){views[i]=through[i];continue;}
        VkImageView chosen{};
        for(auto& v:viewRegistry){const auto& r=v.second;
            if(r.device==device&&r.image==want[i]&&r.type==leftViews[i].type&&r.format==leftViews[i].format&&r.range.aspectMask==leftViews[i].range.aspectMask&&
               r.range.baseMipLevel==leftViews[i].range.baseMipLevel&&r.range.baseArrayLayer==leftViews[i].range.baseArrayLayer&&r.range.layerCount==leftViews[i].range.layerCount){chosen=v.first;break;}}
        if(!chosen){++stats.framebufferMisses;return 2;}
        views[i]=chosen;
    }
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};ci.renderPass=pass;ci.attachmentCount=L.count;ci.pAttachments=views.data();ci.width=L.width;ci.height=L.height;ci.layers=L.layers;
    VkFramebuffer created{};if(raw.createFramebuffer(device,&ci,nullptr,&created)!=VK_SUCCESS||!created){++stats.framebufferMisses;return 2;}
    FramebufferRecord r{device,pass,L.width,L.height,L.layers,L.count,{},true};for(std::uint32_t i=0;i<L.count;++i)r.views[i]=views[i];
    framebufferRegistry[created]=r;++stats.framebuffersOwned;framebufferGeneration.fetch_add(1);out=created;return 1;
}
thread_local SetMap replaySetMap;
void snapshotSetMap(VkDevice d,unsigned owner){
    std::lock_guard lock(coldMutex);replaySetMap.count=0;
    for(const auto& [set,record]:setRegistry){if(record.device!=d||replaySetMap.count>=replaySetMap.pairs.size())continue;replaySetMap.pairs[replaySetMap.count++]={set,owner?record.clone2:record.clone};}
}
void emitState(VkCommandBuffer c,const BoundState& s,const SetMap* map){
    if(s.pipeline)raw.bindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,s.pipeline);
    for(unsigned first=0;first<8;++first){const auto& call=s.setCalls[first];if(!call.valid)continue;
        std::array<VkDescriptorSet,8> sets=call.sets;if(map)for(unsigned i=0;i<call.count;++i)sets[i]=(*map)(sets[i]);
        raw.bindDescriptorSets(c,call.point,call.layout,first,call.count,sets.data(),call.offsetCount,call.offsets.data());}
    for(unsigned i=0;i<16;++i)if(s.vertexMask&(1u<<i))raw.bindVertexBuffers(c,i,1,&s.vertexBuffers[i],&s.vertexOffsets[i]);
    if(s.indexKnown)raw.bindIndexBuffer(c,s.indexBuffer,s.indexOffset,s.indexType);
    if(s.viewportKnown)raw.setViewport(c,0,1,&s.viewport);
    if(s.scissorKnown)raw.setScissor(c,0,1,&s.scissor);
    if(s.depthBiasKnown)raw.setDepthBias(c,s.depthBias[0],s.depthBias[1],s.depthBias[2]);
    if(s.blendKnown)raw.setBlendConstants(c,s.blend);
    if(s.boundsKnown)raw.setDepthBounds(c,s.bounds[0],s.bounds[1]);
    if(s.lineWidthKnown)raw.setLineWidth(c,s.lineWidth);
    for(unsigned f=0;f<2;++f){const VkStencilFaceFlags face=f?VK_STENCIL_FACE_BACK_BIT:VK_STENCIL_FACE_FRONT_BIT;
        if(s.stencilRefKnown&(1u<<f))raw.setStencilReference(c,face,s.stencilRef[f]);
        if(s.stencilCmpKnown&(1u<<f))raw.setStencilCompareMask(c,face,s.stencilCmp[f]);
        if(s.stencilWriteKnown&(1u<<f))raw.setStencilWriteMask(c,face,s.stencilWrite[f]);}
}
void emitState(VkCommandBuffer c,const BoundState& s,bool substituteSets){emitState(c,s,substituteSets?&replaySetMap:nullptr);}
}
namespace {
// Mirrors EngineDepthResolve::copy for the right eye: Depth(role 1) -> SampledDepth(role 4),
// both left in DEPTH_STENCIL_READ_ONLY_OPTIMAL with the engine's tracked layouts updated.
// Test helper: clear a right-eye target (by role) to a solid color, restoring the engine-tracked layout.
void emitClearRight(VkCommandBuffer c,const FrameMaps& maps,unsigned role,float r,float g,float b){
    const EyeImagePair* pair{};for(unsigned k=0;k<maps.imageCount;++k)if(maps.images[k].role==role)pair=&maps.images[k];
    if(!pair||!pair->right||!pair->rightTexture)return;
    std::uint32_t trackedValue{};std::memcpy(&trackedValue,reinterpret_cast<const void*>(pair->rightTexture+0xa0),sizeof trackedValue);
    VkImageLayout tracked=static_cast<VkImageLayout>(trackedValue);
    if(tracked!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL&&tracked!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&tracked!=VK_IMAGE_LAYOUT_GENERAL&&tracked!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&tracked!=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)tracked=VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};bar.srcQueueFamilyIndex=bar.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;bar.image=pair->right;bar.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    bar.oldLayout=tracked;bar.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;bar.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;bar.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    raw.pipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&bar);
    VkClearColorValue value{};value.float32[0]=r;value.float32[1]=g;value.float32[2]=b;value.float32[3]=1.f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    raw.clearColorImage(c,pair->right,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&value,1,&range);
    const VkImageLayout back=tracked==VK_IMAGE_LAYOUT_UNDEFINED?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:tracked;
    bar.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;bar.newLayout=back;bar.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bar.dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
    raw.pipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&bar);
    const std::uint32_t backValue=back;std::memcpy(reinterpret_cast<void*>(pair->rightTexture+0xa0),&backValue,sizeof backValue);
}
bool sdAliasWanted() noexcept { // debug.rdrvr.stereoreplay.sdalias, re-read every 32nd replayed depth stage (~1 s at 36 Hz)
#ifdef __ANDROID__
    if((sdAlias.calls.fetch_add(1,std::memory_order_relaxed)&31u)==0){char value[PROP_VALUE_MAX]{};
        // Default on (property unset); any value other than "1" turns it off.
        sdAlias.on.store(__system_property_get("debug.rdrvr.stereoreplay.sdalias",value)<=0||(value[0]=='1'&&value[1]==0)?1u:0u,std::memory_order_relaxed);}
#endif
    return sdAlias.on.load(std::memory_order_relaxed)!=0;
}
void emitDepthResolve(VkCommandBuffer c,const FrameMaps& maps){
    const EyeImagePair* depth{};const EyeImagePair* sampled{};
    for(unsigned k=0;k<maps.imageCount;++k){const auto& p=maps.images[k];if(p.role==1)depth=&p;else if(p.role==4)sampled=&p;}
    if(!depth||!sampled||!depth->right||!sampled->right||!depth->rightTexture||!sampled->rightTexture||depth->right==sampled->right||
       depth->format!=sampled->format||depth->extent.width!=sampled->extent.width||depth->extent.height!=sampled->extent.height||!depth->extent.width){++stats.depthResolveMisses;return;}
    auto tracked=[](std::uintptr_t texture){std::uint32_t v{};std::memcpy(&v,reinterpret_cast<const void*>(texture+0xa0),sizeof v);return static_cast<VkImageLayout>(v);};
    const bool stencil=depth->format==VK_FORMAT_D24_UNORM_S8_UINT||depth->format==VK_FORMAT_D32_SFLOAT_S8_UINT||depth->format==VK_FORMAT_D16_UNORM_S8_UINT;
    const VkImageAspectFlags aspect=VK_IMAGE_ASPECT_DEPTH_BIT|(stencil?VK_IMAGE_ASPECT_STENCIL_BIT:0);
    VkImageLayout srcOld=tracked(depth->rightTexture),dstOld=tracked(sampled->rightTexture);
    if(srcOld!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL&&srcOld!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL&&srcOld!=VK_IMAGE_LAYOUT_GENERAL)srcOld=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if(sdAliasWanted()){ // SAMPLED DEPTH ALIAS: no copy; right Depth -> DS_RO (same final GPU/tracked layout as the copy path), the
        // right SampledDepth (image, layout, tracked layout) is left untouched and unread until a native depth stage copies it again
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={aspect,0,1,0,1};
        b.image=depth->right;b.oldLayout=srcOld;b.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        raw.pipelineBarrier(c,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,nullptr,0,nullptr,1,&b);
        const std::uint32_t readOnly=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;std::memcpy(reinterpret_cast<void*>(depth->rightTexture+0xa0),&readOnly,sizeof readOnly);
        sdAlias.active.store(true,std::memory_order_relaxed);sdAlias.skipped.fetch_add(1,std::memory_order_relaxed);return;}
    if(dstOld!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL&&dstOld!=VK_IMAGE_LAYOUT_GENERAL)dstOld=VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageMemoryBarrier pre[2]{};
    for(auto& b:pre){b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={aspect,0,1,0,1};}
    pre[0].image=depth->right;pre[0].oldLayout=srcOld;pre[0].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pre[0].srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_SHADER_READ_BIT;pre[0].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    pre[1].image=sampled->right;pre[1].oldLayout=dstOld;pre[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    pre[1].srcAccessMask=VK_ACCESS_SHADER_READ_BIT;pre[1].dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    raw.pipelineBarrier(c,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,pre);
    VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};region.dstSubresource=region.srcSubresource;region.extent=depth->extent;if(!region.extent.depth)region.extent.depth=1;
    raw.copyImage(c,depth->right,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,sampled->right,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    VkImageMemoryBarrier post[2]{};
    for(auto& b:post){b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={aspect,0,1,0,1};b.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;}
    post[0].image=depth->right;post[0].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;post[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    post[1].image=sampled->right;post[1].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;post[1].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    raw.pipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,0,0,nullptr,0,nullptr,2,post);
    const std::uint32_t readOnly=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    std::memcpy(reinterpret_cast<void*>(depth->rightTexture+0xa0),&readOnly,sizeof readOnly);
    std::memcpy(reinterpret_cast<void*>(sampled->rightTexture+0xa0),&readOnly,sizeof readOnly);
    ++stats.depthResolves;
}
}
bool replayFaulted() noexcept {return faulted;}
// EngineTextureMap::texture (declared there) asks this for every right-eye depth-role shader lookup.
bool sampledDepthAliased() noexcept {
    if(!sdAlias.active.load(std::memory_order_relaxed))return false;
    sdAlias.routed.fetch_add(1,std::memory_order_relaxed);return true;
}
void shaderCreated(VkShaderModule module,VkResult result) noexcept {
    Errno preserve;if(!module||result!=VK_SUCCESS||!lastPatchValid){
        if(module&&result==VK_SUCCESS)try{std::lock_guard lock(coldMutex);texScanByModule.erase(module);}catch(...){} // No scan -> unknown, never a reused handle's refs
        lastPatchValid=false;return;}
    try{std::lock_guard lock(coldMutex);shaderRegistry[module]={std::uint32_t(lastPatchReport.status),lastPatchReport.cameraMembers,lastPatchReport.set0Bindings,lastScan,lastHash,lastWords};++stats.shadersRegistered;
        texScanByModule[module]=lastTexScan;}catch(...){}
    mergeStatsValue.noteScan(lastTexScan);
    lastPatchValid=false;
}
void pipelinesCreated(std::uint32_t count,const VkGraphicsPipelineCreateInfo* infos,const VkPipeline* out,VkResult result) noexcept {
    Errno preserve;if(result!=VK_SUCCESS||!infos||!out)return;
    try{std::lock_guard lock(coldMutex);
        for(std::uint32_t i=0;i<count;++i){if(!out[i])continue;PipelineInfo info{};
            for(std::uint32_t s=0;s<infos[i].stageCount;++s)if(infos[i].pStages[s].stage==VK_SHADER_STAGE_VERTEX_BIT){info.module=infos[i].pStages[s].module;auto it=shaderRegistry.find(info.module);if(it!=shaderRegistry.end())info.vs=it->second;else info.vs.status=UINT32_MAX;}
            for(std::uint32_t s=0;s<infos[i].stageCount;++s)if(infos[i].pStages[s].stage==VK_SHADER_STAGE_FRAGMENT_BIT){info.fsModule=infos[i].pStages[s].module;auto f=shaderRegistry.find(info.fsModule);if(f!=shaderRegistry.end())info.fsHash=f->second.hash;}
            for(std::uint32_t s=0;s<infos[i].stageCount;++s){auto it=shaderRegistry.find(infos[i].pStages[s].module);if(it==shaderRegistry.end()||!it->second.scan.valid)continue;
                for(unsigned k=0;k<it->second.scan.count;++k){const auto& e=it->second.scan.entries[k];bool dup=false;
                    for(unsigned m=0;m<info.entryCount;++m)if(info.entries[m].binding==e.binding&&info.entries[m].kind==e.kind&&info.entries[m].offset==e.offset)dup=true;
                    if(!dup&&info.entryCount<info.entries.size())info.entries[info.entryCount++]=e;
                    if(infos[i].pStages[s].stage==VK_SHADER_STAGE_VERTEX_BIT&&e.kind==CameraMember::RageMatrices)info.vsRage=true;
                    if(infos[i].pStages[s].stage==VK_SHADER_STAGE_FRAGMENT_BIT&&e.kind==CameraMember::RageMatrices)info.fsRage=true;}}
            if(const auto* ds=infos[i].pDepthStencilState){ // Census: depth/stencil-writing draws (static state; a dynamic write mask still counts as writing)
                auto writes=[](const VkStencilOpState& f){return f.failOp!=VK_STENCIL_OP_KEEP||f.passOp!=VK_STENCIL_OP_KEEP||f.depthFailOp!=VK_STENCIL_OP_KEEP;};
                info.writesDepth=ds->depthTestEnable&&ds->depthWriteEnable;info.writesStencil=ds->stencilTestEnable&&(writes(ds->front)||writes(ds->back));
                info.testsStencil=ds->stencilTestEnable!=VK_FALSE;}
            { // Union of the stages' texture references (a module without a scan or with an unknown access: unknown)
                merge::TexScan u{};u.valid=true;bool overflow=false;
                for(std::uint32_t st=0;st<infos[i].stageCount;++st){auto m=texScanByModule.find(infos[i].pStages[st].module);
                    if(m==texScanByModule.end()||!m->second.valid||m->second.unknown){u.unknown=true;continue;}
                    for(unsigned k=0;k<m->second.count&&k<m->second.refs.size();++k){const auto ref=m->second.refs[k];bool dup=false;
                        for(unsigned q=0;q<u.count;++q)dup|=u.refs[q].binding==ref.binding&&u.refs[q].offset==ref.offset;
                        if(dup)continue;if(u.count<u.refs.size())u.refs[u.count++]=ref;else{u.unknown=true;overflow=true;}}}
                if(overflow)mergeStatsValue.scan.pipelinesOverflow.fetch_add(1,std::memory_order_relaxed);
                texScanByPipeline[out[i]]=u;}
            pipelineRegistry[out[i]]=info;++stats.pipelinesRegistered;}
        pipelineGeneration.fetch_add(1);
    }catch(...){}
}
void pipelineLayoutCreated(const VkPipelineLayoutCreateInfo* info,VkPipelineLayout layout,VkResult result) noexcept {
    Errno preserve;if(result!=VK_SUCCESS||!info||!layout)return;
    try{std::lock_guard lock(coldMutex);pipelineLayoutRegistry[layout]=info->setLayoutCount&&info->pSetLayouts?info->pSetLayouts[0]:VK_NULL_HANDLE;pipelineGeneration.fetch_add(1);}catch(...){}
}
namespace {
struct ProbeState {unsigned which{};std::array<float,16> relClip{},camera{},prevRelClip{},prevCamera{},otherRelClip{},otherCamera{};bool prevValid{},otherValid{};void* context{};std::uintptr_t ringMapped{};std::uint32_t ringSize{};
    std::array<std::uint32_t,2> depthIdx{},collIdx{},lightIdx{};bool idxValid{};};
thread_local ProbeState probeState;thread_local std::array<float,16> probePrevRel[2]{},probePrevCam[2]{};thread_local bool probePrevValid[2]{};
std::uint32_t textureBindlessIndex(std::uintptr_t texture){if(!texture)return UINT32_MAX;std::uintptr_t data{};std::memcpy(&data,reinterpret_cast<const void*>(texture+0x98),sizeof data);if(!data)return UINT32_MAX;std::uint32_t idx{};std::memcpy(&idx,reinterpret_cast<const void*>(data+0x58),sizeof idx);return idx;}
thread_local const FrameMaps* probeMaps{};
std::atomic<int> probeOverride{-1}; // host tests
bool probeWanted() noexcept { // debug.rdrvr.stereoreplay.probe=1 arms the light-stage probe (re-read every 64th call)
    const int forced=probeOverride.load(std::memory_order_relaxed);if(forced>=0)return forced!=0;
#ifdef __ANDROID__
    static std::atomic<unsigned> calls{};static std::atomic<bool> wanted{};
    if((calls.fetch_add(1,std::memory_order_relaxed)&63u)==0){char value[PROP_VALUE_MAX]{};wanted.store(__system_property_get("debug.rdrvr.stereoreplay.probe",value)>0&&value[0]=='1',std::memory_order_relaxed);}
    return wanted.load(std::memory_order_relaxed);
#else
    return false;
#endif
}
}
void setProbeEnabledForTests(bool enabled) noexcept {probeOverride.store(enabled?1:0,std::memory_order_relaxed);}
void setProbe(unsigned which,const float* relClip,const float* camera,void* context,const float* otherRelClip,const float* otherCamera) noexcept {
    if(!which){probeArmed.store(false,std::memory_order_relaxed);probeState.which=0;return;}
    Errno preserve;if(!probeWanted()){probeArmed.store(false,std::memory_order_relaxed);probeState.which=0;return;} // Off by default
    auto& p=probeState;p.which=0;probeArmed.store(false,std::memory_order_relaxed);if(!relClip||!camera||!context)return;
    std::memcpy(p.relClip.data(),relClip,64);std::memcpy(p.camera.data(),camera,64);p.context=context;
    p.otherValid=otherRelClip&&otherCamera;if(p.otherValid){std::memcpy(p.otherRelClip.data(),otherRelClip,64);std::memcpy(p.otherCamera.data(),otherCamera,64);}
    {const unsigned e=(which-1)&1u;p.prevValid=probePrevValid[e];p.prevRelClip=probePrevRel[e];p.prevCamera=probePrevCam[e];probePrevRel[e]=p.relClip;probePrevCam[e]=p.camera;probePrevValid[e]=true;}
    std::uintptr_t frame=0;std::memcpy(&frame,reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(context)+0x30a0),sizeof frame);
    p.ringMapped=0;p.ringSize=0;if(frame){std::uintptr_t wrapper=0;std::memcpy(&wrapper,reinterpret_cast<const void*>(frame+0x98),sizeof wrapper);std::memcpy(&p.ringMapped,reinterpret_cast<const void*>(frame+0xa0),sizeof p.ringMapped);if(wrapper)std::memcpy(&p.ringSize,reinterpret_cast<const void*>(wrapper+0x20),sizeof p.ringSize);}
    p.idxValid=false;
    if(probeMaps){for(unsigned k=0;k<probeMaps->imageCount;++k){const auto& pr=probeMaps->images[k];std::array<std::uint32_t,2>* dst=pr.role==4?&p.depthIdx:pr.role==2?&p.collIdx:pr.role==3?&p.lightIdx:nullptr;if(!dst)continue;(*dst)[0]=textureBindlessIndex(pr.leftTexture);(*dst)[1]=textureBindlessIndex(pr.rightTexture);p.idxValid=true;}}
    if(p.ringMapped&&p.ringSize){p.which=which;probeArmed.store(true,std::memory_order_relaxed);}
}
void probeDrawArmed(VkCommandBuffer c) noexcept {
    Errno preserve;auto& p=probeState;if(!p.which)return;
    try{
        auto& st=stats.probe[p.which-1];++st.draws;
        auto* s=tracked(c,false);if(!s||!s->pipeline)return;
        const PipelineInfo* info{};VkDescriptorSetLayout set0Layout{};std::vector<std::uint32_t> dyn;
        {std::lock_guard lock(coldMutex);auto it=pipelineRegistry.find(s->pipeline);if(it==pipelineRegistry.end()||!it->second.entryCount)return;info=&it->second;
            const auto& call=s->setCalls[0];if(!call.valid){++st.noSet0;return;}
            auto pl=pipelineLayoutRegistry.find(call.layout);if(pl==pipelineLayoutRegistry.end()){++st.noLayout;return;}set0Layout=pl->second;
            auto lr=layoutRegistry.find(set0Layout);if(lr==layoutRegistry.end()){++st.noLayout;return;}dyn=lr->second.dynamicBindings;}
        ++st.withEntries;const auto& call=s->setCalls[0];
        std::ostringstream sample;bool wantSample=st.sample.empty();
        auto rd=[&](std::uint32_t off,void* out,std::size_t n){if(std::uint64_t(off)+n>p.ringSize)return false;std::memcpy(out,reinterpret_cast<const void*>(p.ringMapped+off),n);return true;};
        auto cmp=[&](const float* a,const float* b,unsigned n){for(unsigned i=0;i<n;++i)if(std::fabs(a[i]-b[i])>1e-3f*(1+std::fabs(b[i])))return false;return true;};
        auto invRow=[&](const float* in,float* out)->bool{double a[4][8]{};for(unsigned r=0;r<4;++r){for(unsigned col=0;col<4;++col)a[r][col]=in[r*4+col];a[r][4+r]=1;}
            for(unsigned col=0;col<4;++col){unsigned piv=col;for(unsigned r=col+1;r<4;++r)if(std::fabs(a[r][col])>std::fabs(a[piv][col]))piv=r;if(std::fabs(a[piv][col])<1e-18)return false;for(unsigned j=0;j<8;++j)std::swap(a[col][j],a[piv][j]);const double sc=a[col][col];for(auto& v:a[col])v/=sc;for(unsigned r=0;r<4;++r)if(r!=col){const double f=a[r][col];for(unsigned j=0;j<8;++j)a[r][j]-=f*a[col][j];}}
            for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col)out[r*4+col]=float(a[r][col+4]);return true;};
        auto arr=[&](const char* name,const float* m,unsigned n){sample<<"\""<<name<<"\":[";for(unsigned i=0;i<n;++i)sample<<(i?",":"")<<m[i];sample<<"],";};
        for(unsigned k=0;k<info->entryCount;++k){const auto& e=info->entries[k];
            unsigned idx=UINT32_MAX;for(unsigned j=0;j<dyn.size()&&j<call.offsetCount;++j)if(dyn[j]==e.binding){idx=j;break;}
            if(idx==UINT32_MAX)continue;const std::uint32_t base=call.offsets[idx];
            switch(e.kind){
            case CameraMember::WvpInverse:{float m[16];if(!rd(base+e.offset,m,64))break;bool zero=true;for(float v:m)if(v!=0.f)zero=false;if(zero){++st.wvpZero;break;}
                double a[4][8]{};bool ok=true;for(unsigned r=0;r<4;++r){for(unsigned col=0;col<4;++col)a[r][col]=p.relClip[r*4+col];a[r][4+r]=1;}
                for(unsigned col=0;col<4&&ok;++col){unsigned piv=col;for(unsigned r=col+1;r<4;++r)if(std::fabs(a[r][col])>std::fabs(a[piv][col]))piv=r;if(std::fabs(a[piv][col])<1e-18){ok=false;break;}for(unsigned j=0;j<8;++j)std::swap(a[col][j],a[piv][j]);const double sc=a[col][col];for(auto& v:a[col])v/=sc;for(unsigned r=0;r<4;++r)if(r!=col){const double f=a[r][col];for(unsigned j=0;j<8;++j)a[r][j]-=f*a[col][j];}}
                float inv[16]{};if(ok)for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col)inv[r*4+col]=float(a[r][col+4]);
                if(ok&&cmp(m,inv,16))++st.wvpMatch;else{++st.wvpMismatch;
                    if(p.prevValid){double b2[4][8]{};bool ok2=true;for(unsigned r=0;r<4;++r){for(unsigned col=0;col<4;++col)b2[r][col]=p.prevRelClip[r*4+col];b2[r][4+r]=1;}
                        for(unsigned col=0;col<4&&ok2;++col){unsigned piv=col;for(unsigned r=col+1;r<4;++r)if(std::fabs(b2[r][col])>std::fabs(b2[piv][col]))piv=r;if(std::fabs(b2[piv][col])<1e-18){ok2=false;break;}for(unsigned j=0;j<8;++j)std::swap(b2[col][j],b2[piv][j]);const double sc=b2[col][col];for(auto& v:b2[col])v/=sc;for(unsigned r=0;r<4;++r)if(r!=col){const double f=b2[r][col];for(unsigned j=0;j<8;++j)b2[r][j]-=f*b2[col][j];}}
                        float inv2[16]{};if(ok2)for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col)inv2[r*4+col]=float(b2[r][col+4]);if(ok2&&cmp(m,inv2,16))++st.wvpMatchPrev;}
                    if(p.otherValid){float inv3[16]{};if(invRow(p.otherRelClip.data(),inv3)&&cmp(m,inv3,16))++st.wvpMatchOther;}
                    if(wantSample){arr("wvpInvDraw",m,16);arr("invRelClipEye",inv,16);arr("relClipPrevEye",p.prevRelClip.data(),16);arr("relClipOtherEye",p.otherRelClip.data(),16);}}break;}
            case CameraMember::RageMatrices:{float m1[16],m3[16];if(!rd(base+64,m1,64)||!rd(base+192,m3,64))break;
                if(cmp(m1,p.relClip.data(),16))++st.m1Match;else{++st.m1Mismatch;if(wantSample)arr("M1draw",m1,16);}
                if(cmp(m3,p.camera.data(),12)&&cmp(m3+12,p.camera.data()+12,3))++st.m3Match;else{++st.m3Mismatch;if(p.prevValid&&cmp(m3,p.prevCamera.data(),12)&&cmp(m3+12,p.prevCamera.data()+12,3))++st.m3MatchPrev;if(p.otherValid&&cmp(m3,p.otherCamera.data(),12)&&cmp(m3+12,p.otherCamera.data()+12,3))++st.m3MatchOther;if(wantSample){arr("M3draw",m3,16);arr("cameraEye",p.camera.data(),16);arr("cameraPrevEye",p.prevCamera.data(),16);}}break;}
            case CameraMember::DepthMapSampler:case CameraMember::ShadowCollectorSampler:case CameraMember::LightBufferSampler:{std::uint32_t v{};if(!rd(base+e.offset,&v,4))break;
                const auto& ref=e.kind==CameraMember::DepthMapSampler?p.depthIdx:e.kind==CameraMember::ShadowCollectorSampler?p.collIdx:p.lightIdx;
                auto& L=e.kind==CameraMember::DepthMapSampler?st.depthL:e.kind==CameraMember::ShadowCollectorSampler?st.collL:st.lightL;
                auto& Rr=e.kind==CameraMember::DepthMapSampler?st.depthR:e.kind==CameraMember::ShadowCollectorSampler?st.collR:st.lightR;
                auto& O=e.kind==CameraMember::DepthMapSampler?st.depthOther:e.kind==CameraMember::ShadowCollectorSampler?st.collOther:st.lightOther;
                if(p.idxValid&&v==ref[0])++L;else if(p.idxValid&&v==ref[1])++Rr;else{++O;}
                if(wantSample){sample<<"\""<<(e.kind==CameraMember::DepthMapSampler?"depthIdx":e.kind==CameraMember::ShadowCollectorSampler?"collIdx":"lightIdx")<<"\":["<<v<<","<<ref[0]<<","<<ref[1]<<"],";}
                break;}
            default:break;}
        }
        if(wantSample){std::string text=sample.str();if(!text.empty()){arr("relClipEye",p.relClip.data(),16);std::lock_guard g(stats.skipMutex);if(st.sample.empty())st.sample=sample.str();}}
    }catch(...){}
}
unsigned liveMask() noexcept {
#ifdef __ANDROID__
    const unsigned menu=testModeMask(testMode.load(std::memory_order_relaxed));
    char value[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.stereoreplay.mask",value)<=0)return menu;
    return menu|unsigned(std::strtoul(value,nullptr,0));
#else
    return 0;
#endif
}
// ================================================================= right-eye uniform rewrite (both replay paths)
namespace {
// Top-down private region of the engine frame ring: reset when the engine starts a new frame object; consecutive
// replays of one frame (depth, color; inline or secondaries) share it.
// The region cursor (topFrame/topCursor), prevRelClipL and the dynamic-binding cache live in threadHot.
constexpr std::uint64_t maxUniformRange=65536;
// Dynamic-binding order per pipeline layout, cached per thread (threadHot.dynCache) and dropped when the registries change.
std::uint64_t dkey(std::uint32_t src,CameraMember kind){return (std::uint64_t(src)<<8)|std::uint64_t(kind);}
int compare16(const float* a,const float* b){float worstD=0,worstT=0;for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){const float v=a[r*4+c];worstD=std::max(worstD,std::fabs(v-b[r*4+c]));worstT=std::max(worstT,std::fabs(v-b[c*4+r]));}
        const float tol=1e-3f;if(worstD<=tol)return 0;if(worstT<=tol)return 1;return 2;}
void transpose16(const float* in,float* out){for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)out[c*4+r]=in[r*4+c];}
void mulRM(const float* a,const float* b,float* out){for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){float v=0;for(unsigned k=0;k<4;++k)v+=a[r*4+k]*b[k*4+c];out[r*4+c]=v;}}
bool invRM(const float* in,float* out){double a[4][8]{};for(unsigned r=0;r<4;++r){for(unsigned c=0;c<4;++c)a[r][c]=in[r*4+c];a[r][4+r]=1;}
        for(unsigned col=0;col<4;++col){unsigned pivot=col;for(unsigned r=col+1;r<4;++r)if(std::fabs(a[r][col])>std::fabs(a[pivot][col]))pivot=r;if(std::fabs(a[pivot][col])<1e-18)return false;for(unsigned j=0;j<8;++j)std::swap(a[col][j],a[pivot][j]);const double sc=a[col][col];for(auto& v:a[col])v/=sc;for(unsigned r=0;r<4;++r)if(r!=col){const double f=a[r][col];for(unsigned j=0;j<8;++j)a[r][j]-=f*a[col][j];}}
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)out[r*4+c]=float(a[r][c+4]);return true;}
bool invert16(const float* in,float* out){double a[4][8]{};for(unsigned r=0;r<4;++r){for(unsigned c=0;c<4;++c)a[r][c]=in[c*4+r];a[r][4+r]=1;}
        for(unsigned col=0;col<4;++col){unsigned pivot=col;for(unsigned r=col+1;r<4;++r)if(std::fabs(a[r][col])>std::fabs(a[pivot][col]))pivot=r;if(std::fabs(a[pivot][col])<1e-18)return false;for(unsigned j=0;j<8;++j)std::swap(a[col][j],a[pivot][j]);const double s=a[col][col];for(auto& v:a[col])v/=s;for(unsigned r=0;r<4;++r)if(r!=col){const double f=a[r][col];for(unsigned j=0;j<8;++j)a[r][j]-=f*a[col][j];}}
        for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)out[c*4+r]=float(a[r][c+4]);return true;}
}
namespace internal {
ReplayRewriter::ReplayRewriter(Stage stage_,const FrameMaps& maps_,unsigned mask_,VkPipelineLayout layout_,const SetMap* sets_,const RingSnapshot& ring,const RewriteOptions& options) noexcept
    :stage(stage_),maps(maps_),mask(mask_),layout(layout_),sets(sets_),
     uboEnabled(ring.uboEnabled),framePtr(ring.framePtr),ringMapped(ring.ringMapped),ringCursor(ring.ringCursor),ringSize(ring.ringSize),ringValid(ring.ringValid),
     region(options.region),ownInfos(options.ownInfos),diagnostics(options.diagnostics){
    threadEntered();
    dedupe.clear();
    align=std::max<std::uint32_t>(maps.uniformAlignment,64);
    // Right-eye blocks live in a private top-down region of the frame ring (the engine allocates bottom-up
    // from its own pointer; writing its cursor field is not enough to reserve space). The region is reset when
    // the engine starts a new frame object; both replays of one frame share it. Keep offset+maxRange <= size.
    if(ringValid&&!region){if(threadHot.topFrame!=framePtr||threadHot.topCursor==0||threadHot.topCursor>ringSize){threadHot.topFrame=framePtr;threadHot.topCursor=(ringSize-maxUniformRange)&~std::uint64_t(align-1);}}
    floorCursor=ringCursor+(1u<<20); // leave 1 MB headroom above the engine's current usage
    if(region&&region->floor>floorCursor)floorCursor=region->floor;
    if(invRM(maps.relClip[0].data(),relLInv)){mulRM(relLInv,maps.relClip[1].data(),K_LtoR);relationValid=invRM(K_LtoR,G_RtoL);}
}
const std::vector<std::uint32_t>* ReplayRewriter::dynFor(VkPipelineLayout l){
    auto& h=threadHot;if(!h.dynCache){h.dynCache=new DynCache;} // per thread, never freed (render thread, worker)
    auto& dynCache=h.dynCache->map;
    auto f=dynCache.find(l);if(f!=dynCache.end())return &f->second;
    std::vector<std::uint32_t> v;{std::lock_guard lock(coldMutex);auto pl=pipelineLayoutRegistry.find(l);if(pl!=pipelineLayoutRegistry.end()){auto lr=layoutRegistry.find(pl->second);if(lr!=layoutRegistry.end())v=lr->second.dynamicBindings;}}
    return &dynCache.emplace(l,std::move(v)).first->second;}
const PipelineInfo* ReplayRewriter::infoFor(VkPipeline pipeline){
    if(ownInfos){ // worker: copy under coldMutex (a concurrent re-registration must not be read half-written); node pointers stay valid for the job
        auto mine=ownedInfos.find(pipeline);if(mine!=ownedInfos.end())return &mine->second;
        PipelineInfo copy{};{std::lock_guard lock(coldMutex);auto f=pipelineRegistry.find(pipeline);if(f==pipelineRegistry.end())return nullptr;copy=f->second;}
        return &ownedInfos.emplace(pipeline,copy).first->second;}
    std::lock_guard lock(coldMutex);auto f=pipelineRegistry.find(pipeline);return f==pipelineRegistry.end()?nullptr:&f->second;}
std::uint32_t ReplayRewriter::alloc(std::uint32_t bytes) noexcept {
    const std::uint64_t need=(std::uint64_t(bytes)+align-1)&~std::uint64_t(align-1);
    if(region){ // shared per-frame region (worker jobs and inline replays of one frame)
        if(region->pressure.load(std::memory_order_relaxed)||((regionAllocs++&7u)==0&&region->liveCursor()>region->floor)){
            region->pressure.store(true,std::memory_order_relaxed);++stats.uboRingFull;return UINT32_MAX;}
        std::uint64_t top=region->top.load(std::memory_order_relaxed);
        do{if(top<need||top-need<floorCursor){++stats.uboRingFull;return UINT32_MAX;}}while(!region->top.compare_exchange_weak(top,top-need,std::memory_order_relaxed));
        ++stats.uboBlocks;stats.uboBytes.fetch_add(bytes);return std::uint32_t(top-need);}
    if(threadHot.topCursor<need||threadHot.topCursor-need<floorCursor){++stats.uboRingFull;return UINT32_MAX;}
    threadHot.topCursor-=need;++stats.uboBlocks;stats.uboBytes.fetch_add(bytes);return std::uint32_t(threadHot.topCursor);}
void ReplayRewriter::writeMat(std::uintptr_t at,const float* m) noexcept {if(transposedMode){float t[16];transpose16(m,t);std::memcpy(reinterpret_cast<void*>(at),t,64);}else std::memcpy(reinterpret_cast<void*>(at),m,64);}
std::uint32_t ReplayRewriter::rageBlock(std::uint32_t src,std::uint32_t blockSize){
        const std::uint32_t bytes=std::max<std::uint32_t>(blockSize,272u); // rage_matrices has a 5th member at 256
        if(src+bytes>ringSize)return UINT32_MAX;const auto dst=alloc(bytes);if(dst==UINT32_MAX)return dst;
        const auto from=ringMapped+src,to=ringMapped+dst;std::memcpy(reinterpret_cast<void*>(to),reinterpret_cast<const void*>(from),bytes);
        if(checks<4){float m1[16],m3[16];std::memcpy(m1,reinterpret_cast<const void*>(from+64),64);std::memcpy(m3,reinterpret_cast<const void*>(from+192),64);
            const int c1=compare16(m1,maps.relClip[0].data()),c3=compare16(m3,maps.camera[0].data());
            (c1==0?stats.rageM1Match:c1==1?stats.rageM1Transposed:stats.rageM1Mismatch)++;(c3==0?stats.rageM3Match:c3==1?stats.rageM3Transposed:stats.rageM3Mismatch)++;
            if(checks==0&&c1==1)transposedMode=true;++checks;
            if(diagnostics&&!stats.uboSampled.exchange(true)){try{std::ostringstream o;auto arr=[&](const char* name,const float* m){o<<"\""<<name<<"\":[";for(unsigned i=0;i<16;++i)o<<(i?",":"")<<m[i];o<<"],";};
                float m0[16],m2[16];std::memcpy(m0,reinterpret_cast<const void*>(from),64);std::memcpy(m2,reinterpret_cast<const void*>(from+128),64);
                arr("M0_gWorldSingle",m0);arr("M1_gWorldView",m1);arr("M2_gWorldViewProj",m2);arr("M3_gViewInverse",m3);arr("relClipL",maps.relClip[0].data());arr("cameraL",maps.camera[0].data());arr("relClipR",maps.relClip[1].data());arr("cameraR",maps.camera[1].data());arr("correction",maps.correction.data());
                std::lock_guard g(stats.skipMutex);stats.uboSamples+=o.str();}catch(...){}}}
        {float m1[16],m1r[16];std::memcpy(m1,reinterpret_cast<const void*>(from+64),64);
         if(relationValid){mulRM(m1,K_LtoR,m1r);std::memcpy(reinterpret_cast<void*>(to+64),m1r,64);}else writeMat(to+64,maps.relClip[1].data());
         if(compare16(m1,maps.relClip[0].data())!=0){++m1Mismatches;++stats.rageM1AllMismatch;
            if(diagnostics&&!sampled_M1mismatch&&(sampled_M1mismatch=true)){try{float m0[16];std::memcpy(m0,reinterpret_cast<const void*>(from),64);std::ostringstream o;auto arr=[&](const char* name,const float* m){o<<"\""<<name<<"\":[";for(unsigned i=0;i<16;++i)o<<(i?",":"")<<m[i];o<<"],";};arr("M1mismatch",m1);arr("M0ofMismatch",m0);arr("relClipL_atMismatch",maps.relClip[0].data());std::lock_guard g(stats.skipMutex);stats.uboSamples+=o.str();}catch(...){}}}
         else ++stats.rageM1AllMatch;}
        {float m3r[16];std::memcpy(m3r,maps.camera[1].data(),64);float w=1.f;std::memcpy(&w,reinterpret_cast<const void*>(from+192+60),sizeof w);m3r[15]=w;writeMat(to+192,m3r);}
        if(!transposedMode){float m2[16],o[16];std::memcpy(m2,reinterpret_cast<const void*>(from+128),64);const float* cc=maps.correction.data();
            for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c){float s=0;for(unsigned k=0;k<4;++k)s+=m2[r*4+k]*cc[k*4+c];o[r*4+c]=s;}std::memcpy(reinterpret_cast<void*>(to+128),o,64);}
        return dst;}
std::uint32_t ReplayRewriter::blockCopy(std::uint32_t src,std::uint32_t memberOffset,CameraMember kind,std::uint32_t blockSize){
        std::uint32_t bytes=std::max<std::uint32_t>({memberOffset+64u,1024u,blockSize});if(src>=ringSize)return UINT32_MAX;bytes=std::min<std::uint32_t>(bytes,ringSize-src);if(bytes<memberOffset+64)return UINT32_MAX;
        {auto f=dedupe.find(dkey(src,kind));if(f!=dedupe.end()){if(f->second.memberOffset==memberOffset&&f->second.bytes>=bytes)return f->second.dst;++stats.uboDedupeMismatch;}}
        const auto dst=alloc(bytes);if(dst==UINT32_MAX)return dst;
        if(blockSize>stats.maxBlockSize.load())stats.maxBlockSize.store(blockSize);
        const auto from=ringMapped+src,to=ringMapped+dst;std::memcpy(reinterpret_cast<void*>(to),reinterpret_cast<const void*>(from),bytes);
        if(kind==CameraMember::WvpInverse){
            if(!relClipRInvValid)relClipRInvValid=invert16(maps.relClip[1].data(),relClipRInv.data());
            if(checks<8){float left[16];std::memcpy(left,reinterpret_cast<const void*>(from+memberOffset),64);float li[16]{};invert16(maps.relClip[0].data(),li);
                (compare16(left,li)==0?stats.wvpInvMatch:stats.wvpInvMismatch)++;++checks;
                if(diagnostics&&!sampled_WVPInvL&&(sampled_WVPInvL=true)){try{std::ostringstream o;auto arr=[&](const char* name,const float* m){o<<"\""<<name<<"\":[";for(unsigned i=0;i<16;++i)o<<(i?",":"")<<m[i];o<<"],";};arr("WVPInvL",left);arr("invRelClipL",li);std::lock_guard g(stats.skipMutex);stats.uboSamples+=o.str();}catch(...){}}}
            {float left[16];std::memcpy(left,reinterpret_cast<const void*>(from+memberOffset),64);bool zero=true;for(float v:left)if(v!=0.f)zero=false;
             float li[16]{};const bool matches=!zero&&invert16(maps.relClip[0].data(),li)&&compare16(left,li)==0;
             if(!zero&&diagnostics&&!sampled_WVPInvNZ&&(sampled_WVPInvNZ=true)){try{std::ostringstream o;auto arr=[&](const char* name,const float* m){o<<"\""<<name<<"\":[";for(unsigned i=0;i<16;++i)o<<(i?",":"")<<m[i];o<<"],";};arr("WVPInvNZ",left);arr("invRelClipL_NZ",li);arr("relClipL_NZ",maps.relClip[0].data());std::lock_guard g(stats.skipMutex);stats.uboSamples+=o.str();}catch(...){}}
             if(!zero&&relationValid){float r[16];mulRM(G_RtoL,left,r);std::memcpy(reinterpret_cast<void*>(to+memberOffset),r,64);++stats.wvpInvRewritten;if(!matches)++stats.wvpInvRelational;}else ++stats.wvpInvLeft;}
        }else if(kind==CameraMember::PrevViewProj){
            if(diagnostics&&!sampled_PrevViewProjL&&(sampled_PrevViewProjL=true)){try{float left[16];std::memcpy(left,reinterpret_cast<const void*>(from+memberOffset),64);std::ostringstream o;auto arr=[&](const char* name,const float* m){o<<"\""<<name<<"\":[";for(unsigned i=0;i<16;++i)o<<(i?",":"")<<m[i];o<<"],";};arr("PrevViewProjL",left);arr("relClipL_now",maps.relClip[0].data());arr("relClipL_prev",threadHot.prevRelClipL.data());std::lock_guard g(stats.skipMutex);stats.uboSamples+=o.str();}catch(...){}}
            writeMat(to+memberOffset,maps.relClip[1].data());++stats.prevProjRight;
            if(!(mask&(1u<<29))){writeMat(from+memberOffset,maps.relClip[0].data());++stats.prevProjLeft;} // left eye: zero velocity too (in place, GPU has not executed yet)
        }else if(kind==CameraMember::CameraMatrix){
            if(checks<8){float left[16];std::memcpy(left,reinterpret_cast<const void*>(from+memberOffset),64);(compare16(left,maps.camera[0].data())==0?stats.cameraMatch:stats.cameraMismatch)++;++checks;
                if(diagnostics&&!sampled_gCameraMatrixL&&(sampled_gCameraMatrixL=true)){try{std::ostringstream o;auto arr=[&](const char* name,const float* m){o<<"\""<<name<<"\":[";for(unsigned i=0;i<16;++i)o<<(i?",":"")<<m[i];o<<"],";};arr("gCameraMatrixL",left);std::lock_guard g(stats.skipMutex);stats.uboSamples+=o.str();}catch(...){}}}
            writeMat(to+memberOffset,maps.camera[1].data());
        }
        dedupe[dkey(src,kind)]=DedupeHit{dst,memberOffset,bytes};return dst;}
void ReplayRewriter::ensurePush(VkCommandBuffer c,bool identity) noexcept {if(identity==pushIdentity)return;raw.pushConstants(c,layout,VK_SHADER_STAGE_VERTEX_BIT,0,64,identity?identityMatrix:maps.correction.data());pushIdentity=identity;if(identity)++stats.identityPushesDraw;}
void ReplayRewriter::emitSet0(VkCommandBuffer c,const std::array<std::uint32_t,16>& offs) noexcept {VkDescriptorSet s0=substituteSet(set0.set);raw.bindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,set0.layout,0,1,&s0,set0.count,offs.data());emittedOffsets=offs;emittedValid=true;set0Pending=false;++stats.uboBinds;}
void ReplayRewriter::rewriteForDraw(VkCommandBuffer c){
        if(!set0.valid)return;
        if(!uboEnabled||!ringValid||!boundInfo||!boundInfo->entryCount){if(set0Pending)emitSet0(c,set0.offsets);if(pushIdentity)ensurePush(c,false);return;}
        const auto* dynv=dynFor(set0.layout);if(!dynv||dynv->empty()){++stats.uboNoDynamic;if(set0Pending)emitSet0(c,set0.offsets);ensurePush(c,false);return;}
        struct DynRef {const std::vector<std::uint32_t>& second;} dynRef{*dynv};const DynRef* dyn=&dynRef;
        std::array<std::uint32_t,16> offs=set0.offsets;bool changed=false,rageReplaced=false;
        for(unsigned k=0;k<boundInfo->entryCount;++k){const auto& e=boundInfo->entries[k];
            unsigned idx=UINT32_MAX;for(unsigned j=0;j<dyn->second.size()&&j<16;++j)if(dyn->second[j]==e.binding){idx=j;break;}
            if(idx==UINT32_MAX||idx>=set0.count){++stats.uboNoDynamic;continue;}
            if(e.kind==CameraMember::CameraMatrix)continue; // gCameraMatrix is a world->shadow transform, not the eye camera
            if(unsigned(e.kind)>=unsigned(CameraMember::DepthMapSampler))continue; // probe-only entries
            if(e.kind==CameraMember::PrevViewProj&&(mask&(1u<<29)))continue;
            const std::uint32_t src=set0.offsets[idx];
            const std::uint32_t dst=e.kind==CameraMember::RageMatrices?rageBlock(src,e.blockSize):blockCopy(src,e.offset,e.kind,e.blockSize);
            if(dst==UINT32_MAX)continue;offs[idx]=dst;changed=true;if(e.kind==CameraMember::RageMatrices)rageReplaced=true;}
        if(set0Pending||!emittedValid||offs!=emittedOffsets)emitSet0(c,offs);
        if(changed){++stats.uboDraws;for(unsigned k=0;k<pipeCount;++k)if(pipes[k].info==boundInfo)pipes[k].rewritten=true;}
        ensurePush(c,rageReplaced&&boundInfo->vsRage);}
// returns true when this draw must be skipped under the live mask
bool ReplayRewriter::classifyDraw(unsigned passIndex) noexcept {
        if(boundInfo&&stage==Stage::Color){bool found=false;for(unsigned k=0;k<pipeCount;++k)if(pipes[k].info==boundInfo){++pipes[k].draws;found=true;break;}
            if(!found&&pipeCount<pipes.size())pipes[pipeCount++]={boundInfo,1,passIndex,false};}
        if(!boundInfo){++drawsUnknown;return (mask&(1u<<24))!=0;}
        const bool patched=boundInfo->vs.status==std::uint32_t(PatchStatus::Patched);
        if(patched){++drawsPatched;return (mask&(1u<<23))!=0;}
        ++drawsUnpatched;if(stage==Stage::Color&&passIndex<=1)++drawsUnpatchedPass0;
        bool found=false;for(unsigned k=0;k<unpatchedCount;++k)if(unpatched[k].module==boundInfo->module){++unpatched[k].draws;found=true;}
        if(!found&&unpatchedCount<unpatched.size())unpatched[unpatchedCount++]={boundInfo->module,boundInfo->vs,1,boundInfo->fsHash};
        return (mask&(1u<<22))!=0;}
void ReplayRewriter::bindPipeline(VkPipelineBindPoint point,VkPipeline pipeline){if(point==VK_PIPELINE_BIND_POINT_GRAPHICS){boundPipeline=pipeline;boundInfo=infoFor(boundPipeline);}}
void ReplayRewriter::bindSets(VkCommandBuffer c,const PBindSets* r){
    auto* sets=reinterpret_cast<const VkDescriptorSet*>(r+1);auto* offsets=reinterpret_cast<const std::uint32_t*>(sets+r->count);
    std::array<VkDescriptorSet,64> substituted{};for(std::uint32_t i=0;i<r->count;++i)substituted[i]=substituteSet(sets[i]);
    if(uboEnabled&&ringValid&&r->point==VK_PIPELINE_BIND_POINT_GRAPHICS&&r->first==0&&r->count>=1){
        unsigned k=0;if(const auto* dv=dynFor(r->layout))k=unsigned(dv->size());
        stats.uboLastOffsets.store(r->offsets);stats.uboLastDynamic.store(k);
        if(k==0||k>16||k>r->offsets){++stats.uboCountMismatch;if(c)raw.bindDescriptorSets(c,r->point,r->layout,r->first,r->count,substituted.data(),r->offsets,offsets);set0.valid=false;set0Pending=false;return;}
        // Set 0 is bound at the draw (with rewritten offsets); the remaining sets of this call are bound now.
        if(r->count>1&&c)raw.bindDescriptorSets(c,r->point,r->layout,1,r->count-1,substituted.data()+1,r->offsets-k,offsets+k);
        set0.layout=r->layout;set0.set=sets[0];set0.valid=true;set0.count=k;for(unsigned i=0;i<k;++i)set0.offsets[i]=offsets[i];emittedValid=false;set0Pending=true;
        return;}
    if(c)raw.bindDescriptorSets(c,r->point,r->layout,r->first,r->count,substituted.data(),r->offsets,offsets);
}
void ReplayRewriter::threadEntered() noexcept {auto& h=threadHot;if(h.dynCacheGeneration!=pipelineGeneration.load()){if(h.dynCache)h.dynCache->map.clear();h.dynCacheGeneration=pipelineGeneration.load();}}
void ReplayRewriter::targetRestarted() noexcept {emittedValid=false;set0Pending=set0.valid;pushIdentity=false;}
void ReplayRewriter::finish() noexcept {
    if(ringValid){stats.uboCursorLast.store(region?region->top.load(std::memory_order_relaxed):threadHot.topCursor);}
    if(stage==Stage::Color)threadHot.prevRelClipL=maps.relClip[0];
    if(stage==Stage::Color){stats.drawsPatched.store(drawsPatched);stats.drawsUnpatched.store(drawsUnpatched);stats.drawsUnknownPipeline.store(drawsUnknown);stats.drawsUnpatchedPass0.store(drawsUnpatchedPass0);
        try{std::lock_guard g(stats.skipMutex); // Raw rows; report() formats them
            stats.pipeRowCount=std::min<unsigned>(pipeCount,unsigned(stats.pipeRows.size()));
            for(unsigned k=0;k<stats.pipeRowCount;++k){const auto& ps=pipes[k];
                stats.pipeRows[k]={ps.info->vs.hash,ps.info->fsHash,ps.draws,ps.pass,ps.info->vs.status,ps.info->entryCount,ps.info->vsRage,ps.info->fsRage,ps.rewritten};}
            stats.unpatchedRowCount=std::min<unsigned>(unpatchedCount,unsigned(stats.unpatchedRows.size()));
            for(unsigned k=0;k<stats.unpatchedRowCount;++k){const auto& u=unpatched[k];
                stats.unpatchedRows[k]={reinterpret_cast<std::uint64_t>(u.module),u.vs.hash,u.fsHash,u.vs.status,u.vs.members,u.vs.set0Bindings,u.draws,u.vs.words};}
        }catch(...){}}
}

// ================================================================= replay helpers (both paths)
const RawCommands& rawCommands() noexcept {return raw;}
Stats& replayStats() noexcept {return stats;}
std::uint64_t replayClockNs() noexcept {return nowNs();}
const BoundState* trackedState(VkCommandBuffer c) noexcept {return tracked(c,false);}
bool pollEyeDump(){return eyeDump.poll();}
Tape& tapeFor(Stage stage) noexcept {return tapes[unsigned(stage)&1u];}
VkPipelineLayout replayPushLayout(VkDevice device) noexcept {return cachedPushLayoutFor(device);}
void snapshotSets(VkDevice d,unsigned owner,SetMap& out){
    std::lock_guard lock(coldMutex);out.count=0;
    for(const auto& [set,record]:setRegistry){if(record.device!=d||out.count>=out.pairs.size())continue;out.pairs[out.count++]={set,owner?record.clone2:record.clone};}
}
void resetLegacyRing() noexcept {threadHot.topFrame=0;threadHot.topCursor=0;}
void emitBoundState(VkCommandBuffer c,const BoundState& s,const SetMap* sets) noexcept {emitState(c,s,sets);}
RingSnapshot snapshotRing(const FrameMaps& maps,unsigned mask) noexcept {
    const bool uboEnabled=!(mask&(1u<<28))&&maps.context;
    std::uintptr_t framePtr=0,ringMapped=0;std::uint64_t ringCursor=0;std::uint32_t ringSize=0;bool ringValid=false;
    if(uboEnabled){
        auto rd=[](std::uintptr_t p,auto& v){std::memcpy(&v,reinterpret_cast<const void*>(p),sizeof v);};
        rd(reinterpret_cast<std::uintptr_t>(maps.context)+0x30a0,framePtr);
        if(framePtr){std::uintptr_t wrapper=0;rd(framePtr+0x98,wrapper);rd(framePtr+0xa0,ringMapped);rd(framePtr+0xa8,ringCursor);if(wrapper)rd(wrapper+0x20,ringSize);}
        ringValid=framePtr&&ringMapped&&ringSize&&ringCursor<=ringSize&&ringMapped%16==0;
        if(ringValid&&stats.wrapperDump.empty()){try{std::uintptr_t wrapper=0;rd(framePtr+0x98,wrapper);std::ostringstream o;o<<"\"frame\":[";for(unsigned i=0;i<0xc0/8;++i){std::uint64_t v=0;rd(framePtr+i*8,v);o<<(i?",":"")<<v;}o<<"],\"wrapper\":[";for(unsigned i=0;i<0x60/8;++i){std::uint64_t v=0;rd(wrapper+i*8,v);o<<(i?",":"")<<v;}o<<"]";std::lock_guard g(stats.skipMutex);stats.wrapperDump=o.str();}catch(...){}}
        if(!ringValid)++stats.uboFrameInvalid;else{stats.uboRingSize.store(ringSize);}
    }
    return RingSnapshot{uboEnabled,framePtr,ringMapped,ringCursor,ringSize,ringValid};
}
ForeignTargets scanForeignTargets(const Tape& t,const FrameMaps& maps,Stage stage){
    const Substitution sub{&maps};
    std::array<VkImage,32> foreign{};unsigned foreignCount=0;
    {std::size_t at=0;std::optional<std::ostringstream> fo;unsigned samples=0;
        // Only the image copies recorded in t.imageCopies (full walk when it overflowed)
        const bool walk=t.imageCopiesOverflow;unsigned next=0;
        while(walk?at<t.used:next<t.imageCopyCount){
            if(!walk)at=t.imageCopies[next++];
            const auto* h=reinterpret_cast<const Header*>(t.bytes.data()+at);const auto* p=t.bytes.data()+at+sizeof(Header);at+=sizeof(Header)+h->bytes;
            VkImage src{},dst{};const char* kind="";
            switch(static_cast<Op>(h->op)){
            case Op::CopyImage:{auto* r=reinterpret_cast<const PCopyImage*>(p);src=r->src;dst=r->dst;kind="copy";break;}
            case Op::BlitImage:{auto* r=reinterpret_cast<const PBlit*>(p);src=r->src;dst=r->dst;kind="blit";break;}
            case Op::ResolveImage:{auto* r=reinterpret_cast<const PResolve*>(p);src=r->src;dst=r->dst;kind="resolve";break;}
            default:continue;}
            bool ms=false,md=false;sub.image(src,&ms);sub.image(dst,&md);
            if(samples<6){if(!fo)fo.emplace();*fo<<(samples?",":"")<<"\""<<kind<<" src="<<reinterpret_cast<std::uint64_t>(src)<<(ms?"(eye)":"")<<" dst="<<reinterpret_cast<std::uint64_t>(dst)<<(md?"(eye)":"")<<"\"";++samples;}
            if(!ms||md)continue;
            bool known=false;for(unsigned i=0;i<foreignCount;++i)if(foreign[i]==dst)known=true;
            if(!known&&foreignCount<foreign.size())foreign[foreignCount++]=dst;
        }
        stats.foreignTargetsLast.store(foreignCount);
        if(samples){std::lock_guard g(stats.skipMutex);stats.foreignSamples[unsigned(stage)]=fo->str();}
    }
    ForeignTargets out{};out.images=foreign;out.count=foreignCount;return out;
}
int resolveRightFramebuffer(const FrameMaps& maps,VkRenderPass pass,VkFramebuffer left,VkFramebuffer& fb){
    thread_local std::unordered_map<std::uint64_t,VkFramebuffer> fbCache;thread_local unsigned fbCacheGeneration=UINT32_MAX;
    if(fbCacheGeneration!=framebufferGeneration.load()){fbCache.clear();fbCacheGeneration=framebufferGeneration.load();}
    const std::uint64_t fbKey=reinterpret_cast<std::uint64_t>(left)^(std::uint64_t(maps.owner&1u)<<63)^(reinterpret_cast<std::uint64_t>(maps.imageCount?maps.images[0].right:VK_NULL_HANDLE)*0x9E3779B97F4A7C15ull);
    int resolved;{auto hit=fbCache.find(fbKey);if(hit!=fbCache.end()){fb=hit->second;resolved=1;++stats.fbCacheHits;}else{const Substitution sub{&maps};resolved=resolveFramebuffer(sub,maps.device,pass,left,fb);if(resolved==1)fbCache.emplace(fbKey,fb);}}
    return resolved;
}
void emitOutsideOp(VkCommandBuffer c,Op op,const std::uint8_t* payload,const FrameMaps& maps,const ForeignTargets& foreign){
    const Substitution sub{&maps};auto* p=const_cast<std::uint8_t*>(payload);
    auto isForeign=[&](VkImage i){return foreign.contains(i);};
    thread_local std::vector<VkImageMemoryBarrier> imageBarriers;
    switch(op){
            case Op::PipelineBarrier:{auto* r=reinterpret_cast<PBarrier*>(p);
                auto* pm=reinterpret_cast<VkMemoryBarrier*>(r+1);auto* pb=reinterpret_cast<VkBufferMemoryBarrier*>(pm+r->memory);auto* pi=reinterpret_cast<VkImageMemoryBarrier*>(pb+r->buffers);
                imageBarriers.clear();++stats.barrierCalls;
                for(std::uint32_t i=0;i<r->images;++i){bool m=false;const auto image=sub.image(pi[i].image,&m);if(pi[i].oldLayout==pi[i].newLayout)++stats.imageBarriersNoop;
                    if(!m){if(!isForeign(pi[i].image)){++stats.imageBarriersDropped;continue;}++stats.foreignBarriersKept;}auto b=pi[i];b.image=image;imageBarriers.push_back(b);++stats.imageBarriersKept;}
                if(!imageBarriers.empty()||r->memory||r->buffers)
                    raw.pipelineBarrier(c,r->src,r->dst,r->flags,r->memory,pm,r->buffers,pb,std::uint32_t(imageBarriers.size()),imageBarriers.data());
                else{++stats.barrierCallsDropped;if(!r->images)++stats.barrierExecOnlyDropped;} // Whole call (incl. its execution dependency) dropped
                break;}
            case Op::CopyImage:{auto* r=reinterpret_cast<PCopyImage*>(p);bool ms=false,md=false;const auto s=sub.image(r->src,&ms),d=sub.image(r->dst,&md);
                if(!md){if(!(ms&&isForeign(r->dst))){++stats.foreignCopiesSkipped;break;}++stats.foreignCopiesReplayed;}
                raw.copyImage(c,s,r->srcLayout,d,r->dstLayout,r->regions,reinterpret_cast<VkImageCopy*>(r+1));break;}
            case Op::CopyBuffer:{auto* r=reinterpret_cast<PCopyBuffer*>(p);raw.copyBuffer(c,r->src,r->dst,r->regions,reinterpret_cast<VkBufferCopy*>(r+1));break;}
            case Op::CopyBufferToImage:{auto* r=reinterpret_cast<PCopyBufferImage*>(p);bool md=false;const auto d=sub.image(r->image,&md);if(!md)break;
                raw.copyBufferToImage(c,r->buffer,d,r->layout,r->regions,reinterpret_cast<VkBufferImageCopy*>(r+1));break;}
            case Op::CopyImageToBuffer:break; // readbacks are never replayed
            case Op::BlitImage:{auto* r=reinterpret_cast<PBlit*>(p);bool ms=false,md=false;sub.image(r->src,&ms);const auto d=sub.image(r->dst,&md);
                if(!md){if(!(ms&&isForeign(r->dst))){++stats.foreignCopiesSkipped;break;}++stats.foreignCopiesReplayed;}
                raw.blitImage(c,sub.image(r->src),r->srcLayout,d,r->dstLayout,r->regions,reinterpret_cast<VkImageBlit*>(r+1),r->filter);break;}
            case Op::ClearColorImage:{auto* r=reinterpret_cast<PClearColor*>(p);bool m=false;const auto i=sub.image(r->image,&m);if(!m)break;
                raw.clearColorImage(c,i,r->layout,&r->value,r->ranges,reinterpret_cast<VkImageSubresourceRange*>(r+1));break;}
            case Op::ClearDepthStencilImage:{auto* r=reinterpret_cast<PClearDepth*>(p);bool m=false;const auto i=sub.image(r->image,&m);if(!m)break;
                raw.clearDepthStencilImage(c,i,r->layout,&r->value,r->ranges,reinterpret_cast<VkImageSubresourceRange*>(r+1));break;}
            case Op::ResolveImage:{auto* r=reinterpret_cast<PResolve*>(p);bool ms=false,md=false;sub.image(r->src,&ms);const auto d=sub.image(r->dst,&md);
                if(!md){if(!(ms&&isForeign(r->dst))){++stats.foreignCopiesSkipped;break;}++stats.foreignCopiesReplayed;}
                raw.resolveImage(c,sub.image(r->src),r->srcLayout,d,r->dstLayout,r->regions,reinterpret_cast<VkImageResolve*>(r+1));break;}
            case Op::FillBuffer:{auto* r=reinterpret_cast<PFill*>(p);raw.fillBuffer(c,r->buffer,r->offset,r->size,r->data);break;}
            case Op::UpdateBuffer:{auto* r=reinterpret_cast<PUpdate*>(p);raw.updateBuffer(c,r->buffer,r->offset,r->size,r+1);break;}
            default:break;
    }
}
void emitReplayHead(VkCommandBuffer c,Stage stage,const FrameMaps& maps,VkPipelineLayout layout,unsigned mask) noexcept {
        {VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;raw.pipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,nullptr,0,nullptr);++stats.startBarriers;}
        raw.pushConstants(c,layout,VK_SHADER_STAGE_VERTEX_BIT,0,64,maps.correction.data());
        if(stage==Stage::Color&&(mask&(1u<<25)))emitClearRight(c,maps,0,0.f,0.f,0.f);
        if(stage==Stage::Color&&(mask&(1u<<26)))emitClearRight(c,maps,3,0.f,0.f,0.f);
}
void emitReplayTail(VkCommandBuffer c,Stage stage,const FrameMaps& maps,VkPipelineLayout layout,unsigned mask,const BoundState& before,const Tape& t,bool dumpNow) noexcept {
        raw.pushConstants(c,layout,VK_SHADER_STAGE_VERTEX_BIT,0,64,identityMatrix);
        emitState(c,before,false);
        // Engine-tracked layouts of the right textures follow the stock
        // textures' layouts captured at the end of the left stage.
        for(unsigned k=0;k<maps.imageCount;++k){const auto& pair=maps.images[k];if(!pair.leftTexture||!pair.rightTexture||pair.role==4)continue; // Right SampledDepth: layout owned by emitDepthResolve (DS_RO, = real GPU layout); its left texture is the stock DEPTH texture
            for(unsigned i=0;i<t.stockLayoutCount;++i)if(t.stockLayouts[i].first==pair.leftTexture){
                std::memcpy(reinterpret_cast<void*>(pair.rightTexture+0xa0),&t.stockLayouts[i].second,sizeof(std::uint32_t));}}
        { // The native path closes the stage through the engine (EndRenderPass + AddImageBarriers). Make every
          // attachment write of the replayed passes available to later copies/samplers (eye capture, lighting).
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
            raw.pipelineBarrier(c,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&mb,0,nullptr,0,nullptr);++stats.endBarriers;}
        if(stage==Stage::Depth&&!(mask&(1u<<20)))emitDepthResolve(c,maps);
        if(dumpNow){try{
            VkPhysicalDevice phys{};{std::lock_guard lock(coldMutex);auto d=deviceRegistry.find(maps.device);if(d!=deviceRegistry.end())phys=d->second.physical;}
            std::vector<EyeDumpEntry> list;
            auto add=[&](const char* name,unsigned role,bool right,bool depthAspect){for(unsigned k=0;k<maps.imageCount;++k){const auto& pr=maps.images[k];if(pr.role!=role)continue;
                list.push_back({name,right?pr.right:pr.left,right?pr.rightTexture:pr.leftTexture,pr.format,pr.extent,depthAspect,0,0});return;}};
            add("R-color",0,true,false);add("R-light",3,true,false);add("R-collector",2,true,false);add("R-sampledDepth",4,true,true);add("R-depth",1,true,true);
            add("L-color",0,false,false);add("L-light",3,false,false);add("L-collector",2,false,false);add("L-sampledDepth",4,false,true);
            std::ostringstream m;auto arr=[&](const char* n,const float* v){m<<"\""<<n<<"\":[";for(unsigned i=0;i<16;++i)m<<(i?",":"")<<v[i];m<<"],";};
            arr("relClipL",maps.relClip[0].data());arr("relClipR",maps.relClip[1].data());arr("cameraL",maps.camera[0].data());arr("cameraR",maps.camera[1].data());arr("correction",maps.correction.data());m<<"\"mask\":"<<mask;
            auto trackedLayout=[](std::uintptr_t t)->VkImageLayout{std::uint32_t v{};if(t)std::memcpy(&v,reinterpret_cast<const void*>(t+0xa0),sizeof v);return static_cast<VkImageLayout>(v);};
            if(phys)eyeDump.record(c,maps.device,phys,list,m.str(),trackedLayout,raw.pipelineBarrier,raw.copyImageToBuffer);
        }catch(...){++eyeDump.failures;}}
        if(stage==Stage::Color&&(mask&(1u<<27)))emitClearRight(c,maps,0,1.f,0.f,1.f);
}
}

// ================================================================= replay
bool replay(Stage stage,VkCommandBuffer c,const FrameMaps& maps) noexcept {
    Errno preserve;faulted=false;
    if(stage==Stage::Depth)sdAliasClear(); // Set again only by this replay's tail (emitDepthResolve)
    if(gpuStagesArmed.load(std::memory_order_relaxed))gpuNoteEyeImages(maps); // DETAIL GPU pass classification (eye targets)
    if(maps.valid)noteCensusEyes(maps); // Census: eye images (render thread)
    auto& t=tapes[unsigned(stage)];
    auto decline=[&](const char* why){++stats.replaysDeclined;stats.lastDecline.store(why);return false;};
    if(!enabled())return decline("DISABLED");
    if(!maps.valid||!maps.device||!c)return decline("MAPS_INVALID");
    if(t.active||t.invalid||!t.commands)return decline(t.invalid?t.invalidReason:"TAPE_NOT_READY");
    if(t.command!=c)return decline("COMMAND_MISMATCH");
    const auto layout=cachedPushLayoutFor(maps.device);if(!layout)return decline("PUSH_LAYOUT");
    const auto began=nowNs();
    snapshotSetMap(maps.device,maps.owner&1u);stats.ownerLast.store(maps.owner&1u);probeMaps=&maps;
    // A frame whose jobs were launched in worker mode stays in worker mode (its ring region is shared) even if the
    // property changes mid-frame.
    const unsigned amode=frameJobsActive()?2u:asyncMode();
    if(amode==2u){ // debug.rdrvr.stereoreplay.async=2: secondaries recorded on the worker thread, spliced here
        switch(replayLaunched(stage,c,maps,layout,replaySetMap,t)){
        case SecondaryOutcome::Done:return true;
        case SecondaryOutcome::Faulted:
            faulted=true;++stats.replaysFaulted;stats.lastDecline.store("SECONDARY_FAULT");
            if(stats.replaysFaulted>=8&&!stats.faultDisabled){enabledValue.store(false);++stats.faultDisabled;}
            return false;
        case SecondaryOutcome::Declined:break; // nothing recorded into the primary: replay inline below (shared region)
        }
    }
    if(amode==1u){ // debug.rdrvr.stereoreplay.async=1: render-pass contents in secondary command buffers (StereoReplayAsync.cpp)
        switch(replaySecondaries(stage,c,maps,layout,replaySetMap,t)){
        case SecondaryOutcome::Done:return true;
        case SecondaryOutcome::Faulted:
            faulted=true;++stats.replaysFaulted;stats.lastDecline.store("SECONDARY_FAULT");
            if(stats.replaysFaulted>=8&&!stats.faultDisabled){enabledValue.store(false);++stats.faultDisabled;}
            return false;
        case SecondaryOutcome::Declined:break; // nothing recorded into the primary: replay inline below
        }
    }
    // Scratch images the LEFT stage filled from eye images (scene copies for refraction/post):
    // the right replay refills them from the right eye before its own consumer passes.
    const ForeignTargets foreign=scanForeignTargets(t,maps,stage);
    const unsigned mask=liveMask();stats.maskLast.store(mask);unsigned passIndex=0;
    const bool dumpNow=stage==Stage::Color&&eyeDump.poll();
    // ---- right-eye uniform rewrite (engine frame uniform ring) ----
    const RingSnapshot ring=snapshotRing(maps,mask);RewriteOptions options{};
    if(amode==2u)options.region=sharedRegionFor(ring); // worker-mode frame: allocate next to the jobs' blocks
    ReplayRewriter rw(stage,maps,mask,layout,&replaySetMap,ring,options);
    BoundState before{};if(auto* s=tracked(c,false))before=*s;
    bool emitted=false;unsigned skipDepth=0;bool inPass=false;unsigned passes=0,skipped=0;
    eyecull::tape::Pending ecRight{}; // EYE CULL B right drops, committed only when this replay completes
    // The replayed pass (prevDraws for the worker's split point; inline fallback of the EXTRA control; SPLIT is
    // not done inline, counted as inlineSkipped)
    const unsigned rightFlags=boundaryRightFlags();auto& bs=stats.boundary;
    if((rightFlags&BoundaryMergeR)&&stage==Stage::Color)mergeStatsValue.keptInline.fetch_add(1,std::memory_order_relaxed); // The inline path never merges
    struct OpenPass {VkRenderPass pass{};VkFramebuffer fb{};VkRect2D area{};unsigned index{UINT32_MAX},draws{};} open{};
    auto passEnded=[&](){
        if(open.index<boundary::prevDrawPasses)bs.prevDraws[(unsigned(stage)&1u)*boundary::prevDrawPasses+open.index].store(std::uint16_t(std::min(open.draws,65535u)),std::memory_order_relaxed);
        if(rightFlags&BoundaryExtraR){PassTwins tw{};
            if(!twinsFor(open.pass,tw)||!tw.empty)++bs.noTwins;else if(!(tw.flags&PassFinalIsReference))++bs.extraLayout;
            else{emitBoundaryBarrier(c);emitEmptyTwin(c,tw.empty,open.fb,open.area);++bs.extraR;bs.insertedThisFrame.fetch_add(1,std::memory_order_relaxed);}}
        if(rightFlags&BoundarySplitR)++bs.inlineSkipped;
        open=OpenPass{};};
    auto fault=[&](const char* why){
        faulted=emitted;++stats.replaysFaulted;stats.lastDecline.store(why);
        if(emitted){try{if(inPass)raw.endRenderPass(c);raw.pushConstants(c,layout,VK_SHADER_STAGE_VERTEX_BIT,0,64,identityMatrix);emitState(c,before,false);}catch(...){}}
        if(stats.replaysFaulted>=8&&!stats.faultDisabled){enabledValue.store(false);++stats.faultDisabled;}
        return false;};
    try{
        emitReplayHead(c,stage,maps,layout,mask);emitted=true;
        emitState(c,t.prologue,true);
        // Moon: viewport in effect for the right-eye scissor remap (StereoScissor.h); STEREO TEST 24 = verbatim
        VkViewport scissorViewport=t.prologue.viewport;bool scissorViewportKnown=t.prologue.viewportKnown;
        const bool scissorVerbatim=testMode.load(std::memory_order_relaxed)==testModeScissorAsLeft;
        std::size_t at=0;
        while(at<t.used){
            const auto* h=reinterpret_cast<const Header*>(t.bytes.data()+at);
            auto* p=const_cast<std::uint8_t*>(t.bytes.data()+at+sizeof(Header));at+=sizeof(Header)+h->bytes;
            const auto op=static_cast<Op>(h->op);
            if(skipDepth){
                if(op==Op::BeginRenderPass)++skipDepth;else if(op==Op::EndRenderPass)--skipDepth;
                continue;
            }
            switch(op){
            case Op::BindPipeline:{auto* r=reinterpret_cast<PBindPipeline*>(p);rw.bindPipeline(r->point,r->pipeline);raw.bindPipeline(c,r->point,r->pipeline);break;}
            case Op::BindDescriptorSets:rw.bindSets(c,reinterpret_cast<PBindSets*>(p));break;
            case Op::BindVertexBuffers:{auto* r=reinterpret_cast<PBindVertices*>(p);auto* buffers=reinterpret_cast<VkBuffer*>(r+1);auto* offsets=reinterpret_cast<VkDeviceSize*>(buffers+r->count);raw.bindVertexBuffers(c,r->first,r->count,buffers,offsets);break;}
            case Op::BindIndexBuffer:{auto* r=reinterpret_cast<PBindIndex*>(p);raw.bindIndexBuffer(c,r->buffer,r->offset,r->type);break;}
            case Op::SetViewport:{auto* r=reinterpret_cast<PViewport*>(p);raw.setViewport(c,r->first,r->count,reinterpret_cast<VkViewport*>(r+1));
                if(r->first==0&&r->count>=1){scissorViewport=reinterpret_cast<VkViewport*>(r+1)[0];scissorViewportKnown=true;}break;}
            case Op::SetScissor:{auto* r=reinterpret_cast<PScissor*>(p);auto* v=reinterpret_cast<VkRect2D*>(r+1);
                if(r->first==0&&r->count==1){const VkRect2D right=scissor::forRight(v[0],scissorViewportKnown,scissorViewport,maps.correction.data(),scissorVerbatim);raw.setScissor(c,0,1,&right);}
                else raw.setScissor(c,r->first,r->count,v);break;}
            case Op::SetLineWidth:{auto* r=reinterpret_cast<PFloat1*>(p);raw.setLineWidth(c,r->a);break;}
            case Op::SetDepthBias:{auto* r=reinterpret_cast<PFloat3*>(p);raw.setDepthBias(c,r->a,r->b,r->c);break;}
            case Op::SetBlendConstants:{auto* r=reinterpret_cast<PFloat4*>(p);raw.setBlendConstants(c,r->v);break;}
            case Op::SetDepthBounds:{auto* r=reinterpret_cast<PFloat2*>(p);raw.setDepthBounds(c,r->a,r->b);break;}
            case Op::SetStencilCompareMask:{auto* r=reinterpret_cast<PStencil*>(p);raw.setStencilCompareMask(c,r->face,r->value);break;}
            case Op::SetStencilWriteMask:{auto* r=reinterpret_cast<PStencil*>(p);raw.setStencilWriteMask(c,r->face,r->value);break;}
            case Op::SetStencilReference:{auto* r=reinterpret_cast<PStencil*>(p);raw.setStencilReference(c,r->face,r->value);break;}
            case Op::PushConstants:{auto* r=reinterpret_cast<PPush*>(p);raw.pushConstants(c,r->layout,r->stages,r->offset,r->size,r+1);break;}
            case Op::BeginRenderPass:{auto* r=reinterpret_cast<PBeginPass*>(p);VkFramebuffer fb{};
                const unsigned thisPass=passIndex++;
                if(stage==Stage::Color&&thisPass<16&&(mask&(1u<<thisPass))){skipDepth=1;++stats.maskPassesSkipped;break;}
                const int resolved=resolveRightFramebuffer(maps,r->pass,r->framebuffer,fb);
                if(resolved==0){skipDepth=1;++skipped;++stats.passesSkipped;break;}
                if(resolved!=1)return fault("FRAMEBUFFER_UNRESOLVED");
                VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=r->pass;b.framebuffer=fb;b.renderArea=r->area;b.clearValueCount=r->clears;b.pClearValues=reinterpret_cast<VkClearValue*>(r+1);
                raw.beginRenderPass(c,&b,r->contents);inPass=true;++passes;++stats.passesReplayed;open={r->pass,fb,r->area,thisPass,0};break;}
            case Op::EndRenderPass:{if(!inPass){++stats.endsSkipped;break;}raw.endRenderPass(c);inPass=false;passEnded();break;}
            case Op::NextSubpass:{if(!inPass){++stats.drawsSkippedOutsidePass;break;}auto* r=reinterpret_cast<PSubpass*>(p);raw.nextSubpass(c,r->contents);break;}
            case Op::Draw:{if(!inPass){++stats.drawsSkippedOutsidePass;break;}if(mask&(1u<<16)){++stats.maskDrawsSkipped;break;}if(eyecull::tape::dropRight(h->pad,ecRight))break;
                if(rw.classifyDraw(passIndex)){++stats.maskDrawsSkipped;break;}rw.rewriteForDraw(c);auto* r=reinterpret_cast<PDraw*>(p);raw.draw(c,r->vertices,r->instances,r->firstVertex,r->firstInstance);++open.draws;break;}
            case Op::DrawIndexed:{if(!inPass){++stats.drawsSkippedOutsidePass;break;}if((mask&(1u<<17))&&stage==Stage::Color&&passIndex>1){++stats.maskDrawsSkipped;break;}if(eyecull::tape::dropRight(h->pad,ecRight))break;
                if(rw.classifyDraw(passIndex)){++stats.maskDrawsSkipped;break;}rw.rewriteForDraw(c);auto* r=reinterpret_cast<PDrawIndexed*>(p);raw.drawIndexed(c,r->indices,r->instances,r->firstIndex,r->vertexOffset,r->firstInstance);++open.draws;break;}
            case Op::DrawIndirect:{if(!inPass){++stats.drawsSkippedOutsidePass;break;}if(rw.classifyDraw(passIndex)){++stats.maskDrawsSkipped;break;}rw.rewriteForDraw(c);auto* r=reinterpret_cast<PIndirect*>(p);raw.drawIndirect(c,r->buffer,r->offset,r->count,r->stride);++open.draws;break;}
            case Op::DrawIndexedIndirect:{if(!inPass){++stats.drawsSkippedOutsidePass;break;}if(rw.classifyDraw(passIndex)){++stats.maskDrawsSkipped;break;}rw.rewriteForDraw(c);auto* r=reinterpret_cast<PIndirect*>(p);raw.drawIndexedIndirect(c,r->buffer,r->offset,r->count,r->stride);++open.draws;break;}
            case Op::ClearAttachments:{if(!inPass){++stats.drawsSkippedOutsidePass;break;}auto* r=reinterpret_cast<PClearAttachments*>(p);auto* pa=reinterpret_cast<VkClearAttachment*>(r+1);
                raw.clearAttachments(c,r->attachments,pa,r->rects,reinterpret_cast<VkClearRect*>(pa+r->attachments));break;}
            case Op::PipelineBarrier:case Op::CopyImage:case Op::CopyBuffer:case Op::CopyBufferToImage:case Op::CopyImageToBuffer:case Op::BlitImage:
            case Op::ClearColorImage:case Op::ClearDepthStencilImage:case Op::ResolveImage:case Op::FillBuffer:case Op::UpdateBuffer:
                emitOutsideOp(c,op,p,maps,foreign);break;
            default:return fault("UNKNOWN_OP");
            }
        }
        if(inPass){raw.endRenderPass(c);inPass=false;++stats.passesAutoClosed;passEnded();}
        emitReplayTail(c,stage,maps,layout,mask,before,t,dumpNow);
        rw.finish();
        faulted=false;++stats.replays;stats.replayNs[unsigned(stage)].store(nowNs()-began);stats.replayCommands[unsigned(stage)].store(t.commands);
        eyecull::tape::commitRight(ecRight);
        (void)passes;(void)skipped;
        return true;
    }catch(...){return fault("REPLAY_EXCEPTION");}
}
bool applyBindlessOverrides(VkDevice d,const BindlessOverride* overrides,unsigned count,unsigned owner) noexcept {
    Errno preserve;if(!enabled()||!d)return false;
    if(!overrides)count=0; // count 0 = restore only
    try{
        thread_local std::vector<VkCopyDescriptorSet> copies;copies.clear();
        thread_local std::vector<SetRecord::OverrideEntry> fresh;
        thread_local DummyWrites dw;dw.clear(); // Revert of a dead engine element
        const unsigned o=owner&1u;const bool act=scrubOn();const std::uint64_t frame=scrubFrame.load(std::memory_order_relaxed);
        std::size_t overrideCopies=0;
        std::lock_guard lock(coldMutex);
        t2.latestCount=0; // This call's copy sources (frozen as lastCopySources when a flag first appears)
        for(auto& [set,record]:setRegistry){if(record.device!=d)continue;
            fresh.clear();const VkDescriptorSet dst=o?record.clone2:record.clone;
            for(unsigned i=0;i<count;++i){if(overrides[i].leftIndex==overrides[i].rightIndex)continue;
                const auto l=overrides[i].leftIndex,r=overrides[i].rightIndex;bool any=false,range=false,unwritten=false,stale=false;
                for(std::uint32_t b=0;b<8;++b){const auto n=record.bindingCounts[b];if(!n)continue;
                    if(l>=n||r>=n){range=true;continue;}
                    const auto& bw=record.bindingWritten[b];if(!bw[l]||!bw[r]){unwritten=true;continue;}
                    // Copy only a LIVE view of the pair's right image. The driver's copy path ORs
                    // the companion flag into the DESTINATION from the SOURCE entry's view pointer
                    // (0x23f28c-0x23f29c), so a dead source (the previous occupant of index r:
                    // FreeDescriptor never clears the slot and the right texture's descriptor is
                    // written only at the next BeginFrame) whose freed memory has a non-null +0x1b8
                    // flags the clone for its whole lifetime. A live view of another image (a
                    // reused handle, or binding 3 / cube content at r) is not this pair's texture
                    // either. No copy: clone[l] keeps the engine's own element l (the LEFT texture
                    // for one frame; the revert below restores it if an older override sits there).
                    // A steady per-frame rise of overridePairsStale = a pair refused every frame =
                    // the right eye samples the LEFT texture (validate=0 copies without this
                    // check). A caller without rightImage keeps copying unvalidated, except that a
                    // KNOWN dead view is never copied.
                    const VkImageView source=r<record.elementView[b].size()?record.elementView[b][r]:VK_NULL_HANDLE;
                    const bool live=liveView(source),strict=overrides[i].rightImage!=VK_NULL_HANDLE;
                    const bool matches=!strict||(r<record.elementImage[b].size()&&record.elementImage[b][r]==overrides[i].rightImage);
                    const bool copyOk=strict?live&&matches:(!source||live);
                    const bool validate=validateOn();
                    noteCopySource(o,0,b,l,r,source,live,matches,copyOk||!validate,frame);
                    if(!copyOk){if(!source)++t2.overridesNoSource;else if(!live)++t2.overridesStaleSource;else ++t2.overridesOtherImage;
                        if(validate){stale=true;continue;}
                        ++t2.unvalidatedCopies;} // validate=0: copied without validation (diagnostic)
                    if(!source)++stats.scrub.entriesUnknownSource; // (legacy caller only)
                    VkCopyDescriptorSet cp{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};cp.srcSet=set;cp.srcBinding=b;cp.srcArrayElement=r;
                    cp.dstSet=dst;cp.dstBinding=b;cp.dstArrayElement=l;cp.descriptorCount=1;copies.push_back(cp);any=true;++overrideCopies;
                    // The slot now holds the engine's element r (its view: the source a later destroy is matched against)
                    bool replaced=false;for(auto& f:fresh)if(f.binding==b&&f.element==l){noteRecentSource(f.source,frame);f.source=source;replaced=true;break;} // a later pair wins (copy order)
                    if(!replaced){fresh.push_back({b,l,source,frame});++stats.scrub.overrideEntries;}}
                if(!any){if(range){++stats.overridesOutOfRange;stats.lastBadOverride.store((std::uint64_t(l)<<32)|r);}
                    if(unwritten){++stats.overridesUnwritten;stats.lastUnwrittenOverride.store((std::uint64_t(l)<<32)|r);}
                    if(stale)++t2.overridePairsStale;}}
            // (2) revert: a slot of this owner overridden earlier and not overridden again now gets the engine's own
            // element back (skipped pair, unwritten / out of range, count 0, mask bit 21), so clone[l] never keeps an old right
            // view. The old source stays "recent" (command buffers recorded before may still hold it).
            auto& live=record.overrides[o];
            for(const auto& e:live){
                bool again=false;for(const auto& f:fresh)if(f.binding==e.binding&&f.element==e.element){again=true;if(f.source!=e.source)noteRecentSource(e.source,frame);break;}
                if(again)continue;
                noteRecentSource(e.source,frame);
                if(!act){++stats.scrub.countOnlyReverts;fresh.push_back(e);continue;} // Count only: the slot keeps the old copy
                const auto& bw=record.bindingWritten[e.binding];
                if(e.element>=bw.size()||!bw[e.element]){++stats.scrub.revertsUnwritten;fresh.push_back(e);continue;} // no engine element to copy back: keep tracking the slot (scrub)
                // The revert copies engine[l] only when that is a live view; a dead one is never
                // copied - the dummy is written instead (the slot then holds no override source: entry dropped). Without a dummy
                // the slot keeps its old copy and stays tracked (like revertsUnwritten), so a later destroy still scrubs it.
                {const VkImageView src=e.element<record.elementView[e.binding].size()?record.elementView[e.binding][e.element]:VK_NULL_HANDLE;
                 const bool srcLive=liveView(src);noteCopySource(o,1,e.binding,e.element,e.element,src,srcLive,true,srcLive,frame);
                 if(!srcLive){++t2.revertsStaleSource;
                     if(!updateAfterBind(record,e.binding)){++t2.dummyRefusedFlags;fresh.push_back(e);} // Stays tracked, as without a dummy
                     else if(dw.add(dst,e.binding,e.element,record.bindingTypes[e.binding]))++t2.dummyWrites;else{++t2.dummyMissing;fresh.push_back(e);}
                     continue;}}
                VkCopyDescriptorSet cp{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};cp.srcSet=set;cp.srcBinding=e.binding;cp.srcArrayElement=e.element;
                cp.dstSet=dst;cp.dstBinding=e.binding;cp.dstArrayElement=e.element;cp.descriptorCount=1;copies.push_back(cp);++stats.scrub.overrideReverts;}
            live.assign(fresh.begin(),fresh.end());
        }
        // Issued under coldMutex, serialized with the destroy-time scrub (both update the clone sets);
        // the dummy writes target revert slots only, which no copy of this call reads or writes (writes run before copies).
        if(!copies.empty()||!dw.writes.empty())raw.updateDescriptorSets(d,std::uint32_t(dw.writes.size()),dw.data(),std::uint32_t(copies.size()),copies.data());
        if(!overrideCopies)return false;
        ++stats.overrideCalls;return true;
    }catch(...){return false;}
}
unsigned bindlessClones() noexcept {std::lock_guard lock(coldMutex);return unsigned(setRegistry.size());}
// ================================================================= public part (see StereoReplay.h)
void setSafeReader(SafeReader reader,void* context) noexcept {safeReaderContext.store(context,std::memory_order_relaxed);safeReader.store(reader,std::memory_order_release);}
bool cloneFlagged(VkDevice d,unsigned owner,std::uint64_t serial,bool launch) noexcept {
    Errno preserve;if(!d||!setprobeOn()||!enabled())return false;
    ensureIdentity(d);if(!driverArmed())return false;
    try{const unsigned o=owner&1u;const std::uint64_t frame=scrubFrame.load(std::memory_order_relaxed);bool flagged=false,rebuiltAtStage=false;
        std::lock_guard lock(coldMutex);
        for(auto& [set,record]:setRegistry){if(record.device!=d)continue;armRecord(set,record);
            auto& p=record.probe;if(!p.armed[1+o])continue;
            const VkDescriptorSet s=o?record.clone2:record.clone;
            if(!(liveFlag(s)&drv::companionBit))continue;
            p.flag[1+o]=1;noteFlag(1+o,frame);noteAnyFlag(frame);
            // An engine set flagged as well (a mirrored engine write of a companion view) costs the native path the same per
            // draw and gives it the same End list: neither containment nor a rebuild (which would re-write that view) helps.
            if(p.armed[0]&&(liveFlag(set)&drv::companionBit)){p.flag[0]=1;noteFlag(0,frame);++t2.engineFlaggedChecks;continue;}
            flagged=true;
            if(rebuildOn()&&rebuildClone(set,record,o,frame)&&!launch)rebuiltAtStage=true;}
        // Containment is keyed by the frame serial (the launch gate runs before frameHook, so scrubFrame differs
        // between a frame's launches and its stages; the serial does not). A clone rebuilt within this frame lacks this frame's
        // override copies (stereoPrepareFrame made them into the old set), and a job launched earlier in the frame bound the old
        // flagged set: the owner stays contained for the whole frame - launches and stages (the stage gate cancels
        // the launched jobs). The next frame (a new serial) uses the rebuilt clone.
        const bool contained=t2.containedSerial[o]==serial;
        if(!flagged&&!contained)return false;
        // contain=0 still contains the frame of a stage-gate rebuild (its new set has no override copies yet)
        if(!containOn()&&!rebuiltAtStage&&!contained){++t2.containOff[o];return false;}
        if(!containOn()&&rebuiltAtStage)++t2.rebuildContainedStages;
        if(!contained){t2.containedSerial[o]=serial;++t2.cloneFlaggedFrames[o];}
        if(!launch)++t2.stageFallbacks[o];
        else if(t2.launchRefusedSerial[o]!=serial){t2.launchRefusedSerial[o]=serial;++t2.launchRefused[o];} // frames, not attempts
        return true;
    }catch(...){return false;}
}
bool dummyDescriptorWanted() noexcept {return !dummyReady.load(std::memory_order_acquire)&&!dummyFailed.load(std::memory_order_acquire);}
void setDummyDescriptor(VkImageView view,VkSampler sampler,VkImageLayout layout) noexcept {
    try{std::lock_guard lock(coldMutex);dummyReady.store(false,std::memory_order_release);dummy=Dummy{};dummy.view=view;dummy.sampler=sampler;dummy.layout=layout;dummy.status=view?"SET":"FAILED";
        dummyFailed.store(!view,std::memory_order_relaxed);dummyReady.store(view!=VK_NULL_HANDLE,std::memory_order_release);}catch(...){}
}
// One-time creation through the loader exports (not the engine's hooked slots: the view never enters viewRegistry, the image
// never enters passResources). A 1x1 RGBA8 OPTIMAL image, usage SAMPLED|TRANSFER_DST only (no attachment usage, so the Adreno
// driver allocates no companion image for it and its view has [+0x1b8] == 0), cleared to 0 and moved to
// SHADER_READ_ONLY_OPTIMAL by one fenced submission on the XR queue under its mutex (the scheme of the VRS / FDM maps); the
// descriptors written with it name SHADER_READ_ONLY_OPTIMAL, the layout it keeps forever. Destroyed by shutdownAfterIdle
// (deviceDestroyed only forgets it). A failure is final for the session (driverSets.dummy).
void ensureDummyDescriptor(VkDevice d,std::uint32_t family,std::uint32_t queueIndex,std::mutex* queueMutex) noexcept {
    Errno preserve;if(!d||!queueMutex||!dummyDescriptorWanted())return;
    VkPhysicalDevice physical{};try{std::lock_guard lock(coldMutex);auto it=deviceRegistry.find(d);if(it!=deviceRegistry.end())physical=it->second.physical;}catch(...){return;}
    VkImage image{};VkDeviceMemory memory{};VkImageView view{};VkSampler sampler{};VkCommandPool pool{};VkFence fence{};
    auto fail=[&](const char* why,bool gpuMayUse){
        if(!gpuMayUse){if(view)vkDestroyImageView(d,view,nullptr);if(sampler)vkDestroySampler(d,sampler,nullptr);if(image)vkDestroyImage(d,image,nullptr);
            if(memory)vkFreeMemory(d,memory,nullptr);if(fence)vkDestroyFence(d,fence,nullptr);if(pool)vkDestroyCommandPool(d,pool,nullptr);} // else leaked: the GPU may still touch them
        try{std::lock_guard lock(coldMutex);dummy.status=why;}catch(...){}
        dummyFailed.store(true,std::memory_order_release);
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR,"RdrVrStereo","dummy descriptor view unavailable: %s",why);
#endif
    };
    try{
        if(!physical)return fail("NO_PHYSICAL_DEVICE",false);
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;ci.extent={1,1,1};ci.mipLevels=1;ci.arrayLayers=1;
        ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(d,&ci,nullptr,&image)!=VK_SUCCESS||!image){image={};return fail("CREATE_IMAGE",false);}
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(d,image,&req);
        VkPhysicalDeviceMemoryProperties mem{};vkGetPhysicalDeviceMemoryProperties(physical,&mem);
        std::uint32_t type=mem.memoryTypeCount; // device-local first, then any allowed type
        for(unsigned pass=0;pass<2&&type==mem.memoryTypeCount;++pass)
            for(std::uint32_t i=0;i<mem.memoryTypeCount&&i<VK_MAX_MEMORY_TYPES;++i)
                if((req.memoryTypeBits&(1u<<i))&&(pass||(mem.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))){type=i;break;}
        if(!req.size||req.size>(1u<<20)||type==mem.memoryTypeCount)return fail("MEMORY_TYPE",false);
        VkMemoryAllocateInfo mi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};mi.allocationSize=req.size;mi.memoryTypeIndex=type;
        if(vkAllocateMemory(d,&mi,nullptr,&memory)!=VK_SUCCESS||!memory){memory={};return fail("ALLOCATE_MEMORY",false);}
        if(vkBindImageMemory(d,image,memory,0)!=VK_SUCCESS)return fail("BIND_MEMORY",false);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;pi.queueFamilyIndex=family;
        if(vkCreateCommandPool(d,&pi,nullptr,&pool)!=VK_SUCCESS||!pool){pool={};return fail("COMMAND_POOL",false);}
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
        VkCommandBuffer cmd{};if(vkAllocateCommandBuffers(d,&cai,&cmd)!=VK_SUCCESS||!cmd)return fail("COMMAND_BUFFER",false);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if(vkBeginCommandBuffer(cmd,&bi)!=VK_SUCCESS)return fail("COMMAND_BEGIN",false);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.srcAccessMask=0;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        const VkClearColorValue zero{};vkCmdClearColorImage(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&b.subresourceRange);
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
        if(vkEndCommandBuffer(cmd)!=VK_SUCCESS)return fail("COMMAND_END",false);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if(vkCreateFence(d,&fi,nullptr,&fence)!=VK_SUCCESS||!fence){fence={};return fail("FENCE",false);}
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;VkResult waited=VK_ERROR_UNKNOWN;
        {std::lock_guard guard(*queueMutex);VkQueue queue{};vkGetDeviceQueue(d,family,queueIndex,&queue);
            if(!queue||vkQueueSubmit(queue,1,&si,fence)!=VK_SUCCESS)return fail("SUBMIT",false);}
        waited=vkWaitForFences(d,1,&fence,VK_TRUE,2000000000ull); // Outside the queue mutex (the XR loop takes it per frame)
        if(waited!=VK_SUCCESS)return fail("FENCE_WAIT",true);
        vkDestroyFence(d,fence,nullptr);fence={};vkDestroyCommandPool(d,pool,nullptr);pool={};
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=VK_FORMAT_R8G8B8A8_UNORM;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(vkCreateImageView(d,&vi,nullptr,&view)!=VK_SUCCESS||!view){view={};return fail("CREATE_VIEW",false);}
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sci.magFilter=sci.minFilter=VK_FILTER_NEAREST;sci.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sci.maxLod=0.f;
        if(vkCreateSampler(d,&sci,nullptr,&sampler)!=VK_SUCCESS||!sampler){sampler={};return fail("CREATE_SAMPLER",false);}
        std::lock_guard lock(coldMutex);dummy=Dummy{d,view,sampler,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,image,memory,"READY"};dummyReady.store(true,std::memory_order_release);
    }catch(...){fail("EXCEPTION",true);}
}
void setDriverProbeForTests(std::uintptr_t setVtable,int setprobe,int contain,int rebuild,int enginescrub,int validate) noexcept {
    try{std::lock_guard g(identityMutex);t2.testVtable.store(setVtable,std::memory_order_relaxed);t2.identity.store(0,std::memory_order_release);t2.why="NOT_TRIED";}catch(...){}
    t2.forcedValidate.store(validate,std::memory_order_relaxed);
    t2.forcedSetprobe.store(setprobe,std::memory_order_relaxed);t2.forcedContain.store(contain,std::memory_order_relaxed);
    t2.forcedRebuild.store(rebuild,std::memory_order_relaxed);t2.forcedEngineScrub.store(enginescrub,std::memory_order_relaxed);
    try{std::lock_guard lock(coldMutex);t2.firstFlagSet=-1;t2.firstFlagOwner=-1;t2.firstFlagFrame=0;t2.firstCount=0;
        t2.containedSerial={UINT64_MAX,UINT64_MAX};t2.launchRefusedSerial={UINT64_MAX,UINT64_MAX};t2.lastAnyFlagFrame=0;
        for(auto& entry:setRegistry){entry.second.probe.checked={};entry.second.probe.unread={};}
        t2.burstFrames.store(0,std::memory_order_relaxed);t2.burstBefore.store(0,std::memory_order_relaxed);for(auto& b:t2.burst)b.store(0,std::memory_order_relaxed);}catch(...){} // P3 restarts
}
// Report stereoReplay.driverSets: the probe of the largest bindless set (the 4 x 16384 g_textures
// layout) and every counter of (a)-(e).
std::string driverSetsReport(){
    std::ostringstream o;
    try{auto hex=[](std::uint64_t v){std::ostringstream h;h<<"\"0x"<<std::hex<<v<<"\"";return h.str();};
        std::lock_guard lock(coldMutex);
        const SetRecord* best=nullptr;VkDescriptorSet bestSet{};std::uint64_t bestSize=0;unsigned flagged[3]{};
        for(const auto& [set,r]:setRegistry){std::uint64_t n=0;for(auto c:r.bindingCounts)n+=c;if(!best||n>bestSize){best=&r;bestSet=set;bestSize=n;}
            for(unsigned k=0;k<3;++k)flagged[k]+=r.probe.flag[k];}
        const char* names[3]{"engine","clone","clone2"};
        o<<"{\"setprobe\":"<<(setprobeOn()?1:0)<<",\"contain\":"<<(containOn()?1:0)<<",\"clonerebuild\":"<<(rebuildOn()?1:0)<<",\"enginescrub\":"<<(engineScrubOn()?1:0) // effective properties
         <<",\"validate\":"<<(validateOn()?1:0)
         <<",\"identity\":\""<<t2.why.load()<<"\",\"dispatchModule\":\""<<(t2.identity.load(std::memory_order_acquire)?t2.dispatchModule:"")<<"\",\"icd\":"<<(t2.identity.load(std::memory_order_acquire)?t2.dispatchIcd:-1)<<",\"driverBase\":"<<hex(t2.identity.load(std::memory_order_acquire)?t2.base:0) // Written before identity (release)
         <<",\"dummy\":\""<<dummy.status<<"\",\"sets\":"<<setRegistry.size()<<",\"flaggedSets\":["<<flagged[0]<<","<<flagged[1]<<","<<flagged[2]<<"]";
        for(unsigned k=0;k<3;++k){o<<",\""<<names[k]<<"\":{";
            if(best){const auto& p=best->probe;
                o<<"\"flag\":"<<unsigned(p.flag[k])<<",\"count\":"<<p.count[k]<<",\"vtableOk\":"<<(p.vtableOk[k]?1:0)<<",\"armed\":"<<(p.armed[k]&&driverArmed()?1:0)
                 <<",\"checked\":"<<(p.checked[k]?1:0)<<",\"vtable\":"<<hex(t2.base&&p.vtable[k]>=t2.base?p.vtable[k]-t2.base:p.vtable[k])<<",\"flaggedFrames\":"<<p.flaggedFrames[k]
                 <<",\"handle\":"<<hex(reinterpret_cast<std::uintptr_t>(probeSet(bestSet,*best,k)));
                if(k)o<<",\"allocFlag\":"<<p.allocFlag[k-1]<<",\"rebuilds\":"<<p.rebuilds[k-1]<<",\"ownPool\":"<<(best->ownPool[k-1]?1:0);}
            o<<"}";}
        o<<",\"bindingFlags\":[";
        if(best){auto L=layoutRegistry.find(best->layout);
            if(L!=layoutRegistry.end())for(std::size_t k=0;k<L->second.bindings.size();++k){const auto& b=L->second.bindings[k];
                o<<(k?",":"")<<"["<<b.binding<<","<<unsigned(b.descriptorType)<<","<<b.descriptorCount<<","<<(k<L->second.bindingFlags.size()?L->second.bindingFlags[k]:0)<<"]";}}
        o<<"],\"layoutFlags\":"<<[&]{if(!best)return 0u;auto L=layoutRegistry.find(best->layout);return L==layoutRegistry.end()?0u:unsigned(L->second.flags);}()
         <<",\"untracked\":"<<(best&&best->untracked?1:0)
         <<",\"firstFlagFrame\":"<<(t2.firstFlagSet>=0?t2.firstFlagFrame:0)<<",\"firstFlagOwner\":"<<t2.firstFlagOwner<<",\"firstFlagSet\":\""<<(t2.firstFlagSet>=0?names[t2.firstFlagSet]:"NONE")<<"\"";
        auto list=[&](const std::array<CopySource,24>& a,unsigned n){o<<"[";
            for(unsigned i=0;i<n&&i<a.size();++i){const auto& c=a[i];
                o<<(i?",":"")<<"{\"owner\":"<<unsigned(c.owner)<<",\"kind\":\""<<copyKindName[c.kind<3?c.kind:0]<<"\",\"binding\":"<<c.binding<<",\"l\":"<<c.l<<",\"r\":"<<c.r
                 <<",\"sourceView\":"<<hex(reinterpret_cast<std::uintptr_t>(c.view))<<",\"live\":"<<(c.live?1:0)<<",\"companion\":"<<(c.companionRead?hex(c.companion):std::string("null"))
                 <<",\"imageMatches\":"<<(c.imageMatches?1:0)<<",\"copied\":"<<(c.copied?1:0)<<",\"frame\":"<<c.frame<<"}";}
            o<<"]";};
        o<<",\"lastCopySources\":";if(t2.firstFlagSet>=0)list(t2.first,t2.firstCount);else list(t2.latest,t2.latestCount);
        o<<",\"latestCopySources\":";list(t2.latest,t2.latestCount);
        o<<",\"probeFrames\":"<<t2.probeFrames<<",\"selfCheckRefused\":"<<t2.selfCheckRefused<<",\"selfCheckUnread\":"<<t2.selfCheckUnread<<",\"copySourcesDropped\":"<<t2.copySourcesDropped
         // (b) source validation
         <<",\"overridesStaleSource\":"<<t2.overridesStaleSource<<",\"overridesOtherImage\":"<<t2.overridesOtherImage<<",\"overridesNoSource\":"<<t2.overridesNoSource
         <<",\"overridePairsStale\":"<<t2.overridePairsStale<<",\"revertsStaleSource\":"<<t2.revertsStaleSource<<",\"scrubsStaleSource\":"<<t2.scrubsStaleSource
         <<",\"dummyWrites\":"<<t2.dummyWrites<<",\"dummyMissing\":"<<t2.dummyMissing<<",\"dummyRefusedFlags\":"<<t2.dummyRefusedFlags<<",\"dummyDestroyed\":"<<t2.dummyDestroyed<<",\"unvalidatedCopies\":"<<t2.unvalidatedCopies
         // (c) containment
         <<",\"cloneFlaggedFrames\":["<<t2.cloneFlaggedFrames[0]<<","<<t2.cloneFlaggedFrames[1]<<"],\"launchRefused\":["<<t2.launchRefused[0]<<","<<t2.launchRefused[1]
         <<"],\"stageFallbacks\":["<<t2.stageFallbacks[0]<<","<<t2.stageFallbacks[1]<<"],\"containOff\":["<<t2.containOff[0]<<","<<t2.containOff[1]<<"],\"engineFlaggedChecks\":"<<t2.engineFlaggedChecks
         <<",\"rebuildContainedStages\":"<<t2.rebuildContainedStages<<",\"lastAnyFlagFrame\":"<<t2.lastAnyFlagFrame
         // (d) recovery
         <<",\"cloneRebuilds\":["<<t2.cloneRebuilds[0]<<","<<t2.cloneRebuilds[1]<<"],\"cloneRebuildFrames\":[";
        {const unsigned n=std::min<unsigned>(t2.rebuildFrameCount,unsigned(t2.rebuildFrames.size()));const unsigned start=t2.rebuildFrameCount-n;
         for(unsigned i=0;i<n;++i)o<<(i?",":"")<<t2.rebuildFrames[(start+i)%t2.rebuildFrames.size()];}
        o<<"],\"rebuildReflagged\":"<<t2.rebuildReflagged<<",\"rebuildRefused\":"<<t2.rebuildRefused<<",\"lastRebuildRefusal\":\""<<t2.lastRebuildRefusal<<"\",\"rebuildCapped\":"<<t2.rebuildCapped
         <<",\"rebuildSpaced\":"<<t2.rebuildSpaced<<",\"rebuildFailed\":"<<t2.rebuildFailed<<",\"rebuildWrites\":"<<t2.rebuildWrites<<",\"rebuildDeadSkipped\":"<<t2.rebuildDeadSkipped<<",\"rebuildLayoutSkipped\":"<<t2.rebuildLayoutSkipped
         <<",\"pendingSets\":"<<deferredSets.size()<<",\"setReleases\":"<<t2.setReleases<<",\"teardownSetReleases\":"<<t2.teardownSetReleases
         // (e) probes and the engine scrub
         <<",\"engineScrub\":{\"companionViews\":"<<t2.staleEngineCompanionViews<<",\"companionUnread\":"<<t2.companionUnread<<",\"slots\":"<<t2.engineScrubSlots<<",\"writes\":"<<t2.engineScrubWrites
         <<",\"refusedFlags\":"<<t2.engineScrubRefusedFlags<<",\"refusedNoDummy\":"<<t2.engineScrubRefusedNoDummy<<",\"deferredViews\":"<<t2.engineScrubDeferredViews<<",\"deferredImages\":"<<t2.engineScrubDeferredImages
         <<",\"destroysDuringPrimaryEnd\":{\"companionViews\":"<<t2.endOverlapViews[0]<<",\"otherViews\":"<<t2.endOverlapViews[1]<<",\"images\":"<<t2.endOverlapImages<<"}"
         <<",\"reusedStaleHandles\":"<<t2.reusedStaleHandles<<",\"staleHeld\":"<<staleHolders.size()<<",\"staleHeldResets\":"<<t2.staleHoldersReset
         <<",\"burstFirstFrame\":"<<t2.burstFirstFrame<<",\"burstFrames\":"<<std::min<unsigned>(t2.burstFrames.load(),unsigned(t2.burst.size()))<<",\"burstBefore\":"<<t2.burstBefore<<",\"burst\":[";
        for(unsigned i=0;i<t2.burst.size();++i)o<<(i?",":"")<<t2.burst[i].load(std::memory_order_relaxed);
        o<<"],\"unflagged\":"<<t2.engineScrubUnflagged<<",\"companionVerified\":0}}";
    }catch(...){return "{}";}
    return o.str();
}
std::string setReport(){
    std::ostringstream o;o<<"[";bool first=true;
    try{std::lock_guard lock(coldMutex);
        for(const auto& [set,r]:setRegistry){o<<(first?"":",")<<"{\"binding\":"<<r.textureBinding<<",\"layoutCount\":"<<r.layoutCount<<",\"variableCount\":"<<r.variableCount<<",\"textureCount\":"<<r.textureCount<<",\"writtenCount\":"<<r.writtenCount<<",\"maxWritten\":"<<r.maxWritten;
            o<<",\"writesPerBinding\":[";for(unsigned k=0;k<8;++k)o<<(k?",":"")<<r.writesPerBinding[k];o<<"]";
            o<<",\"bindingCounts\":[";for(unsigned k=0;k<8;++k)o<<(k?",":"")<<r.bindingCounts[k];o<<"]";
            o<<",\"bindingWritten\":[";for(unsigned k=0;k<8;++k)o<<(k?",":"")<<r.bindingWrittenCount[k];o<<"]";
            o<<",\"aliasTracked\":"<<r.aliasTracked; // Elements with a known image (merge::aliasIndices)
            auto L=layoutRegistry.find(r.layout);o<<",\"layout\":[";
            if(L!=layoutRegistry.end())for(std::size_t k=0;k<L->second.bindings.size();++k){const auto& b=L->second.bindings[k];o<<(k?",":"")<<"["<<b.binding<<","<<unsigned(b.descriptorType)<<","<<b.descriptorCount<<","<<(k<L->second.bindingFlags.size()?L->second.bindingFlags[k]:0)<<"]";}
            o<<"]}";first=false;}
    }catch(...){}
    o<<"]";return o.str();
}
namespace {
using D16=std::array<double,16>;
bool invert(const D16& in,D16& out){
    double a[4][8]{};
    for(unsigned r=0;r<4;++r){for(unsigned col=0;col<4;++col)a[r][col]=in[col*4+r];a[r][4+r]=1;}
    for(unsigned col=0;col<4;++col){unsigned pivot=col;for(unsigned r=col+1;r<4;++r)if(std::fabs(a[r][col])>std::fabs(a[pivot][col]))pivot=r;
        if(!std::isfinite(a[pivot][col])||std::fabs(a[pivot][col])<1e-18)return false;
        for(unsigned j=0;j<8;++j)std::swap(a[col][j],a[pivot][j]);const double s=a[col][col];for(auto& v:a[col])v/=s;
        for(unsigned r=0;r<4;++r)if(r!=col){const double f=a[r][col];for(unsigned j=0;j<8;++j)a[r][j]-=f*a[col][j];}}
    for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col){const double v=a[r][col+4];if(!std::isfinite(v))return false;out[col*4+r]=v;}
    return true;
}
D16 mul(const D16& a,const D16& b){D16 m{};for(unsigned col=0;col<4;++col)for(unsigned r=0;r<4;++r){double s=0;for(unsigned k=0;k<4;++k)s+=a[k*4+r]*b[col*4+k];m[col*4+r]=s;}return m;}
}
bool eyeCorrection(const float* relL,const float* relR,const float* posL,const float* posR,std::array<float,16>& out) noexcept {
    if(!relL||!relR||!posL||!posR)return false;
    D16 L{},R{},T{},invL{};
    for(unsigned i=0;i<16;++i){L[i]=relL[i];R[i]=relR[i];if(!std::isfinite(L[i])||!std::isfinite(R[i]))return false;}
    for(unsigned i=0;i<16;++i)T[i]=(i%5==0)?1.0:0.0;
    for(unsigned i=0;i<3;++i){if(!std::isfinite(posL[i])||!std::isfinite(posR[i]))return false;T[12+i]=double(posL[i])-double(posR[i]);}
    if(!invert(L,invL))return false;
    const D16 c=mul(mul(R,T),invL);
    for(unsigned i=0;i<16;++i){if(!std::isfinite(c[i])||std::fabs(c[i])>3.0e38)return false;out[i]=float(c[i]);}
    return true;
}
void noteFallback(const char* reason) noexcept {++stats.fallbacks;stats.lastFallback.store(reason);}
namespace {
// Caller holds stats.skipMutex.
std::string formatPipelineRows(){std::ostringstream o;for(unsigned k=0;k<stats.pipeRowCount;++k){const auto& ps=stats.pipeRows[k];o<<(k?",":"")<<"{\"vs\":\""<<std::hex<<ps.vsHash<<"\",\"fs\":\""<<ps.fsHash<<std::dec<<"\",\"draws\":"<<ps.draws<<",\"pass\":"<<ps.pass<<",\"status\":"<<ps.status<<",\"vsRage\":"<<(ps.vsRage?1:0)<<",\"fsRage\":"<<(ps.fsRage?1:0)<<",\"entries\":"<<ps.entries<<",\"rewritten\":"<<(ps.rewritten?1:0)<<"}";}return o.str();}
std::string formatUnpatchedRows(){std::ostringstream o;for(unsigned k=0;k<stats.unpatchedRowCount;++k){const auto& u=stats.unpatchedRows[k];o<<(k?",":"")<<"{\"module\":"<<u.module<<",\"status\":"<<u.status<<",\"members\":"<<u.members<<",\"set0Bindings\":"<<u.set0Bindings<<",\"draws\":"<<u.draws<<",\"vsHash\":\""<<std::hex<<u.vsHash<<std::dec<<"\",\"vsWords\":"<<u.words<<",\"fsHash\":\""<<std::hex<<u.fsHash<<std::dec<<"\"}";}return o.str();}
}
// ---- STEREO TEST 20 registry/runtime side (StereoBoundary.h namespace merge) ----
namespace {
std::atomic<int> mergeEnabledOverride{-1};std::atomic<unsigned> mergeCapOverride{};
std::atomic<bool> mergeEnabledProperty{true};std::atomic<unsigned> mergeCapProperty{merge::defaultCap};std::atomic<unsigned> mergePropertyCalls{};
}
namespace merge {
Stats& mergeStats() noexcept {return mergeStatsValue;}
std::uint32_t leftTextureIndex(std::uintptr_t texture) noexcept {const auto i=textureBindlessIndex(texture);return i<65536u?i:UINT32_MAX;}
bool pipelineTextures(VkPipeline pipeline,TexScan& out) noexcept {
    try{std::lock_guard lock(coldMutex);auto it=texScanByPipeline.find(pipeline);if(it==texScanByPipeline.end())return false;out=it->second;return true;}catch(...){return false;}
}
bool dynamicSlots(VkPipelineLayout layout,std::array<std::uint8_t,32>& slotOfBinding) noexcept {
    slotOfBinding.fill(0xff);
    try{std::lock_guard lock(coldMutex);auto pl=pipelineLayoutRegistry.find(layout);if(pl==pipelineLayoutRegistry.end())return false;
        auto lr=layoutRegistry.find(pl->second);if(lr==layoutRegistry.end())return false;
        const auto& dyn=lr->second.dynamicBindings; // set-0 dynamic offset j belongs to binding dyn[j] (element 0 = first j)
        for(std::size_t j=0;j<dyn.size()&&j<255;++j)if(dyn[j]<32&&slotOfBinding[dyn[j]]==0xff)slotOfBinding[dyn[j]]=std::uint8_t(j);
        return true;}catch(...){return false;}
}
unsigned framebufferImages(VkFramebuffer leftFb,std::array<VkImage,9>& images) noexcept {
    images.fill(VK_NULL_HANDLE);unsigned n=0;
    try{std::lock_guard lock(coldMutex);auto it=framebufferRegistry.find(leftFb);if(it==framebufferRegistry.end())return 0;
        for(std::uint32_t i=0;i<it->second.count&&i<it->second.views.size()&&n<images.size();++i){auto v=viewRegistry.find(it->second.views[i]);if(v!=viewRegistry.end())images[n++]=v->second.image;}
    }catch(...){return 0;}
    return n;
}
unsigned aliasIndices(const VkImage* images,unsigned count,std::array<std::uint32_t,16>& out) noexcept {
    unsigned n=0;if(!images)return 0;
    try{std::lock_guard lock(coldMutex);
        for(const auto& entry:setRegistry){const auto& r=entry.second;
            for(unsigned i=0;i<count;++i){if(!images[i])continue;auto found=r.elementsOf.find(images[i]);if(found==r.elementsOf.end())continue;
                for(const std::uint32_t key:found->second){const std::uint32_t element=key&0xffffffu; // Any sampled binding (index values are compared unqualified)
                    bool dup=false;for(unsigned k=0;k<n;++k)dup|=out[k]==element;if(dup)continue;
                    if(n==out.size())return aliasOverflow;out[n++]=element;}}}
    }catch(...){return aliasOverflow;}
    return n;
}
bool enabled() noexcept {
    const int forced=mergeEnabledOverride.load(std::memory_order_relaxed);if(forced>=0)return forced!=0;
#ifdef __ANDROID__
    if((mergePropertyCalls.fetch_add(1,std::memory_order_relaxed)&63u)==0){char v[PROP_VALUE_MAX]{};
        const int n=__system_property_get("debug.rdrvr.stereoreplay.merge",v);
        mergeEnabledProperty.store(!(n>0&&v[0]=='0'&&!v[1]),std::memory_order_relaxed);
        mergeAlwaysProperty.store(n>0&&v[0]=='2'&&!v[1],std::memory_order_relaxed);
        unsigned c=defaultCap;
        if(__system_property_get("debug.rdrvr.stereoreplay.mergecap",v)>0&&v[0]){char* e=nullptr;const unsigned long x=std::strtoul(v,&e,0);if(e!=v)c=unsigned(std::clamp<unsigned long>(x,minCap,maxCap));}
        mergeCapProperty.store(c,std::memory_order_relaxed);}
#endif
    return mergeEnabledProperty.load(std::memory_order_relaxed);
}
unsigned cap() noexcept {const unsigned forced=mergeCapOverride.load(std::memory_order_relaxed);return forced?std::clamp(forced,minCap,maxCap):mergeCapProperty.load(std::memory_order_relaxed);}
void setForTests(int enabledValue,unsigned capValue) noexcept {mergeEnabledOverride.store(enabledValue<0?-1:enabledValue?1:0,std::memory_order_relaxed);mergeCapOverride.store(capValue,std::memory_order_relaxed);}
}
// Boundary test + census (report stereoReplay.boundaryTest / .census / .tapeSample / .barrierFilter)
std::string boundaryReport(){
    std::ostringstream o;const auto& b=stats.boundary;unsigned twinPasses=0;
    for(const auto& s:twinTable){const auto k=s.key.load(std::memory_order_relaxed);if(k&&k!=twinTombstone())++twinPasses;}
    const unsigned phase=boundaryPhase.load(std::memory_order_relaxed);
    o<<"{\"mode\":"<<testMode.load(std::memory_order_relaxed)<<",\"phase\":\""<<(phase==1?"a":phase==2?"b":"-")<<"\",\"flags\":"<<boundaryFlags.load(std::memory_order_relaxed)
     <<",\"framesOn\":"<<b.framesOn<<",\"framesOff\":"<<b.framesOff<<",\"extraInserted\":"<<(b.extraR.load()+b.extraL.load())<<",\"extraR\":"<<b.extraR<<",\"extraL\":"<<b.extraL
     <<",\"splitsR\":"<<b.splitsR<<",\"splitsL\":"<<b.splitsL<<",\"leftVariant\":\""<<leftVariant.load(std::memory_order_relaxed)<<"\",\"queriesObserved\":"<<(boundaryQueriesObserved.load()?1:0)
     <<",\"declined\":{\"noTwins\":"<<b.noTwins<<",\"fewDraws\":"<<b.fewDraws<<",\"inline\":"<<b.inlineSkipped<<",\"leftNotStored\":"<<b.leftNotStored<<",\"leftLayout\":"<<b.leftLayout
     <<",\"leftQuery\":"<<b.leftQuery<<",\"leftContents\":"<<b.leftContents<<",\"extraLayout\":"<<b.extraLayout<<"}"
     <<",\"twinsCreated\":"<<b.twinsCreated<<",\"twinFailures\":"<<b.twinFailures<<",\"twinsDestroyed\":"<<b.twinsDestroyed<<",\"twinPasses\":"<<twinPasses<<",\"tableFull\":"<<b.tableFull
     <<",\"passDescs\":"<<b.passDescs<<",\"prevDraws\":[";
    for(unsigned s=0;s<2;++s){o<<(s?",[":"[");unsigned n=0;for(unsigned k=0;k<boundary::prevDrawPasses;++k)if(b.prevDraws[s*boundary::prevDrawPasses+k].load(std::memory_order_relaxed))n=k+1;
        for(unsigned k=0;k<n;++k)o<<(k?",":"")<<b.prevDraws[s*boundary::prevDrawPasses+k].load(std::memory_order_relaxed);o<<"]";}
    o<<"],\"mergeEnabled\":"<<(merge::enabled()?1:0)<<",\"mergeAlways\":{\"property\":"<<(mergeAlways()?1:0)<<",\"frames\":"<<b.mergeAlwaysFrames.load()<<"}"<<",\"mergeCap\":"<<merge::cap()<<",\"mergeGuard\":{\"overrides\":"<<merge::decideOverrides.load(std::memory_order_relaxed)
     <<",\"gapEntriesSkipped\":"<<merge::gapEntriesSkipped.load(std::memory_order_relaxed)<<"},\"merge\":"<<mergeStatsValue.json();
    o<<"}";return o.str();
}
// Every render pass the engine began on an eye framebuffer while the DETAIL profiler ran (gpuPassEdge), with its PassDesc.
std::string gpuEyePassesJson(){
    std::array<VkRenderPass,32> passes{};std::array<std::uint8_t,32> regions{};unsigned n=0;
    {std::lock_guard lock(gpu::state.m);n=gpu::state.eyePassCount;passes=gpu::state.eyePasses;regions=gpu::state.eyePassRegion;}
    std::ostringstream o;o<<"[";
    for(unsigned i=0;i<n;++i){PassDesc d{};bool known=false;{std::lock_guard lock(coldMutex);auto it=passRegistry.find(passes[i]);if(it!=passRegistry.end()){d=it->second;known=true;}}
        o<<(i?",":"")<<"{\"region\":\""<<gpu::regionName(regions[i])<<"\",\"desc\":"<<(known?passDescJson(passes[i],d):"{\"rp\":\""+hexHandle(passes[i])+"\"}")<<"}";}
    o<<"]";return o.str();
}
// Distinct (render pass, class, region, renderArea) begun in DETAIL frames, with load/store ops (gpuStages.passDescs).
std::string gpuPassDescsJson(){
    struct Row {VkRenderPass rp{};std::string key;unsigned region{};VkRect2D area{};std::uint64_t count{};};
    std::vector<Row> rows;std::uint64_t overflow=0;
    {std::lock_guard lock(gpu::state.m);const auto& s=gpu::state;overflow=s.descOverflow;
        for(unsigned i=0;i<s.descCount;++i){const auto& d=s.descs[i];rows.push_back({d.rp,d.cls<s.classCount?std::string(s.classes[d.cls].key):std::string("?"),d.region,d.area,d.count});}}
    std::ostringstream o;o<<"{\"overflow\":"<<overflow<<",\"list\":[";
    for(std::size_t i=0;i<rows.size();++i){const auto& r=rows[i];PassDesc d{};bool known=false;
        {std::lock_guard lock(coldMutex);auto it=passRegistry.find(r.rp);if(it!=passRegistry.end()){d=it->second;known=true;}}
        o<<(i?",":"")<<"{\"class\":\""<<r.key<<"\",\"region\":\""<<gpu::regionName(r.region)<<"\",\"rp\":\""<<hexHandle(r.rp)<<"\",\"area\":["<<r.area.offset.x<<","<<r.area.offset.y
         <<","<<r.area.extent.width<<","<<r.area.extent.height<<"],\"count\":"<<r.count<<",\"load\":\""<<(known?opSignature(d,false):std::string("?"))
         <<"\",\"store\":\""<<(known?opSignature(d,true):std::string("?"))<<"\"}";}
    o<<"]}";return o.str();
}
std::string censusReport(){
    std::ostringstream o;o<<"{";
    for(unsigned s=0;s<2;++s){const auto& c=stats.census[s];
        o<<(s?",":"")<<"\""<<(s?"color":"depth")<<"\":{\"samples\":"<<c.samples<<",\"passes\":"<<c.passes<<",\"boundaries\":"<<c.boundaries<<",\"sameFbLoad\":"<<c.sameFbLoad<<",\"sameFbClear\":"<<c.sameFbClear
         <<",\"fbSwitch\":"<<c.fbSwitch<<",\"attachmentBarrier\":"<<c.attachmentBarrier<<",\"depthWrittenBefore\":"<<c.depthWrittenBefore<<",\"draws\":"<<c.draws<<",\"dsWriteDraws\":"<<c.dsWriteDraws
         <<",\"passStartStencil\":"<<c.passStartStencil<<",\"restartNoStencil\":"<<c.restartNoStencil<<",\"restartUnknown\":"<<c.restartUnknown<<",\"restartEmpty\":"<<c.restartEmpty
         <<",\"gaps\":{\"barriers\":"<<c.gapBarriers<<",\"imageOwn\":"<<c.gapImageOwn<<",\"imageEye\":"<<c.gapImageEye<<",\"imageOther\":"<<c.gapImageOther<<",\"imageNoop\":"<<c.gapImageNoop
         <<",\"memory\":"<<c.gapMemory<<",\"buffer\":"<<c.gapBuffer<<",\"copyBuffer\":"<<c.gapCopyBuffer<<",\"bufferToImage\":"<<c.gapBufferToImage<<",\"imageToBuffer\":"<<c.gapImageToBuffer
         <<",\"clearColor\":"<<c.gapClearColor<<",\"clearDepth\":"<<c.gapClearDepth<<",\"fill\":"<<c.gapFill<<",\"update\":"<<c.gapUpdate<<",\"imageTransfer\":"<<c.gapImageTransfer<<",\"state\":"<<c.gapState<<",\"push\":"<<c.gapPush<<"}}";}
    const std::string gpuPasses=gpuEyePassesJson(); // gpu state + coldMutex, before skipMutex (resolveFramebuffer nests coldMutex -> skipMutex)
    std::lock_guard g(stats.skipMutex);
    o<<",\"passChain\":["<<(stats.passChain[0].empty()?std::string("{}"):stats.passChain[0])<<","<<(stats.passChain[1].empty()?std::string("{}"):stats.passChain[1])<<"],\"eyePassDescs\":["<<stats.eyePassDescs<<"]";
    o<<",\"eyePassesGpu\":"<<gpuPasses;
    o<<",\"drawHist\":{\"depth\":"<<drawHistJson(0)<<",\"color\":"<<drawHistJson(1)<<"}}";return o.str();
}
// Override-slot bookkeeping / scrub / deferred destroys (top-level fields of stereoReplay, see Stats::Scrub)
std::string scrubReport(){
    const auto& s=stats.scrub;std::size_t live[2]{},pendingViews{},pendingFramebuffers{},pendingImages{},recent{};
    try{std::lock_guard lock(coldMutex);for(const auto& entry:setRegistry)for(unsigned o=0;o<2;++o)live[o]+=entry.second.overrides[o].size();
        pendingViews=deferredViews.size();pendingFramebuffers=deferredFramebuffers.size();pendingImages=deferredImages.size();recent=recentSources.size();}catch(...){}
    std::ostringstream o;
    o<<",\"overrideEntries\":"<<s.overrideEntries<<",\"overrideReverts\":"<<s.overrideReverts<<",\"overrideScrubs\":"<<s.overrideScrubs<<",\"staleOverrideViews\":"<<s.staleOverrideViews
     <<",\"deferredViewDestroys\":"<<s.deferredViewDestroys<<",\"deferredViewFlushes\":"<<s.deferredViewFlushes<<",\"scrub\":"<<(scrubOn()?1:0)
     <<",\"scrubDetail\":{\"liveEntries\":["<<live[0]<<","<<live[1]<<"],\"recentSources\":"<<recent<<",\"pendingViews\":"<<pendingViews<<",\"pendingFramebuffers\":"<<pendingFramebuffers<<",\"maxPending\":"<<s.maxPending
     <<",\"staleLive\":"<<s.staleLive<<",\"staleLiveInWindow\":"<<s.staleLiveInWindow<<",\"staleRecent\":"<<s.staleRecent<<",\"lastStaleView\":"<<s.lastStaleView<<",\"lastStaleFrame\":"<<s.lastStaleFrame
     <<",\"entriesMirrorDropped\":"<<s.entriesMirrorDropped<<",\"entriesUnknownSource\":"<<s.entriesUnknownSource<<",\"revertsUnwritten\":"<<s.revertsUnwritten<<",\"scrubsUnwritten\":"<<s.scrubsUnwritten
     <<",\"deferredFramebufferDestroys\":"<<s.deferredFramebufferDestroys<<",\"deferredFramebufferFlushes\":"<<s.deferredFramebufferFlushes<<",\"framebuffersDestroyedNow\":"<<s.framebuffersDestroyedNow
     <<",\"teardownViewFlushes\":"<<s.teardownViewFlushes<<",\"teardownFramebufferFlushes\":"<<s.teardownFramebufferFlushes<<",\"droppedAtDeviceLoss\":"<<s.droppedAtDeviceLoss
     <<",\"countOnly\":{\"reverts\":"<<s.countOnlyReverts<<",\"scrubs\":"<<s.countOnlyScrubs<<",\"deferrals\":"<<s.countOnlyDeferrals<<"},\"viewDeferUnavailable\":"<<s.viewDeferUnavailable
     <<",\"hooks\":"<<s.hooks<<",\"frameResets\":"<<s.frameResets<<",\"frame\":"<<scrubFrame.load(std::memory_order_relaxed)<<",\"retired\":"<<scrubRetired.load(std::memory_order_relaxed)<<",\"window\":"<<scrubWindow
     // Deferred images; engine-set slots still holding a destroyed view (the view as a hex string)
     <<",\"pendingImages\":"<<pendingImages<<",\"deferredImageDestroys\":"<<s.deferredImageDestroys<<",\"deferredImageFlushes\":"<<s.deferredImageFlushes<<",\"teardownImageFlushes\":"<<s.teardownImageFlushes
     <<",\"staleEngineViews\":"<<s.staleEngineViews<<",\"staleEngineSlots\":"<<s.staleEngineSlots<<",\"lastStaleEngineView\":\"0x"<<std::hex<<s.lastStaleEngineView.load()<<std::dec<<"\""
     <<",\"lastStaleEngineBinding\":"<<(s.lastStaleEngineSlot.load()>>32)<<",\"lastStaleEngineElement\":"<<(s.lastStaleEngineSlot.load()&0xffffffffu)<<",\"lastStaleEngineFrame\":"<<s.lastStaleEngineFrame<<"}";
    return o.str();
}
std::string report(){
    std::ostringstream o;
    o<<"{\"enabled\":"<<(enabled()?"true":"false")<<",\"rawBound\":"<<(rawBound.load()?"true":"false")
     <<",\"shadersPatched\":"<<stats.shadersPatched<<",\"shadersDeclined\":"<<stats.shadersDeclined<<",\"shadersNotVertex\":"<<stats.shadersNotVertex
     <<",\"layoutsPatched\":"<<stats.layoutsPatched<<",\"identityPushes\":"<<stats.identityPushes
     <<",\"viewsTracked\":"<<stats.viewsTracked<<",\"framebuffersTracked\":"<<stats.framebuffersTracked<<",\"framebuffersOwned\":"<<stats.framebuffersOwned<<",\"framebufferMisses\":"<<stats.framebufferMisses
     <<",\"bindlessLayouts\":"<<stats.bindlessLayouts<<",\"bindlessSets\":"<<stats.bindlessSets<<",\"bindlessClones\":"<<bindlessClones()<<",\"mirroredWrites\":"<<stats.mirroredWrites<<",\"mirroredCopies\":"<<stats.mirroredCopies<<",\"overrideCalls\":"<<stats.overrideCalls<<",\"overridesOutOfRange\":"<<stats.overridesOutOfRange<<",\"overridesUnwritten\":"<<stats.overridesUnwritten<<",\"lastBadOverrideLeft\":"<<(stats.lastBadOverride.load()>>32)<<",\"lastBadOverrideRight\":"<<(stats.lastBadOverride.load()&0xffffffffu)<<",\"lastUnwrittenLeft\":"<<(stats.lastUnwrittenOverride.load()>>32)<<",\"lastUnwrittenRight\":"<<(stats.lastUnwrittenOverride.load()&0xffffffffu)<<scrubReport()<<",\"sets\":"<<setReport()
     <<",\"recordings\":"<<stats.recordings<<",\"recordingsInvalid\":"<<stats.recordingsInvalid<<",\"lastInvalid\":\""<<stats.lastInvalid.load()<<"\""
     <<",\"replays\":"<<stats.replays<<",\"replaysFaulted\":"<<stats.replaysFaulted<<",\"replaysDeclined\":"<<stats.replaysDeclined<<",\"lastDecline\":\""<<stats.lastDecline.load()<<"\""
     <<",\"fallbacks\":"<<stats.fallbacks<<",\"lastFallback\":\""<<stats.lastFallback.load()<<"\",\"passesReplayed\":"<<stats.passesReplayed<<",\"passesSkipped\":"<<stats.passesSkipped
     <<",\"depth\":{\"tapeCommands\":"<<stats.tapeCommands[0]<<",\"tapeBytes\":"<<stats.tapeBytes[0]<<",\"replayCommands\":"<<stats.replayCommands[0]<<",\"replayNs\":"<<stats.replayNs[0]<<"}"
     <<",\"color\":{\"tapeCommands\":"<<stats.tapeCommands[1]<<",\"tapeBytes\":"<<stats.tapeBytes[1]<<",\"replayCommands\":"<<stats.replayCommands[1]<<",\"replayNs\":"<<stats.replayNs[1]<<"}"
     <<",\"ubo\":{\"draws\":"<<stats.uboDraws<<",\"binds\":"<<stats.uboBinds<<",\"blocks\":"<<stats.uboBlocks<<",\"bytes\":"<<stats.uboBytes<<",\"frameInvalid\":"<<stats.uboFrameInvalid<<",\"ringFull\":"<<stats.uboRingFull<<",\"dedupeMismatch\":"<<stats.uboDedupeMismatch<<",\"noLayout\":"<<stats.uboNoLayout<<",\"noDynamic\":"<<stats.uboNoDynamic<<",\"cursorWrites\":"<<stats.uboCursorWrites<<",\"countMismatch\":"<<stats.uboCountMismatch<<",\"lastOffsets\":"<<stats.uboLastOffsets<<",\"lastDynamic\":"<<stats.uboLastDynamic<<",\"ringSize\":"<<stats.uboRingSize<<",\"cursorLast\":"<<stats.uboCursorLast<<",\"rageM1\":["<<stats.rageM1Match<<","<<stats.rageM1Transposed<<","<<stats.rageM1Mismatch<<"],\"rageM3\":["<<stats.rageM3Match<<","<<stats.rageM3Transposed<<","<<stats.rageM3Mismatch<<"],\"wvpInv\":["<<stats.wvpInvMatch<<","<<stats.wvpInvMismatch<<"],\"camera\":["<<stats.cameraMatch<<","<<stats.cameraMismatch<<"],\"wvpInvRewritten\":"<<stats.wvpInvRewritten<<",\"wvpInvLeft\":"<<stats.wvpInvLeft<<",\"cacheCleans\":"<<stats.cacheCleans<<",\"maxBlockSize\":"<<stats.maxBlockSize<<",\"rageM1All\":["<<stats.rageM1AllMatch<<","<<stats.rageM1AllMismatch<<"],\"wvpInvRelational\":"<<stats.wvpInvRelational<<",\"endBarriers\":"<<stats.endBarriers<<",\"prevProjRight\":"<<stats.prevProjRight<<",\"prevProjLeft\":"<<stats.prevProjLeft<<",\"identityPushesDraw\":"<<stats.identityPushesDraw<<",\"samples\":{"<<[&]{std::lock_guard g(stats.skipMutex);return stats.uboSamples;}()<<"\"_\":0},\"dump\":{"<<[&]{std::lock_guard g(stats.skipMutex);return stats.wrapperDump.empty()?std::string("\"_\":0"):stats.wrapperDump;}()<<"}}"<<",\"colorPipelines\":["<<[&]{std::lock_guard g(stats.skipMutex);return formatPipelineRows();}()<<"]"<<",\"probe\":["<<[&]{std::ostringstream o;for(unsigned e=0;e<2;++e){const auto& st=stats.probe[e];o<<(e?",":"")<<"{\"draws\":"<<st.draws<<",\"withEntries\":"<<st.withEntries<<",\"noSet0\":"<<st.noSet0<<",\"noLayout\":"<<st.noLayout<<",\"wvp\":["<<st.wvpMatch<<","<<st.wvpMismatch<<","<<st.wvpZero<<"],\"wvpMatchPrev\":"<<st.wvpMatchPrev<<",\"m3MatchPrev\":"<<st.m3MatchPrev<<",\"wvpMatchOther\":"<<st.wvpMatchOther<<",\"m3MatchOther\":"<<st.m3MatchOther<<",\"m1\":["<<st.m1Match<<","<<st.m1Mismatch<<"],\"m3\":["<<st.m3Match<<","<<st.m3Mismatch<<"],\"depth\":["<<st.depthL<<","<<st.depthR<<","<<st.depthOther<<"],\"coll\":["<<st.collL<<","<<st.collR<<","<<st.collOther<<"],\"light\":["<<st.lightL<<","<<st.lightR<<","<<st.lightOther<<"],\"sample\":{"<<[&]{std::lock_guard g(stats.skipMutex);return st.sample;}()<<"\"_\":0}}";}return o.str();}()<<"]"<<",\"eyeDump\":{\"dumps\":"<<eyeDump.dumps<<",\"failures\":"<<eyeDump.failures<<",\"lastError\":\""<<eyeDump.lastError<<"\"}"<<",\"drawsPatched\":"<<stats.drawsPatched<<",\"drawsUnpatched\":"<<stats.drawsUnpatched<<",\"drawsUnknownPipeline\":"<<stats.drawsUnknownPipeline<<",\"drawsUnpatchedPass0\":"<<stats.drawsUnpatchedPass0<<",\"shadersRegistered\":"<<stats.shadersRegistered<<",\"pipelinesRegistered\":"<<stats.pipelinesRegistered<<",\"ownerLast\":"<<stats.ownerLast<<",\"fbCacheHits\":"<<stats.fbCacheHits<<",\"startBarriers\":"<<stats.startBarriers<<",\"maskLast\":"<<stats.maskLast<<",\"maskPassesSkipped\":"<<stats.maskPassesSkipped<<",\"maskDrawsSkipped\":"<<stats.maskDrawsSkipped<<",\"foreignTargetsLast\":"<<stats.foreignTargetsLast<<",\"foreignCopiesReplayed\":"<<stats.foreignCopiesReplayed<<",\"foreignCopiesSkipped\":"<<stats.foreignCopiesSkipped<<",\"foreignBarriersKept\":"<<stats.foreignBarriersKept
     <<",\"opCounts\":[[";for(unsigned k=0;k<33;++k)o<<(k?",":"")<<stats.opCounts[0][k].load();o<<"],[";for(unsigned k=0;k<33;++k)o<<(k?",":"")<<stats.opCounts[1][k].load();o<<"]]"
     <<",\"depthResolves\":"<<stats.depthResolves<<",\"depthResolveMisses\":"<<stats.depthResolveMisses
     <<",\"sdAlias\":{\"on\":"<<sdAlias.on.load()<<",\"active\":"<<(sdAlias.active.load()?1:0)<<",\"skipped\":"<<sdAlias.skipped.load()<<",\"routed\":"<<sdAlias.routed.load()<<"}"
     <<",\"passesAutoClosed\":"<<stats.passesAutoClosed<<",\"drawsSkippedOutsidePass\":"<<stats.drawsSkippedOutsidePass<<",\"endsSkipped\":"<<stats.endsSkipped<<",\"faultDisabled\":"<<stats.faultDisabled
     <<",\"scissor\":{"<<[&]{const auto& k=scissor::counters;std::ostringstream so;auto rect=[&](std::uint64_t v){so<<"["<<(v&0xffff)<<","<<((v>>16)&0xffff)<<","<<((v>>32)&0xffff)<<","<<((v>>48)&0xffff)<<"]";};
         so<<"\"seen\":"<<k.seen.load()<<",\"full\":"<<k.full.load()<<",\"remapped\":"<<k.remapped.load()<<",\"declined\":"<<k.declined.load()<<",\"verbatim\":"<<k.verbatim.load()<<",\"empty\":"<<k.empty.load()
          <<",\"lastLeft\":";rect(k.lastLeft.load()); so<<",\"lastRight\":";rect(k.lastRight.load());
         so<<",\"lastShift\":["<<k.lastShiftX.load()<<","<<k.lastShiftY.load()<<"],\"maxAbsShiftX\":"<<k.maxAbsShiftX.load()<<",\"mode\":\""<<(testMode.load()==testModeScissorAsLeft?"LEFT":"REMAP")<<"\"";return so.str();}()<<"}"
     <<",\"tapeDrawsOutsidePass\":["<<stats.tapeDrawsOutsidePass[0]<<","<<stats.tapeDrawsOutsidePass[1]<<"],\"tapeOpenAtEnd\":["<<stats.tapeOpenAtEnd[0]<<","<<stats.tapeOpenAtEnd[1]<<"],\"skipNoFramebuffer\":"<<stats.skipNoFramebuffer<<",\"skipNoView\":"<<stats.skipNoView<<",\"skipNoEye\":"<<stats.skipNoEye<<",\"skipMixed\":"<<stats.skipMixed<<",\"skipSamples\":[";
    {std::lock_guard g(stats.skipMutex);for(unsigned i=0;i<stats.skipNext;++i){if(i)o<<',';o<<'"'<<stats.skipSamples[i]<<'"';}
     o<<"],\"tapeShape\":[\""<<stats.tapeShape[0]<<"\",\""<<stats.tapeShape[1]<<"\"],\"foreignSamples\":[["<<stats.foreignSamples[0]<<"],["<<stats.foreignSamples[1]<<"]],\"unpatchedDrawn\":["<<formatUnpatchedRows();}
    o<<"],\"patchStatus\":[";
    for(unsigned i=0;i<stats.patchStatus.size();++i){if(i)o<<',';o<<stats.patchStatus[i].load();}
    o<<"],\"tapeSample\":{\"serial\":["<<stats.shapeSerial[0]<<","<<stats.shapeSerial[1]<<"],\"commands\":["<<stats.sampledCommands[0]<<","<<stats.sampledCommands[1]<<"],\"bytes\":["<<stats.sampledBytes[0]<<","<<stats.sampledBytes[1]
     <<"],\"draws\":["<<stats.sampledDraws[0]<<","<<stats.sampledDraws[1]<<"],\"shapeCapped\":["<<stats.shapeCapped[0]<<","<<stats.shapeCapped[1]<<"]}"
     <<",\"barrierFilter\":{\"calls\":"<<stats.barrierCalls<<",\"dropped\":"<<stats.barrierCallsDropped<<",\"execOnlyDropped\":"<<stats.barrierExecOnlyDropped<<",\"imageKept\":"<<stats.imageBarriersKept
     <<",\"imageDropped\":"<<stats.imageBarriersDropped<<",\"imageNoop\":"<<stats.imageBarriersNoop<<"}"
     <<",\"boundaryTest\":"<<boundaryReport()<<",\"census\":"<<censusReport();
    {std::string g=gpuStagesReport();if(!g.empty()&&g.back()=='}')g.insert(g.size()-1,",\"passDescs\":"+gpuPassDescsJson());
     o<<",\"async\":"<<asyncReport()<<",\"gpuStages\":"<<g<<",\"driverSets\":"<<driverSetsReport()<<"}";}return o.str(); // driverSets appended last
}
}
