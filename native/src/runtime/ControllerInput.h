#pragma once
#include "contracts/Contracts.h"
#include "runtime/Presentation.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace rdrvr::runtime {
enum PadButton : std::uint32_t {
    PadA=1u<<0,PadB=1u<<1,PadX=1u<<2,PadY=1u<<3,PadMenu=1u<<4,
    PadLeftStick=1u<<5,PadRightStick=1u<<6,PadLeftGrip=1u<<7,PadRightGrip=1u<<8,
    PadLeftTrigger=1u<<9,PadRightTrigger=1u<<10,
    PadDpadUp=1u<<11, // PadState output only (Android key 19 -> d11 pad 10 = WHISTLE); never in a ControllerSample
    PadSelect=1u<<12  // PadState output only (Android key 109 BUTTON_SELECT -> d11 pad 13 = INVENTORY); never in a ControllerSample
};
struct ControllerSample {
    std::uint64_t sequence{},publishedNs{},epoch{},continuity{},referenceSpace{};
    std::uint32_t buttons{};
    std::array<float,4> sticks{}; // left X/Y, right X/Y; OpenXR positive Y = up
    std::array<float,2> triggers{},grips{};
    std::array<Pose,2> aim{},grip{};
    bool focused{},active{};
    // Separate visual epoch retirement (World <-> Optic) from focus/recenter
    // invalidation. Zero = no separate visual epoch (the ordinary epoch rules alone).
    std::uint64_t inputEpoch{};
    // Bit h = the manual-theater grip chord zeroed grip h of this sample while it was squeezed (runtime/HoldChord.h).
    std::uint8_t chordGrips{};
};
struct PadState {
    std::uint32_t buttons{};
    std::array<float,4> sticks{};
    std::array<float,2> triggers{};
    bool ready{};
    // Inputs held when the pad armed on a gameplay-camera change, sent as released until they read at rest once. By physical hand:
    // bit 0 / 1 = left / right trigger, bit 2 / 3 = left / right grip.
    std::uint8_t armMask{};
};
inline constexpr std::uint8_t armMaskTrigger(unsigned h){return std::uint8_t(1u<<(h&1u));}
inline constexpr std::uint8_t armMaskGrip(unsigned h){return std::uint8_t(4u<<(h&1u));}
inline bool validController(const ControllerSample& sample) {
    bool valid=sample.sequence&&sample.publishedNs&&sample.epoch&&sample.focused&&sample.active&&!(sample.buttons&~0x7fu);
    for(float x:sample.sticks)valid=valid&&std::isfinite(x)&&std::abs(x)<=1.01f;
    for(const auto& axes:{sample.triggers,sample.grips})for(float x:axes)valid=valid&&std::isfinite(x)&&x>=0&&x<=1.01f;
    return valid;
}
class ControllerMailbox {
    mutable std::mutex mutex_;
    ControllerSample latest_{};std::uint64_t sequence_{},continuity_{1};bool available_{};
public:
    static constexpr std::uint64_t maxAgeNs=150'000'000;
    bool publish(const ControllerSample& sample) {
        std::lock_guard guard(mutex_);
        const bool valid=sample.sequence>sequence_&&validController(sample);
        if(!valid){if(available_)++continuity_;available_=false;return false;}
        latest_=sample;sequence_=sample.sequence;available_=true;return true;
    }
    void invalidate(){std::lock_guard guard(mutex_);if(available_)++continuity_;available_=false;}
    bool snapshot(std::uint64_t now,ControllerSample& out)const {
        std::lock_guard guard(mutex_);
        if(!available_||now<latest_.publishedNs||now-latest_.publishedNs>maxAgeNs)return false;
        out=latest_;out.continuity=continuity_;return true;
    }
};
// The game's own menu must stay usable: a hand on a VR weapon / support grip / rein keeps its grip squeezed; the re-arm of a
// presentation change would leave the whole pad dead in the pause menu until every hand lets go. Published by the GAME MENU step (TraceEntry)
// only around an open pause scene: no grip blocks the arming, a squeezed one stays off the pad (no LB / RB edge) until it is released once.
// All grips, not vr::calib::menuHoldHand: clearOwner sets that to -1 as soon as the presentation leaves World.
inline std::atomic<bool> padArmIgnoreGrips{false};
// SOFT RE-ARM: the game's scene camera handing back to gameplay (CameraOverride -> World / Optic, e.g. the end of a close kill's
// third-person shot) re-arms the pad at once instead of waiting for every input at rest: with the weapon grip in HOLD the grip that keeps
// the gun in the hand never reads at rest, so the whole-pad rule left the pad dead until the hand opened (which puts the gun away).
// Inputs held at that moment are sent as released until they read at rest once (no carried-over press). Samples published up to
// padArmSoftUntilNs arm this way (it also covers the XR bridge's epoch bump that follows the change). Entering the scene camera keeps the
// whole-pad rule: there the holster filter does not run (no VR fire gates, no grip captures), so the pad stays unarmed through the shot
// while a grip is held. Any other change (menu, transition, cutscene) stores 0. Lever debug.rdrvr.vr.input.softarm 0 = the whole-pad rule only.
inline std::atomic<std::uint64_t> padArmSoftUntilNs{0};
inline constexpr std::uint64_t padArmSoftWindowNs=1'000'000'000ull;
inline constexpr bool softRearmChange(PresentationContext from,PresentationContext to){
    return from==PresentationContext::CameraOverride&&(to==PresentationContext::World||to==PresentationContext::Optic);
}
// Re-arm diagnostics (report nativeInput.rearm): one record per unarmed episode, written on the input thread when the pad arms again.
enum RearmWhy : std::uint8_t {RearmExternal,RearmPresentationHard,RearmPresentationSoft,RearmEpoch,RearmContinuity,RearmNoInput,RearmWhyCount};
inline constexpr const char* rearmWhyNames[RearmWhyCount]{"EXTERNAL","HARD_PRESENTATION","SOFT_PRESENTATION","EPOCH","CONTINUITY","NO_INPUT"};
struct RearmRecord {std::uint64_t ns{};std::uint8_t why{},from{},to{};bool soft{};float armedAfterMs{};std::uint32_t maskButtons{};std::uint8_t mask{};
    std::uint8_t blockedBy{};std::uint32_t gripOnlyFrames{},frames{};};
struct RearmDiag {
    std::mutex mutex;std::array<RearmRecord,32> ring{};unsigned next{};std::uint64_t total{};
    std::atomic<std::uint64_t> softArms{},hardArms{},unarmedFrames{},unarmedMaxNs{};
    std::atomic<std::uint8_t> lastChangeFrom{},lastChangeTo{},lastChangeWhy{RearmExternal};
    void push(const RearmRecord& r){std::lock_guard g(mutex);ring[next]=r;next=(next+1u)%unsigned(ring.size());++total;}
};
inline RearmDiag rearmDiag{};
// Main thread, on every presentation change (NativeUiContext): opens or closes the soft window.
inline void notePresentationChange(PresentationContext from,PresentationContext to,std::uint64_t now,bool softLever){
    const bool soft=softLever&&softRearmChange(from,to);
    padArmSoftUntilNs.store(soft?now+padArmSoftWindowNs:0,std::memory_order_relaxed);
    rearmDiag.lastChangeFrom.store(std::uint8_t(from),std::memory_order_relaxed);rearmDiag.lastChangeTo.store(std::uint8_t(to),std::memory_order_relaxed);
    rearmDiag.lastChangeWhy.store(soft?RearmPresentationSoft:RearmPresentationHard,std::memory_order_relaxed);
}
// Game-thread policy. Regaining focus cannot turn a held trigger into a shot.
// The game owns button edges; this class emits held state only, once per update.
class ControllerPolicy {
    std::uint64_t epoch_{},continuity_{};bool armed_{};
    std::array<bool,4> analogButtons_{};
    std::array<bool,2> maskedGrips_{},maskedTriggers_{};
    std::uint32_t maskedButtons_{};
    // the current unarmed episode (diagnostics)
    std::uint64_t unarmedSinceNs_{};std::uint8_t why_{RearmExternal},blockedBy_{};std::uint32_t gripOnly_{},frames_{};bool episode_{};
    void clear(){armed_=false;analogButtons_={};epoch_=continuity_=0;maskedGrips_={};maskedTriggers_={};maskedButtons_=0;}
    // a presentation change inside an unarmed episode (the scene camera handing back while the pad is still unarmed from entering it)
    // keeps the episode's start and takes the newer presentation cause
    void begin(std::uint8_t why){if(armed_||!episode_){episode_=true;why_=why;unarmedSinceNs_=0;blockedBy_=0;gripOnly_=0;frames_=0;}
        else if(why==RearmPresentationHard||why==RearmPresentationSoft)why_=why;}
    void arm(const ControllerSample& in,bool soft){
        armed_=true;auto& d=rearmDiag;(soft?d.softArms:d.hardArms).fetch_add(1,std::memory_order_relaxed);
        if(!episode_)return;episode_=false;
        RearmRecord r{};r.ns=in.publishedNs;r.why=why_;r.soft=soft;r.from=d.lastChangeFrom.load(std::memory_order_relaxed);r.to=d.lastChangeTo.load(std::memory_order_relaxed);
        const std::uint64_t ns=unarmedSinceNs_&&in.publishedNs>=unarmedSinceNs_?in.publishedNs-unarmedSinceNs_:0;r.armedAfterMs=float(double(ns)/1e6);
        auto m=d.unarmedMaxNs.load(std::memory_order_relaxed);while(ns>m&&!d.unarmedMaxNs.compare_exchange_weak(m,ns,std::memory_order_relaxed)){}
        r.maskButtons=maskedButtons_;r.mask=mask();r.blockedBy=blockedBy_;r.gripOnlyFrames=gripOnly_;r.frames=frames_;d.push(r);
    }
public:
    // why: the caller's reason (NativePadDriver::rearm passes the kind of the last presentation change)
    void invalidate(std::uint8_t why=RearmExternal){begin(why);clear();}
    std::uint8_t mask()const{
        std::uint8_t m{};for(unsigned h=0;h<2;++h){if(maskedTriggers_[h])m|=armMaskTrigger(h);if(maskedGrips_[h])m|=armMaskGrip(h);}return m;}
    PadState sample(const ControllerSample* input){
        PadState out{};
        if(!input||!validController(*input)){invalidate(RearmNoInput);return out;}
        const auto inputEpoch=input->inputEpoch?input->inputEpoch:input->epoch;
        if(epoch_!=inputEpoch||continuity_!=input->continuity){
            const std::uint8_t why=epoch_!=inputEpoch?RearmEpoch:RearmContinuity;
            invalidate(epoch_||continuity_?why:std::uint8_t(RearmExternal));epoch_=inputEpoch;continuity_=input->continuity;}
        if(!armed_){
            if(!unarmedSinceNs_)unarmedSinceNs_=input->publishedNs?input->publishedNs:1;
            ++frames_;rearmDiag.unarmedFrames.fetch_add(1,std::memory_order_relaxed);
            const auto soft=padArmSoftUntilNs.load(std::memory_order_relaxed);
            if(soft&&input->publishedNs<=soft){
                // SOFT RE-ARM: ready on this sample; everything held now is masked until it reads at rest
                maskedButtons_=input->buttons;
                for(unsigned h=0;h<2;++h){maskedTriggers_[h]=input->triggers[h]>0.15f;maskedGrips_[h]=input->grips[h]>0.15f;}
                arm(*input,true);
            }else{
                bool neutral=input->buttons==0;std::uint8_t blocked=input->buttons?1u:0u;
                for(float x:input->sticks)if(std::abs(x)>0.15f){neutral=false;blocked|=2u;}
                for(float x:input->triggers)if(x>0.15f){neutral=false;blocked|=4u;}
                const bool ignore=padArmIgnoreGrips.load(std::memory_order_relaxed);
                bool grips=false;for(float x:input->grips)grips=grips||x>0.15f;
                if(grips&&!ignore){if(neutral)++gripOnly_;neutral=false;blocked|=8u;}
                blockedBy_|=blocked;
                if(neutral){for(unsigned h=0;h<2;++h)maskedGrips_[h]=ignore&&input->grips[h]>0.15f;arm(*input,false);}
                return out;
            }
        }
        maskedButtons_&=input->buttons; // a masked button that let go is free again
        for(unsigned h=0;h<2;++h){if(input->grips[h]<=0.15f)maskedGrips_[h]=false;if(input->triggers[h]<=0.15f)maskedTriggers_[h]=false;}
        out.ready=true;out.buttons=input->buttons&~maskedButtons_;
        for(unsigned i=0;i<4;++i)out.sticks[i]=std::clamp(input->sticks[i],-1.0f,1.0f);
        for(unsigned i=0;i<2;++i)out.triggers[i]=maskedTriggers_[i]?0.f:std::clamp(input->triggers[i],0.0f,1.0f);
        const float values[]{input->grips[0],input->grips[1],input->triggers[0],input->triggers[1]};
        const std::uint32_t bits[]{PadLeftGrip,PadRightGrip,PadLeftTrigger,PadRightTrigger};
        for(unsigned i=0;i<4;++i){analogButtons_[i]=!(i<2&&maskedGrips_[i])&&!(i>=2&&maskedTriggers_[i-2])&&values[i]>=(analogButtons_[i]?0.45f:0.55f);
            if(analogButtons_[i])out.buttons|=bits[i];}
        out.armMask=mask();
        return out;
    }
};
// Report object nativeInput.rearm: softArms / hardArms = arms by rule; ring = one record per unarmed episode (why it began, the last
// presentation change from / to (PresentationContext values), how long the pad stayed unarmed, what was masked at a soft arm, which
// inputs kept a whole-pad arm waiting: blockedBy bit 0 buttons, 1 sticks, 2 triggers, 3 grips; gripOnlyFrames = samples a grip alone held it).
inline std::string rearmReport(){
    auto& d=rearmDiag;std::string o;char b[360];
    std::snprintf(b,sizeof b,"{\"softArms\":%llu,\"hardArms\":%llu,\"unarmedFrames\":%llu,\"unarmedMaxMs\":%.1f,\"softUntilNs\":%llu,\"ring\":[",
        (unsigned long long)d.softArms.load(),(unsigned long long)d.hardArms.load(),(unsigned long long)d.unarmedFrames.load(),double(d.unarmedMaxNs.load())/1e6,
        (unsigned long long)padArmSoftUntilNs.load());o+=b;
    std::lock_guard g(d.mutex);bool first=true;
    for(unsigned k=0;k<d.ring.size();++k){const auto& r=d.ring[(d.next+k)%d.ring.size()];if(!r.ns)continue;
        std::snprintf(b,sizeof b,"%s{\"ns\":%llu,\"why\":\"%s\",\"from\":%u,\"to\":%u,\"soft\":%d,\"armedAfterMs\":%.1f,\"maskButtons\":%u,\"mask\":%u,"
            "\"blockedBy\":%u,\"gripOnlyFrames\":%u,\"frames\":%u}",first?"":",",(unsigned long long)r.ns,rearmWhyNames[r.why<RearmWhyCount?r.why:0],
            unsigned(r.from),unsigned(r.to),r.soft?1:0,double(r.armedAfterMs),r.maskButtons,unsigned(r.mask),unsigned(r.blockedBy),r.gripOnlyFrames,r.frames);o+=b;first=false;}
    o+="]}";return o;
}
}
