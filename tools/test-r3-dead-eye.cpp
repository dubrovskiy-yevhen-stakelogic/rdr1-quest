// Host C++20 regression (MSVC: tools/test-*.cpp runner): R3 + LT on the controls mapper.
//  - ltForDeadEye / LtTrack: the LT the game gets (delivered sample, freshness by observer count, LT pressed after the last submitted
//    pad); ltGameFor: an engine item in a VR hand keeps the physical LT;
//  - Dead Eye routes send R3 as a deadEyePulse-update pulse (the game reads only the press edge; a held R3 is camera LOOK_BACK);
//  - r3Route and the Mapper: LT held but withheld from the game -> crouch on foot / nothing elsewhere, never the R3 bit;
//    LT reaching the game, a gun in a VR hand and a running Dead Eye keep the R3 press; the decision is fixed at the press;
//  - the SLOW-MO arm rule (ltAims), the vr.controls.r3 ring and its report;
//  - the native pad driver order: controls mapper -> holster filter -> pad observer (the observer sees the withheld LT).
// No device, no engine.
#include "engine/NativePadDriver.h"
#include "VrControls.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

namespace vc=rdrvr::vr::controls;namespace R=rdrvr::runtime;namespace E=rdrvr::engine;
using vc::ms;using vc::LtGame;using vc::R3Route;using vc::Layout;
constexpr std::uint32_t A=vc::bit::A,L3=vc::bit::L3,R3=vc::bit::R3,LT=vc::bit::LT,RT=vc::bit::RT;

struct M {
    vc::Mapper m;std::uint64_t t=5'000'000'000ull;vc::MapIn in{};
    M(Layout l){in.ready=true;in.layout=l;in.jumping=0;}
    std::uint32_t step(std::uint32_t b,LtGame g=LtGame::Physical,std::uint64_t dt=ms(14)){t+=dt;in.now=t;in.buttons=b;in.ltGame=g;return m.apply(in,b);}
};

static void testHelper(){
    assert(vc::ltForDeadEye(true,true,false,false)==LtGame::Physical);                     // no observer note -> physical LT
    assert(vc::ltForDeadEye(false,false,true,false)==LtGame::Physical);
    assert(vc::ltForDeadEye(true,true,false,true)==LtGame::Off);                           // fresh, withheld
    assert(vc::ltForDeadEye(true,true,true,true)==LtGame::On);                             // fresh, delivered
    assert(vc::ltForDeadEye(false,true,true,true)==LtGame::Off);                           // LT up
    assert(vc::ltForDeadEye(true,false,true,true)==LtGame::Pending);                       // LT went down after the last pad
    assert(vc::ltForDeadEye(true,false,false,true)==LtGame::Pending);
    vc::LtTrack tr;
    assert(tr.sample(true,false,0)==LtGame::Physical);                                     // no observer yet
    assert(tr.sample(true,false,1)==LtGame::Off);                                          // first note (LT already down before)
    assert(tr.sample(true,false,1)==LtGame::Physical);                                     // no note since the previous update
    assert(tr.sample(true,false,2)==LtGame::Off);assert(tr.sample(false,false,3)==LtGame::Off);
    assert(tr.sample(true,true,4)==LtGame::Pending);assert(tr.sample(true,true,5)==LtGame::On);
    // a long update (a streaming hitch of any length) changes nothing: freshness is the observer count, not the time
    {vc::LtTrack h;h.sample(true,false,10);assert(h.sample(true,false,11)==LtGame::Off);assert(h.sample(true,false,12)==LtGame::Off);}
    // ltGameFor: lever off or an engine item in a VR hand (firearm, throwable, knife...) -> physical LT
    assert(vc::ltGameFor(true,-1,LtGame::Off)==LtGame::Off&&vc::ltGameFor(true,-1,LtGame::Pending)==LtGame::Pending);
    assert(vc::ltGameFor(true,0,LtGame::Off)==LtGame::Physical&&vc::ltGameFor(true,1,LtGame::Off)==LtGame::Physical);
    assert(vc::ltGameFor(false,-1,LtGame::Off)==LtGame::Physical);
    // SLOW-MO arm (GameXr: gunInHand = deadEyeGun || ltAims): no gun, LT physical but withheld -> not armed
    assert(!vc::ltAims(LtGame::Off,true)&&!vc::ltAims(LtGame::Pending,true)&&vc::ltAims(LtGame::On,true)&&vc::ltAims(LtGame::Physical,true));
    assert(!vc::ltAims(LtGame::Physical,false));
}

static void testRoute(){
    using L=Layout;
    assert(vc::r3Route(L::Foot,false,true,LtGame::Off,false,false)==R3Route::LtWithheldCrouch);
    assert(vc::r3Route(L::Foot,false,true,LtGame::On,false,false)==R3Route::DeadEyeLt);
    assert(vc::r3Route(L::Foot,false,true,LtGame::Physical,false,false)==R3Route::DeadEyeLt);
    assert(vc::r3Route(L::Foot,false,false,LtGame::Off,false,false)==R3Route::Crouch);
    assert(vc::r3Route(L::Foot,false,true,LtGame::Off,true,false)==R3Route::DeadEyeGun);
    assert(vc::r3Route(L::Foot,false,true,LtGame::Off,false,true)==R3Route::DeadEyeEnd);
    assert(vc::r3Route(L::Foot,false,true,LtGame::Pending,false,false)==R3Route::Wait);
    assert(vc::r3Route(L::Foot,false,true,LtGame::Pending,false,false,false)==R3Route::LtWithheldCrouch);
    assert(vc::r3Route(L::Foot,false,true,LtGame::Pending,true,false)==R3Route::DeadEyeGun);
    assert(vc::r3Route(L::Mounted,false,true,LtGame::Off,false,false)==R3Route::LtWithheldSuppressed);
    assert(vc::r3Route(L::Gameplay,false,true,LtGame::Off,false,false)==R3Route::LtWithheldSuppressed);
    assert(vc::r3Route(L::Gameplay,false,false,LtGame::Physical,false,false)==R3Route::Suppressed);
    assert(vc::r3Route(L::Native,false,true,LtGame::Off,false,false)==R3Route::Native);
    assert(vc::r3Route(L::Foot,true,true,LtGame::Off,false,false)==R3Route::Lasso);
}

static void testMapper(){
    // 1. Foot, LT physical, LT withheld from the game: an L3 pulse for 2 updates, R3 never sent while held
    {M f(Layout::Foot);const auto cp=vc::crouchPulses.load(),de=vc::deadEyePresses.load(),lp=rdrvr::vr::deadeye::ltPresses.load(),
        wh=vc::ltWithheldR3.load();
     f.step(LT,LtGame::Off);
     assert(f.step(LT|R3,LtGame::Off)==(LT|L3));assert(f.step(LT|R3,LtGame::Off)==(LT|L3));
     for(int i=0;i<30;++i)assert(f.step(LT|R3,LtGame::Off)==LT);
     assert(f.step(LT,LtGame::Off)==LT);
     assert(vc::crouchPulses.load()==cp+1&&vc::deadEyePresses.load()==de&&rdrvr::vr::deadeye::ltPresses.load()==lp&&vc::ltWithheldR3.load()==wh+1);}
    // 2. Foot, LT reaching the game: one Dead Eye press, R3 as a pulse (a short click is cut by its release)
    {M f(Layout::Foot);const auto de=vc::deadEyePresses.load(),lp=rdrvr::vr::deadeye::ltPresses.load(),cp=vc::crouchPulses.load();
     f.step(LT,LtGame::On);
     assert(f.step(LT|R3,LtGame::On)==(LT|R3));assert(f.step(LT|R3,LtGame::On)==(LT|R3));assert(f.step(LT,LtGame::On)==LT);
     assert(vc::deadEyePresses.load()==de+1&&rdrvr::vr::deadeye::ltPresses.load()==lp+1&&vc::crouchPulses.load()==cp);}
    // 2b. the Dead Eye pulse: a long hold sends R3 for exactly deadEyePulse updates (every Dead Eye route), then a new click a new pulse;
    //     Native / lasso minigame R3 still follows the hold
    {const unsigned P=vc::Mapper{}.tuning.deadEyePulse;assert(P==3);
     auto sent=[](M& f,std::uint32_t b,LtGame g,int n){int c=0;for(int i=0;i<n;++i)if(f.step(b,g)&R3)++c;return c;};
     {M f(Layout::Foot);f.step(LT,LtGame::On);assert(sent(f,LT|R3,LtGame::On,40)==int(P));f.step(LT,LtGame::On);
      assert(sent(f,LT|R3,LtGame::On,40)==int(P));assert(f.step(0,LtGame::Off)==0);}
     {M f(Layout::Foot);f.in.deadEyeGun=true;assert(sent(f,R3,LtGame::Off,40)==int(P));f.step(0,LtGame::Off);}
     {M f(Layout::Foot);f.in.deadEyeActive=true;assert(sent(f,R3,LtGame::Off,40)==int(P));f.step(0,LtGame::Off);}
     {M f(Layout::Gameplay);f.step(LT,LtGame::On);assert(sent(f,LT|R3,LtGame::On,40)==int(P));f.step(0,LtGame::Off);}
     {M f(Layout::Foot);assert(sent(f,LT|R3,LtGame::Physical,40)==int(P));f.step(0);}                   // lever off: same pulse
     {M f(Layout::Foot);assert(f.step(LT|R3,LtGame::Pending)==LT);assert(sent(f,LT|R3,LtGame::On,40)==int(P));f.step(0);} // after a wait
     {M f(Layout::Native);assert(sent(f,R3,LtGame::Off,40)==40);f.step(0);}
     {M f(Layout::Foot);f.in.lassoMinigame=true;assert(sent(f,R3,LtGame::Off,40)==40);f.step(0);}
     // the pulse never leaks into other bits; a not-ready update ends it and, as for every latch, a button still held is a new press after it
     {M f(Layout::Foot);f.in.deadEyeGun=true;assert(f.step(R3|A,LtGame::Off)==(R3|vc::bit::X));f.in.ready=false;assert(f.step(R3,LtGame::Off)==0);
      f.in.ready=true;assert(f.step(R3,LtGame::Off)==R3);f.step(0,LtGame::Off);}}
    // default MapIn (Physical): the physical LT decides
    {M f(Layout::Foot);const auto de=vc::deadEyePresses.load();
     assert(f.step(LT|R3)==(LT|R3));assert(f.step(LT|R3)==(LT|R3));assert(f.step(0)==0);assert(vc::deadEyePresses.load()==de+1);}
    // 3. overrides with LT withheld: a gun in a VR hand -> Dead Eye; a running Dead Eye -> the end press
    {M f(Layout::Foot);f.in.deadEyeGun=true;const auto gp=rdrvr::vr::deadeye::gunPresses.load();
     assert(f.step(LT|R3,LtGame::Off)==(LT|R3));assert(f.step(LT|R3,LtGame::Off)==(LT|R3));assert(f.step(0,LtGame::Off)==0);
     assert(rdrvr::vr::deadeye::gunPresses.load()==gp+1);}
    {M f(Layout::Foot);f.in.deadEyeActive=true;const auto ep=rdrvr::vr::deadeye::endPresses.load();
     assert(f.step(LT|R3,LtGame::Off)==(LT|R3));assert(f.step(0,LtGame::Off)==0);assert(rdrvr::vr::deadeye::endPresses.load()==ep+1);}
    // 4. other layouts with LT withheld: Mounted / Gameplay nothing (r3Suppressed), Native stock R3, lasso minigame stock R3
    for(const auto l:{Layout::Mounted,Layout::Gameplay}){
        M f(l);const auto rs=vc::r3Suppressed.load(),wh=vc::ltWithheldR3.load();
        assert(f.step(LT|R3,LtGame::Off)==LT);assert(f.step(LT|R3,LtGame::Off)==LT);assert(f.step(0,LtGame::Off)==0);
        assert(vc::r3Suppressed.load()==rs+1&&vc::ltWithheldR3.load()==wh+1);
    }
    {M f(Layout::Native);assert(f.step(LT|R3,LtGame::Off)==(LT|R3));assert(f.step(0,LtGame::Off)==0);}
    {M f(Layout::Foot);f.in.lassoMinigame=true;assert(f.step(LT|R3,LtGame::Off)==(LT|R3));assert(f.step(0,LtGame::Off)==0);}
    // 5. the decision is fixed at the press: ltGame flipping while R3 is held changes nothing, in either direction
    {M f(Layout::Foot);f.step(LT,LtGame::Off);
     assert(f.step(LT|R3,LtGame::Off)==(LT|L3));f.step(LT|R3,LtGame::Off);
     for(int i=0;i<10;++i)assert(f.step(LT|R3,LtGame::On)==LT);assert(f.step(LT,LtGame::On)==LT);}
    {M f(Layout::Foot);const auto cp=vc::crouchPulses.load();f.step(LT,LtGame::On);
     assert(f.step(LT|R3,LtGame::On)==(LT|R3));assert(f.step(LT|R3,LtGame::Off)==(LT|R3));assert(f.step(LT|R3,LtGame::Off)==(LT|R3));
     for(int i=0;i<10;++i)assert(f.step(LT|R3,LtGame::Off)==LT);assert(f.step(LT,LtGame::Off)==LT);assert(vc::crouchPulses.load()==cp);}
    // Pending: LT and R3 in the same update -> decided one update later with the delivered LT
    {M f(Layout::Foot);const auto w=vc::r3Waits.load(),de=vc::deadEyePresses.load();
     assert(f.step(LT|R3,LtGame::Pending)==LT);assert(f.step(LT|R3,LtGame::On)==(LT|R3));assert(f.step(LT|R3,LtGame::On)==(LT|R3));
     assert(f.step(0,LtGame::Off)==0);assert(vc::r3Waits.load()==w+1&&vc::deadEyePresses.load()==de+1);}
    {M f(Layout::Foot);const auto cp=vc::crouchPulses.load();
     assert(f.step(LT|R3,LtGame::Pending)==LT);assert(f.step(LT|R3,LtGame::Off)==(LT|L3));assert(f.step(LT|R3,LtGame::Off)==(LT|L3));
     assert(f.step(LT|R3,LtGame::Off)==LT);assert(vc::crouchPulses.load()==cp+1);}
    {M f(Layout::Foot);const auto cp=vc::crouchPulses.load(); // still Pending on the second update: no second wait
     assert(f.step(LT|R3,LtGame::Pending)==LT);assert(f.step(LT|R3,LtGame::Pending)==(LT|L3));assert(vc::crouchPulses.load()==cp+1);}
    // plain R3 crouch and other buttons untouched
    {M f(Layout::Foot);const auto cp=vc::crouchPulses.load();
     assert(f.step(R3,LtGame::Off)==L3);assert(f.step(R3,LtGame::Off)==L3);assert(f.step(R3,LtGame::Off)==0);assert(f.step(RT|R3,LtGame::Off)==RT);
     assert(vc::crouchPulses.load()==cp+1);assert(f.step(A,LtGame::Off)==vc::bit::X);}
}

static std::size_t count(const std::string& s,const char* k){std::size_t n=0,p=0;while((p=s.find(k,p))!=std::string::npos){++n;++p;}return n;}

static void testRing(){
    // route names, heldMs at release, crouch 500 ms later, psi deviation, wrap at 16
    {M w(Layout::Foot);for(int i=0;i<16;++i){w.step(R3,LtGame::Off);w.step(0,LtGame::Off);}} // only released presses in the ring
    M f(Layout::Foot);f.in.holdHand=-1;f.in.crouched=0;f.in.psi=0.f;f.in.psiOrigin=7;
    f.step(LT,LtGame::Off);
    f.step(LT|R3,LtGame::Off);f.in.psi=3.1f;f.step(LT|R3,LtGame::Off);f.in.psi=-3.1f;f.step(LT|R3,LtGame::Off,ms(100));
    f.in.crouched=1;f.step(LT,LtGame::Off,ms(500));
    auto j=vc::r3Report();
    assert(j.find("\"route\":\"LT_WITHHELD_CROUCH\",\"ltPhys\":true,\"ltGame\":\"OFF\",\"ltFresh\":true")!=std::string::npos);
    assert(j.find("\"crouchedAtPress\":0,\"crouchedAfter500ms\":1,\"heldMs\":614")!=std::string::npos);
    {const auto p=j.rfind("\"psiMaxDevDeg\":");assert(p!=std::string::npos);const double d=std::atof(j.c_str()+p+15);
     assert(std::fabs(d-3.1*57.2957795)<0.2);} // both +3.1 and -3.1 rad are 177.6 deg from 0 (wrapped)
    f.in.deadEyeGun=true;f.step(R3,LtGame::Off);f.step(0,LtGame::Off);f.in.deadEyeGun=false;
    f.in.deadEyeActive=true;f.step(R3,LtGame::Off);f.step(0,LtGame::Off);f.in.deadEyeActive=false;
    f.step(LT|R3,LtGame::On);f.step(0,LtGame::On);
    f.step(R3,LtGame::Off);f.step(0,LtGame::Off);
    {M n(Layout::Native);n.step(R3);n.step(0);}
    {M n(Layout::Foot);n.in.lassoMinigame=true;n.step(R3);n.step(0);}
    {M n(Layout::Mounted);n.step(R3,LtGame::Off);n.step(0);n.step(LT|R3,LtGame::Off);n.step(0);}
    j=vc::r3Report();
    for(const char* r:{"\"DEADEYE_GUN\"","\"DEADEYE_END\"","\"DEADEYE_LT\"","\"CROUCH\"","\"NATIVE\"","\"LASSO\"","\"SUPPRESSED\"","\"LT_WITHHELD_SUPPRESSED\""})
        assert(j.find(r)!=std::string::npos);
    assert(j.find("\"heldMs\":-1")==std::string::npos); // every press above was released
    // a flip that comes back (max ~180, end ~0) vs a stick turn (stick deflected, the deviation stays)
    {M g(Layout::Foot);g.in.psiOrigin=9;g.in.psi=0.f;g.in.lookX=0.f;g.step(R3,LtGame::Off);g.in.psi=3.1f;g.step(R3,LtGame::Off);
     g.in.psi=0.f;g.step(R3,LtGame::Off);g.step(0,LtGame::Off);
     const auto k=vc::r3Report();const auto p=k.rfind("\"psiMaxDevDeg\":");
     assert(std::fabs(std::atof(k.c_str()+p+15)-3.1*57.2957795)<0.2);
     assert(k.find("\"psiEndDevDeg\":0.0,\"psiSamples\":3,\"stickMaxX\":0.00",p)!=std::string::npos);
     g.in.psi=0.f;g.step(R3,LtGame::Off);g.in.lookX=-0.9f;g.in.psi=0.5f;g.step(R3,LtGame::Off);g.in.lookX=0.f;g.step(R3,LtGame::Off);g.step(0,LtGame::Off);
     const auto t=vc::r3Report();assert(t.find("\"psiMaxDevDeg\":28.6,\"psiEndDevDeg\":28.6,\"psiSamples\":3,\"stickMaxX\":0.90",t.rfind("\"ageMs\""))!=std::string::npos);}
    // the holster gate and engine aim bits at the press; R3 updates sent (pulse vs crouch)
    {M g(Layout::Foot);g.in.ctxAllowed=0;g.in.blockReason="FORCE_HOLSTER";g.in.aimBits=0;
     for(int i=0;i<10;++i)g.step(LT|R3,LtGame::On);g.step(0,LtGame::Off);
     auto k=vc::r3Report();const auto last=k.rfind("\"ageMs\"");
     assert(k.find("\"route\":\"DEADEYE_LT\"",last)!=std::string::npos&&k.find("\"r3Updates\":3,",last)!=std::string::npos);
     assert(k.find("\"ctxAllowed\":0,\"blockReason\":\"FORCE_HOLSTER\",\"aimBits\":0}",last)!=std::string::npos);
     g.in.ctxAllowed=1;g.in.blockReason="OK";g.in.aimBits=0x0a;for(int i=0;i<10;++i)g.step(R3,LtGame::Off);g.step(0,LtGame::Off);
     k=vc::r3Report();const auto l2=k.rfind("\"ageMs\"");
     assert(k.find("\"route\":\"CROUCH\"",l2)!=std::string::npos&&k.find("\"r3Updates\":0,",l2)!=std::string::npos);
     assert(k.find("\"ctxAllowed\":1,\"blockReason\":\"OK\",\"aimBits\":10}",l2)!=std::string::npos);}
    // a waiting press dropped by a not-ready update: counted, ring route WAIT
    {M g(Layout::Foot);const auto wd=vc::r3WaitDropped.load(),tot=vc::r3Total.load(),cp=vc::crouchPulses.load();
     assert(g.step(LT|R3,LtGame::Pending)==LT);g.in.ready=false;assert(g.step(LT|R3,LtGame::Off)==LT);g.in.ready=true;
     assert(vc::r3WaitDropped.load()==wd+1&&vc::r3Total.load()==tot+1&&vc::crouchPulses.load()==cp);
     const auto k=vc::r3Report();const auto last=k.rfind("\"ageMs\"");
     assert(k.find("\"route\":\"WAIT\"",last)!=std::string::npos&&k.find("\"waited\":true",last)!=std::string::npos);
     assert(k.find("\"waitDropped\":")!=std::string::npos);g.step(0,LtGame::Off);}
    for(int i=0;i<40;++i){f.step(R3,LtGame::Off);f.step(0,LtGame::Off);}
    j=vc::r3Report();assert(count(j,"\"ageMs\"")==16&&j.find("\"torn\":false")!=std::string::npos);
    assert(j.find("\"deliveredLtCheck\":true")!=std::string::npos);
    // the whole controls report: one JSON object, balanced
    const auto all=vc::report();int depth=0;bool str=false;
    for(std::size_t i=0;i<all.size();++i){const char c=all[i];if(c=='"'&&(i==0||all[i-1]!='\\'))str=!str;if(str)continue;
        if(c=='{'||c=='[')++depth;if(c=='}'||c==']'){--depth;assert(depth>=0);}if(depth==0)assert(i==all.size()-1);}
    assert(depth==0&&!str&&all.find("\"r3\":{")!=std::string::npos&&all.find("\"ltWithheld\":")!=std::string::npos);
    std::cout<<vc::r3Report()<<'\n';
}

// ---- the native pad driver order: controls -> holster filter -> observer (fake native input ABI) ----
struct Pad {std::uintptr_t table=0x2000;std::array<unsigned char,0x380> bytes{};} own,external;
struct System {
    std::uintptr_t table=0x1000;std::byte prefix[0x20]{};Pad* slots[4]{};
    std::byte middle[0x20]{};Pad* override{};void* touchOverride{};
} sys;
std::array<std::byte,0x50> info{};bool registered{};
bool readMem(void*,std::uintptr_t p,void* out,std::size_t n){
    for(auto r:{std::pair<const void*,std::size_t>{&sys,sizeof(sys)},{&own,sizeof(own)},{&external,sizeof(external)},{info.data(),info.size()}}){
        const auto b=reinterpret_cast<std::uintptr_t>(r.first);
        if(p>=b&&p-b<=r.second&&n<=r.second-(p-b)){std::memcpy(out,reinterpret_cast<void*>(p),n);return true;}}
    return false;
}
void* getSystem(){return &sys;}
void* getPad(const void*,int i){return i==0&&sys.override?sys.override:sys.slots[i];}
void* getInfo(void*,int){return registered?info.data():nullptr;}
void connect(void*,int,int,int,const char*){sys.slots[1]=&own;registered=true;std::memcpy(info.data()+0x40,&sys.slots[1],sizeof(void*));}
void activate(void*,void*){}
void touch(void*,bool){}
void overridePad(void*,void* p){sys.override=static_cast<Pad*>(p);}
std::uint32_t gameButtons{};
void button(void*,int,int key,bool down){
    const int bitOf=key==106?5:key==107?6:key==104?9:-1; // L3 / R3 / LT (Android keycodes of NativePadEvents)
    if(bitOf>=0){if(down)gameButtons|=1u<<bitOf;else gameButtons&=~(1u<<bitOf);}
}
void axis(void*,int,int,float,bool&){}
vc::Mapper chainMapper;vc::LtTrack chainTrack;bool withholdLt=true;
void controls(void*,const R::ControllerSample* input,std::uint64_t now,R::PadState& state){
    vc::MapIn in{};in.ready=state.ready&&input;in.buttons=state.buttons;in.now=now;in.layout=Layout::Foot;
    in.ltGame=chainTrack.sample((in.buttons&LT)!=0,vc::ltDelivered.load(),vc::padNotes.load());
    state.buttons=chainMapper.apply(in,state.buttons);
}
void holsterLike(void*,const R::ControllerSample*,std::uint64_t,R::PadState& state){
    if(withholdLt){state.triggers[0]=0.f;state.buttons&=~R::PadLeftTrigger;}
}
void observer(std::uint64_t now,std::uint32_t buttons,bool ready) noexcept {vc::noteDelivered(now,buttons,ready);}

static void testDriverOrder(){
    R::ControllerMailbox box;E::NativePadDriver driver(box);sys.slots[0]=&external;
    driver.configure({readMem,getSystem,getPad,getInfo,connect,activate,touch,{button,axis},0x1000,0x2000,overridePad});
    driver.setControlsFilter(controls);driver.setPadFilter(holsterLike);driver.setPadObserver(observer);
    R::ControllerSample sample{};sample.epoch=1;sample.active=sample.focused=true;
    std::uint64_t now=1'000'000'000;std::uint32_t seenL3=0,seenR3=0;
    auto tick=[&](std::uint32_t b,float lt){ // b: face / stick-click bits only, LT from the trigger axis (the policy)
       now+=14'000'000;sample.publishedNs=now;++sample.sequence;sample.buttons=b;sample.triggers[0]=lt;
        assert(box.publish(sample));driver.beforeUpdate(&sys,now);driver.afterUpdate(&sys,now);
        if(gameButtons&(1u<<5))++seenL3;if(gameButtons&(1u<<6))++seenR3;};
    for(int i=0;i<6;++i)tick(0,0.f);
    const auto notes=vc::padNotes.load();
    // empty hands: LT withheld by the filter after the mapper -> LT + R3 = crouch (L3 pulse), never R3 at the game
    tick(0,1.f);tick(0,1.f);
    assert(vc::padNotes.load()>notes&&!vc::ltDelivered.load());
    for(int i=0;i<20;++i)tick(R::PadRightStick,1.f);
    tick(0,0.f);tick(0,0.f);
    assert(seenR3==0&&seenL3>=1);
    // LT reaching the game (filter passes it): LT + R3 = the DEADEYE press as a pulse, never a held R3 (camera LOOK_BACK)
    const unsigned P=vc::mapper.tuning.deadEyePulse;
    withholdLt=false;seenL3=seenR3=0;
    tick(0,1.f);tick(0,1.f);assert(vc::ltDelivered.load());
    for(int i=0;i<20;++i)tick(R::PadRightStick,1.f);
    tick(0,0.f);tick(0,0.f);
    assert(seenR3==P&&seenL3==0);
    // pressed in the same update (LT not known yet): decided one update later
    seenL3=seenR3=0;
    for(int i=0;i<20;++i)tick(R::PadRightStick,1.f);
    tick(0,0.f);tick(0,0.f);assert(seenR3==P&&seenL3==0);
    // a long gap between two updates (no time bound): still the delivered LT, never the physical one
    withholdLt=true;seenL3=seenR3=0;tick(0,1.f);now+=2'000'000'000ull;tick(0,1.f);now+=2'000'000'000ull;
    for(int i=0;i<20;++i)tick(R::PadRightStick,1.f);
    tick(0,0.f);tick(0,0.f);assert(seenR3==0&&seenL3>=1);withholdLt=false;
    withholdLt=true;seenL3=seenR3=0;
    for(int i=0;i<20;++i)tick(R::PadRightStick,1.f);
    tick(0,0.f);tick(0,0.f);assert(seenR3==0&&seenL3>=1);
}

int main(){
    testHelper();testRoute();testMapper();testRing();testDriverOrder();
    std::cout<<"R3 + LT delivered-LT rule, routes, mapper latches, SLOW-MO arm, r3 ring/report and driver order tests passed\n";
    return 0;
}
