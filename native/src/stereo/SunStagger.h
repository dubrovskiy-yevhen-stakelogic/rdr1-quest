#pragma once
// SHADOW UPDATE levels 5 STAGGER / 6 STAGGER FAR = staggered sun-cascade atlas. Included by
// ShadowUpdate.h in mid-file (namespace closed and reopened around the include): uses Pose,
// Thresholds, Delta, delta, angleDeg, clockUs, onRenderThread, stats, tracker, thresholds,
// insideAtlas, shadowMap, sphereBlock, originalRenderInto, tileProbe, TileProbe and foldTiles from
// there. No Vulkan types: the Vulkan side (SunStaggerLayer.h, TraceEntry only) passes plain
// integers, so every decision here can be driven off-device. librdr 1.58 arm64:
//  * Partial frame = the exact RenderIntoShadowMaps runs natively, but its full-atlas grcDevice::Clear (bl @0x2947220,
//    return 0x2947224; GOT 0x2e211f8 JUMP_SLOT) is dropped; a MASKED tile keeps BeginTileAroundSphere (Lock/Unlock balance)
//    with its old light matrix tile+0x10..0x4f written back, and its virtual rmlShadowMap::RenderShadowCasters (vtable
//    slot 0x2dbfe48, blr @0x2947368, return 0x294736c) is skipped with the ltLightProxy list counter +0x68 advanced to t+1
//    (DrawVisibleItemsForViewport draws list 8+[+0x68] and increments it; BeginShadowTileRendering starts it at 1);
//    a DRAWN tile sets the engine's "load next pass" byte librdr+0x4e6933cc (FlushFrameBuffer -> LOAD instead of
//    DONT_CARE) and the layer inserts vkCmdClearAttachments(depth = learned clear value, rect = the tile's quadrant) as the
//    first command of its pass. The drawn tile's matrix + sphere entry are committed only once that clear was confirmed.
//    If that pass is CLEAR / DONT_CARE after all (the byte did not stick), its renderArea is confined to the quadrant
//    (Vulkan load/store ops act only inside renderArea: the masked quadrants keep their content) and the clear is still
//    inserted. An empty drawn tile (no pass) whose old content had draws drops its snapshot,
//    so the next frame renders the full atlas (the native Clear would have emptied it); the byte goes back if no pass took it.
//  * Before every upload of the map (BeginRenderShadowsIntoShadowCollector / SetShadowMapConstants), each drawable tile's
//    16-byte sphere entry CShadowMap+0x130+16t is written from its snapshot (entry 0 stays native).
//  * Full frame (native path incl. the engine Clear) teaches the atlas extent, the clear value and the tile quadrants
//    (vkCmdSetViewport) and commits a snapshot for every tile it drew. Idle frame (indoor gate) = no atlas call + compose.
// Every engine pointer is proven by configure(); every hook acts only with rt.armed (set around the
// mod's own partial call on the render thread); faults (refused mask, wrong loadOp, secondary
// contents, extent, viewport, a drawn tile's pass without its quadrant clear) invalidate the
// snapshots (next frame full) and 3 faulty frames within 600 frames disable the levels for the
// session (they act as EVERY, _su5x).
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine::shadowupdate::stagger {
// ---- librdr 1.58 arm64 (module-relative) ----
inline constexpr std::uintptr_t castersSlotOffset=0x2dbfe48,castersOffset=0x29452a8,castersReturnOffset=0x294736c; // rmlShadowMap vtable +0x10
inline constexpr std::array<unsigned char,32> castersFingerprint{
#include "gamecode/SunStagger_29452a8_32_8d.inc"
};
inline constexpr std::uintptr_t castersCallOffset=0x2947364; // ldr x8,[x8,#0x10]; blr x8
inline constexpr std::array<unsigned char,8> castersCallFingerprint{8,9,64,249,0,1,63,214};
inline constexpr std::uintptr_t proxyField=0x1f8,proxyLoadOffset=0x29452bc; // ldr x0,[x0,#0x1f8]
inline constexpr std::array<unsigned char,4> proxyLoadFingerprint{0,252,64,249};
inline constexpr std::uintptr_t proxyVtableOffset=0x2dd3630;                // ltLightProxy object vptr
inline constexpr std::uintptr_t proxyBuildSlotOffset=0x2dd3648,proxyBuildOffset=0x2a65cac; // BuildVisibleListForViewport (ret)
inline constexpr std::uintptr_t proxyDrawSlotOffset=0x2dd3650,proxyDrawOffset=0x2a666e8;   // DrawVisibleItemsForViewport
inline constexpr std::array<unsigned char,4> proxyBuildFingerprint{192,3,95,214};
inline constexpr std::array<unsigned char,32> drawVisiblePrologue{
#include "gamecode/SunStagger_1914be0_32_8d.inc"
};
inline constexpr std::uintptr_t proxyCounterField=0x68,proxyModeField=0x90,proxyCharField=0x7f,proxyAltField=0x80;
inline constexpr std::uintptr_t beginShadowTileOffset=0x2a65c80;            // stp w2,w1,[x0,#0x68]; ret
inline constexpr std::array<unsigned char,8> beginShadowTileFingerprint{2,4,13,41,192,3,95,214};
inline constexpr std::uintptr_t drawShadowMapsProxyOffset=0x2a53f60;        // str wzr,[x0,#0x90]; cset; bl BeginShadowTileRendering
inline constexpr std::array<unsigned char,12> drawShadowMapsProxyFingerprint{31,144,0,185,226,23,159,26,234,89,9,148};
inline constexpr std::uintptr_t listSelectOffset=0x2a67110;                 // ldr w8,[x20,#0x68]; ldrb w9,[x20,#0x7f]; mov w10,#13; ldrb w11,[x20,#0x80]
inline constexpr std::array<unsigned char,16> listSelectFingerprint{
#include "gamecode/SunStagger_2a67110_16_8d.inc"
};
inline constexpr std::uintptr_t counterIncOffset=0x2a67c50;                 // ldr w8,[x20,#0x68]; add w8,w8,#1; str w8,[x20,#0x68]
inline constexpr std::array<unsigned char,12> counterIncFingerprint{136,106,64,185,8,5,0,17,136,106,0,185};
inline constexpr std::uintptr_t clearOffset=0x1ab8910,clearGotOffset=0x2e211f8,clearReturnOffset=0x2947224,clearShadowMapReturnOffset=0x29470c8;
inline constexpr std::array<unsigned char,32> clearFingerprint{
#include "gamecode/SunStagger_1ab8910_32_8d.inc"
};
inline constexpr std::uintptr_t clearCallOffset=0x2947220,clearShadowMapCallOffset=0x29470c4; // bl 0x2c31730 (grcDevice::Clear PLT)
inline constexpr std::array<unsigned char,4> clearCallFingerprint{68,169,11,148},clearShadowMapCallFingerprint{155,169,11,148};
inline constexpr std::uintptr_t preserveFlagOffset=0x4e6933cc;              // bss byte: "LOAD the next pass" (VkContext_c)
inline constexpr std::uintptr_t flushOffset=0x18add18;                      // VkContext_c::FlushFrameBuffer prologue
inline constexpr std::array<unsigned char,32> flushPrologue{
#include "gamecode/SunStagger_18add18_32_8d.inc"
};
inline constexpr std::uintptr_t flushPageOffset=0x18add9c;                  // adrp x22,0x4e693000
inline constexpr std::array<unsigned char,4> flushPageFingerprint{54,111,38,208};
inline constexpr std::uintptr_t flushReadOffset=0x18addcc;                  // ldr x9,[x19,#0x30b0]; ldrb w10,[x22,#0x3cc]
inline constexpr std::array<unsigned char,8> flushReadFingerprint{105,90,88,249,202,50,79,57};
inline constexpr std::uintptr_t flushSelectOffset=0x18adf78;                // LOAD if previous framebuffer or the byte, else DONT_CARE
inline constexpr std::array<unsigned char,20> flushSelectFingerprint{
#include "gamecode/SunStagger_18adf78_20_8d.inc"
};
inline constexpr std::uintptr_t flushResetOffset=0x18ae0a8;                 // strb wzr,[x22,#0x3cc]
inline constexpr std::array<unsigned char,4> flushResetFingerprint{223,50,15,57};
inline constexpr std::uintptr_t copySetOffset=0x18ae70c;                    // CopyTexture: adrp; mov w10,#1; strb w10,[x9,#0x3cc]
inline constexpr std::array<unsigned char,12> copySetFingerprint{41,111,38,176,42,0,128,82,42,49,15,57};
inline constexpr std::uintptr_t tilesField=0x20,tileCountField=0x28,fadeTilesField=0x1c4,skipFirstField=0x74,tileMatrixField=0x10,tileSphereField=0x70;
inline constexpr std::size_t tileMatrixBytes=0x40,sphereEntryBytes=16;
inline constexpr std::uintptr_t tileMatrixStoreOffset=0x2947010;            // stp q1,q2,[x23,#0x10]; stp q3,q0,[x23,#0x30] (only writer)
inline constexpr std::array<unsigned char,8> tileMatrixStoreFingerprint{225,138,0,173,227,130,1,173};
inline constexpr std::uintptr_t tileLoopOffset=0x2947314;                   // ldrh w8,[x19,#0x28]; ldr w9,[x19,#0x1c4]; cmp
inline constexpr std::array<unsigned char,12> tileLoopFingerprint{104,82,64,121,105,198,65,185,63,1,8,107};
inline constexpr std::uintptr_t skipFirstOffset=0x2947444;                  // tile 0 skipped while +0x74 == 1.0
inline constexpr std::array<unsigned char,16> skipFirstFingerprint{
#include "gamecode/SunStagger_2947444_16_8d.inc"
};
inline constexpr std::uintptr_t dontInvertZOffset=0x2e73674,dontInvertZGotOffset=0x2de07f8; // rage::g_grcDontInvertViewportZ (clear = byte ? 1.0 : 0.0)

// ---- levels, property ----
inline constexpr unsigned levelStagger=5,levelStaggerFar=6;
inline constexpr bool isLevel(unsigned v) noexcept {return v==levelStagger||v==levelStaggerFar;}
// debug.rdrvr.sunstagger (integer, base 0, re-read every 64 RenderScenes; unset = 0)
inline constexpr unsigned PropArea=1,PropNoGate=2,PropOff=4,PropLearn=8,PropOrder=16;
// PLAYER SHADOW WINDOW (default ON at level 5): 32 = off (kill switch, same as STEREO TEST 26 PLAYER SHADOW OFF),
// 64 = the window pass keeps the full atlas renderArea (no area shrink), 128 = the window skips records without a
// bound (kind != 0) instead of drawing them scissor-clipped (a cost lever; may drop their depth inside the window).
inline constexpr unsigned PropNoPlayer=32,PropPlayerFullArea=64,PropPlayerBoundedOnly=128;
inline constexpr float playerMarginBase=.25f,playerMarginMove=.5f,playerNearExtra=.6f,playerMaxFraction=.3f;
inline constexpr float sunFullDeg=1.f,coverageFullDefault=.5f,gateContribution=.9f;
inline constexpr std::uint64_t faultWindowFrames=600;
inline constexpr unsigned faultLimit=3;

// ---- pure policy (host-tested) ----
struct Shape {unsigned drawable{};bool ok{};}; // bit t = tile t is drawn by the engine; ok = {1,2,3} or {1,2}
inline Shape shapeOf(unsigned tileCount,int fadeTiles,float skipFirst) noexcept {
    Shape s{};const int n=std::min(int(tileCount)-fadeTiles,16);const unsigned first=skipFirst==1.f?1u:0u;
    for(int t=int(first);t<n;++t)s.drawable|=1u<<unsigned(t);
    s.ok=s.drawable==0xeu||s.drawable==0x6u;return s;
}
inline unsigned tileOf(unsigned drawable,unsigned k) noexcept { // k-th set bit (cyclic); 0 when empty
    const unsigned n=unsigned(std::popcount(drawable));if(!n)return 0;
    k%=n;for(unsigned t=0;t<32;++t)if(drawable&(1u<<t)){if(!k)return t;--k;}
    return 0;
}
enum Kind : unsigned {Idle,Partial,Full,Native};
enum Reason : unsigned {RoundRobin,Cycle,Gated,NoSnapshot,ShapeChange,Learn,NoPose,Cut,Sun,Coverage,Fault,Forced,Unavailable,ReasonCount};
inline constexpr const char* reasonNames[ReasonCount]{"roundRobin","cycle","gated","noSnapshot","shape","learn","noPose","cut","sun","coverage",
                                                      "fault","forced","unavailable"};
enum GateTrigger : unsigned {GateNone,GateTranslation,GateSun,GateRotation,GateTimer,GateExit,GateTriggerCount};
inline constexpr const char* gateTriggerNames[GateTriggerCount]{"none","translation","sun","rotation","timer","exit"};
struct Inputs {unsigned level{};Shape shape{};unsigned prevDrawable{};bool snapshots{},learned{},fault{},forced{},poseValid{};
               float frameMoveM{},sunDegOldest{};std::array<float,4> coverage{}; // |c_now - c_snap| / r_snap per tile
               bool gate{};bool exit{};Delta sinceCycle{};std::uint64_t sinceCycleUs{~0ull};};
struct Plan {Kind kind{Full};unsigned mask{};unsigned reason{NoSnapshot};unsigned trigger{GateNone};bool cycleStart{};};
struct PlanState {unsigned phase{},cycleLeft{};bool order1213{};};
// Tiles drawn on a partial frame. Level 5: D[phase] (order 1-2-1-3: D[0] on even phases, the others in turn on odd ones);
// level 6: D[0] | D[1 + phase % (|D|-1)].
inline unsigned maskFor(unsigned level,unsigned drawable,unsigned phase,bool order1213) noexcept {
    const unsigned n=unsigned(std::popcount(drawable));if(!n)return 0;
    if(level==levelStaggerFar&&n>=2)return (1u<<tileOf(drawable,0))|(1u<<tileOf(drawable,1+phase%(n-1)));
    if(order1213&&n>=2)return 1u<<tileOf(drawable,phase%2==0?0:1+(phase/2)%(n-1));
    return 1u<<tileOf(drawable,phase%n);
}
// Full triggers in this order: fault, noSnapshot, shape, learn, noPose, cut, sun, coverage (masked tiles of this frame),
// forced. Otherwise: the indoor gate (idle until an INDOOR trigger since the last cycle start, then one cycle of |D|
// (level 5) or |D|-1 (level 6) partial frames) or continuous round robin. A room exit ends the gate at once.
inline Plan plan(PlanState& ps,const Inputs& in,const Thresholds& t,float coverageFull=coverageFullDefault) noexcept {
    if(!isLevel(in.level))return {Native,0,Unavailable};
    const unsigned d=in.shape.drawable,n=unsigned(std::popcount(d));
    PlanState next=ps;Plan p{Partial,0,RoundRobin};
    const bool gate=in.gate&&!in.exit;
    if(in.exit){next.cycleLeft=0;p.trigger=GateExit;}
    if(gate){
        if(!next.cycleLeft){
            const auto& c=in.sinceCycle;unsigned trig=GateNone;
            if(c.valid&&c.translationM>t.indoorTranslationM)trig=GateTranslation;
            else if(c.valid&&c.sunDeg>t.indoorSunDeg)trig=GateSun;
            else if(c.valid&&t.indoorRotationDeg>0.f&&c.rotationDeg>t.indoorRotationDeg)trig=GateRotation;
            else if(!c.valid||in.sinceCycleUs>=t.indoorIntervalUs)trig=GateTimer;
            if(trig==GateNone)p={Idle,0,Gated};
            else{p.trigger=trig;p.cycleStart=true;next.phase=0;next.cycleLeft=in.level==levelStaggerFar&&n>=2?n-1:n;}
        }
        if(p.kind==Partial){p.reason=Cycle;p.mask=maskFor(in.level,d,next.phase,false);++next.phase;if(next.cycleLeft)--next.cycleLeft;}
    }else{
        next.cycleLeft=0;p.mask=maskFor(in.level,d,next.phase,next.order1213&&in.level==levelStagger);++next.phase;
    }
    const auto full=[&](unsigned reason){ps.cycleLeft=0;return Plan{Full,d,reason,p.trigger==GateExit?unsigned(GateExit):unsigned(GateNone),false};};
    if(in.fault)return full(Fault);
    if(!in.snapshots)return full(NoSnapshot);
    if(!in.shape.ok||d!=in.prevDrawable)return full(ShapeChange);
    if(!in.learned)return full(Learn);
    if(!in.poseValid)return full(NoPose);
    if(in.frameMoveM>t.cutM)return full(Cut);
    if(!(in.sunDegOldest<=sunFullDeg))return full(Sun);
    for(unsigned k=0;k<4;++k)if((d&~p.mask)&(1u<<k)&&!(in.coverage[k]<=coverageFull))return full(Coverage);
    if(in.forced)return full(Forced);
    ps=next;return p;
}

// ---- quadrant learning (pure) ----
struct Rect {int x{},y{},w{},h{};constexpr bool operator==(const Rect&) const = default;};
// A Vulkan viewport as a pixel rect: {x, min(y, y+h), w, |h|} (negative height = y-flipped viewport).
inline Rect viewportRect(float x,float y,float w,float h) noexcept {
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(w)||!std::isfinite(h)||std::fabs(x)>1e6f||std::fabs(y)>1e6f||std::fabs(w)>1e6f||std::fabs(h)>1e6f)return {};
    return {int(std::lround(x)),int(std::lround(std::min(y,y+h))),int(std::lround(w)),int(std::lround(std::fabs(h)))};
}
// The width/2 x height/2 cell that contains the rect's centre; true when the rect lies inside that cell.
inline bool cellOf(const Rect& r,unsigned width,unsigned height,Rect& cell) noexcept {
    if(width<2||height<2||width>65536||height>65536||r.w<=0||r.h<=0)return false;
    const int cw=int(width/2),ch=int(height/2);
    const double cx=double(r.x)+double(r.w)*.5,cy=double(r.y)+double(r.h)*.5;
    cell={cx<double(cw)?0:cw,cy<double(ch)?0:ch,cw,ch};
    return r.x>=cell.x&&r.y>=cell.y&&r.x+r.w<=cell.x+cell.w&&r.y+r.h<=cell.y+cell.h;
}
inline constexpr unsigned rectsPerTile=4;
// One full exact render as the layer saw it (render thread; reset by fullBegin).
struct FrameObservation {
    unsigned passes{},width{},height{};bool extentConsistent{true},clearSeen{};float clearDepth{-1.f};
    std::array<int,4> passLoad{-1,-1,-1,-1},passInitial{-1,-1,-1,-1}; // the first 4 atlas passes (VkAttachmentLoadOp, VkImageLayout)
    std::array<std::array<Rect,rectsPerTile>,4> rects{};std::array<unsigned,4> rectCount{},rectOverflow{};
    void addRect(unsigned t,const Rect& r) noexcept {
        if(t>=4)return;
        for(unsigned i=0;i<rectCount[t];++i)if(rects[t][i]==r)return;
        if(rectCount[t]<rectsPerTile)rects[t][rectCount[t]++]=r;else ++rectOverflow[t];
    }
};
struct Learned {
    unsigned width{},height{};float clearDepth{-1.f};const char* clearSource{"none"};int dontInvertZ{-1};
    std::array<Rect,4> quad{};unsigned quadMask{};std::array<unsigned,4> quadSeen{};unsigned validated{};
    std::uint64_t extentChanges{},conflicts{};
};
// After a full exact render: extent (a change resets everything learned), clear value (cross-checked with
// g_grcDontInvertViewportZ when it was readable: byte 1 -> 1.0, 0 -> 0.0) and, per drawable tile, the cell of its
// viewport(s). A tile is validated once its cell was seen identically in >= 2 full renders and no other drawable tile
// has the same cell.
inline void foldLearning(Learned& l,const FrameObservation& o,unsigned drawable,int dontInvertZ) noexcept {
    l.dontInvertZ=dontInvertZ;
    if(o.passes&&o.width&&o.height&&o.extentConsistent&&(o.width!=l.width||o.height!=l.height)){
        if(l.width)++l.extentChanges;
        l.width=o.width;l.height=o.height;l.quad={};l.quadMask=0;l.quadSeen={};l.validated=0;l.clearDepth=-1.f;l.clearSource="none";
    }
    if(o.clearSeen&&std::isfinite(o.clearDepth)&&o.clearDepth>=0.f&&o.clearDepth<=1.f){
        if(dontInvertZ<0){l.clearDepth=o.clearDepth;l.clearSource="pass";}
        else if(o.clearDepth==(dontInvertZ?1.f:0.f)){l.clearDepth=o.clearDepth;l.clearSource="pass+byte";}
        else{l.clearDepth=-1.f;l.clearSource="mismatch";}
    }
    if(!l.width)return;
    for(unsigned t=0;t<4;++t){
        if(!(drawable&(1u<<t)))continue;
        bool any=false,conflict=false;Rect found{};
        for(unsigned i=0;i<o.rectCount[t];++i){Rect c{};if(!cellOf(o.rects[t][i],l.width,l.height,c))continue;if(any&&!(c==found))conflict=true;found=c;any=true;}
        if(conflict){l.quadSeen[t]=0;l.quadMask&=~(1u<<t);++l.conflicts;continue;}
        if(!any)continue; // no viewport inside one cell this time (e.g. an empty tile): keep what was learned
        if(l.quadSeen[t]&&l.quad[t]==found){if(l.quadSeen[t]<1000000u)++l.quadSeen[t];}
        else{l.quad[t]=found;l.quadSeen[t]=1;}
        l.quadMask|=1u<<t;
    }
    l.validated=0;
    for(unsigned t=0;t<4;++t){
        if(!(drawable&(1u<<t))||l.quadSeen[t]<2)continue;
        bool unique=true;
        for(unsigned u=0;u<4;++u)if(u!=t&&(drawable&(1u<<u))&&(l.quadMask&(1u<<u))&&l.quad[u]==l.quad[t])unique=false;
        if(unique)l.validated|=1u<<t;
    }
}
inline bool learnedFor(const Learned& l,unsigned drawable) noexcept {
    return drawable&&drawable<16u&&l.width&&l.height&&l.clearDepth>=0.f&&(l.validated&drawable)==drawable;
}
} // namespace rdrvr::engine::shadowupdate::stagger
#include "SunStaggerPlayer.h" // PLAYER SHADOW WINDOW: pure part (uses Rect)
namespace rdrvr::engine::shadowupdate::stagger {

// ---- render pass table (depth-only single-attachment passes; the layer feeds it from vkCreateRenderPass) ----
inline constexpr int loadOpLoad=0,loadOpClear=1,loadOpDontCare=2; // VkAttachmentLoadOp
struct PassEntry {std::uint64_t handle{};int load{-1},initial{-1},format{};std::uint64_t begins{};};
class PassTable {
public:
    static constexpr unsigned capacity=64;
    void created(std::uint64_t h,int load,int initial,int format) noexcept {
        if(!h)return;std::lock_guard guard(lock_);
        for(unsigned i=0;i<count_;++i)if(entries_[i].handle==h){entries_[i]={h,load,initial,format,0};return;}
        if(count_<capacity)entries_[count_++]={h,load,initial,format,0};else ++overflow_;
    }
    void destroyed(std::uint64_t h) noexcept {
        if(!h)return;std::lock_guard guard(lock_);
        for(unsigned i=0;i<count_;++i)if(entries_[i].handle==h){entries_[i]=entries_[--count_];entries_[count_]={};return;}
    }
    bool find(std::uint64_t h,PassEntry& out,bool countBegin) noexcept {
        std::lock_guard guard(lock_);
        for(unsigned i=0;i<count_;++i)if(entries_[i].handle==h){if(countBegin)++entries_[i].begins;out=entries_[i];return true;}
        return false;
    }
    // [{load, initial, format, begins}] of the passes begun inside an atlas render at a stagger level (creation order)
    void json(std::ostream& o) const {
        std::lock_guard guard(lock_);
        o<<'[';bool first=true;
        for(unsigned i=0;i<count_;++i){const auto& e=entries_[i];if(!e.begins)continue;
            o<<(first?"":",")<<"{\"load\":"<<e.load<<",\"initial\":"<<e.initial<<",\"format\":"<<e.format<<",\"begins\":"<<e.begins<<'}';first=false;}
        o<<']';
    }
    unsigned count() const noexcept {std::lock_guard guard(lock_);return count_;}
    std::uint64_t overflow() const noexcept {std::lock_guard guard(lock_);return overflow_;}
    void clear() noexcept {std::lock_guard guard(lock_);entries_={};count_=0;overflow_=0;}
private:
    mutable std::mutex lock_;std::array<PassEntry,capacity> entries_{};unsigned count_{};std::uint64_t overflow_{};
};
inline PassTable passTable;

// ---- runtime state (render thread only unless atomic) ----
// drew: the committed quadrant content holds caster draws (a partial commit, or a full frame where the tile began a pass and
// set a viewport inside one cell); false = known empty (cleared), so an empty redraw of the tile changes nothing.
// vp = the tile's grcViewport as it was drawn (the window redraw puts it back), content = the player-ish spheres drawn
// into the tile's committed content (what the next window must erase).
struct TileSnap {std::array<unsigned char,tileMatrixBytes> matrix{};std::array<unsigned char,sphereEntryBytes> sphere{};float sun[3]{};std::uint64_t frame{};bool valid{},drew{};
                 std::array<unsigned char,player::vpBytes> vp{};bool vpValid{};player::SphereSet content{};};
struct Runtime {
    const void* map{};
    std::array<TileSnap,4> tiles{},pending{};unsigned pendingMask{};
    PlanState ps{};unsigned drawable{},drawMask{};
    bool armed{},armedFull{},partial{};int tile{-1};bool masked{};
    std::array<unsigned char,tileMatrixBytes> fresh{};
    int clearTile{-1};unsigned clearConfirmed{},passMask{}; // passMask: drawn tiles of this partial render that began a pass
    unsigned fullPassMask{};                                // tiles of this full render that began a pass
    bool byteArmed{};std::uint8_t byteSaved{};              // "load next pass" request not yet taken by a pass begin
    Pose lastPose{},cyclePose{},pose{};std::uint64_t cycleUs{},frame{};
    bool faultPending{};std::array<std::uint64_t,faultLimit> faultFrames{};unsigned faultCount{};
    // PLAYER SHADOW WINDOW (the masked near tile redrawn around the player)
    bool windowPending{},window{},windowEnded{},windowPass{},windowEarly{};int windowTile{-1};int windowConv{-1};Rect windowRect{};player::FRect windowF{};
    float windowT[16]{};unsigned char* windowVp{};std::array<unsigned char,player::vpBytes> freshVp{};player::SphereSet windowDrawn{};std::uint64_t windowT0{},windowDraws{};
    player::SphereSet cur{},last{};float curPos[3]{},lastPos[3]{};bool curPoseValid{},lastValid{};std::uint64_t lastFrame{};
};
inline Runtime rt;
inline Learned learned;                       // render thread (published below for the report)
struct LayerState {int learnTile{-1};bool viewportChecked{};bool insert{};int insertTile{-1};Rect insertRect{};FrameObservation obs{};};
inline LayerState layerState;                 // render thread
// unconfirmed: a drawn tile began a pass but never got its quadrant clear, and no other fault explained it this frame
// (e.g. the pass began before afterBeginTile armed the clear: new draws over old depth).
enum FaultKind : unsigned {FaultMaskRefused,FaultLoadOp,FaultNotInline,FaultExtent,FaultViewport,FaultRawClear,FaultUnconfirmed,FaultCount};
inline constexpr const char* faultNames[FaultCount]{"maskRefused","loadOp","notInline","extent","viewport","rawClear","unconfirmed"};
struct Hooks {std::uintptr_t castersReturn{},clearReturn{},clearShadowMapReturn{},proxyVtable{},preserveFlag{},dontInvertZ{};};
inline Hooks hooks;                           // written once by configure() before the slots go live
using Casters=void(*)(void*,void*,bool,unsigned);
using DeviceClear=void(*)(bool,std::uintptr_t,bool,float,bool,unsigned,int); // grcDevice::Clear(bool,Color32,bool,float,bool,uint,int)
inline Casters originalCasters{};
inline DeviceClear originalClear{};
inline std::atomic<bool> configured{},installed{},rawClearBound{},disabled{};
inline std::atomic<const char*> fingerprint{"unconfigured"},installResult{"notInstalled"},disabledReason{nullptr};
inline std::atomic<bool> layerArmed{};        // relaxed gate for the Vulkan hooks: inside an exact atlas render at a stagger level
inline std::atomic<bool> partialActive{};     // inside the mod's partial call (pipeline-creation counter)
inline std::atomic<unsigned> property{};      // debug.rdrvr.sunstagger bits
inline std::atomic<bool> invalidateRequest{}; // ClearShadowMap seen (any thread): snapshots dropped at the next route
inline std::atomic<std::uint64_t> sceneSerial{},propertyReads{},propertyInvalid{};
// PLAYER SHADOW WINDOW
inline std::atomic<std::uintptr_t> playerCreature{};   // the local player's crCreature (GameXr, left depth stage; 0 = unknown)
inline std::atomic<bool> windowFilterArmed{};          // DrawCommon filter of the window tile (render thread only acts)
inline std::atomic<bool> windowScissorArmed{};         // layer: clamp the engine's scissors into the window
inline std::atomic<bool> rawScissorBound{};            // layer: raw vkCmdSetScissor available
inline void(*scissorRestore)() noexcept{};             // layer: put the engine's scissor back after the window tile
struct PlayerCounters {
    std::atomic<std::uint64_t> considered{},armed{},confirmed{},unconfirmed{},noPass{},casterCalls{};
    std::array<std::atomic<std::uint64_t>,player::ReasonCount> reasons{};
    std::atomic<std::uint64_t> drawn{},skipped{},unbounded{},unpinned{},playerDrawn{},playerRecords{},nearRecords{},observed{},scissorClamps{},areaWindow{};
    std::atomic<std::uint64_t> windowPxSum{},windowPxMax{},drawnMax{};std::atomic<int> convention{-2};
    std::array<std::atomic<std::uint64_t>,8> unboundedKind{}; // +0xac bits 13..15 of the unbounded window records (0 = kind 0 without a sane sphere)
    LogHistogram cpuUs;std::atomic<std::uint64_t> cpuUsSum{};
    std::atomic<int> lastRect[4]{};
};
inline PlayerCounters playerCounters;
struct Counters {
    std::atomic<std::uint64_t> frames{},partialFrames{},fullFrames{},idleFrames{},nativeFrames{};
    std::array<std::atomic<std::uint64_t>,ReasonCount> reasons{};
    std::array<std::atomic<std::uint64_t>,4> tilesDrawn{},tilesMasked{},emptyTiles{},unconfirmedTiles{},maxAgeFrames{};
    std::array<std::atomic<std::uint64_t>,8> ageHist{};
    std::array<std::array<std::atomic<std::uint64_t>,10>,4> coverageHist{};
    std::atomic<std::uint64_t> counterMismatch{},maskRefused{},clearsDropped{},clearShadowMapSeen{},composes{},composeOther{},invalidations{},unplannedTiles{};
    std::atomic<std::uint64_t> emptyRedraws{},byteRestores{}; // empty drawn tile over drawn content -> snapshot dropped; unused byte put back
    std::array<std::atomic<std::uint64_t>,FaultCount> faults{};
    std::atomic<int> gateActive{-1};std::atomic<std::uint64_t> gateCycles{},gateIdle{};std::array<std::atomic<std::uint64_t>,GateTriggerCount> gateTriggers{};
    LogHistogram cpuUs;std::atomic<std::uint64_t> cpuUsSum{};
    // layer
    std::atomic<std::uint64_t> clearsInserted{},clearConfirmed{},loadOpLoad{},loadOpOther{},loadOpUnknown{},notInline{},extentMismatch{},areaShrunk{};
    std::atomic<std::uint64_t> viewportMismatch{},restarts{},maskedPasses{},pipelinesDuringPartial{},learnFrames{},fullPasses{},nonAtlasPasses{};
    std::atomic<std::uint64_t> confined{},unarmedPasses{}; // non-LOAD drawn pass confined to its quadrant; drawn tile's pass before its clear was armed
    // published learning / snapshots
    std::atomic<unsigned> width{},height{},validated{},quadMask{};std::atomic<float> clearDepth{-1.f};std::atomic<const char*> clearSource{"none"};
    std::atomic<int> dontInvertZ{-1};std::array<std::array<std::atomic<int>,4>,4> quad{};std::array<std::atomic<unsigned>,4> quadSeen{};
    std::array<std::atomic<int>,4> fullPassLoad{},fullPassInitial{};std::atomic<std::uint64_t> extentChanges{},conflicts{};
    std::array<std::array<std::atomic<float>,4>,4> sphere{};std::array<std::atomic<std::uint64_t>,4> sphereFrame{};
    std::array<std::atomic<float>,3> camera{};std::atomic<std::uint64_t> frame{};
};
inline Counters counters;
inline void add(std::atomic<std::uint64_t>& a,std::uint64_t v=1) noexcept {a.fetch_add(v,std::memory_order_relaxed);}

// configure: every code fingerprint, the vtable/GOT/proxy slot values, the two engine bytes, and the hooks.
template<class Code,class Read> bool configure(std::uintptr_t base,Code code,Read read) {
    const auto word=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t v{};return read(nullptr,base+slot,&v,sizeof v)&&v==base+target;};
    const auto byte=[&](std::uintptr_t offset,int& out){std::uint8_t v{};if(!read(nullptr,base+offset,&v,1)||v>1)return false;out=v;return true;};
    int preserve{},dontInvert{};const char* r="ok";
    if(!code(castersOffset,castersFingerprint))r="casters";
    else if(!code(castersCallOffset,castersCallFingerprint))r="castersCall";
    else if(!code(proxyLoadOffset,proxyLoadFingerprint))r="proxyLoad";
    else if(!code(proxyBuildOffset,proxyBuildFingerprint)||!code(proxyDrawOffset,drawVisiblePrologue))r="proxyCode";
    else if(!code(beginShadowTileOffset,beginShadowTileFingerprint)||!code(drawShadowMapsProxyOffset,drawShadowMapsProxyFingerprint))r="counterStart";
    else if(!code(listSelectOffset,listSelectFingerprint)||!code(counterIncOffset,counterIncFingerprint))r="counter";
    else if(!code(clearOffset,clearFingerprint)||!code(clearCallOffset,clearCallFingerprint)||!code(clearShadowMapCallOffset,clearShadowMapCallFingerprint))r="clear";
    else if(!code(flushOffset,flushPrologue)||!code(flushPageOffset,flushPageFingerprint)||!code(flushReadOffset,flushReadFingerprint)||
            !code(flushSelectOffset,flushSelectFingerprint)||!code(flushResetOffset,flushResetFingerprint)||!code(copySetOffset,copySetFingerprint))r="flush";
    else if(!code(tileMatrixStoreOffset,tileMatrixStoreFingerprint)||!code(tileLoopOffset,tileLoopFingerprint)||!code(skipFirstOffset,skipFirstFingerprint))r="atlasLoop";
    else if(!word(castersSlotOffset,castersOffset))r="castersSlot";
    else if(!word(clearGotOffset,clearOffset))r="clearGot";
    else if(!word(proxyBuildSlotOffset,proxyBuildOffset)||!word(proxyDrawSlotOffset,proxyDrawOffset))r="proxyVtable";
    else if(!word(dontInvertZGotOffset,dontInvertZOffset))r="dontInvertZGot";
    else if(!byte(preserveFlagOffset,preserve))r="preserveFlag";
    else if(!byte(dontInvertZOffset,dontInvert))r="dontInvertZ";
    else if(!stats.hooked.load(std::memory_order_relaxed))r="shadowUpdateHooks";
    const bool ok=r[0]=='o'&&r[1]=='k';
    hooks={base+castersReturnOffset,base+clearReturnOffset,base+clearShadowMapReturnOffset,base+proxyVtableOffset,base+preserveFlagOffset,base+dontInvertZOffset};
    fingerprint.store(r,std::memory_order_relaxed);configured.store(ok,std::memory_order_release);
    return ok;
}
// Stagger usable: proven, both slots replaced, the hooks live, not disabled by faults.
inline bool ready() noexcept {
    return configured.load(std::memory_order_acquire)&&installed.load(std::memory_order_acquire)&&stats.hooked.load(std::memory_order_relaxed);
}
// Why the levels act as EVERY right now (nullptr = they do not).
inline const char* unavailableReason() noexcept {
    if(!configured.load(std::memory_order_acquire))return fingerprint.load(std::memory_order_relaxed);
    if(!installed.load(std::memory_order_acquire))return installResult.load(std::memory_order_relaxed);
    if(!stats.hooked.load(std::memory_order_relaxed))return "shadowUpdateHooks";
    if(disabled.load(std::memory_order_relaxed))return "disabled";
    if(property.load(std::memory_order_relaxed)&PropOff)return "propertyOff";
    if(!rawClearBound.load(std::memory_order_relaxed))return "noRawClear";
    return nullptr;
}

// ---- engine memory helpers (render thread, inside the engine's own call) ----
inline unsigned char* tileObject(const void* map,unsigned t) noexcept {
    if(!map||t>=4)return nullptr;
    const unsigned char* tiles{};std::memcpy(&tiles,static_cast<const unsigned char*>(map)+tilesField,sizeof tiles);if(!tiles)return nullptr;
    unsigned char* tile{};std::memcpy(&tile,tiles+8*t,sizeof tile);return tile;
}
inline Shape readShape(const void* map) noexcept {
    const auto* m=static_cast<const unsigned char*>(map);std::uint16_t count{};std::int32_t fade{};float skip{};
    std::memcpy(&count,m+tileCountField,sizeof count);std::memcpy(&fade,m+fadeTilesField,sizeof fade);std::memcpy(&skip,m+skipFirstField,sizeof skip);
    return shapeOf(count,fade,skip);
}
inline std::uint8_t loadPreserveFlag() noexcept {
#if defined(__cpp_lib_atomic_ref)
    return std::atomic_ref<std::uint8_t>(*reinterpret_cast<std::uint8_t*>(hooks.preserveFlag)).load(std::memory_order_relaxed);
#else
    return __atomic_load_n(reinterpret_cast<std::uint8_t*>(hooks.preserveFlag),__ATOMIC_RELAXED);
#endif
}
inline void storePreserveFlag(std::uint8_t v) noexcept {
#if defined(__cpp_lib_atomic_ref)
    std::atomic_ref<std::uint8_t>(*reinterpret_cast<std::uint8_t*>(hooks.preserveFlag)).store(v,std::memory_order_relaxed);
#else
    __atomic_store_n(reinterpret_cast<std::uint8_t*>(hooks.preserveFlag),v,__ATOMIC_RELAXED);
#endif
}
// A drawn tile's first pass must LOAD: set the engine byte. The value it had before the first
// unanswered request is kept, so renderPartial can put it back when no pass begin took the request
// (an empty drawn tile): otherwise it would leak into the next engine pass of the render thread
// (DONT_CARE -> LOAD there).
inline void setPreserveFlag() noexcept {
    if(!hooks.preserveFlag)return;
    if(!rt.byteArmed){rt.byteSaved=loadPreserveFlag();rt.byteArmed=true;}
    storePreserveFlag(1);
}
// After the mod's partial call: the request is still pending (no pass began since it was set).
// Only the mod's own 1 is replaced; a 0 means the engine consumed it some other way (null flush) and
// stays.
inline void restorePreserveFlag() noexcept {
    if(!rt.byteArmed)return;
    rt.byteArmed=false;
    if(hooks.preserveFlag&&loadPreserveFlag()==1&&rt.byteSaved!=1){storePreserveFlag(rt.byteSaved);add(counters.byteRestores);}
}
inline int readDontInvertZ() noexcept {
    if(!hooks.dontInvertZ)return -1;std::uint8_t v{};std::memcpy(&v,reinterpret_cast<const void*>(hooks.dontInvertZ),1);return v<=1?int(v):-1;
}
inline float coverageOf(const unsigned char* tile,const TileSnap& s) noexcept {
    if(!tile)return 1e9f;
    float now[4]{},snap[4]{};std::memcpy(now,tile+tileSphereField,sizeof now);std::memcpy(snap,s.sphere.data(),sizeof snap);
    if(!(snap[3]>0.f)||!std::isfinite(snap[3]))return 1e9f;
    const float dx=now[0]-snap[0],dy=now[1]-snap[1],dz=now[2]-snap[2];
    const float c=std::sqrt(dx*dx+dy*dy+dz*dz)/std::sqrt(snap[3]);
    return std::isfinite(c)?c:1e9f;
}
inline void publishTile(unsigned t,const TileSnap& s) noexcept {
    float v[4]{};std::memcpy(v,s.sphere.data(),sizeof v);
    for(unsigned i=0;i<3;++i)counters.sphere[t][i].store(std::isfinite(v[i])?v[i]:0.f,std::memory_order_relaxed);
    counters.sphere[t][3].store(v[3]>0.f&&std::isfinite(v[3])?std::sqrt(v[3]):-1.f,std::memory_order_relaxed);
    counters.sphereFrame[t].store(s.frame,std::memory_order_relaxed);
}
inline void publishLearned() noexcept {
    auto& c=counters;const auto& l=learned;const auto r=std::memory_order_relaxed;
    c.width.store(l.width,r);c.height.store(l.height,r);c.validated.store(l.validated,r);c.quadMask.store(l.quadMask,r);
    c.clearDepth.store(l.clearDepth,r);c.clearSource.store(l.clearSource,r);c.dontInvertZ.store(l.dontInvertZ,r);
    for(unsigned t=0;t<4;++t){c.quad[t][0].store(l.quad[t].x,r);c.quad[t][1].store(l.quad[t].y,r);c.quad[t][2].store(l.quad[t].w,r);c.quad[t][3].store(l.quad[t].h,r);
        c.quadSeen[t].store(l.quadSeen[t],r);}
    c.extentChanges.store(l.extentChanges,r);c.conflicts.store(l.conflicts,r);
}

// ---- snapshots / fail-safe ----
inline void invalidate() noexcept {for(auto& t:rt.tiles)t.valid=false;add(counters.invalidations);}
// The atlas is no longer ours (foreign render, level left, unavailable): snapshots dropped, no compose.
inline void forget() noexcept {invalidate();rt.partial=false;rt.ps={};rt.lastPose={};rt.cyclePose={};rt.cycleUs=0;rt.pendingMask=0;rt.faultPending=false;}
// This frame already entered the disable window (any kind).
inline bool faultedThisFrame() noexcept {return rt.faultCount&&rt.faultFrames[faultLimit-1]==rt.frame;}
// Every fault is counted; the disable window counts at most one faulty frame per frame (3 faulty frames in 600 frames).
inline void fault(unsigned kind) noexcept {
    if(kind>=FaultCount)kind=FaultLoadOp;
    add(counters.faults[kind]);rt.faultPending=true;
    auto& w=rt.faultFrames;
    if(faultedThisFrame())return;
    for(unsigned i=0;i+1<faultLimit;++i)w[i]=w[i+1];
    w[faultLimit-1]=rt.frame;
    if(++rt.faultCount>=faultLimit&&rt.frame-w[0]<faultWindowFrames&&!disabled.exchange(true,std::memory_order_relaxed))
        disabledReason.store(faultNames[kind],std::memory_order_relaxed);
}
// Leaving the stagger levels (other level, FREEZE, SUN OFF): true when the atlas was mixed-time (the caller drops the
// whole-atlas snapshot so the first frame there renders).
inline bool leave() noexcept {
    auto& r=rt;const bool mixed=r.partial;
    bool any=false;for(const auto& t:r.tiles)any=any||t.valid;
    if(mixed||any||r.lastPose.valid)forget();
    counters.gateActive.store(-1,std::memory_order_relaxed);
    return mixed;
}
// Render-thread foreign RenderIntoShadowMaps or a (re)created atlas: nothing of ours is in it any more.
inline void foreignRendered() noexcept {if(rt.partial||rt.tiles[0].valid||rt.tiles[1].valid||rt.tiles[2].valid||rt.tiles[3].valid)forget();}

// ---- route (exact RenderIntoShadowMaps, render thread, stagger level, not FREEZE / SUN OFF) ----
inline Kind route(void* map,const Request& req,const Pose& now) noexcept {
    auto& c=counters;auto& r=rt;add(c.frames);
    ++r.frame;c.frame.store(r.frame,std::memory_order_relaxed);
    // The player spheres seen since the last route (previous frame: its atlas and eye stages) become the prediction
    // base, paired with that frame's camera position.
    if(r.cur.n&&r.curPoseValid){r.last=r.cur;std::memcpy(r.lastPos,r.curPos,sizeof r.lastPos);r.lastValid=true;r.lastFrame=r.frame-1;}
    else if(r.frame>r.lastFrame+2)r.lastValid=false;
    r.cur.clear();r.curPoseValid=now.valid;if(now.valid)std::memcpy(r.curPos,now.position,sizeof r.curPos);
    if(invalidateRequest.exchange(false,std::memory_order_relaxed))invalidate();
    if(r.map!=map){if(r.map)forget();r.map=map;}
    if(now.valid)for(unsigned i=0;i<3;++i)c.camera[i].store(now.position[i],std::memory_order_relaxed);
    if(unavailableReason()){add(c.nativeFrames);add(c.reasons[Unavailable]);leave();return Native;}
    const unsigned bits=property.load(std::memory_order_relaxed);
    const Shape shape=readShape(map);const unsigned d=shape.drawable;
    Inputs in{};in.level=req.level;in.shape=shape;in.prevDrawable=r.drawable;
    in.snapshots=(d&0xfu)!=0;in.sunDegOldest=0.f; // tiles >= 4 never have one: the shape rule catches them
    for(unsigned t=0;t<4;++t){
        if(!(d&(1u<<t)))continue;
        const auto& s=r.tiles[t];
        if(!s.valid){in.snapshots=false;in.coverage[t]=1e9f;continue;}
        in.sunDegOldest=std::max(in.sunDegOldest,now.valid?angleDeg(s.sun,now.sun):180.f);
        in.coverage[t]=coverageOf(tileObject(map,t),s);
        const float cov=in.coverage[t];add(c.coverageHist[t][cov>=.9f||!(cov>=0.f)?9u:unsigned(cov*10.f)]);
        const auto age=r.frame>s.frame?r.frame-s.frame:0;add(c.ageHist[std::min<std::uint64_t>(age,7)]);
        if(age>c.maxAgeFrames[t].load(std::memory_order_relaxed))c.maxAgeFrames[t].store(age,std::memory_order_relaxed);
    }
    in.learned=!(bits&PropLearn)&&learnedFor(learned,d);
    in.fault=r.faultPending;r.faultPending=false;
    in.forced=tracker.forceNext;
    const Delta move=delta(r.lastPose,now);in.poseValid=now.valid&&move.valid;in.frameMoveM=move.valid?move.translationM:0.f;
    in.gate=!(bits&PropNoGate)&&req.indoor&&indoor::stats.contributionLast.load(std::memory_order_relaxed)>=gateContribution;
    in.exit=indoor::consumeExit();
    const auto us=clockUs();
    in.sinceCycle=delta(r.cyclePose,now);in.sinceCycleUs=r.cycleUs&&us>=r.cycleUs?us-r.cycleUs:~0ull;
    r.ps.order1213=(bits&PropOrder)!=0;
    const Plan p=plan(r.ps,in,thresholds);
    r.lastPose=now;r.pose=now;r.drawable=d;
    if(p.cycleStart||p.kind==Full){r.cyclePose=now;r.cycleUs=us?us:1;}
    if(p.cycleStart)add(c.gateCycles);
    if(p.trigger!=GateNone&&p.trigger<GateTriggerCount)add(c.gateTriggers[p.trigger]);
    c.gateActive.store(in.gate&&!in.exit?1:0,std::memory_order_relaxed);
    add(c.reasons[p.reason<ReasonCount?p.reason:NoSnapshot]);
    if(p.kind==Idle){add(c.idleFrames);add(c.gateIdle);}
    else if(p.kind==Partial){add(c.partialFrames);r.drawMask=p.mask;}
    else add(c.fullFrames);
    return p.kind;
}
// Idle frame (indoor gate): the native call is skipped; the composite is uploaded.
inline void idle() noexcept {rt.partial=true;tracker.invalidate();}

// ---- tile hooks (existing BeginTileAroundSphere hook, exact caller, render thread) ----
// PLAYER SHADOW WINDOW helpers.
inline unsigned char* vpObject(const void* map,unsigned t) noexcept { // [[map+0x10]+8t] (grcViewport the tile renders with)
    if(!map||t>=4)return nullptr;
    const unsigned char* vps{};std::memcpy(&vps,static_cast<const unsigned char*>(map)+player::vpArrayField,sizeof vps);if(!vps)return nullptr;
    unsigned char* vp{};std::memcpy(&vp,vps+8*t,sizeof vp);return vp;
}
// The ltLightProxy of this map draws list 8+t at this tile's RenderShadowCasters (sun cascades, no character/alt list).
inline bool proxyReady(const void* map,unsigned t,bool countMismatch) noexcept {
    unsigned char* proxy{};std::memcpy(&proxy,static_cast<const unsigned char*>(map)+proxyField,sizeof proxy);
    if(!proxy)return false;
    std::uintptr_t vptr{};std::uint32_t mode{};std::uint8_t c7f{},c80{};std::int32_t counter{};
    std::memcpy(&vptr,proxy,sizeof vptr);std::memcpy(&mode,proxy+proxyModeField,sizeof mode);
    std::memcpy(&c7f,proxy+proxyCharField,1);std::memcpy(&c80,proxy+proxyAltField,1);std::memcpy(&counter,proxy+proxyCounterField,sizeof counter);
    if(vptr!=hooks.proxyVtable||mode||c7f||c80)return false;
    if(counter!=std::int32_t(t)){if(countMismatch)add(counters.counterMismatch);return false;}
    return true;
}
// Window requested: no kill switch (debug.rdrvr.sunstagger bit 32 / STEREO TEST 26 PLAYER SHADOW OFF).
inline bool windowRequested() noexcept {
    return !(property.load(std::memory_order_relaxed)&PropNoPlayer)&&stereo::testMode.load(std::memory_order_relaxed)!=stereo::testModePlayerShadowOff;
}
inline unsigned windowTileOf(unsigned drawable) noexcept {return tileOf(drawable,0);} // the nearest drawable tile (1)
inline void windowRestoreVp() noexcept {auto& r=rt;if(r.windowVp){std::memcpy(r.windowVp,r.freshVp.data(),player::vpBytes);r.windowVp=nullptr;}}
// beforeBeginTile of a masked tile on a partial frame: decide the window and put the snapshot viewport back (so the native
// BeginTileAroundSphere sets the old view current and rebuilds the old matrix).
inline void windowPrepare(const void* map,unsigned t) noexcept {
    auto& r=rt;auto& pc=playerCounters;
    if(t!=windowTileOf(r.drawable))return;
    add(pc.considered);
    const auto refuse=[&](unsigned why){add(pc.reasons[why<player::ReasonCount?why:unsigned(player::Off)]);};
    if(!windowRequested()||!rawScissorBound.load(std::memory_order_relaxed)||!scissorRestore||!playerCreature.load(std::memory_order_relaxed)){refuse(player::Off);return;}
    const auto& s=r.tiles[t];
    unsigned char* vp=vpObject(map,t);
    if(!s.valid||!s.vpValid||!vp){refuse(player::NoVp);return;}
    if(!(learned.validated&(1u<<t))||!(learned.clearDepth>=0.f)||!learned.width||!learned.height){refuse(player::NotLearned);return;}
    float T[16],entry[4];std::memcpy(T,s.matrix.data(),sizeof T);std::memcpy(entry,s.sphere.data(),sizeof entry);
    const int conv=player::conventionFor(T,entry,learned.quad[t],learned.width,learned.height);
    playerCounters.convention.store(conv,std::memory_order_relaxed);
    if(conv<0){refuse(player::Projection);return;}
    if(!r.lastValid||!r.curPoseValid||r.frame>r.lastFrame+2){refuse(player::NoPlayer);return;}
    float d[3];for(unsigned i=0;i<3;++i)d[i]=r.curPos[i]-r.lastPos[i];
    const float move=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
    if(!std::isfinite(move)||move>thresholds.cutM){refuse(player::NoPlayer);return;}
    // where the player is now: last frame's spheres moved with the camera (first person: the camera rides on the player) and
    // where they were (third person / cutscene camera moving on its own) - both, so a wrong guess never clips him
    player::SphereSet predicted;player::predict(r.last,d,predicted);for(unsigned i=0;i<r.last.n;++i)predicted.add(r.last.s[i]);
    player::WindowInput in{T,conv,learned.width,learned.height,learned.quad[t],&predicted,&s.content,
                           playerMarginBase+playerMarginMove*move,.05f,playerMaxFraction};
    Rect rect{};player::FRect f{};
    if(const unsigned why=player::window(in,rect,f);why!=player::Ok){refuse(why);return;}
    std::memcpy(r.freshVp.data(),vp,player::vpBytes);std::memcpy(vp,s.vp.data(),player::vpBytes);
    r.windowVp=vp;r.windowPending=true;r.windowTile=int(t);r.windowRect=rect;r.windowF=f;r.windowConv=conv;std::memcpy(r.windowT,T,sizeof T);
    r.windowDrawn.clear();r.windowPass=false;r.windowEnded=false;r.windowEarly=false;
}
// afterBeginTile of the prepared tile: the matrix the engine rebuilt from the old viewport must be the snapshot matrix and
// the proxy must be about to draw list 8+t; then the window is armed (LOAD byte, window clear, scissor clamp, filter).
inline void windowArmOrAbort(const void* map,unsigned t) noexcept {
    auto& r=rt;auto& pc=playerCounters;
    if(!r.windowPending)return;
    r.windowPending=false;
    unsigned why=player::Ok;
    if(r.windowTile!=int(t))why=player::Off;
    else if(!player::sameMatrix(r.fresh.data(),r.tiles[t].matrix.data()))why=player::MatrixMismatch;
    else if(r.windowEarly)why=player::EarlyPass; // the tile's pass began at its Lock, before the window clear could be armed
    else if(!proxyReady(map,t,false))why=player::Proxy;
    if(why!=player::Ok){add(pc.reasons[why]);windowRestoreVp();r.windowTile=-1;return;}
    r.window=true;setPreserveFlag();r.clearTile=int(t);
    windowFilterArmed.store(true,std::memory_order_relaxed);windowScissorArmed.store(true,std::memory_order_relaxed);
    add(pc.armed);r.windowT0=clockUs();r.windowDraws=0;
    const auto px=std::uint64_t(r.windowRect.w)*std::uint64_t(r.windowRect.h);add(pc.windowPxSum,px);
    if(px>pc.windowPxMax.load(std::memory_order_relaxed))pc.windowPxMax.store(px,std::memory_order_relaxed);
    pc.lastRect[0].store(r.windowRect.x,std::memory_order_relaxed);pc.lastRect[1].store(r.windowRect.y,std::memory_order_relaxed);
    pc.lastRect[2].store(r.windowRect.w,std::memory_order_relaxed);pc.lastRect[3].store(r.windowRect.h,std::memory_order_relaxed);
}
// The window tile is over (next tile begins, or the mod's partial call returned): fresh viewport
// back, filter and scissor clamp off, the engine's scissor restored. The commit happens in
// finishPartial.
inline void windowEnd() noexcept {
    auto& r=rt;
    if(r.windowPending){r.windowPending=false;windowRestoreVp();r.windowTile=-1;}
    if(!r.window||r.windowEnded)return;
    r.windowEnded=true;
    windowFilterArmed.store(false,std::memory_order_relaxed);windowScissorArmed.store(false,std::memory_order_relaxed);
    windowRestoreVp();
    if(scissorRestore)scissorRestore();
    const auto now=clockUs();const auto us=now>r.windowT0?now-r.windowT0:0;
    playerCounters.cpuUs.add(us);add(playerCounters.cpuUsSum,us);
}
inline void beforeBeginTile(const void* map,int tile) noexcept {
    auto& r=rt;if(!r.armed&&!r.armedFull)return;
    windowEnd(); // The previous tile's window (if any) ends before this tile's BeginTileAroundSphere
    if(tile<0||tile>3||map!=r.map){r.tile=-1;r.masked=false;return;}
    const unsigned t=unsigned(tile),bit=1u<<t;
    r.tile=tile;r.clearTile=-1;
    r.masked=r.armed&&!(r.drawMask&bit);
    if(r.masked&&!r.tiles[t].valid){r.masked=false;r.drawMask|=bit;add(counters.unplannedTiles);} // no snapshot to keep: draw it
    if(r.masked&&!r.window)windowPrepare(map,t); // At most one window per partial render
    layerState.learnTile=tile;layerState.viewportChecked=false;
}
inline void capturePending(void* map,unsigned t,const unsigned char* matrix) noexcept {
    auto& p=rt.pending[t];std::memcpy(p.matrix.data(),matrix,tileMatrixBytes);
    std::memcpy(p.sphere.data(),sphereBlock(map)+sphereEntryBytes*t,sphereEntryBytes);
    for(unsigned i=0;i<3;++i)p.sun[i]=rt.pose.sun[i];
    p.frame=rt.frame;p.valid=true;rt.pendingMask|=1u<<t;
    // The viewport this tile is drawn with (after BeginTileAroundSphere's RegenerateScreen); its player content is
    // collected by playerDraw while the tile's casters draw
    if(const unsigned char* vp=vpObject(map,t)){std::memcpy(p.vp.data(),vp,player::vpBytes);p.vpValid=true;}else p.vpValid=false;
    p.content.clear();
}
inline void afterBeginTile(void* map,int tile) noexcept {
    auto& r=rt;if((!r.armed&&!r.armedFull)||tile<0||tile>3||map!=r.map||r.tile!=tile)return;
    const unsigned t=unsigned(tile);unsigned char* object=tileObject(map,t);
    if(!object){if(r.windowPending){r.windowPending=false;windowRestoreVp();r.windowTile=-1;}return;}
    unsigned char* matrix=object+tileMatrixField;
    if(r.masked){ // keep the old light matrix: the masked quadrant still holds the content it was rendered with
        std::memcpy(r.fresh.data(),matrix,tileMatrixBytes);
        windowArmOrAbort(map,t); // When prepared, fresh = the matrix rebuilt from the snapshot viewport
        std::memcpy(matrix,r.tiles[t].matrix.data(),tileMatrixBytes);return;}
    capturePending(map,t,matrix);
    if(r.armed){setPreserveFlag();r.clearTile=tile;} // partial frame: the tile's first pass LOADs; the layer clears its quadrant
}
// Virtual RenderShadowCasters (vtable slot): true = handled (masked tile skipped, list counter advanced).
inline bool casters(void* map,void*,bool,unsigned,std::uintptr_t caller) noexcept {
    if(caller!=hooks.castersReturn||!caller)return false;
    auto& r=rt;if(!onRenderThread()||!r.armed||!r.masked||map!=r.map||r.tile<0||r.tile>3)return false;
    const unsigned t=unsigned(r.tile);
    // The window tile renders natively (list 8+t, the engine advances the counter); DrawCommon filters to the window
    if(r.window&&!r.windowEnded&&r.windowTile==int(t)){add(playerCounters.casterCalls);return false;}
    if(proxyReady(map,t,true)){
        unsigned char* proxy{};std::memcpy(&proxy,static_cast<unsigned char*>(map)+proxyField,sizeof proxy);
        const std::int32_t next=std::int32_t(t)+1;std::memcpy(proxy+proxyCounterField,&next,sizeof next);add(counters.tilesMasked[t]);return true;
    }
    // Refused: the tile is drawn after all, with its fresh matrix and a cleared quadrant.
    add(counters.maskRefused);
    if(unsigned char* object=tileObject(map,t)){std::memcpy(object+tileMatrixField,r.fresh.data(),tileMatrixBytes);capturePending(map,t,object+tileMatrixField);}
    r.drawMask|=1u<<t;r.masked=false;setPreserveFlag();r.clearTile=int(t);
    fault(FaultMaskRefused);
    return false;
}
// DrawCommon hook (TraceEntry bodyDraw, before every other draw hook, any thread): true = skip this object.
//  * observation (render thread): records of the player's crCreature, and creatures touching him (mount, grappled NPC),
//    give the spheres the next window is built from, and the player content of a tile drawn now;
//  * filter (the window tile only): records whose bound does not touch the window are skipped.
// Common path: two relaxed loads; with the player known, one more field load per object (creature pointer).
inline bool playerDrawSlow(const unsigned char* info,bool pinned,bool filter,std::uintptr_t pcCreature) noexcept {
    auto& pc=playerCounters;
    if(!pinned){if(filter&&onRenderThread())add(pc.unpinned);return false;}
    std::uintptr_t creature{};std::memcpy(&creature,info+player::infoCreature,sizeof creature);
    if(!filter&&!creature)return false;
    if(!onRenderThread())return false;
    auto& r=rt;
    player::Sphere s{};bool has=false,playerish=false;
    if(creature&&pcCreature){
        if(creature==pcCreature){has=player::recordSphere(info,s);playerish=has;if(has)add(pc.playerRecords);}
        else if(r.lastValid){const int p=r.last.primary();
            if(p>=0){has=player::recordSphere(info,s);
                if(has){player::Sphere pp=r.last.s[unsigned(p)];for(unsigned i=0;i<3;++i)pp.c[i]+=r.curPos[i]-r.lastPos[i];
                    playerish=player::nearPlayer(s,pp,playerNearExtra);if(playerish)add(pc.nearRecords);}}}
    }
    const bool windowTile=r.window&&!r.windowEnded&&r.tile==r.windowTile;
    if(playerish){
        add(pc.observed);r.cur.add(s);
        if(windowTile){r.windowDrawn.add(s);add(pc.playerDrawn);}
        else if((r.armed||r.armedFull)&&!r.masked&&r.tile>=0&&r.tile<4&&insideAtlas.load(std::memory_order_relaxed))r.pending[unsigned(r.tile)].content.add(s);
    }
    if(!filter||!windowTile)return false;
    if(!has&&!player::recordSphere(info,s)){
        add(pc.unbounded);{std::uint32_t flags{};std::memcpy(&flags,info+player::infoFlags,4);add(pc.unboundedKind[(flags>>13)&7u]);}
        if(property.load(std::memory_order_relaxed)&PropPlayerBoundedOnly){add(pc.skipped);return true;}
        add(pc.drawn);++r.windowDraws;return false;
    }
    if(player::touches(r.windowT,r.windowConv,learned.width,learned.height,s,r.windowF)){add(pc.drawn);++r.windowDraws;return false;}
    add(pc.skipped);return true;
}
inline bool playerDraw(const void* info,bool pinned) noexcept {
    const bool filter=windowFilterArmed.load(std::memory_order_relaxed);
    const auto pcCreature=playerCreature.load(std::memory_order_relaxed);
    if((!filter&&!pcCreature)||!info)return false;
    return playerDrawSlow(static_cast<const unsigned char*>(info),pinned,filter,pcCreature);
}
// grcDevice::Clear (GOT): true = skip (the full-atlas clear of the mod's own partial call).
inline bool clear(std::uintptr_t caller) noexcept {
    if(!caller)return false;
    if(caller==hooks.clearReturn){
        if(!onRenderThread()||!rt.armed)return false;
        add(counters.clearsDropped);return true;
    }
    if(caller==hooks.clearShadowMapReturn){add(counters.clearShadowMapSeen);invalidateRequest.store(true,std::memory_order_relaxed);}
    return false;
}
// Before an upload of the sphere block (render thread): the per-tile composite while the atlas is ours.
inline bool compose(void* map) noexcept {
    auto& r=rt;if(!r.partial||map!=r.map||!map)return false;
    auto* block=sphereBlock(map);
    for(unsigned t=0;t<4;++t)if(r.drawable&(1u<<t))std::memcpy(block+sphereEntryBytes*t,r.tiles[t].sphere.data(),sphereEntryBytes);
    add(counters.composes);return true;
}
inline void composeOther(void* map) noexcept {if(compose(map))add(counters.composeOther);}

// ---- full / partial renders ----
// Full frame: tile t drew casters = it began a pass AND set a viewport inside one cell (either alone could in theory come
// from an empty tile: a pending clear flushed at its Unlock, a viewport flushed at Lock). Rect overflow counts as seen.
inline bool drewInFull(const FrameObservation& o,unsigned t) noexcept {
    if(t>=4||!(rt.fullPassMask&(1u<<t)))return false;
    if(o.rectOverflow[t])return true;
    for(unsigned i=0;i<o.rectCount[t];++i){Rect c{};if(cellOf(o.rects[t][i],learned.width,learned.height,c))return true;}
    return false;
}
inline void fullBegin(const void* map) noexcept {
    auto& r=rt;if(map!=r.map)return;
    r.armedFull=true;r.pendingMask=0;r.clearTile=-1;r.tile=-1;r.fullPassMask=0;layerState=LayerState{};
    layerArmed.store(true,std::memory_order_relaxed);
}
inline void fullEnd(const void* map) noexcept {
    auto& r=rt;if(!r.armedFull)return;
    layerArmed.store(false,std::memory_order_relaxed);r.armedFull=false;r.tile=-1;layerState.learnTile=-1;
    if(map!=r.map)return;
    const auto& o=layerState.obs;
    foldLearning(learned,o,r.drawable,readDontInvertZ());publishLearned();add(counters.learnFrames);
    for(unsigned t=0;t<4;++t)if(r.pendingMask&(1u<<t)){r.tiles[t]=r.pending[t];r.tiles[t].valid=true;r.tiles[t].drew=drewInFull(o,t);publishTile(t,r.tiles[t]);}
    r.pendingMask=0;r.partial=false;r.ps.phase=0;r.ps.cycleLeft=0;
    for(unsigned i=0;i<4;++i){counters.fullPassLoad[i].store(o.passLoad[i],std::memory_order_relaxed);counters.fullPassInitial[i].store(o.passInitial[i],std::memory_order_relaxed);}
    add(counters.fullPasses,o.passes);
}
// After the mod's partial call: commit each drawn tile whose quadrant clear was confirmed; any other
// drawn tile goes back to its old matrix (this frame's upload stays consistent with the quadrant's old
// content).
//  * A pass without the inserted clear (new draws over old depth) is always a faulty frame:
//    fault(unconfirmed) unless another fault already entered this frame into the disable window.
//  * No pass (no casters): the quadrant keeps its old content. If that content has draws, the native Clear would have
//    emptied it: the snapshot is dropped, so the next frame renders the full atlas (else the last caster's shadow would
//    stay as a ghost until an unrelated full trigger). A snapshot known to be empty is kept.
inline void finishPartial(void* map) noexcept {
    auto& r=rt;
    if(r.window){ // The window tile keeps its snapshot (old matrix); its player content is what the window drew now
        auto& pc=playerCounters;const unsigned wt=unsigned(r.windowTile)&3u,wbit=1u<<wt;
        if(r.clearConfirmed&wbit){r.tiles[wt].content=r.windowDrawn;add(pc.confirmed);add(counters.clearConfirmed);}
        else if(r.windowPass){add(pc.unconfirmed);add(counters.unconfirmedTiles[wt]);if(!faultedThisFrame())fault(FaultUnconfirmed);r.faultPending=true;}
        else add(pc.noPass); // nothing drawn: the old content (and its content spheres) stays
        if(r.windowDraws>pc.drawnMax.load(std::memory_order_relaxed))pc.drawnMax.store(r.windowDraws,std::memory_order_relaxed);
        r.window=false;r.windowEnded=false;r.windowTile=-1;r.windowPass=false;
    }
    for(unsigned t=0;t<4;++t){
        const unsigned bit=1u<<t;if(!(r.drawMask&bit))continue;
        if((r.clearConfirmed&bit)&&(r.pendingMask&bit)){r.tiles[t]=r.pending[t];r.tiles[t].valid=true;r.tiles[t].drew=true;publishTile(t,r.tiles[t]);add(counters.tilesDrawn[t]);add(counters.clearConfirmed);}
        else{
            const bool pass=(r.passMask&bit)!=0;
            if(pass){add(counters.unconfirmedTiles[t]);if(!faultedThisFrame())fault(FaultUnconfirmed);r.faultPending=true;}
            else add(counters.emptyTiles[t]);
            if(unsigned char* object=tileObject(map,t);object&&r.tiles[t].valid){
                std::memcpy(object+tileMatrixField,r.tiles[t].matrix.data(),tileMatrixBytes);
                if(!pass&&r.tiles[t].drew){r.tiles[t].valid=false;add(counters.emptyRedraws);}
            }else r.tiles[t].valid=false; // nothing consistent to fall back to: the next frame renders the full atlas
        }
    }
    r.pendingMask=0;
    if(r.faultPending)invalidate();
}
// The mod's partial call of the native RenderIntoShadowMaps. Returns its CPU time (us).
inline std::uint64_t renderPartial(void* map,bool a,void* sun,bool b,unsigned c,bool d,unsigned cullLevel) noexcept {
    auto& r=rt;
    r.armed=true;r.clearConfirmed=0;r.passMask=0;r.clearTile=-1;r.tile=-1;r.masked=false;r.pendingMask=0;r.byteArmed=false;layerState=LayerState{};
    r.window=r.windowPending=r.windowEnded=r.windowPass=false;r.windowTile=-1;r.windowVp=nullptr;
    tileProbe=TileProbe{};
    layerArmed.store(true,std::memory_order_relaxed);partialActive.store(true,std::memory_order_relaxed);insideAtlas.store(true,std::memory_order_relaxed);
    const auto t0=clockUs();
    originalRenderInto(map,a,sun,b,c,d);
    const auto t1=clockUs();
    windowEnd(); // The window tile was the last one drawn: viewport, scissor, filter back now
    insideAtlas.store(false,std::memory_order_relaxed);partialActive.store(false,std::memory_order_relaxed);layerArmed.store(false,std::memory_order_relaxed);
    r.armed=false;r.tile=-1;r.masked=false;r.clearTile=-1;layerState.learnTile=-1;layerState.insert=false;
    restorePreserveFlag(); // an empty drawn tile left the request pending: it must not reach the next engine pass
    finishPartial(map);
    tracker.invalidate();r.partial=true;
    foldTiles(cullLevel,t0,t1,r.drawMask);
    const auto us=t1>t0?t1-t0:0;counters.cpuUs.add(us);add(counters.cpuUsSum,us);
    return us;
}

// ---- layer side (SunStaggerLayer.h; caller checked layerArmed and onRenderThread) ----
// Before the driver's vkCmdBeginRenderPass. Returns the render area to use instead (partial frames: AREA, or a CLEAR /
// DONT_CARE drawn-tile pass confined to its quadrant) or nullptr.
inline const Rect* passBeginning(std::uint64_t pass,int offsetX,int offsetY,unsigned width,unsigned height,bool hasClear,float firstClear) noexcept {
    auto& r=rt;auto& ls=layerState;ls.insert=false;
    PassEntry e{};const bool known=passTable.find(pass,e,true);
    if(r.armedFull){ // learning
        auto& o=ls.obs;
        if(!known){add(counters.nonAtlasPasses);return nullptr;}
        if(r.tile>=0&&r.tile<4)r.fullPassMask|=1u<<unsigned(r.tile);
        if(o.passes<4){o.passLoad[o.passes]=e.load;o.passInitial[o.passes]=e.initial;}
        if(!o.passes){
            o.width=width;o.height=height;o.extentConsistent=!offsetX&&!offsetY;
            if(e.load==loadOpClear&&hasClear){o.clearDepth=firstClear;o.clearSeen=true;}
        }else if(width!=o.width||height!=o.height||offsetX||offsetY)o.extentConsistent=false;
        ++o.passes;return nullptr;
    }
    if(!r.armed)return nullptr;
    r.byteArmed=false; // every pass begin resets the engine's "load next pass" byte (FlushFrameBuffer @0x18ae0a8)
    const bool windowTile=r.window&&!r.windowEnded&&r.tile==r.windowTile;
    if(windowTile)r.windowPass=true;
    if(r.windowPending&&r.tile==r.windowTile)r.windowEarly=true;
    const bool drawn=r.tile>=0&&r.tile<4&&!r.masked;
    if(drawn)r.passMask|=1u<<unsigned(r.tile);
    if(!known){add(counters.loadOpUnknown);fault(FaultLoadOp);return nullptr;}
    const bool load=e.load==loadOpLoad;
    add(load?counters.loadOpLoad:counters.loadOpOther);
    const int t=r.clearTile;
    if(t<0||t>3){
        if(!load){fault(FaultLoadOp);return nullptr;} // no armed quadrant to confine it to: CLEAR wipes the masked quadrants, DONT_CARE = garbage
        if(r.masked)add(counters.maskedPasses);
        else if(drawn&&!(r.clearConfirmed&(1u<<unsigned(r.tile))))add(counters.unarmedPasses); // before afterBeginTile armed the clear: finishPartial faults unless a later pass of the tile gets it
        else add(counters.restarts); // a restarted pass of a cleared tile (content kept)
        return nullptr;
    }
    if(offsetX||offsetY||width!=learned.width||height!=learned.height){add(counters.extentMismatch);fault(load?FaultExtent:FaultLoadOp);return nullptr;}
    if(!(learned.validated&(1u<<unsigned(t)))||!(learned.clearDepth>=0.f)){fault(load?FaultExtent:FaultLoadOp);return nullptr;}
    if(!load&&!e.initial){fault(FaultLoadOp);return nullptr;} // initialLayout UNDEFINED: the whole image is undefined, renderArea cannot keep it
    const bool window=windowTile&&t==r.windowTile; // The window tile clears (and keeps) only the window rect
    ls.insert=true;ls.insertTile=t;ls.insertRect=window?r.windowRect:learned.quad[unsigned(t)];
    // CLEAR / DONT_CARE (the byte did not stick): renderArea = the quadrant. Load/store ops act only inside renderArea, so the
    // masked quadrants keep their content; the inserted clear defines the drawn quadrant.
    if(!load){add(counters.confined);return &ls.insertRect;}
    if(window&&!(property.load(std::memory_order_relaxed)&PropPlayerFullArea)){add(playerCounters.areaWindow);return &ls.insertRect;}
    if(property.load(std::memory_order_relaxed)&PropArea){add(counters.areaShrunk);return &ls.insertRect;}
    return nullptr;
}
// After the driver's vkCmdBeginRenderPass: true = record vkCmdClearAttachments(rect, depth) now; then clearInserted().
inline bool passBegun(bool inlineContents,bool rawClearAvailable,Rect& rect,float& depth) noexcept {
    auto& ls=layerState;if(!ls.insert)return false;
    ls.insert=false;
    if(!inlineContents){add(counters.notInline);fault(FaultNotInline);return false;}
    if(!rawClearAvailable){fault(FaultRawClear);return false;}
    rect=ls.insertRect;depth=learned.clearDepth;return true;
}
// The clear passBegun just handed out is the player window's (the layer then sets the window scissor).
inline bool windowClearPending() noexcept {const auto& r=rt;return r.window&&!r.windowEnded&&layerState.insertTile>=0&&layerState.insertTile==r.windowTile;}
inline void clearInserted() noexcept {
    auto& r=rt;auto& ls=layerState;add(counters.clearsInserted);
    if(ls.insertTile>=0&&ls.insertTile<4)r.clearConfirmed|=1u<<unsigned(ls.insertTile);
    r.clearTile=-1;ls.insertTile=-1; // one clear per tile, even if the engine restarts its pass
}
// vkCmdSetViewport (viewport 0): full renders collect each tile's distinct rects (from its BeginTileAroundSphere to the
// next one); partial renders check the drawn tile's first rect that fits one cell against its learned quadrant.
inline void viewportSeen(float x,float y,float w,float h) noexcept {
    auto& r=rt;auto& ls=layerState;const int t=ls.learnTile;if(t<0||t>3)return;
    const Rect rect=viewportRect(x,y,w,h);
    if(r.armedFull){ls.obs.addRect(unsigned(t),rect);return;}
    if(!r.armed||(r.masked&&!(r.window&&!r.windowEnded&&r.tile==r.windowTile))||ls.viewportChecked)return; // The window tile is checked too
    Rect cell{};if(!cellOf(rect,learned.width,learned.height,cell))return; // a full-target viewport (Lock) is no evidence
    ls.viewportChecked=true;
    if(!(cell==learned.quad[unsigned(t)])){add(counters.viewportMismatch);fault(FaultViewport);}
}
inline void pipelinesCreated(std::uint32_t n) noexcept {if(partialActive.load(std::memory_order_relaxed))add(counters.pipelinesDuringPartial,n);}

// ---- property, signature, report ----
#if defined(__ANDROID__)
inline void readProperty() noexcept {
    char value[PROP_VALUE_MAX]{};unsigned bits=0;
    if(__system_property_get("debug.rdrvr.sunstagger",value)>0){
        char* end{};const unsigned long v=std::strtoul(value,&end,0);
        if(end&&end!=value&&!*end&&v<=0xffu)bits=unsigned(v);else propertyInvalid.fetch_add(1,std::memory_order_relaxed);
    }
    property.store(bits,std::memory_order_relaxed);propertyReads.fetch_add(1,std::memory_order_relaxed);
}
#endif
// Every RenderScene (TraceEntry observeScene): debug.rdrvr.sunstagger every 64 scenes (scene 1, 65, ...).
inline void sceneBegin() noexcept {
    const auto scene=sceneSerial.fetch_add(1,std::memory_order_relaxed)+1;
#if defined(__ANDROID__)
    if((scene-1)%64==0){const int saved=errno;readProperty();errno=saved;}
#else
    (void)scene;
#endif
}
// Menu suffix source: 1 while the indoor gate holds the atlas, 0 not, -1 not at a stagger level.
inline int gateState() noexcept {return counters.gateActive.load(std::memory_order_relaxed);}
// runtime::sunStaggerReady: -1 not configured yet, 0 the levels act as EVERY (fingerprint, install, faults, no raw clear),
// 1 usable. The OFF property bit is a debug switch and does not count as unavailable.
inline int menuState() noexcept {
    const char* f=fingerprint.load(std::memory_order_relaxed);
    if(f&&!std::strcmp(f,"unconfigured"))return -1;
    return ready()&&!disabled.load(std::memory_order_relaxed)&&rawClearBound.load(std::memory_order_relaxed)?1:0;
}
// byState signature suffix after _su<level> (levels 5/6 only): "x" = acts as EVERY (unavailable / disabled / OFF), else
// a = AREA, n = NOGATE, l = LEARN, o = ORDER 1-2-1-3, w = player window off.
// "z" (any level) while the SunNight.h gate holds the sun shadows off; level 7 NIGHT OFF by day = the level 5 flags.
inline const char* signatureSuffix() noexcept {
    if(sunnight::suppressing())return "z";
    if(!isLevel(actingLevel(stats.level.load(std::memory_order_relaxed))))return "";
    if(unavailableReason())return "x";
    static constexpr const char* table[16]{"","a","n","an","l","al","nl","anl","o","ao","no","ano","lo","alo","nlo","anlo"};
    static constexpr const char* tableW[16]{"w","aw","nw","anw","lw","alw","nlw","anlw","ow","aow","now","anow","low","alow","nlow","anlow"};
    const unsigned b=property.load(std::memory_order_relaxed);
    const unsigned i=((b&PropArea)?1u:0u)|((b&PropNoGate)?2u:0u)|((b&PropLearn)?4u:0u)|((b&PropOrder)?8u:0u);
    return windowRequested()?table[i]:tableW[i]; // w = PLAYER SHADOW WINDOW off (property bit 32 / STEREO TEST 26)
}
inline void report(std::ostream& o) {
    const auto& c=counters;const auto ld=[](const std::atomic<std::uint64_t>& a){return a.load(std::memory_order_relaxed);};
    const auto f=[](float x){return std::isfinite(x)?x:-1.f;};
    const auto arr=[&](const std::array<std::atomic<std::uint64_t>,4>& a){o<<'['<<ld(a[0])<<','<<ld(a[1])<<','<<ld(a[2])<<','<<ld(a[3])<<']';};
    const unsigned level=stats.level.load(std::memory_order_relaxed);const char* why=unavailableReason();const char* dis=disabledReason.load();
    o<<"\"stagger\":{\"level\":"<<level<<",\"name\":\""<<(level==levelStaggerFar?"STAGGER FAR":level==levelStagger?"STAGGER":level==levelNightOff?"STAGGER / NIGHT OFF":"-")
     <<"\",\"ready\":"<<(ready()?1:0)<<",\"fingerprint\":\""<<fingerprint.load()<<"\",\"install\":\""<<installResult.load()
     <<"\",\"rawClear\":"<<(rawClearBound.load()?1:0)<<",\"disabled\":"<<(disabled.load()?1:0)<<",\"disabledReason\":\""<<(dis?dis:"")
     <<"\",\"unavailable\":\""<<(why?why:"")<<"\",\"property\":"<<property.load()<<",\"propertyReads\":"<<propertyReads.load()
     <<",\"propertyInvalid\":"<<propertyInvalid.load()<<",\"flags\":\""<<signatureSuffix()<<'"'
     <<",\"frames\":"<<ld(c.frames)<<",\"partialFrames\":"<<ld(c.partialFrames)<<",\"fullFrames\":"<<ld(c.fullFrames)<<",\"idleFrames\":"<<ld(c.idleFrames)
     <<",\"nativeFrames\":"<<ld(c.nativeFrames)<<",\"full\":{";
    {bool first=true;for(unsigned i=NoSnapshot;i<=Forced;++i){o<<(first?"":",")<<'"'<<reasonNames[i]<<"\":"<<ld(c.reasons[i]);first=false;}}
    o<<"},\"partial\":{\"roundRobin\":"<<ld(c.reasons[RoundRobin])<<",\"cycle\":"<<ld(c.reasons[Cycle])<<"},\"tilesDrawn\":";arr(c.tilesDrawn);
    o<<",\"tilesMasked\":";arr(c.tilesMasked);o<<",\"emptyTiles\":";arr(c.emptyTiles);o<<",\"unconfirmedTiles\":";arr(c.unconfirmedTiles);
    o<<",\"maxAgeFrames\":";arr(c.maxAgeFrames);
    o<<",\"ageHist\":[";for(unsigned i=0;i<8;++i)o<<(i?",":"")<<ld(c.ageHist[i]);
    o<<"],\"coverageHist\":[";for(unsigned t=0;t<4;++t){o<<(t?",":"")<<'[';for(unsigned i=0;i<10;++i)o<<(i?",":"")<<ld(c.coverageHist[t][i]);o<<']';}
    o<<"],\"counterMismatch\":"<<ld(c.counterMismatch)<<",\"maskRefused\":"<<ld(c.maskRefused)<<",\"clearsDropped\":"<<ld(c.clearsDropped)
     <<",\"clearShadowMapSeen\":"<<ld(c.clearShadowMapSeen)<<",\"composes\":"<<ld(c.composes)<<",\"composeOther\":"<<ld(c.composeOther)
     <<",\"invalidations\":"<<ld(c.invalidations)<<",\"unplannedTiles\":"<<ld(c.unplannedTiles)<<",\"emptyRedraws\":"<<ld(c.emptyRedraws)
     <<",\"byteRestores\":"<<ld(c.byteRestores)<<",\"faults\":{";
    for(unsigned i=0;i<FaultCount;++i)o<<(i?",":"")<<'"'<<faultNames[i]<<"\":"<<ld(c.faults[i]);
    o<<"},\"gate\":{\"active\":"<<c.gateActive.load()<<",\"cycles\":"<<ld(c.gateCycles)<<",\"idle\":"<<ld(c.gateIdle)<<",\"triggers\":{";
    for(unsigned i=GateTranslation;i<GateTriggerCount;++i)o<<(i>GateTranslation?",":"")<<'"'<<gateTriggerNames[i]<<"\":"<<ld(c.gateTriggers[i]);
    o<<"},\"contributionLast\":"<<f(indoor::stats.contributionLast.load())<<",\"threshold\":"<<gateContribution<<'}';
    const auto n=c.cpuUs.count();const auto ms=[](double us){return us<0?-1.:us/1000.;};
    o<<",\"cpuMsPartial\":{\"calls\":"<<n<<",\"mean\":"<<(n?double(ld(c.cpuUsSum))/double(n)/1000.:-1.)<<",\"p50\":"<<ms(c.cpuUs.percentile(.5))
     <<",\"p95\":"<<ms(c.cpuUs.percentile(.95))<<'}';
    o<<",\"learned\":{\"width\":"<<c.width.load()<<",\"height\":"<<c.height.load()<<",\"clearDepth\":"<<f(c.clearDepth.load())
     <<",\"clearSource\":\""<<c.clearSource.load()<<"\",\"dontInvertZ\":"<<c.dontInvertZ.load()<<",\"quadrants\":[";
    for(unsigned t=0;t<4;++t)o<<(t?",":"")<<"{\"x\":"<<c.quad[t][0].load()<<",\"y\":"<<c.quad[t][1].load()<<",\"w\":"<<c.quad[t][2].load()
                               <<",\"h\":"<<c.quad[t][3].load()<<",\"seen\":"<<c.quadSeen[t].load()<<'}';
    o<<"],\"validated\":"<<c.validated.load()<<",\"frames\":"<<ld(c.learnFrames)<<",\"extentChanges\":"<<c.extentChanges.load()
     <<",\"conflicts\":"<<c.conflicts.load()<<",\"lastFullPasses\":[";
    for(unsigned i=0;i<4;++i)o<<(i?",":"")<<"{\"load\":"<<c.fullPassLoad[i].load()<<",\"initial\":"<<c.fullPassInitial[i].load()<<'}';
    o<<"]},\"layer\":{\"clearsInserted\":"<<ld(c.clearsInserted)<<",\"clearConfirmed\":"<<ld(c.clearConfirmed)<<",\"loadOpLoad\":"<<ld(c.loadOpLoad)
     <<",\"loadOpOther\":"<<ld(c.loadOpOther)<<",\"loadOpUnknown\":"<<ld(c.loadOpUnknown)<<",\"notInline\":"<<ld(c.notInline)
     <<",\"extentMismatch\":"<<ld(c.extentMismatch)<<",\"viewportMismatch\":"<<ld(c.viewportMismatch)<<",\"areaShrunk\":"<<ld(c.areaShrunk)
     <<",\"restarts\":"<<ld(c.restarts)<<",\"maskedPasses\":"<<ld(c.maskedPasses)<<",\"unarmedPasses\":"<<ld(c.unarmedPasses)<<",\"confined\":"<<ld(c.confined)
     <<",\"fullPasses\":"<<ld(c.fullPasses)<<",\"nonAtlasPasses\":"<<ld(c.nonAtlasPasses)
     <<",\"passTable\":";passTable.json(o);
    o<<",\"passTableEntries\":"<<passTable.count()<<",\"passTableOverflow\":"<<passTable.overflow()
     <<",\"pipelinesDuringPartial\":"<<ld(c.pipelinesDuringPartial)<<"},\"spheres\":[";
    const float cam[3]{c.camera[0].load(),c.camera[1].load(),c.camera[2].load()};const auto frame=ld(c.frame);
    for(unsigned t=0;t<4;++t){
        const float x=c.sphere[t][0].load(),y=c.sphere[t][1].load(),z=c.sphere[t][2].load(),r=c.sphere[t][3].load();const auto sf=ld(c.sphereFrame[t]);
        const float dist=std::sqrt((x-cam[0])*(x-cam[0])+(y-cam[1])*(y-cam[1])+(z-cam[2])*(z-cam[2]));
        o<<(t?",":"")<<"{\"x\":"<<f(x)<<",\"y\":"<<f(y)<<",\"z\":"<<f(z)<<",\"r\":"<<f(r)<<",\"distFromCamera\":"<<(sf?f(dist):-1.f)
         <<",\"ageFrames\":"<<(sf&&frame>=sf?double(frame-sf):-1.)<<'}';
    }
    o<<']';
    { // PLAYER SHADOW WINDOW
        const auto& p=playerCounters;const auto pn=p.cpuUs.count();const auto armed=ld(p.armed);
        o<<",\"player\":{\"requested\":"<<(windowRequested()?1:0)<<",\"creature\":"<<(playerCreature.load()?1:0)<<",\"rawScissor\":"<<(rawScissorBound.load()?1:0)
         <<",\"convention\":"<<p.convention.load()<<",\"considered\":"<<ld(p.considered)<<",\"armed\":"<<armed<<",\"confirmed\":"<<ld(p.confirmed)
         <<",\"unconfirmed\":"<<ld(p.unconfirmed)<<",\"noPass\":"<<ld(p.noPass)<<",\"casterCalls\":"<<ld(p.casterCalls)<<",\"refused\":{";
        for(unsigned i=1;i<player::ReasonCount;++i)o<<(i>1?",":"")<<'"'<<player::reasonNames[i]<<"\":"<<ld(p.reasons[i]);
        o<<"},\"draws\":{\"drawn\":"<<ld(p.drawn)<<",\"skipped\":"<<ld(p.skipped)<<",\"unbounded\":"<<ld(p.unbounded)<<",\"unpinned\":"<<ld(p.unpinned)<<",\"unboundedKind\":[";
        for(unsigned i=0;i<8;++i)o<<(i?",":"")<<ld(p.unboundedKind[i]);
        o<<"]"
         <<",\"perWindow\":"<<(armed?double(ld(p.drawn))/double(armed):-1.)<<",\"maxPerWindow\":"<<ld(p.drawnMax)<<",\"playerDrawn\":"<<ld(p.playerDrawn)<<'}'
         <<",\"observed\":{\"records\":"<<ld(p.observed)<<",\"player\":"<<ld(p.playerRecords)<<",\"near\":"<<ld(p.nearRecords)<<'}'
         <<",\"windowPx\":{\"mean\":"<<(armed?double(ld(p.windowPxSum))/double(armed):-1.)<<",\"max\":"<<ld(p.windowPxMax)<<",\"last\":["<<p.lastRect[0].load()
         <<','<<p.lastRect[1].load()<<','<<p.lastRect[2].load()<<','<<p.lastRect[3].load()<<"]},\"areaWindow\":"<<ld(p.areaWindow)<<",\"scissorClamps\":"<<ld(p.scissorClamps)
         <<",\"cpuMsWindow\":{\"calls\":"<<pn<<",\"mean\":"<<(pn?double(ld(p.cpuUsSum))/double(pn)/1000.:-1.)<<",\"p50\":"<<ms(p.cpuUs.percentile(.5))
         <<",\"p95\":"<<ms(p.cpuUs.percentile(.95))<<"}}";
    }
    o<<'}';
}
} // namespace rdrvr::engine::shadowupdate::stagger
