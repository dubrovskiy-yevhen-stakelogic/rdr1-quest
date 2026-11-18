#pragma once
// HEAD SHOW: John's head comes back in the eyes whenever the VR camera is away from it.
//
// The first-person body hides the player's head in the eye draws (HEAD SHADOW: the collapsed rows swapped in by BodyDrawFilter; the
// fallback: the collapse in the skinning task). That rule (VrBody p.collapse) has no gate term, so in a knockdown the head stays hidden
// while the BODY-mode camera stays at eye height above the capsule (VrCamera: capsule + eye height, the ragdoll low-pass) and John's body
// lies below it without a head. The head joint is not the camera anchor, and hook A (the head lock) barely runs in a ragdoll
// (animPostPhysicsUpdateManager::UpdateActor 0x1d2a964 calls UpdateAttachmentsAndIK only when (actor+0x13c & 0xd) != 0).
// Rule: the distance between the eye (the midpoint of the plan's eye translations, moved by the camera anchor shift to the newest anchor so
// that it matches the newest skeleton row) and the head joint (the skeleton's global row, read by VrBody's tick for every gate) decides:
// shown after it stays above showM for showMs; hidden at once below insideM, after hideMs below hideM (default 0: at once, the eye is
// against the head mesh there: the upright eye sits 15.4 cm above and 11.7 cm in front of the joint, 18.3 cm away), after bandMs while it
// stays between hideM and showM (a camera that settled near the head is not away from it). The hat stays hidden while the head is shown
// closer than hatM (its crown sits above the skull, near the lens). A bad eye or a plan of another actor (cutscene John) holds the state;
// a stale head holds it for staleHideMs, then hides (the rule without HEAD SHOW). No state forces a show: a ragdoll alone never shows a
// head the camera is inside.
// Levers (read every 2 s): debug.rdrvr.vr.body.headshow 0 = off; .headshowcm "show hide inside [hat]" (default 40 28 22 60, needs
// show > hide > inside, hat > hide); .headshowms "show hide [band]" (default 100 0 1000). Report vr.body.headShow.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::headshow {

struct Config {bool on{true};float showM{.40f},hideM{.28f},insideM{.22f},hatM{.60f};
    std::uint64_t showNs{100'000'000ull},hideNs{0},bandNs{1'000'000'000ull},staleHideNs{800'000'000ull};};
inline constexpr std::uint64_t headMaxAgeNs=200'000'000ull;
// "show hide inside [hat]" in cm; anything out of order or out of 5..300 cm gives the defaults.
inline Config parseCm(Config c,const char* text) noexcept {
    const Config d{};if(!text||!*text)return c;
    float s{},h{},i{},t{};const int n=std::sscanf(text,"%f %f %f %f",&s,&h,&i,&t);if(n==3)t=d.hatM*100.f;
    if((n!=3&&n!=4)||!(s>h&&h>i&&i>=5.f&&s<=300.f&&t>h&&t<=300.f)){c.showM=d.showM;c.hideM=d.hideM;c.insideM=d.insideM;c.hatM=d.hatM;return c;}
    c.showM=s/100.f;c.hideM=h/100.f;c.insideM=i/100.f;c.hatM=t/100.f;return c;
}
// "show hide [band]" in ms.
inline Config parseMs(Config c,const char* text) noexcept {
    const Config d{};if(!text||!*text)return c;
    float s{},h{},b{};const int n=std::sscanf(text,"%f %f %f",&s,&h,&b);if(n==2)b=float(double(d.bandNs)/1e6);
    if((n!=2&&n!=3)||!(s>=0.f&&s<=2000.f&&h>=0.f&&h<=5000.f&&b>=0.f&&b<=10000.f)){c.showNs=d.showNs;c.hideNs=d.hideNs;c.bandNs=d.bandNs;return c;}
    c.showNs=std::uint64_t(double(s)*1e6);c.hideNs=std::uint64_t(double(h)*1e6);c.bandNs=std::uint64_t(double(b)*1e6);return c;
}

// The head joint published by VrBody's tick (main thread) for the render side: world translation of the head row, engine units.
enum GateClass : std::uint8_t {ClassOk,ClassRagdoll,ClassDead,ClassOther,ClassCount};
inline constexpr const char* classNames[ClassCount]{"ok","ragdoll","dead","other"};
struct HeadSample {float pos[3]{};std::uint64_t ns{};std::uint32_t guid{};std::uint8_t gateClass{ClassOther};};
class HeadBox {
    std::atomic<std::uint32_t> seq_{};std::atomic<float> x_{},y_{},z_{};std::atomic<std::uint64_t> ns_{};std::atomic<std::uint32_t> guid_{};std::atomic<std::uint8_t> cls_{};
public:
    void store(const HeadSample& s) noexcept {
        seq_.fetch_add(1,std::memory_order_acq_rel);
        x_.store(s.pos[0],std::memory_order_relaxed);y_.store(s.pos[1],std::memory_order_relaxed);z_.store(s.pos[2],std::memory_order_relaxed);
        ns_.store(s.ns,std::memory_order_relaxed);guid_.store(s.guid,std::memory_order_relaxed);cls_.store(s.gateClass,std::memory_order_relaxed);
        seq_.fetch_add(1,std::memory_order_release);
    }
    bool load(HeadSample& s) const noexcept {
        for(int k=0;k<4;++k){const auto a=seq_.load(std::memory_order_acquire);if(a&1u)continue;
            s.pos[0]=x_.load(std::memory_order_relaxed);s.pos[1]=y_.load(std::memory_order_relaxed);s.pos[2]=z_.load(std::memory_order_relaxed);
            s.ns=ns_.load(std::memory_order_relaxed);s.guid=guid_.load(std::memory_order_relaxed);s.gateClass=cls_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);if(seq_.load(std::memory_order_relaxed)==a)return s.ns!=0;}
        return false;
    }
};
inline HeadBox head{};

// ---- the state machine (pure) ----
// shiftM: the anchor shift already applied to eye (report only).
struct Input {std::uint64_t now{};bool eyeOk{};float eye[3]{};bool headOk{};HeadSample head{};float upm{1.f};std::uint32_t eyeGuid{};float shiftM{};};
enum Verdict : std::uint8_t {VerdictStep,VerdictStale,VerdictBadEye,VerdictOtherActor,VerdictOff};
struct Episode {std::uint64_t startNs{},endNs{};float maxM{};std::uint8_t gateStart{},gateEnd{};std::uint32_t guards{};};
struct Machine {
    bool shown{};std::uint64_t farSinceNs{},nearSinceNs{},bandSinceNs{},staleSinceNs{};float lastM{-1.f};
    std::uint64_t shows{},hides{},insideGuards{},bandHides{},staleHides{},staleHolds{},badEyeHolds{},otherActorHolds{},steps{},shownOkFrames{};float maxM{};
    float lastShiftM{},maxShiftM{};
    std::array<std::array<std::uint32_t,6>,ClassCount> hist{};
    std::array<Episode,8> ring{};unsigned next{};Episode cur{};
    static unsigned bucket(float m) noexcept {const float cm=m*100.f;return cm<20.f?0u:cm<28.f?1u:cm<40.f?2u:cm<60.f?3u:cm<100.f?4u:5u;}
    void endEpisode(std::uint64_t now,std::uint8_t cls) noexcept {cur.endNs=now?now:1;cur.gateEnd=cls;ring[next]=cur;next=(next+1u)%unsigned(ring.size());cur=Episode{};}
    void hide(std::uint64_t now,std::uint8_t cls) noexcept {shown=false;++hides;farSinceNs=nearSinceNs=bandSinceNs=0;endEpisode(now,cls);}
    // the head is shown and at least hatM away: the hat is drawn too
    bool hat(const Config& c) const noexcept {return shown&&lastM>=c.hatM;}
    Verdict step(const Input& in,const Config& c) noexcept {
        if(!c.on){if(shown)hide(in.now,in.head.gateClass);farSinceNs=nearSinceNs=bandSinceNs=0;return VerdictOff;}
        if(!in.eyeOk||!std::isfinite(in.eye[0]+in.eye[1]+in.eye[2])){++badEyeHolds;return VerdictBadEye;}
        if(!in.headOk||!in.head.ns||(in.now>in.head.ns&&in.now-in.head.ns>headMaxAgeNs)||!std::isfinite(in.head.pos[0]+in.head.pos[1]+in.head.pos[2])){
            ++staleHolds;if(!staleSinceNs)staleSinceNs=in.now?in.now:1;
            if(shown&&in.now>=staleSinceNs&&in.now-staleSinceNs>=c.staleHideNs){++staleHides;hide(in.now,ClassOther);}
            return VerdictStale;}
        staleSinceNs=0;
        if(!in.eyeGuid||in.eyeGuid!=in.head.guid){++otherActorHolds;return VerdictOtherActor;}
        const float u=in.upm>1e-3f&&std::isfinite(in.upm)?in.upm:1.f;
        const float dx=in.eye[0]-in.head.pos[0],dy=in.eye[1]-in.head.pos[1],dz=in.eye[2]-in.head.pos[2];
        const float m=std::sqrt(dx*dx+dy*dy+dz*dz)/u;if(!std::isfinite(m)){++badEyeHolds;return VerdictBadEye;}
        ++steps;lastM=m;maxM=std::max(maxM,m);
        if(std::isfinite(in.shiftM)){lastShiftM=in.shiftM;maxShiftM=std::max(maxShiftM,in.shiftM);}
        const unsigned cls=in.head.gateClass<ClassCount?in.head.gateClass:unsigned(ClassOther);++hist[cls][bucket(m)];
        const auto since=[&](std::uint64_t& t,std::uint64_t dwell){if(!t)t=in.now?in.now:1;return in.now>=t&&in.now-t>=dwell;};
        if(m<c.insideM){ // the camera is in the head again: hidden on this plan
            if(shown){++insideGuards;++cur.guards;hide(in.now,std::uint8_t(cls));}
            farSinceNs=nearSinceNs=bandSinceNs=0;return VerdictStep;}
        if(!shown){nearSinceNs=bandSinceNs=0;
            if(m>c.showM){if(since(farSinceNs,c.showNs)){shown=true;++shows;farSinceNs=0;cur=Episode{};cur.startNs=in.now?in.now:1;cur.gateStart=std::uint8_t(cls);}}
            else farSinceNs=0;}
        else{farSinceNs=0;cur.maxM=std::max(cur.maxM,m);
            if(m<c.hideM){bandSinceNs=0;if(since(nearSinceNs,c.hideNs))hide(in.now,std::uint8_t(cls));}
            else if(m<=c.showM){nearSinceNs=0;if(since(bandSinceNs,c.bandNs)){++bandHides;hide(in.now,std::uint8_t(cls));}}
            else nearSinceNs=bandSinceNs=0;}
        if(shown&&cls==ClassOk)++shownOkFrames;
        return VerdictStep;
    }
};

// ---- shared state ----
inline std::atomic<bool> shownNow{};     // the last verdict for the player's own plan (main thread: the fallback collapse)
inline std::atomic<bool> leverOn{true};
inline std::atomic<bool> seatOff{}; // VrBody tick: MOUNTED in a vehicle seat (player state 3) - no HEAD SHOW there
inline std::atomic<float> showM{.40f},hideM{.28f},insideM{.22f},hatM{.60f};
inline std::atomic<std::uint64_t> showNs{100'000'000ull},hideNs{0},bandNs{1'000'000'000ull};
inline Config config() noexcept {
    Config c{};c.on=leverOn.load(std::memory_order_relaxed);c.showM=showM.load(std::memory_order_relaxed);c.hideM=hideM.load(std::memory_order_relaxed);
    c.insideM=insideM.load(std::memory_order_relaxed);c.hatM=hatM.load(std::memory_order_relaxed);c.showNs=showNs.load(std::memory_order_relaxed);
    c.hideNs=hideNs.load(std::memory_order_relaxed);c.bandNs=bandNs.load(std::memory_order_relaxed);return c;}
inline void setConfig(const Config& c) noexcept {
    leverOn.store(c.on,std::memory_order_relaxed);showM.store(c.showM,std::memory_order_relaxed);hideM.store(c.hideM,std::memory_order_relaxed);
    insideM.store(c.insideM,std::memory_order_relaxed);hatM.store(c.hatM,std::memory_order_relaxed);showNs.store(c.showNs,std::memory_order_relaxed);
    hideNs.store(c.hideNs,std::memory_order_relaxed);bandNs.store(c.bandNs,std::memory_order_relaxed);}
// Main thread (VrBody tick), every 2 s.
inline void pollLevers(std::uint64_t now) noexcept {
    static std::uint64_t lastNs{};if(lastNs&&now>=lastNs&&now-lastNs<2'000'000'000ull)return;lastNs=now?now:1;
#if defined(__ANDROID__)
    Config c{};char v[PROP_VALUE_MAX]{};
    c.on=!(__system_property_get("debug.rdrvr.vr.body.headshow",v)>0&&v[0]=='0');
    char cm[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.vr.body.headshowcm",cm)>0)c=parseCm(c,cm);
    char ms[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.vr.body.headshowms",ms)>0)c=parseMs(c,ms);
    setConfig(c);
#endif
}

// Render side: one step per plan (key = scene id + head sample of the plan); a repeated key reuses the verdict. head = the head is shown
// for this plan (false for a plan of another actor whatever the held state); hat = the hat is drawn too (the head at least hatM away).
struct Show {bool head{},hat{};};
class Stepper {
    Machine m_{};std::uint64_t keyA_{},keyB_{};Show last_{};
public:
    // The verdict of a key already stepped (the caller then skips building the input).
    template<class Lock>
    bool cached(Lock& lock,std::uint64_t sceneId,std::uint64_t headSampledNs,Show& out) noexcept {
        std::lock_guard g(lock);if(sceneId==keyA_&&headSampledNs==keyB_&&(keyA_||keyB_)){out=last_;return true;}return false;}
    template<class Lock>
    Show step(Lock& lock,std::uint64_t sceneId,std::uint64_t headSampledNs,const Input& in) noexcept {
        std::lock_guard g(lock);
        if(sceneId==keyA_&&headSampledNs==keyB_&&(keyA_||keyB_))return last_;
        keyA_=sceneId;keyB_=headSampledNs;
        Config c=config();if(seatOff.load(std::memory_order_relaxed))c.on=false;m_.step(in,c);
        const bool mine=in.eyeGuid&&in.eyeGuid==in.head.guid;
        last_.head=m_.shown&&mine;last_.hat=last_.head&&m_.hat(c);
        if(mine)shownNow.store(m_.shown,std::memory_order_relaxed);
        else if(!m_.shown)shownNow.store(false,std::memory_order_relaxed);
        return last_;
    }
    template<class Lock>
    std::string report(Lock& lock,std::uint64_t now) noexcept {
        std::lock_guard g(lock);const auto& m=m_;const auto c=config();std::string o;char b[512];
        std::snprintf(b,sizeof b,"{\"enabled\":%s,\"cm\":[%.0f,%.0f,%.0f,%.0f],\"ms\":[%.0f,%.0f,%.0f],\"shown\":%s,\"hat\":%s,\"dCm\":%.1f,\"dCmMax\":%.1f,"
            "\"shiftCm\":%.1f,\"shiftCmMax\":%.1f,\"steps\":%llu,\"shows\":%llu,",
            c.on?"true":"false",double(c.showM*100.f),double(c.hideM*100.f),double(c.insideM*100.f),double(c.hatM*100.f),double(c.showNs)/1e6,double(c.hideNs)/1e6,
            double(c.bandNs)/1e6,m.shown?"true":"false",last_.hat?"true":"false",double(m.lastM*100.f),double(m.maxM*100.f),double(m.lastShiftM*100.f),
            double(m.maxShiftM*100.f),(unsigned long long)m.steps,(unsigned long long)m.shows);o+=b;
        std::snprintf(b,sizeof b,"\"hides\":%llu,\"insideGuards\":%llu,\"bandHides\":%llu,\"staleHides\":%llu,\"staleHolds\":%llu,\"badEyeHolds\":%llu,"
            "\"otherActorHolds\":%llu,\"shownWhileOkFrames\":%llu,\"hist\":{",(unsigned long long)m.hides,(unsigned long long)m.insideGuards,
            (unsigned long long)m.bandHides,(unsigned long long)m.staleHides,(unsigned long long)m.staleHolds,(unsigned long long)m.badEyeHolds,
            (unsigned long long)m.otherActorHolds,(unsigned long long)m.shownOkFrames);o+=b;
        for(unsigned k=0;k<ClassCount;++k){const auto& h=m.hist[k];
            std::snprintf(b,sizeof b,"%s\"%s\":[%u,%u,%u,%u,%u,%u]",k?",":"",classNames[k],h[0],h[1],h[2],h[3],h[4],h[5]);o+=b;}
        o+="},\"episodes\":[";bool first=true;
        for(unsigned k=0;k<m.ring.size();++k){const auto& e=m.ring[(m.next+k)%m.ring.size()];if(!e.startNs)continue;
            std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"durMs\":%.0f,\"maxCm\":%.1f,\"gateStart\":\"%s\",\"gateEnd\":\"%s\",\"guards\":%u}",first?"":",",
                now>=e.startNs?double(now-e.startNs)/1e6:-1.0,e.endNs>=e.startNs?double(e.endNs-e.startNs)/1e6:-1.0,double(e.maxM*100.f),
                classNames[e.gateStart<ClassCount?e.gateStart:3],classNames[e.gateEnd<ClassCount?e.gateEnd:3],e.guards);o+=b;first=false;}
        if(m.shown&&m.cur.startNs){std::snprintf(b,sizeof b,"%s{\"ageMs\":%.0f,\"durMs\":-1,\"maxCm\":%.1f,\"gateStart\":\"%s\",\"open\":true}",first?"":",",
            now>=m.cur.startNs?double(now-m.cur.startNs)/1e6:-1.0,double(m.cur.maxM*100.f),classNames[m.cur.gateStart<ClassCount?m.cur.gateStart:3]);o+=b;}
        o+="]}";return o;
    }
};
inline std::mutex stepLock;
inline Stepper stepper{};
inline std::string report(std::uint64_t now) noexcept {try{return stepper.report(stepLock,now);}catch(...){return "{}";}}
} // namespace rdrvr::vr::headshow
