#pragma once
// SEAT STEADY: the riding eye in a vehicle seat (player state 3: wagon, carriage, stagecoach; GameXr ridingAnchorSample).
// The seat is an attachment on the sprung vehicle body, so John's root carries the body's sway, jolts and yaw wobble, and the
// riding eye (root + forward offset, the heading from the root's c axis) passed all of it to the view; on a horse the root is
// smooth on the saddle and only the height bounces. In a seat:
//  * the root's horizontal position and the heading are Holt-filtered (level + trend: a constant speed or turn rate is followed
//    without lag, the sway at 1..3 Hz is damped), each held within a band of the raw value (a seat swap or a crash drags the
//    eye along at the band edge, a gap or a jump over 4 x the band from the prediction re-seeds);
//  * the seat height comes from a 2 s mean of the settled head height above the root instead of the highest one (a driving
//    animation that raises the head never lifts the view for the rest of the ride), and the gallop lean is 0 (the head bone
//    of the driving animation never moves the eye);
//  * the eye's offset from the raw seat (metres, world) is published for VrBody hook A, which carries John's skeleton by it
//    (the body stays under the view and the vehicle moves around both).
// HORSE > HORSE SHAKE ON keeps the raw motion here too. Levers (read every 256 samples): debug.rdrvr.vr.riding.seatsteady 0 = off,
// .seatms (Holt tau of the position, ms, default 250), .seatcm (position band, cm, default 12), .seatyawms (heading tau, default
// 300), .seatyawdeg (heading band, deg, default 8), .seatbody 0 = John's skeleton is not carried.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace rdrvr::vr::seatsteady {
// yBand: the seat eye HEIGHT is held within this of the raw seat eye (debug.rdrvr.vr.riding.seatycm, 4; 0 = the free Holt + head floor):
// a coach is rigid around its seat - the free Holt lagged its heave by -0.20..+0.15 m at 8-12 m/s and the coach rose through the view
struct Lever {bool on{true},body{true};float tau{.25f},band{.12f},yawTau{.30f},yawBand{.1396263f},yBand{.04f};};
struct Filter {bool have{},seedTrend{};float x{},z{},vx{},vz{},yaw{},vyaw{};std::uint64_t ns{};}; // seedTrend: the next sample sets the trend
struct Result {float x{},z{},yaw{};bool reseeded{},clamped{},yawClamped{};};
// One Holt step of level y / trend v toward raw (dt s, tau s); the level is held within band of raw before the trend update, and a
// trend pointing away from raw at the band edge is dropped (no overshoot after a drag).
inline bool holtBand(float& y,float& v,float raw,float dt,float tau,float band) noexcept {
    const float ka=1.f-std::exp(-dt/tau),kb=1.f-std::exp(-dt/(2.f*tau));
    const float predicted=y+v*dt;float level=predicted+(raw-predicted)*ka;bool hit=false;
    if(level>raw+band){level=raw+band;hit=true;}else if(level<raw-band){level=raw-band;hit=true;}
    v+=((level-y)/std::max(dt,1e-4f)-v)*kb;
    if(hit&&(level-raw)*v>0.f)v=0.f;
    y=level;return hit;
}
// raw x / z = the seat (root) position, rawYaw = its heading (radians, any wrap); fresh = the first sample of a run (gap, mount).
inline Result step(Filter& f,float rawX,float rawZ,float rawYaw,std::uint64_t now,bool fresh,const Lever& lv) noexcept {
    Result r{};constexpr float twoPi=6.2831853f;
    const float dt=f.have&&now>f.ns?std::min(float(double(now-f.ns)*1e-9),.1f):0.f;
    const float dYaw=std::remainder(rawYaw-f.yaw,twoPi);
    const bool jump=f.have&&(std::fabs(rawX-f.x-f.vx*dt)>4.f*lv.band||std::fabs(rawZ-f.z-f.vz*dt)>4.f*lv.band||std::fabs(dYaw-f.vyaw*dt)>4.f*lv.yawBand);
    if(fresh||!f.have||!(dt>0.f)||jump||!std::isfinite(f.x+f.z+f.vx+f.vz+f.yaw+f.vyaw)){
        r.reseeded=f.have&&(fresh||jump);
        f.have=std::isfinite(rawX+rawZ+rawYaw);f.seedTrend=f.have;f.x=rawX;f.z=rawZ;f.vx=f.vz=0.f;f.yaw=rawYaw;f.vyaw=0.f;f.ns=now;
        r.x=rawX;r.z=rawZ;r.yaw=rawYaw;return r;}
    f.ns=now;
    if(f.seedTrend){ // the second sample of a run: the trend from the raw motion (a re-seed at speed follows at once), the level raw
        f.seedTrend=false;f.vx=(rawX-f.x)/dt;f.vz=(rawZ-f.z)/dt;f.vyaw=dYaw/dt;f.x=rawX;f.z=rawZ;f.yaw=rawYaw;
        if(!std::isfinite(f.vx+f.vz+f.vyaw))f.vx=f.vz=f.vyaw=0.f;
        r.x=rawX;r.z=rawZ;r.yaw=rawYaw;return r;}
    r.clamped=holtBand(f.x,f.vx,rawX,dt,lv.tau,lv.band);
    r.clamped=holtBand(f.z,f.vz,rawZ,dt,lv.tau,lv.band)||r.clamped;
    const float unwrapped=f.yaw+dYaw;r.yawClamped=holtBand(f.yaw,f.vyaw,unwrapped,dt,lv.yawTau,lv.yawBand);
    f.yaw=std::remainder(f.yaw,twoPi);
    r.x=f.x;r.z=f.z;r.yaw=f.yaw;return r;
}
// The seat height above the root: a mean (tau s) over the settled frames; seated = the first sample of the run seeds it.
inline float seatUp(float up,float dy,bool seed,bool settled,float dt,float tau) noexcept {
    if(seed||!std::isfinite(up))return dy;
    if(settled&&dt>0.f&&std::isfinite(dy))up+=(dy-up)*(1.f-std::exp(-dt/tau));
    return up;
}

// Published by the riding sample (main thread) for hook A: the eye minus the raw seat eye (m, world), stamped.
inline std::atomic<float> shiftX{0.f},shiftY{0.f},shiftZ{0.f};inline std::atomic<std::uint64_t> shiftNs{0};
inline bool shiftFresh(std::uint64_t now) noexcept {const auto t=shiftNs.load(std::memory_order_acquire);return t&&now>=t&&now-t<150'000'000ull;}
// Report (riding.seat)
inline std::atomic<bool> active{false},bodyLever{true};
// the player sits in a vehicle seat (player state 3) - AppSW: the coach around the seat moves with the camera rig (SpaceWarp)
inline std::atomic<bool> seated{false};
inline std::atomic<std::uint64_t> frames{0},reseeds{0},clampFrames{0},yawClampFrames{0},shakeOn{0},leverOff{0},bodyFrames{0};
inline std::atomic<float> errCm{0.f},errCmMax{0.f},yawErrDeg{0.f},yawErrDegMax{0.f},rawSwayCm{0.f};
} // namespace rdrvr::vr::seatsteady
