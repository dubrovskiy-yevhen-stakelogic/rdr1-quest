#include "diagnostics/PerformanceProfile.h"
#include "StereoMailbox.h"
#include "GraphicsSettings.h"
#include "GameQueues.h"  // The game's own queue handles (split layouts)
#include "FramePacing.h" // presentTelemetry (stale resubmit, no-layer reasons)
#include "KgslClock.h"   // measurementPack (debug.rdrvr.perfmetrics) gates the latch classification
#include <algorithm>
#include <sstream>
#include <thread> // discardGpu yields to submit-gate waiters
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
namespace rdrvr::runtime {
namespace {
bool supported(VkFormat f){return f==VK_FORMAT_R8G8B8A8_UNORM || f==VK_FORMAT_R8G8B8A8_SRGB || f==VK_FORMAT_B8G8R8A8_UNORM || f==VK_FORMAT_B8G8R8A8_SRGB || f==VK_FORMAT_R16G16B16A16_SFLOAT;}
VkFormat storageFormat(VkFormat source,SourceEncoding encoding) {
    if(encoding==SourceEncoding::SrgbEncoded){if(source==VK_FORMAT_R8G8B8A8_UNORM)return VK_FORMAT_R8G8B8A8_SRGB;if(source==VK_FORMAT_B8G8R8A8_UNORM)return VK_FORMAT_B8G8R8A8_SRGB;}
    return source;
}
void transition(VkCommandBuffer c,VkImage image,VkImageLayout old,VkImageLayout next,VkAccessFlags src,VkAccessFlags dst,std::uint32_t layer=0) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=old;b.newLayout=next;
    b.srcAccessMask=src;b.dstAccessMask=dst;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,layer,1};
    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
// Teardown only: a failed wait never cancels submitted work, so anything but
// completion or device loss quarantines the handles (caller throws upward).
void waitTeardown(VkDevice device,VkFence fence) {
    const auto result=vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX);
    if(result!=VK_SUCCESS && result!=VK_ERROR_DEVICE_LOST)throw std::runtime_error("stereo GPU objects still in flight");
}
// Small non-negative integer property (debug overrides); fallback when unset/invalid.
int propertyInt(const char* name,int fallback) noexcept {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX]{};
    if(__system_property_get(name,v)<=0)return fallback;
    int n=0,digits=0;for(const char* c=v;*c;++c){if(*c<'0'||*c>'9'||++digits>4)return fallback;n=n*10+(*c-'0');}
    return digits?n:fallback;
#else
    (void)name;return fallback;
#endif
}
// True for an ANTI-ALIASING row value (0 OFF, 1 TAA, 2 TAA FULL, 3 TAA LITE) usable as a debug.rdrvr.aa override.
bool aaModeProperty(int p) noexcept {return p>=0&&p<int(GraphicsSettings::antiAliasingModes);}
static_assert(antiAliasingRowTaa==int(GraphicsSettings::antiAliasingTaa)&&antiAliasingRowTaaFull==int(GraphicsSettings::antiAliasingTaaFull)&&
              antiAliasingRowTaaLite==int(GraphicsSettings::antiAliasingTaaLite));
}
// GRAPHICS -> ANTI-ALIASING (re-read every 8 captures), debug.rdrvr.aa=<0..3> overrides it and
// debug.rdrvr.aa.subpix=<0..100> sets the FXAA subpixel amount in percent (re-read every 64 captures).
unsigned StereoMailbox::antiAliasingRequest() {
    if((aaPoll_&7)==0){
        aaMenu_=std::min(graphicsSettings.snapshot().requested.antiAliasing,GraphicsSettings::antiAliasingModes-1);
        if((aaPoll_&63)==0){
            aaProperty_=propertyInt("debug.rdrvr.aa",-1);
            antiAliasingProperty.store(aaModeProperty(aaProperty_)?aaProperty_:-1,std::memory_order_relaxed);
            const int subpix=propertyInt("debug.rdrvr.aa.subpix",-1);
            aaParams_.subpix=subpix>=0&&subpix<=100?float(subpix)/100.f:StereoAntiAliasing::Params{}.subpix;
        }
    }
    ++aaPoll_;
    // TAA runs on the render thread (vr/TaaPass.h, before this capture); the mailbox pass for the TAA rows
    // is a copy or FXAA (taaPostFxaa). The raw row value is published for the TAA pass.
    const unsigned row=aaModeProperty(aaProperty_)?unsigned(aaProperty_):aaMenu_;
    antiAliasingRequested.store(int(row),std::memory_order_relaxed);
    return GraphicsSettings::antiAliasingPost(row,taaPostFxaa.load(std::memory_order_relaxed)!=0);
}
// Adopt a finished background build of the format-pair objects; never waits.
void StereoMailbox::antiAliasingCollect() noexcept {
    try{
        if(!aaBuild_.valid()||aaBuild_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        try{auto shared=aaBuild_.get();aaCreateNs_=shared.buildNs;++aaCreates_;aa_.adopt(std::move(shared));}
        catch(...){aaBrokenSource_=aaBuildSource_;aaBrokenTarget_=aaBuildTarget_;++aaBuildFailures_;} // copy for this pair from now on
    }catch(...){}
}
// Shutdown only (the build thread never takes the mailbox lock).
void StereoMailbox::antiAliasingWaitBuild() noexcept {
    if(!aaBuild_.valid())return;
    try{auto shared=aaBuild_.get();StereoAntiAliasing::destroy(shared);}catch(...){}
}
void StereoMailbox::bind(const XrGraphicsBindingVulkan2KHR& b) {
    std::lock_guard guard(mutex_);
    // Once the bridge published the game's queues (GameQueues.h) they are the producer queues; the split
    // layouts bind the XR queue at index 0 of its own create-info. Unpublished: queues 0..queueIndex-1.
    std::array<VkQueue,GameQueues::capacity> game{};const unsigned published=gameQueueList(game.data(),unsigned(game.size()));
    if(device_ || !b.device || !b.physicalDevice || (!b.queueIndex&&!published))throw std::runtime_error("stereo binding requires reserved queue");
    device_=b.device;physical_=b.physicalDevice;family_=b.queueFamilyIndex;accepting_=true;
    warp_.bind(physical_,device_,family_);
    vkGetDeviceQueue(device_,family_,b.queueIndex,&consumerQueue_);
    if(published){for(unsigned i=0;i<published;++i)if(game[i]!=consumerQueue_)producerQueues_.push_back(game[i]);}
    else for(std::uint32_t i=0;i<b.queueIndex;++i){VkQueue q{};vkGetDeviceQueue(device_,family_,i,&q);producerQueues_.push_back(q);}
}
void StereoMailbox::commands() {
    if(pool_)return;
    // One timestamp lane per ring command buffer: producer slots, then consumers.
    gpuTiming_.initialize(physical_,device_,family_,slotCount+consumerCount);
    VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};p.queueFamilyIndex=family_;p.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    checkVk(vkCreateCommandPool(device_,&p,nullptr,&pool_),"stereo command pool");
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=pool_;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;
    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(auto& slot:slots_){
        checkVk(vkAllocateCommandBuffers(device_,&a,&slot.command),"stereo producer commands");
        checkVk(vkCreateFence(device_,&f,nullptr,&slot.fence),"stereo producer fence");
        checkVk(vkCreateSemaphore(device_,&s,nullptr,&slot.ready),"stereo ready");
        checkVk(vkCreateSemaphore(device_,&s,nullptr,&slot.done),"stereo done");
    }
    for(auto& c:consumers_){
        checkVk(vkAllocateCommandBuffers(device_,&a,&c.command),"stereo consumer commands");
        checkVk(vkCreateFence(device_,&f,nullptr,&c.fence),"stereo consumer fence");
    }
}
void StereoMailbox::begin(VkCommandBuffer c,unsigned lane) {
    checkVk(vkResetCommandBuffer(c,0),"stereo command reset");
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(c,&b),"stereo command begin");
    gpuTiming_.begin(c,lane);
}
void StereoMailbox::submit(VkQueue q,std::uint32_t count,const VkSubmitInfo* batches,VkFence fence) {
    diagnostics::WallScope submitTiming(diagnostics::frameTimings.stereoQueueSubmit);
    checkVk(vkQueueSubmit(q,count,batches,fence),"stereo submit");
}
// A submitted command buffer is observed complete exactly once: its timestamp
// lane is read and the slot/consumer becomes reusable. Errors = device loss.
bool StereoMailbox::observeSlot(unsigned i) {
    auto& s=slots_[i];if(!s.submitted)return true;
    const auto result=vkGetFenceStatus(device_,s.fence);
    if(result==VK_NOT_READY)return false;
    checkVk(result,"stereo copy fence");
    s.submitted=false;
    warp_.completed(i); // warpDepth lane, slot part (non-blocking query read after the fence)
    // Attribute the producer GPU time to the FXAA pass or the copy; drop the
    // per-capture source views before the captured images may be recycled.
    if(const auto ns=gpuTiming_.completed(i,false))(s.antiAliased?aaPassGpu_:copyPassGpu_).elapsed(1,ns);
    // When this pair became ready (capture-lane GPU end on CLOCK_MONOTONIC; else now = inexact) -> deadline bookkeeping
    // (a pending miss of this pair is published to the pacer now; exact ready delays feed the late-latch estimate).
    {const auto now=diagnostics::monotonicNs();const auto end=gpuTiming_.endMonotonicNs(i,now);
     s.readyExact=end!=0;s.readyNs=end?end:now;++(end?readyExact_:readyInexact_);
     deadline_.observed(s.sequence,s.publishedNs,s.readyNs,s.readyExact,pairDeadlines);}
    aa_.release(i);return true;
}
bool StereoMailbox::observeConsumer(unsigned i) {
    auto& c=consumers_[i];if(!c.submitted)return true;
    const auto result=vkGetFenceStatus(device_,c.fence);
    if(result==VK_NOT_READY)return false;
    checkVk(result,"stereo transfer fence");
    c.submitted=false;
    // Link the blit to its pair's capture end now - an in-flight pick's capture lane completes only after the pick (the consumer
    // waited the slot's ready semaphore, so that capture has finished; the slot cannot be recaptured while it is still the displayed one).
    if(c.link&&c.slot>=0&&unsigned(c.slot)<slotCount&&slots_[unsigned(c.slot)].sequence==c.seq){
        if(slots_[unsigned(c.slot)].submitted)observeSlot(unsigned(c.slot));
        gpuTiming_.link(slotCount+i,unsigned(c.slot));}
    c.link=false;gpuTiming_.completed(slotCount+i,true);return true;
}
unsigned StereoMailbox::framesInFlight() const {unsigned n=0;for(const auto& s:slots_)n+=s.submitted?1u:0u;return n;}
int StereoMailbox::acquireSlot() {
    // Never the displayed slot (reticle-expiry redraws read it again). Oldest
    // finished copy first; a never-used slot has sequence 0.
    int pick=-1,oldest=-1;
    for(unsigned i=0;i<slotCount;++i){
        if(int(i)==displayedSlot_ || slots_[i].mdToken)continue; // A reserved slot belongs to an in-frame capture
        if(oldest<0 || slots_[i].sequence<slots_[oldest].sequence)oldest=int(i);
        if(!observeSlot(i))continue;
        if(pick<0 || slots_[i].sequence<slots_[pick].sequence)pick=int(i);
    }
    if(pick>=0 || oldest<0)return pick;
    // The GPU is more than two publications behind: bounded wait, counted.
    ++slotWaits_;
    diagnostics::WallScope waitTiming(diagnostics::frameTimings.stereoFenceWait);
    const auto result=vkWaitForFences(device_,1,&slots_[oldest].fence,VK_TRUE,slotWaitNs);
    waitTiming.finish();
    if(result==VK_TIMEOUT){++slotTimeouts_;return -1;}
    checkVk(result,"stereo slot wait");
    return observeSlot(unsigned(oldest))?oldest:-1;
}
int StereoMailbox::acquireConsumer() {
    for(unsigned i=0;i<consumerCount;++i)if(observeConsumer(i))return int(i);
    return -1;
}
bool StereoMailbox::capture(VkQueue queue,const RenderSubmission& frame,std::uint64_t nowNs,SourceEncoding encoding,const StereoReticle& reticle,
                            StereoCaptureTicket* ticket,bool queueOrdered,bool sampledSources,const SpaceWarpSource* warp,const StereoDirectSlot* direct) {
    diagnostics::WallScope timing(diagnostics::frameTimings.stereoCapture);
    diagnostics::WallScope lockTiming(diagnostics::frameTimings.stereoLockWait);
    const std::uint64_t lateSeq=lateWaitSeq_.load(std::memory_order_acquire),lockStart=diagnostics::monotonicNs();
    std::lock_guard guard(mutex_);lockTiming.finish();noteProducerLock(lateSeq,lockStart);
    {const auto t=diagnostics::monotonicNs();producerLockUs_.add(std::uint32_t(std::min<std::uint64_t>((t>lockStart?t-lockStart:0)/1000,0xffffffffull)));}
    const std::array<Extent,2> extents{frame.eyes[0].extent,frame.eyes[1].extent};
    auto reject=[&]{++rejected_;return false;};
    if(!accepting_ || poisoned_ || !device_ || !nowNs || frame.frame.renderFrameId<=lastFrame_ || !frame.frame.xrFrameId ||
       !frame.frame.epochs.tracking || !frame.frame.epochs.referenceSpace || !frame.frame.epochs.resources ||
       frame.completion.kind!=CompletionKind::Fence || checkStereo(frame,frame.frame.epochs,extents)!=SubmissionCheck::Valid ||
       std::find(producerQueues_.begin(),producerQueues_.end(),queue)==producerQueues_.end())return reject();
    for(const auto& e:frame.eyes) {
        const auto layout=static_cast<VkImageLayout>(e.layout);
        if(e.queueFamily!=family_ || e.extent.width>8192 || e.extent.height>8192 || !supported(static_cast<VkFormat>(e.format)) ||
           (encoding==SourceEncoding::SrgbEncoded && e.format==VK_FORMAT_R16G16B16A16_SFLOAT) ||
           (layout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && layout!=VK_IMAGE_LAYOUT_GENERAL && layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))return reject();
        VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(physical_,storageFormat(static_cast<VkFormat>(e.format),encoding),&p);
        const auto feature=e.format==VK_FORMAT_R16G16B16A16_SFLOAT?VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT:VK_FORMAT_FEATURE_BLIT_SRC_BIT;
        if(!(p.optimalTilingFeatures&feature))return reject();
    }
    // A pending completion fence is accepted only when the caller
    // proves queue order (same-queue pass, or consumed covering semaphores).
    // The XR consumer then waits for THIS copy's fence, never the producer.
    const auto completion=vkGetFenceStatus(device_,reinterpret_cast<VkFence>(frame.completion.handle));
    const bool inFlight=completion==VK_NOT_READY && queueOrdered;
    if(completion!=VK_SUCCESS && !inFlight){++notReadyRejected_;return reject();}
    bool changed=!published_ || frame.frame.epochs.resources!=latest_.frame.epochs.resources;
    if(published_ && frame.frame.epochs.resources<latest_.frame.epochs.resources)return reject();
    for(unsigned i=0;i<2;++i)if(eyes_[i].extent!=extents[i] || eyes_[i].format!=storageFormat(static_cast<VkFormat>(frame.eyes[i].format),encoding)) {
        if(published_ && frame.frame.epochs.resources==latest_.frame.epochs.resources)return reject();
        changed=true;
    }
    // MAILBOX DIRECT: the pass already wrote the reserved slot -> never a ring rebuild (it would destroy images the pass may still
    // write) and never another slot. A stale reservation is rejected unconsumed (released by the capture slot at its GPU-proven retire).
    if(direct && (changed || !direct->valid || direct->generation!=generation_ || direct->slot>=slotCount || !direct->token ||
                  slots_[direct->slot].mdToken!=direct->token || slots_[direct->slot].submitted)){++mdStale_;return reject();}
    try {
        if(changed) {
            diagnostics::WallScope rebuildTiming(diagnostics::frameTimings.stereoRebuild);
            // The XR worker may have returned a layer but not called xrEndFrame
            // yet. Only that worker may replace the swapchains on its next frame.
            screensDirty_=true;screenValid_=false;discardGpu();
            for(unsigned i=0;i<2;++i){auto& e=eyes_[i];e.extent=extents[i];e.format=storageFormat(static_cast<VkFormat>(frame.eyes[i].format),encoding);}
            for(auto& slot:slots_)for(unsigned i=0;i<2;++i){
                const auto& e=eyes_[i];
                // COLOR_ATTACHMENT so the anti-aliasing pass can render into the slot.
                VkFormatProperties properties{};vkGetPhysicalDeviceFormatProperties(physical_,e.format,&properties);
                const VkImageUsageFlags attachment=(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:0;
                const auto usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|attachment|
                    (e.format==VK_FORMAT_R16G16B16A16_SFLOAT?VK_IMAGE_USAGE_SAMPLED_BIT:0);
                slot.images[i]=createOwnedImage(physical_,device_,e.extent,e.format,usage,VK_IMAGE_ASPECT_COLOR_BIT,false);
            }
        }
        commands();
        const int m=direct?int(direct->slot):acquireSlot(); // The reserved slot (not displayed, fence + consumer reads observed)
        if(m<0){status_="SLOT_TIMEOUT";return reject();}
        auto& s=slots_[unsigned(m)];
        // FXAA pass in place of the copy when requested and possible; every
        // other case keeps the copy (counted). Creation/view failures never poison.
        // No pipeline compilation here (engine submit hook, mailbox lock held): the
        // format-pair objects are built on a background thread and only polled; until they
        // land the pair is copied (fallback "building"). Per ring generation only cheap
        // views/framebuffers are created here; per capture two source views.
        bool antiAlias=false;const unsigned aaRequested=antiAliasingRequest();int aaReason=-1;
        // A direct pair has no separate source to filter (reserveDirect refuses while FXAA is requested; a menu change between
        // the reservation and this capture shows this one pair unfiltered, counted aaSkips).
        if(direct && aaRequested>=1)++mdAaSkips_;
        if(aaRequested>=1 && !direct){
            using Fallback=StereoAntiAliasing::Fallback;auto fallback=Fallback::Count;
            const auto sourceFormat=static_cast<VkFormat>(frame.eyes[0].format),targetFormat=eyes_[0].format;
            VkFormatProperties targetProperties{};vkGetPhysicalDeviceFormatProperties(physical_,targetFormat,&targetProperties);
            antiAliasingCollect();
            if(!sampledSources)fallback=Fallback::SourceNotSampled;
            else if(frame.eyes[1].format!=frame.eyes[0].format || eyes_[0].format!=eyes_[1].format)fallback=Fallback::EyeFormatsDiffer;
            else if(!(targetProperties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) || // slot images lack the attachment usage
                    !StereoAntiAliasing::supported(physical_,sourceFormat,targetFormat))fallback=Fallback::FormatUnsupported;
            else if(sourceFormat==aaBrokenSource_&&targetFormat==aaBrokenTarget_)fallback=Fallback::CreateFailed;
            else {
                // Another format pair: retire the old objects once no producer pass can still use them.
                if(aa_.ready()&&!aa_.matches(sourceFormat,targetFormat)&&!framesInFlight())aa_.destroy();
                if(!aa_.ready()&&!aaBuild_.valid()){
                    aaBuildSource_=sourceFormat;aaBuildTarget_=targetFormat;
                    try{aaBuild_=std::async(std::launch::async,[device=device_,sourceFormat,targetFormat]{return StereoAntiAliasing::build(device,sourceFormat,targetFormat,slotCount);});}
                    catch(const std::exception&){aaBrokenSource_=sourceFormat;aaBrokenTarget_=targetFormat;++aaBuildFailures_;} // no thread: copy
                }
                if(!aa_.matches(sourceFormat,targetFormat))fallback=Fallback::Building;
                else try{
                    if(!aa_.targetsReady()){
                        std::array<std::array<VkImage,2>,StereoAntiAliasing::maxSlots> targets{};
                        for(unsigned k=0;k<slotCount;++k)for(unsigned i=0;i<2;++i)targets[k][i]=slots_[k].images[i].image;
                        aa_.createTargets(targets,extents);++aaTargetCreates_;
                    }
                    aa_.prepare(unsigned(m),{reinterpret_cast<VkImage>(frame.eyes[0].image),reinterpret_cast<VkImage>(frame.eyes[1].image)},
                                {frame.eyes[0].arrayLayer,frame.eyes[1].arrayLayer});
                    antiAlias=true;
                }catch(const std::exception&){fallback=Fallback::ViewFailed;}
            }
            if(fallback!=Fallback::Count){++aaFallbacks_;++aaFallbackReasons_[unsigned(fallback)];aaReason=int(fallback);}
        }
        aaApplied_=antiAlias?1u:0u;
        // Effective mode for the GRAPHICS row and the byState signature.
        antiAliasingEffective.store(antiAlias?1:aaRequested?2:0,std::memory_order_relaxed);antiAliasingFallback.store(aaReason,std::memory_order_relaxed);
        antiAliasingDebug.store(antiAlias?int(aaParams_.debug):0,std::memory_order_relaxed); // Proof mode of this pair (byState _aa1d<mode>)
        begin(s.command,unsigned(m));
        for(unsigned i=0;i<2 && !direct;++i) { // A direct pair's eyes are already in s.images (written by the pass, GENERAL)
            const auto& source=frame.eyes[i];const auto target=s.images[i].image;const auto image=reinterpret_cast<VkImage>(source.image);
            const auto layout=static_cast<VkImageLayout>(source.layout);
            if(antiAlias){aa_.record(s.command,unsigned(m),i,image,layout,source.arrayLayer,aaParams_);continue;}
            transition(s.command,image,layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT,source.arrayLayer);
            transition(s.command,target,s.initialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,source.arrayLayer,1};copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.extent={source.extent.width,source.extent.height,1};
            vkCmdCopyImage(s.command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
            transition(s.command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,source.arrayLayer);
        }
        warp_.produce(s.command,unsigned(m),warp,frame.frame); // AppSW: slot depth + reprojection vs. previous pair
        // Direct mode: the pair also goes into the oldest ready free ticket (game queue); its XR frame then has no GPU work.
        int directTicket=-1;s.direct=s.directWarp=false;
        if(directMode_>0&&!changed&&!screensDirty_&&eyes_[0].format!=VK_FORMAT_R16G16B16A16_SFLOAT&&eyes_[1].format!=VK_FORMAT_R16G16B16A16_SFLOAT){
            for(unsigned k=0;k<ticketCount_;++k)if(!tickets_[k].seq){if(ticketReady(tickets_[k]))directTicket=int(k);break;}
            if(directTicket<0)++directNoTicket_;
            else{
                auto& tk=tickets_[unsigned(directTicket)];
                for(unsigned i=0;i<2;++i){
                    const auto& e=eyes_[i];const auto source=s.images[i].image;const auto target=e.images[tk.eye[i]].image;
                    // the slot copy / FXAA pass above wrote the source in GENERAL
                    transition(s.command,source,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
                    transition(s.command,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
                    VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                    blit.srcOffsets[1]=blit.dstOffsets[1]={static_cast<std::int32_t>(e.extent.width),static_cast<std::int32_t>(e.extent.height),1};
                    vkCmdBlitImage(s.command,source,VK_IMAGE_LAYOUT_GENERAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
                    transition(s.command,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                }
                s.directWarp=tk.warp.valid&&warp_.recordImages(s.command,unsigned(m),tk.warp);
                s.direct=true;++directCaptures_;
            }
        }
        gpuTiming_.end(s.command,unsigned(m));
        checkVk(vkEndCommandBuffer(s.command),"stereo capture end");
        s.antiAliased=antiAlias;
        if(antiAlias){++aaFrames_;if(eyes_[0].format==VK_FORMAT_R16G16B16A16_SFLOAT)++aaHdrFrames_;}else ++aaCopyFrames_;
        // [drain: the consumer skipped this slot's last ready signal]
        // {wait done (consumer read of the old pair); copy; signal ready}
        const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        std::array<VkSubmitInfo,2> batches{{{VK_STRUCTURE_TYPE_SUBMIT_INFO},{VK_STRUCTURE_TYPE_SUBMIT_INFO}}};std::uint32_t count=0;
        if(s.readyPending){auto& d=batches[count++];d.waitSemaphoreCount=1;d.pWaitSemaphores=&s.ready;d.pWaitDstStageMask=&stage;++readyDrains_;}
        auto& c=batches[count++];
        if(s.donePending){c.waitSemaphoreCount=1;c.pWaitSemaphores=&s.done;c.pWaitDstStageMask=&stage;++doneWaits_;}
        c.commandBufferCount=1;c.pCommandBuffers=&s.command;c.signalSemaphoreCount=1;c.pSignalSemaphores=&s.ready;
        checkVk(vkResetFences(device_,1,&s.fence),"stereo fence reset");
        submit(queue,count,batches.data(),s.fence);
        s.submitted=true;s.readyPending=true;s.donePending=false;s.initialized=true;
        s.sequence=++sequence_;s.frame=frame;s.reticle=reticle.forFrame(frame.frame,nowNs);s.publishedNs=nowNs;s.readyNs=0;s.readyExact=false;
        s.queue=queue;
        if(direct){s.mdToken=0;++mdCaptures_;} // Back in the ring with this pair's sequence (reservation consumed)
        if(directTicket>=0)tickets_[unsigned(directTicket)].seq=s.sequence; // Assigned (submitted with this pair)
        deadline_.captured(s.sequence,nowNs+std::uint64_t(inflightBiasNs_),pairDeadlines); // Miss of a latch that found nothing newer
        published_=true;latest_=frame;publishedNs_=nowNs;lastFrame_=frame.frame.renderFrameId;
        if(inFlight)++inFlightCaptures_;
        framesInFlightMax_=std::max<std::uint64_t>(framesInFlightMax_,framesInFlight());
        if(ticket)*ticket={generation_,s.sequence,unsigned(m)};
        diagnostics::stereoHistory.push(lastFrame_,"CAPTURED",timing.elapsedNs(),extents[0].width,extents[0].height,frame.frame.epochs.resources);
        ++captured_;status_="PAIR_CAPTURED";return true;
    }catch(...){poisoned_=true;screenValid_=false;status_="CAPTURE_FAILED";throw;}
}
// Under mutex_: this render-thread lock wait overlapped a late-latch wait if one was in progress when the call arrived
// (odd) or one started and ended while it waited for the lock (the XR thread changes lateWaitSeq_ only while holding mutex_).
void StereoMailbox::noteProducerLock(std::uint64_t lateSeqBefore,std::uint64_t lockStartNs) noexcept {
    if(!(lateSeqBefore&1) && lateWaitSeq_.load(std::memory_order_relaxed)==lateSeqBefore)return;
    const auto now=diagnostics::monotonicNs();
    ++late_.producerBlocked;late_.producerBlockedUs.add(std::uint32_t(std::min<std::uint64_t>((now>lockStartNs?now-lockStartNs:0)/1000,0xffffffffull)));
}
bool StereoMailbox::waitNewestCopy(VkQueue queue,std::uint64_t timeoutNs) noexcept {
    VkFence fence{};VkDevice device{};
    {std::lock_guard guard(mutex_);
     if(!device_||poisoned_||!sequence_||gatedSeq_==sequence_)return false;
     gatedSeq_=sequence_;++gateChecks_;
     for(auto& s:slots_)if(s.sequence==sequence_){
         if(s.queue==queue&&s.submitted&&vkGetFenceStatus(device_,s.fence)==VK_NOT_READY){fence=s.fence;device=device_;externalWaits_.fetch_add(1,std::memory_order_acq_rel);}
         break;}
    }
    if(!fence)return false;
    const auto t0=diagnostics::monotonicNs();
    const auto r=vkWaitForFences(device,1,&fence,VK_TRUE,timeoutNs);
    const auto t1=diagnostics::monotonicNs();
    externalWaits_.fetch_sub(1,std::memory_order_acq_rel);
    std::lock_guard guard(mutex_);++gateWaits_;if(r==VK_TIMEOUT)++gateTimeouts_;
    gateWaitUs_.add(std::uint32_t(std::min<std::uint64_t>((t1>t0?t1-t0:0)/1000,0xffffffffull)));
    return true;
}
// busy attribution (see mdHold_ in the header). Holder: set (relaxed) ... Clear (seq_cst), then CAS its mark. Render thread: read the
// hold (seq_cst), store the mark (seq_cst), re-read: still that hold -> the holder's later CAS sees the mark; else whichever CAS wins.
StereoMailbox::MdHold::MdHold(const StereoMailbox& mb,unsigned p) noexcept:m(mb),id(++mb.mdHoldIds_),startNs(diagnostics::monotonicNs()) {phase(p);}
void StereoMailbox::MdHold::end() noexcept {
    if(!id)return;
    m.mdHold_.store(0,std::memory_order_seq_cst);
    auto mark=id;
    if(m.mdBusyMark_.compare_exchange_strong(mark,0,std::memory_order_seq_cst)){
        const auto now=diagnostics::monotonicNs(),busy=std::max(m.mdBusyNs_.load(std::memory_order_relaxed),startNs);
        const unsigned p=std::min(m.mdBusyPhaseSeen_.load(std::memory_order_relaxed),unsigned(MdpCount-1));
        const auto us=[](std::uint64_t ns){return std::uint32_t(std::min<std::uint64_t>(ns/1000,0xffffffffull));};
        const auto bucket=[](std::uint32_t v){unsigned b=0;while(b<mdHoldEdgesUs.size()&&v>=mdHoldEdgesUs[b])++b;return b;};
        auto& st=m.mdHoldStats_;const auto hold=us(now>startNs?now-startNs:0),remain=us(now>busy?now-busy:0);
        ++st.measured[p];st.holdUs[p].add(hold);st.allHoldUs.add(hold);st.remainUs.add(remain);++st.holdBuckets[bucket(hold)];++st.remainBuckets[bucket(remain)];
    }
    id=0;
}
void StereoMailbox::noteBusy(std::uint64_t nowNs) noexcept {
    const auto h=mdHold_.load(std::memory_order_seq_cst);const unsigned phase=h?std::min(unsigned(h&0xff),unsigned(MdpCount-1)):unsigned(MdpOther);
    mdBusyPhase_[phase].fetch_add(1,std::memory_order_relaxed);
    if(!h)return;
    const auto id=h>>8;mdBusyNs_.store(nowNs,std::memory_order_relaxed);mdBusyPhaseSeen_.store(phase,std::memory_order_relaxed);
    mdBusyMark_.store(id,std::memory_order_seq_cst);
    if((mdHold_.load(std::memory_order_seq_cst)>>8)!=id){auto mark=id;if(mdBusyMark_.compare_exchange_strong(mark,0,std::memory_order_seq_cst))mdBusyMissed_.fetch_add(1,std::memory_order_relaxed);}
}
void StereoMailbox::WaitHistogram::add(std::uint32_t us) noexcept {
    n[std::min(us/stepUs,buckets-1)].fetch_add(1,std::memory_order_relaxed);total.fetch_add(1,std::memory_order_relaxed);sumUs.fetch_add(us,std::memory_order_relaxed);
    auto m=max.load(std::memory_order_relaxed);while(us>m&&!max.compare_exchange_weak(m,us,std::memory_order_relaxed)){}
}
// MAILBOX DIRECT (see mdToken in the header). The pass writes the slot BEFORE capture()'s batch waits the slot's done semaphore,
// so every earlier reader must be CPU-observed complete here: the producer fence (old copy + warp pass) and each consumer that read it.
bool StereoMailbox::reserveDirect(VkQueue queue,const std::array<Extent,2>& extents,const std::array<VkFormat,2>& formats,SourceEncoding encoding,
                                  std::uint64_t resources,StereoDirectSlot& out,bool retry) noexcept {
    out={};
    std::unique_lock guard(mutex_,std::try_to_lock); // never waits behind the XR thread's render() (late latch, image waits)
    if(!guard.owns_lock()){
        const auto t0=diagnostics::monotonicNs();noteBusy(t0); // Who holds it (always on)
        if(retry){ // Mode 2: bounded try_lock + yield, never blocks (deadlock-free); busy = only the retries that ran out
            mdBusyRetried_.fetch_add(1,std::memory_order_relaxed);
            while(!guard.try_lock()&&diagnostics::monotonicNs()-t0<mdRetryNs)std::this_thread::yield();
            mdBusyWaitUs_.add(std::uint32_t(std::min<std::uint64_t>((diagnostics::monotonicNs()-t0)/1000,0xffffffffull)));
            if(guard.owns_lock())mdBusyWon_.fetch_add(1,std::memory_order_relaxed);
        }
        if(!guard.owns_lock()){mdBusy_.fetch_add(1,std::memory_order_relaxed);return false;}
    }
    ++mdRequests_;const auto refuse=[&](MdWhy why){++mdRefused_[why];return false;};
    if(!accepting_ || poisoned_ || !device_ || !pool_ || !published_ ||
       std::find(producerQueues_.begin(),producerQueues_.end(),queue)==producerQueues_.end())return refuse(MdState);
    // capture() of this pair must not rebuild the ring: same resources epoch, extents and storage formats; no HDR (sampled transfer path)
    if(latest_.frame.epochs.resources!=resources)return refuse(MdConfig);
    for(unsigned i=0;i<2;++i)if(eyes_[i].extent!=extents[i] || eyes_[i].format!=storageFormat(formats[i],encoding) || eyes_[i].format==VK_FORMAT_R16G16B16A16_SFLOAT ||
                               !supported(formats[i]) || formats[i]==VK_FORMAT_R16G16B16A16_SFLOAT)return refuse(MdConfig);
    // FXAA reads a separate source: refused while the last pair ran it or it is requested (cached request, antiAliasingRequest() not polled)
    {const unsigned row=aaModeProperty(aaProperty_)?unsigned(aaProperty_):aaMenu_;
     if(aaApplied_ || GraphicsSettings::antiAliasingPost(row,taaPostFxaa.load(std::memory_order_relaxed)!=0)>=1)return refuse(MdAa);}
    try{
        if(mdOrphans_.capacity()<2*slotCount)mdOrphans_.reserve(2*slotCount); // discardGpu() never allocates for an orphan
        int pick=-1;
        for(unsigned i=0;i<slotCount;++i){
            const auto& s=slots_[i];
            if(int(i)==displayedSlot_ || s.mdToken)continue;
            if(s.sequence && s.sequence>displayedSequence_ && s.sequence>=minSequence_)continue; // its pair may still be latched
            bool ticket=false;for(unsigned k=0;k<ticketCount_;++k)ticket|=s.sequence && tickets_[k].seq==s.sequence;
            if(ticket || !observeSlot(i))continue;
            bool reading=false;for(unsigned c=0;c<consumerCount;++c)if(consumers_[c].submitted && consumers_[c].slot==int(i) && !observeConsumer(c))reading=true;
            if(reading)continue;
            if(pick<0 || s.sequence<slots_[unsigned(pick)].sequence)pick=int(i);
        }
        if(pick<0)return refuse(MdNoSlot);
        auto& s=slots_[unsigned(pick)];
        s.sequence=0;s.mdToken=++mdNextToken_; // out of the ring: every latch path skips sequence 0, acquireSlot() skips mdToken
        out.generation=generation_;out.token=s.mdToken;out.slot=unsigned(pick);out.valid=true;
        for(unsigned i=0;i<2;++i){out.images[i]=s.images[i].image;out.initialized[i]=s.initialized;}
        ++mdReserved_;return true;
    }catch(...){poisoned_=true;screenValid_=false;status_="DIRECT_FENCE_LOST";out={};return false;} // device lost (fence polls)
}
void StereoMailbox::releaseDirect(const StereoDirectSlot& d) noexcept {
    if(!d.valid || !d.token)return;
    std::lock_guard guard(mutex_);
    // Still in the ring: free again (sequence 0 = the oldest slot for the next capture; a layout from UNDEFINED is always legal).
    for(auto& s:slots_)if(s.mdToken==d.token){s.mdToken=0;++mdReleased_;return;}
    // Orphaned by a ring rebuild: nothing writes the images any more.
    for(std::size_t i=0;i<mdOrphans_.size();++i)if(mdOrphans_[i].token==d.token){
        if(device_)for(auto& image:mdOrphans_[i].images)destroyOwnedImage(device_,image);
        mdOrphans_[i]=mdOrphans_.back();mdOrphans_.pop_back();++mdOrphansFreed_;return;}
}
bool StereoMailbox::copyComplete(const StereoCaptureTicket& ticket) noexcept {
    const std::uint64_t lateSeq=lateWaitSeq_.load(std::memory_order_acquire),lockStart=diagnostics::monotonicNs();
    std::lock_guard guard(mutex_);noteProducerLock(lateSeq,lockStart);
    // Rebuild waited every fence; slot reuse observed this copy's fence first.
    if(ticket.generation!=generation_ || ticket.slot>=slotCount || slots_[ticket.slot].sequence!=ticket.sequence)return true;
    try{return observeSlot(ticket.slot);}
    catch(...){poisoned_=true;status_="COPY_FENCE_LOST";return true;} // device lost: nothing executes any more
}
bool StereoMailbox::waitCopy(const StereoCaptureTicket& ticket,std::uint64_t timeoutNs) noexcept {
    std::lock_guard guard(mutex_);
    if(ticket.generation!=generation_ || ticket.slot>=slotCount || slots_[ticket.slot].sequence!=ticket.sequence || !slots_[ticket.slot].submitted)return true;
    diagnostics::WallScope waitTiming(diagnostics::frameTimings.stereoFenceWait);
    const auto result=vkWaitForFences(device_,1,&slots_[ticket.slot].fence,VK_TRUE,timeoutNs);
    waitTiming.finish();
    if(result==VK_TIMEOUT)return false;
    try{return result==VK_SUCCESS && observeSlot(ticket.slot);}
    catch(...){poisoned_=true;status_="COPY_FENCE_LOST";return false;}
}
bool StereoMailbox::releaseSources(const std::array<VkImage,2>& images,std::uint64_t timeoutNs) noexcept {
    std::lock_guard guard(mutex_);
    if(!device_)return true;
    bool ok=true;
    for(unsigned i=0;i<slotCount;++i){
        if(!aa_.references(i,images))continue;
        auto& s=slots_[i];
        if(s.submitted){
            const auto result=vkWaitForFences(device_,1,&s.fence,VK_TRUE,timeoutNs);
            if(result==VK_TIMEOUT){ok=false;continue;}
            try{if(!observeSlot(i)){ok=false;continue;}} // observeSlot releases the views
            catch(...){poisoned_=true;status_="COPY_FENCE_LOST";} // device lost: nothing executes any more
        }
        aa_.release(i);++aaSourceReleases_;
    }
    return ok;
}
void StereoMailbox::screens(XrSession session) {
    if(!screensDirty_ && eyes_[0].swapchain && eyes_[1].swapchain)return;
    chainsProperty_=unsigned(std::clamp(propertyInt("debug.rdrvr.xrchains",1),1,2));
    diagnostics::WallScope timing(diagnostics::frameTimings.stereoScreens);
    if(screensDirty_){discardScreens();screensDirty_=false;}
    std::uint32_t n{};checkXr(xrEnumerateSwapchainFormats(session,0,&n,nullptr),"stereo format count");
    if(!n || n>256)throw std::runtime_error("stereo format count invalid");
    std::vector<std::int64_t> formats(n);checkXr(xrEnumerateSwapchainFormats(session,n,&n,formats.data()),"stereo formats");
    for(unsigned eye=0;eye<2;++eye) {
        auto& e=eyes_[eye];
        // HDR always uses an explicit stateless display curve. Prefer an sRGB
        // attachment; UNORM fallback encodes sRGB in the fragment shader once.
        const bool hdr=e.format==VK_FORMAT_R16G16B16A16_SFLOAT;
        std::array<VkFormat,5> candidates{e.format,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM};
        for(auto f:candidates) {
            if(hdr && f==e.format)continue;
            VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(physical_,f,&p);
            const auto feature=hdr?VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT:VK_FORMAT_FEATURE_BLIT_DST_BIT;
            if(std::find(formats.begin(),formats.end(),f)!=formats.end() && (p.optimalTilingFeatures&feature)){e.xrFormat=f;break;}
        }
        if(e.xrFormat==VK_FORMAT_UNDEFINED)throw std::runtime_error("stereo XR format unsupported");
        XrSwapchainCreateInfo c{XR_TYPE_SWAPCHAIN_CREATE_INFO};c.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        c.format=e.xrFormat;c.sampleCount=c.faceCount=c.arraySize=c.mipCount=1;c.width=e.extent.width;c.height=e.extent.height;
        checkXr(xrCreateSwapchain(session,&c,&e.swapchain),"stereo XR swapchain");
        checkXr(xrEnumerateSwapchainImages(e.swapchain,0,&n,nullptr),"stereo XR images count");
        if(!n || n>16)throw std::runtime_error("stereo XR image count invalid");
        e.images.resize(n,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        checkXr(xrEnumerateSwapchainImages(e.swapchain,n,&n,reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images.data())),"stereo XR images");
        // One sampled-source transfer per ring slot (each binds its own image).
        if(hdr)for(auto& slot:slots_)slot.hdr[eye].create(device_,slot.images[eye].image,e.extent,e.xrFormat,e.images);
        // Second chain (blit path only: the HDR transfers bind the first chain's images).
        if(!hdr&&chainsProperty_>=2){
            checkXr(xrCreateSwapchain(session,&c,&e.swapchain2),"stereo XR swapchain 2");
            checkXr(xrEnumerateSwapchainImages(e.swapchain2,0,&n,nullptr),"stereo XR images 2 count");
            if(!n || n>16)throw std::runtime_error("stereo XR image count invalid");
            e.images2.resize(n,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
            checkXr(xrEnumerateSwapchainImages(e.swapchain2,n,&n,reinterpret_cast<XrSwapchainImageBaseHeader*>(e.images2.data())),"stereo XR images 2");
        }
    }
}
bool StereoMailbox::render(XrSession session,std::uint64_t tracking,std::uint64_t space,std::uint64_t nowNs,StereoLayer& out) {
    diagnostics::WallScope timing(diagnostics::frameTimings.stereoRender);
    diagnostics::WallScope lockTiming(diagnostics::frameTimings.stereoLockWait);
    std::lock_guard guard(mutex_);lockTiming.finish(); // Held throughout, the late-latch wait included
    MdHold hold(*this,MdpRenderLatch); // Busy attribution (phases below)
    // the caller sampled nowNs before this lock; a producer may have
    // published (with its own, later stamp) meanwhile. Re-sample under the lock
    // (same steady clock) so a fresh pair never looks like it is from the future.
    nowNs=std::max<std::uint64_t>(nowNs,diagnostics::monotonicNs());
    // What this call decided (presentTelemetry.lastReason, read by XrHarness right after render()).
    auto& telemetry=presentTelemetry;
    const auto reason=[&](NoLayer r){telemetry.lastReason.store(std::uint8_t(r),std::memory_order_relaxed);};
    telemetry.lastNewPair.store(false,std::memory_order_relaxed);telemetry.lastBlit.store(false,std::memory_order_relaxed);
    lastLatch_.store(std::uint8_t(Latch::None),std::memory_order_relaxed); // Early-outs below stay None
    if(poisoned_ || !published_ || !publishedNs_){reason(NoLayer::NoScreen);return false;}
    const auto age=nowNs>publishedNs_?nowNs-publishedNs_:0;
    // Epoch mismatch: the stored poses belong to an old tracking/reference space -> never shown.
    if(latest_.frame.epochs.tracking!=tracking || latest_.frame.epochs.referenceSpace!=space){
        ++epochRejected_;telemetry.epochRejects.fetch_add(1,std::memory_order_relaxed);reason(NoLayer::Epoch);return false;}
    if(age>maxAgeNs){
        ++epochRejected_;telemetry.ageRejects.fetch_add(1,std::memory_order_relaxed);
        // Stale resubmit (ADAPTIVE or debug.rdrvr.staleresubmit=1): keep the last displayed pair of these
        // epochs on screen (the compositor timewarps it) instead of a frame without a layer. No new pair is picked here.
        if(telemetry.staleEnabled.load(std::memory_order_relaxed) && screenValid_ && displayedEpochs_.tracking==tracking && displayedEpochs_.referenceSpace==space){
            telemetry.staleResubmits.fetch_add(1,std::memory_order_relaxed);reason(NoLayer::Stale);out=displayed_;return true;}
        reason(NoLayer::Age);return false;
    }
    reason(NoLayer::None);
    try {
        screens(session);
        warp_.screens(session); // Motion-vector/depth swapchains (no-op unless SPACEWARP is active)
        // Newest pair newer than the displayed one whose copy has finished.
        // oldest = the oldest newer pair (late-latch target); due = the oldest newer pair without a deadline sample yet.
        int pick=-1,newest=-1,oldest=-1,due=-1,pickOld=-1; // pickOld = the oldest finished newer pair (FIFO latch)
        // Properties (1 Hz). In-flight latch: while AppSW synthesizes, a SUBMITTED pair is latchable (the consumer batch
        // waits its ready semaphore on the GPU); FIFO and the late latch then never apply (pick = newest whenever anything is newer).
        if(nowNs>=fifoNextPropNs_){fifoNextPropNs_=nowNs+1'000'000'000ull;fifoMode_=propertyInt("debug.rdrvr.latchfifo",1);
            prefetchMode_=propertyInt("debug.rdrvr.xrprefetch",1);
            noBlit_=propertyInt("debug.rdrvr.xrnoblit",0);
            directMode_=propertyInt("debug.rdrvr.xrdirect",0);
            latchDepth_=std::clamp(propertyInt("debug.rdrvr.latch.depth",1),1,3);
            inflightMode_=propertyInt("debug.rdrvr.latchinflight",1);inflightBiasNs_=std::int64_t(std::min(propertyInt("debug.rdrvr.latchinflight.biasus",2000),10000))*1000;}
        const bool synth=spaceWarpControl.synthesizing(),inflight=inflightMode_>0&&synth;
        const auto scan=[&]{
            pick=newest=oldest=due=pickOld=-1;
            for(unsigned i=0;i<slotCount;++i){
                const auto& s=slots_[i];
                if(!s.sequence || s.sequence<=displayedSequence_ || s.sequence<minSequence_ ||
                   s.frame.frame.epochs.tracking!=tracking || s.frame.frame.epochs.referenceSpace!=space)continue;
                if(newest<0 || s.sequence>slots_[newest].sequence)newest=int(i);
                if(oldest<0 || s.sequence<slots_[oldest].sequence)oldest=int(i);
                if(s.sequence>deadline_.sampled && (due<0 || s.sequence<slots_[due].sequence))due=int(i);
                const bool finished=observeSlot(i);
                if(finished || inflight){
                    if(pick<0 || s.sequence>slots_[pick].sequence)pick=int(i);}
                if(finished && (pickOld<0 || s.sequence<slots_[pickOld].sequence))pickOld=int(i);
            }};
        scan();
        // Deadline sample: this latch's time and the due pair's state at the pick (before any wait).
        const int dueSlot=due;const std::uint64_t latchNs=nowNs,dueSeq=due>=0?slots_[unsigned(due)].sequence:0;
        const bool dueReadyAtPick=due>=0&&!slots_[unsigned(due)].submitted;
        // Late latch: nothing newer finished, a newer pair in flight -> bounded wait for the oldest one's copy
        // fence (LateLatchPolicy: AppSW synthesizing at half rate only, XR frame / pdt guards, exact ready-delay estimate, expected
        // completion within budget). ONE fence wait of at most the budget, holding the lock throughout (no slices, no
        // unlock / re-lock: the fence cannot be reset or destroyed meanwhile and this thread's time in render() is bounded by the
        // budget after its entry lock). A capture() / copyComplete() arriving meanwhile blocks for up to the budget (counted).
        if(pick<0 && oldest>=0){
            const unsigned slot=unsigned(oldest);
            LateLatchPolicy::Inputs in{};
            in.latchNs=framePacer.lateLatchNs();in.synth=synth;in.halfRate=framePacer.halfRate();in.nowNs=nowNs;in.xrWakeNs=framePacer.lastXrWakeNs();
            in.xrIntervalNs=framePacer.xrIntervalNs();in.pdtNs=framePacer.lastPdtNs();in.pendingPublishedNs=slots_[slot].publishedNs;in.readyDelayNs=deadline_.readyDelayNs(25);
            LateLatchPolicy::Why why{};const std::int64_t budget=LateLatchPolicy::budget(in,&why);
            ++late_.why[why];
            if(budget>0){
                const std::uint64_t t0=diagnostics::monotonicNs();
                ++late_.waits;
                hold.phase(MdpRenderLateWait);lateWaitSeq_.fetch_add(1,std::memory_order_release); // odd: waiting (producer lock waits are attributed)
                const auto r=vkWaitForFences(device_,1,&slots_[slot].fence,VK_TRUE,std::uint64_t(budget));
                lateWaitSeq_.fetch_add(1,std::memory_order_release);hold.phase(MdpRenderLatch); // even again before anything can throw
                const auto t1=diagnostics::monotonicNs();const auto us=[](std::uint64_t ns){return std::uint32_t(std::min<std::uint64_t>(ns/1000,0xffffffffull));};
                if(r!=VK_SUCCESS && r!=VK_TIMEOUT)checkVk(r,"stereo late latch");
                late_.waitUs.add(us(t1-t0));
                if(r==VK_SUCCESS && observeSlot(slot)){
                    ++late_.hits;late_.hitWaitUs.add(us(t1-t0));
                    if(slots_[slot].readyExact){if(t1>=slots_[slot].readyNs)late_.clockUs.add(us(t1-slots_[slot].readyNs));else ++late_.clockNegative;}
                }else ++late_.timeouts;
                if(in.pdtNs>0)late_.pdtLeadUs.add(us(std::uint64_t(std::max<std::int64_t>(0,in.pdtNs-std::int64_t(t1)))));
                nowNs=std::max(nowNs,t1);
                scan();
            }
        }
        // The due pair's deadline sample (one per pair, AppSW synthesizing only): finished at the pick, finished during the
        // wait (rescued miss), or still in flight (pending miss, published when observeSlot sees it finish).
        // In-flight: the due pair is latchable at its capture SUBMIT, margin = latch - submit - bias (exact CPU clock); a latch with
        // nothing newer captured makes the next capture a miss (DeadlineTracker::captured in capture()).
        // The sample is the SHOWN pair (pick = newest); a skipped older pair is never sampled (sampled = the pick). Mode
        // switches drop pending bookkeeping of the other margin kind (GPU-ready vs. capture-submit margins never mix).
        if(inflight!=lastInflight_){deadline_.forgetAll();lastInflight_=inflight;}
        if(inflight){
            if(pick>=0){
                const auto& d=slots_[unsigned(pick)];
                deadline_.latch(latchNs,d.sequence,DeadlineTracker::DueReady,d.publishedNs+std::uint64_t(inflightBiasNs_),true,true,pairDeadlines);
            }else if(newest<0){deadline_.nothingNewer(latchNs,true);++inflightNothing_;}
        }else
        if(synth&&dueSlot>=0&&slots_[unsigned(dueSlot)].sequence==dueSeq){
            const auto& d=slots_[unsigned(dueSlot)];
            const unsigned state=dueReadyAtPick?DeadlineTracker::DueReady:!d.submitted?DeadlineTracker::DueRescued:DeadlineTracker::DueInFlight;
            deadline_.latch(latchNs,dueSeq,state,d.readyNs,d.readyExact,true,pairDeadlines);
        }
        // FIFO latch: while AppSW synthesizes, show the oldest finished unshown pair (every rendered pair reaches the compositor in
        // order; the AppSW motion vectors are relative to the previous produced pair, so in-order display also keeps them right). Never more
        // than one pair behind the newest finished one: then take the pair right before the newest (latency bounded to one slot).
        if(inflight&&pick>=0){++inflightPicks_;if(slots_[unsigned(pick)].submitted)++inflightUnfinished_;}
        if(fifoMode_>0 && synth && !inflight && pick>=0 && pickOld>=0 && pickOld!=pick){
            ++fifoPicks_;
            const auto newestSeq=slots_[unsigned(pick)].sequence;int choose=pickOld;
            if(newestSeq-slots_[unsigned(pickOld)].sequence>=2){ // more than one behind: skip ahead to newest-1 if it is finished
                int prev=-1;for(unsigned i=0;i<slotCount;++i){const auto& s=slots_[i];
                    if(s.sequence+1==newestSeq && s.sequence>displayedSequence_ && !s.submitted && s.frame.frame.epochs.tracking==tracking &&
                       s.frame.frame.epochs.referenceSpace==space){prev=int(i);break;}}
                choose=prev>=0?prev:pick;++fifoSkipAhead_;}
            if(choose!=pick){++fifoOlder_;pick=choose;}
        }
        // Fixed depth >= 2 overrides the pick: the newest unshown pair rendered from views of XR frame <= now - D (same epochs).
        if(latchDepth_>=2&&xrFrameNow_>std::uint64_t(latchDepth_)){
            int sel=-1;const std::uint64_t limit=xrFrameNow_-std::uint64_t(latchDepth_);
            for(unsigned i=0;i<slotCount;++i){const auto& s=slots_[i];
                if(!s.sequence||s.sequence<=displayedSequence_||s.sequence<minSequence_||s.frame.frame.epochs.tracking!=tracking||
                   s.frame.frame.epochs.referenceSpace!=space||s.frame.frame.xrFrameId>limit)continue;
                if(sel<0||s.sequence>slots_[unsigned(sel)].sequence)sel=int(i);}
            pick=sel;if(sel>=0)++depthPicks_;else ++depthNone_;
        }
        if(newest>=0 && newest!=pick)++consumerNotReady_;
        bool latchedNew=false,consumerBusyPick=false;
        bool removeExpired=false;
        if(pick<0 && screenValid_ && displayedReticle_ && displayedSlot_>=0 && slots_[displayedSlot_].sequence==displayedSequence_){
            const auto& d=slots_[displayedSlot_];const auto current=d.reticle.forFrame(d.frame.frame,nowNs);
            removeExpired=!validReticle(current.eyes[0]) && !validReticle(current.eyes[1]);
        }
        // Tickets before anything else (see Ticket in the header). assigned = the prefix of tickets holding captured pairs.
        int own=-1;bool blocked=false;unsigned assigned=0;
        if(pick>=0 || removeExpired){
            const std::uint64_t useSeq=slots_[unsigned(pick>=0?pick:displayedSlot_)].sequence;
            bool newer=false;
            for(unsigned k=0;k<ticketCount_&&tickets_[k].seq;++k){++assigned;
                if(pick>=0&&tickets_[k].seq==useSeq)own=int(k);else if(tickets_[k].seq>useSeq)newer=true;}
            blocked=own<0&&assigned&&(pick<0||newer); // [0, own] releases in order even with newer tickets behind
            // A newer pair holds a ticket: the picked (older, ring) pair cannot be released before it -> the newest FINISHED
            // direct pair instead (FIFO latch / reticle redraw lose to release order), else a repeat
            if(blocked&&pick>=0){
                int best=-1,bestSlot=-1;
                for(unsigned k=0;k<assigned;++k)for(unsigned i=0;i<slotCount;++i)
                    if(slots_[i].sequence==tickets_[k].seq&&slots_[i].direct&&slots_[i].sequence>displayedSequence_&&slots_[i].sequence>=minSequence_&&
                       observeSlot(i)&&slots_[i].frame.frame.epochs.tracking==tracking&&slots_[i].frame.frame.epochs.referenceSpace==space){best=int(k);bestSlot=int(i);}
                if(best>=0){pick=bestSlot;own=best;blocked=false;++directRescued_;}
            }
        }
        // A ticket goes back only when its pair's write finished (in-flight latch picks, two producer queues): else a repeat
        if(!blocked&&(own>=0||assigned)){
            const unsigned upto=own>=0?unsigned(own)+1:assigned;
            for(unsigned k=0;k<upto&&!blocked;++k)for(unsigned i=0;i<slotCount;++i)
                if(slots_[i].sequence==tickets_[k].seq&&!observeSlot(i)){blocked=true;break;}
        }
        if(blocked){++directBlocked_;}
        else if(own>=0){
            // Direct pair: finished (picked as finished), written by its capture -> release tickets [0, own]; no GPU work here
            releaseTickets(unsigned(own)+1);
            auto& s=slots_[unsigned(pick)];
            if(displayedSequence_ && s.sequence>displayedSequence_+1){consumerSkipped_+=s.sequence-displayedSequence_-1;
                if(spaceWarpControl.active())warp_.noteConsumerSkips(s.sequence-displayedSequence_-1);}
            displayedSlot_=pick;displayedSequence_=s.sequence;displayedEpochs_=s.frame.frame.epochs;displayed_.frame=s.frame.frame;
            displayed_.warp=s.directWarp?warp_.layer(unsigned(pick)):SpaceWarpLayer{};
            for(unsigned i=0;i<2;++i){displayed_.swapchains[i]=eyes_[i].swapchain;displayed_.poses[i]=s.frame.eyes[i].renderedPose;displayed_.fov[i]=s.frame.eyes[i].renderedFov;displayed_.extent[i]=s.frame.eyes[i].extent;}
            ++copied_;++directLatches_;if(s.directWarp)++directWarpLatches_;latchedNew=true;displayedPublishedNs_=s.publishedNs;
            telemetry.lastNewPair.store(true,std::memory_order_relaxed);
            diagnostics::stereoHistory.push(s.frame.frame.renderFrameId,"DISPLAYED",timing.elapsedNs(),eyes_[0].extent.width,eyes_[0].extent.height,s.frame.frame.epochs.resources);
            screenValid_=true;displayedReticle_=false;status_="XR_PAIR_READY";
        }
        else if(pick>=0 || removeExpired) {
            const int c=acquireConsumer();
            if(c<0){++consumerBusy_;if(removeExpired)++redrawSkipped_;consumerBusyPick=pick>=0;}
            else {
                if(assigned)releaseTickets(assigned); // Older direct pairs (finished before this newer one) go back first
                const int use=pick>=0?pick:displayedSlot_;
                auto& s=slots_[unsigned(use)];auto& consumer=consumers_[unsigned(c)];
                // An expired marker is erased by redrawing the displayed pair
                // without it (forFrame of an expired reticle is empty).
                const auto marker=pick>=0?s.reticle.forFrame(s.frame.frame,nowNs):StereoReticle{};
                std::array<std::uint32_t,2> indices{};
                chainSets_=eyes_[0].swapchain2&&eyes_[1].swapchain2?2u:1u;
                const unsigned set=chainSets_>1?nextSet_%chainSets_:0u;
                hold.phase(MdpRenderImages);
                const auto imageWaitStart=diagnostics::monotonicNs(); // Always-on eye acquire + xrWaitSwapchainImage cost
                bool ticketUsed=false; // The oldest (free) ticket's images; a prefetch wait that timed out is finished here
                if(ticketCount_&&set==0u){
                    auto& tk=tickets_[0];
                    for(unsigned i=0;i<2;++i)if(!tk.waited[i]){
                        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=XR_INFINITE_DURATION;
                        diagnostics::WallScope imageWait(diagnostics::frameTimings.xrImageWait);
                        const auto result=xrWaitSwapchainImage(eyes_[i].swapchain,&w);
                        if(result!=XR_SUCCESS && result!=XR_SESSION_LOSS_PENDING)throw std::runtime_error("stereo XR ticket wait did not complete");
                        tk.waited[i]=true;}
                    indices=tk.eye;ticketUsed=true;++ticketBlits_;++prefetchUsed_;
                }
                else for(unsigned i=0;i<2;++i) {
                    XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};checkXr(xrAcquireSwapchainImage(eyes_[i].chain(set),&a,&indices[i]),"stereo XR acquire");
                    if(indices[i]>=eyes_[i].imagesOf(set).size())throw std::runtime_error("stereo XR image index");
                    XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=XR_INFINITE_DURATION;
                    diagnostics::WallScope imageWait(diagnostics::frameTimings.xrImageWait);
                    const auto result=xrWaitSwapchainImage(eyes_[i].chain(set),&w);
                    if(result!=XR_SUCCESS && result!=XR_SESSION_LOSS_PENDING)throw std::runtime_error("stereo XR wait did not complete");
                }
                const auto warpWaitStart=diagnostics::monotonicNs();
                const auto toUs=[](std::uint64_t a,std::uint64_t b){return std::uint32_t(std::min<std::uint64_t>((b>a?b-a:0)/1000,0xffffffffull));};
                imageWaitUs_.add(toUs(imageWaitStart,warpWaitStart)); // Eye images only
                const bool ticketWarp=ticketUsed&&tickets_[0].warp.valid;
                const bool ownWarp=pick>=0&&!ticketWarp&&warp_.acquire(unsigned(use)); // Also with a ticket that has no AppSW images
                const bool warped=pick>=0&&(ticketWarp||ownWarp);
                if(pick>=0)warpWaitUs_.add(toUs(warpWaitStart,diagnostics::monotonicNs())); // The four AppSW images
                const unsigned lane=slotCount+unsigned(c);hold.phase(MdpRenderSubmit);
                if(noBlit_>0&&e0NotHdr(eyes_)){ // No GPU work at all; the slot's ready signal stays pending (the producer drains it)
                    ++noBlitFrames_;
                    if(ticketUsed)releaseTickets(1);
                    else for(auto& e:eyes_){XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};checkXr(xrReleaseSwapchainImage(e.chain(set),&r),"stereo XR release");}
                    if(ownWarp)warp_.release();
                    if(chainSets_>1){nextSet_=(set+1)%chainSets_;++setSwaps_;}
                    if(pick>=0){
                        if(displayedSequence_ && s.sequence>displayedSequence_+1)consumerSkipped_+=s.sequence-displayedSequence_-1;
                        displayedSlot_=use;displayedSequence_=s.sequence;displayedEpochs_=s.frame.frame.epochs;displayed_.frame=s.frame.frame;
                        displayed_.warp=warped?warp_.layer(unsigned(use)):SpaceWarpLayer{};
                        for(unsigned i=0;i<2;++i){displayed_.swapchains[i]=eyes_[i].chain(set);displayed_.poses[i]=s.frame.eyes[i].renderedPose;displayed_.fov[i]=s.frame.eyes[i].renderedFov;displayed_.extent[i]=s.frame.eyes[i].extent;}
                        ++copied_;latchedNew=true;displayedPublishedNs_=s.publishedNs;
                    }else{++reticleExpired_;for(unsigned i=0;i<2;++i)displayed_.swapchains[i]=eyes_[i].chain(set);}
                    // the acquired consumer buffer is not submitted: hand it back
                    screenValid_=true;displayedReticle_=false;status_="XR_PAIR_READY";
                }else{
                begin(consumer.command,lane);
                bool reticleDrawn=false;
                for(unsigned i=0;i<2;++i) {
                    auto& e=eyes_[i];auto target=e.imagesOf(set)[indices[i]].image;const auto source=s.images[i].image;
                    if(e.format==VK_FORMAT_R16G16B16A16_SFLOAT){
                        transition(consumer.command,source,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
                        const auto eyeMarker=marker.eyes[i];
                        s.hdr[i].draw(consumer.command,indices[i],eyeMarker);reticleDrawn|=validReticle(eyeMarker);
                        continue;
                    }
                    transition(consumer.command,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
                    VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                    blit.srcOffsets[1]=blit.dstOffsets[1]={static_cast<std::int32_t>(e.extent.width),static_cast<std::int32_t>(e.extent.height),1};
                    vkCmdBlitImage(consumer.command,source,VK_IMAGE_LAYOUT_GENERAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
                    transition(consumer.command,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                }
                bool recordedWarp=warped;
                if(warped){if(ticketWarp)recordedWarp=warp_.recordImages(consumer.command,unsigned(use),tickets_[0].warp);else warp_.record(consumer.command,unsigned(use));}
                gpuTiming_.end(consumer.command,lane);
                checkVk(vkEndCommandBuffer(consumer.command),"stereo XR copy end");
                consumer.link=pick>=0;consumer.slot=use;consumer.seq=s.sequence;
                // [wait the previous read's done (redraw chain)]
                // {wait ready (first read of this pair); read; signal done}
                // done always covers the LAST read of the slot; the next
                // producer copy into it waits done on the GPU.
                const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                std::array<VkSubmitInfo,2> batches{{{VK_STRUCTURE_TYPE_SUBMIT_INFO},{VK_STRUCTURE_TYPE_SUBMIT_INFO}}};std::uint32_t count=0;
                if(s.donePending){auto& d=batches[count++];d.waitSemaphoreCount=1;d.pWaitSemaphores=&s.done;d.pWaitDstStageMask=&stage;}
                auto& x=batches[count++];
                if(s.readyPending){x.waitSemaphoreCount=1;x.pWaitSemaphores=&s.ready;x.pWaitDstStageMask=&stage;}
                x.commandBufferCount=1;x.pCommandBuffers=&consumer.command;x.signalSemaphoreCount=1;x.pSignalSemaphores=&s.done;
                checkVk(vkResetFences(device_,1,&consumer.fence),"stereo consumer fence reset");
                gpuTiming_.noteSubmit(lane); // blitStartLag (CPU counter right before the submit)
                const auto submitStart=diagnostics::monotonicNs(); // A cross-queue semaphore wait must not block the XR thread here
                submit(consumerQueue_,count,batches.data(),consumer.fence);
                {const auto t=diagnostics::monotonicNs();consumerSubmitUs_.add(std::uint32_t(std::min<std::uint64_t>((t>submitStart?t-submitStart:0)/1000,0xffffffffull)));}
                consumer.submitted=true;s.readyPending=false;s.donePending=true;
                telemetry.lastBlit.store(true,std::memory_order_relaxed);if(pick>=0)telemetry.lastNewPair.store(true,std::memory_order_relaxed);
                // The XR runtime orders its use after work already submitted to
                // this binding queue; no CPU wait before release.
                if(ticketUsed)releaseTickets(1); // Eyes + its AppSW images
                else for(auto& e:eyes_){XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};checkXr(xrReleaseSwapchainImage(e.chain(set),&r),"stereo XR release");}
                if(ownWarp)warp_.release();
                if(chainSets_>1){nextSet_=(set+1)%chainSets_;++setSwaps_;} // The next blit goes to the other chain
                if(pick>=0){
                    if(displayedSequence_ && s.sequence>displayedSequence_+1)consumerSkipped_+=s.sequence-displayedSequence_-1;
                    if(displayedSequence_ && s.sequence>displayedSequence_+1 && spaceWarpControl.active())warp_.noteConsumerSkips(s.sequence-displayedSequence_-1);
                    displayedSlot_=use;displayedSequence_=s.sequence;displayedEpochs_=s.frame.frame.epochs;
                    displayed_.frame=s.frame.frame;
                    displayed_.warp=recordedWarp?warp_.layer(unsigned(use)):SpaceWarpLayer{};
                    for(unsigned i=0;i<2;++i){displayed_.swapchains[i]=eyes_[i].chain(set);displayed_.poses[i]=s.frame.eyes[i].renderedPose;displayed_.fov[i]=s.frame.eyes[i].renderedFov;displayed_.extent[i]=s.frame.eyes[i].extent;}
                    ++copied_;latchedNew=true;displayedPublishedNs_=s.publishedNs;
                }else{++reticleExpired_;for(unsigned i=0;i<2;++i)displayed_.swapchains[i]=eyes_[i].chain(set);} // The redraw went to this set
                diagnostics::stereoHistory.push(s.frame.frame.renderFrameId,pick>=0?"DISPLAYED":"RETICLE_EXPIRED",timing.elapsedNs(),eyes_[0].extent.width,eyes_[0].extent.height,s.frame.frame.epochs.resources);
                screenValid_=true;displayedReticle_=reticleDrawn;status_="XR_PAIR_READY";
                if(reticleDrawn)++reticleCopied_;
                }
            }
        }
        if(!screenValid_ || displayedEpochs_.tracking!=tracking || displayedEpochs_.referenceSpace!=space){reason(!screenValid_?NoLayer::NoScreen:NoLayer::Epoch);return false;}
        // Latch classification: new pair, or a repeat of the displayed pair split by why.
        if(measurementPack.load(std::memory_order_relaxed)){
            using L=Latch;const L kind=latchedNew?(newest>=0&&newest!=pick?L::NewNewerInFlight:L::New):consumerBusyPick?L::RepeatConsumerBusy:
                                     newest>=0?L::RepeatNewerNotReady:L::RepeatNothingNewer;
            const auto shown=displayedPublishedNs_,pending=newest>=0&&!latchedNew?slots_[unsigned(newest)].publishedNs:0;
            noteLatch(std::uint8_t(kind),nowNs,displayed_.frame.renderFrameId,shown&&nowNs>shown?nowNs-shown:0,pending&&nowNs>pending?nowNs-pending:0);
        }
        out=displayed_;return true;
    }catch(...){reason(NoLayer::Error);poisoned_=true;screenValid_=false;status_="XR_PAIR_FAILED";throw;}
}
// One classified latch (XR thread, mutex_ held by render()). Counts [0] always, [1] also while AppSW synthesizes.
void StereoMailbox::noteLatch(std::uint8_t kind,std::uint64_t nowNs,std::uint64_t pair,std::uint64_t ageNs,std::uint64_t pendingNs) noexcept {
    using L=Latch;const unsigned synth=spaceWarpControl.synthesizing()?1u:0u;
    const auto us=[](std::uint64_t ns){return std::uint32_t(std::min<std::uint64_t>(ns/1000,0xffffffffull));};
    for(unsigned i=0;i<=synth;++i){
        auto& c=latchCounts_[i];++c.latches;
        switch(L(kind)){
        case L::New:++c.newPairs;latchedAge_[i].add(us(ageNs));break;
        case L::NewNewerInFlight:++c.newPairs;++c.newNewerInFlight;latchedAge_[i].add(us(ageNs));break;
        case L::RepeatNewerNotReady:++c.repeats;++c.repeatsNewerNotReady;pendingAge_[i].add(us(pendingNs));break;
        case L::RepeatNothingNewer:++c.repeats;++c.repeatsNothingNewer;break;
        case L::RepeatConsumerBusy:++c.repeats;++c.repeatsConsumerBusy;break;
        default:break;
        }
    }
    latchRing_[latchNext_]={nowNs,pair,us(ageNs),us(pendingNs),kind,std::uint8_t(synth)};latchNext_=(latchNext_+1)%latchRows;if(latchN_<latchRows)++latchN_;++latchTotal_;
    lastLatch_.store(kind,std::memory_order_relaxed);lastLatchAgeUs_.store(us(ageNs),std::memory_order_relaxed);
}
void StereoMailbox::waitAll() {
    for(unsigned i=0;i<slotCount;++i){auto& s=slots_[i];if(!s.submitted)continue;waitTeardown(device_,s.fence);s.submitted=false;gpuTiming_.completed(i,false);warp_.completed(i);}
    for(unsigned i=0;i<consumerCount;++i){auto& c=consumers_[i];if(!c.submitted)continue;waitTeardown(device_,c.fence);c.submitted=false;gpuTiming_.completed(slotCount+i,true);}
}
bool StereoMailbox::e0NotHdr(const std::array<Eye,2>& eyes) noexcept {return eyes[0].format!=VK_FORMAT_R16G16B16A16_SFLOAT&&eyes[1].format!=VK_FORMAT_R16G16B16A16_SFLOAT;}
void StereoMailbox::releaseTickets(unsigned n) {
    n=std::min(n,ticketCount_);
    for(unsigned k=0;k<n;++k){auto& tk=tickets_[k];
        for(unsigned i=0;i<2;++i){XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};checkXr(xrReleaseSwapchainImage(eyes_[i].swapchain,&r),"stereo XR ticket release");}
        warp_.releaseImages(tk.warp);}
    for(unsigned k=n;k<ticketCount_;++k)tickets_[k-n]=tickets_[k];
    for(unsigned k=ticketCount_-n;k<ticketCount_;++k)tickets_[k]={};
    ticketCount_-=n;
}
void StereoMailbox::prefetchImages() {
    std::array<XrSwapchain,2> chains{};std::array<std::size_t,2> counts{};SpaceWarp::Chains wc{};
    bool wantWarp=false,resume=false;std::array<bool,2> waited{};std::array<std::uint32_t,2> idx{};std::int64_t budget=0;
    {std::lock_guard guard(mutex_);MdHold hold(*this,MdpPrefetch);
     if(prefetchMode_<=0||poisoned_||screensDirty_||!screenValid_)return;
     for(unsigned i=0;i<2;++i){const auto& e=eyes_[i];if(!e.swapchain||e.swapchain2||e.format==VK_FORMAT_R16G16B16A16_SFLOAT)return;chains[i]=e.swapchain;counts[i]=e.images.size();}
     if(ticketCount_&&!ticketReady(tickets_[ticketCount_-1])){ // the last prefetch timed out: finish its waits (waits go in acquire order)
         const auto& tk=tickets_[ticketCount_-1];resume=true;waited=tk.waited;idx=tk.eye;wantWarp=tk.wantWarp&&warp_.ticketChains(wc);
     }else{
         for(unsigned k=0;k<ticketCount_;++k)if(!tickets_[k].seq)return; // one free ticket ahead is enough
         if(ticketCount_>=maxTickets)return;
         wantWarp=warp_.ticketChains(wc);
     }
     // The wait must not push xrWaitFrame past the next wake: budget = next expected wake - 2 ms (1..20 ms)
     const auto now=diagnostics::monotonicNs();const auto wake=framePacer.lastXrWakeNs();const auto interval=framePacer.xrIntervalNs();
     budget=std::clamp<std::int64_t>(wake&&interval>0?std::int64_t(wake)+interval-2'000'000-std::int64_t(now):5'000'000,1'000'000,20'000'000);}
    // Outside mutex_: waits must not block the render thread's capture(). The handles cannot change meanwhile (screens() /
    // discardScreens() run on this XR thread; capture() only assigns READY tickets, never one being waited here).
    const auto start=diagnostics::monotonicNs();bool complete=true,warpOk=false;SpaceWarp::Images images{};
    try{
        if(!resume)for(unsigned i=0;i<2;++i){
            XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};checkXr(xrAcquireSwapchainImage(chains[i],&a,&idx[i]),"stereo XR prefetch acquire");
            if(idx[i]>=counts[i])throw std::runtime_error("stereo XR prefetch image index");}
        for(unsigned i=0;i<2&&complete;++i){
            if(waited[i])continue;
            const std::int64_t left=budget-std::int64_t(diagnostics::monotonicNs()-start);
            XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=XrDuration(std::max<std::int64_t>(left,0));
            const auto result=xrWaitSwapchainImage(chains[i],&w);
            if(result==XR_SUCCESS||result==XR_SESSION_LOSS_PENDING)waited[i]=true;
            else if(result==XR_TIMEOUT_EXPIRED)complete=false;
            else throw std::runtime_error("stereo XR prefetch wait failed");
        }
        if(complete&&wantWarp)warpOk=SpaceWarp::acquireImages(wc,images); // the AppSW images never held the XR thread (warpWaitMs ~0)
    }catch(...){std::lock_guard guard(mutex_);MdHold hold(*this,MdpPrefetch);poisoned_=true;screenValid_=false;status_="XR_PREFETCH_FAILED";throw;}
    const auto us=std::uint32_t(std::min<std::uint64_t>((diagnostics::monotonicNs()-start)/1000,0xffffffffull));
    std::lock_guard guard(mutex_);MdHold hold(*this,MdpPrefetch);
    auto& tk=resume?tickets_[ticketCount_-1]:tickets_[ticketCount_++];
    if(!resume){tk={};tk.eye=idx;}
    tk.waited=waited;tk.wantWarp=wantWarp;
    if(complete&&wantWarp){if(warpOk)tk.warp=images;else warp_.noteAcquireFailure();}
    if(complete)++prefetches_;else ++ticketTimeouts_;
    prefetchWaitUs_.add(us);
}
void StereoMailbox::discardScreens() {
    // Consumer submissions write the XR images through these HDR framebuffers.
    for(unsigned i=0;i<consumerCount;++i){auto& c=consumers_[i];if(!c.submitted)continue;waitTeardown(device_,c.fence);c.submitted=false;gpuTiming_.completed(slotCount+i,true);}
    for(auto& slot:slots_)for(auto& hdr:slot.hdr)hdr.destroy();
    for(auto& e:eyes_){if(e.swapchain)xrDestroySwapchain(e.swapchain);e.swapchain={};e.images.clear();e.xrFormat=VK_FORMAT_UNDEFINED;
        if(e.swapchain2)xrDestroySwapchain(e.swapchain2);e.swapchain2={};e.images2.clear();}
    chainSets_=1;nextSet_=0;tickets_={};ticketCount_=0; // Destroying a swapchain releases its acquired images
    warp_.discardScreens();displayed_.warp={}; // Consumer fences waited above
    screenValid_=false;displayedReticle_=false;
}
void StereoMailbox::discardGpu() {
    // A submit-gate waiter holds a copy of a slot fence outside mutex_; it decrements before re-locking, so this cannot deadlock.
    while(externalWaits_.load(std::memory_order_acquire))std::this_thread::yield();
    // A failed wait does not cancel submitted GPU work: throw before destroying.
    waitAll();
    aa_.destroyTargets(); // Views/framebuffers before the slot images; the format-pair objects stay
    // Framebuffers/descriptors/views must go before either referenced image.
    // Signalled-but-unwaited binary semaphores may be destroyed once every
    // batch referencing them has completed (all fences above).
    for(auto& slot:slots_){
        for(auto& hdr:slot.hdr)hdr.destroy();
        // A reserved slot may still be written by an engine pass (no mailbox fence covers it): keep its images until releaseDirect
        if(slot.mdToken){mdOrphans_.push_back({slot.mdToken,slot.images});slot.images={};slot.mdToken=0;++mdOrphaned_;}
        for(auto& image:slot.images)destroyOwnedImage(device_,image);
        if(slot.fence)vkDestroyFence(device_,slot.fence,nullptr);
        if(slot.ready)vkDestroySemaphore(device_,slot.ready,nullptr);
        if(slot.done)vkDestroySemaphore(device_,slot.done,nullptr);
        slot.command={};slot.fence={};slot.ready=slot.done={};slot.frame={};slot.reticle={};slot.sequence=slot.publishedNs=0;
        slot.submitted=slot.readyPending=slot.donePending=slot.initialized=false;
    }
    warp_.discardGpu();displayed_.warp={}; // Every fence waited above
    for(auto& c:consumers_){if(c.fence)vkDestroyFence(device_,c.fence,nullptr);c={};}
    gpuTiming_.destroy();
    if(pool_)vkDestroyCommandPool(device_,pool_,nullptr);pool_={};
    published_=false;displayedSlot_=-1;displayedReticle_=false;++generation_;displayedPublishedNs_=0;
    deadline_.forgetAll(); // Pending misses whose completion was waited above without observeSlot
}
bool StereoMailbox::freshCompatible(std::uint64_t tracking,std::uint64_t space,std::uint64_t now) const noexcept {
    std::lock_guard guard(mutex_);return accepting_&&!poisoned_&&published_&&publishedNs_&&latest_.frame.mode==RenderMode::Stereo&&
        latest_.frame.epochs.tracking==tracking&&latest_.frame.epochs.referenceSpace==space&&now>=publishedNs_&&now-publishedNs_<=maxAgeNs;
}
void StereoMailbox::invalidate(){std::lock_guard guard(mutex_);publishedNs_=0;screenValid_=false;minSequence_=sequence_+1;}
void StereoMailbox::detachSession(){std::lock_guard guard(mutex_);accepting_=false;discardGpu();discardScreens();publishedNs_=0;}
void StereoMailbox::shutdown(){std::lock_guard guard(mutex_);accepting_=false;discardGpu();discardScreens();
    antiAliasingWaitBuild();aa_.destroy();antiAliasingEffective.store(-1,std::memory_order_relaxed);antiAliasingDebug.store(0,std::memory_order_relaxed); // Format-pair objects (all fences waited)
    // Device teardown (vkDestroyDevice requires the game's submissions complete): orphaned direct slots go now; later releases no-op
    for(auto& o:mdOrphans_)for(auto& image:o.images)destroyOwnedImage(device_,image);
    mdOrphansFreed_+=mdOrphans_.size();mdOrphans_.clear();
    producerQueues_.clear();device_={};physical_={};consumerQueue_={};}
std::string StereoMailbox::spaceWarpReport() const {std::lock_guard guard(mutex_);return warp_.report();}
std::string StereoMailbox::report() const {
    std::unique_lock guard(mutex_);MdHold hold(*this,MdpReport);std::ostringstream s;
    s<<"{\"gpuTimings\":"<<gpuTiming_.report()<<",\"status\":\""<<status_<<"\",\"capturedPairs\":"<<captured_<<",\"copiedPairs\":"<<copied_<<",\"rejectedSources\":"<<rejected_
     <<",\"rejectedEpochOrAge\":"<<epochRejected_
     <<",\"staleResubmits\":"<<presentTelemetry.staleResubmits.load(std::memory_order_relaxed)<<",\"ageRejected\":"<<presentTelemetry.ageRejects.load(std::memory_order_relaxed)
     <<",\"epochRejected\":"<<presentTelemetry.epochRejects.load(std::memory_order_relaxed)<<",\"staleResubmitEnabled\":"<<(presentTelemetry.staleEnabled.load(std::memory_order_relaxed)?"true":"false")
     <<",\"lastRenderFrame\":"<<latest_.frame.renderFrameId<<",\"lastDisplayedFrame\":"<<displayed_.frame.renderFrameId
     <<",\"ring\":{\"slots\":"<<slotCount<<",\"consumers\":"<<consumerCount<<",\"generation\":"<<generation_<<",\"sequence\":"<<sequence_<<",\"displayedSequence\":"<<displayedSequence_
     <<",\"inFlightCaptures\":"<<inFlightCaptures_<<",\"pairsRejectedNotReady\":"<<notReadyRejected_<<",\"slotWaits\":"<<slotWaits_<<",\"slotTimeouts\":"<<slotTimeouts_
     <<",\"framesInFlight\":"<<framesInFlight()<<",\"framesInFlightMax\":"<<framesInFlightMax_<<",\"readyDrains\":"<<readyDrains_<<",\"doneWaits\":"<<doneWaits_
     <<",\"consumerWaits\":0,\"consumerNotReady\":"<<consumerNotReady_<<",\"consumerBusy\":"<<consumerBusy_<<",\"consumerSkippedPairs\":"<<consumerSkipped_<<",\"redrawSkipped\":"<<redrawSkipped_
     <<",\"fifo\":{\"mode\":"<<fifoMode_<<",\"picks\":"<<fifoPicks_<<",\"older\":"<<fifoOlder_<<",\"skipAhead\":"<<fifoSkipAhead_<<"}}"
     <<",\"reticleTransferPairs\":"<<reticleCopied_
     <<",\"reticleExpiryTransfers\":"<<reticleExpired_<<",\"reticleOpacity\":\"native-linear-fade\""
     <<",\"hdrDisplayCurve\":\"reinhard-unit-exposure\"";
    // Anti-aliasing of the published pair (passGpu/copyGpu: producer command buffer, both eyes, DETAIL only).
    // requested/mode by row name (FXAA EDGES / FXAA SPLIT = proof modes); debugMode = fxaa.frag debugMode of the last pair.
    const unsigned requested=aaModeProperty(aaProperty_)?unsigned(aaProperty_):aaMenu_;
    s<<",\"antiAliasing\":{\"requested\":\""<<GraphicsSettings::antiAliasingName(requested)<<"\",\"menu\":"<<aaMenu_<<",\"property\":"<<aaProperty_
     <<",\"mode\":\""<<(aaApplied_?(aaParams_.debug==StereoAntiAliasing::debugEdges?"FXAA EDGES":aaParams_.debug==StereoAntiAliasing::debugSplit?"FXAA SPLIT":"FXAA"):"OFF")<<"\",\"debugMode\":"<<aaParams_.debug
     <<",\"debugName\":\""<<StereoAntiAliasing::debugName(aaParams_.debug)<<"\",\"algorithm\":\"fxaa3.11-quality-p12-green-gather\",\"subpix\":"<<aaParams_.subpix
     <<",\"edgeThreshold\":"<<aaParams_.edgeThreshold<<",\"edgeThresholdMin\":"<<aaParams_.edgeThresholdMin
     <<",\"ready\":"<<(aa_.ready()?"true":"false")<<",\"lumaMode\":"<<aa_.lumaMode()<<",\"outputDecode\":"<<aa_.outputDecode()
     <<",\"effective\":"<<antiAliasingEffective.load(std::memory_order_relaxed)<<",\"brokenPair\":"<<(aaBrokenSource_?"true":"false")
     <<",\"buildPending\":"<<(aaBuild_.valid()?"true":"false")<<",\"builds\":"<<aaCreates_<<",\"buildFailures\":"<<aaBuildFailures_
     <<",\"lastBuildNs\":"<<aaCreateNs_<<",\"buildThread\":\"background\",\"targetCreates\":"<<aaTargetCreates_<<",\"sourceReleases\":"<<aaSourceReleases_
     <<",\"framesProcessed\":"<<aaFrames_<<",\"hdrFrames\":"<<aaHdrFrames_<<",\"copyFrames\":"<<aaCopyFrames_<<",\"fallbacks\":"<<aaFallbacks_<<",\"fallbackReasons\":{";
    for(unsigned i=0;i<aaFallbackReasons_.size();++i)s<<(i?",":"")<<'"'<<StereoAntiAliasing::fallbackName(StereoAntiAliasing::Fallback(i))<<"\":"<<aaFallbackReasons_[i];
    s<<"},\"passGpu\":"<<aaPassGpu_.snapshot().json()<<",\"copyGpu\":"<<copyPassGpu_.snapshot().json()<<'}';
    s<<",\"poisoned\":"<<(poisoned_?"true":"false");
    // MAILBOX DIRECT: requests (render-thread reservations tried, mode on), reserved, captures = pairs whose ring copy was skipped
    // (compare capturedPairs; gpuTimings capture lane / antiAliasing.copyGpu shrink to the warp pass), refused by reason, busy (lock held
    // by the XR thread; mode 2: only retries that ran out), stale (rejected direct captures), aaSkips, released (unconsumed, returned), orphaned/orphansFreed/orphans (rebuilds).
    {unsigned held=0;for(const auto& slot:slots_)held+=slot.mdToken?1u:0u;
     s<<",\"mailboxDirect\":{\"requests\":"<<mdRequests_<<",\"reserved\":"<<mdReserved_<<",\"captures\":"<<mdCaptures_<<",\"held\":"<<held
      <<",\"busy\":"<<mdBusy_.load(std::memory_order_relaxed)<<",\"refused\":{\"state\":"<<mdRefused_[MdState]<<",\"config\":"<<mdRefused_[MdConfig]
      <<",\"aa\":"<<mdRefused_[MdAa]<<",\"noSlot\":"<<mdRefused_[MdNoSlot]<<"},\"stale\":"<<mdStale_<<",\"aaSkips\":"<<mdAaSkips_<<",\"released\":"<<mdReleased_
      <<",\"orphaned\":"<<mdOrphaned_<<",\"orphansFreed\":"<<mdOrphansFreed_<<",\"orphans\":"<<mdOrphans_.size();} // '}' after the busy block below
    // busy attribution, read after busy above: sum(byPhase.busy) >= busy + busyWon (equal while no reservation is in flight)
    const auto busyRetried=mdBusyRetried_.load(std::memory_order_relaxed),busyWon=mdBusyWon_.load(std::memory_order_relaxed),busyMissed=mdBusyMissed_.load(std::memory_order_relaxed);
    std::array<std::uint64_t,MdpCount> busyPhase{};for(unsigned p=0;p<MdpCount;++p)busyPhase[p]=mdBusyPhase_[p].load(std::memory_order_relaxed);
    const auto holdStats=mdHoldStats_;
    // Latch classification: copied under the lock, formatted after it (a snapshot must not stall the XR thread's render()).
    const auto counts=latchCounts_;const auto latched=latchedAge_;const auto pending=pendingAge_;
    const std::vector<LatchRow> ring(latchRing_.begin(),latchRing_.end());const unsigned rows=latchN_,next=latchNext_;const auto total=latchTotal_;
    const auto late=late_;const auto dl=deadline_;const auto readyExact=readyExact_,readyInexact=readyInexact_;
    const auto imageWait=imageWaitUs_,consumerSubmit=consumerSubmitUs_,producerLock=producerLockUs_,warpWait=warpWaitUs_;
    const unsigned chainSets=chainSets_;const auto setSwaps=setSwaps_;
    const int noBlitMode=noBlit_;const auto noBlitFrames=noBlitFrames_;
    const int latchDepth=latchDepth_;const auto depthPicks=depthPicks_,depthNone=depthNone_;
    const int prefetchMode=prefetchMode_;const auto prefetches=prefetches_,prefetchUsed=prefetchUsed_;const auto prefetchWait=prefetchWaitUs_;
    const int directMode=directMode_;const unsigned ticketsNow=ticketCount_;const auto directCaptures=directCaptures_,directNoTicket=directNoTicket_,directLatches=directLatches_,
        directWarpLatches=directWarpLatches_,directBlocked=directBlocked_,directRescued=directRescued_,ticketTimeouts=ticketTimeouts_,ticketBlits=ticketBlits_;
    const auto gateChecks=gateChecks_,gateWaits=gateWaits_,gateTimeouts=gateTimeouts_;const auto gateWait=gateWaitUs_;
    const std::size_t eyeImages=eyes_[0].images.size();
    const int inflightMode=inflightMode_;const std::int64_t inflightBias=inflightBiasNs_;const auto inflightPicks=inflightPicks_,inflightUnfinished=inflightUnfinished_,inflightNothing=inflightNothing_;
    hold.end();guard.unlock(); // The report hold ends under the lock
    const auto window=[&s](const char* key,const AgeWindow& w){
        s<<",\""<<key<<"\":{\"samples\":"<<w.n<<",\"total\":"<<w.total<<",\"sumMs\":"<<std::to_string(w.sumMs);
        if(w.n){auto v=w.v;std::sort(v.begin(),v.begin()+w.n);double sum=0;for(unsigned i=0;i<w.n;++i)sum+=v[i];
            s<<",\"last\":"<<w.last/1e3<<",\"p50\":"<<v[(w.n*50+99)/100-1]/1e3<<",\"p95\":"<<v[(w.n*95+99)/100-1]/1e3<<",\"mean\":"<<sum/1e3/w.n<<",\"max\":"<<w.max/1e3;}
        s<<'}';};
    // (still inside mailboxDirect): mode 2 retries (busyRetried = try_lock failures retried, busyWon = got the lock within retryUs; busy
    // above = the rest), busyWaitUs (every retry; 10 us buckets, p50/p95 = bucket upper edge capped at max). busyHold: byPhase.busy = every
    // try_lock failure by the holder's phase at that moment (other = an uninstrumented holder or already released), measured = holds that
    // timed themselves (holdMs = lock -> unlock, remainMs = busy -> unlock = the wait a retry needs), missed = released before the mark landed;
    // holdBuckets / remainBuckets count those by edgesUs (< edge, last = >= 5000 us).
    {std::array<std::uint32_t,WaitHistogram::buckets> wb{};std::uint64_t wn=0;for(unsigned i=0;i<wb.size();++i){wb[i]=mdBusyWaitUs_.n[i].load(std::memory_order_relaxed);wn+=wb[i];}
     const std::uint64_t waitMax=mdBusyWaitUs_.max.load(std::memory_order_relaxed);
     const auto pct=[&](unsigned q){std::uint64_t c=0;for(unsigned i=0;i<wb.size();++i){c+=wb[i];if(c*100>=wn*q)return std::min<std::uint64_t>(std::uint64_t(i+1)*WaitHistogram::stepUs,waitMax);}return waitMax;};
     s<<",\"busyRetried\":"<<busyRetried<<",\"busyWon\":"<<busyWon<<",\"retryUs\":"<<mdRetryNs/1000<<",\"busyWaitUs\":{\"samples\":"<<wn;
     if(wn)s<<",\"p50\":"<<pct(50)<<",\"p95\":"<<pct(95)<<",\"mean\":"<<double(mdBusyWaitUs_.sumUs.load(std::memory_order_relaxed))/double(wn)<<",\"max\":"<<waitMax;
     s<<'}';}
    {static constexpr const char* phases[]={"other","renderLatch","renderLateWait","renderImages","renderSubmit","prefetch","report"};
     static_assert(sizeof(phases)/sizeof(*phases)==MdpCount);
     s<<",\"busyHold\":{\"missed\":"<<busyMissed<<",\"edgesUs\":[";for(unsigned i=0;i<mdHoldEdgesUs.size();++i)s<<(i?",":"")<<mdHoldEdgesUs[i];
     s<<"],\"holdBuckets\":[";for(unsigned i=0;i<mdHoldBuckets;++i)s<<(i?",":"")<<holdStats.holdBuckets[i];
     s<<"],\"remainBuckets\":[";for(unsigned i=0;i<mdHoldBuckets;++i)s<<(i?",":"")<<holdStats.remainBuckets[i];s<<']';
     window("holdMs",holdStats.allHoldUs);window("remainMs",holdStats.remainUs);
     s<<",\"byPhase\":{";
     for(unsigned p=0;p<MdpCount;++p){s<<(p?",":"")<<'"'<<phases[p]<<"\":{\"busy\":"<<busyPhase[p]<<",\"measured\":"<<holdStats.measured[p];window("holdMs",holdStats.holdUs[p]);s<<'}';}
     s<<"}}}";} // byPhase, busyHold, mailboxDirect
    s<<",\"latch\":{\"enabled\":"<<measurementPack.load(std::memory_order_relaxed)<<",\"total\":"<<total
     <<",\"kinds\":[\"none\",\"new\",\"newNewerInFlight\",\"repeatNewerNotReady\",\"repeatNothingNewer\",\"repeatConsumerBusy\"],\"ageClock\":\"cpu-since-capture-submit\"";
    for(unsigned i=0;i<2;++i){const auto& c=counts[i];
        s<<",\""<<(i?"synth":"all")<<"\":{\"latches\":"<<c.latches<<",\"newPairs\":"<<c.newPairs<<",\"newNewerInFlight\":"<<c.newNewerInFlight<<",\"repeats\":"<<c.repeats
         <<",\"repeatsNewerNotReady\":"<<c.repeatsNewerNotReady<<",\"repeatsNothingNewer\":"<<c.repeatsNothingNewer<<",\"repeatsConsumerBusy\":"<<c.repeatsConsumerBusy;
        window("latchedAgeMs",latched[i]);window("pendingAgeMs",pending[i]);s<<'}';}
    // Late latch (policy outcome per NewerNotReady candidate; waits; wait / hit-wait / pdt lead after the wait / fence
    // signal - GPU end clock check, in ms) and the deadline samples published to the pacer (margins = due latch - ready, us).
    // producerBlocked / producerBlockedMs = render-thread capture() / copyComplete() lock waits that overlapped a
    // late-latch wait (always on; frameTimings.stereoLockWait is DETAIL-only and mixes the XR thread's render() lock waits).
    s<<",\"lateLatch\":{\"latchUs\":"<<framePacer.lateLatchNs()/1000<<",\"waits\":"<<late.waits<<",\"hits\":"<<late.hits<<",\"timeouts\":"<<late.timeouts
     <<",\"producerBlocked\":"<<late.producerBlocked<<",\"policy\":{";
    for(unsigned i=0;i<LateLatchPolicy::whyCount;++i)s<<(i?",":"")<<'"'<<LateLatchPolicy::whyName(i)<<"\":"<<late.why[i];
    s<<'}';window("waitMs",late.waitUs);window("hitWaitMs",late.hitWaitUs);window("pdtLeadMs",late.pdtLeadUs);window("clockCheckMs",late.clockUs);
    window("producerBlockedMs",late.producerBlockedUs);
    s<<",\"clockNegative\":"<<late.clockNegative<<'}';
    unsigned pendingNow=0;for(const auto& p:dl.pending)pendingNow+=p.seq?1u:0u;
    s<<",\"deadline\":{\"samples\":"<<dl.samples<<",\"exact\":"<<dl.exact<<",\"misses\":"<<dl.misses<<",\"rescued\":"<<dl.rescued<<",\"resolved\":"<<dl.resolved
     <<",\"dropped\":"<<dl.dropped<<",\"pending\":"<<pendingNow<<",\"nonSynth\":"<<dl.nonSynth<<",\"readyExact\":"<<readyExact<<",\"readyInexact\":"<<readyInexact
     <<",\"delayResets\":"<<dl.delayResets // Ready-delay window dropped after 96 inexact completions in a row
     <<",\"readyDelayUs\":{\"samples\":"<<dl.dn<<",\"p25\":"<<dl.readyDelayNs(25)/1000<<",\"p50\":"<<dl.readyDelayNs(50)/1000<<",\"p90\":"<<dl.readyDelayNs(90)/1000<<'}'
     <<",\"marginUs\":{\"samples\":"<<dl.mn<<",\"p10\":"<<dl.marginPercentileNs(10)/1000<<",\"p50\":"<<dl.marginPercentileNs(50)/1000<<",\"p90\":"<<dl.marginPercentileNs(90)/1000<<"}}";
    // In-flight latch: mode, bias, picks (unfinished = the pair's copy fence had not signalled at the pick), latches with nothing
    // newer, their misses published at the next capture; always-on XR-thread eye image waits and consumer vkQueueSubmit times (ms).
    s<<",\"inflight\":{\"mode\":"<<inflightMode<<",\"biasUs\":"<<inflightBias/1000<<",\"picks\":"<<inflightPicks<<",\"unfinished\":"<<inflightUnfinished
     <<",\"nothingNewer\":"<<inflightNothing<<",\"nothingMisses\":"<<dl.nothingMisses<<",\"nothingDropped\":"<<dl.nothingDropped<<",\"eyeImages\":"<<eyeImages<<",\"chainSets\":"<<chainSets<<",\"setSwaps\":"<<setSwaps;
    window("imageWaitMs",imageWait);window("warpWaitMs",warpWait);window("consumerSubmitMs",consumerSubmit);window("producerLockMs",producerLock);
    s<<",\"noBlit\":{\"mode\":"<<noBlitMode<<",\"frames\":"<<noBlitFrames<<'}';
    s<<",\"latchDepth\":{\"depth\":"<<latchDepth<<",\"picks\":"<<depthPicks<<",\"none\":"<<depthNone<<'}';
    s<<",\"prefetch\":{\"mode\":"<<prefetchMode<<",\"prefetches\":"<<prefetches<<",\"used\":"<<prefetchUsed<<",\"timeouts\":"<<ticketTimeouts;window("waitMs",prefetchWait);s<<'}';
    s<<",\"direct\":{\"mode\":"<<directMode<<",\"tickets\":"<<ticketsNow<<",\"captures\":"<<directCaptures<<",\"noTicket\":"<<directNoTicket<<",\"latches\":"<<directLatches
     <<",\"warpLatches\":"<<directWarpLatches<<",\"blocked\":"<<directBlocked<<",\"rescued\":"<<directRescued<<",\"ticketBlits\":"<<ticketBlits<<'}';
    s<<",\"submitGate\":{\"checks\":"<<gateChecks<<",\"waits\":"<<gateWaits<<",\"timeouts\":"<<gateTimeouts;window("waitMs",gateWait);s<<'}';
    s<<'}';
    const unsigned first=(next+latchRows-rows)%latchRows;const std::uint64_t t0=rows?ring[first].ns:0;
    s<<",\"history\":{\"capacity\":"<<latchRows<<",\"t0Ns\":"<<t0<<",\"cols\":[\"tUs\",\"pair\",\"kind\",\"ageUs\",\"pendingUs\",\"synth\"],\"rows\":[";
    for(unsigned i=0;i<rows;++i){const auto& r=ring[(first+i)%latchRows];
        s<<(i?",":"")<<'['<<(r.ns-t0)/1000<<','<<r.pair<<','<<unsigned(r.kind)<<','<<r.ageUs<<','<<r.pendingUs<<','<<unsigned(r.synth)<<']';}
    s<<"]}}}";return s.str();
}
} // namespace rdrvr::runtime
