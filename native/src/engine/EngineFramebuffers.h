#pragma once
#include "EngineTargets.h"
#include "CommandResources.h"
#include <atomic>

namespace rdrvr::engine {
// Completed same-framebuffer eye requests without the mod's explicit null binding.
// Native Flush can still restart its pass for clears/barriers.
inline std::atomic<std::uint64_t> sameEyeFramebufferReuses{};
// Non-trivial return transport is intentional: the pinned AAPCS64 function
// returns IntrusivePtr<FrameBuffer> through x8, not x0. Ownership is discharged
// explicitly with the engine's IntrusivePtr destructor, never this destructor.
struct FramebufferRef {void* value{};~FramebufferRef() {}};
static_assert(sizeof(FramebufferRef)==sizeof(void*));
struct alignas(16) FramebufferDesc {std::array<std::byte,0xd0> bytes{};};
struct EngineRasterViewport {std::int32_t left{},right{},top{},bottom{};float minDepth{},maxDepth{};};
struct EngineRasterScissor {std::int32_t left{},right{},top{},bottom{};};
struct EngineRasterObservation {
    bool attempted{},available{},consistent{},fullViewport{},fullScissor{},beforeRestore{};
    EngineRasterViewport viewport{};
    EngineRasterScissor scissor{};
    VkViewport nativeViewport{};
    VkRect2D nativeScissor{};
};
static_assert(sizeof(EngineRasterViewport)==24 && sizeof(EngineRasterScissor)==16);
struct EngineFramebufferApi {
    void (*descriptor)(void*,const void*,const void*){};
    void (*destroyDescriptor)(void*){};
    FramebufferRef (*create)(void*,const void*){};
    void (*release)(void*){};
    std::uint32_t (*increment)(volatile std::uint32_t*){};
    bool (*bind)(void*,void*){}; // GfxContext (updates its cache and VkContext)
    bool (*bindVk)(void*,void*){}; // nullptr here really means no framebuffer
    void (*flush)(void*){};
    bool (*ownsThread)(){};
    PassSourceAbi memory{};
    std::uintptr_t deviceVtable{},gfxVtable{},framebufferVtable{},colorVtable{},depthVtable{},factoryDirty{};
    void (*viewport)(void*,const EngineRasterViewport&){};
    void (*scissor)(void*,const EngineRasterScissor&){};
    explicit operator bool() const {return descriptor && destroyDescriptor && create && release && increment && bind && bindVk && flush && ownsThread && memory.read && memory.contextVtable && deviceVtable && gfxVtable && framebufferVtable && colorVtable && depthVtable && factoryDirty && viewport && scissor;}
};
enum class NativeBindingFlush {NotPending,Flushed,Unavailable};
// Commit only the game's own deferred binding on its registered recording
// thread. No private framebuffer or guessed attachment is installed here.
NativeBindingFlush flushPendingNativeBinding(const EngineFramebufferApi&,CommandResources&,VkDevice,std::uint32_t,
                                            bool (*currentGfx)(void**),void (*factoryFlush)()) noexcept;

// Framebuffers are created by RDR from RDR-owned surfaces. No factory Lock/Unlock
// calls, global binding writes or direct Vulkan resource ownership occur here.
// Targets must outlive this owner. Preparing requires retired target commands;
// binding requires targets.beginUse(serial) before the first engine operation.
class EngineFramebuffers {
    friend class EnginePassBinding;
    EngineFramebufferApi api_{};EngineEyeTargets& targets_;void* device_{};
    std::array<void*,10> buffers_{}; // main, collector/light, optional water depth/refraction
    std::thread::id owner_{};
    std::uint64_t generation_{};
    bool scoped_{},poisoned_{};
    bool owner() const;
    bool release(void*&) noexcept;
    bool inspect(void*,unsigned) const;
    unsigned count() const {return targets_.waterReady()?10u:(targets_.sceneReady()?6u:targets_.eyeCount());}
    static unsigned index(unsigned,EngineTargetRole);
public:
    EngineFramebuffers(EngineFramebufferApi api,EngineEyeTargets& targets,void* device):api_(api),targets_(targets),device_(device){}
    ~EngineFramebuffers();
    EngineFramebuffers(const EngineFramebuffers&)=delete;
    EngineFramebuffers& operator=(const EngineFramebuffers&)=delete;
    bool prepare() noexcept;
    bool reset() noexcept;
    bool ready() const {return !poisoned_ && generation_ && generation_==targets_.generation() && targets_.ready() && buffers_[0] && (targets_.eyeCount()==1 || buffers_[1]);}
};

// Same-command-buffer scope with an explicit expected stock extent. It retains the original framebuffer
// and restores through GfxContext, preserving the factory's global state. This
// saves/restores raster rectangles through the engine API. It does not replay
// draw calls, change cameras, clear attachments or retire GPU use.
class EnginePassBinding {
    EngineFramebuffers& pair_;CommandResources& commands_;VkDevice device_{};std::uint32_t family_{};
    void* gfx_{};void* context_{};void* previous_{};
    CommandRecord command_{};
    EngineRasterViewport previousViewport_{},eyeViewport_{};
    EngineRasterScissor previousScissor_{},eyeScissor_{};
    int buffer_{-1};bool active_{},poisoned_{},touched_{};
    const char* status_{"NOT_PROBED"};
    EngineRasterObservation rasterObservation_{};
    bool beginImpl(void*,void*,bool probe) noexcept;
    bool currentCommand() const;
    bool state(void* desired,void* actual,bool open) const;
    bool rasterMatches(const EngineRasterViewport&,const EngineRasterScissor&) const;
    bool closeImpl();
public:
    EnginePassBinding(EngineFramebuffers& pair,CommandResources& commands,VkDevice device,std::uint32_t family):pair_(pair),commands_(commands),device_(device),family_(family){}
    ~EnginePassBinding();
    EnginePassBinding(const EnginePassBinding&)=delete;
    EnginePassBinding& operator=(const EnginePassBinding&)=delete;
    bool begin(void* gfx,void* vkContext) noexcept;
    bool prepareNative(void* gfx,void* vkContext,void (*factoryFlush)()) noexcept;
    bool probe(void* gfx,void* vkContext) noexcept; // no engine writes or retained references
    const char* status()const noexcept{return status_;}
    bool eye(unsigned,EngineTargetRole role=EngineTargetRole::Color) noexcept;
    bool matches(unsigned,EngineTargetRole role=EngineTargetRole::Color,bool dynamicScissor=false) const noexcept;
    bool finishDraw(unsigned,EngineTargetRole role=EngineTargetRole::Color) noexcept;
    bool flushDraw(unsigned,EngineTargetRole role=EngineTargetRole::Color) noexcept;
    // Diagnostic only: fixed-size guarded reads, no engine/GPU state changes.
    // The target route captures before its mandatory raster restoration.
    void resetRasterObservation() noexcept {rasterObservation_={};}
    void captureRasterObservation(bool beforeRestore=false) noexcept;
    const EngineRasterObservation& rasterObservation()const noexcept{return rasterObservation_;}
    bool close() noexcept;
    // Close the currently bound eye pass if any; true when nothing is bound.
    bool closeIfBound() noexcept;
    // Close the retained native main-color pass before borrowing its pixels as
    // a completed eye. Requires that exact target and a full-image raster.
    bool closeNativeColor(const void* target) noexcept;
    bool restore() noexcept;
    bool active() const {return active_;}
    bool poisoned() const {return poisoned_;}
    VkCommandBuffer command() const {return active_?command_.command:VK_NULL_HANDLE;}
    void* originalFramebuffer() const {return active_?previous_:nullptr;}
    void* framebuffer() const {return active_&&buffer_>=0?pair_.buffers_[buffer_]:nullptr;}
};
}
