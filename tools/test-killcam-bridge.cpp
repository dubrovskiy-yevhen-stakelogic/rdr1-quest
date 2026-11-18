// Host regression: run with a temporary directory as argv[1] (unused). Build as tools/test-casual-trigger.cpp.
// STEREO BRIDGE (runtime/StereoBridge.h) and the scripted-shot side view (vr/VrCutscene.h): the change decision, the hold timer,
// the mailbox age verdict, the tracking gate on the bridge path, the shared shot eligibility, the composer mode for a shot,
// the menu-between regression and the per-shot report counters, replaying the XR frame sequence of a close-range kill.
#include "runtime/StereoBridge.h"
#include "runtime/Policies.h"
#include "vr/VrCutscene.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
namespace B=rdrvr::runtime::bridge;
namespace R=rdrvr::runtime;
namespace C=rdrvr::vr::cutscene;
using P=R::PresentationContext;
static constexpr std::uint64_t ms=1'000'000,s=1'000'000'000ull;

int main(int argc,char**) {
    assert(argc==2);
    // ---- 1. bridgeOnChange table ----
    static_assert(B::bridgeOnChange(true,P::CameraOverride,true,true));   // World -> Override (shot)
    static_assert(B::bridgeOnChange(true,P::World,false,true));           // Override (shot) -> World
    static_assert(B::bridgeOnChange(true,P::World,false,false));          // Cutscene -> World (any row: World is always stereo)
    static_assert(B::bridgeOnChange(true,P::Cutscene,false,true));        // World -> Cutscene, CINEMATIC / FIRST PERSON
    static_assert(B::bridgeOnChange(true,P::CameraTransition,false,true));
    static_assert(!B::bridgeOnChange(true,P::CameraOverride,false,true)); // World -> Override (plain / expired)
    static_assert(!B::bridgeOnChange(false,P::World,false,true));         // Override (expired, theater) -> World
    static_assert(!B::bridgeOnChange(true,P::Menu,true,true));            // World -> Menu
    static_assert(!B::bridgeOnChange(false,P::World,true,true));          // Menu -> World (previous frame not stereo)
    static_assert(!B::bridgeOnChange(true,P::Optic,true,true));           // World -> Optic (the existing retire path)
    static_assert(!B::bridgeOnChange(true,P::Cutscene,false,false));      // World -> Cutscene with SCREEN
    static_assert(!B::bridgeOnChange(true,P::CameraOverride,true,false)); // a shot with SCREEN
    static_assert(!B::bridgeOnChange(true,P::Unknown,true,true));
    static_assert(!B::bridgeOnChange(true,P::World,false,true,true));     // a Menu revision the XR thread never sampled
    static_assert(B::classify(false,P::World,true,true)==B::Decline::NotStereoBefore);
    static_assert(B::classify(true,P::Menu,true,true)==B::Decline::NotCandidate);
    static_assert(B::classify(true,P::World,true,true,false,true)==B::Decline::SpaceChanged);
    static_assert(B::changeAction(true,B::Decline::None,300)==B::ChangeAction::RetireContent);
    static_assert(B::changeAction(false,B::Decline::None,300)==B::ChangeAction::Bridge);
    static_assert(B::changeAction(false,B::Decline::None,0)==B::ChangeAction::Invalidate); // lever 0 = previous behaviour
    static_assert(B::changeAction(false,B::Decline::NotCandidate,300)==B::ChangeAction::Invalidate);

    // ---- 2. bridge timer ----
    {
        B::Timer t;const std::uint64_t t0=100*s;
        assert(!t.armed()&&!t.active(t0));
        t.arm(t0,300,1);assert(t.armed()&&t.active(t0)&&t.active(t0+299*ms)&&!t.active(t0+300*ms));
        assert(t.update(t0+50*ms,1,false,false)==B::Timer::End::None);
        assert(t.update(t0+60*ms,1,true,false)==B::Timer::End::None);   // World again but the shown pair is older than the revision
        assert(t.update(t0+70*ms,1,false,true)==B::Timer::End::None);   // a new-revision pair but no stereo gate yet
        assert(t.update(t0+95*ms,1,true,true)==B::Timer::End::Engaged&&!t.armed());
        t.arm(t0,300,1);
        assert(t.update(t0+299*ms,1,false,false)==B::Timer::End::None);
        assert(t.update(t0+300*ms,1,false,false)==B::Timer::End::Expired&&!t.active(t0+300*ms));
        t.arm(t0,300,1);assert(t.update(t0+10*ms,2,true,true)==B::Timer::End::SpaceChanged&&!t.armed());
        // a second change while armed keeps the first start, never past 2x the lever
        t.arm(t0,300,1);t.arm(t0+250*ms,300,1);assert(t.active(t0+549*ms)&&!t.active(t0+550*ms));
        t.arm(t0+500*ms,300,1);assert(t.active(t0+599*ms)&&!t.active(t0+600*ms));
        t.cancel();assert(!t.active(t0+1*ms));
        B::noteEnd(B::Timer::End::Engaged,t0+95*ms,t0);assert(B::stats.ended.load()==1&&B::stats.lastMs.load()==95);
    }

    // ---- 3. mailbox age verdict (maxAge 150 ms) ----
    {
        constexpr std::uint64_t maxAge=150*ms;
        static_assert(B::ageVerdict(105*ms,maxAge,false,false,true,true)==B::Age::Fresh); // the measured entry / exit gaps: normal repeat path
        static_assert(B::ageVerdict(133*ms,maxAge,false,true,true,true)==B::Age::Fresh);
        static_assert(B::ageVerdict(200*ms,maxAge,false,true,true,true)==B::Age::Hold);
        static_assert(B::ageVerdict(200*ms,maxAge,false,false,true,true)==B::Age::Reject); // bridge expired: no layer -> theater
        static_assert(B::ageVerdict(200*ms,maxAge,true,true,true,true)==B::Age::Stale);
        static_assert(B::ageVerdict(200*ms,maxAge,false,true,false,true)==B::Age::Reject); // nothing displayed
        static_assert(B::ageVerdict(200*ms,maxAge,false,true,true,false)==B::Age::Reject); // other tracking / space epoch
    }

    // ---- 4. tracking gate: the bridge path never takes the 2-sample lost interval ----
    {
        R::TrackingGate g;assert(!g.sample(true)&&g.sample(true)&&g.ready());const auto epoch=g.epoch;
        if(B::changeAction(false,B::Decline::None,300)!=B::ChangeAction::Bridge)assert(false);
        assert(g.sample(true)&&g.ready()&&g.epoch==epoch); // bridge: no invalidate
        g.invalidate();assert(!g.sample(true)&&g.sample(true)); // the full path: one frame without tracking
    }

    // ---- 5. shot eligibility ----
    {
        const std::uint64_t since=50*s;
        static_assert(!B::overrideEligible(0,1,15,2));
        assert(B::overrideEligible(since,since,15,2));               // from the first published tick
        assert(B::overrideEligible(since,since-1,15,2));             // a thread whose clock read precedes the store
        assert(B::overrideEligible(since,since+15*s,15,2)&&!B::overrideEligible(since,since+15*s+1,15,2));
        assert(B::overrideEligible(since,since+3600*s,0,2));         // shotmax 0 = no limit
        assert(B::overrideEligible(0,5,15,1)&&!B::overrideEligible(since,since,15,0));
        C::overrideMode.store(2);C::shotMaxSeconds.store(15);
        B::shotSinceNs.store(since);assert(C::overrideEligible(since+1));
        B::shotSinceNs.store(0);assert(!C::overrideEligible(since+1));
    }

    // ---- 6. composer: a shot in a FIRST PERSON row is CINEMATIC ----
    {
        static_assert(C::modeFor(C::Ctx::Override,C::Ctx::World,C::FirstPerson,true)==C::Cinematic);
        static_assert(C::modeFor(C::Ctx::Transition,C::Ctx::Override,C::FirstPerson,true)==C::Cinematic);
        static_assert(C::modeFor(C::Ctx::Transition,C::Ctx::Cutscene,C::FirstPerson,true)==C::FirstPerson);
        static_assert(C::modeFor(C::Ctx::Override,C::Ctx::World,C::FirstPerson,false)==C::FirstPerson);
        static_assert(C::modeFor(C::Ctx::Cutscene,C::Ctx::World,C::FirstPerson,true)==C::FirstPerson);
        static_assert(C::modeFor(C::Ctx::Override,C::Ctx::World,C::Screen,true)==C::Screen);
        static_assert(C::modeFor(C::Ctx::Override,C::Ctx::World,C::Cinematic,true)==C::Cinematic);
        float stock[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 10,2,5,1}; // looks down -z from (10, 2, 5)
        const auto input=[&](C::Ctx ctx,unsigned mode,std::uint64_t ns){
            C::Input in{};in.ns=ns;in.revision=7;in.ctx=ctx;in.mode=mode;in.stock=stock;in.viewsValid=true;in.hmdYaw=0.f;in.hmdCenter={0,1.6f,0};in.referenceSpace=1;
            in.john.ns=ns;in.john.revision=7;in.john.valid=true;in.john.eye={10.5f,1.7f,5.f};in.john.headingValid=true;in.john.heading=.3f;in.john.creature=0x1234;in.john.hat=0x99;in.john.guid=5;
            return in;};
        const std::uint64_t now=200*s;B::shotSinceNs.store(now-100*ms);
        C::Composer cmp;C::Output o{};std::uintptr_t creature{},hat{};
        cmp.note(C::Ctx::Override);
        auto in=input(C::Ctx::Override,C::modeFor(C::Ctx::Override,cmp.steady(),C::FirstPerson,true),now);
        assert(cmp.step(in,o)&&o.effective==C::Cinematic&&o.guid==C::guidCinematic);
        C::anchorBody(o,creature,hat);assert(creature==1&&hat==0); // nobody hidden, no head fill
        C::Composer fp;fp.note(C::Ctx::Override);
        in=input(C::Ctx::Override,C::modeFor(C::Ctx::Override,fp.steady(),C::FirstPerson,false),now);
        assert(fp.step(in,o)&&o.effective==C::FirstPerson&&o.guid==C::guidFirstPerson);
        C::anchorBody(o,creature,hat);assert(creature==0x1234&&hat==0x99);
        C::Composer cut;cut.note(C::Ctx::Cutscene);
        in=input(C::Ctx::Cutscene,C::modeFor(C::Ctx::Cutscene,cut.steady(),C::FirstPerson,true),now);
        assert(cut.step(in,o)&&o.effective==C::FirstPerson);
        // an expired shot is not eligible: no stereo composition (the theater)
        C::Composer late;late.note(C::Ctx::Override);B::shotSinceNs.store(now-16*s);
        in=input(C::Ctx::Override,C::Cinematic,now);assert(!late.step(in,o));
        B::shotSinceNs.store(0);
    }

    // ---- 7. regression: a Menu between two World samples still invalidates fully ----
    {
        R::PresentationMailbox box;R::PresentationGate gate;std::uint64_t t=10*s;
        box.publish(P::World,t);assert(gate.update(box.snapshot(t)));const auto seen=gate.revision();
        box.publish(P::Menu,t+=5*ms);box.publish(P::World,t+=5*ms); // the XR thread never samples the menu
        const auto snap=box.snapshot(t);assert(snap.menuRevision>seen&&snap.changedNs==t);
        assert(gate.update(snap)&&gate.world()&&gate.context()==P::World);
        assert(B::classify(true,snap.context,false,true,snap.menuRevision>seen)==B::Decline::NotCandidate);
        // World -> Override -> World without a menu: bridged
        const auto seen2=gate.revision();box.publish(P::CameraOverride,t+=20*ms);const auto snap2=box.snapshot(t);
        assert(gate.update(snap2)&&snap2.menuRevision<=seen2&&B::bridgeOnChange(true,snap2.context,true,true,snap2.menuRevision>seen2));
    }

    // ---- shot runs + edge meter: the kill of the device log, as it should look with the bridge ----
    {
        B::ShotRuns runs;B::EdgeMeter edges;std::uint64_t t=1000*s;
        // World stereo before the shot
        for(int i=0;i<5;++i){t+=28*ms;edges.frame(t,1,true,P::World,runs);}
        const std::uint64_t lastWorldPair=t;
        runs.begin(t+5*ms,"camCutsceneCamera");edges.change(P::World,P::CameraOverride,t+5*ms);
        assert(runs.inWindow(t+6*ms));
        // entry: 2 held frames of the last world pair, one in-flight old-revision pair, then the shot's pairs
        t+=14*ms;edges.frame(t,3,false,P::CameraOverride,runs);
        t+=14*ms;edges.frame(t,3,false,P::CameraOverride,runs);
        t+=14*ms;edges.frame(t,1,false,P::CameraOverride,runs);
        t+=14*ms;edges.frame(t,1,true,P::CameraOverride,runs);const auto enterGap=std::int32_t((t-lastWorldPair)/ms);
        for(int i=0;i<20;++i){t+=28*ms;edges.frame(t,i%5==3?2:1,true,P::CameraOverride,runs);}
        runs.bump(B::Eligible,31);runs.bump(B::Composed,31);runs.bump(B::Stereo,31);runs.setMode(C::Cinematic);
        runs.noteReject("WAITING_FOR_STEREO_CULL",t);runs.noteReject("NATIVE_UI_THEATER",t);
        const std::uint64_t lastShotPair=t;
        runs.end(t+3*ms);edges.change(P::CameraOverride,P::World,t+3*ms);
        t+=14*ms;edges.frame(t,3,false,P::World,runs);
        t+=14*ms;edges.frame(t,1,true,P::World,runs);const auto exitGap=std::int32_t((t-lastShotPair)/ms);
        runs.noteReject("FROZEN_TRACKING_EXPIRED",t);runs.noteReject("CULL_SNAPSHOT_UNAVAILABLE",t+900*ms);
        t+=28*ms;edges.frame(t,1,true,P::World,runs); // after the exit edge: not counted
        assert(!edges.exitOpen());
        B::noteHeadDraws(0,0,t); // a CINEMATIC eye scope that hid nothing
        const auto r=runs.current();
        assert(r.enterGapMs==enterGap&&r.exitGapMs==exitGap);
        assert(r.n[B::XrTheater]==0&&r.n[B::XrNone]==0);
        assert(r.n[B::XrHeld]==3&&r.n[B::XrNew]==2+16+1&&r.n[B::XrRepeat]==4);
        assert(r.n[B::RejStereoCull]==1&&r.n[B::RejFrozen]==1&&r.n[B::RejCullSnap]==1&&r.n[B::HeadHidden]==0&&r.n[B::HeadFill]==0);
        assert(!runs.inWindow(r.endNs+s));
        runs.noteReject("WAITING_FOR_STEREO_CULL",r.endNs+s);assert(runs.current().n[B::RejStereoCull]==1);
        // the next run moves this one to the ring
        runs.begin(t+10*s,"camCamera");runs.begin(t+11*s,"ignored"); // a repeated begin while running keeps the run
        const auto rep=runs.report(t+12*s);
        assert(rep.find("\"total\":2")!=std::string::npos&&rep.find("\"camCutsceneCamera\",\"CINEMATIC\",31,31,31,19,4,3,0,0")!=std::string::npos);
        assert(rep.find("\"camCamera\"")!=std::string::npos&&rep.find("ignored")==std::string::npos);
        std::cout<<rep<<"\n";
    }

    // ---- exit window: gameplay head draws after the run (head hidden again / fail-closed misses) ----
    {
        const std::uint64_t t0=3000*s;
        B::noteExitHead(4,1,t0);assert(!B::shots.current().startNs); // no run: nothing
        B::shots.begin(t0,"camCutsceneCamera");assert(!B::shots.ended()&&!B::shots.exitWindow(t0+1*ms));
        B::noteExitHead(2,1,t0+10*ms);assert(B::shots.current().n[B::ExitHeadFill]==0&&B::shots.current().n[B::ExitHeadMiss]==0); // still running
        B::shots.end(t0+700*ms);assert(B::shots.ended()&&B::shots.exitWindow(t0+700*ms)&&!B::shots.exitWindow(t0+699*ms));
        B::noteExitHead(1,1,t0+720*ms);B::noteExitHead(3,0,t0+750*ms);
        B::noteExitHead(5,5,t0+700*ms+s); // past the window
        const auto r=B::shots.current();assert(r.n[B::ExitHeadFill]==4&&r.n[B::ExitHeadMiss]==1);
        const auto rep=B::shots.report(t0+2*s);
        assert(rep.find("headFillDraws,exitHeadFills,exitHeadMisses\"")!=std::string::npos&&rep.find(",0,0,4,1]")!=std::string::npos);
    }

    // ---- revision pairs: the bridge ends on the first pair of the new revision ----
    {
        B::RevisionPairs rp;rp.note(4,7237);rp.note(4,7238);assert(rp.reached(4,7237)&&!rp.reached(5,7240));
        rp.note(5,7242);assert(!rp.reached(5,7241)&&rp.reached(5,7242)&&rp.reached(5,7250)&&!rp.reached(4,7250));
        rp.note(0,1);rp.note(6,0);assert(rp.reached(5,7242));
    }

    // ---- report ----
    {
        B::noteXrKind(P::CameraOverride,1);B::noteXrKind(P::CameraOverride,3);B::noteXrKind(P::Unknown,9);
        const auto rep=B::report(1);
        assert(rep.find("\"bridge\":{\"lever\":300,")==0);
        assert(rep.find("\"CAMERA_OVERRIDE\":{\"NONE\":0,\"NEW\":1,\"REPEAT\":0,\"HELD\":1,\"THEATER\":0,\"DIAGNOSTIC\":0}")!=std::string::npos);
        assert(rep.find("\"UNKNOWN\"")==std::string::npos);
        std::cout<<rep<<"\n";
    }

    // ---- the vr.cutscene report carries the new sections and stays one JSON object ----
    {
        const auto rep=C::report();int depth=0,minDepth=1;bool str=false;
        for(std::size_t i=0;i<rep.size();++i){const char c=rep[i];
            if(c=='"'&&(i==0||rep[i-1]!='\\'))str=!str;
            if(str)continue;
            if(c=='{'||c=='[')++depth;
            else if(c=='}'||c==']'){--depth;if(i+1<rep.size()&&depth<minDepth)minDepth=depth;}}
        assert(depth==0&&minDepth>=1&&!str);
        assert(rep.find("],\"shotView\":1,\"bridge\":{")!=std::string::npos);
        assert(rep.find("\"shots\":{\"total\":")!=std::string::npos&&rep.find("},\"follow\":{")!=std::string::npos);
    }
    std::cout<<"test-killcam-bridge: all checks passed\n";
    return 0;
}
