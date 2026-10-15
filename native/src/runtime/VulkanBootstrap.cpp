#include "VulkanBootstrap.h"
#include <cstring>
#include <vector>
namespace rdrvr::runtime {
namespace {
// Structures a split layout must not meet in the caller's device chain (VUID-VkDeviceCreateInfo-pNext-02830:
// VkPhysicalDeviceProtectedMemoryFeatures may not be chained next to VkPhysicalDeviceVulkan11Features; a second
// ProtectedMemoryFeatures would be a duplicate).
const char* splitChainConflict(const void* next) noexcept {
    unsigned n=0;
    for(auto* s=static_cast<const VkBaseInStructure*>(next);s;s=s->pNext) {
        if(++n>64)return "DEVICE_CHAIN_TOO_LONG";
        if(s->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)return "VULKAN11_FEATURES_IN_CHAIN";
        if(s->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES)return "PROTECTED_FEATURE_IN_CHAIN";
    }
    return nullptr;
}
const char* listedName(const char* const* names,std::uint32_t count,const char* a,const char* b) noexcept {
    for(std::uint32_t i=0;i<count;++i) {
        if(!names[i])continue;
        if(!std::strcmp(names[i],a))return a;
        if(!std::strcmp(names[i],b))return b;
    }
    return nullptr;
}
template<class F> F resolveInstance(PFN_vkGetInstanceProcAddr get,VkInstance instance,F given,const char* name) noexcept {
    return given?given:get&&instance?reinterpret_cast<F>(get(instance,name)):nullptr;
}
}
const char* queueLayoutName(QueueLayout l) noexcept {
    switch(l) {
    case QueueLayout::Appended:return "APPENDED";
    case QueueLayout::SplitHigh:return "SPLIT_HIGH";
    case QueueLayout::SplitNormal:return "SPLIT_NORMAL";
    }
    return "UNKNOWN";
}
const char* bootstrapErrorName(BootstrapError e) {
    switch(e) {
    case BootstrapError::None:return "OK";
    case BootstrapError::WrongPhase:return "WRONG_PHASE";
    case BootstrapError::InvalidInput:return "INVALID_INPUT";
    case BootstrapError::MissingFunctions:return "MISSING_FUNCTIONS";
    case BootstrapError::UnsupportedVersion:return "UNSUPPORTED_VERSION";
    case BootstrapError::PhysicalDeviceMismatch:return "PHYSICAL_DEVICE_MISMATCH";
    case BootstrapError::QueueNotRequested:return "QUEUE_NOT_REQUESTED";
    case BootstrapError::UnsupportedQueue:return "UNSUPPORTED_QUEUE";
    case BootstrapError::XrFailure:return "XR_FAILURE";
    case BootstrapError::VulkanFailure:return "VULKAN_FAILURE";
    case BootstrapError::NullHandle:return "NULL_HANDLE";
    }
    return "UNKNOWN";
}
BootstrapResult VulkanBootstrap::queryRequirements() {
    if(phase_!=Phase::Empty) return {BootstrapError::WrongPhase};
    if(!xr_ || !system_) return {BootstrapError::InvalidInput};
    if(!api_.requirements || !api_.createInstance || !api_.physicalDevice || !api_.createDevice ||
       !api_.getInstanceProcAddr || !api_.queueProperties || !api_.getDeviceQueue)
        return {BootstrapError::MissingFunctions};
    XrGraphicsRequirementsVulkan2KHR limits{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    const auto r=api_.requirements(xr_,system_,&limits);
    if(XR_FAILED(r)) return {BootstrapError::XrFailure,r};
    if(limits.minApiVersionSupported>limits.maxApiVersionSupported) return {BootstrapError::UnsupportedVersion};
    limits_=limits;phase_=Phase::Requirements;return {};
}
BootstrapResult VulkanBootstrap::createInstance(const VkInstanceCreateInfo& request,
        const VkAllocationCallbacks* allocator,VkInstance* output) {
    if(!output) return {BootstrapError::InvalidInput};
    *output=VK_NULL_HANDLE;
    if(phase_!=Phase::Requirements) return {BootstrapError::WrongPhase};
    if(request.sType!=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ||
       (request.pApplicationInfo && request.pApplicationInfo->sType!=VK_STRUCTURE_TYPE_APPLICATION_INFO))
        return {BootstrapError::InvalidInput};
    const auto raw=request.pApplicationInfo?request.pApplicationInfo->apiVersion:0;
    const auto version=raw?raw:VK_API_VERSION_1_0;
    // OpenXR graphics requirements compare API major/minor, not the patch.
    const auto xrVersion=XR_MAKE_VERSION(VK_API_VERSION_MAJOR(version),VK_API_VERSION_MINOR(version),0);
    const auto minimum=XR_MAKE_VERSION(XR_VERSION_MAJOR(limits_.minApiVersionSupported),XR_VERSION_MINOR(limits_.minApiVersionSupported),0);
    const auto maximum=XR_MAKE_VERSION(XR_VERSION_MAJOR(limits_.maxApiVersionSupported),XR_VERSION_MINOR(limits_.maxApiVersionSupported),0);
    if(VK_API_VERSION_VARIANT(version)!=0 || xrVersion<minimum || xrVersion>maximum)
        return {BootstrapError::UnsupportedVersion};
    XrVulkanInstanceCreateInfoKHR info{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    info.systemId=system_;info.pfnGetInstanceProcAddr=api_.getInstanceProcAddr;
    info.vulkanCreateInfo=&request;info.vulkanAllocator=allocator;
    VkInstance created{};VkResult vk=VK_ERROR_INITIALIZATION_FAILED;
    const auto xr=api_.createInstance(xr_,&info,&created,&vk);
    if(XR_FAILED(xr)) return {BootstrapError::XrFailure,xr,vk};
    if(vk!=VK_SUCCESS) return {BootstrapError::VulkanFailure,xr,vk};
    if(!created) return {BootstrapError::NullHandle,xr,vk};
    instance_=created;*output=created;phase_=Phase::Instance;return {};
}
BootstrapResult VulkanBootstrap::selectPhysicalDevice(VkPhysicalDevice* output) {
    if(!output) return {BootstrapError::InvalidInput};
    *output=VK_NULL_HANDLE;
    if(phase_!=Phase::Instance) return {BootstrapError::WrongPhase};
    XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    info.systemId=system_;info.vulkanInstance=instance_;
    VkPhysicalDevice selected{};
    const auto xr=api_.physicalDevice(xr_,&info,&selected);
    if(XR_FAILED(xr)) return {BootstrapError::XrFailure,xr};
    if(!selected) return {BootstrapError::NullHandle};
    physical_=selected;*output=selected;phase_=Phase::Physical;return {};
}
BootstrapResult VulkanBootstrap::createDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo& request,
        const VkAllocationCallbacks* allocator,QueueSelection queue,VkDevice* output) {
    if(!output) return {BootstrapError::InvalidInput};
    *output=VK_NULL_HANDLE;
    if(phase_!=Phase::Physical) return {BootstrapError::WrongPhase};
    if(physical!=physical_) return {BootstrapError::PhysicalDeviceMismatch};
    if(request.sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || !request.queueCreateInfoCount || !request.pQueueCreateInfos)
        return {BootstrapError::InvalidInput};
    // Validate the exact ordinary graphics queue that XR will use. Other game
    // queues and every feature/extension/pNext remain in the original request.
    bool requested=false;
    for(std::uint32_t i=0;i<request.queueCreateInfoCount;++i) {
        const auto& q=request.pQueueCreateInfos[i];
        if(q.sType!=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || !q.queueCount || !q.pQueuePriorities)
            return {BootstrapError::InvalidInput};
        if(q.queueFamilyIndex==queue.family && q.flags==0 && queue.index<q.queueCount) requested=true;
    }
    if(!requested) return {BootstrapError::QueueNotRequested};
    std::uint32_t count{};api_.queueProperties(physical_,&count,nullptr);
    if(queue.family>=count) return {BootstrapError::UnsupportedQueue};
    std::vector<VkQueueFamilyProperties> properties(count);
    api_.queueProperties(physical_,&count,properties.data());
    if(queue.family>=count || queue.index>=properties[queue.family].queueCount ||
       !(properties[queue.family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) return {BootstrapError::UnsupportedQueue};
    XrVulkanDeviceCreateInfoKHR info{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    info.systemId=system_;info.pfnGetInstanceProcAddr=api_.getInstanceProcAddr;
    info.vulkanPhysicalDevice=physical_;info.vulkanCreateInfo=&request;info.vulkanAllocator=allocator;
    VkDevice created{};VkResult vk=VK_ERROR_INITIALIZATION_FAILED;
    const auto xr=api_.createDevice(xr_,&info,&created,&vk);
    if(XR_FAILED(xr)) return {BootstrapError::XrFailure,xr,vk};
    if(vk!=VK_SUCCESS) return {BootstrapError::VulkanFailure,xr,vk};
    if(!created) return {BootstrapError::NullHandle,xr,vk};
    device_=created;selection_=queue;*output=created;phase_=Phase::Device;return {};
}
BootstrapResult VulkanBootstrap::bindQueue(VkQueue* output) {
    if(!output) return {BootstrapError::InvalidInput};
    *output=VK_NULL_HANDLE;
    if(phase_!=Phase::Device) return {BootstrapError::WrongPhase};
    VkQueue queue{};api_.getDeviceQueue(device_,selection_.family,selection_.index,&queue);
    if(!queue) return {BootstrapError::NullHandle};
    queue_=queue;*output=queue;phase_=Phase::Bound;return {};
}
unsigned VulkanBootstrap::gameQueueHandles(VkQueue* output,unsigned max) const {
    if(!output || !device_ || (phase_!=Phase::Device && phase_!=Phase::Bound))return 0;
    const unsigned n=layout_.gameQueues;
    if(!n || n>max)return 0;
    const bool split=splitLayout(layout_.active);
    if(split && !deviceQueue2_)return 0;
    for(unsigned i=0;i<n;++i) {
        VkQueue queue{};
        if(split) {
            // The game's create-info is protected-capable: only vkGetDeviceQueue2 with the same flags may return it
            // (VUID-vkGetDeviceQueue-flags-01841); its submissions stay unprotected.
            VkDeviceQueueInfo2 info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
            info.flags=VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;info.queueFamilyIndex=selection_.family;info.queueIndex=i;
            deviceQueue2_(device_,&info,&queue);
        } else api_.getDeviceQueue(device_,selection_.family,i,&queue);
        if(!queue)return 0;
        output[i]=queue;
    }
    return n;
}
// Every split precondition; nullptr = all passed. Resolves deviceQueue2_ and fills
// layout_.protectedMemory / globalPriorityExt(/Added). Only queries (no Vulkan object is created).
const char* VulkanBootstrap::splitPrecondition(const VkDeviceCreateInfo& request,std::uint32_t selected,
        const VkQueueFamilyProperties& family,QueueLayout layout) {
    const auto& game=request.pQueueCreateInfos[selected]; // flags 0 and the only create-info of the family (checked by the caller)
    if(game.pNext)return "QUEUE_CREATE_PNEXT";
    if(const char* conflict=splitChainConflict(request.pNext))return conflict;
    if(request.enabledExtensionCount>256 || (request.enabledExtensionCount && !request.ppEnabledExtensionNames))
        return "EXTENSION_LIST_INVALID";
    if(!(family.queueFlags&VK_QUEUE_PROTECTED_BIT))return "NO_PROTECTED_QUEUE";
    if(game.queueCount>maxSplitGameQueues)return "TOO_MANY_GAME_QUEUES";
    if(game.queueCount+1>family.queueCount)return "NO_SPARE_QUEUE";
    const auto features2=resolveInstance(api_.getInstanceProcAddr,instance_,api_.physicalFeatures2,"vkGetPhysicalDeviceFeatures2");
    if(!features2)return "NO_FEATURES2";
    deviceQueue2_=resolveInstance(api_.getInstanceProcAddr,instance_,api_.getDeviceQueue2,"vkGetDeviceQueue2");
    if(!deviceQueue2_)return "NO_GET_DEVICE_QUEUE2";
    VkPhysicalDeviceProtectedMemoryFeatures protectedFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&protectedFeatures;
    features2(physical_,&features);
    layout_.protectedMemory=protectedFeatures.protectedMemory==VK_TRUE;
    if(!layout_.protectedMemory)return "NO_PROTECTED_MEMORY";
    if(layout!=QueueLayout::SplitHigh)return nullptr;
    if(const char* enabled=listedName(request.ppEnabledExtensionNames,request.enabledExtensionCount,
                                      VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME,VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME)) {
        layout_.globalPriorityExt=enabled;return nullptr; // the game enabled it already: never add a duplicate
    }
    const auto enumerate=resolveInstance(api_.getInstanceProcAddr,instance_,api_.enumerateDeviceExtensions,"vkEnumerateDeviceExtensionProperties");
    if(!enumerate)return "NO_EXTENSION_QUERY";
    std::uint32_t count{};
    if(enumerate(physical_,nullptr,&count,nullptr)!=VK_SUCCESS || count>4096)return "EXTENSION_QUERY_FAILED";
    std::vector<VkExtensionProperties> available(count);
    const auto listed=enumerate(physical_,nullptr,&count,available.data());
    if((listed!=VK_SUCCESS && listed!=VK_INCOMPLETE) || count>available.size())return "EXTENSION_QUERY_FAILED";
    bool ext=false,khr=false;
    for(std::uint32_t i=0;i<count;++i) {
        ext|=!std::strcmp(available[i].extensionName,VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME);
        khr|=!std::strcmp(available[i].extensionName,VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME);
    }
    if(!ext && !khr)return "NO_GLOBAL_PRIORITY_EXTENSION";
    layout_.globalPriorityExt=ext?VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME:VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME;
    layout_.globalPriorityExtAdded=true;
    return nullptr;
}
BootstrapResult VulkanBootstrap::createDeviceWithReservedQueue(VkPhysicalDevice physical,const VkDeviceCreateInfo& request,
        const VkAllocationCallbacks* allocator,std::uint32_t family,VkDevice* output,QueueLayout layout) {
    layout_={};layout_.requested=layout;
    if(!output) return {BootstrapError::InvalidInput};
    *output=VK_NULL_HANDLE;
    if(phase_!=Phase::Physical) return {BootstrapError::WrongPhase};
    if(physical!=physical_) return {BootstrapError::PhysicalDeviceMismatch};
    if(request.sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO || !request.queueCreateInfoCount ||
       request.queueCreateInfoCount>32 || !request.pQueueCreateInfos) return {BootstrapError::InvalidInput};
    std::uint32_t selected=request.queueCreateInfoCount;
    for(std::uint32_t i=0;i<request.queueCreateInfoCount;++i) {
        const auto& q=request.pQueueCreateInfos[i];
        if(q.sType!=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO || !q.queueCount ||
           q.queueCount>64 || !q.pQueuePriorities) return {BootstrapError::InvalidInput};
        if(q.queueFamilyIndex==family) {
            // Reject mixed protected/ordinary or duplicate family requests.
            if(selected!=request.queueCreateInfoCount || q.flags!=0) return {BootstrapError::UnsupportedQueue};
            selected=i;
        }
    }
    if(selected==request.queueCreateInfoCount) return {BootstrapError::QueueNotRequested};
    std::uint32_t count{};api_.queueProperties(physical_,&count,nullptr);
    if(family>=count || count>256) return {BootstrapError::UnsupportedQueue};
    std::vector<VkQueueFamilyProperties> properties(count);
    api_.queueProperties(physical_,&count,properties.data());
    const auto index=request.pQueueCreateInfos[selected].queueCount;
    if(count>properties.size() || family>=count || index>=properties[family].queueCount ||
       !(properties[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) return {BootstrapError::UnsupportedQueue};
    layout_.gameQueues=index;layout_.familyFlags=properties[family].queueFlags;layout_.familyQueues=properties[family].queueCount;
    if(splitLayout(layout)) {
        // Split layout: the game's create-info turns protected-capable, the XR queue gets its own
        // create-info (the
        // only legal second create-info of one family, VUID-VkDeviceCreateInfo-queueFamilyIndex-02802). All storage is
        // local to this synchronous call; nothing of the caller's request is retained.
        const char* reason=splitPrecondition(request,selected,properties[family],layout);
        if(!reason && gate_ && !gate_(gateContext_))reason="SPLIT_GATE_REFUSED";
        if(!reason) {
            std::vector<VkDeviceQueueCreateInfo> queues(request.pQueueCreateInfos,request.pQueueCreateInfos+request.queueCreateInfoCount);
            queues[selected].flags=VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;
            VkDeviceQueueGlobalPriorityCreateInfoKHR priority{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
            priority.globalPriority=static_cast<VkQueueGlobalPriorityKHR>(splitGlobalPriority);
            const float one=1.f;
            VkDeviceQueueCreateInfo xrQueue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            xrQueue.pNext=layout==QueueLayout::SplitHigh?&priority:nullptr;xrQueue.flags=0;
            xrQueue.queueFamilyIndex=family;xrQueue.queueCount=1;xrQueue.pQueuePriorities=&one;
            queues.push_back(xrQueue);
            std::vector<const char*> extensions;
            VkPhysicalDeviceProtectedMemoryFeatures protectedFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES};
            protectedFeatures.pNext=const_cast<void*>(request.pNext);protectedFeatures.protectedMemory=VK_TRUE;
            auto split=request;split.pNext=&protectedFeatures;
            split.queueCreateInfoCount=static_cast<std::uint32_t>(queues.size());split.pQueueCreateInfos=queues.data();
            if(layout_.globalPriorityExtAdded) {
                if(request.enabledExtensionCount)
                    extensions.assign(request.ppEnabledExtensionNames,request.ppEnabledExtensionNames+request.enabledExtensionCount);
                extensions.push_back(layout_.globalPriorityExt);
                split.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size());split.ppEnabledExtensionNames=extensions.data();
            }
            layout_.splitAttempted=true;
            const auto result=createDevice(physical,split,allocator,{family,0},output);
            layout_.split=result;
            if(result) {
                layout_.active=layout;layout_.globalPriority=layout==QueueLayout::SplitHigh?splitGlobalPriority:0;
                return result;
            }
            reason="SPLIT_CREATE_FAILED";
        }
        layout_.fallback=true;layout_.reason=reason;
        layout_.globalPriorityExt="";layout_.globalPriorityExtAdded=false; // Appended enables no extension of its own
    }
    std::vector<VkDeviceQueueCreateInfo> queues(request.pQueueCreateInfos,request.pQueueCreateInfos+request.queueCreateInfoCount);
    const auto* originalPriorities=queues[selected].pQueuePriorities;
    std::vector<float> priorities(originalPriorities,originalPriorities+index);
    priorities.push_back(1.f);
    queues[selected].queueCount=index+1;queues[selected].pQueuePriorities=priorities.data();
    auto augmented=request;augmented.pQueueCreateInfos=queues.data();
    return createDevice(physical,augmented,allocator,{family,index},output);
}
bool VulkanBootstrap::binding(XrGraphicsBindingVulkan2KHR& output) const {
    if(phase_!=Phase::Bound || !queue_) return false;
    output={XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    output.instance=instance_;output.physicalDevice=physical_;output.device=device_;
    output.queueFamilyIndex=selection_.family;output.queueIndex=selection_.index;return true;
}
} // namespace rdrvr::runtime
