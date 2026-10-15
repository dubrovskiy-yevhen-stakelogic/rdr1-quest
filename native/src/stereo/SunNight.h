#pragma once
// SUN SHADOWS value 7 "STAGGER / NIGHT OFF" = SHADOW UPDATE STAGGER by day, SUN SHADOWS OFF while the game clock is in the
// night window. At night the directional "sun" is the moon: the collector receiver pass in each eye's depth stage (cost:
// eyePassByIndex.leftDepth k1 0.456 ms, 5.2 draws) plus the 1600x1600 cascade atlas (~0.55 ms per frame incl. STAGGER).
// The existing SUN SHADOWS OFF path does all the work: VisibleTrims.h (TraceEntry observeScene) feeds the governed flag to
// GraphicsDrawPolicy (receiver skipped in graphicsSun) and ShadowUpdate.h skips the atlas only while the receiver of this or
// the previous scene really was suppressed (no stale atlas at dawn). Lamp lights and lamp shadows are untouched.
//  * Clock: GameClock.h main-thread sample (sagTimeOfDayManager +0x5c hour/minute, refreshed every 4th main frame) copied
//    here once per second under try_lock (never blocks the render thread). No valid sample -> NO CLOCK = acts as STAGGER.
//  * Window 20:00..05:00, 2 min hysteresis: [19:58,20:00) and [05:00,05:02) keep the previous state (forward game time:
//    off from 20:00 to 05:02).
//  * debug.rdrvr.sunnight (read once per second): unset = the row decides; 0 = kill switch (no gate for any row, 7 acts as
//    STAGGER); 1 = clock gate for any row with SUN SHADOWS on; 2 = forced off (also by day or without the clock).
// Report graphics.shadowUpdate.sunNight; byState signature: SunStagger suffix "z" while the gate holds the sun shadows off
// (_su7z at night, _su7 by day; _su<row>z when the property engages another row).
#include "GameClock.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <ostream>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine::shadowupdate::sunnight {
inline constexpr unsigned rowValue=7,dayLevel=5; // = GraphicsSettings::shadowUpdateNightOff / shadowUpdateStagger (VisibleTrims.h asserts)
inline constexpr unsigned nightStartMin=20*60,nightEndMin=5*60,hysteresisMin=2,dayMinutes=24*60;

// ---- pure (host-tested) ----
inline constexpr bool nightAt(unsigned minute,bool previous) noexcept {
    minute%=dayMinutes;
    if(minute>=nightStartMin||minute<nightEndMin)return true;                                  // 20:00..04:59
    if(minute<nightEndMin+hysteresisMin||minute>=nightStartMin-hysteresisMin)return previous;  // 05:00..05:01, 19:58..19:59
    return false;
}
enum State : int {Idle=0,Day=1,Night=2,NoClock=3,Disabled=4,Forced=5,StateCount=6};
inline constexpr const char* stateNames[StateCount]{"IDLE","DAY","NIGHT","NO CLOCK","DISABLED","FORCED"};
struct Decision {State state{Idle};bool off{};};
// property: -1 unset, 0 kill switch, 1 clock gate for any row, 2 forced off. clock: a valid minute exists; night: nightAt().
inline constexpr Decision decide(bool shadows,unsigned update,int property,bool clock,bool night) noexcept {
    State s=Idle;
    if(property==0)s=update==rowValue?Disabled:Idle;
    else if(property==2)s=Forced;
    else if(property==1||update==rowValue)s=!clock?NoClock:night?Night:Day;
    return {s,shadows&&(s==Night||s==Forced)};
}
// runtime::sunNightMenu value: state + 8 while the property is set (GraphicsMenu.h SUN SHADOWS suffix).
inline constexpr int menuCode(State s,int property) noexcept {return int(s)|(property>=0?8:0);}
inline constexpr int validProperty(int p) noexcept {return p>=0&&p<=2?p:-1;}

// ---- runtime (sceneBegin: render thread; the rest any thread) ----
struct Counters {
    std::atomic<std::uint64_t> scenes{},offScenes{},switches{},propertyReads{},clockReads{},clockBusy{},clockInvalid{};
    std::array<std::atomic<std::uint64_t>,StateCount> byState{};
};
inline Counters counters;
inline std::atomic<int> property{-1},state{-1},clockMinute{-1},lastSwitchTo{-1},lastSwitchMinute{-1};
inline std::atomic<bool> off{};                        // the gate holds the sun shadows off this scene (signature "z")
inline std::atomic<std::uint64_t> clockSampleNs{},lastSwitchNs{},lastSceneNs{};
inline bool nightLatched{};inline std::uint64_t nextPollNs{}; // render thread only
inline bool suppressing() noexcept {return off.load(std::memory_order_relaxed);}
inline int menuState() noexcept {
    const int s=state.load(std::memory_order_relaxed);return s<0?-1:menuCode(State(s),property.load(std::memory_order_relaxed));}
#if defined(__ANDROID__)
inline int readProperty() noexcept {
    char v[PROP_VALUE_MAX]{};const int n=__system_property_get("debug.rdrvr.sunnight",v);return n==1?validProperty(v[0]-'0'):-1;}
#endif
// The GameClock main-thread sample -> minute of day. A busy lock keeps the previous minute; no valid sample = no clock.
inline void readClock() noexcept {
    std::unique_lock lock(gameclock::state.mutex,std::try_to_lock);
    if(!lock.owns_lock()){counters.clockBusy.fetch_add(1,std::memory_order_relaxed);return;}
    const auto& smp=gameclock::state.sample;
    if(!smp.valid||smp.now.hour>23||smp.now.minute>59){clockMinute.store(-1,std::memory_order_relaxed);counters.clockInvalid.fetch_add(1,std::memory_order_relaxed);return;}
    clockMinute.store(int(smp.now.hour*60u+smp.now.minute),std::memory_order_relaxed);clockSampleNs.store(gameclock::state.sampleNs,std::memory_order_relaxed);
    counters.clockReads.fetch_add(1,std::memory_order_relaxed);
}
// Every RenderScene (VisibleTrims.h). nowNs = steady_clock ns. Returns the governed SUN SHADOWS flag for GraphicsDrawPolicy.
inline bool sceneBegin(bool shadows,unsigned update,std::uint64_t nowNs) noexcept {
    auto& c=counters;c.scenes.fetch_add(1,std::memory_order_relaxed);lastSceneNs.store(nowNs,std::memory_order_relaxed);
    if(nowNs>=nextPollNs){
        nextPollNs=nowNs+1'000'000'000ull;
#if defined(__ANDROID__)
        const int saved=errno;property.store(readProperty(),std::memory_order_relaxed);errno=saved;
#endif
        c.propertyReads.fetch_add(1,std::memory_order_relaxed);
        readClock();
    }
    const int p=validProperty(property.load(std::memory_order_relaxed)),minute=clockMinute.load(std::memory_order_relaxed);
    if(minute>=0)nightLatched=nightAt(unsigned(minute),nightLatched);
    const Decision d=decide(shadows,update,p,minute>=0,nightLatched);
    if(d.off!=off.load(std::memory_order_relaxed)){
        c.switches.fetch_add(1,std::memory_order_relaxed);lastSwitchTo.store(d.off?1:0,std::memory_order_relaxed);
        lastSwitchMinute.store(minute,std::memory_order_relaxed);lastSwitchNs.store(nowNs,std::memory_order_relaxed);}
    off.store(d.off,std::memory_order_relaxed);state.store(int(d.state),std::memory_order_relaxed);
    c.byState[d.state].fetch_add(1,std::memory_order_relaxed);if(d.off)c.offScenes.fetch_add(1,std::memory_order_relaxed);
    return shadows&&!d.off;
}
// Report section graphics.shadowUpdate.sunNight (appended by ShadowUpdate.h report()).
inline void report(std::ostream& o) {
    const auto& c=counters;const auto ld=[](const std::atomic<std::uint64_t>& a){return a.load(std::memory_order_relaxed);};
    const int s=state.load(),m=clockMinute.load(),lm=lastSwitchMinute.load();const auto now=lastSceneNs.load(),sample=clockSampleNs.load(),sw=lastSwitchNs.load();
    char hhmm[8]="--:--",lhhmm[8]="--:--";
    if(m>=0)std::snprintf(hhmm,sizeof hhmm,"%02d:%02d",m/60,m%60);
    if(lm>=0)std::snprintf(lhhmm,sizeof lhhmm,"%02d:%02d",lm/60,lm%60);
    o<<"\"sunNight\":{\"rowValue\":"<<rowValue<<",\"property\":"<<property.load()<<",\"state\":\""<<(s>=0&&s<StateCount?stateNames[s]:"-")
     <<"\",\"off\":"<<(off.load()?1:0)<<",\"menu\":"<<menuState()<<",\"window\":\"20:00-05:00\",\"hysteresisMin\":"<<hysteresisMin
     <<",\"clock\":{\"minute\":"<<m<<",\"hhmm\":\""<<hhmm<<"\",\"night\":"<<(nightLatched?1:0)<<",\"reads\":"<<ld(c.clockReads)
     <<",\"busy\":"<<ld(c.clockBusy)<<",\"invalid\":"<<ld(c.clockInvalid)<<",\"sampleAgeMs\":"<<(sample&&now>=sample?double(now-sample)/1e6:-1.)<<'}'
     <<",\"scenes\":"<<ld(c.scenes)<<",\"offScenes\":"<<ld(c.offScenes)<<",\"switches\":"<<ld(c.switches)
     <<",\"lastSwitch\":{\"to\":"<<lastSwitchTo.load()<<",\"at\":\""<<lhhmm<<"\",\"ageS\":"<<(sw&&now>=sw?double(now-sw)/1e9:-1.)<<'}'
     <<",\"propertyReads\":"<<ld(c.propertyReads)<<",\"byState\":{";
    for(int i=0;i<StateCount;++i)o<<(i?",":"")<<'"'<<stateNames[i]<<"\":"<<ld(c.byState[i]);
    o<<"}}";
}
} // namespace rdrvr::engine::shadowupdate::sunnight
