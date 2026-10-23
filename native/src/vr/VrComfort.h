#pragma once
// HORSE VIGNETTE (user: riding was nauseating; then: too much motion for a speed-driven ring, keep it on in the saddle): a fixed comfort
// ring for as long as the player is mounted (horse, wagon), closed in on mounting and opened again after the dismount.
// Header-only (no new TU for build_library.py); the head-locked layer that draws it is runtime/ComfortVignette.
//
//   observeRide(transition, now)     main thread, every riding eye sample (GameXr ridingAnchorSample path): "mounted now", and whether
//                                    the rider is in a mount / dismount animation (MOUNT TURN, report only)
//   Vignette::step(level, now)       XR thread: target 1 while a riding sample is fresh (0 after staleNs = not riding), attack /
//                                    release smoothing -> intensity 0..1
//   clearTan(level, intensity)       XR thread: tangent of the clear half-angle (the ring's inner edge) for the quad's size
//   MOUNT TURN: noteMountSnap(now) (main thread, the one turn to the seated heading) starts a dip to black that dipAlpha(now) plays on
//   the XR thread (colour scale on every layer).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>

namespace rdrvr::vr::comfort {

inline constexpr float attackTau=.12f,releaseTau=.45f;             // s: closes quickly on mounting, opens slowly after the dismount
inline constexpr std::uint64_t staleNs=250'000'000ull;              // no riding sample for this long = not riding (dismount, theater)
// Clear half-angle per level (OFF, LOW, MEDIUM, HIGH) while mounted, and the open angle the ring closes in from on mounting: it starts
// just outside the Quest's FOV (its corners < 2 % dark), so the fade in / out is a ring moving in / out, never a pop.
inline constexpr std::array<float,4> clearDeg{90.f,42.f,33.f,25.f};
inline constexpr float openDeg=57.f;
inline constexpr unsigned levelCount=unsigned(clearDeg.size());
// MOUNT TURN dip: black long enough for the first pair rendered after the snap to reach the display, then a fade in.
inline constexpr std::uint64_t dipBlackNs=120'000'000ull,dipInNs=250'000'000ull;

// ---- main thread -> XR thread ----
inline std::atomic<std::uint64_t> rideNs{0};                        // steady_clock ns of the newest riding sample (0 = none)
inline std::atomic<std::uint64_t> dipNs{0};                         // steady_clock ns of the newest mount snap (0 = none)
inline std::atomic<std::uint64_t> rideSamples{0},transitionSamples{0},mountSnaps{0};
inline std::atomic<float> heldDeg{0};                               // the rider's turn not followed during the MOUNT TURN holds (sum)
// Main thread only: the player is mounted now; transition = in a mount / dismount animation.
inline void observeRide(bool transition,std::uint64_t now) noexcept {
    if(!now)return;
    rideNs.store(now,std::memory_order_release);rideSamples.fetch_add(1,std::memory_order_relaxed);
    if(transition)transitionSamples.fetch_add(1,std::memory_order_relaxed);
}
// Main thread: MOUNT TURN SNAP turned the view to the seated heading now; `heldRad` = the turn skipped during the hold.
inline void noteMountSnap(std::uint64_t now,float heldRad) noexcept {
    dipNs.store(now,std::memory_order_release);mountSnaps.fetch_add(1,std::memory_order_relaxed);
    if(std::isfinite(heldRad))heldDeg.store(heldDeg.load(std::memory_order_relaxed)+std::fabs(heldRad)*57.29578f,std::memory_order_relaxed);}

// ---- XR thread ----
// XR thread: colour scale of the dip (1 = none): 0 for dipBlackNs, then smoothstep up over dipInNs.
inline float dipAlpha(std::uint64_t now) noexcept {
    const auto t0=dipNs.load(std::memory_order_acquire);
    if(!t0||now<t0||now-t0>=dipBlackNs+dipInNs)return 1.f;
    if(now-t0<dipBlackNs)return 0.f;
    const float t=float(double(now-t0-dipBlackNs)/double(dipInNs));return t*t*(3.f-2.f*t);
}
// 1 while mounted (a riding sample within staleNs), else 0.
inline float targetOf(std::uint64_t sampleNs,std::uint64_t now) noexcept {return sampleNs&&now>=sampleNs&&now-sampleNs<=staleNs?1.f:0.f;}
inline float clearTan(unsigned level,float intensity) noexcept {
    if(!level||level>=levelCount)return 0.f;
    const float i=std::clamp(intensity,0.f,1.f),deg=openDeg+(clearDeg[level]-openDeg)*i;
    return std::tan(deg*3.14159265f/180.f);
}
// Published for the report (XR thread writes, any thread reads).
inline std::atomic<float> shownIntensity{0};
inline std::atomic<std::uint64_t> layerFrames{0};
class Vignette {
    float value_{};std::uint64_t lastNs_{};
public:
    // level = Snapshot::rideVignette() (0 = OFF: intensity 0 at once, nothing to fade).
    float step(unsigned level,std::uint64_t now) noexcept {
        const float target=level?targetOf(rideNs.load(std::memory_order_acquire),now):0.f;
        const float dt=lastNs_&&now>lastNs_?std::min(float(double(now-lastNs_)*1e-9),.1f):0.f;lastNs_=now;
        if(!level)value_=0.f;
        else if(dt>0.f){const float tau=target>value_?attackTau:releaseTau;value_+=(target-value_)*(1.f-std::exp(-dt/tau));}
        if(value_<.002f)value_=0.f;
        shownIntensity.store(value_,std::memory_order_relaxed);
        return value_;
    }
    void reset() noexcept {value_=0.f;lastNs_=0;shownIntensity.store(0.f,std::memory_order_relaxed);}
};

// Report fragment (riding.vignette).
inline std::string report() {
    const auto r=[](float v){return std::to_string(std::round(v*100.f)/100.f);};
    return "{\"samples\":"+std::to_string(rideSamples.load(std::memory_order_relaxed))+
        ",\"transitionSamples\":"+std::to_string(transitionSamples.load(std::memory_order_relaxed))+",\"mountSnaps\":"+std::to_string(mountSnaps.load(std::memory_order_relaxed))+
        ",\"heldDeg\":"+r(heldDeg.load(std::memory_order_relaxed))+
        ",\"layerFrames\":"+std::to_string(layerFrames.load(std::memory_order_relaxed))+",\"intensity\":"+r(shownIntensity.load(std::memory_order_relaxed))+"}";
}

} // namespace rdrvr::vr::comfort
