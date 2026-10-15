#pragma once
// heat01 OVERHEAT WARNING (user: "small text like our FPS readout, but RED - HEADSET OVERHEATING, please pause and let it
// cool down - so players see it is the headset overheating, not the mod lagging. A MENU option, default ON: whoever wants to play on
// a hot headset just turns it off").
// XrHarness draws it on the persistent stats quad (PerfPanel::red), independent of PROFILER: readout OFF -> the two red lines alone,
// FPS -> under the FPS line (the crop grows right/down from the FPS readout's corner), DETAIL -> on top of the panel. Never over
// the open menu.
//
// Trigger: FramePacer::flightTick (XR thread, 1 Hz; it re-samples AThermal every 2 s). A sample is HOT when any of:
//   STATUS   Android thermal status >= 2 MODERATE (AThermal_getCurrentThermalStatus; 1 LIGHT = "UX not impacted" is not enough)
//   HEADROOM Android thermal headroom >= 1.00 (AThermal_getThermalHeadroom(0): 1.0 = the OS's own SEVERE throttling point);
//            once latched it stays HOT down to 0.95
//   XR       the runtime's XR_EXT_performance_settings THERMAL notification, CPU or GPU domain, at WARNING (25) or IMPAIRED (75)
//   TEMP     kgsl GPU temperature >= 85.0 C, once latched down to 82.0 C (guard for a runtime that reports none of the above)
// Deliberately NOT a trigger: kgsl thermal_pwrlevel / devfreq max_freq / CPU ceilings. The battery POWER POLICY moves them (Quest 3
// unplugged: GPU ceiling 545/492 MHz, tpl 3-4) at only 51-70 C with status 0 and headroom 0.54-0.67 (162k flight rows of
// /14) - not heat, never trips. Measured without throttling: AC 690 MHz pinned 15 min, GPU plateau 73 C, tpl 0, status 0
// headroom <= 0.73 (the hottest flight row: 75.5 C); no XR perf event in any report so far. (They are still reported, for context.)
// Hysteresis: latched after 3 consecutive HOT samples (~3 s), released after 15 consecutive clear samples (~15 s).
// Switch: graphics.cfg heatWarning (GRAPHICS row HEAT WARNING, default 1); debug.rdrvr.heatwarn (read at most once per second):
// unset/-1 = the menu, 0 off, 1 on, 2 force-show (test). The detector runs and counts while the warning is off.
// Report: framePacing.heatWarning (FramePacing.h report()).
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::runtime {
inline std::atomic<int> heatWarningSetting{1}; // heat01: graphics.cfg heatWarning (0/1), published by GraphicsSettings on load / set / defaults
class HeatWarning {
public:
    static constexpr int statusOn=2,headroomOn100=100,headroomOff100=95,xrLevelOn=25,tempOn10=850,tempOff10=820;
    static constexpr unsigned showAfter=3,hideAfter=15; // consecutive 1 Hz samples
    enum Reason : unsigned {Status=1,Headroom=2,Xr=4,Temp=8,Forced=16};
    // Panel text (PerfPanel::red = the rest of the line in red; panel glyphs only, <= 52 per line) and its crop in panel px
    // (glyph advance 12, line pitch 25, glyph 14 px tall, 12 px margins - the FPS crop is 12+12*12 x 38).
    static constexpr const char* panelText="\x01" "HEADSET OVERHEATING\n" "\x01" "PLEASE PAUSE AND LET IT COOL DOWN";
    static constexpr unsigned panelLines=2,cropWidth=12+12*33; // longest line: 33 glyphs
    static constexpr unsigned cropHeight(unsigned lines) noexcept {return 12+(lines-1)*25+14+12;}
    int property() const noexcept {return property_.load(std::memory_order_relaxed);}
    bool latched() const noexcept {return latched_.load(std::memory_order_relaxed);}
    bool enabled() const noexcept {const int p=property();return p>=0?p!=0:heatWarningSetting.load(std::memory_order_relaxed)!=0;}
    // Any thread, every frame: the red lines are on screen (a menu OFF hides them at once, a latch shows them within 1 s).
    bool shown() const noexcept {return property()==2||(latched()&&enabled());}
    // XR thread (XrHarness): true once per show/hide change, so the stats quad redraws now instead of at its next 2 Hz slot.
    bool takeRedraw() noexcept {const bool s=shown();if(s==drawn_)return false;drawn_=s;return true;}
    // XrPerformance::event: XR_EXT_performance_settings THERMAL notification (domain 0 CPU / 1 GPU, level 0 / 25 / 75).
    void resetXr() noexcept {xrCpu_.store(0,std::memory_order_relaxed);xrGpu_.store(0,std::memory_order_relaxed);}
    void noteXrThermal(unsigned domain,int level) noexcept {(domain?xrGpu_:xrCpu_).store(level,std::memory_order_relaxed);xrEvents_.fetch_add(1,std::memory_order_relaxed);}
    void setPropertyForTest(int p) noexcept {property_.store(p,std::memory_order_relaxed);} // host tests (no Android property)
    // FramePacer::flightTick (XR thread, 1 Hz). status / headroom100: -1 unknown (no AThermal); temp10 0 = unreadable.
    // maxMhz / tpl only go to the report (power policy, see the header).
    void observe(std::uint64_t now,int status,int headroom100,unsigned temp10,unsigned maxMhz,unsigned tpl) noexcept {
        pollProperty(now);
        std::lock_guard lk(m_);
        const bool was=latched_.load(std::memory_order_relaxed);
        const int xr=std::max(xrCpu_.load(std::memory_order_relaxed),xrGpu_.load(std::memory_order_relaxed));
        unsigned why=0;
        if(status>=statusOn&&status<=6)why|=Status;
        if(headroom100>=(was?headroomOff100:headroomOn100)&&headroom100<10000)why|=Headroom;
        if(xr>=xrLevelOn)why|=Xr;
        if(temp10>0&&temp10<2000&&int(temp10)>=(was?tempOff10:tempOn10))why|=Temp;
        auto& s=s_;++s.samples;s.why=why;s.status=status;s.headroom100=headroom100;s.temp10=temp10;s.maxMhz=maxMhz;s.tpl=tpl;
        s.maxStatus=std::max(s.maxStatus,status);s.maxHeadroom100=std::max(s.maxHeadroom100,headroom100<10000?headroom100:-1);s.maxTemp10=std::max(s.maxTemp10,temp10<2000?temp10:0u);
        if(why){++s.hotSamples;for(unsigned i=0;i<4;++i)if(why&(1u<<i))++s.hotBy[i];hotRun_=std::min(hotRun_+1,1000u);clearRun_=0;}
        else{clearRun_=std::min(clearRun_+1,1000u);hotRun_=0;}
        if(!was&&hotRun_>=showAfter){latched_.store(true,std::memory_order_relaxed);++s.latches;s.latchWhy=why;if(!enabled())++s.latchesWhileOff;}
        else if(was&&clearRun_>=hideAfter){latched_.store(false,std::memory_order_relaxed);++s.releases;}
        const bool on=shown();
        if(on&&!s.lastShown){++s.shows;s.shownSince=now;}else if(!on&&s.lastShown&&now>s.shownSince)s.shownNs+=now-s.shownSince;
        s.lastShown=on;
    }
    static void reasonText(unsigned why,char* out,std::size_t size) noexcept {
        if(!size)return;out[0]=0;if(!why){std::snprintf(out,size,"NONE");return;}
        static constexpr const char* names[5]{"STATUS","HEADROOM","XR","TEMP","FORCED"};std::size_t n=0;
        for(unsigned i=0;i<5&&n<size;++i)if(why&(1u<<i)){const int w=std::snprintf(out+n,size-n,"%s%s",n?"+":"",names[i]);if(w<0)break;n+=std::size_t(w);}
    }
    std::string json(std::uint64_t now) const {
        std::lock_guard lk(m_);const auto& s=s_;
        const int p=property();const unsigned forced=p==2?unsigned(Forced):0u;
        char why[48],latchWhy[48];reasonText(s.why|forced,why,sizeof why);reasonText(s.latchWhy,latchWhy,sizeof latchWhy);
        const double shownS=double(s.shownNs+(s.lastShown&&now>s.shownSince?now-s.shownSince:0))/1e9;
        char b[1400];std::snprintf(b,sizeof b,
            "{\"enabled\":%s,\"setting\":%d,\"property\":%d,\"shown\":%s,\"latched\":%s,\"hot\":%s,\"reason\":\"%s\",\"lastLatchReason\":\"%s\","
            "\"shows\":%llu,\"latches\":%llu,\"latchesWhileOff\":%llu,\"releases\":%llu,\"shownS\":%.1f,\"samples\":%llu,\"hotSamples\":%llu,"
            "\"hotBy\":{\"status\":%llu,\"headroom\":%llu,\"xr\":%llu,\"temp\":%llu},"
            "\"last\":{\"status\":%d,\"headroom100\":%d,\"xrCpu\":%d,\"xrGpu\":%d,\"xrEvents\":%llu,\"temp10\":%u,\"maxMhz\":%u,\"tpl\":%u},"
            "\"max\":{\"status\":%d,\"headroom100\":%d,\"temp10\":%u},"
            "\"thresholds\":{\"statusOn\":%d,\"headroomOn100\":%d,\"headroomOff100\":%d,\"xrLevelOn\":%d,\"tempOn10\":%d,\"tempOff10\":%d,\"showAfterS\":%u,\"hideAfterS\":%u}}",
            enabled()?"true":"false",heatWarningSetting.load(std::memory_order_relaxed),p,shown()?"true":"false",latched()?"true":"false",s.why?"true":"false",why,latchWhy,
            u(s.shows),u(s.latches),u(s.latchesWhileOff),u(s.releases),shownS,u(s.samples),u(s.hotSamples),u(s.hotBy[0]),u(s.hotBy[1]),u(s.hotBy[2]),u(s.hotBy[3]),
            s.status,s.headroom100,xrCpu_.load(std::memory_order_relaxed),xrGpu_.load(std::memory_order_relaxed),u(xrEvents_.load(std::memory_order_relaxed)),s.temp10,s.maxMhz,s.tpl,
            s.maxStatus,s.maxHeadroom100,s.maxTemp10,statusOn,headroomOn100,headroomOff100,xrLevelOn,tempOn10,tempOff10,showAfter,hideAfter);
        return b;
    }
private:
    static unsigned long long u(std::uint64_t v) noexcept {return static_cast<unsigned long long>(v);}
    struct Stats {
        std::uint64_t samples{},hotSamples{},latches{},latchesWhileOff{},releases{},shows{},shownSince{},shownNs{};std::array<std::uint64_t,4> hotBy{};
        unsigned why{},latchWhy{},temp10{},maxMhz{},tpl{},maxTemp10{};int status{-1},headroom100{-1},maxStatus{-1},maxHeadroom100{-1};bool lastShown{};
    };
    void pollProperty(std::uint64_t now) noexcept {
        if(now<nextProperty_)return;nextProperty_=now+1'000'000'000ull;
#if defined(__ANDROID__)
        char v[PROP_VALUE_MAX]{};
        property_.store(__system_property_get("debug.rdrvr.heatwarn",v)>0&&v[0]>='0'&&v[0]<='2'&&!v[1]?int(v[0]-'0'):-1,std::memory_order_relaxed);
#endif
    }
    mutable std::mutex m_;Stats s_{};                                   // observe (XR thread) / json (report thread)
    std::atomic<bool> latched_{};std::atomic<int> property_{-1},xrCpu_{0},xrGpu_{0};std::atomic<std::uint64_t> xrEvents_{};
    unsigned hotRun_{},clearRun_{};std::uint64_t nextProperty_{};bool drawn_{}; // XR thread
};
inline HeatWarning heatWarning;
}
