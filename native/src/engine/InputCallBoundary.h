#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
namespace rdrvr::engine {
// The original base Update owns button edges. Injection happens once before it,
// only from the pinned Android Update caller; dt and original exceptions survive.
class InputCallBoundary {
public:
    using Original=void(*)(void*,float);
    using Before=void(*)(void*,std::uint64_t) noexcept;
private:
    Original original_{};Before before_{},after_{};
    inline static thread_local bool inside_{};
    std::atomic<std::uint64_t> injected_{},nested_{},passed_{};
public:
    void configure(Original original,Before before,Before after=nullptr){original_=original;before_=before;after_=after;}
    void update(void* system,float dt,bool pinnedCaller,std::uint64_t now){
        const int incoming=errno;const bool previous=inside_;inside_=true;
        struct Restore{bool previous;~Restore(){inside_=previous;}} restore{previous};
        if(previous)++nested_;
        if(!previous&&pinnedCaller&&before_){before_(system,now);++injected_;}else ++passed_;
        errno=incoming;original_(system,dt);
        const int outgoing=errno;
        if(!previous&&pinnedCaller&&after_)after_(system,now);
        errno=outgoing;
    }
    std::uint64_t injected()const{return injected_.load();}
    std::uint64_t nested()const{return nested_.load();}
    std::uint64_t passed()const{return passed_.load();}
};
}
