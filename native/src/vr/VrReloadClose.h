#pragma once
// CYLINDER CLOSE: the pure, host-tested gesture detectors that close the swung-out revolver cylinder
// besides B - FLICK (a roll impulse about the barrel with a small net angle), WAVE (a velocity impulse
// of the open cylinder's centre along the closing tangent) and PUSH (the free hand pushes the open
// cylinder toward the frame from OUTSIDE; a per-hand automaton in the holding grip frame). Fed XR-tape
// samples (VrThrow.h TapeEntry, 152 B) by reload::Brain::frame on the input thread; the gates that need
// the reload state (row, dwell, events, hand busy, rigid, frame) are passed in per step. No engine, no
// allocation, no locks.
//
// Time: tau = TapeEntry::poseNs (pdtMono, the time the pose / velocity is predicted for); 0 -> publishedNs snapped to the display period
// (median of the last 16 publication intervals). Velocities: the runtime's XrSpaceVelocity when the tape has the bits (RUNTIME), else pose
// differences over tau (POSE); `.reload.velocity pose` forces POSE.
#include "VrThrow.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace rdrvr::vr::reload::close {
using V3=std::array<float,3>;using Q=std::array<float,4>; // Q = x y z w
inline V3 add(V3 a,V3 b) noexcept {return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline V3 sub(V3 a,V3 b) noexcept {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
inline V3 mul(V3 a,float s) noexcept {return {a[0]*s,a[1]*s,a[2]*s};}
inline float dot(V3 a,V3 b) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline V3 cross(V3 a,V3 b) noexcept {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline float len(V3 a) noexcept {return std::sqrt(dot(a,a));}
inline V3 unit(V3 a) noexcept {const float l=len(a);return l>1e-9f&&std::isfinite(l)?mul(a,1.f/l):V3{0,0,0};}
inline bool finite(V3 a) noexcept {return std::isfinite(a[0])&&std::isfinite(a[1])&&std::isfinite(a[2]);}
inline V3 rot(const Q& q,V3 v) noexcept {const V3 u{q[0],q[1],q[2]};const V3 t=mul(cross(u,v),2.f);return add(add(v,mul(t,q[3])),cross(u,t));}
inline Q conj(const Q& q) noexcept {return {-q[0],-q[1],-q[2],q[3]};}
inline Q qmul(const Q& a,const Q& b) noexcept {return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
    a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}
// World point x -> the frame of a pose (p, q): R^T (x - p).
inline V3 toFrame(V3 p,const Q& q,V3 x) noexcept {return rot(conj(q),sub(x,p));}
inline V3 fromFrame(V3 p,const Q& q,V3 x) noexcept {return add(p,rot(q,x));}
// Angular velocity (rad/s, world) that turns qa into qb in dt seconds: 2 log(qb qa^-1) / dt.
inline V3 angularVelocity(const Q& qa,const Q& qb,float dt) noexcept {
    Q d=qmul(qb,conj(qa));if(d[3]<0)for(float& x:d)x=-x;
    const float s=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);if(!(s>1e-9f)||!(dt>0))return {0,0,0};
    const float k=2.f*std::atan2(s,d[3])/s/dt;return {d[0]*k,d[1]*k,d[2]*k};
}
// Closest points of segments [a0,a1] and [b0,b1] (Ericson 5.1.9): out pa (on a), pb (on b).
inline void closestSegments(V3 a0,V3 a1,V3 b0,V3 b1,V3& pa,V3& pb) noexcept {
    const V3 d1=sub(a1,a0),d2=sub(b1,b0),r=sub(a0,b0);const float a=dot(d1,d1),e=dot(d2,d2),f=dot(d2,r);
    float s=0,t=0;
    if(a<=1e-12f&&e<=1e-12f){pa=a0;pb=b0;return;}
    if(a<=1e-12f){s=0;t=std::clamp(f/e,0.f,1.f);}
    else{const float c=dot(d1,r);
        if(e<=1e-12f){t=0;s=std::clamp(-c/a,0.f,1.f);}
        else{const float b=dot(d1,d2),den=a*e-b*b;
            if(den<=1e-4f*a*e){ // (nearly) parallel: the pair is not unique - take the middle of the overlap along b (a palm along the barrel)
                const float t0=dot(sub(a0,b0),d2)/e,t1=dot(sub(a1,b0),d2)/e;const float lo=std::max(0.f,std::min(t0,t1)),hi=std::min(1.f,std::max(t0,t1));
                if(lo<=hi){t=.5f*(lo+hi);pb=add(b0,mul(d2,t));const float u=std::clamp(dot(sub(pb,a0),d1)/a,0.f,1.f);pa=add(a0,mul(d1,u));return;}}
            s=den>1e-12f?std::clamp((b*f-c*e)/den,0.f,1.f):0.f;
            t=(b*s+f)/e;if(t<0){t=0;s=std::clamp(-c/a,0.f,1.f);}else if(t>1){t=1;s=std::clamp((b-c)/a,0.f,1.f);}}}
    pa=add(a0,mul(d1,s));pb=add(b0,mul(d2,t));
}

// ---- geometry: analytic FULL-OPEN constants in the holding grip frame (metres; the visual follow never moves them) ----
struct Geometry {
    bool valid{};
    V3 axis{0,0,-1};         // f: barrel forward (unit)
    V3 up{0,1,0};            // weapon up (unit)
    V3 openCentre{};         // O: centre of the fully swung-out cylinder
    V3 closedCentre{};       // K: centre of the closed cylinder
    V3 tangent{0,1,0};       // t: closing tangent of O (unit, the direction O moves while closing)
    V3 toFrame{1,0,0};       // d: unit(perp_f(K - O)) (from the open cylinder toward the frame)
    float radius{.0137f};    // chamber circle radius
    float half{.019f};       // half the cylinder length (|rear|)
    float swingSign{-1.f};   // the sign actually applied by the swing (side auto mirrors it for the left hand)
};
// ---- configuration (debug.rdrvr.vr.reload.* - parsed by VrReload.h startup) ----
struct Config {
    bool flick{true},wave{true},push{true};           // .reload.flick / .wave / .push 0 (property kills; the CYLINDER CLOSE row also gates)
    float flickRoll{7.f},flickRollUp{9.f},flickBig{12.f},flickNetDeg{50.f};std::uint64_t flickWindowNs{80'000'000};bool flickClose{};
    float waveClose{1.5f};int waveAxes{0};bool waveClose_{};float waveMuzzleDeg{25.f};   // waveAxes 0 tangent, 1 lateral, 2 perp; waveClose_ = waveDir close
    float pushCm{2.f},pushZoneCm{6.f},pushRCm{-1.f},pushHeelCm{4.f},pushTipsCm{11.f};std::uint64_t pushRefractoryNs{400'000'000};
    std::uint64_t closeDwellNs{250'000'000},slamNs{60'000'000};bool follow{true};float followMax{.6f};bool velocityPose{},shadow{};
    float ejectUpDeg{30.f}; // SHAKE eject angle (deg)
};
enum class Source : std::uint8_t {None,B,Flick,Wave,Push,AutoLoad,Snap,Count};
inline constexpr std::array<const char*,unsigned(Source::Count)> sourceNames{"NONE","B","FLICK","WAVE","PUSH","AUTOLOAD","SNAP"};
enum Refusal : unsigned {RefuseDwell,RefuseAfterEvent,RefuseHandBusy,RefuseMuzzleUp,RefuseNotRigid,RefuseNoFrame,RefuseBAfterGesture,RefuseBSpin,RefuseCount};
inline constexpr std::array<const char*,RefuseCount> refusalNames{"DWELL","AFTER_EVENT","HAND_BUSY","MUZZLE_UP","NOT_RIGID","NO_FRAME","B_AFTER_GESTURE","B_SPIN"};
enum PushState : std::uint8_t {PushOut,PushWait,PushArmed,PushLatch,PushBlocked};
enum PushBlock : unsigned {BlockHeld,BlockWrongSide,BlockSeat,BlockEject,BlockDrop,BlockGrab,BlockPluck,BlockOpen,BlockResume,BlockRoll,BlockCount};
inline constexpr std::array<const char*,BlockCount> pushBlockNames{"HELD","WRONG_SIDE","SEAT","EJECT","DROP","GRAB","PLUCK","OPEN","RESUME","ROLL"};

// Counters (report); *Max / *Last are per open (Detector::newOpen resets them).
struct Stats {
    std::uint64_t tapeSamples{},staleFrames{},resets{},resumes{},glitches{},angularOutliers{};
    std::uint64_t poseNsValid{},snapFallback{},runtimeSamples{},poseSamples{};std::array<std::uint64_t,2> velSamples{},velValid{};
    std::array<std::uint64_t,8> intervalHist{};std::array<std::uint64_t,8> stampErrHist{};std::uint64_t stampErrMaxUs{};
    float rollJMax{},rollJLast{},rollNetLast{};std::uint64_t rollNear{},rollNetRejected{},rollClose{},rollOpen{};
    float dvLast{};std::uint64_t waveNear{};float dvTangentClose{},dvTangentOpen{},dvLateral{},dvAxial{};
    std::uint64_t pushEntries{},pushWrongSide{},pushArmed{},pushLatched{},pushAborted{},pushGlitchRejected{},pushAxialSkipped{},pushTrackedGaps{},pushGapResets{},
        pushFollowFrames{};std::array<std::uint64_t,BlockCount> pushBlocked{};float pushMaxCm{},pushLastRelSpeed{},pushCtrlDistCm{};
    std::array<std::uint64_t,3> pushGripAtLatch{}; // OPEN (<= .3), HALF, FIST (>= .9)
    std::array<std::uint64_t,unsigned(Source::Count)> shadow{};
};
// The reload state the detectors are gated on (one per step; times in the input thread's now clock).
struct Gate {
    bool open{};                  // phase OPEN (not OPENING / CLOSING / CLOSED)
    bool flickRow{},waveRow{},pushRow{}; // the CYLINDER CLOSE row (+ the properties)
    bool frame{};                 // the ChamberFrame is fresh for this hand / weapon / space
    bool rigid{true};             // spin::rigid(hold)
    bool freeHeld{};              // the free hand holds a round / speedloader / casing (FLICK / WAVE: HAND_BUSY)
    bool freeFree{};              // the free hand is FREE (PUSH: empty, no phantom, no socket capture)
    std::uint64_t openedNs{},resumeNs{},lastEjectNs{},lastSeatNs{},lastGrabNs{},lastDropNs{},lastPluckNs{},now{};
    float freeGrip{};             // PUSH report: grip at the latch
    bool rollBlock{};             // The free hand is ROLLING the open cylinder (or did < 300 ms ago): no FLICK / WAVE / PUSH
};
struct Decision {Source fire{Source::None};bool refuseTick{};unsigned refusal{RefuseCount};float followV{1.f};bool rollBlocked{};}; // rollBlocked: a gesture fire discarded by Gate::rollBlock

// One detector per open revolver (the holding hand's tape).
class Detector {
public:
    Config cfg{};Stats stats{};
    // why = the PUSH block a hand already inside the zone gets on the first sample (BlockOpen at an open, BlockResume after a menu / gap).
    void reset(PushBlock why=BlockResume) noexcept {++stats.resets;clear(why);} // a history reset (resume, new space, lost frame, hold change)
    // the history only (not counted, the per-open maxima kept for the report): a close
    void clear(PushBlock why) noexcept {hist_={};n_=0;haveHold_=false;quietUntil_=0;cand_=Cand{};wave_={};wn_=0;push_=Push{};push_.fresh=true;push_.freshWhy=why;
        pubN_=0;lastTau_=0;lastPub_=0;theta_=0;}
    void newOpen() noexcept {clear(BlockOpen);stats.rollJMax=stats.rollJLast=stats.rollNetLast=0;stats.dvLast=0;stats.dvTangentClose=stats.dvTangentOpen=stats.dvLateral=stats.dvAxial=0;
        stats.pushMaxCm=0;pushTouched_=false;touch_=false;}
    // Every new tape entry (time order) while the cylinder is open; hold / free = the hand indices; g = the geometry; aimFwd = the free hand's
    // aim forward in its grip frame. Returns the decision of this sample (at most one close).
    Decision step(const throwing::TapeEntry& t,unsigned hold,const Geometry& g,V3 aimFwd,const Gate& gate) noexcept {
        Decision d{};++stats.tapeSamples;
        const unsigned free=1u-hold;
        if(!g.valid||!(t.valid>>hold&1u)){haveHold_=false;return d;}
        // ---- time base ----
        std::uint64_t tau=t.poseNs;
        if(tau){++stats.poseNsValid;if(lastPub_&&lastTau_){const std::uint64_t dp=t.ns>lastPub_?t.ns-lastPub_:0,dt=tau>lastTau_?tau-lastTau_:0;
                const std::uint64_t err=(dp>dt?dp-dt:dt-dp)/1000u;stats.stampErrMaxUs=std::max(stats.stampErrMaxUs,err);++stats.stampErrHist[histBucket(err,true)];}}
        else{++stats.snapFallback;tau=snapTime(t.ns);}
        if(lastTau_&&tau<=lastTau_){lastPub_=t.ns;return d;}
        const std::uint64_t dtNs=lastTau_?tau-lastTau_:0;lastPub_=t.ns;
        if(dtNs)++stats.intervalHist[histBucket(dtNs/1000u,false)];
        const Q q=t.q[hold];const V3 p=t.p[hold];
        const V3 f=unit(rot(q,g.axis)),upW=unit(rot(q,g.up)),tW=unit(rot(q,g.tangent));
        const float muzzle=f[1]; // sin(elevation): LOCAL +Y up
        const bool runtimeW=!cfg.velocityPose&&(t.valid&throwing::tapeAngular(hold)),runtimeV=!cfg.velocityPose&&(t.valid&throwing::tapeLinear(hold));
        ++stats.velSamples[hold];if(t.valid&throwing::tapeAngular(hold))++stats.velValid[hold];
        const bool interval=haveHold_&&dtNs>=4'000'000ull&&dtNs<=60'000'000ull;
        const float dt=float(double(dtNs)*1e-9);
        V3 w{},vC{};bool motion=false;
        if(runtimeW||interval){
            w=runtimeW?t.w[hold]:angularVelocity(prevQ_,q,dt);
            const V3 rO=rot(q,g.openCentre);const V3 c=add(p,rO);
            if(runtimeW&&runtimeV)vC=add(t.v[hold],cross(w,rO));else if(interval)vC=mul(sub(c,prevC_),1.f/dt);
            motion=interval||(runtimeW&&runtimeV);
            if(runtimeW&&runtimeV)++stats.runtimeSamples;else ++stats.poseSamples;
            prevC_=c;
        }else prevC_=add(p,rot(q,g.openCentre));
        if(haveHold_&&dtNs>60'000'000ull){hist_={};n_=0;wn_=0;cand_=Cand{};} // a gap restarts FLICK / WAVE (PUSH has its own gap rules)
        const float speed=len(interval?mul(sub(p,prevP_),1.f/dt):t.v[hold]);
        if(interval&&speed>20.f){++stats.glitches;hist_={};n_=0;wn_=0;cand_=Cand{};motion=false;}
        if(len(w)>70.f)++stats.angularOutliers;
        prevQ_=q;prevP_=p;lastTau_=tau;haveHold_=true;
        // While the free hand rolls the open cylinder (+300 ms) no close gesture
        // decides: the FLICK / WAVE history is dropped (no impulse carried past the block), PUSH keeps its tracking but is BLOCKED (ROLL) until the
        // hand leaves its zone.
        if(gate.rollBlock){
            hist_={};n_=0;wn_=0;cand_=Cand{};
            pushStep(t,hold,free,g,aimFwd,tau,gate,d);blockPush(BlockRoll);
            if(d.fire!=Source::None)d.rollBlocked=true;
            d.fire=Source::None;d.refuseTick=false;d.refusal=RefuseCount;d.followV=1.f;return d;}
        // ---- FLICK ----
        if(motion&&tau<quietUntil_){hist_={};n_=0;wn_=0;cand_=Cand{};motion=false;theta_+=0.f;} // the falling flank of a decided impulse
        if(motion){
            const float wk=dot(w,f);const float wbar=n_?.5f*(wk+hist_[(n_-1)%ring].w):wk;
            theta_+=wk*(interval?dt:0.f);
            hist_[n_%ring]={tau,wbar,theta_,wk};++n_;
            flickStep(tau,wbar,muzzle,g,gate,d);
            if(d.fire==Source::None)waveStep(tau,vC,f,upW,tW,muzzle,gate,d);
        }
        // ---- PUSH (positions only, holding grip frame) ----
        if(d.fire==Source::None)pushStep(t,hold,free,g,aimFwd,tau,gate,d);
        if(d.fire!=Source::None&&cfg.shadow){++stats.shadow[unsigned(d.fire)];d.fire=Source::None;}
        return d;
    }
    // Input frames without new tape (or with a stale tape): the candidate FLICK still decides by time.
    Decision tick(std::uint64_t,const Gate&) noexcept {return {};}
    PushState pushState() const noexcept {return push_.state;}
    float followV() const noexcept {return push_.followV;}
    // A reload event of the free hand (insert / eject / drop / grab / pluck) or an open / resume with the hand in the PUSH zone -> BLOCKED.
    void blockPush(PushBlock why) noexcept {if(push_.state!=PushOut&&push_.state!=PushBlocked){push_.state=PushBlocked;push_.block=why;++stats.pushBlocked[why];push_.push=0;push_.followV=1.f;}
        else if(push_.state==PushOut&&push_.inZone){push_.state=PushBlocked;push_.block=why;++stats.pushBlocked[why];}}
private:
    static constexpr unsigned ring=32;
    struct H {std::uint64_t tau{};float wbar{},theta{},w{};};
    std::array<H,ring> hist_{};unsigned n_{};bool haveHold_{};std::uint64_t quietUntil_{}; // after a FLICK / WAVE fire: one impulse = one decision
    struct Cand {bool on{};std::uint64_t ti{},tj{};float delta{},thr{};bool muzzleUp{};};
    Cand cand_{};
    struct W {std::uint64_t tau{};float s{};V3 v{};float sbar{};V3 vbar{};};
    std::array<W,ring> wave_{};unsigned wn_{};
    struct Push {PushState state{PushOut};PushBlock block{BlockHeld};bool inZone{},prevValid{},fresh{};PushBlock freshWhy{BlockOpen};float push{},side{},prevSide{},followV{1.f};
        V3 prevP{};std::uint64_t prevTau{},lastValidTau{};float latchPush{};};
    Push push_{};bool pushTouched_{};
    Q prevQ_{0,0,0,1};V3 prevP_{},prevC_{};std::uint64_t lastTau_{},lastPub_{};float theta_{};
    std::array<std::uint64_t,16> pubD_{};unsigned pubN_{};
    static unsigned histBucket(std::uint64_t us,bool err) noexcept {
        if(err){const std::uint64_t b[7]{250,500,1000,1500,2500,4000,8000};for(unsigned i=0;i<7;++i)if(us<b[i])return i;return 7;}
        const std::uint64_t b[7]{10000,12000,13000,14500,16000,20000,30000};for(unsigned i=0;i<7;++i)if(us<b[i])return i;return 7;}
    std::uint64_t snapTime(std::uint64_t pub) noexcept {
        if(!lastPub_||pub<=lastPub_)return pub;
        const std::uint64_t d=pub-lastPub_;pubD_[pubN_%16]=d;++pubN_;
        std::array<std::uint64_t,16> s=pubD_;const unsigned n=std::min(pubN_,16u);std::sort(s.begin(),s.begin()+n);const std::uint64_t P=s[n/2]?s[n/2]:d;
        const std::uint64_t k=std::max<std::uint64_t>(1,(d+P/2)/P);return lastTau_+k*P;
    }
    float thetaAt(std::uint64_t at) const noexcept { // theta of the newest sample with tau <= at (else the oldest kept)
        const unsigned m=std::min(n_,ring);float th=hist_[(n_-m)%ring].theta;
        for(unsigned i=0;i<m;++i){const auto& e=hist_[(n_-m+i)%ring];if(e.tau<=at)th=e.theta;else break;}return th;}
    bool common(const Gate& g,bool rowOn,Decision& d,unsigned& refusal) const noexcept {
        (void)d;if(!rowOn){refusal=RefuseCount;return false;} // ROW: not a refusal
        if(!g.frame){refusal=RefuseNoFrame;return false;}
        if(!g.rigid){refusal=RefuseNotRigid;return false;}
        const std::uint64_t base=std::max(g.openedNs,g.resumeNs);
        if(!g.open||g.now<base+cfg.closeDwellNs){refusal=RefuseDwell;return false;}
        return true;
    }
    void flickStep(std::uint64_t tau,float wbar,float muzzle,const Geometry& g,const Gate& gate,Decision& d) noexcept {
        const bool up=muzzle>=std::sin(cfg.ejectUpDeg*3.14159265f/180.f);
        const unsigned m=std::min(n_,ring);
        float best=0;std::uint64_t bestTi=0;
        for(unsigned i=0;i+1<m;++i){const auto& e=hist_[(n_-m+i)%ring];if(tau-e.tau>cfg.flickWindowNs)continue;
            const float delta=wbar-e.wbar;
            if(cfg.flickClose&&delta*g.swingSign<=0.f)continue; // sgnClose = +sign
            if(std::fabs(delta)>std::fabs(best)){best=delta;bestTi=e.tau;}}
        stats.rollJMax=std::max(stats.rollJMax,std::fabs(best));
        const float thrBase=cfg.flickRoll,thr=up?cfg.flickRollUp:cfg.flickRoll;
        if(std::fabs(best)>=cfg.flickBig){fireFlick(best,std::fabs(best),0.f,gate,g,d);n_=0;return;} // one impulse = one decision
        if(std::fabs(best)>=thrBase&&(!cand_.on||std::fabs(best)>std::fabs(cand_.delta))){cand_={true,bestTi,tau,best,thr,up};}
        else if(std::fabs(best)>=.7f*thrBase&&std::fabs(best)<thrBase)++stats.rollNear;
        if(cand_.on&&(tau>=cand_.tj+100'000'000ull||(tau>cand_.tj&&std::fabs(wbar)<=3.f))){
            const std::uint64_t to=std::min<std::uint64_t>(tau,cand_.tj+100'000'000ull),from=cand_.ti>150'000'000ull?cand_.ti-150'000'000ull:0;
            const float net=std::fabs(thetaAt(to)-thetaAt(from))*57.29578f;const Cand c=cand_;cand_=Cand{};n_=0; // decided: its falling flank is not a 2nd impulse
            if(net>cfg.flickNetDeg){++stats.rollNetRejected;stats.rollNetLast=net;return;}
            if(std::fabs(c.delta)<c.thr){ // muzzle up: the raised threshold is not met - a refusal tick (the gesture itself passed)
                unsigned r=RefuseCount;if(common(gate,gate.flickRow,d,r)){d.refuseTick=true;d.refusal=RefuseMuzzleUp;}return;}
            fireFlick(c.delta,std::fabs(c.delta),net,gate,g,d);
        }
    }
    void fireFlick(float delta,float j,float net,const Gate& gate,const Geometry& g,Decision& d) noexcept {
        cand_=Cand{};unsigned r=RefuseCount;
        if(!common(gate,gate.flickRow,d,r)){d.refusal=r;return;} // ROW / DWELL / NO_FRAME / NOT_RIGID: no tick
        if(gate.freeHeld){d.refuseTick=true;d.refusal=RefuseHandBusy;return;}
        if(gate.now<gate.lastEjectNs+300'000'000ull||gate.now<gate.lastSeatNs+250'000'000ull||gate.now<gate.lastGrabNs+250'000'000ull||
           gate.now<gate.lastDropNs+250'000'000ull){d.refuseTick=true;d.refusal=RefuseAfterEvent;return;}
        stats.rollJLast=j;stats.rollNetLast=net;(delta*g.swingSign>0.f?stats.rollClose:stats.rollOpen)++;d.fire=Source::Flick;quietUntil_=lastTau_+200'000'000ull;
    }
    void waveStep(std::uint64_t tau,V3 vC,V3 f,V3 upW,V3 tW,float muzzle,const Gate& gate,Decision& d) noexcept {
        W cur{};cur.tau=tau;cur.v=vC;
        const V3 lat=unit(cross(f,upW));
        cur.s=cfg.waveAxes==1?dot(vC,lat):dot(vC,tW);
        const W* prev=wn_?&wave_[(wn_-1)%ring]:nullptr;
        cur.sbar=prev?.5f*(cur.s+prev->s):cur.s;cur.vbar=prev?mul(add(cur.v,prev->v),.5f):cur.v;
        wave_[wn_%ring]=cur;++wn_;
        const unsigned m=std::min(wn_,ring);float D=0,dirD=0;
        for(unsigned i=0;i+1<m;++i){const auto& e=wave_[(wn_-m+i)%ring];if(tau-e.tau>cfg.flickWindowNs)continue;
            const V3 dv=sub(cur.vbar,e.vbar);const float dt_=dot(dv,tW);
            stats.dvTangentClose=std::max(stats.dvTangentClose,-dt_);stats.dvTangentOpen=std::max(stats.dvTangentOpen,dt_);
            stats.dvLateral=std::max(stats.dvLateral,std::fabs(dot(dv,lat)));stats.dvAxial=std::max(stats.dvAxial,std::fabs(dot(dv,f)));
            float x=0;
            if(cfg.waveAxes==2){const V3 perp=sub(dv,mul(f,dot(dv,f)));x=len(perp);}
            else x=cur.sbar-e.sbar;
            if(std::fabs(x)>std::fabs(D)){D=x;}
            if(x<dirD)dirD=x;}
        const float score=cfg.waveClose_?-dirD:std::fabs(D);
        if(score<cfg.waveClose){if(score>=.7f*cfg.waveClose)++stats.waveNear;return;}
        wn_=0; // one impulse = one decision
        unsigned r=RefuseCount;
        if(!common(gate,gate.waveRow,d,r)){d.refusal=r;return;}
        if(muzzle>=std::sin(cfg.waveMuzzleDeg*3.14159265f/180.f)){d.refuseTick=true;d.refusal=RefuseMuzzleUp;return;}
        if(gate.freeHeld){d.refuseTick=true;d.refusal=RefuseHandBusy;return;}
        if(gate.now<gate.lastEjectNs+300'000'000ull||gate.now<gate.lastSeatNs+250'000'000ull||gate.now<gate.lastGrabNs+250'000'000ull||
           gate.now<gate.lastDropNs+250'000'000ull){d.refuseTick=true;d.refusal=RefuseAfterEvent;return;}
        stats.dvLast=score;d.fire=Source::Wave;quietUntil_=tau+200'000'000ull;
    }
    void pushStep(const throwing::TapeEntry& t,unsigned hold,unsigned free,const Geometry& g,V3 aimFwd,std::uint64_t tau,const Gate& gate,Decision& d) noexcept {
        auto& P=push_;
        const float Rc=cfg.pushRCm>0.f?cfg.pushRCm*.01f:g.radius+.0065f,contactR=Rc+.015f;
        const float zoneIn=cfg.pushZoneCm*.01f,zoneOut=zoneIn+.015f;
        const bool rowOk=gate.pushRow&&gate.frame&&gate.rigid;
        if(!rowOk){if(P.state!=PushOut){P=Push{};}return;}
        if(!(t.valid>>free&1u)){ // a tracking gap of the free hand: no reset; > 100 ms -> the push restarts (state kept)
            ++stats.pushTrackedGaps;P.prevValid=false;return;}
        if(P.lastValidTau&&tau>P.lastValidTau+100'000'000ull&&P.push>0.f){P.push=0;P.followV=1.f;++stats.pushGapResets;}
        // the free hand's palm capsule (heel 4 cm behind, visible fingertips 11 cm ahead of the grip, along the measured aim forward)
        const V3 fwd=unit(rot(t.q[free],aimFwd));
        const V3 a=sub(t.p[free],mul(fwd,cfg.pushHeelCm*.01f)),b=add(t.p[free],mul(fwd,cfg.pushTipsCm*.01f));
        const V3 ha=toFrame(t.p[hold],t.q[hold],a),hb=toFrame(t.p[hold],t.q[hold],b);
        const V3 O=g.openCentre,f=g.axis,dd=g.toFrame;
        V3 pa{},pb{};closestSegments(ha,hb,sub(O,mul(f,g.half)),add(O,mul(f,g.half)),pa,pb);
        const float dist=len(sub(pa,pb)),ax=dot(sub(pa,O),f),side=dot(sub(pa,O),dd);
        const bool adjacent=P.prevValid&&tau>P.prevTau&&tau-P.prevTau<=100'000'000ull;
        const float sidePrev=P.prevValid?P.side:side;
        const V3 dp=adjacent?sub(pa,P.prevP):V3{0,0,0};
        const float vd=adjacent?dot(dp,dd)/std::max(1e-6f,float(double(tau-P.prevTau)*1e-9)):0.f;
        auto inside=[&](float r){return dist<=r;};
        if(P.fresh){P.fresh=false; // the cylinder opened / the input resumed with this hand already in the zone: blocked until it leaves
            if(inside(zoneIn)){P.state=PushBlocked;P.block=P.freshWhy;P.inZone=true;++stats.pushBlocked[P.freshWhy];}}
        switch(P.state){
        case PushOut:
            P.inZone=false;
            if(inside(zoneIn)){P.inZone=true;
                if(!gate.freeFree){P.state=PushBlocked;P.block=BlockHeld;++stats.pushBlocked[BlockHeld];break;}
                if(P.prevValid&&sidePrev>.01f&&side>.01f){P.state=PushBlocked;P.block=BlockWrongSide;++stats.pushWrongSide;++stats.pushBlocked[BlockWrongSide];break;}
                P.state=PushWait;++stats.pushEntries;
                if(!pushTouched_&&side<0.f){pushTouched_=true;touch_=true;}
            }
            break;
        case PushBlocked:
            if(dist>zoneOut||(side<=-(contactR+.015f)&&dist>=contactR+.015f)){P.state=PushOut;P.inZone=false;}
            break;
        case PushWait:
            if(!inside(zoneOut)){P.state=PushOut;P.inZone=false;break;}
            if(!gate.freeFree){P.state=PushBlocked;P.block=BlockHeld;++stats.pushBlocked[BlockHeld];break;}
            {const std::uint64_t ev=std::max({gate.lastSeatNs,gate.lastEjectNs,gate.lastDropNs,gate.lastGrabNs,gate.lastPluckNs});
             const bool refr=gate.now>=ev+cfg.pushRefractoryNs;
             const bool surface=side<=-.005f&&dist>=Rc-.005f&&ax>=-.015f&&ax<=.015f;
             const bool slap=P.prevValid&&adjacent&&sidePrev<=-.005f&&side-sidePrev>=.01f&&ax>=-.025f&&ax<=.025f;
             if(refr&&(surface||slap)){P.state=PushArmed;P.push=0;++stats.pushArmed;
                if(slap)P.push=std::max(0.f,std::max(side,-contactR)-std::max(sidePrev,-contactR));}}
            break;
        case PushArmed:case PushLatch:{
            if(!inside(zoneOut)){if(P.push>=.005f)++stats.pushAborted;P.state=PushOut;P.inZone=false;P.push=0;P.followV=1.f;break;}
            if(!gate.freeFree){P.state=PushBlocked;P.block=BlockHeld;++stats.pushBlocked[BlockHeld];P.push=0;P.followV=1.f;break;}
            if(!adjacent)break;
            if(P.state==PushLatch){ // the latch is confirmed by the next valid sample (a one-sample tracking jump is rejected)
                const float back=std::max(0.f,-(side-sidePrev));
                if(back<=.5f*P.latchPush){
                    stats.pushMaxCm=std::max(stats.pushMaxCm,P.push*100.f);stats.pushLastRelSpeed=vd;
                    stats.pushCtrlDistCm=len(sub(t.p[hold],t.p[free]))*100.f;
                    const float gp=gate.freeGrip;++stats.pushGripAtLatch[gp<=.3f?0:gp>=.9f?2:1];
                    unsigned r=RefuseCount;
                    if(common(gate,true,d,r)&&gate.freeFree){d.fire=Source::Push;d.followV=P.followV;++stats.pushLatched;}
                    P=Push{};break;}
                ++stats.pushGlitchRejected;P.push=std::max(0.f,P.push-back);P.state=PushArmed;break;}
            if(std::fabs(dot(dp,f))>std::fabs(dot(dp,dd))){++stats.pushAxialSkipped;break;} // along the barrel: the ejector rod / a poke
            if(ax>=-.025f&&ax<=.025f)P.push=std::max(0.f,P.push+(std::max(side,-contactR)-std::max(sidePrev,-contactR)));
            stats.pushMaxCm=std::max(stats.pushMaxCm,P.push*100.f);
            if(cfg.follow&&P.push>0.f){const float prog=std::min(P.push/(cfg.pushCm*.01f),1.f);P.followV=1.f-cfg.followMax*prog;++stats.pushFollowFrames;}
            else P.followV=1.f;
            if(P.push>=cfg.pushCm*.01f&&vd>=.05f){P.state=PushLatch;P.latchPush=P.push;}
            break;}
        }
        P.prevP=pa;P.side=side;P.prevSide=sidePrev;P.prevValid=true;P.prevTau=tau;P.lastValidTau=tau;
    }
public:
    bool touch_{}; // set when the free hand first touched from outside this open (the Brain pulses once and clears it)
};
} // namespace rdrvr::vr::reload::close
