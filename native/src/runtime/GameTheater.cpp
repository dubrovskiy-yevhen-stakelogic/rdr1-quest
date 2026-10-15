#include "diagnostics/PerformanceProfile.h"
#include "GameTheater.h"
#include "GameQueues.h"
#include <algorithm>
#include <sstream>
namespace rdrvr::engine::px2d {
// PX 2D (TraceEntry.cpp, debug.rdrvr.px2d): every outer game present, kind 0 skipped, 1 copied to the theater, 2
// WORLD_PRESENT_UNCHANGED, 3 WORLD_STEREO_RETIRED. The lock-free px2d gate of the next RenderFrame reads it (no StereoMailbox mutex on
// the render thread); a copy right after a dropped 2D tail counts graphics.px2d.theaterCopies. Weak: absent = no-op.
__attribute__((weak)) void notePresent(unsigned kind,bool hudOverlay) noexcept;
}
namespace rdrvr::runtime {
namespace {
VkFormat encodedFormat(VkFormat f) {return (f==VK_FORMAT_B8G8R8A8_UNORM || f==VK_FORMAT_B8G8R8A8_SRGB)?VK_FORMAT_B8G8R8A8_SRGB:VK_FORMAT_R8G8B8A8_SRGB;}
bool rgba(VkFormat f) {return f==VK_FORMAT_R8G8B8A8_UNORM || f==VK_FORMAT_B8G8R8A8_UNORM || f==VK_FORMAT_R8G8B8A8_SRGB || f==VK_FORMAT_B8G8R8A8_SRGB;}
bool srgb(VkFormat f) {return f==VK_FORMAT_R8G8B8A8_SRGB || f==VK_FORMAT_B8G8R8A8_SRGB;}
void barrier(VkCommandBuffer cmd,VkImage image,VkImageLayout before,VkImageLayout after,VkAccessFlags src,VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;
    b.oldLayout=before;b.newLayout=after;b.srcAccessMask=src;b.dstAccessMask=dst;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
// Teardown only: a failed wait never cancels submitted work, so anything but
// completion or device loss quarantines the handles (caller throws upward).
void waitTeardown(VkDevice device,VkFence fence) {
    const auto result=vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX);
    if(result!=VK_SUCCESS && result!=VK_ERROR_DEVICE_LOST)throw std::runtime_error("theater GPU objects still in flight");
}
}
void GameTheater::route(PFN_vkCreateSwapchainKHR c,PFN_vkDestroySwapchainKHR d,PFN_vkQueuePresentKHR p) {create_=c;destroy_=d;present_=p;}
void GameTheater::onCompletedPresent(void (*callback)(VkQueue,VkFence,std::span<const VkSemaphore>) noexcept) {std::lock_guard guard(lock_);completed_=callback;}
void GameTheater::onWorldPresentBypass(bool (*qualified)(VkQueue) noexcept) {std::lock_guard guard(lock_);worldBypass_=qualified;}
void GameTheater::onWorldPresentRetire(bool (*qualified)(VkQueue) noexcept) {std::lock_guard guard(lock_);worldRetire_=qualified;}
void GameTheater::onPresentOverlay(bool (*pending)(VkQueue) noexcept,bool (*source)(VkQueue,VkFence,CompletedQuadImage&,std::span<const VkSemaphore>) noexcept) {
    std::lock_guard guard(lock_);overlayPending_=pending;overlaySource_=source;
    overlayStatus_=pending&&source?"WAITING_FOR_HUD":"NOT_CONFIGURED";
}
bool GameTheater::overlaySupported(VkDevice device,VkExtent2D size,VkFormat format) {
    std::lock_guard guard(lock_);
    return mode_==QuadSourceMode::GamePresent && accepting_ && !poisoned_ && device==device_ &&
        overlayPending_ && overlaySource_ && completed_ && !gameImages_.empty() &&
        (gameUsage_&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && size.width==extent_.width && size.height==extent_.height &&
        rgba(format) && srgb(format)==srgb(format_);
}
bool GameTheater::describePresentImage(VkDevice device,VkImage image,PresentImageInfo& out) {
    std::lock_guard guard(lock_);
    if(mode_!=QuadSourceMode::GamePresent||!accepting_||poisoned_||device!=device_||!image||!gameGeneration_||
       std::find(gameImages_.begin(),gameImages_.end(),image)==gameImages_.end())return false;
    out={device,image,extent_,format_,gameUsage_,gameGeneration_};return true;
}
bool GameTheater::prepareOverlay(const CompletedQuadImage& s,VkFence fence) {
    const auto reject=[&]{++overlayRejected_;overlayStatus_="HUD_SOURCE_REJECTED";return false;};
    if(!(gameUsage_&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || s.device!=device_ || !s.image || !s.lifetime || !s.frameId ||
       !fence || s.completed!=fence || vkGetFenceStatus(device_,s.completed)!=VK_SUCCESS || s.queueFamily!=family_ ||
       s.extent.width!=extent_.width || s.extent.height!=extent_.height || s.samples!=VK_SAMPLE_COUNT_1_BIT || s.layers!=1 || s.mips!=1 ||
       !(s.usage&VK_IMAGE_USAGE_SAMPLED_BIT) || !rgba(s.format) || srgb(s.format)!=srgb(format_) ||
       std::find(gameImages_.begin(),gameImages_.end(),s.image)!=gameImages_.end() ||
       (s.layout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && s.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        s.layout!=VK_IMAGE_LAYOUT_GENERAL && s.layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))return reject();
    if(overlayImage_!=s.image || overlayFormat_!=s.format || overlayLifetime_!=s.lifetime) {
        overlay_.destroy();overlayImage_={};overlayLifetime_=0;
        std::vector<XrSwapchainImageVulkan2KHR> images;
        for(auto image:gameImages_){XrSwapchainImageVulkan2KHR target{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};target.image=image;images.push_back(target);}
        overlay_.createOverlay(device_,s.image,{extent_.width,extent_.height},s.format,format_,images);
        overlayImage_=s.image;overlayFormat_=s.format;overlayLifetime_=s.lifetime;
    }
    return true;
}
void GameTheater::bind(const XrGraphicsBindingVulkan2KHR& b) {
    std::lock_guard guard(lock_);
    // The published game queues (Appended: the same handles as the loop below; split: the protected-capable
    // queues, binding index 0). The loop below covers an unpublished list.
    std::array<VkQueue,GameQueues::capacity> listed{};
    const unsigned published=gameQueueList(listed.data(),static_cast<unsigned>(listed.size()));
    if(device_ || !b.device || !b.physicalDevice || (!b.queueIndex && !published))throw std::runtime_error("theater binding requires reserved queue");
    accepting_=true;device_=b.device;physical_=b.physicalDevice;family_=b.queueFamilyIndex;
    if(published)gameQueues_.assign(listed.begin(),listed.begin()+published);
    else for(std::uint32_t i=0;i<b.queueIndex;++i){VkQueue q{};vkGetDeviceQueue(device_,family_,i,&q);gameQueues_.push_back(q);}
    vkGetDeviceQueue(device_,family_,b.queueIndex,&consumerQueue_);
}
VkResult GameTheater::create(VkDevice device,const VkSwapchainCreateInfoKHR* info,const VkAllocationCallbacks* alloc,VkSwapchainKHR* out) {
    std::lock_guard guard(lock_);
    if(!accepting_ || device!=device_ || !info || !out)return create_(device,info,alloc,out);
    VkSurfaceCapabilitiesKHR caps{};VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical_,encodedFormat(info->imageFormat),&props);
    bool eligible=!info->flags && !info->pNext && info->imageArrayLayers==1 && rgba(info->imageFormat) &&
        info->imageExtent.width && info->imageExtent.height && info->imageExtent.width<=8192 && info->imageExtent.height<=8192 &&
        info->preTransform==VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR && info->imageColorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
        (props.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_,info->surface,&caps)==VK_SUCCESS &&
        (caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if(info->imageSharingMode==VK_SHARING_MODE_CONCURRENT) {
        eligible=eligible && info->pQueueFamilyIndices && std::find(info->pQueueFamilyIndices,
            info->pQueueFamilyIndices+info->queueFamilyIndexCount,family_)!=info->pQueueFamilyIndices+info->queueFamilyIndexCount;
    }
    auto copy=*info;if(eligible)copy.imageUsage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto result=create_(device,&copy,alloc,out);
    if(result!=VK_SUCCESS)return result;
    // Device-owned metadata/resources are optional; never change a successful
    // stock create into a failure if diagnostic allocation runs out of memory.
    try {
        discardMailbox();
        // Presents of the previous swapchain may still wait its chain
        // semaphores; they are destroyed with that swapchain, never here.
        if(!chain_.semaphores.empty())retiredChains_.push_back(std::move(chain_));
        chain_={};chain_.swapchain=*out;
        gameSwapchain_=*out;gameImages_.clear();extent_=info->imageExtent;format_=info->imageFormat;gameUsage_=copy.imageUsage;
        if(gameGeneration_==UINT64_MAX)throw std::runtime_error("game swapchain lifetime exhausted");
        ++gameGeneration_;
        status_=eligible?"WAITING_FOR_PRESENT":"SWAPCHAIN_UNSUPPORTED";
        if(eligible){std::uint32_t n{};checkVk(vkGetSwapchainImagesKHR(device,*out,&n,nullptr),"game image count");
            if(!n || n>16)throw std::runtime_error("game image count out of bounds");
            gameImages_.resize(n);checkVk(vkGetSwapchainImagesKHR(device,*out,&n,gameImages_.data()),"game images");gameImages_.resize(n);}
    }catch(const std::exception& e){gameImages_.clear();status_="CAPTURE_SETUP_FAILED";__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","theater setup: %s",e.what());}
    return result;
}
void GameTheater::begin(VkCommandBuffer cmd) {
    checkVk(vkResetCommandBuffer(cmd,0),"theater reset command");
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(cmd,&b),"theater begin command");
}
void GameTheater::submit(VkQueue q,std::uint32_t count,const VkSubmitInfo* batches,VkFence fence) {
    diagnostics::WallScope submitTiming(diagnostics::frameTimings.theaterQueueSubmit);
    checkVk(vkQueueSubmit(q,count,batches,fence),"theater submit");
}
// Blocking wait kept only where a contract needs a CPU-proven completion
// (HUD overlay source, CompletedImage quad) or the ring is exhausted.
void GameTheater::waitFence(VkFence fence,std::uint64_t& counter) {
    ++counter;
    diagnostics::WallScope waitTiming(diagnostics::frameTimings.theaterFenceWait);
    checkVk(vkWaitForFences(device_,1,&fence,VK_TRUE,UINT64_MAX),"theater GPU complete");
}
bool GameTheater::observeSlot(unsigned i) {
    auto& s=slots_[i];if(!s.submitted)return true;
    const auto result=vkGetFenceStatus(device_,s.fence);if(result==VK_NOT_READY)return false;
    checkVk(result,"theater copy fence");s.submitted=false;return true;
}
bool GameTheater::observeRelay(unsigned i) {
    auto& r=relays_[i];if(!r.submitted)return true;
    const auto result=vkGetFenceStatus(device_,r.fence);if(result==VK_NOT_READY)return false;
    checkVk(result,"theater relay fence");r.submitted=false;return true;
}
bool GameTheater::observeConsumer(unsigned i) {
    auto& c=consumers_[i];if(!c.submitted)return true;
    const auto result=vkGetFenceStatus(device_,c.fence);if(result==VK_NOT_READY)return false;
    checkVk(result,"theater transfer fence");c.submitted=false;return true;
}
unsigned GameTheater::framesInFlight() const {unsigned n=0;for(const auto& s:slots_)n+=s.submitted?1u:0u;return n;}
int GameTheater::acquireSlot() {
    // Never the displayed slot; oldest finished copy first (sequence 0 = unused).
    int pick=-1,oldest=-1;
    for(unsigned i=0;i<slotCount;++i){
        if(int(i)==displayedSlot_)continue;
        if(oldest<0 || slots_[i].sequence<slots_[oldest].sequence)oldest=int(i);
        if(!observeSlot(i))continue;
        if(pick<0 || slots_[i].sequence<slots_[pick].sequence)pick=int(i);
    }
    if(pick>=0 || oldest<0)return pick;
    ++slotWaits_;
    diagnostics::WallScope waitTiming(diagnostics::frameTimings.theaterFenceWait);
    const auto result=vkWaitForFences(device_,1,&slots_[oldest].fence,VK_TRUE,slotWaitNs);
    waitTiming.finish();
    if(result==VK_TIMEOUT){++slotTimeouts_;return -1;}
    checkVk(result,"theater slot wait");
    return observeSlot(unsigned(oldest))?oldest:-1;
}
unsigned GameTheater::acquireRelay() {
    // Relay fences are handed to completed_; one is reset only after it has
    // signalled (>= relayCount presents later), so callers never see a reset fence.
    const unsigned r=nextRelay_;nextRelay_=(nextRelay_+1)%relayCount;
    if(!observeRelay(r)){waitFence(relays_[r].fence,relayWaits_);observeRelay(r);}
    return r;
}
int GameTheater::acquireConsumer() {
    for(unsigned i=0;i<consumerCount;++i)if(observeConsumer(i))return int(i);
    return -1;
}
// One binary semaphore per game swapchain image index. Re-signalling chain[i]
// is GPU-ordered after the previous present of image i consumed it: the theater's batch
// waits this present's semaphores, which the game signalled after rendering
// image i, which waited its re-acquire, which follows the presentation engine
// finishing the previous present of i (including its wait on chain[i]).
VkSemaphore GameTheater::chainSemaphore(std::uint32_t index,bool& drain) {
    if(chain_.swapchain!=gameSwapchain_ || index>=gameImages_.size())throw std::runtime_error("theater chain swapchain mismatch");
    if(chain_.semaphores.size()<gameImages_.size()){chain_.semaphores.resize(gameImages_.size(),VK_NULL_HANDLE);chain_.unconsumed.resize(gameImages_.size(),0);}
    auto& semaphore=chain_.semaphores[index];
    if(!semaphore){VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};checkVk(vkCreateSemaphore(device_,&s,nullptr,&semaphore),"theater chain semaphore");}
    // A present that failed before enqueueing (OOM) left it signalled.
    drain=chain_.unconsumed[index]!=0;
    return semaphore;
}
void GameTheater::destroyChain(Chain& chain) noexcept {
    if(device_)for(auto semaphore:chain.semaphores)if(semaphore)vkDestroySemaphore(device_,semaphore,nullptr);
    chain=Chain{};
}
void GameTheater::prepareRelays() {
    if(relays_[0].fence)return;
    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for(auto& r:relays_)checkVk(vkCreateFence(device_,&f,nullptr,&r.fence),"theater relay fence");
}
void GameTheater::prepare() {
    prepareRelays();
    if(pool_)return;
    VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};p.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;p.queueFamilyIndex=family_;
    checkVk(vkCreateCommandPool(device_,&p,nullptr,&pool_),"theater pool");
    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=pool_;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;
    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};im.imageType=VK_IMAGE_TYPE_2D;im.format=mode_==QuadSourceMode::CompletedImage?format_:encodedFormat(format_);im.extent={extent_.width,extent_.height,1};
    im.mipLevels=im.arrayLayers=1;im.samples=VK_SAMPLE_COUNT_1_BIT;im.tiling=VK_IMAGE_TILING_OPTIMAL;im.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkPhysicalDeviceMemoryProperties m{};vkGetPhysicalDeviceMemoryProperties(physical_,&m);
    for(auto& slot:slots_){
        checkVk(vkAllocateCommandBuffers(device_,&a,&slot.command),"theater producer");
        checkVk(vkCreateFence(device_,&f,nullptr,&slot.fence),"theater fence");
        checkVk(vkCreateSemaphore(device_,&s,nullptr,&slot.ready),"theater ready");
        checkVk(vkCreateSemaphore(device_,&s,nullptr,&slot.done),"theater done");
        checkVk(vkCreateImage(device_,&im,nullptr,&slot.image),"theater mailbox");
        VkMemoryRequirements r{};vkGetImageMemoryRequirements(device_,slot.image,&r);
        std::uint32_t type=m.memoryTypeCount;for(std::uint32_t i=0;i<m.memoryTypeCount;++i)if((r.memoryTypeBits&(1u<<i)) && (m.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
        if(type==m.memoryTypeCount)throw std::runtime_error("theater device-local memory missing");
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=r.size;alloc.memoryTypeIndex=type;
        checkVk(vkAllocateMemory(device_,&alloc,nullptr,&slot.memory),"theater memory");checkVk(vkBindImageMemory(device_,slot.image,slot.memory,0),"theater bind");
    }
    for(auto& c:consumers_){
        checkVk(vkAllocateCommandBuffers(device_,&a,&c.command),"theater consumer");
        checkVk(vkCreateFence(device_,&f,nullptr,&c.fence),"theater consumer fence");
    }
}
VkResult GameTheater::present(VkQueue q,const VkPresentInfoKHR* info,PFN_vkQueuePresentKHR next) {
    if(!next)next=present_;
    // Swappy can resolve the direct Vulkan hook too. Capture only at its outer
    // call, then let nested presentation consume the already-adjusted waits.
    static thread_local bool presenting=false;
    if(presenting)return next(q,info);
    struct Scope {bool& flag;Scope(bool& f):flag(f){flag=true;}~Scope(){flag=false;}} scope(presenting);
    diagnostics::WallScope timing(diagnostics::frameTimings.theaterPresent);
    bool consumed=false;VkSemaphore chain{};std::uint32_t chainIndex{};VkSwapchainKHR chainOwner{};
    // PX 2D: this outer present's kind (0 skipped / failed, 1 copied, 2 bypassed, 3 retired), published when present() returns.
    struct Px2dNote {unsigned kind{};bool hud{};~Px2dNote(){if(::rdrvr::engine::px2d::notePresent)::rdrvr::engine::px2d::notePresent(kind,hud);}} px2dNote;
    {
        diagnostics::WallScope lockTiming(diagnostics::frameTimings.theaterLockWait);
        std::lock_guard guard(lock_);lockTiming.finish();++presents_;
        diagnostics::presentHistory.push(presents_,"PRESENT",0,extent_.width,extent_.height);
        if(!accepting_ || poisoned_ || !info || info->pNext || info->swapchainCount!=1 || !info->pSwapchains || !info->pImageIndices ||
           info->pSwapchains[0]!=gameSwapchain_ || info->pImageIndices[0]>=gameImages_.size() ||
           std::find(gameQueues_.begin(),gameQueues_.end(),q)==gameQueues_.end() ||
           !info->waitSemaphoreCount || info->waitSemaphoreCount>16 || !info->pWaitSemaphores){++skipped_;}
        else try {
            // Before touching any binary semaphore or bridge image, preserve
            // the exact native present when stereo has its own completion path.
            // Retain ready/done signals: the next real capture/consumer owns
            // their normal drain. A skipped theater image is explicitly stale.
            const bool pendingOverlay=overlayPending_&&overlayPending_(q);
            const std::span<const VkSemaphore> stock{info->pWaitSemaphores,info->waitSemaphoreCount};
            // waitSemaphoreCount <= 16 (checked above) + done + one drain.
            std::array<VkPipelineStageFlags,18> stages{};stages.fill(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            chainIndex=info->pImageIndices[0];chainOwner=gameSwapchain_;
            if(mode_==QuadSourceMode::GamePresent&&worldBypass_&&!pendingOverlay&&worldBypass_(q)){
                ++worldBypassed_;worldStale_=true;staleSequence_=sequence_;status_="WORLD_PRESENT_UNCHANGED";px2dNote.kind=2;
            }else if(mode_==QuadSourceMode::GamePresent&&worldRetire_&&completed_&&!pendingOverlay&&worldRetire_(q)){
                // The draw queue can already be recording the next pair. Keep
                // this present's exact semaphore dependency to retire the old
                // pair, without copying an unused 2D screen. Old theater ready
                // and done signals belong to its next real capture, not here.
                // Relay {wait stock; signal chain} with a ring fence,
                // no CPU wait; the native present waits the chain semaphore.
                prepareRelays();
                bool drain=false;chain=chainSemaphore(chainIndex,drain);
                const auto r=acquireRelay();
                std::array<VkSubmitInfo,2> batches{{{VK_STRUCTURE_TYPE_SUBMIT_INFO},{VK_STRUCTURE_TYPE_SUBMIT_INFO}}};std::uint32_t count=0;
                if(drain){auto& d=batches[count++];d.waitSemaphoreCount=1;d.pWaitSemaphores=&chain;d.pWaitDstStageMask=stages.data();++chainDrains_;}
                auto& x=batches[count++];x.waitSemaphoreCount=info->waitSemaphoreCount;x.pWaitSemaphores=info->pWaitSemaphores;x.pWaitDstStageMask=stages.data();
                x.signalSemaphoreCount=1;x.pSignalSemaphores=&chain;
                checkVk(vkResetFences(device_,1,&relays_[r].fence),"theater relay reset");
                consumed=true;submit(q,count,batches.data(),relays_[r].fence);relays_[r].submitted=true;chain_.unconsumed[chainIndex]=1;
                ++worldRetired_;worldStale_=true;staleSequence_=sequence_;status_="WORLD_STEREO_RETIRED";px2dNote.kind=3;
                completed_(q,relays_[r].fence,stock);
            }else{
            if(worldBypass_){++worldDenied_;if(pendingOverlay)++worldHudDenied_;}
            px2dNote.kind=1;px2dNote.hud=pendingOverlay; // PX 2D: copied to the theater (not bypassed)
            prepare();
            std::vector<VkSemaphore> waits(stock.begin(),stock.end());
            CompletedQuadImage hud{};bool overlay=false;VkFence proven{};
            if(pendingOverlay && overlaySource_ && completed_) {
                // The producer may only expose the HUD after its real submit
                // has retired. Consume stock waits exactly once before asking:
                // this HUD-only path keeps its CPU-proven (blocking) contract.
                const auto r=acquireRelay();
                VkSubmitInfo x{VK_STRUCTURE_TYPE_SUBMIT_INFO};x.waitSemaphoreCount=static_cast<std::uint32_t>(waits.size());x.pWaitSemaphores=waits.data();x.pWaitDstStageMask=stages.data();
                checkVk(vkResetFences(device_,1,&relays_[r].fence),"theater relay reset");
                consumed=true;submit(q,1,&x,relays_[r].fence);relays_[r].submitted=true;
                waitFence(relays_[r].fence,overlayWaits_);observeRelay(r);
                waits.clear();proven=relays_[r].fence;
                if(overlaySource_(q,proven,hud,stock))overlay=prepareOverlay(hud,proven);
                else overlayStatus_="HUD_SOURCE_UNAVAILABLE";
            }
            const int m=acquireSlot();
            if(m<0){
                // Every free slot still copying after the bounded wait: no 2D
                // capture this present. Unconsumed stock waits go to the native
                // present unchanged; consumed ones were CPU-proven above.
                status_="THEATER_SLOT_TIMEOUT";
                if(consumed && completed_)completed_(q,proven,stock);
            }else{
            auto& s=slots_[unsigned(m)];
            begin(s.command);
            const auto source=gameImages_[info->pImageIndices[0]];
            if(overlay){
                barrier(s.command,hud.image,hud.layout,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
                barrier(s.command,source,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,0,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                overlay_.draw(s.command,info->pImageIndices[0]);
                barrier(s.command,hud.image,VK_IMAGE_LAYOUT_GENERAL,hud.layout,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
            }
            barrier(s.command,source,overlay?VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                overlay?VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT:0,VK_ACCESS_TRANSFER_READ_BIT);
            barrier(s.command,s.image,s.initialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
            VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.dstSubresource=region.srcSubresource;region.extent={extent_.width,extent_.height,1};
            vkCmdCopyImage(s.command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
            barrier(s.command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,VK_ACCESS_TRANSFER_READ_BIT,0);
            checkVk(vkEndCommandBuffer(s.command),"theater end producer");
            // [drain: ready the XR consumer skipped / chain an OOM present left]
            // {wait stock (+done of the slot's last read); copy; signal ready, chain}
            bool drainChain=false;chain=chainSemaphore(chainIndex,drainChain);
            std::array<VkSemaphore,2> drains{};std::uint32_t drainCount=0;
            if(s.readyPending){drains[drainCount++]=s.ready;++readyDrains_;}
            if(drainChain){drains[drainCount++]=chain;++chainDrains_;}
            if(s.donePending){waits.push_back(s.done);++doneWaits_;}
            const std::array<VkSemaphore,2> signals{s.ready,chain};
            std::array<VkSubmitInfo,2> batches{{{VK_STRUCTURE_TYPE_SUBMIT_INFO},{VK_STRUCTURE_TYPE_SUBMIT_INFO}}};std::uint32_t count=0;
            if(drainCount){auto& d=batches[count++];d.waitSemaphoreCount=drainCount;d.pWaitSemaphores=drains.data();d.pWaitDstStageMask=stages.data();}
            auto& x=batches[count++];x.waitSemaphoreCount=static_cast<std::uint32_t>(waits.size());x.pWaitSemaphores=waits.empty()?nullptr:waits.data();x.pWaitDstStageMask=stages.data();
            x.commandBufferCount=1;x.pCommandBuffers=&s.command;x.signalSemaphoreCount=2;x.pSignalSemaphores=signals.data();
            checkVk(vkResetFences(device_,1,&s.fence),"theater reset fence");
            // From this point failure must not retry stock waits: the queue may
            // already have consumed them. Propagate device loss instead.
            consumed=true;submit(q,count,batches.data(),s.fence);
            s.submitted=true;s.readyPending=true;s.donePending=false;s.initialized=true;s.sequence=++sequence_;chain_.unconsumed[chainIndex]=1;
            initialized_=true;worldStale_=false;++captured_;status_="GAME_FRAME_CAPTURED";
            framesInFlightMax_=std::max<std::uint64_t>(framesInFlightMax_,framesInFlight());
            if(overlay){
                // The HUD producer retires its image after completedPresent;
                // the overlay draw must have read it (blocking contract, HUD only).
                waitFence(s.fence,overlayWaits_);observeSlot(unsigned(m));
                ++overlaid_;overlayStatus_="HUD_COMPOSITED";
            }
            if(completed_)completed_(q,s.fence,stock);
            }
            }
        }catch(const std::exception& e){poisoned_=true;status_="CAPTURE_FAILED";px2dNote.kind=0;__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","theater capture: %s",e.what());if(consumed)return VK_ERROR_DEVICE_LOST;}
    }
    if(!consumed)return next(q,info);
    // The theater's last batch consumed every original wait and signals chain after it
    // (and after the copy that reads the image). Without a chain semaphore the
    // waits were CPU-proven by the HUD relay. Preserve pResults and indices.
    auto copy=*info;
    if(chain){copy.waitSemaphoreCount=1;copy.pWaitSemaphores=&chain;}
    else{copy.waitSemaphoreCount=0;copy.pWaitSemaphores=nullptr;}
    const auto result=next(q,&copy);
    // Only a failure to enqueue (OOM) leaves the wait unexecuted.
    if(chain && result!=VK_ERROR_OUT_OF_HOST_MEMORY && result!=VK_ERROR_OUT_OF_DEVICE_MEMORY){
        std::lock_guard guard(lock_);
        if(chain_.swapchain==chainOwner && chainIndex<chain_.unconsumed.size())chain_.unconsumed[chainIndex]=0;
    }
    return result;
}
bool GameTheater::render(XrSession session,VkQueue queue,XrSwapchain& screen,VkExtent2D& extent) {
    std::lock_guard guard(lock_);
    if(mode_!=QuadSourceMode::GamePresent||worldStale_)return false;
    return renderLocked(session,queue,screen,extent);
}
bool GameTheater::renderLocked(XrSession session,VkQueue queue,XrSwapchain& screen,VkExtent2D& extent) {
    try {
    if(!accepting_ || poisoned_ || !pool_ || !initialized_ || queue!=consumerQueue_)return false;
    if(screen_ && (screenExtent_.width!=extent_.width || screenExtent_.height!=extent_.height)){discardScreen();}
    if(!screen_) {
        std::uint32_t n{};checkXr(xrEnumerateSwapchainFormats(session,0,&n,nullptr),"theater format count");
        if(!n || n>256)throw std::runtime_error("theater format count invalid");
        std::vector<std::int64_t> formats(n);
        checkXr(xrEnumerateSwapchainFormats(session,n,&n,formats.data()),"theater formats");screenFormat_=VK_FORMAT_UNDEFINED;
        for(auto f:{VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM}) {
            if(mode_==QuadSourceMode::CompletedImage && f!=VK_FORMAT_R8G8B8A8_SRGB && f!=VK_FORMAT_B8G8R8A8_SRGB)continue;
            VkFormatProperties props{};vkGetPhysicalDeviceFormatProperties(physical_,f,&props);
            if(std::find(formats.begin(),formats.end(),f)!=formats.end() && (props.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT)){screenFormat_=f;break;}
        }
        if(screenFormat_==VK_FORMAT_UNDEFINED)throw std::runtime_error("theater blit format unavailable");
        XrSwapchainCreateInfo c{XR_TYPE_SWAPCHAIN_CREATE_INFO};c.usageFlags=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT|XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        c.format=screenFormat_;c.sampleCount=c.faceCount=c.arraySize=c.mipCount=1;c.width=extent_.width;c.height=extent_.height;
        checkXr(xrCreateSwapchain(session,&c,&screen_),"theater swapchain");screenExtent_=extent_;
        checkXr(xrEnumerateSwapchainImages(screen_,0,&n,nullptr),"theater image count");
        if(!n || n>16)throw std::runtime_error("theater image count invalid");
        screenImages_.resize(n,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        checkXr(xrEnumerateSwapchainImages(screen_,n,&n,reinterpret_cast<XrSwapchainImageBaseHeader*>(screenImages_.data())),"theater images");
    }
    // Newest copy newer than the displayed one that has finished; never wait.
    int pick=-1,newest=-1;
    for(unsigned i=0;i<slotCount;++i){
        const auto& s=slots_[i];if(!s.sequence || s.sequence<=displayedSequence_)continue;
        if(newest<0 || s.sequence>slots_[newest].sequence)newest=int(i);
        if(observeSlot(i) && (pick<0 || s.sequence>slots_[pick].sequence))pick=int(i);
    }
    if(newest>=0 && newest!=pick)++consumerNotReady_;
    if(pick>=0) {
        const int c=acquireConsumer();
        if(c<0)++consumerBusy_;
        else {
        auto& s=slots_[unsigned(pick)];auto& consumer=consumers_[unsigned(c)];
        std::uint32_t index{};XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};checkXr(xrAcquireSwapchainImage(screen_,&a,&index),"theater acquire");
        if(index>=screenImages_.size())throw std::runtime_error("theater image index");
        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=XR_INFINITE_DURATION;
        const auto waited=xrWaitSwapchainImage(screen_,&w);
        if(waited!=XR_SUCCESS && waited!=XR_SESSION_LOSS_PENDING)throw std::runtime_error("theater wait did not complete");
        begin(consumer.command);auto target=screenImages_[index].image;
        barrier(consumer.command,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit b{};b.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};b.dstSubresource=b.srcSubresource;
        b.srcOffsets[1]=b.dstOffsets[1]={static_cast<std::int32_t>(extent_.width),static_cast<std::int32_t>(extent_.height),1};
        vkCmdBlitImage(consumer.command,s.image,VK_IMAGE_LAYOUT_GENERAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&b,VK_FILTER_NEAREST);
        barrier(consumer.command,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        checkVk(vkEndCommandBuffer(consumer.command),"theater end consumer");
        // [drain done (defensive: a new copy has never been read)]
        // {wait ready; blit; signal done} - the next copy into the slot waits done.
        const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        std::array<VkSubmitInfo,2> batches{{{VK_STRUCTURE_TYPE_SUBMIT_INFO},{VK_STRUCTURE_TYPE_SUBMIT_INFO}}};std::uint32_t count=0;
        if(s.donePending){auto& d=batches[count++];d.waitSemaphoreCount=1;d.pWaitSemaphores=&s.done;d.pWaitDstStageMask=&stage;}
        auto& x=batches[count++];
        if(s.readyPending){x.waitSemaphoreCount=1;x.pWaitSemaphores=&s.ready;x.pWaitDstStageMask=&stage;}
        x.commandBufferCount=1;x.pCommandBuffers=&consumer.command;x.signalSemaphoreCount=1;x.pSignalSemaphores=&s.done;
        checkVk(vkResetFences(device_,1,&consumer.fence),"theater consumer fence reset");
        submit(queue,count,batches.data(),consumer.fence);consumer.submitted=true;s.readyPending=false;s.donePending=true;
        // The XR runtime orders its use after work already submitted to this
        // binding queue; no CPU wait before release.
        XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};checkXr(xrReleaseSwapchainImage(screen_,&r),"theater release");
        displayedSlot_=pick;displayedSequence_=s.sequence;
        screenValid_=true;++copied_;status_="XR_GAME_IMAGE_READY";
        }
    }
    // After world frames, show nothing until a post-world copy has
    // been consumed (the XR image still holds the pre-world 2D frame).
    screen=screen_;extent=screenExtent_;return screenValid_ && displayedSequence_>staleSequence_;
    }catch(...){poisoned_=true;status_="XR_COPY_FAILED";throw;}
}
bool GameTheater::captureCompleted(VkQueue queue,const CompletedQuadImage& source,std::uint64_t nowNs) {
    std::lock_guard guard(lock_);
    const auto reject=[&](const char* why){status_=why;++skipped_;return false;};
    if(mode_!=QuadSourceMode::CompletedImage || !accepting_ || poisoned_ || source.device!=device_ || !source.image ||
       !source.completed || !source.frameId || source.frameId<=externalFrame_ || !source.lifetime || !source.trackingEpoch || !source.spaceEpoch ||
       !nowNs || (externalNs_ && nowNs<externalNs_) || source.queueFamily!=family_ ||
       std::find(gameQueues_.begin(),gameQueues_.end(),queue)==gameQueues_.end() ||
       !source.extent.width || !source.extent.height || source.extent.width>8192 || source.extent.height>8192 ||
       source.samples!=VK_SAMPLE_COUNT_1_BIT || source.layers!=1 || source.mips!=1 || !(source.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
       !rgba(source.format) ||
       (source.layout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && source.layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
        source.layout!=VK_IMAGE_LAYOUT_GENERAL && source.layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))return reject("COMPLETED_SOURCE_REJECTED");
    if(vkGetFenceStatus(device_,source.completed)!=VK_SUCCESS)return reject("COMPLETED_SOURCE_NOT_RETIRED");
    try{
        VkFormatProperties props{};vkGetPhysicalDeviceFormatProperties(physical_,source.format,&props);
        if(!(props.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT))return reject("COMPLETED_SOURCE_FORMAT_UNSUPPORTED");
        if(extent_.width!=source.extent.width || extent_.height!=source.extent.height || format_!=source.format){
            discardMailbox();extent_=source.extent;format_=source.format;screenValid_=false;externalValid_=false;
        }
        prepare();
        const int m=acquireSlot();
        if(m<0)return reject("COMPLETED_SLOT_TIMEOUT");
        auto& s=slots_[unsigned(m)];
        begin(s.command);
        barrier(s.command,source.image,source.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        barrier(s.command,s.image,s.initialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.dstSubresource=region.srcSubresource;
        region.extent={extent_.width,extent_.height,1};
        vkCmdCopyImage(s.command,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
        barrier(s.command,source.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,source.layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
        checkVk(vkEndCommandBuffer(s.command),"quad end producer");
        const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        std::array<VkSubmitInfo,2> batches{{{VK_STRUCTURE_TYPE_SUBMIT_INFO},{VK_STRUCTURE_TYPE_SUBMIT_INFO}}};std::uint32_t count=0;
        if(s.readyPending){auto& d=batches[count++];d.waitSemaphoreCount=1;d.pWaitSemaphores=&s.ready;d.pWaitDstStageMask=&stage;++readyDrains_;}
        auto& x=batches[count++];
        if(s.donePending){x.waitSemaphoreCount=1;x.pWaitSemaphores=&s.done;x.pWaitDstStageMask=&stage;++doneWaits_;}
        x.commandBufferCount=1;x.pCommandBuffers=&s.command;x.signalSemaphoreCount=1;x.pSignalSemaphores=&s.ready;
        checkVk(vkResetFences(device_,1,&s.fence),"quad reset fence");
        inFlight_=true;submit(queue,count,batches.data(),s.fence);
        s.submitted=true;s.readyPending=true;s.donePending=false;s.initialized=true;s.sequence=++sequence_;initialized_=true;
        // The HUD producer retires its source when this returns: keep the
        // completedSourceRetired contract (this path blocks).
        waitFence(s.fence,completedWaits_);observeSlot(unsigned(m));inFlight_=false;
        externalValid_=true;externalFrame_=source.frameId;externalTracking_=source.trackingEpoch;externalSpace_=source.spaceEpoch;externalNs_=nowNs;
        ++captured_;status_="COMPLETED_QUAD_CAPTURED";return true;
    }catch(...){poisoned_=true;externalValid_=false;status_="COMPLETED_QUAD_CAPTURE_FAILED";return false;}
}
bool GameTheater::renderCompleted(XrSession session,VkQueue queue,std::uint64_t frame,std::uint64_t tracking,std::uint64_t space,std::uint64_t nowNs,XrSwapchain& screen,VkExtent2D& extent){
    std::lock_guard guard(lock_);
    // Re-sample under the lock (caller sampled before it), as in StereoMailbox::render.
    nowNs=std::max<std::uint64_t>(nowNs,diagnostics::monotonicNs());
    if(mode_!=QuadSourceMode::CompletedImage || !externalValid_)return false;
    if(tracking!=externalTracking_ || space!=externalSpace_ || (nowNs>externalNs_?nowNs-externalNs_:0)>completedMaxAgeNs){
        externalValid_=screenValid_=false;status_="COMPLETED_QUAD_STALE";return false;
    }
    if(!frame || frame!=externalFrame_){status_="WAITING_FOR_MATCHING_WORLD";return false;}
    return renderLocked(session,queue,screen,extent);
}
bool GameTheater::completedSourceRetired(){std::lock_guard guard(lock_);return !inFlight_;}
void GameTheater::invalidateCompleted(){std::lock_guard guard(lock_);if(mode_==QuadSourceMode::CompletedImage){externalValid_=screenValid_=false;status_="COMPLETED_QUAD_INVALIDATED";}}
void GameTheater::discardScreen() {
    // Consumer submissions write the XR images: they must finish first.
    for(auto& c:consumers_)if(c.submitted){waitTeardown(device_,c.fence);c.submitted=false;}
    if(screen_)xrDestroySwapchain(screen_);screen_=XR_NULL_HANDLE;screenImages_.clear();screenValid_=false;screenExtent_={};
}
void GameTheater::discardMailbox() {
    // A failed fence wait does not cancel submitted GPU work. Quarantine resources
    // if completion cannot be established, including during swapchain teardown.
    for(auto& s:slots_)if(s.submitted){waitTeardown(device_,s.fence);s.submitted=false;}
    for(auto& r:relays_)if(r.submitted){waitTeardown(device_,r.fence);r.submitted=false;}
    for(auto& c:consumers_)if(c.submitted){waitTeardown(device_,c.fence);c.submitted=false;}
    inFlight_=false;
    // Signaled but unreferenced binary semaphores may be destroyed once every
    // batch referencing them completed. Chain semaphores stay with their
    // swapchain. No game image/device is destroyed here.
    overlay_.destroy();overlayImage_={};overlayFormat_={};overlayLifetime_=0;
    for(auto& s:slots_){
        if(s.image)vkDestroyImage(device_,s.image,nullptr);
        if(s.memory)vkFreeMemory(device_,s.memory,nullptr);
        if(s.fence)vkDestroyFence(device_,s.fence,nullptr);
        if(s.ready)vkDestroySemaphore(device_,s.ready,nullptr);
        if(s.done)vkDestroySemaphore(device_,s.done,nullptr);
        s=Slot{};
    }
    for(auto& r:relays_){if(r.fence)vkDestroyFence(device_,r.fence,nullptr);r=Relay{};}
    for(auto& c:consumers_){if(c.fence)vkDestroyFence(device_,c.fence,nullptr);c=Consumer{};}
    nextRelay_=0;
    if(pool_)vkDestroyCommandPool(device_,pool_,nullptr);pool_=VK_NULL_HANDLE;
    initialized_=worldStale_=false;displayedSlot_=-1;
}
void GameTheater::destroy(VkDevice d,VkSwapchainKHR s,const VkAllocationCallbacks* a) {
    std::vector<Chain> doomed;bool quarantined=false;
    {std::lock_guard guard(lock_);if(d==device_ && s==gameSwapchain_){
        try {discardMailbox();status_="GAME_SWAPCHAIN_DESTROYED";}
        catch(const std::exception& e){quarantined=true;accepting_=false;poisoned_=true;status_="GPU_TEARDOWN_QUARANTINED";__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","theater teardown: %s",e.what());}
        gameImages_.clear();gameSwapchain_=VK_NULL_HANDLE;
    }
    if(d==device_ && s){
        if(chain_.swapchain==s){doomed.push_back(std::move(chain_));chain_=Chain{};}
        for(auto it=retiredChains_.begin();it!=retiredChains_.end();)if(it->swapchain==s){doomed.push_back(std::move(*it));it=retiredChains_.erase(it);}else ++it;
    }}
    destroy_(d,s,a);
    // The game completed every presentation of s before destroying it; the
    // theater's batches signalling these semaphores completed in discardMailbox (now or
    // when the replacement swapchain was created). Quarantine on doubt.
    if(!doomed.empty() && !quarantined){std::lock_guard guard(lock_);for(auto& c:doomed)destroyChain(c);}
}
void GameTheater::detachSession(){std::lock_guard guard(lock_);accepting_=false;discardMailbox();discardScreen();}
void GameTheater::shutdown(){
    std::lock_guard guard(lock_);accepting_=false;discardMailbox();discardScreen();
    destroyChain(chain_);for(auto& c:retiredChains_)destroyChain(c);retiredChains_.clear();
    gameImages_.clear();gameQueues_.clear();gameSwapchain_=VK_NULL_HANDLE;device_=VK_NULL_HANDLE;
}
std::string GameTheater::report(){std::lock_guard guard(lock_);std::ostringstream s;s<<"{\"status\":\""<<status_<<"\",\"presentCalls\":"<<presents_<<",\"capturedFrames\":"<<captured_<<",\"overlayStatus\":\""<<overlayStatus_<<"\",\"overlaidFrames\":"<<overlaid_<<",\"overlayRejected\":"<<overlayRejected_<<",\"copiedToXr\":"<<copied_<<",\"skippedPresents\":"<<skipped_<<",\"worldBypassed\":"<<worldBypassed_<<",\"worldRetired\":"<<worldRetired_<<",\"worldDenied\":"<<worldDenied_<<",\"worldHudDenied\":"<<worldHudDenied_<<",\"worldStale\":"<<(worldStale_?"true":"false")
    <<",\"ring\":{\"slots\":"<<slotCount<<",\"relays\":"<<relayCount<<",\"consumers\":"<<consumerCount<<",\"sequence\":"<<sequence_<<",\"displayedSequence\":"<<displayedSequence_<<",\"staleSequence\":"<<staleSequence_
    <<",\"slotWaits\":"<<slotWaits_<<",\"slotTimeouts\":"<<slotTimeouts_<<",\"relayWaits\":"<<relayWaits_<<",\"overlayWaits\":"<<overlayWaits_<<",\"completedWaits\":"<<completedWaits_
    <<",\"framesInFlight\":"<<framesInFlight()<<",\"framesInFlightMax\":"<<framesInFlightMax_<<",\"readyDrains\":"<<readyDrains_<<",\"doneWaits\":"<<doneWaits_<<",\"chainDrains\":"<<chainDrains_
    <<",\"chainSemaphores\":"<<chain_.semaphores.size()<<",\"retiredChains\":"<<retiredChains_.size()<<",\"consumerWaits\":0,\"consumerNotReady\":"<<consumerNotReady_<<",\"consumerBusy\":"<<consumerBusy_<<'}'
    <<",\"width\":"<<extent_.width<<",\"height\":"<<extent_.height<<",\"sourceFormat\":"<<format_<<",\"poisoned\":"<<(poisoned_?"true":"false")<<'}';return s.str();}
}
