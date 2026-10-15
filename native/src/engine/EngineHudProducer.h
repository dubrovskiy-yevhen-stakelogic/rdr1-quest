#pragma once
#include "EngineHudPass.h"
#include "HudNativeRouting.h"
namespace rdrvr::engine {
struct HudProducerApi {
    HudResourceApi resources{};EngineTargetApi targets{};EngineFramebufferApi framebuffers{};HudRenderEntry render{};
    std::uintptr_t factoryGlobal{},deviceGlobal{},deviceVtable{};
    void (*clear)(bool,std::uint32_t,bool,float,bool,unsigned,int){};
    bool (*overlay)(void*,VkDevice,VkExtent2D,VkFormat){};void* overlayContext{};
};
// One render-thread owner for the complete original HUD interval. No native UI
// callbacks are replayed. Target use is retired by GameHudCapture's real queue
// completion/discard notifications, never by the end of a CPU draw callback.
class EngineHudProducer {
    HudProducerApi api_;PassResources& images_;CommandResources& commands_;runtime::GameHudCapture& capture_;
    VkDevice device_{};std::uint32_t family_{};void *factory_{},*nativeDevice_{};
    std::unique_ptr<EngineEyeTargets> targets_;
    std::unique_ptr<EngineFramebuffers> buffers_;
    std::unique_ptr<HudNativeRouting> nativeRouting_;
    std::unique_ptr<HudDrawRouting> routing_;
    std::unique_ptr<EngineHudPass> pass_;
    bool disabled_{};const char* status_{"WAITING_FOR_MATCHING_WORLD"};
    bool owners(void*&,void*&) const;
    bool skip(const char* why){status_=why;return false;}
public:
    struct Counts {std::uint64_t begun{},recorded{},cancelled{};HudDrawRouting::Counts routing{};} counts;
    EngineHudProducer(HudProducerApi api,PassResources& images,CommandResources& commands,runtime::GameHudCapture& capture,
                      VkDevice device,std::uint32_t family):api_(api),images_(images),commands_(commands),capture_(capture),device_(device),family_(family){}
    ~EngineHudProducer(){abort();if(pass_)pass_->retire();}
    EngineHudProducer(const EngineHudProducer&)=delete;
    EngineHudProducer& operator=(const EngineHudProducer&)=delete;
    bool begin(VkQueue,const FrameContext&) noexcept;
    bool finish() noexcept;
    void abort() noexcept;
    bool active()const{return pass_&&pass_->active();}
    const char* status()const{return status_;}
};
}
