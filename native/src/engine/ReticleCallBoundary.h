#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
namespace rdrvr::engine {
// PostCameraUpdate calls Update once before UpdateAttack. Observations run only
// after that original call returns successfully, never on recursion or another caller.
class ReticleCallBoundary {
public:
    using Original=void(*)(void*,bool);
    using After=void(*)(void*) noexcept;
private:
    Original original_{};After after_{};
    inline static thread_local bool inside_{};
    std::atomic<std::uint64_t> observed_{},nested_{},passed_{};
public:
    void configure(Original original,After after){original_=original;after_=after;}
    // The native flag selects reticle behavior; it is not a head-tracking gate.
    void update(void* reticle,bool nativeFlag,bool pinnedCaller){
        const bool previous=inside_;inside_=true;
        struct Restore{bool previous;~Restore(){inside_=previous;}} restore{previous};
        if(previous)++nested_;
        original_(reticle,nativeFlag);
        const int nativeErrno=errno;
        if(!previous&&pinnedCaller&&reticle&&after_){after_(reticle);++observed_;}else ++passed_;
        errno=nativeErrno;
    }
    std::uint64_t observed()const{return observed_.load();}
    std::uint64_t nested()const{return nested_.load();}
    std::uint64_t passed()const{return passed_.load();}
};
}
