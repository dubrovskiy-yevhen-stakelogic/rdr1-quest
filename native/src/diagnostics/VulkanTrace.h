#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdint>
#include <string>
namespace rdrvr::diagnostics {
struct TraceReader {
    void* context{};
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};
};
struct TraceCapture { std::string json; bool complete{}; };
TraceCapture captureInstance(const VkInstanceCreateInfo*,TraceReader);
TraceCapture captureDevice(const VkDeviceCreateInfo*,TraceReader);
struct TraceDispatch {
    PFN_vkCreateInstance instance{};
    PFN_vkCreateDevice device{};
    PFN_vkGetDeviceQueue queue{};
    PFN_vkQueueSubmit submit{};
    PFN_vkEnumeratePhysicalDevices enumerate{};
    PFN_vkDestroyInstance destroyInstance{};
    PFN_vkDestroyDevice destroyDevice{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties families{};
    PFN_vkDeviceWaitIdle idle{};
};
struct TraceOutput {
    void* context{};
    void (*write)(void*,const std::string&){};
    std::uint64_t (*clockNs)(){};
    std::uint64_t (*threadId)(){};
};
// Configured once before any wrapper is published. Never creates extra Vulkan
// work, changes input/output pointers or VkResult, or destroys caller resources.
class VulkanTrace {
    TraceDispatch api_{};
    TraceReader reader_{};
    TraceOutput output_{};
    std::atomic<unsigned> instances_{},devices_{},queues_{},submits_{},failures_{};
    std::atomic<unsigned> enumerations_{},instanceDestructions_{},deviceDestructions_{},familyQueries_{};
    void record(const char*,const TraceCapture&,std::uint64_t,std::uint64_t,VkResult,
                std::uintptr_t,std::uintptr_t,std::uintptr_t,std::uintptr_t) noexcept;
public:
    struct Counts {unsigned instances,devices,queues,submits,failures,enumerations,instanceDestructions,deviceDestructions,familyQueries;};
    Counts counts() const {return {instances_.load(),devices_.load(),queues_.load(),submits_.load(),failures_.load(),
        enumerations_.load(),instanceDestructions_.load(),deviceDestructions_.load(),familyQueries_.load()};}
    void configure(TraceDispatch api,TraceReader reader,TraceOutput output) {api_=api;reader_=reader;output_=output;}
    VkResult createInstance(const VkInstanceCreateInfo*,const VkAllocationCallbacks*,VkInstance*) noexcept;
    VkResult createDevice(VkPhysicalDevice,const VkDeviceCreateInfo*,const VkAllocationCallbacks*,VkDevice*) noexcept;
    void getDeviceQueue(VkDevice,std::uint32_t,std::uint32_t,VkQueue*) noexcept;
    VkResult queueSubmit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence) noexcept;
    VkResult deviceWaitIdle(VkDevice d) noexcept {return api_.idle(d);}
    VkResult enumeratePhysicalDevices(VkInstance,std::uint32_t*,VkPhysicalDevice*) noexcept;
    void destroyInstance(VkInstance,const VkAllocationCallbacks*) noexcept;
    void destroyDevice(VkDevice,const VkAllocationCallbacks*) noexcept;
    void getPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice,std::uint32_t*,VkQueueFamilyProperties*) noexcept;
};
} // namespace rdrvr::diagnostics
