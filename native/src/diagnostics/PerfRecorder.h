#pragma once
// PERF RECORD (menu HUD > PERF RECORD / MARK NOW; vr.cfg "perfRecord"): records a whole session to look at performance drops later.
// The flight recorder (FramePacing.h flight.json) keeps only 1 Hz means of the
// last 20 minutes, restarts with every launch (the next session overwrites it after 10 s) and has no context; this one logs every
// game frame of the whole session with what was happening, plus the moments the player marked.
//
// File: <files>/rdrvr/profiles/profile-YYYYMMDD-HHMMSS.csv (local time of the start; the 8 newest are kept, older ones are removed
// when a new one starts; one file is capped at maxFileBytes, then the row reads FULL). tools/perf_profile.py summarizes it.
// Mixed-row CSV; every row kind has a "#<kind>,..." column line in the header:
//   F  one row per game frame (render thread, after FramePacer::pace in gameStereoSubmitted): frame interval, render-thread busy /
//      paced sleep / CPU time, GPU lite span, the six eye stage wall times (EngineStereoPass, StageScope), main-thread frames / worst
//      frame / CPU since the previous row, XR frames / new pairs / repeats since the previous row, flags (AppSW synth, half rate,
//      menu, cutscene, dead eye, position valid, 64 = REGRID wait; bits 8-15 = world-pass rejects since the previous row, see
//      SkipBit), player foot state (2 = on foot, else mounted / vehicle / other), game state
//      (3 = in game, 5 = loading), held weapon class (engine::WeaponClass, -1 none) and the player position.
//   S  one row per second (writer thread): GPU clock / ceiling / temperature / thermal power level, up to 4 CPU cluster clocks /
//      ceilings, battery % / charging, process CPU % (of one core) and RSS, XR / stereo / new-pair / main-thread rates, the worst
//      main-thread frame of that second, rows dropped so far.
//   T  every 2 s (writer thread): the 6 busiest threads of the process, name:percent of one core.
//   M  a MARK NOW press: time of the press, the time the menu was opened for it (the drop the player saw is just before), mark number.
//   E  events: START, MENU_OPEN, MENU_CLOSE, END, FULL.
// Cost: nothing while OFF beyond one relaxed atomic load per hook. While ON: two clock reads per eye stage, two thread-CPU clock
// reads per frame (render + main), a ~170 byte row per frame into a mutex-guarded vector; formatting, file writes and the sysfs /
// procfs sampling run on one background thread (drained every 250 ms, flushed every drain: a killed process loses < 0.5 s).
// Threads: poll / xrFrame / mark on the XR thread, renderFrame + StageScope on the render thread, mainFrame on the engine main
// thread (sagShellDerived::UpdateInputs wrapper), configureDirectory once at startup. All state is heap-allocated and never freed
// (the detached writer may outlive static destruction).
#include "FrameTimings.h"
#include "ThreadCpuScope.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(__ANDROID__)||defined(__linux__)
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define RDRVR_PERFREC_POSIX 1
#endif

namespace rdrvr::diagnostics::perfrec {

inline constexpr std::uint64_t maxFileBytes=120ull*1024*1024; // ~3 h at 72 rows/s, ~6 h at 36
inline constexpr unsigned keepFiles=8;
inline constexpr std::size_t pendingCap=1u<<16;              // rows waiting for the writer (dropped beyond, counted)
enum Stage : unsigned {LeftDepth,LeftLight,LeftColor,RightDepth,RightLight,RightColor,StageCount};
enum Flag : std::uint32_t {FlagSynth=1u,FlagHalfRate=2u,FlagMenu=4u,FlagCutscene=8u,FlagDeadEye=16u,FlagPosValid=32u,FlagRegrid=64u};
// World-pass rejects (GameXr recordWorldReject) OR-ed into skipMask, exchanged into the next F row's flags (bits 8-15).
enum SkipBit : std::uint32_t {SkipWaitingStereoCull=1u<<8,SkipPreviousEyesPending=1u<<9,SkipCompletion=1u<<10,SkipFrozenTracking=1u<<11,
                              SkipCullSnapshot=1u<<12,SkipEarlyVisibilityRevoked=1u<<13,SkipWorldPassDeclined=1u<<14,SkipOther=1u<<15};
inline std::atomic<std::uint32_t> skipMask{}; // main / render thread (fetch_or), render thread (exchange)
enum State : int {StateOff=0,StateStarting=1,StateRecording=2,StateFull=3,StateNoDir=4,StateFileError=5};

// Mirrors "a file is open and rows are accepted" for the cheap hook checks.
inline std::atomic<bool> recording{false};
// Bumped (writer thread) every time `recording` turns true: renderFrame never bridges an OFF gap, even OFF -> ON inside one drain
// (same file, same t0).
inline std::atomic<std::uint32_t> recordEpoch{};
// render thread (StageScope), exchanged by renderFrame
inline std::array<std::atomic<std::uint64_t>,StageCount> stageNs{};
// main thread (mainFrame): monotonic totals + worst frame since the last F / S row
inline std::atomic<std::uint64_t> mainFrames{},mainCpuNs{};
inline std::atomic<std::uint32_t> mainMaxUsF{},mainMaxUsS{};
// XR thread (xrFrame): monotonic totals
inline std::atomic<std::uint64_t> xrFrames{},xrStereo{},xrNew{},xrRepeat{};
inline std::atomic<bool> xrSynth{};
inline std::atomic<std::uint64_t> lastMenuOpenNs{};

struct FrameContext {float x{},y{},z{};int foot{-1},gameState{-1},heldKind{-1};bool posValid{},menu{},cutscene{},deadEye{},halfRate{},regrid{};};

struct Row {
    char kind{};std::uint64_t tNs{};std::array<std::int64_t,22> v{};float x{},y{},z{};
};

inline void atomicMax(std::atomic<std::uint32_t>& m,std::uint32_t v) noexcept {
    auto c=m.load(std::memory_order_relaxed);while(v>c&&!m.compare_exchange_weak(c,v,std::memory_order_relaxed)){}}
inline std::uint32_t clampUs(std::uint64_t ns) noexcept {const auto us=ns/1000u;return us>0xffffffffull?0xffffffffu:std::uint32_t(us);}

class Recorder {
public:
    void configureDirectory(const std::string& rdrvrDir) {std::lock_guard lk(m_);dir_=rdrvrDir;}
    // XR thread, every frame: the effective PERF RECORD setting.
    void poll(bool wanted) noexcept {
        if(wanted==want_.load(std::memory_order_relaxed))return;
        want_.store(wanted,std::memory_order_release);
        if(wanted){
            state_.store(StateStarting,std::memory_order_relaxed);
            if(!threadStarted_){threadStarted_=true;try{std::thread([this]{run();}).detach();}catch(...){threadStarted_=false;state_.store(StateFileError,std::memory_order_relaxed);}}
        }else{recording.store(false,std::memory_order_release);state_.store(StateOff,std::memory_order_relaxed);}
        cv_.notify_one();
    }
    void push(const Row& r) noexcept {
        if(!recording.load(std::memory_order_acquire))return;
        std::lock_guard lk(m_);
        if(pending_.size()>=pendingCap){dropped_.fetch_add(1,std::memory_order_relaxed);return;}
        try{pending_.push_back(r);}catch(...){dropped_.fetch_add(1,std::memory_order_relaxed);}
    }
    std::uint64_t t0() const noexcept {return t0_.load(std::memory_order_acquire);}
    int state() const noexcept {return state_.load(std::memory_order_relaxed);}
    std::uint64_t bytes() const noexcept {return bytes_.load(std::memory_order_relaxed);}
    unsigned marks() const noexcept {return marks_.load(std::memory_order_relaxed);}
    unsigned nextMark() noexcept {return marks_.fetch_add(1,std::memory_order_relaxed)+1;}
    void resetMarks() noexcept {marks_.store(0,std::memory_order_relaxed);}
private:
    std::mutex m_;std::condition_variable cv_;std::vector<Row> pending_,work_;std::string dir_;
    bool threadStarted_{}; // XR thread only
    std::atomic<bool> want_{false};std::atomic<int> state_{StateOff};
    std::atomic<std::uint64_t> t0_{},bytes_{},dropped_{};std::atomic<unsigned> marks_{};
    // ---- writer thread ----
    std::FILE* file_{};bool failed_{};std::string text_;
    std::uint64_t nextSecond_{},nextThreads_{};
    std::uint64_t sXr_{},sStereo_{},sNew_{},sMain_{},sProcTicks_{},sProcNs_{};
    std::vector<int> policies_;
    struct ThreadTicks {int tid{};std::uint64_t ticks{};};std::vector<ThreadTicks> threads_;std::uint64_t threadsNs_{};
    static long long readLong(const std::string& path) noexcept {
#ifdef RDRVR_PERFREC_POSIX
        std::FILE* fp=std::fopen(path.c_str(),"r");long long v=-1;if(fp){if(std::fscanf(fp,"%lld",&v)!=1)v=-1;std::fclose(fp);}return v;
#else
        (void)path;return -1;
#endif
    }
    static std::string readText(const std::string& path,std::size_t cap) {
        std::string out;
#ifdef RDRVR_PERFREC_POSIX
        if(std::FILE* fp=std::fopen(path.c_str(),"r")){char b[512];std::size_t n;while(out.size()<cap&&(n=std::fread(b,1,sizeof b,fp))>0)out.append(b,n);std::fclose(fp);}
#else
        (void)path;(void)cap;
#endif
        return out;
    }
    void write(const char* s,std::size_t n) noexcept {
        if(!file_||!n)return;
        if(std::fwrite(s,1,n,file_)!=n)failed_=true;
        bytes_.fetch_add(n,std::memory_order_relaxed);
    }
    void writeText() noexcept {write(text_.data(),text_.size());text_.clear();}
    void line(const char* fmt,...) noexcept __attribute__((format(printf,2,3))) {
        char b[768];va_list a;va_start(a,fmt);const int n=std::vsnprintf(b,sizeof b,fmt,a);va_end(a);
        if(n>0)try{text_.append(b,std::size_t(std::min(n,int(sizeof b)-1)));if(n>=int(sizeof b))text_+='\n';}catch(...){} // cut: the row still ends
    }
    // Writer-side transition: only from the state the writer acted on, so a newer OFF / ON from poll() (XR thread) is never
    // overwritten (a plain store could turn a fresh STARTING into OFF and the recording would never start).
    bool moveState(int from,int to) noexcept {return state_.compare_exchange_strong(from,to,std::memory_order_acq_rel);}
    static void startRows() noexcept {recordEpoch.fetch_add(1,std::memory_order_relaxed);recording.store(true,std::memory_order_release);}
    double ms(std::uint64_t tNs) const noexcept {const auto t0=t0_.load(std::memory_order_relaxed);return tNs>t0?double(tNs-t0)/1e6:0.0;}
    // The 8 newest profile-*.csv stay (keepFiles - 1 before a new one is created).
    static void rotate(const std::string& dir) {
#ifdef RDRVR_PERFREC_POSIX
        std::vector<std::string> names;
        if(DIR* d=opendir(dir.c_str())){while(dirent* e=readdir(d)){const std::string n=e->d_name;
            if(n.size()>12&&n.compare(0,8,"profile-")==0&&n.compare(n.size()-4,4,".csv")==0){names.push_back(n);chmod((dir+"/"+n).c_str(),0664);}}closedir(d);}
        std::sort(names.begin(),names.end());
        while(names.size()+1>keepFiles){std::remove((dir+"/"+names.front()).c_str());names.erase(names.begin());}
#else
        (void)dir;
#endif
    }
    bool open() {
        std::string dir;{std::lock_guard lk(m_);dir=dir_;}
        if(dir.empty()){moveState(StateStarting,StateNoDir);return false;}
        const std::string pdir=dir+"/profiles";
#ifdef RDRVR_PERFREC_POSIX
        if(mkdir(pdir.c_str(),02770)!=0&&errno!=EEXIST){moveState(StateStarting,StateFileError);return false;}
        chmod(pdir.c_str(),0775); // adb (shell, group ext_data_rw) must read the profiles; 0700 would hide them
#endif
        try{rotate(pdir);}catch(...){}
        const std::time_t now=std::time(nullptr);std::tm tm{};
#ifdef RDRVR_PERFREC_POSIX
        localtime_r(&now,&tm);
#endif
        char stamp[32];std::strftime(stamp,sizeof stamp,"%Y%m%d-%H%M%S",&tm);
        std::string path=pdir+"/profile-"+stamp+".csv";
#ifdef RDRVR_PERFREC_POSIX
        for(int i=2;i<10;++i){struct stat st{};if(stat(path.c_str(),&st)!=0)break;path=pdir+"/profile-"+stamp+"-"+std::to_string(i)+".csv";}
#endif
        file_=std::fopen(path.c_str(),"wb");
        if(!file_){moveState(StateStarting,StateFileError);return false;}
#ifdef RDRVR_PERFREC_POSIX
        chmod(path.c_str(),0664); // Other-readable: the FUSE chmod drops the dir setgid, files then get the app group
#endif
        {std::lock_guard lk(m_);pending_.clear();} // a row pushed just after the previous close() must not land at t=0 here
        failed_=false;bytes_.store(0,std::memory_order_relaxed);dropped_.store(0,std::memory_order_relaxed);resetMarks();
        const auto t0=monotonicNs();t0_.store(t0,std::memory_order_release);
        char date[40];std::strftime(date,sizeof date,"%Y-%m-%d %H:%M:%S",&tm);
        line("# rdrvr perf profile v1\n# start_local=%s file=%s\n",date,path.c_str());
        // The settings in force at the start (key=value lines joined with ';').
        for(const char* cfg:{"graphics.cfg","vr.cfg"}){std::string t=readText(dir+"/"+cfg,4096);
            for(auto& c:t)if(c=='\n'||c=='\r')c=';';else if(c==',')c=' ';
            try{text_.append("# cfg ").append(cfg).append(" ").append(t).append("\n");}catch(...){}} // not line(): 768 B would cut it
        policies_.clear();
        for(int p=0;p<10&&policies_.size()<4;++p)if(readLong("/sys/devices/system/cpu/cpufreq/policy"+std::to_string(p)+"/cpuinfo_max_freq")>0)policies_.push_back(p);
        std::string pl;for(int p:policies_)pl+=(pl.empty()?"":" ")+std::to_string(p);
        line("# cpu_policies=%s\n",pl.c_str());
        line("#F,t_ms,frame_us,busy_us,slept_us,rcpu_us,gpu_us,ldepth_us,llight_us,lcolor_us,rdepth_us,rlight_us,rcolor_us,"
             "main_frames,main_max_us,main_cpu_us,xr,xr_stereo,xr_new,xr_rep,flags,foot,game_state,held,x,y,z\n");
        line("#S,t_ms,gpu_mhz,gpu_max_mhz,gpu_temp10,gpu_tpl,cpu0_mhz,cpu0_max,cpu1_mhz,cpu1_max,cpu2_mhz,cpu2_max,cpu3_mhz,cpu3_max,"
             "batt_pct,charging,proc_cpu_pct,rss_mb,xr_hz,stereo_hz,new_hz,main_hz,main_max_us,dropped\n");
        line("#T,t_ms,thread:pct...\n#M,t_ms,menu_open_t_ms,mark\n#E,t_ms,what\n");
        line("E,0.0,START\n");
        writeText();if(file_)std::fflush(file_);
        sXr_=xrFrames.load();sStereo_=xrStereo.load();sNew_=xrNew.load();sMain_=mainFrames.load();sProcTicks_=procTicks();sProcNs_=t0;
        threads_.clear();threadsNs_=0;sampleThreads(t0,false);
        nextSecond_=t0+1'000'000'000ull;nextThreads_=t0+2'000'000'000ull;mainMaxUsS.store(0,std::memory_order_relaxed);
        if(moveState(StateStarting,StateRecording))startRows(); // else OFF came meanwhile: the next loop closes the file
        return true;
    }
    void close(const char* why) noexcept {
        recording.store(false,std::memory_order_release);
        if(!file_)return;
        drain();
        line("E,%.1f,%s\n",ms(monotonicNs()),why);writeText();
        std::fclose(file_);file_=nullptr;
    }
    static std::uint64_t procTicks() noexcept {
        const std::string s=readText("/proc/self/stat",1024);const auto r=s.rfind(')');if(r==std::string::npos)return 0;
        // fields after ')': 3 state ... 14 utime 15 stime
        unsigned long long ut=0,st=0;
        if(std::sscanf(s.c_str()+r+1," %*c %*d %*d %*d %*d %*d %*u %*llu %*llu %*llu %*llu %llu %llu",&ut,&st)!=2)return 0;
        return ut+st;
    }
    static long ticksPerSecond() noexcept {
#ifdef RDRVR_PERFREC_POSIX
        const long t=sysconf(_SC_CLK_TCK);return t>0?t:100;
#else
        return 100;
#endif
    }
    void sampleThreads(std::uint64_t now,bool emit) {
#ifdef RDRVR_PERFREC_POSIX
        struct Busy {std::uint64_t d{};std::string name;};std::vector<Busy> top;std::vector<ThreadTicks> next;
        if(DIR* d=opendir("/proc/self/task")){
            while(dirent* e=readdir(d)){
                if(e->d_name[0]<'0'||e->d_name[0]>'9')continue;
                const int tid=std::atoi(e->d_name);
                const std::string s=readText(std::string("/proc/self/task/")+e->d_name+"/stat",1024);
                const auto l=s.find('('),r=s.rfind(')');if(l==std::string::npos||r==std::string::npos||r<l)continue;
                unsigned long long ut=0,st=0;
                if(std::sscanf(s.c_str()+r+1," %*c %*d %*d %*d %*d %*d %*u %*llu %*llu %*llu %*llu %llu %llu",&ut,&st)!=2)continue;
                const std::uint64_t ticks=ut+st;next.push_back({tid,ticks});
                if(!emit)continue;
                std::uint64_t before=ticks;for(const auto& t:threads_)if(t.tid==tid){before=t.ticks;break;}
                if(ticks>before){std::string n=s.substr(l+1,r-l-1);for(auto& c:n)if(c==','||c==':'||c=='\n'||c==' ')c='_';top.push_back({ticks-before,n});}
            }
            closedir(d);
        }
        if(emit&&threadsNs_&&now>threadsNs_){
            std::sort(top.begin(),top.end(),[](const Busy& a,const Busy& b){return a.d>b.d;});
            const double secs=double(now-threadsNs_)/1e9,tps=double(ticksPerSecond());
            std::string o;char b[96];
            for(std::size_t i=0;i<top.size()&&i<6;++i){std::snprintf(b,sizeof b,",%s:%.0f",top[i].name.c_str(),100.0*double(top[i].d)/tps/secs);o+=b;}
            line("T,%.1f%s\n",ms(now),o.c_str());
        }
        threads_.swap(next);threadsNs_=now;
#else
        (void)now;(void)emit;
#endif
    }
    void second(std::uint64_t now) {
        const auto x=xrFrames.load(),st=xrStereo.load(),nw=xrNew.load(),mf=mainFrames.load();
        const double secs=sProcNs_&&now>sProcNs_?double(now-sProcNs_)/1e9:1.0;
        const auto pt=procTicks();const double procPct=pt>=sProcTicks_?100.0*double(pt-sProcTicks_)/double(ticksPerSecond())/secs:0.0;
        long long cur[4]{-1,-1,-1,-1},mx[4]{-1,-1,-1,-1};
        for(std::size_t i=0;i<policies_.size()&&i<4;++i){const std::string b="/sys/devices/system/cpu/cpufreq/policy"+std::to_string(policies_[i]);
            const auto c=readLong(b+"/scaling_cur_freq"),m=readLong(b+"/scaling_max_freq");cur[i]=c>0?c/1000:-1;mx[i]=m>0?m/1000:-1;}
        const auto gclk=readLong("/sys/class/kgsl/kgsl-3d0/gpuclk"),gmax=readLong("/sys/class/kgsl/kgsl-3d0/devfreq/max_freq");
        const auto gtemp=readLong("/sys/class/kgsl/kgsl-3d0/temp"),tpl=readLong("/sys/class/kgsl/kgsl-3d0/thermal_pwrlevel");
        const auto batt=readLong("/sys/class/power_supply/battery/capacity");
        const std::string bs=readText("/sys/class/power_supply/battery/status",32);
        const int charging=bs.empty()?-1:(bs[0]=='C'||bs[0]=='F')?1:0; // Charging / Full
        long long rssMb=-1;{const std::string sm=readText("/proc/self/statm",128);unsigned long long size=0,res=0;
#ifdef RDRVR_PERFREC_POSIX
            if(std::sscanf(sm.c_str(),"%llu %llu",&size,&res)==2)rssMb=(long long)(res*std::uint64_t(sysconf(_SC_PAGESIZE))/(1024*1024));
#else
            (void)size;(void)res;
#endif
        }
        line("S,%.1f,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d,%.0f,%lld,%.1f,%.1f,%.1f,%.1f,%u,%llu\n",ms(now),
             gclk>0?gclk/1'000'000:-1,gmax>0?gmax/1'000'000:-1,gtemp>0?gtemp/100:-1,tpl,cur[0],mx[0],cur[1],mx[1],cur[2],mx[2],cur[3],mx[3],
             batt,charging,procPct,rssMb,double(x-sXr_)/secs,double(st-sStereo_)/secs,double(nw-sNew_)/secs,double(mf-sMain_)/secs,
             mainMaxUsS.exchange(0,std::memory_order_relaxed),(unsigned long long)dropped_.load(std::memory_order_relaxed));
        sXr_=x;sStereo_=st;sNew_=nw;sMain_=mf;sProcTicks_=pt;sProcNs_=now;
    }
    void format(const Row& r) {
        switch(r.kind){
        case 'F':{
            char b[400];int n=std::snprintf(b,sizeof b,"F,%.2f",ms(r.tNs));
            for(const auto v:r.v)if(n>0&&n<int(sizeof b))n+=std::snprintf(b+n,sizeof b-std::size_t(n),",%lld",(long long)v);
            if(n>0&&n<int(sizeof b))n+=std::snprintf(b+n,sizeof b-std::size_t(n),",%.1f,%.1f,%.1f\n",double(r.x),double(r.y),double(r.z));
            if(n>0&&n<int(sizeof b))text_.append(b,std::size_t(n));
            break;}
        case 'M':line("M,%.1f,%.1f,%lld\n",ms(r.tNs),ms(std::uint64_t(r.v[0])),(long long)r.v[1]);break;
        case 'O':line("E,%.1f,MENU_OPEN\n",ms(r.tNs));break;
        case 'C':line("E,%.1f,MENU_CLOSE\n",ms(r.tNs));break;
        default:break;
        }
    }
    void drain() noexcept {
        {std::lock_guard lk(m_);work_.swap(pending_);}
        try{for(const auto& r:work_){format(r);if(text_.size()>=32768)writeText();}}catch(...){}
        work_.clear();writeText();
    }
    void run() noexcept {
        try{pending_.reserve(4096);work_.reserve(4096);text_.reserve(65536);}catch(...){}
#ifdef RDRVR_PERFREC_POSIX
        pthread_setname_np(pthread_self(),"rdrvr-perfrec");
#endif
        for(;;){
            {std::unique_lock lk(m_);cv_.wait_for(lk,std::chrono::milliseconds(250));}
            const bool want=want_.load(std::memory_order_acquire);
            try{
                if(!want){close("END");continue;} // poll(false) already set OFF
                if(file_&&moveState(StateStarting,StateRecording))startRows(); // OFF -> ON within one drain: the same file goes on
                if(!file_){if(state_.load(std::memory_order_relaxed)!=StateStarting)continue; // FULL / error: wait for OFF -> ON
                    if(!open())continue;}
                drain();
                const auto now=monotonicNs();
                if(now>=nextSecond_){second(now);nextSecond_+=1'000'000'000ull;if(now>=nextSecond_)nextSecond_=now+1'000'000'000ull;}
                if(now>=nextThreads_){sampleThreads(now,true);nextThreads_=now+2'000'000'000ull;}
                writeText();
                if(file_&&std::fflush(file_)!=0)failed_=true;
                if(failed_){close("WRITE_ERROR");moveState(StateRecording,StateFileError);} // STARTING meanwhile = a new file next loop
                else if(bytes_.load(std::memory_order_relaxed)>=maxFileBytes){close("FULL");moveState(StateRecording,StateFull);}
            }catch(...){}
        }
    }
};
inline Recorder& recorder() {static Recorder* r=new Recorder;return *r;}

// ---- hooks ----
// XR thread, every xrEndFrame: the PERF RECORD setting, XR frame counts, the menu open edge (MARK NOW is backdated to it).
inline void xrFrame(bool wanted,bool stereo,bool newPair,bool repeat,bool synth,bool menuOpen) noexcept {
    auto& r=recorder();r.poll(wanted);
    static bool menuWas=false; // XR thread only
    const bool edge=menuOpen!=menuWas;menuWas=menuOpen;
    const auto edgeNs=edge?monotonicNs():0;
    if(edge&&menuOpen)lastMenuOpenNs.store(edgeNs,std::memory_order_relaxed); // also while OFF: PERF RECORD turned ON in this menu visit
    if(!recording.load(std::memory_order_acquire))return;
    xrFrames.fetch_add(1,std::memory_order_relaxed);if(stereo)xrStereo.fetch_add(1,std::memory_order_relaxed);
    if(newPair)xrNew.fetch_add(1,std::memory_order_relaxed);if(repeat)xrRepeat.fetch_add(1,std::memory_order_relaxed);
    xrSynth.store(synth,std::memory_order_relaxed);
    if(edge){Row e{};e.kind=menuOpen?'O':'C';e.tNs=edgeNs;r.push(e);}
}
// XR thread (menu MARK NOW): stamps the moment; the menu open time is the closer estimate of when the drop was seen.
inline bool mark() noexcept {
    if(!recording.load(std::memory_order_acquire))return false;
    auto& r=recorder();Row m{};m.kind='M';m.tNs=monotonicNs();
    const auto open=lastMenuOpenNs.load(std::memory_order_relaxed);
    m.v[0]=std::int64_t(open&&open<=m.tNs&&m.tNs-open<120'000'000'000ull?open:m.tNs);m.v[1]=r.nextMark();r.push(m);return true;
}
// Engine main thread, once per game update (the qualified UpdateInputs call).
inline void mainFrame() noexcept {
    static std::uint64_t prevWall=0,prevCpu=0; // main thread only
    if(!recording.load(std::memory_order_relaxed)){prevWall=0;return;}
    const auto now=monotonicNs(),cpu=threadCpuNs();
    if(prevWall&&now>prevWall&&now-prevWall<5'000'000'000ull){
        const auto us=clampUs(now-prevWall);mainFrames.fetch_add(1,std::memory_order_relaxed);
        if(cpu>=prevCpu)mainCpuNs.fetch_add(cpu-prevCpu,std::memory_order_relaxed);
        atomicMax(mainMaxUsF,us);atomicMax(mainMaxUsS,us);}
    prevWall=now;prevCpu=cpu;
}
// Render thread, eye stage of the stereo pass (EngineStereoPass): wall time added to this frame's stage total.
class StageScope {
    unsigned index_;std::uint64_t start_{};
public:
    StageScope(unsigned eye,unsigned phase) noexcept:index_(std::min(eye,1u)*3u+std::min(phase,2u)){if(recording.load(std::memory_order_relaxed))start_=monotonicNs();}
    StageScope(const StageScope&)=delete;StageScope& operator=(const StageScope&)=delete;
    ~StageScope(){if(start_){const auto now=monotonicNs();if(now>start_)stageNs[index_].fetch_add(now-start_,std::memory_order_relaxed);}}
};
// Render thread, after FramePacer::pace of a stereo frame. paceBegin / paceEnd = monotonicNs() around pace().
inline void renderFrame(std::uint64_t paceBegin,std::uint64_t paceEnd,std::uint64_t gpuSerial,std::uint32_t gpuUs,const FrameContext& c) noexcept {
    struct Prev {std::uint32_t epoch{};std::uint64_t begin{},end{},cpu{},serial{},main{},mainCpu{},xr{},stereo{},fresh{},repeat{};};
    static Prev p{}; // render thread only
    const std::uint32_t skips=skipMask.exchange(0,std::memory_order_relaxed)&0xff00u; // always drained: no stale bits across an OFF gap
    if(!recording.load(std::memory_order_acquire)){p.begin=0;return;}
    const auto cpu=threadCpuNs();
    const auto mf=mainFrames.load(std::memory_order_relaxed),mc=mainCpuNs.load(std::memory_order_relaxed);
    const auto x=xrFrames.load(std::memory_order_relaxed),xs=xrStereo.load(std::memory_order_relaxed),
               xn=xrNew.load(std::memory_order_relaxed),xp=xrRepeat.load(std::memory_order_relaxed);
    std::array<std::uint64_t,StageCount> st{};for(unsigned i=0;i<StageCount;++i)st[i]=stageNs[i].exchange(0,std::memory_order_relaxed);
    const auto mainMax=mainMaxUsF.exchange(0,std::memory_order_relaxed);
    const auto epoch=recordEpoch.load(std::memory_order_relaxed); // a new recording (or OFF -> ON) never bridges the gap
    const bool first=p.epoch!=epoch||!p.begin||paceBegin<=p.begin||paceBegin-p.begin>5'000'000'000ull;
    if(!first){
        Row r{};r.kind='F';r.tNs=paceEnd;
        r.v[0]=clampUs(paceBegin-p.begin);r.v[1]=clampUs(paceBegin>p.end?paceBegin-p.end:0);r.v[2]=clampUs(paceEnd>paceBegin?paceEnd-paceBegin:0);
        r.v[3]=clampUs(cpu>=p.cpu?cpu-p.cpu:0);r.v[4]=gpuSerial!=p.serial?std::int64_t(gpuUs):0;
        for(unsigned i=0;i<StageCount;++i)r.v[5+i]=clampUs(st[i]);
        r.v[11]=std::int64_t(mf-p.main);r.v[12]=mainMax;r.v[13]=clampUs(mc>=p.mainCpu?mc-p.mainCpu:0);
        r.v[14]=std::int64_t(x-p.xr);r.v[15]=std::int64_t(xs-p.stereo);r.v[16]=std::int64_t(xn-p.fresh);r.v[17]=std::int64_t(xp-p.repeat);
        std::uint32_t f=0;if(xrSynth.load(std::memory_order_relaxed))f|=FlagSynth;if(c.halfRate)f|=FlagHalfRate;if(c.menu)f|=FlagMenu;
        if(c.cutscene)f|=FlagCutscene;if(c.deadEye)f|=FlagDeadEye;if(c.posValid)f|=FlagPosValid;if(c.regrid)f|=FlagRegrid;f|=skips;
        r.v[18]=f;r.v[19]=c.foot;r.v[20]=c.gameState;r.v[21]=c.heldKind;r.x=c.x;r.y=c.y;r.z=c.z;
        recorder().push(r);
    }
    p.epoch=epoch;p.begin=paceBegin;p.end=paceEnd;p.cpu=cpu;p.serial=gpuSerial;p.main=mf;p.mainCpu=mc;p.xr=x;p.stereo=xs;p.fresh=xn;p.repeat=xp;
}
// Menu text (XR thread): " - REC 12 MB", " - FULL", ...
inline void statusSuffix(char* out,std::size_t size) noexcept {
    if(!size)return;out[0]=0;auto& r=recorder();
    switch(r.state()){
    case StateStarting:std::snprintf(out,size," - STARTING");break;
    case StateRecording:std::snprintf(out,size," - REC %llu MB",(unsigned long long)(r.bytes()/(1024*1024)));break;
    case StateFull:std::snprintf(out,size," - FULL");break;
    case StateNoDir:std::snprintf(out,size," - NO FOLDER");break;
    case StateFileError:std::snprintf(out,size," - FILE ERROR");break;
    default:break;
    }
}
} // namespace rdrvr::diagnostics::perfrec
