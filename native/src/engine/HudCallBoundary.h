#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <utility>

namespace rdrvr::engine {
enum class HudStage : unsigned { Inventory,Flash,Ui,Radar,Count };
enum class HudPhase { Before,After,Aborted };
// The retail UI remains the sole owner of its updates and draw calls. Observer
// exceptions cannot suppress or repeat a draw. No viewport/object is retained.
class HudCallBoundary {
    inline static thread_local bool observing_{};
    using Observer=void(*)(HudStage,HudPhase);
    Observer observer_{};
    std::atomic<std::uint64_t> qualified_{},completed_{},aborted_{},errors_{},nested_{};
    void notify(HudStage stage,HudPhase phase) noexcept {
        const int saved=errno;
        try{if(observer_)observer_(stage,phase);}catch(...){++errors_;}
        errno=saved;
    }
public:
    void configure(Observer observer){observer_=observer;}
    template<class F,class... Args> void call(HudStage stage,bool exactCaller,F original,Args&&... args){
        if(!exactCaller || observing_){if(exactCaller && observing_)++nested_;original(std::forward<Args>(args)...);return;}
        struct Scope {Scope(){observing_=true;}~Scope(){observing_=false;}} scope;
        ++qualified_;notify(stage,HudPhase::Before);
        try{original(std::forward<Args>(args)...);}catch(...){++aborted_;notify(stage,HudPhase::Aborted);throw;}
        ++completed_;notify(stage,HudPhase::After);
    }
    std::uint64_t qualified()const{return qualified_;}
    std::uint64_t completed()const{return completed_;}
    std::uint64_t aborted()const{return aborted_;}
    std::uint64_t errors()const{return errors_;}
    std::uint64_t nested()const{return nested_;}
};
}
