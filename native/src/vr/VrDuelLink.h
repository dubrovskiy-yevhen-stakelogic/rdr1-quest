#pragma once
// DUEL ON LOOK, input side (VrDuelStart.h has the rest). Runs in GameXr controlsPadFilter on the native input thread, on the PHYSICAL
// Touch buttons before the BUTTONS remap: while the CHEATS row is ON and the player is in the ON FOOT layout, a Y press with the left
// trigger already held posts one command to the cheats module (main thread) and is hidden from the game until Y is released, so it never
// mounts / uses anything. LT stays visible (LT + R3 Dead Eye keeps working); a Y that was down before LT is an ordinary Y.
#include "VrMainThread.h"
#include <atomic>
#include <cstdint>

namespace rdrvr::vr::duellink {
namespace mt=::rdrvr::vr::mainthread;
inline constexpr std::uint16_t opPress=9;
inline std::atomic<bool> enabled{};
inline std::atomic<std::uint64_t> chords{},postFailures{};
// Returns the bits to clear from the physical word. Input thread only (the latch needs no atomics).
inline std::uint32_t filter(std::uint32_t buttons,std::uint32_t lt,std::uint32_t y,bool usable) noexcept {
    static bool yBefore{},held{};
    const bool yDown=(buttons&y)!=0;
    if(!yDown)held=false;
    else if(!yBefore&&(buttons&lt)&&usable&&enabled.load(std::memory_order_relaxed)){
        held=true;chords.fetch_add(1,std::memory_order_relaxed);
        mt::Command c{};c.module=mt::ModuleCheats;c.op=opPress;
        if(!mt::post(c))postFailures.fetch_add(1,std::memory_order_relaxed);
    }
    yBefore=yDown;
    return held?y:0u;
}
} // namespace rdrvr::vr::duellink
