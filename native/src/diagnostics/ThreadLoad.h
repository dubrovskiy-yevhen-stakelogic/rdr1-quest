#pragma once
#include "ThreadCpuScope.h"
#include <mutex>
#include <atomic>

namespace rdrvr::diagnostics {
// Entire render-owner thread between scene entries, including work outside
// RenderScene. One fully scheduled thread is 100%; not a device-wide average.
class ThreadLoad {
public:
    struct Sample {std::uint64_t owner{},wall{},cpu{};};
    struct Snapshot {bool valid{};double percent{};std::uint64_t wallNs{},cpuNs{},stamp{};};
private:
    mutable std::mutex mutex_;
    Sample base_{},last_{};Snapshot latest_{};
public:
    void observe(Sample s) {
        std::lock_guard lock(mutex_);
        if(!s.owner||!s.wall||!s.cpu){base_={};last_={};latest_={};return;}
        if(s.owner!=last_.owner||s.wall<=last_.wall||s.cpu<last_.cpu||s.wall-last_.wall>2'000'000'000ull){
            base_=last_=s;latest_={};return;
        }
        last_=s;
        const auto wall=s.wall-base_.wall;
        if(wall<1'000'000'000ull)return;
        const auto cpu=s.cpu-base_.cpu;
        // A bad clock pair must not become a clipped, apparently valid 100%.
        latest_={cpu<=wall,100.*double(cpu)/double(wall),wall,cpu,s.wall};base_=s;
    }
    void sample() {
        if(!profilingEnabled())return;
        const int saved=errno;
        static std::atomic<std::uint64_t> nextOwner{};
        static thread_local const auto owner=++nextOwner;
        const auto cpu=threadCpuNs();observe({owner,monotonicNs(),cpu});errno=saved;
    }
    Snapshot snapshot(std::uint64_t now)const {
        std::lock_guard lock(mutex_);auto s=latest_;
        s.valid=s.valid&&now>=s.stamp&&now-s.stamp<=2'000'000'000ull;return s;
    }
    std::string report()const {
        const auto s=snapshot(monotonicNs());std::ostringstream o;
        o<<"{\"source\":\"render-owner-thread-cpu-between-scenes\",\"cpuPercentBasis\":\"one-thread\",\"currentValid\":"<<s.valid
         <<",\"intervalWallNs\":"<<s.wallNs<<",\"intervalCpuNs\":"<<s.cpuNs<<",\"stampNs\":"<<s.stamp;
        if(s.valid)o<<",\"percent\":"<<s.percent;
        return o.str()+'}';
    }
};
inline ThreadLoad renderThreadLoad;
}
