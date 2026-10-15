#pragma once
// Per-eye temporal anti-aliasing of the stereo pair on the RENDER thread (port of the GTA5 Quest TAA).
// For each eye, after the eye's colour stage and before the hands pass and the capture copy
// (EngineStereoPass, TaaHook.h): pass R (resolve): eye colour (3x3) + eye depth (5-tap cross) +
// this eye's history (reprojected with the plan's current/previous view-projection, no jitter, no
// motion vectors) -> new history (RGBA16F: TAA colour, blend b) + aux (RG16F: nearest inverse
// depth, weight); pass A (apply): eye colour = mix(eye, TAA, b) by fixed-function blending; b = 0
// pixels are discarded, so a reset (or "reset every frame") leaves the eye bit-identical to TAA
// off. Optional clamped 5-tap sharpen. Then the mailbox copies the pair (runtime::taaPostFxaa,
// default 0; debug.rdrvr.vr.taa.fxaa=1 runs the mailbox FXAA after TAA).
// Rows (GRAPHICS -> ANTI-ALIASING): OFF / TAA (no jitter, still image = the raw frame, AA from head motion)
// / TAA FULL: EyeViewports draws both eye clones with a Halton(2,3) sub-pixel projection jitter (TaaJitter.h; plan.projection
// and so culling, hands, timewarp, SpaceWarp and the reprojection stay un-jittered), the resolve accumulates the jittered
// samples into the un-jittered history (supersampling of the pixel footprint), outputs the un-jittered colour for every pixel
// (b = 0 pixels: the current frame interpolated at p + jitter), the clamped sharpen runs, nothing is discarded, the area lever
// is ignored. The jitter runs only while TAA FULL resolved both eyes of the last frame (any skip stops it next scene).
// TAA LITE (row value 3, menu order OFF / TAA LITE / TAA / TAA FULL): the cost variant for the
// Quest 3S. Pass R = taa_lite.frag: history = one image in the eye's own format (B8G8R8A8_SRGB: 4 B/px, rgb = the displayed
// colour, a = weight / 16), no aux / depth history, the cross YCoCg clip, one depth gather, one bilinear history tap; only a
// centred area (debug.rdrvr.vr.taa.lite.area, default 70 % of width/height = ~50 % of the pixels; the periphery stays raw);
// pass A = plain copy of the area into the eye (no blending, loadOp DONT_CARE). No jitter. Same reset rules / near field.
// TAA FULL's apply also uses the copy pipeline (its blend alpha is 1: same RGB); all modes batch
// their image barriers.
//
// Threads:
//   render thread configure() every frame, the eye hook (EngineStereoPass), imageDestroyed()
//   worker        one background thread (started by configure): base objects + the resolve pipeline, one apply pipeline
//                 per eye colour format (no compile on the render thread; the eye is skipped while building)
//   any           report(), state()
// Any failure disables TAA only (state FAILED, menu "TAA - FAILED"; the mailbox FXAA still runs): the hook records only
// after every fallible step succeeded, leaves all images in their received layouts and restores the engine state.
// Kill switch: debug.rdrvr.vr.taa=0 at startup: no hook, no worker, no Vulkan object, menu "TAA - UNAVAILABLE".
// Properties (read by configure; .debug, .fxaa and .jitter also re-read every 64 frames):
//   debug.rdrvr.vr.taa.debug    bit 0 reset every frame, bit 1 pure TAA (b = 1), bit 2 apply pass also on reset frames,
//                               bit 3 sharpen also in plain TAA
//   debug.rdrvr.vr.taa.fxaa     0 plain copy after TAA (default), 1 mailbox FXAA after TAA
//   debug.rdrvr.vr.taa.jitter   TAA FULL jitter amplitude in % (100 = +-0.5 px; 0 = jitter off, live)
//   debug.rdrvr.vr.taa.jitterlen TAA FULL Halton sequence length (8; 2..64)
//   debug.rdrvr.vr.taa.near     near field in cm (default 100)           debug.rdrvr.vr.taa.clip   clip gamma x100 (125)
//   (debug.rdrvr.vr.taa.sharp: TAA FULL sharpen amount x100, default 50)
//   debug.rdrvr.vr.taa.cutcm    camera cut per frame in cm (default 50)  debug.rdrvr.vr.taa.cutdeg cut in degrees (5)
//   debug.rdrvr.vr.taa.nmax     max history weight (default 10)          debug.rdrvr.vr.taa.sharp  sharpen x100 (50)
//   debug.rdrvr.vr.taa.depthtest depth test range in m (60, 0 = off)    debug.rdrvr.vr.taa.gpu    0 = no timestamps
//   debug.rdrvr.vr.taa.taps     5 (default: cross moments) / 9 (GTA5 3x3)
//   debug.rdrvr.vr.taa.area     resolved centred area in % of width/height (default 100; 30..100): cost lever
//   TAA LITE (all live, every 64 frames):
//   debug.rdrvr.vr.taa.lite.area  centred area in % of width/height (default 70; 30..100)
//   debug.rdrvr.vr.taa.lite.cubic 1 = 5-tap Catmull-Rom history (as TAA) instead of one bilinear tap (default 0)
//   debug.rdrvr.vr.taa.lite.depth 0 = rotation-only reprojection, no depth read and no near field (default 1)
// While AppSW synthesizes, the row TAA (1) runs as TAA LITE (3); TAA FULL, an explicit TAA LITE and
// OFF are honoured. Session-only effective mode: graphics.cfg / antiAliasingRequested keep the
// selected row. The switch goes through the normal row-change path (new history in the other
// format + margins -> Reset::Resize, both eyes).
//   debug.rdrvr.vr.taa.appswlite  1 (default) = override on, 0 = TAA stays TAA under AppSW. The
//                                 property and the AppSW probe are re-read at most once per second
//                                 (render thread, eye 0); the mode is fixed per render frame.
// Report vr.taa: effectiveMode/effective, appswLite{enabled,probe,synthesizing,overrideActive,
// overrideCount,overrideFrames}; menu "TAA > LITE (APPSW)" (taaMenuState bit 8); gpuStages byState
// "_ta<signatureDigit()>" (GameXr stereoGpuState).
#include "TaaHook.h"
#include "TaaMath.h"
#include "TaaJitter.h"
#include "HandsPass.h"
#include "runtime/AntiAliasingStatus.h" // Row values for appSwEffectiveMode
#include <mutex>
#include <string>

namespace rdrvr::vr::taa {
enum class State : int {Off,Killed,Building,Ready,Failed};
const char* stateName(State) noexcept;
State state() noexcept;

// The row value the TAA pass runs for (pure; host-tested). enabled = debug.rdrvr.vr.taa.appswlite != 0.
constexpr int appSwEffectiveMode(int requested,bool synthesizing,bool enabled) noexcept {
    return enabled&&synthesizing&&requested==runtime::antiAliasingRowTaa?runtime::antiAliasingRowTaaLite:requested;}
// taaMenuState bit 8 = the AppSW TAA -> LITE override is in force (GraphicsMenu.h shows "TAA > LITE (APPSW)").
inline constexpr int menuStateAppSwLite=0x100;
// AppSW probe (GameXr: runtime::spaceWarpControl.synthesizing()); any thread, one atomic store. Null = never synthesizing.
void setSynthesizingProbe(bool (*probe)() noexcept) noexcept;
// The row value the pass ran for at the last render frame (-1 = no eye call yet) / the override is in force.
int effectiveMode() noexcept;
bool appSwOverrideActive() noexcept;
// byState digit "_ta<n>": the effective row value while the module is READY, else 0 (no TAA GPU work).
int signatureDigit() noexcept;
// Host tests: the appswlite value instead of the property (-1 = property) and re-evaluate the latch at the next eye call.
void setAppSwLiteForTests(int value) noexcept;

// Engine bound-state restore after the TAA commands (same callbacks as the hands pass: GameXr handsRestore*).
void setEngineRestore(hands::EngineRestore) noexcept;
// Render thread, every frame (idempotent; next to hands::configure): binds the session device, reads the kill switch /
// properties once, starts the worker and installs the eye hook; publishes the menu state (runtime::taaMenuState).
void configure(VkPhysicalDevice,VkDevice,std::uint32_t family,std::uint32_t queueIndex,std::mutex* queueMutex) noexcept;
// gameImageDestroyed, before the driver destroy: drops views/framebuffers/descriptor sets on that engine image.
void imageDestroyed(VkDevice,VkImage) noexcept;
// destroyDevice, after the device idle, under the queue mutex.
void shutdownAfterIdle(VkDevice) noexcept;
// JSON object for the live report key vr.taa.
std::string report();

// Hook body (exposed for host tests; EngineStereoPass reaches it through eyeHook).
bool recordEye(const hands::EyeCall&) noexcept;
// Host tests: wait (bounded) until the worker finished every requested build; true = nothing pending.
bool waitIdleForTests(unsigned milliseconds) noexcept;
// Host tests: tuning instead of the properties (gpu 0/1 timestamps).
void setTuningForTests(const Tuning&,int gpu) noexcept;
} // namespace rdrvr::vr::taa
