// Host regression (MSVC: cl /std:c++20 /EHsc /I<fake sys/system_properties.h> /ID:/RDRVRBuild/fast/generated /Inative/src /Inative/src/vr):
// the object glow table, the SMALL OBJECTS keep-list and the extra QUEST MARKER beam.
#include <cstdio>
#include <cstring>
#include <string>
#include "engine/SmallObjectCull.h"
#include "vr/VrMarker.h"
namespace og=rdrvr::engine::objectglow;
namespace mk=rdrvr::vr::marker;
using Cull=rdrvr::engine::SmallObjectCull;
static int fails=0;
#define CHECK(c,msg) do{if(!(c)){std::printf("FAIL %s  (%s)\n",msg,#c);++fails;}else std::printf("ok   %s\n",msg);}while(0)

static bool plainRead(void*,std::uintptr_t a,void* out,std::size_t n){std::memcpy(out,reinterpret_cast<const void*>(a),n);return true;}

// The engine's layout: entry[64] stride 0x30, count at +0xc00.
struct Table {
    alignas(16) unsigned char bytes[0xc10]{};std::uintptr_t instance{};
    std::array<std::array<float,4>,64> effects{};
    void add(const float* pos,bool priority,unsigned type=0){
        std::int32_t n;std::memcpy(&n,bytes+0xc00,4);auto* e=bytes+std::size_t(n)*0x30;
        if(pos){std::memcpy(effects[unsigned(n)].data(),pos,12);const auto p=reinterpret_cast<std::uintptr_t>(effects[unsigned(n)].data());std::memcpy(e+0x18,&p,8);}
        e[0x20]=priority?1:0;e[0x21]=(unsigned char)type;++n;std::memcpy(bytes+0xc00,&n,4);}
    void use(){instance=reinterpret_cast<std::uintptr_t>(bytes);og::instanceSlot.store(reinterpret_cast<std::uintptr_t>(&instance));}
    void clear(){std::memset(bytes,0,sizeof bytes);}
};

// A camera at the origin looking anywhere; a drawable with the skull's cull sphere (radius 0.4243) at a given place.
struct Scene {
    Cull cull;alignas(16) unsigned char viewport[0x200]{};unsigned char request[0x90]{};unsigned char container[8]{};
    alignas(16) unsigned char drawable[0xa0]{};alignas(16) unsigned char info[0xb0]{};
    Scene(){cull.configure(plainRead);const float p11=673.3f*2.f/Cull::eyeHeightPx;std::memcpy(viewport+0x1c0+0x14,&p11,4);
        const auto c=reinterpret_cast<std::uintptr_t>(container);std::memcpy(request+0x88,&c,8);
        const float r=.4243f;std::memcpy(drawable+0x90,&r,4);const auto d=reinterpret_cast<std::uintptr_t>(drawable);std::memcpy(info+0x80,&d,8);}
    void frame(unsigned px){cull.frame(reinterpret_cast<std::uintptr_t>(request),viewport,px,0);}
    bool skipped(const float* at){
        float m[16]{1,0,0,0,0,1,0,0,0,0,1,0,at[0],at[1],at[2],1};std::memcpy(info,m,sizeof m);
        bool skip;{Cull::Stage stage(cull,Cull::Depth,true);const auto t=cull.begin(container,info,true);skip=t.skip;cull.end(t);}
        return skip;}
};
static bool has(const std::string& s,const char* what){return s.find(what)!=std::string::npos;}

int main(){
    {   // readPositions
        Table t{};t.use();const float a[3]{10,1,2},b[3]{20,1,2},origin[3]{0,0,0};
        t.add(a,false);t.add(nullptr,false);t.add(b,true,3);t.add(origin,true);
        og::Glow g[16];unsigned entries=0;const unsigned n=og::readPositions(plainRead,g,16,entries);
        CHECK(entries==4,"entries = the table size");
        CHECK(n==2,"an entry without an effect and a position at the origin are dropped");
        CHECK(n==2&&g[0].pos[0]==10.f&&g[0].priority==0,"pickup entry: priority 0");
        CHECK(n==2&&g[1].pos[0]==20.f&&g[1].priority==1&&g[1].type==3,"script entry: priority and type");
        std::int32_t bad=65;std::memcpy(t.bytes+0xc00,&bad,4);CHECK(og::readPositions(plainRead,g,16,entries)==0,"a count above the capacity is refused");
        og::instanceSlot.store(0);std::memcpy(t.bytes+0xc00,&bad,4);CHECK(og::readPositions(plainRead,g,16,entries)==0,"no install: empty table");
    }
    const float skull[3]{-3258.26f+3282.55f,1.f,2725.94f-2704.5f};
    const float glowPoint[3]{skull[0],skull[1]+.1f,skull[2]};
    {   // the keep-list (the skull at 32.5 m is 17.6 px)
        Table t{};Scene s{};
        s.frame(32);CHECK(s.skipped(skull),"32 px, no install: the object is skipped");s.cull.endFrame();
        t.use();t.add(glowPoint,true);
        s.frame(32);s.frame(32);s.frame(32);s.frame(32);
        CHECK(!s.skipped(skull),"32 px: the object under a glow stays");
        const float elsewhere[3]{skull[0]+4.f,skull[1],skull[2]};
        CHECK(s.skipped(elsewhere),"32 px: an object 4 m away is still skipped");
        s.cull.endFrame();
        const auto r=s.cull.report();
        CHECK(has(r,"\"objectGlows\":1"),"report objectGlows");
        CHECK(has(r,"\"objectGlowPriority\":1"),"report objectGlowPriority");
        CHECK(has(r,"\"objectGlowNearestPriority\":true"),"report objectGlowNearestPriority");
        CHECK(has(r,"\"glowKept\":1"),"report glowKept");
        CHECK(has(r,"\"objectGlowNearestPx\":17."),"report objectGlowNearestPx is the object's size");
        const auto m=r.find("\"objectGlowNearestM\":");CHECK(m!=std::string::npos&&r.compare(m+21,3,"32.")==0,"report objectGlowNearestM");
        t.clear();s.frame(32);s.frame(32);s.frame(32);s.frame(32);
        CHECK(s.skipped(skull),"glow gone: the size test again");s.cull.endFrame();
        t.add(glowPoint,false);s.frame(0);s.frame(0);s.frame(0);s.frame(0);
        CHECK(!s.cull.active(),"threshold 0: inactive");
        og::instanceSlot.store(0);
    }
    {   // a pickup glow is kept but sorts after a priority glow for the sample
        Table t{};Scene s{};t.use();const float nearer[3]{5,0,5};t.add(nearer,false);t.add(glowPoint,true);
        s.frame(16);CHECK(!s.skipped(nearer),"a pickup under its glow stays");s.cull.endFrame();
        CHECK(has(s.cull.report(),"\"objectGlowNearestPriority\":true"),"the priority glow is sampled, not the nearer pickup");
        s.frame(16);s.frame(16);s.frame(16);s.frame(16);
        {   const float tiny=.03f,beside[3]{glowPoint[0]+.2f,glowPoint[1],glowPoint[2]};std::memcpy(s.drawable+0x90,&tiny,4);(void)s.skipped(beside);
            const float big=.4243f;std::memcpy(s.drawable+0x90,&big,4);(void)s.skipped(skull);s.cull.endFrame();}
        CHECK(has(s.cull.report(),"\"objectGlowNearestPx\":17."),"nearest px: the object at the glow, not a smaller neighbour");
        og::instanceSlot.store(0);
    }
    {   // the beam: an object glow never takes the objective's beam
        const rdrvr::vr::arc::V3 eye{0,1.7f,0},up{0,1,0};const mk::Look look{};
        std::array<mk::Target,3> t{};t[0]={{10,0,0},mk::SrcObjectGlow,-3};t[1]={{0,0,30},mk::SrcObjective,33};t[2]={{0,0,50},mk::SrcMission,1};
        rdrvr::vr::hands::ArcFrame f{};std::array<unsigned,mk::maxDrawn> idx{};
        CHECK(mk::buildFrame(t.data(),3,eye,up,1.f,7,look,f,&idx)==2,"glow + objective: two beams");
        CHECK(idx[0]==1&&idx[1]==0,"the objective's beam first, the nearest objective, then the glow");
        CHECK(f.marker.centre[2]==30.f,"the disc stays on the objective");
        CHECK(f.count==7,"3 points + an invisible link + 3 points");
        CHECK(mk::buildFrame(t.data()+1,2,eye,up,1.f,7,look,f,&idx)==1,"without a glow: one beam, as before");
        CHECK(idx[0]==1-1,"the nearest target");
        CHECK(f.count==3,"3 points, no link");
        CHECK(mk::buildFrame(t.data(),1,eye,up,1.f,7,look,f,&idx)==1&&idx[0]==0,"glow alone: one beam");
        CHECK(f.marker.centre[0]==10.f,"the disc on the glow when alone");
        std::array<mk::Target,2> two{};two[0]={{1,0,0},mk::SrcObjectGlow,-3};two[1]={{0,0,30},mk::SrcObjective,33};
        CHECK(mk::buildFrame(two.data(),2,eye,up,1.f,7,look,f,&idx)==1&&idx[0]==1,"glow inside the arrival radius: no beam for it");
        std::array<mk::Target,2> merged{};merged[0]={{0,0,20},mk::SrcObjective,33};merged[1]={{10,0,30},std::uint32_t(mk::SrcObjective|mk::SrcObjectGlow),33};
        CHECK(mk::buildFrame(merged.data(),2,eye,up,1.f,7,look,f,&idx)==2&&idx[0]==0&&idx[1]==1,"a glow merged with a blip keeps a beam when a nearer target has the first");
        merged[0].pos={0,0,50};
        CHECK(mk::buildFrame(merged.data(),2,eye,up,1.f,7,look,f,&idx)==1&&idx[0]==1,"a glow merged with the nearest blip: one beam");
    }
    std::printf(fails?"%d FAILED\n":"all passed\n",fails);
    return fails?1:0;
}
