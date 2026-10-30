#pragma once
// WELCOME: a one-time-per-launch message on the menu panel at the player's first steps in the stereo world (never during a
// cutscene / menu): mod version, early-stage warning, charger advice, the GPU boost state, where the controls are, the Discord link. The first press of
// any button / trigger / grip closes it; that press never reaches the game (the input stays masked until everything is released).
// XR thread only. debug.rdrvr.vr.welcome 0 = never show.
// compose() re-reads GpuBoost::on() on every panel redraw (2 Hz)
// while the panel is open. It opens after 2 s on foot in the stereo world, long after the helper's first 2 s poll, so the first draw is settled.
// The how-to line follows the state: helper running = the GRAPHICS row is what is off; no helper = the PC script or QGO.
#include "VrControls.h"
#include "runtime/ControllerInput.h"
#include "runtime/GpuBoost.h"
#include "runtime/ModVersion.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::welcome {
// Panel text (\x05 = PerfPanel::orange; <= 52 glyphs per line). With the orange block 23 lines: PerfPanel tightens the pitch to 19 px, the
// trailing newline keeps a margin under the last line (PerfPanel fits a long text exactly to the panel's bottom edge).
// XR thread: `out` must hold the whole text (1 KB is plenty).
inline void compose(char* out,std::size_t size) noexcept {
    auto& boost=::rdrvr::runtime::gpuBoost;const bool on=boost.on();boost.noteWelcome(on);
    char block[256];
    if(on)std::snprintf(block,sizeof block,"  - GPU BOOST: ON\n");
    else std::snprintf(block,sizeof block,
        "\n"
        "  \x05" "GPU BOOST IS OFF - PLEASE ENABLE IT\n"
        "  \x05" "FOR A BETTER EXPERIENCE:\n"
        "  \x05" "%s\n"
        "\n"
        "  \x05" "PLAYING WITHOUT BOOST? TURN APPSW OFF:\n"
        "  \x05" "MENU > GRAPHICS > SPACEWARP\n",boost.helper()?"MENU > GRAPHICS > GPU BOOST":"RUN THE GPU BOOST SCRIPT OR SET GPU 7 IN QGO");
    std::snprintf(out,size,
        "\n"
        "  WELCOME TO RDR RICH VR MOD %s\n"
        "\n"
        "  THE MOD IS IN AN EARLY STAGE:\n"
        "  EXPECT BUGS AND PERFORMANCE ISSUES.\n"
        "\n"
        "  FOR THE BEST EXPERIENCE:\n"
        "  - PLAY WITH THE CHARGER CONNECTED\n"
        "%s"
        "\n"
        "  CONTROLS: HOLD THE LEFT MENU BUTTON > CONTROLS\n"
        "\n"
        "  DISCORD:\n"
        "  \x02https://discord.gg/5pUVuZVQj\n"
        "\n"
        "  PRESS ANY BUTTON TO CLOSE\n",::rdrvr::runtime::modVersion,block);
}
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
