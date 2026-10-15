#pragma once
#include <span>
#include "VulkanBridge.h"
#include "HdrTransfer.h"
#include <array>
#include <mutex>
#include <string>
#include <vector>
namespace rdrvr::runtime {
enum class QuadSourceMode { GamePresent,CompletedImage };
struct PresentImageInfo {
    VkDevice device{};VkImage image{};VkExtent2D extent{};VkFormat format{};
    VkImageUsageFlags usage{};std::uint64_t lifetime{};
};
struct CompletedQuadImage {
    VkDevice device{};VkImage image{};VkExtent2D extent{};VkFormat format{};VkImageLayout layout{};
    VkImageUsageFlags usage{};VkSampleCountFlagBits samples{};std::uint32_t layers{},mips{},queueFamily{};
    VkFence completed{};
    std::uint64_t frameId{},lifetime{},trackingEpoch{},spaceEpoch{};
};
// GPU mailbox ring. The producer runs inside the game's serialized present call, the consumer on the
// reserved XR queue; neither side waits for its own copy. The producer reuses a slot only after its
// copy fence signalled (and GPU-waits the consumer's done signal); the consumer reads only copies it
// observed complete. Binary semaphores carry memory dependencies across the queues, each signal
// waited exactly once. The native present waits a chain semaphore (one per game swapchain image)
// signalled by the theater's last batch.
class GameTheater {
public:
    static constexpr unsigned slotCount=3,relayCount=4,consumerCount=3;
    static constexpr std::uint64_t slotWaitNs=50'000'000;
private:
    struct Slot {
        VkImage image{};VkDeviceMemory memory{};VkCommandBuffer command{};VkFence fence{};VkSemaphore ready{},done{};
        std::uint64_t sequence{};bool submitted{},readyPending{},donePending{},initialized{};
    };
    struct Relay {VkFence fence{};bool submitted{};};
    struct Consumer {VkCommandBuffer command{};VkFence fence{};bool submitted{};};
    // Present-wait semaphores belong to one swapchain: destroyed only after the
    // game destroyed that swapchain (or at device shutdown).
    struct Chain {VkSwapchainKHR swapchain{};std::vector<VkSemaphore> semaphores;std::vector<std::uint8_t> unconsumed;};
    const QuadSourceMode mode_;
    std::mutex lock_;
    VkDevice device_{}; VkPhysicalDevice physical_{}; std::uint32_t family_{};
    std::vector<VkQueue> gameQueues_;
    VkQueue consumerQueue_{};
    VkSwapchainKHR gameSwapchain_{}; std::vector<VkImage> gameImages_;
    VkExtent2D extent_{}; VkFormat format_{};
    VkImageUsageFlags gameUsage_{};
    std::uint64_t gameGeneration_{};
    HdrTransfer overlay_;
    VkImage overlayImage_{};VkFormat overlayFormat_{};std::uint64_t overlayLifetime_{};
    std::uint64_t overlaid_{},overlayRejected_{};
    const char* overlayStatus_{"NOT_CONFIGURED"};
    std::array<Slot,slotCount> slots_{};
    std::array<Relay,relayCount> relays_{};unsigned nextRelay_{};
    std::array<Consumer,consumerCount> consumers_{};
    VkCommandPool pool_{};
    Chain chain_{};std::vector<Chain> retiredChains_;
    // World presents leave the 2D image stale; images published up to
    // staleSequence_ are pre-world and must not be shown when the 2D path resumes.
    std::uint64_t sequence_{},displayedSequence_{},staleSequence_{};int displayedSlot_{-1};
    bool accepting_{},initialized_{},poisoned_{},inFlight_{};
    XrSwapchain screen_{}; std::vector<XrSwapchainImageVulkan2KHR> screenImages_;
    VkFormat screenFormat_{}; VkExtent2D screenExtent_{}; bool screenValid_{};
    std::uint64_t presents_{},captured_{},copied_{},skipped_{};
    std::uint64_t worldBypassed_{},worldRetired_{},worldDenied_{},worldHudDenied_{};
    std::uint64_t slotWaits_{},slotTimeouts_{},relayWaits_{},overlayWaits_{},completedWaits_{},readyDrains_{},doneWaits_{},chainDrains_{};
    std::uint64_t consumerNotReady_{},consumerBusy_{},framesInFlightMax_{};
    bool worldStale_{};
    std::uint64_t externalFrame_{},externalTracking_{},externalSpace_{},externalNs_{};
    bool externalValid_{};
    const char* status_{"WAITING_FOR_SWAPCHAIN"};
    PFN_vkCreateSwapchainKHR create_{}; PFN_vkDestroySwapchainKHR destroy_{}; PFN_vkQueuePresentKHR present_{};
    void (*completed_)(VkQueue,VkFence,std::span<const VkSemaphore>) noexcept{};
    bool (*overlayPending_)(VkQueue) noexcept{};
    bool (*overlaySource_)(VkQueue,VkFence,CompletedQuadImage&,std::span<const VkSemaphore>) noexcept{};
    bool (*worldBypass_)(VkQueue) noexcept{};
    bool (*worldRetire_)(VkQueue) noexcept{};
    bool prepareOverlay(const CompletedQuadImage&,VkFence);
    void prepareRelays();
    void prepare();
    void discardMailbox();
    void discardScreen();
    void destroyChain(Chain&) noexcept;
    void begin(VkCommandBuffer);
    void submit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence);
    void waitFence(VkFence,std::uint64_t& counter);
    bool observeSlot(unsigned);
    bool observeRelay(unsigned);
    bool observeConsumer(unsigned);
    int acquireSlot();
    unsigned acquireRelay();
    int acquireConsumer();
    unsigned framesInFlight() const;
    VkSemaphore chainSemaphore(std::uint32_t imageIndex,bool& drain);
    bool renderLocked(XrSession,VkQueue,XrSwapchain&,VkExtent2D&);
public:
    explicit GameTheater(QuadSourceMode mode=QuadSourceMode::GamePresent):mode_(mode){}
    static constexpr std::uint64_t completedMaxAgeNs=150'000'000;
    void route(PFN_vkCreateSwapchainKHR,PFN_vkDestroySwapchainKHR,PFN_vkQueuePresentKHR);
    void bind(const XrGraphicsBindingVulkan2KHR&);
    // The fence may still be pending: the callback's batch has
    // consumed the present's waits on this queue; work it queues afterwards on
    // the same queue is GPU-ordered after them. The fence stays valid (never
    // reset) until it has signalled.
    void onCompletedPresent(void (*callback)(VkQueue,VkFence,std::span<const VkSemaphore>) noexcept);
    // Read-only qualification under this mailbox lock. It must verify live XR
    // world/tracking, an independently completed stereo pair, retained capture
    // capacity and no active HUD. Must not reenter this GameTheater or submit.
    void onWorldPresentBypass(bool (*qualified)(VkQueue) noexcept);
    // Fresh world/no retained or recording HUD, but a stereo pair may still
    // need this present's semaphore dependency. After strict bypass declines,
    // consume only native waits, complete their fence and invoke completed_.
    // Does not copy the 2D image or touch retained theater ready/done signals.
    void onWorldPresentRetire(bool (*qualified)(VkQueue) noexcept);
    // Optional mono HUD producer. pending only reports commands successfully
    // submitted on this queue. source is called after all stock present waits
    // have completed (CPU-proven fence), and must retain its image through
    // completedPresent. Callbacks cannot reenter this GameTheater (its lock is held).
    void onPresentOverlay(bool (*pending)(VkQueue) noexcept,bool (*source)(VkQueue,VkFence,CompletedQuadImage&,std::span<const VkSemaphore>) noexcept);
    bool overlaySupported(VkDevice,VkExtent2D,VkFormat);
    // Exact metadata from the successful native swapchain creation/enumeration.
    // This does not acquire an image or authorize copying from an open pass.
    bool describePresentImage(VkDevice,VkImage,PresentImageInfo&);
    VkResult create(VkDevice,const VkSwapchainCreateInfoKHR*,const VkAllocationCallbacks*,VkSwapchainKHR*);
    void destroy(VkDevice,VkSwapchainKHR,const VkAllocationCallbacks*);
    VkResult present(VkQueue,const VkPresentInfoKHR*,PFN_vkQueuePresentKHR next=nullptr);
    // Called under XR private-queue lock. Returns the latest completed image;
    // repeated compositor frames reuse its released swapchain image.
    bool render(XrSession,VkQueue,XrSwapchain&,VkExtent2D&);
    // CompletedImage mode reuses the theater's tested single-image transfer.
    // Caller owns/serializes the game queue and retains this source through
    // return. The fence must cover the source's final write and closed pass.
    // RGBA/BGRA format defines color encoding; alpha is copied without tone mapping.
    // This mode still waits for its copy (completedSourceRetired contract).
    bool captureCompleted(VkQueue,const CompletedQuadImage&,std::uint64_t nowNs);
    // After a failed capture, retain its source until this returns true or
    // device teardown establishes completion. Failure is not cancellation.
    bool completedSourceRetired();
    bool renderCompleted(XrSession,VkQueue,std::uint64_t worldFrameId,std::uint64_t trackingEpoch,std::uint64_t spaceEpoch,std::uint64_t nowNs,XrSwapchain&,VkExtent2D&);
    void invalidateCompleted();
    void detachSession();
    void shutdown();
    std::string report();
};
}
