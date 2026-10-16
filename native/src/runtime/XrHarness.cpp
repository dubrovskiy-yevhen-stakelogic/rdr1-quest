#include "engine/PipelinePassHashMemo.h"
#include "engine/PrimaryEyeBoundary.h"
#include "engine/NativeDescriptorReuse.h"
#ifdef RDRVR_HEAD_CAMERA
#include "engine/DynamicUniformDescriptors.h"
#include "engine/NativeFramePacing.h"
#include "NativeEarlyTelemetry.h"
#endif
#include "diagnostics/DrawStageTimings.h"
#include "diagnostics/ThreadLoad.h"
#include "FragmentRateTelemetry.h"
#ifdef RDRVR_BENCH_CONTROL
#include "runtime/BenchMenuInput.h"
#endif
// Theater anchor/display-validity adapted from MiamiVR Quest xr_vulkan_session.cpp.
// Copyright (c) 2026 the MiamiVR Quest port contributors. MIT; see MiamiVR-Quest-LICENSE.txt.
#include "XrHarness.h"
#include "vr/HandsPass.h"  // HANDS: XR-frame keyed controller pose ring (pushPose)
#include "vr/VrHolsters.h" // HOLSTERS: menu freeze note + VR haptic pulses (VrHaptics.h)
#include "vr/VrMelee.h"    // MELEE: XR-rate controller tape (pushSample)
#include "vr/VrTeleportLink.h" // TELEPORT: BLINK colour scale (fade command / word / pair id, watchdog)
#include "vr/VrCutscene.h" // CUTSCENES: an engaged stereo cutscene shows the stereo layer like World
#include "vr/VrHud.h"         // HUD: head / wrist quads of the game's HUD
#include "vr/VrWelcome.h"      // WELCOME: first-steps message on the menu panel
#include "vr/VrInputRec.h"     // INPUT RECORD / REPLAY (debug.rdrvr.input.record / .replay): hooks after every OpenXR input call
#ifdef RDRVR_HEAD_CAMERA
#endif
#include "GraphicsResolution.h"
#include "diagnostics/FrameTimings.h"
#include "diagnostics/ThreadCpuScope.h"
#include "diagnostics/PerformanceProfile.h"
#include <cstdio>
#include "diagnostics/PerformanceProfile.h"
#include "GameTheater.h"
#include "FramePacing.h"
#include "HeatWarning.h" // heat01: OVERHEAT WARNING (red lines on the stats quad)
#include "contracts/SubmissionPolicy.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <chrono>
#include <atomic>
#include <ctime> // struct timespec (XR_KHR_convert_timespec_time)
namespace rdrvr::runtime {
namespace {
// World, or a Cutscene / CameraTransition presentation whose revision the render thread engaged as a stereo cutscene
// (vr/VrCutscene.h). Read fresh each XR frame; the gate's own change handling (retire / re-anchor) is untouched.
bool stereoCutscene(const PresentationMailbox* m) noexcept {
    if(!m)return false;
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto st=m->snapshot(now);
    return (st.context==PresentationContext::Cutscene||st.context==PresentationContext::CameraTransition||
            (st.context==PresentationContext::CameraOverride&&::rdrvr::vr::cutscene::overrideLever.load(std::memory_order_relaxed)))&&
           ::rdrvr::vr::cutscene::engaged(st.revision,now);
}
// pdtMono of the XR frame whose predictedDisplayTime locateHands locates (XR thread; kept outside the
// mirror XrHarness.h). -1 / 0 = no conversion.
std::atomic<std::int64_t> xrPoseMonoNs{0};
void configureThreads(XrPerformance& performance,XrInstance instance,XrSession session,bool supported){
    if(!supported)return;
    PFN_xrSetAndroidApplicationThreadKHR setter{};
    if(XR_FAILED(xrGetInstanceProcAddr(instance,"xrSetAndroidApplicationThreadKHR",reinterpret_cast<PFN_xrVoidFunction*>(&setter)))||!setter)return;
    performance.threads().configure([setter,session](unsigned role,std::uint32_t tid){
        return setter(session,role?XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR:XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR,tid);
    });
}
// Synthetic head motion (debug.rdrvr.stand.motion "yawDeg,yawPeriodS,pitchDeg,pitchPeriodS", e.g.
// "60,8,12,5.3"; unset/0 = off; re-read at 1 Hz on the XR thread): a sinusoidal yaw (about world
// up, around the eye midpoint) and pitch (local) is applied to the eye poses the game renders with
// (and so to the submitted layer poses), for testing with a headset that lies still. Never set in
// normal play.
namespace standmotion {
float yawA{},yawP{8.f},pitchA{},pitchP{5.3f};std::uint64_t nextRead{},t0{};bool on{};
Quat mul(const Quat& a,const Quat& b){return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
                                              a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};}
void apply(std::array<Pose,2>& poses,std::uint64_t now){
    if(now>=nextRead){nextRead=now+1'000'000'000ull;
        char v[PROP_VALUE_MAX]{};float a=0,b=8,c=0,d=5.3f;
        if(__system_property_get("debug.rdrvr.stand.motion",v)>0)std::sscanf(v,"%f,%f,%f,%f",&a,&b,&c,&d);
        const bool want=(a>0.f||c>0.f)&&b>0.5f&&d>0.5f;if(want&&!on)t0=now;on=want;yawA=a;yawP=b;pitchA=c;pitchP=d;}
    if(!on)return;
    const float t=float(double(now-t0)*1e-9),k=3.14159265f/180.f;
    const float yaw=yawA*k*std::sin(2.f*3.14159265f*t/yawP),pitch=pitchA*k*std::sin(2.f*3.14159265f*t/pitchP);
    const Quat qy{0,std::sin(yaw/2),0,std::cos(yaw/2)},qx{std::sin(pitch/2),0,0,std::cos(pitch/2)};
    const Vec3 c{(poses[0].position[0]+poses[1].position[0])/2,(poses[0].position[1]+poses[1].position[1])/2,(poses[0].position[2]+poses[1].position[2])/2};
    for(auto& p:poses){
        p.orientation=mul(qy,mul(p.orientation,qx));
        const auto r=rotate(qy,{p.position[0]-c[0],p.position[1]-c[1],p.position[2]-c[2]});
        p.position={c[0]+r[0],c[1]+r[1],c[2]+r[2]};}
}
}
Pose poseOf(const XrPosef& p,bool valid) {
    Pose out{{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w},
             {p.position.x,p.position.y,p.position.z},valid,valid};
    if(!validPose(out)) out.orientationValid=out.positionValid=false;
    return out;
}
XrPosef identityPose() { XrPosef p{}; p.orientation.w=1; return p; }
// Rigid pose algebra (unit quaternions) for pose-compensated AppSW re-submits.
XrQuaternionf quatMul(const XrQuaternionf& a,const XrQuaternionf& b) {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y, a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x, a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w, a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
XrVector3f quatRotate(const XrQuaternionf& q,const XrVector3f& v) {
    const XrQuaternionf p{v.x,v.y,v.z,0.f},c{-q.x,-q.y,-q.z,q.w};const auto r=quatMul(quatMul(q,p),c);return {r.x,r.y,r.z};
}
XrPosef poseMul(const XrPosef& a,const XrPosef& b) { // a * b (apply b, then a)
    XrPosef o{};o.orientation=quatMul(a.orientation,b.orientation);const auto t=quatRotate(a.orientation,b.position);
    o.position={a.position.x+t.x,a.position.y+t.y,a.position.z+t.z};return o;
}
XrPosef poseInverse(const XrPosef& a) {
    XrPosef o{};o.orientation={-a.orientation.x,-a.orientation.y,-a.orientation.z,a.orientation.w};
    const auto t=quatRotate(o.orientation,a.position);o.position={-t.x,-t.y,-t.z};return o;
}
// predictedDisplayTime -> CLOCK_MONOTONIC ns through XR_KHR_convert_timespec_time (enabled by GameXr,
// runtime::xrTimespecEnabled). Own typedef: the vendored declaration needs XR_USE_TIMESPEC, which the build does not define.
// Returns -1 when the extension is not enabled, 0 when the conversion failed. XR thread only (one harness).
struct TimeConvert {
    using Fn=XrResult(XRAPI_PTR*)(XrInstance,XrTime,struct timespec*);
    XrInstance instance{};Fn fn{};bool resolved{};
    std::int64_t monotonic(XrInstance inst,XrTime time) noexcept {
        if(xrTimespecEnabled.load(std::memory_order_relaxed)!=1||!inst||time<=0)return -1;
        if(!resolved||inst!=instance){
            instance=inst;resolved=true;fn=nullptr;PFN_xrVoidFunction f{};
            if(XR_SUCCEEDED(xrGetInstanceProcAddr(inst,"xrConvertTimeToTimespecTimeKHR",&f))&&f)fn=reinterpret_cast<Fn>(f);
        }
        if(!fn)return 0;
        struct timespec ts{};
        if(XR_FAILED(fn(inst,time,&ts)))return 0;
        return std::int64_t(ts.tv_sec)*1'000'000'000ll+std::int64_t(ts.tv_nsec);
    }
};
TimeConvert timeConvert;
// dynres01: XR_META_recommended_layer_resolution (enabled by GameXr G5, runtime::xrDynresEnabled; debug.rdrvr.dynres 0 = not requested).
// Hypothesis: on Quest 3/3S the runtime grants GPU levels 5/6 on battery only to "dynamic resolution" apps -> query the recommendation
// for the world projection layer once per XR frame that submits it and only REPORT it (runtime::xrDynres); never touches rendering.
struct DynresQuery {
    PFN_xrGetRecommendedLayerResolutionMETA fn{};bool logged{};
    void resolve(XrInstance inst) noexcept {
        fn=nullptr;if(xrDynresEnabled.load(std::memory_order_relaxed)!=1||!inst)return;
        PFN_xrVoidFunction f{};
        if(XR_SUCCEEDED(xrGetInstanceProcAddr(inst,"xrGetRecommendedLayerResolutionMETA",&f))&&f)fn=reinterpret_cast<PFN_xrGetRecommendedLayerResolutionMETA>(f);
        xrDynres.fn.store(fn?1:0,std::memory_order_relaxed);
        __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","dynres01 xrGetRecommendedLayerResolutionMETA %s",fn?"resolved":"MISSING");
    }
    void query(XrSession session,const XrCompositionLayerProjection& layer,XrTime time) noexcept {
        if(!fn||!session||time<=0)return;
        constexpr auto r=std::memory_order_relaxed;auto& t=xrDynres;
        XrRecommendedLayerResolutionGetInfoMETA info{XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_GET_INFO_META};
        info.layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);info.predictedDisplayTime=time;
        XrRecommendedLayerResolutionMETA out{XR_TYPE_RECOMMENDED_LAYER_RESOLUTION_META};
        const XrResult result=fn(session,&info,&out);
        t.calls.fetch_add(1,r);t.lastResult.store(int(result),r);
        if(layer.viewCount&&layer.views){const auto& e=layer.views[0].subImage.imageRect.extent;t.rectW.store(e.width,r);t.rectH.store(e.height,r);}
        if(XR_FAILED(result))t.fail.fetch_add(1,r);
        else{t.ok.fetch_add(1,r);t.valid.store(out.isValid?1:0,r);if(out.isValid)t.validFrames.fetch_add(1,r);
             t.w.store(out.recommendedImageDimensions.width,r);t.h.store(out.recommendedImageDimensions.height,r);}
        if(!logged){logged=true;__android_log_print(ANDROID_LOG_INFO,"RdrVrXR","dynres01 first query result=%d valid=%u recommended=%dx%d",
            int(result),unsigned(out.isValid),out.recommendedImageDimensions.width,out.recommendedImageDimensions.height);}
    }
};
DynresQuery dynresQuery;
// Per-XR-frame state that XrHarness.h does not hold (one harness per process; XR thread only).
struct PresentState {std::uint64_t lastPair{};bool appSwOverBudget{};};
PresentState presentState;
std::uint32_t toUs(std::uint64_t ns) noexcept {return std::uint32_t(std::min<std::uint64_t>(ns/1000,UINT32_MAX));}
bool validFlags(XrSpaceLocationFlags flags) {
    constexpr auto required=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT|
                            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    return (flags&required)==required;
}
// INPUT RECORD / REPLAY glue (VrInputRec.h holds no OpenXR types). inputFocus = the session focus the input gates use this XR frame
// (focused_, or the recorded focus while a replay plays): tracking gate, xrSyncActions gate, ControllerSample::focused. XR thread only.
bool inputFocus{};
void poseFloats(const XrPosef& p,float* o){o[0]=p.orientation.x;o[1]=p.orientation.y;o[2]=p.orientation.z;o[3]=p.orientation.w;o[4]=p.position.x;o[5]=p.position.y;o[6]=p.position.z;}
XrPosef floatsPose(const float* o){XrPosef p{};p.orientation={o[0],o[1],o[2],o[3]};p.position={o[4],o[5],o[6]};return p;}
// xrLocateViews result -> recorder; a playing replay overwrites flags / count / poses / fovs.
void viewsHook(XrViewState& state,std::uint32_t& count,std::array<XrView,2>& views){
    float eye[2][7],fov[2][4];std::uint32_t flags=std::uint32_t(state.viewStateFlags);
    for(unsigned i=0;i<2;++i){poseFloats(views[i].pose,eye[i]);const auto& f=views[i].fov;fov[i][0]=f.angleLeft;fov[i][1]=f.angleRight;fov[i][2]=f.angleUp;fov[i][3]=f.angleDown;}
    if(!::rdrvr::vr::inputrec::views(flags,count,eye,fov))return;
    state.viewStateFlags=flags;
    for(unsigned i=0;i<2;++i){views[i].pose=floatsPose(eye[i]);views[i].fov={fov[i][0],fov[i][1],fov[i][2],fov[i][3]};}
}
}
XrPath XrHarness::path(const char* text) { XrPath p{}; checkXr(xrStringToPath(instance_,text,&p),text); return p; }
void XrHarness::initialize(JavaVM* vm,jobject activity) {
    auto init=proc<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE,"xrInitializeLoaderKHR");
    XrLoaderInitInfoAndroidKHR loader{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR}; loader.applicationVM=vm; loader.applicationContext=activity;
    checkXr(init(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader)),"Android loader init");
    std::uint32_t n{}; checkXr(xrEnumerateInstanceExtensionProperties(nullptr,0,&n,nullptr),"extension count");
    std::vector<XrExtensionProperties> available(n,{XR_TYPE_EXTENSION_PROPERTIES});
    checkXr(xrEnumerateInstanceExtensionProperties(nullptr,n,&n,available.data()),"extensions");
    auto has=[&](const char* name) {return std::any_of(available.begin(),available.end(),[&](auto& e){return std::strcmp(e.extensionName,name)==0;});};
    std::vector<const char*> extensions{XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    for(auto name:extensions) if(!has(name)) throw std::runtime_error(std::string("required extension missing: ")+name);
    PerformanceSupport support;support.enable(available,extensions);
    XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR}; android.applicationVM=vm; android.applicationActivity=activity;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO}; ci.next=&android;
    std::strcpy(ci.applicationInfo.applicationName,"RDRVR Vulkan Harness");
    std::strcpy(ci.applicationInfo.engineName,"rdrvr"); ci.applicationInfo.applicationVersion=4; ci.applicationInfo.engineVersion=1;
    ci.applicationInfo.apiVersion=XR_API_VERSION_1_0; ci.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size()); ci.enabledExtensionNames=extensions.data();
    checkXr(xrCreateInstance(&ci,&instance_),"create OpenXR instance");
    XrInstanceProperties runtime{XR_TYPE_INSTANCE_PROPERTIES}; checkXr(xrGetInstanceProperties(instance_,&runtime),"runtime properties");
    __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","build=%s runtime=%s version=%llu",RDRVR_BUILD_ID,runtime.runtimeName,static_cast<unsigned long long>(runtime.runtimeVersion));
    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO}; si.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    checkXr(xrGetSystem(instance_,&si,&system_),"HMD system");
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES}; checkXr(xrGetSystemProperties(instance_,system_,&sp),"system properties");
    __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","system=%s orientation=%u position=%u",sp.systemName,sp.trackingProperties.orientationTracking,sp.trackingProperties.positionTracking);
    checkXr(xrEnumerateEnvironmentBlendModes(instance_,system_,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&n,nullptr),"blend count");
    std::vector<XrEnvironmentBlendMode> blends(n);
    checkXr(xrEnumerateEnvironmentBlendModes(instance_,system_,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,n,&n,blends.data()),"blend modes");
    if(std::find(blends.begin(),blends.end(),XR_ENVIRONMENT_BLEND_MODE_OPAQUE)==blends.end()) throw std::runtime_error("opaque stereo runtime required");
    gpu_.create(instance_,system_);
    auto binding=gpu_.binding();
    XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO}; sc.next=&binding; sc.systemId=system_;
    checkXr(xrCreateSession(instance_,&sc,&session_),"create Vulkan session");
    performance_.initialize(instance_,session_,support);
    configureThreads(performance_,instance_,session_,support.threads);
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rs.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL; rs.poseInReferenceSpace=identityPose();
    checkXr(xrCreateReferenceSpace(session_,&rs,&local_),"LOCAL space");
    checkXr(xrCreateReferenceSpace(session_,&rs,&base_),"base LOCAL space");
    createActions(); createSwapchains();
    if(has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
        refresh_=proc<PFN_xrGetDisplayRefreshRateFB>(instance_,"xrGetDisplayRefreshRateFB");
        auto enumerate=proc<PFN_xrEnumerateDisplayRefreshRatesFB>(instance_,"xrEnumerateDisplayRefreshRatesFB");
        checkXr(enumerate(session_,0,&n,nullptr),"refresh count"); std::vector<float> rates(n);
        checkXr(enumerate(session_,n,&n,rates.data()),"refresh rates");
        for(float rate:rates) __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","available_refresh=%.1f",rate);
        float rate{}; checkXr(refresh_(session_,&rate),"current refresh");
        __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","current_refresh=%.1f",rate);
    }
}
void XrHarness::createActions() {
    XrActionSetCreateInfo ac{XR_TYPE_ACTION_SET_CREATE_INFO}; std::strcpy(ac.actionSetName,"diagnostic"); std::strcpy(ac.localizedActionSetName,"Diagnostic");
    checkXr(xrCreateActionSet(instance_,&ac,&actions_),"action set");
    handPaths_={path("/user/hand/left"),path("/user/hand/right")};
    XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO}; ai.actionType=XR_ACTION_TYPE_POSE_INPUT; ai.countSubactionPaths=2; ai.subactionPaths=handPaths_.data();
    std::strcpy(ai.actionName,"aim"); std::strcpy(ai.localizedActionName,"Controller aim"); checkXr(xrCreateAction(actions_,&ai,&aim_),"aim action");
    ai.actionType=XR_ACTION_TYPE_BOOLEAN_INPUT; ai.countSubactionPaths=0; ai.subactionPaths=nullptr;
    std::strcpy(ai.actionName,"recenter_scene"); std::strcpy(ai.localizedActionName,"Recenter scene"); checkXr(xrCreateAction(actions_,&ai,&recenter_),"recenter action");
    std::vector<XrActionSuggestedBinding> bindings{{aim_,path("/user/hand/left/input/aim/pose")},
        {aim_,path("/user/hand/right/input/aim/pose")}};
    if(controllers_)controllerActions_.create(instance_,actions_,handPaths_,bindings);
    else bindings.push_back({recenter_,path("/user/hand/left/input/x/click")});
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile=path("/interaction_profiles/oculus/touch_controller"); suggested.countSuggestedBindings=static_cast<std::uint32_t>(bindings.size()); suggested.suggestedBindings=bindings.data();
    { // SPIN: a runtime that refuses the trigger/touch bindings refuses the whole Touch suggestion -> retry without them (VrTouch.h)
        const XrResult touchResult=xrSuggestInteractionProfileBindings(instance_,&suggested);
        if(XR_FAILED(touchResult)&&::rdrvr::vr::touch::bindingCount.load(std::memory_order_relaxed)>0){
            ::rdrvr::vr::touch::dropTouchBindings(bindings);::rdrvr::vr::touch::bound.store(false,std::memory_order_relaxed);
            ::rdrvr::vr::touch::retried.store(true,std::memory_order_relaxed);
            suggested.countSuggestedBindings=static_cast<std::uint32_t>(bindings.size());suggested.suggestedBindings=bindings.data();
            checkXr(xrSuggestInteractionProfileBindings(instance_,&suggested),"Touch bindings");
        }else{checkXr(touchResult,"Touch bindings");::rdrvr::vr::touch::bound.store(::rdrvr::vr::touch::bindingCount.load(std::memory_order_relaxed)>0,std::memory_order_relaxed);}
    }
    suggested.interactionProfile=path("/interaction_profiles/khr/simple_controller"); suggested.countSuggestedBindings=2;
    checkXr(xrSuggestInteractionProfileBindings(instance_,&suggested),"simple controller aim bindings");
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; attach.countActionSets=1; attach.actionSets=&actions_;
    checkXr(xrAttachSessionActionSets(session_,&attach),"attach actions");
    if(controllers_)controllerActions_.createSpaces(session_);
    for(unsigned i=0;i<2;++i) {
        XrActionSpaceCreateInfo space{XR_TYPE_ACTION_SPACE_CREATE_INFO}; space.action=aim_; space.subactionPath=handPaths_[i]; space.poseInActionSpace=identityPose();
        checkXr(xrCreateActionSpace(session_,&space,&handSpaces_[i]),"aim space");
    }
}
void XrHarness::initializeBorrowed(XrInstance instance,XrSystemId system,const VulkanBootstrap& gpu,GameTheater* theater,TrackedViewsMailbox* gameViews,StereoMailbox* stereo,ControllerMailbox* controllers,PresentationMailbox* presentation,GameTheater* hud) {
    if(instance_ || session_)throw std::runtime_error("session already initialized");
    instance_=instance;system_=system;borrowed_=true;theater_=theater;hud_=hud;gameViews_=gameViews;stereo_=stereo;controllers_=controllers;presentation_=presentation;gpu_.borrow(gpu);
#ifdef RDRVR_MANUAL_THEATER
    controllerActions_.enableTheaterSwitch(presentation_!=nullptr);
    controllerActions_.setGripsBusy(&::rdrvr::vr::holsters::gripsEngaged); // No theater grip chord while holsters own a grip
#endif
    auto binding=gpu_.binding();
    XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO};create.next=&binding;create.systemId=system_;
    checkXr(xrCreateSession(instance_,&create,&session_),"create game-device XR session");
    dynresQuery.resolve(instance_); // dynres01: game instance (GameXr G5); nothing when the extension is not enabled
    performance_.initialize(instance_,session_,performanceSupport_);
    configureThreads(performance_,instance_,session_,performanceSupport_.threads);
    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;space.poseInReferenceSpace=identityPose();
    checkXr(xrCreateReferenceSpace(session_,&space,&local_),"game LOCAL space");
    checkXr(xrCreateReferenceSpace(session_,&space,&base_),"game base space");
    if(hud_){
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        if(XR_SUCCEEDED(xrGetSystemProperties(instance_,system_,&properties)) && properties.graphicsProperties.maxLayerCount>=2){
            space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;
            if(XR_FAILED(xrCreateReferenceSpace(session_,&space,&hudSpace_)))hudSpace_=XR_NULL_HANDLE;
        }
        if(!hudSpace_)__android_log_print(ANDROID_LOG_WARN,"RdrVrXR","HUD layer unavailable; world/theater remain active");
    }
    createActions();createSwapchains();
    XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
    if(hudSpace_&&XR_SUCCEEDED(xrGetSystemProperties(instance_,system_,&properties))&&properties.graphicsProperties.maxLayerCount>=3){
        try{perfOverlay_.initialize(session_,binding);perfPixels_=std::make_unique<diagnostics::PerfPanel>();perfOverlayEnabled_=true;}
        catch(const std::exception& e){perfOverlay_.destroy();__android_log_print(ANDROID_LOG_WARN,"RdrVrXR","stats unavailable: %s",e.what());}
    }
}
void XrHarness::invalidateTracking(){
    graphicsMenu_.invalidate();
    ++inputEpoch_;
    if(haptics_)haptics_->invalidate();
    tracking_.invalidate();recenterButton_.invalidate();controllerActions_.invalidate();if(controllers_)controllers_->invalidate();
    recenterRequested_=false;if(gameViews_)gameViews_->invalidate();if(stereo_)stereo_->invalidate();if(hud_)hud_->invalidateCompleted();
}
void XrHarness::requestExit() {
    controllerActions_.outputHaptics(nullptr);if(haptics_)haptics_->invalidate();
    if(session_ && running_)checkXr(xrRequestExitSession(session_),"request session exit");
}
void XrHarness::createSwapchains() {
    std::uint32_t count{}; checkXr(xrEnumerateViewConfigurationViews(instance_,system_,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,0,&count,nullptr),"eye count");
    if(count!=2) throw std::runtime_error("exactly two stereo views required");
    std::array<XrViewConfigurationView,2> config{{{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
    checkXr(xrEnumerateViewConfigurationViews(instance_,system_,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&count,config.data()),"eye configuration");
    resolutionTelemetry.recommendedWidth.store(config[0].recommendedImageRectWidth,std::memory_order_relaxed);
    resolutionTelemetry.recommendedHeight.store(config[0].recommendedImageRectHeight,std::memory_order_relaxed);
    if(borrowed_){
        // Game world/theater own their swapchains. As in MiamiVR's release
        // renderer, never allocate the unused standalone geometry/depth path.
        for(unsigned i=0;i<2;++i)diagnostics::stereoHistory.push(i,"XR_RECOMMENDED",0,config[i].recommendedImageRectWidth,config[i].recommendedImageRectHeight);
        return;
    }
    checkXr(xrEnumerateSwapchainFormats(session_,0,&count,nullptr),"format count"); std::vector<std::int64_t> formats(count);
    checkXr(xrEnumerateSwapchainFormats(session_,count,&count,formats.data()),"formats");
    VkFormat selected=VK_FORMAT_UNDEFINED;
    for(auto format:{VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_B8G8R8A8_UNORM}) {
        VkFormatProperties p{}; vkGetPhysicalDeviceFormatProperties(gpu_.physical,format,&p);
        if(std::find(formats.begin(),formats.end(),format)!=formats.end() && (p.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) {selected=format;break;}
    }
    if(selected==VK_FORMAT_UNDEFINED) throw std::runtime_error("no compatible RGBA swapchain format");
    renderer_.initialize(gpu_,selected);
    for(unsigned i=0;i<2;++i) {
        auto& eye=eyes_[i]; eye.extent={config[i].recommendedImageRectWidth,config[i].recommendedImageRectHeight};
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO}; sc.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sc.format=selected; sc.sampleCount=1; sc.width=eye.extent.width; sc.height=eye.extent.height; sc.faceCount=1; sc.arraySize=1; sc.mipCount=1;
        checkXr(xrCreateSwapchain(session_,&sc,&eye.swapchain),"eye swapchain");
        checkXr(xrEnumerateSwapchainImages(eye.swapchain,0,&count,nullptr),"image count");
        eye.images.resize(count,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        checkXr(xrEnumerateSwapchainImages(eye.swapchain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data())),"eye images");
        eye.targets.resize(count);
        for(unsigned j=0;j<count;++j) renderer_.createTarget(eye.images[j].image,selected,eye.extent,eye.targets[j]);
        __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","eye=%u extent=%ux%u images=%u format=%d",i,eye.extent.width,eye.extent.height,count,selected);
    }
}
void XrHarness::events() {
    while(true) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        auto result=xrPollEvent(instance_,&event);
        if(result==XR_EVENT_UNAVAILABLE) return;
        checkXr(result,"poll event");
        if(event.type==XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT)performance_.event(*reinterpret_cast<const XrEventDataPerfSettingsEXT*>(&event));
        if(event.type==XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB)performance_.refreshChanged(*reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event));
        if(event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {::rdrvr::vr::inputrec::sessionEnd();invalidateTracking();sessionLost_=true; exit_=true; running_=false; return;}
        if(event.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const auto& changed=*reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
            // While an input replay plays, a LIVE recenter (system or Guardian) is ignored: the
            // replayed poses live in the recorded LOCAL space, the recorded recenters are re-applied by the replay (frame())
            if(changed.session==session_ && changed.referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL && ::rdrvr::vr::inputrec::ignoreLive()) {}
            else if(changed.session==session_ && changed.referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL) {
                pendingSpaceChange_=std::max(pendingSpaceChange_,changed.changeTime);
                invalidateTracking();
            }
        }
        if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& changed=*reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            if(changed.session!=session_) continue;
            state_=changed.state;
            const bool focus=state_==XR_SESSION_STATE_FOCUSED;
            if(focus!=focused_ && !::rdrvr::vr::inputrec::ignoreLive()) invalidateTracking(); // While replaying, the recorded focus rules
            focused_=focus;
            gpuBoost.setSession(state_==XR_SESSION_STATE_VISIBLE||state_==XR_SESSION_STATE_FOCUSED); // boost01: 0 while not shown
            __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","session_state=%d epoch=%llu",state_,static_cast<unsigned long long>(tracking_.epoch));
            if(state_==XR_SESSION_STATE_READY && !running_) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                checkXr(xrBeginSession(session_,&begin),"begin session"); running_=true;performance_.begin();
            } else if(state_==XR_SESSION_STATE_STOPPING && running_) {
                ::rdrvr::vr::inputrec::sessionEnd(); // A recording ends with the session
                invalidateTracking();checkXr(xrEndSession(session_),"end session"); running_=false;
            } else if(state_==XR_SESSION_STATE_EXITING || state_==XR_SESSION_STATE_LOSS_PENDING) {
                ::rdrvr::vr::inputrec::sessionEnd();
                invalidateTracking();sessionLost_=state_==XR_SESSION_STATE_LOSS_PENDING; exit_=true; running_=false;
            }
        }
    }
}
std::array<Pose,2> XrHarness::locateHands(XrTime time) {
    std::array<Pose,2> hands{};
    XrActiveActionSet active{actions_,XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO}; sync.countActiveActionSets=1; sync.activeActionSets=&active;
    auto synced=xrSyncActions(session_,&sync);
    namespace ir=::rdrvr::vr::inputrec; // The gate / aim / pad reads below go through the recorder (a playing replay replaces them)
    if(!ir::handsGate(!(synced==XR_SESSION_NOT_FOCUSED || !inputFocus))) {graphicsMenu_.invalidate();recenterButton_.invalidate();controllerActions_.invalidate();if(controllers_)controllers_->invalidate();return hands;}
    checkXr(synced,"sync actions");
    std::uint32_t aimFlags[2]{};float aimPose[2][7]{{0,0,0,1},{0,0,0,1}}; // Raw aim (ir::aimActive | XrSpaceLocationFlags, pose)
    for(unsigned i=0;i<2;++i) {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO}; get.action=aim_; get.subactionPath=handPaths_[i];
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE}; checkXr(xrGetActionStatePose(session_,&get,&state),"aim active");
        if(!state.isActive) continue;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        checkXr(xrLocateSpace(handSpaces_[i],local_,time,&location),"locate controller");
        aimFlags[i]=ir::aimActive|std::uint32_t(location.locationFlags);poseFloats(location.pose,aimPose[i]);
    }
    ir::aim(aimFlags,aimPose);
    for(unsigned i=0;i<2;++i)if(aimFlags[i]&ir::aimActive)hands[i]=poseOf(floatsPose(aimPose[i]),validFlags(aimFlags[i]&~ir::aimActive));
    if(controllers_){
        ControllerSample sample{};sample.sequence=frame_;sample.epoch=tracking_.epoch;sample.inputEpoch=inputEpoch_;sample.referenceSpace=spaceEpoch_;sample.focused=inputFocus&&tracking_.ready();sample.aim=hands;
        sample.publishedNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        bool recenter{},theaterToggle{};
        bool inputReady=controllerActions_.read(session_,local_,time,sample,recenter,theaterToggle);
#ifdef RDRVR_BENCH_CONTROL
        const bool nativeMenu=presentation_&&!presentation_->manualTheater()&&presentation_->snapshot(sample.publishedNs).context==PresentationContext::Menu;
        inputReady=benchMenuInput.apply(sample,nativeMenu,sample.publishedNs)||inputReady;
        { // Bench: debug.rdrvr.vr.padinject "<nonce>:<hex touch buttons>:<left x>:<left y>:<ms>" holds those buttons / that left
          // stick for ms (a new nonce restarts; buttons 0 = stick only). Device tests without a wearer (riding memory check).
            static unsigned polls{};static char lastNonce[24]{};static std::uint64_t until{};static std::uint32_t injButtons{};static float injX{},injY{},injRx{};
            if((polls++&7u)==0){char v[PROP_VALUE_MAX]{};
                if(__system_property_get("debug.rdrvr.vr.padinject",v)>0){char nonce[24]{};unsigned b{};float x{},y{};unsigned ms{};
                    float rx{};const int got=std::sscanf(v,"%23[^:]:%x:%f:%f:%u:%f",nonce,&b,&x,&y,&ms,&rx); // Optional 6th = right stick x
                    if(got>=5&&std::strcmp(nonce,lastNonce)!=0){
                        std::snprintf(lastNonce,sizeof lastNonce,"%s",nonce);injButtons=b&0x7fu;injX=std::clamp(x,-1.f,1.f);injY=std::clamp(y,-1.f,1.f);injRx=got>=6?std::clamp(rx,-1.f,1.f):0.f;
                        until=sample.publishedNs+std::uint64_t(std::min(ms,600000u))*1'000'000ull;}}}
            if(until&&sample.publishedNs<until){sample.active=true;sample.focused=true;sample.buttons|=injButtons;sample.sticks={injX,injY,injRx,0.f};sample.triggers={};sample.grips={};inputReady=true;} // Every axis defined (an idle controller may leave garbage -> invalid sample)
            else until=0;
        }
#endif
        bool graphicsInput=perfOverlayEnabled_&&graphicsMenu_.update(sample);
        if(!graphicsInput&&perfOverlayEnabled_&&::rdrvr::vr::welcome::consume(sample,sample.publishedNs))graphicsInput=true; // The welcome panel eats input
        if(graphicsInput){sample.buttons=0;sample.sticks={};sample.triggers={};sample.grips={};recenter=theaterToggle=false;}
        if(inputReady)::rdrvr::vr::touch::note(graphicsInput?0u: ::rdrvr::vr::touch::pendingFor(sample.publishedNs),sample.publishedNs,graphicsInput,
            sample.triggers.data(),sample.grips.data()); // SPIN: after the menu mask
        { // VR page RECENTER row (A) = the stick-click recenter chord; XR thread only
            static std::uint32_t vrRecenterSeen=::rdrvr::vr::recenterRequests.load(std::memory_order_acquire);
            if(::rdrvr::vr::takeRecenter(vrRecenterSeen))recenter=true;
        }
        if(inputReady)::rdrvr::vr::hands::pushPose(sample); // HANDS: XR-frame keyed pose ring (render thread picks plan.tracking.frameId)
        if(inputReady)::rdrvr::vr::melee::pushSample(sample); // MELEE: XR-rate pose tape for the main-thread swing measurement
        // THROW: XR-rate grip tape for the release velocity (VrThrow.h), plus the time the poses
        // were located for (pdtMono of this XR frame; 0 = unknown) and whether the VR menu ate the
        // input (masked); the reload gesture detectors difference on poseNs and use the runtime
        // velocities of the same sample.
        if(inputReady){const std::int64_t pdt=xrPoseMonoNs.load(std::memory_order_relaxed);
            ::rdrvr::vr::throwing::pushSample(sample,pdt>0?std::uint64_t(pdt):0u,graphicsInput);}
        // HAMMER: the swipe detector at XR rate (its only owner; the stroke haptic starts in this
        // XR frame); disarmed by the menu (graphicsInput) / the 300 ms menu freeze itself. No hammer module = no sink.
        if(inputReady){if(const auto* hs=::rdrvr::vr::holsters::hammerSinks.load(std::memory_order_acquire);hs&&hs->xrSample)hs->xrSample(sample,graphicsInput);}
        if(inputReady)controllers_->publish(sample);else controllers_->invalidate();
        ControllerSample current{};HapticSample feedback{};
        const bool hapticReady=!graphicsInput&&haptics_&&controllers_->snapshot(sample.publishedNs,current)&&haptics_->snapshot(sample.publishedNs,current,feedback);
        ::rdrvr::vr::holsters::noteMenu(graphicsInput,sample.publishedNs);                  // Freeze holsters while the menu eats input
        const auto vrPulse=::rdrvr::vr::haptics::xrFrame(sample.publishedNs,graphicsInput); // Holster pulses (dropped while the menu is open)
        if(hapticReady)for(unsigned h=0;h<2;++h)if(vrPulse.active[h])feedback.amplitude[h]=0; // a 30 ms native pulse would replace the VR pulse
        controllerActions_.outputHaptics(hapticReady?&feedback:nullptr);
        for(unsigned h=0;h<2;++h)if(vrPulse.start[h])controllerActions_.pulse(h,vrPulse.amplitude[h],vrPulse.durationNs[h]);
        if(recenter)recenterRequested_=true;
        if(theaterToggle&&presentation_)presentation_->toggleManualTheater();
        return hands;
    }
    XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO}; get.action=recenter_;
    XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN}; checkXr(xrGetActionStateBoolean(session_,&get,&button),"recenter button");
    {constexpr std::uint32_t rb=1u<<ir::bit::recenter;ir::PadRaw raw{};raw.ok=rb; // The non-controller recenter action through the recorder
     raw.active=button.isActive?rb:0u;raw.state=button.isActive&&button.currentState?rb:0u;raw.changed=button.changedSinceLastSync?rb:0u;
     ir::pad(raw,false);button.isActive=(raw.active&raw.ok&rb)?XR_TRUE:XR_FALSE;button.currentState=(raw.state&rb)?XR_TRUE:XR_FALSE;}
    if(recenterButton_.sample(button.isActive,button.currentState,inputFocus))
        recenterRequested_=true;
    return hands;
}
void XrHarness::resetSpace(XrTime time,bool toHead) {
    XrReferenceSpaceCreateInfo create{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    create.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL; create.poseInReferenceSpace=identityPose();
    if(toHead) {
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO}; locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime=time; locate.space=base_;
        XrViewState state{XR_TYPE_VIEW_STATE}; std::array<XrView,2> views{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}}; std::uint32_t count{};
        checkXr(xrLocateViews(session_,&locate,&state,2,&count,views.data()),"recenter locate");
        const auto required=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT|
                            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT|XR_VIEW_STATE_POSITION_TRACKED_BIT;
        auto head=poseOf(views[0].pose,count==2 && (state.viewStateFlags&required)==required);
        auto right=poseOf(views[1].pose,count==2 && (state.viewStateFlags&required)==required);
        // A recenter regenerated by a replay still re-epochs on a headset without (live) tracking or lying lenses up / down
        // (identity LOCAL; the replayed poses are in the recorded LOCAL space anyway)
        const auto forward=validPose(head)&&validPose(right)?rotate(head.orientation,{0,0,-1}):Vec3{};
        if(forward[0]*forward[0]+forward[2]*forward[2]<0.001f) {if(!::rdrvr::vr::inputrec::replaying())return;}
        else{
        float yaw=std::atan2(-forward[0],-forward[2]);
        create.poseInReferenceSpace.orientation={0,std::sin(yaw/2),0,std::cos(yaw/2)};
        create.poseInReferenceSpace.position={(head.position[0]+right.position[0])/2,
            (head.position[1]+right.position[1])/2,(head.position[2]+right.position[2])/2};
        }
    }
    XrSpace replacement{};
    checkXr(xrCreateReferenceSpace(session_,&create,&replacement),"recenter space");
    xrDestroySpace(local_); local_=replacement;
    ++spaceEpoch_; anchored_=false;invalidateTracking();
    __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","space_recreated epoch=%llu user=%d",static_cast<unsigned long long>(spaceEpoch_),toHead);
}
void XrHarness::frame() {
    if(!running_) return;
    performance_.pump(presentation_&&(presentationGate_.world()||stereoCutscene(presentation_)));
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO}; XrFrameState frameState{XR_TYPE_FRAME_STATE};
    const auto waitBegin=diagnostics::monotonicNs(); // Always-on XR frame timing (xrHistory)
    {diagnostics::WallScope timing(diagnostics::frameTimings.xrWait);
     checkXr(xrWaitFrame(session_,&wait,&frameState),"wait frame");}
    const auto wake=diagnostics::monotonicNs();
    // predictedDisplayTime in CLOCK_MONOTONIC (-1 without XR_KHR_convert_timespec_time, 0 on failure).
    const std::int64_t pdtMono=timeConvert.monotonic(instance_,frameState.predictedDisplayTime);
    xrPoseMonoNs.store(pdtMono,std::memory_order_relaxed); // The tape's poseNs (locateHands below uses this frame's predictedDisplayTime)
    // XR frame grid for the render-thread frame lock; menu/property state of FRAME RATE and SPACEWARP.
    framePacer.observeXrFrame(wake,frameState.predictedDisplayPeriod,performance_.displayHz(),pdtMono);
    const auto requested=graphicsSettings.snapshot().requested; // Also SHARPEN below
    {// FRAME RATE ADAPTIVE gates the SPACEWARP menu value: AppSW only while the governor is at 36 with
     // headroom (FramePacer::governorAllowsSpaceWarp); held -> configure(0) stops production and chaining at once.
     // A debug.rdrvr.spacewarp property still overrides the menu (SpaceWarpControl) and is not gated.
     const bool adaptive=framePacer.requestedMode(requested.frameRate)==FramePacer::adaptiveMode;
     const bool held=adaptive&&requested.spaceWarp&&!framePacer.governorAllowsSpaceWarp();
     spaceWarpControl.configure(held?0u:requested.spaceWarp);
     // LOCK 36 is forced only while space-warp info is really being chained (recent noteSubmit with info)
     // or while the AppSW budget guard suspends it (the runtime returns to 72 Hz, LOCK 36 continues).
     // In ADAPTIVE the force does not override the governor's level (FramePacer::configure).
     const bool forced=spaceWarpControl.lockForced();
     framePacer.configure(requested.frameRate,forced);spaceWarpMenuSupport.store(spaceWarpControl.available()?1:0,std::memory_order_relaxed);
     spaceWarpMenuForced.store(forced?1:0,std::memory_order_relaxed);
     spaceWarpMenuIssue.store(held&&spaceWarpControl.available()?"HELD BY ADAPTIVE":spaceWarpControl.issue(),std::memory_order_relaxed);
     if(held)framePacer.noteAppSwBlocked();
     // The AppSW budget guard tripped (rising edge) -> the governor revokes AppSW and backs off.
     const bool over=adaptive&&spaceWarpControl.active()&&spaceWarpControl.overBudget(wake);
     if(over&&!presentState.appSwOverBudget)framePacer.noteAppSwOverBudget();
     presentState.appSwOverBudget=over;
     frameRateGovernorHz.store(int(framePacer.governorLevelHz()),std::memory_order_relaxed);}
    diagnostics::WallScope workTiming(diagnostics::frameTimings.xrWork);
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO}; checkXr(xrBeginFrame(session_,&begin),"begin frame");
    ++frame_;
    // INPUT RECORD / REPLAY (VrInputRec.h): 1 Hz properties; while a replay plays, this frame's recorded sample (display-time axis)
    // and whether it passed a recorded runtime recenter. inputFocus = the focus the input gates use (recorded while replaying).
    namespace ir=::rdrvr::vr::inputrec;
    const bool replaySpaceChange=ir::frameBegin(wake,pdtMono,frameState.predictedDisplayPeriod,frameState.shouldRender==XR_TRUE);
    inputFocus=ir::focus(focused_);
    {static bool replayFocus=false,replayFocusSeen=false; // A recorded focus change invalidates like the live session event (events())
     if(ir::replaying()){if(replayFocusSeen&&inputFocus!=replayFocus)invalidateTracking();replayFocus=inputFocus;replayFocusSeen=true;}else replayFocusSeen=false;}
    if(presentation_){
        const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(presentationGate_.update(presentation_->snapshot(now))){
            // Retire the prior mode's pixels/poses before returning to stereo.
            // The existing GPU mailbox keeps unfinished submissions alive.
            if(presentationGate_.preservesInput()){
                // Scope entry/exit must not release the trigger that holds it
                // open. A new visual epoch still rejects pre-transition work.
                tracking_.retireContent();
                if(gameViews_)gameViews_->invalidate();if(stereo_)stereo_->invalidate();if(hud_)hud_->invalidateCompleted();
            }else invalidateTracking();
            anchored_=false;
        }
    }
    if(pendingSpaceChange_ && frameState.predictedDisplayTime>=pendingSpaceChange_) {
        resetSpace(frameState.predictedDisplayTime,false); pendingSpaceChange_=0; recenterRequested_=false;ir::note(ir::flag::spaceChange);
    }
    if(recenterRequested_ && !pendingSpaceChange_) { resetSpace(frameState.predictedDisplayTime,true); recenterRequested_=false;ir::note(ir::flag::userRecenter); }
    // A recorded runtime recenter (reference-space change) is re-applied where the replay passes it (manual recenters regenerate
    // from the replayed stick chord / VR page RECENTER above)
    if(replaySpaceChange && !pendingSpaceChange_) { resetSpace(frameState.predictedDisplayTime,false); recenterRequested_=false;ir::note(ir::flag::spaceChange); }
    std::array<XrCompositionLayerProjectionView,2> layers{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
    std::array<XrCompositionLayerSpaceWarpInfoFB,2> warpInfo{{{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB},{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB}}};
    std::uint64_t presentedPair=0; // renderFrameId of the stereo pair in this frame
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION}; layer.space=local_; layer.viewCount=2; layer.views=layers.data();
    XrCompositionLayerSettingsFB layerSettings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB}; // SHARPEN (projection layer only)
    bool sharpened=false,trackingReady=false,mailboxAsked=false;const bool spacePending=pendingSpaceChange_!=0; // No-layer reasons
    const XrCompositionLayerBaseHeader* submitted=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    XrCompositionLayerQuad cinema{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};bool hudRendered=false;
    ::rdrvr::vr::hud::Layers vrHud{}; // Only next to a world stereo projection layer (never theater / menus / loading)
    bool rendered=false;bool diagnostic=false;bool gameFrame=false;bool gameStereo=false;
    // On fatal render failure, teardown waits the private queue and destroys the session;
    // it does not publish a partial pair or release an unfinished image.
    if(frameState.shouldRender && !pendingSpaceChange_) {
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO}; locate.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime=frameState.predictedDisplayTime; locate.space=local_;
        XrViewState viewsState{XR_TYPE_VIEW_STATE}; std::uint32_t count{};
        std::array<XrView,2> views{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
        checkXr(xrLocateViews(session_,&locate,&viewsState,2,&count,views.data()),"locate stereo views");
        if(ir::xr.on)viewsHook(viewsState,count,views); // Raw views -> recorder; a playing replay replaces flags / count / poses / fovs
        viewFlags_=viewsState.viewStateFlags;
        // Theater rule (MiamiVR): valid orientation is sufficient for display.
        // Tracked bits gate physical input, not whether a panel can be shown.
        const bool orientation=count==2 && (viewFlags_&XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        const bool position=count==2 && (viewFlags_&XR_VIEW_STATE_POSITION_VALID_BIT);
        if(orientation)for(unsigned i=0;i<2;++i) {
            if(position)lastPositions_[i]=views[i].pose.position;
            else views[i].pose.position=lastPositions_[i];
        }
        std::array<Pose,2> poses{poseOf(views[0].pose,orientation),poseOf(views[1].pose,orientation)};
        if(orientation&&!ir::replaying())standmotion::apply(poses,diagnostics::monotonicNs());
        if(orientation && validPose(poses[0]) && validPose(poses[1])) {
            constexpr auto fullTracking=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT|
                XR_VIEW_STATE_ORIENTATION_TRACKED_BIT|XR_VIEW_STATE_POSITION_TRACKED_BIT;
            const bool ready=tracking_.sample(inputFocus && (viewFlags_&fullTracking)==fullTracking);trackingReady=ready;
            auto hands=locateHands(frameState.predictedDisplayTime);
            if(gameViews_) {
                if(ready) {
                    TrackedViews sample{};sample.frameId=frame_;sample.displayTime=frameState.predictedDisplayTime;
                    sample.publishedNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                    sample.epochs={tracking_.epoch,spaceEpoch_,1};sample.eyes=poses;
                    // VIEW AHEAD (debug.rdrvr.xr.viewahead = N predicted display periods, 0..3; read 1 Hz): the game renders
                    // this sample's pair for the XR frame that SHOWS it (lockstep: one XR frame later), not for this frame's display time.
                    // Without it the content is predicted for an earlier display time than the frame it is submitted in,
                    // and head turns judder while AppSW synthesizes.
                    {static int ahead=0;static std::uint64_t nextRead=0;const auto t=diagnostics::monotonicNs();
                     // Applied only while AppSW synthesizes (default: the latch depth, below)
                     if(t>=nextRead){nextRead=t+1'000'000'000ull;char v[PROP_VALUE_MAX]{};
                         // default = the latch depth (debug.rdrvr.latch.depth, default 1): render for the frame that shows the pair
                         int depth=1;if(__system_property_get("debug.rdrvr.latch.depth",v)>0)depth=std::clamp(std::atoi(v),1,3);
                         ahead=__system_property_get("debug.rdrvr.xr.viewahead",v)>0?std::clamp(std::atoi(v),0,3):depth;}
                     if(ahead>0&&spaceWarpControl.synthesizing()&&!ir::replaying()&&frameState.predictedDisplayPeriod>0){
                         XrViewLocateInfo ali{XR_TYPE_VIEW_LOCATE_INFO};ali.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                         ali.displayTime=frameState.predictedDisplayTime+XrTime(ahead)*frameState.predictedDisplayPeriod;ali.space=local_;
                         XrViewState avs{XR_TYPE_VIEW_STATE};std::uint32_t ac{};std::array<XrView,2> av{{{XR_TYPE_VIEW},{XR_TYPE_VIEW}}};
                         if(XR_SUCCEEDED(xrLocateViews(session_,&ali,&avs,2,&ac,av.data()))&&ac==2&&(avs.viewStateFlags&XR_VIEW_STATE_ORIENTATION_VALID_BIT)){
                             const bool apos=(avs.viewStateFlags&XR_VIEW_STATE_POSITION_VALID_BIT)!=0;
                             for(unsigned i=0;i<2;++i){if(!apos)av[i].pose.position=views[i].pose.position;sample.eyes[i]=poseOf(av[i].pose,true);}
                             if(validPose(sample.eyes[0])&&validPose(sample.eyes[1]))sample.displayTime=ali.displayTime;else sample.eyes=poses;
                         }}}
                    const auto margin=graphicsSettings.snapshot().requested.edgeMarginPercent;
                    for(unsigned i=0;i<2;++i){const auto f=views[i].fov;sample.fov[i]=graphicsFov({f.angleLeft,f.angleRight,f.angleUp,f.angleDown},margin);}
                    gameViews_->publish(sample);
                } else gameViews_->invalidate();
            }
            framePacer.notifyXrViews(); // LOCKSTEP: the render thread starts its next frame on these views (FramePacer::pace)
            XrSwapchain screen{};VkExtent2D extent{};
            StereoLayer stereoLayer{};
            const auto nowNs=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            bool stereoReady=false;
            if(stereo_)stereo_->setXrFrame(frame_); // This XR frame's id (views just published with it)
            if(ready && stereo_ && (!presentation_||presentationGate_.world()||stereoCutscene(presentation_)))try{mailboxAsked=true;stereoReady=stereo_->render(session_,tracking_.epoch,spaceEpoch_,nowNs,stereoLayer);}
                catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","stereo transfer disabled: %s",e.what());}
            if(stereoReady) {
                for(unsigned i=0;i<2;++i) {
                    const auto& p=stereoLayer.poses[i];const auto f=stereoLayer.fov[i];
                    // Fresh predicted views cannot relabel pixels from an older game frame.
                    layers[i].pose={{p.orientation[0],p.orientation[1],p.orientation[2],p.orientation[3]},
                                    {p.position[0],p.position[1],p.position[2]}};
                    layers[i].fov={f.left,f.right,f.up,f.down};
                    layers[i].subImage.swapchain=stereoLayer.swapchains[i];
                    layers[i].subImage.imageRect={{0,0},{static_cast<std::int32_t>(stereoLayer.extent[i].width),static_cast<std::int32_t>(stereoLayer.extent[i].height)}};
                }
                // AppSW: chain motion vectors + depth of this pair onto both projection views.
                // FRAME_SKIP_BIT ("skip current frame's space warp extrapolation", registry) on cuts / no
                // history, and by default on a pair re-submitted without re-rendering (debug.rdrvr.spacewarp.skip).
                presentedPair=stereoLayer.frame.renderFrameId;
                if(spaceWarpControl.active())spaceWarpControl.noteFrame(presentedPair); // Per-mode cadence
                if(stereoLayer.warp.valid&&spaceWarpControl.active()){
                    const auto& w=stereoLayer.warp;const bool resubmit=w.frame==spaceWarpControl.lastSubmittedFrame;
                    // AppSW budget guard: > 7 re-submits in the last 72 half-rate XR frames suspend the info
                    // (back-off 5 s doubling to 40 s); while suspended nothing is chained and the runtime returns to 72 Hz.
                    const auto warpNow=diagnostics::monotonicNs();const bool chain=!spaceWarpControl.overBudget(warpNow);
                    const int policy=spaceWarpControl.skipPolicy();bool info=false,skip=false;
                    // Policy 3: the n-th consecutive re-submit of a pair shows it from where the app space has moved since
                    // (constant locomotion: inv(delta)^n * renderedPose, delta = inv(prev)*cur of this pair; identity without history).
                    if(resubmit)++resubmitRun_;else resubmitRun_=0;
                    const auto halfPeriod=framePacer.periodNs();
                    const bool halfRateNow=halfPeriod>0&&frameState.predictedDisplayPeriod>halfPeriod+halfPeriod/2;
                    if(resubmit&&policy==3&&!w.skip&&chain&&halfRateNow&&spaceWarpControl.deltaMode()==1){ // One pair per XR frame
                        const XrPosef step=poseInverse(w.delta);XrPosef move=identityPose();
                        for(unsigned n=0;n<std::min(resubmitRun_,4u);++n)move=poseMul(step,move);
                        for(unsigned i=0;i<2;++i)layers[i].pose=poseMul(move,layers[i].pose);
                        ++resubmitCompensated_;spaceWarpControl.compensatedResubmits.fetch_add(1,std::memory_order_relaxed);
                    }
                    if(chain&&!(resubmit&&policy==2)){
                        skip=w.skip||(resubmit&&(policy==1||policy==3));info=true; // Policy 3 keeps the skip bit
                        const XrRect2Di rect{{0,0},{static_cast<std::int32_t>(w.extent.width),static_cast<std::int32_t>(w.extent.height)}};
                        for(unsigned i=0;i<2;++i){auto& x=warpInfo[i];
                            x.layerFlags=skip?XR_COMPOSITION_LAYER_SPACE_WARP_INFO_FRAME_SKIP_BIT_FB:0;
                            x.motionVectorSubImage.swapchain=w.motion[i];x.motionVectorSubImage.imageRect=rect;x.motionVectorSubImage.imageArrayIndex=0;
                            x.depthSubImage.swapchain=w.depth[i];x.depthSubImage.imageRect=rect;x.depthSubImage.imageArrayIndex=0;
                            x.appSpaceDeltaPose=resubmit?identityPose():w.delta;
                            x.minDepth=w.minDepth;x.maxDepth=w.maxDepth;x.nearZ=w.nearZ;x.farZ=w.farZ;
                            layers[i].next=&x;}
                    }
                    spaceWarpControl.lastSubmittedFrame=w.frame; // also while suspended: fresh on resume
                    if(chain){const auto period=framePacer.periodNs(); // Count only inside the runtime's half-rate loop
                        spaceWarpControl.noteCadence(resubmit,period>0&&frameState.predictedDisplayPeriod>period+period/2,warpNow);}
                    spaceWarpControl.noteSubmit(info,skip,resubmit,info&&!resubmit?w.delta:identityPose(),frameState.predictedDisplayPeriod);
                }
                rendered=true;gameStereo=true;
                // SHARPEN: XrCompositionLayerSettingsFB on the projection layer (never on the views, which carry
                // the space-warp info; never a super-sampling bit). AUTO without XR_META_automatic_layer_filter = NORMAL.
                if(requested.sharpen>0&&requested.sharpen<GraphicsSettings::sharpenModes){
                    if(xrLayerSettingsEnabled.load(std::memory_order_relaxed)==1){
                        const bool autoFilter=xrAutoLayerFilterEnabled.load(std::memory_order_relaxed)==1;
                        layerSettings.layerFlags=GraphicsSettings::sharpenLayerFlags(requested.sharpen,autoFilter);layer.next=&layerSettings;sharpened=true;
                        presentTelemetry.settingsChained.fetch_add(1,std::memory_order_relaxed);presentTelemetry.settingsFlags.store(layerSettings.layerFlags,std::memory_order_relaxed);
                        sharpenEffective.store(requested.sharpen==3&&!autoFilter?1:int(requested.sharpen),std::memory_order_relaxed);
                    }else{presentTelemetry.settingsMissingExtension.fetch_add(1,std::memory_order_relaxed);sharpenEffective.store(0,std::memory_order_relaxed);}
                }else sharpenEffective.store(0,std::memory_order_relaxed);
                presentTelemetry.settingsLastChained.store(sharpened,std::memory_order_relaxed);
                if(hud_ && hudSpace_)try{
                    XrSwapchain hudScreen{};VkExtent2D hudExtent{};
                    if(hud_->renderCompleted(session_,gpu_.queue,stereoLayer.frame.renderFrameId,tracking_.epoch,spaceEpoch_,nowNs,hudScreen,hudExtent)){
                        hud.space=hudSpace_;hud.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
                        // Premultiplied alpha: do not request UNPREMULTIPLIED_ALPHA.
                        hud.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        hud.pose=identityPose();hud.pose.position.z=-2.0f;
                        hud.subImage.swapchain=hudScreen;hud.subImage.imageRect.extent={static_cast<std::int32_t>(hudExtent.width),static_cast<std::int32_t>(hudExtent.height)};
                        hud.size={2.56f,2.56f*float(hudExtent.height)/float(hudExtent.width)};hudRendered=true;
                    }
                }catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","HUD transfer disabled: %s",e.what());}
                { // HUD: newest finished HUD ring slot -> XR swapchain (no CPU wait), head quad (+ left wrist radar in HAND)
                    namespace vh=::rdrvr::vr::hud;
                    vh::Pose eyes[2]{};
                    for(unsigned i=0;i<2;++i){const auto& q=poses[i];
                        for(unsigned k=0;k<4;++k)eyes[i].q[k]=q.orientation[k];for(unsigned k=0;k<3;++k)eyes[i].p[k]=q.position[k];eyes[i].valid=validPose(q);}
                    vh::Pose grip{};
                    if(controllers_&&vh::mode()==vh::Hand){ControllerSample cs{};
                        if(controllers_->snapshot(std::uint64_t(nowNs),cs)&&validPose(cs.grip[0])){
                            for(unsigned k=0;k<4;++k)grip.q[k]=cs.grip[0].orientation[k];for(unsigned k=0;k<3;++k)grip.p[k]=cs.grip[0].position[k];grip.valid=true;}}
                    vh::xrFrame(session_,gpu_.queue,gpu_.queueFamily,local_,eyes,grip,std::uint64_t(nowNs),vrHud);
                }
            } else if(theater_ && theater_->render(session_,gpu_.queue,screen,extent)) {
                if(!anchored_) {
                    const auto forward=rotate(poses[0].orientation,{0,0,-1});
                    const float yaw=std::atan2(-forward[0],-forward[2]);
                    theaterAnchor_.orientation={0,std::sin(yaw/2),0,std::cos(yaw/2)};
                    theaterAnchor_.position={(views[0].pose.position.x+views[1].pose.position.x)*0.5f-2.0f*std::sin(yaw),
                        (views[0].pose.position.y+views[1].pose.position.y)*0.5f,
                        (views[0].pose.position.z+views[1].pose.position.z)*0.5f-2.0f*std::cos(yaw)};
                    anchored_=true;
                }
                cinema.space=local_;cinema.pose=theaterAnchor_;cinema.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
                cinema.subImage.swapchain=screen;cinema.subImage.imageRect.extent={static_cast<std::int32_t>(extent.width),static_cast<std::int32_t>(extent.height)};
                cinema.size={2.56f,2.56f*float(extent.height)/float(extent.width)};
                submitted=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cinema);rendered=true;gameFrame=true;
            } else if(!borrowed_) {
            // Diagnostic geometry belongs only to the standalone XR harness.
            for(unsigned i=0;i<2;++i) {
                auto& eye=eyes_[i]; std::uint32_t index{};
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; checkXr(xrAcquireSwapchainImage(eye.swapchain,&ai,&index),"acquire eye");
                if(!eye.cycle.acquire() || index>=eye.targets.size()) throw std::runtime_error("invalid eye acquire");
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout=XR_INFINITE_DURATION;
                const auto waited=xrWaitSwapchainImage(eye.swapchain,&wi);
                checkXr(waited,"wait eye");
                if(!eye.cycle.wait(waited==XR_SUCCESS || waited==XR_SESSION_LOSS_PENDING)) throw std::runtime_error("eye wait did not complete");
                if(waited==XR_SESSION_LOSS_PENDING) {sessionLost_=true;exit_=true;}
                const auto f=views[i].fov;
                if(!eye.cycle.submit()) throw std::runtime_error("eye submission order");
                renderer_.draw(eye.targets[index],eye.extent,i,poses[i],{f.angleLeft,f.angleRight,f.angleUp,f.angleDown},hands);
                if(!eye.cycle.complete(true)) throw std::runtime_error("GPU completion order");
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; checkXr(xrReleaseSwapchainImage(eye.swapchain,&release),"release eye");
                if(!eye.cycle.release()) throw std::runtime_error("eye release order");
                layers[i].pose=views[i].pose; layers[i].fov=views[i].fov;
                layers[i].subImage.swapchain=eye.swapchain;
                layers[i].subImage.imageRect={{0,0},{static_cast<std::int32_t>(eye.extent.width),static_cast<std::int32_t>(eye.extent.height)}};
            }
            rendered=true;diagnostic=true;
            }
        } else {++invalidViews_;graphicsMenu_.invalidate();tracking_.sample(false);recenterButton_.invalidate();if(gameViews_)gameViews_->invalidate();if(controllers_)controllers_->invalidate();controllerActions_.invalidate();if(hud_)hud_->invalidateCompleted();}
    } else { if(!frameState.shouldRender)++shouldNotRender_;graphicsMenu_.invalidate();tracking_.sample(false); recenterButton_.invalidate();if(gameViews_)gameViews_->invalidate();if(controllers_)controllers_->invalidate();controllerActions_.invalidate();if(hud_)hud_->invalidateCompleted(); }
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO}; end.displayTime=frameState.predictedDisplayTime; end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrCompositionLayerQuad stats{XR_TYPE_COMPOSITION_LAYER_QUAD};bool statsReady=false;
    if(perfOverlayEnabled_&&rendered)try{
        const auto now=diagnostics::monotonicNs();const diagnostics::PerfPanel* update=nullptr;
        if(graphicsMenu_.takeDirty())perfOverlay_.requestUpdate();
        if(heatWarning.takeRedraw())perfOverlay_.requestUpdate(); // heat01: show / hide the red lines now, not at the next 2 Hz slot
        const bool menuOpen=graphicsMenu_.open();
        if(::rdrvr::vr::welcome::frame(gameStereo,now))perfOverlay_.requestUpdate();
        const bool welcomeOn=!menuOpen&&::rdrvr::vr::welcome::active();
        {static bool wasWelcome{};if(wasWelcome!=welcomeOn){wasWelcome=welcomeOn;perfOverlay_.requestUpdate();}}
        const auto profilerMode=graphicsSettings.profilerMode();
        const bool fpsOnly=profilerMode==GraphicsSettings::ProfilerMode::Fps;
        // heat01 OVERHEAT WARNING (HeatWarning.h): the red lines do not depend on PROFILER - under the FPS line, alone (heatOnly) when
        // the readout is off, on top of DETAIL; never over the open menu.
        static_assert(diagnostics::PerfPanel::red=='\x01'&&HeatWarning::panelText[0]==diagnostics::PerfPanel::red);
        const bool heat=!menuOpen&&heatWarning.shown();
        const bool heatOnly=heat&&!fpsOnly&&!diagnostics::profilingEnabled();
        if(menuOpen&&perfOverlay_.due(now)){
            const auto pairs=diagnostics::stereoHistory.recent("CAPTURED",now);
            perfPixels_->draw(graphicsMenu_.pageText(pairs.hz,performance_.displayHz(),pairs.width,pairs.height));update=perfPixels_.get(); // GRAPHICS / VR / CHEATS
        }else if(welcomeOn&&perfOverlay_.due(now)){
            perfPixels_->draw(::rdrvr::vr::welcome::text);update=perfPixels_.get();
        }else if(!menuOpen&&!welcomeOn&&(fpsOnly||heatOnly)&&perfOverlay_.due(now)){
            char text[128];int n=0; // heat01: + the red lines (was text[32], FPS only)
            if(fpsOnly){
            const auto pairs=diagnostics::stereoHistory.recent("CAPTURED",now);
            const char* tag=framePacer.overlayTag(); // ADAPTIVE level ("36A" / "24A"), 12 glyphs fit the 156 px crop
            if(gameStereo&&pairs.events>1)n=std::snprintf(text,sizeof text,*tag?"%.1f FPS %s":"%.1f FPS",pairs.hz,tag);
            else n=std::snprintf(text,sizeof text,"-- FPS");
            }
            n=n<0?0:std::min(n,int(sizeof text)-1);text[n]=0;
            if(heat)std::snprintf(text+n,sizeof text-std::size_t(n),"%s%s",n?"\n":"",HeatWarning::panelText); // heat01
            perfPixels_->draw(text);update=perfPixels_.get();
        }else if(!menuOpen&&diagnostics::profilingEnabled()&&perfOverlay_.due(now)){
            const auto pairs=diagnostics::stereoHistory.recent("CAPTURED",now);
            const auto attempts=diagnostics::worldHistory.recent("RECORDED",now);
            const auto total=diagnostics::worldHistory.recent(nullptr,now);
            const auto ui=diagnostics::worldHistory.recent("NATIVE_UI_THEATER",now);
            const auto failure=diagnostics::worldFailureHistory.latest();
            const auto eyes=diagnostics::frameTimings.stereoDraw.recentSnapshot();
            const double xrHz=perfStamp_&&now>perfStamp_?double(frame_-perfFrame_)*1e9/double(now-perfStamp_):0;
            const auto worldAttempts=total.events-ui.events;
            const double skip=worldAttempts?100.*(worldAttempts-attempts.events)/worldAttempts:0;
            char gpuLoad[24],cpuLoad[24],cpuWorstLoad[24],drs[24];
            const auto nativeScale=resolutionTelemetry.nativeScale.load(std::memory_order_relaxed);
            if(nativeScale<=0)std::snprintf(drs,sizeof(drs),"N/A");else std::snprintf(drs,sizeof(drs),"%.0f%%",100.*nativeScale);
            const auto gpuPercent=performance_.latestPercent("/perfmetrics_meta/device/gpu_utilization"),cpuPercent=performance_.latestPercent("/perfmetrics_meta/device/cpu_utilization_average");
            if(gpuPercent<0)std::snprintf(gpuLoad,sizeof(gpuLoad),"N/A");else std::snprintf(gpuLoad,sizeof(gpuLoad),"%.0f%%",gpuPercent);
            if(cpuPercent<0)std::snprintf(cpuLoad,sizeof(cpuLoad),"N/A");else std::snprintf(cpuLoad,sizeof(cpuLoad),"%.0f%%",cpuPercent);
            const auto cpuWorst=performance_.latestPercent("/perfmetrics_meta/device/cpu_utilization_worst");
            if(cpuWorst<0)std::snprintf(cpuWorstLoad,sizeof(cpuWorstLoad),"N/A");else std::snprintf(cpuWorstLoad,sizeof(cpuWorstLoad),"%.0f%%",cpuWorst);
            const auto eyeCpu=diagnostics::threadCpuTimings.stereoDraw.recentSnapshot();
            char cpuMs[24],appCores[24];
            const auto cores=performance_.process().cores();
            if(cores<0)std::snprintf(appCores,sizeof(appCores),"N/A");else std::snprintf(appCores,sizeof(appCores),"%.2f",cores);
            if(eyeCpu.samples)std::snprintf(cpuMs,sizeof(cpuMs),"%.1f",eyeCpu.recentMean/1e6);else std::snprintf(cpuMs,sizeof(cpuMs),"N/A");
            const auto ms=[](const diagnostics::DurationWindow& w){return w.recentSnapshot().recentMean/1e6;};
            const auto& cpu=diagnostics::threadCpuTimings;
            const bool primary=engine::primaryEyeReuseLatest.load(std::memory_order_relaxed);
            char renderLoad[24];const auto load=diagnostics::renderThreadLoad.snapshot(now);
            if(load.valid)std::snprintf(renderLoad,sizeof(renderLoad),"%.0f%%",load.percent);else std::snprintf(renderLoad,sizeof(renderLoad),"N/A");
            const auto rateState=fragmentRateReadiness.load(std::memory_order_relaxed);
#ifdef RDRVR_HEAD_CAMERA
            constexpr const char* displayVersion="0.1.1";
#else
            constexpr const char* displayVersion="0.1.1";
#endif
            char descriptorReuse[40];
#ifdef RDRVR_HEAD_CAMERA
            const auto ds=engine::dynamicUniformDescriptors.snapshot();
            if(!ds.ready)std::snprintf(descriptorReuse,sizeof descriptorReuse,"OFF");
            else if(ds.calls)std::snprintf(descriptorReuse,sizeof descriptorReuse,"%.0f%% F%llu",100.*double(ds.hits)/double(ds.calls),static_cast<unsigned long long>(ds.faults));
            else std::snprintf(descriptorReuse,sizeof descriptorReuse,"WAIT");
#else
            const auto descriptorHits=engine::nativeDescriptorReuse.hits(),descriptorMisses=engine::nativeDescriptorReuse.misses();
            if(!engine::nativeDescriptorReuse.installed())std::snprintf(descriptorReuse,sizeof descriptorReuse,"OFF");
            else if(descriptorHits+descriptorMisses)std::snprintf(descriptorReuse,sizeof descriptorReuse,"%.0f%%",100.*double(descriptorHits)/double(descriptorHits+descriptorMisses));
            else std::snprintf(descriptorReuse,sizeof descriptorReuse,"WAIT");
#endif
            char pacing[96];
#ifdef RDRVR_HEAD_CAMERA
            const auto cap=engine::nativeFramePacing.snapshot();
            const char* capState="NATIVE";
            using PaceState=engine::NativeFramePacing::State;
            switch(cap.state){
            case PaceState::Baseline:capState="BASELINE";break;
            case PaceState::Active72:capState="ACTIVE";break;
            case PaceState::TimerGuard:capState="TIMER";break;
            case PaceState::Swappy:capState="SWAPPY";break;
            case PaceState::Invalid:capState="INVALID";break;
            case PaceState::SleepFailure:capState="WAIT FAIL";break;
            default:break;
            }
            if(!cap.installed||!cap.timeInstalled)std::snprintf(pacing,sizeof pacing,"VR72 HOOK OFF");
            else std::snprintf(pacing,sizeof pacing,"VR72 %s SKIP %llu BASE %.1fMS",capState,
                static_cast<unsigned long long>(cap.bypassed),
                cap.baseline?double(cap.baselineWallNs)/cap.baseline/1e6:0.);

#else
            std::snprintf(pacing,sizeof pacing,"CAP N/A");
#endif
            char cull[80]{};
#ifdef RDRVR_HEAD_CAMERA
            const auto hidden=experimental::nativeEarlyTelemetry.snapshot();
            std::snprintf(cull,sizeof(cull),"EARLY SKIP L %u/%u R %u/%u WALLS %u",
                hidden.skipped[0],hidden.skipped[1],hidden.skipped[2],hidden.skipped[3],hidden.sources);
#else
            std::snprintf(cull,sizeof(cull),"CULL N/A");
#endif
            const char* tag=framePacer.overlayTag();char xrLine[48];std::snprintf(xrLine,sizeof xrLine,*tag?"XR %.1f HZ %s":"XR %.1f HZ",xrHz,tag);
            char text[1600];const int heatN=heat?std::max(0,std::snprintf(text,sizeof text,"%s\n",HeatWarning::panelText)):0; // heat01: red lines on top
            std::snprintf(text+heatN,sizeof(text)-std::size_t(heatN),
                "RDR VR %s - %s %s\nDETAILED LOG ON\nNEW %.1f FPS   %s\nEYE %uX%u REC %uX%u DRS %s AGE %.0f MS\nEXTRA WALL %.1f MS CPU %s MS\nSCENE CPU %.1f MS /36FPS BUDGET 27.8\nLEFT  DEPTH %.1f COLOR %.1f MS CPU\nRIGHT DEPTH %.1f COLOR %.1f MS CPU\n%s\n%s\nRENDER THREAD %s - 100%% IS 1 THREAD\nAPP CPU %s CORES RSS PEAK %llu MB\nDEVICE GPU %s CPU AVG %s MAX %s DS %s\nDROP %.0f%% LAST %.30s\nDISPLAY %.0f /72 HZ HINT CPU %s GPU %s VRS %s",
                displayVersion,gameStereo?"STEREO":"THEATER",primary?"MAIN+RIGHT":"REPLAY2",pairs.hz,xrLine,pairs.width,pairs.height,resolutionTelemetry.recommendedWidth.load(),resolutionTelemetry.recommendedHeight.load(),drs,pairs.ageNs/1e6,
                eyes.recentMean/1e6,cpuMs,ms(cpu.nativeSceneInclusive),ms(primary?cpu.stockDepth:cpu.leftDepth),ms(primary?cpu.stockColor:cpu.leftColor),ms(cpu.rightDepth),ms(cpu.rightColor),
                cull,pacing,renderLoad,appCores,static_cast<unsigned long long>(performance_.process().peakRssMiB()),gpuLoad,cpuLoad,cpuWorstLoad,descriptorReuse,
                skip,failure.time?failure.state:"NONE",performance_.displayHz(),performance_.acceptedMode(0),performance_.acceptedMode(1),rateState==2&&gameStereo?"60/85":rateState?"READY":"OFF");
            perfPixels_->draw(text);update=perfPixels_.get();perfStamp_=now;perfFrame_=frame_;
        }
        statsReady=perfOverlay_.pump(now,update,hudSpace_,stats);
        // (user: the eyes had to look up a lot): 0.95 -> 0.78 m wide, centre 0.10 m below eye level (top edge ~9 deg up, was ~17)
        if(menuOpen||welcomeOn){stats.pose.position={0,-0.10f,-1.1f};stats.size={0.78f,0.78f*diagnostics::PerfPanel::height/diagnostics::PerfPanel::width};}
        if(!menuOpen&&!welcomeOn&&(fpsOnly||heatOnly)){
            // Crop the same persistent overlay to one text line; retain glyph size.
            // heat01: + the red lines (or those alone): the crop grows right / down from the FPS readout's top-left corner, so the
            // FPS text never moves (w 156, h 38 = the old quad exactly: centre -0.30, 0.24 as pump() places it).
            constexpr float W=float(diagnostics::PerfPanel::width);
            const unsigned w=heat?std::max(156u,HeatWarning::cropWidth):156u,h=heat?HeatWarning::cropHeight(HeatWarning::panelLines+(fpsOnly?1u:0u)):38u;
            stats.subImage.imageRect.extent={int(w),int(h)};
            stats.size={0.70f*float(w)/W,0.70f*float(h)/W};
            stats.pose.position.x=-0.30f-0.35f*156/W+0.5f*stats.size.width;stats.pose.position.y=0.24f+0.35f*38/W-0.5f*stats.size.height;
        }
        if(!menuOpen&&!welcomeOn&&!graphicsSettings.fpsVisible()&&!heat)statsReady=false; // heat01: the warning shows with the readout OFF
    }catch(const std::exception& e){perfOverlayEnabled_=false;__android_log_print(ANDROID_LOG_WARN,"RdrVrXR","stats disabled: %s",e.what());}
    std::array<const XrCompositionLayerBaseHeader*,6> submittedLayers{};unsigned layerCount=0; // + HUD head / wrist / map quads
    if(rendered){submittedLayers[layerCount++]=submitted;if(hudRendered)submittedLayers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud);
        if(gameStereo&&vrHud.headValid)submittedLayers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&vrHud.head);
        if(gameStereo&&vrHud.wristValid)submittedLayers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&vrHud.wrist);
        if(gameStereo&&vrHud.mapValid)submittedLayers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&vrHud.map);
        if(statsReady)submittedLayers[layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&stats);}
    // TELEPORT BLINK: XR_KHR_composition_layer_color_scale_bias (a, a, a, 1) on EVERY submitted layer while a fade
    // runs - the projection (in front of SHARPEN's settings struct), the theater / cinema fall-through (never a bright flat screen mid-blink),
    // the HUD quad and the perf / menu stats quad. Applied by the compositor at 72 Hz, also on re-submitted pairs. Alpha forced to 1 outside
    // world presentation; the heartbeat watchdog / abort latch live in the fade player.
    namespace tf=::rdrvr::vr::tploco::fade;
    const auto blinkNs=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto blink=tf::xrFrame(blinkNs,!presentation_||presentationGate_.world());
    std::array<XrCompositionLayerColorScaleBiasKHR,6> blinkScale{}; // One per submitted layer
    XrCompositionLayerColorScaleBiasKHR wristScale{XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR}; // Opaque wrist minimap
    if(gameStereo&&vrHud.wristValid&&!blink.active&&tf::extensionEnabled.load(std::memory_order_relaxed)==1&&vrHud.wristAlphaScale>1.001f){
        const float k=vrHud.wristAlphaScale;wristScale.colorScale={k,k,k,k};wristScale.colorBias={0,0,0,0};wristScale.next=vrHud.wrist.next;vrHud.wrist.next=&wristScale;}
    if(blink.active&&tf::extensionEnabled.load(std::memory_order_relaxed)==1)for(unsigned i=0;i<layerCount;++i){
        auto* base=const_cast<XrCompositionLayerBaseHeader*>(submittedLayers[i]);auto& sc=blinkScale[i];
        sc.type=XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR;sc.colorScale={blink.alpha,blink.alpha,blink.alpha,1.f};sc.colorBias={0,0,0,0};
        sc.next=base->next;base->next=&sc;
        if(submittedLayers[i]==reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cinema))::rdrvr::vr::tploco::linkBump(tf::stats.cinemaDuringFade);
    }
    end.layerCount=layerCount;end.layers=layerCount?submittedLayers.data():nullptr;
    workTiming.finish();
    const auto endBegin=diagnostics::monotonicNs();
    {diagnostics::WallScope timing(diagnostics::frameTimings.xrEnd);
     checkXr(xrEndFrame(session_,&end),"end frame");}
    tf::xrSubmitted(blink,gameStereo?presentedPair:0); // TELEPORT: ONE word per xrEndFrame {serial, alpha, flags} + the newest pair id
    // The next eye images are acquired + waited now, in the idle time before xrWaitFrame, not between the next latch and xrEndFrame
    if(gameStereo&&stereo_)try{stereo_->prefetchImages();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","stereo prefetch failed: %s",e.what());}
    const auto endDone=diagnostics::monotonicNs();
    // dynres01: the projection layer exactly as submitted (still in scope), after xrEndFrame + prefetch so it never delays the frame and stays out
    // of endUs; once per XR frame that submitted it (world stereo; the standalone diagnostic never has the extension). Result ignored.
    if(rendered&&submitted==reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer))dynresQuery.query(session_,layer,frameState.predictedDisplayTime);
    ir::frameEnd(gameStereo,!presentation_||presentationGate_.world()); // Commit this frame's inputs; record / replay start at the first world stereo frame
    framePacer.observePresented(presentedPair,frameState.predictedDisplayTime);
    {// No-layer reasons, layer frames for the XR QUEUE verify gate, xrHistory.
     const auto mailboxReason=NoLayer(presentTelemetry.lastReason.load(std::memory_order_relaxed));
     NoLayer why=NoLayer::None;
     if(!rendered){
         const bool world=!presentation_||presentationGate_.world()||stereoCutscene(presentation_);
         why=!frameState.shouldRender?NoLayer::ShouldNotRender:spacePending?NoLayer::Epoch:!trackingReady?NoLayer::NoTracking:
             mailboxAsked?(mailboxReason==NoLayer::None||mailboxReason==NoLayer::Stale?NoLayer::NoScreen:mailboxReason):NoLayer::NotWorld;
         presentTelemetry.noteNoLayer(world,why);
     }
     if(rendered&&gameStereo)xrLayerFramesSubmitted.fetch_add(1,std::memory_order_relaxed); // World stereo layers only (theater/menu never verify the split)
     const bool stale=gameStereo&&mailboxAsked&&mailboxReason==NoLayer::Stale;
     const bool newPair=gameStereo&&presentedPair&&presentedPair!=presentState.lastPair;
     if(gameStereo)presentState.lastPair=presentedPair;
     {const auto fnow=diagnostics::monotonicNs(); // Flight recorder (1 Hz row; the overlay FPS value is sampled only when a row is due)
      const float fps=framePacer.flightDue(fnow)?float(diagnostics::stereoHistory.recent("CAPTURED",fnow).hz):-1.f;
      framePacer.flightTick(fnow,gameStereo,newPair,gameStereo&&mailboxAsked&&!newPair,fps,spaceWarpControl.synthesizing());}
     const LayerKind kind=gameStereo?(stale?LayerKind::StereoStale:newPair?LayerKind::StereoNew:LayerKind::StereoRepeat):gameFrame?LayerKind::Theater:diagnostic?LayerKind::Diagnostic:LayerKind::None;
     XrFrameRecord r{};r.frame=frame_;r.wakeNs=wake;r.pdtNs=pdtMono>0?pdtMono:std::int64_t(frameState.predictedDisplayTime);
     r.waitUs=toUs(wake-waitBegin);r.workUs=toUs(endBegin>wake?endBegin-wake:0);r.endUs=toUs(endDone>endBegin?endDone-endBegin:0);
     r.periodUs=toUs(std::uint64_t(std::max<XrDuration>(frameState.predictedDisplayPeriod,0)));r.pair=presentedPair;
     r.kind=std::uint8_t(kind);r.reason=std::uint8_t(rendered?(stale?NoLayer::Stale:NoLayer::None):why);
     r.flags=std::uint8_t((newPair?XrHistory::NewPair:0)|(mailboxAsked&&presentTelemetry.lastBlit.load(std::memory_order_relaxed)?XrHistory::BlitSubmitted:0)|
                          (pdtMono>0?XrHistory::PdtMonotonic:0)|(sharpened&&rendered?XrHistory::SharpenChained:0));
     // Flag 16 NewerNotReady = this frame re-showed the displayed pair while a newer pair's copy was still
     // on the GPU (GPU-late repeat); a kind 2 row without it = nothing newer captured (render/pacer-late) or consumer busy.
     // Set only while debug.rdrvr.perfmetrics != 0 (StereoMailbox classifies only then). The pair age per latch is in
     // stereoTransfer.latch.history (join by time).
     constexpr std::uint8_t xrFlagNewerNotReady=16; // XrHistory::Flags (FramePacing.h) ends at 8
     if(gameStereo&&mailboxAsked&&stereo_&&stereo_->lastLatch()==StereoMailbox::Latch::RepeatNewerNotReady)r.flags|=xrFlagNewerNotReady;
     xrHistory.push(r);}
    spaceWarpControl.pollRuntime(instance_,session_); // /perfmetrics_meta/compositor/spacewarp_mode, 1 Hz while active
    spaceWarpMenuRuntime.store(spaceWarpControl.runtimeMode()>=0?int(spaceWarpControl.runtimeMode()):-1,std::memory_order_relaxed);
    layerMode_=gameStereo?"GAME_STEREO":gameFrame?"GAME_THEATER":diagnostic?"DIAGNOSTIC_STEREO":"NO_LAYER";
    if(rendered)++layerFrames_;
    if(diagnostic)++pairs_;
    if(gameFrame)++theaterFrames_;
    if(gameStereo)++worldStereoLayers_;
    if(hudRendered)++hudLayers_;
    if(frame_%300==0)__android_log_print(ANDROID_LOG_INFO,"RdrVrXR","layers=%llu theater=%llu should0=%llu invalidViews=%llu flags=0x%llx",static_cast<unsigned long long>(layerFrames_),static_cast<unsigned long long>(theaterFrames_),static_cast<unsigned long long>(shouldNotRender_),static_cast<unsigned long long>(invalidViews_),static_cast<unsigned long long>(viewFlags_));
    if(frame_%300==0) __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","xr_frames=%llu new_pairs=%llu tracking_epoch=%llu space_epoch=%llu focused=%d",static_cast<unsigned long long>(frame_),static_cast<unsigned long long>(pairs_),static_cast<unsigned long long>(tracking_.epoch),static_cast<unsigned long long>(spaceEpoch_),focused_);
}
XrHarness::~XrHarness() {
    ::rdrvr::vr::inputrec::sessionEnd(); // The final chunk goes to the writer (detached thread; fflush per chunk)
    controllerActions_.outputHaptics(nullptr);if(haptics_)haptics_->invalidate();
    if(controllers_)controllers_->invalidate();
    if(gameViews_)gameViews_->invalidate();
    gpu_.waitIdle();
    ::rdrvr::vr::hud::xrSessionEnded(); // HUD swapchain before the session (its consumer copies finished: idle above)
    perfOverlay_.destroy();
    if(stereo_) {
        // On a failed fence wait, leave uncertain GPU handles alive until device
        // destruction. Never throw from the session destructor or free in-flight images.
        try{stereo_->detachSession();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","stereo teardown quarantined: %s",e.what());}
    }
    if(theater_) {
        try{theater_->detachSession();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","theater teardown quarantined: %s",e.what());}
    }
    if(hud_) {
        try{hud_->detachSession();}catch(const std::exception& e){__android_log_print(ANDROID_LOG_ERROR,"RdrVrXR","HUD teardown quarantined: %s",e.what());}
    }
    for(auto& eye:eyes_) {
        for(auto& t:eye.targets) renderer_.destroyTarget(t);
        if(eye.swapchain) xrDestroySwapchain(eye.swapchain);
    }
    renderer_.destroy();
    controllerActions_.destroySpaces();
    for(auto space:handSpaces_) if(space) xrDestroySpace(space);
    if(local_) xrDestroySpace(local_);
    if(base_) xrDestroySpace(base_);
    if(hudSpace_) xrDestroySpace(hudSpace_);
    if(session_) xrDestroySession(session_);
    if(actions_) xrDestroyActionSet(actions_);
    gpu_.destroy();
    if(instance_ && !borrowed_) xrDestroyInstance(instance_);
    __android_log_print(ANDROID_LOG_INFO,"RdrVrXR","shutdown frames=%llu pairs=%llu lost=%d",static_cast<unsigned long long>(frame_),static_cast<unsigned long long>(pairs_),sessionLost_);
}
} // namespace rdrvr::runtime

