#include "EngineHudPass.h"
#include <cerrno>
namespace rdrvr::engine {
namespace {struct Errno {int value{errno};~Errno(){errno=value;}};}
bool EngineHudPass::retire() noexcept {
    Errno preserve;if(poisoned_||binding_)return false;if(!pending_)return true;
    if(capture_.retiredFrame()!=pending_||!targets_.retire(pending_))return false;pending_=0;return true;
}
HudPassResult EngineHudPass::begin(VkQueue queue,const FrameContext& frame,void* gfx,void* context) noexcept {
    Errno preserve;
    if(!retire()||!clear_||!buffers_.ready()||!targets_.transparentMono()||targets_.pending()||
       !frame.gameTickId||!frame.renderFrameId||frame.epochs.resources!=targets_.generation())return HudPassResult::Skipped;
    const auto* source=targets_.image(0);const auto* depth=targets_.image(0,true);
    // Native SWF uses stencil for clipping; a depth-only target is not a valid
    // replacement even when its dimensions and color image match perfectly.
    if(!source||!depth||(depth->image.format!=VK_FORMAT_D24_UNORM_S8_UINT&&depth->image.format!=VK_FORMAT_D32_SFLOAT_S8_UINT))return HudPassResult::Skipped;
    if(!targets_.beginUse(frame.renderFrameId))return HudPassResult::Skipped;
    pending_=frame.renderFrameId;bool begun=false;
    try{
        binding_=std::make_unique<EnginePassBinding>(buffers_,commands_,device_,family_);context_=context;
        if(binding_->begin(gfx,context))begun=capture_.beginFrame(queue,frame,{source->image.extent.width,source->image.extent.height},source->image.format,binding_->command());
        if(begun&&binding_->eye(0)){
            // grcDevice::Clear is static: packed Color32 in W1, depth in S0.
            clear_(true,0,true,1.0f,true,0,0xff);
            if(binding_->flushDraw(0)&&(!routing_||routing_->begin(gfx,binding_->originalFramebuffer(),binding_->framebuffer())))return HudPassResult::Begun;
        }
    }catch(...){}
    if(begun)return unwind(false);
    const bool restored=!binding_||binding_->restore();binding_.reset();context_=nullptr;
    if(!restored||!targets_.retire(pending_)){poisoned_=true;return HudPassResult::Quarantined;}
    pending_=0;return HudPassResult::Skipped;
}
HudPassResult EngineHudPass::unwind(bool copy) noexcept {
    Errno preserve;if(!binding_)return poisoned_?HudPassResult::Quarantined:HudPassResult::Skipped;
    bool good=false;
    const bool routed=!routing_||routing_->end();
    try{
        if(copy&&routed&&binding_->finishDraw(0)&&binding_->close()){
            PassSource source{};
            if(readPassSource(images_,device_,memory_,reinterpret_cast<std::uintptr_t>(targets_.target(0)),reinterpret_cast<std::uintptr_t>(context_),source)&&source.command==binding_->command()){
                const auto& image=source.image;
                good=capture_.record(source.command,{image.image,{image.extent.width,image.extent.height},image.format,source.layout,image.usage,image.samples,family_,0,image.layers});
            }
        }
    }catch(...){}
    const bool restored=binding_->restore();binding_.reset();context_=nullptr;
    if(!restored){capture_.cancelFrame(pending_);poisoned_=true;return HudPassResult::Quarantined;}
    if(!good){capture_.cancelFrame(pending_);return HudPassResult::Cancelled;}
    return HudPassResult::Recorded;
}
HudPassResult EngineHudPass::finish() noexcept {return unwind(true);}
HudPassResult EngineHudPass::abort() noexcept {return unwind(false);}
EngineHudPass::~EngineHudPass(){abort();}
}
