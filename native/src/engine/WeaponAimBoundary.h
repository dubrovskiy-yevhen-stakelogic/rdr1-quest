#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cmath>
#include "WeaponFreeAimTarget.h"
namespace rdrvr::engine {
// Retail rage::Vector3 has a 16-byte stride, including an unspecified fourth
// lane. The hook passes it by pointer, never by value/HFA. Do not expose the
// mod's three-float runtime Vec3 directly as a retail argument.
struct alignas(16) WeaponAimVector {float x{},y{},z{},padding{};};
static_assert(sizeof(WeaponAimVector)==16&&alignof(WeaponAimVector)==16);
struct WeaponAimCall {
    void* weapon{};const WeaponAimVector* target{};bool targetFlag{};
    std::uint32_t targetGuid{};const void* intendedTarget{};int mode{};
    const WeaponAimVector* targetVelocity{};bool aimerFlag{};
    std::uint16_t instance{},part{};int targetIndex{};
};
class WeaponAimBoundary {
public:
    using Original=void(*)(void*,const WeaponAimVector*,bool,std::uint32_t,const void*,int,
        const WeaponAimVector*,bool,std::uint16_t,std::uint16_t,int);
    // The provider must freshly qualify owner, weapon class, native context,
    // tracking, target geometry and native tracked-shot/launch-override state.
    // A true result supplies a FREE aim point: all correlated target metadata
    // is reset together. The native aimer choice and original call frequency
    // remain unchanged. This does not clear persistent flags in the weapon;
    // the provider must decline incompatible tracked-shot state before enabling.
    using Prepare=bool(*)(void*,const WeaponAimCall&,WeaponAimVector&);
    using After=void(*)(void*,bool) noexcept;
private:
    Original original_{};Prepare prepare_{};After after_{};void* context_{};
    inline static thread_local bool inside_{};
    std::atomic<std::uint64_t> passed_{},prepared_{},nested_{},failed_{};
public:
    // Configure once before publication; no concurrent reconfiguration.
    void configure(Original original,Prepare prepare,void* context=nullptr,After after=nullptr){original_=original;prepare_=prepare;context_=context;after_=after;}
    void aim(const WeaponAimCall& call,bool pinnedCaller){
        const int incoming=errno;const bool previous=inside_;inside_=true;
        struct Restore{bool previous;~Restore(){inside_=previous;}} restore{previous};
        WeaponAimVector point{};const WeaponAimVector* target=call.target;
        WeaponFreeAimTarget freeTarget;
        if(previous)++nested_;
        if(!previous&&pinnedCaller&&prepare_&&call.weapon&&call.target){
            try {
                if(prepare_(context_,call,point)){
                    if(std::isfinite(point.x)&&std::isfinite(point.y)&&std::isfinite(point.z)){
                        target=&point;++prepared_;
                    }else ++failed_;
                }
            }catch(...){++failed_;}
        }
        if(target==call.target)++passed_;
        errno=incoming;
        // Original is deliberately outside the catch. Never swallow an engine
        // exception, and never retry after its firing/aim state has changed.
        if(target==call.target){
            original_(call.weapon,target,call.targetFlag,call.targetGuid,call.intendedTarget,call.mode,
                      call.targetVelocity,call.aimerFlag,call.instance,call.part,call.targetIndex);
        }else{
            original_(call.weapon,target,freeTarget.tracked,freeTarget.guid,freeTarget.intended.data(),freeTarget.mode,
                      nullptr,call.aimerFlag,freeTarget.instance,freeTarget.part,freeTarget.index);
        }
        const int nativeErrno=errno;
        if(!previous&&pinnedCaller&&after_)after_(context_,target!=call.target);
        errno=nativeErrno;
    }
    std::uint64_t passed()const{return passed_.load();}
    std::uint64_t prepared()const{return prepared_.load();}
    std::uint64_t nested()const{return nested_.load();}
    std::uint64_t failed()const{return failed_.load();}
};
}
