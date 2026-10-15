#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>

namespace rdrvr::engine {
class ReticleUiCallBoundary {
public:
    using Original=void(*)(void*);
    using Observe=void(*)(void*,void*,bool);
private:
    Original original_{};Observe observe_{};void* context_{};
    inline static thread_local bool inside_{};
    std::atomic<std::uint64_t> entered_{},completed_{},failed_{},nested_{};
    void observe(void* object,bool complete)noexcept{
        try{observe_(context_,object,complete);if(complete)++completed_;else ++entered_;}
        catch(...){++failed_;}
    }
public:
    void configure(Original original,Observe observer,void* context=nullptr){original_=original;observe_=observer;context_=context;}
    void paint(void* object){
        const int incoming=errno;const bool previous=inside_;inside_=true;
        struct Restore{bool value;~Restore(){inside_=value;}} restore{previous};
        if(previous)++nested_;
        const bool observed=!previous&&object&&observe_;
        if(observed)observe(object,false);errno=incoming;
        original_(object);const int nativeErrno=errno;
        if(observed)observe(object,true);errno=nativeErrno;
    }
    std::uint64_t entered()const{return entered_.load();}
    std::uint64_t completed()const{return completed_.load();}
    std::uint64_t failed()const{return failed_.load();}
    std::uint64_t nested()const{return nested_.load();}
};
}
