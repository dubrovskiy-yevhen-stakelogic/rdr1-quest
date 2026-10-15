#include "HudResources.h"
#include <cerrno>
namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
template<class T> bool read(const PassSourceAbi& api,std::uintptr_t base,std::size_t offset,T& out){
    return base && offset<=UINTPTR_MAX-base && sizeof(T)-1<=UINTPTR_MAX-(base+offset) &&
        api.read(api.context,base+offset,&out,sizeof(out));
}
bool table(const PassSourceAbi& api,std::uintptr_t p,std::uintptr_t expected){
    std::uintptr_t actual{};return p && !(p%8) && read(api,p,0,actual) && actual==expected;
}
bool colorFormat(VkFormat f){return f==VK_FORMAT_R8G8B8A8_UNORM || f==VK_FORMAT_R8G8B8A8_SRGB ||
    f==VK_FORMAT_B8G8R8A8_UNORM || f==VK_FORMAT_B8G8R8A8_SRGB || f==VK_FORMAT_R16G16B16A16_SFLOAT;}
}
const char* hudResourceStatusName(HudResourceStatus s) noexcept {
    switch(s){
    case HudResourceStatus::Ready:return "READY";
    case HudResourceStatus::UnregisteredImage:return "UNREGISTERED_IMAGE";
    case HudResourceStatus::NotConfigured:return "NOT_CONFIGURED";
    case HudResourceStatus::WrongThread:return "WRONG_THREAD";
    case HudResourceStatus::InvalidBinding:return "INVALID_BINDING";
    case HudResourceStatus::InvalidCommand:return "INVALID_COMMAND";
    case HudResourceStatus::InvalidImage:return "INVALID_IMAGE";
    }return "UNKNOWN";
}
HudResourceStatus HudResources::inspect(HudResourceSet& out) noexcept {
    Errno preserve;
    try{
        const auto& m=api_.memory;
        if(!device_ || !m.read || !m.contextVtable || !m.textureVtable || !api_.ownsThread || !api_.currentGfx ||
           !api_.gfxVtable || !api_.framebufferVtable || !api_.colorVtable || !api_.depthVtable || !api_.factoryDirty)return HudResourceStatus::NotConfigured;
        if(!api_.ownsThread())return HudResourceStatus::WrongThread;
        HudResourceSet s{};void* gfx{};std::uint32_t cachedId{},refs{};std::uint8_t dirty{};
        if(!api_.currentGfx(&gfx))return HudResourceStatus::InvalidBinding;
        s.gfx=reinterpret_cast<std::uintptr_t>(gfx);
        if(!table(m,s.gfx,api_.gfxVtable) || !read(m,s.gfx,0x128,s.context) || !table(m,s.context,m.contextVtable) ||
           !read(m,s.gfx,0x158,s.framebuffer) || !table(m,s.framebuffer,api_.framebufferVtable) ||
           !read(m,s.framebuffer,0xc,refs) || !refs || refs>=0x7fffffff ||
           !read(m,s.framebuffer,0x10,s.framebufferId) || !read(m,s.gfx,0x160,cachedId) || cachedId!=s.framebufferId ||
           !read(m,s.framebuffer,0xf8,s.width) || !read(m,s.framebuffer,0xfc,s.height) ||
           !s.width || !s.height || s.width>8192 || s.height>8192 ||
           !read(m,s.framebuffer,0x20,s.surface) || !table(m,s.surface,api_.colorVtable) ||
           !read(m,s.framebuffer,0x18,s.depth) || !read(m,s.context,0x30b8,s.desired) ||
           !read(m,s.context,0x30b0,s.actual) || !read(m,s.context,0x30a8,s.renderPass) ||
           !read(m,s.context,0x32a8,s.pendingClears) || !read(m,api_.factoryDirty,0,dirty))return HudResourceStatus::InvalidBinding;
        s.factoryDirty=dirty!=0;
        // This observer tolerates pending native bindings and clears, records
        // them explicitly, and never flushes or changes them to make a test pass.
        for(unsigned i=1;i<8;++i){std::uintptr_t extra{};
            if(!read(m,s.framebuffer,0x20+i*8,extra) || extra)return HudResourceStatus::InvalidBinding;
        }
        std::uintptr_t wrapper{},data{};VkCommandBuffer command{};VkImage image{};
        if(!read(m,s.context,0x30a0,wrapper) || !wrapper || wrapper%8 || !read(m,wrapper,0x38,command) ||
           !commands_.recording(device_,command,family_,s.command))return HudResourceStatus::InvalidCommand;
        std::uint32_t width{},height{},layout{};
        if(!read(m,s.surface,0x30,s.texture) || !table(m,s.texture,m.textureVtable) ||
           !read(m,s.texture,0x48,width) || width!=s.width || !read(m,s.texture,0x4c,height) || height!=s.height ||
           !read(m,s.texture,0x98,data) || !data || data%8 || !read(m,data,0x40,image) || !image ||
           !read(m,s.texture,0xa0,layout))return HudResourceStatus::InvalidImage;
        s.layout=static_cast<VkImageLayout>(layout);
        s.registered=images_.lookup(device_,image,s.image);
        if(!s.registered&&api_.externalImage){
            s.external=api_.externalImage(api_.externalContext,device_,image,s.image);s.registered=s.external;
        }
        if(s.registered){
            const auto& i=s.image;
            if(i.device!=device_||i.image!=image||!i.generation||i.extent.width!=s.width || i.extent.height!=s.height || i.extent.depth!=1 ||
               !colorFormat(i.format) || !(i.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
               i.samples!=VK_SAMPLE_COUNT_1_BIT || i.layers!=1 || i.mips!=1)return HudResourceStatus::InvalidImage;
        }else{s.image.image=image;s.image.device=device_;}
        if(s.depth){
            std::uintptr_t depthData{};VkImage depthImage{};std::uint32_t depthWidth{},depthHeight{},depthLayout{};
            if(!table(m,s.depth,api_.depthVtable)||!read(m,s.depth,0x30,s.depthTexture)||!table(m,s.depthTexture,m.textureVtable)||
               !read(m,s.depthTexture,0x48,depthWidth)||depthWidth!=s.width||!read(m,s.depthTexture,0x4c,depthHeight)||depthHeight!=s.height||
               !read(m,s.depthTexture,0x98,depthData)||!depthData||depthData%8||!read(m,depthData,0x40,depthImage)||!depthImage||depthImage==image||
               !read(m,s.depthTexture,0xa0,depthLayout))return HudResourceStatus::InvalidImage;
            s.depthLayout=static_cast<VkImageLayout>(depthLayout);s.depthRegistered=images_.lookup(device_,depthImage,s.depthImage);
            if(s.depthRegistered){const auto& d=s.depthImage;
                s.stencil=d.format==VK_FORMAT_D24_UNORM_S8_UINT||d.format==VK_FORMAT_D32_SFLOAT_S8_UINT;
                if((!s.stencil&&d.format!=VK_FORMAT_D16_UNORM&&d.format!=VK_FORMAT_D32_SFLOAT)||
                   d.extent.width!=s.width||d.extent.height!=s.height||d.extent.depth!=1||d.samples!=VK_SAMPLE_COUNT_1_BIT||d.layers!=1||d.mips!=1||
                   !(d.usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))return HudResourceStatus::InvalidImage;
            }else{s.depthImage.image=depthImage;s.depthImage.device=device_;}
        }
        out=s;return s.registered&&(!s.depth||s.depthRegistered)?HudResourceStatus::Ready:HudResourceStatus::UnregisteredImage;
    }catch(...){return HudResourceStatus::InvalidBinding;}
}
bool HudResources::sameImage(const HudResourceSet& a,const HudResourceSet& b) noexcept {
    return a.registered && b.registered && a.image.generation && a.command.recording &&
        a.gfx==b.gfx && a.context==b.context && a.framebuffer==b.framebuffer && a.framebufferId==b.framebufferId &&
        a.surface==b.surface && a.texture==b.texture && a.depth==b.depth && a.depthTexture==b.depthTexture &&
        (!a.depth||(a.depthRegistered&&b.depthRegistered&&a.depthImage.image==b.depthImage.image&&a.depthImage.generation==b.depthImage.generation)) &&
        a.external==b.external && a.width==b.width && a.height==b.height && a.image.device==b.image.device &&
        a.image.image==b.image.image && a.image.generation==b.image.generation &&
        a.command.command==b.command.command && a.command.generation==b.command.generation && a.command.recording==b.command.recording;
}
}
