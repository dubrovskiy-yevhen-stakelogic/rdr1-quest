#pragma once
// WELCOME (user): a one-time-per-launch message on the menu panel at the player's first steps in the stereo world (never during a
// cutscene / menu): early-stage warning, charger + GPU boost script advice, where the controls are, the Discord link. The first press of
// any button / trigger / grip closes it; that press never reaches the game (the input stays masked until everything is released).
// XR thread only. debug.rdrvr.vr.welcome 0 = never show.
#include "VrControls.h"
#include "runtime/ControllerInput.h"
#include <atomic>
#include <cstdint>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::welcome {
inline constexpr const char* text=
    "\n"
    "  WELCOME TO RDR RICH VR MOD\n"
    "\n"
    "  THE MOD IS IN AN EARLY STAGE:\n"
    "  EXPECT BUGS AND PERFORMANCE ISSUES.\n"
    "\n"
    "  FOR THE BEST EXPERIENCE:\n"
    "  - PLAY WITH THE CHARGER CONNECTED\n"
    "  - RUN THE GPU BOOST SCRIPT\n"
    "\n"
    "  CONTROLS: HOLD THE LEFT MENU BUTTON > CONTROLS\n"
    "\n"
    "  DISCORD:\n"
    "  \x02https://discord.gg/5pUVuZVQj\n"
    "\n"
    "  PRESS ANY BUTTON TO CLOSE";
enum class State : std::uint8_t {Waiting,Shown,Closing,Done};
inline std::atomic<unsigned> stateAtom{0};
inline std::atomic<std::uint64_t> shownNs{},closedNs{};
inline State state() noexcept {return State(stateAtom.load(std::memory_order_relaxed));}
inline bool active() noexcept {return state()==State::Shown;}
namespace detail {
inline std::uint64_t worldSince{};
inline bool lever() noexcept {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX]{};return !(__system_property_get("debug.rdrvr.vr.welcome",v)>0&&v[0]=='0');
#else
    return true;
#endif
}
}
// XR thread, once per frame after the layers were chosen: gameStereo = the world stereo layer is shown this frame.
// Returns true when the panel content changed (redraw now).
inline bool frame(bool gameStereo,std::uint64_t now) noexcept {
    const auto s=state();
    if(s!=State::Waiting)return false;
    const bool foot=::rdrvr::vr::controls::lastLayout.load(std::memory_order_relaxed)==unsigned(::rdrvr::vr::controls::Layout::Foot);
    if(!gameStereo||!foot){detail::worldSince=0;return false;}
    if(!detail::worldSince){detail::worldSince=now;return false;}
    if(now-detail::worldSince<2'000'000'000ull)return false; // 2 s of on-foot stereo world = the player has control
    if(!detail::lever()){stateAtom.store(unsigned(State::Done),std::memory_order_relaxed);return false;}
    stateAtom.store(unsigned(State::Shown),std::memory_order_relaxed);shownNs.store(now,std::memory_order_relaxed);return true;
}
// XR thread, every controller sample (after the VR menu took its share). true = the sample belongs to the welcome panel (caller masks it).
inline bool consume(const ::rdrvr::runtime::ControllerSample& sample,std::uint64_t now) noexcept {
    const auto s=state();
    if(s!=State::Shown&&s!=State::Closing)return false;
    bool pressed=(sample.buttons&~std::uint32_t(::rdrvr::runtime::PadMenu))!=0;
    for(float t:sample.triggers)pressed=pressed||t>.7f;
    for(float g:sample.grips)pressed=pressed||g>.7f;
    bool released=sample.buttons==0;
    for(float t:sample.triggers)released=released&&t<.2f;
    for(float g:sample.grips)released=released&&g<.2f;
    if(s==State::Shown){
        if(pressed&&now>shownNs.load(std::memory_order_relaxed)+500'000'000ull){stateAtom.store(unsigned(State::Closing),std::memory_order_relaxed);closedNs.store(now,std::memory_order_relaxed);}
        return true;
    }
    if(released){stateAtom.store(unsigned(State::Done),std::memory_order_relaxed);return false;} // Closing: masked until all released
    return true;
}
} // namespace rdrvr::vr::welcome
