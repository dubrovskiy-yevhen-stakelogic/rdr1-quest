#pragma once
// Short VR haptic pulses (holster enter / grab / return / refusal) for the Touch controllers.
//
// Any thread posts pulse(hand, amplitude, duration); the XR thread (XrHarness::locateHands, the only caller of
// xrApplyHapticFeedback) calls xrFrame(now) once per synced frame, right after the existing native-rumble
// ControllerActions::outputHaptics(), and applies frame.start[h] with ControllerActions' haptic action.
// While a VR pulse is active on a hand the native rumble for that hand is masked to 0 for the frame (otherwise the
// next 30 ms native pulse would replace ours: an OpenXR apply replaces the running vibration of that action/hand).
// Lock-free (bounded MPMC ring), stale pulses (> maxAgeNs since posting) are dropped, never queued across menus.
#include "VrMainThread.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rdrvr::vr::haptics {
enum Source : std::uint16_t {SourceOther=0,SourceSlotEnter=1,SourceGrab=2,SourceReturn=3,SourceRefuse=4,
    SourceMeleeHit=5,SourceMeleeKill=6,SourceParry=7,
    SourceReload=8,SourceHammer=9,SourceSpin=10,SourceTeleport=11,SourceLasso=12,
    SourceCount=13};
inline constexpr std::uint16_t sourceSlots=16; // bySource size
inline constexpr std::array<Source,SourceCount> allSources{SourceOther,SourceSlotEnter,SourceGrab,SourceReturn,SourceRefuse,SourceMeleeHit,
    SourceMeleeKill,SourceParry,SourceReload,SourceHammer,SourceSpin,SourceTeleport,SourceLasso};
inline constexpr bool sourceIdsUnique() noexcept {
    for(std::size_t i=0;i<allSources.size();++i){if(allSources[i]>=sourceSlots)return false;
        for(std::size_t j=i+1;j<allSources.size();++j)if(allSources[i]==allSources[j])return false;}
    return true;
}
static_assert(sourceIdsUnique(),"haptic source ids unique and < bySource size");
struct Pulse {std::uint64_t postedNs{};float amplitude{};std::uint32_t durationUs{};std::uint8_t hand{};std::uint16_t source{};};
// Design values; refuse is a weak, long buzz.
inline constexpr float enterAmplitude=.25f,grabAmplitude=.6f,returnAmplitude=.4f,refuseAmplitude=.15f;
inline constexpr std::uint32_t enterUs=15'000,grabUs=40'000,returnUs=25'000,refuseUs=60'000;
inline constexpr std::uint64_t maxAgeNs=100'000'000;

inline LockFreeQueue<Pulse,32> queue{};
inline std::atomic<bool> enabled{true};
inline std::atomic<std::uint64_t> posted{},dropped{},started{},stale{},merged{},suppressed{};
inline std::array<std::atomic<std::uint64_t>,sourceSlots> bySource{};

inline bool pulse(unsigned hand,float amplitude,std::uint32_t durationUs,std::uint64_t now=0,std::uint16_t source=SourceOther) noexcept {
    if(!enabled.load(std::memory_order_relaxed)||hand>1||!std::isfinite(amplitude)||amplitude<=0||!durationUs){dropped.fetch_add(1,std::memory_order_relaxed);return false;}
    Pulse p{now?now:nowNs(),std::fmin(amplitude,1.f),durationUs>1'000'000u?1'000'000u:durationUs,std::uint8_t(hand),source};
    if(!queue.push(p)){dropped.fetch_add(1,std::memory_order_relaxed);return false;}
    posted.fetch_add(1,std::memory_order_relaxed);if(source<bySource.size())bySource[source].fetch_add(1,std::memory_order_relaxed);return true;
}

struct Frame {
    std::array<bool,2> start{},active{};          // start: apply now; active: a VR pulse owns the hand (mask native rumble)
    std::array<float,2> amplitude{};
    std::array<std::uint64_t,2> durationNs{};      // remaining duration of the started pulse
};
// XR thread only (single consumer). Several pulses for one hand in one frame: the strongest wins (the rest merged).
// A new pulse replaces a running one only if it is at least as strong or the running one has ended.
class Player {
    std::array<std::uint64_t,2> end_{};std::array<float,2> amplitude_{};
public:
    Frame frame(std::uint64_t now,bool suppress=false) noexcept {
        Frame out{};std::array<Pulse,2> best{};std::array<bool,2> have{};
        Pulse p{};
        while(queue.pop(p)){
            if(now>p.postedNs&&now-p.postedNs>maxAgeNs){stale.fetch_add(1,std::memory_order_relaxed);continue;}
            if(suppress){suppressed.fetch_add(1,std::memory_order_relaxed);continue;}
            const unsigned h=p.hand&1u;
            if(have[h]){merged.fetch_add(1,std::memory_order_relaxed);if(p.amplitude<=best[h].amplitude)continue;}
            best[h]=p;have[h]=true;
        }
        for(unsigned h=0;h<2;++h){
            const bool running=now<end_[h];
            if(have[h]&&(!running||best[h].amplitude>=amplitude_[h])){
                const std::uint64_t duration=std::uint64_t(best[h].durationUs)*1000ull;
                end_[h]=now+duration;amplitude_[h]=best[h].amplitude;
                out.start[h]=true;out.amplitude[h]=best[h].amplitude;out.durationNs[h]=duration;started.fetch_add(1,std::memory_order_relaxed);
            }else if(have[h])merged.fetch_add(1,std::memory_order_relaxed);
            if(suppress){end_[h]=0;amplitude_[h]=0;}
            out.active[h]=now<end_[h];
        }
        return out;
    }
    void reset() noexcept {end_={};amplitude_={};}
};
inline Player xrPlayer{};
inline Frame xrFrame(std::uint64_t now,bool suppress=false) noexcept {return xrPlayer.frame(now,suppress);}

inline std::string report() {
    char b[768];
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"posted\":%llu,\"started\":%llu,\"dropped\":%llu,\"stale\":%llu,\"merged\":%llu,\"suppressed\":%llu,"
        "\"bySource\":{\"enter\":%llu,\"grab\":%llu,\"return\":%llu,\"refuse\":%llu,\"meleeHit\":%llu,\"meleeKill\":%llu,"
        "\"reload\":%llu,\"hammer\":%llu,\"spin\":%llu,\"teleport\":%llu,\"lasso\":%llu}}", // + reload, hammer, spin, teleport, lasso
        enabled.load()?"true":"false",(unsigned long long)posted.load(),(unsigned long long)started.load(),(unsigned long long)dropped.load(),
        (unsigned long long)stale.load(),(unsigned long long)merged.load(),(unsigned long long)suppressed.load(),
        (unsigned long long)bySource[SourceSlotEnter].load(),(unsigned long long)bySource[SourceGrab].load(),
        (unsigned long long)bySource[SourceReturn].load(),(unsigned long long)bySource[SourceRefuse].load(),
        (unsigned long long)bySource[SourceMeleeHit].load(),(unsigned long long)bySource[SourceMeleeKill].load(),
        (unsigned long long)bySource[SourceReload].load(),(unsigned long long)bySource[SourceHammer].load(),(unsigned long long)bySource[SourceSpin].load(),
        (unsigned long long)bySource[SourceTeleport].load(),(unsigned long long)bySource[SourceLasso].load());
    return b;
}
} // namespace rdrvr::vr::haptics
