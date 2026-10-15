#pragma once
#include "runtime/ControllerInput.h"
#include <cerrno>

namespace rdrvr::engine {
struct NativePadEventApi {
    // Exact AndroidGamepad ABI: this, device id, Android key/axis; float in s0.
    void (*button)(void*,int,int,bool){};
    void (*axis)(void*,int,int,float,bool&){};
};
inline bool submitPadEvents(NativePadEventApi api,void* ownedPad,int deviceId,const runtime::PadState& state) noexcept {
    if(!api.button||!api.axis||!ownedPad)return false;
    const int incoming=errno;
    try{
        bool significant{};
        const int axes[]{0,1,11,14,17,18};
        const float values[]{state.sticks[0],-state.sticks[1],state.sticks[2],-state.sticks[3],state.triggers[0],state.triggers[1]};
        for(unsigned i=0;i<6;++i)api.axis(ownedPad,deviceId,axes[i],values[i],significant);
        // AxisEvent's default threshold can classify zero as down. Explicit
        // button states run AFTER axes so neutral triggers always release.
        // Also key 19 (D-pad up -> d11 pad 10, AndroidGamepad_c::GamepadButtonEvent 0x198cb0c), driven by PadDpadUp.
        const int keys[]{96,97,99,100,108,106,107,102,103,104,105,19};
        const std::uint32_t bits[]{runtime::PadA,runtime::PadB,runtime::PadX,runtime::PadY,runtime::PadMenu,
            runtime::PadLeftStick,runtime::PadRightStick,runtime::PadLeftGrip,runtime::PadRightGrip,runtime::PadLeftTrigger,runtime::PadRightTrigger,
            runtime::PadDpadUp};
        for(unsigned i=0;i<12;++i)api.button(ownedPad,deviceId,keys[i],(state.buttons&bits[i])!=0);
        errno=incoming;return true;
    }catch(...){errno=incoming;return false;}
}
}
