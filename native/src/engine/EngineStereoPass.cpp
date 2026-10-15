#include "StereoTestMode.h"
#include "diagnostics/FrameTimings.h"
#include "diagnostics/ThreadCpuScope.h"
#include "EngineStereoPass.h"
#include "vr/HandsHook.h"
#include "vr/TaaHook.h"
#include <cerrno>
#include <cstring>
#include <sstream>
#include <cmath>

namespace rdrvr::engine {
namespace {struct Errno {int value{errno};~Errno(){errno=value;}};
// VR hands into the closed eye colour, in the pass command buffer, before the eye's capture copy
// (vr/HandsHook.h). The hook leaves every image in its received layout and restores the engine's bound state;
// it never changes `good` (like recordWarpDepth). The eye depth is the same target the AppSW depth copy reads.
// The per-eye TAA hook (vr/TaaHook.h) runs first with the same call: the TAA result is written back
// into the eye colour, then the hands are drawn over it (never into the TAA history), then the capture copy reads it.
void drawVrHands(PassResources& images,VkDevice device,const PassSourceAbi& memory,std::uint32_t family,const PassSource& color,unsigned eye,
                 std::uintptr_t depthTarget,const StereoCameraPlan& plan,std::uint64_t renderFrame,const runtime::GameStereoCapture& capture) noexcept {
    namespace hands=::rdrvr::vr::hands;
    const auto hook=hands::eyeHook.load(std::memory_order_acquire);
    const auto taa=::rdrvr::vr::taa::eyeHook.load(std::memory_order_acquire);
    if((!hook&&!taa)||eye>1)return;
    try{
        const auto target=[](const ImageRecord& r,VkImageLayout layout){hands::EyeTarget t{};t.image=r.image;t.format=r.format;
            t.extent={r.extent.width,r.extent.height};t.layout=layout;t.usage=r.usage;t.samples=r.samples;t.layers=r.layers;t.mips=r.mips;
            t.generation=r.generation;return t;};
        hands::EyePlan p{};p.sceneId=plan.sceneId;p.trackingFrame=plan.tracking.frameId;p.trackingEpoch=plan.tracking.epochs.tracking;
        p.referenceSpace=plan.tracking.epochs.referenceSpace;p.eyes=plan.tracking.eyes;p.projection=plan.projection;p.world=plan.world;
        p.unitsPerMetre=plan.unitsPerMetre;p.nearClip=plan.nearClip;p.farClip=plan.farClip;
        p.headSampledNs=plan.headAnchored?plan.headSampledNs:0; // VR weapon frame key (held hand + laser)
        hands::EyeCall call{};call.device=device;call.command=color.command;call.family=family;call.eye=eye;call.renderFrame=renderFrame;
        call.retiredFrame=capture.retiredFrame();call.color=target(color.image,color.layout);call.plan=&p;
        TargetImage depth{};
        if(depthTarget&&readTargetImage(images,device,memory,depthTarget,depth)&&depth.image.image&&depth.image.device==device)
            call.depth=target(depth.image,depth.layout);
        if(taa)taa(call);   // Before the hands; leaves every image in its received layout
        if(hook)hook(call);
    }catch(...){}
}
}
void EngineStereoPass::observeRaster(unsigned eye,SceneStage stage,std::uint64_t frame,const EngineRasterObservation& value) noexcept {
    if(eye>=2||unsigned(stage)>=3)return;
    try{
        std::lock_guard lock(rasterMutex_);auto& out=rasterStages_[eye*3+unsigned(stage)];
        ++out.samples;out.last=value;out.lastFrame=frame;
        if(!value.available)++out.unavailable;
        else if(!value.consistent)++out.mismatched;
        else if(value.fullViewport&&value.fullScissor)++out.full;
        else{++out.clipped;out.lastClipped=value;out.lastClippedFrame=frame;}
    }catch(...){}
}
std::string EngineStereoPass::rasterReport()const {
    std::array<RasterStage,6> stages{};{std::lock_guard lock(rasterMutex_);stages=rasterStages_;}
    std::ostringstream o;o<<"{\"privateStageEndpoints\":[";
    const auto number=[&](float n){if(std::isfinite(n))o<<n;else o<<"null";};
    const auto rectangle=[&](const EngineRasterObservation& r){
        o<<"{\"available\":"<<r.available<<",\"consistent\":"<<r.consistent<<",\"beforeRestore\":"<<r.beforeRestore
         <<",\"fullViewport\":"<<r.fullViewport<<",\"fullScissor\":"<<r.fullScissor
         <<",\"viewport\":["<<r.viewport.left<<','<<r.viewport.right<<','<<r.viewport.top<<','<<r.viewport.bottom
         <<"],\"scissor\":["<<r.scissor.left<<','<<r.scissor.right<<','<<r.scissor.top<<','<<r.scissor.bottom
         <<"],\"nativeViewport\":[";number(r.nativeViewport.x);o<<',';number(r.nativeViewport.y);o<<',';number(r.nativeViewport.width);o<<',';number(r.nativeViewport.height);
        o<<"],\"nativeScissor\":["<<r.nativeScissor.offset.x<<','<<r.nativeScissor.offset.y<<','<<r.nativeScissor.extent.width<<','<<r.nativeScissor.extent.height<<"]}";
    };
    for(unsigned i=0;i<stages.size();++i){const auto& s=stages[i];if(i)o<<',';
        o<<"{\"eye\":"<<i/3<<",\"stage\":"<<i%3<<",\"samples\":"<<s.samples<<",\"full\":"<<s.full<<",\"clipped\":"<<s.clipped
         <<",\"unavailable\":"<<s.unavailable<<",\"mismatched\":"<<s.mismatched<<",\"lastFrame\":"<<s.lastFrame<<",\"last\":";
        rectangle(s.last);o<<",\"lastClippedFrame\":"<<s.lastClippedFrame<<",\"lastClipped\":";rectangle(s.lastClipped);o<<'}';}
    o<<"]}";return o.str();
}
void EngineStereoPass::recordWarpDepth(VkCommandBuffer command,unsigned eye,std::uintptr_t depthTarget) noexcept {
    if(!depthTarget||!runtime::GameStereoCapture::warpWanted())return;
    try{
        TargetImage depth{};
        if(!readTargetImage(images_,device_,memory_,depthTarget,depth)||!depth.image.image)return;
        const auto& i=depth.image;
        capture_.recordDepth(command,eye,{i.image,{i.extent.width,i.extent.height},i.format,depth.layout,i.usage,i.samples,family_,0,i.layers});
    }catch(...){}
}
bool EngineStereoPass::retire() noexcept {
    Errno preserve;
    if(poisoned_)return false;
    if(!pending_)return true;
    // Completion is published only by the capture owner after the correct game
    // queue fence (or observed discard); a CPU draw return is not retirement.
    if(capture_.retiredFrame()<pending_ || !targets_.retire(pending_))return false;
    pending_=0;return true;
}
StereoPassResult EngineStereoPass::draw(VkQueue queue,EyeViewports& views,const FrameContext& frame,runtime::SourceEncoding encoding,
                                       void* gfx,void* context,Draw render,void* user,const runtime::StereoReticle& reticle,const NativeEyeSource* primary) noexcept {
    Errno preserve;
    nativeEyeReused_=false;
    status_="PASS_READINESS";
    if(!retire() || !render || !views.ready() || !buffers_.ready() || !targets_.ready() || targets_.pending())return StereoPassResult::Skipped;
    const auto& plan=views.plan();const auto& tracking=plan.tracking;
    status_="FRAME_IDENTITY";
    if(!frame.gameTickId || frame.renderFrameId!=plan.sceneId || frame.xrFrameId!=tracking.frameId ||
       frame.predictedDisplayTime!=tracking.displayTime || frame.mode!=RenderMode::Stereo ||
       frame.epochs.tracking!=tracking.epochs.tracking || frame.epochs.referenceSpace!=tracking.epochs.referenceSpace ||
       frame.epochs.resources!=targets_.generation())return StereoPassResult::Skipped;
    runtime::CaptureFrame output{};output.frame=frame;output.poses=tracking.eyes;output.fov=tracking.fov;output.encoding=encoding;
    output.reticle=reticle;
    for(unsigned i=0;i<2;++i){const auto* image=targets_.image(i);if(!image)return StereoPassResult::Skipped;
        output.extents[i]={image->image.extent.width,image->image.extent.height};output.formats[i]=image->image.format;}
    bool reusePrimary=false;
    if(primary){
        status_="PRIMARY_EYE_IDENTITY";const auto& color=primary->color;
        if(primary->sceneId!=plan.sceneId || primary->xrFrameId!=tracking.frameId ||
           primary->viewport!=views.viewport(0) || !views.matches(0))return StereoPassResult::Skipped;
        reusePrimary=color.target&&color.texture&&color.image.device==device_&&color.image.image&&color.image.generation&&
           color.image.extent.width==output.extents[0].width&&color.image.extent.height==output.extents[0].height&&
           color.image.format==output.formats[0]&&color.image.layers==1&&color.image.mips==1&&color.image.samples==VK_SAMPLE_COUNT_1_BIT;
    }
    status_="TARGET_BEGIN_USE";
    if(!targets_.beginUse(frame.renderFrameId))return StereoPassResult::Skipped;
    pending_=frame.renderFrameId;
    EnginePassBinding binding(buffers_,commands_,device_,family_);bool begun=false,good=true;
    try {
        if(binding.begin(gfx,context)){status_="CAPTURE_BEGIN";begun=capture_.beginFrame(queue,output,binding.command());}
        else status_=binding.status();
        if(begun){
            if(reusePrimary){
                diagnostics::WallScope timing(diagnostics::frameTimings.eyeCopy);
                status_="PRIMARY_EYE_CLOSE";
                good=binding.closeNativeColor(reinterpret_cast<const void*>(primary->color.target));
                if(good){
                    status_="PRIMARY_EYE_SOURCE";PassSource source{};
                    good=readPassSource(images_,device_,memory_,primary->color.target,reinterpret_cast<std::uintptr_t>(context),source) &&
                        source.command==binding.command() && source.texture==primary->color.texture &&
                        source.image.image==primary->color.image.image && source.image.generation==primary->color.image.generation;
                    if(good){status_="PRIMARY_EYE_RECORD";const auto& image=source.image;
                        drawVrHands(images_,device_,memory_,family_,source,0,primary->depthTarget,plan,pending_,capture_);
                        good=capture_.recordEye(source.command,0,{image.image,{image.extent.width,image.extent.height},image.format,source.layout,image.usage,image.samples,family_,0,image.layers});
                        nativeEyeReused_=good;
                        if(good)recordWarpDepth(source.command,0,primary->depthTarget);}
                }
                // A cropped/unavailable native source is an optimization miss,
                // not a reason to fall back to theater. If no copy was attempted
                // and the binding is sound, draw both private eyes.
                if(!good&&!binding.poisoned()&&std::strcmp(status_,"PRIMARY_EYE_RECORD"))good=true;
            }
            for(unsigned eye=nativeEyeReused_?1u:0u;eye<2 && good;++eye){
                lastColorReplayed_=false;
                status_="VIEW_ACTIVATE";good=views.activate(eye);
                if(good){status_="EYE_BIND";good=binding.eye(eye);}
                if(good){status_="RETAIL_DRAW";good=render(user,eye,views.viewport(eye),targets_.target(eye),targets_.target(eye,true),binding);}
                if(good){status_="EYE_STATE_AFTER_DRAW";good=views.matches(eye)&&(lastColorReplayed_||binding.matches(eye));}
                if(good&&!lastColorReplayed_){status_="EYE_CLOSE";good=binding.close();}
                if(good){diagnostics::WallScope timing(diagnostics::frameTimings.eyeCopy);status_="EYE_SOURCE";PassSource source{};
                    good=readPassSource(images_,device_,memory_,reinterpret_cast<std::uintptr_t>(targets_.target(eye)),reinterpret_cast<std::uintptr_t>(context),source) && source.command==binding.command();
                    if(good){status_="EYE_RECORD";const auto& image=source.image;
                        drawVrHands(images_,device_,memory_,family_,source,eye,reinterpret_cast<std::uintptr_t>(targets_.target(eye,true)),plan,pending_,capture_);
                        good=capture_.recordEye(source.command,eye,{image.image,{image.extent.width,image.extent.height},image.format,source.layout,image.usage,image.samples,family_,0,image.layers});
                        if(good)recordWarpDepth(source.command,eye,reinterpret_cast<std::uintptr_t>(targets_.target(eye,true)));
}
                }
            }
        }
    }catch(...){status_="PASS_EXCEPTION";good=false;}
    // Always restore both pieces even when either fails. Unknown command/context
    // state quarantines resources; it must not be made reusable by a later fence
    // that covers only the original command buffer.
    diagnostics::WallScope restoreTiming(diagnostics::frameTimings.passRestore);
    const bool restoredBinding=binding.restore();
    // The primary scope still owns left-eye constants. Restore left here so
    // its outer owner can restore stock after restoring the captured globals.
    bool restoredCamera=false;
    try{restoredCamera=primary?views.activate(0):views.restore();}catch(...){}
    restoreTiming.finish();
    if(!restoredBinding || !restoredCamera){status_=!restoredBinding?"RESTORE_BINDING":"RESTORE_CAMERA";poisoned_=true;if(begun)capture_.cancelFrame(pending_);return StereoPassResult::Quarantined;}
    if(!begun){
        // No target binding or eye draw can have occurred before capture begins.
        if(!targets_.retire(pending_)){poisoned_=true;return StereoPassResult::Quarantined;}
        pending_=0;return StereoPassResult::Skipped;
    }
    if(!good){capture_.cancelFrame(pending_);return StereoPassResult::Cancelled;}
    status_="PAIR_RECORDED";return StereoPassResult::Recorded;
}
StereoPassResult EngineStereoPass::drawScene(VkQueue queue,EyeViewports& views,const FrameContext& frame,runtime::SourceEncoding encoding,
                                            void* gfx,void* context,EngineDepthResolve& resolve,SceneDraw render,void* user,const runtime::StereoReticle& reticle,const NativeEyeSource* primary) noexcept {
    Errno preserve;
    status_="SCENE_READINESS";
    if(!targets_.sceneReady() || resolve.poisoned() || !render)return StereoPassResult::Skipped;
    struct Scene {
        EngineStereoPass& owner;EngineEyeTargets& targets;EyeViewports& views;EngineDepthResolve& resolve;SceneDraw draw;void* user;void* context;
        bool depthReplayed{},colorReplayed{};
        static bool eye(void* p,unsigned eye,const void* viewport,const void*,const void*,EnginePassBinding& binding){
            auto& s=*static_cast<Scene*>(p);
            auto stage=[&](SceneStage phase,EngineTargetRole role){
                auto& t=diagnostics::frameTimings;
                diagnostics::WallScope timing(phase==SceneStage::Depth?(eye?t.rightDepth:t.leftDepth):phase==SceneStage::Lighting?(eye?t.rightLight:t.leftLight):(eye?t.rightColor:t.leftColor));
                auto& c=diagnostics::threadCpuTimings;
                diagnostics::ThreadCpuScope cpuTiming(phase==SceneStage::Depth?(eye?c.rightDepth:c.leftDepth):phase==SceneStage::Lighting?(eye?c.rightLight:c.leftLight):(eye?c.rightColor:c.leftColor));
                const auto label=[&](const char* depth,const char* light,const char* color){return phase==SceneStage::Depth?depth:phase==SceneStage::Lighting?light:color;};
                if(EngineStereoPass::gpuStage)EngineStereoPass::gpuStage(eye,phase,true);
                struct GpuEnd{unsigned eye;SceneStage phase;~GpuEnd(){if(EngineStereoPass::gpuStage)EngineStereoPass::gpuStage(eye,phase,false);}} gpuEnd{eye,phase};
                s.owner.status_=label("DEPTH_VIEW_BEFORE","LIGHT_VIEW_BEFORE","COLOR_VIEW_BEFORE");if(!s.views.matches(eye))return false;
                if(EngineStereoPass::replayStage&&eye==1&&phase!=SceneStage::Lighting){
                    bool handled=false;s.owner.status_=label("DEPTH_REPLAY","LIGHT_REPLAY","COLOR_REPLAY");
                    const bool ok=EngineStereoPass::replayStage(EngineStereoPass::replayUser,phase,eye,binding,handled);
                    if(handled){if(phase==SceneStage::Depth)s.depthReplayed=true;else{s.colorReplayed=true;s.owner.lastColorReplayed_=true;}
                        if(ok)s.owner.status_=label("DEPTH_REPLAYED","LIGHT_REPLAYED","COLOR_REPLAYED");return ok;}
                }
                s.owner.status_=label("DEPTH_BIND","LIGHT_BIND","COLOR_BIND");if(!binding.eye(eye,role))return false;
                binding.resetRasterObservation();
                const auto observe=[&]{if(!binding.rasterObservation().attempted)binding.captureRasterObservation();
                    s.owner.observeRaster(eye,phase,s.views.plan().sceneId,binding.rasterObservation());};
                s.owner.status_=label("DEPTH_DRAW","LIGHT_DRAW","COLOR_DRAW");bool good{};
                try{good=s.draw(s.user,phase,eye,viewport,s.targets,binding);}catch(...){observe();throw;}
                observe();if(!good)return false;
                s.owner.status_=label("DEPTH_VIEW_AFTER","LIGHT_VIEW_AFTER","COLOR_VIEW_AFTER");if(!s.views.matches(eye))return false;
                s.owner.status_=label("DEPTH_BINDING_AFTER","LIGHT_BINDING_AFTER","COLOR_BINDING_AFTER");return binding.matches(eye,role);
            };
            if(!stage(SceneStage::Depth,EngineTargetRole::ShadowCollector))return false;
            if(!s.depthReplayed){
                s.owner.status_="DEPTH_CLOSE";if(!binding.close())return false;
                s.owner.status_="DEPTH_COPY";if(!s.resolve.copy(eye,s.context,binding.command()))return false;
            }
            if(EngineStereoPass::probeStage)EngineStereoPass::probeStage(EngineStereoPass::replayUser,eye,true);
            const bool lit=stage(SceneStage::Lighting,EngineTargetRole::LightAccumulation);
            if(EngineStereoPass::probeStage)EngineStereoPass::probeStage(EngineStereoPass::replayUser,eye,false);
            return lit &&
                stage(SceneStage::Color,EngineTargetRole::Color);
        }
    } scene{*this,targets_,views,resolve,render,user,context};
    const auto result=draw(queue,views,frame,encoding,gfx,context,Scene::eye,&scene,reticle,primary);
    if(resolve.poisoned()){poisoned_=true;return StereoPassResult::Quarantined;}
    return result;
}
}
