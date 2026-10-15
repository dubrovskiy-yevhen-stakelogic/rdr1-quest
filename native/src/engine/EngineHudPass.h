#pragma once
#include "EngineFramebuffers.h"
#include "runtime/GameHudCapture.h"
#include "HudDrawRouting.h"
#include <memory>
namespace rdrvr::engine {
enum class HudPassResult {Skipped,Begun,Recorded,Cancelled,Quarantined};
// A synchronous scope around the original native HUD interval. It does not
// invoke/replay UI, change a camera, or declare CPU return to be GPU completion.
// Caller must first qualify the frame, target/alpha routing and presentation.
class EngineHudPass {
    EngineEyeTargets& targets_;EngineFramebuffers& buffers_;PassResources& images_;CommandResources& commands_;
    runtime::GameHudCapture& capture_;PassSourceAbi memory_{};VkDevice device_{};std::uint32_t family_{};
    using Clear=void(*)(bool,std::uint32_t,bool,float,bool,unsigned,int);
    Clear clear_{};std::unique_ptr<EnginePassBinding> binding_;void* context_{};
    HudDrawRouting* routing_{}; // borrowed, must outlive the pass
    std::uint64_t pending_{};bool poisoned_{};
    HudPassResult unwind(bool copy) noexcept;
public:
    EngineHudPass(EngineEyeTargets& targets,EngineFramebuffers& buffers,PassResources& images,CommandResources& commands,
                  runtime::GameHudCapture& capture,PassSourceAbi memory,VkDevice device,std::uint32_t family,Clear clear,HudDrawRouting* routing=nullptr):
        targets_(targets),buffers_(buffers),images_(images),commands_(commands),capture_(capture),memory_(memory),device_(device),family_(family),clear_(clear),routing_(routing){}
    ~EngineHudPass();
    EngineHudPass(const EngineHudPass&)=delete;EngineHudPass& operator=(const EngineHudPass&)=delete;
    bool retire() noexcept;
    HudPassResult begin(VkQueue,const FrameContext&,void* gfx,void* context) noexcept;
    HudPassResult finish() noexcept;
    HudPassResult abort() noexcept;
    bool active() const {return binding_!=nullptr;}
    bool poisoned() const {return poisoned_;}
};
}
