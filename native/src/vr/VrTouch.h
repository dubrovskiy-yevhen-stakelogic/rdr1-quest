#pragma once
// The XR thread's per-sample side channel next to the controller sample, and the SPIN hooks.
//
// XrSide = the OpenXR runtime's own velocities of both GRIP spaces
// (XrSpaceVelocity chained to the grip xrLocateSpace in ControllerActions::read: angular rad/s, linear m/s, in the base space = LOCAL)
// stamped with the sample's publishedNs; throwing::pushSample copies them into the XR tape when the stamps match (no division by dt for
// the FLICK / WAVE detectors). A velocity the runtime does not report VALID is simply absent (bit clear), the sample itself stays valid.
// SPIN adds the touch bits / counters / bindingCount here (touch::enabled() reads debug.rdrvr.vr.spin and .spin.touch itself).
// Threads: the XR thread writes and reads xrSide (ControllerActions::read -> XrHarness::locateHands -> throwing::pushSample, same thread);
// the atomics below are any-thread report words. No change to the mirror ControllerInput.h / ControllerActions.h (ODR trap).
#include <array>
#include <atomic>
#include <cstdint>
#if defined(__ANDROID__)
#include <sys/system_properties.h> // SPIN: touch::enabled()
#endif

namespace rdrvr::vr::touch {
struct XrSide {
    std::array<std::array<float,3>,2> w{},v{};   // per hand: angular (rad/s) and linear (m/s) velocity of the grip space, LOCAL
    std::uint32_t validBits{};                     // bit h = angular velocity of hand h VALID, bit 2+h = linear velocity VALID
    std::uint64_t publishedNs{};                   // == ControllerSample::publishedNs of the sample these belong to
};
inline XrSide xrSide{};                            // XR thread only
inline std::atomic<std::uint64_t> velocitySamples{},angularValid{},linearValid{};
// XR thread, ControllerActions::read: one hand's velocities (flags = XrSpaceVelocityFlags).
inline void noteVelocity(unsigned hand,std::uint64_t publishedNs,bool angularOk,const float* angular,bool linearOk,const float* linear) noexcept {
    if(hand>1)return;
    if(xrSide.publishedNs!=publishedNs){xrSide=XrSide{};xrSide.publishedNs=publishedNs;velocitySamples.fetch_add(1,std::memory_order_relaxed);}
    if(angularOk&&angular){xrSide.w[hand]={angular[0],angular[1],angular[2]};xrSide.validBits|=1u<<hand;angularValid.fetch_add(1,std::memory_order_relaxed);}
    if(linearOk&&linear){xrSide.v[hand]={linear[0],linear[1],linear[2]};xrSide.validBits|=4u<<hand;linearValid.fetch_add(1,std::memory_order_relaxed);}
}
// ---- SPIN: the trigger-touch side channel (XR thread -> input thread). ControllerActions
// creates `pad_trigger_touch` bound LAST to /user/hand/{left,right}/input/trigger/touch when enabled(); read() -> pending(); XrHarness, AFTER the
// VR-menu mask -> note(); VrSpin.h reads bits / ns. createActions retries the Touch suggestion without the touch bindings (dropTouchBindings).
inline constexpr std::uint32_t touchLeft=1u,touchRight=2u,touchActive=4u;
inline std::atomic<std::uint32_t> bits{};            // the last noted sample: bit h index on the trigger, bit 2 the touch action active (0 while masked)
inline std::atomic<std::uint64_t> ns{};              // its publishedNs (freshness for the input brain)
inline std::atomic<std::uint64_t> reads{},activeReads{},maskedReads{},touchedReads{};
inline std::atomic<std::uint32_t> bindingCount{};    // 2 when ControllerActions::create appended the two trigger/touch bindings (LAST), else 0
inline std::atomic<bool> bound{},retried{};          // bound = the Touch suggest WITH the touch bindings succeeded; retried = it failed -> re-suggested without
inline std::uint32_t xrPendingBits{};inline std::uint64_t xrPendingNs{}; // XR thread only (ControllerActions::read -> XrHarness)
inline constexpr std::uint32_t readBits(bool leftActive,bool leftState,bool rightActive,bool rightState) noexcept {
    return (leftActive&&leftState?touchLeft:0u)|(rightActive&&rightState?touchRight:0u)|((leftActive||rightActive)?touchActive:0u);}
// -1 = not read yet; 1 enabled; 0 = debug.rdrvr.vr.spin 0 or debug.rdrvr.vr.spin.touch 0.
inline std::atomic<int> enabledState{-1};
inline bool enabled() noexcept {
    const int s=enabledState.load(std::memory_order_acquire);if(s>=0)return s==1;
    bool on=true;
#if defined(__ANDROID__)
    {char v[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.vr.spin",v)>0&&v[0]=='0')on=false;}
    if(on){char v[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.vr.spin.touch",v)>0&&v[0]=='0')on=false;}
#endif
    enabledState.store(on?1:0,std::memory_order_release);return on;
}
// XR thread, ControllerActions::read (a failed read leaves 0 = "no touch", never invalidates the sample).
inline void pending(std::uint32_t b,std::uint64_t publishedNs) noexcept {xrPendingBits=b&7u;xrPendingNs=publishedNs;}
inline std::uint32_t pendingFor(std::uint64_t publishedNs) noexcept {return xrPendingNs==publishedNs?xrPendingBits:0u;}
// SPIN entry: with only the touch bit of the ONE
// sample of the release frame, and the index lifts / the grip analog lags while the hand opens -> every opening read "no finger". XR-rate
// per-hand history instead: the newest sample with the index on the trigger (touch), with the trigger analog in the light band, and with
// the grip still firmly closed (> gripHighLevel = holsters gripClose) - VrSpin's entry looks back over [grip last closed - 100 ms, now].
inline constexpr float lightLo=.03f,lightHi=.50f,gripHighLevel=.65f;
inline std::array<std::atomic<std::uint64_t>,2> touchOnNs{},lightOnNs{},gripHighNs{};
// The newest sample with the trigger analog above anyLo (the finger is ON the trigger, any depth)
inline constexpr float anyLo=.02f;
inline std::array<std::atomic<std::uint64_t>,2> trigAnyNs{};
inline std::atomic<std::uint64_t> lightReads{};
// XR thread, AFTER the VR menu mask: masked = the menu eats the input (bits forced to 0). triggers / grips = the sample's (masked) analogs.
inline void note(std::uint32_t b,std::uint64_t publishedNs,bool masked,const float* triggers=nullptr,const float* grips=nullptr) noexcept {
    const std::uint32_t v=masked?0u:(b&7u);
    reads.fetch_add(1,std::memory_order_relaxed);
    if(masked)maskedReads.fetch_add(1,std::memory_order_relaxed);
    if(v&touchActive)activeReads.fetch_add(1,std::memory_order_relaxed);
    if(v&(touchLeft|touchRight))touchedReads.fetch_add(1,std::memory_order_relaxed);
    if(!masked&&publishedNs){
        bool light=false;
        for(unsigned h=0;h<2;++h){
            if((v>>h)&1u)touchOnNs[h].store(publishedNs,std::memory_order_relaxed);
            if(triggers&&triggers[h]>=lightLo&&triggers[h]<=lightHi){lightOnNs[h].store(publishedNs,std::memory_order_relaxed);light=true;}
            if(triggers&&triggers[h]>anyLo)trigAnyNs[h].store(publishedNs,std::memory_order_relaxed);
            if(grips&&grips[h]>gripHighLevel)gripHighNs[h].store(publishedNs,std::memory_order_relaxed);
        }
        if(light)lightReads.fetch_add(1,std::memory_order_relaxed);
    }
    bits.store(v,std::memory_order_relaxed);ns.store(publishedNs,std::memory_order_release);
}
// XrHarness::createActions retry (spin R1): drop exactly the bindingCount bindings ControllerActions appended LAST.
template<class V> unsigned dropTouchBindings(V& bindings) noexcept {
    const unsigned n=bindingCount.load(std::memory_order_relaxed);unsigned k=0;
    while(k<n&&!bindings.empty()){bindings.pop_back();++k;}
    return k;
}
} // namespace rdrvr::vr::touch

namespace rdrvr::vr::spin {
using RigidFn=bool(*)(int hand) noexcept;
inline std::atomic<RigidFn> rigidFn{};
inline bool rigid(int hand) noexcept {const auto f=rigidFn.load(std::memory_order_acquire);if(!f)return true;try{return f(hand);}catch(...){return true;}}
inline std::atomic<bool> aimSuppressedFlag{};      // the HOLD hand is owned and not CATCHING / LEAVING: ForceAim not wanted (D12)
inline std::atomic<std::uint32_t> fingerMask{};    // bit h = the spin owns hand h (any Brain state but OFF): laser off, B masked, reload / hammer refused
// SPIN: rigid() - a hand the spin owns (ARMING .. LEAVING) is not rigid.
inline bool rigidFromMask(int hand) noexcept {return hand<0||hand>1||!((fingerMask.load(std::memory_order_acquire)>>unsigned(hand))&1u);}
} // namespace rdrvr::vr::spin
