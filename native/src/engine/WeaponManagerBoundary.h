#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
namespace rdrvr::engine {
// One ordinary manager update includes native aiming, firing, audio and FX.
// Observers cannot replace arguments or replay any of those effects.
class WeaponManagerBoundary {
public:
    using Original=void(*)(void*);
    using Observe=void(*)(void*,void*,bool);
private:
    Original original_{};Observe observe_{};void* context_{};
    inline static thread_local bool inside_{};
    std::atomic<std::uint64_t> entered_{},completed_{},failed_{},nested_{};
    void observe(void* weapon,bool complete)noexcept{
        try{observe_(context_,weapon,complete);}catch(...){++failed_;}
    }
public:
    void configure(Original original,Observe callback,void* context=nullptr){original_=original;observe_=callback;context_=context;}
    void update(void* weapon,bool pinned){
        const int incoming=errno;const bool previous=inside_;inside_=true;
        struct Restore{bool previous;~Restore(){inside_=previous;}} restore{previous};
        const bool eligible=!previous&&pinned&&weapon&&observe_;
        if(previous)++nested_;
        if(eligible){observe(weapon,false);++entered_;}
        errno=incoming;
        original_(weapon);
        const int nativeErrno=errno;
        if(eligible){observe(weapon,true);++completed_;}
        errno=nativeErrno;
    }
    std::uint64_t entered()const{return entered_.load();}
    std::uint64_t completed()const{return completed_.load();}
    std::uint64_t failed()const{return failed_.load();}
    std::uint64_t nested()const{return nested_.load();}
};
}
