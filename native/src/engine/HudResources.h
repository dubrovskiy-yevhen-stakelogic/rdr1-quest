#pragma once
#include "PassResources.h"
#include "CommandResources.h"

namespace rdrvr::engine {
struct HudResourceApi {
    PassSourceAbi memory{};
    bool (*ownsThread)(){};
    bool (*currentGfx)(void**){};
    std::uintptr_t gfxVtable{},framebufferVtable{},colorVtable{},factoryDirty{},depthVtable{};
    bool (*externalImage)(void*,VkDevice,VkImage,ImageRecord&){};void* externalContext{};
};
// A borrowed snapshot, valid only during the synchronous native draw boundary.
// Reading a framebuffer is not permission to copy from an open render pass.
struct HudResourceSet {
    std::uintptr_t gfx{},context{},framebuffer{},surface{},texture{},depth{};
    std::uintptr_t desired{},actual{},renderPass{};
    std::uint32_t width{},height{},pendingClears{},framebufferId{};
    CommandRecord command{};
    ImageRecord image{},depthImage{};
    std::uintptr_t depthTexture{};VkImageLayout layout{},depthLayout{};
    bool factoryDirty{},registered{},depthRegistered{},stencil{},external{};
};
enum class HudResourceStatus { Ready,UnregisteredImage,NotConfigured,WrongThread,InvalidBinding,InvalidCommand,InvalidImage };
const char* hudResourceStatusName(HudResourceStatus) noexcept;
class HudResources {
    HudResourceApi api_;PassResources& images_;CommandResources& commands_;
    VkDevice device_{};std::uint32_t family_{};
public:
    HudResources(HudResourceApi api,PassResources& images,CommandResources& commands,VkDevice device,std::uint32_t family)
        :api_(api),images_(images),commands_(commands),device_(device),family_(family){}
    // UnregisteredImage still provides object/binding metadata, but no format,
    // generation or usage claims. In particular WSI images must not be guessed.
    HudResourceStatus inspect(HudResourceSet&) noexcept;
    static bool sameImage(const HudResourceSet&,const HudResourceSet&) noexcept;
};
}
