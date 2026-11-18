// Host regression: run with a temporary directory for vr.cfg as argv[1]. Build as tools/test-casual-trigger.cpp.
// KEEP GUN (vr/VrKeepGun.h): the pure decisions, then InputBrain::frame with a long gun in the right hand and the left hand on its
// barrel. The main-thread poll needs the engine, so the test publishes what the poll publishes (the hold, the context, the restore
// request) and reads the commands the brain posts through a ModuleHolsters execute handler.
#include "vr/VrControls.h"
#include "vr/VrHammer.h"
#include "vr/VrNoAutoAim.h"
#include "runtime/HoldChord.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>
namespace V=rdrvr::vr;
namespace H=V::holsters;
namespace K=V::keepgun;
namespace R=rdrvr::runtime;
static std::uint64_t now=1'000'000'000ull,sequence=1;
static R::ControllerSample in{};
static rdrvr::Pose head{};
static H::InputBrain* brain{};
static std::vector<V::mainthread::Command> commands;
static void capture(const V::mainthread::Command& c,const V::mainthread::Frame&) noexcept {commands.push_back(c);}
static rdrvr::Pose pose(float x,float y,float z){
    rdrvr::Pose p{};p.position={x,y,z};p.orientation={0.f,0.f,0.f,1.f};p.positionValid=p.orientationValid=true;return p;}
static constexpr float fistX=.2f,fistY=1.f,fistZ=-.3f;
static void leftAt(float forward,float side=0.f){in.grip[0]=in.aim[0]=pose(fistX+side,fistY,fistZ-forward);}
static void frame(float leftGrip,float rightGrip){
    now+=14'000'000;in.publishedNs=now;in.sequence=++sequence;in.grips={leftGrip,rightGrip};in.triggers={0.f,0.f};
    R::PadState pad{};pad.ready=true;
    if(leftGrip>=.55f)pad.buttons|=R::PadLeftGrip;
    if(rightGrip>=.55f)pad.buttons|=R::PadRightGrip;
    brain->frame(&in,&head,true,now,pad);
    V::mainthread::run(true,now);
}
static void run(float leftGrip,float rightGrip,int frames){for(int i=0;i<frames;++i)frame(leftGrip,rightGrip);}
static void contextNormal(){H::contextAllowed=true;H::foregripGate=H::ForegripOpen;H::deadEyeRunning=false;H::grabMask=1u<<H::SlotLongarm;}
static void contextDeadEye(){H::contextAllowed=false;H::foregripGate=H::ForegripDeadEye;H::deadEyeRunning=true;H::grabMask=0;}
static void contextClosed(){H::contextAllowed=false;H::foregripGate=H::ForegripClosed;H::deadEyeRunning=false;H::grabMask=0;H::foregripWhy="COVER";}
// The poll's view of a draw that landed / of the engine putting the item away.
static void holdLongGun(std::uint32_t seq){H::currentSlot=H::SlotLongarm;H::heldKind=1;H::holdHand=1;H::holdOurs=true;H::holdSequence=seq;++H::holdGeneration;}
static void engineTookIt(){H::currentSlot=-1;H::holdHand=-1;H::holdOurs=false;H::holdSequence=0;++H::holdGeneration;}
static const V::mainthread::Command* last(std::uint16_t op){for(auto i=commands.rbegin();i!=commands.rend();++i)if(i->op==op)return &*i;return nullptr;}
static std::size_t count(std::uint16_t op){std::size_t n=0;for(const auto& c:commands)n+=c.op==op;return n;}
// The right hand squeezes the gun as the hold press (after a re-latch), the left hand takes the barrel.
static void takeGun(std::uint32_t seq){
    holdLongGun(seq);contextNormal();leftAt(.70f,.60f);run(0.f,0.f,4);run(0.f,1.f,2);
    leftAt(.35f);run(.9f,1.f,3);assert(H::supportHand.load()==0);
}
static void postRequest(std::uint32_t seq,int context){
    K::Request r{};r.slot=H::SlotLongarm;r.hand=1;r.holdSequence=seq;r.ns=now;r.context=context;K::post(r);}

int main(int argc,char** argv) {
    assert(argc==2);
    // ---- pure decisions ----
    constexpr std::uint64_t s=1'000'000'000ull,w=2*s;
    static_assert(K::contextAt(10*s,true,0,false,0,w)==K::ContextDeadEye);
    static_assert(K::contextAt(10*s,false,0,true,0,w)==K::ContextDuel);
    static_assert(K::contextAt(10*s,false,9*s,false,0,w)==K::ContextDeadEyeEnd);
    static_assert(K::contextAt(10*s,false,7*s,false,0,w)==K::ContextNone);
    static_assert(K::contextAt(10*s,false,0,false,9*s,w)==K::ContextDuelEnd);
    static_assert(K::contextAt(10*s,false,9*s,false,9*s+1,w)==K::ContextDuelEnd);
    static_assert(K::contextAt(10*s,false,0,false,0,w)==K::ContextNone);
    static_assert(K::contextAt(5*s,false,9*s,false,0,w)==K::ContextNone); // an end stamped after now (clock skew) never counts
    // ---- NO AUTO AIM: the gate of the stock lock suppression ----
    {namespace N=V::noautoaim;using G=N::GateIn;
     static_assert(N::gateOpen(G{})==N::GateNoWeapon);
     static_assert(N::gateOpen(G{true,1,1,true,false,false,false,true})==N::GateOpen);                 // a revolver in the VR hand
     static_assert(N::gateOpen(G{true,7,2,false,false,false,false,true})==N::GateOpen);                // throwing knives (thrown, slot 2)
     static_assert(N::gateOpen(G{true,-1,2,false,false,false,false,true})==N::GateOpen);               // a throwable whose class is not read yet
     static_assert(N::gateOpen(G{false,-1,-1,false,true,false,false,true})==N::GateOpen);              // the holsters' own force aim
     static_assert(N::gateOpen(G{true,11,3,false,true,false,false,true})==N::GateLasso);               // the lasso keeps its lock
     static_assert(N::gateOpen(G{false,-1,3,false,true,false,false,true})==N::GateLasso);
     static_assert(N::gateOpen(G{true,1,1,true,true,true,false,true})==N::GateDuel);                   // a duel keeps the native lock
     static_assert(N::gateOpen(G{true,10,0,false,false,false,false,true})==N::GateMelee);              // knife / fists melee lock untouched
     static_assert(N::gateOpen(G{true,1,1,true,true,false,true,true})==N::GateOpen);                   // Dead Eye with lever deadeye 1
     static_assert(N::gateOpen(G{true,1,1,true,true,false,true,false})==N::GateDeadEye);               // deadeye 0: stock in Dead Eye
     static_assert(N::gateOpen(G{true,10,2,false,false,false,false,true})==N::GateOpen);               // throwing knives taken right after the knife: kind still melee
     static_assert(N::gateOpen(G{true,11,1,true,false,false,false,true})==N::GateOpen);                // a revolver taken right after the lasso: kind still lasso
     static_assert(N::gateOpen(G{true,1,0,false,false,false,false,true})==N::GateMelee);               // the knife taken right after a revolver: the slot decides
     static_assert(N::gateOpen(G{true,10,-1,false,false,false,false,true})==N::GateMelee);             // slot unknown: the engine class
     static_assert(N::gateOpen(G{false,11,-1,false,true,false,false,true})==N::GateLasso);
     static_assert(N::kindMelee==H::kindMelee&&N::slotMelee==H::SlotMelee&&N::slotThrown==H::SlotThrown&&N::slotLasso==H::SlotLasso);
     static_assert(N::restoreLockOn(true,true,true,0)&&!N::restoreLockOn(false,true,true,0)&&!N::restoreLockOn(true,false,true,0)&&
                   !N::restoreLockOn(true,true,false,0)&&!N::restoreLockOn(true,true,true,2)); // the stock request back only over a zeroed word
     static_assert(N::modeOf(0)==N::ModeStock&&N::modeOf(1)==N::ModeSuppress&&N::modeOf(2)==N::ModeObserve&&N::modeOf(7)==N::ModeSuppress);
     static_assert(N::anyLock(N::LockFields{0,0,0,1})&&!N::anyLock(N::LockFields{}));
     assert(std::fabs(N::wrapDeg(350.f)+10.f)<1e-4f&&std::fabs(N::wrapDeg(-190.f)-170.f)<1e-4f);
     const auto r=N::report();
     for(const char* key:{"\"hook\":","\"mode\":\"suppress\"","\"reticle\":{\"calls\":","\"cleared\":","\"camera\":{\"calls\":","\"softZeroed\":",
                          "\"combatMode\":{\"current\":","\"edges\":[","\"lockOn\":{\"on\":","\"restored\":"})assert(r.find(key)!=std::string::npos);}
    constexpr std::uint64_t tail=750'000'000ull;
    static_assert(K::tailKeeps(true,true,true,true,true,false,10*s,10*s-tail/2,0,tail));
    static_assert(!K::tailKeeps(true,true,true,true,true,false,10*s,10*s-tail,0,tail));
    static_assert(K::tailKeeps(true,true,true,true,true,false,10*s,0,10*s-1,tail));
    static_assert(!K::tailKeeps(false,true,true,true,true,false,10*s,10*s-1,0,tail));
    static_assert(!K::tailKeeps(true,false,true,true,true,false,10*s,10*s-1,0,tail)); // never newly set
    static_assert(!K::tailKeeps(true,true,false,true,true,false,10*s,10*s-1,0,tail)); // the hand is empty
    static_assert(!K::tailKeeps(true,true,true,false,true,false,10*s,10*s-1,0,tail));
    static_assert(!K::tailKeeps(true,true,true,true,false,false,10*s,10*s-1,0,tail));
    static_assert(!K::tailKeeps(true,true,true,true,true,true,10*s,10*s-1,0,tail)); // control locked
    static_assert(!K::tailKeeps(true,true,true,true,true,false,10*s,10*s-1,0,0));
    static_assert(K::waitedIntentKeeps(true,true,.45f,.30f)&&!K::waitedIntentKeeps(true,true,.30f,.30f)&&!K::waitedIntentKeeps(true,false,.9f,.30f)&&
                  !K::waitedIntentKeeps(false,true,.9f,.30f));
    {K::Request r{};r.holdSequence=7;r.ns=10*s;
     K::HandFacts f{};f.latched=f.captured=true;f.pressSequence=7;
     assert(K::decide(r,f,true,10*s,K::restoreWaitNs)==K::DecidePost);
     assert(K::decide(r,f,false,10*s,K::restoreWaitNs)==K::DecideWait);
     assert(K::decide(r,f,false,10*s+K::restoreWaitNs+1,K::restoreWaitNs)==K::DecideExpired);
     auto g=f;g.latched=false;assert(K::decide(r,g,true,10*s,K::restoreWaitNs)==K::DecideOpened);
     g=f;g.captured=false;assert(K::decide(r,g,true,10*s,K::restoreWaitNs)==K::DecideOpened);
     g=f;g.pressSequence=8;assert(K::decide(r,g,true,10*s,K::restoreWaitNs)==K::DecideOpened);
     g=f;g.otherJob=true;assert(K::decide(r,g,true,10*s,K::restoreWaitNs)==K::DecideBusy);
     g=f;g.poseValid=false;assert(K::decide(r,g,true,10*s,K::restoreWaitNs)==K::DecideWait);
     g=f;g.holdMode=false;g.latched=g.captured=false;g.pressSequence=0;assert(K::decide(r,g,true,10*s,K::restoreWaitNs)==K::DecidePost);} // TOGGLE
    {K::Request r{};r.ns=10*s;r.context=K::ContextDuel;
     static_assert(K::clockStart(K::Request{0,1,1,0,10*s,K::ContextDuel},15*s,16*s,20*s)==15*s);
     static_assert(K::clockStart(K::Request{0,1,1,0,10*s,K::ContextDuel},0,16*s,20*s)==10*s);
     static_assert(K::clockStart(K::Request{0,1,1,0,10*s,K::ContextDuel},29*s,31*s,20*s)==10*s); // the cap
     static_assert(K::clockStart(K::Request{0,1,1,0,10*s,K::ContextDeadEye},15*s,16*s,20*s)==10*s); // Dead Eye keeps its own wait
     auto c=r;c.ns=K::clockStart(r,15*s,16*s,K::duelHoldCapNs);assert(K::decide(c,K::HandFacts{false,true,true,0,false,true},false,16*s,K::restoreWaitNs)==K::DecideWait);}

    // ---- the brain ----
    assert(V::vrSettings.configurePath((std::filesystem::absolute(argv[1])/"vr.cfg").string()));
    assert(V::vrSettings.loadOnce());
    assert(V::vrSettings.applyWeaponPreset(V::PresetRealistic));
    H::engineReady=true;V::mainthread::markInstalled();
    assert(V::mainthread::registerModule(V::mainthread::ModuleHolsters,"holsters-test",nullptr,&capture));
    H::setConfig({true,H::GripMode::Hold,H::MarkerMode::Near,true,true});
    static H::InputBrain instance;brain=&instance;
    in.epoch=in.continuity=in.referenceSpace=1;in.active=in.focused=true;
    head=pose(.0f,1.6f,0.f);
    in.grip[1]=in.aim[1]=pose(fistX,fistY,fistZ);
    H::ownedMask=1u<<H::SlotLongarm;

    // 1. The engine puts the long gun away at the end of Dead Eye while both grips stay closed: it is drawn back into the same hand
    // (instant), the left hand takes the barrel again without a new squeeze, and nothing reaches the game as LB / RB.
    takeGun(7);
    {const auto draws=count(H::OpDraw);
     engineTookIt();postRequest(7,K::ContextDeadEyeEnd);frame(.9f,1.f);
     assert(count(H::OpDraw)==draws+1&&K::posted.load()==1);
     const auto* d=last(H::OpDraw);assert(d->arg[0]==H::SlotLongarm&&d->arg[1]==1&&d->arg[2]==H::DrawRestore);
     assert(H::supportHand.load()<0&&H::mouthWhistles.load()==0);
     holdLongGun(d->sequence);run(.9f,1.f,3);
     assert(H::supportHand.load()==0&&K::supportsRestored.load()==1);
     // Opening the hand still holsters it.
     const auto puts=count(H::OpPutAway);run(.9f,0.f,2);assert(count(H::OpPutAway)==puts+1);
     engineTookIt();run(0.f,0.f,4);}

    // 2. The grip opened before the gate let the restore through: no draw (an open hand is a put-away).
    takeGun(11);
    {const auto draws=count(H::OpDraw),opened=K::opened.load();
     engineTookIt();contextClosed();postRequest(11,K::ContextDeadEyeEnd);run(.9f,1.f,3);
     assert(count(H::OpDraw)==draws&&K::waitFrames.load()>0);
     run(.9f,0.f,2);contextNormal();run(.9f,0.f,2);
     assert(count(H::OpDraw)==draws&&K::opened.load()==opened+1);
     run(0.f,0.f,4);}

    // 3. Dead Eye still running: the restore waits; it gives up after restoreWaitNs.
    takeGun(13);
    {const auto draws=count(H::OpDraw),expired=K::expired.load();
     engineTookIt();contextDeadEye();postRequest(13,K::ContextDeadEye);
     const auto until=now+K::restoreWaitNs+50'000'000ull;while(now<until)frame(.9f,1.f);
     contextNormal();frame(.9f,1.f);
     assert(count(H::OpDraw)==draws&&K::expired.load()==expired+1);
     run(0.f,0.f,4);}

    // 3b. A put-away in the duel's result phase: the wait clock stands while the VR duel route runs, then the restore draws.
    takeGun(37);
    {const auto draws=count(H::OpDraw),expired=K::expired.load();
     engineTookIt();V::duelctl::route=true;postRequest(37,K::ContextDuel);
     const auto until=now+K::restoreWaitNs+500'000'000ull;while(now<until)frame(.9f,1.f);
     assert(count(H::OpDraw)==draws&&K::expired.load()==expired&&K::duelHeldFrames.load()>0);
     V::duelctl::route=false;frame(.9f,1.f);
     assert(count(H::OpDraw)==draws+1&&last(H::OpDraw)->arg[2]==H::DrawRestore);
     holdLongGun(last(H::OpDraw)->sequence);run(.9f,1.f,2);
     run(.9f,0.f,2);engineTookIt();run(0.f,0.f,4);}

    // 4. The other hand holds something again (a game draw landed there): no draw.
    takeGun(17);
    {const auto draws=count(H::OpDraw),busy=K::busy.load();
     engineTookIt();postRequest(17,K::ContextDuelEnd);H::holdHand=0;H::holdOurs=true;H::holdSequence=99;H::currentSlot=H::SlotLongarm;++H::holdGeneration;
     frame(.9f,1.f);assert(count(H::OpDraw)==draws&&K::busy.load()==busy+1);
     engineTookIt();run(0.f,0.f,4);}

    // 5. A release intent that waited for Dead Eye: a grip back above the open threshold when it ends keeps the gun; a grip that
    // stays open puts it away when it ends.
    takeGun(19);
    {const auto puts=count(H::OpPutAway),kept=K::waitedKept.load();
     contextDeadEye();frame(.9f,0.f);run(.9f,0.f,3);frame(.9f,.45f);run(.9f,.45f,2);
     assert(count(H::OpPutAway)==puts);
     contextNormal();frame(.9f,.45f);
     assert(count(H::OpPutAway)==puts&&K::waitedKept.load()==kept+1);
     run(.9f,.45f,5);assert(count(H::OpPutAway)==puts);
     // The kept gun holsters on the next real open.
     run(.9f,0.f,2);assert(count(H::OpPutAway)==puts+1);
     engineTookIt();run(0.f,0.f,4);}
    takeGun(23);
    {const auto puts=count(H::OpPutAway),kept=K::waitedKept.load();
     contextDeadEye();frame(.9f,0.f);run(.9f,0.f,3);assert(count(H::OpPutAway)==puts);
     contextNormal();frame(.9f,0.f);
     assert(count(H::OpPutAway)==puts+1&&K::waitedKept.load()==kept);
     engineTookIt();run(0.f,0.f,4);}

    // 6. The lever off: a request still pending is dropped, and a waited release intent runs as before.
    K::restoreOn=false;K::enabled=false;
    takeGun(29);
    {const auto draws=count(H::OpDraw),off=K::offRequests.load();
     engineTookIt();postRequest(29,K::ContextDeadEyeEnd);frame(.9f,1.f);
     assert(count(H::OpDraw)==draws&&K::offRequests.load()==off+1);
     run(0.f,0.f,4);}
    takeGun(31);
    {const auto puts=count(H::OpPutAway);
     contextDeadEye();frame(.9f,0.f);run(.9f,0.f,3);frame(.9f,.45f);contextNormal();frame(.9f,.45f);
     assert(count(H::OpPutAway)==puts+1);
     engineTookIt();run(0.f,0.f,4);}
    K::restoreOn=true;K::enabled=true;

    // HOLD-END GRIP LATCH: a prop hold that ends while both grips stay closed (the hand body lost: "body", or detached, safety, teleport,
    // mode) never lets the closed grips reach the game as LB (weapon wheel) / RB; the theater chord runs on every sample as the XR thread
    // runs it (busy = holsters::gripsEngaged) before the brain sees it.
    {namespace GL=V::grablink;namespace R=rdrvr::runtime;
     R::HoldChord chord{};std::array<bool,2> chordHeld{};bool worldOn=true;
     engineTookIt();contextNormal();
     in.grip[0]=in.aim[0]=pose(-.30f,1.30f,-.60f);in.grip[1]=in.aim[1]=pose(.30f,1.30f,-.60f); // in front: no socket, no mouth
     const auto step=[&](float g0,float g1,bool ready)->std::uint32_t{
        now+=14'000'000;in.publishedNs=now;in.sequence=++sequence;in.grips={g0,g1};in.triggers={0.f,0.f};
        if(ready){GL::setReady(0,true,now);GL::setReady(1,true,now);}
        if(GL::holding(0))GL::stampHolding(0,now);if(GL::holding(1))GL::stampHolding(1,now);
        // gripsEngaged reads the grab stamps on the steady clock: the test clock's grab busy is added here
        R::theaterGripChord(chord,chordHeld,in.grips,now,(GL::chordGuard.load()&&GL::grabBusy(now))||H::gripsEngaged(),2'000'000'000,&in.chordGrips);
        R::PadState pad{};pad.ready=true;
        if(in.grips[0]>=.55f)pad.buttons|=R::PadLeftGrip;
        if(in.grips[1]>=.55f)pad.buttons|=R::PadRightGrip;
        brain->frame(&in,&head,worldOn,now,pad);V::mainthread::run(true,now);
        return pad.buttons&(R::PadLeftGrip|R::PadRightGrip);};
     for(const std::uint8_t end:{std::uint8_t(7),std::uint8_t(3),std::uint8_t(10),std::uint8_t(11),std::uint8_t(8)}){ // body, detached, safety, teleport, mode
        for(int i=0;i<4;++i)step(0.f,0.f,true);
        const auto native0=H::nativeGripPresses.load(),grabs0=GL::latchBy[0][GL::LatchGrab].load()+GL::latchBy[1][GL::LatchGrab].load();
        assert(step(1.f,1.f,true)==0); // both hands at a prop: LatchGrab, captured
        assert(GL::latchBy[0][GL::LatchGrab].load()+GL::latchBy[1][GL::LatchGrab].load()==grabs0+2);
        GL::setHolding(0,true,now);GL::setHolding(1,true,now);GL::setHoldKind(0,GL::HoldHeavy);GL::setHoldKind(1,GL::HoldHeavy);
        for(int i=0;i<20;++i)assert(step(1.f,1.f,false)==0);
        // the hold ends (the ready stamps gone too) with both grips still closed
        GL::lastEnd[0]=end;GL::lastEnd[1]=end;GL::setHolding(0,false,0);GL::setHolding(1,false,0);GL::setHoldKind(0,GL::HoldNone);GL::setHoldKind(1,GL::HoldNone);
        GL::setReady(0,false,now);GL::setReady(1,false,now);
        const auto unarmed0=R::theaterChordStats.consumed.load();
        for(int i=0;i<200;++i){assert(step(1.f,1.f,false)==0);
            // a busy flip for one sample (a ready stamp of the other hand): the raw closed grips must still never latch fresh
            if(i==50){GL::setReady(1,true,now);assert(step(1.f,1.f,false)==0);GL::setReady(1,false,now);}}
        assert(H::nativeGripPresses.load()==native0&&R::theaterChordStats.consumed.load()==unarmed0&&GL::capturedMask.load()==3);
        // the grips open: the rows close, the latch ends; a fresh press of the left hand alone away from every zone is the game's LB again
        for(int i=0;i<3;++i)step(0.f,0.f,false);
        assert(GL::capturedMask.load()==0);
        {std::lock_guard lock(GL::gripLatchMutex);bool found=false;
         for(const auto& r:GL::gripLatchRing)if(r.ns&&r.end==end&&r.openMs>0&&r.firstLatch<0&&r.captured==1&&r.maskedFrames>=200)found=true;
         assert(found);}
        assert(step(1.f,0.f,false)==R::PadLeftGrip&&H::nativeGripPresses.load()==native0+1);
        for(int i=0;i<3;++i)step(0.f,0.f,false);}
     assert(GL::holdEndNativeLeak[0].load()==0&&GL::holdEndNativeLeak[1].load()==0);
     // the lever off (no captured-grip busy, no chord ownership): the unarmed chord passes the closed grips untouched all the same
     GL::holdLatch=false;
     {for(int i=0;i<4;++i)step(0.f,0.f,true);const auto native0=H::nativeGripPresses.load();
      assert(step(1.f,1.f,true)==0);GL::setHolding(0,true,now);GL::setHolding(1,true,now);for(int i=0;i<10;++i)assert(step(1.f,1.f,false)==0);
      GL::lastEnd[0]=7;GL::lastEnd[1]=7;GL::setHolding(0,false,0);GL::setHolding(1,false,0);GL::setReady(0,false,now);GL::setReady(1,false,now);
      for(int i=0;i<100;++i){assert(step(1.f,1.f,false)==0);
          if(i==50){GL::setReady(1,true,now);assert(step(1.f,1.f,false)==0);GL::setReady(1,false,now);}} // the busy flip that let the v13 grips through
      assert(H::nativeGripPresses.load()==native0);for(int i=0;i<3;++i)step(0.f,0.f,false);}
     GL::holdLatch=true;
     // an armed chord on two empty hands: its squeeze is the chord's (captured: masked until open), it still fires after 2 s; a busy
     // flip in the middle that lets the raw grips through never makes them LB / RB
     {for(int i=0;i<4;++i)step(0.f,0.f,false);
      const auto native0=H::nativeGripPresses.load(),owned0=GL::chordOwnedPresses[0].load()+GL::chordOwnedPresses[1].load(),fired0=R::theaterChordStats.fired.load();
      std::uint32_t leaked=0;
      for(int i=0;i<160;++i){leaked|=step(1.f,1.f,false);if(i==80){GL::setReady(0,true,now);leaked|=step(1.f,1.f,false);GL::setReady(0,false,now);}}
      assert(leaked==0&&H::nativeGripPresses.load()==native0&&GL::chordOwnedPresses[0].load()+GL::chordOwnedPresses[1].load()==owned0+2);
      assert(R::theaterChordStats.fired.load()==fired0);// the busy flip re-armed the chord: the 2 s start again after a full release
      for(int i=0;i<3;++i)step(0.f,0.f,false);
      for(int i=0;i<160;++i)leaked|=step(1.f,1.f,false);
      assert(leaked==0&&R::theaterChordStats.fired.load()==fired0+1);
      for(int i=0;i<3;++i)step(0.f,0.f,false);
      assert(step(0.f,1.f,false)==R::PadRightGrip&&H::nativeGripPresses.load()==native0+1); // one hand alone: native RB
      for(int i=0;i<3;++i)step(0.f,0.f,false);}
     // a hold that ends (mode) as the world suspends (a cutscene) with both grips closed: the published mask keeps the chord off while the
     // grips stay closed and goes stale without the brain; the hands open in the suspension: the published mask drops them (the chord free
     // again) while the captures stay for the resume; squeezed again in the suspension: still out of the mask, and the resume with the grips
     // closed still masks them (no LB / RB); opened in the world: a fresh press is the game's LB again. nativeGripPresses counts the
     // brain's routed latches only (a suspended frame routes nothing)
     {for(int i=0;i<4;++i)step(0.f,0.f,true);
      assert(step(1.f,1.f,true)==0);GL::setHolding(0,true,now);GL::setHolding(1,true,now);for(int i=0;i<10;++i)assert(step(1.f,1.f,false)==0);
      GL::lastEnd[0]=8;GL::lastEnd[1]=8;GL::setHolding(0,false,0);GL::setHolding(1,false,0);GL::setReady(0,false,now);GL::setReady(1,false,now);
      const auto native0=H::nativeGripPresses.load();
      worldOn=false;for(int i=0;i<5;++i)step(1.f,1.f,false); // (a suspended frame leaves the pad to the caller: no routing, no masks)
      assert(GL::capturedMask.load()==3);
      {const auto b0=GL::chordCapturedBusy.load();H::gripsEngaged();assert(GL::chordCapturedBusy.load()==b0+1);
       std::this_thread::sleep_for(std::chrono::milliseconds(300)); // no brain frame for 300 ms: the mask is stale
       H::gripsEngaged();assert(GL::chordCapturedBusy.load()==b0+1&&!GL::capturedFresh(rdrvr::vr::nowNs()));}
      for(int i=0;i<3;++i)step(0.f,0.f,false);
      assert(GL::capturedMask.load()==0&&GL::capturedFresh(rdrvr::vr::nowNs()));
      {const auto b0=GL::chordCapturedBusy.load();H::gripsEngaged();assert(GL::chordCapturedBusy.load()==b0);}
      for(int i=0;i<10;++i){step(1.f,1.f,false);assert(GL::capturedMask.load()==0);}
      worldOn=true;for(int i=0;i<5;++i)assert(step(1.f,1.f,false)==0);
      assert(H::nativeGripPresses.load()==native0);
      for(int i=0;i<3;++i)step(0.f,0.f,false);assert(GL::capturedMask.load()==0);
      assert(step(1.f,0.f,false)==R::PadLeftGrip&&H::nativeGripPresses.load()==native0+1);for(int i=0;i<3;++i)step(0.f,0.f,false);}
     in.grip[1]=in.aim[1]=pose(fistX,fistY,fistZ);}

    // The report carries the fields.
    const auto report=H::report();
    for(const char* key:{"\"keepGun\":{\"on\":","\"tailPolls\":","\"edgeReads\":","\"engineLeavesDeadEye\":","\"restores\":{\"requests\":","\"waitedKept\":","\"forcedHolster\":",
                         "\"lastLeave\":{\"cause\":","\"sinceForceDropMs\":","\"sinceDeadEyeEndMs\":","\"sinceDuelEndMs\":"})
        assert(report.find(key)!=std::string::npos);
    std::cout<<"keep gun tests passed\n";
}
