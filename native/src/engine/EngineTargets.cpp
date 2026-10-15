#include "EngineTargets.h"
#include <cerrno>
#include <cstring>
#include <limits>
namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
template<class T>void put(TargetParams& p,std::size_t offset,T value){std::memcpy(p.bytes.data()+offset,&value,sizeof(value));}
template<class T>bool read(const PassSourceAbi& a,const void* object,std::size_t offset,T& out){
    const auto base=reinterpret_cast<std::uintptr_t>(object);
    return base && offset<=UINTPTR_MAX-base && sizeof(T)-1<=UINTPTR_MAX-(base+offset) && a.read(a.context,base+offset,&out,sizeof(out));
}
VkFormat colorFormat(std::uint32_t grc,bool srgb){
    // Pinned CreateParams tables 0x12af148/0x12af1b0 and GetVkFormat.
    // Engine format 2 is BGRA, not RGBA; the enums are not interchangeable.
    switch(grc){
    case 2:return srgb?VK_FORMAT_B8G8R8A8_SRGB:VK_FORMAT_B8G8R8A8_UNORM;
    case 4:return srgb?VK_FORMAT_UNDEFINED:VK_FORMAT_R32_SFLOAT;
    case 7:return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 26:return srgb?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM;
    default:return VK_FORMAT_UNDEFINED;
    }
}
bool depth(VkFormat f){return f==VK_FORMAT_D16_UNORM || f==VK_FORMAT_D32_SFLOAT || f==VK_FORMAT_D24_UNORM_S8_UINT || f==VK_FORMAT_D32_SFLOAT_S8_UINT;}
unsigned count(const EngineTargetSpec& s){return s.mono?2u:(s.water.refractionEngineFormat?14u:(s.scene.collectorEngineFormat?10u:4u));}
bool depthIndex(unsigned i){return (i<4 && i%2) || (i>=8 && i<10);}
bool waterSpec(const EngineTargetSpec& s){
    if(s.water==EngineWaterTargetSpec{})return true;
    const auto& w=s.water;
    return !s.mono && s.scene.collectorEngineFormat && w.refractionFormat!=VK_FORMAT_UNDEFINED &&
        (w.refractionEngineFormat==2 || w.refractionEngineFormat==7 || w.refractionEngineFormat==26) &&
        colorFormat(w.refractionEngineFormat,w.refractionSrgb)==w.refractionFormat;
}
bool sceneSpec(const EngineSceneTargetSpec& s){
    if(s==EngineSceneTargetSpec{})return true;
    return s.collectorFormat!=VK_FORMAT_UNDEFINED && s.lightFormat!=VK_FORMAT_UNDEFINED &&
        colorFormat(s.collectorEngineFormat,false)==s.collectorFormat && colorFormat(s.lightEngineFormat,s.lightSrgb)==s.lightFormat;
}
VkFormat imageFormat(const EngineTargetSpec& s,unsigned i){
    if(depthIndex(i))return s.depthFormat;
    if(i<4)return s.colorFormat;
    if(i>=10)return i%2?s.water.refractionFormat:VK_FORMAT_R32_SFLOAT;
    return i%2?s.scene.lightFormat:s.scene.collectorFormat;
}
}
bool EngineEyeTargets::owner() const {return api_ && (owner_==std::thread::id{} || owner_==std::this_thread::get_id()) && api_.ownsThread();}
bool EngineEyeTargets::release(void* target) noexcept {
    if(!target)return true;
    try {
        std::uintptr_t table{};std::uint32_t references{};
        if(!read(api_.memory,target,0,table) || table!=api_.memory.targetVtable ||
           !read(api_.memory,target,0x10,references) || !references)return false;
        // Same sequence as grPostFX::DeleteRenderTargets. +0x14 is NOT this refcount.
        if(!api_.decrement(reinterpret_cast<volatile std::uint32_t*>(static_cast<std::byte*>(target)+0x10)))api_.deleteTarget(target);
        return true;
    }catch(...){return false;}
}
bool EngineEyeTargets::discard(std::array<void*,capacity>& targets) noexcept {
    bool ok=true;
    for(auto& p:targets){if(!p)continue;void* value=p;p=nullptr;if(!release(value))ok=false;}
    if(!ok)poisoned_=true; // Unknown ownership: quarantine; never retry a decrement.
    return ok;
}
bool EngineEyeTargets::inspect(const std::array<void*,capacity>& targets,const EngineTargetSpec& s,std::array<TargetImage,capacity>& result){
    for(unsigned i=0;i<count(s);++i){
        if(!readTargetImage(registry_,device_,api_.memory,reinterpret_cast<std::uintptr_t>(targets[i]),result[i]))return false;
        const auto& r=result[i].image;
        const bool isDepth=depthIndex(i);
        const auto attachment=isDepth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if(r.extent.width!=s.width || r.extent.height!=s.height || r.layers!=1 || r.mips!=1 || r.samples!=VK_SAMPLE_COUNT_1_BIT ||
           r.format!=imageFormat(s,i) || !(r.usage&attachment) ||
           (!isDepth && !(r.usage&VK_IMAGE_USAGE_SAMPLED_BIT)) ||
           (i<4 && !isDepth && !(r.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) ||
           (isDepth && s.scene.collectorEngineFormat && (!(r.usage&VK_IMAGE_USAGE_SAMPLED_BIT) ||
                !(r.usage&(i>=8?VK_IMAGE_USAGE_TRANSFER_DST_BIT:VK_IMAGE_USAGE_TRANSFER_SRC_BIT)))))return false;
        for(unsigned j=0;j<i;++j)if(targets[i]==targets[j] || result[i].texture==result[j].texture || r.image==result[j].image.image)return false;
    }
    return true;
}
bool EngineEyeTargets::prepare(const EngineTargetSpec& s) noexcept {
    Errno preserve;
    std::array<void*,capacity> next{};
    try {
        if(!owner() || !device_ || !factory_ || poisoned_ || pending_ || !s.width || !s.height || s.width>8192 || s.height>8192 ||
           (bool(s.sourceWidth)!=bool(s.sourceHeight)) || s.sourceWidth>8192 || s.sourceHeight>8192 ||
           (s.sourceWidth && (s.mono || !s.scene.collectorEngineFormat)) ||
           s.colorFormat==VK_FORMAT_UNDEFINED || colorFormat(s.engineColorFormat,s.colorSrgb)!=s.colorFormat || !depth(s.depthFormat) || !sceneSpec(s.scene) || !waterSpec(s) ||
           (s.mono && s.scene!=EngineSceneTargetSpec{}) || (!s.mono && s.transparent))return false;
        std::uintptr_t table{};
        if(!read(api_.memory,factory_,0,table) || table!=api_.factoryVtable)return false;
        owner_=std::this_thread::get_id();
        if(ready() && s==spec_){std::array<TargetImage,capacity> actual{};
            if(!inspect(targets_,s,actual))return false;
            for(unsigned i=0;i<count(s);++i)if(actual[i].image.generation!=images_[i].image.generation)return false;
            return true;
        }
        if(generation_==std::numeric_limits<std::uint64_t>::max())return false;
        constexpr const char* names[]{"rdrvr-left-color","rdrvr-left-depth","rdrvr-right-color","rdrvr-right-depth",
            "rdrvr-left-collector","rdrvr-left-light","rdrvr-right-collector","rdrvr-right-light","rdrvr-left-sampled-depth","rdrvr-right-sampled-depth",
            "rdrvr-left-water-depth","rdrvr-left-refraction","rdrvr-right-water-depth","rdrvr-right-refraction"};
        for(unsigned i=0;i<count(s);++i){
            const bool isDepth=depthIndex(i),isLight=i>=4 && i<8 && i%2,isRefraction=i>=10 && i%2;
            TargetParams p{};api_.params(&p,nullptr);
            put(p,0x4,std::uint32_t{0}); // no MSAA
            put(p,0xc,std::uint32_t{1}); // one mip
            put(p,0x18,std::uintptr_t{0}); // no parent/alias
            put(p,0x34,std::uint8_t{0}); // no display-driven size/DRS
            put(p,0x35,std::uint8_t((isLight && s.scene.lightSrgb) || (isRefraction && s.water.refractionSrgb) || (!isDepth && i<4 && s.colorSrgb)));
            put(p,0x38,isDepth?std::uint32_t{0}:(i>=10?(isRefraction?s.water.refractionEngineFormat:4u):
                (i<4?s.engineColorFormat:(isLight?s.scene.lightEngineFormat:s.scene.collectorEngineFormat))));
            put(p,0x48,std::uintptr_t{0}); // no external memory
            if(s.transparent && !isDepth)for(unsigned component=0;component<4;++component)put(p,0x50+component*sizeof(float),0.0f);
            if(i>=4 && i<8){
                // Retail collector starts white; light accumulation starts black.
                const float clear=isLight?0.0f:1.0f;
                for(unsigned component=0;component<4;++component)put(p,0x50+component*sizeof(float),clear);
            }
            put(p,0x66,std::uint8_t{0});
            const auto* name=s.mono?(isDepth?"rdrvr-hud-depth":"rdrvr-hud-color"):names[i];
            next[i]=api_.create(factory_,name,isDepth?2:3,int(s.width),int(s.height),32,&p);
            if(!next[i]){discard(next);return false;}
            // A duplicate factory return would mean two acquired references, so
            // discard still releases each acquisition exactly once.
        }
        std::array<TargetImage,capacity> actual{};
        if(!inspect(next,s,actual)){discard(next);return false;}
        if(!discard(targets_)){discard(next);return false;}
        targets_=next;images_=actual;spec_=s;++generation_;return true;
    }catch(...){discard(next);return false;}
}
bool EngineEyeTargets::beginUse(std::uint64_t serial) noexcept {
    Errno preserve;
    try {
        if(!owner() || !ready() || pending_ || !serial || serial<=lastSerial_)return false;
        std::array<TargetImage,capacity> actual{};
        if(!inspect(targets_,spec_,actual))return false;
        for(unsigned i=0;i<count(spec_);++i)if(actual[i].image.generation!=images_[i].image.generation)return false;
        pending_=lastSerial_=serial;return true;
    }catch(...){return false;}
}
bool EngineEyeTargets::retire(std::uint64_t serial) noexcept {
    Errno preserve;
    try{if(!owner() || !pending_ || pending_!=serial)return false;pending_=0;return true;}catch(...){return false;}
}
bool EngineEyeTargets::reset() noexcept {
    Errno preserve;
    try{if(!owner() || pending_ || poisoned_)return false;return discard(targets_);}catch(...){return false;}
}
EngineEyeTargets::~EngineEyeTargets(){reset();} // Pending/wrong-thread objects remain engine-owned.
unsigned EngineEyeTargets::index(unsigned eye,EngineTargetRole role){
    if(eye>=2)return capacity;
    switch(role){
    case EngineTargetRole::Color:return eye*2;
    case EngineTargetRole::Depth:return eye*2+1;
    case EngineTargetRole::ShadowCollector:return 4+eye*2;
    case EngineTargetRole::LightAccumulation:return 5+eye*2;
    case EngineTargetRole::SampledDepth:return 8+eye;
    case EngineTargetRole::WaterDepth:return 10+eye*2;
    case EngineTargetRole::Refraction:return 11+eye*2;
    }
    return capacity;
}
const void* EngineEyeTargets::target(unsigned eye,EngineTargetRole role) const {
    const unsigned i=index(eye,role);return eye<eyeCount() && i<capacity?targets_[i]:nullptr;
}
const TargetImage* EngineEyeTargets::image(unsigned eye,EngineTargetRole role) const {
    const unsigned i=index(eye,role);return eye<eyeCount() && i<capacity && targets_[i]?&images_[i]:nullptr;
}
}
