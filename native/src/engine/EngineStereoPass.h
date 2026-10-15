#pragma once
#include "EngineFramebuffers.h"
#include "EyeViewports.h"
#include "EngineDepthResolve.h"
#include "runtime/GameStereoCapture.h"
#include <mutex>
#include <string>

namespace rdrvr::engine {
enum class StereoPassResult {Skipped,Recorded,Cancelled,Quarantined};
enum class SceneStage {Depth,Lighting,Color};
// A qualified native main pass already rendered the real frozen left camera.
// Its target is still owned by the scene; the adapter copies it synchronously
// before any right-eye draw and verifies its image generation again.
struct NativeEyeSource {
    TargetImage color{};
    const void* viewport{};
    std::uint64_t sceneId{},xrFrameId{};
    std::uintptr_t depthTarget{}; // AppSW: engine depth target of the native left eye (0 = none)
};
// An audited caller supplies a camera-dependent draw body, NOT RenderFrame or
// an update callback. It must consume the same frozen simulation/culling snapshot
// for both calls, clear its eye attachments and preserve the binding/camera.
// The adapter handles cameras, color/depth binding, copies and GPU retirement.
class EngineStereoPass {
    EngineEyeTargets& targets_;EngineFramebuffers& buffers_;PassResources& images_;CommandResources& commands_;
    runtime::GameStereoCapture& capture_;
    PassSourceAbi memory_{};VkDevice device_{};std::uint32_t family_{};
    const char* status_{"NOT_ATTEMPTED"};
    std::uint64_t pending_{};bool poisoned_{},nativeEyeReused_{};
    struct RasterStage {
        std::uint64_t samples{},full{},clipped{},unavailable{},mismatched{},lastFrame{},lastClippedFrame{};
        EngineRasterObservation last{},lastClipped{};
    };
    mutable std::mutex rasterMutex_;
    std::array<RasterStage,6> rasterStages_{};
    void observeRaster(unsigned,SceneStage,std::uint64_t,const EngineRasterObservation&) noexcept;
    bool lastColorReplayed_{};
    // AppSW: optional depth blit of the eye just copied (transfer commands only; never fails the pass).
    void recordWarpDepth(VkCommandBuffer,unsigned eye,std::uintptr_t depthTarget) noexcept;
public:
    // Stereo replay hook: a RIGHT depth/color stage may be produced by
    // replaying recorded Vulkan commands into the closed command buffer.
    // handled=true means the hook fully produced the stage (ok = success).
    using ReplayStage=bool(*)(void* user,SceneStage,unsigned eye,EnginePassBinding&,bool& handled);
    static inline ReplayStage replayStage{};static inline void* replayUser{};
    using ProbeStage=void(*)(void* user,unsigned eye,bool begin);static inline ProbeStage probeStage{};
    // GPU stage markers: called at the start and end of every eye stage.
    using GpuStage=void(*)(unsigned eye,SceneStage,bool begin);static inline GpuStage gpuStage{};
    using Draw=bool(*)(void*,unsigned,const void* viewport,const void* color,const void* depth,EnginePassBinding&);
    using SceneDraw=bool(*)(void*,SceneStage,unsigned,const void* viewport,const EngineEyeTargets&,EnginePassBinding&);
    EngineStereoPass(EngineEyeTargets& targets,EngineFramebuffers& buffers,PassResources& images,CommandResources& commands,
                     runtime::GameStereoCapture& capture,PassSourceAbi memory,VkDevice device,std::uint32_t family):
        targets_(targets),buffers_(buffers),images_(images),commands_(commands),capture_(capture),memory_(memory),device_(device),family_(family){}
    EngineStereoPass(const EngineStereoPass&)=delete;
    EngineStereoPass& operator=(const EngineStereoPass&)=delete;
    bool retire() noexcept;
    StereoPassResult draw(VkQueue,EyeViewports&,const FrameContext&,runtime::SourceEncoding,
                          void* gfx,void* vkContext,Draw,void* user,const runtime::StereoReticle& reticle={},const NativeEyeSource* primary=nullptr) noexcept;
    // Depth/collector -> separate depth copy -> lighting -> main color, per eye.
    // Callbacks still need audited retail shader/resource/one-time-update routing.
    StereoPassResult drawScene(VkQueue,EyeViewports&,const FrameContext&,runtime::SourceEncoding,
                               void* gfx,void* vkContext,EngineDepthResolve&,SceneDraw,void* user,const runtime::StereoReticle& reticle={},const NativeEyeSource* primary=nullptr) noexcept;
    const char* status()const noexcept{return status_;}
    bool pending() const {return pending_!=0;}
    bool poisoned() const {return poisoned_;}
    bool nativeEyeReused() const {return nativeEyeReused_;}
    // Six private-eye stage endpoints; a borrowed native-left eye has no samples.
    std::string rasterReport()const;
};
}
