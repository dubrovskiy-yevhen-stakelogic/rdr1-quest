// Host regression (MSVC: build as tools/test-casual-trigger.cpp). The pad re-arm after a presentation change: the whole-pad rule, the soft
// re-arm when the scene camera hands back (CameraOverride -> World / Optic) with its held-input mask, the change classification, an XR epoch bump
// inside and after the window, and the report.
#include "runtime/ControllerInput.h"
#include <cassert>
#include <iostream>
using namespace rdrvr::runtime;
using P=PresentationContext;

int main(){
    ControllerSample in{};in.sequence=1;in.epoch=1;in.continuity=1;in.referenceSpace=1;in.active=in.focused=true;
    std::uint64_t now=10'000'000'000ull;
    ControllerPolicy policy;
    const auto sample=[&]{now+=13'888'889;in.publishedNs=now;++in.sequence;return policy.sample(&in);};
    sample();assert(sample().ready); // armed at rest

    // 1. whole-pad rule (window closed): a gun held in HOLD keeps its grip squeezed, the pad never arms
    padArmSoftUntilNs=0;in.grips[1]=.9f;
    policy.invalidate(RearmPresentationHard);
    for(int i=0;i<100;++i){const auto o=sample();assert(!o.ready&&o.triggers[1]==0.f&&!o.buttons);}
    in.grips[1]=.1f;assert(!sample().ready);assert(sample().ready); // armed on the neutral sample, ready from the next
    {const auto j=rearmReport();assert(j.find("\"why\":\"HARD_PRESENTATION\"")!=std::string::npos&&j.find("\"blockedBy\":8")!=std::string::npos&&
        j.find("\"gripOnlyFrames\":100")!=std::string::npos);}

    // 2. soft re-arm: ready on the first sample, held inputs masked until at rest, sticks live
    // entering the scene camera keeps the whole-pad rule (no holster filter there): a held gun grip keeps the pad unarmed through the shot
    notePresentationChange(P::World,P::CameraOverride,now,true);assert(!padArmSoftUntilNs.load());
    in.grips[1]=.9f;policy.invalidate(rearmDiag.lastChangeWhy.load());
    in.triggers[1]=1.f;for(int i=0;i<60;++i){const auto o=sample();assert(!o.ready&&o.triggers[1]==0.f&&!o.buttons);}in.triggers[1]=0.f;
    notePresentationChange(P::CameraOverride,P::World,now,true);assert(padArmSoftUntilNs.load()==now+padArmSoftWindowNs);
    in.grips[1]=.9f;in.triggers[1]=1.f;in.buttons=PadA;in.sticks[0]=.8f;
    policy.invalidate(rearmDiag.lastChangeWhy.load());
    {const auto o=sample();
     assert(o.ready&&o.triggers[1]==0.f&&!(o.buttons&PadRightTrigger)&&!(o.buttons&PadRightGrip)&&!(o.buttons&PadA)&&o.sticks[0]==.8f);
     assert(o.armMask==(armMaskTrigger(1)|armMaskGrip(1)));}
    for(int i=0;i<20;++i){const auto o=sample();assert(o.ready&&o.triggers[1]==0.f&&!(o.buttons&(PadRightTrigger|PadA|PadRightGrip)));}
    in.triggers[1]=.1f;{const auto o=sample();assert(o.ready&&o.armMask==armMaskGrip(1)&&o.triggers[1]==.1f);}
    in.triggers[1]=1.f;{const auto o=sample();assert(o.triggers[1]==1.f&&(o.buttons&PadRightTrigger)&&o.armMask==armMaskGrip(1));} // fires on this tick
    in.buttons=0;sample();in.buttons=PadA;assert(sample().buttons&PadA);                                                          // A free after one release
    assert(!(sample().buttons&PadRightGrip));in.grips[1]=.1f;sample();in.grips[1]=.9f;assert(sample().buttons&PadRightGrip);     // grip free after it opened
    in={};in.sequence=now;in.epoch=1;in.continuity=1;in.referenceSpace=1;in.active=in.focused=true;sample();
    {const auto j=rearmReport();assert(j.find("\"why\":\"SOFT_PRESENTATION\"")!=std::string::npos&&j.find("\"soft\":1")!=std::string::npos);}

    // 3. classification
    assert(!softRearmChange(P::World,P::CameraOverride)&&softRearmChange(P::CameraOverride,P::World));
    assert(!softRearmChange(P::Optic,P::CameraOverride)&&softRearmChange(P::CameraOverride,P::Optic));
    assert(!softRearmChange(P::World,P::Menu)&&!softRearmChange(P::CameraOverride,P::CameraTransition)&&!softRearmChange(P::World,P::CameraTransition));
    assert(!softRearmChange(P::World,P::Optic)&&!softRearmChange(P::CameraOverride,P::Cutscene)&&!softRearmChange(P::Menu,P::CameraOverride));
    notePresentationChange(P::CameraOverride,P::World,now,true);assert(padArmSoftUntilNs.load());
    notePresentationChange(P::World,P::CameraTransition,now,true);assert(!padArmSoftUntilNs.load());          // a hard change closes it
    notePresentationChange(P::CameraOverride,P::World,now,false);assert(!padArmSoftUntilNs.load());           // lever off

    // 4. the XR bridge's epoch bump inside the window arms soft; after it, the whole-pad rule
    notePresentationChange(P::CameraOverride,P::World,now,true);
    in.grips[0]=.9f;in.inputEpoch=5;{const auto o=sample();assert(o.ready&&o.armMask==armMaskGrip(0)&&!(o.buttons&PadLeftGrip));}
    now+=padArmSoftWindowNs;++in.inputEpoch;assert(!sample().ready&&!sample().ready);
    in.grips[0]=0.f;sample();assert(sample().ready);
    // no input: unarmed, no output
    assert(!policy.sample(nullptr).ready);
    {const auto j=rearmReport();assert(j.find("\"softArms\":2")!=std::string::npos);}
    std::cout<<"Pad re-arm whole-pad rule, soft re-arm masks, classification and epoch window tests passed\n";
    return 0;
}
