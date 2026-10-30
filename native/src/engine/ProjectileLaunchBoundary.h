#pragma once
#include "WeaponFireBoundary.h"
#include "NativeAimProbe.h"
#include <cstring>
namespace rdrvr::engine {
struct ProjectileLaunchCall {
    void* projectile{};const WeaponNativeMatrix* world{};int weaponType{};
    const AimNativeVector* velocity{};std::uint32_t actorGuid{};const AimNativeVector* muzzle{};
    bool nativeFlag{},finalFlag{};float scalar{};const AimNativeVector* delta{};
};
// Exact ARM64 Launch ABI: x0 object, x1 matrix, w2 type, x3 velocity, w4 GUID,
// x5 muzzle, w6/w7 bools, s0 scalar, and the final Vector3* at [entry sp].
// Read-only observation inside one already-pinned native FireProjectile call. One substitution point (THROWABLE, VrThrowable.h):
// adjust may replace the matrix / velocity / muzzle of the local player's thrown-weapon launch (copies; the native arguments are
// never written) - the observer then sees the substituted call, exactly what the engine got.
class ProjectileLaunchBoundary {
public:
    using Original=void(*)(void*,const WeaponNativeMatrix*,int,const AimNativeVector*,std::uint32_t,
                          const AimNativeVector*,bool,bool,float,const AimNativeVector*);
    using Observe=void(*)(void*,const ProjectileLaunchCall&,const WeaponFireScope&,bool);
    using Adjust=bool(*)(const ProjectileLaunchCall&,const WeaponFireScope&,WeaponNativeMatrix&,AimNativeVector&,AimNativeVector&);
private:
    Original original_{};Observe observe_{};void* context_{};Adjust adjust_{};inline static thread_local bool inside_{};
    std::atomic<std::uint64_t> entered_{},completed_{},failed_{},nested_{},adjusted_{};
    void observe(const ProjectileLaunchCall& call,const WeaponFireScope& scope,bool complete)noexcept{
        try{observe_(context_,call,scope,complete);if(complete)++completed_;else ++entered_;}catch(...){++failed_;}
    }
public:
    void configure(Original original,Observe observer,void* context=nullptr,Adjust adjust=nullptr){original_=original;observe_=observer;context_=context;adjust_=adjust;}
    void launch(const ProjectileLaunchCall& native,bool pinned){
        const int incoming=errno;const bool previous=inside_;inside_=true;
        struct Restore{bool value;~Restore(){inside_=value;}} restore{previous};
        if(previous)++nested_;const auto scope=WeaponFireBoundary::scope();
        ProjectileLaunchCall call=native;WeaponNativeMatrix world{};AimNativeVector velocity{},muzzle{};
        if(!previous&&pinned&&scope.serial&&scope.weapon&&native.projectile&&native.world&&native.velocity&&native.muzzle&&adjust_){
            std::memcpy(&world,native.world,sizeof world);std::memcpy(&velocity,native.velocity,sizeof velocity); // FireProjectile locals
            std::memcpy(&muzzle,native.muzzle,sizeof muzzle);
            bool changed=false;try{changed=adjust_(native,scope,world,velocity,muzzle);}catch(...){changed=false;}
            if(changed){call.world=&world;call.velocity=&velocity;call.muzzle=&muzzle;++adjusted_;}
            errno=incoming;
        }
        const bool observed=!previous&&pinned&&scope.serial&&scope.weapon&&call.projectile&&observe_;
        if(observed)observe(call,scope,false);errno=incoming;
        original_(call.projectile,call.world,call.weaponType,call.velocity,call.actorGuid,call.muzzle,
                  call.nativeFlag,call.finalFlag,call.scalar,call.delta);
        const int nativeErrno=errno;if(observed)observe(call,scope,true);errno=nativeErrno;
    }
    std::uint64_t entered()const{return entered_.load();}
    std::uint64_t completed()const{return completed_.load();}
    std::uint64_t failed()const{return failed_.load();}
    std::uint64_t nested()const{return nested_.load();}
    std::uint64_t adjusted()const{return adjusted_.load();}
};
}
