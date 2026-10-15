#pragma once
#include "WeaponFireBoundary.h"
#include "NativeAimProbe.h"
namespace rdrvr::engine {
struct ProjectileLaunchCall {
    void* projectile{};const WeaponNativeMatrix* world{};int weaponType{};
    const AimNativeVector* velocity{};std::uint32_t actorGuid{};const AimNativeVector* muzzle{};
    bool nativeFlag{},finalFlag{};float scalar{};const AimNativeVector* delta{};
};
// Exact ARM64 Launch ABI: x0 object, x1 matrix, w2 type, x3 velocity, w4 GUID,
// x5 muzzle, w6/w7 bools, s0 scalar, and the final Vector3* at [entry sp].
// Read-only observation inside one already-pinned native FireProjectile call.
class ProjectileLaunchBoundary {
public:
    using Original=void(*)(void*,const WeaponNativeMatrix*,int,const AimNativeVector*,std::uint32_t,
                          const AimNativeVector*,bool,bool,float,const AimNativeVector*);
    using Observe=void(*)(void*,const ProjectileLaunchCall&,const WeaponFireScope&,bool);
private:
    Original original_{};Observe observe_{};void* context_{};inline static thread_local bool inside_{};
    std::atomic<std::uint64_t> entered_{},completed_{},failed_{},nested_{};
    void observe(const ProjectileLaunchCall& call,const WeaponFireScope& scope,bool complete)noexcept{
        try{observe_(context_,call,scope,complete);if(complete)++completed_;else ++entered_;}catch(...){++failed_;}
    }
public:
    void configure(Original original,Observe observer,void* context=nullptr){original_=original;observe_=observer;context_=context;}
    void launch(const ProjectileLaunchCall& call,bool pinned){
        const int incoming=errno;const bool previous=inside_;inside_=true;
        struct Restore{bool value;~Restore(){inside_=value;}} restore{previous};
        if(previous)++nested_;const auto scope=WeaponFireBoundary::scope();
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
};
}
