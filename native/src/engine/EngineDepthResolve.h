#pragma once
#include "EngineTargets.h"
#include "CommandResources.h"

namespace rdrvr::engine {
struct EngineDepthCopyApi {
    // Pinned VkTexture_c::CopyImageToImage: this is DESTINATION; its third
    // argument is SOURCE. Both textures finish in DEPTH_STENCIL_READ_ONLY.
    void (*copy)(void*,VkCommandBuffer,void*,VkImageCopy*){};
    bool (*ownsThread)(){};
    PassSourceAbi memory{};
    explicit operator bool() const {return copy && ownsThread && memory.read && memory.contextVtable;}
};
// Resolves to a separate per-eye image before shaders sample depth. The Android
// SetReadOnlyDepthHint merely marks a dirty bit; it is not proof that attaching
// and sampling the same depth image is valid. No Vulkan handle/layout ownership
// or engine shader binding is transferred to this adapter.
class EngineDepthResolve {
    EngineDepthCopyApi api_;EngineEyeTargets& targets_;PassResources& images_;CommandResources& commands_;
    VkDevice device_;std::uint32_t family_;std::thread::id owner_{std::this_thread::get_id()};
    bool poisoned_{};
    bool command(void*,VkCommandBuffer,CommandRecord&) const;
    bool image(unsigned,EngineTargetRole,TargetImage&) const;
public:
    EngineDepthResolve(EngineDepthCopyApi api,EngineEyeTargets& targets,PassResources& images,CommandResources& commands,
                       VkDevice device,std::uint32_t family):api_(api),targets_(targets),images_(images),commands_(commands),device_(device),family_(family){}
    bool copy(unsigned eye,void* vkContext,VkCommandBuffer expected) noexcept;
    bool poisoned() const {return poisoned_;}
};
}
