#pragma once
#ifdef XR_USE_PLATFORM_ANDROID
#include <jni.h>
#endif
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <cstdint>

namespace rdrvr::runtime {
// Injected entry points let the exact production path run in host tests.
struct VulkanBootstrapDispatch {
    PFN_xrGetVulkanGraphicsRequirements2KHR requirements{};
    PFN_xrCreateVulkanInstanceKHR createInstance{};
    PFN_xrGetVulkanGraphicsDevice2KHR physicalDevice{};
    PFN_xrCreateVulkanDeviceKHR createDevice{};
    PFN_vkGetInstanceProcAddr getInstanceProcAddr{};
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queueProperties{};
    PFN_vkGetDeviceQueue getDeviceQueue{};
    // XR QUEUE: used only for a split queue layout request. A null entry is resolved through getInstanceProcAddr on
    // the XR-created instance (core Vulkan 1.1 names); Appended never calls or resolves them.
    PFN_vkGetPhysicalDeviceFeatures2 physicalFeatures2{};
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensions{};
    PFN_vkGetDeviceQueue2 getDeviceQueue2{};
};
enum class BootstrapError {
    None, WrongPhase, InvalidInput, MissingFunctions, UnsupportedVersion,
    PhysicalDeviceMismatch, QueueNotRequested, UnsupportedQueue,
    XrFailure, VulkanFailure, NullHandle
};
struct BootstrapResult {
    BootstrapError error{BootstrapError::None};
    XrResult xr{XR_SUCCESS};
    VkResult vk{VK_SUCCESS};
    explicit operator bool() const { return error==BootstrapError::None; }
};
const char* bootstrapErrorName(BootstrapError error);
struct QueueSelection { std::uint32_t family{}, index{}; };
// XR QUEUE layout: where the XR binding queue sits next to the game's queues in the reserved family.
//  Appended    the game's create-info gets one extra ordinary queue (priority 1.0); binding {family, game count}.
//  SplitHigh   the game's create-info becomes PROTECTED-capable (count and priorities unchanged, the game still submits
//              unprotected work), VkPhysicalDeviceProtectedMemoryFeatures{protectedMemory=1} is prepended to the device
//              chain, and a second create-info {flags 0, count 1, priority 1.0} carries VkDeviceQueueGlobalPriorityCreateInfoKHR
//              HIGH (512, never REALTIME) plus VK_EXT_global_priority (else the KHR name); binding {family, 0}.
//  SplitNormal the same split without the global priority struct or extension (layout control).
// Only a split puts the XR queue in a different KGSL priority class: the driver ignores pQueuePriorities, and a global
// priority on the shared create-info applies to every queue in it.
enum class QueueLayout : std::uint8_t { Appended=0, SplitHigh=2, SplitNormal=3 };
constexpr bool splitLayout(QueueLayout l) noexcept { return l==QueueLayout::SplitHigh || l==QueueLayout::SplitNormal; }
const char* queueLayoutName(QueueLayout layout) noexcept; // APPENDED, SPLIT_HIGH, SPLIT_NORMAL
inline constexpr std::uint32_t maxSplitGameQueues=4;       // runtime::GameQueues::capacity (redirect table)
inline constexpr std::uint32_t splitGlobalPriority=VK_QUEUE_GLOBAL_PRIORITY_HIGH_KHR;
struct QueueLayoutResult {
    QueueLayout requested{QueueLayout::Appended}, active{QueueLayout::Appended};
    std::uint32_t gameQueues{};          // the game's queue count in the reserved family (= the Appended binding index)
    bool fallback{};                     // a split was requested and Appended was used
    BootstrapResult split{};             // the split create's result (valid when splitAttempted)
    const char* reason{"OK"};            // static string: OK or why Appended was used
    bool prevUnverified{};               // filled by GameVulkanBridge (xrQueueVerified gate at startup)
    bool splitAttempted{};               // xrCreateVulkanDeviceKHR was called with the split request
    bool protectedMemory{};              // protectedMemory feature reported (queried for split requests only)
    const char* globalPriorityExt{""};   // global-priority extension enabled for SplitHigh ("" none)
    bool globalPriorityExtAdded{};       // ... and the mod appended it (false: the game already enabled it)
    std::uint32_t globalPriority{};      // VkQueueGlobalPriorityKHR on the XR create-info (0 none)
    std::uint32_t familyFlags{}, familyQueues{}; // VkQueueFamilyProperties of the reserved family
};

// One caller owns this state machine. It NEVER destroys Vulkan objects, adopts
// existing handles, edits a request, or retains pointers into caller create-info.
// Successful output handles belong to the caller, including on later failure.
// Caller must serialize game queue work with XR begin/end/acquire/release.
class VulkanBootstrap {
public:
    enum class Phase { Empty, Requirements, Instance, Physical, Device, Bound };
    VulkanBootstrap(XrInstance xr,XrSystemId system,VulkanBootstrapDispatch api)
        : xr_(xr),system_(system),api_(api) {}
    VulkanBootstrap(const VulkanBootstrap&)=delete;
    VulkanBootstrap& operator=(const VulkanBootstrap&)=delete;
    BootstrapResult queryRequirements();
    const XrGraphicsRequirementsVulkan2KHR& requirements() const { return limits_; }
    BootstrapResult createInstance(const VkInstanceCreateInfo& request,
                                   const VkAllocationCallbacks* allocator,VkInstance* output);
    BootstrapResult selectPhysicalDevice(VkPhysicalDevice* output);
    BootstrapResult createDevice(VkPhysicalDevice physical,const VkDeviceCreateInfo& request,
                                 const VkAllocationCallbacks* allocator,QueueSelection queue,VkDevice* output);
    // Explicit opt-in for RDR: keep every game queue and feature, append one
    // ordinary queue in the selected family, and bind XR to that private index.
    // The caller request is never edited; only this synchronous XR request uses
    // owned copies of the queue array and the selected family's priorities.
    // A split layout falls back to Appended when any precondition fails (queueLayout().reason) and
    // retries once with Appended when the split create fails (reason SPLIT_CREATE_FAILED, the
    // split XrResult/VkResult kept in queueLayout().split).
    BootstrapResult createDeviceWithReservedQueue(VkPhysicalDevice physical,const VkDeviceCreateInfo& request,
                                 const VkAllocationCallbacks* allocator,std::uint32_t family,VkDevice* output,
                                 QueueLayout layout=QueueLayout::Appended);
    const QueueLayoutResult& queueLayout() const { return layout_; }
    // The game's own queues of the reserved family once the device exists: Appended getDeviceQueue(family,i),
    // split getDeviceQueue2({PROTECTED, family, i}), i < queueLayout().gameQueues. Returns the count written; 0 on the
    // wrong phase, a null handle, or when the game's count exceeds max (nothing partial).
    unsigned gameQueueHandles(VkQueue* output,unsigned max) const;
    // Called once, right before the split xrCreateVulkanDeviceKHR (every precondition passed). false
    // cancels the split (reason SPLIT_GATE_REFUSED, Appended is used). nullptr = no gate.
    using SplitGate=bool(*)(void* context) noexcept;
    void splitGate(SplitGate gate,void* context) noexcept { gate_=gate; gateContext_=context; }
    BootstrapResult bindQueue(VkQueue* output);
    bool binding(XrGraphicsBindingVulkan2KHR& output) const;
    Phase phase() const { return phase_; }
private:
    XrInstance xr_{};
    XrSystemId system_{};
    VulkanBootstrapDispatch api_;
    Phase phase_{Phase::Empty};
    XrGraphicsRequirementsVulkan2KHR limits_{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    VkInstance instance_{};
    VkPhysicalDevice physical_{};
    VkDevice device_{};
    VkQueue queue_{};
    QueueSelection selection_{};
    QueueLayoutResult layout_{};
    PFN_vkGetDeviceQueue2 deviceQueue2_{};
    SplitGate gate_{};
    void* gateContext_{};
    const char* splitPrecondition(const VkDeviceCreateInfo& request,std::uint32_t selected,
                                  const VkQueueFamilyProperties& family,QueueLayout layout);
};
} // namespace rdrvr::runtime
