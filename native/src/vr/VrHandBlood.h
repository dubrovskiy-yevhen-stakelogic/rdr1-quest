#pragma once
// BLOOD ON THE KNUCKLES: a bare hand that broke glass (VrHandPhys glassBlood, past the off / holding gates) stamps its hand here; the
// VR hands pass (HandsPass.cpp) tints that hand's knuckle face dark red in hands.frag for .bloodtints seconds (60), then fades it over
// 15 s. The engine's own blood drop stays as it was - drawn in the world, it is hidden by the VR hand mesh in front of it.
// The tint sticks to the skin: the vertex shader measures it on the FIST pose (UXRH pose 3), so an opened hand keeps it on the same
// knuckles. Knuckle face centre in UXRH model space (both meshes identical): the fist's front vertices (pose 3, x > 0.115 m, normal +x)
// average (0.123, 0.009, 0.006) m. Levers: debug.rdrvr.vr.handphys.bloodtint 0 = off, .bloodtints (s, 60).
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace rdrvr::vr::handblood {
inline std::uint64_t steadyNs() noexcept {return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline std::array<std::atomic<std::uint64_t>,2> stampNs{}; // steady ns of the last glass break per hand (0 = none)
inline std::atomic<bool> enabled{true};
inline std::atomic<float> holdS{60.f};
inline std::atomic<std::uint64_t> stamps{},drawnFrames{};
inline constexpr float centre[3]{.123f,.009f,.006f};
inline constexpr float fadeS=15.f;
inline void stamp(unsigned hand) noexcept {
    if(!enabled.load(std::memory_order_relaxed))return;const auto now=steadyNs();
    stampNs[hand&1u].store(now?now:1ull,std::memory_order_relaxed);stamps.fetch_add(1,std::memory_order_relaxed);}
// 0..1: full for holdS, then a linear fade over fadeS
inline float intensity(unsigned hand) noexcept {
    if(!enabled.load(std::memory_order_relaxed))return 0.f;const auto now=steadyNs();
    const auto t=stampNs[hand&1u].load(std::memory_order_relaxed);if(!t||now<t)return 0.f;
    const float age=float(double(now-t)*1e-9),hold=holdS.load(std::memory_order_relaxed);
    if(age<=hold)return 1.f;
    const float k=1.f-(age-hold)/fadeS;return k>0.f?k:0.f;}
} // namespace rdrvr::vr::handblood
