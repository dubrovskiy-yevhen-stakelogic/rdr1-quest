#pragma once
// MEMINFO: GetAvailableSystemMemoryBytes (0x1841b10, no arguments, returns bytes) fopen()s "/proc/meminfo" and fgets() it on
//   every call. Callers (all through the PLT -> JUMP_SLOT 0x2e145f8): VkDevice_c::IsDeviceLowOnMemory (bl 0x18aa03c, returns
//   below 100 MiB; grcDevice::BeginFrame calls it once per frame through VkDevice vtable +0x118), rage::CheckpointSystemMemory
//   (bl 0x1a21d28) and Mallocator_c::GetFreeBytes (tail b 0x185fe14: the return address is its caller's -> "other").
//   The cache answers from a value younger than 500 ms while that value is >= 512 MiB (the low-memory test fires at
//   100 MiB, far below the floor where caching stops). Kill switch: debug.rdrvr.meminfocache=0 (read once at install;
//   the hook then only forwards and counts).
// HYGIENE: occluderCaptureStartup is written by TraceEntry (T8) when the cold occluder capture is configured: 1 = OCCLUSION
//   was ON at startup (capture on), 0 = OFF (capture cache pointers passed as nullptr -> every capture hook returns at its
//   ready_ check). The report also shows whether the ablation/model-probe fast path (TraceEntry T3) is in effect.
#include "RenderAblation54.h"
#include "NativeModelProbe69.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#include <time.h>
#endif

#if defined(_MSC_VER)&&!defined(__clang__)
#include <intrin.h>
#ifndef RDRVR_EN_NOINLINE
#define RDRVR_EN_NOINLINE __declspec(noinline)
#define RDRVR_EN_RETURN_ADDRESS() reinterpret_cast<std::uintptr_t>(_ReturnAddress())
#endif
#else
#ifndef RDRVR_EN_NOINLINE
#define RDRVR_EN_NOINLINE __attribute__((noinline))
#define RDRVR_EN_RETURN_ADDRESS() reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))
#endif
#endif

namespace rdrvr::engine::enginetex {
// -1 = unset/empty/not a number, otherwise the decimal value of the system property. A switch is OFF only at "0".
inline int propertyValue(const char* name) noexcept {
#ifdef __ANDROID__
    const int saved=errno;char value[PROP_VALUE_MAX]{};const int n=__system_property_get(name,value);errno=saved;
    if(n<=0)return -1;
    int v=0;for(int i=0;i<n;++i){if(value[i]<'0'||value[i]>'9'||v>100000)return -1;v=v*10+(value[i]-'0');}
    return v;
#else
    (void)name;return -1;
#endif
}
struct AndroidProperty {int operator()(const char* name) const noexcept {return propertyValue(name);}};
inline std::uint64_t monotonicNs() noexcept {
#ifdef __ANDROID__
    timespec t{};if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return std::uint64_t(t.tv_sec)*1000000000ull+std::uint64_t(t.tv_nsec);
#else
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
}

namespace rdrvr::engine::meminfo {
// ---- librdr 1.58 arm64 offsets (module-relative) ----
inline constexpr std::uintptr_t functionOffset=0x1841b10;   // d11::Platform_c::GetAvailableSystemMemoryBytes()
inline constexpr std::uintptr_t gotOffset=0x2e145f8;        // R_AARCH64_JUMP_SLOT
inline constexpr std::array<unsigned char,48> fingerprint{
#include "gamecode/MeminfoCache_1841b10_48_8d.inc"
};
inline constexpr std::uintptr_t lowMemoryOffset=0x18aa034;  // d11::VkDevice_c::IsDeviceLowOnMemory() (bl at +8)
inline constexpr std::array<unsigned char,32> lowMemoryFingerprint{
#include "gamecode/MeminfoCache_18aa034_32_8d.inc"
};
inline constexpr std::uintptr_t lowMemoryReturn=0x18aa040;
inline constexpr std::uintptr_t checkpointCallOffset=0x1a21d28; // rage::CheckpointSystemMemory: bl GetAvailableSystemMemoryBytes@plt
inline constexpr std::array<unsigned char,4> checkpointCallFingerprint{130,216,71,148};
inline constexpr std::uintptr_t checkpointReturn=0x1a21d2c;
inline constexpr std::uint64_t maxAgeNs=500'000'000ull;
inline constexpr std::uint64_t floorBytes=512ull<<20;

enum Caller : unsigned {LowMemory,Checkpoint,Other,CallerCount};
inline constexpr const char* callerNames[CallerCount]{"lowMemory","checkpoint","other"};

// Host-testable cache. Clock()() returns monotonic ns (0 never a valid stamp), Fn()() is the original function.
template<class Clock,class Fn> class Cache {
    std::atomic<std::uint64_t> bytes_{},stamp_{};   // stamp_ 0 = never refreshed
    std::atomic<bool> refreshing_{};
    std::uintptr_t lowMemoryReturn_{},checkpointReturn_{};
public:
    Clock clock;Fn fn;
    std::atomic<bool> enabled{true};
    std::atomic<std::uint64_t> calls{},refreshes{},served{},contended{},floorBypass{},direct{},disabledCalls{};
    std::array<std::atomic<std::uint64_t>,CallerCount> byCaller{};
    std::atomic<std::uint64_t> lastBytes{},minBytes{UINT64_MAX};
    Cache() = default;
    Cache(Clock c,Fn f):clock(c),fn(f){}
    void configure(std::uintptr_t lowMemoryReturnAddress,std::uintptr_t checkpointReturnAddress,bool on) noexcept {
        lowMemoryReturn_=lowMemoryReturnAddress;checkpointReturn_=checkpointReturnAddress;enabled.store(on,std::memory_order_relaxed);
    }
    Caller classify(std::uintptr_t caller) const noexcept {
        return caller&&caller==lowMemoryReturn_?LowMemory:caller&&caller==checkpointReturn_?Checkpoint:Other;
    }
    std::uint64_t get(std::uintptr_t caller) noexcept {
        calls.fetch_add(1,std::memory_order_relaxed);byCaller[classify(caller)].fetch_add(1,std::memory_order_relaxed);
        if(!enabled.load(std::memory_order_relaxed)){disabledCalls.fetch_add(1,std::memory_order_relaxed);return note(fn());}
        const std::uint64_t now=clock();
        const std::uint64_t stamp=stamp_.load(std::memory_order_acquire);
        const std::uint64_t cached=bytes_.load(std::memory_order_relaxed);
        const bool valid=stamp!=0;
        const bool fresh=valid&&now>=stamp&&now-stamp<maxAgeNs;
        if(fresh&&cached>=floorBytes){served.fetch_add(1,std::memory_order_relaxed);return cached;}
        if(fresh)floorBypass.fetch_add(1,std::memory_order_relaxed); // below the floor: re-read on every call
        if(refreshing_.exchange(true,std::memory_order_acq_rel)){
            // Another thread is inside the original right now: answer with the last value unless it is low.
            if(valid&&cached>=floorBytes){contended.fetch_add(1,std::memory_order_relaxed);return cached;}
            direct.fetch_add(1,std::memory_order_relaxed);return note(fn());
        }
        const std::uint64_t value=fn();
        bytes_.store(value,std::memory_order_relaxed);
        std::uint64_t at=clock();if(!at)at=1;
        stamp_.store(at,std::memory_order_release);
        refreshing_.store(false,std::memory_order_release);
        refreshes.fetch_add(1,std::memory_order_relaxed);
        return note(value);
    }
    std::uint64_t note(std::uint64_t value) noexcept {
        lastBytes.store(value,std::memory_order_relaxed);
        auto low=minBytes.load(std::memory_order_relaxed);
        while(value<low&&!minBytes.compare_exchange_weak(low,value,std::memory_order_relaxed)){}
        return value;
    }
    void reset() noexcept {
        bytes_.store(0);stamp_.store(0);refreshing_.store(false);enabled.store(true);
        for(auto* a:{&calls,&refreshes,&served,&contended,&floorBypass,&direct,&disabledCalls,&lastBytes})a->store(0);
        for(auto& a:byCaller)a.store(0);
        minBytes.store(UINT64_MAX);
    }
};

using Original=std::uint64_t(*)();
inline Original original{};
struct CoarseClock {
    std::uint64_t operator()() const noexcept {
#ifdef __ANDROID__
        timespec t{};if(clock_gettime(CLOCK_MONOTONIC_COARSE,&t))return 0;
        return std::uint64_t(t.tv_sec)*1000000000ull+std::uint64_t(t.tv_nsec);
#else
        return enginetex::monotonicNs();
#endif
    }
};
struct CallOriginal {std::uint64_t operator()() const noexcept {return original?original():0;}};
inline Cache<CoarseClock,CallOriginal> cache;
inline std::atomic<bool> hooked{};

// GOT replacement for GetAvailableSystemMemoryBytes (any thread). errno is left as the caller had it.
RDRVR_EN_NOINLINE inline std::uint64_t hook() noexcept {
    const auto caller=RDRVR_EN_RETURN_ADDRESS();
    const int saved=errno;const std::uint64_t value=cache.get(caller);errno=saved;return value;
}

inline std::string report() {
    const auto& c=cache;std::ostringstream o;
    const auto mib=[](std::uint64_t b){return double(b)/double(1u<<20);};
    const auto low=c.minBytes.load();
    o<<"\"meminfo\":{\"hooked\":"<<hooked.load()<<",\"enabled\":"<<c.enabled.load()<<",\"calls\":"<<c.calls.load()
     <<",\"refreshes\":"<<c.refreshes.load()<<",\"served\":"<<c.served.load()<<",\"contended\":"<<c.contended.load()
     <<",\"floorBypass\":"<<c.floorBypass.load()<<",\"direct\":"<<c.direct.load()<<",\"disabledCalls\":"<<c.disabledCalls.load()
     <<",\"byCaller\":{";
    for(unsigned i=0;i<CallerCount;++i)o<<(i?",":"")<<'"'<<callerNames[i]<<"\":"<<c.byCaller[i].load();
    o<<"},\"lastMiB\":"<<mib(c.lastBytes.load())<<",\"minMiB\":";
    if(low==UINT64_MAX)o<<"null";else o<<mib(low);
    o<<",\"maxAgeMs\":"<<maxAgeNs/1000000<<",\"floorMiB\":"<<(floorBytes>>20)<<'}';
    return o.str();
}
}

namespace rdrvr::engine::hygiene {
// TraceEntry T8: -1 = the occluder capture configure() was never reached, 0 = OCCLUSION OFF at startup (capture gated), 1 = ON.
inline std::atomic<int> occluderCaptureStartup{-1};
inline std::string report() {
    const int startup=occluderCaptureStartup.load(std::memory_order_relaxed);
    const bool armed=experimental::renderAblation54.armed();
    const unsigned eyes=experimental::nativeModelProbe69.liveEyes();
    std::ostringstream o;
    o<<"\"hygiene\":{\"occluderCapture\":{\"startupOcclusion\":"<<startup<<",\"gated\":"<<(startup==0?1:0)<<'}'
     <<",\"ablationFastPath\":"<<(!armed&&!eyes?1:0)<<",\"ablationArmed\":"<<(armed?1:0)
     <<",\"ablationPropertySkips\":"<<experimental::renderAblation54.propertySkips()
     <<",\"modelProbeLiveEyes\":"<<eyes<<'}';
    return o.str();
}
}
