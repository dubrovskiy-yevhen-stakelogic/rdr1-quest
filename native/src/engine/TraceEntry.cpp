#include "WorkerLayerIntegration.h"
#include "SceneInputTape.h"
#include "SceneRouteTape.h"
#include "UniformIntegration.h"
#include "OrdinaryReplayIntegration.h"
#include "OrdinaryIntegration.h"
#include <new>
#ifdef RDRVR_HEAD_CAMERA
#include "PlannerReplayCapture.h"
#endif
#include "PipelinePassHashMemo.h"
#include "UnusedDrawLabel.h"
#include "OccluderInjection.h"
#include "SmallObjectCull.h"
#ifdef RDRVR_HEAD_CAMERA
#include "DynamicUniformDescriptors.h"
#include "VmaUniformLife92.h"
#include "VmaHookTransaction92.h"
#include "VmaCodeProof92.h"
#include <sys/system_properties.h>
#include "engine/ColdOcclusionRegistry.h"
#include "NativeQueryState.h"
#include "NativeQueryCodeProof.h"
#include "GameMainVisibilityWait.h"
#include "MainJoin35Diagnostic.h"
#include "NativeMainVisibilityWaitProof.h"
#include "NativeEarlyTraceRoute.h"
#include "RenderAblation54.h"
#include "NativeVertexInputCode58.h"
#include "InstanceFrequency67.h"
#include "NativeModelProbe69.h"
#include "NativeModelCode69.h"
#include "InstanceFrequencyCode67.h"
#include "opaque-run/NativeOpaqueRunHookState.h"
#include "opaque-run/NativeOpaqueRunForward.h"
#include "NativeEarlyBootstrap.h"
#include "NativeEarlyPaletteBootstrap.h"
#include "opaque-run/NativeEarlyColdOwners.h"
#include "NativeEarlyGeometryMutation.h"
#include "NativeOrdinaryGeometryEpoch.h"
#include "OrdinaryReadOnlyPins.h"
#include "NativeEarlySceneLifetime.h"
#include "GameEffectMutation.h"
#include "GameResourceMutation.h"
#include "NativeResourceMutationProof.h"
#include "NativeEffectMutationProof.h"
#include "NativeOcclusionVertexCapture.h"
#include "NativeFramePacing.h"
#include "runtime/GraphicsSettings.h"
#include "GraphicsDrawPolicy.h"
#include "ShadowGrassQuality.h"
#include "PostFxSkip.h"         // STEREO TEST 16 SKIP POSTFX
#include "LampShadows.h"        // LAMP SHADOWS (lamp shadow update / spot cascade / caster cull + readout)
#include "VisibleTrims.h"       // SUN SHADOWS NIGHT OFF gate, LAMP SHADOWS CAP 2 / OFF menu state (observeScene)
#include "SunStaggerLayer.h"     // SHADOW UPDATE 5/6 STAGGER, Vulkan side (before IndexedDispatch.fragment.inc)
#include "S02Levers.h"           // Lever switch board
static_assert(rdrvr::runtime::GraphicsSettings::s02MaskBits==rdrvr::engine::s02::userMask,"cfg s02Mask clamp matches the lever mask");
#include "EngineColorRestart.h"  // COLOUR RESTARTS OFF (configured here, scoped in StereoReplay.cpp)
#include "EyeCull.h"             // EYE CULL A: per-eye count of MAIN objects, skip outside both eyes, bodyDraw
#include "CategoryAblation.h"    // GEOMETRY COST PROFILER (debug.rdrvr.ablate): draw categories suppressed in turn, report graphics.ablation
#include "CreatureLod.h"         // CREATURE LOD (debug.rdrvr.lod.creature): NPC / horse authored LODs by distance, report graphics.creatureLod
#include "StaticLod.h"           // STATIC LOD (debug.rdrvr.lod.static): distant statics draw their authored lo drawable, report graphics.staticLod
#include "TerrainLod.h"          // TERRAIN LOD (debug.rdrvr.lod.terrain, default off): terrain quadtree split distance, report graphics.terrainLod
#include "LowFreqRate.h"         // LOW-FREQUENCY PASSES (debug.rdrvr.lowfreq): per-pipeline coarse fragment rate (MAX with the foveation map), report graphics.lowFreq
#include "IndoorRt.h"            // INDOOR RT + AO-sphere observer
#include "diagnostics/TextureQualityTelemetry.h"
#include "EngineTextureFilter.h" // TEXTURE FILTER / MIP BIAS, engineLod
#include "MeminfoCache.h"        // Meminfo cache, hygiene report
#include "Teleport.h"            // Player position, saved spots, teleport on the main thread
#include "GameClock.h"           // debug.rdrvr.clock / clock.freeze on the same main-thread hook
#include "vr/VrMenuPages.h"      // MENU: vr.cfg, debug.rdrvr.vr.* kill switches, VR/CHEATS pages, report vr.menu
#include "vr/VrLocomotion.h"     // LOCO: free locomotion + bump/stumble suppression (hooks installed in arm)
#include "vr/VrCamera.h"         // CAMERA: mover Sim vtable fingerprint, report vr.camera
#include "WeaponContextReader.h" // HOLSTERS: context adapter
#include "vr/VrHolsters.h"       // HOLSTERS (VrMainThread.h, VrHaptics.h), report vr.holsters
#include "vr/VrWeapon.h"         // WEAPON: report vr.weapon
#include "vr/VrControls.h"       // Controls: fast jump hook install, report vr.controls
#include "vr/VrMelee.h"          // MELEE: install (prints, GOT hook SetupActionTreePacket), report vr.melee
#include "vr/VrCover.h"          // AUTO COVER: install (COVER input filter + SetAttachRequest GOT hooks), report vr.cover
#include "vr/VrBody.h"           // BODY: install, report vr.body
#include "vr/VrCutscene.h"       // CUTSCENES: report vr.cutscene
#include "vr/VrArc.h"            // ARC: reports vr.arc / vr.probes
#include "vr/VrTeleportLoco.h"   // TELEPORT locomotion: install (prints / GOT words), report vr.tploco
#include "vr/VrLasso.h"          // LASSO: graded install (prints / GOT words / H1-H3), report vr.lasso
#include "vr/VrMarker.h"         // QUEST MARKER: install (prints / GOT words, reads only), report vr.marker
#include "vr/VrMotionActors.h"   // AppSW per-actor motion vectors (main-thread actor matrices), report vr.mvactors
#include "vr/VrInputRec.h"       // INPUT RECORD / REPLAY: files dir (rdrvr/), reports vr.inputRec / vr.inputReplay
#include "vr/HandsPass.h"        // HANDS: report vr.hands
#include "vr/TaaPass.h"          // TAA: report vr.taa
#include "vr/VrHud.h"            // HUD: optional coverage alpha on the HUD draw flushes, report vr.hud
#include "vr/VrCheats.h"         // CHEATS: CHEATS page rows + main-thread engine calls, report vr.cheats
#endif
#include "NativeDescriptorReuse.h"
#include "NativeDrawSuppression.h"
#include "StereoReplay.h"
#include "BodyDrawRecordReader.h"
#include "diagnostics/PerformanceProfile.h"
#include "diagnostics/VerificationGate.h"
#ifdef RDRVR_BENCH_CONTROL
#include "diagnostics/ShaderCapture.h"
#endif
#include "TreeTimerRouting.h"
#ifdef RDRVR_FRAGMENT_RATE
#include "GameFragmentRate.h"
#endif
#include "GameFoveation.h"
#include "NativeResolutionPolicy.h"
#include "NativeEyeResolution.h"
#include "NativeFramebufferExtent.h"
#include "TraceEntry.h"
#include "runtime/AndroidThreadPolicy.h"
#include "TraceSymbols.h"
#ifdef RDRVR_GAME_XR
#include "GameXr.h"
#endif
#include "ProbePolicy.h"
#ifdef RDRVR_TOUCH_CONTROLS
#include "InputCallBoundary.h"
#include "ReticleCallBoundary.h"
#endif
#ifdef RDRVR_CAMERA_BRIDGE
#include "CameraCallBoundary.h"
#ifdef RDRVR_PASS_RESOURCES
#include "VisibilityCallBoundary.h"
#include "VisibilityCullBoundary.h"
#endif
#endif
#include "diagnostics/VulkanTrace.h"
#include "diagnostics/ThreadMemoryReader.h"
#include "diagnostics/GuardedReadSites.h"
#include "diagnostics/GuardedReadVector.h"
#include <link.h>
#include <android/native_activity.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory> // lowfreq: twin batch (std::unique_ptr)
#include <mutex>
#include <sstream>
#include <vector>
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_GAME_XR)&&defined(RDRVR_TOUCH_CONTROLS)&&defined(RDRVR_CAMERA_BRIDGE)&&defined(RDRVR_NATIVE_MENUS)
#define RDRVR_PX2D 1 // PX 2D: needs the bind-only draw path (postfx), the HUD frame hooks and gameNativeFramePacingAllowed
#if __has_include(<vr/VrHud.h>) // (angle form: some build tools reject an unresolved quoted include even under __has_include)
#include <vr/VrHud.h> // With the VR HUD built in, the HUD interval stays native while its HUD row is not OFF
#define RDRVR_PX2D_VRHUD 1
#endif
#endif
#ifdef RDRVR_HEAD_CAMERA
// PX 2D (debug.rdrvr.px2d, live, read about once per second on the render thread at RenderScene entry; 0 = native =
// the engine's exact behaviour; report graphics.px2d). In a stereo pair frame the RenderFrame 2D tail only feeds the 1346x720 Android surface,
// whose world present is bypassed (theater.worldBypassed): the headset gets the eyes captured inside RenderScene. Its draws go bind-only +
// NO-FLUSH (postfx::dropDraw / noFlushNow: no vkCmdDraw and no BindInputLayout = no render-pass begin; locks, clears, CPU work native;
// pending clears land at the next native flush = the unlock in Apply360Gamma / EndFrame, 2D surface only):
//   bit 1 (1): the HUD/UI span from the exact grcWorldIdentity (hudBegin) through TouchUI::Render, RenderFrame's PIX "ClearAlpha" (grcState
//              16 = 8, full-screen alpha=1 grcBegin/grcVertex x3/grcEnd triangle on the HUD target; straight-line code 0x2a589e8..0x2a58bc8)
//              up to the entry of MoviePlayer::DrawUpdate (GOT 0x2e49a20, exact return 0x2a58bcc; any DrawUpdate caller closes the span,
//              without that hook the span ends at TouchUI::Render), plus the dormant HUD producer's eager factory / frame-buffer flush
//              (EngineHudProducer::begin, dormant memo: the next native Clear/draw consumes the pending binding instead, the engine's order).
//   bit 2 (2): grPostFX::Apply360Gamma "#Final Gamma Adjust" (GOT 0x2e22f80, exact return 0x2a58c54): the function runs natively (PIX,
//              UnlockRenderTargetBindBackBuffer(0,null,-1,false), effect vars), only its TWODBlit draw is dropped; EndFrame's FlushFrameBuffer
//              (dirty bit 1) + TransitionBackBuffer still give the Android backbuffer its (now empty) pass and the present layout.
//              Its enable byte [this+0x470] = 0 (0x23e8fdc ldrb / cbz: returns at once, the 2D blit then is grPostFX::Process's BlitToRT)
//              opens no span (gammaInactive).
// MoviePlayer::DrawUpdate / Draw (in-world cinema video) and everything after Apply360Gamma stay native. Gate, once per RenderFrame at
// the exact grcWorldIdentity (after the HUD producer's Begin), LOCK-FREE (never StereoMailbox::freshCompatible, whose mutex
// the XR thread's render() holds through its late-latch / fence / swapchain waits): a RenderScene ran inside this RenderFrame &&
// postfx::pairScene() (its stereo pair was recorded) && the HUD producer did not go active in this frame (EngineHudProducer::begin ->
// producerBegun) && the last game present (GameTheater::present -> notePresent, the previous RenderFrame's) was WORLD_PRESENT_UNCHANGED
// or WORLD_STEREO_RETIRED (= every gameWorldPresentBypass / gameWorldPresentOnly predicate: HUD producer / capture, pending overlay,
// mailbox fresh, present queue) && gameNativeFramePacingAllowed() now (session, presentation WORLD, world recorded, tracked views; short
// locks only). Theater / menu / cutscene-screen frames and frames whose HUD is captured stay native. A nested RenderFrame, a RenderScene
// and the RenderFrame exit close every span.
namespace rdrvr::engine::px2d {
inline constexpr unsigned hudBit=1,gammaBit=2;
// librdr 1.58 arm64 (llvm-readelf -r JUMP_SLOTs of librdr.so; raw bytes at file offset = vaddr - 0x4000).
inline constexpr std::uintptr_t gammaOffset=0x23e8fb8,gammaGotOffset=0x2e22f80,gammaReturnOffset=0x2a58c54; // rage::grPostFX::Apply360Gamma
inline constexpr std::array<unsigned char,64> gammaFingerprint{
#include "gamecode/TraceEntry_23e8fb8_64_8d.inc"
};
inline constexpr std::array<unsigned char,4> gammaCall{124,113,7,148};                                        // bl Apply360Gamma@plt @0x2a58c50
inline constexpr std::uintptr_t movieOffset=0x2997874,movieGotOffset=0x2e49a20,movieReturnOffset=0x2a58bcc;  // MoviePlayer::DrawUpdate(bool)
inline constexpr std::array<unsigned char,32> movieFingerprint{
#include "gamecode/TraceEntry_2997874_32_8d.inc"
};
inline constexpr std::array<unsigned char,4> movieCall{238,166,8,148};                                        // bl DrawUpdate@plt @0x2a58bc8
namespace {
std::atomic<int> property{-1};std::atomic<std::uint64_t> propertyReads{};std::uint64_t lastReadNs{}; // property 0..3, -1 absent/invalid (= 0)
std::atomic<bool> movieHooked{},gammaHooked{};std::atomic<const char*> movieFp{"unconfigured"},gammaFp{"unconfigured"};
struct Stats {
    std::atomic<std::uint64_t> frames{},gateChecks{},gateOpen{},noScene{},noPair{},noWorld{},hudSpans{},hudDropped{},tailSpans{},tailDropped{};
    std::atomic<std::uint64_t> gammaCalls{},gammaForeign{},gammaSpans{},gammaDropped{},asked{},flushSkipped{},flushMiss{},flushVerify{},flushOverlay{};
    std::atomic<std::uint64_t> memoStored{},closedMovie{},closedMovieForeign{},closedHudEnd{},closedFrameEnd{},closedNested{},closedScene{};
    std::atomic<std::uint64_t> theaterCopies{},theaterHudCopies{};
    std::atomic<std::uint64_t> noPresent{},noHudProducer{},presents{},gammaInactive{}; // Lock-free gate reasons / present notes
};
Stats stats;
std::atomic<std::uint64_t> lastDropScene{}; // postfx::sceneSerial of the last RenderFrame that opened a drop span (GameTheater present check)
// The last game present (GameTheater::present -> notePresent): (stats.frames at the present << 2) | kind
// (0 skipped / not the game swapchain, 1 copied to the theater, 2 WORLD_PRESENT_UNCHANGED, 3 WORLD_STEREO_RETIRED).
inline constexpr unsigned presentSkipped=0,presentCopied=1,presentBypassed=2,presentRetired=3;
static_assert(presentSkipped<presentCopied&&presentCopied<presentBypassed&&presentBypassed<presentRetired&&presentRetired<4,"kind fits 2 bits");
std::atomic<std::uint64_t> presentState{};
struct FrameState {std::uint64_t scene{},serial{},hudBefore{},tailBefore{};bool inFrame{},gateDone{},gateOpen{},ask{},hud{},tail{},producerActive{};};
thread_local FrameState frame{}; // render thread: the RenderFrame in progress
void bump(std::atomic<std::uint64_t>& c) noexcept {c.fetch_add(1,std::memory_order_relaxed);}
// The last game present kept the world (bypassed / retired) and came from the previous RenderFrame (or this one); the
// present-time verdict already holds the StereoMailbox freshness, so the render thread never takes that mutex at grcWorldIdentity.
inline bool presentWorld(const FrameState& f) noexcept {
    const auto p=presentState.load(std::memory_order_relaxed);return (p&3u)>=presentBypassed&&f.serial-(p>>2)<=1u;}
void openDrop(std::uint64_t& before) noexcept {
    before=postfx::threadDropped;++postfx::threadScopes;++postfx::threadNoFlush;postfx::liveScopes.fetch_add(1,std::memory_order_relaxed);
    lastDropScene.store(postfx::sceneSerial.load(std::memory_order_relaxed),std::memory_order_relaxed);
}
std::uint64_t closeDrop(std::uint64_t before) noexcept {
    postfx::liveScopes.fetch_sub(1,std::memory_order_relaxed);--postfx::threadNoFlush;--postfx::threadScopes;return postfx::threadDropped-before;}
// tailDropped = draws dropped after TouchUI::Render (ClearAlpha: 1 per frame expected).
bool closeHud() noexcept {
    auto& f=frame;if(!f.hud)return false;
    if(f.tail)stats.tailDropped.fetch_add(postfx::threadDropped-f.tailBefore,std::memory_order_relaxed);
    f.hud=f.tail=false;stats.hudDropped.fetch_add(closeDrop(f.hudBefore),std::memory_order_relaxed);return true;
}
void closeAll(std::atomic<std::uint64_t>& reason) noexcept {if(closeHud())bump(reason);}
}
// Requested bits while the bind-only draw path is installed (one relaxed load when off).
inline unsigned want() noexcept {
    const int p=property.load(std::memory_order_relaxed);
    return p>0&&postfx::ready.load(std::memory_order_relaxed)?unsigned(p)&(hudBit|gammaBit):0u;
}
// observeScene (render thread): a span never covers a RenderScene, and a RenderScene after the gate keeps the rest of that RenderFrame
// native (gamma too); debug.rdrvr.px2d at most once per second.
inline void sceneBegin() noexcept {
    closeAll(stats.closedScene);if(frame.gateDone)frame.gateOpen=false;
#if defined(__ANDROID__)
    const auto t=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if(!propertyReads.load(std::memory_order_relaxed)||t-lastReadNs>=1000000000ull){
        lastReadNs=t;const int saved=errno;char v[PROP_VALUE_MAX]{};const int n=__system_property_get("debug.rdrvr.px2d",v);errno=saved;
        // Unset -> 1 (HUD 2D tail only; cuts post from about 0.5 to 0.1 ms per frame). With
        // RDRVR_PX2D_VRHUD (VR HUD built in) unset = off until preGate checks the VR HUD mode.
#ifdef RDRVR_PX2D_VRHUD
        constexpr int unsetValue=-1;
#else
        constexpr int unsetValue=1;
#endif
        property.store(n<=0?unsetValue:n==1&&v[0]>='0'&&v[0]<='3'?v[0]-'0':-1,std::memory_order_relaxed);bump(propertyReads);}
#endif
}
#ifdef RDRVR_PX2D
// RAII in the RenderFrame hook (hudFrame): per-frame gate state; every span is closed at the exit, an outer one before a nested frame.
class Frame {
    FrameState saved_;bool nested_;
public:
    Frame() noexcept:saved_(frame),nested_(frame.inFrame){
        if(nested_)closeAll(stats.closedNested);
        frame=FrameState{};frame.inFrame=true;frame.scene=postfx::sceneSerial.load(std::memory_order_relaxed);
        frame.serial=stats.frames.fetch_add(1,std::memory_order_relaxed)+1; // presentWorld age check
    }
    ~Frame(){
        closeAll(stats.closedFrameEnd);
        frame=saved_;if(nested_){frame.hud=frame.tail=frame.ask=false;frame.gateDone=true;frame.gateOpen=false;} // the outer frame stays native
    }
    Frame(const Frame&)=delete;Frame& operator=(const Frame&)=delete;
};
// Before the HUD producer's Begin: the part of the gate that does not depend on the producer (it only arms the flush memo).
inline bool preGate() noexcept {
    auto& f=frame;return f.inFrame&&!f.gateDone&&postfx::sceneSerial.load(std::memory_order_relaxed)!=f.scene&&postfx::pairScene()&&presentWorld(f);}
inline void armProducer(unsigned bits) noexcept {frame.ask=(bits&hudBit)&&preGate();} // hudBegin, before the native grcWorldIdentity
// At the exact grcWorldIdentity after the HUD producer's Begin (frame.producerActive is current): once per RenderFrame, lock-free.
inline bool gate() noexcept {
    auto& f=frame;if(f.gateDone)return f.gateOpen;
    f.gateDone=true;bump(stats.gateChecks);
    if(!f.inFrame||postfx::sceneSerial.load(std::memory_order_relaxed)==f.scene){bump(stats.noScene);return false;}
    if(!postfx::pairScene()){bump(stats.noPair);return false;}
    if(f.producerActive){bump(stats.noHudProducer);return false;} // the native HUD producer captures this frame's HUD interval
#ifdef RDRVR_PX2D_VRHUD
    if(::rdrvr::vr::hud::mode()!=::rdrvr::vr::hud::Off){bump(stats.noWorld);return false;} // the VR HUD captures this interval
#endif
    if(!presentWorld(f)){bump(stats.noPresent);return false;} // last present copied / skipped / too old (theater, HUD overlay, stale mailbox)
    // No gameWorldPresentOnly here (StereoMailbox::freshCompatible blocks behind the XR thread's render()); the current
    // frame's session / presentation WORLD / world recorded / tracked views take short locks only (as the frame-pacing hook already does).
    if(!gameNativeFramePacingAllowed()){bump(stats.noWorld);return false;}
    bump(stats.gateOpen);return f.gateOpen=true;
}
inline void hudBegin(unsigned bits) noexcept {
    auto& f=frame;f.ask=false;
    if(!bits||!gate())return;
    if((bits&hudBit)&&!f.hud){f.hud=true;openDrop(f.hudBefore);bump(stats.hudSpans);}
}
// After TouchUI::Render: the exact one keeps the span open over ClearAlpha while the DrawUpdate close is installed.
inline void hudEnd(bool exact) noexcept {
    auto& f=frame;if(!f.hud||f.tail)return;
    if(exact&&movieHooked.load(std::memory_order_relaxed)){f.tail=true;f.tailBefore=postfx::threadDropped;bump(stats.tailSpans);return;}
    closeHud();bump(stats.closedHudEnd);
}
// MoviePlayer::DrawUpdate entry, any caller: decoded video for in-world screens is never inside a span.
inline void movieUpdate(bool exact) noexcept {if(closeHud())bump(exact?stats.closedMovie:stats.closedMovieForeign);}
// Around the exact Apply360Gamma call (bit 2, gate of this RenderFrame open).
inline constexpr std::uintptr_t gammaEnableOffset=0x470; // Apply360Gamma 0x23e8fdc ldrb w8,[x0,#0x470]; cbz (gammaFingerprint[36..43])
class Gamma {
    bool active_{};std::uint64_t before_{};
public:
    Gamma(bool exact,const void* self) noexcept {
        if(!exact){bump(stats.gammaForeign);return;}
        bump(stats.gammaCalls);const auto& f=frame;
        if(!(want()&gammaBit)||!f.inFrame||!f.gateDone||!f.gateOpen)return;
        // Byte 0 = the native function returns at once (no TWODBlit to drop): not a hook-coverage failure, counted apart.
        if(!self||!*(reinterpret_cast<const volatile unsigned char*>(self)+gammaEnableOffset)){bump(stats.gammaInactive);return;}
        active_=true;openDrop(before_);bump(stats.gammaSpans);
    }
    ~Gamma(){if(active_)stats.gammaDropped.fetch_add(closeDrop(before_),std::memory_order_relaxed);}
    Gamma(const Gamma&)=delete;Gamma& operator=(const Gamma&)=delete;
};
#endif
// EngineHudProducer::begin (weak references there): true only inside this frame's exact grcWorldIdentity Begin with bit 1 and the
// producer-independent gate part true; events 0 flush skipped (dormant memo hit, overlay still unsupported), 1 memo miss, 2 re-verify,
// 3 overlay now supported, 4 dormant memo stored.
bool producerFlushAsk() noexcept {if(!frame.ask)return false;bump(stats.asked);return true;}
void producerFlushNote(unsigned event) noexcept {
    switch(event){case 0:bump(stats.flushSkipped);break;case 1:bump(stats.flushMiss);break;case 2:bump(stats.flushVerify);break;
        case 3:bump(stats.flushOverlay);break;default:bump(stats.memoStored);break;}
}
// EngineHudProducer::begin exit (weak reference there): the producer holds a native HUD interval in this RenderFrame, so
// its HUD draws feed the HUD layer and the gate stays closed (lock-free stand-in for GameXr's hudProducerActive).
void producerBegun(bool active) noexcept {if(active)frame.producerActive=true;}
// GameTheater::present (weak reference there), every outer game present: kind 0 skipped, 1 copied to the theater (not bypassed),
// 2 WORLD_PRESENT_UNCHANGED, 3 WORLD_STEREO_RETIRED. Publishes the lock-free gate input; each dropped 2D tail is checked by one present
// only.
void notePresent(unsigned kind,bool hudOverlay) noexcept {
    presentState.store(stats.frames.load(std::memory_order_relaxed)<<2|(kind&3u),std::memory_order_relaxed);bump(stats.presents);
    if(!lastDropScene.load(std::memory_order_relaxed))return;
    const auto dropped=lastDropScene.exchange(0,std::memory_order_relaxed);const auto scene=postfx::sceneSerial.load(std::memory_order_relaxed);
    if(kind!=presentCopied||!scene||dropped!=scene)return;
    bump(stats.theaterCopies);if(hudOverlay)bump(stats.theaterHudCopies);
}
inline std::string report() {
    std::ostringstream o;const auto& s=stats;
    o<<"\"px2d\":{\"property\":"<<property.load()<<",\"want\":"<<want()<<",\"propertyReads\":"<<propertyReads.load()
     <<",\"movieHooked\":"<<(movieHooked.load()?1:0)<<",\"movieFingerprint\":\""<<movieFp.load()<<"\",\"gammaHooked\":"<<(gammaHooked.load()?1:0)
     <<",\"gammaFingerprint\":\""<<gammaFp.load()<<"\",\"frames\":"<<s.frames.load()<<",\"gateChecks\":"<<s.gateChecks.load()<<",\"gateOpen\":"<<s.gateOpen.load()
     <<",\"gateNoScene\":"<<s.noScene.load()<<",\"gateNoPair\":"<<s.noPair.load()<<",\"gateHudProducer\":"<<s.noHudProducer.load()
     <<",\"gateNoPresent\":"<<s.noPresent.load()<<",\"gateNoWorld\":"<<s.noWorld.load()
     <<",\"presents\":"<<s.presents.load()<<",\"lastPresent\":"<<(presentState.load()&3u) // 0 skipped 1 copied 2 bypassed 3 retired
     <<",\"hudSpans\":"<<s.hudSpans.load()<<",\"hudDropped\":"<<s.hudDropped.load()<<",\"tailSpans\":"<<s.tailSpans.load()<<",\"tailDropped\":"<<s.tailDropped.load()
     <<",\"gammaCalls\":"<<s.gammaCalls.load()<<",\"gammaForeign\":"<<s.gammaForeign.load()<<",\"gammaSkipped\":"<<s.gammaSpans.load()<<",\"gammaDropped\":"<<s.gammaDropped.load()
     <<",\"gammaInactive\":"<<s.gammaInactive.load()
     <<",\"producer\":{\"asked\":"<<s.asked.load()<<",\"flushSkipped\":"<<s.flushSkipped.load()<<",\"memoMiss\":"<<s.flushMiss.load()<<",\"reverify\":"<<s.flushVerify.load()
     <<",\"overlayNow\":"<<s.flushOverlay.load()<<",\"memoStored\":"<<s.memoStored.load()<<'}'
     <<",\"theaterCopies\":"<<s.theaterCopies.load()<<",\"theaterHudCopies\":"<<s.theaterHudCopies.load()
     <<",\"closed\":{\"movie\":"<<s.closedMovie.load()<<",\"movieForeign\":"<<s.closedMovieForeign.load()<<",\"hudEnd\":"<<s.closedHudEnd.load()
     <<",\"frameEnd\":"<<s.closedFrameEnd.load()<<",\"nested\":"<<s.closedNested.load()<<",\"scene\":"<<s.closedScene.load()<<"}}";
    return o.str();
}
} // namespace rdrvr::engine::px2d
#endif
namespace rdrvr::engine {
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_NATIVE_MENUS)
void gameConfigureHudScreenViewport(std::uintptr_t) noexcept;std::string gameHudScreenViewportReport();
std::string gameEyeWaterReport();
void gameConfigureSubtitles(std::uintptr_t,void (*)(void*,bool)) noexcept;std::string gameSubtitlesReport();
void gameConfigureWaterQuery(std::uintptr_t) noexcept;
void gameConfigureCutsceneActors(std::uintptr_t,std::uintptr_t,std::uintptr_t,const char* (*)(std::uintptr_t,int),std::uintptr_t,std::uintptr_t) noexcept;std::string gameCutsceneActorNames();
#endif
namespace {
using namespace trace_generated;
#ifdef RDRVR_HEAD_CAMERA
#include "IndexedDispatch.fragment.inc"
#endif
#ifdef RDRVR_GAME_SESSION
#ifdef RDRVR_PASS_RESOURCES
#ifdef RDRVR_HEAD_CAMERA
constexpr unsigned cameraDispatchCount=8;
constexpr unsigned dynamicDispatchCount=3+cameraDispatchCount+indexedDispatchCount;
#else
constexpr unsigned dynamicDispatchCount=0;
#endif
#ifdef RDRVR_BENCH_CONTROL
#ifdef RDRVR_FRAGMENT_RATE
constexpr unsigned dispatchCount=31+dynamicDispatchCount;
#else
constexpr unsigned dispatchCount=25+dynamicDispatchCount;
#endif
diagnostics::ShaderCapture shaderCapture;
PFN_vkCreateShaderModule originalCreateShader{};
bool readMemory(void*,std::uintptr_t,void*,std::size_t);
VKAPI_ATTR VkResult VKAPI_CALL createShader(VkDevice d,const VkShaderModuleCreateInfo* c,const VkAllocationCallbacks* a,VkShaderModule* out){
    const VkShaderModuleCreateInfo* patched{};const auto* info=stereo::patchShaderCreate(c,&patched)?patched:c;
    const auto result=shaderCapture.invoke(cameraCreateShader,d,info,a,out,readMemory,nullptr);const int saved=errno;stereo::shaderCreated(result==VK_SUCCESS&&out?*out:VK_NULL_HANDLE,result);
#ifdef RDRVR_HEAD_CAMERA
    coldOcclusionRegistry.createdShader(d,c,result==VK_SUCCESS&&out?*out:VK_NULL_HANDLE,result);
    if(result==VK_SUCCESS&&out&&c&&c->sType==VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO){ // Ablate: technique of the ORIGINAL code (cold)
        ablate::shaderCreated(c->pCode,c->codeSize,reinterpret_cast<std::uintptr_t>(*out));
        lowfreq::shaderCreated(c->pCode,c->codeSize,reinterpret_cast<std::uintptr_t>(*out));} // lowfreq: fragment technique group (cold)
#endif
    errno=saved;return result;
}
#else
constexpr unsigned dispatchCount=24+dynamicDispatchCount;
#endif
PFN_vkGetInstanceProcAddr originalInstanceProc{};PFN_vkGetDeviceProcAddr originalDeviceProc{};
#ifdef RDRVR_FRAGMENT_RATE
PFN_vkCreateRenderPass originalRateCreatePass{};PFN_vkDestroyRenderPass originalRateDestroyPass{};
PFN_vkCreateFramebuffer originalRateCreateFramebuffer{};PFN_vkCreateGraphicsPipelines originalRateCreatePipelines{};
PFN_vkCreateImageView originalRateCreateView{};PFN_vkDestroyImageView originalRateDestroyView{};
// VRS bridge (KHR rate attachment, unconfigured on an FDM device) -> FDM density patch (only on the FDM device)
// -> driver; the foveation registry observes every pass/framebuffer/view (begin-time eye substitution).
VKAPI_ATTR VkResult VKAPI_CALL densityCreatePass(VkDevice d,const VkRenderPassCreateInfo* c,const VkAllocationCallbacks* a,VkRenderPass* out){return gameFoveationCreatePass(originalRateCreatePass,d,c,a,out);}
// Boundary twins (STEREO TEST 10/11/12, StereoBoundary.h): empty/head/tail twins of every engine
// pass the fragment-rate bridge converted, created through the SAME bridge call (identical R8_UINT
// VRS attachment: every framebuffer of P - engine, foveation variants, right-eye resolved ones -
// can begin them, and P's pipelines/secondaries are valid inside them) but not through
// rateCreatePass, so the foveation pass shapes and eye LOAD/CLEAR counters stay untouched. A twin
// the bridge did not convert would not be compatible with P: all three are dropped then
// (twinFailures). Every pass gets a PassDesc.
void boundaryTwinsDestroy(VkDevice d,const stereo::PassTwins& t,const VkAllocationCallbacks* a){
    for(const VkRenderPass h:{t.empty,t.head,t.tail})if(h){gameFoveationPassDestroyed(d,h);gameFragmentRateBridge().destroyPass(originalRateDestroyPass,d,h,a);}
}
void boundaryTwinsCreate(VkDevice d,const VkRenderPassCreateInfo* c,const VkAllocationCallbacks* a,VkRenderPass pass){
    stereo::PassTwins twins{};if(!c)return;
    stereo::boundary::passFlags(*c,twins.flags);
    runtime::FragmentRateRouting::Pass converted{};
    if(gameFragmentRateBridge().routing().pass(d,pass,converted)){
        stereo::boundary::TwinInfos infos;
        if(stereo::boundary::deriveTwins(*c,infos)){
            bool ok=true;VkRenderPass* slots[3]{&twins.empty,&twins.head,&twins.tail};
            for(unsigned k=0;k<3&&ok;++k){VkRenderPass h{};
                const auto r=gameFragmentRateBridge().createPass(densityCreatePass,d,&infos.infos[k],a,&h);
                if(r!=VK_SUCCESS||!h){ok=false;break;}
                *slots[k]=h;runtime::FragmentRateRouting::Pass twin{};
                ok=gameFragmentRateBridge().routing().pass(d,h,twin)&&twin.attachments==converted.attachments;}
            if(!ok){boundaryTwinsDestroy(d,twins,a);const auto flags=twins.flags;twins=stereo::PassTwins{};twins.flags=flags;stereo::noteTwinFailure();}
        }
    }
    stereo::renderPassCreated(d,pass,c,twins);
}
VKAPI_ATTR VkResult VKAPI_CALL rateCreatePass(VkDevice d,const VkRenderPassCreateInfo* c,const VkAllocationCallbacks* a,VkRenderPass* out){
    const auto r=gameFragmentRateBridge().createPass(densityCreatePass,d,c,a,out);const int saved=errno;gameFoveationPassObserved(d,c,r==VK_SUCCESS&&out?*out:VK_NULL_HANDLE,r);
    if(r==VK_SUCCESS&&out&&*out){boundaryTwinsCreate(d,c,a,*out);
#ifdef RDRVR_HEAD_CAMERA
        shadowupdate::stagger::layer::passCreated(*out,c); // Depth-only pass table (atlas loadOp check)
#endif
    }
    errno=saved;return r;
}
VKAPI_ATTR void VKAPI_CALL rateDestroyPass(VkDevice d,VkRenderPass p,const VkAllocationCallbacks* a){
#ifdef RDRVR_HEAD_CAMERA
    shadowupdate::stagger::layer::passDestroyed(p);
#endif
    gameFoveationPassDestroyed(d,p);
    {const int saved=errno;stereo::PassTwins twins{};if(stereo::renderPassDestroyed(d,p,twins))boundaryTwinsDestroy(d,twins,a);errno=saved;} // P's GPU lifetime covers its twins
    gameFragmentRateBridge().destroyPass(originalRateDestroyPass,d,p,a);}
VKAPI_ATTR VkResult VKAPI_CALL stereoCreateFramebuffer(VkDevice d,const VkFramebufferCreateInfo* c,const VkAllocationCallbacks* a,VkFramebuffer* out){
    const auto r=originalRateCreateFramebuffer(d,c,a,out);const int saved=errno;stereo::framebufferCreated(d,c,r==VK_SUCCESS&&out?*out:VK_NULL_HANDLE,r);
    gameFoveationFramebufferCreated(d,c,r==VK_SUCCESS&&out?*out:VK_NULL_HANDLE,r);errno=saved;return r;
}
VKAPI_ATTR VkResult VKAPI_CALL densityCreateFramebuffer(VkDevice d,const VkFramebufferCreateInfo* c,const VkAllocationCallbacks* a,VkFramebuffer* out){return gameFoveationCreateFramebuffer(stereoCreateFramebuffer,d,c,a,out);}
VKAPI_ATTR VkResult VKAPI_CALL rateCreateFramebuffer(VkDevice d,const VkFramebufferCreateInfo* c,const VkAllocationCallbacks* a,VkFramebuffer* out){return gameFragmentRateBridge().createFramebuffer(densityCreateFramebuffer,d,c,a,out);}
VKAPI_ATTR VkResult VKAPI_CALL actualRateCreatePipelines(VkDevice d,VkPipelineCache cache,std::uint32_t n,const VkGraphicsPipelineCreateInfo* c,const VkAllocationCallbacks* a,VkPipeline* out){
    const auto result=cameraRuntime.createPipelines(d,cache,n,c,a,out);const int saved=errno;
#ifdef RDRVR_HEAD_CAMERA
    coldOcclusionRegistry.createdPipelines(d,n,c,out,result);
#endif
    errno=saved;return result;
}
#ifdef RDRVR_HEAD_CAMERA
VKAPI_ATTR void VKAPI_CALL coldDestroyPipeline(VkDevice d,VkPipeline pipeline,const VkAllocationCallbacks* a); // lowfreq: twin rollback (defined below)
// Coarse TWINS of the planned engine pipelines (LowFreqRate.h), created through the same bridge /
// camera / occlusion / stereo / ablate registration as an engine batch, so a bound twin is an ordinary
// known pipeline (tape, right-eye replay, frozen camera, census). The engine keeps its own unchanged
// 1x1 handle; ablateBindPipeline picks.
void lowFreqTwins(VkDevice d,VkPipelineCache cache,std::uint32_t n,const VkGraphicsPipelineCreateInfo* c,const VkAllocationCallbacks* a,const VkPipeline* out,VkResult r,const lowfreq::Batch& b){
    std::unique_ptr<lowfreq::TwinBatch> t(new(std::nothrow) lowfreq::TwinBatch{}); // ~10 KB: off the engine thread's stack (the bridge adds ~23 KB)
    if(!t||!lowfreq::twinBatch(n,c,out,r,b,*t))return;
    const auto start=std::chrono::steady_clock::now();
    const auto tr=gameFragmentRateBridge().createPipelines(actualRateCreatePipelines,d,cache,t->count,t->info.data(),a,t->out.data(),t->size.data());
    stereo::pipelinesCreated(t->count,t->info.data(),t->out.data(),tr);ablate::pipelinesCreated(t->count,t->info.data(),t->out.data(),tr==VK_SUCCESS);
    const auto ns=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count());
    lowfreq::twinsCreated(out,*t,tr,b,ns,[d,a](VkPipeline twin){coldDestroyPipeline(d,twin,a);});
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL rateCreatePipelines(VkDevice d,VkPipelineCache cache,std::uint32_t n,const VkGraphicsPipelineCreateInfo* c,const VkAllocationCallbacks* a,VkPipeline* out){
#ifdef RDRVR_HEAD_CAMERA
    lowfreq::Batch lowFreq{};const bool lowFreqPlanned=lowfreq::plan(d,n,c,lowFreq,gameFragmentRateCapabilities); // lowfreq: twin size per classified pipeline (cold)
#endif
    const auto r=gameFragmentRateBridge().createPipelines(actualRateCreatePipelines,d,cache,n,c,a,out);const int saved=errno;stereo::pipelinesCreated(n,c,out,r);
#ifdef RDRVR_HEAD_CAMERA
    shadowupdate::stagger::layer::pipelinesCreated(n); // Pipelines created inside a partial atlas render (one relaxed load otherwise)
    ablate::pipelinesCreated(n,c,out,r==VK_SUCCESS); // Ablate: VS/FS technique family, topology, depth-only (cold)
    lowfreq::created(n,out); // lowfreq: a reused engine handle forgets a stale twin
    if(lowFreqPlanned)lowFreqTwins(d,cache,n,c,a,out,r,lowFreq);
#endif
    errno=saved;return r;}
VKAPI_ATTR VkResult VKAPI_CALL stereoCreateView(VkDevice d,const VkImageViewCreateInfo* c,const VkAllocationCallbacks* a,VkImageView* out){
    const auto r=originalRateCreateView(d,c,a,out);const int saved=errno;stereo::imageViewCreated(d,c,r==VK_SUCCESS&&out?*out:VK_NULL_HANDLE,r);
    gameFoveationViewCreated(d,c,r==VK_SUCCESS&&out?*out:VK_NULL_HANDLE,r);errno=saved;return r;
}
// stereo::imageViewDestroyed scrubs the bindless clone slots that still hold an override copy of v (before the
// driver destroys it) and, under debug.rdrvr.stereoreplay.scrub (default 1), may take the destroy over: true = deferred,
// it calls originalRateDestroyView itself once the frame retired (stereo::frameHook) or at shutdownAfterIdle. The
// registries (fragment-rate routing, foveation) forget v now either way: the engine has released the handle.
VKAPI_ATTR void VKAPI_CALL stereoDestroyView(VkDevice d,VkImageView v,const VkAllocationCallbacks* a){const int saved=errno;const bool deferred=stereo::imageViewDestroyed(d,v,a,originalRateDestroyView);gameFoveationViewDestroyed(d,v);errno=saved;if(!deferred)originalRateDestroyView(d,v,a);}
VKAPI_ATTR VkResult VKAPI_CALL rateCreateView(VkDevice d,const VkImageViewCreateInfo* c,const VkAllocationCallbacks* a,VkImageView* out){return gameFragmentRateBridge().createView(stereoCreateView,d,c,a,out);}
VKAPI_ATTR void VKAPI_CALL rateDestroyView(VkDevice d,VkImageView v,const VkAllocationCallbacks* a){gameFragmentRateBridge().destroyView(stereoDestroyView,d,v,a);}
// Stereo replay descriptor-set mirror and framebuffer lifetime slots.
PFN_vkAllocateDescriptorSets originalStereoAllocateSets{};PFN_vkUpdateDescriptorSets originalStereoUpdateSets{};PFN_vkFreeDescriptorSets originalStereoFreeSets{};
PFN_vkResetDescriptorPool originalStereoResetPool{};PFN_vkDestroyDescriptorPool originalStereoDestroyPool{};PFN_vkDestroyFramebuffer originalStereoDestroyFramebuffer{};
std::atomic<VkDevice> stereoDevice{};
VKAPI_ATTR VkResult VKAPI_CALL stereoAllocateSets(VkDevice d,const VkDescriptorSetAllocateInfo* i,VkDescriptorSet* out){const auto r=originalStereoAllocateSets(d,i,out);const int saved=errno;stereo::descriptorSetsAllocated(d,i,out,r);errno=saved;return r;}
VKAPI_ATTR void VKAPI_CALL stereoUpdateSets(VkDevice d,std::uint32_t n,const VkWriteDescriptorSet* w,std::uint32_t m,const VkCopyDescriptorSet* c){stereo::updateDescriptorSets(d,n,w,m,c);}
VKAPI_ATTR VkResult VKAPI_CALL stereoFreeSets(VkDevice d,VkDescriptorPool p,std::uint32_t n,const VkDescriptorSet* sets){const int saved=errno;stereo::descriptorSetsFreed(d,p,n,sets);errno=saved;return originalStereoFreeSets(d,p,n,sets);}
VKAPI_ATTR VkResult VKAPI_CALL stereoResetPool(VkDevice d,VkDescriptorPool p,VkDescriptorPoolResetFlags f){const int saved=errno;stereo::descriptorPoolReset(d,p);errno=saved;return originalStereoResetPool(d,p,f);}
VKAPI_ATTR void VKAPI_CALL stereoDestroyPool(VkDevice d,VkDescriptorPool p,const VkAllocationCallbacks* a){const int saved=errno;stereo::descriptorPoolDestroyed(d,p);errno=saved;originalStereoDestroyPool(d,p,a);}
VKAPI_ATTR void VKAPI_CALL stereoDestroyFramebuffer(VkDevice d,VkFramebuffer f,const VkAllocationCallbacks* a){const int saved=errno;stereo::framebufferDestroyed(d,f);gameFoveationFramebufferDestroyed(d,f);errno=saved;originalStereoDestroyFramebuffer(d,f,a);}
#endif
#ifdef RDRVR_HEAD_CAMERA
std::atomic<bool> dynamicNativeHooksReady{},executeLifetimeReady{},coldLifecycleReady{},occlusionNativeOwnersReady{};
bool nativeEarlyResourceHooksReady()noexcept;
bool nativeEarlyRuntimeAvailable()noexcept {
    return coldLifecycleReady.load(std::memory_order_acquire)&&occlusionNativeOwnersReady.load(std::memory_order_acquire)&&
        experimental::nativeEarlyBootstrap.ready()&&nativeEarlyResourceHooksReady()&&nativeQueryHooksReady.load(std::memory_order_acquire);
}

void refreshOcclusionReady()noexcept {
    runtime::graphicsOcclusionAvailable.store(nativeEarlyRuntimeAvailable(),std::memory_order_release);
}
PFN_vkDestroyShaderModule originalColdDestroyShader{};PFN_vkDestroyPipeline originalColdDestroyPipeline{};
VKAPI_ATTR void VKAPI_CALL coldDestroyShader(VkDevice d,VkShaderModule shader,const VkAllocationCallbacks* a){
    const int saved=errno;coldOcclusionRegistry.destroyedShader(d,shader);errno=saved;cameraRuntime.destroyShader(d,shader,a);
}
VKAPI_ATTR void VKAPI_CALL coldDestroyPipeline(VkDevice d,VkPipeline pipeline,const VkAllocationCallbacks* a){
    const auto twin=lowfreq::release(reinterpret_cast<std::uintptr_t>(pipeline)); // lowfreq: the engine pipeline's coarse twin dies with it
    const int saved=errno;coldOcclusionRegistry.destroyedPipeline(d,pipeline);errno=saved;cameraRuntime.destroyPipeline(d,pipeline,a);
    if(twin)coldDestroyPipeline(d,reinterpret_cast<VkPipeline>(twin),a); // a twin has no twin: one level
}
PFN_vkCmdBeginQuery originalObservedBeginQuery{};
PFN_vkCmdEndQuery originalObservedEndQuery{};
VKAPI_ATTR void VKAPI_CALL observedBeginQuery(VkCommandBuffer c,VkQueryPool pool,std::uint32_t index,VkQueryControlFlags flags){
    indexedObservation.sideEffect(c);
    const int saved=errno;nativeQueryGuard.beginQuery(queryHandle(c),queryHandle(pool),index);stereo::leftQueryEdge(c,1);errno=saved; // No left split while a query is active in the pass
    ablate::queryEdge(1); // Ablate: nothing is dropped inside an engine query (one relaxed load when off)
    originalObservedBeginQuery(c,pool,index,flags);
}
VKAPI_ATTR void VKAPI_CALL observedEndQuery(VkCommandBuffer c,VkQueryPool pool,std::uint32_t index){
    indexedObservation.sideEffect(c);originalObservedEndQuery(c,pool,index);const int saved=errno;nativeQueryGuard.endQuery(queryHandle(c),queryHandle(pool),index);stereo::leftQueryEdge(c,-1);errno=saved;
    ablate::queryEdge(-1);
}
PFN_vkCmdExecuteCommands originalDynamicExecute{};
VKAPI_ATTR void VKAPI_CALL dynamicExecute(VkCommandBuffer c,std::uint32_t n,const VkCommandBuffer* commands){
    if(auto* stereoTape_=stereo::recordingFor(c))stereo::recordUnsupported(*stereoTape_,c,"vkCmdExecuteCommands");
    const int saved=errno;nativeQueryGuard.commandUnknown(queryHandle(c));dynamicUniformDescriptors.commandStateInvalidated(c);errno=saved;
    indexedObservation.unknownState(c);cameraRuntime.executeCommands(c,n,commands);
}
PFN_vkCreateDescriptorSetLayout originalDynamicCreateLayout{};
PFN_vkDestroyDescriptorSetLayout originalDynamicDestroyLayout{};
PFN_vkCmdBindDescriptorSets originalDynamicBind{};
VKAPI_ATTR VkResult VKAPI_CALL dynamicCreateLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo* c,const VkAllocationCallbacks* a,VkDescriptorSetLayout* out){
    const auto r=dynamicUniformDescriptors.createLayout(cameraCreateSetLayout,d,c,a,out);const int saved=errno;
    stereo::descriptorLayoutCreated(d,c,r==VK_SUCCESS&&out?*out:VK_NULL_HANDLE,r);errno=saved;return r;
}
VKAPI_ATTR void VKAPI_CALL dynamicDestroyLayout(VkDevice d,VkDescriptorSetLayout l,const VkAllocationCallbacks* a){const int saved=errno;stereo::descriptorLayoutDestroyed(d,l);errno=saved;dynamicUniformDescriptors.destroyLayout(cameraDestroySetLayout,d,l,a);}
VKAPI_ATTR void VKAPI_CALL dynamicBind(VkCommandBuffer c,VkPipelineBindPoint p,VkPipelineLayout l,std::uint32_t first,std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t n,const std::uint32_t* offsets){dynamicUniformDescriptors.bind(cameraBindDescriptors,c,p,l,first,count,sets,n,offsets);}
// Uniform-buffer lifetimes for DynamicUniformDescriptors' cross-frame set
// retention come from the routed vkCreateBuffer/vkDestroyBuffer device procs (every
// engine Vulkan pointer, including the slots VMA copies, is obtained through
// instanceProc/deviceProc). The original is captured from the proc lookup itself,
// first-wins; a later lookup returning a different original stays native (unobserved
// buffers simply never qualify for retention).
std::atomic<PFN_vkCreateBuffer> originalBufferCreate92{};std::atomic<PFN_vkDestroyBuffer> originalBufferDestroy92{};
VKAPI_ATTR VkResult VKAPI_CALL bufferCreate92(VkDevice d,const VkBufferCreateInfo* c,const VkAllocationCallbacks* a,VkBuffer* out){
    const auto r=originalBufferCreate92.load(std::memory_order_acquire)(d,c,a,out);const int saved=errno;
    if(r==VK_SUCCESS&&c&&out&&*out)dynamicUniformDescriptors.bufferCreated(d,*out,c->size,c->usage);
    errno=saved;return r;
}
VKAPI_ATTR void VKAPI_CALL bufferDestroy92(VkDevice d,VkBuffer b,const VkAllocationCallbacks* a){
    const int saved=errno;dynamicUniformDescriptors.bufferDestroyed(d,b);errno=saved;
    originalBufferDestroy92.load(std::memory_order_acquire)(d,b,a);
}
template<class Fn>PFN_vkVoidFunction bufferRoute92(PFN_vkVoidFunction original,std::atomic<Fn>& slot,Fn wrapper)noexcept{
    const auto incoming=reinterpret_cast<Fn>(original);if(incoming==wrapper)return original;
    Fn expected{};if(slot.compare_exchange_strong(expected,incoming,std::memory_order_acq_rel)||expected==incoming)return reinterpret_cast<PFN_vkVoidFunction>(wrapper);
    return original;
}
#endif
PFN_vkCreateImage originalCreateImage{};PFN_vkDestroyImage originalDestroyImage{};
PFN_vkCreateCommandPool originalCreatePool{};PFN_vkDestroyCommandPool originalDestroyPool{};
PFN_vkAllocateCommandBuffers originalAllocateCommands{};PFN_vkFreeCommandBuffers originalFreeCommands{};
PFN_vkBeginCommandBuffer originalBeginCommand{};PFN_vkEndCommandBuffer originalEndCommand{};
PFN_vkResetCommandBuffer originalResetCommand{};PFN_vkResetCommandPool originalResetPool{};
VKAPI_ATTR VkResult VKAPI_CALL createPool(VkDevice d,const VkCommandPoolCreateInfo* c,const VkAllocationCallbacks* a,VkCommandPool* out){
    auto r=originalCreatePool(d,c,a,out);const int saved=errno;gameCommands().poolCreated(d,c,r==VK_SUCCESS && out?*out:VK_NULL_HANDLE,r);errno=saved;return r;
}
VKAPI_ATTR void VKAPI_CALL destroyPool(VkDevice d,VkCommandPool p,const VkAllocationCallbacks* a){
    indexedObservation.invalidateAll();
    const int saved=errno;gameCommands().poolDestroyed(d,p);
#ifdef RDRVR_HEAD_CAMERA
    nativeQueryGuard.poolInvalidated(queryHandle(d),queryHandle(p),true);
    dynamicUniformDescriptors.poolDestroyed(d,p);
#endif
    errno=saved;cameraRuntime.destroyPool(d,p,a);const int resultErrno=errno;
    uniformCapture.revoke();uniformArena.poolDestroyed(d,p);errno=resultErrno;
}
VKAPI_ATTR VkResult VKAPI_CALL allocateCommands(VkDevice d,const VkCommandBufferAllocateInfo* c,VkCommandBuffer* out){
    auto r=cameraRuntime.allocateCommands(d,c,out);const int saved=errno;gameCommands().allocated(d,c,out,r);
#ifdef RDRVR_HEAD_CAMERA
    if(r==VK_SUCCESS&&c&&out)for(std::uint32_t i=0;i<c->commandBufferCount;++i){
        nativeQueryGuard.allocated(queryHandle(d),queryHandle(c->commandPool),queryHandle(out[i]),
            c->sType==VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO&&!c->pNext&&c->level==VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        dynamicUniformDescriptors.commandAllocated(d,c->commandPool,out[i]);
        if(c->sType==VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO&&!c->pNext)
            uniformArena.commandAllocated(d,c->commandPool,out[i],c->level,r);
    }
#endif
    errno=saved;return r;
}
VKAPI_ATTR void VKAPI_CALL freeCommands(VkDevice d,VkCommandPool p,std::uint32_t n,const VkCommandBuffer* c){
    if(c)for(std::uint32_t i=0;i<n;++i)indexedObservation.invalidate(c[i]);
    const int saved=errno;gameCommands().freed(d,p,n,c);
#ifdef RDRVR_HEAD_CAMERA
    if(c)for(std::uint32_t i=0;i<n;++i){nativeQueryGuard.freed(queryHandle(d),queryHandle(p),queryHandle(c[i]));dynamicUniformDescriptors.commandFreed(d,p,c[i]);}
#endif
    errno=saved;cameraRuntime.freeCommands(d,p,n,c);const int resultErrno=errno;
    uniformCapture.revoke();if(c)for(std::uint32_t i=0;i<n;++i)uniformArena.commandFreed(d,p,c[i]);errno=resultErrno;
}
VKAPI_ATTR VkResult VKAPI_CALL beginCommand(VkCommandBuffer c,const VkCommandBufferBeginInfo* b){
    auto r=cameraRuntime.beginCommand(c,b);const int saved=errno;gameCommands().begun(c,b,r);
    if(r==VK_SUCCESS)stereo::commandBegun(stereoDevice.load(std::memory_order_acquire),c);
    indexedObservation.begun(c,r);
#ifdef RDRVR_HEAD_CAMERA
    nativeQueryRecordingBegun(queryHandle(c),r==VK_SUCCESS,b&&b->sType==VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO&&!b->pNext&&!b->pInheritanceInfo&&!(b->flags&VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT));
    dynamicUniformDescriptors.commandBegun(c,r);
    uniformCapture.revoke();uniformArena.commandBegun(c,b,r);
#endif
    errno=saved;return r;
}
VKAPI_ATTR VkResult VKAPI_CALL endCommand(VkCommandBuffer c){nativeVertexStreamMerge58.publish();indexedObservation.invalidate(c);stereo::gpuCommandEnd(c);const int incoming=errno; // Last DETAIL GPU marker
#ifdef RDRVR_HEAD_CAMERA
    nativeQueryGuard.ended(queryHandle(c));
#endif
    errno=incoming;auto r=[&]{stereo::PrimaryEndScope endScope;return cameraRuntime.endCommand(c);}(); // The driver's End walk runs inside
    const int saved=errno;gameCommands().ended(c,r);
#ifdef RDRVR_HEAD_CAMERA
    dynamicUniformDescriptors.commandEnded(c,r);
    uniformCapture.revoke();uniformArena.commandEnded(c,r);
#endif
    errno=saved;return r;}
VKAPI_ATTR VkResult VKAPI_CALL resetCommand(VkCommandBuffer c,VkCommandBufferResetFlags f){indexedObservation.invalidate(c);const int incoming=errno;
#ifdef RDRVR_HEAD_CAMERA
    nativeQueryGuard.reset(queryHandle(c));
#endif
    errno=incoming;auto r=cameraRuntime.resetCommand(c,f);const int saved=errno;gameCommands().reset(c,r);
#ifdef RDRVR_HEAD_CAMERA
    dynamicUniformDescriptors.commandReset(c,r);
    uniformCapture.revoke();uniformArena.commandReset(c,r);
#endif
    errno=saved;return r;}
VKAPI_ATTR VkResult VKAPI_CALL resetPool(VkDevice d,VkCommandPool p,VkCommandPoolResetFlags f){indexedObservation.invalidateAll();const int incoming=errno;
#ifdef RDRVR_HEAD_CAMERA
    nativeQueryGuard.poolInvalidated(queryHandle(d),queryHandle(p),false);
#endif
    errno=incoming;auto r=cameraRuntime.resetPool(d,p,f);const int saved=errno;gameCommands().poolReset(d,p,r);
#ifdef RDRVR_HEAD_CAMERA
    dynamicUniformDescriptors.poolReset(d,p,r);
    uniformCapture.revoke();uniformArena.poolReset(d,p,r);
#endif
    errno=saved;return r;}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceProc(VkInstance,const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice,const char*);
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue,std::uint32_t,const VkSubmitInfo*,VkFence);
#ifdef RDRVR_HEAD_CAMERA
// ablate (CategoryAblation.h): in front of the camera hooks (proc route below + engine dispatch slots). Off: one relaxed load.
// A dropped draw never reaches cameraDraw* (target-route discard, probe, stereo tape, scene routes): the right eye replays without it.
VKAPI_ATTR void VKAPI_CALL ablateBindPipeline(VkCommandBuffer c,VkPipelineBindPoint p,VkPipeline pipeline){
    // Inside a HIGH/ULTRA eye pass a classified engine pipeline is replaced by its coarse twin BEFORE
    // every other hook sees it (ablate, camera, stereo tape -> the right-eye replay binds the same twin).
    // One relaxed load until a twin exists.
    pipeline=reinterpret_cast<VkPipeline>(lowfreq::bind(reinterpret_cast<std::uintptr_t>(c),p==VK_PIPELINE_BIND_POINT_GRAPHICS,reinterpret_cast<std::uintptr_t>(pipeline)));
    if(ablate::armed())ablate::bindPipeline(reinterpret_cast<std::uintptr_t>(c),p==VK_PIPELINE_BIND_POINT_GRAPHICS,reinterpret_cast<std::uintptr_t>(pipeline));
    cameraBindPipeline(c,p,pipeline);}
// lowfreq: the pass changed since the bind (twin <-> engine pipeline): rebind through the camera / stereo route (tape records it).
inline void lowFreqDraw(VkCommandBuffer c){if(const auto rebind=lowfreq::draw(reinterpret_cast<std::uintptr_t>(c)))cameraBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,reinterpret_cast<VkPipeline>(rebind));}
VKAPI_ATTR void VKAPI_CALL ablateDraw(VkCommandBuffer c,std::uint32_t n,std::uint32_t instances,std::uint32_t first,std::uint32_t base){ // The draw args feed ablate's owner key
    if(ablate::armed()&&ablate::drop(reinterpret_cast<std::uintptr_t>(c),n,instances,first,0,false))return;lowFreqDraw(c);cameraDraw(c,n,instances,first,base);}
VKAPI_ATTR void VKAPI_CALL ablateDrawIndexed(VkCommandBuffer c,std::uint32_t n,std::uint32_t instances,std::uint32_t first,std::int32_t vertex,std::uint32_t base){
    if(ablate::armed()&&ablate::drop(reinterpret_cast<std::uintptr_t>(c),n,instances,first,vertex,true))return;lowFreqDraw(c);cameraDrawIndexed(c,n,instances,first,vertex,base);}
#else
constexpr auto ablateBindPipeline=cameraBindPipeline;constexpr auto ablateDraw=cameraDraw;constexpr auto ablateDrawIndexed=cameraDrawIndexed;
#endif
PFN_vkVoidFunction commandProc(PFN_vkVoidFunction original,const char* name){
    if(!original || !name)return original;
    if(auto route=indexedProc(original,name);route!=original)return route;
    // Suspended admission still retains device/command children. Future
    // lifecycle proc lookups must keep the teardown/submission owner route.
    if(cameraRoutesInstalled.load(std::memory_order_acquire)||cameraOwnerDevice.load(std::memory_order_acquire)){
        if(!std::strcmp(name,"vkDestroyDevice"))return reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
        if(!std::strcmp(name,"vkQueueSubmit"))return reinterpret_cast<PFN_vkVoidFunction>(submit);
    }
    if(cameraRoutesInstalled.load(std::memory_order_acquire)){
        if(!std::strcmp(name,"vkCreatePipelineLayout"))return reinterpret_cast<PFN_vkVoidFunction>(cameraCreatePipelineLayout);
        if(!std::strcmp(name,"vkDestroyPipelineLayout"))return reinterpret_cast<PFN_vkVoidFunction>(cameraDestroyPipelineLayout);
        if(!std::strcmp(name,"vkCmdBindPipeline"))return reinterpret_cast<PFN_vkVoidFunction>(ablateBindPipeline);
        if(!std::strcmp(name,"vkCmdDraw"))return reinterpret_cast<PFN_vkVoidFunction>(ablateDraw);
        if(!std::strcmp(name,"vkCmdDrawIndexed"))return reinterpret_cast<PFN_vkVoidFunction>(ablateDrawIndexed);
    }
#ifdef RDRVR_BENCH_CONTROL
    if(auto shader=diagnostics::shaderCaptureProc(original,name,originalCreateShader,createShader);shader!=original)return shader;
#endif
#ifdef RDRVR_HEAD_CAMERA
    if(nativeQueryHooksReady.load(std::memory_order_acquire)){
        if(!std::strcmp(name,"vkCmdBeginQuery"))return reinterpret_cast<PFN_vkVoidFunction>(observedBeginQuery);
        if(!std::strcmp(name,"vkCmdEndQuery"))return reinterpret_cast<PFN_vkVoidFunction>(observedEndQuery);
    }
    if(coldLifecycleReady.load(std::memory_order_acquire)){
        if(!std::strcmp(name,"vkDestroyShaderModule"))return reinterpret_cast<PFN_vkVoidFunction>(coldDestroyShader);
        if(!std::strcmp(name,"vkDestroyPipeline"))return reinterpret_cast<PFN_vkVoidFunction>(coldDestroyPipeline);
    }
    if(executeLifetimeReady.load(std::memory_order_acquire)&&!std::strcmp(name,"vkCmdExecuteCommands"))
        return reinterpret_cast<PFN_vkVoidFunction>(dynamicExecute);
    if(!std::strcmp(name,"vkCreateBuffer"))return bufferRoute92<PFN_vkCreateBuffer>(original,originalBufferCreate92,bufferCreate92);
    if(!std::strcmp(name,"vkDestroyBuffer"))return bufferRoute92<PFN_vkDestroyBuffer>(original,originalBufferDestroy92,bufferDestroy92);
#endif
    const std::pair<const char*,PFN_vkVoidFunction> routes[]{
        {"vkGetInstanceProcAddr",reinterpret_cast<PFN_vkVoidFunction>(instanceProc)},
        {"vkGetDeviceProcAddr",reinterpret_cast<PFN_vkVoidFunction>(deviceProc)},
#ifdef RDRVR_HEAD_CAMERA
        {"vkCreateDescriptorSetLayout",reinterpret_cast<PFN_vkVoidFunction>(dynamicCreateLayout)},
        {"vkDestroyDescriptorSetLayout",reinterpret_cast<PFN_vkVoidFunction>(dynamicDestroyLayout)},
        {"vkCmdBindDescriptorSets",reinterpret_cast<PFN_vkVoidFunction>(dynamicBind)},
#endif
#ifdef RDRVR_FRAGMENT_RATE
        {"vkCreateRenderPass",reinterpret_cast<PFN_vkVoidFunction>(rateCreatePass)},{"vkDestroyRenderPass",reinterpret_cast<PFN_vkVoidFunction>(rateDestroyPass)},
        {"vkCreateFramebuffer",reinterpret_cast<PFN_vkVoidFunction>(rateCreateFramebuffer)},{"vkCreateGraphicsPipelines",reinterpret_cast<PFN_vkVoidFunction>(rateCreatePipelines)},
        {"vkCreateImageView",reinterpret_cast<PFN_vkVoidFunction>(rateCreateView)},{"vkDestroyImageView",reinterpret_cast<PFN_vkVoidFunction>(rateDestroyView)},
#endif
        {"vkCreateCommandPool",reinterpret_cast<PFN_vkVoidFunction>(createPool)},{"vkDestroyCommandPool",reinterpret_cast<PFN_vkVoidFunction>(destroyPool)},
        {"vkAllocateCommandBuffers",reinterpret_cast<PFN_vkVoidFunction>(allocateCommands)},{"vkFreeCommandBuffers",reinterpret_cast<PFN_vkVoidFunction>(freeCommands)},
        {"vkBeginCommandBuffer",reinterpret_cast<PFN_vkVoidFunction>(beginCommand)},{"vkEndCommandBuffer",reinterpret_cast<PFN_vkVoidFunction>(endCommand)},
        {"vkResetCommandBuffer",reinterpret_cast<PFN_vkVoidFunction>(resetCommand)},{"vkResetCommandPool",reinterpret_cast<PFN_vkVoidFunction>(resetPool)}};
    for(auto& route:routes)if(!std::strcmp(name,route.first))return route.second;
    return original;
}
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice d,const VkImageCreateInfo* c,const VkAllocationCallbacks* a,VkImage* out) {
    const auto result=originalCreateImage(d,c,a,out);const int saved=errno;
#ifdef RDRVR_HEAD_CAMERA
    diagnostics::textureQualityTelemetry.created(d,c,result==VK_SUCCESS&&out?*out:VK_NULL_HANDLE,result);
#endif
    gameImageCreated(d,c,result==VK_SUCCESS && out?*out:VK_NULL_HANDLE,result);errno=saved;return result;
}
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice d,VkImage image,const VkAllocationCallbacks* a) {
    const int saved=errno;gameImageDestroyed(d,image);stereo::imageDestroyed(d,image);
#ifdef RDRVR_FRAGMENT_RATE
    gameFragmentRateBridge().routing().removeImage(d,image);
#endif
    // An image with a deferred view still pending (stereoDestroyView) is deferred with it: the driver's
    // vkEndCommandBuffer walk reads the view's image too. The registries above forgot it already; true = the stereo module
    // calls originalDestroyImage itself after the view (stereo::frameHook / shutdownAfterIdle).
    const bool deferred=stereo::imageDestroyDeferred(d,image,a,originalDestroyImage);
    errno=saved;if(!deferred)originalDestroyImage(d,image,a);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL instanceProc(VkInstance instance,const char* name) {
    auto result=originalInstanceProc(instance,name);return commandProc(imageProc(result,name,createImage,destroyImage),name);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice device,const char* name) {
    auto result=originalDeviceProc(device,name);return commandProc(imageProc(result,name,createImage,destroyImage),name);
}
#else
constexpr unsigned dispatchCount=12;
#endif
PFN_vkQueuePresentKHR originalSwappy{};
std::atomic<bool> swappyInterposed{};
VKAPI_ATTR VkResult VKAPI_CALL timedSwappyPresent(VkQueue q,const VkPresentInfoKHR* p){
    diagnostics::WallScope timing(diagnostics::frameTimings.swappyPresent);return originalSwappy(q,p);
}
VKAPI_ATTR VkResult VKAPI_CALL swappyPresent(VkQueue q,const VkPresentInfoKHR* p){return gamePresentVia(q,p,timedSwappyPresent);}
#else
constexpr unsigned dispatchCount=8;
#endif
std::mutex lock;
std::mutex initLock;
std::string enginePath,status="NOT_CONFIGURED";
std::vector<std::string> events;
diagnostics::VerificationGate verification;
std::atomic<bool> pltInstalled{};
std::atomic<bool> patchFault{};
std::atomic<unsigned> dropped{},initCalls{};
bool entrySeen{},armed{};
void* library{}; // Process lifetime: game retains entry and dispatch pointers.
diagnostics::ThreadMemoryReader memory;
std::uintptr_t engineBase{};
using Init=bool(*)();
Init originalInit{};
diagnostics::VulkanTrace tracer;
std::array<std::uintptr_t,dispatchCount> originalDispatch{};
bool traceConfigured{};
// Guarded (process_vm_readv) reads are noted with their call site (report "guardedReads"),
// only while debug.rdrvr.guardedsites=1 (one relaxed load otherwise).
diagnostics::GuardedReadSites guardedReadSites;
// Plain copies for reads entirely inside the pinned engine's PT_LOAD segments, which
// stay mapped readable for the process lifetime (configureDirectReads). Everything else
// (driver/malloc/stack memory, unknown pointers) stays kernel-checked.
diagnostics::DirectReadRanges directReads;
// Site = (direct caller, its caller). One frame-record step, validated to stay on
// this stack: [fp] = caller's frame record, [callerFp+8] = the caller's return address.
__attribute__((noinline)) void noteGuardedSite(const void* returnAddress,const void* frame,std::size_t size)noexcept{
    const auto pc0=diagnostics::GuardedReadSites::strip(reinterpret_cast<std::uint64_t>(returnAddress));
    std::uint64_t pc1{};const auto fp=reinterpret_cast<std::uintptr_t>(frame);std::uintptr_t callerFp{};
    std::memcpy(&callerFp,reinterpret_cast<const void*>(fp),sizeof callerFp);
    if(callerFp>fp&&callerFp-fp<(std::uintptr_t(1)<<20)&&!(callerFp&15)){
        std::memcpy(&pc1,reinterpret_cast<const void*>(callerFp+8),sizeof pc1);pc1=diagnostics::GuardedReadSites::strip(pc1);}
    guardedReadSites.note(pc0,pc1,size);
}
__attribute__((noinline)) bool readMemory(void*,std::uintptr_t p,void* target,std::size_t size) {
    auto& work=diagnostics::guardedReads;
    if(target&&size&&size<=65536&&directReads.contains(p,size)){
        std::memcpy(target,reinterpret_cast<const void*>(p),size);++work.direct;return true;}
    if(guardedReadSites.enabled())noteGuardedSite(__builtin_return_address(0),__builtin_frame_address(0),size);
    ++work.calls;++work.segments;work.bytes+=size;
    const bool good=memory.read(p,target,size);if(!good)++work.failures;return good;
}
// Vectored form of readMemory (registered in diagnostics::guardedReadVector). Direct
// segments are copied; the rest go in one process_vm_readv per 64 segments. All-or-nothing.
__attribute__((noinline)) bool readMemoryVector(const diagnostics::ReadSegment* segments,std::size_t count) {
    if(!count)return true;if(!segments)return false;
    auto& work=diagnostics::guardedReads;
    std::array<diagnostics::ReadSegment,64> guarded;std::size_t pending{},bytes{};
    const void* returnAddress=__builtin_return_address(0);const void* frame=__builtin_frame_address(0);
    const auto flush=[&]{
        if(!pending)return true;
        if(guardedReadSites.enabled())noteGuardedSite(returnAddress,frame,bytes);
        ++work.calls;work.segments+=pending;work.bytes+=bytes;
        const bool good=memory.readv(guarded.data(),pending);if(!good)++work.failures;
        pending=bytes=0;return good;
    };
    for(std::size_t i=0;i<count;++i){const auto& s=segments[i];
        if(s.target&&s.size&&s.size<=65536&&directReads.contains(s.source,s.size)){
            std::memcpy(s.target,reinterpret_cast<const void*>(s.source),s.size);++work.direct;continue;}
        guarded[pending++]=s;bytes+=s.size;
        if(pending==guarded.size()&&!flush())return false;
    }
    return flush();
}
// The pinned engine's readable PT_LOAD segments (its 1.2 GB static heap is .bss),
// published only when /proc/self/maps shows each one fully mapped readable. The pinned
// librdr imports no mprotect/madvise; the mod only adds PROT_WRITE to GOT pages.
bool configureDirectReads(std::uintptr_t base)noexcept{
    using Range=diagnostics::DirectReadRanges::Range;
    try{
        struct Search{std::uintptr_t base;std::vector<Range> ranges;bool found;} search{base,{},false};
        dl_iterate_phdr([](dl_phdr_info* info,std::size_t,void* data)->int{
            auto& s=*static_cast<Search*>(data);if(info->dlpi_addr!=s.base)return 0;
            s.found=true;
            for(int i=0;i<info->dlpi_phnum;++i){const auto& ph=info->dlpi_phdr[i];
                if(ph.p_type==PT_LOAD&&(ph.p_flags&PF_R)&&ph.p_memsz)s.ranges.push_back({s.base+ph.p_vaddr,s.base+ph.p_vaddr+ph.p_memsz});}
            return 1;},&search);
        if(!search.found||search.ranges.empty()||search.ranges.size()>diagnostics::DirectReadRanges::capacity)return false;
        std::vector<Range> readable;
        FILE* maps=std::fopen("/proc/self/maps","r");if(!maps)return false;
        char line[1024];
        while(std::fgets(line,sizeof(line),maps)){
            unsigned long long b{},e{};char perms[5]{};
            if(std::sscanf(line,"%llx-%llx %4s",&b,&e,perms)==3&&perms[0]=='r')readable.push_back({std::uintptr_t(b),std::uintptr_t(e)});
        }
        std::fclose(maps);
        for(const auto& r:search.ranges)if(!diagnostics::DirectReadRanges::covered(r.begin,r.end,readable))return false;
        return directReads.publish(search.ranges);
    }catch(...){return false;}
}
// Native occluder quads; active only in STEREO TEST modes
// stereo::occluderTestMode ("OCCLUDER A/B TEST") and stereo::occluderAlwaysMode ("OCCLUDERS ON").
OccluderInjection occluderInjection;
// Early small-object culling at DrawCommon (GRAPHICS -> SMALL OBJECTS, debug.rdrvr.smallobjects=<px>).
SmallObjectCull smallObjectCull;
std::uint64_t now() {return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
std::uint64_t tid() {return gettid();}
#ifdef RDRVR_TOUCH_CONTROLS
InputCallBoundary inputBoundary;std::atomic<bool> inputInterposed{};
ReticleCallBoundary reticleBoundary;std::atomic<bool> reticleInterposed{};
WeaponAimBoundary weaponAimBoundary;WeaponPoseCallBoundary weaponPoseBoundary;WeaponManagerBoundary weaponManagerBoundary;
std::atomic<bool> weaponAimInterposed{},weaponPoseInterposed{},weaponManagerInterposed{};
#ifdef RDRVR_HEAD_CAMERA
ReticleUiCallBoundary reticleUiBoundary;std::atomic<bool> reticleUiInterposed{};
void observeReticleUi(void*,void* object,bool complete){gameObserveReticleUi(object,complete,now());}
void paintReticleUi(void* object){reticleUiBoundary.paint(object);}
ProjectileLaunchBoundary projectileLaunchBoundary;std::atomic<bool> projectileLaunchInterposed{};
void observeProjectileLaunch(void*,const ProjectileLaunchCall& call,const WeaponFireScope& scope,bool complete){gameObserveProjectileLaunch(call,scope,complete,now());}
__attribute__((noinline)) void launchProjectile(void* projectile,const WeaponNativeMatrix* world,int weaponType,const AimNativeVector* velocity,
        std::uint32_t actorGuid,const AimNativeVector* muzzle,bool nativeFlag,bool finalFlag,float scalar,const AimNativeVector* delta){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    projectileLaunchBoundary.launch({projectile,world,weaponType,velocity,actorGuid,muzzle,nativeFlag,finalFlag,scalar,delta},
        caller==engineBase+projectileLaunchReturnOffset);
}
WeaponModelCallBoundary propModelBoundary,artModelBoundary;std::atomic<bool> weaponModelInterposed{};
bool prepareWeaponModel(void*,const WeaponModelCall& call,WeaponNativeMatrix& matrix){return gamePrepareWeaponModel(call,matrix,now());}
void afterWeaponModel(void*,const WeaponModelCall& call,bool prepared,bool result)noexcept{gameWeaponModelComplete(call,prepared,result);}
__attribute__((noinline)) bool transformWeaponProp(void* model,const WeaponNativeMatrix* matrix){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    return propModelBoundary.transform({model,matrix},caller==engineBase+weaponModelReturnOffset);
}
__attribute__((noinline)) bool transformWeaponArtProp(void* model,const WeaponNativeMatrix* matrix){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    return artModelBoundary.transform({model,matrix},caller==engineBase+weaponModelReturnOffset);
}
#endif
NativeRumbleBoundary rumbleBoundary;std::atomic<bool> rumbleInterposed{};
WeaponFireBoundary weaponFireBoundary;std::atomic<bool> weaponFireInterposed{};
void observeWeaponFire(void*,const WeaponFireCall& call,bool complete){gameObserveWeaponFire(call,complete,now(),tid());}
void fireProjectiles(void* weapon,void* projectile,int count,const WeaponNativeMatrix* matrices,float scalar,int mode,bool nativeFlag,int nativeValue,bool finalFlag){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const bool pinned=caller==engineBase+weaponFireAnimationReturnOffset||caller==engineBase+weaponFireModeReturnOffset||caller==engineBase+weaponFireExplodingReturnOffset;
    weaponFireBoundary.fire({weapon,projectile,count,matrices,scalar,mode,nativeFlag,nativeValue,finalFlag},pinned);
}
void observeRumble(void*,void* pad,int actuator,float value){gameObserveRumble(pad,actuator,value,now());}
void setActuator(void* pad,int actuator,float value){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    rumbleBoundary.invoke(pad,actuator,value,caller==engineBase+rumbleReturnOffset);
}
void afterReticle(void* reticle)noexcept{gameObserveReticle(reticle,now(),tid());}
#ifdef RDRVR_HEAD_CAMERA
__attribute__((noinline)) void nativeDisplayWait(void* display) noexcept {
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const bool qualified=caller==engineBase+framePacingThreadSiteReturnOffset||caller==engineBase+framePacingSwapSiteReturnOffset;
    nativeFramePacing.invoke(display,qualified,qualified&&gameNativeFramePacingAllowed());
}
__attribute__((noinline)) void nativeUpdateTime(void* shell) noexcept {
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const bool qualified=caller==engineBase+framePacingTimeSiteReturnOffset;
    nativeFramePacing.invokeTime(shell,qualified,qualified&&gameNativeFramePacingAllowed());
}
WeaponOwnerApi holsterOwnerApi{}; // Copy of the configured weapon-observation api for the holster context reader
bool holsterContext(void*,std::uintptr_t player,std::uintptr_t actor,std::uint32_t guid,vr::holsters::PlayerBlocks& out) noexcept {
    LocalWeapon p{};p.player=player;p.actor=actor;p.actorGuid=guid;WeaponContext c{};
    if(!WeaponContextReader(holsterOwnerApi).inspectPlayer(p,c))return false;
    out.alive=c.alive;out.controlDisabled=c.controlDisabled;out.deadEye=c.deadEye;out.riding=c.riding;out.ridingVehicle=c.ridingVehicle;
    out.vehicleTransition=c.vehicleTransition;out.duel=c.duel;out.mountedWeapon=c.mountedWeapon;out.mountingHorse=c.mountingHorse;
    out.blindFiring=c.blindFiring;out.cover=c.cover;out.linkedAnimation=c.linkedAnimation;out.usingGringo=c.usingGringo;return true;
}
// sagShellDerived::UpdateInputs (GOT 0x2e499c0). The original runs first and unchanged; only the call from
// sagShellDerived::Execute (main thread, after PerformDMZ/WaitForRenderThread, before the next simulation step)
// samples the player and executes saves/teleports (Teleport.h).
// The same qualified call drains the VR main-thread queue (holsters, cheats; VrMainThread.h). The wrapper is installed
// on its own fingerprints (installUpdateInputs in arm), not on the teleport/weapon ones; teleport::gameFrame is inert while
// teleport is not configured.
using NativeUpdateInputs=void(*)(void*);NativeUpdateInputs nativeUpdateInputsOriginal{};
__attribute__((noinline)) void nativeUpdateInputs(void* self) noexcept {
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    nativeUpdateInputsOriginal(self);
    const bool qualified=caller==engineBase+teleport::updateInputsReturnOffset;
    teleport::gameFrame(qualified);
    vr::mainthread::run(qualified,vr::nowNs()); // Holsters (and cheats) on the engine main thread
    gameclock::gameFrame(qualified); // Game clock (sagTimeOfDayManager): debug.rdrvr.clock / clock.freeze
}
PipelinePassHashMemo::Original nativePassHash{};
using DynamicLayoutFn=bool(*)(void*,const void*,const void*);
using DynamicInputFn=bool(*)(void*);
DynamicLayoutFn nativeDynamicLayout{};DynamicInputFn nativeDynamicInput{};
DynamicUniformDescriptors::NativeOriginal nativeDynamicSet{};
rdr92::VmaUniformLife uniformLife92(dynamicUniformDescriptors);
__attribute__((noinline)) VkResult vmaCreate92(void* allocator,const VkBufferCreateInfo* ci,const void* ac,VkBuffer* buffer,void** allocation,void* info){
    return uniformLife92.create(allocator,ci,ac,buffer,allocation,info);
}
__attribute__((noinline)) VkResult vmaAligned92(void* allocator,const VkBufferCreateInfo* ci,const void* ac,VkDeviceSize alignment,VkBuffer* buffer,void** allocation,void* info){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    return uniformLife92.aligned(allocator,ci,ac,alignment,buffer,allocation,info,caller);
}
__attribute__((noinline)) void vmaDestroy92(void* allocator,VkBuffer buffer,void* allocation){uniformLife92.destroy(allocator,buffer,allocation);}
bool descriptor92StartupRequested()noexcept{
    // descriptor-set retention is on by default; debug.rdrvr.descriptor92=0 opts out.
    char value[PROP_VALUE_MAX]{};__system_property_get("debug.rdrvr.descriptor92",value);
    return !(value[0]=='0'&&value[1]==0);
}
bool dynamicLayout(void* layout,const void* vertex,const void* pixel){
    DynamicUniformDescriptors::LayoutScope scope(dynamicUniformDescriptors,
        dynamicNativeHooksReady.load(std::memory_order_acquire)&&dynamicUniformDescriptors.nativeLayoutEligible(vertex,pixel));
    return nativeDynamicLayout(layout,vertex,pixel);
}
__attribute__((noinline)) bool dynamicInput(void* context){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    DynamicUniformDescriptors::DrawScope scope(dynamicUniformDescriptors,context,dynamicNativeHooksReady.load(std::memory_order_acquire));
    if(!scope.good())return false;
    using VertexScope58=decltype(nativeVertexStreamMerge58)::Scope;
    VertexScope58 vertices58(nativeVertexStreamMerge58,context,engineBase+0x18af7d8,nativeVertexInputReady58.load(std::memory_order_acquire),experimental::renderAblation54.vertexMergeAllowed());
    const bool result=nativeDynamicInput(context);
    // On failure the scope restores native dirty flags for the next attempt;
    // no primitive may consume an incomplete converted descriptor binding.
    const bool good=result&&scope.good();
    if(good&&experimental::NativeEarlyTraceRoute::source()){
        const bool exact=std::find(nativeOcclusionWitnessInputReturns.begin(),nativeOcclusionWitnessInputReturns.end(),caller-engineBase)!=nativeOcclusionWitnessInputReturns.end();
        DynamicUniformDescriptors::UniformBinding b{};scope.matrixBinding(b);
        experimental::NativeEarlyTraceRoute::prepared(context,{reinterpret_cast<std::uintptr_t>(b.command),reinterpret_cast<std::uintptr_t>(b.descriptor),
            reinterpret_cast<std::uintptr_t>(b.buffer),b.offset,b.range,b.valid},exact);
    }
    return good;
}
std::uintptr_t dynamicSet(void* context,void* pool,const void* layout,const void* constants,void* vertex,void* pixel,int type){
    return dynamicUniformDescriptors.nativeSet(nativeDynamicSet,context,pool,layout,constants,vertex,pixel,type);
}
__attribute__((noinline)) std::uint32_t memoPassHash(const void* data,std::size_t size,std::uint32_t seed){
    const auto pc=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    return pipelinePassHashMemo.invoke(nativePassHash,data,size,seed,pc==engineBase+passHashSiteReturnOffset);
}

#ifdef RDRVR_HEAD_CAMERA
__attribute__((noinline)) void instanceReset67(void* manager,bool flag){
    rdr95::ResourceOriginalErrno nativeError;
    const bool lease=experimental::instanceFrequencyReady67.load(std::memory_order_acquire)&&
        experimental::renderAblation54.instanceFrequencyRequested();
    errno=nativeError.value;
    try{experimental::instanceFrequency67.reset(manager,flag,lease);}
    catch(...){nativeError.value=errno;throw;}
    nativeError.value=errno;
}
__attribute__((noinline)) int instanceClear67(void* container,bool flag){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    return experimental::instanceFrequency67.clear(container,flag,
        experimental::instanceFrequencyReady67.load(std::memory_order_acquire)&&caller==engineBase+0x2abced8);
}
__attribute__((noinline)) int instanceStart67(void* manager,int threshold){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    return experimental::instanceFrequency67.start(manager,threshold,
        experimental::instanceFrequencyReady67.load(std::memory_order_acquire)&&caller==engineBase+0x2a298d4);
}
using AblationModelDraw54=void(*)(const void*,const void*,int,int);
AblationModelDraw54 originalAblationModel54{};
__attribute__((noinline)) void ablationModelDraw54(const void* model,const void* group,int bucket,int flags){
    if(!experimental::renderAblation54.armed()&&!experimental::nativeModelProbe69.liveEyes()){originalAblationModel54(model,group,bucket,flags);return;} // No thread_local reads per model draw
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const bool exact=(caller==engineBase+0x1d9d608||caller==engineBase+0x1d9d7ec)&&experimental::renderAblation54.selectedPass();
    experimental::nativeModelProbe69.model(model,caller==engineBase+0x1d9d608?0u:1u,exact);
    if(experimental::renderAblation54.model(exact))return;
    originalAblationModel54(model,group,bucket,flags);
}
#endif
__attribute__((noinline)) void bodyDraw(void* container,const void* info,int commands,int instance,float fade,unsigned flags,unsigned extra){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const bool pinned=caller==engineBase+bodyOpaqueReturnOffset||caller==engineBase+bodyAlphaReturnOffset||
        caller==engineBase+bodyMain0ReturnOffset||caller==engineBase+bodyMain1ReturnOffset;
#ifdef RDRVR_HEAD_CAMERA
    // PLAYER SHADOW WINDOW (SunStagger.h): player sphere observation; in the window tile, objects off the window are skipped
    if(shadowupdate::stagger::playerDraw(info,pinned))return;
    shadowupdate::casterDraw(); // Caster draws per atlas tile (one relaxed load outside the exact atlas render)
    lampshadow::casterDraw();   // Caster draws per lamp shadow slot (one relaxed load outside an exact DrawShadow)
#endif
    // Measure (and optionally skip) small MAIN objects in the exact stock depth/colour
    // stages before any other draw hook sees them; end() runs for every begin().
    const auto smallToken=smallObjectCull.begin(container,info,pinned);
    struct SmallEnd{const SmallObjectCull::Token& token;~SmallEnd(){smallObjectCull.end(token);}} smallEnd{smallToken};
    if(smallToken.skip)return;
    const auto eyeCull=eyecull::decide(smallToken,info,instance,gameEyeViewports); // EYE CULL A (EyeCull.h): count per eye; lever bit 0x400 skips outside both
    if(eyeCull.skip)return;
    const eyecull::tape::Scope eyeCullPad(eyeCull.pad,eyeCull.scoped); // EYE CULL B: per-eye pad of this object's draws (EyeCullTape.h)
    if(pinned)occluderInjection.body(); // Per-frame DrawCommon count (one relaxed add)
#ifdef RDRVR_HEAD_CAMERA
    // Ablation, scene-route tape and early route are gated by process-wide flags, so
    // the common path touches no thread_local here (selectedPass/selectedMain only when armed).
    if(experimental::renderAblation54.armed()&&experimental::renderAblation54.body(pinned&&experimental::renderAblation54.selectedPass()))return;
    ablate::ObjectScope ablateObject(info,instance,gameEyeViewports); // Ablate: object bits for this DrawCommon's draws (one relaxed load when off)
    experimental::NativeEarlyTraceRoute::OpaqueDraw run(reinterpret_cast<std::uintptr_t>(container),
        caller>=engineBase?caller-engineBase:0);
#endif
    rdr95::scene_routes::Body routeBody(reinterpret_cast<std::uintptr_t>(container),info,caller,commands,instance,fade,flags,extra,pinned,
        rdr95::scene_routes::state.load(std::memory_order_relaxed)==1&&rdr95::CameraMainRoute::selectedMain());
    if constexpr(ordinaryMainStartupRequested()){rdr95::ordinary_integration::Body ordinaryBody(container,info,pinned);
        gameBodyDraw(container,info,commands,instance,fade,flags,extra,pinned);}
    else gameBodyDraw(container,info,commands,instance,fade,flags,extra,pinned);
}
#endif
void observeWeaponManager(void*,void* weapon,bool complete){gameObserveWeaponManager(weapon,complete,now(),tid());}
__attribute__((noinline)) void updateWeaponManager(void* weapon){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    weaponManagerBoundary.update(weapon,caller==engineBase+weaponManagerReturnOffset);
}
bool prepareWeaponAim(void*,const WeaponAimCall& call,WeaponAimVector& point){
    const auto stamp=now();gameObserveWeaponAim(call,stamp,tid());return gamePrepareWeaponAim(call,point,stamp);
}
void afterWeaponAim(void*,bool prepared)noexcept{gameWeaponAimComplete(prepared);}
bool observeWeaponPose(void*,const WeaponPoseCall& call,WeaponNativeMatrix& matrix){
    const auto stamp=now();gameObserveWeaponPose(call,stamp,tid());return gamePrepareWeaponPose(call,matrix,stamp);
}
void afterWeaponPose(void*,const WeaponPoseCall& call,bool prepared)noexcept{gameObserveWeaponPoseComplete(call,prepared,now(),tid());}
__attribute__((noinline)) void updateWeaponPose(void* weapon,const WeaponNativeMatrix* matrix,const AimNativeVector* velocity,bool flag,float scalar){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    weaponPoseBoundary.update({weapon,matrix,velocity,flag,scalar},caller==engineBase+weaponPoseReturnOffset);
}
__attribute__((noinline)) void aimWeapon(void* weapon,const WeaponAimVector* target,bool flag,std::uint32_t guid,const void* intended,int mode,
                                      const WeaponAimVector* velocity,bool aimer,std::uint16_t instance,std::uint16_t part,int index){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    weaponAimBoundary.aim({weapon,target,flag,guid,intended,mode,velocity,aimer,instance,part,index},caller==engineBase+weaponAimReturnOffset);
}
__attribute__((noinline)) void updateReticle(void* reticle,bool nativeFlag){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    reticleBoundary.update(reticle,nativeFlag,caller==engineBase+weaponReticleReturnOffset);
}
__attribute__((noinline)) void updateInput(void* system,float dt){
    const int incoming=errno;const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const auto timestamp=now();errno=incoming;
    inputBoundary.update(system,dt,caller==engineBase+inputUpdateReturnOffset,timestamp);
}
#endif
#ifdef RDRVR_CAMERA_BRIDGE
#ifdef RDRVR_NATIVE_MENUS
HudCallBoundary hudBoundary;HudFrameBoundary hudFrameBoundary;std::atomic<bool> hudInterposed{};
using HudDraw=void(*)(void*,const void*);
using HudUiDraw=void(*)(void*,const void*,float,unsigned);
using HudRadarDraw=void(*)(void*);
using HudBegin=void(*)();
HudRadarDraw originalHudFrame{},originalHudEnd{};HudBegin originalHudBegin{};
HudBegin originalHudRenderFlush{};HudRoutingApi::Bind originalHudRenderBind{};
__attribute__((noinline)) void hudRenderFlush(){
    // Every DrawIndexedPrimitive/DrawPrimitive flushes through here; outside the HUD
    // pass no routing is live on any thread: straight to the original (no TLS, no LR test).
    if(!HudDrawRouting::live()){
        // debug.rdrvr.vr.hud.alpha 1 only (one relaxed load otherwise): coverage alpha for draws into the cleared HUD target.
        if(::rdrvr::vr::hud::alphaLive()){
            const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
            ::rdrvr::vr::hud::alphaFlush(originalHudRenderFlush,caller!=engineBase+hudClearFlushReturnOffset);return;}
        originalHudRenderFlush();return;}
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    HudDrawRouting::flush(originalHudRenderFlush,caller!=engineBase+hudClearFlushReturnOffset);
}
bool hudRenderBind(void* gfx,void* framebuffer){
    const bool result=HudDrawRouting::bind(originalHudRenderBind,gfx,framebuffer);
#ifdef RDRVR_HEAD_CAMERA
    gamePrimaryNativeFramebufferBound(gfx,framebuffer,result);
#endif
    return result;
}
HudDraw originalHudInventory{},originalHudFlash{};HudUiDraw originalHudUi{};HudRadarDraw originalHudRadar{};
template<std::size_t N> bool hudCaller(const std::array<std::uintptr_t,N>& returns,std::uintptr_t caller){
    if(!hudInterposed.load(std::memory_order_acquire))return false;
    for(auto offset:returns)if(caller==engineBase+offset)return true;
    return false;
}
void closeHudClearAlphaDrop() noexcept; // Defined below, with the PIX hooks
__attribute__((noinline)) void hudFrame(void* renderer){
    struct ClearAlphaGuard {~ClearAlphaGuard(){closeHudClearAlphaDrop();}} clearAlphaGuard; // Never leaks past RenderFrame
#ifdef RDRVR_PX2D
    px2d::Frame px2dFrame; // PX 2D: per-RenderFrame gate; closes every 2D drop span at the exit (and an outer one before a nested frame)
#endif
    hudFrameBoundary.frame(hudInterposed.load(std::memory_order_acquire),renderer,originalHudFrame);
}
__attribute__((noinline)) void hudBegin(){
#ifdef RDRVR_PX2D
    // PX 2D (debug.rdrvr.px2d): bit 1 lets the dormant HUD producer leave its eager flush to the next native Clear/draw, then (after
    // the producer's Begin) the gate opens the HUD/UI + ClearAlpha drop span (closed at MoviePlayer::DrawUpdate); the gate also serves gamma.
    const bool exact=hudCaller(hudBeginReturns,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
    const unsigned px2dBits=exact?px2d::want():0u;
    if(px2dBits)px2d::armProducer(px2dBits);
    hudFrameBoundary.begin(exact,originalHudBegin);
    if(px2dBits)px2d::hudBegin(px2dBits);
#else
    hudFrameBoundary.begin(hudCaller(hudBeginReturns,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))),originalHudBegin);
#endif
}
__attribute__((noinline)) void hudEnd(void* object){
#ifdef RDRVR_PX2D
    const bool exact=hudCaller(hudEndReturns,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
    hudFrameBoundary.end(exact,originalHudEnd,object);
    px2d::hudEnd(exact); // TouchUI::Render (inside the span) returned; the exact one keeps the span over ClearAlpha
#else
    hudFrameBoundary.end(hudCaller(hudEndReturns,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))),originalHudEnd,object);
#endif
}
#ifdef RDRVR_PX2D
// PX 2D bit 1: MoviePlayer::DrawUpdate(bool) closes the HUD/UI + ClearAlpha drop span on entry (any caller; it and MoviePlayer::Draw
// stay native: decoded video may feed in-world cinema screens). x0 / w1 and the x0 result pass through unchanged.
std::uintptr_t px2dMovieOriginal{};
__attribute__((noinline)) std::uintptr_t px2dMovieUpdate(void* self,bool flag){
    px2d::movieUpdate(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+px2d::movieReturnOffset);
    return reinterpret_cast<std::uintptr_t(*)(void*,bool)>(px2dMovieOriginal)(self,flag);
}
// PX 2D bit 2: grPostFX::Apply360Gamma(this) runs natively; in gated frames its "#Final Gamma Adjust" TWODBlit is bind-only + no-flush.
std::uintptr_t px2dGammaOriginal{};
__attribute__((noinline)) void px2dGamma(void* self){
    px2d::Gamma scope(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+px2d::gammaReturnOffset,self);
    reinterpret_cast<void(*)(void*)>(px2dGammaOriginal)(self);
}
#endif
__attribute__((noinline)) void hudInventory(void* object,const void* viewport){
    hudBoundary.call(HudStage::Inventory,hudCaller(hudInventoryReturns,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))),originalHudInventory,object,viewport);
}
__attribute__((noinline)) void hudFlash(void* object,const void* viewport){
    hudBoundary.call(HudStage::Flash,hudCaller(hudFlashReturns,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))),originalHudFlash,object,viewport);
}
__attribute__((noinline)) void hudUi(void* object,const void* viewport,float dt,unsigned flags){
    hudBoundary.call(HudStage::Ui,hudCaller(hudUiReturns,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))),originalHudUi,object,viewport,dt,flags);
}
__attribute__((noinline)) void hudRadar(void* object){
    hudBoundary.call(HudStage::Radar,hudCaller(hudRadarReturns,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))),originalHudRadar,object);
}
// Right after TouchUI::Render, sagRenderer::RenderFrame runs PIX "ClearAlpha" (PIXBegin bl @0x2a58a24 .. PIXEnd bl
// @0x2a58bbc): grcState 16 (colour write) = 8 (A only) and a fullscreen grcBegin/grcVertex x3/grcEnd triangle with Color32 -1, i.e.
// alpha = 1 over the whole HUD target. In frames whose HUD the VR HUD cleared and recorded (hud::takeClearAlpha) the triangle's
// vkCmdDraw is dropped through the postfx bind-only draw path (pipeline / descriptors / FlushFrameBuffer stay native), so the HUD
// keeps its coverage alpha for the ring copy at submit. Exact return addresses; every other PIX call passes straight through.
inline constexpr std::uintptr_t pixBeginOffset=0x1a925c4,pixBeginGotOffset=0x2e1d6f8,pixEndOffset=0x1a9291c,pixEndGotOffset=0x2e1d708;
inline constexpr std::uintptr_t clearAlphaBeginReturnOffset=0x2a58a28,clearAlphaEndReturnOffset=0x2a58bc0;
inline constexpr std::array<unsigned char,16> pixBeginFingerprint{
#include "gamecode/TraceEntry_1a9250c_16_8d.inc"
};
inline constexpr std::array<unsigned char,16> pixEndFingerprint{
#include "gamecode/TraceEntry_1a9291c_16_8d.inc"
};
inline constexpr std::array<unsigned char,4> clearAlphaBeginCall{195,69,7,148},clearAlphaEndCall{101,69,7,148};
using PixBeginFn=void(*)(unsigned,const char*);using PixEndFn=void(*)();
PixBeginFn originalPixBegin{};PixEndFn originalPixEnd{};
thread_local bool hudClearAlphaDrop{};thread_local std::uint64_t hudClearAlphaBefore{};
void closeHudClearAlphaDrop() noexcept {
    if(!hudClearAlphaDrop)return;
    hudClearAlphaDrop=false;--postfx::threadScopes;postfx::liveScopes.fetch_sub(1,std::memory_order_relaxed);
    ::rdrvr::vr::hud::clearAlphaDone(postfx::threadDropped-hudClearAlphaBefore); // 0 dropped = the alpha=1 triangle drew: that HUD frame is not published
}
__attribute__((noinline)) void hudPixBegin(unsigned color,const char* name){
    originalPixBegin(color,name);
    // (perf session 3S profile): the return-address compare first - the thread_local read is a dynamic TLS resolve per draw
    if(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+clearAlphaBeginReturnOffset&&!hudClearAlphaDrop&&
       postfx::ready.load(std::memory_order_relaxed)&&::rdrvr::vr::hud::takeClearAlpha()){
        hudClearAlphaDrop=true;hudClearAlphaBefore=postfx::threadDropped;++postfx::threadScopes;postfx::liveScopes.fetch_add(1,std::memory_order_relaxed);}
}
__attribute__((noinline)) void hudPixEnd(){
    if(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+clearAlphaEndReturnOffset&&hudClearAlphaDrop)closeHudClearAlphaDrop();
    originalPixEnd();
}
// RADAR FIX: CRadar::DrawRadar (0x2a1a14c) computes the minimap window in its 720-high virtual layout
// (1374x720 = grcDevice::sm_GlobalWidth/Height) and passes it as PIXELS to grcViewport::SetWindow
// (bl @0x2a1aa48) and grcDevice::SetScissor (bl @0x2a1aa9c), which divide by / clamp to
// grcDevice::sm_Width/Height = the locked VR HUD target (1680x1760). Unscaled, terrain and blips land
// squeezed into the top-left and the terrain (blend SRC=DESTALPHA) misses the alpha circle mask.
// Inside a VR HUD interval those two calls get the window scaled by sm_Width/sm_GlobalWidth,
// sm_Height/sm_GlobalHeight (exact return addresses; every other caller passes through).
// debug.rdrvr.vr.hud.radarfix 0 = off.
inline constexpr std::uintptr_t radarSetWindowOffset=0x1afe848,radarSetWindowGotOffset=0x2e22780,radarSetWindowReturnOffset=0x2a1aa4c;
inline constexpr std::uintptr_t radarSetScissorOffset=0x1abaaf8,radarSetScissorGotOffset=0x2e21388,radarSetScissorReturnOffset=0x2a1aaa0;
inline constexpr std::uintptr_t deviceWidthOffset=0x2e73698,deviceGlobalWidthOffset=0x2e736a0;
inline constexpr std::array<unsigned char,16> radarSetWindowFingerprint{
#include "gamecode/TraceEntry_183963c_16_8d.inc"
};
inline constexpr std::array<unsigned char,16> radarSetScissorFingerprint{
#include "gamecode/TraceEntry_185d404_16_8d.inc"
};
inline constexpr std::array<unsigned char,4> radarSetWindowCall{254,101,8,148},radarSetScissorCall{237,91,8,148};
using RadarSetWindowFn=void(*)(void*,int,int,int,int,float,float,bool);using RadarSetScissorFn=void(*)(int,int,int,int);
RadarSetWindowFn originalRadarSetWindow{};RadarSetScissorFn originalRadarSetScissor{};
std::atomic<std::uint64_t> radarFixWindows{},radarFixScissors{},radarFixSkipped{};std::atomic<int> radarFixDims[4]{};std::atomic<bool> radarFixHooked{};
bool radarFixScale(float& sx,float& sy) noexcept {
    static unsigned calls=0;static bool lever=true;
    if((calls++&255u)==0){char v[PROP_VALUE_MAX]{};lever=!(__system_property_get("debug.rdrvr.vr.hud.radarfix",v)>0&&v[0]=='0');}
    std::int32_t d[2]{},g[2]{};
    // (user: the map was sometimes squashed, then fine): no hud::active() gate - frames outside the VR HUD interval (transitions
    // the post-cutscene delay) drew the radar unscaled. The device / virtual size ratio alone decides (equal sizes = no scaling).
    if(!lever||!readMemory(nullptr,engineBase+deviceWidthOffset,d,sizeof d)||!readMemory(nullptr,engineBase+deviceGlobalWidthOffset,g,sizeof g)||
       g[0]<64||g[1]<64||d[0]<64||d[1]<64){radarFixSkipped.fetch_add(1,std::memory_order_relaxed);return false;}
    radarFixDims[0].store(d[0],std::memory_order_relaxed);radarFixDims[1].store(d[1],std::memory_order_relaxed);
    radarFixDims[2].store(g[0],std::memory_order_relaxed);radarFixDims[3].store(g[1],std::memory_order_relaxed);
    sx=float(d[0])/float(g[0]);sy=float(d[1])/float(g[1]);
    return sx>.25f&&sx<8.f&&sy>.25f&&sy<8.f&&(std::fabs(sx-1.f)>1e-3f||std::fabs(sy-1.f)>1e-3f);
}
// TILES INDOORS (user: inside buildings the minimap went grey): the game's script hides the radar map tiles indoors
// (hudScript::SetRadarTilesVisible(false) -> CRadar::bTilesVisible, copied to bTilesVisibleDraw 0x2f76277 in CRadar::PerformDMZ);
// DrawRadar reads bTilesVisibleDraw after this SetWindow (@0x2a1ab58). Set to 1 here = the town map stays visible indoors (the
// special interior maps, radar mode 1 / 3, take their own branch). debug.rdrvr.vr.hud.tiles 0 = the stock behaviour.
inline constexpr std::uintptr_t radarTilesVisibleDrawOffset=0x2f76277;
std::atomic<std::uint64_t> radarTilesForced{};
__attribute__((noinline)) void radarSetWindow(void* vp,int x,int y,int w,int h,float zmin,float zmax,bool flag){
    float sx{},sy{};
    if(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+radarSetWindowReturnOffset){
        static unsigned calls=0;static bool tiles=true;
        if((calls++&255u)==0){char v[PROP_VALUE_MAX]{};tiles=!(__system_property_get("debug.rdrvr.vr.hud.tiles",v)>0&&v[0]=='0');}
        auto* flagByte=reinterpret_cast<volatile std::uint8_t*>(engineBase+radarTilesVisibleDrawOffset);
        if(tiles&&!*flagByte){*flagByte=1;radarTilesForced.fetch_add(1,std::memory_order_relaxed);}
    }
    if(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+radarSetWindowReturnOffset&&radarFixScale(sx,sy)){
        const int x1=int(std::lround(float(x+w)*sx)),y1=int(std::lround(float(y+h)*sy));
        x=int(std::lround(float(x)*sx));y=int(std::lround(float(y)*sy));w=x1-x;h=y1-y;radarFixWindows.fetch_add(1,std::memory_order_relaxed);}
    originalRadarSetWindow(vp,x,y,w,h,zmin,zmax,flag);
}
__attribute__((noinline)) void radarSetScissor(int x,int y,int w,int h){
    float sx{},sy{};
    if(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+radarSetScissorReturnOffset&&radarFixScale(sx,sy)){
        const int x1=int(std::lround(float(x+w)*sx)),y1=int(std::lround(float(y+h)*sy));
        x=int(std::lround(float(x)*sx));y=int(std::lround(float(y)*sy));w=x1-x;h=y1-y;radarFixScissors.fetch_add(1,std::memory_order_relaxed);}
    originalRadarSetScissor(x,y,w,h);
}
// The terrain tiles (CRadar::DrawRadarMap, bl @0x2a1ab78 inside DrawRadar) blend colour as
// SRC=DESTALPHA,DST=ONE but also overwrite the destination ALPHA with their own (~0.5); stock
// RenderFrame later forces alpha = 1 (PIX "ClearAlpha", which the VR HUD drops to keep coverage),
// so in VR the minimap would come out half transparent. Inside a VR HUD interval the call runs with
// grcState 16 (colour write mask) = 7 (RGB): the circle mask alpha (1) stays. Restored right after.
inline constexpr std::uintptr_t radarMapOffset=0x2a19bf8,radarMapGotOffset=0x2e5e670,radarMapReturnOffset=0x2a1ab7c;
inline constexpr std::uintptr_t grcSetStateOffset=0x1af99e0,grcGetStateOffset=0x1af9fc4;
inline constexpr std::array<unsigned char,16> radarMapFingerprint{
#include "gamecode/TraceEntry_2a19bf8_16_8d.inc"
};
inline constexpr std::array<unsigned char,16> grcSetStateFingerprint{
#include "gamecode/TraceEntry_1af99e0_16_8d.inc"
};
inline constexpr std::array<unsigned char,16> grcGetStateFingerprint{
#include "gamecode/TraceEntry_1af9fc4_16_8d.inc"
};
inline constexpr std::array<unsigned char,4> radarMapCall{42,69,10,148};
using RadarMapFn=void(*)(void*,bool,int,int,bool);using GrcSetStateFn=void(*)(unsigned,unsigned);using GrcGetStateFn=unsigned(*)(unsigned);
RadarMapFn originalRadarMap{};GrcSetStateFn grcSetState{};GrcGetStateFn grcGetState{};
std::atomic<std::uint64_t> radarMapRgbOnly{};std::atomic<bool> radarMapHooked{};
__attribute__((noinline)) void radarDrawMap(void* self,bool a,int b,int c,bool d){
    float sx{},sy{};
    if(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+radarMapReturnOffset&&grcSetState&&grcGetState&&radarFixScale(sx,sy)){
        const unsigned saved=grcGetState(16u);grcSetState(16u,7u);
        originalRadarMap(self,a,b,c,d);
        grcSetState(16u,saved);radarMapRgbOnly.fetch_add(1,std::memory_order_relaxed);return;
    }
    originalRadarMap(self,a,b,c,d);
}
// Same RGB-only colour mask around that quad (exact return addresses; every other immediate draw
// passes straight through).
inline constexpr std::uintptr_t grcBeginOffset=0x1af56f0,grcBeginGotOffset=0x2e22410,radarQuadBeginReturnOffset=0x2a1ad88;
inline constexpr std::uintptr_t grcEndOffset=0x1af5858,grcEndGotOffset=0x2e22420,radarQuadEndReturnOffset=0x2a1ae2c;
inline constexpr std::array<unsigned char,16> grcBeginFingerprint{
#include "gamecode/TraceEntry_1af56f0_16_8d.inc"
};
inline constexpr std::array<unsigned char,16> grcEndFingerprint{
#include "gamecode/TraceEntry_1af5858_16_8d.inc"
};
inline constexpr std::array<unsigned char,4> radarQuadBeginCall{119,99,8,148},radarQuadEndCall{86,99,8,148};
using GrcBeginFn=std::uintptr_t(*)(int,int);using GrcEndFn=void(*)();
GrcBeginFn originalGrcBegin{};GrcEndFn originalGrcEnd{};
std::atomic<std::uint64_t> radarInteriorRgbOnly{};std::atomic<bool> radarQuadHooked{};
thread_local bool radarQuadMasked{};thread_local unsigned radarQuadSavedMask{};
__attribute__((noinline)) std::uintptr_t radarGrcBegin(int mode,int count){
    float sx{},sy{};
    if(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+radarQuadBeginReturnOffset&&!radarQuadMasked&&grcSetState&&grcGetState&&radarFixScale(sx,sy)){
        radarQuadSavedMask=grcGetState(16u);grcSetState(16u,7u);radarQuadMasked=true;}
    return originalGrcBegin(mode,count);
}
__attribute__((noinline)) void radarGrcEnd(){
    const bool mine=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+radarQuadEndReturnOffset&&radarQuadMasked;
    originalGrcEnd();
    if(mine){grcSetState(16u,radarQuadSavedMask);radarQuadMasked=false;radarInteriorRgbOnly.fetch_add(1,std::memory_order_relaxed);}
}
// PTFX CULL (RE: ptxEffectRuleStd::DrawEffect 0x2329df8 -> bl CullEffect @0x2329e28 -> rdrParticleDefaultCulling, sphere vs
// *grcViewport::sm_Current): inside the early tail (postfx::tail::early::cullViewport set) sm_Current is the left EYE viewport for the
// call only; drawing / billboards / shader globals keep the native current viewport. debug.rdrvr.vr.ptfxcull 0 = off.
inline constexpr std::uintptr_t ptfxCullOffset=0x2329bd0,ptfxCullGotOffset=0x2e30a00,ptfxCullReturnOffset=0x2329e2c,smCurrentViewportOffset=0x4e6e14d8;
inline constexpr std::array<unsigned char,16> ptfxCullFingerprint{
#include "gamecode/TraceEntry_198c8d0_16_8d.inc"
};
inline constexpr std::array<unsigned char,4> ptfxCullCall{70,154,36,148};
using PtfxCullFn=int(*)(void*,void*);PtfxCullFn originalPtfxCull{};
std::atomic<std::uint64_t> ptfxCullSwaps{},ptfxCullKept{},ptfxCullDropped{};std::atomic<bool> ptfxCullHooked{};
__attribute__((noinline)) int ptfxCullEffect(void* rule,void* inst){
    const void* eye=::rdrvr::engine::postfx::tail::early::cullViewport;
    static unsigned calls=0;static int lever=1;
    if((calls++&1023u)==0){char v[PROP_VALUE_MAX]{};lever=__system_property_get("debug.rdrvr.vr.ptfxcull",v)>0&&v[0]>='0'&&v[0]<='1'?v[0]-'0':1;}
    if(!eye||!lever||reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))!=engineBase+ptfxCullReturnOffset)return originalPtfxCull(rule,inst);
    auto* slot=reinterpret_cast<const void**>(engineBase+smCurrentViewportOffset);
    const void* saved=*slot;*slot=eye;
    const int r=originalPtfxCull(rule,inst);
    *slot=saved;ptfxCullSwaps.fetch_add(1,std::memory_order_relaxed);(r==1?ptfxCullDropped:ptfxCullKept).fetch_add(1,std::memory_order_relaxed);
    return r;
}
// TOUCH PTFX: camManager::GetViewport is the cull getter (JUMP_SLOT 0x2e48050, getterCull); its caller TouchImagePftxManager::Render
// (ret 0x2b2bcb0) gets the left EYE viewport while the early tail draws it inside the left colour stage (postfx::tail::early::touchActive).
inline constexpr std::uintptr_t touchGetViewportReturnOffset=0x2b2bcb0;
inline constexpr std::array<unsigned char,4> touchGetViewportCall{205,77,5,148};
std::atomic<std::uint64_t> touchEyeViewports{};std::atomic<bool> touchHooked{};
std::string radarFixReport(){
    return "{\"hooked\":"+std::string(radarFixHooked.load()?"true":"false")+",\"windows\":"+std::to_string(radarFixWindows.load())+",\"scissors\":"+
        std::to_string(radarFixScissors.load())+",\"skipped\":"+std::to_string(radarFixSkipped.load())+",\"device\":["+std::to_string(radarFixDims[0].load())+","+
        std::to_string(radarFixDims[1].load())+"],\"global\":["+std::to_string(radarFixDims[2].load())+","+std::to_string(radarFixDims[3].load())+"],\"mapHooked\":"+
        std::string(radarMapHooked.load()?"true":"false")+",\"mapRgbOnly\":"+std::to_string(radarMapRgbOnly.load())+",\"quadHooked\":"+std::string(radarQuadHooked.load()?"true":"false")+",\"interiorRgbOnly\":"+std::to_string(radarInteriorRgbOnly.load())+",\"tilesForced\":"+std::to_string(radarTilesForced.load())+"}";
}
#endif
using Scene=CameraCallBoundary::Scene;
using Current=CameraCallBoundary::Current;
Scene originalScene{};Current originalCurrent{};
CameraCallBoundary cameraBoundary;
std::atomic<bool> sceneInterposed{};
#ifdef RDRVR_STEREO_BRIDGE
bool readSelectedViewport(const void** selected){
    return readMemory(nullptr,engineBase+currentViewportOffset,selected,sizeof(*selected));
}
#endif
#ifdef RDRVR_PASS_RESOURCES
VisibilityCallBoundary visibilityBoundary;
VisibilityCullBoundary cullBoundary;
std::atomic<bool> cullInterposed{},occlusionConfigured{};
std::atomic<std::uintptr_t> validatedGfxResolver{};
std::atomic<bool> replayInterposed{};
std::atomic<bool> profileInterposed{};
std::atomic<bool> retailInterposed{};RetailSceneBoundary retailBoundary;
#ifdef RDRVR_HEAD_CAMERA
void (*nativePrimitiveDraw)(void*,int,int,int){};
void (*nativePrimitiveIndexed)(void*,int,int,int,int){};
void (*nativePrimitiveInstanced)(void*,int,int,int,int,int){};
void (*nativePrimitiveIndexedInstanced)(void*,int,int,int,int,int,int){};
// SKIP POSTFX (STEREO TEST 16, PostFxSkip.h): inside an active post-processing site the draw keeps
// its d11 binding work (BindInputLayout = pipeline/descriptors + FlushFrameBuffer with pending
// clears), only the vkCmdDraw* is dropped. Inside an active RenderFrame post scope with NO-FLUSH
// (lever bit 0x8) the dropped draw also skips BindInputLayout (no FlushFrameBuffer = no empty
// render pass); never inside a PX TAIL scope (postfx::noFlushNow).
bool (*nativeBindInputLayout)(void*){};
void suppressPrimitiveDraw(void* context,int mode,int first,int count){
    if(postfx::dropDraw()){if(!postfx::noFlushNow())nativeBindInputLayout(context);else postfx::bindsSkipped.fetch_add(1,std::memory_order_relaxed);return;}
    if(nativeDrawSuppression.skip(NativeDrawKind::Draw))return;
    auto* earlySource=experimental::NativeEarlyTraceRoute::source();
    if(!earlySource){nativePrimitiveDraw(context,mode,first,count);return;}
    experimental::CurrentNativeWallSources::Primitive scope(*earlySource,context,false,mode,first,count,0);
    nativePrimitiveDraw(context,mode,first,count);
}
void suppressPrimitiveIndexed(void* context,int mode,int first,int count,int base){
    if(postfx::dropDraw()){if(!postfx::noFlushNow())nativeBindInputLayout(context);else postfx::bindsSkipped.fetch_add(1,std::memory_order_relaxed);return;}
    if(nativeDrawSuppression.skip(NativeDrawKind::Indexed))return;
    auto* earlySource=experimental::NativeEarlyTraceRoute::source();
    if(!earlySource){nativePrimitiveIndexed(context,mode,first,count,base);return;}
    experimental::CurrentNativeWallSources::Primitive scope(*earlySource,context,true,mode,first,count,base);
    nativePrimitiveIndexed(context,mode,first,count,base);
}
void suppressPrimitiveInstanced(void* context,int mode,int a,int b,int c,int d){
    if(postfx::dropDraw()){if(!postfx::noFlushNow())nativeBindInputLayout(context);else postfx::bindsSkipped.fetch_add(1,std::memory_order_relaxed);return;}
    if(nativeDrawSuppression.skip(NativeDrawKind::Instanced))return;
    auto* earlySource=experimental::NativeEarlyTraceRoute::source();
    if(!earlySource){nativePrimitiveInstanced(context,mode,a,b,c,d);return;}
    experimental::CurrentNativeWallSources::Primitive scope(*earlySource,context,false,mode,0,0,0);
    nativePrimitiveInstanced(context,mode,a,b,c,d);
}
void suppressPrimitiveIndexedInstanced(void* context,int mode,int a,int b,int c,int d,int e){
    if(postfx::dropDraw()){if(!postfx::noFlushNow())nativeBindInputLayout(context);else postfx::bindsSkipped.fetch_add(1,std::memory_order_relaxed);return;}
    if(nativeDrawSuppression.skip(NativeDrawKind::IndexedInstanced))return;
    auto* earlySource=experimental::NativeEarlyTraceRoute::source();
    if(!earlySource){nativePrimitiveIndexedInstanced(context,mode,a,b,c,d,e);return;}
    experimental::CurrentNativeWallSources::Primitive scope(*earlySource,context,false,mode,0,0,0);
    nativePrimitiveIndexedInstanced(context,mode,a,b,c,d,e);
}
void (*nativeOcclusionOptimise)(void*){};
bool (*nativeOcclusionVertexLock)(const void*,unsigned,unsigned,unsigned){};
bool (*nativeOcclusionVertexReadOnly)(const void*){};
void* (*nativeOcclusionIndexReadOnly)(const void*){};
bool (*nativeOcclusionVertexUnlock)(const void*){};
void* (*nativeOcclusionIndexLock)(const void*,unsigned,unsigned,unsigned){};
void (*nativeOcclusionIndexUnlock)(const void*){};
void (*nativeOcclusionBufferDestroy)(void*){};
void occlusionOptimise(void* geometry){
    const experimental::NativeEarlyGeometryMutation::Api api{nullptr,
        [](void*,std::uintptr_t address,void* bytes,std::size_t n)noexcept{return readMemory(nullptr,address,bytes,n);},
        experimental::nativeEarlyBootstrap.ready()};
    experimental::NativeEarlyGeometryMutation::invoke(rdr95::nativeResourceGate,api,geometry,[&]{
        NativeOcclusionVertexCapture::Scope scope(nativeOcclusionVertexCapture,geometry);
        nativeOcclusionOptimise(geometry);
    });
}
bool occlusionVertexReadOnly(const void* vertex){
    return experimental::nativeOrdinaryGeometryEpoch.map(rdr95::nativeResourceGate,vertex,
        experimental::NativeOrdinaryGeometryEpoch::Kind::Vertex,[&]{return nativeOcclusionVertexReadOnly(vertex);},
        experimental::NativeOrdinaryGeometryEpoch::Entry::ReadOnly);
}
void* occlusionIndexReadOnly(const void* index){
    return experimental::nativeOrdinaryGeometryEpoch.map(rdr95::nativeResourceGate,index,
        experimental::NativeOrdinaryGeometryEpoch::Kind::Index,[&]{return nativeOcclusionIndexReadOnly(index);},
        experimental::NativeOrdinaryGeometryEpoch::Entry::ReadOnly);
}
bool occlusionVertexLock(const void* vertex,unsigned mode,unsigned first,unsigned count){
    return experimental::nativeOrdinaryGeometryEpoch.map(rdr95::nativeResourceGate,vertex,
        experimental::NativeOrdinaryGeometryEpoch::Kind::Vertex,[&]{
            nativeOcclusionVertexCapture.beforeLock(vertex);
            return nativeOcclusionVertexLock(vertex,mode,first,count);
        });
}
__attribute__((noinline)) bool occlusionVertexUnlock(const void* vertex){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    return experimental::nativeOrdinaryGeometryEpoch.unmap(rdr95::nativeResourceGate,vertex,
        experimental::NativeOrdinaryGeometryEpoch::Kind::Vertex,[&]{
            nativeOcclusionVertexCapture.beforeUnlock(vertex,caller);
            const bool result=nativeOcclusionVertexUnlock(vertex);
            if(!result)nativeOcclusionVertexCapture.beforeLock(vertex);
            return result;
        });
}
void occlusionBufferDestroy(void* buffer){
    experimental::nativeOrdinaryGeometryEpoch.mutate(rdr95::nativeResourceGate,[&]{
        nativeOcclusionVertexCapture.beforeBackingDestroy(buffer);nativeOcclusionBufferDestroy(buffer);
    });
}
void* occlusionIndexLock(const void* index,unsigned mode,unsigned first,unsigned count){
    return experimental::nativeOrdinaryGeometryEpoch.map(rdr95::nativeResourceGate,index,
        experimental::NativeOrdinaryGeometryEpoch::Kind::Index,[&]{
            nativeOcclusionVertexCapture.beforeIndexLock(index);
            auto* result=nativeOcclusionIndexLock(index,mode,first,count);
            if(!result)nativeOcclusionVertexCapture.afterIndexUnlock(index);
            return result;
        });
}
void occlusionIndexUnlock(const void* index){
    experimental::nativeOrdinaryGeometryEpoch.unmap(rdr95::nativeResourceGate,index,
        experimental::NativeOrdinaryGeometryEpoch::Kind::Index,[&]{
            nativeOcclusionIndexUnlock(index);nativeOcclusionVertexCapture.afterIndexUnlock(index);
        });
}
void (*nativeOcclusionGeometry)(const void*){};
void occlusionGeometry(const void* geometry){
    auto* earlySource=experimental::NativeEarlyTraceRoute::source();
    if(!earlySource){nativeOcclusionGeometry(geometry);return;}
    experimental::NativeEarlyTraceRoute::Geometry scope(geometry,true);nativeOcclusionGeometry(geometry);
}
void (*nativeOcclusionTiles)(void*,unsigned){};
__attribute__((noinline)) void occlusionTiles(void* object,unsigned pass){
    const int incoming=errno;const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    std::uintptr_t table{};
    const bool exact=pass==0&&caller==engineBase+depthCoverageSiteReturnOffset&&
        readMemory(nullptr,reinterpret_cast<std::uintptr_t>(object),&table,sizeof table)&&table==engineBase+depthCoverageShadowVtableOffset;
    experimental::NativeEarlyTraceRoute::Tiles scope(exact);
    errno=incoming;nativeOcclusionTiles(object,pass);
}
bool borrowedOcclusionRead(void*,std::uintptr_t p,void* out,std::size_t n){std::memcpy(out,reinterpret_cast<const void*>(p),n);return true;}
bool writeReplayPhase(void*,std::uintptr_t address,const void* value,std::size_t size){
    if(address!=engineBase+replayPhaseIndexOffset||size!=sizeof(std::uint32_t)||!value)return false;
    std::uint32_t phase{};std::memcpy(&phase,value,sizeof phase);
    if(phase!=1&&phase!=3)return false;
    // The pinned writable data slot and GOT identity are validated at arm and
    // again by ReplayRenderPhase on the native render owner before each write.
    std::memcpy(reinterpret_cast<void*>(address),&phase,sizeof phase);return true;
}
__attribute__((noinline)) void retainGrassVisibility(void* manager){
    const auto pc=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    gameGrassVisibility().endUsingVis(manager,pc==engineBase+grassEndCallerReturnOffset);
}
void (*nativePrimarySetup)(void*,bool){};
void (*nativePrimaryShaft)(void*,const void*,const void*){};
__attribute__((noinline)) void primarySetup(void* renderer,bool enabled){
    const auto pc=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    gamePrimaryEye().setup(renderer,pc==engineBase+primarySetupSiteReturnOffset,[&]{nativePrimarySetup(renderer,enabled);});
}
__attribute__((noinline)) void primaryShaft(void* shaft,const void* viewport,const void* camera){
    const auto pc=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const auto native=[&](const void* selected,const void* matrix){nativePrimaryShaft(shaft,selected,matrix);};
    if(pc==engineBase+primaryShaftSiteReturnOffset)gamePrimaryEye().shaft(viewport,camera,true,native);
    else gameEyeShaft().shaft(viewport,camera,pc==engineBase+primaryDepthShaftSiteReturnOffset||
                             pc==engineBase+primaryWorldShaftSiteReturnOffset,native);
}
#endif
std::atomic<bool> resolutionInterposed{};NativeResolutionPolicy resolutionPolicy;
#ifdef RDRVR_HEAD_CAMERA
using EyeInitial=void(*)(void*);
EyeInitial nativeEyeRendererInit{},nativeEyePostfxInit{},nativeEyeGrassInit{},nativeEyeReflectionInit{};
void (*nativeEyeTreesInit)(){};
EyeInitial nativeEyeBackbuffers{};
void eyeRendererInit(void* renderer){
    NativeEyeResolution::InitialScope scope(nativeEyeResolution,NativeEyeResolution::Family::Renderer,renderer);
    nativeEyeRendererInit(renderer);scope.finish();
}
void eyePostfxInit(void* renderer){
    NativeEyeResolution::InitialScope scope(nativeEyeResolution,NativeEyeResolution::Family::Postfx,renderer);
    nativeEyePostfxInit(renderer);scope.finish();
}
void eyeGrassInit(void* grass){
    NativeEyeResolution::InitialScope scope(nativeEyeResolution,NativeEyeResolution::Family::Grass,grass);
    nativeEyeGrassInit(grass);scope.finish();
}
void eyeTreesInit(){
    NativeEyeResolution::InitialScope scope(nativeEyeResolution,NativeEyeResolution::Family::Trees);
    nativeEyeTreesInit();scope.finish();
}
void eyeReflectionInit(void* renderer){
    NativeEyeResolution::InitialScope scope(nativeEyeResolution,NativeEyeResolution::Family::Reflection,renderer);
    nativeEyeReflectionInit(renderer);scope.finish();
}
__attribute__((noinline)) void eyeMaxScreen(int* width,int* height){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    auto site=NativeEyeResolution::Family::None;
    for(unsigned i=0;i<eyeResolutionMaxScreenReturns.size();++i)
        if(caller==engineBase+eyeResolutionMaxScreenReturns[i])site=static_cast<NativeEyeResolution::Family>(i);
    nativeEyeResolution.maxScreen(width,height,site);
}
__attribute__((noinline)) void eyeNativeResize(void* renderer,int width,int height){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));bool exact=false;
    for(auto pc:eyeResolutionResizeReturns)exact|=caller==engineBase+pc;
    nativeEyeResolution.resize(renderer,width,height,exact);
}
__attribute__((noinline)) bool eyeDisplayDepth(void* surface,const void* descriptor){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    return nativeFramebufferExtent.createDepth(surface,descriptor,caller==engineBase+framebufferDepthDeviceReturnOffset);
}
void eyeFramebufferCtor(void* framebuffer,const void* descriptor,std::uint32_t hash){
    nativeFramebufferExtent.constructFramebuffer(framebuffer,descriptor,hash);
}
bool eyeRequestedExtent(void*,std::array<std::uint32_t,2>& size){
    const auto requested=nativeEyeResolution.requestedExtent();
    size={requested.width,requested.height};return bool(requested);
}
void eyeDepthCreated(void*,void* surface,std::uint32_t width,std::uint32_t height,bool valid){
    nativeEyeResolution.depthCreated(surface,width,height,valid);
}
__attribute__((noinline)) void eyeBackbuffers(void* display){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    nativeEyeBackbuffers(display);
    if(nativeFramebufferExtent.confirmNoDisplayDepth(display,caller==engineBase+framebufferBackbuffersInitialReturnOffset))
        nativeEyeResolution.displayDepthNotRequired(true);
}
__attribute__((noinline)) void updateResolution(void* renderer){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const bool exact=caller==engineBase+resolutionUpdateReturnOffset;
    nativeEyeResolution.update(renderer,exact,gameEyeResolutionCoordinatesReady(),gameEyeResolutionWorldActive(),
        [&]{resolutionPolicy.update(renderer,exact);});
}
bool writeResolutionFlag(std::uintptr_t address,std::uint8_t value){
    // Only the checked live renderer argument +584 in UpdateDRS reaches here.
    // The native method writes this same renderer synchronously on this thread.
    std::memcpy(reinterpret_cast<void*>(address),&value,1);return true;
}
#endif
#ifdef RDRVR_NATIVE_MENUS
// RDR's IsMenuActive() is TRUE during story cutscenes, so NativeUiContext would never reach its
// cutscene check. While the cutscene manager plays, the menu answer is "no" -> Cutscene context.
// debug.rdrvr.vr.cutscene.menu 0 = native answer. The manager singleton is checked first (the
// native dereferences it unchecked).
bool (*uiMenuActiveNative)(){};bool (*uiCutscenePlayingNative)(){};
// IsMenuActive() = !GetUIObject("HudScene")->visible || UIManager[+0x90]+0x178 >= 1 (menu stack). Story scenes outside the cutscene
// manager only hide the HUD. Lever debug.rdrvr.vr.cutscene.menu: 0 native, 1 (default) not a menu while a cutscene plays, 2 + a hidden HUD
// with an empty menu stack is not a menu (the camera context decides). Counters per answer in vr.uiMenu.
std::atomic<std::uint64_t> uiMenuCalls{},uiMenuNative{},uiMenuStack{},uiMenuHudOnly{},uiMenuCutscene{},uiMenuStackUnread{};
std::atomic<int> uiMenuLever{1},uiMenuLastStack{-1};
bool uiMenuUnlessCutscene(){
    uiMenuCalls.fetch_add(1,std::memory_order_relaxed);
    if(!uiMenuActiveNative||!uiMenuActiveNative())return false;
    uiMenuNative.fetch_add(1,std::memory_order_relaxed);
    static unsigned calls=0;
    if((calls++&255u)==0){char v[PROP_VALUE_MAX]{};const int n=__system_property_get("debug.rdrvr.vr.cutscene.menu",v);
        uiMenuLever.store(n>0&&v[0]>='0'&&v[0]<='2'?v[0]-'0':1,std::memory_order_relaxed);}
    const int lever=uiMenuLever.load(std::memory_order_relaxed);
    std::uintptr_t ui{},stackObj{};std::int32_t depth{-1};
    const bool stackOk=readMemory(nullptr,engineBase+uiSingletonOffset,&ui,sizeof ui)&&ui&&readMemory(nullptr,ui+0x90,&stackObj,sizeof stackObj)&&stackObj&&
        readMemory(nullptr,stackObj+0x178,&depth,sizeof depth);
    uiMenuLastStack.store(stackOk?depth:-1,std::memory_order_relaxed);
    if(!stackOk)uiMenuStackUnread.fetch_add(1,std::memory_order_relaxed);
    else if(depth>=1)uiMenuStack.fetch_add(1,std::memory_order_relaxed);
    else uiMenuHudOnly.fetch_add(1,std::memory_order_relaxed);
    if(lever==0)return true;
    if(uiCutscenePlayingNative){
        std::uintptr_t manager{},table{};
        if(readMemory(nullptr,engineBase+uiCutsceneSingletonOffset,&manager,sizeof manager)&&manager&&readMemory(nullptr,manager,&table,sizeof table)&&
           (table==engineBase+uiCutsceneTableOffset||table==engineBase+uiCutsceneSingletonTableOffset)&&uiCutscenePlayingNative()){
            uiMenuCutscene.fetch_add(1,std::memory_order_relaxed);return false;}
    }
    if(lever>=2&&stackOk&&depth<1)return false;
    return true;
}
#endif
VisibilityCallBoundary::Original originalVisibility{};
std::atomic<bool> visibilityInterposed{};
std::atomic<bool> textureGlobalInterposed{},textureInstanceInterposed{};
std::atomic<bool> targetLockInterposed{},targetUnlockInterposed{};
std::atomic<bool> sceneResolveInterposed{};
std::uint64_t beginVisibility(void* renderer,void* viewport,const void* camera){
    std::uint32_t marker{};
    if(!readMemory(nullptr,engineBase+visibilityMarkerOffset,&marker,sizeof(marker)))return 0;
    return gameBeginVisibility(reinterpret_cast<std::uintptr_t>(renderer),marker,reinterpret_cast<std::uintptr_t>(viewport),
        reinterpret_cast<std::uintptr_t>(camera),now(),readMemory,nullptr);
}
__attribute__((noinline)) void cachedCull(void* request,void* begin,void* end,bool rooms,bool special){
    cullBoundary.cached(request,begin,end,rooms,special,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+cullCachedReturnOffset);
}
// Runs inside cullBoundary.cached() after the native CreateVisibleShaftList.
void occluderAfterCached(void* request,const void* viewport,bool rooms,bool special)noexcept{
#ifdef RDRVR_HEAD_CAMERA
    indoor::publishRooms(rooms); // MAIN rooms bool for IndoorState.h
#endif
    occluderInjection.afterCached(request,viewport,rooms,special);
    smallObjectCull.frame(reinterpret_cast<std::uintptr_t>(request),viewport,runtime::graphicsSettings.smallObjectsPx(), // Histogram only on request
        s02::eyeCullTesting.load(std::memory_order_relaxed)?1:-1); // EYE CULL needs counted tokens even with SMALL OBJECTS 0 (counts only, no clock)
    s02::smallObjectsPx.store(int(smallObjectCull.thresholdPx()),std::memory_order_relaxed); // byState _so<px>[p] = the threshold in force
    s02::smallObjectsProperty.store(smallObjectCull.thresholdFromProperty(),std::memory_order_relaxed);
}
__attribute__((noinline)) void retailDepth(void* renderer,void* viewport){
    const bool exact=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+retailDepthReturnOffset;
    SmallObjectCull::Stage smallStage(smallObjectCull,SmallObjectCull::Depth,exact);
#ifdef RDRVR_HEAD_CAMERA
    ablate::Scope ablateStage(ablate::SDepth,exact); // Ablate: stock depth stage bit (one relaxed load when off)
#endif
    retailBoundary.depth(renderer,viewport,exact);
}
__attribute__((noinline)) void retailLights(void* manager,const void* shaft){
    const bool exact=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+retailLightsReturnOffset;
#ifdef RDRVR_HEAD_CAMERA
    ablate::Scope ablateStage(ablate::SLight,exact,true); // Light stage bit, as ablateStereoLights (one relaxed load when off)
#endif
    retailBoundary.lights(manager,shaft,exact);
}
#ifdef RDRVR_HEAD_CAMERA
// The stereo pass consumer (RetailSceneDraw, inside retailColor's colour scope) calls the engine's depth and
// light functions directly: give them the same exclusive stage bit as the left stages, so no ablate category depends on the eye.
RetailSceneBoundary::Draw ablateStereoDepthOriginal{};RetailSceneBoundary::Lights ablateStereoLightsOriginal{};
__attribute__((noinline)) void ablateStereoDepth(void* renderer,void* viewport){
    ablate::Scope ablateStage(ablate::SDepth,true,true);ablateStereoDepthOriginal(renderer,viewport);}
__attribute__((noinline)) void ablateStereoLights(void* manager,const void* shaft){
    ablate::Scope ablateStage(ablate::SLight,true,true);ablateStereoLightsOriginal(manager,shaft);}
#endif
__attribute__((noinline)) void retailColor(void* renderer,void* viewport){
    const bool exact=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+retailColorReturnOffset;
#ifdef RDRVR_HEAD_CAMERA
    ReplayGrassVisibility::Scope grass(gameGrassVisibility(),gameGrassVisibility().ready()&&retailBoundary.qualifiesColor(renderer,viewport,exact));
    ablate::Scope ablateStage(ablate::SColor,exact); // Ablate: stock colour stage bit (incl. the stereo pass consumer)
#endif
    {SmallObjectCull::Stage smallStage(smallObjectCull,SmallObjectCull::Color,exact);
    retailBoundary.color(renderer,viewport,exact);}
    if(exact){smallObjectCull.endFrame();const auto& work=diagnostics::guardedReads;guardedReadSites.frame(work.calls,work.direct,work.segments);}
    // MAIN wait (depth stage) and LEFT depth+color draws of this frame are
    // complete; sample request draw counts before the next PrepareVisibilityList.
    if(exact){ // + visible grass fields of this frame's exact left colour grass draw (UINT32_MAX when no grass draw ran)
        static std::uint64_t lastGrassDraws{};const auto grassDraws=shadowgrass::state.grassDraws.load(std::memory_order_relaxed);
        const int fields=shadowgrass::state.visibleFieldsLast.load(std::memory_order_relaxed);
        const std::uint32_t grass=grassDraws!=lastGrassDraws&&fields>=0?std::uint32_t(fields):UINT32_MAX;lastGrassDraws=grassDraws;
        occluderInjection.sample(stereo::lastTapeDraws(stereo::Stage::Depth),stereo::lastTapeDraws(stereo::Stage::Color),
            diagnostics::drawStageTimings.lastObjects(0),diagnostics::drawStageTimings.lastObjects(2),grass);} // stockDepth, stockColor
    // Grass scope destruction executes the native cleanup before the caller's
    // matrix unlock/semaphore signal. This also covers a declined consumer.

}
void observeVisibility(void* renderer,void* viewport,const void* camera){visibilityBoundary.invoke(renderer,viewport,camera);}
const void* selectCull(void* renderer,void* viewport,const void* camera){
    return gamePrepareCull(reinterpret_cast<std::uintptr_t>(renderer),visibilityBoundary.serial(renderer,viewport,camera),viewport);
}
void prepareCull(void* renderer,void* viewport,const void* camera){cullBoundary.prepare(renderer,viewport,camera);}
void workerCull(void* manager,void* request){cullBoundary.worker(manager,request);}
__attribute__((noinline)) const void* getterCull(const void* manager,int channel){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    { // TOUCH PTFX: the objective X sprite drawn inside the left colour stage uses the eye viewport
        namespace te=::rdrvr::engine::postfx::tail::early;
        if(te::touchActive&&te::cullViewport&&caller==engineBase+touchGetViewportReturnOffset&&touchHooked.load(std::memory_order_relaxed)){
            touchEyeViewports.fetch_add(1,std::memory_order_relaxed);return te::cullViewport;}
    }
    return cullBoundary.viewport(manager,channel,caller==engineBase+cullGetterReturnOffset);
}
__attribute__((noinline)) void signalCull(void* thread){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    cullBoundary.signal(thread,caller==engineBase+cullSignalReturnOffset);
}
#endif
void captureCamera(const void* viewport) {
#ifdef RDRVR_PASS_RESOURCES
    if(visibilityInterposed.load(std::memory_order_acquire)){
        std::uint32_t marker{};
        if(readMemory(nullptr,engineBase+visibilityMarkerOffset,&marker,sizeof(marker)))
            gameObserveFrozenCamera(reinterpret_cast<std::uintptr_t>(cameraBoundary.renderer()),marker,reinterpret_cast<std::uintptr_t>(viewport),now(),readMemory,nullptr);
        return;
    }
#endif
    gameObserveCamera(reinterpret_cast<std::uintptr_t>(viewport),now(),tid(),readMemory,nullptr);
}
#ifdef RDRVR_HEAD_CAMERA
using GraphicsSun=void(*)(void*,const void*,float,bool,bool,bool,int);
using GraphicsGrass=void(*)(void*,int);
GraphicsSun originalGraphicsSun{};GraphicsGrass originalGraphicsGrass{};
__attribute__((noinline)) void graphicsSun(void* manager,const void* viewport,float range,bool cloud,bool clip,bool stencil,int tiles){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const int saved=errno;const bool exact=caller==engineBase+graphicsSunSiteReturnOffset&&gameGraphicsEyeQualified(viewport);
    if(exact){ // Receiver argument histogram, STEREO TEST 14/15 cloud/range change; SUN SHADOWS OFF marks the scene
        const auto args=shadowupdate::receiver(range,cloud,tiles,stereo::testMode.load(std::memory_order_relaxed));range=args.range;cloud=args.cloud;
        if(graphicsDrawPolicy.disabled(GraphicsDrawPolicy::Feature::SunShadows))shadowupdate::receiverSuppressed(postfx::sceneSerial.load(std::memory_order_relaxed));
    }
    errno=saved;
    if(exact)stereo::gpuMark(stereo::GpuMark::SunBegin); // DETAIL GPU span of the sun shadow pass
    ablate::Scope ablateSun(ablate::PSunCollector); // Ablate: sun shadow collector draws (one relaxed load when off)
    graphicsDrawPolicy.draw(GraphicsDrawPolicy::Feature::SunShadows,exact,[&]{
        shadowgrass::FilterScope filter(manager,exact); // Receiver filter for this call (restored after it)
        originalGraphicsSun(manager,viewport,range,cloud,clip,stencil,tiles);});
    if(exact)stereo::gpuMark(stereo::GpuMark::SunEnd);
}
__attribute__((noinline)) void graphicsGrass(void* manager,int pass){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    const int saved=errno;const bool exact=((pass==0&&caller==engineBase+graphicsGrassColorSiteReturnOffset)||
                      (pass==2&&caller==engineBase+graphicsGrassDepthSiteReturnOffset))&&gameGraphicsEyeQualified();
    errno=saved;
    ablate::Scope ablateGrass(ablate::PGrass); // Ablate: grass manager draws (one relaxed load when off)
    graphicsDrawPolicy.draw(GraphicsDrawPolicy::Feature::Grass,exact,[&]{
        shadowgrass::observeGrassDraw(manager,pass,exact); // Visible fields vs GRASS DISTANCE cull
        originalGraphicsGrass(manager,pass);});
}
// SHADOW UPDATE (ShadowUpdate.h): CShadowMap::RenderIntoShadowMaps / BeginTileAroundSphere /
// BeginRenderShadowsIntoShadowCollector / SetShadowMapConstants. The exact caller is proven by the return address;
// every other caller passes through unchanged.
__attribute__((noinline)) void shadowRenderInto(void* map,bool a,void* sun,bool b,unsigned c,bool d){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));const int saved=errno;
    const unsigned mode=stereo::testMode.load(std::memory_order_relaxed);
    // SUN SHADOWS OFF: skip the atlas only while the receiver of this or the previous scene really was suppressed.
    const shadowupdate::Request request{shadowupdate::stats.level.load(std::memory_order_relaxed),mode==stereo::testModeFreezeShadowAtlas,
        graphicsDrawPolicy.disabled(GraphicsDrawPolicy::Feature::SunShadows)&&shadowupdate::receiverRecentlyOff(postfx::sceneSerial.load(std::memory_order_relaxed))};
    ablate::Scope ablateAtlas(ablate::PSunAtlas); // Ablate: sun cascade atlas casters (one relaxed load when off)
    errno=saved;shadowupdate::renderInto(map,a,sun,b,c,d,caller,request);
}
__attribute__((noinline)) void shadowBeginTile(void* map,int tile,bool b){
    shadowupdate::beginTile(map,tile,b,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
}
__attribute__((noinline)) void shadowCollector(void* map,bool a,bool b,bool c){
    shadowupdate::collector(map,a,b,c,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
}
__attribute__((noinline)) void shadowConstants(void* map){
    shadowupdate::constants(map,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
}
// SHADOW UPDATE STAGGER (SunStagger.h): vtable slot rmlShadowMap::RenderShadowCasters (a masked
// tile of the mod's own partial atlas render is skipped with the proxy's list counter advanced) and
// GOT grcDevice::Clear (the full-atlas clear of that call is dropped). Both act only while
// stagger::rt.armed (the mod's own call, render thread); every other call passes through unchanged.
__attribute__((noinline)) void shadowCasters(void* map,void* vp,bool b,unsigned w){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));const int saved=errno;
    const bool handled=shadowupdate::stagger::casters(map,vp,b,w,caller);errno=saved;
    if(!handled)shadowupdate::stagger::originalCasters(map,vp,b,w);
}
// grcDevice::Clear(bool, Color32, bool, float, bool, unsigned, int): w0, x1 (Color32 passed through whole), w2, s0, w3, w4, w5.
__attribute__((noinline)) void shadowDeviceClear(bool c,std::uintptr_t color,bool z,float depth,bool s,unsigned st,int mask){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));const int saved=errno;
    const bool skip=shadowupdate::stagger::clear(caller);errno=saved;
    if(!skip)shadowupdate::stagger::originalClear(c,color,z,depth,s,st,mask);
}
// LAMP SHADOWS (LampShadows.h): sagRenderer::DrawCubeShadowMaps / CCubeShadowMap::RenderIntoCubeShadowMap /
// CCubeShadowMap::DrawShadow. The exact callers are proven by the return address; every other caller passes through.
__attribute__((noinline)) void lampDrawCube(void* self){
    const int saved=errno;const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    errno=saved;lampshadow::drawCube(self,caller);
}
__attribute__((noinline)) void lampRenderInto(void* cube){
    const int saved=errno;const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    errno=saved;lampshadow::renderInto(cube,caller);
}
__attribute__((noinline)) void lampDrawShadow(void* self,int face,int slot,bool a,bool b,bool c,void* viewport){
    ablate::Scope ablateLamp(ablate::PLamp); // Ablate: lamp cube / spot shadow casters (one relaxed load when off)
    lampshadow::drawShadow(self,face,slot,a,b,c,viewport,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
}
// STEREO TEST 16 SKIP POSTFX (PostFxSkip.h): RenderFrame's post-capture calls run natively with bind-only
// draws. x0 is passed through (DrawSunOptics also has a non-void path); the exact sites ignore it.
std::array<std::uintptr_t,postfx::SiteCount> postOriginal{};
template<unsigned Site> __attribute__((noinline)) std::uintptr_t postSite(void* self){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    postfx::Scope scope(Site,caller==engineBase+postfx::routes[Site].ret,stereo::testMode.load(std::memory_order_relaxed));
    return reinterpret_cast<std::uintptr_t(*)(void*)>(postOriginal[Site])(self);
}
template<unsigned Site> __attribute__((noinline)) std::uintptr_t postSiteFlag(void* self,bool flag){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    postfx::Scope scope(Site,caller==engineBase+postfx::routes[Site].ret,stereo::testMode.load(std::memory_order_relaxed));
    return reinterpret_cast<std::uintptr_t(*)(void*,bool)>(postOriginal[Site])(self,flag);
}
// POST FX OFF FAST (PostFxSkip.h postfx::deep). GOT replacement of rdrPostFX::ProcessRDR2 (reached from
// grPostFX::ProcessPostProcess blr @0x23eb078 through the grPostFXEffectRDR2 thunk) and the factory vtable+0xa8 tap
// (UnlockRenderTargetBindBackBuffer = BlitToRT's unlock while grPostFX+0x470 is clear; gameTargetUnlock only sees +0xa0).
std::uintptr_t postRdr2Original{},postUnlockBackBufferOriginal{};
__attribute__((noinline)) void postRdr2(void* self){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
    if(postfx::deep::plan(caller==engineBase+postfx::deep::callerReturn,self,engineBase)!=postfx::deep::Deep){
        postfx::deep::Calibrate calibrate;reinterpret_cast<void(*)(void*)>(postRdr2Original)(self);return;
    }
    postfx::deep::Run run(engineBase,self); // Suspend + lock replay + ProcessNone + StoreMotionBlurMtxs
}
void postUnlockBackBuffer(void* factory,int index,const void* flags,int resolve,bool window){
    postfx::noteUnlock(true);
    reinterpret_cast<void(*)(void*,int,const void*,int,bool)>(postUnlockBackBufferOriginal)(factory,index,flags,resolve,window);
}
// PX TAIL (PostFxSkip.h postfx::tail): the 11 RenderScene tail calls after the capture. Every one takes at most
// (this, one int/bool/pointer) and no floating-point argument; x0..x3 and the x0 result are passed through unchanged.
std::array<std::uintptr_t,postfx::tail::Count> tailOriginal{};
template<unsigned Site> __attribute__((noinline)) std::uintptr_t tailSite(std::uintptr_t a,std::uintptr_t b,std::uintptr_t c,std::uintptr_t d){
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));const int saved=errno;
    const bool exact=postfx::tail::exactCaller(Site,engineBase,caller);
    if(postfx::tail::early::skipNative(Site,exact)){errno=saved;return 0;} // TAIL IN EYES: already drawn inside the left colour stage
    postfx::tail::Scope scope(Site,exact);
    errno=saved;
    return reinterpret_cast<std::uintptr_t(*)(std::uintptr_t,std::uintptr_t,std::uintptr_t,std::uintptr_t)>(tailOriginal[Site])(a,b,c,d);
}
// GPU markers: GpuMark TailEnd at renLightPrePassLightsMgr::EndMainColorPass (body = ret), QueriesEnd after
// sagFxManager::RunQueries; exact callers only, observation only.
std::uintptr_t endMainColorOriginal{},runQueriesOriginal{};
__attribute__((noinline)) void tailEndMarker(void* self){
    if(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+postfx::tail::endMainColorReturnOffset){
        const int saved=errno;postfx::tail::tailEndMarks.fetch_add(1,std::memory_order_relaxed);stereo::gpuMark(stereo::GpuMark::TailEnd);errno=saved;}
    reinterpret_cast<void(*)(void*)>(endMainColorOriginal)(self);
}
__attribute__((noinline)) void queriesEndMarker(void* self){
    const bool exact=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))==engineBase+postfx::tail::runQueriesReturnOffset;
    reinterpret_cast<void(*)(void*)>(runQueriesOriginal)(self);
    if(exact){const int saved=errno;postfx::tail::queriesEndMarks.fetch_add(1,std::memory_order_relaxed);stereo::gpuMark(stereo::GpuMark::QueriesEnd);errno=saved;}
}
// INDOOR RT and the AO-sphere observer (IndoorRt.h).
__attribute__((noinline)) void indoorReflection(void* renderer,void* viewport){
    const int saved=errno;const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));errno=saved;
    indoorrt::reflection(renderer,viewport,caller);
}
__attribute__((noinline)) void indoorTree(void* renderer){
    const int saved=errno;const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));errno=saved;
    indoorrt::tree(renderer,caller);
}
__attribute__((noinline)) void aoSphereObserver(void* manager,const float* v4,bool flag){
    const int saved=errno;const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));errno=saved;
    indoorrt::aoSphere(manager,v4,flag,caller);
}
#endif
__attribute__((noinline)) void observeScene(void* renderer,void* viewport,const void* camera) {
#ifdef RDRVR_HEAD_CAMERA
    // Construct first: finishes AFTER native and replay timing scopes, including
    // every early return/unwind from the synchronous RenderScene invocation.
    PlannerReplayCapture::Scene diagnosticCapture(plannerReplayCapture);
#endif
    runtime::nativeThreadIds[1].store(static_cast<std::uint32_t>(tid()),std::memory_order_relaxed);
#ifdef RDRVR_PASS_RESOURCES
    ReplayUpdates::Scene replay(gameReplayUpdates(),renderer);
#ifdef RDRVR_HEAD_CAMERA
    const auto settings=runtime::graphicsSettings.snapshot().requested;
    shadowupdate::stats.level.store(settings.shadowUpdate,std::memory_order_relaxed); // Live SHADOW UPDATE
    shadowupdate::stats.cullLevel.store(settings.cascadeCull,std::memory_order_relaxed); // Live CASCADE CULL
    postfx::sceneBegin(settings.postFx); // POST FX row; scene serial (SUN SHADOWS OFF atlas skip, STEREO TEST 16 pair check)
    px2d::sceneBegin(); // PX 2D: debug.rdrvr.px2d about once per second; closes any 2D drop span (never across a RenderScene)
    postfx::tail::early::sceneBegin(postfx::sceneSerial.load(std::memory_order_relaxed)); // debug.rdrvr.tail every 64 scenes
    ablate::sceneBegin(stereo::gpuStagesArmed.load(std::memory_order_relaxed)); // Ablate: debug.rdrvr.ablate every 64 scenes; window schedule
    creaturelod::sceneBegin(); // CREATURE LOD: debug.rdrvr.lod.creature* at most once per second; perScene window every 64 scenes
    staticlod::sceneBegin();terrainlod::sceneBegin(); // STATIC / TERRAIN LOD: debug.rdrvr.lod.static* / lod.terrain* once per second, 1 s window
    lowfreq::sceneBegin(); // LOW-FREQUENCY PASSES: 1 s window of coarse draws per scene, property re-read (restartPending only)
    lampshadow::sceneBegin(settings.lampShadows); // LAMP SHADOWS row; debug.rdrvr.lamp* / spotcascade every 64 scenes
    runtime::lampShadowsProperty.store(lampshadow::propertyActive()?1:-1,std::memory_order_relaxed);
    shadowupdate::stagger::sceneBegin(); // Debug.rdrvr.sunstagger every 64 scenes
    runtime::sunStaggerGate.store(shadowupdate::stagger::gateState(),std::memory_order_relaxed); // SUN SHADOWS IN ROOM suffix
    runtime::sunStaggerReady.store(shadowupdate::stagger::menuState(),std::memory_order_relaxed); // Faults / raw clear binding
    s02::cfgMask.store(int(settings.s02Mask&s02::userMask),std::memory_order_relaxed); // Graphics.cfg s02Mask
    s02::sceneBegin(); // Debug.rdrvr.s02* every 64 scenes; this frame's lever mask (latched at the previous frame's end)
    postfx::tail::wantedMask.store(s02::wants(s02::PT)?s02::postTailMask():0u,std::memory_order_relaxed);
    postfx::noFlushWanted.store(s02::wants(s02::NF),std::memory_order_relaxed);
    runtime::postFxMenuState.store(postfx::menuState(),std::memory_order_relaxed);
    runtime::postFxMenuProperty.store(postfx::property.load(std::memory_order_relaxed),std::memory_order_relaxed);
    shadowgrass::state.grassLevel.store(settings.grassDistance,std::memory_order_relaxed);
    enginetex::publish(settings,stereo::testMode.load(std::memory_order_relaxed),stereo::boundaryPhase.load(std::memory_order_relaxed));
    runtime::shadowMapCascadeExtent.store(shadowgrass::state.createdExtent.load(std::memory_order_relaxed),std::memory_order_relaxed);
    // The token identifies only this synchronous settings scope; leaf calls
    // additionally prove the actual frozen scene, viewport and exact caller.
    // SUN SHADOWS governed by the NIGHT OFF clock gate (VisibleTrims.h / SunNight.h; debug.rdrvr.sunnight 0 = kill switch)
    GraphicsDrawPolicy::Scene graphics(graphicsDrawPolicy,{trims::sceneBegin(settings),settings.grass},
        reinterpret_cast<std::uintptr_t>(renderer),true);
    PrimaryEyeBoundary::Scene primary(gamePrimaryEye(),renderer,viewport);
#endif
    rdr95::NativeJoinedListOwner::SceneScope earlyListScope(experimental::nativeEarlyListOwner,
        reinterpret_cast<std::uintptr_t>(renderer),reinterpret_cast<std::uintptr_t>(viewport),
        reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))-engineBase);
    RetailSceneBoundary::Scene retail(retailBoundary,renderer,viewport);
#endif
    cameraBoundary.scene(renderer,viewport,camera);
}
void* observeCurrent(const void* viewport,bool download,bool window,bool fullRes) {return cameraBoundary.current(viewport,download,window,fullRes);}
#endif
void event(void*,const std::string& value) {
    std::lock_guard guard(lock);
    if(events.size()<128 && value.size()<131072) events.push_back(value);else ++dropped;
}
void state(const char* value) {std::lock_guard guard(lock);status=value;__android_log_print(ANDROID_LOG_INFO,"RdrVrTrace","state=%s build=%s",value,RDRVR_BUILD_ID);}
struct Mapping {std::uintptr_t begin{},end{};int protection{};};
bool mapping(std::uintptr_t address,std::size_t size,Mapping& out) {
    FILE* maps=std::fopen("/proc/self/maps","r");if(!maps)return false;
    char line[1024];bool found=false;
    while(std::fgets(line,sizeof(line),maps)) {
        unsigned long long begin{},end{};char perms[5]{};
        if(std::sscanf(line,"%llx-%llx %4s",&begin,&end,perms)!=3)continue;
        if(address>=begin && address<end && size<=end-address) {
            out={static_cast<std::uintptr_t>(begin),static_cast<std::uintptr_t>(end),
                (perms[0]=='r'?PROT_READ:0)|(perms[1]=='w'?PROT_WRITE:0)|(perms[2]=='x'?PROT_EXEC:0)};found=true;break;
        }
    }
    std::fclose(maps);return found;
}
#ifdef RDRVR_PASS_RESOURCES
bool worldOcclusionValid() noexcept {
    if(!occlusionConfigured.load(std::memory_order_acquire))return false;
    unsigned char screen{},world{};
    return readMemory(nullptr,engineBase+resourceScreenOcclusionOffset,&screen,1)&&!screen&&
        readMemory(nullptr,engineBase+resourceWorldOcclusionOffset,&world,1)&&!world;
}
bool ensureWorldOcclusion() noexcept {
    if(occlusionConfigured.load(std::memory_order_acquire))return worldOcclusionValid();
    // One process-lifetime conservative setting before dispatching the first
    // stereo request, never a temporary flag flip around asynchronous work.
    for(auto offset:{resourceScreenOcclusionOffset,resourceWorldOcclusionOffset}){
        Mapping map{};unsigned char value{};
        if(!mapping(engineBase+offset,1,map)||(map.protection&(PROT_READ|PROT_WRITE))!=(PROT_READ|PROT_WRITE)||
           !readMemory(nullptr,engineBase+offset,&value,1)||value>1)return false;
    }
    __atomic_store_n(reinterpret_cast<unsigned char*>(engineBase+resourceScreenOcclusionOffset),0,__ATOMIC_RELEASE);
    __atomic_store_n(reinterpret_cast<unsigned char*>(engineBase+resourceWorldOcclusionOffset),0,__ATOMIC_RELEASE);
    occlusionConfigured.store(true,std::memory_order_release);return worldOcclusionValid();
}
std::uintptr_t nativeEarlyCurrentGfxSlot()noexcept {
    const int error=errno;struct Restore{int value;~Restore(){errno=value;}} restore{error};
    const auto descriptor=engineBase+gfxTlsOffset;std::uintptr_t resolver{};
    if(!readMemory(nullptr,descriptor,&resolver,sizeof resolver)||!resolver)return 0;
    if(validatedGfxResolver.load(std::memory_order_acquire)!=resolver){
        Mapping code{};if(!mapping(resolver,4,code)||!(code.protection&PROT_EXEC))return 0;
        validatedGfxResolver.store(resolver,std::memory_order_release);
    }
    using Resolve=std::intptr_t(*)(void*);
    const auto offset=reinterpret_cast<Resolve>(resolver)(reinterpret_cast<void*>(descriptor));
    const auto thread=reinterpret_cast<std::uintptr_t>(__builtin_thread_pointer());
    if(offset>=0&&std::uintptr_t(offset)>UINTPTR_MAX-thread)return 0;
    if(offset<0&&std::uintptr_t(-(offset+1))+1>thread)return 0;
    return thread+std::uintptr_t(offset);
}
bool currentEngineGfx(void** out){
    if(!out)return false;*out=nullptr;
    const auto descriptor=engineBase+gfxTlsOffset;std::uintptr_t resolver{};
    if(!readMemory(nullptr,descriptor,&resolver,sizeof(resolver))||!resolver)return false;
    // The engine and bionic stay loaded for this process. Revalidate a changed
    // TLSDESC resolver (including lazy resolution), not /proc/self/maps on
    // every HUD state flush. Never cache the per-thread result or TLS slot.
    if(validatedGfxResolver.load(std::memory_order_acquire)!=resolver){
        Mapping code{};
        if(!mapping(resolver,4,code)||!(code.protection&PROT_EXEC))return false;
        validatedGfxResolver.store(resolver,std::memory_order_release);
    }
    // R_AARCH64_TLSDESC returns the offset from TPIDR_EL0; the descriptor is
    // passed in x0, exactly as in the pinned GfxContext::MakeCurrent/Clear.
    using Resolve=std::intptr_t(*)(void*);
    const auto offset=reinterpret_cast<Resolve>(resolver)(reinterpret_cast<void*>(descriptor));
    const auto slot=reinterpret_cast<std::uintptr_t>(__builtin_thread_pointer())+offset;
    // The slot is this thread's own TLS storage, returned by the engine's TLSDESC
    // resolver for the running thread (allocated before it returns), so it stays mapped
    // while this call runs: plain load. An unresolved-weak resolver yields slot 0.
    if(slot>=0x10000&&!(slot%alignof(void*))){
        std::memcpy(out,reinterpret_cast<const void*>(slot),sizeof(*out));++diagnostics::guardedReads.direct;return *out!=nullptr;}
    return readMemory(nullptr,slot,out,sizeof(*out))&&*out;
}
#endif
bool replacePointer(std::uintptr_t address,std::uintptr_t expected,std::uintptr_t replacement) {
    Mapping map{};if(address%sizeof(void*) || !mapping(address,sizeof(void*),map) ||
       !(map.protection&PROT_READ) || (map.protection&PROT_EXEC))return false;
    std::uintptr_t old{};if(!readMemory(nullptr,address,&old,sizeof(old)) || old!=expected)return false;
    const auto pageSize=sysconf(_SC_PAGESIZE);if(pageSize<=0)return false;
    const auto page=address-(address%static_cast<std::uintptr_t>(pageSize));
    if(page<map.begin || std::uintptr_t(pageSize)>map.end-page)return false;
    const bool readonly=!(map.protection&PROT_WRITE);
    if(readonly && mprotect(reinterpret_cast<void*>(page),pageSize,map.protection|PROT_WRITE)!=0)return false;
    auto* slot=reinterpret_cast<std::uintptr_t*>(address);
    bool changed=__atomic_compare_exchange_n(slot,&expected,replacement,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
    if(readonly && mprotect(reinterpret_cast<void*>(page),pageSize,map.protection)!=0) {
        patchFault.store(true);
        // Roll back while still writable, never silently leave the mod's hook on a
        // page whose original protection could not be restored.
        if(changed) {auto ours=replacement;__atomic_compare_exchange_n(slot,&ours,old,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);}
        const bool protectionRestored=mprotect(reinterpret_cast<void*>(page),pageSize,map.protection)==0;
        __android_log_print(ANDROID_LOG_ERROR,"RdrVrTrace","pointer patch protection failure; retry_restored=%d",protectionRestored);
        return false;
    }
    return changed;
}
#include "OrdinaryBootstrap.fragment.inc"
#include "opaque-run/NativeOpaqueRunTrace.fragment.inc"
#include "MainVisibilityWaitTrace.fragment.inc"
#include "EffectMutationTrace.fragment.inc"
#include "ResourceMutationTrace.fragment.inc"
#ifdef RDRVR_HEAD_CAMERA
bool nativeEarlyResourceHooksReady()noexcept {
    return resourceMutationHooksReady.load(std::memory_order_acquire)&&effectMutationHookReady.load(std::memory_order_acquire);
}
#endif
#ifdef RDRVR_HEAD_CAMERA
bool armEarlyResourceCoverage()noexcept {
    const bool ready=effectMutationHookReady.load(std::memory_order_acquire)&&resourceMutationHooksReady.load(std::memory_order_acquire);
    return rdr95::nativeResourceGate.setCoverage(ready?rdr95::NativeResourceGate::Required:0);
}
#endif
#ifdef RDRVR_HEAD_CAMERA
bool refreshEarlyListCoverage()noexcept {
    experimental::nativeEarlyListOwner.coverage(false);
    if(!experimental::nativeEarlyBootstrap.ready()||experimental::nativeEarlyBootstrap.image()!=engineBase||
       !sceneInterposed.load(std::memory_order_acquire)||!mainVisibilityWaitSlotOwned||
       !mainVisibilityWaitHookReady.load(std::memory_order_acquire))return false;
    std::uintptr_t scene{},wait{};
    if(!readMemory(nullptr,engineBase+rdr95::joined_list::sceneGot,&scene,sizeof scene)||scene!=reinterpret_cast<std::uintptr_t>(observeScene)||
       !readMemory(nullptr,engineBase+rdr95::joined_list::waitGot,&wait,sizeof wait)||wait!=reinterpret_cast<std::uintptr_t>(observedMainVisibilityWait))return false;
    auto proof=experimental::nativeEarlyBootstrap.imageProof();
    if(!rdr95::joined_list::codeMatches(engineBase,[&](std::uintptr_t p,unsigned n,const char* sha){return proof.code&&proof.code(proof.context,p,n,sha);}))return false;
    experimental::nativeEarlyListOwner.coverage(true);return true;
}
#endif
VKAPI_ATTR VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo* r,const VkAllocationCallbacks* a,VkInstance* out) {return tracer.createInstance(r,a,out);}
void initializeCameraRuntime(VkPhysicalDevice,VkDevice,std::uint64_t) noexcept;
VKAPI_ATTR VkResult VKAPI_CALL createDevice(VkPhysicalDevice p,const VkDeviceCreateInfo* r,const VkAllocationCallbacks* a,VkDevice* out) {
    const auto result=tracer.createDevice(p,r,a,out);const int saved=errno;
    if(result==VK_SUCCESS&&out&&*out){stereoDevice.store(*out,std::memory_order_release);stereo::deviceCreated(*out,p);}
#ifdef RDRVR_HEAD_CAMERA
    if(result==VK_SUCCESS&&out&&*out){
        // Own certificates from device creation, even if InitVulkan has not
        // finished installing destruction hooks. Culling stays lifecycle-gated.
        static std::atomic<std::uint64_t> epoch{};coldOcclusionRegistry.bind(*out,++epoch);ownedStaticVertices.bind(1);ownedStaticOccluders.bind(1);
        initializeCameraRuntime(p,*out,epoch.load(std::memory_order_relaxed));
    }
    if(result==VK_SUCCESS&&out&&*out&&dynamicNativeHooksReady.load(std::memory_order_acquire)){
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(p,&props);
        if(props.limits.minUniformBufferOffsetAlignment<=UINT32_MAX){
            DynamicUniformDescriptors::Api api{};
            api.createPool=reinterpret_cast<PFN_vkCreateDescriptorPool>(originalDeviceProc(*out,"vkCreateDescriptorPool"));
            api.destroyPool=reinterpret_cast<PFN_vkDestroyDescriptorPool>(originalDeviceProc(*out,"vkDestroyDescriptorPool"));
            api.resetPool=reinterpret_cast<PFN_vkResetDescriptorPool>(originalDeviceProc(*out,"vkResetDescriptorPool"));
            api.allocate=reinterpret_cast<PFN_vkAllocateDescriptorSets>(originalDeviceProc(*out,"vkAllocateDescriptorSets"));
            api.update=reinterpret_cast<PFN_vkUpdateDescriptorSets>(originalDeviceProc(*out,"vkUpdateDescriptorSets"));
            api.destroyLayout=cameraDestroySetLayout;
#ifdef __ANDROID__
            // Cold diagnostic only: prove the PFNs actually supplied to DUD.
            // Owner identity is evidence, never descriptor-pool authority.
            const auto route87=[&](const char*name,PFN_vkVoidFunction fn){
                const auto address=reinterpret_cast<std::uintptr_t>(fn);Dl_info owner{};
                const bool known=address&&dladdr(reinterpret_cast<void*>(address),&owner)&&owner.dli_fbase&&owner.dli_fname;
                const auto base=known?reinterpret_cast<std::uintptr_t>(owner.dli_fbase):std::uintptr_t{};
                const char*file=known?(std::strrchr(owner.dli_fname,'/')?std::strrchr(owner.dli_fname,'/')+1:owner.dli_fname):"unknown";
                __android_log_print(ANDROID_LOG_INFO,"RdrWorker87","route=DUD device=%p api=%s fn=%p owner=%s offset=%llu workerOwned=%u",
                    reinterpret_cast<void*>(*out),name,reinterpret_cast<void*>(address),file,
                    static_cast<unsigned long long>(known&&address>=base?address-base:0),unsigned(rdr2_game_worker::ownsEntry(address)));
            };
            route87("vkCreateDescriptorPool",reinterpret_cast<PFN_vkVoidFunction>(api.createPool));
            route87("vkDestroyDescriptorPool",reinterpret_cast<PFN_vkVoidFunction>(api.destroyPool));
            route87("vkResetDescriptorPool",reinterpret_cast<PFN_vkVoidFunction>(api.resetPool));
            route87("vkAllocateDescriptorSets",reinterpret_cast<PFN_vkVoidFunction>(api.allocate));
            route87("vkUpdateDescriptorSets",reinterpret_cast<PFN_vkVoidFunction>(api.update));
#endif

            // Converted bindings drop UPDATE_AFTER_BIND, so all
            // ordinary per-stage and pipeline-layout limits apply as well.
            const auto uniformLimit=std::min({props.limits.maxDescriptorSetUniformBuffersDynamic,
                props.limits.maxDescriptorSetUniformBuffers,props.limits.maxPerStageDescriptorUniformBuffers,
                props.limits.maxPerStageResources>8?props.limits.maxPerStageResources-8:0u});
            const bool configured92=dynamicUniformDescriptors.configure(*out,api,{uniformLimit,props.limits.maxBoundDescriptorSets,
                static_cast<std::uint32_t>(props.limits.minUniformBufferOffsetAlignment),props.limits.maxUniformBufferRange});
            const bool requested92=descriptor92StartupRequested();
            // Coverage proof = routed vkCreateBuffer/vkDestroyBuffer. The VMA PLT proof does not
            // qualify a create on device and its generic-create path would revoke dispatch-observed
            // buffers, so it stays unbound.
            const bool active92=configured92&&requested92&&dynamicUniformDescriptors.enablePersistentBuffers(*out);
            // Coverage = enablePersistentBuffers result (dispatch-routed buffer lifetimes); the VMA PLT hook state does not gate retention.
            __android_log_print(ANDROID_LOG_INFO,"RdrDescriptor92","device=%p requested=%u configured=%u coverage=%u",reinterpret_cast<void*>(*out),unsigned(requested92),unsigned(configured92),unsigned(active92));
        }
    }
#endif
    errno=saved;return result;
}
VKAPI_ATTR void VKAPI_CALL getQueue(VkDevice d,std::uint32_t family,std::uint32_t index,VkQueue* out) {tracer.getDeviceQueue(d,family,index,out);}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue q,std::uint32_t count,const VkSubmitInfo* in,VkFence fence) {
#ifdef RDRVR_STEREO_BRIDGE
    gameStereoBeforeSubmit(q,count,in); // Also sees the batches (submit gate: stereo pass submit only)
#endif
    const auto result=tracer.queueSubmit(q,count,in,fence);
    uniformSubmitted(count,in,result);
#ifdef RDRVR_STEREO_BRIDGE
    const int saved=errno;gameStereoSubmitted(q,count,in,result);errno=saved;
#endif
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL enumerate(VkInstance i,std::uint32_t* n,VkPhysicalDevice* p) {return tracer.enumeratePhysicalDevices(i,n,p);}
VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance i,const VkAllocationCallbacks* a) {tracer.destroyInstance(i,a);}
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice d,const VkAllocationCallbacks* a) {
#ifdef RDRVR_HEAD_CAMERA
    rdr95::ResourceOriginalErrno nativeError;
    uniformLife92.suspendDevice();
    disarmNativeOpaqueRunHooks();experimental::nativeOrdinaryGeometryEpoch.suspend();experimental::nativeEarlyColdOwners.disarm();
    experimental::nativeEarlyListOwner.coverage(false);effectMutationCoverageDisarm();disarmResourceMutationHooks();nativeQueryCoverageDisarm();
    rdr95::NativeResourceGate::Writer drain(rdr95::nativeResourceGate);
    if(cameraOwnerDevice.load(std::memory_order_acquire)==d){
        cameraRoutesInstalled.store(false,std::memory_order_release);
    experimental::nativeModelProbeReady69.store(false,std::memory_order_release);
    experimental::instanceFrequencyReady67.store(false,std::memory_order_release);
    nativeVertexInputReady58.store(false,std::memory_order_release);nativeVertexStreamMerge58.disable();
        disarmOrdinaryObserver();
        indexedObservation.disarm("TraceEntry.cpp:1049");cameraRuntime.disarm();uniformDeviceDisarm(d);
    }
    // Actual GameXr teardown retains ownership until its users have stopped
    // and originalIdle supplies completion; do not abandon children here.
    nativeQueryGuard.deviceDestroyed(queryHandle(d));errno=nativeError.value;
#endif
    tracer.destroyDevice(d,a);
    {const int afterDestroy=errno;stereo::deviceDestroyed(d);VkDevice expected=d;stereoDevice.compare_exchange_strong(expected,VK_NULL_HANDLE);errno=afterDestroy;}
#ifdef RDRVR_HEAD_CAMERA
    nativeError.value=errno;
#endif
}
VKAPI_ATTR void VKAPI_CALL families(VkPhysicalDevice p,std::uint32_t* n,VkQueueFamilyProperties* f) {tracer.getPhysicalDeviceQueueFamilyProperties(p,n,f);}
#ifdef RDRVR_GAME_SESSION
VKAPI_ATTR VkResult VKAPI_CALL deviceIdle(VkDevice d) {return tracer.deviceWaitIdle(d);}
#endif
#include "CameraTraceBootstrap.fragment.inc"
bool tracedInit() {
    const int incoming=errno;
    std::lock_guard initGuard(initLock);
#ifdef RDRVR_HEAD_CAMERA
    OrdinaryDispatchFailure ordinaryDispatch;
    uniformLife92.suspendDevice();
    // The Observer/Mirror is one-shot. Reinitialization cannot retag old
    // recordings or partially captured creation metadata as a fresh session.
    if(traceConfigured)disarmOrdinaryObserver();
#endif
    cameraRoutesInstalled.store(false,std::memory_order_release);
    if(traceConfigured)experimental::nativeModelProbeReady69.store(false,std::memory_order_release);
    if(traceConfigured)experimental::instanceFrequencyReady67.store(false,std::memory_order_release);
    if(traceConfigured){nativeVertexInputReady58.store(false,std::memory_order_release);nativeVertexStreamMerge58.disable();}
    if(traceConfigured){indexedObservation.disarm("TraceEntry.cpp:1075");cameraRuntime.disarm();uniformCapture.disarm();}
    errno=incoming;
#ifdef RDRVR_HEAD_CAMERA
    disarmNativeOpaqueRunHooks();experimental::nativeOrdinaryGeometryEpoch.suspend();experimental::nativeEarlyColdOwners.disarm();
    experimental::nativeEarlyListOwner.coverage(false);effectMutationCoverageDisarm();disarmResourceMutationHooks();nativeQueryCoverageDisarm();
    executeLifetimeReady.store(false,std::memory_order_release);coldLifecycleReady.store(false,std::memory_order_release);runtime::graphicsOcclusionAvailable.store(false,std::memory_order_release);
#endif
    const bool result=[] {
#ifdef RDRVR_HEAD_CAMERA
        rdr95::ResourceOriginalErrno nativeError;
        rdr95::NativeResourceGate::Writer drain(rdr95::nativeResourceGate);
        errno=nativeError.value;
        try{const bool result=originalInit();nativeError.value=errno;return result;}
        catch(...){nativeError.value=errno;throw;}
#else
        return originalInit();
#endif
    }(); // Exact no-argument bool ABI; no borrow can overlap reinitialization.
    struct RestoreErrno {int value;~RestoreErrno(){errno=value;}} restoreErrno{errno};
    const auto count=++initCalls;
    try {
        if(!result) {state("ORIGINAL_INIT_FAILED");return result;}
#ifdef RDRVR_HEAD_CAMERA
        if(!rearmEffectMutationHooks())effectMutationCoverageDisarm();
        if(!rearmResourceMutationHooks())disarmResourceMutationHooks();
        armEarlyResourceCoverage();
        refreshEarlyListCoverage();
        if(experimental::NativeEarlyTraceRoute::ObserveOpaqueRuns&&!rearmNativeOpaqueRunHooks())disarmNativeOpaqueRunHooks();
        if(!experimental::nativeEarlyColdOwners.rearm()||!experimental::nativeOrdinaryGeometryEpoch.arm(experimental::nativeEarlyColdOwners)){experimental::nativeOrdinaryGeometryEpoch.suspend();experimental::nativeEarlyColdOwners.disarm();}
#endif
        std::array<std::uintptr_t,dispatchCount> offsets{vkCreateInstanceOffset,vkCreateDeviceOffset,vkGetDeviceQueueOffset,vkQueueSubmitOffset,vkEnumeratePhysicalDevicesOffset,vkDestroyInstanceOffset,vkDestroyDeviceOffset,vkGetPhysicalDeviceQueueFamilyPropertiesOffset
#ifdef RDRVR_GAME_SESSION
            ,vkDeviceWaitIdleOffset,vkCreateSwapchainKHROffset,vkDestroySwapchainKHROffset,vkQueuePresentKHROffset
#ifdef RDRVR_PASS_RESOURCES
            ,vkCreateImageOffset,vkDestroyImageOffset,vkGetInstanceProcAddrOffset,vkGetDeviceProcAddrOffset
            ,vkCreateCommandPoolOffset,vkDestroyCommandPoolOffset,vkAllocateCommandBuffersOffset,vkFreeCommandBuffersOffset,vkBeginCommandBufferOffset,vkEndCommandBufferOffset,vkResetCommandBufferOffset,vkResetCommandPoolOffset
#ifdef RDRVR_BENCH_CONTROL
            ,vkCreateShaderModuleOffset
#ifdef RDRVR_FRAGMENT_RATE
            ,vkCreateRenderPassOffset,vkDestroyRenderPassOffset,vkCreateFramebufferOffset,vkCreateGraphicsPipelinesOffset,vkCreateImageViewOffset,vkDestroyImageViewOffset
#endif
#endif
#endif
#endif
#ifdef RDRVR_HEAD_CAMERA

            ,indexedSlot_vkCmdBindVertexBuffers
            ,indexedSlot_vkCmdBindIndexBuffer
            ,indexedSlot_vkCmdSetViewport
            ,indexedSlot_vkCmdSetScissor
            ,indexedSlot_vkCmdSetLineWidth
            ,indexedSlot_vkCmdSetDepthBias
            ,indexedSlot_vkCmdSetBlendConstants
            ,indexedSlot_vkCmdSetDepthBounds
            ,indexedSlot_vkCmdSetStencilCompareMask
            ,indexedSlot_vkCmdSetStencilWriteMask
            ,indexedSlot_vkCmdSetStencilReference
            ,indexedSlot_vkCmdPushConstants
            ,indexedSlot_vkCmdBeginRenderPass
            ,indexedSlot_vkCmdEndRenderPass
            ,indexedSlot_vkCmdNextSubpass
            ,indexedSlot_vkCmdDrawIndirect
            ,indexedSlot_vkCmdDrawIndexedIndirect
            ,indexedSlot_vkCmdDispatch
            ,indexedSlot_vkCmdDispatchIndirect
            ,indexedSlot_vkCmdCopyBuffer
            ,indexedSlot_vkCmdCopyImage
            ,indexedSlot_vkCmdBlitImage
            ,indexedSlot_vkCmdCopyBufferToImage
            ,indexedSlot_vkCmdCopyImageToBuffer
            ,indexedSlot_vkCmdUpdateBuffer
            ,indexedSlot_vkCmdFillBuffer
            ,indexedSlot_vkCmdClearColorImage
            ,indexedSlot_vkCmdClearDepthStencilImage
            ,indexedSlot_vkCmdClearAttachments
            ,indexedSlot_vkCmdResolveImage
            ,indexedSlot_vkCmdSetEvent
            ,indexedSlot_vkCmdResetEvent
            ,indexedSlot_vkCmdWaitEvents
            ,indexedSlot_vkCmdPipelineBarrier
            ,indexedSlot_vkCmdResetQueryPool
            ,indexedSlot_vkCmdWriteTimestamp
            ,indexedSlot_vkCmdCopyQueryPoolResults
            ,vkCreateDescriptorSetLayoutOffset,vkDestroyDescriptorSetLayoutOffset,vkCmdBindDescriptorSetsOffset
            ,cameraCreateLayoutSlot,cameraDestroyLayoutSlot,cameraBindPipelineSlot,cameraDrawSlot,cameraDrawIndexedSlot
            ,vkDestroyShaderModuleOffset,vkDestroyPipelineOffset,vkCmdExecuteCommandsOffset
#endif
        },old{},replacements{
            reinterpret_cast<std::uintptr_t>(createInstance),reinterpret_cast<std::uintptr_t>(createDevice),reinterpret_cast<std::uintptr_t>(getQueue),reinterpret_cast<std::uintptr_t>(submit),reinterpret_cast<std::uintptr_t>(enumerate),reinterpret_cast<std::uintptr_t>(destroyInstance),reinterpret_cast<std::uintptr_t>(destroyDevice),reinterpret_cast<std::uintptr_t>(families)
#ifdef RDRVR_GAME_SESSION
            ,reinterpret_cast<std::uintptr_t>(deviceIdle),reinterpret_cast<std::uintptr_t>(gameCreateSwapchain),reinterpret_cast<std::uintptr_t>(gameDestroySwapchain),reinterpret_cast<std::uintptr_t>(gamePresent)
#ifdef RDRVR_PASS_RESOURCES
            ,reinterpret_cast<std::uintptr_t>(createImage),reinterpret_cast<std::uintptr_t>(destroyImage),reinterpret_cast<std::uintptr_t>(instanceProc),reinterpret_cast<std::uintptr_t>(deviceProc)
            ,reinterpret_cast<std::uintptr_t>(createPool),reinterpret_cast<std::uintptr_t>(destroyPool),reinterpret_cast<std::uintptr_t>(allocateCommands),reinterpret_cast<std::uintptr_t>(freeCommands),reinterpret_cast<std::uintptr_t>(beginCommand),reinterpret_cast<std::uintptr_t>(endCommand),reinterpret_cast<std::uintptr_t>(resetCommand),reinterpret_cast<std::uintptr_t>(resetPool)
#ifdef RDRVR_BENCH_CONTROL
            ,reinterpret_cast<std::uintptr_t>(createShader)
#ifdef RDRVR_FRAGMENT_RATE
            ,reinterpret_cast<std::uintptr_t>(rateCreatePass),reinterpret_cast<std::uintptr_t>(rateDestroyPass),reinterpret_cast<std::uintptr_t>(rateCreateFramebuffer),reinterpret_cast<std::uintptr_t>(rateCreatePipelines),reinterpret_cast<std::uintptr_t>(rateCreateView),reinterpret_cast<std::uintptr_t>(rateDestroyView)
#endif
#endif
#endif
#endif
#ifdef RDRVR_HEAD_CAMERA

            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdBindVertexBuffers)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdBindIndexBuffer)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdSetViewport)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdSetScissor)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdSetLineWidth)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdSetDepthBias)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdSetBlendConstants)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdSetDepthBounds)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdSetStencilCompareMask)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdSetStencilWriteMask)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdSetStencilReference)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdPushConstants)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdBeginRenderPass)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdEndRenderPass)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdNextSubpass)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdDrawIndirect)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdDrawIndexedIndirect)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdDispatch)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdDispatchIndirect)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdCopyBuffer)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdCopyImage)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdBlitImage)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdCopyBufferToImage)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdCopyImageToBuffer)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdUpdateBuffer)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdFillBuffer)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdClearColorImage)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdClearDepthStencilImage)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdClearAttachments)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdResolveImage)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdSetEvent)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdResetEvent)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdWaitEvents)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdPipelineBarrier)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdResetQueryPool)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdWriteTimestamp)
            ,reinterpret_cast<std::uintptr_t>(indexed_vkCmdCopyQueryPoolResults)
            ,reinterpret_cast<std::uintptr_t>(dynamicCreateLayout),reinterpret_cast<std::uintptr_t>(dynamicDestroyLayout),reinterpret_cast<std::uintptr_t>(dynamicBind)
            ,reinterpret_cast<std::uintptr_t>(cameraCreatePipelineLayout),reinterpret_cast<std::uintptr_t>(cameraDestroyPipelineLayout)
            ,reinterpret_cast<std::uintptr_t>(ablateBindPipeline),reinterpret_cast<std::uintptr_t>(ablateDraw),reinterpret_cast<std::uintptr_t>(ablateDrawIndexed)
            ,reinterpret_cast<std::uintptr_t>(coldDestroyShader),reinterpret_cast<std::uintptr_t>(coldDestroyPipeline),reinterpret_cast<std::uintptr_t>(dynamicExecute)
#endif
        };
        for(unsigned i=0;i<offsets.size();++i) {
            if(!readMemory(nullptr,engineBase+offsets[i],&old[i],sizeof(old[i]))) {state("DISPATCH_UNREADABLE");return result;}
            Dl_info owner{};Mapping map{};
            if(!old[i] || !dladdr(reinterpret_cast<void*>(old[i]),&owner) || !owner.dli_fname ||
               (std::strcmp(std::strrchr(owner.dli_fname,'/')?std::strrchr(owner.dli_fname,'/')+1:owner.dli_fname,"libvulkan.so")!=0 && !rdr2_game_worker::ownsEntry(old[i])) ||
               !mapping(old[i],1,map) || !(map.protection&PROT_EXEC)) {state("DISPATCH_OWNER_REJECTED");return result;}
        }
        if(traceConfigured && old!=originalDispatch) {state("DISPATCH_CHANGED_ON_REINIT");return result;}
        if(!traceConfigured) {
            diagnostics::TraceDispatch dispatch{reinterpret_cast<PFN_vkCreateInstance>(old[0]),reinterpret_cast<PFN_vkCreateDevice>(old[1]),
                reinterpret_cast<PFN_vkGetDeviceQueue>(old[2]),reinterpret_cast<PFN_vkQueueSubmit>(old[3]),
                reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(old[4]),reinterpret_cast<PFN_vkDestroyInstance>(old[5]),
                reinterpret_cast<PFN_vkDestroyDevice>(old[6]),reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(old[7])
#ifdef RDRVR_GAME_SESSION
                ,reinterpret_cast<PFN_vkDeviceWaitIdle>(old[8])
#endif
            };
#ifdef RDRVR_GAME_XR
            std::uintptr_t getProc{};
            Dl_info getProcOwner{};Mapping getProcMap{};
            if(readMemory(nullptr,engineBase+vkGetInstanceProcAddrOffset,&getProc,sizeof(getProc)) && getProc &&
               dladdr(reinterpret_cast<void*>(getProc),&getProcOwner) && getProcOwner.dli_fname &&
               (std::strcmp(std::strrchr(getProcOwner.dli_fname,'/')?std::strrchr(getProcOwner.dli_fname,'/')+1:getProcOwner.dli_fname,"libvulkan.so")==0 || rdr2_game_worker::ownsEntry(getProc)) &&
               mapping(getProc,1,getProcMap) && (getProcMap.protection&PROT_EXEC))
                cameraGameDeviceRoute.store(gameXrRoute(dispatch,reinterpret_cast<PFN_vkGetInstanceProcAddr>(getProc)),std::memory_order_release);
#endif
#ifdef RDRVR_GAME_SESSION
            gameTheaterRoute(reinterpret_cast<PFN_vkCreateSwapchainKHR>(old[9]),reinterpret_cast<PFN_vkDestroySwapchainKHR>(old[10]),reinterpret_cast<PFN_vkQueuePresentKHR>(old[11]));
#endif
#ifdef RDRVR_PASS_RESOURCES
            originalCreateImage=reinterpret_cast<PFN_vkCreateImage>(old[12]);originalDestroyImage=reinterpret_cast<PFN_vkDestroyImage>(old[13]);
            originalInstanceProc=reinterpret_cast<PFN_vkGetInstanceProcAddr>(old[14]);originalDeviceProc=reinterpret_cast<PFN_vkGetDeviceProcAddr>(old[15]);
            originalCreatePool=reinterpret_cast<PFN_vkCreateCommandPool>(old[16]);originalDestroyPool=reinterpret_cast<PFN_vkDestroyCommandPool>(old[17]);
            originalAllocateCommands=reinterpret_cast<PFN_vkAllocateCommandBuffers>(old[18]);originalFreeCommands=reinterpret_cast<PFN_vkFreeCommandBuffers>(old[19]);
            originalBeginCommand=reinterpret_cast<PFN_vkBeginCommandBuffer>(old[20]);originalEndCommand=reinterpret_cast<PFN_vkEndCommandBuffer>(old[21]);
            originalResetCommand=reinterpret_cast<PFN_vkResetCommandBuffer>(old[22]);originalResetPool=reinterpret_cast<PFN_vkResetCommandPool>(old[23]);
#ifdef RDRVR_BENCH_CONTROL
            originalCreateShader=reinterpret_cast<PFN_vkCreateShaderModule>(old[24]);
#ifdef RDRVR_FRAGMENT_RATE
            originalRateCreatePass=reinterpret_cast<PFN_vkCreateRenderPass>(old[25]);originalRateDestroyPass=reinterpret_cast<PFN_vkDestroyRenderPass>(old[26]);
            originalRateCreateFramebuffer=reinterpret_cast<PFN_vkCreateFramebuffer>(old[27]);originalRateCreatePipelines=reinterpret_cast<PFN_vkCreateGraphicsPipelines>(old[28]);
            originalRateCreateView=reinterpret_cast<PFN_vkCreateImageView>(old[29]);originalRateDestroyView=reinterpret_cast<PFN_vkDestroyImageView>(old[30]);
#endif
#endif
#endif
#ifdef RDRVR_HEAD_CAMERA
            originalDynamicCreateLayout=reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(old[dispatchCount-cameraDispatchCount-3]);
            originalDynamicDestroyLayout=reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(old[dispatchCount-cameraDispatchCount-2]);
            originalDynamicBind=reinterpret_cast<PFN_vkCmdBindDescriptorSets>(old[dispatchCount-cameraDispatchCount-1]);
#endif
            originalCameraCreateLayout=reinterpret_cast<PFN_vkCreatePipelineLayout>(old[dispatchCount-8]);
            originalCameraDestroyLayout=reinterpret_cast<PFN_vkDestroyPipelineLayout>(old[dispatchCount-7]);
            originalCameraBindPipeline=reinterpret_cast<PFN_vkCmdBindPipeline>(old[dispatchCount-6]);
            originalCameraDraw=reinterpret_cast<PFN_vkCmdDraw>(old[dispatchCount-5]);
            originalCameraDrawIndexed=reinterpret_cast<PFN_vkCmdDrawIndexed>(old[dispatchCount-4]);
            originalColdDestroyShader=reinterpret_cast<PFN_vkDestroyShaderModule>(old[dispatchCount-3]);
            originalColdDestroyPipeline=reinterpret_cast<PFN_vkDestroyPipeline>(old[dispatchCount-2]);
            originalDynamicExecute=reinterpret_cast<PFN_vkCmdExecuteCommands>(old[dispatchCount-1]);
            indexedRaw_vkCmdBindVertexBuffers=reinterpret_cast<PFN_vkCmdBindVertexBuffers>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+0]);
            indexedRaw_vkCmdBindIndexBuffer=reinterpret_cast<PFN_vkCmdBindIndexBuffer>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+1]);
            indexedRaw_vkCmdSetViewport=reinterpret_cast<PFN_vkCmdSetViewport>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+2]);
            indexedRaw_vkCmdSetScissor=reinterpret_cast<PFN_vkCmdSetScissor>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+3]);
            indexedRaw_vkCmdSetLineWidth=reinterpret_cast<PFN_vkCmdSetLineWidth>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+4]);
            indexedRaw_vkCmdSetDepthBias=reinterpret_cast<PFN_vkCmdSetDepthBias>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+5]);
            indexedRaw_vkCmdSetBlendConstants=reinterpret_cast<PFN_vkCmdSetBlendConstants>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+6]);
            indexedRaw_vkCmdSetDepthBounds=reinterpret_cast<PFN_vkCmdSetDepthBounds>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+7]);
            indexedRaw_vkCmdSetStencilCompareMask=reinterpret_cast<PFN_vkCmdSetStencilCompareMask>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+8]);
            indexedRaw_vkCmdSetStencilWriteMask=reinterpret_cast<PFN_vkCmdSetStencilWriteMask>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+9]);
            indexedRaw_vkCmdSetStencilReference=reinterpret_cast<PFN_vkCmdSetStencilReference>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+10]);
            indexedRaw_vkCmdPushConstants=reinterpret_cast<PFN_vkCmdPushConstants>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+11]);
            indexedRaw_vkCmdBeginRenderPass=reinterpret_cast<PFN_vkCmdBeginRenderPass>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+12]);
            indexedRaw_vkCmdEndRenderPass=reinterpret_cast<PFN_vkCmdEndRenderPass>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+13]);
            indexedRaw_vkCmdNextSubpass=reinterpret_cast<PFN_vkCmdNextSubpass>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+14]);
            indexedRaw_vkCmdDrawIndirect=reinterpret_cast<PFN_vkCmdDrawIndirect>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+15]);
            indexedRaw_vkCmdDrawIndexedIndirect=reinterpret_cast<PFN_vkCmdDrawIndexedIndirect>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+16]);
            indexedRaw_vkCmdDispatch=reinterpret_cast<PFN_vkCmdDispatch>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+17]);
            indexedRaw_vkCmdDispatchIndirect=reinterpret_cast<PFN_vkCmdDispatchIndirect>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+18]);
            indexedRaw_vkCmdCopyBuffer=reinterpret_cast<PFN_vkCmdCopyBuffer>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+19]);
            indexedRaw_vkCmdCopyImage=reinterpret_cast<PFN_vkCmdCopyImage>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+20]);
            indexedRaw_vkCmdBlitImage=reinterpret_cast<PFN_vkCmdBlitImage>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+21]);
            indexedRaw_vkCmdCopyBufferToImage=reinterpret_cast<PFN_vkCmdCopyBufferToImage>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+22]);
            indexedRaw_vkCmdCopyImageToBuffer=reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+23]);
            indexedRaw_vkCmdUpdateBuffer=reinterpret_cast<PFN_vkCmdUpdateBuffer>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+24]);
            indexedRaw_vkCmdFillBuffer=reinterpret_cast<PFN_vkCmdFillBuffer>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+25]);
            indexedRaw_vkCmdClearColorImage=reinterpret_cast<PFN_vkCmdClearColorImage>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+26]);
            indexedRaw_vkCmdClearDepthStencilImage=reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+27]);
            indexedRaw_vkCmdClearAttachments=reinterpret_cast<PFN_vkCmdClearAttachments>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+28]);
            indexedRaw_vkCmdResolveImage=reinterpret_cast<PFN_vkCmdResolveImage>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+29]);
            indexedRaw_vkCmdSetEvent=reinterpret_cast<PFN_vkCmdSetEvent>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+30]);
            indexedRaw_vkCmdResetEvent=reinterpret_cast<PFN_vkCmdResetEvent>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+31]);
            indexedRaw_vkCmdWaitEvents=reinterpret_cast<PFN_vkCmdWaitEvents>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+32]);
            indexedRaw_vkCmdPipelineBarrier=reinterpret_cast<PFN_vkCmdPipelineBarrier>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+33]);
            indexedRaw_vkCmdResetQueryPool=reinterpret_cast<PFN_vkCmdResetQueryPool>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+34]);
            indexedRaw_vkCmdWriteTimestamp=reinterpret_cast<PFN_vkCmdWriteTimestamp>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+35]);
            indexedRaw_vkCmdCopyQueryPoolResults=reinterpret_cast<PFN_vkCmdCopyQueryPoolResults>(old[dispatchCount-cameraDispatchCount-3-indexedDispatchCount+36]);
            if(!bindIndexedObservationOriginals()){state("INDEXED_RAW_BOOTSTRAP_DECLINED");return result;}
            if(!bindCameraRuntimeOriginals(reinterpret_cast<PFN_vkQueueSubmit>(old[3]))){state("CAMERA_RAW_BOOTSTRAP_DECLINED");return result;}
            // Tracer still measures one queue submission; the composed terminal
            // forwards once to the captured raw PFN and retains camera storage.
            dispatch.submit=cameraSubmit;
            tracer.configure(dispatch,{nullptr,readMemory},{nullptr,event,now,tid});
            originalDispatch=old;traceConfigured=true;
        }
        unsigned changed=0;
        for(;changed<offsets.size();++changed)if(!replacePointer(engineBase+offsets[changed],old[changed],replacements[changed]))break;
        if(changed!=offsets.size()) {
            bool restored=true;
            while(changed) {--changed;restored &= replacePointer(engineBase+offsets[changed],replacements[changed],old[changed]);}
            if(!restored)patchFault.store(true);
            state(restored?"DISPATCH_PATCH_DECLINED":"DISPATCH_ROLLBACK_INCOMPLETE");
        } else {
#ifdef RDRVR_HEAD_CAMERA
            executeLifetimeReady.store(true,std::memory_order_release);
            coldLifecycleReady.store(true,std::memory_order_release);
            cameraRoutesInstalled.store(true,std::memory_order_release);
            // Query coverage is still disabled here. Publish the combined
            // readiness only after the query hook transaction below.
#endif
#ifdef RDRVR_HEAD_CAMERA
            // Pinned retail symbol VARIABLES, not their GOT indirections. The
            // native query functions load these two dispatch slots.
            const std::array<std::uintptr_t,2> queryOffsets{0x4e694158,0x4e694160};
            const std::array<std::uintptr_t,2> queryWrappers{reinterpret_cast<std::uintptr_t>(observedBeginQuery),reinterpret_cast<std::uintptr_t>(observedEndQuery)};
            std::array<std::uintptr_t,2> queryOriginals{};bool queryValid=executeLifetimeReady.load(std::memory_order_acquire)&&
                nativeQueryCodeMatches(engineBase,[](std::uintptr_t p,void* bytes,std::size_t n){Mapping code{};return mapping(p,n,code)&&(code.protection&PROT_EXEC)&&readMemory(nullptr,p,bytes,n);});
            for(unsigned i=0;i<2;++i){Dl_info owner{};Mapping map{};
                queryValid=queryValid&&readMemory(nullptr,engineBase+queryOffsets[i],&queryOriginals[i],sizeof(std::uintptr_t))&&queryOriginals[i]&&
                    dladdr(reinterpret_cast<void*>(queryOriginals[i]),&owner)&&owner.dli_fname&&
                    (!std::strcmp(std::strrchr(owner.dli_fname,'/')?std::strrchr(owner.dli_fname,'/')+1:owner.dli_fname,"libvulkan.so") || rdr2_game_worker::ownsEntry(queryOriginals[i]))&&
                    mapping(queryOriginals[i],1,map)&&(map.protection&PROT_EXEC);
            }
            unsigned queryPatched{};
            if(queryValid){originalObservedBeginQuery=reinterpret_cast<PFN_vkCmdBeginQuery>(queryOriginals[0]);originalObservedEndQuery=reinterpret_cast<PFN_vkCmdEndQuery>(queryOriginals[1]);
                while(queryPatched<2&&replacePointer(engineBase+queryOffsets[queryPatched],queryOriginals[queryPatched],queryWrappers[queryPatched]))++queryPatched;}
            if(queryPatched==2){nativeQueryCoverageArm();stereo::boundaryQueriesObserved.store(true,std::memory_order_release);} // Left splits need to see the engine's queries
            else while(queryPatched){--queryPatched;if(!replacePointer(engineBase+queryOffsets[queryPatched],queryWrappers[queryPatched],queryOriginals[queryPatched]))patchFault=true;}
            // Stereo replay: bind every raw driver entry it emits, then publish
            // the six additional lifetime slots as one rollback group.
            {
                const std::array<std::uintptr_t,7> stereoSlots{0x4e693fc8,0x4e693fd8,0x4e693fd0,0x4e693fc0,0x4e693fb8,0x4e693fe8,0x4e693fb0};
                std::array<std::uintptr_t,7> stereoOriginals{};bool stereoValid=true;
                for(unsigned i=0;i<stereoSlots.size();++i){Dl_info owner{};Mapping map{};
                    stereoValid=stereoValid&&readMemory(nullptr,engineBase+stereoSlots[i],&stereoOriginals[i],sizeof(std::uintptr_t))&&stereoOriginals[i]&&
                        dladdr(reinterpret_cast<void*>(stereoOriginals[i]),&owner)&&owner.dli_fname&&
                        (!std::strcmp(std::strrchr(owner.dli_fname,'/')?std::strrchr(owner.dli_fname,'/')+1:owner.dli_fname,"libvulkan.so")||rdr2_game_worker::ownsEntry(stereoOriginals[i]))&&
                        mapping(stereoOriginals[i],1,map)&&(map.protection&PROT_EXEC);
                }
                if(stereoValid){
                    originalStereoAllocateSets=reinterpret_cast<PFN_vkAllocateDescriptorSets>(stereoOriginals[0]);
                    originalStereoUpdateSets=reinterpret_cast<PFN_vkUpdateDescriptorSets>(stereoOriginals[1]);
                    originalStereoFreeSets=reinterpret_cast<PFN_vkFreeDescriptorSets>(stereoOriginals[2]);
                    originalStereoResetPool=reinterpret_cast<PFN_vkResetDescriptorPool>(stereoOriginals[3]);
                    originalStereoDestroyPool=reinterpret_cast<PFN_vkDestroyDescriptorPool>(stereoOriginals[4]);
                    originalStereoDestroyFramebuffer=reinterpret_cast<PFN_vkDestroyFramebuffer>(stereoOriginals[5]);
                    stereo::RawCommands rawStereo{};
                    rawStereo.bindPipeline=originalCameraBindPipeline;rawStereo.bindDescriptorSets=originalDynamicBind;
                    rawStereo.bindVertexBuffers=indexedRaw_vkCmdBindVertexBuffers;rawStereo.bindIndexBuffer=indexedRaw_vkCmdBindIndexBuffer;
                    rawStereo.setViewport=indexedRaw_vkCmdSetViewport;rawStereo.setScissor=indexedRaw_vkCmdSetScissor;rawStereo.setLineWidth=indexedRaw_vkCmdSetLineWidth;
                    rawStereo.setDepthBias=indexedRaw_vkCmdSetDepthBias;rawStereo.setBlendConstants=indexedRaw_vkCmdSetBlendConstants;rawStereo.setDepthBounds=indexedRaw_vkCmdSetDepthBounds;
                    rawStereo.setStencilCompareMask=indexedRaw_vkCmdSetStencilCompareMask;rawStereo.setStencilWriteMask=indexedRaw_vkCmdSetStencilWriteMask;rawStereo.setStencilReference=indexedRaw_vkCmdSetStencilReference;
                    rawStereo.pushConstants=indexedRaw_vkCmdPushConstants;rawStereo.beginRenderPass=indexedRaw_vkCmdBeginRenderPass;rawStereo.endRenderPass=indexedRaw_vkCmdEndRenderPass;rawStereo.nextSubpass=indexedRaw_vkCmdNextSubpass;
                    rawStereo.draw=originalCameraDraw;rawStereo.drawIndexed=originalCameraDrawIndexed;rawStereo.drawIndirect=indexedRaw_vkCmdDrawIndirect;rawStereo.drawIndexedIndirect=indexedRaw_vkCmdDrawIndexedIndirect;
                    rawStereo.pipelineBarrier=indexedRaw_vkCmdPipelineBarrier;rawStereo.copyImage=indexedRaw_vkCmdCopyImage;rawStereo.copyBuffer=indexedRaw_vkCmdCopyBuffer;
                    rawStereo.copyBufferToImage=indexedRaw_vkCmdCopyBufferToImage;rawStereo.copyImageToBuffer=indexedRaw_vkCmdCopyImageToBuffer;rawStereo.blitImage=indexedRaw_vkCmdBlitImage;
                    rawStereo.clearAttachments=indexedRaw_vkCmdClearAttachments;rawStereo.clearColorImage=indexedRaw_vkCmdClearColorImage;rawStereo.clearDepthStencilImage=indexedRaw_vkCmdClearDepthStencilImage;
                    shadowupdate::stagger::layer::bindRawClear(indexedRaw_vkCmdClearAttachments); // The atlas quadrant clear
                    shadowupdate::stagger::layer::bindRawScissor(indexedRaw_vkCmdSetScissor); // The player window scissor
                    rawStereo.resolveImage=indexedRaw_vkCmdResolveImage;rawStereo.fillBuffer=indexedRaw_vkCmdFillBuffer;rawStereo.updateBuffer=indexedRaw_vkCmdUpdateBuffer;
                    rawStereo.createPipelineLayout=originalCameraCreateLayout;rawStereo.destroyPipelineLayout=originalCameraDestroyLayout;
                    rawStereo.createFramebuffer=originalRateCreateFramebuffer;rawStereo.destroyFramebuffer=originalStereoDestroyFramebuffer;
                    rawStereo.createDescriptorPool=reinterpret_cast<PFN_vkCreateDescriptorPool>(stereoOriginals[6]);rawStereo.destroyDescriptorPool=originalStereoDestroyPool;
                    rawStereo.allocateDescriptorSets=originalStereoAllocateSets;rawStereo.freeDescriptorSets=originalStereoFreeSets;rawStereo.updateDescriptorSets=originalStereoUpdateSets;
                    rawStereo.executeCommands=originalDynamicExecute; // original vkCmdExecuteCommands slot (secondaries path, debug.rdrvr.stereoreplay.async=1)
                    const std::array<std::uintptr_t,6> stereoWrappers{reinterpret_cast<std::uintptr_t>(stereoAllocateSets),reinterpret_cast<std::uintptr_t>(stereoUpdateSets),
                        reinterpret_cast<std::uintptr_t>(stereoFreeSets),reinterpret_cast<std::uintptr_t>(stereoResetPool),reinterpret_cast<std::uintptr_t>(stereoDestroyPool),reinterpret_cast<std::uintptr_t>(stereoDestroyFramebuffer)};
                    unsigned stereoPatched{};
                    stereo::setSafeReader(readMemory,nullptr); // Driver-object reads (probe self-check, [view+0x1b8]) never fault
                    if(stereo::bindRaw(rawStereo))while(stereoPatched<6&&replacePointer(engineBase+stereoSlots[stereoPatched],stereoOriginals[stereoPatched],stereoWrappers[stereoPatched]))++stereoPatched;
                    if(stereoPatched!=6)while(stereoPatched){--stereoPatched;if(!replacePointer(engineBase+stereoSlots[stereoPatched],stereoWrappers[stereoPatched],stereoOriginals[stereoPatched]))patchFault=true;}
                    __android_log_print(ANDROID_LOG_INFO,"RdrStereoReplay","dispatch patched=%u enabled=%u",stereoPatched,unsigned(stereo::enabled()));
                }
            }

#endif
#ifdef RDRVR_HEAD_CAMERA
            // bindIndexedObservationOriginals and bindCameraRuntimeOriginals
            // succeeded before the first CAS. Reach this point only after
            // every main dispatch slot and the query observer stage complete.
            refreshOcclusionReady();
            ordinaryDispatch.committed=armOrdinaryObserverAfterDispatch();
#endif
            std::lock_guard guard(lock);armed=true;status="TRACING";
        }
        event(nullptr,"{\"event\":\"InitVulkanReturned\",\"generation\":"+std::to_string(count)+",\"threadId\":"+std::to_string(tid())+",\"timeNs\":"+std::to_string(now())+"}");
    } catch(...) { /* Preserve native initialization outcome. */ }
    return result;
}
template<std::size_t N> bool fingerprint(std::uintptr_t p,const std::array<unsigned char,N>& expected) {
    std::array<unsigned char,N> actual{};return readMemory(nullptr,p,actual.data(),actual.size()) && actual==expected;
}
void arm(void* entry) {
    static std::mutex coldArm;
    static bool coldAttempted{};
    std::lock_guard coldGuard(coldArm);
    const bool firstColdAttempt=!coldAttempted;
    coldAttempted=true;
    if(pltInstalled.load(std::memory_order_acquire))return;
    if(!verification.ready()) {state("MISSED_HASH_PENDING");return;}
    std::uint32_t known=0x72647231,copy{};
    if(!memory.initialize() || !memory.read(reinterpret_cast<std::uintptr_t>(&known),&copy,sizeof(copy)) || copy!=known) {
        state("READ_UNAVAILABLE");return;
    }
    Dl_info info{};
    if(!dladdr(entry,&info) || !info.dli_fbase || !info.dli_fname || enginePath!=info.dli_fname) {state("MAPPED_PATH_REJECTED");return;}
    engineBase=reinterpret_cast<std::uintptr_t>(info.dli_fbase);
    if(reinterpret_cast<std::uintptr_t>(entry)!=engineBase+entryOffset || !fingerprint(engineBase+entryOffset,entryFingerprint) ||
       !fingerprint(engineBase+initOffset,initFingerprint)) {state("FINGERPRINT_REJECTED");return;}
    // Pinned engine identified; reads inside its segments become plain copies.
    configureDirectReads(engineBase);
    diagnostics::guardedReadVector.configure(readMemory,readMemoryVector);
    // If Vulkan is already initialized, do not pretend that creation can still be captured.
    for(auto offset:{vkCreateInstanceOffset,vkCreateDeviceOffset,vkGetDeviceQueueOffset,vkQueueSubmitOffset,vkEnumeratePhysicalDevicesOffset,vkDestroyInstanceOffset,vkDestroyDeviceOffset,vkGetPhysicalDeviceQueueFamilyPropertiesOffset
#ifdef RDRVR_GAME_SESSION
            ,vkDeviceWaitIdleOffset,vkCreateSwapchainKHROffset,vkDestroySwapchainKHROffset,vkQueuePresentKHROffset
#ifdef RDRVR_PASS_RESOURCES
            ,vkCreateImageOffset,vkDestroyImageOffset
            ,vkCreateCommandPoolOffset,vkDestroyCommandPoolOffset,vkAllocateCommandBuffersOffset,vkFreeCommandBuffersOffset,vkBeginCommandBufferOffset,vkEndCommandBufferOffset,vkResetCommandBufferOffset,vkResetCommandPoolOffset
#ifdef RDRVR_BENCH_CONTROL
            ,vkCreateShaderModuleOffset
#endif
#endif
#endif
#ifdef RDRVR_HEAD_CAMERA

            ,indexedSlot_vkCmdBindVertexBuffers
            ,indexedSlot_vkCmdBindIndexBuffer
            ,indexedSlot_vkCmdSetViewport
            ,indexedSlot_vkCmdSetScissor
            ,indexedSlot_vkCmdSetLineWidth
            ,indexedSlot_vkCmdSetDepthBias
            ,indexedSlot_vkCmdSetBlendConstants
            ,indexedSlot_vkCmdSetDepthBounds
            ,indexedSlot_vkCmdSetStencilCompareMask
            ,indexedSlot_vkCmdSetStencilWriteMask
            ,indexedSlot_vkCmdSetStencilReference
            ,indexedSlot_vkCmdPushConstants
            ,indexedSlot_vkCmdBeginRenderPass
            ,indexedSlot_vkCmdEndRenderPass
            ,indexedSlot_vkCmdNextSubpass
            ,indexedSlot_vkCmdDrawIndirect
            ,indexedSlot_vkCmdDrawIndexedIndirect
            ,indexedSlot_vkCmdDispatch
            ,indexedSlot_vkCmdDispatchIndirect
            ,indexedSlot_vkCmdCopyBuffer
            ,indexedSlot_vkCmdCopyImage
            ,indexedSlot_vkCmdBlitImage
            ,indexedSlot_vkCmdCopyBufferToImage
            ,indexedSlot_vkCmdCopyImageToBuffer
            ,indexedSlot_vkCmdUpdateBuffer
            ,indexedSlot_vkCmdFillBuffer
            ,indexedSlot_vkCmdClearColorImage
            ,indexedSlot_vkCmdClearDepthStencilImage
            ,indexedSlot_vkCmdClearAttachments
            ,indexedSlot_vkCmdResolveImage
            ,indexedSlot_vkCmdSetEvent
            ,indexedSlot_vkCmdResetEvent
            ,indexedSlot_vkCmdWaitEvents
            ,indexedSlot_vkCmdPipelineBarrier
            ,indexedSlot_vkCmdResetQueryPool
            ,indexedSlot_vkCmdWriteTimestamp
            ,indexedSlot_vkCmdCopyQueryPoolResults
            ,vkCreateDescriptorSetLayoutOffset,vkDestroyDescriptorSetLayoutOffset,vkCmdBindDescriptorSetsOffset
            ,cameraCreateLayoutSlot,cameraDestroyLayoutSlot,cameraBindPipelineSlot,cameraDrawSlot,cameraDrawIndexedSlot
            ,vkDestroyShaderModuleOffset,vkDestroyPipelineOffset,vkCmdExecuteCommandsOffset
#endif
        }) {
        std::uintptr_t pointer{};if(!readMemory(nullptr,engineBase+offset,&pointer,sizeof(pointer)) || pointer) {state("ALREADY_INITIALIZED_OR_UNREADABLE");return;}
    }
    #ifdef RDRVR_HEAD_CAMERA
    if(!installEffectMutationHooks())effectMutationCoverageDisarm();
    if(!installResourceMutationHooks())disarmResourceMutationHooks();
    armEarlyResourceCoverage();
#endif
    if(ordinaryPacketProducerEnabled&&firstColdAttempt&&!installOrdinaryObserverCold())state("ORDINARY_OBSERVER_DECLINED");
    originalInit=reinterpret_cast<Init>(engineBase+initOffset);
    bool installed=replacePointer(engineBase+gotOffset,reinterpret_cast<std::uintptr_t>(originalInit),reinterpret_cast<std::uintptr_t>(tracedInit));
#ifdef RDRVR_GAME_SESSION
    if(installed) {
        originalSwappy=reinterpret_cast<PFN_vkQueuePresentKHR>(engineBase+swappyOffset);
        const bool valid=fingerprint(engineBase+swappyOffset,swappyFingerprint);
        const bool hooked=valid && replacePointer(engineBase+swappyGotOffset,reinterpret_cast<std::uintptr_t>(originalSwappy),reinterpret_cast<std::uintptr_t>(swappyPresent));
        swappyInterposed=hooked;
        if(!hooked){if(!replacePointer(engineBase+gotOffset,reinterpret_cast<std::uintptr_t>(tracedInit),reinterpret_cast<std::uintptr_t>(originalInit)))patchFault=true;installed=false;}
    }
#endif
#ifdef RDRVR_HEAD_CAMERA
    if(installed){
        const auto checked67=[&](std::uintptr_t offset,const auto& bytes){
            Mapping code{};return mapping(engineBase+offset,bytes.size(),code)&&(code.protection&PROT_EXEC)&&fingerprint(engineBase+offset,bytes);
        };
        struct Route67 {std::uintptr_t slot,original,replacement;};
        const Route67 routes67[]{
            {0x2e5f978,0x2abcd14,reinterpret_cast<std::uintptr_t>(instanceReset67)},
            {0x2e5f1c8,0x2a29810,reinterpret_cast<std::uintptr_t>(instanceClear67)},
            {0x2e5f1d0,0x2a298f0,reinterpret_cast<std::uintptr_t>(instanceStart67)}};
        bool valid67=experimental::instanceProof67::proof(checked67);
        for(const auto& route:routes67){std::uintptr_t pointer{};
            valid67=valid67&&readMemory(nullptr,engineBase+route.slot,&pointer,sizeof(pointer))&&pointer==engineBase+route.original;}
        if(valid67){
            experimental::instanceFrequency67.configure(
                reinterpret_cast<experimental::InstanceFrequency67::Reset>(engineBase+0x2abcd14),
                reinterpret_cast<experimental::InstanceFrequency67::Clear>(engineBase+0x2a29810),
                reinterpret_cast<experimental::InstanceFrequency67::Start>(engineBase+0x2a298f0),readMemory);
            unsigned patched67{};
            for(;patched67<std::size(routes67);++patched67){const auto& route=routes67[patched67];
                if(!replacePointer(engineBase+route.slot,engineBase+route.original,route.replacement))break;}
            if(patched67==std::size(routes67))experimental::instanceFrequencyReady67.store(true,std::memory_order_release);
            else while(patched67){const auto& route=routes67[--patched67];
                if(!replacePointer(engineBase+route.slot,route.replacement,engineBase+route.original))patchFault=true;}
        }
    }
#endif
#ifdef RDRVR_TOUCH_CONTROLS
    if(installed){
        const std::uintptr_t offsets[]{inputUpdateOffset,inputAndroidUpdateOffset,inputGetSystemOffset,inputGetPadOffset,inputGetInfoOffset,inputConnectOffset,inputActivateOffset,inputTouchModeOffset,inputButtonOffset,inputAxisOffset,inputSetOverrideOffset};
        const std::array<unsigned char,32> prints[]{inputUpdateFingerprint,inputAndroidUpdateFingerprint,inputGetSystemFingerprint,inputGetPadFingerprint,inputGetInfoFingerprint,inputConnectFingerprint,inputActivateFingerprint,inputTouchModeFingerprint,inputButtonFingerprint,inputAxisFingerprint,inputSetOverrideFingerprint};
        bool valid=true;
        for(unsigned i=0;i<std::size(offsets);++i){Mapping code{};valid=valid&&mapping(engineBase+offsets[i],32,code)&&(code.protection&PROT_EXEC)&&fingerprint(engineBase+offsets[i],prints[i]);}
        if(valid){
            NativePadApi api{};api.read=readMemory;
            api.setOverride=reinterpret_cast<decltype(api.setOverride)>(engineBase+inputSetOverrideOffset);
            api.getSystem=reinterpret_cast<decltype(api.getSystem)>(engineBase+inputGetSystemOffset);
            api.getPad=reinterpret_cast<decltype(api.getPad)>(engineBase+inputGetPadOffset);
            api.getInfo=reinterpret_cast<decltype(api.getInfo)>(engineBase+inputGetInfoOffset);
            api.connect=reinterpret_cast<decltype(api.connect)>(engineBase+inputConnectOffset);
            api.activate=reinterpret_cast<decltype(api.activate)>(engineBase+inputActivateOffset);
            api.touchMode=reinterpret_cast<decltype(api.touchMode)>(engineBase+inputTouchModeOffset);
            api.events.button=reinterpret_cast<decltype(api.events.button)>(engineBase+inputButtonOffset);
            api.events.axis=reinterpret_cast<decltype(api.events.axis)>(engineBase+inputAxisOffset);
            api.systemVtable=engineBase+inputSystemVtableOffset;api.padVtable=engineBase+inputPadVtableOffset;
            gameConfigureInput(api);
            const auto checkedWeapon=[&](std::uintptr_t offset,const auto& print){
                Mapping code{};return mapping(engineBase+offset,print.size(),code)&&(code.protection&PROT_EXEC)&&fingerprint(engineBase+offset,print);
            };
#ifdef RDRVR_HEAD_CAMERA
            // ONE install attempt of the sagShellDerived::UpdateInputs GOT wrapper (teleport + VR main-thread executor), gated
            // only by its own code, call-site and (inside replacePointer) GOT value. The teleport block calls it after configuring
            // teleport; otherwise it runs after the weapon block, so holsters/cheats never depend on teleport prints.
            bool updateInputsTried{},updateInputsInstalled{};
            const auto installUpdateInputs=[&]()->bool{
                if(updateInputsTried)return updateInputsInstalled;
                updateInputsTried=true;
                if(!checkedWeapon(teleport::updateInputsOffset,teleport::updateInputsFingerprint)||
                   !checkedWeapon(teleport::updateInputsReturnOffset-4,teleport::updateInputsSiteFingerprint))return false;
                nativeUpdateInputsOriginal=reinterpret_cast<NativeUpdateInputs>(engineBase+teleport::updateInputsOffset);
                updateInputsInstalled=replacePointer(engineBase+teleport::updateInputsGotOffset,engineBase+teleport::updateInputsOffset,reinterpret_cast<std::uintptr_t>(nativeUpdateInputs));
                if(updateInputsInstalled)vr::mainthread::markInstalled(); // The VR main-thread executor is live
                return updateInputsInstalled;
            };
#endif
            if(checkedWeapon(rumbleSetActuatorOffset,rumbleSetActuatorFingerprint)&&
               checkedWeapon(rumbleManagerUpdateOffset,rumbleManagerUpdateFingerprint)&&
               checkedWeapon(rumblePlayerEnabledOffset,rumblePlayerEnabledFingerprint)&&
               checkedWeapon(rumbleGamepadOffset,rumbleGamepadFingerprint)&&
               checkedWeapon(rumbleReturnOffset-4,rumbleCallFingerprint)){
                gameConfigureRumble({readMemory,nullptr});
                rumbleBoundary.configure(reinterpret_cast<NativeRumbleBoundary::Original>(engineBase+rumbleSetActuatorOffset),observeRumble);
                rumbleInterposed=replacePointer(engineBase+rumbleGotOffset,engineBase+rumbleSetActuatorOffset,reinterpret_cast<std::uintptr_t>(setActuator));
            }
            if(checkedWeapon(weaponPlayerLookupOffset,weaponPlayerLookupFingerprint)&&
               checkedWeapon(weaponActorLookupOffset,weaponActorLookupFingerprint)&&
               checkedWeapon(weaponInventoryCheckOffset,weaponInventoryCheckFingerprint)&&
               checkedWeapon(weaponActiveLookupOffset,weaponActiveLookupFingerprint)&&
               checkedWeapon(weaponItemLookupOffset,weaponItemLookupFingerprint)&&
               checkedWeapon(weaponLaunchOffset,weaponLaunchFingerprint)&&
               checkedWeapon(weaponEnumLookupOffset,weaponEnumLookupFingerprint)&&
               checkedWeapon(weaponFireProjectileOffset,weaponFireProjectileFingerprint)&&
               checkedWeapon(weaponAnimationFireOffset,weaponAnimationFireFingerprint)&&
               checkedWeapon(weaponUpdatePoseOffset,weaponUpdatePoseFingerprint)&&
               checkedWeapon(weaponManagerAimOffset,weaponManagerAimFingerprint)&&
               checkedWeapon(weaponLaunchOverrideOffset,weaponLaunchOverrideFingerprint)&&
               checkedWeapon(weaponDriveLaunchOverrideOffset,weaponDriveLaunchOverrideFingerprint)&&
               checkedWeapon(weaponIntendedResetOffset,weaponIntendedResetFingerprint)&&
               checkedWeapon(weaponIntendedSetOffset,weaponIntendedSetFingerprint)&&
               checkedWeapon(weaponSetupTargetPacketOffset,weaponSetupTargetPacketFingerprint)&&
               checkedWeapon(weaponPacketZeroOffset,weaponPacketZeroFingerprint)&&
               checkedWeapon(weaponPacketTargetPositionOffset,weaponPacketTargetPositionFingerprint)&&
               checkedWeapon(weaponPerformPacketOffset,weaponPerformPacketFingerprint)&&
               checkedWeapon(weaponContextAliveOffset,weaponContextAliveFingerprint)&&
               checkedWeapon(weaponContextControlOffset,weaponContextControlFingerprint)&&
               checkedWeapon(weaponContextStateOffset,weaponContextStateFingerprint)&&
               checkedWeapon(weaponContextRideOffset,weaponContextRideFingerprint)&&
               checkedWeapon(weaponContextVehicleOffset,weaponContextVehicleFingerprint)&&
               checkedWeapon(weaponContextDeadEyeOffset,weaponContextDeadEyeFingerprint)&&
               checkedWeapon(weaponContextEnterOffset,weaponContextEnterFingerprint)&&
               checkedWeapon(weaponContextLeaveOffset,weaponContextLeaveFingerprint)&&
               checkedWeapon(weaponContextVehSourceOffset,weaponContextVehSourceFingerprint)&&
               checkedWeapon(weaponContextDuelOffset,weaponContextDuelFingerprint)&&
               checkedWeapon(weaponContextMountedOffset,weaponContextMountedFingerprint)&&
               checkedWeapon(weaponContextMountingOffset,weaponContextMountingFingerprint)&&
               checkedWeapon(weaponContextBlindOffset,weaponContextBlindFingerprint)&&
               checkedWeapon(weaponContextCoverOffset,weaponContextCoverFingerprint)&&
               checkedWeapon(weaponContextLinkedOffset,weaponContextLinkedFingerprint)&&
               checkedWeapon(weaponContextGringoOffset,weaponContextGringoFingerprint)){
                WeaponOwnerApi owner{};owner.read=readMemory;
                if(checkedWeapon(weaponFrameTimerInitOffset,weaponFrameTimerInitFingerprint)&&
                   checkedWeapon(weaponFrameTimerUpdateOffset,weaponFrameTimerUpdateFingerprint)&&
                   checkedWeapon(weaponFrameTimerResetOffset,weaponFrameTimerResetFingerprint)){
                    owner.frameTimer=engineBase+weaponFrameTimerOffset;owner.frameTimerTable=engineBase+weaponFrameTimerTableOffset;
                }
                owner.playerManager=engineBase+weaponPlayerManagerOffset;
                owner.gohManagerGlobal=engineBase+weaponGohManagerOffset;owner.gohSlotsGlobal=engineBase+weaponGohSlotsOffset;
                owner.actorManagerGlobal=engineBase+weaponActorManagerOffset;owner.actorSlotsGlobal=engineBase+weaponActorSlotsOffset;
                owner.playerManagerTable=engineBase+weaponPlayerManagerTableOffset;owner.playerTable=engineBase+weaponPlayerTableOffset;
                owner.inventoryTable=engineBase+weaponInventoryTableOffset;owner.itemTable=engineBase+weaponItemTableOffset;
                owner.weaponTable=engineBase+weaponObjectTableOffset;
                owner.typeApi.read=readMemory;owner.typeApi.tree=engineBase+weaponEnumTreeOffset;
                owner.typeApi.baseTypeTable=engineBase+weaponBaseTypeTableOffset;
                owner.typeApi.enumTables={engineBase+weaponEnumPistolTableOffset,engineBase+weaponEnumDualPistolTableOffset,engineBase+weaponEnumRifleTableOffset,engineBase+weaponEnumRepeaterTableOffset,engineBase+weaponEnumSniperRifleTableOffset,engineBase+weaponEnumShotgunTableOffset,engineBase+weaponEnumThrownTableOffset,engineBase+weaponEnumThrownExplodingTableOffset,engineBase+weaponEnumBowTableOffset,engineBase+weaponEnumMeleeTableOffset,engineBase+weaponEnumLassoTableOffset,engineBase+weaponEnumTurretTableOffset,engineBase+weaponEnumCannonTableOffset};
                gameConfigureWeaponObservation(owner);
#ifdef RDRVR_HEAD_CAMERA
                if(vr::holsters::startupEnabled()){ // HOLSTERS: debug.rdrvr.vr.holsters 0 = no engine use at all
                    namespace vh=vr::holsters;
                    if(checkedWeapon(vh::selectItemOffset,vh::selectItemFingerprint)&&checkedWeapon(vh::canDrawOffset,vh::canDrawFingerprint)&&
                       checkedWeapon(vh::putAwayWeaponOffset,vh::putAwayWeaponFingerprint)&&checkedWeapon(vh::putAwayItemOffset,vh::putAwayItemFingerprint)&&
                       checkedWeapon(vh::startItemChangeOffset,vh::startItemChangeFingerprint)){
                        holsterOwnerApi=owner;
                        vh::EngineApi h{};h.read=readMemory;h.gameData=engineBase+teleport::gameDataOffset;
                        h.playerManager=engineBase+weaponPlayerManagerOffset;h.playerManagerTable=engineBase+weaponPlayerManagerTableOffset;
                        h.playerTable=engineBase+weaponPlayerTableOffset;h.actorManagerGlobal=engineBase+weaponActorManagerOffset;
                        h.actorSlotsGlobal=engineBase+weaponActorSlotsOffset;h.gohManagerGlobal=engineBase+weaponGohManagerOffset;
                        h.gohSlotsGlobal=engineBase+weaponGohSlotsOffset;h.inventoryTable=engineBase+vh::inventoryTableOffset;
                        h.itemTables={engineBase+vh::weaponItemTableOffset,engineBase+vh::lassoItemTableOffset};
                        h.typeTables={engineBase+vh::weaponTypeTableOffset,engineBase+vh::lassoTypeTableOffset};
                        h.selectItem=reinterpret_cast<vh::SelectItemFn>(engineBase+vh::selectItemOffset);
                        h.putAwayWeapon=reinterpret_cast<vh::PutAwayWeaponFn>(engineBase+vh::putAwayWeaponOffset);
                        h.canDraw=reinterpret_cast<vh::CanDrawFn>(engineBase+vh::canDrawOffset);
                        h.context=holsterContext;h.animatedDraw=vh::startupAnimatedDraw();
                        // WEAPON KEEP: hold-mode timer refresh + force aim while a weapon is in the VR hand (whole-function prints).
                        vr::weapon::startup(); // idempotent (also called by gameConfigureInput): properties before the first tick
                        if(checkedWeapon(vh::setHoldModeOffset,vh::setHoldModeFingerprint))
                            h.setHoldMode=reinterpret_cast<vh::SetHoldModeFn>(engineBase+vh::setHoldModeOffset);
                        if(checkedWeapon(vh::setForceAimOffset,vh::setForceAimFingerprint))
                            h.setForceAim=reinterpret_cast<vh::SetForceAimFn>(engineBase+vh::setForceAimOffset);
                        // DUAL: the holstered-item Update path (proof only, never called) gates the phantom;
                        // SetShouldRender / RefreshWeaponRender (whole functions) are optional (null = the phantom is never force-rendered).
                        if(!checkedWeapon(vh::onUpdatePositionCallOffset,vh::onUpdatePositionCallFingerprint)||
                           !checkedWeapon(vh::updateHolsteredOffset,vh::updateHolsteredFingerprint))vh::dualUnavailable("ENGINE MISMATCH");
                        if(checkedWeapon(vh::setShouldRenderOffset,vh::setShouldRenderFingerprint)&&checkedWeapon(vh::refreshWeaponRenderOffset,vh::refreshWeaponRenderFingerprint)){
                            h.setShouldRender=reinterpret_cast<vh::SetShouldRenderFn>(engineBase+vh::setShouldRenderOffset);
                            h.refreshRender=reinterpret_cast<vh::RefreshRenderFn>(engineBase+vh::refreshWeaponRenderOffset);}
                        // WEAPON: weapWeapon::SetProjectileLaunchOverride (whole function) - VR-held weapon launches from its muzzle.
                        if(checkedWeapon(vr::weapon::setLaunchOverrideOffset,vr::weapon::setLaunchOverrideFingerprint))
                            vr::weapon::setLaunchOverride.store(reinterpret_cast<vr::weapon::SetLaunchOverrideFn>(engineBase+vr::weapon::setLaunchOverrideOffset),std::memory_order_release);
                        vh::configureEngine(h);
                    }else vh::engineUnavailable("ENGINE MISMATCH");
                }
#endif
#ifdef RDRVR_HEAD_CAMERA
                { // Exact bytes of every engine function used + relocated GOT values, then one GOT hook.
                    const auto got=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t value{};
                        return readMemory(nullptr,engineBase+slot,&value,sizeof value)&&value==engineBase+target;};
                    if(checkedWeapon(teleport::withHeadingOffset,teleport::withHeadingFingerprint)&&
                       checkedWeapon(teleport::toMatrixOffset,teleport::toMatrixFingerprint)&&
                       checkedWeapon(teleport::updateInputsOffset,teleport::updateInputsFingerprint)&&
                       checkedWeapon(teleport::updateInputsReturnOffset-4,teleport::updateInputsSiteFingerprint)&&
                       checkedWeapon(teleport::updateGameStateOffset,teleport::updateGameStateFingerprint)&&
                       checkedWeapon(teleport::worldLoadedOffset,teleport::worldLoadedFingerprint)&&
                       checkedWeapon(teleport::streamerUpdateOffset,teleport::streamerUpdateFingerprint)&&got(teleport::streamerGotOffset,teleport::streamerOffset)&&
                       got(teleport::toMatrixGotOffset,teleport::toMatrixOffset)&&got(teleport::gameDataGotOffset,teleport::gameDataOffset)&&
                       got(teleport::playerTeleportingGotOffset,teleport::playerTeleportingOffset)&&
                       got(teleport::playerManagerGotOffset,weaponPlayerManagerOffset)&&
                       got(teleport::actorManagerGotOffset,weaponActorManagerOffset)&&got(teleport::actorSlotsGotOffset,weaponActorSlotsOffset)){
                        teleport::EngineApi tp{};tp.read=readMemory;tp.playerManager=owner.playerManager;tp.playerManagerTable=owner.playerManagerTable;
                        tp.playerTable=owner.playerTable;tp.actorManagerGlobal=owner.actorManagerGlobal;tp.actorSlotsGlobal=owner.actorSlotsGlobal;
                        tp.gameData=engineBase+teleport::gameDataOffset;tp.playerTeleporting=engineBase+teleport::playerTeleportingOffset;
                        tp.teleport=reinterpret_cast<teleport::TeleportFn>(engineBase+teleport::withHeadingOffset);
                        tp.streamer=engineBase+teleport::streamerOffset;tp.worldLoaded=reinterpret_cast<teleport::WorldLoadedFn>(engineBase+teleport::worldLoadedOffset);
                        teleport::configureEngine(tp);
                        if(installUpdateInputs())teleport::markHookInstalled(); // Shared wrapper install (see installUpdateInputs)
                        else teleport::engineUnavailable("HOOK NOT INSTALLED");
                    }else teleport::engineUnavailable("ENGINE MISMATCH");
                    // GAME CLOCK: exact bytes of SetTimeOfDay/UpdateTimeOfDay/Get24HourTime/Update + the tick call site,
                    // relocated GOT values (sm_Instance, vtable, the four JUMP_SLOTs untouched); no hook of its own: it runs in the
                    // UpdateInputs wrapper above, so it needs that hook.
                    if(!teleport::hookInstalled.load(std::memory_order_acquire))gameclock::engineUnavailable("NO MAIN-THREAD HOOK");
                    else if(checkedWeapon(gameclock::setTimeOfDayOffset,gameclock::setTimeOfDayFingerprint)&&
                       checkedWeapon(gameclock::updateTimeOfDayOffset,gameclock::updateTimeOfDayFingerprint)&&
                       checkedWeapon(gameclock::get24HourOffset,gameclock::get24HourFingerprint)&&
                       checkedWeapon(gameclock::updateOffset,gameclock::updateFingerprint)&&
                       checkedWeapon(gameclock::tickSiteOffset,gameclock::tickSiteFingerprint)&&
                       got(gameclock::instanceGotOffset,gameclock::instanceOffset)&&got(gameclock::vtableGotOffset,gameclock::vtableOffset)&&
                       got(gameclock::setTimeOfDayGotOffset,gameclock::setTimeOfDayOffset)&&got(gameclock::updateTimeOfDayGotOffset,gameclock::updateTimeOfDayOffset)&&
                       got(gameclock::get24HourGotOffset,gameclock::get24HourOffset)&&got(gameclock::updateGotOffset,gameclock::updateOffset)&&
                       got(teleport::gameDataGotOffset,teleport::gameDataOffset)){
                        gameclock::EngineApi ck{};ck.read=readMemory;ck.instance=engineBase+gameclock::instanceOffset;
                        ck.vptr=engineBase+gameclock::vtableOffset+0x10;ck.gameData=engineBase+teleport::gameDataOffset;
                        ck.setTime=reinterpret_cast<gameclock::SetTimeFn>(engineBase+gameclock::setTimeOfDayOffset);
                        gameclock::configureEngine(ck);gameclock::markHookInstalled();
                    }else gameclock::engineUnavailable("ENGINE MISMATCH");
                }
#endif
#ifdef RDRVR_HEAD_CAMERA
                if(checkedWeapon(reticleUiPaintOffset,reticleUiPaintFingerprint)&&
                   checkedWeapon(reticleUiGetNameOffset,reticleUiGetNameFingerprint)&&
                   checkedWeapon(reticleUiGetContextOffset,reticleUiGetContextFingerprint)&&
                   checkedWeapon(reticleUiSetFloatOffset,reticleUiSetFloatFingerprint)&&
                   checkedWeapon(reticleUiSetVariantOffset,reticleUiSetVariantFingerprint)){
                    reticleUiBoundary.configure(reinterpret_cast<ReticleUiCallBoundary::Original>(engineBase+reticleUiPaintOffset),observeReticleUi);
                    reticleUiInterposed=replacePointer(engineBase+reticleUiPaintSlotOffset,engineBase+reticleUiPaintOffset,reinterpret_cast<std::uintptr_t>(paintReticleUi));
                    if(reticleUiInterposed)gameConfigureReticleUi({readMemory,nullptr,engineBase+reticleUiTableOffset,engineBase+reticleUiSetFloatOffset},true);
                }
                gameNativeReticleReady(checkedWeapon(weaponReticleStatesOffset,weaponReticleStatesFingerprint)&&
                    checkedWeapon(weaponReticleStatesCtorOffset,weaponReticleStatesCtorFingerprint)&&
                    checkedWeapon(weaponReticleAllowedOffset,weaponReticleAllowedFingerprint)&&
                    checkedWeapon(weaponReticlePreventOffset,weaponReticlePreventFingerprint)&&
                    checkedWeapon(weaponReticleEvaluateOffset,weaponReticleEvaluateFingerprint));
#endif
#ifdef RDRVR_HEAD_CAMERA
                if(checkedWeapon(movementPlayerInputOffset,movementPlayerInputFingerprint)&&
                   checkedWeapon(movementFootInputOffset,movementFootInputFingerprint)&&
                   checkedWeapon(movementFootParametersOffset,movementFootParametersFingerprint)&&
                   checkedWeapon(movementFootApplyOffset,movementFootApplyFingerprint)&&
                   checkedWeapon(movementControlVectorOffset,movementControlVectorFingerprint)&&
                   checkedWeapon(movementControlOverrideOffset,movementControlOverrideFingerprint)&&
                   checkedWeapon(movementScriptedGaitOffset,movementScriptedGaitFingerprint)&&
                   checkedWeapon(movementLockedOffset,movementLockedFingerprint))
                    gameConfigureLocomotion({owner,engineBase+movementFootTableOffset});
                if(checkedWeapon(bodyDrawCommonOffset,bodyDrawCommonFingerprint)&&checkedWeapon(bodyDrawOpaqueOffset,bodyDrawOpaqueFingerprint)&&
                   checkedWeapon(bodyDrawAlphaOffset,bodyDrawAlphaFingerprint)&&checkedWeapon(bodyDrawContainerOffset,bodyDrawContainerFingerprint)&&
                   checkedWeapon(bodyInfoSetOffset,bodyInfoSetFingerprint)&&checkedWeapon(bodyDynamicDrawOffset,bodyDynamicDrawFingerprint)&&
                   checkedWeapon(bodyEntityCreatureOffset,bodyEntityCreatureFingerprint)&&checkedWeapon(bodyInstanceCreatureOffset,bodyInstanceCreatureFingerprint)&&
                   checkedWeapon(bodyOpaqueReturnOffset-4,bodyOpaqueCallFingerprint)&&checkedWeapon(bodyAlphaReturnOffset-4,bodyAlphaCallFingerprint)&&
                   checkedWeapon(bodyMain0ReturnOffset-4,bodyMain0CallFingerprint)&&checkedWeapon(bodyMain1ReturnOffset-4,bodyMain1CallFingerprint)){
                    if(checkedWeapon(bodyGetHatOffset,bodyGetHatFingerprint)&&checkedWeapon(bodyHatFragmentOffset,bodyHatFragmentFingerprint)&&
                       checkedWeapon(bodySetAttachedOffset,bodySetAttachedFingerprint))
                        gameConfigureBodyAttachments({reinterpret_cast<BodyAttachmentApi::Lookup>(engineBase+bodyGetHatOffset),
                            reinterpret_cast<BodyAttachmentApi::Lookup>(engineBase+bodyHatFragmentOffset),engineBase+bodyNavTableOffset,engineBase+bodyGohTableOffset});
                    // All diagnostic wrappers forward once, preserving argument and return ABI.
                    if(checkedWeapon(passHashOffset,passHashFingerprint)&&checkedWeapon(passHashOwnerOffset,passHashOwnerFingerprint)&&
                       checkedWeapon(passHashSiteReturnOffset-4,passHashSiteCallFingerprint)){
                        nativePassHash=reinterpret_cast<PipelinePassHashMemo::Original>(engineBase+passHashOffset);
                        pipelinePassHashMemo.installed(replacePointer(engineBase+passHashSiteGotOffset,engineBase+passHashOffset,reinterpret_cast<std::uintptr_t>(memoPassHash)));
                    }
                    // Descriptor reuse hits only about 2% of requests in gameplay, while the
                    // cache's key/registry work is paid on every request: the native path stays.
                    nativeDescriptorReuse.installed(false);
                    if(nativeFramePacingProof(checkedWeapon)&&
                       checkedWeapon(framePacingThreadSiteReturnOffset-4,framePacingThreadSiteCallFingerprint)&&
                       checkedWeapon(framePacingSwapSiteReturnOffset-4,framePacingSwapSiteCallFingerprint)&&
                       checkedWeapon(framePacingTimeSiteReturnOffset-4,framePacingTimeSiteCallFingerprint)&&
                       nativeFramePacing.configure({reinterpret_cast<NativeFramePacing::Wait>(engineBase+framePacingOffset),
                           reinterpret_cast<NativeFramePacing::Seconds>(engineBase+framePacingClockOffset),
                           reinterpret_cast<const std::uintptr_t*>(engineBase+framePacingDeviceOffset),
                           reinterpret_cast<const double*>(engineBase+framePacingOriginOffset),
                           reinterpret_cast<const void*>(engineBase+framePacingTimeManagerOffset),::usleep,
                           reinterpret_cast<NativeFramePacing::UpdateTime>(engineBase+framePacingUpdateTimeOffset)})){
                        const std::uintptr_t slots[]{framePacingGotOffset,framePacingUpdateTimeGotOffset};
                        const std::uintptr_t functions[]{framePacingOffset,framePacingUpdateTimeOffset};
                        const std::uintptr_t wrappers[]{reinterpret_cast<std::uintptr_t>(nativeDisplayWait),reinterpret_cast<std::uintptr_t>(nativeUpdateTime)};
                        unsigned patched{};
                        while(patched<2&&replacePointer(engineBase+slots[patched],engineBase+functions[patched],wrappers[patched]))++patched;
                        if(patched==2){nativeFramePacing.timeInstalled(true);nativeFramePacing.installed(true);}
                        else while(patched){--patched;if(!replacePointer(engineBase+slots[patched],wrappers[patched],engineBase+functions[patched]))patchFault=true;}
                    }
                    if(dynamicUniformProof(checkedWeapon)&&checkedWeapon(descriptorReuseOffset,descriptorReuseFingerprint)){
                        nativeDynamicLayout=reinterpret_cast<DynamicLayoutFn>(engineBase+dynamicLayoutOffset);
                        nativeDynamicInput=reinterpret_cast<DynamicInputFn>(engineBase+dynamicBindInputOffset);
                        nativeDynamicSet=reinterpret_cast<DynamicUniformDescriptors::NativeOriginal>(engineBase+descriptorReuseOffset);
                        const std::uintptr_t slots[]{dynamicLayoutGotOffset,dynamicBindInputGotOffset,descriptorReuseSiteGotOffset};
                        const std::uintptr_t functions[]{dynamicLayoutOffset,dynamicBindInputOffset,descriptorReuseOffset};
                        const std::uintptr_t wrappers[]{reinterpret_cast<std::uintptr_t>(dynamicLayout),reinterpret_cast<std::uintptr_t>(dynamicInput),reinterpret_cast<std::uintptr_t>(dynamicSet)};
                        unsigned patched{};
                        while(patched<3&&replacePointer(engineBase+slots[patched],engineBase+functions[patched],wrappers[patched]))++patched;
                        if(patched==3){dynamicNativeHooksReady.store(true,std::memory_order_release);
                            if constexpr(!ordinaryMainStartupRequested()){
                                if(checkedWeapon(dynamicBindInputOffset,experimental::nativeVertexInputFingerprint58)){
                                    nativeVertexStreamMerge58.enable();nativeVertexInputReady58.store(true,std::memory_order_release);
                                }
                            }
                        }
                        else while(patched){--patched;if(!replacePointer(engineBase+slots[patched],wrappers[patched],engineBase+functions[patched]))patchFault=true;}
                    }
                    // Boot already certified all native Vulkan dispatch slots zero.
                    // Hook the VMA entries before allocator initialization; global
                    // vkCreate/Destroy slots are copied by VMA and are insufficient.
                    if(dynamicNativeHooksReady.load(std::memory_order_acquire)&&rdr92::vmaCodeProof(checkedWeapon)&&
                       uniformLife92.configure(reinterpret_cast<rdr92::VmaUniformLife::Create>(engineBase+0x18e2780),
                           reinterpret_cast<rdr92::VmaUniformLife::Aligned>(engineBase+0x18e2ab0),
                           reinterpret_cast<rdr92::VmaUniformLife::Destroy>(engineBase+0x18e2f64),
                           reinterpret_cast<rdr92::VmaUniformLife::Info>(engineBase+0x18dfc10),engineBase+0x18b1a14)){
                        const std::array<rdr92::VmaHookRoute,3> routes{{
                            {engineBase+0x2e17000,engineBase+0x18e2780,reinterpret_cast<std::uintptr_t>(vmaCreate92)},
                            {engineBase+0x2e16ce8,engineBase+0x18e2ab0,reinterpret_cast<std::uintptr_t>(vmaAligned92)},
                            {engineBase+0x2e16d00,engineBase+0x18e2f64,reinterpret_cast<std::uintptr_t>(vmaDestroy92)}}};
                        const auto read=[&](std::uintptr_t slot,std::uintptr_t expected){Mapping m{};
                            return mapping(slot,sizeof expected,m)&&(m.protection&PROT_READ)&&
                                __atomic_load_n(reinterpret_cast<std::uintptr_t*>(slot),__ATOMIC_ACQUIRE)==expected;};
                        bool rollbackFault{};
                        uniformLife92.installed(rdr92::installVmaHooks(routes,read,replacePointer,rollbackFault));
                        if(rollbackFault)patchFault=true;
                    }
                    const auto checkedSource=[&](std::uintptr_t offset,const auto& bytes){
                        if(offset==staticTrianglesFvfSizesOffset||offset==staticTrianglesFvfOrderOffset){Mapping data{};
                            return mapping(engineBase+offset,bytes.size(),data)&&(data.protection&PROT_READ)&&fingerprint(engineBase+offset,bytes);}
                        return checkedWeapon(offset,bytes);
                    };
                    // The native selector is mutable RW data, not a code fingerprint.
                    // Validate its exact relocated GOT ownership; never pin its value.
                    Mapping shadowSelectorMapping{},shadowSelectorGotMapping{};
                    std::uintptr_t shadowSelectorPointer{};
                    const auto shadowSelectorAddress=engineBase+nativeOcclusionWitnessShadowBufferOffset;
                    const bool shadowSelectorReady=
                        mapping(shadowSelectorAddress,nativeOcclusionWitnessShadowBufferSize,shadowSelectorMapping)&&
                        (shadowSelectorMapping.protection&(PROT_READ|PROT_WRITE))==(PROT_READ|PROT_WRITE)&&
                        mapping(engineBase+nativeOcclusionWitnessShadowBufferGotOffset,sizeof(shadowSelectorPointer),shadowSelectorGotMapping)&&
                        (shadowSelectorGotMapping.protection&PROT_READ)&&
                        readMemory(nullptr,engineBase+nativeOcclusionWitnessShadowBufferGotOffset,&shadowSelectorPointer,sizeof(shadowSelectorPointer))&&
                        shadowSelectorPointer==shadowSelectorAddress;
                    if(dynamicNativeHooksReady.load()&&shadowSelectorReady&&staticTrianglesSourceProof(checkedSource)&&nativeOcclusionWitnessProof(checkedWeapon)&&checkedWeapon(rdr106::vertexReadOnly,rdr106::vertexReadOnlyBytes)&&depthCoverageProof(checkedWeapon)){
                        nativeOcclusionGeometry=reinterpret_cast<decltype(nativeOcclusionGeometry)>(engineBase+nativeOcclusionWitnessGeometryOffset);
                        nativeOcclusionTiles=reinterpret_cast<decltype(nativeOcclusionTiles)>(engineBase+depthCoverageTilesOffset);
                        nativeOcclusionOptimise=reinterpret_cast<decltype(nativeOcclusionOptimise)>(engineBase+nativeOcclusionWitnessOptimiseOffset);
                        nativeOcclusionVertexReadOnly=reinterpret_cast<decltype(nativeOcclusionVertexReadOnly)>(engineBase+rdr106::vertexReadOnly);
                        nativeOcclusionIndexReadOnly=reinterpret_cast<decltype(nativeOcclusionIndexReadOnly)>(engineBase+nativeOcclusionWitnessIndexLockROOffset);
                        nativeOcclusionVertexLock=reinterpret_cast<decltype(nativeOcclusionVertexLock)>(engineBase+nativeOcclusionWitnessVertexLockOffset);
                        nativeOcclusionVertexUnlock=reinterpret_cast<decltype(nativeOcclusionVertexUnlock)>(engineBase+nativeOcclusionWitnessVertexUnlockOffset);
                        nativeOcclusionIndexLock=reinterpret_cast<decltype(nativeOcclusionIndexLock)>(engineBase+nativeOcclusionWitnessIndexLockOffset);
                        nativeOcclusionIndexUnlock=reinterpret_cast<decltype(nativeOcclusionIndexUnlock)>(engineBase+nativeOcclusionWitnessIndexUnlockOffset);
                        nativeOcclusionBufferDestroy=reinterpret_cast<decltype(nativeOcclusionBufferDestroy)>(engineBase+nativeOcclusionWitnessBufferDestroyOffset);
                        const bool capture=runtime::graphicsSettings.snapshot().requested.occlusion; // No cold occluder capture with OCCLUSION OFF at startup
                        hygiene::occluderCaptureStartup.store(capture?1:0,std::memory_order_relaxed);
                        nativeOcclusionVertexCapture.configure({readMemory,nullptr,engineBase+nativeOcclusionWitnessGeometryTableAddressPointOffset,
                            engineBase+nativeOcclusionWitnessVertexTableAddressPointOffset,engineBase+nativeOcclusionWitnessBufferTableAddressPointOffset,
                            engineBase+nativeOcclusionWitnessOptimisePositionUnlockReturnOffset,1,true,
                            engineBase+nativeOcclusionWitnessIndexTableAddressPointOffset},capture?&ownedStaticVertices:nullptr,capture?&ownedStaticOccluders:nullptr);
                        // Invalidation is installed before optimizer capture. The complete
                        // set is required; partial installation is rolled back together.
                        const std::array<std::uintptr_t,12> slots{nativeOcclusionWitnessVertexLockSlotOffset,
                            nativeOcclusionWitnessIndexLockSlotOffset,nativeOcclusionWitnessIndexUnlockSlotOffset,
                            nativeOcclusionWitnessBufferDestroyGotOffset,nativeOcclusionWitnessBufferDestroySlotOffset,
                            nativeOcclusionWitnessVertexUnlockSlotOffset,nativeOcclusionWitnessOptimiseGotOffset,
                            nativeOcclusionWitnessGeometrySiteGotOffset,nativeOcclusionWitnessGeometrySlotOffset,depthCoverageTilesGotOffset,rdr106::vertexReadOnlySlot,rdr106::indexReadOnlySlot};
                        const std::array<std::uintptr_t,12> originals{nativeOcclusionWitnessVertexLockOffset,
                            nativeOcclusionWitnessIndexLockOffset,nativeOcclusionWitnessIndexUnlockOffset,
                            nativeOcclusionWitnessBufferDestroyOffset,nativeOcclusionWitnessBufferDestroyOffset,
                            nativeOcclusionWitnessVertexUnlockOffset,nativeOcclusionWitnessOptimiseOffset,
                            nativeOcclusionWitnessGeometryOffset,nativeOcclusionWitnessGeometryOffset,depthCoverageTilesOffset,rdr106::vertexReadOnly,nativeOcclusionWitnessIndexLockROOffset};
                        const std::array<std::uintptr_t,12> wrappers{reinterpret_cast<std::uintptr_t>(occlusionVertexLock),
                            reinterpret_cast<std::uintptr_t>(occlusionIndexLock),reinterpret_cast<std::uintptr_t>(occlusionIndexUnlock),
                            reinterpret_cast<std::uintptr_t>(occlusionBufferDestroy),reinterpret_cast<std::uintptr_t>(occlusionBufferDestroy),
                            reinterpret_cast<std::uintptr_t>(occlusionVertexUnlock),reinterpret_cast<std::uintptr_t>(occlusionOptimise),
                            reinterpret_cast<std::uintptr_t>(occlusionGeometry),reinterpret_cast<std::uintptr_t>(occlusionGeometry),reinterpret_cast<std::uintptr_t>(occlusionTiles),reinterpret_cast<std::uintptr_t>(occlusionVertexReadOnly),reinterpret_cast<std::uintptr_t>(occlusionIndexReadOnly)};
                        experimental::nativeOrdinaryGeometryEpoch.suspend();experimental::nativeEarlyColdOwners.disarm();
                        unsigned patched{};while(patched<slots.size()&&replacePointer(engineBase+slots[patched],engineBase+originals[patched],wrappers[patched]))++patched;
                        if(patched==slots.size()){
                            NativeStaticOcclusionSource::Api source{readMemory,nullptr,engineBase+nativeOcclusionWitnessStaticTableAddressPointOffset,
                                engineBase+contextVtableOffset,{readMemory,nullptr,engineBase+staticTrianglesVertexTableAddressPointOffset,
                                engineBase+staticTrianglesIndexTableAddressPointOffset,engineBase+staticTrianglesBufferTableAddressPointOffset,true,&ownedStaticVertices,1,&ownedStaticOccluders},true,
                                engineBase+nativeOcclusionWitnessDrawableDrawOffset,borrowedOcclusionRead};
                            NativeOcclusionDrawWitness::Api witness{engineBase+nativeOcclusionWitnessContextTableAddressPointOffset,
                                engineBase+nativeOcclusionWitnessPipelineTableAddressPointOffset,engineBase+nativeOcclusionWitnessFramebufferTableAddressPointOffset,
                                engineBase+nativeOcclusionWitnessBufferTableAddressPointOffset,engineBase+nativeOcclusionWitnessStaticTableAddressPointOffset,
                                engineBase+nativeOcclusionWitnessDrawableDrawOffset,engineBase+nativeOcclusionWitnessGeometryTableAddressPointOffset,true,shadowSelectorAddress,
                                engineBase+staticTrianglesVertexTableAddressPointOffset,engineBase+staticTrianglesIndexTableAddressPointOffset};
                            experimental::NativeEarlyImageProof::Api imageChecks{nullptr,
                                [](void*,std::uintptr_t address,void* bytes,std::size_t n)noexcept{return readMemory(nullptr,address,bytes,n);},
                                [](void*,std::uintptr_t address,std::size_t n,unsigned flags)noexcept{
                                    Mapping region{};if(!mapping(address,n,region))return false;
                                    const int required=((flags&4)?PROT_READ:0)|((flags&2)?PROT_WRITE:0)|((flags&1)?PROT_EXEC:0);
                                    return (region.protection&required)==required;
                                }};
                            experimental::nativeEarlyBootstrap.publish(engineBase,imageChecks,source,witness,nativeEarlyCurrentGfxSlot);
                            experimental::nativeEarlyPaletteBootstrap.publish(engineBase,imageChecks);
                            using ColdOwners=experimental::NativeEarlyColdOwners;
                            static_assert(ColdOwners::SlotOffsets==std::array<std::uintptr_t,12>{nativeOcclusionWitnessVertexLockSlotOffset,
                                nativeOcclusionWitnessIndexLockSlotOffset,nativeOcclusionWitnessIndexUnlockSlotOffset,
                                nativeOcclusionWitnessBufferDestroyGotOffset,nativeOcclusionWitnessBufferDestroySlotOffset,
                                nativeOcclusionWitnessVertexUnlockSlotOffset,nativeOcclusionWitnessOptimiseGotOffset,
                                nativeOcclusionWitnessGeometrySiteGotOffset,nativeOcclusionWitnessGeometrySlotOffset,depthCoverageTilesGotOffset,rdr106::vertexReadOnlySlot,rdr106::indexReadOnlySlot});
                            static_assert(ColdOwners::OriginalOffsets==std::array<std::uintptr_t,12>{nativeOcclusionWitnessVertexLockOffset,
                                nativeOcclusionWitnessIndexLockOffset,nativeOcclusionWitnessIndexUnlockOffset,
                                nativeOcclusionWitnessBufferDestroyOffset,nativeOcclusionWitnessBufferDestroyOffset,
                                nativeOcclusionWitnessVertexUnlockOffset,nativeOcclusionWitnessOptimiseOffset,
                                nativeOcclusionWitnessGeometryOffset,nativeOcclusionWitnessGeometryOffset,depthCoverageTilesOffset,rdr106::vertexReadOnly,nativeOcclusionWitnessIndexLockROOffset});
                            if(experimental::nativeEarlyColdOwners.bind(engineBase,{imageChecks.context,imageChecks.read,imageChecks.range})){
                                std::array<ColdOwners::Slot,12> installed{};
                                for(unsigned i=0;i<installed.size();++i)installed[i]={engineBase+slots[i],wrappers[i]};
                                if(experimental::nativeEarlyColdOwners.publish(installed))experimental::nativeOrdinaryGeometryEpoch.arm(experimental::nativeEarlyColdOwners);
                            }
                        }else while(patched){--patched;if(!replacePointer(engineBase+slots[patched],wrappers[patched],engineBase+originals[patched]))patchFault=true;}
                    }
                    if(checkedWeapon(unusedDrawLabelOwnerOffset,unusedDrawLabelOwnerFingerprint)&&
                       checkedWeapon(unusedDrawLabelFormatOffset,unusedDrawLabelFormatFingerprint)&&
                       checkedWeapon(unusedDrawLabelPixOffset,unusedDrawLabelPixFingerprint)&&
                       checkedWeapon(unusedDrawLabelSiteReturnOffset-4,unusedDrawLabelSiteCallFingerprint)&&
                       checkedWeapon(unusedDrawLabelConsumerReturnOffset-4,unusedDrawLabelConsumerCallFingerprint)&&
                       unusedDrawLabel.configure(engineBase+unusedDrawLabelFormatOffset,engineBase+unusedDrawLabelSiteReturnOffset))
                        replacePointer(engineBase+unusedDrawLabelSiteGotOffset,engineBase+unusedDrawLabelFormatOffset,unusedDrawLabel.entry());
                    gameConfigureBodyFilter(reinterpret_cast<BodyDrawFilter::Original>(engineBase+bodyDrawCommonOffset),readBodyDrawRecordField);
                    const bool bodyReady=replacePointer(engineBase+bodyOpaqueGotOffset,engineBase+bodyDrawCommonOffset,reinterpret_cast<std::uintptr_t>(bodyDraw));
                    gameBodyFilterHooksReady(bodyReady);
                    eyecull::configure(bodyReady); // EYE CULL A/B installed / fingerprint from the real DrawCommon hook result
#ifdef RDRVR_HEAD_CAMERA
                    if(checkedWeapon(0x1b13184,std::array<std::uint8_t,32>{
#include "gamecode/TraceEntry_1843e28_32_8h.inc"
})&&
                       checkedWeapon(0x1d9d604,std::array<std::uint8_t,4>{0x1f,0x5d,0x3a,0x94})&&
                       checkedWeapon(0x1d9d7e8,std::array<std::uint8_t,4>{0xa6,0x5c,0x3a,0x94})){
                        originalAblationModel54=reinterpret_cast<AblationModelDraw54>(engineBase+0x1b13184);
                        experimental::renderAblation54.modelHookReady(replacePointer(engineBase+0x2e22ba0,engineBase+0x1b13184,reinterpret_cast<std::uintptr_t>(ablationModelDraw54)));
                        const auto checked69=[&](std::uintptr_t offset,const auto& bytes){
                            Mapping code{};return mapping(engineBase+offset,bytes.size(),code)&&(code.protection&PROT_EXEC)&&fingerprint(engineBase+offset,bytes);
                        };
                        bool valid69=experimental::modelProof69::proof(checked69);
                        const std::array<std::pair<std::uintptr_t,std::uintptr_t>,3> globals69{{{0x2de0958,0x4e6e14d8},{0x2de1000,0x2e7a3f0},{0x2de09a0,0x4e6d8be0}}};
                        for(const auto& [slot,offset]:globals69){std::uintptr_t value{};Mapping region{};
                            valid69=valid69&&readMemory(nullptr,engineBase+slot,&value,sizeof(value))&&value==engineBase+offset&&
                                mapping(value,8,region)&&(region.protection&PROT_READ);
                        }
                        std::uintptr_t installed69{};valid69=valid69&&readMemory(nullptr,engineBase+0x2e22ba0,&installed69,8)&&
                            installed69==reinterpret_cast<std::uintptr_t>(ablationModelDraw54);
                        if(valid69){experimental::nativeModelProbe69.configure(engineBase+0x4e6e14d8,engineBase+0x2e7a3f0,engineBase+0x4e6d8be0,
                            engineBase+0x1b12904,reinterpret_cast<experimental::NativeModelProbe69::Test>(engineBase+0x1b12904));
                            experimental::nativeModelProbeReady69.store(true,std::memory_order_release);}

                    }
#endif

                    // Cover the complete native draw vtable range before permitting
                    // primary-eye body suppression. Preserve all upstream native
                    // update/prepare/skinning work; gate only the final submissions.
                    if(checkedWeapon(primitiveDrawOffset,primitiveDrawFingerprint)&&checkedWeapon(primitiveIndexedOffset,primitiveIndexedFingerprint)&&
                       checkedWeapon(primitiveInstancedOffset,primitiveInstancedFingerprint)&&checkedWeapon(primitiveIndexedInstancedOffset,primitiveIndexedInstancedFingerprint)&&
                       checkedWeapon(primitiveNoBindOffset,primitiveNoBindFingerprint)&&checkedWeapon(primitiveNoBindCountOffset,primitiveNoBindCountFingerprint)&&
                       checkedWeapon(primitiveIndirectOffset,primitiveIndirectFingerprint)&&checkedWeapon(primitiveMultiMSCOffset,primitiveMultiMSCFingerprint)&&
                       checkedWeapon(primitiveDrawCallOffset,primitiveDrawCallFingerprint)&&checkedWeapon(primitiveMultiIndirectOffset,primitiveMultiIndirectFingerprint)){
                        const std::uintptr_t slots[]={primitiveDrawVtableSlotOffset,primitiveIndexedVtableSlotOffset,primitiveInstancedVtableSlotOffset,primitiveIndexedInstancedVtableSlotOffset};
                        const std::uintptr_t originals[]={primitiveDrawOffset,primitiveIndexedOffset,primitiveInstancedOffset,primitiveIndexedInstancedOffset};
                        const std::uintptr_t wrappers[]={reinterpret_cast<std::uintptr_t>(suppressPrimitiveDraw),reinterpret_cast<std::uintptr_t>(suppressPrimitiveIndexed),reinterpret_cast<std::uintptr_t>(suppressPrimitiveInstanced),reinterpret_cast<std::uintptr_t>(suppressPrimitiveIndexedInstanced)};
                        const std::uintptr_t dependencySlots[]={primitiveNoBindVtableSlotOffset,primitiveNoBindCountVtableSlotOffset,primitiveIndirectVtableSlotOffset,primitiveMultiMSCVtableSlotOffset,primitiveDrawCallVtableSlotOffset,primitiveMultiIndirectVtableSlotOffset};
                        const std::uintptr_t dependencyFunctions[]={primitiveNoBindOffset,primitiveNoBindCountOffset,primitiveIndirectOffset,primitiveMultiMSCOffset,primitiveDrawCallOffset,primitiveMultiIndirectOffset};
                        bool slotsValid=true;
                        for(unsigned i=0;i<6;++i){std::uintptr_t value{};slotsValid=slotsValid&&readMemory(nullptr,engineBase+dependencySlots[i],&value,sizeof(value))&&value==engineBase+dependencyFunctions[i];}
                        nativePrimitiveDraw=reinterpret_cast<decltype(nativePrimitiveDraw)>(engineBase+primitiveDrawOffset);
                        nativePrimitiveIndexed=reinterpret_cast<decltype(nativePrimitiveIndexed)>(engineBase+primitiveIndexedOffset);
                        nativePrimitiveInstanced=reinterpret_cast<decltype(nativePrimitiveInstanced)>(engineBase+primitiveInstancedOffset);
                        nativePrimitiveIndexedInstanced=reinterpret_cast<decltype(nativePrimitiveIndexedInstanced)>(engineBase+primitiveIndexedInstancedOffset);
                        unsigned patched{};
                        if(slotsValid)while(patched<4&&replacePointer(engineBase+slots[patched],engineBase+originals[patched],wrappers[patched]))++patched;
                        if(patched==4){for(unsigned i=0;i<4;++i)nativeDrawSuppression.installed(static_cast<NativeDrawKind>(i));}
                        else while(patched){--patched;if(!replacePointer(engineBase+slots[patched],wrappers[patched],engineBase+originals[patched]))patchFault=true;}
                    }
                    occlusionNativeOwnersReady.store(bodyReady&&nativeDrawSuppression.ready(),std::memory_order_release);
                    if(experimental::NativeEarlyTraceRoute::ObserveOpaqueRuns&&bodyReady&&experimental::nativeEarlyBootstrap.ready()){
                        experimental::NativeOpaqueRunHookState::Api routeApi{nullptr,
                            [](void*,std::uintptr_t a,void* b,std::size_t n)noexcept{return readMemory(nullptr,a,b,n);},
                            [](void*,std::uintptr_t a,std::size_t n,unsigned flags)noexcept{
                                Mapping region{};if(!mapping(a,n,region))return false;
                                const int required=((flags&4)?PROT_READ:0)|((flags&2)?PROT_WRITE:0)|((flags&1)?PROT_EXEC:0);
                                return (region.protection&required)==required;
                            },
                            [](void*,std::uintptr_t a,std::uintptr_t& value)noexcept{
                                if(!a||(a&(alignof(std::uintptr_t)-1)))return false;
                                value=__atomic_load_n(reinterpret_cast<const std::uintptr_t*>(a),__ATOMIC_ACQUIRE);return true;
                            },
                            [](void*,std::uintptr_t a,std::uintptr_t expected,std::uintptr_t value)noexcept{return replacePointer(a,expected,value);},
                            [](void*)noexcept{
                                return occlusionNativeOwnersReady.load(std::memory_order_acquire)&&experimental::nativeEarlyBootstrap.ready()&&
                                    __atomic_load_n(reinterpret_cast<const std::uintptr_t*>(engineBase+bodyOpaqueGotOffset),__ATOMIC_ACQUIRE)==reinterpret_cast<std::uintptr_t>(bodyDraw);
                            }};
                        if(!setupNativeOpaqueRunHooks(engineBase,routeApi)||!installNativeOpaqueRunHooks())disarmNativeOpaqueRunHooks();
                        if(opaqueRunHookState.fault())patchFault=true;
                    }
                    refreshOcclusionReady();
                }
#endif
                if(checkedWeapon(weaponManagerUpdateOffset,weaponManagerUpdateFingerprint)&&
                   checkedWeapon(weaponManagerLoopOffset,weaponManagerLoopFingerprint)&&
                   checkedWeapon(weaponManagerReturnOffset-4,weaponManagerCallFingerprint)){
                    weaponManagerBoundary.configure(reinterpret_cast<WeaponManagerBoundary::Original>(engineBase+weaponManagerUpdateOffset),observeWeaponManager);
                    weaponManagerInterposed=replacePointer(engineBase+weaponManagerGotOffset,engineBase+weaponManagerUpdateOffset,reinterpret_cast<std::uintptr_t>(updateWeaponManager));
                }
                if(checkedWeapon(weaponVectorAimOffset,weaponVectorAimFingerprint)&&
                   checkedWeapon(weaponInventoryAimOffset,weaponInventoryAimFingerprint)&&
                   checkedWeapon(weaponAimReturnOffset-4,weaponAimCallFingerprint)){
                    weaponAimBoundary.configure(reinterpret_cast<WeaponAimBoundary::Original>(engineBase+weaponVectorAimOffset),prepareWeaponAim,nullptr,afterWeaponAim);
                    weaponAimInterposed=replacePointer(engineBase+weaponAimGotOffset,engineBase+weaponVectorAimOffset,reinterpret_cast<std::uintptr_t>(aimWeapon));
                }
                if(checkedWeapon(weaponUpdatePoseOffset,weaponUpdatePoseFingerprint)&&
                   checkedWeapon(weaponInventoryPoseOffset,weaponInventoryPoseFingerprint)&&
                   checkedWeapon(weaponPoseReturnOffset-4,weaponPoseCallFingerprint)){
                    weaponPoseBoundary.configure(reinterpret_cast<WeaponPoseCallBoundary::Original>(engineBase+weaponUpdatePoseOffset),observeWeaponPose,nullptr,afterWeaponPose);
                    weaponPoseInterposed=replacePointer(engineBase+weaponPoseGotOffset,engineBase+weaponUpdatePoseOffset,reinterpret_cast<std::uintptr_t>(updateWeaponPose));
                }
#ifdef RDRVR_HEAD_CAMERA
                if(weaponPoseInterposed&&checkedWeapon(weaponDeferredCopyOffset,weaponDeferredCopyFingerprint)&&
                   checkedWeapon(weaponUpdateSkeletonOffset,weaponUpdateSkeletonFingerprint)&&
                   checkedWeapon(weaponModelCallPreludeOffset,weaponModelCallPreludeFingerprint)&&
                   checkedWeapon(weaponPropTransformOffset,weaponPropTransformFingerprint)&&
                   checkedWeapon(weaponArtPropTransformOffset,weaponArtPropTransformFingerprint)&&
                   checkedWeapon(weaponPropFragmentOffset,weaponPropFragmentFingerprint)&&
                   checkedWeapon(weaponArtPropFragmentOffset,weaponArtPropFragmentFingerprint)&&
                   checkedWeapon(weaponPropInterfaceTransformOffset,weaponPropInterfaceTransformFingerprint)&&
                   checkedWeapon(weaponArtPropInterfaceTransformOffset,weaponArtPropInterfaceTransformFingerprint)){
                    propModelBoundary.configure(reinterpret_cast<WeaponModelCallBoundary::Original>(engineBase+weaponPropTransformOffset),prepareWeaponModel,nullptr,afterWeaponModel);
                    artModelBoundary.configure(reinterpret_cast<WeaponModelCallBoundary::Original>(engineBase+weaponArtPropTransformOffset),prepareWeaponModel,nullptr,afterWeaponModel);
                    const bool prop=replacePointer(engineBase+weaponPropTransformSlotOffset,engineBase+weaponPropTransformOffset,reinterpret_cast<std::uintptr_t>(transformWeaponProp));
                    const bool art=prop&&replacePointer(engineBase+weaponArtPropTransformSlotOffset,engineBase+weaponArtPropTransformOffset,reinterpret_cast<std::uintptr_t>(transformWeaponArtProp));
                    if(prop&&!art&&!replacePointer(engineBase+weaponPropTransformSlotOffset,reinterpret_cast<std::uintptr_t>(transformWeaponProp),engineBase+weaponPropTransformOffset))patchFault=true;
                    if(prop&&art){
                        WeaponModelBindingApi model{readMemory,nullptr,engineBase+weaponObjectTableOffset,
                            engineBase+weaponGohManagerOffset,engineBase+weaponGohSlotsOffset,
                            engineBase+weaponPropTableOffset,engineBase+weaponArtPropTableOffset};
                        gameConfigureWeaponModel(model,true);weaponModelInterposed=true;
                    }
                }
#endif
                if(checkedWeapon(weaponFireProjectileOffset,weaponFireProjectileFingerprint)&&
                   checkedWeapon(weaponOnFireProjectileOffset,weaponOnFireProjectileFingerprint)&&
                   checkedWeapon(weaponFindAndFireOffset,weaponFindAndFireFingerprint)&&
                   checkedWeapon(weaponCreateExplodingOffset,weaponCreateExplodingFingerprint)&&
                   checkedWeapon(weaponFireAnimationReturnOffset-4,weaponFireAnimationCallFingerprint)&&
                   checkedWeapon(weaponFireModeReturnOffset-4,weaponFireModeCallFingerprint)&&
                   checkedWeapon(weaponFireExplodingReturnOffset-4,weaponFireExplodingCallFingerprint)){
                    weaponFireBoundary.configure(reinterpret_cast<WeaponFireBoundary::Original>(engineBase+weaponFireProjectileOffset),observeWeaponFire);
                    weaponFireInterposed=replacePointer(engineBase+weaponFireAnimationGotOffset,engineBase+weaponFireProjectileOffset,reinterpret_cast<std::uintptr_t>(fireProjectiles));
                }
#ifdef RDRVR_HEAD_CAMERA
                if(weaponFireInterposed&&checkedWeapon(weaponLaunchOffset,weaponLaunchFingerprint)&&
                   checkedWeapon(projectileLaunchReturnOffset-4,projectileLaunchCallFingerprint)&&
                   checkedWeapon(projectileLaunchPreludeOffset,projectileLaunchPreludeFingerprint)){
                    projectileLaunchBoundary.configure(reinterpret_cast<ProjectileLaunchBoundary::Original>(engineBase+weaponLaunchOffset),observeProjectileLaunch);
                    projectileLaunchInterposed=replacePointer(engineBase+projectileLaunchGotOffset,engineBase+weaponLaunchOffset,reinterpret_cast<std::uintptr_t>(launchProjectile));
                    if(projectileLaunchInterposed)gameConfigureProjectileLaunch({readMemory,nullptr,engineBase+projectileObjectTableOffset},true);
                }
#endif
                if(checkedWeapon(weaponReticleUpdateOffset,weaponReticleUpdateFingerprint)&&
                   checkedWeapon(weaponPostCameraOffset,weaponPostCameraFingerprint)&&
                   checkedWeapon(weaponReticleTargetsOffset,weaponReticleTargetsFingerprint)&&
                   checkedWeapon(weaponReticleReturnOffset-4,weaponReticleCallFingerprint)&&
                   checkedWeapon(weaponProbeExcludeOffset,weaponProbeExcludeFingerprint)&&
                   checkedWeapon(weaponProbeMultipleOffset,weaponProbeMultipleFingerprint)&&
                   checkedWeapon(weaponProbeClosestOffset,weaponProbeClosestFingerprint)&&
                   checkedWeapon(weaponActorHeadOffset,weaponActorHeadFingerprint)&&
                   checkedWeapon(weaponMainMuzzleOffset,weaponMainMuzzleFingerprint)&&
                   checkedWeapon(weaponMuzzleMatrixOffset,weaponMuzzleMatrixFingerprint)&&
                   checkedWeapon(weaponUpdatePoseOffset,weaponUpdatePoseFingerprint)&&
                   checkedWeapon(weaponInventoryPoseOffset,weaponInventoryPoseFingerprint)){
                    ReticleProbeApi probe{};probe.read=readMemory;probe.reticleTable=engineBase+weaponReticleTableOffset;
                    probe.head=reinterpret_cast<decltype(probe.head)>(engineBase+weaponActorHeadOffset);
                    probe.muzzle=reinterpret_cast<decltype(probe.muzzle)>(engineBase+weaponMainMuzzleOffset);
                    probe.attachment.read=readMemory;probe.attachment.weaponTable=engineBase+weaponObjectTableOffset;
                   probe.attachment.muzzleMatrix=reinterpret_cast<decltype(probe.attachment.muzzleMatrix)>(engineBase+weaponMuzzleMatrixOffset);
                    if(checkedWeapon(weaponWorldMatrixOffset,weaponWorldMatrixFingerprint))
                        probe.attachment.worldMatrix=reinterpret_cast<decltype(probe.attachment.worldMatrix)>(engineBase+weaponWorldMatrixOffset);
                    if(checkedWeapon(weaponSkeletonOffset,weaponSkeletonFingerprint)&&
                       checkedWeapon(weaponSkeletonUpdateOffset,weaponSkeletonUpdateFingerprint))
                        probe.attachment.skeleton=reinterpret_cast<decltype(probe.attachment.skeleton)>(engineBase+weaponSkeletonOffset);
                    probe.probe.exclude=reinterpret_cast<decltype(probe.probe.exclude)>(engineBase+weaponProbeExcludeOffset);
                    probe.probe.probe=reinterpret_cast<decltype(probe.probe.probe)>(engineBase+weaponProbeMultipleOffset);
                    probe.probe.closest=reinterpret_cast<decltype(probe.probe.closest)>(engineBase+weaponProbeClosestOffset);
                    gameConfigureReticleProbe(probe);
                    { // CAMERA: mvrMoverSimComponent vtable (ragdoll fact +0xb8c, roomscale gate). Data word + 2 code prints.
                        namespace ef=::rdrvr::vr::camera::engine_facts;std::uintptr_t got{};
                        if(::rdrvr::vr::enabled(::rdrvr::vr::Feature::Camera)&&
                           checkedWeapon(ef::moverSimCtorOffset,ef::moverSimCtorFingerprint)&&
                           checkedWeapon(ef::becomeRagdollOffset,ef::becomeRagdollFingerprint)&&
                           readMemory(nullptr,engineBase+ef::moverSimVtableGot,&got,sizeof got)&&got==engineBase+ef::moverSimVtableSymbol)
                            ::rdrvr::vr::camera::moverSimVtable.store(engineBase+ef::moverSimVtableAddressPoint,std::memory_order_release);
                    }
                    reticleBoundary.configure(reinterpret_cast<ReticleCallBoundary::Original>(engineBase+weaponReticleUpdateOffset),afterReticle);
                    reticleInterposed=replacePointer(engineBase+weaponReticleGotOffset,engineBase+weaponReticleUpdateOffset,reinterpret_cast<std::uintptr_t>(updateReticle));
                }
            }
#ifdef RDRVR_HEAD_CAMERA
            installUpdateInputs(); // VR main-thread executor even when the weapon/teleport prints failed (no-op if already tried)
            { // LOCO: own code prints + GOT values inside install(); the PLAYERMGR global is
              // verified by its GOT word here (as CHEATS). Kill switches
              // debug.rdrvr.vr.loco/.bump/.stumble (loadKillSwitches() runs in entry() before arm);
              // debug.rdrvr.vr.assist 0 -> not touched; debug.rdrvr.vr.immovable is OPT-IN (=1):
              // the engine's bit20 path zeroes the player's inverse mass against every contact
              // incl. static geometry (PreComputeImpacts 0x28903b8, no actor check).
                const auto locoGot=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t value{};
                    return readMemory(nullptr,engineBase+slot,&value,sizeof value)&&value==engineBase+target;};
                rdrvr::vr::loco::EngineApi loco{};loco.read=readMemory;loco.base=engineBase;
                if(locoGot(teleport::playerManagerGotOffset,weaponPlayerManagerOffset)){
                    loco.playerManager=engineBase+weaponPlayerManagerOffset;loco.playerManagerTable=engineBase+weaponPlayerManagerTableOffset;
                    loco.playerTable=engineBase+weaponPlayerTableOffset;
                }
                loco.footTable=engineBase+movementFootTableOffset;
                rdrvr::vr::loco::Switches sw{};
                sw.loco=rdrvr::vr::enabled(rdrvr::vr::Feature::Loco);sw.bump=rdrvr::vr::enabled(rdrvr::vr::Feature::Bump);
                sw.stumble=rdrvr::vr::enabled(rdrvr::vr::Feature::Stumble);
                sw.immovable=rdrvr::vr::loco::propertyExplicitOn("debug.rdrvr.vr.immovable");sw.assist=rdrvr::vr::loco::propertyOn("debug.rdrvr.vr.assist");
                rdrvr::vr::loco::install(loco,sw,checkedWeapon,locoGot,
                    [](std::uintptr_t a,std::uintptr_t e,std::uintptr_t r){return replacePointer(a,e,r);});
                { // Controls: FAST JUMP = JumpSequenceAnimTask::SetupNewJump GOT hook (start at LAUNCH + retimed launch clip for
                  // the local on-foot player; vr/VrControls.h). Own code prints + GOT words inside install(); PLAYERMGR verified above.
                  // debug.rdrvr.vr.fastjump 0 = not installed; debug.rdrvr.vr.jumplaunch <ms> = launch clip length (0 = native).
                    rdrvr::vr::controls::EngineApi jump{};jump.read=readMemory;jump.base=engineBase;
                    jump.playerManager=loco.playerManager;jump.playerManagerTable=loco.playerManagerTable;jump.playerTable=loco.playerTable;
                    rdrvr::vr::controls::install(jump,checkedWeapon,locoGot,
                        [](std::uintptr_t a,std::uintptr_t e,std::uintptr_t r){return replacePointer(a,e,r);});
                }
                { // MELEE: physical VR melee. install() checks whole-function prints of every
                  // engine call, the .rodata input->intention tables, GOT / vtable words, then hooks sagPlayerInputFoot::SetupActionTreePacket
                  // (GOT 0x2e53710); any mismatch leaves melee UNAVAILABLE and touches nothing else. Ticks on the UpdateInputs executor.
                  // debug.rdrvr.vr.melee 0 = nothing installed (VR row MELEE "- PROP", forced NATIVE).
                    namespace vm=::rdrvr::vr::melee;
                    vm::EngineApi melee{};melee.read=readMemory;melee.base=engineBase;
                    if(locoGot(teleport::gameDataGotOffset,teleport::gameDataOffset))melee.gameData=engineBase+teleport::gameDataOffset;
                    melee.playerManager=loco.playerManager;melee.playerManagerTable=loco.playerManagerTable;melee.playerTable=loco.playerTable;
                    melee.context=holsterContext;
                    const auto bytesMatch=[&](std::uintptr_t offset,const unsigned char* bytes,std::size_t size,int protection){
                        if(!bytes||!size||size>4096)return false;
                        Mapping m{};std::vector<unsigned char> actual(size);
                        return mapping(engineBase+offset,size,m)&&(m.protection&protection)&&readMemory(nullptr,engineBase+offset,actual.data(),size)&&
                            std::memcmp(actual.data(),bytes,size)==0;};
                    vm::install(melee,
                        [&](std::uintptr_t o,const unsigned char* b,std::size_t n){return bytesMatch(o,b,n,PROT_EXEC);},
                        [&](std::uintptr_t o,const unsigned char* b,std::size_t n){return bytesMatch(o,b,n,PROT_READ);},
                        locoGot,[](std::uintptr_t a,std::uintptr_t e,std::uintptr_t r){return replacePointer(a,e,r);});
                }
                { // AUTO COVER: GOT hooks ApplyStickInputOnFoot (0x2e536f8; filters the
                  // player's COVER input 5 = pad RB = right grip before ApplyInput reads it) and aniCover::SetAttachRequest (0x2e3c320;
                  // counts routes, drops the action-tree task route for the player). Own prints + GOT/vtable words inside install();
                  // PLAYERMGR verified above, actor slots by their GOT word here. debug.rdrvr.vr.cover 0 = nothing installed (NATIVE).
                    rdrvr::vr::cover::EngineApi cover{};cover.read=readMemory;cover.base=engineBase;
                    cover.playerManager=loco.playerManager;cover.playerManagerTable=loco.playerManagerTable;cover.playerTable=loco.playerTable;
                    if(locoGot(teleport::actorSlotsGotOffset,weaponActorSlotsOffset))cover.actorSlots=engineBase+weaponActorSlotsOffset;
                    rdrvr::vr::cover::install(cover,checkedWeapon,locoGot,
                        [](std::uintptr_t a,std::uintptr_t e,std::uintptr_t r){return replacePointer(a,e,r);});
                }
                { // BODY: the player's body in the eyes, head collapsed for its skinning, arms IK'd to
                  // the controllers. install() checks every print (both broadcasts, UpdateAttachmentsAndIK + compose site = the qualifying
                  // returns, PartialUpdate / Update = the skeleton layout, grmMatrixSet::Update + its task site) and the three GOT words, then
                  // hooks GOT 0x2e33718 (A), 0x2e33510 (B), 0x2e22ad0 (C) all-or-nothing; any mismatch -> UNAVAILABLE, nothing hooked.
                  // The skeleton chain / actor slots use MELEE's verified words (checked again here). debug.rdrvr.vr.body 0 = not installed.
                    namespace vb=::rdrvr::vr::bodyik;namespace vm=::rdrvr::vr::melee;
                    vb::EngineApi body{};body.read=readMemory;body.base=engineBase;
                    if(locoGot(teleport::gameDataGotOffset,teleport::gameDataOffset))body.gameData=engineBase+teleport::gameDataOffset;
                    body.playerManager=loco.playerManager;body.playerManagerTable=loco.playerManagerTable;body.playerTable=loco.playerTable;
                    if(locoGot(vm::actorManagerGotOffset,vm::actorManagerOffset)&&locoGot(vm::actorSlotsGotOffset,vm::actorSlotsOffset)){
                        body.actorManager=engineBase+vm::actorManagerOffset;body.actorSlots=engineBase+vm::actorSlotsOffset;}
                    if(locoGot(vm::entityVtableGotOffset,vm::entityVtableOffset)&&locoGot(vm::instGetSkeletonSlotOffset,vm::instSkeletonOffset)&&
                       checkedWeapon(vm::instSkeletonOffset,vm::instSkeletonFingerprint)){
                        body.entityVtable=engineBase+vm::entityVtableOffset+vm::vtableAddressPoint;body.instSkeletonFn=engineBase+vm::instSkeletonOffset;}
                    body.simVtable=engineBase+rdrvr::vr::loco::simVtableOffset;body.footTable=engineBase+movementFootTableOffset;
                    body.context=holsterContext;
                    vb::install(body,[&](std::uintptr_t o,const unsigned char* b,std::size_t n){
                            if(!b||!n||n>4096)return false;Mapping m{};std::vector<unsigned char> actual(n);
                            return mapping(engineBase+o,n,m)&&(m.protection&PROT_EXEC)&&readMemory(nullptr,engineBase+o,actual.data(),n)&&
                                std::memcmp(actual.data(),b,n)==0;},
                        locoGot,[](std::uintptr_t a,std::uintptr_t e,std::uintptr_t r){return replacePointer(a,e,r);});
                }
            }
            { // TELEPORT locomotion (vr/VrTeleportLoco.h): needs the verified teleport
              // API (teleport::engineReady: TeleportActorWithHeading + its 128 B ToMatrix head + GOT words) and the VR executor; then the WHOLE
              // TeleportActorToMatrix (0x404 B: the 25.0 guard, the streamer / g_playerTeleporting stores, the flag words), FindIntersectionWithMaterial
              // (+ its level GLOB_DAT), sTestCapsule (also bound into the arc probe API), EnvLiquidManager::GetInstance /
              // GetWaterElevation (+ m_Instance), ConfigureActorExcludeInsts' JUMP_SLOT, the minigame singleton GLOB_DAT; IsPositionIndoors + the
              // rmpWorld GLOB_DAT are optional (report only). Any mismatch -> UNAVAILABLE (LOCOMOTION TELEPORT runs the FREE path), nothing else
              // changes. debug.rdrvr.vr.tploco 0 -> nothing checked, rows PROP.
                namespace tl=::rdrvr::vr::tploco;namespace te=::rdrvr::vr::tploco::elf;
                const auto tpGot=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t value{};
                    return readMemory(nullptr,engineBase+slot,&value,sizeof value)&&value==engineBase+target;};
                if(!::rdrvr::vr::enabled(::rdrvr::vr::Feature::TpLoco))tl::unavailable("KILLED");
                else if(!teleport::engineReady.load(std::memory_order_acquire))tl::unavailable("NO TELEPORT API");
                else if(!vr::mainthread::executorInstalled())tl::unavailable("NO EXECUTOR");
                else if(!(checkedWeapon(te::toMatrixOffset,te::toMatrixFullFingerprint)&&checkedWeapon(te::groundOffset,te::groundFingerprint)&&
                          checkedWeapon(te::capsuleOffset,te::capsuleFingerprint)&&checkedWeapon(te::liquidInstanceOffset,te::liquidInstanceFingerprint)&&
                          checkedWeapon(te::waterOffset,te::waterElevationFingerprint)&&checkedWeapon(te::excludeOffset,weaponProbeExcludeFingerprint)&&
                          tpGot(te::groundGotOffset,te::groundOffset)&&tpGot(te::levelInstanceGotOffset,te::levelInstanceOffset)&&
                          tpGot(te::capsuleGotOffset,te::capsuleOffset)&&tpGot(te::liquidInstanceGotOffset,te::liquidInstanceOffset)&&
                          tpGot(te::liquidGlobalGotOffset,te::liquidGlobalOffset)&&tpGot(te::waterGotOffset,te::waterOffset)&&
                          tpGot(te::excludeGotOffset,te::excludeOffset)&&tpGot(te::minigameGotOffset,te::minigameOffset)))tl::unavailable("ENGINE MISMATCH");
                else{
                    tl::EngineApi e{};
                    {std::lock_guard lock(teleport::state.mutex);e.tp=teleport::state.api;}
                    e.ground=reinterpret_cast<tl::GroundFn>(engineBase+te::groundOffset);
                    e.capsule=reinterpret_cast<tl::CapsuleFn>(engineBase+te::capsuleOffset);
                    e.liquid=reinterpret_cast<tl::LiquidInstanceFn>(engineBase+te::liquidInstanceOffset);
                    e.water=reinterpret_cast<tl::WaterFn>(engineBase+te::waterOffset);
                    if(checkedWeapon(te::indoorsOffset,te::indoorsFingerprint)&&tpGot(te::worldGotOffset,te::worldOffset)){
                        e.indoors=reinterpret_cast<tl::IndoorsFn>(engineBase+te::indoorsOffset);e.worldInstance=engineBase+te::worldOffset;}
                    e.minigameInstance=engineBase+te::minigameOffset;
                    tl::bindArcCapsule(e.capsule); // the shared sTestCapsule print (also used by the lasso's capsule chords)
                    tl::install(e);
                }
            }
            { // LASSO: graded install. H (holsters-only rules) needs
              // nothing more; R = the reader prints (zoom release, aniLasso setters / hogtie, CheckIsLassoAttached / MiniGameFocus, the victim read,
              // OnInit data, the data parser, the LassoProbe flags) + the GLOB_DAT words (minigame singleton, sm_Gravity, sm_GlobalManager,
              // sm_FlashProxy) + the actor-table words (MELEE's); L = the launch prints + the JUMP_SLOTs of H1 (invWeapon::OnUseItem from
              // invLassoWeapon::OnUseItem) and H2 (UpdateInFlight from OnUpdate), installed H2 then H1, all or nothing; O = the off-screen block +
              // IsPointSetVisible's JUMP_SLOT (H3, first). H4 (`.lasso.launch direct`) needs the live vtable word vt[0x30] == OnUseItem.
              // debug.rdrvr.vr.lasso 0 -> nothing checked or hooked, rows PROP.
                namespace vl=::rdrvr::vr::lasso;namespace le=::rdrvr::vr::lasso::elf;namespace vm=::rdrvr::vr::melee;namespace te=::rdrvr::vr::tploco::elf;
                const auto lGot=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t value{};
                    return readMemory(nullptr,engineBase+slot,&value,sizeof value)&&value==engineBase+target;};
                const auto allPrints=[&](const auto& list){bool ok=true;for(const auto& p:list){Mapping m{};std::vector<unsigned char> actual(p.size);
                    ok=ok&&mapping(engineBase+p.offset,p.size,m)&&(m.protection&PROT_EXEC)&&readMemory(nullptr,engineBase+p.offset,actual.data(),p.size)&&
                        std::memcmp(actual.data(),p.bytes,p.size)==0;}
                    return ok;};
                vl::config=vl::readConfig();
                vl::Checks c{};
                if(::rdrvr::vr::enabled(::rdrvr::vr::Feature::Lasso)){
                    c.executor=vr::mainthread::executorInstalled();c.holsters=vr::holsters::engineReady.load(std::memory_order_acquire);
                    c.readerPrints=allPrints(le::readerPrints);
                    c.readerGlobals=lGot(le::minigameGotOffset,le::minigameOffset)&&lGot(le::gravityGotOffset,le::gravityOffset)&&lGot(le::timeGotOffset,le::timeOffset)&&
                        lGot(le::proxyGotOffset,le::proxyOffset)&&lGot(vm::actorManagerGotOffset,vm::actorManagerOffset)&&lGot(vm::actorSlotsGotOffset,vm::actorSlotsOffset);
                    c.launchPrints=allPrints(le::launchPrints);
                    c.launchGots=lGot(le::useItemGotOffset,le::weaponUseItemOffset)&&lGot(le::inFlightGotOffset,le::inFlightOffset);
                    c.offscreenPrint=allPrints(le::offscreenPrints);c.offscreenGot=lGot(le::visibleGotOffset,le::visibleOffset);
                    c.directWord=lGot(le::useItemVtableWordOffset,le::onUseItemOffset);
                    vl::Env e{};e.read=readMemory;e.base=engineBase;e.lassoItemTable=engineBase+le::lassoItemTableOffset;
                    e.actorManagerGlobal=engineBase+vm::actorManagerOffset;e.actorSlotsGlobal=engineBase+vm::actorSlotsOffset;
                    e.minigameGlobal=engineBase+le::minigameOffset;e.gravityObject=engineBase+le::gravityOffset;e.timeObject=engineBase+le::timeOffset;
                    e.proxyObject=engineBase+le::proxyOffset;
                    e.useItemOriginal=reinterpret_cast<vl::UseItemFn>(engineBase+le::weaponUseItemOffset);
                    e.inFlightOriginal=reinterpret_cast<vl::InFlightFn>(engineBase+le::inFlightOffset);
                    e.visibleOriginal=reinterpret_cast<vl::VisibleFn>(engineBase+le::visibleOffset);
                    if(c.directWord)e.directUseItem=reinterpret_cast<vl::UseItemFn>(engineBase+le::onUseItemOffset);
                    e.useItemReturn=engineBase+le::useItemReturnOffset;e.inFlightReturn=engineBase+le::inFlightReturnOffset;
                    for(std::size_t i=0;i<e.offscreenReturns.size();++i)e.offscreenReturns[i]=engineBase+le::offscreenReturnOffsets[i];
                    vl::env=e; // before any hook can run
                    // The engine capsule probe for the LASSO arc: TELEPORT binds it; without TELEPORT, the same print here
                    if(const auto* api=::rdrvr::vr::arc::engineApi.load(std::memory_order_acquire);api&&*api&&!api->capsule&&
                       checkedWeapon(te::capsuleOffset,te::capsuleFingerprint)&&lGot(te::capsuleGotOffset,te::capsuleOffset))
                        ::rdrvr::vr::tploco::bindArcCapsule(reinterpret_cast<::rdrvr::vr::tploco::CapsuleFn>(engineBase+te::capsuleOffset));
                }
                vl::Slots sl{};
                sl.useGot=engineBase+le::useItemGotOffset;sl.useOriginal=engineBase+le::weaponUseItemOffset;sl.useHook=reinterpret_cast<std::uintptr_t>(&vl::useItemHook);
                sl.flightGot=engineBase+le::inFlightGotOffset;sl.flightOriginal=engineBase+le::inFlightOffset;sl.flightHook=reinterpret_cast<std::uintptr_t>(&vl::inFlightHook);
                sl.visibleGot=engineBase+le::visibleGotOffset;sl.visibleOriginal=engineBase+le::visibleOffset;sl.visibleHook=reinterpret_cast<std::uintptr_t>(&vl::visibleHook);
                std::string lassoFailures;
                const unsigned grades=vl::installGrades(c,sl,[](void*,std::uintptr_t a,std::uintptr_t e,std::uintptr_t r)noexcept{
                    try{return replacePointer(a,e,r);}catch(...){return false;}},nullptr,lassoFailures);
                vl::finishInstall(grades,lassoFailures);
            }
            { // QUEST MARKER: the radar blip table and the GPS waypoints are only READ (no hook) by
              // ModuleMarker. Prints of every layout it relies on + the GLOB_DAT words of CRadar::RadarTrace / RadarBlips (required), hudManager (GPS
              // waypoints) and ScriptGlowManager (optional). Any required mismatch -> row UNAVAILABLE, nothing read. debug.rdrvr.vr.marker 0 -> row PROP.
                namespace mk=::rdrvr::vr::marker;namespace me=::rdrvr::vr::marker::elf;
                if(!::rdrvr::vr::enabled(::rdrvr::vr::Feature::Marker))mk::refuse(mk::Install::Property,"PROP");
                else{
                    const auto mGot=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t value{};
                        return readMemory(nullptr,engineBase+slot,&value,sizeof value)&&value==engineBase+target;};
                    const bool prints=checkedWeapon(me::traceComplexCtorOffset,me::traceComplexCtorFingerprint)&&checkedWeapon(me::setBlipParamOffset,me::setBlipParamFingerprint)&&
                        checkedWeapon(me::entityBlipStoreOffset,me::entityBlipStoreFingerprint)&&checkedWeapon(me::objectiveVisibleOffset,me::objectiveVisibleFingerprint)&&
                        checkedWeapon(me::blipDescCtorOffset,me::blipDescCtorFingerprint)&&checkedWeapon(me::modelLoopOffset,me::modelLoopFingerprint);
                    const bool gps=checkedWeapon(me::waypointSetOffset,me::waypointSetFingerprint)&&checkedWeapon(me::gpsSlotOffset,me::gpsSlotFingerprint)&&
                        mGot(me::hudManagerGotOffset,me::hudManagerOffset);
                    const bool glow=checkedWeapon(me::scriptGlowCreateOffset,me::scriptGlowCreateFingerprint)&&mGot(me::glowManagerGotOffset,me::glowManagerOffset);
                    if(!prints)mk::refuse(mk::Install::Prints,"PRINTS");
                    else if(!mGot(me::radarTraceGotOffset,me::radarTraceOffset)||!mGot(me::radarBlipsGotOffset,me::radarBlipsOffset))mk::refuse(mk::Install::Got,"GOT");
                    else{mk::Env e{};e.read=readMemory;e.radarTrace=engineBase+me::radarTraceOffset;e.radarBlips=engineBase+me::radarBlipsOffset;
                        if(gps)e.hudManager=engineBase+me::hudManagerOffset;
                        if(glow)e.glowManager=engineBase+me::glowManagerOffset;
                        mk::install(e);}
                }
            }
            { // AppSW per-actor motion vectors - the actor table words (MELEE's), the VR executor; nothing is hooked.
                namespace vm=::rdrvr::vr::melee;namespace mva=::rdrvr::vr::mvactors;
                const auto aGot=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t value{};
                    return readMemory(nullptr,engineBase+slot,&value,sizeof value)&&value==engineBase+target;};
                mva::Env e{};
                if(aGot(vm::actorManagerGotOffset,vm::actorManagerOffset)&&aGot(vm::actorSlotsGotOffset,vm::actorSlotsOffset)){
                    e.read=readMemory;e.actorManager=engineBase+vm::actorManagerOffset;e.actorSlots=engineBase+vm::actorSlotsOffset;}
                // Per-bone vectors (vr/VrMotionBones.h): the skeleton chain words as BODY checks
                // them (entity vtable GOT word, the inst vtable's GetSkeleton slot, the GetSkeleton
                // print); any mismatch -> per-bone vectors unavailable (root only).
                if(e.read&&aGot(vm::entityVtableGotOffset,vm::entityVtableOffset)&&aGot(vm::instGetSkeletonSlotOffset,vm::instSkeletonOffset)&&
                   checkedWeapon(vm::instSkeletonOffset,vm::instSkeletonFingerprint)){
                    e.entityVtable=engineBase+vm::entityVtableOffset+vm::vtableAddressPoint;e.instSkeletonFn=engineBase+vm::instSkeletonOffset;}
                mva::install(e);
            }
            { // RELOAD: every print of reload::elf::prints
              // (the whole aniAim::RequestReload, AddAmmo / SetAmmo / DryFire / RemoveAmmo / GetAmmoPerClip, the getters the hook and the executor
              // rely on, invWeapon::OnUpdate / OnReHolster for the +0xd0 refill timer, the fire decrement, DiscardAmmo, the AmmoComponent getters)
              // + the three JUMP_SLOT words; then ModuleReload, the RequestReload GOT hook (required) and the AddAmmo / SetAmmo audit hooks
              // (optional, `.reload.audit 0`). Any mismatch -> effective NATIVE (B = the game's reload, nothing blocked), row UNAVAILABLE, the
              // reason in vr.weapon.reload.install. debug.rdrvr.vr.reload 0 -> nothing checked or hooked, row PROP.
                namespace vrl=::rdrvr::vr::reload;namespace re=::rdrvr::vr::reload::elf;
                if(vrl::config.enabled&&::rdrvr::vr::enabled(::rdrvr::vr::Feature::Reload)){
                    const auto rGot=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t value{};
                        return readMemory(nullptr,engineBase+slot,&value,sizeof value)&&value==engineBase+target;};
                    bool prints=true;
                    for(const auto& p:re::prints){Mapping m{};std::vector<unsigned char> actual(p.size);
                        prints=prints&&mapping(engineBase+p.offset,p.size,m)&&(m.protection&PROT_EXEC)&&readMemory(nullptr,engineBase+p.offset,actual.data(),p.size)&&
                            std::memcmp(actual.data(),p.bytes,p.size)==0;}
                    const bool gots=rGot(re::requestReloadGotOffset,re::requestReloadOffset)&&rGot(re::addAmmoGotOffset,re::addAmmoOffset)&&
                        rGot(re::setAmmoGotOffset,re::setAmmoOffset);
                    vrl::Env e{};e.read=readMemory;e.base=engineBase;e.weaponItemTable=engineBase+::rdrvr::vr::holsters::weaponItemTableOffset;
                    e.addAmmo=reinterpret_cast<vrl::AddAmmoFn>(engineBase+re::addAmmoOffset);e.setAmmo=reinterpret_cast<vrl::SetAmmoFn>(engineBase+re::setAmmoOffset);
                    e.removeAmmo=reinterpret_cast<vrl::RemoveAmmoFn>(engineBase+re::removeAmmoOffset);
                    e.perClip=reinterpret_cast<vrl::PerClipFn>(engineBase+re::ammoPerClipOffset);e.dryFire=reinterpret_cast<vrl::DryFireFn>(engineBase+re::dryFireOffset);
                    vrl::Slots sl{};sl.requestGot=engineBase+re::requestReloadGotOffset;sl.requestOriginal=engineBase+re::requestReloadOffset;
                    sl.addGot=engineBase+re::addAmmoGotOffset;sl.addOriginal=engineBase+re::addAmmoOffset;
                    sl.setGot=engineBase+re::setAmmoGotOffset;sl.setOriginal=engineBase+re::setAmmoOffset;
                    vrl::install(e,prints,gots,sl,[](std::uintptr_t a,std::uintptr_t x,std::uintptr_t r){try{return replacePointer(a,x,r);}catch(...){return false;}});
                }else vrl::installState.store(int(vrl::Install::Killed),std::memory_order_release);
            }
            { // HAMMER: every print of hammer::elf::prints (exact bytes in an
              // executable mapping) graded by class - P the RT pulse path, R the field reads, O the
              // ChamberTime override, V the visual H2 - + the GOT word of rage::sysTimeManager::sm_GlobalManager (the game time T; R and O need it)
              // + the five aniAim vtable slots (the aim pointer check). Nothing is hooked. debug.rdrvr.vr.hammer 0 -> not started, nothing checked.
                namespace vhm=::rdrvr::vr::hammer;namespace he=::rdrvr::vr::hammer::elf;
                if(vhm::started.load(std::memory_order_acquire)){
                    std::array<bool,he::prints.size()> matched{};
                    for(std::size_t i=0;i<he::prints.size();++i){const auto& p=he::prints[i];Mapping m{};std::vector<unsigned char> actual(p.size);
                        matched[i]=mapping(engineBase+p.offset,p.size,m)&&(m.protection&PROT_EXEC)&&readMemory(nullptr,engineBase+p.offset,actual.data(),p.size)&&
                            std::memcmp(actual.data(),p.bytes,p.size)==0;}
                    std::uintptr_t timeValue{};
                    const bool got=readMemory(nullptr,engineBase+he::timeGotOffset,&timeValue,sizeof timeValue)&&timeValue==engineBase+he::timeGlobalOffset;
                    bool vtable=true;
                    for(std::size_t i=0;i<he::aimSlots.size();++i){std::uintptr_t v{};
                        vtable=vtable&&readMemory(nullptr,engineBase+he::aimVptrOffset+i*8,&v,sizeof v)&&v==engineBase+he::aimSlots[i];}
                    vhm::Env e{};e.read=readMemory;e.base=engineBase;e.weaponTable=engineBase+weaponObjectTableOffset;
                    e.timeAddr=got?engineBase+he::timeGlobalOffset+he::timeField:0;e.aimVptr=engineBase+he::aimVptrOffset;
                    vhm::configureEngine(e,matched,got,vtable);
                }
            }
            { // CHEATS: rows on the VR menu CHEATS page; every engine function/global verified inside install() (whole-function
              // prints, GOT words); executed on the main thread by the UpdateInputs wrapper. debug.rdrvr.vr.cheats 0 = no rows, no engine use.
                namespace vc=::rdrvr::vr::cheats;
                const auto cheatGot=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t value{};
                    return readMemory(nullptr,engineBase+slot,&value,sizeof value)&&value==engineBase+target;};
                vc::EngineApi c{};c.read=readMemory;c.gameData=engineBase+vc::gameDataOffset;
                if(cheatGot(teleport::playerManagerGotOffset,weaponPlayerManagerOffset)&&cheatGot(teleport::actorManagerGotOffset,weaponActorManagerOffset)&&
                   cheatGot(teleport::actorSlotsGotOffset,weaponActorSlotsOffset)){
                    c.playerManager=engineBase+weaponPlayerManagerOffset;c.playerManagerTable=engineBase+weaponPlayerManagerTableOffset;
                    c.playerTable=engineBase+weaponPlayerTableOffset;c.actorManagerGlobal=engineBase+weaponActorManagerOffset;
                    c.actorSlotsGlobal=engineBase+weaponActorSlotsOffset;
                    c.inventoryTable=engineBase+weaponInventoryTableOffset; // Ammo rows use only a verified inventory component
                }
                vc::install(c,engineBase,checkedWeapon,cheatGot);
                // RAIN QUALITY: DROPS = print-checked rain quad count scaled around the
                // early TAIL IN EYES weather call (PostFxSkip.h); DRY = GOT hook CVfxWeather::PreRender (0x2e5d838) zeroing the lights-manager
                // wetness copy. Row appended to CHEATS (next to WEATHER); debug.rdrvr.rain 0..6 overrides it, debug.rdrvr.rainhook 0 = no hook.
                ::rdrvr::vr::rain::install(engineBase,checkedWeapon,cheatGot,[](std::uintptr_t a,std::uintptr_t e,std::uintptr_t r){return replacePointer(a,e,r);});
                // SAVE FIX 2: AddItem watcher (VrCheats.h); the slot must hold exactly the engine's AddItem. debug.rdrvr.vr.cheats 0 = none.
                if(!::rdrvr::vr::killed(::rdrvr::vr::Feature::Cheats)&&cheatGot(vc::addItemSlotOffset,vc::addItemOffset)){
                    vc::addItemOriginal.store(reinterpret_cast<vc::AddItemFn>(engineBase+vc::addItemOffset),std::memory_order_release);
                    if(!replacePointer(engineBase+vc::addItemSlotOffset,engineBase+vc::addItemOffset,reinterpret_cast<std::uintptr_t>(&vc::addItemHook)))
                        vc::addItemOriginal.store(nullptr,std::memory_order_release);
                }
                ::rdrvr::vr::registerCheatRow({"RAIN QUALITY",&::rdrvr::vr::rain::rowValue,&::rdrvr::vr::rain::rowAdjust,&::rdrvr::vr::rain::rowActivate,nullptr});
                // TELEPORT TO: stick = town, A = teleport (Teleport.h towns; on foot only, like the TELEPORT row)
                ::rdrvr::vr::registerCheatRow({"TELEPORT TO",
                    [](void*,char* out,std::size_t size)noexcept{namespace tp=::rdrvr::teleport;const int i=std::clamp(tp::cheatTown.load(std::memory_order_relaxed),0,tp::townCount-1);
                        const auto fb=tp::feedback.load(std::memory_order_relaxed);const char* why=tp::feedbackReason.load(std::memory_order_relaxed);
                        std::snprintf(out,size,"%s%s%s",tp::towns[i].name,fb==tp::Feedback::TpPending?" - GOING":fb==tp::Feedback::TpFailed?" - FAILED ":"",fb==tp::Feedback::TpFailed&&why?why:"");},
                    [](void*,int dir)noexcept{namespace tp=::rdrvr::teleport;tp::cheatTown.store((tp::cheatTown.load(std::memory_order_relaxed)+dir+tp::townCount)%tp::townCount);},
                    [](void*)noexcept{namespace tp=::rdrvr::teleport;if(tp::engineReady.load(std::memory_order_acquire))tp::cheatTownRequest.store(tp::cheatTown.load(std::memory_order_relaxed)+1);},
                    nullptr});
                ::rdrvr::vr::registerCheatRow({"MY SPOTS",
                    [](void*,char* out,std::size_t size)noexcept{::rdrvr::teleport::menuText(out,size);},
                    [](void*,int dir)noexcept{::rdrvr::teleport::menuInput(dir>0?1:-1,false);},
                    [](void*)noexcept{::rdrvr::teleport::menuInput(0,true);},
                    nullptr});
            }
#endif
#ifdef RDRVR_NATIVE_MENUS
            Mapping uiCode{};
            if(mapping(engineBase+uiMenuOffset,32,uiCode)&&(uiCode.protection&PROT_EXEC)&&fingerprint(engineBase+uiMenuOffset,uiMenuFingerprint)&&
               mapping(engineBase+uiCutsceneOffset,32,uiCode)&&(uiCode.protection&PROT_EXEC)&&fingerprint(engineBase+uiCutsceneOffset,uiCutsceneFingerprint)){
                NativeCameraApi camera{};
                if(checkedWeapon(uiCameraInitChannelsOffset,uiCameraInitChannelsFingerprint)&&
                   checkedWeapon(uiCameraGameActiveOffset,uiCameraGameActiveFingerprint)&&checkedWeapon(uiCameraTopOffset,uiCameraTopFingerprint)&&
                   checkedWeapon(uiCameraChannelUpdateOffset,uiCameraChannelUpdateFingerprint)&&
                   checkedWeapon(uiCameraTransitionUpdateOffset,uiCameraTransitionUpdateFingerprint)&&
                   checkedWeapon(uiCameraTransitionResetOffset,uiCameraTransitionResetFingerprint))
                    camera={readMemory,nullptr,engineBase+uiCameraSingletonOffset,engineBase+uiCameraTableOffset,engineBase+uiCameraChannelTableOffset,engineBase+uiCameraSingletonTableOffset};
                NativeScopeApi scope{};
                if(checkedWeapon(uiScopeActiveOffset,uiScopeActiveFingerprint)&&checkedWeapon(uiScopeSetStateOffset,uiScopeSetStateFingerprint)&&
                   checkedWeapon(uiScopePreDrawOffset,uiScopePreDrawFingerprint)&&checkedWeapon(uiScopeInitOffset,uiScopeInitFingerprint)&&
                   checkedWeapon(uiScopeConstructorOffset,uiScopeConstructorFingerprint))
                    scope={readMemory,nullptr,engineBase+uiScopeSingletonOffset,engineBase+uiScopeTableOffset,engineBase+uiScopeDisableParamOffset,engineBase+uiScopeSingletonTableOffset};
                uiMenuActiveNative=reinterpret_cast<bool(*)()>(engineBase+uiMenuOffset);uiCutscenePlayingNative=reinterpret_cast<bool(*)()>(engineBase+uiCutsceneOffset);
#ifdef RDRVR_HEAD_CAMERA
                gameConfigureCutsceneActors(engineBase+uiCutsceneSingletonOffset,engineBase+uiCutsceneTableOffset,
                    uiCutsceneSingletonTableOffset?engineBase+uiCutsceneSingletonTableOffset:0,
                    reinterpret_cast<const char*(*)(std::uintptr_t,int)>(engineBase+0x23f5748), // Rdr2CutSceneManager::GetCutsceneActorName(int) const
                    engineBase+weaponGohManagerOffset,engineBase+0x4e6ecfd8u); // aGuidGeneral<gohBase> sm_Manager / sm_ManagerSlots
#endif
                gameConfigureUi({readMemory,&uiMenuUnlessCutscene,engineBase+uiSingletonOffset,engineBase+uiTableOffset,
                    reinterpret_cast<bool(*)()>(engineBase+uiCutsceneOffset),engineBase+uiCutsceneSingletonOffset,engineBase+uiCutsceneTableOffset,camera,scope,engineBase+uiSingletonTableOffset,engineBase+uiCutsceneSingletonTableOffset});
#ifdef RDRVR_HEAD_CAMERA
                if(camera&&checkedWeapon(turnSetOrientationOffset,turnSetOrientationFingerprint)&&
                   checkedWeapon(turnViewportOrientationOffset,turnViewportOrientationFingerprint)&&
                   checkedWeapon(turnCmxGetEulersOffset,turnCmxGetEulersFingerprint)&&
                   checkedWeapon(turnCmxSetEulersOffset,turnCmxSetEulersFingerprint)&&
                   checkedWeapon(turnCmxParametersOffset,turnCmxParametersFingerprint)&&
                   checkedWeapon(turnInitGameCameraOffset,turnInitGameCameraFingerprint)&&
                   checkedWeapon(turnMatrixEulersOffset,turnMatrixEulersFingerprint))
                    gameConfigureSnapTurn({camera,engineBase+turnGameTableOffset,engineBase+turnCmxTableOffset,
                        reinterpret_cast<decltype(NativeCameraTurnApi::setOrientation)>(engineBase+turnSetOrientationOffset)});
#endif
            }
            struct HudRoute {std::uintptr_t slot,original,replacement;const std::array<unsigned char,32>* print;};
            const HudRoute hudRoutes[]{
                {hudInventoryGotOffset,hudInventoryOffset,reinterpret_cast<std::uintptr_t>(hudInventory),&hudInventoryFingerprint},
                {hudFlashGotOffset,hudFlashOffset,reinterpret_cast<std::uintptr_t>(hudFlash),&hudFlashFingerprint},
                {hudUiGotOffset,hudUiOffset,reinterpret_cast<std::uintptr_t>(hudUi),&hudUiFingerprint},
                {hudRadarGotOffset,hudRadarOffset,reinterpret_cast<std::uintptr_t>(hudRadar),&hudRadarFingerprint},
                {hudFrameGotOffset,hudFrameOffset,reinterpret_cast<std::uintptr_t>(hudFrame),&hudFrameFingerprint},
                {hudBeginGotOffset,hudBeginOffset,reinterpret_cast<std::uintptr_t>(hudBegin),&hudBeginFingerprint},
                {hudEndGotOffset,hudEndOffset,reinterpret_cast<std::uintptr_t>(hudEnd),&hudEndFingerprint},
                {hudRenderFlushGotOffset,hudRenderFlushOffset,reinterpret_cast<std::uintptr_t>(hudRenderFlush),&hudRenderFlushFingerprint},
                {hudRenderBindGotOffset,hudRenderBindOffset,reinterpret_cast<std::uintptr_t>(hudRenderBind),&hudRenderBindFingerprint}};
            bool hudValid=true;
            for(const auto& route:hudRoutes){Mapping code{};hudValid=hudValid&&mapping(engineBase+route.original,32,code)&&
                (code.protection&PROT_EXEC)&&fingerprint(engineBase+route.original,*route.print);}
            Mapping blendMap{},setMap{},factoryFlushMap{};
            hudValid=hudValid&&mapping(engineBase+hudBlendBlockOffset,HudBlendReader::bytes,blendMap)&&
                (blendMap.protection&(PROT_READ|PROT_WRITE|PROT_EXEC))==(PROT_READ|PROT_WRITE)&&
                mapping(engineBase+hudRenderSetOffset,32,setMap)&&(setMap.protection&PROT_EXEC)&&
                fingerprint(engineBase+hudRenderSetOffset,hudRenderSetFingerprint)&&
                mapping(engineBase+hudFactoryFlushOffset,32,factoryFlushMap)&&(factoryFlushMap.protection&PROT_EXEC)&&
                fingerprint(engineBase+hudFactoryFlushOffset,hudFactoryFlushFingerprint);
            if(hudValid){
                originalHudInventory=reinterpret_cast<HudDraw>(engineBase+hudInventoryOffset);originalHudFlash=reinterpret_cast<HudDraw>(engineBase+hudFlashOffset);
                originalHudUi=reinterpret_cast<HudUiDraw>(engineBase+hudUiOffset);originalHudRadar=reinterpret_cast<HudRadarDraw>(engineBase+hudRadarOffset);
                originalHudFrame=reinterpret_cast<HudRadarDraw>(engineBase+hudFrameOffset);originalHudEnd=reinterpret_cast<HudRadarDraw>(engineBase+hudEndOffset);
                originalHudBegin=reinterpret_cast<HudBegin>(engineBase+hudBeginOffset);
                originalHudRenderFlush=reinterpret_cast<HudBegin>(engineBase+hudRenderFlushOffset);
                originalHudRenderBind=reinterpret_cast<HudRoutingApi::Bind>(engineBase+hudRenderBindOffset);
                unsigned count{};
                for(const auto& route:hudRoutes){if(!replacePointer(engineBase+route.slot,engineBase+route.original,route.replacement))break;++count;}
                if(count==std::size(hudRoutes)){
#ifdef RDRVR_HEAD_CAMERA
#endif
                    gameConfigureHudRender({engineBase+hudBlendBlockOffset,reinterpret_cast<void(*)(unsigned,int)>(engineBase+hudRenderSetOffset),originalHudRenderFlush,originalHudRenderBind,reinterpret_cast<HudBegin>(engineBase+hudFactoryFlushOffset)});
                    hudBoundary.configure(gameObserveHud);hudFrameBoundary.configure(gameObserveHudFrame);hudInterposed.store(true,std::memory_order_release);
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_NATIVE_MENUS)
                    gameConfigureHudScreenViewport(engineBase+0x4e6e14e0u); // rage::grcViewport::sm_ScreenViewport (grcViewport*)
#endif
                    // ClearAlpha drop hooks (PIXBegin / PIXEnd JUMP_SLOTs). Both or none; without them the VR HUD stays off.
                    if(!::rdrvr::vr::killed(::rdrvr::vr::Feature::Hud)){
                        Mapping pb{},pe{};
                        const bool pixValid=mapping(engineBase+pixBeginOffset,16,pb)&&(pb.protection&PROT_EXEC)&&fingerprint(engineBase+pixBeginOffset,pixBeginFingerprint)&&
                            mapping(engineBase+pixEndOffset,16,pe)&&(pe.protection&PROT_EXEC)&&fingerprint(engineBase+pixEndOffset,pixEndFingerprint)&&
                            fingerprint(engineBase+clearAlphaBeginReturnOffset-4,clearAlphaBeginCall)&&fingerprint(engineBase+clearAlphaEndReturnOffset-4,clearAlphaEndCall);
                        if(pixValid){
                            originalPixBegin=reinterpret_cast<PixBeginFn>(engineBase+pixBeginOffset);originalPixEnd=reinterpret_cast<PixEndFn>(engineBase+pixEndOffset);
                            if(replacePointer(engineBase+pixBeginGotOffset,engineBase+pixBeginOffset,reinterpret_cast<std::uintptr_t>(hudPixBegin))){
                                if(replacePointer(engineBase+pixEndGotOffset,engineBase+pixEndOffset,reinterpret_cast<std::uintptr_t>(hudPixEnd)))::rdrvr::vr::hud::setClearAlphaHooked(true);
                                else if(!replacePointer(engineBase+pixBeginGotOffset,reinterpret_cast<std::uintptr_t>(hudPixBegin),engineBase+pixBeginOffset))patchFault=true;
                            }
                        }
                        if(!pixValid)::rdrvr::vr::hud::noteClearAlphaFingerprint(false);
                        // RADAR FIX: SetWindow / SetScissor JUMP_SLOTs, only DrawRadar's two calls are changed
                        Mapping rw{},rs{};
                        if(mapping(engineBase+radarSetWindowOffset,16,rw)&&(rw.protection&PROT_EXEC)&&fingerprint(engineBase+radarSetWindowOffset,radarSetWindowFingerprint)&&
                           mapping(engineBase+radarSetScissorOffset,16,rs)&&(rs.protection&PROT_EXEC)&&fingerprint(engineBase+radarSetScissorOffset,radarSetScissorFingerprint)&&
                           fingerprint(engineBase+radarSetWindowReturnOffset-4,radarSetWindowCall)&&fingerprint(engineBase+radarSetScissorReturnOffset-4,radarSetScissorCall)){
                            originalRadarSetWindow=reinterpret_cast<RadarSetWindowFn>(engineBase+radarSetWindowOffset);
                            originalRadarSetScissor=reinterpret_cast<RadarSetScissorFn>(engineBase+radarSetScissorOffset);
                            if(replacePointer(engineBase+radarSetWindowGotOffset,engineBase+radarSetWindowOffset,reinterpret_cast<std::uintptr_t>(radarSetWindow))){
                                if(replacePointer(engineBase+radarSetScissorGotOffset,engineBase+radarSetScissorOffset,reinterpret_cast<std::uintptr_t>(radarSetScissor)))radarFixHooked.store(true);
                                else if(!replacePointer(engineBase+radarSetWindowGotOffset,reinterpret_cast<std::uintptr_t>(radarSetWindow),engineBase+radarSetWindowOffset))patchFault=true;
                            }
                            Mapping rm{};
                            if(radarFixHooked.load()&&mapping(engineBase+radarMapOffset,16,rm)&&(rm.protection&PROT_EXEC)&&fingerprint(engineBase+radarMapOffset,radarMapFingerprint)&&
                               fingerprint(engineBase+grcSetStateOffset,grcSetStateFingerprint)&&fingerprint(engineBase+grcGetStateOffset,grcGetStateFingerprint)&&
                               fingerprint(engineBase+radarMapReturnOffset-4,radarMapCall)){
                                originalRadarMap=reinterpret_cast<RadarMapFn>(engineBase+radarMapOffset);
                                grcSetState=reinterpret_cast<GrcSetStateFn>(engineBase+grcSetStateOffset);grcGetState=reinterpret_cast<GrcGetStateFn>(engineBase+grcGetStateOffset);
                                if(replacePointer(engineBase+radarMapGotOffset,engineBase+radarMapOffset,reinterpret_cast<std::uintptr_t>(radarDrawMap)))radarMapHooked.store(true);
                            }
                            Mapping gb{},ge{};
                            if(radarMapHooked.load()&&mapping(engineBase+grcBeginOffset,16,gb)&&(gb.protection&PROT_EXEC)&&mapping(engineBase+grcEndOffset,16,ge)&&(ge.protection&PROT_EXEC)&&
                               fingerprint(engineBase+grcBeginOffset,grcBeginFingerprint)&&fingerprint(engineBase+grcEndOffset,grcEndFingerprint)&&
                               fingerprint(engineBase+radarQuadBeginReturnOffset-4,radarQuadBeginCall)&&fingerprint(engineBase+radarQuadEndReturnOffset-4,radarQuadEndCall)){
                                originalGrcBegin=reinterpret_cast<GrcBeginFn>(engineBase+grcBeginOffset);originalGrcEnd=reinterpret_cast<GrcEndFn>(engineBase+grcEndOffset);
                                if(replacePointer(engineBase+grcEndGotOffset,engineBase+grcEndOffset,reinterpret_cast<std::uintptr_t>(radarGrcEnd))){
                                    if(replacePointer(engineBase+grcBeginGotOffset,engineBase+grcBeginOffset,reinterpret_cast<std::uintptr_t>(radarGrcBegin)))radarQuadHooked.store(true);
                                    else if(!replacePointer(engineBase+grcEndGotOffset,reinterpret_cast<std::uintptr_t>(radarGrcEnd),engineBase+grcEndOffset))patchFault=true;}
                            }
                        }
                        { // SUBTITLES: hudManager::SetSubtitlesEnabled + atSingleton<hudManager>::sm_Instance
                            constexpr std::array<unsigned char,16> fp{
#include "gamecode/TraceEntry_29ed288_16_8d.inc"
};Mapping sm{};
                            if(mapping(engineBase+0x29ed288u,16,sm)&&(sm.protection&PROT_EXEC)&&fingerprint(engineBase+0x29ed288u,fp))
                                gameConfigureSubtitles(engineBase+0x4e6ed0d8u,reinterpret_cast<void(*)(void*,bool)>(engineBase+0x29ed288u));
                            {std::uintptr_t q{};if(readMemory(nullptr,engineBase+0x2e10bb0u,&q,sizeof q)&&q==engineBase+0x4e85f560u)gameConfigureWaterQuery(q);} // g_WaterQuery (GOT 0x2e10bb0)
                        }
                        touchHooked.store(fingerprint(engineBase+touchGetViewportReturnOffset-4,touchGetViewportCall));
                        {
                            Mapping pc{};
                            if(mapping(engineBase+ptfxCullOffset,16,pc)&&(pc.protection&PROT_EXEC)&&fingerprint(engineBase+ptfxCullOffset,ptfxCullFingerprint)&&
                               fingerprint(engineBase+ptfxCullReturnOffset-4,ptfxCullCall)){
                                originalPtfxCull=reinterpret_cast<PtfxCullFn>(engineBase+ptfxCullOffset);
                                if(replacePointer(engineBase+ptfxCullGotOffset,engineBase+ptfxCullOffset,reinterpret_cast<std::uintptr_t>(ptfxCullEffect)))ptfxCullHooked.store(true);
                            }
                        }
                    }
                }
                else while(count){const auto& route=hudRoutes[--count];if(!replacePointer(engineBase+route.slot,route.replacement,engineBase+route.original))patchFault=true;}
            }
#endif
            inputBoundary.configure(reinterpret_cast<InputCallBoundary::Original>(engineBase+inputUpdateOffset),gameUpdateInput,gameAfterUpdateInput);
            inputInterposed=replacePointer(engineBase+inputUpdateGotOffset,engineBase+inputUpdateOffset,reinterpret_cast<std::uintptr_t>(updateInput));
        }
    }
#endif
// This optional read-only hook is independent: if it declines, the working
// theater remains available and the report explicitly says no scene observation.
#ifdef RDRVR_CAMERA_BRIDGE
    if(installed && fingerprint(engineBase+sceneOffset,sceneFingerprint) && fingerprint(engineBase+currentOffset,currentFingerprint)) {
        originalScene=reinterpret_cast<Scene>(engineBase+sceneOffset);
        originalCurrent=reinterpret_cast<Current>(engineBase+currentOffset);
#ifdef RDRVR_STEREO_BRIDGE
        const std::array<std::uintptr_t,6> addresses{viewportCopyOffset,viewportCameraOffset,viewportProjectionOffset,viewportUploadOffset,viewportOwnerOffset,viewportDestroyOffset};
        const std::array<std::array<unsigned char,32>,6> fingerprints{viewportCopyFingerprint,viewportCameraFingerprint,viewportProjectionFingerprint,viewportUploadFingerprint,viewportOwnerFingerprint,viewportDestroyFingerprint};
        Mapping currentMap{};
        bool valid=mapping(engineBase+currentViewportOffset,8,currentMap) && (currentMap.protection&(PROT_READ|PROT_WRITE))==(PROT_READ|PROT_WRITE);
        for(unsigned i=0;i<addresses.size();++i){std::array<unsigned char,32> bytes{};Mapping m{};
            const auto a=engineBase+addresses[i];valid=valid && mapping(a,32,m) && (m.protection&PROT_EXEC) && readMemory(nullptr,a,bytes.data(),bytes.size()) && bytes==fingerprints[i];}
        if(valid){ViewportApi api{};
            api.copy=reinterpret_cast<decltype(api.copy)>(engineBase+viewportCopyOffset);
            api.camera=reinterpret_cast<decltype(api.camera)>(engineBase+viewportCameraOffset);
            api.projection=reinterpret_cast<decltype(api.projection)>(engineBase+viewportProjectionOffset);
            api.current=originalCurrent;
            api.upload=reinterpret_cast<decltype(api.upload)>(engineBase+viewportUploadOffset);
            api.ownsThread=reinterpret_cast<decltype(api.ownsThread)>(engineBase+viewportOwnerOffset);
            api.destroy=reinterpret_cast<decltype(api.destroy)>(engineBase+viewportDestroyOffset);
            api.readCurrent=readSelectedViewport;
            cameraRecordingOwner.store(api.ownsThread,std::memory_order_release);
            gameConfigureViewports(api);
#ifdef RDRVR_HEAD_CAMERA
            Mapping phaseData{},phaseGot{};std::uintptr_t phasePointer{};
            const bool phaseValid=mapping(engineBase+replayPhaseIndexOffset,4,phaseData)&&
                (phaseData.protection&(PROT_READ|PROT_WRITE|PROT_EXEC))==(PROT_READ|PROT_WRITE)&&
                mapping(engineBase+replayPhaseIndexGotOffset,8,phaseGot)&&(phaseGot.protection&PROT_READ)&&
                readMemory(nullptr,engineBase+replayPhaseIndexGotOffset,&phasePointer,sizeof phasePointer)&&phasePointer==engineBase+replayPhaseIndexOffset&&
                fingerprint(engineBase+replayPhaseSceneOffset,replayPhaseSceneFingerprint)&&
                fingerprint(engineBase+replayPhaseDepthOffset,replayPhaseDepthFingerprint)&&
                fingerprint(engineBase+replayPhaseLightsOffset,replayPhaseLightsFingerprint)&&
                fingerprint(engineBase+replayPhaseColorOffset,replayPhaseColorFingerprint)&&
                fingerprint(engineBase+replayPhaseBucketOffset,replayPhaseBucketFingerprint)&&
                fingerprint(engineBase+replayPhaseCommonOffset,replayPhaseCommonFingerprint);
            if(phaseValid)gameConfigureReplayPhase({readMemory,writeReplayPhase,nullptr,api.ownsThread,
                engineBase+replayPhaseIndexOffset,engineBase+replayPhaseIndexGotOffset});
            if(fingerprint(engineBase+grassEndOffset,grassEndFingerprint)&&fingerprint(engineBase+grassDrawOffset,grassDrawFingerprint)&&
               fingerprint(engineBase+grassCallerOffset,grassCallerFingerprint)&&fingerprint(engineBase+grassBeginOffset,grassBeginFingerprint)&&
               fingerprint(engineBase+grassResetOffset,grassResetFingerprint)&&fingerprint(engineBase+grassVisibilityOffset,grassVisibilityFingerprint)&&
               fingerprint(engineBase+grassVisibilityParallelOffset,grassVisibilityParallelFingerprint)&&
               fingerprint(engineBase+grassEndTailOffset,grassEndTailFingerprint)&&
               fingerprint(engineBase+grassEndCallerReturnOffset-4,grassEndCallerCallFingerprint)){
                auto& grass=gameGrassVisibility();
                if(grass.configure(reinterpret_cast<ReplayGrassVisibility::Original>(engineBase+grassEndOffset)))
                    grass.installed(replacePointer(engineBase+grassEndGotOffset,engineBase+grassEndOffset,reinterpret_cast<std::uintptr_t>(retainGrassVisibility)));
            }
#endif
#ifdef RDRVR_PASS_RESOURCES
            Mapping globalCode{};bool globalsValid=mapping(engineBase+globalSetOffset,32,globalCode)&&
                (globalCode.protection&PROT_EXEC)&&fingerprint(engineBase+globalSetOffset,globalSetFingerprint);
            Mapping lodData{};
            if(mapping(engineBase+resourceTreeHighLodOffset,4,lodData)&&(lodData.protection&PROT_READ))
                gameConfigureLodDiagnostics(readMemory,nullptr,engineBase+resourceTreeHighLodOffset);
            const std::pair<std::uintptr_t,std::size_t> globalObjects[]{{globalValuesOffset,17288},{globalBuffersOffset,3848},{globalSizesOffset,16},{globalCommandOffset,8}};
            for(const auto& [offset,bytes]:globalObjects){Mapping m{};globalsValid=globalsValid&&mapping(engineBase+offset,bytes,m)&&(m.protection&PROT_READ);}
            if(globalsValid){EngineGlobalApi globals{};globals.read=readMemory;globals.ownsThread=api.ownsThread;
                globals.set=reinterpret_cast<decltype(globals.set)>(engineBase+globalSetOffset);
                globals.globals=engineBase+globalValuesOffset;globals.buffers=engineBase+globalBuffersOffset;
                globals.typeSizes=engineBase+globalSizesOffset;globals.commandBuffer=engineBase+globalCommandOffset;
                gameConfigureGlobals(globals);
            }
            Mapping nearMap{},farMap{};
            if(mapping(engineBase+viewportNearOffset,32,nearMap) && (nearMap.protection&PROT_EXEC) && fingerprint(engineBase+viewportNearOffset,viewportNearFingerprint) &&
               mapping(engineBase+viewportFarOffset,32,farMap) && (farMap.protection&PROT_EXEC) && fingerprint(engineBase+viewportFarOffset,viewportFarFingerprint)){
                CullViewportApi cull{api,reinterpret_cast<void(*)(void*,float)>(engineBase+viewportNearOffset),reinterpret_cast<void(*)(void*,float)>(engineBase+viewportFarOffset)};
                gameConfigureCullViewport(cull);
            }
            bool tablesValid=true;
            for(auto offset:{targetVtableOffset,textureVtableOffset,contextVtableOffset,factoryVtableOffset}) {
                Mapping m{};std::uintptr_t function{};Mapping code{};
                tablesValid=tablesValid && mapping(engineBase+offset,8,m) && (m.protection&PROT_READ) && !(m.protection&PROT_EXEC) &&
                    readMemory(nullptr,engineBase+offset,&function,8) && mapping(function,4,code) && (code.protection&PROT_EXEC);
            }
            if(tablesValid){
                PassSourceAbi memory{engineBase+targetVtableOffset,engineBase+textureVtableOffset,engineBase+contextVtableOffset,readMemory,nullptr};
                gameConfigurePassSource(memory);
                const std::array<std::uintptr_t,4> offsets{targetParamsOffset,targetCreateOffset,targetDecrementOffset,targetDeleteOffset};
                const std::array<std::array<unsigned char,32>,4> expected{targetParamsFingerprint,targetCreateFingerprint,targetDecrementFingerprint,targetDeleteFingerprint};
                bool targetValid=true;
                for(unsigned i=0;i<offsets.size();++i){Mapping m{};const auto a=engineBase+offsets[i];targetValid=targetValid && mapping(a,32,m) && (m.protection&PROT_EXEC) && fingerprint(a,expected[i]);}
                std::uintptr_t deleting{},creating{};
                targetValid=targetValid && readMemory(nullptr,engineBase+targetVtableOffset+8,&deleting,8) && deleting==engineBase+targetDeleteOffset &&
                    readMemory(nullptr,engineBase+factoryVtableOffset+0x90,&creating,8) && creating==engineBase+targetCreateOffset;
                if(targetValid){EngineTargetApi targets{};
                    targets.params=reinterpret_cast<decltype(targets.params)>(engineBase+targetParamsOffset);
                    targets.create=reinterpret_cast<decltype(targets.create)>(engineBase+targetCreateOffset);
                    targets.decrement=reinterpret_cast<decltype(targets.decrement)>(engineBase+targetDecrementOffset);
                    targets.deleteTarget=reinterpret_cast<decltype(targets.deleteTarget)>(engineBase+targetDeleteOffset);
                    targets.ownsThread=api.ownsThread;targets.factoryVtable=engineBase+factoryVtableOffset;targets.memory=memory;
                    gameConfigureEngineTargets(targets);
                    const std::array<std::uintptr_t,10> fbOffsets{framebufferDescriptorOffset,framebufferDescriptorDestroyOffset,framebufferCreateOffset,framebufferReleaseOffset,framebufferIncrementOffset,framebufferBindOffset,framebufferBindVkOffset,framebufferFlushOffset,rasterViewportOffset,rasterScissorOffset};
                    const std::array<std::array<unsigned char,32>,10> fbPrints{framebufferDescriptorFingerprint,framebufferDescriptorDestroyFingerprint,framebufferCreateFingerprint,framebufferReleaseFingerprint,framebufferIncrementFingerprint,framebufferBindFingerprint,framebufferBindVkFingerprint,framebufferFlushFingerprint,rasterViewportFingerprint,rasterScissorFingerprint};
                    bool fbValid=true;
                    for(unsigned i=0;i<fbOffsets.size();++i){Mapping m{};const auto a=engineBase+fbOffsets[i];fbValid=fbValid && mapping(a,32,m) && (m.protection&PROT_EXEC) && fingerprint(a,fbPrints[i]);}
                    for(auto offset:{framebufferVtableOffset,gfxVtableOffset,gfxDeviceVtableOffset,colorSurfaceVtableOffset,depthSurfaceVtableOffset}){
                        Mapping m{},code{};std::uintptr_t function{};fbValid=fbValid && mapping(engineBase+offset,8,m) && (m.protection&PROT_READ) && !(m.protection&PROT_EXEC) && readMemory(nullptr,engineBase+offset,&function,8) && mapping(function,4,code) && (code.protection&PROT_EXEC);
                    }
                    std::uintptr_t createFb{},bindFb{};Mapping dirtyMap{};
                    fbValid=fbValid && readMemory(nullptr,engineBase+gfxDeviceVtableOffset+0x28,&createFb,8) && createFb==engineBase+framebufferCreateOffset &&
                        readMemory(nullptr,engineBase+contextVtableOffset+0x178,&bindFb,8) && bindFb==engineBase+framebufferBindVkOffset &&
                        mapping(engineBase+factoryDirtyOffset,1,dirtyMap) && (dirtyMap.protection&(PROT_READ|PROT_WRITE))==(PROT_READ|PROT_WRITE);
                    if(fbValid){EngineFramebufferApi fb{};
                        fb.descriptor=reinterpret_cast<decltype(fb.descriptor)>(engineBase+framebufferDescriptorOffset);
                        fb.destroyDescriptor=reinterpret_cast<decltype(fb.destroyDescriptor)>(engineBase+framebufferDescriptorDestroyOffset);
                        fb.create=reinterpret_cast<decltype(fb.create)>(engineBase+framebufferCreateOffset);
                        fb.release=reinterpret_cast<decltype(fb.release)>(engineBase+framebufferReleaseOffset);
                        fb.increment=reinterpret_cast<decltype(fb.increment)>(engineBase+framebufferIncrementOffset);
                        fb.bind=reinterpret_cast<decltype(fb.bind)>(engineBase+framebufferBindOffset);
                        fb.bindVk=reinterpret_cast<decltype(fb.bindVk)>(engineBase+framebufferBindVkOffset);
                        fb.flush=reinterpret_cast<decltype(fb.flush)>(engineBase+framebufferFlushOffset);
                        fb.viewport=reinterpret_cast<decltype(fb.viewport)>(engineBase+rasterViewportOffset);
                        fb.scissor=reinterpret_cast<decltype(fb.scissor)>(engineBase+rasterScissorOffset);
                        fb.ownsThread=api.ownsThread;fb.memory=memory;fb.deviceVtable=engineBase+gfxDeviceVtableOffset;
                        fb.gfxVtable=engineBase+gfxVtableOffset;fb.framebufferVtable=engineBase+framebufferVtableOffset;
                        fb.colorVtable=engineBase+colorSurfaceVtableOffset;fb.depthVtable=engineBase+depthSurfaceVtableOffset;fb.factoryDirty=engineBase+factoryDirtyOffset;
                        gameConfigureFramebuffers(fb);
                        RetailResourceApi resources{};resources.memory=memory;resources.ownsThread=api.ownsThread;resources.currentGfx=currentEngineGfx;
                        resources.factory=engineBase+resourceFactoryOffset;resources.device=engineBase+resourceDeviceOffset;
                        resources.collector=engineBase+resourceCollectorOffset;resources.postfx=engineBase+resourcePostfxOffset;
                        resources.lights=engineBase+resourceLightsOffset;resources.timer=engineBase+resourceTimerOffset;
                        resources.factoryVtable=targets.factoryVtable;resources.deviceVtable=fb.deviceVtable;resources.gfxVtable=fb.gfxVtable;resources.framebufferVtable=fb.framebufferVtable;
                        gameConfigureRetailResources(resources);

                        Mapping lockMap{},unlockMap{},resolveMap{};std::uintptr_t lockEntry{},unlockEntry{};
                        const auto lockAddress=engineBase+factoryLockOffset,unlockAddress=engineBase+factoryUnlockOffset;
                        const auto resolveAddress=engineBase+sceneResolveOffset;
                        const auto lockSlot=engineBase+factoryVtableOffset+0x98,unlockSlot=engineBase+factoryVtableOffset+0xa0;
                        if(mapping(lockAddress,32,lockMap) && (lockMap.protection&PROT_EXEC) && fingerprint(lockAddress,factoryLockFingerprint) &&
                           mapping(unlockAddress,32,unlockMap) && (unlockMap.protection&PROT_EXEC) && fingerprint(unlockAddress,factoryUnlockFingerprint) &&
                           mapping(resolveAddress,32,resolveMap) && (resolveMap.protection&PROT_EXEC) && fingerprint(resolveAddress,sceneResolveFingerprint) &&
                           readMemory(nullptr,lockSlot,&lockEntry,8) && lockEntry==lockAddress &&
                           readMemory(nullptr,unlockSlot,&unlockEntry,8) && unlockEntry==unlockAddress){
                            EngineTargetBindingApi bindings{};bindings.lock=reinterpret_cast<decltype(bindings.lock)>(lockAddress);
                            bindings.unlock=reinterpret_cast<decltype(bindings.unlock)>(unlockAddress);
                            bindings.resolveDepth=reinterpret_cast<decltype(bindings.resolveDepth)>(resolveAddress);gameConfigureTargetBindings(bindings);
                            targetLockInterposed=replacePointer(lockSlot,lockAddress,reinterpret_cast<std::uintptr_t>(gameTargetLock));
                            if(targetLockInterposed){
                                targetUnlockInterposed=replacePointer(unlockSlot,unlockAddress,reinterpret_cast<std::uintptr_t>(gameTargetUnlock));
                                if(!targetUnlockInterposed){
                                    if(replacePointer(lockSlot,reinterpret_cast<std::uintptr_t>(gameTargetLock),lockAddress))targetLockInterposed=false;
                                    else patchFault=true;
                                }
                            }
                            if(targetLockInterposed && targetUnlockInterposed){
                                sceneResolveInterposed=replacePointer(engineBase+sceneResolveGotOffset,resolveAddress,reinterpret_cast<std::uintptr_t>(gameTargetResolveDepth));
                                if(!sceneResolveInterposed){
                                    if(replacePointer(unlockSlot,reinterpret_cast<std::uintptr_t>(gameTargetUnlock),unlockAddress))targetUnlockInterposed=false;else patchFault=true;
                                    if(replacePointer(lockSlot,reinterpret_cast<std::uintptr_t>(gameTargetLock),lockAddress))targetLockInterposed=false;else patchFault=true;
                                }
                            }
                            gameTargetHooksReady(targetLockInterposed && targetUnlockInterposed && sceneResolveInterposed);
                        }
                        Mapping copyMap{};const auto copyAddress=engineBase+depthCopyOffset;
                        if(mapping(copyAddress,32,copyMap) && (copyMap.protection&PROT_EXEC) && fingerprint(copyAddress,depthCopyFingerprint)){
                            EngineDepthCopyApi depth{};depth.copy=reinterpret_cast<decltype(depth.copy)>(copyAddress);
                            depth.ownsThread=api.ownsThread;depth.memory=memory;gameConfigureDepthCopy(depth);
                        }
                        Mapping globalMap{},instanceMap{};const auto globalAddress=engineBase+textureGlobalOffset,instanceAddress=engineBase+textureInstanceOffset;
                        if(mapping(globalAddress,32,globalMap) && (globalMap.protection&PROT_EXEC) && fingerprint(globalAddress,textureGlobalFingerprint) &&
                           mapping(instanceAddress,32,instanceMap) && (instanceMap.protection&PROT_EXEC) && fingerprint(instanceAddress,textureInstanceFingerprint)){
                            EngineTextureApi textures{};textures.global=reinterpret_cast<decltype(textures.global)>(globalAddress);
                            textures.instance=reinterpret_cast<decltype(textures.instance)>(instanceAddress);textures.ownsThread=api.ownsThread;
                            gameConfigureTextures(textures);
                            textureGlobalInterposed=replacePointer(engineBase+textureGlobalGotOffset,globalAddress,reinterpret_cast<std::uintptr_t>(gameTextureGlobal));
                            if(textureGlobalInterposed){
                                textureInstanceInterposed=replacePointer(engineBase+textureInstanceGotOffset,instanceAddress,reinterpret_cast<std::uintptr_t>(gameTextureInstance));
                                if(!textureInstanceInterposed){
                                    if(replacePointer(engineBase+textureGlobalGotOffset,reinterpret_cast<std::uintptr_t>(gameTextureGlobal),globalAddress))textureGlobalInterposed=false;
                                    else patchFault=true;
                                }
                            }
                            gameTextureHooksReady(textureGlobalInterposed && textureInstanceInterposed);
                        }
                    }
                }
            }
#endif
        }
        cameraBoundary.configure(originalScene,originalCurrent,captureCamera,gameSceneFinished);
#else
        cameraBoundary.configure(originalScene,originalCurrent,captureCamera);
#endif
        if(replacePointer(engineBase+currentGotOffset,reinterpret_cast<std::uintptr_t>(originalCurrent),reinterpret_cast<std::uintptr_t>(observeCurrent))) {
            sceneInterposed=replacePointer(engineBase+sceneGotOffset,reinterpret_cast<std::uintptr_t>(originalScene),reinterpret_cast<std::uintptr_t>(observeScene));
            if(!sceneInterposed && !replacePointer(engineBase+currentGotOffset,reinterpret_cast<std::uintptr_t>(observeCurrent),reinterpret_cast<std::uintptr_t>(originalCurrent)))patchFault=true;
#ifdef RDRVR_PASS_RESOURCES
            Mapping code{},marker{};
            if(sceneInterposed && mapping(engineBase+visibilityOffset,32,code) && (code.protection&PROT_EXEC) &&
               fingerprint(engineBase+visibilityOffset,visibilityFingerprint) && mapping(engineBase+visibilityMarkerOffset,4,marker) &&
               (marker.protection&(PROT_READ|PROT_WRITE))==(PROT_READ|PROT_WRITE)){
                originalVisibility=reinterpret_cast<VisibilityCallBoundary::Original>(engineBase+visibilityOffset);
                visibilityBoundary.configure(originalVisibility,beginVisibility,gameFinishVisibility);
                visibilityInterposed.store(replacePointer(engineBase+visibilityGotOffset,reinterpret_cast<std::uintptr_t>(originalVisibility),reinterpret_cast<std::uintptr_t>(observeVisibility)),std::memory_order_release);
                if(visibilityInterposed){
                    struct Route {std::uintptr_t slot,original,replacement;const std::array<unsigned char,32>* fingerprint;};
                    const Route routes[]{
                        {cullPrepareGotOffset,cullPrepareOffset,reinterpret_cast<std::uintptr_t>(prepareCull),&cullPrepareFingerprint},
                        {cullWorkerGotOffset,cullWorkerOffset,reinterpret_cast<std::uintptr_t>(workerCull),&cullWorkerFingerprint},
                        {cullGetterGotOffset,cullGetterOffset,reinterpret_cast<std::uintptr_t>(getterCull),&cullGetterFingerprint},
                        {cullSignalGotOffset,cullSignalOffset,reinterpret_cast<std::uintptr_t>(signalCull),&cullSignalFingerprint},
                        {cullCachedGotOffset,cullCachedOffset,reinterpret_cast<std::uintptr_t>(cachedCull),&cullCachedFingerprint}};
                    bool valid=true;
                    for(const auto& route:routes){Mapping map{};valid=valid&&mapping(engineBase+route.original,32,map)&&(map.protection&PROT_EXEC)&&fingerprint(engineBase+route.original,*route.fingerprint);}
                    if(valid){
                        cullBoundary.configure(reinterpret_cast<VisibilityCullBoundary::Prepare>(engineBase+cullPrepareOffset),
                            reinterpret_cast<VisibilityCullBoundary::Worker>(engineBase+cullWorkerOffset),
                            reinterpret_cast<VisibilityCullBoundary::Getter>(engineBase+cullGetterOffset),
                            reinterpret_cast<VisibilityCullBoundary::Signal>(engineBase+cullSignalOffset),
                            selectCull,gameBorrowCull,gameDrawFinished,readMemory);
                        cullBoundary.configureCached(reinterpret_cast<VisibilityCullBoundary::Cached>(engineBase+cullCachedOffset),gameQualifyVisibility,gameWorldVisibilityValid);
                        {   // Occluder quads; inert unless STEREO TEST occluderTestMode/occluderAlwaysMode.
                            const bool occluderCode=OccluderInjection::codeMatches(engineBase,[](std::uintptr_t p,const std::array<unsigned char,32>& print){
                                Mapping code{};return mapping(p,print.size(),code)&&(code.protection&PROT_EXEC)&&fingerprint(p,print);});
                            Mapping slots{};
                            const bool occluderSlots=mapping(engineBase+OccluderInjection::occShaftListOffset,OccluderInjection::shaftBytes,slots)&&
                                (slots.protection&(PROT_READ|PROT_WRITE))==(PROT_READ|PROT_WRITE);
                            if(occluderCode&&occluderSlots)occluderInjection.configure(engineBase,readMemory);
                            // The per-MAIN-frame callback also feeds the small-object culler's camera
                            // snapshot, independent of the occluder code fingerprints.
                            smallObjectCull.configure(readMemory);cullBoundary.configureAfterCached(occluderAfterCached);
                        }
                        gameConfigureWorldVisibility(ensureWorldOcclusion,worldOcclusionValid);
                        unsigned installedCull{};
                        for(const auto& route:routes){if(!replacePointer(engineBase+route.slot,engineBase+route.original,route.replacement))break;++installedCull;}
                        if(installedCull==std::size(routes)){cullInterposed=true;gameCullHooksReady(true);
#ifdef RDRVR_WORLD_PREVIEW
                            gameEnableWorldCulling(true);
#endif
                        }
                        else while(installedCull){const auto& route=routes[--installedCull];if(!replacePointer(engineBase+route.slot,route.replacement,engineBase+route.original))patchFault=true;}
                    }
                    Mapping profileCode{};
#ifdef RDRVR_HEAD_CAMERA
                    if(mapping(engineBase+resolutionUpdateOffset,32,profileCode)&&(profileCode.protection&PROT_EXEC)&&
                       fingerprint(engineBase+resolutionUpdateOffset,resolutionUpdateFingerprint)){
                        resolutionPolicy.configure(reinterpret_cast<NativeResolutionPolicy::Original>(engineBase+resolutionUpdateOffset),readMemory,writeResolutionFlag);
                        resolutionInterposed=replacePointer(engineBase+resolutionUpdateGotOffset,engineBase+resolutionUpdateOffset,reinterpret_cast<std::uintptr_t>(updateResolution));
                    }
#endif
                    if(mapping(engineBase+profileDrawOffset,32,profileCode)&&(profileCode.protection&PROT_EXEC)&&fingerprint(engineBase+profileDrawOffset,profileDrawFingerprint)){
                        gameWorldProfile().configure(reinterpret_cast<WorldDrawProfile::Original>(engineBase+profileDrawOffset),readMemory);
                        profileInterposed=replacePointer(engineBase+profileDrawGotOffset,engineBase+profileDrawOffset,reinterpret_cast<std::uintptr_t>(gameProfileDraw));
                        gameProfileHooksReady(profileInterposed);
                    }
                    const Route mainPasses[]{
                        {retailDepthGotOffset,retailDepthOffset,reinterpret_cast<std::uintptr_t>(retailDepth),&retailDepthFingerprint},
                        {retailLightsGotOffset,retailLightsOffset,reinterpret_cast<std::uintptr_t>(retailLights),&retailLightsFingerprint},
                        {retailColorGotOffset,retailColorOffset,reinterpret_cast<std::uintptr_t>(retailColor),&retailColorFingerprint}};
                    valid=true;
                    for(const auto& route:mainPasses){Mapping map{};valid=valid&&mapping(engineBase+route.original,32,map)&&(map.protection&PROT_EXEC)&&fingerprint(engineBase+route.original,*route.fingerprint);}
                    if(valid){
                        retailBoundary.configure(reinterpret_cast<RetailSceneBoundary::Draw>(engineBase+retailDepthOffset),
                            reinterpret_cast<RetailSceneBoundary::Lights>(engineBase+retailLightsOffset),
                            reinterpret_cast<RetailSceneBoundary::Draw>(engineBase+retailColorOffset),eyecull::stereoPassConsumer<gameDrawRetailWorld,RetailSceneFrame>); // EYE CULL books the stereo pass (consumer) apart
                        unsigned installedMain{};
                        for(const auto& route:mainPasses){if(!replacePointer(engineBase+route.slot,engineBase+route.original,route.replacement))break;++installedMain;}
                        if(installedMain==std::size(mainPasses)){
                            retailInterposed=true;
                            const std::uintptr_t auxOffsets[]{retailCollectorOffset,retailBeginColorOffset,retailSetupDepthOffset,retailDepthTargetsOffset,retailClearOffset};
                            const std::array<unsigned char,32> auxPrints[]{retailCollectorFingerprint,retailBeginColorFingerprint,retailSetupDepthFingerprint,retailDepthTargetsFingerprint,retailClearFingerprint};
                            bool auxValid=true;
                            for(unsigned i=0;i<std::size(auxOffsets);++i){Mapping m{};const auto a=engineBase+auxOffsets[i];auxValid=auxValid&&mapping(a,32,m)&&(m.protection&PROT_EXEC)&&fingerprint(a,auxPrints[i]);}
                            if(auxValid){RetailDrawApi draw{};
                                draw.depth=reinterpret_cast<decltype(draw.depth)>(engineBase+retailDepthOffset);draw.color=reinterpret_cast<decltype(draw.color)>(engineBase+retailColorOffset);
                                draw.lights=reinterpret_cast<decltype(draw.lights)>(engineBase+retailLightsOffset);draw.collector=reinterpret_cast<decltype(draw.collector)>(engineBase+retailCollectorOffset);
                                draw.beginColor=reinterpret_cast<decltype(draw.beginColor)>(engineBase+retailBeginColorOffset);draw.setupDepth=reinterpret_cast<decltype(draw.setupDepth)>(engineBase+retailSetupDepthOffset);
                                draw.depthTargets=reinterpret_cast<decltype(draw.depthTargets)>(engineBase+retailDepthTargetsOffset);draw.clear=reinterpret_cast<decltype(draw.clear)>(engineBase+retailClearOffset);
#ifdef RDRVR_HEAD_CAMERA
                                ablateStereoDepthOriginal=draw.depth;ablateStereoLightsOriginal=draw.lights;draw.depth=ablateStereoDepth;draw.lights=ablateStereoLights;
#endif
                                gameConfigureRetailDraw(draw);
#ifdef RDRVR_HEAD_CAMERA
                                bool primaryValid=true;
                                const auto checkPrimary=[&](std::uintptr_t offset,const auto& bytes){Mapping m{};return mapping(engineBase+offset,bytes.size(),m)&&(m.protection&PROT_EXEC)&&fingerprint(engineBase+offset,bytes);};
                                primaryValid=checkPrimary(primarySetupOffset,primarySetupFingerprint)&&checkPrimary(primaryShaftOffset,primaryShaftFingerprint)&&
                                    checkPrimary(primarySceneOffset,primarySceneFingerprint)&&checkPrimary(primarySetupSiteReturnOffset-4,primarySetupSiteCallFingerprint)&&
                                    checkPrimary(primaryShaftSiteReturnOffset-4,primaryShaftSiteCallFingerprint)&&primaryEyeProof(checkPrimary)&&
                                    checkPrimary(primaryDepthShaftSiteReturnOffset-4,primaryDepthShaftSiteCallFingerprint)&&
                                    checkPrimary(primaryWorldShaftSiteReturnOffset-4,primaryWorldShaftSiteCallFingerprint)&&
                                    primaryDepthShaftSiteGotOffset==primaryShaftSiteGotOffset&&primaryWorldShaftSiteGotOffset==primaryShaftSiteGotOffset;
                                if(primaryValid){
                                    nativePrimarySetup=reinterpret_cast<decltype(nativePrimarySetup)>(engineBase+primarySetupOffset);
                                    nativePrimaryShaft=reinterpret_cast<decltype(nativePrimaryShaft)>(engineBase+primaryShaftOffset);
                                    const bool setupInstalled=replacePointer(engineBase+primarySetupSiteGotOffset,engineBase+primarySetupOffset,reinterpret_cast<std::uintptr_t>(primarySetup));
                                    const bool shaftInstalled=setupInstalled&&replacePointer(engineBase+primaryShaftSiteGotOffset,engineBase+primaryShaftOffset,reinterpret_cast<std::uintptr_t>(primaryShaft));
                                    if(shaftInstalled){retailBoundary.configureRoutes(gamePrimaryDepth,gamePrimaryLights,gamePrimaryColor);gamePrimaryHooksReady(true);}
                                    else if(setupInstalled&&!replacePointer(engineBase+primarySetupSiteGotOffset,reinterpret_cast<std::uintptr_t>(primarySetup),engineBase+primarySetupOffset))patchFault=true;
                                }
#endif
                            }
                        }
                        else while(installedMain){const auto& route=mainPasses[--installedMain];if(!replacePointer(engineBase+route.slot,route.replacement,engineBase+route.original))patchFault=true;}
                    }
                    const Route updates[]{
                        {replayBlendGotOffset,replayBlendOffset,reinterpret_cast<std::uintptr_t>(gameReplayBlend),&replayBlendFingerprint},
                        {replayIndoorGotOffset,replayIndoorOffset,reinterpret_cast<std::uintptr_t>(gameReplayIndoor),&replayIndoorFingerprint},
                        {replayCharacterGotOffset,replayCharacterOffset,reinterpret_cast<std::uintptr_t>(gameReplayCharacter),&replayCharacterFingerprint},
                        {replayCloudGotOffset,replayCloudOffset,reinterpret_cast<std::uintptr_t>(gameReplayCloud),&replayCloudFingerprint},
                        {replayTreeTimerBeginGotOffset,replayTreeTimerBeginOffset,reinterpret_cast<std::uintptr_t>(gameReplayTreeTimerBegin),&replayTreeTimerBeginFingerprint},
                        {replayTreeBuffersGotOffset,replayTreeBuffersOffset,reinterpret_cast<std::uintptr_t>(rdrvrTreeTimerBuffers),&replayTreeBuffersFingerprint}};
                    valid=true;
                    for(const auto& route:updates){Mapping map{};valid=valid&&mapping(engineBase+route.original,32,map)&&(map.protection&PROT_EXEC)&&fingerprint(engineBase+route.original,*route.fingerprint);}
                    valid=valid&&fingerprint(engineBase+treeDrawTimerReturnOffset-4,treeDrawTimerFingerprint)&&
                        fingerprint(engineBase+treeResolveTimerReturnOffset-4,treeResolveTimerFingerprint);
                    if(valid){
                        gameReplayUpdates().configure({reinterpret_cast<ReplayUpdates::Original>(engineBase+replayBlendOffset),
                            reinterpret_cast<ReplayUpdates::Original>(engineBase+replayIndoorOffset),reinterpret_cast<ReplayUpdates::Original>(engineBase+replayCharacterOffset)});
                        gameReplayUpdates().configureCloud(reinterpret_cast<ReplayUpdates::Cloud>(engineBase+replayCloudOffset));
                        gameReplayUpdates().configureTreeTimer(reinterpret_cast<ReplayUpdates::TreeTimer>(engineBase+replayTreeTimerBeginOffset));
                        treeTimerRouting.configure(reinterpret_cast<TreeTimerRouting::Count>(engineBase+replayTreeBuffersOffset),gameReplayTreeTimerEnd,
                            {{{engineBase+treeDrawTimerReturnOffset,engineBase+treeDrawTimerResumeOffset},
                              {engineBase+treeResolveTimerReturnOffset,engineBase+treeResolveTimerResumeOffset}}});
                        unsigned installedReplay{};
                        for(const auto& route:updates){if(!replacePointer(engineBase+route.slot,engineBase+route.original,route.replacement))break;++installedReplay;}
                        if(installedReplay==std::size(updates)){replayInterposed=true;gameReplayHooksReady(true);}
                        else while(installedReplay){const auto& route=updates[--installedReplay];if(!replacePointer(engineBase+route.slot,route.replacement,engineBase+route.original))patchFault=true;}
                    }
#ifdef RDRVR_HEAD_CAMERA
                    const auto checkedEyeImage=[&](std::uintptr_t offset,const auto& bytes){
                        const int required=offset==framebufferDisplayDepthNameOffset?PROT_READ:PROT_EXEC;
                        Mapping data{};return mapping(engineBase+offset,bytes.size(),data)&&(data.protection&required)&&fingerprint(engineBase+offset,bytes);
                    };
                    if(graphicsDrawProof(checkedEyeImage)){
                        originalGraphicsSun=reinterpret_cast<GraphicsSun>(engineBase+graphicsSunOffset);
                        originalGraphicsGrass=reinterpret_cast<GraphicsGrass>(engineBase+graphicsGrassOffset);
                        struct GraphicsRoute{std::uintptr_t slot,original,replacement;};
                        const GraphicsRoute routes[]{
                            {graphicsSunGotOffset,graphicsSunOffset,reinterpret_cast<std::uintptr_t>(graphicsSun)},
                            {graphicsGrassGotOffset,graphicsGrassOffset,reinterpret_cast<std::uintptr_t>(graphicsGrass)}};
                        unsigned count{};for(const auto& route:routes){if(!replacePointer(engineBase+route.slot,engineBase+route.original,route.replacement))break;++count;}
                        if(count==std::size(routes)){graphicsDrawPolicy.installed(true);}
                        else while(count){const auto& route=routes[--count];if(!replacePointer(engineBase+route.slot,route.replacement,engineBase+route.original))patchFault=true;}
                    }
                    { // SHADOW QUALITY (receiver uniforms), SHADOW MAP (CShadowMap::CreateRenderTargets), GRASS DISTANCE (grassManager::Update)
                        namespace sg=shadowgrass;
                        const auto code=[&](std::uintptr_t offset,const auto& bytes,int protection){
                            Mapping m{};return mapping(engineBase+offset,bytes.size(),m)&&(m.protection&protection)&&fingerprint(engineBase+offset,bytes);};
                        const auto got=[&](std::uintptr_t slot,std::uintptr_t target){
                            std::uintptr_t value{};return readMemory(nullptr,engineBase+slot,&value,sizeof value)&&value==engineBase+target;};
                        const auto startup=runtime::graphicsSettings.snapshot().requested;
                        sg::state.grassLevel.store(startup.grassDistance);shadowupdate::stats.level.store(startup.shadowUpdate); // qualityLevel stays 0 (inert)
                        shadowupdate::stats.cullLevel.store(startup.cascadeCull);
                        sg::state.mapRequestedLevel.store(startup.shadowMap);runtime::shadowMapStartupLevel.store(int(startup.shadowMap));
                        sg::config.lightsInstance=engineBase+sg::lightsInstanceOffset;sg::config.graphicsOptions=engineBase+sg::graphicsOptionsOffset;
                        sg::config.pcSizeX=engineBase+sg::pcSizeXOffset;sg::config.pcSizeY=engineBase+sg::pcSizeYOffset;
                        sg::config.grassInstance=engineBase+sg::grassInstanceOffset;sg::config.grassExtra=engineBase+sg::grassExtraOffset;
                        // Receiver: the hooked RenderDirectionalShadow (graphicsDrawProof) calls SetupConstants, which loads these fields.
                        sg::config.filter=graphicsDrawPolicy.ready()&&got(sg::lightsInstanceGotOffset,sg::lightsInstanceOffset)&&
                            code(sg::lightConstantsFilterOffset,sg::lightConstantsFilterFingerprint,PROT_EXEC)&&code(sg::lightConstantsSamplesOffset,sg::lightConstantsSamplesFingerprint,PROT_EXEC)&&
                            code(sg::lightConstantsSoftnessOffset,sg::lightConstantsSoftnessFingerprint,PROT_EXEC)&&code(sg::lightConstantsBlendOffset,sg::lightConstantsBlendFingerprint,PROT_EXEC);
                        sg::config.grassPairs=graphicsDrawPolicy.ready()&&got(0x2de6948,sg::grassInstanceOffset)&& // GOT atSingleton<grassManager>::sm_Instance
                            code(sg::grassDrawPairsOffset,sg::grassDrawPairsFingerprint,PROT_EXEC);
                        if(code(sg::shadowMapCreateOffset,sg::shadowMapCreateFingerprint,PROT_EXEC)&&code(sg::shadowMapTableOffset,sg::shadowMapTableFingerprint,PROT_READ)&&
                           got(sg::graphicsOptionsGotOffset,sg::graphicsOptionsOffset)&&got(sg::pcSizeXGotOffset,sg::pcSizeXOffset)&&got(sg::pcSizeYGotOffset,sg::pcSizeYOffset)){
                            sg::originalShadowMapCreate=reinterpret_cast<sg::ShadowMapCreate>(engineBase+sg::shadowMapCreateOffset);
                            sg::config.map=true; // before the slot goes live: the hook reads config
                            if(replacePointer(engineBase+sg::shadowMapCreateGotOffset,engineBase+sg::shadowMapCreateOffset,reinterpret_cast<std::uintptr_t>(&sg::shadowMapCreateHook)))
                                sg::state.mapHook.store(true);
                        }
                        if(code(sg::grassUpdateOffset,sg::grassUpdateFingerprint,PROT_EXEC)&&code(sg::grassUpdateBaseOffset,sg::grassUpdateBaseFingerprint,PROT_READ)){
                            sg::originalGrassUpdate=reinterpret_cast<sg::GrassUpdate>(engineBase+sg::grassUpdateOffset);
                            sg::config.grass=true;
                            if(replacePointer(engineBase+sg::grassUpdateGotOffset,engineBase+sg::grassUpdateOffset,reinterpret_cast<std::uintptr_t>(&sg::grassUpdateHook)))
                                sg::state.grassHook.store(true);
                        }
                    }
                    { // SHADOW UPDATE (RenderIntoShadowMaps skip + BeginTileAroundSphere snapshot + sphere restore before both uploads)
                        namespace su=shadowupdate;
                        const auto code=[&](std::uintptr_t offset,const auto& bytes){
                            Mapping m{};return mapping(engineBase+offset,bytes.size(),m)&&(m.protection&PROT_EXEC)&&fingerprint(engineBase+offset,bytes);};
                        std::uintptr_t current{};
                        const bool valid=readMemory(nullptr,engineBase+su::currentViewportGotOffset,&current,sizeof current)&&current==engineBase+su::currentViewportOffset&&
                            code(su::renderIntoOffset,su::renderIntoFingerprint)&&code(su::beginTileOffset,su::beginTileFingerprint)&&
                            code(su::collectorOffset,su::collectorFingerprint)&&code(su::constantsOffset,su::constantsFingerprint)&&
                            code(su::renderIntoReturnOffset-4,su::renderIntoCallFingerprint)&&code(su::beginTileReturnOffset-4,su::beginTileCallFingerprint)&&
                            code(su::collectorReturnOffset-4,su::collectorCallFingerprint)&&code(su::constantsReturnOffset-4,su::constantsCallFingerprint);
                        if(valid){
                            su::config={engineBase+su::shadowMapVtableOffset,engineBase+su::currentViewportOffset,engineBase+su::renderIntoReturnOffset,
                                engineBase+su::beginTileReturnOffset,engineBase+su::collectorReturnOffset,engineBase+su::constantsReturnOffset};
                            su::originalRenderInto=reinterpret_cast<su::RenderInto>(engineBase+su::renderIntoOffset);
                            su::originalBeginTile=reinterpret_cast<su::BeginTile>(engineBase+su::beginTileOffset);
                            su::originalCollector=reinterpret_cast<su::Collector>(engineBase+su::collectorOffset);
                            su::originalConstants=reinterpret_cast<su::Constants>(engineBase+su::constantsOffset);
                            struct ShadowRoute{std::uintptr_t slot,original,replacement;};
                            // RenderIntoShadowMaps goes live last: no frame is skipped before snapshot and restore are installed.
                            const ShadowRoute routes[]{
                                {su::beginTileGotOffset,su::beginTileOffset,reinterpret_cast<std::uintptr_t>(shadowBeginTile)},
                                {su::collectorGotOffset,su::collectorOffset,reinterpret_cast<std::uintptr_t>(shadowCollector)},
                                {su::constantsGotOffset,su::constantsOffset,reinterpret_cast<std::uintptr_t>(shadowConstants)},
                                {su::renderIntoGotOffset,su::renderIntoOffset,reinterpret_cast<std::uintptr_t>(shadowRenderInto)}};
                            unsigned count{};for(const auto& route:routes){if(!replacePointer(engineBase+route.slot,engineBase+route.original,route.replacement))break;++count;}
                            if(count==std::size(routes))su::stats.hooked.store(true);
                            else while(count){const auto& route=routes[--count];if(!replacePointer(engineBase+route.slot,route.replacement,engineBase+route.original))patchFault=true;}
                        }
                        runtime::shadowUpdateReady.store(su::stats.hooked.load()?1:0,std::memory_order_relaxed);
                        // Indoor signal (env byte + rooms provenance) and CASCADE CULL (CS2/CS3) fingerprints
                        indoor::configure(engineBase,code,readMemory);su::configureCascadeCull(engineBase,code,readMemory);
                        runtime::cascadeCullReady.store(su::cascadeCullConfigured()?1:0,std::memory_order_relaxed);
                        { // SHADOW UPDATE 5 STAGGER / 6 STAGGER FAR (SunStagger.h). Code, vtable/GOT/proxy slots, the two
                          // engine bytes and the hooks are proven first; GOT grcDevice::Clear, then the RenderShadowCasters
                          // vtable slot (the Clear slot is rolled back if the second swap fails). Inert until a stagger level runs.
                            namespace ss=su::stagger;
                            if(ss::configure(engineBase,code,readMemory)){
                                ss::originalClear=reinterpret_cast<ss::DeviceClear>(engineBase+ss::clearOffset);
                                ss::originalCasters=reinterpret_cast<ss::Casters>(engineBase+ss::castersOffset);
                                const bool clearLive=replacePointer(engineBase+ss::clearGotOffset,engineBase+ss::clearOffset,reinterpret_cast<std::uintptr_t>(shadowDeviceClear));
                                const bool castersLive=clearLive&&replacePointer(engineBase+ss::castersSlotOffset,engineBase+ss::castersOffset,reinterpret_cast<std::uintptr_t>(shadowCasters));
                                if(clearLive&&!castersLive&&!replacePointer(engineBase+ss::clearGotOffset,reinterpret_cast<std::uintptr_t>(shadowDeviceClear),engineBase+ss::clearOffset))patchFault=true;
                                ss::installResult.store(castersLive?"ok":clearLive?"castersSlot":"clearGot",std::memory_order_relaxed);
                                ss::installed.store(castersLive,std::memory_order_release);
                            }
                            runtime::sunStaggerReady.store(ss::menuState(),std::memory_order_relaxed);
                        }
                    }
                    { // LAMP SHADOWS (LampShadows.h). Code, call sites, GOT slots and globals are proven first; DrawShadow and
                      // RenderIntoCubeShadowMap go live before DrawCubeShadowMaps, and nothing is skipped before `hooked` (set last).
                        namespace ls=lampshadow;
                        const auto code=[&](std::uintptr_t offset,const auto& bytes){
                            Mapping m{};return mapping(engineBase+offset,bytes.size(),m)&&(m.protection&PROT_EXEC)&&fingerprint(engineBase+offset,bytes);};
                        if(ls::configure(engineBase,code,readMemory)){
                            ls::originalDrawCube=reinterpret_cast<ls::DrawCube>(engineBase+ls::drawCubeOffset);
                            ls::originalRenderInto=reinterpret_cast<ls::RenderInto>(engineBase+ls::renderIntoOffset);
                            ls::originalDrawShadow=reinterpret_cast<ls::DrawShadow>(engineBase+ls::drawShadowOffset);
                            struct LampRoute{std::uintptr_t slot,original,replacement;};
                            const LampRoute routes[]{
                                {ls::drawShadowGotOffset,ls::drawShadowOffset,reinterpret_cast<std::uintptr_t>(lampDrawShadow)},
                                {ls::renderIntoGotOffset,ls::renderIntoOffset,reinterpret_cast<std::uintptr_t>(lampRenderInto)},
                                {ls::drawCubeGotOffset,ls::drawCubeOffset,reinterpret_cast<std::uintptr_t>(lampDrawCube)}};
                            unsigned count{};for(const auto& route:routes){if(!replacePointer(engineBase+route.slot,engineBase+route.original,route.replacement))break;++count;}
                            if(count==std::size(routes))ls::hooked.store(true);
                            else while(count){const auto& route=routes[--count];if(!replacePointer(engineBase+route.slot,route.replacement,engineBase+route.original))patchFault=true;}
                        }
                        runtime::lampShadowsReady.store(ls::hooked.load()?1:0,std::memory_order_relaxed);
                    }
                    { // CREATURE LOD (CreatureLod.h): FNV prints of DrawBig / DrawN / PostUpdate / GetCreature + call words + GOT words,
                      // then GOT DrawN, DrawBig, PostUpdate all-or-nothing; debug.rdrvr.lod.creature.hooks 0 = nothing hooked. The VR BODY's
                      // skeleton (BODY published / collapse) is never switched.
                        creaturelod::Api api{};api.base=engineBase;
                        api.read=[](std::uintptr_t a,void* o,std::size_t n){return readMemory(nullptr,a,o,n);};
                        api.exec=[](std::uintptr_t a,std::size_t n){Mapping m{};return mapping(a,n,m)&&(m.protection&PROT_EXEC)!=0;};
                        api.replace=[](std::uintptr_t a,std::uintptr_t e,std::uintptr_t r){return replacePointer(a,e,r);};
                        api.playerSkeleton=[]()->std::uintptr_t{namespace vb=::rdrvr::vr::bodyik;vb::Published p{};
                            return vb::published.load(p)&&p.skel?p.skel:vb::collapseSkeleton.load(std::memory_order_relaxed);};
                        api.playerCreature=[]()->std::uintptr_t{return shadowupdate::stagger::playerCreature.load(std::memory_order_relaxed);};
                        if(creaturelod::install(api)<0)patchFault=true;
                    }
                    { // STEREO TEST 16 SKIP POSTFX (needs the d11 draw wrappers of nativeDrawSuppression)
                        namespace pf=postfx;
                        const auto code=[&](std::uintptr_t offset,const auto& bytes){
                            Mapping m{};return mapping(engineBase+offset,bytes.size(),m)&&(m.protection&PROT_EXEC)&&fingerprint(engineBase+offset,bytes);};
                        bool valid=nativeDrawSuppression.ready()&&code(pf::bindInputLayoutOffset,pf::bindInputLayoutFingerprint);
                        for(const auto& route:pf::routes)valid=valid&&code(route.original,route.code)&&code(route.ret-4,route.call);
                        if(valid){
                            nativeBindInputLayout=reinterpret_cast<bool(*)(void*)>(engineBase+pf::bindInputLayoutOffset);
                            for(unsigned i=0;i<pf::SiteCount;++i)postOriginal[i]=engineBase+pf::routes[i].original;
                            const std::uintptr_t wrappers[pf::SiteCount]{reinterpret_cast<std::uintptr_t>(&postSite<pf::LowresParticles>),
                                reinterpret_cast<std::uintptr_t>(&postSiteFlag<pf::SunOptics>),reinterpret_cast<std::uintptr_t>(&postSite<pf::PppProcess>),
                                reinterpret_cast<std::uintptr_t>(&postSiteFlag<pf::PostFxProcess>),reinterpret_cast<std::uintptr_t>(&postSite<pf::PppDraw>)};
                            unsigned count{};
                            while(count<pf::SiteCount&&replacePointer(engineBase+pf::routes[count].got,engineBase+pf::routes[count].original,wrappers[count]))++count;
                            if(count==pf::SiteCount)pf::ready.store(true);
                            else while(count){--count;if(!replacePointer(engineBase+pf::routes[count].got,wrappers[count],engineBase+pf::routes[count].original))patchFault=true;}
                        }
                    }
                    if(postfx::ready.load()){ // POST FX OFF FAST. Any failed check leaves deep NOT_INSTALLED (OFF FAST = bind-only).
                        namespace dp=postfx::deep;
                        const auto code=[&](std::uintptr_t offset,const auto& bytes){
                            Mapping m{};return mapping(engineBase+offset,bytes.size(),m)&&(m.protection&PROT_EXEC)&&fingerprint(engineBase+offset,bytes);};
                        const auto deepUnlockSlot=engineBase+factoryVtableOffset+dp::factoryUnlockBackBufferSlot;
                        std::uintptr_t rdr2Entry{},unlockEntry{};
                        const bool deepValid=targetLockInterposed.load()&&targetUnlockInterposed.load()&&
                            code(dp::rdr2,dp::rdr2Fingerprint)&&code(dp::thunk,dp::thunkFingerprint)&&code(dp::callerCall,dp::callerCallWord)&&
                            code(dp::processNone,dp::processNoneFingerprint)&&code(dp::unlockBackBuffer,dp::unlockBackBufferFingerprint)&&
                            readMemory(nullptr,engineBase+dp::gotRdr2,&rdr2Entry,sizeof rdr2Entry)&&rdr2Entry==engineBase+dp::rdr2&&
                            readMemory(nullptr,deepUnlockSlot,&unlockEntry,sizeof unlockEntry)&&unlockEntry==engineBase+dp::unlockBackBuffer&&
                            dp::configure(engineBase,[](std::uintptr_t a,void* o,std::size_t n){return readMemory(nullptr,a,o,n);},
                                reinterpret_cast<std::uintptr_t>(&gameTargetLock),code(dp::storeMotionBlur,dp::storeMotionBlurFingerprint));
                        if(deepValid){
                            postRdr2Original=engineBase+dp::rdr2;postUnlockBackBufferOriginal=engineBase+dp::unlockBackBuffer;
                            if(replacePointer(deepUnlockSlot,engineBase+dp::unlockBackBuffer,reinterpret_cast<std::uintptr_t>(&postUnlockBackBuffer))){
                                dp::unlockTap.store(true);
                                if(replacePointer(engineBase+dp::gotRdr2,engineBase+dp::rdr2,reinterpret_cast<std::uintptr_t>(&postRdr2)))dp::installed.store(true);
                                else if(replacePointer(deepUnlockSlot,reinterpret_cast<std::uintptr_t>(&postUnlockBackBuffer),engineBase+dp::unlockBackBuffer))dp::unlockTap.store(false);
                                else patchFault=true;
                            }
                        }
                    }
#ifdef RDRVR_PX2D
                    if(postfx::ready.load()){ // PX 2D (debug.rdrvr.px2d): MoviePlayer::DrawUpdate span close (bit 1, ClearAlpha) + grPostFX::Apply360Gamma
                      // (bit 2). Each route is proven first (fn print, call word at ret-4, *GOT == fn); a failed check leaves that part native
                      // (graphics.px2d.movieFingerprint / gammaFingerprint; without the DrawUpdate close the HUD span ends at TouchUI::Render).
                        namespace p2=px2d;
                        const auto code=[&](std::uintptr_t offset,const auto& bytes){
                            Mapping m{};return mapping(engineBase+offset,bytes.size(),m)&&(m.protection&PROT_EXEC)&&fingerprint(engineBase+offset,bytes);};
                        const auto got=[&](std::uintptr_t slot,std::uintptr_t target){
                            std::uintptr_t v{};return readMemory(nullptr,engineBase+slot,&v,sizeof v)&&v==engineBase+target;};
                        const char* fp=!code(p2::movieOffset,p2::movieFingerprint)?"code":!code(p2::movieReturnOffset-4,p2::movieCall)?"call":
                            !got(p2::movieGotOffset,p2::movieOffset)?"got":"ok";
                        if(fp[0]=='o'){
                            px2dMovieOriginal=engineBase+p2::movieOffset; // before the slot goes live
                            if(replacePointer(engineBase+p2::movieGotOffset,engineBase+p2::movieOffset,reinterpret_cast<std::uintptr_t>(&px2dMovieUpdate)))p2::movieHooked.store(true);
                            else fp="patch";
                        }
                        p2::movieFp.store(fp);
                        fp=!code(p2::gammaOffset,p2::gammaFingerprint)?"code":!code(p2::gammaReturnOffset-4,p2::gammaCall)?"call":!got(p2::gammaGotOffset,p2::gammaOffset)?"got":"ok";
                        if(fp[0]=='o'){
                            px2dGammaOriginal=engineBase+p2::gammaOffset; // before the slot goes live
                            if(replacePointer(engineBase+p2::gammaGotOffset,engineBase+p2::gammaOffset,reinterpret_cast<std::uintptr_t>(&px2dGamma)))p2::gammaHooked.store(true);
                            else fp="patch";
                        }
                        p2::gammaFp.store(fp);
                    }else{px2d::movieFp.store("postFxNotReady");px2d::gammaFp.store("postFxNotReady");}
#endif
                    { // Engine sides of the levers (S02Levers.h). Every JUMP_SLOT route is proven first (fn
                      // fingerprint, call word at ret-4, *GOT == fn); a failed check leaves that site native and the lever's installed
                      // bit clear (its effective bit is then cleared with disabledReason "notInstalled"). The wrappers only act
                      // while the requested mask (debug.rdrvr.s02, else graphics.cfg s02Mask, default 0x1F) or
                      // debug.rdrvr.s02ab asks for the lever; debug.rdrvr.s02 0 = native; s02Mask=0x0 = native
                      // except LR (cfgRequest adds LR to every cfg mask; EYE CULL never joins a zero cfg mask).
                      // asks for the lever; debug.rdrvr.s02 0 = native; s02Mask=0x0 = native except LR (forces LR into every
                      // cfg mask; EYE CULL never joins a zero cfg mask, S02Levers.h cfgRequest).
                        const auto code=[&](std::uintptr_t offset,const auto& bytes){
                            Mapping m{};return mapping(engineBase+offset,bytes.size(),m)&&(m.protection&PROT_EXEC)&&fingerprint(engineBase+offset,bytes);};
                        const auto got=[&](std::uintptr_t slot,std::uintptr_t target){
                            std::uintptr_t v{};return readMemory(nullptr,engineBase+slot,&v,sizeof v)&&v==engineBase+target;};
                        // CR: code bytes + GOT now; the device pointer and its byte (must be 0) on the render thread at the first use
                        const bool cr=colorrestart::configure(engineBase,code,&readMemory);
                        s02::setFingerprint(s02::CR,cr?"ok":colorrestart::reason());s02::setInstalled(s02::CR,cr);
                        // MG: mod code only (StereoReplay.cpp latchBoundaryPhase / StereoReplayAsync.cpp merge splice)
                        s02::setFingerprint(s02::MG,"ok");s02::setInstalled(s02::MG,true);
                        // PT: the 11 tail sites; they need the bind-only draw path (POST FX site hooks + nativeBindInputLayout)
                        namespace pt=postfx::tail;
                        unsigned tailHooked=0;
                        if(postfx::ready.load()){
                            const std::uintptr_t wrappers[pt::Count]{reinterpret_cast<std::uintptr_t>(&tailSite<pt::Weather>),reinterpret_cast<std::uintptr_t>(&tailSite<pt::Volumetric>),
                                reinterpret_cast<std::uintptr_t>(&tailSite<pt::Gravestones>),reinterpret_cast<std::uintptr_t>(&tailSite<pt::PlayerNames>),
                                reinterpret_cast<std::uintptr_t>(&tailSite<pt::HiresParticles>),reinterpret_cast<std::uintptr_t>(&tailSite<pt::FxObjects>),
                                reinterpret_cast<std::uintptr_t>(&tailSite<pt::TouchPftx>),reinterpret_cast<std::uintptr_t>(&tailSite<pt::LightCones>),
                                reinterpret_cast<std::uintptr_t>(&tailSite<pt::LightGlows>),reinterpret_cast<std::uintptr_t>(&tailSite<pt::MovieBeam>),
                                reinterpret_cast<std::uintptr_t>(&tailSite<pt::Optics>)};
                            for(unsigned i=0;i<pt::Count;++i){
                                const auto& r=pt::routes[i];const char* fp="ok";
                                bool codeOk=false;
                                if(r.codeBytes==24)codeOk=code(r.original,r.code);
                                else{std::array<unsigned char,16> head{};std::copy_n(r.code.begin(),head.size(),head.begin());codeOk=code(r.original,head);}
                                if(!codeOk)fp="code";
                                else for(unsigned k=0;k<r.rets&&k<2;++k)if(!code(r.ret[k]-4,r.call[k]))fp="call";
                                if(fp[0]=='o'&&!got(r.got,r.original))fp="got";
                                if(fp[0]=='o'){tailOriginal[i]=engineBase+r.original; // before the slot goes live
                                    if(replacePointer(engineBase+r.got,engineBase+r.original,wrappers[i]))tailHooked|=1u<<i;else fp="patch";}
                                pt::fingerprint[i].store(fp);
                            }
                        }else for(auto& f:pt::fingerprint)f.store("postFxNotReady");
                        pt::hookedMask.store(tailHooked);
                        pt::early::configure(engineBase,tailOriginal,tailHooked,&readMemory); // TAIL IN EYES (STEREO TEST 21 / debug.rdrvr.tail)
                        s02::setFingerprint(s02::PT,tailHooked==(1u<<pt::Count)-1u?"ok":tailHooked?"partial":postfx::ready.load()?"none":"postFxNotReady");
                        s02::setInstalled(s02::PT,tailHooked!=0);
                        // Markers TailEnd / QueriesEnd (observation only; GpuMark writes only while DETAIL is armed)
                        {const char* fp=!code(pt::endMainColorOffset,pt::endMainColorFingerprint)||!code(pt::endMainColorReturnOffset-4,pt::endMainColorCallFingerprint)?"endMainColorCode":
                             !code(pt::runQueriesOffset,pt::runQueriesFingerprint)||!code(pt::runQueriesReturnOffset-4,pt::runQueriesCallFingerprint)?"runQueriesCode":
                             !got(pt::endMainColorGotOffset,pt::endMainColorOffset)||!got(pt::runQueriesGotOffset,pt::runQueriesOffset)?"got":"ok";
                         if(fp[0]=='o'){
                             endMainColorOriginal=engineBase+pt::endMainColorOffset;runQueriesOriginal=engineBase+pt::runQueriesOffset;
                             if(replacePointer(engineBase+pt::endMainColorGotOffset,endMainColorOriginal,reinterpret_cast<std::uintptr_t>(&tailEndMarker))){
                                 if(replacePointer(engineBase+pt::runQueriesGotOffset,runQueriesOriginal,reinterpret_cast<std::uintptr_t>(&queriesEndMarker)))pt::markersHooked.store(true);
                                 else{fp="patch";if(!replacePointer(engineBase+pt::endMainColorGotOffset,reinterpret_cast<std::uintptr_t>(&tailEndMarker),endMainColorOriginal))patchFault=true;}
                             }else fp="patch";}
                         pt::markerFingerprint.store(fp);}
                        // NF: the suppressPrimitive* branch exists whenever the bind-only draw path is installed
                        s02::setFingerprint(s02::NF,postfx::ready.load()?"ok":"postFxNotReady");s02::setInstalled(s02::NF,postfx::ready.load());
                        // IR / IT (+ the AO-sphere observer)
                        namespace ir=indoorrt;
                        ir::configure(engineBase,code,readMemory);
                        if(ir::fpOk(ir::stats.reflectionFp)){ir::originalReflection=reinterpret_cast<ir::Reflection>(engineBase+ir::reflectionOffset);
                            if(replacePointer(engineBase+ir::reflectionGotOffset,engineBase+ir::reflectionOffset,reinterpret_cast<std::uintptr_t>(&indoorReflection)))ir::stats.reflectionHooked.store(true);
                            else ir::stats.reflectionFp.store("patch");}
                        if(ir::fpOk(ir::stats.treeFp)){ir::originalTree=reinterpret_cast<ir::Tree>(engineBase+ir::treeOffset);
                            if(replacePointer(engineBase+ir::treeGotOffset,engineBase+ir::treeOffset,reinterpret_cast<std::uintptr_t>(&indoorTree)))ir::stats.treeHooked.store(true);
                            else ir::stats.treeFp.store("patch");}
                        if(ir::fpOk(ir::stats.aoFp)){ir::originalAo=reinterpret_cast<ir::AoSphere>(engineBase+ir::aoOffset);
                            if(replacePointer(engineBase+ir::aoGotOffset,engineBase+ir::aoOffset,reinterpret_cast<std::uintptr_t>(&aoSphereObserver)))ir::stats.aoHooked.store(true);
                            else ir::stats.aoFp.store("patch");}
                        const bool indoorSignal=indoor::config.envReady.load()||indoor::config.roomsReady.load();
                        s02::setFingerprint(s02::IR,!ir::stats.reflectionHooked.load()?ir::stats.reflectionFp.load():indoorSignal?"ok":"noIndoorSignal");
                        s02::setInstalled(s02::IR,ir::stats.reflectionHooked.load()&&indoorSignal);
                        s02::setFingerprint(s02::IT,!ir::stats.treeHooked.load()?ir::stats.treeFp.load():indoorSignal?"ok":"noIndoorSignal");
                        s02::setInstalled(s02::IT,ir::stats.treeHooked.load()&&indoorSignal);
                    }
                    { // TEXTURE FILTER / MIP BIAS (RecreateSamplerStates observer, then PerformDMZ, rollback),
                      // engineLod BeginFrame observer, meminfo cache. debug.rdrvr.engineknobs=0 installs nothing.
                        const auto code=[&](std::uintptr_t offset,const auto& bytes){
                            Mapping m{};return mapping(engineBase+offset,bytes.size(),m)&&(m.protection&PROT_EXEC)&&fingerprint(engineBase+offset,bytes);};
                        const auto read=[&](std::uintptr_t address,void* out,std::size_t size){return readMemory(nullptr,address,out,size);};
                        const auto replace=[&](std::uintptr_t slot,std::uintptr_t expected,std::uintptr_t replacement){return replacePointer(slot,expected,replacement);};
                        const auto knobs=enginetex::install(engineBase,code,read,replace);
                        if(knobs.rollbackFault)patchFault=true;
                        runtime::textureFilterReady.store(knobs.textureFilter?(knobs.live?1:2):0,std::memory_order_relaxed);
                    }
                    { // STATIC LOD / TERRAIN LOD (StaticLod.h, TerrainLod.h): every consumer, call site and engine-rule byte span plus the
                      // GOT word is proven first; one JUMP_SLOT each (AddToDrawListFade, GetTerrainStreamingFactor); any mismatch = not hooked.
                        const auto code=[&](std::uintptr_t offset,const auto& bytes){
                            Mapping m{};return mapping(engineBase+offset,bytes.size(),m)&&(m.protection&PROT_EXEC)&&fingerprint(engineBase+offset,bytes);};
                        const auto read=[&](std::uintptr_t address,void* out,std::size_t size){return readMemory(nullptr,address,out,size);};
                        const auto replace=[&](std::uintptr_t slot,std::uintptr_t expected,std::uintptr_t replacement){return replacePointer(slot,expected,replacement);};
                        staticlod::install(engineBase,code,read,replace);terrainlod::install(engineBase,code,read,replace);
                    }
                    const auto checkedEyeStock=[&](std::uintptr_t offset,std::uintptr_t got){
                        Mapping data{};std::uintptr_t pointer{};
                        return mapping(engineBase+offset,sizeof(std::int32_t),data)&&
                            (data.protection&(PROT_READ|PROT_WRITE))==(PROT_READ|PROT_WRITE)&&
                            readMemory(nullptr,engineBase+got,&pointer,sizeof(pointer))&&pointer==engineBase+offset;
                    };
                    if(resolutionInterposed&&gameEyeResolutionCoordinatesReady()&&eyeResolutionProof(checkedEyeImage)&&nativeFramebufferExtentProof(checkedEyeImage)&&
                       checkedEyeStock(eyeResolutionStockWidthOffset,eyeResolutionStockWidthGotOffset)&&
                       checkedEyeStock(eyeResolutionStockHeightOffset,eyeResolutionStockHeightGotOffset)&&
                       nativeEyeResolution.configure({reinterpret_cast<NativeEyeResolution::MaxScreen>(engineBase+eyeResolutionMaxScreenOffset),
                           reinterpret_cast<NativeEyeResolution::Resize>(engineBase+eyeResolutionResizeOffset),
                           reinterpret_cast<NativeEyeResolution::Resize>(engineBase+eyeResolutionRebuildOffset),readMemory,engineBase+eyeResolutionVegetationSettingsOffset,
                           engineBase+eyeResolutionStockWidthOffset,engineBase+eyeResolutionStockHeightOffset})&&
                       nativeFramebufferExtent.configure({reinterpret_cast<NativeFramebufferExtent::DepthCreate>(engineBase+framebufferDepthCreateOffset),
                           reinterpret_cast<NativeFramebufferExtent::FramebufferCtor>(engineBase+framebufferCtorOffset),
                           eyeRequestedExtent,eyeDepthCreated,nullptr,
                           engineBase+framebufferDepthVtableAddressPointOffset,engineBase+framebufferColorVtableAddressPointOffset,
                           engineBase+framebufferTextureVtableAddressPointOffset,engineBase+framebufferObjectVtableAddressPointOffset,
                           reinterpret_cast<const void*>(engineBase+framebufferDisplayDepthNameOffset),
                           engineBase+framebufferDisplayVtableAddressPointOffset})){
                        nativeEyeRendererInit=reinterpret_cast<EyeInitial>(engineBase+eyeResolutionRendererInitOffset);
                        nativeEyePostfxInit=reinterpret_cast<EyeInitial>(engineBase+eyeResolutionPostfxInitOffset);
                        nativeEyeGrassInit=reinterpret_cast<EyeInitial>(engineBase+eyeResolutionGrassInitOffset);
                        nativeEyeTreesInit=reinterpret_cast<void(*)()>(engineBase+eyeResolutionTreesInitOffset);
                        nativeEyeReflectionInit=reinterpret_cast<EyeInitial>(engineBase+eyeResolutionReflectionInitOffset);
                        nativeEyeBackbuffers=reinterpret_cast<EyeInitial>(engineBase+framebufferBackbuffersOffset);
                        struct EyeRoute {std::uintptr_t slot,original,replacement;};
                        const EyeRoute eyeRoutes[]{
                            {eyeResolutionRendererInitGotOffset,eyeResolutionRendererInitOffset,reinterpret_cast<std::uintptr_t>(eyeRendererInit)},
                            {eyeResolutionPostfxInitGotOffset,eyeResolutionPostfxInitOffset,reinterpret_cast<std::uintptr_t>(eyePostfxInit)},
                            {eyeResolutionGrassInitGotOffset,eyeResolutionGrassInitOffset,reinterpret_cast<std::uintptr_t>(eyeGrassInit)},
                            {eyeResolutionTreesInitGotOffset,eyeResolutionTreesInitOffset,reinterpret_cast<std::uintptr_t>(eyeTreesInit)},
                            {eyeResolutionReflectionInitGotOffset,eyeResolutionReflectionInitOffset,reinterpret_cast<std::uintptr_t>(eyeReflectionInit)},
                            {eyeResolutionMaxScreenGotOffset,eyeResolutionMaxScreenOffset,reinterpret_cast<std::uintptr_t>(eyeMaxScreen)},
                            {eyeResolutionResizeGotOffset,eyeResolutionResizeOffset,reinterpret_cast<std::uintptr_t>(eyeNativeResize)},
                            {framebufferDepthCreateVtableSlotOffset,framebufferDepthCreateOffset,reinterpret_cast<std::uintptr_t>(eyeDisplayDepth)},
                            {framebufferBackbuffersGotOffset,framebufferBackbuffersOffset,reinterpret_cast<std::uintptr_t>(eyeBackbuffers)},
                            {framebufferCtorGotOffset,framebufferCtorOffset,reinterpret_cast<std::uintptr_t>(eyeFramebufferCtor)},
                            {framebufferCtorAliasGotOffset,framebufferCtorOffset,reinterpret_cast<std::uintptr_t>(eyeFramebufferCtor)}};
                        unsigned patched{};
                        while(patched<std::size(eyeRoutes)){
                            const auto& route=eyeRoutes[patched];
                            if(!replacePointer(engineBase+route.slot,engineBase+route.original,route.replacement))break;
                            ++patched;
                        }
                        if(patched==std::size(eyeRoutes)){
                            nativeFramebufferExtent.installed(true);
                            nativeEyeResolution.installed(true);
                        }else while(patched){const auto& route=eyeRoutes[--patched];
                            if(!replacePointer(engineBase+route.slot,route.replacement,engineBase+route.original))patchFault=true;}
                    }
#endif
                }
            }
#endif
        }
    }
#endif
    #ifdef RDRVR_HEAD_CAMERA
    if(installed&&experimental::nativeEarlyBootstrap.ready()&&sceneInterposed.load(std::memory_order_acquire)&&installMainVisibilityWaitHook())
        refreshEarlyListCoverage();
#endif
#ifdef RDRVR_HEAD_CAMERA
    if(!installed)disarmOrdinaryObserver();
#endif
    pltInstalled.store(installed,std::memory_order_release);
    state(installed?"INIT_PLT_ARMED":"INIT_PLT_DECLINED");
}
}
void traceConfigure(const char* path) {std::lock_guard guard(lock);if(!entrySeen && enginePath.empty()){enginePath=path;status="HASHING";}}
void traceVerified(const char* path,const char* hash) {
    std::lock_guard guard(lock);if(enginePath==path && acceptedHash(hash,engineSha))verification.markVerified();
}
// Snapshot trace-owned fields under their own lock. Never hold this lock while
// collecting XR/render subsystem reports, which have independent lifetimes
// and synchronization. Event writers must remain able to make progress.
static std::string traceBaseReport() {
    std::lock_guard guard(lock);std::ostringstream out;
    const auto counts=tracer.counts();
#ifdef RDRVR_TOUCH_CONTROLS
    constexpr const char* gameplayHooks="true";
#else
    constexpr const char* gameplayHooks="false";
#endif
    out<<"{\"schemaVersion\":2,\"buildId\":\""<<RDRVR_BUILD_ID<<"\",\"engineSha256\":\""<<engineSha
       <<"\",\"mode\":\"vulkan-call-trace\",\"status\":\""<<status<<"\",\"hashVerified\":"<<(verification.ready()?"true":"false")
       <<",\"entryVerification\":"<<verification.report()
       <<",\"entrySeen\":"<<(entrySeen?"true":"false")<<",\"dispatchInterposed\":"<<(armed?"true":"false")
       <<",\"initPltInterposed\":"<<(pltInstalled?"true":"false")
#ifdef RDRVR_HEAD_CAMERA
       <<",\"uniformLife92\":{\"hooks\":"<<uniformLife92.installed()<<",\"qualifiedCreates\":"<<uniformLife92.qualifiedCreates()<<",\"retirements\":"<<uniformLife92.retirements()<<'}'
#endif
       <<",\"patchFault\":"<<(patchFault?"true":"false")<<",\"recordFailures\":"<<counts.failures
       <<",\"memoryReadBackend\":\""<<memory.backend()<<"\",\"memoryReaderReady\":"<<(memory.ready()?"true":"false")
       <<",\"memoryReadErrno\":"<<memory.error()
       <<",\"inlinePatch\":false,\"gameplayHooks\":"<<gameplayHooks<<",\"runtimeGameplay\":\"PENDING\",\"initCalls\":"<<initCalls
       <<",\"eventsDropped\":"<<dropped<<",\"events\":[";
    for(unsigned i=0;i<events.size();++i){if(i)out<<',';out<<events[i];}
    out<<"],\"callCounts\":{\"createInstance\":"<<counts.instances<<",\"createDevice\":"<<counts.devices
       <<",\"getDeviceQueue\":"<<counts.queues<<",\"queueSubmit\":"<<counts.submits
       <<",\"enumeratePhysicalDevices\":"<<counts.enumerations<<",\"destroyInstance\":"<<counts.instanceDestructions
       <<",\"destroyDevice\":"<<counts.deviceDestructions<<",\"getPhysicalDeviceQueueFamilyProperties\":"<<counts.familyQueries<<"},\"creationBudgetExceeded\":"
       <<((counts.instances>8||counts.devices>8)?"true":"false")<<",\"submitSamplesLimited\":"<<(counts.submits>32?"true":"false")<<",\"lifetimeBudgetExceeded\":"<<((counts.enumerations>16||counts.instanceDestructions>8||counts.deviceDestructions>8||counts.familyQueries>8||counts.queues>16)?"true":"false")<<'}';
    return out.str();
}
#ifdef RDRVR_BENCH_CONTROL
bool traceEnableShaderCapture(){return shaderCapture.enable();}
std::vector<unsigned char> traceTakeShader(){return shaderCapture.take();}
#endif
#ifdef RDRVR_HEAD_CAMERA
// Live report object "vr" = every VR module (menu, loco, camera, hands, holsters with haptics + mainThread).
std::string vrReport() {
    std::string out="{\"menu\":"+::rdrvr::vr::menuReport();
    out+=",\"loco\":"+::rdrvr::vr::loco::report();
    out+=",\"camera\":"+::rdrvr::vr::camera::rig.report();
    out+=",\"hands\":"+::rdrvr::vr::hands::report();
    out+=",\"taa\":"+::rdrvr::vr::taa::report();
    out+=",\"holsters\":"+::rdrvr::vr::holsters::report();
    out+=",\"weapon\":"+::rdrvr::vr::weapon::report();
    out+=",\"throw\":"+::rdrvr::vr::throwing::report();
    out+=",\"spin\":"+::rdrvr::vr::spin::report();
    out+=",\"controls\":"+::rdrvr::vr::controls::report();
    out+=",\"melee\":"+::rdrvr::vr::melee::report();
    out+=",\"cover\":"+::rdrvr::vr::cover::report();
    out+=",\"body\":"+::rdrvr::vr::bodyik::report();
    out+=",\"cheats\":"+::rdrvr::vr::cheats::report();
    out+=",\"rain\":"+::rdrvr::vr::rain::report();
    out+=",\"arc\":"+::rdrvr::vr::arc::report();          // ARC: main side (contexts, start, tracer, commit, channels, jitter)
    out+=",\"probes\":"+::rdrvr::vr::arc::probesReport(); // ARC: main-thread probe ledger (laser, throw flight, arcs, commit, ...)
    out+=",\"tploco\":"+::rdrvr::vr::tploco::report();
    out+=",\"lasso\":"+::rdrvr::vr::lasso::report();
    out+=",\"marker\":"+::rdrvr::vr::marker::report();
    out+=",\"cutscene\":"+::rdrvr::vr::cutscene::report();
    out+=",\"hud\":"+::rdrvr::vr::hud::report();
    { // rage::grcViewport::sm_ScreenViewport (0x4e6e14e0, a grcViewport*) width/height (+0x330/+0x334): the window
      // hudStreamingMap::DrawMap resets to (SetCurrent(screen) + ResetWindow) - the minimap interior lands there, not on the eye target
        std::uintptr_t vp{};std::int32_t wh[2]{-1,-1};
        if(engineBase&&readMemory(nullptr,engineBase+0x4e6e14e0u,&vp,sizeof vp)&&vp&&vp<UINTPTR_MAX-0x400)readMemory(nullptr,vp+0x330,wh,sizeof wh);
        out+=",\"hudScreenViewport\":["+std::to_string(wh[0])+","+std::to_string(wh[1])+"]";
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_NATIVE_MENUS)
        out+=",\"hudScreenViewportPatch\":"+gameHudScreenViewportReport();
        out+=",\"eyeWater\":"+gameEyeWaterReport();
        out+=",\"subtitles\":"+gameSubtitlesReport();
        out+=",\"radarFix\":"+radarFixReport();
        out+=",\"ptfxCull\":{\"hooked\":"+std::string(ptfxCullHooked.load()?"true":"false")+",\"swaps\":"+std::to_string(ptfxCullSwaps.load())+
             ",\"kept\":"+std::to_string(ptfxCullKept.load())+",\"dropped\":"+std::to_string(ptfxCullDropped.load())+"}";
        out+=",\"touchPftx\":{\"hooked\":"+std::string(touchHooked.load()?"true":"false")+",\"eyeViewports\":"+std::to_string(touchEyeViewports.load())+"}";
        {std::string n=gameCutsceneActorNames();for(auto& ch:n)if(ch=='"'||ch==92||static_cast<unsigned char>(ch)<32)ch='_';out+=",\"cutsceneActors\":\""+n+"\"";}
#endif
#ifdef RDRVR_NATIVE_MENUS
        out+=",\"uiMenu\":{\"lever\":"+std::to_string(uiMenuLever.load())+",\"calls\":"+std::to_string(uiMenuCalls.load())+",\"native\":"+std::to_string(uiMenuNative.load())+
             ",\"stack\":"+std::to_string(uiMenuStack.load())+",\"hudOnly\":"+std::to_string(uiMenuHudOnly.load())+",\"cutscene\":"+std::to_string(uiMenuCutscene.load())+
             ",\"unread\":"+std::to_string(uiMenuStackUnread.load())+",\"lastStack\":"+std::to_string(uiMenuLastStack.load())+"}";
#endif
    }
    out+=",\"mvactors\":"+::rdrvr::vr::mvactors::report();
    out+=",\"inputRec\":"+::rdrvr::vr::inputrec::recordReport();    // INPUT RECORD (debug.rdrvr.input.record)
    out+=",\"inputReplay\":"+::rdrvr::vr::inputrec::replayReport(); // INPUT REPLAY (debug.rdrvr.input.replay)
    return out+"}";
}
#endif
std::string traceReport() {
    auto json=traceBaseReport();
#ifdef RDRVR_HEAD_CAMERA
    std::ostringstream ordinary;
    ordinary<<",\"ordinaryDepthReplay\":{\"requested\":"<<(ordinaryMainStartupRequested()?"true":"false")
        <<",\"observationEnabled\":"<<(indexedObservation.enabled()?"true":"false")
        <<",\"frames\":"<<ordinaryReplayFrames.load(std::memory_order_relaxed);
    for(unsigned i=0;i<std::size(ordinaryReplayWorkNames);++i)
        ordinary<<",\""<<ordinaryReplayWorkNames[i]<<"\":"<<ordinaryReplayLastWork[i].load(std::memory_order_relaxed);
    ordinary<<",\"coldStage\":\""<<ordinaryColdStage.load()<<"\",\"armPrerequisites\":"<<ordinaryArmPrerequisites.load()
        <<",\"armResult\":"<<ordinaryArmResult.load()<<",\"observationLifecycle\":"<<indexedObservation.lifecycleBits()
        <<",\"disarmReason\":\""<<indexedObservation.disarmReason()<<"\",\"cachedHooksReady\":"<<runtime::graphicsOcclusionAvailable.load();
    if(auto* t=rdr95::ordinary_integration::transaction.load())ordinary<<",\"installDiagnostic\":"<<t->installDiagnostic()
        <<",\"installOffset\":"<<t->diagnosticOffset()<<",\"installValue\":"<<t->diagnosticValue()<<",\"ownedSlots\":"<<t->ownedMask();
    ordinary<<",\"gateProbe\":{\"frameLimit\":32,\"frames\":"<<rdr95::gateprobe::publishedFrames.load(std::memory_order_acquire);
    for(unsigned i=0;i<rdr95::gateprobe::Count;++i)ordinary<<",\""<<rdr95::gateprobe::names[i]<<"\":"<<rdr95::gateprobe::published[i].load(std::memory_order_relaxed);
    ordinary<<",\"sampleBuckets\":[";
    for(unsigned k=0;k<2;++k){if(k)ordinary<<',';ordinary<<"{\"kind\":\""<<(k?"observerCallerOffset":"staticDrawableVtableOffset")<<"\",\"overflow\":"<<rdr95::gateprobe::publishedOverflow[k].load()<<",\"buckets\":[";
        bool comma=false;for(unsigned i=0;i<16;++i){auto n=rdr95::gateprobe::publishedBuckets[k][2*i+1].load();if(!n)continue;if(comma)ordinary<<',';comma=true;
            ordinary<<"{\"offset\":"<<rdr95::gateprobe::publishedBuckets[k][2*i].load()<<",\"count\":"<<n<<'}';}ordinary<<"]}";}
    ordinary<<"],\"bodyGateInputs\":[";
    for(unsigned i=0;i<32;++i){if(i)ordinary<<',';ordinary<<rdr95::gateprobe::publishedBodyClasses[i].load();}
    ordinary<<"]}}";json.insert(json.size()-1,ordinary.str());
    const auto settings=runtime::graphicsSettings.snapshot();const auto g=graphicsDrawPolicy.counts();
    std::ostringstream graphics;graphics<<",\"graphics\":{\"ready\":"<<graphicsDrawPolicy.ready()
        <<",\"resolutionPercent\":"<<settings.requested.resolutionPercent<<",\"startupResolutionPercent\":"<<settings.startupResolutionPercent
        <<",\"pendingRestart\":"<<settings.pendingRestart<<",\"shadows\":"<<settings.requested.shadows
        <<",\"grass\":"<<settings.requested.grass<<",\"edgeMarginPercent\":"<<settings.requested.edgeMarginPercent
        <<",\"profilerEpoch\":"<<diagnostics::profilerTicket()<<",\"profiler\":"<<runtime::graphicsSettings.detailedProfiling()<<",\"profilerMode\":\""<<runtime::GraphicsSettings::profilerModeName(settings.requested.profilerMode)<<"\""<<",\"occlusionAvailable\":"<<nativeEarlyRuntimeAvailable()<<",\"occlusion\":"<<settings.requested.occlusion
        <<",\"persistenceOk\":"<<settings.persistenceOk<<",\"scenes\":"<<g.scenes<<",\"nested\":"<<g.nested
        <<",\"sunCalls\":"<<g.sunCalls<<",\"sunSkipped\":"<<g.sunSkipped
        <<",\"grassCalls\":"<<g.grassCalls<<",\"grassSkipped\":"<<g.grassSkipped
        <<",\"shadowMapSetting\":"<<settings.requested.shadowMap<<",\"shadowUpdateSetting\":"<<settings.requested.shadowUpdate
        <<",\"cfgVersion\":"<<settings.cfgVersionLoaded<<",\"cfgMigrated\":"<<(settings.cfgMigrated?1:0) // cfgVersion < 3 -> current defaults this start
        <<",\"cfgMigrationSaved\":"<<(settings.cfgMigrationSaved?1:0)
        <<",\"settings53\":{\"occlusionV2\":"<<settings.requested.occlusion<<",\"legacyOcclusion\":"<<settings.legacyOcclusion
        <<",\"postFx\":"<<settings.requested.postFx<<",\"postFxName\":\""<<runtime::GraphicsSettings::postFxName(settings.requested.postFx)<<'"'
        <<",\"postFxMenuState\":"<<runtime::postFxMenuState.load(std::memory_order_relaxed)<<",\"postFxProperty\":"<<runtime::postFxMenuProperty.load(std::memory_order_relaxed)
        <<",\"textureFilter\":"<<settings.requested.textureFilter<<",\"textureFilterName\":\""<<runtime::GraphicsSettings::textureFilterName(settings.requested.textureFilter)<<'"'
        <<",\"textureFilterReady\":"<<runtime::textureFilterReady.load(std::memory_order_relaxed)
        <<",\"mipBias\":"<<settings.requested.mipBias<<",\"mipBiasName\":\""<<runtime::GraphicsSettings::mipBiasName(settings.requested.mipBias)<<'"'
        <<",\"sharpen\":"<<settings.requested.sharpen<<",\"sharpenName\":\""<<runtime::GraphicsSettings::sharpenName(settings.requested.sharpen)<<'"'
        <<",\"sharpenEffective\":"<<runtime::sharpenEffective.load(std::memory_order_relaxed)
        <<",\"cascadeCull\":"<<settings.requested.cascadeCull<<",\"cascadeCullName\":\""<<runtime::GraphicsSettings::cascadeCullName(settings.requested.cascadeCull)<<'"'
        <<",\"cascadeCullReady\":"<<runtime::cascadeCullReady.load(std::memory_order_relaxed)
        <<",\"sunStaggerReady\":"<<runtime::sunStaggerReady.load(std::memory_order_relaxed)<<",\"sunStaggerGate\":"<<runtime::sunStaggerGate.load(std::memory_order_relaxed)
        <<",\"lampShadows\":"<<settings.requested.lampShadows<<",\"lampShadowsName\":\""<<runtime::GraphicsSettings::lampShadowsName(settings.requested.lampShadows)<<'"'
        <<",\"lampShadowsReady\":"<<runtime::lampShadowsReady.load(std::memory_order_relaxed)
        <<",\"s02Mask\":\""<<s02::hex4(settings.requested.s02Mask)<<"\",\"s02MaskClamped\":"<<(settings.s02MaskClamped?1:0)
        <<",\"xrQueuePriority\":"<<settings.requested.xrQueuePriority<<",\"xrQueuePriorityName\":\""<<runtime::GraphicsSettings::xrQueuePriorityName(settings.requested.xrQueuePriority)<<'"'
        <<",\"startupXrQueuePriority\":"<<settings.startupXrQueuePriority<<",\"xrQueueVerified\":"<<settings.requested.xrQueueVerified
        <<",\"xrQueueLayoutActive\":"<<runtime::xrQueueLayoutActive.load(std::memory_order_relaxed)
        <<",\"frameRate\":"<<settings.requested.frameRate<<",\"frameRateName\":\""<<runtime::GraphicsSettings::frameRateName(settings.requested.frameRate)<<'"'
        <<",\"governorHz\":"<<runtime::frameRateGovernorHz.load(std::memory_order_relaxed)
        <<",\"foveation\":"<<settings.requested.foveation<<",\"foveationName\":\""<<runtime::GraphicsSettings::foveationName(settings.requested.foveation)<<'"'
        <<",\"foveationLive\":"<<runtime::foveationLiveLevel.load(std::memory_order_relaxed)
        <<",\"shadowUpdate\":"<<settings.requested.shadowUpdate<<",\"shadowUpdateName\":\""<<runtime::GraphicsSettings::shadowUpdateName(settings.requested.shadowUpdate)<<'"'
        <<",\"cameraIndoor\":"<<runtime::cameraIndoorMenu.load(std::memory_order_relaxed)
        <<",\"grassRow\":\""<<(settings.requested.grass?runtime::GraphicsSettings::grassDistanceName(settings.requested.grassDistance):"OFF")<<"\"}"
        <<','<<shadowgrass::report()<<','<<lampshadow::report()<<','<<postfx::report()<<','<<postfx::tail::early::report()<<','<<enginetex::report()<<','<<meminfo::report()<<','<<hygiene::report()
        <<','<<px2d::report() // PX 2D: graphics.px2d
        <<','<<s02::report([]{std::ostringstream f;f<<"\"ptSites\":{";
            for(unsigned i=0;i<postfx::tail::Count;++i){const char* x=postfx::tail::fingerprint[i].load();f<<(i?",":"")<<'"'<<postfx::tail::names[i]<<"\":\""<<(x?x:"unconfigured")<<'"';}
            f<<"},\"ptMarkers\":\""<<postfx::tail::markerFingerprint.load()<<"\",\"irReflection\":\""<<indoorrt::stats.reflectionFp.load()<<"\",\"itTree\":\""<<indoorrt::stats.treeFp.load()
             <<"\",\"ao\":\""<<indoorrt::stats.aoFp.load()<<'"';return f.str();}())
        <<','<<colorrestart::report()<<','<<indoorrt::report()<<','<<ablate::report()<<','<<creaturelod::report()<<','<<staticlod::report()<<','<<terrainlod::report()
        <<','<<lowfreq::report(gameFragmentRateBridge().coarsePipelines(),runtime::foveationLiveLevel.load(std::memory_order_relaxed))<<"}"
        <<",\"stereoOcclusion\":{\"available\":"<<nativeEarlyRuntimeAvailable()<<",\"backend\":\"early-main-before-native-drawcommon\"}"<<",\"textureQuality\":"<<diagnostics::textureQualityTelemetry.report();
    json.pop_back();json+=graphics.str()+"}";
#endif

#ifdef RDRVR_BENCH_CONTROL
    json.pop_back();json+=",\"shaderCapture\":"+shaderCapture.report()+"}";
#endif
#ifdef RDRVR_GAME_XR
    json.pop_back();
#ifdef RDRVR_GAME_SESSION
    json+=",\"swappyInterposed\":"+std::string(swappyInterposed?"true":"false");
#endif
#ifdef RDRVR_CAMERA_BRIDGE
    json+=",\"sceneCameraInterposed\":"+std::string(sceneInterposed?"true":"false")+
        ",\"sceneObservationErrors\":"+std::to_string(cameraBoundary.errors())+
        ",\"sceneNestedCalls\":"+std::to_string(cameraBoundary.nested())+
        ",\"sceneWithoutCameraCapture\":"+std::to_string(cameraBoundary.missed());
#ifdef RDRVR_NATIVE_MENUS
    json+=",\"hudInterposed\":"+std::string(hudInterposed?"true":"false")+
        ",\"hudCalls\":"+std::to_string(hudBoundary.qualified())+",\"hudCompleted\":"+std::to_string(hudBoundary.completed())+
        ",\"hudAborted\":"+std::to_string(hudBoundary.aborted())+",\"hudObserverErrors\":"+std::to_string(hudBoundary.errors())+
        ",\"hudNested\":"+std::to_string(hudBoundary.nested())+
        ",\"hudFrames\":"+std::to_string(hudFrameBoundary.frames())+",\"hudIntervals\":"+std::to_string(hudFrameBoundary.begins())+
        ",\"hudIntervalEnds\":"+std::to_string(hudFrameBoundary.ends())+",\"hudIntervalAborts\":"+std::to_string(hudFrameBoundary.aborts())+
        ",\"hudFrameErrors\":"+std::to_string(hudFrameBoundary.errors());
#endif
#ifdef RDRVR_PASS_RESOURCES
    json+=",\"visibilityInterposed\":"+std::string(visibilityInterposed?"true":"false")+
        ",\"textureGlobalInterposed\":"+std::string(textureGlobalInterposed?"true":"false")+
        ",\"textureInstanceInterposed\":"+std::string(textureInstanceInterposed?"true":"false")+
        ",\"targetLockInterposed\":"+std::string(targetLockInterposed?"true":"false")+
        ",\"targetUnlockInterposed\":"+std::string(targetUnlockInterposed?"true":"false")+
        ",\"sceneResolveInterposed\":"+std::string(sceneResolveInterposed?"true":"false")+
        ",\"conservativeOcclusionConfigured\":"+std::string(occlusionConfigured?"true":"false")+
        ",\"cullInterposed\":"+std::string(cullInterposed?"true":"false")+
        ",\"nativeRoomTraversals\":"+std::to_string(cullBoundary.traversalCalls(true))+
        ",\"visibilityRoomRecordsLast\":"+std::to_string(cullBoundary.roomRecordsLast())+
        ",\"visibilityRoomRecordsMax\":"+std::to_string(cullBoundary.roomRecordsMax())+
        ",\"visibilityRoomRecordReadFailures\":"+std::to_string(cullBoundary.roomRecordReadFailures())+
        ",\"nativeOutdoorTraversals\":"+std::to_string(cullBoundary.traversalCalls(false))+
        ",\"occluderTest\":"+occluderInjection.report()+
        ",\"smallObjects\":"+smallObjectCull.report()+
        ",\"guardedReads\":"+[]{Dl_info self{};return guardedReadSites.report(dladdr(reinterpret_cast<void*>(&readMemory),&self)?reinterpret_cast<std::uintptr_t>(self.dli_fbase):0);}()+
        ",\"directReads\":{\"ranges\":"+std::to_string(directReads.count())+",\"mappedBytes\":"+std::to_string(directReads.bytes())+'}'+
        ",\"replayInterposed\":"+std::string(replayInterposed?"true":"false")+
        ",\"profileInterposed\":"+std::string(profileInterposed?"true":"false")+
        ",\"worldProfile\":"+gameWorldProfile().report()+ // cpu02: debug.rdrvr.profile.plain state, per-eye calls/filtered identity
        ",\"retailInterposed\":"+std::string(retailInterposed?"true":"false")+
        ",\"resolutionInterposed\":"+std::string(resolutionInterposed?"true":"false")+
        ",\"nativeResolution\":"+resolutionPolicy.report()+
        ",\"retailScenes\":"+std::to_string(retailBoundary.complete())+
        ",\"retailConsumed\":"+std::to_string(retailBoundary.consumed())+
        ",\"retailErrors\":"+std::to_string(retailBoundary.errors())+
        ",\"visibilityObservationErrors\":"+std::to_string(visibilityBoundary.errors())+
        ",\"visibilityNestedCalls\":"+std::to_string(visibilityBoundary.nested());
#endif
#endif
#ifdef RDRVR_TOUCH_CONTROLS
    json+=",\"reticleInterposed\":"+std::string(reticleInterposed?"true":"false")+
        ",\"reticleObserved\":"+std::to_string(reticleBoundary.observed())+",\"reticleNested\":"+std::to_string(reticleBoundary.nested());
    json+=",\"weaponManagerInterposed\":"+std::string(weaponManagerInterposed?"true":"false")+
        ",\"weaponManagerEntered\":"+std::to_string(weaponManagerBoundary.entered())+",\"weaponManagerCompleted\":"+std::to_string(weaponManagerBoundary.completed())+
        ",\"weaponAimInterposed\":"+std::string(weaponAimInterposed?"true":"false")+
        ",\"weaponPoseInterposed\":"+std::string(weaponPoseInterposed?"true":"false")+
        ",\"weaponAimPrepared\":"+std::to_string(weaponAimBoundary.prepared())+",\"weaponPosePrepared\":"+std::to_string(weaponPoseBoundary.prepared())+",\"weaponPoseCompleted\":"+std::to_string(weaponPoseBoundary.completed())+
        ",\"rumbleInterposed\":"+std::string(rumbleInterposed?"true":"false")+",\"rumbleObserved\":"+std::to_string(rumbleBoundary.observed())+
        ",\"rumbleObserverFailed\":"+std::to_string(rumbleBoundary.failed())+",\"rumbleNested\":"+std::to_string(rumbleBoundary.nested())+
        ",\"weaponFireInterposed\":"+std::string(weaponFireInterposed?"true":"false")+",\"weaponFireObserved\":"+std::to_string(weaponFireBoundary.entered())+
        ",\"weaponFireCompleted\":"+std::to_string(weaponFireBoundary.completed())+",\"weaponFireObserverFailed\":"+std::to_string(weaponFireBoundary.failed())+
        ",\"weaponFireNested\":"+std::to_string(weaponFireBoundary.nested());
#ifdef RDRVR_HEAD_CAMERA
    json+=",\"reticleUiInterposed\":"+std::string(reticleUiInterposed?"true":"false")+
        ",\"reticleUiEntered\":"+std::to_string(reticleUiBoundary.entered())+
        ",\"reticleUiCompleted\":"+std::to_string(reticleUiBoundary.completed())+
        ",\"reticleUiObserverFailed\":"+std::to_string(reticleUiBoundary.failed());
    json+=",\"projectileLaunchInterposed\":"+std::string(projectileLaunchInterposed?"true":"false")+
        ",\"projectileLaunchEntered\":"+std::to_string(projectileLaunchBoundary.entered())+
        ",\"projectileLaunchCompleted\":"+std::to_string(projectileLaunchBoundary.completed())+
        ",\"projectileLaunchObserverFailed\":"+std::to_string(projectileLaunchBoundary.failed());
    json+=",\"weaponModelInterposed\":"+std::string(weaponModelInterposed?"true":"false")+
        ",\"weaponModelPrepared\":"+std::to_string(propModelBoundary.prepared()+artModelBoundary.prepared())+
        ",\"weaponModelCompleted\":"+std::to_string(propModelBoundary.completed()+artModelBoundary.completed())+
        ",\"weaponModelNativeTrue\":"+std::to_string(propModelBoundary.nativeTrue()+artModelBoundary.nativeTrue());
#endif
    json+=",\"inputInterposed\":"+std::string(inputInterposed?"true":"false")+
        ",\"inputInjected\":"+std::to_string(inputBoundary.injected())+",\"inputNested\":"+std::to_string(inputBoundary.nested());
#endif
    const auto gameReport=gameXrReport();
    // A reporting request made in focused stereo gameplay arms ONE diagnostic
    // MAIN inventory on the next primary join. It never enables DETAIL timing.
    if(gameReport.find("\"sessionStatus\":\"GAME_STEREO\"")!=std::string::npos&&
       gameReport.find("\"sessionState\":5")!=std::string::npos&&
       gameReport.find("\"state\":\"NATIVE_WORLD\"")!=std::string::npos)
        rdr95::scene_input::arm();
    return json+",\"vr\":"+vrReport()+",\"teleport\":"+teleport::report()+",\"clock\":"+gameclock::report()+",\"stereoReplay\":"+stereo::report()+",\"cameraDispatchPolicy\":\"NATIVE_EYE_VIEWPORTS_PASSTHROUGH\""+",\"ordinaryGeometryEpoch\":"+experimental::nativeOrdinaryGeometryEpoch.report()+",\"sceneInputTape\":"+rdr95::scene_input::report()+",\"sceneRoutes\":"+rdr95::scene_routes::report()+",\"gameBootstrap\":"+gameReport+"}";
#else
    return json;
#endif
}
std::string fallbackEnginePath(ANativeActivity* activity) {
    auto* env=activity->env;
    if(env->PushLocalFrame(8)<0){env->ExceptionClear();return {};}
    std::string path;
    auto cls=env->GetObjectClass(activity->clazz);
    auto method=cls?env->GetMethodID(cls,"getApplicationInfo","()Landroid/content/pm/ApplicationInfo;"):nullptr;
    auto app=method?env->CallObjectMethod(activity->clazz,method):nullptr;
    auto info=app&&!env->ExceptionCheck()?env->GetObjectClass(app):nullptr;
    auto field=info?env->GetFieldID(info,"nativeLibraryDir","Ljava/lang/String;"):nullptr;
    auto dir=field?static_cast<jstring>(env->GetObjectField(app,field)):nullptr;
    if(dir&&!env->ExceptionCheck()) {
        const char* text=env->GetStringUTFChars(dir,nullptr);
        if(text) {try{path=std::string(text)+"/librdr.so";}catch(...){env->ReleaseStringUTFChars(dir,text);env->PopLocalFrame(nullptr);throw;}
            env->ReleaseStringUTFChars(dir,text);}
    }
    if(env->ExceptionCheck())env->ExceptionClear();env->PopLocalFrame(nullptr);return path;
}
void entry(ANativeActivity* activity,void* saved,std::size_t size) noexcept {
    using Entry=void(*)(ANativeActivity*,void*,std::size_t);
    Entry original{};
    try {
        {std::lock_guard guard(lock);entrySeen=true;}
        if(enginePath.empty()) {
            auto path=fallbackEnginePath(activity);std::lock_guard guard(lock);enginePath=path;
        }
        if(enginePath.empty()) {state("CONFIGURATION_MISSING");ANativeActivity_finish(activity);return;}
        if(!library)library=dlopen(enginePath.c_str(),RTLD_NOW|RTLD_LOCAL);
        if(!library) {state("ENGINE_LOAD_FAILED");ANativeActivity_finish(activity);return;}
        original=reinterpret_cast<Entry>(dlsym(library,"ANativeActivity_onCreate"));
        Dl_info owner{};
        if(!original || !dladdr(reinterpret_cast<void*>(original),&owner) || !owner.dli_fname || enginePath!=owner.dli_fname) {
            state("ENGINE_ENTRY_MISSING");ANativeActivity_finish(activity);return;
        }
        // A restored NativeActivity can arrive before the background hash.
        // No trace lock is held here: traceVerified must be free to complete.
        verification.waitFor(std::chrono::milliseconds(3000));
#ifdef RDRVR_HEAD_CAMERA
        const char* files=activity->externalDataPath?activity->externalDataPath:activity->internalDataPath;
        if(files){
            const auto dir=std::string(files)+"/rdrvr";struct stat info{};
            if((mkdir(dir.c_str(),0700)==0||errno==EEXIST)&&stat(dir.c_str(),&info)==0&&S_ISDIR(info.st_mode))
                {runtime::graphicsSettings.configurePath(dir+"/graphics.cfg");teleport::configureDirectory(dir); // Teleport-spots.txt
                 ::rdrvr::vr::vrSettings.configurePath(dir+"/vr.cfg"); // VR page settings next to graphics.cfg
                 ::rdrvr::vr::controlmap::configurePath(dir+"/controls.cfg");::rdrvr::vr::controlmap::loadOnce();
                 ::rdrvr::vr::inputrec::configureDirectory(dir); // Input-rec-*.bin recordings / replay files
                 runtime::framePacer.setFlightFile(dir+"/flight.json");} // Flight recorder timeline every 10 s
        }
        runtime::graphicsSettings.loadOnce();
        ::rdrvr::vr::vrSettings.loadOnce(); // Missing file = defaults; malformed = defaults + SAVE FAILED until the next save
        diagnostics::setProfiling(runtime::graphicsSettings.profiler());
        ::rdrvr::vr::loadKillSwitches(); // debug.rdrvr.vr.<menu|loco|bump|stumble|camera|roomscale|hands|holsters|twohand|cheats> 0, before any hook
#endif
        arm(reinterpret_cast<void*>(original));
#ifdef RDRVR_GAME_XR
        if(pltInstalled.load(std::memory_order_acquire))gameXrInitialize(activity->vm,activity->clazz);
#endif
    } catch(...) {if(!original){ANativeActivity_finish(activity);return;}}
    original(activity,saved,size);
}
} // namespace rdrvr::engine
extern "C" __attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity* activity,void* saved,std::size_t size) {
    rdrvr::engine::entry(activity,saved,size);
}
