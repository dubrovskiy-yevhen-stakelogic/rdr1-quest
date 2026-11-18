// Host regression (MSVC: build as tools/test-casual-trigger.cpp). HEAD SHOW: the head comes back while the eye is away from the head joint
// (knockdown, get-up) and only then; no flicker on spikes, an immediate hide when the camera is back at the head, a camera that settles
// 28..40 cm away hides after the band dwell, the hat only far from the eye, held state on bad input, a stale head hides after its hold,
// one step per plan, lever parsing.
#include "vr/VrHeadShow.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <mutex>
namespace HS=rdrvr::vr::headshow;

static std::uint32_t lcg=12345u;
static float noise(float amp){lcg=lcg*1664525u+1013904223u;return (float((lcg>>8)&0xffff)/65535.f*2.f-1.f)*amp;}

struct Run {
    HS::Machine m{};HS::Config c{};std::uint64_t now=1'000'000'000ull;int transitions=0;bool last=false;
    std::uint64_t firstShowNs{},firstHideNs{};
    bool step(float d,std::uint8_t cls=HS::ClassOk,std::uint64_t dtNs=13'888'889ull,bool fresh=true,std::uint32_t eyeGuid=7,bool eyeOk=true){
        now+=dtNs;HS::Input in{};in.now=now;in.eyeOk=eyeOk;in.eye[0]=d;in.eye[1]=1.7f;in.eye[2]=0.f;in.headOk=true;in.upm=1.f;in.eyeGuid=eyeGuid;
        in.head.pos[0]=0.f;in.head.pos[1]=1.7f;in.head.pos[2]=0.f;in.head.ns=fresh?now-5'000'000ull:now-500'000'000ull;in.head.guid=7;in.head.gateClass=cls;
        m.step(in,c);if(m.shown!=last){++transitions;last=m.shown;if(m.shown&&!firstShowNs)firstShowNs=now;if(!m.shown&&!firstHideNs)firstHideNs=now;}
        return m.shown;}
};

int main(){
    // 1. normal play: d = 0.19 +- 0.03, look-down freeze excursions to 0.34, crouch dips: never shown
    {Run r{};
     for(int i=0;i<72*60;++i){float d=.19f+noise(.03f);if(i%400<30)d=.34f+noise(.005f);if(i%900<40)d=.12f;assert(!r.step(std::fabs(d)));}
     assert(r.transitions==0&&r.m.shows==0);}
    // 2. knockdown: 0.19 -> 1.4 over 300 ms, 2 s down, get-up back to 0.19 over 1 s, +-5 cm noise
    {Run r{};std::uint64_t crossNs{},belowNs{};
     for(int i=0;i<36;++i)r.step(.19f);
     for(int i=0;i<=21;++i){const float d=.19f+(1.4f-.19f)*float(i)/21.f+noise(.05f);r.step(d,HS::ClassRagdoll);if(!crossNs&&d>.40f)crossNs=r.now;}
     for(int i=0;i<144;++i)r.step(1.4f+noise(.05f),HS::ClassRagdoll);
     // get-up ramp to 0.25 m (under hide, over inside): hidden on the first plan under hide (no view from inside the face); then 0.19
     for(int i=0;i<=72;++i){const float base=1.4f+(.25f-1.4f)*float(i)/72.f;const float d=base+noise(.05f)*(base>.35f?1.f:.1f);
        r.step(d,HS::ClassOther);if(!belowNs&&d<.28f)belowNs=r.now;}
     for(int i=0;i<72;++i)r.step(.25f+noise(.005f),HS::ClassOther);
     for(int i=0;i<72;++i)r.step(.19f+noise(.01f));
     assert(r.transitions==2&&r.m.shows==1&&r.m.hides==1);
     assert(r.firstShowNs>=crossNs&&r.firstShowNs-crossNs<=r.c.showNs+3*13'888'889ull);
     assert(r.firstHideNs>=belowNs&&r.firstHideNs-belowNs>=r.c.hideNs&&r.firstHideNs-belowNs<=r.c.hideNs+3*13'888'889ull);
     assert(r.m.hist[HS::ClassRagdoll][5]>0&&r.m.shownOkFrames==0);}
    // 3. spike: 1.2 m for 50 ms -> no show
    {Run r{};for(int i=0;i<10;++i)r.step(.19f);for(int i=0;i<3;++i)r.step(1.2f);for(int i=0;i<10;++i)r.step(.19f);assert(r.transitions==0);}
    // 4. snap: shown, then the camera back into the head in one plan -> hidden on that plan
    {Run r{};for(int i=0;i<20;++i)r.step(1.2f);assert(r.m.shown);assert(!r.step(.15f));assert(r.m.insideGuards==1&&r.m.hides==1);}
    // 4b. the camera settles 28..40 cm from the joint: shown no longer than the band dwell; spikes in the band do not hide
    {Run r{};for(int i=0;i<20;++i)r.step(1.2f);assert(r.m.shown);
     for(int i=0;i<60;++i){r.step(.33f);}assert(r.m.shown); // 833 ms
     for(int i=0;i<20;++i){r.step(.33f);}assert(!r.m.shown&&r.m.bandHides==1);
     Run q{};for(int i=0;i<20;++i)q.step(1.2f);for(int k=0;k<10;++k){for(int i=0;i<30;++i)q.step(.35f);q.step(.6f);}assert(q.m.shown&&q.m.bandHides==0);}
    // 4c. the hat: drawn only with the head at least 60 cm away
    {Run r{};for(int i=0;i<20;++i)r.step(1.2f);const HS::Config c{};assert(r.m.hat(c));r.step(.5f);assert(r.m.shown&&!r.m.hat(c));
     r.step(.7f);assert(r.m.hat(c));Run q{};assert(!q.m.hat(c));}
    // 5. stale head / bad eye / another actor: state held
    {Run r{};for(int i=0;i<20;++i)r.step(1.2f);assert(r.m.shown);
     for(int i=0;i<50;++i){assert(r.step(.1f,HS::ClassOk,13'888'889ull,false));}assert(r.m.staleHolds==50); // 694 ms: held
     for(int i=0;i<5;++i){assert(r.step(.1f,HS::ClassOk,13'888'889ull,true,7,false));}
     for(int i=0;i<5;++i){assert(r.step(.1f,HS::ClassOk,13'888'889ull,true,0xC5C50003u));}assert(r.m.otherActorHolds==5);
     Run q{};for(int i=0;i<50;++i)assert(!q.step(1.2f,HS::ClassRagdoll,13'888'889ull,false));
     // a head that stays stale past staleHideNs: hidden (the rule without HEAD SHOW), counted
     Run t{};for(int i=0;i<20;++i)t.step(1.2f);assert(t.m.shown);
     for(int i=0;i<57;++i)assert(t.step(1.2f,HS::ClassOk,13'888'889ull,false));
     for(int i=0;i<3;++i)t.step(1.2f,HS::ClassOk,13'888'889ull,false);assert(!t.m.shown&&t.m.staleHides==1);
     // a fresh sample in between restarts the stale hold
     Run w{};for(int i=0;i<20;++i)w.step(1.2f);for(int k=0;k<4;++k){for(int i=0;i<40;++i)w.step(1.2f,HS::ClassOk,13'888'889ull,false);w.step(1.2f);}
     assert(w.m.shown&&w.m.staleHides==0);}
    // 6. one step per plan key (the stepper), and another actor's plan never reports shown
    {std::mutex lock;HS::Stepper st{};HS::setConfig(HS::Config{});std::uint64_t now=5'000'000'000ull;
     const auto in=[&](float d,std::uint32_t guid){HS::Input x{};x.now=now;x.eyeOk=true;x.eye[0]=d;x.headOk=true;x.upm=1.f;x.eyeGuid=guid;x.head.ns=now;x.head.guid=7;return x;};
     for(std::uint64_t k=1;k<=20;++k){now+=13'888'889ull;st.step(lock,k,k,in(1.2f,7));st.step(lock,k,k,in(1.2f,7));}
     assert(HS::shownNow.load());
     now+=13'888'889ull;assert(st.step(lock,100,100,in(1.2f,7)).head);assert(st.step(lock,100,100,in(.1f,7)).head); // the same key: the first verdict
     {HS::Show c{};assert(st.cached(lock,100,100,c)&&c.head&&c.hat&&!st.cached(lock,102,102,c));}
     now+=13'888'889ull;{const auto v=st.step(lock,101,101,in(1.2f,0xC5C50003u));assert(!v.head&&!v.hat);} // a cutscene John's plan: never shown
     const auto j=st.report(lock,now);assert(j.find("\"shown\":true")!=std::string::npos&&j.find("\"episodes\":[")!=std::string::npos);}
    // 7. levers: off -> never shown; bad strings -> defaults
    {Run r{};r.c.on=false;for(int i=0;i<50;++i)assert(!r.step(1.4f));}
    {const HS::Config d{};HS::Config c=HS::parseCm(d,"50 30 20");assert(std::fabs(c.showM-.5f)<1e-6f&&std::fabs(c.hideM-.3f)<1e-6f&&std::fabs(c.insideM-.2f)<1e-6f&&c.hatM==d.hatM);
     c=HS::parseCm(d,"50 30 20 80");assert(std::fabs(c.hatM-.8f)<1e-6f);c=HS::parseCm(d,"50 30 20 25");assert(c.hatM==d.hatM&&c.showM==d.showM);
     c=HS::parseCm(c,"30 30 20");assert(c.showM==d.showM&&c.hideM==d.hideM&&c.insideM==d.insideM);
     c=HS::parseCm(d,"40 28 2");assert(c.insideM==d.insideM);c=HS::parseCm(d,"abc");assert(c.showM==d.showM);
     c=HS::parseMs(d,"50 500");assert(c.showNs==50'000'000ull&&c.hideNs==500'000'000ull&&c.bandNs==d.bandNs);c=HS::parseMs(c,"-1 5");assert(c.showNs==d.showNs&&c.hideNs==d.hideNs);
     c=HS::parseMs(d,"50 0 2000");assert(c.bandNs==2'000'000'000ull&&c.hideNs==0);assert(d.hideNs==0&&d.bandNs==1'000'000'000ull);}
    // 8. the seqlock box
    {HS::HeadSample s{};s.pos[0]=1.f;s.pos[1]=2.f;s.pos[2]=3.f;s.ns=42;s.guid=7;s.gateClass=HS::ClassRagdoll;HS::head.store(s);
     HS::HeadSample o{};assert(HS::head.load(o)&&o.pos[2]==3.f&&o.ns==42&&o.guid==7&&o.gateClass==HS::ClassRagdoll);}
    std::cout<<"Head show normal play, knockdown, spike, snap, band, hat, held / stale input, per-plan step and lever tests passed\n";
    return 0;
}
