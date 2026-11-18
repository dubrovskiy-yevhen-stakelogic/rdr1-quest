// Host regression (MSVC: cl /std:c++20 /EHsc /I<fake sys/system_properties.h> /Inative/src /Inative/src/vr):
// APPSW per-actor blocks around a ragdoll: SpaceWarpActors.h buildActorBlock keeps camera-only vectors for an actor flagged ragdoll /
// get-up in either snapshot (ActorRagdoll) and for an NPC root that turned more than maxTurnDeg within the pair (ActorTurn); a walking turn
// is still written. VrMotionActors.h ragdollStep: the skip per tick from the mover's ragdoll word (ragdoll, the hold after it, none), the
// episode ring and the counters.
#include <cmath>
#include <cstdio>
#include <cstring>
#include "runtime/SpaceWarpActors.h"
#include "vr/VrMotionActors.h"
namespace sw=rdrvr::runtime::spacewarp;
namespace mva=rdrvr::vr::mvactors;
static int fails=0;
#define CHECK(c,msg) do{if(!(c)){std::printf("FAIL %s  (%s)\n",msg,#c);++fails;}else std::printf("ok   %s\n",msg);}while(0)

// An upright root turned by yaw degrees about y at (x, 0, z): rows a, b, c, d.
static sw::ActorPose pose(std::uint32_t guid,std::int32_t race,float yawDeg,float x,float z,std::uint8_t skip=0){
    sw::ActorPose p{};p.guid=guid;p.race=race;p.skip=skip;const float r=yawDeg*3.14159265f/180.f,c=std::cos(r),s=std::sin(r);
    const float m[12]{c,0,-s, 0,1,0, s,0,c, x,0,z};std::memcpy(p.m,m,sizeof m);return p;}
static sw::GpuActors out{};
// One eye pair with one actor in each snapshot (identity clip matrix, camera 3 m behind): the result and the stats.
static sw::ActorResult run(const sw::ActorPose& cur,const sw::ActorPose& prev,sw::ActorStats& st,double maxTurnDeg=sw::defaultMaxTurnDeg){
    static sw::ActorSet ca{},pa{};ca=sw::ActorSet{};pa=sw::ActorSet{};ca.serial=2;pa.serial=1;ca.count=pa.count=1;ca.actor[0]=cur;pa.actor[0]=prev;
    static const float rel[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};static const float pos[3]{0,0,-3.f};
    sw::ActorTrace tr[sw::maxActors]{};std::memset(&out,0,sizeof out);
    sw::buildActorBlock({rel,pos,&ca,nullptr},{rel,pos,&pa,nullptr},1.0,true,out,st,tr,0.5,nullptr,maxTurnDeg);
    return sw::ActorResult(tr[0].result);}

int main(){
    // ---------- buildActorBlock ----------
    {sw::ActorStats st{};
     {const auto r=run(pose(7,0,8.f,.04f,0),pose(7,0,0.f,0,0),st);
      CHECK(r==sw::ActorWritten&&st.angleHist[2]==1&&st.stepHist[0]==1&&std::fabs(st.maxAngleDeg-8.f)<.05f,
            "actor block: an 8 degree turn while walking 4 cm: written (turn and step histograms)");
      if(r!=sw::ActorWritten)std::printf("     result %s\n",sw::actorResultName(r));}
     CHECK(run(pose(7,0,180.f,.05f,0),pose(7,0,0.f,0,0),st)==sw::ActorTurn&&st.turn==1,"actor block: a 180 degree root yaw flip of an NPC: camera vectors (turn)");
     CHECK(run(pose(7,1,60.f,.05f,0),pose(7,1,0.f,0,0),st)==sw::ActorTurn&&st.turn==2,"actor block: a 60 degree turn of a horse within one pair: turn");
     CHECK(run(pose(7,0,40.f,.05f,0),pose(7,0,0.f,0,0),st)==sw::ActorWritten,"actor block: 40 degrees in one pair: under the 45 degree guard, written");
     CHECK(run(pose(sw::playerGuid,-2,180.f,.05f,0),pose(sw::playerGuid,-2,0.f,0,0),st)==sw::ActorWritten,"actor block: the player block is not turn-guarded");
     CHECK(run(pose(7,0,180.f,.05f,0),pose(7,0,0.f,0,0),st,0.0)==sw::ActorWritten,"actor block: actorturn 0 = no turn limit");
     CHECK(run(pose(7,0,0.f,.05f,0,1),pose(7,0,0.f,0,0),st)==sw::ActorRagdoll&&run(pose(7,0,0.f,.05f,0),pose(7,0,0.f,0,0,2),st)==sw::ActorRagdoll&&st.ragdoll==2,
           "actor block: ragdoll / get-up in either snapshot: camera vectors (ragdoll)");
     CHECK(run(pose(7,0,0.f,2.f,0,1),pose(7,0,0.f,0,0),st)==sw::ActorRagdoll&&st.jump==0,"actor block: a ragdoll's 2 m root step counts as ragdoll, not jump");
     CHECK(std::string(sw::actorResultName(sw::ActorRagdoll))=="ragdoll"&&std::string(sw::actorResultName(sw::ActorTurn))=="turn"&&sw::ActorResultCount==10,
           "actor result names: ragdoll, turn");
     CHECK(sw::actorAngleBin(2.f)==0&&sw::actorAngleBin(46.f)==5&&sw::actorAngleBin(NAN)==5&&sw::actorStepBin(.05)==0&&sw::actorStepBin(1.2)==5,"actor histogram bins");}
    // ---------- ragdollStep ----------
    {const std::uint64_t ms=1'000'000ull,hold=500*ms;std::uint64_t tick=100,now=1'000'000'000ull;
     const auto step=[&](float yaw,float x,bool known,std::uint32_t word,bool gate=true){const auto p=pose(42,0,yaw,x,0);++tick;now+=14*ms;
        return mva::ragdollStep(42,0,p.m,known,word,true,gate,tick,now,hold);};
     const auto e0=mva::ragStats.enters.load(),x0=mva::ragStats.exits.load(),f0=mva::ragStats.flipsRagdoll.load(),ep0=mva::ragStats.episodes.load();
     const auto s0=step(0,0,true,0),s1=step(0,.01f,true,0);
     const auto r1=step(180.f,.02f,true,1),r2=step(0.f,.04f,true,1),r3=step(170.f,.06f,true,1);
     const auto h1=step(90.f,.07f,true,0);
     std::uint8_t h2=0;for(int i=0;i<20;++i)h2=std::max(h2,step(90.f,.07f,true,0)); // 280 ms: still the hold
     std::uint8_t after=9;for(int i=0;i<30;++i)after=step(90.f,.07f,true,0); // past 500 ms
     CHECK(s0==0&&s1==0&&r1==1&&r2==1&&r3==1&&h1==2&&h2==2&&after==0,"ragdoll step: none, ragdoll while the word is set, the hold after it clears, then none");
     CHECK(mva::ragStats.enters.load()==e0+1&&mva::ragStats.exits.load()==x0+1&&mva::ragStats.flipsRagdoll.load()>=f0+3&&mva::ragStats.episodes.load()==ep0+1,
           "ragdoll step: one episode (entered, exited, three yaw flips while ragdoll, closed on the first clean tick)");
     {std::lock_guard lock(mva::ragRingMutex);const auto& e=mva::ragRing[(mva::ragRingNext+mva::ragRing.size()-1)%mva::ragRing.size()];
      CHECK(e.guid==42&&e.ticks==3&&e.flips>=2&&e.maxTurnDeg>170.f&&e.exitTurnDeg>70.f&&e.firstCleanMs>=500,"ragdoll episode: ticks, flips, the exit turn, the first clean tick after the hold");}
     CHECK(step(0,0,false,1)==0&&step(0,0,true,1,false)==0,"ragdoll step: an unread word is no ragdoll; gate off: measured, never skipped");
     for(int i=0;i<60;++i)step(0,0,true,0);}
    // a corpse (HandleHasDied sets the word for good): camera vectors over the fall window only, then its own vectors while it lies or is
    // carried; a flip past the window skips it again for the hold
    {const std::uint64_t ms=1'000'000ull,hold=500*ms,win=3000*ms;std::uint64_t tick=5000,now=50'000'000'000ull;
     const auto step=[&](float yaw,float x,std::uint32_t word){const auto p=pose(77,0,yaw,x,0);++tick;now+=14*ms;
        return mva::ragdollStep(77,0,p.m,true,word,true,true,tick,now,hold,win);};
     step(0,0,0);
     std::uint8_t inFall=1;for(int i=0;i<200;++i)inFall=std::min(inFall,step(0,0,1)); // 2.8 s
     std::uint8_t lying=9;for(int i=0;i<30;++i)lying=step(0,0,1);                     // past 3 s, still
     std::uint8_t carried=9;float x=0;for(int i=0;i<60;++i){x+=.15f;carried=std::max<std::uint8_t>(carried==9?0:carried,step(float(i%10),x,1));} // dragged along at 10 m/s
     const auto k0=mva::ragStats.keptTicks.load();
     const auto flip=step(180.f,x,1);std::uint8_t after=0;for(int i=0;i<20;++i)after=std::max(after,step(180.f,x,1)); // 280 ms of the hold
     std::uint8_t settled=9;for(int i=0;i<40;++i)settled=step(180.f,x,1);
     CHECK(inFall==1&&lying==0&&carried==0,"corpse: camera vectors over the 3 s fall window, then its own vectors lying or carried (no judder on a carried body)");
     CHECK(flip==1&&after==1&&settled==0&&mva::ragStats.keptTicks.load()>k0,"corpse: a 180 degree flip past the window: camera vectors for the hold, then its own again");
     {const auto p=pose(78,0,0,0,0);++tick;now+=14*ms;mva::ragdollStep(78,0,p.m,true,1,true,true,tick,now,hold,0);
      std::uint8_t all=1;for(int i=0;i<400;++i){const auto q=pose(78,0,0,0,0);++tick;now+=14*ms;all=std::min(all,mva::ragdollStep(78,0,q.m,true,1,true,true,tick,now,hold,0));}
      CHECK(all==1,"ragdollwin 0: camera vectors for the whole ragdoll");}
     step(0,x,0);for(int i=0;i<60;++i)step(0,x,0);
     {std::lock_guard lock(mva::ragRingMutex);const auto& e=mva::ragRing[(mva::ragRingNext+mva::ragRing.size()-1)%mva::ragRing.size()];
      CHECK(e.guid==77&&e.keptTicks>=80&&e.skipTicks>=20&&e.lastUnstableMs>3000,"corpse episode: ticks past the window kept / skipped, the last unstable tick (ms)");}
     }
    {const std::string r=mva::report();
     CHECK(r.find("\"ragdoll\":{\"gate\":")!=std::string::npos&&r.find("\"bigMoves\":[")!=std::string::npos&&r.front()=='{'&&r.back()=='}',"mvactors report: the ragdoll section");
     int depth=0;bool ok=true;for(char c:r){if(c=='{'||c=='[')++depth;if(c=='}'||c==']')--depth;if(depth<0)ok=false;}CHECK(ok&&depth==0,"mvactors report: braces balance");
     std::printf("%s\n",r.c_str());}
    std::printf(fails?"\n%d FAILED\n":"\nALL PASSED\n",fails);
    return fails?1:0;
}
