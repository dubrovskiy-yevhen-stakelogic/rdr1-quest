#pragma once
#include <cstdint>
#include <mutex>
namespace rdrvr::runtime {
enum class PresentationContext { Unknown, World, Menu, Cutscene, CameraOverride, CameraTransition, Optic };
// A held native aim trigger must survive entering/leaving the stock optic.
// Menu/cutscene/unknown transitions still require a neutral controller sample.
inline bool preserveHeldInput(PresentationContext before,PresentationContext after){
    const auto gameplay=[](PresentationContext c){return c==PresentationContext::World||c==PresentationContext::Optic;};
    return gameplay(before)&&gameplay(after);
}
struct PresentationState {
    PresentationContext context{PresentationContext::Unknown};
    std::uint64_t revision{},publishedNs{},inputRevision{};
    bool world()const{return context==PresentationContext::World;}
};
// Main thread publishes native UI state. XR/render threads only read snapshots.
// Missing/stale observations keep the complete stock frame visible in theater.
class PresentationMailbox {
    mutable std::mutex mutex_;PresentationState state_{};bool manualTheater_{};
public:
    static constexpr std::uint64_t maxAgeNs=500'000'000;
    bool toggleManualTheater(){std::lock_guard guard(mutex_);manualTheater_=!manualTheater_;++state_.revision;++state_.inputRevision;return manualTheater_;}
    bool manualTheater()const{std::lock_guard guard(mutex_);return manualTheater_;}
    bool publish(PresentationContext context,std::uint64_t now){
        std::lock_guard guard(mutex_);
        if(!now||now<state_.publishedNs)return false;
        const bool changed=!state_.revision||state_.context!=context;
        if(changed){
            ++state_.revision;
            if(!state_.inputRevision||!preserveHeldInput(state_.context,context))++state_.inputRevision;
        }
        state_.context=context;state_.publishedNs=now;return changed;
    }
    PresentationState snapshot(std::uint64_t now)const{
        std::lock_guard guard(mutex_);auto state=state_;
        if(!state.publishedNs||now<state.publishedNs||now-state.publishedNs>maxAgeNs)state.context=PresentationContext::Unknown;
        if(manualTheater_)state.context=PresentationContext::Menu;
        return state;
    }
};
// Each consumer notices even a Menu -> World round trip between its samples.
class PresentationGate {
    std::uint64_t revision_{},inputRevision_{};bool world_{},preserveInput_{};
    PresentationContext context_{PresentationContext::Unknown};
public:
    bool update(PresentationState state){
        const bool changed=revision_!=state.revision||world_!=state.world()||context_!=state.context;
        // A Menu -> World round trip cannot masquerade as a harmless optic
        // change just because the XR consumer did not sample the menu itself.
        preserveInput_=changed&&state.inputRevision&&inputRevision_==state.inputRevision&&preserveHeldInput(context_,state.context);
        revision_=state.revision;inputRevision_=state.inputRevision;context_=state.context;world_=state.world();return changed;
    }
    bool world()const{return world_;}
    bool preservesInput()const{return preserveInput_;}
};
}
