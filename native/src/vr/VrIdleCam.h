#pragma once
// IDLE CAMERA: standing still must not send the game to 2D.
//
// Facts (librdr.so 1.58 arm64 + the player script in content_android.rpf, decrypted and disassembled):
//  - The player script's 'Exec Idle Cam' step (code 0xa047): the game camera is on top of the primary channel &&
//    InputTracker::GetTimeSinceLastInput(pad) > 120.0 (func 0xc7e8 = return 120.f) -> global 0x19ed = 1 and func 0xa23a creates a script
//    gohCamera named "idlecam" and pushes it with rdr2GOH::SetCurrentCameraOnChannel(cam, 0, ...) -> camManager::SetCameraOnChannel.
//    While active: GetTimeSinceLastInput < 120 -> it removes the camera itself and resets everything.
//  - The top camera is then a plain camCamera, not the game camera: NativeCameraMode::Override -> PresentationContext::CameraOverride ->
//    no head anchor, no stereo, the 2D theater until the next stick / button input.
//  - rage::IdleCameraContext::IsContextActive (0x2b40208, the phone port's touch context) identifies it the same way: top camera of
//    channel 0 -> camCamera +0x5d0 script wrapper -> gohNamedBase::GetObjectName (+0x34) == "idlecam".
//  - InputTracker::m_InputTracking 0x4e75c400: 4 pads of 12 bytes {move, look, button} seconds since the last input; Update runs inside
//    sagShellDerived::UpdateInputs; GetTimeSinceLastInput = the minimum of the three. Of the 198 scripts found in the game's archives only
//    the player script reads a timer and only through that minimum (the move / button natives have no importer, the look one is not a native).
//
// The fix. In VR the head is the look input, so look stays "just used":
//  1. TIMERS: after each qualified sagShellDerived::UpdateInputs the look seconds of all pads are 0 (engine main thread, before the
//     script step). The minimum stays 0, the script never decides the player is idle. No camera, no hidden HUD, no use-context change.
//  2. GUARD: camManager::SetCameraOnChannel (GOT) drops a push of the camera named "idlecam" on channel 0 and releases its transitions the
//     way the engine does when the camera is already on top (0x2594cbc). No other camera (cutscene, scripted) is touched. Only reachable
//     with timers off or when the script's force bit (static 0x7b+0x218) is set.
//
// Lever debug.rdrvr.vr.idlecam (re-read about every 256 main frames): 0 = stock (the idle camera and its 2D come back), unset / 1 = timers + guard,
// 2 = guard only, 3 = timers only. Report vr.idleCam: crossings counts the moments the stock game would have started the idle camera.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::idlecam {
inline constexpr std::uintptr_t trackingOffset=0x4e75c400u,setCameraOffset=0x2594bb0u,setCameraGotOffset=0x2e50350u,releaseTransitionOffset=0x25909d8u;
inline constexpr std::uintptr_t cameraWrapperOffset=0x5d0u,wrapperNameOffset=0x34u;
inline constexpr std::array<unsigned char,24> setCameraFingerprint{
#include "gamecode/VrIdleCam_18405d4_24_8h.inc"
};
inline constexpr std::array<unsigned char,24> releaseTransitionFingerprint{
#include "gamecode/VrIdleCam_25909d8_24_8h.inc"
};
inline constexpr char cameraName[8]="idlecam";
inline constexpr float stockTriggerSeconds=120.f; // the script's func 0xc7e8

inline std::atomic<int> lever{1};
inline std::atomic<float*> tracking{nullptr};
inline std::atomic<bool> guardInstalled{false};
inline std::atomic<std::uint64_t> frames{},keptFrames{},crossings{},pushes{},blocked{},lastCrossingNs{},lastBlockedNs{};
inline std::atomic<float> idleSeconds{0.f},maxIdleSeconds{0.f},lastBlockedIdleSeconds{0.f};

inline bool keepTimers() noexcept {const int l=lever.load(std::memory_order_relaxed);return l==1||l==3;}
inline bool guarding() noexcept {const int l=lever.load(std::memory_order_relaxed);return l==1||l==2;}
inline void readLever() noexcept {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX]{};const int n=__system_property_get("debug.rdrvr.vr.idlecam",v);
    lever.store(n>0&&v[0]>='0'&&v[0]<='3'&&!v[1]?v[0]-'0':1,std::memory_order_relaxed);
#endif
}
// Engine main thread, right after the original sagShellDerived::UpdateInputs (InputTracker::Update just ran; the script step comes later).
inline void frame(std::uint64_t now) noexcept {
    static unsigned calls=0;static float lookIdle=0.f;static bool over=false;
    if((calls++&255u)==0)readLever();
    frames.fetch_add(1,std::memory_order_relaxed);
    volatile float* const t=tracking.load(std::memory_order_acquire);if(!t)return;
    const bool keep=keepTimers();const float move=t[0],look=t[1],button=t[2];
    // With timers kept the look field holds only this frame's step (0 = the look stick is in use); the sum keeps the honest idle time for the report.
    lookIdle=!keep?look:look>0.f?lookIdle+look:0.f;
    const float idle=std::min({move,lookIdle,button});
    idleSeconds.store(idle,std::memory_order_relaxed);
    if(idle>maxIdleSeconds.load(std::memory_order_relaxed))maxIdleSeconds.store(idle,std::memory_order_relaxed);
    if(idle>stockTriggerSeconds){if(!over){over=true;crossings.fetch_add(1,std::memory_order_relaxed);lastCrossingNs.store(now,std::memory_order_relaxed);}}
    else over=false;
    if(!keep)return;
    for(unsigned pad=0;pad<4;++pad)t[pad*3+1]=0.f;
    keptFrames.fetch_add(1,std::memory_order_relaxed);
}
inline void noteBlocked(std::uint64_t now) noexcept {
    blocked.fetch_add(1,std::memory_order_relaxed);lastBlockedNs.store(now,std::memory_order_relaxed);
    lastBlockedIdleSeconds.store(idleSeconds.load(std::memory_order_relaxed),std::memory_order_relaxed);
}
inline std::string report(std::uint64_t now) {
    const auto age=[&](std::uint64_t t)->long long {return t&&now>=t?(long long)((now-t)/1000000u):-1;};
    char b[512];
    std::snprintf(b,sizeof b,"{\"lever\":%d,\"timers\":%s,\"guard\":%s,\"trackerMapped\":%s,\"guardInstalled\":%s,\"frames\":%llu,\"keptFrames\":%llu,\"idleS\":%.1f,\"maxIdleS\":%.1f,\"stockTriggerS\":%.0f,"
        "\"crossings\":%llu,\"lastCrossingAgeMs\":%lld,\"pushes\":%llu,\"blocked\":%llu,\"lastBlockedAgeMs\":%lld,\"lastBlockedIdleS\":%.1f}",
        lever.load(),keepTimers()?"true":"false",guarding()?"true":"false",tracking.load()?"true":"false",guardInstalled.load()?"true":"false",
        (unsigned long long)frames.load(),(unsigned long long)keptFrames.load(),double(idleSeconds.load()),double(maxIdleSeconds.load()),double(stockTriggerSeconds),
        (unsigned long long)crossings.load(),age(lastCrossingNs.load()),(unsigned long long)pushes.load(),(unsigned long long)blocked.load(),age(lastBlockedNs.load()),double(lastBlockedIdleSeconds.load()));
    return b;
}
} // namespace rdrvr::vr::idlecam
