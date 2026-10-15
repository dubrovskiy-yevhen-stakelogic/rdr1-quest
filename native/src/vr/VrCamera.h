#pragma once
// Camera anchor from the player capsule + roomscale. Header-only, no engine access of its own:
// the engine facts come from HeadAnchorReader (BodyAnchorFacts), the HMD from the TrackedViews mailbox, the LOCAL->world
// yaw from VisibilityCameraBridge::worldYaw, the LOCO record from VrBodyLink.h.
//
// Threads: Rig::apply runs on the engine main thread in the PostCameraUpdate reticle hook (after physics, GameXr
// gameObserveReticle); Rig::report on the report writer. The render thread never touches the rig: it receives the result
// inside HeadCameraAnchor (position + TrackingOrigin in ONE mailbox sample), so a capsule of one frame is never composed with
// the body point B of another.
//
// World eye (render thread, CameraAdapter):  eye = P + R(psi) * (hmdLocal - O)
//   BODY:  P = C + up * eyeHeight (C = capsule base mover+0x70, up = world Y), O = (B.x, recenterY, B.z)
//   HEAD:  P = head bone (legacy), O = (eyeAtLatch.x, recenterY, eyeAtLatch.z) (legacy recenter point, persistent)
//   psi = stock camera yaw (pitch/roll dropped) composed with the latched inverse HMD yaw.
// B is a LOCAL-space "body point" (the neck pivot under the HMD, see neckBack/neckDown). Roomscale asks LOCO to move the
// capsule toward the neck (gain * R(psi)*(neck-B), hard-limited per frame and per second) and moves B by the capsule's TOTAL
// measured motion while roomscale owns it, so the view never jumps; push-back keeps |neck-B| <= maxLean.
// Crediting is lag-agnostic: crediting per substep would give B the WRONG SIGN on the device (the
// engine realises a substep's command one substep later) -> positive feedback. Requests go out only
// with a settled neutral stick, a convergence watchdog (ROOMSCALE FAULT) stops a runaway, and
// ROOMSCALE defaults to OFF (VrSettings.h).
#include "HeadCameraAnchor.h"
#include "VrBodyLink.h"
#include "VrSettings.h"
#include "VrTeleportLink.h" // TELEPORT: warp note -> filter / B snap, warp head key K, psi samples
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace rdrvr::vr::camera {
using runtime::Vec3;
using runtime::Quat;
using engine::BodyAnchorFacts;
using engine::HeadCameraAnchor;
using engine::TrackingOrigin;

// ---- engine facts (librdr 1.58 VA) ----
// mvrMoverSimComponent vtable: the player's mover must be a Sim mover for the ragdoll flag (+0xb8c) and for roomscale.
// TraceEntry checks both code fingerprints and the GOT data word, then stores engineBase+moverSimVtableAddressPoint below.
namespace engine_facts {
inline constexpr std::uintptr_t moverSimCtorOffset=0x2892c34; // mvrMoverSimComponent::mvrMoverSimComponent(): str [GOT]+0x10,[x0]
inline constexpr std::array<unsigned char,36> moverSimCtorFingerprint={
#include "gamecode/VrCamera_2892c34_36_8d.inc"
};
inline constexpr std::uintptr_t becomeRagdollOffset=0x2892e9c;  // mvrMoverSimComponent::BecomeRagdoll: ... str w8(=1),[x19,#0xb8c]
inline constexpr std::array<unsigned char,52> becomeRagdollFingerprint={
#include "gamecode/VrCamera_2892e9c_52_8d.inc"
};
inline constexpr std::uintptr_t moverSimVtableGot=0x2e0c468;          // R_AARCH64_GLOB_DAT _ZTV20mvrMoverSimComponent
inline constexpr std::uintptr_t moverSimVtableSymbol=0x2db79d0;
inline constexpr std::uintptr_t moverSimVtableAddressPoint=0x2db79e0; // symbol + 0x10 = *(mover)
}
// engineBase+moverSimVtableAddressPoint once verified (0 = unverified: no ragdoll fact, roomscale UNAVAILABLE). Any thread.
inline std::atomic<std::uintptr_t> moverSimVtable{0};

enum class Mode : std::uint8_t {Body,Head};
struct Settings {
    bool enabled{true};          // false = debug.rdrvr.vr.camera 0: legacy anchor/latch untouched (sample with requireHead)
    Mode mode{Mode::Body};       // CAMERA BODY|HEAD
    float eyeHeight{};           // EYE HEIGHT: 0 = AUTO, otherwise metres
    bool roomscale{false};       // ROOMSCALE ON|OFF (default OFF; forced OFF by debug.rdrvr.vr.roomscale 0 or camera 0)
    // LOCO can consume head requests at all (mover hook installed + LOCOMOTION FREE). Set by GameXr each frame;
    // false = ROOMSCALE ON behaves like OFF (lean limit maxLeanOff, no requests) and the row shows "NO LOCO".
    bool locoReady{true};
    float bodyEyeUp{.16f};       // BODY OFFSET UP - the eye above the character's head joint while the body is shown (AUTO); default 16 cm
};
inline Settings settingsFrom(const Snapshot& s) noexcept {
    Settings out{};out.enabled=enabled(Feature::Camera);out.mode=s.bodyCamera()?Mode::Body:Mode::Head;
    out.eyeHeight=s.eyeHeightMeters();out.roomscale=s.roomscale()&&enabled(Feature::Roomscale)&&out.enabled;
    out.bodyEyeUp=s.bodyEyeUpMeters();
    return out;
}
// HeadAnchorReader::sample(..., facts, requireHead): only the legacy and HEAD anchors need the head bone.
inline bool requireHead(const Settings& s) noexcept {return !s.enabled||s.mode==Mode::Head;}

struct Tuning {
    float defaultEyeHeight=1.62f,minEyeHeight=1.3f,maxEyeHeight=2.0f;
    float calibrationTau=3.f;        // AUTO: slow filter of headBone.y - C.y
    float stillSpeed=0.10f;          // m/s horizontal: "standing without moving"
    float stillSeconds=0.5f;         // stillness before samples count
    float firstCalibrationSeconds=1.f; // first calibration = mean over this window (then the slow filter)
    float minSample=1.0f,maxSample=2.4f; // plausible headBone - capsule base
    float ragdollTau=0.25f,recoverySeconds=0.5f,snapDistance=3.f;
    float maxLeanOn=0.30f,maxLeanOff=0.60f;
    // ---- roomscale ----
    // Requests start when |lean| >= startLean and stop below stopLean (hysteresis: no hunting around a dead zone).
    float startLean=0.05f,stopLean=0.02f;
    // Proportional follow: request = gain * R(psi) * lean per frame. Lag-tolerant: e[n+1] = e[n] - k*e[n-d] is stable for
    // k < 2 sin(pi / (2(2d+1))) = 1.0 (d=1), 0.618 (d=2), 0.445 (d=3); the device shows ~1 substep of lag.
    float gain=0.35f;
    // Hard limits: metres per frame and metres per second (leaky bucket, capacity = one second).
    float maxRequestPerFrame=0.05f,maxRequestSpeed=1.5f;
    // The capsule only follows the head while the stick has been neutral this long and the capsule is (nearly) still.
    float stickSettleSeconds=0.30f,settleSpeed=0.15f;
    std::uint64_t trailNs=400'000'000; // capsule motion keeps being credited to B this long after the last request
    float maxCreditPerFrame=0.15f;     // larger capsule jumps (teleport, scripted warp) are never absorbed into B
    // Convergence watchdog: faultFrames consecutive credited frames (each >= faultMinMotion) after which the lean did not
    // shrink -> ROOMSCALE FAULT. Runaway guard: credited capsule travel exceeds the neck's own travel by runawayMargin
    // (both decaying with runawayTau) -> ROOMSCALE FAULT. A fault disables requests until ROOMSCALE is switched OFF and ON.
    // faultMetres: the same streak also faults once the capsule has moved this far without the lean shrinking (bounds the
    // excursion of a wrong-way mover at any frame rate: 15 frames at 29 fps x 5 cm would be 0.75 m).
    unsigned faultFrames=15;float faultMetres=0.12f,faultMinMotion=0.003f,runawayTau=2.f,runawayMargin=0.6f;
    // Requests going out this long with no LOCO record carrying recordApplied+headId (gates LOCKED/SCRIPTED/COVER/LADDER,
    // ragdoll, pause) = the capsule does not follow: lean limit back to maxLeanOff, status "NO LOCO". The limit shrinks back
    // to maxLeanOn at leanCapRate (no view jump from a sudden push-back when consumption resumes or ROOMSCALE is switched ON).
    std::uint64_t consumeTimeoutNs=300'000'000;float leanCapRate=0.5f;
    float neckBack=0.09f,neckDown=0.10f;
    std::uint64_t holdNs=500'000'000; // B survives anchor losses shorter than this
    std::uint64_t recordMaxAgeNs=100'000'000;
};

struct Input {
    std::uint64_t now{};
    Settings settings{};
    bool sampled{};                  // HeadAnchorReader::sample returned true this PostCameraUpdate
    BodyAnchorFacts facts{};
    bool viewsValid{};runtime::TrackedViews views{}; // TrackedViewsMailbox::snapshot(now) on the main thread
    bool psiValid{};float psi{};std::uint64_t psiOrigin{}; // VisibilityCameraBridge::worldYaw(now, psi, psiOrigin)
    bool recordValid{};body::FrameRecord record{};     // body::latestRecord(record) (LOCO)
};

class Rig {
public:
    enum class Relatch : unsigned {Initial,Recenter,Actor,Loss,Count};
private:
    struct Head {Vec3 eye{},neck{};Quat q{0,0,0,1};float latchYaw{};Epochs epochs{};bool valid{};};
    struct Pending {std::uint32_t id{};std::uint64_t serial{};};
    Tuning t_{};
    mutable std::mutex mutex_; // main thread (apply) vs report writer only; never taken on the render thread
    // tracking origin
    TrackingOrigin origin_{};std::array<float,2> b_{};std::array<float,2> eyeLatch_{};std::uint64_t serialCounter_{};
    bool relatchPending_{};Relatch relatchReason_{Relatch::Initial};
    std::array<std::uint64_t,unsigned(Relatch::Count)> relatches_{};
    std::uint32_t actorGuid_{};std::uint64_t lossNs_{},lastNs_{},lastApplyNs_{};
    // eye height
    bool calibrated_{},bodySource_{},recalibrating_{};std::uint64_t sourceChanges_{};float calibration_{};float stillFor_{},firstSum_{},firstTime_{};unsigned firstCount_{};
    std::uint64_t calibrationSamples_{};bool hasPrevC_{};Vec3 prevC_{};
    // ragdoll filter
    bool filterInit_{},recovering_{};float recoverT_{};Vec3 filtered_{};std::uint64_t ragdollFiltered_{};
    // roomscale
    std::array<Pending,8> pending_{};std::uint32_t lastRecordFrame_{};
    std::array<float,2> lean_{},request_{},achieved_{};std::uint32_t requestId_{};
    std::uint64_t requests_{},pushBacks_{},accounted_{},rejected_{},missingRequest_{},staleRecords_{},recordGaps_{};
    float achievedSum_{};bool pushBackFrame_{},requestOutstanding_{};
    std::uint64_t requestSinceNs_{},lastConsumedNs_{},locoIdleFrames_{};float leanCap_{-1.f};
    // ownership / limits / watchdog
    bool active_{};float budget_{-1.f};float stickQuiet_{};float speed_{};bool hasLastC_{};std::array<float,2> lastC_{};
    std::uint64_t ownedUntilNs_{},ownedFrames_{},stickFrames_{},lastRecordNs_{};float requestedSum_{};
    bool fault_{},roomSettingPrev_{};const char* faultReason_{""};std::uint64_t faults_{},rearms_{},badFrames_{};unsigned badStreak_{};
    float badMetres_{};
    float creditedAcc_{},neckAcc_{};bool hasPrevNeck_{};std::array<float,2> prevNeck_{};bool hasPrevPsi_{};float prevPsi_{};
    // World-yaw (psi = stock camera heading) drift between applies. With the CAMERA YAW HOLD (VrYawHold.h) psi only
    // changes by snap/teleport turns (jumps); a steady drift while the stick moves the capsule = the follow camera turning the
    // VR world.
    float psiDriftDeg_{},psiDriftStickDeg_{},psiDriftMaxDegPerSec_{},psiStickSeconds_{};std::uint64_t psiJumps_{},psiDriftFrames_{};
    // report
    std::uint64_t frames_{},samples_{},lost_{},headRejects_{},yawPublished_{};
    const char* anchor_{"NONE"};const char* reason_{""};const char* eyeSource_{"AUTO_DEFAULT"};const char* roomState_{"OFF"};
    float eyeHeight_{};Vec3 c_{};float headYaw_{};bool psiValid_{},ragdoll_{},onFoot_{},sim_{},enabled_{true};Mode mode_{Mode::Body};
    // TELEPORT: the warp the rig snapped (filter + B), its head key K, psi before it, applies since
    std::uint64_t warpHandled_{},warpSerial_{},warpKey_{},warpNs_{},warpSnaps_{},warpExpired_{};std::uint32_t warpFrames_{};float psiBeforeWarp_{};
    bool warpBSnapped_{};std::array<float,3> bSnapWorld_{};

    static float clampf(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}
    static float wrapPi(float a){while(a>3.14159265f)a-=6.28318531f;while(a<-3.14159265f)a+=6.28318531f;return a;}
    // world = R_y(psi) * local for horizontal vectors (R_y(psi) maps LOCAL +Z to (sin psi, cos psi)).
    static std::array<float,2> toWorld(float psi,float x,float z){const float s=std::sin(psi),c=std::cos(psi);return {x*c+z*s,-x*s+z*c};}
    static std::array<float,2> toLocal(float psi,float x,float z){const float s=std::sin(psi),c=std::cos(psi);return {x*c-z*s,x*s+z*c};}
    bool headFrom(const runtime::TrackedViews& v,Head& h) const {
        if(!rdrvr::validPose(v.eyes[0])||!rdrvr::validPose(v.eyes[1]))return false;
        float sign{},norm{};Quat q{};
        for(unsigned j=0;j<4;++j)sign+=v.eyes[0].orientation[j]*v.eyes[1].orientation[j];
        for(unsigned j=0;j<4;++j){q[j]=v.eyes[0].orientation[j]+(sign<0?-1.f:1.f)*v.eyes[1].orientation[j];norm+=q[j]*q[j];}
        norm=std::sqrt(norm);if(!std::isfinite(norm)||norm<1e-3f)return false;
        for(float& f:q)f/=norm;
        for(unsigned j=0;j<3;++j)h.eye[j]=(v.eyes[0].position[j]+v.eyes[1].position[j])*.5f;
        const auto forward=runtime::rotate(q,{0,0,-1});
        const float horizontal=std::hypot(forward[0],forward[2]);
        if(!std::isfinite(horizontal)||horizontal<0.0316f)return false; // same 0.001 squared limit as CameraAdapter
        // Rigid neck pivot (see Tuning::neckDown). Only its XZ is used (B, lean); Y stays the eye height.
        const auto pivot=runtime::rotate(q,{0,-t_.neckDown,t_.neckBack});
        h.neck=h.eye;h.neck[0]+=pivot[0];h.neck[2]+=pivot[2];
        h.latchYaw=std::atan2(-forward[0],-forward[2]);h.q=q;h.epochs=v.epochs;h.valid=true;
        for(float f:h.eye)if(!std::isfinite(f))return false;
        for(float f:h.neck)if(!std::isfinite(f))return false;
        return true;
    }
    void latch(const Head& h,Relatch why){
        b_={h.neck[0],h.neck[2]};eyeLatch_={h.eye[0],h.eye[2]};
        origin_.position={h.neck[0],h.eye[1],h.neck[2]};
        origin_.inverseYaw={0,-std::sin(h.latchYaw/2),0,std::cos(h.latchYaw/2)};
        origin_.referenceSpace=h.epochs.referenceSpace;origin_.serial=++serialCounter_;origin_.valid=true;
        pending_={};lean_={};request_={};relatchPending_=false;++relatches_[unsigned(why)];
        // A new LOCAL frame starts idle; nothing measured in the old one is credited into the new one.
        active_=false;ownedUntilNs_=0;badStreak_=0;badMetres_=0;creditedAcc_=neckAcc_=0;hasPrevNeck_=false;
    }
    // A request LOCO has not consumed yet must not move the capsule later (gated frames, menus): replace it by a zero one.
    void cancelRequest(std::uint64_t now){
        if(!requestOutstanding_)return;requestOutstanding_=false;
        if(const auto id=body::requestHead(0,0,now))pending_[id%pending_.size()]={id,origin_.serial};
    }
    void markRelatch(Relatch why){if(!relatchPending_){relatchPending_=true;relatchReason_=why;}}
    void setFault(const char* why,std::uint64_t now){
        if(fault_)return;
        fault_=true;faultReason_=why;++faults_;active_=false;ownedUntilNs_=0;cancelRequest(now);
    }
    // Lag-agnostic accounting. The capsule's TOTAL measured XZ motion between two applies is credited to B while
    // roomscale owns the capsule (the rig requested within trailNs, the new LOCO record says LOCO owned
    // the XZ and no stick was commanded). Where in the frame the engine realises a command (on the
    // device: one substep later) does not matter, and every bit of motion the rig caused is credited
    // exactly once, in the frame the capsule shows it.
    void credit(const Input& in,const Head& h,bool hmd,float dt){
        const auto& f=in.facts;
        const std::array<float,2> c{f.capsule[0],f.capsule[2]};
        std::array<float,2> d{};bool haveD=false;
        if(hasLastC_&&std::isfinite(c[0])&&std::isfinite(c[1])){d={c[0]-lastC_[0],c[1]-lastC_[1]};haveD=std::isfinite(d[0])&&std::isfinite(d[1]);}
        lastC_=c;hasLastC_=std::isfinite(c[0])&&std::isfinite(c[1]);
        speed_=haveD&&dt>0?std::hypot(d[0],d[1])/dt:0.f;
        if(f.velocityKnown&&std::isfinite(f.velocity[0])&&std::isfinite(f.velocity[2]))speed_=std::max(speed_,std::hypot(f.velocity[0],f.velocity[2]));
        bool owned=false;
        if(in.recordValid&&in.record.frame&&in.record.frame!=lastRecordFrame_){
            const auto& r=in.record;
            const bool contiguous=!lastRecordFrame_||r.frame==lastRecordFrame_+1;
            if(!contiguous)++recordGaps_;
            lastRecordFrame_=r.frame;
            // The record must belong to the physics of THIS frame (after the previous apply).
            const bool fresh=r.ns&&r.ns>lastApplyNs_&&!(in.now>r.ns&&in.now-r.ns>t_.recordMaxAgeNs);
            if(!fresh)++staleRecords_;else lastRecordNs_=in.now;
            if(r.flags&body::recordStick){stickQuiet_=0;++stickFrames_;}else stickQuiet_+=dt;
            if(fresh&&(r.flags&body::recordApplied)&&r.headId){
                lastConsumedNs_=in.now; // LOCO applied a head part this physics frame
                bool ours=false;for(const auto& e:pending_)if(e.id==r.headId&&e.serial==origin_.serial)ours=true;
                if(!ours)++missingRequest_;
            }
            owned=fresh&&contiguous&&(r.flags&body::recordApplied)&&!(r.flags&body::recordStick)&&ownedUntilNs_&&in.now<=ownedUntilNs_;
        }
        if(!owned||!haveD)return;
        if(!origin_.valid||mode_!=Mode::Body||!psiValid_||!f.onFoot()){ownedUntilNs_=0;active_=false;return;}
        const float m=std::hypot(d[0],d[1]);
        if(!std::isfinite(m)||m>t_.maxCreditPerFrame){++rejected_;ownedUntilNs_=0;active_=false;return;}
        const float lb=hmd?std::hypot(h.neck[0]-b_[0],h.neck[2]-b_[1]):-1.f;
        const auto local=toLocal(in.psi,d[0],d[1]);
        b_[0]+=local[0];b_[1]+=local[1];achieved_=d;achievedSum_+=m;++accounted_;++ownedFrames_;creditedAcc_+=m;
        if(hmd){
            const float la=std::hypot(h.neck[0]-b_[0],h.neck[2]-b_[1]);
            const bool snap=hasPrevPsi_&&std::fabs(wrapPi(in.psi-prevPsi_))>0.1f; // snap turn this frame: direction unreliable
            if(!snap&&m>=t_.faultMinMotion&&lb>=t_.stopLean){
                // Correct motion toward the target shrinks the lean by ~m (wall slides by m*cos(angle)); the device fault
                // (credit with the wrong sign) grows it by m. Only "no shrink at all" counts; tiny slides (< 3 mm) never do.
                if(la>=lb-0.02f*m){++badFrames_;badMetres_+=m;
                    if(++badStreak_>=t_.faultFrames||badMetres_>=t_.faultMetres)setFault("NO CONVERGENCE",in.now);}
                else{badStreak_=0;badMetres_=0;}
            }
        }
    }
    float eyeHeight(const Input& in,float dt){
        const auto& f=in.facts;
        float speed=1e9f;
        if(f.velocityKnown)speed=std::hypot(f.velocity[0],f.velocity[2]);
        if(hasPrevC_&&dt>0){const float moved=std::hypot(f.capsule[0]-prevC_[0],f.capsule[2]-prevC_[2])/dt;
            speed=f.velocityKnown?std::max(speed,moved):moved;}
        prevC_=f.capsule;hasPrevC_=true;
        float sample=f.headValid?f.head[1]-f.capsule[1]:0.f;bool headOk=f.headValid;
        // BODY: while the player's head is collapsed for the eyes (VrBody hook C) the native head query returns the collapsed
        // head, about 11 cm too low for the eyes. Then the sample is VrBody's
        // animated head joint above the skeleton root (= the capsule base) + BODY OFFSET UP (eye above the head joint); no native
        // samples at all while collapsing without it. A source change restarts the calibration (1 s mean, no 3 s drift).
        bool bodySource=false;
        if(status::bodyCollapsing.load(std::memory_order_relaxed)&&mode_==Mode::Body){
            const int mm=status::bodyHeadMm.load(std::memory_order_relaxed);
            if(mm>0){sample=float(mm)*.001f+in.settings.bodyEyeUp;headOk=true;bodySource=true;}else headOk=false;
        }
        if(bodySource!=bodySource_){bodySource_=bodySource;recalibrating_=true;stillFor_=0;firstSum_=0;firstTime_=0;firstCount_=0;++sourceChanges_;}
        const bool still=f.onFoot()&&headOk&&std::isfinite(speed)&&speed<t_.stillSpeed&&
            std::isfinite(sample)&&sample>=t_.minSample&&sample<=t_.maxSample;
        if(!still){stillFor_=0;firstSum_=0;firstTime_=0;firstCount_=0;}
        else{
            stillFor_+=dt;
            if(stillFor_>=t_.stillSeconds&&dt>0){
                const float s=clampf(sample,t_.minEyeHeight,t_.maxEyeHeight);
                if(!calibrated_||recalibrating_){firstSum_+=s;firstTime_+=dt;++firstCount_; // A source change re-runs the 1 s mean (old value kept meanwhile)
                    if(firstTime_>=t_.firstCalibrationSeconds&&firstCount_){calibration_=firstSum_/float(firstCount_);calibrated_=true;recalibrating_=false;}}
                else calibration_+=(s-calibration_)*(1-std::exp(-dt/t_.calibrationTau));
                ++calibrationSamples_;
            }
        }
        status::eyeHeightMm.store(calibrated_?int(std::lround(calibration_*1000)):-1,std::memory_order_relaxed);
        if(in.settings.eyeHeight>0){eyeSource_="FIXED";return clampf(in.settings.eyeHeight,t_.minEyeHeight,t_.maxEyeHeight);}
        eyeSource_=calibrated_?(bodySource_?"AUTO_BODY":"AUTO"):"AUTO_DEFAULT";
        return calibrated_?clampf(calibration_,t_.minEyeHeight,t_.maxEyeHeight):t_.defaultEyeHeight;
    }
    Vec3 filter(const Vec3& target,bool ragdoll,float dt){
        float d2{};for(unsigned i=0;i<3;++i)d2+=(target[i]-filtered_[i])*(target[i]-filtered_[i]);
        if(!filterInit_||!std::isfinite(d2)||d2>t_.snapDistance*t_.snapDistance){filtered_=target;filterInit_=true;recovering_=false;return filtered_;}
        float tau{};
        if(ragdoll){tau=t_.ragdollTau;recovering_=true;recoverT_=0;}
        else if(recovering_){recoverT_+=dt;tau=t_.ragdollTau*(1-recoverT_/t_.recoverySeconds); // shrink the lag back to 0
            if(tau<=0.005f){recovering_=false;tau=0;}}
        if(tau<=0){filtered_=target;return filtered_;}
        const float a=dt>0?1-std::exp(-dt/tau):0.f;
        for(unsigned i=0;i<3;++i)filtered_[i]+=(target[i]-filtered_[i])*a;
        ++ragdollFiltered_;return filtered_;
    }
    void publishStatus(){
        using namespace status;
        if(!enabled_){publish(Feature::Camera,Property,nullptr);publish(Feature::Roomscale,Property,nullptr);return;}
        const char* cam=ragdoll_?"RAGDOLL":std::string_view(anchor_)=="NONE"?"NO ANCHOR":mode_==Mode::Head?nullptr:
            std::string_view(reason_).empty()?"ACTIVE":"FALLBACK";
        publish(Feature::Camera,Ready,cam);
        if(!moverSimVtable.load(std::memory_order_relaxed))publish(Feature::Roomscale,Unavailable,nullptr); // fingerprint failed
        else publish(Feature::Roomscale,Ready,std::string_view(roomState_)=="OFF"?nullptr:roomState_);
    }
    // TELEPORT: the first apply whose capsule moved >= min(0.5 m, 0.6 x the warp) from the pre-warp C (<= 500 ms
    // after the note) snaps the ragdoll filter (no glide), forgets the previous capsule (no credit / speed), cancels any roomscale request,
    // records the warp head key K (= anchor.sampledNs: the render thread's pair identity) and psi before the warp, and (BODY, HMD, .bsnap)
    // moves B under the current neck (no yaw change) -> the eye stands above the ring; bSnapWorld = the world shift that B change causes.
    // While the warp is fresh (1 s) every apply publishes psi for the teleport HOLD (stable / correction).
    void noteWarp(const Input& in,const Head& h,bool hmd,const HeadCameraAnchor& anchor){
        tploco::warp::Note wn{};
        if(tploco::warp::note.load(wn)&&wn.serial&&wn.serial!=warpHandled_){
            if(in.now>wn.ns&&in.now-wn.ns>500'000'000ull){warpHandled_=wn.serial;++warpExpired_;tploco::linkBump(tploco::warp::cameraSnapExpired);}
            else{
                const auto& c=in.facts.capsule;float d2=0.f,t2=0.f,l2=0.f;
                for(unsigned i=0;i<3;++i){const float e=c[i]-wn.from[i],t=e-wn.delta[i];d2+=e*e;t2+=t*t;l2+=wn.delta[i]*wn.delta[i];}
                const float d=std::sqrt(d2),dTarget=std::sqrt(t2),dl=std::sqrt(l2);
                // The validator accepts hops from 0.30 m (`.limits` minCm), which a fixed 0.5 m would never snap (no B snap, no
                // key K -> pair-identity fallback, snapExpired). The threshold is 0.6 x the note's own warp length within [0.10, 0.50]; below
                // 0.5 m the capsule must also be nearer the target than the old place (a pre-warp sample with roomscale drift never snaps).
                const float need=std::isfinite(dl)?std::clamp(.6f*dl,.10f,.5f):.5f;
                if(std::isfinite(d)&&d>=need&&(d>=.5f||(std::isfinite(dTarget)&&dTarget<d))){
                    warpHandled_=wn.serial;warpSerial_=wn.serial;warpKey_=anchor.sampledNs;warpNs_=in.now;warpFrames_=0;
                    psiBeforeWarp_=hasPrevPsi_?prevPsi_:in.psi;
                    filterInit_=false;recovering_=false;hasLastC_=false;hasPrevC_=false;
                    cancelRequest(in.now);active_=false;ownedUntilNs_=0;badStreak_=0;badMetres_=0;creditedAcc_=neckAcc_=0;hasPrevNeck_=false;
                    warpBSnapped_=false;bSnapWorld_={};
                    if(wn.bsnap&&mode_==Mode::Body&&hmd&&origin_.valid&&psiValid_){
                        const std::array<float,2> old=b_;b_={h.neck[0],h.neck[2]};lean_={};
                        const auto w=toWorld(in.psi,old[0]-b_[0],old[1]-b_[1]);bSnapWorld_={w[0],0.f,w[1]};warpBSnapped_=true;
                    }
                    ++warpSnaps_;tploco::linkBump(tploco::warp::cameraSnaps);
                    tploco::fade::warpKey.store(warpKey_,std::memory_order_release); // before headCamera.publish(anchor) of this apply
                }
            }
        }
        if(warpSerial_&&in.now>=warpNs_&&in.now-warpNs_<=1'000'000'000ull){
            tploco::warp::CameraResult r{};r.serial=warpSerial_;r.key=warpKey_;r.psiNs=in.now;r.psiBefore=psiBeforeWarp_;r.psiNow=psiValid_?in.psi:psiBeforeWarp_;
            r.psiValid=psiValid_;r.frames=++warpFrames_;r.snapped=1;r.bSnapped=warpBSnapped_;for(unsigned i=0;i<3;++i)r.bSnap[i]=bSnapWorld_[i];
            tploco::warp::camera.store(r);
        }
    }
public:
    explicit Rig(Tuning t={}):t_(t){}
    const Tuning& tuning() const {return t_;}
    bool faulted() const {std::lock_guard lock(mutex_);return fault_;}
    // Main thread, once per PostCameraUpdate, after HeadAnchorReader::sample (anchor = its output, in.sampled = its result).
    // Rewrites anchor.position/up/origin/kind. Returns false when nothing may be published (caller invalidates the mailbox).
    bool apply(const Input& in,HeadCameraAnchor& anchor){
        std::lock_guard lock(mutex_);
        const float dt=lastNs_&&in.now>lastNs_?std::min(float(double(in.now-lastNs_)*1e-9),0.1f):0.f;lastNs_=in.now;++frames_;
        enabled_=in.settings.enabled;mode_=in.settings.mode;pushBackFrame_=false;reason_="";
        Head h{};const bool hmd=in.viewsValid&&headFrom(in.views,h);
        psiValid_=in.psiValid&&std::isfinite(in.psi);
        // headYawWorld for LOCO: the rendered view's yaw, whatever latch the render used (psi is that plan's mapping).
        if(hmd&&psiValid_){
            const auto f=runtime::rotate(h.q,{0,0,-1});const auto w=toWorld(in.psi,f[0],f[2]);
            if(std::hypot(w[0],w[1])>0.1f){headYaw_=std::atan2(w[0],w[1]);body::publishHeadYaw(headYaw_,in.now);++yawPublished_;}
        }
        if(in.settings.roomscale&&!roomSettingPrev_&&fault_){fault_=false;faultReason_="";badStreak_=0;badMetres_=0;creditedAcc_=neckAcc_=0;++rearms_;}
        roomSettingPrev_=in.settings.roomscale;
        auto finish=[&](bool result){
            if(psiValid_&&hasPrevPsi_){ // Psi drift statistics
                const float d=std::fabs(wrapPi(in.psi-prevPsi_))*57.2957795f;
                if(d>5.7f)++psiJumps_;
                else if(std::isfinite(d)){
                    psiDriftDeg_+=d;if(d>0.f)++psiDriftFrames_;
                    if(stickQuiet_<=0.f&&dt>0.f){psiDriftStickDeg_+=d;psiStickSeconds_+=dt;}
                    if(dt>0.f&&d/dt>psiDriftMaxDegPerSec_)psiDriftMaxDegPerSec_=d/dt;
                }
            }
            lastApplyNs_=in.now;if(psiValid_){prevPsi_=in.psi;hasPrevPsi_=true;}
            publishStatus();return result;};
        if(!enabled_){anchor_="DISABLED";roomState_="OFF";anchor.origin={};anchor.kind=HeadCameraAnchor::Kind::LegacyHead;
            active_=false;ownedUntilNs_=0;hasLastC_=false;
            cancelRequest(in.now);if(in.sampled)++samples_;else ++lost_;return finish(in.sampled);}
        if(!in.sampled||!in.facts.valid){
            ++lost_;anchor_="NONE";roomState_=fault_?"FAULT":"PAUSED";ragdoll_=onFoot_=false;
            if(!lossNs_)lossNs_=in.now;
            if(in.now-lossNs_>t_.holdNs)markRelatch(Relatch::Loss);
            active_=false;ownedUntilNs_=0;hasLastC_=false; // the capsule of the next sample is not comparable
            cancelRequest(in.now);return finish(false);
        }
        if(lossNs_&&in.now-lossNs_>t_.holdNs)markRelatch(Relatch::Loss);
        lossNs_=0;++samples_;
        if(actorGuid_&&anchor.actorGuid!=actorGuid_){markRelatch(Relatch::Actor);calibrated_=false;filterInit_=false;hasPrevC_=false;hasLastC_=false;}
        actorGuid_=anchor.actorGuid;
        if(hmd){
            if(!origin_.valid)latch(h,Relatch::Initial);
            else if(origin_.referenceSpace!=h.epochs.referenceSpace)latch(h,Relatch::Recenter);
            else if(relatchPending_)latch(h,relatchReason_);
        }
        const auto& f=in.facts;ragdoll_=f.ragdoll;onFoot_=f.onFoot();sim_=f.simKnown;c_=f.capsule;
        noteWarp(in,h,hmd,anchor);                     // TELEPORT: before credit (a warp is never credited to B)
        credit(in,h,hmd,dt);
        eyeHeight_=eyeHeight(in,dt);
        if(mode_==Mode::Head){
            if(!f.headValid){++headRejects_;anchor_="NONE";reason_="NO_HEAD_BONE";active_=false;cancelRequest(in.now);return finish(false);}
            anchor.position=f.head;anchor.up=f.up;anchor.kind=HeadCameraAnchor::Kind::Head;anchor_="HEAD";filterInit_=false;
        }else{
            const Vec3 target{f.capsule[0],f.capsule[1]+eyeHeight_,f.capsule[2]};
            const bool filtering=f.ragdoll||recovering_;
            anchor.position=filter(target,f.ragdoll,dt);anchor.up={0,1,0};
            anchor.kind=filtering?HeadCameraAnchor::Kind::BodyFiltered:HeadCameraAnchor::Kind::Body;
            anchor_="BODY";
            reason_=f.ragdoll?"RAGDOLL_FILTERED":filtering?"RAGDOLL_RECOVERY":!f.moverMatrix?"ACTOR_MATRIX_NOT_MOVER":
                !f.simKnown?"SIM_MOVER_UNKNOWN":"";
        }
        // Runaway guard: the credited motion must be explained by the neck's own travel (decaying sums).
        {const float k=dt>0?std::exp(-dt/t_.runawayTau):1.f;creditedAcc_*=k;neckAcc_*=k;
         if(hmd){if(hasPrevNeck_){const float n=std::hypot(h.neck[0]-prevNeck_[0],h.neck[2]-prevNeck_[1]);if(std::isfinite(n)&&n<0.5f)neckAcc_+=n;}
             prevNeck_={h.neck[0],h.neck[2]};hasPrevNeck_=true;}
         if(!fault_&&creditedAcc_>neckAcc_+t_.runawayMargin)setFault("RUNAWAY",in.now);}
        // Lean, push-back and the roomscale request (BODY only; HEAD keeps the legacy fixed recenter point).
        // Roomscale only counts when LOCO can consume it (settings.locoReady) and does (watchdog); otherwise the
        // lean limit is the ROOMSCALE OFF one - a 0.30 m cage with a capsule that never follows is worse than OFF.
        const bool roomWanted=in.settings.roomscale&&in.settings.locoReady&&!fault_;
        // Also idle when LOCO publishes no fresh physics records at all (mover not stepped: the requests could never
        // be consumed, and without records the stick state is unknown so none are sent).
        const bool noRecords=!lastRecordNs_||(in.now>lastRecordNs_&&in.now-lastRecordNs_>t_.consumeTimeoutNs);
        if(noRecords)stickQuiet_=0; // stick state unknown without records: settle again on fresh ones before any request
        const bool locoIdle=roomWanted&&(noRecords||(requestSinceNs_&&in.now>requestSinceNs_&&
            in.now-std::max(requestSinceNs_,lastConsumedNs_)>t_.consumeTimeoutNs));
        if(locoIdle)++locoIdleFrames_;
        const float capTarget=roomWanted&&!locoIdle?t_.maxLeanOn:t_.maxLeanOff;
        if(!(leanCap_>0.f)||capTarget>=leanCap_)leanCap_=capTarget;
        else leanCap_=std::max(capTarget,leanCap_-t_.leanCapRate*(dt>0?dt:0.011f));
        // Request budget (hard per-second limit): refills at maxRequestSpeed, holds at most one second of it.
        if(budget_<0.f)budget_=t_.maxRequestPerFrame;
        budget_=std::min(t_.maxRequestSpeed,budget_+t_.maxRequestSpeed*dt);
        request_={};roomState_=!in.settings.roomscale?"OFF":fault_?"FAULT":!in.settings.locoReady?"NO LOCO":locoIdle?"NO LOCO":"PAUSED";
        bool requested=false;
        if(origin_.valid&&hmd&&mode_==Mode::Body){
            float lx=h.neck[0]-b_[0],lz=h.neck[2]-b_[1];
            const float maxLean=leanCap_,len=std::hypot(lx,lz);
            if(std::isfinite(len)&&len>maxLean){const float k=1-maxLean/len;b_[0]+=lx*k;b_[1]+=lz*k;lx*=1-k;lz*=1-k;
                ++pushBacks_;pushBackFrame_=true;}
            lean_={lx,lz};
            // Only while the render composes THIS origin (psiOrigin) and the mover is free (LOCO gates again).
            const bool eligible=roomWanted&&f.onFoot()&&psiValid_&&in.psiOrigin==origin_.serial;
            if(eligible&&!locoIdle)roomState_=pushBackFrame_?"BLOCKED":"ACTIVE";
            // The capsule follows the head only with a neutral stick (settled) and from (near) standstill; the stick's own
            // motion is never mixed with a head part (the view simply follows the capsule while the stick moves it).
            const bool settled=stickQuiet_>=t_.stickSettleSeconds&&(active_||speed_<=t_.settleSpeed);
            const float l=std::hypot(lx,lz);
            if(!eligible||!settled||!std::isfinite(l))active_=false;
            else if(!active_&&l>=t_.startLean)active_=true;
            else if(active_&&l<t_.stopLean)active_=false;
            if(active_){
                auto w=toWorld(in.psi,lx*t_.gain,lz*t_.gain);float wl=std::hypot(w[0],w[1]);
                const float frameLimit=std::min(t_.maxRequestPerFrame,t_.maxRequestSpeed*std::max(dt,1.f/90.f));
                const float lim=std::min(frameLimit,budget_);
                if(wl>lim&&wl>0){w[0]*=lim/wl;w[1]*=lim/wl;wl=lim;}
                if(wl>1e-4f){
                    if(const auto id=body::requestHead(w[0],w[1],in.now)){
                        budget_-=wl;requestedSum_+=wl;requestId_=id;request_=w;++requests_;pending_[id%pending_.size()]={id,origin_.serial};
                        requested=requestOutstanding_=true;ownedUntilNs_=in.now+t_.trailNs;}
                }
            }
        }else active_=false;
        if(!requested)cancelRequest(in.now);
        if(!requested)requestSinceNs_=0;else if(!requestSinceNs_)requestSinceNs_=in.now;
        if(origin_.valid){
            if(mode_==Mode::Body)origin_.position={b_[0],origin_.position[1],b_[1]};
            else origin_.position={eyeLatch_[0],origin_.position[1],eyeLatch_[1]};
            anchor.origin=origin_;
        }else{anchor.origin={};if(!hmd)reason_="NO_TRACKING_ORIGIN";}
        return finish(true);
    }
    std::string report() const {
        std::lock_guard lock(mutex_);std::ostringstream o;o<<std::fixed<<std::setprecision(3);
        const auto num=[&](float v)->std::ostringstream&{if(std::isfinite(v))o<<v;else o<<"null";return o;};
        const auto vec=[&](std::initializer_list<float> v){o<<'[';bool first=true;for(float x:v){if(!first)o<<',';first=false;num(x);}o<<']';};
        o<<"{\"enabled\":"<<(enabled_?"true":"false")<<",\"mode\":\""<<(mode_==Mode::Body?"BODY":"HEAD")<<"\",\"anchor\":\""<<anchor_
         <<"\",\"reason\":\""<<reason_<<"\",\"eyeHeight\":";num(eyeHeight_);
        o<<",\"eyeHeightSource\":\""<<eyeSource_<<"\",\"calibrated\":"<<(calibrated_?"true":"false")<<",\"calibration\":";num(calibration_);
        o<<",\"calibrationSamples\":"<<calibrationSamples_<<",\"bodySource\":{\"active\":"<<(bodySource_?"true":"false")<<",\"recalibrating\":"
         <<(recalibrating_?"true":"false")<<",\"changes\":"<<sourceChanges_<<",\"headMm\":"<<status::bodyHeadMm.load(std::memory_order_relaxed)<<'}';
        o<<",\"C\":";vec({c_[0],c_[1],c_[2]});
        o<<",\"lean\":";vec({lean_[0],lean_[1]});o<<",\"B\":";vec({b_[0],origin_.position[1],b_[1]});
        o<<",\"origin\":{\"valid\":"<<(origin_.valid?"true":"false")<<",\"serial\":"<<origin_.serial<<",\"referenceSpace\":"<<origin_.referenceSpace
         <<",\"relatch\":{\"initial\":"<<relatches_[0]<<",\"recenter\":"<<relatches_[1]<<",\"actor\":"<<relatches_[2]<<",\"loss\":"<<relatches_[3]<<"}}";
        o<<",\"pushBack\":{\"total\":"<<pushBacks_<<",\"thisFrame\":"<<(pushBackFrame_?1:0)<<"},\"roomscale\":\""<<roomState_<<'"';
        o<<",\"leanCap\":";num(leanCap_);o<<",\"locoIdleFrames\":"<<locoIdleFrames_;
        o<<",\"requestHead\":{\"id\":"<<requestId_<<",\"xz\":";vec({request_[0],request_[1]});o<<",\"count\":"<<requests_
         <<",\"sumMetres\":";num(requestedSum_);o<<",\"active\":"<<(active_?"true":"false")<<",\"budget\":";num(budget_);o<<'}';
        o<<",\"achievedHead\":{\"xz\":";vec({achieved_[0],achieved_[1]});o<<",\"sumMetres\":";num(achievedSum_);
        o<<",\"accounted\":"<<accounted_<<",\"rejected\":"<<rejected_<<",\"missingRequest\":"<<missingRequest_
         <<",\"staleRecords\":"<<staleRecords_<<",\"recordGaps\":"<<recordGaps_<<"}";
        // ownership / stick / watchdog
        o<<",\"ownedFrames\":"<<ownedFrames_<<",\"stickFrames\":"<<stickFrames_<<",\"stickQuiet\":";num(stickQuiet_);
        o<<",\"capsuleSpeed\":";num(speed_);
        o<<",\"fault\":{\"active\":"<<(fault_?"true":"false")<<",\"reason\":\""<<faultReason_<<"\",\"count\":"<<faults_
         <<",\"rearms\":"<<rearms_<<",\"badFrames\":"<<badFrames_<<",\"badStreak\":"<<badStreak_<<",\"credited2s\":";num(creditedAcc_);
        o<<",\"neckTravel2s\":";num(neckAcc_);o<<'}';
        o<<",\"ragdollFiltered\":"<<ragdollFiltered_<<",\"ragdoll\":"<<(ragdoll_?"true":"false")<<",\"onFoot\":"<<(onFoot_?"true":"false")
         <<",\"simMover\":"<<(sim_?"true":"false")<<",\"psiValid\":"<<(psiValid_?"true":"false")<<",\"headYawWorld\":";num(headYaw_);
        o<<",\"psiDrift\":{\"deg\":";num(psiDriftDeg_);o<<",\"degWhileStick\":";num(psiDriftStickDeg_);
        o<<",\"degPerSecWhileStick\":";num(psiStickSeconds_>0.f?psiDriftStickDeg_/psiStickSeconds_:0.f);
        o<<",\"maxDegPerSec\":";num(psiDriftMaxDegPerSec_);o<<",\"driftFrames\":"<<psiDriftFrames_<<",\"jumps\":"<<psiJumps_<<'}';
        o<<",\"warpSnaps\":"<<warpSnaps_<<",\"warpSnapExpired\":"<<warpExpired_<<",\"warpBSnapped\":"<<(warpBSnapped_?"true":"false")
         <<",\"bSnapWorld\":";vec({bSnapWorld_[0],bSnapWorld_[1],bSnapWorld_[2]});
        o<<",\"yawPublished\":"<<yawPublished_<<",\"frames\":"<<frames_<<",\"samples\":"<<samples_<<",\"lost\":"<<lost_
         <<",\"headRejects\":"<<headRejects_<<'}';
        return o.str();
    }
};
// The one rig (GameXr owns the call sites; header-only like the other vr modules).
inline Rig rig;

} // namespace rdrvr::vr::camera
