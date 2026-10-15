#pragma once
// Measurement pack: in-app kgsl GPU clock / thermal reader.
// XrPerformance::pump calls sample() at 1 Hz on the XR thread (half a second after the META poll) while debug.rdrvr.perfmetrics
// is not 0. One sample = open/read/close of 7 small sysfs files into one stack buffer; no allocation per sample.
// open() failing with EACCES/EPERM (SELinux, untrusted_app) = DENIED, any other open error = MISSING; both are sticky (never
// retried: no audit spam). A read/parse failure = BAD (counted, retried next second). Files (/sys/class/kgsl/kgsl-3d0/):
//   gpuclk (Hz), gpu_busy_percentage ("84 %"), thermal_pwrlevel (0 = fastest level allowed), devfreq/max_freq, devfreq/min_freq
//   (Hz), temp (mC), gpu_clock_stats (accumulated BUSY us per power level, level 0 = freq_table_mhz[0]; read once).
// Windows = the last `window` samples. busyWeightedMHz = sum(MHz*busy%)/sum(busy%) of the 1 Hz point samples;
// clockStats.window* = the exact busy-time-weighted clock from the gpu_clock_stats delta over the same window
// (the exact counterpart of busyWeightedMHz).
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <sstream>
#include <string>
#if defined(__ANDROID__) || defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif
namespace rdrvr::runtime {
// debug.rdrvr.perfmetrics in effect, re-read by XrPerformance::pump at most once per second. 1 (default) = measurement
// pack on: performance block + core META counters + kgsl at 1 Hz in every profiler mode, StereoMailbox latch classification and
// xrHistory flag 16 (NewerNotReady). 0 = DETAIL-only poll/publish, no kgsl, no latch stats, no flag.
inline std::atomic<int> measurementPack{1};
class KgslClock {
public:
    enum File : unsigned {GpuClk,Busy,ThermalPwrlevel,DevfreqMax,DevfreqMin,Temp,ClockStats,FreqTable,FileCount};
    enum Status : std::uint8_t {Untried,Ok,Denied,Missing,Bad};
    static constexpr unsigned window=32,maxLevels=16;
    struct Reading { // one sample (sample() fills it from sysfs; host tests fill it directly)
        std::array<bool,FileCount> valid{};
        std::int64_t gpuHz{},busyPct{},pwrlevel{},maxHz{},minHz{},tempMilliC{};
        std::array<std::uint64_t,maxLevels> stats{};unsigned statsCount{};
    };
    static constexpr const char* path(unsigned f) noexcept {
        constexpr const char* p[FileCount]{"/sys/class/kgsl/kgsl-3d0/gpuclk","/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage","/sys/class/kgsl/kgsl-3d0/thermal_pwrlevel",
            "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq","/sys/class/kgsl/kgsl-3d0/devfreq/min_freq","/sys/class/kgsl/kgsl-3d0/temp",
            "/sys/class/kgsl/kgsl-3d0/gpu_clock_stats","/sys/class/kgsl/kgsl-3d0/freq_table_mhz"};
        return f<FileCount?p[f]:"";
    }
    static constexpr const char* name(unsigned f) noexcept {
        constexpr const char* n[FileCount]{"gpuclk","gpu_busy_percentage","thermal_pwrlevel","devfreq/max_freq","devfreq/min_freq","temp","gpu_clock_stats","freq_table_mhz"};
        return f<FileCount?n[f]:"";
    }
    static constexpr const char* statusName(Status s) noexcept {
        switch(s){case Ok:return "OK";case Denied:return "DENIED";case Missing:return "MISSING";case Bad:return "BAD";default:return "UNTRIED";}
    }
    // First (optionally negative) decimal integer in text, after leading blanks; false if there is none.
    static bool parseInt(const char* t,std::int64_t& out) noexcept {
        while(*t==' '||*t=='\t'||*t=='\n')++t;
        bool neg=false;if(*t=='-'){neg=true;++t;}
        if(*t<'0'||*t>'9')return false;
        std::int64_t v=0;for(int d=0;*t>='0'&&*t<='9';++t)if(++d<=18)v=v*10+(*t-'0');
        out=neg?-v:v;return true;
    }
    // Up to maxLevels unsigned decimal integers separated by blanks/newlines; stops at any other character.
    static unsigned parseList(const char* t,std::array<std::uint64_t,maxLevels>& out) noexcept {
        unsigned n=0;
        while(n<maxLevels){
            while(*t==' '||*t=='\t'||*t=='\n')++t;
            if(*t<'0'||*t>'9')break;
            std::uint64_t v=0;for(int d=0;*t>='0'&&*t<='9';++t)if(++d<=19)v=v*10+std::uint64_t(*t-'0');
            out[n++]=v;
        }
        return n;
    }
    // XR thread, 1 Hz. Never blocks beyond the sysfs reads; errno preserved.
    void sample(std::uint64_t nowNs) noexcept {
        const int saved=errno;Reading r{};char buf[256];
        for(unsigned f=0;f<FileCount;++f){
            if(f==FreqTable&&levels_)continue; // read once
            if(status_[f]==Denied||status_[f]==Missing){r.valid[f]=false;continue;}
            const Status s=readFile(f,buf,sizeof buf);
            if(s!=Ok){status_[f]=s;++failures_[f];continue;}
            bool ok=false;std::int64_t v{};
            switch(f){
            case GpuClk:ok=parseInt(buf,v)&&v>0;r.gpuHz=v;break;
            case Busy:ok=parseInt(buf,v)&&v>=0&&v<=100;r.busyPct=v;break;
            case ThermalPwrlevel:ok=parseInt(buf,v)&&v>=0;r.pwrlevel=v;break;
            case DevfreqMax:ok=parseInt(buf,v)&&v>0;r.maxHz=v;break;
            case DevfreqMin:ok=parseInt(buf,v)&&v>0;r.minHz=v;break;
            case Temp:ok=parseInt(buf,v);r.tempMilliC=v;break;
            case ClockStats:r.statsCount=parseList(buf,r.stats);ok=r.statsCount>0;break;
            case FreqTable:{std::array<std::uint64_t,maxLevels> t{};const unsigned n=parseList(buf,t);ok=n>0;if(ok)setLevels(t,n);break;}
            default:break;
            }
            status_[f]=ok?Ok:Bad;if(!ok)++failures_[f];r.valid[f]=ok;
        }
        observe(r,nowNs);errno=saved;
    }
    void setLevels(const std::array<std::uint64_t,maxLevels>& mhz,unsigned n) noexcept {
        levels_=std::min(n,maxLevels);for(unsigned i=0;i<levels_;++i)levelsMHz_[i]=mhz[i];
    }
    // Pure: fold one reading into the windows (host-testable).
    void observe(const Reading& r,std::uint64_t nowNs) noexcept {
        ++samples_;lastNs_=nowNs;if(!firstNs_)firstNs_=nowNs;
        Point p{};p.ns=nowNs;
        if(r.valid[GpuClk])p.mhz=double(r.gpuHz)/1e6;
        if(r.valid[Busy])p.busy=double(r.busyPct);
        if(r.valid[Temp])p.tempC=double(r.tempMilliC)/(r.tempMilliC>1000||r.tempMilliC<-1000?1000.:1.); // mC (kgsl) or C
        if(r.valid[ThermalPwrlevel])p.pwrlevel=int(r.pwrlevel);
        if(r.valid[DevfreqMax])p.maxMHz=int(r.maxHz/1'000'000);
        if(r.valid[DevfreqMin])p.minMHz=int(r.minHz/1'000'000);
        if(r.valid[ClockStats]&&r.statsCount){
            p.statsValid=true;p.stats=r.stats;p.statsCount=r.statsCount;
            bool reset=statsHave_&&r.statsCount!=statsFirstCount_;
            for(unsigned i=0;!reset&&statsHave_&&i<r.statsCount;++i)reset=r.stats[i]<statsLast_[i];
            if(!statsHave_||reset){statsFirst_=r.stats;statsFirstCount_=r.statsCount;statsFirstNs_=nowNs;statsHave_=true;if(reset)++statsResets_;}
            statsLast_=r.stats;statsLastNs_=nowNs;
        }
        ring_[next_]=p;next_=(next_+1)%window;if(n_<window)++n_;
    }
    std::uint64_t samples() const noexcept {return samples_;}
    Status status(unsigned f) const noexcept {return f<FileCount?status_[f]:Untried;}
    // "OK" all per-sample files readable; "DENIED" gpuclk denied (SELinux); "PARTIAL" some file missing/denied; "IDLE" never sampled.
    const char* overall() const noexcept {
        if(!samples_)return "IDLE";
        if(status_[GpuClk]==Denied)return "DENIED";
        for(unsigned f=0;f<FreqTable;++f)if(status_[f]!=Ok)return status_[GpuClk]==Ok?"PARTIAL":"MISSING";
        return "OK";
    }
    // Window means for host tests / the report (-1 when no valid sample).
    double busyWeightedMHz() const noexcept {
        double w=0,s=0;for(unsigned i=0;i<n_;++i){const auto& p=ring_[i];if(p.mhz>0&&p.busy>=0){s+=p.mhz*p.busy;w+=p.busy;}}
        return w>0?s/w:-1;
    }
    // Exact busy-time-weighted MHz from two gpu_clock_stats snapshots (needs freq_table_mhz); -1 if unknown.
    double statsWeightedMHz(const std::array<std::uint64_t,maxLevels>& a,const std::array<std::uint64_t,maxLevels>& b,unsigned count,double* busyUs=nullptr) const noexcept {
        double s=0,t=0;for(unsigned i=0;i<count&&i<levels_;++i){const double d=b[i]>a[i]?double(b[i]-a[i]):0.;s+=d*double(levelsMHz_[i]);t+=d;}
        if(busyUs)*busyUs=t;return t>0?s/t:-1;
    }
    std::string report(std::uint64_t nowNs) const {
        std::ostringstream o;o<<"{\"status\":\""<<overall()<<"\",\"samples\":"<<samples_<<",\"lastAgeMs\":"<<(lastNs_&&nowNs>=lastNs_?double(nowNs-lastNs_)/1e6:-1.)
         <<",\"windowSamples\":"<<n_<<",\"files\":{";
        for(unsigned f=0;f<FileCount;++f)o<<(f?",":"")<<'"'<<name(f)<<"\":{\"status\":\""<<statusName(status_[f])<<"\",\"failures\":"<<failures_[f]<<'}';
        o<<'}';
        const unsigned first=(next_+window-n_)%window;
        auto stat=[&](const char* key,auto get,bool percentiles){
            std::array<double,window> v{};unsigned k=0;double sum=0,last=-1;
            for(unsigned i=0;i<n_;++i){const double x=get(ring_[(first+i)%window]);if(x<=-1e9)continue;v[k++]=x;sum+=x;last=x;}
            o<<",\""<<key<<"\":{\"samples\":"<<k;
            if(k){std::sort(v.begin(),v.begin()+k);o<<",\"last\":"<<last<<",\"min\":"<<v[0]<<",\"max\":"<<v[k-1]<<",\"mean\":"<<sum/k;
                if(percentiles)o<<",\"p50\":"<<v[(k*50+99)/100-1]<<",\"p95\":"<<v[(k*95+99)/100-1];}
            o<<'}';
        };
        stat("gpuMHz",[](const Point& p){return p.mhz>0?p.mhz:-2e9;},true);
        stat("busyPct",[](const Point& p){return p.busy>=0?p.busy:-2e9;},true);
        stat("tempC",[](const Point& p){return p.tempC;},true);
        stat("thermalPwrlevel",[](const Point& p){return p.pwrlevel>=0?double(p.pwrlevel):-2e9;},false);
        stat("devfreqMaxMHz",[](const Point& p){return p.maxMHz>0?double(p.maxMHz):-2e9;},false);
        stat("devfreqMinMHz",[](const Point& p){return p.minMHz>0?double(p.minMHz):-2e9;},false);
        o<<",\"busyWeightedMHz\":"<<busyWeightedMHz();
        // gpu_clock_stats: exact busy-time histogram over the window (oldest valid -> newest valid entry) and the session.
        o<<",\"clockStats\":{\"levelsMHz\":[";for(unsigned i=0;i<levels_;++i)o<<(i?",":"")<<levelsMHz_[i];o<<"],\"resets\":"<<statsResets_;
        int a=-1,b=-1;for(unsigned i=0;i<n_;++i){const unsigned j=(first+i)%window;if(!ring_[j].statsValid)continue;if(a<0)a=int(j);b=int(j);}
        if(a>=0&&b>=0&&a!=b&&ring_[a].statsCount==ring_[b].statsCount){
            const auto& pa=ring_[a];const auto& pb=ring_[b];double busy=0;const double mhz=statsWeightedMHz(pa.stats,pb.stats,pb.statsCount,&busy);
            double raw=0;o<<",\"windowBusyMsByLevel\":[";for(unsigned i=0;i<pb.statsCount;++i){const double d=pb.stats[i]>pa.stats[i]?double(pb.stats[i]-pa.stats[i]):0.;raw+=d;o<<(i?",":"")<<d/1000.;}
            const double wall=double(pb.ns-pa.ns)/1e3;
            o<<"],\"windowSeconds\":"<<wall/1e6<<",\"windowBusyWeightedMHz\":"<<mhz<<",\"windowBusyPctOfWall\":"<<(wall>0?100.*raw/wall:-1.);
        }
        if(statsHave_&&statsLastNs_>statsFirstNs_){
            double busy=0;const double mhz=statsWeightedMHz(statsFirst_,statsLast_,statsFirstCount_,&busy);double raw=0;
            for(unsigned i=0;i<statsFirstCount_;++i)raw+=statsLast_[i]>statsFirst_[i]?double(statsLast_[i]-statsFirst_[i]):0.;
            const double wall=double(statsLastNs_-statsFirstNs_)/1e3;
            o<<",\"sessionSeconds\":"<<wall/1e6<<",\"sessionBusyWeightedMHz\":"<<mhz<<",\"sessionBusyPctOfWall\":"<<(wall>0?100.*raw/wall:-1.);
        }
        o<<"}}";return o.str();
    }
private:
    struct Point {
        std::uint64_t ns{};double mhz{-1},busy{-1},tempC{-2e9};int pwrlevel{-1},maxMHz{-1},minMHz{-1};
        std::array<std::uint64_t,maxLevels> stats{};unsigned statsCount{};bool statsValid{};
    };
    static Status readFile([[maybe_unused]] unsigned f,[[maybe_unused]] char* buf,[[maybe_unused]] unsigned size) noexcept {
#if defined(__ANDROID__) || defined(__linux__)
        const int fd=::open(path(f),O_RDONLY|O_CLOEXEC);
        if(fd<0){const int e=errno;return e==EACCES||e==EPERM?Denied:Missing;}
        const auto got=::read(fd,buf,size-1);const int e=errno;::close(fd);
        if(got<=0)return got<0&&(e==EACCES||e==EPERM)?Denied:Bad;
        buf[got]=0;return Ok;
#else
        return Missing;
#endif
    }
    std::array<Status,FileCount> status_{};std::array<std::uint32_t,FileCount> failures_{};
    std::array<Point,window> ring_{};unsigned n_{},next_{};
    std::uint64_t samples_{},firstNs_{},lastNs_{};
    std::array<std::uint64_t,maxLevels> levelsMHz_{},statsFirst_{},statsLast_{};unsigned levels_{},statsFirstCount_{};
    std::uint64_t statsFirstNs_{},statsLastNs_{},statsResets_{};bool statsHave_{};
};
}
