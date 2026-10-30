// Host regression (MSVC: cl /std:c++20 /EHsc /I<fake sys/system_properties.h> /Inative/src /Inative/src/vr): throwable::Brain input sequences.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "vr/VrThrowable.h"
namespace TB=rdrvr::vr::throwable;
namespace R=rdrvr::runtime;
static int fails=0;
#define CHECK(c,msg) do{if(!(c)){std::printf("FAIL %s  (%s)\n",msg,#c);++fails;}else std::printf("ok   %s\n",msg);}while(0)
static std::uint64_t now=1'000'000'000ull;
struct Rig {
    TB::Brain b{};R::ControllerSample in{};R::PadState pad{};int hold{1},kind{8};bool aimUp{true},toggle{},reins{};unsigned rtFrames{};
    Rig(){in.sequence=1;in.epoch=in.continuity=in.referenceSpace=1;in.active=in.focused=true;}
    bool frame(float trig,float grip,int dt_ms=11){now+=std::uint64_t(dt_ms)*1'000'000ull;++in.sequence;in.publishedNs=now;
        in.triggers={0.f,0.f};in.grips={0.f,0.f};if(hold>=0){in.triggers[unsigned(hold)]=trig;in.grips[unsigned(hold)]=grip;}
        TB::RouteIn r{};r.in=&in;r.hold=hold;r.kind=kind;r.aimUp=aimUp;r.toggle=toggle;r.reins=reins;r.now=now;pad={};
        const bool o=b.route(r,pad);if(pad.triggers[1]>0.f)++rtFrames;return o;}
    void run(float trig,float grip,int ms){for(int t=0;t<ms;t+=11)frame(trig,grip);}
    void settle(){run(0.f,in.grips[unsigned(hold<0?1:hold)],2200);} // any pending throw expires
};
static std::uint64_t seq(){return TB::requestSeq.load();}
int main(){
    TB::startup();
    CHECK(TB::active(),"throwable active (THROWABLES VR default)");
    CHECK(TB::config.minAimNs==30'000'000ull&&TB::config.downDot<.71f&&TB::config.relWaitNs==150'000'000ull,"defaults: debounce 30 ms, down .7, release window 150 ms");
    { // 1 full-fist grab (trigger already squeezed) then the fist relaxes - no aim, no throw
        Rig g;const auto s0=seq();const auto a0=TB::stats.aimStarts.load();
        g.run(1.f,1.f,300);
        CHECK(TB::stats.aimStarts.load()==a0,"carried squeeze starts no aim");
        CHECK(!g.b.wantsAim(true,now),"carried squeeze wants no force aim (no lit fuse)");
        g.run(0.f,1.f,300);
        CHECK(seq()==s0&&g.rtFrames==0,"releasing a carried squeeze throws nothing");
        g.run(1.f,1.f,300);CHECK(TB::stats.aimStarts.load()==a0+1,"fresh press after the latch aims");
        CHECK(g.b.wantsAim(true,now),"aiming wants force aim");
        g.run(0.f,1.f,100);CHECK(seq()==s0,"no post inside the 150 ms window");
        g.run(0.f,1.f,100);CHECK(seq()==s0+1,"aimed throw posted after the window, grip closed");
        g.run(0.f,1.f,400);CHECK(g.rtFrames==3,"RT pulse = 3 frames");
        g.settle();
    }
    { // 2 aim, then the whole hand opens with the trigger (put-away) - cancel
        Rig g;g.frame(0.f,1.f);const auto s0=seq();const auto c0=TB::stats.aimOpenCancels.load();
        g.run(1.f,1.f,300);g.frame(0.f,.9f);g.frame(0.f,.6f);g.frame(0.f,.3f);g.run(0.f,0.f,300);
        CHECK(seq()==s0&&TB::stats.aimOpenCancels.load()==c0+1&&g.rtFrames==0,"trigger + grip opening together = put-away, no throw");
    }
    { // 3 opening hand still above gripOpen at the decision: grip .95 -> .40 within the window (openDrop 1.01 = off -> a throw)
        Rig g;g.frame(0.f,.95f);const auto s0=seq();g.run(1.f,.95f,300);
        for(int i=0;i<12;++i)g.frame(0.f,.95f-.05f*float(i));g.run(0.f,.4f,200);
        CHECK(seq()==s0+1,"openDrop off - a hand still at .40 throws (only an open hand cancels)");
    }
    { // 3b a hand that only RELAXES after the release (1.0 -> .55, drop .45) still throws
        Rig g;g.frame(0.f,1.f);const auto s0=seq();g.run(1.f,1.f,300);
        for(int i=0;i<10;++i)g.frame(0.f,1.f-.045f*float(i));g.run(0.f,.55f,200);
        CHECK(seq()==s0+1,"relaxing hand = aimed throw");g.settle();
    }
    { // 4 a LOOSE hold (grip .4 steady, above gripOpen .30) throws
        Rig g;g.frame(0.f,.4f);const auto s0=seq();g.run(1.f,.4f,300);g.run(0.f,.4f,250);
        CHECK(seq()==s0+1,"loose hold at .40 = aimed throw");g.settle();
    }
    { // 5 the Quest middle-finger artifact: grip dips to .45 while the trigger is pulled, recovers on release
        Rig g;g.frame(0.f,1.f);const auto s0=seq();g.run(1.f,.45f,300);g.frame(0.f,.5f);g.run(0.f,.95f,250);
        CHECK(seq()==s0+1,"grip recovering after the trigger = throw");g.settle();
    }
    { // 6 TOGGLE: the item in an open hand (grip .1) throws
        Rig g;g.toggle=true;g.frame(0.f,.1f);const auto s0=seq();g.run(1.f,.1f,300);g.run(0.f,.1f,250);
        CHECK(seq()==s0+1,"TOGGLE grip .1 = aimed throw");g.settle();
    }
    { // 7 holsters release() (HOLD grip opened) inside the window cancels
        Rig g;g.frame(0.f,1.f);const auto s0=seq();g.run(1.f,1.f,300);g.frame(0.f,1.f);g.b.cancelRelease();g.run(0.f,1.f,300);
        CHECK(seq()==s0&&!g.b.aiming(),"release() cancels the pending aimed throw");
    }
    { // 8 taps: 20 ms bounce no throw, 60 ms tap throws
        Rig g;g.frame(0.f,1.f);const auto s0=seq();g.frame(1.f,1.f);g.frame(1.f,1.f,9);g.frame(0.f,1.f,9);g.run(0.f,1.f,250);
        CHECK(seq()==s0,"20 ms bounce = no throw");
        g.run(1.f,1.f,60);g.run(0.f,1.f,250);CHECK(seq()==s0&&g.b.wantsAim(true,now),"60 ms tap on dynamite = LIGHT (stance), no throw");g.settle();
        Rig k;k.kind=7;k.frame(0.f,1.f);const auto s1=seq();k.run(1.f,1.f,60);k.run(0.f,1.f,250);CHECK(seq()==s1+1,"60 ms knife tap = quick throw");k.settle();
    }
    { // 9 the reins brain holds the hand (back on the rein after a physical throw): the rein squeeze never aims / throws
        Rig g;g.frame(0.f,1.f);g.reins=true;const auto s0=seq();const auto a0=TB::stats.aimStarts.load();
        g.run(1.f,1.f,400);g.run(0.f,1.f,300);g.run(1.f,1.f,300);
        CHECK(TB::stats.aimStarts.load()==a0&&seq()==s0&&!g.b.wantsAim(true,now),"rein hand: no aim, no throw, no force aim");
        g.reins=false;g.run(1.f,1.f,100);
        CHECK(TB::stats.aimStarts.load()==a0,"leaving the rein with the trigger still squeezed is no fresh press");
        g.run(0.f,1.f,100);g.run(1.f,1.f,200);CHECK(TB::stats.aimStarts.load()==a0+1,"a fresh press after the rein aims");
        g.run(0.f,1.f,300);g.settle();
    }
    { // 10 a one-frame kind -1 while aiming keeps the aim
        Rig g;g.frame(0.f,1.f);const auto s0=seq();const auto c0=TB::stats.aimCancelled.load();
        g.run(1.f,1.f,200);g.kind=-1;const bool o=g.frame(1.f,1.f);g.kind=8;g.run(1.f,1.f,50);
        CHECK(o,"kind -1 for one frame: still owned (no gun routing of the aim trigger)");
        CHECK(TB::stats.aimCancelled.load()==c0,"kind glitch cancels nothing");
        g.run(0.f,1.f,250);CHECK(seq()==s0+1,"throw after the glitch");g.settle();
    }
    { // 11 a pending pulse never survives a switch to a gun in the same hand
        Rig g;g.frame(0.f,1.f);g.aimUp=false;
        g.run(1.f,1.f,300);g.run(0.f,1.f,170);
        const unsigned rt0=g.rtFrames;
        g.kind=1;g.run(0.f,1.f,50);g.run(0.f,1.f,3000);
        g.kind=8;g.run(0.f,1.f,300);
        CHECK(g.rtFrames==rt0,"stale pulse dropped (no RT at the dynamite draw)");
        CHECK(TB::stats.pulsesDropped.load()>=1,"pulsesDropped counted");
        g.settle();
    }
    { // 12 lasso/duel drop clears the aim
        Rig g;g.frame(0.f,1.f);g.run(1.f,1.f,200);CHECK(g.b.aiming(),"aiming before drop");
        g.b.drop();CHECK(!g.b.aiming()&&!g.b.wantsAim(true,now),"drop: no aim, no force aim");
        CHECK(TB::aimHand.load()==-1,"drop hides the aim arc");
    }
    { // 13 physical release direction rules + the released hand
        Rig g;g.frame(0.f,1.f);now+=3'000'000'000ull;
        rdrvr::vr::throwing::ReleaseEstimator::Estimate e{};e.valid=true;e.speedPeak=3.5f;e.speedRelease=2.5f; // above the MIN SWING default and its 3.0 maximum
        const float c50=std::cos(50.f*3.14159265f/180.f),s50=std::sin(50.f*3.14159265f/180.f);
        e.v={0.f,-1.5f*s50,-1.5f*c50};
        const auto d0=TB::stats.down.load();
        int r=g.b.physical(1,g.in,e,false,false,false,8,now);CHECK(r==0&&TB::stats.down.load()==d0+1,"dynamite 50 deg down = put-away (was a throw at John's feet)");
        r=g.b.physical(1,g.in,e,false,false,false,7,now);CHECK(r==0,"tomahawk 50 deg down (> 44 deg of .7) = put-away");
        const float c25=std::cos(25.f*3.14159265f/180.f),s25=std::sin(25.f*3.14159265f/180.f);
        e.v={0.f,-3.f*s25,-3.f*c25};e.speedPeak=3.5f;e.speedRelease=3.f;
        r=g.b.physical(1,g.in,e,false,false,false,8,now);CHECK(r==1,"dynamite 25 deg down overhand = a throw");
        CHECK(TB::releasePending(1,now),"released hand published for the reins");
        CHECK(!TB::releasePending(0,now),"other hand not");
        CHECK(TB::releasePending(1,now+2'000'000'000ull)&&!TB::releasePending(1,now+3'500'000'000ull),"released hand expires after maxDelay + explodeExtra (dynamite) + settle");
        TB::launchedNs.store(now+100'000'000ull);TB::launchedSeq.store(TB::requestSeq.load());
        CHECK(TB::releasePending(1,now+300'000'000ull)&&!TB::releasePending(1,now+500'000'000ull),"launched: pending until launch + settle 350 ms");
        // a later (aimed) throw never reopens the old physical release
        now+=3'000'000'000ull;g.run(1.f,1.f,300);g.run(0.f,1.f,250);
        TB::launchedNs.store(now);TB::launchedSeq.store(TB::requestSeq.load());
        CHECK(!TB::releasePending(1,now+100'000'000ull),"a later aimed launch does not reopen the released hand");
    }
    // the THROWN_EXPLODING RT press chain + the LIGHT tap
    const auto swing=[](){rdrvr::vr::throwing::ReleaseEstimator::Estimate e{};e.valid=true;e.speedPeak=4.f;e.speedRelease=3.f;e.v={0.f,.3f,-3.5f};return e;};
    const auto aimBox=[](std::int32_t state,bool lit,std::int16_t type=0x18,std::int32_t cls=3){TB::EngineAim a{};a.ns=now;a.valid=1;a.state=state;
        a.stateChange=lit?0x4000:0;a.typeId=type;a.typeClass=cls;TB::engineAimBox.store(a);};
    const auto unread=[](){TB::EngineAim a{};a.ns=now;TB::engineAimBox.store(a);};
    { // 14 dynamite swing, engine aim unreadable: 3 presses (tap, held, tap) spaced by the blind window, then the chain ends
        Rig g;g.frame(0.f,1.f);now+=3'000'000'000ull;unread();auto& K=TB::perKind[1];
        const auto p0=K.presses.load(),h0=K.holdPresses.load(),n0=K.noStart.load(),s0=seq();
        CHECK(g.b.physical(1,g.in,swing(),false,false,false,8,now)==1&&seq()==s0+1,"dynamite swing posts a physical throw");
        for(int t=0;t<3300;t+=11){unread();g.frame(0.f,0.f);}
        CHECK(K.presses.load()==p0+3&&K.holdPresses.load()==h0+1,"3 presses, the 2nd held");
        CHECK(g.rtFrames>=40&&g.rtFrames<=50,"RT frames = 3 + ~41 held + 3");
        CHECK(K.noStart.load()==n0+1,"no start seen = noStart");
        g.settle();
    }
    { // 15 engine aim ready, the engine starts the throw after the first press: no second press
        Rig g;g.frame(0.f,1.f);now+=3'000'000'000ull;aimBox(6,false);auto& K=TB::perKind[1];
        const auto p0=K.presses.load(),st0=K.started.load(),o0=K.startedOn[1].load(),r0=K.pressesReady.load();
        g.b.physical(1,g.in,swing(),false,false,false,8,now);bool set=false;
        for(int t=0;t<2500;t+=11){aimBox(set?13:6,false);g.frame(0.f,0.f);if(g.rtFrames&&!set){set=true;TB::engineThrowStartNs.store(now);}}
        CHECK(K.presses.load()==p0+1&&K.pressesReady.load()==r0+1&&g.rtFrames==3,"one ready press");
        CHECK(K.started.load()==st0+1&&K.startedOn[1].load()==o0+1,"started on attempt 1");
        g.settle();
    }
    { // 16 engine aim-in (state 2) for 500 ms: the press waits for the engine (ScheduleThrow refuses states 0..4)
        Rig g;g.frame(0.f,1.f);now+=3'000'000'000ull;aimBox(2,false);
        g.b.physical(1,g.in,swing(),false,false,false,8,now);const auto t0=now;std::uint64_t first=0;
        for(int t=0;t<1500;t+=11){aimBox(now-t0<500'000'000ull?2:6,false);g.frame(0.f,0.f);if(g.rtFrames&&!first)first=now;}
        CHECK(first&&first-t0>=500'000'000ull,"first press only once the engine aim is ready");
        TB::engineThrowStartNs.store(now);g.settle();
    }
    { // 17 LIGHT tap on unlit dynamite: no throw, the aim stance kept; then a long aim throws
        Rig g;g.frame(0.f,1.f);now+=3'000'000'000ull;unread();const auto s0=seq(),l0=TB::perKind[1].lightTaps.load();
        g.run(1.f,1.f,150);g.run(0.f,1.f,400);
        CHECK(seq()==s0&&TB::perKind[1].lightTaps.load()==l0+1,"tap = LIGHT, no throw");
        CHECK(g.b.wantsAim(true,now),"lit stance wants force aim");
        g.run(1.f,1.f,600);g.run(0.f,1.f,300);CHECK(seq()==s0+1,"long aim after the light = aimed throw");
        TB::launchedNs.store(now);TB::launchedSeq.store(TB::requestSeq.load());g.settle();
        CHECK(!g.b.wantsAim(true,now),"after the launch the stance ends (next stick unlit)");
    }
    { // 18 engine fuse flag (fire bottle lit at its draw) = held stance; knife tap still an aimed throw; knife chain = 1 press
        Rig g;g.frame(0.f,1.f);now+=3'000'000'000ull;aimBox(6,true,0x10);CHECK(g.b.wantsAim(true,now),"lit at its draw (class 3, type 0x10): aim stance");
        aimBox(6,true,0x18);CHECK(!g.b.wantsAim(true,now),"a 0x4000 left on an aim-lit type (0x18) raises no stance on a fresh stick");
        aimBox(6,true,-1,-1);CHECK(!g.b.wantsAim(true,now),"unread type = no stance from the flag");
        {TB::EngineAim a{};a.ns=now+20'000'000ull;a.valid=1;a.state=6;a.stateChange=0x4000;a.typeId=0x10;a.typeClass=3;TB::engineAimBox.store(a);}
        CHECK(g.b.wantsAim(true,now),"a main-thread read stamped after the input sample is fresh");
        Rig k;k.kind=7;k.frame(0.f,1.f);now+=3'000'000'000ull;unread();const auto s0=seq(),p0=TB::perKind[0].presses.load();
        k.run(1.f,1.f,150);k.run(0.f,1.f,300);CHECK(seq()==s0+1,"knife tap = quick aimed throw (unchanged)");
        for(int t=0;t<3000;t+=11){unread();k.frame(0.f,1.f);}
        CHECK(TB::perKind[0].presses.load()==p0+1&&k.rtFrames==3,"knife: one 3-frame press, no retry");
        k.settle();
    }
    { // 19 LIGHT tap after an earlier aimed throw (its launched pendingSeq_ is never cleared by a put-away): the stance stays
        Rig g;g.frame(0.f,1.f);now+=3'000'000'000ull;unread();const auto s0=seq();
        g.run(1.f,1.f,600);g.run(0.f,1.f,300);CHECK(seq()==s0+1,"aimed throw");
        TB::launchedNs.store(now);TB::launchedSeq.store(TB::requestSeq.load());g.run(0.f,1.f,500);
        g.run(1.f,1.f,150);g.run(0.f,1.f,100);CHECK(seq()==s0+1&&g.b.wantsAim(true,now),"tap after an old launch = LIGHT");
        g.run(0.f,1.f,500);CHECK(g.b.wantsAim(true,now),"the stance survives the old launched seq");
        TB::config.aim=TB::AimMode::Off;Rig o;o.frame(0.f,1.f);now+=3'000'000'000ull;const auto s1=seq();o.run(1.f,1.f,150);o.run(0.f,1.f,300);
        CHECK(seq()==s1+1,"FORCE AIM off: a dynamite tap stays the quick throw");TB::config.aim=TB::AimMode::Trigger;now+=4'000'000'000ull;
    }
    std::printf("%s (%d failures)\n",fails?"FAILED":"ALL OK",fails);
    return fails?1:0;
}
