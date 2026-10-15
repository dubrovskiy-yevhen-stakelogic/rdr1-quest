#pragma once
#include <cstdint>
namespace rdrvr::runtime {
// CPU gets one short boost window; menu/focus changes never re-arm it.
// The CPU-favor APK profile trades GPU headroom separately in the governor.
struct PerformancePolicy {
    static constexpr std::uint64_t boostNs=25'000'000'000ull;
    std::uint64_t expires{};bool used{},thermal{};
    int level(std::uint64_t now,bool world){
        if(world&&!used&&!thermal){used=true;expires=now+boostNs;}
        return expires&&now<expires&&!thermal?75:50;
    }
    static constexpr int gpuLevel() noexcept {return 75;} // stereo-replay: GPU-bound test (was 50)
    void warning(){thermal=true;expires=0;}
};
}
