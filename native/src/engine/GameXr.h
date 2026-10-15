#pragma once
#include <jni.h>
#include <string>
#include "diagnostics/VulkanTrace.h"
#ifdef RDRVR_STEREO_BRIDGE
#include "runtime/StereoMailbox.h"
#include "runtime/GameStereoCapture.h"
#include "EyeViewports.h"
#endif
#ifdef RDRVR_PASS_RESOURCES
#include "PassResources.h"
#include "EngineTargets.h"
#include "CommandResources.h"
#include "EngineFramebuffers.h"
#include "EngineStereoPass.h"
#include "EngineDepthResolve.h"
#include "EngineTextureScope.h"
#include "EngineTextureMap.h"
#include "TextureCallBoundary.h"
#include "TargetCallBoundary.h"
#include "SceneCallBoundary.h"
#include "CullViewport.h"
#include "CullViewportPool.h"
#include "ReplayUpdates.h"
#include "EngineGlobalState.h"
#include "RetailSceneBoundary.h"
#include "PrimaryEyeBoundary.h"
#include "EyeShaftBoundary.h"
#include "RetailSceneDraw.h"
#include "ReplayRenderPhase.h"
#include "ReplayGrassVisibility.h"
#include "RetailResources.h"
#include "HudResources.h"
#include "HudCallBoundary.h"
#include "HudFrameBoundary.h"
#include "HudNativeRouting.h"
#include <memory>
#endif
#ifdef RDRVR_TOUCH_CONTROLS
#include "NativePadDriver.h"
#include "NativeRumble.h"
#include "LocomotionContextReader.h"
#include "WeaponOwnerReader.h"
#include "ReticleProbeReader.h"
#include "WeaponAimBoundary.h"
#include "WeaponPoseCallBoundary.h"
#include "WeaponModelCallBoundary.h"
#include "WeaponModelBindingReader.h"
#include "WeaponManagerBoundary.h"
#include "WeaponFireBoundary.h"
#include "ProjectileLaunchReader.h"
#include "NativeReticleUiReader.h"
#include "ReticleUiCallBoundary.h"
#include "BodyDrawFilter.h"
#include "BodyAttachmentReader.h"
#endif
#ifdef RDRVR_NATIVE_MENUS
#include "NativeUiContext.h"
#include "NativeCameraTurn.h"
#include "runtime/GameTheater.h"
#endif
namespace rdrvr::engine {
#ifdef RDRVR_NATIVE_MENUS
void gameConfigureUi(NativeUiApi) noexcept;
bool gameCaptureHud(VkQueue,const runtime::CompletedQuadImage&,std::uint64_t nowNs) noexcept;
#endif
#ifdef RDRVR_TOUCH_CONTROLS
void gameConfigureInput(NativePadApi) noexcept;
void gameConfigureRumble(NativeRumbleApi) noexcept;
void gameObserveRumble(void*,int,float,std::uint64_t) noexcept;
void gameConfigureWeaponObservation(WeaponOwnerApi) noexcept;
void gameConfigureReticleProbe(ReticleProbeApi) noexcept;
void gameConfigureWeaponModel(WeaponModelBindingApi,bool) noexcept;
void gameNativeReticleReady(bool) noexcept;
void gameConfigureReticleUi(NativeReticleUiReader::Api,bool) noexcept;
void gameObserveReticleUi(void*,bool,std::uint64_t) noexcept;
void gameConfigureProjectileLaunch(ProjectileLaunchReader::Api,bool) noexcept;
void gameObserveProjectileLaunch(const ProjectileLaunchCall&,const WeaponFireScope&,bool,std::uint64_t) noexcept;
bool gamePrepareWeaponModel(const WeaponModelCall&,WeaponNativeMatrix&,std::uint64_t) noexcept;
void gameWeaponModelComplete(const WeaponModelCall&,bool,bool) noexcept;
void gameObserveReticle(void*,std::uint64_t,std::uint64_t) noexcept;
void gameObserveWeaponManager(void*,bool,std::uint64_t,std::uint64_t) noexcept;
void gameObserveWeaponFire(const WeaponFireCall&,bool,std::uint64_t,std::uint64_t) noexcept;
void gameObserveWeaponAim(const WeaponAimCall&,std::uint64_t,std::uint64_t) noexcept;
bool gamePrepareWeaponAim(const WeaponAimCall&,WeaponAimVector&,std::uint64_t) noexcept;
void gameWeaponAimComplete(bool) noexcept;
void gameObserveWeaponPose(const WeaponPoseCall&,std::uint64_t,std::uint64_t) noexcept;
bool gamePrepareWeaponPose(const WeaponPoseCall&,WeaponNativeMatrix&,std::uint64_t) noexcept;
void gameObserveWeaponPoseComplete(const WeaponPoseCall&,bool,std::uint64_t,std::uint64_t) noexcept;
void gameUpdateInput(void*,std::uint64_t) noexcept;
void gameAfterUpdateInput(void*,std::uint64_t) noexcept;
#ifdef RDRVR_HEAD_CAMERA
bool gameNativeFramePacingAllowed() noexcept;
bool gameWorldPresentBypass(VkQueue) noexcept;
bool gameWorldPresentOnly(VkQueue) noexcept;
bool gameEyeResolutionCoordinatesReady() noexcept;
bool gameEyeResolutionWorldActive() noexcept;
void gameConfigureLocomotion(LocomotionApi) noexcept;
#ifdef RDRVR_NATIVE_MENUS
void gameConfigureSnapTurn(NativeCameraTurnApi) noexcept;
#endif
void gameConfigureBodyAttachments(BodyAttachmentApi) noexcept;
void gameConfigureBodyFilter(BodyDrawFilter::Original,BodyDrawFilter::Reader) noexcept;
void gameBodyFilterHooksReady(bool) noexcept;
void gameBodyDraw(void*,const void*,int,int,float,unsigned,unsigned,bool);
#endif
#endif
#ifdef RDRVR_PASS_RESOURCES
void gameObserveHud(HudStage,HudPhase) noexcept;
void gameObserveHudFrame(HudFrameEvent,void*) noexcept;
void gameConfigureHudRender(HudRenderEntry) noexcept;
void gameConfigureGlobals(EngineGlobalApi) noexcept;
void gameConfigureReplayPhase(ReplayRenderPhaseApi) noexcept;
ReplayGrassVisibility& gameGrassVisibility() noexcept;
std::unique_ptr<EngineGlobalState> gameCreateGlobalState() noexcept;
void gameConfigureRetailResources(RetailResourceApi) noexcept;
void gameConfigureRetailDraw(RetailDrawApi) noexcept;
WorldDrawProfile& gameWorldProfile() noexcept;
void gameProfileHooksReady(bool) noexcept;
void gameProfileDraw(void*,void*,const void*,unsigned,unsigned,unsigned,bool,bool,unsigned);
bool gameDrawRetailWorld(const RetailSceneFrame&);
PrimaryEyeBoundary& gamePrimaryEye() noexcept;
EyeShaftBoundary& gameEyeShaft() noexcept;
void gamePrimaryNativeFramebufferBound(void*,void*,bool) noexcept;
void gamePrimaryHooksReady(bool) noexcept;
void gamePrimaryDepth(RetailSceneBoundary::Draw,void*,void*,bool);
void gamePrimaryColor(RetailSceneBoundary::Draw,void*,void*,bool);
void gamePrimaryLights(RetailSceneBoundary::Lights,void*,const void*,bool);
void gameConfigureLodDiagnostics(PassSourceAbi::Reader,void*,std::uintptr_t) noexcept;
void gameConfigurePassSource(PassSourceAbi) noexcept;
void gameConfigureCullViewport(CullViewportApi) noexcept;
std::unique_ptr<CullViewport> gameCreateCullViewport() noexcept;
void gameCullHooksReady(bool) noexcept;
void gameEnableWorldCulling(bool) noexcept;
void gameConfigureWorldVisibility(bool(*)() noexcept,bool(*)() noexcept) noexcept;
bool gameWorldVisibilityValid() noexcept;
bool gameGraphicsEyeQualified(const void* viewport=nullptr) noexcept;
void gameQualifyVisibility(const void*,const void*,bool) noexcept;
const void* gamePrepareCull(std::uintptr_t renderer,std::uint64_t serial,const void* viewport) noexcept;
CullViewportPool::Borrow gameBorrowCull(const void* viewport) noexcept;
void gameDrawFinished(void* renderThread) noexcept;
ReplayUpdates& gameReplayUpdates() noexcept;
void gameReplayHooksReady(bool) noexcept;
bool gameReplayReady() noexcept;
void gameReplayTreeTimerBegin();
bool gameReplayTreeTimerEnd();
void gameReplayBlend(void*);
void gameReplayIndoor(void*);
void gameReplayCharacter(void*);
bool gameReplayCloud(void*,const void*,bool,void**);
std::uint64_t gameBeginVisibility(std::uintptr_t renderer,std::uint32_t marker,std::uintptr_t viewport,std::uintptr_t camera,std::uint64_t ns,
                                bool(*)(void*,std::uintptr_t,void*,std::size_t),void*);
void gameFinishVisibility(std::uint64_t,bool) noexcept;
void gameObserveFrozenCamera(std::uintptr_t renderer,std::uint32_t marker,std::uintptr_t viewport,std::uint64_t ns,
                             bool(*)(void*,std::uintptr_t,void*,std::size_t),void*);
void gameConfigureEngineTargets(EngineTargetApi) noexcept;
void gameConfigureFramebuffers(EngineFramebufferApi) noexcept;
void gameConfigureDepthCopy(EngineDepthCopyApi) noexcept;
void gameConfigureTextures(EngineTextureApi) noexcept;
void gameTextureHooksReady(bool) noexcept;
void gameTextureGlobal(int,const void*);
void gameTextureInstance(const void*,void*,int,const void*,bool,bool);
bool gameWithTextureRoutes(EngineTextureMap&,TextureCallBoundary::Body,void*);
void gameConfigureTargetBindings(EngineTargetBindingApi) noexcept;
void gameTargetHooksReady(bool) noexcept;
void gameTargetLock(void*,int,const void*,const void*,unsigned,bool,int,unsigned,bool);
void gameTargetUnlock(void*,int,const void*,int,bool);
void gameTargetResolveDepth(void*,bool);
bool gameWithTargetRoutes(EnginePassBinding&,EngineTextureMap&,const EngineEyeTargets&,void*,unsigned,EngineTargetRole,TargetCallBoundary::Body,void*,EngineDepthRoute={});
bool gameWithSceneRoutes(EngineTextureMap&,std::span<const StockSceneTarget>,EnginePassBinding&,const EngineEyeTargets&,
                         void*,unsigned,EngineTargetRole,EngineDepthRoute,TargetCallBoundary::Body,void*);
std::unique_ptr<EngineTextureScope> gameCreateTextureScope(EngineTextureScope::Map,void*) noexcept;
std::unique_ptr<EngineTextureMap> gameCreateTextureMap(EngineEyeTargets&) noexcept;
std::unique_ptr<EngineDepthResolve> gameCreateDepthResolve(EngineEyeTargets&) noexcept;
std::unique_ptr<EngineEyeTargets> gameCreateEyeTargets(void* factory) noexcept;
std::unique_ptr<EngineFramebuffers> gameCreateFramebuffers(EngineEyeTargets&,void* gfxDevice) noexcept;
std::unique_ptr<EnginePassBinding> gameCreatePassBinding(EngineFramebuffers&) noexcept;
std::unique_ptr<EngineStereoPass> gameCreateStereoPass(EngineEyeTargets&,EngineFramebuffers&) noexcept;
CommandResources& gameCommands() noexcept;
std::uint64_t gameCommandRecordingId(VkDevice,VkCommandBuffer) noexcept;
bool gameRecordEngineEye(unsigned,std::uintptr_t target,std::uintptr_t context) noexcept;
void gameImageCreated(VkDevice,const VkImageCreateInfo*,VkImage,VkResult) noexcept;
void gameImageDestroyed(VkDevice,VkImage) noexcept;
bool gameReadPassSource(std::uintptr_t,std::uintptr_t,PassSource&) noexcept;
#endif
void gameXrInitialize(JavaVM*,jobject) noexcept;
bool gameXrRoute(diagnostics::TraceDispatch&,PFN_vkGetInstanceProcAddr) noexcept;
std::string gameXrReport();
#ifdef RDRVR_CAMERA_BRIDGE
void gameObserveCamera(std::uintptr_t,std::uint64_t,std::uint64_t,
                       bool(*)(void*,std::uintptr_t,void*,std::size_t),void*);
#endif
#ifdef RDRVR_STEREO_BRIDGE
void gameConfigureViewports(ViewportApi) noexcept;
void gameSceneFinished() noexcept;
EyeViewports* gameEyeViewports() noexcept;
void gameStereoSubmitted(VkQueue,std::uint32_t,const VkSubmitInfo*,VkResult) noexcept;
void gameStereoBeforeSubmit(VkQueue,std::uint32_t count=0,const VkSubmitInfo* submits=nullptr) noexcept;
bool gameBeginStereoPasses(VkQueue,const runtime::CaptureFrame&) noexcept;
bool gameRecordStereoEye(VkCommandBuffer,unsigned,const runtime::PassImage&) noexcept;
bool gameCaptureStereo(VkQueue,const RenderSubmission&,runtime::SourceEncoding) noexcept;
#endif
#ifdef RDRVR_GAME_SESSION
VkResult gamePresentVia(VkQueue,const VkPresentInfoKHR*,PFN_vkQueuePresentKHR);
void gameTheaterRoute(PFN_vkCreateSwapchainKHR,PFN_vkDestroySwapchainKHR,PFN_vkQueuePresentKHR);
VKAPI_ATTR VkResult VKAPI_CALL gameCreateSwapchain(VkDevice,const VkSwapchainCreateInfoKHR*,const VkAllocationCallbacks*,VkSwapchainKHR*);
VKAPI_ATTR void VKAPI_CALL gameDestroySwapchain(VkDevice,VkSwapchainKHR,const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL gamePresent(VkQueue,const VkPresentInfoKHR*);
#endif
}
