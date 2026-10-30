#pragma once
// SAVE LOG: the ROOT menu row
// writes the full report (the same JSON as vulkan-trace.json) to <files>/rdrvr/saved-logs/log-YYYYMMDD-HHMMSS.json. The file outlives the
// session: vulkan-trace.json is only written on a request while the profiler is off and is rotated at the next launch. The 40 newest stay.
// request(): any thread, never blocks (own detached background thread, one at a time). Report vr.menu.savedLog.
// A press writes the report alone. debug.rdrvr.savelog.eyes 1 at the press also keeps ONE eye picture next to it (about 110 MB per press).
// The JSON is still written at the press (its time = the moment of the press); the thread then waits for the menu to close (at most 15 s), 1 s more, asks for
// one normal eye dump (stereo/EyeDump.h, recorded on the render thread, written on its own thread) and moves the finished files to
// saved-logs/log-<stamp>-eye-dump*.json / *.raw (+ the AppSW half, eye-dump-warp*, when AppSW is on). The 6 newest stay.
// tools/saved_eye_dump_to_png.py converts them.
// Any leftover rdrvr/eye-dump* (a dump by property nobody pulled) is deleted first, so a stale file can never pass for this picture.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#if defined(__ANDROID__)||defined(__linux__)
#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#define RDRVR_SAVEDLOG_POSIX 1
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::diagnostics::savedlog {
inline constexpr unsigned keepFiles=40,keepEyeDumps=6;
// The picture's waits. eyeWaitMs: no stereo replay running (2D, loading, headset off) = nobody takes the request. warpWaitMs: the AppSW
// half finishes up to 5 s after its pair (SpaceWarp::dumpPoll) on its own writer thread.
inline constexpr unsigned menuWaitMs=15000,afterCloseMs=1000,eyeWaitMs=12000,warpWaitMs=8000,pollMs=50;
inline constexpr std::uint64_t eyeFreeBytes=512ull<<20; // below this free space on the device: the report only
inline constexpr const char* eyeTag="-eye-dump";
using Build=std::string(*)();
// What the picture needs from modules this header must not include (Vulkan, AppSW, menu): GameXr.cpp fills it. Empty = the report alone.
struct EyeHooks {
    const char* directory{};                      // Where EyeDump writes its files
    bool (*menuOpen)() noexcept{};
    void (*request)(bool on) noexcept{};          // on: one dump; off: drop a request nobody took (it would fire much later)
    std::uint64_t (*flushed)() noexcept{};        // Dumps whose files are written
    std::uint64_t (*refused)() noexcept{};        // Dumps that could not be recorded (no memory)
    bool (*warpActive)() noexcept{};              // AppSW on: the same dump also makes eye-dump-warp*
    std::uint64_t (*warpWritten)() noexcept{};    // AppSW dumps whose files are written
};
enum class State : unsigned {Idle,Saving,Saved,Failed,WaitMenu,Picture};
inline std::atomic<unsigned> stateAtom{unsigned(State::Idle)};
inline std::atomic<std::uint32_t> saves{},failures{},lastBytes{};
inline std::atomic<std::uint32_t> eyeRequests{},eyeSaved{},eyeFailed{},eyeFiles{},eyeWaitedMs{};
inline std::atomic<std::uint64_t> eyeBytes{};
inline std::atomic<bool> eyeWarp{};
inline std::mutex mutex;
inline std::string directory,lastName,lastError,eyeLast,eyeError;
inline Build build{};
inline EyeHooks eyeHooks{};
inline State state() noexcept {return State(stateAtom.load(std::memory_order_acquire));}
inline void configure(const std::string& rdrvrDir,Build report) {std::lock_guard lk(mutex);directory=rdrvrDir;build=report;}
inline void configureEyes(const EyeHooks& hooks) {std::lock_guard lk(mutex);eyeHooks=hooks;}
#ifdef RDRVR_SAVEDLOG_POSIX
namespace detail {
inline bool startsWith(const std::string& s,const char* prefix) {return s.rfind(prefix,0)==0;}
inline bool endsWith(const std::string& s,const char* tail) {const std::string t=tail;return s.size()>=t.size()&&s.compare(s.size()-t.size(),t.size(),t)==0;}
inline std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> names;
    if(DIR* d=opendir(dir.c_str())){while(dirent* e=readdir(d))names.emplace_back(e->d_name);closedir(d);}
    return names;
}
// The reports and the eye dumps are counted apart: log-<stamp>.json (40) / the whole set log-<stamp>-eye-dump* (6).
inline void rotate(const std::string& dir) {
    std::vector<std::string> reports,eyes,stamps;
    for(const auto& n:listDir(dir)){
        if(!startsWith(n,"log-"))continue;
        const auto tag=n.find(eyeTag);
        if(tag!=std::string::npos){eyes.push_back(n);stamps.push_back(n.substr(0,tag));}
        else if(n.size()>9&&endsWith(n,".json"))reports.push_back(n);
    }
    std::sort(reports.begin(),reports.end());
    for(std::size_t i=0;i+keepFiles<reports.size();++i)std::remove((dir+"/"+reports[i]).c_str());
    std::sort(stamps.begin(),stamps.end());stamps.erase(std::unique(stamps.begin(),stamps.end()),stamps.end());
    if(stamps.size()<=keepEyeDumps)return;
    const std::string& newestDropped=stamps[stamps.size()-keepEyeDumps-1];
    for(const auto& n:eyes)if(n.substr(0,n.find(eyeTag))<=newestDropped)std::remove((dir+"/"+n).c_str());
}
inline const char* write(const std::string& rdrvr,Build report,std::string& name,std::uint32_t& bytes) {
    const std::string dir=rdrvr+"/saved-logs";
    if(mkdir(dir.c_str(),0775)!=0&&errno!=EEXIST)return "NO DIRECTORY";
    chmod(dir.c_str(),0775); // adb (shell, group ext_data_rw) must read them, as PERF RECORD
    const std::string json=report();
    const std::time_t t=std::time(nullptr);std::tm tm{};localtime_r(&t,&tm);
    char stamp[48];std::snprintf(stamp,sizeof stamp,"log-%04d%02d%02d-%02d%02d%02d",tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec);
    name=stamp;
    const std::string temp=dir+"/"+name+".tmp",target=dir+"/"+name+".json";
    FILE* f=std::fopen(temp.c_str(),"wb");
    if(!f)return "NO FILE";
    const bool written=std::fwrite(json.data(),1,json.size(),f)==json.size()&&std::fflush(f)==0&&fsync(fileno(f))==0;
    if(std::fclose(f)!=0||!written){std::remove(temp.c_str());return "WRITE FAILED";}
    chmod(temp.c_str(),0664);
    if(std::rename(temp.c_str(),target.c_str())!=0){std::remove(temp.c_str());return "RENAME FAILED";}
    bytes=std::uint32_t(std::min<std::size_t>(json.size(),0xffffffffu));
    rotate(dir);return nullptr;
}
inline std::uint64_t nowMs() noexcept {return std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline void rest() {std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));}
inline bool eyeFile(const std::string& n) {return startsWith(n,"eye-dump")&&(endsWith(n,".raw")||endsWith(n,".json"));}
// rename, or a copy when the two paths are not one mount (EXDEV): only this background thread waits for it
inline bool moveFile(const std::string& from,const std::string& to) {
    if(std::rename(from.c_str(),to.c_str())==0){chmod(to.c_str(),0664);return true;}
    FILE* in=std::fopen(from.c_str(),"rb");if(!in)return false;
    const std::string temp=to+".tmp";FILE* out=std::fopen(temp.c_str(),"wb");if(!out){std::fclose(in);return false;}
    std::vector<char> chunk(1u<<20);bool ok=true;
    for(std::size_t n;(n=std::fread(chunk.data(),1,chunk.size(),in))>0;)if(std::fwrite(chunk.data(),1,n,out)!=n){ok=false;break;}
    ok=!std::ferror(in)&&ok;std::fclose(in);ok=std::fclose(out)==0&&ok;
    if(ok&&std::rename(temp.c_str(),to.c_str())!=0)ok=false;
    if(!ok){std::remove(temp.c_str());return false;}
    chmod(to.c_str(),0664);std::remove(from.c_str());return true;
}
// Every file of the finished dump -> saved-logs/<name>-eye-dump*; the JSON files last, so a prefixed eye-dump.json means the raws are there.
inline const char* moveEyeFiles(const std::string& from,const std::string& dir,const std::string& name,bool warp) {
    std::vector<std::string> files;bool meta=false;
    for(const auto& n:listDir(from)){
        if(!eyeFile(n)||(!warp&&startsWith(n,"eye-dump-warp")))continue;
        meta=meta||n=="eye-dump.json";files.push_back(n);}
    if(!meta)return "NO FILES";
    std::stable_partition(files.begin(),files.end(),[](const std::string& n){return endsWith(n,".raw");});
    std::uint32_t moved=0;std::uint64_t bytes=0;const char* error=nullptr;
    for(const auto& n:files){
        struct stat info{};const std::string source=from+"/"+n;const bool sized=stat(source.c_str(),&info)==0;
        if(!moveFile(source,dir+"/"+name+"-"+n)){error="MOVE FAILED";continue;}
        ++moved;if(sized)bytes+=std::uint64_t(info.st_size);}
    eyeFiles.store(moved,std::memory_order_relaxed);eyeBytes.store(bytes,std::memory_order_relaxed);eyeWarp.store(warp,std::memory_order_relaxed);
    return error;
}
// Background thread. Returns the row's reason (panel glyphs, <= 14 columns) or nullptr = saved.
inline const char* takePicture(const std::string& rdrvr,const std::string& name,const EyeHooks& h) {
    const std::string dir=rdrvr+"/saved-logs",from=h.directory;
    eyeFiles.store(0,std::memory_order_relaxed);eyeBytes.store(0,std::memory_order_relaxed);eyeWarp.store(false,std::memory_order_relaxed);eyeWaitedMs.store(0,std::memory_order_relaxed);
    struct statvfs disk{};
    if(statvfs(dir.c_str(),&disk)==0&&std::uint64_t(disk.f_bavail)*disk.f_frsize<eyeFreeBytes)return "DISK FULL";
    const auto pressed=nowMs();std::uint64_t closed=0;
    for(;;){
        const auto t=nowMs();
        if(h.menuOpen()){
            closed=0;eyeWaitedMs.store(std::uint32_t(t-pressed),std::memory_order_relaxed);
            if(t-pressed>=menuWaitMs)return "MENU OPEN";
            stateAtom.store(unsigned(State::WaitMenu),std::memory_order_release);
        }else{
            if(!closed)closed=t;
            stateAtom.store(unsigned(State::Picture),std::memory_order_release);
            if(t-closed>=afterCloseMs)break;
        }
        rest();
    }
    for(const auto& n:listDir(from))if(eyeFile(n))std::remove((from+"/"+n).c_str());
    const auto flushed=h.flushed(),refused=h.refused(),warpDone=h.warpWritten();
    h.request(true);
    const auto asked=nowMs();
    while(h.flushed()==flushed){
        if(h.refused()!=refused)return "NO MEMORY";
        if(nowMs()-asked>=eyeWaitMs){h.request(false);return "NO STEREO";}
        rest();
    }
    bool warp=false;
    if(h.warpActive()){const auto t=nowMs();while(!(warp=h.warpWritten()!=warpDone)&&nowMs()-t<warpWaitMs)rest();}
    const char* error=moveEyeFiles(from,dir,name,warp);
    rotate(dir);
    return error;
}
inline bool eyesWanted(const EyeHooks& h) noexcept {
    if(!h.directory||!h.menuOpen||!h.request||!h.flushed||!h.refused||!h.warpActive||!h.warpWritten)return false;
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX]{};return __system_property_get("debug.rdrvr.savelog.eyes",v)>0&&v[0]=='1'&&!v[1];
#else
    return false;
#endif
}
}
#endif
inline bool busy(State s) noexcept {return s==State::Saving||s==State::WaitMenu||s==State::Picture;}
// false = already saving / not configured yet
inline bool request() noexcept {
    std::string dir;Build report{};EyeHooks hooks{};
    try{std::lock_guard lk(mutex);dir=directory;report=build;hooks=eyeHooks;}catch(...){return false;}
    if(dir.empty()||!report)return false;
    unsigned expected=stateAtom.load(std::memory_order_relaxed);
    if(busy(State(expected))||!stateAtom.compare_exchange_strong(expected,unsigned(State::Saving),std::memory_order_acq_rel))return false;
#ifdef RDRVR_SAVEDLOG_POSIX
    try{std::thread([dir,report,hooks]{
        setpriority(PRIO_PROCESS,0,10); // the trace writer's priority: never competes with the game threads
        const bool eyes=detail::eyesWanted(hooks);
        if(eyes)eyeRequests.fetch_add(1,std::memory_order_relaxed); // Before the report: its own savedLog shows the picture on its way
        {std::lock_guard lk(mutex);eyeError.clear();}
        std::string name;std::uint32_t bytes{};const char* error="REPORT FAILED";
        try{error=detail::write(dir,report,name,bytes);}catch(...){}
        {std::lock_guard lk(mutex);lastName=name;lastError=error?error:"";}
        if(error)failures.fetch_add(1,std::memory_order_relaxed);
        else{lastBytes.store(bytes,std::memory_order_relaxed);saves.fetch_add(1,std::memory_order_relaxed);}
        if(eyes){
            const char* why=error?"NO REPORT":"FAILED";
            if(!error)try{why=detail::takePicture(dir,name,hooks);}catch(...){}
            try{std::lock_guard lk(mutex);eyeError=why?why:"";if(!why)eyeLast=name;}catch(...){}
            (why?eyeFailed:eyeSaved).fetch_add(1,std::memory_order_relaxed);
        }
        stateAtom.store(unsigned(error?State::Failed:State::Saved),std::memory_order_release);
    }).detach();return true;}catch(...){}
#endif
    stateAtom.store(unsigned(State::Failed),std::memory_order_release);failures.fetch_add(1,std::memory_order_relaxed);return false;
}
// The ROOT row's value (panel glyphs: no lower case; <= 34 columns)
inline void value(char* out,std::size_t size) {
    if(!size)return;
    switch(state()){
    case State::Saving:std::snprintf(out,size,"SAVING...");break;
    case State::WaitMenu:std::snprintf(out,size,"CLOSE THE MENU FOR THE PICTURE");break;
    case State::Picture:std::snprintf(out,size,"TAKING THE PICTURE...");break;
    case State::Saved:{std::lock_guard lk(mutex);
        if(!eyeError.empty())std::snprintf(out,size,"SAVED - NO PICTURE: %s",eyeError.c_str());
        else{const bool picture=!eyeLast.empty()&&eyeLast==lastName;
            // log-YYYYMMDD-HHMMSS -> HH:MM:SS
            if(lastName.size()>=19)std::snprintf(out,size,"SAVED %.2s:%.2s:%.2s%s",lastName.c_str()+13,lastName.c_str()+15,lastName.c_str()+17,picture?" WITH PICTURE":"");
            else std::snprintf(out,size,"SAVED");}
        break;}
    case State::Failed:{std::lock_guard lk(mutex);std::snprintf(out,size,"FAILED - %s",lastError.c_str());break;}
    default:std::snprintf(out,size,"PRESS A TO SAVE THE LOG");break;}
}
inline std::string report() {
    std::lock_guard lk(mutex);
    return "{\"saves\":"+std::to_string(saves.load(std::memory_order_relaxed))+",\"failures\":"+std::to_string(failures.load(std::memory_order_relaxed))+
        ",\"lastBytes\":"+std::to_string(lastBytes.load(std::memory_order_relaxed))+",\"last\":\""+lastName+"\",\"error\":\""+lastError+"\""+
        ",\"state\":"+std::to_string(stateAtom.load(std::memory_order_relaxed))+
        ",\"eyeRequests\":"+std::to_string(eyeRequests.load(std::memory_order_relaxed))+",\"eyeSaved\":"+std::to_string(eyeSaved.load(std::memory_order_relaxed))+
        ",\"eyeFailed\":"+std::to_string(eyeFailed.load(std::memory_order_relaxed))+",\"eyeLast\":\""+eyeLast+"\",\"eyeError\":\""+eyeError+"\""+
        ",\"eyeFiles\":"+std::to_string(eyeFiles.load(std::memory_order_relaxed))+",\"eyeBytes\":"+std::to_string(eyeBytes.load(std::memory_order_relaxed))+
        ",\"eyeWarp\":"+(eyeWarp.load(std::memory_order_relaxed)?"true":"false")+",\"eyeMenuWaitMs\":"+std::to_string(eyeWaitedMs.load(std::memory_order_relaxed))+'}';
}
}
