#pragma once
// GPU BOOST: the GPU level for players without QGO, without setting it by hand before every launch.
// An app cannot set debug.* properties (shell user only), so the kit's GPU_BOOST script starts tools/rdrvr_boost.sh on the headset
// over adb once per headset boot (shell user; keeps running after adb disconnects, dies at reboot - the same per-boot step as QGO).
// The helper copies the level the game asks for to debug.oculus.gpuLevel while the game process lives and clears it when the game
// asks for 0 or exits: the property pins min = max SYSTEM-WIDE (Quest 3: 7 -> 690 MHz even in Home; "" -> 492..640).
// Request = rdrvr/boost.req, one line 0 / 6 / 7, replaced atomically (tmp + rename) by the worker thread below:
//   GRAPHICS row GPU BOOST (graphics.cfg gpuBoost 0 OFF / 1 640 / 2 690, default 690) -> 0 / 6 / 7;
//   0 while the XR session is not VISIBLE / FOCUSED (headset off, Home);
//   690 -> 640 while the thermal trigger is latched (HeatWarning.h, whether the notice is shown or not);
//   debug.rdrvr.boost 0 / 6 / 7 overrides the row (bench).
// The helper writes debug.rdrvr.boostd = its clock (s, CLOCK_REALTIME) every 2 s: NO HELPER when it is missing or > 10 s old.
// Worker thread (started by the first session state change): 4 Hz, reads the properties and writes the file only on a change, so
// the XR thread never touches /sdcard (FUSE). Report: framePacing.gpuBoost.
// on() = is the player boosted, for the WELCOME panel (VrWelcome.h), whoever did it (the kit's helper or QGO). True when any of:
//   the clock property holds level >= 5 (levels pin min = max: 4 545, 5 599, 6 640, 7 690 MHz; QGO's GPU level is the same property);
//   the kgsl clock floor (devfreq/min_freq, 1 Hz) is >= 690 MHz: only level 7 pins that high, whatever tool set it. Not lower: the runtime moves the
//   floor by itself (3S reports with NO boost: min 492 and 545, min = max 545 on battery; ceilings 545..640), so 599 / 640 could say ON to an unboosted player;
//   the helper is alive and the game asked for a level (applied within its 2 s poll: no false OFF right after the headset is put back on).
#include "HeatWarning.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::runtime {
inline std::atomic<int> gpuBoostSetting{2}; // graphics.cfg gpuBoost (0 OFF, 1 640, 2 690), published by GraphicsSettings
class GpuBoost {
public:
    static constexpr const char* requestPath="/sdcard/Android/data/com.rockstargames.rdr/files/rdrvr/boost.req";
    static constexpr long helperStaleS=10;
    static constexpr int onLevel=5,onFloorMhz=690; // on() thresholds
    static constexpr int levelOf(int setting) noexcept {return setting==1?6:setting==2?7:0;}
    static constexpr int mhzOf(int level) noexcept {return level==6?640:level==7?690:0;}
    // XR thread (XrHarness::pollEvents): session state changed; running = VISIBLE or FOCUSED.
    void setSession(bool running) noexcept {
        running_.store(running,std::memory_order_relaxed);
        std::call_once(started_,[this]{try{std::thread([this]{loop();}).detach();}catch(...){threadFailed_.store(true,std::memory_order_relaxed);}});
    }
    int property() const noexcept {return property_.load(std::memory_order_relaxed);}   // debug.rdrvr.boost 0/6/7, -1 unset
    int requested() const noexcept {return requested_.load(std::memory_order_relaxed);} // level in boost.req (0/6/7), -1 not written yet
    int applied() const noexcept {return applied_.load(std::memory_order_relaxed);}     // debug.oculus.gpuLevel, -1 empty
    bool helper() const noexcept {return helper_.load(std::memory_order_relaxed);}      // helper heartbeat fresh
    bool heatCapped() const noexcept {return heatCapped_.load(std::memory_order_relaxed);}
    int clockFloorMhz() const noexcept {return floorMhz_.load(std::memory_order_relaxed);} // kgsl devfreq/min_freq, -1 unreadable / not read yet
    bool on() const noexcept {return applied()>=onLevel||clockFloorMhz()>=onFloorMhz||(helper()&&requested()>0);} // See the file header
    // What the WELCOME panel detected (-1 not shown yet, 0 boost off, 1 on): first = when it opened, last = the last redraw.
    void noteWelcome(bool boosted) noexcept {int never=-1;welcomeFirst_.compare_exchange_strong(never,boosted?1:0,std::memory_order_relaxed);welcomeLast_.store(boosted?1:0,std::memory_order_relaxed);}
    std::string json() const {
        char b[480];std::snprintf(b,sizeof b,
            "{\"setting\":%d,\"property\":%d,\"running\":%s,\"requested\":%d,\"applied\":%d,\"helper\":%s,\"helperAgeS\":%ld,\"heatCapped\":%s,"
            "\"on\":%s,\"clockFloorMhz\":%d,\"welcomeFirst\":%d,\"welcomeLast\":%d,\"writes\":%llu,\"writeFailures\":%llu,\"heatCaps\":%llu,\"thread\":\"%s\"}",
            gpuBoostSetting.load(std::memory_order_relaxed),property(),running_.load(std::memory_order_relaxed)?"true":"false",requested(),applied(),
            helper()?"true":"false",helperAge_.load(std::memory_order_relaxed),heatCapped()?"true":"false",
            on()?"true":"false",clockFloorMhz(),welcomeFirst_.load(std::memory_order_relaxed),welcomeLast_.load(std::memory_order_relaxed),
            u(writes_.load(std::memory_order_relaxed)),u(writeFailures_.load(std::memory_order_relaxed)),u(heatCaps_.load(std::memory_order_relaxed)),
            threadFailed_.load(std::memory_order_relaxed)?"FAILED":started()?"RUNNING":"NOT_STARTED");
        return b;
    }
private:
    static unsigned long long u(std::uint64_t v) noexcept {return static_cast<unsigned long long>(v);}
    bool started() const noexcept {return ticks_.load(std::memory_order_relaxed)>0;}
    [[noreturn]] void loop() noexcept {
        for(std::uint64_t retryAt=0,tick=0;;++tick){
            ticks_.store(tick+1,std::memory_order_relaxed);
            readProperties();if(tick%4==0)readClockFloor();
            const int prop=property();
            int want=prop>=0?prop:levelOf(gpuBoostSetting.load(std::memory_order_relaxed));
            if(!running_.load(std::memory_order_relaxed))want=0;
            const bool capped=want==7&&heatWarning.latched();
            if(capped){want=6;if(!heatCapped())heatCaps_.fetch_add(1,std::memory_order_relaxed);}
            heatCapped_.store(capped,std::memory_order_relaxed);
            if(want!=requested()&&tick>=retryAt){
                if(write(want)){requested_.store(want,std::memory_order_relaxed);writes_.fetch_add(1,std::memory_order_relaxed);}
                else{writeFailures_.fetch_add(1,std::memory_order_relaxed);retryAt=tick+20;} // 5 s
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    void readProperties() noexcept {
#if defined(__ANDROID__)
        char v[PROP_VALUE_MAX]{};
        const int n=__system_property_get("debug.rdrvr.boost",v);
        property_.store(n==1&&(v[0]=='0'||v[0]=='6'||v[0]=='7')?int(v[0]-'0'):-1,std::memory_order_relaxed);
        char g[PROP_VALUE_MAX]{};char* end{};
        const long level=__system_property_get("debug.oculus.gpuLevel",g)>0?std::strtol(g,&end,10):-1;
        applied_.store(level>=0&&end&&!*end?int(level):-1,std::memory_order_relaxed);
        char h[PROP_VALUE_MAX]{};end=nullptr;
        const long beat=__system_property_get("debug.rdrvr.boostd",h)>0?std::strtol(h,&end,10):0;
        const bool valid=beat>0&&end&&!*end;const long age=valid?long(std::time(nullptr))-beat:-1;
        helperAge_.store(age,std::memory_order_relaxed);
        helper_.store(valid&&age>=-helperStaleS&&age<=helperStaleS,std::memory_order_relaxed);
#endif
    }
    void readClockFloor() noexcept { // Worker thread, 1 Hz. A failed open is never retried (SELinux audit spam, same rule as KgslClock)
#if defined(__ANDROID__)
        static bool dead{};if(dead)return;
        std::FILE* f=std::fopen("/sys/class/kgsl/kgsl-3d0/devfreq/min_freq","r");if(!f){dead=true;floorMhz_.store(-1,std::memory_order_relaxed);return;}
        long long hz=0;const bool ok=std::fscanf(f,"%lld",&hz)==1;std::fclose(f);
        floorMhz_.store(ok&&hz>0?int(hz/1000000):-1,std::memory_order_relaxed);
#endif
    }
    static bool write(int level) noexcept {
        const std::string target=requestPath,temporary=target+".tmp";
        std::FILE* f=std::fopen(temporary.c_str(),"w");
        if(!f)return false;
        const bool ok=std::fprintf(f,"%d\n",level)>0;
        if(std::fclose(f)!=0||!ok){std::remove(temporary.c_str());return false;}
        if(std::rename(temporary.c_str(),target.c_str())!=0){std::remove(temporary.c_str());return false;}
        return true;
    }
    std::once_flag started_;
    std::atomic<bool> running_{},helper_{},heatCapped_{},threadFailed_{};
    std::atomic<int> property_{-1},requested_{-1},applied_{-1},floorMhz_{-1},welcomeFirst_{-1},welcomeLast_{-1};std::atomic<long> helperAge_{-1};
    std::atomic<std::uint64_t> ticks_{},writes_{},writeFailures_{},heatCaps_{};
};
inline GpuBoost gpuBoost;
}
