#pragma once
// LASSO (menu rows LASSO THROW / LASSO ASSIST): the engine side of the VR lasso. Header-only; the light shared
// part (brain, boxes, config, counters) is VrLassoLink.h (holsters include that).
//   * ModuleLasso (mainthread id 8, after Holsters / Arc, before Teleport): batched guarded reads of the lasso item, the player's aniLasso,
//     aniLassoData (LaunchVelocity / MaxThrowDistance / ProbeCapsulseRadius), the victim, the mover velocity, the root, the minigame focus /
//     prompt code, gravity, dt -> EngineFacts; the actor scan for the assist (4 Hz, VrMelee's verified constants); the LASSO arc producer
//     (engine-step Euler + the engine's x/z-only homing + the pitch solve, Tracer::runPath with the engine capsule probe 0x100028 / live
//     radius, gold ring / disc / cross) -> ShownBox (the arc that was SHOWN = the arc H1 launches); H4 direct launch (option); ropeBusy ->
//     tploco::externalBusy; the row note; tick-side latch drops (dead, cutscene); counters.
//   * hooks (GOT): H1 invWeapon::OnUseItem from invLassoWeapon::OnUseItem (p0, v0 and the assist fields of the mod's plan, the flight token),
//     H2 invLassoWeapon::UpdateInFlight from OnUpdate (token flights only: the homing fields every step, +0x40c restored at the end),
//     H3 grcViewport::IsPointSetVisible at the nine off-screen sites (a loop thrown outside the game camera stays alive).
//   * graded install (H holsters-only / R reader / L launch / O off-screen), report vr.lasso.
//     Any print / GOT mismatch -> that grade off (UNAVAILABLE / NATIVE ONLY), never a half mode.
#include "VrLassoLink.h"
#include "VrArc.h"
#include "VrHolsters.h"
#include "VrTeleportLink.h"
#include "diagnostics/GuardedReadVector.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace rdrvr::vr::lasso {
namespace hs=::rdrvr::vr::hands;namespace am=::rdrvr::vr::arc::math;

// ============================== ELF identities (librdr.so arm64 sha 3f80845e..; .text file offset = vaddr - 0x4000) ==============================
namespace elf {
inline constexpr std::uintptr_t lassoItemTableOffset=0x2d8d498;                                     // vptr of invLassoWeapon (vtable symbol 0x2d8d488)
inline constexpr std::uintptr_t onUseItemOffset=0x268f338,useItemVtableWordOffset=0x2d8d4c8;       // invLassoWeapon::OnUseItem = vt[0x30] (ABS64 word)
inline constexpr std::uintptr_t weaponUseItemOffset=0x267715c,useItemGotOffset=0x2e52838,useItemPltOffset=0x2c943b0,useItemReturnOffset=0x268f58c; // H1
inline constexpr std::uintptr_t inFlightOffset=0x268a740,inFlightGotOffset=0x2e52a38,inFlightPltOffset=0x2c947b0,inFlightReturnOffset=0x268a600;   // H2
inline constexpr std::uintptr_t visibleOffset=0x1b0048c,visibleGotOffset=0x2e22838,visiblePltOffset=0x2c343b0;                                      // H3
inline constexpr std::array<std::uintptr_t,9> offscreenReturnOffsets{0x26898c0,0x26898f8,0x268992c,0x2689968,0x268999c,0x26899d8,0x2689a0c,0x2689a44,0x2689a78};
// GLOB_DAT words (the GOT value is the global's address): minigame singleton pointer variable, sm_Gravity / sm_GlobalManager / sm_FlashProxy objects
inline constexpr std::uintptr_t minigameGotOffset=0x2e01f60,minigameOffset=0x4e75ebe0,gravityGotOffset=0x2de2110,gravityOffset=0x4e6f80c0,
    timeGotOffset=0x2ddf340,timeOffset=0x4e6d6510,proxyGotOffset=0x2e04d60,proxyOffset=0x4e7b79d0;
inline constexpr std::uintptr_t lassoOnUseItemOffset=0x268f338;
inline constexpr std::array<unsigned char,652> lassoOnUseItemFingerprint{
#include "gamecode/VrLasso_268f338_652_8d.inc"
};
inline constexpr std::uintptr_t lassoUpdateInFlightHeadOffset=0x268a740;
inline constexpr std::array<unsigned char,640> lassoUpdateInFlightHeadFingerprint{
#include "gamecode/VrLasso_268a740_640_8d.inc"
};
inline constexpr std::uintptr_t lassoUpdateInFlightRangeOffset=0x268ac3c;
inline constexpr std::array<unsigned char,192> lassoUpdateInFlightRangeFingerprint{
#include "gamecode/VrLasso_268ac3c_192_8d.inc"
};
inline constexpr std::uintptr_t lassoInFlightCallSiteOffset=0x268a5f8;
inline constexpr std::array<unsigned char,16> lassoInFlightCallSiteFingerprint{
#include "gamecode/VrLasso_268a5f8_16_8d.inc"
};
inline constexpr std::uintptr_t lassoInFlightHitFailOffset=0x268abd0;
inline constexpr std::array<unsigned char,112> lassoInFlightHitFailFingerprint{
#include "gamecode/VrLasso_268abd0_112_8d.inc"
};
inline constexpr std::uintptr_t lassoLockRefreshOffset=0x2689a88;
inline constexpr std::array<unsigned char,276> lassoLockRefreshFingerprint{
#include "gamecode/VrLasso_2689a88_276_8d.inc"
};
inline constexpr std::uintptr_t lassoOnThrowWeaponOffset=0x26863c4;
inline constexpr std::array<unsigned char,56> lassoOnThrowWeaponFingerprint{
#include "gamecode/VrLasso_26863c4_56_8d.inc"
};
inline constexpr std::uintptr_t lassoStateDispatchOffset=0x268a214;
inline constexpr std::array<unsigned char,36> lassoStateDispatchFingerprint{
#include "gamecode/VrLasso_268a214_36_8d.inc"
};
inline constexpr std::uintptr_t lassoHitSucceedCountdownOffset=0x268a60c;
inline constexpr std::array<unsigned char,28> lassoHitSucceedCountdownFingerprint{
#include "gamecode/VrLasso_268a60c_28_8d.inc"
};
inline constexpr std::uintptr_t lassoHandleAttachmentGateOffset=0x2688efc;
inline constexpr std::array<unsigned char,44> lassoHandleAttachmentGateFingerprint{
#include "gamecode/VrLasso_2688efc_44_8d.inc"
};
inline constexpr std::uintptr_t zoomLassoReleaseOffset=0x269bd00;
inline constexpr std::array<unsigned char,76> zoomLassoReleaseFingerprint{
#include "gamecode/VrLasso_269bd00_76_8d.inc"
};
inline constexpr std::uintptr_t aniLassoSettersOffset=0x2177578;
inline constexpr std::array<unsigned char,72> aniLassoSettersFingerprint{
#include "gamecode/VrLasso_2177578_72_8d.inc"
};
inline constexpr std::uintptr_t hogTieSetterOffset=0x21775c0;
inline constexpr std::array<unsigned char,20> hogTieSetterFingerprint{
#include "gamecode/VrLasso_21775c0_20_8d.inc"
};
inline constexpr std::uintptr_t checkIsLassoAttachedOffset=0x26ab340;
inline constexpr std::array<unsigned char,68> checkIsLassoAttachedFingerprint{
#include "gamecode/VrLasso_26ab340_68_8d.inc"
};
inline constexpr std::uintptr_t checkIsMiniGameFocusOffset=0x26ab414;
inline constexpr std::array<unsigned char,60> checkIsMiniGameFocusFingerprint{
#include "gamecode/VrLasso_26ab414_60_8d.inc"
};
inline constexpr std::uintptr_t lassoOnUpdateVictimOffset=0x2689b9c;
inline constexpr std::array<unsigned char,52> lassoOnUpdateVictimFingerprint{
#include "gamecode/VrLasso_2689b9c_52_8d.inc"
};
inline constexpr std::uintptr_t lassoOnInitDataOffset=0x2683424;
inline constexpr std::array<unsigned char,32> lassoOnInitDataFingerprint{
#include "gamecode/VrLasso_2683424_32_8d.inc"
};
inline constexpr std::uintptr_t lassoDataParserOffset=0x2176f40;
inline constexpr std::array<unsigned char,64> lassoDataParserFingerprint{
#include "gamecode/VrLasso_2176f40_64_8d.inc"
};
inline constexpr std::uintptr_t lassoProbeFlagsOffset=0x2682af0;
inline constexpr std::array<unsigned char,64> lassoProbeFlagsFingerprint{
#include "gamecode/VrLasso_2682af0_64_8d.inc"
};
// grade O (off-screen, H3)
inline constexpr std::uintptr_t lassoOffscreenBlockOffset=0x2689874;
inline constexpr std::array<unsigned char,532> lassoOffscreenBlockFingerprint{
#include "gamecode/VrLasso_2689874_532_8d.inc"
};
// (print, offset) lists per grade for the install and the host test
struct PrintRef {std::uintptr_t offset;const unsigned char* bytes;std::size_t size;};
inline constexpr std::array<PrintRef,10> launchPrints{PrintRef{lassoOnUseItemOffset,lassoOnUseItemFingerprint.data(),lassoOnUseItemFingerprint.size()},PrintRef{lassoUpdateInFlightHeadOffset,lassoUpdateInFlightHeadFingerprint.data(),lassoUpdateInFlightHeadFingerprint.size()},PrintRef{lassoUpdateInFlightRangeOffset,lassoUpdateInFlightRangeFingerprint.data(),lassoUpdateInFlightRangeFingerprint.size()},PrintRef{lassoInFlightCallSiteOffset,lassoInFlightCallSiteFingerprint.data(),lassoInFlightCallSiteFingerprint.size()},PrintRef{lassoInFlightHitFailOffset,lassoInFlightHitFailFingerprint.data(),lassoInFlightHitFailFingerprint.size()},PrintRef{lassoLockRefreshOffset,lassoLockRefreshFingerprint.data(),lassoLockRefreshFingerprint.size()},PrintRef{lassoOnThrowWeaponOffset,lassoOnThrowWeaponFingerprint.data(),lassoOnThrowWeaponFingerprint.size()},PrintRef{lassoStateDispatchOffset,lassoStateDispatchFingerprint.data(),lassoStateDispatchFingerprint.size()},PrintRef{lassoHitSucceedCountdownOffset,lassoHitSucceedCountdownFingerprint.data(),lassoHitSucceedCountdownFingerprint.size()},PrintRef{lassoHandleAttachmentGateOffset,lassoHandleAttachmentGateFingerprint.data(),lassoHandleAttachmentGateFingerprint.size()}};
inline constexpr std::array<PrintRef,9> readerPrints{PrintRef{zoomLassoReleaseOffset,zoomLassoReleaseFingerprint.data(),zoomLassoReleaseFingerprint.size()},PrintRef{aniLassoSettersOffset,aniLassoSettersFingerprint.data(),aniLassoSettersFingerprint.size()},PrintRef{hogTieSetterOffset,hogTieSetterFingerprint.data(),hogTieSetterFingerprint.size()},PrintRef{checkIsLassoAttachedOffset,checkIsLassoAttachedFingerprint.data(),checkIsLassoAttachedFingerprint.size()},PrintRef{checkIsMiniGameFocusOffset,checkIsMiniGameFocusFingerprint.data(),checkIsMiniGameFocusFingerprint.size()},PrintRef{lassoOnUpdateVictimOffset,lassoOnUpdateVictimFingerprint.data(),lassoOnUpdateVictimFingerprint.size()},PrintRef{lassoOnInitDataOffset,lassoOnInitDataFingerprint.data(),lassoOnInitDataFingerprint.size()},PrintRef{lassoDataParserOffset,lassoDataParserFingerprint.data(),lassoDataParserFingerprint.size()},PrintRef{lassoProbeFlagsOffset,lassoProbeFlagsFingerprint.data(),lassoProbeFlagsFingerprint.size()}};
inline constexpr std::array<PrintRef,1> offscreenPrints{PrintRef{lassoOffscreenBlockOffset,lassoOffscreenBlockFingerprint.data(),lassoOffscreenBlockFingerprint.size()}};
// Rope pull (checked lazily by the tick through env.read; a mismatch = that part off, counted):
// aniLasso::LassoYank() 0x21798dc..0x2179964 (victim guid +0x2c/+0x2e validated, item = +0x30 -> invLassoWeapon::YankLasso(victim): the minigame
// server OnEvent + sagMsgLassoYank to the victim, direction victim -> user) = what LassoActionTask::Begin 0x22addf0 runs for the native yank.
inline constexpr std::uintptr_t lassoYankOffset=0x21798dc;
inline constexpr std::array<unsigned char,136> lassoYankFingerprint{
#include "gamecode/VrLasso_21798dc_136_8d.inc"
};
// sagLassoDrawable::CopyVerletDrawVerts() 0x268fba0: verts (16 B each) -> drawable+0xc0, count -> drawable+0x8c0 (u32), source the verlet cloth
// [drawable+0x8d0]+0xf0. The drawable = [invDrawnLasso+0xb8] (invDrawnLasso::AllocRope 0x2681418, operator new 0x8e0), the drawn lasso =
// [item+0x130] (invLassoWeapon::AllocRope 0x268328c), a 60-vertex strand (InitStrand 0x26812b0).
inline constexpr std::uintptr_t lassoDrawCopyOffset=0x268fba0;
inline constexpr std::array<unsigned char,60> lassoDrawCopyFingerprint{
#include "gamecode/VrLasso_268fba0_60_8d.inc"
};
// Rope reel: invLassoWeapon::UpdateReelIn() 0x268b0c8 (the stock timed reel-in, OnUpdate state 10): the catch's phConstraint =
// item+0x108, its max length = constraint+0x90 (shortened here by frame-dt * ReelInRate data+0x40, floor data+0x48 on foot / +0x4c for a
// user state-4 owner), mirrored to item+0x148; item+0x144 = the drawn length (CalcuateDrawnLength 0x268df04) that ConstraintCallbackHuman
// 0x268d028 rebuilds the constraint with (AttachObjects max(min, +0x144)). UpdateShorten 0x268aecc (states 8/9) trims the drawn rope verts to the
// constraint length. LassoYank only broadcasts sagMsgLassoYank (handled by aiComponentBehaviorAnimal::HandleLassoYank alone: a human victim
// never moves); the reel = the same constraint write the engine does, done per pull. Three prints over UpdateReelIn = the layout the mod writes.
inline constexpr std::uintptr_t reelInOffset=0x268b0c8;
inline constexpr std::array<unsigned char,176> reelInFingerprint{
#include "gamecode/VrLasso_268b0c8_176_8d.inc"
};
inline constexpr std::uintptr_t reelInMinOffset=0x268b1cc;
inline constexpr std::array<unsigned char,16> reelInMinFingerprint{
#include "gamecode/VrLasso_268b1cc_16_8d.inc"
};
inline constexpr std::uintptr_t reelInStoreOffset=0x268b21c;
inline constexpr std::array<unsigned char,8> reelInStoreFingerprint{0,72,1,189,0,145,0,189};
} // namespace elf

namespace off {
inline constexpr std::size_t itemOwner=0x20,itemAim=0xe0,itemTargetGoh=0xf0,itemTargetComp=0xf4,itemVictimIndex=0x260,itemVictimGen=0x262,itemData=0x280,
    itemLead=0x2f0,itemTail=0x300,itemVel=0x310,itemFlight=0x344,itemState=0x3d0,itemAimValid=0x40c,itemForceMiss=0x40d,blockBegin=0xe0,blockEnd=0x418;
inline constexpr std::size_t dataForceAttractor=0x23,dataLaunchVelocity=0x54,dataMaxThrow=0x58,dataProbeRadius=0x68,dataSize=0x88;
inline constexpr std::size_t actorAnim=0x38,animLassoBlock=0xab8,blockLasso=0x40,aniUser=0x18,aniVictim=0x1c,aniHogtie=0x20,aniSize=0x34;
inline constexpr std::size_t actorMover=0x88,moverVelocity=0x7b0,actorComponent=0xb0,compMatrix=0x18,matrixPos=0x30,actorGoh=0x188,actorFlags=0x118,
    actorHealth=0x60,healthValue=0x20,healthFlags=0x150,compRace=0xa30,compRiderLink=0x894,compMountLink=0x8a0,actorBlock=0x190;
inline constexpr std::uint32_t healthDead=0x40;
inline constexpr std::size_t minigameFocus=0x58,proxyBegin=0x10,proxyIndex=0x148,timeDelta=0x8;
// Rope pull: the engine's drawn rope (elf::lassoDrawCopyOffset)
inline constexpr std::size_t itemDrawn=0x130,drawnDrawable=0xb8,drawableVerts=0xc0,drawableCount=0x8c0;inline constexpr unsigned drawableMaxVerts=128;
// Rope reel (elf::reelInOffset)
inline constexpr std::size_t itemConstraint=0x108,itemDrawnLen=0x144,itemConLen=0x148,dataReelMin=0x48,dataReelMinMounted=0x4c,constraintMaxLen=0x90;
} // namespace off

// ============================== engine side (injected; TraceEntry on the device, fakes in host tests) ==============================
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using UseItemFn=bool(*)(void*);using InFlightFn=void(*)(void*);using VisibleFn=bool(*)(const void*,const void*,int);
struct Env {
    ReadFn read{};std::uintptr_t base{};
    std::uintptr_t lassoItemTable{},actorManagerGlobal{},actorSlotsGlobal{},minigameGlobal{},gravityObject{},timeObject{},proxyObject{};
    UseItemFn useItemOriginal{};InFlightFn inFlightOriginal{};VisibleFn visibleOriginal{};
    UseItemFn directUseItem{};                 // H4: invLassoWeapon::OnUseItem after the live vtable word check (null = H4 unavailable)
    std::uintptr_t useItemReturn{},inFlightReturn{};std::array<std::uintptr_t,9> offscreenReturns{};
    explicit operator bool() const noexcept {return read&&lassoItemTable&&actorManagerGlobal&&actorSlotsGlobal;}
};
inline Env env{};
namespace rd {
template<class T> inline bool get(std::uintptr_t p,std::size_t o,T& out) noexcept {
    return env.read&&p&&p<=UINTPTR_MAX-o-sizeof(T)&&env.read(nullptr,p+o,&out,sizeof(T));}
inline bool block(std::uintptr_t p,void* out,std::size_t n) noexcept {return env.read&&p&&p<=UINTPTR_MAX-n&&env.read(nullptr,p,out,n);}
inline bool vread(const diagnostics::ReadSegment* s,std::size_t n) noexcept {try{return diagnostics::guardedReadVector.read(env.read,nullptr,s,n);}catch(...){return false;}}
template<class T> inline T at(const unsigned char* b,std::size_t o) noexcept {T v{};std::memcpy(&v,b+o,sizeof v);return v;}
inline V3 vec(const unsigned char* b,std::size_t o) noexcept {return {at<float>(b,o),at<float>(b,o+4),at<float>(b,o+8)};}
// the hooks' own object (the engine passed it: a live object of this process) - direct field access
template<class T> inline T field(std::uintptr_t item,std::size_t o) noexcept {T v{};std::memcpy(&v,reinterpret_cast<const void*>(item+o),sizeof v);return v;}
template<class T> inline void put(std::uintptr_t item,std::size_t o,T v) noexcept {std::memcpy(reinterpret_cast<void*>(item+o),&v,sizeof v);}
inline V3 fieldV3(std::uintptr_t item,std::size_t o) noexcept {return {field<float>(item,o),field<float>(item,o+4),field<float>(item,o+8)};}
inline void putV3(std::uintptr_t item,std::size_t o,const V3& v) noexcept {put(item,o,v[0]);put(item,o+4,v[1]);put(item,o+8,v[2]);}
// guid -> actor through the aGuidGeneral<aActor> slots (16 B: ptr, u16 generation at +8)
inline std::uintptr_t actorOf(std::uint32_t guid) noexcept {
    std::uintptr_t manager{},slots{};std::uint16_t count{};
    if(!guid||guid==0xffffffffu||!get(env.actorManagerGlobal,0,manager)||!get(manager,0,count)||!get(env.actorSlotsGlobal,0,slots)||!slots)return 0;
    const std::uintptr_t index=guid&0xffffu;if(index>=count)return 0;
    unsigned char e[16]{};if(!block(slots+index*16,e,sizeof e))return 0;
    const auto a=at<std::uintptr_t>(e,0);return a&&at<std::uint16_t>(e,8)==(guid>>16)?a:0;
}
inline bool actorRoot(std::uintptr_t actor,V3& out) noexcept {
    std::uintptr_t comp{},mtx{};float p[3]{};
    if(!get(actor,off::actorComponent,comp)||!comp||!get(comp,off::compMatrix,mtx)||!mtx||!block(mtx+off::matrixPos,p,sizeof p))return false;
    out={p[0],p[1],p[2]};return am::finite(out)&&std::fabs(p[0])<1e6f&&std::fabs(p[1])<1e6f&&std::fabs(p[2])<1e6f;
}
inline bool actorAlive(std::uintptr_t actor) noexcept {
    std::uintptr_t health{};float hp{};std::uint32_t flags{};
    return get(actor,off::actorHealth,health)&&health&&get(health,off::healthValue,hp)&&get(health,off::healthFlags,flags)&&hp>0.f&&!(flags&off::healthDead);
}
inline bool moverVelocity(std::uintptr_t actor,V3& out) noexcept {
    std::uintptr_t mover{};float v[3]{};
    if(!get(actor,off::actorMover,mover)||!mover||!block(mover+off::moverVelocity,v,sizeof v))return false;
    out={v[0],v[1],v[2]};return am::finite(out);
}
} // namespace rd

// ============================== the engine step (UpdateInFlight 0x268a798-0x268a9bc) and its uses ==============================
// Explicit Euler with the OLD velocity (p += v dt; v += g dt), then the homing (+0x40d == 0 branch): T = normalize(target - p), V = v^,
// perp = T - V (T.V); p.x += 100 dt perp.x; p.z += 100 dt perp.z (y and v untouched). Range: |p - root| > MaxThrowDistance ends the flight.
// Double precision; the engine uses floats + invsqrtf_fast (a tiny difference).
inline constexpr unsigned pathCapacity=256;
struct SimIn {V3 p0{},v0{},g{0,-9.8f,0};double dt{1.0/30};double maxTime{2.5};V3 root{};double range{0};bool homing{};V3 target{};};
struct Path {std::array<V3,pathCapacity> p{};unsigned n{};bool rangeCut{};};   // p[0] = p0, p[i] = after step i (p[n..] stale: never read)
// Per call: no path clear (p[n..] is never read), one division per length, the range test squared.
inline void simulate(const SimIn& s,Path& out) noexcept {
    double p[3]{s.p0[0],s.p0[1],s.p0[2]},v[3]{s.v0[0],s.v0[1],s.v0[2]};const double g[3]{s.g[0],s.g[1],s.g[2]};
    out.p[0]=s.p0;out.n=1;out.rangeCut=false;
    if(!(s.dt>1e-4)||!std::isfinite(s.dt))return;
    const unsigned steps=std::min<unsigned>(unsigned(s.maxTime/s.dt),pathCapacity-1);
    const double range2=s.range>0?s.range*s.range:0.0,push=100.0*s.dt;const double tg[3]{s.target[0],s.target[1],s.target[2]},rt[3]{s.root[0],s.root[1],s.root[2]};
    for(unsigned i=0;i<steps;++i){
        for(int k=0;k<3;++k)p[k]+=v[k]*s.dt;
        for(int k=0;k<3;++k)v[k]+=g[k]*s.dt;
        if(s.homing){
            double T[3]{tg[0]-p[0],tg[1]-p[1],tg[2]-p[2]};
            const double tl=std::sqrt(T[0]*T[0]+T[1]*T[1]+T[2]*T[2]),vl=std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
            if(tl>1e-9&&vl>1e-9){const double it=1.0/tl,iv=1.0/vl;for(double& x:T)x*=it;const double V[3]{v[0]*iv,v[1]*iv,v[2]*iv};
                const double d=T[0]*V[0]+T[1]*V[1]+T[2]*V[2];
                p[0]+=push*(T[0]-V[0]*d);p[2]+=push*(T[2]-V[2]*d);}
        }
        out.p[out.n++]={float(p[0]),float(p[1]),float(p[2])};
        if(range2>0){const double dx=p[0]-rt[0],dy=p[1]-rt[1],dz=p[2]-rt[2];
            if(dx*dx+dy*dy+dz*dz>range2){out.rangeCut=true;break;}}
    }
}
// OnUseItem 0x268f474-0x268f514: v0 = (normalize(aim - p0) + (0, 0.1, 0)) * LaunchVelocity + ownerVelocity (Y up); k = 1 (WYSIWYG).
inline V3 launchVelocity(const V3& dir,float lv,float k,const V3& ownerVel) noexcept {
    return {dir[0]*lv*k+ownerVel[0],(dir[1]+.1f)*lv*k+ownerVel[1],dir[2]*lv*k+ownerVel[2]};}
struct Approach {double d{1e30};unsigned i{};double y{};};
// horizontal (x / z) closest approach of samples [1, n) to q (from the first step on, strict improvement)
// (squared distances in the loop, one sqrt at the end)
inline Approach closestN(const Path& path,unsigned n,const V3& q) noexcept {
    Approach a{};double best=1e60;const double qx=q[0],qz=q[2];n=std::min(n,path.n);
    for(unsigned i=1;i<n;++i){const double dx=double(path.p[i][0])-qx,dz=double(path.p[i][2])-qz,h=dx*dx+dz*dz;if(h<best){best=h;a.i=i;a.y=path.p[i][1];}}
    if(best<1e60)a.d=std::sqrt(best);
    return a;}
inline Approach closest(const Path& path,const V3& q) noexcept {return closestN(path,path.n,q);}
inline V3 dirOf(double yawRad,double pitchRad) noexcept {return {float(std::sin(yawRad)*std::cos(pitchRad)),float(std::sin(pitchRad)),float(std::cos(yawRad)*std::cos(pitchRad))};}
inline double yawOf(const V3& d) noexcept {return std::atan2(double(d[0]),double(d[2]));}
inline double pitchOf(const V3& d) noexcept {const double l=std::sqrt(double(d[0])*d[0]+double(d[1])*d[1]+double(d[2])*d[2]);return l>1e-9?std::asin(std::clamp(double(d[1])/l,-1.0,1.0)):0.0;}
// Pitch solve: bisection on the launch pitch (yaw kept) so the loop's height at the target's horizontal closest
// approach equals targetY. err(lo) > 0 or err(hi) < 0 -> unsolved. `homed` = the engine homing toward `target` in the simulation.
// 12 iterations by default (a +-8 deg window -> 0.004 deg = 1.4 mm at 20 m).
struct SolveIn {SimIn sim{};double yawRad{};float lv{};float k{1.f};V3 ownerVel{};V3 target{};float targetY{};bool homed{};unsigned iterations{12};};
inline bool solvePitch(const SolveIn& in,double loDeg,double hiDeg,double& outDeg) noexcept {
    Path path{};
    const auto err=[&](double deg){SimIn s=in.sim;s.v0=launchVelocity(dirOf(in.yawRad,deg*3.14159265358979/180.0),in.lv,in.k,in.ownerVel);
        s.homing=in.homed;s.target=in.target;simulate(s,path);const auto a=closest(path,in.target);return a.d<1e29?a.y-double(in.targetY):1e9;};
    double lo=loDeg,hi=hiDeg;if(err(lo)>0||err(hi)<0)return false;
    for(unsigned i=0;i<in.iterations;++i){const double m=.5*(lo+hi);if(err(m)>0)hi=m;else lo=m;}
    outDeg=.5*(lo+hi);return std::isfinite(outDeg);
}

// ============================== assist candidates and target selection ==============================
struct Candidate {std::uint32_t guid{},goh{};std::uintptr_t actor{};V3 feet{};std::int32_t race{-1};};
inline constexpr unsigned maxCandidates=12;
struct Sizes {float head{1.85f},chest{1.0f};};
inline Sizes sizesOf(std::int32_t race) noexcept {return race==1?Sizes{2.4f,1.2f}:Sizes{1.85f,1.0f};} // [H] human 0, horse 1 (VrMelee)
struct Selection {bool valid{};Candidate c{};V3 point{};bool solved{};double pitchDeg{};unsigned index{};double score{};};
struct SelectIn {const Path* path{};unsigned cutIndex{};V3 p0{};V3 dir{};float radiusUnits{};float probeRadius{};float upm{1.f};float angleDeg{12.f};
                 float pitchWindowDeg{8.f};SolveIn solve{};};
// The cheap part (every candidate): yaw within angle, horizontal closest approach on the unhomed path <= radius before the cut.
inline bool prefilter(const SelectIn& s,const Candidate& c,Approach& a) noexcept {
    if(!s.path||s.path->n<2)return false;
    const V3 to=am::sub(c.feet,s.p0);const double bearing=std::atan2(double(to[0]),double(to[2]));
    double dy=yawOf(s.dir)-bearing;while(dy>3.14159265358979)dy-=6.28318530717959;while(dy<-3.14159265358979)dy+=6.28318530717959;
    if(std::fabs(dy)>s.angleDeg*3.14159265358979/180.0)return false;
    a=closestN(*s.path,s.cutIndex+1,c.feet);
    return a.d<=s.radiusUnits;
}
// The last pitch solve per candidate is reused while nothing that decides it moved - the
// aimed yaw / pitch within 0.3 deg, the target relative to p0 within 5 cm, the owner velocity within 0.1 m/s, same LaunchVelocity / dt /
// window / race. The arc is always re-simulated from the (reused) pitch, so what is shown is still exactly what H1 launches.
struct SolveMemo {
    bool valid{},solved{};std::uint32_t guid{};std::int32_t race{-1};double yaw{},pitch{},deg{},dt{};V3 rel{},ownerVel{};float lv{},window{},upm{1.f};
    bool matches(const SolveMemo& o) const noexcept {
        constexpr double tol=.3*3.14159265358979/180.0;double dyaw=yaw-o.yaw;while(dyaw>3.14159265358979)dyaw-=6.28318530717959;while(dyaw<-3.14159265358979)dyaw+=6.28318530717959;
        return valid&&o.valid&&guid==o.guid&&race==o.race&&lv==o.lv&&window==o.window&&upm==o.upm&&std::fabs(dt-o.dt)<1e-6&&std::fabs(dyaw)<tol&&
            std::fabs(pitch-o.pitch)<tol&&am::distance(rel,o.rel)<=.05f*upm&&am::distance(ownerVel,o.ownerVel)<=.1f*upm;
    }
};
// One candidate: the prefilter, then vertically reachable (height in [feet - r, head + r]) or a pitch solve within +-window of the aimed pitch.
// memo (optional): in = the previous solve of this candidate (reused when it matches), out = this frame's; solve (optional): -1 no solve
// (prefilter / window 0), 0 solved now, 1 reused from the memo.
inline bool evaluate(const SelectIn& s,const Candidate& c,Selection& out,SolveMemo* memo=nullptr,int* solve=nullptr) noexcept {
    if(solve)*solve=-1;
    Approach a{};if(!prefilter(s,c,a))return false;
    const Sizes z=sizesOf(c.race);const double h=a.y-c.feet[1];
    Selection r{};r.c=c;r.point={c.feet[0],c.feet[1]+z.chest*s.upm,c.feet[2]};r.index=a.i;r.score=a.d;r.valid=true;
    SolveIn si=s.solve;si.target=r.point;si.targetY=r.point[1];si.yawRad=yawOf(s.dir);
    const double aimedRad=pitchOf(s.dir),aimed=aimedRad*180.0/3.14159265358979;double deg{};bool solved=false;
    if(s.pitchWindowDeg>0.f){
        SolveMemo now{};now.valid=true;now.guid=c.guid;now.race=c.race;now.yaw=si.yawRad;now.pitch=aimedRad;now.dt=si.sim.dt;now.rel=am::sub(r.point,s.p0);
        now.ownerVel=si.ownerVel;now.lv=si.lv;now.window=s.pitchWindowDeg;now.upm=s.upm;
        if(memo&&memo->matches(now)){solved=memo->solved;deg=memo->deg;if(solve)*solve=1;}
        else{solved=solvePitch(si,aimed-s.pitchWindowDeg,aimed+s.pitchWindowDeg,deg);now.solved=solved;now.deg=deg;if(memo)*memo=now;if(solve)*solve=0;}
    }
    if(solved){r.solved=true;r.pitchDeg=deg;}
    else if(!(h>=-double(s.probeRadius)&&h<=double(z.head*s.upm+s.probeRadius)))return false;
    out=r;return true;
}

// ============================== the module (main thread, ModuleLasso = 8) ==============================
inline std::atomic<std::uint64_t> mainThreadToken{0};
inline std::uint64_t threadToken() noexcept {static thread_local char t;return reinterpret_cast<std::uint64_t>(&t);}
inline void noteHookThread() noexcept {const auto m=mainThreadToken.load(std::memory_order_relaxed);bump(m&&m==threadToken()?stats.hookMain:stats.hookOther);}
struct FlightBox {std::uint64_t ns{};float p[3]{},v[3]{},gMeasured[3]{};std::uint32_t state{},flight{},steps{},token{};};
inline body::Seqlock<FlightBox> flightBox;
struct Readout {   // the tick's engine reads (report)
    bool item{},ownerOk{},data{},ani{},victim{},mover{},root{},minigame{},prompt{},gravity{},time{};
    std::uint32_t lassoState{},flight{},userState{},victimState{},hogtie{},promptCode{32};std::uint8_t aimValid{},forceMiss{},forceAttractor{};
    float launchVelocity{},maxThrow{},probeRadius{},dt{};V3 gravityV{0,-9.8f,0},aim{},lead{},vel{},moverVel{},rootPos{};std::int32_t focus{-1};
    std::uint32_t victimGuid{0xffffffffu};unsigned reads{};
    std::uintptr_t aniPtr{},mgInst{},drawn{};   // Rope pull: the player's aniLasso, the minigame server instance, item+0x130
    // Rope reel: the item, its catch constraint (item+0x108), the drawn / constraint lengths (+0x144 / +0x148), the reel floors (data+0x48/+0x4c)
    std::uintptr_t itemPtr{},constraint{};float drawnLen{},conLen{},reelMinFoot{},reelMinMount{};bool mounted{};
};
class Module {
public:
    bool installed{};float dtEma{0.f};Readout last{};
    std::array<Candidate,maxCandidates> cand{};unsigned candCount{};std::uint64_t lastScanNs{};
    std::atomic<const char*> noteNow{"NOT OWNED"};std::atomic<float> lastLv{0.f},lastRange{0.f},lastRadius{0.f},lastDtMs{0.f};
    std::atomic<std::uint32_t> lastPromptCode{32};std::atomic<int> lastRace{-1};
    // Rope pull (report): the geometry source (ENGINE / LINE / NONE), engine verts, the hand / victim end distances, the last grab distance,
    // the lazily checked engine prints (0 unchecked, 1 ok, 2 mismatch / unreadable) of LassoYank / CopyVerletDrawVerts, the last pull (m)
    std::atomic<const char*> ropeSource{"NONE"};std::atomic<unsigned> ropeVerts{0};std::atomic<float> ropeHandEndCm{0.f},ropeVictimEndCm{0.f},
        lastGrabCm{0.f},lastPullCm{0.f},lastPullSpeed{0.f};std::atomic<int> yankPrint{0},drawPrint{0};
    // Rope reel (report): the UpdateReelIn prints (0/1/2), the constraint length / target / floor, the victim distance and the gain
    // (victim distance at the pull minus 400 ms later; > 0 = the victim came closer)
    std::atomic<int> reelPrint{0};std::atomic<float> reelLenCm{0.f},reelTargetCm{0.f},reelMinCm{0.f},reelVictimCm{0.f},lastReelGainCm{0.f};
    void tick(const mainthread::Frame& fr) noexcept;
    bool reelActiveNow() const noexcept {return reelActive_;}
    void resetForTests() noexcept {this->~Module();::new(this) Module{};}
private:
    arc::Tracer tracer_{};bool armed_{},shown_{},frozenValid_{};hs::ArcFrame lastFrame_{};V3 smooth_{},frozenDir_{};bool smoothValid_{};std::uint64_t smoothNs_{};
    std::uint64_t commitSeq_{},shownSeq_{};Selection target_{};std::uint64_t switchSince_{};std::uint32_t switchGuid_{};
    V3 p0Checked_{};V3 p0Fixed_{};bool p0CheckValid_{};std::uint64_t p0CheckWindow_{};
    std::uint32_t prevLasso_{99},prevUser_{99},prevVictim_{99};bool prevMinigame_{};int prevCode_{-1};std::uint64_t detachedSince_{},lastTickNs_{};
    V3 prevRoot_{};bool prevRootValid_{};
    std::array<SolveMemo,maxCandidates+1> memo_{};unsigned memoNext_{};   // The last pitch solve per candidate (reused while nothing moved)
    SolveMemo* memoFor(std::uint32_t guid) noexcept {
        SolveMemo* fresh=nullptr;for(auto& m:memo_){if(m.valid&&m.guid==guid)return &m;if(!m.valid&&!fresh)fresh=&m;}
        if(!fresh){fresh=&memo_[memoNext_++%memo_.size()];*fresh=SolveMemo{};}
        return fresh;}
    bool evaluateMemo(const SelectIn& s,const Candidate& c,Selection& out) noexcept {
        int solve=-1;const bool ok=evaluate(s,c,out,s.pitchWindowDeg>0.f?memoFor(c.guid):nullptr,&solve);
        if(solve==0)bump(stats.solves);else if(solve==1)bump(stats.solvesReused);
        return ok;}
    void readEngine(const PlayerLink& pl,Readout& r) noexcept;
    void scan(const PlayerLink& pl,const Readout& r,std::uint64_t now) noexcept;
    void hideArc() noexcept {if(shown_){hs::clearArc(hs::ArcChannel::Lasso);shown_=false;bump(stats.arcHidden);}tracer_.reset();frozenValid_=false;target_=Selection{};lastFrame_.count=0;}
    void produce(const Intent& in,const PlayerLink& pl,const Readout& r,std::uint64_t now) noexcept;
    void direct(const PlayerLink& pl,std::uint64_t now) noexcept;
    void note(const PlayerLink& pl,bool plValid,bool attached,std::uint64_t now) noexcept;
    struct RopeGrab {bool on{};V3 anchorRel{},dir{};float lastPull{},sinceYank{},speed{};std::uint64_t lastNs{},nextHaptic{};};
    std::array<RopeGrab,2> grab_{};std::array<bool,2> wasClosed_{{true,true}};std::uint64_t lastYankNs_{};float pullCmAcc_{};bool engaged_{};
    V3 holdAnchor_{};bool holdAnchorOk_{};
    using YankFn=bool(*)(void*);YankFn yankFn_{};
    // Rope reel: the constraint being shortened, its target length (world units), ramping; the gain probe
    std::uintptr_t reelCon_{};float reelTarget_{};bool reelActive_{};std::uint64_t reelLastNs_{},reelProbeNs_{};float reelDistAtPull_{};
    static constexpr unsigned polyMax=64;std::array<V3,polyMax> poly_{};std::array<float,polyMax> along_{};unsigned polyN_{};
    std::array<float,off::drawableMaxVerts*4> ropeRaw_{};   // the engine's drawn verts of this tick (x, y, z, w)
    void ropeReset(bool leftCatch) noexcept;
    void ropePrints() noexcept;
    bool ropeGeometry(const Readout& r,const V3& holdHand,bool holdOk,float upm,std::uintptr_t& drawable,unsigned& count,unsigned& handEnd) noexcept;
    void ropeFollow(std::uintptr_t drawable,unsigned count,unsigned handEnd,const V3& to,float upm) noexcept;
    void ropeYank(const Readout& r,bool minigame,std::uint64_t now) noexcept;
    bool reelUsable(const Readout& r) const noexcept;
    bool reelVictimDist(const Readout& r,float& dist) const noexcept;
    void reelPull(const Readout& r,std::uint64_t now) noexcept;
    void reelTick(const Readout& r,std::uint64_t now) noexcept;
    void rope(const Readout& r,bool attached,bool minigame,std::uint64_t now) noexcept;
};
inline Module module;

inline void Module::readEngine(const PlayerLink& pl,Readout& r) noexcept {
    r=Readout{};using diagnostics::ReadSegment;
    alignas(8) unsigned char head[0x28]{},blk[off::blockEnd-off::blockBegin]{};std::uintptr_t a38{},mover{},comp{},mgInst{};float grav[3]{},dt{};
    std::uintptr_t proxyRange[2]{};std::uint32_t proxyIdx{};
    const std::uintptr_t item=pl.slotItem?pl.slotItem:0;
    // phase A: the item's head + its 0xe0..0x418 block, the actor's first links, the globals
    std::array<ReadSegment,10> s{};unsigned n=0;
    if(item){s[n++]={item,head,sizeof head};s[n++]={item+off::blockBegin,blk,sizeof blk};}
    s[n++]={pl.actor+off::actorAnim,&a38,8};s[n++]={pl.actor+off::actorMover,&mover,8};s[n++]={pl.actor+off::actorComponent,&comp,8};
    if(env.minigameGlobal)s[n++]={env.minigameGlobal,&mgInst,8};
    if(env.gravityObject)s[n++]={env.gravityObject,grav,sizeof grav};
    if(env.timeObject)s[n++]={env.timeObject+off::timeDelta,&dt,4};
    if(env.proxyObject){s[n++]={env.proxyObject+off::proxyBegin,proxyRange,sizeof proxyRange};s[n++]={env.proxyObject+off::proxyIndex,&proxyIdx,4};}
    r.reads+=1;
    if(!rd::vread(s.data(),n)){for(unsigned i=0;i<n;++i)if(!rd::block(s[i].source,s[i].target,s[i].size))std::memset(s[i].target,0,s[i].size);r.reads+=n;}
    if(item){
        r.item=rd::at<std::uintptr_t>(head,0)==env.lassoItemTable;r.ownerOk=r.item&&rd::at<std::uint32_t>(head,off::itemOwner)==pl.guid;
        if(r.item){const auto B=[&](std::size_t o){return o-off::blockBegin;};
            r.aim=rd::vec(blk,B(off::itemAim));r.lead=rd::vec(blk,B(off::itemLead));r.vel=rd::vec(blk,B(off::itemVel));
            r.flight=blk[B(off::itemFlight)];r.lassoState=rd::at<std::uint32_t>(blk,B(off::itemState));r.aimValid=blk[B(off::itemAimValid)];
            r.forceMiss=blk[B(off::itemForceMiss)];
            const std::uint32_t vi=rd::at<std::uint16_t>(blk,B(off::itemVictimIndex)),vg=rd::at<std::uint16_t>(blk,B(off::itemVictimGen));
            r.victimGuid=vi==0xffffu?0xffffffffu:(vi|(vg<<16));
            r.drawn=rd::at<std::uintptr_t>(blk,B(off::itemDrawn));
            r.itemPtr=item;r.constraint=rd::at<std::uintptr_t>(blk,B(off::itemConstraint));r.drawnLen=rd::at<float>(blk,B(off::itemDrawnLen));
            r.conLen=rd::at<float>(blk,B(off::itemConLen));r.mounted=pl.riding!=0;
            const auto data=rd::at<std::uintptr_t>(blk,B(off::itemData));
            alignas(8) unsigned char d[off::dataSize]{};
            if(data&&rd::block(data,d,sizeof d)){r.data=true;r.forceAttractor=d[off::dataForceAttractor];r.launchVelocity=rd::at<float>(d,off::dataLaunchVelocity);
                r.maxThrow=rd::at<float>(d,off::dataMaxThrow);r.probeRadius=rd::at<float>(d,off::dataProbeRadius);
                r.reelMinFoot=rd::at<float>(d,off::dataReelMin);r.reelMinMount=rd::at<float>(d,off::dataReelMinMounted);
                if(!std::isfinite(r.launchVelocity)||r.launchVelocity<0.f||r.launchVelocity>1000.f)r.launchVelocity=0.f;
                if(!std::isfinite(r.maxThrow)||r.maxThrow<0.f||r.maxThrow>1000.f)r.maxThrow=0.f;
                if(!std::isfinite(r.probeRadius)||r.probeRadius<0.f||r.probeRadius>10.f)r.probeRadius=0.f;}
            ++r.reads;
        }
    }
    // phase B: the second links (guarded per link, CheckIsLassoAttached 0x26ab350: [[[actor+0x38]+0xab8]+0x40])
    std::uintptr_t ab8{},mtx{},lassoPtr{};float vel[3]{},pos[3]{};std::int32_t focus{-1};std::uint32_t code{32};
    if(a38&&rd::get(a38,off::animLassoBlock,ab8)&&ab8&&rd::get(ab8,off::blockLasso,lassoPtr)&&lassoPtr){
        alignas(8) unsigned char ani[off::aniSize]{};
        if(rd::block(lassoPtr,ani,sizeof ani)){r.ani=true;r.aniPtr=lassoPtr;r.userState=rd::at<std::uint32_t>(ani,off::aniUser);r.hogtie=rd::at<std::uint32_t>(ani,off::aniHogtie);}
        r.reads+=4;}
    if(mover&&rd::block(mover+off::moverVelocity,vel,sizeof vel)){r.moverVel={vel[0],vel[1],vel[2]};r.mover=am::finite(r.moverVel);++r.reads;}
    if(comp&&rd::get(comp,off::compMatrix,mtx)&&mtx&&rd::block(mtx+off::matrixPos,pos,sizeof pos)){r.rootPos={pos[0],pos[1],pos[2]};r.root=am::finite(r.rootPos);r.reads+=2;}
    r.mgInst=mgInst;
    if(env.minigameGlobal){if(!mgInst){r.minigame=true;r.focus=-1;}else if(rd::get(mgInst,off::minigameFocus,focus)){r.minigame=true;r.focus=focus;++r.reads;}}
    if(env.proxyObject){const auto b=proxyRange[0],e=proxyRange[1];
        if(b&&e>=b&&proxyIdx<(e-b)/8){if(rd::get(b+std::uintptr_t(proxyIdx)*8+4,0,code)){r.prompt=true;r.promptCode=code<=32?code:32;++r.reads;}}
        else{r.prompt=true;r.promptCode=32;}}
    if(env.gravityObject){r.gravityV={grav[0],grav[1],grav[2]};r.gravity=am::finite(r.gravityV)&&am::length(r.gravityV)>.1f&&am::length(r.gravityV)<1000.f;
        if(!r.gravity)r.gravityV={0,-9.8f,0};}
    if(env.timeObject){r.dt=dt;r.time=std::isfinite(dt)&&dt>1e-4f&&dt<.5f;}
    // the victim (OnUpdate 0x2689ba4: item+0x260 / +0x262 validated against the actor table) and its aniLasso victim state
    if(r.victimGuid!=0xffffffffu){if(const auto va=rd::actorOf(r.victimGuid)){std::uintptr_t v38{},vab8{},vl{};
        if(rd::get(va,off::actorAnim,v38)&&v38&&rd::get(v38,off::animLassoBlock,vab8)&&vab8&&rd::get(vab8,off::blockLasso,vl)&&vl&&
           rd::get(vl,off::aniVictim,r.victimState)){r.victim=true;std::uint32_t hog{};if(rd::get(vl,off::aniHogtie,hog)&&hog)r.hogtie=hog;}
        r.reads+=5;}}
}

// Actor scan, batched like VrMelee::scan - the slot table,
// then ONE vector read per stage (actor blocks, component matrix pointers, matrix positions, then race / rider links / health of the actors in
// range), a failed batch falls back to per-segment reads (one unreadable actor never hides the others).
// Only called with ASSIST ON in SWING / TRIGGER (the candidates are used nowhere else).
namespace scanlim {inline constexpr unsigned slots=1024,actors=256,near=32;} // VrMelee maxSlots / maxActors
struct ScanScratch {
    std::array<unsigned char,scanlim::slots*16> slots{};std::array<unsigned char,scanlim::actors*off::actorBlock> actor{};
    std::array<diagnostics::ReadSegment,scanlim::actors> segs{};std::array<std::uintptr_t,scanlim::actors> addr{},mtx{};std::array<std::uint32_t,scanlim::actors> index{};
    std::array<std::array<float,3>,scanlim::actors> pos{};
    struct Pre {std::uintptr_t actor{},comp{},health{};std::uint32_t guid{},goh{};};std::array<Pre,scanlim::actors> pre{};
    struct Late {std::int32_t race{-1};std::uint32_t links[4]{};float hp{};std::uint32_t flags{};};std::array<Late,scanlim::near> late{};
    std::array<diagnostics::ReadSegment,scanlim::near*4> lateSegs{};
};
inline ScanScratch scanScratch;   // main thread only (ModuleLasso)
inline void Module::scan(const PlayerLink& pl,const Readout& r,std::uint64_t now) noexcept {
    const auto t0=nowNs();bump(stats.scans);lastScanNs=now;candCount=0;auto& B=scanScratch;unsigned calls=0;
    const auto batch=[&](diagnostics::ReadSegment* s,unsigned n,int fill){ // all-or-nothing vector read, then per-segment fallback
        if(!n)return;++calls;if(rd::vread(s,n))return;
        for(unsigned k=0;k<n;++k){++calls;if(!rd::block(s[k].source,s[k].target,s[k].size)){std::memset(s[k].target,fill,s[k].size);s[k].size=0;}}};
    std::uintptr_t manager{},slots{};std::uint16_t count{};
    if(!rd::get(env.actorManagerGlobal,0,manager)||!rd::get(manager,0,count)||!rd::get(env.actorSlotsGlobal,0,slots)||!slots)return;
    calls+=3;
    const unsigned nSlots=std::min<unsigned>(count,scanlim::slots);
    if(!nSlots||!rd::block(slots,B.slots.data(),std::size_t(nSlots)*16))return;
    ++calls;
    std::uint32_t playerLinks[4]{};std::uintptr_t pcomp{};
    if(rd::get(pl.actor,off::actorComponent,pcomp)&&pcomp)rd::block(pcomp+off::compRiderLink,playerLinks,sizeof playerLinks);
    calls+=2;
    // stage 1: the actor blocks
    unsigned m=0;
    for(unsigned i=0;i<nSlots&&m<scanlim::actors;++i){const auto a=rd::at<std::uintptr_t>(B.slots.data(),std::size_t(i)*16);if(!a||a==pl.actor)continue;
        B.index[m]=i;B.addr[m]=a;B.segs[m]={a,B.actor.data()+std::size_t(m)*off::actorBlock,off::actorBlock};++m;}
    batch(B.segs.data(),m,0);
    unsigned q=0;
    for(unsigned k=0;k<m;++k){
        if(!B.segs[k].size)continue;const unsigned char* ab=B.actor.data()+std::size_t(k)*off::actorBlock;const unsigned i=B.index[k];
        const auto guid=rd::at<std::uint32_t>(ab,8);const auto gen=rd::at<std::uint16_t>(B.slots.data(),std::size_t(i)*16+8);
        if((guid&0xffffu)!=i||(guid>>16)!=gen||guid==pl.guid||(rd::at<std::uint32_t>(ab,off::actorFlags)&1u))continue;
        const auto comp=rd::at<std::uintptr_t>(ab,off::actorComponent),health=rd::at<std::uintptr_t>(ab,off::actorHealth);if(!comp||!health)continue;
        B.pre[q++]={B.addr[k],comp,health,guid,rd::at<std::uint32_t>(ab,off::actorGoh)};
    }
    // stage 2: the component's matrix pointer; stage 3: the matrix translation (actorRoot)
    for(unsigned k=0;k<q;++k){B.mtx[k]=0;B.segs[k]={B.pre[k].comp+off::compMatrix,&B.mtx[k],sizeof(std::uintptr_t)};}
    batch(B.segs.data(),q,0);
    std::array<unsigned,scanlim::actors> live{};unsigned w=0;
    for(unsigned k=0;k<q;++k)if(B.mtx[k]){live[w]=k;B.segs[w]={B.mtx[k]+off::matrixPos,B.pos[k].data(),sizeof(float)*3};++w;}
    batch(B.segs.data(),w,0xff);
    const float range=(r.maxThrow>0.f?r.maxThrow:25.f)+2.f;
    struct Near {unsigned k{};V3 feet{};float d{};};std::array<Near,scanlim::near> near{};unsigned nn=0;
    for(unsigned j=0;j<w;++j){
        if(!B.segs[j].size)continue;const unsigned k=live[j];const auto& p=B.pos[k];
        const V3 feet{p[0],p[1],p[2]};if(!am::finite(feet)||std::fabs(p[0])>=1e6f||std::fabs(p[1])>=1e6f||std::fabs(p[2])>=1e6f)continue;
        const float d=am::distance(feet,r.rootPos);if(!(d<=range))continue;
        if(nn<near.size())near[nn++]={k,feet,d};
        else{unsigned x=0;for(unsigned t=1;t<nn;++t)if(near[t].d>near[x].d)x=t;if(d<near[x].d)near[x]={k,feet,d};}
    }
    // stage 4: race, rider / mount links, health value / flags of the actors in range
    unsigned ls=0;
    for(unsigned t=0;t<nn;++t){const auto& pr=B.pre[near[t].k];auto& lt=B.late[t];lt=ScanScratch::Late{};
        B.lateSegs[ls++]={pr.comp+off::compRace,&lt.race,sizeof lt.race};B.lateSegs[ls++]={pr.comp+off::compRiderLink,lt.links,sizeof lt.links};
        B.lateSegs[ls++]={pr.health+off::healthValue,&lt.hp,sizeof lt.hp};B.lateSegs[ls++]={pr.health+off::healthFlags,&lt.flags,sizeof lt.flags};}
    batch(B.lateSegs.data(),ls,0);
    struct Keep {Candidate c{};float d{};};std::array<Keep,scanlim::near> keepList{};unsigned nk=0;
    for(unsigned t=0;t<nn;++t){
        const auto& lt=B.late[t];const auto& pr=B.pre[near[t].k];
        if(!B.lateSegs[t*4].size||!B.lateSegs[t*4+1].size||lt.race<0||lt.race>17)continue;
        if(pr.guid==playerLinks[0]||pr.guid==playerLinks[3]||lt.links[0]==pl.guid||lt.links[3]==pl.guid)continue; // own mount / rider
        if(!B.lateSegs[t*4+2].size||!B.lateSegs[t*4+3].size||!(lt.hp>0.f)||(lt.flags&off::healthDead))continue;      // alive (rd::actorAlive)
        Candidate c{};c.guid=pr.guid;c.goh=pr.goh;c.actor=pr.actor;c.feet=near[t].feet;c.race=lt.race;keepList[nk++]={c,near[t].d};
    }
    std::sort(keepList.begin(),keepList.begin()+nk,[](const Keep& x,const Keep& y){return x.d<y.d;});
    const unsigned keep=std::min<unsigned>({nk,config.scanMax,maxCandidates});
    for(unsigned k=0;k<keep;++k)cand[k]=keepList[k].c;candCount=keep;bump(stats.candidates,keep);bump(stats.scanReads,calls);
    const auto us=(nowNs()-t0)/1000u;stats.scanUsSum.fetch_add(us,std::memory_order_relaxed);maxOf(stats.scanUsMax,us);
}

// The shown start p0 = the throw hand's start point (BODY IK hand when it drives the arm) + aim * 0.08; chest -> p0 checked with
// arc::probe (user LASSO; on a hit p0 = hit - 5 cm); clamped to <= 1.0 m from the chest (root + 1.35 m up [H]). The arc is drawn from it and the
// plan copies it (p0Rel = p0 - root).
inline void Module::produce(const Intent& in,const PlayerLink& pl,const Readout& r,std::uint64_t now) noexcept {
    (void)pl;auto& c=arc::frameContext(now);bump(stats.arcFrames);
    if(!c.world()||!c.inputValid()||!r.root){hideArc();return;}
    if(in.frozen&&lastFrame_.count){ // LAUNCH_PENDING: the committed arc stays on screen as it was shown (re-keyed, no new trace)
        lastFrame_.key=c.key();if(lastFrame_.key){hs::publishArc(hs::ArcChannel::Lasso,lastFrame_);shown_=true;bump(stats.arcPublished);}return;}
    const unsigned th=in.throwHand<2?in.throwHand:1u;V3 o{},aim{};
    if(!c.aimRay(th,o,aim)){hideArc();return;}
    arc::MapInputs map{};if(!c.map(map)){hideArc();return;}
    const float upm=std::max(c.upm(),.01f);const V3 up{0,1,0};
    if(in.gaze){ // .lasso.swing.aim gaze: the HMD forward (LOCAL -> world), pitch clamped -45 .. +40 deg
        V3 g=am::normalized(am::xformDir(map.localToWorld,{in.headFwd[0],in.headFwd[1],in.headFwd[2]}));
        if(am::length(g)>.5f){double p=pitchOf(g);p=std::clamp(p,-45.0*3.14159265358979/180.0,40.0*3.14159265358979/180.0);aim=dirOf(yawOf(g),p);}
    }
    // 60 ms smoothing of the live aim; the commit freezes it (SWING: hand speed > swing.commit; TRIGGER: the full press)
    {const double dt=smoothNs_&&now>smoothNs_?double(now-smoothNs_)*1e-9:0.0;smoothNs_=now;
     const float a=smoothValid_&&dt>0.0?float(1.0-std::exp(-dt/.06)):1.f;smooth_=smoothValid_?am::normalized(am::lerp(smooth_,aim,a)):aim;smoothValid_=true;}
    V3 dir=smooth_;
    if(in.committed){if(in.commitSeq!=commitSeq_||!frozenValid_){commitSeq_=in.commitSeq;frozenDir_=dir;frozenValid_=true;}dir=frozenDir_;}
    else{commitSeq_=in.commitSeq;frozenValid_=false;}
    // the shown start
    V3 start{};if(c.startPoint(th,start)==arc::StartSource::None)start=o;
    V3 p0=am::add(start,am::mul(aim,.08f*upm));
    const V3 chest=am::add(r.rootPos,am::mul(up,1.35f*upm));
    {const V3 d=am::sub(p0,chest);const float l=am::length(d);if(l>1.f*upm){p0=am::add(chest,am::mul(d,1.f*upm/l));bump(stats.p0Clamped);}}
    if(!p0CheckValid_||am::distance(p0,p0Checked_)>.03f*upm||c.window()>=p0CheckWindow_+8||c.window()<p0CheckWindow_){ // moved > 3 cm or every 8th window
        arc::ProbeQuery q{};q.from=chest;q.to=p0;q.include=0x500208;q.select=arc::Select::AnySurface;arc::Hit h{};
        const auto pr=arc::probe(c,arc::ProbeUser::Lasso,q,h);
        if(pr==arc::ProbeResult::Clear){p0Checked_=p0;p0Fixed_=p0;p0CheckValid_=true;p0CheckWindow_=c.window();}
        else if(pr==arc::ProbeResult::Hit){const V3 d=am::normalized(am::sub(p0,chest));p0Fixed_=am::sub(h.point,am::mul(d,.05f*upm));p0Checked_=p0;
            p0CheckValid_=true;p0CheckWindow_=c.window();bump(stats.p0WallHits);}
        else if(!p0CheckValid_){p0Fixed_=p0;} // no budget / failure: keep the last verdict (or the unchecked point once)
    }
    if(p0CheckValid_&&am::distance(p0,p0Checked_)<=.03f*upm)p0=p0Fixed_;
    const float lv=r.launchVelocity;
    if(!(lv>0.f)){bump(stats.noLaunchVelocity);hideArc();return;}
    // NATIVE: the engine's own target (+0xe0 when +0x40c) or the aim ray
    if(in.mode==2&&r.aimValid&&am::finite(r.aim)){const V3 d=am::normalized(am::sub(r.aim,p0));if(am::length(d)>.5f)dir=d;}
    const float k=1.f;const V3 g=r.gravityV;const double dt=dtEma>1e-4f?double(dtEma):1.0/30;
    const double range=double(r.maxThrow>0.f?r.maxThrow:25.f*upm);
    SimIn si{};si.p0=p0;si.g=g;si.dt=dt;si.maxTime=2.5;si.root=r.rootPos;si.range=range;si.v0=launchVelocity(dir,lv,k,r.moverVel);
    static Path path{};simulate(si,path);
    // assist (ASSIST ON, SWING / TRIGGER): target selection on the unhomed path, the pitch solve on the homed one, then the homed path
    bool haveTarget=false;Selection sel{};
    if(settings().lassoAssist()&&in.mode!=2&&candCount){
        SelectIn s{};s.path=&path;s.cutIndex=path.n-1;s.p0=p0;s.dir=dir;s.radiusUnits=config.assistRadiusM*upm;s.probeRadius=r.probeRadius;s.upm=upm;
        s.angleDeg=config.assistAngleDeg;s.pitchWindowDeg=config.assistPitchDeg;
        s.solve.sim=si;s.solve.lv=lv;s.solve.k=k;s.solve.ownerVel=r.moverVel;s.solve.homed=true;
        // the cheap prefilter for every candidate, the (pitch-solving) evaluation for the 3 best only (+ the current target): lasso tick budget
        std::array<std::pair<double,unsigned>,maxCandidates> order{};unsigned no=0;
        for(unsigned i=0;i<candCount;++i){Approach a{};if(prefilter(s,cand[i],a))order[no++]={a.d,i};}
        std::sort(order.begin(),order.begin()+no,[](const auto& x,const auto& y){return x.first<y.first;});
        // Every candidate is evaluated at most once per tick (the current target is not solved again when it was one of the
        // best), and a solve is reused from the memo while the aim / geometry did not move (evaluateMemo, counters arc.solves / solvesReused)
        Selection best{};std::array<std::uint32_t,3> tried{};std::array<bool,3> triedOk{};unsigned nt=0;
        for(unsigned j=0;j<no&&j<3&&!best.valid;++j){const auto& cd=cand[order[j].second];Selection e{};const bool ok=evaluateMemo(s,cd,e);
            tried[nt]=cd.guid;triedOk[nt++]=ok;if(ok)best=e;}
        // hysteresis: keep the current target unless another is 30 % closer for 150 ms
        Selection cur{};bool curOk=false;
        if(target_.valid){bool done=false;
            for(unsigned j=0;j<nt;++j)if(tried[j]==target_.c.guid){done=true;if(triedOk[j]){cur=best;curOk=true;}break;}
            if(!done)for(unsigned i=0;i<candCount;++i)if(cand[i].guid==target_.c.guid){curOk=evaluateMemo(s,cand[i],cur);break;}}
        if(curOk&&best.valid&&best.c.guid!=cur.c.guid){
            if(best.score<.7*cur.score){if(switchGuid_!=best.c.guid){switchGuid_=best.c.guid;switchSince_=now;}
                if(now-switchSince_>=msNs(150)){sel=best;bump(stats.targetSwitches);switchGuid_=0;}else sel=cur;}
            else{sel=cur;switchGuid_=0;}
        }else if(curOk)sel=cur;else sel=best;
        haveTarget=sel.valid;
    }
    if(!haveTarget)target_=Selection{};
    else{
        if(!target_.valid||target_.c.guid!=sel.c.guid)bump(stats.targets);
        target_=sel;
        if(sel.solved){bump(stats.pitchSolved);dir=dirOf(yawOf(dir),sel.pitchDeg*3.14159265358979/180.0);}else bump(stats.pitchUnsolved);
        si.v0=launchVelocity(dir,lv,k,r.moverVel);si.homing=true;si.target=sel.point;simulate(si,path);
    }
    if(path.rangeCut)bump(stats.outOfRange);
    // resample to <= 64 drawn points (the engine steps: 75..90 for 2.5 s)
    std::array<V3,hs::arcMaxPoints> pts{};unsigned np=0;
    {const unsigned n=path.n;const unsigned stride=std::max(1u,(n+hs::arcMaxPoints-2)/(hs::arcMaxPoints-1));
     for(unsigned i=0;i<n&&np<hs::arcMaxPoints-1;i+=stride)pts[np++]=path.p[i];
     if(np&&(n-1)%stride)pts[np++]=path.p[n-1];}
    // the engine probe: sTestCapsule include 0x100028, the live ProbeCapsulseRadius (LassoProbe 0x2682af0..0x2682b28); no capsule -> segment fallback
    arc::TraceConfig cfg{};const auto* api=arc::engineApi.load(std::memory_order_acquire);
    const bool capsule=config.probe==ProbeMode::Engine&&api&&api->capsule&&r.probeRadius>0.f;
    if(!capsule)bump(stats.probeFallback);
    const double capUs=[&]{const auto& u=arc::ledger.users[unsigned(arc::ProbeUser::Lasso)];const auto n=arc::u64(u.casts);return n?double(arc::u64(u.nsSum))/double(n)/1e3:0.0;}();
    cfg.maxChords=capUs>150.0?4u:8u;cfg.pointsPerChord=std::max(1u,(np>1?np-1:1)/cfg.maxChords+((np>1?np-1:1)%cfg.maxChords?1u:0u));cfg.losFromEye=false;
    cfg.query.include=capsule?0x100028u:0x500208u;cfg.query.select=arc::Select::AnySurface;cfg.query.radiusM=capsule?r.probeRadius:0.f;cfg.query.state=7;
    cfg.query.type=0xffffffffu;cfg.query.maxHits=8;cfg.user=arc::ProbeUser::Lasso;
    arc::ledger.demand(arc::ProbeUser::Lasso,cfg.maxChords+1); // a changed arc (commit, target, assist) is fully cast next window, not Pending for long
    const auto t=tracer_.runPath(c,pts.data(),np,cfg);
    bump(stats.arcCasts,t.casts);if(t.hit)bump(stats.landings);
    // the marker: gold ring when the target capsule (inflated by r) is reached before any surface hit; a disc on the hit (= where the engine
    // fails, HIT_FAIL); a red cross at the range cut; else none
    bool ring=false;
    if(haveTarget){
        const Sizes z=sizesOf(target_.c.race);const auto a=closest(path,target_.point);
        const bool reached=a.d<=double(config.assistRadiusM*upm)&&a.y>=target_.c.feet[1]-r.probeRadius&&a.y<=target_.c.feet[1]+z.head*upm+r.probeRadius;
        float atLand=1e30f;if(t.hit){const V3 hp=t.landing.point;atLand=std::hypot(hp[0]-target_.point[0],hp[2]-target_.point[2]);}
        const float alongTarget=am::distance(path.p[std::min(a.i,path.n-1)],p0),alongHit=t.hit?am::distance(t.landing.point,p0):1e30f;
        ring=reached&&(!t.hit||alongTarget<=alongHit+.25f*upm||atLand<=r.probeRadius+.5f*upm);
    }
    arc::Look look=arc::lassoLook(ring,in.committed!=0);
    if(!ring&&!t.hit&&path.rangeCut){look.marker.shape=hs::ArcMarkerShape::Cross;look.marker.rgba=hs::packRgba(1.f,.25f,.15f,.95f);}
    hs::ArcFrame f{};arc::toFrame(c,tracer_,t,look,!t.pending,f);
    if(f.count>=2&&!t.pending){
        if(ring){const Sizes z=sizesOf(target_.c.race);(void)z;f.marker=look.marker;f.marker.shape=hs::ArcMarkerShape::Ring;
            f.marker.centre[0]=target_.point[0];f.marker.centre[1]=target_.c.feet[1]+.9f*upm;f.marker.centre[2]=target_.point[2];
            f.marker.normal[0]=0;f.marker.normal[1]=1;f.marker.normal[2]=0;bump(stats.rings);}
        else if(!t.hit&&path.rangeCut){const auto& e=f.points[f.count-1].position;f.marker=look.marker;f.marker.centre[0]=e[0];f.marker.centre[1]=e[1];
            f.marker.centre[2]=e[2];f.marker.normal[0]=f.marker.normal[1]=f.marker.normal[2]=0;}
    }
    if(!f.key){hideArc();return;}
    hs::publishArc(hs::ArcChannel::Lasso,f);shown_=true;bump(stats.arcPublished);lastFrame_=f;
    // the shown box (what H1 launches)
    ShownBox sb{};sb.ns=now;sb.key=f.key;sb.seq=++shownSeq_;sb.referenceSpace=map.referenceSpace;sb.mode=in.mode;sb.valid=f.count>=2&&am::finite(dir);
    sb.hasTarget=haveTarget;sb.committed=in.committed;sb.k=k;sb.upm=upm;sb.dtS=float(dt);
    for(unsigned i=0;i<3;++i){sb.p0[i]=p0[i];sb.p0Rel[i]=p0[i]-r.rootPos[i];sb.dir[i]=dir[i];sb.root[i]=r.rootPos[i];}
    if(haveTarget){sb.targetGoh=target_.c.goh;sb.targetActor=target_.c.guid;for(unsigned i=0;i<3;++i)sb.targetPoint[i]=target_.point[i];
        sb.targetUp=target_.point[1]-target_.c.feet[1];lastRace.store(target_.c.race,std::memory_order_relaxed);}
    {const unsigned n=path.n;const unsigned stride=std::max(1u,(n>1?n-1:1)/7);sb.shownStride=stride;
     for(unsigned kk=0;kk<8;++kk){const auto& q=path.p[std::min(kk*stride,n-1)];for(unsigned i=0;i<3;++i)sb.shownArc[kk*3+i]=q[i]-r.rootPos[i];}sb.shownCount=8;}
    shownBox.store(sb);
}

// Direct launch (`.lasso.launch direct`): write +0xe0 = p0 + dir * 50, +0x40c = 1 and call vt[0x30] = invLassoWeapon::OnUseItem
// (the live vtable word was verified at install) - what OnUseItemRemote does for remote owners minus its bit1 gate. H1 then applies the plan.
inline void Module::direct(const PlayerLink& pl,std::uint64_t now) noexcept {
    const auto req=directRequestSeq.load(std::memory_order_acquire);if(!req||req==directDoneSeq.load(std::memory_order_relaxed))return;
    directDoneSeq.store(req,std::memory_order_relaxed);
    Plan p{};if(!planBox.load(p)||p.seq!=req||!env.directUseItem||!pl.slotItem||pl.slotItem!=pl.handItem||config.dry){bump(stats.directRefused);return;}
    const auto item=pl.slotItem;if(rd::field<std::uintptr_t>(item,0)!=env.lassoItemTable){bump(stats.directRefused);return;}
    V3 root{};if(!rd::actorRoot(pl.actor,root)){bump(stats.directRefused);return;}
    const V3 aimAt{root[0]+p.p0Rel[0]+p.dir[0]*50.f,root[1]+p.p0Rel[1]+p.dir[1]*50.f,root[2]+p.p0Rel[2]+p.dir[2]*50.f};
    rd::putV3(item,off::itemAim,aimAt);rd::put<std::uint8_t>(item,off::itemAimValid,1);
    try{env.directUseItem(reinterpret_cast<void*>(item));bump(stats.directLaunches);}catch(...){bump(stats.exceptions);}
    (void)now;
}

inline void Module::note(const PlayerLink& pl,bool plValid,bool attached,std::uint64_t now) noexcept {
    const auto st=brain.state();const bool gradeL=(grades.load(std::memory_order_relaxed)&GradeL)!=0;
    const auto nn=notOwnedNs.load(std::memory_order_relaxed);
    const char* n="LEFT HIP";
    if(minigameFlag.load(std::memory_order_relaxed))n="MINIGAME";
    else if(st==BrainState::Caught||(st==BrainState::Recover&&attached))n="CAUGHT";
    else if(st==BrainState::Flying||st==BrainState::LaunchPending)n="FLYING";
    else if(st==BrainState::Ready)n="READY";
    else if(!plValid||!pl.owned||(nn&&now>=nn&&now-nn<msNs(3000)))n="NOT OWNED";
    else if(!gradeL&&settings().lassoThrowMode()!=2)n="NATIVE ONLY";
    else if(settings().lassoThrowMode()!=2&&!arcUsable.load(std::memory_order_relaxed))n="NO ARC";
    else if(pl.inHand)n="IN HAND";
    noteNow.store(n,std::memory_order_relaxed);status::publish(Feature::Lasso,status::Ready,n);
}

// ============================== rope pull (main thread) ==============================
// After a catch the player pulls the ENGINE's rope in with the VR hands: no second rope is drawn. Geometry = the engine's drawn rope verts
// (sagLassoDrawable copy of the verlet strand, world units, checked: finite, one end within 2.5 m of the player root, the other within 4 m of the
// victim's chest) else, invisibly, the straight line holding hand -> victim chest. A grip closed (edge) within .rope.radius of it grabs the rope;
// pulling that hand toward the body along the rope (measured relative to the player root from the grab point) accumulates; every .rope.step
// (faster than .rope.minspeed, or 2 steps slow, or a jerk >= 1.5 m/s over 8 cm) = one native yank: aniLasso::LassoYank (the stock yank: the victim is
// pulled toward the player), in a lasso minigame the prompted input as a pad pulse (the brain presses it). Opening that hand lets its rope go slack;
// the other hand re-grabs further along = hand over hand. Haptics: a tick on the grab, tension (stretch since the last yank + pull speed) every
// 50 ms while held, a strong pulse per yank. .rope.follow draw: the drawn rope's hand end is moved to the front-most grabbing hand (the drawable's
// copy only: never the simulated verlet state).
inline void Module::ropeReset(bool leftCatch) noexcept {
    for(auto& g:grab_)g=RopeGrab{};ropeGrabMask.store(0,std::memory_order_relaxed);
    if(leftCatch){engaged_=false;ropeEngagedNs.store(0,std::memory_order_relaxed);lastYankNs_=0;holdAnchorOk_=false;
        reelCon_=0;reelActive_=false;reelProbeNs_=0;reelLastNs_=0;}
}
inline void Module::ropePrints() noexcept {
    const auto check=[](std::uintptr_t offset,const unsigned char* bytes,std::size_t n)->bool{
        if(!env.read||!env.base||n>160)return false;unsigned char b[160]{};
        return rd::block(env.base+offset,b,n)&&std::memcmp(b,bytes,n)==0;};
    if(!yankPrint.load(std::memory_order_relaxed)){const bool ok=check(elf::lassoYankOffset,elf::lassoYankFingerprint.data(),elf::lassoYankFingerprint.size());
        if(ok)yankFn_=reinterpret_cast<YankFn>(env.base+elf::lassoYankOffset);yankPrint.store(ok?1:2,std::memory_order_relaxed);}
    if(!drawPrint.load(std::memory_order_relaxed))
        drawPrint.store(check(elf::lassoDrawCopyOffset,elf::lassoDrawCopyFingerprint.data(),elf::lassoDrawCopyFingerprint.size())?1:2,std::memory_order_relaxed);
    if(!reelPrint.load(std::memory_order_relaxed)){ // UpdateReelIn is 176 B: read in two halves
        const auto& f=elf::reelInFingerprint;
        const bool ok=check(elf::reelInOffset,f.data(),88)&&check(elf::reelInOffset+88,f.data()+88,f.size()-88)&&
            check(elf::reelInMinOffset,elf::reelInMinFingerprint.data(),elf::reelInMinFingerprint.size())&&
            check(elf::reelInStoreOffset,elf::reelInStoreFingerprint.data(),elf::reelInStoreFingerprint.size());
        reelPrint.store(ok?1:2,std::memory_order_relaxed);}
}
inline bool Module::ropeGeometry(const Readout& r,const V3& holdHand,bool holdOk,float upm,std::uintptr_t& drawable,unsigned& count,unsigned& handEnd) noexcept {
    drawable=0;count=0;handEnd=0;polyN_=0;
    V3 victim{};bool victimOk=false;
    if(r.victimGuid!=0xffffffffu)if(const auto va=rd::actorOf(r.victimGuid)){V3 v{};if(rd::actorRoot(va,v)){victim=am::add(v,V3{0.f,1.0f*upm,0.f});victimOk=true;}}
    const auto finishAlong=[&]{along_[0]=0.f;for(unsigned i=1;i<polyN_;++i)along_[i]=along_[i-1]+am::distance(poly_[i-1],poly_[i]);};
    if(drawPrint.load(std::memory_order_relaxed)==1&&r.drawn){
        std::uintptr_t d{};std::uint32_t n{};
        if(rd::get(r.drawn,off::drawnDrawable,d)&&d&&rd::get(d,off::drawableCount,n)&&n>=2&&n<=off::drawableMaxVerts&&
           rd::block(d+off::drawableVerts,ropeRaw_.data(),std::size_t(n)*16)){
            bool ok=true;
            for(unsigned i=0;i<n&&ok;++i)for(unsigned k=0;k<3;++k){const float x=ropeRaw_[i*4+k];ok=ok&&std::isfinite(x)&&std::fabs(x)<1e6f;}
            if(ok){
                const V3 a{ropeRaw_[0],ropeRaw_[1],ropeRaw_[2]},b{ropeRaw_[(n-1)*4],ropeRaw_[(n-1)*4+1],ropeRaw_[(n-1)*4+2]};
                const float da=am::distance(a,r.rootPos),db=am::distance(b,r.rootPos);
                handEnd=da<=db?0u:n-1;const V3 ve=handEnd?a:b;const float dh=std::min(da,db),dv=victimOk?am::distance(ve,victim):0.f;
                ropeHandEndCm.store(dh/upm*100.f,std::memory_order_relaxed);ropeVictimEndCm.store(dv/upm*100.f,std::memory_order_relaxed);
                if(dh<=2.5f*upm&&dv<=4.f*upm){
                    const unsigned m=std::min<unsigned>(n,polyMax);
                    for(unsigned k=0;k<m;++k){unsigned i=unsigned((std::uint64_t(k)*(n-1)+(m-1)/2)/(m-1));if(handEnd)i=n-1-i;
                        poly_[k]={ropeRaw_[i*4],ropeRaw_[i*4+1],ropeRaw_[i*4+2]};}
                    polyN_=m;finishAlong();drawable=d;count=n;bump(stats.ropeGeomEngine);
                    ropeSource.store("ENGINE",std::memory_order_relaxed);ropeVerts.store(n,std::memory_order_relaxed);return true;
                }
            }
            bump(stats.ropeGeomBad);
        }
    }
    if(holdOk&&victimOk){poly_[0]=holdHand;poly_[1]=victim;polyN_=2;finishAlong();bump(stats.ropeGeomLine);
        ropeSource.store("LINE",std::memory_order_relaxed);ropeVerts.store(0,std::memory_order_relaxed);return true;}
    bump(stats.ropeGeomNone);ropeSource.store("NONE",std::memory_order_relaxed);return false;
}
inline void Module::ropeFollow(std::uintptr_t drawable,unsigned count,unsigned handEnd,const V3& to,float upm) noexcept {
    const unsigned i0=handEnd;const V3 v0{ropeRaw_[i0*4],ropeRaw_[i0*4+1],ropeRaw_[i0*4+2]};
    const V3 delta=am::sub(to,v0);if(!am::finite(delta)||am::length(delta)>1.5f*upm)return; // implausible: leave the engine's rope alone
    const float lb=.8f*upm;float acc=0.f;V3 prev=v0;
    for(unsigned k=0;k<count;++k){const unsigned i=handEnd?count-1-k:k;const V3 v{ropeRaw_[i*4],ropeRaw_[i*4+1],ropeRaw_[i*4+2]};
        if(k)acc+=am::distance(v,prev);prev=v;if(acc>=lb)break;
        const float t=1.f-acc/lb,w=t*t*(3.f-2.f*t);const V3 nv=am::add(v,am::mul(delta,w));
        std::memcpy(reinterpret_cast<void*>(drawable+off::drawableVerts+std::uintptr_t(i)*16),nv.data(),sizeof(float)*3);}
    bump(stats.followWrites);
}
inline void Module::ropeYank(const Readout& r,bool minigame,std::uint64_t now) noexcept {
    lastYankNs_=now;
    if(minigame){ // The prompted input as a pad pulse
        int a=-1;switch(r.promptCode){case 0:a=YankLt;break;case 1:a=YankRt;break;case 2:a=YankL1;break;case 3:a=YankR1;break;case 4:a=YankY;break;
            case 5:a=YankB;break;case 6:a=YankA;break;case 7:a=YankX;break;case 24:a=YankAlt;break;default:break;}
        if(a<0){bump(stats.yankUnsupportedCode);lastRefusal.store("YANK_CODE",std::memory_order_relaxed);return;}
        if(config.dry){bump(stats.yankRefused);return;}
        yankPadAction.store(a,std::memory_order_relaxed);yankPadSeq.fetch_add(1,std::memory_order_release);bump(stats.yankPad);bump(stats.yankPulses);return;
    }
    if(r.hogtie||r.victimState==12||r.victimState==13||r.userState==15){bump(stats.yankRefused);lastRefusal.store("YANK_HOGTIE",std::memory_order_relaxed);return;}
    if(config.dry){bump(stats.yankRefused);lastRefusal.store("YANK_DRY",std::memory_order_relaxed);return;}
    // The pull itself = shorten the catch constraint (the victim is dragged in by the physics); the native yank below only reaches animals
    if(config.reel)reelPull(r,now);
    if(config.ropeYank==2)return;   // off: no native yank / pad pulse
    if(config.ropeYank==1){yankPadAction.store(config.yankBtn>=0?config.yankBtn:int(YankA),std::memory_order_relaxed);
        yankPadSeq.fetch_add(1,std::memory_order_release);bump(stats.yankPad);bump(stats.yankPulses);return;}
    const bool state=r.userState==5||r.lassoState==8||r.lassoState==9;
    if(!yankFn_||!r.aniPtr||!r.mgInst||!state){bump(stats.yankRefused);
        lastRefusal.store(!yankFn_?"YANK_NO_PRINT":!state?"YANK_STATE":"YANK_NO_ANI",std::memory_order_relaxed);return;}
    try{yankFn_(reinterpret_cast<void*>(r.aniPtr));bump(stats.yankEngine);bump(stats.yankPulses);}catch(...){bump(stats.exceptions);}
}
// Rope reel: what UpdateReelIn does (elf::reelInOffset), per pull: the catch constraint's max length (constraint+0x90, mirrored to
// item+0x148, the rebuild length item+0x144 lowered with it) gets a target .rope.reel.step shorter than min(length, victim distance + 0.5 m),
// floored at max(data+0x48 / +0x4c mounted, .rope.reel.min); the tick ramps the length down to it at .rope.reel.rate m/s (the stock reel's way:
// a gradual constraint change, never a jump). Consecutive pulls stack (the next target starts from the previous one). Only the player's own
// lasso item (vtable + owner checked this tick), only its live constraint (re-read this tick, finite, 0.05..100 m), only a victim not in a
// tug-of-war / hogtie / free state, only when UpdateReelIn's code still matches (the offsets the mod writes).
inline bool Module::reelUsable(const Readout& r) const noexcept {
    if(reelPrint.load(std::memory_order_relaxed)!=1||!r.item||!r.ownerOk||!r.itemPtr||!r.constraint||!r.data)return false;
    if(r.lassoState!=8&&r.lassoState!=9&&r.lassoState!=10)return false;   // ATTACHED_TARGET / _ON_GROUND / _REELIN_TARGET
    if(r.victimGuid==0xffffffffu||r.hogtie||r.userState==15)return false;
    const auto v=r.victimState;return !(v>=2&&v<=6)&&v!=11&&v!=12&&v!=13;   // tug-of-war, FREE, HOGTIE, POST_HOGTIED
}
inline bool Module::reelVictimDist(const Readout& r,float& dist) const noexcept {
    if(!r.root||r.victimGuid==0xffffffffu)return false;
    const auto va=rd::actorOf(r.victimGuid);V3 v{};if(!va||!rd::actorRoot(va,v))return false;
    dist=am::distance(v,r.rootPos);return std::isfinite(dist);
}
inline void Module::reelPull(const Readout& r,std::uint64_t now) noexcept {
    auto& c=arc::frameContext(now);const float upm=std::max(c.upm(),.01f);
    float len{};
    if(!reelUsable(r)||!rd::get(r.constraint,off::constraintMaxLen,len)||!std::isfinite(len)||len<.05f*upm||len>100.f*upm){
        bump(stats.reelRefused);lastRefusal.store(reelPrint.load(std::memory_order_relaxed)!=1?"REEL_NO_PRINT":"REEL_STATE",std::memory_order_relaxed);return;}
    float dataMin=r.mounted?r.reelMinMount:r.reelMinFoot;
    if(!std::isfinite(dataMin)||dataMin<.3f*upm||dataMin>10.f*upm)dataMin=1.5f*upm;
    const float floorLen=std::max(dataMin,config.reelMinM*upm);
    float dist{};const bool distOk=reelVictimDist(r,dist);
    float base=len;if(distOk)base=std::min(base,dist+.5f*upm);
    if(reelActive_&&reelCon_==r.constraint)base=std::min(base,reelTarget_);
    const float target=std::max(floorLen,base-config.reelStepM*upm);
    reelMinCm.store(floorLen/upm*100.f,std::memory_order_relaxed);
    if(target>=len-.005f*upm){bump(stats.reelAtMin);lastRefusal.store("REEL_AT_MIN",std::memory_order_relaxed);return;}
    reelCon_=r.constraint;reelTarget_=target;reelActive_=true;reelLastNs_=now;bump(stats.reelPulls);
    reelTargetCm.store(target/upm*100.f,std::memory_order_relaxed);
    if(distOk){reelDistAtPull_=dist;reelProbeNs_=now+msNs(400);}
}
inline void Module::reelTick(const Readout& r,std::uint64_t now) noexcept {
    auto& c=arc::frameContext(now);const float upm=std::max(c.upm(),.01f);
    float dist{};const bool distOk=reelVictimDist(r,dist);
    if(distOk)reelVictimCm.store(dist/upm*100.f,std::memory_order_relaxed);
    if(reelProbeNs_&&now>=reelProbeNs_){reelProbeNs_=0;
        if(distOk){const float gain=(reelDistAtPull_-dist)/upm*100.f;lastReelGainCm.store(gain,std::memory_order_relaxed);
            if(gain>0.f)bump(stats.reelGainCm,std::uint64_t(gain));}}
    if(!reelActive_)return;
    if(!reelUsable(r)){reelActive_=false;bump(stats.reelStopped);return;}
    if(r.constraint!=reelCon_){reelCon_=r.constraint;bump(stats.reelRebuilt);}   // rebuilt (ragdoll swap): keep shortening the new one
    float len{};
    if(!rd::get(r.constraint,off::constraintMaxLen,len)||!std::isfinite(len)||len<.05f*upm||len>100.f*upm){reelActive_=false;bump(stats.reelStopped);return;}
    reelLenCm.store(len/upm*100.f,std::memory_order_relaxed);
    if(len<=reelTarget_+1e-4f*upm){reelActive_=false;return;}   // reached (or the engine shortened it further: UpdateShorten / a stock reel)
    double dt=r.time?double(r.dt):(now>reelLastNs_?double(now-reelLastNs_)*1e-9:0.0);reelLastNs_=now;
    dt=std::clamp(dt,0.0,.05);
    const float next=std::max(reelTarget_,len-float(dt)*config.reelRateMps*upm);
    if(!(next<len))return;
    if(rd::field<std::uintptr_t>(r.itemPtr,0)!=env.lassoItemTable){reelActive_=false;bump(stats.reelStopped);return;}
    rd::put(r.constraint,off::constraintMaxLen,next);rd::put(r.itemPtr,off::itemConLen,next);
    if(std::isfinite(r.drawnLen)&&r.drawnLen>next)rd::put(r.itemPtr,off::itemDrawnLen,next);
    bump(stats.reelWrites);reelLenCm.store(next/upm*100.f,std::memory_order_relaxed);
}
inline void Module::rope(const Readout& r,bool attached,bool minigame,std::uint64_t now) noexcept {
    const auto st=brain.state();
    // outside a catch nothing is fetched; the grips count as closed (a hand already closed when the catch starts needs a real open -> close)
    if(!config.rope||st!=BrainState::Caught){ropeReset(true);wasClosed_={true,true};return;}
    auto& c=arc::frameContext(now);
    const auto* raw=c.inputValid()?c.raw():nullptr;
    std::array<bool,2> closedNow=wasClosed_;
    if(raw)for(unsigned h=0;h<2;++h){const float g=raw->grips[h];closedNow[h]=g>=gripClose?true:(g<=gripOpen?false:wasClosed_[h]);}
    const bool on=attached&&r.root&&r.item&&r.ownerOk&&raw&&c.world();
    if(!on){ropeReset(false);wasClosed_=closedNow;return;}
    ropePrints();bump(stats.ropeFrames);
    if(!minigame)reelTick(r,now);else reelActive_=false;
    const float upm=std::max(c.upm(),.01f);
    std::array<V3,2> hp{};std::array<bool,2> hv{};for(unsigned h=0;h<2;++h)hv[h]=c.gripPoint(h,hp[h]);
    const int hold=holdHandFlag.load(std::memory_order_relaxed);const bool holdOk=hold==0||hold==1?hv[unsigned(hold)]:false;
    // the line fallback starts where the holding hand last held the coil (closed, not on the rope): a hand reaching out along it can grab it
    if(holdOk&&(closedNow[unsigned(hold)]||!holdAnchorOk_)&&!grab_[unsigned(hold)].on){holdAnchor_=hp[unsigned(hold)];holdAnchorOk_=true;}
    std::uintptr_t drawable{};unsigned count{},handEnd{};
    const bool geom=ropeGeometry(r,holdAnchor_,holdAnchorOk_,upm,drawable,count,handEnd);
    const V3 victimEnd=geom?poly_[polyN_-1]:V3{};
    unsigned mask=0;
    for(unsigned h=0;h<2;++h){
        auto& g=grab_[h];
        if(g.on&&(!closedNow[h]||!hv[h]))g.on=false; // let go: this hand's rope goes slack
        if(!g.on&&closedNow[h]&&!wasClosed_[h]&&hv[h]&&geom){ // a squeeze: on the rope?
            // the holding hand never "grabs" its own coil: for it the first 15 cm from the rope's hand end do not count
            const float skip=int(h)==hold?.15f*upm:0.f;
            float best=1e30f;V3 bq{};
            for(unsigned i=0;i+1<polyN_;++i){const V3 a=poly_[i],ab=am::sub(poly_[i+1],a);const float l2=am::dot(ab,ab),l=std::sqrt(l2);
                if(along_[i]+l<skip)continue;
                float t=l2>1e-12f?std::clamp(am::dot(am::sub(hp[h],a),ab)/l2,0.f,1.f):0.f;
                if(skip>0.f&&along_[i]+l*t<skip)t=l>1e-6f?std::min(1.f,(skip-along_[i])/l):1.f;
                const V3 q=am::add(a,am::mul(ab,t));const float d=am::distance(hp[h],q);if(d<best){best=d;bq=q;}}
            lastGrabCm.store(best/upm*100.f,std::memory_order_relaxed);
            if(best<=config.ropeRadiusM*upm){
                V3 dir=am::normalized(am::sub(victimEnd,bq));if(am::length(dir)<.5f)dir=am::normalized(am::sub(victimEnd,hp[h]));
                if(am::length(dir)>.5f){g=RopeGrab{};g.on=true;g.anchorRel=am::sub(hp[h],r.rootPos);g.dir=dir;g.lastNs=now;g.nextHaptic=now+msNs(50);
                    bump(stats.ropeGrabs);ropeGrabNs[h].store(now,std::memory_order_relaxed);haptics::pulse(h,.35f,25'000,now,haptics::SourceLasso);}
            }else if(best<=3.f*config.ropeRadiusM*upm)bump(stats.grabMisses);
        }
        if(!g.on)continue;
        mask|=1u<<h;
        const V3 rel=am::sub(hp[h],r.rootPos);
        const float pull=am::dot(am::sub(g.anchorRel,rel),g.dir)/upm;   // m toward the body along the rope since the grab
        const float delta=pull-g.lastPull;g.lastPull=pull;
        const double dt=now>g.lastNs?double(now-g.lastNs)*1e-9:0.0;g.lastNs=now;
        if(std::isfinite(delta)){
            if(dt>0.0&&dt<.25){const float inst=float(double(delta)/dt);if(std::isfinite(inst))g.speed+=(inst-g.speed)*.5f;}
            if(delta>0.f){pullCmAcc_+=delta*100.f;const auto whole=std::uint64_t(pullCmAcc_);if(whole){bump(stats.pullCm,whole);pullCmAcc_-=float(whole);}}
            g.sinceYank=std::max(0.f,g.sinceYank+delta);
        }
        lastPullCm.store(pull*100.f,std::memory_order_relaxed);lastPullSpeed.store(g.speed,std::memory_order_relaxed);
        const bool step=g.sinceYank>=config.pullStepM&&(g.speed>=config.pullMinSpeed||g.sinceYank>=2.f*config.pullStepM);
        const bool jerk=g.speed>=1.5f&&g.sinceYank>=.08f;
        if((step||jerk)&&(!lastYankNs_||now<lastYankNs_||now-lastYankNs_>=msNs(config.yankGapMs))){
            g.sinceYank=0.f;bump(stats.pulls);ropeYank(r,minigame,now);
            for(unsigned k=0;k<2;++k)if(k==h||grab_[k].on)haptics::pulse(k,.9f,60'000,now,haptics::SourceLasso);
        }else if(now>=g.nextHaptic){ // tension = the stretch since the last yank + the pull speed
            const float tension=std::clamp(.15f+.5f*std::clamp(g.sinceYank/config.pullStepM,0.f,1.f)+.35f*std::clamp(g.speed/1.5f,0.f,1.f),0.f,1.f);
            haptics::pulse(h,.05f+.5f*tension,40'000,now,haptics::SourceLasso);g.nextHaptic=now+msNs(50);}
    }
    const unsigned prev=ropeGrabMask.exchange(mask,std::memory_order_relaxed);
    if(mask){engaged_=true;ropeEngagedNs.store(now,std::memory_order_relaxed);}
    else if(prev&&engaged_){bump(stats.slackStarts);if(hold==0||hold==1)haptics::pulse(unsigned(hold),.15f,20'000,now,haptics::SourceLasso);}
    // the engine's drawn rope: its hand end follows the front-most grabbing hand (the one nearest the victim)
    if(config.ropeFollow&&mask&&drawable&&count){int f=-1;float fd=1e30f;
        for(unsigned h=0;h<2;++h)if(mask&(1u<<h)){const float d=am::distance(hp[h],victimEnd);if(d<fd){fd=d;f=int(h);}}
        if(f>=0)ropeFollow(drawable,count,handEnd,hp[unsigned(f)],upm);}
    wasClosed_=closedNow;
}

inline void Module::tick(const mainthread::Frame& fr) noexcept {
    try{
        const auto now=fr.now;const auto t0=nowNs();bump(stats.ticks);
        if(!mainThreadToken.load(std::memory_order_relaxed))mainThreadToken.store(threadToken(),std::memory_order_relaxed);
        if(killed(Feature::Lasso)){status::publish(Feature::Lasso,status::Property);return;}
        if(!(grades.load(std::memory_order_acquire)&GradeR)){ // reader prints failed: grade H only, the row says UNAVAILABLE
            status::publish(Feature::Lasso,status::Unavailable);ropeBusyFlag.store(false);minigameFlag.store(false);
            tploco::externalBusy.fetch_and(~1u,std::memory_order_relaxed);return;}
        PlayerLink pl{};const bool plValid=playerBox.load(pl)&&pl.valid&&freshNs(pl.ns,now,msNs(150));
        const auto st=brain.state();
        const bool relevant=plValid&&pl.owned&&(pl.inHand||st!=BrainState::Off||aimRequestFlag.load(std::memory_order_relaxed));
        Readout r{};
        if(relevant){readEngine(pl,r);last=r;}
        else bump(stats.skipped);
        if(r.time)dtEma=dtEma>0.f?dtEma+(r.dt-dtEma)*.1f:r.dt;
        const bool attached=r.ani&&(userCaught(r.userState)||(r.item&&lassoCaught(r.lassoState)));
        const bool minigame=relevant&&r.minigame&&r.focus>=0&&(pl.inHand||attached);
        // EngineFacts for the brain (next pad update)
        EngineFacts f{};f.ns=now;f.valid=relevant&&r.item;f.item=r.item&&r.ownerOk;f.owned=plValid&&pl.owned;f.inGame=plValid&&pl.gameState==3&&pl.playerFlags;
        f.alive=plValid&&pl.alive;f.lassoState=r.lassoState;f.flight=r.flight;f.userState=r.userState;f.victimState=r.victimState;f.hogtie=r.hogtie;
        f.promptCode=r.promptCode;f.minigame=minigame;f.attached=attached;f.mounted=pl.riding;f.launchVelocity=r.launchVelocity;
        f.maxThrowDistance=r.maxThrow;f.probeRadius=r.probeRadius;
        engineBox.store(f);
        lastLv.store(r.launchVelocity,std::memory_order_relaxed);lastRange.store(r.maxThrow,std::memory_order_relaxed);
        lastRadius.store(r.probeRadius,std::memory_order_relaxed);lastDtMs.store(dtEma*1000.f,std::memory_order_relaxed);
        minigameFlag.store(minigame,std::memory_order_relaxed);promptCodeFlag.store(int(r.promptCode),std::memory_order_relaxed);
        lastPromptCode.store(r.promptCode,std::memory_order_relaxed);
        if(minigame){if(int(r.promptCode)!=prevCode_&&r.promptCode<=32)bump(stats.promptCodesSeen[r.promptCode]);prevCode_=int(r.promptCode);}else prevCode_=-1;
        if(minigame&&!prevMinigame_)noteTrace(unsigned(st),f,0,0,"MINIGAME",now);prevMinigame_=minigame;
        // Rope pull: grab / pull the engine's rope after a catch
        if(relevant)rope(r,attached,minigame,now);else ropeReset(true);
        // engine transitions -> the trace; catch counters
        if(relevant&&(r.lassoState!=prevLasso_||r.userState!=prevUser_||r.victimState!=prevVictim_)){
            if(userTugWar(r.userState)&&!userTugWar(prevUser_))bump(stats.tugWar);
            if((r.victimState==12||r.victimState==13)&&prevVictim_!=12&&prevVictim_!=13)bump(stats.hogtie);
            if(r.lassoState==lsHitSucceed&&prevLasso_!=lsHitSucceed){bump(stats.catches);if(directDoneSeq.load()&&directDoneSeq.load()==planConsumedSeq.load())bump(stats.catchesAfterDirect);}
            prevLasso_=r.lassoState;prevUser_=r.userState;prevVictim_=r.victimState;noteTrace(unsigned(st),f,0,0,"ENGINE",now);}
        // Rope busy: teleport gate 4 sees this frame's state (ModuleTeleport ticks after this module)
        const bool busy=st==BrainState::Flying||st==BrainState::Caught||(st==BrainState::Recover&&attached)||minigame||st==BrainState::LaunchPending;
        ropeBusyFlag.store(busy,std::memory_order_relaxed);
        if(busy)tploco::externalBusy.fetch_or(1u,std::memory_order_relaxed);else tploco::externalBusy.fetch_and(~1u,std::memory_order_relaxed);
        // involuntary motion / warps while busy (root speed > 0.5 m/s while CAUGHT with the move stick neutral; a > 2 m jump in one frame)
        if(relevant&&r.root){
            if(prevRootValid_&&lastTickNs_&&now>lastTickNs_){const float d=am::distance(r.rootPos,prevRoot_);const float upm=std::max(arc::frameContext(now).upm(),.01f);
                const float speed=d/upm/float(double(now-lastTickNs_)*1e-9);
                if(busy&&d>2.f*upm)bump(stats.warpWhileBusy);
                if(st==BrainState::Caught&&speed>.5f){const auto* raw=arc::frameContext(now).raw();
                    if(raw&&std::hypot(raw->sticks[0],raw->sticks[1])<.2f)bump(stats.involuntaryMotionFrames);}}
            prevRoot_=r.rootPos;prevRootValid_=true;}
        else prevRootValid_=false;
        lastTickNs_=now;
        // tick-side latch drops: not in game / dead; the brain not called any more (cutscene / presentation) and the engine let go for 300 ms
        if(aimRequestFlag.load(std::memory_order_relaxed)){
            if(plValid&&!pl.alive){aimRequestFlag.store(false);bump(stats.dropped[DropDead]);}
            else{const auto b=brainNs.load(std::memory_order_relaxed);const bool stale=!b||now<b?false:now-b>msNs(250);
                if(stale&&!attached&&!(r.item&&(r.flight||r.lassoState==lsInFlight))){if(!detachedSince_)detachedSince_=now;
                    if(now-detachedSince_>=msNs(300)){aimRequestFlag.store(false);bump(stats.dropped[DropCutscene]);detachedSince_=0;}}
                else detachedSince_=0;}
        }else detachedSince_=0;
        if(forceAimMirror.load(std::memory_order_relaxed)&&(pl.inHand||attached)&&!attached&&st!=BrainState::Ready&&st!=BrainState::LaunchPending&&st!=BrainState::Flying)
            bump(stats.stuckAimFrames);
        // H4 direct launch (option)
        if(config.launch==LaunchMode::Direct&&plValid)direct(pl,now);
        // the LASSO arc: arm FIRST (the arm edge restarts the BUILDING age), then the status gate
        const bool armWanted=plValid&&pl.inHand&&config.arc!=ArcShow::Off&&!killed(Feature::Arc)&&!killed(Feature::Hands);
        if(armWanted&&!armed_){hs::armArc(hs::ArcChannel::Lasso,true);armed_=true;bump(stats.arcArms);}
        else if(!armWanted&&armed_){hs::armArc(hs::ArcChannel::Lasso,false);armed_=false;}
        bool usable=!killed(Feature::Arc)&&!killed(Feature::Hands);
        if(usable&&armed_){const auto as=hs::arcStatus();
            if(as==hs::ArcStatus::Failed||as==hs::ArcStatus::Killed||(as==hs::ArcStatus::Building&&hs::arcStatusAgeMs()>3000))usable=false;}
        const auto* api=arc::engineApi.load(std::memory_order_acquire);if(!api||!*api)usable=false;
        arcUsable.store(usable,std::memory_order_relaxed);if(!usable&&pl.inHand)bump(stats.noArcTicks);
        Intent in{};const bool haveIntent=intentBox.load(in)&&freshNs(in.ns,now,msNs(150));
        if(armed_&&usable&&!(haveIntent&&in.arcWanted))arc::ledger.demand(arc::ProbeUser::Lasso,9); // The first aim frame fully granted
        if(relevant&&usable&&haveIntent&&in.arcWanted&&pl.inHand){
            // The actor scan only feeds the assist - none with ASSIST OFF or in NATIVE (candidates dropped, the next ASSIST
            // frame scans at once)
            if(settings().lassoAssist()&&in.mode!=2){
                if(!lastScanNs||now<lastScanNs||now-lastScanNs>=msNs(1000/std::max(1u,config.scanHz)))scan(pl,r,now);}
            else{if(candCount||lastScanNs){candCount=0;lastScanNs=0;}bump(stats.scansSkipped);}
            produce(in,pl,r,now);
        }else hideArc();
        note(pl,plValid,attached,now);
        stats.readsPerFrameSum.fetch_add(r.reads,std::memory_order_relaxed);
        const auto us=(nowNs()-t0)/1000u;stats.tickUsSum.fetch_add(us,std::memory_order_relaxed);maxOf(stats.tickUsMax,us);
    }catch(...){bump(stats.exceptions);}
}
inline void moduleTick(const mainthread::Frame& f) noexcept {module.tick(f);}

// ============================== hooks (core logic: host-testable; the item is the engine's own live object) ==============================
struct Token {
    std::uintptr_t item{};std::uint64_t planSeq{},ns{};std::uint8_t saved40c{};bool active{},target{},pre{},homedEver{};
    std::uint32_t targetGoh{0xffffffffu},targetActor{0xffffffffu};float targetUp{};V3 targetPoint{};std::uint32_t steps{};
    double sp[3]{},sv[3]{};V3 preP{},preV{};float upm{1.f};
};
inline Token token;
inline std::atomic<bool> directInProgress{false};
inline bool planValid(const Plan& p) noexcept {
    float n=0;for(float x:p.dir){if(!std::isfinite(x))return false;n+=x*x;}
    for(float x:p.p0Rel)if(!std::isfinite(x))return false;
    return std::fabs(std::sqrt(n)-1.f)<=1e-3f&&std::isfinite(p.k)&&p.k>0.f&&p.k<4.f&&std::isfinite(p.upm)&&p.upm>0.f;
}
// the live target point (feet + up) of an actor guid; false = gone / dead
inline bool targetPoint(std::uint32_t guid,float up,V3& out) noexcept {
    const auto a=rd::actorOf(guid);if(!a||!rd::actorAlive(a))return false;V3 feet{};if(!rd::actorRoot(a,feet))return false;
    out={feet[0],feet[1]+up,feet[2]};return true;
}
// Stale tokens: a token flight that ended OUTSIDE UpdateInFlight (OnReHolster / a socket put-away / a force-holster / death / the
// off-screen ReleaseAndReelInLasso without grade O) never reached flightPost's +0x344 drop, so the token stayed armed for the item and the next
// plan-less flight (NATIVE, the NO ARC fallback, a rejected-plan RT throw) would be steered as the mod's. Every OnUseItem starts a new flight (
// release, +0x344 = 1, state 3): H1 retires the old token of that item before anything else, H2 drops a token older than 6 s (a flight lasts
// <= 2.5 s, the brain caps FLYING at 4 s). +0x40c: only the mod's last write is undone (OnAim always writes 1 - a value that differs from it
// is the engine's and stays); never a write to another item (the token's item may be gone).
inline constexpr std::uint64_t tokenMaxAgeNs=6'000'000'000ull;
inline void retireToken(std::uintptr_t item) noexcept {
    if(!token.active)return;
    if(token.item==item&&item&&!config.dry){
        const std::uint8_t ours=token.target?1:0;const auto cur=rd::field<std::uint8_t>(item,off::itemAimValid);
        if(cur==ours&&cur!=token.saved40c){rd::put<std::uint8_t>(item,off::itemAimValid,token.saved40c);bump(stats.restored40c);}
    }
    token=Token{};bump(stats.tokensRetired);
}
// H1: after the original invWeapon::OnUseItem (invLassoWeapon::OnUseItem's own writes are done: p0 = FINGER_12_R, v0 = the engine formula).
inline void afterUseItem(std::uintptr_t item,std::uint64_t now) noexcept {
    noteHookThread();
    if(!(grades.load(std::memory_order_acquire)&GradeL)||!item)return;
    if(rd::field<std::uintptr_t>(item,0)!=env.lassoItemTable)return;
    if(token.active&&token.item==item)retireToken(item); // a new flight of this item: the old token is over (before the plan checks)
    Plan p{};const bool have=planBox.load(p)&&p.seq&&p.seq!=planConsumedSeq.load(std::memory_order_acquire);
    if(!have){bump(stats.noPlan);return;}
    if(now<p.ns||now-p.ns>msNs(config.planAgeMs)){bump(stats.stalePlan);return;}
    if(p.mode==2){bump(stats.noPlan);return;}
    PlayerLink pl{};if(!playerBox.load(pl)||!pl.valid||rd::field<std::uint32_t>(item,off::itemOwner)!=pl.guid){bump(stats.ownerMismatch);return;}
    if(!planValid(p)){bump(stats.rejectedPlan);return;}
    float lv{};const auto data=rd::field<std::uintptr_t>(item,off::itemData);
    if(!rd::get(data,off::dataLaunchVelocity,lv)||!(lv>0.f)||!std::isfinite(lv)){bump(stats.rejectedPlan);return;}
    V3 ownerVel{};if(!rd::moverVelocity(pl.actor,ownerVel))ownerVel={0,0,0};
    V3 root{};if(!rd::actorRoot(pl.actor,root)){bump(stats.rejectedPlan);return;}
    const V3 p0Engine=rd::fieldV3(item,off::itemLead);const V3 p0Vr{root[0]+p.p0Rel[0],root[1]+p.p0Rel[1],root[2]+p.p0Rel[2]};
    if(am::finite(p0Engine)){const auto um=std::uint64_t(double(am::distance(p0Engine,p0Vr))/double(p.upm)*1e6);
        stats.p0EngineVsVrUmSum.fetch_add(um,std::memory_order_relaxed);maxOf(stats.p0EngineVsVrUmMax,um);bump(stats.p0Samples);}
    {const auto us=(now-p.ns)/1000u;stats.rtToLaunchUsSum.fetch_add(us,std::memory_order_relaxed);maxOf(stats.rtToLaunchUsMax,us);}
    const V3 p0=config.p0==P0Mode::Vr?p0Vr:p0Engine;const V3 dir{p.dir[0],p.dir[1],p.dir[2]};
    const V3 v0=launchVelocity(dir,lv,p.k,ownerVel);
    if(config.dry){bump(stats.dryCalls);return;}
    const auto saved=rd::field<std::uint8_t>(item,off::itemAimValid);
    rd::putV3(item,off::itemLead,p0);rd::putV3(item,off::itemTail,p0);rd::putV3(item,off::itemVel,v0);bump(stats.v0Written);
    Token t{};t.item=item;t.planSeq=p.seq;t.ns=now;t.saved40c=saved;t.active=true;t.upm=p.upm;
    const bool wantTarget=p.targetGoh!=0xffffffffu&&p.targetActor!=0xffffffffu&&settings().lassoAssist();
    V3 tp{p.targetPoint[0],p.targetPoint[1],p.targetPoint[2]};
    if(wantTarget&&targetPoint(p.targetActor,p.targetUp,tp)){
        rd::putV3(item,off::itemAim,tp);rd::put<std::uint32_t>(item,off::itemTargetGoh,p.targetGoh);rd::put<std::uint32_t>(item,off::itemTargetComp,0xffffffffu);
        rd::put<std::uint8_t>(item,off::itemAimValid,1);rd::put<std::uint8_t>(item,off::itemForceMiss,0);
        t.target=true;t.homedEver=true;t.targetGoh=p.targetGoh;t.targetActor=p.targetActor;t.targetUp=p.targetUp;t.targetPoint=tp;
    }else{rd::put<std::uint8_t>(item,off::itemAimValid,0);if(wantTarget)bump(stats.targetLost);}
    for(int k=0;k<3;++k){t.sp[k]=p0[k];t.sv[k]=v0[k];}
    token=t;planConsumedSeq.store(p.seq,std::memory_order_release);bump(stats.consumed);bump(stats.tokenFlights);
    // shownVsThrown: the committed arc in the plan vs the arc recomputed from what was written (root-relative; the target shifted with the root)
    if(p.shownCount==8&&p.dtS>0.f){
        EngineFacts e{};(void)e;SimIn s{};s.p0=p0;s.v0=v0;s.dt=p.dtS;s.maxTime=2.5;s.root=root;s.range=0;
        const auto* ab=&module.last;s.g=ab->gravity?ab->gravityV:V3{0,-9.8f,0};
        if(t.target){s.homing=true;s.target=tp;}
        static Path path{};simulate(s,path);double worst=0;
        for(unsigned kk=0;kk<8;++kk){const auto i=std::min(kk*std::max(1u,p.shownStride),path.n-1);
            const V3 q=am::sub(path.p[i],root);const V3 w{p.shownArc[kk*3],p.shownArc[kk*3+1],p.shownArc[kk*3+2]};worst=std::max(worst,double(am::distance(q,w)));}
        const auto um=std::uint64_t(worst/double(p.upm)*1e6);stats.shownVsThrownUmSum.fetch_add(um,std::memory_order_relaxed);maxOf(stats.shownVsThrownUmMax,um);
        bump(stats.shownSamples);
    }
}
// H2 pre: only for the mod's token flight in state 3 with +0x344 == 1: re-assert the assist fields before every step (OnAim / the lock refresh may
// rewrite them); everything else passes untouched (flightsNative; `.lasso.unowned strip` -> +0x40c = 0 in SWING / TRIGGER).
inline void flightPre(std::uintptr_t item,std::uint64_t now) noexcept {
    noteHookThread();
    if(!(grades.load(std::memory_order_acquire)&GradeL)||!item)return;
    bump(stats.steps);
    if(token.active&&now>token.ns&&now-token.ns>tokenMaxAgeNs)retireToken(item); // A token never outlives a flight
    const auto flight=rd::field<std::uint8_t>(item,off::itemFlight);const auto state=rd::field<std::uint32_t>(item,off::itemState);
    if(token.active&&token.item==item&&flight==1&&state==3){
        token.pre=true;token.preP=rd::fieldV3(item,off::itemLead);token.preV=rd::fieldV3(item,off::itemVel);
        if(config.dry)return;
        if(token.target){V3 tp{};
            if(targetPoint(token.targetActor,token.targetUp,tp)){token.targetPoint=tp;rd::putV3(item,off::itemAim,tp);
                rd::put<std::uint32_t>(item,off::itemTargetGoh,token.targetGoh);rd::put<std::uint32_t>(item,off::itemTargetComp,0xffffffffu);
                rd::put<std::uint8_t>(item,off::itemAimValid,1);rd::put<std::uint8_t>(item,off::itemForceMiss,0);bump(stats.homingOn);}
            else{token.target=false;rd::put<std::uint8_t>(item,off::itemAimValid,0);bump(stats.targetLost);}
        }else{rd::put<std::uint8_t>(item,off::itemAimValid,0);bump(stats.homingOff);}
        return;
    }
    if(flight==1&&state==3){
        if(!(token.active&&token.item==item))bump(stats.flightsNative);
        if(config.unowned==Unowned::Strip&&settings().lassoThrowMode()!=2&&!config.dry){rd::put<std::uint8_t>(item,off::itemAimValid,0);bump(stats.stripped);}
    }
}
// H2 post: the actual flight (seqlock), the measured gravity, predErr (homing-off token flights: the engine lead vs the module's own integration of
// the written p0 / v0 with the engine's dt); the post-call where +0x344 dropped restores +0x40c (the next OnUseItem without a fresh OnAim
// would skip its velocity, cbz 0x268f470) and clears the token.
inline void flightPost(std::uintptr_t item,std::uint64_t now) noexcept {
    if(!token.active||token.item!=item||!token.pre)return;
    token.pre=false;++token.steps;
    const V3 p=rd::fieldV3(item,off::itemLead),v=rd::fieldV3(item,off::itemVel);
    const auto state=rd::field<std::uint32_t>(item,off::itemState);const auto flight=rd::field<std::uint8_t>(item,off::itemFlight);
    float dt=0.f;if(env.timeObject)rd::get(env.timeObject,off::timeDelta,dt);
    FlightBox fb{};fb.ns=now;for(int k=0;k<3;++k){fb.p[k]=p[k];fb.v[k]=v[k];}fb.state=state;fb.flight=flight;fb.steps=token.steps;fb.token=1;
    if(dt>1e-4f&&std::isfinite(dt))for(int k=0;k<3;++k)fb.gMeasured[k]=(v[k]-token.preV[k])/dt;
    flightBox.store(fb);
    if(dt>1e-4f&&std::isfinite(dt)&&!token.homedEver){ // predErr: flights that never homed (the shadow integrates the pure engine step)
        const V3 g=module.last.gravity?module.last.gravityV:V3{0,-9.8f,0};
        for(int k=0;k<3;++k){token.sp[k]+=token.sv[k]*dt;token.sv[k]+=double(g[k])*dt;}
        if(flight==1){const double e=std::sqrt((p[0]-token.sp[0])*(p[0]-token.sp[0])+(p[1]-token.sp[1])*(p[1]-token.sp[1])+(p[2]-token.sp[2])*(p[2]-token.sp[2]));
            const auto um=std::uint64_t(e/double(token.upm)*1e6);stats.predErrUmSum.fetch_add(um,std::memory_order_relaxed);maxOf(stats.predErrUmMax,um);bump(stats.predSamples);}
    }
    if(flight==0){
        if(!config.dry){rd::put<std::uint8_t>(item,off::itemAimValid,token.saved40c);bump(stats.restored40c);}
        if(lassoCaught(state))bump(stats.hits);
        else{V3 root{};PlayerLink pl{};
            if(playerBox.load(pl)&&pl.valid&&rd::actorRoot(pl.actor,root)&&module.last.maxThrow>0.f&&am::distance(p,root)>module.last.maxThrow)bump(stats.rangeFails);
            else bump(stats.fails);}
        token=Token{};
    }
}
// H3: at the nine off-screen sites of invLassoWeapon::OnUpdate a loop outside the game camera's view stays alive (the original first).
inline bool offscreenResult(bool original,std::uintptr_t returnAddress) noexcept {
    if(original)return true;
    if(!(grades.load(std::memory_order_acquire)&GradeO)||!config.offscreen)return false;
    for(const auto s:env.offscreenReturns)if(s&&returnAddress==s){bump(stats.offscreenOverrides);return true;}
    return false;
}
#ifdef __ANDROID__
__attribute__((noinline)) inline bool useItemHook(void* item) noexcept {
    const auto ra=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const auto original=env.useItemOriginal;const bool r=original?original(item):false;
    if(ra==env.useItemReturn){try{afterUseItem(reinterpret_cast<std::uintptr_t>(item),nowNs());}catch(...){bump(stats.exceptions);}}
    return r;
}
__attribute__((noinline)) inline void inFlightHook(void* item) noexcept {
    const auto ra=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const bool ours=ra==env.inFlightReturn;const auto now=nowNs();
    if(ours){try{flightPre(reinterpret_cast<std::uintptr_t>(item),now);}catch(...){bump(stats.exceptions);}}
    if(const auto original=env.inFlightOriginal)original(item);
    if(ours){try{flightPost(reinterpret_cast<std::uintptr_t>(item),nowNs());}catch(...){bump(stats.exceptions);}}
}
__attribute__((noinline)) inline bool visibleHook(const void* viewport,const void* points,int n) noexcept {
    const auto ra=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const auto original=env.visibleOriginal;const bool r=original?original(viewport,points,n):false;
    try{return offscreenResult(r,ra);}catch(...){return r;}
}
#endif

struct Checks {bool executor{},holsters{},readerPrints{},readerGlobals{},launchPrints{},launchGots{},offscreenPrint{},offscreenGot{},directWord{};};
struct Slots {std::uintptr_t useGot{},useOriginal{},useHook{},flightGot{},flightOriginal{},flightHook{},visibleGot{},visibleOriginal{},visibleHook{};};
using ReplaceFn=bool(*)(void* ctx,std::uintptr_t slot,std::uintptr_t expected,std::uintptr_t replacement) noexcept;
inline std::string installFailures;
// Order H3, H2, H1; L all-or-nothing (H1 failing after H2 puts H2 back); `.inflight 0` / `.offscreen 0` skip their hook. Returns the grades.
inline unsigned installGrades(const Checks& c,const Slots& s,ReplaceFn replace,void* ctx,std::string& failures) noexcept {
    failures.clear();
    if(killed(Feature::Lasso))return 0;
    unsigned g=GradeH;
    if(!c.executor){failures+="NO_EXECUTOR,";return g;}
    if(!c.holsters){failures+="NO_HOLSTERS,";return g;}
    if(!c.readerPrints)failures+="READER_PRINTS,";
    if(!c.readerGlobals)failures+="READER_GLOBALS,";
    if(!c.readerPrints||!c.readerGlobals)return g;
    g|=GradeR;
    if(config.offscreen){
        if(c.offscreenPrint&&c.offscreenGot){if(replace&&replace(ctx,s.visibleGot,s.visibleOriginal,s.visibleHook))g|=GradeO;else failures+="H3_REPLACE,";}
        else failures+="OFFSCREEN_PRINT,";}
    if(c.launchPrints&&c.launchGots){
        bool h2=false;
        if(config.inflight){h2=replace&&replace(ctx,s.flightGot,s.flightOriginal,s.flightHook);if(!h2)failures+="H2_REPLACE,";}
        if(!config.inflight||h2){
            if(replace&&replace(ctx,s.useGot,s.useOriginal,s.useHook))g|=GradeL;
            else{failures+="H1_REPLACE,";if(h2&&!(replace&&replace(ctx,s.flightGot,s.flightHook,s.flightOriginal)))failures+="H2_PUTBACK,";}
        }
    }else failures+="LAUNCH_PRINTS,";
    if(!c.directWord)failures+="NO_DIRECT,";
    return g;
}
inline std::string gradeText(unsigned g) noexcept {std::string t;if(g&GradeH)t+='H';if(g&GradeR)t+='R';if(g&GradeL)t+='L';if(g&GradeO)t+='O';return t;}
// Publishes the grades, the status and registers ModuleLasso (tick only) when the executor runs. Idempotent registration.
inline bool finishInstall(unsigned g,const std::string& failures) noexcept {
    installFailures=failures;grades.store(g,std::memory_order_release);
    if(killed(Feature::Lasso)){installState.store("KILLED");status::publish(Feature::Lasso,status::Property);return false;}
    installState.store(g&GradeR?"INSTALLED":"UNAVAILABLE");
    status::publish(Feature::Lasso,g&GradeR?status::Ready:status::Unavailable,g&GradeR?"NOT OWNED":nullptr);
    if(!mainthread::executorInstalled())return false;
    if(!mainthread::registerModule(mainthread::ModuleLasso,"lasso",&moduleTick,nullptr)){installState.store("DUPLICATE MODULE");return false;}
    module.installed=(g&GradeR)!=0;return module.installed;
}
inline void resetModuleForTests() noexcept {
    module.resetForTests();token=Token{};flightBox.store(FlightBox{});grades.store(0);installState.store("NOT ATTEMPTED");installFailures.clear();
    env=Env{};config=Config{};mainthread::registerModule(mainthread::ModuleLasso,"lasso",nullptr,nullptr);mainThreadToken.store(0);
    tploco::externalBusy.store(0);resetForTests();
}

// ============================== report vr.lasso ==============================
inline std::string report() {
    std::string o;o.reserve(8192);char b[1600];const auto& s=stats;const auto vs=settings();
    const auto g=grades.load();const auto st=brain.state();
    EngineFacts f{};engineBox.load(f);PlayerLink pl{};playerBox.load(pl);const auto& r=module.last;
    const auto U=[](const Counter& c){return (unsigned long long)u64(c);};
    std::snprintf(b,sizeof b,"{\"enabled\":%s,\"killed\":%s,\"install\":{\"state\":\"%s\",\"grade\":\"%s\",\"failures\":\"%s\"},\"dry\":%s,\"mode\":\"%s\","
        "\"assist\":%s,\"arc\":\"%s\",\"note\":\"%s\",\"state\":\"%s\",\"holdHand\":%d,\"throwHand\":%d,\"throwHandRule\":\"%s\",\"owned\":%s,\"slotRaw\":%s,"
        "\"inHand\":%s,\"aimRequest\":%s,\"ropeBusy\":%s,\"faceRemap\":%s,\"arcUsable\":%s,\"lastRefusal\":\"%s\",",
        active()?"true":"false",killed(Feature::Lasso)?"true":"false",installState.load(),gradeText(g).c_str(),installFailures.c_str(),config.dry?"true":"false",
        modeNames[std::min(vs.lassoThrowMode(),2u)],vs.lassoAssist()?"true":"false",arcShowNames[unsigned(config.arc)],module.noteNow.load(),
        brainStateNames[unsigned(st)],holdHandFlag.load(),throwHandFlag.load(),handRuleNames[unsigned(config.throwHand)],pl.owned?"true":"false",
        pl.slotItem?"true":"false",pl.inHand?"true":"false",aimRequest()?"true":"false",ropeBusy()?"true":"false",faceRemap()?"true":"false",
        arcUsable.load()?"true":"false",lastRefusal.load());o+=b;
    FlightBox fb{};flightBox.load(fb);
    std::snprintf(b,sizeof b,"\"engine\":{\"item\":%s,\"ownerOk\":%s,\"lassoState\":\"%s\",\"flight\":%u,\"userState\":\"%s\",\"victimState\":\"%s\",\"hogtie\":\"%s\","
        "\"minigame\":%s,\"focus\":%d,\"promptCode\":%u,\"launchVelocity\":%.3f,\"maxThrowDistance\":%.3f,\"probeRadius\":%.3f,\"forceAttractor\":%s,"
        "\"aimValid\":%u,\"forceMiss\":%u,\"gravity\":[%.3f,%.3f,%.3f],\"gravityMeasured\":[%.3f,%.3f,%.3f],\"dtMs\":%.2f,\"mounted\":%s,\"root\":%s,"
        "\"mover\":%s,\"ani\":%s,\"victim\":%s},",
        r.item?"true":"false",r.ownerOk?"true":"false",nameOf(lassoStateNames,r.lassoState),r.flight,nameOf(userStateNames,r.userState),
        nameOf(victimStateNames,r.victimState),nameOf(hogtieNames,r.hogtie),f.minigame?"true":"false",r.focus,r.promptCode,double(r.launchVelocity),
        double(r.maxThrow),double(r.probeRadius),r.forceAttractor?"true":"false",unsigned(r.aimValid),unsigned(r.forceMiss),double(r.gravityV[0]),
        double(r.gravityV[1]),double(r.gravityV[2]),double(fb.gMeasured[0]),double(fb.gMeasured[1]),double(fb.gMeasured[2]),double(module.lastDtMs.load()),
        pl.riding?"true":"false",r.root?"true":"false",r.mover?"true":"false",r.ani?"true":"false",r.victim?"true":"false");o+=b;
    const auto tk=u64(s.ticks);
    std::snprintf(b,sizeof b,"\"tick\":{\"ticks\":%llu,\"usAvg\":%.1f,\"usMax\":%llu,\"readsPerFrame\":%.2f,\"skipped\":%llu,\"exceptions\":%llu},",
        U(s.ticks),tk?double(u64(s.tickUsSum))/double(tk):0.0,U(s.tickUsMax),tk?double(u64(s.readsPerFrameSum))/double(tk):0.0,U(s.skipped),U(s.exceptions));o+=b;
    std::snprintf(b,sizeof b,"\"brain\":{\"frames\":%llu,\"ready\":%llu,\"committed\":%llu,\"strongTicks\":%llu,\"cancelSlow\":%llu,\"cancelDown\":%llu,"
        "\"cancelGlitch\":%llu,\"cancelTracking\":%llu,\"estimateFallbacks\":%llu,\"rtPulses\":%llu,\"aimRequestFrames\":%llu,\"aimLatchedSuspendedFrames\":%llu,"
        "\"aimDroppedBy\":{\"grip\":%llu,\"engine\":%llu,\"dead\":%llu,\"cutscene\":%llu},\"stuckAimFrames\":%llu,\"maskedGrip\":%llu,\"holdTriggerWithheld\":%llu,"
        "\"openHandThrows\":%llu,\"keepHoldFrames\":%llu,\"regripGraces\":%llu,\"regripSaved\":%llu,\"recoverTimeouts\":%llu,\"rearmWaits\":%llu,"
        "\"nativeFallbackFrames\":%llu,\"flightCapHits\":%llu,\"toggleReleases\":%llu,\"catchPulses\":%llu,\"missPulses\":%llu,\"lastSwingPeak\":%.2f},",
        U(s.frames),U(s.ready),U(s.committed),U(s.strongTicks),U(s.cancelSlow),U(s.cancelDown),U(s.cancelGlitch),U(s.cancelTracking),U(s.estimateFallbacks),
        U(s.rtPulses),U(s.aimRequestFrames),U(s.aimLatchedSuspendedFrames),U(s.dropped[DropGrip]),U(s.dropped[DropEngine]),U(s.dropped[DropDead]),
        U(s.dropped[DropCutscene]),U(s.stuckAimFrames),U(s.maskedGrip),U(s.holdTriggerWithheld),U(s.openHandThrows),U(s.keepHoldFrames),U(s.regripGraces),
        U(s.regripSaved),U(s.recoverTimeouts),U(s.rearmWaits),U(s.nativeFallbackFrames),U(s.flightCapHits),U(s.toggleReleases),U(s.catchPulses),
        U(s.missPulses),double(lastSwingPeak.load()));o+=b;
    std::snprintf(b,sizeof b,"\"holsters\":{\"active\":%s,\"notOwnedMasked\":%llu,\"mountedLassoGrabs\":%llu,\"transferRefusedLasso\":%llu,"
        "\"phantomRefusedLasso\":%llu,\"socketGrabDeferred\":%llu,\"socketGrabDwelled\":%llu,\"lassoZoneEnters\":%llu,\"lassoTaught\":%s},",
        holstersActive()?"true":"false",U(s.notOwnedMasked),U(s.mountedLassoGrabs),U(s.transferRefusedLasso),U(s.phantomRefusedLasso),U(s.socketGrabDeferred),
        U(s.socketGrabDwelled),U(s.lassoZoneEnters),taught.load()?"true":"false");o+=b;
    const auto cs=u64(s.consumed),p0n=u64(s.p0Samples);
    std::snprintf(b,sizeof b,"\"launch\":{\"plans\":%llu,\"consumed\":%llu,\"rejectedPlan\":%llu,\"timeouts\":%llu,\"rtToLaunchMsAvg\":%.1f,\"rtToLaunchMsMax\":%.1f,"
        "\"directLaunches\":%llu,\"directRefused\":%llu,\"p0EngineVsVrCmAvg\":%.2f,\"p0EngineVsVrCmMax\":%.2f,\"p0Clamped\":%llu,\"p0WallHits\":%llu,"
        "\"v0Written\":%llu,\"ownerMismatch\":%llu,\"noPlan\":%llu,\"stalePlan\":%llu,\"dryCalls\":%llu,\"planSeq\":%llu,\"consumedSeq\":%llu},",
        U(s.plans),(unsigned long long)cs,U(s.rejectedPlan),U(s.timeouts),cs?double(u64(s.rtToLaunchUsSum))/double(cs)/1e3:0.0,double(u64(s.rtToLaunchUsMax))/1e3,U(s.directLaunches),
        U(s.directRefused),p0n?double(u64(s.p0EngineVsVrUmSum))/double(p0n)/1e4:0.0,double(u64(s.p0EngineVsVrUmMax))/1e4,U(s.p0Clamped),U(s.p0WallHits),
        U(s.v0Written),U(s.ownerMismatch),U(s.noPlan),U(s.stalePlan),U(s.dryCalls),(unsigned long long)brain.planSeq(),
        (unsigned long long)planConsumedSeq.load());o+=b;
    const auto ps=u64(s.predSamples),ss=u64(s.shownSamples);
    std::snprintf(b,sizeof b,"\"flight\":{\"steps\":%llu,\"tokenFlights\":%llu,\"flightsNative\":%llu,\"homingOn\":%llu,\"homingOff\":%llu,\"targetLost\":%llu,"
        "\"offscreenOverrides\":%llu,\"predErrCmAvg\":%.2f,\"predErrCmMax\":%.2f,\"shownVsThrownCmAvg\":%.2f,\"shownVsThrownCmMax\":%.2f,\"hits\":%llu,"
        "\"fails\":%llu,\"rangeFails\":%llu,\"catches\":%llu,\"catchesAfterDirect\":%llu,\"restored40c\":%llu,\"stripped\":%llu,\"tokenActive\":%s,\"tokensRetired\":%llu,"
        "\"last\":{\"state\":\"%s\",\"flight\":%u,\"steps\":%u,\"p\":[%.2f,%.2f,%.2f]}},",
        U(s.steps),U(s.tokenFlights),U(s.flightsNative),U(s.homingOn),U(s.homingOff),U(s.targetLost),U(s.offscreenOverrides),
        ps?double(u64(s.predErrUmSum))/double(ps)/1e4:0.0,double(u64(s.predErrUmMax))/1e4,ss?double(u64(s.shownVsThrownUmSum))/double(ss)/1e4:0.0,
        double(u64(s.shownVsThrownUmMax))/1e4,U(s.hits),U(s.fails),U(s.rangeFails),U(s.catches),U(s.catchesAfterDirect),U(s.restored40c),U(s.stripped),
        token.active?"true":"false",U(s.tokensRetired),nameOf(lassoStateNames,fb.state),fb.flight,fb.steps,double(fb.p[0]),double(fb.p[1]),double(fb.p[2]));o+=b;
    std::snprintf(b,sizeof b,"\"arc\":{\"frames\":%llu,\"published\":%llu,\"hidden\":%llu,\"targets\":%llu,\"targetSwitches\":%llu,\"pitchSolved\":%llu,"
        "\"pitchUnsolved\":%llu,\"outOfRange\":%llu,\"landings\":%llu,\"rings\":%llu,\"casts\":%llu,\"probeFallback\":%llu,\"noLaunchVelocity\":%llu,"
        "\"noArcTicks\":%llu,\"arms\":%llu,\"lastTargetRace\":%d,\"solves\":%llu,\"solvesReused\":%llu},",
        U(s.arcFrames),U(s.arcPublished),U(s.arcHidden),U(s.targets),U(s.targetSwitches),U(s.pitchSolved),U(s.pitchUnsolved),U(s.outOfRange),U(s.landings),
        U(s.rings),U(s.arcCasts),U(s.probeFallback),U(s.noLaunchVelocity),U(s.noArcTicks),U(s.arcArms),module.lastRace.load(),U(s.solves),U(s.solvesReused));o+=b;
    const auto sc=u64(s.scans);
    std::snprintf(b,sizeof b,"\"scan\":{\"scans\":%llu,\"candidates\":%u,\"candidatesTotal\":%llu,\"usAvg\":%.1f,\"usMax\":%llu,\"readsAvg\":%.1f,"
        "\"skippedTicks\":%llu},",
        U(s.scans),module.candCount,U(s.candidates),sc?double(u64(s.scanUsSum))/double(sc):0.0,U(s.scanUsMax),sc?double(u64(s.scanReads))/double(sc):0.0,
        U(s.scansSkipped));o+=b;
    std::snprintf(b,sizeof b,"\"catch\":{\"caught\":%llu,\"released\":%llu,\"tugWar\":%llu,\"hogtie\":%llu,\"faceRemapFrames\":%llu,\"minigameFrames\":%llu,"
        "\"minigameTriggerFrames\":%llu,\"minigameRtReleases\":%llu,\"promptUnpressable\":%llu,\"involuntaryMotionFrames\":%llu,\"warpWhileBusy\":%llu,"
        "\"stockFaceFrames\":%llu,\"promptCodesSeen\":{",
        U(s.caught),U(s.released),U(s.tugWar),U(s.hogtie),U(s.faceRemapFrames),U(s.minigameFrames),U(s.minigameTriggerFrames),U(s.minigameRtReleases),
        U(s.promptUnpressable),U(s.involuntaryMotionFrames),U(s.warpWhileBusy),U(s.stockFaceFrames));o+=b;
    {bool first=true;for(unsigned i=0;i<s.promptCodesSeen.size();++i){const auto n=u64(s.promptCodesSeen[i]);if(!n)continue;
        std::snprintf(b,sizeof b,"%s\"%u\":%llu",first?"":",",i,(unsigned long long)n);o+=b;first=false;}}
    o+="}},";
    std::snprintf(b,sizeof b,"\"rope\":{\"on\":%s,\"grabMask\":%u,\"engaged\":%s,\"source\":\"%s\",\"verts\":%u,\"handEndCm\":%.1f,\"victimEndCm\":%.1f,"
        "\"lastGrabCm\":%.1f,\"lastPullCm\":%.1f,\"lastPullSpeed\":%.2f,\"yankPrint\":%d,\"drawPrint\":%d,\"ropeGrabs\":%llu,\"pulls\":%llu,\"pullCm\":%llu,"
        "\"yankPulses\":%llu,\"yankEngine\":%llu,\"yankPad\":%llu,\"yankRefused\":%llu,\"yankUnsupportedCode\":%llu,\"grabMisses\":%llu,\"frames\":%llu,"
        "\"geomEngine\":%llu,\"geomLine\":%llu,\"geomBad\":%llu,\"geomNone\":%llu,\"followWrites\":%llu,\"slackKeeps\":%llu,\"slackStarts\":%llu,"
        "\"toggleGuarded\":%llu,\"graceSavedByRope\":%llu,",
        config.rope?"true":"false",ropeGrabMask.load(),ropeEngagedNs.load()?"true":"false",module.ropeSource.load(),module.ropeVerts.load(),
        double(module.ropeHandEndCm.load()),double(module.ropeVictimEndCm.load()),double(module.lastGrabCm.load()),double(module.lastPullCm.load()),
        double(module.lastPullSpeed.load()),module.yankPrint.load(),module.drawPrint.load(),U(s.ropeGrabs),U(s.pulls),U(s.pullCm),U(s.yankPulses),
        U(s.yankEngine),U(s.yankPad),U(s.yankRefused),U(s.yankUnsupportedCode),U(s.grabMisses),U(s.ropeFrames),U(s.ropeGeomEngine),U(s.ropeGeomLine),
        U(s.ropeGeomBad),U(s.ropeGeomNone),U(s.followWrites),U(s.slackKeeps),U(s.slackStarts),U(s.toggleGuarded),U(s.graceSavedByRope));o+=b;
    std::snprintf(b,sizeof b,"\"reel\":{\"on\":%s,\"print\":%d,\"active\":%s,\"lenCm\":%.1f,\"targetCm\":%.1f,\"minCm\":%.1f,\"victimCm\":%.1f,"
        "\"lastGainCm\":%.1f,\"pulls\":%llu,\"refused\":%llu,\"atMin\":%llu,\"writes\":%llu,\"rebuilt\":%llu,\"stopped\":%llu,\"gainCm\":%llu}},",
        config.reel?"true":"false",module.reelPrint.load(),module.reelActiveNow()?"true":"false",double(module.reelLenCm.load()),
        double(module.reelTargetCm.load()),double(module.reelMinCm.load()),double(module.reelVictimCm.load()),double(module.lastReelGainCm.load()),
        U(s.reelPulls),U(s.reelRefused),U(s.reelAtMin),U(s.reelWrites),U(s.reelRebuilt),U(s.reelStopped),U(s.reelGainCm));o+=b;
    std::snprintf(b,sizeof b,"\"hookThreads\":{\"main\":%llu,\"other\":%llu},\"trace\":[",U(s.hookMain),U(s.hookOther));o+=b;
    {const auto head=traceHead.load();const std::uint64_t n=std::min<std::uint64_t>(head,traceRing.size());bool first=true;
     for(std::uint64_t i=head-n;i<head;++i){TraceEntry t{};if(!traceRing[i%traceRing.size()].load(t))continue;
         char reason[17]{};std::memcpy(reason,t.reason,16);
         std::snprintf(b,sizeof b,"%s{\"ns\":%llu,\"brain\":\"%s\",\"lasso\":%u,\"user\":%u,\"victim\":%u,\"hogtie\":%u,\"forceAimOurs\":%s,\"aimRequest\":%u,"
             "\"remap\":%s,\"reason\":\"%s\"}",first?"":",",(unsigned long long)t.ns,t.brain<7?brainStateNames[t.brain]:"?",t.lasso,t.user,t.victim,t.hogtie,
             t.forceAimOurs?"true":"false",unsigned(t.aimRequest),t.remap?"true":"false",reason);o+=b;first=false;}}
    std::snprintf(b,sizeof b,"],\"config\":{\"launch\":\"%s\",\"inflight\":%d,\"offscreen\":%d,\"holdTrigger\":\"%s\",\"p0\":\"%s\",\"swingMin\":%.2f,"
        "\"swingCommit\":%.2f,\"swingAim\":\"%s\",\"kmin\":%.2f,\"kmax\":%.2f,\"rtPulse\":%u,\"minReadyMs\":%u,\"planAgeMs\":%u,\"assistRadius\":%.2f,"
        "\"assistAngle\":%.1f,\"assistPitch\":%.1f,\"probe\":\"%s\",\"unowned\":\"%s\",\"regripMs\":%u,\"deadzone\":%.2f,\"scanHz\":%u,\"scanMax\":%u,"
        "\"layout\":%d,\"yank\":%d,\"rope\":{\"on\":%d,\"radius\":%.3f,\"step\":%.3f,\"minSpeed\":%.2f,\"yank\":\"%s\",\"yankBtn\":%d,\"slackMs\":%u,"
        "\"gapMs\":%u,\"follow\":\"%s\",\"reel\":%d,\"reelStep\":%.2f,\"reelRate\":%.2f,\"reelMin\":%.2f}}}",
        config.launch==LaunchMode::Direct?"direct":"rt",int(config.inflight),int(config.offscreen),config.holdTriggerReady?"ready":"off",
        config.p0==P0Mode::Engine?"engine":"vr",double(config.swingMin),double(config.swingCommit),config.swingGaze?"gaze":"hand",double(config.kmin),
        double(config.kmax),config.rtPulse,config.minReadyMs,config.planAgeMs,double(config.assistRadiusM),double(config.assistAngleDeg),
        double(config.assistPitchDeg),config.probe==ProbeMode::Bullet?"bullet":"engine",config.unowned==Unowned::Strip?"strip":"native",config.regripMs,
        double(config.deadzone),config.scanHz,config.scanMax,int(config.layout),int(config.yank),int(config.rope),double(config.ropeRadiusM),
        double(config.pullStepM),double(config.pullMinSpeed),config.ropeYank==1?"pad":config.ropeYank==2?"off":"engine",config.yankBtn,config.slackMs,
        config.yankGapMs,config.ropeFollow?"draw":"off",int(config.reel),double(config.reelStepM),double(config.reelRateMps),double(config.reelMinM));o+=b;
    return o;
}
} // namespace rdrvr::vr::lasso
