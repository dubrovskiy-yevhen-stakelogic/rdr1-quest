#pragma once
// CREATURE LOD. NPCs and horses use their AUTHORED lower LODs by distance, in both eyes and in
// every shadow / reflection request. In a crowded interior scene on Quest 3, creature draws cost
// about 3.0 ms GPU for 38.6 DrawCommon objects (~19 creatures x 2 stages), 212 draws and 209 k
// triangles per frame (left tape).
// librdr 1.58 arm64. [V] = read in the binary.
//  * Creatures are frag types. sagDynamicNode::Draw @0x2a79750 (single caller, bl @0x2a79888) calls sagDynamicNode::DrawBig
//    @0x2a7993c (PLT 0x2caede0, JUMP_SLOT 0x2e5fd50) with x0 node, x2 sagFragType (fragInst+0x78 / +0x98), x5 = request+0x120 (camera
//    of THIS visibility request; request+0xcc 13 = MAIN), x6 skeleton, x7 fragHierarchyInst [V]. DrawBig computes the radial distance
//    |c-cam|-r (c = mid of node+0x60/+0x70, r = node+0x7c) and draws through sagDynamicObjectMgr::DrawN @0x2a7b404 (PLT 0x2caedf0,
//    JUMP_SLOT 0x2e5fd58; only caller: 6 sites in DrawBig) whose first stack int is the LOD: -1 = DrawN picks it from the drawable's
//    lodGroup thresholds +0x70/+0x74/+0x78 (LOD ptrs +0x50..+0x68) against the distance, >= 0 = explicit [V @0x2a7b4a8..0x2a7b500].
//    DrawN fetches the crCreature (fragInst+0x18 -> +0x8 -> +0x50 -> entEntityComponent::GetCreature) into renDrawBucketParams+0x78
//    = renDrawBucketInfo+0x98, the ablation's "creature" class [V @0x2a7b504..0x2a7b530].
//  * WHY CREATURES DRAW FULL LOD: frag types with sagFragType+0x211 != 0 (set from the "#fd"
//    streamed-drawable LOD, InitAfterLoad @0x2875cd0) do NOT use the per-instance distance at all.
//    DrawBig reads the per-TYPE detail level of propTextureLOD (stub = type+0x208, ptl =
//    stub+0x40; ptl+0 current level, +4 target level) and draws: level 2 (or stub+0x40 null, or a
//    shadow request type 10/11 with node flag 0x400) -> the resident drawable with explicit LOD 2;
//    level 0/1 -> the streamed "$/fragments/ <name>_hilod" / "_medlod" drawable (ptl+0x20 / +0x38,
//    24-byte pgStreamable entries) with explicit LOD 0/1 (character cloth: level 0 ->
//    AddDrawableWithCharClothToDrawList) [V @0x2a79b14..0x2a79c30, 0x2a79e0c..0x2a79e60,
//    0x2a7a048..0x2a7a138]. The level is decided once per frame for the whole TYPE by
//    propTextureLOD::PostUpdate @0x287d624 (PLT 0x2ca3e50, JUMP_SLOT 0x2e5a588, single caller bl
//    @0x287ea08 in propTextureLODManager::Update) from info+8 = min over the type's visible
//    instances of distance / size factor (preTextureLodUpdateSpu: size clamp s_TLD 0.2..3,
//    offscreen s_FLD) with the hysteresis table .rodata 0x12fbbc8 {24,40} {20,42} {20,38} (from
//    level 0 / 1 / 2: below lo -> 0, above hi -> 2, else 1) [V]. Horses (ptl+0x76 bit 5, stub+0x5e
//    >= 2 = sagFragTypeStub::sm_horseShaderGroups index) never go below the medlod within 300
//    (dist/size) [V @0x287d708, 0x287d890]. So (1) every instance of a type draws the level of its
//    NEAREST visible instance (shared ped / horse models in a town stay hilod), (2) hilod lasts to
//    24 m and medlod to 40 m (the resident PSCharacterLod model appears only beyond ~40 m), (3) the
//    VR camera is not the cause (union apex backshift <= 4 IPD; LOD is never FOV-scaled).
//  * The streamed hilod / medlod drawables of a level that is not current may be evicted at any frame (PerformEvictions
//    @0x287e024 -> pgQueueScheduler::Unschedule with DeleteFunc): a per-instance switch may only use the RESIDENT drawable (part of
//    the frag type, alive while the instance is) or the level the type already has.
// LEVERS (each live; debug.rdrvr.lod.creature.mask bits, default 7):
//  1 PER-TYPE: PostUpdate wrapper (exact caller) remaps info+8 of creature types (ptl registered by the DrawBig wrapper as a
//    crCreature's type, ptl+8 == stub re-checked) before the native call and restores it after: the ENGINE then switches and
//    streams hilod -> medlod -> resident at the policy distances (both eyes, every request, cloth, streaming memory, hysteresis).
//  2 PER-INSTANCE RESIDENT: DrawBig wrapper. A creature of a +0x211 type whose own distance (MAIN camera, radial) maps beyond the
//    authored resident distance (enter 41 / leave 38 in authored units, per-node hysteresis decided in MAIN, reused by shadow
//    requests) while its type is still at level 0/1 gets DrawBig with a stack copy of the frag type (0x218 B; DrawBig only loads from
//    it, no stores, no address escapes [V]) whose +0x208 points to a stub copy whose +0x40 points to a ptl copy with level 0 and 4
//    = 2: the native code then draws exactly its own resident path (DrawN resident drawable, LOD 2; also for cloth peds) [V]. The
//    resident drawable (damage / hierarchy variant as DrawBig selects it @0x2a799d0..0x2a79a78) must have LOD 2 (+0x60). Horses are
//    excluded (engine rule above) unless pinned.
//  4 LODGROUP CREATURES (type+0x211 == 0, DrawN LOD -1): DrawN wrapper sets an explicit LOD from the remapped distance (never more
//    detail than native, never a null LOD, never the native "beyond range" LOD 3 cull).
//  Details: (a) the per-instance lever uses the ENGINE'S OWN LOD METRIC of the instance, exactly as
//    preTextureLodUpdateSpu @0x287f0b0 builds info+8 [V]: m = min over the POIs (sagRenderer::sm_Instance +0xa8 rdrPOI[], u16
//    count +0xb0, at most 2, stride 0x4a0; StartTextureLODPreUpdate @0x287efc8..0x287f04c) of min(d, d / s''), d = |box centre -
//    POI pos|, s' = clamp(node+0x6c, s_TLD_SizeMinClamp 0.2, s_TLD_SizeMaxClamp 3.0), s'' = s' >= 1 ? s' : 0.4 s' + 0.6
//    [@0x287f3dc..0x287f4c8, 0x287f5d4..0x287f648]; node+0x6c = box half-diagonal, +0x7c = max half-extent (UpdateAABB
//    @0x2a7a8c0..0x2a7a938). Resident enters at m > 42 (table row 1 hi) and leaves at m <= 38 (row 2 hi), the engine's own rule for
//    a lone instance. While a POI has the sniper flag (+0x2d; compression by s_CLD_Sniper_Compress 6 inside the cos 0.7..0.9 cone)
//    or no fresh POI sample exists, the per-instance lever is native. POIs are sampled in the PostUpdate hook (sagShellDerived::
//    PerformDMZ, the sync point where the engine itself reads them). (b) protect = SURFACE distance max(0, |c - head| - R), R =
//    max(+0x6c, +0x7c), from the MAIN camera (DrawBig's dist+r fallback inside the sphere lets the ridden horse through), plus the
//    player's crCreature identity; shadow requests without a fresh MAIN camera are native. (c) pin 2 keeps horses at the engine's
//    medlod floor and excludes them per instance; pin 3 = pin 2 including horses (horse: stub+0x5e >= 2, propTextureLOD::Init
//    @0x287c74c).
// Never touched: surface distance < protect (debug.rdrvr.lod.creature.protect, default 2.5 m: the player's body, the ridden horse,
// a held / hogtied actor), the player's crCreature and the VR BODY's own skeleton (TraceEntry passes bodyik::collapseSkeleton).
// PROPERTIES (re-read at most once per second from observeScene): debug.rdrvr.lod.creature = policy: unset / "1" = authored
// distances applied per instance (DEFAULT ON: per-type identical to native, only instances whose own engine metric is beyond the
// engine's resident distance drop to the resident model while a nearer instance holds their type higher), 0 = kill (hooks forward
// everything), s (0.25 <= s < 8) = distance scale (2 = LOD distances halved), m (>= 8) = medlod from m and resident from m*40/24,
// "a:b" = medlod from a, resident from b (metric units: metres for a human-size creature, metres / size for large ones).
// debug.rdrvr.lod.creature.pin = -1 (default) / 0 hilod / 1 medlod / 2 resident (horses medlod) / 3 resident incl. horses, for every
// creature outside protect (for eye-dump comparisons). debug.rdrvr.lod.creature.hooks 0 at boot = nothing hooked. Report graphics.creatureLod.
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <time.h>
#endif

#if defined(_MSC_VER)&&!defined(__clang__)
#include <intrin.h>
#define RDRVR_CL_NOINLINE __declspec(noinline)
#define RDRVR_CL_CALLER() reinterpret_cast<std::uintptr_t>(_ReturnAddress())
#else
#define RDRVR_CL_NOINLINE __attribute__((noinline))
#define RDRVR_CL_CALLER() reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))
#endif

namespace rdrvr::engine::creaturelod {
// ---- librdr 1.58 arm64 (module-relative) ----
inline constexpr std::uintptr_t drawBigOffset=0x2a7993c,drawBigGotOffset=0x2e5fd50,drawBigReturnOffset=0x2a7988c;
inline constexpr std::uintptr_t drawNOffset=0x2a7b404,drawNGotOffset=0x2e5fd58,drawNReturnFirst=0x2a79bb4,drawNReturnLast=0x2a7a0ec;
inline constexpr std::uintptr_t postUpdateOffset=0x287d624,postUpdateGotOffset=0x2e5a588,postUpdateReturnOffset=0x287ea0c;
inline constexpr std::uintptr_t getCreatureOffset=0x290ef10,getCreatureGotOffset=0x2e478e8; // entEntityComponent::GetCreature
inline constexpr std::uintptr_t levelTableOffset=0x12fbbc8;                                   // .rodata {24,40},{20,42},{20,38}
// Data the engine LOD metric reads (GLOB_DAT GOT words = base + symbol, checked at install)
inline constexpr std::uintptr_t rendererOffset=0x4e85f110,rendererGotOffset=0x2e01660;        // sagRenderer::sm_Instance
inline constexpr std::uintptr_t tldMinOffset=0x2f3e7cc,tldMinGotOffset=0x2e0c2c0;             // s_TLD_SizeMinClamp (.data 0.2)
inline constexpr std::uintptr_t tldMaxOffset=0x2f3e7d0,tldMaxGotOffset=0x2e0c2a8;             // s_TLD_SizeMaxClamp (.data 3.0)
struct Print {std::uintptr_t begin,end;std::uint64_t fnv;const char* name;};
// FNV-1a 64 of the code ranges the levers depend on (computed from librdr.so 1.58 arm64; any mismatch -> nothing hooked).
inline constexpr std::array<Print,16> prints{{
    {0x2a7993c,0x2a79ae0,0xbf8cc23c0387a8b6ull,"drawBigHead"},        // prologue, distance, resident drawable, lodGroup compares
    {0x2a79b14,0x2a79c40,0x448b22074c941dccull,"drawBigLevels"},      // cloth + non-cloth ptl level paths (DrawN LOD 2 / streamed)
    {0x2a79d50,0x2a79d94,0xb408af0914ff75a1ull,"drawBigSkinnedNoLod"},// type+0x211 == 0: DrawN LOD -1
    {0x2a79e0c,0x2a79e68,0x689d66253cb1abb6ull,"drawBigStreamed"},    // level 0/1 streamed drawable
    {0x2a7a048,0x2a7a0a8,0x45c5a8a84b846c4dull,"drawBigClothMed"},    // cloth ped, level 1
    {0x2a79864,0x2a7988c,0x974b60bfaf644c91ull,"drawCaller"},         // Draw: x5 = request+0x120, bl DrawBig
    {0x2a7b404,0x2a7b534,0x5f4451cecc4b79e2ull,"drawNHead"},          // DrawN: LOD -1 selection, creature fetch
    {0x287d624,0x287d8c4,0x6049054140b18140ull,"postUpdate"},         // info+8 -> ptl+0x68, level decision, horse rule
    {0x287e9f8,0x287ea0c,0x9c1dae99b9e9b082ull,"postUpdateCaller"},   // ldr x0,[x22],#0x18; bl PostUpdate
    {0x290ef10,0x290ef18,0x818ead48a2b1c77cull,"getCreature"},
    {0x287efc8,0x287f04c,0x1e3e4486daaadbf2ull,"poiTask"},           // Renderer +0xa8 POIs / +0xb0 count / +0x2d sniper
    {0x287f0e4,0x287f278,0x9a5fc857c0a3fca4ull,"lodSetup"},          // task POI, sniper flags, s_CLD / s_TLD GOT loads
    {0x287f3dc,0x287f4c8,0x35d5a9cbf4a9a4dfull,"lodDistance"},       // node box centre, |c - poi|, sniper compression, node+0x6c
    {0x287f5d4,0x287f64c,0xc397151c76db12eaull,"lodSize"},           // clamp(size), 0.4 s + 0.6, min(d, d / s'')
    {0x287c73c,0x287c764,0x2d2b9cc38a84bc46ull,"horseInit"},         // propTextureLOD::Init: horse bit 5 = stub+0x5e >= 2
    {0x2a7a8c0,0x2a7a93c,0x507520409ade97a2ull,"updateAabb"}}};      // node+0x6c half-diagonal, +0x7c max half-extent
inline constexpr std::array<std::uintptr_t,4> drawNCallsOutsidePrints{0x2a79d44,0x2a79ff4,0x2a7a0e8,0x2a79bb0}; // bl DrawN
inline constexpr std::array<std::uint32_t,4> drawNCallWords{0x9408d42bu,0x9408d37fu,0x9408d342u,0x9408d490u};
// ---- engine layouts [V] ----
inline constexpr std::size_t nodeFragInst=0xc0,nodeBoxMin=0x60,nodeBoxMax=0x70,nodeRadius=0x7c,nodeSize=0x6c;
inline constexpr std::size_t rendererPoi=0xa8,rendererPoiCount=0xb0,poiStride=0x4a0,poiSniper=0x2d,stubHorseGroup=0x5e;
inline constexpr std::size_t instChain=0x18,instHealth=0x90,typeDrawable=0xc8,typeDrawables=0xd0,typeDamaged=0xe4,typeStub=0x208,
    typeStreamed=0x211,stubPtl=0x40,ptlLevel=0x0,ptlTarget=0x4,ptlStub=0x8,ptlFlags=0x76,hierIndex=0x80,infoDistance=0x8;
inline constexpr std::size_t drawableLod=0x50,drawableThreshold=0x70,requestFromCamera=0x120,requestType=0xcc;
inline constexpr int requestMain=13;
inline constexpr std::size_t typeCopyBytes=0x218,stubCopyBytes=0x60,ptlCopyBytes=0x78; // DrawBig reads type <= +0x211, stub +0x40
inline constexpr std::uint16_t ptlDrawableLod=0x4,ptlHorse=0x20;
// ---- policy (pure) ----
inline constexpr float authoredMed=24.f,authoredLow=40.f;      // table from level 0: medlod beyond 24, resident beyond 40
inline constexpr float residentEnter=42.f,residentLeave=38.f;  // table row 1 hi (enter) / row 2 hi (stay), metric units
inline constexpr std::array<float,3> pinDistance{0.f,30.f,1000.f}; // info+8 forcing level 0 / 1 / 2 (1000 > the horse 300 rule)
inline constexpr int pinMax=3;                                     // 3 = pin 2 including horses
enum class Kind : unsigned {Off,Scale,Metres};
struct Policy {Kind kind{Kind::Scale};float scale{1.f},d1{authoredMed},d2{authoredLow};};
enum Mask : unsigned {PerType=1,PerInstance=2,LodGroup=4,AllLevers=7};
// "" -> default (scale 1), "0" -> off, s < 8 -> scale, m >= 8 -> metres m : m*40/24, "a:b" -> metres. false = invalid text.
inline bool parsePolicy(const char* text,Policy& out) noexcept {
    // lodoff: unset = OFF; "1" = the authored distances (scale 1)
    out=Policy{};if(!text||!*text){out.kind=Kind::Off;return true;}
    char* end{};const float a=std::strtof(text,&end);if(end==text||!std::isfinite(a))return false;
    if(*end==':'){const char* s=end+1;const float b=std::strtof(s,&end);
        if(end==s||*end||!std::isfinite(b)||!(a>=1.f)||!(b>a)||b>1000.f)return false;out.kind=Kind::Metres;out.d1=a;out.d2=b;return true;}
    if(*end)return false;
    if(a==0.f){out.kind=Kind::Off;return true;}
    if(a>=0.25f&&a<8.f){out.kind=Kind::Scale;out.scale=a;return true;}
    if(a>=8.f&&a<=600.f){out.kind=Kind::Metres;out.d1=a;out.d2=a*authoredLow/authoredMed;return true;}
    return false;
}
// Distance d (policy units: metres, or dist/size for the per-type value) -> the authored scale where A starts LOD1 and B LOD2.
// Metres: d1 -> A, d2 -> B, piecewise linear, slope >= 1 beyond d2; monotonic and continuous.
inline float remap(float d,float a,float b,const Policy& p) noexcept {
    if(!std::isfinite(d))return d;
    switch(p.kind){
    case Kind::Off:return d;
    case Kind::Scale:return d*p.scale;
    case Kind::Metres:
        if(!(a>0.f)||!(b>a))return d*a/p.d1;
        if(d<p.d1)return d*a/p.d1;
        if(d<p.d2)return a+(d-p.d1)*(b-a)/(p.d2-p.d1);
        return b+(d-p.d2)*std::max(1.f,b/p.d2);
    }
    return d;
}
// DrawN's own LOD -1 selection [V @0x2a7b4b0..0x2a7b500]: thresholds compared with >=, a null LOD is skipped, 3 = beyond (culled).
inline int engineLevel(float dist,const float* thr,const bool* has) noexcept {
    if(!(thr[0]<dist)&&has[0])return 0;
    if(!(thr[1]<dist)&&has[1])return 1;
    return (has[2]&&!(thr[2]<dist))?2:3;
}
// Explicit LOD for a lodGroup creature (DrawN LOD -1), or -1 = leave native. Never more detail than native (except pin 0), never a
// null LOD, never the native cull (3). pin -1 = policy.
inline int lodGroupLevel(float dist,const float* thr,const bool* has,const Policy& p,int pin) noexcept {
    const int native=engineLevel(dist,thr,has);if(native>2)return -1;
    int want=native;
    if(pin>=0)want=std::min(pin,2);
    else{
        const bool pair=std::isfinite(thr[0])&&std::isfinite(thr[1])&&thr[0]>0.f&&thr[1]>thr[0]&&thr[1]<1.0e4f;
        const float a=pair?thr[0]:authoredMed,b=pair?thr[1]:authoredLow;
        want=std::min(engineLevel(remap(dist,a,b,p),thr,has),2);
        if(want<native)return -1;
    }
    if(pin<0&&want==native)return -1;
    int level=want;
    while(level>0&&!has[level])--level;
    if(!has[level])return -1;
    if(pin<0&&level<=native)return -1;
    return level==native?-1:level;
}
// The engine's LOD metric of ONE instance (preTextureLodUpdateSpu, see the header): size factor s'' and
// min over the POIs of min(d, d / s''). Non-finite -> NaN (callers then stay native).
inline float sizeFactor(float size,float lo,float hi) noexcept {
    if(!std::isfinite(size)||!std::isfinite(lo)||!std::isfinite(hi)||!(lo>0.f)||!(hi>=lo))return std::nanf("");
    const float s=std::min(std::max(size,lo),hi);return s-1.f>=0.f?s:0.4f*s+0.6f;
}
inline float lodMetric(const float* centre,const float (*poi)[3],unsigned count,float factor) noexcept {
    if(!count||!(factor>0.f))return std::nanf("");
    float best=INFINITY;
    for(unsigned i=0;i<count&&i<2;++i){float d2=0.f;for(unsigned k=0;k<3;++k){const float v=centre[k]-poi[i][k];d2+=v*v;}
        const float d=std::sqrt(d2);best=std::min(best,std::min(d,d/factor));}
    return std::isfinite(best)?best:std::nanf("");
}
// Protect test on the SURFACE distance from the head: inside the bound (or a NaN bound) counts as 0 = protected.
inline float surfaceDistance(float centreDistance,float size,float radius) noexcept {
    float r=radius;if(size>r||!(r==r))r=size;
    const float s=centreDistance-r;return s>0.f?s:0.f;
}
// Per-instance resident decision with hysteresis (state: 0 unknown, 1 native, 2 resident); metric = lodMetric (engine units).
inline unsigned residentState(float metric,unsigned previous,const Policy& p) noexcept {
    const float m=remap(std::max(metric,0.f),authoredMed,authoredLow,p);
    return m>(previous==2?residentLeave:residentEnter)?2u:1u;
}
// Pin -> info+8 for the per-type lever: pin 2 keeps horses at the engine's medlod floor, pin 3 includes them.
inline float pinTypeDistance(int pin,bool horse) noexcept {
    const unsigned level=pin>=3?2u:(pin==2&&horse)?1u:unsigned(std::max(pin,0));return pinDistance[std::min(level,2u)];
}
// The drawable DrawBig would draw with LOD 2 [V @0x2a799d0..0x2a79a78]: damaged variant when fragInst+0x90 is not >= 0, then the
// hierarchy's drawable index. Direct loads (DrawBig dereferences the same fields unguarded in the same call).
template<class T> inline T ld(std::uintptr_t a) noexcept {T v;std::memcpy(&v,reinterpret_cast<const void*>(a),sizeof v);return v;}
template<class T> inline void st(unsigned char* p,T v) noexcept {std::memcpy(p,&v,sizeof v);}
inline std::uintptr_t residentDrawable(std::uintptr_t type,std::uintptr_t node,std::uintptr_t hier) noexcept {
    const auto fi=ld<std::uintptr_t>(node+nodeFragInst);std::uintptr_t drawable{};
    if(fi&&!(ld<float>(fi+instHealth)>=0.f)){
        const int idx=ld<std::int32_t>(type+typeDamaged);const auto arr=ld<std::uintptr_t>(type+typeDrawables);
        if(idx!=-1&&arr)drawable=ld<std::uintptr_t>(arr+std::uintptr_t(std::int64_t(idx))*8);
    }
    if(!drawable)drawable=ld<std::uintptr_t>(type+typeDrawable);
    if(hier){const int h=ld<std::int32_t>(hier+hierIndex);
        if(h>=0){const auto arr=ld<std::uintptr_t>(type+typeDrawables);drawable=arr?ld<std::uintptr_t>(arr+std::uintptr_t(h)*8):0;}}
    return drawable;
}
// Stack copies presenting level 2 to DrawBig (type -> stub -> ptl).
struct Fake {alignas(16) unsigned char type[typeCopyBytes];alignas(16) unsigned char stub[stubCopyBytes];alignas(16) unsigned char ptl[ptlCopyBytes];};
inline const void* buildFake(Fake& f,std::uintptr_t type,std::uintptr_t stub,std::uintptr_t ptl) noexcept {
    std::memcpy(f.type,reinterpret_cast<const void*>(type),typeCopyBytes);std::memcpy(f.stub,reinterpret_cast<const void*>(stub),stubCopyBytes);
    std::memcpy(f.ptl,reinterpret_cast<const void*>(ptl),ptlCopyBytes);
    st<std::int32_t>(f.ptl+ptlLevel,2);st<std::int32_t>(f.ptl+ptlTarget,2);
    st<std::uintptr_t>(f.stub+stubPtl,reinterpret_cast<std::uintptr_t>(f.ptl));st<std::uintptr_t>(f.type+typeStub,reinterpret_cast<std::uintptr_t>(f.stub));
    return f.type;
}
// ---- small lock-free tables (keyed by engine pointers; stale after maxAgeMs) ----
template<unsigned Size> class Table {
public:
    struct Slot {std::atomic<std::uintptr_t> key{},aux{};std::atomic<std::uint32_t> stamp{};std::atomic<std::uint8_t> state{},level{},horse{};};
    Slot* find(std::uintptr_t key,std::uint32_t now,std::uint32_t maxAgeMs,bool claim) noexcept {
        if(!key)return nullptr;
        const unsigned h=unsigned((std::uint64_t(key)*0x9e3779b97f4a7c15ull)>>40)%Size;
        Slot* free{};
        for(unsigned i=0;i<8;++i){auto& s=slots_[(h+i)%Size];const auto k=s.key.load(std::memory_order_acquire);
            if(k==key)return &s;
            if(!free&&(!k||now-s.stamp.load(std::memory_order_relaxed)>maxAgeMs))free=&s;}
        if(!claim||!free)return nullptr;
        auto k=free->key.load(std::memory_order_relaxed);
        if(!free->key.compare_exchange_strong(k,key,std::memory_order_acq_rel))return nullptr;
        free->state.store(0,std::memory_order_relaxed);free->aux.store(0,std::memory_order_relaxed);free->stamp.store(now,std::memory_order_release);
        return free;
    }
    template<class F> void each(F f) const noexcept {for(const auto& s:slots_)if(s.key.load(std::memory_order_acquire))f(s);}
    void clear() noexcept {for(auto& s:slots_){s.key.store(0,std::memory_order_relaxed);s.state.store(0,std::memory_order_relaxed);}} // host test
private:
    std::array<Slot,Size> slots_{};
};
// ---- runtime state ----
using DrawBigFn=void(*)(void*,void*,const void*,const void*,const void*,const float*,const void*,void*,float);
using DrawNFn=void(*)(void*,void*,const void*,const void*,const void*,const void*,void*,float,bool,int,int);
using PostUpdateFn=void(*)(void*,void*,const void*,int,bool,bool);
using GetCreatureFn=std::uintptr_t(*)(std::uintptr_t);
inline std::uintptr_t engine{};inline DrawBigFn originalDrawBig{};inline DrawNFn originalDrawN{};inline PostUpdateFn originalPostUpdate{};
inline GetCreatureFn getCreature{};inline std::uintptr_t(*playerSkeleton)(){};inline std::uintptr_t(*playerCreature)(){};
inline std::uintptr_t rendererVar{},tldMinVar{},tldMaxVar{}; // &sagRenderer::sm_Instance, &s_TLD_SizeMin/MaxClamp
// POIs as the engine's texture-LOD task saw them (sampled in the PostUpdate hook = PerformDMZ), read on the visibility threads.
struct PoiCache {std::atomic<std::uint32_t> pos[2][3]{},count{},sniper{},ms{};std::atomic<std::uint64_t> samples{};};
inline PoiCache poiCache{};
inline constexpr std::uint32_t poiMaxAgeMs=1000;
inline std::atomic<bool> hooked{};inline std::atomic<const char*> installState{"NOT_ATTEMPTED"};
struct Live {std::atomic<unsigned> kind{unsigned(Kind::Scale)},mask{AllLevers};std::atomic<std::uint32_t> scale{},d1{},d2{},protect{};std::atomic<int> pin{-1};};
inline Live live{};
inline std::uint32_t bitsOf(float v) noexcept {std::uint32_t u;std::memcpy(&u,&v,4);return u;}
inline float floatOf(std::uint32_t u) noexcept {float v;std::memcpy(&v,&u,4);return v;}
inline Policy policy() noexcept {
    Policy p{};p.kind=Kind(live.kind.load(std::memory_order_relaxed));p.scale=floatOf(live.scale.load(std::memory_order_relaxed));
    p.d1=floatOf(live.d1.load(std::memory_order_relaxed));p.d2=floatOf(live.d2.load(std::memory_order_relaxed));return p;}
struct PropertyText {char policy[96]{},pin[16]{},protect[16]{},mask[16]{};};
inline PropertyText propertyText{};inline std::atomic<std::uint32_t> propertyTextSeq{}; // single writer (observeScene), report() copies
inline std::atomic<std::uint64_t> propertyReads{},policyRejected{};
inline std::atomic<std::uint32_t> mainCam[3]{};inline std::atomic<std::uint32_t> mainCamMs{};
inline Table<1024> instances{};inline Table<256> types{};
inline std::uint32_t nowMs() noexcept {
#if defined(__ANDROID__)
    timespec t{};clock_gettime(CLOCK_MONOTONIC_COARSE,&t);return std::uint32_t(std::uint64_t(t.tv_sec)*1000u+std::uint64_t(t.tv_nsec)/1000000u);
#else
    return std::uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
inline constexpr std::array<float,5> distEdges{6.f,14.f,24.f,40.f,80.f}; // engine LOD metric (metres for a human); 6 buckets
inline unsigned distBucket(float m) noexcept {unsigned b=0;while(b<distEdges.size()&&!(m<distEdges[b]))++b;return b;}
using Counter=std::atomic<std::uint64_t>;
struct Stats {
    Counter drawBig,foreignDrawBig,creatureMain,creatureOther,streamedType,noPtl,lodGroupType,horse,protectedCount,playerSkip;
    Counter forcedMain,forcedOther,forcedPin,alreadyResident,noResidentLod,notSkinned,wantMedKeepHi,heldByHysteresis,tableFull,camFallback;
    Counter drawN,drawNExplicit[3],drawNKept,postUpdate,postUpdateForeign,postTypeCreature,postRemapped,postPinned,postProtected,postStale;
    Counter scenes;
    Counter poiStale,sniperNative,metricInvalid,horseExcluded,playerCreatureSkip,poiSamples;
    std::array<std::array<Counter,4>,6> mainByDist{};   // engine-metric bucket x engine level 0/1/2 (+0x211 types) / 3 = lodGroup type
    std::array<Counter,6> forcedByDist{};
};
inline Stats stats{};
inline void bump(Counter& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
struct Ctx {std::uintptr_t node{};Policy policy{};int pin{-1};bool off{};};
inline thread_local const Ctx* tlsCtx{};

// ---- POI sample (PostUpdate hook, PerformDMZ) -> visibility threads ----
inline void samplePoi(std::uint32_t now) noexcept {
    unsigned count=0;bool sniper=false;
    const auto inst=rendererVar?ld<std::uintptr_t>(rendererVar):0;const auto pois=inst?ld<std::uintptr_t>(inst+rendererPoi):0;
    if(pois)count=std::min<unsigned>(ld<std::uint16_t>(inst+rendererPoiCount),2u); // the task clamps to 2 (@0x287efe4)
    for(unsigned i=0;i<count;++i){const auto q=pois+std::uintptr_t(i)*poiStride;
        for(unsigned k=0;k<3;++k)poiCache.pos[i][k].store(bitsOf(ld<float>(q+4*k)),std::memory_order_relaxed);
        if(ld<std::uint8_t>(q+poiSniper))sniper=true;}
    poiCache.count.store(count,std::memory_order_relaxed);poiCache.sniper.store(sniper?1u:0u,std::memory_order_relaxed);
    poiCache.ms.store(now,std::memory_order_relaxed);poiCache.samples.fetch_add(1,std::memory_order_release);
}
inline bool loadPoi(float (&pos)[2][3],unsigned& count,bool& sniper,std::uint32_t now) noexcept {
    if(!poiCache.samples.load(std::memory_order_acquire)||now-poiCache.ms.load(std::memory_order_relaxed)>poiMaxAgeMs)return false;
    count=std::min(poiCache.count.load(std::memory_order_relaxed),2u);sniper=poiCache.sniper.load(std::memory_order_relaxed)!=0;
    for(unsigned i=0;i<count;++i)for(unsigned k=0;k<3;++k)pos[i][k]=floatOf(poiCache.pos[i][k].load(std::memory_order_relaxed));
    return count>0;
}

// ---- hooks (GOT; exact callers proven by return address) ----
inline void drawNCore(std::uintptr_t caller,void* node,void* fi,const void* drawable,const void* m1,const void* m2,const void* skel,
    void* mset,float dist,bool flag,int lod,int extra) {
    const Ctx* c=tlsCtx;
    if(c&&lod==-1&&drawable&&reinterpret_cast<std::uintptr_t>(node)==c->node){
        const auto ret=caller-engine;
        if(ret>=drawNReturnFirst&&ret<=drawNReturnLast){
            const auto d=reinterpret_cast<std::uintptr_t>(drawable);bump(stats.drawN);
            float thr[3];bool has[3];for(unsigned i=0;i<3;++i){thr[i]=ld<float>(d+drawableThreshold+4*i);has[i]=ld<std::uintptr_t>(d+drawableLod+8*i)!=0;}
            const int level=c->off?-1:lodGroupLevel(dist,thr,has,c->policy,c->pin);
            if(level>=0){bump(stats.drawNExplicit[unsigned(level)]);originalDrawN(node,fi,drawable,m1,m2,skel,mset,dist,flag,level,extra);return;}
            bump(stats.drawNKept);
        }
    }
    originalDrawN(node,fi,drawable,m1,m2,skel,mset,dist,flag,lod,extra);
}
RDRVR_CL_NOINLINE inline void drawNHook(void* node,void* fi,const void* drawable,const void* m1,const void* m2,const void* skel,
    void* mset,float dist,bool flag,int lod,int extra) {
    drawNCore(RDRVR_CL_CALLER(),node,fi,drawable,m1,m2,skel,mset,dist,flag,lod,extra);
}
struct CtxScope {const Ctx* saved;explicit CtxScope(const Ctx* c) noexcept:saved(tlsCtx){tlsCtx=c;} ~CtxScope(){tlsCtx=saved;}
    CtxScope(const CtxScope&)=delete;CtxScope& operator=(const CtxScope&)=delete;};
inline void drawBigCore(std::uintptr_t caller,void* node,void* dn,const void* type,const void* m1,const void* m2,const float* cam,
    const void* skel,void* hier,float f) {
    const Kind kind=Kind(live.kind.load(std::memory_order_relaxed));
    if(kind==Kind::Off||!node||!type||!cam){originalDrawBig(node,dn,type,m1,m2,cam,skel,hier,f);return;}
    {thread_local unsigned calls{};if(!(++calls&63u))bump(stats.drawBig,64);} // every DrawBig (sampled: no shared atomic per call)
    if(caller!=engine+drawBigReturnOffset){bump(stats.foreignDrawBig);originalDrawBig(node,dn,type,m1,m2,cam,skel,hier,f);return;}
    const int savedErrno=errno;
    const auto n=reinterpret_cast<std::uintptr_t>(node),t=reinterpret_cast<std::uintptr_t>(type);
    // crCreature exactly as DrawN fetches it (@0x2a7b504..0x2a7b530)
    std::uintptr_t creature{};
    if(const auto fi=ld<std::uintptr_t>(n+nodeFragInst)){const auto c=ld<std::uintptr_t>(fi+instChain);
        if(c&&!ld<std::uint8_t>(c+0x18))if(const auto e=ld<std::uintptr_t>(c+0x8))if(const auto comp=ld<std::uintptr_t>(e+0x50))creature=getCreature(comp);}
    if(!creature){errno=savedErrno;originalDrawBig(node,dn,type,m1,m2,cam,skel,hier,f);return;}
    const auto request=reinterpret_cast<std::uintptr_t>(cam)-requestFromCamera;
    const bool main=ld<std::int32_t>(request+requestType)==requestMain;
    bump(main?stats.creatureMain:stats.creatureOther);
    const std::uint32_t now=nowMs();
    // protect + census from the HEAD (MAIN camera); another request without a fresh MAIN camera stays native.
    float head[3]{cam[0],cam[1],cam[2]};
    if(main){for(unsigned i=0;i<3;++i)mainCam[i].store(bitsOf(cam[i]),std::memory_order_relaxed);mainCamMs.store(now,std::memory_order_release);}
    else if(now-mainCamMs.load(std::memory_order_acquire)<2000u){for(unsigned i=0;i<3;++i)head[i]=floatOf(mainCam[i].load(std::memory_order_relaxed));}
    else{bump(stats.camFallback);errno=savedErrno;originalDrawBig(node,dn,type,m1,m2,cam,skel,hier,f);return;}
    float centre[3],d2=0.f;
    for(unsigned i=0;i<3;++i){centre[i]=0.5f*(ld<float>(n+nodeBoxMin+4*i)+ld<float>(n+nodeBoxMax+4*i));const float v=centre[i]-head[i];d2+=v*v;}
    const float headDist=std::sqrt(std::max(d2,0.f)),size=ld<float>(n+nodeSize),surface=surfaceDistance(headDist,size,ld<float>(n+nodeRadius));
    const float protect=floatOf(live.protect.load(std::memory_order_relaxed));
    const auto playerSkel=playerSkeleton?playerSkeleton():0;const auto playerCr=playerCreature?playerCreature():0;
    const bool skelMatch=playerSkel&&reinterpret_cast<std::uintptr_t>(skel)==playerSkel,crMatch=playerCr&&creature==playerCr;
    const bool isPlayer=skelMatch||crMatch,guarded=isPlayer||!(surface>=protect);
    if(isPlayer){bump(stats.playerSkip);if(!skelMatch)bump(stats.playerCreatureSkip);}else if(guarded)bump(stats.protectedCount);
    const Policy p=policy();const int pin=live.pin.load(std::memory_order_relaxed);const unsigned mask=live.mask.load(std::memory_order_relaxed);
    // engine LOD metric of THIS instance from the POIs the texture-LOD task used; the census falls back to the head
    const float factor=sizeFactor(size,tldMinVar?ld<float>(tldMinVar):std::nanf(""),tldMaxVar?ld<float>(tldMaxVar):std::nanf(""));
    float poi[2][3]{};unsigned poiCount=0;bool sniper=false;
    const bool poiFresh=loadPoi(poi,poiCount,sniper,now);
    const float metric=poiFresh?lodMetric(centre,poi,poiCount,factor):std::nanf("");
    float census=metric;
    if(!std::isfinite(census)){const float h[1][3]{{head[0],head[1],head[2]}};census=lodMetric(centre,h,1,factor);if(!std::isfinite(census))census=headDist;}
    const unsigned bucket=distBucket(census);
    const bool streamed=ld<std::uint8_t>(t+typeStreamed)!=0;
    const auto stub=ld<std::uintptr_t>(t+typeStub);const auto ptl=streamed&&stub?ld<std::uintptr_t>(stub+stubPtl):0;
    const bool stubHorse=stub&&ld<std::uint8_t>(stub+stubHorseGroup)>=2; // propTextureLOD::Init @0x287c74c
    if(!ptl){
        if(streamed){bump(stats.noPtl);errno=savedErrno;originalDrawBig(node,dn,type,m1,m2,cam,skel,hier,f);return;} // native resident LOD 2
        bump(stats.lodGroupType);if(main)bump(stats.mainByDist[bucket][3]);
        const bool horseOut=pin==2&&stubHorse;if(horseOut&&!guarded)bump(stats.horseExcluded);
        const Ctx ctx{n,p,pin,guarded||horseOut||!((mask&LodGroup)||pin>=0)};CtxScope scope(&ctx);
        errno=savedErrno;originalDrawBig(node,dn,type,m1,m2,cam,skel,hier,f);return;
    }
    bump(stats.streamedType);
    const int level=ld<std::int32_t>(ptl+ptlLevel),target=ld<std::int32_t>(ptl+ptlTarget);
    const auto flags=ld<std::uint16_t>(ptl+ptlFlags);const bool horse=(flags&ptlHorse)!=0||stubHorse;if(horse)bump(stats.horse);
    if(main&&level>=0&&level<=2)bump(stats.mainByDist[bucket][unsigned(level)]);
    if(auto* s=types.find(ptl,now,10000u,true)){s->aux.store(stub,std::memory_order_relaxed);s->level.store(std::uint8_t(level&0xff),std::memory_order_relaxed);
        s->horse.store(horse?1:0,std::memory_order_relaxed);s->stamp.store(now,std::memory_order_release);}
    else bump(stats.tableFull);
    bool resident=false;
    if(pin>=2){resident=!guarded&&(pin>=3||!horse);if(!guarded&&!resident)bump(stats.horseExcluded);}
    else if(pin<0&&(mask&PerInstance)&&!guarded&&!horse){
        if(!poiFresh)bump(stats.poiStale);
        else if(sniper)bump(stats.sniperNative);           // zoom: the engine compresses distances inside the scope cone -> native
        else if(!std::isfinite(metric))bump(stats.metricInvalid);
        else{
            auto* s=instances.find(n,now,2000u,main);
            if(s&&s->aux.load(std::memory_order_relaxed)!=t&&s->state.load(std::memory_order_relaxed))s->state.store(0,std::memory_order_relaxed); // reused node
            const unsigned previous=s?s->state.load(std::memory_order_relaxed):0u;
            unsigned state=previous;
            const float mapped=remap(std::max(metric,0.f),authoredMed,authoredLow,p);
            if(main||!previous){state=residentState(metric,previous,p);
                if(main&&s){s->aux.store(t,std::memory_order_relaxed);s->state.store(std::uint8_t(state),std::memory_order_relaxed);s->stamp.store(now,std::memory_order_release);}
                if(main&&previous==2&&state==2&&!(mapped>residentEnter))bump(stats.heldByHysteresis);}
            if(main&&!s)bump(stats.tableFull);
            resident=state==2;
            if(!resident&&main&&level==0&&mapped>authoredMed)bump(stats.wantMedKeepHi);
        }
    }
    if(resident){
        const auto mainDrawable=ld<std::uintptr_t>(t+typeDrawable); // fragType::GetSkinned @0x1ef6acc: the +0x211 level paths are skinned only
        if(level==2&&target==2)bump(stats.alreadyResident);
        else if(!mainDrawable||!(ld<float>(mainDrawable+0x90)>0.f))bump(stats.notSkinned);
        else{
            const auto drawable=residentDrawable(t,n,reinterpret_cast<std::uintptr_t>(hier));
            if(drawable&&ld<std::uintptr_t>(drawable+drawableLod+16)){
                bump(pin>=2?stats.forcedPin:main?stats.forcedMain:stats.forcedOther);if(main)bump(stats.forcedByDist[bucket]);
                Fake fake;const void* faked=buildFake(fake,t,stub,ptl);
                errno=savedErrno;originalDrawBig(node,dn,faked,m1,m2,cam,skel,hier,f);return;
            }
            bump(stats.noResidentLod);
        }
    }
    errno=savedErrno;originalDrawBig(node,dn,type,m1,m2,cam,skel,hier,f);
}
RDRVR_CL_NOINLINE inline void drawBigHook(void* node,void* dn,const void* type,const void* m1,const void* m2,const float* cam,
    const void* skel,void* hier,float f) {
    drawBigCore(RDRVR_CL_CALLER(),node,dn,type,m1,m2,cam,skel,hier,f);
}
inline void postUpdateCore(std::uintptr_t caller,void* ptl,void* info,const void* poi,int count,bool a,bool b) {
    const Kind kind=Kind(live.kind.load(std::memory_order_relaxed));const int pin=live.pin.load(std::memory_order_relaxed);
    const unsigned mask=live.mask.load(std::memory_order_relaxed);
    if(kind==Kind::Off||!ptl||!info||b){originalPostUpdate(ptl,info,poi,count,a,b);return;}
    if(caller==engine+postUpdateReturnOffset){thread_local std::uint32_t last{~0u};const std::uint32_t t=nowMs();
        if(t!=last){last=t;samplePoi(t);bump(stats.poiSamples);}}
    if(!((mask&PerType)||pin>=0)){originalPostUpdate(ptl,info,poi,count,a,b);return;}
    bump(stats.postUpdate);
    if(caller!=engine+postUpdateReturnOffset){bump(stats.postUpdateForeign);originalPostUpdate(ptl,info,poi,count,a,b);return;}
    const auto key=reinterpret_cast<std::uintptr_t>(ptl);const std::uint32_t now=nowMs();
    auto* s=types.find(key,now,10000u,false);
    const auto flags=ld<std::uint16_t>(key+ptlFlags);
    if(!s||now-s->stamp.load(std::memory_order_acquire)>10000u||!(flags&ptlDrawableLod)){originalPostUpdate(ptl,info,poi,count,a,b);return;}
    if(ld<std::uintptr_t>(key+ptlStub)!=s->aux.load(std::memory_order_relaxed)){bump(stats.postStale);originalPostUpdate(ptl,info,poi,count,a,b);return;}
    bump(stats.postTypeCreature);
    auto* slot=static_cast<unsigned char*>(info)+infoDistance;float d;std::memcpy(&d,slot,4);
    float mapped=d;
    if(pin>=0){if(d<floatOf(live.protect.load(std::memory_order_relaxed)))bump(stats.postProtected);else{mapped=pinTypeDistance(pin,(flags&ptlHorse)||s->horse.load(std::memory_order_relaxed));bump(stats.postPinned);}}
    else mapped=remap(d,authoredMed,authoredLow,policy());
    if(!(mapped!=d)||!std::isfinite(mapped)){originalPostUpdate(ptl,info,poi,count,a,b);return;}
    if(pin<0)bump(stats.postRemapped);
    std::memcpy(slot,&mapped,4);originalPostUpdate(ptl,info,poi,count,a,b);std::memcpy(slot,&d,4);
}
RDRVR_CL_NOINLINE inline void postUpdateHook(void* ptl,void* info,const void* poi,int count,bool a,bool b) {
    postUpdateCore(RDRVR_CL_CALLER(),ptl,info,poi,count,a,b);
}

// ---- properties (observeScene; at most once per second) ----
inline bool property(const char* name,char* out,std::size_t size) noexcept {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX]{};if(__system_property_get(name,v)<=0){out[0]=0;return false;}
    std::snprintf(out,size,"%s",v);return true;
#else
    (void)name;(void)size;out[0]=0;return false;
#endif
}
inline void readProperties() noexcept {
    PropertyText t{};property("debug.rdrvr.lod.creature",t.policy,sizeof t.policy);property("debug.rdrvr.lod.creature.pin",t.pin,sizeof t.pin);
    property("debug.rdrvr.lod.creature.protect",t.protect,sizeof t.protect);property("debug.rdrvr.lod.creature.mask",t.mask,sizeof t.mask);
    Policy p{};if(!parsePolicy(t.policy,p)){bump(policyRejected);p=Policy{};}
    int pin=-1;if(t.pin[0]){char* e{};const long v=std::strtol(t.pin,&e,10);if(e!=t.pin&&!*e&&v>=0&&v<=pinMax)pin=int(v);}
    float protect=2.5f;if(t.protect[0]){char* e{};const float v=std::strtof(t.protect,&e);if(e!=t.protect&&!*e&&v>=0.f&&v<=20.f)protect=v;}
    unsigned mask=AllLevers;if(t.mask[0]){char* e{};const long v=std::strtol(t.mask,&e,0);if(e!=t.mask&&!*e&&v>=0&&v<=7)mask=unsigned(v);}
    live.scale.store(bitsOf(p.scale),std::memory_order_relaxed);live.d1.store(bitsOf(p.d1),std::memory_order_relaxed);live.d2.store(bitsOf(p.d2),std::memory_order_relaxed);
    live.protect.store(bitsOf(protect),std::memory_order_relaxed);live.pin.store(pin,std::memory_order_relaxed);live.mask.store(mask,std::memory_order_relaxed);
    live.kind.store(unsigned(p.kind),std::memory_order_release);
    const auto seq=propertyTextSeq.load(std::memory_order_relaxed);propertyTextSeq.store(seq+1,std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);propertyText=t;propertyTextSeq.store(seq+2,std::memory_order_release);bump(propertyReads);
}
inline std::atomic<std::uint32_t> lastReadMs{};inline std::atomic<bool> everRead{};
// perScene window (render thread): every 64 scenes the counter deltas / scenes of the chosen counters.
struct Window {std::uint64_t scenes{},creatureMain{},creatureOther{},forcedMain{},forcedOther{},forcedPin{},postRemapped{},postPinned{},drawNExplicit{};
    std::array<std::array<std::uint64_t,4>,6> byDist{};};
inline Window windowBase{};inline std::atomic<std::uint64_t> windowSeq{};
inline std::array<std::atomic<std::uint32_t>,8+24> windowPerScene{}; // floats: see report()
inline Window snapshot() noexcept {
    Window w{};const auto& s=stats;w.scenes=s.scenes.load();w.creatureMain=s.creatureMain.load();w.creatureOther=s.creatureOther.load();
    w.forcedMain=s.forcedMain.load();w.forcedOther=s.forcedOther.load();w.forcedPin=s.forcedPin.load();w.postRemapped=s.postRemapped.load();
    w.postPinned=s.postPinned.load();w.drawNExplicit=s.drawNExplicit[0].load()+s.drawNExplicit[1].load()+s.drawNExplicit[2].load();
    for(unsigned b=0;b<6;++b)for(unsigned l=0;l<4;++l)w.byDist[b][l]=s.mainByDist[b][l].load();
    return w;
}
inline void sceneBegin() noexcept {
    const auto scene=stats.scenes.fetch_add(1,std::memory_order_relaxed)+1;
    const std::uint32_t now=nowMs();
    if(!everRead.load(std::memory_order_relaxed)||now-lastReadMs.load(std::memory_order_relaxed)>=1000u){
        const int saved=errno;readProperties();errno=saved;lastReadMs.store(now,std::memory_order_relaxed);everRead.store(true,std::memory_order_relaxed);}
    if(scene%64)return;
    const Window w=snapshot(),b=windowBase;const double n=double(w.scenes-b.scenes);if(n<=0)return;
    const auto put=[&](unsigned i,double v){windowPerScene[i].store(bitsOf(float(v)),std::memory_order_relaxed);};
    put(0,n);put(1,double(w.creatureMain-b.creatureMain)/n);put(2,double(w.creatureOther-b.creatureOther)/n);
    put(3,double(w.forcedMain-b.forcedMain)/n);put(4,double(w.forcedOther-b.forcedOther)/n);put(5,double(w.forcedPin-b.forcedPin)/n);
    put(6,double((w.postRemapped-b.postRemapped)+(w.postPinned-b.postPinned))/n);put(7,double(w.drawNExplicit-b.drawNExplicit)/n);
    for(unsigned d=0;d<6;++d)for(unsigned l=0;l<4;++l)put(8+d*4+l,double(w.byDist[d][l]-b.byDist[d][l])/n);
    windowBase=w;windowSeq.fetch_add(1,std::memory_order_release);
}

// ---- install: prints of every range + call word + GOT word, then DrawN, DrawBig, PostUpdate all-or-nothing ----
struct Api {std::uintptr_t base{};bool(*read)(std::uintptr_t,void*,std::size_t){};bool(*exec)(std::uintptr_t,std::size_t){};
    bool(*replace)(std::uintptr_t,std::uintptr_t,std::uintptr_t){};std::uintptr_t(*playerSkeleton)(){};
    std::uintptr_t(*playerCreature)(){};}; // The player's crCreature, 0 = unknown
inline std::uint64_t fnv1a(const unsigned char* p,std::size_t n) noexcept {std::uint64_t h=0xcbf29ce484222325ull;for(std::size_t i=0;i<n;++i)h=(h^p[i])*0x100000001b3ull;return h;}
// Returns 1 hooked, 0 not hooked (state says why), -1 a rollback failed (caller raises patchFault).
inline int install(const Api& api) noexcept {
    if(hooked.load())return 1;
    char hooksProp[16]{};property("debug.rdrvr.lod.creature.hooks",hooksProp,sizeof hooksProp);
    if(hooksProp[0]=='0'&&!hooksProp[1]){installState.store("DISABLED_BY_PROPERTY");return 0;}
    if(!api.base||!api.read||!api.exec||!api.replace){installState.store("NO_API");return 0;}
    for(const auto& pr:prints){const std::size_t n=pr.end-pr.begin;unsigned char buf[0x300];
        if(n>sizeof buf||!api.exec(api.base+pr.begin,n)||!api.read(api.base+pr.begin,buf,n)||fnv1a(buf,n)!=pr.fnv){installState.store(pr.name);return 0;}}
    for(unsigned i=0;i<drawNCallsOutsidePrints.size();++i){std::uint32_t w{};
        if(!api.read(api.base+drawNCallsOutsidePrints[i],&w,4)||w!=drawNCallWords[i]){installState.store("drawNCallWord");return 0;}}
    float table[6]{};if(!api.read(api.base+levelTableOffset,table,sizeof table)||table[0]!=24.f||table[1]!=40.f||table[2]!=20.f||table[3]!=42.f||
        table[4]!=20.f||table[5]!=38.f){installState.store("levelTable");return 0;}
    struct Slot{std::uintptr_t got,fn;};const Slot slots[]{{drawNGotOffset,drawNOffset},{drawBigGotOffset,drawBigOffset},{postUpdateGotOffset,postUpdateOffset},
        {getCreatureGotOffset,getCreatureOffset}};
    for(const auto& s:slots){std::uintptr_t v{};if(!api.read(api.base+s.got,&v,sizeof v)||v!=api.base+s.fn){installState.store("gotWord");return 0;}}
    const Slot data[]{{rendererGotOffset,rendererOffset},{tldMinGotOffset,tldMinOffset},{tldMaxGotOffset,tldMaxOffset}};
    for(const auto& s:data){std::uintptr_t v{};if(!api.read(api.base+s.got,&v,sizeof v)||v!=api.base+s.fn){installState.store("dataGotWord");return 0;}}
    engine=api.base;playerSkeleton=api.playerSkeleton;playerCreature=api.playerCreature;
    rendererVar=api.base+rendererOffset;tldMinVar=api.base+tldMinOffset;tldMaxVar=api.base+tldMaxOffset;
    originalDrawBig=reinterpret_cast<DrawBigFn>(api.base+drawBigOffset);originalDrawN=reinterpret_cast<DrawNFn>(api.base+drawNOffset);
    originalPostUpdate=reinterpret_cast<PostUpdateFn>(api.base+postUpdateOffset);getCreature=reinterpret_cast<GetCreatureFn>(api.base+getCreatureOffset);
    readProperties();
    struct Route{std::uintptr_t got,fn,hook;};
    const Route routes[]{{drawNGotOffset,drawNOffset,reinterpret_cast<std::uintptr_t>(&drawNHook)},
        {drawBigGotOffset,drawBigOffset,reinterpret_cast<std::uintptr_t>(&drawBigHook)},
        {postUpdateGotOffset,postUpdateOffset,reinterpret_cast<std::uintptr_t>(&postUpdateHook)}};
    unsigned count{};for(const auto& r:routes){if(!api.replace(api.base+r.got,api.base+r.fn,r.hook))break;++count;}
    if(count==std::size(routes)){hooked.store(true);installState.store("HOOKED");return 1;}
    bool fault=false;while(count){const auto& r=routes[--count];if(!api.replace(api.base+r.got,r.hook,api.base+r.fn))fault=true;}
    installState.store(fault?"ROLLBACK_FAULT":"GOT_SWAP_FAILED");return fault?-1:0;
}

inline std::string report() {
    const auto& s=stats;std::string o;o.reserve(2048);char b[256];
    const auto u=[&](const char* k,const Counter& c){std::snprintf(b,sizeof b,",\"%s\":%llu",k,(unsigned long long)c.load(std::memory_order_relaxed));o+=b;};
    const Policy p=policy();static constexpr const char* kinds[3]{"OFF","SCALE","METRES"};
    PropertyText pt{};for(int i=0;i<4;++i){const auto a=propertyTextSeq.load(std::memory_order_acquire);if(a&1u)continue;
        pt=propertyText;std::atomic_thread_fence(std::memory_order_acquire);if(propertyTextSeq.load(std::memory_order_relaxed)==a)break;pt=PropertyText{};}
    std::snprintf(b,sizeof b,"\"creatureLod\":{\"hooked\":%d,\"state\":\"%s\",\"policy\":{\"kind\":\"%s\",\"scale\":%.3f,\"d1\":%.2f,\"d2\":%.2f}",
        hooked.load()?1:0,installState.load(),kinds[unsigned(p.kind)%3],double(p.scale),double(p.d1),double(p.d2));o+=b;
    std::snprintf(b,sizeof b,",\"pin\":%d,\"protect\":%.2f,\"mask\":%u,\"properties\":{\"policy\":\"%s\",\"pin\":\"%s\",\"protect\":\"%s\",\"mask\":\"%s\"}",
        live.pin.load(),double(floatOf(live.protect.load())),live.mask.load(),pt.policy,pt.pin,pt.protect,pt.mask);o+=b;
    u("propertyReads",propertyReads);u("policyRejected",policyRejected);u("scenes",s.scenes);
    u("drawBig",s.drawBig);u("foreignDrawBig",s.foreignDrawBig);u("creatureMain",s.creatureMain);u("creatureOther",s.creatureOther);
    u("streamedType",s.streamedType);u("noPtl",s.noPtl);u("lodGroupType",s.lodGroupType);u("horse",s.horse);u("protected",s.protectedCount);
    u("playerSkip",s.playerSkip);u("forcedMain",s.forcedMain);u("forcedOther",s.forcedOther);u("forcedPin",s.forcedPin);
    u("alreadyResident",s.alreadyResident);u("noResidentLod",s.noResidentLod);u("notSkinned",s.notSkinned);u("wantMedKeepHi",s.wantMedKeepHi);
    u("heldByHysteresis",s.heldByHysteresis);u("tableFull",s.tableFull);u("camFallback",s.camFallback);
    u("drawN",s.drawN);u("drawNLod0",s.drawNExplicit[0]);u("drawNLod1",s.drawNExplicit[1]);u("drawNLod2",s.drawNExplicit[2]);u("drawNKept",s.drawNKept);
    u("postUpdate",s.postUpdate);u("postUpdateForeign",s.postUpdateForeign);u("postTypeCreature",s.postTypeCreature);
    u("postRemapped",s.postRemapped);u("postPinned",s.postPinned);u("postProtected",s.postProtected);u("postStale",s.postStale);
    u("poiStale",s.poiStale);u("sniperNative",s.sniperNative);u("metricInvalid",s.metricInvalid);u("horseExcluded",s.horseExcluded);
    u("playerCreatureSkip",s.playerCreatureSkip);u("poiSamples",s.poiSamples);
    {const std::uint32_t t=nowMs();const bool any=poiCache.samples.load(std::memory_order_acquire)!=0;
        std::snprintf(b,sizeof b,",\"poi\":{\"count\":%u,\"sniper\":%u,\"ageMs\":%lld,\"x\":%.1f,\"y\":%.1f,\"z\":%.1f},\"tld\":[%.2f,%.2f]",
            poiCache.count.load(),poiCache.sniper.load(),any?(long long)(t-poiCache.ms.load()):-1LL,double(floatOf(poiCache.pos[0][0].load())),
            double(floatOf(poiCache.pos[0][1].load())),double(floatOf(poiCache.pos[0][2].load())),tldMinVar?double(ld<float>(tldMinVar)):-1.0,
            tldMaxVar?double(ld<float>(tldMaxVar)):-1.0);o+=b;}
    // creature types seen in the last 10 s: count by the level the engine had at their last draw, horses
    unsigned live10=0,horses=0;std::array<unsigned,4> lv{};const std::uint32_t now=nowMs();
    types.each([&](const auto& t){if(now-t.stamp.load(std::memory_order_relaxed)>10000u)return;++live10;if(t.horse.load())++horses;lv[std::min<unsigned>(t.level.load(),3u)]++;});
    std::snprintf(b,sizeof b,",\"types\":{\"live\":%u,\"horses\":%u,\"level\":[%u,%u,%u,%u]}",live10,horses,lv[0],lv[1],lv[2],lv[3]);o+=b;
    o+=",\"distEdgesM\":[6,14,24,40,80],\"distUnit\":\"engineLodMetric\",\"mainByDist\":[";
    for(unsigned d=0;d<6;++d){std::snprintf(b,sizeof b,"%s[%llu,%llu,%llu,%llu]",d?",":"",(unsigned long long)s.mainByDist[d][0].load(),
        (unsigned long long)s.mainByDist[d][1].load(),(unsigned long long)s.mainByDist[d][2].load(),(unsigned long long)s.mainByDist[d][3].load());o+=b;}
    o+="],\"forcedByDist\":[";
    for(unsigned d=0;d<6;++d){std::snprintf(b,sizeof b,"%s%llu",d?",":"",(unsigned long long)s.forcedByDist[d].load());o+=b;}
    o+="]";
    const auto w=[&](unsigned i){return double(floatOf(windowPerScene[i].load(std::memory_order_relaxed)));};
    std::snprintf(b,sizeof b,",\"perScene\":{\"seq\":%llu,\"scenes\":%.0f,\"creatureMain\":%.2f,\"creatureOther\":%.2f,\"forcedMain\":%.2f,"
        "\"forcedOther\":%.2f,\"forcedPin\":%.2f,\"typeRemaps\":%.2f,\"drawNExplicit\":%.2f,\"mainByDist\":[",(unsigned long long)windowSeq.load(),
        w(0),w(1),w(2),w(3),w(4),w(5),w(6),w(7));o+=b;
    for(unsigned d=0;d<6;++d){std::snprintf(b,sizeof b,"%s[%.2f,%.2f,%.2f,%.2f]",d?",":"",w(8+d*4),w(9+d*4),w(10+d*4),w(11+d*4));o+=b;}
    o+="]}}";
    return o;
}
} // namespace rdrvr::engine::creaturelod
