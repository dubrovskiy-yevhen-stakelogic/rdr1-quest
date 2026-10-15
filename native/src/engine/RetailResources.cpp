#include "diagnostics/FrameTimings.h"
#include "RetailResources.h"
#include <cerrno>
#include <sstream>
namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
template<class T>bool read(const PassSourceAbi& api,std::uintptr_t base,std::size_t offset,T& out){
    return base&&offset<=UINTPTR_MAX-base&&sizeof(T)<=UINTPTR_MAX-(base+offset)&&api.read(api.context,base+offset,&out,sizeof(out));
}
std::uintptr_t address(const void* p){return reinterpret_cast<std::uintptr_t>(p);}
unsigned format(VkFormat f,bool& srgb){srgb=false;switch(f){
    case VK_FORMAT_B8G8R8A8_SRGB:srgb=true;return 2;
    case VK_FORMAT_B8G8R8A8_UNORM:return 2;
    case VK_FORMAT_R8G8B8A8_SRGB:srgb=true;return 26;
    case VK_FORMAT_R8G8B8A8_UNORM:return 26;
    case VK_FORMAT_R16G16B16A16_SFLOAT:return 7;
    default:return 0;
}}
}
bool RetailResources::inspect(const RetailSceneFrame& frame,RetailResourceSet& out,bool includeWater) noexcept {
    diagnostics::WallScope timing(diagnostics::frameTimings.resourceInspect);
    Errno preserve;status_="API_OR_FRAME";targetStatus_="NOT_READ";sourceIndex_=-1;observed_={};
    boundRead_=boundColorMatches_=boundDepthMatches_=false;
    try{
        if(!api_||!api_.ownsThread()||!frame.renderer||!frame.viewport||!frame.lights||!frame.shaft)return false;
        const auto& m=api_.memory;RetailResourceSet s{};void* lights{};std::uintptr_t table{};
        status_="FACTORY";
        if(!read(m,api_.factory,0,s.factory)||!read(m,address(s.factory),0,table)||table!=api_.factoryVtable)return false;
        status_="DEVICE";
        if(!read(m,api_.device,0,s.device)||!read(m,address(s.device),0,table)||table!=api_.deviceVtable)return false;
        status_="COLLECTOR";if(!read(m,api_.collector,0,s.collector)||!s.collector)return false;
        status_="POSTFX";if(!read(m,api_.postfx,0,s.postfx)||!s.postfx)return false;
        status_="LIGHTS";if(!read(m,api_.lights,0,lights)||lights!=frame.lights)return false;
        status_="TIMER";if(!read(m,api_.timer,0x5c,s.timerOrdinal))return false;
        status_="GFX";if(!api_.currentGfx(&s.gfx)||!read(m,address(s.gfx),0,table)||table!=api_.gfxVtable)return false;
        status_="CONTEXT";if(!read(m,address(s.gfx),0x128,s.context)||!read(m,address(s.context),0,table)||table!=m.contextVtable)return false;
        status_="FRAMEBUFFER";if(!read(m,address(s.gfx),0x158,s.framebuffer)||!read(m,address(s.framebuffer),0,table)||table!=api_.framebufferVtable)return false;
        void *color{},*collector{},*light{},*secondary{},*lightDepth{};
        status_="TARGET_POINTERS";
        if(!read(m,address(s.postfx),0x10,color)||!read(m,address(s.collector),0x40,collector)||
           !read(m,address(s.collector),0x48,s.depth)||!read(m,address(s.collector),0x50,secondary)||
           !read(m,address(lights),0x878,light)||!read(m,address(lights),0x9c0,lightDepth)||
           !read(m,address(lights),0x9c8,s.sampled)||!s.sampled||lightDepth!=s.depth)return false;
        const StockSceneTarget sources[]{{color,EngineTargetRole::Color},{s.depth,EngineTargetRole::Depth},
            {collector,EngineTargetRole::ShadowCollector},{light,EngineTargetRole::LightAccumulation},
            {s.sampled,EngineTargetRole::SampledDepth},{secondary,EngineTargetRole::SampledDepth}};
        for(const auto& source:sources){
            sourceIndex_=static_cast<int>(&source-sources);status_="NULL_TARGET";
            if(!source.target){if(&source==&sources[5])continue;return false;}
            status_="TARGET_IMAGE";
            TargetImage image{};if(!readTargetImage(images_,device_,m,address(source.target),image,&targetStatus_))return false;
            observed_[sourceIndex_]=image.image;status_="IMAGE_SHAPE";
            const auto& i=image.image;
            if(i.samples!=VK_SAMPLE_COUNT_1_BIT||i.layers!=1||i.mips!=1||!i.extent.width||!i.extent.height||
               i.extent.width>8192||i.extent.height>8192)return false;
            status_="EXTENT_MISMATCH";
            if(s.count&&(i.extent.width!=s.images[0].image.extent.width||i.extent.height!=s.images[0].image.extent.height))return false;
            s.stock[s.count]=source;s.images[s.count++]=image;
        }
        sourceIndex_=-1;status_="BOUND_SURFACES";
        void *boundColor{},*boundDepth{},*colorSurface{},*depthSurface{};
        if(!read(m,address(s.framebuffer),0x20,boundColor)||!read(m,address(s.framebuffer),0x18,boundDepth)||
           !read(m,address(color),0x70,colorSurface)||!read(m,address(s.depth),0xa0,depthSurface))return false;
        boundRead_=true;boundColorMatches_=boundColor&&boundColor==colorSurface;boundDepthMatches_=boundDepth&&boundDepth==depthSurface;
        if(!boundColorMatches_||!boundDepthMatches_)return false;
        bool colorSrgb{},collectorSrgb{};
        s.spec.width=s.images[0].image.extent.width;s.spec.height=s.images[0].image.extent.height;
        s.spec.colorFormat=s.images[0].image.format;s.spec.engineColorFormat=format(s.spec.colorFormat,colorSrgb);
        s.spec.depthFormat=s.images[1].image.format;s.spec.scene.collectorFormat=s.images[2].image.format;
        s.spec.scene.collectorEngineFormat=format(s.spec.scene.collectorFormat,collectorSrgb);
        s.spec.scene.lightFormat=s.images[3].image.format;s.spec.scene.lightEngineFormat=format(s.spec.scene.lightFormat,s.spec.scene.lightSrgb);
        status_="COLOR_FORMAT";if(!s.spec.engineColorFormat)return false;
        s.spec.colorSrgb=colorSrgb; // Retail LowPrecisionColorPass uses grc 2 with CreateParams +0x35.
        status_="COLLECTOR_FORMAT";if(!s.spec.scene.collectorEngineFormat||collectorSrgb)return false;
        status_="LIGHT_FORMAT";if(!s.spec.scene.lightEngineFormat)return false;
        status_="SAMPLED_DEPTH_FORMAT";
        for(unsigned i=4;i<s.count;++i)if(s.images[i].image.format!=s.spec.depthFormat)return false;
        if(includeWater){
            status_="WATER_RESOURCES";
            // RebuildRenderTargets names +550 "Depth Resolve" (grc 4/R32F),
            // +558 "Refraction RT". Both resize with the main scene extent.
            std::array<void*,2> water{},again{};
            if(!read(m,address(frame.renderer),0x550,water))return false;
            for(unsigned n=0;n<2;++n){
                TargetImage image{};
                if(!readTargetImage(images_,device_,m,address(water[n]),image))return false;
                const auto& i=image.image;
                if(i.extent.width!=s.spec.width || i.extent.height!=s.spec.height || i.layers!=1 || i.mips!=1 ||
                   i.samples!=VK_SAMPLE_COUNT_1_BIT || !(i.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) || !(i.usage&VK_IMAGE_USAGE_SAMPLED_BIT))return false;
                for(unsigned j=0;j<s.count;++j)if(s.stock[j].target==water[n] || s.images[j].texture==image.texture || s.images[j].image.image==i.image)return false;
                if(n==0){if(i.format!=VK_FORMAT_R32_SFLOAT)return false;}
                else{
                    s.spec.water.refractionFormat=i.format;
                    s.spec.water.refractionEngineFormat=format(i.format,s.spec.water.refractionSrgb);
                    if(!s.spec.water.refractionEngineFormat)return false;
                }
                if(s.count>=s.stock.size())return false;
                s.stock[s.count]={water[n],n?EngineTargetRole::Refraction:EngineTargetRole::WaterDepth};s.images[s.count++]=image;
            }
            if(!read(m,address(frame.renderer),0x550,again) || again!=water)return false;
            for(unsigned n=0;n<2;++n){TargetImage now{};const auto& captured=s.images[s.count-2+n];
                if(!readTargetImage(images_,device_,m,address(water[n]),now) || now.texture!=captured.texture ||
                   now.image.image!=captured.image.image || now.image.generation!=captured.image.generation)return false;
            }
        }
        out=s;status_="READY";return true;
    }catch(...){status_="EXCEPTION";return false;}
}
std::string RetailResources::report()const {
    std::ostringstream out;out<<"{\"state\":\""<<status_<<"\",\"targetState\":\""<<targetStatus_<<"\",\"sourceIndex\":"<<sourceIndex_<<",\"images\":[";
    for(unsigned i=0;i<observed_.size();++i){const auto& v=observed_[i];if(i)out<<',';
        out<<"{\"format\":"<<v.format<<",\"width\":"<<v.extent.width<<",\"height\":"<<v.extent.height
           <<",\"samples\":"<<v.samples<<",\"layers\":"<<v.layers<<",\"mips\":"<<v.mips<<'}';}
    out<<"],\"boundRead\":"<<boundRead_<<",\"boundColorMatches\":"<<boundColorMatches_<<",\"boundDepthMatches\":"<<boundDepthMatches_<<'}';return out.str();
}
const char* RetailResources::difference(const RetailResourceSet& a,const RetailResourceSet& b) noexcept {
    if(a.factory!=b.factory||a.device!=b.device||a.collector!=b.collector||a.postfx!=b.postfx)return "OWNER";
    if(a.gfx!=b.gfx||a.context!=b.context||a.framebuffer!=b.framebuffer)return "BINDING";
    if(a.depth!=b.depth||a.sampled!=b.sampled)return "DEPTH_POINTER";
    if(a.spec!=b.spec||a.count!=b.count)return "SHAPE";
    for(unsigned i=0;i<a.count;++i)if(a.stock[i].target!=b.stock[i].target||a.stock[i].role!=b.stock[i].role||
        a.images[i].texture!=b.images[i].texture||a.images[i].image.image!=b.images[i].image.image||a.images[i].image.generation!=b.images[i].image.generation)return "IMAGE_IDENTITY";
    // Check the global timer last: a concurrent tick must not conceal an
    // actual target or context change.
    return a.timerOrdinal==b.timerOrdinal?nullptr:"TIMER_ADVANCED";
}
bool RetailResources::same(const RetailResourceSet& a,const RetailResourceSet& b) noexcept {return difference(a,b)==nullptr;}
}
