#pragma once
// STEREO BRIDGE: a presentation change between two stereo views (World <-> a scripted gameplay shot, World <-> a stereo
// cutscene / camera transition) keeps the tracking state, the tracked views and the stereo mailbox. The XR thread keeps
// asking the mailbox, which may hold the displayed pair past its usual age limit, until a pair of the new presentation
// revision is displayed, the hold expires (then the theater) or the reference space changes. Menus, optics, plain scripted
// cameras, unknown contexts and any change after a frame without game stereo keep the full invalidation (theater).
// Also: the scripted-shot eligibility shared by the main, render and XR threads, the per-shot report ring and the XR layer
// kinds per presentation context (report vr.cutscene). Pure decisions + small atomics; no engine, no XR calls.
#include "Presentation.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
namespace rdrvr::runtime::bridge {
using Ctx=PresentationContext;
inline constexpr std::uint64_t nsPerMs=1'000'000;
inline constexpr unsigned defaultBridgeMs=300,maxBridgeMs=1000;
// debug.rdrvr.vr.cutscene.bridge <ms> (0 = no bridge: every change invalidates). Read by vr::cutscene::readOverrideLever.
inline std::atomic<unsigned> bridgeMs{defaultBridgeMs};
// Start of the current scripted shot (NativeCameraMode::Shot), 0 = none. Stored by NativeUiContext::update before the
// presentation publish, so a thread that sees the CameraOverride context also sees its start.
inline std::atomic<std::uint64_t> shotSinceNs{0};

// mode: 1 = every Override, 0 = none, else (2) scripted shots during their first maxS seconds (maxS <= 0: no limit).
constexpr bool overrideEligible(std::uint64_t since,std::uint64_t now,int maxS,int mode) noexcept {
    if(mode==1)return true;
    if(mode==0)return false;
    return since&&(maxS<=0||now<since||now-since<=std::uint64_t(maxS)*1'000'000'000ull);
}

// ---- XR thread: what a presentation change does ----
enum class Decline : std::uint8_t {None,NotStereoBefore,NotCandidate,SpaceChanged};
// prevGameStereo: the last XR frame showed a game stereo layer. menuBetween: a Menu revision was published after the revision
// the XR thread saw last (a short menu it never sampled must not show pre-menu pixels). spacePending: a recenter / space change
// is queued (it invalidates anyway).
constexpr Decline classify(bool prevGameStereo,Ctx next,bool shotEligible,bool cutsceneRequested,bool menuBetween=false,bool spacePending=false) noexcept {
    if(!prevGameStereo)return Decline::NotStereoBefore;
    const bool candidate=next==Ctx::World||(next==Ctx::CameraOverride&&shotEligible&&cutsceneRequested)||
        ((next==Ctx::Cutscene||next==Ctx::CameraTransition)&&cutsceneRequested);
    if(!candidate||menuBetween)return Decline::NotCandidate;
    if(spacePending)return Decline::SpaceChanged;
    return Decline::None;
}
constexpr bool bridgeOnChange(bool prevFrameGameStereo,Ctx newCtx,bool shotEligible,bool cutsceneRequested,bool menuBetween=false) noexcept {
    return classify(prevFrameGameStereo,newCtx,shotEligible,cutsceneRequested,menuBetween)==Decline::None;
}
// RetireContent = the existing optic path (held input preserved); Bridge = input invalidated, tracking / views / mailbox kept;
// Invalidate = everything (the theater until the new context qualifies).
enum class ChangeAction : std::uint8_t {RetireContent,Bridge,Invalidate};
constexpr ChangeAction changeAction(bool preservesInput,Decline d,unsigned ms) noexcept {
    return preservesInput?ChangeAction::RetireContent:(ms&&d==Decline::None)?ChangeAction::Bridge:ChangeAction::Invalidate;
}

// XR thread. A re-arm while armed (a second change) keeps the first start; the hold never exceeds twice the lever from it.
class Timer {
    std::uint64_t armedNs_{},untilNs_{},space_{};bool active_{};
public:
    enum class End : std::uint8_t {None,Engaged,Expired,SpaceChanged};
    void arm(std::uint64_t now,unsigned ms,std::uint64_t space) noexcept {
        const std::uint64_t span=std::uint64_t(ms)*nsPerMs;
        if(!active_||space!=space_){armedNs_=now;untilNs_=now+span;}
        else untilNs_=std::max(untilNs_,std::min(now+span,armedNs_+2*span));
        space_=space;active_=true;
    }
    void cancel() noexcept {active_=false;}
    bool armed() const noexcept {return active_;}
    bool active(std::uint64_t now) const noexcept {return active_&&now<untilNs_;}
    std::uint64_t armedNs() const noexcept {return armedNs_;}
    // After this frame's layer: stereoGate = World / an engaged stereo cutscene; pairOfRevision = the displayed pair was rendered
    // in the current presentation revision.
    End update(std::uint64_t now,std::uint64_t space,bool stereoGate,bool pairOfRevision) noexcept {
        if(!active_)return End::None;
        if(space!=space_){active_=false;return End::SpaceChanged;}
        if(stereoGate&&pairOfRevision){active_=false;return End::Engaged;}
        if(now>=untilNs_){active_=false;return End::Expired;}
        return End::None;
    }
};

// ---- StereoMailbox::render: a published pair older than maxAge ----
// Fresh = the normal latch; Stale = the stale-resubmit repeat (ADAPTIVE); Hold = the bridge repeat of the displayed pair;
// Reject = no layer. Stale / Hold need the displayed pair valid in the current tracking and reference-space epochs.
enum class Age : std::uint8_t {Fresh,Stale,Hold,Reject};
constexpr Age ageVerdict(std::uint64_t age,std::uint64_t maxAge,bool staleEnabled,bool bridging,bool screenValid,bool displayedEpochsMatch) noexcept {
    if(age<=maxAge)return Age::Fresh;
    if(!screenValid||!displayedEpochsMatch)return Age::Reject;
    return staleEnabled?Age::Stale:bridging?Age::Hold:Age::Reject;
}

// Render thread (a recorded stereo pair) -> XR thread: the first renderFrameId recorded in a presentation revision.
// Writer: store the scene, then the revision (release); a torn read only delays the bridge end.
struct RevisionPairs {
    std::atomic<std::uint64_t> revision{},firstScene{};
    void note(std::uint64_t rev,std::uint64_t scene) noexcept {
        if(!rev||!scene||revision.load(std::memory_order_relaxed)==rev)return;
        firstScene.store(scene,std::memory_order_relaxed);revision.store(rev,std::memory_order_release);
    }
    bool reached(std::uint64_t rev,std::uint64_t pair) const noexcept {
        return rev&&pair&&revision.load(std::memory_order_acquire)==rev&&pair>=firstScene.load(std::memory_order_relaxed);
    }
};
inline RevisionPairs revisionPairs;

// ---- report counters ----
struct Stats {
    std::atomic<std::uint64_t> arms{},armsEnter{},armsExit{},declinedNotStereo{},declinedNotCandidate{},declinedSpace{},
        heldFrames{},ended{},expired{},cancelled{},headHiddenDraws{},headFillDraws{};
    std::atomic<std::uint32_t> lastMs{},maxSeenMs{};
};
inline Stats stats;
inline void bump(std::atomic<std::uint64_t>& c,std::uint64_t k=1) noexcept {c.fetch_add(k,std::memory_order_relaxed);}
inline void noteDecline(Decline d) noexcept {
    if(d==Decline::NotStereoBefore)bump(stats.declinedNotStereo);
    else if(d==Decline::NotCandidate)bump(stats.declinedNotCandidate);
    else if(d==Decline::SpaceChanged)bump(stats.declinedSpace);
}
inline void noteEnd(Timer::End e,std::uint64_t now,std::uint64_t armedNs) noexcept {
    if(e==Timer::End::None)return;
    if(e==Timer::End::Engaged)bump(stats.ended);else if(e==Timer::End::Expired)bump(stats.expired);else bump(stats.cancelled);
    const auto ms=std::uint32_t(std::min<std::uint64_t>(now>armedNs?(now-armedNs)/nsPerMs:0,0xffffffffu));
    stats.lastMs.store(ms,std::memory_order_relaxed);
    if(ms>stats.maxSeenMs.load(std::memory_order_relaxed))stats.maxSeenMs.store(ms,std::memory_order_relaxed);
}
// XR frames per presentation context (PresentationContext order) x layer kind (FramePacing.h LayerKind order:
// None, StereoNew, StereoRepeat, StereoStale, Theater, Diagnostic). XR thread only.
inline constexpr unsigned ctxCount=7,kindCount=6;
inline std::array<std::array<std::atomic<std::uint64_t>,kindCount>,ctxCount> xrKind{};
inline void noteXrKind(Ctx c,unsigned kind) noexcept {const auto i=unsigned(c);if(i<ctxCount&&kind<kindCount)bump(xrKind[i][kind]);}

// Scripted camera runs (CameraOverride), the last 8 + the current one. The main thread starts / ends a run around the publish
// of the context; render-thread counters (frames, rejects, head draws) and XR-thread counters (layers, edge gaps) go to the
// current run while its context lasts and for windowNs after it ended (the exit edge).
// ExitHeadFill / ExitHeadMiss: gameplay eye-scope draws of the player after the run ended (its exit window): the head-collapsed
// copy bound or taken as is (head hidden again) / no captured head yet, the whole player record hidden for that draw (fail closed).
enum Counter : unsigned {Eligible,Composed,Stereo,XrNew,XrRepeat,XrHeld,XrTheater,XrNone,RejStereoCull,RejFrozen,RejCullSnap,HeadHidden,HeadFill,
    ExitHeadFill,ExitHeadMiss,CounterCount};
struct Row {std::uint64_t startNs{},endNs{};const char* cls{""};unsigned mode{};std::array<std::uint32_t,CounterCount> n{};std::int32_t enterGapMs{-1},exitGapMs{-1};};
class ShotRuns {
    std::atomic<std::uint64_t> startNs_{},endNs_{};std::atomic<const char*> cls_{""};std::atomic<unsigned> mode_{};
    std::array<std::atomic<std::uint32_t>,CounterCount> n_{};std::atomic<std::int32_t> enterGap_{-1},exitGap_{-1};
    mutable std::mutex mutex_;std::array<Row,8> ring_{};std::uint64_t total_{};
    Row live() const noexcept {
        Row r{};r.startNs=startNs_.load(std::memory_order_acquire);r.endNs=endNs_.load(std::memory_order_relaxed);r.cls=cls_.load(std::memory_order_relaxed);
        r.mode=mode_.load(std::memory_order_relaxed);for(unsigned i=0;i<CounterCount;++i)r.n[i]=n_[i].load(std::memory_order_relaxed);
        r.enterGapMs=enterGap_.load(std::memory_order_relaxed);r.exitGapMs=exitGap_.load(std::memory_order_relaxed);return r;
    }
public:
    static constexpr std::uint64_t windowNs=1'000'000'000;
    // Main thread, entering CameraOverride: the previous run goes to the ring.
    void begin(std::uint64_t now,const char* cls) noexcept {
        try{
            std::lock_guard lock(mutex_);
            if(startNs_.load(std::memory_order_relaxed)&&!endNs_.load(std::memory_order_relaxed))return; // still running (a repeated begin)
            if(startNs_.load(std::memory_order_relaxed)){ring_[total_%ring_.size()]=live();++total_;}
            startNs_.store(0,std::memory_order_relaxed);endNs_.store(0,std::memory_order_relaxed);
            for(auto& c:n_)c.store(0,std::memory_order_relaxed);
            enterGap_.store(-1,std::memory_order_relaxed);exitGap_.store(-1,std::memory_order_relaxed);mode_.store(0,std::memory_order_relaxed);
            cls_.store(cls?cls:"",std::memory_order_relaxed);startNs_.store(now?now:1,std::memory_order_release);
        }catch(...){}
    }
    void end(std::uint64_t now) noexcept {if(startNs_.load(std::memory_order_relaxed)&&!endNs_.load(std::memory_order_relaxed))endNs_.store(now?now:1,std::memory_order_relaxed);}
    // After the run ended, inside its exit window (the first gameplay frames after the shot).
    bool exitWindow(std::uint64_t now) const noexcept {
        if(!startNs_.load(std::memory_order_acquire))return false;
        const auto e=endNs_.load(std::memory_order_relaxed);return e&&now>=e&&now-e<windowNs;
    }
    bool ended() const noexcept {return startNs_.load(std::memory_order_relaxed)&&endNs_.load(std::memory_order_relaxed);}
    bool inWindow(std::uint64_t now) const noexcept {
        if(!startNs_.load(std::memory_order_acquire))return false;
        const auto e=endNs_.load(std::memory_order_relaxed);return !e||now<e||now-e<windowNs;
    }
    void bump(Counter c,std::uint32_t k=1) noexcept {if(c<CounterCount)n_[c].fetch_add(k,std::memory_order_relaxed);}
    void bumpIn(Counter c,std::uint64_t now,std::uint32_t k=1) noexcept {if(k&&inWindow(now))bump(c,k);}
    void setMode(unsigned m) noexcept {mode_.store(m,std::memory_order_relaxed);}
    void setEnterGap(std::int32_t ms) noexcept {enterGap_.store(ms,std::memory_order_relaxed);}
    void setExitGap(std::int32_t ms) noexcept {exitGap_.store(ms,std::memory_order_relaxed);}
    // Render thread, every world-pass reject (literal reason).
    void noteReject(const char* why,std::uint64_t now) noexcept {
        if(!why||!inWindow(now))return;
        if(!std::strcmp(why,"WAITING_FOR_STEREO_CULL"))bump(RejStereoCull);
        else if(!std::strcmp(why,"FROZEN_TRACKING_EXPIRED"))bump(RejFrozen);
        else if(!std::strcmp(why,"CULL_SNAPSHOT_UNAVAILABLE"))bump(RejCullSnap);
    }
    Row current() const noexcept {return live();}
    std::string report(std::uint64_t now) const {
        std::array<Row,8> ring{};std::uint64_t total{};Row cur{};
        {std::lock_guard lock(mutex_);ring=ring_;total=total_;cur=live();}
        static constexpr const char* modes[]{"SCREEN","CINEMATIC","FIRST_PERSON"};
        std::string o="{\"total\":"+std::to_string(total+(cur.startNs?1:0))+
            ",\"columns\":\"ageMs,durMs,class,modeUsed,eligible,composed,stereo,xrNew,xrRepeat,xrHeld,xrTheater,xrNone,enterGapMs,exitGapMs,rejStereoCull,rejFrozen,rejCullSnap,headHiddenDraws,headFillDraws,exitHeadFills,exitHeadMisses\",\"rows\":[";
        bool first=true;
        const auto row=[&](const Row& r){
            if(!r.startNs)return;
            char b[400];const long long age=now>=r.startNs?(long long)((now-r.startNs)/nsPerMs):-1,dur=r.endNs>=r.startNs&&r.endNs?(long long)((r.endNs-r.startNs)/nsPerMs):-1;
            std::snprintf(b,sizeof b,"%s[%lld,%lld,\"%s\",\"%s\",%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%u,%u,%u,%u,%u,%u,%u]",first?"":",",age,dur,r.cls?r.cls:"",modes[r.mode<3?r.mode:0],
                r.n[Eligible],r.n[Composed],r.n[Stereo],r.n[XrNew],r.n[XrRepeat],r.n[XrHeld],r.n[XrTheater],r.n[XrNone],r.enterGapMs,r.exitGapMs,
                r.n[RejStereoCull],r.n[RejFrozen],r.n[RejCullSnap],r.n[HeadHidden],r.n[HeadFill],r.n[ExitHeadFill],r.n[ExitHeadMiss]);
            o+=b;first=false;};
        const std::uint64_t kept=std::min<std::uint64_t>(total,ring.size());
        for(std::uint64_t k=total-kept;k<total;++k)row(ring[k%ring.size()]);
        row(cur);
        return o+"]}";
    }
};
inline ShotRuns shots;
// Render thread: eye-scope player-record hides and HEAD SHADOW fills while the plan is a cutscene CINEMATIC plan (expected 0:
// the side view draws John with his head).
inline void noteHeadDraws(std::uint64_t hidden,std::uint64_t fills,std::uint64_t now) noexcept {
    if(hidden){bump(stats.headHiddenDraws,hidden);shots.bumpIn(HeadHidden,now,std::uint32_t(std::min<std::uint64_t>(hidden,0xffffffffu)));}
    if(fills){bump(stats.headFillDraws,fills);shots.bumpIn(HeadFill,now,std::uint32_t(std::min<std::uint64_t>(fills,0xffffffffu)));}
}

// Render thread: a gameplay eye scope inside the exit window of the last run (fills = head copies bound or taken as is, misses =
// player record hidden because no head copy was captured yet).
inline void noteExitHead(std::uint64_t fills,std::uint64_t misses,std::uint64_t now) noexcept {
    if(!shots.exitWindow(now))return;
    if(fills)shots.bump(ExitHeadFill,std::uint32_t(std::min<std::uint64_t>(fills,0xffffffffu)));
    if(misses)shots.bump(ExitHeadMiss,std::uint32_t(std::min<std::uint64_t>(misses,0xffffffffu)));
}

// XR thread: the gap between the last new pair before a CameraOverride edge and the first new pair of the new revision after
// it, and the layer kinds of the run (its context, then the exit edge until that first pair or windowNs).
class EdgeMeter {
    std::uint64_t lastNewNs_{},enterBase_{},exitBase_{},exitSince_{};bool enter_{},exit_{};
    static std::int32_t gap(std::uint64_t base,std::uint64_t now) noexcept {
        return base&&now>=base?std::int32_t(std::min<std::uint64_t>((now-base)/nsPerMs,0x7fffffff)):-1;}
public:
    void change(Ctx prev,Ctx next,std::uint64_t now) noexcept {
        if(next==Ctx::CameraOverride&&prev!=Ctx::CameraOverride){enter_=true;enterBase_=lastNewNs_;exit_=false;}
        else if(prev==Ctx::CameraOverride&&next!=Ctx::CameraOverride){exit_=true;exitBase_=lastNewNs_;exitSince_=now;enter_=false;}
    }
    void frame(std::uint64_t now,unsigned kind,bool pairOfRevision,Ctx ctx,ShotRuns& runs) noexcept {
        if(ctx==Ctx::CameraOverride||exit_)
            runs.bump(kind==1?XrNew:kind==2?XrRepeat:kind==3?XrHeld:kind==4?XrTheater:XrNone);
        if(kind==1){
            if(pairOfRevision){
                if(enter_){runs.setEnterGap(gap(enterBase_,now));enter_=false;}
                if(exit_){runs.setExitGap(gap(exitBase_,now));exit_=false;}
            }
            lastNewNs_=now;
        }
        if(exit_&&now>=exitSince_&&now-exitSince_>=ShotRuns::windowNs)exit_=false;
    }
    bool exitOpen() const noexcept {return exit_;}
};

inline std::string report(std::uint64_t now) {
    char b[640];
    std::snprintf(b,sizeof b,"\"bridge\":{\"lever\":%u,\"maxMs\":%u,\"arms\":%llu,\"armsEnter\":%llu,\"armsExit\":%llu,"
        "\"declined\":{\"notStereoBefore\":%llu,\"notCandidate\":%llu,\"spaceChanged\":%llu},\"heldFrames\":%llu,\"ended\":%llu,\"expired\":%llu,"
        "\"cancelled\":%llu,\"lastMs\":%u,\"maxSeenMs\":%u},\"headHiddenDraws\":%llu,\"headFillDraws\":%llu,",
        bridgeMs.load(std::memory_order_relaxed),2*bridgeMs.load(std::memory_order_relaxed),
        (unsigned long long)stats.arms.load(),(unsigned long long)stats.armsEnter.load(),(unsigned long long)stats.armsExit.load(),
        (unsigned long long)stats.declinedNotStereo.load(),(unsigned long long)stats.declinedNotCandidate.load(),(unsigned long long)stats.declinedSpace.load(),
        (unsigned long long)stats.heldFrames.load(),(unsigned long long)stats.ended.load(),(unsigned long long)stats.expired.load(),
        (unsigned long long)stats.cancelled.load(),unsigned(stats.lastMs.load()),unsigned(stats.maxSeenMs.load()),
        (unsigned long long)stats.headHiddenDraws.load(),(unsigned long long)stats.headFillDraws.load());
    std::string o=b;
    static constexpr const char* ctxNames[ctxCount]{"UNKNOWN","WORLD","MENU","CUTSCENE","CAMERA_OVERRIDE","CAMERA_TRANSITION","OPTIC"};
    static constexpr const char* kindNames[kindCount]{"NONE","NEW","REPEAT","HELD","THEATER","DIAGNOSTIC"};
    o+="\"xrKindByCtx\":{";bool firstCtx=true;
    for(unsigned c=0;c<ctxCount;++c){
        std::uint64_t any=0;for(unsigned k=0;k<kindCount;++k)any+=xrKind[c][k].load(std::memory_order_relaxed);
        if(!any)continue;
        o+=firstCtx?"\"":",\"";o+=ctxNames[c];o+="\":{";firstCtx=false;
        for(unsigned k=0;k<kindCount;++k){o+=k?",\"":"\"";o+=kindNames[k];o+="\":"+std::to_string(xrKind[c][k].load(std::memory_order_relaxed));}
        o+="}";
    }
    o+="},\"shots\":"+shots.report(now);
    return o;
}
} // namespace rdrvr::runtime::bridge
