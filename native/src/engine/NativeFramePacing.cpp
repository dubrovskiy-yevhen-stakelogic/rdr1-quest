#include "NativeFramePacing.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <sstream>

namespace rdrvr::engine {
namespace {
template<class T>T read(const void* p,std::size_t n=0)noexcept{T v{};std::memcpy(&v,static_cast<const unsigned char*>(p)+n,sizeof v);return v;}
constexpr auto relaxed=std::memory_order_relaxed;
std::uint64_t elapsedNs(double a,double b)noexcept{
    const double seconds=b-a;if(!std::isfinite(seconds)||seconds<=0||seconds>60)return 0;
    return std::uint64_t(seconds*1e9);
}
}
bool NativeFramePacing::configure(Api api)noexcept{
    if(ready()||!api.wait||!api.seconds||!api.deviceSlot||!api.origin||!api.timeManager||!api.sleep||!api.updateTime)return false;
    api_=api;ready_.store(true,std::memory_order_release);return true;
}
bool NativeFramePacing::ready()const noexcept{return ready_.load(std::memory_order_acquire);}
void NativeFramePacing::installed(bool value)noexcept{installed_.store(value&&ready(),std::memory_order_release);}
void NativeFramePacing::timeInstalled(bool value)noexcept{timeInstalled_.store(value&&ready(),std::memory_order_release);}
void NativeFramePacing::invoke(void* display,bool qualified,bool xrWorld)noexcept{
    const int incoming=errno;calls_.fetch_add(1,relaxed);
    // An unconfigured instance has no callable native target. The production
    // hook is never published before configure succeeds.
    if(!ready())return;
    // The two pinned callers are alternative owners of the same native display
    // deadline. Serialize the small per-frame pacing state across owner changes.
    const std::lock_guard<std::mutex> lock(pacingMutex_);
    auto forward=[&](bool baseline){
        pacedDisplay_=nullptr;pacedDevice_=0;lastReturn_=0;
        const auto start=api_.seconds();errno=incoming;api_.wait(display);const int saved=errno;
        const auto duration=elapsedNs(start,api_.seconds());forwarded_.fetch_add(1,relaxed);nativeWallNs_.fetch_add(duration,relaxed);
        if(baseline)baselineWallNs_.fetch_add(duration,relaxed);errno=saved;
    };
    if(!installed_.load(std::memory_order_acquire)||!timeInstalled_.load(std::memory_order_acquire)||!qualified||!display){state_.store(State::Native,relaxed);forward(false);return;}
    const auto device=read<std::uintptr_t>(api_.deviceSlot);
    if(!device||read<std::uintptr_t>(reinterpret_cast<const void*>(device),0x58)!=reinterpret_cast<std::uintptr_t>(display)){state_.store(State::Invalid,relaxed);invalid_.fetch_add(1,relaxed);forward(false);return;}
    const auto period=read<float>(display,0xa4);
    const auto swappy=read<unsigned char>(reinterpret_cast<const void*>(device),0x13d3);
    const auto mode=read<std::int32_t>(api_.timeManager,0x6c);
    const auto fixedOverride=read<unsigned char>(api_.timeManager,0x6a);
    const auto initializing=read<unsigned char>(api_.timeManager,0x68);
    const auto minStep=read<float>(api_.timeManager,0x58),maxRealStep=read<float>(api_.timeManager,0x50);
    const auto scale=read<float>(api_.timeManager,0x84);timeScale_.store(scale,relaxed);
    useSwappy_.store(swappy,relaxed);timerMode_.store(mode,relaxed);
    fixedOverride_.store(fixedOverride,relaxed);timerInitializing_.store(initializing,relaxed);
    minSimulationStepUs_.store(std::isfinite(minStep)&&minStep>=0&&minStep<=1?std::int64_t(double(minStep)*1e6):-1,relaxed);
    maxRealStepUs_.store(std::isfinite(maxRealStep)&&maxRealStep>=0&&maxRealStep<=1?std::int64_t(double(maxRealStep)*1e6):-1,relaxed);
    minFrameTimeUs_.store(std::isfinite(period)&&period>=0&&period<=1?std::int64_t(double(period)*1e6):-1,relaxed);
    if(swappy){state_.store(State::Swappy,relaxed);swappy_.fetch_add(1,relaxed);forward(false);return;}
    // Update also has a one-update reset path and a fixed-step override even
    // when mode is RealTime. Only the normal measured-time path is eligible.
    // Gameplay sets an 1/120-s minimum. A bounded 72-Hz display cadence remains
    // below that rate; unknown larger floors retain native pacing. Nothing in
    // the simulation clock or its clamps is changed.
    if(mode!=0||fixedOverride||initializing||maxRealStep!=0.f||(scale!=1.f&&scale!=0.9f)||!std::isfinite(minStep)||minStep<0||minStep>1.f/120||timeFault_.load(std::memory_order_acquire)){state_.store(State::TimerGuard,relaxed);fixed_.fetch_add(1,relaxed);forward(false);return;}
    if(!xrWorld){state_.store(State::Native,relaxed);forward(false);return;}
    // Bounds only qualify the replacement; unknown native configurations retain
    // their original behavior. No clamp or value is written to the cap setting.
    if(!std::isfinite(period)||period<0.001f||period>1.f){state_.store(State::Invalid,relaxed);invalid_.fetch_add(1,relaxed);forward(false);return;}
    const double origin=read<double>(api_.origin),now=api_.seconds()-origin,deadline=read<double>(display,0x260);
    if(!std::isfinite(origin)||!std::isfinite(now)||!std::isfinite(deadline)||now<0||deadline<0||std::abs(deadline-now)>60){state_.store(State::Invalid,relaxed);invalid_.fetch_add(1,relaxed);forward(false);return;}
    const auto positive=std::max(0.0,deadline-now);const auto waitNs=std::uint64_t(positive*1e9);
    lastWaitUs_.store(std::int64_t(positive*1e6),relaxed);requestedWaitNs_.fetch_add(waitNs,relaxed);
    if(positive>0)positiveWait_.fetch_add(1,relaxed);
    auto sample=baseline_.load(relaxed);
    while(sample<baselineSamples){if(baseline_.compare_exchange_weak(sample,sample+1,relaxed)){
        state_.store(State::Baseline,relaxed);forward(true);const int saved=errno;
        const auto returned=api_.seconds()-origin;
        if(std::isfinite(returned)&&returned>=now){pacedDisplay_=display;pacedDevice_=device;pacedOrigin_=origin;lastReturn_=returned;}
        errno=saved;return;
    }}
    if(pacedDisplay_!=display||pacedDevice_!=device||pacedOrigin_!=origin){pacedDisplay_=display;pacedDevice_=device;pacedOrigin_=origin;lastReturn_=now;}
    if(now<lastReturn_){state_.store(State::Invalid,relaxed);invalid_.fetch_add(1,relaxed);forward(false);return;}
    const auto target=lastReturn_+1.0/maxHz;
    double returned=now;
    unsigned attempts=0;
    state_.store(State::Active72,relaxed);
    while(returned<target){
        if(++attempts>64){state_.store(State::SleepFailure,relaxed);sleepFailures_.fetch_add(1,relaxed);forward(false);return;}
        // Round upward and re-read the native clock after every interruption.
        // Advancing from actual return prevents catch-up bursts after overruns.
        const auto micros=static_cast<unsigned>(std::ceil((target-returned)*1e6));
        pacedSleepCalls_.fetch_add(1,relaxed);const int result=api_.sleep(std::max(1u,micros));const int sleepError=errno;
        const auto after=api_.seconds()-origin;
        if(!std::isfinite(after)||after<returned){state_.store(State::Invalid,relaxed);invalid_.fetch_add(1,relaxed);forward(false);return;}
        pacedWaitNs_.fetch_add(elapsedNs(returned,after),relaxed);returned=after;
        if(result!=0&&sleepError!=EINTR){state_.store(State::SleepFailure,relaxed);sleepFailures_.fetch_add(1,relaxed);forward(false);return;}
    }
    // The original's only mutable output is its next deadline. Rebase it in the
    // same native clock domain instead of accumulating sleep debt while XR is
    // pacing presentation. Never alter minFrameTime, mode, dt or timescale.
    std::memcpy(static_cast<unsigned char*>(display)+0x260,&returned,sizeof returned);
    lastReturn_=returned;state_.store(State::Active72,relaxed);
    bypassed_.fetch_add(1,relaxed);errno=incoming;
}
void NativeFramePacing::invokeTime(void* shell,bool qualified,bool xrWorld)noexcept{
    const int incoming=errno;timeCalls_.fetch_add(1,relaxed);if(!ready())return;
    // Never share this mutex with presentation: UpdateTime precedes the native
    // render-thread wait, so a common mutex would create a cross-thread cycle.
    const std::lock_guard<std::mutex> lock(timeMutex_);
    auto forward=[&]{timeOwner_=nullptr;errno=incoming;api_.updateTime(shell);};
    if(!installed_.load(std::memory_order_acquire)||!timeInstalled_.load(std::memory_order_acquire)||!qualified||!shell||!xrWorld){forward();return;}
    const auto floor=read<float>(api_.timeManager,0x58),scale=read<float>(api_.timeManager,0x84);timeScale_.store(scale,relaxed);
    if(read<std::int32_t>(api_.timeManager,0x6c)!=0||read<unsigned char>(api_.timeManager,0x6a)||read<unsigned char>(api_.timeManager,0x68)||read<float>(api_.timeManager,0x50)!=0.f||(scale!=1.f&&scale!=0.9f)||!std::isfinite(floor)||floor<0||floor>1.f/120){forward();return;}
    auto now=api_.seconds();
    auto fail=[&]{timeFault_.store(true,std::memory_order_release);timeFailures_.fetch_add(1,relaxed);forward();};
    if(!std::isfinite(now)||now<0||(timeOwner_==shell&&now<lastTimeReturn_)){fail();return;}
    // The previous native sample precedes its return; the next sample follows
    // this guard. Using POST-to-PRE spacing therefore bounds the actual sample
    // gap without modifying any tick, accumulated time, dt, or scale field.
    // The microsecond margin covers the native float conversion at 120 Hz.
    const auto target=(timeOwner_==shell?lastTimeReturn_:now)+double(floor)/double(scale)+0.000001;
    unsigned attempts=0;
    while(now<target){
        if(++attempts>64){fail();return;}
        const auto micros=static_cast<unsigned>(std::ceil((target-now)*1e6));
        timeSleepCalls_.fetch_add(1,relaxed);const int result=api_.sleep(std::max(1u,micros));const int sleepError=errno;const auto after=api_.seconds();
        if(!std::isfinite(after)||after<now){fail();return;}
        timeWaitNs_.fetch_add(elapsedNs(now,after),relaxed);now=after;
        if(result!=0&&sleepError!=EINTR){fail();return;}
    }
    errno=incoming;api_.updateTime(shell);const int saved=errno;const auto after=api_.seconds();
    if(std::isfinite(after)&&after>=now){timeOwner_=shell;lastTimeReturn_=after;timeFault_.store(false,std::memory_order_release);timeGuarded_.fetch_add(1,relaxed);}
    else{timeOwner_=nullptr;timeFault_.store(true,std::memory_order_release);timeFailures_.fetch_add(1,relaxed);}
    errno=saved;
}
NativeFramePacing::Stats NativeFramePacing::snapshot()const noexcept{
    return {ready(),installed_.load(std::memory_order_acquire),calls_.load(relaxed),forwarded_.load(relaxed),baseline_.load(relaxed),bypassed_.load(relaxed),positiveWait_.load(relaxed),swappy_.load(relaxed),fixed_.load(relaxed),invalid_.load(relaxed),nativeWallNs_.load(relaxed),baselineWallNs_.load(relaxed),requestedWaitNs_.load(relaxed),minFrameTimeUs_.load(relaxed),lastWaitUs_.load(relaxed),timerMode_.load(relaxed),useSwappy_.load(relaxed),minSimulationStepUs_.load(relaxed),maxRealStepUs_.load(relaxed),fixedOverride_.load(relaxed),timerInitializing_.load(relaxed),state_.load(relaxed),pacedSleepCalls_.load(relaxed),pacedWaitNs_.load(relaxed),sleepFailures_.load(relaxed),timeInstalled_.load(std::memory_order_acquire),timeScale_.load(relaxed),timeCalls_.load(relaxed),timeGuarded_.load(relaxed),timeSleepCalls_.load(relaxed),timeWaitNs_.load(relaxed),timeFailures_.load(relaxed)};
}
std::string NativeFramePacing::report()const{
    const auto s=snapshot();std::ostringstream o;o<<"{\"ready\":"<<s.ready<<",\"installed\":"<<s.installed<<",\"calls\":"<<s.calls<<",\"forwarded\":"<<s.forwarded<<",\"baselineCalls\":"<<s.baseline<<",\"bypassed\":"<<s.bypassed<<",\"positiveDeadlineCalls\":"<<s.positiveWait<<",\"swappyCalls\":"<<s.swappy<<",\"fixedModeCalls\":"<<s.fixed<<",\"invalid\":"<<s.invalid<<",\"nativeWallMs\":"<<double(s.nativeWallNs)*1e-6<<",\"baselineWallMs\":"<<double(s.baselineWallNs)*1e-6<<",\"requestedWaitMs\":"<<double(s.requestedWaitNs)*1e-6<<",\"minFrameTimeUs\":"<<s.minFrameTimeUs<<",\"lastWaitUs\":"<<s.lastWaitUs<<",\"timerMode\":"<<s.timerMode<<",\"useSwappy\":"<<s.useSwappy<<",\"minSimulationStepUs\":"<<s.minSimulationStepUs<<",\"maxRealStepUs\":"<<s.maxRealStepUs<<",\"fixedOverride\":"<<s.fixedOverride<<",\"timerInitializing\":"<<s.timerInitializing<<",\"state\":"<<unsigned(s.state)<<",\"maxHz\":"<<maxHz<<",\"pacedSleepCalls\":"<<s.pacedSleepCalls<<",\"pacedWaitMs\":"<<double(s.pacedWaitNs)*1e-6<<",\"sleepFailures\":"<<s.sleepFailures<<",\"timeInstalled\":"<<s.timeInstalled<<",\"timeScale\":"<<(std::isfinite(s.timeScale)?std::to_string(s.timeScale):"null")<<",\"timeCalls\":"<<s.timeCalls<<",\"timeGuarded\":"<<s.timeGuarded<<",\"timeSleepCalls\":"<<s.timeSleepCalls<<",\"timeWaitMs\":"<<double(s.timeWaitNs)*1e-6<<",\"timeFailures\":"<<s.timeFailures<<'}';return o.str();
}
}
