// Host regression (MSVC: build as tools/test-casual-trigger.cpp). The weapon wheel block: decision table, a model of the HUD opener as
// rdrHudDefault::HandleInputs / Update run it, install refusal (against D:/RDRVRBuild/re/librdr.so when present), the LB hold ring and
// LB off the pad while blocking, engine put-aways after an LB release.
#include "vr/VrWheel.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
namespace W=rdrvr::vr::wheel;

static int originalCalls{};static bool originalResult{true};
static bool original(const void*) {++originalCalls;return originalResult;}
static constexpr std::uintptr_t base=0x7000000000ull;

static void resetStats(){for(auto& s:W::sites){s.calls=0;s.blocked=0;s.passed=0;s.lastNs=0;}W::otherCalls=0;W::radialOpenSeen=0;originalCalls=0;}

// rdrHudDefault as proven in all.s: HandleInputs gates every event on CanQuickPickActivate (0x29f9a38..0x29f9a4c), SHOW_HIDE sets +0x89,
// RELEASED clears it and sends the tap; Update accumulates +0x8c and opens past 0.2 s; RadialWeaponQuickPick::Update exits a focused
// wheel when the predicate is false.
struct Hud {
    bool flag{};float timer{};bool open{};int taps{};int exits{};
    void event(int e){ // 0 = other, 1 = SHOW_HIDE, 2 = SHOW_HIDE*RELEASED
        const bool can=W::decide(nullptr,base+W::siteReturns[W::SiteHud],base,original,1);
        if(!can){flag=false;timer=0.f;return;}
        if(e==1)flag=true;
        else if(e==2){flag=false;++taps;} // 0x29f9ac0: "quickpick_released" on every release (the tap, or the open wheel's commit)
    }
    void update(float dt){
        if(open){const bool can=W::decide(nullptr,base+W::siteReturns[W::SiteRadialUpdate],base,original,2);if(!can){open=false;++exits;}}
        if(!flag)return;timer+=dt;if(timer>0.2f){open=true;flag=false;timer=0.f;} // 0x29f98a8..0x29f9924: activate, then +0x89 / +0x8c cleared
    }
};
static void hold(Hud& h,float seconds){h.event(1);for(float t=0;t<seconds;t+=1.f/72.f)h.update(1.f/72.f);h.event(2);}

// librdr.so PT_LOAD segments, served as the engine image at `base`.
struct Image {std::vector<unsigned char> file;struct Seg{std::uint64_t va,off,size;};std::vector<Seg> segs;bool ok{};
    std::uintptr_t corrupt{};std::uint64_t gotWord{};
    bool load(const char* path){std::ifstream f(path,std::ios::binary);if(!f)return false;file.assign(std::istreambuf_iterator<char>(f),{});
        if(file.size()<64)return false;std::uint64_t phoff{};std::uint16_t phent{},phnum{};std::memcpy(&phoff,&file[0x20],8);std::memcpy(&phent,&file[0x36],2);std::memcpy(&phnum,&file[0x38],2);
        for(unsigned i=0;i<phnum;++i){const auto* p=&file[phoff+i*phent];std::uint32_t t{};std::memcpy(&t,p,4);if(t!=1)continue;
            Seg s{};std::memcpy(&s.off,p+8,8);std::memcpy(&s.va,p+16,8);std::memcpy(&s.size,p+32,8);segs.push_back(s);}
        ok=!segs.empty();return ok;}
    bool read(std::uintptr_t a,void* out,std::size_t n)const{
        if(a<base)return false;const auto va=a-base;
        for(const auto& s:segs)if(va>=s.va&&va+n<=s.va+s.size){std::memcpy(out,&file[s.off+(va-s.va)],n);
            if(corrupt&&a<=corrupt&&corrupt<a+n)static_cast<unsigned char*>(out)[corrupt-a]^=0x5a;return true;}
        return false;}
};

int main(){
    // 1. decision table: lever x holsters x caller
    for(int lever=0;lever<=2;++lever)for(int hol=0;hol<=1;++hol){
        W::lever=lever;W::holstersOn=hol!=0;
        const bool want=lever==2||(lever==1&&hol);
        assert(W::blocks(lever,hol!=0)==want);
        for(unsigned s=0;s<W::SiteCount;++s){resetStats();originalResult=true;
            const bool r=W::decide(nullptr,base+W::siteReturns[s],base,original,5);
            assert(r==!want);assert(originalCalls==(want?0:1));
            assert(W::sites[s].calls==1&&W::sites[s].blocked==(want?1u:0u)&&W::sites[s].passed==(want?0u:1u));
            assert(W::radialOpenSeen==((want&&s==W::SiteRadialUpdate)?1u:0u));}
        resetStats(); // an unknown caller is always stock
        originalResult=true;assert(W::decide(nullptr,base+0x1234,base,original,5)&&originalCalls==1&&W::otherCalls==1);
        originalResult=false;assert(!W::decide(nullptr,base+W::siteReturns[0]+4,base,original,5)&&originalCalls==2&&W::otherCalls==2);
    }
    assert(W::siteOf(0x29f9a3c,0)==-1&&W::siteOf(base-4,base)==-1&&W::siteOf(base+0x2578360,base)==W::SiteRadialDeactivate);

    // 2. the HUD model
    originalResult=true;
    {W::lever=0;W::holstersOn=true;resetStats();Hud h{};
     hold(h,0.1f);assert(!h.open&&h.taps==1);              // stock: a short tap sends quickpick_released
     hold(h,0.25f);assert(h.open);h.open=false;             // past 0.2 s the wheel opens
     hold(h,10.f);assert(h.open&&h.taps==3);}
    {W::lever=1;W::holstersOn=true;resetStats();Hud h{};
     for(float s:{0.1f,0.25f,10.f}){hold(h,s);assert(!h.open&&!h.flag&&h.taps==0);} // blocked: never opens, no tap, +0x89 clear
     assert(W::sites[W::SiteHud].blocked>=6&&W::radialOpenSeen==0);}
    {W::lever=1;W::holstersOn=false;resetStats();Hud h{};hold(h,0.25f);assert(h.open);} // holsters off: the stock wheel
    {W::lever=0;W::holstersOn=true;resetStats();Hud h{};h.event(1);for(int i=0;i<30;++i)h.update(1.f/72.f);assert(h.open);
     W::lever=2;h.update(1.f/72.f);assert(!h.open&&h.exits==1&&W::radialOpenSeen==1); // the lever on while open: Exit on the next frame
     h.event(1);for(int i=0;i<100;++i)h.update(1.f/72.f);assert(!h.open);}

    // 3. install refusal
    {const auto replaceNever=[](std::uintptr_t,std::uintptr_t,std::uintptr_t){assert(false&&"replace called");return false;};
     const auto gotOk=[](std::uintptr_t,std::uintptr_t){return true;};
     const auto readNone=[](void*,std::uintptr_t,void*,std::size_t){return false;};
     W::hooked=false;
     assert(!W::install(readNone,base,true,gotOk,replaceNever)&&std::strcmp(W::installState.load(),"OFF")==0);
     assert(!W::install(readNone,base,false,gotOk,replaceNever)&&std::strcmp(W::installState.load(),"ENGINE MISMATCH")==0);
     Image img;
     if(img.load("D:/RDRVRBuild/re/librdr.so")){
        const auto rd=[&](void*,std::uintptr_t a,void* o,std::size_t n){return img.read(a,o,n);};
        for(const auto& p:W::prints){img.corrupt=base+p.offset+p.size/2;
            assert(!W::install(rd,base,false,gotOk,replaceNever)&&std::strcmp(W::installState.load(),"ENGINE MISMATCH")==0);}
        img.corrupt=0;
        const auto gotWord=[&](std::uintptr_t slot,std::uintptr_t target){
            assert(slot==W::canQuickPickGotOffset&&target==W::canQuickPickOffset);
            // R_AARCH64_JUMP_SLOT: the file's GOT word holds the PLT resolver; the loaded word is checked against base+target on device
            return false;};
        assert(!W::install(rd,base,false,gotWord,replaceNever)&&std::strcmp(W::installState.load(),"WORD MISMATCH")==0);
        int replaced=0;
        assert(!W::install(rd,base,false,gotOk,[&](std::uintptr_t a,std::uintptr_t e,std::uintptr_t r){
            assert(a==base+W::canQuickPickGotOffset&&e==base+W::canQuickPickOffset&&r==reinterpret_cast<std::uintptr_t>(&W::canQuickPickHook));++replaced;return false;}));
        assert(replaced==1&&std::strcmp(W::installState.load(),"HOOK NOT INSTALLED")==0&&!W::hooked);
        assert(W::install(rd,base,false,gotOk,[&](std::uintptr_t,std::uintptr_t,std::uintptr_t){++replaced;return true;}));
        assert(replaced==2&&std::strcmp(W::installState.load(),"INSTALLED")==0&&W::hooked);
        std::cout<<"prints checked against librdr.so\n";
     }else std::cout<<"librdr.so not found: print checks skipped\n";
     W::hooked=false;}

    // 4. LB ring
    {W::PadTrack t{};std::uint64_t now=1'000'000'000;
     const auto run=[&](bool lb,unsigned frames,std::uint8_t ctx,int pres){for(unsigned i=0;i<frames;++i){now+=13'888'889;t.note(now,lb,true,ctx,pres);}};
     run(false,5,W::CtxWorld,1);run(true,10,W::CtxWorld,1);run(false,3,W::CtxWorld,1); // ~139 ms
     assert(t.total==1&&t.holdsOver200==0&&t.framesInWorld==10&&t.ring[0].durNs>130'000'000ull&&t.ring[0].durNs<150'000'000ull&&t.ring[0].ctx==W::CtxWorld);
     run(true,30,W::CtxNotWorld,4);run(false,1,W::CtxWorld,1);                         // ~417 ms in a scene camera
     assert(t.total==2&&t.holdsOver200==1&&t.framesInWorld==10&&t.ring[1].ctx==W::CtxNotWorld&&t.ring[1].presentation==4);
     for(int i=0;i<20;++i){run(true,2,W::CtxWorld,1);run(false,2,W::CtxWorld,1);}
     assert(t.total==22&&t.next==22%16);
     const auto json=[&]{W::padTrack=t;return W::report();}();
     assert(json.find("\"holdsOver200ms\":1")!=std::string::npos&&json.find("\"holds\":22")!=std::string::npos);}

    // 5. LB off the pad while blocking (hook installed or not): its release in select state 4 would holster the held item
    for(int pres=0;pres<=6;++pres){
        const bool gameplay=pres==1||pres==4||pres==5||pres==6;
        assert(W::dropsLb(1,true,pres,false)==gameplay);
        assert(!W::dropsLb(1,true,pres,true)); // lasso yank
        assert(!W::dropsLb(0,true,pres,false)); // stock wheel: stock LB
        assert(W::dropsLb(2,false,pres,false)==gameplay);
        assert(!W::dropsLb(1,false,pres,false));}
    // 6. engine put-aways against the last LB release the game received
    {W::PadTrack t{};const std::uint64_t ms=1'000'000ull;std::uint64_t now=5'000*ms;
     W::enginePutAways=0;W::putAwayAfterLb=0;W::lbReleaseNs=0;
     W::noteEnginePutAway(now);assert(W::enginePutAways==1&&W::putAwayAfterLb==0); // no LB ever
     t.note(now,true,true,W::CtxWorld,1);now+=100*ms;t.note(now,false,true,W::CtxWorld,1);
     W::noteEnginePutAway(now+250*ms);assert(W::enginePutAways==2&&W::putAwayAfterLb==1);
     W::noteEnginePutAway(now+400*ms);assert(W::enginePutAways==3&&W::putAwayAfterLb==1);
     W::padTrack=t;const auto json=W::report();
     assert(json.find("\"putAways\":{\"engine\":3,\"afterLb\":1}")!=std::string::npos&&json.find("\"drops\":{\"hooked\":0,\"unhooked\":0}")!=std::string::npos);}
    std::cout<<"Wheel block decision, HUD model, install refusal, LB ring, LB drop and put-away tests passed\n";
    return 0;
}
