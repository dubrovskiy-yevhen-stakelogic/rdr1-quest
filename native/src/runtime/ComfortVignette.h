#pragma once
#include "XrPlatform.h"
#include <vector>
namespace rdrvr::runtime {
// HORSE VIGNETTE (vr/VrComfort.h): one static black ring mask (premultiplied alpha, clear centre) on a quad in VIEW space, submitted
// right after the world projection layer (the HUD and menu quads stay on top). The compositor draws it at the display rate with the
// current head pose, so the ring never lags the game's pairs and AppSW never warps it. The aperture is the quad's size alone: one
// upload at startup, nothing per frame. XR worker owns every operation; zero-timeout image wait and fence polling like PerfOverlay.
class ComfortVignette {
    VkDevice device_{};VkQueue queue_{};XrSwapchain swap_{};VkBuffer buffer_{};VkDeviceMemory memory_{};
    VkCommandPool pool_{};VkCommandBuffer command_{};VkFence fence_{};
    std::vector<XrSwapchainImageVulkan2KHR> images_;std::uint32_t index_{};
    bool acquired_{},submitted_{},valid_{};
    void releaseStaging() noexcept;
public:
    static constexpr std::uint32_t size=512;
    static constexpr float innerU=.2f,outerU=.4f; // Mask radius (quad half-size = 1): clear below innerU, black from outerU to the edge
    static constexpr float distance=2.5f;          // m in front of the head, behind the HUD quads
    void initialize(XrSession,const XrGraphicsBindingVulkan2KHR&);
    bool ready()const{return valid_;}
    // Finishes the one upload (non-blocking); then fills `out` for a ring whose clear half-angle has tangent clearTan. false = no layer.
    bool layer(XrSpace space,float clearTan,XrCompositionLayerQuad& out);
    // Only after the owning queue/device is idle, including teardown failures.
    void destroy() noexcept;
};
}
