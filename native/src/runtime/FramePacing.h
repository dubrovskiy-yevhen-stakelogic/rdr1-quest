#pragma once
// Render-thread frame-rate lock (GRAPHICS -> FRAME RATE AUTO / LOCK 36 / LOCK 24).
//
// The render thread calls pace() right after the stereo pass of frame N was submitted
// (GameStereoCapture::submitted, outside every lock): pass N and its mailbox copy are
// already queued, so the GPU keeps working while the render thread sleeps before it
// starts frame N+1. The wake-up is snapped to the XR thread's frame grid (xrWaitFrame
// returns, phase-locked loop over the display period), so frame starts have a constant
// phase relative to the compositor and every pair is displayed for the same number of
// display periods (LOCK 36 at 72 Hz = 2).
//
// Grid spacing g: the display period p, or the XR frame interval when the runtime paces
// xrWaitFrame at a multiple of p (AppSW half rate: g = 2p, so the wake-up parity relative to
// the XR frames is fixed and a phase flip of g/2 really changes it). Phase = phaseIndex*g/2.
//
// Phase controller (XR thread, per 32-pair window): a pair is irregular when its XR-frame gap
// differs from the lock interval / XR interval; irregular pairs at or right after a render-thread
// overrun are not counted (overruns are attributed per pair, never cumulatively). >= 4 effective
// irregular pairs in a window flip the phase, unless the other phase measured no better within the
// last 16 windows. The window after a flip evaluates it: better -> hold 1 window; not better ->
// unproductive, hold doubles (max 16 windows), and if the new phase is worse it flips back.
//
// An overrun under a half-rate grid (s > p) keeps lastWake on the grid (overrunWake), so it costs only
// the missed slot; the grid PLL ignores isolated outlier wake-ups (re-anchor after outlierRun in a row).
//
//  - FRAME RATE ADAPTIVE (mode 3): PacingGovernor picks 36 or 24 from the frame cost c = max(GPU lite span + mailbox
//    lanes, render-thread busy time). Down (36 -> 24) on 3 overruns in 24 frames, p90(36) > 26.5 ms or 4 cadence
//    breaks in 32 pairs; up (24 -> 36) when p95(72) <= 25.0 ms with >= 60 GPU samples after the hold (5 s, doubling up
//    to 40 s when a drop follows a step up within 10 s). Only in ADAPTIVE: the predictedDisplayTime grid (pdt - median
//    lead), the PDT-gap phase controller, the SPACEWARP gate (AppSW only at 36 with headroom) and the overlay tag.
//    AUTO / LOCK 36 / LOCK 24 pace on the PLL grid with the XR-frame phase controller; grid lead, cadence and
//    per-mode counters are reported in every mode.
//  - Globals below (presentTelemetry, xrHistory, lane EMAs) rather than StereoMailbox.h / XrHarness.h members:
//    one stereo mailbox and one XR harness per process.
//  - Stale resubmit (StereoMailbox::render): only in ADAPTIVE or with debug.rdrvr.staleresubmit=1.
//
// AppSW pacer: under the runtime's half-rate loop (s > p and the lock interval == s, i.e. AppSW at 36 of 72 Hz)
//    the render thread does not snap onto the s grid of raw xrWaitFrame returns (every parity flip / anchor move costs a
//    whole extra p slot = 41.7 ms frames). It free-runs: wake = lastWake + T (T = predictedDisplayPeriod when within
//    +-10% of the interval, else the interval), nudged by clamp(e/16, +-0.5 ms) toward a separate slow XR grid (gain 1/16,
//    wrap-aware, never re-anchored by outliers), e = wrap(anchor - wake) into (-T/2, T/2]. Overruns keep the on-cadence
//    overrunWake rule. The binary s/2 autophase flips are frozen on this path (appswfree=1) or slewed (appswfree=2).
//    s == p (72 Hz) keeps the grid snap. Telemetry in every mode: slotSkipSleeps, anchorJumps, busyNs, appswPacer.
//
// SAT cadence (appswfree=3, the default): on that free path, while the GPU is saturated (cost
//    C + margin > T), wake at lastWake + (C + margin) instead of overrunning back-to-back, so pair N finishes before N+1 is
//    submitted (no 2-owner throttle wait) and stays latchable ~2Ts - C instead of being evicted by capture N+2 (3-slot mailbox).
//    C = mean lite span (32) + mailbox lanes + residual. Preemption-robust residual: LEARNED from the measured production
//    interval while back-to-back (a frame after >= 3 overruns: interval - mean of the newest 8 lite spans - lanes; mean of the
//    newest 32 samples, clamped [0, 2.5 ms]; 0.9 ms until 16 samples; samples skipped while the lite window moves > 1 ms (scene
//    change) and rejected > 6 ms (CPU-bound / stall)). That interval holds every compositor preemption (GPU 100% busy), so C
//    reproduces the back-to-back cycle and is then carried by the lite delta; preemptions that move into SAT's ~margin gaps
//    lower lite and the true cycle together (host model: SAT lite mean == back-to-back lite mean, no limit cycle).
//    Enter after 24 fresh samples with C + margin0 > T + 0.3 ms; exit after 72 with C + margin0 < T - 0.7 ms and a hold
//    (2 s, doubling to 16 s when re-entered within 10 s of an exit). Cadence = clamp(C + margin, T + 0.25 ms, 2T), slewed
//    <= 0.25 ms per frame; margin +0.25 ms per SAT overrun (<= 3 ms; the entry frame + 1 not counted), -0.1 ms per 64 clean
//    frames. 28 of the last 32 SAT frames overran (render-thread bound): exit, re-entry blocked for the hold. C > 2T: SAT
//    does not pace (free path, back-to-back on its cadence). No lite data (stale > 32 calls): no SAT. In SAT an overrun sets
//    lastWake = now (never catch up back-to-back); no slow-lock nudge, no slot-skip count. Unsaturated scenes and no GPU data:
//    the free path wake for wake. Report: framePacing.appswPacer.sat{...} and satPath{...}.
// SAT guards: (a) SAT never paces on an estimate that fits T: C + (margin - base) <= T -> the free wake,
//    SAT stays armed (no T + 0.25 ms floor except sat=1: a cadence above T on an unsaturated GPU drifts against the XR grid and
//    repeats a pair every ~T/(cadence-T) frames). Armed -> paced again only on proof, a free-wake overrun (C overestimates an
//    idle-gapped GPU by up to ~0.5 ms when compositor slices land inside the lite span). (b) Entry also needs proof: >= 24 of the
//    last 32 free-path frames overran (and waited on the capture throttle when captureThrottleFed). (c) The exit gate uses an EMA
//    (1/64 per fresh sample) of C, not the 32-span mean that beats with the 72 Hz compositor. Render-thread bound: the residual
//    learns only from frames that waited on the throttle (captureThrottleWaitNs; until GameStereoCapture feeds it, every
//    back-to-back frame counts), and SAT exits "cpu" when the p25 render-thread time of its last 32 paced frames > T - 0.5 ms
//    (the free path then latches nearly every pair; SAT only adds cadence), blocked for the hold; unfed (busy includes the
//    throttle wait) a +1 ms margin probe comes first and must fail to lower p25 by 1 ms. The calibration ring is dropped on cpu /
//    overrun exits and on a scene change (lite window moving > 1 ms for 4 fresh samples; in SAT only an above-default residual),
//    so an inflated residual from a CPU-bound episode cannot set the cadence of a later scene.
//
// Deadline-aware phase: the free path holds the cadence, not a deadline-safe PHASE.
//    (1) Deadline samples: StereoMailbox::render (XR thread, while AppSW
//    synthesizes) gives every pair ONE margin = its due latch (render() entry of the first XR frame at which it is the oldest
//    captured, undisplayed, unsampled pair) - its ready time (capture-lane GPU end timestamp on CLOCK_MONOTONIC via the CPU
//    generic timer, GpuDuration::endMonotonicNs; else the observation time = inexact). A pair not finished at its due latch is a
//    miss (margin < 0, published when its completion is observed). SPSC atomics ring pairDeadlines -> FramePacer::pace().
//    (2) PhaseController (debug.rdrvr.pacing.phasectl, default 1): on the free path (appswfree 1/3, not SAT-paced) an offset on
//    the slow-lock anchor, applied at once (wake and anchor move together, <= 0.5 ms per paced frame, never a slot). Every 24
//    exact samples: p10 < 3 ms -> earlier by (3.5 - p10)/2, p10 > 4 ms -> later by (p10 - 3.5)/2 (0.25..4 ms), then a settle;
//    a miss pushes earlier at once by |margin| (0.25..2 ms, 1 ms inexact; one push per 6 samples) and blocks relaxing for 96
//    samples; > T of earlier pushes without a relax or a healthy window = spin (no further pushes for 256 samples). Samples are
//    ignored while the render lock is off its anchor by > 2.5 ms (runtime parity flips: the slow lock, not the phase, has to move).
//    An inexact miss with no exact sample in the last 96 samples (debug.rdrvr.gpulite=0, no blitStartLag yet, another
//    counter rate) is counted (missesNoData) but never pushes: without exact margins nothing could relax the phase again.
//    (3) LateLatchPolicy (debug.rdrvr.latchus 0..6000, default 3000): StereoMailbox::render may wait for the oldest newer pair's
//    copy fence when no newer pair is finished, only while AppSW synthesizes at half rate (never at 72 Hz), bounded by the XR
//    frame (elapsed since xrWaitFrame + wait <= interval/2), predictedDisplayTime (>= 40 ms left after the wait) and the
//    expected completion (capture submit + p25 ready delay within the budget + 0.5 ms). No exact ready delay (none
//    yet, or the window dropped after 96 inexact completions in a row) = no wait (noData). The wait is ONE fence wait holding the
//    mailbox lock: the render thread's capture()/copyComplete() can block for up to the budget (counted: producerBlocked).
#include "diagnostics/FrameTimings.h"
#include "HeatWarning.h" // heat01: fed by flightTick, reported in report()
#include "GpuBoost.h" // boost01: reported in report()
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <condition_variable>
#include <cstdio> // KGSL temp / clock
#if defined(__ANDROID__)
#include <dlfcn.h> // AThermal via dlsym
#endif
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#if defined(__ANDROID__) || defined(__linux__)
#include <time.h>
#endif

namespace rdrvr::runtime {
// ---------------------------------------------------------------- shared present state
// GpuDuration (always-on mailbox lanes): EMA 1/8 of the capture (producer copy/FXAA) and display (XR blit) GPU spans.
inline std::atomic<std::uint64_t> laneCaptureNs{0},laneDisplayNs{0};
inline std::atomic<bool> gpuLanesAlwaysOn{true}; // FramePacer::configure: false when debug.rdrvr.gpulite=0
enum class LayerKind : std::uint8_t {None,StereoNew,StereoRepeat,StereoStale,Theater,Diagnostic};
// Why an XR frame carried no layer (XrHarness), and what StereoMailbox::render decided last (lastReason; Stale = the
// last displayed pair was resubmitted after the age limit, i.e. a layer WAS returned).
enum class NoLayer : std::uint8_t {None,Age,Epoch,NoScreen,NotWorld,NoTracking,ShouldNotRender,Error,Stale};
inline const char* layerKindName(LayerKind k) noexcept {
    switch(k){case LayerKind::StereoNew:return "STEREO_NEW";case LayerKind::StereoRepeat:return "STEREO_REPEAT";case LayerKind::StereoStale:return "STEREO_STALE";
    case LayerKind::Theater:return "THEATER";case LayerKind::Diagnostic:return "DIAGNOSTIC";default:return "NONE";}
}
inline const char* noLayerName(NoLayer r) noexcept {
    switch(r){case NoLayer::Age:return "AGE";case NoLayer::Epoch:return "EPOCH";case NoLayer::NoScreen:return "NO_SCREEN";case NoLayer::NotWorld:return "NOT_WORLD";
    case NoLayer::NoTracking:return "NO_TRACKING";case NoLayer::ShouldNotRender:return "SHOULD_NOT_RENDER";case NoLayer::Error:return "ERROR";case NoLayer::Stale:return "STALE";default:return "NONE";}
}
struct PresentTelemetry {
    // StereoMailbox::render (XR thread, mailbox lock).
    std::atomic<std::uint64_t> staleResubmits{},ageRejects{},epochRejects{};
    std::atomic<std::uint8_t> lastReason{};                    // NoLayer of the last render() call
    std::atomic<bool> lastNewPair{},lastBlit{},staleEnabled{}; // staleEnabled: FramePacer::configure (ADAPTIVE / property)
    // XrHarness: frames without any layer, by reason (world = the presentation gate said WORLD).
    std::atomic<std::uint64_t> noLayerTotal{},noLayerAge{},noLayerEpoch{},noLayerNoScreen{},noLayerNoTracking{},noLayerError{},noLayerNonWorld{},noLayerShouldNotRender{};
    // XrHarness: SHARPEN (XrCompositionLayerSettingsFB on the projection layer). settingsLastChained: the last stereo frame.
    std::atomic<std::uint64_t> settingsChained{},settingsFlags{},settingsMissingExtension{};std::atomic<bool> settingsLastChained{};
    void noteNoLayer(bool world,NoLayer r) noexcept {
        noLayerTotal.fetch_add(1,std::memory_order_relaxed);
        if(r==NoLayer::ShouldNotRender){noLayerShouldNotRender.fetch_add(1,std::memory_order_relaxed);return;}
        if(!world){noLayerNonWorld.fetch_add(1,std::memory_order_relaxed);return;}
        auto& c=r==NoLayer::Age?noLayerAge:r==NoLayer::Epoch?noLayerEpoch:r==NoLayer::NoScreen?noLayerNoScreen:r==NoLayer::NoTracking?noLayerNoTracking:noLayerError;
        c.fetch_add(1,std::memory_order_relaxed);
    }
    void resetForTests() noexcept {
        for(auto* a:{&staleResubmits,&ageRejects,&epochRejects,&noLayerTotal,&noLayerAge,&noLayerEpoch,&noLayerNoScreen,&noLayerNoTracking,&noLayerError,
                     &noLayerNonWorld,&noLayerShouldNotRender,&settingsChained,&settingsFlags,&settingsMissingExtension})a->store(0,std::memory_order_relaxed);
        lastReason.store(0,std::memory_order_relaxed);lastNewPair.store(false,std::memory_order_relaxed);lastBlit.store(false,std::memory_order_relaxed);
        staleEnabled.store(false,std::memory_order_relaxed);settingsLastChained.store(false,std::memory_order_relaxed);
    }
    std::string noLayerJson() const {
        std::ostringstream o;
        o<<"{\"total\":"<<noLayerTotal.load()<<",\"world\":{\"age\":"<<noLayerAge.load()<<",\"epoch\":"<<noLayerEpoch.load()<<",\"noScreen\":"<<noLayerNoScreen.load()
         <<",\"noTracking\":"<<noLayerNoTracking.load()<<",\"error\":"<<noLayerError.load()<<"},\"nonWorld\":"<<noLayerNonWorld.load()
         <<",\"shouldNotRender\":"<<noLayerShouldNotRender.load()<<",\"staleResubmits\":"<<staleResubmits.load()<<",\"staleEnabled\":"<<(staleEnabled.load()?"true":"false")<<'}';
        return o.str();
    }
    std::string layerSettingsJson() const {
        std::ostringstream o;
        o<<"{\"chained\":"<<(settingsLastChained.load()?1:0)<<",\"chainedFrames\":"<<settingsChained.load()<<",\"flags\":"<<settingsFlags.load()
         <<",\"missingExtension\":"<<settingsMissingExtension.load()<<'}';
        return o.str();
    }
};
inline PresentTelemetry presentTelemetry;

// One XR frame (XrHarness::frame, XR thread). Times: wake = xrWaitFrame return (CLOCK_MONOTONIC ns); pdt =
// predictedDisplayTime in CLOCK_MONOTONIC ns when flag PdtMonotonic, else the raw XrTime.
struct XrFrameRecord {
    std::uint64_t frame{},wakeNs{};std::int64_t pdtNs{};
    std::uint32_t waitUs{},workUs{},endUs{},periodUs{};
    std::uint64_t pair{};std::uint8_t kind{},reason{},flags{};
};
class XrHistory {
public:
    static constexpr unsigned capacity=512;
    enum Flags : std::uint8_t {NewPair=1,BlitSubmitted=2,PdtMonotonic=4,SharpenChained=8};
    void push(const XrFrameRecord& r) noexcept {std::lock_guard lock(mutex_);ring_[next_]=r;next_=(next_+1)%capacity;if(n_<capacity)++n_;++total_;}
    std::uint64_t total() const noexcept {std::lock_guard lock(mutex_);return total_;}
    void resetForTests() noexcept {std::lock_guard lock(mutex_);n_=next_=0;total_=0;}
    // Columnar: wakeUs relative to t0 (oldest wake); pdtUs relative to t0 when flag 4, else to the oldest raw pdt.
    std::string json() const {
        std::lock_guard lock(mutex_);std::ostringstream o;
        const unsigned first=(next_+capacity-n_)%capacity;
        const std::uint64_t t0=n_?ring_[first].wakeNs:0;std::int64_t raw0=0;
        for(unsigned i=0;i<n_;++i){const auto& r=ring_[(first+i)%capacity];if(!(r.flags&PdtMonotonic)&&r.pdtNs){raw0=r.pdtNs;break;}}
        o<<"{\"capacity\":"<<capacity<<",\"total\":"<<total_<<",\"t0Ns\":"<<t0
         <<",\"cols\":[\"frame\",\"wakeUs\",\"pdtUs\",\"waitUs\",\"workUs\",\"endUs\",\"periodUs\",\"pair\",\"kind\",\"reason\",\"flags\"],\"rows\":[";
        for(unsigned i=0;i<n_;++i){
            const auto& r=ring_[(first+i)%capacity];
            const std::int64_t pdt=!r.pdtNs?0:(r.flags&PdtMonotonic)?(r.pdtNs-std::int64_t(t0))/1000:(r.pdtNs-raw0)/1000;
            o<<(i?",":"")<<'['<<r.frame<<','<<std::int64_t(r.wakeNs-t0)/1000<<','<<pdt<<','<<r.waitUs<<','<<r.workUs<<','<<r.endUs<<','<<r.periodUs
             <<','<<r.pair<<','<<unsigned(r.kind)<<','<<unsigned(r.reason)<<','<<unsigned(r.flags)<<']';
        }
        o<<"]}";return o.str();
    }
private:
    mutable std::mutex mutex_;std::array<XrFrameRecord,capacity> ring_{};unsigned n_{},next_{};std::uint64_t total_{};
};
inline XrHistory xrHistory;

// GameXr (stereo pass): framePacer.pace({stereo::gpuLiteSerial(), gpuLiteFrameUs()*1000}). serial changes once per new GPU span.
struct GpuCostSample {std::uint64_t serial{},frameNs{};};
// captureThrottleWaitNs = render-thread wait in the 2-owner capture throttle, cumulative wall ns,
// and captureThrottleFed = that writer runs (set on every throttle check, waiting or not). FramePacer::pace() takes the delta
// since its previous call = this frame's throttle wait.
// While unfed, SAT treats every back-to-back frame as GPU-bound and relies on the "cpu" exit.
inline std::atomic<std::uint64_t> captureThrottleWaitNs{0};inline std::atomic<bool> captureThrottleFed{false};

// FRAME RATE ADAPTIVE level controller (pure; times in ns; host-testable). Level 24 or 36.
struct PacingGovernor {
    struct Config {
        std::uint64_t dropNs{26'500'000},upNs{25'000'000},appSwOffNs{26'000'000};
        std::uint64_t holdNs{5'000'000'000ull},maxHoldNs{40'000'000'000ull},flapNs{10'000'000'000ull},calmNs{30'000'000'000ull},appSwDelayNs{3'000'000'000ull};
        unsigned overruns{3},overrunWindow{24},cadenceBreaks{4},dropSamples{36},upSamples{72},upGpuSamples{60},settle{4};
    };
    enum Reason : unsigned {ReasonNone=0,ReasonOverruns=1,ReasonCostP90=2,ReasonCadence=3};
    static const char* reasonName(unsigned r) noexcept {return r==ReasonOverruns?"overruns":r==ReasonCostP90?"costP90":r==ReasonCadence?"cadence":"none";}
    static constexpr unsigned ring=72;
    Config cfg{};
    bool active{};unsigned level{24};
    std::array<std::uint32_t,ring> cost{},cpu{};std::array<std::uint8_t,ring> gpu{}; // microseconds; gpu: sample had GPU data
    unsigned n{},next{};
    std::uint32_t overrunMask{};unsigned settleLeft{};
    std::uint64_t tDown{},tUp{},tChange{},hold{};
    std::uint64_t ups{},downs{},samplesTotal{},gpuMissing{};std::array<std::uint64_t,4> downBy{};unsigned lastReason{};
    bool appSw{};std::uint64_t appSwSince{},appSwBlockUntil{},appSwHold{},appSwLastTrip{},appSwTrips{},appSwRevokes{};
    std::uint64_t lastCostNs{};bool lastGpuValid{};

    void clear() noexcept {n=next=0;overrunMask=0;}
    void enter(std::uint64_t now) noexcept {
        active=true;level=24;tDown=tChange=now;tUp=0;hold=cfg.holdNs;settleLeft=0;
        appSw=false;appSwBlockUntil=0;appSwHold=cfg.holdNs;appSwLastTrip=0;clear();
    }
    void leave() noexcept {active=false;appSw=false;clear();}
    unsigned levelHz() const noexcept {return active?level:0u;}
    // Nearest-rank percentile (pct of 100) of the newest min(window, n) samples, microseconds (0 = no sample).
    std::uint32_t percentileUs(unsigned window,unsigned pct,bool cpuRing=false) const noexcept {
        const unsigned m=std::min(std::min(window,n),ring);if(!m)return 0;
        std::array<std::uint32_t,ring> s{};const auto& src=cpuRing?cpu:cost;
        for(unsigned i=0;i<m;++i)s[i]=src[(next+ring-1-i)%ring];
        const unsigned k=std::max(1u,std::min(m,(m*pct+99)/100));
        std::nth_element(s.begin(),s.begin()+(k-1),s.begin()+m);return s[k-1];
    }
    unsigned gpuSamples(unsigned window) const noexcept {
        const unsigned m=std::min(std::min(window,n),ring);unsigned c=0;
        for(unsigned i=0;i<m;++i)c+=gpu[(next+ring-1-i)%ring];
        return c;
    }
    // One paced frame (render thread). Returns true when the level changed.
    bool step(std::uint64_t now,std::uint64_t costNs,bool gpuValid,bool overrun,unsigned cadenceBreaks,std::uint64_t cpuNs=0) noexcept {
        if(!active)return false;
        cost[next]=std::uint32_t(std::min<std::uint64_t>(costNs/1000,UINT32_MAX));cpu[next]=std::uint32_t(std::min<std::uint64_t>(cpuNs/1000,UINT32_MAX));
        gpu[next]=std::uint8_t(gpuValid?1:0);next=(next+1)%ring;if(n<ring)++n;
        ++samplesTotal;if(!gpuValid)++gpuMissing;lastCostNs=costNs;lastGpuValid=gpuValid;
        if(level==36){
            if(settleLeft)--settleLeft;
            else{const std::uint32_t mask=cfg.overrunWindow>=32?0xffffffffu:((1u<<cfg.overrunWindow)-1u);overrunMask=((overrunMask<<1)|(overrun?1u:0u))&mask;}
            unsigned why=ReasonNone;
            if(unsigned(std::popcount(overrunMask))>=cfg.overruns)why=ReasonOverruns;
            else if(n>=cfg.dropSamples&&std::uint64_t(percentileUs(cfg.dropSamples,90))*1000>cfg.dropNs)why=ReasonCostP90;
            else if(cadenceBreaks>=cfg.cadenceBreaks)why=ReasonCadence;
            if(why){
                hold=now-tUp<cfg.flapNs?std::min(hold*2,cfg.maxHoldNs):cfg.holdNs; // a drop right after a step up: back-off grows
                level=24;tDown=tChange=now;++downs;++downBy[why];lastReason=why;clear();
                if(appSw){appSw=false;++appSwRevokes;}
                return true;
            }
            if(now-tUp>=cfg.calmNs)hold=cfg.holdNs; // 30 s at 36 without a drop: the back-off starts over
            if(appSw){if(n>=cfg.dropSamples&&std::uint64_t(percentileUs(cfg.dropSamples,90))*1000>cfg.appSwOffNs){appSw=false;++appSwRevokes;blockAppSw(now);}} // Back-off (no 2 s on/off flapping)
            else if(now>=appSwBlockUntil&&now-tUp>=cfg.appSwDelayNs&&n>=cfg.upSamples&&std::uint64_t(percentileUs(cfg.upSamples,95))*1000<=cfg.upNs){appSw=true;appSwSince=now;}
            return false;
        }
        if(n>=cfg.upSamples&&gpuSamples(cfg.upSamples)>=cfg.upGpuSamples&&std::uint64_t(percentileUs(cfg.upSamples,95))*1000<=cfg.upNs&&now-tDown>=hold){
            level=36;tUp=tChange=now;++ups;clear();settleLeft=cfg.settle;lastReason=ReasonNone;return true;
        }
        return false;
    }
    // Block re-allowing AppSW for a back-off: holdNs, doubling (to maxHoldNs) when the previous revoke is less than calmNs old.
    // Shared by the guard trip and the cost revoke (p90 > appSwOffNs), so neither can toggle AppSW every ~2 s.
    void blockAppSw(std::uint64_t now) noexcept {
        appSwHold=appSwLastTrip&&now-appSwLastTrip<cfg.calmNs?std::min(appSwHold*2,cfg.maxHoldNs):cfg.holdNs;
        appSwLastTrip=now;appSwBlockUntil=now+appSwHold;
    }
    // The AppSW budget guard tripped (XR thread -> render thread): revoke, block re-allowing for a back-off.
    void noteAppSwOverBudget(std::uint64_t now) noexcept {
        ++appSwTrips;blockAppSw(now);
        if(appSw){appSw=false;++appSwRevokes;}
    }
};

// AppSW saturation cadence controller (SAT, appswfree=3; pure, times in ns, host-testable; FramePacer holds it under
// mutex_). step() returns the SAT cadence (> 0) for this paced frame, or 0 = take the free path.
struct AppSwSaturation {
    struct Config {
        std::int64_t residualNs{900'000},residualMinNs{0},residualMaxNs{2'500'000},defaultLanesNs{1'400'000};
        std::int64_t marginNs{1'000'000},marginMaxNs{3'000'000},marginUpNs{250'000},marginDownNs{100'000};
        std::int64_t enterNs{300'000},exitNs{700'000},minStepNs{250'000},slewNs{250'000};
        std::int64_t learnLoNs{-2'000'000},learnHiNs{6'000'000},learnSettleNs{1'000'000}; // interval - lite - lanes: clamp low / reject high; lite settled
        std::uint64_t holdNs{2'000'000'000ull},maxHoldNs{16'000'000'000ull},flapNs{10'000'000'000ull};
        unsigned minSamples{16},enterSamples{24},exitSamples{72},decayFrames{64},learnSamples{16},learnLite{8},staleCalls{32},capPeriods{2},overrunExit{28};
        std::int64_t cpuSlackNs{500'000},cpuProbeNs{1'000'000};unsigned enterBound{24},costTau{64},cpuSamples{32},dropRun{4},cpuOverruns{8}; // CPU exit at T - slack;
        // unfed probe step; saturation proof (of 32 free-path frames); exit EMA; cpu window; scene change = lite moving 4 fresh samples in a row;
        // cpu check only with <= 8 of the last 32 SAT frames overrunning
        bool cpuExit{true};                                                                     // debug.rdrvr.pacing.satcpu
    };
    enum Reason : std::uint8_t {ReasonNone=0,ReasonGpu=1,ReasonForced=2};
    enum Exit : std::uint8_t {ExitNone=0,ExitCool=1,ExitForced=2,ExitStale=3,ExitOff=4,ExitOverruns=5,ExitCpu=6};
    static const char* reasonName(unsigned r) noexcept {return r==ReasonGpu?"gpu":r==ReasonForced?"forced":"none";}
    static const char* exitName(unsigned e) noexcept {return e==ExitCool?"cool":e==ExitForced?"forced":e==ExitStale?"stale":e==ExitOff?"off":e==ExitOverruns?"overruns":e==ExitCpu?"cpu":"none";}
    static constexpr unsigned window=32;
    Config cfg{};
    std::array<std::uint32_t,window> liteUs{};unsigned n{},next{};                   // newest fresh lite spans (microseconds)
    bool sat{};std::int64_t cadence{},margin{},marginBase{},lastCost{},lastTarget{},lastLanes{},lastT{};
    unsigned above{},below{},clean{},settle{};std::uint32_t overrunMask{};          // overrunMask: the last 32 SAT frames
    std::uint64_t tEnter{},tExit{},hold{},blockUntil{};                            // blockUntil: no re-entry after an overrun exit
    std::array<std::int32_t,window> resUs{};std::array<std::uint32_t,window> prodUs{};unsigned rn{},rnext{};std::uint64_t learned{}; // production-interval calibration ring
    std::uint64_t enters{},exits{},paced{},overruns{},marginBumps{},capped{},noGpu{},learnSkips{},learnRejects{};std::uint8_t reason{},lastExit{};
    // boundMask = the last 32 free-path frames (bit: overran [and waited on the capture throttle when fed]); costEma =
    // exit gate; moving = the lite window moves (scene change); armed = SAT state, this frame took the free wake (C + bumps <= T);
    // cpuUs = render-thread time of the last 32 SAT-paced frames (busy, minus the throttle wait when fed).
    std::uint32_t boundMask{};std::int64_t costEma{};bool armed{},pacing{};unsigned movingRun{};            // pacing: SAT state paces (else armed)
    std::array<std::uint32_t,window> cpuUs{};unsigned cn{},cnext{};std::int64_t probeP25{};                // probeP25: p25 when the last probe fired
    std::uint64_t armedFree{},cpuExits{},cpuProbes{},overrunExits{},drops{},learnCpuSkips{};

    void addLite(std::uint64_t ns) noexcept {
        if(ns<=1'000'000ull||ns>=200'000'000ull)return;
        liteUs[next]=std::uint32_t(ns/1000);next=(next+1)%window;if(n<window)++n;
    }
    std::int64_t liteMeanNs(unsigned newest=window) const noexcept {
        const unsigned k=std::min(newest,n);if(!k)return 0;std::int64_t s=0;for(unsigned i=0;i<k;++i)s+=liteUs[(next+window-1-i)%window];return s*1000/std::int64_t(k);
    }
    bool residualIsLearned() const noexcept {return rn>=cfg.learnSamples;} // The current calibration ring (learned = cumulative)
    std::int64_t residualLearnedNs() const noexcept {std::int64_t v=0;for(unsigned i=0;i<rn;++i)v+=resUs[i];return rn?v*1000/std::int64_t(rn):0;}
    std::int64_t productionNs() const noexcept {std::int64_t v=0;for(unsigned i=0;i<rn;++i)v+=prodUs[i];return rn?v*1000/std::int64_t(rn):0;}
    std::int64_t residualNs() const noexcept {return residualIsLearned()?std::clamp(residualLearnedNs(),cfg.residualMinNs,cfg.residualMaxNs):cfg.residualNs;}
    std::int64_t lanesOr(std::int64_t lanes) const noexcept {return lanes>0?lanes:cfg.defaultLanesNs;}
    std::int64_t costNs(std::int64_t lanes) const noexcept {return n?liteMeanNs()+lanesOr(lanes)+residualNs():0;}
    // p25 of the render-thread time of the last SAT-paced frames: a CPU-bound thread is slow on every frame, while CPU
    // spikes (and the throttle waits right after them) or an under-paced ramp lift only part of the window.
    std::int64_t cpuP25Ns() const noexcept {
        if(!cn)return 0;std::array<std::uint32_t,window> s=cpuUs;std::nth_element(s.begin(),s.begin()+cn/4,s.begin()+cn);return std::int64_t(s[cn/4])*1000;
    }
    void dropCalibration() noexcept {if(rn)++drops;rn=rnext=0;} // Residual back to the default until 16 new samples
    // A back-to-back frame (this and the previous >= 3 paced frames overran): its pace-to-pace interval is the GPU production
    // cycle (2-owner throttle bound), compositor preemption included wherever it lands. Residual = mean of the newest 32
    // (interval - mean of the newest 8 lite spans - lanes) samples (a plain mean: an EMA would freeze +-0.3 ms of per-pair noise at
    // SAT entry). Skipped while the lite window moves (|mean8 - mean32| > 1 ms: a scene change, where the 32-window still
    // holds the previous scene and every sample would be off by the step).
    // gpuBound = the frame waited on the capture throttle (or the throttle is not fed); a back-to-back frame without a
    // wait is render-thread time (CPU-bound), never a GPU cycle.
    void learn(std::int64_t intervalNs,std::int64_t lanes,bool gpuBound=true) noexcept {
        if(n<cfg.minSamples||intervalNs<=0||intervalNs>=200'000'000)return;
        if(!gpuBound){++learnCpuSkips;return;}
        const std::int64_t recent=liteMeanNs(cfg.learnLite);
        if(std::llabs(recent-liteMeanNs())>cfg.learnSettleNs){++learnSkips;return;}
        const std::int64_t raw=intervalNs-recent-lanesOr(lanes);
        if(raw>cfg.learnHiNs){++learnRejects;return;}                              // > 6 ms: CPU-bound (no throttle wait) or a stall, not a GPU cycle
        const std::int64_t r=std::max(raw,cfg.learnLoNs);
        resUs[rnext]=std::int32_t(r/1000);prodUs[rnext]=std::uint32_t(intervalNs/1000);rnext=(rnext+1)%window;if(rn<window)++rn;++learned;
    }
    void enter(std::uint64_t now,std::uint8_t why) noexcept {
        // Re-entered within flapNs of the exit (or of the end of an overrun exit's block): the back-off grows, else it starts over.
        const std::uint64_t calm=tExit?std::max(tExit,blockUntil)+cfg.flapNs:0;
        hold=!hold?cfg.holdNs:(tExit&&now<calm)?std::min(hold*2,cfg.maxHoldNs):cfg.holdNs;
        sat=true;tEnter=now;++enters;reason=why;cadence=0;above=below=clean=0;settle=2;overrunMask=0;margin=marginBase=cfg.marginNs;cn=cnext=0;probeP25=0;armed=false;pacing=true;
    }
    void leave(std::uint64_t now,std::uint8_t why) noexcept {sat=false;tExit=now;++exits;lastExit=why;cadence=0;above=below=clean=0;cn=cnext=0;boundMask=0;armed=pacing=false;}
    // Off the free path / appswfree != 3: leave SAT (counted as exit "off" when it was active) and forget the lite window.
    void reset(std::uint64_t now) noexcept {if(sat)leave(now,ExitOff);n=next=0;above=below=0;lastCost=lastTarget=0;boundMask=0;costEma=0;movingRun=0;armed=false;}
    // One paced frame on the free path with appswfree=3 (render thread). T = free cadence, lanes = mailbox lane EMAs (0 =
    // unknown), fresh = a new lite sample this call, overrunPrev = the previous paced frame overran, force 0 auto / 1 SAT /
    // 2 never, gpuAge = paced calls since the last fresh lite sample. boundPrev = the previous frame overran and
    // (when the throttle is fed) waited on it (-1 = overrunPrev); cpuNs = this frame's render-thread time (-1 = unknown);
    // cpuPure = cpuNs excludes the capture-throttle wait (fed).
    std::int64_t step(std::uint64_t now,std::int64_t T,std::int64_t lanes,bool fresh,bool overrunPrev,int force,unsigned gpuAge,int boundPrev=-1,std::int64_t cpuNs=-1,bool cpuPure=false) noexcept {
        if(gpuAge>cfg.staleCalls&&n){n=next=0;above=below=0;costEma=0;movingRun=0;} // the lite span stopped: forget the window
        const bool haveGpu=n>=cfg.minSamples;
        const std::int64_t C=haveGpu?costNs(lanes):0,cap=std::int64_t(cfg.capPeriods)*T;
        lastCost=C;lastLanes=lanesOr(lanes);lastT=T;armed=false;
        if(fresh&&haveGpu){ // Exit EMA (beat-proof); a scene change (lite window moves > 1 ms) drops the calibration
            costEma=costEma?costEma+(C-costEma)/std::int64_t(std::max(1u,cfg.costTau)):C;
            // A real step keeps |mean8 - mean32| > 1 ms for ~20 samples, beat ripple for 1-2: 4 in a row. In SAT (no back-to-back
            // frames to relearn from) only an above-default residual is dropped: a drop can only remove an inflated value; a real one
            // comes back through the overrun bumps and the SAT back-to-back frames that learn it again.
            movingRun=std::llabs(liteMeanNs(cfg.learnLite)-liteMeanNs())>cfg.learnSettleNs?movingRun+1:0;
            if(movingRun==cfg.dropRun&&(!sat||residualNs()>cfg.residualNs))dropCalibration();
        }
        if(cfg.marginNs!=marginBase){marginBase=cfg.marginNs;margin=cfg.marginNs;} // property changed: new base, bumps dropped
        if(force==2){if(sat)leave(now,ExitForced);above=0;return 0;}
        if(!sat){
            boundMask=(boundMask<<1)|((boundPrev<0?overrunPrev:boundPrev>0)?1u:0u); // Proof of saturation
            if(force==1)enter(now,ReasonForced);
            else{
                if(!haveGpu){++noGpu;above=0;return 0;}
                if(fresh)above=C+cfg.marginNs>T+cfg.enterNs&&C<=cap?above+1:0;    // gates use the base margin: spikes cannot latch SAT on
                if(above<cfg.enterSamples||unsigned(std::popcount(boundMask))<cfg.enterBound||now<blockUntil)return 0;
                enter(now,ReasonGpu);
            }
        }else if(force!=1){
            if(!haveGpu){leave(now,ExitStale);++noGpu;return 0;}
            if(fresh)below=costEma+cfg.marginNs<T-cfg.exitNs?below+1:0;              // EMA, not the 32-span mean
            if(below>=cfg.exitSamples&&now-tEnter>=hold){leave(now,ExitCool);return 0;}
            // Render-thread bound (p25 of the last 32 SAT-paced frames > T - 0.5 ms): the free path latches nearly
            // every pair, SAT only adds cadence. Exit, block re-entry for the hold, drop the (probably CPU-inflated) calibration.
            // Judged only while <= 8 of the last 32 SAT frames overran (not in a bump storm). Unfed, busy also holds the throttle wait, which grows slowly
            // without overruns when SAT runs a little short of the real cycle (lite-blind GPU work beyond the residual clamp): probe
            // first: margin +1 ms and a new window. The probe passes when p25 drops by a full step (an under-paced GPU drains several
            // ms of throttle wait; CPU noise moves p25 well under 1 ms): the margin stays. A probe that did not move p25: exit when
            // still high (render-thread time does not depend on the cadence), else undo its margin. At the 3 ms cap: exit.
            if(cfg.cpuExit&&cn>=cfg.cpuSamples&&unsigned(std::popcount(overrunMask))<=cfg.cpuOverruns){
                const std::int64_t p=cpuP25Ns(),mx=std::max(cfg.marginMaxNs,marginBase);
                const bool high=p>T-cfg.cpuSlackNs,passed=probeP25&&p<probeP25-cfg.cpuProbeNs;
                if(probeP25&&!passed&&!high){margin=std::max(marginBase,margin-cfg.cpuProbeNs);probeP25=0;}
                else if(!high)probeP25=0;
                else if(!cpuPure&&margin<mx&&(!probeP25||passed)){margin=std::min(margin+cfg.cpuProbeNs,mx);clean=0;cn=cnext=0;probeP25=p;++cpuProbes;}
                else{leave(now,ExitCpu);++cpuExits;blockUntil=now+hold;dropCalibration();return 0;}
            }
        }
        if(haveGpu&&C>cap){++capped;cadence=0;overrunMask=0;settle=2;return 0;}  // beyond 2T: back-to-back on the free cadence
        // A SAT overrun bumps the margin. Settle: the entry frame and the next one (the first SAT wake still starts from the free
        // path's back-to-back lastWake and usually overruns once; likewise after capped frames) are not counted.
        const bool settling=settle>0;if(settle)--settle;
        if(overrunPrev&&!settling){++overruns;clean=0;const std::int64_t mx=std::max(cfg.marginMaxNs,marginBase);if(margin<mx){margin=std::min(margin+cfg.marginUpNs,mx);++marginBumps;}}
        else if(!overrunPrev&&++clean>=cfg.decayFrames){clean=0;margin=std::max(marginBase,margin-cfg.marginDownNs);}
        // Overrunning anyway (CPU-bound, or C short by more than the 3 ms margin cap): SAT paces nothing -> free path (it re-learns
        // the residual back-to-back and may re-enter through the gate after the hold, which doubles as for any quick re-entry).
        overrunMask=(overrunMask<<1)|(overrunPrev&&!settling?1u:0u);
        if(force!=1&&unsigned(std::popcount(overrunMask))>=cfg.overrunExit){leave(now,ExitOverruns);++overrunExits;blockUntil=now+hold;dropCalibration();return 0;} // re-entry after the hold
        // Never pace on an estimate that fits T. Cx = C + bumps <= T: the free wake, SAT stays armed. Armed ->
        // paced again only on proof: a free-wake overrun (C overestimates an unsaturated GPU by up to ~0.5 ms when compositor slices
        // land inside the lite span; an estimate short of the real cycle shows up as such overruns, each also a +0.25 ms bump).
        // Entry starts paced (24 free-path overruns). The T + minStep floor only under sat=1.
        if(force!=1){
            if(C+(margin-marginBase)<=T)pacing=false;else if(!pacing&&overrunPrev&&!settling)pacing=true;
            if(!pacing){cadence=0;lastTarget=0;armed=true;++armedFree;return 0;}
        }
        const std::int64_t target=force==1?std::clamp(C+margin,T+cfg.minStepNs,std::max(cap,T+cfg.minStepNs)):std::min(C+margin,cap);lastTarget=target;
        cadence=cadence?cadence+std::clamp(target-cadence,-cfg.slewNs,cfg.slewNs):target;
        if(!settling&&cpuNs>0&&cpuNs<200'000'000){cpuUs[cnext]=std::uint32_t(cpuNs/1000);cnext=(cnext+1)%window;if(cn<window)++cn;}
        ++paced;return cadence;
    }
};

// ---------------------------------------------------------------- deadline-aware phase
// One deadline sample per rendered pair (StereoMailbox::render, XR thread, while AppSW synthesizes). Times: CLOCK_MONOTONIC ns.
// dueNs = render() entry of the pair's due latch; readyNs = copy fence (GPU end timestamp when Exact, else observation time);
// marginNs = dueNs - readyNs. Miss = not finished at the due latch (NewerNotReady); Rescued = finished in that latch's wait.
enum PairDeadlineFlags : std::uint8_t {DeadlineExact=1,DeadlineMiss=2,DeadlineRescued=4,DeadlineSynth=8};
struct PairDeadline {std::uint64_t pair{},dueNs{},readyNs{};std::int64_t marginNs{};std::uint8_t flags{};};
// Single writer (whoever holds the stereo mailbox lock) -> single reader (FramePacer::pace, render thread). Relaxed atomic
// fields with a per-entry version (seqlock): the reader skips entries the writer lapped or was writing (counted as lost).
class PairDeadlineRing {
public:
    static constexpr unsigned capacity=64;
    void push(const PairDeadline& d) noexcept {
        const auto i=head_.load(std::memory_order_relaxed);auto& e=ring_[i%capacity];
        e.version.store(2*i+1,std::memory_order_relaxed);std::atomic_thread_fence(std::memory_order_release);
        e.pair.store(d.pair,std::memory_order_relaxed);e.dueNs.store(d.dueNs,std::memory_order_relaxed);e.readyNs.store(d.readyNs,std::memory_order_relaxed);
        e.marginNs.store(d.marginNs,std::memory_order_relaxed);e.flags.store(d.flags,std::memory_order_relaxed);
        e.version.store(2*i+2,std::memory_order_release);head_.store(i+1,std::memory_order_release);
    }
    // Copies up to max entries after cursor into out (cursor advances); lapped or torn entries are skipped and added to *lost.
    unsigned read(std::uint64_t& cursor,PairDeadline* out,unsigned max,std::uint64_t* lost=nullptr) const noexcept {
        const auto h=head_.load(std::memory_order_acquire);
        if(cursor>h)cursor=h;
        if(h-cursor>capacity){if(lost)*lost+=h-cursor-capacity;cursor=h-capacity;}
        unsigned n=0;
        for(;cursor<h&&n<max;++cursor){
            const auto& e=ring_[cursor%capacity];
            const auto v1=e.version.load(std::memory_order_acquire);
            const PairDeadline d{e.pair.load(std::memory_order_relaxed),e.dueNs.load(std::memory_order_relaxed),e.readyNs.load(std::memory_order_relaxed),
                                 e.marginNs.load(std::memory_order_relaxed),e.flags.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            if(v1!=2*cursor+2||e.version.load(std::memory_order_relaxed)!=v1){if(lost)++*lost;continue;}
            out[n++]=d;
        }
        return n;
    }
    std::uint64_t total() const noexcept {return head_.load(std::memory_order_acquire);}
private:
    struct Entry {std::atomic<std::uint64_t> version{},pair{},dueNs{},readyNs{};std::atomic<std::int64_t> marginNs{};std::atomic<std::uint8_t> flags{};};
    std::array<Entry,capacity> ring_{};std::atomic<std::uint64_t> head_{};
};
inline PairDeadlineRing pairDeadlines;

// Deadline bookkeeping of the stereo mailbox (pure; StereoMailbox calls it under its lock, the host test with a model).
// Due pair of a latch = the oldest captured pair with sequence > max(displayed, sampled). It gets exactly one sample: finished at
// the pick -> margin >= 0; finished during the late-latch wait -> Miss|Rescued; still in flight -> pending, published as a Miss
// when observed(). observed() also records the ready delay (ready - capture submit) for the late-latch estimate.
struct DeadlineTracker {
    static constexpr unsigned window=32,marginWindow=256,pendingSlots=4,staleInexact=96;
    struct Pending {std::uint64_t seq{},dueNs{};bool synth{};};
    std::uint64_t sampled{};std::array<Pending,pendingSlots> pending{};
    std::array<std::uint32_t,window> delayUs{};unsigned dn{},dnext{},inexactRun{};
    std::array<std::int32_t,marginWindow> marginUs{};unsigned mn{},mnext{};          // exact margins (report)
    std::uint64_t samples{},exact{},misses{},rescued{},resolved{},dropped{},nonSynth{},delayResets{};
    enum State : unsigned {DueReady=1,DueRescued=2,DueInFlight=3};
    // In-flight latch (StereoMailbox, debug.rdrvr.latchinflight): a pair is latchable once its capture is SUBMITTED, so its
    // margin = due latch - capture submit (- bias). A latch with nothing newer captured is due for the NEXT capture: capture()
    // publishes that pair's miss (margin < 0) through captured(). Only a capture within maxNothingNs after that latch is a
    // PHASE miss; a later one (render-thread overrun, hitch, early XR wake) is counted (nothingDropped) but never pushes the phase - the
    // pair is then simply shown at the next latch with a healthy margin.
    static constexpr std::uint64_t maxNothingNs=12'000'000;
    std::uint64_t nothingDueNs{};bool nothingSynth{};std::uint64_t nothingMisses{},nothingDropped{};
    void nothingNewer(std::uint64_t latchNs,bool synth) noexcept {if(!nothingDueNs){nothingDueNs=latchNs;nothingSynth=synth;}}
    void captured(std::uint64_t seq,std::uint64_t readyNs,PairDeadlineRing& ring) noexcept {
        if(!nothingDueNs)return;
        if(seq>sampled&&readyNs>=nothingDueNs&&readyNs-nothingDueNs<=maxNothingNs){
            sampled=seq;++nothingMisses;publish(seq,nothingDueNs,readyNs,true,DeadlineMiss,nothingSynth,ring);}
        else ++nothingDropped;
        nothingDueNs=0;
    }
    void reset() noexcept {sampled=0;pending={};nothingDueNs=0;}
    // A pair's copy completion was observed (readyNs = GPU end when exact, else the observation time).
    // staleInexact inexact completions in a row drop the ready-delay window (the late latch then has no estimate
    // and does not wait, LateLatchPolicy::NoData) instead of steering on delays from before the GPU clock went away.
    void observed(std::uint64_t seq,std::uint64_t publishedNs,std::uint64_t readyNs,bool isExact,PairDeadlineRing& ring) noexcept {
        if(isExact)inexactRun=0;else if(inexactRun<UINT32_MAX&&++inexactRun>=staleInexact&&dn){dn=dnext=0;++delayResets;}
        if(isExact&&publishedNs&&readyNs>publishedNs&&readyNs-publishedNs<200'000'000ull){
            delayUs[dnext]=std::uint32_t((readyNs-publishedNs)/1000);dnext=(dnext+1)%window;if(dn<window)++dn;}
        for(auto& p:pending)if(p.seq&&p.seq==seq){publish(seq,p.dueNs,readyNs,isExact,DeadlineMiss,p.synth,ring);p={};++resolved;}
    }
    // One latch (after the late-latch wait). dueSeq 0 = nothing captured waits. Returns true when a sample (or pending) was made.
    bool latch(std::uint64_t latchNs,std::uint64_t dueSeq,unsigned state,std::uint64_t readyNs,bool isExact,bool synth,PairDeadlineRing& ring) noexcept {
        if(!dueSeq||dueSeq<=sampled)return false;
        sampled=dueSeq;
        // Pending pairs older than the newest captured one that are no longer in the ring are dropped by the caller (forget()).
        if(state==DueInFlight){
            Pending* slot=nullptr;for(auto& p:pending)if(!p.seq){slot=&p;break;}
            if(!slot){slot=&pending[0];for(auto& p:pending)if(p.seq<slot->seq)slot=&p;++dropped;} // oldest unresolved: lost
            *slot={dueSeq,latchNs,synth};return true;
        }
        publish(dueSeq,latchNs,readyNs,isExact,state==DueRescued?std::uint8_t(DeadlineMiss|DeadlineRescued):std::uint8_t(0),synth,ring);return true;
    }
    // The ring was rebuilt / a pending pair's slot was reused before its completion was seen: drop it (counted).
    void forget(std::uint64_t seq) noexcept {for(auto& p:pending)if(p.seq==seq){p={};++dropped;}}
    void forgetAll() noexcept {for(auto& p:pending)if(p.seq){p={};++dropped;}nothingDueNs=0;}
    bool isPending(std::uint64_t seq) const noexcept {for(const auto& p:pending)if(p.seq&&p.seq==seq)return true;return false;}
    // Nearest-rank percentile of the exact ready delays (ns, 0 = none yet).
    std::int64_t readyDelayNs(unsigned pct) const noexcept {
        if(!dn)return 0;std::array<std::uint32_t,window> s=delayUs;const unsigned k=std::max(1u,std::min(dn,(dn*pct+99)/100));
        std::nth_element(s.begin(),s.begin()+(k-1),s.begin()+dn);return std::int64_t(s[k-1])*1000;
    }
    std::int64_t marginPercentileNs(unsigned pct) const noexcept {
        if(!mn)return 0;std::array<std::int32_t,marginWindow> s=marginUs;const unsigned k=std::max(1u,std::min(mn,(mn*pct+99)/100));
        std::nth_element(s.begin(),s.begin()+(k-1),s.begin()+mn);return std::int64_t(s[k-1])*1000;
    }
private:
    void publish(std::uint64_t seq,std::uint64_t dueNs,std::uint64_t readyNs,bool isExact,std::uint8_t flags,bool synth,PairDeadlineRing& ring) noexcept {
        const std::int64_t margin=std::int64_t(dueNs)-std::int64_t(readyNs);
        flags=std::uint8_t(flags|(isExact?DeadlineExact:0)|(synth?DeadlineSynth:0));
        ++samples;if(isExact)++exact;if(flags&DeadlineMiss)++misses;if(flags&DeadlineRescued)++rescued;if(!synth)++nonSynth;
        if(isExact){marginUs[mnext]=std::int32_t(std::clamp<std::int64_t>(margin/1000,-1'000'000,1'000'000));mnext=(mnext+1)%marginWindow;if(mn<marginWindow)++mn;}
        ring.push({seq,dueNs,readyNs,margin,flags});
    }
};

// Late-latch wait budget (pure). The XR thread may wait for the oldest newer pair's copy fence when no newer
// pair is finished: only while AppSW synthesizes at half rate (never at 72 Hz); the XR frame keeps <= interval/2 of work (incl.
// the wait) after xrWaitFrame; >= 40 ms stay before predictedDisplayTime after the wait; the pair is expected (capture submit +
// p25 ready delay) within the budget + 0.5 ms. No exact ready delay = no estimate = no wait (NoData; otherwise every
// NewerNotReady latch would wait the full budget for pairs ~20 ms away when the GPU clock is unavailable).
struct LateLatchPolicy {
    enum Why : std::uint8_t {Wait=0,Off=1,NotSynth=2,FullRate=3,XrLate=4,PdtNear=5,Far=6,NoData=7};
    static constexpr unsigned whyCount=8;
    static const char* whyName(unsigned w) noexcept {
        return w==Wait?"wait":w==Off?"off":w==NotSynth?"notSynth":w==FullRate?"fullRate":w==XrLate?"xrLate":w==PdtNear?"pdtNear":w==Far?"far":
               w==NoData?"noData":"none";}
    static constexpr std::int64_t maxNs=6'000'000,minWaitNs=200'000,minPdtLeadNs=40'000'000,slackNs=500'000;
    struct Inputs {
        std::int64_t latchNs{};bool synth{},halfRate{};std::uint64_t nowNs{},xrWakeNs{};std::int64_t xrIntervalNs{},pdtNs{};  // pdtNs 0 = unknown
        std::uint64_t pendingPublishedNs{};std::int64_t readyDelayNs{};                                                       // readyDelay 0 = unknown
    };
    static std::int64_t budget(const Inputs& in,Why* why=nullptr) noexcept {
        const auto out=[why](Why w){if(why)*why=w;return std::int64_t(0);};
        if(in.latchNs<=0)return out(Off);
        if(!in.synth)return out(NotSynth);
        if(!in.halfRate)return out(FullRate);
        std::int64_t b=std::min(in.latchNs,maxNs);
        if(in.xrWakeNs&&in.xrIntervalNs>0){
            const std::int64_t used=in.nowNs>=in.xrWakeNs?std::int64_t(in.nowNs-in.xrWakeNs):0;
            b=std::min(b,in.xrIntervalNs/2-used);
        }
        if(b<minWaitNs)return out(XrLate);
        if(in.pdtNs>0){const std::int64_t lead=in.pdtNs-std::int64_t(in.nowNs)-b;if(lead<minPdtLeadNs){b-=minPdtLeadNs-lead;if(b<minWaitNs)return out(PdtNear);}}
        if(in.readyDelayNs<=0||!in.pendingPublishedNs)return out(NoData);
        const std::int64_t remaining=std::int64_t(in.pendingPublishedNs)+in.readyDelayNs-std::int64_t(in.nowNs);
        if(remaining>b+slackNs)return out(Far);
        if(why)*why=Wait;
        return b;
    }
};

// Deadline-aware phase controller of the AppSW free path (pure; FramePacer holds it under mutex_). Output:
// an offset (ns) on the free path's slow-lock anchor; negative = the render thread wakes earlier = more margin, more latency.
struct PhaseController {
    struct Config {
        std::int64_t targetLoNs{3'000'000},targetHiNs{4'000'000},slewNs{500'000},stepMinNs{250'000},stepMaxNs{4'000'000};
        std::int64_t missMinNs{250'000},missMaxNs{2'000'000},missInexactNs{1'000'000};
        unsigned evalSamples{24},settleExtra{3},holdSamples{96},missGap{6},spinHoldSamples{256};
        unsigned exactStaleSamples{96}; // An inexact miss pushes only if an exact sample came within this many samples
    };
    static constexpr unsigned window=32;
    Config cfg{};
    std::array<std::int32_t,window> mUs{};unsigned n{},next{},sinceEval{},settle{},hold{},sinceMissPush{UINT32_MAX/2},spinHold{};
    unsigned sinceExact{UINT32_MAX/2}; // Samples since the last exact one (UINT32_MAX/2 = none since the reset)
    std::int64_t offset{},goal{},earlierRun{},lastT{27'777'778},lastDelta{},lastAdjust{},lastP10{},lastP50{};bool haveP{};
    std::uint64_t samples{},misses{},missesInexact{},missesNoData{},rescued{},evaluations{},earlier{},later{},missPushes{},spins{},wraps{},windowSamples{};
    void clearWindow() noexcept {n=next=sinceEval=0;}
    void reset() noexcept {clearWindow();settle=hold=spinHold=0;sinceMissPush=sinceExact=UINT32_MAX/2;offset=goal=earlierRun=lastDelta=0;haveP=false;}
    std::int64_t percentileNs(unsigned pct) const noexcept {
        if(!n)return 0;std::array<std::int32_t,window> s{};for(unsigned i=0;i<n;++i)s[i]=mUs[(next+window-n+i)%window];
        const unsigned k=std::max(1u,std::min(n,(n*pct+99)/100));std::nth_element(s.begin(),s.begin()+(k-1),s.begin()+n);return std::int64_t(s[k-1])*1000;
    }
    // Move the goal (earlier < 0). Earlier movement without a relax beyond one period = spin: the GPU cannot make the deadline at
    // any phase (or the margin signal is broken); stop pushing for a while instead of rotating the phase around the period.
    void move(std::int64_t d) noexcept {
        if(d<0){if(spinHold)return;earlierRun-=d;if(earlierRun>lastT){spinHold=cfg.spinHoldSamples;earlierRun=0;++spins;}}
        else earlierRun=0;
        goal+=d;lastAdjust=d;
        const std::int64_t dist=std::llabs(goal-offset);settle=unsigned((dist+cfg.slewNs-1)/std::max<std::int64_t>(1,cfg.slewNs))+cfg.settleExtra;clearWindow();
    }
    // One deadline sample (render thread, from the ring).
    void add(std::int64_t marginNs,std::uint8_t flags) noexcept {
        ++samples;if(spinHold)--spinHold;if(sinceMissPush<UINT32_MAX)++sinceMissPush;
        const bool isExact=(flags&DeadlineExact)!=0;
        if(isExact)sinceExact=0;else if(sinceExact<UINT32_MAX)++sinceExact;
        if(flags&DeadlineMiss){
            ++misses;if(flags&DeadlineRescued)++rescued;if(!isExact)++missesInexact;hold=cfg.holdSamples;
            // An inexact miss (real, size unknown) with no exact sample recently: only exact windows can relax the
            // phase again, so pushing here would only ratchet earlier (gpulite=0, no blitStartLag yet). Counted, not acted on.
            if(!isExact&&sinceExact>cfg.exactStaleSamples){++missesNoData;return;}
            if(sinceMissPush>=cfg.missGap){sinceMissPush=0;++missPushes;move(-(isExact?std::clamp(-marginNs,cfg.missMinNs,cfg.missMaxNs):cfg.missInexactNs));}
            return;                                                     // a miss sample is the pre-push phase: not in the window
        }
        if(hold)--hold;                                                 // Every finished pair runs the hold down
        if(!isExact)return;                                             // an inexact margin of a finished pair carries nothing
        if(settle){--settle;return;}                                    // pairs rendered before the last move
        mUs[next]=std::int32_t(std::clamp<std::int64_t>(marginNs/1000,-1'000'000,1'000'000));next=(next+1)%window;if(n<window)++n;++windowSamples;
        if(++sinceEval<cfg.evalSamples)return;
        sinceEval=0;++evaluations;
        const std::int64_t p10=percentileNs(10),mid=(cfg.targetLoNs+cfg.targetHiNs)/2;lastP10=p10;lastP50=percentileNs(50);haveP=true;
        if(p10>=cfg.targetLoNs)earlierRun=0;                            // a healthy window: the earlier pushes reached the deadline
        if(p10<cfg.targetLoNs){++earlier;move(-std::clamp((mid-p10)/2,cfg.stepMinNs,cfg.stepMaxNs));}
        else if(p10>cfg.targetHiNs&&!hold){++later;move(std::clamp((p10-mid)/2,cfg.stepMinNs,cfg.stepMaxNs));}
    }
    // One paced frame on the controlled path: slew the offset toward the goal (<= slewNs); lastDelta = this frame's move (the
    // caller shifts the wake by it, so the phase moves at once and the slow lock sees no error). Offset and goal wrap together
    // into (-T/2, T/2] (the anchor is used modulo T, so a wrap does not move the wake).
    std::int64_t step(std::int64_t T) noexcept {
        if(T>0)lastT=T;
        lastDelta=std::clamp(goal-offset,-cfg.slewNs,cfg.slewNs);offset+=lastDelta;
        if(T>0){if(offset>T/2){offset-=T;goal-=T;++wraps;}else if(offset<=-T/2){offset+=T;goal+=T;++wraps;}}
        return offset;
    }
};

class FramePacer {
public:
    enum class Mode : unsigned {Auto=0,Lock36=1,Lock24=2,Adaptive=3};
    static constexpr unsigned modes=4,windowPairs=32,flipThreshold=4,maxHoldWindows=16,rateExpiryWindows=16;
    static constexpr unsigned adaptiveMode=3,byModeCount=5; // byMode AUTO, LOCK 36, LOCK 24, ADAPTIVE 36, ADAPTIVE 24
    static constexpr unsigned outlierRun=3; // Consecutive grid outliers (|err| > p/4) before the PLL re-anchors
    static constexpr unsigned leadSamples=32,gpuValidCalls=8; // PDT lead median window; GPU sample lifetime in pace() calls
    static constexpr std::int64_t freeGain=16,freeMaxStepNs=500'000; // AppSW slow lock gain (XR grid and wake) / max wake step
    static constexpr std::uint64_t throttleWaitedNs=250'000;          // A capture-throttle wait >= this = the frame waited for the GPU
    static const char* modeName(unsigned m) noexcept {return m==1?"LOCK 36":m==2?"LOCK 24":m==3?"ADAPTIVE":"AUTO";}
    static const char* byModeName(unsigned i) noexcept {return i==1?"LOCK 36":i==2?"LOCK 24":i==3?"ADAPTIVE 36":i==4?"ADAPTIVE 24":"AUTO";}
    static unsigned targetHz(unsigned m) noexcept {return m==1?36u:m==2?24u:0u;}
    // Rounded / ceiling integer division (b > 0).
    static std::int64_t roundDiv(std::int64_t a,std::int64_t b) noexcept {return a>=0?(a+b/2)/b:-((-a+b/2)/b);}
    static std::int64_t ceilDiv(std::int64_t a,std::int64_t b) noexcept {return a>=0?(a+b-1)/b:-((-a)/b);}
    // Number of display periods per paced frame (>= 1).
    static std::int64_t multiple(std::int64_t periodNs,unsigned hz) noexcept {
        if(periodNs<=0||!hz)return 1;
        return std::max<std::int64_t>(1,std::llround(1e9/double(periodNs)/double(hz)));
    }
    // Grid spacing: p, or the XR interval rounded to whole periods when it is > 1.5 p.
    static std::int64_t spacing(std::int64_t periodNs,std::int64_t xrIntervalNs) noexcept {
        if(periodNs<=0)return periodNs;
        if(xrIntervalNs>periodNs+periodNs/2)return std::max<std::int64_t>(1,std::llround(double(xrIntervalNs)/double(periodNs)))*periodNs;
        return periodNs;
    }
    // Wake-up target for the next frame: the first grid point (anchor + k*spacing) at or after
    // lastWake + interval - period/2. With spacing == period this is the grid point nearest to
    // lastWake + interval.
    static std::uint64_t target(std::uint64_t lastWake,std::int64_t interval,std::uint64_t anchor,std::int64_t spacing,std::int64_t period) noexcept {
        const std::uint64_t lo=lastWake+std::uint64_t(interval)-std::uint64_t(period/2);
        const std::int64_t diff=std::int64_t(lo-anchor); // modular difference, |diff| << 2^63
        return anchor+std::uint64_t(ceilDiv(diff,spacing)*spacing);
    }
    // The lastWake recorded after an overrun (now >= wake). Under a runtime-paced half-rate loop
    // (spacing s > p, AppSW) an overrun costs only the slot it missed: stay on the grid (drop the whole missed
    // slots), so the next frame targets the very next XR slot. At s == p lastWake = now; under s > p that
    // rule would push the next wake two slots ahead once the overrun exceeds p/2 (6.94 ms at 72 Hz).
    static std::uint64_t overrunWake(std::uint64_t wake,std::uint64_t now,std::int64_t spacing,std::int64_t period) noexcept {
        return spacing>period&&now>=wake?wake+(now-wake)/std::uint64_t(spacing)*std::uint64_t(spacing):now;
    }
    static std::int64_t wrapNs(std::int64_t x,std::int64_t m) noexcept {std::int64_t r=x%m;if(r<0)r+=m;return r>m/2?r-m:r;}
    // AppSW cadence T: the runtime's predicted display period when within +-10% of the lock interval, else the interval.
    static std::int64_t freeCadence(std::int64_t predicted,std::int64_t interval) noexcept {return predicted>=interval-interval/10&&predicted<=interval+interval/10?predicted:interval;}
    // Free-running half-rate wake: lastWake + T nudged toward the slow anchor by clamp(e/16, +-0.5 ms), e = wrap(anchor -
    // (lastWake + T)) into (-T/2, T/2]. An anchor move (runtime parity flip) can never make the pacer skip a slot. !lock: no anchor yet.
    static std::uint64_t freeWake(std::uint64_t lastWake,std::int64_t cadence,bool lock,std::uint64_t anchor,std::int64_t* error=nullptr) noexcept {
        const std::uint64_t base=lastWake+std::uint64_t(cadence);
        const std::int64_t e=lock?wrapNs(std::int64_t(anchor-base),cadence):0;if(error)*error=e;
        return base+std::uint64_t(std::clamp<std::int64_t>(e/freeGain,-freeMaxStepNs,freeMaxStepNs));
    }
    // ---------------------------------------------------------------- XR thread
    // After every xrWaitFrame. refreshHz: actual display rate (0 = unknown, then the
    // predicted period is used; under AppSW the predicted period may be doubled).
    // pdtMonoNs = predictedDisplayTime converted to CLOCK_MONOTONIC (XR_KHR_convert_timespec_time);
    // -1 = no conversion available, 0 = the conversion failed. Feeds the PDT grid (used by ADAPTIVE only) and its report.
    void observeXrFrame(std::uint64_t wakeNs,std::int64_t predictedPeriodNs,float refreshHz,std::int64_t pdtMonoNs=-1) noexcept {
        std::int64_t p=refreshHz>1.f&&std::isfinite(refreshHz)?std::int64_t(std::llround(1e9/double(refreshHz))):predictedPeriodNs;
        if(p<2'000'000||p>100'000'000)p=13'888'889;
        period_.store(p,std::memory_order_relaxed);predicted_.store(predictedPeriodNs,std::memory_order_relaxed);
        const auto last=lastXr_.load(std::memory_order_relaxed);
        if(last&&wakeNs>last){
            const std::int64_t d=std::int64_t(wakeNs-last);
            if(d<4*p){auto e=xrInterval_.load(std::memory_order_relaxed);e=e?e+(d-e)/8:d;xrInterval_.store(e,std::memory_order_relaxed);}
        }
        lastXr_.store(wakeNs,std::memory_order_relaxed);
        spacing_.store(spacing(p,xrInterval_.load(std::memory_order_relaxed)),std::memory_order_relaxed);
        auto g=grid_.load(std::memory_order_relaxed);
        if(!g)g=wakeNs;
        else{
            const std::uint64_t predicted=g+std::uint64_t(roundDiv(std::int64_t(wakeNs-g),p)*p);
            const std::int64_t err=std::int64_t(wakeNs-predicted);
            // A single late xrWaitFrame return (the XR thread itself late) is ignored; the grid
            // re-anchors only after outlierRun consecutive outliers (a real phase change of the runtime).
            if(std::llabs(err)>p/4){if(++outliers_>=outlierRun){g=wakeNs;++gridResets_;outliers_=0;}else{g=predicted;++gridOutliers_;}}
            else{outliers_=0;g=predicted+std::uint64_t(err/8);}
        }
        grid_.store(g,std::memory_order_relaxed);xrFrames_.fetch_add(1,std::memory_order_relaxed);
        observePdt(wakeNs,p,pdtMonoNs);
        // Slow phase lock of the half-rate XR grid for the AppSW free cadence (gain 1/16, wrap-aware, no outlier re-anchor;
        // (re)initialised on entering half rate or after a > 500 ms XR gap). s == p: cleared.
        const auto s=spacing_.load(std::memory_order_relaxed);
        if(s>p){
            const std::int64_t t=freeCadence(predictedPeriodNs,s);auto sg=slowGrid_.load(std::memory_order_relaxed);
            if(!sg||!last||wakeNs<=last||wakeNs-last>500'000'000ull){sg=wakeNs;slowInits_.fetch_add(1,std::memory_order_relaxed);}
            else{const std::int64_t e=wrapNs(std::int64_t(wakeNs-sg),t);sg=wakeNs-std::uint64_t(e)+std::uint64_t(e/freeGain);slowError_.store(e,std::memory_order_relaxed);
                auto a=slowAbsEma_.load(std::memory_order_relaxed);slowAbsEma_.store(a+(std::int64_t(std::llabs(e))-a)/freeGain,std::memory_order_relaxed);}
            slowGrid_.store(sg,std::memory_order_relaxed);slowFrames_.fetch_add(1,std::memory_order_relaxed);
        }else slowGrid_.store(0,std::memory_order_relaxed);
    }
    // FLIGHT RECORDER (XR thread, every xrEndFrame): one row per second, ring of flightRows (20 min) -> report framePacing.flight.
    // Columns: t s since the first tick; xr frames; stereo layer frames; new pairs shown; repeats (stereo frames re-showing the same pair);
    // fps = the in-headset FPS overlay value x10 (captured pairs/s, stereoHistory "CAPTURED"); gpu mean/max us (lite span of the frames
    // finished this second); busy mean/max us (render-thread time per paced frame); overruns; GPU temp x10 and clock MHz (kgsl);
    // synth = AppSW synthesizing; tight = lockstep ran.
    bool flightDue(std::uint64_t now) const noexcept {return !flNext_||now>=flNext_;}
    void flightTick(std::uint64_t now,bool stereo,bool newPair,bool repeat,float capturedHz,bool synth) noexcept {
        if(!flT0_){flT0_=now;flNext_=now+1'000'000'000ull;flOverrunsSeen_=overruns_.load(std::memory_order_relaxed);}
        ++flXr_;if(stereo)++flStereo_;if(newPair)++flNew_;if(repeat)++flRep_;
        if(now<flNext_)return;
        FlightRow r{};r.t=std::uint32_t((now-flT0_)/1'000'000'000ull);r.xr=flXr_;r.stereo=flStereo_;r.newPairs=flNew_;r.repeats=flRep_;
        r.fps10=capturedHz>0&&capturedHz<1000?std::uint32_t(capturedHz*10.f+0.5f):0;
        const auto gs=flGpuSum_.exchange(0,std::memory_order_relaxed),gn=flGpuN_.exchange(0,std::memory_order_relaxed),gm=flGpuMax_.exchange(0,std::memory_order_relaxed);
        const auto bs=flBusySum_.exchange(0,std::memory_order_relaxed),bn=flBusyN_.exchange(0,std::memory_order_relaxed),bm=flBusyMax_.exchange(0,std::memory_order_relaxed);
        r.gpuUs=gn?std::uint32_t(gs/gn/1000):0;r.gpuMaxUs=std::uint32_t(gm/1000);r.busyUs=bn?std::uint32_t(bs/bn/1000):0;r.busyMaxUs=std::uint32_t(bm/1000);
        const auto ov=overruns_.load(std::memory_order_relaxed);r.overruns=std::uint32_t(ov-flOverrunsSeen_);flOverrunsSeen_=ov;
        r.temp10=std::uint32_t(readSysLong("/sys/class/kgsl/kgsl-3d0/temp")/100);r.mhz=std::uint32_t(readSysLong("/sys/class/kgsl/kgsl-3d0/gpuclk")/1'000'000);
        r.synth=synth?1:0;r.tight=tightActive_.load(std::memory_order_relaxed)?1:0;
        // Throttling evidence - GPU devfreq ceiling + kgsl thermal power level, big-cluster CPU ceiling / current clock, Android thermal
        // status (0 none .. 6 shutdown) and headroom x100 (AThermal, 1.00 = the OS starts severe throttling; sampled every 2 s, NaN-safe).
        r.maxMhz=std::uint32_t(readSysLong("/sys/class/kgsl/kgsl-3d0/devfreq/max_freq")/1'000'000);
        r.tpl=std::uint32_t(readSysLong("/sys/class/kgsl/kgsl-3d0/thermal_pwrlevel"));
        r.cpuMaxMhz=std::uint32_t(readSysLong("/sys/devices/system/cpu/cpufreq/policy2/scaling_max_freq")/1000);
        r.cpuCurMhz=std::uint32_t(readSysLong("/sys/devices/system/cpu/cpufreq/policy2/scaling_cur_freq")/1000);
        if(!(r.t&1u))thermalSample();
        r.thStatus=std::uint32_t(thStatus_<0?99:thStatus_);r.headroom100=std::uint32_t(thHeadroom100_<0?9999:thHeadroom100_);
        heatWarning.observe(now,thStatus_,thHeadroom100_,r.temp10,r.maxMhz,r.tpl); // heat01: OVERHEAT WARNING trigger (1 Hz, HeatWarning.h)
        {std::lock_guard lk(flMutex_);flRing_[flNext0_]=r;flNext0_=(flNext0_+1)%flightRows;if(flCount_<flightRows)++flCount_;}
        flXr_=flStereo_=flNew_=flRep_=0;
        flNext_+=1'000'000'000ull;if(now>=flNext_)flNext_=now+1'000'000'000ull;
        // Every 10 s the whole ring goes to <files>/rdrvr/flight.json from a detached writer (closing the session from the
        // system menu kills the process without the exit report). Never blocks the XR thread.
        if(r.t%10==9&&!flWriting_.exchange(true,std::memory_order_acq_rel)){
            std::string path;{std::lock_guard lk(flMutex_);path=flPath_;}
            if(path.empty()){flWriting_.store(false,std::memory_order_release);}
            else{try{std::thread([this,path]{
                     try{const std::string body=flightJson();const std::string tmp=path+".tmp";
                         if(std::FILE* fp=std::fopen(tmp.c_str(),"wb")){std::fwrite(body.data(),1,body.size(),fp);std::fclose(fp);std::rename(tmp.c_str(),path.c_str());}}catch(...){}
                     flWriting_.store(false,std::memory_order_release);}).detach();}
                 catch(...){flWriting_.store(false,std::memory_order_release);}}
        }
    }
    void setFlightFile(const std::string& path) {std::lock_guard lk(flMutex_);flPath_=path;} // TraceEntry: <files>/rdrvr/flight.json
    std::string flightJson() const {
        std::ostringstream o;o<<"{\"cols\":[\"t\",\"xr\",\"stereo\",\"newPairs\",\"repeats\",\"fps10\",\"gpuUs\",\"gpuMaxUs\",\"busyUs\",\"busyMaxUs\",\"overruns\",\"temp10\",\"mhz\",\"synth\",\"tight\","
           "\"maxMhz\",\"tpl\",\"cpuMaxMhz\",\"cpuCurMhz\",\"thStatus\",\"headroom100\"],\"rows\":[";
        std::lock_guard lk(flMutex_);const unsigned first=(flNext0_+flightRows-flCount_)%flightRows;
        for(unsigned i=0;i<flCount_;++i){const auto& r=flRing_[(first+i)%flightRows];
            o<<(i?",":"")<<'['<<r.t<<','<<r.xr<<','<<r.stereo<<','<<r.newPairs<<','<<r.repeats<<','<<r.fps10<<','<<r.gpuUs<<','<<r.gpuMaxUs<<','<<r.busyUs<<','<<r.busyMaxUs
             <<','<<r.overruns<<','<<r.temp10<<','<<r.mhz<<','<<unsigned(r.synth)<<','<<unsigned(r.tight)
             <<','<<r.maxMhz<<','<<r.tpl<<','<<r.cpuMaxMhz<<','<<r.cpuCurMhz<<','<<r.thStatus<<','<<r.headroom100<<']';}
        o<<"]}";return o.str();
    }
    // SUBMIT GATE policy (render thread, before a game submit): lockstep ran the last paced frame and debug.rdrvr.submitgate != 0.
    bool submitGateArmed() const noexcept {return submitGate_.load(std::memory_order_relaxed)&&tightActive_.load(std::memory_order_relaxed);}
    std::uint64_t submitGateCapNs() const noexcept {return std::uint64_t(submitGateCapUs_.load(std::memory_order_relaxed))*1000;}
    // LOCKSTEP (XR thread): the views of this XR frame are published (gameViews) -> a render thread waiting in pace() starts.
    void notifyXrViews() noexcept {
        {std::lock_guard lk(tightM_);++tightSeq_;tightNs_=clockNs();}
        tightCv_.notify_all();
    }
    void observePresented(std::uint64_t renderFrameId,std::int64_t pdtRawNs=0) noexcept {
        ++sincePair_;
        if(!renderFrameId||renderFrameId==lastPair_)return;
        const bool first=!lastPair_;lastPair_=renderFrameId;
        const unsigned gap=sincePair_;sincePair_=0;
        // Overruns since the previous pair belong to this pair (per-pair attribution).
        const auto overruns=overruns_.load(std::memory_order_relaxed);
        const bool overrun=overruns!=overrunsAtPair_;overrunsAtPair_=overruns;
        const auto mode=mode_.load(std::memory_order_relaxed);
        const auto p=period_.load(std::memory_order_relaxed);const auto xi=xrInterval_.load(std::memory_order_relaxed);
        // Cadence in display periods (PDT differences, else XR-frame gaps scaled by the XR interval).
        std::int64_t gapP=0;
        if(pdtRawNs>0&&lastPairPdt_>0&&pdtRawNs>lastPairPdt_&&p>0)gapP=roundDiv(pdtRawNs-lastPairPdt_,p);
        lastPairPdt_=pdtRawNs>0?pdtRawNs:0;
        const unsigned hz=effectiveHz(mode);const unsigned index=modeIndex(mode);
        byMode_[index].pairs.fetch_add(1,std::memory_order_relaxed);
        const auto epoch=levelEpoch_.load(std::memory_order_relaxed)+modeEpoch_;
        if(epoch!=seenEpoch_){seenEpoch_=epoch;breakMask_.store(0,std::memory_order_relaxed);skipPairs_=2;lastGapP_=0;} // level / mode change
        if(first)return;
        std::lock_guard lock(mutex_);
        {
            const std::int64_t gp=gapP>0?gapP:p>0&&xi>0?std::max<std::int64_t>(1,std::llround(double(gap)*double(xi)/double(p))):std::int64_t(gap);
            cadencePdt_=gapP>0;++pairGapPeriods_[std::size_t(std::min<std::int64_t>(std::max<std::int64_t>(gp,1),6)-1)];
            const std::int64_t expected=hz?multiple(p,hz):lastGapP_; // AUTO: a break is a change of the pair length
            const bool brk=expected>0&&gp!=expected;lastGapP_=gp;
            if(skipPairs_)--skipPairs_;
            else{
                breakMask_.store((breakMask_.load(std::memory_order_relaxed)<<1)|(brk?1u:0u),std::memory_order_relaxed);
                if(brk){++cadenceBreaks_;byMode_[index].cadenceBreaks.fetch_add(1,std::memory_order_relaxed);}
            }
        }
        ++pairs_;++gaps_[std::min<unsigned>(std::max(gap,1u),5u)-1u];
        if(!mode||!p||!xi||!hz){resetWindow();return;}
        // ADAPTIVE judges the phase on PDT gaps (display periods) when known; every other mode on XR-frame gaps.
        const bool pdtPhase=mode==adaptiveMode&&gapP>0;
        const auto expected=pdtPhase?multiple(p,hz):std::max<std::int64_t>(1,std::llround(double(multiple(p,hz)*p)/double(xi)));
        const bool irregular=(pdtPhase?gapP:std::int64_t(gap))!=expected;
        if(irregular)++irregularPairs_;
        if(overrun)++overrunPairs_;
        irregularBits_=(irregularBits_<<1)|unsigned(irregular);overrunBits_=(overrunBits_<<1)|unsigned(overrun);
        if(++pairsInWindow_<windowPairs)return;
        endWindow();
    }
    // Menu value + AppSW force (AppSW renders at exactly half the display rate).
    void configure(unsigned frameRateSetting,bool spaceWarp,std::uint64_t now=diagnostics::monotonicNs()) noexcept {
        if(now>=nextProperties_){
            nextProperties_=now+1'000'000'000ull;
#if defined(__ANDROID__)
            char v[PROP_VALUE_MAX]{};
            property_.store(__system_property_get("debug.rdrvr.pacing",v)>0&&v[0]>='0'&&v[0]<='3'&&!v[1]?int(v[0]-'0'):-1,std::memory_order_relaxed);
            v[0]=0;manualPhase_.store(__system_property_get("debug.rdrvr.pacing.phaseus",v)>0?std::int64_t(std::strtol(v,nullptr,10))*1000:0,std::memory_order_relaxed);
            v[0]=0;autoPhase_.store(!(__system_property_get("debug.rdrvr.pacing.autophase",v)>0&&v[0]=='0'),std::memory_order_relaxed);
            v[0]=0;pdtGridEnabled_.store(!(__system_property_get("debug.rdrvr.pacing.grid",v)>0&&v[0]=='0'),std::memory_order_relaxed);
            const auto us=[&](const char* name,std::int64_t fallback){v[0]=0;if(__system_property_get(name,v)<=0)return fallback;
                const long x=std::strtol(v,nullptr,10);return x>=10'000&&x<=60'000?std::int64_t(x):fallback;};
            dropUs_.store(us("debug.rdrvr.pacing.dropus",26'500),std::memory_order_relaxed);upUs_.store(us("debug.rdrvr.pacing.upus",25'000),std::memory_order_relaxed);
            v[0]=0;staleProperty_.store(__system_property_get("debug.rdrvr.staleresubmit",v)>0&&(v[0]=='0'||v[0]=='1')&&!v[1]?int(v[0]-'0'):-1,std::memory_order_relaxed);
            v[0]=0;gpuLanesAlwaysOn.store(!(__system_property_get("debug.rdrvr.gpulite",v)>0&&v[0]=='0'),std::memory_order_relaxed);
            v[0]=0;appSwFree_.store(__system_property_get("debug.rdrvr.pacing.appswfree",v)>0&&v[0]>='0'&&v[0]<='3'&&!v[1]?int(v[0]-'0'):3,std::memory_order_relaxed);
            // SAT force (0 auto / 1 SAT / 2 never) and base margin (0..5000 us, else 1000)
            v[0]=0;satForce_.store(__system_property_get("debug.rdrvr.pacing.sat",v)>0&&v[0]>='0'&&v[0]<='2'&&!v[1]?int(v[0]-'0'):0,std::memory_order_relaxed);
            v[0]=0;{long x=-1;if(__system_property_get("debug.rdrvr.pacing.satmarginus",v)>0){char* e{};x=std::strtol(v,&e,10);if(e==v||*e)x=-1;}
                    satMarginNs_.store(x>=0&&x<=5000?std::int64_t(x)*1000:1'000'000,std::memory_order_relaxed);}
            v[0]=0;satCpuExit_.store(!(__system_property_get("debug.rdrvr.pacing.satcpu",v)>0&&v[0]=='0'),std::memory_order_relaxed);
            // Phase controller (0 off, else on) and late-latch budget (0..6000 us, unset / invalid -> 3000)
            v[0]=0;phaseCtl_.store(!(__system_property_get("debug.rdrvr.pacing.phasectl",v)>0&&v[0]=='0'),std::memory_order_relaxed);
            v[0]=0;tight_.store(__system_property_get("debug.rdrvr.pacing.tight",v)>0&&v[0]>='0'&&v[0]<='2'&&!v[1]?int(v[0]-'0'):2,std::memory_order_relaxed); // Lockstep: 0 off, 1 AUTO, 2 always (default)
            v[0]=0;submitGate_.store(!(__system_property_get("debug.rdrvr.submitgate",v)>0&&v[0]=='0'&&!v[1]),std::memory_order_relaxed);
            v[0]=0;{long x=-1;if(__system_property_get("debug.rdrvr.submitgate.capus",v)>0){char* e{};x=std::strtol(v,&e,10);if(e==v||*e)x=-1;}
                    submitGateCapUs_.store(x>=0&&x<=20000?int(x):4000,std::memory_order_relaxed);}
            v[0]=0;{long x=-1;if(__system_property_get("debug.rdrvr.pacing.tightgateus",v)>0){char* e{};x=std::strtol(v,&e,10);if(e==v||*e)x=-1;}
                    tightGateNs_.store(x>=0&&x<=8000?std::int64_t(x)*1000:2'800'000,std::memory_order_relaxed);}
            v[0]=0;{long x=-1;if(__system_property_get("debug.rdrvr.latchus",v)>0){char* e{};x=std::strtol(v,&e,10);if(e==v||*e)x=-1;}
                    latchUs_.store(x>=0&&x<=6000?int(x):3000,std::memory_order_relaxed);}
#endif
        }
        unsigned m=requestedMode(frameRateSetting);
        setting_.store(frameRateSetting,std::memory_order_relaxed);spaceWarp_.store(spaceWarp,std::memory_order_relaxed);
        if(spaceWarp&&m!=adaptiveMode)m=1;
        if(m!=mode_.load(std::memory_order_relaxed))++modeEpoch_;
        mode_.store(m,std::memory_order_relaxed);
        const int stale=staleProperty_.load(std::memory_order_relaxed);
        presentTelemetry.staleEnabled.store(stale==1||(stale<0&&m==adaptiveMode),std::memory_order_relaxed);
    }
    // The mode the menu/property asks for, before the SPACEWARP force (XrHarness: ADAPTIVE gate).
    unsigned requestedMode(unsigned frameRateSetting) const noexcept {
        const int p=property_.load(std::memory_order_relaxed);
        return p>=0?unsigned(p):frameRateSetting<modes?frameRateSetting:0u;
    }
    unsigned mode() const noexcept {return mode_.load(std::memory_order_relaxed);}
    std::int64_t periodNs() const noexcept {return period_.load(std::memory_order_relaxed);}
    std::int64_t xrIntervalNs() const noexcept {return xrInterval_.load(std::memory_order_relaxed);}
    std::int64_t predictedPeriodNs() const noexcept {return predicted_.load(std::memory_order_relaxed);}
    std::uint64_t xrFrames() const noexcept {return xrFrames_.load(std::memory_order_relaxed);}
    std::uint64_t gridNs() const noexcept {return grid_.load(std::memory_order_relaxed);}
    unsigned phaseIndex() const noexcept {return phaseIndex_.load(std::memory_order_relaxed);}
    void countOverrun() noexcept {overruns_.fetch_add(1,std::memory_order_relaxed);}
    std::uint64_t pdtGridNs() const noexcept {return pdtValid_.load(std::memory_order_relaxed)?pdtGrid_.load(std::memory_order_relaxed):0;} // 0 = invalid
    std::int64_t leadNs() const noexcept {return lead_.load(std::memory_order_relaxed);}
    unsigned cadenceBreaks() const noexcept {return unsigned(std::popcount(breakMask_.load(std::memory_order_relaxed)));}
    unsigned governorLevelHz() const noexcept {return mode_.load(std::memory_order_relaxed)==adaptiveMode?governorHz_.load(std::memory_order_relaxed):0u;}
    bool governorAllowsSpaceWarp() const noexcept {return governorLevelHz()==36&&appSwAllowed_.load(std::memory_order_relaxed);}
    // byState letter _pl: 0 AUTO, 1 LOCK 36, 2 LOCK 24, 3 ADAPTIVE at 36, 4 ADAPTIVE at 24.
    unsigned signatureCode() const noexcept {return modeIndex(mode_.load(std::memory_order_relaxed));}
    const char* overlayTag() const noexcept {const auto i=signatureCode();return i==3?"36A":i==4?"24A":"";}
    // XR thread: the AppSW budget guard tripped (ADAPTIVE) / the gate held a requested SPACEWARP this XR frame.
    void noteAppSwOverBudget() noexcept {appSwOverBudget_.fetch_add(1,std::memory_order_relaxed);}
    void noteAppSwBlocked() noexcept {appSwBlocks_.fetch_add(1,std::memory_order_relaxed);}
    // Free-cadence accessors / host-test seams (configure() re-reads appswfree on Android; the clock seam is null there).
    bool appSwFreeActive() const noexcept {return freeActive_.load(std::memory_order_relaxed);}
    int appSwFreeSetting() const noexcept {return appSwFree_.load(std::memory_order_relaxed);}
    void overrideAppSwFree(int v) noexcept {appSwFree_.store(v,std::memory_order_relaxed);}
    std::uint64_t slowGridNs() const noexcept {return slowGrid_.load(std::memory_order_relaxed);}
    std::int64_t lockErrorNs() const noexcept {return lockError_.load(std::memory_order_relaxed);}
    std::uint64_t slotSkipSleeps() const noexcept {return slotSkipSleeps_.load(std::memory_order_relaxed);}
    std::uint64_t anchorJumps() const noexcept {return anchorJumps_.load(std::memory_order_relaxed);}
    std::uint64_t overruns() const noexcept {return overruns_.load(std::memory_order_relaxed);}
    unsigned phaseFlips() const noexcept {std::lock_guard lock(mutex_);return unsigned(phaseFlips_);}
    // SAT accessors / host-test seams (the properties override these on Android once per second).
    bool satActive() const noexcept {return satActive_.load(std::memory_order_relaxed);}
    void overrideSat(int force,std::int64_t marginUs=1000,bool cpuExit=true) noexcept {satForce_.store(force,std::memory_order_relaxed);satMarginNs_.store(marginUs*1000,std::memory_order_relaxed);satCpuExit_.store(cpuExit,std::memory_order_relaxed);}
    AppSwSaturation satState() const {std::lock_guard lock(mutex_);return sat_;}
    void configureSatForTests(const AppSwSaturation::Config& c) {std::lock_guard lock(mutex_);sat_.cfg=c;}
    // Late-latch accessors (StereoMailbox, XR thread) and host-test seams (the properties override them once per second).
    std::int64_t lateLatchNs() const noexcept {return std::int64_t(latchUs_.load(std::memory_order_relaxed))*1000;}
    std::uint64_t lastXrWakeNs() const noexcept {return lastXr_.load(std::memory_order_relaxed);}
    std::int64_t lastPdtNs() const noexcept {return lastPdt_.load(std::memory_order_relaxed);}   // 0 = unknown
    bool halfRate() const noexcept {const auto p=period_.load(std::memory_order_relaxed);return p>0&&spacing_.load(std::memory_order_relaxed)>p;}
    bool phaseCtlEnabled() const noexcept {return phaseCtl_.load(std::memory_order_relaxed);}
    bool phaseCtlActive() const noexcept {return phaseActive_.load(std::memory_order_relaxed);}
    bool phaseCtlRan() const noexcept {return phaseRan_.load(std::memory_order_relaxed);}      // the last paced frame applied it
    void overridePhaseCtl(bool on) noexcept {phaseCtl_.store(on,std::memory_order_relaxed);}
    void overrideLateLatchUs(int us) noexcept {latchUs_.store(us,std::memory_order_relaxed);}
    PhaseController phaseState() const {std::lock_guard lock(mutex_);return phase_;}
    void configurePhaseForTests(const PhaseController::Config& c) {std::lock_guard lock(mutex_);phase_.cfg=c;}
    void resetDeadlineCursorForTests() noexcept {deadlineCursor_=pairDeadlines.total();}
    void overrideManualPhaseForTests(std::int64_t ns) noexcept {manualPhase_.store(ns,std::memory_order_relaxed);} // = debug.rdrvr.pacing.phaseus
    using TestClock=std::uint64_t(*)(void*);using TestSleep=void(*)(void*,std::uint64_t);
    void setClockForTests(TestClock now,TestSleep sleep,void* ctx) noexcept {testNow_=now;testSleep_=sleep;testCtx_=ctx;}
    // ---------------------------------------------------------------- render thread
    // Call only while holding no lock another thread may need. Returns the slept ns.
    // gpu = newest GPU lite span (GameXr); pace() without arguments = no GPU data (ADAPTIVE then stays at 24).
    std::uint64_t pace(const GpuCostSample& gpu={}) noexcept {
        const int saved=errno;
        const auto mode=mode_.load(std::memory_order_relaxed);
        const auto now=clockNs(); // diagnostics::monotonicNs() (host-test seam)
        // Cost inputs and per-mode accounting (every mode).
        const std::uint64_t busy=lastReturn_&&now>lastReturn_?now-lastReturn_:0;
        // This frame's capture-throttle wait (delta of the cumulative GameStereoCapture counter; fed = the writer runs).
        const bool thrFed=captureThrottleFed.load(std::memory_order_relaxed);const std::uint64_t thrCum=captureThrottleWaitNs.load(std::memory_order_relaxed);
        const std::uint64_t waitNs=thrSeen_&&thrCum>=thrCum_&&thrCum-thrCum_<500'000'000ull?thrCum-thrCum_:0;thrCum_=thrCum;thrSeen_=true;
        const bool waitedPrev=waitedPrev_;waitedPrev_=thrFed&&waitNs>=throttleWaitedNs;throttleWait_.store(waitNs,std::memory_order_relaxed);
        bool fresh=false;
        if(gpu.serial!=gpuSerial_){gpuSerial_=gpu.serial;if(gpu.frameNs>0&&gpu.frameNs<200'000'000ull){gpuNs_.store(gpu.frameNs,std::memory_order_relaxed);fresh=true;}}
        // FLIGHT RECORDER (render thread side): render-thread time between pace() calls and GPU lite spans, summed per second.
        if(busy&&busy<500'000'000ull){flBusySum_.fetch_add(busy,std::memory_order_relaxed);flBusyN_.fetch_add(1,std::memory_order_relaxed);flMax(flBusyMax_,busy);}
        if(fresh){flGpuSum_.fetch_add(gpu.frameNs,std::memory_order_relaxed);flGpuN_.fetch_add(1,std::memory_order_relaxed);flMax(flGpuMax_,gpu.frameNs);}
        {const auto age=gpuAge_.load(std::memory_order_relaxed);gpuAge_.store(fresh?0u:age<UINT32_MAX?age+1:age,std::memory_order_relaxed);}
        // Deadline samples published by the mailbox since the last call (always drained; the phase controller below uses
        // them only on its path, every other frame counts them as ignored).
        std::array<PairDeadline,16> dl{};unsigned dlN=0;
        {std::uint64_t lost=0;dlN=pairDeadlines.read(deadlineCursor_,dl.data(),unsigned(dl.size()),&lost);
         if(lost)deadlineLost_.fetch_add(lost,std::memory_order_relaxed);if(dlN)deadlineRead_.fetch_add(dlN,std::memory_order_relaxed);}
        dlPending_=dlN;phaseRan_.store(false,std::memory_order_relaxed); // finish() counts them as ignored unless the controller took them
        // Lockstep / the submit gate hold for THIS frame only - every non-lockstep return leaves them off
        // (a flag left on would let the submit gate stall 72/24 Hz frames by up to 4 ms).
        const bool tightWas=tightPrev_;tightPrev_=false;tightActive_.store(false,std::memory_order_relaxed);
        const unsigned index=modeIndex(mode);
        if(lastPace_&&now>lastPace_&&now-lastPace_<500'000'000ull)byMode_[index].ns.fetch_add(now-lastPace_,std::memory_order_relaxed);
        lastPace_=now;byMode_[index].frames.fetch_add(1,std::memory_order_relaxed);
        if((mode==adaptiveMode)!=governorActive_)switchGovernor(mode==adaptiveMode,now);
        if(!mode){lastWake_=0;++autoFrames_;pdtPaced_.store(false,std::memory_order_relaxed);freeActive_.store(false,std::memory_order_relaxed);satActive_.store(false,std::memory_order_relaxed);overrunRun_=0;return finish(saved,0);}
        const bool adaptive=mode==adaptiveMode;
        const auto p=period_.load(std::memory_order_relaxed);
        // ADAPTIVE anchors on the predictedDisplayTime grid when available; every other mode on the PLL.
        auto g=grid_.load(std::memory_order_relaxed);
        const bool pdt=adaptive&&pdtGridEnabled_.load(std::memory_order_relaxed)&&pdtValid_.load(std::memory_order_relaxed);
        if(pdt){const auto q=pdtGrid_.load(std::memory_order_relaxed);if(q)g=q;}
        pdtPaced_.store(pdt,std::memory_order_relaxed);
        const auto lastXr=lastXr_.load(std::memory_order_relaxed);
        if(p<=0||!g||!lastXr||now<lastXr||now-lastXr>500'000'000ull){lastWake_=now;++noGrid_;freeActive_.store(false,std::memory_order_relaxed);satActive_.store(false,std::memory_order_relaxed);overrunRun_=0;return finish(saved,0);}
        const auto interval=multiple(p,effectiveHz(mode))*p;
        if(!lastWake_||now<lastWake_||now-lastWake_>std::uint64_t(interval+4*p)){lastWake_=now;++resyncs_;overrunRun_=0;return finish(saved,0);} // A stall is no production sample
        auto s=spacing_.load(std::memory_order_relaxed);if(s<p)s=p;
        const std::int64_t offset=std::int64_t(phaseIndex_.load(std::memory_order_relaxed))*(s/2)+manualPhase_.load(std::memory_order_relaxed);
        const std::uint64_t anchor=g+std::uint64_t(offset);
        noteAnchor(g,s,p); // anchorJumps (raw grid, telemetry only)
        // AppSW free-running half-rate cadence (s > p, lock interval == XR interval; appswfree 1/2/3); otherwise target().
        const int fm=appSwFree_.load(std::memory_order_relaxed);
        const bool freeRun=fm>0&&s>p&&interval==s;freeActive_.store(freeRun,std::memory_order_relaxed);
        std::int64_t T=s,lockErr=0;std::uint64_t wake;bool satNow=false;
        const std::int64_t lanes=std::int64_t(laneCaptureNs.load(std::memory_order_relaxed)+laneDisplayNs.load(std::memory_order_relaxed));
        if(freeRun){
            const std::int64_t pdp=predicted_.load(std::memory_order_relaxed);T=freeCadence(pdp,interval);
            // SAT cadence (appswfree=3) while the GPU is saturated; otherwise (and fm 1/2) the free wake below.
            std::int64_t Ts=0;
            if(fm==3){std::lock_guard lock(mutex_);sat_.cfg.marginNs=satMarginNs_.load(std::memory_order_relaxed);sat_.cfg.cpuExit=satCpuExit_.load(std::memory_order_relaxed);
                if(fresh)sat_.addLite(gpu.frameNs);
                // bound = the previous frame overran (and waited on the throttle when fed); cpu = busy - this frame's wait (fed)
                Ts=sat_.step(now,T,lanes,fresh,overrunRun_>0,satForce_.load(std::memory_order_relaxed),gpuAge_.load(std::memory_order_relaxed),
                             overrunRun_>0&&(!thrFed||waitedPrev)?1:0,std::int64_t(busy-(thrFed?std::min(waitNs,busy):0)),thrFed);}
            else if(satDirty_){std::lock_guard lock(mutex_);sat_.reset(now);}
            satDirty_=fm==3;satNow=Ts>0;
            // LOCKSTEP: start at the next XR views publish (or at once when one came during this frame); no clock, no slow lock,
            // no phase controller (deadline samples are drained and counted as ignored). SAT keeps its own cadence (saturated GPU).
            // AUTO (tight 1) only while the GPU has headroom: EMA (1/16) of lite span + mailbox
            // lanes below T - gate opens it, above T - gate + 1 ms closes it.
            if(fresh){const std::int64_t c=std::int64_t(gpu.frameNs)+lanes;tightCost_=tightCost_?tightCost_+(c-tightCost_)/16:c;}
            {const std::int64_t gate=tightGateNs_.load(std::memory_order_relaxed);
             if(tightCost_>0){if(tightGate_&&tightCost_>T-gate+1'000'000){tightGate_=false;tightCloses_.fetch_add(1,std::memory_order_relaxed);}
                              else if(!tightGate_&&tightCost_<T-gate){tightGate_=true;tightOpens_.fetch_add(1,std::memory_order_relaxed);}}
             tightCostUs_.store(tightCost_/1000,std::memory_order_relaxed);tightGateOpen_.store(tightGate_,std::memory_order_relaxed);}
            const int tightMode=tight_.load(std::memory_order_relaxed);
            if(!satNow&&(tightMode==2||(tightMode==1&&tightGate_))){
                if(phaseActive_.load(std::memory_order_relaxed)){std::lock_guard lock(mutex_);phase_.reset();phaseActive_.store(false,std::memory_order_relaxed);}
                tightActive_.store(true,std::memory_order_relaxed);satActive_.store(false,std::memory_order_relaxed);
                bool waited=false,late=false,timeout=false;std::uint64_t pubNs=0;
                {std::unique_lock lk(tightM_);
                 if(!tightWas)tightConsumed_=0; // Re-entry - publishes from before (other path) are not a late frame
                 if(tightSeq_!=tightConsumed_&&tightConsumed_)late=true;      // the XR frame began during this render frame: start now
                 else{const auto seen=tightSeq_;
                      waited=tightCv_.wait_for(lk,std::chrono::nanoseconds(T+4'000'000),[&]{return tightSeq_!=seen;});timeout=!waited;}
                 tightConsumed_=tightSeq_;pubNs=tightNs_;}
                const auto after=clockNs();
                ++paced_;byMode_[index].paced.fetch_add(1,std::memory_order_relaxed);
                HalfRateCounters& h=halfRate_[0];h.paced.fetch_add(1,std::memory_order_relaxed);
                if(late){countOverrun();halfRateOverruns_.fetch_add(1,std::memory_order_relaxed);byMode_[index].overruns.fetch_add(1,std::memory_order_relaxed);
                         h.overruns.fetch_add(1,std::memory_order_relaxed);tightLate_.fetch_add(1,std::memory_order_relaxed);
                         {std::lock_guard lock(mutex_);noteBusy(busy);if(pubNs&&now>pubNs)late_.add(std::int64_t(now-pubNs));}}
                else{if(timeout)tightTimeouts_.fetch_add(1,std::memory_order_relaxed);else tightWaits_.fetch_add(1,std::memory_order_relaxed);
                     {std::lock_guard lock(mutex_);sleep_.add(std::int64_t(after-now));noteBusy(busy);if(pubNs&&after>=pubNs)phaseError_.add(std::int64_t(after-pubNs));}
                     ++slept_;byMode_[index].slept.fetch_add(1,std::memory_order_relaxed);h.slept.fetch_add(1,std::memory_order_relaxed);}
                if(lastWake_&&after>lastWake_)notePeriods(h,after-lastWake_,p);
                overrunRun_=late?overrunRun_+1:0;lastWake_=after;cadenceNs_.store(T,std::memory_order_relaxed);lockError_.store(0,std::memory_order_relaxed);
                tightPrev_=true;
                if(adaptive)governorStep(now,busy,late); // ADAPTIVE must still drop to 24 / revoke AppSW in lockstep
                return finish(saved,waited?after-now:0);
            }
            tightActive_.store(false,std::memory_order_relaxed);
            if(satNow){wake=lastWake_+std::uint64_t(Ts);T=Ts;cadenceNs_.store(Ts,std::memory_order_relaxed);cadenceRuntime_.store(false,std::memory_order_relaxed);lockError_.store(0,std::memory_order_relaxed);}
            else{
                const auto sg=slowGrid_.load(std::memory_order_relaxed);
                // Phase controller (appswfree 1/3, flips frozen, property on). Its offset rides on the anchor; this frame's
                // slew step (<= 0.5 ms) also shifts lastWake, so wake and anchor move together: the phase changes at once and the
                // slow lock sees no error. Samples are ignored while the render lock is off its anchor (runtime parity flip).
                std::int64_t ctl=0,shift=0;
                if(sg&&phaseCtl_.load(std::memory_order_relaxed)&&(fm==1||fm==3)){
                    std::lock_guard lock(mutex_);
                    if(!phaseActive_.load(std::memory_order_relaxed))phase_.reset();
                    for(unsigned i=0;i<dlN;++i){
                        if(!(dl[i].flags&DeadlineSynth)||lockLoud_){++phaseIgnored_;if(lockLoud_)++phaseIgnoredLock_;continue;}
                        phase_.add(dl[i].marginNs,dl[i].flags);
                    }
                    dlPending_=0;ctl=phase_.step(T);shift=phase_.lastDelta;phaseActive_.store(true,std::memory_order_relaxed);phaseRan_.store(true,std::memory_order_relaxed);
                }else if(phaseActive_.load(std::memory_order_relaxed)){std::lock_guard lock(mutex_);phase_.reset();phaseActive_.store(false,std::memory_order_relaxed);}
                wake=freeWake(std::uint64_t(std::int64_t(lastWake_)+shift),T,sg!=0,sg+std::uint64_t(offset+ctl),&lockErr);
                lockLoud_=std::llabs(lockErr)>lockQuietNs?lockLoudFrames:lockLoud_?lockLoud_-1:0u;
                cadenceNs_.store(T,std::memory_order_relaxed);cadenceRuntime_.store(T==pdp,std::memory_order_relaxed);lockError_.store(lockErr,std::memory_order_relaxed);
                if(std::llabs(lockErr/freeGain)>freeMaxStepNs)clampedSteps_.fetch_add(1,std::memory_order_relaxed);
            }
        }else{
            wake=target(lastWake_,interval,anchor,s,p);
            if(satDirty_){std::lock_guard lock(mutex_);sat_.reset(now);satDirty_=false;} // Off the free path (72 Hz, 24 Hz, AppSW off)
            if(phaseActive_.load(std::memory_order_relaxed)){std::lock_guard lock(mutex_);phase_.reset();phaseActive_.store(false,std::memory_order_relaxed);}
        }
        satActive_.store(satNow,std::memory_order_relaxed);
        const std::uint64_t prevWake=lastWake_;HalfRateCounters* hr=s>p?&halfRate_[satNow?2:freeRun?0:1]:nullptr;
        ++paced_;byMode_[index].paced.fetch_add(1,std::memory_order_relaxed);if(hr)hr->paced.fetch_add(1,std::memory_order_relaxed);
        if(now>=wake){
            countOverrun();if(s>p)halfRateOverruns_.fetch_add(1,std::memory_order_relaxed);
            byMode_[index].overruns.fetch_add(1,std::memory_order_relaxed);
            {std::lock_guard lock(mutex_);late_.add(std::int64_t(now-wake));noteBusy(busy);if(freeRun&&!satNow)lockErrorWin_.add(lockErr);
             // A back-to-back frame (>= 3 overruns before it): busy = the production interval -> SAT residual calibration.
             // Only a frame that waited on the capture throttle (when fed) is a GPU cycle.
             if(freeRun&&fm==3&&overrunRun_>=3)sat_.learn(std::int64_t(busy),lanes,!thrFed||waitNs>=throttleWaitedNs);}
            lastWake_=satNow?now:overrunWake(wake,now,T,p);
            if(overrunRun_<UINT32_MAX)++overrunRun_;
            if(hr){hr->overruns.fetch_add(1,std::memory_order_relaxed);notePeriods(*hr,lastWake_-prevWake,p);}
            if(adaptive)governorStep(now,busy,true);
            return finish(saved,0);
        }
        overrunRun_=0;
        if(wake-now>std::uint64_t(interval+2*s)){lastWake_=now;++resyncs_;return finish(saved,0);}
        if(adaptive)governorStep(now,busy,false);
        // A sleep that stretches the cadence beyond interval + p/2 without an overrun = a deliberately skipped slot.
        // Not counted on the SAT cadence (deliberately longer than the XR interval).
        if(!satNow&&wake-prevWake>std::uint64_t(interval+p/2)){slotSkipSleeps_.fetch_add(1,std::memory_order_relaxed);if(hr)hr->slotSkips.fetch_add(1,std::memory_order_relaxed);}
        sleepTo(wake);
        const auto after=clockNs();
        {std::lock_guard lock(mutex_);sleep_.add(std::int64_t(after-now));phaseError_.add(std::int64_t(after-wake));noteBusy(busy);if(freeRun&&!satNow)lockErrorWin_.add(lockErr);}
        if(hr){hr->slept.fetch_add(1,std::memory_order_relaxed);notePeriods(*hr,wake-prevWake,p);}
        ++slept_;byMode_[index].slept.fetch_add(1,std::memory_order_relaxed);lastWake_=wake;return finish(saved,after-now);
    }
    std::string report() const {
        const auto mode=mode_.load(std::memory_order_relaxed);const auto p=period_.load(std::memory_order_relaxed);
        const auto k=multiple(p,effectiveHz(mode));const auto s=spacing_.load(std::memory_order_relaxed);
        const bool hasRate=mode&&effectiveHz(mode);
        std::ostringstream o;
        o<<"{\"mode\":\""<<modeName(mode)<<"\",\"modeSetting\":"<<setting_.load(std::memory_order_relaxed)<<",\"propertyOverride\":"<<property_.load(std::memory_order_relaxed)
         <<",\"spaceWarpForced\":"<<(spaceWarp_.load(std::memory_order_relaxed)?"true":"false")
         <<",\"targetHz\":"<<(hasRate&&p>0?1e9/double(k*p):0.)<<",\"displayHz\":"<<(p>0?1e9/double(p):0.)<<",\"periodNs\":"<<p<<",\"multiple\":"<<(hasRate?k:0)
         <<",\"predictedDisplayPeriodNs\":"<<predicted_.load(std::memory_order_relaxed)
         <<",\"paced\":"<<paced_.load()<<",\"slept\":"<<slept_.load()<<",\"overruns\":"<<overruns_.load()<<",\"resyncs\":"<<resyncs_.load()
         <<",\"noGrid\":"<<noGrid_.load()<<",\"autoFrames\":"<<autoFrames_.load()<<",\"signatureCode\":"<<signatureCode()<<",\"overlayTag\":\""<<overlayTag()<<'"'
         // AppSW free cadence (effective on the last paced frame / property) and slot-loss counters (every mode).
         <<",\"appswFree\":"<<(freeActive_.load(std::memory_order_relaxed)?"true":"false")<<",\"appswFreeProperty\":"<<appSwFree_.load(std::memory_order_relaxed)
         <<",\"slotSkipSleeps\":"<<slotSkipSleeps_.load()<<",\"anchorJumps\":"<<anchorJumps_.load();
        {
            std::lock_guard lock(mutex_);
            // busyNs = render-thread time between pace() calls (now - lastReturn_, paced frames); appswPacer = half-rate detail.
            const auto lockErr=lockError_.load(std::memory_order_relaxed);
            const int fm=appSwFree_.load(std::memory_order_relaxed);const bool satOn=satActive_.load(std::memory_order_relaxed); // 3 = SAT (flips frozen as 1)
            o<<",\"busyNs\":"<<busy_.json()<<",\"appswPacer\":{\"flips\":\""<<(fm==2?"slewed":fm==1||fm==3?"frozen":"normal")<<"\",\"frozenWindows\":"<<frozenWindows_
             <<",\"cadenceNs\":"<<cadenceNs_.load(std::memory_order_relaxed)<<",\"cadenceSource\":\""<<(satOn?"sat":cadenceRuntime_.load(std::memory_order_relaxed)?"predictedDisplayPeriod":"interval")
             <<"\",\"phaseErrorUs\":"<<lockErr/1000<<",\"lockErrorNs\":"<<lockErrorWin_.json()<<",\"clampedSteps\":"<<clampedSteps_.load()
             <<",\"slowLock\":{\"frames\":"<<slowFrames_.load()<<",\"inits\":"<<slowInits_.load()<<",\"valid\":"<<(slowGrid_.load(std::memory_order_relaxed)?"true":"false")
             <<",\"lastErrorUs\":"<<slowError_.load(std::memory_order_relaxed)/1000<<",\"absErrorEmaUs\":"<<slowAbsEma_.load(std::memory_order_relaxed)/1000<<'}'
             <<",\"anchorJumpsHalfRate\":"<<anchorJumpsHalfRate_.load()<<",\"free\":"<<halfRate_[0].json()<<",\"grid\":"<<halfRate_[1].json();
            // SAT controller state (active = the last paced frame used the SAT cadence) and the SAT path counters.
            const auto& st=sat_;
            o<<",\"sat\":{\"active\":"<<(satOn?"true":"false")<<",\"mode\":\""<<(st.sat?"SAT":"FREE")<<"\",\"property\":"<<fm<<",\"force\":"<<satForce_.load(std::memory_order_relaxed)
             <<",\"costUs\":"<<st.lastCost/1000<<",\"liteMeanUs\":"<<st.liteMeanNs()/1000<<",\"lanesUs\":"<<st.lastLanes/1000<<",\"residualUs\":"<<st.residualNs()/1000
             <<",\"residualSource\":\""<<(st.residualIsLearned()?"learned":"default")<<"\",\"learnedSamples\":"<<st.learned<<",\"learnSkips\":"<<st.learnSkips<<",\"learnRejects\":"<<st.learnRejects<<",\"productionUs\":"<<st.productionNs()/1000
             <<",\"marginUs\":"<<st.margin/1000<<",\"marginBaseUs\":"<<satMarginNs_.load(std::memory_order_relaxed)/1000<<",\"cadenceUs\":"<<st.cadence/1000<<",\"targetUs\":"<<st.lastTarget/1000
             <<",\"periodUs\":"<<st.lastT/1000<<",\"capUs\":"<<std::int64_t(st.cfg.capPeriods)*st.lastT/1000
             <<",\"enters\":"<<st.enters<<",\"exits\":"<<st.exits<<",\"reason\":\""<<AppSwSaturation::reasonName(st.reason)<<"\",\"lastExit\":\""<<AppSwSaturation::exitName(st.lastExit)
             <<"\",\"paced\":"<<st.paced<<",\"overruns\":"<<st.overruns<<",\"marginBumps\":"<<st.marginBumps<<",\"capped\":"<<st.capped<<",\"noGpu\":"<<st.noGpu
             <<",\"holdMs\":"<<st.hold/1'000'000ull<<",\"samples\":"<<st.n<<",\"above\":"<<st.above<<",\"below\":"<<st.below
             // armed (SAT state, free wake: C + bumps <= T), saturation proof, exit EMA, render-thread check, throttle feed
             <<",\"armed\":"<<(st.sat&&st.armed?"true":"false")<<",\"armedFree\":"<<st.armedFree<<",\"bound\":"<<std::popcount(st.boundMask)<<",\"costEmaUs\":"<<st.costEma/1000
             <<",\"cpuExit\":"<<(st.cfg.cpuExit?"true":"false")<<",\"cpuP25Us\":"<<st.cpuP25Ns()/1000<<",\"cpuSamples\":"<<st.cn<<",\"cpuExits\":"<<st.cpuExits<<",\"cpuProbes\":"<<st.cpuProbes<<",\"overrunExits\":"<<st.overrunExits
             <<",\"calibrationDrops\":"<<st.drops<<",\"learnCpuSkips\":"<<st.learnCpuSkips<<",\"throttleFed\":"<<(captureThrottleFed.load(std::memory_order_relaxed)?"true":"false")
             <<",\"throttleWaitUs\":"<<throttleWait_.load(std::memory_order_relaxed)/1000<<'}'
             <<",\"satPath\":"<<halfRate_[2].json();
            // Deadline-aware phase controller (offset on the free path's anchor; margins = due latch - ready, exact samples).
            const auto& ph=phase_;
            o<<",\"phasectl\":{\"property\":"<<(phaseCtl_.load(std::memory_order_relaxed)?1:0)<<",\"active\":"<<(phaseRan_.load(std::memory_order_relaxed)?"true":"false")
             <<",\"offsetUs\":"<<ph.offset/1000<<",\"goalUs\":"<<ph.goal/1000<<",\"marginP10Us\":"<<(ph.haveP?ph.lastP10/1000:0)<<",\"marginP50Us\":"<<(ph.haveP?ph.lastP50/1000:0)
             <<",\"windowP10Us\":"<<ph.percentileNs(10)/1000<<",\"window\":"<<ph.n<<",\"samples\":"<<ph.samples<<",\"misses\":"<<ph.misses<<",\"missesInexact\":"<<ph.missesInexact
             <<",\"missesNoData\":"<<ph.missesNoData<<",\"exactAgo\":"<<(ph.sinceExact>=UINT32_MAX/2?-1ll:(long long)ph.sinceExact)
             <<",\"rescued\":"<<ph.rescued<<",\"adjust\":{\"evaluations\":"<<ph.evaluations<<",\"earlier\":"<<ph.earlier<<",\"later\":"<<ph.later<<",\"missPushes\":"<<ph.missPushes
             <<",\"lastUs\":"<<ph.lastAdjust/1000<<",\"spins\":"<<ph.spins<<",\"wraps\":"<<ph.wraps<<'}'<<",\"hold\":"<<ph.hold<<",\"settle\":"<<ph.settle
             <<",\"ignored\":"<<phaseIgnored_<<",\"ignoredLock\":"<<phaseIgnoredLock_<<",\"ringRead\":"<<deadlineRead_.load(std::memory_order_relaxed)
             <<",\"ringLost\":"<<deadlineLost_.load(std::memory_order_relaxed)<<",\"ringTotal\":"<<pairDeadlines.total()
             <<",\"targetUs\":["<<ph.cfg.targetLoNs/1000<<','<<ph.cfg.targetHiNs/1000<<"],\"latchUs\":"<<latchUs_.load(std::memory_order_relaxed)<<'}'
             <<",\"tight\":{\"property\":"<<tight_.load(std::memory_order_relaxed)<<",\"active\":"<<(tightActive_.load(std::memory_order_relaxed)?"true":"false")
             <<",\"waits\":"<<tightWaits_.load(std::memory_order_relaxed)<<",\"late\":"<<tightLate_.load(std::memory_order_relaxed)<<",\"timeouts\":"<<tightTimeouts_.load(std::memory_order_relaxed)
             <<",\"gateOpen\":"<<(tightGateOpen_.load(std::memory_order_relaxed)?"true":"false")<<",\"costUs\":"<<tightCostUs_.load(std::memory_order_relaxed)
             <<",\"gateUs\":"<<tightGateNs_.load(std::memory_order_relaxed)/1000<<",\"opens\":"<<tightOpens_.load(std::memory_order_relaxed)<<",\"closes\":"<<tightCloses_.load(std::memory_order_relaxed)<<"}}";
            o<<",\"sleepNs\":"<<sleep_.json()<<",\"phaseErrorNs\":"<<phaseError_.json()<<",\"overrunLateNs\":"<<late_.json()
             <<",\"xr\":{\"frames\":"<<xrFrames_.load()<<",\"intervalNs\":"<<xrInterval_.load(std::memory_order_relaxed)<<",\"gridSpacingNs\":"<<s
             <<",\"gridResets\":"<<gridResets_.load()<<",\"gridOutliers\":"<<gridOutliers_.load()<<",\"halfRateOverruns\":"<<halfRateOverruns_.load()<<",\"phaseIndex\":"<<phaseIndex_.load(std::memory_order_relaxed)
             <<",\"phaseOffsetNs\":"<<std::int64_t(phaseIndex_.load(std::memory_order_relaxed))*(s/2)<<",\"manualPhaseNs\":"<<manualPhase_.load(std::memory_order_relaxed)
             <<",\"autoPhase\":"<<(autoPhase_.load(std::memory_order_relaxed)?"true":"false")<<",\"phaseFlips\":"<<phaseFlips_
             <<",\"productiveFlips\":"<<productiveFlips_<<",\"unproductiveFlips\":"<<unproductiveFlips_<<",\"flipBacks\":"<<flipBacks_
             <<",\"holdWindows\":"<<holdWindows_<<",\"windows\":"<<windows_<<",\"lastWindowIrregular\":"<<lastWindowIrregular_
             <<",\"phaseIrregularRate\":["<<rate_[0]<<','<<rate_[1]<<"]"
             <<",\"pairs\":"<<pairs_<<",\"irregularPairs\":"<<irregularPairs_<<",\"overrunPairs\":"<<overrunPairs_
             <<",\"pairGapXrFrames\":["<<gaps_[0]<<','<<gaps_[1]<<','<<gaps_[2]<<','<<gaps_[3]<<','<<gaps_[4]<<"]}";
            // Pair cadence in display periods (pdt = predictedDisplayTime differences).
            o<<",\"cadence\":{\"source\":\""<<(cadencePdt_?"pdt":"xrframes")<<"\",\"pairGapPeriods\":[";
            for(unsigned i=0;i<pairGapPeriods_.size();++i)o<<(i?",":"")<<pairGapPeriods_[i];
            o<<"],\"breaks\":"<<cadenceBreaks_<<",\"recentBreaks32\":"<<cadenceBreaks()<<'}';
        }
        o<<",\"grid\":{\"source\":\""<<(pdtPaced_.load(std::memory_order_relaxed)?"pdt":"pll")<<"\",\"pdtEnabled\":"<<(pdtGridEnabled_.load(std::memory_order_relaxed)?"true":"false")
         <<",\"pdtValid\":"<<(pdtValid_.load(std::memory_order_relaxed)?"true":"false")<<",\"pdtFrames\":"<<pdtFrames_.load()<<",\"leadNs\":"<<lead_.load(std::memory_order_relaxed)
         <<",\"lateWakes\":"<<lateWakes_.load()<<",\"convertFailures\":"<<convertFailures_.load()<<",\"convertUnavailable\":"<<convertUnavailable_.load()<<'}';
        o<<",\"byMode\":{";
        for(unsigned i=0;i<byModeCount;++i){const auto& b=byMode_[i];
            o<<(i?",":"")<<'"'<<byModeName(i)<<"\":{\"frames\":"<<b.frames.load()<<",\"paced\":"<<b.paced.load()<<",\"slept\":"<<b.slept.load()<<",\"overruns\":"<<b.overruns.load()
             <<",\"ms\":"<<b.ns.load()/1'000'000ull<<",\"pairs\":"<<b.pairs.load()<<",\"cadenceBreaks\":"<<b.cadenceBreaks.load()<<'}';}
        o<<'}';
        {
            std::lock_guard lock(governorMutex_);const auto& gv=governor_;const auto now=diagnostics::monotonicNs();
            const bool on=governorActive_;
            const auto gpuNs=gpuNs_.load(std::memory_order_relaxed);const auto gpuAge=gpuAge_.load(std::memory_order_relaxed);
            const char* status=!on?"OFF":(gpuAge>gpuValidCalls||!gpuNs)?"NO_GPU_DATA":"OK";
            o<<",\"governor\":{\"active\":"<<(on?"true":"false")<<",\"level\":"<<(on?gv.level:0u)<<",\"ups\":"<<gv.ups<<",\"downs\":"<<gv.downs
             <<",\"downBy\":{\"overruns\":"<<gv.downBy[PacingGovernor::ReasonOverruns]<<",\"costP90\":"<<gv.downBy[PacingGovernor::ReasonCostP90]<<",\"cadence\":"<<gv.downBy[PacingGovernor::ReasonCadence]<<'}'
             <<",\"lastDownBy\":\""<<PacingGovernor::reasonName(gv.lastReason)<<"\",\"holdMs\":"<<gv.hold/1'000'000ull
             <<",\"sinceChangeMs\":"<<(on&&now>gv.tChange?(now-gv.tChange)/1'000'000ull:0)<<",\"samples\":"<<gv.n<<",\"samplesTotal\":"<<gv.samplesTotal
             <<",\"costP50Us\":"<<gv.percentileUs(72,50)<<",\"costP90Us\":"<<gv.percentileUs(36,90)<<",\"costP95Us\":"<<gv.percentileUs(72,95)
             <<",\"gpuLiteUs\":"<<gpuNs/1000<<",\"gpuAgeCalls\":"<<gpuAge<<",\"lanesUs\":"<<(laneCaptureNs.load(std::memory_order_relaxed)+laneDisplayNs.load(std::memory_order_relaxed))/1000
             <<",\"cpuWorkP50Us\":"<<gv.percentileUs(72,50,true)<<",\"gpuMissing\":"<<gv.gpuMissing<<",\"gpuSamples72\":"<<gv.gpuSamples(72)
             <<",\"msAt36\":"<<byMode_[3].ns.load()/1'000'000ull<<",\"msAt24\":"<<byMode_[4].ns.load()/1'000'000ull
             <<",\"appSwAllowed\":"<<(gv.appSw?"true":"false")<<",\"appSwBlocks\":"<<appSwBlocks_.load()<<",\"appSwTrips\":"<<gv.appSwTrips<<",\"appSwRevokes\":"<<gv.appSwRevokes
             <<",\"dropUs\":"<<gv.cfg.dropNs/1000<<",\"upUs\":"<<gv.cfg.upNs/1000<<",\"status\":\""<<status<<"\"}";
        }
        o<<",\"noLayerFrames\":"<<presentTelemetry.noLayerJson()<<",\"layerSettings\":"<<presentTelemetry.layerSettingsJson()<<",\"xrHistory\":"<<xrHistory.json()
         <<",\"heatWarning\":"<<heatWarning.json(diagnostics::monotonicNs()) // heat01
         <<",\"gpuBoost\":"<<gpuBoost.json() // boost01
         <<",\"flight\":"<<flightJson()<<'}';
        return o.str();
    }
private:
    // Signed sample window (last 256), exact nearest-rank percentiles at report time.
    struct Window {
        std::array<std::int64_t,256> v{};unsigned n{},next{};std::uint64_t total{};
        void add(std::int64_t x) noexcept {v[next]=x;next=(next+1)%v.size();if(n<v.size())++n;++total;}
        std::string json() const {
            std::ostringstream o;o<<"{\"samples\":"<<n<<",\"total\":"<<total;
            if(n){auto s=v;std::sort(s.begin(),s.begin()+n);long double sum{};for(unsigned i=0;i<n;++i)sum+=s[i];
                o<<",\"min\":"<<s[0]<<",\"p50\":"<<s[(n*50+99)/100-1]<<",\"p95\":"<<s[(n*95+99)/100-1]<<",\"max\":"<<s[n-1]<<",\"mean\":"<<std::int64_t(sum/n);}
            o<<'}';return o.str();
        }
    };
    // Per-mode counters (index = signatureCode()).
    struct ModeCounters {std::atomic<std::uint64_t> frames{},paced{},slept{},overruns{},ns{},pairs{},cadenceBreaks{};};
    // Half-rate (s > p) paced frames per path ([0] free, [1] grid snap, [2] SAT); periods = cadence-point (lastWake) deltas
    // rounded to display periods 1, 2, 3, 4, 5+.
    struct HalfRateCounters {
        std::atomic<std::uint64_t> paced{},slept{},overruns{},slotSkips{};std::array<std::atomic<std::uint64_t>,5> periods{};
        std::string json() const {
            std::ostringstream o;o<<"{\"paced\":"<<paced.load()<<",\"slept\":"<<slept.load()<<",\"overruns\":"<<overruns.load()<<",\"slotSkipSleeps\":"<<slotSkips.load()<<",\"wakeIntervalPeriods\":[";
            for(unsigned i=0;i<periods.size();++i)o<<(i?",":"")<<periods[i].load();
            o<<"]}";return o.str();
        }
    };
    static void notePeriods(HalfRateCounters& c,std::uint64_t d,std::int64_t p) noexcept {
        const std::int64_t k=std::clamp<std::int64_t>(roundDiv(std::int64_t(d),p),1,5);c.periods[std::size_t(k-1)].fetch_add(1,std::memory_order_relaxed);
    }
    // |raw grid move| > p/4 (wrapped to the spacing) between paced pace() calls; deliberate phase offsets are not part of g.
    void noteAnchor(std::uint64_t g,std::int64_t s,std::int64_t p) noexcept {
        if(prevGrid_&&s==prevSpacing_&&std::llabs(wrapNs(std::int64_t(g-prevGrid_),s))>p/4){
            anchorJumps_.fetch_add(1,std::memory_order_relaxed);if(s>p)anchorJumpsHalfRate_.fetch_add(1,std::memory_order_relaxed);}
        prevGrid_=g;prevSpacing_=s;
    }
    void noteBusy(std::uint64_t busy) noexcept {if(busy&&busy<500'000'000ull)busy_.add(std::int64_t(busy));} // caller holds mutex_
    std::uint64_t clockNs() const noexcept {return testNow_?testNow_(testCtx_):diagnostics::monotonicNs();}
    void sleepTo(std::uint64_t ns) noexcept {if(testSleep_)testSleep_(testCtx_,ns);else sleepUntil(ns);}
    static void sleepUntil(std::uint64_t ns) noexcept {
#if defined(__ANDROID__) || defined(__linux__)
        timespec ts{time_t(ns/1'000'000'000ull),long(ns%1'000'000'000ull)};
        for(unsigned i=0;i<64;++i){const int r=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&ts,nullptr);if(r!=EINTR)break;}
#else
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(ns)));
#endif
    }
    unsigned effectiveHz(unsigned mode) const noexcept {return mode==adaptiveMode?governorHz_.load(std::memory_order_relaxed):targetHz(mode);}
    unsigned modeIndex(unsigned mode) const noexcept {return mode==adaptiveMode?(governorHz_.load(std::memory_order_relaxed)==36?3u:4u):mode<3?mode:0u;}
    std::uint64_t finish(int saved,std::uint64_t slept) noexcept {
        if(dlPending_){std::lock_guard lock(mutex_);phaseIgnored_+=dlPending_;dlPending_=0;} // Deadline samples no controller took
        lastReturn_=clockNs();errno=saved;return slept;}
    // PDT grid (XR thread). Lead = median of the last 32 (pdt - wake); grid = pdt - lead.
    void observePdt(std::uint64_t wakeNs,std::int64_t p,std::int64_t pdtMonoNs) noexcept {
        if(pdtMonoNs<0){convertUnavailable_.fetch_add(1,std::memory_order_relaxed);pdtValid_.store(false,std::memory_order_relaxed);lastPdt_.store(0,std::memory_order_relaxed);return;}
        const std::int64_t lead=pdtMonoNs-std::int64_t(wakeNs);
        if(!pdtMonoNs||std::llabs(lead)>=500'000'000){convertFailures_.fetch_add(1,std::memory_order_relaxed);pdtValid_.store(false,std::memory_order_relaxed);lastPdt_.store(0,std::memory_order_relaxed);return;}
        leads_[leadNext_]=lead;leadNext_=(leadNext_+1)%leadSamples;if(leadCount_<leadSamples)++leadCount_;
        std::array<std::int64_t,leadSamples> s=leads_;std::nth_element(s.begin(),s.begin()+leadCount_/2,s.begin()+leadCount_);
        const std::int64_t median=s[leadCount_/2];
        const std::uint64_t grid=std::uint64_t(pdtMonoNs-median);
        if(std::llabs(std::int64_t(wakeNs-grid))>p/4)lateWakes_.fetch_add(1,std::memory_order_relaxed);
        lead_.store(median,std::memory_order_relaxed);pdtGrid_.store(grid,std::memory_order_relaxed);pdtValid_.store(true,std::memory_order_relaxed);
        lastPdt_.store(pdtMonoNs,std::memory_order_relaxed); // This XR frame's predictedDisplayTime (late-latch guard)
        pdtFrames_.fetch_add(1,std::memory_order_relaxed);
    }
    // Governor entry/exit (render thread).
    void switchGovernor(bool on,std::uint64_t now) noexcept {
        std::lock_guard lock(governorMutex_);governorActive_=on;
        if(on){governor_.enter(now);governorHz_.store(governor_.level,std::memory_order_relaxed);}
        else{governor_.leave();governorHz_.store(0,std::memory_order_relaxed);}
        appSwAllowed_.store(false,std::memory_order_relaxed);levelEpoch_.fetch_add(1,std::memory_order_relaxed);
    }
    // One governor sample (ADAPTIVE, a paced frame; not after a resync / grid loss).
    void governorStep(std::uint64_t now,std::uint64_t busy,bool overrun) noexcept {
        if(busy>500'000'000ull)return;
        const auto gpuNs=gpuNs_.load(std::memory_order_relaxed);
        const bool gpuValid=gpuNs>0&&gpuAge_.load(std::memory_order_relaxed)<=gpuValidCalls;
        const std::uint64_t lanes=laneCaptureNs.load(std::memory_order_relaxed)+laneDisplayNs.load(std::memory_order_relaxed);
        const std::uint64_t cost=std::max<std::uint64_t>(gpuValid?gpuNs+lanes:0,busy);
        const unsigned breaks=cadenceBreaks();
        std::lock_guard lock(governorMutex_);
        governor_.cfg.dropNs=std::uint64_t(dropUs_.load(std::memory_order_relaxed))*1000;governor_.cfg.upNs=std::uint64_t(upUs_.load(std::memory_order_relaxed))*1000;
        const auto trips=appSwOverBudget_.load(std::memory_order_relaxed);
        if(trips!=appSwTripsSeen_){appSwTripsSeen_=trips;governor_.noteAppSwOverBudget(now);}
        if(governor_.step(now,cost,gpuValid,overrun,breaks,busy)){
            governorHz_.store(governor_.level,std::memory_order_relaxed);levelEpoch_.fetch_add(1,std::memory_order_relaxed);
            breakMask_.store(0,std::memory_order_relaxed);
        }
        appSwAllowed_.store(governor_.appSw,std::memory_order_relaxed);
    }
    void resetWindow() noexcept {irregularBits_=overrunBits_=0;pairsInWindow_=0;}
    // Caller holds mutex_; a full window of windowPairs pairs has been recorded.
    void endWindow() noexcept {
        // Irregular pairs at or right after an overrun pair are the overrun's, not the phase's.
        const unsigned effective=irregularBits_&~(overrunBits_|(overrunBits_<<1));
        const int r=std::popcount(effective);
        const unsigned cur=phaseIndex_.load(std::memory_order_relaxed),other=cur^1u;
        ++windows_;lastWindowIrregular_=r;rate_[cur]=r;rateWindow_[cur]=windows_;
        resetWindow();
        if(!autoPhase_.load(std::memory_order_relaxed)){evaluating_=false;return;}
        // On the AppSW free cadence (appswfree=1) a binary s/2 flip would move the anchor by a whole period: frozen.
        // appswfree=2 keeps the flips; the slow lock then slews to the new anchor (<= 0.5 ms per frame, no slot skip).
        // Frozen for appswfree=3 as well (SAT or its free path).
        if(freeActive_.load(std::memory_order_relaxed)&&(appSwFree_.load(std::memory_order_relaxed)==1||appSwFree_.load(std::memory_order_relaxed)==3)){evaluating_=false;++frozenWindows_;return;}
        if(evaluating_){ // first window after a flip
            evaluating_=false;sinceFlip_=0;
            if(r<rateBeforeFlip_){++productiveFlips_;holdWindows_=1;return;}
            ++unproductiveFlips_;holdWindows_=std::min(holdWindows_*2,maxHoldWindows);
            if(r>rateBeforeFlip_){phaseIndex_.store(other,std::memory_order_relaxed);++flipBacks_;} // the new phase is worse: return
            return;
        }
        ++sinceFlip_;
        const bool otherKnown=rate_[other]>=0&&windows_-rateWindow_[other]<=rateExpiryWindows;
        if(r>=int(flipThreshold)&&sinceFlip_>=holdWindows_&&(!otherKnown||rate_[other]<r)){
            rateBeforeFlip_=r;phaseIndex_.store(other,std::memory_order_relaxed);++phaseFlips_;evaluating_=true;sinceFlip_=0;
        }
    }
    std::atomic<unsigned> mode_{},setting_{},phaseIndex_{};std::atomic<int> property_{-1};std::atomic<bool> spaceWarp_{},autoPhase_{true};
    std::atomic<std::int64_t> period_{},predicted_{},xrInterval_{},spacing_{},manualPhase_{};
    std::atomic<std::uint64_t> grid_{},lastXr_{},xrFrames_{},gridResets_{},gridOutliers_{},halfRateOverruns_{};
    unsigned outliers_{};                                              // XR thread
    std::atomic<std::uint64_t> paced_{},slept_{},overruns_{},resyncs_{},noGrid_{},autoFrames_{};
    std::uint64_t nextProperties_{};                                   // XR thread
    std::uint64_t lastWake_{};                                         // render thread
    std::uint64_t lastPair_{},overrunsAtPair_{};unsigned sincePair_{}; // XR thread
    mutable std::mutex mutex_;                                         // windows + controller state below
    Window sleep_,phaseError_,late_;
    std::uint64_t pairs_{},irregularPairs_{},overrunPairs_{},phaseFlips_{},productiveFlips_{},unproductiveFlips_{},flipBacks_{},windows_{};
    std::array<std::uint64_t,5> gaps_{};
    unsigned irregularBits_{},overrunBits_{},pairsInWindow_{},sinceFlip_{},holdWindows_{1};
    std::array<int,2> rate_{-1,-1};std::array<std::uint64_t,2> rateWindow_{};int rateBeforeFlip_{},lastWindowIrregular_{-1};bool evaluating_{};
    // ---- ADAPTIVE governor, PDT grid, cadence and per-mode counters
    // properties (XR thread writes)
    std::atomic<bool> pdtGridEnabled_{true};std::atomic<std::int64_t> dropUs_{26'500},upUs_{25'000};std::atomic<int> staleProperty_{-1};
    // PDT grid (XR thread writes)
    std::array<std::int64_t,leadSamples> leads_{};unsigned leadNext_{},leadCount_{};
    std::atomic<std::int64_t> lead_{};std::atomic<std::uint64_t> pdtGrid_{},pdtFrames_{},lateWakes_{},convertFailures_{},convertUnavailable_{};
    std::atomic<bool> pdtValid_{},pdtPaced_{};
    // cadence (XR thread; arrays under mutex_)
    std::int64_t lastPairPdt_{},lastGapP_{};unsigned skipPairs_{};std::uint64_t seenEpoch_{},modeEpoch_{};
    std::array<std::uint64_t,6> pairGapPeriods_{};std::uint64_t cadenceBreaks_{};bool cadencePdt_{};
    std::atomic<std::uint32_t> breakMask_{};std::atomic<std::uint64_t> levelEpoch_{};
    // per mode
    std::array<ModeCounters,byModeCount> byMode_{};
    // governor (render thread; governor_ under governorMutex_)
    mutable std::mutex governorMutex_;PacingGovernor governor_{};bool governorActive_{};
    std::atomic<unsigned> governorHz_{};std::atomic<bool> appSwAllowed_{};
    std::atomic<std::uint64_t> appSwOverBudget_{},appSwBlocks_{};std::uint64_t appSwTripsSeen_{};
    std::uint64_t gpuSerial_{},lastReturn_{},lastPace_{};                             // render thread
    std::atomic<std::uint64_t> gpuNs_{};std::atomic<std::uint32_t> gpuAge_{UINT32_MAX}; // render thread writes, report reads
    // ---- AppSW free cadence
    std::atomic<int> appSwFree_{3};std::atomic<bool> freeActive_{},cadenceRuntime_{};                         // property (XR thread) / render thread
    std::atomic<std::uint64_t> slowGrid_{},slowFrames_{},slowInits_{};std::atomic<std::int64_t> slowError_{},slowAbsEma_{}; // XR thread writes
    std::atomic<std::int64_t> cadenceNs_{},lockError_{};std::atomic<std::uint64_t> clampedSteps_{},slotSkipSleeps_{},anchorJumps_{},anchorJumpsHalfRate_{};
    std::array<HalfRateCounters,3> halfRate_{};
    std::uint64_t prevGrid_{};std::int64_t prevSpacing_{};                                                     // render thread
    Window busy_,lockErrorWin_;std::uint64_t frozenWindows_{};                                                 // under mutex_
    TestClock testNow_{};TestSleep testSleep_{};void* testCtx_{};                                              // host tests only
    AppSwSaturation sat_{};                                                                                    // under mutex_
    std::atomic<int> satForce_{};std::atomic<std::int64_t> satMarginNs_{1'000'000};std::atomic<bool> satActive_{}; // properties (XR thread) / render thread
    std::uint32_t overrunRun_{};bool satDirty_{};                                                              // render thread
    std::atomic<bool> satCpuExit_{true};std::atomic<std::uint64_t> throttleWait_{};                            // Property / last throttle wait
    std::uint64_t thrCum_{};bool thrSeen_{},waitedPrev_{};                                                     // render thread
    // ---- deadline-aware phase controller + late-latch budget
    static constexpr std::int64_t lockQuietNs=2'500'000;static constexpr unsigned lockLoudFrames=4;          // samples ignored while the lock is off
    std::atomic<bool> phaseCtl_{true},phaseActive_{},phaseRan_{};std::atomic<int> latchUs_{3000};               // properties (XR thread) / render thread
    std::atomic<std::int64_t> lastPdt_{};                                                                      // XR thread: this frame's pdt (0 = unknown)
    PhaseController phase_{};std::uint64_t phaseIgnored_{},phaseIgnoredLock_{};                               // under mutex_
    std::uint64_t deadlineCursor_{pairDeadlines.total()};unsigned dlPending_{},lockLoud_{};                    // render thread
    std::atomic<std::uint64_t> deadlineRead_{},deadlineLost_{};
    // ---- LOCKSTEP (debug.rdrvr.pacing.tight; host tests keep 0 - only the Android property read turns it on)
    std::mutex tightM_;std::condition_variable tightCv_;std::uint64_t tightSeq_{},tightNs_{};                // XR thread -> render thread
    std::uint64_t tightConsumed_{};                                                                            // render thread
    std::atomic<int> tight_{0};std::atomic<bool> tightActive_{};std::atomic<std::uint64_t> tightWaits_{},tightLate_{},tightTimeouts_{}; // 0 off, 1 AUTO, 2 always (Android property default: 2; host tests keep 0)
    bool tightPrev_{}; // Lockstep ran the previous paced frame (render thread)
    std::atomic<std::int64_t> tightGateNs_{2'800'000};std::int64_t tightCost_{};bool tightGate_{true};                 // AUTO gate
    std::atomic<std::int64_t> tightCostUs_{};std::atomic<bool> tightGateOpen_{true};std::atomic<std::uint64_t> tightOpens_{},tightCloses_{};
    std::atomic<bool> submitGate_{false};                                                                      // Android property: default on
    struct FlightRow {std::uint32_t t{},xr{},stereo{},newPairs{},repeats{},fps10{},gpuUs{},gpuMaxUs{},busyUs{},busyMaxUs{},overruns{},temp10{},mhz{};std::uint8_t synth{},tight{};
                      std::uint32_t maxMhz{},tpl{},cpuMaxMhz{},cpuCurMhz{},thStatus{},headroom100{};};
    // Android thermal API through dlsym (libandroid, API 30+; no link dependency). XR thread only.
    int thStatus_{-1},thHeadroom100_{-1};void* thMgr_{};bool thTried_{};
    using ThAcquire=void*(*)();using ThStatus=int(*)(void*);using ThHeadroom=float(*)(void*,int);ThStatus thGetStatus_{};ThHeadroom thGetHeadroom_{};
    void thermalSample() noexcept {
#if defined(__ANDROID__)
        if(!thTried_){thTried_=true;
            if(void* lib=dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL)){
                const auto acq=reinterpret_cast<ThAcquire>(dlsym(lib,"AThermal_acquireManager"));
                thGetStatus_=reinterpret_cast<ThStatus>(dlsym(lib,"AThermal_getCurrentThermalStatus"));
                thGetHeadroom_=reinterpret_cast<ThHeadroom>(dlsym(lib,"AThermal_getThermalHeadroom"));
                if(acq)thMgr_=acq();}}
        if(!thMgr_)return;
        if(thGetStatus_)thStatus_=thGetStatus_(thMgr_);
        if(thGetHeadroom_){const float h=thGetHeadroom_(thMgr_,0);if(h==h&&h>=0.f&&h<100.f)thHeadroom100_=int(h*100.f+0.5f);else thHeadroom100_=-1;}
#endif
    }
    static constexpr unsigned flightRows=1200;
    std::array<FlightRow,flightRows> flRing_{};unsigned flNext0_{},flCount_{};mutable std::mutex flMutex_;
    std::string flPath_;std::atomic<bool> flWriting_{};
    std::uint64_t flT0_{},flNext_{},flOverrunsSeen_{};std::uint32_t flXr_{},flStereo_{},flNew_{},flRep_{};                     // XR thread
    std::atomic<std::uint64_t> flBusySum_{},flBusyN_{},flBusyMax_{},flGpuSum_{},flGpuN_{},flGpuMax_{};                           // render -> XR
    static void flMax(std::atomic<std::uint64_t>& m,std::uint64_t v) noexcept {auto c=m.load(std::memory_order_relaxed);while(v>c&&!m.compare_exchange_weak(c,v,std::memory_order_relaxed)){}}
    static long long readSysLong(const char* path) noexcept {
#if defined(__ANDROID__)||defined(__linux__)
        const int saved=errno;std::FILE* fp=std::fopen(path,"r");long long v=0;if(fp){if(std::fscanf(fp,"%lld",&v)!=1)v=0;std::fclose(fp);}errno=saved;return v;
#else
        (void)path;return 0;
#endif
    }
    std::atomic<int> submitGateCapUs_{4000};                                                                   // debug.rdrvr.submitgate.capus
};
inline FramePacer framePacer;
} // namespace rdrvr::runtime
