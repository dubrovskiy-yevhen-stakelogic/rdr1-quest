#pragma once
#include "GameStereoCapture.h"
#include "GameTheater.h"
namespace rdrvr::runtime {
// One retained mono image copied while the native HUD pass is closed. The
// original draw is never replayed. The copy's actual submit, not its CPU return,
// qualifies it for native present and the matching XR world frame.
class GameHudCapture {
    mutable std::mutex mutex_;
    VkDevice device_{};VkPhysicalDevice physical_{};std::uint32_t family_{};
    std::vector<VkQueue> queues_;VkQueue queue_{};
    std::thread::id owner_{};
    OwnedImage image_{};Extent extent_{};VkFormat format_{};FrameContext frame_{};
    VkCommandBuffer pass_{},copy_{};
    SubmitCompletion passCompletion_{},copyCompletion_{};
    bool accepting_{},active_{},poisoned_{},initialized_{},recorded_{},submitted_{},passSubmitted_{},passDiscarded_{},cancelled_{};
    std::uint64_t lastFrame_{},retiredFrame_{},generation_{},prepared_{},published_{},discarded_{},rejected_{},fenceWaits_{};
    bool covered(VkQueue,VkFence,std::span<const VkSemaphore>) const;
    bool completed(VkQueue,VkFence,std::span<const VkSemaphore>) const;
    CompletedQuadImage source(VkFence) const;
public:
    void bind(const XrGraphicsBindingVulkan2KHR&);
    bool beginFrame(VkQueue,const FrameContext&,Extent,VkFormat,VkCommandBuffer pass);
    // Caller owns the primary command, outside a render pass, on this family.
    bool record(VkCommandBuffer,const PassImage&);
    void cancelFrame(std::uint64_t) noexcept;
    void submitted(VkQueue,std::uint32_t,const VkSubmitInfo*,VkResult) noexcept;
    void commandInvalidated(VkCommandBuffer) noexcept;
    bool pendingOverlay(VkQueue) const noexcept;
    // Includes retained recordings not yet submitted and cancelled captures
    // still requiring a real completion. Unknown/unbound state is conservative.
    bool requiresPresent() const noexcept;
    // Borrow remains retained until completePresent has covered the overlay.
    bool overlaySource(VkQueue,VkFence,CompletedQuadImage&,std::span<const VkSemaphore> completedWaits={}) const noexcept;
    // The theater's present batch fence may still be pending; with
    // every other proof in place this waits for it (bounded, counted).
    static constexpr std::uint64_t fenceWaitNs=100'000'000;
    bool completePresent(VkQueue,VkFence,GameTheater& quad,std::uint64_t nowNs,std::span<const VkSemaphore> completedWaits={});
    std::uint64_t retiredFrame() const noexcept;
    void shutdownRetired();
    std::string report() const;
};
}
