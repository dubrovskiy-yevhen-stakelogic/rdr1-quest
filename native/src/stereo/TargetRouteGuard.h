#pragma once
// Draw guard for failed routed stages. A mod-routed native stage (TargetCallBoundary::invoke) that fails
// after it closed the eye pass leaves the engine's VkContext with desired = actual = pass = NULL:
// FlushFrameBuffer then begins nothing and the engine's next draw would be recorded outside any render
// pass (Adreno vkCmdDraw/vkCmdDrawIndexed read [passState+0x4e8 = NULL]+0xf8, vkCmdClearAttachments
// +0x100). A failed routed stage is always cancelled (Scene::eye -> false -> EngineStereoPass::draw
// Cancelled/Quarantined), so none of its remaining native draws can matter: the draw hooks drop them
// (CameraIntegration.h cameraDraw/cameraDrawIndexed, IndexedDispatch.fragment.inc DrawIndirect/
// DrawIndexedIndirect/ClearAttachments). This is TargetCallBoundary::discarding()/discardedDraws of the
// report. Header-only (inline variables, one definition per program) so the host test can exercise it.
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace rdrvr::engine::targetroute {
enum class Discard : unsigned {Draw,DrawIndexed,DrawIndirect,DrawIndexedIndirect,ClearAttachments,Count};
// Routed stages that have failed and are still running (any thread). 0 on every healthy frame: the draw hooks then pay
// one relaxed load of a global and never touch TLS.
inline std::atomic<unsigned> armed{};
// [render thread] Active::failed of the routed stage running on this thread (set/cleared by TargetCallBoundary::invoke).
inline thread_local const bool* activeFailed{};
inline thread_local bool activeArmed{}; // this thread's failed route holds one `armed` reference
inline std::atomic<std::uint64_t> discarded[unsigned(Discard::Count)]{};
// TargetCallBoundary::discarding(): the routed stage on this thread has failed; its remaining native draws must not be
// recorded (the eye pass may be closed).
inline bool discarding() noexcept {
    if(!armed.load(std::memory_order_relaxed))return false;
    const bool* failed=activeFailed;return failed&&*failed;
}
// The draw hooks' first statement: true = drop this command (counted).
inline bool discard(Discard kind) noexcept {
    if(!discarding())return false;
    discarded[unsigned(kind)].fetch_add(1,std::memory_order_relaxed);return true;
}
inline std::uint64_t discardedDraws() noexcept {std::uint64_t n=0;for(const auto& d:discarded)n+=d.load(std::memory_order_relaxed);return n;}

// Rare events (route failures, depth-resolve rejects): key -> count. Render thread writes, report thread reads.
struct Tally {
    struct Row {std::string key;std::uint64_t n{};};
    std::mutex m;std::array<Row,24> rows{};unsigned used{};std::uint64_t overflow{};
    void add(const std::string& key) noexcept {
        try{std::lock_guard g(m);
            for(unsigned i=0;i<used;++i)if(rows[i].key==key){++rows[i].n;return;}
            if(used<rows.size()){rows[used].key=key;rows[used].n=1;++used;}else ++overflow;}catch(...){}
    }
    std::string json() {
        std::ostringstream o;o<<'{';std::lock_guard g(m);
        for(unsigned i=0;i<used;++i)o<<(i?",":"")<<'"'<<rows[i].key<<"\":"<<rows[i].n;
        if(overflow)o<<(used?",":"")<<"\"OTHER\":"<<overflow;
        o<<'}';return o.str();
    }
    void resetForTests() {std::lock_guard g(m);for(auto& r:rows)r=Row{};used=0;overflow=0;}
};
// Never destroyed (no static destructor may run while the render thread still reports a failure at process exit).
inline Tally& failures() noexcept {static Tally* t=new Tally;return *t;}
inline Tally& depthRejects() noexcept {static Tally* t=new Tally;return *t;}
inline Tally& depthRejectLayouts() noexcept {static Tally* t=new Tally;return *t;}
inline std::atomic<std::uint64_t> failureTotal{},logLines{},logSuppressed{},lastLogNs{};
inline std::atomic<const char*> lastFailure{"NONE"},lastDepthReject{"NONE"};
inline std::atomic<unsigned> lastFailureEye{},lastFailureRole{};
inline std::atomic<int> lastRejectSrc{-1},lastRejectDst{-1};
// Routed stage invokes that passed the preflight, per eye x role (EngineTargetRole: 0 Color, 2 ShadowCollector =
// the DEPTH stage, 3 LightAccumulation). An eye-1 colour or depth invoke is a native right-eye stage: a replay fallback,
// including the silent ones (replay disabled after 8 faults, views not ready) that stereoFallbackStages does not count.
// The right-eye LIGHTING stage is never replayed (EngineStereoPass.cpp), so the report's
// invokes.eye1 = [colour ~0, depth ~0, light ~frames] on a healthy run; only the first two indicate fallbacks.
inline std::atomic<std::uint64_t> invokes[2][4]{};
inline std::atomic<std::uint64_t> preflightRejects{},reopened{},reopenFailed{};

inline void enterRoute(const bool* failed,unsigned eye,unsigned role) noexcept {
    activeFailed=failed;if(role<4)invokes[eye&1u][role].fetch_add(1,std::memory_order_relaxed);
}
inline void leaveRoute() noexcept {
    activeFailed=nullptr;
    if(activeArmed){activeArmed=false;armed.fetch_sub(1,std::memory_order_relaxed);}
}
// First failure of a routed stage (reason = the static string TargetCallBoundary::fail stores as status_): counted per
// reason, arms the draw guard while the stage runs on, and logs at most one RdrVr WARN line per second.
inline void noteFailure(const char* reason,unsigned eye,unsigned role) noexcept {
    const int savedErrno=errno;
    if(activeFailed&&!activeArmed){activeArmed=true;armed.fetch_add(1,std::memory_order_relaxed);}
    failures().add(reason?reason:"NULL");++failureTotal;
    lastFailure.store(reason?reason:"NULL",std::memory_order_relaxed);lastFailureEye.store(eye,std::memory_order_relaxed);lastFailureRole.store(role,std::memory_order_relaxed);
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    auto last=lastLogNs.load(std::memory_order_relaxed);
    if(last&&now-last<1000000000ull){++logSuppressed;errno=savedErrno;return;}
    lastLogNs.store(now?now:1,std::memory_order_relaxed);++logLines;
#ifdef __ANDROID__
    const char* reject=lastDepthReject.load(std::memory_order_relaxed);
    __android_log_print(ANDROID_LOG_WARN,"RdrVr","target route failed: %s eye=%u role=%u (stage cancelled, its native draws dropped; last depth reject %s src=%d dst=%d; %llu failures, %llu lines suppressed)",
        reason?reason:"NULL",eye,role,reject,lastRejectSrc.load(std::memory_order_relaxed),lastRejectDst.load(std::memory_order_relaxed),
        (unsigned long long)failureTotal.load(),(unsigned long long)logSuppressed.load());
#endif
    errno=savedErrno;
}
// EngineDepthResolve::copy (overlay) returned false: which check rejected, and the tracked layouts it saw (-1 = not read).
inline void noteDepthReject(const char* reason,int srcLayout,int dstLayout) noexcept {
    depthRejects().add(reason);lastDepthReject.store(reason,std::memory_order_relaxed);
    lastRejectSrc.store(srcLayout,std::memory_order_relaxed);lastRejectDst.store(dstLayout,std::memory_order_relaxed);
    if(srcLayout>=0||dstLayout>=0){try{depthRejectLayouts().add("s"+std::to_string(srcLayout)+"_d"+std::to_string(dstLayout));}catch(...){}}
}
// gameBootstrap.targetBoundary (GameXr report, next to worldDrawReport). New object; existing fields are untouched.
inline std::string report() {
    std::ostringstream o;const auto r=[](const std::atomic<std::uint64_t>& a){return a.load(std::memory_order_relaxed);};
    o<<"{\"failures\":"<<failures().json()<<",\"failureTotal\":"<<r(failureTotal)<<",\"lastFailure\":\""<<lastFailure.load()<<"\",\"lastFailureEye\":"<<lastFailureEye.load()
     <<",\"lastFailureRole\":"<<lastFailureRole.load()<<",\"logLines\":"<<r(logLines)<<",\"logSuppressed\":"<<r(logSuppressed)
     <<",\"discardedDraws\":"<<discardedDraws()<<",\"discarded\":{\"draw\":"<<r(discarded[0])<<",\"drawIndexed\":"<<r(discarded[1])<<",\"drawIndirect\":"<<r(discarded[2])
     <<",\"drawIndexedIndirect\":"<<r(discarded[3])<<",\"clearAttachments\":"<<r(discarded[4])<<"},\"armed\":"<<armed.load()
     // per eye: [color, shadowCollector (DEPTH stage), lightAccumulation]
     <<",\"invokes\":{\"eye0\":["<<r(invokes[0][0])<<","<<r(invokes[0][2])<<","<<r(invokes[0][3])<<"],\"eye1\":["<<r(invokes[1][0])<<","<<r(invokes[1][2])<<","<<r(invokes[1][3])<<"]}"
     <<",\"preflightRejects\":"<<r(preflightRejects)<<",\"reopenedAfterCopyFailure\":"<<r(reopened)<<",\"reopenFailed\":"<<r(reopenFailed)
     <<",\"depthResolve\":{\"rejects\":"<<depthRejects().json()<<",\"layouts\":"<<depthRejectLayouts().json()<<",\"lastReject\":\""<<lastDepthReject.load()
     <<"\",\"lastRejectLayouts\":["<<lastRejectSrc.load()<<","<<lastRejectDst.load()<<"]}}";
    return o.str();
}
}
