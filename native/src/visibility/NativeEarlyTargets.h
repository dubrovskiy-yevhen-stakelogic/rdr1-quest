#pragma once
#include "engine/PassResources.h"
#include "engine/NativeOcclusionDrawWitness.h"
#include "NativeEarlyImageRegistry.h"
#include <cstring>

namespace rdrvr::experimental {
// Resolve the surfaces from the EXACT stage-owned targets. In particular,
// RIGHT depth uses its ShadowCollector, not the retained native LEFT target.
// This is a metadata read under the caller's short native resource lease. It
// never binds targets, calls engine methods, or creates image ownership.
class NativeEarlyTargets {
public:
    using Address=std::uintptr_t;
    using Target=engine::NativeOcclusionDrawWitness::Target;
    struct Api {
        engine::PassResources* images{};VkDevice device{};engine::PassSourceAbi memory{};
        unsigned width{},height{};
        NativeEarlyImageRegistry::Cache2* cache{};
    };
    static bool capture(const Api& api,Address color,Address depth,Target& out,
                        const engine::TargetImage* expectedColor=nullptr,
                        const engine::TargetImage* expectedDepth=nullptr,
                        NativeEarlyImageRegistry::Ticket* receipt=nullptr)noexcept {
        out={};
        if(receipt)*receipt={};
        if(!api.images||!api.device||!api.memory.read||!api.width||!api.height||
           !color||!depth||color==depth||color>UINTPTR_MAX-0xa8||depth>UINTPTR_MAX-0xa8||
           bool(expectedColor)!=bool(expectedDepth)||(api.cache&&!receipt))return false;
        try {
            const auto ticket=api.cache?api.cache->issue(*api.images):NativeEarlyImageRegistry::Ticket{};
            if(api.cache&&!ticket)return false;
            engine::TargetImage c{},d{};
            if(!readImage(api,color,c,ticket)||!readImage(api,depth,d,ticket))return false;
            if(expectedColor&&(!same(c,*expectedColor)||!same(d,*expectedDepth)))return false;
            for(const auto* image:{&c.image,&d.image})
                if(image->device!=api.device||image->extent.width!=api.width||image->extent.height!=api.height||
                   image->extent.depth!=1||image->samples!=VK_SAMPLE_COUNT_1_BIT||image->layers!=1||
                   image->mips!=1||!image->generation)return false;
            if(c.image.image==d.image.image||d.image.format!=VK_FORMAT_D24_UNORM_S8_UINT||
               !(d.image.usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)||
               !(c.image.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))return false;
            Address cs{},ds{},ct{},dt{};
            auto read=[&](Address address,Address& result){return address&&address<=UINTPTR_MAX-sizeof result&&
                api.memory.read(api.memory.context,address,&result,sizeof result);};
            if(!read(color+0x70,cs)||!read(depth+0xa0,ds)||!cs||!ds||cs==ds||
               cs>UINTPTR_MAX-0x38||ds>UINTPTR_MAX-0x38||!read(cs+0x30,ct)||!read(ds+0x30,dt)||
               ct!=c.texture||dt!=d.texture)return false;
            if(api.cache&&!ticket.current())return false;
            out.colorSurfaces[0]=cs;out.depthSurface=ds;out.width=api.width;out.height=api.height;out.verified=true;
            if(receipt)*receipt=ticket;
            return true;
        }catch(...){return false;}
    }
private:
    static bool readImage(const Api& api,Address target,engine::TargetImage& out,
                          const NativeEarlyImageRegistry::Ticket& ticket) {
        if(!api.cache)return engine::readTargetImage(*api.images,api.device,api.memory,target,out);
        // Same pinned target/texture/data reads as readTargetImage. Only the
        // registry lookup is memoized; no native pointer or layout is reused.
        if(!api.memory.targetVtable||!api.memory.textureVtable||!target||target%8)return false;
        auto read=[&](Address p,auto& value){return p&&sizeof(value)<=UINTPTR_MAX-p&&
            api.memory.read(api.memory.context,p,&value,sizeof value);};
        auto field=[]<class T>(const auto& bytes,unsigned offset){T value{};std::memcpy(&value,bytes.data()+offset,sizeof value);return value;};
        std::array<std::byte,0xac> t{};std::array<std::byte,0xa4> textureBytes{};
        if(!read(target,t)||field.template operator()<Address>(t,0)!=api.memory.targetVtable)return false;
        const auto texture=field.template operator()<Address>(t,0x60);
        if(!texture||texture%8||!read(texture,textureBytes)||field.template operator()<Address>(textureBytes,0)!=api.memory.textureVtable)return false;
        const auto data=field.template operator()<Address>(textureBytes,0x98);
        VkImage image{};
        if(!data||data%8||data>UINTPTR_MAX-0x40||!read(data+0x40,image)||!image)return false;
        engine::ImageRecord record{};
        if(!api.cache->lookup(*api.images,api.device,image,record,ticket))return false;
        const auto width=field.template operator()<std::uint16_t>(t,0xa8),height=field.template operator()<std::uint16_t>(t,0xaa);
        if(record.extent.width!=width||record.extent.height!=height||
           field.template operator()<std::uint32_t>(textureBytes,0x48)!=width||
           field.template operator()<std::uint32_t>(textureBytes,0x4c)!=height)return false;
        out={record,static_cast<VkImageLayout>(field.template operator()<std::uint32_t>(textureBytes,0xa0)),target,texture};return true;
    }
    static bool same(const engine::TargetImage& a,const engine::TargetImage& b)noexcept {
        // Layout may advance legitimately during the stage. Identity and image
        // incarnation must be the exact retained EngineEyeTargets publication.
        return a.target==b.target&&a.texture==b.texture&&a.image.image==b.image.image&&
            a.image.device==b.image.device&&a.image.generation==b.image.generation;
    }
};
}
