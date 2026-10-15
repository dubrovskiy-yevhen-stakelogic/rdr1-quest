#pragma once
// CAMERA YAW HOLD (LOCO / CAMERA): the stock follow camera must not turn the VR world.
//
// Why: the VR world yaw psi IS the stock CMX camera's heading (prepareHeadCamera keeps the stock
// camera's horizontal heading, CameraAdapter composes the HMD on top; LOCO's stick direction = psi
// + HMD yaw). On foot the stock camera is camCMXCameraActions::Roam_Camera, a follow camera: every
// frame Roam_Camera::UpdateFollowBehavior (0x25fe460) re-derives its orientation from its follow
// matrix (this+0x480) and turns it (pull-around ApplyPullAroundOrientationOffsets 0x25ff438,
// non-stagnant auto-centre RoamTuneData::AutoCenterNonStagnent*, stagnant auto-centre
// camGlueCMXGame+0x1c98..) so that it ends up BEHIND the character. Walking forward the character
// already faces away from the camera - nothing to do. Stick BACK (or sideways) makes the native
// animation turn the character toward the camera (side); the follow camera then swings around to
// get behind him again -> psi rotates while the player holds the stick and keeps the head still ->
// the world turns in the headset AND the stick's world direction (psi + HMD) turns with it -> the
// capsule walks an arc (and keeps arcing: the character keeps facing "back", the camera keeps chasing).
//
// How: GOT hook of Roam_Camera::UpdateFollowBehavior (JUMP_SLOT 0x2e50bc8; its ONLY caller is CalculateCommonVariables,
// bl 0x25fdc0c, return 0x25fdc10). While the VR player is on foot (LOCO gate ACTIVE, fresh VR head yaw) the yaw change the
// follow behaviour made in this call is rotated back out of both follow matrices (this+0x480 and the tilted copy this+0x4c0;
// rows a, b, c about world Y, row d = camera position about the attach point this+0x3c0, exactly how ComputeFollowMatrix
// builds it: d = attach + R(yaw)*offset - c*distance). Pitch/roll, vertical look, collision and the camera position math
// stay native. Explicit orientation changes PASS untouched: the reset/"set orientation" paths (this+0x41c,
// this+0x430 != this+0x385, parameter +0x2e0 = camGlueCMXGame::SetCameraOrientation = SNAP TURN / teleport view turn,
// parameter +0x690 absolute heading) and any jump > largeDeltaRad in one call.
// Kill switch: setprop debug.rdrvr.vr.yawhold 0 (read once at install). Report: vr.loco.cameraYawHold.
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace rdrvr::vr::yawhold {

// ---- ELF identities (librdr.so 1.58 arm64; .text file offset = vaddr - 0x4000) ----
inline constexpr std::uintptr_t followOffset=0x25fe460,followGotOffset=0x2e50bc8;   // Roam_Camera::UpdateFollowBehavior
inline constexpr std::uintptr_t followSiteOffset=0x25fdbfc,followReturnOffset=0x25fdc10; // in CalculateCommonVariables
inline constexpr std::uintptr_t resetFlagsOffset=0x25fdb70;   // CalculateCommonVariables reset test (0x41c / 0x430!=0x385 / param 0x2e0)
inline constexpr std::uintptr_t getParametersOffset=0x25afa14; // cmxAction::GetParameters: [this+0xb8] ? [+0x10] : 0
inline constexpr std::uintptr_t followTailOffset=0x25fe870;   // UpdateFollowBehavior tail: final rows of this+0x4c0 / this+0x4e0
inline constexpr std::uintptr_t followPositionOffset=0x25fd7fc; // ComputeFollowMatrix: d = attach(+0x3c0) + R(yaw)*offset(+0x400) - c*dist
inline constexpr std::uintptr_t roamVtable=0x2d85dc0,roamClampedVtable=0x2d85e88;    // address points (symbol + 0x10)
inline constexpr std::uintptr_t roamOnUpdateSlot=0x2d85e38,roamClampedOnUpdateSlot=0x2d85f00,roamOnUpdate=0x25fd884;
inline constexpr std::array<unsigned char,64> followFingerprint{
#include "gamecode/VrYawHold_25fe460_64_8d.inc"
};
inline constexpr std::array<unsigned char,24> followSiteFingerprint{
#include "gamecode/VrYawHold_25fdbfc_24_8d.inc"
};
inline constexpr std::array<unsigned char,52> resetFlagsFingerprint{
#include "gamecode/VrYawHold_25fdb70_52_8d.inc"
};
inline constexpr std::array<unsigned char,20> getParametersFingerprint{
#include "gamecode/VrYawHold_25afa14_20_8d.inc"
};
inline constexpr std::array<unsigned char,88> followTailFingerprint{
#include "gamecode/VrYawHold_25fe870_88_8d.inc"
};
inline constexpr std::array<unsigned char,100> followPositionFingerprint{
#include "gamecode/VrYawHold_25fd7fc_100_8d.inc"
};
namespace field {
inline constexpr std::size_t followMatrix=0x480,tiltedMatrix=0x4c0,attach=0x3c0; // Matrix34 rows a,b,c,d at +0/+0x10/+0x20/+0x30
inline constexpr std::size_t resetA=0x41c,cutCur=0x430,cutPrev=0x385,blockData=0xb8,blockParameters=0x10;
inline constexpr std::size_t paramTable=0x8,paramSetOrientation=0x2e0,paramAbsoluteHeading=0x690;
}
inline constexpr float largeDeltaRad=0.35f; // ~20 deg in one camera update = an intentional cut/turn, never a follow drift

// ---- pure math (host-tested) ----
// Heading of a Matrix34 forward row c (floats 8..10), the same atan2(c.x, c.z) Roam_Camera::OnUpdate stores at +0x150.
// NaN when the row is (nearly) vertical or not finite.
inline float rowYaw(const float* m) noexcept {
    const float x=m[8],z=m[10];
    if(!std::isfinite(x)||!std::isfinite(z)||std::hypot(x,z)<0.1f)return NAN;
    return std::atan2(x,z);
}
inline float wrapPi(float a) noexcept {
    if(!std::isfinite(a))return a;
    while(a>3.14159265f)a-=6.28318531f;while(a<-3.14159265f)a+=6.28318531f;return a;
}
// Rotate a horizontal (x,z) pair so that atan2(x,z) grows by `delta` (world Y axis; y untouched).
inline void rotateXZ(float& x,float& z,float delta) noexcept {
    const float s=std::sin(delta),c=std::cos(delta),nx=x*c+z*s,nz=-x*s+z*c;x=nx;z=nz;
}
// Rotate a whole Matrix34 (16 floats: rows a,b,c,d with a pad float each) about the vertical through `attach` by `delta`:
// a, b, c are directions, d is the camera position.
inline void rotateMatrix(float* m,const float* attach,float delta) noexcept {
    for(unsigned r=0;r<3;++r)rotateXZ(m[r*4],m[r*4+2],delta);
    float dx=m[12]-attach[0],dz=m[14]-attach[2];rotateXZ(dx,dz,delta);m[12]=attach[0]+dx;m[14]=attach[2]+dz;
}
enum class Verdict : unsigned {Held,Unchanged,Reset,Large,BadRow,Count};
inline constexpr std::array<const char*,unsigned(Verdict::Count)> verdictNames{"held","unchanged","passedReset","passedLarge","badRow"};
// One UpdateFollowBehavior call: `before` = heading of the follow matrix before the original ran, m1/m2 = the follow and
// tilted matrices after it. Cancels the call's yaw change unless it was an explicit set (reset) or a big jump.
inline Verdict hold(float before,bool reset,float* m1,float* m2,const float* attach,float& delta) noexcept {
    delta=0.f;
    const float after=rowYaw(m1);
    if(!std::isfinite(before)||!std::isfinite(after))return Verdict::BadRow;
    delta=wrapPi(after-before);
    if(reset)return Verdict::Reset;
    if(std::fabs(delta)>largeDeltaRad)return Verdict::Large;
    if(std::fabs(delta)<1e-6f)return Verdict::Unchanged;
    for(unsigned i=0;i<3;++i)if(!std::isfinite(attach[i]))return Verdict::BadRow;
    rotateMatrix(m1,attach,-delta);
    if(m2)rotateMatrix(m2,attach,-delta);
    return Verdict::Held;
}

// ---- counters (hook on the camera update thread = engine main thread; report on the writer) ----
inline std::atomic<bool> installed{},switchOn{true};
inline std::atomic<const char*> installState{"NOT ATTEMPTED"};
inline std::array<std::atomic<std::uint64_t>,unsigned(Verdict::Count)> verdicts{};
inline std::atomic<std::uint64_t> calls{},gateClosed{},otherCaller{},wrongType{},readFailures{};
inline std::atomic<float> cancelledDeg{},cancelledMaxDeg{},lastDeltaDeg{},passedDeg{};
inline std::atomic<std::uint64_t> lastHeldNs{};
inline void count(Verdict v,float delta,std::uint64_t now) noexcept {
    verdicts[unsigned(v)].fetch_add(1,std::memory_order_relaxed);
    const float d=std::fabs(delta)*57.2957795f;
    if(std::isfinite(d))lastDeltaDeg.store(delta*57.2957795f,std::memory_order_relaxed);
    if(v==Verdict::Held&&std::isfinite(d)){
        cancelledDeg.store(cancelledDeg.load(std::memory_order_relaxed)+d,std::memory_order_relaxed);
        if(d>cancelledMaxDeg.load(std::memory_order_relaxed))cancelledMaxDeg.store(d,std::memory_order_relaxed);
        lastHeldNs.store(now,std::memory_order_relaxed);
    }else if((v==Verdict::Reset||v==Verdict::Large)&&std::isfinite(d))passedDeg.store(passedDeg.load(std::memory_order_relaxed)+d,std::memory_order_relaxed);
}
// "cameraYawHold":{...} body (an object).
inline std::string report() {
    std::string out;char b[640];
    std::snprintf(b,sizeof b,"{\"hook\":\"%s\",\"switch\":%s,\"calls\":%llu,\"gateClosed\":%llu,\"otherCaller\":%llu,\"wrongType\":%llu,"
        "\"readFailures\":%llu",installState.load(),switchOn.load()?"true":"false",(unsigned long long)calls.load(),
        (unsigned long long)gateClosed.load(),(unsigned long long)otherCaller.load(),(unsigned long long)wrongType.load(),
        (unsigned long long)readFailures.load());
    out+=b;
    for(unsigned i=0;i<unsigned(Verdict::Count);++i){
        std::snprintf(b,sizeof b,",\"%s\":%llu",verdictNames[i],(unsigned long long)verdicts[i].load());out+=b;}
    std::snprintf(b,sizeof b,",\"cancelledDeg\":%.2f,\"cancelledMaxDegPerCall\":%.3f,\"lastDeltaDeg\":%.3f,\"passedDeg\":%.1f}",
        double(cancelledDeg.load()),double(cancelledMaxDeg.load()),double(lastDeltaDeg.load()),double(passedDeg.load()));
    out+=b;
    return out;
}

#if defined(__ANDROID__)
// ======================================= engine side (Android only) =======================================
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using FollowFn=void(*)(void* self);
using GateFn=bool(*)() noexcept;
namespace detail {
inline ReadFn read{};inline std::uintptr_t base{};inline FollowFn original{};inline GateFn gate{};
template<class T> inline bool get(std::uintptr_t p,std::size_t o,T& out) noexcept {
    return read&&p&&p<=UINTPTR_MAX-o&&p+o<=UINTPTR_MAX-sizeof(T)&&read(nullptr,p+o,&out,sizeof(T));}
// The reset/"set orientation" tests CalculateCommonVariables (0x25fdb70) and UpdateFollowBehavior (0x25fe730/0x25fe900)
// make before they rebuild the follow matrix from the variables. Unreadable = treated as reset (never hold blindly).
inline bool explicitSet(std::uintptr_t self) noexcept {
    std::uint8_t a{},cur{},prev{};
    if(!get(self,field::resetA,a)||!get(self,field::cutCur,cur)||!get(self,field::cutPrev,prev))return true;
    if(a||cur!=prev)return true;
    std::uintptr_t data{},params{},p0{},table{},flag{},absolute{};std::uint8_t set{},abs{};
    if(!get(self,field::blockData,data)||!data||!get(data,field::blockParameters,params)||!params||!get(params,0,p0)||!p0||
       !get(p0,field::paramTable,table)||!table||!get(table,field::paramSetOrientation,flag)||!flag||!get(flag,0,set))return true;
    if(set)return true;
    if(get(table,field::paramAbsoluteHeading,absolute)&&absolute&&get(absolute,0,abs)&&abs)return true;
    return false;
}
} // namespace detail
__attribute__((noinline)) inline void followHook(void* self) noexcept {
    const auto original=detail::original;
    calls.fetch_add(1,std::memory_order_relaxed);
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if(caller!=detail::base+followReturnOffset){otherCaller.fetch_add(1,std::memory_order_relaxed);original(self);return;}
    if(!switchOn.load(std::memory_order_relaxed)||!detail::gate||!detail::gate()){gateClosed.fetch_add(1,std::memory_order_relaxed);original(self);return;}
    const auto s=reinterpret_cast<std::uintptr_t>(self);
    std::uintptr_t vptr{};
    if(!detail::get(s,0,vptr)){readFailures.fetch_add(1,std::memory_order_relaxed);original(self);return;}
    if(vptr!=detail::base+roamVtable&&vptr!=detail::base+roamClampedVtable){wrongType.fetch_add(1,std::memory_order_relaxed);original(self);return;}
    const bool reset=detail::explicitSet(s);
    float m1[16]{},m2[16]{},attach[4]{};
    const bool rowRead=detail::get(s,field::followMatrix,m1);
    const float before=rowRead?rowYaw(m1):NAN;
    original(self);
    // The object is live (its own update just ran with it): plain copies in/out of the two matrices.
    auto* p=reinterpret_cast<unsigned char*>(self);
    std::memcpy(m1,p+field::followMatrix,sizeof m1);std::memcpy(m2,p+field::tiltedMatrix,sizeof m2);
    std::memcpy(attach,p+field::attach,sizeof attach);
    float delta{};const auto v=hold(before,reset,m1,m2,attach,delta);
    if(v==Verdict::Held){std::memcpy(p+field::followMatrix,m1,sizeof m1);std::memcpy(p+field::tiltedMatrix,m2,sizeof m2);}
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    count(v,delta,ns>0?std::uint64_t(ns):0);
}
// code(offset, fingerprint) / got(slot, target) / replace(address, expected, replacement): the LOCO install lambdas.
template<class Code,class Got,class Replace>
inline void install(ReadFn read,std::uintptr_t base,bool on,GateFn gate,Code&& code,Got&& got,Replace&& replace) noexcept {
    switchOn.store(on);
    if(!on){installState.store("KILLED");return;}
    if(!read||!base||!gate){installState.store("NO ENGINE API");return;}
    if(!(code(followOffset,followFingerprint)&&code(followSiteOffset,followSiteFingerprint)&&code(resetFlagsOffset,resetFlagsFingerprint)&&
         code(getParametersOffset,getParametersFingerprint)&&code(followTailOffset,followTailFingerprint)&&
         code(followPositionOffset,followPositionFingerprint)&&got(followGotOffset,followOffset)&&
         got(roamOnUpdateSlot,roamOnUpdate)&&got(roamClampedOnUpdateSlot,roamOnUpdate))){installState.store("ENGINE MISMATCH");return;}
    detail::read=read;detail::base=base;detail::gate=gate;detail::original=reinterpret_cast<FollowFn>(base+followOffset);
    if(replace(base+followGotOffset,base+followOffset,reinterpret_cast<std::uintptr_t>(&followHook))){
        installed.store(true,std::memory_order_release);installState.store("INSTALLED");}
    else installState.store("HOOK NOT INSTALLED");
}
#endif // __ANDROID__

} // namespace rdrvr::vr::yawhold
