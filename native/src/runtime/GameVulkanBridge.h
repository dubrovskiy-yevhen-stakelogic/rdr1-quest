#pragma once
#include "VulkanBootstrap.h"
#include <array>
namespace rdrvr::runtime {
class GraphicsSettings;
struct GameVulkanDispatch {
    PFN_vkCreateInstance createInstance{};
    PFN_vkCreateDevice createDevice{};
    PFN_vkDestroyInstance destroyInstance{};
    PFN_vkDestroyDevice destroyDevice{};
    // QU: the game's original vkGetDeviceQueue (GameXr G9 routes the game's calls through getDeviceQueue below).
    // Null = the redirect is not routed: split queue layouts are refused (reason REDIRECT_UNAVAILABLE).
    PFN_vkGetDeviceQueue queue{};
};
// QU: queue layout selection for the game device (read once, before the first XR device create).
//  debug.rdrvr.queueprio 0/2/3 wins (developer override; the xrQueueVerified gate is reported but not applied);
//  otherwise graphics.cfg xrQueuePriority 0 NORMAL = Appended, 1 PRIORITY = SplitHigh; no cfg loaded = Appended.
//  PRIORITY while the previous split start never reached 300 layer frames (startupVerified false) = Appended,
//  reason PREV_SPLIT_UNVERIFIED.
struct QueueLayoutChoice {
    QueueLayout requested{QueueLayout::Appended},layout{QueueLayout::Appended};
    const char* source{"default"};   // property | cfg | default
    const char* reason{"OK"};
    bool prevUnverified{};
};
QueueLayoutChoice chooseQueueLayout(int property,bool cfgLoaded,unsigned cfgPriority,bool startupVerified) noexcept;
int queuePriorityProperty() noexcept; // debug.rdrvr.queueprio: 0, 2 or 3 (single digit), else -1; -1 off Android
struct QueueLayoutInputs {
    int (*property)() noexcept{};      // nullptr = no property
    GraphicsSettings* settings{};      // cfg source and gate writes; nullptr = default layout, no gate
};
QueueLayoutInputs defaultQueueLayoutInputs() noexcept; // {queuePriorityProperty, &graphicsSettings}
// Called under the Android adapter lock. No sessions, frames or queue submissions.
class GameVulkanBridge {
    VulkanBootstrap* bootstrap_{};
    GameVulkanDispatch original_{};
    VkInstance instance_{};VkDevice device_{};
    bool active_{},bound_{};
    const char* state_{"FLAT"};
    BootstrapResult last_{};
    QueueLayoutInputs inputs_{};
    QueueLayoutResult layout_{};
    QueueLayoutChoice choice_{};
    bool startupLatched_{},startupVerified_{true},gateWritten_{},gateOptional_{};
    std::array<VkQueue,4> redirect_{};unsigned redirectCount_{};std::uint32_t redirectFamily_{};
    static bool splitGate(void* context) noexcept;
public:
    GameVulkanBridge(VulkanBootstrap* bootstrap,GameVulkanDispatch original)
        :GameVulkanBridge(bootstrap,original,defaultQueueLayoutInputs()) {}
    GameVulkanBridge(VulkanBootstrap* bootstrap,GameVulkanDispatch original,QueueLayoutInputs inputs)
        :bootstrap_(bootstrap),original_(original),active_(bootstrap!=nullptr),inputs_(inputs) {}
    static bool isGameInstance(const VkInstanceCreateInfo*);
    VkResult createInstance(const VkInstanceCreateInfo*,const VkAllocationCallbacks*,VkInstance*);
    VkResult createDevice(VkPhysicalDevice,const VkDeviceCreateInfo*,const VkAllocationCallbacks*,VkDevice*);
    void destroyInstance(VkInstance,const VkAllocationCallbacks*);
    void destroyDevice(VkDevice,const VkAllocationCallbacks*);
    // QU: the game's vkGetDeviceQueue. Split layout: the game's own (family, index < game count) queues are the
    // protected-capable handles resolved with vkGetDeviceQueue2 at creation; every other call is forwarded unchanged.
    void getDeviceQueue(VkDevice device,std::uint32_t family,std::uint32_t index,VkQueue* output);
    bool binding(XrGraphicsBindingVulkan2KHR& out) const {return bound_ && bootstrap_->binding(out);}
    VkInstance gameInstance() const {return active_?instance_:VK_NULL_HANDLE;}
    const char* state() const {return state_;}
    BootstrapResult lastResult() const {return last_;}
    const QueueLayoutResult& queueLayout() const {return layout_;}   // QU: the last device create's layout
    const QueueLayoutChoice& queueLayoutChoice() const {return choice_;}
};
}
