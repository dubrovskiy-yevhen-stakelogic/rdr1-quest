#include "VulkanBridge.h"
#include "VulkanBootstrap.h"
#include <algorithm>
#include <vector>
namespace rdrvr::runtime {
namespace {
void require(BootstrapResult result,const char* operation) {
    if(!result) throw std::runtime_error(std::string(operation)+" "+bootstrapErrorName(result.error)+
        " xr="+std::to_string(result.xr)+" vk="+std::to_string(result.vk));
}
}
void VulkanBridge::create(XrInstance xr, XrSystemId system) {
    if(instance || device) throw std::runtime_error("VulkanBridge already owns a context");
    owned_=true;
    VulkanBootstrapDispatch dispatch{
        proc<PFN_xrGetVulkanGraphicsRequirements2KHR>(xr,"xrGetVulkanGraphicsRequirements2KHR"),
        proc<PFN_xrCreateVulkanInstanceKHR>(xr,"xrCreateVulkanInstanceKHR"),
        proc<PFN_xrGetVulkanGraphicsDevice2KHR>(xr,"xrGetVulkanGraphicsDevice2KHR"),
        proc<PFN_xrCreateVulkanDeviceKHR>(xr,"xrCreateVulkanDeviceKHR"),
        vkGetInstanceProcAddr,vkGetPhysicalDeviceQueueFamilyProperties,vkGetDeviceQueue};
    VulkanBootstrap bootstrap(xr,system,dispatch);
    require(bootstrap.queryRequirements(),"Vulkan requirements");
    const auto& limits=bootstrap.requirements();
    auto cv=[](XrVersion v) { return VK_MAKE_VERSION(XR_VERSION_MAJOR(v),XR_VERSION_MINOR(v),0); };
    const auto api=std::max(std::uint32_t(VK_API_VERSION_1_0),cv(limits.minApiVersionSupported));
    if (api>VK_API_VERSION_1_1 || api>cv(limits.maxApiVersionSupported))
        throw std::runtime_error("harness supports Vulkan 1.0/1.1; incompatible runtime requirement");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName="RDRVR harness"; app.apiVersion=api;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&app;
    // The same path will accept the original game create-info once its early
    // creation hook is validated. Here only the diagnostic owner supplies it.
    require(bootstrap.createInstance(ci,nullptr,&instance),"create Vulkan instance");
    require(bootstrap.selectPhysicalDevice(&physical),"XR physical device");
    std::uint32_t count{}; vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical,&count,families.data());
    auto found=std::find_if(families.begin(),families.end(),[](const auto& f){return f.queueCount && (f.queueFlags&VK_QUEUE_GRAPHICS_BIT);});
    if(found==families.end()) throw std::runtime_error("no graphics queue");
    queueFamily=static_cast<std::uint32_t>(found-families.begin()); queueIndex=0;
    float priority=1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex=queueFamily; qi.queueCount=1; qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
    require(bootstrap.createDevice(physical,di,nullptr,{queueFamily,queueIndex},&device),"create Vulkan device");
    require(bootstrap.bindQueue(&queue),"bind Vulkan queue");
    XrGraphicsBindingVulkan2KHR verified{};
    if(!bootstrap.binding(verified) || verified.device!=device || verified.instance!=instance)
        throw std::runtime_error("negotiated Vulkan context mismatch");
    VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical,&properties);
    __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","vulkan=%s api=%u queue=%u/%u",properties.deviceName,api,queueFamily,queueIndex);
}
XrGraphicsBindingVulkan2KHR VulkanBridge::binding() const {
    if(!instance || !physical || !device || !queue) throw std::runtime_error("Vulkan binding not ready");
    XrGraphicsBindingVulkan2KHR b{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    b.instance=instance; b.physicalDevice=physical; b.device=device; b.queueFamilyIndex=queueFamily; b.queueIndex=queueIndex;
    return b;
}
void VulkanBridge::borrow(const VulkanBootstrap& negotiated) {
    if(instance || device)throw std::runtime_error("VulkanBridge context already set");
    XrGraphicsBindingVulkan2KHR b{};
    if(!negotiated.binding(b))throw std::runtime_error("borrow requires negotiated binding");
    VkQueue q{};vkGetDeviceQueue(b.device,b.queueFamilyIndex,b.queueIndex,&q);
    if(!q)throw std::runtime_error("negotiated queue missing");
    instance=b.instance;physical=b.physicalDevice;device=b.device;queue=q;
    queueFamily=b.queueFamilyIndex;queueIndex=b.queueIndex;owned_=false;
}
void VulkanBridge::waitIdle() noexcept {
    if(owned_ && device)vkDeviceWaitIdle(device);
    else if(queue)vkQueueWaitIdle(queue);
}
void VulkanBridge::destroy() noexcept {
    if(owned_ && device) { vkDeviceWaitIdle(device); vkDestroyDevice(device,nullptr); }
    if(owned_ && instance) vkDestroyInstance(instance,nullptr);
    device={}; instance={}; physical={}; queue={}; queueFamily=queueIndex=0;
    owned_=false;
}
} // namespace rdrvr::runtime
