#pragma once
#include "runtime/Haptics.h"
#include <cerrno>

namespace rdrvr::engine {
struct NativeRumbleApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};
};
// Called after the final SetActuatorValue(1,value) in sagRumbleFxManager::Update.
// Native disable/keyboard branches already produced zero for both actuators.
inline bool readNativeRumble(NativeRumbleApi api,std::uintptr_t pad,int actuator,float value,
                             std::array<float,2>& out)noexcept{
    const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
    try{
        if(!api.read||!pad||pad>UINTPTR_MAX-0x38||actuator!=1||!std::isfinite(value)||value<0||value>1)return false;
        int index{},again{};std::array<float,2> amplitudes{},repeat{};
        if(!api.read(api.context,pad+8,&index,sizeof(index))||index!=0||
           !api.read(api.context,pad+0x30,amplitudes.data(),sizeof(amplitudes))||
           !api.read(api.context,pad+0x30,repeat.data(),sizeof(repeat))||amplitudes!=repeat||
           !api.read(api.context,pad+8,&again,sizeof(again))||again!=index||amplitudes[1]!=value)return false;
        for(float a:amplitudes)if(!std::isfinite(a)||a<0||a>1)return false;
        out=amplitudes;return true;
    }catch(...){return false;}
}
class NativeRumbleBoundary {
public:
    using Original=void(*)(void*,int,float);
    using Observe=void(*)(void*,void*,int,float);
private:
    Original original_{};Observe observe_{};void* context_{};
    inline static thread_local bool inside_{};
    std::atomic<std::uint64_t> observed_{},failed_{},nested_{};
public:
    void configure(Original original,Observe observe,void* context=nullptr){original_=original;observe_=observe;context_=context;}
    void invoke(void* pad,int actuator,float value,bool pinned){
        const int saved=errno;const bool previous=inside_;inside_=true;
        struct Restore{bool value;~Restore(){inside_=value;}} restore{previous};
        if(previous)++nested_;
        errno=saved;original_(pad,actuator,value);
        const int nativeErrno=errno;
        if(!previous&&pinned&&pad&&observe_){try{observe_(context_,pad,actuator,value);++observed_;}catch(...){++failed_;}}
        errno=nativeErrno;
    }
    std::uint64_t observed()const{return observed_.load();}
    std::uint64_t failed()const{return failed_.load();}
    std::uint64_t nested()const{return nested_.load();}
};
}
