#include "EngineHudProducer.h"
namespace rdrvr::engine::px2d {
// PX 2D (TraceEntry.cpp, debug.rdrvr.px2d bit 1). Weak references: a build without them keeps the native path unchanged.
__attribute__((weak)) bool producerFlushAsk() noexcept;
__attribute__((weak)) void producerFlushNote(unsigned event) noexcept;
__attribute__((weak)) void producerBegun(bool active) noexcept; // begin() exit, active() = this frame's HUD is captured
}
namespace rdrvr::engine {
namespace {struct Errno {int saved{errno};~Errno(){errno=saved;}};
// PX 2D: the dormant verdict of the last asked native run that had to flush (HUD_STOCK_COMPOSITOR_UNAVAILABLE after the flush),
// keyed by the pre-flush binding, with the post-flush size/format the overlay check saw. Render thread only (one producer per thread).
// The image registry generation is part of the key (a recycled VkImage handle never matches), and any native flush run
// that was not asked (px2d 0 / 2, gate pre-check false) drops the memo, so a later asked run never reuses a verdict older than it.
struct DormantMemo {
    bool valid{},factoryDirty{};std::uintptr_t gfx{},context{},framebuffer{},surface{},desired{},actual{};std::uint32_t width{},height{},framebufferId{};
    VkImage image{};VkFormat format{};std::uint64_t generation{};std::uint32_t outWidth{},outHeight{};VkFormat outFormat{};unsigned hits{};
};
thread_local DormantMemo dormantMemo;
bool dormantKey(const DormantMemo& m,const HudResourceSet& s) noexcept {
    return m.valid&&m.factoryDirty==s.factoryDirty&&m.gfx==s.gfx&&m.context==s.context&&m.framebuffer==s.framebuffer&&m.surface==s.surface&&
        m.desired==s.desired&&m.actual==s.actual&&m.width==s.width&&m.height==s.height&&m.framebufferId==s.framebufferId&&
        m.image==s.image.image&&m.format==s.image.format&&m.generation==s.image.generation;
}
void dormantStore(const HudResourceSet& pre,const HudResourceSet& post) noexcept {
    auto& m=dormantMemo;m=DormantMemo{};
    m.factoryDirty=pre.factoryDirty;m.gfx=pre.gfx;m.context=pre.context;m.framebuffer=pre.framebuffer;m.surface=pre.surface;m.desired=pre.desired;
    m.actual=pre.actual;m.width=pre.width;m.height=pre.height;m.framebufferId=pre.framebufferId;m.image=pre.image.image;m.format=pre.image.format;
    m.generation=pre.image.generation;m.outWidth=post.width;m.outHeight=post.height;m.outFormat=post.image.format;m.valid=true;
}
void px2dNote(unsigned event) noexcept {if(px2d::producerFlushNote)px2d::producerFlushNote(event);}
}
bool EngineHudProducer::owners(void*& factory,void*& device) const {
    const auto& m=api_.resources.memory;
    const auto read=[&](std::uintptr_t p,auto& value){return p&&p%8==0&&p<=UINTPTR_MAX-sizeof(value)&&m.read(m.context,p,&value,sizeof(value));};
    std::uintptr_t f{},d{},ft{},dt{};
    if(!read(api_.factoryGlobal,f)||!read(api_.deviceGlobal,d)||!read(f,ft)||!read(d,dt)||
       ft!=api_.targets.factoryVtable||dt!=api_.deviceVtable)return false;
    factory=reinterpret_cast<void*>(f);device=reinterpret_cast<void*>(d);return true;
}
bool EngineHudProducer::begin(VkQueue queue,const FrameContext& world) noexcept {
    Errno preserve;
    // On every exit, tell the lock-free px2d gate whether this frame's HUD interval is now held (it then stays native).
    struct Px2dBegun {const EngineHudProducer& p;~Px2dBegun(){if(px2d::producerBegun)px2d::producerBegun(p.active());}} px2dBegun{*this};
    try{
        if(disabled_)return skip("HUD_DISABLED_AFTER_INCOMPLETE_INTERVAL");
        if(active())return skip("HUD_INTERVAL_ALREADY_ACTIVE");
        if(!queue||!world.renderFrameId||world.mode!=RenderMode::Stereo||!api_.resources.memory.read||
           !api_.targets||!api_.framebuffers||!api_.render||!api_.render.factoryFlush||!api_.clear||!api_.overlay)return skip("HUD_APIS_UNAVAILABLE");
        HudResources resources(api_.resources,images_,commands_,device_,family_);HudResourceSet before{},after{};
        if(resources.inspect(before)!=HudResourceStatus::Ready)return skip("HUD_NATIVE_SOURCE_UNAVAILABLE");
        bool memoRun=false;HudResourceSet pre{}; // PX 2D: an asked run that flushed natively (stores the dormant memo)
        if(before.factoryDirty||before.pendingClears||!before.renderPass||before.desired!=before.framebuffer||before.actual!=before.framebuffer){
            // PX 2D (debug.rdrvr.px2d bit 1, gated pair frame whose HUD/UI span is dropped bind-only + no-flush): when the same
            // pre-flush binding last ended dormant and the overlay still refuses that HUD size/format, the eager flush below is left
            // to the next native Clear/draw (the engine's own order) and the verdict is the same; every 64th hit re-verifies natively.
            if(px2d::producerFlushAsk&&px2d::producerFlushAsk()){
                auto& m=dormantMemo;
                if(!dormantKey(m,before)){m.valid=false;px2dNote(1);}
                else if(++m.hits%64==0)px2dNote(2);
                else{
                    const auto f=m.outFormat;
                    const bool c4=f==VK_FORMAT_B8G8R8A8_UNORM||f==VK_FORMAT_B8G8R8A8_SRGB||f==VK_FORMAT_R8G8B8A8_UNORM||f==VK_FORMAT_R8G8B8A8_SRGB;
                    if(!c4||!api_.overlay(api_.overlayContext,device_,{m.outWidth,m.outHeight},f)){px2dNote(0);return skip("HUD_STOCK_COMPOSITOR_UNAVAILABLE");}
                    m.valid=false;px2dNote(3);
                }
                memoRun=true;pre=before;
            }else dormantMemo.valid=false; // An unasked native flush run; its verdict is not stored, so the memo may be stale
            // RenderFrame can leave a post-FX factory target pending immediately
            // before grcWorldIdentity. Consume it through the same native APIs
            // the next Clear/draw uses, then discover the actual HUD attachment.
            void *factory{},*device{};if(!owners(factory,device))return skip("HUD_NATIVE_OWNERS_UNAVAILABLE");
            api_.render.factoryFlush();api_.framebuffers.flush(reinterpret_cast<void*>(before.context));
            if(resources.inspect(before)!=HudResourceStatus::Ready||before.factoryDirty||before.pendingClears||!before.renderPass||
               before.desired!=before.framebuffer||before.actual!=before.framebuffer)return skip("HUD_NATIVE_BINDING_PENDING");
        }
        const auto format=before.image.format;
        const bool bgra=format==VK_FORMAT_B8G8R8A8_UNORM||format==VK_FORMAT_B8G8R8A8_SRGB;
        const bool rgba=format==VK_FORMAT_R8G8B8A8_UNORM||format==VK_FORMAT_R8G8B8A8_SRGB;
        if((!bgra&&!rgba)||!api_.overlay(api_.overlayContext,device_,{before.width,before.height},format)){
            if(memoRun){dormantStore(pre,before);px2dNote(4);}
            return skip("HUD_STOCK_COMPOSITOR_UNAVAILABLE");
        }
        if(memoRun)dormantMemo.valid=false; // PX 2D: the overlay accepts this HUD target
        if(before.depth&&!before.stencil)return skip("HUD_NATIVE_DEPTH_HAS_NO_STENCIL");
        void *factory{},*device{};if(!owners(factory,device))return skip("HUD_NATIVE_OWNERS_UNAVAILABLE");
        if(pass_&&!pass_->retire())return skip("HUD_PREVIOUS_GPU_USE_PENDING");
        if(factory_!=factory||nativeDevice_!=device){
            pass_.reset();routing_.reset();nativeRouting_.reset();buffers_.reset();targets_.reset();
            factory_=factory;nativeDevice_=device;
        }
        if(!targets_){
            auto targets=std::make_unique<EngineEyeTargets>(api_.targets,images_,device_,factory);
            auto buffers=std::make_unique<EngineFramebuffers>(api_.framebuffers,*targets,device);
            auto nativeRouting=std::make_unique<HudNativeRouting>(api_.resources,api_.deviceGlobal,api_.deviceVtable,api_.render);
            auto routing=std::make_unique<HudDrawRouting>(nativeRouting->api());
            auto pass=std::make_unique<EngineHudPass>(*targets,*buffers,images_,commands_,capture_,api_.resources.memory,device_,family_,api_.clear,routing.get());
            targets_=std::move(targets);buffers_=std::move(buffers);nativeRouting_=std::move(nativeRouting);routing_=std::move(routing);pass_=std::move(pass);
        }
        // Pinned type=2,bits=32 selects native SurfaceFormat 45 (D24S8).
        // The image registry must confirm the actual Vulkan format afterwards.
        EngineTargetSpec spec{before.width,before.height,bgra?2u:26u,format,VK_FORMAT_D24_UNORM_S8_UINT};
        spec.mono=spec.transparent=true;spec.colorSrgb=format==VK_FORMAT_B8G8R8A8_SRGB||format==VK_FORMAT_R8G8B8A8_SRGB;
        if(!targets_->prepare(spec)||!buffers_->prepare())return skip("HUD_MONO_TARGETS_UNAVAILABLE");
        void *factoryAfter{},*deviceAfter{};
        if(!owners(factoryAfter,deviceAfter)||factoryAfter!=factory||deviceAfter!=device||
           resources.inspect(after)!=HudResourceStatus::Ready||!HudResources::sameImage(before,after))return skip("HUD_SOURCE_CHANGED_DURING_PREPARE");
        auto frame=world;frame.epochs.resources=targets_->generation();
        const auto result=pass_->begin(queue,frame,reinterpret_cast<void*>(before.gfx),reinterpret_cast<void*>(before.context));
        if(result!=HudPassResult::Begun){
            if(result==HudPassResult::Quarantined||result==HudPassResult::Cancelled)disabled_=true;
            return skip(result==HudPassResult::Quarantined?"HUD_BEGIN_QUARANTINED":"HUD_BEGIN_DECLINED");
        }
        ++counts.begun;status_="HUD_NATIVE_INTERVAL_ACTIVE";return true;
    }catch(...){abort();return skip("HUD_PREPARE_EXCEPTION");}
}
bool EngineHudProducer::finish() noexcept {
    Errno preserve;if(!active())return false;
    const auto result=pass_->finish();counts.routing=routing_->counts;
    if(result==HudPassResult::Recorded){++counts.recorded;status_="HUD_MONO_COPY_RECORDED";return true;}
    ++counts.cancelled;disabled_=true;
    status_=result==HudPassResult::Quarantined?"HUD_RESTORE_QUARANTINED":
        (counts.routing.unsupported?"HUD_UNSUPPORTED_BLEND":"HUD_INTERVAL_CANCELLED");return false;
}
void EngineHudProducer::abort() noexcept {
    Errno preserve;if(!active())return;
    const auto result=pass_->abort();counts.routing=routing_->counts;++counts.cancelled;disabled_=true;
    status_=result==HudPassResult::Quarantined?"HUD_ABORT_QUARANTINED":"HUD_INTERVAL_ABORTED";
}
}
