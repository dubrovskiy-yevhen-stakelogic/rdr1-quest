#pragma once
// VR hands (+ holster markers) drawn on the RENDER thread into each eye of the stereo pair, tested against
// the eye's world depth, before the pair is captured. Owns only private Vulkan objects;
// the engine's eye images are used through views/framebuffers created here and destroyed from gameImageDestroyed.
//
// Threads:
//   XR thread     pushPose()                       every locateHands sample (seqlock ring keyed by XR frame id)
//   main thread   setHeldHand(), publishMarkers()  HOLSTERS state (atomics / seqlock)
//   render thread configure() every frame, the eye hook (EngineStereoPass, HandsHook.h), imageDestroyed()
//   worker        one background thread (started by configure): base objects (mesh in host-visible memory,
//                 albedo staging buffer filled on the CPU), pipelines per format pair. No queue submit, the
//                 caller's queue mutex is never taken; the albedo copy is recorded by the render thread into
//                 the engine command buffer (recordEye).
//   any           report(), state()
// BUILDING (or a requested pipeline) longer than 10 s -> FAILED BUILD_TIMEOUT / PIPELINE_TIMEOUT; report().build shows
// the stage reached, per-stage times, lastError and the stage a timeout hit.
// Any failure disables the module (state FAILED, menu " - FAILED"); the stereo pair is never affected: the hook only
// records after every fallible step succeeded, leaves all images in their received layouts and restores the engine state.
// Kill switch: debug.rdrvr.vr.hands=0 at startup (VrSettings.h kill switches, re-checked here): no hook, no worker,
// no Vulkan object. Menu: VR page HANDS ON|OFF (settings().hands()). Debug properties (read once by configure):
//   debug.rdrvr.vr.hands.depth  a = auto (default), 0 = no world test, 1 = sampled world depth, 2 = copied world depth
//   debug.rdrvr.vr.hands.bright brightness percent (default 100, 10..300)
//   debug.rdrvr.vr.hands.gpu    0 = no timestamps (default 1: one timed frame in 16)
//   debug.rdrvr.vr.hands.heldsync 0 = the holding hand from its own XR-frame sample; default 1: the holding hand
//                               is drawn with the VR weapon frame's sample and map, rigid with the engine-drawn gun
// LASER SIGHT (menu, settings().laser(), kill debug.rdrvr.vr.laser 0) is drawn here too, even with HANDS OFF.
// ARC (API in HandsArcApi.h): up to 4 arc channels (LASSO,
// TELEPORT, THROW, DEBUG) are drawn here as camera-facing polyline ribbons with end markers / landing pillars, world-depth tested with a
// per-point tolerance and an x-ray floor, also with HANDS OFF and outside the pose gate (one overlay-source table, the early gate keeps
// the pass alive only while a source has VISIBLE content). Kill debug.rdrvr.vr.arc 0 (Feature::Arc, startup): no arc shaders / ring /
// pipelines, publishArc dropped, arcStatus KILLED; hands / laser / markers unchanged. debug.rdrvr.vr.hands 0 kills the arcs too.
// Arc levers (render thread, re-read every 64 frames): debug.rdrvr.vr.arc.xray 0|1, .arc.width <25..400 %>, .arc.minpx <10..40 tenths of a
// pixel>, .arc.depth 0 NONE for arc-only passes | 1 auto | 2 COPY, .arc.asw 0|1 (solid, dimmer arcs on AppSW-synthesized frames),
// .arc.rects (reserved: phase-2 render bands). Report vr.hands.arc.
#include "HandsMath.h"
#include "HandsHook.h"
#include "HandsArcApi.h"
#include <mutex>
#include <span>
#include <string>

namespace rdrvr::vr::hands {
enum class State : int {Off,Killed,Idle,Building,Ready,Failed};
const char* stateName(State) noexcept;
State state() noexcept;

// XR thread (XrHarness::locateHands, after controllerActions_.read filled grip/aim/grips/triggers).
void pushPose(const runtime::ControllerSample&) noexcept;
// Main thread (HOLSTERS): the physical hand holding a weapon (-1 none, 0 left, 1 right) is drawn as a fist.
void setHeldHand(int hand) noexcept;
// DUAL (main thread, HOLSTERS): bit h = hand h holds an item (the engine item or the phantom second item) -> drawn as a fist.
void setHeldMask(unsigned mask) noexcept;
// Main thread (HOLSTERS): up to MarkerSet::capacity markers in LOCAL of `referenceSpace` (ControllerSample::referenceSpace);
// count 0 hides all. Markers of another reference space (recenter) are not drawn.
void publishMarkers(std::span<const Marker> markers,std::uint64_t referenceSpace,std::uint64_t publishedNs) noexcept;
// Integration: hide the hands while they make no sense (theater, manual theater, loading); default visible.
void setSuppressed(bool) noexcept;
// publishWeapon (engine main thread, PostCameraUpdate): the VR-held weapon of the frame whose eye plan
// has headSampledNs == f.headNs.
// The pass draws the holding hand with f's sample through f.map (rigid with the engine-drawn gun; debug.rdrvr.vr.hands.heldsync 0
// = own XR-frame sample) and, with Laser in f.flags (menu LASER SIGHT ON), the laser from f.muzzle along f.direction for
// f.laserMetres (probe hit or the default length), world-depth tested like the hands, with a dot at its end.
void publishWeapon(const WeaponFrame& f) noexcept;
// DUAL: the same for the phantom (second item in the other hand, VrWeapon phantom placement): held-hand sync of its hand and,
// for a phantom firearm, a second laser. No residual statistics (those stay the engine item's).
void publishPhantomWeapon(const WeaponFrame& f) noexcept;

// Engine bound-state restore after the hands commands. ready(cmd): the engine command buffer's
// state is tracked (else the eye is skipped, nothing recorded). restore(cmd): identity vertex push on the replay push
// layout + emitBoundState(tracked) + dynamicUniformDescriptors.commandStateInvalidated (as the stereo replay tail).
struct EngineRestore {bool (*ready)(VkCommandBuffer) noexcept{};void (*restore)(VkCommandBuffer) noexcept{};};
void setEngineRestore(EngineRestore) noexcept;

void configure(VkPhysicalDevice,VkDevice,std::uint32_t family,std::uint32_t queueIndex,std::mutex* queueMutex) noexcept;
// gameImageDestroyed, before the driver destroy: drops views/framebuffers/descriptor sets on that engine image.
void imageDestroyed(VkDevice,VkImage) noexcept;
// destroyDevice, after the device idle, under the queue mutex (the worker never takes that mutex).
void shutdownAfterIdle(VkDevice) noexcept;
// JSON object for the live report key vr.hands.
std::string report();

// Hook body (exposed for host tests; EngineStereoPass reaches it through eyeHook).
bool recordEye(const EyeCall&) noexcept;
// Host tests: wait (bounded) until the worker finished every requested build; true = nothing pending.
bool waitIdleForTests(unsigned milliseconds) noexcept;
// Host tests: set the debug properties without Android (depth: -1 auto, 0/1/2; bright percent; gpu 0/1).
void setDebugForTests(int depth,int brightPercent,int gpu) noexcept;
// Host tests: build timeout (0 = default 10 s) and a stop-aware stall of the worker in one stage (stageForTests name,
// e.g. "IMAGES"; -1 = none). Applies to the next configure.
void setBuildTimeoutForTests(unsigned milliseconds,int stallStage,unsigned stallMilliseconds) noexcept;
int stageForTests(const char* name) noexcept;
} // namespace rdrvr::vr::hands
