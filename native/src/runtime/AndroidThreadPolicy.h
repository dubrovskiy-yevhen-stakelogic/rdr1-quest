#pragma once
#include <openxr/openxr.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <sstream>
namespace rdrvr::runtime {
// Observed at the native input and scene boundaries, never guessed from names.
inline std::array<std::atomic<std::uint32_t>,2> nativeThreadIds{};
inline constexpr const char* androidThreadExtension="XR_KHR_android_thread_settings";
class AndroidThreadPolicy {
    struct Role {std::uint32_t observed{},accepted{};XrResult result{XR_ERROR_FUNCTION_UNSUPPORTED};unsigned attempts{};std::uint64_t next{};};
    std::array<Role,2> roles_{};
    std::function<XrResult(unsigned,std::uint32_t)> set_;
public:
    void configure(std::function<XrResult(unsigned,std::uint32_t)> setter){set_=std::move(setter);begin();}
    void begin(){roles_={};}
    void pump(std::uint64_t now){
        if(!set_)return;
        for(unsigned i=0;i<2;++i){auto& role=roles_[i];const auto tid=nativeThreadIds[i].load(std::memory_order_relaxed);
            if(tid!=role.observed){role={};role.observed=tid;}
            if(!tid||role.accepted==tid||now<role.next||role.attempts>=8)continue;
            ++role.attempts;role.next=now+1'000'000'000ull;
            role.result=set_(i,tid);if(XR_SUCCEEDED(role.result))role.accepted=tid;
        }
    }
    unsigned accepted()const{unsigned n=0;for(const auto& r:roles_)if(r.observed&&r.accepted==r.observed)++n;return n;}
    std::string report()const{
        std::ostringstream o;o<<"{\"available\":"<<bool(set_)<<",\"roles\":[";
        for(unsigned i=0;i<2;++i){const auto& r=roles_[i];if(i)o<<',';
            o<<"{\"role\":\""<<(i?"RENDERER_MAIN":"APPLICATION_MAIN")<<"\",\"tid\":"<<r.observed
             <<",\"acceptedTid\":"<<r.accepted<<",\"result\":"<<r.result<<",\"attempts\":"<<r.attempts<<'}';}
        return o.str()+"]}";
    }
};
}
