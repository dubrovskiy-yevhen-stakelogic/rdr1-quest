#pragma once
#include "XrPlatform.h"
#include "ControllerInput.h"
#include "HoldChord.h"
#include "Haptics.h"
#include "vr/VrControls.h" // Controls: L3/R3 single clicks + recenter chord (StickClickArbiter)
#include <vector>

namespace rdrvr::runtime {
class ControllerActions {
    XrAction stick_{},trigger_{},squeeze_{},click_{},gripPose_{};
    std::array<XrAction,5> face_{};
    std::array<XrPath,2> hands_{};
    std::array<XrSpace,2> gripSpaces_{};
    HoldChord theaterChord_;
    ::rdrvr::vr::controls::StickClickArbiter stickClicks_; // Controls: replaces the 1 s both-clicks HoldChord
    std::array<bool,2> theaterGrips_{};bool manualTheater_{};
    bool(*gripsBusy_)() noexcept{}; // VR holsters own a grip -> theater chord disarmed, grips untouched
    XrAction haptic_{};XrSession hapticSession_{};HapticOutput hapticOutput_;
    static bool applyHaptic(void*,unsigned,float,std::uint64_t);
    static bool stopHaptic(void*,unsigned);
public:
    void create(XrInstance,XrActionSet,const std::array<XrPath,2>&,std::vector<XrActionSuggestedBinding>&);
    void createSpaces(XrSession);
    void destroySpaces() noexcept;
    void outputHaptics(const HapticSample* sample){hapticOutput_.update(sample);}
    // One short VR pulse (holsters) on the same haptic action; XR thread, after outputHaptics of the frame.
    bool pulse(unsigned hand,float amplitude,std::uint64_t durationNs){return applyHaptic(this,hand,amplitude,durationNs);}
    const HapticOutput& hapticOutput()const{return hapticOutput_;}
    // Called after the owning harness's single xrSyncActions, never a second sync.
    // Stick clicks pass through StickClickArbiter: a single L3/R3 click reaches the game as a >= 120 ms
    // pulse (release inside 150 ms) or while held (after 150 ms); both clicked within 150 ms = chord, consumed until each
    // is released, held >= 600 ms = recenter.
    bool read(XrSession,XrSpace,XrTime,ControllerSample&,bool& recenter,bool& theaterToggle);
    void enableTheaterSwitch(bool enabled){manualTheater_=enabled;invalidate();}
    void setGripsBusy(bool(*busy)() noexcept){gripsBusy_=busy;} // XR thread predicate (holsters::gripsEngaged)
    void invalidate(){stickClicks_.invalidate();theaterChord_.invalidate();theaterGrips_={};hapticOutput_.update(nullptr);}
};
}
