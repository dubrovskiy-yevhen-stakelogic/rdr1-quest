#pragma once
// SNIPER SCOPE: a magnified picture-in-picture lens on the eyepiece of a VR-held sniper rifle (render thread, per eye).
//
// The stock scope (SniperScopeManager, a full-screen 2D overlay) is switched off in VR (GameXr vrNativeScopeParam): it turned the
// presentation into NATIVE_OPTIC = the 2D theater with every VR aim path declined. Instead this pass draws a disc at the
// eyepiece (perpendicular to the barrel) that shows the view along the barrel, magnified.
// Content: no second scene render (a third native replay would cost ~11 ms CPU on the 3S). The aiming eye's own image along
// the barrel is covered by the rifle's scope tube, but the OTHER eye sees that direction past the tube (6 cm of eye
// separation = ~30 deg of parallax at the eyepiece): at that eye's hook the pass copies a small square of its colour around
// the barrel's vanishing point (vkCmdCopyImage, <= 256 x 256 px, same format) into a private image, and every eye draws the
// lens from that copy (mapped by world direction, so head motion between the copy and the draw does not shift the picture).
// The source eye is the one farther from the scope axis; when it is the right eye the left eye shows the previous frame's
// copy (one frame old, still direction-correct). Resolution = the eye's pixels magnified (zoom 4 = 4x softer than the eye).
// The rifle's muzzle can reach the edge of the picture at low zoom (it lies ~3.5 deg off the axis in the other eye).
//
// Threads: main thread publish() (VR weapon tag of a held sniper rifle); render thread configure() every frame, the eye hook,
// imageDestroyed(); any report(). Pipelines are built on the render thread at the first eye call (one small pipeline, a
// one-off hitch of a few ms), not when the rifle comes up. Any Vulkan failure disables the module (state FAILED).
// Properties: debug.rdrvr.vr.scope 0 = off at startup (no hook, no objects). Live (re-read every 64 frames, eye 0):
//   debug.rdrvr.vr.scope.zoom    magnification x10 (default 40 = 4x; 15..150)
//   debug.rdrvr.vr.scope.lens    "fwdCm upCm radiusMm" eyepiece centre: along the barrel from the point of the bore line
//                                nearest to the holding grip (+ = towards the muzzle), above the bore line; disc radius
//                                (default "0 6 18")
//   debug.rdrvr.vr.scope.relief  eye relief in cm the magnification is computed for (default 8)
//   debug.rdrvr.vr.scope.reticle 0 none, 1 duplex (default)
//   debug.rdrvr.vr.scope.bright  brightness percent (default 100)
//   debug.rdrvr.vr.scope.show    0 = no lens (the copy is skipped too) without a restart
//   debug.rdrvr.vr.scope.near    NEAR MASK: source pixels nearer than this many cm from the source eye (the rifle's own bolt,
//                                scope tube, barrel near the vanishing point) fade to the dark glass, soft edge to 1.2 x
//                                (default 150; 0 = off: no depth copy). Needs the eye depth (EyeCall.depth, TRANSFER_SRC usage, a
//                                format the private 256 x 256 depth copy supports); otherwise the lens draws without the mask.
//                                Cost: a <= 256 x 256 depth copy + 4 barriers on the source eye, one gather per lens pixel.
//   debug.rdrvr.vr.scope.full    FULL ZOOM zone "backCm latCm fadeCm fovDeg" (default "6 2.5 2.5 50"), see below.
// FULL ZOOM (menu SCOPE LENS > FULL ZOOM, vr.cfg scopeFullZoom, OFF by default; OFF = the lens above, unchanged): the eye NEARER
// to the scope axis, when it is at most backCm behind the eyepiece (or 4 cm in front: the face pushed into the rifle) and latCm off the
// axis, sees the zoomed picture over (almost) its whole view: a disc of fovDeg half-angle centred on the barrel direction, black
// around it (the scope's surround), the rifle's own ZOOM (content half-angle = fovDeg / zoom; smaller when that content would not fit
// the 512 x 512 copy: at zoom 4 ~46 of the 50 deg fit a 3S eye, at zoom 2 the picture shrinks to ~23 deg). It fades in / out
// over fadeCm (both the distance and the off-axis limits), blended in angles from the eyepiece disc, so the start is the very lens
// that eye saw. The OTHER eye keeps its normal view (+ its own small lens): one eye on the scope and the world in the other is how
// a scope is used with both eyes open, while a full-view picture in both eyes that ignores head rotation is what makes people sick;
// the copy also has to come from that eye's normal image. Cost while the zone is entered: one full-eye draw (one fetch + one depth
// gather per pixel) in that eye and a copy of up to 512 x 512 in the other; nothing is allocated per frame.
// (debug.rdrvr.vr.scope.native 1 = keep the game's full-screen scope: GameXr, main thread.)
// Report vr.scope.
#include "ScopeHook.h"
#include "HandsMath.h"
#include "HandsPass.h"
#include <atomic>
#include <string>

namespace rdrvr::vr::scope {
enum class State : int {Off,Killed,Ready,Failed};
State state() noexcept;
// Main thread (vrWeaponTag): the engine item in the VR hand is a sniper rifle; f = the same frame the hands pass gets.
void publish(const hands::WeaponFrame& f) noexcept;
// Main thread (GameXr vrNativeScopeParam): the stock scope switch. -1 not configured, 0 off by the mod, 1 kept (lever .native 1),
// 2 already set by someone else (left alone), 3 lever 1 but the manager was never initialised (kept off). writes / restores = the mod's.
inline std::atomic<int> nativeScope{-1};
inline std::atomic<std::uint64_t> nativeScopeWrites{},nativeScopeRestores{};
void setEngineRestore(hands::EngineRestore) noexcept;
// Render thread, every frame (idempotent; next to taa::configure): binds the device, reads the kill switch, installs the hook.
void configure(VkPhysicalDevice,VkDevice,std::uint32_t family) noexcept;
// gameImageDestroyed, before the driver destroy: drops views / framebuffers on that engine image.
void imageDestroyed(VkDevice,VkImage) noexcept;
// destroyDevice, after the device idle, under the queue mutex.
void shutdownAfterIdle(VkDevice) noexcept;
// JSON object for the live report key vr.scope.
std::string report();
bool recordEye(const hands::EyeCall&) noexcept;
} // namespace rdrvr::vr::scope
