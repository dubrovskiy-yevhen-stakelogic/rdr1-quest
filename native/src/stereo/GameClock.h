#pragma once
// GAME CLOCK profiling control. Sets the in-game time of day (and optionally freezes it) on the engine main thread, so the
// heavy night state (lamps lit, shadowed point lights, low sun) can be reproduced for unattended profiling.
//
//  - sagTimeOfDayManager::sm_Instance (.bss 0x4e75cb30, GLOB_DAT GOT 0x2e014f8) -> heap object of 0x80 bytes (InitClass
//    @0x23989c8: operator new(0x80) + ctor, vptr = vtable 0x2d4c608 + 0x10). Fields:
//      +0x08 f32 acceleration factor (game seconds per real second, Update: +0x58 += TIME.seconds * accel; set by script
//            SET_TIME_ACCELERATION -> SetAccelerationFactor(f,false); Init/Reset write 0)
//      +0x30 u8  time-warp (slow motion) active          +0x50 u8 PAUSED: Update() skips the advance, still runs UpdateInternal
//            (written only by Init/Reset/ctor = 0 and by rdr2CutsAssetMgrEntity::DispatchEvent: 1 on cutscene load, 0 at the end)
//      +0x54 f32 Kahan remainder of the advance          +0x58 f32 ABSOLUTE GAME SECONDS (day*86400 + seconds of day)
//      +0x5c u32 packed sagTimeOfDay = day(bits 0-14) | hour(15-19) | minute(20-25) | second(26-31), derived from +0x58
//      +0x60 / +0x64 packed dawn (06:00) / dusk (18:00) for IsLightOut/IsDarkOut      +0x68 f32 absolute hours (+0x58/3600)
//  - setter = sagTimeOfDayManager::SetTimeOfDay(sagTimeOfDay const&) @0x239945c (JUMP_SLOT 0x2e49390): seconds of day from
//    h/m/s; day field 0 -> current day, +1 if the target is earlier than now (the clock only moves FORWARD, like sleeping);
//    clears the remainder +0x54 and the remote-sync block +0x70..+0x7c, stores +0x58, tail-calls UpdateTimeOfDay(true)
//    @0x239935c which recomputes +0x5c/+0x68 at once (+ a net event only in a joined MP session). The same call the engine
//    itself uses for SET_TIME_OF_DAY / SET_TIME_AS_INT script natives and RDRUICredits::OnShow (FromHours(13) -> SetTimeOfDay).
//  - tick = sagShellDerived::Update(float) @0x23a2790 `bl sagTimeOfDayManager::Update` (only while
//    gdGameData::IsCurrentlySimulating), on the main thread; the mod's hook (sagShellDerived::UpdateInputs,
//    Teleport.h) runs on the same thread between two simulation steps. Every consumer (sagWorld::Update ->
//    rmwEnvironment/ltLightingMgr::SetTimeOfDay, rdrDynamicLight::CalculateDayNightFactors, SelectShadows,
//    WeatherManager::ProcessWeather, population/gringo schedules, audio) polls the manager each frame, so a
//    jump is consistent from the next frame on.
// Properties (polled once per second on the main thread; the first value seen is a BASELINE, a CHANGE triggers):
//   debug.rdrvr.clock         "HH[:MM][:tag]"  set the clock to HH:MM (00..23, 00..59; next occurrence, engine rule);
//                             a non-numeric 2nd/3rd field is a tag so the same time can be requested again ("19:a", "19:30:b");
//                             "-" / "x" / "off" = no-op reset.
//   debug.rdrvr.clock.freeze  "1[:tag]" freeze (engine pause flag +0x50, re-asserted every frame, e.g. after a cutscene clears
//                             it), "0" release (only a flag the mod set is cleared).
// Report: top-level `clock` (TraceEntry). Without a property change nothing is written.
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::gameclock {

// ---- ELF identities (librdr.so; file offset = vaddr - 0x4000 for .text) ----
inline constexpr std::uintptr_t instanceOffset=0x4e75cb30,instanceGotOffset=0x2e014f8;      // sagTimeOfDayManager::sm_Instance
inline constexpr std::uintptr_t vtableOffset=0x2d4c608,vtableGotOffset=0x2e01a10;           // vtable; object vptr = vtable+0x10
inline constexpr std::uintptr_t setTimeOfDayOffset=0x239945c,setTimeOfDayGotOffset=0x2e49390;
inline constexpr std::uintptr_t updateTimeOfDayOffset=0x239935c,updateTimeOfDayGotOffset=0x2e49388; // SetTimeOfDay tail-calls it via PLT
inline constexpr std::uintptr_t get24HourOffset=0x2399558,get24HourGotOffset=0x2e47078;
inline constexpr std::uintptr_t updateOffset=0x2398c10,updateGotOffset=0x2e49368;          // the tick (pause flag, accel, Kahan add)
inline constexpr std::uintptr_t tickSiteOffset=0x23a2784;                                   // sagShellDerived::Update: adrp/ldr GOT/ldr/bl Update
// manager fields
inline constexpr std::size_t accelField=0x08,warpField=0x30,pausedField=0x50,secondsField=0x58,packedField=0x5c,dawnField=0x60,duskField=0x64;
// Exact bytes: whole SetTimeOfDay (136), UpdateTimeOfDay (256), Get24HourTime (60), Update (264), the tick call site (16).
inline constexpr std::array<unsigned char,136> setTimeOfDayFingerprint{
#include "gamecode/GameClock_239945c_136_8d.inc"
};
inline constexpr std::array<unsigned char,256> updateTimeOfDayFingerprint{
#include "gamecode/GameClock_239935c_256_8d.inc"
};
inline constexpr std::array<unsigned char,60> get24HourFingerprint{
#include "gamecode/GameClock_2399558_60_8d.inc"
};
inline constexpr std::array<unsigned char,264> updateFingerprint{
#include "gamecode/GameClock_2398c10_264_8d.inc"
};
inline constexpr std::array<unsigned char,16> tickSiteFingerprint{
#include "gamecode/GameClock_23a2784_16_8d.inc"
};

// ---- sagTimeOfDay packing (pure; host-tested) ----
struct Time {unsigned day{},hour{},minute{},second{};};
inline Time decode(std::uint32_t v) noexcept {return {v&0x7fffu,(v>>15)&0x1fu,(v>>20)&0x3fu,v>>26};}
inline std::uint32_t encode(const Time& t) noexcept {
    return (t.day&0x7fffu)|((t.hour&0x1fu)<<15)|((t.minute&0x3fu)<<20)|((t.second&0x3fu)<<26);}
inline unsigned secondsOfDay(const Time& t) noexcept {return t.hour*3600u+t.minute*60u+t.second;}

// ---- request parsing (pure; host-tested) ----
struct Request {int hour{-1},minute{0};std::string tag;};
enum class Parse {Request,Reset,Bad};
inline bool digits(const std::string& s,std::size_t maxLen) noexcept {
    if(s.empty()||s.size()>maxLen)return false;for(char c:s)if(c<'0'||c>'9')return false;return true;}
// "HH[:MM][:tag]"; HH 0..23, MM 0..59 (1-2 digits each). "-", "x", "off", "" = Reset (no-op).
inline Parse parseRequest(const std::string& raw,Request& r) {
    r=Request{};
    if(raw.empty()||raw=="-"||raw=="x"||raw=="X"||raw=="off"||raw=="OFF")return Parse::Reset;
    const auto c1=raw.find(':');
    const std::string h=raw.substr(0,c1);
    if(!digits(h,2))return Parse::Bad;
    r.hour=std::atoi(h.c_str());if(r.hour>23)return Parse::Bad;
    if(c1==std::string::npos)return Parse::Request;
    const auto c2=raw.find(':',c1+1);
    const std::string second=raw.substr(c1+1,c2==std::string::npos?std::string::npos:c2-c1-1);
    if(digits(second,2)){
        r.minute=std::atoi(second.c_str());if(r.minute>59)return Parse::Bad;
        if(c2!=std::string::npos)r.tag=raw.substr(c2+1);
    }else r.tag=raw.substr(c1+1); // a non-numeric second field is the tag ("19:a")
    return Parse::Request;
}
// "1[:tag]" freeze, "0[:tag]" release, anything else = no change (returns -1).
inline int parseFreeze(const std::string& raw) noexcept {
    const std::string v=raw.substr(0,raw.find(':'));
    return v=="1"?1:v=="0"?0:-1;
}

// ---- engine binding ----
using SetTimeFn=void(*)(void* manager,const std::uint32_t* packedTime); // sagTimeOfDayManager::SetTimeOfDay(this, sagTimeOfDay const&)
struct EngineApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};
    std::uintptr_t instance{};   // address of sm_Instance (holds the manager pointer)
    std::uintptr_t vptr{};       // expected manager vptr (vtable + 0x10)
    std::uintptr_t gameData{};   // GAMEDATA (state u32 at +0; 3 = in game)
    SetTimeFn setTime{};
};
struct Sample {
    bool valid{};std::uintptr_t manager{};int gameState{-1};
    Time now{};float seconds{},accel{};bool paused{},warp{};Time dawn{},dusk{};
};
struct Last {
    bool valid{};std::string value,source,result{"NONE"},reason;int hour{-1},minute{0};
    Time before{},after{};float beforeSeconds{},afterSeconds{};double jumpHours{};
    std::uint64_t requestNs{},executeNs{};
};
struct State {
    std::mutex mutex;
    EngineApi api{};
    std::string engineState{"NOT_CONFIGURED"};
    std::string lastProperty,lastFreezeProperty;bool propertyBaseline{};
    Request pending{};std::string pendingValue;bool hasPending{};std::uint64_t pendingNs{};
    Last last{};
    Sample sample{};std::uint64_t sampleNs{};const char* sampleReason{"NO SAMPLE"};
    bool freezeWanted{},ownsPause{};
    std::uint64_t frames{}; // tick() calls (sampling cadence)
};
inline State state;
inline std::atomic<bool> engineReady{},hookInstalled{};
inline std::atomic<std::uint64_t> qualifiedCalls{},exceptions{};
inline std::atomic<unsigned> requests{},done{},failures{},badProperties{},freezeWrites{},releaseWrites{},foreignPauseFrames{};
inline constexpr std::uint64_t transientWaitNs=30'000'000'000ull; // a request waits this long for GAMEDATA==3 / a manager

inline std::uint64_t monotonicNs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}

inline void configureEngine(const EngineApi& api) noexcept {
    try{std::lock_guard lock(state.mutex);state.api=api;state.engineState="WAITING_FOR_MAIN_THREAD";}catch(...){}
    engineReady.store(api.read&&api.instance&&api.vptr&&api.gameData&&api.setTime,std::memory_order_release);
}
inline void engineUnavailable(const char* why) noexcept {
    engineReady.store(false,std::memory_order_release);
    try{std::lock_guard lock(state.mutex);state.engineState=why;}catch(...){}
}
inline void markHookInstalled() noexcept {hookInstalled.store(true,std::memory_order_release);}

namespace detail {
template<class T> inline bool get(const EngineApi& a,std::uintptr_t p,std::size_t o,T& out) noexcept {
    return a.read&&p&&p<=UINTPTR_MAX-o&&p+o<=UINTPTR_MAX-sizeof(T)&&a.read(nullptr,p+o,&out,sizeof(T));}
// Returns nullptr on success, else why the manager is not usable.
inline const char* sample(const EngineApi& a,Sample& s) noexcept {
    s=Sample{};std::uint32_t gs{};
    if(get(a,a.gameData,0,gs))s.gameState=int(gs);
    std::uintptr_t m{};
    if(!get(a,a.instance,0,m)||!m)return "NO MANAGER";
    unsigned char block[0x68]{}; // one guarded read of fields +0x00..+0x67
    if(m%8||!a.read||!a.read(nullptr,m,block,sizeof block))return "READ";
    std::uintptr_t vptr{};std::memcpy(&vptr,block,sizeof vptr);
    if(vptr!=a.vptr)return "BAD MANAGER";
    std::uint32_t packed{},dawn{},dusk{};
    std::memcpy(&s.seconds,block+secondsField,4);std::memcpy(&packed,block+packedField,4);std::memcpy(&s.accel,block+accelField,4);
    std::memcpy(&dawn,block+dawnField,4);std::memcpy(&dusk,block+duskField,4);
    const bool paused=block[pausedField]!=0,warp=block[warpField]!=0;
    if(!std::isfinite(s.seconds)||s.seconds<0)return "BAD TIME";
    s.manager=m;s.now=decode(packed);s.paused=paused;s.warp=warp;s.dawn=decode(dawn);s.dusk=decode(dusk);s.valid=true;
    return nullptr;
}
inline void finishLast(Last& l,const char* result,const char* reason) {l.result=result;l.reason=reason?reason:"";}
} // namespace detail

// One main-thread step. `clockProperty`/`freezeProperty` non-null = a property poll happened this frame (values as read).
// Host tests call this directly with fake engine memory.
inline void tick(const std::string* clockProperty,const std::string* freezeProperty,std::uint64_t now,const char* source="PROP") noexcept {
    if(!engineReady.load(std::memory_order_acquire))return;
    try{
        using namespace detail;
        std::unique_lock lock(state.mutex,std::try_to_lock);
        if(!lock.owns_lock())return; // report copy in progress: next frame
        auto& s=state;const auto& a=s.api;
        if(s.engineState=="WAITING_FOR_MAIN_THREAD")s.engineState="MAIN_THREAD";
        // 1. properties (edge-triggered after the baseline)
        if(clockProperty&&freezeProperty){
            if(!s.propertyBaseline){s.propertyBaseline=true;s.lastProperty=*clockProperty;s.lastFreezeProperty=*freezeProperty;}
            else{
                if(*clockProperty!=s.lastProperty){s.lastProperty=*clockProperty;Request r{};
                    switch(parseRequest(*clockProperty,r)){
                    case Parse::Request:requests.fetch_add(1);s.pending=r;s.pendingValue=*clockProperty;s.hasPending=true;s.pendingNs=now;break;
                    case Parse::Reset:break;
                    case Parse::Bad:requests.fetch_add(1);badProperties.fetch_add(1);failures.fetch_add(1);
                        s.last=Last{};s.last.valid=true;s.last.value=*clockProperty;s.last.source=source;s.last.requestNs=now;finishLast(s.last,"FAILED","BAD PROPERTY");break;
                    }
                }
                if(*freezeProperty!=s.lastFreezeProperty){s.lastFreezeProperty=*freezeProperty;const int f=parseFreeze(*freezeProperty);if(f>=0)s.freezeWanted=f==1;}
            }
        }
        // 2. sample: 3 guarded reads; every 4th frame, every frame while a request is pending
        if(!s.hasPending&&((s.frames++)&3u))return;
        Sample smp{};const char* why=sample(a,smp);
        s.sample=smp;s.sampleNs=now;s.sampleReason=why?why:"OK";
        const bool inGame=smp.valid&&smp.gameState==3;
        // 3. freeze (engine pause flag +0x50; only while in game, only the mod's own flag is released)
        if(smp.valid){
            volatile std::uint8_t* paused=reinterpret_cast<volatile std::uint8_t*>(smp.manager+pausedField);
            if(s.freezeWanted&&inGame&&!smp.paused){*paused=1;s.ownsPause=true;freezeWrites.fetch_add(1);smp.paused=true;s.sample.paused=true;}
            else if(s.freezeWanted&&smp.paused&&!s.ownsPause)foreignPauseFrames.fetch_add(1); // a cutscene holds it; taken over when it clears
            else if(!s.freezeWanted&&s.ownsPause){
                if(smp.paused){*paused=0;releaseWrites.fetch_add(1);s.sample.paused=false;}
                s.ownsPause=false;
            }
        }
        // 4. pending clock request
        if(!s.hasPending)return;
        const Request r=s.pending;
        const auto fail=[&](const char* reason){s.hasPending=false;failures.fetch_add(1);
            s.last=Last{};s.last.valid=true;s.last.value=s.pendingValue;s.last.source=source;s.last.hour=r.hour;s.last.minute=r.minute;
            s.last.requestNs=s.pendingNs;finishLast(s.last,"FAILED",reason);};
        if(!smp.valid||!inGame){
            if(now-s.pendingNs<transientWaitNs)return; // loading / title / pause transitions
            fail(why?why:"NOT IN GAME");return;
        }
        s.hasPending=false;
        Last l{};l.valid=true;l.value=s.pendingValue;l.source=source;l.hour=r.hour;l.minute=r.minute;l.requestNs=s.pendingNs;l.executeNs=now;
        l.before=smp.now;l.beforeSeconds=smp.seconds;
        const std::uint32_t target=encode({0,unsigned(r.hour),unsigned(r.minute),0}); // day 0 = engine "next occurrence"
        a.setTime(reinterpret_cast<void*>(smp.manager),&target);
        Sample after{};const char* afterWhy=sample(a,after);
        if(afterWhy){finishLast(l,"FAILED",afterWhy);failures.fetch_add(1);s.last=l;return;}
        s.sample=after;l.after=after.now;l.afterSeconds=after.seconds;l.jumpHours=(double(after.seconds)-double(l.beforeSeconds))/3600.0;
        if(after.now.hour==unsigned(r.hour)&&after.now.minute==unsigned(r.minute)){finishLast(l,"DONE",nullptr);done.fetch_add(1);} // seconds not compared: float +0x58 is exact only up to day ~194
        else{finishLast(l,"FAILED","READBACK");failures.fetch_add(1);}
        s.last=l;
    }catch(...){exceptions.fetch_add(1);}
}

#if defined(__ANDROID__)
inline std::string propertyValue(const char* name) {
    char value[PROP_VALUE_MAX]{};const int saved=errno;const int n=__system_property_get(name,value);errno=saved;
    return n>0?std::string(value,std::size_t(n)):std::string();
}
// Main thread, once per sagShellDerived::Execute iteration (qualified = the call from Execute; see Teleport.h).
inline void gameFrame(bool qualified) noexcept {
    if(!qualified)return;
    qualifiedCalls.fetch_add(1,std::memory_order_relaxed);
    if(!engineReady.load(std::memory_order_acquire))return;
    try{
        const auto now=monotonicNs();
        static std::uint64_t nextPoll=0;
        if(now>=nextPoll){
            nextPoll=now+1'000'000'000ull;
            const auto c=propertyValue("debug.rdrvr.clock"),f=propertyValue("debug.rdrvr.clock.freeze");
            tick(&c,&f,now);
        }else tick(nullptr,nullptr,now);
    }catch(...){exceptions.fetch_add(1);}
}
#else
inline void gameFrame(bool) noexcept {}
#endif

// ---- report (any thread) ----
inline std::string jsonSafe(const std::string& v) {std::string o;o.reserve(v.size());for(char c:v)o+=(c=='"'||c=='\\'||static_cast<unsigned char>(c)<0x20)?'?':c;return o;}
inline std::string report() {
    std::string out;out.reserve(1024);char b[768];
    std::lock_guard lock(state.mutex);const auto& s=state;const auto& m=s.sample;
    std::snprintf(b,sizeof b,"{\"ready\":%s,\"hookInstalled\":%s,\"engineState\":\"%s\",\"qualifiedCalls\":%llu,\"exceptions\":%llu,",
        engineReady.load()?"true":"false",hookInstalled.load()?"true":"false",jsonSafe(s.engineState).c_str(),
        (unsigned long long)qualifiedCalls.load(),(unsigned long long)exceptions.load());out+=b;
    const double hours=m.valid?double(secondsOfDay(m.now))/3600.0:-1.0;
    const bool lightOut=m.valid&&secondsOfDay(m.now)>secondsOfDay(m.dawn)&&secondsOfDay(m.now)<secondsOfDay(m.dusk); // IsLightOut
    std::snprintf(b,sizeof b,"\"current\":{\"valid\":%s,\"hour\":%u,\"minute\":%u,\"second\":%u,\"day\":%u,\"hours24\":%.3f,\"absoluteSeconds\":%.1f,\"accel\":%.3f,\"paused\":%s,\"timeWarp\":%s,\"lightOut\":%s,\"gameState\":%d,\"reason\":\"%s\",\"ageMs\":%.0f},",
        m.valid?"true":"false",m.now.hour,m.now.minute,m.now.second,m.now.day,hours,double(m.seconds),double(m.accel),m.paused?"true":"false",
        m.warp?"true":"false",lightOut?"true":"false",m.gameState,s.sampleReason,s.sampleNs?double(monotonicNs()-s.sampleNs)/1e6:-1.0);out+=b;
    const auto& l=s.last;const auto ms=[](std::uint64_t a,std::uint64_t z){return a&&z>=a?double(z-a)/1e6:-1.0;};
    std::snprintf(b,sizeof b,"\"lastRequest\":{\"valid\":%s,\"value\":\"%s\",\"source\":\"%s\",\"target\":\"%02d:%02d\",\"result\":\"%s\",\"reason\":\"%s\",\"before\":\"d%u %02u:%02u:%02u\",\"after\":\"d%u %02u:%02u:%02u\",\"jumpHours\":%.3f,\"queueMs\":%.1f,\"ageS\":%.1f},",
        l.valid?"true":"false",jsonSafe(l.value).c_str(),jsonSafe(l.source).c_str(),l.hour,l.minute,jsonSafe(l.result).c_str(),jsonSafe(l.reason).c_str(),
        l.before.day,l.before.hour,l.before.minute,l.before.second,l.after.day,l.after.hour,l.after.minute,l.after.second,l.jumpHours,
        ms(l.requestNs,l.executeNs),l.executeNs?double(monotonicNs()-l.executeNs)/1e9:-1.0);out+=b;
    std::snprintf(b,sizeof b,"\"pending\":%s,\"requests\":%u,\"done\":%u,\"failures\":%u,\"badProperties\":%u,\"frozen\":%s,\"freeze\":{\"requested\":%s,\"ownsPause\":%s,\"writes\":%u,\"releases\":%u,\"foreignPauseFrames\":%u},\"property\":\"%s\",\"freezeProperty\":\"%s\"}",
        s.hasPending?"true":"false",requests.load(),done.load(),failures.load(),badProperties.load(),
        (s.freezeWanted&&m.paused)?"true":"false",s.freezeWanted?"true":"false",s.ownsPause?"true":"false",freezeWrites.load(),releaseWrites.load(),
        foreignPauseFrames.load(),jsonSafe(s.lastProperty).c_str(),jsonSafe(s.lastFreezeProperty).c_str());out+=b;
    return out;
}
} // namespace rdrvr::gameclock
