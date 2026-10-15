#pragma once
#include "VulkanBridge.h"
#include "XrPerformance.h"
#include "PerfOverlay.h"
#include "GraphicsMenu.h"
#include <memory>
#include "Policies.h"
#include "TrackedViews.h"
#include "StereoMailbox.h"
#include "ControllerActions.h"
#include "Presentation.h"
#include "harness/SceneRenderer.h"
#include <array>
#include <vector>
namespace rdrvr::runtime {
class GameTheater;
class XrHarness {
    struct Eye {
        XrSwapchain swapchain{};
        Extent extent{};
        std::vector<XrSwapchainImageVulkan2KHR> images;
        std::vector<harness::Target> targets;
        ImageCycle cycle;
    };
    XrInstance instance_{};
    XrSystemId system_{};
    XrSession session_{};
    XrSpace local_{},base_{},hudSpace_{};
    XrActionSet actions_{};
    XrAction aim_{},recenter_{};
    std::array<XrPath,2> handPaths_{};
    std::array<XrSpace,2> handSpaces_{};
    std::array<Eye,2> eyes_{};
    VulkanBridge gpu_;
    XrPerformance performance_;
    PerformanceSupport performanceSupport_;
    PerfOverlay perfOverlay_;bool perfOverlayEnabled_{};
    GraphicsMenu graphicsMenu_;
    std::unique_ptr<diagnostics::PerfPanel> perfPixels_;
    std::uint64_t perfStamp_{},perfFrame_{};
    harness::SceneRenderer renderer_;
    TrackingGate tracking_;
    std::uint64_t inputEpoch_{1};
    PressGate recenterButton_;
    XrSessionState state_{XR_SESSION_STATE_UNKNOWN};
    bool running_{},exit_{},focused_{},sessionLost_{};
    bool recenterRequested_{};
    bool borrowed_{};
    GameTheater* theater_{};
    GameTheater* hud_{};
    TrackedViewsMailbox* gameViews_{};
    StereoMailbox* stereo_{};
    ControllerMailbox* controllers_{};
    HapticMailbox* haptics_{};
    ControllerActions controllerActions_;
    PresentationMailbox* presentation_{};PresentationGate presentationGate_;
    std::uint64_t worldStereoLayers_{},hudLayers_{};
    const char* layerMode_{"NO_LAYER"};
    XrPosef theaterAnchor_{};
    bool anchored_{};
    std::uint64_t layerFrames_{},theaterFrames_{},shouldNotRender_{},invalidViews_{};
    XrViewStateFlags viewFlags_{};
    std::array<XrVector3f,2> lastPositions_{{{-0.032f,0,0},{0.032f,0,0}}};
    unsigned resubmitRun_{};std::uint64_t resubmitCompensated_{}; // Consecutive AppSW re-submits of one pair; pose-compensated ones
    std::uint64_t frame_{},pairs_{},spaceEpoch_{1};
    XrTime pendingSpaceChange_{};
    PFN_xrGetDisplayRefreshRateFB refresh_{};
    XrPath path(const char* text);
    void createActions();
    void createSwapchains();
    void resetSpace(XrTime time,bool toHead);
    std::array<Pose,2> locateHands(XrTime time);
public:
    XrHarness()=default;
    XrHarness(const XrHarness&)=delete;
    XrHarness& operator=(const XrHarness&)=delete;
    ~XrHarness();
    void initialize(JavaVM* vm,jobject activity);
    void initializeBorrowed(XrInstance,XrSystemId,const VulkanBootstrap&,GameTheater* = nullptr,TrackedViewsMailbox* = nullptr,StereoMailbox* = nullptr,ControllerMailbox* = nullptr,PresentationMailbox* = nullptr,GameTheater* hud = nullptr);
    void setPerformanceSupport(PerformanceSupport support){performanceSupport_=support;}
    void requestExit();
    void setHaptics(HapticMailbox* mailbox){haptics_=mailbox;}
    const HapticOutput& hapticOutput()const{return controllerActions_.hapticOutput();}
    const char* layerMode()const{return layerMode_;}
    std::uint64_t frames() const {return frame_;}
    std::uint64_t pairs() const {return pairs_;}
    std::uint64_t layers() const {return layerFrames_;}
    std::uint64_t theaterFrames() const {return theaterFrames_;}
    std::uint64_t worldStereoLayers() const {return worldStereoLayers_;}
    std::uint64_t hudLayers() const {return hudLayers_;}
    std::uint64_t shouldNotRender() const {return shouldNotRender_;}
    std::uint64_t invalidViews() const {return invalidViews_;}
    XrViewStateFlags viewFlags() const {return viewFlags_;}
    bool sessionCreated() const {return session_!=XR_NULL_HANDLE;}
    XrSessionState sessionState() const {return state_;}
    void events();
    void frame();
    void invalidateTracking();
    bool running() const { return running_; }
    bool exiting() const { return exit_; }
};
} // namespace rdrvr::runtime
