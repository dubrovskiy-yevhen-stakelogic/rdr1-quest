// Host regression: run with a temporary directory for vr.cfg as argv[1]. Build as tools/test-casual-trigger.cpp.
// Drives InputBrain::frame with a long gun in the right hand and the left hand free. The main-thread poll needs the engine, so the test
// publishes what the poll publishes and covers its decision through foregripGateOf.
#include "vr/VrControls.h"
#include "vr/VrHammer.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
namespace V=rdrvr::vr;
namespace H=V::holsters;
namespace R=rdrvr::runtime;
static std::uint64_t now=1'000'000'000ull,sequence=1;
static R::ControllerSample in{};
static rdrvr::Pose head{};
static H::InputBrain* brain{};
static rdrvr::Pose pose(float x,float y,float z){
    rdrvr::Pose p{};p.position={x,y,z};p.orientation={0.f,0.f,0.f,1.f};p.positionValid=p.orientationValid=true;return p;}
// The gun looks along -Z from (.2, 1, -.3): 25 cm of barrel ahead of the fist is z -.55.
static constexpr float fistX=.2f,fistY=1.f,fistZ=-.3f;
static void leftAt(float forward,float side=0.f,float up=0.f){
    in.grip[0]=in.aim[0]=pose(fistX+side,fistY+up,fistZ-forward);}
// One input frame: the pad carries the grip bits the policy would send; returns them after the brain's masks.
static R::PadState frame(float leftGrip,float rightGrip=1.f,float rightTrigger=0.f){
    now+=14'000'000;in.publishedNs=now;in.sequence=++sequence;in.grips={leftGrip,rightGrip};in.triggers={0.f,rightTrigger};
    R::PadState pad{};pad.ready=true;
    if(leftGrip>=.55f)pad.buttons|=R::PadLeftGrip;
    if(rightGrip>=.55f)pad.buttons|=R::PadRightGrip;
    brain->frame(&in,&head,true,now,pad);return pad;
}
static void run(float leftGrip,int frames,float rightGrip=1.f){for(int i=0;i<frames;++i)frame(leftGrip,rightGrip);}
// The barrel direction the weapon pipeline gets for the current sample.
static std::array<float,4> barrelNow(){auto w=in;H::weaponInput(w);return w.aim[1].orientation;}
static bool sameTurn(const std::array<float,4>& a,const std::array<float,4>& b){float d=0;for(unsigned i=0;i<4;++i)d+=std::fabs(a[i]-b[i]);return d<1e-3f;}
// The poll's three stores at each context.
static void contextNormal(){H::contextAllowed=true;H::foregripGate=H::ForegripOpen;H::deadEyeRunning=false;}
static void contextDeadEye(){H::contextAllowed=false;H::foregripGate=H::ForegripDeadEye;H::deadEyeRunning=true;}
static void contextClosed(bool deadEye){H::contextAllowed=false;H::foregripGate=H::ForegripClosed;H::deadEyeRunning=deadEye;H::foregripWhy="COVER";}
static void freshHand(){leftAt(.70f,.60f);run(0.f,16);assert(H::supportHand.load()<0);}
int main(int argc,char** argv) {
    assert(argc==2);
    assert(V::vrSettings.configurePath((std::filesystem::absolute(argv[1])/"vr.cfg").string()));
    assert(V::vrSettings.loadOnce());
    assert(V::vrSettings.applyWeaponPreset(V::PresetRealistic));
    assert(V::vrSettings.set(V::Key::TwoHandFire,1));
    // The poll's decision: only Dead Eye alone opens the gate.
    using H::foregripGateOf;
    assert(foregripGateOf(true,false,true,true,false,true)==H::ForegripOpen);
    assert(foregripGateOf(true,false,false,true,false,true)==H::ForegripOpen);
    assert(foregripGateOf(false,false,true,true,false,true)==H::ForegripClosed);
    assert(foregripGateOf(false,true,true,true,false,true)==H::ForegripDeadEye);
    assert(foregripGateOf(false,true,false,true,false,true)==H::ForegripClosed);
    assert(foregripGateOf(false,true,true,false,false,true)==H::ForegripClosed);
    assert(foregripGateOf(false,true,true,true,true,true)==H::ForegripClosed);
    assert(foregripGateOf(false,true,true,true,false,false)==H::ForegripClosed);

    H::engineReady=true;V::mainthread::markInstalled();
    H::setConfig({true,H::GripMode::Hold,H::MarkerMode::Near,true,true});
    static H::InputBrain instance;brain=&instance;
    in.epoch=in.continuity=in.referenceSpace=1;in.active=in.focused=true;
    head=pose(.0f,1.6f,0.f);
    in.grip[1]=in.aim[1]=pose(fistX,fistY,fistZ);
    H::currentSlot=H::SlotLongarm;H::heldKind=1;H::holdHand=1;H::holdOurs=true;H::holdSequence=7;++H::holdGeneration;
    H::ownedMask=1u<<H::SlotLongarm;H::grabMask=0;
    contextNormal();

    // The hold press: after the first frames' re-latch, a squeeze of the holding hand is adopted as the press that holds the gun.
    leftAt(.70f,.60f);run(0.f,2,0.f);run(0.f,2,1.f);assert(H::resumes.load()==1);
    freshHand();
    // Grip on the barrel, then Dead Eye starts: the grip, the attachment and the masks do not move.
    leftAt(.35f,.10f);run(.9f,40);assert(H::supportHand.load()==0);
    const auto generation=H::attachmentGeneration.load();
    const auto bent=barrelNow();assert(!sameTurn(bent,{0.f,0.f,0.f,1.f}));
    contextDeadEye();run(.9f,20);
    assert(H::supportHand.load()==0&&H::attachmentGeneration.load()==generation);
    assert(sameTurn(barrelNow(),bent));
    assert(H::deadEyeGripsAtStart.load()==1&&H::deadEyeSupportFrames.load()==20&&H::deadEyeLongGunFrames.load()==20);
    assert(H::foregripGateDrops.load()==0);
    {const auto p=frame(.9f);assert(!(p.buttons&R::PadLeftGrip));}
    // TWO HAND FIRE: the held barrel lets a shot through in Dead Eye.
    {const auto blocked=H::oneHandBlocked.load();frame(.9f,1.f,0.f);const auto p=frame(.9f,1.f,1.f);
     assert(H::oneHandBlocked.load()==blocked&&p.triggers[1]>0.f);frame(.9f,1.f,0.f);}
    // Dead Eye ends: the grip stays and counts as kept. A stale Dead Eye context bit keeps the gate open meanwhile.
    H::deadEyeRunning=false;run(.9f,3);assert(H::supportHand.load()==0&&H::deadEyeGripsKept.load()==1);
    contextNormal();run(.9f,3);assert(H::supportHand.load()==0&&H::attachmentGeneration.load()==generation);
    assert(sameTurn(barrelNow(),bent));

    // A barrel taken during Dead Eye, an opened support hand lets go after its 150 ms as usual and may take it again.
    leftAt(.35f);run(0.f,16);assert(H::supportHand.load()<0);
    freshHand();contextDeadEye();leftAt(.35f);
    {const auto taken=H::deadEyeGripsTaken.load();const auto p=frame(.9f);
     assert(H::supportHand.load()==0&&H::deadEyeGripsTaken.load()==taken+1&&!(p.buttons&R::PadLeftGrip));}
    run(0.f,16);assert(H::supportHand.load()<0);
    run(.9f,2);assert(H::supportHand.load()==0);
    run(0.f,16);assert(H::supportHand.load()<0);

    // A squeeze at the fist: a transfer outside Dead Eye, masked in it.
    freshHand();leftAt(.06f,.03f);
    {const auto transfers=H::transfersRequested.load(),masked=H::deadEyeTransfersMasked.load();const auto p=frame(.9f);
     assert(H::transfersRequested.load()==transfers&&H::deadEyeTransfersMasked.load()==masked+1&&H::supportHand.load()<0&&!(p.buttons&R::PadLeftGrip));
     const auto q=frame(.9f);assert(!(q.buttons&R::PadLeftGrip));}
    run(0.f,16);
    // TWO HAND AIM off has no foregrip: the transfer path stays as it is, in Dead Eye too.
    H::setConfig({true,H::GripMode::Hold,H::MarkerMode::Near,false,true});
    {const auto transfers=H::transfersRequested.load();frame(.9f);assert(H::transfersRequested.load()==transfers+1);}
    run(0.f,16);H::setConfig({true,H::GripMode::Hold,H::MarkerMode::Near,true,true});
    // Normal context: a squeeze ahead of the fist in the transfer zone beyond the barrel reach still transfers.
    contextNormal();freshHand();leftAt(.05f,.05f);
    {const auto transfers=H::transfersRequested.load();frame(.9f);assert(H::transfersRequested.load()==transfers+1);}
    run(0.f,16);
    // Along the barrel but past its reach: masked in Dead Eye, never LB.
    contextDeadEye();freshHand();leftAt(.40f,.30f);
    {const auto native=H::nativeGripPresses.load();const auto p=frame(.9f);
     assert(H::nativeGripPresses.load()==native&&!(p.buttons&R::PadLeftGrip)&&H::supportHand.load()<0);}
    run(0.f,16);
    // Out of the gun's reach: the grip is native as it is today.
    freshHand();leftAt(.40f,.60f);
    {const auto native=H::nativeGripPresses.load();run(.9f,2);assert(H::nativeGripPresses.load()==native+1);}
    run(0.f,16);

    // A socket under the barrel: Dead Eye's empty socket mask must not turn the squeeze into a dead zone. Hand 35 cm along the barrel,
    // which is where the free hand ends up; the sockets are the fixed offsets around the upright head.
    {H::ownedMask=(1u<<H::SlotLongarm)|(1u<<H::SlotThrown);head=pose(fistX,1.3f,fistZ-.15f-.08f);
     freshHand();leftAt(.35f);
     const auto taken=H::deadEyeGripsTaken.load(),back=H::backMasked.load();frame(.9f);
     assert(H::supportHand.load()==0&&H::deadEyeGripsTaken.load()==taken+1&&H::backMasked.load()==back);
     run(0.f,16);head=pose(.0f,1.6f,0.f);H::ownedMask=1u<<H::SlotLongarm;}

    // Any other refusal drops the grip and names it; it does not come back without a new squeeze.
    contextNormal();freshHand();leftAt(.35f);run(.9f,2);assert(H::supportHand.load()==0);
    {const auto drops=H::foregripGateDrops.load();contextClosed(false);frame(.9f);
     assert(H::supportHand.load()<0&&H::foregripGateDrops.load()==drops+1&&!std::strcmp(H::foregripLastDrop.load(),"COVER"));
     const auto p=frame(.9f);assert(!(p.buttons&R::PadLeftGrip));
     contextNormal();run(.9f,3);assert(H::supportHand.load()<0);}
    run(0.f,16);
    // The same inside Dead Eye, counted as such, and that grip is reported lost.
    contextNormal();freshHand();leftAt(.35f);run(.9f,2);contextDeadEye();run(.9f,2);assert(H::supportHand.load()==0);
    {const auto drops=H::foregripGateDropsDeadEye.load(),lost=H::deadEyeGripsLost.load();contextClosed(true);frame(.9f);
     assert(H::supportHand.load()<0&&H::foregripGateDropsDeadEye.load()==drops+1);
     contextNormal();frame(.9f);assert(H::deadEyeGripsLost.load()==lost+1);}
    run(0.f,16);

    // Opening the holding hand in Dead Eye: the put-away waits for the context, the support hand changes nothing.
    contextNormal();freshHand();leftAt(.35f);run(.9f,2);assert(H::supportHand.load()==0);
    contextDeadEye();
    {const auto intents=H::holsterIntents.load(),releases=H::holdReleases.load(),wait=H::holsterWaitFrames.load();
     frame(.9f,1.f);frame(.9f,0.f);run(.9f,3,0.f);
     assert(H::supportHand.load()==0&&H::holsterIntents.load()==intents+1&&H::holdReleases.load()==releases&&H::holsterWaitFrames.load()>wait);
     // A squeeze before it ends keeps the gun.
     const auto cancelled=H::holsterCancelled.load();frame(.9f,1.f);assert(H::holsterCancelled.load()==cancelled+1);
     // Opened again, the put-away runs when Dead Eye ends and the grip goes with the gun.
     frame(.9f,0.f);run(.9f,2,0.f);assert(H::holsterIntents.load()==intents+2&&H::holdReleases.load()==releases);
     contextNormal();frame(.9f,0.f);assert(H::holdReleases.load()==releases+1&&H::supportHand.load()==0);
     H::holdHand=-1;H::currentSlot=-1;H::holdOurs=false;++H::holdGeneration;frame(.9f,0.f);assert(H::supportHand.load()<0);}

    // The report carries the fields.
    const auto report=H::report();
    for(const char* key:{"\"foregrip\":{\"gate\":","\"gateDrops\":","\"gateDropsInDeadEye\":","\"supportFrames\":","\"gripsKept\":","\"gripsLost\":","\"gripsTaken\":","\"transfersMasked\":"})
        assert(report.find(key)!=std::string::npos);
    std::cout<<"Dead Eye foregrip tests passed\n";
}
