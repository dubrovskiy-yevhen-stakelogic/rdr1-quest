#pragma once
#include <atomic>
// HUD: the game's own 2D HUD (radar + blips + GPS route, region names, objectives, subtitles,
// prompts, meters) shown in world VR as OpenXR quad layer(s) composited by the runtime. Nothing is drawn twice and no eye
// is re-rendered: the game keeps executing its HUD interval (sagRenderer::RenderFrame grcWorldIdentity .. TouchUI::Render)
// every frame into the stock LEFT eye colour target (1680x1760 B8G8R8A8_SRGB, already copied into the stereo pair at the
// DrawColorPass return, so its later content never reaches the headset). In world stereo frames:
//   render thread  HUD Begin  : GameXr clears that target to transparent (native grcDevice::Clear + VkContext::FlushFrameBuffer,
//                               the existing EngineHudProducer sequence) -> the HUD draws land on (0,0,0,0) = premultiplied RGBA.
//                  HUD End    : the HUD target image + the recording command buffer are remembered (recorded()).
//                  vkQueueSubmit of that command buffer (gameStereoSubmitted): one own command buffer on the SAME queue right behind
//                               it: barrier (tracked layout -> TRANSFER_SRC), LINEAR blit (downscale, debug.rdrvr.vr.hud.scale) into a
//                               3-slot ring image, barrier back to the tracked layout; own fence, NO CPU wait, never a semaphore on
//                               the game's present. try_lock only: a busy ring / mutex drops this HUD frame (the previous one stays).
//   XR thread      xrFrame()  : newest ring slot whose fence is signalled (vkGetFenceStatus, never waits) -> blit into one XR
//                               swapchain [HUD | wrist square] on the XR binding queue (no CPU wait) -> quad layers:
//                               HEAD = whole HUD, lazy yaw-follow in LOCAL (deadzone 20 deg), 10 deg below the eyes;
//                               HAND = head quad without the radar rect (cleared hole) + the radar rect blitted into the wrist square,
//                               shown on the back of the LEFT wrist (grip pose) while looked at (gaze reveal).
// SUBTITLES (stereo cutscenes): GameXr captures a cutscene / transition frame (and 3 s after it) as subtitles only: the consumer copies
// just the subtitle band of the Flash area and shows it as one quad below the view (lazy yaw follow like HEAD), in HEAD and HAND,
// with no panel / radar / wrist. The letterbox bars are dropped in the VR HUD interval (TraceEntry LETTERBOX).
//   debug.rdrvr.vr.hud.subtitles 0 = no capture in cutscenes      debug.rdrvr.vr.hud.subband "x0,y0,x1,y1" per mille of the HUD
//   image (default 0,250,818,409 = bottom of the 1374x720 Flash area)      debug.rdrvr.vr.hud.subdown degrees below the eyes (20)
// Fail closed: any mismatch (fingerprinted APIs, source format, layout, extent, frame identity) leaves the game's HUD interval
// untouched for that frame; FAILED latches only on Vulkan/XR object failures. Kill switch debug.rdrvr.vr.hud 0 (startup): the
// HUD row is OFF - PROP, no clear, no copy, no layer. Theater / menus / loading: no HUD layer at all
// (the layers are only added next to a world stereo projection; the clear is only done in RenderScenes that recorded a pair).
// Properties (re-read every 64 XR frames):
//   debug.rdrvr.vr.hud.scale   ring/HUD resolution in % of the source (default 50, 25..100)
//   debug.rdrvr.vr.hud.every   capture every Nth world frame (default 1 = every frame, 1..4)
//   debug.rdrvr.vr.hud.alpha   0 (default) native HUD alpha, 1 = coverage alpha per draw (HudAlphaFlush, costs CPU per HUD draw)
//   debug.rdrvr.vr.hud.radar   "x0,y0,x1,y1" per mille of the HUD image (default 10,660,350,990 = bottom-left radar + health ring)
//   debug.rdrvr.vr.hud.deg     HEAD quad width in degrees (overrides HUD SIZE: SMALL 40 / MEDIUM 50 / LARGE 62)
//   debug.rdrvr.vr.hud.dist    HEAD quad distance in cm (default 140)       debug.rdrvr.vr.hud.down  degrees below the eyes (10)
//   debug.rdrvr.vr.hud.follow  1 (default) lazy yaw follow, 0 = head-locked
//   debug.rdrvr.vr.hud.wrist   "along,lift,size" mm (default 130,-15,100): along = behind the grip origin towards the elbow (hand forward
//                              = aim forward in grip (0,-.71,-.71), the VrArmIk convention), lift = out of the BACK of the left hand
//                              (grip -X; +X leaves the left palm)                                     debug.rdrvr.vr.hud.gaze  1 / 0 always
//   debug.rdrvr.vr.hud.mapfix "sx0,sy0,sx1,sy1,dx0,dy0,dx1,dy1" per mille of the HUD image (empty = off): content drawn at the
//                              src rect (the minimap interior, drawn by hudStreamingMap::DrawMap through grcViewport::sm_ScreenViewport
//                              whose window is not the eye target's) is cut out of the head image and shown as its own quad over the
//                              dst rect (HEAD: on the head quad; HAND: on the wrist radar). Live.
//   debug.rdrvr.vr.hud.dump N  (any new value) writes the next ring image to rdrvr/hud-dump.raw + hud-dump.json (BGRA/RGBA 8-bit).
//   debug.rdrvr.vr.hud.maxage  ms after which a HUD image is not shown any more (default 400)
//   debug.rdrvr.vr.hud.wristtilt  % of the remaining angle the wrist map also tilts towards the eyes (default 70, 0 = forearm roll only)
//   debug.rdrvr.vr.hud.wristfacing 1 (default): the wrist map fades in while the forearm lies across the view (watch check), 2 = while
//                              its skin side faces the eyes, 0 = gaze only (all on top of the gaze cone)
// AMMO ON GUN (menu HUD > AMMO ON GUN, default ON; any HUD mode but OFF): the held firearm's clip / pool ("6/45", the engine ammo VrReload
// reads) as a small strip lying along the gun on the OUTER side of the holding hand (the Vice City VR wrist AMMO panel idea), read across
// the gun with its top towards the muzzle, turned to the eyes. No render pass: the digits (5x7 font, CPU) go into a device buffer by
// vkCmdUpdateBuffer when the text changes and are copied into a strip of the HUD swapchain by the consume command buffer.
//   debug.rdrvr.vr.hud.ammo     "along,across,lift,width" mm (default 50,45,0,60) from the bore point level with the drawn model origin:
//                              along = towards the muzzle, across = outwards (right of a right-hand gun), lift = gun up
//   debug.rdrvr.vr.hud.ammotilt % of the remaining angle the strip tilts towards the eyes (default 50)
//   debug.rdrvr.vr.hud.ammoface 1 (default) turned about the barrel to the eyes, 0 = flat on the gun's top plane
//   debug.rdrvr.vr.hud.ammocrop "x0,y0,x1,y1" per mille of the HUD image (empty = off): the stock ammo element is cut out of the head
//                              image and shown on the gun instead of the digits (live)
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <array>
#include <cstdint>
#include <string>

namespace rdrvr::vr::hud {
enum Mode : unsigned {Off=0,Head=1,Hand=2};
enum class State : int {Off,Killed,Idle,Ready,Failed};
const char* stateName(State) noexcept;
State state() noexcept;
// Menu HUD value with the kill switch applied (Off while killed / failed).
unsigned mode() noexcept;
// Cutscene subtitles wanted: HUD row not OFF, SUBTITLES not OFF, debug.rdrvr.vr.hud.subtitles not 0.
bool subtitlesWanted() noexcept;

// ---- render thread (GameXr) ----
// Session device (startSession). No Vulkan object is created here.
void bind(VkPhysicalDevice physical,VkDevice device,std::uint32_t family) noexcept;
// HUD Begin: should this world frame be captured at all (mode, kill switch, FAILED latch, every-N)? Counts the frame.
bool wanted(std::uint64_t renderFrameId) noexcept;
enum class Skip : unsigned {NoWorld,NotWorld,Apis,Source,Binding,Format,Clear,Ring,Busy,Layout,Lock,Alpha,Count};
void skip(Skip why) noexcept;
// The cleared target of this HUD interval (after the transparent clear was verified applied).
struct Source {VkImage image{};std::uint64_t generation{};VkExtent2D extent{};VkFormat format{};VkImageUsageFlags usage{};std::uintptr_t texture{},framebuffer{};std::uint64_t frame{};
    bool subtitles{};}; // subtitles: a stereo cutscene frame (only the subtitle band is shown)
void begun(const Source&) noexcept;
bool active() noexcept;                  // between begun() and recorded()/cancel() on this frame
const Source& activeSource() noexcept;   // render thread only
// HUD End: the command buffer that holds the HUD draws (submitted later on the game queue).
void recorded(VkCommandBuffer command) noexcept;
void cancel() noexcept;
// Game vkQueueSubmit (after the native submit returned). readLayout reads the engine-tracked layout of the source texture
// (texture+0xa0) and confirms the image registry generation; false = drop this HUD frame.
using LayoutReader=bool(*)(std::uintptr_t texture,VkImage image,std::uint64_t generation,VkImageLayout& out) noexcept;
void submitted(VkQueue queue,std::uint32_t count,const VkSubmitInfo* submits,VkResult result,LayoutReader readLayout) noexcept;
// RenderFrame's PIX "ClearAlpha" pass right after TouchUI writes alpha = 1 over the whole HUD target (colour write A only, a
// fullscreen triangle). TraceEntry hooks PIXBegin / PIXEnd (exact returns 0x2a58a28 / 0x2a58bc0) and drops that draw through the
// postfx bind-only path for a recorded VR HUD frame. Without both hooks the VR HUD never clears (mode stays effective, frames skip).
void setClearAlphaHooked(bool) noexcept;
bool clearAlphaHooked() noexcept;
void noteClearAlphaFingerprint(bool) noexcept;
bool takeClearAlpha() noexcept;                 // PIXBegin ClearAlpha: drop it for the pending HUD frame?
void clearAlphaDone(std::uint64_t dropped) noexcept; // PIXEnd ClearAlpha: draws dropped (0 = not dropped -> the frame is cancelled)

// ---- optional coverage alpha (debug.rdrvr.vr.hud.alpha 1): every HUD draw flush to the cleared target ----
struct AlphaApi {
    bool (*boundFramebuffer)(std::uintptr_t& out) noexcept{}; // gfx+0x158 of the render thread's GfxContext
    bool (*invoke)(void (*original)()) {};                    // HudAlphaFlush::invoke(true); false = unsupported state
};
void configureAlpha(AlphaApi) noexcept;
bool alphaWanted() noexcept;
void alphaBegin() noexcept;              // after begun()
void alphaEnd() noexcept;
inline std::atomic<unsigned> alphaLiveCount{}; // Inline: read on every FlushRenderState
inline bool alphaLive() noexcept {return alphaLiveCount.load(std::memory_order_relaxed)!=0;} // one relaxed load (hot path of every engine draw flush)
void alphaFlush(void (*original)(),bool colorDraw);

// ---- AMMO ON GUN (main thread: GameXr vrWeaponTag, right after hands::publishWeapon) ----
// The VR-held weapon of this frame: map = LOCAL metres -> world of its placement (uniform scale), muzzle / dir / up / modelOrigin in world
// (up or modelOrigin zero = unknown), grip / aim orientation of the holding controller in LOCAL, hand 0 left / 1 right; clip / pool = the
// engine ammo of that weapon (-1 = unknown), infinite = unlimited pool. Kept in LOCAL for the XR thread (shown while < 250 ms old).
void noteGun(const float map[16],const float muzzle[3],const float dir[3],const float up[3],const float modelOrigin[3],
             const float gripLocal[3],const float aimLocalQ[4],int hand,bool firearm,int clip,int pool,bool infinite) noexcept;

// ---- XR thread (XrHarness::frame, inside the world stereo branch; privateQueueMutex held) ----
struct Pose {float q[4]{0,0,0,1};float p[3]{};bool valid{};};
struct Layers {XrCompositionLayerQuad head{XR_TYPE_COMPOSITION_LAYER_QUAD},wrist{XR_TYPE_COMPOSITION_LAYER_QUAD},map{XR_TYPE_COMPOSITION_LAYER_QUAD},
    ammo{XR_TYPE_COMPOSITION_LAYER_QUAD}; // AMMO ON GUN: the clip / pool strip beside the held firearm
    bool headValid{},wristValid{},mapValid{},ammoValid{};
    // The game draws the minimap at 50 % alpha (stock: translucent over the world). On the wrist it is made opaque by a
    // colour scale (premultiplied RGBA x wristAlphaScale, clamped by the compositor; corners at alpha 0 stay clear). XrHarness chains it
    // only when XR_KHR_composition_layer_color_scale_bias is enabled and no teleport blink uses the slot.
    float wristAlphaScale{1.f};};
// views = the two located eye poses in `local`; leftGrip = the left controller grip pose in `local` (may be invalid).
bool xrFrame(XrSession session,VkQueue queue,std::uint32_t queueFamily,XrSpace local,const Pose views[2],const Pose& leftGrip,
             std::uint64_t nowNs,Layers& out) noexcept;
// XrHarness destructor, after gpu_.waitIdle(), before xrDestroySession.
void xrSessionEnded() noexcept;
// Device destroy, after the device went idle (privateQueueMutex held).
void shutdownAfterIdle(VkDevice) noexcept;
std::string report();
}
