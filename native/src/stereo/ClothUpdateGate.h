#pragma once
// CLOTH GATE + CLOTH RING: two DIAGNOSTIC levers for the duplicated horse tail (a normal tail plus a second frozen, trembling
// one, both eyes). OFF unless debug.rdrvr.cloth.hooks is 1 at boot: no hook, no ring proof, no property polling, so a normal run is native.
// librdr 1.58 arm64. [V] = read in the binary.
//  * Horse tail = rage::characterCloth. sagClothMgr::UpdateCloth @0x2a8ee50 does `bl ShouldUpdate @0x2a8eea8 (PLT 0x2caf320,
//    JUMP_SLOT 0x2e5fff0); tbz w0,#0 -> 0x2a8f220` (only clears contacts) [V]. That is the only caller. False = no env-cloth and
//    no char-cloth update, so characterClothController::ApplySimulationToMesh (0x1f0c60c) never writes the build slot.
//  * sagClothMgr::ShouldUpdate(fragInst* x1, int kind w2) @0x2a8fb00..0x2a8fcb4 (x0 unused): camManager::GetViewport(channel
//    [camManager]+0x48) -> null or vp+0x370 == 0 -> true; kind 1 sphere = [drawable]+0x20.. + fragInst+0x50/+0x58 (translation),
//    true when [GOT 0x2ddc550]+0x15 or [[inst+0x88]+0x258]+0x3b == 1; else GetSphereCullStatus(sphere) != 0 [V]. In VR the
//    viewport is the STOCK follow camera (VisibilityCullBoundary substitutes the eye only at the audited visibility site), so
//    a horse the head looks at but the stock frustum misses is never simulated (hypothesis A).
//  * Cloth VB ring (hypothesis B): rage::grmGeometry::sm_MultiBufferCount .data 0x2e7a400 (GLOB_DAT 0x2de0f40, value 4),
//    s_BuildBuffer bss 0x4e6e18e0 (0x2de0f50), s_DrawBuffer bss 0x4e6e18e4 (0x2de0f58) [V]. SwapBuffers @0x1b1594c (PerformDMZ,
//    once per frame): draw = build; build = (build+1) % count. grmGeometryQB::Draw @0x1b19110 binds slot s_DrawBuffer (and
//    POSITION1 = slot (draw+count-1) % count @0x1b193d0); grmGeometryQB::DirectDraw @0x1b19050 always binds slot 0. count = 1
//    puts Draw, DirectDraw and the simulation on slot 0 (removes any stage / slot mismatch; the sim may tear-write slot 0).
//
// LEVER 1, GOT wrapper on ShouldUpdate (installed from TraceEntry next to CREATURE LOD; FNV prints of ShouldUpdate, the
//   UpdateCloth call site, the call word and the GOT word; any mismatch = no hook). Native result first, always. Only the
//   audited return address 0x2a8eeac is ever changed. Runs on the cloth task threads: atomics + the fragInst translation only.
//   debug.rdrvr.cloth.update  0 / unset = native (DEFAULT)
//                             1 = force true for kind 1 (character cloth) within debug.rdrvr.cloth.radius of the fresh MAIN camera
//                                 (creaturelod::freshMainCam, published by the MAIN DrawBig, < 2 s old; none = native, noCam++)
//                             2 = force true for every kind
//   debug.rdrvr.cloth.radius  metres, default 40 (1..1000)
//   debug.rdrvr.cloth.hooks   1 at boot = both levers available; anything else = nothing hooked, nothing polled
// LEVER 2, debug.rdrvr.cloth.ring N (unset = never touched; only "1" or "4"): writes sm_MultiBufferCount after its GOT word
//   == base+0x2e7a400 and the SwapBuffers / Draw prints matched and the boot value was 4. 1 is STICKY until the game restarts:
//   a cloth geometry double-buffered while count == 1 (EnableDoubleVertexBuffer @0x1b15548 creates slots 1..count-1 only) has
//   null slots 1..3, and Draw @0x1b193f8 dereferences the POSITION1 slot without a null check -> a later 4 would crash.
//   So "4" after "1" is refused (ring.refused++); restart the game to restore.
// Properties are re-read at most once per second from sceneBegin (render thread). Report graphics.clothGate / graphics.clothRing.
#include "CreatureLod.h" // creaturelod::freshMainCam (MAIN camera position)
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine::clothgate {
// ---- librdr 1.58 arm64 (module-relative) ----
inline constexpr std::uintptr_t shouldUpdateOffset=0x2a8fb00,shouldUpdateEnd=0x2a8fcb4,shouldUpdateGot=0x2e5fff0;
inline constexpr std::uintptr_t callSite=0x2a8eea8,shouldUpdateReturn=0x2a8eeac;  // bl 0x2caf320 (only caller, UpdateCloth)
inline constexpr std::uint32_t callWord=0x9408811eu;
inline constexpr std::uintptr_t multiBufferCountOffset=0x2e7a400,multiBufferCountGot=0x2de0f40; // .data, int 4
inline constexpr std::uintptr_t buildBufferOffset=0x4e6e18e0,buildBufferGot=0x2de0f50;          // bss int
inline constexpr std::uintptr_t drawBufferOffset=0x4e6e18e4,drawBufferGot=0x2de0f58;            // bss int
inline constexpr std::size_t instPos=0x50; // fragInst matrix translation x,y,z (ShouldUpdate kind 1 reads +0x50 / +0x58)
struct Print {std::uintptr_t begin,end;std::uint64_t fnv;const char* name;};
// FNV-1a 64 of librdr.so 1.58 arm64 code (file offset = VA - 0x4000 in the R E segment).
inline constexpr std::array<Print,2> gatePrints{{
    {0x2a8fb00,0x2a8fcb4,0x946b641795b8f78bull,"shouldUpdate"},       // whole function (viewport, sphere, cull status)
    {0x2a8ee84,0x2a8eeb0,0x4367383c17a061b0ull,"updateClothCall"}}};  // UpdateCloth: args, bl ShouldUpdate, tbz
inline constexpr std::array<Print,2> ringPrints{{
    {0x1b1594c,0x1b15984,0x4ed36edf5e01ead5ull,"swapBuffers"},        // draw = build; build = (build+1) % count
    {0x1b193d0,0x1b19400,0xd22a8a6086117a1full,"drawPosition1"}}};    // POSITION1 slot (draw+count-1) % count

using Fn=bool(*)(void*,void*,int);
using Counter=std::atomic<std::uint64_t>;
struct KindStats {Counter calls{},nativeTrue{},nativeFalse{},forced{},farSkip{},noCam{};};
inline std::array<KindStats,3> byKind{}; // [0] env cloth (kind 0), [1] character cloth (kind 1), [2] any other kind
inline Counter foreign{},propertyReads{},rejected{};
inline Fn original{};inline std::uintptr_t engine{};
inline std::atomic<bool> hooked{};inline std::atomic<const char*> installState{"NOT_ATTEMPTED"};
inline std::atomic<int> mode{0};                                    // debug.rdrvr.cloth.update
inline std::atomic<std::uint32_t> radiusBits{0x42200000u};          // debug.rdrvr.cloth.radius, 40.f
// Last native-false call (audited caller): fragInst, its translation, the distance to the MAIN camera (-1 = none).
// lastFalse = any kind; lastCharFalse = kind 1 only (the horse tail; env cloth such as flags would otherwise overwrite it).
struct LastFalse {std::atomic<std::uintptr_t> inst{};std::atomic<int> kind{-1};
    std::atomic<std::uint32_t> pos[3]{},dist{},ms{};std::atomic<std::uint64_t> seq{};};
inline LastFalse lastFalse{},lastCharFalse{};

inline std::uint32_t bitsOf(float v) noexcept {std::uint32_t u;std::memcpy(&u,&v,4);return u;}
inline float floatOf(std::uint32_t u) noexcept {float v;std::memcpy(&v,&u,4);return v;}
inline void bump(Counter& c) noexcept {c.fetch_add(1,std::memory_order_relaxed);}

// ---- LEVER 1: the GOT wrapper (cloth task threads) ----
inline bool decide(std::uintptr_t caller,void* mgr,void* inst,int kind) noexcept {
    const bool n=original(mgr,inst,kind); // native first: identical viewport / cull work and side effects
    if(caller!=engine+shouldUpdateReturn){bump(foreign);return n;}
    auto& k=byKind[kind==0?0:kind==1?1:2];
    bump(k.calls);
    if(n){bump(k.nativeTrue);return true;}
    bump(k.nativeFalse);
    const int m=mode.load(std::memory_order_relaxed);
    float pos[3]{},head[3]{};bool havePos=false,haveHead=false;
    if(inst){std::memcpy(pos,static_cast<const char*>(inst)+instPos,sizeof pos);havePos=true;}
    if(havePos)haveHead=creaturelod::freshMainCam(head);
    float dist=-1.f;
    if(havePos&&haveHead){float d2=0.f;for(unsigned i=0;i<3;++i){const float v=pos[i]-head[i];d2+=v*v;}dist=std::sqrt(d2);}
    {   const auto inner=reinterpret_cast<std::uintptr_t>(inst);const std::uint32_t ms=creaturelod::nowMs();
        const auto record=[&](LastFalse& l) noexcept {
            l.inst.store(inner,std::memory_order_relaxed);l.kind.store(kind,std::memory_order_relaxed);
            for(unsigned i=0;i<3;++i)l.pos[i].store(bitsOf(pos[i]),std::memory_order_relaxed);
            l.dist.store(bitsOf(dist),std::memory_order_relaxed);l.ms.store(ms,std::memory_order_relaxed);
            l.seq.fetch_add(1,std::memory_order_release);};
        record(lastFalse);if(kind==1)record(lastCharFalse);}
    if(m<=0||!inst)return false;
    if(m==1){
        if(kind!=1)return false; // environment cloth stays native in mode 1
        if(!haveHead){bump(k.noCam);return false;}
        const float r=floatOf(radiusBits.load(std::memory_order_relaxed));
        if(!(dist>=0.f&&dist<=r)){bump(k.farSkip);return false;}
    }
    bump(k.forced);return true;
}
__attribute__((noinline)) inline bool hook(void* mgr,void* inst,int kind) {
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    return decide(caller,mgr,inst,kind);
}

// ---- LEVER 2: the ring count (render thread) ----
struct Ring {
    std::atomic<bool> ready{};std::atomic<const char*> state{"NOT_ATTEMPTED"};
    std::atomic<std::uintptr_t> count{},draw{},build{};  // absolute addresses, proven at install
    std::atomic<int> requested{0};                       // 0 = unset, 1 / 4
    std::atomic<bool> sticky{};                          // 1 was written: 4 refused until restart
    Counter writes{},refused{};
};
inline Ring ring{};
inline int readInt(std::uintptr_t a) noexcept {return a?__atomic_load_n(reinterpret_cast<const int*>(a),__ATOMIC_RELAXED):-1;}
inline void applyRing(int want) noexcept {
    ring.requested.store(want,std::memory_order_relaxed);
    if(!ring.ready.load(std::memory_order_acquire)||want==0)return;
    const auto a=ring.count.load(std::memory_order_relaxed);const int now=readInt(a);
    if(now==want)return;
    if(want==4&&ring.sticky.load(std::memory_order_relaxed)){bump(ring.refused);ring.state.store("RING1_STICKY_RESTART_TO_RESTORE");return;}
    if(want!=1)return;
    __atomic_store_n(reinterpret_cast<int*>(a),1,__ATOMIC_RELEASE);ring.sticky.store(true,std::memory_order_relaxed);
    bump(ring.writes);ring.state.store("RING1");
}

// ---- properties (sceneBegin; at most once per second) ----
inline bool property(const char* name,char* out,std::size_t size) noexcept {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX]{};if(__system_property_get(name,v)<=0){out[0]=0;return false;}
    std::snprintf(out,size,"%s",v);return true;
#else
    (void)name;(void)size;out[0]=0;return false;
#endif
}
struct PropertyText {char update[16]{},radius[16]{},ring[16]{};};
// Printable ASCII without quote / backslash (the text is embedded raw in the report JSON); anything else becomes '?'.
inline void jsonSafe(char* s) noexcept {for(;*s;++s){const auto c=static_cast<unsigned char>(*s);if(c<0x20u||c>0x7eu||c=='"'||c=='\\')*s='?';}}
inline PropertyText propertyText{};inline std::atomic<std::uint32_t> propertyTextSeq{}; // single writer (sceneBegin)
inline void readProperties() noexcept {
    PropertyText t{};property("debug.rdrvr.cloth.update",t.update,sizeof t.update);
    property("debug.rdrvr.cloth.radius",t.radius,sizeof t.radius);property("debug.rdrvr.cloth.ring",t.ring,sizeof t.ring);
    int m=0;if(t.update[0]){char* e{};const long v=std::strtol(t.update,&e,10);if(e!=t.update&&!*e&&v>=0&&v<=2)m=int(v);else bump(rejected);}
    float r=40.f;if(t.radius[0]){char* e{};const float v=std::strtof(t.radius,&e);
        if(e!=t.radius&&!*e&&std::isfinite(v)&&v>=1.f&&v<=1000.f)r=v;else bump(rejected);}
    int ringWant=0;if(t.ring[0]){if(!std::strcmp(t.ring,"1"))ringWant=1;else if(!std::strcmp(t.ring,"4"))ringWant=4;else bump(rejected);}
    radiusBits.store(bitsOf(r),std::memory_order_relaxed);mode.store(m,std::memory_order_relaxed);
    applyRing(ringWant);
    const auto seq=propertyTextSeq.load(std::memory_order_relaxed);propertyTextSeq.store(seq+1,std::memory_order_relaxed);
    jsonSafe(t.update);jsonSafe(t.radius);jsonSafe(t.ring);
    std::atomic_thread_fence(std::memory_order_release);propertyText=t;propertyTextSeq.store(seq+2,std::memory_order_release);bump(propertyReads);
}
inline std::atomic<std::uint32_t> lastReadMs{};inline std::atomic<bool> everRead{},armed{};
inline void sceneBegin() noexcept {
    if(!armed.load(std::memory_order_relaxed))return;
    const std::uint32_t now=creaturelod::nowMs();
    if(everRead.load(std::memory_order_relaxed)&&now-lastReadMs.load(std::memory_order_relaxed)<1000u)return;
    const int saved=errno;readProperties();errno=saved;
    lastReadMs.store(now,std::memory_order_relaxed);everRead.store(true,std::memory_order_relaxed);
}

// ---- install ----
struct Api {std::uintptr_t base{};bool(*read)(std::uintptr_t,void*,std::size_t){};bool(*exec)(std::uintptr_t,std::size_t){};
    bool(*replace)(std::uintptr_t,std::uintptr_t,std::uintptr_t){};};
inline std::uint64_t fnv1a(const unsigned char* p,std::size_t n) noexcept {std::uint64_t h=0xcbf29ce484222325ull;for(std::size_t i=0;i<n;++i)h=(h^p[i])*0x100000001b3ull;return h;}
template<std::size_t N> inline const char* checkPrints(const Api& api,const std::array<Print,N>& prints) noexcept {
    for(const auto& pr:prints){const std::size_t n=pr.end-pr.begin;unsigned char buf[0x200];
        if(n>sizeof buf||!api.exec(api.base+pr.begin,n)||!api.read(api.base+pr.begin,buf,n)||fnv1a(buf,n)!=pr.fnv)return pr.name;}
    return nullptr;
}
inline bool gotWord(const Api& api,std::uintptr_t got,std::uintptr_t symbol) noexcept {
    std::uintptr_t v{};return api.read(api.base+got,&v,sizeof v)&&v==api.base+symbol;}
inline void installRing(const Api& api) noexcept {
    if(ring.ready.load())return;
    if(const char* bad=checkPrints(api,ringPrints)){ring.state.store(bad);return;}
    if(!gotWord(api,multiBufferCountGot,multiBufferCountOffset)||!gotWord(api,buildBufferGot,buildBufferOffset)||
       !gotWord(api,drawBufferGot,drawBufferOffset)){ring.state.store("gotWord");return;}
    int count{},draw{},build{};
    if(!api.read(api.base+multiBufferCountOffset,&count,sizeof count)||!api.read(api.base+drawBufferOffset,&draw,sizeof draw)||
       !api.read(api.base+buildBufferOffset,&build,sizeof build)){ring.state.store("unreadable");return;}
    if(count!=4||draw<0||draw>=4||build<0||build>=4){ring.state.store("bootValue");return;}
    ring.count.store(api.base+multiBufferCountOffset,std::memory_order_relaxed);ring.draw.store(api.base+drawBufferOffset,std::memory_order_relaxed);
    ring.build.store(api.base+buildBufferOffset,std::memory_order_relaxed);
    ring.state.store("READY_UNTOUCHED");ring.ready.store(true,std::memory_order_release);
}
// Returns 1 hooked, 0 not hooked (state says why), -1 a rollback failed (caller raises patchFault). The ring lever is
// proven independently (its own prints + GOT words), whatever the gate does.
inline int install(const Api& api) noexcept {
    if(!api.base||!api.read||!api.exec||!api.replace){installState.store("NO_API");ring.state.store("NO_API");return 0;}
    char hooksProp[16]{};property("debug.rdrvr.cloth.hooks",hooksProp,sizeof hooksProp);
    if(!(hooksProp[0]=='1'&&!hooksProp[1])){installState.store("OFF");ring.state.store("OFF");return 0;}
    armed.store(true,std::memory_order_relaxed);
    installRing(api);
    if(hooked.load())return 1;
    if(const char* bad=checkPrints(api,gatePrints)){installState.store(bad);return 0;}
    {std::uint32_t w{};if(!api.read(api.base+callSite,&w,4)||w!=callWord){installState.store("callWord");return 0;}}
    if(!gotWord(api,shouldUpdateGot,shouldUpdateOffset)){installState.store("gotWord");return 0;}
    engine=api.base;original=reinterpret_cast<Fn>(api.base+shouldUpdateOffset);
    readProperties();
    if(!api.replace(api.base+shouldUpdateGot,api.base+shouldUpdateOffset,reinterpret_cast<std::uintptr_t>(&hook))){
        installState.store("GOT_SWAP_FAILED");return 0;}
    hooked.store(true);installState.store("HOOKED");return 1;
}

// ---- report: "clothGate":{...},"clothRing":{...} ----
inline double jf(float v) noexcept {return std::isfinite(v)&&std::fabs(v)<1e9f?double(v):-1.0;} // JSON has no nan / inf
inline std::string report() {
    std::string o;o.reserve(1536);char b[320];
    PropertyText pt{};for(int i=0;i<4;++i){const auto a=propertyTextSeq.load(std::memory_order_acquire);if(a&1u)continue;
        pt=propertyText;std::atomic_thread_fence(std::memory_order_acquire);if(propertyTextSeq.load(std::memory_order_relaxed)==a)break;pt=PropertyText{};}
    std::snprintf(b,sizeof b,"\"clothGate\":{\"hooked\":%d,\"state\":\"%s\",\"mode\":%d,\"radius\":%.1f,\"properties\":{\"update\":\"%s\",\"radius\":\"%s\",\"ring\":\"%s\"}"
        ",\"propertyReads\":%llu,\"rejected\":%llu,\"foreign\":%llu",hooked.load()?1:0,installState.load(),mode.load(),double(floatOf(radiusBits.load())),
        pt.update,pt.radius,pt.ring,(unsigned long long)propertyReads.load(),(unsigned long long)rejected.load(),(unsigned long long)foreign.load());o+=b;
    static constexpr const char* names[3]{"env","character","other"};
    for(unsigned i=0;i<3;++i){const auto& k=byKind[i];
        std::snprintf(b,sizeof b,",\"%s\":{\"calls\":%llu,\"nativeTrue\":%llu,\"nativeFalse\":%llu,\"forced\":%llu,\"farSkip\":%llu,\"noCam\":%llu}",names[i],
            (unsigned long long)k.calls.load(),(unsigned long long)k.nativeTrue.load(),(unsigned long long)k.nativeFalse.load(),
            (unsigned long long)k.forced.load(),(unsigned long long)k.farSkip.load(),(unsigned long long)k.noCam.load());o+=b;}
    for(unsigned w=0;w<2;++w){const LastFalse& l=w?lastCharFalse:lastFalse;
        const auto seq=l.seq.load(std::memory_order_acquire);const std::uint32_t t=creaturelod::nowMs();
        // signed: a cloth thread may store ms after t was read
        const long long age=seq?(long long)std::int32_t(t-l.ms.load()):-1LL;
        std::snprintf(b,sizeof b,",\"%s\":{\"seq\":%llu,\"kind\":%d,\"inst\":\"0x%llx\",\"ageMs\":%lld,\"pos\":[%.2f,%.2f,%.2f],\"headDist\":%.2f}",
            w?"lastCharFalse":"lastFalse",(unsigned long long)seq,l.kind.load(),(unsigned long long)l.inst.load(),age,
            jf(floatOf(l.pos[0].load())),jf(floatOf(l.pos[1].load())),jf(floatOf(l.pos[2].load())),jf(floatOf(l.dist.load())));o+=b;}
    {float h[3]{};const bool cam=creaturelod::freshMainCam(h);
        std::snprintf(b,sizeof b,",\"mainCam\":{\"fresh\":%d,\"pos\":[%.2f,%.2f,%.2f]}}",cam?1:0,jf(h[0]),jf(h[1]),jf(h[2]));o+=b;}
    const bool rr=ring.ready.load(std::memory_order_acquire);
    std::snprintf(b,sizeof b,",\"clothRing\":{\"ready\":%d,\"state\":\"%s\",\"requested\":%d,\"sticky\":%d,\"count\":%d,\"drawBuffer\":%d,\"buildBuffer\":%d"
        ",\"writes\":%llu,\"refused\":%llu}",rr?1:0,ring.state.load(),ring.requested.load(),ring.sticky.load()?1:0,
        rr?readInt(ring.count.load()):-1,rr?readInt(ring.draw.load()):-1,rr?readInt(ring.build.load()):-1,
        (unsigned long long)ring.writes.load(),(unsigned long long)ring.refused.load());o+=b;
    return o;
}
} // namespace rdrvr::engine::clothgate
