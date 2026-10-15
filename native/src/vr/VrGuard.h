#pragma once
// FIST BLOCK: squeezing both hands and holding them up like a boxing guard blocks blows.
// Pure guard-pose detector (input thread, holsters InputBrain) + the state it publishes for the melee main-thread tick (VrMelee.h),
// which drives the engine block: aniMelee::SetBlockState(true) on the player = aniMelee+0x55c bit 0x80, the bit every NPC attacker's
// aniMelee::MeleeMachine tests before it applies a strike (0x1e41de8..0x1e41ee8: blocked -> aniTarget::SetBlockedImpact (victim
// +0x55c |= 0x40), ApplyKnockback, Broadcast(animMsgMeleeBlocked), NO damage; the block meter aniMelee+0x130 can break the guard).
//
// Pose (OpenXR LOCAL, +Y up, metres, the HMD's yaw frame: forward = where the head looks, horizontally):
//  - both grips closed (>= .65 to engage, >= .45 to stay), both VR hands empty (no weapon / phantom / foregrip / pending grab);
//  - each hand in front of the face / chest: 0.06..0.60 m ahead of the eyes, 0.50 m below .. 0.15 m above them, <= 0.38 m to the side
//    (leaving: the box grows by 0.08 m);
//  - forearm roughly vertical: the controller's aim ray points up (dot(aim forward, up) >= 0.30 to engage, >= 0.10 to stay) - a fist held
//    knuckles-up points the Touch aim ray at the ceiling, a jab points it forward;
//  - held >= 150 ms to engage (a punch crossing the box never blocks), any condition lost for >= 120 ms releases (hysteresis in time).
// The grips of hands inside the box never reach the game (no LB weapon wheel / RB cover while guarding): InputBrain pre-masks them.
// Kill switch: debug.rdrvr.vr.melee.block 0 (startup; report vr.melee.block.killed); menu HANDS / WEAPONS -> FIST BLOCK ON/OFF (VrMelee
// publishes `active` = FIST BLOCK ON + MELEE PHYSICAL + MELEE FISTS ON + the engine block functions verified).
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace rdrvr::vr::guard {
using V3=std::array<float,3>;

struct Config {
    float fwdMin{.06f},fwdMax{.60f},upMin{-.50f},upMax{.15f},sideMax{.38f},margin{.08f};
    float gripEnter{.65f},gripStay{.45f},forearmEnter{.30f},forearmStay{.10f};
    std::uint64_t engageNs{150'000'000},releaseNs{120'000'000},freshNs{200'000'000}; // freshNs: main thread trusts a state this old
};
inline Config config{};

enum State : int {StateIdle=0,StateArming=1,StateEngaged=2};
inline constexpr const char* stateName(int s) noexcept {return s==StateEngaged?"ENGAGED":s==StateArming?"ARMING":"IDLE";}

// One hand as the input brain sees it this frame.
struct HandIn {bool valid{},empty{};V3 pos{};V3 aimForward{0,0,-1};float grip{};};
// Head-yaw frame coordinates of a hand: x = right, y = up, f = forward (metres, from the HMD position).
struct Local {float side{},up{},forward{};};
inline Local toHead(const V3& head,float headYaw,const V3& p) noexcept {
    const float dx=p[0]-head[0],dy=p[1]-head[1],dz=p[2]-head[2];
    // inverse of holsters yawRotate(yaw): rotate by -yaw about +Y (OpenXR: +X right, -Z forward)
    const float c=std::cos(-headYaw),s=std::sin(-headYaw);
    const float x=dx*c+dz*s,z=-dx*s+dz*c;
    return {x,dy,-z};
}
inline bool inBox(const Local& l,float m) noexcept {
    const auto& c=config;
    return l.forward>=c.fwdMin-m&&l.forward<=c.fwdMax+m&&l.up>=c.upMin-m&&l.up<=c.upMax+m&&std::fabs(l.side)<=c.sideMax+m;
}
inline float forearmUp(const HandIn& h) noexcept { // dot(aim forward, +Y), aim forward normalised
    const float n=std::sqrt(h.aimForward[0]*h.aimForward[0]+h.aimForward[1]*h.aimForward[1]+h.aimForward[2]*h.aimForward[2]);
    return n>1e-4f&&std::isfinite(n)?h.aimForward[1]/n:-1.f;
}

// ---- published (input thread writes, main thread / report read) ----
inline std::atomic<bool> killed{},active{}; // active = FIST BLOCK on + physical fists on (VrMelee tick, every frame)
inline std::atomic<int> state{StateIdle};inline std::atomic<bool> premaskAny{}; // premaskAny: a raised empty fist this frame (theater chord off)
inline std::atomic<std::uint64_t> stampNs{},engagedSinceNs{};
inline std::atomic<std::uint64_t> frames{},armingFrames{},engagedFrames{},engages{},releases{},aborted{},maskedPresses{},premaskedFrames{};
inline std::atomic<float> lastForearm[2]{},lastForward[2]{},lastUp[2]{},lastSide[2]{};
inline void bump(std::atomic<std::uint64_t>& a) noexcept {a.fetch_add(1,std::memory_order_relaxed);}
// Main thread: the engaged state, only while the input brain keeps publishing it.
inline bool engagedFresh(std::uint64_t now) noexcept {
    if(state.load(std::memory_order_acquire)!=StateEngaged)return false;
    const auto t=stampNs.load(std::memory_order_relaxed);
    return t&&(now>=t?now-t:t-now)<=config.freshNs;
}

class Detector {
public:
    struct Out {int state{StateIdle};std::array<bool,2> premask{};bool engagedEdge{},releasedEdge{};};
    // headYaw: holsters' head yaw (atan2(-f.x,-f.z)). enabled false = idle (and released).
    Out update(bool enabled,const V3& head,float headYaw,const std::array<HandIn,2>& hands,std::uint64_t now) noexcept {
        Out o{};
        if(!enabled){if(state_==StateEngaged)o.releasedEdge=true;state_=StateIdle;armNs_=lostNs_=0;o.state=state_;return o;}
        std::array<Local,2> l{};std::array<bool,2> enter{},stay{};
        for(unsigned h=0;h<2;++h){
            const auto& x=hands[h];if(!x.valid||!x.empty)continue;
            l[h]=toHead(head,headYaw,x.pos);const float up=forearmUp(x);
            lastForearm[h].store(up,std::memory_order_relaxed);lastForward[h].store(l[h].forward,std::memory_order_relaxed);
            lastUp[h].store(l[h].up,std::memory_order_relaxed);lastSide[h].store(l[h].side,std::memory_order_relaxed);
            enter[h]=inBox(l[h],0.f)&&x.grip>=config.gripEnter&&up>=config.forearmEnter;
            stay[h]=inBox(l[h],config.margin)&&x.grip>=config.gripStay&&up>=config.forearmStay;
            o.premask[h]=inBox(l[h],config.margin)&&up>=config.forearmStay; // a raised empty fist: its grip is no LB / RB
        }
        const bool candidate=enter[0]&&enter[1],keep=stay[0]&&stay[1];
        switch(state_){
        case StateIdle:if(candidate){state_=StateArming;armNs_=now;}break;
        case StateArming:
            if(!candidate){state_=StateIdle;armNs_=0;bump(aborted);}
            else if(now>=armNs_&&now-armNs_>=config.engageNs){state_=StateEngaged;lostNs_=0;o.engagedEdge=true;}
            break;
        case StateEngaged:
            if(keep)lostNs_=0;
            else{if(!lostNs_)lostNs_=now?now:1;
                if(now>=lostNs_&&now-lostNs_>=config.releaseNs){state_=StateIdle;armNs_=lostNs_=0;o.releasedEdge=true;}}
            break;
        default:state_=StateIdle;break;
        }
        o.state=state_;return o;
    }
    void reset() noexcept {state_=StateIdle;armNs_=lostNs_=0;}
    int current() const noexcept {return state_;}
private:
    int state_{StateIdle};std::uint64_t armNs_{},lostNs_{};
};
} // namespace rdrvr::vr::guard
