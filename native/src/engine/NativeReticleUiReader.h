#pragma once
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace rdrvr::engine {
struct NativeReticleUiSample {
    float alpha{},previousAlpha{},accuracy{},x{},y{};
    std::int32_t value{},frame{};
    bool hasMovie{};
    bool operator==(const NativeReticleUiSample&)const=default;
};
// Borrowed only during the exact DuelReticule::OnPaint call. This does not
// invoke a UI getter, retain any native pointer, or mutate Flash/UI state.
class NativeReticleUiReader {
public:
    struct Api {
        bool (*read)(void*,std::uintptr_t,void*,std::size_t){};
        void* context{};std::uintptr_t reticleTable{},contextSetFloat{};
    };
private:
    Api api_;
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return api_.read&&p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&
            api_.read(api_.context,p+offset,&out,sizeof out);
    }
    struct State {
        std::uintptr_t table{},name{},context{},contextTable{},setFloat{},movie{};
        std::array<char,8> label{};NativeReticleUiSample sample{};
        bool operator==(const State&)const=default;
    };
    bool sample(std::uintptr_t object,State& s)const{
        if(!get(object,0,s.table)||s.table!=api_.reticleTable||!get(object,0x20,s.name)||
           !get(s.name,0,s.label)||s.label!=std::array<char,8>{'R','e','t','i','c','l','e',0}||
           !get(object,0x60,s.context)||!get(s.context,0,s.contextTable)||
           !get(s.contextTable,0x120,s.setFloat)||s.setFloat!=api_.contextSetFloat||
           !get(object,0x8c,s.sample.accuracy)||!get(object,0x90,s.sample.x)||!get(object,0x94,s.sample.y)||
           !get(object,0x98,s.movie)||!get(object,0xa0,s.sample.value)||!get(object,0xa4,s.sample.alpha)||
           !get(object,0xa8,s.sample.previousAlpha)||!get(object,0xac,s.sample.frame))return false;
        s.sample.hasMovie=s.movie!=0;
        for(float value:{s.sample.alpha,s.sample.previousAlpha,s.sample.accuracy,s.sample.x,s.sample.y})
            if(!std::isfinite(value))return false;
        // These are observed values, not visibility permission. Do not clamp
        // them or infer a successful Flash draw from a nonnull movie pointer.
        return true;
    }
public:
    explicit NativeReticleUiReader(Api api):api_(api){}
    bool inspect(std::uintptr_t object,NativeReticleUiSample& out)const noexcept{
        const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} restore{saved};
        try{
            if(!api_.reticleTable||!api_.contextSetFloat)return false;
            State a{},b{};if(!sample(object,a)||!sample(object,b)||a!=b)return false;
            out=a.sample;return true;
        }catch(...){return false;}
    }
};
}
