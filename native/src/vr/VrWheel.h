#pragma once
// WHEEL: the game's radial weapon wheel (RadialWeaponQuickPick) stays closed while weapons are picked from the VR holsters.
//
// Stock path (librdr.so 1.58 arm64, vaddr):
//  rdrHudDefault::HandleInputs 0x29f9944 runs for every HUD input event. With a player (hudManager+0xa20) it calls
//  sagPlayer::CanQuickPickActivate (0x29f9a38, return 0x29f9a3c) and tests `orn w8,w21,w8; tbz` at 0x29f9a44: the MpWeaponSuppressor
//  object active (w21) or a false result clears hud+0x89 / hud+0x8c and returns. Otherwise @RADIAL_MENU.SHOW_HIDE sets hud+0x89 and
//  @RADIAL_MENU.SHOW_HIDE*RELEASED clears it and sends "quickpick_released" (UIManager vtable +0x3b0, the tap).
//  rdrHudDefault::Update 0x29f9864 adds the frame time to hud+0x8c while +0x89 is set; past 0.2 s it activates, shows and focuses the
//  "RadialWeaponQuickPick" UI object and sets player+0x43e. That is the wheel's only opener. The default binding of SHOW_HIDE
//  (static initializer 0x1ac8aec, group 17) is pad 14 = LB = the left grip, TAB and two touch regions.
//  CanQuickPickActivate 0x26a0088 is a pure predicate. Its five callers all go through the PLT stub 0x2c8e4b0 (JUMP_SLOT 0x2e4f8b8):
//   0x29f9a3c HandleInputs (the opener gate), 0x2578180 RadialWeaponQuickPick::Update (false while focused -> UIObject::Exit, an open
//   wheel closes), 0x2578360 RadialWeaponQuickPick::ShouldDeactivate, 0x25774b4 WeaponQuickPick::Update, 0x25776dc
//   WeaponQuickPick::ShouldDeactivate.
// Fix: GOT hook of CanQuickPickActivate. A call from one of the five known return addresses gets false while the lever blocks; any
// other caller, or the lever off, gets the stock result. The HUD then takes the engine's own "cannot quick-pick" branch: no wheel, no
// tap message, whatever button drives SHOW_HIDE. PREVIOUS_TAB / SKIP_DEAL (the other LB actions), the holsters (SelectItem), Dead Eye,
// the satchel and the lasso yank never read this predicate.
// Any print or word mismatch installs nothing (stock behaviour).
// LB itself (pad side, hook or not): sagPlayer::UpdateWeaponSelection 0x269f320 enters select state 4 on a press of inventory input 16
// (GetCurrWpnRadialSelectInput) or 9 with an item in hand and AllowWeaponsCycle (0x269f800..0x269f87c); in state 4 the release of
// inventory input 8 (GetCurrWpnHolsterInput; the inventory inputs are player+0x128, input_inv.xml) calls UpdateWeaponSpecialMove(2) +
// PutAwayItem unless player+0x43e (set only by the wheel opener 0x29f9934) or LinearWeaponQuickPick+0x6d4 is set (0x269f774..0x269f798):
// the stock tap holsters, the hold opens the wheel. With the wheel blocked +0x43e stays 0 and every such release would put the VR-held
// item away. The holster input shares the wheel's button (the +0x43e test only makes sense that way), so while the lever blocks, LB is
// dropped from the pad in gameplay contexts (World, a scene camera or its transition, the optic: presentation 1, 4, 5, 6) whether or not
// the hook is in; menus and cutscenes keep it (PREVIOUS_TAB), and so does the lasso while its rope is busy (its yank presses LB).
// Lever debug.rdrvr.vr.wheel (read every 2 s): 0 = stock wheel and LB, 1 (default) = blocked while the HOLSTERS row is on, 2 = always blocked.
// Report vr.wheel.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#if defined(_MSC_VER)&&!defined(__clang__)
#define RDRVR_WHEEL_NOINLINE __declspec(noinline)
#else
#define RDRVR_WHEEL_NOINLINE __attribute__((noinline))
#endif

namespace rdrvr::vr::wheel {

struct Print {std::uintptr_t offset;std::uint32_t size;std::uint64_t fnv;};
inline constexpr std::uintptr_t canQuickPickOffset=0x26a0088,canQuickPickGotOffset=0x2e4f8b8;
// CanQuickPickActivate; its PLT stub; HandleInputs gate site and SHOW_HIDE / RELEASED branch; Update's 0.2 s opener; the four
// quick-pick call sites.
inline constexpr Print prints[]{{canQuickPickOffset,0xf0,0xf5ea4540a5f00b1dull},{0x2c8e4b0,0x10,0x260ee28f0a3df4ebull},
    {0x29f9a30,0x20,0x4bec7540d141360aull},{0x29f9a68,0x50,0xf4aee284f05c2d7eull},{0x29f9878,0x30,0x464a2addfea19f05ull},
    {0x2578168,0x20,0x35bf3636d9dd114dull},{0x2578344,0x38,0x63db8eff153ba3a6ull},{0x25774a0,0x20,0x801b8ff95954a4c0ull},
    {0x25776c0,0x38,0x6e0d11d381d694e8ull}};
enum Site : unsigned {SiteHud,SiteRadialUpdate,SiteRadialDeactivate,SiteQuickUpdate,SiteQuickDeactivate,SiteCount};
inline constexpr std::uintptr_t siteReturns[SiteCount]{0x29f9a3c,0x2578180,0x2578360,0x25774b4,0x25776dc};
inline constexpr const char* siteNames[SiteCount]{"hud","radialUpdate","radialDeactivate","quickUpdate","quickDeactivate"};

// ---- decision (pure) ----
inline constexpr bool blocks(int lever,bool holsters) noexcept {return lever==2||(lever==1&&holsters);}
inline constexpr int siteOf(std::uintptr_t returnAddress,std::uintptr_t base) noexcept {
    if(!base||returnAddress<base)return -1;
    const auto off=returnAddress-base;
    for(unsigned i=0;i<SiteCount;++i)if(off==siteReturns[i])return int(i);
    return -1;
}

using CanQuickPickFn=bool(*)(const void* player);
inline CanQuickPickFn originalCanQuickPick{};
inline std::atomic<std::uintptr_t> engineBase{};
inline std::atomic<bool> hooked{};
inline std::atomic<const char*> installState{"NOT ATTEMPTED"};
inline std::atomic<int> lever{1};
inline std::atomic<bool> holstersOn{}; // HOLSTERS row (set by the holster pad filter every pad update)

struct SiteStats {std::atomic<std::uint64_t> calls{},blocked{},passed{},lastNs{};};
inline std::array<SiteStats,SiteCount> sites{};
inline std::atomic<std::uint64_t> otherCalls{},otherLast{};
// A call from the Radial Update site while blocking = that wheel was open and focused and is being closed (UIObject::Exit).
inline std::atomic<std::uint64_t> radialOpenSeen{},radialOpenLastNs{};
using ClockFn=std::uint64_t(*)() noexcept;
inline std::atomic<ClockFn> clockFn{};

// The hook body, testable without the engine: returnAddress / base as the engine call sees them.
inline bool decide(const void* player,std::uintptr_t returnAddress,std::uintptr_t base,CanQuickPickFn original,std::uint64_t now) noexcept {
    const int site=siteOf(returnAddress,base);
    const bool block=blocks(lever.load(std::memory_order_relaxed),holstersOn.load(std::memory_order_relaxed));
    if(site<0){otherCalls.fetch_add(1,std::memory_order_relaxed);otherLast.store(base&&returnAddress>=base?returnAddress-base:returnAddress,std::memory_order_relaxed);
        return original?original(player):false;}
    auto& st=sites[unsigned(site)];st.calls.fetch_add(1,std::memory_order_relaxed);st.lastNs.store(now,std::memory_order_relaxed);
    if(block){st.blocked.fetch_add(1,std::memory_order_relaxed);
        if(site==int(SiteRadialUpdate)){radialOpenSeen.fetch_add(1,std::memory_order_relaxed);radialOpenLastNs.store(now,std::memory_order_relaxed);}
        return false;}
    st.passed.fetch_add(1,std::memory_order_relaxed);
    return original?original(player):false;
}
RDRVR_WHEEL_NOINLINE inline bool canQuickPickHook(const void* player) noexcept {
    const auto lr=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const auto c=clockFn.load(std::memory_order_relaxed);
    return decide(player,lr,engineBase.load(std::memory_order_relaxed),originalCanQuickPick,c?c():0);
}

// Startup: nothing is hooked before every print and the GOT word checked out. read(ctx,address,out,size) = TraceEntry readMemory,
// got(slot,target) = word check, replace(address,expected,replacement) = TraceEntry replacePointer.
template<class Read,class Got,class Replace>
inline bool install(Read&& read,std::uintptr_t base,bool killed,Got&& got,Replace&& replace) noexcept {
    if(killed){installState.store("OFF");return false;}
    const auto hash=[&](std::uintptr_t address,std::uint32_t size){
        std::uint64_t h=0xcbf29ce484222325ull;unsigned char chunk[256];
        for(std::uint32_t done=0;done<size;){const std::uint32_t n=std::min<std::uint32_t>(size-done,sizeof chunk);
            if(!read(nullptr,address+done,chunk,n))return std::uint64_t{};
            for(std::uint32_t i=0;i<n;++i)h=(h^chunk[i])*0x100000001b3ull;
            done+=n;}
        return h;};
    for(const auto& p:prints)if(hash(base+p.offset,p.size)!=p.fnv){installState.store("ENGINE MISMATCH");return false;}
    if(!got(canQuickPickGotOffset,canQuickPickOffset)){installState.store("WORD MISMATCH");return false;}
    originalCanQuickPick=reinterpret_cast<CanQuickPickFn>(base+canQuickPickOffset);
    engineBase.store(base,std::memory_order_relaxed);
    if(!replace(base+canQuickPickGotOffset,base+canQuickPickOffset,reinterpret_cast<std::uintptr_t>(&canQuickPickHook))){
        installState.store("HOOK NOT INSTALLED");return false;}
    hooked.store(true,std::memory_order_release);installState.store("INSTALLED");
    return true;
}

// ---- pad side (native input thread): LB holds as the game receives them, and the fallback mask ----
// Context of the holster pad filter for this update, read by notePad: why LB could reach the game.
enum PadContext : std::uint8_t {CtxUnknown,CtxWorld,CtxNoInput,CtxNotWorld,CtxHolstersOff,CtxCount};
inline constexpr const char* ctxNames[CtxCount]{"unknown","world","noInput","notWorld","holstersOff"};
inline std::atomic<std::uint8_t> padContext{CtxUnknown};
inline std::atomic<int> presentation{-1}; // runtime::PresentationContext of the update, -1 = unknown
struct LbHold {std::uint64_t startNs{},durNs{};std::uint8_t ctx{};std::int8_t presentation{-1};bool ready{};};
inline constexpr std::uint64_t wheelOpenNs=200'000'000ull; // rdrHudDefault::Update threshold 0x3e4ccccd
// lbReleaseNs: the last LB release the game received (input thread), read by noteEnginePutAway (main thread).
inline std::atomic<std::uint64_t> lbReleaseNs{},enginePutAways{},putAwayAfterLb{};
inline constexpr std::uint64_t putAwayAfterLbNs=300'000'000ull;
struct PadTrack {
    std::array<LbHold,16> ring{};unsigned next{};std::uint64_t total{};
    bool down{};LbHold cur{};
    std::uint64_t framesInWorld{},holdsOver200{},dropsHooked{},dropsUnhooked{};
    void note(std::uint64_t now,bool lb,bool ready,std::uint8_t ctx,int pres) noexcept {
        if(lb&&ready&&ctx==CtxWorld)++framesInWorld;
        if(lb&&!down){down=true;cur=LbHold{};cur.startNs=now;cur.ctx=ctx;cur.presentation=std::int8_t(pres);cur.ready=ready;}
        else if(!lb&&down){down=false;cur.durNs=now>=cur.startNs?now-cur.startNs:0;if(cur.durNs>wheelOpenNs)++holdsOver200;
            lbReleaseNs.store(now?now:1,std::memory_order_relaxed);
            ring[next]=cur;next=(next+1u)%unsigned(ring.size());++total;}
    }
};
inline PadTrack padTrack{};
inline std::uint64_t leverReadNs{};
inline void readLever(std::uint64_t now) noexcept {
    if(leverReadNs&&now>=leverReadNs&&now-leverReadNs<2'000'000'000ull)return;
    leverReadNs=now?now:1;
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX]{};
    if(__system_property_get("debug.rdrvr.vr.wheel",v)>0){const int x=std::atoi(v);lever.store(std::clamp(x,0,2),std::memory_order_relaxed);}
    else lever.store(1,std::memory_order_relaxed);
#endif
}
// NativePadDriver observer: the buttons exactly as submitted (bit 7 = PadLeftGrip = LB).
inline constexpr std::uint32_t padLeftGripBit=1u<<7;
inline void notePad(std::uint64_t now,std::uint32_t buttons,bool ready) noexcept {
    readLever(now);
    padTrack.note(now,(buttons&padLeftGripBit)!=0,ready,padContext.load(std::memory_order_relaxed),presentation.load(std::memory_order_relaxed));
}
// LB off the pad while the lever blocks, in gameplay contexts (World, a scene camera or its transition, the optic: presentation 1, 4,
// 5, 6); menus and cutscenes keep it (PREVIOUS_TAB). lassoOwns = the lasso rope is busy (its yank presses LB).
inline bool dropsLb(int leverValue,bool holsters,int pres,bool lassoOwns) noexcept {
    return blocks(leverValue,holsters)&&!lassoOwns&&(pres==1||pres==4||pres==5||pres==6);
}
// Main thread: the engine put the VR-held item away (VrHolsters engine-holster forensics). Counted against the last LB release the game
// received: a put-away within putAwayAfterLbNs of it is the select-state-4 holster path.
inline void noteEnginePutAway(std::uint64_t now) noexcept {
    enginePutAways.fetch_add(1,std::memory_order_relaxed);
    const auto r=lbReleaseNs.load(std::memory_order_relaxed);
    if(r&&now>=r&&now-r<=putAwayAfterLbNs)putAwayAfterLb.fetch_add(1,std::memory_order_relaxed);
}

// Report object vr.wheel. hud.blocked > 0 = SHOW_HIDE was refused at the opener; radialOpenSeen > 0 = a wheel was open while
// blocking (closed by the engine on that frame); lb = LB holds the game received (durMs over 200 can open a stock wheel), drops = pad
// updates LB was taken off (hook installed / not); putAways = engine put-aways of the VR-held item, afterLb = within 300 ms of an LB release.
inline std::string report() {
    const auto u=[](const std::atomic<std::uint64_t>& v){return (unsigned long long)v.load(std::memory_order_relaxed);};
    const auto c=clockFn.load(std::memory_order_relaxed);const std::uint64_t now=c?c():0;
    const auto age=[&](std::uint64_t ns){return ns&&now>=ns?double(now-ns)/1e6:-1.0;};
    std::string o;char b[512];
    std::snprintf(b,sizeof b,"{\"state\":\"%s\",\"lever\":%d,\"holsters\":%s,\"blocking\":%s,\"sites\":{",installState.load(),lever.load(),
        holstersOn.load()?"true":"false",blocks(lever.load(),holstersOn.load())?"true":"false");o+=b;
    for(unsigned i=0;i<SiteCount;++i){const auto& s=sites[i];
        std::snprintf(b,sizeof b,"%s\"%s\":{\"calls\":%llu,\"blocked\":%llu,\"passed\":%llu,\"lastAgeMs\":%.0f}",i?",":"",siteNames[i],u(s.calls),u(s.blocked),u(s.passed),age(s.lastNs.load()));o+=b;}
    std::snprintf(b,sizeof b,"},\"other\":{\"calls\":%llu,\"lastOffset\":\"0x%llx\"},\"radialOpenSeen\":%llu,\"radialOpenAgeMs\":%.0f,",
        u(otherCalls),u(otherLast),u(radialOpenSeen),age(radialOpenLastNs.load()));o+=b;
    const auto& t=padTrack;
    std::snprintf(b,sizeof b,"\"putAways\":{\"engine\":%llu,\"afterLb\":%llu},",u(enginePutAways),u(putAwayAfterLb));o+=b;
    std::snprintf(b,sizeof b,"\"lb\":{\"holds\":%llu,\"framesInWorld\":%llu,\"holdsOver200ms\":%llu,\"drops\":{\"hooked\":%llu,\"unhooked\":%llu},\"down\":%s,\"ring\":[",
        (unsigned long long)t.total,(unsigned long long)t.framesInWorld,(unsigned long long)t.holdsOver200,(unsigned long long)t.dropsHooked,
        (unsigned long long)t.dropsUnhooked,t.down?"true":"false");o+=b;
    bool first=true;
    for(unsigned k=0;k<t.ring.size();++k){const auto& r=t.ring[(t.next+k)%t.ring.size()];if(!r.startNs)continue;
        std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"durMs\":%.0f,\"ctx\":\"%s\",\"presentation\":%d,\"ready\":%d}",first?"":",",age(r.startNs),double(r.durNs)/1e6,
            ctxNames[r.ctx<CtxCount?r.ctx:0],int(r.presentation),r.ready?1:0);o+=b;first=false;}
    o+="]}}";
    return o;
}
} // namespace rdrvr::vr::wheel
