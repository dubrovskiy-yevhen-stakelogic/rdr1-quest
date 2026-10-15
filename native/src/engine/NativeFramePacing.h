#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace rdrvr::engine {
// Replaces only the pinned Android display limiter while real-time gameplay is
// producing XR world frames. Native simulation clocks and presentation remain
// owned by the engine. An initial native baseline proves whether it was waiting.
class NativeFramePacing {
public:
    using Wait=void(*)(void* display);
    using Seconds=double(*)();
    using Sleep=int(*)(unsigned microseconds);
    using UpdateTime=void(*)(void* shell);
    struct Api {Wait wait{};Seconds seconds{};const std::uintptr_t* deviceSlot{};const double* origin{};const void* timeManager{};Sleep sleep{};UpdateTime updateTime{};};
    static constexpr unsigned baselineSamples=32;
    static constexpr unsigned maxHz=72;
    enum class State:std::uint8_t {Native,Baseline,Active72,TimerGuard,Swappy,Invalid,SleepFailure};
    struct Stats {
        bool ready{},installed{};std::uint64_t calls{},forwarded{},baseline{},bypassed{},positiveWait{},swappy{},fixed{},invalid{};
        std::uint64_t nativeWallNs{},baselineWallNs{},requestedWaitNs{};
        std::int64_t minFrameTimeUs{},lastWaitUs{};int timerMode{-1},useSwappy{-1};
        std::int64_t minSimulationStepUs{},maxRealStepUs{};int fixedOverride{-1},timerInitializing{-1};
        State state{State::Native};std::uint64_t pacedSleepCalls{},pacedWaitNs{},sleepFailures{};
        bool timeInstalled{};float timeScale{};std::uint64_t timeCalls{},timeGuarded{},timeSleepCalls{},timeWaitNs{},timeFailures{};
    };
    // All pointers refer to the exact pinned native image, qualified before the
    // GOT entry is published. These bindings remain valid for image lifetime.
    bool configure(Api)noexcept;
    bool ready()const noexcept;
    void installed(bool value)noexcept;
    void timeInstalled(bool value)noexcept;
    void invoke(void* display,bool qualifiedCaller,bool xrWorld)noexcept;
    void invokeTime(void* shell,bool qualifiedCaller,bool xrWorld)noexcept;
    Stats snapshot()const noexcept;
    std::string report()const;
private:
    Api api_{};std::atomic<bool> ready_{},installed_{};
    std::atomic<std::uint64_t> calls_{},forwarded_{},baseline_{},bypassed_{},positiveWait_{},swappy_{},fixed_{},invalid_{};
    std::atomic<std::uint64_t> nativeWallNs_{},baselineWallNs_{},requestedWaitNs_{};
    std::atomic<std::int64_t> minFrameTimeUs_{},lastWaitUs_{};
    std::atomic<int> timerMode_{-1},useSwappy_{-1};
    std::atomic<std::int64_t> minSimulationStepUs_{},maxRealStepUs_{};
    std::atomic<int> fixedOverride_{-1},timerInitializing_{-1};
    std::atomic<State> state_{State::Native};
    std::atomic<std::uint64_t> pacedSleepCalls_{},pacedWaitNs_{},sleepFailures_{};
    std::mutex pacingMutex_;
    void* pacedDisplay_{};std::uintptr_t pacedDevice_{};double pacedOrigin_{},lastReturn_{};
    std::atomic<bool> timeInstalled_{},timeFault_{};
    std::atomic<float> timeScale_{};
    std::atomic<std::uint64_t> timeCalls_{},timeGuarded_{},timeSleepCalls_{},timeWaitNs_{},timeFailures_{};
    std::mutex timeMutex_;void* timeOwner_{};double lastTimeReturn_{};
};
inline NativeFramePacing nativeFramePacing;
}
