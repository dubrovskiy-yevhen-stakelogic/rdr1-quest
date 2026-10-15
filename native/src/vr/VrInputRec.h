#pragma once
// INPUT RECORD / REPLAY: a play-through (head + controllers, every XR frame, exact timing) is
// recorded and replayed 1:1 later, e.g. on a headset lying still (same save, same game -> the same
// scene, the same looks / shots / menus).
// debug.rdrvr.input.record 1 arms a recording: it starts at the first XR frame that submits a WORLD
// stereo layer (presentationGate_.world() && gameStereo) and stops at session end (STOPPING /
// EXITING / loss), on 0, or after 30 min. One recording per arming (0 -> 1 re-arms; a new process
// re-arms while the property stays 1). File <files>/rdrvr/input-rec-<unix s>.bin (next to
// graphics.cfg). debug.rdrvr.input.replay <name> loads <files>/rdrvr/<name> in the background; the
// replay starts at the first world stereo frame after the load and substitutes the recorded inputs
// from the next XR frame on; at the end of the recording live input returns (state DONE). Empty / 0
// = off. Both properties are re-read at 1 Hz on the XR thread.
// Every raw OpenXR input the mod reads per XR frame goes through the hooks below, called right
// after the OpenXR calls by XrHarness::frame / locateHands and ControllerActions::read:
// xrLocateViews (view state flags, view count, both view poses + fovs, before the position
// fallback and the synthetic head motion), the session focus (it gates tracking and input), the
// xrSyncActions focus gate, the aim pose action (isActive) + xrLocateSpace (pose + location
// flags), every ControllerActions action state (stick / trigger / squeeze / stick click / grip
// pose / A B X Y menu / trigger touch: result, isActive, currentState, changedSinceLastSync), the
// grip xrLocateSpace (pose, location flags, XrSpaceVelocity), the non-controller recenter action,
// and runtime reference-space changes (applied before the views are located). While a replay
// plays, the hooks overwrite the live values with the recorded ones, so everything downstream
// (tracking gate, views mailbox, the submitted layer poses, ControllerSample, stick-click arbiter
// + recenter chord, VR / graphics menu, hands / melee / throw / hammer tapes, trigger-touch
// channel) runs unchanged on replayed input:
//   * lookup axis = display time: key = tNs + horizonNs (recorded), query = (wake - replay start) + this frame's horizon;
//   * poses (views, aim, grip) slerp / lerp between the two neighbouring records, grip velocities lerp; flags / fov / sticks from the record
//     at or before the query;
//   * booleans (stick clicks, A B X Y menu, trigger touch, recenter action) through an edge queue: every recorded edge becomes exactly one
//     replayed edge, in order, at most one per channel per XR frame (a 1-frame press survives a slower replay; nothing doubles);
//   * triggers / grips: the latest value, except that a crossing of 0.5 is never skipped (the crossing record's value is delivered first);
//   * a recorded runtime recenter (reference-space change) is re-applied (resetSpace) when the replay passes it; user recenters (stick
//     chord, VR page RECENTER) regenerate from the replayed buttons.
// Haptics (outputs) are not recorded. Recording during a replay records what the frames used (a copy, flag::replayed) -> fidelity diff.
// Threads: XR thread = hooks, commit, property poll; writer thread (detached, one per recording: fopen / fwrite / fflush per chunk, the XR
// thread only copies 360 B per frame into a preallocated ring) ; loader thread (detached, one per replay file: fread + validation); report
// thread = atomics + the name strings under textMutex.
// File v1 (little endian): FileHeader (128 B) + layout text (python struct codes "name:code ...", NUL padded to 8) + Frame records (360 B).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#define RDRVR_INPUTREC_LOG(...) __android_log_print(ANDROID_LOG_INFO,"RdrVrInput",__VA_ARGS__)
#else
#define RDRVR_INPUTREC_LOG(...) ((void)0)
#endif

namespace rdrvr::vr::inputrec {
// ---- file format v1 ----
inline constexpr char magic[8]{'R','D','R','V','R','I','N','P'};
inline constexpr std::uint32_t version=1;
inline constexpr std::int64_t maxRecordNs=30ll*60*1'000'000'000; // 30 min cap
namespace bit { // PadRaw ok / active / changed bit numbers (+hand: 0 left, 1 right; face +0..4 = A B X Y menu); state = the boolean ones
inline constexpr unsigned stick=0,trigger=2,squeeze=4,click=6,gripPose=8,face=10,touch=15,gripLocated=17,recenter=19;
}
inline constexpr std::uint32_t boolMask=(3u<<bit::click)|(31u<<bit::face)|(3u<<bit::touch)|(1u<<bit::recenter);
// One ControllerActions::read (or the non-controller recenter read) worth of raw OpenXR action states.
struct PadRaw {
    float stick[2][2]{};                  // per hand x, y (OpenXR +y up)
    float trigger[2]{},squeeze[2]{};
    float grip[2][7]{};                   // grip pose per hand: qx qy qz qw px py pz (LOCAL)
    float angular[2][3]{},linear[2][3]{}; // XrSpaceVelocity of the grip space (rad/s, m/s, LOCAL)
    std::uint32_t ok{};                   // XR_SUCCEEDED of the call (bit::*; gripLocated = the grip xrLocateSpace)
    std::uint32_t active{};               // isActive
    std::uint32_t state{};                // boolean currentState (boolMask bits)
    std::uint32_t changed{};              // changedSinceLastSync (replay: the delivered boolean edges; nothing in the mod reads it)
    std::uint32_t gripFlags[2]{};         // XrSpaceLocationFlags of the grip locate
    std::uint32_t velFlags[2]{};          // XrSpaceVelocityFlags
};
namespace flag {
inline constexpr std::uint32_t viewsLocated=1u<<0,focused=1u<<1,padRead=1u<<2,shouldRender=1u<<3,gameStereo=1u<<4,world=1u<<5,
    spaceChange=1u<<6,userRecenter=1u<<7,pdtMono=1u<<8,replayed=1u<<9,handsLocated=1u<<10,controllerPath=1u<<11;
}
inline constexpr std::uint32_t aimActive=1u<<31; // Frame::aimFlags: the aim action was active (low bits = XrSpaceLocationFlags)
struct Frame {
    std::int64_t tNs{};                   // this XR frame's wake (after xrWaitFrame) - the first recorded frame's wake (CLOCK_MONOTONIC)
    std::int64_t horizonNs{};             // predictedDisplayTime (CLOCK_MONOTONIC) - wake; 0 = unknown (flag::pdtMono clear)
    std::int64_t periodNs{};              // predictedDisplayPeriod
    std::uint32_t flags{};                // flag::*
    std::uint32_t viewFlags{};            // XrViewStateFlags of xrLocateViews
    std::uint32_t viewCount{};
    std::uint32_t aimFlags[2]{};
    float eye[2][7]{};                    // view poses (LOCAL): qx qy qz qw px py pz
    float fov[2][4]{};                    // angleLeft angleRight angleUp angleDown
    float aim[2][7]{};                    // aim poses (LOCAL)
    PadRaw pad{};
    std::uint32_t reserved{};
};
static_assert(sizeof(PadRaw)==168&&sizeof(Frame)==360&&std::is_trivially_copyable_v<Frame>&&std::is_standard_layout_v<Frame>,"input-rec v1 layout");
static_assert(offsetof(Frame,eye)==44&&offsetof(Frame,pad)==188&&offsetof(Frame,reserved)==356,"input-rec v1 offsets");
// Python struct codes in field order (every file stores a copy, so a reader can build its parser from it)
inline constexpr char layout[]="tNs:q horizonNs:q periodNs:q flags:I viewFlags:I viewCount:I aimFlags:2I eye:14f fov:8f aim:14f "
    "stick:4f trigger:2f squeeze:2f grip:14f angular:6f linear:6f padOk:I padActive:I padState:I padChanged:I gripFlags:2I velFlags:2I reserved:I";
inline constexpr std::uint32_t layoutBytes=(sizeof(layout)+7)/8*8;
struct FileHeader {
    char magic[8]{};
    std::uint32_t version{},headerBytes{},frameBytes{},layoutBytes{}; // headerBytes = 128 + layoutBytes = offset of the first Frame
    std::int64_t startUnixNs{},startMonoNs{};                          // CLOCK_REALTIME / CLOCK_MONOTONIC of frame 0's wake
    char buildId[72]{};                                                // RDRVR_BUILD_ID of the recording build
    std::uint32_t flags{},reserved[3]{};
};
static_assert(sizeof(FileHeader)==128,"input-rec v1 header");
#ifdef RDRVR_BUILD_ID
inline constexpr const char* buildIdText=RDRVR_BUILD_ID;
#else
inline constexpr const char* buildIdText="host";
#endif

// ---- math: shortest-arc slerp (renormalised) + position lerp; out may alias a ----
inline void blendPose(const float* a,const float* b,float w,float* out) noexcept {
    double d=0;for(unsigned k=0;k<4;++k)d+=double(a[k])*double(b[k]);
    const double s=d<0?-1.:1.;d=std::fabs(d);
    double wa=1.-w,wb=w;
    if(d<0.9995){const double th=std::acos(std::min(d,1.)),sn=std::sin(th);wa=std::sin((1.-w)*th)/sn;wb=std::sin(double(w)*th)/sn;}
    double q[4],n=0;for(unsigned k=0;k<4;++k){q[k]=wa*a[k]+s*wb*b[k];n+=q[k]*q[k];}
    n=n>0?1./std::sqrt(n):0.;for(unsigned k=0;k<4;++k)out[k]=float(q[k]*n);
    for(unsigned k=4;k<7;++k)out[k]=a[k]+(b[k]-a[k])*w;
}
inline float analogOf(const PadRaw& p,unsigned a) noexcept {return a<2?p.trigger[a]:p.squeeze[a-2];} // a: trigger L R, squeeze L R
inline float& analogOf(PadRaw& p,unsigned a) noexcept {return a<2?p.trigger[a]:p.squeeze[a-2];}

// ---- shared text (files dir, names, errors): XR thread on transitions, writer / loader on failure, report thread ----
inline std::mutex textMutex;
inline std::string directory;             // <files>/rdrvr, TraceEntry entry() before the XR thread exists
inline void configureDirectory(const std::string& d) noexcept {try{std::lock_guard g(textMutex);directory=d;}catch(...){}}
inline std::string directoryCopy() noexcept {try{std::lock_guard g(textMutex);return directory;}catch(...){return {};}}
inline std::int64_t realtimeNs() noexcept {
    return std::int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
// A replay file name from the property: [A-Za-z0-9._-]{1,80}, no "..": nothing to escape in the report, no path outside rdrvr/.
inline bool safeName(const char* s) noexcept {
    const std::size_t n=std::strlen(s);if(!n||n>80||std::strstr(s,".."))return false;
    for(std::size_t i=0;i<n;++i){const char c=s[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return false;}
    return true;
}

// ---- RECORD ----
namespace rec {
enum State:int{Off,Armed,Recording,Stopping,Done,Failed};
inline constexpr unsigned chunkFrames=64,chunkCount=32;     // 23 KB chunks (~0.9 s at 72 Hz), 0.7 MB ring (~28 s of writer stall)
struct Chunk{Frame f[chunkFrames];std::uint32_t count{};};
inline Chunk ring[chunkCount];                              // bss: not resident until a recording touches it
inline std::atomic<std::uint64_t> produced{},consumed{};     // chunks handed to / written by the writer
inline std::atomic<int> state{Off};
inline std::atomic<bool> stopRequest{},writerBusy{};
inline std::atomic<std::uint64_t> frames{},bytes{},droppedChunks{},droppedFrames{};
inline std::atomic<std::int64_t> lastTNs{};
inline std::string file,error;                              // textMutex
// XR thread
inline std::uint64_t made{};inline unsigned fill{};inline std::int64_t startMono{};inline bool requested{};
inline void setError(const char* why) noexcept {try{std::lock_guard g(textMutex);error=why;}catch(...){}}
// Writer thread: header + layout, then every completed chunk (fwrite + fflush); exits after the final chunk once stop is requested.
inline void writerMain(std::string path,FileHeader header) noexcept {
    std::FILE* f=std::fopen(path.c_str(),"wb");const char* why=f?nullptr:"open failed";
    if(f){char text[layoutBytes]{};std::memcpy(text,layout,sizeof layout);
        if(std::fwrite(&header,sizeof header,1,f)!=1||std::fwrite(text,sizeof text,1,f)!=1||std::fflush(f)!=0)why="header write failed";
        else bytes.fetch_add(sizeof header+sizeof text,std::memory_order_relaxed);}
    while(!why){
        const bool stop=stopRequest.load(std::memory_order_acquire); // before produced: the final chunk is published before the request
        const auto p=produced.load(std::memory_order_acquire);auto c=consumed.load(std::memory_order_relaxed);
        if(c<p){
            for(;c<p;++c){const auto& ch=ring[c%chunkCount];
                if(ch.count&&std::fwrite(ch.f,sizeof(Frame),ch.count,f)!=ch.count){why="write failed";break;}
                bytes.fetch_add(std::uint64_t(ch.count)*sizeof(Frame),std::memory_order_relaxed);consumed.store(c+1,std::memory_order_release);}
            if(!why&&std::fflush(f)!=0)why="flush failed";
            continue;
        }
        if(stop)break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if(f&&std::fclose(f)!=0&&!why)why="close failed";
    if(why){setError(why);RDRVR_INPUTREC_LOG("input record FAILED: %s (%s)",why,path.c_str());}
    else RDRVR_INPUTREC_LOG("input record DONE %s frames=%llu bytes=%llu",path.c_str(),(unsigned long long)frames.load(),(unsigned long long)bytes.load());
    state.store(why?Failed:Done,std::memory_order_release);
    writerBusy.store(false,std::memory_order_release);
}
inline void arm() noexcept {
    produced.store(0);consumed.store(0);stopRequest.store(false);made=0;fill=0;startMono=0;
    frames.store(0);bytes.store(0);droppedChunks.store(0);droppedFrames.store(0);lastTNs.store(0);
    try{std::lock_guard g(textMutex);file.clear();error.clear();}catch(...){}
    state.store(Armed,std::memory_order_release);RDRVR_INPUTREC_LOG("input record ARMED");
}
// XR thread, the first world stereo frame after arming: state RECORDING before the writer exists (a writer failure is never overwritten).
inline void start(std::uint64_t wake,std::int64_t unixNs) noexcept {
    const std::string dir=directoryCopy();
    if(dir.empty()){setError("no files directory");state.store(Failed);return;}
    char name[64];std::snprintf(name,sizeof name,"input-rec-%lld.bin",static_cast<long long>(unixNs/1'000'000'000));
    FileHeader h{};std::memcpy(h.magic,magic,sizeof magic);h.version=version;h.headerBytes=sizeof(FileHeader)+layoutBytes;h.frameBytes=sizeof(Frame);
    h.layoutBytes=layoutBytes;h.startUnixNs=unixNs;h.startMonoNs=std::int64_t(wake);std::snprintf(h.buildId,sizeof h.buildId,"%s",buildIdText);
    try{std::lock_guard g(textMutex);file=name;}catch(...){}
    startMono=std::int64_t(wake);writerBusy.store(true);state.store(Recording,std::memory_order_release);
    try{std::thread(writerMain,dir+"/"+name,h).detach();}catch(...){writerBusy.store(false);setError("writer thread");state.store(Failed);return;}
    RDRVR_INPUTREC_LOG("input record START %s/%s",dir.c_str(),name);
}
// XR thread: one frame into the ring; a ring full of unwritten chunks drops frames (counted), never waits.
inline void commit(const Frame& fr) noexcept {
    if(fill==0&&made-consumed.load(std::memory_order_acquire)>=chunkCount){
        if(droppedFrames.fetch_add(1,std::memory_order_relaxed)%chunkFrames==0)droppedChunks.fetch_add(1,std::memory_order_relaxed);return;}
    auto& ch=ring[made%chunkCount];ch.f[fill++]=fr;frames.fetch_add(1,std::memory_order_relaxed);lastTNs.store(fr.tNs,std::memory_order_relaxed);
    if(fill==chunkFrames){ch.count=fill;fill=0;produced.store(++made,std::memory_order_release);}
}
// XR thread: ARMED -> OFF; RECORDING -> STOPPING (final partial chunk published, then the stop request; the writer sets DONE / FAILED).
inline void stop(const char* why) noexcept {
    (void)why;int s=state.load(std::memory_order_acquire);
    if(s==Armed){state.store(Off);return;}
    if(s!=Recording)return;
    if(fill){ring[made%chunkCount].count=fill;fill=0;produced.store(++made,std::memory_order_release);}
    state.compare_exchange_strong(s,Stopping);stopRequest.store(true,std::memory_order_release);
    RDRVR_INPUTREC_LOG("input record STOP (%s) frames=%llu",why,(unsigned long long)frames.load());
}
} // namespace rec

// ---- REPLAY ----
namespace play {
enum State:int{Off,Loading,Ready,Playing,Done,Failed};
struct Track {
    FileHeader header{};std::vector<Frame> f;std::vector<std::int64_t> key; // key = display-time axis (tNs + horizonNs, non-decreasing)
    std::uint64_t generation{};std::string error;
};
inline std::atomic<int> state{Off};
inline std::atomic<Track*> pending{};           // loader -> XR thread
inline std::atomic<std::uint64_t> generation{};
inline std::atomic<std::uint64_t> frames{},records{},lateFrames{},skipped{},edges{},edgesLate{},edgesCollapsed{},spaceChanges{},liveIgnored{};
inline constexpr std::int64_t maxEdgeLagNs=100'000'000; // a channel queue trailing the previous delivery by more (a channel toggling faster
                                                        // than the replay frames) collapses to the newest state: bounded lag / scan cost
inline std::atomic<std::int64_t> elapsedNs{},durationNs{};
inline std::string file,error,recordedBuild;     // textMutex
// XR thread
inline Track* track{};inline char requested[96]{};inline std::uint64_t startWake{};
inline std::int64_t idx{},prevIdx{},eventIdx{},padIdx{};
inline std::array<std::int64_t,32> boolIdx{};inline std::uint32_t delivered{};   // per boolean bit: last record consumed / delivered states
inline std::array<std::int64_t,4> analogIdx{};inline std::uint32_t analogHigh{};  // per analog: last record consumed / delivered >= 0.5
inline Frame cur{};                              // this XR frame's replayed sample (interpolated)
inline void setError(const char* why) noexcept {try{std::lock_guard g(textMutex);error=why;}catch(...){}}
// Large frees off the XR thread (a 30 min track is ~47 MB).
inline void discard(Track* t) noexcept {if(!t)return;try{std::thread([t]{delete t;}).detach();}catch(...){delete t;}}
// Synchronous file load + validation (loader thread; host test). Never null.
inline Track* load(const std::string& path) noexcept {
    Track* t=nullptr;try{t=new Track;}catch(...){return nullptr;}
    std::FILE* fp=std::fopen(path.c_str(),"rb");
    auto bad=[&](const char* why){t->error=why;t->f.clear();t->key.clear();if(fp)std::fclose(fp);fp=nullptr;return t;};
    if(!fp)return bad("open failed");
    auto& h=t->header;
    if(std::fread(&h,sizeof h,1,fp)!=1)return bad("short header");
    if(std::memcmp(h.magic,magic,sizeof magic)!=0)return bad("bad magic");
    if(h.version!=version)return bad("unsupported version");
    if(h.frameBytes!=sizeof(Frame)||h.layoutBytes!=layoutBytes||h.headerBytes!=sizeof(FileHeader)+layoutBytes)return bad("frame layout mismatch");
    char text[layoutBytes]{};
    if(std::fread(text,sizeof text,1,fp)!=1||std::strncmp(text,layout,sizeof layout)!=0)return bad("layout text mismatch");
    if(std::fseek(fp,0,SEEK_END)!=0)return bad("seek failed");
    const long size=std::ftell(fp);
    if(size<long(h.headerBytes))return bad("short file");
    const std::size_t n=std::size_t(size-long(h.headerBytes))/sizeof(Frame); // a torn last frame (killed writer) is ignored
    if(n<2)return bad("fewer than 2 frames");
    try{t->f.resize(n);t->key.resize(n);}catch(...){return bad("out of memory");}
    if(std::fseek(fp,long(h.headerBytes),SEEK_SET)!=0||std::fread(t->f.data(),sizeof(Frame),n,fp)!=n)return bad("read failed");
    std::fclose(fp);fp=nullptr;
    for(std::size_t i=0;i<n;++i){
        if(i&&t->f[i].tNs<=t->f[i-1].tNs)return bad("time not increasing");
        const std::int64_t k=t->f[i].tNs+t->f[i].horizonNs;t->key[i]=i?std::max(t->key[i-1],k):k;
    }
    return t;
}
inline void fail(const char* why) noexcept {setError(why);state.store(Failed,std::memory_order_release);RDRVR_INPUTREC_LOG("input replay FAILED: %s",why);}
// XR thread (every frame while LOADING): take the loader's track.
inline void adopt() noexcept {
    Track* t=pending.exchange(nullptr,std::memory_order_acq_rel);if(!t)return;
    if(t->generation!=generation.load(std::memory_order_relaxed)||state.load(std::memory_order_relaxed)!=Loading){discard(t);return;}
    if(!t->error.empty()){try{std::lock_guard g(textMutex);error=t->error;}catch(...){}state.store(Failed,std::memory_order_release);
        RDRVR_INPUTREC_LOG("input replay FAILED: %s",t->error.c_str());discard(t);return;}
    discard(track);track=t;records.store(t->f.size());durationNs.store(t->key.back());
    {const char* id=t->header.buildId;try{std::lock_guard g(textMutex);recordedBuild.assign(id,std::find(id,id+sizeof t->header.buildId,'\0'));}catch(...){}}
    state.store(Ready,std::memory_order_release);
    RDRVR_INPUTREC_LOG("input replay READY frames=%zu duration=%.1f s build=%.8s",t->f.size(),double(t->key.back())*1e-9,t->header.buildId);
}
// XR thread, the first world stereo frame after READY: substitution starts with the next XR frame.
inline void begin(std::uint64_t wake) noexcept {
    startWake=wake;idx=prevIdx=eventIdx=0;padIdx=-1;boolIdx.fill(-1);analogIdx.fill(-1);delivered=0;analogHigh=0;
    frames.store(0);lateFrames.store(0);skipped.store(0);edges.store(0);edgesLate.store(0);edgesCollapsed.store(0);spaceChanges.store(0);elapsedNs.store(0);
    liveIgnored.store(0);state.store(Playing,std::memory_order_release);RDRVR_INPUTREC_LOG("input replay START");
}
inline void finish() noexcept {
    state.store(Done,std::memory_order_release);discard(track);track=nullptr;
    RDRVR_INPUTREC_LOG("input replay DONE frames=%llu late=%llu edges=%llu",(unsigned long long)frames.load(),(unsigned long long)lateFrames.load(),
        (unsigned long long)edges.load());
}
// XR thread, every frame while PLAYING: the sample for this frame's display time. Returns true when a recorded runtime recenter
// (reference-space change) was passed.
inline bool step(std::uint64_t wake,std::int64_t horizon) noexcept {
    const auto& T=*track;const std::int64_t n=std::int64_t(T.f.size());
    const std::int64_t d=std::int64_t(wake-startWake)+horizon;elapsedNs.store(d,std::memory_order_relaxed);
    const auto last=std::size_t(n-1);if(d>T.key[last]+std::max<std::int64_t>(T.f[last].periodNs,1'000'000)){finish();return false;}
    while(idx+1<n&&T.key[std::size_t(idx+1)]<=d)++idx;
    if(idx-prevIdx>1){lateFrames.fetch_add(1,std::memory_order_relaxed);skipped.fetch_add(std::uint64_t(idx-prevIdx-1),std::memory_order_relaxed);}
    prevIdx=idx;const auto i0=std::size_t(idx);cur=T.f[i0];
    if(idx+1<n&&d>T.key[i0]){
        const auto& b=T.f[i0+1];const float w=float(double(d-T.key[i0])/double(T.key[i0+1]-T.key[i0]));
        if(cur.flags&b.flags&flag::viewsLocated)for(unsigned i=0;i<2;++i)blendPose(cur.eye[i],b.eye[i],w,cur.eye[i]);
        if(cur.flags&b.flags&flag::handsLocated)for(unsigned i=0;i<2;++i)if(cur.aimFlags[i]&b.aimFlags[i]&aimActive)blendPose(cur.aim[i],b.aim[i],w,cur.aim[i]);
        if(cur.flags&b.flags&flag::padRead)for(unsigned i=0;i<2;++i)if((cur.pad.ok&b.pad.ok)>>(bit::gripLocated+i)&1u){
            blendPose(cur.pad.grip[i],b.pad.grip[i],w,cur.pad.grip[i]);
            for(unsigned k=0;k<3;++k){cur.pad.angular[i][k]+=(b.pad.angular[i][k]-cur.pad.angular[i][k])*w;cur.pad.linear[i][k]+=(b.pad.linear[i][k]-cur.pad.linear[i][k])*w;}}
    }
    frames.fetch_add(1,std::memory_order_relaxed);
    bool space=false;for(auto j=eventIdx+1;j<=idx;++j)space=space||(T.f[std::size_t(j)].flags&flag::spaceChange)!=0;
    eventIdx=std::max(eventIdx,idx);if(space)spaceChanges.fetch_add(1,std::memory_order_relaxed);
    return space;
}
// XR thread, the controller read of a PLAYING frame: the replayed raw states. Edge queue for booleans, crossing keep for analogs.
inline void pad(PadRaw& raw) noexcept {
    if(!(cur.flags&flag::padRead)){raw=PadRaw{};raw.ok=~(3u<<bit::gripLocated);return;} // recorded: no read -> an inactive sample (invalidated)
    const auto& T=*track;const std::uint32_t before=delivered;
    raw=cur.pad;
    // lag = how far a channel's queue trails the previous delivery (a gap without deliveries is not lag: its edges still come one by one)
    const std::int64_t stale=padIdx>=0?T.key[std::size_t(padIdx)]-maxEdgeLagNs:INT64_MIN;
    for(unsigned b=0;b<32;++b){
        if(!((boolMask>>b)&1u))continue;
        const bool on=(delivered>>b)&1u;auto& ci=boolIdx[b];std::int64_t found=-1;
        if(ci>=0&&ci<padIdx&&T.key[std::size_t(ci)]<stale){ci=idx;edgesCollapsed.fetch_add(1,std::memory_order_relaxed); // lagging queue: newest state
            if(bool((cur.pad.state>>b)&1u)!=on){delivered^=1u<<b;edges.fetch_add(1,std::memory_order_relaxed);}continue;}
        for(auto j=std::max<std::int64_t>(ci+1,0);j<=idx;++j){const auto& r=T.f[std::size_t(j)];
            if((r.flags&flag::padRead)&&bool((r.pad.state>>b)&1u)!=on){found=j;break;}}
        if(found>=0){delivered^=1u<<b;ci=found;edges.fetch_add(1,std::memory_order_relaxed);if(found<=padIdx)edgesLate.fetch_add(1,std::memory_order_relaxed);}
        else ci=idx;
    }
    raw.state=(raw.state&~boolMask)|(delivered&boolMask);raw.changed=(raw.changed&~boolMask)|((delivered^before)&boolMask);
    for(unsigned a=0;a<4;++a){
        const bool high=(analogHigh>>a)&1u;auto& ci=analogIdx[a];std::int64_t found=-1;
        if(ci>=0&&ci<padIdx&&T.key[std::size_t(ci)]<stale){ci=idx;edgesCollapsed.fetch_add(1,std::memory_order_relaxed);
            if((analogOf(cur.pad,a)>=.5f)!=high)analogHigh^=1u<<a;continue;}
        for(auto j=std::max<std::int64_t>(ci+1,0);j<=idx;++j){const auto& r=T.f[std::size_t(j)];
            if((r.flags&flag::padRead)&&(analogOf(r.pad,a)>=.5f)!=high){found=j;break;}}
        if(found>=0){analogHigh^=1u<<a;ci=found;analogOf(raw,a)=analogOf(T.f[std::size_t(found)].pad,a);}
        else ci=idx;
    }
    padIdx=idx;
}
} // namespace play

// ---- XR thread hooks (XrHarness.cpp, ControllerActions.cpp). Both properties unset: one bool test per hook. ----
struct XrState {
    bool on{},capturing{},replaying{};    // on = capturing || replaying
    std::uint64_t wake{},nextPoll{};
    Frame f{};                            // this frame's inputs as used (committed by frameEnd while RECORDING)
};
inline XrState xr{};
// debug.rdrvr.input.record: '1' arms (one recording per arming), anything else stops / disarms.
inline void requestRecord(bool want) noexcept {
    using namespace rec;
    if(!want){requested=false;const int s=state.load();if(s==Armed)state.store(Off);else if(s==Recording)stop("property 0");return;}
    if(requested)return;
    const int s=state.load();
    if(s==Armed||s==Recording||s==Stopping||writerBusy.load(std::memory_order_acquire))return; // the previous writer still drains: next poll
    requested=true;arm();
}
// debug.rdrvr.input.replay: a file name in rdrvr/ starts loading it; empty / "0" = off (live input at once).
inline void requestReplay(const char* name) noexcept {
    using namespace play;
    const char* want=(name&&*name&&std::strcmp(name,"0")!=0)?name:"";
    if(std::strncmp(want,requested,sizeof requested)==0)return;
    std::snprintf(requested,sizeof requested,"%s",want);
    generation.fetch_add(1,std::memory_order_relaxed);discard(track);track=nullptr;xr.replaying=false;
    frames.store(0);records.store(0);lateFrames.store(0);skipped.store(0);edges.store(0);edgesLate.store(0);edgesCollapsed.store(0);spaceChanges.store(0);
    elapsedNs.store(0);durationNs.store(0);liveIgnored.store(0);
    const bool safe=safeName(want);
    try{std::lock_guard g(textMutex);file=safe?want:"";error.clear();recordedBuild.clear();}catch(...){}
    if(!*want){state.store(Off,std::memory_order_release);return;}
    if(!safe){fail("bad file name");return;}
    const std::string dir=directoryCopy();if(dir.empty()){fail("no files directory");return;}
    state.store(Loading,std::memory_order_release);
    const auto gen=generation.load(std::memory_order_relaxed);
    try{std::thread([path=dir+"/"+want,gen]{
        Track* t=load(path);if(!t)return;t->generation=gen;
        if(Track* old=pending.exchange(t,std::memory_order_acq_rel))delete old;}).detach();}
    catch(...){fail("loader thread");}
    RDRVR_INPUTREC_LOG("input replay LOADING %s",want);
}
inline void pollProperties() noexcept {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX]{};
    requestRecord(__system_property_get("debug.rdrvr.input.record",v)>0&&v[0]=='1');
    char r[PROP_VALUE_MAX]{};__system_property_get("debug.rdrvr.input.replay",r);requestReplay(r);
#endif
}
// After xrBeginFrame. Returns true when the replay passed a recorded runtime recenter (the caller re-applies resetSpace).
inline bool frameBegin(std::uint64_t wake,std::int64_t pdtMono,std::int64_t periodNs,bool shouldRender) noexcept {
    xr.wake=wake;if(wake>=xr.nextPoll){xr.nextPoll=wake+1'000'000'000ull;pollProperties();}
    if(play::state.load(std::memory_order_relaxed)==play::Loading)play::adopt();
    const int rs=rec::state.load(std::memory_order_relaxed);
    xr.capturing=rs==rec::Armed||rs==rec::Recording;
    xr.replaying=play::track&&play::state.load(std::memory_order_relaxed)==play::Playing;
    if(!xr.capturing&&!xr.replaying){xr.on=false;return false;}
    const std::int64_t horizon=pdtMono>0?pdtMono-std::int64_t(wake):0;bool space=false;
    if(xr.replaying){space=play::step(wake,horizon);xr.replaying=play::state.load(std::memory_order_relaxed)==play::Playing;}
    xr.on=xr.capturing||xr.replaying;
    if(xr.capturing){xr.f=Frame{};xr.f.horizonNs=horizon;xr.f.periodNs=periodNs;
        xr.f.flags=(shouldRender?flag::shouldRender:0u)|(pdtMono>0?flag::pdtMono:0u)|(xr.replaying?flag::replayed:0u);}
    return space;
}
inline bool replaying() noexcept {return xr.replaying;}
// XrHarness::events: a LIVE event while a replay plays (reference-space change = system / Guardian recenter, focus change) is
// ignored - the replayed poses are in the recorded LOCAL space and the recorded recenters / focus are re-applied. Counted.
inline bool ignoreLive() noexcept {if(!xr.replaying)return false;play::liveIgnored.fetch_add(1,std::memory_order_relaxed);return true;}
// Session focus as the input gates see it (tracking gate, xrSyncActions gate, ControllerSample::focused).
inline bool focus(bool live) noexcept {
    if(!xr.on)return live;
    const bool f=xr.replaying?(play::cur.flags&flag::focused)!=0:live;
    if(xr.capturing&&f)xr.f.flags|=flag::focused;
    return f;
}
inline void note(std::uint32_t f) noexcept {if(xr.capturing)xr.f.flags|=f;} // flag::spaceChange / flag::userRecenter applied this frame
// xrLocateViews result (raw: before the position fallback / synthetic head motion).
// Returns true when replaced.
inline bool views(std::uint32_t& flags,std::uint32_t& count,float (&eye)[2][7],float (&fov)[2][4]) noexcept {
    if(!xr.on)return false;
    if(xr.replaying){
        if(play::cur.flags&flag::viewsLocated){flags=play::cur.viewFlags;count=play::cur.viewCount;
            std::memcpy(eye,play::cur.eye,sizeof eye);std::memcpy(fov,play::cur.fov,sizeof fov);}
        else{flags=0;count=0;} // recorded: no views this frame -> an invalid-views frame
    }
    if(xr.capturing){xr.f.flags|=flag::viewsLocated;xr.f.viewFlags=flags;xr.f.viewCount=count;std::memcpy(xr.f.eye,eye,sizeof eye);std::memcpy(xr.f.fov,fov,sizeof fov);}
    return xr.replaying;
}
// locateHands: the xrSyncActions / focus gate (true = the actions were read).
inline bool handsGate(bool live) noexcept {
    if(!xr.on)return live;
    const bool g=xr.replaying?(play::cur.flags&flag::handsLocated)!=0:live;
    if(xr.capturing&&g)xr.f.flags|=flag::handsLocated;
    return g;
}
// locateHands: aim action isActive (aimActive) + xrLocateSpace flags / pose per hand.
inline void aim(std::uint32_t (&flags)[2],float (&pose)[2][7]) noexcept {
    if(!xr.on)return;
    if(xr.replaying){std::memcpy(flags,play::cur.aimFlags,sizeof flags);std::memcpy(pose,play::cur.aim,sizeof pose);}
    if(xr.capturing){std::memcpy(xr.f.aimFlags,flags,sizeof flags);std::memcpy(xr.f.aim,pose,sizeof pose);}
}
// ControllerActions::read (controllerPath) / the non-controller recenter read: every action state of this read.
inline void pad(PadRaw& raw,bool controllerPath) noexcept {
    if(!xr.on)return;
    if(xr.replaying)play::pad(raw);
    if(xr.capturing){xr.f.pad=raw;xr.f.flags|=flag::padRead|(controllerPath?flag::controllerPath:0u);}
}
// After xrEndFrame: record / replay start at the first world stereo frame; commit while RECORDING; 30 min cap.
inline void frameEnd(bool gameStereo,bool world) noexcept {
    if(gameStereo){
        if(rec::state.load(std::memory_order_relaxed)==rec::Armed&&xr.capturing)rec::start(xr.wake,realtimeNs());
        if(play::track&&play::state.load(std::memory_order_relaxed)==play::Ready)play::begin(xr.wake);
    }
    if(!xr.capturing||rec::state.load(std::memory_order_relaxed)!=rec::Recording)return;
    xr.f.flags|=(gameStereo?flag::gameStereo:0u)|(world?flag::world:0u);xr.f.tNs=std::int64_t(xr.wake)-rec::startMono;
    rec::commit(xr.f);
    if(xr.f.tNs>=maxRecordNs)rec::stop("30 min");
}
inline void sessionEnd() noexcept {rec::stop("session end");}

// ---- report: vr.inputRec / vr.inputReplay ----
inline const char* recStateName(int s) noexcept {static const char* n[]{"OFF","ARMED","RECORDING","STOPPING","DONE","FAILED"};return s>=0&&s<6?n[s]:"?";}
inline const char* playStateName(int s) noexcept {static const char* n[]{"OFF","LOADING","READY","PLAYING","DONE","FAILED"};return s>=0&&s<6?n[s]:"?";}
inline std::string recordReport() {
    std::string f,e;{std::lock_guard g(textMutex);f=rec::file;e=rec::error;}
    char b[512];std::snprintf(b,sizeof b,"{\"state\":\"%s\",\"frames\":%llu,\"bytes\":%llu,\"file\":\"%s\",\"droppedChunks\":%llu,\"droppedFrames\":%llu,"
        "\"durationS\":%.1f,\"error\":\"%s\"}",recStateName(rec::state.load()),(unsigned long long)rec::frames.load(),(unsigned long long)rec::bytes.load(),
        f.c_str(),(unsigned long long)rec::droppedChunks.load(),(unsigned long long)rec::droppedFrames.load(),double(rec::lastTNs.load())*1e-9,e.c_str());
    return b;
}
inline std::string replayReport() {
    std::string f,e,build;{std::lock_guard g(textMutex);f=play::file;e=play::error;build=play::recordedBuild;}
    char b[640];std::snprintf(b,sizeof b,"{\"state\":\"%s\",\"file\":\"%s\",\"frames\":%llu,\"records\":%llu,\"elapsedS\":%.2f,\"durationS\":%.2f,"
        "\"lateFrames\":%llu,\"skippedRecords\":%llu,\"edges\":%llu,\"edgesLate\":%llu,\"edgesCollapsed\":%llu,\"spaceChanges\":%llu,\"liveEventsIgnored\":%llu,\"recordedBuild\":\"%.16s\",\"sameBuild\":%s,\"error\":\"%s\"}",
        playStateName(play::state.load()),f.c_str(),(unsigned long long)play::frames.load(),(unsigned long long)play::records.load(),
        double(play::elapsedNs.load())*1e-9,double(play::durationNs.load())*1e-9,(unsigned long long)play::lateFrames.load(),
        (unsigned long long)play::skipped.load(),(unsigned long long)play::edges.load(),(unsigned long long)play::edgesLate.load(),
        (unsigned long long)play::edgesCollapsed.load(),(unsigned long long)play::spaceChanges.load(),(unsigned long long)play::liveIgnored.load(),build.c_str(),
        build.empty()?"null":build==buildIdText?"true":"false",e.c_str());
    return b;
}
} // namespace rdrvr::vr::inputrec
