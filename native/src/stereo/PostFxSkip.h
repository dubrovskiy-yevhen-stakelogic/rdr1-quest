#pragma once
// STEREO TEST 16 SKIP POSTFX and GRAPHICS -> POST FX OFF (VR) / ON / OFF FAST.
// The engine work that sagRenderer::RenderFrame (@0x2a5810c) runs AFTER the stereo pair was recorded (the pair is
// recorded inside RenderScene_RenderThread, bl @0x2a585c8). Call sites after it (librdr 1.58 arm64, llvm-objdump; every
// GOT checked with llvm-readelf -r):
//   sagWorld::DrawEnd bl @0x2a586d0             - NOT touched: virtual target world+0x1d480 vtable+0x30 is unknown and
//                                                 may feed the next frame.
//   sagFxManager::DrawLowresParticles bl @0x2a586e0  GOT 0x2e5cae0  return 0x2a586e4 (other caller: tail @0x2a50d34)
//   OpticsManager::DrawSunOptics bl @0x2a58700       GOT 0x2e5d4d8  return 0x2a58704 (other caller @0x2a46140 = inside
//                                                                                   rdrPostFX::ProcessRDR2, covered)
//   PPPElements::Process bl @0x2a58714               GOT 0x2e5d620  return 0x2a58718
//   rage::grPostFX::Process(bool) bl @0x2a58744      GOT 0x2e22f78  return 0x2a58748 (BeginPostProcess,
//       ProcessPostProcess -> effect vtable+0x20 -> rdrPostFX::ProcessRDR2 = TAA/DOF/bloom/luminance/tonemap, EndPostProcess)
//   PPPElements::DrawPPPElements bl @0x2a58758      GOT 0x2e5d630  return 0x2a5875c
// then grcWorldIdentity (the VR HUD producer starts its OWN cleared target there, EngineHudPass) and hudManager::Draw.
//
// OFF (VR) / STEREO TEST 16 = "bind only": the functions are NOT skipped (their render-target lock/unlock pairs span the
// calls: EndPostProcess unlocks what ProcessRDR2 left locked; RenderFrame rebinds when grPostFX+0x470 is set), every d11
// draw they issue on this thread keeps d11::VkContext_c::BindInputLayout() @0x18af214 (pipeline + descriptors +
// FlushFrameBuffer = render-pass begin with its pending clears) and only the vkCmdDraw* is dropped (Draw/DrawIndexed/
// DrawInstanced/DrawIndexedInstanced 0x18af928/0x18af990/0x18afa84/0x18afa00). Render passes, clears, barriers, target
// locks and all CPU work stay native. Active only while the stereo pair of this RenderScene was recorded, so theater/menu
// frames keep their post-processing.
// The headset never sees the post output: both eyes are copied into slot-private mailbox images right after their colour
// stage (EngineStereoPass.cpp:89-121, GameStereoCapture.cpp:99-115), the mailbox copies the BGRA8 eye (or runs the mod FXAA)
// and blits NEAREST into the XR swapchain (StereoMailbox.cpp:254-261/405-415). Post output only feeds the 1346x720 2D
// surface (theater fallback / flat mirror).
//
// OFF FAST (namespace deep): rdrPostFX::ProcessRDR2 (GOT 0x2e5f598, reached from ProcessPostProcess `blr x8` @0x23eb078
// through the grPostFXEffectRDR2::Process thunk) is replaced for one frame by what grPostFXEffectNone would do:
// grPostFX::ProcessNone (one BlitToRT with rt=null of [self+0x108]) + StoreMotionBlurMtxs, after replaying the target lock
// ProcessRDR2 natively leaves open. The D11 factory (grcTextureFactoryD11) is a slot model, not a stack: Lock(+0x98)
// rebinds slot 0/depth, Unlock(+0xa0) and UnlockRenderTargetBindBackBuffer(+0xa8, used by BlitToRT whenever +0x470 is
// clear) clear it, EndPostProcess always ends with Unlock(0). Deep is used only after the per-call lock trace of the native
// ProcessRDR2 (all three taps) was identical for 30 consecutive pair frames with a representable shape (0: ends with an
// unlock; +1: ends with the single open lock), and a watchdog checks the grPostFX::Process lock balance in every deep frame.
#include "StereoTestMode.h"
#include "vr/VrRain.h" // RAIN QUALITY (DropScope around the early weather call)
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <time.h>
#endif

namespace rdrvr::engine::postfx {
enum Site : unsigned {LowresParticles,SunOptics,PppProcess,PostFxProcess,PppDraw,SiteCount};
inline constexpr const char* siteNames[SiteCount]{"lowresParticles","sunOptics","pppProcess","postFxProcess","pppDraw"};
struct Route {std::uintptr_t original,got,ret;std::array<unsigned char,4> call;std::array<unsigned char,16> code;};
inline constexpr std::array<Route,SiteCount> routes{{
    {0x2931d18,0x2e5cae0,0x2a586e4,{136,64,9,148},{
#include "gamecode/PostFxSkip_2931ce4_16_8d.inc"
}},  // sagFxManager::DrawLowresParticles
    {0x296d0e8,0x2e5d4d8,0x2a58704,{124,69,9,148},{
#include "gamecode/PostFxSkip_296d0e8_16_8d.inc"
}},          // OpticsManager::DrawSunOptics(bool)
    {0x29753f8,0x2e5d620,0x2a58718,{27,70,9,148},{
#include "gamecode/PostFxSkip_184ad6c_16_8d.inc"
}},   // PPPElements::Process
    {0x23e8f6c,0x2e22f78,0x2a58748,{187,114,7,148},{
#include "gamecode/PostFxSkip_1843aa8_16_8d.inc"
}},   // rage::grPostFX::Process(bool)
    {0x2975888,0x2e5d630,0x2a5875c,{18,70,9,148},{
#include "gamecode/PostFxSkip_183ed68_16_8d.inc"
}}}}; // PPPElements::DrawPPPElements
inline constexpr std::uintptr_t bindInputLayoutOffset=0x18af214; // d11::VkContext_c::BindInputLayout()
inline constexpr std::array<unsigned char,32> bindInputLayoutFingerprint{
#include "gamecode/PostFxSkip_186348c_32_8d.inc"
};

// The POST FX row. OFF (VR) = bind-only (default), ON = native, OFF FAST = deep (falls back to bind-only).
enum Request : unsigned {Off=0,On=1,OffFast=2,RequestCount=3};
inline constexpr const char* requestNames[RequestCount]{"OFF (VR)","ON","OFF FAST"};
inline constexpr const char* requestName(unsigned r) noexcept {return r<RequestCount?requestNames[r]:"?";}
inline constexpr unsigned propertyInterval=64; // debug.rdrvr.postfx is re-read every 64 RenderScenes (scene 1, 65, ...)

inline std::atomic<bool> ready{};                          // site hooks + bind-only draw path installed
inline std::atomic<std::uint64_t> sceneSerial{},pairSerial{}; // render thread: RenderScene entry / stereo pair recorded
inline std::atomic<unsigned> liveScopes{};                 // any thread inside an active site scope (gate before TLS)
inline thread_local unsigned threadScopes{};
inline thread_local std::uint64_t threadDropped{};
// PX NO-FLUSH: inside an ACTIVE RenderFrame site scope (the five sites above, never a tail scope) a
// dropped draw also skips d11::VkContext_c::BindInputLayout, the only draw-side caller of FlushFrameBuffer 0x18add18 (= the d11
// render-pass begin), so a post target that only receives dropped draws never begins a pass. Pending clears stay pending and
// land at the next flush (the HUD producer's +0x478 pass / EndFrame; 2D surface only). noFlushWanted is published per
// RenderScene by TraceEntry from the lever mask of the frame; threadNoFlush/threadTail are TLS read only after dropDraw() is true.
inline std::atomic<bool> noFlushWanted{};
inline thread_local unsigned threadNoFlush{},threadTail{};
inline std::atomic<std::uint64_t> noFlushScopes{},bindsSkipped{};
inline bool noFlushNow() noexcept {return threadNoFlush!=0&&threadTail==0;}
struct SiteStats {
    std::atomic<std::uint64_t> calls{},active{},droppedDraws{};
    std::atomic<std::uint64_t> cpuNsSkipped{},cpuNsNative{},cpuNsDeep{},timedSkipped{},timedNative{},timedDeep{}; // exact calls only
};
inline std::array<SiteStats,SiteCount> sites{};
inline std::atomic<std::uint64_t> pairs{};
inline std::atomic<unsigned> request{Off};                 // POST FX row, latched per RenderScene (sceneBegin)
inline std::atomic<int> property{-1};                      // debug.rdrvr.postfx 0..2, -1 absent/invalid
inline std::atomic<unsigned> lastMode{};                   // STEREO TEST at the last exact grPostFX::Process (report)
inline std::atomic<std::uint64_t> skippedScenes{},nativePairScenes{},deepScenes{},staleTheaterCaptures{},deepTheaterCaptures{};
inline std::atomic<std::uint64_t> staleWorldCaptures{}; // Stale captures while the presentation is WORLD (see notePresent)
inline std::atomic<std::uint64_t> presentChecks{},propertyReads{},lastSkippedScene{},lastDeepScene{},lastNativePairScene{};

// Row value with the property override applied (no readiness, no STEREO TEST 16, no deep fallback).
inline unsigned requested() noexcept {
    const int p=property.load(std::memory_order_relaxed);
    if(p>=0&&p<int(RequestCount))return unsigned(p);
    const unsigned r=request.load(std::memory_order_relaxed);
    return r<RequestCount?r:unsigned(Off);
}
inline bool pairScene() noexcept {
    const auto scene=sceneSerial.load(std::memory_order_relaxed);
    return scene&&pairSerial.load(std::memory_order_relaxed)==scene;
}
inline std::uint64_t threadCpuNs() noexcept {
#if defined(__ANDROID__)
    timespec t{};if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t))return 0;
    return std::uint64_t(t.tv_sec)*1000000000ull+std::uint64_t(t.tv_nsec);
#else
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// Arguments of grcTextureFactoryD11::LockRenderTarget (factory vtable+0x98, interposed by GameXr gameTargetLock).
struct LockArgs {int index;const void* color;const void* depth;unsigned face;bool changeDepth;int resolve;unsigned mip;bool window;};
inline bool sameLock(const LockArgs& a,const LockArgs& b) noexcept {
    return a.index==b.index&&a.color==b.color&&a.depth==b.depth&&a.face==b.face&&a.changeDepth==b.changeDepth&&
        a.resolve==b.resolve&&a.mip==b.mip&&a.window==b.window;
}

namespace deep {
// Fingerprints (librdr 1.58 arm64; verified with llvm-objdump/llvm-readelf).
inline constexpr std::uintptr_t gotRdr2=0x2e5f598;          // R_AARCH64_JUMP_SLOT rdrPostFX::ProcessRDR2 (PLT 0x2cade70 reads it)
inline constexpr std::uintptr_t rdr2=0x2a43e28;             // rdrPostFX::ProcessRDR2()
inline constexpr std::array<unsigned char,16> rdr2Fingerprint{
#include "gamecode/PostFxSkip_1913a80_16_8d.inc"
};
inline constexpr std::uintptr_t thunk=0x2a43e20;            // grPostFXEffectRDR2::Process: mov x0,x1; b ProcessRDR2@plt
inline constexpr std::array<unsigned char,8> thunkFingerprint{224,3,1,170,19,168,9,20};
inline constexpr std::uintptr_t callerCall=0x23eb078,callerReturn=0x23eb07c; // ProcessPostProcess: blr x8 (effect vtable+0x20)
inline constexpr std::array<unsigned char,4> callerCallWord{0,1,63,214};
inline constexpr std::uintptr_t processNone=0x23ef624;      // grPostFX::ProcessNone(): BlitToRT(rt=null,[+0x108],[+0x3f8],[+0x1b0])
inline constexpr std::array<unsigned char,16> processNoneFingerprint{
#include "gamecode/PostFxSkip_23ef624_16_8d.inc"
};
inline constexpr std::uintptr_t storeMotionBlur=0x23eac04;  // grPostFX::StoreMotionBlurMtxs() (only caller: ProcessRDR2+0x288)
inline constexpr std::array<unsigned char,16> storeMotionBlurFingerprint{
#include "gamecode/PostFxSkip_23eac04_16_8d.inc"
};
inline constexpr std::uintptr_t postFxInstanceGot=0x2de10b8; // R_AARCH64_GLOB_DAT rage::grPostFX::sm_instance
inline constexpr std::uintptr_t factoryInstanceGot=0x2ddffb8; // R_AARCH64_GLOB_DAT rage::grcTextureFactory::sm_Instance
inline constexpr std::uintptr_t flag470=0x470;              // RenderFrame re-locks [+0x478] when set (0x2a58768..0x2a587dc)
inline constexpr std::uintptr_t factoryLockSlot=0x98,factoryUnlockSlot=0xa0,factoryUnlockBackBufferSlot=0xa8; // + factory vtable
inline constexpr std::uintptr_t unlockBackBuffer=0x1ac2d0c; // grcTextureFactoryD11::UnlockRenderTargetBindBackBuffer (vtable 0x2cde218)
inline constexpr std::array<unsigned char,32> unlockBackBufferFingerprint{
#include "gamecode/PostFxSkip_1ac2a8c_32_8d.inc"
};
// resetLimit reference changes within resetWindow pair samples = the final target is not stable (session-permanent).
inline constexpr unsigned calibrationCalls=30,reverifyInterval=128,resetLimit=32,resetWindow=256,stackMax=8;

enum class Status : unsigned {Calibrating,Ok,NoLockTaps,LockDelta,LockShape,FinalTargetUnstable,LockImbalance};
enum class Event : unsigned char {None,Lock,Unlock,UnlockBackBuffer};
inline constexpr const char* eventName(Event e) noexcept {
    return e==Event::Lock?"lock":e==Event::Unlock?"unlock":e==Event::UnlockBackBuffer?"unlockBackBuffer":"none";
}
enum Decision : unsigned {Native,Deep};

// Installation (TraceEntry): installed = ProcessRDR2 GOT hook live; unlockTap = vtable+0xa8 tap live.
inline std::atomic<bool> installed{},unlockTap{};
using Reader=bool(*)(std::uintptr_t address,void* out,std::size_t size);
using LockFn=void(*)(void*,int,const void*,const void*,unsigned,bool,int,unsigned,bool);
struct Config {std::uintptr_t base{};Reader read{};std::uintptr_t instanceVar{},factoryVar{},lockEntry{},processNone{},storeMotionBlur{};};
inline Config config{};                                    // written once before the GOT hook goes live

// One native ProcessRDR2 call, as seen through the three factory taps (render thread only).
struct Sample {
    int delta{};unsigned locks{},unlocks{},backBufferUnlocks{},depth{};bool underflow{},overflow{};
    Event last{};LockArgs lastLock{};LockArgs top{};int siteDelta{};
};
struct Reference {bool valid{};int delta{};Event last{};LockArgs lock{};int siteDelta{};};
struct Track {
    bool track{},inRdr2{},scopePair{},scopeActive{},scopeWantFast{},samplePending{},deepRan{},entryValid{};
    unsigned char entryFlag{};
    int siteLocks{},siteUnlocks{};
    unsigned locks{},unlocks{},backBufferUnlocks{},depth{};bool underflow{},overflow{};
    Event last{};LockArgs lastLock{};std::array<LockArgs,stackMax> stack{};
    Sample sample{};
    std::uintptr_t lockFn{},factory{};
};
inline thread_local Track tl{};
struct Fallbacks {std::atomic<std::uint64_t> caller{},scope{},instance{},flag470{},factory{},calibration{};};
struct FinalLock {std::atomic<bool> valid{},window{},hasDepth{},changeDepth{};std::atomic<int> index{},resolve{};std::atomic<unsigned> face{},mip{};};
struct State {
    std::atomic<bool> calibrated{},disabled{},permanent{},lastEngineFallback{};
    std::atomic<Status> status{Status::Calibrating};
    std::atomic<int> statusDelta{},rdr2Delta{},siteDelta{},lastDelta{};
    std::atomic<unsigned> lastEvent{};
    std::atomic<std::uint32_t> stableCalls{};
    std::array<std::atomic<std::uint64_t>,5> deltaHist{}; // <=-2,-1,0,+1,>=+2 (every tracked native ProcessRDR2)
    std::atomic<std::uint64_t> calls{},deepCalls{},lockReplays{},noneCalls{},motionBlurCalls{},faults{},reverifyCalls{},reverifyCounter{};
    std::atomic<std::uint64_t> samples{},theaterSamples{},resets{},recalibrations{},interruptions{},untracked{},ignoredSamples{},noTapSamples{},extraSamples{};
    Fallbacks fallbacks{};
    FinalLock finalLock{};
};
inline State state{};
inline Reference reference{};                               // render thread only (the report reads State atomics)
inline std::uint64_t pairSamples{},resetWindowStart{};      // render thread only
inline unsigned recentResets{};

inline bool readBytes(std::uintptr_t address,void* out,std::size_t size) noexcept {
    if(!address||!out||!size)return false;
    if(config.read)return config.read(address,out,size);
    std::memcpy(out,reinterpret_cast<const void*>(address),size);return true;
}
template<class T> inline bool readValue(std::uintptr_t address,T& out) noexcept {return readBytes(address,&out,sizeof out);}
// Install: reads the two GLOB_DAT entries once (the variables never move); lockEntry = the expected factory vtable+0x98
// value (gameTargetLock), 0 = unchecked. Returns false (deep stays NOT_INSTALLED) when a GOT read fails.
inline bool configure(std::uintptr_t engineBase,Reader read,std::uintptr_t expectedLockEntry,bool storeMotionBlurVerified) noexcept {
    Config c{};c.base=engineBase;c.read=read;config=c;
    std::uintptr_t instanceVar{},factoryVar{};
    if(!engineBase||!readValue(engineBase+postFxInstanceGot,instanceVar)||!instanceVar||
       !readValue(engineBase+factoryInstanceGot,factoryVar)||!factoryVar){config=Config{};return false;}
    c.instanceVar=instanceVar;c.factoryVar=factoryVar;c.lockEntry=expectedLockEntry;
    c.processNone=engineBase+processNone;c.storeMotionBlur=storeMotionBlurVerified?engineBase+storeMotionBlur:0;
    config=c;return true;
}

inline bool usable() noexcept {
    return installed.load(std::memory_order_relaxed)&&unlockTap.load(std::memory_order_relaxed)&&state.calibrated.load(std::memory_order_relaxed)&&
        !state.disabled.load(std::memory_order_relaxed)&&!state.permanent.load(std::memory_order_relaxed);
}
// The menu's FALLBACK: OFF FAST cannot become active in this session (or the taps are missing).
inline bool failed() noexcept {
    return !installed.load(std::memory_order_relaxed)||!unlockTap.load(std::memory_order_relaxed)||state.disabled.load(std::memory_order_relaxed)||
        state.permanent.load(std::memory_order_relaxed)||state.status.load(std::memory_order_relaxed)==Status::NoLockTaps;
}
inline std::string statusName() {
    if(!installed.load())return "NOT_INSTALLED";
    if(!unlockTap.load())return "DEEP_NO_UNLOCK_TAP";
    switch(state.status.load()){
    case Status::Ok:return "OK";
    case Status::NoLockTaps:return "DEEP_NO_LOCK_TAPS";
    case Status::LockDelta:return "DEEP_LOCK_DELTA_"+std::to_string(state.statusDelta.load());
    case Status::LockShape:return "DEEP_LOCK_SHAPE";
    case Status::FinalTargetUnstable:return "DEEP_FINAL_TARGET_UNSTABLE";
    case Status::LockImbalance:return "DEEP_LOCK_IMBALANCE";
    default:return "CALIBRATING";
    }
}
inline void setStatus(Status s,int d=0) noexcept {state.statusDelta.store(d,std::memory_order_relaxed);state.status.store(s,std::memory_order_relaxed);}
// Session-permanent: deep is never used again (OFF FAST = bind-only, menu FALLBACK).
inline void fail(Status s,int d=0) noexcept {state.permanent.store(true);state.calibrated.store(false);reference.valid=false;setStatus(s,d);}
inline void forget() noexcept {
    reference.valid=false;state.calibrated.store(false,std::memory_order_relaxed);state.stableCalls.store(0,std::memory_order_relaxed);
    if(!state.permanent.load(std::memory_order_relaxed)&&state.status.load(std::memory_order_relaxed)==Status::Ok)setStatus(Status::Calibrating);
}
// A grPostFX::Process without a stereo pair (theater/menu): the calibration is valid only for an uninterrupted run of pair frames.
inline void interrupt() noexcept {
    if(reference.valid||state.calibrated.load(std::memory_order_relaxed))state.interruptions.fetch_add(1,std::memory_order_relaxed);
    forget();
}
// Representable: ends with an unlock and nothing open (0), or ends with the single open lock (+1). Never deeper than 8.
inline bool shapeOk(const Sample& s) noexcept {
    if(s.underflow||s.overflow||!s.locks)return false;
    if(s.delta==1)return s.last==Event::Lock&&s.depth==1&&sameLock(s.top,s.lastLock);
    return s.delta==0&&s.depth==0&&(s.last==Event::Unlock||s.last==Event::UnlockBackBuffer);
}
inline bool matches(const Reference& r,const Sample& s) noexcept {
    return r.valid&&r.delta==s.delta&&r.last==s.last&&r.siteDelta==s.siteDelta&&(s.delta!=1||sameLock(r.lock,s.lastLock));
}
inline void publishReference(const Sample& s) noexcept {
    auto& f=state.finalLock;const bool open=s.delta==1;
    f.valid.store(open);f.index.store(open?s.lastLock.index:0);f.face.store(open?s.lastLock.face:0u);f.resolve.store(open?s.lastLock.resolve:0);
    f.mip.store(open?s.lastLock.mip:0u);f.window.store(open&&s.lastLock.window);f.hasDepth.store(open&&s.lastLock.depth);
    f.changeDepth.store(open&&s.lastLock.changeDepth);
    state.rdr2Delta.store(s.delta);state.siteDelta.store(s.siteDelta);state.lastEvent.store(unsigned(s.last));
}
// End of the exact grPostFX::Process that contained one native ProcessRDR2 (siteDelta filled in by the scope).
inline void commit(const Sample& s,bool pairFrame) noexcept {
    auto& st=state;st.samples.fetch_add(1,std::memory_order_relaxed);
    st.deltaHist[s.delta<=-2?0u:s.delta>=2?4u:unsigned(s.delta+2)].fetch_add(1,std::memory_order_relaxed);
    st.lastDelta.store(s.delta,std::memory_order_relaxed);
    if(!pairFrame){st.theaterSamples.fetch_add(1,std::memory_order_relaxed);return;}
    if(++pairSamples-resetWindowStart>resetWindow){resetWindowStart=pairSamples;recentResets=0;}
    if(st.permanent.load()||st.disabled.load()||!unlockTap.load())return; // without the +0xa8 tap the trace is incomplete
    if(!s.locks){st.noTapSamples.fetch_add(1,std::memory_order_relaxed);forget();setStatus(Status::NoLockTaps);return;}
    if(s.delta<0||s.delta>1){fail(Status::LockDelta,s.delta);return;}
    if(!shapeOk(s)){fail(Status::LockShape,s.delta);return;}
    if(!matches(reference,s)){
        if(reference.valid){
            if(st.calibrated.load())st.recalibrations.fetch_add(1,std::memory_order_relaxed);
            st.resets.fetch_add(1,std::memory_order_relaxed);
            if(++recentResets>=resetLimit){fail(Status::FinalTargetUnstable);return;}
        }
        reference=Reference{true,s.delta,s.last,s.lastLock,s.siteDelta};
        st.calibrated.store(false);st.stableCalls.store(1);publishReference(s);setStatus(Status::Calibrating);
        return;
    }
    const auto n=st.stableCalls.load(std::memory_order_relaxed);
    st.stableCalls.store(n<0xffffffffu?n+1:n,std::memory_order_relaxed);
    if(n+1>=calibrationCalls&&!st.calibrated.load()){st.calibrated.store(true);setStatus(Status::Ok);}
}
// Deep frame: the whole grPostFX::Process must have the calibrated native lock balance.
inline void watchdog(int siteDelta) noexcept {
    if(reference.valid&&siteDelta==reference.siteDelta)return;
    state.faults.fetch_add(1);state.disabled.store(true);fail(Status::LockImbalance,siteDelta);
}

// Per ProcessRDR2 call (TraceEntry postRdr2). Native for everything that is not an OFF FAST pair frame; for those the
// reason is counted in fallbacks. Every 128th deep candidate runs natively once more (re-verification sample).
inline Decision plan(bool exact,void* self,std::uintptr_t engineBase) noexcept {
    auto& st=state;auto& t=tl;st.calls.fetch_add(1,std::memory_order_relaxed);
    t.entryValid=false;t.entryFlag=0;t.lockFn=0;t.factory=0;
    if(!exact){st.fallbacks.caller.fetch_add(1,std::memory_order_relaxed);return Native;}
    if(!t.track){st.fallbacks.scope.fetch_add(1,std::memory_order_relaxed);return Native;}
    const auto selfAddress=reinterpret_cast<std::uintptr_t>(self);
    unsigned char flag=0xff;
    t.entryValid=selfAddress&&readValue(selfAddress+flag470,flag);t.entryFlag=flag;
    if(!(t.scopeActive&&t.scopePair&&t.scopeWantFast))return Native;   // ON, OFF (VR), STEREO TEST 16, theater: native by design
    std::uintptr_t instance{};
    if(engineBase!=config.base||!config.instanceVar||!readValue(config.instanceVar,instance)||instance!=selfAddress){
        st.fallbacks.instance.fetch_add(1,std::memory_order_relaxed);st.lastEngineFallback.store(true,std::memory_order_relaxed);return Native;}
    if(!t.entryValid||flag){st.fallbacks.flag470.fetch_add(1,std::memory_order_relaxed);st.lastEngineFallback.store(true,std::memory_order_relaxed);return Native;}
    if(!usable()||!config.processNone){st.fallbacks.calibration.fetch_add(1,std::memory_order_relaxed);return Native;}
    if(st.reverifyCounter.fetch_add(1,std::memory_order_relaxed)%reverifyInterval==reverifyInterval-1){
        st.reverifyCalls.fetch_add(1,std::memory_order_relaxed);return Native;}
    if(reference.delta==1){
        std::uintptr_t factory{},vtable{},entry{};
        if(!readValue(config.factoryVar,factory)||!factory||!readValue(factory,vtable)||!vtable||!readValue(vtable+factoryLockSlot,entry)||!entry||
           (config.lockEntry&&entry!=config.lockEntry)){
            st.fallbacks.factory.fetch_add(1,std::memory_order_relaxed);st.lastEngineFallback.store(true,std::memory_order_relaxed);return Native;}
        t.lockFn=entry;t.factory=factory;
    }
    st.lastEngineFallback.store(false,std::memory_order_relaxed);
    return Deep;
}
// RAII around the native ProcessRDR2 call (any mode): records the lock trace of this call.
class Calibrate {
    bool armed_{};
public:
    Calibrate() noexcept {
        auto& t=tl;
        if(!t.track){state.untracked.fetch_add(1,std::memory_order_relaxed);return;}
        if(t.inRdr2)return;
        armed_=true;t.inRdr2=true;t.locks=t.unlocks=t.backBufferUnlocks=t.depth=0;t.underflow=t.overflow=false;t.last=Event::None;t.lastLock=LockArgs{};
    }
    ~Calibrate() {
        if(!armed_)return;
        auto& t=tl;t.inRdr2=false;
        if(!t.entryValid||t.entryFlag){state.ignoredSamples.fetch_add(1,std::memory_order_relaxed);return;} // +0x470 set (deep never runs then) or no plan() entry
        Sample s{};s.locks=t.locks;s.unlocks=t.unlocks;s.backBufferUnlocks=t.backBufferUnlocks;s.depth=t.depth;s.underflow=t.underflow;s.overflow=t.overflow;
        s.delta=int(t.locks)-int(t.unlocks+t.backBufferUnlocks);s.last=t.last;s.lastLock=t.lastLock;
        s.top=t.depth>=1&&t.depth<=stackMax?t.stack[t.depth-1]:LockArgs{};
        if(t.samplePending)state.extraSamples.fetch_add(1,std::memory_order_relaxed);
        t.sample=s;t.samplePending=true;
    }
    Calibrate(const Calibrate&)=delete;Calibrate& operator=(const Calibrate&)=delete;
};
// Deep body (plan() returned Deep): Suspend bind-only so the None blit draws (the 2D surface stays fresh), replay the
// calibrated open lock through the factory vtable+0x98 (gameTargetLock), ProcessNone(self), StoreMotionBlurMtxs(self).
class Run {
public:
    Run(std::uintptr_t engineBase,void* self) noexcept {
        (void)engineBase;
        auto& st=state;auto& t=tl;st.deepCalls.fetch_add(1,std::memory_order_relaxed);
        const unsigned suspended=threadScopes;threadScopes=0;
        if(t.lockFn){
            const auto& a=reference.lock;
            reinterpret_cast<LockFn>(t.lockFn)(reinterpret_cast<void*>(t.factory),a.index,a.color,a.depth,a.face,a.changeDepth,a.resolve,a.mip,a.window);
            st.lockReplays.fetch_add(1,std::memory_order_relaxed);
        }
        reinterpret_cast<void(*)(void*)>(config.processNone)(self);st.noneCalls.fetch_add(1,std::memory_order_relaxed);
        if(config.storeMotionBlur){reinterpret_cast<void(*)(void*)>(config.storeMotionBlur)(self);st.motionBlurCalls.fetch_add(1,std::memory_order_relaxed);}
        threadScopes=suspended;t.deepRan=true;t.lockFn=0;t.factory=0;
    }
    Run(const Run&)=delete;Run& operator=(const Run&)=delete;
};
inline std::string report() {
    auto& st=state;const auto& f=st.finalLock;std::ostringstream o;
    o<<"{\"installed\":"<<installed.load()<<",\"unlockTap\":"<<unlockTap.load()<<",\"status\":\""<<statusName()<<"\",\"calibrated\":"<<st.calibrated.load()
     <<",\"stableCalls\":"<<st.stableCalls.load()<<",\"rdr2Delta\":"<<st.rdr2Delta.load()<<",\"siteDelta\":"<<st.siteDelta.load()
     <<",\"lastDelta\":"<<st.lastDelta.load()<<",\"deltaHist\":[";
    for(unsigned i=0;i<st.deltaHist.size();++i)o<<(i?",":"")<<st.deltaHist[i].load();
    o<<"],\"finalEvent\":\""<<eventName(Event(st.lastEvent.load()))<<"\",\"finalLock\":{\"valid\":"<<f.valid.load()<<",\"index\":"<<f.index.load()
     <<",\"face\":"<<f.face.load()<<",\"resolve\":"<<f.resolve.load()<<",\"mip\":"<<f.mip.load()<<",\"window\":"<<f.window.load()
     <<",\"hasDepth\":"<<f.hasDepth.load()<<",\"changeDepth\":"<<f.changeDepth.load()<<'}'
     <<",\"calls\":"<<st.calls.load()<<",\"deepCalls\":"<<st.deepCalls.load()<<",\"lockReplays\":"<<st.lockReplays.load()<<",\"noneCalls\":"<<st.noneCalls.load()
     <<",\"motionBlurCalls\":"<<st.motionBlurCalls.load()<<",\"storeMotionBlur\":"<<(config.storeMotionBlur!=0)<<",\"reverifyCalls\":"<<st.reverifyCalls.load()
     <<",\"fallbacks\":{\"caller\":"<<st.fallbacks.caller.load()<<",\"scope\":"<<st.fallbacks.scope.load()<<",\"instance\":"<<st.fallbacks.instance.load()
     <<",\"flag470\":"<<st.fallbacks.flag470.load()<<",\"factory\":"<<st.fallbacks.factory.load()<<",\"calibration\":"<<st.fallbacks.calibration.load()<<'}'
     <<",\"faults\":"<<st.faults.load()<<",\"disabled\":"<<st.disabled.load()<<",\"permanent\":"<<st.permanent.load()
     <<",\"samples\":"<<st.samples.load()<<",\"theaterSamples\":"<<st.theaterSamples.load()<<",\"resets\":"<<st.resets.load()<<",\"recalibrations\":"<<st.recalibrations.load()
     <<",\"interruptions\":"<<st.interruptions.load()<<",\"untracked\":"<<st.untracked.load()<<",\"ignoredSamples\":"<<st.ignoredSamples.load()
     <<",\"noTapSamples\":"<<st.noTapSamples.load()<<",\"extraSamples\":"<<st.extraSamples.load()<<'}';
    return o.str();
}
} // namespace deep

// The POST FX value in force: hooks missing -> ON; STEREO TEST 16 -> OFF (isolates bind-only even with ON or OFF FAST);
// property beats the row; OFF FAST without a usable deep variant -> OFF (bind-only).
inline unsigned effective(unsigned mode) noexcept {
    if(!ready.load(std::memory_order_relaxed))return On;
    if(mode==stereo::testModeSkipPostFx)return Off;
    const unsigned r=requested();
    return r==OffFast&&!deep::usable()?unsigned(Off):r;
}
// Theater and menu frames (no stereo pair in this RenderScene) keep their post-processing.
inline bool wanted(unsigned mode) noexcept {return pairScene()&&effective(mode)!=On;}
// _pf0 bind-only, _pf1 ON, _pf2 OFF FAST active. OFF FAST whose last candidate fell back for an engine reason
// (instance / +0x470 / factory) is reported as _pf0, the work that really ran.
inline unsigned signatureDigit(unsigned mode) noexcept {
    const unsigned e=effective(mode);
    return e==OffFast&&deep::state.lastEngineFallback.load(std::memory_order_relaxed)?unsigned(Off):e;
}
// runtime::postFxMenuState: 0 hooks missing, 1 ready, 2 OFF FAST calibrating, 3 OFF FAST active, 4 OFF FAST fallback/disabled.
inline int menuState() noexcept {
    if(!ready.load(std::memory_order_relaxed))return 0;
    if(requested()!=OffFast)return 1;
    if(deep::failed())return 4;
    if(!deep::state.calibrated.load(std::memory_order_relaxed))return 2;
    return deep::state.lastEngineFallback.load(std::memory_order_relaxed)?4:3;
}

// Factory lock taps (GameXr gameTargetLock/gameTargetUnlock, TraceEntry postUnlockBackBuffer). One TLS load unless the
// thread is inside an exact grPostFX::Process scope.
inline void noteLock(const LockArgs& a) noexcept {
    auto& t=deep::tl;if(!t.track)return;
    ++t.siteLocks;if(!t.inRdr2)return;
    ++t.locks;t.last=deep::Event::Lock;t.lastLock=a;
    if(t.depth<deep::stackMax)t.stack[t.depth]=a;else t.overflow=true;
    ++t.depth;
}
inline void noteUnlock(bool backBuffer=false) noexcept {
    auto& t=deep::tl;if(!t.track)return;
    ++t.siteUnlocks;if(!t.inRdr2)return;
    if(backBuffer){++t.backBufferUnlocks;t.last=deep::Event::UnlockBackBuffer;}else{++t.unlocks;t.last=deep::Event::Unlock;}
    if(t.depth)--t.depth;else t.underflow=true;
}

inline void sceneBegin(unsigned cfgPostFx) noexcept {
    const auto scene=sceneSerial.fetch_add(1,std::memory_order_relaxed)+1;
    request.store(cfgPostFx<RequestCount?cfgPostFx:unsigned(Off),std::memory_order_relaxed);
#if defined(__ANDROID__)
    if((scene-1)%propertyInterval==0){
        const int saved=errno;char value[PROP_VALUE_MAX]{};
        const int n=__system_property_get("debug.rdrvr.postfx",value);
        errno=saved;
        property.store(n==1&&value[0]>='0'&&value[0]<char('0'+RequestCount)?value[0]-'0':-1,std::memory_order_relaxed);
        propertyReads.fetch_add(1,std::memory_order_relaxed);
    }
#else
    (void)scene;
#endif
}
inline void sceneBegin() noexcept {sceneBegin(On);}
inline void pairRecorded() noexcept {pairSerial.store(sceneSerial.load(std::memory_order_relaxed),std::memory_order_relaxed);pairs.fetch_add(1,std::memory_order_relaxed);}
// Per engine draw (hot path): one relaxed load while no site scope is live anywhere.
inline bool dropDraw() noexcept {
    if(!liveScopes.load(std::memory_order_relaxed)||!threadScopes)return false;
    ++threadDropped;return true;
}
// RAII around one exact call site; nested scopes (DrawSunOptics inside ProcessRDR2) count once at the outer site.
// Exact calls are timed in thread-CPU time; the exact grPostFX::Process also opens the lock tracking of deep.
class Scope {
    unsigned site_;bool active_{},timed_{},tracking_{},pair_{},previousTrack_{},noFlush_{};std::uint64_t before_{},cpu0_{};
public:
    Scope(unsigned site,bool exact,unsigned mode) noexcept:site_(site){
        if(!exact||site>=SiteCount)return;
        sites[site].calls.fetch_add(1,std::memory_order_relaxed);
        timed_=true;pair_=pairScene();
        if(site==PostFxProcess){
            lastMode.store(mode,std::memory_order_relaxed);
            auto& t=deep::tl;tracking_=true;previousTrack_=t.track;
            t.track=true;t.inRdr2=false;t.siteLocks=t.siteUnlocks=0;t.samplePending=false;t.deepRan=false;
            t.scopePair=pair_;t.scopeActive=false;t.scopeWantFast=false;
            if(!pair_)deep::interrupt();
        }
        if(wanted(mode)){
            active_=true;before_=threadDropped;++threadScopes;liveScopes.fetch_add(1,std::memory_order_relaxed);
            if(tracking_){deep::tl.scopeActive=true;deep::tl.scopeWantFast=mode!=stereo::testModeSkipPostFx&&requested()==OffFast;}
            if(noFlushWanted.load(std::memory_order_relaxed)){noFlush_=true;++threadNoFlush;noFlushScopes.fetch_add(1,std::memory_order_relaxed);}
        }
        cpu0_=threadCpuNs();
    }
    ~Scope(){
        if(!timed_)return;
        if(noFlush_)--threadNoFlush;
        const auto cpu=threadCpuNs();const auto ns=cpu>cpu0_?cpu-cpu0_:0;
        bool deepRan=false;
        if(tracking_){
            auto& t=deep::tl;deepRan=t.deepRan;
            const int siteDelta=t.siteLocks-t.siteUnlocks;
            if(t.samplePending){t.samplePending=false;t.sample.siteDelta=siteDelta;deep::commit(t.sample,pair_);}
            if(deepRan)deep::watchdog(siteDelta);
            t.track=previousTrack_;t.inRdr2=false;t.deepRan=false;t.scopeActive=t.scopeWantFast=false;
        }
        auto& s=sites[site_];const auto scene=sceneSerial.load(std::memory_order_relaxed);
        if(active_){
            liveScopes.fetch_sub(1,std::memory_order_relaxed);--threadScopes;
            s.active.fetch_add(1,std::memory_order_relaxed);s.droppedDraws.fetch_add(threadDropped-before_,std::memory_order_relaxed);
            s.cpuNsSkipped.fetch_add(ns,std::memory_order_relaxed);s.timedSkipped.fetch_add(1,std::memory_order_relaxed);
            if(deepRan){s.cpuNsDeep.fetch_add(ns,std::memory_order_relaxed);s.timedDeep.fetch_add(1,std::memory_order_relaxed);}
            if(lastSkippedScene.exchange(scene,std::memory_order_relaxed)!=scene)skippedScenes.fetch_add(1,std::memory_order_relaxed);
            if(deepRan&&lastDeepScene.exchange(scene,std::memory_order_relaxed)!=scene)deepScenes.fetch_add(1,std::memory_order_relaxed);
        }else{
            s.cpuNsNative.fetch_add(ns,std::memory_order_relaxed);s.timedNative.fetch_add(1,std::memory_order_relaxed);
            if(tracking_&&pair_&&lastNativePairScene.exchange(scene,std::memory_order_relaxed)!=scene)nativePairScenes.fetch_add(1,std::memory_order_relaxed);
        }
    }
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};

// ---- PX TAIL: bind-only for the RenderScene tail drawn AFTER the stereo pair was captured ----
// sagRenderer::RenderScene_RenderThread @0x2a530f0 calls DrawColorPass (bl @0x2a53930); the pair is captured at its return
// 0x2a53934 (retailColorReturnOffset). Everything below is drawn into the native LEFT target only and never reaches the headset.
// 11 JUMP_SLOT wrappers with exact returns: GOT -> fn, fn fingerprint, call word
// at ret-4. Foreign callers (CVfxWeather from ProcessRDR2 @0x2a44084, glows from DrawCheapEnvironmentMap @0x2a5308c/0x2a53098)
// fail the return check and stay native. Inside an active tail scope the d11 draws keep BindInputLayout (pipeline, descriptors,
// FlushFrameBuffer); only vkCmdDraw* is dropped. Locks, clears, copies, queries and all CPU work stay native (none of the 11
// functions runs a query; OpticsManager::RunQueries / aiShell::RunOcclusionQueries are separate calls after EndMainColorPass).
// BinocularsManager::Draw (@0x2a53a6c) is excluded (it clears). Gate: pairScene() && wantedMask (PT in force ? the
// debug.rdrvr.posttail mask : 0, published per RenderScene by TraceEntry) has the site's bit. NO-FLUSH never applies here: the
// AI query pass follows on the eye target.
namespace tail {
enum Site : unsigned {Weather,Volumetric,Gravestones,PlayerNames,HiresParticles,FxObjects,TouchPftx,LightCones,LightGlows,MovieBeam,Optics,Count};
inline constexpr const char* names[Count]{"weather","volumetric","gravestones","playerNames","hiresParticles","fxObjects","touchPftx",
    "lightCones","lightGlows","movieBeam","optics"};
struct Route {std::uintptr_t original,got;unsigned rets;std::array<std::uintptr_t,2> ret;std::array<std::array<unsigned char,4>,2> call;
    unsigned codeBytes;std::array<unsigned char,24> code;};
inline constexpr std::array<Route,Count> routes{{
    {0x2980f64,0x2e5d840,1,{0x2a53984,0},{{{144,90,9,148},{}}},16,{
#include "gamecode/PostFxSkip_2980f64_16_8d.inc"
}},                  // CVfxWeather::Render
    {0x2a34a10,0x2e5f388,2,{0x2a53998,0x2a53a04},{{{47,104,9,148},{20,104,9,148}}},24,
        {
#include "gamecode/PostFxSkip_20da1fc_24_8d.inc"
}},                                      // renLightPrePassLightsMgr::RenderVolumetricLights(bool) x2
    {0x2a87ad4,0x2e5c6d0,1,{0x2a539bc,0},{{{202,81,9,148},{}}},16,{
#include "gamecode/PostFxSkip_2a87650_16_8d.inc"
}},                   // GravestoneGenerator::Draw(u8)
    {0x27f8ab4,0x2e59250,1,{0x2a539c0,0},{{{137,55,9,148},{}}},16,{
#include "gamecode/PostFxSkip_186a950_16_8d.inc"
}},          // PlayerNames::DrawNameListInWorld
    {0x2931ce4,0x2e5cab0,1,{0x2a539e4,0},{{{176,83,9,148},{}}},24,
        {
#include "gamecode/PostFxSkip_2931ce4_24_8d.inc"
}},                                  // sagFxManager::DrawHighresParticles
    {0x2931a90,0x2e5caa8,1,{0x2a539ec,0},{{{170,83,9,148},{}}},24,
        {
#include "gamecode/PostFxSkip_1868460_24_8d.inc"
}},                                      // sagFxManager::DrawFXObjects
    {0x2b2bc7c,0x2e5f8b8,1,{0x2a539f8,0},{{{175,106,9,148},{}}},16,{
#include "gamecode/PostFxSkip_2b2bc7c_16_8d.inc"
}},                 // TouchImagePftxManager::Render
    {0x2a8abf4,0x2e5f8c0,1,{0x2a53a18,0},{{{171,106,9,148},{}}},24,
        {
#include "gamecode/PostFxSkip_1913a80_24_8d.inc"
}},                                     // sagLightConeMgr::Draw
    {0x2a8bb04,0x2e5f860,1,{0x2a53a30,0},{{{117,106,9,148},{}}},24,
        {
#include "gamecode/PostFxSkip_186a950_24_8d.inc"
}},                                     // sagLightGlowMgr::Draw(GlowPass)
    {0x2ac5574,0x2e5f8c8,1,{0x2a53a40,0},{{{165,106,9,148},{}}},16,{
#include "gamecode/PostFxSkip_1913a80_16_8d.inc"
}},           // WorldMoviePlayer::DrawBeam
    {0x296fa08,0x2e5d4f8,1,{0x2a53a60,0},{{{181,88,9,148},{}}},24,
        {
#include "gamecode/PostFxSkip_184ff0c_24_8d.inc"
}}}};                                    // OpticsManager::Draw(grcViewport*)
// GPU markers of the post region (StereoReplay.h GpuMark TailEnd / QueriesEnd; observation only, never change behaviour).
inline constexpr std::uintptr_t endMainColorOffset=0x2a3b4dc,endMainColorGotOffset=0x2e5f4d8,endMainColorReturnOffset=0x2a53a88; // body = ret
inline constexpr std::array<unsigned char,4> endMainColorFingerprint{192,3,95,214},endMainColorCallFingerprint{155,104,9,148};
inline constexpr std::uintptr_t runQueriesOffset=0x2931954,runQueriesGotOffset=0x2e5ca88,runQueriesReturnOffset=0x2a53b24; // sagFxManager::RunQueries
inline constexpr std::array<unsigned char,24> runQueriesFingerprint{
#include "gamecode/PostFxSkip_2931954_24_8d.inc"
};
inline constexpr std::array<unsigned char,4> runQueriesCallFingerprint{76,83,9,148};

inline std::atomic<unsigned> hookedMask{};            // sites whose JUMP_SLOT wrapper is live
inline std::atomic<unsigned> wantedMask{};            // Per RenderScene: PT in force ? posttail mask : 0
inline std::atomic<bool> markersHooked{};
inline std::array<std::atomic<const char*>,Count> fingerprint{};
inline std::atomic<const char*> markerFingerprint{"unconfigured"};
struct SiteStats {std::atomic<std::uint64_t> calls{},foreign{},skipped{},droppedDraws{},cpuNsSkipped{},cpuNsNative{},timedSkipped{},timedNative{};};
inline std::array<SiteStats,Count> sites{};
inline std::atomic<std::uint64_t> skippedScenes{},lastSkippedScene{},tailEndMarks{},queriesEndMarks{};
inline bool ready() noexcept {return hookedMask.load(std::memory_order_relaxed)!=0;}
inline bool exactCaller(unsigned site,std::uintptr_t engineBase,std::uintptr_t caller) noexcept {
    if(site>=Count)return false;const auto& r=routes[site];
    for(unsigned i=0;i<r.rets&&i<2;++i)if(caller==engineBase+r.ret[i])return true;
    return false;
}
// RAII around one exact tail call (bind-only when wanted). Not exact: counted foreign, native.
class Scope {
    unsigned site_;bool active_{},timed_{};std::uint64_t before_{},cpu0_{};
public:
    Scope(unsigned site,bool exact) noexcept:site_(site){
        if(site>=Count)return;auto& s=sites[site];s.calls.fetch_add(1,std::memory_order_relaxed);
        if(!exact){s.foreign.fetch_add(1,std::memory_order_relaxed);return;}
        timed_=true;
        if(pairScene()&&(wantedMask.load(std::memory_order_relaxed)&(1u<<site))){
            active_=true;before_=threadDropped;++threadScopes;++threadTail;liveScopes.fetch_add(1,std::memory_order_relaxed);}
        cpu0_=threadCpuNs();
    }
    ~Scope(){
        if(!timed_)return;
        const auto cpu=threadCpuNs();const auto ns=cpu>cpu0_?cpu-cpu0_:0;auto& s=sites[site_];
        if(active_){
            liveScopes.fetch_sub(1,std::memory_order_relaxed);--threadScopes;--threadTail;
            s.skipped.fetch_add(1,std::memory_order_relaxed);s.droppedDraws.fetch_add(threadDropped-before_,std::memory_order_relaxed);
            s.cpuNsSkipped.fetch_add(ns,std::memory_order_relaxed);s.timedSkipped.fetch_add(1,std::memory_order_relaxed);
            const auto scene=sceneSerial.load(std::memory_order_relaxed);
            if(lastSkippedScene.exchange(scene,std::memory_order_relaxed)!=scene)skippedScenes.fetch_add(1,std::memory_order_relaxed);
        }else{s.cpuNsNative.fetch_add(ns,std::memory_order_relaxed);s.timedNative.fetch_add(1,std::memory_order_relaxed);}
    }
    Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
};
// ---- TAIL IN EYES (STEREO TEST 21 or debug.rdrvr.tail=1) ----
// Stock order: everything above is drawn AFTER the pair capture, so GPU rain
// (CVfxWeather::Render -> CPtFxGPU::Render), lamp volumes, hi-res particles, FX objects, light cones and lamp coronas never
// reach the headset. Early mode calls the six visual sites itself, in the native order and
// with the native arguments, right after the native DrawColorPass returns INSIDE the recorded left colour stage (GameXr
// primaryDraw, selected Color scope): the draws land in the left eye target before its capture and in the colour tape the right
// eye replays (the replay is generic: UBO camera rewrite + clip correction push). Then the native exact calls of the same
// RenderScene are skipped entirely (tailSite in TraceEntry.cpp). RenderScene_RenderThread between bl DrawColorPass @0x2a53930 and
// bl EndMainColorPass @0x2a53a84 only does PIX markers, strb renderer+0x6aa (no reader in librdr), these calls, Gravestones/
// PlayerNames/TouchPftx/DrawBeam (left native), grcState::SetState(11,0)+OpticsManager::Draw (post-only, left native),
// BinocularsManager::Draw and UnbindVelocityBuffer; matrix unlock / scene semaphore come after EndMainColorPass.
// Native arguments (RenderScene disassembly 0x2a53978..0x2a53a2c; GLOB_DAT relocs checked):
//   CVfxWeather::Render(this = *GOT 0x2e014f0 = &g_vfxWeather 0x4e829580)
//   renLightPrePassLightsMgr::RenderVolumetricLights(*(*GOT 0x2e0a860 = &sm_Instance 0x4e85e728), true) ... later (.., false)
//   if(!*(*GOT 0x2e0e158 = &PARAM_noptfx 0x4e827440 +0x18)) sagFxManager::DrawHighresParticles(*(&sm_Instance 0x4e6f8f10, GOT 0x2deac58))
//   sagFxManager::DrawFXObjects(same)
//   if(cone = *(&sagLightConeMgr::sm_Instance 0x4e8a4b40, GOT 0x2e10a38)) sagLightConeMgr::Draw(cone)
//   if(glow = *(&sagLightGlowMgr::sm_Instance 0x4e8a4b78, GOT 0x2e10a28)) sagLightGlowMgr::Draw(glow, 0)
// Known risks: GPU rain simulated twice if CPtFxGPU updates in place inside Render (rain twice as fast in one
// eye); TAA (no motion vectors) may smear rain streaks; GPU cost ~+0.6..1.2 ms (the tail fill is drawn in both eyes).
namespace early {
inline constexpr unsigned movedMask=(1u<<Weather)|(1u<<Volumetric)|(1u<<HiresParticles)|(1u<<FxObjects)|(1u<<LightCones)|(1u<<LightGlows)|(1u<<TouchPftx);
// TOUCH PTFX: rage::TouchImagePftxManager::Render (rage::TOUCHPTFX 0x4e8e9e80, GOT 0x2e02b28) draws world-placed sprites
// (TouchImagePftx(CSprite2d*, pos, orient, Color32)) - the objective's yellow X on the ground - in the RenderScene tail after the eye
// copy, through camManager::GetViewport(channel 0) (the stock camera). Moved here like the particles; TraceEntry's GetViewport wrapper
// (ret 0x2b2bcb0, touchActive) hands it the left EYE viewport, the right eye gets it from the replay.
inline constexpr std::uintptr_t touchGot=0x2e02b28,touchVar=0x4e8e9e80;
inline thread_local bool touchActive{};
inline constexpr std::uintptr_t weatherGot=0x2e014f0,weatherObject=0x4e829580;
inline constexpr std::uintptr_t lightsMgrGot=0x2e0a860,lightsMgrVar=0x4e85e728;
inline constexpr std::uintptr_t noPtfxGot=0x2e0e158,noPtfxVar=0x4e827440,noPtfxField=0x18;
inline constexpr std::uintptr_t fxMgrGot=0x2deac58,fxMgrVar=0x4e6f8f10;
inline constexpr std::uintptr_t coneGot=0x2e10a38,coneVar=0x4e8a4b40;
inline constexpr std::uintptr_t glowGot=0x2e10a28,glowVar=0x4e8a4b78;
struct Config {std::uintptr_t weather{},lightsMgr{},noPtfx{},fxMgr{},cone{},glow{},touch{};std::array<std::uintptr_t,Count> fn{};bool ok{};};
inline Config config;                                   // written once at install, before any RenderScene uses it
inline std::atomic<const char*> fingerprint{"unconfigured"};
inline std::atomic<int> property{-1};                   // debug.rdrvr.tail: 1 on, 0 off (beats the STEREO TEST row), -1 absent
inline std::atomic<std::uint64_t> ranScene{};           // postfx::sceneSerial of the RenderScene whose tail was drawn early
inline std::atomic<std::uint64_t> runs{},declined{},nativeSkipped{},propertyReads{};
inline std::array<std::atomic<std::uint64_t>,Count> siteCalls{};
inline std::atomic<const char*> lastDecline{""};
// PTFX CULL: the eye viewport of the recorded left colour stage while run() draws the high-res particles (GameXr sets it around run());
// TraceEntry's ptxEffectRuleStd::CullEffect wrapper culls against it instead of the stock grcViewport::sm_Current (the narrow 1374x720 game
// camera): effects in the VR field of view but outside the stock frustum (the objective's yellow X seen from close by) would be dropped.
inline thread_local const void* cullViewport{};
// TraceEntry, after the tail hooks: GLOB_DAT slots must hold the expected globals; fn = the native function of every moved site.
template<class Read> bool configure(std::uintptr_t base,const std::array<std::uintptr_t,Count>& originals,unsigned hooked,Read read) {
    const auto got=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t v{};return read(nullptr,base+slot,&v,sizeof v)&&v==base+target;};
    const char* r="ok";
    if((hooked&movedMask)!=movedMask)r="tailNotHooked";
    else if(!got(weatherGot,weatherObject)||!got(lightsMgrGot,lightsMgrVar)||!got(noPtfxGot,noPtfxVar)||!got(fxMgrGot,fxMgrVar)||
            !got(coneGot,coneVar)||!got(glowGot,glowVar)||!got(touchGot,touchVar))r="globals";
    Config c{};
    if(r[0]=='o'){c.weather=base+weatherObject;c.lightsMgr=base+lightsMgrVar;c.noPtfx=base+noPtfxVar;c.fxMgr=base+fxMgrVar;c.cone=base+coneVar;c.glow=base+glowVar;c.touch=base+touchVar;
        c.fn=originals;c.ok=true;
        for(unsigned i=0;i<Count;++i)if(((movedMask>>i)&1u)&&!c.fn[i])c.ok=false;
        if(!c.ok)r="noOriginal";}
    config=c;fingerprint.store(r,std::memory_order_relaxed);return c.ok;
}
// Every RenderScene (TraceEntry observeScene): debug.rdrvr.tail every 64 scenes.
inline void sceneBegin(std::uint64_t scene) noexcept {
#if defined(__ANDROID__)
    if(scene%64==1){const int saved=errno;char v[PROP_VALUE_MAX]{};const int n=__system_property_get("debug.rdrvr.tail",v);
        property.store(n==1&&(v[0]=='0'||v[0]=='1')?v[0]-'0':-1,std::memory_order_relaxed);propertyReads.fetch_add(1,std::memory_order_relaxed);errno=saved;}
#else
    (void)scene;
#endif
}
inline bool requested() noexcept {
    const int p=property.load(std::memory_order_relaxed);if(p>=0)return p==1;
    (void)::rdrvr::engine::stereo::testModeTailInEyes; // ON by default; debug.rdrvr.tail 0 = the tail stays after the pair capture
    return true;}
inline bool active() noexcept {return config.ok&&requested();}
template<class P> inline P peekAt(std::uintptr_t a) noexcept {P v{};std::memcpy(&v,reinterpret_cast<const void*>(a),sizeof v);return v;}
// Render thread, selected LEFT Color scope, right after the native DrawColorPass returned (tape still recording).
inline void run() noexcept {
    if(!active())return;
    const auto scene=sceneSerial.load(std::memory_order_relaxed);
    if(!scene||ranScene.load(std::memory_order_relaxed)==scene){declined.fetch_add(1,std::memory_order_relaxed);lastDecline.store("scene",std::memory_order_relaxed);return;}
    const auto& c=config;
    const auto mgr=peekAt<std::uintptr_t>(c.lightsMgr),fx=peekAt<std::uintptr_t>(c.fxMgr);
    if(!mgr||!fx){declined.fetch_add(1,std::memory_order_relaxed);lastDecline.store(!mgr?"lightsMgr":"fxMgr",std::memory_order_relaxed);return;}
    const int saved=errno;
    ranScene.store(scene,std::memory_order_relaxed); // from here the native exact calls of this scene are skipped
    using Fn0=void(*)(void*);using FnB=void(*)(void*,bool);using FnI=void(*)(void*,int);
    const auto call0=[&](unsigned site,std::uintptr_t self){siteCalls[site].fetch_add(1,std::memory_order_relaxed);reinterpret_cast<Fn0>(c.fn[site])(reinterpret_cast<void*>(self));};
    {::rdrvr::vr::rain::DropScope drops;call0(Weather,c.weather);} // RAIN QUALITY: scaled rain quad count for this call only
    siteCalls[Volumetric].fetch_add(1,std::memory_order_relaxed);reinterpret_cast<FnB>(c.fn[Volumetric])(reinterpret_cast<void*>(mgr),true);
    if(!peekAt<std::uintptr_t>(c.noPtfx+noPtfxField))call0(HiresParticles,fx);
    call0(FxObjects,fx);
    if(c.touch&&cullViewport){touchActive=true;call0(TouchPftx,c.touch);touchActive=false;} // The objective X (eye viewport via TraceEntry)
    siteCalls[Volumetric].fetch_add(1,std::memory_order_relaxed);reinterpret_cast<FnB>(c.fn[Volumetric])(reinterpret_cast<void*>(mgr),false);
    if(const auto cone=peekAt<std::uintptr_t>(c.cone))call0(LightCones,cone);
    if(const auto glow=peekAt<std::uintptr_t>(c.glow)){siteCalls[LightGlows].fetch_add(1,std::memory_order_relaxed);reinterpret_cast<FnI>(c.fn[LightGlows])(reinterpret_cast<void*>(glow),0);}
    runs.fetch_add(1,std::memory_order_relaxed);errno=saved;
}
// tailSite wrapper: true = this exact native call was already drawn early in this RenderScene (skip it, no CPU work).
inline bool skipNative(unsigned site,bool exact) noexcept {
    if(!exact||site>=Count||!((movedMask>>site)&1u))return false;
    const auto scene=sceneSerial.load(std::memory_order_relaxed);
    if(!scene||ranScene.load(std::memory_order_relaxed)!=scene)return false;
    nativeSkipped.fetch_add(1,std::memory_order_relaxed);return true;
}
inline std::string report() {
    std::ostringstream o;
    o<<"\"tailInEyes\":{\"fingerprint\":\""<<fingerprint.load()<<"\",\"requested\":"<<(requested()?1:0)<<",\"property\":"<<property.load()
     <<",\"runs\":"<<runs.load()<<",\"declined\":"<<declined.load()<<",\"lastDecline\":\""<<lastDecline.load()<<"\",\"nativeSkipped\":"<<nativeSkipped.load()<<",\"siteCalls\":{";
    bool first=true;for(unsigned i=0;i<Count;++i)if((movedMask>>i)&1u){o<<(first?"":",")<<'"'<<names[i]<<"\":"<<siteCalls[i].load();first=false;}
    o<<"}}";return o.str();
}
} // namespace early
} // namespace tail
// GameXr gameWorldPresentBypass: a world present that is NOT bypassed hands the 2D backbuffer to the theater. After a
// bind-only scene that image is stale (expected 0); after a deep scene it holds the fresh raw blit (counted apart).
// theaterShown = the presentation is not WORLD, so the XR thread shows the theater and the
// stale image really reaches the headset (staleTheaterCaptures). In WORLD the capture is shown only
// on an XR frame without a stereo layer (theater fallback): staleWorldCaptures.
inline void notePresent(bool bypassed,bool theaterShown=true) noexcept {
    presentChecks.fetch_add(1,std::memory_order_relaxed);
    if(bypassed)return;
    const auto scene=sceneSerial.load(std::memory_order_relaxed);
    if(!scene||lastSkippedScene.load(std::memory_order_relaxed)!=scene)return;
    (lastDeepScene.load(std::memory_order_relaxed)==scene?deepTheaterCaptures:theaterShown?staleTheaterCaptures:staleWorldCaptures).fetch_add(1,std::memory_order_relaxed);
}
inline double meanMs(std::uint64_t ns,std::uint64_t count) noexcept {return count?double(ns)/double(count)/1e6:0.0;}
inline std::string report(unsigned mode) {
    std::ostringstream o;const unsigned e=effective(mode);
    o<<"\"skipPostFx\":{\"ready\":"<<ready.load()<<",\"mode\":"<<stereo::testModeSkipPostFx<<",\"testMode\":"<<mode
     <<",\"setting\":"<<request.load()<<",\"property\":"<<property.load()<<",\"requested\":"<<requested()<<",\"effective\":"<<e
     <<",\"name\":\""<<requestName(e)<<"\",\"menuState\":"<<menuState()<<",\"scenes\":"<<sceneSerial.load()<<",\"pairs\":"<<pairs.load()
     <<",\"skippedScenes\":"<<skippedScenes.load()<<",\"nativePairScenes\":"<<nativePairScenes.load()<<",\"deepScenes\":"<<deepScenes.load()
     <<",\"staleTheaterCaptures\":"<<staleTheaterCaptures.load()<<",\"staleWorldCaptures\":"<<staleWorldCaptures.load()<<",\"deepTheaterCaptures\":"<<deepTheaterCaptures.load()
     <<",\"presentChecks\":"<<presentChecks.load()<<",\"propertyReads\":"<<propertyReads.load()<<",\"sites\":{";
    for(unsigned i=0;i<SiteCount;++i){
        const auto& s=sites[i];
        o<<(i?",":"")<<'"'<<siteNames[i]<<"\":{\"calls\":"<<s.calls.load()<<",\"skipped\":"<<s.active.load()<<",\"droppedDraws\":"<<s.droppedDraws.load()
         <<",\"cpuMsSkipped\":"<<meanMs(s.cpuNsSkipped.load(),s.timedSkipped.load())<<",\"cpuMsNative\":"<<meanMs(s.cpuNsNative.load(),s.timedNative.load())
         <<",\"cpuMsDeep\":"<<meanMs(s.cpuNsDeep.load(),s.timedDeep.load())<<",\"timedSkipped\":"<<s.timedSkipped.load()<<",\"timedNative\":"<<s.timedNative.load()<<'}';
    }
    o<<"},\"tail\":{\"hooked\":\"0x"<<std::hex<<tail::hookedMask.load()<<"\",\"mask\":\"0x"<<tail::wantedMask.load()<<std::dec<<"\",\"markers\":"<<(tail::markersHooked.load()?1:0)
     <<",\"markerFingerprint\":\""<<tail::markerFingerprint.load()<<"\",\"tailEndMarks\":"<<tail::tailEndMarks.load()<<",\"queriesEndMarks\":"<<tail::queriesEndMarks.load()
     <<",\"skippedScenes\":"<<tail::skippedScenes.load()<<",\"sites\":{";
    for(unsigned i=0;i<tail::Count;++i){
        const auto& s=tail::sites[i];const char* fp=tail::fingerprint[i].load();
        o<<(i?",":"")<<'"'<<tail::names[i]<<"\":{\"fingerprint\":\""<<(fp?fp:"unconfigured")<<"\",\"calls\":"<<s.calls.load()<<",\"foreign\":"<<s.foreign.load()
         <<",\"skipped\":"<<s.skipped.load()<<",\"droppedDraws\":"<<s.droppedDraws.load()
         <<",\"cpuMsSkipped\":"<<meanMs(s.cpuNsSkipped.load(),s.timedSkipped.load())<<",\"cpuMsNative\":"<<meanMs(s.cpuNsNative.load(),s.timedNative.load())<<'}';
    }
    o<<"}},\"noFlush\":{\"enabled\":"<<(noFlushWanted.load()?1:0)<<",\"scopes\":"<<noFlushScopes.load()<<",\"bindsSkipped\":"<<bindsSkipped.load()<<'}';
    o<<",\"deep\":"<<deep::report()<<'}';
    return o.str();
}
inline std::string report() {return report(lastMode.load(std::memory_order_relaxed));}
}
