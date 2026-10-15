#pragma once
// CUTSCENES: cutscenes in stereo instead of the flat theater quad (VC / SA VR "stereo cutscenes").
// Menu MOVEMENT > CUTSCENES: SCREEN (the 2D theater) / CINEMATIC (default) / FIRST PERSON.
//
//   CINEMATIC eye = cutscene camera position, world heading = the cutscene camera's horizontal
//   heading (pitch / roll dropped by prepareHeadCamera), the HMD (rotation + position) on top.
//   Camera cuts (position jump / view-direction jump in one frame) re-latch the HMD yaw + position
//   origin so the player faces the new shot from its camera point; smooth moves are followed.
//   FIRST PERSON eye = John's eye pivots (head bone fallback), heading = John's facing latched at
//   cutscene start (re-latched at a cut only when John turned > 45 deg / moved > 2 m), HMD on top.
//   The player actor must be grabbed by the cutscene (actor+0x112 bits 1/2,
//   rdr2scripting::actor::ActorIsGrabbedByCutscene) and not hidden by it (bit 3); else CINEMATIC
//   (fallback). John's head is hidden by the gameplay BODY path (eye BodyDrawFilter: head-collapse
//   fill, or the whole body when BODY is off).
//
// Composition: VisibilityCameraBridge::begin -> prepareHeadCamera keeps the STOCK camera's heading
// thetaS; the anchor's TrackingOrigin (position + inverseYaw) is set by this module:
// inverseYaw = R_y(phi - a0 - thetaS) gives the effective world yaw phi - a0 for any target phi
// (phi = thetaS CINEMATIC, the latched John heading FIRST PERSON; a0 = HMD yaw at the latch), offsets rotate with it (CameraAdapter).
// Threads: noteMain()/publishJohn() = engine main thread (PostCameraUpdate reticle hook); Composer::step = render thread
// (CreateSceneVisibility -> gameBeginVisibility); engaged() / report() any thread. Header-only, no engine reads here (GameXr reads).
// Kill switch debug.rdrvr.vr.cutscene 0 (row PROP, SCREEN). Fail closed: anything missing ->
// no anchor -> the theater.
#include "VrSettings.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <mutex>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::cutscene {
using V3=std::array<float,3>;
enum Mode : unsigned {Screen=0,Cinematic=1,FirstPerson=2};
enum class Ctx : unsigned {Other,World,Cutscene,Transition,Override};
// debug.rdrvr.vr.cutscene.override 1 (live, GameXr re-reads it): scripted CameraOverride cameras are cinematic too (story scenes
// that RDR runs as scripted cameras instead of the cutscene manager). Default 0 = cutscene manager only.
inline std::atomic<bool> overrideLever{false};
inline void readOverrideLever() noexcept {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX]{};const int n=__system_property_get("debug.rdrvr.vr.cutscene.override",v);
    overrideLever.store(n>0&&v[0]=='1',std::memory_order_relaxed);
#endif
}
// Diagnostics: visibility calls per runtime::PresentationContext (Unknown, World, Menu, Cutscene, CameraOverride, CameraTransition,
// Optic) and the last 16 context changes seen there (context, ms since the first call) - which context RDR reports in a story scene.
inline std::array<std::atomic<std::uint64_t>,8> ctxVisibility{};
inline std::array<std::atomic<std::uint32_t>,16> ctxChange{};inline std::atomic<std::uint32_t> ctxChanges{0};
inline std::uint64_t ctxFirstNs{};inline unsigned ctxLast{~0u}; // render thread only
inline void noteContext(unsigned c,std::uint64_t ns) noexcept {
    if(c<8)ctxVisibility[c].fetch_add(1,std::memory_order_relaxed);
    if(!ctxFirstNs)ctxFirstNs=ns;
    if(c!=ctxLast){ctxLast=c;const auto ms=std::uint32_t(std::min<std::uint64_t>((ns-ctxFirstNs)/1000000u,0x0fffffffu));
        const auto i=ctxChanges.fetch_add(1,std::memory_order_relaxed);ctxChange[i&15u].store((c&15u)<<28|ms,std::memory_order_relaxed);}
}
inline constexpr std::uint32_t guidCinematic=0xC5C50001u,guidFirstPerson=0xC5C50002u; // anchor actorGuid of cutscene plans (never a player)
inline constexpr std::uint64_t originSerialBit=std::uint64_t(1)<<62;                 // TrackingOrigin serials (the rig counts from 1)
inline constexpr std::uint64_t johnMaxAgeNs=150'000'000,johnHoldNs=500'000'000,engagedMaxAgeNs=250'000'000; // hold: last good John bridges short gaps
inline constexpr float cutMetres=2.f,cutMetresPerS=25.f,cutDeg=30.f,cutDegPerS=90.f,fpRelatchDeg=45.f,fpRelatchMetres=2.f,fpMaxCameraMetres=60.f;
inline constexpr float kPi=3.14159265358979f;
inline float wrapPi(float a) noexcept {while(a>kPi)a-=2*kPi;while(a<-kPi)a+=2*kPi;return a;}
inline bool finite3(const V3& v) noexcept {return std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]);}
inline float dist(const V3& a,const V3& b) noexcept {const float x=a[0]-b[0],y=a[1]-b[1],z=a[2]-b[2];return std::sqrt(x*x+y*y+z*z);}
// R_y convention of CameraAdapter / prepareHeadCamera: R_y(t)*(0,0,1) = (sin t, 0, cos t); a camera with world column c ("back") has
// heading atan2(c.x, c.z); a FORWARD vector f has heading atan2(-f.x, -f.z).
inline bool headingOfForward(float fx,float fz,float& out) noexcept {if(!(fx*fx+fz*fz>1e-4f)||!std::isfinite(fx+fz))return false;out=std::atan2(-fx,-fz);return true;}

inline bool requested() noexcept {return enabled(Feature::Cutscene)&&settings().cutsceneMode()!=Screen;}
inline const char* modeName(unsigned m) noexcept {return m==Cinematic?"CINEMATIC":m==FirstPerson?"FIRST_PERSON":"SCREEN";}

// ---- main thread -> render thread: John (the player actor) for FIRST PERSON ----
struct John {
    std::uint64_t ns{},revision{};bool valid{};V3 eye{},head{};float heading{};bool headingValid{},eyePivots{};
    std::uint32_t guid{};std::uintptr_t creature{},hat{};const char* why{"NONE"};
};
inline std::mutex johnMutex;inline John johnLatest{};
inline void publishJohn(const John& j) noexcept {std::lock_guard lock(johnMutex);johnLatest=j;}
inline John johnSnapshot() noexcept {std::lock_guard lock(johnMutex);return johnLatest;}
// main thread: presentation context of this PostCameraUpdate (VrBody arms gate / head-collapse suppression read these)
inline std::atomic<bool> inCutscene{false},suppressCollapse{false};

// ---- engaged state (render thread writes after a successful anchored begin; every stereo gate reads) ----
inline std::atomic<std::uint64_t> engagedRevision{0},engagedNs{0};
inline std::atomic<unsigned> engagedMode{Screen};
inline bool engaged(std::uint64_t revision,std::uint64_t now) noexcept {
    const auto r=engagedRevision.load(std::memory_order_acquire),t=engagedNs.load(std::memory_order_acquire);
    return r&&r==revision&&t&&(now<t||now-t<=engagedMaxAgeNs);
}
inline bool cutscenePlan(std::uint32_t actorGuid) noexcept {return actorGuid==guidCinematic||actorGuid==guidFirstPerson;}

// ---- report counters ----
struct Stats {
    std::atomic<std::uint64_t> frames{},eligible{},composed{},stereo{},beginFailed{},fp{},cinematic{},cuts{},relatches{},fallbacks{},
        badCamera{},noViews{},johnSamples{},johnValid{},johnCollapsed{},johnSubstitute{},r3Cycles{};
    std::atomic<const char*> fallback{"NONE"},source{"NONE"},head{"NONE"},johnWhy{"NONE"};
    std::atomic<float> lastCutM{0},lastCutDeg{0};
};
inline Stats stats{};
inline void bump(std::atomic<std::uint64_t>& c) noexcept {c.fetch_add(1,std::memory_order_relaxed);}

// ---- render thread composer ----
struct Input {
    std::uint64_t ns{},revision{};Ctx ctx{Ctx::Other};unsigned mode{Screen};
    const float* stock{}; // stock camera world (column-major 4x4: column 2 = back, column 3 = position)
    bool viewsValid{};float hmdYaw{};V3 hmdCenter{};std::uint64_t referenceSpace{};
    John john{};
};
struct Output {V3 position{};float inverseYaw{};V3 origin{};std::uint64_t serial{},referenceSpace{};unsigned effective{Screen};std::uint32_t guid{};
    std::uintptr_t creature{},hat{};bool cut{};};
class Composer {
    bool active_{},havePrev_{};std::uint64_t revision_{},space_{},serials_{},prevNs_{};unsigned effective_{Screen};
    V3 prevPos_{},prevFwd_{},origin_{},latchEye_{};float a0_{},phiFp_{};std::uint64_t serial_{};
    Ctx steady_{Ctx::Other};John good_{};
public:
    // Every visibility call (any presentation): remembers the last non-transition context for the Transition rule.
    void note(Ctx c) noexcept {if(c!=Ctx::Transition)steady_=c;}
    // A camera transition is cinematic only next to a cutscene / gameplay (World -> T -> Cutscene, Cutscene -> T -> World);
    // Override (scripted / minigame / death cameras with essential 2D HUD) and menus stay in the theater.
    bool eligible(Ctx c) const noexcept {
        const bool ov=overrideLever.load(std::memory_order_relaxed);
        return c==Ctx::Cutscene||(ov&&c==Ctx::Override)||(c==Ctx::Transition&&(steady_==Ctx::Cutscene||steady_==Ctx::World||(ov&&steady_==Ctx::Override)));}
    void reset() noexcept {active_=false;havePrev_=false;good_=John{};}
    bool active() const noexcept {return active_;}
    bool step(const Input& in,Output& out) noexcept {
        bump(stats.frames);
        if(in.mode==Screen||!eligible(in.ctx)){reset();return false;}
        bump(stats.eligible);stats.source.store(in.ctx==Ctx::Cutscene?"CUTSCENE":in.ctx==Ctx::Override?"OVERRIDE":"TRANSITION",std::memory_order_relaxed);
        if(!in.stock){bump(stats.badCamera);reset();return false;}
        const float* w=in.stock;const V3 pos{w[12],w[13],w[14]},fwd{-w[8],-w[9],-w[10]};
        float thetaS{};
        if(!finite3(pos)||!finite3(fwd)||!headingOfForward(fwd[0],fwd[2],thetaS)){bump(stats.badCamera);reset();return false;}
        if(!in.viewsValid||!in.referenceSpace||!finite3(in.hmdCenter)||!std::isfinite(in.hmdYaw)){bump(stats.noViews);reset();return false;}
        // effective mode: FIRST PERSON needs a fresh valid John of this presentation, near the cutscene camera
        unsigned eff=Cinematic;const char* why="NONE";John john=in.john;
        if(in.mode==FirstPerson){
            // a FIRST PERSON already running keeps the last good John for johnHoldNs (one stale / collapsed sample must not snap the view)
            const bool current=john.ns&&john.revision==in.revision&&!(in.ns>john.ns&&in.ns-john.ns>johnMaxAgeNs);
            bool held=false;
            if(current&&john.valid)good_=john;
            else if(active_&&effective_==FirstPerson&&good_.valid&&good_.revision==in.revision&&!(in.ns>good_.ns&&in.ns-good_.ns>johnHoldNs)){john=good_;held=true;}
            const auto& j=john;
            if(!held&&!current)why="JOHN_STALE";
            else if(!j.valid)why=j.why&&*j.why?j.why:"JOHN_INVALID";
            else if(!finite3(j.eye)||dist(j.eye,pos)>fpMaxCameraMetres)why="JOHN_FAR";
            else eff=FirstPerson;
            if(eff!=FirstPerson){bump(stats.fallbacks);stats.fallback.store(why,std::memory_order_relaxed);}
        }
        // cut: position / view direction jump in one frame (thresholds grow with the frame gap)
        bool cut=false;
        if(havePrev_&&active_&&revision_==in.revision){
            const float dt=in.ns>prevNs_?std::fmin(float(double(in.ns-prevNs_)*1e-9),.5f):0.f;
            const float d=dist(pos,prevPos_);
            const float dot=fwd[0]*prevFwd_[0]+fwd[1]*prevFwd_[1]+fwd[2]*prevFwd_[2];
            const float deg=std::acos(std::fmax(-1.f,std::fmin(1.f,dot)))*(180.f/kPi);
            if(d>std::fmax(cutMetres,cutMetresPerS*dt)||deg>cutDeg+cutDegPerS*dt){cut=true;bump(stats.cuts);
                stats.lastCutM.store(d,std::memory_order_relaxed);stats.lastCutDeg.store(deg,std::memory_order_relaxed);}
        }
        bool relatch=!active_||revision_!=in.revision||space_!=in.referenceSpace||effective_!=eff;
        if(!relatch&&cut){
            if(eff==Cinematic)relatch=true;
            else if(john.headingValid&&std::fabs(wrapPi(john.heading-phiFp_))*(180.f/kPi)>fpRelatchDeg)relatch=true;
            else if(dist(john.eye,latchEye_)>fpRelatchMetres)relatch=true;
        }
        if(relatch){
            a0_=in.hmdYaw;origin_=in.hmdCenter;space_=in.referenceSpace;serial_=originSerialBit|++serials_;bump(stats.relatches);
            if(eff==FirstPerson){phiFp_=john.headingValid?john.heading:thetaS;latchEye_=john.eye;}
        }
        active_=true;revision_=in.revision;effective_=eff;havePrev_=true;prevPos_=pos;prevFwd_=fwd;prevNs_=in.ns;
        const float phi=eff==FirstPerson?phiFp_:thetaS;
        out=Output{};out.position=eff==FirstPerson?john.eye:pos;out.inverseYaw=wrapPi(phi-a0_-thetaS);out.origin=origin_;
        out.serial=serial_;out.referenceSpace=space_;out.effective=eff;out.cut=cut;
        out.guid=eff==FirstPerson?guidFirstPerson:guidCinematic;
        if(eff==FirstPerson){out.creature=john.creature;out.hat=john.hat;bump(stats.fp);}else bump(stats.cinematic);
        bump(stats.composed);return true;
    }
};
inline Composer composer{}; // render thread only

inline std::string report() {
    char b[1400];const auto s=settings();
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto t=engagedNs.load(std::memory_order_relaxed);const bool active=t&&(now<t||now-t<=engagedMaxAgeNs);
    const unsigned em=engagedMode.load(std::memory_order_relaxed);
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"mode\":\"%s\",\"active\":%s,\"effective\":\"%s\",\"engagedRevision\":%llu,\"inCutscene\":%s,\"suppressCollapse\":%s,"
        "\"source\":\"%s\",\"headBone\":\"%s\",\"fallbackReason\":\"%s\",\"johnWhy\":\"%s\",\"frames\":%llu,\"eligible\":%llu,\"composed\":%llu,"
        "\"stereo\":%llu,\"beginFailed\":%llu,\"fp\":%llu,\"cinematic\":%llu,\"cuts\":%llu,\"relatches\":%llu,\"fallbacks\":%llu,\"badCamera\":%llu,"
        "\"noViews\":%llu,\"johnSamples\":%llu,\"johnValid\":%llu,\"johnCollapsed\":%llu,\"lastCutM\":%.2f,\"lastCutDeg\":%.1f,\"override\":%d,\"johnSubstitute\":%llu,\"r3Cycles\":%llu,",
        enabled(Feature::Cutscene)?"true":"false",modeName(s.cutsceneMode()),active?"true":"false",modeName(active?em:0u),(unsigned long long)engagedRevision.load(std::memory_order_relaxed),
        inCutscene.load(std::memory_order_relaxed)?"true":"false",suppressCollapse.load(std::memory_order_relaxed)?"true":"false",
        stats.source.load(),stats.head.load(),stats.fallback.load(),stats.johnWhy.load(),
        (unsigned long long)stats.frames.load(),(unsigned long long)stats.eligible.load(),(unsigned long long)stats.composed.load(),
        (unsigned long long)stats.stereo.load(),(unsigned long long)stats.beginFailed.load(),(unsigned long long)stats.fp.load(),
        (unsigned long long)stats.cinematic.load(),(unsigned long long)stats.cuts.load(),(unsigned long long)stats.relatches.load(),
        (unsigned long long)stats.fallbacks.load(),(unsigned long long)stats.badCamera.load(),(unsigned long long)stats.noViews.load(),
        (unsigned long long)stats.johnSamples.load(),(unsigned long long)stats.johnValid.load(),(unsigned long long)stats.johnCollapsed.load(),
        double(stats.lastCutM.load()),double(stats.lastCutDeg.load()),overrideLever.load()?1:0,(unsigned long long)stats.johnSubstitute.load(),(unsigned long long)stats.r3Cycles.load());
    std::string out=b;
    out+="\"ctxVisibility\":{";const char* names[8]{"UNKNOWN","WORLD","MENU","CUTSCENE","CAMERA_OVERRIDE","CAMERA_TRANSITION","OPTIC","?"};
    for(unsigned i=0;i<8;++i){out+=i?",\"":"\"";out+=names[i];out+="\":"+std::to_string(ctxVisibility[i].load());}
    out+="},\"ctxChanges\":[";const auto n=ctxChanges.load();
    for(std::uint32_t k=n>16?n-16:0,first=1;k<n;++k,first=0){const auto v=ctxChange[k&15u].load();
        out+=first?"":",";out+="[\"";out+=names[std::min(7u,v>>28)];out+="\","+std::to_string(v&0x0fffffffu)+"]";}
    out+="]}";
    return out;
}
} // namespace rdrvr::vr::cutscene
