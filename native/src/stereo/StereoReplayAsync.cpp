// Stereo replay, secondary-command-buffer path (debug.rdrvr.stereoreplay.async=1). See StereoReplayAsync.h.
//
// One job per replayed stage:
//  prepareJob  [render] copies FrameMaps and the bindless set map, snapshots the live mask, the engine frame ring
//              and the engine's bound state (for the restore), picks/resets the right-target owner's command pool.
//  recordJob   [any]    walks the tape once: every tape render pass becomes one secondary command buffer that starts
//              by re-establishing the complete bound state (secondaries inherit nothing) and then replays the pass
//              with the shared right-eye rewrite (ReplayRewriter); ops outside passes only update the shadow state
//              or become skeleton entries. Never touches the primary, tracked(), texture objects or DUD.
//  spliceJob   [render] start barrier + correction push, skeleton (outside ops exactly as the inline replay emits
//              them; BeginRenderPass(SECONDARY_COMMAND_BUFFERS) + ExecuteCommands + EndRenderPass per pass), then the
//              inline replay's tail (identity push, state restore, +0xa0 layouts, end barrier, depth resolve, dump).
// Anything the shadow cannot represent declines the job before the primary is touched; the inline replay runs.
#include "StereoTestMode.h"
#include "StereoReplayAsync.h"
#include "StereoReplayInternal.h"
#include "StereoScissor.h" // Moon: right-eye remap of sub-rectangle scissors
#include "EyeCullTape.h"   // EYE CULL B: tape draws flagged outside the right eye are not replayed
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef __ANDROID__
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <unistd.h>
#endif

namespace rdrvr::engine::stereo {
namespace secondary {
using namespace internal;
namespace {
constexpr float identity[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
std::atomic<unsigned> modeOverride{UINT32_MAX};
struct Counters {
    std::atomic<std::uint64_t> jobs{},fallbacks{},faults{},secondaries{},executeCalls{},poolsCreated{},poolResets{},allocated{};
    std::atomic<std::uint64_t> prepareNs[2]{},recordNs[2]{},spliceNs[2]{};
    std::atomic<const char*> lastDecline{"NONE"};
    // worker mode (async=2)
    std::atomic<std::uint64_t> launched{},workerJobs{},workerDeclined{},takeovers{},predictionMismatches{},discarded{},floorViolations{},joinWaits{};
    std::atomic<std::uint64_t> workerRecordNs[2]{},joinWaitNs[2]{},renderNs[2]{};
    std::atomic<const char*> lastMismatch{"NONE"};
    // streaming (async=2 default): run sums for averages (divide by jobsPerStage)
    std::atomic<std::uint64_t> streamed{},lateLaunches{},streamRefused{},workerChunks{},tails{},waitedJobs{},notReadyJobs{},tapeInvalid{};
    std::atomic<std::uint64_t> jobsPerStage[2]{},joinWaitNsSum[2]{},workerRecordNsSum[2]{},renderNsSum[2]{},tailNsSum[2]{};
    std::atomic<std::uint64_t> joinWaitCount[2]{},workerRecordJobs[2]{},tailCount[2]{}; // renderNsSum counts jobsPerStage
    std::atomic<std::uint64_t> relaunched{},cancelled{},ringTight{},ringPressureJobs{},ringPressureFrames{},ringWatchResets{};
    std::atomic<long long> asyncProperty{-1}; // raw debug.rdrvr.stereoreplay.async (-1: unset)
    // Worker recording per right-target owner (FrameMaps::owner = clone / clone2): jobs started,
    // chunk wall time spent in record* (cancelled jobs included) and draws of the jobs that finished (Done)
    std::atomic<std::uint64_t> ownerJobs[2]{},ownerRecordNs[2]{},ownerDraws[2]{},ownerDone[2]{};
} counters;

// ---------------------------------------------------------------- device entry points
AsyncDeviceFunctions testFunctionsCopy{};std::atomic<bool> testFunctionsSet{};
AsyncDeviceFunctions functions() noexcept {
    if(testFunctionsSet.load(std::memory_order_acquire))return testFunctionsCopy;
    AsyncDeviceFunctions f{}; // loader exports: bypass the engine dispatch slots and every capture observer
    f.createCommandPool=::vkCreateCommandPool;f.destroyCommandPool=::vkDestroyCommandPool;f.resetCommandPool=::vkResetCommandPool;
    f.allocateCommandBuffers=::vkAllocateCommandBuffers;f.beginCommandBuffer=::vkBeginCommandBuffer;f.endCommandBuffer=::vkEndCommandBuffer;
    return f;
}

// ---------------------------------------------------------------- command pools (one per right-target owner)
struct Pool {VkDevice device{};std::uint32_t family{UINT32_MAX};VkCommandPool pool{};std::vector<VkCommandBuffer> buffers;std::size_t used{};std::uint64_t frame{};bool lastColor{};};
std::mutex poolMutex; // held for prepare+record (pool use) and teardown
Pool pools[2];

// ---------------------------------------------------------------- job
struct SkeletonEntry {std::uint32_t at{};                     // tape offset of the op header
                      std::uint32_t secondary{UINT32_MAX};    // pass entries: index into Job::secondaries
                      VkFramebuffer framebuffer{};bool autoClosed{};
                      // STEREO TEST 10/11: part 0 whole pass (twin: its empty twin, begun after it when EXTRA is on),
                      // 1 head / 2 tail of a split pass (twin: the head / tail render pass the splice begins instead of P)
                      std::uint8_t part{};VkRenderPass twin{};
                      // STEREO TEST 20: draws recorded in the pass, merge::PassBits of its draws, the recorded
                      // LEFT framebuffer (own images of the next gap) and the pass's PassTwinFlags (0 without twins)
                      std::uint32_t draws{};std::uint8_t mergeBits{};VkFramebuffer leftFb{};std::uint32_t passFlags{};};
// Shadow of the logical bound state of the replayed stream. Secondaries inherit nothing, so every pass's
// secondary starts by re-emitting it. Descriptor binds are kept as the ordered list of calls still contributing
// a binding (exact re-emission, including set calls the per-index BoundState tracking would lose).
struct Shadow {
    BoundState state{}; // setCalls unused
    struct SetCall {VkPipelineBindPoint point{};VkPipelineLayout layout{};std::uint32_t first{},count{},offsetCount{};std::array<VkDescriptorSet,8> sets{};std::array<std::uint32_t,64> offsets{};};
    std::array<SetCall,16> calls{};unsigned callCount{};
    bool pushSets(VkPipelineBindPoint point,VkPipelineLayout layout,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* s,std::uint32_t n,const std::uint32_t* o) noexcept {
        if(first>=8||!count||count>8||first+count>8||n>64)return false;
        unsigned w=0;for(unsigned i=0;i<callCount;++i){const auto& k=calls[i];if(k.first>=first&&k.first+k.count<=first+count)continue;calls[w++]=calls[i];} // fully rebound
        callCount=w;if(callCount==calls.size())return false;
        auto& c=calls[callCount++];c.point=point;c.layout=layout;c.first=first;c.count=count;c.offsetCount=n;
        for(std::uint32_t i=0;i<count;++i)c.sets[i]=s[i];for(std::uint32_t i=0;i<n;++i)c.offsets[i]=o[i];return true;}
    // The prologue is emitted like emitState(prologue): calls in ascending first set.
    bool seed(const BoundState& prologue) noexcept {
        state=prologue;callCount=0;
        for(unsigned first=0;first<8;++first){state.setCalls[first].valid=false;const auto& k=prologue.setCalls[first];if(!k.valid)continue;
            if(!pushSets(k.point,k.layout,first,k.count,k.sets.data(),k.offsetCount,k.offsets.data()))return false;}
        return true;}
};
// Resumable recording state: a streaming job consumes its tape in chunks while the render thread still records it.
struct RecordCursor {bool begun{};Shadow sh{};unsigned skipDepth{},passIndex{};bool inPass{};VkCommandBuffer cur{};SkeletonEntry open{};std::size_t at{};
    // The open pass (tape pass index, its render pass, draws recorded so far, split point and twins)
    unsigned thisPass{},draws{},splitAt{};VkRenderPass pass{},head{},tail{};
    // merge::PassBits of the open pass so far; its framebuffer slot in Job::merge (-1: unverifiable)
    std::uint8_t passBits{};int fbSlot{-1};
    eyecull::tape::Pending ecRight{};}; // EYE CULL B right drops of this job, committed at spliceJob Done only
// STEREO TEST 20 state of a colour job. Prepared on the render thread (indices of the maps' engine
// textures), extended by the recording (framebuffer index sets, per-pipeline scans, dynamic slots), consumed by the splice.
struct MergeJob {
    bool on{};unsigned cap{merge::defaultCap};
    std::array<std::uint32_t,8> roleIdx{},rightIdx{};unsigned images{}; // bindless index of maps.images[k].left/rightTexture
    struct Fb {VkFramebuffer leftFb{};std::array<VkImage,9> images{};unsigned n{};std::array<std::uint32_t,32> idx{};unsigned idxN{};bool valid{};};
    std::array<Fb,4> fbs{};unsigned fbCount{};
    std::unordered_map<VkPipeline,merge::TexScan> scans;            // job-local cache of merge::pipelineTextures
    struct Dyn {VkPipelineLayout layout{};std::array<std::uint8_t,32> slot{};bool ok{};};
    std::array<Dyn,4> dyn{};unsigned dynCount{},dynNext{};
    std::uint64_t ringReads{};
    // splice scratch (render thread)
    std::vector<merge::PassInfo> infos;std::vector<merge::Keep> keep;std::vector<std::uint32_t> passAt;std::vector<std::uint8_t> skip;std::vector<VkCommandBuffer> batch;
    void reset() noexcept {on=false;cap=merge::defaultCap;roleIdx.fill(UINT32_MAX);rightIdx.fill(UINT32_MAX);images=0;fbs={};fbCount=0;scans.clear();dyn={};dynCount=0;dynNext=0;ringReads=0;}
};
struct Job {
    // prepareJob [render]
    Stage stage{};VkCommandBuffer primary{};FrameMaps maps{};unsigned mask{};VkPipelineLayout layout{};SetMap sets{};BoundState before{};
    const Tape* tape{};ForeignTargets foreign{};RingSnapshot ring{};Pool* pool{};AsyncDeviceFunctions fn{};
    RewriteOptions opts{}; // worker mode: shared frame ring region, job-owned PipelineInfo copies, no sample strings
    // recordJob [any]
    std::optional<ReplayRewriter> rewriter;std::vector<VkCommandBuffer> secondaries;std::vector<SkeletonEntry> skeleton;
    const char* decline{"NONE"};
    RecordCursor rc{};bool streamed{}; // streamed: launched at the stage start (foreign targets known only at the splice)
    unsigned boundary{};               // BoundaryExtraR/SplitR of the frame (latched at launch / prepare; the splice uses the same)
    std::atomic<bool> cancel{};        // worker mode: the job was invalidated; recordStep stops at the next op
    MergeJob merge{};                  // (on: BoundaryMergeR and the colour stage)
};
Job jobs[2]; // per stage; owned by the render thread in async mode 1

// ---------------------------------------------------------------- merge (STEREO TEST 20)
// [render] at prepare/launch: the bindless indices of the maps' engine textures. Any left index missing -> no merging.
void setupMerge(Job& j) noexcept {
    auto& mj=j.merge;mj.reset();
    if(!(j.boundary&BoundaryMergeR)||j.stage!=Stage::Color)return;
    mj.on=true;mj.cap=merge::cap();mj.images=std::min(j.maps.imageCount,unsigned(mj.roleIdx.size()));
    for(unsigned k=0;k<mj.images;++k){mj.roleIdx[k]=merge::leftTextureIndex(j.maps.images[k].leftTexture);mj.rightIdx[k]=merge::leftTextureIndex(j.maps.images[k].rightTexture);
        if(mj.roleIdx[k]==UINT32_MAX)mj.on=false;}
    if(!mj.on)merge::mergeStats().keptNoIndex.fetch_add(1,std::memory_order_relaxed);
}
// [any] the index set of a recorded LEFT framebuffer: left indices of the maps images attached to it, unioned with every
// bindless element whose view's image is one of those images or their right images (aliases). -1: unverifiable.
int fbSlotFor(Job& j,VkFramebuffer leftFb) noexcept {
    auto& mj=j.merge;
    for(unsigned i=0;i<mj.fbCount;++i)if(mj.fbs[i].leftFb==leftFb)return mj.fbs[i].valid?int(i):-1;
    if(mj.fbCount==mj.fbs.size())return -1;
    const unsigned slot=mj.fbCount++;auto& f=mj.fbs[slot];f=MergeJob::Fb{};f.leftFb=leftFb;
    f.n=merge::framebufferImages(leftFb,f.images);
    std::array<VkImage,18> query{};unsigned q=0;
    auto add=[&](std::uint32_t idx){for(unsigned i=0;i<f.idxN;++i)if(f.idx[i]==idx)return;if(f.idxN<f.idx.size())f.idx[f.idxN++]=idx;};
    for(unsigned i=0;i<f.n;++i)for(unsigned k=0;k<mj.images;++k){const auto& pair=j.maps.images[k];if(pair.left!=f.images[i])continue;
        add(mj.roleIdx[k]);if(q<query.size())query[q++]=pair.left;if(pair.right&&q<query.size())query[q++]=pair.right;}
    std::array<std::uint32_t,16> alias{};const unsigned na=merge::aliasIndices(query.data(),q,alias);
    if(na>alias.size()||!f.n){f.valid=false;return -1;}
    unsigned extra=0;
    for(unsigned a=0;a<na;++a){bool known=false;for(unsigned k=0;k<mj.images;++k)known|=alias[a]==mj.roleIdx[k]||alias[a]==mj.rightIdx[k];if(!known)++extra;add(alias[a]);}
    auto& ms=merge::mergeStats();if(extra)ms.aliasExtra.fetch_add(extra,std::memory_order_relaxed);
    const unsigned shown=std::min<unsigned>(f.idxN,merge::Stats::lastMax);for(unsigned i=0;i<shown;++i)ms.attachIdx[i].store(f.idx[i],std::memory_order_relaxed);ms.attachIdxCount.store(shown,std::memory_order_relaxed);
    f.valid=true;return int(slot);
}
const std::array<std::uint8_t,32>* dynSlotsFor(MergeJob& mj,VkPipelineLayout layout) noexcept {
    for(unsigned i=0;i<mj.dynCount;++i)if(mj.dyn[i].layout==layout)return mj.dyn[i].ok?&mj.dyn[i].slot:nullptr;
    unsigned i=mj.dynCount<mj.dyn.size()?mj.dynCount++:(mj.dynNext++%unsigned(mj.dyn.size()));
    auto& d=mj.dyn[i];d.layout=layout;d.ok=merge::dynamicSlots(layout,d.slot);return d.ok?&d.slot:nullptr;
}
// [any] after a replayed draw of the open pass (until its first positive or inconclusive check): does the draw sample an
// attachment of the pass's framebuffer? The bindless indices the bound pipeline reads (merge::TexScan refs = set-0 uniform
// binding + member offset) are read from the engine frame ring at the bound set-0 dynamic offsets (left values: the
// right replay reads the same members).
void mergeDrawCheck(Job& j,RecordCursor& rc) noexcept {
    auto& mj=j.merge;
    try{
        if(rc.fbSlot<0){rc.passBits|=merge::BitUnknown;return;}
        const VkPipeline pipeline=rc.sh.state.pipeline;
        auto it=mj.scans.find(pipeline);
        if(it==mj.scans.end()){merge::TexScan scan{};if(!pipeline||!merge::pipelineTextures(pipeline,scan)){scan=merge::TexScan{};scan.unknown=true;}it=mj.scans.emplace(pipeline,scan).first;}
        const merge::TexScan& scan=it->second;
        if(!scan.valid||scan.unknown){rc.passBits|=merge::BitUnknown;return;}
        if(!scan.count)return;
        const Shadow::SetCall* call=nullptr;for(unsigned i=rc.sh.callCount;i-->0;)if(rc.sh.calls[i].first==0){call=&rc.sh.calls[i];break;}
        const auto* slots=call?dynSlotsFor(mj,call->layout):nullptr;
        if(!slots||!j.ring.ringValid||!j.ring.ringMapped){rc.passBits|=merge::BitUnknown;return;}
        const auto& fb=mj.fbs[unsigned(rc.fbSlot)];
        for(unsigned k=0;k<scan.count&&k<scan.refs.size();++k){const auto ref=scan.refs[k];
            const unsigned slot=ref.binding<32?(*slots)[ref.binding]:0xffu;
            if(slot==0xffu||slot>=call->offsetCount){rc.passBits|=merge::BitUnknown;return;}
            const std::uint64_t at=std::uint64_t(call->offsets[slot])+ref.offset;
            if(at+4>j.ring.ringSize){rc.passBits|=merge::BitUnknown;return;}
            const std::uint32_t value=*reinterpret_cast<const volatile std::uint32_t*>(j.ring.ringMapped+at);++mj.ringReads;
            for(unsigned i=0;i<fb.idxN;++i)if(fb.idx[i]==value){rc.passBits|=merge::BitSamples;return;}
        }
    }catch(...){rc.passBits|=merge::BitUnknown;}
}
// M7 callback (MergeScan.h barrierDroppable): true = emitOutsideOp would emit a barrier for this image (an eye image of
// the maps or a foreign scratch target), so a no-op barrier on it is not droppable unless it is an own image.
bool emittedByReplay(VkImage image,const void* ctx) {
    const Job& j=*static_cast<const Job*>(ctx);
    for(unsigned k=0;k<j.maps.imageCount&&k<j.maps.images.size();++k)if(j.maps.images[k].left==image)return true;
    return j.foreign.contains(image);
}
// First failed rule M1..M9 between the pass infos a and b (the splice's own check of every decide() Merge).
merge::Keep mergeRule(const merge::PassInfo& a,const merge::PassInfo& b,std::uint64_t group,unsigned cap) noexcept {
    using merge::Keep;
    if(a.special||b.special)return Keep::Twin;
    if(!a.fb||a.fb!=b.fb)return Keep::Fb;
    if(!a.rp||a.rp!=b.rp)return Keep::Pass;
    if(a.area.offset.x!=b.area.offset.x||a.area.offset.y!=b.area.offset.y||a.area.extent.width!=b.area.extent.width||a.area.extent.height!=b.area.extent.height)return Keep::Area;
    if(!b.loadsAll)return Keep::Loads;
    if(!a.storesAll)return Keep::Stores;
    if(!b.gapDroppable)return Keep::Gap;
    if((a.bits|b.bits)&merge::BitSamples)return Keep::Sample;        // M8 on both sides of the boundary
    if((a.bits|b.bits)&merge::BitUnknown)return Keep::SampleUnknown;
    if(group+b.draws>cap)return Keep::Cap;
    return Keep::Merge;
}
// [render] before the splice of a merging job: one decision per pass entry (merge::decide, re-checked) and the gap entries
// that lie between two merged passes (not emitted). False: no pass entry (nothing to plan).
bool planMerge(Job& j) noexcept {
    auto& mj=j.merge;
    try{
        const Tape& t=*j.tape;const std::size_t n=j.skeleton.size();
        mj.infos.clear();mj.keep.clear();mj.passAt.clear();mj.skip.assign(n,0);
        bool gapOk=true;const MergeJob::Fb* own=nullptr; // the previous pass's framebuffer images (own images of its gap)
        for(std::size_t i=0;i<n;++i){const auto& e=j.skeleton[i];
            const auto* h=reinterpret_cast<const Header*>(t.bytes.data()+e.at);const auto* p=t.bytes.data()+e.at+sizeof(Header);
            if(e.secondary==UINT32_MAX){
                if(static_cast<Op>(h->op)!=Op::PipelineBarrier){gapOk=false;continue;}
                const auto* r=reinterpret_cast<const PBarrier*>(p);const auto* pm=reinterpret_cast<const VkMemoryBarrier*>(r+1);
                const auto* pb=reinterpret_cast<const VkBufferMemoryBarrier*>(pm+r->memory);const auto* pi=reinterpret_cast<const VkImageMemoryBarrier*>(pb+r->buffers);
                if(!merge::barrierDroppable(pi,r->images,r->memory,r->buffers,own?own->images.data():nullptr,own?own->n:0u,&emittedByReplay,&j))gapOk=false;
                continue;}
            const auto* r=reinterpret_cast<const PBeginPass*>(p);
            merge::PassInfo info{};info.fb=e.framebuffer;info.rp=r->pass;info.area=r->area;
            info.loadsAll=(e.passFlags&PassLoadsAll)!=0;info.storesAll=(e.passFlags&PassStoresAll)!=0;
            info.draws=e.draws;info.bits=e.mergeBits;info.gapDroppable=gapOk;info.special=e.part!=0||e.twin!=VK_NULL_HANDLE;
            mj.infos.push_back(info);mj.passAt.push_back(std::uint32_t(i));gapOk=true;
            own=nullptr;for(unsigned f=0;f<mj.fbCount;++f)if(mj.fbs[f].leftFb==e.leftFb&&mj.fbs[f].valid)own=&mj.fbs[f];}
        const unsigned np=unsigned(mj.infos.size());if(!np)return false;
        mj.keep.assign(np,merge::Keep::First);
        merge::decide(mj.infos.data(),np,mj.cap,mj.keep.data());
        std::uint64_t group=mj.infos[0].draws;
        for(unsigned k=1;k<np;++k){
            if(mj.keep[k]==merge::Keep::Merge){const auto rule=mergeRule(mj.infos[k-1],mj.infos[k],group,mj.cap);
                if(rule!=merge::Keep::Merge){mj.keep[k]=rule;merge::decideOverrides.fetch_add(1,std::memory_order_relaxed);}}
            group=mj.keep[k]==merge::Keep::Merge?group+mj.infos[k].draws:mj.infos[k].draws;}
        for(unsigned k=1;k<np;++k)if(mj.keep[k]==merge::Keep::Merge)for(std::uint32_t i=mj.passAt[k-1]+1;i<mj.passAt[k];++i)mj.skip[i]=1;
        return true;
    }catch(...){return false;}
}

void emitPreamble(const Job& j,VkCommandBuffer s,const Shadow& sh) noexcept {
    const auto& raw=rawCommands();
    emitBoundState(s,sh.state,nullptr); // pipeline, vertex/index buffers, viewport/scissor and dynamic state
    for(unsigned i=0;i<sh.callCount;++i){const auto& k=sh.calls[i];std::array<VkDescriptorSet,8> sets=k.sets;for(std::uint32_t n=0;n<k.count;++n)sets[n]=j.sets(sets[n]);
        raw.bindDescriptorSets(s,k.point,k.layout,k.first,k.count,sets.data(),k.offsetCount,k.offsets.data());}
    raw.pushConstants(s,j.layout,VK_SHADER_STAGE_VERTEX_BIT,0,64,j.maps.correction.data());
}
VkCommandBuffer nextSecondary(Job& j){
    Pool& p=*j.pool;
    if(p.used==p.buffers.size()){
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=p.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_SECONDARY;ai.commandBufferCount=4;
        std::array<VkCommandBuffer,4> fresh{};if(j.fn.allocateCommandBuffers(p.device,&ai,fresh.data())!=VK_SUCCESS)return VK_NULL_HANDLE;
        p.buffers.insert(p.buffers.end(),fresh.begin(),fresh.end());counters.allocated+=fresh.size();
    }
    return p.buffers[p.used++];
}

// ---------------------------------------------------------------- prepare [render]
bool prepareJob(Job& j,Stage stage,VkCommandBuffer primary,const FrameMaps& maps,VkPipelineLayout layout,const SetMap& sets,const Tape& t){
    j.decline="NONE";j.rewriter.reset();j.secondaries.clear();j.skeleton.clear();
    if(!rawCommands().executeCommands){j.decline="NO_EXECUTE_COMMANDS";return false;}
    j.fn=functions();
    if(!j.fn.createCommandPool||!j.fn.destroyCommandPool||!j.fn.resetCommandPool||!j.fn.allocateCommandBuffers||!j.fn.beginCommandBuffer||!j.fn.endCommandBuffer){j.decline="NO_POOL_FUNCTIONS";return false;}
    if(maps.queueFamily==UINT32_MAX){j.decline="NO_QUEUE_FAMILY";return false;}
    // ExecuteCommands leaves the primary's bound state undefined: the restore needs the engine's tracked state.
    const BoundState* before=trackedState(primary);if(!before){j.decline="NO_TRACKED_STATE";return false;}
    j.stage=stage;j.primary=primary;j.maps=maps;j.layout=layout;j.sets=sets;j.before=*before;j.tape=&t;
    j.mask=liveMask();replayStats().maskLast.store(j.mask);j.boundary=boundaryRightFlags();setupMerge(j);
    j.foreign=scanForeignTargets(t,maps,stage);
    j.ring=snapshotRing(maps,j.mask);
    Pool& p=pools[maps.owner&1u];
    if(p.pool&&p.device!=maps.device)p=Pool{}; // pool of a destroyed device (teardown hooks normally clear it)
    if(p.pool&&p.family!=maps.queueFamily){j.decline="POOL_FAMILY";return false;}
    if(!p.pool){
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};ci.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;ci.queueFamilyIndex=maps.queueFamily;
        VkCommandPool pool{};if(j.fn.createCommandPool(maps.device,&ci,nullptr,&pool)!=VK_SUCCESS||!pool){j.decline="POOL_CREATE";return false;}
        p.device=maps.device;p.family=maps.queueFamily;p.pool=pool;p.buffers.clear();p.used=0;p.frame=maps.frameSerial;++counters.poolsCreated;
    }else{
        // Once per frame and owner. Legal: an owner is reused only after its previous frame retired on the GPU
        // (EngineStereoPass::retire waits for the game queue fence), and within one frame the depth job's
        // secondaries (already referenced by a primary) are kept for the color job.
        const bool newFrame=maps.frameSerial?p.frame!=maps.frameSerial:(stage==Stage::Depth||p.lastColor);
        if(newFrame){if(j.fn.resetCommandPool(p.device,p.pool,0)!=VK_SUCCESS){j.decline="POOL_RESET";return false;}p.used=0;p.frame=maps.frameSerial;++counters.poolResets;}
    }
    p.lastColor=stage==Stage::Color;j.pool=&p;
    return true;
}

// ---------------------------------------------------------------- record [any]
bool recordBegin(Job& j){
    j.rewriter.emplace(j.stage,j.maps,j.mask,j.layout,&j.sets,j.ring,j.opts);
    j.rc=RecordCursor{};
    if(!j.rc.sh.seed(j.tape->prologue)){j.decline="SHADOW_PROLOGUE";return false;}
    j.rc.begun=true;return true;
}
// Records the tape ops [rc.at, limit) (limit is an op boundary). Pass state, shadow, the open secondary and the
// rewrite state persist in the job between calls, so a streaming job continues exactly where it stopped.
bool recordStep(Job& j,std::size_t limit){
    const auto& raw=rawCommands();auto& st=replayStats();const Tape& t=*j.tape;auto& rw=*j.rewriter;auto& rc=j.rc;
    rw.threadEntered();
    auto& sh=rc.sh;auto& skipDepth=rc.skipDepth;auto& passIndex=rc.passIndex;auto& inPass=rc.inPass;auto& cur=rc.cur;auto& open=rc.open;auto& at=rc.at;
    auto fail=[&](const char* why){j.decline=why;if(inPass&&cur)j.fn.endCommandBuffer(cur);inPass=false;return false;};
    auto& bs=replayStats().boundary;const unsigned si=unsigned(j.stage)&1u;
    // SPLIT: close the open secondary after the split draw (its skeleton entry becomes the head), continue the pass in
    // a new secondary with the same inheritance (P, subpass 0, the resolved framebuffer: valid inside the tail twin, see
    // StereoBoundary.h) and the full bound-state preamble, as at every Begin.
    auto split=[&]()->bool{
        if(j.fn.endCommandBuffer(cur)!=VK_SUCCESS){cur=VK_NULL_HANDLE;return fail("SECONDARY_END");}
        open.part=1;open.twin=rc.head;open.draws=rc.draws;open.mergeBits=rc.passBits;j.skeleton.push_back(open);cur=VK_NULL_HANDLE;
        const VkCommandBuffer next=nextSecondary(j);if(!next)return fail("SECONDARY_ALLOC");
        VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};inherit.renderPass=rc.pass;inherit.subpass=0;inherit.framebuffer=open.framebuffer;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT|VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;begin.pInheritanceInfo=&inherit;
        if(j.fn.beginCommandBuffer(next,&begin)!=VK_SUCCESS)return fail("SECONDARY_BEGIN");
        cur=next;open=SkeletonEntry{open.at,std::uint32_t(j.secondaries.size()),open.framebuffer,false,2,rc.tail};j.secondaries.push_back(cur);
        emitPreamble(j,cur,sh);rw.targetRestarted();rc.splitAt=0;return true;};
    auto drawn=[&]()->bool{ // after every replayed draw
        if(j.merge.on&&!(rc.passBits&(merge::BitSamples|merge::BitUnknown)))mergeDrawCheck(j,rc);
        ++rc.draws;return !(rc.splitAt&&rc.draws==rc.splitAt)||split();};
    RingRegion* const region=j.opts.region; // worker mode only (shared frame region with the live overlap guard)
    auto pressure=[&]{return region&&region->pressure.load(std::memory_order_relaxed);};
    auto engineAbove=[&]{if(region&&region->liveCursor()>region->floor){region->pressure.store(true,std::memory_order_relaxed);return true;}return false;};
    if(engineAbove())return fail("RING_PRESSURE");
    // The storage never moves while a job reads it (reserved before streaming starts). Read the
    // base pointer once per chunk: the vector header shares a cache line with used/commands/draws,
    // which the render thread writes on every push, and a per-op reload (forced by the opaque
    // driver calls below) would pull that line on every op.
    std::uint8_t* const base=const_cast<std::uint8_t*>(t.bytes.data());
    while(at<limit){
        if(j.cancel.load(std::memory_order_relaxed))return fail("CANCELLED");
        if(pressure())return fail("RING_PRESSURE"); // a refused allocation left a draw with left-eye blocks: drop the job
        const auto* h=reinterpret_cast<const Header*>(base+at);const auto opAt=std::uint32_t(at);
        auto* p=base+at+sizeof(Header);at+=sizeof(Header)+h->bytes;
        const auto op=static_cast<Op>(h->op);
        if(skipDepth){ // skipped pass: nothing of it is replayed, not even its state (as inline)
            if(op==Op::BeginRenderPass)++skipDepth;else if(op==Op::EndRenderPass)--skipDepth;
            continue;
        }
        switch(op){
        case Op::BindPipeline:{auto* r=reinterpret_cast<PBindPipeline*>(p);if(r->point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return fail("NON_GRAPHICS_BIND");
            rw.bindPipeline(r->point,r->pipeline);sh.state.pipeline=r->pipeline;sh.state.pipelinePoint=r->point;if(inPass)raw.bindPipeline(cur,r->point,r->pipeline);break;}
        case Op::BindDescriptorSets:{auto* r=reinterpret_cast<PBindSets*>(p);if(r->point!=VK_PIPELINE_BIND_POINT_GRAPHICS)return fail("NON_GRAPHICS_BIND");
            auto* sets=reinterpret_cast<const VkDescriptorSet*>(r+1);auto* offsets=reinterpret_cast<const std::uint32_t*>(sets+r->count);
            if(!sh.pushSets(r->point,r->layout,r->first,r->count,sets,r->offsets,offsets))return fail("SHADOW_SETS");
            rw.bindSets(inPass?cur:VK_NULL_HANDLE,r);break;} // outside a pass: logical state only (re-emitted by the next preamble)
        case Op::BindVertexBuffers:{auto* r=reinterpret_cast<PBindVertices*>(p);auto* buffers=reinterpret_cast<VkBuffer*>(r+1);auto* offsets=reinterpret_cast<VkDeviceSize*>(buffers+r->count);
            if(r->first>=16||r->count>16||r->first+r->count>16)return fail("SHADOW_VERTEX");
            for(std::uint32_t i=0;i<r->count;++i){sh.state.vertexBuffers[r->first+i]=buffers[i];sh.state.vertexOffsets[r->first+i]=offsets[i];sh.state.vertexMask|=1u<<(r->first+i);}
            if(inPass)raw.bindVertexBuffers(cur,r->first,r->count,buffers,offsets);break;}
        case Op::BindIndexBuffer:{auto* r=reinterpret_cast<PBindIndex*>(p);sh.state.indexBuffer=r->buffer;sh.state.indexOffset=r->offset;sh.state.indexType=r->type;sh.state.indexKnown=true;
            if(inPass)raw.bindIndexBuffer(cur,r->buffer,r->offset,r->type);break;}
        case Op::SetViewport:{auto* r=reinterpret_cast<PViewport*>(p);if(r->first!=0||r->count!=1)return fail("SHADOW_VIEWPORT");auto* v=reinterpret_cast<VkViewport*>(r+1);
            sh.state.viewport=v[0];sh.state.viewportKnown=true;if(inPass)raw.setViewport(cur,0,1,v);break;}
        case Op::SetScissor:{auto* r=reinterpret_cast<PScissor*>(p);if(r->first!=0||r->count!=1)return fail("SHADOW_SCISSOR");auto* v=reinterpret_cast<VkRect2D*>(r+1);
            // Moon: the right-eye rectangle (StereoScissor.h) is both emitted and kept for the next preamble
            sh.state.scissor=scissor::forRight(v[0],sh.state.viewportKnown,sh.state.viewport,j.maps.correction.data(),testMode.load(std::memory_order_relaxed)==testModeScissorAsLeft);
            sh.state.scissorKnown=true;if(inPass)raw.setScissor(cur,0,1,&sh.state.scissor);break;}
        case Op::SetLineWidth:{auto* r=reinterpret_cast<PFloat1*>(p);sh.state.lineWidth=r->a;sh.state.lineWidthKnown=true;if(inPass)raw.setLineWidth(cur,r->a);break;}
        case Op::SetDepthBias:{auto* r=reinterpret_cast<PFloat3*>(p);sh.state.depthBias[0]=r->a;sh.state.depthBias[1]=r->b;sh.state.depthBias[2]=r->c;sh.state.depthBiasKnown=true;if(inPass)raw.setDepthBias(cur,r->a,r->b,r->c);break;}
        case Op::SetBlendConstants:{auto* r=reinterpret_cast<PFloat4*>(p);std::memcpy(sh.state.blend,r->v,sizeof sh.state.blend);sh.state.blendKnown=true;if(inPass)raw.setBlendConstants(cur,r->v);break;}
        case Op::SetDepthBounds:{auto* r=reinterpret_cast<PFloat2*>(p);sh.state.bounds[0]=r->a;sh.state.bounds[1]=r->b;sh.state.boundsKnown=true;if(inPass)raw.setDepthBounds(cur,r->a,r->b);break;}
        case Op::SetStencilCompareMask:case Op::SetStencilWriteMask:case Op::SetStencilReference:{auto* r=reinterpret_cast<PStencil*>(p);
            auto& value=op==Op::SetStencilCompareMask?sh.state.stencilCmp:op==Op::SetStencilWriteMask?sh.state.stencilWrite:sh.state.stencilRef;
            auto& known=op==Op::SetStencilCompareMask?sh.state.stencilCmpKnown:op==Op::SetStencilWriteMask?sh.state.stencilWriteKnown:sh.state.stencilRefKnown;
            if(r->face&VK_STENCIL_FACE_FRONT_BIT){value[0]=r->value;known|=1;}if(r->face&VK_STENCIL_FACE_BACK_BIT){value[1]=r->value;known|=2;}
            if(inPass){if(op==Op::SetStencilCompareMask)raw.setStencilCompareMask(cur,r->face,r->value);else if(op==Op::SetStencilWriteMask)raw.setStencilWriteMask(cur,r->face,r->value);else raw.setStencilReference(cur,r->face,r->value);}
            break;}
        case Op::PushConstants:return fail("PUSH_CONSTANTS"); // the engine's own push constants are not shadowed
        case Op::BeginRenderPass:{auto* r=reinterpret_cast<PBeginPass*>(p);VkFramebuffer fb{};
            if(inPass)return fail("NESTED_PASS");
            const unsigned thisPass=passIndex++;
            if(j.stage==Stage::Color&&thisPass<16&&(j.mask&(1u<<thisPass))){skipDepth=1;++st.maskPassesSkipped;break;}
            const int resolved=resolveRightFramebuffer(j.maps,r->pass,r->framebuffer,fb);
            if(resolved==0){skipDepth=1;++st.passesSkipped;break;}
            if(resolved!=1)return fail("FRAMEBUFFER_UNRESOLVED");
            if(r->contents!=VK_SUBPASS_CONTENTS_INLINE)return fail("PASS_CONTENTS");
            cur=nextSecondary(j);if(!cur)return fail("SECONDARY_ALLOC");
            VkCommandBufferInheritanceInfo inherit{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};inherit.renderPass=r->pass;inherit.subpass=0;inherit.framebuffer=fb;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT|VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;begin.pInheritanceInfo=&inherit;
            if(j.fn.beginCommandBuffer(cur,&begin)!=VK_SUCCESS){cur=VK_NULL_HANDLE;return fail("SECONDARY_BEGIN");}
            rc.thisPass=thisPass;rc.pass=r->pass;rc.draws=0;rc.splitAt=0;rc.head=VK_NULL_HANDLE;rc.tail=VK_NULL_HANDLE;VkRenderPass extra{};
            if(j.boundary){PassTwins tw{};const bool have=twinsFor(r->pass,tw);
                if(j.boundary&BoundaryExtraR){if(!have||!tw.empty)++bs.noTwins;else if(!(tw.flags&PassFinalIsReference))++bs.extraLayout;else extra=tw.empty;}
                if(j.boundary&BoundarySplitR){
                    const unsigned prev=thisPass<boundary::prevDrawPasses?bs.prevDraws[si*boundary::prevDrawPasses+thisPass].load(std::memory_order_relaxed):0u;
                    if(!have||!tw.head||!tw.tail)++bs.noTwins;else if(!(rc.splitAt=boundary::splitPoint(prev)))++bs.fewDraws;else{rc.head=tw.head;rc.tail=tw.tail;}}}
            inPass=true;open=SkeletonEntry{opAt,std::uint32_t(j.secondaries.size()),fb,false,0,extra};j.secondaries.push_back(cur);
            rc.passBits=0;rc.fbSlot=-1;
            if(j.merge.on){PassTwins tw{};open.leftFb=r->framebuffer;open.passFlags=twinsFor(r->pass,tw)?tw.flags:0u;rc.fbSlot=fbSlotFor(j,r->framebuffer);}
            emitPreamble(j,cur,sh);rw.targetRestarted();
            ++st.passesReplayed;break;}
        case Op::EndRenderPass:{if(!inPass){++st.endsSkipped;break;}
            inPass=false;if(j.fn.endCommandBuffer(cur)!=VK_SUCCESS)return fail("SECONDARY_END");
            if(rc.thisPass<boundary::prevDrawPasses)bs.prevDraws[si*boundary::prevDrawPasses+rc.thisPass].store(std::uint16_t(std::min(rc.draws,65535u)),std::memory_order_relaxed);
            open.draws=rc.draws;open.mergeBits=rc.passBits;
            j.skeleton.push_back(open);cur=VK_NULL_HANDLE;break;}
        case Op::NextSubpass:{if(!inPass){++st.drawsSkippedOutsidePass;break;}return fail("NEXT_SUBPASS");}
        case Op::Draw:{if(!inPass){++st.drawsSkippedOutsidePass;break;}if(j.mask&(1u<<16)){++st.maskDrawsSkipped;break;}if(eyecull::tape::dropRight(h->pad,rc.ecRight))break;
            if(rw.classifyDraw(passIndex)){++st.maskDrawsSkipped;break;}rw.rewriteForDraw(cur);auto* r=reinterpret_cast<PDraw*>(p);raw.draw(cur,r->vertices,r->instances,r->firstVertex,r->firstInstance);if(!drawn())return false;break;}
        case Op::DrawIndexed:{if(!inPass){++st.drawsSkippedOutsidePass;break;}if((j.mask&(1u<<17))&&j.stage==Stage::Color&&passIndex>1){++st.maskDrawsSkipped;break;}if(eyecull::tape::dropRight(h->pad,rc.ecRight))break;
            if(rw.classifyDraw(passIndex)){++st.maskDrawsSkipped;break;}rw.rewriteForDraw(cur);auto* r=reinterpret_cast<PDrawIndexed*>(p);raw.drawIndexed(cur,r->indices,r->instances,r->firstIndex,r->vertexOffset,r->firstInstance);if(!drawn())return false;break;}
        case Op::DrawIndirect:{if(!inPass){++st.drawsSkippedOutsidePass;break;}if(rw.classifyDraw(passIndex)){++st.maskDrawsSkipped;break;}rw.rewriteForDraw(cur);auto* r=reinterpret_cast<PIndirect*>(p);raw.drawIndirect(cur,r->buffer,r->offset,r->count,r->stride);if(!drawn())return false;break;}
        case Op::DrawIndexedIndirect:{if(!inPass){++st.drawsSkippedOutsidePass;break;}if(rw.classifyDraw(passIndex)){++st.maskDrawsSkipped;break;}rw.rewriteForDraw(cur);auto* r=reinterpret_cast<PIndirect*>(p);raw.drawIndexedIndirect(cur,r->buffer,r->offset,r->count,r->stride);if(!drawn())return false;break;}
        case Op::ClearAttachments:{if(!inPass){++st.drawsSkippedOutsidePass;break;}auto* r=reinterpret_cast<PClearAttachments*>(p);auto* pa=reinterpret_cast<VkClearAttachment*>(r+1);
            raw.clearAttachments(cur,r->attachments,pa,r->rects,reinterpret_cast<VkClearRect*>(pa+r->attachments));break;}
        case Op::PipelineBarrier:{if(inPass){if(j.streamed)return fail("BARRIER_IN_PASS_STREAMED");emitOutsideOp(cur,op,p,j.maps,j.foreign);break;} // subpass self-dependency: stays in the pass
            j.skeleton.push_back(SkeletonEntry{opAt});break;}
        case Op::CopyImage:case Op::CopyBuffer:case Op::CopyBufferToImage:case Op::BlitImage:case Op::ClearColorImage:case Op::ClearDepthStencilImage:
        case Op::ResolveImage:case Op::FillBuffer:case Op::UpdateBuffer:{if(inPass)return fail("TRANSFER_IN_PASS");j.skeleton.push_back(SkeletonEntry{opAt});break;}
        case Op::CopyImageToBuffer:break; // readbacks are never replayed
        default:return fail("UNKNOWN_OP");
        }
    }
    if(pressure()||engineAbove())return fail("RING_PRESSURE");
    return true;
}
bool recordFinish(Job& j){
    auto& rc=j.rc;
    if(rc.inPass){ // the last pass is usually still open at the end of the recording window (auto-closed, as inline)
        rc.inPass=false;if(j.fn.endCommandBuffer(rc.cur)!=VK_SUCCESS){j.decline="SECONDARY_END";return false;}
        if(rc.thisPass<boundary::prevDrawPasses)replayStats().boundary.prevDraws[(unsigned(j.stage)&1u)*boundary::prevDrawPasses+rc.thisPass].store(std::uint16_t(std::min(rc.draws,65535u)),std::memory_order_relaxed);
        rc.open.autoClosed=true;rc.open.draws=rc.draws;rc.open.mergeBits=rc.passBits;j.skeleton.push_back(rc.open);
    }
    return true;
}
bool recordJob(Job& j){return recordBegin(j)&&recordStep(j,j.tape->used)&&recordFinish(j);}

// ---------------------------------------------------------------- splice [render]
SecondaryOutcome spliceJob(Job& j) noexcept {
    const auto& raw=rawCommands();auto& st=replayStats();const auto c=j.primary;const Tape& t=*j.tape;
    bool emitted=false,inPass=false;
    try{
        emitReplayHead(c,j.stage,j.maps,j.layout,j.mask);emitted=true;
        // STEREO TEST 20 (window a): consecutive passes that decide() merges share ONE render pass instance:
        // Begin(P, SECONDARY contents, the first pass's clear values) -> ExecuteCommands(S_a..S_b) -> End; the gap entries
        // between merged passes (layout no-op barriers, rule M7) are not emitted. Without merging: one instance per pass.
        auto& mj=j.merge;const bool merging=mj.on&&planMerge(j);
        auto& ms=merge::mergeStats();auto& batch=mj.batch;batch.clear();
        std::uint32_t groupDraws=0;unsigned passEntry=0;std::array<std::uint32_t,merge::Stats::lastMax> groups{};unsigned groupCount=0;
        auto closeGroup=[&]{ // merging: the open group's secondaries and its End
            if(!inPass)return;
            if(!batch.empty()){raw.executeCommands(c,std::uint32_t(batch.size()),batch.data());++counters.executeCalls;batch.clear();}
            raw.endRenderPass(c);inPass=false;
            ms.noteGroup(groupDraws);if(groupCount<groups.size())groups[groupCount]=groupDraws;++groupCount;};
        for(std::size_t i=0;i<j.skeleton.size();++i){const auto& e=j.skeleton[i];
            const auto* h=reinterpret_cast<const Header*>(t.bytes.data()+e.at);const auto* p=t.bytes.data()+e.at+sizeof(Header);
            if(e.secondary==UINT32_MAX){
                if(merging){if(mj.skip[i]){merge::gapEntriesSkipped.fetch_add(1,std::memory_order_relaxed);continue;}closeGroup();}
                emitOutsideOp(c,static_cast<Op>(h->op),p,j.maps,j.foreign);continue;}
            const auto* r=reinterpret_cast<const PBeginPass*>(p);
            if(merging){const unsigned k=passEntry++;
                if(inPass&&k<mj.keep.size()&&mj.keep[k]==merge::Keep::Merge){ // continues the open group
                    batch.push_back(j.secondaries[e.secondary]);groupDraws+=e.draws;
                    ms.merged.fetch_add(1,std::memory_order_relaxed);st.boundary.insertedThisFrame.fetch_add(1,std::memory_order_relaxed); // _m20a "inserted" = boundaries removed
                    if(e.autoClosed)++st.passesAutoClosed;continue;}
                closeGroup();
                if(!e.part&&!e.twin){ // opens a group (its End is emitted by the next kept boundary / gap op / the end)
                    VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=r->pass;b.framebuffer=e.framebuffer;b.renderArea=r->area;
                    b.clearValueCount=r->clears;b.pClearValues=reinterpret_cast<const VkClearValue*>(r+1);
                    raw.beginRenderPass(c,&b,VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);inPass=true;
                    batch.push_back(j.secondaries[e.secondary]);groupDraws=e.draws;
                    if(e.autoClosed)++st.passesAutoClosed;continue;}
            }
            // A split pass is begun as its head (P's clear values) and its tail (after the store->load memory
            // barrier; no CLEAR in a tail, clearValueCount 0); both carry the resolved framebuffer of P.
            const bool tail=e.part==2;
            if(tail)emitBoundaryBarrier(c);
            VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};b.renderPass=e.part&&e.twin?e.twin:r->pass;b.framebuffer=e.framebuffer;b.renderArea=r->area;
            if(!tail){b.clearValueCount=r->clears;b.pClearValues=reinterpret_cast<const VkClearValue*>(r+1);}
            raw.beginRenderPass(c,&b,VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);inPass=true;
            raw.executeCommands(c,1,&j.secondaries[e.secondary]);++counters.executeCalls;
            raw.endRenderPass(c);inPass=false;
            if(tail){++st.boundary.splitsR;st.boundary.insertedThisFrame.fetch_add(1,std::memory_order_relaxed);}
            else if(!e.part&&e.twin){emitBoundaryBarrier(c);emitEmptyTwin(c,e.twin,e.framebuffer,r->area);
                ++st.boundary.extraR;st.boundary.insertedThisFrame.fetch_add(1,std::memory_order_relaxed);}
            if(e.autoClosed)++st.passesAutoClosed;
        }
        if(merging){closeGroup();
            const unsigned np=unsigned(mj.infos.size());unsigned sampling=0;
            ms.jobs.fetch_add(1,std::memory_order_relaxed);ms.passes.fetch_add(np,std::memory_order_relaxed);ms.boundaries.fetch_add(np?np-1:0,std::memory_order_relaxed);
            for(unsigned k=0;k<np;++k){if(k)ms.noteKept(mj.keep[k]);
                if(k<merge::Stats::passBitsMax)ms.lastPassBits[k].store(std::uint8_t(mj.infos[k].bits&3u),std::memory_order_relaxed); // Per-pass sample hits
                if(mj.infos[k].bits&merge::BitSamples){if(sampling<merge::Stats::lastMax)ms.lastSampling[sampling].store(k,std::memory_order_relaxed);++sampling;}}
            ms.lastPassCount.store(std::min<unsigned>(np,merge::Stats::passBitsMax),std::memory_order_relaxed);
            ms.samplingPasses.fetch_add(sampling,std::memory_order_relaxed);ms.lastSamplingCount.store(std::min<unsigned>(sampling,merge::Stats::lastMax),std::memory_order_relaxed);
            const unsigned shown=std::min<unsigned>(groupCount,merge::Stats::lastMax);for(unsigned k=0;k<shown;++k)ms.lastGroups[k].store(groups[k],std::memory_order_relaxed);
            ms.lastGroupCount.store(shown,std::memory_order_relaxed);ms.ringReads.fetch_add(mj.ringReads,std::memory_order_relaxed);mj.ringReads=0;}
        const bool dumpNow=j.stage==Stage::Color&&pollEyeDump();
        emitReplayTail(c,j.stage,j.maps,j.layout,j.mask,j.before,t,dumpNow);
        j.rewriter->finish();
        eyecull::tape::commitRight(j.rc.ecRight); // Only a spliced job's right drops count
        return SecondaryOutcome::Done;
    }catch(...){
        if(!emitted){j.decline="SPLICE_EXCEPTION";return SecondaryOutcome::Declined;}
        try{if(inPass)raw.endRenderPass(c);raw.pushConstants(c,j.layout,VK_SHADER_STAGE_VERTEX_BIT,0,64,identity);emitBoundState(c,j.before,nullptr);}catch(...){}
        return SecondaryOutcome::Faulted;
    }
}

// ================================================================= worker mode (async=2)
// One persistent worker thread (created on the first launch), a job slot per stage and a per-frame context
// (shared ring region). All slot/context fields are guarded by Worker::m; the worker only touches Queued slots
// (and the job it runs), the render thread owns Idle slots (launch) and Done/Declined ones (splice).
enum SlotState : int {Idle,Queued,RunningWorker,RunningRender,Done,Declined,Discarded,Consumed};
struct Slot {int state{Idle};Job job;bool invalid{};const char* invalidWhy{"NONE"};std::uint64_t serial{},token{};
    // A streaming job is launched at the stage start; endRecording finalizes it (final length, primary, foreign targets).
    bool finalized{};std::size_t finalOffset{};VkCommandBuffer primary{};ForeignTargets foreign{};std::uint64_t workerNs{};};
struct FrameContext {bool active{};std::uint64_t serial{};RingRegion region;bool regionValid{},ringTight{};bool relaunch[2]{};};
struct Worker {
    std::mutex m;std::condition_variable cv;std::thread* thread{};bool stop{},paused{};
    Slot slots[2];FrameContext ctx;Pool pools[2][2]; // pools[owner][stage]: a stage's job never shares a pool with the other stage
    std::atomic<bool> active{};                       // ctx.active, readable without the lock
    std::atomic<bool> sleeping{};                     // the worker waits for published ops (publishers notify only then)
    unsigned waiters{};                               // Render-thread (and test) waits on cv: the worker notifies only then
};
template<class Pred> void waitWorker(Worker& w,std::unique_lock<std::mutex>& lk,Pred pred){++w.waiters;w.cv.wait(lk,pred);--w.waiters;}
// Never destroyed: no static destructor may run while the worker waits on its condition variable at process exit.
Worker& W() noexcept {static Worker* w=new Worker;return *w;}
std::atomic<int> workerNice{INT32_MIN},workerNiceErrno{};
std::atomic<unsigned> workerAffinity{},workerCapacityMin{},workerCapacityMax{};std::atomic<int> workerAffinityErrno{};
// Engine ring usage: the right-eye region must stay above the engine's own (bottom-up) allocations. The high water
// is windowed: the peak engine cursor of each of the last 8 worker-mode frames (a spike raises the floor for at most
// 8 frames), reset when the ring configuration changes (another ring size, a known frame object with another mapping,
// more than 4 distinct frame objects) and at device teardown. All observations come from the render thread.
struct RingWatch {
    std::mutex m;
    std::array<std::pair<std::uintptr_t,std::uintptr_t>,4> frames{};unsigned frameCount{};std::uint32_t size{};
    std::array<std::uint64_t,8> peaks{};unsigned peakCount{},peakNext{};std::uint64_t framePeak{},lastUsage{};
    void resetLocked() noexcept {frames={};frameCount=0;size=0;peaks={};peakCount=0;peakNext=0;framePeak=0;lastUsage=0;}
    void reset() noexcept {try{std::lock_guard lock(m);resetLocked();}catch(...){}}
    // returns true when the ring configuration changed (window reset)
    bool observe(std::uintptr_t framePtr,std::uintptr_t mapped,std::uint32_t ringSize,std::uint64_t cursor) noexcept {
        try{std::lock_guard lock(m);
            bool known=false,changed=size&&ringSize!=size;
            for(unsigned i=0;i<frameCount;++i)if(frames[i].first==framePtr){known=true;changed|=frames[i].second!=mapped;}
            if(!known&&frameCount==frames.size())changed=true;
            if(changed)resetLocked();
            if(!known||changed)frames[frameCount++]={framePtr,mapped};
            size=ringSize;framePeak=std::max(framePeak,cursor);return changed;
        }catch(...){return false;}
    }
    std::uint64_t highWater() noexcept {
        try{std::lock_guard lock(m);std::uint64_t high=framePeak;for(unsigned i=0;i<peakCount;++i)high=std::max(high,peaks[i]);return high;}catch(...){return 0;}
    }
    void frameEnded(std::uint64_t regionUsage) noexcept { // UINT64_MAX: no region this frame (keep the last usage)
        try{std::lock_guard lock(m);peaks[peakNext]=framePeak;peakNext=(peakNext+1)%peaks.size();peakCount=std::min<unsigned>(peakCount+1,unsigned(peaks.size()));
            framePeak=0;if(regionUsage!=UINT64_MAX)lastUsage=regionUsage;}catch(...){}
    }
    std::uint64_t usage() noexcept {try{std::lock_guard lock(m);return lastUsage;}catch(...){return 0;}}
} ringWatch;
std::atomic<std::int64_t> ringMarginMin{INT64_MAX};std::atomic<std::uint64_t> regionFloorLast{};
constexpr std::uint64_t highWaterMargin=256u<<10;
void observeEngineCursor(std::uintptr_t framePtr,std::uintptr_t mapped,std::uint32_t size,std::uint64_t cursor,const RingRegion* region) noexcept {
    if(ringWatch.observe(framePtr,mapped,size,cursor))++counters.ringWatchResets;
    if(!region)return;
    const auto margin=std::int64_t(region->top.load(std::memory_order_relaxed))-std::int64_t(cursor); // region bottom - engine cursor
    std::int64_t low=ringMarginMin.load(std::memory_order_relaxed);while(margin<low&&!ringMarginMin.compare_exchange_weak(low,margin,std::memory_order_relaxed)){}
    if(cursor>region->floor)++counters.floorViolations;
}
std::atomic<unsigned> workerDelayUs{}; // host tests only: hold a running job (exercises the splice's wait)
constexpr std::uint64_t regionHeadroom=65536; // = maxUniformRange of the inline rewrite (offset+range <= ring size)
// reason tables (static strings)
struct ReasonTable {
    std::mutex m;std::array<std::pair<const char*,std::uint64_t>,32> rows{};unsigned count{};
    void note(const char* why) noexcept {
        try{std::lock_guard lock(m);
            for(unsigned i=0;i<count;++i)if(rows[i].first==why||!std::strcmp(rows[i].first,why)){++rows[i].second;return;}
            if(count<rows.size())rows[count++]={why,1};}catch(...){}
    }
    std::string json(){std::ostringstream o;o<<"{";std::lock_guard lock(m);for(unsigned i=0;i<count;++i)o<<(i?",":"")<<"\""<<rows[i].first<<"\":"<<rows[i].second;o<<"}";return o.str();}
};
ReasonTable notLaunchedReasons,      // final launch attempt of a stage failed (the stage replays inline)
            streamNotLaunchedReasons,// stage-start (streaming) launch failed; the stage end retries
            streamDeclinedReasons;   // the worker declined a streamed job before the stage ended
void noteReason(const char* why) noexcept {notLaunchedReasons.note(why);}
std::string notLaunchedReport(){return notLaunchedReasons.json();}
bool streamingSpecific(const char* why) noexcept {return !std::strcmp(why,"BARRIER_IN_PASS_STREAMED");} // a complete tape can do it
bool workerRunning() noexcept {auto& w=W();std::lock_guard lock(w.m);return w.thread!=nullptr;}

// Runnable: queued and either finalized (rest of the tape + end) or with newly published ops (streaming).
bool runnable(const Slot& s) noexcept {
    return s.state==Queued&&!s.invalid&&(s.finalized||s.job.tape->published.load(std::memory_order_seq_cst)>s.job.rc.at);
}
struct Tuning {std::atomic<unsigned> publish{16},wake{0},pollUs{250},streamMask{3},affinityMask{};std::atomic<int> nice{-10},streamNice{0},affinity{0};std::atomic<bool> forced{};} tuning;
enum AffinityPolicy : int {AffinityAuto,AffinityBig,AffinityMask,AffinityNone,AffinityLittle,AffinityCore2,AffinityAuto05};
std::atomic<int> affinityPolicy{AffinityAuto};std::atomic<unsigned> affinityRequested{},affinityBase{},affinityAll{};
const char* policyName(int p) noexcept {
    switch(p){case AffinityBig:return "big";case AffinityMask:return "mask";case AffinityNone:return "none";case AffinityLittle:return "little";
              case AffinityCore2:return "core2";case AffinityAuto05:return "auto05";default:return "auto";}
}
// Effective worker settings for a STEREO TEST mode (menu first, then the properties).
struct Placement {int policy{AffinityAuto};unsigned mask{};int boostNice{-10},streamNice{0};};
Placement placementFor(unsigned menu) noexcept {
    Placement p{tuning.affinity.load(std::memory_order_relaxed),tuning.affinityMask.load(std::memory_order_relaxed),tuning.nice.load(std::memory_order_relaxed),tuning.streamNice.load(std::memory_order_relaxed)};
    switch(menu){
    case testModeWorkerLittle:p.policy=AffinityLittle;break;
    case testModeWorkerNice0:p.boostNice=0;p.streamNice=0;break;
    case testModeWorkerCore2:p.policy=AffinityCore2;break;
    case testModeWorkerAsync05:p.policy=AffinityAuto05;p.boostNice=-10;p.streamNice=-10;break;
    default:break;
    }
    return p;
}
unsigned effectiveStreamMask() noexcept { // [render] at launch
    return testMode.load(std::memory_order_relaxed)==testModeWorkerColorEnd?1u:tuning.streamMask.load(std::memory_order_relaxed);
}
constexpr unsigned cpuSlots=8;
struct CpuStats {
    // written by the render thread
    std::atomic<std::uint64_t> render[cpuSlots]{};                      // CPU at each left-stage start
    std::atomic<std::uint64_t> renderStages{},renderMigrations{},renderNivcsw{},renderNvcsw{}; // per left stage (begin..end)
    std::atomic<std::uint64_t> coldWaits{},wakes{};
    std::atomic<unsigned> renderSeen{},renderMask{};std::atomic<int> renderStageCpu{-1},renderLastCpu{-1};
    std::atomic<int> renderPolicy{-1},renderRtPriority{-1},renderNice{INT32_MIN};
    // the render thread's current CPU (stage begin/end and every streaming publication): read by the worker per chunk
    alignas(64) std::atomic<int> renderNowCpu{-1};
    // written by the worker
    alignas(64) std::atomic<std::uint64_t> worker[cpuSlots]{};          // per chunk
    std::atomic<std::uint64_t> inStageChunks{},sameCpu{},polls{},affinityChanges{},follows{},niceChanges{};
    std::atomic<std::uint64_t> freqKhzSum[cpuSlots]{},freqSamples[cpuSlots]{};std::atomic<int> freqErrno{};
    std::atomic<int> policy{AffinityAuto},appliedMode{-1},boostNice{-10},streamNice{0},workerPolicy{-1};
    std::atomic<unsigned> requested{},inherited{},online{},big{},little{},mainMask{};std::atomic<unsigned> capacity[cpuSlots]{};
} cpuStats;
// ---- per-STEREO-TEST-mode accumulation (report async.byMode). The mode of a frame is the menu value at the depth
// stage start; the first 4 frames after a switch (worker settings, cadence) and frames switched mid-way are not
// accumulated (counted as settling).
constexpr unsigned settleFrames=4;
struct ModeStats {
    std::atomic<std::uint64_t> frames{},settling{},depthCpuNs{},depthObjects{},colorCpuNs{},colorObjects{};  // drawStageTimings (DETAIL profiler)
    std::atomic<std::uint64_t> stageFrames{},depthStageNs{},colorStageNs{},stereoDrawFrames{},stereoDrawNs{}; // threadCpuTimings stockDepth/stockColor/stereoDraw
    std::atomic<std::uint64_t> jobs{},waited{},joinWaitNs{},tails{},tailNs{},takeovers{},fallbacks{};       // splices (render thread)
    std::atomic<std::uint64_t> stages{},nivcsw{},migrations{};                                               // left stages (render thread)
    std::atomic<std::uint64_t> chunks{},sameCpu{},workerNs{};                                                // worker
};
std::array<ModeStats,testModeCount> byMode;
std::atomic<int> frameSlot{-1}; // byMode index of the current frame (-1: settling)
ModeStats* slotStats() noexcept {const int s=frameSlot.load(std::memory_order_relaxed);return s>=0&&s<int(testModeCount)?&byMode[unsigned(s)]:nullptr;}
struct FrameTrack {unsigned mode{UINT32_MAX},stable{};std::uint64_t serial[5]{};} frameTrack; // render thread only
#ifdef __ANDROID__
unsigned propUnsigned(const char* name,unsigned fallback,unsigned high) noexcept {
    char v[PROP_VALUE_MAX]{};if(__system_property_get(name,v)<=0||!v[0])return fallback;
    char* e=nullptr;const unsigned long x=std::strtoul(v,&e,0);if(e==v)return fallback;return unsigned(std::min<unsigned long>(x,high));
}
int propInt(const char* name,int fallback,int low,int high) noexcept {
    char v[PROP_VALUE_MAX]{};if(__system_property_get(name,v)<=0||!v[0])return fallback;
    char* e=nullptr;const long x=std::strtol(v,&e,0);if(e==v)return fallback;return int(std::clamp<long>(x,low,high));
}
void readAffinityProperty() noexcept {
    char v[PROP_VALUE_MAX]{};const int len=__system_property_get("debug.rdrvr.stereoreplay.affinity",v);
    int policy=AffinityAuto;unsigned requested=0;
    if(len>0&&v[0]&&std::strcmp(v,"auto")){
        if(!std::strcmp(v,"big"))policy=AffinityBig;else if(!std::strcmp(v,"none"))policy=AffinityNone;else if(!std::strcmp(v,"little"))policy=AffinityLittle;
        else if(!std::strcmp(v,"core2"))policy=AffinityCore2;else if(!std::strcmp(v,"auto05"))policy=AffinityAuto05;
        else{char* e=nullptr;const unsigned long x=std::strtoul(v,&e,16);if(e!=v){if(!x)policy=AffinityNone;else{policy=AffinityMask;requested=unsigned(x);}}}
    }
    tuning.affinity.store(policy,std::memory_order_relaxed);tuning.affinityMask.store(requested,std::memory_order_relaxed);
}
unsigned maskOf(const cpu_set_t& set) noexcept {unsigned m=0;for(unsigned cpu=0;cpu<32;++cpu)if(CPU_ISSET(cpu,&set))m|=1u<<cpu;return m;}
// worker thread only
struct WorkerCpu {bool init{};unsigned inherited{},online{},big{},little{},big05{},current{},prevSeen{},followBase{};unsigned caps[32]{};unsigned low{},high{};
                  int fds[cpuSlots]{};unsigned cpus{};bool follow{};int nice{INT32_MIN};Placement placement{};};
WorkerCpu& workerCpu() noexcept {static WorkerCpu c;return c;}
void workerCpuInit() noexcept {
    auto& c=workerCpu();if(c.init)return;c.init=true;
    cpu_set_t set;CPU_ZERO(&set);
    if(sched_getaffinity(0,sizeof set,&set)==0)c.inherited=maskOf(set); // the render thread's set (the worker is created by it)
    CPU_ZERO(&set);if(sched_getaffinity(getpid(),sizeof set,&set)==0)cpuStats.mainMask.store(maskOf(set)); // the process main thread's set
    if(FILE* f=std::fopen("/sys/devices/system/cpu/online","r")){ // "0-5" / "0-3,5"
        char buf[64]{};if(std::fgets(buf,sizeof buf,f)){const char* p=buf;
            while(*p){char* e=nullptr;const unsigned long a=std::strtoul(p,&e,10);if(e==p)break;unsigned long b=a;p=e;
                if(*p=='-'){b=std::strtoul(p+1,&e,10);p=e;}for(unsigned long cpu=a;cpu<=b&&cpu<32;++cpu)c.online|=1u<<cpu;if(*p==',')++p;else break;}}
        std::fclose(f);}
    unsigned n=0;c.low=UINT32_MAX;
    for(unsigned cpu=0;cpu<32;++cpu){char path[96];std::snprintf(path,sizeof path,"/sys/devices/system/cpu/cpu%u/cpu_capacity",cpu);
        FILE* f=std::fopen(path,"r");if(!f)break;unsigned v=0;if(std::fscanf(f,"%u",&v)!=1)v=0;std::fclose(f);c.caps[cpu]=v;n=cpu+1;
        if(cpu<cpuSlots)cpuStats.capacity[cpu].store(v);if(v){c.low=std::min(c.low,v);c.high=std::max(c.high,v);}}
    if(!c.online)for(unsigned cpu=0;cpu<std::max(n,1u);++cpu)c.online|=1u<<cpu;
    if(!c.inherited)c.inherited=c.online;
    if(n&&c.high){workerCapacityMin.store(c.low);workerCapacityMax.store(c.high);}
    c.big=c.online;c.little=0;
    if(n&&c.high>c.low){c.big=0;for(unsigned cpu=0;cpu<n;++cpu){if(!(c.online&(1u<<cpu)))continue;if(c.caps[cpu]>c.low)c.big|=1u<<cpu;else if(c.caps[cpu])c.little|=1u<<cpu;}}
    c.big05=c.big&c.inherited;if(!c.big05)c.big05=c.inherited; // Big CPUs within the inherited set (the whole inherited set if none)
    cpuStats.inherited.store(c.inherited);cpuStats.online.store(c.online);cpuStats.big.store(c.big);cpuStats.little.store(c.little);
    affinityAll.store(c.inherited);affinityBase.store(c.big05);
    const long conf=sysconf(_SC_NPROCESSORS_CONF);c.cpus=unsigned(std::clamp<long>(conf,1,long(cpuSlots)));
    for(unsigned cpu=0;cpu<cpuSlots;++cpu){c.fds[cpu]=-1;if(cpu>=c.cpus)continue;char path[96];std::snprintf(path,sizeof path,"/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq",cpu);
        c.fds[cpu]=open(path,O_RDONLY|O_CLOEXEC);if(c.fds[cpu]<0&&!cpuStats.freqErrno.load(std::memory_order_relaxed))cpuStats.freqErrno.store(errno?errno:-1);}
    c.nice=getpriority(PRIO_PROCESS,gettid());workerNice.store(c.nice);cpuStats.workerPolicy.store(sched_getscheduler(0)); // inherited from the render thread
    errno=0;
}
void sampleFrequencies() noexcept {
    auto& c=workerCpu();
    for(unsigned cpu=0;cpu<c.cpus;++cpu){if(c.fds[cpu]<0)continue;char buf[32];const ssize_t r=pread(c.fds[cpu],buf,sizeof buf-1,0);
        if(r<=0)continue;buf[r]=0;const unsigned long khz=std::strtoul(buf,nullptr,10);if(!khz)continue;
        cpuStats.freqKhzSum[cpu].fetch_add(khz,std::memory_order_relaxed);cpuStats.freqSamples[cpu].fetch_add(1,std::memory_order_relaxed);}
    errno=0;
}
bool pinWorker(unsigned want) noexcept { // worker thread; the effective mask (cpuset-restricted) is read back
    auto& c=workerCpu();if(!want)return false;
    cpu_set_t set;CPU_ZERO(&set);for(unsigned cpu=0;cpu<32;++cpu)if(want&(1u<<cpu))CPU_SET(cpu,&set);
    errno=0;
    if(sched_setaffinity(0,sizeof set,&set)!=0){workerAffinityErrno.store(errno);errno=0;return false;}
    CPU_ZERO(&set);const unsigned effective=sched_getaffinity(0,sizeof set,&set)==0?maskOf(set):want;
    c.current=effective;workerAffinity.store(effective);cpuStats.affinityChanges.fetch_add(1,std::memory_order_relaxed);errno=0;return true;
}
void applyPlacement(unsigned menu) noexcept { // worker thread: re-resolves the policy (mode change / property refresh)
    auto& c=workerCpu();const Placement p=placementFor(menu);c.placement=p;c.follow=false;
    const unsigned seen=cpuStats.renderSeen.exchange(0,std::memory_order_relaxed),recent=seen|c.prevSeen;c.prevSeen=seen;
    const int now=cpuStats.renderNowCpu.load(std::memory_order_relaxed);const unsigned nowBit=now>=0&&now<32?1u<<now:0u;
    unsigned want=0;
    switch(p.policy){
    case AffinityNone:want=c.current?c.online:0;break; // undo an earlier pin; never pinned: leave the thread alone
    case AffinityBig:want=c.big05;break;
    case AffinityMask:want=p.mask;break;
    case AffinityLittle:want=c.little;break;
    case AffinityCore2:want=(c.online&4u)?(c.caps[2]>c.low||!c.little?4u:4u|c.little):c.little;break;
    case AffinityAuto05:{want=c.big05&~recent;if(!want)want=c.inherited&~recent;
        const int last=cpuStats.renderLastCpu.load(std::memory_order_relaxed);if(!want&&last>=0&&last<32)want=c.big05&~(1u<<last);if(!want)want=c.big05;break;}
    default:c.follow=true;c.followBase=c.inherited;want=c.inherited&~nowBit;if(!want)want=c.inherited;break;
    }
    cpuStats.policy.store(p.policy,std::memory_order_relaxed);cpuStats.requested.store(want,std::memory_order_relaxed);
    cpuStats.appliedMode.store(int(menu),std::memory_order_relaxed);cpuStats.boostNice.store(p.boostNice);cpuStats.streamNice.store(p.streamNice);
    affinityPolicy.store(p.policy,std::memory_order_relaxed);affinityRequested.store(p.policy==AffinityMask?p.mask:0u,std::memory_order_relaxed);
    if(want&&want!=c.current)pinWorker(want);
}
#endif
std::atomic<unsigned> appliedMenu{UINT32_MAX}; // worker: STEREO TEST mode its settings were resolved for
void refreshWorkerSettings() noexcept { // worker thread: start, every 128 chunks and on a STEREO TEST mode change
    const unsigned menu=testMode.load(std::memory_order_relaxed);appliedMenu.store(menu,std::memory_order_relaxed);
#ifdef __ANDROID__
    if(!tuning.forced.load(std::memory_order_relaxed)){
        tuning.publish.store(std::max(1u,propUnsigned("debug.rdrvr.stereoreplay.publish",16,4096)),std::memory_order_relaxed);
        tuning.wake.store(propUnsigned("debug.rdrvr.stereoreplay.wake",0,4096),std::memory_order_relaxed);
        tuning.pollUs.store(propUnsigned("debug.rdrvr.stereoreplay.poll",250,100000),std::memory_order_relaxed);
        tuning.streamMask.store(propUnsigned("debug.rdrvr.stereoreplay.stream",3,3),std::memory_order_relaxed);
        tuning.nice.store(propInt("debug.rdrvr.stereoreplay.nice",-10,-20,19),std::memory_order_relaxed);
        tuning.streamNice.store(propInt("debug.rdrvr.stereoreplay.streamnice",0,-20,19),std::memory_order_relaxed);
        readAffinityProperty();}
    workerCpuInit();applyPlacement(menu);
#endif
}
// Worker thread, before every chunk: STEREO TEST mode change, render-CPU following, priority, CPU statistics.
void workerChunkBegin(std::uint64_t chunk,bool finalized,std::size_t backlog) noexcept {
    if(testMode.load(std::memory_order_relaxed)!=appliedMenu.load(std::memory_order_relaxed)||(chunk&127u)==127u)refreshWorkerSettings();
#ifdef __ANDROID__
    auto& c=workerCpu();
    if(c.follow){ // keep off the render thread's current CPU
        const int now=cpuStats.renderNowCpu.load(std::memory_order_relaxed);
        if(now>=0&&now<32&&(c.current&(1u<<now))){const unsigned want=c.followBase&~(1u<<now);
            if(want){if(pinWorker(want))cpuStats.follows.fetch_add(1,std::memory_order_relaxed);else c.follow=false;}} // refused: stop until the next refresh
    }
    // keep pace at streamNice; boost once the tape is complete (the splice may wait) or when far behind
    const int nice=finalized||backlog>=(64u<<10)?c.placement.boostNice:c.placement.streamNice;
    if(nice!=c.nice){errno=0;
        if(setpriority(PRIO_PROCESS,gettid(),nice)==0){c.nice=nice;cpuStats.niceChanges.fetch_add(1,std::memory_order_relaxed);}
        else{workerNiceErrno.store(errno);c.nice=getpriority(PRIO_PROCESS,gettid());}
        workerNice.store(c.nice);errno=0;}
    const int cpu=sched_getcpu();
    if(cpu>=0&&cpu<int(cpuSlots))cpuStats.worker[cpu].fetch_add(1,std::memory_order_relaxed);
    const int render=cpuStats.renderStageCpu.load(std::memory_order_relaxed);const int renderNow=cpuStats.renderNowCpu.load(std::memory_order_relaxed);
    if(render>=0){cpuStats.inStageChunks.fetch_add(1,std::memory_order_relaxed);
        if(renderNow==cpu){cpuStats.sameCpu.fetch_add(1,std::memory_order_relaxed);if(auto* m=slotStats())m->sameCpu.fetch_add(1,std::memory_order_relaxed);}}
    if((chunk&31u)==0)sampleFrequencies();
    errno=0;
#else
    (void)finalized;(void)backlog;
#endif
    if(auto* m=slotStats())m->chunks.fetch_add(1,std::memory_order_relaxed);
}
void workerMain() noexcept {
#ifdef __ANDROID__
    pthread_setname_np(pthread_self(),"rdrvr-stereo");
    refreshWorkerSettings(); // Cadence properties, placement policy; the nice value is set per chunk
#else
    workerNice.store(0);
#endif
    auto& w=W();std::unique_lock lk(w.m);
    auto pick=[&]()->Slot*{if(w.paused)return nullptr;for(auto& x:w.slots)if(runnable(x))return &x;return nullptr;}; // depth first
    for(;;){
        Slot* s=nullptr;
        while(!w.stop&&!(s=pick())){
            // Announce the sleep, then look once more: a publisher either sees `sleeping` or the
            // worker sees its ops (seq_cst).
            w.sleeping.store(true,std::memory_order_seq_cst);
            if((s=pick())){w.sleeping.store(false,std::memory_order_seq_cst);break;}
            // A streaming job waits for ops: poll (by default the render thread does not wake the worker
            // mid-stage; endRecording does); no job at all: sleep until a launch
            const unsigned poll=tuning.pollUs.load(std::memory_order_relaxed);
            if(poll&&(w.slots[0].state==Queued||w.slots[1].state==Queued)){
                if(w.cv.wait_for(lk,std::chrono::microseconds(poll))==std::cv_status::timeout)cpuStats.polls.fetch_add(1,std::memory_order_relaxed);}
            else w.cv.wait(lk);
            w.sleeping.store(false,std::memory_order_seq_cst);
        }
        if(w.stop)break;
        Job& j=s->job;const bool finalized=s->finalized;
        const std::size_t limit=finalized?s->finalOffset:j.tape->published.load(std::memory_order_acquire);
        s->state=RunningWorker;if(w.waiters)w.cv.notify_all();lk.unlock();
        workerChunkBegin(counters.workerChunks.load(std::memory_order_relaxed),finalized,limit>j.rc.at?limit-j.rc.at:0);
        if(const unsigned delay=workerDelayUs.load(std::memory_order_relaxed)){ // host tests only; a cancel ends it
            const auto until=std::chrono::steady_clock::now()+std::chrono::microseconds(delay);
            while(std::chrono::steady_clock::now()<until&&!j.cancel.load(std::memory_order_relaxed))std::this_thread::sleep_for(std::chrono::microseconds(250));}
        const auto began=replayClockNs();bool ok=true,done=false;
        const unsigned ownerIdx=j.maps.owner&1u;
        if(!j.rc.begun)counters.ownerJobs[ownerIdx].fetch_add(1,std::memory_order_relaxed);
        try{
            if(!j.rc.begun){j.opts.diagnostics=false;ok=recordBegin(j);}
            if(ok)ok=recordStep(j,limit);
            if(ok&&finalized){ok=recordFinish(j);done=ok;}
        }catch(...){j.decline="RECORD_EXCEPTION";ok=false;}
        const auto spent=replayClockNs()-began;
        counters.ownerRecordNs[ownerIdx].fetch_add(spent,std::memory_order_relaxed);
        if(done){counters.ownerDone[ownerIdx].fetch_add(1,std::memory_order_relaxed);counters.ownerDraws[ownerIdx].fetch_add(j.tape?j.tape->draws:0u,std::memory_order_relaxed);} // tape complete (finalized)
        lk.lock();
        s->workerNs+=spent;++counters.workerChunks;if(auto* m=slotStats())m->workerNs.fetch_add(spent,std::memory_order_relaxed);
        if(s->invalid){s->state=Discarded;++counters.cancelled;} // invalidated meanwhile: no further recording, nothing to wait for
        else if(!ok){s->state=Declined;++counters.workerDeclined;
            if(!std::strcmp(j.decline,"RING_PRESSURE"))++counters.ringPressureJobs;
            if(j.streamed&&!finalized)streamDeclinedReasons.note(j.decline);}
        else if(done){s->state=Done;++counters.workerJobs;counters.workerRecordNs[unsigned(j.stage)&1u].store(s->workerNs);}
        else s->state=Queued; // streaming: wait for more published ops
        if(w.waiters)w.cv.notify_all();
    }
}
bool ensureWorker(Worker& w) noexcept { // caller holds w.m
    if(w.thread)return true;
    try{w.stop=false;w.thread=new std::thread(workerMain);return true;}catch(...){return false;}
}
void resetSlots(Worker& w) noexcept { // caller holds w.m; no slot is RunningWorker
    for(auto& s:w.slots){s.state=Idle;s.invalid=false;s.invalidWhy="NONE";s.job.rewriter.reset();}
    w.ctx.active=false;w.ctx.regionValid=false;w.ctx.ringTight=false;w.ctx.relaunch[0]=w.ctx.relaunch[1]=false;w.active.store(false,std::memory_order_release);
}
// Invalidate a job (caller holds w.m): a queued one is dropped, a running one stops at its next op (D).
void cancelSlot(Slot& s,const char* why) noexcept {
    if(!s.invalid){s.invalid=true;s.invalidWhy=why;}
    s.job.cancel.store(true,std::memory_order_relaxed);
    if(s.state==Queued)s.state=Discarded;
}
void stopWorker(VkDevice device,bool destroyPools) noexcept {
    auto& w=W();std::thread* t=nullptr;
    {std::lock_guard lk(w.m);t=w.thread;w.thread=nullptr;w.stop=true;w.cv.notify_all();}
    if(t){try{if(t->joinable())t->join();}catch(...){}delete t;}
    ringWatch.reset();
    try{std::lock_guard lk(w.m);w.stop=false;resetSlots(w);
        for(auto& s:w.slots){s.job.secondaries.clear();s.job.skeleton.clear();s.job.pool=nullptr;}
        std::lock_guard pl(poolMutex);const auto fn=functions();
        for(auto& owner:w.pools)for(auto& p:owner)if(p.device==device){if(destroyPools&&p.pool&&fn.destroyCommandPool)fn.destroyCommandPool(device,p.pool,nullptr);p=Pool{};}
    }catch(...){}
}
// Discard (Queued) or join (RunningWorker) every job of the current frame, then drop the frame context. [render]
void finishFrame(Worker& w,std::unique_lock<std::mutex>& lk) noexcept {
    for(auto& s:w.slots)if(s.state==Queued||s.state==RunningWorker)cancelSlot(s,"FRAME_ENDED"); // not spliced this frame
    waitWorker(w,lk,[&]{return w.slots[0].state!=RunningWorker&&w.slots[1].state!=RunningWorker;});
    for(auto& s:w.slots)if(s.state!=Idle&&s.job.tape)const_cast<Tape*>(s.job.tape)->streaming.store(false,std::memory_order_relaxed);
    if(w.ctx.active&&w.ctx.regionValid){ // engine usage of this frame object so far (render thread; the frame object is current)
        auto& r=w.ctx.region;observeEngineCursor(r.framePtr,r.mapped,r.size,r.liveCursor(),&r);
        if(r.pressure.load(std::memory_order_relaxed))++counters.ringPressureFrames;
        ringWatch.frameEnded(r.topInit-r.top.load(std::memory_order_relaxed));
    }else if(w.ctx.active)ringWatch.frameEnded(UINT64_MAX);
    for(auto& s:w.slots)if(s.state!=Idle&&s.state!=Consumed)++counters.discarded;
    resetSlots(w);
}
bool sameTargets(const FrameMaps& a,const FrameMaps& b) noexcept {
    if(a.imageCount!=b.imageCount||a.context!=b.context||a.device!=b.device||a.owner!=b.owner||a.queueFamily!=b.queueFamily||a.frameSerial!=b.frameSerial||a.valid!=b.valid)return false;
    for(unsigned k=0;k<a.imageCount;++k){const auto& x=a.images[k];const auto& y=b.images[k];
        if(x.left!=y.left||x.right!=y.right||x.leftTexture!=y.leftTexture||x.rightTexture!=y.rightTexture||x.role!=y.role||x.format!=y.format||
           x.extent.width!=y.extent.width||x.extent.height!=y.extent.height||x.extent.depth!=y.extent.depth)return false;}
    return true;
}
bool sameMaps(const FrameMaps& a,const FrameMaps& b) noexcept { // every input of the recording
    return sameTargets(a,b)&&a.uniformAlignment==b.uniformAlignment&&
        !std::memcmp(a.relClip,b.relClip,sizeof a.relClip)&&!std::memcmp(a.camera,b.camera,sizeof a.camera)&&!std::memcmp(a.correction.data(),b.correction.data(),sizeof(float)*16);
}
bool sameSets(const SetMap& a,const SetMap& b) noexcept {
    if(a.count!=b.count)return false;for(unsigned i=0;i<a.count;++i)if(a.pairs[i]!=b.pairs[i])return false;return true;
}
}
}

// ================================================================= internal entry points
namespace internal {
SecondaryOutcome replaySecondaries(Stage stage,VkCommandBuffer primary,const FrameMaps& maps,VkPipelineLayout layout,const SetMap& sets,const Tape& t) noexcept {
    using namespace secondary;
    auto& j=jobs[unsigned(stage)&1u];auto& st=replayStats();
    const auto began=replayClockNs();
    auto declined=[&]{++counters.fallbacks;counters.lastDecline.store(j.decline);j.rewriter.reset();return SecondaryOutcome::Declined;};
    std::unique_lock lock(poolMutex,std::defer_lock);
    try{lock.lock();}catch(...){j.decline="POOL_LOCK";return declined();}
    bool ok=false;
    try{ok=prepareJob(j,stage,primary,maps,layout,sets,t);}catch(...){j.decline="PREPARE_EXCEPTION";ok=false;}
    if(!ok)return declined();
    const auto prepared=replayClockNs();
    try{ok=recordJob(j);}catch(...){j.decline="RECORD_EXCEPTION";ok=false;}
    if(!ok)return declined();
    lock.unlock();
    const auto recorded=replayClockNs();
    const auto outcome=spliceJob(j);
    const auto now=replayClockNs();const unsigned s=unsigned(stage)&1u;
    if(outcome==SecondaryOutcome::Done){
        ++st.replays;st.replayNs[s].store(now-began);st.replayCommands[s].store(t.commands);
        ++counters.jobs;counters.secondaries+=j.secondaries.size();
        counters.prepareNs[s].store(prepared-began);counters.recordNs[s].store(recorded-prepared);counters.spliceNs[s].store(now-recorded);
    }else if(outcome==SecondaryOutcome::Faulted){++counters.faults;counters.lastDecline.store("SPLICE_FAULT");}
    else{++counters.fallbacks;counters.lastDecline.store(j.decline);}
    j.rewriter.reset();
    return outcome;
}
void secondaryShutdownAfterIdle(VkDevice device) noexcept {
    using namespace secondary;
    stopWorker(device,true);
    try{std::lock_guard lock(poolMutex);const auto fn=functions();
        for(auto& p:pools)if(p.pool&&p.device==device){if(fn.destroyCommandPool)fn.destroyCommandPool(device,p.pool,nullptr);p=Pool{};}
        for(auto& j:jobs){j.secondaries.clear();j.skeleton.clear();j.pool=nullptr;}
    }catch(...){}
}
void secondaryDeviceDestroyed(VkDevice device) noexcept {
    using namespace secondary;
    stopWorker(device,false);
    try{std::lock_guard lock(poolMutex);for(auto& p:pools)if(p.device==device)p=Pool{};for(auto& j:jobs){j.secondaries.clear();j.skeleton.clear();j.pool=nullptr;}}catch(...){}
}
void noteRenderStage(Stage stage,bool begin) noexcept {
    using secondary::cpuStats;
    if(begin&&stage==Stage::Depth){ // Frame start: the STEREO TEST mode this frame accumulates into
        auto& f=secondary::frameTrack;const unsigned mode=testMode.load(std::memory_order_relaxed);
        if(mode!=f.mode){f.mode=mode;f.stable=0;}
        secondary::frameSlot.store(f.stable>=secondary::settleFrames&&mode<testModeCount?int(mode):-1,std::memory_order_relaxed);
    }
#ifdef __ANDROID__
    struct Open {int cpu{-1};long nivcsw{},nvcsw{};bool valid{};unsigned count{};};static Open stage_; // render thread only
    const int cpu=sched_getcpu();rusage ru{};const bool ok=getrusage(RUSAGE_THREAD,&ru)==0;
    if(cpu>=0)cpuStats.renderNowCpu.store(cpu,std::memory_order_relaxed);
    if(begin){
        if(cpu>=0&&cpu<int(secondary::cpuSlots)){cpuStats.render[cpu].fetch_add(1,std::memory_order_relaxed);cpuStats.renderSeen.fetch_or(1u<<cpu,std::memory_order_relaxed);}
        stage_.cpu=cpu;stage_.valid=ok;if(ok){stage_.nivcsw=ru.ru_nivcsw;stage_.nvcsw=ru.ru_nvcsw;}
        cpuStats.renderStageCpu.store(cpu,std::memory_order_relaxed);if(cpu>=0)cpuStats.renderLastCpu.store(cpu,std::memory_order_relaxed);
        if((stage_.count++&511u)==0){ // The render thread's scheduling class, priority and CPU set
            cpuStats.renderPolicy.store(sched_getscheduler(0),std::memory_order_relaxed);sched_param sp{};if(sched_getparam(0,&sp)==0)cpuStats.renderRtPriority.store(sp.sched_priority,std::memory_order_relaxed);
            errno=0;const int nice=getpriority(PRIO_PROCESS,gettid());if(!errno)cpuStats.renderNice.store(nice,std::memory_order_relaxed);
            cpu_set_t set;CPU_ZERO(&set);if(sched_getaffinity(0,sizeof set,&set)==0)cpuStats.renderMask.store(secondary::maskOf(set),std::memory_order_relaxed);}
    }else{
        if(stage_.valid&&ok){const auto nivcsw=std::uint64_t(ru.ru_nivcsw-stage_.nivcsw);const bool moved=cpu!=stage_.cpu;
            cpuStats.renderStages.fetch_add(1,std::memory_order_relaxed);if(moved)cpuStats.renderMigrations.fetch_add(1,std::memory_order_relaxed);
            cpuStats.renderNivcsw.fetch_add(nivcsw,std::memory_order_relaxed);cpuStats.renderNvcsw.fetch_add(std::uint64_t(ru.ru_nvcsw-stage_.nvcsw),std::memory_order_relaxed);
            if(auto* m=secondary::slotStats()){m->stages.fetch_add(1,std::memory_order_relaxed);m->nivcsw.fetch_add(nivcsw,std::memory_order_relaxed);if(moved)m->migrations.fetch_add(1,std::memory_order_relaxed);}}
        stage_.valid=false;cpuStats.renderStageCpu.store(-1,std::memory_order_relaxed);if(cpu>=0)cpuStats.renderLastCpu.store(cpu,std::memory_order_relaxed);
    }
    errno=0;
#else
    if(!begin)if(auto* m=secondary::slotStats())m->stages.fetch_add(1,std::memory_order_relaxed);
#endif
}
void noteRenderCpu() noexcept {
#ifdef __ANDROID__
    const int saved=errno;const int cpu=sched_getcpu();if(cpu>=0)secondary::cpuStats.renderNowCpu.store(cpu,std::memory_order_relaxed);errno=saved;
#endif
}
void noteColdWait() noexcept {secondary::cpuStats.coldWaits.fetch_add(1,std::memory_order_relaxed);}
// ---- worker mode (async=2) ----
bool frameJobsActive() noexcept {return secondary::W().active.load(std::memory_order_acquire);}
RingRegion* sharedRegionFor(const RingSnapshot& ring) noexcept {
    using namespace secondary;auto& w=W();if(!ring.ringValid||!w.active.load(std::memory_order_acquire))return nullptr;
    try{std::lock_guard lk(w.m);
        if(!w.ctx.active||!w.ctx.regionValid||w.ctx.region.framePtr!=ring.framePtr||w.ctx.region.mapped!=ring.ringMapped||w.ctx.region.size!=ring.ringSize)return nullptr;
        return &w.ctx.region;}catch(...){return nullptr;}
}
void tapeProgress() noexcept {
    auto& w=secondary::W();if(!w.sleeping.load(std::memory_order_seq_cst))return;
    try{{std::lock_guard lk(w.m);}w.cv.notify_all();secondary::cpuStats.wakes.fetch_add(1,std::memory_order_relaxed);}catch(...){} // the lock orders this wake after the worker's last check
}
void tapeFinished(Stage stage) noexcept {
    using namespace secondary;auto& w=W();if(!w.active.load(std::memory_order_acquire))return;
    const unsigned si=unsigned(stage)&1u;Tape& t=tapeFor(stage);
    try{
        FrameMaps maps{};bool want=false;
        {std::lock_guard lk(w.m);auto& s=w.slots[si];if(s.job.tape==&t&&s.state!=Idle)t.streaming.store(false,std::memory_order_relaxed);
            if(s.state==Declined&&s.job.streamed&&!s.finalized&&s.job.tape==&t&&streamingSpecific(s.job.decline)){
                // the worker declined the streamed job for a reason the complete tape does not have: free the slot so
                // the stage end launches it again in the non-streamed mode (GameXr: stageLaunched() is false)
                s.state=Idle;s.invalid=false;s.invalidWhy="NONE";s.job.rewriter.reset();w.ctx.relaunch[si]=true;return;}
            want=(s.state==Queued||s.state==RunningWorker)&&!s.finalized&&s.job.tape==&t;if(want)maps=s.job.maps;}
        if(!want)return;
        const ForeignTargets foreign=scanForeignTargets(t,maps,stage); // render thread, whole tape
        std::lock_guard lk(w.m);auto& s=w.slots[si];
        if(!((s.state==Queued||s.state==RunningWorker)&&!s.finalized))return;
        s.finalOffset=t.used;s.primary=t.command;s.foreign=foreign;s.finalized=true;
        t.published.store(t.used,std::memory_order_seq_cst);
        if(t.invalid||!t.commands){cancelSlot(s,"TAPE_INVALID");++counters.tapeInvalid;}
        w.cv.notify_all();
    }catch(...){}
}
void waitTapeIdle(Stage stage) noexcept {
    using namespace secondary;auto& w=W();if(!w.active.load(std::memory_order_acquire))return;
    try{std::unique_lock lk(w.m);auto& s=w.slots[unsigned(stage)&1u];if(s.state==Queued||s.state==RunningWorker)cancelSlot(s,"TAPE_REUSED");
        waitWorker(w,lk,[&]{return s.state!=RunningWorker;});}catch(...){}
}
// Splice of a launched job [render]: prediction check, then Done -> splice, Queued -> take over (record here),
// RunningWorker -> wait; anything else declines (nothing recorded into the primary) and replay() runs inline.
SecondaryOutcome replayLaunched(Stage stage,VkCommandBuffer primary,const FrameMaps& maps,VkPipelineLayout layout,const SetMap& sets,const Tape& t) noexcept {
    using namespace secondary;auto& w=W();auto& st=replayStats();const unsigned si=unsigned(stage)&1u;
    const auto began=replayClockNs();
    auto declined=[&](const char* why){++counters.fallbacks;counters.lastDecline.store(why);if(auto* m=slotStats())m->fallbacks.fetch_add(1,std::memory_order_relaxed);return SecondaryOutcome::Declined;};
    std::unique_lock lk(w.m,std::defer_lock);try{lk.lock();}catch(...){return declined("WORKER_LOCK");}
    Slot& s=w.slots[si];Job& j=s.job;
    if(!w.ctx.active||s.state==Idle||s.state==Consumed||s.state==Discarded){const char* why=s.invalid?s.invalidWhy:"NOT_LAUNCHED";lk.unlock();return declined(why);}
    if(s.state==Declined&&!s.invalid){const char* d=j.decline;s.state=Consumed;j.rewriter.reset();lk.unlock();return declined(d);} // the worker's own reason
    // Every input of the recording must equal the splice's (the maps were predicted before the consumer filled them).
    const char* why=s.invalid?s.invalidWhy:nullptr;
    if(!why){
        if(!s.finalized)why="NOT_FINALIZED";
        else if(s.serial!=maps.frameSerial)why="FRAME_SERIAL";
        else if(s.primary!=primary||j.tape!=&t)why="TAPE";
        else if(!sameMaps(j.maps,maps))why="MAPS";
        else if(j.mask!=liveMask())why="MASK";
        else if(j.layout!=layout)why="PUSH_LAYOUT";
        else if(!sameSets(j.sets,sets))why="BINDLESS_SETS";
        if(why){cancelSlot(s,why);++counters.predictionMismatches;counters.lastMismatch.store(why);}
    }
    if(why){ // discard; a job already running is joined first so the inline replay's ring writes (incl. the in-place
             // left PrevViewProj) come after the worker's and the two never run concurrently on one stage
        if(s.state==RunningWorker){const auto t0=replayClockNs();waitWorker(w,lk,[&]{return s.state!=RunningWorker;});const auto waited=replayClockNs()-t0;counters.joinWaitNs[si].store(waited);counters.joinWaitNsSum[si]+=waited;++counters.joinWaitCount[si];++counters.joinWaits;
            if(auto* m=slotStats()){m->waited.fetch_add(1,std::memory_order_relaxed);m->joinWaitNs.fetch_add(waited,std::memory_order_relaxed);}}
        lk.unlock();return declined(why);}
    std::uint64_t waitedNs=0,tailNs=0;
    if(s.state==RunningWorker){ // the worker is on a chunk of this job (with streaming, usually the last few ops)
        const auto t0=replayClockNs();waitWorker(w,lk,[&]{return s.state!=RunningWorker;});waitedNs=replayClockNs()-t0;}
    if(s.state==Queued){ // ops (or just the end) not recorded yet: record the rest here instead of waking the worker
        s.state=RunningRender;lk.unlock();const bool whole=!j.rc.begun;
        if(whole)j.opts.diagnostics=true;
        const auto t0=replayClockNs();bool ok=true;
        try{if(!j.rc.begun)ok=recordBegin(j);if(ok)ok=recordStep(j,s.finalOffset);if(ok)ok=recordFinish(j);}catch(...){j.decline="RECORD_EXCEPTION";ok=false;}
        tailNs=replayClockNs()-t0;counters.recordNs[si].store(tailNs);counters.tailNsSum[si]+=tailNs;++counters.tailCount[si];if(whole)++counters.takeovers;else ++counters.tails;
        if(auto* m=slotStats()){(whole?m->takeovers:m->tails).fetch_add(1,std::memory_order_relaxed);m->tailNs.fetch_add(tailNs,std::memory_order_relaxed);}
        if(!ok&&!std::strcmp(j.decline,"RING_PRESSURE"))++counters.ringPressureJobs;
        lk.lock();s.state=ok?Done:Declined;}
    if(waitedNs){counters.joinWaitNs[si].store(waitedNs);counters.joinWaitNsSum[si]+=waitedNs;++counters.joinWaitCount[si];++counters.joinWaits;++counters.waitedJobs;
        if(auto* m=slotStats()){m->waited.fetch_add(1,std::memory_order_relaxed);m->joinWaitNs.fetch_add(waitedNs,std::memory_order_relaxed);}}
    if(waitedNs||tailNs)++counters.notReadyJobs;
    const bool done=s.state==Done;s.state=Consumed;lk.unlock(); // Consumed: the render thread owns the job until frame end
    if(!done){j.rewriter.reset();return declined(j.decline);}
    if(j.ring.ringValid){ // same engine frame object, and the engine's own allocations have not reached the shared region
        const RingSnapshot now=snapshotRing(maps,j.mask);
        if(now.framePtr!=j.ring.framePtr||now.ringMapped!=j.ring.ringMapped||now.ringSize!=j.ring.ringSize)why="RING_FRAME";
        else{observeEngineCursor(now.framePtr,now.ringMapped,now.ringSize,now.ringCursor,j.opts.region);if(j.opts.region&&now.ringCursor>j.opts.region->top.load(std::memory_order_relaxed))why="RING_OVERRUN";}
        if(why){++counters.predictionMismatches;counters.lastMismatch.store(why);j.rewriter.reset();return declined(why);}
    }
    const BoundState* before=trackedState(primary);if(!before){j.rewriter.reset();return declined("NO_TRACKED_STATE");}
    j.before=*before;j.primary=s.primary;j.foreign=s.foreign; // finalized at endRecording (render thread)
    if(j.merge.on){ // The bindless indices the recording checked against must still be the actual ones
        bool same=std::min(maps.imageCount,unsigned(j.merge.roleIdx.size()))==j.merge.images;
        for(unsigned k=0;same&&k<j.merge.images;++k)same=merge::leftTextureIndex(maps.images[k].leftTexture)==j.merge.roleIdx[k];
        if(!same){j.merge.on=false;merge::mergeStats().keptIndexChanged.fetch_add(1,std::memory_order_relaxed);}}
    const auto spliceBegan=replayClockNs();
    const auto outcome=spliceJob(j);
    const auto now=replayClockNs();
    if(outcome==SecondaryOutcome::Done){
        ++st.replays;st.replayNs[si].store(now-began);st.replayCommands[si].store(t.commands);
        ++counters.jobs;counters.secondaries+=j.secondaries.size();counters.spliceNs[si].store(now-spliceBegan);counters.renderNs[si].store(now-began);
        ++counters.jobsPerStage[si];counters.renderNsSum[si]+=now-began;
        if(s.workerNs){counters.workerRecordNsSum[si]+=s.workerNs;++counters.workerRecordJobs[si];} // incl. jobs finished by a tail
        if(auto* m=slotStats())m->jobs.fetch_add(1,std::memory_order_relaxed);
    }else if(outcome==SecondaryOutcome::Faulted){++counters.faults;counters.lastDecline.store("SPLICE_FAULT");}
    else{++counters.fallbacks;counters.lastDecline.store(j.decline);}
    j.rewriter.reset();
    return outcome;
}
}

// ================================================================= public
unsigned asyncMode() noexcept {
    const unsigned forced=secondary::modeOverride.load(std::memory_order_relaxed);
    if(forced!=UINT32_MAX)return forced==1u||forced==2u?forced:0u;
#ifdef __ANDROID__
    const unsigned menu=testMode.load(std::memory_order_relaxed);
    if(menu==testModeNoAsyncWorker)return 0;  // STEREO TEST "NO ASYNC WORKER": inline replay
    if(liveMask()&(1u<<31))return 1;          // "ASYNC SECONDARIES": mask bit 31 (debug.rdrvr.stereoreplay.mask)
    char value[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.stereoreplay.async",value)>0&&value[0]){
        const unsigned long v=std::strtoul(value,nullptr,0);secondary::counters.asyncProperty.store((long long)v,std::memory_order_relaxed);
        return unsigned(std::min<unsigned long>(v,2ul));} // clamped to {0,1,2}; 0 forces the inline replay
    return 2; // default: worker recording (streamed from the left stages)
#else
    return 0;
#endif
}
void setAsyncModeForTests(unsigned mode) noexcept {secondary::modeOverride.store(mode,std::memory_order_relaxed);}
void setAsyncDeviceFunctionsForTests(const AsyncDeviceFunctions* f) noexcept {
    if(f){secondary::testFunctionsCopy=*f;secondary::testFunctionsSet.store(true,std::memory_order_release);}
    else secondary::testFunctionsSet.store(false,std::memory_order_release);
}
std::uint64_t asyncFallbacksForTests() noexcept {return secondary::counters.fallbacks.load();}
// ---- worker mode (async=2), GameXr render thread ----
bool launchWanted() noexcept {return asyncMode()==2u&&enabled();}
void noteNotLaunched(Stage,const char* reason) noexcept {secondary::noteReason(reason?reason:"?");}
void noteStreamNotLaunched(Stage,const char* reason) noexcept {secondary::streamNotLaunchedReasons.note(reason?reason:"?");}
void launchSecondaries(Stage stage,const FrameMaps& m,std::uint64_t token,bool streaming) noexcept {
    using namespace secondary;using namespace internal;
    const int savedErrno=errno;auto& w=W();const unsigned si=unsigned(stage)&1u;const auto began=replayClockNs();
    auto refuse=[&](const char* why){if(streaming){++counters.streamRefused;streamNotLaunchedReasons.note(why);}else noteReason(why);errno=savedErrno;}; // a stage-start miss retries at the stage end
    try{
        if(!enabled())return refuse("DISABLED");
        if(streaming&&!(effectiveStreamMask()&(1u<<si)))return refuse("STREAM_OFF"); // Stream property / STEREO TEST 7: launched at the stage end
        Tape& t=tapeFor(stage);
        if(streaming?(!t.active||t.invalid||t.commands||!t.reserveStorage()):(t.active||t.invalid||!t.commands||!t.command))return refuse("TAPE_NOT_READY");
        if(!m.valid||!m.device)return refuse("MAPS_INVALID");
        if(!rawCommands().executeCommands)return refuse("NO_EXECUTE_COMMANDS");
        const auto fn=functions();
        if(!fn.createCommandPool||!fn.destroyCommandPool||!fn.resetCommandPool||!fn.allocateCommandBuffers||!fn.beginCommandBuffer||!fn.endCommandBuffer)return refuse("NO_POOL_FUNCTIONS");
        if(m.queueFamily==UINT32_MAX)return refuse("NO_QUEUE_FAMILY");
        const auto layout=replayPushLayout(m.device);if(!layout)return refuse("PUSH_LAYOUT");
        std::unique_lock lk(w.m);
        if(w.ctx.active&&w.ctx.serial!=m.frameSerial)finishFrame(w,lk); // the previous frame never reached FinishPrimary
        if(!w.ctx.active){w.ctx.active=true;w.ctx.serial=m.frameSerial;w.ctx.regionValid=false;w.active.store(true,std::memory_order_release);resetLegacyRing();}
        Slot& s=w.slots[si];
        if(s.state!=Idle)return refuse("SLOT_BUSY");
        lk.unlock(); // an Idle slot belongs to the render thread until it is queued
        // ---- prepare (render thread): everything the recording reads is copied into the job ----
        Job& j=s.job;j.decline="NONE";j.rewriter.reset();j.secondaries.clear();j.skeleton.clear();
        j.stage=stage;j.primary=streaming?VK_NULL_HANDLE:t.command;j.maps=m;j.layout=layout;j.tape=&t;j.fn=fn;j.pool=nullptr;j.streamed=streaming;j.rc=RecordCursor{};
        j.boundary=boundaryRightFlags(); // Latched at the previous frame's end, constant through this frame
        setupMerge(j);                   // Indices of the predicted maps (re-checked at the splice)
        j.cancel.store(false,std::memory_order_relaxed);
        snapshotSets(m.device,m.owner&1u,j.sets);
        j.mask=liveMask();
        j.foreign=streaming?ForeignTargets{}:scanForeignTargets(t,m,stage); // streaming: the tape does not exist yet
        j.ring=snapshotRing(m,j.mask);
        if(j.ring.ringValid)observeEngineCursor(j.ring.framePtr,j.ring.ringMapped,j.ring.ringSize,j.ring.ringCursor,nullptr);
        j.opts=RewriteOptions{};j.opts.ownInfos=true;j.opts.diagnostics=false;
        if(j.ring.ringValid){
            lk.lock();auto& r=w.ctx.region;
            if(w.ctx.ringTight)return refuse("RING_TIGHT");
            if(!w.ctx.regionValid){const std::uint64_t align=std::max<std::uint32_t>(m.uniformAlignment,64);
                const std::uint64_t top=(j.ring.ringSize-regionHeadroom)&~(align-1);
                // floor: 1 MB above the engine now, and above the engine's peak usage of the recent frames (the engine
                // keeps allocating while a streaming job already writes right-eye blocks from the top)
                const std::uint64_t floor=std::max<std::uint64_t>(j.ring.ringCursor+(1u<<20),ringWatch.highWater()+highWaterMargin);
                const std::uint64_t usage=ringWatch.usage(),needed=std::max<std::uint64_t>(1u<<20,usage+usage/2);
                regionFloorLast.store(floor);
                if(top<floor||top-floor<needed){ // no worker jobs this frame: the inline replay keeps its own floor (cursor + 1 MB)
                    w.ctx.ringTight=true;++counters.ringTight;return refuse("RING_TIGHT");}
                r.framePtr=j.ring.framePtr;r.mapped=j.ring.ringMapped;r.size=j.ring.ringSize;
                r.top.store(top,std::memory_order_relaxed);r.topInit=top;r.floor=floor;r.pressure.store(false,std::memory_order_relaxed);
                w.ctx.regionValid=true;}
            else if(r.framePtr!=j.ring.framePtr||r.mapped!=j.ring.ringMapped||r.size!=j.ring.ringSize)return refuse("RING_FRAME_CHANGED");
            j.opts.region=&r;lk.unlock();
        }
        { // the stage's pool of the predicted owner: that owner is retired (GPU fence) so its previous secondaries are done
            std::lock_guard pl(poolMutex);Pool& p=w.pools[m.owner&1u][si];
            if(p.pool&&p.device!=m.device)p=Pool{};
            if(p.pool&&p.family!=m.queueFamily)return refuse("POOL_FAMILY");
            if(!p.pool){
                VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};ci.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;ci.queueFamilyIndex=m.queueFamily;
                VkCommandPool pool{};if(fn.createCommandPool(m.device,&ci,nullptr,&pool)!=VK_SUCCESS||!pool)return refuse("POOL_CREATE");
                p.device=m.device;p.family=m.queueFamily;p.pool=pool;p.buffers.clear();p.used=0;p.frame=m.frameSerial;++counters.poolsCreated;
            }else if(p.frame!=m.frameSerial||!m.frameSerial){
                if(fn.resetCommandPool(p.device,p.pool,0)!=VK_SUCCESS)return refuse("POOL_RESET");p.used=0;p.frame=m.frameSerial;++counters.poolResets;}
            j.pool=&p;
        }
        lk.lock();
        if(!ensureWorker(w))return refuse("WORKER_START");
        s.invalid=false;s.invalidWhy="NONE";s.serial=m.frameSerial;s.token=token;s.workerNs=0;
        s.finalized=!streaming;s.finalOffset=streaming?0:t.used;s.primary=j.primary;s.foreign=j.foreign;
        if(streaming){t.sinceNotify=0;t.sinceWake=0;t.publishEvery=std::max(1u,tuning.publish.load(std::memory_order_relaxed));t.wakeEvery=tuning.wake.load(std::memory_order_relaxed);
            t.published.store(0,std::memory_order_relaxed);t.streaming.store(true,std::memory_order_seq_cst);++counters.streamed;}else ++counters.lateLaunches;
        if(!streaming&&w.ctx.relaunch[si]){w.ctx.relaunch[si]=false;++counters.relaunched;}
        s.state=Queued;++counters.launched;
        counters.prepareNs[si].store(replayClockNs()-began);
        w.cv.notify_all();
    }catch(...){refuse("LAUNCH_EXCEPTION");}
    errno=savedErrno;
}
bool stageLaunched(Stage stage) noexcept {
    auto& w=secondary::W();if(!w.active.load(std::memory_order_acquire))return false;
    try{std::lock_guard lk(w.m);return w.slots[unsigned(stage)&1u].state!=secondary::Idle;}catch(...){return false;}
}
void validateLaunched(const FrameMaps& filled,std::uint64_t token) noexcept {
    using namespace secondary;auto& w=W();if(!w.active.load(std::memory_order_acquire))return;
    try{std::lock_guard lk(w.m);if(!w.ctx.active)return;
        for(auto& s:w.slots){
            if(s.state==Idle||s.state==Consumed||s.state==Discarded||s.invalid)continue;
            const char* why=s.token!=token?"OWNER":s.serial!=filled.frameSerial?"FRAME_SERIAL":!sameTargets(s.job.maps,filled)?"TARGETS":nullptr;
            if(!why)continue;
            cancelSlot(s,why);++counters.predictionMismatches;counters.lastMismatch.store(why);
        }
    }catch(...){}
}
void finishFrameJobs() noexcept {
    using namespace secondary;auto& w=W();if(!w.active.load(std::memory_order_acquire))return;
    try{std::unique_lock lk(w.m);finishFrame(w,lk);}catch(...){}
}
// GameXr calls this before an eye-target rebuild
// (spec change) or an owner reset. This frame's jobs were launched with maps predicted from the previous generation: cancel
// them and join a running chunk BEFORE prepare() (its bindless writes then never meet a worker that has the clone set
// bound). The frame context stays: the stages decline with `why` (replayLaunched) and replay inline. Counters are
// attempts (a failing prepare() with a differing spec retries every frame); predictionMismatches stays the owner-race
// counter. The join can block for one running chunk, once per rebuild.
namespace {
struct RebuildCancelCounters {std::atomic<std::uint64_t> attempts{},jobs{},joinNs{};std::atomic<const char*> last{"NONE"};} rebuildCancels;
// The rebuild* counters above count only TARGET_REBUILD and OWNER_RESET (the eye-target / owner
// cancels); every cancel is also tallied per reason [attempts, jobs] (CLONE_FLAGGED = the stage
// gate's containment cancel).
constexpr const char* cancelReasons[4]{"TARGET_REBUILD","OWNER_RESET","CLONE_FLAGGED","OTHER"};
std::array<std::array<std::atomic<std::uint64_t>,2>,4> cancelsByReason{};
unsigned cancelReasonIndex(const char* why) noexcept {for(unsigned i=0;i<3;++i)if(why&&!std::strcmp(why,cancelReasons[i]))return i;return 3;}
}
unsigned cancelFrameJobs(const char* why) noexcept {
    using namespace secondary;auto& w=W();if(!w.active.load(std::memory_order_acquire))return 0;
    try{std::unique_lock lk(w.m);if(!w.ctx.active)return 0;unsigned n=0;
        for(auto& s:w.slots){if(s.state==Idle||s.state==Consumed||s.state==Discarded)continue;
            cancelSlot(s,why);++n;                                  // Queued -> Discarded here; RunningWorker -> Discarded by
            if(s.state==Done||s.state==Declined)s.state=Discarded;} // the worker after its chunk; Done/Declined: never spliced
        const auto t0=replayClockNs();
        waitWorker(w,lk,[&]{return w.slots[0].state!=RunningWorker&&w.slots[1].state!=RunningWorker;});
        if(n){const unsigned reason=cancelReasonIndex(why);++cancelsByReason[reason][0];cancelsByReason[reason][1]+=n;
            if(reason<2){++rebuildCancels.attempts;rebuildCancels.jobs+=n;rebuildCancels.joinNs+=replayClockNs()-t0;rebuildCancels.last.store(why);}}
        return n;}catch(...){return 0;}
}
void noteFrameStages(const FrameStageSample& f) noexcept {
    using namespace secondary;auto& t=frameTrack;
    const StageSample* samples[5]{&f.depthBucket,&f.colorBucket,&f.depthStage,&f.colorStage,&f.stereoDraw};
    bool fresh[5]{};for(unsigned i=0;i<5;++i){fresh[i]=samples[i]->ns!=UINT64_MAX&&samples[i]->serial!=t.serial[i];t.serial[i]=samples[i]->serial;}
    const unsigned mode=testMode.load(std::memory_order_relaxed);const int slot=frameSlot.load(std::memory_order_relaxed);
    if(mode==t.mode)++t.stable;
    if(mode>=testModeCount)return;
    auto& m=byMode[mode];
    if(slot!=int(mode)){m.settling.fetch_add(1,std::memory_order_relaxed);return;} // first frames after a switch, or switched mid-frame
    if(fresh[0]&&fresh[1]&&f.depthObjects!=UINT32_MAX&&f.colorObjects!=UINT32_MAX){
        m.frames.fetch_add(1,std::memory_order_relaxed);
        m.depthCpuNs.fetch_add(f.depthBucket.ns,std::memory_order_relaxed);m.depthObjects.fetch_add(f.depthObjects,std::memory_order_relaxed);
        m.colorCpuNs.fetch_add(f.colorBucket.ns,std::memory_order_relaxed);m.colorObjects.fetch_add(f.colorObjects,std::memory_order_relaxed);}
    if(fresh[2]&&fresh[3]){m.stageFrames.fetch_add(1,std::memory_order_relaxed);
        m.depthStageNs.fetch_add(f.depthStage.ns,std::memory_order_relaxed);m.colorStageNs.fetch_add(f.colorStage.ns,std::memory_order_relaxed);}
    if(fresh[4]){m.stereoDrawFrames.fetch_add(1,std::memory_order_relaxed);m.stereoDrawNs.fetch_add(f.stereoDraw.ns,std::memory_order_relaxed);}
}
void setWorkerPausedForTests(bool paused) noexcept {auto& w=secondary::W();std::lock_guard lk(w.m);w.paused=paused;w.cv.notify_all();}
void setWorkerDelayForTests(unsigned microseconds) noexcept {secondary::workerDelayUs.store(microseconds);}
void setStreamTuningForTests(unsigned publish,unsigned wake,unsigned pollUs,unsigned streamMask) noexcept {
    auto& t=secondary::tuning;
    if(!publish){t.forced.store(false);t.publish.store(16);t.wake.store(0);t.pollUs.store(250);t.streamMask.store(3);return;} // Defaults: not forced, the properties apply
    t.publish.store(publish);t.wake.store(wake);t.pollUs.store(pollUs);t.streamMask.store(streamMask&3u);t.forced.store(true);
}
void waitWorkerForTests(bool untilRunning) noexcept {
    using namespace secondary;auto& w=W();std::unique_lock lk(w.m);
    auto any=[&](int state){return w.slots[0].state==state||w.slots[1].state==state;};
    ++w.waiters;w.cv.wait_for(lk,std::chrono::seconds(5),[&]{return untilRunning?any(RunningWorker):!runnable(w.slots[0])&&!runnable(w.slots[1])&&!any(RunningWorker);});--w.waiters;
}
AsyncWorkerCounters asyncWorkerCountersForTests() noexcept {
    auto& c=secondary::counters;
    return {c.launched.load(),c.takeovers.load(),c.predictionMismatches.load(),c.workerJobs.load(),c.joinWaits.load(),c.discarded.load(),c.floorViolations.load(),c.jobs.load(),c.fallbacks.load(),c.tails.load(),
        c.relaunched.load(),c.cancelled.load(),c.ringTight.load(),c.ringPressureJobs.load()};
}
std::uint64_t asyncRegionTopForTests() noexcept {auto& w=secondary::W();std::lock_guard lk(w.m);return w.ctx.regionValid?w.ctx.region.top.load():0;}
std::string asyncReport(){
    using namespace secondary;auto& c=counters;std::ostringstream o;
    o<<"{\"mode\":"<<asyncMode()<<",\"jobs\":"<<c.jobs<<",\"fallbacks\":"<<c.fallbacks<<",\"faults\":"<<c.faults<<",\"secondaries\":"<<c.secondaries<<",\"executeCalls\":"<<c.executeCalls
     <<",\"lastDecline\":\""<<c.lastDecline.load()<<"\",\"executeBound\":"<<(internal::rawCommands().executeCommands?1:0)
     <<",\"poolsCreated\":"<<c.poolsCreated<<",\"poolResets\":"<<c.poolResets<<",\"allocated\":"<<c.allocated
     <<",\"prepareNs\":["<<c.prepareNs[0]<<","<<c.prepareNs[1]<<"],\"recordNs\":["<<c.recordNs[0]<<","<<c.recordNs[1]<<"],\"spliceNs\":["<<c.spliceNs[0]<<","<<c.spliceNs[1]<<"]"
     <<",\"launched\":"<<c.launched<<",\"notLaunched\":"<<notLaunchedReport()<<",\"workerJobs\":"<<c.workerJobs<<",\"workerDeclined\":"<<c.workerDeclined
     <<",\"workerRecordNs\":["<<c.workerRecordNs[0]<<","<<c.workerRecordNs[1]<<"],\"joinWaitNs\":["<<c.joinWaitNs[0]<<","<<c.joinWaitNs[1]<<"],\"joinWaits\":"<<c.joinWaits
     <<",\"takeovers\":"<<c.takeovers<<",\"predictionMismatches\":"<<c.predictionMismatches<<",\"lastMismatch\":\""<<c.lastMismatch.load()<<"\",\"discarded\":"<<c.discarded
     <<",\"floorViolations\":"<<c.floorViolations<<",\"replayRenderNs\":["<<c.renderNs[0]<<","<<c.renderNs[1]<<"]"
     <<",\"streamed\":"<<c.streamed<<",\"lateLaunches\":"<<c.lateLaunches<<",\"streamRefused\":"<<c.streamRefused<<",\"workerChunks\":"<<c.workerChunks<<",\"tails\":"<<c.tails
     <<",\"wakes\":"<<cpuStats.wakes.load()<<",\"polls\":"<<cpuStats.polls.load()
     <<",\"waitedJobs\":"<<c.waitedJobs<<",\"notReadyJobs\":"<<c.notReadyJobs<<",\"tapeInvalid\":"<<c.tapeInvalid
     <<",\"relaunched\":"<<c.relaunched<<",\"cancelled\":"<<c.cancelled
     <<",\"rebuildCancelAttempts\":"<<rebuildCancels.attempts<<",\"rebuildCancelledJobs\":"<<rebuildCancels.jobs<<",\"rebuildJoinNs\":"<<rebuildCancels.joinNs
     <<",\"lastRebuildCancel\":\""<<rebuildCancels.last.load()<<"\""
     <<",\"rebuildCancelByReason\":{"<<[]{std::string s;for(unsigned i=0;i<4;++i)s+=std::string(i?",":"")+"\""+cancelReasons[i]+"\":["+std::to_string(cancelsByReason[i][0].load())+","+std::to_string(cancelsByReason[i][1].load())+"]";return s;}()<<"}"
     <<",\"property\":"<<c.asyncProperty.load()
     <<",\"streamNotLaunched\":"<<streamNotLaunchedReasons.json()<<",\"streamDeclined\":"<<streamDeclinedReasons.json()
     // averages: each sum / the count next to it
     <<",\"sums\":{\"replayRenderNs\":["<<c.renderNsSum[0]<<","<<c.renderNsSum[1]<<"],\"replayJobs\":["<<c.jobsPerStage[0]<<","<<c.jobsPerStage[1]
     <<"],\"joinWaitNs\":["<<c.joinWaitNsSum[0]<<","<<c.joinWaitNsSum[1]<<"],\"joinWaits\":["<<c.joinWaitCount[0]<<","<<c.joinWaitCount[1]
     <<"],\"workerRecordNs\":["<<c.workerRecordNsSum[0]<<","<<c.workerRecordNsSum[1]<<"],\"workerRecordJobs\":["<<c.workerRecordJobs[0]<<","<<c.workerRecordJobs[1]
     <<"],\"tailNs\":["<<c.tailNsSum[0]<<","<<c.tailNsSum[1]<<"],\"tails\":["<<c.tailCount[0]<<","<<c.tailCount[1]<<"]}"
     <<",\"ring\":{\"highWater\":"<<ringWatch.highWater()<<",\"lastUsage\":"<<ringWatch.usage()<<",\"watchResets\":"<<c.ringWatchResets<<",\"marginMin\":"<<(ringMarginMin.load()==INT64_MAX?0:ringMarginMin.load())
     <<",\"floorLast\":"<<regionFloorLast.load()<<",\"tight\":"<<c.ringTight<<",\"pressureJobs\":"<<c.ringPressureJobs<<",\"pressureFrames\":"<<c.ringPressureFrames<<"}"
     <<",\"worker\":{\"running\":"<<(workerRunning()?1:0)<<",\"nice\":"<<workerNice.load()<<",\"niceErrno\":"<<workerNiceErrno.load()
     <<",\"affinity\":"<<workerAffinity.load()<<",\"affinityErrno\":"<<workerAffinityErrno.load()<<",\"capacity\":["<<workerCapacityMin.load()<<","<<workerCapacityMax.load()<<"]"
     <<",\"affinityPolicy\":\""<<policyName(affinityPolicy.load())<<"\",\"affinityRequested\":"<<affinityRequested.load()<<",\"affinityBase\":"<<affinityBase.load()
     <<",\"affinityAll\":"<<affinityAll.load()<<",\"affinityChanges\":"<<cpuStats.affinityChanges.load()
     // Placement in effect (STEREO TEST mode it was resolved for), CPU topology, render-CPU following, priority changes
     <<",\"appliedMode\":"<<cpuStats.appliedMode.load()<<",\"wanted\":"<<cpuStats.requested.load()<<",\"follows\":"<<cpuStats.follows.load()
     <<",\"boostNice\":"<<cpuStats.boostNice.load()<<",\"streamNice\":"<<cpuStats.streamNice.load()<<",\"niceChanges\":"<<cpuStats.niceChanges.load()
     <<",\"inherited\":"<<cpuStats.inherited.load()<<",\"online\":"<<cpuStats.online.load()<<",\"big\":"<<cpuStats.big.load()<<",\"little\":"<<cpuStats.little.load()
     <<",\"mainMask\":"<<cpuStats.mainMask.load()<<",\"policy\":"<<cpuStats.workerPolicy.load()<<",\"capacities\":[";
    for(unsigned i=0;i<cpuSlots;++i)o<<(i?",":"")<<cpuStats.capacity[i].load();
    o<<"]}"
     // Streaming cadence in effect, CPU placement of the render thread (per left stage) and of the worker (per chunk)
     <<",\"tuning\":{\"publish\":"<<tuning.publish.load()<<",\"wake\":"<<tuning.wake.load()<<",\"pollUs\":"<<tuning.pollUs.load()<<",\"stream\":"<<tuning.streamMask.load()
     <<",\"effectiveStream\":"<<effectiveStreamMask()<<",\"nice\":"<<tuning.nice.load()<<",\"streamNice\":"<<tuning.streamNice.load()
     <<",\"affinity\":\""<<policyName(tuning.affinity.load())<<"\",\"forced\":"<<(tuning.forced.load()?1:0)<<"}";
    auto list=[&](const std::atomic<std::uint64_t>* a){o<<"[";for(unsigned i=0;i<cpuSlots;++i)o<<(i?",":"")<<a[i].load();o<<"]";};
    o<<",\"cpu\":{\"render\":";list(cpuStats.render);o<<",\"worker\":";list(cpuStats.worker);
    o<<",\"renderStages\":"<<cpuStats.renderStages.load()<<",\"renderMigrations\":"<<cpuStats.renderMigrations.load()
     <<",\"renderNivcsw\":"<<cpuStats.renderNivcsw.load()<<",\"renderNvcsw\":"<<cpuStats.renderNvcsw.load()
     <<",\"inStageChunks\":"<<cpuStats.inStageChunks.load()<<",\"sameCpu\":"<<cpuStats.sameCpu.load()<<",\"coldWaits\":"<<cpuStats.coldWaits.load()
     <<",\"renderPolicy\":"<<cpuStats.renderPolicy.load()<<",\"renderRtPriority\":"<<cpuStats.renderRtPriority.load()<<",\"renderNice\":"<<cpuStats.renderNice.load()
     <<",\"renderMask\":"<<cpuStats.renderMask.load()<<",\"freqKhz\":[";
    for(unsigned i=0;i<cpuSlots;++i){const auto n=cpuStats.freqSamples[i].load();o<<(i?",":"")<<(n?cpuStats.freqKhzSum[i].load()/n:0);}
    o<<"],\"freqSamples\":";list(cpuStats.freqSamples);o<<",\"freqErrno\":"<<cpuStats.freqErrno.load()<<"}";
    // Per STEREO TEST mode (frames after a 4-frame settle). Per-object us = depthCpuNs/depthObjects/1000; per-frame
    // ms = depthCpuNs/frames/1e6 (drawStageTimings bucket CPU, DETAIL profiler) and depthStageNs/stageFrames (threadCpu stock stage).
    o<<",\"byMode\":{";bool first=true;
    for(unsigned i=0;i<testModeCount;++i){const auto& m=byMode[i];
        if(!m.frames.load()&&!m.settling.load()&&!m.stageFrames.load()&&!m.jobs.load()&&!m.chunks.load())continue;
        o<<(first?"":",")<<"\""<<i<<"\":{\"name\":\""<<testModeName(i)<<"\",\"frames\":"<<m.frames.load()<<",\"settling\":"<<m.settling.load()
         <<",\"depthCpuNs\":"<<m.depthCpuNs.load()<<",\"depthObjects\":"<<m.depthObjects.load()<<",\"colorCpuNs\":"<<m.colorCpuNs.load()<<",\"colorObjects\":"<<m.colorObjects.load()
         <<",\"stageFrames\":"<<m.stageFrames.load()<<",\"depthStageNs\":"<<m.depthStageNs.load()<<",\"colorStageNs\":"<<m.colorStageNs.load()
         <<",\"stereoDrawFrames\":"<<m.stereoDrawFrames.load()<<",\"stereoDrawNs\":"<<m.stereoDrawNs.load()
         <<",\"jobs\":"<<m.jobs.load()<<",\"waited\":"<<m.waited.load()<<",\"joinWaitNs\":"<<m.joinWaitNs.load()<<",\"tails\":"<<m.tails.load()<<",\"tailNs\":"<<m.tailNs.load()
         <<",\"takeovers\":"<<m.takeovers.load()<<",\"fallbacks\":"<<m.fallbacks.load()<<",\"stages\":"<<m.stages.load()<<",\"nivcsw\":"<<m.nivcsw.load()<<",\"migrations\":"<<m.migrations.load()
         <<",\"chunks\":"<<m.chunks.load()<<",\"sameCpu\":"<<m.sameCpu.load()<<",\"workerNs\":"<<m.workerNs.load()<<"}";first=false;}
    o<<"}";
    // Per owner (0 = clone, 1 = clone2):
    // draws count finished jobs only (done), recordNs every chunk (also of cancelled jobs).
    o<<",\"byOwner\":[";
    for(unsigned k=0;k<2;++k)o<<(k?",":"")<<"{\"jobs\":"<<c.ownerJobs[k].load()<<",\"recordNs\":"<<c.ownerRecordNs[k].load()<<",\"draws\":"<<c.ownerDraws[k].load()<<",\"done\":"<<c.ownerDone[k].load()<<"}";
    o<<"]}";
    return o.str();
}
}
