#pragma once
// NPC AIM: NPCs react to a VR-held firearm aimed at them.
//
// Stock path (librdr.so 1.58 arm64, vaddr):
//  sagPlayer::UpdateInput (main thread, every frame) 0x2698964: aiObjectInterfaceActorSAG::WeaponGetAimTarget(out,true,false) (vtable slot +0xe0,
//  0x1e5e1c8). For a drawn weapon (type+0x408 bit2, aniAim+0x3bc >= 5) it returns tgtReticleComponent::GetBestTarget(true,false)
//  (0x26ca84c), else aniAim::GetTargetGuidIfAiming. No LT, TARGET input or force-aim bit is read on this path.
//  Gate 0x2698b1c: outside melee the report needs sagPlayerStates+0x10 (reticle fade alpha) > 0 or an active SniperScopeManager.
//  Then 0x2698b74: aiMemory::ReportAimingAtMe on the target (actor+0x28 AI, +0x120 memory; skipped when AI+0x158a bit1 is set), ForceUpdateOnActor,
//  ProcessTargetedObserversFor (bystanders), AttentionReasoner::SetReactToWeaponTarget, PlayerThreatenSpeech, and player+0x110 = the target's
//  sagGuid (cleared at the top of UpdateInput 0x26981b0). AI side: aiMemory::UpdateAimerList 0x20214f0 (every aimer entry) and
//  aiPerceptionModuleVision::UpdateMemoryOfSeenObject 0x2025970 ask WeaponGetAimTarget(out,true,true) of the aimer.
//
// Why it does not fire with a VR-held firearm:
//  1. vrNativeReticle sets prevention bit 0x40000000 (sagPlayerStates+0xc), EvaluateReticleDraw fades alpha to 0, the gate above returns.
//  2. The reticle's target (+0x3900 / +0x3914) comes from a probe along the native camera (tgtReticleComponent::UpdateTargetData 0x26c3adc), not the barrel.
//  3. Script side: native 0x86baac6c (rdr2TargetingScript::GetActorUnderReticle, handler 0x26ccae0, reads +0x3900 / +0x3914) stays on that camera probe. It is in 223 of the
//     decoded scripts. A helper shared by 143 of them (0xaf12 in script 0180) returns reason 4 after a 1.3 s timer when, within 20 m: under reticle == the actor,
//     states+0x14 != 0 (native 0xa842786) and aiScript::MemoryGetIsVisible.
//
// Fix: GOT hook of GetBestTarget (JUMP_SLOT 0x2e47ef0). Its only call sites are WeaponGetAimTarget 0x1e5e2ac (soft = true) and
// sagPlayer::UpdateZoomState 0x269be18 (soft = false, stock here). So the override reaches the AI report and AI queries only; the reticle
// fields read by aniAim::RequestFire / GunUpCheck, SetupTargetPacket and the HUD stay unchanged.
//  - tick() (GameXr vrNpcAim, main thread, right after the reticle Update) probes the barrel line with the reticle's own query at 10 Hz, up to
//    the weapon's reticle range (+0x3860). Hit instance -> actor as UpdateTargetData 0x26c3b98: [inst+0x18] = user, [user+0x18] == 0, [user+8] = actor; goh guid = actor+0x188.
//    An actor must stay on the barrel for dwellMs before it is published, and stays published for holdNs after the last hit.
//  - The hook returns the published guid for this reticle (soft calls). For the UpdateInput call (multi = false, just before the gate) it
//    raises alpha to 0.001 when lower; EvaluateReticleDraw fades it back in the same frame, before sagPlayer::UpdateHudReticles reads it.
//  - Native 0x86baac6c: its handler in the engine's native table (scrThread::RegisterCommand 0x1be2940, table pointer 0x4e6eb898, size 0x4e6eb8a0) is replaced by
//    underReticle(), which runs the stock handler and returns the published guid when asked about the player. scrThread::Validate copies handlers into a script
//    program when it loads, so a program loaded before the replacement keeps the stock handler.
// Gates: VR-held firearm whose shots use the barrel (owner aimAllowed: no cover, blind fire, vehicle, linked animation, control disabled), no duel.
// Any print or word mismatch installs nothing: stock behaviour.
// debug.rdrvr.vr.npcaim 0 = stock (read every 2 s), debug.rdrvr.vr.npcaim.script 0 = stock result of the native, debug.rdrvr.vr.npcaim.dwell <ms> (default 300). Report vr.npcAim.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/system_properties.h>

namespace rdrvr::vr::npcaim {

struct Print {std::uintptr_t offset;std::uint32_t size;std::uint64_t fnv;};
inline constexpr std::uintptr_t getBestTargetOffset=0x26ca84c,getBestTargetGotOffset=0x2e47ef0,
    aiVtableSlotOffset=0x2d080e8+0x10+0xe0,weaponGetAimTargetOffset=0x1e5e1c8;
// GetBestTarget + its PLT stub; WeaponGetAimTarget; UpdateInput (top, report section); the other callers; instance -> actor; alpha fade; range fields; aimer list layout.
inline constexpr Print prints[]{{getBestTargetOffset,0x130,0x293219ab28a1560full},{0x2c7f120,0x10,0xfae78fc48da56995ull},
    {weaponGetAimTargetOffset,0x1dc,0x689191499fb6098eull},{0x2698178,0x40,0x2f0b8b2b0699e463ull},{0x2698964,0x2cc,0x344e0cc03b76e2c9ull},
    {0x269be0c,0x10,0xb44a7a37960536a9ull},{0x20215d0,0x20,0xd2d27291a59a8b40ull},{0x2025970,0x2c,0x6ca59c0af2e765e3ull},
    {0x26c3b98,0x20,0x0c3149742c35d72aull},{0x26c3f94,0x8,0xe7e3120a7a9c05f3ull},{0x26abf84,0x74,0xd4dfda9983ad26ffull},
    {0x26c13c0,0x48,0x38862424d5822346ull},{0x202257c,0x48,0x7f19a2e835c91855ull}};
inline constexpr std::uintptr_t underReticleOffset=0x26ccae0,nativeTableOffset=0x4e6eb898,nativeTableSizeOffset=0x4e6eb8a0,getActorFromGuidOffset=0x2467798;
inline constexpr std::uint32_t underReticleHash=0x86baac6cu,nativeEntryStride=16,nativeEntryHandler=8;
// The native's handler and its registration; RegisterCommand (table layout, probe order); the table lookup of scrThread::Validate; gohActor::GetActorFromGuid.
inline constexpr Print scriptPrints[]{{underReticleOffset,0x68,0x4e6722579758c3c1ull},{0x26cc974,0x14,0x0d140754dc086320ull},
    {0x1be2940,0x14c,0x2f23916f3c7201a1ull},{0x1be1ff4,0x7c,0x2c5f1b7fa00c00aaull},{getActorFromGuidOffset,0x9c,0x3c680a0175e129a1ull}};
namespace field {
inline constexpr std::size_t playerAimedActor=0x110,playerStates=0x410,statesAlpha=0x10,statesZoom=0x14,reticleRange=0x3860,infoResult=0,infoArgs=0x10,
    instUser=0x18,userActor=0x8,userKind=0x18,actorGoh=0x188,actorAi=0x28,aiFlags=0x158a,aiMemoryAimers=0x120+0x5bc;
}
inline constexpr std::uint64_t castIntervalNs=100'000'000,holdNs=400'000'000;
inline constexpr float alphaEpsilon=0.001f,fallbackRangeM=60.f;

using GetBestTargetFn=std::uint32_t(*)(const void* reticle,bool soft,bool multi);
inline GetBestTargetFn originalGetBestTarget{};
inline std::atomic<bool> hooked{},leverOn{true};
inline std::atomic<const char*> installState{"NOT ATTEMPTED"};
inline std::atomic<int> dwellMs{300};
// Written by tick() on the main thread. The hook can run on AI threads and reads only these.
inline std::atomic<const void*> publishedReticle{};
inline std::atomic<std::uintptr_t> publishedStates{};
inline std::atomic<std::uint32_t> publishedTarget{},publishedPlayer{};
// UpdateInput calls since the last tick(): the override stops when the reticle boundary stopped running (no Update, no tick).
inline std::atomic<std::uint32_t> reportsSinceTick{};
inline constexpr std::uint32_t maxReportsPerTick=5;
// Counters and last values of report().
inline std::atomic<std::uint64_t> casts{},castFails{},castNsSum{},castNsMax{},hits{},actorHits{},targetsPublished{},targetFrames{},noReticleRange{},
    hookCalls{},hookReportCalls{},hookReportOverrides{},hookAimerOverrides{},alphaWrites{},
    stimulusFrames{},stimulusTargets{},targetNoAi{},targetAiIgnored{},zoomFrames{},scriptCalls{},scriptOverrides{};
inline std::atomic<std::uint32_t> lastTarget{},lastStimulus{};
inline std::atomic<std::int32_t> lastAimers{-1},maxAimers{-1};
inline std::atomic<float> lastDistanceM{-1.f},lastRangeM{-1.f};

inline std::uint32_t getBestTarget(const void* reticle,bool soft,bool multi) noexcept {
    hookCalls.fetch_add(1,std::memory_order_relaxed);
    if(soft){
        if(!multi)hookReportCalls.fetch_add(1,std::memory_order_relaxed);
        const auto target=publishedTarget.load(std::memory_order_acquire);
        if(target&&reticle==publishedReticle.load(std::memory_order_relaxed)&&reportsSinceTick.load(std::memory_order_relaxed)<maxReportsPerTick){
            if(!multi){
                reportsSinceTick.fetch_add(1,std::memory_order_relaxed);
                hookReportOverrides.fetch_add(1,std::memory_order_relaxed);
                if(const auto states=publishedStates.load(std::memory_order_relaxed)){
                    auto* word=reinterpret_cast<std::uint32_t*>(states+field::statesAlpha);
                    float alpha{};const auto bits=__atomic_load_n(word,__ATOMIC_RELAXED);std::memcpy(&alpha,&bits,sizeof alpha);
                    if(!(alpha>=alphaEpsilon)){std::uint32_t next{};std::memcpy(&next,&alphaEpsilon,sizeof next);
                        __atomic_store_n(word,next,__ATOMIC_RELAXED);alphaWrites.fetch_add(1,std::memory_order_relaxed);}
                }
            }else hookAimerOverrides.fetch_add(1,std::memory_order_relaxed);
            return target;
        }
    }
    return originalGetBestTarget(reticle,soft,multi);
}

inline void clearTarget() noexcept {
    publishedTarget.store(0,std::memory_order_release);
    publishedReticle.store(nullptr,std::memory_order_relaxed);publishedStates.store(0,std::memory_order_relaxed);publishedPlayer.store(0,std::memory_order_relaxed);
}

// Native 0x86baac6c. Info: +0 result pointer, +0x10 arguments (8 byte stride: goh guid of the asking actor, soft flag). Script thread; reads only the published atomics.
using NativeFn=void(*)(void* info);
using GetActorFromGuidFn=void*(*)(std::uint32_t goh);
inline NativeFn originalUnderReticle{};
inline GetActorFromGuidFn getActorFromGuid{};
inline std::atomic<bool> scriptOn{true},scriptPatched{},scriptDone{};
inline std::atomic<std::uintptr_t> scriptBase{};
inline std::atomic<const char*> scriptState{"NOT ATTEMPTED"};
inline void underReticle(void* info) noexcept {
    originalUnderReticle(info);
    scriptCalls.fetch_add(1,std::memory_order_relaxed);
    if(!scriptOn.load(std::memory_order_relaxed))return;
    const auto target=publishedTarget.load(std::memory_order_acquire);
    if(!target||reportsSinceTick.load(std::memory_order_relaxed)>=maxReportsPerTick)return;
    const auto p=reinterpret_cast<std::uintptr_t>(info);
    const auto* args=*reinterpret_cast<const std::uint32_t* const*>(p+field::infoArgs);
    if(args[0]!=publishedPlayer.load(std::memory_order_relaxed)||!getActorFromGuid(target))return;
    **reinterpret_cast<std::uint32_t* const*>(p+field::infoResult)=target;
    scriptOverrides.fetch_add(1,std::memory_order_relaxed);
}
// Finds the native's entry as scrThread::Validate does and swaps its handler once. An empty table or entry = not registered yet: the caller tries again.
template<class Read>
inline void patchUnderReticle(std::uintptr_t base,Read&& read) noexcept {
    std::uintptr_t table{};std::uint32_t size{};
    if(!read(base+nativeTableOffset,&table,sizeof table)||!table||table%8||!read(base+nativeTableSizeOffset,&size,sizeof size)||!size)return;
    std::uint32_t step=underReticleHash,index=underReticleHash%size;
    for(std::uint32_t n=0;n<size;++n){
        const std::uintptr_t entry=table+std::uintptr_t(index)*nativeEntryStride;std::uint32_t hash{};
        if(!read(entry,&hash,sizeof hash)||!hash)return;
        if(hash==underReticleHash){
            std::uintptr_t handler{};
            if(!read(entry+nativeEntryHandler,&handler,sizeof handler))return;
            if(handler!=base+underReticleOffset){scriptState.store("WORD MISMATCH");scriptDone.store(true);return;}
            originalUnderReticle=reinterpret_cast<NativeFn>(handler);
            getActorFromGuid=reinterpret_cast<GetActorFromGuidFn>(base+getActorFromGuidOffset);
            __atomic_store_n(reinterpret_cast<std::uintptr_t*>(entry+nativeEntryHandler),reinterpret_cast<std::uintptr_t>(&underReticle),__ATOMIC_RELEASE);
            scriptPatched.store(true);scriptDone.store(true);scriptState.store("INSTALLED");return;
        }
        step=(step>>1)+1;index=(index+step)%size;
    }
}

// What GameXr hands to tick(): the barrel of the VR-held firearm when every gate passed (want), and the player for the engine outcome read.
struct Frame {
    std::uint64_t now{};std::uintptr_t player{},reticle{},actor{};bool want{};
    float origin[3]{},direction[3]{},unitsPerMetre{1.f};
};
struct Hit {bool ok{},blocked{};std::uintptr_t instance{};float distance{};};

// Main thread, sagShellDerived::UpdateInputs: from the first frame after sagShellDerived::Init registered the natives, ahead of most script program loads. read = TraceEntry readMemory.
template<class Read>
inline void scriptPoll(Read&& read) noexcept {
    if(scriptDone.load(std::memory_order_relaxed))return;
    if(const auto base=scriptBase.load(std::memory_order_relaxed))patchUnderReticle(base,[&](std::uintptr_t p,void* out,std::size_t n){return read(nullptr,p,out,n);});
}

// Main thread, reticle boundary. read(address,out,size) = the mod's checked reader; cast(start,end,hit) = NativeAimProbe::castBetween.
template<class Read,class Cast>
inline void tick(const Frame& f,Read&& read,Cast&& cast) noexcept {
    static std::uint64_t leverNs{},lastCastNs{},candidateSinceNs{},candidateHitNs{},lastTargetHitNs{},scriptTryNs{};
    static std::uint32_t candidate{},engineLast{};
    const auto get=[&](std::uintptr_t p,auto& v){return p&&read(p,&v,sizeof v);};
    reportsSinceTick.store(0,std::memory_order_relaxed);
    if(!leverNs||f.now<leverNs||f.now-leverNs>2'000'000'000ull){leverNs=f.now?f.now:1;
        char v[PROP_VALUE_MAX]{};leverOn.store(!(__system_property_get("debug.rdrvr.vr.npcaim",v)>0&&v[0]=='0'),std::memory_order_relaxed);
        char d[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.vr.npcaim.dwell",d)>0)dwellMs.store(std::clamp(std::atoi(d),0,5000),std::memory_order_relaxed);
        char t[PROP_VALUE_MAX]{};scriptOn.store(!(__system_property_get("debug.rdrvr.vr.npcaim.script",t)>0&&t[0]=='0'),std::memory_order_relaxed);}
    if(!scriptDone.load(std::memory_order_relaxed)&&(!scriptTryNs||f.now<scriptTryNs||f.now-scriptTryNs>500'000'000ull)){scriptTryNs=f.now?f.now:1;
        if(const auto base=scriptBase.load(std::memory_order_relaxed))patchUnderReticle(base,[&](std::uintptr_t p,void* out,std::size_t n){return read(p,out,n);});}
    // Engine outcome of this frame's sagPlayer::UpdateInput: non-zero only after ReportAimingAtMe ran. Counted with the lever off too.
    std::uint32_t aimed{};
    if(get(f.player?f.player+field::playerAimedActor:0,aimed)&&aimed){
        stimulusFrames.fetch_add(1,std::memory_order_relaxed);
        if(aimed!=engineLast){stimulusTargets.fetch_add(1,std::memory_order_relaxed);lastStimulus.store(aimed,std::memory_order_relaxed);}
        engineLast=aimed;
    }else engineLast=0;
    if(!f.want||!leverOn.load(std::memory_order_relaxed)){clearTarget();candidate=0;return;}
    const auto current=publishedTarget.load(std::memory_order_relaxed);
    if(current){targetFrames.fetch_add(1,std::memory_order_relaxed);
        std::uint32_t zoom{};if(get(publishedStates.load(std::memory_order_relaxed)+field::statesZoom,zoom)&&zoom)zoomFrames.fetch_add(1,std::memory_order_relaxed);}
    const auto expire=[&]{if(publishedTarget.load(std::memory_order_relaxed)&&f.now-lastTargetHitNs>holdNs)clearTarget();};
    if(lastCastNs&&f.now>=lastCastNs&&f.now-lastCastNs<castIntervalNs){expire();return;}
    lastCastNs=f.now?f.now:1;
    float range{};
    if(!get(f.reticle+field::reticleRange,range)||!std::isfinite(range)||range<=0.f){noReticleRange.fetch_add(1,std::memory_order_relaxed);range=fallbackRangeM*f.unitsPerMetre;}
    range=std::min(range,1500.f);lastRangeM.store(range/f.unitsPerMetre,std::memory_order_relaxed);
    float end[3]{};for(unsigned i=0;i<3;++i)end[i]=f.origin[i]+f.direction[i]*range;
    Hit hit{};cast(f.origin,end,hit);
    casts.fetch_add(1,std::memory_order_relaxed);
    if(!hit.ok){castFails.fetch_add(1,std::memory_order_relaxed);expire();return;}
    std::uintptr_t user{},actor{};std::uint8_t kind{1};std::uint32_t goh{};
    if(hit.blocked){hits.fetch_add(1,std::memory_order_relaxed);
        if(hit.instance&&get(hit.instance+field::instUser,user)&&user&&get(user+field::userKind,kind)&&!kind&&get(user+field::userActor,actor)&&actor&&actor!=f.actor&&
           get(actor+field::actorGoh,goh)&&goh){actorHits.fetch_add(1,std::memory_order_relaxed);lastDistanceM.store(hit.distance/f.unitsPerMetre,std::memory_order_relaxed);}
        else goh=0;}
    if(!goh){if(f.now-candidateHitNs>holdNs)candidate=0;expire();return;}
    if(goh!=candidate){candidate=goh;candidateSinceNs=f.now;}
    candidateHitNs=f.now;
    if(goh==publishedTarget.load(std::memory_order_relaxed))lastTargetHitNs=f.now;
    else if(f.now-candidateSinceNs>=std::uint64_t(dwellMs.load(std::memory_order_relaxed))*1'000'000ull){
        std::uintptr_t states{};std::uint32_t player{};
        if(get(f.player?f.player+field::playerStates:0,states)&&states&&states%8==0&&get(f.actor+field::actorGoh,player)&&player){
            publishedStates.store(states,std::memory_order_relaxed);publishedReticle.store(reinterpret_cast<const void*>(f.reticle),std::memory_order_relaxed);
            publishedPlayer.store(player,std::memory_order_relaxed);
            publishedTarget.store(goh,std::memory_order_release);lastTarget.store(goh,std::memory_order_relaxed);
            lastTargetHitNs=f.now;targetsPublished.fetch_add(1,std::memory_order_relaxed);
        }
    }else expire();
    // The target's AI as sagPlayer::UpdateInput sees it: no AI / ignored = no report, aimers = aiMemory aimer entries (the player's report adds one).
    if(goh==publishedTarget.load(std::memory_order_relaxed)){
        std::uintptr_t ai{};std::uint8_t flags{};std::int32_t aimers{-1};
        if(!get(actor+field::actorAi,ai)||!ai)targetNoAi.fetch_add(1,std::memory_order_relaxed);
        else{
            if(get(ai+field::aiFlags,flags)&&(flags&2u))targetAiIgnored.fetch_add(1,std::memory_order_relaxed);
            if(get(ai+field::aiMemoryAimers,aimers)){lastAimers.store(aimers,std::memory_order_relaxed);
                if(aimers>maxAimers.load(std::memory_order_relaxed))maxAimers.store(aimers,std::memory_order_relaxed);}
        }
    }
}
inline void noteCastNs(std::uint64_t ns) noexcept {
    castNsSum.fetch_add(ns,std::memory_order_relaxed);
    auto m=castNsMax.load(std::memory_order_relaxed);while(ns>m&&!castNsMax.compare_exchange_weak(m,ns,std::memory_order_relaxed)){}
}

// Startup: nothing is hooked before every print and word checked out. got(slot,target) = word check, replace(address,expected,replacement) = TraceEntry replacePointer.
template<class Read,class Got,class Replace>
inline bool install(Read&& read,std::uintptr_t base,Got&& got,Replace&& replace) noexcept {
    const auto hash=[&](std::uintptr_t address,std::uint32_t size){
        std::uint64_t h=0xcbf29ce484222325ull;unsigned char chunk[256];
        for(std::uint32_t done=0;done<size;){const std::uint32_t n=std::min<std::uint32_t>(size-done,sizeof chunk);
            if(!read(nullptr,address+done,chunk,n))return std::uint64_t{};
            for(std::uint32_t i=0;i<n;++i)h=(h^chunk[i])*0x100000001b3ull;
            done+=n;}
        return h;};
    for(const auto& p:prints)if(hash(base+p.offset,p.size)!=p.fnv){installState.store("ENGINE MISMATCH");return false;}
    if(!got(getBestTargetGotOffset,getBestTargetOffset)||!got(aiVtableSlotOffset,weaponGetAimTargetOffset)){installState.store("WORD MISMATCH");return false;}
    originalGetBestTarget=reinterpret_cast<GetBestTargetFn>(base+getBestTargetOffset);
    if(!replace(base+getBestTargetGotOffset,base+getBestTargetOffset,reinterpret_cast<std::uintptr_t>(getBestTarget))){installState.store("HOOK FAILED");return false;}
    installState.store("INSTALLED");hooked.store(true,std::memory_order_release);
    bool scriptOk=true;for(const auto& p:scriptPrints)if(hash(base+p.offset,p.size)!=p.fnv)scriptOk=false;
    if(scriptOk){scriptState.store("PENDING");scriptBase.store(base,std::memory_order_relaxed);}
    else{scriptState.store("ENGINE MISMATCH");scriptDone.store(true);}
    return true;
}

// Report object vr.npcAim. Reading it on the next test:
//  casts / actorHits = the barrel probe saw an actor; targetsPublished / lastTarget (goh guid) = the override was armed;
//  hook.reportCalls = WeaponGetAimTarget passed its weapon and aniAim gates; hook.reportOverrides = the override reached UpdateInput;
//  stimulusFrames / stimulusTargets / lastStimulus (sagGuid) = ReportAimingAtMe ran (the melee path of UpdateInput writes it too); aimers = entries in the target's aiMemory;
//  zoomFrames = armed frames with states+0x14 != 0, which the ambient scripts require; script.calls / script.overrides = native 0x86baac6c, called / answered with the barrel target.
inline std::string report() {
    const auto u=[](const std::atomic<std::uint64_t>& v){return (unsigned long long)v.load(std::memory_order_relaxed);};
    const auto n=casts.load(std::memory_order_relaxed);
    char b[1700];
    std::snprintf(b,sizeof b,"{\"state\":\"%s\",\"on\":%s,\"dwellMs\":%d,\"casts\":%llu,\"castFails\":%llu,\"castUsAvg\":%.1f,\"castUsMax\":%.1f,\"hits\":%llu,\"actorHits\":%llu,"
        "\"rangeM\":%.1f,\"noRange\":%llu,\"lastDistanceM\":%.1f,\"targetsPublished\":%llu,\"targetFrames\":%llu,\"lastTarget\":%u,"
        "\"hook\":{\"calls\":%llu,\"reportCalls\":%llu,\"reportOverrides\":%llu,\"aimerOverrides\":%llu,\"alphaWrites\":%llu},"
        "\"stimulusFrames\":%llu,\"stimulusTargets\":%llu,\"lastStimulus\":%u,\"targetNoAi\":%llu,\"targetAiIgnored\":%llu,\"aimers\":%d,\"aimersMax\":%d,\"zoomFrames\":%llu,"
        "\"script\":{\"state\":\"%s\",\"on\":%s,\"calls\":%llu,\"overrides\":%llu}}",
        installState.load(),leverOn.load()?"true":"false",dwellMs.load(),u(casts),u(castFails),n?double(castNsSum.load())/double(n)/1e3:0.0,double(castNsMax.load())/1e3,
        u(hits),u(actorHits),double(lastRangeM.load()),u(noReticleRange),double(lastDistanceM.load()),u(targetsPublished),u(targetFrames),lastTarget.load(),
        u(hookCalls),u(hookReportCalls),u(hookReportOverrides),u(hookAimerOverrides),u(alphaWrites),
        u(stimulusFrames),u(stimulusTargets),lastStimulus.load(),u(targetNoAi),u(targetAiIgnored),lastAimers.load(),maxAimers.load(),u(zoomFrames),
        scriptState.load(),scriptOn.load()?"true":"false",u(scriptCalls),u(scriptOverrides));
    return b;
}
} // namespace rdrvr::vr::npcaim
