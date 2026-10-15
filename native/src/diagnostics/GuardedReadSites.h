#pragma once
// Accounting of guarded (process_vm_readv) reads.
// - Reads before the first world frame only count as "startup" (the site table is
//   reserved for steady-state frames; 128-slot table filled during startup).
// - A site is the pair (pc0, pc1): pc0 = return address into the direct caller of the
//   reader, pc1 = that caller's own return address (one frame-record step, validated
//   against the current stack; 0 when unavailable). Wrappers/lambdas therefore show
//   their real user in pc1.
// - Calls made on the render thread (the thread that calls frame()) are counted
//   separately per site.
// - The site table (frame-record walk + table insert per guarded read) runs only
//   when requested: property debug.rdrvr.guardedsites=1 (re-read every 64 frames).
//   The render thread's per-frame counts (kernel-checked calls, the segments they
//   served, direct copies) are always kept; they come from the thread's own counters.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#include <unistd.h>
#endif

namespace rdrvr::diagnostics {
class GuardedReadSites {
public:
    static constexpr unsigned capacity=1024,probes=32,ringFrames=32,reportSites=32;
private:
    std::array<std::atomic<std::uint64_t>,capacity> pc0_{},pc1_{};
    std::array<std::atomic<std::uint64_t>,capacity> calls_{},renderCalls_{},bytes_{};
    std::atomic<std::uint64_t> total_{},startup_{},dropped_{},frames_{},worldCalls_{};
    std::atomic<std::int64_t> renderTid_{-1};
    std::array<std::atomic<std::uint32_t>,ringFrames> allPerFrame_{},renderPerFrame_{},directPerFrame_{},segmentsPerFrame_{};
    std::uint64_t lastTotal_{},lastRender_{},lastDirect_{},lastSegments_{};bool primed_{};
    std::atomic<bool> enabled_{};std::atomic<std::uint64_t> sampledFrames_{},enabledFrames_{};unsigned propertyAge_{};
    static bool property()noexcept{
#ifdef __ANDROID__
        char v[PROP_VALUE_MAX]{};return __system_property_get("debug.rdrvr.guardedsites",v)>0&&v[0]=='1'&&!v[1];
#else
        return false;
#endif
    }
    static std::int64_t currentTid()noexcept{
#ifdef __ANDROID__
        static thread_local std::int64_t tid=gettid();return tid;
#else
        static thread_local int marker{};return static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&marker));
#endif
    }
public:
    // Strip pointer-authentication/tag bits (user addresses are below 2^48 on Android arm64).
    static std::uint64_t strip(std::uint64_t pc)noexcept{return pc&0x0000ffffffffffffull;}
    // One relaxed load; callers skip the frame walk and note() when false.
    bool enabled()const noexcept{return enabled_.load(std::memory_order_relaxed);}
    void enable(bool on)noexcept{enabled_.store(on,std::memory_order_relaxed);}
    void note(std::uint64_t pc0,std::uint64_t pc1,std::size_t size)noexcept{
        total_.fetch_add(1,std::memory_order_relaxed);
        if(renderTid_.load(std::memory_order_relaxed)<0){startup_.fetch_add(1,std::memory_order_relaxed);return;}
        worldCalls_.fetch_add(1,std::memory_order_relaxed);
        const bool render=currentTid()==renderTid_.load(std::memory_order_relaxed);
        const std::uint64_t key=pc0*0x9e3779b97f4a7c15ull^pc1*0xc2b2ae3d27d4eb4full;
        const unsigned start=unsigned((key>>17)%capacity);
        for(unsigned i=0;i<probes;++i){
            const unsigned k=(start+i)%capacity;
            auto a=pc0_[k].load(std::memory_order_acquire);
            if(!a){std::uint64_t expected=0;
                if(pc0_[k].compare_exchange_strong(expected,pc0,std::memory_order_acq_rel)){pc1_[k].store(pc1,std::memory_order_release);a=pc0;}
                else a=expected;}
            if(a!=pc0)continue;
            auto b=pc1_[k].load(std::memory_order_acquire);
            if(b!=pc1){
                // A slot claimed for pc0 whose pc1 is not yet published: wait briefly, else probe on.
                for(unsigned spin=0;spin<64&&b==0&&pc1!=0;++spin)b=pc1_[k].load(std::memory_order_acquire);
                if(b!=pc1)continue;
            }
            calls_[k].fetch_add(1,std::memory_order_relaxed);if(render)renderCalls_[k].fetch_add(1,std::memory_order_relaxed);
            bytes_[k].fetch_add(size,std::memory_order_relaxed);return;
        }
        dropped_.fetch_add(1,std::memory_order_relaxed);
    }
    // Render thread, once per world frame, with that thread's cumulative counters:
    // calls = kernel-checked copies, direct = plain copies, segments = copies served by calls.
    void frame(std::uint64_t renderThreadCalls,std::uint64_t renderDirect=0,std::uint64_t renderSegments=0,bool refreshProperty=true)noexcept{
        if(refreshProperty&&(propertyAge_++&63)==0)enable(property());
        if(renderTid_.load(std::memory_order_relaxed)<0)renderTid_.store(currentTid(),std::memory_order_relaxed);
        const auto total=total_.load(std::memory_order_relaxed);
        if(primed_){
            const auto n=frames_.load(std::memory_order_relaxed);
            allPerFrame_[n%ringFrames].store(std::uint32_t(total-lastTotal_),std::memory_order_relaxed);
            renderPerFrame_[n%ringFrames].store(std::uint32_t(renderThreadCalls-lastRender_),std::memory_order_relaxed);
            directPerFrame_[n%ringFrames].store(std::uint32_t(renderDirect-lastDirect_),std::memory_order_relaxed);
            segmentsPerFrame_[n%ringFrames].store(std::uint32_t(renderSegments-lastSegments_),std::memory_order_relaxed);
            frames_.store(n+1,std::memory_order_release);
            if(enabled())enabledFrames_.fetch_add(1,std::memory_order_relaxed);
        }
        primed_=true;lastTotal_=total;lastRender_=renderThreadCalls;lastDirect_=renderDirect;lastSegments_=renderSegments;
    }
    std::uint64_t total()const noexcept{return total_.load(std::memory_order_relaxed);}
    // libraryBase: load address of this library (offsets are symbolized with offset-4).
    std::string report(std::uint64_t libraryBase)const{
        auto u=[](std::uint64_t v){return std::to_string(v);};
        const auto frames=frames_.load(std::memory_order_acquire);const auto n=frames<ringFrames?frames:ringFrames;
        double all=0,render=0,direct=0,segments=0;
        for(std::uint64_t i=0;i<n;++i){all+=allPerFrame_[i].load(std::memory_order_relaxed);render+=renderPerFrame_[i].load(std::memory_order_relaxed);
            direct+=directPerFrame_[i].load(std::memory_order_relaxed);segments+=segmentsPerFrame_[i].load(std::memory_order_relaxed);}
        char buf[64];
        auto offset=[&](std::uint64_t pc){if(!pc)return std::string("null");
            std::snprintf(buf,sizeof buf,"\"0x%llx\"",static_cast<unsigned long long>(pc>=libraryBase?pc-libraryBase:pc));return std::string(buf);};
        // Per-site rates over the frames during which the table was enabled.
        const auto siteFrames=enabledFrames_.load(std::memory_order_relaxed);
        auto perFrame=[&](std::uint64_t v){if(!siteFrames)return std::string("null");std::snprintf(buf,sizeof buf,"%.2f",double(v)/double(siteFrames));return std::string(buf);};
        std::string o="{\"total\":"+u(total_.load(std::memory_order_relaxed))+",\"startup\":"+u(startup_.load(std::memory_order_relaxed))+
            ",\"world\":"+u(worldCalls_.load(std::memory_order_relaxed))+",\"dropped\":"+u(dropped_.load(std::memory_order_relaxed))+
            ",\"frames\":"+u(frames)+",\"renderTid\":"+std::to_string(renderTid_.load(std::memory_order_relaxed));
        auto mean=[&](double sum){if(!n)return std::string("null");std::snprintf(buf,sizeof buf,"%.1f",sum/double(n));return std::string(buf);};
        o+=",\"sitesEnabled\":";o+=enabled()?"true":"false";o+=",\"sitesEnabledFrames\":"+u(enabledFrames_.load(std::memory_order_relaxed));
        // All-thread totals are counted by note(), i.e. only while the site table is enabled.
        o+=",\"guardedReadsPerFrame\":"+(enabledFrames_.load(std::memory_order_relaxed)?mean(all):std::string("null"));
        o+=",\"renderThreadPerFrame\":"+mean(render)+",\"renderThreadSegmentsPerFrame\":"+mean(segments)+",\"renderThreadDirectPerFrame\":"+mean(direct);
        // Top sites by render-thread calls, then all calls (steady state since the first world frame).
        struct Entry{std::uint64_t render,calls;unsigned k;};
        std::array<Entry,capacity> order{};unsigned used=0;
        for(unsigned k=0;k<capacity;++k)if(pc0_[k].load(std::memory_order_relaxed))
            order[used++]={renderCalls_[k].load(std::memory_order_relaxed),calls_[k].load(std::memory_order_relaxed),k};
        const unsigned shown=used<reportSites?used:reportSites;
        for(unsigned i=0;i<shown;++i)for(unsigned j=i+1;j<used;++j)
            if(order[j].render>order[i].render||(order[j].render==order[i].render&&order[j].calls>order[i].calls))std::swap(order[i],order[j]);
        o+=",\"sitesUsed\":"+u(used)+",\"sites\":[";
        for(unsigned i=0;i<shown;++i){
            const auto k=order[i].k;if(i)o+=',';
            o+="{\"offset\":"+offset(pc0_[k].load(std::memory_order_relaxed))+",\"callerOffset\":"+offset(pc1_[k].load(std::memory_order_relaxed));
            o+=",\"calls\":"+u(order[i].calls)+",\"renderCalls\":"+u(order[i].render)+",\"bytes\":"+u(bytes_[k].load(std::memory_order_relaxed));
            o+=",\"callsPerFrame\":"+perFrame(order[i].calls)+",\"renderCallsPerFrame\":"+perFrame(order[i].render)+'}';
        }
        o+="]}";return o;
    }
};
} // namespace rdrvr::diagnostics
