#include "CategoryAblation.h" // ablate::appendSignature (gpuStages.byState _abb / _ab<k>)
#include "WorkerLayerIntegration.h"
#include "OrdinaryReplayFrame.h"
#include "OrdinaryReplayContext.h"
#include "OrdinaryMainStartupRequest.h"
#include "UniformIntegration.h"
#include "OrdinaryReplayIntegration.h"
#include "PipelinePassHashMemo.h"
#include "UnusedDrawLabel.h"
#ifdef RDRVR_PASS_RESOURCES
#include "NativeEarlyImageRegistry.h"
#endif
#ifdef RDRVR_HEAD_CAMERA
#include "DynamicUniformDescriptors.h"
#include "NativeEarlyRuntimeBinding.h"
#include "NativeEarlyTraceRoute.h"
#include "NativeEarlySceneLifetime.h"
#include "NativeEarlyTargets.h"
#include "NativeEarlyPassTargets.h"
#include "NativeEarlyPaletteBootstrap.h"
#include "NativeEarlyTelemetry.h"
#include "EarlyWork45Publication.h"
#include "RenderAblation54.h"
#include "InstanceFrequency67.h"
#include "NativeModelProbe69.h"
#include "opaque-run/NativeEarlyColdOwners.h"
#include "GameEffectMutation.h"
#include "NativeOcclusionVertexCapture.h"
#include "NativeFramePacing.h"
#include "NativeEyeResolution.h"
#include "NativeFramebufferExtent.h"
#include "NativePrimaryRaster.h"
#include "EyeShaftBoundary.h"
#endif
#include "NativeDescriptorReuse.h"
#include "NativeDrawSuppression.h"
#include "diagnostics/PerformanceProfile.h"
#include "WorldReplayGate.h"
#include <map>
#include "GameXr.h"
#include "StereoReplay.h"
#include "EyeDump.h" // eyeDumpAutoRequest (debug.rdrvr.eyedump.cutscene)
#include "vr/VrControlMap.h" // BUTTONS: Touch button remap before the VrControls mapper
#include "StereoReplayInternal.h" // HANDS: trackedState / emitBoundState / replayPushLayout for the hands restore
#include "vr/HandsPass.h"
#include "vr/TaaPass.h"
#include "vr/VrHud.h"             // HUD: stock HUD target -> OpenXR quad layers
#include <algorithm>
#include <cmath>
#include "GraphicsDrawPolicy.h"
#include "PostFxSkip.h" // STEREO TEST 16 needs to know that this RenderScene recorded the stereo pair
#include "TargetRouteGuard.h" // Reports gameBootstrap.targetBoundary
#include "ShadowUpdate.h" // indoor::flag() (IndoorState.h), shadowupdate::cascadeCullEffective()
#include "EngineTextureFilter.h"
#include "LampShadows.h" // lampshadow::signatureCode() (_ls suffix)
#include "S02Levers.h"         // _x<effective lever mask>[t] suffix, per-frame gates
#include "EngineColorRestart.h" // CR gate (colorrestart::usable)
#include "runtime/GraphicsSettings.h"
#include "runtime/GameQueues.h"
#include "runtime/GraphicsResolution.h"
#ifdef RDRVR_FRAGMENT_RATE
#include "GameFragmentRate.h"
#endif
#include "GameFoveation.h"
#ifdef RDRVR_BENCH_CONTROL
#include "runtime/BenchMenuInput.h"
#endif
#include "LodDiagnostics.h"
#include "diagnostics/FrameTimings.h"
#include "diagnostics/ThreadCpuScope.h"
#include "diagnostics/JsonNumber.h"
#ifdef RDRVR_TOUCH_CONTROLS
#include "ControllerAim.h"
#include "WeaponContextReader.h"
#include "WeaponCallTrace.h"
#include "WeaponPoseGeometry.h"
#include "HeadRelativeMovement.h"
#include "ControllerAimTarget.h"
#include "ControllerWeaponAttachment.h"
#include "WeaponSkeletonAudit.h"
#include "AnimatedControllerAttachment.h"
#include "NativeReticleVisibility.h"
#include "AimTargetReticle.h"
#include "SnapTurn.h"
#include "Teleport.h" // Rendered view yaw publish + restore
#include "vr/VrLocomotion.h" // LOCO: onStick (mapMovement), menu status
#include "vr/VrHolsters.h"   // HOLSTERS: pad filter, weapon-hand substitution, HANDS bridge
#include "vr/VrWeapon.h"     // WEAPON: weapon in the VR hand (pose / model / aim), report vr.weapon
#include "vr/VrControls.h"   // Controls: Touch button layout, sprint/crouch/jump, fast jump counters
#include "vr/VrMelee.h"      // MELEE: LOCAL -> world map provider (vrMeleeWorldMap)
#include "vr/VrBody.h"       // BODY: frame provider (vrBodyFrame), body visible in the eyes, weapon pose reuse / native hold
#include "vr/VrCutscene.h"   // CUTSCENES: stereo cutscenes (composer, engaged state, John facts, report vr.cutscene)
#include "vr/VrArc.h"        // ARC: Context providers (anchor / map / raw input), engine probe binding, probe ledger, ModuleArc
#include "vr/VrTeleportLoco.h" // TELEPORT: stick route (mapMovement), controls / holsters neutral stick, colour-scale extension
#include "vr/VrLasso.h"      // LASSO: face remap / minigame routing (controls), minigame focus (sticks, snap turn), marker flags
#if __has_include("vr/VrReload.h")
#include "vr/VrReload.h"
#endif
#endif
#include "runtime/GameVulkanBridge.h"
#include "runtime/XrPlatform.h"
#include "runtime/XrPerformance.h"
#include <unistd.h>
#include <memory>
#include <mutex>
#include <optional>
#include <cctype>
#include <sstream>
#include <vector>
#include <cstring>
#ifdef RDRVR_CAMERA_BRIDGE
#include "SceneCameraBridge.h"
#include "HeadAnchorReader.h"
#include "vr/VrCamera.h" // CAMERA: body anchor + roomscale rig
#ifdef RDRVR_NATIVE_HUD
#include "EngineHudProducer.h"
#endif
#ifdef RDRVR_PASS_RESOURCES
#include "VisibilityCameraBridge.h"
#endif
#endif
#ifdef RDRVR_GAME_SESSION
#include "runtime/XrHarness.h"
#include "runtime/FramePacing.h"
#include "runtime/GameTheater.h"
#include "runtime/GameHudCapture.h"
#include <thread>
#include <atomic>
#include <chrono>
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h> // Screen viewport lever
#endif
namespace rdrvr::engine {
runtime::PerformanceSupport performanceSupport;
namespace {
std::mutex mutex;
JavaVM* javaVm{};jobject activity{};
XrInstance xr{};XrSystemId system{};
bool attempted{};
const char* state="NOT_INITIALIZED";
XrResult xrResult=XR_SUCCESS;
#ifdef RDRVR_HEAD_CAMERA
XrResult eyeRecommendationResult=XR_ERROR_INITIALIZATION_FAILED;
std::uint32_t eyeRecommendationCount{};
#endif
std::unique_ptr<runtime::VulkanBootstrap> bootstrap;
std::unique_ptr<runtime::GameVulkanBridge> bridge;
#ifdef RDRVR_GAME_SESSION
// XR thread never takes `mutex`; destruction may join it while holding that lock.
runtime::GameTheater theater;
#ifdef RDRVR_TOUCH_CONTROLS
runtime::ControllerMailbox controllers;NativePadDriver nativePad(controllers);
runtime::HapticMailbox haptics;NativeRumbleApi rumbleApi{};
std::atomic<std::uint64_t> hapticApplied{},hapticStopped{},hapticFailed{},hapticDeclined{};
ControllerAimSpace controllerAimSpace;
WeaponOwnerApi weaponObservationApi{};
WeaponCallTrace weaponCallTrace;
thread_local WeaponPoseGeometryAudit poseGeometryAudit;
thread_local std::uint64_t poseGeometryOrdinal{},poseGeometryRevision{};
std::atomic<std::uint64_t> poseGeometryBegun{},poseGeometryCompleted{},poseGeometryDeclined{},poseGeometryAnimated{};
std::mutex poseGeometryMutex;WeaponPoseGeometryDelta poseGeometryLast{};std::uint64_t poseGeometrySampleNs{};
thread_local ControllerWeaponAttachment controllerAttachment;
thread_local ControllerAttachmentPlan pendingAttachment;
thread_local AnimatedControllerAttachment animatedAttachment;
thread_local AnimatedAttachmentPlan pendingAnimatedAttachment;
thread_local ScopedWeaponModelPose scopedModelPose;
thread_local bool pendingAttachmentAnimated{};
WeaponModelBindingApi weaponModelApi{};std::atomic<bool> weaponModelReady{};
std::atomic<std::uint64_t> animatedAttachmentPrepared{},animatedAttachmentCompleted{};
thread_local bool pendingAttachmentLocal{},pendingAttachmentPrepared{};
std::atomic<const char*> attachmentState{"WAITING_FOR_NATIVE_UPDATE"};
std::atomic<std::uint64_t> attachmentPrepared{},attachmentCompleted{},attachmentDeclined{},attachmentPendingFire{},attachmentClamped{};
std::mutex attachmentMutex;std::uint64_t attachmentStamp{};std::uint32_t attachmentGuid{};
runtime::Vec3 attachmentMuzzle{};bool attachmentAnimated{};
ProjectileLaunchReader::Api projectileLaunchApi{};std::atomic<bool> projectileLaunchReady{};
struct LaunchRecord {
    ProjectileLaunchSample input{};std::uint64_t stamp{},completedNs{},fireSerial{},revision{};
    bool completionRead{},argumentsCopied{},ownerStillCurrent{},contextAvailable{},controllerTargetAvailable{},controllerTargetTracked{};
    std::uint8_t nativeFlags{};std::uint32_t contextFlags{};runtime::Vec3 controllerTarget{};
};
struct PendingLaunch {LaunchRecord record{};LocalWeapon owner{};void* projectile{};bool valid{};};
thread_local PendingLaunch pendingLaunch;
std::mutex launchMutex;std::array<LaunchRecord,8> launchRecords{};unsigned launchNext{},launchUsed{};
std::atomic<std::uint64_t> launchCaptured{},launchCompleted{},launchDeclined{},launchOverwritten{};
NativeReticleUiReader::Api reticleUiApi{};std::atomic<bool> reticleUiReady{};
struct ReticleUiRecord {NativeReticleUiSample before{},after{};std::uint64_t stamp{},completedNs{};bool completed{};};
thread_local ReticleUiRecord pendingReticleUi;thread_local bool pendingReticleUiValid{};
std::mutex reticleUiMutex;ReticleUiRecord latestReticleUi{};
std::atomic<std::uint64_t> reticleUiCalls{},reticleUiCaptured{},reticleUiCompleted{},reticleUiDeclined{};
struct SkeletonAuditRecord {WeaponSkeletonEvidence evidence{};std::uint64_t stamp{},samples{},declined{},coherent{};std::uint32_t actorGuid{};};
std::mutex skeletonAuditMutex;std::array<SkeletonAuditRecord,3> skeletonAuditRecords{};
HeadCameraMailbox headCamera;
std::atomic<const char*> headCameraState{"WAITING_FOR_LOCAL_HEAD"};
std::atomic<std::uint64_t> headCameraSamples{},headCameraRejected{},headCameraRecorded{};
#ifdef RDRVR_HEAD_CAMERA
LocomotionApi locomotionApi{};MovementMappingLatch locomotionLatch;
std::atomic<const char*> locomotionState{"NOT_CONFIGURED"};
std::atomic<std::uint64_t> locomotionMapped{},locomotionUnavailable{},locomotionNative{};
bool mapMovement(void*,const runtime::ControllerSample&,std::uint64_t,std::array<float,2>&);
#ifdef RDRVR_NATIVE_MENUS
NativeCameraTurnApi snapTurnApi{};SnapTurnLatch snapTurnLatch;
std::atomic<bool> snapTurnReady{};
std::atomic<const char*> snapTurnState{"NOT_CONFIGURED"};
std::atomic<std::uint64_t> snapTurnApplied{},snapTurnDeclined{},smoothTurnFrames{};
bool mapHorizontalLook(void*,const runtime::ControllerSample&,std::uint64_t,float&);
int teleportViewTurn(float radians,std::uint64_t& turnNs) noexcept;
#endif
BodyAttachmentApi bodyAttachmentApi;BodyDrawFilter bodyDrawFilter;std::atomic<bool> bodyDrawReady{};
#endif
std::atomic<std::uint64_t> weaponContextSampleNs{};
std::atomic<const char*> weaponContextState{"NOT_OBSERVED"};
std::atomic<std::uint32_t> weaponContextFlags{},weaponLinkedGuid{},weaponGringoGuid{};
void clearWeaponContext(const char* state){weaponContextState=state;weaponContextSampleNs=0;weaponContextFlags=0;weaponLinkedGuid=0;weaponGringoGuid=0;weaponCallTrace.invalidate();}
WeaponOwnerReader weaponOwner;std::atomic<bool> weaponObservationReady{};
ReticleProbeApi reticleProbeApi{};std::atomic<bool> reticleProbeReady{};
std::atomic<std::uint64_t> reticleUpdates{},reticleProbeSamples{},reticleProbeSampleNs{};
std::atomic<const char*> reticleProbeState{"NOT_CONFIGURED"};
std::atomic<bool> reticleProbeBlocked{},reticleHeadValid{};
std::atomic<float> reticleProbeDistance{},reticleHeadGap{};
std::atomic<bool> reticleMuzzleProbeValid{},reticleMuzzleBlocked{};
std::atomic<float> reticleMuzzleDistance{},reticleMuzzleToTarget{};
std::atomic<bool> reticleAttachmentValid{};std::atomic<float> reticleAttachmentScale{};
struct GripWeaponPreview {
    std::uint64_t stamp{},scene{},sequence{},tracking{},reference{};std::uint32_t actorGuid{};int type{};
    runtime::Vec3 grip{},muzzle{},direction{},target{};float distance{};bool poseValid{},probeValid{},clamped{},blocked{};
};
std::mutex gripPreviewMutex;GripWeaponPreview gripPreview{};
std::atomic<bool> reticleOriginClamped{};std::atomic<float> reticleRequestedReach{},reticleSafeReach{};
std::atomic<WeaponClass> localWeaponClass{};std::atomic<int> localWeaponTypeId{};
std::atomic<const char*> weaponState{"NOT_CONFIGURED"};
std::atomic<std::uint64_t> localWeaponSamples{};std::atomic<std::uint32_t> localWeaponGuid{};
std::atomic<std::uint64_t> aimMapped{},aimUnavailable{};
std::atomic<const char*> aimState{"WAITING_FOR_WORLD"};
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
constexpr bool controllerTargetEnabled=true;
#else
constexpr bool controllerTargetEnabled=false;
#endif
std::atomic<const char*> targetAimState{"WAITING_FOR_NATIVE_AIM"};
std::atomic<std::uint64_t> targetAimPrepared{},targetAimDeclined{},targetAimCompleted{};
std::atomic<std::uint64_t> targetAimIgnored{};
std::mutex targetAimMutex;ControllerAimTarget lastAimTarget{};std::uint64_t targetAimStamp{},targetAimRevision{};std::uint32_t targetAimGuid{};
bool targetAimTracked{};thread_local bool pendingAimTracked{};
std::atomic<bool> nativeReticleReady{};NativeReticleFrame nativeReticleFrame{};
std::uint64_t nativeReticleSamples{},nativeReticleDeclined{},nativeReticleHidden{};
thread_local ControllerAimTarget pendingAimTarget;
thread_local std::uint64_t pendingAimStamp{},pendingAimRevision{};thread_local std::uint32_t pendingAimGuid{};
thread_local bool pendingAimLocal{};
std::atomic<std::uint64_t> targetReticlePairs{};
#ifdef RDRVR_NATIVE_MENUS
runtime::PresentationMailbox presentation;NativeUiContext nativeUi(presentation);
runtime::GameHudCapture hudCapture;
runtime::GameTheater hudQuad(runtime::QuadSourceMode::CompletedImage);std::atomic<std::uint64_t> xrHudLayers{};
#endif
#endif
#ifdef RDRVR_STEREO_BRIDGE
runtime::StereoMailbox stereoMailbox;
runtime::GameStereoCapture passCapture;
#ifdef RDRVR_PASS_RESOURCES
PassResources passResources;experimental::NativeEarlyImageRegistry earlyImageRegistry(passResources);PassSourceAbi passSourceAbi{};std::atomic<bool> passSourceReady{};
LodDiagnostics lodDiagnostics;
EngineTargetApi engineTargetApi{};std::atomic<bool> engineTargetApiReady{};
EngineFramebufferApi framebufferApi{};std::atomic<bool> framebufferApiReady{};
EngineDepthCopyApi depthCopyApi{};std::atomic<bool> depthCopyApiReady{};
EngineTextureApi textureApi{};std::atomic<bool> textureApiReady{};
TextureCallBoundary textureBoundary;std::atomic<bool> textureHooksReady{};
TargetCallBoundary targetBoundary;std::atomic<bool> targetHooksReady{};
CommandResources commandResources;
CullViewportApi cullViewportApi{};std::atomic<bool> cullViewportApiReady{};
std::unique_ptr<CullViewportPool> cullViewports;
std::atomic<bool> cullHooksReady{},worldCullingEnabled{};
bool (*ensureWorldVisibility)() noexcept{};bool (*validWorldVisibility)() noexcept{};
std::atomic<std::uint64_t> cullWrites{},stockCullProbes{},cullCompletions{},cullDeclined{};
thread_local std::uintptr_t cullDrawRenderer{};
thread_local std::uint64_t cullDrawSerial{};
EngineGlobalApi globalApi{};std::atomic<bool> globalApiReady{};
ReplayRenderPhaseApi replayPhaseApi{};std::atomic<bool> replayPhaseReady{};
ReplayGrassVisibility grassVisibility;
std::atomic<std::uint64_t> replayPhasePairs{},replayPhaseFaults{};
std::atomic<std::uint64_t> retailScenes{};
RetailDrawApi retailDrawApi{};std::atomic<bool> retailDrawReady{},profileHooksReady{};WorldDrawProfile worldProfile;
RetailResourceApi retailResourceApi{};std::atomic<bool> retailResourcesReady{};
struct HudObservation {
    std::mutex mutex;
    struct Stage {std::uint64_t calls{},completed{},stable{},changed{},aborted{};const char* status{"WAITING_FOR_DRAW"};
        std::uint32_t width{},height{},format{},depthFormat{},layout{},clears{};bool registered{},stencil{},open{},desiredMatches{},actualMatches{},dirty{};};
    std::array<Stage,static_cast<unsigned>(HudStage::Count)> stages{};
    std::string report(){std::lock_guard guard(mutex);std::ostringstream out;out<<'[';
        constexpr const char* names[]{"inventory","flash","ui","radar"};
        for(unsigned i=0;i<stages.size();++i){const auto& s=stages[i];if(i)out<<',';
            out<<"{\"stage\":\""<<names[i]<<"\",\"state\":\""<<s.status<<"\",\"sampledCalls\":"<<s.calls<<",\"completed\":"<<s.completed
               <<",\"stable\":"<<s.stable<<",\"changed\":"<<s.changed<<",\"aborted\":"<<s.aborted
               <<",\"width\":"<<s.width<<",\"height\":"<<s.height<<",\"format\":"<<s.format<<",\"depthFormat\":"<<s.depthFormat<<",\"stencil\":"<<(s.stencil?"true":"false")<<",\"layout\":"<<s.layout
               <<",\"pendingClears\":"<<s.clears<<",\"registered\":"<<(s.registered?"true":"false")
               <<",\"open\":"<<(s.open?"true":"false")<<",\"desiredMatches\":"<<(s.desiredMatches?"true":"false")
               <<",\"actualMatches\":"<<(s.actualMatches?"true":"false")<<",\"factoryDirty\":"<<(s.dirty?"true":"false")<<'}';
        }out<<']';return out.str();
    }
} hudObservation;
struct HudFrameContext {void* renderer{};FrameContext world{};bool interval{};std::uint64_t hudStartNs{};};
thread_local HudFrameContext hudFrameContext;
std::atomic<std::uint64_t> hudMatchedWorldFrames{},hudUnmatchedFrames{},hudLastMatchedFrame{};
std::atomic<const char*> hudFrameStatus{"WAITING_FOR_RENDER_FRAME"};
HudRenderEntry hudRenderEntry{};std::atomic<bool> hudRenderReady{};
#ifdef RDRVR_NATIVE_MENUS
bool describeHudPresentImage(void*,VkDevice device,VkImage image,ImageRecord& out){
    runtime::PresentImageInfo info{};if(!theater.describePresentImage(device,image,info))return false;
    out={info.image,info.device,{info.extent.width,info.extent.height,1},info.format,info.usage,VK_SAMPLE_COUNT_1_BIT,1,1,info.lifetime};return true;
}
#endif
#ifdef RDRVR_NATIVE_HUD
thread_local std::unique_ptr<EngineHudProducer> hudProducer;
std::atomic<bool> hudProducerActive{};
std::atomic<std::uint64_t> hudNativeBegun{},hudNativeRecorded{},hudNativeCancelled{};
std::atomic<unsigned> hudAlphaNormalized{},hudAlphaUnsupported{},hudTemporaryTargets{},hudMappedTargets{};
void publishHudProducer(){
    if(!hudProducer)return;
    hudProducerActive.store(hudProducer->active(),std::memory_order_release);
    hudFrameStatus=hudProducer->status();const auto& c=hudProducer->counts;
    hudNativeBegun=c.begun;hudNativeRecorded=c.recorded;hudNativeCancelled=c.cancelled;
    hudAlphaNormalized=c.routing.normalized;hudAlphaUnsupported=c.routing.unsupported;
    hudTemporaryTargets=c.routing.temporary;hudMappedTargets=c.routing.mapped;
}
#endif
std::atomic<VkQueue> worldGameQueue{};
std::atomic<std::uint64_t> worldRecorded{},worldSkipped{},worldCancelled{},worldQuarantined{};
std::atomic<const char*> worldStatus{"PROFILE_NOT_ENABLED"};
WorldReplayGate worldReplayGate;
std::mutex resourceReportMutex;
std::string resourceReport{"{}"},globalReport{"{}"},retailDrawReport{"{}"},worldCompletionReport{"{}"},worldRasterReport{"{}"},primaryRasterReport{"{}"};
std::array<std::string,32> resourceFailures{};unsigned resourceFailureNext{},resourceFailureSize{};
std::map<std::string,std::uint64_t> resourceFailureCounts;
std::atomic<std::uint64_t> deferredWorldFlushes{},deferredWorldRecovered{};
std::atomic<const char*> bindingProbeStatus{"NOT_PROBED"},worldPassStatus{"NOT_ATTEMPTED"};
std::mutex worldRejectMutex;std::map<std::string,std::uint64_t> worldRejectCounts;
void recordWorldReject(const char* why){std::lock_guard lock(worldRejectMutex);++worldRejectCounts[why];}
struct RetailPassOwner {
    void* factory{};void* device{};bool poisoned{};
    std::unique_ptr<EngineEyeTargets> images;
    std::unique_ptr<EngineFramebuffers> buffers;
    std::unique_ptr<EngineTextureMap> map;
    std::unique_ptr<EngineDepthResolve> resolve;
    std::unique_ptr<EngineStereoPass> pass;
    EngineTargetSpec preparedSpec{}; // The spec of the images' current generation
};
thread_local std::unique_ptr<RetailPassOwner> retailPass,alternateRetailPass;
// Eye-target rebuilds (generation change of an existing owner), the launched jobs cancelled for them, the
// last rebuild's frame and extents (packed width<<16|height). Report primaryEye.eyeTargetRebuild*.
std::atomic<std::uint64_t> eyeTargetRebuilds{},eyeTargetRebuildJobs{},eyeTargetRebuildFrame{},waterRebuildsAvoided{};std::atomic<std::uint32_t> eyeTargetRebuildFrom{},eyeTargetRebuildTo{};

ReplayUpdates replayUpdates;std::atomic<bool> replayHooksReady{};
EyeShaftBoundary eyeShaft;
PrimaryEyeBoundary primaryEye;std::atomic<bool> primaryHooksReady{};
std::atomic<bool> primaryRestoreFault{};
std::atomic<std::uint64_t> primaryFrames{},primaryFallbacks{};
// Stereo replay (RIGHT eye from recorded LEFT commands).
::rdrvr::engine::stereo::FrameMaps stereoFrameMaps;bool stereoFrameReady{};
std::array<std::uintptr_t,8> stereoStockTextures{};unsigned stereoStockTextureCount{};
std::atomic<std::uint64_t> stereoReplayedStages{},stereoFallbackStages{},stereoFaultedStages{},stereoFramesMapped{},stereoOverrideFrames{};
std::atomic<std::uint64_t> stereoCloneFlaggedStages{}; // Right-eye stages drawn natively because the owner's clone was flagged
std::atomic<float> stereoRelClipDiff{};std::atomic<unsigned> stereoRelClipChecks{};std::atomic<std::uint64_t> stereoOverrideDupes{};thread_local unsigned stereoOwnerLast{};std::atomic<std::uint64_t> stereoOwnerSwaps{};std::atomic<float> stereoEyeRotationDiff{},stereoRightMidFrameDiff{};runtime::Mat4 stereoLastRelR{};bool stereoLastRelRValid{};
bool stereoPrepareFrame(EyeViewports& views){
    auto& maps=stereoFrameMaps;
    if(stereoFrameReady)return maps.valid;
    stereoFrameReady=true;maps.valid=false;
    runtime::Mat4 relL{},camL{},relR{},camR{};
    if(!views.cameraMatrices(0,relL,camL)||!views.cameraMatrices(1,relR,camR)){::rdrvr::engine::stereo::noteFallback("EYE_MATRICES");return false;}
    maps.relClip[0]=relL;maps.relClip[1]=relR;maps.camera[0]=camL;maps.camera[1]=camR;
    {const auto& plan0=views.plan();float worst=0;for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)worst=std::max(worst,std::fabs(plan0.world[0][r*4+c]-plan0.world[1][r*4+c]));
     if(worst>stereoEyeRotationDiff.load(std::memory_order_relaxed))stereoEyeRotationDiff.store(worst,std::memory_order_relaxed);}
    const auto& plan=views.plan();
    const float posL[3]{plan.world[0][12],plan.world[0][13],plan.world[0][14]},posR[3]{plan.world[1][12],plan.world[1][13],plan.world[1][14]};
    if(!::rdrvr::engine::stereo::eyeCorrection(relL.data(),relR.data(),posL,posR,maps.correction)){::rdrvr::engine::stereo::noteFallback("EYE_CORRECTION");return false;}
    // Diagnostic: compare the regenerated relative clip with P*inverse(R) from the plan.
    if(stereoRelClipChecks.load(std::memory_order_relaxed)<64){
        float worst=0;
        for(unsigned eye=0;eye<2;++eye){const auto& w=plan.world[eye];const auto& p=plan.projection[eye];const auto& actual=eye?relR:relL;
            runtime::Mat4 rinv{};for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)rinv[c*4+r]=w[r*4+c];rinv[15]=1;
            const auto expected=runtime::multiply(p,rinv);
            for(unsigned i=0;i<16;++i)worst=std::max(worst,std::fabs(expected[i]-actual[i]));}
        if(worst>stereoRelClipDiff.load(std::memory_order_relaxed))stereoRelClipDiff.store(worst,std::memory_order_relaxed);
        ++stereoRelClipChecks;
    }
    // Bindless overrides: left texture slot <- right texture descriptor.
    std::array<::rdrvr::engine::stereo::BindlessOverride,8> overrides{};unsigned count=0;
    const auto index=[&](std::uintptr_t texture,std::uint32_t& out){std::uintptr_t data{};
        return texture&&passSourceAbi.read&&passSourceAbi.read(passSourceAbi.context,texture+0x98,&data,sizeof data)&&data&&
            passSourceAbi.read(passSourceAbi.context,data+0x58,&out,sizeof out)&&out<65536;};
    for(unsigned i=0;i<maps.imageCount;++i){std::uint32_t l{},r{};
        if(!(index(maps.images[i].leftTexture,l)&&index(maps.images[i].rightTexture,r))||l==r)continue;
        // The left stock Depth and SampledDepth are one texture (the engine's fast path samples the depth
        // attachment directly inside the depth stage). Keep the FIRST mapping (role order: Depth before
        // SampledDepth) so the replayed sun-shadow pass samples the right depth attachment of this frame,
        // not the right SampledDepth copy that is refreshed only at the end of the depth replay.
        bool dup=false;for(unsigned k=0;k<count;++k)if(overrides[k].leftIndex==l){dup=true;++stereoOverrideDupes;break;}
        if(!dup)overrides[count++]={l,r,maps.images[i].right};} // The copy source must be a live view of this right image
    // Also called without a pair (count 0) or with the overrides masked (bit 21): the call first restores the
    // owner's clone slots overridden earlier and not overridden now (true only when an override copy was made).
    {const bool masked=(::rdrvr::engine::stereo::liveMask()&(1u<<21))!=0;
     if(::rdrvr::engine::stereo::applyBindlessOverrides(maps.device,overrides.data(),masked?0u:count,maps.owner))++stereoOverrideFrames;}
    maps.valid=maps.imageCount>0;return maps.valid;
}
// Whether the replay handled the running right colour stage (stereoReplayStage, every return), read by
// stereoGpuStage at the stage end for colorRestart.stereo.rightNative.
thread_local bool stereoRightColorHandled{};
bool stereoReplayStage(void*,SceneStage stage,unsigned eye,EnginePassBinding& binding,bool& handled){
    handled=false;
    struct NoteColor{SceneStage stage;unsigned eye;const bool& handled;~NoteColor(){if(eye==1&&stage==SceneStage::Color)stereoRightColorHandled=handled;}} noteColor{stage,eye,handled};
    if(eye!=1||!::rdrvr::engine::stereo::enabled())return true;
    auto* views=gameEyeViewports();if(!views||!views->ready())return true;
    const auto rs=stage==SceneStage::Depth?::rdrvr::engine::stereo::Stage::Depth:stereo::Stage::Color;
    {const unsigned mask=::rdrvr::engine::stereo::liveMask();
     if((stage==SceneStage::Depth&&(mask&(1u<<19)))||(stage!=SceneStage::Depth&&(mask&(1u<<18)))){::rdrvr::engine::stereo::noteFallback("MASK_NATIVE");++stereoFallbackStages;return true;}}
    if(!::rdrvr::engine::stereo::tapeReady(rs)){::rdrvr::engine::stereo::noteFallback(stage==SceneStage::Depth?"DEPTH_TAPE":"COLOR_TAPE");++stereoFallbackStages;return true;}
    if(!stereoPrepareFrame(*views)){++stereoFallbackStages;return true;}
    // After this frame's override copies, before anything binds the owner's clone: a clone with the
    // Adreno companion flag costs about 0.2 ms per replayed draw, so that owner's right eye is drawn
    // natively (engine sets, never flagged) and this frame's worker jobs (they bound the clone at
    // launch) are cancelled and joined. cloneFlagged rebuilds the clone
    // (debug.rdrvr.stereoreplay.clonerebuild).
    if(::rdrvr::engine::stereo::cloneFlagged(stereoFrameMaps.device,stereoFrameMaps.owner,stereoFrameMaps.frameSerial)){ // Keyed by the frame serial
        ::rdrvr::engine::stereo::cancelFrameJobs("CLONE_FLAGGED");
        ::rdrvr::engine::stereo::noteFallback("CLONE_FLAGGED");++stereoFallbackStages;++stereoCloneFlaggedStages;return true;}
    if(!binding.closeIfBound()){handled=true;return false;}
    {runtime::Mat4 relNow{},camNow{};if(views->cameraMatrices(1,relNow,camNow)){
        if(stage==SceneStage::Depth){stereoLastRelR=relNow;stereoLastRelRValid=true;}
        else if(stereoLastRelRValid){float worst=0;for(unsigned i=0;i<16;++i)worst=std::max(worst,std::fabs(relNow[i]-stereoLastRelR[i]));if(worst>stereoRightMidFrameDiff.load(std::memory_order_relaxed))stereoRightMidFrameDiff.store(worst,std::memory_order_relaxed);}
        if(stage!=SceneStage::Depth){stereoFrameMaps.relClip[1]=relNow;stereoFrameMaps.camera[1]=camNow;}}}
    const bool ok=::rdrvr::engine::stereo::replay(rs,binding.command(),stereoFrameMaps);
    dynamicUniformDescriptors.commandStateInvalidated(binding.command());
    if(ok){handled=true;++stereoReplayedStages;return true;}
    if(::rdrvr::engine::stereo::replayFaulted()){++stereoFaultedStages;::rdrvr::engine::stereo::noteFallback("REPLAY_FAULTED");++stereoFallbackStages;return true;} // state restored by replay(); native path re-binds
    ::rdrvr::engine::stereo::noteFallback("REPLAY_DECLINED");++stereoFallbackStages;return true;
}
// This frame's stage CPU + HUD FPS for the current settings state, then the signature for the next frames
// (resolution, sun shadows, grass, foveation when the setting exists, small objects; StereoReplay appends the STEREO TEST mode).
void stereoGpuState(const ::rdrvr::engine::stereo::FrameStageSample& f) noexcept {
    const auto hz=diagnostics::stereoHistory.recent("CAPTURED",diagnostics::monotonicNs()).hz; // the HUD "NEW %.1f FPS"
    ::rdrvr::engine::stereo::gpuNoteFrameCpu(f,hz);
    const auto s=runtime::graphicsSettings.snapshot();const auto& v=s.requested;
    // The foveation level the NEXT frame records with (STEREO TEST 17 sweep, AUTO, property or menu;
    // decided by gameFoveationFrame, applied one frame late) + "a" while FOVEATION AUTO chose it; requested value without an API.
    int fov=::rdrvr::engine::gameFoveationNextLevel();
    if(fov<0)fov=[](const auto& x)->int{if constexpr(requires{x.foveation;})return int(x.foveation);
                                         else if constexpr(requires{x.foveationLevel;})return int(x.foveationLevel);else return -1;}(v);
    const char* fovAuto=::rdrvr::engine::gameFoveationAutoActive()?"a":"";
    char sig[128]; // gpu::signatureSize 128; gpuSetState cuts the graphics part so the _m/_pt suffix always fits
    // _aa<effective> from the stereo mailbox (runtime/AntiAliasingStatus.h): 0 copy (OFF), 1 FXAA ran,
    // 2 FXAA requested but copied (fallback), -1 no pair yet. Includes debug.rdrvr.aa overrides.
    const int aa=runtime::antiAliasingEffective.load(std::memory_order_relaxed);
    // _so = the SMALL OBJECTS threshold SmallObjectCull applied last (+ "p" when debug.rdrvr.smallobjects set it); the menu
    // value until the first MAIN visibility frame.
    const int soNow=::rdrvr::engine::s02::smallObjectsPx.load(std::memory_order_relaxed);
    const unsigned so=soNow>=0?unsigned(soNow):v.smallObjectsPx;const char* soProp=soNow>=0&&::rdrvr::engine::s02::smallObjectsProperty.load(std::memory_order_relaxed)?"p":"";
    if(v.resolutionPercent==s.startupResolutionPercent)std::snprintf(sig,sizeof sig,"res%u_sh%d_gr%d_fov%d%s_so%u%s_aa%d",s.startupResolutionPercent,v.shadows?1:0,v.grass?1:0,fov,fovAuto,so,soProp,aa);
    else std::snprintf(sig,sizeof sig,"res%up%u_sh%d_gr%d_fov%d%s_so%u%s_aa%d",s.startupResolutionPercent,v.resolutionPercent,v.shadows?1:0,v.grass?1:0,fov,fovAuto,so,soProp,aa);
    { // _aa1d<fxaa.frag debugMode> for the proof modes FXAA EDGES (d1) / FXAA SPLIT (d2); plain FXAA stays _aa1
        const int d=runtime::antiAliasingDebug.load(std::memory_order_relaxed);const std::size_t used=std::strlen(sig);
        if(aa==1&&d>0&&used+1<sizeof sig)std::snprintf(sig+used,sizeof sig-used,"d%d",d);
    }
    { // _su<SHADOW UPDATE>_sm<cascade map extent the engine created, 0 unknown>_gd<GRASS DISTANCE>
      // _su = the level ShadowUpdate.h acts on (stored at every RenderScene; debug.rdrvr.gfx changes it through the row)
      // + SunStagger suffix at levels 5/6: x = acts as EVERY (unavailable/disabled/OFF), a AREA, n NOGATE, l LEARN, o ORDER
        const std::size_t used=std::strlen(sig);
        if(used+1<sizeof sig)std::snprintf(sig+used,sizeof sig-used,"_su%u%s_sm%u_gd%u",::rdrvr::engine::shadowupdate::stats.level.load(std::memory_order_relaxed),
            ::rdrvr::engine::shadowupdate::stagger::signatureSuffix(),runtime::shadowMapCascadeExtent.load(std::memory_order_relaxed),v.grassDistance);
    }
    { // _sw<requested SPACEWARP row value> (the number as the menu stores it), so the SpaceWarp cost separates
        const std::size_t used=std::strlen(sig);
        // Suffix "s" while the runtime really synthesizes (warp info chained), so ON-but-suspended frames separate.
        if(used+1<sizeof sig)std::snprintf(sig+used,sizeof sig-used,"_sw%u%s",unsigned(v.spaceWarp),runtime::spaceWarpControl.synthesizing()?"s":"");
    }
    { // _oc<OCCLUSION>_pf<POST FX effective digit>_in<indoor 1/0/x>_pl<pacing code 0..4>_em<EDGE MARGIN %>
        const int indoorFlag=::rdrvr::engine::indoor::flag();
        const char inChar=indoorFlag==1?'1':indoorFlag==0?'0':'x';
        const std::size_t used=std::strlen(sig);
        if(used+1<sizeof sig)std::snprintf(sig+used,sizeof sig-used,"_oc%d_pf%u_in%c_pl%u_em%u",v.occlusion?1:0,
            ::rdrvr::engine::postfx::signatureDigit(::rdrvr::engine::stereo::testMode.load(std::memory_order_relaxed)),inChar,
            runtime::framePacer.signatureCode(),unsigned(v.edgeMarginPercent));
    }
    ::rdrvr::engine::enginetex::appendSignature(sig,sizeof sig); // _af<cap|x>_mb<centi|x>
    { // _cc<CASCADE CULL in memory>_sp<SHARPEN chained>_qp<XR queue layout 0/2/3>
        const std::size_t used=std::strlen(sig);
        if(used+1<sizeof sig)std::snprintf(sig+used,sizeof sig-used,"_cc%u_sp%d_qp%d",::rdrvr::engine::shadowupdate::cascadeCullEffective(),
            runtime::sharpenEffective.load(std::memory_order_relaxed),std::max(runtime::xrQueueLayoutActive.load(std::memory_order_relaxed),0));
    }
    { // _ls<lamp update 0 EVERY/1 1/2/2 1/3><spot cascade 1 ON/0 OFF><lamp cull 0/1 X2/2 X4> = LAMP SHADOWS levers in force
        const std::size_t used=std::strlen(sig);
        if(used+1<sizeof sig)std::snprintf(sig+used,sizeof sig-used,"_ls%03u",::rdrvr::engine::lampshadow::signatureCode());
    }
    { // _ta<render-thread TAA row in effect> 0 OFF (or module not READY), 1 TAA, 2 TAA FULL, 3 TAA LITE (incl. the AppSW TAA -> LITE
      // override, vr/TaaPass.h): "_aa" above is only the mailbox FXAA/copy digit, so _ta separates TAA / LITE / OFF. Before _x (last item).
        const std::size_t used=std::strlen(sig);
        if(used+1<sizeof sig)std::snprintf(sig+used,sizeof sig-used,"_ta%d",::rdrvr::vr::taa::signatureDigit());
    }
    ::rdrvr::engine::ablate::appendSignature(sig,sizeof sig); // _abb / _ab<k> / _abp<k> / _abc of the next scene (nothing while off)
    { // _x<effective lever mask of the next frame>[t] (S02Levers.h), LAST graphics item (gpuSetState cuts the graphics part)
        namespace s2=::rdrvr::engine::s02;
        const unsigned mode=::rdrvr::engine::stereo::testMode.load(std::memory_order_relaxed);
        const unsigned installed=s2::installedMask.load(std::memory_order_relaxed);
        const auto inst=[&](unsigned bit){return (installed&bit)!=0;};
        const int indoorFlag=::rdrvr::engine::indoor::flag();
        const char* indoorWhy=indoorFlag==0?"outdoor":"indoorUnknown";
        const bool mergeLatched=(::rdrvr::engine::stereo::boundaryFlags.load(std::memory_order_relaxed)&::rdrvr::engine::stereo::BoundaryMergeR)&&
            ::rdrvr::engine::stereo::boundaryPhase.load(std::memory_order_relaxed)==0;
        s2::Gates g{};
        g.cr={inst(s2::CR)&&::rdrvr::engine::colorrestart::usable(),inst(s2::CR)?::rdrvr::engine::colorrestart::reason():"notInstalled"};
        g.mg={mergeLatched,::rdrvr::engine::stereo::testModeAlternating(mode)?"stereoTest":!::rdrvr::engine::stereo::merge::enabled()?"mergeProperty0":"notLatched"};
        g.pt={inst(s2::PT)&&s2::postTailMask()!=0,!inst(s2::PT)?"notInstalled":"posttail0"};
        g.nf={inst(s2::NF)&&::rdrvr::engine::postfx::effective(mode)!=::rdrvr::engine::postfx::On,!inst(s2::NF)?"notInstalled":"postFxOn"};
        g.ir={inst(s2::IR)&&indoorFlag==1,!inst(s2::IR)?"notInstalled":indoorWhy};
        g.it={inst(s2::IT)&&indoorFlag==1,!inst(s2::IT)?"notInstalled":indoorWhy};
        g.forced=mergeLatched?s2::MG:0u; // debug.rdrvr.stereoreplay.merge=2 (alias) merges without the MG lever bit
        s2::appendSignature(sig,sizeof sig,s2::publishEffective(g),s2::transitionNow.load(std::memory_order_relaxed));
    }
    ::rdrvr::engine::stereo::gpuSetState(sig); // Appends _m<STEREO TEST mode> and a/b (boundary phase, latched just before) and _pt<1|0|L>
}
// GPU stage markers of the stereo pass eye stages: eye 1 = right; eye 0 only when the left eye is not
// reused from the primary path (first marker wins, the primary left stages mark first).
void stereoGpuStage(unsigned eye,SceneStage phase,bool begin) noexcept {
    using M=::rdrvr::engine::stereo::GpuMark;
    const M m=phase==SceneStage::Depth?(eye?(begin?M::RightDepthBegin:M::RightDepthEnd):(begin?M::LeftDepthBegin:M::LeftDepthEnd)):
              phase==SceneStage::Lighting?(eye?(begin?M::RightLightBegin:M::RightLightEnd):(begin?M::LeftLightBegin:M::LeftLightEnd)):
              (eye?(begin?M::RightColorBegin:M::RightColorEnd):(begin?M::LeftColorBegin:M::LeftColorEnd));
    ::rdrvr::engine::stereo::gpuMark(m);
    // EngineStereoPass calls this around every eye stage, replayed or native, so the CR scope covers each
    // Color stage of the stereo pass too: a right colour stage the replay does not handle (stereo off, MASK_NATIVE, COLOR_TAPE,
    // CLONE_FLAGGED, REPLAY_DECLINED/FAULTED) is re-run by the engine through d11 and must merge like the left one; eye 0 runs
    // here only when the left eye is not reused. The byte is not read by a replay (raw Vulkan). Same frame mask as the left.
    if(phase==SceneStage::Color){
        if(begin){stereoRightColorHandled=false;::rdrvr::engine::colorrestart::beginStereo(::rdrvr::engine::s02::wants(::rdrvr::engine::s02::CR));}
        else ::rdrvr::engine::colorrestart::endStereo(eye==1&&!stereoRightColorHandled);
    }
    if(phase==SceneStage::Lighting){ // LR: stereo-pass light stages (right eye; left when redrawn) merge only with debug.rdrvr.lr.right=1
        if(begin)::rdrvr::engine::colorrestart::beginLight(true,::rdrvr::engine::s02::wants(::rdrvr::engine::s02::LR),::rdrvr::engine::s02::lrRight());
        else ::rdrvr::engine::colorrestart::endLight();
    }
}
void stereoProbeStage(void*,unsigned eye,bool begin) noexcept {
    if(!begin){::rdrvr::engine::stereo::setProbe(0,nullptr,nullptr,nullptr);return;}
    auto* views=gameEyeViewports();runtime::Mat4 rel{},cam{},orel{},ocam{};
    if(!views||!views->ready()||!views->cameraMatrices(eye,rel,cam)){::rdrvr::engine::stereo::setProbe(0,nullptr,nullptr,nullptr);return;}
    const bool other=views->cameraMatrices(eye^1u,orel,ocam);
    ::rdrvr::engine::stereo::setProbe(eye+1,rel.data(),cam.data(),stereoFrameMaps.context,other?orel.data():nullptr,other?ocam.data():nullptr);
}
// Worker mode (debug.rdrvr.stereoreplay.async=2): the right-eye jobs are launched right after each LEFT stage is
// recorded, i.e. before gameDrawRetailWorld fills stereoFrameMaps. Predict this frame's maps: the owner the
// consumer will pick (its retailPass rule; only a retired owner, so the owner's secondary pools may be reset),
// that owner's maps from its previous fill (same owner object and image generation), this frame's eye matrices.
// The consumer checks the prediction (validateLaunched) and the splice re-checks every input of the recording.
std::uint64_t stereoFrameSerial{};              // FrameMaps::frameSerial, incremented by the consumer fill
thread_local const void* stereoOwnerSlots[2]{}; // owner index by RetailPassOwner identity (consumer fill)
std::array<::rdrvr::engine::stereo::FrameMaps,2> stereoLastMaps{};std::array<std::uint64_t,2> stereoLastToken{};
std::uint64_t stereoOwnerToken(const RetailPassOwner* owner) noexcept {
    return owner?std::uint64_t(reinterpret_cast<std::uintptr_t>(owner))^((owner->images?owner->images->generation():0)*0x9E3779B97F4A7C15ull):0;
}
bool stereoPredictMaps(::rdrvr::engine::stereo::FrameMaps& out,std::uint64_t& token,const char*& why) noexcept {
    auto* views=gameEyeViewports();if(!views||!views->ready()){why="VIEWS_NOT_READY";return false;}
    if(!retailPass||!retailPass->pass){why="NO_OWNER";return false;}
    if(retailPass->poisoned){why="OWNER_QUARANTINED";return false;}
    // Same choice as gameDrawRetailWorld; retire() only commits a GPU completion that already happened (monotonic).
    const RetailPassOwner* owner=nullptr;
    if(retailPass->pass->retire())owner=retailPass.get();
    else if(alternateRetailPass&&alternateRetailPass->pass&&!alternateRetailPass->poisoned&&alternateRetailPass->pass->retire())owner=alternateRetailPass.get();
    else{why="OWNERS_PENDING";return false;}
    unsigned idx=2;for(unsigned i=0;i<2;++i)if(stereoOwnerSlots[i]==owner)idx=i;
    if(idx>1){why="OWNER_UNSEEN";return false;}
    token=stereoOwnerToken(owner);
    if(!stereoLastMaps[idx].valid||stereoLastToken[idx]!=token){why="NO_PREVIOUS_MAPS";return false;}
    out=stereoLastMaps[idx];out.frameSerial=stereoFrameSerial+1;
    if(::rdrvr::engine::stereo::cloneFlagged(out.device,out.owner,out.frameSerial,true)){why="CLONE_FLAGGED";return false;}
    runtime::Mat4 relL{},camL{},relR{},camR{};
    if(!views->cameraMatrices(0,relL,camL)||!views->cameraMatrices(1,relR,camR)){why="EYE_MATRICES";return false;}
    out.relClip[0]=relL;out.relClip[1]=relR;out.camera[0]=camL;out.camera[1]=camR;
    const auto& plan=views->plan();
    const float posL[3]{plan.world[0][12],plan.world[0][13],plan.world[0][14]},posR[3]{plan.world[1][12],plan.world[1][13],plan.world[1][14]};
    if(!::rdrvr::engine::stereo::eyeCorrection(relL.data(),relR.data(),posL,posR,out.correction)){why="EYE_CORRECTION";return false;}
    return true;
}
// streaming: right after beginRecording(stage) (the worker follows the recording); otherwise the retry after
// endRecording(stage) with the complete tape. Only the final attempt counts a not-launched reason.
void stereoLaunchStage(::rdrvr::engine::stereo::Stage stage,bool streaming) noexcept {
    try{
        if(!::rdrvr::engine::stereo::launchWanted())return;
        ::rdrvr::engine::stereo::FrameMaps predicted{};std::uint64_t token{};const char* why="NONE";
        if(!stereoPredictMaps(predicted,token,why)){
            if(streaming)::rdrvr::engine::stereo::noteStreamNotLaunched(stage,why);else ::rdrvr::engine::stereo::noteNotLaunched(stage,why);return;}
        ::rdrvr::engine::stereo::launchSecondaries(stage,predicted,token,streaming);
    }catch(...){}
}
void invalidatedCommand(VkCommandBuffer cmd) noexcept {
    passCapture.commandInvalidated(cmd);
#ifdef RDRVR_NATIVE_MENUS
    hudCapture.commandInvalidated(cmd);
#endif
}
#endif
void completedStereoPasses(VkQueue queue,VkFence fence,std::span<const VkSemaphore> waits) noexcept {
    diagnostics::WallScope timing(diagnostics::frameTimings.presentTransfers);
    try {
        const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        passCapture.completePresent(queue,fence,stereoMailbox,now,waits);
#ifdef RDRVR_NATIVE_MENUS
        hudCapture.completePresent(queue,fence,hudQuad,now,waits);
#endif
    }catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","pass capture publish failed: %s",e.what());}
    catch(...){}
}
std::atomic<std::uint64_t> stereoPresentedNs{},worldStereoLayers{},clonedViewPairs{},cloneFailures{};
ViewportApi viewportApi{};
std::atomic<bool> viewportApiReady{};
thread_local std::unique_ptr<EyeViewports> eyeViewports;
#endif
#ifdef RDRVR_CAMERA_BRIDGE
runtime::TrackedViewsMailbox gameViews;
SceneCameraBridge cameraBridge(gameViews);
#ifdef RDRVR_PASS_RESOURCES
VisibilityCameraBridge visibilityBridge(gameViews);
#endif
#endif
std::mutex privateQueueMutex;
PFN_vkDeviceWaitIdle originalIdle{};
XrGraphicsBindingVulkan2KHR sessionBinding{};
#ifdef RDRVR_HEAD_CAMERA
// HANDS: restore of the engine's bound command state after the hands commands.
bool handsRestoreReady(VkCommandBuffer c) noexcept {return c&&::rdrvr::engine::stereo::internal::trackedState(c)!=nullptr;}
void handsRestore(VkCommandBuffer c) noexcept {
    namespace st=::rdrvr::engine::stereo;
    static const float identity[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    try{
        if(st::enabled())if(const auto layout=st::internal::replayPushLayout(sessionBinding.device))
            vkCmdPushConstants(c,layout,VK_SHADER_STAGE_VERTEX_BIT,0,64,identity);
        if(const auto* s=st::internal::trackedState(c))st::internal::emitBoundState(c,*s,nullptr);
        dynamicUniformDescriptors.commandStateInvalidated(c);
    }catch(...){}
}
#endif
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_NATIVE_MENUS)
// HUD (vr/VrHud.h): engine reads for the HUD producer. All on the render thread.
// Engine-tracked layout of the HUD texture at the game's submission (texture+0x98 -> +0x40 VkImage, +0xa0 layout) and the
// image registry generation of the cleared target (an image re-created between the HUD End and the submit is refused).
bool vrHudLayout(std::uintptr_t texture,VkImage image,std::uint64_t generation,VkImageLayout& out) noexcept {
    try{
        const auto& m=framebufferApi.memory;std::uintptr_t data{};VkImage actual{};std::uint32_t layout{};ImageRecord r{};
        if(!m.read||!texture||!m.read(m.context,texture+0x98,&data,sizeof data)||!data||data%8||!m.read(m.context,data+0x40,&actual,sizeof actual)||
           actual!=image||!m.read(m.context,texture+0xa0,&layout,sizeof layout)||!passResources.lookup(sessionBinding.device,image,r)||r.generation!=generation)return false;
        out=static_cast<VkImageLayout>(layout);return true;
    }catch(...){return false;}
}
// The GfxContext framebuffer bound on this thread (gfx+0x158, as HudNativeRouting::current).
bool vrHudBoundFramebuffer(std::uintptr_t& out) noexcept {
    try{
        void* gfx{};const auto& m=framebufferApi.memory;
        return retailResourceApi.currentGfx&&retailResourceApi.currentGfx(&gfx)&&gfx&&m.read&&m.read(m.context,reinterpret_cast<std::uintptr_t>(gfx)+0x158,&out,sizeof out);
    }catch(...){return false;}
}
#ifdef RDRVR_NATIVE_HUD
// debug.rdrvr.vr.hud.alpha 1: the HudAlphaFlush (coverage alpha, RGB blend unchanged) on draws into the cleared target.
bool vrHudAlphaInvoke(void (*original)()) {
    static std::unique_ptr<HudBlendReader> blend;static std::unique_ptr<HudAlphaFlush> alpha;
    if(!hudRenderReady.load(std::memory_order_acquire)||hudRenderEntry.flush!=original){original();return false;}
    if(!alpha){
        blend=std::make_unique<HudBlendReader>(HudBlendReader{framebufferApi.memory.read,framebufferApi.memory.context,hudRenderEntry.blendBlock,framebufferApi.ownsThread});
        alpha=std::make_unique<HudAlphaFlush>(HudAlphaApi{HudBlendReader::read,blend.get(),hudRenderEntry.set,hudRenderEntry.flush});
    }
    const auto r=alpha->invoke(true);
    return r==HudAlphaResult::Normalized||r==HudAlphaResult::MaskOnly;
}
#endif
#endif
std::mutex errorMutex;std::string sessionError;
std::atomic<bool> stopWorker{},workerActive{},sessionCreated{},sessionRunning{};
std::atomic<std::uint64_t> xrFrames{},xrPairs{},xrLayers{},xrTheaterFrames{},xrShouldNotRender{},xrInvalidViews{};
std::atomic<unsigned long long> xrViewFlags{};
std::atomic<unsigned> gameIdleCalls{};
std::atomic<int> xrSessionState{};
std::atomic<const char*> sessionStatus{"NOT_STARTED"};
// Process lifetime object; native Android termination does not run a C++ join.
std::thread* worker{};
void stopAndJoin() {
    stopWorker=true;
    if(worker && worker->joinable())worker->join();
}
void startSession() {
    XrGraphicsBindingVulkan2KHR binding{};
    if(worker || !bridge->binding(binding))return;
    sessionBinding=binding;theater.bind(binding);
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_NATIVE_MENUS)
    ::rdrvr::vr::hud::bind(binding.physicalDevice,binding.device,binding.queueFamilyIndex);
#ifdef RDRVR_NATIVE_HUD
    ::rdrvr::vr::hud::configureAlpha({vrHudBoundFramebuffer,vrHudAlphaInvoke});
#else
    ::rdrvr::vr::hud::configureAlpha({vrHudBoundFramebuffer,nullptr});
#endif
#endif
#ifdef RDRVR_NATIVE_MENUS
    hudQuad.bind(binding);hudCapture.bind(binding);
    theater.onPresentOverlay([](VkQueue q) noexcept {return hudCapture.pendingOverlay(q);},
        [](VkQueue q,VkFence fence,runtime::CompletedQuadImage& image,std::span<const VkSemaphore> waits) noexcept {return hudCapture.overlaySource(q,fence,image,waits);});
#endif
#ifdef RDRVR_STEREO_BRIDGE
    stereoMailbox.bind(binding);passCapture.bind(binding,true,true);theater.onCompletedPresent(completedStereoPasses);
#ifdef RDRVR_HEAD_CAMERA
    theater.onWorldPresentBypass(gameWorldPresentBypass);
    theater.onWorldPresentRetire(gameWorldPresentOnly);
#endif
#endif
#ifdef RDRVR_PASS_RESOURCES
    {experimental::NativeEarlyImageRegistry::Mutation mutation(earlyImageRegistry);
        if(!passResources.bind(binding.device))throw std::runtime_error("pass resource registry bind failed");}
    earlyImageRegistry.setCoverage(true);
    if(!commandResources.bind(binding.device,invalidatedCommand))throw std::runtime_error("command resource registry bind failed");
#endif
    const auto xrInstance=xr;const auto xrSystem=system;auto* negotiated=bootstrap.get();
    stopWorker=false;workerActive=true;sessionStatus="STARTING";
    try {worker=new std::thread([xrInstance,xrSystem,negotiated] {
        std::unique_ptr<runtime::XrHarness> scene;bool exitRequested=false;
        JNIEnv* workerEnv{};bool attached=false;
        struct Detach {JavaVM* vm;bool& attached;~Detach(){if(attached)vm->DetachCurrentThread();}} detach{javaVm,attached};
        try {
            if(javaVm->GetEnv(reinterpret_cast<void**>(&workerEnv),JNI_VERSION_1_6)!=JNI_OK) {
                if(javaVm->AttachCurrentThread(&workerEnv,nullptr)!=JNI_OK)throw std::runtime_error("XR thread JVM attach failed");
                attached=true;
            }
            scene=std::make_unique<runtime::XrHarness>();
            scene->setPerformanceSupport(performanceSupport);
#ifdef RDRVR_TOUCH_CONTROLS
            scene->setHaptics(&haptics);
#endif
            {std::lock_guard queueGuard(privateQueueMutex);scene->initializeBorrowed(xrInstance,xrSystem,*negotiated,&theater
#ifdef RDRVR_CAMERA_BRIDGE
                ,&gameViews
#endif
#ifdef RDRVR_STEREO_BRIDGE
                ,&stereoMailbox
#endif
#ifdef RDRVR_TOUCH_CONTROLS
                ,&controllers
#endif
#ifdef RDRVR_NATIVE_MENUS
                ,&presentation,&hudQuad
#endif
            );}
            sessionCreated=true;sessionStatus="SESSION_CREATED";
            while(!scene->exiting()) {
                {
                    std::lock_guard queueGuard(privateQueueMutex);
                    scene->events();
                    if(stopWorker && !exitRequested) {
                        if(!scene->running())break;
                        scene->requestExit();exitRequested=true;
                    }
                    if(scene->running())scene->frame();
#ifdef RDRVR_TOUCH_CONTROLS
                    hapticApplied=scene->hapticOutput().applied();hapticStopped=scene->hapticOutput().stopped();hapticFailed=scene->hapticOutput().failed();
#endif
                    xrFrames=scene->frames();xrPairs=scene->pairs();sessionRunning=scene->running();xrSessionState=scene->sessionState();
                    xrLayers=scene->layers();xrTheaterFrames=scene->theaterFrames();xrShouldNotRender=scene->shouldNotRender();xrInvalidViews=scene->invalidViews();xrViewFlags=scene->viewFlags();
#ifdef RDRVR_NATIVE_MENUS
                    xrHudLayers=scene->hudLayers();
#endif
                    if(scene->running() && xrFrames.load())sessionStatus=scene->layerMode();
#ifdef RDRVR_STEREO_BRIDGE
                    const auto presented=scene->worldStereoLayers();
                    stereoPresentedNs=(scene->running()&&presented!=worldStereoLayers.load())?diagnostics::monotonicNs():0;
                    worldStereoLayers=presented;

#endif
                    if(exitRequested && !scene->running())break;
                }
                if(!scene->running())std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            sessionStatus="SESSION_STOPPED";
        }catch(const std::exception& error) {
            {std::lock_guard errorGuard(errorMutex);sessionError=error.what();}
            sessionCreated=scene && scene->sessionCreated();sessionStatus="SESSION_FAILED";
            __android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","session failed: %s",error.what());
        }catch(...) {sessionStatus="SESSION_FAILED";}
        {std::lock_guard queueGuard(privateQueueMutex);scene.reset();}
        sessionRunning=false;workerActive=false;
    });}catch(...) {workerActive=false;sessionStatus="THREAD_START_FAILED";}
}
VKAPI_ATTR VkResult VKAPI_CALL deviceIdle(VkDevice d) {
    ++gameIdleCalls;std::lock_guard guard(privateQueueMutex);return originalIdle(d);
}
#endif
void releaseActivity() {
    if(!javaVm || !activity)return;
    JNIEnv* env{};bool attached=false;
    if(javaVm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK) {
        if(javaVm->AttachCurrentThread(&env,nullptr)!=JNI_OK)return;
        attached=true;
    }
    env->DeleteGlobalRef(activity);activity=nullptr;
    if(attached)javaVm->DetachCurrentThread();
}
bool checked(XrResult result,const char* failure) {
    xrResult=result;if(XR_FAILED(result)){state=failure;return false;}return true;
}
VKAPI_ATTR VkResult VKAPI_CALL instance(const VkInstanceCreateInfo* r,const VkAllocationCallbacks* a,VkInstance* out) {
    std::lock_guard guard(mutex);
    try {
        const bool layerAvailable88=rdr2_game_worker::available();
        rdr2_game_worker::InstanceRequest request(r,layerAvailable88);
        auto containsWorker88=[](const VkInstanceCreateInfo* info) noexcept {
            if(!info||info->enabledLayerCount>256||(info->enabledLayerCount&&!info->ppEnabledLayerNames))return -1;
            for(uint32_t i=0;i<info->enabledLayerCount;++i)
                if(info->ppEnabledLayerNames[i]&&!std::strcmp(info->ppEnabledLayerNames[i],rdr2_game_worker::LayerName))return 1;
            return 0;
        };
        __android_log_print(ANDROID_LOG_INFO,"RdrWorker88","instanceLayerAvailable=%u originalWorkerLayer=%d requestedWorkerLayer=%d",unsigned(layerAvailable88),containsWorker88(r),containsWorker88(request.get()));
        return bridge->createInstance(request.get(),a,out);
    }catch(...){state="INSTANCE_EXCEPTION";if(out)*out=VK_NULL_HANDLE;return VK_ERROR_OUT_OF_HOST_MEMORY;}
}
VKAPI_ATTR VkResult VKAPI_CALL device(VkPhysicalDevice p,const VkDeviceCreateInfo* r,const VkAllocationCallbacks* a,VkDevice* out) {
    std::lock_guard guard(mutex);
    try {
        const VkDeviceCreateInfo* request=r;
#ifdef RDRVR_FRAGMENT_RATE
        // FOVEATION API (graphics.cfg foveationApi / debug.rdrvr.foveation.api): FDM and the KHR rate
        // features are mutually exclusive on one VkDevice, so exactly one backend is requested here.
        const bool foveationEligible=r&&bridge->gameInstance()&&!std::strcmp(bridge->state(),"XR_INSTANCE_CREATED");
        const auto foveationApi=gameFoveationApiRequest();
        if(foveationEligible)gameFoveationQuery(p);
        foveation::DensityDevicePlan densityPlan;
        bool densityRequested=foveationEligible&&foveationApi==foveation::Api::Fdm&&gameFoveationDensityPlan(*r,densityPlan);
        runtime::FragmentRateDevicePlan ratePlan;
        bool rateRequested=foveationEligible&&!densityRequested&&foveationApi!=foveation::Api::None&&
            gameFragmentRateDevicePlan(bridge->gameInstance(),p,*r,ratePlan);
        if(densityRequested)request=densityPlan.info();else if(rateRequested)request=ratePlan.info();
#endif
        auto result=bridge->createDevice(p,request,a,out);
#ifdef RDRVR_FRAGMENT_RATE
        if(result!=VK_SUCCESS&&densityRequested){ // the runtime/driver refused the density map: the original request, unchanged
            gameFoveationDensityRejected(result);densityRequested=false;result=bridge->createDevice(p,r,a,out);}
        XrGraphicsBindingVulkan2KHR rateBinding{};
        if(result==VK_SUCCESS&&(rateRequested||densityRequested)&&bridge->binding(rateBinding)){
            const bool rateReady=rateRequested&&gameFragmentRateInitialize(p,*out,rateBinding.queueFamilyIndex,rateBinding.queueIndex);
            gameFoveationDeviceCreated(densityRequested?foveation::Api::Fdm:rateReady?foveation::Api::Vrs:foveation::Api::None,
                p,*out,rateBinding.queueFamilyIndex,rateBinding.queueIndex,&privateQueueMutex);
        }else if(result==VK_SUCCESS)gameFoveationDeviceCreated(foveation::Api::None,p,*out,0,0,nullptr);
#endif
#ifdef RDRVR_GAME_SESSION
        if(result==VK_SUCCESS)startSession();
#endif
        return result;
    }catch(...){state="DEVICE_EXCEPTION";if(out)*out=VK_NULL_HANDLE;return VK_ERROR_OUT_OF_HOST_MEMORY;}
}
// The game's vkGetDeviceQueue (engine GOT -> VulkanTrace -> here). Split queue layouts return the game's
// protected-capable queues; everything else is forwarded unchanged. bridge is set before api.queue points here and
// is never reset; the redirect table is written before the game receives its VkDevice.
VKAPI_ATTR void VKAPI_CALL deviceQueue(VkDevice d,std::uint32_t family,std::uint32_t index,VkQueue* out) {
    bridge->getDeviceQueue(d,family,index,out);
}
VKAPI_ATTR void VKAPI_CALL destroyInstance(VkInstance i,const VkAllocationCallbacks* a) {
    std::lock_guard guard(mutex);
#ifdef RDRVR_GAME_SESSION
    // An unrelated helper instance must not stop the game session.
    if(worker && i==sessionBinding.instance){stopAndJoin();
        try{theater.shutdown();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","theater shutdown quarantined: %s",e.what());}
#ifdef RDRVR_NATIVE_MENUS
        try{hudQuad.shutdown();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","HUD shutdown quarantined: %s",e.what());}
#endif
#ifdef RDRVR_STEREO_BRIDGE
        try{stereoMailbox.shutdown();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","stereoMailbox shutdown quarantined: %s",e.what());}
#endif
    }
#endif
    bridge->destroyInstance(i,a);
    if(!std::strcmp(bridge->state(),"GAME_INSTANCE_DESTROYED") && xr) {
        // Worker/session resources have already been released.
        xrDestroyInstance(xr);xr=XR_NULL_HANDLE;releaseActivity();
    }
}
VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice d,const VkAllocationCallbacks* a) {
    std::lock_guard guard(mutex);
#ifdef RDRVR_GAME_SESSION
    if(worker && d==sessionBinding.device){stopAndJoin();
        try{theater.shutdown();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","theater shutdown quarantined: %s",e.what());}
#ifdef RDRVR_NATIVE_MENUS
        try{hudQuad.shutdown();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","HUD shutdown quarantined: %s",e.what());}
#endif
#ifdef RDRVR_STEREO_BRIDGE
        try{stereoMailbox.shutdown();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","stereoMailbox shutdown quarantined: %s",e.what());}
#endif
    }
#endif
#ifdef RDRVR_STEREO_BRIDGE
    if(d==sessionBinding.device) {
        #ifdef RDRVR_NATIVE_MENUS
        try{hudCapture.shutdownRetired();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","HUD capture shutdown quarantined: %s",e.what());}
#endif
        try{passCapture.shutdownRetired();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","pass capture shutdown quarantined: %s",e.what());}
    }
#endif
#ifdef RDRVR_PASS_RESOURCES
    commandResources.clear(d);earlyImageRegistry.setCoverage(false);
    {experimental::NativeEarlyImageRegistry::Mutation mutation(earlyImageRegistry);passResources.clear(d);}
#endif
#ifdef RDRVR_FRAGMENT_RATE
    gameFoveationDestroy(d); // Variants reference the rate/density maps: destroyed first
    gameFragmentRateDestroy(d);
#endif
#ifdef RDRVR_HEAD_CAMERA
    if(::rdrvr::vr::hands::state()!=::rdrvr::vr::hands::State::Off){ // HANDS: join worker, free every hands object after idle
        std::lock_guard queueGuard(privateQueueMutex);originalIdle(d);::rdrvr::vr::hands::shutdownAfterIdle(d);}
    if(::rdrvr::vr::taa::state()!=::rdrvr::vr::taa::State::Off){ // TAA: join worker, free every TAA object after idle
        std::lock_guard queueGuard(privateQueueMutex);originalIdle(d);::rdrvr::vr::taa::shutdownAfterIdle(d);}
    if(d==sessionBinding.device&&::rdrvr::vr::hud::state()!=::rdrvr::vr::hud::State::Killed){ // HUD: ring / pools / fences after idle
        std::lock_guard queueGuard(privateQueueMutex);originalIdle(d);::rdrvr::vr::hud::shutdownAfterIdle(d);}
#endif
#ifdef RDRVR_HEAD_CAMERA
    ownedStaticOccluders.clear();ownedStaticVertices.clear();coldOcclusionRegistry.clear(d);runtime::graphicsOcclusionAvailable.store(false,std::memory_order_release);
    if(dynamicUniformDescriptors.ready()||cameraOwnerDevice.load(std::memory_order_acquire)==d||uniformOwnerDevice.load(std::memory_order_acquire)==d){
        std::lock_guard queueGuard(privateQueueMutex);
        const auto idle=originalIdle(d);
        if(idle==VK_SUCCESS||idle==VK_ERROR_DEVICE_LOST){
            if(dynamicUniformDescriptors.ready())dynamicUniformDescriptors.shutdownAfterIdle(d);
            ::rdrvr::engine::stereo::shutdownAfterIdle(d);
            uniformShutdownAfterIdle(d,idle);
            cameraRuntime.shutdownAfterIdle(d,idle);
        }else{
            if(dynamicUniformDescriptors.ready())dynamicUniformDescriptors.abandonDevice(d);
        }
    }
#endif
    // Covers retained owners after failed idle/suspended admission too. No
    // Vulkan destroy is performed here, immediately before the native device.
    indexedObservation.disarm("GameXr.cpp:551");uniformAbandonDevice(d);
    cameraRuntime.abandonDevice(d);
    auto cameraDevice=d;cameraOwnerDevice.compare_exchange_strong(cameraDevice,VK_NULL_HANDLE,std::memory_order_acq_rel);
    bridge->destroyDevice(d,a);
#ifdef RDRVR_HEAD_CAMERA
    if(d==sessionBinding.device){
        nativeEyeResolution.installed(false);
        nativeFramebufferExtent.resetAfterIdle();
    }
#endif
}
}
#ifdef RDRVR_GAME_SESSION
VkResult gamePresentVia(VkQueue q,const VkPresentInfoKHR* p,PFN_vkQueuePresentKHR next){
#ifdef RDRVR_PASS_RESOURCES
    worldGameQueue.store(q,std::memory_order_release);
#endif
    return theater.present(q,p,next);
}
void gameTheaterRoute(PFN_vkCreateSwapchainKHR c,PFN_vkDestroySwapchainKHR d,PFN_vkQueuePresentKHR p){theater.route(c,d,p);}
VKAPI_ATTR VkResult VKAPI_CALL gameCreateSwapchain(VkDevice d,const VkSwapchainCreateInfoKHR* c,const VkAllocationCallbacks* a,VkSwapchainKHR* out){return theater.create(d,c,a,out);}
VKAPI_ATTR void VKAPI_CALL gameDestroySwapchain(VkDevice d,VkSwapchainKHR s,const VkAllocationCallbacks* a){theater.destroy(d,s,a);}
VKAPI_ATTR VkResult VKAPI_CALL gamePresent(VkQueue q,const VkPresentInfoKHR* p){
#ifdef RDRVR_PASS_RESOURCES
    worldGameQueue.store(q,std::memory_order_release);
#endif
    return theater.present(q,p);
}
#endif
#ifdef RDRVR_CAMERA_BRIDGE
void gameObserveCamera(std::uintptr_t viewport,std::uint64_t ns,std::uint64_t thread,SceneCameraBridge::Reader reader,void* context) {
#ifdef RDRVR_STEREO_BRIDGE
    eyeViewports.reset();StereoCameraPlan plan{};
    if(cameraBridge.observe(viewport,ns,thread,reader,context,&plan) && viewportApiReady.load(std::memory_order_acquire)) {
        try {auto pair=std::make_unique<EyeViewports>(viewportApi);
            if(pair->prepare(reinterpret_cast<const void*>(viewport),plan)){eyeViewports=std::move(pair);++clonedViewPairs;}
            else ++cloneFailures;
        }catch(...){++cloneFailures;}
    }
#else
    cameraBridge.observe(viewport,ns,thread,reader,context);
#endif
}
#endif
#ifdef RDRVR_PASS_RESOURCES
#ifdef RDRVR_TOUCH_CONTROLS
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
// HOLSTERS: HANDS bridge, VR settings -> holsters config, menu status.
void holsterMarkersToHands(const vr::holsters::MarkerSet& m) noexcept {
    std::array<vr::hands::Marker,vr::hands::MarkerSet::capacity> out{};
    const unsigned n=std::min<unsigned>(m.count,unsigned(out.size()));
    for(unsigned i=0;i<n;++i){for(unsigned j=0;j<3;++j)out[i].position[j]=m.markers[i].position[j];
        out[i].radius=m.markers[i].radius;out[i].highlight=m.markers[i].highlight;out[i].flags=m.markers[i].flags;} // + per-marker flags
    vr::hands::publishMarkers(std::span<const vr::hands::Marker>(out.data(),n),m.referenceSpace,m.sampleNs);
}
void holsterHeldToHands(int hand) noexcept {vr::hands::setHeldHand(hand);}
// RELOAD: the reload overlay -> the hands pass (same layout); a killed hands module = no markers (counted).
void reloadOverlayToHands(const vr::reload::Overlay& o) noexcept {
    if(vr::hands::state()==vr::hands::State::Killed){vr::reload::ctr.overlayUnavailable.fetch_add(1,std::memory_order_relaxed);return;}
    vr::hands::ReloadOverlay h{};static_assert(sizeof(h)==sizeof(o));std::memcpy(&h,&o,sizeof h);vr::hands::publishReloadOverlay(h);
}
void holsterHeldMaskToHands(unsigned mask) noexcept { // DUAL: engine hand (weapon frame) + every held hand (fists)
    vr::hands::setHeldHand(vr::holsters::holdHand.load(std::memory_order_relaxed));vr::hands::setHeldMask(mask);}
void holsterSettingsAndStatus() noexcept {
    namespace vh=vr::holsters;const auto vs=vr::settings();
    vh::setConfig({vs.holsters(),vs.gripToggle()?vh::GripMode::Toggle:vh::GripMode::Hold,
        vs.holsterMarkers()==vr::Snapshot::MarkersAlways?vh::MarkerMode::Always:vs.holsterMarkers()==vr::Snapshot::MarkersOff?vh::MarkerMode::Off:vh::MarkerMode::Near,
        vs.twoHandAim(),vs.dualWield()});
    { // DUAL WIELD row: PROP (debug.rdrvr.vr.dual 0) / UNAVAILABLE / READY / DUAL (a phantom is held); no note while OFF
        const auto ds=vh::dualStatus();
        vr::status::publish(vr::Feature::Dual,ds==vh::DualStatus::Property?vr::status::Property:ds==vh::DualStatus::Unavailable?vr::status::Unavailable:vr::status::Ready,
            ds==vh::DualStatus::Ready||ds==vh::DualStatus::Dual?vh::dualStatusText(ds):nullptr);
    }
    { // THROW WEAPON row: PROP (debug.rdrvr.vr.throw 0) / UNAVAILABLE (holsters not running) / READY / FLYING / ON GROUND / NO PROBE
        namespace vt=vr::throwing;vt::setMenu(vs.throwWeapon());
        const bool ready=vh::engineReady.load(std::memory_order_acquire)&&!vh::killedByProperty.load(std::memory_order_relaxed);
        vr::status::publish(vr::Feature::Throw,vt::killed.load(std::memory_order_relaxed)?vr::status::Property:ready?vr::status::Ready:vr::status::Unavailable,
            ready&&vs.throwWeapon()?vt::statusNote():nullptr);
    }
    { // GUN SPIN row: PROP (debug.rdrvr.vr.spin 0) / UNAVAILABLE / HOLD ONLY / SPINNING / TOUCH / LIGHT PRESS
        const bool ready=vh::engineReady.load(std::memory_order_acquire)&&!vh::killedByProperty.load(std::memory_order_relaxed);
        vr::spin::publishStatus(ready,vs.gunSpin(),vs.gripToggle());
    }
    vr::reload::publishStatus(); // RELOAD: RELOAD MODE row (PROP / UNAVAILABLE / OPEN / 4/6 / NO AMMO / NO CYLINDER / NO FRAME / CONTEXT)
    vr::hammer::publishStatus(); // HAMMER: HAMMER row (PROP / UNAVAILABLE / NO HAMMER / FALLBACK / NEEDS AIM / COCKED / DOWN / SWIPE OVER HAMMER)
    const auto st=vh::status();
    const auto code=st==vh::Status::Property?vr::status::Property:
        (st==vh::Status::Unavailable||st==vh::Status::NoExecutor)?vr::status::Unavailable:vr::status::Ready;
    vr::status::publish(vr::Feature::Holsters,code,code==vr::status::Ready?vh::statusText(st):nullptr);
    vr::status::publish(vr::Feature::TwoHand,vr::killed(vr::Feature::TwoHand)?vr::status::Property:code==vr::status::Ready?vr::status::Ready:vr::status::Unavailable,
        code==vr::status::Ready&&vh::supportHand.load(std::memory_order_relaxed)>=0?"ACTIVE":nullptr);
}
// HMD pose in OpenXR LOCAL (same space as the controller poses) = mid-eye of the latest tracked views of the aim plan.
bool holsterHead(std::uint64_t now,Pose& head){
    StereoCameraPlan plan{};
    if(!controllerAimSpace.plan(plan)||!plan.tracking.publishedNs||now<plan.tracking.publishedNs||now-plan.tracking.publishedNs>150'000'000)return false;
    head=plan.tracking.eyes[0];
    for(unsigned i=0;i<3;++i)head.position[i]=.5f*(plan.tracking.eyes[0].position[i]+plan.tracking.eyes[1].position[i]);
    return validPose(head);
}
void holsterPadFilter(void*,const runtime::ControllerSample* input,std::uint64_t now,runtime::PadState& state){
    holsterSettingsAndStatus();
    const bool world=presentation.snapshot(now).world();
    Pose head{};const bool haveHead=input&&world&&holsterHead(now,head);
    // TELEPORT: while TELEPORT owns the movement stick the holsters see it neutral (the fallback
    // body yaw must not turn while aiming); mapMovement ran first in this pad update.
    runtime::ControllerSample neutral{};
    vr::holsters::padFrame(vr::tploco::stickNeutralView(input,neutral),haveHead?&head:nullptr,world,now,state);
    // Physical weapon hand / support grip changed: restart the attachment continuity.
    static std::uint32_t lastGeneration{};const auto generation=vr::holsters::attachmentGeneration.load(std::memory_order_relaxed);
    if(generation!=lastGeneration){lastGeneration=generation;controllerAttachment.reset();animatedAttachment.reset();scopedModelPose.reset();}
}
// Every weapon-pipeline sample: logical hand 1 = the physical holding hand (+ two-hand barrel).
bool holsterOutputInput(std::uint64_t now,runtime::ControllerSample& input){
    if(!nativePad.outputInput(now,input))return false;
    vr::holsters::weaponInput(input);return true;
}
bool holsterWeaponSample(runtime::ControllerSample& input){vr::holsters::weaponInput(input);return true;}
// Controls: Touch -> game button layout (vr/VrControls.h Mapper). NativePadDriver runs it on
// the native input thread BEFORE the holster filter. Layout: presentation Menu/Unknown = stock pad; otherwise the player's
// state (LocomotionContextReader, 2 = on foot). Jump flag / crouch posture of the player's mover cached by the LOCO mover hook
// (guarded reads; only for the report and the jump latency counters).
void controlsPadFilter(void*,const runtime::ControllerSample* input,std::uint64_t now,runtime::PadState& state){
    namespace vc=vr::controls;
    static_assert(unsigned(runtime::PadA)==vc::bit::A&&unsigned(runtime::PadB)==vc::bit::B&&unsigned(runtime::PadX)==vc::bit::X&&
        unsigned(runtime::PadY)==vc::bit::Y&&unsigned(runtime::PadMenu)==vc::bit::Menu&&unsigned(runtime::PadLeftStick)==vc::bit::L3&&
        unsigned(runtime::PadRightStick)==vc::bit::R3&&unsigned(runtime::PadLeftTrigger)==vc::bit::LT&&unsigned(runtime::PadDpadUp)==vc::bit::DpadUp);
    if(!vc::controlsEnabled.load(std::memory_order_relaxed)){vr::loco::sprintRequest.store(false,std::memory_order_relaxed);return;}
    vc::MapIn in{};in.ready=state.ready&&input;in.buttons=state.buttons;in.now=now;
    if(input){in.stickX=input->sticks[0];in.stickY=input->sticks[1];}
    in.stickOwned=vr::tploco::ownsStick.load(std::memory_order_relaxed);         // TELEPORT: neutral stick for sprint
    in.teleportWalk=vr::tploco::walkModifier.load(std::memory_order_relaxed);   // TELEPORT: L3 = escape-walk modifier
    // LASSO: a catch on the rope -> Touch A = pad A, X = X on foot; a lasso minigame -> the stock face buttons
    in.lassoRemap=vr::lasso::faceRemap();in.lassoMinigame=vr::lasso::minigameFocus();in.promptCode=vr::lasso::promptCode();
    if(in.ready&&in.lassoRemap)vr::lasso::bump(vr::lasso::stats.faceRemapFrames);
    if(in.ready&&in.lassoMinigame)vr::lasso::bump(vr::lasso::stats.stockFaceFrames);
    const auto context=presentation.snapshot(now).context;
    // (user: A no longer skipped cutscenes once they were 3D): a cutscene keeps the stock pad (A = A = skip); the FOOT layout
    // turned A into pad X there because the player still reads as on foot.
    const bool menu=context==runtime::PresentationContext::Menu||context==runtime::PresentationContext::Unknown||context==runtime::PresentationContext::Cutscene;
    LocomotionContext player{};const bool playerRead=!menu&&in.ready&&LocomotionContextReader(locomotionApi).sample(player);
    in.layout=vc::layoutFor(menu,playerRead,player.playerMode);
    if(in.layout==vc::Layout::Foot||in.layout==vc::Layout::Mounted)in.buttons=::rdrvr::vr::controlmap::apply(in.buttons,in.layout==vc::Layout::Mounted);
    const auto mover=vr::loco::playerMover.load(std::memory_order_relaxed);const auto moverNs=vr::loco::moverFrameNs.load(std::memory_order_relaxed);
    std::uint16_t flags{},posture{};const auto& reader=locomotionApi.player;
    if(mover&&moverNs&&now>=moverNs&&now-moverNs<250'000'000ull&&reader.read&&
       reader.read(reader.context,mover+vc::field::moverInstFlags,&flags,sizeof flags)&&
       reader.read(reader.context,mover+vc::field::moverPosture,&posture,sizeof posture)){
        in.jumping=(flags&vc::field::jumpingBit)?1:0;
        const bool crouched=(posture&0xf00u)==0x100u; // sagPlayerInputFoot::IsCrouching 0x26b641c
        if(crouched!=vc::crouching.exchange(crouched,std::memory_order_relaxed))vc::crouchChanges.fetch_add(1,std::memory_order_relaxed);
    }
    vc::jumpFlag.store(in.jumping,std::memory_order_relaxed);
    state.buttons=vc::mapper.apply(in,state.buttons);
    vr::loco::sprintRequest.store(vc::sprintWanted.load(std::memory_order_relaxed),std::memory_order_relaxed);
}
#else
bool holsterOutputInput(std::uint64_t now,runtime::ControllerSample& input){return nativePad.outputInput(now,input);}
bool holsterWeaponSample(runtime::ControllerSample&){return true;}
#endif
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
// MELEE (vr/VrMelee.h): OpenXR LOCAL -> game world of the newest camera plan for the main-thread melee tick. The same
// rigid transform as mapRay / the VR weapon (plan.world[0] o eye0^-1, VrWeapon.h planRotate) plus the body-anchor shift since the
// plan's head sample; head = the plan's eye 0 (LOCAL) and the eye midpoint (world).
bool vrMeleeWorldMap(::rdrvr::vr::melee::WorldMap& out,std::uint64_t now) noexcept {
    namespace vwm=::rdrvr::vr::weapon;
    try{
        StereoCameraPlan plan{};if(!controllerAimSpace.plan(plan))return false;
        const auto& track=plan.tracking;
        if(!plan.sceneId||!track.frameId||!track.publishedNs||now<track.publishedNs||now-track.publishedNs>vwm::config.planMaxAgeNs)return false;
        if(!track.epochs.tracking||!track.epochs.referenceSpace||!::rdrvr::validPose(track.eyes[0]))return false;
        if(!std::isfinite(plan.unitsPerMetre)||plan.unitsPerMetre<.01f||plan.unitsPerMetre>100)return false;
        for(const auto& w:plan.world)for(float x:w)if(!std::isfinite(x))return false;
        runtime::Vec3 shift{};
        if(vwm::config.latency){vwm::AnchorRecord pa{},la{};if(vwm::anchors.find(plan.headSampledNs,pa)&&vwm::anchors.newest(la))vwm::anchorShift(plan,pa,la,shift);}
        ::rdrvr::vr::melee::WorldMap m{};
        for(unsigned c=0;c<3;++c){runtime::Vec3 e{};e[c]=1;const auto r=vwm::planRotate(plan,e);m.r[c]=r[0];m.r[3+c]=r[1];m.r[6+c]=r[2];}
        for(unsigned c=0;c<3;++c){const float l=m.r[c]*m.r[c]+m.r[3+c]*m.r[3+c]+m.r[6+c]*m.r[6+c];if(!(std::abs(l-1.f)<.01f))return false;}
        const auto& eye=track.eyes[0];const auto f=runtime::rotate(eye.orientation,{0,0,-1});
        m.localOrigin={eye.position[0],eye.position[1],eye.position[2]};m.headLocal=m.localOrigin;m.headForwardLocal={f[0],f[1],f[2]};
        m.worldOrigin={plan.world[0][12]+shift[0],plan.world[0][13]+shift[1],plan.world[0][14]+shift[2]};
        m.headWorld={(plan.world[0][12]+plan.world[1][12])*.5f+shift[0],(plan.world[0][13]+plan.world[1][13])*.5f+shift[1],
                     (plan.world[0][14]+plan.world[1][14])*.5f+shift[2]};
        m.units=plan.unitsPerMetre;m.epoch=track.epochs.tracking;m.space=track.epochs.referenceSpace;m.stampNs=track.publishedNs;m.valid=true;
        out=m;return true;
    }catch(...){return false;}
}
// BODY: this frame's controller poses for the arm IK, mapped exactly like the
// VR weapon (VrWeapon.h preparePlan = plan / turn bridge, anchor shift, this frame's capsule prediction; then mapRay), and - for a
// VR-held animated firearm - this frame's BARREL placement A (computePlacement with the alt flag / scale of the last weapon Update),
// kept in vw::bodyPre for the weapon pose of the same hook scope. Any thread (hook A runs on post-physics / motion-tree workers).
bool vrBodyFrame(::rdrvr::vr::bodyik::Frame& out,std::uintptr_t actor,std::uint64_t serial,std::uint64_t now) noexcept {
    namespace vwm=::rdrvr::vr::weapon;namespace AK=::rdrvr::vr::armik;namespace vh=::rdrvr::vr::holsters;
    try{
        out=::rdrvr::vr::bodyik::Frame{};
        vwm::PoseRequest req{};req.now=now;
        // No fresh controller sample (a > 150 ms hitch, a sample published a few us after `now`, the
        // controllers asleep) does not fail the whole frame: the plan / eye still feed the body yaw + head lock, only the hands are
        // invalid.
        const bool input=controllers.snapshot(now,req.raw);
        out.handsInput=input;if(!input)out.why="NO_INPUT";
        StereoCameraPlan plan{};if(controllerAimSpace.plan(plan))req.plan=&plan;
        if(vwm::config.predict==vwm::Predict::Capsule){const auto& api=reticleProbeApi.attachment;
            req.capsuleValid=api.read&&vwm::readCapsule([&](std::uintptr_t p,void* v,std::size_t n){return api.read(api.context,p,v,n);},actor,req.capsule);}
        StereoCameraPlan shifted{};vwm::PlanPrep prep{};
        if(const auto why=vwm::preparePlan(req,shifted,prep)){out.why=vwm::reasonNames[why];return false;}
        out.upm=shifted.unitsPerMetre;
        if(prep.haveLatest)out.up={prep.latest.up[0],prep.latest.up[1],prep.latest.up[2]};
        const auto v3=[](const runtime::Vec3& v){return AK::Vec3{v[0],v[1],v[2]};};
        for(unsigned h=0;input&&h<2;++h){ // No sample -> no hands (head-only frame)
            ControllerRay grip{},aim{};auto& hp=out.hand[h];
            if(vwm::mapRay(shifted,req.raw,h,true,now,vwm::config.planMaxAgeNs,{},grip)!=vwm::Ok||
               vwm::mapRay(shifted,req.raw,h,false,now,vwm::config.planMaxAgeNs,{},aim)!=vwm::Ok)continue;
            // grip frame columns = world directions of the XR grip x / y / z (z = -aim of the grip ray), re-orthonormalised
            const AK::Vec3 z=AK::Normalize(-v3(grip.direction));AK::Vec3 x=AK::Normalize(v3(grip.right));
            const AK::Vec3 y=AK::Normalize(AK::Cross(z,x));x=AK::Cross(y,z);
            hp.frame=AK::Mat33{x,y,z};hp.gripPos=v3(grip.origin);hp.aimFwd=AK::Normalize(v3(aim.direction));
            hp.squeeze=req.raw.grips[h];hp.trigger=req.raw.triggers[h];
            hp.valid=AK::Finite(hp.gripPos)&&AK::Finite(hp.aimFwd)&&AK::Finite(x)&&AK::Finite(y);
        }
        { // the HMD (eye midpoint) and its forward, in the same world
            const auto& w0=shifted.world[0];const auto& w1=shifted.world[1];
            out.eye={(w0[12]+w1[12])*.5f,(w0[13]+w1[13])*.5f,(w0[14]+w1[14])*.5f};
            const auto f=vwm::planRotate(shifted,runtime::rotate(shifted.tracking.eyes[0].orientation,{0,0,-1}));
            out.headFwd=AK::Normalize(v3(f));out.headValid=AK::Finite(out.headFwd)&&AK::Finite(out.eye);
            out.headSampledNs=shifted.headAnchored?shifted.headSampledNs:0; // VrBody holds a warp snap until >= the camera's key K
        }
        { // LOCAL -> world of the same shifted plan (the holster sockets on the shown body go back to LOCAL with its inverse)
            const auto m=vwm::planMap(shifted);
            out.localToWorld={{m[0],m[1],m[2]},{m[4],m[5],m[6]},{m[8],m[9],m[10]},{m[12],m[13],m[14]}};
            out.space=shifted.tracking.epochs.referenceSpace;out.mapValid=out.space&&AK::Finite(out.localToWorld);
        }
        // the engine item in a VR hand
        const int hold=vh::holdHand.load(std::memory_order_acquire);out.hold=hold;out.support=vh::supportHand.load(std::memory_order_relaxed);
        auto o=vwm::ownerSnapshot();
        if(hold>=0&&vwm::config.enabled&&o.weapon&&o.poseAllowed&&o.geometry.valid&&o.geometry.weapon==o.weapon&&
           (!o.stampNs||now<=o.stampNs||now-o.stampNs<=250'000'000)){
            out.weapon=o.weapon;out.firearm=vwm::firearm(o.kind);
            // BODY rule: an animated firearm (authored muzzle locator) is held by its BARREL placement; knife / thrown /
            // lasso / rigid items hang natively in the IK'd hand.
            out.barrel=out.firearm&&o.geometry.animated&&vwm::config.hold!=vwm::HoldMode::Hand;
            const float scale=vwm::lastPoseScale.load(std::memory_order_relaxed);
            if(out.barrel&&scale>0.f&&input){
                // The effective alt (the VR holding side under BODY) - VrBody derives the wrist of THAT side
                // (left VR hand -> wrist_l_attachment), the weapon pose of this scope recomputes the same flag and reuses this placement
                const bool nativeAlt=vwm::lastNativeAlt.load(std::memory_order_relaxed);
                req.hold=hold;req.alt=vwm::effectiveAlt(nativeAlt,hold,o.geometry,::rdrvr::vr::bodyik::bodyVisible());req.scale=scale;req.owner=&o;
                if(req.alt!=nativeAlt)vwm::sideAltFrames.fetch_add(1,std::memory_order_relaxed);
                req.input=req.raw;vh::weaponInput(req.input);
                vwm::Latest l{};
                if(vwm::computePlacement(req,l)==vwm::Ok){
                    const auto& A=l.placement.attachment;
                    out.attachment={{A[0],A[1],A[2]},{A[4],A[5],A[6]},{A[8],A[9],A[10]},{A[12],A[13],A[14]}};
                    out.muzzle=v3(l.placement.origin);out.barrelDir=v3(l.placement.direction);out.alt=req.alt;out.placed=true;
                    vwm::bodyPre={serial,o.weapon,req.alt,req.scale,l};vwm::bodyPreComputed.fetch_add(1,std::memory_order_relaxed);
                }else vwm::bodyPreFailed.fetch_add(1,std::memory_order_relaxed);
            }
        }
        out.valid=true;return true;
    }catch(...){out.why="EXCEPTION";return false;}
}
#endif
#ifdef RDRVR_HEAD_CAMERA
// BODY: the creature the eye BodyDrawFilter scopes hide. While the body is shown (BODY ON, hooks in, head collapse ready) a
// value that never matches a crCreature: the player's body is drawn, the hat node stays hidden.
static std::uintptr_t eyeBodyCreature(std::uintptr_t creature) noexcept {
#if defined(RDRVR_NATIVE_MENUS) && defined(RDRVR_TOUCH_CONTROLS)
    return ::rdrvr::vr::bodyik::bodyVisible()?::rdrvr::vr::bodyik::showBodyCreature:creature;
#else
    return creature;
#endif
}
// BODY: the hat node hidden outside the eye scopes too (shadow maps) while the body is shown with its head collapsed (the shared
// skin matrices take the head out of the shadow as well, the hat would float). 0 = off.
static std::uintptr_t globalHatNode(std::uintptr_t hat) noexcept {
#if defined(RDRVR_NATIVE_MENUS) && defined(RDRVR_TOUCH_CONTROLS)
    // With the head back in the shadow (HEAD SHADOW live) the hat stays in the shadow too
    if(::rdrvr::vr::bodyik::headShadowLive.load(std::memory_order_relaxed)&&::rdrvr::vr::bodyik::headFillInstalled.load(std::memory_order_relaxed))return 0;
    return ::rdrvr::vr::bodyik::bodyVisible()?hat:0;
#else
    (void)hat;return 0;
#endif
}
#endif
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
// THROW glue (defined with the weapon glue below).
bool vrThrowStart(const vr::throwing::StartRequest& r,vr::throwing::StartPose& out) noexcept;
bool vrThrowProbe(std::uint32_t actorGuid,const vr::throwing::V3& from,const vr::throwing::V3& to,bool& blocked,vr::throwing::Hit& out) noexcept;
bool vrThrowMap(std::uint64_t now,vr::throwing::Map& out) noexcept;
bool vrThrowPlayer(std::uintptr_t actor,vr::throwing::V3& capsule) noexcept;
std::uintptr_t vrThrowOwnerWeapon() noexcept;
// ARC glue (defined with the throw glue below).
bool vrArcAnchor(std::uint64_t now,vr::arc::AnchorInputs& out) noexcept;
bool vrArcMap(std::uint64_t now,vr::arc::MapInputs& out) noexcept;
bool vrArcInput(std::uint64_t now,runtime::ControllerSample& raw) noexcept;
bool vrArcSynthesizing() noexcept;
#endif
void gameConfigureInput(NativePadApi api) noexcept {
    nativePad.configure(api);
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
    if(vr::holsters::startupEnabled())nativePad.setPadFilter(holsterPadFilter); // debug.rdrvr.vr.holsters 0 = no filter
    vr::holsters::markerSink=holsterMarkersToHands;vr::holsters::heldHandSink=holsterHeldToHands; // HANDS bridge
    vr::holsters::heldMaskSink=holsterHeldMaskToHands; // DUAL: both held hands (engine item + phantom) drawn as fists
    vr::reload::overlaySink.store(&reloadOverlayToHands,std::memory_order_release); // RELOAD: overlay channel 2b
    vr::weapon::startup(); // debug.rdrvr.vr.weapon* properties (before any weapon hook / holster tick can run)
    vr::controls::loadProperties();nativePad.setControlsFilter(controlsPadFilter); // Controls (debug.rdrvr.vr.controls 0 = stock layout)
    vr::melee::worldMapFn.store(&vrMeleeWorldMap,std::memory_order_release); // MELEE: LOCAL -> world for the melee tick
    vr::bodyik::frameFn.store(&vrBodyFrame,std::memory_order_release); // BODY: this frame's controller poses for the arm IK
    // THROW (VrThrow.h): start pose from the release placement, the laser's probe, LOCAL<->world, the player's capsule, the owner
    vr::throwing::startFn.store(&vrThrowStart,std::memory_order_release);vr::throwing::probeFn.store(&vrThrowProbe,std::memory_order_release);
    vr::throwing::mapFn.store(&vrThrowMap,std::memory_order_release);vr::throwing::playerFn.store(&vrThrowPlayer,std::memory_order_release);
    vr::throwing::ownerWeaponFn.store(&vrThrowOwnerWeapon,std::memory_order_release);
    // ARC: the per-tick Context providers + ModuleArc (DEBUG producer, live levers) +
    // the AppSW probe of the hands pass. The engine probe pointers are bound in gameConfigureReticleProbe (after the TraceEntry prints).
    vr::arc::anchorFn.store(&vrArcAnchor,std::memory_order_release);vr::arc::mapFn.store(&vrArcMap,std::memory_order_release);
    vr::arc::inputFn.store(&vrArcInput,std::memory_order_release);
    if(!vr::arc::install())vr::arc::bump(vr::arc::moduleStats.exceptions); // refused = a different handler already owns ModuleArc (counted)
    vr::hands::setSynthesizingProbe(&vrArcSynthesizing);
#if __has_include("vr/VrReload.h")
    vr::controls::reloadConsumer.store(&vr::reload::consumeB,std::memory_order_release);
#endif
#endif
}
void gameConfigureRumble(NativeRumbleApi api) noexcept {rumbleApi=api;}
void gameObserveRumble(void* pad,int actuator,float value,std::uint64_t now) noexcept {
    try{
        runtime::ControllerSample input{};runtime::HapticSample sample{};
        if(!nativePad.outputInput(now,input)||!readNativeRumble(rumbleApi,reinterpret_cast<std::uintptr_t>(pad),actuator,value,sample.amplitude)){
            ++hapticDeclined;haptics.invalidate();return;
        }
        sample.stamp=now;sample.inputEpoch=input.inputEpoch?input.inputEpoch:input.epoch;sample.continuity=input.continuity;
        haptics.publish(sample);
    }catch(...){haptics.invalidate();++hapticDeclined;}
}
#ifdef RDRVR_HEAD_CAMERA
void gameConfigureLocomotion(LocomotionApi api) noexcept {locomotionApi=api;nativePad.setMovement(mapMovement);locomotionState="WAITING_FOR_WORLD";}
#ifdef RDRVR_NATIVE_MENUS
void gameConfigureSnapTurn(NativeCameraTurnApi api) noexcept {
    snapTurnApi=api;nativePad.setHorizontalLook(api?mapHorizontalLook:nullptr);snapTurnState=api?"WAITING_FOR_WORLD":"NOT_CONFIGURED";
    snapTurnReady.store(bool(api),std::memory_order_release);
    teleport::viewAvailable.store(bool(api),std::memory_order_release);
    teleport::viewTurn.store(api?&teleportViewTurn:nullptr,std::memory_order_release); // Main thread, no controller needed
}
#endif
void gameConfigureBodyAttachments(BodyAttachmentApi api) noexcept {bodyAttachmentApi=api;}
void gameConfigureBodyFilter(BodyDrawFilter::Original original,BodyDrawFilter::Reader read) noexcept {bodyDrawFilter.configure(original,read);}
void gameBodyFilterHooksReady(bool ready) noexcept {bodyDrawReady.store(ready,std::memory_order_release);}
void gameBodyDraw(void* container,const void* info,int commands,int instance,float fade,unsigned flags,unsigned extra,bool pinned){
#ifdef RDRVR_HEAD_CAMERA
    auto* early=experimental::NativeEarlyTraceRoute::controller();
    if(!early||!early->active()){
        experimental::NativeEarlyTraceRoute::opaqueBeforeOriginal();
        bodyDrawFilter.draw(container,info,commands,instance,fade,flags,extra,pinned);return;
    }
    std::uint32_t fadeBits{};std::memcpy(&fadeBits,&fade,sizeof fade);
    const bool exactMain=pinned&&(!early->depth()||experimental::NativeEarlyTraceRoute::mainTiles());
    experimental::NativeEarlyFrameController::Object occlusion(*early,{reinterpret_cast<std::uintptr_t>(container),reinterpret_cast<std::uintptr_t>(info),commands,instance,fadeBits,flags,extra},exactMain);
    if(occlusion.skip()){experimental::NativeEarlyTraceRoute::opaqueCommitSkipped();return;}
    experimental::NativeEarlyTraceRoute::opaqueBeforeOriginal();
#endif
    bodyDrawFilter.draw(container,info,commands,instance,fade,flags,extra,pinned);
#ifdef RDRVR_HEAD_CAMERA
    occlusion.complete();
#endif
}
#endif
void gameConfigureWeaponObservation(WeaponOwnerApi api) noexcept {weaponObservationApi=api;weaponOwner.configure(api);weaponCallTrace.configure(api);weaponObservationReady.store(bool(api),std::memory_order_release);}
void gameNativeReticleReady(bool ready) noexcept {nativeReticleReady.store(ready,std::memory_order_release);}
void gameConfigureWeaponModel(WeaponModelBindingApi api,bool ready) noexcept {
    weaponModelApi=api;weaponModelReady.store(ready,std::memory_order_release);
}
// ===== WEAPON glue =====
// The weapon in a VR hand (holsters::holdHand >= 0) is placed here, BEFORE the qualified attachment pipeline, on ANY thread.
enum class WeaponObserveGap : int {None=0,NoContext=1,NoWorld=2};
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
namespace vw=::rdrvr::vr::weapon;
// Main thread (PostCameraUpdate, after weaponOwner.inspect + context): authored muzzle/locator geometry of the held weapon.
bool vrWeaponGeometry(const LocalWeapon& owner,const vw::Geometry&,vw::Geometry& out) noexcept {
    try{
        const auto& api=reticleProbeApi.attachment;
        if(!reticleProbeReady.load(std::memory_order_acquire)||!api.read||!api.muzzleMatrix||!api.worldMatrix){vw::geometryState="NO_NATIVE_GETTERS";return false;}
        const auto get=[&](std::uintptr_t p,std::size_t off,auto& v){return p&&api.read(api.context,p+off,&v,sizeof v);};
        std::uintptr_t table{},base{},locator{};std::uint32_t modelGuid{},flags{};
        if(!get(owner.weapon,0,table)||table!=api.weaponTable||!get(owner.weapon,0x28,base)||!get(owner.weapon,0x338,locator)||
           !get(owner.weapon,0x2bc,modelGuid)||!get(owner.weapon,0x120,flags)){vw::geometryState="WEAPON_UNREADABLE";return false;}
        const bool animated=(flags&0x20)!=0;WeaponNativeMatrix l0{},l1{};l0.values=vw::identity4;l1.values=vw::identity4;
        if(locator){ // weapWeapon::Update: animated [+0x338]+0xc0 / +0x100 (alt-handed), rigid [+0x338]+0x80
            if(animated?(!get(locator,0xc0,l0)||!get(locator,0x100,l1)):!get(locator,0x80,l0)){vw::geometryState="LOCATOR_UNREADABLE";return false;}
            if(!animated)l1=l0;
        }
        const auto* root=api.worldMatrix(reinterpret_cast<const void*>(owner.weapon),false);
        WeaponNativeMatrix r0{},r1{},muzzle{};
        if(!root||!get(reinterpret_cast<std::uintptr_t>(root),0,r0)){vw::geometryState="ROOT_UNREADABLE";return false;}
        for(float& x:muzzle.values)x=std::numeric_limits<float>::quiet_NaN();
        api.muzzleMatrix(reinterpret_cast<const void*>(owner.weapon),&muzzle,false);
        if(api.worldMatrix(reinterpret_cast<const void*>(owner.weapon),false)!=root||!get(reinterpret_cast<std::uintptr_t>(root),0,r1)||
           vw::affine(r0.values)!=vw::affine(r1.values)){vw::geometryState="ROOT_MOVED";return false;}
        vw::Geometry g{};
        if(!vw::buildGeometry(r0.values,muzzle.values,l0.values,l1.values,g)){vw::geometryState="NOT_RIGID";return false;}
        g.animated=animated;g.weapon=owner.weapon;g.base=base;g.locatorPtr=locator;g.modelGuid=modelGuid;g.flags=flags;
        if(vw::config.handBone){ // Engine wrist joint in the attachment bone frame (actor skeleton; melee's verified chain API)
            const auto& ma=::rdrvr::vr::melee::engine.api();
            vw::refreshHandBones([&](std::uintptr_t p,void* v,std::size_t n){return api.read(api.context,p,v,n);},owner.actor,owner.item,
                                 ma.entityVtable,ma.instSkeletonFn,g.bone);}
        out=g;vw::geometryState="OK";return true;
    }catch(...){vw::geometryState="EXCEPTION";return false;}
}
// HAMMER: the engine facts of the VR-held revolver + the override write (restore first), after
// publishOwner (forceAllowed of this frame); with no VR-held weapon an empty call (the override of a weapon that left the hand is restored).
void vrHammerObserve(const LocalWeapon* owner,const WeaponContext* context,std::uint64_t now) noexcept {
    namespace vhm=::rdrvr::vr::hammer;
    try{
        if(!vhm::started.load(std::memory_order_acquire))return;
        vhm::ObserveIn in{};in.now=now;
        if(owner&&context&&::rdrvr::vr::holsters::holdHand.load(std::memory_order_acquire)>=0){
            const auto o=vw::ownerSnapshot();
            in.weapon=owner->weapon;in.actor=owner->actor;in.player=owner->player;in.actorGuid=owner->actorGuid;in.kind=int(owner->type.kind);
            in.typeId=int(owner->type.id);in.held=o.weapon==owner->weapon;in.forceAllowed=in.held&&o.forceAllowed;in.poseAllowed=in.held&&o.poseAllowed;
            in.riding=context->riding;
            const auto l=vw::latestPlacement();
            if(l.weapon==owner->weapon&&l.ns){in.barrel={l.placement.direction[0],l.placement.direction[1],l.placement.direction[2]};in.barrelNs=l.ns;in.barrelValid=true;}
        }
        vhm::observe(in);
    }catch(...){}
}
void vrWeaponObserve(const LocalWeapon* owner,const WeaponContext* context,std::uint64_t now,WeaponObserveGap gap=WeaponObserveGap::None) noexcept {
    try{
        { // RELOAD: the context word + the on-hand weapon for the manual gate and the RequestReload hook,
          // BEFORE the holdHand early return (an on-hand weapon VR does not hold still vetoes /
          // un-vetoes the hook: stagecoach, wagon, ...).
            namespace vrl=::rdrvr::vr::reload;vrl::Context c{};c.gap=int(gap);
            if(owner&&context){c.valid=true;c.weapon=owner->weapon;c.kind=int(owner->type.kind);c.alive=context->alive;c.controlDisabled=context->controlDisabled;
                c.deadEye=context->deadEye;c.riding=context->riding;c.ridingVehicle=context->ridingVehicle;c.vehicleTransition=context->vehicleTransition;
                c.duel=context->duel;c.mountedWeapon=context->mountedWeapon;c.mountingHorse=context->mountingHorse;c.linkedAnimation=context->linkedAnimation;
                c.usingGringo=context->usingGringo;c.cover=context->cover;}
            vrl::noteContext(c,now);
        }
        if(!vw::config.enabled||!owner||!context||::rdrvr::vr::holsters::holdHand.load(std::memory_order_acquire)<0){
            vw::clearOwner();::rdrvr::vr::holsters::heldFirearm.store(false,std::memory_order_relaxed);
            ::rdrvr::vr::holsters::heldKind.store(-1,std::memory_order_relaxed);vw::clearPhantomOwner();vrHammerObserve(nullptr,nullptr,now);return;}
        vw::publishOwner(*owner,*context,now,[&](const vw::Geometry& old,vw::Geometry& out){return vrWeaponGeometry(*owner,old,out);});
        vrHammerObserve(owner,context,now); // HAMMER (after this frame's forceAllowed)
        // DUAL: the phantom item's own weapWeapon (inventory slot item +0xa0, verified table/owner guid, type from the enum tree)
        // with the player's context; its geometry through the same getters (the geometry state text stays the engine item's).
        namespace vh=::rdrvr::vr::holsters;
        const auto pw=vh::phantomWeapon.load(std::memory_order_acquire),pi=vh::phantomItem.load(std::memory_order_relaxed);
        const int ph=vh::phantomHand.load(std::memory_order_acquire);
        if(!pw||!pi||ph<0){vw::clearPhantomOwner();return;}
        LocalWeapon p{};p.player=owner->player;p.actor=owner->actor;p.inventory=owner->inventory;p.actorGuid=owner->actorGuid;p.item=pi;p.weapon=pw;
        static std::uintptr_t typedWeapon{};static WeaponType typed{};static unsigned typedAge{};
        if(typedWeapon!=pw||++typedAge%30u==0){
            const auto& api=weaponObservationApi;std::uintptr_t table{};std::uint32_t guid{};WeaponType t{};
            const bool ok=api.read&&api.read(api.context,pw,&table,sizeof table)&&table==api.weaponTable&&
                api.read(api.context,pw+0x20,&guid,sizeof guid)&&guid==owner->actorGuid&&WeaponTypeReader(api.typeApi).inspect(pw,t);
            if(!ok){typedWeapon=0;vw::clearPhantomOwner();vw::phantomGeometryState.store("PHANTOM_UNREADABLE",std::memory_order_relaxed);return;}
            typedWeapon=pw;typed=t;typedAge=0;
        }
        p.type=typed;
        vw::publishPhantom(p,*context,ph,now,[&](const vw::Geometry& old,vw::Geometry& out){
            const char* saved=vw::geometryState.load(std::memory_order_relaxed);const bool ok=vrWeaponGeometry(p,old,out);
            vw::phantomGeometryState.store(vw::geometryState.load(std::memory_order_relaxed),std::memory_order_relaxed);
            vw::geometryState.store(saved,std::memory_order_relaxed);return ok;});
    }catch(...){}
}
// Drops the engine's per-frame camera launch override (+0x9c2, set by sagPlayer::UpdateAttack) of the VR-held weapon
// so ComputeMainMuzzlePosition returns the VR muzzle (VrWeapon.h "SetProjectileLaunchOverride"). site: Pose = before the weapon's
// Update (aniAim::Fire runs inside it), Aim = before the replaced Aim, Fire = observation only (still set at FireProjectile).
void vrClearLaunch(std::uintptr_t weapon,vw::LaunchSite site) noexcept {
    try{
        const auto& api=reticleProbeApi.attachment;std::uint8_t flags[2]{};
        if(!weapon||!api.read||!api.read(api.context,weapon+vw::weaponLaunchOverride,flags,sizeof flags)){vw::launchUnreadable.fetch_add(1,std::memory_order_relaxed);return;}
        if(!flags[0])return;
        vw::launchSeen[site].fetch_add(1,std::memory_order_relaxed);
        if(flags[1]){vw::launchDrv.fetch_add(1,std::memory_order_relaxed);return;} // scripted DrV override: the setter keeps it, so it stays
        const auto set=vw::setLaunchOverride.load(std::memory_order_acquire);
        if(site==vw::LaunchFire||!vw::config.launch||!set)return;
        static constexpr float identity[16]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        set(reinterpret_cast<void*>(weapon),false,identity);vw::launchCleared[site].fetch_add(1,std::memory_order_relaxed);
    }catch(...){}
}
// Manual reload stage 1: the cylinder bone of the VR-held revolver, after this Update's native pose (VrReload.h).
void vrWeaponReload(const WeaponPoseCall& call,bool placed) noexcept {
    namespace vrl=::rdrvr::vr::reload;
    try{
        if(!vrl::config.enabled||!vrl::started.load(std::memory_order_relaxed))return;
        const auto weapon=reinterpret_cast<std::uintptr_t>(call.weapon);const auto o=vw::ownerSnapshot();
        (void)placed; // the swing follows the hold, not this frame's placement (a declined frame must not snap the cylinder shut)
        const bool held=o.weapon==weapon&&::rdrvr::vr::holsters::holdHand.load(std::memory_order_acquire)>=0&&
            vrl::revolverKind(int(o.kind))&&o.geometry.valid&&o.geometry.weapon==weapon;
        if(!held&&!vrl::modified(weapon))return;
        const auto& api=reticleProbeApi.attachment;if(!api.skeleton||!api.read)return;
        const auto skeleton=reinterpret_cast<std::uintptr_t>(api.skeleton(call.weapon,false));
        vrl::Engine e{};e.read=api.read;e.context=api.context;
        const auto now=diagnostics::monotonicNs();
        // RELOAD: the placement the drawn gun used this Update -> the ChamberFrame in the holding grip frame
        vrl::GripMap gm{};
        if(held){const auto l=vw::latestPlacement();const int hold=::rdrvr::vr::holsters::holdHand.load(std::memory_order_acquire);
            if(l.weapon==weapon&&l.hand==hold&&l.ns&&now>=l.ns&&now-l.ns<=100'000'000ull){static_assert(sizeof(l.map)==sizeof(gm.map));
                std::memcpy(gm.map.data(),l.map.data(),sizeof gm.map);gm.grip=l.localGrip;gm.hand=l.hand;gm.referenceSpace=l.referenceSpace;gm.ns=l.ns;gm.valid=true;}
            else vrl::ctr.latestMiss.fetch_add(1,std::memory_order_relaxed);}
        vrl::apply(e,weapon,skeleton,o.geometry.modelMuzzle,held,now,&gm);
    }catch(...){}
}
// HAMMER: inside this weapon's own Update, after the reload's swing: the Zone (the spur in
// the holding grip frame of the placement this Update used), the native hammer trace, the self-gated visual H2 (restored once the gun leaves the hand).
void vrWeaponHammer(const WeaponPoseCall& call) noexcept {
    namespace vhm=::rdrvr::vr::hammer;namespace vrl=::rdrvr::vr::reload;
    try{
        if(!vhm::started.load(std::memory_order_acquire))return;
        const auto weapon=reinterpret_cast<std::uintptr_t>(call.weapon);const auto o=vw::ownerSnapshot();
        const int hold=::rdrvr::vr::holsters::holdHand.load(std::memory_order_acquire);
        const bool held=o.weapon==weapon&&hold>=0&&vrl::revolverKind(int(o.kind))&&o.geometry.valid&&o.geometry.weapon==weapon;
        if(!held&&!vhm::written(weapon))return;
        const auto& api=reticleProbeApi.attachment;if(!api.skeleton||!api.read)return;
        vrl::Engine e{};e.read=api.read;e.context=api.context;
        const auto now=diagnostics::monotonicNs();
        vrl::GripMap gm{};
        if(held){const auto l=vw::latestPlacement();
            if(l.weapon==weapon&&l.hand==hold&&l.ns&&now>=l.ns&&now-l.ns<=100'000'000ull){static_assert(sizeof(l.map)==sizeof(gm.map));
                std::memcpy(gm.map.data(),l.map.data(),sizeof gm.map);gm.grip=l.localGrip;gm.hand=l.hand;gm.referenceSpace=l.referenceSpace;gm.ns=l.ns;gm.valid=true;}}
        vhm::PoseIn in{};in.weapon=weapon;in.skeleton=reinterpret_cast<std::uintptr_t>(api.skeleton(call.weapon,false));in.held=held;in.typeId=int(o.typeId);
        in.generation=::rdrvr::vr::holsters::holdGeneration.load(std::memory_order_relaxed);in.now=now;in.modelMuzzle=o.geometry.modelMuzzle;in.grip=gm.valid?&gm:nullptr;
        vhm::pose(e,in);
    }catch(...){}
}
// weapWeapon::ManagerUpdate entry (ManagerAim -> weapSimpleAimer::Aim also reads ComputeMainMuzzlePosition).
void vrWeaponManager(void* weapon) noexcept {
    try{
        if(!vw::config.enabled||!vw::started.load(std::memory_order_relaxed))return;
        const auto o=vw::ownerSnapshot();
        if(o.weapon&&o.weapon==reinterpret_cast<std::uintptr_t>(weapon)&&vw::firearm(o.kind)&&o.aimAllowed&&
           ::rdrvr::vr::holsters::holdHand.load(std::memory_order_acquire)>=0){
            vrClearLaunch(o.weapon,vw::LaunchManager);
            // Barrel target in vector mode (overrides lock-on Aim(gohGUID) / AimToMiss) + zero spread, before ManagerAim/fire.
            const auto& api=reticleProbeApi.attachment;
            if(vw::config.aimFix&&api.read)vw::forceBarrelAim([&](std::uintptr_t p,void* v,std::size_t n){return api.read(api.context,p,v,n);},
                &vw::processWrite,o.weapon,vw::latestPlacement(),diagnostics::monotonicNs(),!vw::config.spread);
        }
    }catch(...){}
}
void vrWeaponFire(const WeaponFireCall& call,std::uint64_t now) noexcept {
    try{
        if(!vw::config.enabled||!vw::started.load(std::memory_order_relaxed))return;
        const auto weapon=reinterpret_cast<std::uintptr_t>(call.weapon);vw::noteFire(weapon,now);
        ::rdrvr::vr::reload::noteEngineFire(weapon,now); // RELOAD: counted only (the executor applies it to the chamber model)
        if(weapon&&weapon==vw::ownerSnapshot().weapon){vrClearLaunch(weapon,vw::LaunchFire);
            ::rdrvr::vr::throwing::lastShotNs.store(now,std::memory_order_relaxed); // THROW: no throw within 250 ms of a shot (recoil flicks)
            const auto& api=reticleProbeApi.attachment; // Engine launch direction vs this frame's barrel
            if(api.read)vw::observeShot([&](std::uintptr_t p,void* v,std::size_t n){return api.read(api.context,p,v,n);},weapon,vw::latestPlacement(),now);}
    }catch(...){}
}
// DUAL: the phantom's own holstered weapWeapon::Update (same pinned caller): the placement of the phantom hand through the SAME
// pipeline as the engine item (hold mode, calibration, rigid fix, capsule prediction), no launch override / aim / reload.
bool vrPhantomPose(const WeaponPoseCall& call,WeaponNativeMatrix& matrix,std::uint64_t now) noexcept {
    try{
        auto o=vw::phantomSnapshot();const auto cw=reinterpret_cast<std::uintptr_t>(call.weapon);
        const int hand=::rdrvr::vr::holsters::phantomHand.load(std::memory_order_acquire);
        if(o.weapon!=cw){ // just swapped (the old engine item is the phantom now): the observer re-publishes at PostCameraUpdate
            const auto e=vw::ownerSnapshot();if(!cw||e.weapon!=cw)return vw::phantomDecline(vw::NoOwner,now);
            static_cast<vw::Owner&>(o)=e;o.hand=hand;o.aimAllowed=o.forceAllowed=false;vw::phantomAdopted.fetch_add(1,std::memory_order_relaxed);}
        vw::phantomCalls.fetch_add(1,std::memory_order_relaxed);
        const auto no=[&](vw::Reason why){vw::phantomDecline(why,now);vw::Latest last{};
            if(hand<0||!o.poseAllowed||!vw::phantomHoldLastPlacement(o.weapon,why,now,last))return false;
            matrix.values=last.placement.attachment;const auto scope=WeaponPoseCallBoundary::scope();
            vw::pending={scope.serial,o.weapon,last.placement.model,true,false,o.geometry.animated};return true;};
        if(hand<0||hand>1)return no(vw::NotHeld);
        if(!o.stampNs||(now>o.stampNs&&now-o.stampNs>250'000'000))return no(vw::StaleOwner);
        if(!o.poseAllowed)return no(vw::attachable(o.kind,vw::config)?vw::Context:vw::Class);
        if(!o.geometry.valid||o.geometry.weapon!=o.weapon)return no(vw::NoGeometry);
        vw::PoseRequest req{};req.now=now;req.hold=hand;req.alt=false;req.owner=&o;
        if(!controllers.snapshot(now,req.raw))return no(vw::NoInput);
        req.input=req.raw;::rdrvr::vr::holsters::phantomInput(req.input); // logical hand 1 = the phantom hand
        StereoCameraPlan plan{};if(controllerAimSpace.plan(plan))req.plan=&plan;
        runtime::Mat4 source{},unit{};
        if(!WeaponAttachmentReader(reticleProbeApi.attachment).argument(call.attachment,source)||!deriveModelMuzzle(source,source,unit,req.scale))return no(vw::BadArgument);
        if(vw::config.predict==vw::Predict::Capsule){const auto& api=reticleProbeApi.attachment;
            req.capsuleValid=api.read&&vw::readCapsule([&](std::uintptr_t p,void* v,std::size_t n){return api.read(api.context,p,v,n);},o.actor,req.capsule);}
        vw::Latest l{};
        if(const auto why=vw::computePlacement(req,l))return no(why);
        matrix.values=l.placement.attachment;
        { // Holster-locator correction (vw::PhantomFix)
            std::lock_guard lock(vw::phantomFix.m);
            if(vw::phantomFix.valid&&vw::phantomFix.weapon==o.weapon){auto a=runtime::multiply(l.placement.model,vw::phantomFix.k);
                if(vw::rigidize(a,req.scale)){matrix.values=a;vw::phantomFixApplied.fetch_add(1,std::memory_order_relaxed);}else vw::phantomFixBad.fetch_add(1,std::memory_order_relaxed);}
        }
        const auto scope=WeaponPoseCallBoundary::scope();
        vw::pending={scope.serial,o.weapon,l.placement.model,true,false,o.geometry.animated,true,matrix.values};
        vw::publishPhantomPlacement(l);vw::phantomPlaced.fetch_add(1,std::memory_order_relaxed);
        vw::phantomLastHand.store(hand,std::memory_order_relaxed);vw::phantomLastNs.store(now,std::memory_order_relaxed);
        return true;
    }catch(...){vw::pending=vw::Pending{};return vw::phantomDecline(vw::Exception,now);}
}
// ===== THROW glue =====
// Main thread (holsters OpThrow, before the put-away): the gun model at release = the item's last placement (engine item: `latest`,
// DUAL phantom: its own), that placement's LOCAL->world map, the flying point (35 % from the grip towards the muzzle, model space), the
// rest axes (barrel = muzzle locator -Z, up = its +Y); the item's owner is kept for the catching-hand pose.
bool vrThrowStart(const vr::throwing::StartRequest& r,vr::throwing::StartPose& out) noexcept {
    namespace vt=::rdrvr::vr::throwing;
    try{
        const auto l=r.phantom?vw::phantomPlacement():vw::latestPlacement();
        if(!l.weapon||l.weapon!=r.weapon||!l.ns)return false;
        const vw::Owner o=r.phantom?static_cast<vw::Owner>(vw::phantomSnapshot()):vw::ownerSnapshot();
        if(o.weapon!=r.weapon||!o.geometry.valid||o.geometry.weapon!=o.weapon)return false;
        const auto& p=l.placement;
        // the locator this placement used (VrWeapon place(): HAND = the holding side, BARREL = the alt flag of the last Update)
        const bool alt=!r.phantom&&vw::lastPoseAlt.load(std::memory_order_relaxed);
        const unsigned li=p.hand?(r.hand==0?1u:0u):(alt?1u:0u);
        runtime::Mat4 inv{};if(!vw::affineInverse(o.geometry.locator[li],inv))return false;
        const auto& mm=o.geometry.modelMuzzle;
        out=vt::StartPose{};
        for(unsigned i=0;i<16;++i){out.model[i]=p.model[i];out.map[i]=l.map[i];}
        if(!r.phantom){vt::M4 spun{};if(::rdrvr::vr::spin::spunFor(r.weapon,r.now,spun))out.model=spun;} // SPIN: the flight starts where the spinning gun is
        for(unsigned i=0;i<3;++i)out.centre[i]=inv[12+i]+(mm[12+i]-inv[12+i])*.35f; // inv origin = the hand bone (grip) in model space
        out.barrel=vt::math::normalized({-mm[8],-mm[9],-mm[10]});out.gunUp=vt::math::normalized({mm[4],mm[5],mm[6]});
        out.upm=p.unitsPerMetre>0.f?p.unitsPerMetre:1.f;out.ns=l.ns;out.actorGuid=o.actorGuid;out.actor=o.actor;out.animated=o.geometry.animated;
        vw::AnchorRecord la{};
        if(vw::anchors.newest(la)){out.up={la.up[0],la.up[1],la.up[2]};if(la.velocityValid)out.moverVelocity={la.velocity[0],la.velocity[1],la.velocity[2]};}
        if(!vt::math::finite(out.centre)||vt::math::length(out.barrel)<.5f||vt::math::length(out.up)<.5f)return false;
        vw::setThrowOwner(o);
        return true;
    }catch(...){return false;}
}
// Main thread (ModuleThrow tick): the laser's engine probe between two world points (player excluded). Normal = intersection +0x20 [H].
bool vrThrowProbe(std::uint32_t actorGuid,const vr::throwing::V3& from,const vr::throwing::V3& to,bool& blocked,vr::throwing::Hit& out) noexcept {
    namespace vt=::rdrvr::vr::throwing;
    try{
        blocked=false;out=vt::Hit{};
        if(!reticleProbeReady.load(std::memory_order_acquire)||!actorGuid)return false;
        AimNativeVector a{},b{};for(unsigned i=0;i<3;++i){a.v[i]=from[i];b.v[i]=to[i];}
        NativeAimHit h{};
        // ARC: VrThrow flight casts are a hard user of the probe ledger (never refused, charged, they shrink the next soft budget)
        const auto t0=diagnostics::monotonicNs();
        const bool cast=NativeAimProbe(reticleProbeApi.probe).castBetween(actorGuid,a,b,h);
        {namespace va=::rdrvr::vr::arc;const float metres=va::math::distance({from[0],from[1],from[2]},{to[0],to[1],to[2]});
         va::ledger.take(va::ProbeUser::ThrowFlight);va::ledger.charge(va::ProbeUser::ThrowFlight,diagnostics::monotonicNs()-t0,metres,!cast);}
        if(!cast)return false;
        blocked=h.blocked;
        if(h.blocked){
            for(unsigned i=0;i<3;++i)out.point[i]=h.point.v[i];
            float n[3]{};std::memcpy(n,h.intersection.bytes.data()+0x20,sizeof n);
            out.normal={n[0],n[1],n[2]};const float len=vt::math::length(out.normal);out.normalValid=std::isfinite(len)&&len>.9f&&len<1.1f;
        }
        return true;
    }catch(...){return false;}
}
// Main thread: OpenXR LOCAL (metres) -> world of this frame (the weapon's plan preparation) + its reference space (catch target).
bool vrThrowMap(std::uint64_t now,vr::throwing::Map& out) noexcept {
    try{
        vw::PoseRequest req{};req.now=now;
        StereoCameraPlan plan{};if(controllerAimSpace.plan(plan))req.plan=&plan;
        StereoCameraPlan shifted{};vw::PlanPrep prep{};
        if(vw::preparePlan(req,shifted,prep)!=vw::Ok)return false;
        const auto m=vw::planMap(shifted);for(unsigned i=0;i<16;++i)out.localToWorld[i]=m[i];
        out.referenceSpace=shifted.tracking.epochs.referenceSpace;return out.referenceSpace!=0;
    }catch(...){return false;}
}
bool vrThrowPlayer(std::uintptr_t actor,vr::throwing::V3& capsule) noexcept {
    try{
        const auto& api=reticleProbeApi.attachment;runtime::Vec3 c{};
        if(!actor||!api.read||!vw::readCapsule([&](std::uintptr_t p,void* v,std::size_t n){return api.read(api.context,p,v,n);},actor,c))return false;
        capsule={c[0],c[1],c[2]};return true;
    }catch(...){return false;}
}
std::uintptr_t vrThrowOwnerWeapon() noexcept {try{return vw::ownerSnapshot().weapon;}catch(...){return 0;}}
// ===== ARC glue =====
// Main thread (a mainthread tick, UpdateInputs N): the head anchor PostCameraUpdate N published - the very snapshot gameBeginVisibility
// takes at Draw+0x10c of this Execute iteration, so key == the head sample of the eye pair Draw N renders. The player's GUID is the
// anchor's (never the held weapon's owner). world = presentation world && anchor && the VR menu not eating input.
bool vrArcAnchor(std::uint64_t now,vr::arc::AnchorInputs& out) noexcept {
    try{
        out=vr::arc::AnchorInputs{};
        const auto state=presentation.snapshot(now);HeadCameraAnchor a{};
        if(!state.world()||!headCamera.snapshot(now,state.revision,a)||!a.sampledNs)return false;
        out.key=a.sampledNs;out.actorGuid=a.actorGuid;out.up={a.up[0],a.up[1],a.up[2]};out.anchorPosition={a.position[0],a.position[1],a.position[2]};
        const auto menuNs=vr::holsters::menuSeenNs.load(std::memory_order_relaxed);
        const bool menu=menuNs&&(now<menuNs||now-menuNs<vr::holsters::menuFreezeNs);
        out.world=!menu;return true;
    }catch(...){return false;}
}
// Main thread: OpenXR LOCAL (metres) -> world of this frame (vrThrowMap: the weapon's plan preparation) + the HMD eye midpoint of the same
// shifted plan (vrBodyFrame out.eye) + whether the snap-turn bridge rotated it.
bool vrArcMap(std::uint64_t now,vr::arc::MapInputs& out) noexcept {
    try{
        out=vr::arc::MapInputs{};
        vw::PoseRequest req{};req.now=now;
        StereoCameraPlan plan{};if(controllerAimSpace.plan(plan))req.plan=&plan;
        StereoCameraPlan shifted{};vw::PlanPrep prep{};
        if(vw::preparePlan(req,shifted,prep)!=vw::Ok)return false;
        const auto m=vw::planMap(shifted);for(unsigned i=0;i<16;++i)out.localToWorld[i]=m[i];
        const auto& w0=shifted.world[0];const auto& w1=shifted.world[1];
        out.eye={(w0[12]+w1[12])*.5f,(w0[13]+w1[13])*.5f,(w0[14]+w1[14])*.5f};
        out.referenceSpace=shifted.tracking.epochs.referenceSpace;out.upm=shifted.unitsPerMetre>0.f?shifted.unitsPerMetre:1.f;
        out.bridged=prep.bridged;out.valid=out.referenceSpace!=0;return out.valid;
    }catch(...){return false;}
}
// Main thread: the RAW controller sample (physical hands 0 left / 1 right; no holsters::weaponInput substitution).
bool vrArcInput(std::uint64_t now,runtime::ControllerSample& raw) noexcept {try{return controllers.snapshot(now,raw);}catch(...){return false;}}
// Render thread (hands pass, once per render frame): AppSW synthesizing (arcs drawn solid and dimmer, counted).
bool vrArcSynthesizing() noexcept {try{return runtime::spaceWarpControl.synthesizing();}catch(...){return false;}}
// CATCHING: the catching hand's placement through the engine item's pipeline (the thrown item's owner copy); still holstered = the
// holster-locator correction K, already in the hand = the plain attachment.
bool vrThrowCatchPose(const WeaponPoseCall& call,WeaponNativeMatrix& matrix,std::uint64_t now,const runtime::Mat4& K,float scale) noexcept {
    namespace vt=::rdrvr::vr::throwing;
    const int hand=vt::catchHand.load(std::memory_order_relaxed);const auto cw=reinterpret_cast<std::uintptr_t>(call.weapon);
    auto o=vw::throwOwnerSnapshot();
    if(hand<0||hand>1||o.weapon!=cw||!o.geometry.valid||o.geometry.weapon!=cw){vt::bump(vt::poseDeclines[vt::DeclineCatch]);return false;}
    o.stampNs=now;
    const bool inHand=vt::catchInHand.load(std::memory_order_relaxed);
    const bool nativeAlt=inHand&&call.nativeFlag; // The catching hand's side under BODY
    vw::PoseRequest req{};req.now=now;req.hold=hand;req.alt=vw::effectiveAlt(nativeAlt,hand,o.geometry,::rdrvr::vr::bodyik::bodyVisible());req.owner=&o;req.scale=scale;
    if(!controllers.snapshot(now,req.raw)){vt::bump(vt::poseDeclines[vt::DeclineCatch]);return false;}
    req.input=req.raw;req.input.grip[1]=req.raw.grip[unsigned(hand)];req.input.aim[1]=req.raw.aim[unsigned(hand)]; // logical hand 1 = the catching hand
    StereoCameraPlan plan{};if(controllerAimSpace.plan(plan))req.plan=&plan;
    if(vw::config.predict==vw::Predict::Capsule){const auto& api=reticleProbeApi.attachment;
        req.capsuleValid=api.read&&vw::readCapsule([&](std::uintptr_t p,void* v,std::size_t n){return api.read(api.context,p,v,n);},o.actor,req.capsule);}
    vw::Latest l{};if(vw::computePlacement(req,l)!=vw::Ok){vt::bump(vt::poseDeclines[vt::DeclineCatch]);return false;}
    runtime::Mat4 passed=l.placement.attachment;
    if(inHand&&req.alt!=nativeAlt){runtime::Mat4 a{};if(vw::engineAttachment(o.geometry,nativeAlt,l.placement.model,scale,a))passed=a;}
    if(!inHand){passed=runtime::multiply(l.placement.model,K);if(!vw::rigidize(passed,scale)){vt::bump(vt::poseDeclines[vt::DeclineCatch]);return false;}}
    matrix.values=passed;const auto scope=WeaponPoseCallBoundary::scope();
    vw::pending={scope.serial,cw,l.placement.model,true,false,o.geometry.animated,false,passed,!inHand};
    vt::bump(vt::poseCatching);return true;
}
// Any thread: the thrown weapon's own (holstered) weapWeapon::Update. FLYING / RESTING: the arc model at the hook's clock, passed as
// model * K (K = vw::throwFix, the inverse holster locator); CATCHING: the catching hand (else the
// frozen pose); RETURNING: a blend from the last pose to the native holster (the call's own argument). Decline = native = in the holster.
bool vrThrowPose(const WeaponPoseCall& call,WeaponNativeMatrix& matrix,std::uint64_t now) noexcept {
    namespace vt=::rdrvr::vr::throwing;
    try{
        vt::bump(vt::poseCalls);
        const auto cw=reinterpret_cast<std::uintptr_t>(call.weapon);
        const auto decline=[&](vt::PoseDecline why){vt::bump(vt::poseDeclines[why]);vw::pending=vw::Pending{};return false;};
        vt::Arc a{};if(!vt::arcBox.load(a)||a.state==std::uint32_t(vt::StateIdle))return decline(vt::DeclineNoArc);
        if(a.weapon!=cw)return decline(vt::DeclineWeapon);
        runtime::Mat4 source{},unit{};float scale{};
        if(!WeaponAttachmentReader(reticleProbeApi.attachment).argument(call.attachment,source)||!deriveModelMuzzle(source,source,unit,scale))
            return decline(vt::DeclineArgument);
        runtime::Mat4 K{};bool haveK=false;
        {std::lock_guard lock(vw::throwFix.m);if(vw::throwFix.valid&&vw::throwFix.weapon==cw){K=vw::throwFix.k;haveK=true;}}
        if(!haveK){ // First override: the in-hand relation (inverse hand locator); the real holster K is computed after this Update
            const auto o=vw::throwOwnerSnapshot();
            if(o.weapon!=cw||!o.geometry.valid||!vw::affineInverse(o.geometry.locator[0],K))return decline(vt::DeclineWeapon);}
        if(a.state==std::uint32_t(vt::StateCatching)&&vrThrowCatchPose(call,matrix,now,K,scale))return true; // else the frozen pose
        vt::M4 m{};
        if(a.state==std::uint32_t(vt::StateReturning)){
            runtime::Mat4 kInv{};if(!vw::affineInverse(K,kInv))return decline(vt::DeclineRigid);
            const auto native=runtime::multiply(source,kInv); // the model where the holster puts it
            vt::M4 nm{};for(unsigned i=0;i<16;++i)nm[i]=native[i];
            m=vt::math::blend(a.lastModel,nm,vt::returnFraction(a,now));vt::bump(vt::poseReturning);
        }else if(!vt::evalModel(a,now,m))return decline(vt::DeclineEval);
        runtime::Mat4 model{};for(unsigned i=0;i<16;++i)model[i]=m[i];
        auto passed=runtime::multiply(model,K);
        if(!vw::rigidize(passed,scale))return decline(vt::DeclineRigid);
        matrix.values=passed;const auto scope=WeaponPoseCallBoundary::scope();
        vw::pending={scope.serial,cw,model,true,false,a.animated!=0,false,passed,true};
        if(haveK)vt::bump(vt::fixApplied);
        vt::bump(vt::posePlaced);return true;
    }catch(...){vw::pending=vw::Pending{};return false;}
}
// Any thread: weapWeapon::Update attachment of the VR-held weapon.
bool vrWeaponPose(const WeaponPoseCall& call,WeaponNativeMatrix& matrix,std::uint64_t now) noexcept {
    vw::pending=vw::Pending{};
    if(!vw::config.enabled||!vw::started.load(std::memory_order_relaxed))return false;
    try{
        { // THROW: the thrown weapon first (lock-free); while it is being caught the owner path takes over once the observer published it
            const auto cw=reinterpret_cast<std::uintptr_t>(call.weapon);
            if(vw::poseTarget(cw,0)==vw::PoseTarget::Thrown&&
               !(::rdrvr::vr::throwing::poseState(cw)==::rdrvr::vr::throwing::StateCatching&&vw::ownerSnapshot().weapon==cw))return vrThrowPose(call,matrix,now);
        }
        // DUAL: the phantom first (lock-free check; during a swap the owner snapshot may still name the same weapon)
        if(vw::poseTarget(reinterpret_cast<std::uintptr_t>(call.weapon),0)==vw::PoseTarget::Phantom)return vrPhantomPose(call,matrix,now);
        auto o=vw::ownerSnapshot();
        if(!o.weapon||reinterpret_cast<std::uintptr_t>(call.weapon)!=o.weapon){ // other actors' weapons: not counted
            // DUAL: right after a swap / promotion the new engine item is still published as the phantom owner (observer pending)
            const auto p=vw::phantomSnapshot();
            if(!p.weapon||reinterpret_cast<std::uintptr_t>(call.weapon)!=p.weapon||::rdrvr::vr::holsters::holdHand.load(std::memory_order_acquire)<0)return false;
            o=static_cast<const vw::Owner&>(p);o.aimAllowed=o.forceAllowed=false;vw::phantomAdopted.fetch_add(1,std::memory_order_relaxed);
        }
        // BODY: a HAND-held item (knife / thrown / lasso / rigid) whose hand the BODY IK drove in this hook scope stays on the
        // engine's native attachment of that skeletal hand (it hangs in the hand like the knife).
        if(::rdrvr::vr::bodyik::nativeHold(o.weapon)){vw::bodyNativeHold.fetch_add(1,std::memory_order_relaxed);return false;}
        vw::stats[vw::ChannelPose].calls.fetch_add(1,std::memory_order_relaxed);
        const int hold=::rdrvr::vr::holsters::holdHand.load(std::memory_order_acquire);
        // HOLD LAST: a pipeline refusal (no plan / input, map, reach, place) while held keeps the last placement instead of the
        // engine's animated hand for that frame (VrWeapon.h holdLastPlacement; context / class refusals stay native).
        const auto no=[&](vw::Reason why){vw::decline(vw::ChannelPose,why,now);vw::Latest last{};
            if(hold<0||!o.poseAllowed||!vw::holdLastPlacement(o.weapon,why,now,last))return false;
            matrix.values=last.placement.attachment;const auto scope=WeaponPoseCallBoundary::scope();
            vw::pending={scope.serial,o.weapon,last.placement.model,true,false,o.geometry.animated};return true;};
        if(hold<0)return no(vw::NotHeld);
        if(!o.stampNs||(now>o.stampNs&&now-o.stampNs>250'000'000))return no(vw::StaleOwner);
        if(!o.poseAllowed)return no(vw::attachable(o.kind,vw::config)?vw::Context:vw::Class);
        if(!o.geometry.valid||o.geometry.weapon!=o.weapon)return no(vw::NoGeometry);
        // Placement pipeline in VrWeapon.h computePlacement (wrist anchor, this frame's anchor predicted from the capsule,
        // snap-turn bridge, LOCAL->world map + raw hand sample kept for the hands pass / laser).
        // BODY places the gun with the locator / hand bone of the VR holding side (the engine keeps its
        // right attachment for the left VR hand); the attachment handed back is converted to the engine's locator below.
        const bool nativeAlt=call.nativeFlag;
        vw::PoseRequest req{};req.now=now;req.hold=hold;req.alt=vw::effectiveAlt(nativeAlt,hold,o.geometry,::rdrvr::vr::bodyik::bodyVisible());req.owner=&o;
        if(!controllers.snapshot(now,req.raw))return no(vw::NoInput);
        req.input=req.raw;::rdrvr::vr::holsters::weaponInput(req.input); // logical hand 1 = the physical holding hand (+ two-hand barrel)
        StereoCameraPlan plan{};if(controllerAimSpace.plan(plan))req.plan=&plan; // none: native turn in flight -> turn bridge
        runtime::Mat4 source{},unit{};
        if(!WeaponAttachmentReader(reticleProbeApi.attachment).argument(call.attachment,source)||!deriveModelMuzzle(source,source,unit,req.scale))return no(vw::BadArgument);
        if(vw::config.predict==vw::Predict::Capsule){const auto& api=reticleProbeApi.attachment;
            req.capsuleValid=api.read&&vw::readCapsule([&](std::uintptr_t p,void* v,std::size_t n){return api.read(api.context,p,v,n);},o.actor,req.capsule);
            (req.capsuleValid?vw::capsuleReads:vw::capsuleFails).fetch_add(1,std::memory_order_relaxed);}
        vw::lastPoseAlt.store(req.alt,std::memory_order_relaxed);vw::lastPoseScale.store(req.scale,std::memory_order_relaxed);
        vw::lastNativeAlt.store(nativeAlt,std::memory_order_relaxed); // The engine's own flag (vrBodyFrame derives the effective one)
        vw::Latest l{};
        { // BODY: the placement the BODY hook derived the holding wrist from (same thread, same hook scope) - hand and gun coincide
            const auto& pre=vw::bodyPre;const auto bodyScope=::rdrvr::vr::bodyik::scopeSerial();
            if(bodyScope&&pre.serial==bodyScope&&pre.weapon==o.weapon&&pre.alt==req.alt&&std::fabs(pre.scale-req.scale)<=1e-4f*std::max(1.f,std::fabs(req.scale))){
                l=pre.latest;vw::bodyPreUsed.fetch_add(1,std::memory_order_relaxed);}
            else{
                if(bodyScope&&pre.serial==bodyScope)vw::bodyPreMismatch.fetch_add(1,std::memory_order_relaxed);
                if(const auto why=vw::computePlacement(req,l))return no(why);
            }
        }
        // SPIN (VrSpin.h): the hinge of this held placement for the input brain; a hand the spin owns gets the gun rotated about the
        // trigger finger: model' = model * C (model space), handed back through the engine's own locator (engineAttachment) + the model hook.
        namespace vsp=::rdrvr::vr::spin;
        bool spun=false;runtime::Mat4 spunModel{},spunAttachment{};
        if(vsp::started.load(std::memory_order_relaxed)&&!vsp::killed.load(std::memory_order_relaxed)&&o.geometry.animated&&!l.placement.hand){
            const auto gen=::rdrvr::vr::holsters::holdGeneration.load(std::memory_order_relaxed);
            runtime::Mat4 linv{};
            if(vw::affineInverse(o.geometry.locator[req.alt?1u:0u],linv)){
                vsp::PlacementIn pin{};pin.hand=hold;pin.weapon=o.weapon;pin.generation=gen;pin.now=now;pin.referenceSpace=l.referenceSpace;
                for(unsigned i=0;i<16;++i){pin.model[i]=l.placement.model[i];pin.map[i]=l.map[i];}
                pin.gripPos={l.localGrip.position[0],l.localGrip.position[1],l.localGrip.position[2]};
                pin.gripRot={l.localGrip.orientation[0],l.localGrip.orientation[1],l.localGrip.orientation[2],l.localGrip.orientation[3]};
                const auto& mm=o.geometry.modelMuzzle;
                pin.handM={linv[12],linv[13],linv[14]};pin.muzzleM={mm[12],mm[13],mm[14]};pin.upM={mm[4],mm[5],mm[6]};
                pin.fwdM={-mm[8],-mm[9],-mm[10]}; // Barrel = -Z of the authored muzzle locator
                // PIVOT: the weapon's 'trigger' bone in model space (skeleton root^-1 * global; once per weapon, retried on failure)
                if(vsp::triggerWanted(o.weapon)){
                    namespace vrl=::rdrvr::vr::reload;const auto& api=reticleProbeApi.attachment;
                    bool ok=false;vsp::V3 posM{};std::array<char,32> nm{};
                    if(api.skeleton&&api.read){
                        vrl::Engine e{};e.read=api.read;e.context=api.context;vrl::Skeleton k{};
                        if(e.skeleton(reinterpret_cast<std::uintptr_t>(api.skeleton(call.weapon,false)),k)){
                            int bone=-1;std::array<char,32> name{};
                            for(const char* want:{"trigger","trig"}){for(unsigned i=0;i<k.count&&bone<0;++i)if(e.name(k,i,name)&&vrl::containsCi(name.data(),want)){bone=int(i);nm=name;}
                                if(bone>=0)break;}
                            vrl::Mat root{},global{},rootInv{};
                            if(bone>=0&&e.matrix(k.root,root)&&e.matrix(k.globals+unsigned(bone)*vrl::globalStride,global)&&vrl::affineInverse(root,rootInv)){
                                const auto p=vrl::point(rootInv,vrl::col(global,3));posM={p[0],p[1],p[2]};
                                ok=std::isfinite(posM[0])&&std::isfinite(posM[1])&&std::isfinite(posM[2]);}
                        }
                    }
                    vsp::noteTrigger(o.weapon,ok,posM,nm.data());
                }
                pin.upm=l.placement.unitsPerMetre>0.f?l.placement.unitsPerMetre:1.f;
                vsp::notePlacement(pin);
            }
            vsp::M4 C{};
            if(vsp::evalPose(hold,o.weapon,gen,now,C)==vsp::Eval::Spun){
                runtime::Mat4 c{};for(unsigned i=0;i<16;++i)c[i]=C[i];
                spunModel=runtime::multiply(l.placement.model,c);
                if(vw::engineAttachment(o.geometry,nativeAlt,spunModel,req.scale,spunAttachment)){spun=true;vsp::bump(vsp::stats.posePlaced);}
                else vsp::bump(vsp::stats.poseRigidFail);
            }
            if(spun){vsp::M4 m{};for(unsigned i=0;i<16;++i)m[i]=spunModel[i];vsp::noteSpun(o.weapon,m,now);}
            else vsp::clearSpun(o.weapon);
        }
        matrix.values=l.placement.attachment;
        if(req.alt!=nativeAlt){ // The engine's Update multiplies ITS locator: hand it model * inverse(locator[native]) -> the same model
            vw::sideAltPoses.fetch_add(1,std::memory_order_relaxed);runtime::Mat4 a{};
            if(vw::engineAttachment(o.geometry,nativeAlt,l.placement.model,req.scale,a)){matrix.values=a;vw::sideAltConverted.fetch_add(1,std::memory_order_relaxed);}
            else vw::sideAltConvertFailed.fetch_add(1,std::memory_order_relaxed);}
        if(spun)matrix.values=spunAttachment;
        const auto scope=WeaponPoseCallBoundary::scope();
        vw::pending={scope.serial,o.weapon,spun?spunModel:l.placement.model,true,false,o.geometry.animated}; // SPIN: the spun model for UpdateSkeleton
        vw::publishPlacement(l);vw::accepted(vw::ChannelPose,now);
        if(vw::firearm(o.kind)&&o.aimAllowed)vrClearLaunch(o.weapon,vw::LaunchPose); // aniAim::Fire runs inside this Update
        return true;
    }catch(...){vw::pending=vw::Pending{};return vw::decline(vw::ChannelPose,vw::Exception,now);}
}
// Same thread, inside that Update: UpdateSkeleton's SetTransform(model, weapon+0x80) of an animated weapon gets attachment*locator.
bool vrWeaponModel(const WeaponModelCall& call,WeaponNativeMatrix& matrix) noexcept {
    auto& p=vw::pending;if(!p.valid)return false;
    const auto now=diagnostics::monotonicNs();
    vw::stats[vw::ChannelModel].calls.fetch_add(1,std::memory_order_relaxed);
    const auto scope=WeaponPoseCallBoundary::scope();
    if(!scope.serial||scope.serial!=p.serial||scope.phase!=WeaponPosePhase::Native||!scope.prepared||reinterpret_cast<std::uintptr_t>(scope.weapon)!=p.weapon)
        return vw::decline(vw::ChannelModel,vw::ModelScope,now);
    if(p.modelUsed)return vw::decline(vw::ChannelModel,vw::ModelUsed,now);
    if(reinterpret_cast<std::uintptr_t>(call.matrix)!=p.weapon+0x80)return vw::decline(vw::ChannelModel,vw::ModelMatrix,now);
    p.modelUsed=true;matrix.values=p.model;vw::accepted(vw::ChannelModel,now);return true;
}
void vrWeaponPoseComplete(const WeaponPoseCall& call,bool prepared) noexcept {
    auto& p=vw::pending;
    const bool placed=p.valid&&reinterpret_cast<std::uintptr_t>(call.weapon)==p.weapon;
    if(placed){
        if(prepared)vw::poseCompleted.fetch_add(1,std::memory_order_relaxed);
        if((p.phantom||p.thrown)&&prepared){
            const auto& api=reticleProbeApi.attachment;runtime::Mat4 m{};auto& fix=p.thrown?vw::throwFix:vw::phantomFix;
            if(api.read&&api.read(api.context,p.weapon+0x80,m.data(),sizeof(float)*16)){
                m[3]=m[7]=m[11]=0;m[15]=1;runtime::Mat4 inv{};
                if(vw::affineInverse(m,inv)){const auto k=runtime::multiply(inv,p.passed);
                    std::lock_guard lock(fix.m);
                    if(!p.thrown&&fix.valid&&fix.weapon==p.weapon){float d=0;for(unsigned i=0;i<12;++i)d=std::max(d,std::fabs(k[i]-fix.k[i]));
                        const auto micro=std::uint64_t(std::min(d,1000.f)*1e6f);if(micro>vw::phantomFixChangedMicro.load(std::memory_order_relaxed))vw::phantomFixChangedMicro.store(micro,std::memory_order_relaxed);}
                    fix.weapon=p.weapon;fix.k=k;fix.valid=true;
                    if(p.thrown)::rdrvr::vr::throwing::bump(::rdrvr::vr::throwing::fixMeasured);else vw::phantomFixMeasured.fetch_add(1,std::memory_order_relaxed);
                }else if(!p.thrown)vw::phantomFixBad.fetch_add(1,std::memory_order_relaxed);
            }else if(!p.thrown)vw::phantomFixReadFailures.fetch_add(1,std::memory_order_relaxed);
        }
        if(p.animated&&!p.modelUsed)vw::modelSkipped.fetch_add(1,std::memory_order_relaxed);
    }
    vrWeaponReload(call,placed&&prepared); // Cylinder swing (or restore) inside this weapon's own Update
    vrWeaponHammer(call); // HAMMER: Zone + native trace + H2 (after the swing, same write window)
    p=vw::Pending{};
}
// weapWeapon::Aim (vector overload) of the VR-held firearm: a free target on the barrel line.
bool vrWeaponAim(const WeaponAimCall& call,WeaponAimVector& point,std::uint64_t now,bool clearLaunch=true) noexcept {
    if(!vw::config.enabled||!vw::started.load(std::memory_order_relaxed))return false;
    try{
        const auto o=vw::ownerSnapshot();
        if(!o.weapon||reinterpret_cast<std::uintptr_t>(call.weapon)!=o.weapon)return false;
        vw::stats[vw::ChannelAim].calls.fetch_add(1,std::memory_order_relaxed);
        const auto no=[&](vw::Reason why){return vw::decline(vw::ChannelAim,why,now);};
        if(::rdrvr::vr::holsters::holdHand.load(std::memory_order_acquire)<0)return no(vw::NotHeld);
        if(!o.stampNs||(now>o.stampNs&&now-o.stampNs>250'000'000))return no(vw::StaleOwner);
        if(!vw::firearm(o.kind))return no(vw::Class); // Knife / thrown / lasso Aim calls
        if(!o.aimAllowed)return no(vw::AimBlocked);
        runtime::Vec3 origin{},direction{};float unitsPerMetre=1;
        const auto l=vw::latestPlacement();
        if(l.weapon==o.weapon&&l.ns&&now>=l.ns&&now-l.ns<=150'000'000){
            origin=l.placement.origin;direction=l.placement.direction;unitsPerMetre=l.placement.unitsPerMetre;vw::aimFromPose.fetch_add(1,std::memory_order_relaxed);
        }else{ // no placement this frame (e.g. first frame after the draw): the holding controller's aim ray itself
            runtime::ControllerSample input{};if(!controllers.snapshot(now,input))return no(vw::NoInput);
            ::rdrvr::vr::holsters::weaponInput(input);
            StereoCameraPlan plan{};if(!controllerAimSpace.plan(plan))return no(vw::NoPlan);
            ControllerRay ray{};if(const auto why=vw::mapRay(plan,input,1,false,now,vw::config.planMaxAgeNs,{},ray))return no(why);
            origin=ray.origin;direction=ray.direction;unitsPerMetre=ray.unitsPerMetre;vw::aimFromController.fetch_add(1,std::memory_order_relaxed);
        }
        const auto p=vw::aimPoint(origin,direction,vw::config.aimDistanceM,unitsPerMetre);
        for(float x:p)if(!std::isfinite(x))return no(vw::Place);
        point.x=p[0];point.y=p[1];point.z=p[2];point.padding=0;vw::accepted(vw::ChannelAim,now);
        if(clearLaunch)vrClearLaunch(o.weapon,vw::LaunchAim); // Launch from the VR muzzle, not the camera
        return true;
    }catch(...){return vw::decline(vw::ChannelAim,vw::Exception,now);}
}
// PostCameraUpdate right after headCamera.publish (main thread = the native owner thread, pinned reticle boundary):
// anchor ring + mover facts, this frame's placement tagged with the head sample its eyes render with, the laser probe along that
// placement's barrel, and the frame published to the hands pass (held hand + laser drawn with exactly this pose).
void vrWeaponTag(const HeadCameraAnchor& anchor,const BodyAnchorFacts& facts,std::uint64_t now) noexcept {
    try{
        vw::noteAnchor(anchor,&facts);
        if(!vw::config.enabled||!vw::started.load(std::memory_order_relaxed)||::rdrvr::vr::holsters::holdHand.load(std::memory_order_acquire)<0)return;
        const auto vs=::rdrvr::vr::settings();
        // One frame per held item: k = 0 the engine item, 1 the DUAL phantom (a phantom firearm has its own laser: whichever
        // firearm is in a hand shows where it would shoot; with swap-fire its trigger makes it the engine item and fires along it).
        const auto frameOf=[&](const vw::Tagged& t,unsigned k){
            const auto& l=t.latest;
            const bool laser=l.firearm&&vs.laser()&&::rdrvr::vr::enabled(::rdrvr::vr::Feature::Laser)&&
                !(l.hand>=0&&l.hand<2&&((::rdrvr::vr::spin::fingerMask.load(std::memory_order_relaxed)>>unsigned(l.hand))&1u)); // SPIN: no laser while it spins
            const float upm=l.placement.unitsPerMetre>0.f?l.placement.unitsPerMetre:1.f;
            float metres=vw::config.laserLengthM;bool hit=false,probed=false,fromProbe=false;
            if(laser){
                (k?vw::phantomLaserTags:vw::laserTags).fetch_add(1,std::memory_order_relaxed);
                static unsigned ordinal[2]{};static float lastMetres[2]{};static bool lastHit[2]{};static std::uint64_t lastNs[2]{};
                const unsigned every=vw::config.laserProbe;
                // ARC: the laser is a soft user of the main-thread probe ledger (1 cast per Execute iteration while it has demand);
                // refused -> the last probed length (its <= 200 ms fallback). Without the main-thread executor (UpdateInputs print
                // mismatch) no ledger window ever rolls: the laser then probes charged and never refused, as
                // with the arcs killed (debug.rdrvr.vr.arc 0 / .hands 0); the ledger also gives the laser a
                // floor outside .arc.casts (VrArc.h Ledger).
                if(every&&reticleProbeReady.load(std::memory_order_acquire)&&(++ordinal[k]%every==0||!lastNs[k]||now-lastNs[k]>200'000'000ull)&&
                   (!::rdrvr::vr::mainthread::executorInstalled()||::rdrvr::vr::killed(::rdrvr::vr::Feature::Arc)||::rdrvr::vr::killed(::rdrvr::vr::Feature::Hands)||
                    ::rdrvr::vr::arc::ledger.take(::rdrvr::vr::arc::ProbeUser::Laser))){
                    AimNativeVector a{},b{};
                    for(unsigned i=0;i<3;++i){a.v[i]=l.placement.origin[i];b.v[i]=l.placement.origin[i]+l.placement.direction[i]*metres*upm;}
                    NativeAimHit h{};const auto t0=diagnostics::monotonicNs();const float castM=metres;bool castOk=false;
                    if(NativeAimProbe(reticleProbeApi.probe).castBetween(vw::ownerSnapshot().actorGuid,a,b,h)){
                        probed=fromProbe=true;vw::laserProbes.fetch_add(1,std::memory_order_relaxed);castOk=true;
                        if(h.blocked&&std::isfinite(h.distance)){hit=true;metres=std::max(.05f,h.distance/upm);vw::laserHits.fetch_add(1,std::memory_order_relaxed);}
                        lastMetres[k]=metres;lastHit[k]=hit;lastNs[k]=now;
                    }else{vw::laserProbeFails.fetch_add(1,std::memory_order_relaxed);lastNs[k]=0;}
                    const auto dt=diagnostics::monotonicNs()-t0;vw::laserProbeNsSum.fetch_add(dt,std::memory_order_relaxed);
                    ::rdrvr::vr::arc::ledger.charge(::rdrvr::vr::arc::ProbeUser::Laser,dt,castM,!castOk);
                    auto m=vw::laserProbeNsMax.load(std::memory_order_relaxed);while(dt>m&&!vw::laserProbeNsMax.compare_exchange_weak(m,dt)){}
                }else if(lastNs[k]&&now-lastNs[k]<=200'000'000ull){metres=lastMetres[k];hit=lastHit[k];fromProbe=true;} // between probes: the last probed length
                if(!k){vw::setTagLaser(anchor.sampledNs,metres,hit,probed);vw::laserLastM.store(metres,std::memory_order_relaxed);}
            }
            ::rdrvr::vr::hands::WeaponFrame f{};
            f.headNs=anchor.sampledNs;f.referenceSpace=l.referenceSpace;f.epoch=l.epoch;f.sequence=l.sequence;f.hand=l.hand;
            f.flags=(laser?::rdrvr::vr::hands::WeaponFrame::Laser:0u)|(hit?::rdrvr::vr::hands::WeaponFrame::Hit:0u)|
                (l.firearm?::rdrvr::vr::hands::WeaponFrame::Firearm:0u)|(fromProbe?::rdrvr::vr::hands::WeaponFrame::Probed:0u)|
                (l.turnBridged?::rdrvr::vr::hands::WeaponFrame::Bridged:0u); // The hands pass reports the residual of bridged frames apart
            f.aim=l.localAim;f.grip=l.localGrip;f.map=l.map;f.muzzle=l.placement.origin;f.direction=l.placement.direction;
            f.laserMetres=metres;f.laserWidthMm=vw::config.laserWidthMm;f.unitsPerMetre=upm;
            return f;};
        vw::Tagged t{};
        if(vw::findTag(anchor.sampledNs,t)){::rdrvr::vr::hands::publishWeapon(frameOf(t,0));vw::laserPublished.fetch_add(1,std::memory_order_relaxed);}
        vw::Tagged pt{};
        if(::rdrvr::vr::holsters::phantomHand.load(std::memory_order_acquire)>=0&&vw::findPhantomTag(anchor.sampledNs,pt))
            ::rdrvr::vr::hands::publishPhantomWeapon(frameOf(pt,1));
    }catch(...){}
}
#else
void vrWeaponTag(const HeadCameraAnchor& anchor,const BodyAnchorFacts&,std::uint64_t) noexcept {::rdrvr::vr::weapon::noteAnchor(anchor);}
void vrWeaponFire(const WeaponFireCall&,std::uint64_t) noexcept {}
void vrWeaponManager(void*) noexcept {}
void vrWeaponObserve(const LocalWeapon*,const WeaponContext*,std::uint64_t,WeaponObserveGap=WeaponObserveGap::None) noexcept {}
bool vrWeaponPose(const WeaponPoseCall&,WeaponNativeMatrix&,std::uint64_t) noexcept {return false;}
bool vrWeaponModel(const WeaponModelCall&,WeaponNativeMatrix&) noexcept {return false;}
void vrWeaponPoseComplete(const WeaponPoseCall&,bool) noexcept {}
bool vrWeaponAim(const WeaponAimCall&,WeaponAimVector&,std::uint64_t) noexcept {return false;}
#endif
bool trackedWeaponMuzzle(const ControllerAttachmentScope& scope,std::uint64_t now,const ControllerRay& reference,ControllerRay& out){
    return controllerAttachment.muzzleRay(reticleProbeApi.attachment,scope,now,reference,out)||
        animatedAttachment.muzzleRay(reticleProbeApi.attachment,scope,now,reference,out);
}
bool gamePrepareWeaponModel(const WeaponModelCall& call,WeaponNativeMatrix& matrix,std::uint64_t now) noexcept {
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
    const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
    if(vrWeaponModel(call,matrix))return true; // VR-held animated weapon, this Update's attachment * locator
    try{
        if(!weaponModelReady.load(std::memory_order_acquire)||!pendingAttachmentPrepared||!pendingAttachmentAnimated||
           !weaponOwner.onOwnerThread()||now<pendingAttachment.sampledNs||now-pendingAttachment.sampledNs>100'000'000)return false;
        const auto view=presentation.snapshot(now);LocalWeapon owner{};WeaponContext context{};
        runtime::ControllerSample input{};StereoCameraPlan camera{};ControllerRay ray{};
        if(!view.world()||!weaponOwner.inspect(owner)||!WeaponContextReader(weaponObservationApi).inspect(owner,context)||
           !holsterOutputInput(now,input)||!controllerAimSpace.plan(camera)||!headCamera.matches(camera,now,view.revision)||
           !sameAttachmentScope(pendingAttachment.scope,{owner,view.revision,input.epoch,input.referenceSpace,input.continuity})||
           !controllerAimRay(owner,context,camera,input,view.revision,now,ray)||!validPose(input.grip[1]))return false;
        return scopedModelPose.prepare(weaponModelApi,reticleProbeApi.attachment,call,matrix);
    }catch(...){return false;}
#else
    return false;
#endif
}
void gameWeaponModelComplete(const WeaponModelCall& call,bool prepared,bool result) noexcept {
    scopedModelPose.after(call,prepared,result);
}
// ARC: the arc layer's engine probe = the same three print-checked pointers as the laser (NativeAimProbeApi; ABI-identical 16-byte
// vector / 0x70 intersection types, static_asserts in VrArc.h). capsule (sTestCapsule) stays null (its print is not verified).
static ::rdrvr::vr::arc::EngineProbeApi arcProbeApi{};
void gameConfigureReticleProbe(ReticleProbeApi api) noexcept {reticleProbeApi=api;reticleProbeReady.store(bool(api),std::memory_order_release);reticleProbeState=api?"WAITING_FOR_NATIVE_RETICLE":"NOT_CONFIGURED";
    static_assert(sizeof(AimNativeVector)==sizeof(::rdrvr::vr::arc::EngineVec)&&sizeof(AimNativeSegment)==sizeof(::rdrvr::vr::arc::EngineSegment)&&
                  sizeof(AimNativeIntersection)==sizeof(::rdrvr::vr::arc::EngineIntersection));
    if(api){arcProbeApi.exclude=api.probe.exclude;
        arcProbeApi.probe=reinterpret_cast<decltype(arcProbeApi.probe)>(api.probe.probe);
        arcProbeApi.closest=reinterpret_cast<decltype(arcProbeApi.closest)>(api.probe.closest);
        ::rdrvr::vr::arc::engineApi.store(&arcProbeApi,std::memory_order_release);}
    else ::rdrvr::vr::arc::engineApi.store(nullptr,std::memory_order_release);
}
#ifdef RDRVR_NATIVE_MENUS
bool worldPresentationAllowed();
#endif
void gameObserveWeaponManager(void* weapon,bool complete,std::uint64_t timestamp,std::uint64_t thread) noexcept{
    if(!complete)vrWeaponManager(weapon); // Camera launch override off before ManagerAim of the VR-held weapon
    if(!weaponObservationReady.load(std::memory_order_acquire))return;
    weaponCallTrace.record(reinterpret_cast<std::uintptr_t>(weapon),
        {complete?WeaponCallPhase::ManagerComplete:WeaponCallPhase::Manager,timestamp,thread,reticleUpdates.load()});
}
void gameConfigureReticleUi(NativeReticleUiReader::Api api,bool ready) noexcept {
    reticleUiApi=api;reticleUiReady.store(ready,std::memory_order_release);
}
void gameObserveReticleUi(void* object,bool complete,std::uint64_t timestamp) noexcept {
    const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} restore{saved};
    try{
        if(!complete){
            pendingReticleUiValid=false;
            // Sample the first paint, then one in thirty; never add UI ticks.
            const auto ordinal=reticleUiCalls.fetch_add(1)+1;
            if(ordinal!=1&&ordinal%30)return;
            if(!timestamp||!reticleUiReady.load(std::memory_order_acquire))return;
            ReticleUiRecord record{};record.stamp=timestamp;
            if(!NativeReticleUiReader(reticleUiApi).inspect(reinterpret_cast<std::uintptr_t>(object),record.before)){++reticleUiDeclined;return;}
            pendingReticleUi=record;pendingReticleUiValid=true;++reticleUiCaptured;return;
        }
        if(!pendingReticleUiValid)return;
        auto record=pendingReticleUi;pendingReticleUiValid=false;record.completedNs=timestamp;
        record.completed=timestamp>=record.stamp&&timestamp-record.stamp<=100'000'000&&
            NativeReticleUiReader(reticleUiApi).inspect(reinterpret_cast<std::uintptr_t>(object),record.after);
        if(record.completed)++reticleUiCompleted;else ++reticleUiDeclined;
        std::lock_guard lock(reticleUiMutex);latestReticleUi=record;
    }catch(...){pendingReticleUiValid=false;++reticleUiDeclined;}
}
void gameConfigureProjectileLaunch(ProjectileLaunchReader::Api api,bool ready) noexcept {
    projectileLaunchApi=api;projectileLaunchReady.store(ready,std::memory_order_release);
}
void gameObserveProjectileLaunch(const ProjectileLaunchCall& call,const WeaponFireScope& scope,bool complete,std::uint64_t timestamp) noexcept {
    const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
    try{
        if(!complete)pendingLaunch={};
        if(!projectileLaunchReady.load(std::memory_order_acquire)||!weaponObservationReady.load(std::memory_order_acquire)||
           !scope.serial||!scope.weapon||!weaponOwner.onOwnerThread())return;
        ProjectileLaunchReader reader(projectileLaunchApi);
        if(!complete){
            LocalWeapon owner{};
            if(!weaponOwner.inspect(owner)||reinterpret_cast<void*>(owner.weapon)!=scope.weapon||call.actorGuid!=owner.actorGuid)return;
            LaunchRecord record{};
            if(!reader.capture(call,owner,record.input)){++launchDeclined;return;}
            record.stamp=timestamp;record.fireSerial=scope.serial;
            WeaponContext context{};record.contextAvailable=WeaponContextReader(weaponObservationApi).inspect(owner,context);
            if(record.contextAvailable)record.contextFlags=context.flags();
#ifdef RDRVR_NATIVE_MENUS
            const auto view=presentation.snapshot(timestamp);record.revision=view.revision;
            {std::lock_guard lock(targetAimMutex);
                record.controllerTargetAvailable=view.world()&&targetAimGuid==owner.actorGuid&&targetAimRevision==view.revision&&
                    targetAimStamp&&timestamp>=targetAimStamp&&timestamp-targetAimStamp<=100'000'000;
                if(record.controllerTargetAvailable){record.controllerTarget={lastAimTarget.point.x,lastAimTarget.point.y,lastAimTarget.point.z};
                    record.controllerTargetTracked=targetAimTracked;}
            }
#endif
            pendingLaunch={record,owner,call.projectile,true};++launchCaptured;return;
        }
        if(!pendingLaunch.valid||pendingLaunch.projectile!=call.projectile||pendingLaunch.record.fireSerial!=scope.serial)return;
        auto pending=pendingLaunch;pendingLaunch.valid=false;auto& record=pending.record;record.completedNs=timestamp;
        LocalWeapon current{};record.ownerStillCurrent=weaponOwner.inspect(current)&&sameAimOwner(current,pending.owner);
        ProjectileLaunchCopy copy{};
        record.completionRead=timestamp>=record.stamp&&timestamp-record.stamp<=100'000'000&&record.ownerStillCurrent&&
            reader.completion(call,record.input,copy);
        if(record.completionRead){record.argumentsCopied=copy.matchesArguments;record.nativeFlags=copy.nativeFlags;}
        else ++launchDeclined;
        {std::lock_guard lock(launchMutex);launchRecords[launchNext]=record;launchNext=(launchNext+1)%unsigned(launchRecords.size());
            if(launchUsed<launchRecords.size())++launchUsed;else ++launchOverwritten;}
        ++launchCompleted;
    }catch(...){pendingLaunch.valid=false;++launchDeclined;}
}
void gameObserveWeaponFire(const WeaponFireCall& call,bool complete,std::uint64_t timestamp,std::uint64_t thread) noexcept{
    if(!complete)vrWeaponFire(call,timestamp); // Per-shot telemetry (engine fire of the VR-held weapon)
    if(!weaponObservationReady.load(std::memory_order_acquire))return;
    WeaponCallRecord record{complete?WeaponCallPhase::FireComplete:WeaponCallPhase::Fire,timestamp,thread,reticleUpdates.load()};
    record.flags=(call.nativeFlag?1u:0u)|(call.finalFlag?2u:0u)|(call.projectile?4u:0u)|(call.matrices?8u:0u);
    if(std::isfinite(call.scalar))record.nativeScalar=call.scalar;else record.flags|=0x80000000u;
    record.projectileCount=call.count;record.fireMode=call.mode;record.fireValue=call.nativeValue;
    weaponCallTrace.record(reinterpret_cast<std::uintptr_t>(call.weapon),record);
}
void gameObserveWeaponAim(const WeaponAimCall& call,std::uint64_t timestamp,std::uint64_t thread) noexcept{
    if(!weaponObservationReady.load(std::memory_order_acquire))return;
    WeaponCallRecord record{WeaponCallPhase::Aim,timestamp,thread,reticleUpdates.load(),call.targetGuid,call.mode,call.targetIndex,call.instance,call.part};
    record.flags=(call.targetFlag?1u:0u)|(call.aimerFlag?2u:0u)|(call.targetVelocity?4u:0u)|(call.intendedTarget?8u:0u);
    weaponCallTrace.record(reinterpret_cast<std::uintptr_t>(call.weapon),record);
}
bool gamePrepareWeaponAim(const WeaponAimCall& call,WeaponAimVector& point,std::uint64_t timestamp) noexcept{
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
    diagnostics::WallScope timing(diagnostics::frameTimings.controllerTarget);
    const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
    const auto decline=[](const char* state){targetAimState=state;++targetAimDeclined;
        std::lock_guard lock(targetAimMutex);targetAimStamp=0;return false;};
    pendingAimLocal=false;
    if(vrWeaponAim(call,point,timestamp))return true; // VR-held firearm, target on the barrel line
    try{
        if(!weaponObservationReady.load(std::memory_order_acquire)||!reticleProbeReady.load(std::memory_order_acquire)||
           !weaponOwner.onOwnerThread()){++targetAimIgnored;return false;}
        LocalWeapon owner{};
        if(!weaponOwner.inspect(owner))return decline("OWNER_UNAVAILABLE");
        if(owner.weapon!=reinterpret_cast<std::uintptr_t>(call.weapon)){++targetAimIgnored;return false;}
        pendingAimLocal=true;
        if(nativeUi.update(timestamp)&&nativeUi.inputRearmRequired())nativePad.rearm();
        const auto view=presentation.snapshot(timestamp);
        if(!view.world())return decline("NATIVE_PRESENTATION");
        runtime::ControllerSample input{};StereoCameraPlan plan{};WeaponContext context{};ControllerRay ray{};
        if(!holsterOutputInput(timestamp,input))return decline("TOUCH_INPUT_UNAVAILABLE");
        if(!WeaponContextReader(weaponObservationApi).inspect(owner,context))return decline("CONTEXT_UNAVAILABLE");
        if(!controllerAimSpace.plan(plan)||!headCamera.matches(plan,timestamp,view.revision)||
           !controllerAimRay(owner,context,plan,input,view.revision,timestamp,ray))return decline("NATIVE_CONTEXT_OR_TRACKING");
        ControllerRay muzzle{};
        const bool tracked=trackedWeaponMuzzle(
            {owner,view.revision,input.epoch,input.referenceSpace,input.continuity},timestamp,ray,muzzle);
        if(tracked)ray=muzzle;
        ControllerAimTarget target{};
        const ControllerAimTargetApi api{reticleProbeApi.read,reticleProbeApi.context,reticleProbeApi.reticleTable,reticleProbeApi.probe};
        if(!ControllerAimTargetReader(api).sample(owner,ray,target))return decline("TARGET_QUERY_UNAVAILABLE");
        const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        const auto afterView=presentation.snapshot(now);LocalWeapon after{};runtime::ControllerSample current{};StereoCameraPlan currentPlan{};
        if(!afterView.world()||afterView.revision!=view.revision||!holsterOutputInput(now,current)||
           current.epoch!=input.epoch||current.referenceSpace!=input.referenceSpace||current.continuity!=input.continuity||
           !validPose(current.aim[1])||!controllerAimSpace.plan(currentPlan)||currentPlan.sceneId!=plan.sceneId||
           !headCamera.matches(plan,now,view.revision)||!weaponOwner.inspect(after)||!sameAimOwner(owner,after)||
           !WeaponContextReader(weaponObservationApi).inspect(after,context)||
           !controllerAimRay(after,context,plan,input,view.revision,now,ray))return decline("TARGET_CONTEXT_CHANGED");
        ControllerRay afterMuzzle{};
        if(tracked&&(!trackedWeaponMuzzle(
            {after,view.revision,input.epoch,input.referenceSpace,input.continuity},now,ray,afterMuzzle)||afterMuzzle!=muzzle))
            return decline("TARGET_ATTACHMENT_CHANGED");
        // The native target setter and manager retain their call frequency,
        // RNG/aimer selection, native animated muzzle on the tracked attachment,
        // spread and projectile arrays. Target and model share the actual muzzle.
        pendingAimTarget=target;pendingAimStamp=now;pendingAimGuid=owner.actorGuid;pendingAimRevision=view.revision;
        pendingAimTracked=tracked;
        point=target.point;++targetAimPrepared;targetAimState="CONTROLLER_TARGET_PREPARED";return true;
    }catch(...){return decline("TARGET_QUERY_EXCEPTION");}
#else
    return false;
#endif
}
void gameWeaponAimComplete(bool prepared) noexcept {
    if(!pendingAimLocal)return;
    pendingAimLocal=false;
    std::lock_guard lock(targetAimMutex);
    if(prepared){lastAimTarget=pendingAimTarget;targetAimStamp=pendingAimStamp;targetAimGuid=pendingAimGuid;
        targetAimRevision=pendingAimRevision;targetAimTracked=pendingAimTracked;++targetAimCompleted;}
    else targetAimStamp=0;
}
bool gamePrepareWeaponPose(const WeaponPoseCall& call,WeaponNativeMatrix& matrix,std::uint64_t timestamp) noexcept{
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
    diagnostics::WallScope timing(diagnostics::frameTimings.controllerAttachment);
    const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
    pendingAttachmentLocal=pendingAttachmentPrepared=pendingAttachmentAnimated=false;scopedModelPose.reset();
    if(vrWeaponPose(call,matrix,timestamp))return true; // VR-held weapon at the holding controller (any thread)
    const auto decline=[](const char* reason){controllerAttachment.reset();animatedAttachment.reset();scopedModelPose.reset();attachmentState=reason;++attachmentDeclined;
        std::lock_guard lock(attachmentMutex);attachmentStamp=0;return false;};
    try{
        if(!weaponObservationReady.load(std::memory_order_acquire)||!reticleProbeReady.load(std::memory_order_acquire)||
           !weaponOwner.onOwnerThread())return false;
        LocalWeapon owner{};if(!weaponOwner.inspect(owner))return decline("OWNER_UNAVAILABLE");
        if(owner.weapon!=reinterpret_cast<std::uintptr_t>(call.weapon))return false;
        pendingAttachmentLocal=true;
        if(nativeUi.update(timestamp)&&nativeUi.inputRearmRequired())nativePad.rearm();
        const auto view=presentation.snapshot(timestamp);runtime::ControllerSample input{};StereoCameraPlan plan{};WeaponContext context{};
        if(!view.world()||!holsterOutputInput(timestamp,input)||!controllerAimSpace.plan(plan)||
           !headCamera.matches(plan,timestamp,view.revision)||!WeaponContextReader(weaponObservationApi).inspect(owner,context))
            return decline("NATIVE_CONTEXT_OR_TRACKING");
        ControllerAttachmentPlan proposed{};
        bool animated=false;AnimatedAttachmentPlan animatedPlan{};
        if(!controllerAttachment.prepare(reticleProbeApi.attachment,NativeAimProbe(reticleProbeApi.probe),owner,context,
            plan,input,view.revision,timestamp,call,proposed)){
            if(!weaponModelReady.load(std::memory_order_acquire)||
               !animatedAttachment.prepare(reticleProbeApi.attachment,weaponModelApi,NativeAimProbe(reticleProbeApi.probe),owner,context,
                   plan,input,view.revision,timestamp,call,animatedPlan))return decline("ATTACHMENT_NOT_QUALIFIED");
            proposed=animatedPlan.pose;animated=true;
        }else animatedAttachment.reset();
        const auto now=diagnostics::monotonicNs();const auto afterView=presentation.snapshot(now);
        LocalWeapon after{};WeaponContext afterContext{};runtime::ControllerSample current{};StereoCameraPlan currentPlan{};
        if(!afterView.world()||afterView.revision!=view.revision||!holsterOutputInput(now,current)||
           current.epoch!=input.epoch||current.referenceSpace!=input.referenceSpace||current.continuity!=input.continuity||
           !validPose(current.aim[1])||!validPose(current.grip[1])||!controllerAimSpace.plan(currentPlan)||
           currentPlan.sceneId!=plan.sceneId||!headCamera.matches(plan,now,view.revision)||
           !weaponOwner.inspect(after)||!sameAimOwner(owner,after)||!WeaponContextReader(weaponObservationApi).inspect(after,afterContext)||
           afterContext.flags()!=context.flags()||now<timestamp||now-timestamp>100'000'000)
            return decline("ATTACHMENT_CONTEXT_CHANGED");
        if(animated){
            if(!scopedModelPose.arm(owner,animatedPlan.binding,call.nativeFlag,proposed.before.model,
                animatedPlan.modelArgument.values,proposed.argument.values)||
               !WeaponAttachmentReader(reticleProbeApi.attachment).argument(call.attachment,matrix.values))return decline("MODEL_SCOPE_UNAVAILABLE");
            // Keep the original attachment until the one native model movement
            // returns true. Only then can the wrapper-owned argument be changed.
            pendingAnimatedAttachment=animatedPlan;++animatedAttachmentPrepared;
        }else matrix=proposed.argument;
        pendingAttachment=proposed;pendingAttachmentPrepared=true;pendingAttachmentAnimated=animated;
        ++attachmentPrepared;if(proposed.continuedPendingFire)++attachmentPendingFire;if(proposed.tracked.clamped)++attachmentClamped;
        attachmentState="CONTROLLER_ATTACHMENT_PREPARED";return true;
    }catch(...){return decline("ATTACHMENT_EXCEPTION");}
#else
    return false;
#endif
}
void completeControllerAttachment(const WeaponPoseCall& call,bool prepared,std::uint64_t timestamp) noexcept{
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
    if(!pendingAttachmentLocal)return;pendingAttachmentLocal=false;
    const auto decline=[](){controllerAttachment.reset();animatedAttachment.reset();scopedModelPose.reset();attachmentState="ATTACHMENT_COMPLETION_UNAVAILABLE";
        std::lock_guard lock(attachmentMutex);attachmentStamp=0;};
    if(!prepared||!pendingAttachmentPrepared){pendingAttachmentPrepared=false;decline();return;}
    pendingAttachmentPrepared=false;
    try{
        const auto view=presentation.snapshot(timestamp);LocalWeapon owner{};WeaponContext context{};
        runtime::ControllerSample input{};StereoCameraPlan camera{};ControllerRay ray{};
        if(!view.world()||!weaponOwner.onOwnerThread()||!weaponOwner.inspect(owner)||
           !WeaponContextReader(weaponObservationApi).inspect(owner,context)||!holsterOutputInput(timestamp,input)||
           !controllerAimSpace.plan(camera)||!headCamera.matches(camera,timestamp,view.revision)||
           !controllerAimRay(owner,context,camera,input,view.revision,timestamp,ray)||!validPose(input.grip[1])){decline();return;}
        WeaponAttachmentGeometry actual{};
        const ControllerAttachmentScope scope{owner,view.revision,input.epoch,input.referenceSpace,input.continuity};
        const bool completed=pendingAttachmentAnimated?
            animatedAttachment.finish(reticleProbeApi.attachment,weaponModelApi,pendingAnimatedAttachment,scope,call,prepared,scopedModelPose,timestamp,actual):
            controllerAttachment.finish(reticleProbeApi.attachment,pendingAttachment,scope,call,prepared,timestamp,actual);
        if(!completed){decline();return;}
        if(pendingAttachmentAnimated)++animatedAttachmentCompleted;
        {std::lock_guard lock(attachmentMutex);attachmentStamp=timestamp;attachmentGuid=owner.actorGuid;attachmentAnimated=pendingAttachmentAnimated;
            for(unsigned i=0;i<3;++i)attachmentMuzzle[i]=actual.muzzle[12+i];}
        ++attachmentCompleted;attachmentState="CONTROLLER_ATTACHMENT_COMPLETED";
    }catch(...){decline();}
#endif
}
void observeWeaponSkeleton(const LocalWeapon& owner,unsigned phase,std::uint64_t timestamp) noexcept{
    if(phase>=skeletonAuditRecords.size())return;
    try{
        WeaponSkeletonEvidence evidence{};LocalWeapon after{};WeaponContext context{};
        const bool sampled=WeaponSkeletonAudit(reticleProbeApi.attachment).inspect(owner,evidence)&&
            weaponOwner.inspect(after)&&sameAimOwner(owner,after)&&
            WeaponContextReader(weaponObservationApi).inspect(after,context)&&!context.knownLock();
        std::lock_guard lock(skeletonAuditMutex);auto& record=skeletonAuditRecords[phase];
        if(!sampled){record.stamp=0;++record.declined;return;}
        record.evidence=evidence;record.stamp=timestamp;record.actorGuid=owner.actorGuid;++record.samples;
        if(evidence.pose.rootMatchesModel&&evidence.pose.globalsMatchLocals)++record.coherent;
    }catch(...){std::lock_guard lock(skeletonAuditMutex);auto& record=skeletonAuditRecords[phase];record.stamp=0;++record.declined;}
}
void gameObserveWeaponPose(const WeaponPoseCall& call,std::uint64_t timestamp,std::uint64_t thread) noexcept{
    poseGeometryAudit.reset();
    if(!weaponObservationReady.load(std::memory_order_acquire))return;
    WeaponCallRecord record{WeaponCallPhase::Pose,timestamp,thread,reticleUpdates.load()};
    record.flags=(call.nativeFlag?1u:0u)|(call.velocity?2u:0u);
    if(std::isfinite(call.nativeScalar))record.nativeScalar=call.nativeScalar;else record.flags|=0x80000000u;
    if(!weaponCallTrace.record(reinterpret_cast<std::uintptr_t>(call.weapon),record))return;
#ifdef RDRVR_NATIVE_MENUS
    try{
        if(++poseGeometryOrdinal%30!=1||!reticleProbeReady.load(std::memory_order_acquire)||!weaponOwner.onOwnerThread())return;
        const auto view=presentation.snapshot(timestamp);if(!view.world())return;
        LocalWeapon owner{};WeaponContext context{};
        if(!weaponOwner.inspect(owner)||owner.weapon!=reinterpret_cast<std::uintptr_t>(call.weapon)||
           !WeaponContextReader(weaponObservationApi).inspect(owner,context)||context.knownLock()||
           !poseGeometryAudit.begin(reticleProbeApi.attachment,owner,call,timestamp)){++poseGeometryDeclined;return;}
        poseGeometryRevision=view.revision;++poseGeometryBegun;
        observeWeaponSkeleton(owner,0,timestamp);
    }catch(...){poseGeometryAudit.reset();++poseGeometryDeclined;}
#endif
}
void gameObserveWeaponPoseComplete(const WeaponPoseCall& call,bool prepared,std::uint64_t timestamp,std::uint64_t thread) noexcept{
    vrWeaponPoseComplete(call,prepared);
    completeControllerAttachment(call,prepared,timestamp);
    if(!weaponObservationReady.load(std::memory_order_acquire))return;
    WeaponCallRecord record{WeaponCallPhase::PoseComplete,timestamp,thread,reticleUpdates.load()};
    record.flags=(call.nativeFlag?1u:0u)|(call.velocity?2u:0u)|(prepared?4u:0u);
    if(std::isfinite(call.nativeScalar))record.nativeScalar=call.nativeScalar;else record.flags|=0x80000000u;
    const bool recorded=weaponCallTrace.record(reinterpret_cast<std::uintptr_t>(call.weapon),record);
#ifdef RDRVR_NATIVE_MENUS
    if(!poseGeometryAudit.pending())return;
    try{
        const auto view=presentation.snapshot(timestamp);LocalWeapon owner{};WeaponContext context{};WeaponPoseGeometryDelta delta{};
        if(!recorded||!view.world()||view.revision!=poseGeometryRevision||!weaponOwner.onOwnerThread()||!weaponOwner.inspect(owner)||
           !WeaponContextReader(weaponObservationApi).inspect(owner,context)||context.knownLock()||
           !poseGeometryAudit.finish(reticleProbeApi.attachment,owner,call,prepared,timestamp,delta)){
            poseGeometryAudit.reset();++poseGeometryDeclined;return;
        }
        {std::lock_guard guard(poseGeometryMutex);poseGeometryLast=delta;poseGeometrySampleNs=timestamp;}
        ++poseGeometryCompleted;if(delta.animated)++poseGeometryAnimated;
        observeWeaponSkeleton(owner,1,timestamp);
    }catch(...){poseGeometryAudit.reset();++poseGeometryDeclined;}
#endif
}
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
#define RDRVR_VR_CUTSCENE 1
// ===== CUTSCENES =====
// The presentation stays Cutscene / CameraTransition (every gameplay module keeps its world() gate: no hands, holsters, weapons, loco,
// snap turn, HUD capture). Only the RENDER path (visibility / cull / primary eye / replay / XR layer / present bypass) also accepts an
// engaged cutscene: stereoPresentation(). Cutscene anchors live in their own mailbox (the render thread publishes them at
// CreateSceneVisibility from the stock = cutscene camera), so the main thread's gameplay headCamera publish/invalidate never races them.
HeadCameraMailbox cutsceneCamera;
static ::rdrvr::vr::cutscene::Ctx cutsceneCtx(runtime::PresentationContext c) noexcept {
    using P=runtime::PresentationContext;using C=::rdrvr::vr::cutscene::Ctx;
    return c==P::World?C::World:c==P::Cutscene?C::Cutscene:c==P::CameraTransition?C::Transition:c==P::CameraOverride?C::Override:C::Other;}
static bool cutsceneStereo(const runtime::PresentationState& s,std::uint64_t now) noexcept {
    return (s.context==runtime::PresentationContext::Cutscene||s.context==runtime::PresentationContext::CameraTransition||
            (s.context==runtime::PresentationContext::CameraOverride&&::rdrvr::vr::cutscene::overrideLever.load(std::memory_order_relaxed)))&&
        ::rdrvr::vr::cutscene::engaged(s.revision,now);}
static bool stereoPresentation(const runtime::PresentationState& s,std::uint64_t now) noexcept {return s.world()||cutsceneStereo(s,now);}
static bool stereoPresentationAllowed() noexcept {
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    return stereoPresentation(presentation.snapshot(now),now);}
// The render-path head check: gameplay plans against headCamera, cutscene plans against cutsceneCamera (same revision / age rules).
static bool renderHeadMatches(const StereoCameraPlan& plan,std::uint64_t now,const runtime::PresentationState& s) noexcept {
    if(s.world())return headCamera.matches(plan,now,s.revision);
    return cutsceneStereo(s,now)&&::rdrvr::vr::cutscene::cutscenePlan(plan.actorGuid)&&cutsceneCamera.matches(plan,now,s.revision);}
// Main thread (PostCameraUpdate): John for FIRST PERSON. The player actor (PLAYERMGR+0xa20 -> sagPlayer -> registry, as HeadAnchorReader)
// must be grabbed by the cutscene (actor+0x112 bit 1/2 = ActorIsGrabbedByCutscene 0x24201fc) and not hidden (bit 3 = ActorIsHiddenByCutscene
// 0x2420268); in a transition after a cutscene only "not hidden". Eye = mid of the eyePivot_l/_r globals (VrBody rig), else the head bone.
// The cutscene's own actors (rdr2CutSceneManager +0xf248: entries of 0x18 bytes, u32 gohGUID at +0, u16 count at +0xf250;
// names through the native rdr2CutSceneManager::GetCutsceneActorName(int) const). Set by TraceEntry; checked against the manager vtable.
std::uintptr_t cutManagerGlobal{},cutManagerTable{},cutManagerTable2{},gohManagerGlobal{},gohSlotsGlobal{};const char* (*cutActorName)(std::uintptr_t,int){};
// The cutscene entries hold gohGUIDs (bhHelperFunctions::GetActorPtr 0x20404a4): aGuidGeneral<gohBase> sm_Manager (u16 count at
// [mgr]) / sm_ManagerSlots (16-byte slots: object, u16 generation at +8); object+0x2c == 0xf = actor; object+0xb0 = the actor GUID.
void gameConfigureCutsceneActors(std::uintptr_t global,std::uintptr_t table,std::uintptr_t table2,const char* (*name)(std::uintptr_t,int),
                                 std::uintptr_t gohManager,std::uintptr_t gohSlots) noexcept {
    cutManagerGlobal=global;cutManagerTable=table;cutManagerTable2=table2;cutActorName=name;gohManagerGlobal=gohManager;gohSlotsGlobal=gohSlots;}
std::mutex cutNamesMutex;std::string cutNames;
std::string gameCutsceneActorNames(){std::lock_guard lock(cutNamesMutex);return cutNames;}
static bool icontains(const char* s,const char* w) noexcept {
    if(!s)return false;const std::size_t n=std::strlen(w);
    for(;*s;++s){std::size_t i=0;while(i<n&&s[i]&&std::tolower(static_cast<unsigned char>(s[i]))==w[i])++i;if(i==n)return true;}
    return false;}
static void cutsceneJohn(const runtime::PresentationState& state,::rdrvr::vr::cutscene::John& j) noexcept {
    namespace bk=::rdrvr::vr::bodyik;namespace cs=::rdrvr::vr::cutscene;const auto& api=weaponObservationApi;
    const auto get=[&](std::uintptr_t p,std::size_t o,auto& out){return p&&p<=UINTPTR_MAX-o-sizeof(out)&&api.read(api.context,p+o,&out,sizeof(out));};
    j.why="NO_API";
    if(!api.read||!reticleProbeApi.head||!weaponObservationReady.load(std::memory_order_acquire))return;
    std::uintptr_t table{},player{},manager{},slots{},actor{},again{},entity{},instance{},creature{};std::uint32_t guid{};std::uint8_t active{};
    std::uint16_t count{},generation{},f112{};
    j.why="NO_PLAYER";
    if(!get(api.playerManager,0,table)||table!=api.playerManagerTable||!get(api.playerManager,0xa20,player)||!get(player,0,table)||
       table!=api.playerTable||!get(player,0x5f1,active)||active!=1||!get(player,0x5f4,guid)||!get(api.actorManagerGlobal,0,manager)||
       !get(api.actorSlotsGlobal,0,slots)||!get(manager,0,count)||!slots)return;
    std::uintptr_t index=guid&0xffffu;
    if(index>=count||!get(slots,index*16,actor)||!actor||!get(slots,index*16+8,generation)||generation!=(guid>>16))return;
    j.why="NO_FLAGS";if(!get(actor,0x112,f112))return;
    bool substituted=false;
    if(f112&8u){
        // RDR hides the player and plays the scene with its own John: an actor grabbed by the cutscene (bits 1/2), not hidden,
        // with the player's skeleton data (same model = same crSkeletonData as VrBody's rig). Scanned every 250 ms, cached in between.
        static std::uintptr_t cachedIndex{~std::uintptr_t(0)};static std::uint16_t cachedGen{};static std::uint64_t scanNs{};
        const bk::Rig* pr=bk::currentRig;j.why="HIDDEN_BY_CUTSCENE";
        if(!bk::installed.load(std::memory_order_acquire)||!pr||!pr->data)return;
        auto candidate=[&](std::uintptr_t i,std::uintptr_t& a,std::uint16_t& g,std::uint16_t& f)->bool{
            bk::SkelRead ks{};
            return get(slots,i*16,a)&&a&&get(slots,i*16+8,g)&&get(a,0x112,f)&&(f&6u)&&!(f&8u)&&bk::readSkeleton(a,ks)&&ks.data==pr->data;};
        std::uintptr_t a{};std::uint16_t g{},f{};bool found=false;
        auto byGuid=[&](std::uint32_t gid)->bool{ // a cutscene actor's gohGUID -> live, not hidden actor
            const std::uintptr_t i=gid&0xffffu;std::uint16_t gg{},ff{};std::uintptr_t aa{};
            if(i>=count||!get(slots,i*16,aa)||!aa||!get(slots,i*16+8,gg)||gg!=(gid>>16)||!get(aa,0x112,ff)||(ff&8u))return false;
            a=aa;g=gg;f=ff;cachedIndex=i;cachedGen=gg;return true;};
        if(cachedIndex<count&&candidate(cachedIndex,a,g,f)&&g==cachedGen)found=true;
        else if(cachedIndex<count&&get(slots,cachedIndex*16,a)&&a&&get(slots,cachedIndex*16+8,g)&&g==cachedGen&&get(a,0x112,f)&&!(f&8u))found=true;
        else if(j.ns<scanNs||j.ns-scanNs>250'000'000ull){
            scanNs=j.ns;cachedIndex=~std::uintptr_t(0);
            // 1) by name among the cutscene's own actors ("john" / "marston"); every name goes to the report
            std::uintptr_t mgr{},mt{},entries{};std::uint16_t n{};
            if(cutManagerGlobal&&cutActorName&&get(cutManagerGlobal,0,mgr)&&mgr&&get(mgr,0,mt)&&(mt==cutManagerTable||mt==cutManagerTable2)&&
               get(mgr,0xf248,entries)&&entries&&get(mgr,0xf250,n)&&n<64){
                std::string names;
                for(std::uint16_t i=0;i<n;++i){
                    std::uint32_t gid{};if(!get(entries,std::size_t(i)*0x18,gid))continue;
                    const char* nm=cutActorName(mgr,int(i));char row[96];std::snprintf(row,sizeof row,"%s%s:%08x",i?",":"",nm?nm:"?",gid);names+=row;
                    if(!found&&(icontains(nm,"player")||icontains(nm,"john")||icontains(nm,"marston"))){
                        std::uintptr_t gm{},gs{},obj{};std::uint16_t gc{},gg{};std::uint8_t type{};std::uint32_t handle{};
                        if(gohManagerGlobal&&get(gohManagerGlobal,0,gm)&&gm&&get(gm,0,gc)&&(gid&0xffffu)<gc&&get(gohSlotsGlobal,0,gs)&&gs&&
                           get(gs,std::size_t(gid&0xffffu)*16,obj)&&obj&&get(gs,std::size_t(gid&0xffffu)*16+8,gg)&&gg==(gid>>16)&&
                           get(obj,0x2c,type)&&type==0xf&&get(obj,0xb0,handle)&&byGuid(handle))found=true;
                        else if(byGuid(gid))found=true;
                    }
                }
                {std::lock_guard lock(cutNamesMutex);cutNames=names;}
            }
            // 2) the player's skeleton data among the grabbed actors
            for(std::uintptr_t i=0;i<count&&!found;++i)if(candidate(i,a,g,f)){found=true;cachedIndex=i;cachedGen=g;}
        }
        if(!found)return;
        index=cachedIndex;actor=a;f112=f;guid=(std::uint32_t(g)<<16)|std::uint32_t(index);cs::bump(cs::stats.johnSubstitute);substituted=true;
    }
    if(!substituted&&state.context==runtime::PresentationContext::Cutscene&&!(f112&6u)){j.why="NOT_IN_CUTSCENE";return;}
    j.why="NO_CREATURE";
    if(!get(actor,0x50,entity)||!get(entity,0x88,instance)||!get(instance,0x138,creature)||!creature)return;
    AimNativeVector head{};head.v.fill(std::numeric_limits<float>::quiet_NaN());
    j.why="NO_HEAD";
    if(!reticleProbeApi.head(reinterpret_cast<const void*>(actor),&head)||!std::isfinite(head.v[0]+head.v[1]+head.v[2]))return;
    if(!get(slots,index*16,again)||again!=actor){j.why="NO_PLAYER";return;}
    j.head={head.v[0],head.v[1],head.v[2]};j.eye=j.head;j.guid=guid;j.creature=creature;
    j.hat=readBodyHat(bodyAttachmentApi,api.read,api.context,actor);
    // eye pivots of this skeleton (VrBody's rig; same main thread as its tick)
    bk::SkelRead sk{};const bk::Rig* r=bk::currentRig;
    if(bk::installed.load(std::memory_order_acquire)&&r&&bk::readSkeleton(actor,sk)&&r->data==sk.data&&r->count==int(sk.count)&&
       r->eyePivot[0]>=0&&r->eyePivot[1]>=0&&r->eyePivot[0]<r->count&&r->eyePivot[1]<r->count){
        float a[3]{},b[3]{};
        if(bk::detail::block(sk.globals+std::uintptr_t(r->eyePivot[0])*64+48,a,sizeof a)&&bk::detail::block(sk.globals+std::uintptr_t(r->eyePivot[1])*64+48,b,sizeof b)){
            const cs::V3 e{(a[0]+b[0])*.5f,(a[1]+b[1])*.5f,(a[2]+b[2])*.5f};const float d=cs::dist(e,j.head);
            if(cs::finite3(e)&&d<.3f){
                // collapsed head sample (VrBody hook C skinning window): not a pose - the composer holds the last good John
                if(d<.02f){cs::bump(cs::stats.johnCollapsed);j.why="HEAD_COLLAPSED";return;}
                j.eye=e;j.eyePivots=true;j.headingValid=cs::headingOfForward(e[0]-j.head[0],e[2]-j.head[2],j.heading);
            }
        }
    }
    if(!j.headingValid){ // actor (mover) matrix: forward = -row c (mvrMoverComponent::GetHeading = atan2(-c.x, -c.z))
        std::uintptr_t component{},matrix{};std::array<float,3> c{};
        if(get(actor,0xb0,component)&&get(component,0x18,matrix)&&get(matrix,0x20,c))j.headingValid=cs::headingOfForward(-c[0],-c[2],j.heading);
    }
    j.valid=true;j.why="OK";
}
static void vrCutsceneObserve(const runtime::PresentationState& state,std::uint64_t now) noexcept {
    namespace cs=::rdrvr::vr::cutscene;
    try{
        // Kill switch: no VrBody gates either
        const bool live=::rdrvr::vr::enabled(::rdrvr::vr::Feature::Cutscene);
        const bool cut=live&&(state.context==runtime::PresentationContext::Cutscene||
                              (state.context==runtime::PresentationContext::CameraOverride&&cs::overrideLever.load(std::memory_order_relaxed))),
                   trans=live&&state.context==runtime::PresentationContext::CameraTransition;
        cs::inCutscene.store(cut,std::memory_order_relaxed);
        { // R3 (right stick click) inside a cutscene cycles CINEMATIC -> FIRST PERSON -> SCREEN (saved like the menu row)
            static bool held=true;runtime::ControllerSample in{};
            const bool down=controllers.snapshot(now,in)&&(in.buttons&runtime::PadRightStick);
            if(live&&(cut||trans)&&down&&!held){
                const unsigned cur=::rdrvr::vr::settings().cutsceneMode();
                ::rdrvr::vr::vrSettings.set(::rdrvr::vr::Key::Cutscenes,cur==cs::Cinematic?unsigned(cs::FirstPerson):cur==cs::FirstPerson?unsigned(cs::Screen):unsigned(cs::Cinematic));
                cs::bump(cs::stats.r3Cycles);
            }
            held=down;
            // Debug lever: debug.rdrvr.vr.cutscene.force N (1 SCREEN, 2 CINEMATIC, 3 FIRST PERSON) sets the mode once per new value
            static unsigned polls=0;static int lastForce=0;
            if((polls++&31u)==0){char v[PROP_VALUE_MAX]{};const int f=__system_property_get("debug.rdrvr.vr.cutscene.force",v)>0?std::atoi(v):0;
                if(f>=1&&f<=3&&f!=lastForce)::rdrvr::vr::vrSettings.set(::rdrvr::vr::Key::Cutscenes,unsigned(f-1));
                lastForce=f;}
        }
        const auto mode=::rdrvr::vr::settings().cutsceneMode();const bool on=cs::requested();
        cs::John j{};j.ns=now;j.revision=state.revision;j.why=on&&(cut||trans)?"NOT_FIRST_PERSON":"NOT_ACTIVE";
        if(on&&(cut||trans)&&mode==cs::FirstPerson){cutsceneJohn(state,j);cs::bump(cs::stats.johnSamples);if(j.valid)cs::bump(cs::stats.johnValid);}
        cs::publishJohn(j);cs::stats.johnWhy.store(j.why,std::memory_order_relaxed);
        cs::stats.head.store(!j.valid?"NONE":j.eyePivots?"EYE_PIVOTS":"HEAD_BONE",std::memory_order_relaxed);
        cs::suppressCollapse.store((cut||trans)&&!(j.valid&&mode==cs::FirstPerson),std::memory_order_relaxed);
        if(!live){::rdrvr::vr::status::publish(::rdrvr::vr::Feature::Cutscene,::rdrvr::vr::status::Property);return;}
        const bool active=cutsceneStereo(state,now);
        ::rdrvr::vr::status::publish(::rdrvr::vr::Feature::Cutscene,::rdrvr::vr::status::Ready,
            !active?nullptr:mode==cs::FirstPerson&&cs::engagedMode.load(std::memory_order_relaxed)!=cs::FirstPerson?"FALLBACK":"ACTIVE");
    }catch(...){}
}
// Render thread (CreateSceneVisibility): the cutscene anchor of this visibility call, published to cutsceneCamera.
static bool cutsceneAnchor(std::uintptr_t viewport,std::uintptr_t camera,std::uint64_t ns,VisibilityCameraBridge::Reader read,void* context,
                           const runtime::PresentationState& state,::rdrvr::vr::cutscene::Ctx ctx,HeadCameraAnchor& out) noexcept {
    namespace cs=::rdrvr::vr::cutscene;
    try{
        StockCamera stock{};std::array<std::byte,cameraReadBytes> fields{};
        bool cam=read&&viewport&&viewport<=UINTPTR_MAX-cameraReadOffset-cameraReadBytes&&read(context,viewport+cameraReadOffset,fields.data(),fields.size());
        if(cam&&camera)cam=camera<=UINTPTR_MAX-64&&read(context,camera,fields.data(),64);
        cam=cam&&decodeCamera(fields,stock);
        cs::Input in{};in.ns=ns;in.revision=state.revision;in.ctx=ctx;in.mode=::rdrvr::vr::settings().cutsceneMode();in.stock=cam?stock.world.data():nullptr;
        runtime::TrackedViews v{};
        if(gameViews.snapshot(ns,v)&&validPose(v.eyes[0])&&validPose(v.eyes[1])){
            runtime::Quat q{};float sign{},norm{};
            for(unsigned k=0;k<4;++k)sign+=v.eyes[0].orientation[k]*v.eyes[1].orientation[k];
            for(unsigned k=0;k<4;++k){q[k]=v.eyes[0].orientation[k]+(sign<0?-1.f:1.f)*v.eyes[1].orientation[k];norm+=q[k]*q[k];}
            norm=std::sqrt(norm);
            if(norm>1e-3f){for(float& f:q)f/=norm;const auto fwd=runtime::rotate(q,{0,0,-1});
                in.viewsValid=cs::headingOfForward(fwd[0],fwd[2],in.hmdYaw);
                for(unsigned k=0;k<3;++k)in.hmdCenter[k]=(v.eyes[0].position[k]+v.eyes[1].position[k])*.5f;
                in.referenceSpace=v.epochs.referenceSpace;}
        }
        in.john=cs::johnSnapshot();
        cs::Output o{};if(!cs::composer.step(in,o))return false;
        HeadCameraAnchor a{};a.position=o.position;a.up={0,1,0};a.actorGuid=o.guid;a.sampledNs=ns;a.presentationRevision=state.revision;
        // CINEMATIC: a creature that never matches (nobody hidden, no head fill); FIRST PERSON: John -> the gameplay eye body rules
        a.bodyCreature=o.effective==cs::FirstPerson&&o.creature?o.creature:std::uintptr_t(1);a.hatNode=o.effective==cs::FirstPerson?o.hat:0;
        a.origin.position=o.origin;a.origin.inverseYaw={0,std::sin(o.inverseYaw*.5f),0,std::cos(o.inverseYaw*.5f)};
        a.origin.referenceSpace=o.referenceSpace;a.origin.serial=o.serial;a.origin.valid=true;a.kind=HeadCameraAnchor::Kind::Head;
        if(!cutsceneCamera.publish(a))return false;
        cs::engagedMode.store(o.effective,std::memory_order_relaxed);out=a;return true;
    }catch(...){return false;}
}
#endif
// RIDING: HeadAnchorReader (mirror) rejects the player while player+0x5d4 is 3/5 (mounted / attached: the mover's +0x830 phInst)
// and while the actor matrix changes between its two reads, so on a horse no head is published -> CULL_SNAPSHOT_UNAVAILABLE every
// frame -> the 2D theater. When the strict sample fails and the player's mover is attached, this relaxed sample (same identity chain,
// no state / lock / matrix-stability checks, the native head bone required) publishes a HEAD anchor (CAMERA HEAD for that frame).
// debug.rdrvr.vr.riding 0 = off.
std::atomic<std::uint64_t> ridingSamples{},ridingNoHead{},ridingNotAttached{},ridingIdentity{},ridingTurns{},ridingTurnDeclined{};std::atomic<std::int32_t> ridingLastState{-1};
float ridingHeading{},ridingForwardHeading{};bool ridingHeadingValid{};std::atomic<std::uint64_t> ridingRecenters{};std::atomic<float> ridingRecenterDeg{0}; // main thread: the rider's heading of the last riding sample (atan2(c.x, c.z))
std::atomic<float> ridingTurnedDeg{0};
struct RidingTrace {float speed{},dy{},lf{},ls{},up{},fwd{},bodyY{},eyeY{};};std::array<RidingTrace,64> ridingTrace{};std::atomic<unsigned> ridingTraceNext{};
#if defined(RDRVR_NATIVE_MENUS)
namespace {int ridingViewTurn(float radians) noexcept;}
#endif
static bool ridingLever() noexcept {
    static unsigned calls=0;static bool lever=true;
    if((calls++&255u)==0){char v[PROP_VALUE_MAX]{};lever=!(__system_property_get("debug.rdrvr.vr.riding",v)>0&&v[0]=='0');}
    return lever;}
static bool ridingAnchorSample(std::uintptr_t reticle,std::uint64_t now,std::uint64_t revision,HeadCameraAnchor& out,BodyAnchorFacts& facts) noexcept {
    const auto& api=weaponObservationApi;
    const auto get=[&](std::uintptr_t p,std::size_t o,auto& v){return p&&p<=UINTPTR_MAX-o-sizeof(v)&&api.read(api.context,p+o,&v,sizeof(v));};
    if(!api.read||!reticleProbeApi.head||!reticleProbeApi.reticleTable||!now||!revision)return false;
    std::uintptr_t table{},player{},manager{},slots{},actor{},back{},component{},matrix{},health{},entity{},instance{},creature{},mover{},attached{};
    std::uint32_t guid{},pstate{};std::uint16_t count{},generation{};std::uint8_t active{};float hp{};
    if(!get(reticle,0,table)||table!=reticleProbeApi.reticleTable||!get(api.playerManager,0,table)||table!=api.playerManagerTable||
       !get(api.playerManager,0xa20,player)||!get(player,0,table)||table!=api.playerTable||!get(player,0x5f1,active)||active!=1||
       !get(player,0x5f4,guid)||!get(api.actorManagerGlobal,0,manager)||!get(api.actorSlotsGlobal,0,slots)||!get(manager,0,count)||!slots){
        ridingIdentity.fetch_add(1,std::memory_order_relaxed);return false;}
    const std::uintptr_t index=guid&0xffffu;
    if(index>=count||!get(slots,index*16,actor)||!actor||!get(slots,index*16+8,generation)||generation!=(guid>>16)||
       !get(reticle,0x10,back)||back!=actor||!get(actor,0xb0,component)||!get(component,0x18,matrix)||!matrix||
       !get(actor,0x60,health)||!get(health,0x20,hp)||!std::isfinite(hp)||hp<=0||
       !get(actor,0x50,entity)||!get(entity,0x88,instance)||!get(instance,0x138,creature)||!creature){ridingIdentity.fetch_add(1,std::memory_order_relaxed);return false;}
    if(get(player,0x5d4,pstate))ridingLastState.store(std::int32_t(pstate),std::memory_order_relaxed);
    if(!get(actor,0x88,mover)||!mover||!get(mover,0x830,attached)||!attached){ridingNotAttached.fetch_add(1,std::memory_order_relaxed);return false;}
    runtime::Mat4 body{};if(!get(matrix,0,body))return false;
    for(unsigned c=0;c<4;++c)for(unsigned r=0;r<3;++r)if(!std::isfinite(body[c*4+r]))return false;
    AimNativeVector head{};head.v.fill(std::numeric_limits<float>::quiet_NaN());
    if(!reticleProbeApi.head(reinterpret_cast<const void*>(actor),&head)||!std::isfinite(head.v[0]+head.v[1]+head.v[2])){ridingNoHead.fetch_add(1,std::memory_order_relaxed);return false;}
    HeadCameraAnchor a{};a.actorGuid=guid;a.sampledNs=now;a.presentationRevision=revision;a.bodyCreature=creature;
    a.hatNode=readBodyHat(bodyAttachmentApi,api.read,api.context,actor);
    for(unsigned n=0;n<3;++n){a.position[n]=head.v[n];a.up[n]=body[4+n];}
    // RIDING EYE (users: galloping dropped the view to the bottom of the horse - a gallop right after mounting never learned the
    // offset, so the raw head bone (leaning to the horse's neck) was the eye; the body bounced in front of the smoothed eye):
    //  * up = the HIGHEST head-above-body seen this ride (the upright seat; the gallop lean never lowers it), reset at every mount;
    //  * forward / side = running mean (1 s) over the upright frames only (head within 4 cm of that height);
    //  * eye = body origin + that offset + the eye above / in front of the head joint (8 cm / 6 cm: the body's bounce stays below the view);
    //  * height low-passed (debug.rdrvr.vr.riding.bob <ms>, default 500, 0 = raw). Report riding.trace (10 Hz ring) for tuning.
    {
        static std::uint64_t lastNs{},lastTraceNs{};static float off[3]{},y{},prevBody[3]{};static bool have{};static unsigned calls{};static int tauMs{500};
        if((calls++&255u)==0){char v[PROP_VALUE_MAX]{};tauMs=__system_property_get("debug.rdrvr.vr.riding.bob",v)>0?std::clamp(std::atoi(v),0,3000):500;}
        float fx=-body[8],fz=-body[10];const float fl=std::sqrt(fx*fx+fz*fz);if(fl>1e-3f){fx/=fl;fz/=fl;}else{fx=0;fz=1;}
        const float sx=fz,sz=-fx;
        const float dx=head.v[0]-body[12],dy=head.v[1]-body[13],dz=head.v[2]-body[14];
        const float lf=dx*fx+dz*fz,ls=dx*sx+dz*sz;
        const bool fresh=!lastNs||now<lastNs||now-lastNs>500'000'000ull;
        const float dt=fresh?0.f:float(double(now-lastNs)*1e-9);
        float speed=0.f;
        if(!fresh&&dt>1e-4f){const float mx=body[12]-prevBody[0],mz=body[14]-prevBody[2];speed=std::sqrt(mx*mx+mz*mz)/dt;}
        prevBody[0]=body[12];prevBody[1]=body[13];prevBody[2]=body[14];
        if(fresh||!have){off[0]=lf;off[1]=ls;off[2]=dy;have=true;}
        else{
            if(dy>off[2])off[2]=dy;
            if(dy>off[2]-.04f){const float k=1.f-std::exp(-dt/1.f);off[0]+=(lf-off[0])*k;off[1]+=(ls-off[1])*k;}
        }
        constexpr float eyeUp=.08f,eyeFwd=.06f;
        const float ex=body[12]+fx*(off[0]+eyeFwd)+sx*off[1],ez=body[14]+fz*(off[0]+eyeFwd)+sz*off[1],ey=body[13]+off[2]+eyeUp;
        // (user: climbing a hill dropped the view to the saddle): a plain low-pass lagged a ramp
        // by speed * tau. Double exponential smoothing (level + trend, Holt): ramps are followed
        // without lag, the gait bounce (~2 Hz) stays damped; and the eye never sinks more than 10
        // cm below the head joint + eyeUp.
        static float vy{};
        if(fresh||tauMs<=0||!std::isfinite(y)||!std::isfinite(vy)){y=ey;vy=0.f;}
        else{const float tau=float(tauMs)*1e-3f;const float ka=1.f-std::exp(-dt/tau),kb=1.f-std::exp(-dt/(2.f*tau));
            const float predicted=y+vy*dt;const float level=predicted+(ey-predicted)*ka;
            vy+=((level-y)/std::max(dt,1e-4f)-vy)*kb;y=level;}
        const float floorY=head.v[1]+eyeUp-.10f;
        if(std::isfinite(floorY)&&y<floorY){y=floorY;if(vy<0.f)vy=0.f;}
        lastNs=now;
        if(std::isfinite(ex+ez+y)){a.position[0]=ex;a.position[2]=ez;a.position[1]=tauMs>0?y:ey;}
        if(!lastTraceNs||now-lastTraceNs>=100'000'000ull){lastTraceNs=now;
            RidingTrace t{};t.speed=speed;t.dy=dy;t.lf=lf;t.ls=ls;t.up=off[2];t.fwd=off[0];t.bodyY=body[13];t.eyeY=a.position[1];
            const auto n=ridingTraceNext.fetch_add(1,std::memory_order_relaxed);ridingTrace[n%ridingTrace.size()]=t;}
    }
    { // HORSE CAM UP / FWD: the eye moved up and forward along the rider's heading (forward = -c, horizontal)
        const auto st=::rdrvr::vr::settings();const float fx=-body[8],fz=-body[10],fl=std::sqrt(fx*fx+fz*fz);
        a.position[1]+=st.rideUpMeters();
        if(fl>1e-3f){a.position[0]+=fx/fl*st.rideFwdMeters();a.position[2]+=fz/fl*st.rideFwdMeters();}
    }
    ridingHeading=std::atan2(body[8],body[10]);ridingHeadingValid=std::isfinite(ridingHeading);
    ridingForwardHeading=std::atan2(-body[8],-body[10]); // the rider's forward (-c) in the headYawWorld convention atan2(f.x, f.z)
    a.up={0,1,0}; // Never the rider's tilting matrix (the horse gait pitched / rolled the view)
    BodyAnchorFacts f{};for(unsigned n=0;n<3;++n){f.capsule[n]=body[12+n];f.up[n]=a.up[n];f.head[n]=head.v[n];}
    f.headValid=true;f.attached=true;f.moverMatrix=matrix==mover+0x40;f.valid=true;
    facts=f;out=a;ridingSamples.fetch_add(1,std::memory_order_relaxed);return true;
}
// SUBTITLES (RE: hudManager::SetSubtitlesEnabled(hudManager*,bool) 0x29ed288 sets bit 2 of hudManager+0x2583c and excludes the
// "Splash_Subtitle" UI object; objectives / help / prompts untouched). Main thread: menu HUD > SUBTITLES OFF keeps it off; back to GAME
// re-enables once (the game's SyncOptionsToGame then applies its own option again).
std::uintptr_t subtitleHudGlobal{};void (*subtitleSetEnabled)(void*,bool){};
std::atomic<std::uint64_t> subtitleForcedOff{},subtitleRestored{};bool subtitleWeForced{};
void gameConfigureSubtitles(std::uintptr_t hudGlobal,void (*setEnabled)(void*,bool)) noexcept {subtitleHudGlobal=hudGlobal;subtitleSetEnabled=setEnabled;}
static void subtitlesApply() noexcept {
    if(!subtitleHudGlobal||!subtitleSetEnabled||!weaponObservationApi.read)return;
    std::uintptr_t hud{};std::uint8_t flags{};
    if(!weaponObservationApi.read(weaponObservationApi.context,subtitleHudGlobal,&hud,sizeof hud)||!hud||
       !weaponObservationApi.read(weaponObservationApi.context,hud+0x2583c,&flags,sizeof flags))return;
    const bool on=(flags>>2)&1u;
    if(::rdrvr::vr::settings().subtitlesOff()){if(on){subtitleSetEnabled(reinterpret_cast<void*>(hud),false);subtitleWeForced=true;subtitleForcedOff.fetch_add(1,std::memory_order_relaxed);}}
    else if(subtitleWeForced){subtitleWeForced=false;if(!on){subtitleSetEnabled(reinterpret_cast<void*>(hud),true);subtitleRestored.fetch_add(1,std::memory_order_relaxed);}}
}
std::string gameSubtitlesReport(){return "{\"forcedOff\":"+std::to_string(subtitleForcedOff.load())+",\"restored\":"+std::to_string(subtitleRestored.load())+"}";}
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
// The barrel-line point of the VR-held firearm for this player's reticle, on foot only (player+0x5d4 == 2, also right after a
// dismount). Shares vrWeaponAim's ray and freshness gates; the launch state is left alone.
static bool vrOnFootBarrelTarget(const LocalWeapon& owner,void* reticle,WeaponAimVector& point,std::uint64_t now) noexcept {
    if(!weaponOwner.onOwnerThread()||!worldPresentationAllowed()||!controllerAimFirearm(owner.type.kind))return false;
    WeaponContext context{};
    if(!WeaponContextReader(weaponObservationApi).inspect(owner,context)||!vw::gates(context,owner.type.kind,vw::config,now).aim)return false;
    std::uintptr_t currentReticle{};std::uint32_t mode{};const auto& api=weaponObservationApi;
    if(!api.read(api.context,owner.actor+0xb8,&currentReticle,sizeof currentReticle)||!currentReticle||
       currentReticle!=reinterpret_cast<std::uintptr_t>(reticle)||!api.read(api.context,owner.player+0x5d4,&mode,sizeof mode)||mode!=2)return false;
    const auto cached=vw::ownerSnapshot();
    if(cached.actor!=owner.actor||cached.item!=owner.item||cached.weapon!=owner.weapon||cached.actorGuid!=owner.actorGuid)return false;
    WeaponAimCall call{};call.weapon=reinterpret_cast<void*>(owner.weapon);
    return vrWeaponAim(call,point,now,false);
}
#endif
// RETICLE TARGET (VrWeapon.h): right after tgtReticleComponent::Update, the barrel-line point replaces the camera-ray hit at
// reticle +0x3810 / +0x3820, which aniAim::UpdateInternalStates copies to aniAim+0x390 for GunUpCheck.
static void vrReticleTarget(void* reticle,std::uint64_t now) noexcept {
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
    if(!vw::config.reticleTarget||!reticle||!weaponObservationReady.load(std::memory_order_acquire))return;
    try{
        LocalWeapon owner{};if(!weaponOwner.inspect(owner)||!owner.player)return;
        WeaponAimVector point{};
        if(!vrOnFootBarrelTarget(owner,reticle,point,now)){vw::reticleTargetSkipped.fetch_add(1,std::memory_order_relaxed);return;}
        const auto r=reinterpret_cast<std::uintptr_t>(reticle);float native[3]{};
        const auto l=vw::latestPlacement();const float upm=l.placement.unitsPerMetre>0.f?l.placement.unitsPerMetre:1.f;
        if(weaponObservationApi.read(weaponObservationApi.context,r+0x3810,native,sizeof native)&&l.weapon==owner.weapon){
            const float dx=native[0]-l.placement.origin[0],dz=native[2]-l.placement.origin[2],m=std::sqrt(dx*dx+dz*dz)/upm;
            if(std::isfinite(m)){vw::reticleNativeM.store(m,std::memory_order_relaxed);if(m<1.5f)vw::reticleTargetNear.fetch_add(1,std::memory_order_relaxed);}
        }
        const float v[3]{point.x,point.y,point.z};
        vw::processWrite(r+0x3810,v,sizeof v);vw::processWrite(r+0x3820,v,sizeof v);
        vw::reticleTargetSet.fetch_add(1,std::memory_order_relaxed);
    }catch(...){vw::reticleTargetSkipped.fetch_add(1,std::memory_order_relaxed);}
#else
    (void)reticle;(void)now;
#endif
}
// NATIVE RETICLE (VrWeapon.h): the mod's prevention bit is set on the player's sagPlayerStates while a VR-held firearm is in
// hand and cleared again (only on the same, still current states object) once it is not.
static void vrNativeReticle() noexcept {
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
    static std::uintptr_t appliedPlayer{},appliedStates{};
    if(!weaponObservationReady.load(std::memory_order_acquire))return;
    try{
        const auto& api=weaponObservationApi;constexpr auto bit=vw::reticlePreventionBit;
        LocalWeapon owner{};std::uintptr_t states{};bool want=false;
        if(vw::config.hideReticle&&::rdrvr::vr::holsters::holdHand.load(std::memory_order_acquire)>=0&&weaponOwner.inspect(owner)&&owner.player&&
           api.read(api.context,owner.player+0x410,&states,sizeof states)&&states&&states%8==0){
            const auto o=vw::ownerSnapshot();want=o.weapon&&o.weapon==owner.weapon&&vw::firearm(o.kind);
        }
        if(appliedStates&&(!want||states!=appliedStates)){
            std::uintptr_t current{};std::uint32_t mask{};
            if(api.read(api.context,appliedPlayer+0x410,&current,sizeof current)&&current==appliedStates&&
               api.read(api.context,appliedStates+0xc,&mask,sizeof mask)&&(mask&bit)){mask&=~bit;vw::processWrite(appliedStates+0xc,&mask,sizeof mask);}
            appliedPlayer=appliedStates=0;
        }
        if(want){
            std::uint32_t mask{};
            if(api.read(api.context,states+0xc,&mask,sizeof mask)){
                if(!(mask&bit)){mask|=bit;vw::processWrite(states+0xc,&mask,sizeof mask);}
                appliedPlayer=owner.player;appliedStates=states;vw::reticleHiddenFrames.fetch_add(1,std::memory_order_relaxed);
            }
        }else vw::reticleShownFrames.fetch_add(1,std::memory_order_relaxed);
    }catch(...){}
#endif
}
void gameObserveReticle(void* reticle,std::uint64_t timestamp,std::uint64_t thread) noexcept {
    const int saved=errno;struct Restore{int saved;~Restore(){errno=saved;}} restore{saved};
#ifdef RDRVR_NATIVE_MENUS
    // Camera transitions may finish/start after the input phase. Refresh on the
    // pinned post-camera boundary before publishing a head or querying weapons.
    if(nativeUi.update(timestamp)&&nativeUi.inputRearmRequired())nativePad.rearm();
#endif
    const auto ordinal=++reticleUpdates;
    try{subtitlesApply();}catch(...){}
    vrReticleTarget(reticle,timestamp);
    vrNativeReticle();

#ifdef RDRVR_HEAD_CAMERA
    try{
        diagnostics::WallScope timing(diagnostics::frameTimings.headSample);
        const auto state=presentation.snapshot(timestamp);HeadCameraAnchor anchor{};
        // CAMERA: body anchor + roomscale (vr/VrCamera.h). rig.apply runs EVERY call (it times anchor losses);
        // debug.rdrvr.vr.camera 0 -> the plain head anchor and the adapter's own latch.
        auto cameraSettings=::rdrvr::vr::camera::settingsFrom(::rdrvr::vr::settings());
        // Roomscale needs a consumer - LOCO's mover hook and LOCOMOTION FREE (else lean limit OFF + "NO LOCO").
        // TELEPORT: LOCOMOTION TELEPORT keeps LOCO's mover path (roomscale, the L3 escape walk): locoOwnsCapsule() = FREE or TELEPORT
        cameraSettings.locoReady=::rdrvr::vr::loco::moverInstalled.load(std::memory_order_acquire)&&::rdrvr::vr::settings().locoOwnsCapsule();
        ::rdrvr::vr::camera::Input cameraInput{};cameraInput.now=timestamp;cameraInput.settings=cameraSettings;
        cameraInput.sampled=state.world()&&bodyDrawReady.load(std::memory_order_acquire)&&reticleProbeReady.load(std::memory_order_acquire)&&weaponObservationReady.load(std::memory_order_acquire)&&
           HeadAnchorReader({weaponObservationApi,reticleProbeApi.head,reticleProbeApi.reticleTable,bodyAttachmentApi,
                             ::rdrvr::vr::camera::moverSimVtable.load(std::memory_order_acquire)})
               .sample(reinterpret_cast<std::uintptr_t>(reticle),timestamp,state.revision,anchor,&cameraInput.facts,
                       ::rdrvr::vr::camera::requireHead(cameraSettings));
        bool ridingEye=false;std::array<float,3> ridingEyePos{};
        if(!cameraInput.sampled&&state.world()&&bodyDrawReady.load(std::memory_order_acquire)&&reticleProbeReady.load(std::memory_order_acquire)&&
           weaponObservationReady.load(std::memory_order_acquire)&&ridingLever()&&
           ridingAnchorSample(reinterpret_cast<std::uintptr_t>(reticle),timestamp,state.revision,anchor,cameraInput.facts)){
            ridingEye=true;for(unsigned n=0;n<3;++n)ridingEyePos[n]=anchor.position[n];
            cameraInput.sampled=true;cameraInput.settings.mode=::rdrvr::vr::camera::Mode::Head; // RIDING: head bone on a horse
            // (user: the horse turns, the view does not): the rider's heading change is applied to the native camera every frame
            // (the same setter as snap turn; stick right = negative = heading atan2(c.x,c.z) decreasing). debug.rdrvr.vr.riding.turn 0 = off.
            static float lastHeading{};static std::uint64_t lastTurnNs{};static unsigned polls{};static bool turnLever{true};
            if((polls++&255u)==0){char v[PROP_VALUE_MAX]{};turnLever=!(__system_property_get("debug.rdrvr.vr.riding.turn",v)>0&&v[0]=='0');}
            if(ridingHeadingValid){
#if defined(RDRVR_NATIVE_MENUS)
                // (user: after mounting the view had to be recentred): on the first riding sample after >= 0.5 s on foot the view
                // is turned once so the current head yaw faces the horse (headYawWorld += d for NativeCameraTurn(d)).
                if(turnLever&&(!lastTurnNs||timestamp<lastTurnNs||timestamp-lastTurnNs>500'000'000ull)){float hy{};
                    if(::rdrvr::vr::body::headYaw(hy,timestamp)){const float d=std::remainder(ridingForwardHeading-hy,2.f*3.14159265f);
                        if(std::isfinite(d)&&ridingViewTurn(d)==1){ridingRecenters.fetch_add(1,std::memory_order_relaxed);ridingRecenterDeg.store(d*57.29578f,std::memory_order_relaxed);}}}
#endif
                if(lastTurnNs&&timestamp>lastTurnNs&&timestamp-lastTurnNs<300'000'000ull&&turnLever){
                    const float d=std::remainder(ridingHeading-lastHeading,2.f*3.14159265f);
#if defined(RDRVR_NATIVE_MENUS)
                    if(std::fabs(d)>1e-4f&&std::fabs(d)<.8f){const int r=ridingViewTurn(d);
                        if(r==1){ridingTurns.fetch_add(1,std::memory_order_relaxed);ridingTurnedDeg.store(ridingTurnedDeg.load(std::memory_order_relaxed)+d*57.29578f,std::memory_order_relaxed);}
                        else ridingTurnDeclined.fetch_add(1,std::memory_order_relaxed);}
#endif
                }
                lastHeading=ridingHeading;lastTurnNs=timestamp;
            }
        }
        cameraInput.viewsValid=gameViews.snapshot(timestamp,cameraInput.views);
        cameraInput.psiValid=visibilityBridge.worldYaw(timestamp,cameraInput.psi,cameraInput.psiOrigin)&&
            !(cameraInput.psiOrigin&::rdrvr::vr::cutscene::originSerialBit); // A cutscene plan's yaw is never the gameplay psi
        cameraInput.recordValid=::rdrvr::vr::body::latestRecord(cameraInput.record);
        // (user: HORSE CAM UP / FWD did nothing): the rig's HEAD mode copies the raw head bone (facts.head) into the anchor, which threw
        // away the riding eye (anti-bob low-pass + the calibration offsets); on a horse the riding eye wins.
        const bool rigOk=::rdrvr::vr::camera::rig.apply(cameraInput,anchor);
        if(rigOk&&ridingEye){for(unsigned n=0;n<3;++n)anchor.position[n]=ridingEyePos[n];anchor.up={0,1,0};}
        if(rigOk&&headCamera.publish(anchor)){
            vrWeaponTag(anchor,cameraInput.facts,timestamp); // Anchor ring, mover facts, frame tag, laser probe, hands publish
            ++headCameraSamples;headCameraState="LOCAL_HEAD_PUBLISHED";
        }else{headCamera.invalidate();++headCameraRejected;headCameraState=state.world()?"LOCAL_HEAD_UNAVAILABLE":"THEATER";}
    }catch(...){headCamera.invalidate();++headCameraRejected;headCameraState="HEAD_QUERY_EXCEPTION";}
#ifdef RDRVR_VR_CUTSCENE
    vrCutsceneObserve(presentation.snapshot(timestamp),timestamp); // John (FIRST PERSON), VrBody cutscene gates, row note
#endif
#endif
    // Keep association continuous at the post-camera cadence. Expensive native
    // physics/attachment diagnostics below remain throttled independently.
    bool targetContextAllowed=false;std::uint32_t currentTargetActor{};NativeReticleFrame reticleVisibility{};
    try{
        LocalWeapon owner{};WeaponContext context{};
        bool allowed=weaponObservationReady.load(std::memory_order_acquire);
#ifdef RDRVR_NATIVE_MENUS
        allowed=allowed&&worldPresentationAllowed();
#endif
        if(allowed&&weaponOwner.inspect(owner)){
            ++localWeaponSamples;localWeaponGuid=owner.actorGuid;localWeaponClass=owner.type.kind;localWeaponTypeId=owner.type.id;
            if(WeaponContextReader(weaponObservationApi).inspect(owner,context)){
                targetContextAllowed=!context.physicalAimBlocked()&&controllerAimFirearm(owner.type.kind);currentTargetActor=owner.actorGuid;
#if defined(RDRVR_HEAD_CAMERA) && defined(RDRVR_NATIVE_MENUS)
                const auto visibilityView=presentation.snapshot(timestamp);
                if(nativeReticleReady.load(std::memory_order_acquire)&&weaponOwner.onOwnerThread()&&
                   visibilityView.world()&&targetContextAllowed&&
                   NativeReticleVisibilityReader(weaponObservationApi).inspect(owner,reticleVisibility.state)){
                    reticleVisibility.stamp=timestamp;reticleVisibility.revision=visibilityView.revision;
                }
#endif
                weaponContextSampleNs=timestamp;weaponContextFlags=context.flags();weaponLinkedGuid=context.linkedActorGuid;weaponGringoGuid=context.gringoGuid;
                weaponContextState=context.knownLock()?"NATIVE_CONTEXT_LOCK_OBSERVED":
                    context.physicalAimBlocked()?"NATIVE_AIM_OVERRIDE_OBSERVED":"NO_KNOWN_LOCKS_OBSERVED";
                if(context.knownLock())weaponCallTrace.invalidate();else weaponCallTrace.bind(owner,timestamp);
                vrWeaponObserve(&owner,&context,timestamp); // Owner, gates and geometry of the VR-held weapon
            }else{clearWeaponContext("CONTEXT_UNAVAILABLE");vrWeaponObserve(nullptr,nullptr,timestamp,WeaponObserveGap::NoContext);}
        }else{
            localWeaponClass=WeaponClass::Unknown;localWeaponTypeId=0;
            clearWeaponContext(allowed?"WEAPON_UNAVAILABLE":"THEATER");
            vrWeaponObserve(nullptr,nullptr,timestamp,allowed?WeaponObserveGap::None:WeaponObserveGap::NoWorld);
        }
        weaponState=weaponOwner.state();
    }catch(...){weaponCallTrace.invalidate();}
    {std::lock_guard lock(targetAimMutex);if(!targetContextAllowed||currentTargetActor!=targetAimGuid)targetAimStamp=0;
        nativeReticleFrame=reticleVisibility;
        if(reticleVisibility.stamp){++nativeReticleSamples;if(!reticleVisibility.state.visible)++nativeReticleHidden;}
        else ++nativeReticleDeclined;
    }
    weaponCallTrace.record(reinterpret_cast<std::uintptr_t>(reticle),{WeaponCallPhase::Reticle,timestamp,thread,ordinal});
    if(ordinal%30!=1)return;
    reticleProbeSampleNs=0;reticleProbeBlocked=false;reticleHeadValid=false;reticleProbeDistance=0;reticleHeadGap=0;
    reticleMuzzleProbeValid=false;reticleMuzzleBlocked=false;reticleMuzzleDistance=0;reticleMuzzleToTarget=0;
    reticleAttachmentValid=false;reticleAttachmentScale=0;
    reticleOriginClamped=false;reticleRequestedReach=0;reticleSafeReach=0;
    {std::lock_guard lock(gripPreviewMutex);gripPreview={};}
    try{
        if(!reticleProbeReady.load(std::memory_order_acquire)||!weaponObservationReady.load(std::memory_order_acquire))return;
#ifdef RDRVR_NATIVE_MENUS
        if(!worldPresentationAllowed()){reticleProbeState="THEATER";return;}
#endif
        const auto now=timestamp;
        runtime::ControllerSample input{};ControllerRay ray{};
        StereoCameraPlan mapping{};
        if(!controllers.snapshot(now,input)||!holsterWeaponSample(input)||!controllerAimSpace.plan(mapping)||!mapControllerRay(mapping,input,1,now,ray)){reticleProbeState="NO_CURRENT_WORLD_RAY";return;}
        ControllerWeaponFrame frame{};const bool gripValid=mapControllerWeaponFrame(mapping,input,1,now,frame);
        LocalWeapon owner{};WeaponContext context{};
        if(!weaponOwner.inspect(owner)||!WeaponContextReader(weaponObservationApi).inspect(owner,context)){reticleProbeState="OWNER_OR_CONTEXT_UNAVAILABLE";return;}
        if(context.knownLock()){reticleProbeState="NATIVE_CONTEXT_LOCK";return;}
        observeWeaponSkeleton(owner,2,timestamp);
        weaponCallTrace.bind(owner,now);
        ReticleProbeSample sample{};
        if(!ReticleProbeReader(reticleProbeApi).sample(reinterpret_cast<std::uintptr_t>(reticle),owner,ray,sample,gripValid?&frame:nullptr)){reticleProbeState="NATIVE_PROBE_UNAVAILABLE";return;}
        LocalWeapon after{};
        if(!weaponOwner.inspect(after)||after.actorGuid!=owner.actorGuid||after.actor!=owner.actor||after.player!=owner.player||after.weapon!=owner.weapon||after.item!=owner.item||after.inventory!=owner.inventory||
           after.type.id!=owner.type.id||after.type.kind!=owner.type.kind||
           !WeaponContextReader(weaponObservationApi).inspect(after,context)||context.knownLock()){reticleProbeState="OWNER_OR_CONTEXT_CHANGED";return;}
        if(sample.trackedWeaponPoseValid){
            GripWeaponPreview preview{};const auto& planned=sample.trackedWeaponPose;const auto& plannedRay=planned.pose.muzzleRay;
            preview.stamp=now;preview.scene=plannedRay.sceneId;preview.sequence=plannedRay.controllerSequence;preview.tracking=plannedRay.trackingEpoch;preview.reference=plannedRay.referenceSpaceEpoch;
            preview.actorGuid=owner.actorGuid;preview.type=owner.type.id;preview.poseValid=true;preview.probeValid=sample.trackedWeaponProbeValid;
            preview.clamped=planned.clamped;preview.blocked=sample.trackedWeaponHit.blocked;preview.distance=sample.trackedWeaponHit.distance;
            for(unsigned i=0;i<3;++i){preview.grip[i]=planned.pose.attachmentWorld[12+i];preview.target[i]=sample.trackedWeaponHit.point.v[i];}
            preview.muzzle=plannedRay.origin;preview.direction=plannedRay.direction;
            std::lock_guard lock(gripPreviewMutex);gripPreview=preview;
        }
        ++reticleProbeSamples;reticleProbeSampleNs=now;reticleProbeBlocked=sample.hit.blocked;reticleProbeDistance=sample.hit.distance;
        reticleHeadValid=sample.headValid;reticleHeadGap=sample.headToRayOrigin;
        reticleMuzzleProbeValid=sample.muzzleProbeValid;reticleMuzzleBlocked=sample.muzzleHit.blocked;
        reticleMuzzleDistance=sample.muzzleHit.distance;reticleMuzzleToTarget=sample.muzzleToTarget;
        reticleAttachmentValid=sample.attachmentValid;reticleAttachmentScale=sample.attachment.scale;
        reticleOriginClamped=sample.trackedOriginClamped;reticleRequestedReach=sample.requestedReach;reticleSafeReach=sample.safeReach;
        reticleProbeState=sample.hit.blocked?"NATIVE_BULLET_STOPPER_OBSERVED":"NATIVE_CLEAR_RAY_OBSERVED";
    }catch(...){reticleProbeState="NATIVE_PROBE_EXCEPTION";}
}
void gameAfterUpdateInput(void* system,std::uint64_t now) noexcept {nativePad.afterUpdate(system,now);}
void gameUpdateInput(void* system,std::uint64_t now) noexcept {
    runtime::nativeThreadIds[0].store(static_cast<std::uint32_t>(gettid()),std::memory_order_relaxed);
#ifdef RDRVR_NATIVE_MENUS
    if(nativeUi.update(now)&&nativeUi.inputRearmRequired())nativePad.rearm();
#endif
    nativePad.beforeUpdate(system,now);
    const int aimErrno=errno;struct AimErrno {int saved;~AimErrno(){errno=saved;}} preserveAimErrno{aimErrno};
    try{
    runtime::ControllerSample sample{};ControllerRay ray{};
#ifdef RDRVR_NATIVE_MENUS
    if(!worldPresentationAllowed()){controllerAimSpace.invalidate();aimState="THEATER";clearWeaponContext("THEATER");return;}
#endif
    if(controllers.snapshot(now,sample)&&controllerAimSpace.ray(sample,1,now,ray)){
        const auto mapped=++aimMapped;aimState="CONTROLLER_WORLD_RAY_OBSERVED";
        if(mapped%30==1&&weaponObservationReady.load(std::memory_order_acquire)){
            LocalWeapon observed{};
            if(weaponOwner.inspect(observed)){++localWeaponSamples;localWeaponGuid=observed.actorGuid;localWeaponClass=observed.type.kind;localWeaponTypeId=observed.type.id;
                WeaponContext context{};
                if(WeaponContextReader(weaponObservationApi).inspect(observed,context)){
                    weaponContextSampleNs=now;weaponContextFlags=context.flags();weaponLinkedGuid=context.linkedActorGuid;weaponGringoGuid=context.gringoGuid;
                    weaponContextState=context.knownLock()?"NATIVE_CONTEXT_LOCK_OBSERVED":
                        context.physicalAimBlocked()?"NATIVE_AIM_OVERRIDE_OBSERVED":"NO_KNOWN_LOCKS_OBSERVED";
                    if(context.knownLock())weaponCallTrace.invalidate();else weaponCallTrace.bind(observed,now);
                }else{clearWeaponContext("CONTEXT_UNAVAILABLE");}
            }
            else{localWeaponClass=WeaponClass::Unknown;localWeaponTypeId=0;clearWeaponContext("WEAPON_UNAVAILABLE");}
            weaponState=weaponOwner.state();
        }
    }else{++aimUnavailable;aimState="WAITING_FOR_MATCHING_WORLD_AND_HAND";clearWeaponContext("NO_CURRENT_WORLD_RAY");}
    }catch(...){++aimUnavailable;aimState="AIM_OBSERVER_EXCEPTION";clearWeaponContext("OBSERVER_EXCEPTION");}
}
#ifdef RDRVR_NATIVE_MENUS
#ifdef RDRVR_HEAD_CAMERA
namespace {
bool mapHorizontalLook(void*,const runtime::ControllerSample& input,std::uint64_t now,float& x){
    // LASSO: a lasso minigame owns the right stick (rotate / wiggle prompts) - never a snap turn
    if(::rdrvr::vr::lasso::minigameFocus()){snapTurnLatch.sample(false,{},x,now);snapTurnState="LASSO_MINIGAME";return false;}
    const auto view=presentation.snapshot(now);LocomotionContext context{};StereoCameraPlan plan{};
    bool eligible=false;SnapTurnIdentity id{};
    if(view.world()){
        if(!LocomotionContextReader(locomotionApi).sample(context)){
            snapTurnLatch.sample(false,{},x,now);snapTurnState="WAITING_FOR_PLAYER_CONTEXT";x=0;return true;
        }
        if(context.ordinaryFoot){
            // Require a current rendered camera and tracking identity before
            // calling the engine. Never apply a queued turn after stale data.
            std::array<float,2> unused{};
            eligible=!(input.buttons&runtime::PadMenu)&&controllerAimSpace.plan(plan)&&plan.actorGuid==context.actorGuid&&
                headCamera.matches(plan,now,view.revision)&&headRelativeStick(plan,input,now,unused);
            id={input.inputEpoch?input.inputEpoch:input.epoch,input.continuity,input.referenceSpace,view.revision,context.actorGuid};
        }
    }
    // TURN SMOOTH (user): stick-proportional turn every frame (dead zone.15, debug.rdrvr.vr.turn.speed deg/s, default 150) through the
    // same native setter; like the riding turn only the camera + weapon follow.
    if(::rdrvr::vr::settings().smoothTurn()){
        static std::uint64_t lastNs{};static unsigned polls{};static float rate{150.f};
        if((polls++&255u)==0){char v[PROP_VALUE_MAX]{};rate=__system_property_get("debug.rdrvr.vr.turn.speed",v)>0?std::clamp(float(std::atof(v)),30.f,360.f):150.f;}
        const float dt=lastNs&&now>lastNs?std::min(float(double(now-lastNs)*1e-9),.05f):0.f;lastNs=now;
        snapTurnLatch.sample(false,{},x,now);
        if(!view.world()||!context.ordinaryFoot){snapTurnState="NATIVE_CONTEXT";return false;}
        const float stickX=x,ax=std::fabs(x);const float mag=ax>.15f?(ax-.15f)/.85f:0.f;x=0;
        if(!eligible){snapTurnState="WAITING_FOR_CURRENT_CAMERA";return true;}
        if(mag<=0.f||dt<=0.f){snapTurnState="SMOOTH_READY";return true;}
        const float delta=-std::copysign(mag*rate*3.14159265f/180.f*dt,stickX);
        std::array<float,3> applied{};
        if(!NativeCameraTurn(snapTurnApi).apply(delta,applied)){++snapTurnDeclined;snapTurnState="CAMERA_CHANGED_OR_UNAVAILABLE";return true;}
        ++smoothTurnFrames;snapTurnState="SMOOTH_APPLIED";
        ::rdrvr::vr::tploco::warp::noteUserTurn();::rdrvr::vr::weapon::noteTurn(delta,diagnostics::monotonicNs());
        return true;
    }
    const float delta=snapTurnLatch.sample(eligible,id,x,now);
    if(!view.world()||!context.ordinaryFoot){snapTurnState="NATIVE_CONTEXT";return false;}
    // Consume only horizontal look while ordinary on-foot VR owns turning.
    // Vertical look, menu/optic/horse controls and every button stay native.
    x=0;
    if(!eligible){snapTurnState="WAITING_FOR_CURRENT_CAMERA";return true;}
    if(!delta){snapTurnState="READY_OR_WAITING_FOR_STICK_CENTER";return true;}
    std::array<float,3> applied{};
    if(!NativeCameraTurn(snapTurnApi).apply(delta,applied)){
        ++snapTurnDeclined;snapTurnState="CAMERA_CHANGED_OR_UNAVAILABLE";return true;
    }
    ++snapTurnApplied;snapTurnState="NATIVE_SNAP_APPLIED";
    ::rdrvr::vr::tploco::warp::noteUserTurn(); // A player turn during a teleport HOLD is not psi drift (no correction)
    // Controller aim resumes only from a head sample made after the native
    // mutation, including when an earlier render finishes concurrently.
    controllerAimSpace.afterNativeTurn(diagnostics::monotonicNs());
    ::rdrvr::vr::weapon::noteTurn(delta,diagnostics::monotonicNs()); // VR-held weapon follows the turn while no plan exists
    controllerAttachment.reset();animatedAttachment.reset();scopedModelPose.reset();
    {std::lock_guard lock(targetAimMutex);targetAimStamp=0;}
    return true;
}
// Teleport view turn without a controller sample. Called by teleport::gameFrame from the sagShellDerived::UpdateInputs
// wrapper: the SAME main thread and frame slot as mapHorizontalLook (NativePadDriver runs inside d11::InputSystem_c::Update,
// called by D11RageCore_c::Update @0x23a032c just before UpdateInputs @0x23a033c in sagShellDerived::Execute).
// Same gates as snap turn minus the stick/controller: world presentation, ordinary on-foot context, a current published plan
// for the local actor whose head sample matches; same native setter and the same post-turn resets.
// Returns 1 applied (turnNs = the afterNativeTurn stamp), 0 not eligible now (retry), -1 setter declined.
int teleportViewTurn(float radians,std::uint64_t& turnNs) noexcept {
    try{
        const auto now=diagnostics::monotonicNs();
        const auto view=presentation.snapshot(now);if(!view.world())return 0;
        LocomotionContext context{};if(!LocomotionContextReader(locomotionApi).sample(context)||!context.ordinaryFoot)return 0;
        StereoCameraPlan plan{};
        if(!controllerAimSpace.plan(plan)||plan.actorGuid!=context.actorGuid||!headCamera.matches(plan,now,view.revision))return 0;
        std::array<float,3> applied{};
        if(!NativeCameraTurn(snapTurnApi).apply(radians,applied)){++snapTurnDeclined;snapTurnState="CAMERA_CHANGED_OR_UNAVAILABLE";return -1;}
        ++snapTurnApplied;snapTurnState="TELEPORT_VIEW_APPLIED";
        turnNs=diagnostics::monotonicNs();
        controllerAimSpace.afterNativeTurn(turnNs);
        ::rdrvr::vr::weapon::noteTurn(radians,turnNs);
        controllerAttachment.reset();animatedAttachment.reset();scopedModelPose.reset();
        {std::lock_guard lock(targetAimMutex);targetAimStamp=0;}
        return 1;
    }catch(...){return -1;}
}
// RIDING: the horse's turn applied to the native camera (main thread, PostCameraUpdate; same setter and resets as teleportViewTurn
// no on-foot gate). 1 applied, -1 declined.
int ridingViewTurn(float radians) noexcept {
    try{
        std::array<float,3> applied{};
        if(!snapTurnReady.load(std::memory_order_acquire)||!NativeCameraTurn(snapTurnApi).apply(radians,applied))return -1;
        // ONLY the camera. The snap-turn resets (controller / animated attachment, scoped model pose, aim stamp) run every frame
        // here and leaked engine images.
        ::rdrvr::vr::weapon::noteTurn(radians,diagnostics::monotonicNs());
        return 1;
    }catch(...){return -1;}
}
bool mapMovement(void*,const runtime::ControllerSample& input,std::uint64_t now,std::array<float,2>& axes){
    auto mode=MovementMapping::Native;std::array<float,2> mapped{};
    namespace tl=rdrvr::vr::tploco;
    const auto vs=rdrvr::vr::settings();
    const auto view=presentation.snapshot(now);
    const auto presented=stereoPresentedNs.load();
    // Theater uses the native camera, including before the first stereoMailbox pair.
    const bool useHead=movementUsesHeadCamera(view.world(),presented,now);
    LocomotionContext context{};const bool contextRead=useHead&&LocomotionContextReader(locomotionApi).sample(context);
    // TELEPORT: who owns the movement stick this pad update. Owned -> engine axes 0 and
    // LOCO stick 0 (controls / holsters see it neutral); FreeWalk (L3 escape walk, long BLOCKED) -> the FREE path x .walk; Native (lasso
    // minigame focus) -> native stick, LOCO transparent; Free -> the FREE path (module OFF: UNAVAILABLE - FREE / NO ARC, not on foot, lasso).
    const bool teleport=vs.teleportLocomotion();
    auto route=tl::StickRoute::Free;
    if(teleport){
        const auto menuNs=vr::holsters::menuSeenNs.load(std::memory_order_relaxed);
        const bool menu=menuNs&&(now<menuNs||now-menuNs<vr::holsters::menuFreezeNs);
        route=tl::publishStick(input.sticks[0],input.sticks[1],(input.buttons&runtime::PadLeftStick)!=0,contextRead&&context.ordinaryFoot,view.world(),menu,
            contextRead?context.actorGuid:0u,true,now);
    }else tl::clearStick();
    // LASSO: a lasso minigame -> native sticks (stick prompts), LOCO / TELEPORT overrides suspended
    const bool lassoNative=::rdrvr::vr::lasso::minigameFocus();
    const bool owned=route==tl::StickRoute::Owned&&!lassoNative,walk=route==tl::StickRoute::FreeWalk&&!lassoNative,native=route==tl::StickRoute::Native||lassoNative;
    const float walkScale=walk?std::clamp(tl::walkScale.load(std::memory_order_relaxed),.2f,1.f):1.f;
    { // LOCO config from the VR menu, the stick for v_vr (same sample as the head-relative mapping), menu status.
        rdrvr::vr::loco::setConfig({teleport?!native:(vs.freeLocomotion()&&!lassoNative),vs.moveSpeedScale()*walkScale,vs.suppressBumps(),vs.suppressStumble(),teleport});
        rdrvr::vr::loco::onStick(owned||native?0.f:input.sticks[0],owned||native?0.f:input.sticks[1]);
        using rdrvr::vr::status::publish;using rdrvr::vr::Feature;namespace st=rdrvr::vr::status;
        publish(Feature::Loco,rdrvr::vr::loco::moverInstalled.load()?st::Ready:st::Unavailable,rdrvr::vr::loco::stateNote());
        publish(Feature::Bump,rdrvr::vr::loco::bumpInstalled.load()?st::Ready:st::Unavailable);
        publish(Feature::Stumble,rdrvr::vr::loco::nmInstalled.load()?st::Ready:st::Unavailable);
    }
    if(useHead&&!native){
        if(!contextRead)mode=MovementMapping::Unavailable;
        else if(context.ordinaryFoot){
            StereoCameraPlan plan{};
            if(controllerAimSpace.movementPlan(plan)&&plan.actorGuid==context.actorGuid&&headCamera.matches(plan,now,view.revision)&&
               headRelativeStick(plan,input,now,mapped))mode=MovementMapping::Head;
            else mode=MovementMapping::Unavailable;
        }
    }
    if(owned)mapped={0.f,0.f};else if(walk){mapped[0]*=walkScale;mapped[1]*=walkScale;}
    if(mode==MovementMapping::Head){++locomotionMapped;locomotionState="HEAD_RELATIVE_FOOT";}
    else if(mode==MovementMapping::Unavailable){++locomotionUnavailable;locomotionState="WAITING_FOR_CURRENT_MOVEMENT_FRAME";}
    else{++locomotionNative;locomotionState="NATIVE_CONTEXT";}
    axes=locomotionLatch.sample(mode,axes,mapped);return true;
}
}
#endif
void gameConfigureUi(NativeUiApi api) noexcept {nativeUi.configure(api);}
bool worldPresentationAllowed() {
    const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return presentation.snapshot(now).world();
}
#ifdef RDRVR_HEAD_CAMERA
bool gameNativeFramePacingAllowed() noexcept {
    const int saved=errno;bool allowed=false;
    try {
        runtime::TrackedViews tracked{};const auto now=diagnostics::monotonicNs();
        allowed=sessionRunning.load()&&!stopWorker.load()&&!primaryRestoreFault.load(std::memory_order_acquire)&&
            worldRecorded.load(std::memory_order_relaxed)>0&&worldQuarantined.load(std::memory_order_relaxed)==0&&
#ifdef RDRVR_VR_CUTSCENE
            stereoPresentation(presentation.snapshot(now),now)&& // + an engaged stereo cutscene
#else
            presentation.snapshot(now).world()&&
#endif
            gameViews.snapshot(now,tracked);
    }catch(...){}
    errno=saved;return allowed;
}
bool gameWorldPresentOnly(VkQueue) noexcept {
    const int saved=errno;bool allowed=false;
    try {
        runtime::TrackedViews tracked{};const auto now=diagnostics::monotonicNs();
        allowed=gameNativeFramePacingAllowed()&&gameViews.snapshot(now,tracked)&&
#ifdef RDRVR_NATIVE_HUD
            !hudProducerActive.load(std::memory_order_acquire)&&
#endif
            !hudCapture.requiresPresent()&&
            stereoMailbox.freshCompatible(tracked.epochs.tracking,tracked.epochs.referenceSpace,now);
    }catch(...){}
    errno=saved;return allowed;
}
bool gameWorldPresentBypass(VkQueue queue) noexcept {
    const int saved=errno;bool allowed=false;bool theaterShown=true; // Presentation not WORLD -> the theater is on screen
    try {
        runtime::TrackedViews tracked{};const auto now=diagnostics::monotonicNs();
#ifdef RDRVR_VR_CUTSCENE
        theaterShown=!stereoPresentation(presentation.snapshot(now),now);
#else
        theaterShown=!presentation.snapshot(now).world();
#endif
        allowed=gameNativeFramePacingAllowed()&&gameViews.snapshot(now,tracked)&&
#ifdef RDRVR_NATIVE_HUD
            !hudProducerActive.load(std::memory_order_acquire)&&
#endif
            !hudCapture.requiresPresent()&&
            stereoMailbox.freshCompatible(tracked.epochs.tracking,tracked.epochs.referenceSpace,now)&&
            passCapture.canBypassPresent(queue,tracked.epochs.tracking,tracked.epochs.referenceSpace,now);
    }catch(...){}
    ::rdrvr::engine::postfx::notePresent(allowed,theaterShown); // Stale 2D backbuffer handed to the theater after a skipped scene
    errno=saved;return allowed;
}
bool gameEyeResolutionCoordinatesReady() noexcept {
    return !nativeFramebufferExtent.faulted()&&viewportApiReady.load(std::memory_order_acquire)&&primaryHooksReady.load(std::memory_order_acquire)&&
        framebufferApiReady.load(std::memory_order_acquire)&&engineTargetApiReady.load(std::memory_order_acquire)&&
        textureHooksReady.load(std::memory_order_acquire)&&targetHooksReady.load(std::memory_order_acquire)&&
        globalApiReady.load(std::memory_order_acquire)&&retailResourcesReady.load(std::memory_order_acquire)&&
        retailDrawReady.load(std::memory_order_acquire)&&replayHooksReady.load(std::memory_order_acquire)&&
        worldCullingEnabled.load(std::memory_order_acquire)&&!primaryRestoreFault.load(std::memory_order_acquire);
}
bool gameEyeResolutionWorldActive() noexcept {
    const int saved=errno;bool world=false;
#ifdef RDRVR_VR_CUTSCENE
    try {world=stereoPresentationAllowed();}catch(...){} // Eye resolution also for stereo cutscenes
#else
    try {world=worldPresentationAllowed();}catch(...){}
#endif
    errno=saved;return world;
}
#endif
bool gameCaptureHud(VkQueue queue,const runtime::CompletedQuadImage& image,std::uint64_t nowNs) noexcept {
    const int saved=errno;bool result=false;
    try{if(sessionRunning.load()&&!stopWorker.load()&&worldPresentationAllowed())result=hudQuad.captureCompleted(queue,image,nowNs);}catch(...){}
    errno=saved;return result;
}
#endif
#endif
void gameConfigureGlobals(EngineGlobalApi api) noexcept {globalApi=api;globalApiReady.store(bool(api),std::memory_order_release);}
void gameConfigureReplayPhase(ReplayRenderPhaseApi api) noexcept {replayPhaseApi=api;replayPhaseReady.store(bool(api),std::memory_order_release);}
ReplayGrassVisibility& gameGrassVisibility() noexcept {return grassVisibility;}
std::unique_ptr<EngineGlobalState> gameCreateGlobalState() noexcept {
    if(!globalApiReady.load(std::memory_order_acquire)||!sessionRunning.load()||stopWorker.load())return {};
    try{if(globalApi.ownsThread())return std::make_unique<EngineGlobalState>(globalApi);}catch(...){}
    return {};
}
void gameConfigureRetailResources(RetailResourceApi api) noexcept {retailResourceApi=api;
#ifdef RDRVR_HEAD_CAMERA
    nativePrimaryRaster.configure({framebufferApi,api});
#endif
    retailResourcesReady.store(bool(api),std::memory_order_release);
}
void gameConfigureHudRender(HudRenderEntry api) noexcept {hudRenderEntry=api;hudRenderReady.store(bool(api),std::memory_order_release);}
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_NATIVE_MENUS)
// HUD Begin (grcWorldIdentity return 0x2a587e4, RenderFrame has just locked [grPostFX+0x478] = the stock LEFT eye target,
// whose content was already copied into the stereo pair): clear it to transparent with the engine's own grcDevice::Clear +
// VkContext::FlushFrameBuffer, so every native HUD draw of this interval (injuries, hudManager, minigame, script text, radar, Flash,
// UIManager, PPP, D11 UI, TouchUI) lands on (0,0,0,0). Only in RenderScenes that recorded a stereo pair while WORLD is presented.
// The pending factory binding is consumed first exactly like EngineHudProducer::begin.
HudResourceApi vrHudResourceApi() {
    const auto& fb=framebufferApi;
    HudResourceApi api{fb.memory,fb.ownsThread,retailResourceApi.currentGfx,fb.gfxVtable,fb.framebufferVtable,fb.colorVtable,fb.factoryDirty,fb.depthVtable};
    api.externalImage=describeHudPresentImage;return api;
}
// hudStreamingMap::DrawMap does grcViewport::SetCurrent(sm_ScreenViewport) + ResetWindow = window (0,0,w,h) of the screen
// viewport (+0x330/+0x334 = 1374x720, the stock swapchain) inside the 1680x1760 eye target, so the minimap tiles + blips land in the
// top-left 1374x720 and the tiles fail the circle stencil (no terrain). The content maps exactly onto the
// grey ring when that window is the whole target. During a VR HUD interval the screen viewport's w/h = the target's, restored at End /
// cancel. debug.rdrvr.vr.hud.screenvp 0 = off (then debug.rdrvr.vr.hud.mapfix can move the blips instead).
std::uintptr_t vrHudScreenViewportGlobal{};
void gameConfigureHudScreenViewport(std::uintptr_t global) noexcept {vrHudScreenViewportGlobal=global;}
struct ScreenViewportPatch {std::uintptr_t vp{};std::int32_t saved[2]{};bool active{};std::atomic<std::uint64_t> applied{},restored{},skipped{};std::atomic<int> lastW{-1},lastH{-1};};
ScreenViewportPatch screenVp;
static void screenViewportRestore() noexcept {
    if(!screenVp.active)return;
    std::memcpy(reinterpret_cast<void*>(screenVp.vp+0x330),screenVp.saved,sizeof screenVp.saved);screenVp.active=false;screenVp.restored.fetch_add(1,std::memory_order_relaxed);
}
static void screenViewportApply(std::uint32_t w,std::uint32_t h) noexcept {
    screenViewportRestore();
    static unsigned calls=0;static bool lever=true;
    if((calls++&255u)==0){char v[PROP_VALUE_MAX]{};lever=__system_property_get("debug.rdrvr.vr.hud.screenvp",v)>0&&v[0]=='1';} // Default OFF
    const auto& m=framebufferApi.memory;std::uintptr_t vp{};std::int32_t wh[2]{};
    if(!lever||!vrHudScreenViewportGlobal||!m.read||!m.read(m.context,vrHudScreenViewportGlobal,&vp,sizeof vp)||!vp||vp>UINTPTR_MAX-0x400||
       !m.read(m.context,vp+0x330,wh,sizeof wh)||wh[0]<64||wh[1]<64||wh[0]>8192||wh[1]>8192){screenVp.skipped.fetch_add(1,std::memory_order_relaxed);return;}
    screenVp.lastW.store(wh[0],std::memory_order_relaxed);screenVp.lastH.store(wh[1],std::memory_order_relaxed);
    if(std::uint32_t(wh[0])==w&&std::uint32_t(wh[1])==h)return;
    screenVp.vp=vp;screenVp.saved[0]=wh[0];screenVp.saved[1]=wh[1];
    const std::int32_t nw[2]{std::int32_t(w),std::int32_t(h)};std::memcpy(reinterpret_cast<void*>(vp+0x330),nw,sizeof nw);
    screenVp.active=true;screenVp.applied.fetch_add(1,std::memory_order_relaxed);
}
// WATER: WorldDrawProfile::Eye keeps only buckets 0..3,5 (mask & 0x2f), which drops water (bucket 4:
// PreWater refraction copy + Depth Resolve lock + water draws, all inside DrawColorPass before the eye
// copy) from both eyes. The LEFT colour stage runs unfiltered when this frame's resource snapshot also
// covers the water targets (inspect includeWater -> WaterDepth / Refraction right-eye twins for the
// replay); otherwise that frame keeps the filter. debug.rdrvr.vr.water 0 = always filtered. w01 WATER: eyeWaterFrame was set by the consumer AFTER the left colour stage had drawn, so
// the left colour always ran filtered (mask & 0x2f) and the lake had no water in either eye. Now
// the colour stage keeps bucket 4 when the previous consumer paired the water targets
// (eyeWaterReady); the consumer of the same frame must pair them again, else the right eye is
// skipped (WATER_TARGETS_UNPAIRED). debug.rdrvr.vr.water: unset = GRAPHICS > WATER (w02), 1 =
// this, 0 = old filter, 2 = diagnostic (left colour stage with no profile at all).
bool eyeWaterColor{},eyeWaterReady{};std::atomic<std::uint64_t> eyeWaterFrames{},eyeWaterFallbacks{},eyeWaterOff{},eyeWaterColorStages{},eyeWaterUnpaired{};
std::uintptr_t waterQueryGlobal{};std::atomic<std::uint64_t> waterForced{},waterQueryWasZero{};
// w02: GRAPHICS > WATER (runtime::waterSetting, read every call so the row acts at once); the property (0/1/2, polled every 256 calls)
// overrides it and is published for the row's PROP suffix.
static int eyeWaterMode() noexcept {
    static unsigned calls=0;static int prop=-1;
    if((calls++&255u)==0){char v[PROP_VALUE_MAX]{};prop=__system_property_get("debug.rdrvr.vr.water",v)>0&&v[0]>='0'&&v[0]<='2'&&!v[1]?v[0]-'0':-1;
        runtime::waterMenuProperty.store(prop,std::memory_order_relaxed);}
    return prop>=0?prop:runtime::waterSetting.load(std::memory_order_relaxed)!=0?1:0;}
static bool eyeWaterLever() noexcept {return eyeWaterMode()!=0;}
std::string gameEyeWaterReport(){
    return "{\"frames\":"+std::to_string(eyeWaterFrames.load())+",\"fallbacks\":"+std::to_string(eyeWaterFallbacks.load())+",\"off\":"+std::to_string(eyeWaterOff.load())+",\"colorStages\":"+std::to_string(eyeWaterColorStages.load())+",\"unpaired\":"+std::to_string(eyeWaterUnpaired.load())+",\"ready\":"+std::string(eyeWaterReady?"true":"false")+",\"queryGlobal\":"+std::string(waterQueryGlobal?"true":"false")+",\"forced\":"+std::to_string(waterForced.load())+",\"queryWasZero\":"+std::to_string(waterQueryWasZero.load())+"}";}
std::string gameHudScreenViewportReport(){
    return "{\"applied\":"+std::to_string(screenVp.applied.load())+",\"restored\":"+std::to_string(screenVp.restored.load())+",\"skipped\":"+
        std::to_string(screenVp.skipped.load())+",\"native\":["+std::to_string(screenVp.lastW.load())+","+std::to_string(screenVp.lastH.load())+"]}";
}
bool vrHudBegin() noexcept {
    namespace hud=::rdrvr::vr::hud;
    try{
        if(!hud::wanted(hudFrameContext.world.renderFrameId))return false;
        { // No HUD for 3 s after a cutscene / transition / scripted camera - the game still draws its letterbox / fade
          // (a black rectangle) into the HUD interval for a while after the world is back
            static std::uint64_t lastCinematicNs=0;const auto now=diagnostics::monotonicNs();const auto st=presentation.snapshot(now);
            using P=runtime::PresentationContext;
            if(st.context==P::Cutscene||st.context==P::CameraTransition||st.context==P::CameraOverride||st.context==P::Menu)lastCinematicNs=now; // Menu: scripted scenes hide the HUD = MENU
            if(lastCinematicNs&&now-lastCinematicNs<3'000'000'000ull){hud::skip(hud::Skip::NotWorld);return false;}
        }
        if(!hudFrameContext.world.renderFrameId||hudFrameContext.world.mode!=RenderMode::Stereo){hud::skip(hud::Skip::NoWorld);return false;}
        // WORLD only: in a stereo cutscene the HUD interval also draws the letterbox bars (black rectangles on the HUD panel)
        if(!sessionRunning.load()||stopWorker.load()||!worldPresentationAllowed()){hud::skip(hud::Skip::NotWorld);return false;}
        if(!framebufferApiReady.load(std::memory_order_acquire)||!retailResourcesReady.load(std::memory_order_acquire)||!retailDrawReady.load(std::memory_order_acquire)||
           !retailDrawApi.clear||!framebufferApi.flush||!::rdrvr::engine::postfx::ready.load(std::memory_order_relaxed)){hud::skip(hud::Skip::Apis);return false;} // postfx: ClearAlpha drop path
        HudResources reader(vrHudResourceApi(),passResources,commandResources,sessionBinding.device,sessionBinding.queueFamilyIndex);
        HudResourceSet s{};
        if(reader.inspect(s)!=HudResourceStatus::Ready){hud::skip(hud::Skip::Source);return false;}
        if(s.factoryDirty){
            if(!hudRenderReady.load(std::memory_order_acquire)||!hudRenderEntry.factoryFlush){hud::skip(hud::Skip::Binding);return false;}
            hudRenderEntry.factoryFlush();
            if(reader.inspect(s)!=HudResourceStatus::Ready){hud::skip(hud::Skip::Source);return false;}
        }
        if(s.factoryDirty||s.desired!=s.framebuffer){hud::skip(hud::Skip::Binding);return false;}
        const auto i=s.image;
        if(s.external||!(i.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)||i.samples!=VK_SAMPLE_COUNT_1_BIT||i.layers!=1||i.mips!=1||s.width<64||s.height<64||
           (i.format!=VK_FORMAT_B8G8R8A8_SRGB&&i.format!=VK_FORMAT_R8G8B8A8_SRGB&&i.format!=VK_FORMAT_B8G8R8A8_UNORM&&i.format!=VK_FORMAT_R8G8B8A8_UNORM)){
            hud::skip(hud::Skip::Format);return false;}
        // grcDevice::Clear(color, Color32 0 = transparent black, no depth, no stencil); the game's own call right after clears depth/stencil.
        retailDrawApi.clear(true,0u,false,1.0f,false,0u,0xff);
        framebufferApi.flush(reinterpret_cast<void*>(s.context));
        HudResourceSet a{};
        if(reader.inspect(a)!=HudResourceStatus::Ready||a.pendingClears||a.factoryDirty||!a.renderPass||a.desired!=a.framebuffer||a.actual!=a.framebuffer||
           a.image.image!=i.image||a.texture!=s.texture){hud::skip(hud::Skip::Clear);return false;}
        hud::Source src{};src.image=a.image.image;src.generation=a.image.generation;src.extent={a.width,a.height};src.format=a.image.format;
        src.usage=a.image.usage;src.texture=a.texture;src.framebuffer=a.framebuffer;src.frame=hudFrameContext.world.renderFrameId;
        hud::begun(src);hud::alphaBegin();screenViewportApply(a.width,a.height);return true;
    }catch(...){hud::cancel();return false;}
}
// HUD End (TouchUI::Render return 0x2a589e8): the command buffer recording the HUD; copied at its vkQueueSubmit.
void vrHudEnd() noexcept {
    namespace hud=::rdrvr::vr::hud;
    screenViewportRestore();
    if(!hud::active())return;
    hud::alphaEnd();
    try{
        HudResources reader(vrHudResourceApi(),passResources,commandResources,sessionBinding.device,sessionBinding.queueFamilyIndex);
        HudResourceSet e{};const auto src=hud::activeSource();
        if(reader.inspect(e)==HudResourceStatus::Ready&&e.image.image==src.image&&e.image.generation==src.generation&&e.texture==src.texture&&e.command.command){
            hud::recorded(e.command.command);return;}
        hud::skip(hud::Skip::Binding);
    }catch(...){}
    hud::cancel();
}
#endif
void gameObserveHudFrame(HudFrameEvent event,void* renderer) noexcept {
    const int saved=errno;
    try{
    if(event==HudFrameEvent::Enter){hudFrameContext={};hudFrameContext.renderer=renderer;}
    else if(hudFrameContext.renderer==renderer){
        if(event==HudFrameEvent::Begin){
            hudFrameContext.interval=true;
            if(hudFrameContext.world.renderFrameId){++hudMatchedWorldFrames;hudLastMatchedFrame=hudFrameContext.world.renderFrameId;hudFrameStatus="WORLD_MATCHED_WAITING_FOR_HUD_ROUTING";}
            else{++hudUnmatchedFrames;hudFrameStatus="NO_WORLD_IN_THIS_RENDER_FRAME";}
            bool vrHud=false; // The VR HUD owns this interval; the dormant EngineHudProducer is not asked (saves its per-frame inspect)
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_NATIVE_MENUS)
            vrHud=vrHudBegin();
            if(vrHud)hudFrameStatus="VR_HUD_CLEARED";
#endif
#ifdef RDRVR_NATIVE_HUD
            if(!vrHud&&hudFrameContext.world.renderFrameId&&sessionRunning.load()&&!stopWorker.load()&&worldPresentationAllowed()&&
               hudRenderReady.load(std::memory_order_acquire)&&engineTargetApiReady.load(std::memory_order_acquire)&&
               framebufferApiReady.load(std::memory_order_acquire)&&retailResourcesReady.load(std::memory_order_acquire)&&retailDrawReady.load(std::memory_order_acquire)){
                diagnostics::WallScope prepareTiming(diagnostics::frameTimings.hudPrepare);
                if(!hudProducer){
                    const auto& fb=framebufferApi;HudProducerApi api{};
                    api.resources={fb.memory,fb.ownsThread,retailResourceApi.currentGfx,fb.gfxVtable,fb.framebufferVtable,fb.colorVtable,fb.factoryDirty,fb.depthVtable};
                    api.resources.externalImage=describeHudPresentImage;
                    api.targets=engineTargetApi;api.framebuffers=fb;api.render=hudRenderEntry;
                    api.factoryGlobal=retailResourceApi.factory;api.deviceGlobal=retailResourceApi.device;api.deviceVtable=retailResourceApi.deviceVtable;
                    api.clear=retailDrawApi.clear;
                    api.overlay=[](void*,VkDevice d,VkExtent2D size,VkFormat format){return theater.overlaySupported(d,size,format);};
                    hudProducer=std::make_unique<EngineHudProducer>(api,passResources,commandResources,hudCapture,sessionBinding.device,sessionBinding.queueFamilyIndex);
                }
                hudProducerActive.store(true,std::memory_order_release);
                if(hudProducer->begin(worldGameQueue.load(std::memory_order_acquire),hudFrameContext.world))hudFrameContext.hudStartNs=diagnostics::monotonicNs();
                publishHudProducer();
            }
#endif
        }else if(event==HudFrameEvent::End){
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_NATIVE_MENUS)
            vrHudEnd();
#endif
#ifdef RDRVR_NATIVE_HUD
            if(hudProducer&&hudProducer->active()){
                diagnostics::frameTimings.hudInterval.elapsedWall(hudFrameContext.hudStartNs,diagnostics::monotonicNs());
                diagnostics::WallScope finishTiming(diagnostics::frameTimings.hudFinish);
                hudProducer->finish();publishHudProducer();
            }
#endif
            hudFrameContext.interval=false;
        }else if(event==HudFrameEvent::Abort||event==HudFrameEvent::Leave){
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_NATIVE_MENUS)
            ::rdrvr::vr::hud::cancel(); // An unfinished interval publishes nothing (no-op after End)
            screenViewportRestore();
#endif
#ifdef RDRVR_NATIVE_HUD
            if(hudProducer&&hudProducer->active()){
                diagnostics::frameTimings.hudInterval.elapsedWall(hudFrameContext.hudStartNs,diagnostics::monotonicNs());
                diagnostics::WallScope finishTiming(diagnostics::frameTimings.hudFinish);
                hudProducer->abort();publishHudProducer();
            }
#endif
            hudFrameContext={};if(event==HudFrameEvent::Abort)hudFrameStatus="HUD_INTERVAL_ABORTED";
        }
    }
    }catch(...){
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_NATIVE_MENUS)
        ::rdrvr::vr::hud::cancel();
        screenViewportRestore();
#endif
#ifdef RDRVR_NATIVE_HUD
        if(hudProducer)hudProducer->abort();
        hudProducerActive.store(false,std::memory_order_release);
#endif
        hudFrameContext={};hudFrameStatus="HUD_FRAME_OBSERVER_EXCEPTION";
    }
    errno=saved;
}
void gameObserveHud(HudStage stage,HudPhase phase) noexcept {
    struct Borrow {HudResourceSet before{};HudStage stage{};bool valid{},sampled{};};
    static thread_local Borrow borrowed;
    static thread_local std::array<std::uint64_t,static_cast<unsigned>(HudStage::Count)> visits{};
    const int saved=errno;
    try{
        const auto index=static_cast<unsigned>(stage);if(index>=hudObservation.stages.size())return;
        // Diagnostic sampling must not add hundreds of self-memory reads to
        // every game frame. Pair the first eight visits, then every 120th visit.
        if(phase==HudPhase::Before){borrowed={};const auto visit=++visits[index];borrowed.sampled=visit<=8 || visit%120==0;}
        if(!borrowed.sampled)return;
        HudResourceSet current{};HudResourceStatus result=HudResourceStatus::NotConfigured;
        if(sessionRunning.load()&&!stopWorker.load()&&framebufferApiReady.load(std::memory_order_acquire)&&retailResourcesReady.load(std::memory_order_acquire)){
            const auto& fb=framebufferApi;
            HudResourceApi api{fb.memory,fb.ownsThread,retailResourceApi.currentGfx,fb.gfxVtable,fb.framebufferVtable,fb.colorVtable,fb.factoryDirty,fb.depthVtable};
#ifdef RDRVR_NATIVE_MENUS
            api.externalImage=describeHudPresentImage;
#endif
            HudResources reader(api,passResources,commandResources,sessionBinding.device,sessionBinding.queueFamilyIndex);
            if(phase!=HudPhase::Aborted)result=reader.inspect(current);
        }
        std::lock_guard guard(hudObservation.mutex);auto& s=hudObservation.stages[index];
        if(phase==HudPhase::Aborted){++s.aborted;s.status="DRAW_ABORTED";borrowed={};}
        else{
            s.status=hudResourceStatusName(result);s.width=current.width;s.height=current.height;
            s.format=current.image.format;s.depthFormat=current.depthImage.format;s.stencil=current.stencil;s.layout=current.layout;s.clears=current.pendingClears;
            s.registered=current.registered;s.open=current.renderPass!=0;s.dirty=current.factoryDirty;
            s.desiredMatches=current.framebuffer&&current.desired==current.framebuffer;s.actualMatches=current.framebuffer&&current.actual==current.framebuffer;
            if(phase==HudPhase::Before){++s.calls;borrowed={current,stage,result==HudResourceStatus::Ready,true};}
            else{++s.completed;
                if(borrowed.valid&&borrowed.stage==stage&&result==HudResourceStatus::Ready){
                    if(HudResources::sameImage(borrowed.before,current))++s.stable;else ++s.changed;
                }borrowed={};
            }
        }
    }catch(...){borrowed={};}
    errno=saved;
}
void gameConfigureRetailDraw(RetailDrawApi api) noexcept {retailDrawApi=api;retailDrawReady.store(bool(api),std::memory_order_release);}
void gameConfigureLodDiagnostics(PassSourceAbi::Reader read,void* user,std::uintptr_t treeHigh) noexcept {lodDiagnostics.configure(read,user,treeHigh);}
WorldDrawProfile& gameWorldProfile() noexcept {return worldProfile;}
void gameProfileHooksReady(bool ready) noexcept {profileHooksReady.store(ready,std::memory_order_release);}
void gameProfileDraw(void* c,void* r,const void* mask,unsigned cats,unsigned pass,unsigned flags,bool a,bool b,unsigned tail){worldProfile.draw(c,r,mask,cats,pass,flags,a,b,tail);}
PrimaryEyeBoundary& gamePrimaryEye() noexcept {return primaryEye;}
EyeShaftBoundary& gameEyeShaft() noexcept {return eyeShaft;}
void gamePrimaryNativeFramebufferBound(void* gfx,void* framebuffer,bool nativeResult) noexcept {
#ifdef RDRVR_HEAD_CAMERA
    const int saved=errno;
    const auto* selected=primaryEye.nativeSelection();auto* views=gameEyeViewports();
    if(selected&&views&&views->ready()&&views->matches(0)&&views->plan().sceneId==selected->sceneId&&
       selected->sceneId==cullDrawSerial&&selected->eyeViewport==views->viewport(0))
        nativePrimaryRaster.bound(gfx,framebuffer,nativeResult);
    errno=saved;
#else
    (void)gfx;(void)framebuffer;(void)nativeResult;
#endif
}
bool gameGraphicsEyeQualified(const void* viewport) noexcept {
#ifdef RDRVR_HEAD_CAMERA
    auto* views=gameEyeViewports();
    if(!primaryHooksReady.load(std::memory_order_acquire)||!views||!views->ready()||!cullViewports||
       !cullDrawRenderer||!cullDrawSerial||views->plan().sceneId!=cullDrawSerial||
       !replayUpdates.good(reinterpret_cast<void*>(cullDrawRenderer))||!gameWorldVisibilityValid()||
       !cullViewports->stereoCull(cullDrawRenderer,cullDrawSerial))return false;
    for(unsigned eye=0;eye<2;++eye)if((!viewport||viewport==views->viewport(eye))&&views->matches(eye))return true;
#else
    (void)viewport;
#endif
    return false;
}
void gamePrimaryHooksReady(bool ready) noexcept {
#ifdef RDRVR_HEAD_CAMERA
    eyeShaft.configure([](void*,const void* viewport,const void*,EyeShaftBoundary::Selection& selected){
        auto* views=gameEyeViewports();
        if(!primaryHooksReady.load(std::memory_order_acquire)||!views||!views->ready()||!cullViewports||
           !cullDrawRenderer||!cullDrawSerial||views->plan().sceneId!=cullDrawSerial||
           !replayUpdates.good(reinterpret_cast<void*>(cullDrawRenderer))||!gameWorldVisibilityValid())return false;
        const unsigned eye=viewport==views->viewport(0)?0:viewport==views->viewport(1)?1:2;
        if(eye>1||!views->matches(eye)||!cullViewports->stereoCull(cullDrawRenderer,cullDrawSerial))return false;
        const auto* conservative=cullViewports->viewport(cullDrawRenderer,cullDrawSerial);
        if(!conservative)return false;
        selected={viewport,conservative,cullDrawSerial};return true;
    });
    primaryEye.configure(
        [](void*,void* renderer,const void*,PrimaryEyeBoundary::Selection& selected){
            const auto now=diagnostics::monotonicNs();auto* views=gameEyeViewports();
            if(!primaryHooksReady.load(std::memory_order_acquire)||!sessionRunning.load()||stopWorker.load()||
#ifdef RDRVR_VR_CUTSCENE
               !stereoPresentationAllowed()||
#else
               !worldPresentationAllowed()||
#endif
               !views||!views->ready()||!retailDrawReady.load()||!retailResourcesReady.load()||
               !profileHooksReady.load()||!replayHooksReady.load()||!textureHooksReady.load()||!targetHooksReady.load()||
               !globalApiReady.load()||!bodyDrawReady.load()||!nativeDrawSuppression.ready()||!worldCullingEnabled.load()||!worldGameQueue.load()||
               !replayPhaseReady.load()||!grassVisibility.ready()||
               !cullViewports||cullDrawRenderer!=reinterpret_cast<std::uintptr_t>(renderer)||!cullDrawSerial||
               views->plan().sceneId!=cullDrawSerial||!views->plan().bodyCreature||!gameWorldVisibilityValid())return false;
            const auto& plan=views->plan();const auto state=presentation.snapshot(now);
#ifdef RDRVR_VR_CUTSCENE
            if(!renderHeadMatches(plan,now,state)||!worldReplayGate.ready(plan,now))return false;
#else
            if(!state.world()||!headCamera.matches(plan,now,state.revision)||!worldReplayGate.ready(plan,now))return false;
#endif
            const auto fullEye=nativeEyeResolution.activeExtent(renderer);
            if(fullEye.width&&(!fullEye.height||!views->prepareScreen(fullEye.width,fullEye.height)))return false;
            const auto* lighting=cullViewports->viewport(cullDrawRenderer,cullDrawSerial);
            if(!lighting)return false;
            if(fullEye.width&&!nativePrimaryRaster.begin(renderer,fullEye.width,fullEye.height))return false;
            if(!views->activate(0))return false;
            selected={views->viewport(0),lighting,plan.sceneId};return true;
        },
        [](void*,void* renderer,const void*,const PrimaryEyeBoundary::Selection& selected){
            auto* views=gameEyeViewports();
            return nativePrimaryRaster.good()&&views&&views->ready()&&views->matches(0)&&views->plan().sceneId==selected.sceneId&&
                views->viewport(0)==selected.eyeViewport&&cullDrawRenderer==reinterpret_cast<std::uintptr_t>(renderer)&&
                cullDrawSerial==selected.sceneId;
        },
        [](void*,bool){
            auto* views=gameEyeViewports();const bool rasterGood=nativePrimaryRaster.end();
            const bool cameraGood=!views||views->restore();const bool good=rasterGood&&cameraGood;
            if(!good&&!primaryRestoreFault.exchange(true,std::memory_order_acq_rel)){
                // Every path uses this callback, including declined setup,
                // consumer early returns and Scene unwinding. An unknown native
                // camera must never re-arm itself on a subsequent frame.
                primaryHooksReady.store(false,std::memory_order_release);
                primaryEyeReuseLatest.store(false,std::memory_order_relaxed);
                if(retailPass)retailPass->poisoned=true;
                if(alternateRetailPass)alternateRetailPass->poisoned=true;
                ++worldQuarantined;worldStatus="PRIMARY_CAMERA_RESTORE_QUARANTINED";
                worldReplayGate.invalidate();
                if(views)passCapture.cancelFrame(views->plan().sceneId);
            }
            return good;
        });
    primaryHooksReady.store(ready,std::memory_order_release);
#else
    (void)ready;
#endif
}
#ifdef RDRVR_HEAD_CAMERA
#include "GameEarlyVisibility.fragment.inc"
#include "CameraGamePass.fragment.inc"
#endif
static void primaryDraw(PrimaryEyeBoundary::Stage stage,RetailSceneBoundary::Draw original,void* renderer,void* stock,bool exact){
    primaryEye.draw(stage,renderer,stock,exact,[&](const void* viewport,bool selected){
#ifdef RDRVR_HEAD_CAMERA
        const auto* cameraViews=gameEyeViewports();
        const auto* cameraPlan=cameraViews&&cameraViews->ready()?&cameraViews->plan():nullptr;
        CameraGamePass cameraPass(selected,0,stage==PrimaryEyeBoundary::Stage::Depth?
            rdr95::CameraMainRoute::Stage::Depth:rdr95::CameraMainRoute::Stage::Color,
            viewport,cameraPlan?cameraPlan->sceneId:0,cameraPlan?cameraPlan->tracking.frameId:0);
        if(selected){
            auto* views=gameEyeViewports();const auto& plan=views->plan();
            const bool depth=stage==PrimaryEyeBoundary::Stage::Depth;
            if(depth){
                rdr95::OrdinaryReplayFrame::begin(plan.sceneId,plan.tracking.frameId);
                earlyBegin(renderer,stock);
            }
            else if(earlyStage(EarlyPass::LeftColor)){if(auto* controller=earlyController())controller->enter(EarlyPass::LeftColor);}
            auto* early=earlyController();
            experimental::NativeEarlyTraceRoute::Pass earlyRoute(early);
            std::optional<experimental::NativeMainVisibilityRoute::PrimaryDepth> main;
            if(depth){if(auto* boundary=earlyMainBoundary())main.emplace(*boundary);}
            struct Pause{bool left{};~Pause(){if(!left)earlyLeave();}} pause;
#if defined(RDRVR_NATIVE_MENUS) && defined(RDRVR_TOUCH_CONTROLS)
            { // HEAD SHADOW: the eye draws of the player bind a head-collapsed copy of its matrix set; the
              // skinning task keeps the set uncollapsed only once this fill is wired (VrBody hook C checks headFillInstalled)
                const bool fill=::rdrvr::vr::bodyik::config.headShadow&&bodyDrawReady;
                bodyDrawFilter.setHeadFill(fill?&::rdrvr::vr::bodyik::headFill:nullptr);
                ::rdrvr::vr::bodyik::headFillInstalled.store(fill,std::memory_order_release);
            }
#endif
            bodyDrawFilter.setGlobalHat(globalHatNode(plan.hatNode)); // BODY: no lone hat in the shadow (next passes / frames)
            bodyDrawFilter.setProbeCreature(plan.bodyCreature); // Probe: is the player's head its own draw node?
            ::rdrvr::engine::shadowupdate::stagger::playerCreature.store(plan.bodyCreature>1?plan.bodyCreature:0,std::memory_order_relaxed); // PLAYER SHADOW WINDOW (1 = CINEMATIC: none)
            BodyDrawFilter::Eye body(bodyDrawFilter,eyeBodyCreature(plan.bodyCreature),plan.hatNode,true);
            std::optional<WorldDrawProfile::Eye> profile; // WATER: the colour stage unfiltered when the water targets are paired
            const int waterMode=depth?0:eyeWaterMode();const bool leftWater=!depth&&waterMode!=0&&eyeWaterReady; // w01
            if(!depth){eyeWaterColor=leftWater;if(leftWater)eyeWaterColorStages.fetch_add(1,std::memory_order_relaxed);}
            if(!(leftWater&&waterMode==2))profile.emplace(worldProfile,nullptr,leftWater);
            const auto tape=depth?::rdrvr::engine::stereo::Stage::Depth:stereo::Stage::Color;
            ::rdrvr::engine::stereo::beginRecording(tape);stereoLaunchStage(tape,true);
            struct EndRecording{::rdrvr::engine::stereo::Stage tape;~EndRecording(){::rdrvr::engine::stereo::endRecording(tape,stereoStockTextures.data(),stereoStockTextureCount);
                if(!::rdrvr::engine::stereo::stageLaunched(tape))stereoLaunchStage(tape,false);}} endRecording{tape};
            original(renderer,const_cast<void*>(viewport));
            if(!depth){ // TAIL IN EYES: rain/lamp volumes/particles/coronas inside the recorded left colour stage
                namespace te=::rdrvr::engine::postfx::tail::early;te::cullViewport=viewport; // PTFX CULL: particles culled by the eye
                struct ClearCull{~ClearCull(){::rdrvr::engine::postfx::tail::early::cullViewport=nullptr;}} clearCull;
                te::run();
            }
            earlyLeave();pause.left=true;return body.good()&&(!profile||profile->good())&&earlyGood()&&rdr95::OrdinaryReplayFrame::good();
        }
#endif
        original(renderer,const_cast<void*>(viewport));return true;
    });
}
void gamePrimaryDepth(RetailSceneBoundary::Draw original,void* renderer,void* stock,bool exact){primaryDraw(PrimaryEyeBoundary::Stage::Depth,original,renderer,stock,exact);}
void gamePrimaryColor(RetailSceneBoundary::Draw original,void* renderer,void* stock,bool exact){primaryDraw(PrimaryEyeBoundary::Stage::Color,original,renderer,stock,exact);}
void gamePrimaryLights(RetailSceneBoundary::Lights original,void* manager,const void* shaft,bool exact){
    // Lighting is never a MAIN eye-camera producer, including reentrant or
    // nonexact calls that happen inside an otherwise selected Color pass.
#ifdef RDRVR_HEAD_CAMERA
    CameraGamePass cameraMask(false,0,rdr95::CameraMainRoute::Stage::Unsupported,nullptr,0,0);
#endif
    primaryEye.lights(exact,[&]{if(exact){stereoProbeStage(nullptr,0,true);::rdrvr::engine::stereo::gpuMark(::rdrvr::engine::stereo::GpuMark::LeftLightBegin);
            ::rdrvr::engine::colorrestart::beginLight(false,::rdrvr::engine::s02::wants(::rdrvr::engine::s02::LR),false);} // LR: primary left light stage
        struct End{bool on;~End(){if(on){::rdrvr::engine::colorrestart::endLight(); // LR: compare-and-restore first
            ::rdrvr::engine::stereo::gpuMark(::rdrvr::engine::stereo::GpuMark::LeftLightEnd);stereoProbeStage(nullptr,0,false);}}} end{exact};original(manager,shaft);});
}
// AppSW: per-eye relative clip (P * R^T, identical to EyeViewports::cameraMatrices, see
// stereoRelClipDiff), eye positions and the LOCAL->world transform of this frame's camera plan.
runtime::SpaceWarpFrame spaceWarpFrame(const StereoCameraPlan& plan,std::uint64_t frame) noexcept {
    runtime::SpaceWarpFrame f{};f.frame=frame;
    for(unsigned e=0;e<2;++e){
        const auto& w=plan.world[e];
        runtime::Mat4 rinv{};for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)rinv[c*4+r]=w[r*4+c];rinv[15]=1;
        const auto rel=runtime::multiply(plan.projection[e],rinv);
        std::copy(rel.begin(),rel.end(),f.relClip[e].begin());
        f.position[e]={w[12],w[13],w[14]};
    }
    const auto& eye=plan.tracking.eyes[0];
    const auto W=runtime::spacewarp::worldFromLocal(plan.world[0].data(),eye.orientation.data(),eye.position.data(),plan.unitsPerMetre>0?plan.unitsPerMetre:1.0);
    std::copy(W.begin(),W.end(),f.worldFromLocal.begin());
    f.unitsPerMetre=plan.unitsPerMetre;f.nearClip=plan.nearClip;f.farClip=plan.farClip;f.projectionY=plan.projection[0][5];
    bool finite=std::isfinite(f.nearClip)&&std::isfinite(f.farClip)&&f.farClip>f.nearClip&&f.nearClip>0;
    for(const auto& m:f.relClip)for(float v:m)finite=finite&&std::isfinite(v);
    for(double v:f.worldFromLocal)finite=finite&&std::isfinite(v);
    f.valid=finite;
    if(finite){ // Per-actor motion vectors - the newest main-thread actor matrices + the eye for the scan's distance order
        runtime::spacewarp::actorChannel.latest(f.actors);
        runtime::spacewarp::actorChannel.setCamera(f.position[0][0],f.position[0][1],f.position[0][2]);}
    return f;
}
bool gameDrawRetailWorld(const RetailSceneFrame& scene) {
    struct FinishPrimary {~FinishPrimary(){
#ifdef RDRVR_HEAD_CAMERA
        {::rdrvr::engine::stereo::FrameStageSample f{}; // Per-STEREO-TEST-mode stage CPU (report async.byMode)
         auto& d=diagnostics::drawStageTimings;
         f.depthBucket={d.lastCpu(0),d.finishedStages(0)};f.colorBucket={d.lastCpu(2),d.finishedStages(2)};f.depthObjects=d.lastObjects(0);f.colorObjects=d.lastObjects(2);
         auto cpu=[](const diagnostics::DurationWindow& w){const auto s=w.recentSnapshot();return ::rdrvr::engine::stereo::StageSample{s.samples?s.last:UINT64_MAX,s.total};};
         f.depthStage=cpu(diagnostics::threadCpuTimings.stockDepth);f.colorStage=cpu(diagnostics::threadCpuTimings.stockColor);f.stereoDraw=cpu(diagnostics::threadCpuTimings.stereoDraw);
         ::rdrvr::engine::stereo::noteFrameStages(f);
         ::rdrvr::engine::stereo::latchBoundaryPhase(); // STEREO TEST 10/11/12 on/off for the next frame (before gpuSetState)
         { // Arm BEFORE the signature, so its _pt1/_pt0 (DETAIL) or _ptL (lite span) suffix describes the next frames
          const bool detail=diagnostics::profilingEnabled();
          ::rdrvr::engine::stereo::gpuStagesArmed.store(detail,std::memory_order_relaxed);
          ::rdrvr::engine::stereo::gpuLiteArmed.store(!detail,std::memory_order_relaxed);
          runtime::cameraIndoorMenu.store(::rdrvr::engine::indoor::flag(),std::memory_order_relaxed); // SUN SHADOWS INDOOR menu suffix
          runtime::gameQueueVerifyTick();} // xrQueueVerified after 300 layer frames, KGSL preempt sampling
         if(::rdrvr::engine::stereo::gpuStagesArmed.load(std::memory_order_relaxed)||::rdrvr::engine::stereo::gpuLiteArmed.load(std::memory_order_relaxed))stereoGpuState(f);}
        ::rdrvr::engine::stereo::finishFrameJobs(); // worker mode: join/discard this frame's jobs before the next frame's tapes
        earlyFinish();rdr95::OrdinaryReplayFrame::end();
#endif
        primaryEye.finish();}} finishPrimary;
    const auto* primarySelection=primaryEye.ready(scene.renderer,scene.viewport);
#ifdef RDRVR_HEAD_CAMERA
    if(!primarySelection)earlyEnd();
#endif
    diagnostics::WallScope worldTiming(diagnostics::frameTimings.worldAttempt);
    ++retailScenes;
#ifdef RDRVR_HEAD_CAMERA
    experimental::renderAblation54.frame(retailScenes);
    rdr2_game_worker::diagnosticWindow74(experimental::renderAblation54.workerDiagnosticNonce74());
    experimental::RenderAblation54::World ablationWorld54(experimental::renderAblation54);
#endif
    diagnostics::WorldEvent profileEvent(cullDrawSerial);
    auto skip=[&](const char* why,const char* detail=nullptr){profileEvent.result(why);worldReplayGate.skipped(!std::strcmp(why,"PREVIOUS_EYES_PENDING"));recordWorldReject(why);worldStatus=why;++worldSkipped;
        if(std::strcmp(why,"NATIVE_UI_THEATER"))diagnostics::worldFailureHistory.push(cullDrawSerial,detail?detail:why);
        return false;};
    if(!sessionRunning.load()||stopWorker.load())return skip("XR_NOT_RUNNING");
    if(primaryRestoreFault.load(std::memory_order_acquire))return skip("PRIMARY_CAMERA_RESTORE_QUARANTINED");
#ifdef RDRVR_HEAD_CAMERA
    if(!replayPhaseReady.load(std::memory_order_acquire)||!grassVisibility.scopeActive()||!grassVisibility.good())return skip("REPLAY_STATE_UNAVAILABLE");
#endif
#ifdef RDRVR_NATIVE_MENUS
#ifdef RDRVR_VR_CUTSCENE
    if(!stereoPresentationAllowed())return skip("NATIVE_UI_THEATER");
#else
    if(!worldPresentationAllowed())return skip("NATIVE_UI_THEATER");
#endif
#endif
    if(!worldCullingEnabled.load(std::memory_order_acquire))return skip("CULL_PROFILE_NOT_ENABLED");
    if(!retailDrawReady.load(std::memory_order_acquire)||!retailResourcesReady.load(std::memory_order_acquire)||
       !profileHooksReady.load(std::memory_order_acquire)||!replayHooksReady.load(std::memory_order_acquire)||
       !textureHooksReady.load(std::memory_order_acquire)||!targetHooksReady.load(std::memory_order_acquire)||
       !globalApiReady.load(std::memory_order_acquire))return skip("WORLD_APIS_UNAVAILABLE");
    auto* views=gameEyeViewports();
    if(!views||!views->ready()||!cullDrawSerial||cullDrawSerial!=views->plan().sceneId||
       cullDrawRenderer!=reinterpret_cast<std::uintptr_t>(scene.renderer))return skip("CULL_SNAPSHOT_UNAVAILABLE");
#ifdef RDRVR_HEAD_CAMERA
    if(!primarySelection){
        const auto fullEye=nativeEyeResolution.activeExtent(scene.renderer);
        if(fullEye.width&&!views->prepareScreen(fullEye.width,fullEye.height))return skip("FULL_EYE_SCREEN_UNAVAILABLE");
    }
#endif
    const auto* unionShaft=cullViewports->shaft(cullDrawRenderer,cullDrawSerial);
    if(!unionShaft||!gameWorldVisibilityValid())return skip("CONSERVATIVE_VISIBILITY_UNAVAILABLE");
    const auto queue=worldGameQueue.load(std::memory_order_acquire);if(!queue)return skip("GAME_QUEUE_UNAVAILABLE");
    const auto& plan=views->plan();
    const char* trackingStatus="NOT_CHECKED";std::uint64_t trackingCheckedNs{};
    auto trackingCurrent=[&]{runtime::TrackedViews latest{};const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#ifdef RDRVR_HEAD_CAMERA
        trackingCheckedNs=now<0?0:static_cast<std::uint64_t>(now);
        const auto state=presentation.snapshot(now);
#ifdef RDRVR_VR_CUTSCENE
        if(!stereoPresentation(state,std::uint64_t(now))){trackingStatus="PRESENTATION_CHANGED";return false;}
        if(!renderHeadMatches(plan,std::uint64_t(now),state)){trackingStatus="HEAD_CONTEXT_OR_AGE";return false;}
#else
        if(!state.world()){trackingStatus="PRESENTATION_CHANGED";return false;}
        if(!headCamera.matches(plan,now,state.revision)){trackingStatus="HEAD_CONTEXT_OR_AGE";return false;}
#endif
#endif
        trackingCheckedNs=now<0?0:static_cast<std::uint64_t>(now);
        if(now<0||trackingCheckedNs<plan.tracking.publishedNs||trackingCheckedNs-plan.tracking.publishedNs>runtime::TrackedViewsMailbox::maxAgeNs){trackingStatus="FROZEN_POSE_AGE";return false;}
        if(!gameViews.snapshot(now,latest)){trackingStatus="LIVE_TRACKING_UNAVAILABLE";return false;}
        if(latest.epochs!=plan.tracking.epochs){trackingStatus="TRACKING_EPOCH_CHANGED";return false;}
        trackingStatus="READY";return true;};
    if(!trackingCurrent())return skip("FROZEN_TRACKING_EXPIRED");
    LodDiagnostics::Pair lodPair(lodDiagnostics,reinterpret_cast<std::uintptr_t>(scene.renderer),reinterpret_cast<std::uintptr_t>(scene.viewport),plan,diagnostics::monotonicNs());
    if(retailPass){
        if(retailPass->poisoned)return skip("WORLD_OWNER_QUARANTINED");
        if(!retailPass->pass->retire()){
            if(alternateRetailPass&&(alternateRetailPass->poisoned||!alternateRetailPass->pass->retire()))return skip("PREVIOUS_EYES_PENDING");
            retailPass.swap(alternateRetailPass);
        }
    }
    RetailResources resources(retailResourceApi,passResources,sessionBinding.device);RetailResourceSet before{},after{};
    auto resourceFailure=[&](const char* phase){
        // Keep failures even when the next frame succeeds. Store orientations
        // (not native pointers) to correlate direction-dependent theater drops.
        std::ostringstream o;o<<"{\"frame\":"<<plan.sceneId<<",\"ns\":"<<diagnostics::monotonicNs()<<",\"phase\":\""<<phase
            <<"\",\"probe\":"<<resources.report()<<",\"baseForward\":[";
        for(unsigned i=0;i<3;++i){if(i)o<<',';o<<-plan.baseWorld[8+i];}o<<"],\"eyeForward\":[";
        for(unsigned i=0;i<3;++i){if(i)o<<',';o<<-(plan.world[0][8+i]+plan.world[1][8+i])*.5f;}o<<"]}";
        std::lock_guard lock(resourceReportMutex);++resourceFailureCounts[resources.status()];
        resourceFailures[resourceFailureNext]=o.str();resourceFailureNext=(resourceFailureNext+1)%resourceFailures.size();
        if(resourceFailureSize<resourceFailures.size())++resourceFailureSize;
    };
    const bool leftWater=eyeWaterColor;eyeWaterColor=false; // w01: did this frame's left colour stage draw water
    bool water=eyeWaterLever();if(!water){eyeWaterOff.fetch_add(1,std::memory_order_relaxed);eyeWaterReady=false;}
    bool resourcesGood=resources.inspect(scene,before,water);
    if(water&&!resourcesGood){water=false;eyeWaterReady=false;eyeWaterFallbacks.fetch_add(1,std::memory_order_relaxed);resourcesGood=resources.inspect(scene,before,false);}
    if(leftWater&&!water){eyeWaterUnpaired.fetch_add(1,std::memory_order_relaxed);return skip("WATER_TARGETS_UNPAIRED");}
#ifdef RDRVR_NATIVE_MENUS
    if(!resourcesGood&&!std::strcmp(resources.status(),"BOUND_SURFACES")&&hudRenderReady.load()){
        resourceFailure("BEFORE_DEFERRED_FLUSH");
        const auto flushed=flushPendingNativeBinding(framebufferApi,commandResources,sessionBinding.device,
            sessionBinding.queueFamilyIndex,retailResourceApi.currentGfx,hudRenderEntry.factoryFlush);
        if(flushed==NativeBindingFlush::Flushed){
            ++deferredWorldFlushes;resourcesGood=resources.inspect(scene,before,water);
            if(resourcesGood)++deferredWorldRecovered;
        }
    }
#endif
    // The diagnostic report strings are rebuilt every 16th world frame (and on failures), not every frame
    static unsigned worldReportTick{};const bool worldReportFrame=(worldReportTick++&15u)==0;
    if(worldReportFrame||!resourcesGood){std::lock_guard lock(resourceReportMutex);resourceReport=resources.report();}
    if(!resourcesGood){resourceFailure("INITIAL");return skip("RETAIL_RESOURCES_UNAVAILABLE",resources.status());}
    NativeEyeSource nativeLeft{};
    if(primarySelection)nativeLeft={before.images[0],primarySelection->eyeViewport,plan.sceneId,plan.tracking.frameId};
    if(primarySelection)for(unsigned i=0;i<before.count;++i)if(before.stock[i].role==EngineTargetRole::Depth){nativeLeft.depthTarget=reinterpret_cast<std::uintptr_t>(before.stock[i].target);break;}
    if(retailPass){
        if(retailPass->factory!=before.factory||retailPass->device!=before.device){
            ::rdrvr::engine::stereo::cancelFrameJobs("OWNER_RESET"); // A new owner may get the old token (same address)
            retailPass.reset();}
    }
    if(!retailPass){
        auto next=std::make_unique<RetailPassOwner>();next->factory=before.factory;next->device=before.device;
        next->images=gameCreateEyeTargets(before.factory);if(!next->images)return skip("EYE_OWNER_UNAVAILABLE");
        next->buffers=gameCreateFramebuffers(*next->images,before.device);next->map=gameCreateTextureMap(*next->images);next->resolve=gameCreateDepthResolve(*next->images);
        if(!next->buffers||!next->map||!next->resolve)return skip("WORLD_RESOURCE_APIS_UNAVAILABLE");
        next->pass=gameCreateStereoPass(*next->images,*next->buffers);if(!next->pass)return skip("WORLD_PASS_UNAVAILABLE");
        retailPass=std::move(next);
    }
    auto& owner=*retailPass;
    // Frame stamp + release of deferred view/framebuffer destroys (same watermark as the foveation variant LRU),
    // before prepare() may rebuild the eye targets
    ::rdrvr::engine::stereo::frameHook(plan.sceneId,passCapture.retiredFrame());
    diagnostics::WallScope targetTiming(diagnostics::frameTimings.targetPrepare);
    // A spec change rebuilds the eye targets. This frame's worker jobs were launched with the previous
    // generation's predicted maps (old right images / owned framebuffers / clone slots): cancel + join them first.
    // The engine defers the old targets' Vulkan destroys by GetNumBuffers()+1 frames (d11 DeferredDeleteList), so
    // the old objects stay valid meanwhile; the cancel closes the late-cancel window and makes rebuilds visible.
    const std::uint64_t generationBefore=owner.images->generation();
    // LEAK (user: recentering on a horse killed the memory): a one-frame water fallback (the water targets were not found this frame
    // seen at recenters) changed the spec to "no water" and back = two eye-target rebuilds, and a rebuild does not free the old engine
    // targets. With water targets already built, such a frame is skipped instead of rebuilding.
    // not a skipped frame (a cutscene without water targets would stay in the theater): the frame renders without water on the
    // water-capable targets (spec.water only allocates the two extra roles; it never enables water draws).
    EngineTargetSpec prepareSpec=before.spec;
    if(generationBefore&&!(owner.preparedSpec==before.spec)&&!(before.spec.water!=EngineWaterTargetSpec{})&&(owner.preparedSpec.water!=EngineWaterTargetSpec{})){
        auto withWater=before.spec;withWater.water=owner.preparedSpec.water;
        if(withWater==owner.preparedSpec){prepareSpec=withWater;waterRebuildsAvoided.fetch_add(1,std::memory_order_relaxed);}
    }
    unsigned rebuildJobs=0;
    if(generationBefore&&!(owner.preparedSpec==prepareSpec))rebuildJobs=::rdrvr::engine::stereo::cancelFrameJobs("TARGET_REBUILD");
    if(!owner.images->prepare(prepareSpec))return skip("WORLD_TARGETS_UNAVAILABLE");
    if(owner.images->generation()!=generationBefore){
        const auto from=owner.preparedSpec;owner.preparedSpec=prepareSpec;
        if(generationBefore){++eyeTargetRebuilds;eyeTargetRebuildJobs+=rebuildJobs;eyeTargetRebuildFrame=plan.sceneId;
            eyeTargetRebuildFrom=(from.width<<16)|(from.height&0xffff);eyeTargetRebuildTo=(before.spec.width<<16)|(before.spec.height&0xffff);
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_WARN,"RdrVrStereo","eye targets rebuilt gen %llu->%llu frame %llu %ux%u->%ux%u fmt %d->%d, %u launched job(s) cancelled",
                (unsigned long long)generationBefore,(unsigned long long)owner.images->generation(),(unsigned long long)plan.sceneId,
                from.width,from.height,before.spec.width,before.spec.height,int(from.colorFormat),int(before.spec.colorFormat),rebuildJobs);
#endif
        }}
#ifdef RDRVR_FRAGMENT_RATE
    gameFragmentRateEyes(*owner.images,plan,privateQueueMutex);
    gameFoveationFrame(*owner.images,plan,privateQueueMutex,passCapture.retiredFrame()); // Level poll + eye registration (after the VRS maps), variant eviction watermark
#endif
#ifdef RDRVR_HEAD_CAMERA
    ::rdrvr::vr::hands::setEngineRestore({&handsRestoreReady,&handsRestore}); // HANDS (two atomic stores)
    ::rdrvr::vr::hands::configure(sessionBinding.physicalDevice,sessionBinding.device,sessionBinding.queueFamilyIndex,
                                  sessionBinding.queueIndex,&privateQueueMutex); // idempotent: kill switch, worker, eye hook, menu status
    ::rdrvr::vr::taa::setEngineRestore({&handsRestoreReady,&handsRestore}); // TAA: same engine restore as the hands
    ::rdrvr::runtime::GameStereoCapture::setEngineRestore(&handsRestoreReady,&handsRestore); // AppSW depth direct: same restore
    ::rdrvr::vr::taa::setSynthesizingProbe(+[]() noexcept {try{return ::rdrvr::runtime::spaceWarpControl.synthesizing();}catch(...){return false;}}); // AppSW TAA -> LITE
    ::rdrvr::vr::taa::configure(sessionBinding.physicalDevice,sessionBinding.device,sessionBinding.queueFamilyIndex,
                                sessionBinding.queueIndex,&privateQueueMutex); // idempotent: kill switch, worker, eye hook, menu state
#endif
    if(!owner.buffers->prepare())return skip("WORLD_TARGETS_UNAVAILABLE");
    targetTiming.finish();
    {
        EngineStereoPass::replayStage=stereoReplayStage;EngineStereoPass::probeStage=stereoProbeStage;EngineStereoPass::gpuStage=stereoGpuStage;
        auto& maps=stereoFrameMaps;maps=::rdrvr::engine::stereo::FrameMaps{};stereoFrameReady=false;maps.device=sessionBinding.device;maps.context=before.context;
        { // secondaries path (debug.rdrvr.stereoreplay.async=1): engine queue family for the secondary pools, per-frame pool reset
            maps.queueFamily=sessionBinding.queueFamilyIndex;maps.frameSerial=++stereoFrameSerial;}
        { // owner index 0/1 by identity of the RetailPassOwner (two alternate when the previous frame has not retired)
            auto& ownerSlots=stereoOwnerSlots;const void* key=&owner;unsigned idx=0;
            if(ownerSlots[0]==key)idx=0;else if(ownerSlots[1]==key)idx=1;else if(!ownerSlots[0])ownerSlots[0]=key,idx=0;else if(!ownerSlots[1])ownerSlots[1]=key,idx=1;
            else{idx=stereoOwnerLast^1u;ownerSlots[idx]=key;}
            if(idx!=stereoOwnerLast)++stereoOwnerSwaps;stereoOwnerLast=idx;maps.owner=idx;}
        stereoStockTextureCount=0;
        for(unsigned i=0;i<before.count&&maps.imageCount<maps.images.size();++i){
            const auto role=before.stock[i].role;const auto* right=owner.images->image(1,role);
            if(!right||!before.images[i].image.image||!right->image.image)continue;
            auto& pair=maps.images[maps.imageCount++];
            pair.left=before.images[i].image.image;pair.right=right->image.image;pair.leftTexture=before.images[i].texture;pair.rightTexture=right->texture;pair.role=unsigned(role);pair.extent=right->image.extent;pair.format=right->image.format;
            if(stereoStockTextureCount<stereoStockTextures.size())stereoStockTextures[stereoStockTextureCount++]=before.images[i].texture;
        }
        maps.valid=maps.imageCount>0;if(maps.valid)++stereoFramesMapped;
        { // worker mode: check the launched jobs' predicted maps against this fill; keep it for the next prediction
            const auto token=stereoOwnerToken(&owner);::rdrvr::engine::stereo::validateLaunched(maps,token);
            if(maps.valid){stereoLastMaps[maps.owner&1u]=maps;stereoLastToken[maps.owner&1u]=token;}}
        // The mod-owned 1x1 dummy view (written instead of a dead copy source and over destroyed companion views
        // in engine elements), created once on the XR queue under its mutex (a fenced one-time transition, like the VRS maps)
        if(::rdrvr::engine::stereo::dummyDescriptorWanted())
            ::rdrvr::engine::stereo::ensureDummyDescriptor(sessionBinding.device,sessionBinding.queueFamilyIndex,sessionBinding.queueIndex,&privateQueueMutex);
    }
    diagnostics::WallScope preflightTiming(diagnostics::frameTimings.nativePreflight);
    EnginePassBinding nativePass(*owner.buffers,commandResources,sessionBinding.device,sessionBinding.queueFamilyIndex);
    void (*factoryFlush)(){};
#ifdef RDRVR_NATIVE_MENUS
    if(hudRenderReady.load())factoryFlush=hudRenderEntry.factoryFlush;
#endif
    const bool nativeReady=nativePass.prepareNative(before.gfx,before.context,factoryFlush);
    bindingProbeStatus=nativePass.status();
    if(!nativeReady)return skip("NATIVE_PASS_PREFLIGHT_FAILED");
    if(!trackingCurrent())return skip("SNAPSHOT_CHANGED_DURING_PREPARE",trackingStatus);
    if(!resources.inspect(scene,after,water)){resourceFailure("PREPARE");return skip("SNAPSHOT_CHANGED_DURING_PREPARE",resources.status());}
    if(!RetailResources::same(before,after))return skip("SNAPSHOT_CHANGED_DURING_PREPARE",RetailResources::difference(before,after));
    eyeWaterReady=water&&(owner.preparedSpec.water!=EngineWaterTargetSpec{});if(water)eyeWaterFrames.fetch_add(1,std::memory_order_relaxed); // w01: next left colour stage keeps bucket 4
    preflightTiming.finish();
    auto globals=gameCreateGlobalState();
    if(!globals)return skip("GLOBAL_OWNER_UNAVAILABLE");
    const bool constantsReady=globals->capture();
    if(worldReportFrame||!constantsReady){std::lock_guard lock(resourceReportMutex);globalReport=globals->report();}
    if(!constantsReady)return skip("GLOBAL_SNAPSHOT_UNAVAILABLE");
    if(!worldReplayGate.qualify(plan,owner.images->generation(),diagnostics::monotonicNs())){
        globals->discardUnchanged();return skip("REPLAY_BLOCKED_IN_THIS_CONTEXT");
    }
    if(!cullViewports->stereoCull(cullDrawRenderer,cullDrawSerial)){
        globals->discardUnchanged();recordWorldReject("WAITING_FOR_STEREO_CULL");
        profileEvent.result("WAITING_FOR_STEREO_CULL");worldStatus="WAITING_FOR_STEREO_CULL";++worldSkipped;return false;
    }
    const auto preDrawNs=trackingCheckedNs;
    FrameContext frame{};frame.gameTickId=std::uint64_t(before.timerOrdinal)+1;frame.renderFrameId=plan.sceneId;
    frame.xrFrameId=plan.tracking.frameId;frame.predictedDisplayTime=plan.tracking.displayTime;frame.mode=RenderMode::Stereo;
    frame.epochs=plan.tracking.epochs;frame.epochs.resources=owner.images->generation();
    auto eyeScene=scene;eyeScene.shaft=unionShaft;
    RetailSceneDraw draw(retailDrawApi,eyeScene,before.collector,before.factory,before.context,before.depth,before.sampled,before.sources(),
        *owner.map,textureBoundary,targetBoundary,*owner.resolve,replayUpdates,worldProfile);
#ifdef RDRVR_HEAD_CAMERA
    ReplayRenderPhase phase(replayPhaseApi);
    if(!phase.capture()){globals->discardUnchanged();return skip("REPLAY_PHASE_UNAVAILABLE");}
    struct PhaseCleanup {
        ReplayRenderPhase& phase;RetailPassOwner& owner;std::uint64_t frame;bool checked{},committed{};
        ~PhaseCleanup(){
            if(committed)return;
            // Also cover unexpected unwinding between the draw and diagnostic
            // string allocation. A restored CPU integer alone does not permit
            // publishing a pair whose completion checks were never reached.
            const bool restored=checked||phase.restore();passCapture.cancelFrame(frame);
            if(!checked&&(!restored||!phase.good())){
                owner.poisoned=true;++replayPhaseFaults;++worldQuarantined;
                worldStatus="REPLAY_PHASE_RESTORE_QUARANTINED";
            }
        }
    } phaseCleanup{phase,owner,frame.renderFrameId};
    struct PhaseDraw {
        RetailSceneDraw* draw;ReplayRenderPhase* phase;std::uint64_t frame;bool primary;
        static bool invoke(void* user,SceneStage stage,unsigned eye,const void* viewport,const EngineEyeTargets& images,EnginePassBinding& binding){
            auto& s=*static_cast<PhaseDraw*>(user);
            const auto* cameraViews=gameEyeViewports();
            const auto* cameraPlan=cameraViews&&cameraViews->ready()?&cameraViews->plan():nullptr;
            CameraGamePass cameraPass(s.primary&&eye==1&&stage!=SceneStage::Lighting,eye,
                stage==SceneStage::Depth?rdr95::CameraMainRoute::Stage::Depth:
                stage==SceneStage::Color?rdr95::CameraMainRoute::Stage::Color:rdr95::CameraMainRoute::Stage::Unsupported,
                viewport,s.frame,cameraPlan?cameraPlan->tracking.frameId:0);
            const auto next=stage==SceneStage::Depth?ReplayRenderPhase::Stage::Depth:
                stage==SceneStage::Lighting?ReplayRenderPhase::Stage::Lighting:ReplayRenderPhase::Stage::Color;
            EarlyController* early{};
            if(s.primary&&eye==1&&stage!=SceneStage::Lighting){
                const auto pass=stage==SceneStage::Depth?EarlyPass::RightDepth:EarlyPass::RightColor;
                if(earlyStage(pass,&images)){early=earlyController();if(early)early->enter(pass);}
            }
            experimental::NativeEarlyTraceRoute::Pass earlyRoute(early);
            struct Pause{bool left{};~Pause(){if(!left)earlyLeave();}} pause;
            const bool result=s.phase->enter(next)&&RetailSceneDraw::draw(s.draw,stage,eye,viewport,images,binding);
            earlyLeave();pause.left=true;return result&&earlyGood()&&rdr95::OrdinaryReplayFrame::good();
        }
    } phased{&draw,&phase,plan.sceneId,primarySelection!=nullptr};
#endif
    StereoPassResult result{};bool bodyFilterGood=true;
    diagnostics::WallScope drawTiming(diagnostics::frameTimings.stereoDraw);
    diagnostics::ThreadCpuScope drawCpuTiming(diagnostics::threadCpuTimings.stereoDraw);
    ::rdrvr::engine::stereo::gpuMark(::rdrvr::engine::stereo::GpuMark::StereoBegin); // DETAIL GPU stage marker
#ifdef RDRVR_HEAD_CAMERA
    runtime::StereoReticle targetReticle{};
    {std::lock_guard lock(targetAimMutex);
        const auto now=diagnostics::monotonicNs();
        projectVisibleAimTarget(plan,lastAimTarget,targetAimGuid,targetAimRevision,targetAimStamp,nativeReticleFrame,now,targetReticle);
    }
    {
        BodyDrawFilter::Eye bodyEye(bodyDrawFilter,eyeBodyCreature(plan.bodyCreature),plan.hatNode);
        result=owner.pass->drawScene(queue,*views,frame,runtime::SourceEncoding::Linear,before.gfx,before.context,*owner.resolve,PhaseDraw::invoke,&phased,targetReticle,primarySelection?&nativeLeft:nullptr);
        bodyFilterGood=bodyEye.good();
    }
#else
    result=owner.pass->drawScene(queue,*views,frame,runtime::SourceEncoding::Linear,before.gfx,before.context,*owner.resolve,RetailSceneDraw::draw,&draw);
#endif
    ::rdrvr::engine::stereo::gpuMark(::rdrvr::engine::stereo::GpuMark::StereoEnd);
    drawCpuTiming.finish();drawTiming.finish();
    worldPassStatus=owner.pass->status();
    if(diagnostics::profilingEnabled()){std::lock_guard lock(resourceReportMutex);if(worldReportFrame||retailDrawReport=="{}")retailDrawReport=draw.report();
        // Keep hot-path observations fixed-size; serialize occasionally. Odd
        // cadence also samples both owners if their frame use alternates.
        if(frame.renderFrameId%31==0||worldRasterReport=="{}"){
            worldRasterReport=owner.pass->rasterReport();
#ifdef RDRVR_HEAD_CAMERA
            primaryRasterReport=nativePrimaryRaster.report();
#endif
        }}
    const bool globalsRestored=globals->restore();
    const bool primaryRestored=primaryEye.finish();
    bool phaseRestored=true;
#ifdef RDRVR_HEAD_CAMERA
    phaseRestored=phase.restore()&&phase.good()&&grassVisibility.good();
    phaseCleanup.checked=true;
    if(phaseRestored)++replayPhasePairs;else ++replayPhaseFaults;
#endif
    bool ordinaryReplayGood=true;
#ifdef RDRVR_HEAD_CAMERA
    ordinaryReplayGood=rdr95::OrdinaryReplayFrame::good();
#endif
    const bool restored=globalsRestored&&primaryRestored&&phaseRestored&&ordinaryReplayGood;
    if(!restored||result==StereoPassResult::Quarantined){
        passCapture.cancelFrame(frame.renderFrameId);owner.poisoned=true;++worldQuarantined;worldReplayGate.fault(plan,owner.images->generation());worldStatus="WORLD_RESTORE_QUARANTINED";profileEvent.result(worldStatus);return false;
    }
#ifdef RDRVR_HEAD_CAMERA
    // A resource change revokes this visibility result; it is not evidence of
    // failed native state restoration and must not quarantine both eye owners.
    if(!earlyPairGood()){
        passCapture.cancelFrame(frame.renderFrameId);worldReplayGate.invalidate();++worldCancelled;
        worldStatus="EARLY_VISIBILITY_REVOKED";profileEvent.result(worldStatus);return false;
    }
#endif
    if(result!=StereoPassResult::Recorded){
        if(result==StereoPassResult::Cancelled){++worldCancelled;worldReplayGate.fault(plan,owner.images->generation());worldStatus="WORLD_PAIR_CANCELLED";profileEvent.result(worldStatus);return false;}
        worldReplayGate.fault(plan,owner.images->generation());return skip("WORLD_PASS_DECLINED");
    }
    const bool visibilityGood=gameWorldVisibilityValid()&&graphicsDrawPolicy.good(),trackingGood=trackingCurrent(),resourceRead=resources.inspect(scene,after,water);
    if(!resourceRead)resourceFailure("COMPLETION");
    const char* resourceChange=resourceRead?RetailResources::difference(before,after):"UNREADABLE";
    const bool timerOnly=resourceChange&&!std::strcmp(resourceChange,"TIMER_ADVANCED");
    const auto completion=!bodyFilterGood?WorldCompletion::BodyFilterFault:!visibilityGood?WorldCompletion::VisibilityFault:
        (resourceChange&&!timerOnly)?WorldCompletion::ResourceFault:timerOnly?WorldCompletion::TimerAdvanced:
        !trackingGood?WorldCompletion::TrackingExpired:WorldCompletion::Ready;
    const char* completionName=completion==WorldCompletion::BodyFilterFault?"BODY_FILTER_FAILED":completion==WorldCompletion::VisibilityFault?"VISIBILITY_CHANGED":
        completion==WorldCompletion::ResourceFault?"RESOURCE_CHANGED":completion==WorldCompletion::TimerAdvanced?"TIMER_ADVANCED":
        completion==WorldCompletion::TrackingExpired?"TRACKING_EXPIRED":"READY";
    if(diagnostics::profilingEnabled()&&(worldReportFrame||completion!=WorldCompletion::Ready)){std::ostringstream report;const auto age=[](std::uint64_t now,std::uint64_t stamp){return now>=stamp?now-stamp:0;};
        report<<"{\"state\":\""<<completionName<<"\",\"frame\":"<<frame.renderFrameId<<",\"tracking\":\""<<trackingStatus
            <<"\",\"resource\":\""<<resources.status()<<"\",\"resourceDifference\":\""<<(resourceChange?resourceChange:"NONE")
            <<"\",\"bodyGood\":"<<bodyFilterGood<<",\"visibilityGood\":"<<visibilityGood<<",\"timerBefore\":"<<before.timerOrdinal<<",\"timerAfter\":"<<after.timerOrdinal
            <<",\"poseAgeBeforeNs\":"<<age(preDrawNs,plan.tracking.publishedNs)<<",\"poseAgeAfterNs\":"<<age(trackingCheckedNs,plan.tracking.publishedNs)
            <<",\"headAgeBeforeNs\":"<<age(preDrawNs,plan.headSampledNs)<<",\"headAgeAfterNs\":"<<age(trackingCheckedNs,plan.headSampledNs)<<'}';
        std::lock_guard lock(resourceReportMutex);worldCompletionReport=report.str();}
    if(completion!=WorldCompletion::Ready){
        diagnostics::worldFailureHistory.push(frame.renderFrameId,completionName);
        passCapture.cancelFrame(frame.renderFrameId);++worldCancelled;
        worldReplayGate.rejectCompletion(plan,owner.images->generation(),completion);recordWorldReject(completionName);worldStatus=completionName;profileEvent.result(completionName);return false;
    }
    if(runtime::GameStereoCapture::warpWanted())passCapture.setWarpFrame(spaceWarpFrame(plan,frame.renderFrameId));
    if(hudFrameContext.renderer==scene.renderer&&!hudFrameContext.interval)hudFrameContext.world=frame;
    diagnostics::frameTimings.worldRecorded.elapsedWall(worldTiming.start(),diagnostics::monotonicNs());
#ifdef RDRVR_TOUCH_CONTROLS
#ifdef RDRVR_VR_CUTSCENE
    if(!::rdrvr::vr::cutscene::cutscenePlan(plan.actorGuid)) // A cutscene plan never becomes the controllers' aim space
#endif
    controllerAimSpace.publish(plan);
    if(plan.headAnchored)++headCameraRecorded;
    { // Rendered view (left eye, forward = -Z column) for teleport spots; stamped with the plan's head sample so the
      // main-thread turn loop only measures plans made after its last native turn (same rule as ControllerAimSpace::publish).
        const auto& w=plan.world[0];const float fx=-w[8],fy=-w[9],fz=-w[10];
        teleport::publishView(float(std::atan2(double(fx),double(fz))*57.29577951308232),
            float(std::asin(double(std::fmax(-1.f,std::fmin(1.f,fy))))*57.29577951308232),teleport::monotonicNs(),plan.headAnchored?plan.headSampledNs:0);
    }
#ifdef RDRVR_HEAD_CAMERA
    if(targetReticle.frameId)++targetReticlePairs;
#endif
#endif
    primaryEyeReuseLatest.store(owner.pass->nativeEyeReused(),std::memory_order_relaxed);
    if(owner.pass->nativeEyeReused())++primaryFrames;else if(primarySelection)++primaryFallbacks;
    ++worldRecorded;worldStatus="WORLD_PAIR_RECORDED";profileEvent.result("RECORDED");
    ::rdrvr::engine::postfx::pairRecorded(); // RenderFrame's post-processing after this scene may go bind-only (STEREO TEST 16)
#ifdef RDRVR_HEAD_CAMERA
    phaseCleanup.committed=true;
#endif
    return true;
}
void gameConfigurePassSource(PassSourceAbi api) noexcept {passSourceAbi=api;passSourceReady.store(api.read && api.targetVtable && api.textureVtable && api.contextVtable,std::memory_order_release);}
void gameConfigureCullViewport(CullViewportApi api) noexcept {
    if(cullViewportApiReady.load(std::memory_order_acquire))return;
    try{if(api){cullViewports=std::make_unique<CullViewportPool>(api);cullViewportApi=api;cullViewportApiReady.store(true,std::memory_order_release);}}catch(...){}
}
void gameCullHooksReady(bool ready) noexcept {cullHooksReady.store(ready,std::memory_order_release);}
ReplayUpdates& gameReplayUpdates() noexcept {return replayUpdates;}
void gameReplayHooksReady(bool ready) noexcept {replayHooksReady.store(ready,std::memory_order_release);}
bool gameReplayReady() noexcept {return replayHooksReady.load(std::memory_order_acquire);}
void gameReplayTreeTimerBegin(){replayUpdates.treeTimerBegin();}
bool gameReplayTreeTimerEnd(){return replayUpdates.skipTreeTimer(true);}
void gameReplayBlend(void* object){replayUpdates.invoke(ReplayUpdate::BlendShapes,object);}
void gameReplayIndoor(void* object){replayUpdates.invoke(ReplayUpdate::IndoorContribution,object);}
void gameReplayCharacter(void* object){replayUpdates.invoke(ReplayUpdate::CharacterVisibility,object);}
bool gameReplayCloud(void* object,const void* parameters,bool frozen,void** textures){return replayUpdates.cloud(object,parameters,frozen,textures);}
void gameConfigureWorldVisibility(bool(*ensure)() noexcept,bool(*valid)() noexcept) noexcept {ensureWorldVisibility=ensure;validWorldVisibility=valid;}
bool gameWorldVisibilityValid() noexcept {return validWorldVisibility&&validWorldVisibility();}
void gameQualifyVisibility(const void* viewport,const void* shaft,bool success) noexcept {
    if(cullViewportApiReady.load(std::memory_order_acquire))cullViewports->visibility(viewport,shaft,success);
}
void gameEnableWorldCulling(bool enabled) noexcept {worldCullingEnabled.store(enabled,std::memory_order_release);}
const void* gamePrepareCull(std::uintptr_t renderer,std::uint64_t serial,const void* viewport) noexcept {
    if(!cullViewportApiReady.load(std::memory_order_acquire))return nullptr;
    cullViewports->collect();
#ifdef RDRVR_NATIVE_MENUS
#ifdef RDRVR_VR_CUTSCENE
    if(!stereoPresentationAllowed())return nullptr;
#else
    if(!worldPresentationAllowed())return nullptr;
#endif
#endif
    if(!serial||!cullHooksReady.load(std::memory_order_acquire)||!worldCullingEnabled.load(std::memory_order_acquire)||!sessionRunning.load()||stopWorker.load())return nullptr;
    if(!ensureWorldVisibility||!ensureWorldVisibility()){++cullDeclined;return nullptr;}
    try{VisibilityTicket ticket{};
        if(visibilityBridge.prepared(serial,renderer,ticket)){
            ticket.stereoCull=worldReplayGate.ready(ticket.eyes,diagnostics::monotonicNs());
            if(auto* clone=cullViewports->lease(viewport,ticket)){++cullWrites;if(!ticket.stereoCull)++stockCullProbes;return clone;}
        }
    }catch(...){}
    ++cullDeclined;return nullptr;
}
CullViewportPool::Borrow gameBorrowCull(const void* viewport) noexcept {
    // Already published engine requests still need their camera after XR stops.
    if(cullViewportApiReady.load(std::memory_order_acquire))return cullViewports->borrow(viewport);
    return {};
}
void gameDrawFinished(void* renderThread) noexcept {
    const auto thread=reinterpret_cast<std::uintptr_t>(renderThread);
    if(cullDrawSerial&&thread>=8&&thread-8==cullDrawRenderer){
        if(cullViewports->complete(cullDrawRenderer,cullDrawSerial))++cullCompletions;
        cullDrawSerial=0;cullDrawRenderer=0;
    }
}
std::unique_ptr<CullViewport> gameCreateCullViewport() noexcept {
    if(!cullViewportApiReady.load(std::memory_order_acquire) || !sessionRunning.load() || stopWorker.load())return {};
    try{return std::make_unique<CullViewport>(cullViewportApi);}catch(...){return {};}
}
unsigned cutsceneStereoRun{}; // Stereo cutscene frames since the last non-cutscene frame
// WATER IN CUTSCENES:
// sagRenderer::IsWaterVisible() = g_WaterQuery (0x4e85f560) +0x24 || renderer +0x1fd4 || environment flags. Test lever
// debug.rdrvr.vr.waterforce 1 = +0x24 set to 1 before every stereo cutscene frame, 2 = also in the world. Report waterForce.
void gameConfigureWaterQuery(std::uintptr_t q) noexcept {waterQueryGlobal=q;}
static void waterForce(bool cutscene) noexcept {
    static unsigned polls{};static int lever{};if((polls++&63u)==0){char v[PROP_VALUE_MAX]{};lever=__system_property_get("debug.rdrvr.vr.waterforce",v)>0?std::atoi(v):0;}
    if(!waterQueryGlobal||!(lever==2||(lever==1&&cutscene)))return;
    auto* b=reinterpret_cast<volatile std::uint8_t*>(waterQueryGlobal+0x24);
    if(!*b){waterQueryWasZero.fetch_add(1,std::memory_order_relaxed);*b=1;}waterForced.fetch_add(1,std::memory_order_relaxed);
}
std::uint64_t gameBeginVisibility(std::uintptr_t renderer,std::uint32_t marker,std::uintptr_t viewport,std::uintptr_t camera,std::uint64_t ns,
                                VisibilityCameraBridge::Reader reader,void* context){
#ifdef RDRVR_HEAD_CAMERA
    const auto state=presentation.snapshot(ns);HeadCameraAnchor anchor{};
#ifdef RDRVR_VR_CUTSCENE
    { // A cutscene / transition camera becomes a head anchor (CINEMATIC / FIRST PERSON); failure = no anchor = the theater
        namespace cs=::rdrvr::vr::cutscene;const auto ctx=cutsceneCtx(state.context);cs::composer.note(ctx);
        cs::noteContext(unsigned(state.context),ns); // Which context a story scene reports
        {static unsigned leverCalls=0;if((leverCalls++&63u)==0)cs::readOverrideLever();}
        if(!state.world()&&cs::requested()&&cs::composer.eligible(ctx)){
            waterForce(true);
            const bool ok=cutsceneAnchor(viewport,camera,ns,reader,context,state,ctx,anchor);
            const auto serial=visibilityBridge.begin(renderer,marker,viewport,camera,ns,reader,context,ok?&anchor:nullptr,true);
            if(ok&&serial){cs::engagedRevision.store(state.revision,std::memory_order_release);cs::engagedNs.store(ns,std::memory_order_release);cs::bump(cs::stats.stereo);
                // debug.rdrvr.eyedump.cutscene K = an eye dump on the K-th stereo frame of every cutscene (the first frames of a scene)
                static unsigned polls{};static int dumpAt{};if((polls++&63u)==0){char v[PROP_VALUE_MAX]{};dumpAt=__system_property_get("debug.rdrvr.eyedump.cutscene",v)>0?std::atoi(v):0;}
                if(dumpAt>0&&++cutsceneStereoRun==unsigned(dumpAt))::rdrvr::engine::stereo::eyeDumpAutoRequest.store(true,std::memory_order_relaxed);}
            else if(ok)cs::bump(cs::stats.beginFailed);
            else cs::engagedRevision.store(0,std::memory_order_release);
            ::rdrvr::vr::hands::setSuppressed(ok); // no VR hands / laser / markers / arcs in a cutscene
            return serial;
        }
        cs::composer.reset();::rdrvr::vr::hands::setSuppressed(false);cutsceneStereoRun=0;waterForce(false);
    }
#endif
    const bool current=state.world()&&headCamera.snapshot(ns,state.revision,anchor);
    return visibilityBridge.begin(renderer,marker,viewport,camera,ns,reader,context,current?&anchor:nullptr,true);
#else
    return visibilityBridge.begin(renderer,marker,viewport,camera,ns,reader,context);
#endif
}
void gameFinishVisibility(std::uint64_t serial,bool success) noexcept {visibilityBridge.finish(serial,success);}
void gameObserveFrozenCamera(std::uintptr_t renderer,std::uint32_t marker,std::uintptr_t viewport,std::uint64_t ns,
                             VisibilityCameraBridge::Reader reader,void* context){
    // Keep this token beyond SceneFinished: DrawEnd still borrows the request.
    // An unexpected replacement loses retirement of the older slot rather than
    // freeing a camera whose remaining engine borrowers are unknown.
    cullDrawRenderer=renderer;cullDrawSerial=cullViewportApiReady.load(std::memory_order_acquire)?cullViewports->serial(renderer,marker):0;
    eyeViewports.reset();VisibilityTicket ticket{};
    if(visibilityBridge.consume(renderer,marker,viewport,ns,reader,context,ticket)&&viewportApiReady.load(std::memory_order_acquire)){
#ifdef RDRVR_HEAD_CAMERA
        const auto state=presentation.snapshot(ns);
#ifdef RDRVR_VR_CUTSCENE
        if(!renderHeadMatches(ticket.eyes,ns,state))return;
#else
        if(!state.world()||!headCamera.matches(ticket.eyes,ns,state.revision))return;
#endif
#endif
        try{auto pair=std::make_unique<EyeViewports>(viewportApi);
            if(pair->prepare(reinterpret_cast<const void*>(viewport),ticket.eyes)){eyeViewports=std::move(pair);++clonedViewPairs;}
            else ++cloneFailures;
        }catch(...){++cloneFailures;}
    }
}
CommandResources& gameCommands() noexcept {return commandResources;}
std::uint64_t gameCommandRecordingId(VkDevice device,VkCommandBuffer command) noexcept {
    if(!sessionRunning.load()||device!=sessionBinding.device)return 0;
    CommandRecord record{};
    return commandResources.recording(device,command,sessionBinding.queueFamilyIndex,record)?record.recording:0;
}
void gameConfigureEngineTargets(EngineTargetApi api) noexcept {engineTargetApi=api;engineTargetApiReady.store(bool(api),std::memory_order_release);}
void gameConfigureFramebuffers(EngineFramebufferApi api) noexcept {framebufferApi=api;framebufferApiReady.store(bool(api),std::memory_order_release);}
void gameConfigureDepthCopy(EngineDepthCopyApi api) noexcept {depthCopyApi=api;depthCopyApiReady.store(bool(api),std::memory_order_release);}
void gameConfigureTextures(EngineTextureApi api) noexcept {textureApi=api;textureBoundary.configure(api);textureApiReady.store(bool(api),std::memory_order_release);}
void gameTextureHooksReady(bool ready) noexcept {textureHooksReady.store(ready,std::memory_order_release);}
void gameConfigureTargetBindings(EngineTargetBindingApi api) noexcept {targetBoundary.configure(api);}
void gameTargetHooksReady(bool ready) noexcept {targetHooksReady.store(ready,std::memory_order_release);}
void gameTargetLock(void* factory,int index,const void* color,const void* depth,unsigned face,bool changeDepth,int resolve,unsigned mip,bool window){
    ::rdrvr::engine::postfx::noteLock({index,color,depth,face,changeDepth,resolve,mip,window}); // POST FX OFF FAST lock trace
    targetBoundary.lock(factory,index,color,depth,face,changeDepth,resolve,mip,window);
}
void gameTargetUnlock(void* factory,int index,const void* flags,int resolve,bool window){
    ::rdrvr::engine::postfx::noteUnlock(); // POST FX OFF FAST lock trace (+0xa0)
    targetBoundary.unlock(factory,index,flags,resolve,window);
}
void gameTargetResolveDepth(void* manager,bool force){targetBoundary.resolveDepth(manager,force);}
bool gameWithSceneRoutes(EngineTextureMap& map,std::span<const StockSceneTarget> stock,EnginePassBinding& binding,const EngineEyeTargets& targets,
                         void* factory,unsigned eye,EngineTargetRole role,EngineDepthRoute depth,TargetCallBoundary::Body body,void* user){
    if(!targetHooksReady.load(std::memory_order_acquire) || !textureHooksReady.load(std::memory_order_acquire) ||
       !sessionRunning.load() || stopWorker.load())return false;
    return invokeSceneRoutes(textureBoundary,targetBoundary,map,stock,binding,targets,factory,eye,role,depth,body,user);
}
bool gameWithTargetRoutes(EnginePassBinding& binding,EngineTextureMap& map,const EngineEyeTargets& targets,void* factory,unsigned eye,EngineTargetRole role,TargetCallBoundary::Body body,void* user,EngineDepthRoute depth){
    if(!targetHooksReady.load(std::memory_order_acquire) || !sessionRunning.load() || stopWorker.load())return false;
    return targetBoundary.invoke(binding,map,targets,factory,eye,role,body,user,depth);
}
void gameTextureGlobal(int id,const void* texture){textureBoundary.global(id,texture);}
void gameTextureInstance(const void* effect,void* data,int id,const void* texture,bool force,bool unbind){textureBoundary.instance(effect,data,id,texture,force,unbind);}
bool gameWithTextureRoutes(EngineTextureMap& map,TextureCallBoundary::Body body,void* user){
    if(!textureHooksReady.load(std::memory_order_acquire) || !textureApiReady.load(std::memory_order_acquire) || !sessionRunning.load() || stopWorker.load())return false;
    return textureBoundary.invoke(EngineTextureMap::route,&map,body,user);
}
std::unique_ptr<EngineTextureScope> gameCreateTextureScope(EngineTextureScope::Map map,void* user) noexcept {
    if(!textureApiReady.load(std::memory_order_acquire) || !sessionRunning.load() || stopWorker.load())return {};
    try{if(textureApi.ownsThread())return std::make_unique<EngineTextureScope>(textureApi,map,user);}catch(...){}
    return {};
}
std::unique_ptr<EngineTextureMap> gameCreateTextureMap(EngineEyeTargets& targets) noexcept {
    if(!textureApiReady.load(std::memory_order_acquire) || !sessionRunning.load() || stopWorker.load())return {};
    try{if(textureApi.ownsThread())return std::make_unique<EngineTextureMap>(targets,passResources,passSourceAbi,sessionBinding.device,textureApi.ownsThread);}catch(...){}
    return {};
}
std::unique_ptr<EngineDepthResolve> gameCreateDepthResolve(EngineEyeTargets& targets) noexcept {
    if(!depthCopyApiReady.load(std::memory_order_acquire) || !sessionRunning.load() || stopWorker.load())return {};
    try {if(depthCopyApi.ownsThread())return std::make_unique<EngineDepthResolve>(depthCopyApi,targets,passResources,commandResources,sessionBinding.device,sessionBinding.queueFamilyIndex);}catch(...){}
    return {};
}
std::unique_ptr<EngineStereoPass> gameCreateStereoPass(EngineEyeTargets& targets,EngineFramebuffers& buffers) noexcept {
    if(!framebufferApiReady.load(std::memory_order_acquire) || !sessionRunning.load() || stopWorker.load())return {};
    try {if(framebufferApi.ownsThread())return std::make_unique<EngineStereoPass>(targets,buffers,passResources,commandResources,passCapture,passSourceAbi,sessionBinding.device,sessionBinding.queueFamilyIndex);}catch(...){}
    return {};
}
std::unique_ptr<EngineFramebuffers> gameCreateFramebuffers(EngineEyeTargets& targets,void* device) noexcept {
    if(!framebufferApiReady.load(std::memory_order_acquire) || !sessionRunning.load() || stopWorker.load())return {};
    try {if(framebufferApi.ownsThread())return std::make_unique<EngineFramebuffers>(framebufferApi,targets,device);}catch(...){}
    return {};
}
std::unique_ptr<EnginePassBinding> gameCreatePassBinding(EngineFramebuffers& pair) noexcept {
    if(!framebufferApiReady.load(std::memory_order_acquire) || !sessionRunning.load() || stopWorker.load())return {};
    try {if(framebufferApi.ownsThread())return std::make_unique<EnginePassBinding>(pair,commandResources,sessionBinding.device,sessionBinding.queueFamilyIndex);}catch(...){}
    return {};
}
std::unique_ptr<EngineEyeTargets> gameCreateEyeTargets(void* factory) noexcept {
    if(!engineTargetApiReady.load(std::memory_order_acquire) || !sessionRunning.load() || stopWorker.load())return {};
    try {if(engineTargetApi.ownsThread())return std::make_unique<EngineEyeTargets>(engineTargetApi,passResources,sessionBinding.device,factory);}catch(...){}
    return {};
}
void gameImageCreated(VkDevice d,const VkImageCreateInfo* c,VkImage image,VkResult result) noexcept {
    const int saved=errno;{experimental::NativeEarlyImageRegistry::Mutation mutation(earlyImageRegistry);passResources.created(d,c,image,result);}errno=saved;
}
void gameImageDestroyed(VkDevice d,VkImage image) noexcept {
    const int saved=errno;
#ifdef RDRVR_HEAD_CAMERA
    ::rdrvr::vr::hands::imageDestroyed(d,image); // HANDS: views/framebuffers/sets on this engine image, before the driver destroy
    ::rdrvr::vr::taa::imageDestroyed(d,image);   // TAA: same contract
#endif
    {experimental::NativeEarlyImageRegistry::Mutation mutation(earlyImageRegistry);passResources.destroyed(d,image);}errno=saved;
}
bool gameReadPassSource(std::uintptr_t target,std::uintptr_t context,PassSource& out) noexcept {
    if(!passSourceReady.load(std::memory_order_acquire) || !viewportApiReady.load(std::memory_order_acquire))return false;
    try {PassSource source{};CommandRecord cmd{};
        if(!viewportApi.ownsThread() || !readPassSource(passResources,sessionBinding.device,passSourceAbi,target,context,source) ||
           !commandResources.recording(sessionBinding.device,source.command,sessionBinding.queueFamilyIndex,cmd))return false;
        out=source;return true;
    }catch(...){return false;}
}
bool gameRecordEngineEye(unsigned eye,std::uintptr_t target,std::uintptr_t context) noexcept {
    if(!sessionRunning.load() || stopWorker.load())return false;
    try {PassSource source{};if(!gameReadPassSource(target,context,source))return false;
        const auto& i=source.image;
        return passCapture.recordEye(source.command,eye,{i.image,{i.extent.width,i.extent.height},i.format,source.layout,i.usage,i.samples,sessionBinding.queueFamilyIndex,0,i.layers});
    }catch(...){return false;}
}
#endif
#ifdef RDRVR_STEREO_BRIDGE
void gameConfigureViewports(ViewportApi api) noexcept {viewportApi=api;viewportApiReady.store(bool(api),std::memory_order_release);}
void gameSceneFinished() noexcept {
#ifdef RDRVR_PASS_RESOURCES
    primaryEye.finish();
#endif
    eyeViewports.reset();
}
EyeViewports* gameEyeViewports() noexcept {return eyeViewports.get();}
void gameStereoBeforeSubmit(VkQueue queue,std::uint32_t count,const VkSubmitInfo* submits) noexcept {
    const int saved=errno;
    // Native queue ownership is already held by the intercepted submission.
    // Publish completed older work before a new heavy command reaches the queue.
    try{passCapture.completeSubmitted(queue,stereoMailbox,diagnostics::monotonicNs());}catch(...){}
    // SUBMIT GATE: in lockstep the next frame's submit on the capture queue waits for the newest pair's copy (bounded).
    // Only the submit carrying the next stereo pass (heavy end-of-frame submit) waits, not the FIRST submit
    // of the frame (a wait there would stall the render thread mid-frame).
    if(runtime::framePacer.submitGateArmed()&&count&&submits&&passCapture.carriesPass(queue,count,submits))
        stereoMailbox.waitNewestCopy(queue,runtime::framePacer.submitGateCapNs());
    errno=saved;
}
void gameStereoSubmitted(VkQueue queue,std::uint32_t count,const VkSubmitInfo* submits,VkResult result) noexcept {
    const bool stereoPass=passCapture.submitted(queue,count,submits,result);
#ifdef RDRVR_NATIVE_MENUS
    hudCapture.submitted(queue,count,submits,result);
#endif
#if defined(RDRVR_HEAD_CAMERA)&&defined(RDRVR_NATIVE_MENUS)
    ::rdrvr::vr::hud::submitted(queue,count,submits,result,vrHudLayout); // HUD copy right behind its own command buffer, no wait
#endif
    // Frame-rate lock. Pass N and its mailbox copy are queued; no capture lock is held here.
    if(stereoPass)runtime::framePacer.pace({::rdrvr::engine::stereo::gpuLiteSerial(),std::uint64_t(::rdrvr::engine::stereo::gpuLiteFrameUs())*1000u}); // ADAPTIVE governor GPU cost
}
bool gameBeginStereoPasses(VkQueue queue,const runtime::CaptureFrame& frame) noexcept {
    if(!sessionRunning.load() || stopWorker.load())return false;
    try{return passCapture.beginFrame(queue,frame);}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","pass capture begin failed: %s",e.what());return false;}
    catch(...){return false;}
}
bool gameRecordStereoEye(VkCommandBuffer command,unsigned eye,const runtime::PassImage& source) noexcept {
    try{return passCapture.recordEye(command,eye,source);}catch(...){return false;}
}
bool gameCaptureStereo(VkQueue queue,const RenderSubmission& frame,runtime::SourceEncoding encoding) noexcept {
    try {
        const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        return stereoMailbox.capture(queue,frame,now,encoding);
    }catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","stereoMailbox capture failed: %s",e.what());return false;}
    catch(...){return false;}
}
#endif
void gameXrInitialize(JavaVM* vm,jobject object) noexcept {
    std::lock_guard guard(mutex);if(attempted)return;attempted=true;javaVm=vm;
    try {
        JNIEnv* env{};
        if(!vm || vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK){state="JNI_UNAVAILABLE";return;}
        activity=env->NewGlobalRef(object);
        if(!activity){env->ExceptionClear();state="ACTIVITY_REF_FAILED";return;}
        auto init=runtime::proc<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE,"xrInitializeLoaderKHR");
        XrLoaderInitInfoAndroidKHR loader{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};loader.applicationVM=vm;loader.applicationContext=activity;
        if(!checked(init(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader)),"LOADER_INIT_FAILED"))return;
        std::uint32_t count{};
        if(!checked(xrEnumerateInstanceExtensionProperties(nullptr,0,&count,nullptr),"EXTENSION_QUERY_FAILED"))return;
        if(count>256){state="EXTENSION_LIMIT";return;}
        std::vector<XrExtensionProperties> available(count,{XR_TYPE_EXTENSION_PROPERTIES});
        if(!checked(xrEnumerateInstanceExtensionProperties(nullptr,count,&count,available.data()),"EXTENSION_QUERY_FAILED"))return;
        const char* required[]={XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
        for(auto name:required) {
            bool found=false;for(const auto& p:available)found |= !std::strcmp(name,p.extensionName);
            if(!found){state="REQUIRED_EXTENSION_MISSING";return;}
        }
        XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};android.applicationVM=vm;android.applicationActivity=activity;
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};info.next=&android;
        std::strcpy(info.applicationInfo.applicationName,"RDRVR Game Bootstrap");std::strcpy(info.applicationInfo.engineName,"rdrvr");
        info.applicationInfo.applicationVersion=12;info.applicationInfo.engineVersion=1;info.applicationInfo.apiVersion=XR_API_VERSION_1_0;
#ifdef RDRVR_CAMERA_BRIDGE
        info.applicationInfo.applicationVersion=13;
#endif
#ifdef RDRVR_STEREO_BRIDGE
        info.applicationInfo.applicationVersion=14;
#endif
#ifdef RDRVR_PASS_RESOURCES
        info.applicationInfo.applicationVersion=17;
#endif
        std::vector<const char*> extensions(std::begin(required),std::end(required));
        performanceSupport.enable(available,extensions);
        // Application SpaceWarp when the runtime offers XR_FB_space_warp.
        for(const auto& p:available)if(!std::strcmp(p.extensionName,XR_FB_SPACE_WARP_EXTENSION_NAME)){extensions.push_back(XR_FB_SPACE_WARP_EXTENSION_NAME);runtime::spaceWarpControl.extension.store(true);break;}
        { // Optional extensions when listed; a failed xrCreateInstance retries once without them.
            static constexpr const char* timespecExtension="XR_KHR_convert_timespec_time"; // literal: XR_USE_TIMESPEC is not defined
            const auto listed=[&](const char* name){for(const auto& p:available)if(!std::strcmp(p.extensionName,name))return true;return false;};
            const std::size_t optionalFrom=extensions.size();
            bool timespec=listed(timespecExtension),layerSettings=listed(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
            bool autoFilter=layerSettings&&listed(XR_META_AUTOMATIC_LAYER_FILTER_EXTENSION_NAME);
            // TELEPORT BLINK: not requested while TELEPORT is killed (debug.rdrvr.vr.tploco 0; TraceEntry loaded
            // the kill switches before gameXrInitialize); `extensionListed` still says what the runtime offers.
            const bool colorScaleListed=listed(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
            bool colorScale=colorScaleListed&&!vr::killed(vr::Feature::TpLoco);
            vr::tploco::fade::extensionListed.store(colorScaleListed?1:0,std::memory_order_relaxed);
            if(colorScale)extensions.push_back(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME); // first optional (index optionalFrom)
            if(timespec)extensions.push_back(timespecExtension);
            if(layerSettings)extensions.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
            if(autoFilter)extensions.push_back(XR_META_AUTOMATIC_LAYER_FILTER_EXTENSION_NAME);
            // dynres01: XR_META_recommended_layer_resolution when listed and debug.rdrvr.dynres is not "0" (read once; default on). Pushed LAST
            // (the dedicated retry below pops it). XrHarness queries it per projection frame and only reports (runtime::xrDynres).
            const bool dynresListed=listed(XR_META_RECOMMENDED_LAYER_RESOLUTION_EXTENSION_NAME);
            bool dynresProp=true;{char v[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.dynres",v)>0&&!std::strcmp(v,"0"))dynresProp=false;}
            runtime::xrDynres.listed.store(dynresListed?1:0,std::memory_order_relaxed);runtime::xrDynres.prop.store(dynresProp?1:0,std::memory_order_relaxed);
            bool dynres=dynresListed&&dynresProp;
            if(dynres)extensions.push_back(XR_META_RECOMMENDED_LAYER_RESOLUTION_EXTENSION_NAME);
            info.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size());info.enabledExtensionNames=extensions.data();
            XrResult created=xrCreateInstance(&info,&xr);
            // A failure with the TELEPORT extension retries without it ALONE first (timespec / SHARPEN layer settings / auto
            // filter survive it); only a second failure drops every optional extension
            // dynres01: the experimental XR_META_recommended_layer_resolution is dropped ALONE first (before the TELEPORT retry)
            if(XR_FAILED(created)&&dynres&&!extensions.empty()&&!std::strcmp(extensions.back(),XR_META_RECOMMENDED_LAYER_RESOLUTION_EXTENSION_NAME)){
                runtime::xrOptionalExtensionsRetryResult.store(int(created),std::memory_order_relaxed);
                extensions.pop_back();dynres=false;
                info.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size());info.enabledExtensionNames=extensions.data();
                created=xrCreateInstance(&info,&xr);
            }
            if(XR_FAILED(created)&&colorScale&&extensions.size()>optionalFrom){
                runtime::xrOptionalExtensionsRetryResult.store(int(created),std::memory_order_relaxed);
                extensions.erase(extensions.begin()+std::ptrdiff_t(optionalFrom));colorScale=false;
                info.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size());info.enabledExtensionNames=extensions.data();
                created=xrCreateInstance(&info,&xr);
            }
            if(XR_FAILED(created)&&extensions.size()>optionalFrom){
                runtime::xrOptionalExtensionsRetryResult.store(int(created),std::memory_order_relaxed);
                extensions.resize(optionalFrom);timespec=layerSettings=autoFilter=colorScale=dynres=false; // dynres01: + dynres
                info.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size());info.enabledExtensionNames=extensions.data();
                created=xrCreateInstance(&info,&xr);
            }
            const bool instanceOk=XR_SUCCEEDED(created);
            runtime::xrTimespecEnabled.store(instanceOk&&timespec?1:0,std::memory_order_relaxed);
            runtime::xrLayerSettingsEnabled.store(instanceOk&&layerSettings?1:0,std::memory_order_relaxed);
            runtime::xrAutoLayerFilterEnabled.store(instanceOk&&autoFilter?1:0,std::memory_order_relaxed);
            runtime::xrDynresEnabled.store(instanceOk&&dynres?1:0,std::memory_order_relaxed); // dynres01
            __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","dynres01 XR_META_recommended_layer_resolution listed=%d prop=%d enabled=%d",
                dynresListed?1:0,dynresProp?1:0,instanceOk&&dynres?1:0);
            vr::tploco::fade::extensionEnabled.store(instanceOk&&colorScale?1:0,std::memory_order_relaxed); // TELEPORT: no extension -> INSTANT ONLY
            if(!checked(created,"XR_INSTANCE_FAILED"))return;
        }
        XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};get.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if(!checked(xrGetSystem(xr,&get,&system),"XR_SYSTEM_FAILED"))return;
        if(runtime::spaceWarpControl.extension.load()){ // Recommended motion-vector rect
            XrSystemSpaceWarpPropertiesFB warp{XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};properties.next=&warp;
            const auto warpResult=xrGetSystemProperties(xr,system,&properties);runtime::spaceWarpControl.propertiesResult.store(warpResult);
            if(XR_SUCCEEDED(warpResult)&&warp.recommendedMotionVectorImageRectWidth&&warp.recommendedMotionVectorImageRectHeight&&
               warp.recommendedMotionVectorImageRectWidth<=4096&&warp.recommendedMotionVectorImageRectHeight<=4096){
                runtime::spaceWarpControl.width.store(warp.recommendedMotionVectorImageRectWidth);runtime::spaceWarpControl.height.store(warp.recommendedMotionVectorImageRectHeight);}
            __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","spacewarp result=%d recommended=%ux%u",warpResult,warp.recommendedMotionVectorImageRectWidth,warp.recommendedMotionVectorImageRectHeight);
        }
#ifdef RDRVR_HEAD_CAMERA
        // Resolve one coherent stereoMailbox recommendation before the game creates
        // any Vulkan targets. The worker's later per-field telemetry is not an
        // allocation request, and must not trigger growth of in-flight images.
        std::array<XrViewConfigurationView,2> eyeConfig{{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        eyeRecommendationResult=xrEnumerateViewConfigurationViews(xr,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            static_cast<std::uint32_t>(eyeConfig.size()),&eyeRecommendationCount,eyeConfig.data());
        if(XR_SUCCEEDED(eyeRecommendationResult)&&eyeRecommendationCount==2){
            const auto& l=eyeConfig[0];const auto& r=eyeConfig[1];
            if(l.recommendedImageRectWidth==r.recommendedImageRectWidth&&l.recommendedImageRectHeight==r.recommendedImageRectHeight&&
               l.recommendedImageRectWidth<=l.maxImageRectWidth&&l.recommendedImageRectWidth<=r.maxImageRectWidth&&
               l.recommendedImageRectHeight<=l.maxImageRectHeight&&l.recommendedImageRectHeight<=r.maxImageRectHeight)
            {
                const auto percent=runtime::graphicsSettings.snapshot().startupResolutionPercent;
                const auto extent=runtime::graphicsExtent(l.recommendedImageRectWidth,l.recommendedImageRectHeight,
                    std::min(l.maxImageRectWidth,r.maxImageRectWidth),std::min(l.maxImageRectHeight,r.maxImageRectHeight),percent);
                if(extent)nativeEyeResolution.request(extent.width,extent.height);
            }
        }
#endif
        state="XR_SYSTEM_READY";
    }catch(...){state="XR_INITIALIZE_EXCEPTION";}
    __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","bootstrap=%s xr=%d",state,xrResult);
}
std::uint32_t gameCameraApplicationVersion(VkPhysicalDevice physical,VkDevice device) noexcept {
    const int saved=errno;
    struct RestoreErrno {int value;~RestoreErrno(){errno=value;}} restore{saved};
    try {
        std::lock_guard guard(mutex);
        XrGraphicsBindingVulkan2KHR binding{};
        if(!physical||!device||!bridge||!bridge->binding(binding)||
           binding.physicalDevice!=physical||binding.device!=device||
           !binding.instance||binding.instance!=bridge->gameInstance())return 0;
        // GameVulkanBridge::isGameInstance admits only the explicit 1.1
        // redemption request. A helper or flat device cannot reach this tuple.
        return VK_API_VERSION_1_1;
    }catch(...){return 0;}
}
bool gameXrRoute(diagnostics::TraceDispatch& api,PFN_vkGetInstanceProcAddr getProc) noexcept {
    std::lock_guard guard(mutex);
    if(bridge)return false;
    try {
        if(xr && system) {
            using runtime::proc;
            runtime::VulkanBootstrapDispatch dispatch{
                proc<PFN_xrGetVulkanGraphicsRequirements2KHR>(xr,"xrGetVulkanGraphicsRequirements2KHR"),
                proc<PFN_xrCreateVulkanInstanceKHR>(xr,"xrCreateVulkanInstanceKHR"),
                proc<PFN_xrGetVulkanGraphicsDevice2KHR>(xr,"xrGetVulkanGraphicsDevice2KHR"),
                proc<PFN_xrCreateVulkanDeviceKHR>(xr,"xrCreateVulkanDeviceKHR"),getProc,api.families,api.queue};
            bootstrap=std::make_unique<runtime::VulkanBootstrap>(xr,system,dispatch);
            auto result=bootstrap->queryRequirements();
            if(!result){xrResult=result.xr;state="XR_REQUIREMENTS_FAILED_FLAT";bootstrap.reset();}
            else state="XR_ROUTING_READY";
        }
        bridge=std::make_unique<runtime::GameVulkanBridge>(bootstrap.get(),runtime::GameVulkanDispatch{api.instance,api.device,api.destroyInstance,api.destroyDevice,api.queue});
        api.instance=instance;api.device=device;api.destroyInstance=destroyInstance;api.destroyDevice=destroyDevice;
        if(api.queue)api.queue=deviceQueue; // VulkanBootstrapDispatch above keeps the original
#ifdef RDRVR_GAME_SESSION
        originalIdle=api.idle;api.idle=deviceIdle;
#endif
        return true;
    }catch(...){state="XR_ROUTE_SETUP_FAILED_FLAT";bootstrap.reset();return false;}
}
std::string gameXrReport() {
    std::lock_guard guard(mutex);std::ostringstream out;
    XrGraphicsBindingVulkan2KHR binding{};const bool bound=bridge && bridge->binding(binding);
    const auto result=bridge?bridge->lastResult():runtime::BootstrapResult{};
    out<<"{\"initialization\":\""<<state<<"\",\"initializationXrResult\":"<<xrResult
       <<",\"routing\":\""<<(bound?"XR_VULKAN_BOUND":bridge?bridge->state():"NOT_ROUTED")<<"\",\"error\":\""<<runtime::bootstrapErrorName(result.error)
       <<"\",\"xrResult\":"<<result.xr<<",\"vkResult\":"<<result.vk<<",\"bindingReady\":"<<(bound?"true":"false")
       ;
#ifdef RDRVR_HEAD_CAMERA
    out<<",\"eyeRecommendationResult\":"<<eyeRecommendationResult<<",\"eyeRecommendationCount\":"<<eyeRecommendationCount
       <<",\"nativeEyeResolution\":"<<nativeEyeResolution.report()<<",\"nativeFramebufferExtent\":"<<nativeFramebufferExtent.report();
#endif
#ifdef RDRVR_GAME_SESSION
    out<<",\"sessionCreated\":"<<(sessionCreated?"true":"false")<<",\"frameLoopIterations\":"<<xrFrames<<",\"framesSubmitted\":"<<xrLayers
       <<",\"stereoPairs\":"<<xrPairs<<",\"sessionStatus\":\""<<sessionStatus.load()<<"\",\"sessionRunning\":"<<(sessionRunning?"true":"false")
       <<",\"workerActive\":"<<(workerActive?"true":"false")<<",\"sessionState\":"<<xrSessionState
       <<",\"gameDeviceIdleCalls\":"<<gameIdleCalls
       <<",\"layerFrames\":"<<xrLayers<<",\"theaterFrames\":"<<xrTheaterFrames
       <<",\"shouldNotRenderFrames\":"<<xrShouldNotRender<<",\"invalidViewFrames\":"<<xrInvalidViews
       <<",\"lastViewFlags\":"<<xrViewFlags<<",\"theater\":"<<theater.report();
#else
    out<<",\"sessionCreated\":false,\"framesSubmitted\":0";
#endif
#ifdef RDRVR_GAME_SESSION
    {std::lock_guard errorGuard(errorMutex);out<<",\"sessionError\":\"";
        for(unsigned char c:sessionError){if(c=='"' || c=='\\')out<<'\\';if(c>=32)out<<c;}
        out<<'"';}
#endif
    if(bound)out<<",\"queueFamily\":"<<binding.queueFamilyIndex<<",\"privateQueueIndex\":"<<binding.queueIndex;
    out<<",\"xrOptionalExtensions\":{\"convertTimespec\":"<<runtime::xrTimespecEnabled.load(std::memory_order_relaxed)
       <<",\"layerSettings\":"<<runtime::xrLayerSettingsEnabled.load(std::memory_order_relaxed)
       <<",\"autoLayerFilter\":"<<runtime::xrAutoLayerFilterEnabled.load(std::memory_order_relaxed);
    {const auto& d=runtime::xrDynres;constexpr auto r=std::memory_order_relaxed; // dynres01: XR_META_recommended_layer_resolution query (report only)
     out<<",\"dynres\":{\"enabled\":"<<runtime::xrDynresEnabled.load(r)<<",\"listed\":"<<d.listed.load(r)<<",\"prop\":"<<d.prop.load(r)<<",\"fn\":"<<d.fn.load(r)
        <<",\"calls\":"<<d.calls.load(r)<<",\"ok\":"<<d.ok.load(r)<<",\"fail\":"<<d.fail.load(r)<<",\"lastResult\":"<<d.lastResult.load(r)
        <<",\"valid\":"<<d.valid.load(r)<<",\"validFrames\":"<<d.validFrames.load(r)<<",\"w\":"<<d.w.load(r)<<",\"h\":"<<d.h.load(r)
        <<",\"rectW\":"<<d.rectW.load(r)<<",\"rectH\":"<<d.rectH.load(r)<<'}';}
    out<<",\"retryResult\":"<<runtime::xrOptionalExtensionsRetryResult.load(std::memory_order_relaxed)<<'}'
       <<",\"queueLayout\":"<<runtime::gameQueueReport();
#ifdef RDRVR_FRAGMENT_RATE
    out<<",\"fragmentRate\":"<<gameFragmentRateReport()<<",\"foveation\":"<<gameFoveationReport();
#endif
#ifdef RDRVR_CAMERA_BRIDGE
    out<<",\"camera\":"<<cameraBridge.report();
#endif
#ifdef RDRVR_STEREO_BRIDGE
    out<<",\"stereoTransfer\":"<<stereoMailbox.report()<<",\"worldStereoLayers\":"<<worldStereoLayers
       <<",\"passCapture\":"<<passCapture.report()<<",\"eyeViewportApi\":"<<(viewportApiReady?"true":"false")<<",\"clonedViewPairs\":"<<clonedViewPairs<<",\"cloneFailures\":"<<cloneFailures;
#endif
#ifdef RDRVR_PASS_RESOURCES
    out<<",\"treeTimers\":{\"suppressedBegins\":"<<replayUpdates.treeTimersSkipped(false)<<",\"suppressedEnds\":"<<replayUpdates.treeTimersSkipped(true)<<'}';
    out<<",\"passResources\":"<<passResources.report()<<",\"commands\":"<<commandResources.report()<<",\"passSourceReader\":"<<(passSourceReady?"true":"false")<<",\"engineTargetApi\":"<<(engineTargetApiReady?"true":"false")<<",\"engineFramebufferApi\":"<<(framebufferApiReady?"true":"false");
    out<<",\"hudObservation\":"<<hudObservation.report();
#endif
#ifdef RDRVR_PASS_RESOURCES
    out<<",\"lodDiagnostics\":"<<lodDiagnostics.report();
    out<<",\"visibilityCamera\":"<<visibilityBridge.report()<<",\"cullViewportApi\":"<<(cullViewportApiReady?"true":"false")<<",\"depthCopyApi\":"<<(depthCopyApiReady?"true":"false")<<",\"textureApi\":"<<(textureApiReady?"true":"false");
    {std::lock_guard lock(resourceReportMutex);out<<",\"retailResourceProbe\":"<<resourceReport<<",\"worldGlobalProbe\":"<<globalReport<<",\"worldDrawReport\":"<<retailDrawReport<<",\"worldCompletionReport\":"<<worldCompletionReport<<",\"worldRasterReport\":"<<worldRasterReport<<",\"primaryRasterReport\":"<<primaryRasterReport;
        out<<",\"resourceFailureCounts\":{";bool comma=false;for(const auto& [reason,count]:resourceFailureCounts){if(comma)out<<',';comma=true;out<<'"'<<reason<<"\":"<<count;}out<<"},\"resourceFailures\":[";
        for(unsigned i=0;i<resourceFailureSize;++i){if(i)out<<',';out<<resourceFailures[(resourceFailureNext+resourceFailures.size()-resourceFailureSize+i)%resourceFailures.size()];}out<<']';}
    out<<",\"targetBoundary\":"<<::rdrvr::engine::targetroute::report(); // Route failures, dropped draws, depth rejects
    out<<",\"worldFailureHistory\":"<<diagnostics::worldFailureHistory.report();
    out<<",\"deferredWorldBinding\":{\"flushed\":"<<deferredWorldFlushes<<",\"recovered\":"<<deferredWorldRecovered<<'}';
    out<<",\"worldBindingProbe\":\""<<bindingProbeStatus.load()<<"\",\"worldPassStage\":\""<<worldPassStatus.load()<<"\"";
    {std::lock_guard lock(worldRejectMutex);out<<",\"worldRejectedStages\":{";bool comma=false;
        for(const auto& [stage,count]:worldRejectCounts){if(comma)out<<',';comma=true;out<<'"'<<stage<<"\":"<<count;}out<<'}';}
    out<<",\"retailWorld\":{\"state\":\""<<worldStatus.load()<<"\",\"scenes\":"<<retailScenes
       <<",\"recorded\":"<<worldRecorded<<",\"skipped\":"<<worldSkipped<<",\"cancelled\":"<<worldCancelled<<",\"quarantined\":"<<worldQuarantined
       <<",\"drawApi\":"<<(retailDrawReady?"true":"false")<<",\"resourceApi\":"<<(retailResourcesReady?"true":"false")<<",\"globalsApi\":"<<(globalApiReady?"true":"false")<<"}";
    out<<",\"cullRouting\":{\"enabled\":"<<(worldCullingEnabled?"true":"false")<<",\"hooksReady\":"<<(cullHooksReady?"true":"false")
       <<",\"engineCullWrites\":"<<cullWrites<<",\"stockProbes\":"<<stockCullProbes<<",\"completed\":"<<cullCompletions<<",\"declined\":"<<cullDeclined
       <<",\"pending\":"<<(cullViewportApiReady.load(std::memory_order_acquire)?cullViewports->pending():0)<<"}";

#endif
#ifdef RDRVR_TOUCH_CONTROLS
#ifdef RDRVR_HEAD_CAMERA
    const auto earlyWorkPacket=earlyWorkPublication.read();
    out<<",\"staticStereoOcclusion\":{\"backend\":\"EARLY_MAIN\",\"state\":\""<<earlyState.load()
        <<"\",\"scenes\":"<<earlyScenes.load()<<",\"joinedScenes\":"<<earlyJoinedScenes.load()<<",\"completeScenes\":"<<earlyCompleteScenes.load()
        <<",\"skippedCalls\":"<<earlySkippedCalls.load()<<",\"sourceCalls\":"<<earlySourceCalls.load()<<",\"lastSources\":"<<earlyWorkPacket.sources
        <<",\"lastPermitted\":"<<earlyWorkPacket.permitted<<",\"lastSkipped\":[";
    for(unsigned i=0;i<4;++i){if(i)out<<',';out<<earlyWorkPacket.skipped[i];}out<<"],\"workPublication\":{\"stable\":"<<(earlyWorkPacket.stable?"true":"false")
        <<",\"sequenceBefore\":"<<earlyWorkPacket.sequenceBefore<<",\"sequenceAfter\":"<<earlyWorkPacket.sequenceAfter
        <<",\"scene\":"<<earlyWorkPacket.scene<<",\"pose\":"<<earlyWorkPacket.pose<<"},\"lastWork\":{";
    for(unsigned i=0;i<std::size(earlyWorkNames);++i){if(i)out<<',';out<<'"'<<earlyWorkNames[i]<<"\":"<<earlyWorkPacket.values[i];}out<<"}}";
    experimental::main_join35::write(out);
    {const auto c=nativeOcclusionVertexCapture.stats();const auto v=ownedStaticVertices.statistics();
        out<<",\"ownedStaticVertices\":{\"scopes\":"<<c.scopes<<",\"attempts\":"<<c.attempts
           <<",\"captured\":"<<c.captured<<",\"refused\":"<<c.refused<<",\"entries\":"<<v.entries
           <<",\"bytes\":"<<v.bytes<<",\"invalidations\":"<<v.invalidations<<",\"evictions\":"<<v.evictions<<'}';}
    {const auto c=nativeOcclusionVertexCapture.stats();const auto v=ownedStaticOccluders.statistics();
        out<<",\"ownedStaticOccluders\":{\"attempts\":"<<c.meshAttempts<<",\"captured\":"<<c.meshCaptured
           <<",\"refused\":"<<c.meshRefused<<",\"entries\":"<<v.entries<<",\"bytes\":"<<v.bytes
           <<",\"invalidations\":"<<v.invalidations<<",\"evictions\":"<<v.evictions
           <<",\"inputTriangles\":"<<v.inputTriangles<<",\"patches\":"<<v.patches<<",\"mergedTriangles\":"<<v.mergedTriangles<<'}';}

    {const auto s=coldOcclusionRegistry.statistics();
        out<<",\"coldOcclusionRegistry\":{\"deviceEpoch\":"<<s.deviceEpoch<<",\"shaders\":"<<s.shaders
           <<",\"livePipelines\":"<<s.livePipelines<<",\"retainedRecords\":"<<s.retainedRecords
           <<",\"declined\":"<<s.declined<<",\"shaderInspections\":"<<s.shaderInspections
           <<",\"unknownPipelines\":"<<s.unknownPipelines<<",\"certificateReasons\":{";
        for(unsigned i=0;i<s.reasons.size();++i){if(i)out<<',';
            out<<'"'<<StaticOcclusionCertificate::reasonName(StaticOcclusionCertificate::PipelineReason(i))<<"\":"<<s.reasons[i];}
        out<<"}}";}
    out<<",\"bodyDrawFilter\":{\"hooksReady\":"<<(bodyDrawReady?"true":"false")<<",\"inspected\":"<<bodyDrawFilter.inspected()
       <<",\"hidden\":"<<bodyDrawFilter.hidden()<<",\"hatsHidden\":"<<bodyDrawFilter.hats()
       <<",\"globalHat\":"<<(bodyDrawFilter.globalHat()?"true":"false")<<",\"hatsHiddenOutsideEyes\":"<<bodyDrawFilter.globalHats()
       <<",\"playerNodesOutsideEyes\":"<<bodyDrawFilter.probeNodes()<<",\"playerRecordsOutsideEyes\":"<<bodyDrawFilter.probeRecords()
       <<",\"playerNodesOverflow\":"<<bodyDrawFilter.probeOverflow()
       <<",\"headFill\":"<<(bodyDrawFilter.headFillActive()?"true":"false")<<",\"headSwaps\":"<<bodyDrawFilter.headSwaps()
       <<",\"headMisses\":"<<bodyDrawFilter.headMisses()<<",\"headAsIs\":"<<bodyDrawFilter.headAsIs()<<",\"errors\":"<<bodyDrawFilter.errors()<<"}";
#endif
#ifdef RDRVR_HEAD_CAMERA
    out<<",\"locomotion\":{\"state\":\""<<locomotionState.load()<<"\",\"mapped\":"<<locomotionMapped
       <<",\"native\":"<<locomotionNative<<",\"unavailable\":"<<locomotionUnavailable<<"}";
#ifdef RDRVR_NATIVE_MENUS
    out<<",\"snapTurn\":{\"enabled\":"<<(snapTurnReady.load(std::memory_order_acquire)?"true":"false")<<",\"state\":\""<<snapTurnState.load()
       <<"\",\"degrees\":30,\"nativeSetterCalls\":"<<snapTurnApplied<<",\"declined\":"<<snapTurnDeclined<<",\"smoothFrames\":"<<smoothTurnFrames<<"}";
#endif
#endif
    out<<",\"controlMap\":"<<::rdrvr::vr::controlmap::report();
    out<<",\"riding\":{\"samples\":"<<ridingSamples.load()<<",\"noHead\":"<<ridingNoHead.load()<<",\"notAttached\":"<<ridingNotAttached.load()
       <<",\"identity\":"<<ridingIdentity.load()<<",\"lastPlayerState\":"<<ridingLastState.load()
       <<",\"turns\":"<<ridingTurns.load()<<",\"turnDeclined\":"<<ridingTurnDeclined.load()<<",\"turnedDeg\":"<<ridingTurnedDeg.load()
       <<",\"recenters\":"<<ridingRecenters.load()<<",\"lastRecenterDeg\":"<<ridingRecenterDeg.load();
    {const unsigned n=ridingTraceNext.load(std::memory_order_relaxed),m=std::min<unsigned>(n,unsigned(ridingTrace.size()));out<<",\"trace\":[";
     for(unsigned k=0;k<m;++k){const auto& t=ridingTrace[(n-m+k)%ridingTrace.size()];if(k)out<<',';
        out<<'['<<std::round(t.speed*100)/100<<','<<std::round(t.dy*1000)/1000<<','<<std::round(t.lf*1000)/1000<<','<<std::round(t.ls*1000)/1000<<','
           <<std::round(t.up*1000)/1000<<','<<std::round(t.fwd*1000)/1000<<','<<std::round(t.bodyY*1000)/1000<<','<<std::round(t.eyeY*1000)/1000<<']';}
     out<<"]";}
    out<<"}";
    out<<",\"headCamera\":{\"enabled\":"
#ifdef RDRVR_HEAD_CAMERA
       <<"true"
#else
       <<"false"
#endif
       <<",\"state\":\""<<headCameraState.load()<<"\",\"samples\":"<<headCameraSamples<<",\"rejected\":"<<headCameraRejected
       <<",\"recordedPairs\":"<<headCameraRecorded<<"}";
    out<<",\"controllerAim\":{\"state\":\""<<aimState.load()<<"\",\"mapped\":"<<aimMapped
       <<",\"unavailable\":"<<aimUnavailable<<",\"weaponState\":\""<<weaponState.load()<<"\",\"localWeaponSamples\":"<<localWeaponSamples
       <<",\"lastWeaponClass\":\""<<weaponClassName(localWeaponClass.load())<<"\",\"lastWeaponTypeId\":"<<localWeaponTypeId
       <<",\"nativeContextState\":\""<<weaponContextState.load()<<"\",\"nativeContextFlags\":"<<weaponContextFlags
       <<",\"nativeContextSampleNs\":"<<weaponContextSampleNs
       <<",\"linkedActorReference\":"<<weaponLinkedGuid<<",\"gringoReference\":"<<weaponGringoGuid
       <<",\"lastWeaponActorGuid\":"<<localWeaponGuid<<",\"weaponInjection\":"<<(controllerTargetEnabled?"true":"false")
       <<",\"reticleProbeState\":\""<<reticleProbeState.load()<<"\",\"reticleUpdates\":"<<reticleUpdates
       <<",\"reticleProbeSamples\":"<<reticleProbeSamples<<",\"reticleProbeSampleNs\":"<<reticleProbeSampleNs
       <<",\"reticleProbeBlocked\":"<<(reticleProbeBlocked?"true":"false")<<",\"reticleProbeDistance\":"<<reticleProbeDistance
       <<",\"reticleHeadValid\":"<<(reticleHeadValid?"true":"false")<<",\"reticleHeadToRayOrigin\":"<<reticleHeadGap
       <<",\"reticleMuzzleProbeValid\":"<<(reticleMuzzleProbeValid?"true":"false")<<",\"reticleMuzzleBlocked\":"<<(reticleMuzzleBlocked?"true":"false")
       <<",\"reticleMuzzleProbeDistance\":"<<reticleMuzzleDistance<<",\"reticleMuzzleToTarget\":"<<reticleMuzzleToTarget
       <<",\"reticleAttachmentValid\":"<<(reticleAttachmentValid?"true":"false")<<",\"reticleAttachmentScale\":"<<reticleAttachmentScale
       <<",\"reticleOriginClamped\":"<<(reticleOriginClamped?"true":"false")<<",\"reticleRequestedReach\":"<<reticleRequestedReach
       <<",\"reticleSafeReach\":"<<reticleSafeReach<<"}";
    {std::lock_guard lock(targetAimMutex);const auto& t=lastAimTarget;
        out<<",\"controllerTarget\":{\"enabled\":"<<(controllerTargetEnabled?"true":"false")
           <<",\"state\":\""<<targetAimState.load()<<"\",\"prepared\":"<<targetAimPrepared<<",\"completed\":"<<targetAimCompleted
           <<",\"declined\":"<<targetAimDeclined<<",\"sampleNs\":"<<targetAimStamp<<",\"actorGuid\":"<<targetAimGuid
           <<",\"ignoredForeignCalls\":"<<targetAimIgnored
           <<",\"sceneId\":"<<t.ray.sceneId<<",\"controllerSequence\":"<<t.ray.controllerSequence
           <<",\"trackingEpoch\":"<<t.ray.trackingEpoch<<",\"referenceSpaceEpoch\":"<<t.ray.referenceSpaceEpoch
           <<",\"point\":["<<t.point.x<<','<<t.point.y<<','<<t.point.z<<"],\"distance\":"<<t.distance
           <<",\"blocked\":"<<(t.blocked?"true":"false")<<",\"wallClamped\":"<<(t.wallClamped?"true":"false")
           <<",\"reticleRecordedPairs\":"<<targetReticlePairs
           <<",\"attachmentInjection\":"<<(controllerTargetEnabled?"true":"false")
           <<",\"projectileInjection\":false,\"muzzleSource\":\""<<(targetAimTracked?"TRACKED_NATIVE_ATTACHMENT":"NATIVE_ANIMATION")<<"\"}";
        out<<",\"nativeReticleVisibility\":{\"ready\":"<<(nativeReticleReady.load()?"true":"false")
           <<",\"sampleNs\":"<<nativeReticleFrame.stamp<<",\"revision\":"<<nativeReticleFrame.revision
           <<",\"actorGuid\":"<<nativeReticleFrame.state.actorGuid<<",\"prevention\":"<<nativeReticleFrame.state.prevention
           <<",\"alpha\":"<<nativeReticleFrame.state.alpha<<",\"visible\":"<<(nativeReticleFrame.state.visible?"true":"false")
           <<",\"samples\":"<<nativeReticleSamples<<",\"declined\":"<<nativeReticleDeclined<<",\"hidden\":"<<nativeReticleHidden<<"}";
    }
    {std::lock_guard lock(attachmentMutex);
        out<<",\"controllerAttachment\":{\"enabled\":"<<(controllerTargetEnabled?"true":"false")
            <<",\"animatedLatest\":"<<(attachmentAnimated?"true":"false")
            <<",\"animatedModelReady\":"<<(weaponModelReady.load()?"true":"false")
            <<",\"animatedPrepared\":"<<animatedAttachmentPrepared.load()<<",\"animatedCompleted\":"<<animatedAttachmentCompleted.load()
           <<",\"state\":\""<<attachmentState.load()<<"\",\"prepared\":"<<attachmentPrepared<<",\"completed\":"<<attachmentCompleted
           <<",\"declined\":"<<attachmentDeclined<<",\"continuedPendingFire\":"<<attachmentPendingFire<<",\"wallClamped\":"<<attachmentClamped
           <<",\"sampleNs\":"<<attachmentStamp<<",\"actorGuid\":"<<attachmentGuid
           <<",\"muzzle\":["<<attachmentMuzzle[0]<<','<<attachmentMuzzle[1]<<','<<attachmentMuzzle[2]<<"]}";
    }
    {std::lock_guard lock(gripPreviewMutex);const auto& p=gripPreview;
        out<<",\"trackedWeaponPreview\":{\"weaponInjection\":false,\"poseValid\":"<<(p.poseValid?"true":"false")
           <<",\"probeValid\":"<<(p.probeValid?"true":"false")<<",\"sampleNs\":"<<p.stamp<<",\"sceneId\":"<<p.scene
           <<",\"controllerSequence\":"<<p.sequence<<",\"trackingEpoch\":"<<p.tracking<<",\"referenceSpaceEpoch\":"<<p.reference
           <<",\"actorGuid\":"<<p.actorGuid<<",\"weaponTypeId\":"<<p.type<<",\"wallClamped\":"<<(p.clamped?"true":"false")
           <<",\"blocked\":"<<(p.blocked?"true":"false")<<",\"distance\":"<<p.distance;
        const auto vector=[&](const char* name,const runtime::Vec3& v){out<<",\""<<name<<"\":["<<v[0]<<','<<v[1]<<','<<v[2]<<']';};
        vector("attachment",p.grip);vector("muzzle",p.muzzle);vector("direction",p.direction);vector("target",p.target);out<<'}';
    }
    {std::lock_guard lock(reticleUiMutex);const auto& r=latestReticleUi;
        out<<",\"reticleUi\":{\"readOnly\":true,\"ready\":"<<(reticleUiReady.load()?"true":"false")
           <<",\"calls\":"<<reticleUiCalls.load()<<",\"captured\":"<<reticleUiCaptured.load()
           <<",\"completed\":"<<reticleUiCompleted.load()<<",\"declined\":"<<reticleUiDeclined.load()
           <<",\"sampleNs\":"<<r.stamp<<",\"completedNs\":"<<r.completedNs<<",\"completionRead\":"<<(r.completed?"true":"false");
        const auto sample=[&](const char* name,const NativeReticleUiSample& v){
            out<<",\""<<name<<"\":{\"alpha\":"<<v.alpha<<",\"previousAlpha\":"<<v.previousAlpha
               <<",\"accuracy\":"<<v.accuracy<<",\"x\":"<<v.x<<",\"y\":"<<v.y
               <<",\"value\":"<<v.value<<",\"frame\":"<<v.frame<<",\"hasMovie\":"<<(v.hasMovie?"true":"false")<<'}';};
        sample("before",r.before);sample("after",r.after);out<<'}';
    }
    {std::lock_guard lock(launchMutex);
        out<<",\"projectileLaunch\":{\"readOnly\":true,\"ready\":"<<(projectileLaunchReady.load()?"true":"false")
           <<",\"captured\":"<<launchCaptured.load()<<",\"completed\":"<<launchCompleted.load()
           <<",\"declined\":"<<launchDeclined.load()<<",\"overwritten\":"<<launchOverwritten.load()<<",\"records\":[";
        const auto vector=[&](const char* name,const runtime::Vec3& v){out<<",\""<<name<<"\":["<<v[0]<<','<<v[1]<<','<<v[2]<<']';};
        for(unsigned i=0;i<launchUsed;++i){const auto& r=launchRecords[(launchNext+launchRecords.size()-launchUsed+i)%launchRecords.size()];const auto& p=r.input;
            if(i)out<<',';out<<"{\"sampleNs\":"<<r.stamp<<",\"completedNs\":"<<r.completedNs<<",\"fireSerial\":"<<r.fireSerial
               <<",\"revision\":"<<r.revision<<",\"actorGuid\":"<<p.actorGuid<<",\"weaponType\":"<<p.weaponType
               <<",\"scalar\":"<<p.scalar<<",\"nativeFlag\":"<<(p.nativeFlag?"true":"false")<<",\"finalFlag\":"<<(p.finalFlag?"true":"false")
               <<",\"completionRead\":"<<(r.completionRead?"true":"false")<<",\"argumentsCopied\":"<<(r.argumentsCopied?"true":"false")
               <<",\"ownerStillCurrent\":"<<(r.ownerStillCurrent?"true":"false")<<",\"nativeFlags\":"<<unsigned(r.nativeFlags)
               <<",\"contextAvailable\":"<<(r.contextAvailable?"true":"false")<<",\"contextFlags\":"<<r.contextFlags
               <<",\"controllerTargetAvailable\":"<<(r.controllerTargetAvailable?"true":"false")<<",\"controllerTargetTracked\":"<<(r.controllerTargetTracked?"true":"false")
               <<",\"world\":[";
            for(unsigned j=0;j<p.world.size();++j){if(j)out<<',';out<<p.world[j];}out<<']';
            vector("velocity",p.velocity);vector("muzzle",p.muzzle);vector("delta",p.delta);vector("controllerTarget",r.controllerTarget);out<<'}';
        }out<<"]}";
    }
    {std::lock_guard lock(skeletonAuditMutex);out<<",\"weaponSkeletonAudit\":{\"readOnly\":true,\"maxBones\":"<<SkeletonPoseAudit::maxBones<<",\"phases\":[";
        for(unsigned i=0;i<skeletonAuditRecords.size();++i){const auto& r=skeletonAuditRecords[i];const auto& p=r.evidence.pose;
            if(i)out<<',';out<<"{\"phase\":"<<i<<",\"sampleNs\":"<<r.stamp<<",\"actorGuid\":"<<r.actorGuid
                <<",\"samples\":"<<r.samples<<",\"declined\":"<<r.declined<<",\"coherentSamples\":"<<r.coherent
                <<",\"flags\":"<<r.evidence.flags<<",\"modelGuid\":"<<r.evidence.modelGuid<<",\"bones\":"<<p.bones
                <<",\"rootMatchesModel\":"<<(p.rootMatchesModel?"true":"false")<<",\"globalsMatchLocals\":"<<(p.globalsMatchLocals?"true":"false")
                <<",\"maxBasisError\":"<<p.maxBasisError<<",\"maxPositionError\":"<<p.maxPositionError<<",\"rootModelDistance\":"<<p.rootModelDistance<<'}';}
        out<<"]}";
    }
    const auto weaponCalls=weaponCallTrace.snapshot();
    out<<",\"weaponCallTrace\":{\"schema\":5,\"associationBound\":"<<(weaponCalls.bound?"true":"false")
       <<",\"actorGuid\":"<<weaponCalls.actorGuid<<",\"weaponTypeId\":"<<weaponCalls.weaponTypeId
       <<",\"overwritten\":"<<weaponCalls.overwritten<<",\"ignored\":"<<weaponCalls.ignored
       <<",\"columns\":[\"phase\",\"ns\",\"thread\",\"reticleOrdinal\",\"targetGuid\",\"mode\",\"targetIndex\",\"instance\",\"part\",\"flags\",\"nativeScalar\",\"nativeTimerOrdinal\",\"projectileCount\",\"fireMode\",\"fireValue\"],\"records\":[";
    for(unsigned i=0;i<weaponCalls.used;++i){const auto& r=weaponCalls.records[i];if(i)out<<",";
        out<<"["<<unsigned(r.phase)<<","<<r.stamp<<","<<r.thread<<","<<r.reticleOrdinal<<","<<r.targetGuid<<","<<r.mode<<","<<r.targetIndex
           <<","<<r.instance<<","<<r.part<<","<<r.flags<<","<<diagnostics::JsonNumber{r.nativeScalar}<<","<<r.nativeTimerOrdinal<<","<<r.projectileCount<<","<<r.fireMode<<","<<r.fireValue<<"]";
    }
    out<<"]}";
    {std::lock_guard guard(poseGeometryMutex);out<<",\"weaponPoseGeometry\":{\"begun\":"<<poseGeometryBegun<<",\"completed\":"<<poseGeometryCompleted
       <<",\"declined\":"<<poseGeometryDeclined<<",\"animated\":"<<poseGeometryAnimated<<",\"sampleNs\":"<<poseGeometrySampleNs
       <<",\"localOffsetDelta\":"<<poseGeometryLast.offsetDistance<<",\"localRotationDeltaDegrees\":"<<poseGeometryLast.rotationDegrees<<",\"weaponInjection\":false}";}
    out<<",\"nativeInput\":{\"state\":\""<<nativePad.status()<<"\",\"updates\":"<<nativePad.updates()
       <<",\"neutralUpdates\":"<<nativePad.neutralUpdates()<<",\"created\":"<<nativePad.created()
       <<",\"filterRejected\":"<<nativePad.filterRejected()<<",\"controlsRejected\":"<<nativePad.controlsRejected()
       <<",\"delivery\":"<<nativePad.deliveryReport()<<"}";
    out<<",\"haptics\":{\"source\":\"native-actuator-pair\",\"published\":"<<haptics.published()
       <<",\"rejected\":"<<haptics.rejected()<<",\"declined\":"<<hapticDeclined<<",\"applied\":"<<hapticApplied
       <<",\"stopped\":"<<hapticStopped<<",\"failed\":"<<hapticFailed<<",\"maxAgeNs\":"<<runtime::HapticMailbox::maxAgeNs
       <<",\"pulseNs\":"<<runtime::HapticOutput::pulseNs<<"}";
#endif
#ifdef RDRVR_NATIVE_MENUS
    const auto presentationNow=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto effective=presentation.snapshot(presentationNow);
    out<<",\"presentation\":{\"worldAllowed\":"<<(effective.world()?"true":"false")
       <<",\"revision\":"<<effective.revision<<",\"observedNs\":"<<effective.publishedNs<<",\"sampleNs\":"<<presentationNow<<"}";
    out<<",\"nativeUi\":{\"state\":\""<<nativeUi.status()<<"\",\"observations\":"<<nativeUi.observations()
       <<",\"changes\":"<<nativeUi.changes()<<",\"rejected\":"<<nativeUi.rejected()<<",\"manualTheater\":"<<(presentation.manualTheater()?"true":"false")<<"}";
    out<<",\"hudLayers\":"<<xrHudLayers<<",\"hudTransfer\":"<<hudQuad.report()<<",\"hudCapture\":"<<hudCapture.report()
       <<",\"hudFrameStatus\":\""<<hudFrameStatus.load()<<"\",\"hudMatchedWorldFrames\":"<<hudMatchedWorldFrames
       <<",\"hudUnmatchedFrames\":"<<hudUnmatchedFrames<<",\"hudLastMatchedFrame\":"<<hudLastMatchedFrame;
#ifdef RDRVR_NATIVE_HUD
    out<<",\"nativeHudEnabled\":true,\"hudNativeBegun\":"<<hudNativeBegun<<",\"hudNativeRecorded\":"<<hudNativeRecorded
       <<",\"hudNativeCancelled\":"<<hudNativeCancelled<<",\"hudAlphaNormalized\":"<<hudAlphaNormalized
       <<",\"hudAlphaUnsupported\":"<<hudAlphaUnsupported<<",\"hudTemporaryTargets\":"<<hudTemporaryTargets<<",\"hudMappedTargets\":"<<hudMappedTargets;
#else
    out<<",\"nativeHudEnabled\":false";
#endif
#endif
    out<<",\"performance\":"<<runtime::performanceTelemetry.report();
    out<<",\"framePacing\":"<<runtime::framePacer.report();
#ifdef RDRVR_STEREO_BRIDGE
    {const auto control=runtime::spaceWarpControl.report();out<<",\"spaceWarp\":"<<control.substr(0,control.size()-1)<<",\"gpu\":"<<stereoMailbox.spaceWarpReport()<<'}';}
#else
    out<<",\"spaceWarp\":"<<runtime::spaceWarpControl.report();
#endif
#ifdef RDRVR_HEAD_CAMERA
    out<<",\"renderAblation54\":"<<experimental::renderAblation54.report();
#endif
    out<<",\"wallTimings\":"<<diagnostics::frameTimings.report();
    out<<",\"threadCpuTimings\":"<<diagnostics::threadCpuTimings.report();
    #ifdef RDRVR_BENCH_CONTROL
    out<<",\"benchInput\":"<<runtime::benchMenuInput.report();
    #endif
    out<<",\"renderThreadLoad\":"<<diagnostics::renderThreadLoad.report();
    out<<",\"drawStageTimings\":"<<diagnostics::drawStageTimings.report();
    out<<",\"pipelinePassHashMemo\":"<<pipelinePassHashMemo.report();
    out<<",\"nativeDescriptorReuse\":"<<nativeDescriptorReuse.report();
#ifdef RDRVR_HEAD_CAMERA
    out<<",\"dynamicUniformDescriptors\":"<<dynamicUniformDescriptors.report();
    out<<",\"nativeVertexStreamMerge58\":"<<nativeVertexStreamMerge58.report();
    out<<",\"nativeModelProbeReady69\":"<<(experimental::nativeModelProbeReady69.load()?"true":"false")<<",\"nativeModelProbe69\":"<<experimental::nativeModelProbe69.report();
    out<<",\"instanceFrequencyReady67\":"<<(experimental::instanceFrequencyReady67.load()?"true":"false")<<",\"instanceFrequency67\":"<<experimental::instanceFrequency67.report();
    out<<",\"nativeFramePacing\":"<<nativeFramePacing.report();
#endif
    out<<",\"unusedDrawLabel\":{\"configured\":"<<(unusedDrawLabel.ready()?"true":"false")
       <<",\"skipped\":"<<unusedDrawLabel.skipped()<<'}';
    out<<",\"replayRenderPhase\":{\"ready\":"<<(replayPhaseReady.load()?"true":"false")
       <<",\"pairs\":"<<replayPhasePairs.load()<<",\"faults\":"<<replayPhaseFaults.load()<<'}';
    out<<",\"grassVisibility\":{\"ready\":"<<(grassVisibility.ready()?"true":"false")
       <<",\"heldCalls\":"<<grassVisibility.heldCalls()<<",\"cleanups\":"<<grassVisibility.cleanups()
       <<",\"passthrough\":"<<grassVisibility.passthroughCalls()<<",\"faults\":"<<grassVisibility.faults()<<'}';
#ifdef RDRVR_PASS_RESOURCES
    out<<",\"sameEyeFramebufferReuses\":"<<sameEyeFramebufferReuses.load(std::memory_order_relaxed);
    out<<",\"eyeShaftRouting\":{\"qualified\":"<<eyeShaft.qualified()<<",\"routed\":"<<eyeShaft.routed()
       <<",\"declined\":"<<eyeShaft.declined()<<",\"nested\":"<<eyeShaft.nested()<<'}';
    out<<",\"primaryEye\":{\"installed\":"<<primaryHooksReady.load()<<",\"started\":"<<primaryEye.started()
        <<",\"completed\":"<<primaryEye.completed()<<",\"reusedFrames\":"<<primaryFrames.load()<<",\"fallbackFrames\":"<<primaryFallbacks.load()
        <<",\"stereoReplayStages\":"<<stereoReplayedStages.load()<<",\"stereoFallbackStages\":"<<stereoFallbackStages.load()<<",\"stereoFaultedStages\":"<<stereoFaultedStages.load()
        <<",\"stereoFramesMapped\":"<<stereoFramesMapped.load()<<",\"stereoOverrideFrames\":"<<stereoOverrideFrames.load()<<",\"stereoRelClipDiff\":"<<stereoRelClipDiff.load()<<",\"stereoOwnerSwaps\":"<<stereoOwnerSwaps.load()<<",\"stereoOverrideDupes\":"<<stereoOverrideDupes.load()<<",\"stereoEyeRotationDiff\":"<<stereoEyeRotationDiff.load()<<",\"stereoRightMidFrameDiff\":"<<stereoRightMidFrameDiff.load()
        <<",\"stereoCloneFlaggedStages\":"<<stereoCloneFlaggedStages.load()
        <<",\"waterRebuildsAvoided\":"<<waterRebuildsAvoided.load()<<",\"eyeTargetRebuilds\":"<<eyeTargetRebuilds.load()<<",\"eyeTargetRebuildJobs\":"<<eyeTargetRebuildJobs.load()<<",\"eyeTargetRebuildFrame\":"<<eyeTargetRebuildFrame.load()
        <<",\"eyeTargetRebuildFrom\":"<<eyeTargetRebuildFrom.load()<<",\"eyeTargetRebuildTo\":"<<eyeTargetRebuildTo.load()
        <<",\"declined\":"<<primaryEye.declined()<<",\"faults\":"<<primaryEye.faults()<<",\"restoreFailures\":"<<primaryEye.restoreFailures()<<'}';
    out<<",\"primaryBodySubmission\":{\"installedMask\":"<<nativeDrawSuppression.installedMask()<<",\"scopes\":"<<nativeDrawSuppression.scopes()
        <<",\"skippedDraws\":"<<nativeDrawSuppression.skipped()<<",\"rejected\":"<<nativeDrawSuppression.rejected()<<'}';
#endif
    out<<",\"worldHistory\":"<<diagnostics::worldHistory.report()<<",\"stereoHistory\":"<<diagnostics::stereoHistory.report()<<",\"presentHistory\":"<<diagnostics::presentHistory.report();
    out<<'}';return out.str();
}
}
