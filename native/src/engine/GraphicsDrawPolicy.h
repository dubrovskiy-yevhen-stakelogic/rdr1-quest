#pragma once
#include <atomic>
#include <cstdint>
#include <utility>

namespace rdrvr::engine {
// Rendering only: visibility construction/ownership, depth collection, resolve,
// and native state setup outside the qualified leaf draw always run normally.
class GraphicsDrawPolicy {
public:
    enum class Feature {SunShadows,Grass};
    struct Settings {bool sunShadows{true},grass{true};};
    struct Counts {std::uint64_t scenes{},nested{},sunCalls{},sunSkipped{},grassCalls{},grassSkipped{};};
private:
    struct State {GraphicsDrawPolicy* owner{};Settings settings{};std::uint64_t scene{};bool qualified{},failed{};};
    inline static thread_local State* current_{};
    std::atomic<bool> installed_{};
    std::atomic<std::uint64_t> scenes_{},nested_{},sunCalls_{},sunSkipped_{},grassCalls_{},grassSkipped_{};
public:
    void installed(bool value)noexcept{installed_.store(value,std::memory_order_release);}
    bool ready()const noexcept{return installed_.load(std::memory_order_acquire);}
    class Scene {
        State state_;State* previous_{};
    public:
        Scene(GraphicsDrawPolicy& owner,Settings settings,std::uint64_t scene,bool qualifiedWorld)noexcept:
            state_{&owner,settings,scene,qualifiedWorld&&scene&&owner.ready(),false},previous_(current_){
            if(previous_){previous_->failed=true;state_.failed=true;owner.nested_.fetch_add(1,std::memory_order_relaxed);}
            current_=&state_;if(state_.qualified&&!state_.failed)owner.scenes_.fetch_add(1,std::memory_order_relaxed);
        }
        ~Scene(){current_=previous_;}
        Scene(const Scene&)=delete;Scene& operator=(const Scene&)=delete;
        bool good()const noexcept{return !state_.failed;}
    };
    bool good()const noexcept{return !current_||current_->owner!=this||!current_->failed;}
    bool active()const noexcept{return current_&&current_->owner==this&&current_->qualified&&!current_->failed&&ready();}
    // true when this thread's qualified scene has the feature switched off (suppress() would skip its exact
    // draw), so work that only feeds that draw can be skipped as well (the sun-shadow cascade atlas). No counters.
    bool disabled(Feature feature)const noexcept{
        if(!active())return false;
        return feature==Feature::SunShadows?!current_->settings.sunShadows:feature==Feature::Grass?!current_->settings.grass:false;
    }
    // Caller additionally proves the pinned return address, active eye pointer,
    // and (for grass) pass 0/2. Unqualified calls always retain native behavior.
    bool suppress(Feature feature,bool exactEyeCaller)noexcept{
        if(!exactEyeCaller||!active())return false;
        if(feature==Feature::SunShadows){sunCalls_.fetch_add(1,std::memory_order_relaxed);
            if(!current_->settings.sunShadows){sunSkipped_.fetch_add(1,std::memory_order_relaxed);return true;}}
        else if(feature==Feature::Grass){grassCalls_.fetch_add(1,std::memory_order_relaxed);
            if(!current_->settings.grass){grassSkipped_.fetch_add(1,std::memory_order_relaxed);return true;}}
        return false;
    }
    template<class Native>void draw(Feature feature,bool exactEyeCaller,Native&& native){
        if(!suppress(feature,exactEyeCaller))std::forward<Native>(native)();
    }
    Counts counts()const noexcept{return {scenes_.load(std::memory_order_relaxed),nested_.load(std::memory_order_relaxed),
        sunCalls_.load(std::memory_order_relaxed),sunSkipped_.load(std::memory_order_relaxed),
        grassCalls_.load(std::memory_order_relaxed),grassSkipped_.load(std::memory_order_relaxed)};}
};
inline GraphicsDrawPolicy graphicsDrawPolicy;
}
