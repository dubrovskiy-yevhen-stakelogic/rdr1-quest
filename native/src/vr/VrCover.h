#pragma once
// AUTO COVER: stop the player from snapping into cover (crates, walls) in VR.
//
// Why it happens in VR (librdr.so 1.58 arm64, vaddr; .text file offset = vaddr - 0x4000):
//  - The Touch right grip is a pad button: runtime::ControllerPolicy sets PadRightGrip at squeeze >= 0.55, NativePadEvents sends
//    Android key 103 (BUTTON_R1) -> AndroidGamepad_c::GamepadButtonEvent -> d11 pad button 15 (RB). The Android port's default
//    binding of FOOT action COVER is pad 15. Every grip squeeze that holsters/melee do not capture (a fist,
//    a missed holster grab, a two-hand grip off the foregrip) is a COVER press.
//  - sagPlayerInputFoot::ApplyInput (0x26b5100, vtable slot +0x30) reads foot input 5 = COVER (GetInput(5), 0x26b54fc / 0x26b5568;
//    the only GetInput(5) call sites of the foot class). A new press (ioValue (b0^b2)&0x80 down, (b0^b3)&0x80 up) with a cover
//    surface found by the mover (mover+0x990 normal != 0, FindWallsOrCover) sets mover+0xacc |= 2 (cover toggle, inline
//    0x26b5740) and calls aniCover::SetAttachRequest(true) (0x26b5754 -> PLT -> GOT 0x2e3c320; mover+0xacc |= 4); the action
//    tree's ConditionCoverAttachRequested / ConditionCoverToggle then run CoverAnimationTask -> Schedule*Attach -> cover.
//    A press with no surface yet sets foot+0x16c bit3 and attaches on the next frame if a surface appears (0x26b53dc..0x26b5430).
//  - Other routes: script sagActorScript::AttachPlayerToCover -> aniCover::SetScriptAttachRequest (aniCover+0xbc |= 0x60, left
//    native: missions), the action-tree task SetCoverAttachRequestTask::Begin (0x22a970c), aniMelee::SetMeleeMode (false only).
//
// Block (AUTO COVER OFF = settings().suppressAutoCover(); debug.rdrvr.vr.cover 0 = nothing installed, NATIVE):
//  1. INPUT: GOT hook of sagPlayerInputFoot::ApplyStickInputOnFoot (JUMP_SLOT 0x2e536f8; its only callers are the first calls of
//     ApplyInput 0x26b5144/0x26b518c/0x26b51e4, before any GetInput(5)). For the local player's foot input, while the player is
//     not in cover (mover+0xaca == 0, mover+0xacc bits 1|2 clear), the COVER ioValue (values[5], 16 bytes) is made "up, was up"
//     (b2 = b3 = b0) before ApplyInput reads it. ioMapper::Update (0x1b2f2ec) rolls b3 = b2, b2 = 0 and re-ORs the pad every
//     update, so nothing persists: NATIVE again on the next update. Only input 5 is touched: MELEE_BLOCK (input 14, same pad 15),
//     the grip itself, holsters and melee are unchanged. In cover (scripted) the input passes natively.
//  2. REQUEST: GOT hook of aniCover::SetAttachRequest (JUMP_SLOT 0x2e3c320). Counts every caller; drops a true request from the
//     action-tree task for the local player when it is not in cover and no script request is set (aniCover+0xbc bit5). The
//     ApplyInput call is never dropped (its inline toggle write would stay) - with (1) working it only fires for script requests;
//     a non-script one is counted as "leaked". Script / melee / AI requests always pass.
// Every engine function, call site, data layout, GOT word and vtable slot used is fingerprinted in install(); any mismatch
// installs nothing (row AUTO COVER " - UNAVAILABLE"). Report "vr":{"cover":...}.
#include "VrSettings.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rdrvr::vr::cover {

// ============================== engine facts (librdr.so 1.58, vaddr) ==============================
inline constexpr std::uintptr_t applyInputOffset=0x26b5100,        // sagPlayerInputFoot::ApplyInput()
    applyStickOffset=0x26b582c,applyStickGotOffset=0x2e536f8,      // sagPlayerInputFoot::ApplyStickInputOnFoot() (JUMP_SLOT, only)
    coverInputSiteOffset=0x26b54f0,                                // ApplyInput: GetInput(5) x2 + press-edge tests
    attachSiteOffset=0x26b5730,                                    // ApplyInput: SetAttachRequest(true) / GetAttachRequest / GetCoverToggle
    getInputOffset=0x26a7ce8,                                      // sagPlayerInput::GetInput(int): (*(this+0x20))[i], 16 bytes each
    ioRollOffset=0x1b2f2ec,ioWriteOffset=0x1b2f974,                // rage::ioMapper::Update: b3 = b2, b2 = 0; b2 = max(b0^b2, v)
    setAttachRequestOffset=0x2164528,setAttachRequestGotOffset=0x2e3c320, // aniCover::SetAttachRequest(bool) (JUMP_SLOT, only)
    attachTaskOffset=0x22a970c,                                    // SetCoverAttachRequestTask::Begin()
    scriptAttachOffset=0x2164688,                                  // aniCover::SetScriptAttachRequest(bool,bool): +0xbc |= 0x60
    meleeSiteOffset=0x1e428c4,                                     // aniMelee::SetMeleeMode: SetAttachRequest(false), SetCoverToggle(false)
    footVtableOffset=0x2d8e1d8,footVtableGotOffset=0x2e081a0,      // vtable for sagPlayerInputFoot (+0x10 address point)
    footApplyInputSlotOffset=0x2d8e218,footGetInputSlotOffset=0x2d8e248; // vtable +0x10+0x30 ApplyInput, +0x10+0x60 GetInput (ABS64)
inline constexpr std::uintptr_t vtableAddressPoint=0x10;
// Return addresses pinned by the hooks.
inline constexpr std::array<std::uintptr_t,3> stickReturns{0x26b5148,0x26b5190,0x26b51e8};
inline constexpr std::uintptr_t attachReturnApplyInput=0x26b5758,attachReturnTask=0x22a973c,attachReturnScript=0x21646c4,
    attachReturnMelee=0x1e428d0;
namespace field {
inline constexpr std::size_t footValues=0x20,footPlayer=0x40,footBlock=0x48;           // ApplyInput 0x26b5528, GetInput 0x26a7ce8
inline constexpr std::size_t pmPlayer=0xa20,playerFoot=0x118,playerActive=0x5f1,playerGuid=0x5f4,playerBlock=0x5f8; // GetPlayerGuid 0x2697cd8
inline constexpr std::size_t actorMover=0x88,actorAnim=0x38,animBlock=0xab8,blockCover=0x18;  // ApplyInput 0x26b52f4 / 0x26b5460..0x26b54f4
inline constexpr std::size_t moverInCover=0xaca,moverCoverFlags=0xacc;                 // u8 (loco COVER gate) / u16 (bit1 toggle, bit2 request)
inline constexpr std::size_t coverOwner=0x8,ownerActor=0x10,coverScript=0xbc;          // SetAttachRequest 0x216454c; script bits 0x21646e0
inline constexpr unsigned coverInput=5,ioValueSize=16;
inline constexpr std::uint16_t flagToggle=0x2,flagRequest=0x4,scriptBit=0x20;
}
// Code prints (bytes at the vaddr; PC-relative branches/adrp are position independent).
inline constexpr std::array<unsigned char,48> applyStickFingerprint{
#include "gamecode/VrCover_26b582c_48_8d.inc"
};
inline constexpr std::array<unsigned char,236> applyInputHeadFingerprint{
#include "gamecode/VrCover_26b5100_236_8d.inc"
};
inline constexpr std::array<unsigned char,136> coverInputSiteFingerprint{
#include "gamecode/VrCover_26b54f0_136_8d.inc"
};
inline constexpr std::array<unsigned char,72> attachSiteFingerprint{
#include "gamecode/VrCover_26b5730_72_8d.inc"
};
inline constexpr std::array<unsigned char,16> getInputFingerprint{
#include "gamecode/VrCover_26a7ce8_16_8d.inc"
};
inline constexpr std::array<unsigned char,36> ioRollFingerprint{
#include "gamecode/VrCover_1b2f2ec_36_8d.inc"
};
inline constexpr std::array<unsigned char,52> ioWriteFingerprint{
#include "gamecode/VrCover_1b2f974_52_8d.inc"
};
inline constexpr std::array<unsigned char,56> setAttachRequestFingerprint{
#include "gamecode/VrCover_2164528_56_8d.inc"
};
inline constexpr std::array<unsigned char,60> attachTaskFingerprint{
#include "gamecode/VrCover_22a970c_60_8d.inc"
};
inline constexpr std::array<unsigned char,112> scriptAttachFingerprint{
#include "gamecode/VrCover_2164688_112_8d.inc"
};
inline constexpr std::array<unsigned char,24> meleeSiteFingerprint{
#include "gamecode/VrCover_1e428c4_24_8d.inc"
};

// ============================== pure logic (host-tested) ==============================
// rage::ioValue bytes: b0 = invert mask, b1 = update stamp, b2 = this update's raw value, b3 = previous update's raw value.
inline constexpr bool ioDown(const unsigned char* v) noexcept {return ((v[0]^v[2])&0x80u)!=0;}
inline constexpr bool ioWasDown(const unsigned char* v) noexcept {return ((v[0]^v[3])&0x80u)!=0;}
struct IoFilter {bool down{},newPress{},modified{};};
// Makes the value "up, was up" (no press, no release edge) in place. Only b2/b3 change.
inline IoFilter filterIoValue(unsigned char* v) noexcept {
    IoFilter r{};r.down=ioDown(v);r.newPress=r.down&&!ioWasDown(v);
    if(r.down||ioWasDown(v)){v[2]=v[0];v[3]=v[0];r.modified=true;}
    return r;
}
// The player is in cover or a cover request is pending (script, or one made before AUTO COVER OFF):
// the input passes natively, so leaving (scripted) cover is never affected.
inline constexpr bool coverBusy(std::uint8_t inCover,std::uint16_t flags) noexcept {
    return inCover!=0||(flags&(field::flagToggle|field::flagRequest))!=0;}

enum class Route : unsigned {ApplyInput,Task,Script,Melee,Other,Count};
inline constexpr std::array<const char*,unsigned(Route::Count)> routeNames{"applyInput","task","script","melee","other"};
inline constexpr Route classify(std::uintptr_t callerOffset) noexcept {
    return callerOffset==attachReturnApplyInput?Route::ApplyInput:callerOffset==attachReturnTask?Route::Task:
        callerOffset==attachReturnScript?Route::Script:callerOffset==attachReturnMelee?Route::Melee:Route::Other;}
enum class Verdict : unsigned {Pass,Block,Leak};
// on = the request value; local = the aniCover belongs to the local player; busy = coverBusy(); script = aniCover+0xbc bit5.
inline constexpr Verdict requestVerdict(bool suppress,bool on,Route route,bool local,bool busy,bool script) noexcept {
    if(!suppress||!on||!local||busy||script)return Verdict::Pass;
    if(route==Route::Task)return Verdict::Block;
    if(route==Route::ApplyInput)return Verdict::Leak;
    return Verdict::Pass;
}
// Cover entry bookkeeping (mover+0xaca rising edge), sampled once per ApplyInput.
struct EntryTracker {
    std::uint8_t last{};bool primed{};
    // returns true on a rising edge (0 -> in cover) after the first sample
    bool sample(std::uint8_t inCover) noexcept {const bool edge=primed&&!last&&inCover;last=inCover;primed=true;return edge;}
    void reset() noexcept {last=0;primed=false;}
};

// ============================== counters / report ==============================
inline std::atomic<const char*> installState{"NOT ATTEMPTED"},fingerprintState{"NOT CHECKED"};
inline std::atomic<bool> inputInstalled{},requestInstalled{};
inline std::atomic<std::uint64_t> frames{},wrongCaller{},notLocal{},readFailures{},busyFrames{},pressesBlocked{},framesBlocked{},
    pressesPassed{},requestCalls{},requestsBlocked{},requestsLeaked{},requestsPassed{},requestNotLocal{},inCoverFrames{},entries{},
    entriesWhileOff{},scriptEntries{},toggleFrames{},requestFrames{},lastBlockNs{};
inline std::array<std::atomic<std::uint64_t>,unsigned(Route::Count)> requestsTrue{};
inline std::atomic<std::uintptr_t> playerActorCache{};
inline std::uint64_t nowNs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}

inline std::string report() {
    std::string out;char b[1400];
    const bool suppress=::rdrvr::vr::settings().suppressAutoCover();
    const auto lb=lastBlockNs.load(),now=nowNs();
    const double ageMs=lb&&now>=lb?double(now-lb)/1e6:-1.0;
    const auto blocked=pressesBlocked.load()+requestsBlocked.load(),passed=pressesPassed.load()+requestsPassed.load();
    std::snprintf(b,sizeof b,"{\"installed\":%s,\"state\":\"%s\",\"fingerprint\":\"%s\",\"killed\":%s,\"mode\":\"%s\",\"blocked\":%llu,\"passed\":%llu,"
        "\"input\":{\"installed\":%s,\"frames\":%llu,\"wrongCaller\":%llu,\"notLocal\":%llu,\"readFailures\":%llu,\"busyFrames\":%llu,"
        "\"pressesBlocked\":%llu,\"framesBlocked\":%llu,\"pressesPassed\":%llu,\"lastBlockAgeMs\":%.0f},"
        "\"request\":{\"installed\":%s,\"calls\":%llu,\"notLocal\":%llu,\"blocked\":%llu,\"leaked\":%llu,\"passed\":%llu,\"true\":{",
        inputInstalled.load()?"true":"false",installState.load(),fingerprintState.load(),killed(Feature::Cover)?"true":"false",
        suppress?"OFF":"NATIVE",(unsigned long long)blocked,(unsigned long long)passed,
        inputInstalled.load()?"true":"false",(unsigned long long)frames.load(),(unsigned long long)wrongCaller.load(),
        (unsigned long long)notLocal.load(),(unsigned long long)readFailures.load(),(unsigned long long)busyFrames.load(),
        (unsigned long long)pressesBlocked.load(),(unsigned long long)framesBlocked.load(),(unsigned long long)pressesPassed.load(),ageMs,
        requestInstalled.load()?"true":"false",(unsigned long long)requestCalls.load(),(unsigned long long)requestNotLocal.load(),
        (unsigned long long)requestsBlocked.load(),(unsigned long long)requestsLeaked.load(),(unsigned long long)requestsPassed.load());
    out+=b;
    for(unsigned i=0;i<unsigned(Route::Count);++i){
        std::snprintf(b,sizeof b,"%s\"%s\":%llu",i?",":"",routeNames[i],(unsigned long long)requestsTrue[i].load());out+=b;}
    std::snprintf(b,sizeof b,"}},\"cover\":{\"inCoverFrames\":%llu,\"entries\":%llu,\"entriesWhileOff\":%llu,\"scriptEntries\":%llu,"
        "\"toggleFrames\":%llu,\"requestFrames\":%llu}}",
        (unsigned long long)inCoverFrames.load(),(unsigned long long)entries.load(),(unsigned long long)entriesWhileOff.load(),
        (unsigned long long)scriptEntries.load(),(unsigned long long)toggleFrames.load(),(unsigned long long)requestFrames.load());
    out+=b;
    return out;
}

#if defined(__ANDROID__)
// ============================== engine side (Android only) ==============================
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using ApplyStickFn=void(*)(void* foot);
using SetAttachFn=void(*)(void* cover,bool on);
struct EngineApi {ReadFn read{};std::uintptr_t base{},playerManager{},playerManagerTable{},playerTable{},actorSlots{};};
namespace detail {
inline EngineApi api{};
inline std::atomic<ApplyStickFn> stickOriginal{};inline std::atomic<SetAttachFn> attachOriginal{};
inline EntryTracker tracker{}; // game thread (ApplyInput) only
// Physical press edge of input 5 (game thread). The engine's own b3 cannot be used while filtering:
// b3 = the filtered b2 = up.
inline bool rawDown{};
template<class T> inline bool get(std::uintptr_t p,std::size_t o,T& out) noexcept {
    return api.read&&p&&p<=UINTPTR_MAX-o&&p+o<=UINTPTR_MAX-sizeof(T)&&api.read(nullptr,p+o,&out,sizeof(T));}
// PLAYERMGR player (vtable-checked, active) -> its actor (actor slot table indexed by the player GUID, GUID cross-checked).
inline bool localPlayer(std::uintptr_t& player,std::uintptr_t& actor) noexcept {
    std::uintptr_t table{},vt{},slots{};std::uint8_t active{};std::uint32_t guid{},actorGuid{};
    if(!get(api.playerManager,0,table)||table!=api.playerManagerTable||!get(api.playerManager,field::pmPlayer,player)||!player||
       !get(player,0,vt)||vt!=api.playerTable||!get(player,field::playerActive,active)||active!=1||!get(player,field::playerGuid,guid))return false;
    if(!get(api.actorSlots,0,slots)||!slots||!get(slots+std::uintptr_t(guid&0xffffu)*16u,0,actor)||!actor||
       !get(actor,0x8,actorGuid)||actorGuid!=guid)return false;
    return true;
}
inline bool moverState(std::uintptr_t actor,std::uint8_t& inCover,std::uint16_t& flags) noexcept {
    std::uintptr_t mover{};return get(actor,field::actorMover,mover)&&mover&&get(mover,field::moverInCover,inCover)&&get(mover,field::moverCoverFlags,flags);}
inline bool scriptRequested(std::uintptr_t cover) noexcept {std::uint16_t bits{};return get(cover,field::coverScript,bits)&&(bits&field::scriptBit);}
inline std::uintptr_t coverOf(std::uintptr_t actor) noexcept {
    std::uintptr_t anim{},block{},cover{};
    return get(actor,field::actorAnim,anim)&&anim&&get(anim,field::animBlock,block)&&block&&get(block,field::blockCover,cover)?cover:0;}
// Runs at the start of every ApplyInput, before its GetInput(5) reads (game thread).
inline void beforeApplyInput(std::uintptr_t foot,std::uintptr_t caller) noexcept {
    const auto b=api.base;
    if(caller!=b+stickReturns[0]&&caller!=b+stickReturns[1]&&caller!=b+stickReturns[2]){wrongCaller.fetch_add(1,std::memory_order_relaxed);return;}
    frames.fetch_add(1,std::memory_order_relaxed);
    std::array<std::uintptr_t,field::footBlock/8> f{};std::uintptr_t player{},actor{},playerFoot{};
    if(!api.read||!api.read(nullptr,foot,f.data(),sizeof f)){readFailures.fetch_add(1,std::memory_order_relaxed);return;}
    if(f[0]!=b+footVtableOffset+vtableAddressPoint||!localPlayer(player,actor)||f[field::footPlayer/8]!=player||
       !get(player,field::playerFoot,playerFoot)||playerFoot!=foot){notLocal.fetch_add(1,std::memory_order_relaxed);return;}
    playerActorCache.store(actor,std::memory_order_relaxed);
    std::uint8_t inCover{};std::uint16_t flags{};
    if(!moverState(actor,inCover,flags)){readFailures.fetch_add(1,std::memory_order_relaxed);return;}
    const bool suppress=::rdrvr::vr::settings().suppressAutoCover();
    if(inCover)inCoverFrames.fetch_add(1,std::memory_order_relaxed);
    if(flags&field::flagToggle)toggleFrames.fetch_add(1,std::memory_order_relaxed);
    if(flags&field::flagRequest)requestFrames.fetch_add(1,std::memory_order_relaxed);
    if(tracker.sample(inCover)){
        entries.fetch_add(1,std::memory_order_relaxed);
        const auto cover=coverOf(actor);
        if(cover&&scriptRequested(cover))scriptEntries.fetch_add(1,std::memory_order_relaxed);
        else if(suppress)entriesWhileOff.fetch_add(1,std::memory_order_relaxed);
    }
    std::uintptr_t values{};unsigned char v[4]{};
    const auto holder=f[field::footValues/8];
    if(!holder||!get(holder,0,values)||!values){readFailures.fetch_add(1,std::memory_order_relaxed);return;}
    const auto record=values+std::uintptr_t(field::coverInput)*field::ioValueSize;
    if(!api.read(nullptr,record,v,sizeof v)){readFailures.fetch_add(1,std::memory_order_relaxed);return;}
    const bool fresh=ioDown(v)&&!rawDown;rawDown=ioDown(v);
    if(!suppress||coverBusy(inCover,flags)){
        if(coverBusy(inCover,flags))busyFrames.fetch_add(1,std::memory_order_relaxed);
        if(fresh)pressesPassed.fetch_add(1,std::memory_order_relaxed);
        return;
    }
    unsigned char w[4]{v[0],v[1],v[2],v[3]};
    const auto r=filterIoValue(w);
    if(!r.modified)return;
    // The engine owns this memory and reads it a few instructions later on this thread (GetInput(5)): plain byte stores.
    auto* p=reinterpret_cast<volatile unsigned char*>(record);p[2]=w[2];p[3]=w[3];
    framesBlocked.fetch_add(1,std::memory_order_relaxed);
    if(fresh){pressesBlocked.fetch_add(1,std::memory_order_relaxed);lastBlockNs.store(nowNs(),std::memory_order_relaxed);}
}
} // namespace detail

__attribute__((noinline)) inline void applyStickHook(void* foot) noexcept {
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    detail::beforeApplyInput(reinterpret_cast<std::uintptr_t>(foot),caller);
    detail::stickOriginal.load(std::memory_order_acquire)(foot);
}
__attribute__((noinline)) inline void setAttachRequestHook(void* cover,bool on) noexcept {
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const auto original=detail::attachOriginal.load(std::memory_order_acquire);
    requestCalls.fetch_add(1,std::memory_order_relaxed);
    if(!on){original(cover,on);return;}
    const auto route=classify(caller-detail::api.base);
    requestsTrue[unsigned(route)].fetch_add(1,std::memory_order_relaxed);
    const bool suppress=::rdrvr::vr::settings().suppressAutoCover();
    bool local=false,busy=false,script=false;
    if(suppress&&(route==Route::Task||route==Route::ApplyInput)){
        const auto c=reinterpret_cast<std::uintptr_t>(cover);std::uintptr_t owner{},actor{};
        const auto player=playerActorCache.load(std::memory_order_relaxed);
        local=player&&detail::get(c,field::coverOwner,owner)&&owner&&detail::get(owner,field::ownerActor,actor)&&actor==player;
        if(!local)requestNotLocal.fetch_add(1,std::memory_order_relaxed);
        std::uint8_t inCover{};std::uint16_t flags{};
        // ApplyInput has already set the toggle bit when it calls with true: only mover+0xaca (in cover) counts as busy there.
        if(local&&detail::moverState(actor,inCover,flags))busy=route==Route::ApplyInput?inCover!=0:coverBusy(inCover,flags);
        script=local&&detail::scriptRequested(c);
    }
    switch(requestVerdict(suppress,on,route,local,busy,script)){
        case Verdict::Block:requestsBlocked.fetch_add(1,std::memory_order_relaxed);lastBlockNs.store(nowNs(),std::memory_order_relaxed);return;
        case Verdict::Leak:requestsLeaked.fetch_add(1,std::memory_order_relaxed);break;
        default:requestsPassed.fetch_add(1,std::memory_order_relaxed);break;
    }
    original(cover,on);
}

// code(offset, fingerprint) = executable bytes match; got(slot, target) = *(base+slot) == base+target (GOT words, relocated vtable
// slots); replace(address, expected, replacement) = TraceEntry replacePointer. Fails closed: any mismatch -> nothing installed.
template<class Code,class Got,class Replace>
inline bool install(const EngineApi& api,Code&& code,Got&& got,Replace&& replace) noexcept {
    using namespace ::rdrvr::vr;
    const auto fail=[&](const char* state,const char* print){installState.store(state);if(print)fingerprintState.store(print);
        status::publish(Feature::Cover,status::Unavailable);return false;};
    if(killed(Feature::Cover)){installState.store("KILLED");status::publish(Feature::Cover,status::Property);return false;}
    if(!api.read||!api.base||!api.playerManager||!api.playerManagerTable||!api.playerTable||!api.actorSlots)return fail("NO ENGINE API",nullptr);
    struct Print {std::uintptr_t offset;bool ok;const char* name;};
    const Print prints[]{{applyStickOffset,code(applyStickOffset,applyStickFingerprint),"ApplyStickInputOnFoot"},
        {applyInputOffset,code(applyInputOffset,applyInputHeadFingerprint),"ApplyInput head"},
        {coverInputSiteOffset,code(coverInputSiteOffset,coverInputSiteFingerprint),"ApplyInput GetInput(5)"},
        {attachSiteOffset,code(attachSiteOffset,attachSiteFingerprint),"ApplyInput attach site"},
        {getInputOffset,code(getInputOffset,getInputFingerprint),"GetInput"},
        {ioRollOffset,code(ioRollOffset,ioRollFingerprint),"ioMapper roll"},{ioWriteOffset,code(ioWriteOffset,ioWriteFingerprint),"ioMapper write"},
        {setAttachRequestOffset,code(setAttachRequestOffset,setAttachRequestFingerprint),"SetAttachRequest"},
        {attachTaskOffset,code(attachTaskOffset,attachTaskFingerprint),"SetCoverAttachRequestTask"},
        {scriptAttachOffset,code(scriptAttachOffset,scriptAttachFingerprint),"SetScriptAttachRequest"},
        {meleeSiteOffset,code(meleeSiteOffset,meleeSiteFingerprint),"SetMeleeMode site"}};
    for(const auto& p:prints)if(!p.ok)return fail("ENGINE MISMATCH",p.name);
    struct Word {std::uintptr_t slot,target;const char* name;};
    const Word words[]{{applyStickGotOffset,applyStickOffset,"GOT ApplyStickInputOnFoot"},
        {setAttachRequestGotOffset,setAttachRequestOffset,"GOT SetAttachRequest"},{footVtableGotOffset,footVtableOffset,"GOT foot vtable"},
        {footApplyInputSlotOffset,applyInputOffset,"VTABLE ApplyInput"},{footGetInputSlotOffset,getInputOffset,"VTABLE GetInput"}};
    for(const auto& w:words)if(!got(w.slot,w.target))return fail("ENGINE MISMATCH",w.name);
    fingerprintState.store("OK");
    detail::api=api;const auto b=api.base;
    detail::stickOriginal.store(reinterpret_cast<ApplyStickFn>(b+applyStickOffset),std::memory_order_release);
    detail::attachOriginal.store(reinterpret_cast<SetAttachFn>(b+setAttachRequestOffset),std::memory_order_release);
    if(!replace(b+applyStickGotOffset,b+applyStickOffset,reinterpret_cast<std::uintptr_t>(&applyStickHook)))
        return fail("HOOK NOT INSTALLED",nullptr);
    inputInstalled.store(true,std::memory_order_release);
    const bool request=replace(b+setAttachRequestGotOffset,b+setAttachRequestOffset,reinterpret_cast<std::uintptr_t>(&setAttachRequestHook));
    requestInstalled.store(request,std::memory_order_release);
    installState.store(request?"INSTALLED":"INSTALLED INPUT ONLY");
    status::publish(Feature::Cover,status::Ready,nullptr);
    return true;
}
#endif // __ANDROID__
} // namespace rdrvr::vr::cover
