#pragma once
#include "WeaponAttachmentReader.h"
#include <atomic>

namespace rdrvr::engine {
struct WeaponFireCall {
    void* weapon{};void* projectile{};int count{};const WeaponNativeMatrix* matrices{};
    float scalar{};int mode{};bool nativeFlag{};int nativeValue{};bool finalFlag{};
};
struct WeaponFireScope {std::uint64_t serial{};void* weapon{};};
// Observation only. The Matrix34 pointer can reference multiple pellets; it is
// borrowed for this call, never dereferenced, copied, retained or substituted.
class WeaponFireBoundary {
public:
    using Original=void(*)(void*,void*,int,const WeaponNativeMatrix*,float,int,bool,int,bool);
    using Observe=void(*)(void*,const WeaponFireCall&,bool);
private:
    Original original_{};Observe observe_{};void* context_{};
    inline static thread_local unsigned depth_{};
    inline static thread_local std::uint64_t nextSerial_{};
    inline static thread_local WeaponFireScope scope_{};
    inline static thread_local bool native_{};
    std::atomic<std::uint64_t> entered_{},completed_{},failed_{},nested_{};
    void observe(const WeaponFireCall& call,bool complete)noexcept{
        try{observe_(context_,call,complete);if(complete)++completed_;else ++entered_;}catch(...){++failed_;}
    }
public:
    void configure(Original original,Observe observer,void* context=nullptr){original_=original;observe_=observer;context_=context;}
    static WeaponFireScope scope(){return depth_==1&&native_?scope_:WeaponFireScope{};}
    void fire(const WeaponFireCall& call,bool pinned){
        const int saved=errno;const bool previous=depth_!=0;const auto parent=scope_;const bool parentNative=native_;++depth_;
        struct Restore{WeaponFireScope parent;bool native;~Restore(){scope_=parent;native_=native;--depth_;}} restore{parent,parentNative};
        if(!previous){scope_={};native_=false;
            if(pinned&&call.weapon&&nextSerial_!=UINT64_MAX)scope_={++nextSerial_,call.weapon};
        }
        if(previous)++nested_;
        const bool observed=!previous&&pinned&&call.weapon&&observe_;
        if(observed)observe(call,false);
        errno=saved;
        if(!previous)native_=true;
        original_(call.weapon,call.projectile,call.count,call.matrices,call.scalar,call.mode,call.nativeFlag,call.nativeValue,call.finalFlag);
        const int nativeErrno=errno;if(!previous)native_=false;if(observed)observe(call,true);errno=nativeErrno;
    }
    std::uint64_t entered()const{return entered_.load();}
    std::uint64_t completed()const{return completed_.load();}
    std::uint64_t failed()const{return failed_.load();}
    std::uint64_t nested()const{return nested_.load();}
};
}
