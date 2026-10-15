#pragma once
#include <array>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <utility>
namespace rdrvr::engine {
// Values are the pinned grceRenderState/D3D input enums, not Vulkan enums.
struct HudBlendState {
    unsigned source{},destination{},operation{},alphaSource{},alphaDestination{},alphaOperation{},colorMask{};
    bool separate{};
    bool operator==(const HudBlendState&) const=default;
};
struct HudAlphaApi {
    bool (*read)(void*,HudBlendState&){};void* context{};
    void (*set)(unsigned,int){};
    void (*flush)(){};
};
enum class HudAlphaResult {PassThrough,MaskOnly,Normalized,Unsupported,Quarantined};
// The retail Flush creates/binds the corrected native blend object. Restore
// its requested alpha values immediately afterwards through SetRenderState;
// that marks the native cache dirty for the next flush without changing RGB,
// stencil state or the default-state arrays. No private Vulkan binding occurs.
class HudAlphaFlush {
    HudAlphaApi api_;bool poisoned_{};
    inline static thread_local bool inside_{};
    bool restore(const HudBlendState& s) noexcept {
        bool good=true;
        const std::array<std::pair<unsigned,int>,5> states{{{24,int(s.alphaOperation)},{26,int(s.alphaSource)},
            {27,int(s.alphaDestination)},{25,int(s.separate)},{19,int(s.colorMask)}}};
        for(const auto& [key,value]:states)try{api_.set(key,value);}catch(...){good=false;}
        try{HudBlendState actual{};good=api_.read(api_.context,actual)&&actual==s&&good;}catch(...){good=false;}
        if(!good)poisoned_=true;return good;
    }
public:
    explicit HudAlphaFlush(HudAlphaApi api):api_(api){}
    HudAlphaResult invoke(bool eligible){
        if(!eligible||inside_||poisoned_||!api_.read||!api_.set){api_.flush();return poisoned_?HudAlphaResult::Quarantined:HudAlphaResult::PassThrough;}
        struct Scope {Scope(){inside_=true;}~Scope(){inside_=false;}} scope;
        const int incoming=errno;HudBlendState before{};bool readable=false;
        try{readable=api_.read(api_.context,before);}catch(...){}
        errno=incoming;
        if(!readable){api_.flush();return HudAlphaResult::Unsupported;}
        if(!before.colorMask){api_.flush();return HudAlphaResult::MaskOnly;}
        // Source factors that depend on destination color/alpha cannot be
        // separated from the world into one premultiplied RGBA texture.
        const bool independent=(before.source>=1&&before.source<=6)||before.source==14||before.source==15;
        if(!independent||before.operation!=1||(before.destination!=2&&before.destination!=6)||
           (before.colorMask!=7&&before.colorMask!=15)||before.alphaOperation<1||before.alphaOperation>5||
           before.alphaSource<1||before.alphaSource>15||before.alphaDestination<1||before.alphaDestination>15){
            api_.flush();return HudAlphaResult::Unsupported;
        }
        bool changed=false;
        try{
            // Normal: As + Ad*(1-As). Additive: preserve Ad, because the added
            // RGB contribution must not darken the original world underneath.
            api_.set(24,1);api_.set(26,before.destination==6?2:1);api_.set(27,before.destination);
            api_.set(25,1);api_.set(19,15);changed=true;
        }catch(...){}
        if(!changed){const bool restored=restore(before);errno=incoming;api_.flush();return restored?HudAlphaResult::Unsupported:HudAlphaResult::Quarantined;}
        errno=incoming;std::exception_ptr failure;
        try{api_.flush();}catch(...){failure=std::current_exception();}
        const int outgoing=errno;const bool restored=restore(before);errno=outgoing;
        if(failure)std::rethrow_exception(failure);
        return restored?HudAlphaResult::Normalized:HudAlphaResult::Quarantined;
    }
    bool poisoned() const {return poisoned_;}
};
}
