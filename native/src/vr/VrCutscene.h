#pragma once
// CUTSCENES: cutscenes in stereo instead of the flat theater quad (VC / SA VR "stereo cutscenes").
// Menu MOVEMENT > CUTSCENES: SCREEN (the 2D theater) / CINEMATIC (default) / FIRST PERSON.
//
//   CINEMATIC eye = cutscene camera position, world heading = the cutscene camera's horizontal
//   heading (pitch / roll dropped by prepareHeadCamera), the HMD (rotation + position) on top.
//   Camera cuts (position jump / view-direction jump in one frame) re-latch the HMD yaw + position
//   origin so the player faces the new shot from its camera point; smooth moves are followed.
//   FIRST PERSON eye = John's eye pivots (head bone fallback), world yaw = where John's HEAD faces (the head joint's right axis of
//   the rig's own bind pose, GameXr cutsceneJohn), followed with a dead zone and smoothing, snapped at a cut / a big jump; re-latched
//   at a cut when John turned > 45 deg / moved > 2 m: the position origin only, the HMD yaw a0 stays from the start of the run, so looking
//   straight ahead is John's forward all scene long (follow 0: latched at the start of the run, a0 too); HMD on top. John = the player actor when it
//   plays the scene itself (grabbed by the cutscene, actor+0x112 bits 1/2, not hidden, bit 3), else the cutscene's own actor
//   (PLAYER_cs, by name). Once FIRST PERSON has had a good John it never goes to the stock shot camera again: a hidden John is
//   tracked while his bones are sane, a missing one holds the last good eye and heading until he is back. Only before the first good
//   John (not resolved yet, hidden, far from the shot) and in a scene whose cast has no John (the hold would freeze on the last scene)
//   the view is the stock camera (CINEMATIC, counted as a fallback).
//   John's head: the player's by the gameplay BODY path (eye BodyDrawFilter: head-collapse fill, or the whole body when BODY is
//   off); the cutscene's own John has no collapse, his whole record is hidden in the eyes (guidFirstPersonSub).
//
// Composition: VisibilityCameraBridge::begin -> prepareHeadCamera keeps the STOCK camera's heading
// thetaS; the anchor's TrackingOrigin (position + inverseYaw) is set by this module:
// inverseYaw = R_y(phi - a0 - thetaS) gives the effective world yaw phi - a0 for any target phi
// (phi = thetaS CINEMATIC, the followed John heading FIRST PERSON; a0 = HMD yaw at the latch), offsets rotate with it (CameraAdapter).
// World forward heading = thetaS + inverseYaw + hmdYaw = phi + (hmdYaw - a0): the stock heading cancels, a stock camera cut does not rotate FIRST PERSON.
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
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <limits>
#include <mutex>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::cutscene {
using V3=std::array<float,3>;
enum Mode : unsigned {Screen=0,Cinematic=1,FirstPerson=2};
enum class Ctx : unsigned {Other,World,Cutscene,Transition,Override};
// Scripted cameras (CameraOverride: the top camera is not the game camera and no cutscene plays). RDR runs its short gameplay
// sequences (looting, hitching the horse, hogtying, finishers, skinning) as gohCameraShot sequences on a camCutsceneCamera
// (CutsceneCameraTuning::PlayCutScene, rdr2GOH::AddCameraTransitionShot*): NativeCameraMode::Shot. Those are cinematic (stereo)
// for the first shotMax seconds of the shot; longer scripted cameras (minigame tables with their 2D HUD) fall back to the theater.
// debug.rdrvr.vr.cutscene.override (live, GameXr re-reads it): 0 = no Override is cinematic, 1 = every Override
// (any camera class, no time limit), unset / other = 2 = scripted shots only (default).
// debug.rdrvr.vr.cutscene.shotmax <seconds> (default 15, 0 = no limit).
// debug.rdrvr.vr.cutscene.cull 1 = the mod's own culls (SMALL OBJECTS skip, EYE CULL A/B) also act in stereo cutscenes; unset /
// 0 (default) = none there: animated cutscene props are tested with their ROOT matrix sphere (SmallObjectCull::sphere), which
// is not where the skinned / bone-driven geometry is, so props would vanish in both eyes (A / SMALL OBJECTS) or in one (B pads).
// FIRST PERSON yaw (live): debug.rdrvr.vr.cutscene.follow 0 = a latched heading; .followdz <deg> dead zone (default 6);
// .followrate <deg/s> top speed (default 90).
inline std::atomic<int> overrideMode{2},shotMaxSeconds{15};
inline std::atomic<bool> overrideShot{false},cutsceneCull{false},followOn{true};
inline std::atomic<float> followDzDeg{6.f},followRateDegS{90.f};
inline std::atomic<int> shadowCamProperty{-1}; // debug.rdrvr.vr.cutscene.shadowcam 0 / 1 / 2 beats the SUN SHADOW CAM row (stereo/CutsceneShadowCam.h)
inline std::atomic<std::uint64_t> shotExpired{0},cullKept{0}; // shots past shotMax (theater), SMALL OBJECTS skips undone in cutscenes
inline void readOverrideLever() noexcept {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX]{};int n=__system_property_get("debug.rdrvr.vr.cutscene.override",v);
    overrideMode.store(n>0&&(v[0]=='0'||v[0]=='1')&&!v[1]?v[0]-'0':2,std::memory_order_relaxed);
    n=__system_property_get("debug.rdrvr.vr.cutscene.shotmax",v);
    shotMaxSeconds.store(n>0&&v[0]>='0'&&v[0]<='9'?std::clamp(std::atoi(v),0,3600):15,std::memory_order_relaxed);
    n=__system_property_get("debug.rdrvr.vr.cutscene.cull",v);
    cutsceneCull.store(n>0&&v[0]=='1',std::memory_order_relaxed);
    n=__system_property_get("debug.rdrvr.vr.cutscene.follow",v);
    followOn.store(!(n>0&&v[0]=='0'),std::memory_order_relaxed);
    n=__system_property_get("debug.rdrvr.vr.cutscene.followdz",v);
    followDzDeg.store(n>0&&v[0]>='0'&&v[0]<='9'?std::clamp(float(std::atof(v)),0.f,45.f):6.f,std::memory_order_relaxed);
    n=__system_property_get("debug.rdrvr.vr.cutscene.followrate",v);
    followRateDegS.store(n>0&&v[0]>='0'&&v[0]<='9'?std::clamp(float(std::atof(v)),5.f,720.f):90.f,std::memory_order_relaxed);
    n=__system_property_get("debug.rdrvr.vr.cutscene.shadowcam",v);
    shadowCamProperty.store(n==1&&v[0]>='0'&&v[0]<='2'?v[0]-'0':-1,std::memory_order_relaxed);
#endif
}
// The Override presentation is cinematic (overrideShot: GameXr, main thread, from NativeUiContext::shotSince + shotMax).
inline bool overrideEligible() noexcept {
    const int m=overrideMode.load(std::memory_order_relaxed);return m==1||(m==2&&overrideShot.load(std::memory_order_relaxed));}
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
// anchor actorGuid of cutscene plans (never a player); Sub = FIRST PERSON on the cutscene's own John (no head collapse of his: whole record hidden, no head fill)
inline constexpr std::uint32_t guidCinematic=0xC5C50001u,guidFirstPerson=0xC5C50002u,guidFirstPersonSub=0xC5C50003u;
inline constexpr std::uint64_t originSerialBit=std::uint64_t(1)<<62;                 // TrackingOrigin serials (the rig counts from 1)
inline constexpr std::uint64_t johnMaxAgeNs=150'000'000,engagedMaxAgeNs=250'000'000;
inline constexpr float cutMetres=2.f,cutMetresPerS=25.f,cutDeg=30.f,cutDegPerS=90.f,fpRelatchDeg=45.f,fpRelatchMetres=2.f,fpMaxCameraMetres=60.f;
// FIRST PERSON follow: stops when within fpFollowStopDeg, time constant fpFollowTauS; one-frame heading change > fpSnapDeg / eye jump = a discontinuity
// of John's animation (a cut the stock camera may not show); a hidden John further than hiddenJumpMetres from his last visible eye is not believed (held).
inline constexpr float fpFollowStopDeg=1.f,fpFollowTauS=.25f,fpSnapDeg=40.f,johnCutMetres=1.f,johnCutMetresPerS=8.f,hiddenJumpMetres=8.f;
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
enum HeadingSource : unsigned {HeadingNone=0,HeadingAxes=1,HeadingEyeOffset=2,HeadingMover=3}; // HeadingMover: the real player only (a cutscene actor's mover is the scene root)
struct John {
    std::uint64_t ns{},revision{};bool valid{};V3 eye{},head{};float heading{};bool headingValid{},eyePivots{};
    std::uint32_t guid{};std::uintptr_t creature{},hat{};const char* why{"NONE"};
    bool hidden{},substitute{},noJohn{};unsigned source{HeadingNone}; // noJohn: the cutscene's cast is readable and nobody in it is John
    float moverHeading{},offsetHeading{};bool moverValid{},offsetValid{}; // the alternative sources, report ring only: how far they are from the head axes
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
inline bool cutscenePlan(std::uint32_t actorGuid) noexcept {return actorGuid==guidCinematic||actorGuid==guidFirstPerson||actorGuid==guidFirstPersonSub;}
// A draw of a stereo cutscene plan skips no object for the mod's own culls (SMALL OBJECTS / EYE CULL), see cutsceneCull.
inline bool cullExempt(std::uint32_t actorGuid) noexcept {return cutscenePlan(actorGuid)&&!cutsceneCull.load(std::memory_order_relaxed);}

// ---- report counters ----
inline void bump(std::atomic<std::uint64_t>& c) noexcept {c.fetch_add(1,std::memory_order_relaxed);}
// Frames per reason (one writer: the render thread); a string that does not fit the table goes to the last row.
struct WhyCounts {
    std::array<std::atomic<const char*>,8> name{};std::array<std::atomic<std::uint64_t>,8> n{};
    void add(const char* w) noexcept {
        for(std::size_t i=0;i<name.size();++i){const char* c=name[i].load(std::memory_order_relaxed);
            if(!c){name[i].store(w,std::memory_order_relaxed);c=w;}
            if(c==w||std::strcmp(c,w)==0||i+1==name.size()){bump(n[i]);return;}}}
    void append(std::string& o) const {o+="{";bool first=true;
        for(std::size_t i=0;i<name.size();++i){const char* c=name[i].load(std::memory_order_relaxed);if(!c)break;
            o+=first?"\"":",\"";o+=c;o+="\":"+std::to_string(n[i].load(std::memory_order_relaxed));first=false;}
        o+="}";}
};
struct Stats {
    std::atomic<std::uint64_t> frames{},eligible{},composed{},stereo{},beginFailed{},fp{},cinematic{},cuts{},relatches{},fallbacks{},
        badCamera{},noViews{},johnSamples{},johnValid{},johnCollapsed{},johnSubstitute{},r3Cycles{},
        holdFrames{},holdRuns{},holdMaxFrames{},followFrames{},snaps{},johnCuts{},
        headingAxes{},headingEyeOffset{},headingMover{},headingNone{},
        hiddenFrames{},hiddenMoved{},hiddenFrozen{},hiddenJumps{},lockScans{},locks{},lockLost{},rigResolves{},skelNative{},skelFail{};
    std::atomic<const char*> fallback{"NONE"},source{"NONE"},head{"NONE"},johnWhy{"NONE"},heading{"NONE"};
    std::atomic<float> lastCutM{0},lastCutDeg{0},followedDeg{0};
    std::atomic<bool> johnHidden{false};
    WhyCounts fallbacksBy{},holdsBy{};
};
inline Stats stats{};
inline const char* headingName(unsigned s) noexcept {return s==HeadingAxes?"HEAD_AXES":s==HeadingEyeOffset?"EYE_OFFSET":s==HeadingMover?"MOVER":"NONE";}
// The rig John's bones were read with (main thread, on a new skeleton; report).
struct RigFacts {std::uintptr_t data{};int own{},count{},head{-1},eyeL{-1},eyeR{-1},clavR{-1},clavL{-1};const char* state{"NONE"},*rightFrom{"NONE"};V3 right{};float bindFwdEyeCm{};};
inline RigFacts rigFacts{}; // johnMutex
inline void publishRigFacts(const RigFacts& f) noexcept {std::lock_guard lock(johnMutex);rigFacts=f;}
// Recent FIRST PERSON samples for the report: 2 per second, plus every state change (relatch, cut, hold start / end). Angles in degrees.
struct RingSample {std::uint32_t ms{};std::uint8_t eff{},held{},hidden{},source{},relatch{},cut{};float heading{},phi{},thetaS{},hmd{},a0{},mover{},offset{},camM{};const char* why{};};
inline std::mutex ringMutex;inline std::array<RingSample,48> ring{};inline std::uint32_t ringNext{}; // ringMutex
inline void pushRing(const RingSample& r) noexcept {std::lock_guard lock(ringMutex);ring[ringNext++%ring.size()]=r;}

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
    // FIRST PERSON follow: good_ = the last good John of this scene (never dropped for a missing John, only with the scene / the mode)
    V3 prevEye_{};float prevHeading_{};bool havePrevEye_{},havePrevHeading_{},phiValid_{},following_{},prevHeld_{};unsigned prevRingEff_{Screen};
    std::uint64_t holdRun_{},ringNs_{},ringFirstNs_{};
    void softReset() noexcept {active_=false;havePrev_=false;havePrevEye_=false;havePrevHeading_=false;following_=false;}
public:
    // Every visibility call (any presentation): remembers the last non-transition context for the Transition rule.
    void note(Ctx c) noexcept {if(c!=Ctx::Transition)steady_=c;}
    // A camera transition is cinematic only next to a cutscene / gameplay (World -> T -> Cutscene, Cutscene -> T -> World);
    // Override: scripted gameplay shots only (overrideEligible); other scripted / minigame cameras with essential 2D HUD and menus
    // stay in the theater.
    bool eligible(Ctx c) const noexcept {
        const bool ov=overrideEligible();
        return c==Ctx::Cutscene||(ov&&c==Ctx::Override)||(c==Ctx::Transition&&(steady_==Ctx::Cutscene||steady_==Ctx::World||(ov&&steady_==Ctx::Override)));}
    void reset() noexcept {softReset();good_=John{};phiValid_=false;holdRun_=0;prevHeld_=false;}
    bool active() const noexcept {return active_;}
    bool step(const Input& in,Output& out) noexcept {
        bump(stats.frames);
        if(in.mode==Screen||!eligible(in.ctx)){reset();return false;}
        bump(stats.eligible);stats.source.store(in.ctx==Ctx::Cutscene?"CUTSCENE":in.ctx==Ctx::Override?"OVERRIDE":"TRANSITION",std::memory_order_relaxed);
        if(!in.stock){bump(stats.badCamera);softReset();return false;}
        const float* w=in.stock;const V3 pos{w[12],w[13],w[14]},fwd{-w[8],-w[9],-w[10]};
        float thetaS{};
        if(!finite3(pos)||!finite3(fwd)||!headingOfForward(fwd[0],fwd[2],thetaS)){bump(stats.badCamera);softReset();return false;}
        if(!in.viewsValid||!in.referenceSpace||!finite3(in.hmdCenter)||!std::isfinite(in.hmdYaw)){bump(stats.noViews);softReset();return false;}
        // effective mode. FIRST PERSON: this frame's John when he is usable, else the last good John of this scene (eye and heading held,
        // never the stock camera). Before the first good John (he must be visible and near the shot): the stock camera, a counted fallback.
        unsigned eff=Cinematic;const char* why="NONE";John john=in.john;bool held=false;
        if(in.mode==FirstPerson){
            const bool current=john.ns&&john.revision==in.revision&&!(in.ns>john.ns&&in.ns-john.ns>johnMaxAgeNs);
            if(current&&john.noJohn)good_=John{};
            const bool usable=current&&john.valid&&finite3(john.eye)&&(good_.valid||(!john.hidden&&dist(john.eye,pos)<=fpMaxCameraMetres));
            if(!usable)why=!current?"JOHN_STALE":!john.valid?(john.why&&*john.why?john.why:"JOHN_INVALID"):!finite3(john.eye)?"JOHN_NOT_FINITE":john.hidden?"JOHN_HIDDEN_AT_START":"JOHN_FAR_AT_START";
            if(usable){good_=john;eff=FirstPerson;}
            else if(good_.valid){john=good_;held=true;eff=FirstPerson;bump(stats.holdFrames);stats.holdsBy.add(why);
                if(!holdRun_++)bump(stats.holdRuns);
                if(holdRun_>stats.holdMaxFrames.load(std::memory_order_relaxed))stats.holdMaxFrames.store(holdRun_,std::memory_order_relaxed);}
            else{bump(stats.fallbacks);stats.fallbacksBy.add(why);stats.fallback.store(why,std::memory_order_relaxed);}
            if(!held)holdRun_=0;
        }else{good_=John{};holdRun_=0;}
        const float dt=havePrev_&&in.ns>prevNs_?std::fmin(float(double(in.ns-prevNs_)*1e-9),.5f):0.f;
        // cut: position / view direction jump in one frame (thresholds grow with the frame gap)
        bool cut=false;
        if(havePrev_&&active_&&revision_==in.revision){
            const float d=dist(pos,prevPos_);
            const float dot=fwd[0]*prevFwd_[0]+fwd[1]*prevFwd_[1]+fwd[2]*prevFwd_[2];
            const float deg=std::acos(std::fmax(-1.f,std::fmin(1.f,dot)))*(180.f/kPi);
            if(d>std::fmax(cutMetres,cutMetresPerS*dt)||deg>cutDeg+cutDegPerS*dt){cut=true;bump(stats.cuts);
                stats.lastCutM.store(d,std::memory_order_relaxed);stats.lastCutDeg.store(deg,std::memory_order_relaxed);}
        }
        // Not in a camera transition: with the gameplay body on, the player's head follows the HMD there, phi would chase itself
        const bool follow=eff==FirstPerson&&in.ctx!=Ctx::Transition&&followOn.load(std::memory_order_relaxed);
        // John's own discontinuity: the eye jumped (a new animation section, back from a hold) even if the stock camera did not cut
        bool johnCut=false;
        if(follow&&!held&&havePrevEye_&&dist(john.eye,prevEye_)>std::fmax(johnCutMetres,johnCutMetresPerS*dt)){johnCut=true;bump(stats.johnCuts);}
        const bool first=!active_||revision_!=in.revision||space_!=in.referenceSpace||effective_!=eff;
        bool relatch=first;
        if(!relatch&&(cut||johnCut)){
            if(eff==Cinematic)relatch=true;
            else if(john.headingValid&&std::fabs(wrapPi(john.heading-phiFp_))*(180.f/kPi)>fpRelatchDeg)relatch=true;
            else if(dist(john.eye,latchEye_)>fpRelatchMetres)relatch=true;
        }
        if(relatch){
            // A cut while following keeps a0: re-latching it on a turned head would make THAT direction John's forward for the rest of the scene
            if(first||!follow)a0_=in.hmdYaw;
            origin_=in.hmdCenter;space_=in.referenceSpace;serial_=originSerialBit|++serials_;bump(stats.relatches);following_=false;
            if(eff==FirstPerson){if(john.headingValid)phiFp_=john.heading;else if(!phiValid_)phiFp_=thetaS;phiValid_=true;latchEye_=john.eye;}
        }else if(follow&&!held&&john.headingValid){
            // phi follows John's head: idle motion inside the dead zone is ignored, a real turn is followed until phi is on his heading again
            const float e=wrapPi(john.heading-phiFp_),ae=std::fabs(e),dz=followDzDeg.load(std::memory_order_relaxed)*(kPi/180.f);
            const bool jump=havePrevHeading_&&std::fabs(wrapPi(john.heading-prevHeading_))>fpSnapDeg*(kPi/180.f);
            if((cut||johnCut||jump)&&ae>dz){phiFp_=john.heading;following_=false;bump(stats.snaps);}
            else{
                if(!following_&&ae>dz)following_=true;
                if(following_&&dt>0.f){
                    const float stepped=std::fmin(ae*(1.f-std::exp(-dt/fpFollowTauS)),followRateDegS.load(std::memory_order_relaxed)*(kPi/180.f)*dt);
                    phiFp_=wrapPi(phiFp_+std::copysign(stepped,e));bump(stats.followFrames);
                    stats.followedDeg.store(stats.followedDeg.load(std::memory_order_relaxed)+stepped*(180.f/kPi),std::memory_order_relaxed);
                    if(ae<fpFollowStopDeg*(kPi/180.f))following_=false;
                }
            }
        }
        if(eff==FirstPerson&&!held){prevEye_=john.eye;havePrevEye_=true;if(john.headingValid){prevHeading_=john.heading;havePrevHeading_=true;}}
        if(eff!=FirstPerson){havePrevEye_=false;havePrevHeading_=false;following_=false;}
        active_=true;revision_=in.revision;effective_=eff;havePrev_=true;prevPos_=pos;prevFwd_=fwd;prevNs_=in.ns;
        const float phi=eff==FirstPerson?phiFp_:thetaS;
        out=Output{};out.position=eff==FirstPerson?john.eye:pos;out.inverseYaw=wrapPi(phi-a0_-thetaS);out.origin=origin_;
        out.serial=serial_;out.referenceSpace=space_;out.effective=eff;out.cut=cut;
        out.guid=eff==FirstPerson?(john.substitute?guidFirstPersonSub:guidFirstPerson):guidCinematic;
        if(eff==FirstPerson){out.creature=john.creature;out.hat=john.hat;bump(stats.fp);}else bump(stats.cinematic);
        if(in.mode==FirstPerson&&(!ringNs_||in.ns<ringNs_||in.ns-ringNs_>=500'000'000ull||relatch||cut||johnCut||held!=prevHeld_||eff!=prevRingEff_)){
            if(!ringFirstNs_)ringFirstNs_=in.ns;
            const float toDeg=180.f/kPi,nan=std::numeric_limits<float>::quiet_NaN();
            RingSample r{};r.ms=std::uint32_t(std::min<std::uint64_t>((in.ns-ringFirstNs_)/1000000u,0xffffffffu));r.eff=std::uint8_t(eff);r.held=held;r.hidden=john.hidden;
            r.source=std::uint8_t(eff==FirstPerson?john.source:HeadingNone);r.relatch=relatch;r.cut=cut?1:johnCut?2:0;
            r.heading=eff==FirstPerson&&john.headingValid?john.heading*toDeg:nan;r.phi=phi*toDeg;r.thetaS=thetaS*toDeg;r.hmd=in.hmdYaw*toDeg;r.a0=a0_*toDeg;
            r.mover=eff==FirstPerson&&john.moverValid?john.moverHeading*toDeg:nan;r.offset=eff==FirstPerson&&john.offsetValid?john.offsetHeading*toDeg:nan;
            r.camM=eff==FirstPerson?dist(john.eye,pos):0.f;r.why=why;pushRing(r);ringNs_=in.ns;
        }
        prevHeld_=held;prevRingEff_=eff;
        bump(stats.composed);return true;
    }
};
inline Composer composer{}; // render thread only

inline std::string report() {
    char b[1700];const auto s=settings();
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto t=engagedNs.load(std::memory_order_relaxed);const bool active=t&&(now<t||now-t<=engagedMaxAgeNs);
    const unsigned em=engagedMode.load(std::memory_order_relaxed);
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"mode\":\"%s\",\"active\":%s,\"effective\":\"%s\",\"engagedRevision\":%llu,\"inCutscene\":%s,\"suppressCollapse\":%s,"
        "\"source\":\"%s\",\"headBone\":\"%s\",\"fallbackReason\":\"%s\",\"johnWhy\":\"%s\",\"frames\":%llu,\"eligible\":%llu,\"composed\":%llu,"
        "\"stereo\":%llu,\"beginFailed\":%llu,\"fp\":%llu,\"cinematic\":%llu,\"cuts\":%llu,\"relatches\":%llu,\"fallbacks\":%llu,\"badCamera\":%llu,"
        "\"noViews\":%llu,\"johnSamples\":%llu,\"johnValid\":%llu,\"johnCollapsed\":%llu,\"lastCutM\":%.2f,\"lastCutDeg\":%.1f,\"override\":%d,\"overrideShot\":%d,\"shotMaxS\":%d,\"shotExpired\":%llu,\"cull\":%d,\"cullKept\":%llu,\"johnSubstitute\":%llu,\"r3Cycles\":%llu,",
        enabled(Feature::Cutscene)?"true":"false",modeName(s.cutsceneMode()),active?"true":"false",modeName(active?em:0u),(unsigned long long)engagedRevision.load(std::memory_order_relaxed),
        inCutscene.load(std::memory_order_relaxed)?"true":"false",suppressCollapse.load(std::memory_order_relaxed)?"true":"false",
        stats.source.load(),stats.head.load(),stats.fallback.load(),stats.johnWhy.load(),
        (unsigned long long)stats.frames.load(),(unsigned long long)stats.eligible.load(),(unsigned long long)stats.composed.load(),
        (unsigned long long)stats.stereo.load(),(unsigned long long)stats.beginFailed.load(),(unsigned long long)stats.fp.load(),
        (unsigned long long)stats.cinematic.load(),(unsigned long long)stats.cuts.load(),(unsigned long long)stats.relatches.load(),
        (unsigned long long)stats.fallbacks.load(),(unsigned long long)stats.badCamera.load(),(unsigned long long)stats.noViews.load(),
        (unsigned long long)stats.johnSamples.load(),(unsigned long long)stats.johnValid.load(),(unsigned long long)stats.johnCollapsed.load(),
        double(stats.lastCutM.load()),double(stats.lastCutDeg.load()),overrideMode.load(),overrideShot.load()?1:0,shotMaxSeconds.load(),
        (unsigned long long)shotExpired.load(),cutsceneCull.load()?1:0,(unsigned long long)cullKept.load(),(unsigned long long)stats.johnSubstitute.load(),(unsigned long long)stats.r3Cycles.load());
    std::string out=b;
    out+="\"ctxVisibility\":{";const char* names[8]{"UNKNOWN","WORLD","MENU","CUTSCENE","CAMERA_OVERRIDE","CAMERA_TRANSITION","OPTIC","?"};
    for(unsigned i=0;i<8;++i){out+=i?",\"":"\"";out+=names[i];out+="\":"+std::to_string(ctxVisibility[i].load());}
    out+="},\"ctxChanges\":[";const auto n=ctxChanges.load();
    for(std::uint32_t k=n>16?n-16:0,first=1;k<n;++k,first=0){const auto v=ctxChange[k&15u].load();
        out+=first?"":",";out+="[\"";out+=names[std::min(7u,v>>28)];out+="\","+std::to_string(v&0x0fffffffu)+"]";}
    out+="]";
    {char t[512];std::snprintf(t,sizeof t,",\"follow\":{\"on\":%d,\"dzDeg\":%.1f,\"rateDegS\":%.0f,\"frames\":%llu,\"followedDeg\":%.0f,\"snaps\":%llu,\"johnCuts\":%llu},"
        "\"heading\":{\"now\":\"%s\",\"headAxes\":%llu,\"eyeOffset\":%llu,\"mover\":%llu,\"none\":%llu},",
        followOn.load()?1:0,double(followDzDeg.load()),double(followRateDegS.load()),(unsigned long long)stats.followFrames.load(),double(stats.followedDeg.load()),
        (unsigned long long)stats.snaps.load(),(unsigned long long)stats.johnCuts.load(),stats.heading.load(),(unsigned long long)stats.headingAxes.load(),
        (unsigned long long)stats.headingEyeOffset.load(),(unsigned long long)stats.headingMover.load(),(unsigned long long)stats.headingNone.load());out+=t;
     std::snprintf(t,sizeof t,"\"hold\":{\"frames\":%llu,\"runs\":%llu,\"maxFrames\":%llu,\"by\":",(unsigned long long)stats.holdFrames.load(),
        (unsigned long long)stats.holdRuns.load(),(unsigned long long)stats.holdMaxFrames.load());out+=t;stats.holdsBy.append(out);
     out+="},\"fallbacksBy\":";stats.fallbacksBy.append(out);
     std::snprintf(t,sizeof t,",\"hidden\":{\"now\":%d,\"frames\":%llu,\"bonesMoved\":%llu,\"bonesFrozen\":%llu,\"jumpsHeld\":%llu},\"lock\":{\"scans\":%llu,\"locks\":%llu,\"lost\":%llu},",
        stats.johnHidden.load()?1:0,(unsigned long long)stats.hiddenFrames.load(),(unsigned long long)stats.hiddenMoved.load(),(unsigned long long)stats.hiddenFrozen.load(),
        (unsigned long long)stats.hiddenJumps.load(),(unsigned long long)stats.lockScans.load(),(unsigned long long)stats.locks.load(),(unsigned long long)stats.lockLost.load());out+=t;
     RigFacts f{};{std::lock_guard lock(johnMutex);f=rigFacts;}
     std::snprintf(t,sizeof t,"\"rig\":{\"own\":%d,\"count\":%d,\"head\":%d,\"eyeL\":%d,\"eyeR\":%d,\"clavR\":%d,\"clavL\":%d,\"state\":\"%s\",\"rightFrom\":\"%s\",\"right\":[%.3f,%.3f,%.3f],\"bindFwdEyeCm\":%.1f,\"resolves\":%llu,\"skelNative\":%llu,\"skelFail\":%llu},",
        f.own,f.count,f.head,f.eyeL,f.eyeR,f.clavR,f.clavL,f.state,f.rightFrom,double(f.right[0]),double(f.right[1]),double(f.right[2]),double(f.bindFwdEyeCm),
        (unsigned long long)stats.rigResolves.load(),(unsigned long long)stats.skelNative.load(),(unsigned long long)stats.skelFail.load());out+=t;}
    // eff: 1 CINEMATIC, 2 FIRST PERSON; src: 0 none, 1 head axes, 2 eye offset, 3 mover; cut: 1 stock camera, 2 John; mover / eyeOffset: the alternative heading sources; camM: John's eye to the shot camera
    out+="\"ringColumns\":\"ms,eff,held,hidden,src,relatch,cut,head,phi,thetaS,hmd,a0,mover,eyeOffset,camM,why\",\"ring\":[";
    {std::lock_guard lock(ringMutex);const auto n2=ringNext,cap=std::uint32_t(ring.size());
     const auto num=[](float v,char* d,std::size_t m){if(std::isfinite(v))std::snprintf(d,m,"%.1f",double(v));else std::snprintf(d,m,"null");};
     for(std::uint32_t k=n2>cap?n2-cap:0,first=1;k<n2;++k,first=0){const auto& r=ring[k%cap];char h[16],p[16],th[16],hm[16],a[16],mo[16],of[16],row[220];
        num(r.heading,h,sizeof h);num(r.phi,p,sizeof p);num(r.thetaS,th,sizeof th);num(r.hmd,hm,sizeof hm);num(r.a0,a,sizeof a);num(r.mover,mo,sizeof mo);num(r.offset,of,sizeof of);
        std::snprintf(row,sizeof row,"%s[%u,%u,%u,%u,%u,%u,%u,%s,%s,%s,%s,%s,%s,%s,%.1f,\"%s\"]",first?"":",",r.ms,unsigned(r.eff),unsigned(r.held),unsigned(r.hidden),unsigned(r.source),unsigned(r.relatch),unsigned(r.cut),
            h,p,th,hm,a,mo,of,double(r.camM),r.why?r.why:"");out+=row;}}
    out+="]}";
    return out;
}
} // namespace rdrvr::vr::cutscene
