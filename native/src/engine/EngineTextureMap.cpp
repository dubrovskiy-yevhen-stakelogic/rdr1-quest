#include "EngineTextureMap.h"
#include <cerrno>
// sampledDepthAliased (stereo/StereoReplay.cpp, debug.rdrvr.stereoreplay.sdalias): true from a replayed right depth stage that skipped its
// Depth -> SampledDepth copy until the frame end. Weak: a link without the stereo replay keeps the SampledDepth routing.
namespace rdrvr::engine::stereo {__attribute__((weak)) bool sampledDepthAliased() noexcept;}
namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
// Shader depth of this eye. Normally SampledDepth (attachment depth and shader depth deliberately differ); while the
// right SampledDepth was not refreshed this frame (sdalias), the right Depth itself, i.e. one image for both roles like the
// stock left eye (the replayed right tapes already sample it through the first bindless override).
EngineTargetRole shaderDepth(unsigned eye){
    return eye==1 && &stereo::sampledDepthAliased && stereo::sampledDepthAliased()?EngineTargetRole::Depth:EngineTargetRole::SampledDepth;}
constexpr EngineTargetRole roles[]{EngineTargetRole::Color,EngineTargetRole::Depth,EngineTargetRole::ShadowCollector,
    EngineTargetRole::LightAccumulation,EngineTargetRole::SampledDepth,EngineTargetRole::WaterDepth,EngineTargetRole::Refraction};
bool depthRole(EngineTargetRole role){return role==EngineTargetRole::Depth || role==EngineTargetRole::SampledDepth;}
EngineTargetRole normalized(EngineTargetRole role){return depthRole(role)?EngineTargetRole::Depth:role;}
bool supported(EngineTargetRole role){for(auto r:roles)if(r==role)return true;return false;}
bool compatible(const TargetImage& a,const TargetImage& b,const EngineEyeTargets& targets){return a.image.format==b.image.format &&
    targets.sourceExtentMatches(b.image.extent.width,b.image.extent.height) &&
    a.image.layers==1 && b.image.layers==1 && a.image.mips==1 && b.image.mips==1 &&
    a.image.samples==VK_SAMPLE_COUNT_1_BIT && b.image.samples==VK_SAMPLE_COUNT_1_BIT;}
}
bool EngineTextureMap::valid() const {
    return ownsThread_ && owner_==std::this_thread::get_id() && ownsThread_() && serial_ &&
        targets_.sceneReady() && targets_.pendingSerial()==serial_ && targets_.generation()==generation_;
}
bool EngineTextureMap::destination(EngineTargetRole role,const void** out,const TargetImage* original) const {
    const auto* known=targets_.image(eye_,role);const auto* target=targets_.target(eye_,role);TargetImage actual{};
    if(!known || !readTargetImage(images_,device_,memory_,reinterpret_cast<std::uintptr_t>(target),actual) ||
       actual.image.generation!=known->image.generation || actual.texture!=known->texture ||
       (original && (!compatible(actual,*original,targets_) || actual.image.image==original->image.image || actual.texture==original->texture)))return false;
    *out=target;return true;
}
bool EngineTextureMap::prepare(unsigned eye,std::span<const StockSceneTarget> stock) noexcept {
    Errno preserve;
    // Invalidate first: a rejected attempt must not leave an earlier eye active.
    if(owner_!=std::this_thread::get_id())return false;
    serial_=generation_=0;count_=0;
    try{
        if(!ownsThread_ || !ownsThread_() || !targets_.sceneReady() || !targets_.pending() || eye>=2 || stock.empty() || stock.size()>entries_.size())return false;
        std::array<Entry,8> next{};std::size_t count=0;eye_=eye;
        for(const auto& source:stock){
            if(!source.target || !supported(source.role))return false;
            TargetImage image{};const void* replacement{};const auto role=normalized(source.role);
            if(!readTargetImage(images_,device_,memory_,reinterpret_cast<std::uintptr_t>(source.target),image) ||
               !destination(role,&replacement,&image))return false;
            for(unsigned other=0;other<2;++other)for(auto r:roles){const auto* owned=targets_.image(other,r);
                if(!owned)continue; // optional water resources are absent in the current production profile
                if(source.target==targets_.target(other,r) || image.texture==owned->texture || image.image.image==owned->image.image)return false;
            }
            for(std::size_t j=0;j<count;++j){
                if(next[j].stock==source.target || next[j].original.texture==image.texture || next[j].original.image.image==image.image.image)
                    if(next[j].role!=role)return false;
            }
            next[count++]={source.target,role,image};
        }
        unsigned slot=0;
        for(unsigned other=0;other<2;++other)for(auto role:roles)
            owned_[slot++]={targets_.target(other,role),role,other};
        entries_=next;count_=count;generation_=targets_.generation();serial_=targets_.pendingSerial();return true;
    }catch(...){return false;}
}
bool EngineTextureMap::texture(const void* source,const void** out) const noexcept {
    Errno preserve;if(!out)return false;*out=source;
    try{
        if(!valid())return false;
        if(!source)return true;
        for(std::size_t i=0;i<count_;++i)if(entries_[i].stock==source){
            const auto& e=entries_[i];TargetImage now{};
            if(!readTargetImage(images_,device_,memory_,reinterpret_cast<std::uintptr_t>(source),now) ||
               now.image.generation!=e.original.image.generation || now.texture!=e.original.texture)return false;
            return destination(depthRole(e.role)?shaderDepth(eye_):e.role,out,&now);
        }
        // Engine callbacks may pass the mod's target on a subsequent call. Never let
        // the other eye escape into this eye's shader constants.
        for(const auto& owned:owned_)if(source==owned.target){
            if(owned.eye!=eye_)return false;
            return destination(depthRole(owned.role)?shaderDepth(eye_):owned.role,out);
        }
        return true; // ordinary assets and camera-independent shadow maps
    }catch(...){return false;}
}
bool EngineTextureMap::attachment(const void* source,bool depth,const void** out) const noexcept {
    Errno preserve;if(!out)return false;*out=source;
    try{
        if(!valid() || !source)return false;
        for(std::size_t i=0;i<count_;++i)if(entries_[i].stock==source){
            const auto& e=entries_[i];TargetImage now{};
            if(depthRole(e.role)!=depth || !readTargetImage(images_,device_,memory_,reinterpret_cast<std::uintptr_t>(source),now) ||
               now.image.generation!=e.original.image.generation || now.texture!=e.original.texture)return false;
            return destination(e.role,out,&now);
        }
        for(const auto& owned:owned_)if(owned.eye==eye_ && source==owned.target){
            if(depthRole(owned.role)!=depth)return false;return destination(depth?EngineTargetRole::Depth:owned.role,out);
        }
        return false; // unknown nested target needs an explicit pass policy
    }catch(...){return false;}
}
}
