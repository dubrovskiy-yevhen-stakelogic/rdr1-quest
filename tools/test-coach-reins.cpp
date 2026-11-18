// Host regression (MSVC: cl /std:c++20 /EHsc /I<fake sys/system_properties.h> /Inative/src /Inative/src/vr; build as tools/test-casual-trigger.cpp):
// COACH REINS (vr/VrCoachReins.h): the steer shaping, the tap machine against a model of sagPlayerInputWagon::ApplyInput, the reins brain in
// coach mode; SEAT STEADY (vr/VrSeatSteady.h): the seat filter on a moving, swaying and wobbling seat.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "vr/VrReins.h"
#include "vr/VrSeatSteady.h"
namespace C=rdrvr::vr::coach;
namespace RN=rdrvr::vr::reins;
namespace SS=rdrvr::vr::seatsteady;
static int fails=0;
#define CHECK(c,msg) do{if(!(c)){std::printf("FAIL %s  (%s)\n",msg,#c);++fails;}else std::printf("ok   %s\n",msg);}while(0)
static constexpr std::uint64_t ms=1'000'000ull;

// sagPlayerInputWagon::ApplyInput 0x26bb148: next = clamp(nav + step, step < 0 ? 0 : 1, 5); the shift timer is ready on a frame without a
// shift input; a ready timer with next != nav moves nav at once and restarts the 2 s timer.
struct Wagon {
    int nav{1};float timer{2.f};bool driving{true};
    void frame(bool whip,bool back,float dt) {
        if(!driving)return;
        const int step=whip?1:back?-1:0;
        int next=nav+step;next=std::min(next,5);next=std::max(next,step<0?0:1);
        if(timer<2.f)timer=step==0?2.f:timer+dt;
        if(timer>=2.f&&next!=nav){nav=next;timer=0.f;}
    }
};
struct TapRun {std::uint64_t now{1000*ms};int ups{},downs{};int minNav{9};};
// The input thread at 72 Hz, the game frame at 36 Hz, the riding sample publishes the nav state one game frame late.
static void runTaps(C::Tap& t,Wagon& w,int want,int frames,TapRun& r) {
    int published=w.nav;bool whip=false,back=false;
    for(int i=0;i<frames;++i){
        r.now+=14*ms;
        const auto o=t.step(r.now,true,want,published);
        if(o.whip&&!whip)++r.ups;if(o.back&&!back)++r.downs;whip=o.whip;back=o.back;
        if(i&1){published=w.nav;w.frame(whip,back,.028f);r.minNav=std::min(r.minNav,w.nav);}
    }
}
static void testShape() {
    bool lin=true;
    for(float s=.05f;s<=1.f;s+=.05f){const float x=C::shapeSteer(s,true),g=std::pow((std::fabs(x)-.25f)/.75f,2.f);if(std::fabs(g-s)>.002f)lin=false;}
    CHECK(lin,"shapeSteer: the game's dead zone 0.25 and square give the rein steer back");
    CHECK(C::shapeSteer(-.5f,true)<-.25f&&C::shapeSteer(.005f,true)==0.f,"shapeSteer: sign kept, a hair of steer is 0");
    CHECK(C::shapeSteer(.4f,false)==.4f&&C::shapeSteer(3.f,false)==1.f,"shapeSteer: .coachlin 0 = the raw steer, clamped");
    CHECK(C::navOfGait(0)==1&&C::navOfGait(4)==5&&C::navOfGait(9)==5&&C::gaitOfNav(1)==0&&C::gaitOfNav(0)==0&&C::gaitOfNav(5)==4,"gait <-> nav state");
}
static void testTaps() {
    {C::Tap t{};Wagon w{};TapRun r{};runTaps(t,w,4,200,r);
        CHECK(w.nav==4&&r.ups==3&&r.downs==0,"taps: stop -> canter = three Whip taps, no overshoot");
        runTaps(t,w,1,300,r);CHECK(w.nav==1&&r.downs==3&&r.minNav>=1,"taps: canter -> stop = three back taps, never backing");
        runTaps(t,w,1,100,r);CHECK(r.downs==3&&w.nav==1,"taps: at stop nothing more");}
    {C::Tap t{};Wagon w{};w.nav=5;TapRun r{};runTaps(t,w,1,400,r);CHECK(w.nav==1&&r.minNav==1,"taps: gallop -> stop ends at stop");}
    {C::Tap t{};Wagon w{};w.driving=false;TapRun r{};runTaps(t,w,3,100,r);
        const int first=r.ups;CHECK(first==3&&t.backoffs==1,"taps: three taps that move nothing back off");
        runTaps(t,w,3,25,r);CHECK(r.ups==first,"taps: no tap during the 1 s back-off");}
    {C::Tap t{};std::uint64_t now=1000*ms;auto o=t.step(now,true,3,1);CHECK(o.whip,"taps: a press starts at once");
        o=t.step(now+20*ms,false,3,1);CHECK(!o.whip&&!o.back,"taps: not live = released");
        o=t.step(now+40*ms,true,3,-1);CHECK(!o.whip,"taps: unknown nav state = nothing");
        o=t.step(now+400*ms,true,1,0);CHECK(!o.whip&&!o.back,"taps: nav 0 (the player holds back) = no Whip over it");}
}
// The reins brain in coach mode: hands in front of the head, the frame from horseMinusHead 0.
struct Rider {
    RN::Brain b{};RN::In in{};std::uint64_t now{5000*ms},seq{1};float yL{1.0f},yR{1.0f},zL{-.45f},zR{-.45f};float gL{},gR{};
    RN::Out step() {
        now+=14*ms;in.now=now;in.sampleNs=now;in.sequence=++seq;in.mounted=true;in.layoutMounted=true;in.haveHead=true;in.gripLive=true;
        in.head={0.f,1.6f,0.f};in.headFwd={0.f,0.f,-1.f};in.valid={true,true};
        in.pos[0]={-.15f,yL,zL};in.pos[1]={.15f,yR,zR};in.grip={gL,gR};
        RN::publishRiding(now,0.f,true);
        return b.step(in);
    }
    void run(int n) {for(int i=0;i<n;++i)last=step();}
    RN::Out last{};
};
static void testBrain() {
    RN::hardStopUntil.store(0);RN::brakePullUntil.store(0);
    Rider r{};r.in.coach=true;r.in.coachGait=2;
    r.run(10);CHECK(r.b.level()==2,"coach: no rein held = the gait follows the coach (TROT)");
    CHECK(!r.last.stick&&!r.last.a,"coach: nothing output before a grab");
    r.gL=r.gR=1.f;r.run(40);
    CHECK(RN::stats.held.load()==2,"coach: both reins grabbed");
    CHECK(r.last.stick&&r.last.y==0.f&&!r.last.a,"coach: a held rein = MOVE_X only, no throttle, no A");
    CHECK(RN::stats.mode.load()==RN::ModeCoach,"coach: mode COACH");
    // a snap (the WHIP stroke): both hands up 7 cm in 84 ms, then down 9 cm in 84 ms
    for(int i=0;i<6;++i){r.yL+=.0117f;r.yR+=.0117f;r.last=r.step();}
    for(int i=0;i<6;++i){r.yL-=.015f;r.yR-=.015f;r.last=r.step();}
    r.run(30);
    CHECK(RN::stats.flicks.load()==1,"coach: the whip stroke is one accepted snap");
    CHECK(r.b.level()==3,"coach: a snap = one gait up from the coach's own (TROT -> CANTER)");
    CHECK(!r.last.a&&RN::hardStopUntil.load()==0,"coach: no spur A, no hard stop");
    // the right rein drawn back 20 cm = a right turn (COACH TURN: full lock 28 cm)
    r.zR+=.20f;r.run(30);
    CHECK(r.last.stick&&r.last.x>.5f,"coach: the right rein pulled = MOVE_X right");
    r.zR-=.20f;r.run(30);
    // both hands raised 40 cm and held: the stop
    for(int i=0;i<10;++i){r.yL+=.04f;r.yR+=.04f;r.last=r.step();}
    r.run(70);
    CHECK(r.b.level()==0,"coach: both reins lifted and held = HALT");
    CHECK(RN::hardStopUntil.load()==0&&RN::brakePullUntil.load()==0,"coach: the stop asks no horse hard stop / brake pull");
    for(int i=0;i<10;++i){r.yL-=.04f;r.yR-=.04f;r.last=r.step();}
    r.run(40);
    // let go: the gait follows the coach again
    r.gL=r.gR=0.f;r.in.coachGait=4;r.run(60);
    CHECK(RN::stats.held.load()==0&&r.b.level()==4,"coach: reins let go = the gait follows the coach (GALLOP)");
    // the player's own stick while holding: no output, the gait follows
    r.gL=r.gR=1.f;r.run(40);r.in.userY=.8f;r.in.coachGait=1;r.run(10);
    CHECK(!r.last.stick&&r.b.level()==1,"coach: the player's own stick = no output, the gait follows the coach");
    r.in.userY=0.f;r.run(60);
    // the player's own A (Whip) while holding the reins: the gait takes the nav state it set, also after a short press
    r.in.coachGait=1;r.run(40);r.in.userA=true;r.run(3);r.in.userA=false;r.in.coachGait=2;r.run(10);
    CHECK(r.b.level()==2,"coach: the player's own A = the gait follows the coach (no back tap against it)");
    r.in.coachGait=1;r.run(60);
    // a horse after the coach: the coach rules are gone
    Rider h{};h.in.coach=false;h.gL=h.gR=1.f;h.run(40);
    CHECK(RN::stats.mode.load()!=RN::ModeCoach,"horse: never the COACH mode");
}
// SEAT STEADY: 72 Hz samples
static void testSeat() {
    SS::Lever lv{};
    {SS::Filter f{};std::uint64_t now=1000*ms;float maxErr=0.f;
        for(int i=0;i<200;++i){now+=14*ms;const float t=float(i)*.014f;const auto r=SS::step(f,10.f*t,0.f,0.f,now,i==0,lv);if(i>=72)maxErr=std::max(maxErr,std::fabs(r.x-10.f*t));}
        std::printf("     constant 10 m/s: lag after 1 s %.2f cm\n",maxErr*100.f);
        CHECK(maxErr<.01f,"seat: a constant 10 m/s is followed within 1 cm after 1 s");}
    {SS::Filter f{};std::uint64_t now=1000*ms;float rawPk=0.f,outPk=0.f;
        for(int i=0;i<600;++i){now+=14*ms;const float t=float(i)*.014f,sway=.05f*std::sin(6.2831853f*2.f*t);
            const auto r=SS::step(f,8.f*t,sway,0.f,now,i==0,lv);if(i>=216){rawPk=std::max(rawPk,std::fabs(sway));outPk=std::max(outPk,std::fabs(r.z));}}
        std::printf("     2 Hz sway 5 cm: %.2f cm left\n",outPk*100.f);
        CHECK(outPk<.5f*rawPk,"seat: a 2 Hz 5 cm sway is damped to under half");}
    {SS::Filter f{};std::uint64_t now=1000*ms;float rawPk=0.f,outPk=0.f,lagMax=0.f;
        for(int i=0;i<600;++i){now+=14*ms;const float t=float(i)*.014f,path=.349f*t,wob=.0524f*std::sin(6.2831853f*1.5f*t);
            const auto r=SS::step(f,0.f,0.f,std::remainder(path+wob,6.2831853f),now,i==0,lv);
            if(i>=216){const float e=std::remainder(r.yaw-path,6.2831853f);rawPk=std::max(rawPk,std::fabs(wob));outPk=std::max(outPk,std::fabs(e));}}
        (void)lagMax;std::printf("     1.5 Hz yaw wobble 3 deg on a 20 deg/s turn: %.2f deg left\n",outPk*57.3f);
        CHECK(outPk<.6f*rawPk,"seat: a 1.5 Hz 3 deg yaw wobble on a turn is damped, the turn followed");}
    {SS::Filter f{};std::uint64_t now=1000*ms;bool inBand=true;float last=0.f;
        for(int i=0;i<300;++i){now+=14*ms;const float raw=i<50?0.f:.3f;const auto r=SS::step(f,raw,0.f,0.f,now,i==0,lv);
            if(std::fabs(r.x-raw)>lv.band+1e-4f)inBand=false;last=r.x;}
        CHECK(inBand&&std::fabs(last-.3f)<.005f,"seat: a 30 cm seat swap keeps the eye within the band and settles on the new seat");}
    {SS::Filter f{};std::uint64_t now=1000*ms;SS::step(f,1.f,2.f,.5f,now,true,lv);now+=14*ms;SS::step(f,1.1f,2.f,.5f,now,false,lv);
        now+=14*ms;const auto r=SS::step(f,40.f,-3.f,2.f,now,false,lv);
        CHECK(r.reseeded&&r.x==40.f&&r.z==-3.f&&r.yaw==2.f,"seat: a teleport re-seeds on the raw seat");
        now+=900*ms;const auto g=SS::step(f,41.f,-3.f,2.f,now,true,lv);CHECK(g.x==41.f,"seat: a fresh run re-seeds");}
    {float up=.6f;up=SS::seatUp(up,.9f,false,true,.014f,2.f);CHECK(up<.61f,"seat: a raised head moves the seat height slowly (mean, not the highest)");
        CHECK(SS::seatUp(up,.7f,true,false,.014f,2.f)==.7f,"seat: the seat height re-seeds on a mount");}
}
int main() {
    testShape();testTaps();testBrain();testSeat();
    std::printf(fails?"%d FAILED\n":"ALL OK\n",fails);
    return fails?1:0;
}
