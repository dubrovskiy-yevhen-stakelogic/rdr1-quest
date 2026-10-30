#pragma once
// GAME MENU flat view: no VR menu row, the tap of the left menu button is the only route.
// What stays is what makes that tap usable: the flat view and the pad arming with a gun in hand.
//
// Engine facts (librdr.so 1.58 arm64):
//  - PAUSE = pad 12 Start (static initializer 0x1ac7018: first Binding(12, pad)); Android key 108 -> d11 pad 12. Released -> fuiEvent
//    '@PAUSE.PAUSE*RELEASED' -> rdrHudScene::HandleInputs 0x29fa0f8 -> UINavigator::StackPush("PauseMenuScene"); in a cutscene
//    UICutscene::HandleInputs 0x29fafb8 -> StackPush("Cutscenes_Paused"). Sent by the tap of the left menu button (GraphicsMenuInput).
//  - Display: IsMenuActive() (HudScene hidden or popups) -> PresentationContext::Menu -> no world pass, the stock 2D frame (1374x720)
//    on the theater quad, re-anchored 2 m in front of the head on the context change (XrHarness). The pad is the stock pad there
//    (Layout::Native): stick / A / B navigate, B or the tap of the left menu button leaves.
//  - rage::UIObject::IsVisible(true) = vtable +0xe0 (the game's own call in ShowPauseMenu). rdrPauseMenuScene::IsVisibleOrTransitioning
//    reads a Flash variable, so it is not used here. An open scene forces PresentationContext::Menu, so it is never invisible behind
//    the stereo world or a stereo cutscene (TraceEntry uiMenuUnlessCutscene).
//
// Threads: noteTap on the XR thread; step on the engine main thread (the NativeUiContext owner); atomics between.
// Lever (read at 4 Hz or so from the main thread): debug.rdrvr.vr.gamemenu.force 0 = an open scene does not force the Menu presentation.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rdrvr::vr::gamemenu {
inline constexpr std::uint64_t armGraceNs=2'000'000'000; // padArmIgnoreGrips stays this long after the scene closed (the re-arm on the way back to the world)
inline std::atomic<int> force{1},visible{-1}; // visible -1 = the engine query never ran
inline std::atomic<bool> engineReady{false};
inline std::atomic<std::uint64_t> shown{},closes{},forcedNative{},forcedCutscene{},stuckAtBoot{};
// An open scene forces the Menu presentation only after the scene was seen hidden once (armed) and the mod sent Start this session
// (startSent), so a game that never gets a Start from us (and a visible bit that would stay set) is left alone.
inline std::atomic<bool> armed{false},startSent{false};
inline std::atomic<std::uint64_t> taps{};
inline bool forcing(bool scene) noexcept {return scene&&armed.load(std::memory_order_relaxed)&&startSent.load(std::memory_order_relaxed)&&force.load(std::memory_order_relaxed);}
inline void noteTap() noexcept {taps.fetch_add(1,std::memory_order_relaxed);startSent.store(true,std::memory_order_relaxed);} // XR thread: the left menu button's own Start
// Main thread, every few game updates.
inline void step(bool scene) noexcept {
    const int was=visible.exchange(scene?1:0,std::memory_order_relaxed);
    if(!scene)armed.store(true,std::memory_order_relaxed);
    if(scene&&was!=1)shown.fetch_add(1,std::memory_order_relaxed);
    else if(!scene&&was==1)closes.fetch_add(1,std::memory_order_relaxed);
}
inline std::string report() {
    char b[360];
    std::snprintf(b,sizeof b,"{\"force\":%d,\"engine\":%s,\"armed\":%s,\"visible\":%d,\"taps\":%llu,\"shown\":%llu,\"closes\":%llu,"
        "\"forcedNative\":%llu,\"forcedCutscene\":%llu,\"stuckAtBoot\":%llu}",
        force.load(),engineReady.load()?"true":"false",armed.load()?"true":"false",visible.load(),
        (unsigned long long)taps.load(),(unsigned long long)shown.load(),(unsigned long long)closes.load(),
        (unsigned long long)forcedNative.load(),(unsigned long long)forcedCutscene.load(),(unsigned long long)stuckAtBoot.load());
    return b;
}
} // namespace rdrvr::vr::gamemenu
