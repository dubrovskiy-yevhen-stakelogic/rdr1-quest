#include "GameStereoCapture.h"
#include "GameQueues.h"
#include "StereoTestMode.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
namespace rdrvr::runtime {
namespace {
bool supported(VkFormat f) {return f==VK_FORMAT_R8G8B8A8_UNORM || f==VK_FORMAT_R8G8B8A8_SRGB || f==VK_FORMAT_B8G8R8A8_UNORM || f==VK_FORMAT_B8G8R8A8_SRGB || f==VK_FORMAT_R16G16B16A16_SFLOAT;}
void transition(VkCommandBuffer cmd,VkImage image,VkImageLayout before,VkImageLayout after,VkAccessFlags src,VkAccessFlags dst,std::uint32_t layer=0) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=before;b.newLayout=after;b.srcAccessMask=src;b.dstAccessMask=dst;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,layer,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
// Same clock as the present/early callers (steady_clock == CLOCK_MONOTONIC).
std::uint64_t steadyNs() noexcept {
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return ns>0?static_cast<std::uint64_t>(ns):0;
}
// StereoMailbox mdRetryNs), re-read at most once per second by the thread recording the stereo
// pass (recordEye eye 0). Constant-initialised atomics: no TLS, no static-init guard.
std::atomic<std::uint64_t> mailboxDirectNextNs{0};std::atomic<int> mailboxDirectValue{0};
int mailboxDirectMode() noexcept {
    const auto now=steadyNs();
    if(now>=mailboxDirectNextNs.load(std::memory_order_relaxed)){
        mailboxDirectNextNs.store(now+1'000'000'000ull,std::memory_order_relaxed);
        int mode=1; // default 1 (about 1.0 ms less render-thread busy time, fewer repeated frames); 0 = the copy path
#ifdef __ANDROID__
        char v[PROP_VALUE_MAX]{};
        if(__system_property_get("debug.rdrvr.mailbox.direct",v)>0&&v[0]>='0'&&v[0]<='9'&&!v[1])mode=v[0]-'0';
#endif
        mailboxDirectValue.store(mode,std::memory_order_relaxed);
    }
    return mailboxDirectValue.load(std::memory_order_relaxed);
}
bool liveSwapEyes() noexcept {return (::rdrvr::engine::stereo::testModeMask(::rdrvr::engine::stereo::testMode.load())&(1u<<30))!=0;} // SWAP EYES test
}
void StereoCaptureSlot::bind(const XrGraphicsBindingVulkan2KHR& b,bool earlyCompletion,bool externalEpoch) {
    std::lock_guard guard(mutex_);
    // The published game queues (Appended: the same handles as the loop below; split: the protected-capable
    // queues, binding index 0). An unpublished list falls back to the vkGetDeviceQueue loop.
    std::array<VkQueue,GameQueues::capacity> listed{};
    const unsigned published=gameQueueList(listed.data(),static_cast<unsigned>(listed.size()));
    if(device_ || !b.device || !b.physicalDevice || (!b.queueIndex && !published))throw std::runtime_error("pass capture requires reserved queue");
    device_=b.device;physical_=b.physicalDevice;family_=b.queueFamilyIndex;
    if(published)queues_.assign(listed.begin(),listed.begin()+published);
    else for(std::uint32_t i=0;i<b.queueIndex;++i){VkQueue q{};vkGetDeviceQueue(device_,family_,i,&q);queues_.push_back(q);}
    accepting_=true;earlyCompletion_=earlyCompletion;externalEpoch_=externalEpoch;
}
bool StereoCaptureSlot::beginFrame(VkQueue q,const CaptureFrame& f,VkCommandBuffer passCommand) {
    std::lock_guard guard(mutex_);auto reject=[&]{++rejected_;return false;};
    if(!accepting_ || poisoned_ || active_ || !device_ || f.frame.mode!=RenderMode::Stereo ||
       f.frame.renderFrameId<=lastFrame_ || !f.frame.xrFrameId || f.frame.predictedDisplayTime<=0 ||
       !f.frame.epochs.tracking || !f.frame.epochs.referenceSpace || !f.frame.epochs.resources ||
       std::find(queues_.begin(),queues_.end(),q)==queues_.end())return reject();
    bool changed=!images_[0].image || !images_[1].image || f.encoding!=latest_.encoding;
    for(unsigned i=0;i<2;++i) {
        if(!validPose(f.poses[i]) || !usableFov(f.fov[i]) || !supported(f.formats[i]) ||
           !f.extents[i].width || !f.extents[i].height || f.extents[i].width>8192 || f.extents[i].height>8192 ||
           (f.encoding==SourceEncoding::SrgbEncoded && f.formats[i]==VK_FORMAT_R16G16B16A16_SFLOAT))return reject();
        changed|=f.formats[i]!=latest_.formats[i] || f.extents[i]!=latest_.extents[i];
    }
    try {
        if(changed) {
            // A same-queue publication retires immediately while its
            // mailbox copy may still be queued; it must finish reading the old
            // images before they are destroyed. (Reuse without destruction is
            // ordered by the pass queue or by the in-flight retirement rule.)
            if(lastTicketValid_ && mailbox_ && !mailbox_->waitCopy(lastTicket_,reuseWaitNs)){++reuseWaits_;return reject();}
            // No mailbox anti-aliasing source view may outlive the images replaced below.
            if(mailbox_ && images_[0].image && !mailbox_->releaseSources({images_[0].image,images_[1].image},reuseWaitNs)){++reuseWaits_;return reject();}
            lastTicketValid_=false;
            // Allocate both replacements before retiring a working generation.
            std::array<OwnedImage,2> next{};
            // Also SAMPLED (bilinear) when the format allows it, so the mailbox can
            // anti-alias from these images; otherwise it keeps copying them.
            bool sampled=true;
            for(unsigned i=0;i<2;++i){VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(physical_,f.formats[i],&p);
                const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
                sampled=sampled&&(p.optimalTilingFeatures&need)==need;}
            const VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|(sampled?VK_IMAGE_USAGE_SAMPLED_BIT:0);
            try {for(unsigned i=0;i<2;++i)next[i]=createOwnedImage(physical_,device_,f.extents[i],f.formats[i],usage,VK_IMAGE_ASPECT_COLOR_BIT,false);}
            catch(...){for(auto& image:next)destroyOwnedImage(device_,image);throw;}
            for(auto& image:images_)destroyOwnedImage(device_,image);
            images_=next;sampled_=sampled;initialized_={};++resourceEpoch_;
        }
        if(md_.valid)releaseDirect(); // Defensive (retire() returns every reservation; !active_ = the old pass is GPU-proven done)
        mdEyes_=0;
        latest_=f;latest_.frame.epochs.resources=externalEpoch_?f.frame.epochs.resources:resourceEpoch_;queue_=q;owner_=std::this_thread::get_id();
        recorded_=submitted_=0;commands_={};completion_={};cancelled_=false;passCommand_=passCommand;passSubmitted_=passDiscarded_=false;
        inFlight_=markerCoversCopy_=publishRejected_=false;ticket_={};
        warpRecorded_=0;warpFrameSet_=false;warpDirect_=false;warpDual_=false;warpDualRecorded_=0;
        warpTimed_=0;warpTicket_=diagnostics::profilerTicket(); // warpDepth lane of this frame (DETAIL only)
        active_=true;lastFrame_=f.frame.renderFrameId;++prepared_;return true;
    }catch(...){++rejected_;throw;}
}
void StereoCaptureSlot::cancelFrame(std::uint64_t frame) noexcept {
    std::lock_guard guard(mutex_);
    if(active_ && frame==latest_.frame.renderFrameId && owner_==std::this_thread::get_id())cancelled_=true;
}
std::uint64_t StereoCaptureSlot::retiredFrame() const noexcept {std::lock_guard guard(mutex_);return retiredFrame_;}
StereoCaptureSlot::PresentState StereoCaptureSlot::presentState(VkQueue q) const noexcept {
    std::lock_guard guard(mutex_);
    const bool healthy=accepting_&&!poisoned_&&(!active_||!cancelled_||inFlight_);
    auto reason=ArmReason::Inactive;
    if(active_){
        if(!healthy)reason=ArmReason::Unhealthy;
        // Published in flight: retirement needs neither this present nor its queue.
        else if(inFlight_)reason=ArmReason::Ready;
        else if(queue_!=q)reason=ArmReason::PresentQueue;
        else if(!passSubmitted_)reason=ArmReason::Recording;
        else if(recorded_!=3||submitted_!=3)reason=ArmReason::Partial;
        else if(!passCommand_||passDiscarded_||commands_[0]!=passCommand_||commands_[1]!=passCommand_)reason=ArmReason::Command;
        else if(!earlyCompletion_||!earlyFence_||!earlyQueue_||earlyFrame_!=latest_.frame.renderFrameId)reason=ArmReason::Marker;
        else if(completion_[0].queue!=earlyQueue_||completion_[1].queue!=earlyQueue_||completion_[2].queue!=earlyQueue_)reason=ArmReason::SubmitQueue;
        else if(owner_!=earlyThread_)reason=ArmReason::Thread;
        else reason=ArmReason::Ready;
    }
    // This qualifies a future same-DRAW-queue publication, not a cross-queue
    // read based on a host fence. The present call keeps its native semaphores;
    // only the next submit hook on earlyQueue_ may perform the mailbox copy.
    return {latest_.frame,healthy,active_,reason==ArmReason::Ready,queue_,earlyQueue_,reason};
}
bool StereoCaptureSlot::recordEye(VkCommandBuffer cmd,unsigned eye,const PassImage& s,StereoMailbox* mailbox) {
    std::lock_guard guard(mutex_);auto reject=[&]{++rejected_;return false;};
    if(!active_ || poisoned_ || cancelled_ || inFlight_ || owner_!=std::this_thread::get_id() || !cmd || eye>1 ||
       (passCommand_ && cmd!=passCommand_) || recorded_!=(eye==0?0u:1u) || !s.image || s.image==images_[0].image || s.image==images_[1].image ||
       s.extent!=latest_.extents[eye] || s.format!=latest_.formats[eye] ||
       s.family!=family_ || s.samples!=VK_SAMPLE_COUNT_1_BIT || !(s.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
       !s.arrayLayers || s.arrayLayer>=s.arrayLayers ||
       (s.layout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && s.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        s.layout!=VK_IMAGE_LAYOUT_GENERAL && s.layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))return reject();
    // MAILBOX DIRECT: eye 0 decides the pair (eye 1 follows it): a reserved mailbox ring slot, else images_ (the copy path, counted).
    if(eye==0&&!md_.valid){
        mdEyes_=0;mdMode_=mailboxDirectMode();
        if(mdMode_>0){
            if(!mailbox)++mdNoMailbox_;
            else{
                const bool swap=liveSwapEyes();std::array<Extent,2> extents{};std::array<VkFormat,2> formats{};
                for(unsigned i=0;i<2;++i){const unsigned src=swap?1u-i:i;extents[i]=latest_.extents[src];formats[i]=latest_.formats[src];} // published eye i
                if(mailbox->reserveDirect(queue_,extents,formats,latest_.encoding,latest_.frame.epochs.resources,md_,mdMode_==2)){mdMailbox_=mailbox;mdSwap_=swap;++mdPairs_;}
                else ++mdRefused_;
            }
        }
    }
    const bool direct=md_.valid;const unsigned slotEye=mdSwap_?1u-eye:eye; // The slot holds the PUBLISHED eye order
    const VkImage target=direct?md_.images[slotEye]:images_[eye].image;
    const bool targetInitialized=direct?md_.initialized[slotEye]:initialized_[eye];
    transition(cmd,s.image,s.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,s.arrayLayer);
    transition(cmd,target,targetInitialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy c{};c.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,s.arrayLayer,1};c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={s.extent.width,s.extent.height,1};
    vkCmdCopyImage(cmd,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_GENERAL,1,&c);
    transition(cmd,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,s.arrayLayer);
    commands_[eye]=cmd;recorded_|=1u<<eye;if(direct)mdEyes_|=1u<<eye;return true;
}
bool StereoCaptureSlot::swapEyes() const noexcept {return md_.valid?mdSwap_:liveSwapEyes();} // A direct pair keeps its slot's eye order
// Caller holds mutex_. Hands an unconsumed reservation back (only when nothing can write it any more: retire / teardown).
void StereoCaptureSlot::releaseDirect() noexcept {
    if(md_.valid&&mdMailbox_){mdMailbox_->releaseDirect(md_);++mdReleases_;}
    md_={};mdEyes_=0;mdSwap_=false;
}
namespace {
bool warpDepthFormat(VkFormat f) noexcept {
    return f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_X8_D24_UNORM_PACK32||f==VK_FORMAT_D32_SFLOAT||f==VK_FORMAT_D32_SFLOAT_S8_UINT||f==VK_FORMAT_D16_UNORM||f==VK_FORMAT_D16_UNORM_S8_UINT;
}
VkImageAspectFlags warpAspects(VkFormat f) noexcept {
    return VK_IMAGE_ASPECT_DEPTH_BIT|((f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_D32_SFLOAT_S8_UINT||f==VK_FORMAT_D16_UNORM_S8_UINT)?VK_IMAGE_ASPECT_STENCIL_BIT:0);
}
void warpBarrier(VkCommandBuffer cmd,VkImage image,VkImageAspectFlags aspect,VkImageLayout before,VkImageLayout after,VkAccessFlags src,VkAccessFlags dst,std::uint32_t layer=0) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=before;b.newLayout=after;b.srcAccessMask=src;b.dstAccessMask=dst;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={aspect,0,1,layer,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
}
// Depth direct: one eye into direct_ (SpaceWarp.h SpaceWarpDirect). Caller holds mutex_; false = nothing recorded, why set.
bool StereoCaptureSlot::recordDirect(VkCommandBuffer cmd,unsigned eye,const PassImage& s,unsigned& why) {
    if(spaceWarpControl.directRefused()){why=spacewarp::DirectMailbox;return false;} // the mailbox could not consume a direct pair
    const VkFormat format=direct_.targetFor(physical_);const Extent target=spaceWarpControl.extent();
    if(format!=VK_FORMAT_UNDEFINED&&direct_.hasImages()&&!direct_.matches(format,target)){
        // Replacement only (format and MV rect are fixed per device/session), never between the eyes of one pair; the
        // previous mailbox copy may still read the old images.
        if(warpRecorded_||(lastTicketValid_&&mailbox_&&!mailbox_->waitCopy(lastTicket_,reuseWaitNs))){why=spacewarp::DirectImages;return false;}
    }
    // the pass objects shared by both slots, built in the background (never compiled here); not ready = "building".
    SpaceWarpDirectShared shared{};if(format!=VK_FORMAT_UNDEFINED)(void)spaceWarpDirectPipeline.get(device_,format,shared);
    const bool timed=(warpTicket_&1)&&warpQueryPool(); // warpDepth lane, capture part (DETAIL): same queries as the copy
    const SpaceWarpDirect::Source source{s.image,s.format,s.extent,s.layout,s.usage,s.arrayLayer};
    if(!direct_.record(physical_,device_,cmd,eye,source,target,&shared,spaceWarpRestore.ready.load(std::memory_order_acquire),
                       spaceWarpRestore.restore.load(std::memory_order_acquire),timed?warpQueries_:VK_NULL_HANDLE,2*eye,why))return false;
    if(timed)warpTimed_|=1u<<eye;
    if(!warpDual_&&warpDepth_[0].image&&(!warpFullTicketValid_||!mailbox_||mailbox_->copyComplete(warpFullTicket_))){
        for(auto& image:warpDepth_)destroyOwnedImage(device_,image);
        warpFormat_={};warpExtent_={};warpFullTicketValid_=false;++warpFullReleases_;
    }
    return true;
}
bool StereoCaptureSlot::recordDepth(VkCommandBuffer cmd,unsigned eye,const PassImage& s) {
    std::lock_guard guard(mutex_);
    if(!spaceWarpControl.producing())return false; // OFF or suspended by the AppSW budget guard
    auto reject=[&]{++warpRejected_;return false;};
    // TRANSFER_SRC usage is checked by the copy path only (the direct pass samples the eye depth).
    if(!active_ || poisoned_ || cancelled_ || inFlight_ || owner_!=std::this_thread::get_id() || !cmd || eye>1 ||
       (passCommand_ && cmd!=passCommand_) || !(recorded_&(1u<<eye)) || (warpRecorded_&(1u<<eye)) || !s.image ||
       s.family!=family_ || s.samples!=VK_SAMPLE_COUNT_1_BIT || !warpDepthFormat(s.format) ||
       !s.extent.width || !s.extent.height || !s.arrayLayers || s.arrayLayer>=s.arrayLayers ||
       (s.layout!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && s.layout!=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL &&
        s.layout!=VK_IMAGE_LAYOUT_GENERAL && s.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && s.layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))return reject();
    // Depth direct (debug.rdrvr.spacewarp.depthdirect, default 1): the pair's path is decided at its first recorded eye.
    // depthdual: one requested pair also records the full-size copy (for the both-paths dump).
    if(!warpRecorded_){warpDirect_=spaceWarpControl.depthDirect();warpDualRecorded_=0;
        warpDual_=warpDirect_&&spacewarp::claim(spacewarp::dualRequests,spacewarp::dualTaken)!=0;}
    if(warpDirect_){
        unsigned why=spacewarp::DirectReasonCount;
        if(recordDirect(cmd,eye,s,why)){
            ++warpDirectEyes_;spaceWarpControl.noteDirectEye(true,0);
            warpRecorded_|=1u<<eye;if(warpRecorded_==3u)spaceWarpControl.noteDirectPair(true);
            if(warpDual_){ // The copy path's full-size copy of the same eye depth, untimed (the lane belongs to the direct pass)
                if(recordCopy(cmd,eye,s,false))warpDualRecorded_|=1u<<eye;else ++warpDualFailures_;
                if(warpRecorded_==3u&&warpDualRecorded_==3u)++warpDualPairs_;}
            return true;
        }
        ++warpDirectFallbacks_;warpDirectLastReason_=int(why);
        // The other eye is already direct: no mixed pair (this pair goes without motion data; "pair" in the reasons).
        if(warpRecorded_){spaceWarpControl.noteDirectEye(false,spacewarp::DirectPair);return reject();}
        spaceWarpControl.noteDirectEye(false,why);warpDirect_=false;warpDual_=false; // the whole pair takes the full-size copy
    }
    if(!recordCopy(cmd,eye,s,true))return reject();
    warpRecorded_|=1u<<eye;if(warpRecorded_==3u)spaceWarpControl.noteDirectPair(false);
    return true;
}
// The full-size copy path of one eye. Caller holds mutex_ and checked the PassImage; false = nothing
// recorded. time: timestamps around it (warpDepth lane, capture part) - false for the depthdual copy next to a direct pass.
bool StereoCaptureSlot::recordCopy(VkCommandBuffer cmd,unsigned eye,const PassImage& s,bool time) {
    if(!(s.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT))return false;
    VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(physical_,s.format,&p);
    constexpr VkFormatFeatureFlags blitBits=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
    const bool blit=(p.optimalTilingFeatures&blitBits)==blitBits;
    // The mailbox samples this image or copies it (fallback).
    if(!(p.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))return false;
    const auto wanted=spaceWarpControl.extent();
    // Nearest blit to the motion-vector rect; without blit support a full-resolution copy (counted).
    const Extent target=blit?Extent{std::min(wanted.width,s.extent.width),std::min(wanted.height,s.extent.height)}:s.extent;
    if(!target.width || !target.height)return false;
    if(!warpDepth_[0].image || warpFormat_!=s.format || !(warpExtent_==target)) {
        if(eye&&warpDepth_[0].image&&(warpDual_?(warpDualRecorded_&1u):(warpRecorded_&1u)))return false;
        if(warpDepth_[0].image && lastTicketValid_ && mailbox_ && !mailbox_->waitCopy(lastTicket_,reuseWaitNs))return false;
        std::array<OwnedImage,2> next{};
        // SAMPLED + a DEPTH-aspect view for the mailbox's slot-depth downsample pass.
        try{for(auto& image:next)image=createOwnedImage(physical_,device_,target,s.format,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT,true);}
        catch(...){for(auto& image:next)destroyOwnedImage(device_,image);return false;}
        for(auto& image:warpDepth_)destroyOwnedImage(device_,image);
        warpDepth_=next;warpFormat_=s.format;warpExtent_=target;++warpCreates_;
    }
    const auto aspect=warpAspects(s.format);
    const bool timed=time&&(warpTicket_&1)&&warpQueryPool(); // warpDepth lane (capture part)
    warpBarrier(cmd,s.image,aspect,s.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,s.arrayLayer);
    warpBarrier(cmd,warpDepth_[eye].image,aspect,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
    if(timed){vkCmdResetQueryPool(cmd,warpQueries_,2*eye,2);vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,warpQueries_,2*eye);}
    if(blit){
        VkImageBlit b{};b.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,s.arrayLayer,1};b.dstSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
        b.srcOffsets[1]={static_cast<std::int32_t>(s.extent.width),static_cast<std::int32_t>(s.extent.height),1};
        b.dstOffsets[1]={static_cast<std::int32_t>(target.width),static_cast<std::int32_t>(target.height),1};
        vkCmdBlitImage(cmd,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,warpDepth_[eye].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&b,VK_FILTER_NEAREST);++warpBlits_;
    }else{
        VkImageCopy c{};c.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,s.arrayLayer,1};c.dstSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};c.extent={target.width,target.height,1};
        vkCmdCopyImage(cmd,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,warpDepth_[eye].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);++warpCopies_;
    }
    // Left sampled for the mailbox (SpaceWarpSource::layout); the copy fallback transitions it to TRANSFER_SRC itself.
    warpBarrier(cmd,warpDepth_[eye].image,aspect,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT);
    warpBarrier(cmd,s.image,aspect,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,s.arrayLayer);
    if(timed){vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,warpQueries_,2*eye+1);warpTimed_|=1u<<eye;}
    return true;
}
// One timestamp pair per eye (capture part of the warpDepth lane); lazily created, false = unsupported.
bool StereoCaptureSlot::warpQueryPool() noexcept {
    if(warpQueries_)return true;
    if(warpQueryTried_||!device_||!physical_)return false;
    warpQueryTried_=true;
    std::uint32_t count{};vkGetPhysicalDeviceQueueFamilyProperties(physical_,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(physical_,&count,families.data());
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical_,&props);
    if(family_>=count||!families[family_].timestampValidBits||!(props.limits.timestampPeriod>0))return false;
    const auto bits=families[family_].timestampValidBits;warpMask_=bits>=64?UINT64_MAX:(1ull<<bits)-1;warpPeriod_=props.limits.timestampPeriod;
    VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=4;
    if(vkCreateQueryPool(device_,&q,nullptr,&warpQueries_)!=VK_SUCCESS){warpQueries_={};return false;}
    return true;
}
// Caller holds mutex_; the pass that recorded both eyes' depth copies is GPU-complete (retirement). Non-blocking.
void StereoCaptureSlot::readWarpTiming() noexcept {
    const bool timed=warpTimed_==3u&&passSubmitted_&&warpQueries_;warpTimed_=0;
    if(!timed||!diagnostics::profilerTicketValid(warpTicket_))return;
    struct Value {std::uint64_t ticks,available;};std::array<Value,4> v{};
    const auto r=vkGetQueryPoolResults(device_,warpQueries_,0,4,sizeof(v),v.data(),sizeof(Value),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if(r!=VK_SUCCESS||!v[0].available||!v[1].available||!v[2].available||!v[3].available){++warpTimingUnavailable_;return;}
    const double ns=double(((v[1].ticks-v[0].ticks)&warpMask_)+((v[3].ticks-v[2].ticks)&warpMask_))*warpPeriod_;
    if(!(ns>=0)||ns>1e9){++warpTimingUnavailable_;return;}
    spaceWarpControl.noteDepthGpu(0,latest_.frame.renderFrameId,std::max<std::uint64_t>(1,std::uint64_t(ns)),warpDirect_);
}
void StereoCaptureSlot::setWarpFrame(const SpaceWarpFrame& f) noexcept {
    std::lock_guard guard(mutex_);
    if(!active_ || !f.valid || f.frame!=latest_.frame.renderFrameId)return;
    warpFrame_=f;warpFrameSet_=true;++warpFrames_;
}
// Caller holds mutex_. Both depth blits were recorded in the pass command and submitted.
const SpaceWarpSource* StereoCaptureSlot::warpSource(SpaceWarpSource& out) const noexcept {
    if(warpRecorded_!=3u || !warpFrameSet_ || (submitted_&3u)!=3u)return nullptr;
    out.frame=warpFrame_;
    if(warpDirect_){ // The MV-rect pair written by the direct passes (render pass final layout TRANSFER_SRC_OPTIMAL)
        if(!direct_.image(0) || !direct_.image(1))return nullptr;
        out.depth={direct_.image(0),direct_.image(1)};out.format=direct_.format();out.extent=direct_.extent();
        out.views={};out.layout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;out.direct=true;
        if(warpDual_&&warpDualRecorded_==3u&&warpDepth_[0].view&&warpDepth_[1].view){ // depthdual: + the copy path's full-size copy
            out.dualDepth={warpDepth_[0].image,warpDepth_[1].image};out.dualViews={warpDepth_[0].view,warpDepth_[1].view};out.dualFormat=warpFormat_;out.dualExtent=warpExtent_;}
    }else{
        if(!warpDepth_[0].image || !warpDepth_[1].image)return nullptr;
        out.depth={warpDepth_[0].image,warpDepth_[1].image};out.format=warpFormat_;out.extent=warpExtent_;
        out.views={warpDepth_[0].view,warpDepth_[1].view};out.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    if(swapEyes()){ // SWAP EYES test: same swap as submission() (a direct pair's swap is fixed at its reservation)
        std::swap(out.depth[0],out.depth[1]);std::swap(out.views[0],out.views[1]);std::swap(out.frame.relClip[0],out.frame.relClip[1]);std::swap(out.frame.position[0],out.frame.position[1]);
        std::swap(out.dualDepth[0],out.dualDepth[1]);std::swap(out.dualViews[0],out.dualViews[1]);
    }
    return &out;
}
void StereoCaptureSlot::commandInvalidated(VkCommandBuffer cmd) noexcept {
    std::lock_guard guard(mutex_);if(!active_ || !cmd)return;
    if(passCommand_==cmd){if(!externalEpoch_||!passSubmitted_)cancelled_=true;if(!passSubmitted_)passDiscarded_=true;}
    for(unsigned i=0;i<2;++i)if((recorded_&(1u<<i)) && commands_[i]==cmd){
        if(!externalEpoch_||!(submitted_&(1u<<i)))cancelled_=true;
        if(!(submitted_&(1u<<i))){recorded_&=~(1u<<i);commands_[i]={};}
    }
}
bool StereoCaptureSlot::carriesPass(VkQueue q,std::uint32_t count,const VkSubmitInfo* submits) noexcept {
    std::lock_guard guard(mutex_);
    if(!active_||inFlight_||!passCommand_||passDiscarded_||passSubmitted_||!count||!submits||count>256||std::find(queues_.begin(),queues_.end(),q)==queues_.end())return false;
    for(std::uint32_t j=0;j<count;++j){
        if(submits[j].commandBufferCount>4096||(submits[j].commandBufferCount&&!submits[j].pCommandBuffers))return false;
        for(std::uint32_t k=0;k<submits[j].commandBufferCount;++k)if(submits[j].pCommandBuffers[k]==passCommand_)return true;}
    return false;
}
unsigned StereoCaptureSlot::submitted(VkQueue q,std::uint32_t count,const VkSubmitInfo* submits,VkResult result,StereoMailbox* mailbox,std::uint64_t nowNs) noexcept {
    std::lock_guard guard(mutex_);
    if(!active_ || inFlight_ || std::find(queues_.begin(),queues_.end(),q)==queues_.end() || !count || !submits)return 0;
    if(result!=VK_SUCCESS){poisoned_=true;return 0;}
    if(count>256)return 0;
    unsigned flags=0;
    for(std::uint32_t j=0;j<count;++j) {
        if(submits[j].commandBufferCount>4096 || (submits[j].commandBufferCount && !submits[j].pCommandBuffers))return flags;
        for(std::uint32_t k=0;k<submits[j].commandBufferCount;++k){
            if(passCommand_ && !passDiscarded_ && !passSubmitted_ && submits[j].pCommandBuffers[k]==passCommand_){
                passSubmitted_=true;flags|=passObserved;completion_[2].observe(q,submits[j]);if(q!=queue_)++crossQueueSubmissions_;}
            for(unsigned eye=0;eye<2;++eye)if((recorded_&(1u<<eye)) && !(submitted_&(1u<<eye)) && submits[j].pCommandBuffers[k]==commands_[eye]){submitted_|=1u<<eye;completion_[eye].observe(q,submits[j]);}
        }
    }
    // The hook is still inside the engine's vkQueueSubmit call, preserving its
    // external queue serialization. No host wait and no stock semaphore change.
    // A marker is queued for EVERY submitted pass whose recorded
    // commands were all submitted on this queue (it proves their completion),
    // so partial/cancelled pairs retire through finish() without any wait.
    // Only a single verified command containing the entire pass is published.
    if(!earlyCompletion_||!passSubmitted_||submitted_!=recorded_||earlyFrame_==latest_.frame.renderFrameId||
       completion_[2].queue!=q||!sameQueue(q))return flags;
    const bool complete=recorded_==3&&submitted_==3&&commands_[0]==passCommand_&&commands_[1]==passCommand_&&!cancelled_;
    if(owner_!=std::this_thread::get_id()){++earlyWrongThread_;return flags;}
    if(earlySubmitted_){
        // An older marker of this slot is still pending (the slot was retired
        // by a present fence): keep the classic present/early paths this frame.
        const auto status=vkGetFenceStatus(device_,earlyFence_);
        if(status==VK_NOT_READY)return flags;
        if(status!=VK_SUCCESS){poisoned_=true;return flags;}
        earlySubmitted_=false;
    }
    if(!earlyFence_){VkFenceCreateInfo c{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if(vkCreateFence(device_,&c,nullptr,&earlyFence_)!=VK_SUCCESS){earlyFence_={};++earlyFailed_;return flags;}}
    if(vkResetFences(device_,1,&earlyFence_)!=VK_SUCCESS){++earlyFailed_;return flags;}
    // Publish in flight. The mailbox copy is queued on q right
    // behind the pass (queue order + ALL_COMMANDS barriers), then the marker,
    // whose fence therefore covers pass AND copy. No CPU wait here.
    if(complete && mailbox && !publishRejected_){
        try{if(publishInFlight(q,earlyFence_,*mailbox,nowNs,true))flags|=publishedNow;}
        catch(...){poisoned_=true;return flags;}
    }
    if(!complete)++partialMarkers_;
    VkSubmitInfo marker{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if(vkQueueSubmit(q,1,&marker,earlyFence_)!=VK_SUCCESS){++earlyFailed_;markerCoversCopy_=false;return flags;}
    earlySubmitted_=true;
    earlyFrame_=latest_.frame.renderFrameId;earlyQueue_=q;earlyThread_=std::this_thread::get_id();++earlyMarkers_;
    return flags;
}
bool StereoCaptureSlot::completePresent(VkQueue q,VkFence fence,StereoMailbox& mailbox,std::uint64_t nowNs,std::span<const VkSemaphore> completedWaits) {
    std::lock_guard guard(mutex_);
    if(inFlight_){mailbox_=&mailbox;retireReady();return false;}
    if(!active_ || poisoned_ || submitted_!=recorded_ || (passCommand_ && !passSubmitted_ && !passDiscarded_) ||
       q!=queue_ || !fence)return false;
    bool covered=!passSubmitted_||completion_[2].covered(q,completedWaits);
    for(unsigned eye=0;eye<2;++eye)if(submitted_&(1u<<eye))covered&=completion_[eye].covered(q,completedWaits);
    if(!covered){++unprovenPresents_;return false;}
    const auto status=vkGetFenceStatus(device_,fence);
    if(status==VK_SUCCESS)return finish(q,fence,mailbox,nowNs);
    if(status!=VK_NOT_READY){poisoned_=true;return false;}
    // The caller consumed the covering waits on q in the batch that
    // signals fence, so a copy queued on q now runs after the pass. Publish in
    // flight; retire when that copy is observed complete.
    if(!cancelled_ && recorded_==3 && !publishRejected_ && publishInFlight(q,fence,mailbox,nowNs,false))return true;
    // Nothing (more) to publish, but retirement still needs GPU completion.
    // The slot's own marker covers the pass: the early poll/throttle retires it.
    if(earlySubmitted_ && earlyFrame_==latest_.frame.renderFrameId)return false;
    // Nothing ever reached the GPU (pass discarded / never recorded).
    if(!passSubmitted_ && submitted_==0){discardRetire();return false;}
    // Queue a slot-owned fence on q behind the batch that consumed
    // the covering waits; polls retire the slot once it signals. No wait here.
    if(retireFenceSubmitted_ && retireFenceFrame_==latest_.frame.renderFrameId)return false;
    if(queueRetireFence(q))return false;
    ++discardWaits_;
    diagnostics::WallScope timing(diagnostics::frameTimings.stereoFenceWait);
    const auto waited=vkWaitForFences(device_,1,&fence,VK_TRUE,discardWaitNs);
    timing.finish();
    if(waited==VK_SUCCESS)return finish(q,fence,mailbox,nowNs);
    if(waited!=VK_TIMEOUT)poisoned_=true;
    return false;
}
bool StereoCaptureSlot::completeSubmitted(VkQueue serializedQueue,StereoMailbox& mailbox,std::uint64_t nowNs){
    std::lock_guard guard(mutex_);
    ++earlyPolls_;
    if(inFlight_){mailbox_=&mailbox;retireReady();return false;}
    if(retireDiscarded())return false;
    // Partial/cancelled passes have markers too; finish() discards them.
    if(!earlyCompletion_||!active_||poisoned_||!earlyFence_||!earlySubmitted_||earlyFrame_!=latest_.frame.renderFrameId||
       !passSubmitted_||submitted_!=recorded_)return false;
    if(serializedQueue!=earlyQueue_){++earlyWrongQueue_;return false;}
    if(earlyThread_!=std::this_thread::get_id()||owner_!=earlyThread_){++earlyWrongThread_;return false;}
    const auto result=vkGetFenceStatus(device_,earlyFence_);
    if(result==VK_NOT_READY){++earlyNotReady_;return false;}
    if(result!=VK_SUCCESS){++earlyFailed_;poisoned_=true;return false;}
    ++earlyReady_;earlySubmitted_=false;
    const bool published=finish(earlyQueue_,earlyFence_,mailbox,nowNs);if(published)++earlyPublished_;return published;
}
bool StereoCaptureSlot::pollRetire() noexcept {
    std::lock_guard guard(mutex_);return inFlight_?retireReady():retireDiscarded();
}
StereoCaptureSlot::WaitResult StereoCaptureSlot::waitRetire(VkQueue serializedQueue,StereoMailbox* mailbox,std::uint64_t nowNs,std::uint64_t timeoutNs,bool& published) noexcept {
    std::lock_guard guard(mutex_);published=false;
    if(!active_)return WaitResult::NotPending;
    if(poisoned_)return WaitResult::Unavailable;
    if(inFlight_ && retireReady())return WaitResult::Retired;
    if(!inFlight_ && retireDiscarded())return WaitResult::Retired;
    // A pair queued only behind the slot's own present-queue retire fence.
    if(!inFlight_ && retireFenceSubmitted_ && retireFenceFrame_==latest_.frame.renderFrameId &&
       !(earlySubmitted_ && earlyFrame_==latest_.frame.renderFrameId)){
        ++throttleWaits_;
        const auto result=vkWaitForFences(device_,1,&retireFence_,VK_TRUE,timeoutNs);
        if(result==VK_TIMEOUT){++throttleTimeouts_;return WaitResult::Timeout;}
        if(result!=VK_SUCCESS){poisoned_=true;return WaitResult::Unavailable;}
        return retireDiscarded()?WaitResult::Retired:WaitResult::Unavailable;
    }
    // Only the slot's own marker can be waited on: it follows the pass (and the
    // in-flight copy when markerCoversCopy_) on its queue.
    if(!earlySubmitted_ || earlyFrame_!=latest_.frame.renderFrameId)return WaitResult::Unavailable;
    if(!inFlight_ && (!mailbox || serializedQueue!=earlyQueue_ || owner_!=std::this_thread::get_id() || earlyThread_!=owner_ ||
       !passSubmitted_ || submitted_!=recorded_))return WaitResult::Unavailable;
    ++throttleWaits_;
    const auto result=vkWaitForFences(device_,1,&earlyFence_,VK_TRUE,timeoutNs);
    if(result==VK_TIMEOUT){++throttleTimeouts_;return WaitResult::Timeout;}
    if(result!=VK_SUCCESS){poisoned_=true;return WaitResult::Unavailable;}
    if(inFlight_)return retireReady()?WaitResult::Retired:WaitResult::Unavailable;
    // Not published in flight (mailbox rejected it or was not bound yet):
    // publish now through the early path on the marker's own queue, which
    // the caller (inside the submit hook on serializedQueue) owns.
    earlySubmitted_=false;++earlyReady_;
    try{published=finish(earlyQueue_,earlyFence_,*mailbox,nowNs);if(published)++earlyPublished_;}
    catch(...){poisoned_=true;return WaitResult::Unavailable;}
    return active_?WaitResult::Unavailable:WaitResult::Retired;
}
RenderSubmission StereoCaptureSlot::submission(VkFence fence) const {
    RenderSubmission f{};f.frame=latest_.frame;f.completion={CompletionKind::Fence,reinterpret_cast<std::uint64_t>(fence),0};
    // Diagnostic (STEREO TEST "SWAP EYES"): publish the eyes swapped (image, pose and fov), so the system
    // screenshot (left eye) shows the right eye's rendering.
    const bool swapEyes=this->swapEyes(); // A direct pair's slot already holds the eyes in its reservation's order
    for(unsigned i=0;i<2;++i){auto& e=f.eyes[i];const unsigned src=swapEyes?1u-i:i;e.renderFrameId=f.frame.renderFrameId;e.epochs=f.frame.epochs;
        e.image=reinterpret_cast<std::uint64_t>(images_[src].image);e.lifetimeToken=resourceEpoch_;e.extent=latest_.extents[src];e.format=latest_.formats[src];
        e.layout=VK_IMAGE_LAYOUT_GENERAL;e.queueFamily=family_;e.renderedPose=latest_.poses[src];e.renderedFov=latest_.fov[src];}
    return f;
}
bool StereoCaptureSlot::sameQueue(VkQueue q) const {
    if(passSubmitted_ && completion_[2].queue!=q)return false;
    for(unsigned eye=0;eye<2;++eye)if((submitted_&(1u<<eye)) && completion_[eye].queue!=q)return false;
    return true;
}
void StereoCaptureSlot::retire() noexcept {
    readWarpTiming(); // Every retirement of submitted work is GPU-proven
    releaseDirect(); // An unconsumed reservation goes back (its pass is GPU-proven complete or never reached the GPU)
    direct_.retired(device_); // The pass that sampled through this pair's engine depth views is complete (or never ran)
    active_=false;inFlight_=false;retiredFrame_=latest_.frame.renderFrameId;recorded_=0;
}
// A publication whose warp source was the full-size pair (conservatively: any recorded copy pair) keeps those images alive.
void StereoCaptureSlot::notePublished(const StereoCaptureTicket& ticket) noexcept {
    if(warpRecorded_==3u&&(!warpDirect_||warpDualRecorded_)){warpFullTicket_=ticket;warpFullTicketValid_=true;}
}
void StereoCaptureSlot::discardRetire() noexcept {
    // Same layout bookkeeping as finish(): a submitted copy ran its transitions.
    for(unsigned i=0;i<2;++i)if((submitted_&(1u<<i))&&!(mdEyes_&(1u<<i)))initialized_[i]=true; // A direct eye never touched images_
    retire();++discarded_;++retiredDiscarded_;
}
// Unpublished pairs that need no marker wait: nothing submitted at
// all (pass recording invalidated), or the present-queue retire fence signalled.
bool StereoCaptureSlot::retireDiscarded() noexcept {
    if(!active_ || inFlight_ || poisoned_)return false;
    if(!passSubmitted_ && passDiscarded_ && submitted_==0){discardRetire();return true;}
    if(!retireFenceSubmitted_ || retireFenceFrame_!=latest_.frame.renderFrameId)return false;
    const auto status=vkGetFenceStatus(device_,retireFence_);
    if(status==VK_NOT_READY)return false;
    if(status!=VK_SUCCESS){poisoned_=true;return false;}
    retireFenceSubmitted_=false;discardRetire();return true;
}
// Empty batch on the present queue q, queued after the caller's batch that
// consumed the covering waits: its fence signals after those waits' signal
// operations (signal-operation order), i.e. after every covered submission.
bool StereoCaptureSlot::queueRetireFence(VkQueue q) noexcept {
    if(retireFenceSubmitted_){
        const auto status=vkGetFenceStatus(device_,retireFence_);
        if(status!=VK_SUCCESS)return false; // an older use still pending: fall back
        retireFenceSubmitted_=false;
    }
    if(!retireFence_){VkFenceCreateInfo c{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if(vkCreateFence(device_,&c,nullptr,&retireFence_)!=VK_SUCCESS){retireFence_={};return false;}}
    if(vkResetFences(device_,1,&retireFence_)!=VK_SUCCESS)return false;
    VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if(vkQueueSubmit(q,1,&empty,retireFence_)!=VK_SUCCESS)return false;
    retireFenceSubmitted_=true;retireFenceFrame_=latest_.frame.renderFrameId;++retireFences_;return true;
}
// In-flight publication retires once the GPU has provably finished the pass
// and the copy reading images_: the slot's marker (if submitted) has signalled, and
// either it was queued after the copy or the copy ticket itself completed.
bool StereoCaptureSlot::retireReady() noexcept {
    if(!active_ || !inFlight_ || poisoned_)return false;
    if(earlySubmitted_){
        const auto status=vkGetFenceStatus(device_,earlyFence_);
        if(status==VK_NOT_READY)return false;
        if(status!=VK_SUCCESS){poisoned_=true;return false;}
        earlySubmitted_=false;
    }
    // markerCoversCopy_ is only set together with a submitted marker, and the
    // marker is only cleared after it was observed signalled.
    if(!markerCoversCopy_ && !(mailbox_ && mailbox_->copyComplete(ticket_)))return false;
    retire();++retiredInFlight_;return true;
}
bool StereoCaptureSlot::publishInFlight(VkQueue q,VkFence completion,StereoMailbox& mailbox,std::uint64_t nowNs,bool markerAfter){
    // The submitted eye copies have executed their layout transitions once the
    // pass runs; a later recording must start from GENERAL.
    for(unsigned i=0;i<2;++i)if((submitted_&(1u<<i))&&!(mdEyes_&(1u<<i)))initialized_[i]=true; // A direct eye never touched images_
    StereoCaptureTicket ticket{};
    SpaceWarpSource warp{};
    const auto* direct=directPair(); // Unconsumed on a rejection (handed back at retire)
    if(!mailbox.capture(q,submission(completion),nowNs,latest_.encoding,latest_.reticle,&ticket,true,sampled_,warpSource(warp),direct)){++inFlightRejected_;publishRejected_=true;return false;}
    if(direct){md_={};++mdConsumed_;} // The mailbox owns the slot again
    inFlight_=true;ticket_=ticket;lastTicket_=ticket;lastTicketValid_=true;mailbox_=&mailbox;markerCoversCopy_=markerAfter;notePublished(ticket);
    ++published_;++inFlightPublished_;return true;
}
bool StereoCaptureSlot::finish(VkQueue q,VkFence fence,StereoMailbox& mailbox,std::uint64_t nowNs){
    // A recording alone never changes the actual image layout. In particular,
    // a discarded first recording must leave a new image UNDEFINED next time.
    for(unsigned i=0;i<2;++i)if((submitted_&(1u<<i))&&!(mdEyes_&(1u<<i)))initialized_[i]=true; // A direct eye never touched images_
    if(cancelled_ || recorded_!=3 || publishRejected_){retire();++discarded_;return false;}
    StereoCaptureTicket ticket{};bool result=false;
    SpaceWarpSource warp{};
    const auto* direct=directPair();
    try{result=mailbox.capture(q,submission(fence),nowNs,latest_.encoding,latest_.reticle,&ticket,false,sampled_,warpSource(warp),direct);}
    catch(...){poisoned_=true;throw;} // mailbox may still reference images_ after a failed submission.
    if(result&&direct){md_={};++mdConsumed_;} // The mailbox owns the slot again
    if(!result){retire();++discarded_;return false;}
    ++published_;lastTicket_=ticket;lastTicketValid_=true;mailbox_=&mailbox;notePublished(ticket);
    // Same queue as every source command: the next pass reusing images_ is
    // queued after this copy. Otherwise keep the images owned until the copy
    // is observed complete (no wait on this thread).
    if(sameQueue(q)){retire();return true;}
    inFlight_=true;ticket_=ticket;markerCoversCopy_=false;return true;
}
void StereoCaptureSlot::shutdownRetired() {
    std::lock_guard guard(mutex_);accepting_=false;
    if(!device_)return;
    const auto idle=vkDeviceWaitIdle(device_);
    if(idle!=VK_SUCCESS && idle!=VK_ERROR_DEVICE_LOST)throw std::runtime_error("pass capture GPU objects still in flight");
    if(mailbox_ && images_[0].image)(void)mailbox_->releaseSources({images_[0].image,images_[1].image},0);
    for(auto& image:images_)destroyOwnedImage(device_,image);
    for(auto& image:warpDepth_)destroyOwnedImage(device_,image);
    direct_.destroy(device_);warpFullTicketValid_=false;
    if(warpQueries_)vkDestroyQueryPool(device_,warpQueries_,nullptr);warpQueries_={};warpQueryTried_=false;warpTimed_=0;
    if(earlyFence_)vkDestroyFence(device_,earlyFence_,nullptr);earlyFence_={};earlyFrame_=0;earlySubmitted_=false;
    if(retireFence_)vkDestroyFence(device_,retireFence_,nullptr);retireFence_={};retireFenceSubmitted_=false;
    releaseDirect(); // Device idle (a mailbox already shut down destroyed its orphans: no-op then)
    active_=false;inFlight_=false;lastTicketValid_=false;mailbox_=nullptr;recorded_=0;queues_.clear();device_={};physical_={};
}
std::string StereoCaptureSlot::report() const {
    std::lock_guard guard(mutex_);std::ostringstream s;
    s<<"{\"preparedFrames\":"<<prepared_<<",\"publishedPairs\":"<<published_<<",\"discardedPairs\":"<<discarded_
     <<",\"crossQueueSubmissions\":"<<crossQueueSubmissions_<<",\"unprovenPresents\":"<<unprovenPresents_<<",\"rejectedCalls\":"<<rejected_<<",\"recordedEyeMask\":"<<recorded_<<",\"submittedEyeMask\":"<<submitted_<<",\"resourceEpoch\":"<<resourceEpoch_
     <<",\"initializedMask\":"<<(unsigned(initialized_[0])|(unsigned(initialized_[1])<<1))
     <<",\"retiredFrame\":"<<retiredFrame_<<",\"passSubmitted\":"<<(passSubmitted_?"true":"false")<<",\"passDiscarded\":"<<(passDiscarded_?"true":"false")
     <<",\"earlyMarkers\":"<<earlyMarkers_<<",\"earlyPublished\":"<<earlyPublished_<<",\"earlyNotReady\":"<<earlyNotReady_<<",\"earlyWrongThread\":"<<earlyWrongThread_<<",\"earlyFailed\":"<<earlyFailed_
     <<",\"earlyPolls\":"<<earlyPolls_<<",\"earlyReady\":"<<earlyReady_<<",\"earlyWrongQueue\":"<<earlyWrongQueue_
     <<",\"inFlightPublished\":"<<inFlightPublished_<<",\"inFlightRejected\":"<<inFlightRejected_<<",\"retiredInFlight\":"<<retiredInFlight_
     <<",\"throttleFenceWaits\":"<<throttleWaits_<<",\"throttleTimeouts\":"<<throttleTimeouts_<<",\"discardWaits\":"<<discardWaits_<<",\"reuseWaitFailures\":"<<reuseWaits_
     <<",\"warpBlits\":"<<warpBlits_<<",\"warpCopies\":"<<warpCopies_<<",\"warpTimingUnavailable\":"<<warpTimingUnavailable_<<",\"warpRejected\":"<<warpRejected_<<",\"warpCreates\":"<<warpCreates_<<",\"warpFrames\":"<<warpFrames_<<",\"partialMarkers\":"<<partialMarkers_<<",\"retireFences\":"<<retireFences_<<",\"retiredDiscarded\":"<<retiredDiscarded_
     <<",\"warpDirectEyes\":"<<warpDirectEyes_<<",\"warpDirectFallbacks\":"<<warpDirectFallbacks_<<",\"warpDirectLastReason\":\""<<(warpDirectLastReason_>=0?spacewarp::directReasonName(unsigned(warpDirectLastReason_)):"")<<'"'
     <<",\"warpDirectCreates\":"<<direct_.creates()<<",\"warpFullReleases\":"<<warpFullReleases_<<",\"warpFullImages\":"<<(warpDepth_[0].image?"true":"false")
     <<",\"warpDualPairs\":"<<warpDualPairs_<<",\"warpDualFailures\":"<<warpDualFailures_
     // MAILBOX DIRECT: mode (last read), pairs recorded into a reserved ring slot, refused (copy path), noMailbox, consumed by capture(),
     // releases (unconsumed, handed back at retire), held (this slot's pair holds a reservation now)
     <<",\"mailboxDirect\":{\"mode\":"<<mdMode_<<",\"pairs\":"<<mdPairs_<<",\"refused\":"<<mdRefused_<<",\"noMailbox\":"<<mdNoMailbox_
     <<",\"consumed\":"<<mdConsumed_<<",\"releases\":"<<mdReleases_<<",\"held\":"<<(md_.valid?"true":"false")<<'}'
     <<",\"inFlight\":"<<(inFlight_?"true":"false")<<",\"markerCoversCopy\":"<<(markerCoversCopy_?"true":"false")<<",\"markerPending\":"<<(earlySubmitted_?"true":"false")
     <<",\"awaitingPresent\":"<<(active_&&!inFlight_?"true":"false")<<",\"cancelled\":"<<(cancelled_?"true":"false")<<",\"poisoned\":"<<(poisoned_?"true":"false")<<'}';return s.str();
}

void GameStereoCapture::bind(const XrGraphicsBindingVulkan2KHR& b,bool early,bool buffered){
    std::lock_guard lock(mutex_);count_=buffered?2:1;for(unsigned i=0;i<count_;++i)slots_[i].bind(b,early,buffered);
    // Start the AppSW depth-direct pipeline build now (background thread, shared by both slots), so the first
    // SPACEWARP pair finds it ready instead of compiling inside the engine's command recording.
    directDevice_=b.device;spaceWarpDirectPipeline.request(b.device,SpaceWarpDirect::targetFormat(b.physicalDevice));
}
void GameStereoCapture::poll() noexcept {for(unsigned i=0;i<count_;++i)slots_[i].pollRetire();}
bool GameStereoCapture::beginFrame(VkQueue q,const CaptureFrame& frame,VkCommandBuffer command){
    std::lock_guard lock(mutex_);if(frame.frame.renderFrameId<=lastFrame_)return false;
    poll();
    for(unsigned i=0;i<count_;++i)if(!slots_[i].pendingFrame()){
        auto f=frame;const bool changed=!generation_||frame.extents!=config_.extents||frame.formats!=config_.formats||frame.encoding!=config_.encoding;
        const auto epoch=generation_+unsigned(changed);if(!epoch)return false;f.frame.epochs.resources=epoch;
        if(!slots_[i].beginFrame(q,f,command))return false;
        recording_=i;lastFrame_=frame.frame.renderFrameId;generation_=epoch;config_=frame;return true;
    }
    ++slotPressure_;return false;
}
void GameStereoCapture::cancelFrame(std::uint64_t id)noexcept{std::lock_guard lock(mutex_);for(unsigned i=0;i<count_;++i)slots_[i].cancelFrame(id);}
std::uint64_t GameStereoCapture::retiredFrame()const noexcept{
    std::lock_guard lock(mutex_);std::uint64_t pending=UINT64_MAX,retired=0;
    for(unsigned i=0;i<count_;++i){const auto id=slots_[i].pendingFrame();if(id)pending=std::min(pending,id);retired=std::max(retired,slots_[i].retiredFrame());}
    return pending==UINT64_MAX?retired:std::min(retired,pending-1);
}
bool GameStereoCapture::recordEye(VkCommandBuffer cmd,unsigned eye,const PassImage& source){std::lock_guard lock(mutex_);return slots_[recording_].recordEye(cmd,eye,source,mailbox_);} // + MAILBOX DIRECT target
bool GameStereoCapture::carriesPass(VkQueue q,std::uint32_t count,const VkSubmitInfo* info)noexcept{
    std::lock_guard lock(mutex_);for(unsigned i=0;i<count_;++i)if(slots_[i].carriesPass(q,count,info))return true;return false;}
bool GameStereoCapture::submitted(VkQueue q,std::uint32_t count,const VkSubmitInfo* info,VkResult result)noexcept{
    std::lock_guard lock(mutex_);const auto now=steadyNs();unsigned flags=0;
    for(unsigned i=0;i<count_;++i){
        const auto f=slots_[i].submitted(q,count,info,result,mailbox_,now);
        if(f&StereoCaptureSlot::publishedNow){published(i,now);++inFlightPublished_;}
        flags|=f;
    }
    if(!(flags&StereoCaptureSlot::passObserved)||count_<2)return (flags&StereoCaptureSlot::passObserved)!=0;
    // Throttle: pass N is queued; if no slot is free for frame N+1,
    // its world draw would skip stereo (PREVIOUS_EYES_PENDING) while the GPU
    // is still on N-1. Wait for N-1 here instead: the GPU keeps pass N.
    poll();
    captureThrottleFed.store(true,std::memory_order_relaxed); // SAT: FramePacer learns only GPU-bound (throttled) frames
    unsigned pending=0;int oldest=-1;
    for(unsigned i=0;i<count_;++i){const auto id=slots_[i].pendingFrame();if(!id)continue;++pending;
        if(oldest<0||id<slots_[unsigned(oldest)].pendingFrame())oldest=int(i);}
    if(pending<count_||oldest<0)return true;
    bool late=false;
    const auto ticket=diagnostics::profilerTicket();const auto start=diagnostics::monotonicNs();
    const auto waited=slots_[unsigned(oldest)].waitRetire(q,mailbox_,now,throttleNs,late);
    const auto end=diagnostics::monotonicNs();
    if(end>start)captureThrottleWaitNs.fetch_add(end-start,std::memory_order_relaxed); // SAT throttle-wait feed
    if(late)published(unsigned(oldest),end);
    // Only real waits are counted/sampled; immediate retirements
    // (already signalled) would pull the window's p50 to 0.
    if(end>=start && end-start>=realWaitNs){
        ++throttleWaits_;
        if((ticket&1) && diagnostics::profilerTicketValid(ticket))throttleWait_.elapsedWall(start,end);
    }else ++throttleImmediate_;
    switch(waited){
    case StereoCaptureSlot::WaitResult::Retired:++throttleRetired_;break;
    case StereoCaptureSlot::WaitResult::Timeout:++throttleTimeouts_;break;
    case StereoCaptureSlot::WaitResult::Unavailable:++throttleUnavailable_;break;
    default:break;
    }
    return true;
}
bool GameStereoCapture::recordDepth(VkCommandBuffer cmd,unsigned eye,const PassImage& depth){std::lock_guard lock(mutex_);return slots_[recording_].recordDepth(cmd,eye,depth);}
void GameStereoCapture::setWarpFrame(const SpaceWarpFrame& f)noexcept{std::lock_guard lock(mutex_);for(unsigned i=0;i<count_;++i)if(f.frame&&slots_[i].pendingFrame()==f.frame)slots_[i].setWarpFrame(f);}
bool GameStereoCapture::warpWanted()noexcept{return spaceWarpControl.producing();} // Not while the budget guard suspends AppSW
void GameStereoCapture::setEngineRestore(bool(*ready)(VkCommandBuffer) noexcept,void(*restore)(VkCommandBuffer) noexcept) noexcept {
    spaceWarpRestore.ready.store(ready,std::memory_order_release);spaceWarpRestore.restore.store(restore,std::memory_order_release);
}
void GameStereoCapture::commandInvalidated(VkCommandBuffer cmd)noexcept{std::lock_guard lock(mutex_);for(unsigned i=0;i<count_;++i)slots_[i].commandInvalidated(cmd);}
bool GameStereoCapture::completePresent(VkQueue q,VkFence fence,StereoMailbox& mailbox,std::uint64_t now,std::span<const VkSemaphore> waits){
    std::lock_guard lock(mutex_);mailbox_=&mailbox;bool result=false;unsigned first=0;
    poll();
    if(count_==2&&slots_[1].pendingFrame()&&(!slots_[0].pendingFrame()||slots_[1].pendingFrame()<slots_[0].pendingFrame()))first=1;
    for(unsigned i=0;i<count_;++i){const auto slot=(first+i)%count_;if(slots_[slot].completePresent(q,fence,mailbox,now,waits)){published(slot,now);result=true;}}
    return result;
}
bool GameStereoCapture::completeSubmitted(VkQueue serializedQueue,StereoMailbox& mailbox,std::uint64_t now){
    std::lock_guard lock(mutex_);mailbox_=&mailbox;bool result=false;unsigned first=0;
    poll();
    if(count_==2&&slots_[1].pendingFrame()&&(!slots_[0].pendingFrame()||slots_[1].pendingFrame()<slots_[0].pendingFrame()))first=1;
    for(unsigned i=0;i<count_;++i){const auto slot=(first+i)%count_;if(slots_[slot].completeSubmitted(serializedQueue,mailbox,now)){published(slot,now);result=true;}}
    return result;
}
void GameStereoCapture::published(unsigned slot,std::uint64_t now) noexcept {
    const auto state=slots_[slot].presentState(VK_NULL_HANDLE);
    // Publication may run on the draw queue. Future present qualification must
    // still match the queue declared by beginFrame, not that producer queue.
    if(state.frame.renderFrameId>published_.renderFrameId){published_=state.frame;publishedQueue_=state.presentQueue;publishedNs_=now;}
}
bool GameStereoCapture::canBypassPresent(VkQueue q,std::uint64_t tracking,std::uint64_t space,std::uint64_t now) noexcept {
    std::lock_guard lock(mutex_);++bypassChecks_;
    poll();
    if(!publishedNs_||publishedQueue_!=q||!published_.renderFrameId||now<publishedNs_||now-publishedNs_>StereoMailbox::maxAgeNs){++bypassNoFresh_;return false;}
    if(!tracking||!space||published_.epochs.tracking!=tracking||published_.epochs.referenceSpace!=space||published_.epochs.resources!=generation_){++bypassEpoch_;return false;}
    unsigned pending=0;bool latest=published_.renderFrameId==lastFrame_,crossQueue=false;
    for(unsigned i=0;i<count_;++i){const auto slot=slots_[i].presentState(q);
        if(!slot.healthy){++bypassUnhealthy_;return false;}
        if(!slot.active)continue;++pending;
        if(slot.frame.epochs.tracking!=tracking||slot.frame.epochs.referenceSpace!=space||slot.frame.epochs.resources!=generation_){++bypassEpoch_;return false;}
        if(!slot.armed){
            ++bypassUnarmed_;
            switch(slot.reason){
            case StereoCaptureSlot::ArmReason::Recording:++bypassRecording_;break;
            case StereoCaptureSlot::ArmReason::Partial:++bypassPartial_;break;
            case StereoCaptureSlot::ArmReason::Marker:++bypassMarker_;break;
            case StereoCaptureSlot::ArmReason::PresentQueue:
            case StereoCaptureSlot::ArmReason::SubmitQueue:++bypassQueue_;break;
            case StereoCaptureSlot::ArmReason::Thread:++bypassThread_;break;
            default:++bypassCommand_;break;
            }
            return false;
        }
        crossQueue|=slot.markerQueue!=q;
        if(slot.frame.renderFrameId==lastFrame_)latest=true;
    }
    if(pending>=count_){++bypassPressure_;return false;}
    if(!latest){++bypassUnarmed_;++bypassLatestMissing_;return false;}
    ++bypassAllowed_;if(crossQueue)++bypassCrossQueue_;return true;
}
void GameStereoCapture::shutdownRetired(){std::lock_guard lock(mutex_);for(unsigned i=0;i<count_;++i)slots_[i].shutdownRetired();mailbox_=nullptr;
    spaceWarpDirectPipeline.destroy(directDevice_);directDevice_={}; // After the slots' per-slot objects (device idle)
}
std::string GameStereoCapture::report()const{std::lock_guard lock(mutex_);if(count_==1)return slots_[0].report();
    std::ostringstream out;out<<"{\"buffered\":true,\"capacity\":2,\"slotPressure\":"<<slotPressure_<<",\"publishedFrame\":"<<published_.renderFrameId
        <<",\"bypassChecks\":"<<bypassChecks_<<",\"bypassAllowed\":"<<bypassAllowed_<<",\"bypassCrossQueue\":"<<bypassCrossQueue_<<",\"bypassNoFresh\":"<<bypassNoFresh_<<",\"bypassEpoch\":"<<bypassEpoch_
        <<",\"bypassUnhealthy\":"<<bypassUnhealthy_<<",\"bypassPressure\":"<<bypassPressure_<<",\"bypassUnarmed\":"<<bypassUnarmed_
        <<",\"bypassRecording\":"<<bypassRecording_<<",\"bypassPartial\":"<<bypassPartial_<<",\"bypassMarker\":"<<bypassMarker_
        <<",\"bypassQueue\":"<<bypassQueue_<<",\"bypassThread\":"<<bypassThread_<<",\"bypassCommand\":"<<bypassCommand_<<",\"bypassLatestMissing\":"<<bypassLatestMissing_
        <<",\"pipeline\":{\"inFlightPublished\":"<<inFlightPublished_<<",\"throttleWaits\":"<<throttleWaits_<<",\"throttleImmediate\":"<<throttleImmediate_<<",\"throttleRetired\":"<<throttleRetired_
        <<",\"throttleTimeouts\":"<<throttleTimeouts_<<",\"throttleUnavailable\":"<<throttleUnavailable_<<",\"throttleWait\":"<<throttleWait_.snapshot().json()<<'}'
        <<",\"slots\":["<<slots_[0].report()<<","<<slots_[1].report()<<"]}";return out.str();}
}
