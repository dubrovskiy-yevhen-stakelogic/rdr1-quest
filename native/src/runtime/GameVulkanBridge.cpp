#include "GameVulkanBridge.h"
#include "GameQueues.h"
#include "GraphicsSettings.h"
#include <cstring>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
namespace rdrvr::runtime {
bool GameVulkanBridge::isGameInstance(const VkInstanceCreateInfo* r) {
    if(!r || r->sType!=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO || !r->pApplicationInfo) return false;
    const auto& a=*r->pApplicationInfo;
    if(a.sType!=VK_STRUCTURE_TYPE_APPLICATION_INFO || a.apiVersion!=VK_API_VERSION_1_1 ||
       !a.pApplicationName || !a.pEngineName || std::strcmp(a.pApplicationName,"redemption") ||
       std::strcmp(a.pEngineName,"redemption") || r->enabledExtensionCount>128 ||
       !r->ppEnabledExtensionNames) return false;
    bool surface=false,android=false;
    for(unsigned i=0;i<r->enabledExtensionCount;++i) {
        const auto* name=r->ppEnabledExtensionNames[i];if(!name)return false;
        surface |= !std::strcmp(name,"VK_KHR_surface");android |= !std::strcmp(name,"VK_KHR_android_surface");
    }
    return surface&&android;
}
namespace {
VkResult vkFailure(BootstrapResult result) {return result.vk<0?result.vk:VK_ERROR_INITIALIZATION_FAILED;}
static_assert(GameQueues::capacity==maxSplitGameQueues,"split redirect table == published game-queue list");
}
QueueLayoutChoice chooseQueueLayout(int property,bool cfgLoaded,unsigned cfgPriority,bool startupVerified) noexcept {
    QueueLayoutChoice c;c.prevUnverified=!startupVerified;
    if(property==0||property==2||property==3) {
        c.requested=c.layout=static_cast<QueueLayout>(property);c.source="property";return c;
    }
    if(!cfgLoaded)return c; // "default": Appended
    c.source="cfg";c.requested=c.layout=cfgPriority==1?QueueLayout::SplitHigh:QueueLayout::Appended;
    if(splitLayout(c.requested)&&!startupVerified){c.layout=QueueLayout::Appended;c.reason="PREV_SPLIT_UNVERIFIED";}
    return c;
}
int queuePriorityProperty() noexcept {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX]{};
    if(__system_property_get("debug.rdrvr.queueprio",v)==1&&(v[0]=='0'||v[0]=='2'||v[0]=='3'))return v[0]-'0';
#endif
    return -1;
}
QueueLayoutInputs defaultQueueLayoutInputs() noexcept {return {queuePriorityProperty,&graphicsSettings};}
// Right before the split xrCreateVulkanDeviceKHR: persist xrQueueVerified=0 so a crash, hang or dead display of this
// start makes the next PRIORITY start use Appended. A cfg-sourced split needs the write; a property split does not.
bool GameVulkanBridge::splitGate(void* context) noexcept {
    auto& self=*static_cast<GameVulkanBridge*>(context);
    self.gateWritten_=self.inputs_.settings&&self.inputs_.settings->setXrQueueVerified(false);
    return self.gateWritten_||self.gateOptional_;
}
VkResult GameVulkanBridge::createInstance(const VkInstanceCreateInfo* request,const VkAllocationCallbacks* allocator,VkInstance* output) {
    if(!active_ || !isGameInstance(request))return original_.createInstance(request,allocator,output);
    // No ordinal-based interception: the temporary GPU-info instance passes through.
    if(instance_) {if(output)*output=VK_NULL_HANDLE;state_="SECOND_GAME_INSTANCE_REJECTED";return VK_ERROR_INITIALIZATION_FAILED;}
    last_=bootstrap_->createInstance(*request,allocator,output);
    if(!last_){state_="XR_INSTANCE_CREATE_FAILED";active_=false;return vkFailure(last_);}
    instance_=*output;state_="XR_INSTANCE_CREATED";VkPhysicalDevice physical{};
    last_=bootstrap_->selectPhysicalDevice(&physical);
    // Instance was successfully created; never hide or leak it on later failure.
    // With no XR session, ordinary Vulkan creation can continue on this instance.
    if(!last_){active_=false;state_="XR_GPU_SELECTION_FAILED_FLAT";}
    return VK_SUCCESS;
}
VkResult GameVulkanBridge::createDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo* request,const VkAllocationCallbacks* allocator,VkDevice* output) {
    if(!active_ || !instance_)return original_.createDevice(physical,request,allocator,output);
    if(!request){if(output)*output=VK_NULL_HANDLE;return VK_ERROR_INITIALIZATION_FAILED;}
    // The queue layout. xrQueueVerified is latched at the first create: a repeated create in this process (the FDM
    // retry) must not see the 0 this process wrote itself.
    auto* settings=inputs_.settings;
    if(!startupLatched_){startupLatched_=true;startupVerified_=!settings||settings->snapshot().requested.xrQueueVerified;}
    const auto snapshot=settings?settings->snapshot():GraphicsSettings::Snapshot{};
    choice_=chooseQueueLayout(inputs_.property?inputs_.property():-1,settings&&snapshot.loaded,snapshot.requested.xrQueuePriority,startupVerified_);
    QueueLayout layout=choice_.layout;const char* bridgeReason=nullptr;
    if(splitLayout(layout)&&!original_.queue){layout=QueueLayout::Appended;bridgeReason="REDIRECT_UNAVAILABLE";}
    gateWritten_=false;gateOptional_=!std::strcmp(choice_.source,"property");
    bootstrap_->splitGate(splitLayout(layout)?&GameVulkanBridge::splitGate:nullptr,this);
    last_=bootstrap_->createDeviceWithReservedQueue(physical,*request,allocator,0,output,layout);
    bootstrap_->splitGate(nullptr,nullptr);
    layout_=bootstrap_->queueLayout();
    layout_.requested=choice_.requested;layout_.prevUnverified=choice_.prevUnverified;
    if(splitLayout(choice_.requested)&&!splitLayout(layout_.active)) {
        layout_.fallback=true;
        if(bridgeReason)layout_.reason=bridgeReason;
        else if(!splitLayout(choice_.layout))layout_.reason=choice_.reason;
    }
    const bool created=bool(last_);
    // The gate wrote 0 but no split device exists (the split create failed and Appended succeeded): nothing to verify.
    if(created&&gateWritten_&&!splitLayout(layout_.active)&&settings){settings->setXrQueueVerified(true);gateWritten_=false;}
    redirectCount_=0;redirect_={};
    unsigned published=0;
    if(created) {
        device_=*output;
        std::array<VkQueue,GameQueues::capacity> handles{};
        published=bootstrap_->gameQueueHandles(handles.data(),static_cast<unsigned>(handles.size()));
        if(published)publishGameQueues(handles.data(),published);else gameQueues.reset();
        if(splitLayout(layout_.active)) {
            redirect_=handles;redirectCount_=published;redirectFamily_=0;
            if(published!=layout_.gameQueues)layout_.reason="GAME_QUEUE_RESOLVE_FAILED"; // the device exists: report only
        }
    }
    GameQueueLayout::Info info;
    info.requested=static_cast<int>(layout_.requested);info.active=created?static_cast<int>(layout_.active):-1;
    info.source=choice_.source;info.reason=layout_.reason;info.fallback=layout_.fallback;info.prevUnverified=layout_.prevUnverified;
    info.splitAttempted=layout_.splitAttempted;info.protectedMemory=layout_.protectedMemory;
    info.splitXrResult=layout_.splitAttempted?static_cast<int>(layout_.split.xr):0;
    info.splitVkResult=layout_.splitAttempted?static_cast<int>(layout_.split.vk):0;
    info.gameQueues=layout_.gameQueues;info.gameQueueFlags=splitLayout(layout_.active)?unsigned(VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT):0u;
    info.xrFamily=0;info.xrIndex=splitLayout(layout_.active)?0u:layout_.gameQueues;info.xrFlags=0;info.globalPriority=layout_.globalPriority;
    info.familyFlags=layout_.familyFlags;info.familyQueues=layout_.familyQueues;
    info.globalPriorityExt=layout_.globalPriorityExt;info.globalPriorityExtAdded=layout_.globalPriorityExtAdded;
    info.gateWritten=gateWritten_;info.startupVerified=startupVerified_;info.deviceCreated=created;
    gameQueueLayout.publish(info,settings,xrLayerFramesSubmitted.load(std::memory_order_relaxed));
    xrQueueLayoutReason.store(layout_.reason,std::memory_order_relaxed);
    xrQueueLayoutActive.store(info.active,std::memory_order_relaxed);
    if(!created){state_="XR_DEVICE_CREATE_FAILED";return vkFailure(last_);}
    VkQueue privateQueue{};last_=bootstrap_->bindQueue(&privateQueue);
    bound_=bool(last_);state_=bound_?"XR_VULKAN_BOUND_NO_SESSION":"XR_QUEUE_BIND_FAILED";
    // Once created, the VkDevice belongs to the game even if binding fails.
    return VK_SUCCESS;
}
void GameVulkanBridge::getDeviceQueue(VkDevice device,std::uint32_t family,std::uint32_t index,VkQueue* output) {
    if(device && device==device_ && family==redirectFamily_ && index<redirectCount_ && output) {
        *output=redirect_[index];gameQueueLayout.redirects.fetch_add(1,std::memory_order_relaxed);return;
    }
    gameQueueLayout.forwarded.fetch_add(1,std::memory_order_relaxed);
    if(original_.queue)original_.queue(device,family,index,output);
    else if(output)*output=VK_NULL_HANDLE;
}
void GameVulkanBridge::destroyDevice(VkDevice device,const VkAllocationCallbacks* allocator) {
    if(device && device==device_){bound_=false;active_=false;device_=VK_NULL_HANDLE;redirectCount_=0;gameQueues.reset();state_="GAME_DEVICE_DESTROYED";}
    original_.destroyDevice(device,allocator);
}
void GameVulkanBridge::destroyInstance(VkInstance instance,const VkAllocationCallbacks* allocator) {
    if(instance && instance==instance_){bound_=false;active_=false;instance_=VK_NULL_HANDLE;state_="GAME_INSTANCE_DESTROYED";}
    original_.destroyInstance(instance,allocator);
}
}
