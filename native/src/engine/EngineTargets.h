#pragma once
#include "PassResources.h"
#include <array>
#include <cstddef>
#include <thread>

namespace rdrvr::engine {
struct alignas(16) TargetParams {std::array<std::byte,0x70> bytes{};};
struct EngineTargetApi {
    void (*params)(void*,void*){};
    void* (*create)(void*,const char*,int,int,int,int,void*){};
    std::uint32_t (*decrement)(volatile std::uint32_t*){};
    void (*deleteTarget)(void*){}; // deleting D0, not D1/D2
    bool (*ownsThread)(){};
    std::uintptr_t factoryVtable{};
    PassSourceAbi memory{};
    explicit operator bool() const {return params && create && decrement && deleteTarget && ownsThread && factoryVtable && memory.read && memory.targetVtable && memory.textureVtable;}
};
struct EngineSceneTargetSpec {
    std::uint32_t collectorEngineFormat{},lightEngineFormat{};
    VkFormat collectorFormat{},lightFormat{};
    bool lightSrgb{};
    bool operator==(const EngineSceneTargetSpec&)const=default;
};
// WaterDepth is the R32F color target named "Depth Resolve" by the engine,
// not a depth/stencil attachment. Refraction has no depth attachment.
enum class EngineTargetRole {Color,Depth,ShadowCollector,LightAccumulation,SampledDepth,WaterDepth,Refraction};
struct EngineWaterTargetSpec {
    std::uint32_t refractionEngineFormat{};
    VkFormat refractionFormat{};
    bool refractionSrgb{};
    bool operator==(const EngineWaterTargetSpec&)const=default;
};
struct EngineTargetSpec {
    std::uint32_t width{},height{},engineColorFormat{};
    VkFormat colorFormat{},depthFormat{};
    EngineSceneTargetSpec scene{}; // all-zero disables intermediate colors/depth copies
    bool mono{},colorSrgb{},transparent{}; // mono/transparent are HUD-only; colorSrgb also covers retail LDR scenes
    EngineWaterTargetSpec water{}; // optional, requires scene; does not enable water draws
    // Explicit stock extent for a separately qualified eye-resolution route.
    // Zero/zero retains the original exact-size policy. Only scene targets may
    // opt in; this does not update viewport matrices or shader pixel constants.
    std::uint32_t sourceWidth{},sourceHeight{};
    bool operator==(const EngineTargetSpec&)const=default;
};
// Owns engine references, never the contained Vulkan handles. A pass owner must
// mark use BEFORE recording any command/binding, and retire that exact serial
// only after its commands are discarded or GPU completion has been established.
// No implicit GPU wait or binding mutation occurs here.
class EngineEyeTargets {
    EngineTargetApi api_{};PassResources& registry_;VkDevice device_{};void* factory_{};
    std::thread::id owner_{};
    // Keep the original color/depth indices; optional collector/light follow.
    static constexpr unsigned capacity=14;
    std::array<void*,capacity> targets_{};
    std::array<TargetImage,capacity> images_{};
    EngineTargetSpec spec_{};
    std::uint64_t generation_{},lastSerial_{},pending_{};
    bool poisoned_{};
    bool owner() const;
    bool release(void*) noexcept;
    bool discard(std::array<void*,capacity>&) noexcept;
    bool inspect(const std::array<void*,capacity>&,const EngineTargetSpec&,std::array<TargetImage,capacity>&);
    static unsigned index(unsigned eye,EngineTargetRole role);
public:
    EngineEyeTargets(EngineTargetApi api,PassResources& registry,VkDevice device,void* factory):api_(api),registry_(registry),device_(device),factory_(factory){}
    ~EngineEyeTargets();
    EngineEyeTargets(const EngineEyeTargets&)=delete;
    EngineEyeTargets& operator=(const EngineEyeTargets&)=delete;
    bool prepare(const EngineTargetSpec&) noexcept;
    bool beginUse(std::uint64_t serial) noexcept;
    bool retire(std::uint64_t serial) noexcept;
    bool reset() noexcept;
    bool ready() const {return generation_ && targets_[0] && !poisoned_;}
    bool pending() const {return pending_!=0;}
    std::uint64_t pendingSerial() const {return pending_;}
    std::uint64_t generation() const {return generation_;}
    unsigned eyeCount() const {return ready()?(spec_.mono?1u:2u):0u;}
    bool transparentMono() const {return ready()&&spec_.mono&&spec_.transparent;}
    bool sourceExtentMatches(std::uint32_t width,std::uint32_t height) const {
        return ready()&&width==(spec_.sourceWidth?spec_.sourceWidth:spec_.width)&&
            height==(spec_.sourceHeight?spec_.sourceHeight:spec_.height);
    }
    const void* target(unsigned eye,bool depth=false) const {return eye<eyeCount()?targets_[eye*2+unsigned(depth)]:nullptr;}
    const TargetImage* image(unsigned eye,bool depth=false) const {return eye<eyeCount()?&images_[eye*2+unsigned(depth)]:nullptr;}
    const void* target(unsigned eye,EngineTargetRole role) const;
    const TargetImage* image(unsigned eye,EngineTargetRole role) const;
    bool sceneReady() const {return ready() && targets_[4] && targets_[5] && targets_[6] && targets_[7] && targets_[8] && targets_[9];}
    bool waterReady() const {return sceneReady() && targets_[10] && targets_[11] && targets_[12] && targets_[13];}
};
}
