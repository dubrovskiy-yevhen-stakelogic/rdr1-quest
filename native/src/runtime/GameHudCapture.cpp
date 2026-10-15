#include "GameHudCapture.h"
#include "GameQueues.h"
#include <algorithm>
#include <sstream>
namespace rdrvr::runtime {
namespace {
bool rgba(VkFormat f){return f==VK_FORMAT_R8G8B8A8_UNORM||f==VK_FORMAT_B8G8R8A8_UNORM||f==VK_FORMAT_R8G8B8A8_SRGB||f==VK_FORMAT_B8G8R8A8_SRGB;}
void transition(VkCommandBuffer cmd,VkImage image,VkImageLayout before,VkImageLayout after,VkAccessFlags src,VkAccessFlags dst){
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=before;b.newLayout=after;
    b.srcAccessMask=src;b.dstAccessMask=dst;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
constexpr auto usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
}
void GameHudCapture::bind(const XrGraphicsBindingVulkan2KHR& b){
    std::lock_guard guard(mutex_);
    // The published game queues (Appended: the same handles as the loop below; split: the protected-capable
    // queues, binding index 0). The loop below serves an unpublished list.
    std::array<VkQueue,GameQueues::capacity> listed{};
    const unsigned published=gameQueueList(listed.data(),static_cast<unsigned>(listed.size()));
    if(device_||!b.device||!b.physicalDevice||(!b.queueIndex&&!published))throw std::runtime_error("HUD capture requires reserved queue");
    device_=b.device;physical_=b.physicalDevice;family_=b.queueFamilyIndex;
    if(published)queues_.assign(listed.begin(),listed.begin()+published);
    else for(std::uint32_t i=0;i<b.queueIndex;++i){VkQueue q{};vkGetDeviceQueue(device_,family_,i,&q);queues_.push_back(q);}
    accepting_=true;
}
bool GameHudCapture::beginFrame(VkQueue queue,const FrameContext& frame,Extent extent,VkFormat format,VkCommandBuffer pass){
    std::lock_guard guard(mutex_);const auto reject=[&]{++rejected_;return false;};
    if(!accepting_||poisoned_||active_||!pass||frame.mode!=RenderMode::Stereo||frame.renderFrameId<=lastFrame_||
       !frame.xrFrameId||frame.predictedDisplayTime<=0||!frame.epochs.tracking||!frame.epochs.referenceSpace||!frame.epochs.resources||
       !extent.width||!extent.height||extent.width>8192||extent.height>8192||!rgba(format)||
       std::find(queues_.begin(),queues_.end(),queue)==queues_.end())return reject();
    if(!image_.image||extent!=extent_||format!=format_){
        auto next=createOwnedImage(physical_,device_,extent,format,usage,VK_IMAGE_ASPECT_COLOR_BIT,false);
        destroyOwnedImage(device_,image_);image_=next;extent_=extent;format_=format;initialized_=false;++generation_;
    }
    frame_=frame;queue_=queue;owner_=std::this_thread::get_id();pass_=pass;copy_={};
    recorded_=submitted_=passSubmitted_=passDiscarded_=cancelled_=false;passCompletion_={};copyCompletion_={};
    active_=true;lastFrame_=frame.renderFrameId;++prepared_;return true;
}
bool GameHudCapture::record(VkCommandBuffer command,const PassImage& s){
    std::lock_guard guard(mutex_);const auto reject=[&]{++rejected_;return false;};
    if(!active_||poisoned_||cancelled_||recorded_||owner_!=std::this_thread::get_id()||command!=pass_||!s.image||s.image==image_.image||
       s.extent!=extent_||s.format!=format_||s.family!=family_||s.samples!=VK_SAMPLE_COUNT_1_BIT||s.arrayLayer||s.arrayLayers!=1||
       !(s.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)||(s.layout!=VK_IMAGE_LAYOUT_GENERAL&&s.layout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL&&
       s.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL&&s.layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))return reject();
    transition(command,s.image,s.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    transition(command,image_.image,initialized_?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy c{};c.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.dstSubresource=c.srcSubresource;c.extent={extent_.width,extent_.height,1};
    vkCmdCopyImage(command,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image_.image,VK_IMAGE_LAYOUT_GENERAL,1,&c);
    transition(command,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
    recorded_=true;copy_=command;return true;
}
void GameHudCapture::cancelFrame(std::uint64_t frame) noexcept {
    std::lock_guard guard(mutex_);if(active_&&frame_.renderFrameId==frame&&owner_==std::this_thread::get_id())cancelled_=true;
}
void GameHudCapture::commandInvalidated(VkCommandBuffer command) noexcept {
    std::lock_guard guard(mutex_);if(!active_||!command)return;
    if(command==pass_){cancelled_=true;if(!passSubmitted_)passDiscarded_=true;}
    if(recorded_&&command==copy_){cancelled_=true;if(!submitted_){recorded_=false;copy_={};}}
}
void GameHudCapture::submitted(VkQueue queue,std::uint32_t count,const VkSubmitInfo* submits,VkResult result) noexcept {
    std::lock_guard guard(mutex_);if(!active_||std::find(queues_.begin(),queues_.end(),queue)==queues_.end()||!count||!submits)return;
    if(result!=VK_SUCCESS){poisoned_=true;return;}
    if(count>256)return;
    for(std::uint32_t j=0;j<count;++j){
        const auto& s=submits[j];if(s.commandBufferCount>4096||(s.commandBufferCount&&!s.pCommandBuffers))return;
        for(std::uint32_t k=0;k<s.commandBufferCount;++k){
            if(!passDiscarded_&&!passSubmitted_&&s.pCommandBuffers[k]==pass_){passSubmitted_=true;passCompletion_.observe(queue,s);}
            if(recorded_&&!submitted_&&s.pCommandBuffers[k]==copy_){submitted_=true;copyCompletion_.observe(queue,s);}
        }
    }
}
bool GameHudCapture::pendingOverlay(VkQueue queue) const noexcept {
    std::lock_guard guard(mutex_);return active_&&!poisoned_&&!cancelled_&&recorded_&&submitted_&&passSubmitted_&&queue==queue_;
}
bool GameHudCapture::requiresPresent() const noexcept {std::lock_guard guard(mutex_);return !accepting_||poisoned_||active_;}
bool GameHudCapture::covered(VkQueue queue,VkFence fence,std::span<const VkSemaphore> waits) const {
    return active_&&!poisoned_&&queue==queue_&&fence&&submitted_==recorded_&&(passSubmitted_||passDiscarded_)&&
        (!passSubmitted_||passCompletion_.covered(queue,waits))&&(!submitted_||copyCompletion_.covered(queue,waits));
}
bool GameHudCapture::completed(VkQueue queue,VkFence fence,std::span<const VkSemaphore> waits) const {
    return covered(queue,fence,waits)&&vkGetFenceStatus(device_,fence)==VK_SUCCESS;
}
CompletedQuadImage GameHudCapture::source(VkFence fence) const {
    return {device_,image_.image,{extent_.width,extent_.height},format_,VK_IMAGE_LAYOUT_GENERAL,usage,VK_SAMPLE_COUNT_1_BIT,1,1,family_,fence,
        frame_.renderFrameId,generation_,frame_.epochs.tracking,frame_.epochs.referenceSpace};
}
bool GameHudCapture::overlaySource(VkQueue queue,VkFence fence,CompletedQuadImage& out,std::span<const VkSemaphore> waits) const noexcept {
    std::lock_guard guard(mutex_);if(!completed(queue,fence,waits)||cancelled_||!recorded_)return false;out=source(fence);return true;
}
bool GameHudCapture::completePresent(VkQueue queue,VkFence fence,GameTheater& quad,std::uint64_t nowNs,std::span<const VkSemaphore> waits){
    std::lock_guard guard(mutex_);
    // The theater does not CPU-wait its present batch. Only an
    // active, fully covered capture waits for it here (overlay frames already
    // arrive with a CPU-proven fence, so in practice: cancelled captures).
    if(covered(queue,fence,waits)&&vkGetFenceStatus(device_,fence)==VK_NOT_READY){
        ++fenceWaits_;const auto waited=vkWaitForFences(device_,1,&fence,VK_TRUE,fenceWaitNs);
        if(waited!=VK_SUCCESS&&waited!=VK_TIMEOUT){poisoned_=true;return false;}
    }
    if(!completed(queue,fence,waits))return false;
    if(submitted_)initialized_=true;
    bool published=false;
    if(!cancelled_&&recorded_){
        published=quad.captureCompleted(queue,source(fence),nowNs);
        // A failed wait may leave the quad transfer reading this image. Do not
        // retire/reuse it merely because captureCompleted returned false.
        if(!quad.completedSourceRetired()){poisoned_=true;return false;}
    }
    active_=false;retiredFrame_=frame_.renderFrameId;recorded_=false;
    if(published)++published_;else ++discarded_;return published;
}
std::uint64_t GameHudCapture::retiredFrame() const noexcept {std::lock_guard guard(mutex_);return retiredFrame_;}
void GameHudCapture::shutdownRetired(){
    std::lock_guard guard(mutex_);accepting_=false;if(!device_)return;
    const auto result=vkDeviceWaitIdle(device_);if(result!=VK_SUCCESS&&result!=VK_ERROR_DEVICE_LOST)throw std::runtime_error("HUD capture still in flight");
    destroyOwnedImage(device_,image_);active_=false;queues_.clear();device_={};physical_={};
}
std::string GameHudCapture::report() const {
    std::lock_guard guard(mutex_);std::ostringstream out;
    out<<"{\"preparedFrames\":"<<prepared_<<",\"publishedFrames\":"<<published_<<",\"discardedFrames\":"<<discarded_
       <<",\"rejectedCalls\":"<<rejected_<<",\"fenceWaits\":"<<fenceWaits_<<",\"generation\":"<<generation_<<",\"retiredFrame\":"<<retiredFrame_
       <<",\"awaitingPresent\":"<<(active_?"true":"false")<<",\"recorded\":"<<(recorded_?"true":"false")
       <<",\"submitted\":"<<(submitted_?"true":"false")<<",\"cancelled\":"<<(cancelled_?"true":"false")<<",\"poisoned\":"<<(poisoned_?"true":"false")<<'}';return out.str();
}
}
