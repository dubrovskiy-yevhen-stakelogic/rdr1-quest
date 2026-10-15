#pragma once
#include "FrameTimings.h"
#if defined(__ANDROID__) || defined(__linux__)
#include <sys/resource.h>
#endif
namespace rdrvr::diagnostics {
// Process totals include all game/native/driver workers. 100% is one CPU core;
// this is deliberately distinct from OpenXR's device-wide average percentage.
class ProcessLoad {
public:
    struct Sample {std::uint64_t ns{},userNs{},systemNs{},peakRssKiB{},minorFaults{},majorFaults{};};
private:
    Sample previous_{},latest_{};bool valid_{};
    double userPercent_{},systemPercent_{};std::uint64_t minor_{},major_{};
public:
    void observe(const Sample& s){
        valid_=s.ns&&previous_.ns&&s.ns>previous_.ns&&s.userNs>=previous_.userNs&&s.systemNs>=previous_.systemNs&&
            s.minorFaults>=previous_.minorFaults&&s.majorFaults>=previous_.majorFaults;
        if(valid_){const auto elapsed=double(s.ns-previous_.ns);
            userPercent_=100.*double(s.userNs-previous_.userNs)/elapsed;systemPercent_=100.*double(s.systemNs-previous_.systemNs)/elapsed;
            minor_=s.minorFaults-previous_.minorFaults;major_=s.majorFaults-previous_.majorFaults;
        }
        latest_=s;previous_=s;
    }
    void sample()noexcept{
        const int saved=errno;Sample s{};
#if defined(__ANDROID__) || defined(__linux__)
        rusage usage{};
        if(getrusage(RUSAGE_SELF,&usage)==0&&usage.ru_utime.tv_sec>=0&&usage.ru_utime.tv_usec>=0&&usage.ru_stime.tv_sec>=0&&usage.ru_stime.tv_usec>=0&&
           usage.ru_maxrss>=0&&usage.ru_minflt>=0&&usage.ru_majflt>=0){
            s={monotonicNs(),std::uint64_t(usage.ru_utime.tv_sec)*1'000'000'000ull+std::uint64_t(usage.ru_utime.tv_usec)*1000,
                std::uint64_t(usage.ru_stime.tv_sec)*1'000'000'000ull+std::uint64_t(usage.ru_stime.tv_usec)*1000,
                std::uint64_t(usage.ru_maxrss),std::uint64_t(usage.ru_minflt),std::uint64_t(usage.ru_majflt)};
        }
#endif
        observe(s);errno=saved;
    }
    double cores()const{return valid_?(userPercent_+systemPercent_)/100.:-1.;}
    std::uint64_t peakRssMiB()const{return latest_.peakRssKiB/1024;}
    std::string report()const{
        std::ostringstream o;o<<"{\"source\":\"getrusage-self\",\"cpuPercentBasis\":\"one-core\",\"currentValid\":"<<valid_
            <<",\"peakRssKiB\":"<<latest_.peakRssKiB;
        if(valid_)o<<",\"userPercent\":"<<userPercent_<<",\"systemPercent\":"<<systemPercent_<<",\"cpuCores\":"<<cores()
                   <<",\"intervalMinorFaults\":"<<minor_<<",\"intervalMajorFaults\":"<<major_;
        return o.str()+'}';
    }
};
}
