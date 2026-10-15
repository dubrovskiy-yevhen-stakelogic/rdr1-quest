#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>

namespace rdrvr::engine {
// Observe the first matching SetCurrent after RDR has locked matrix updates and
// installed the optional scene camera. The original functions always run once.
class CameraCallBoundary {
public:
    using Scene=void(*)(void*,void*,const void*);
    using Current=void*(*)(const void*,bool,bool,bool);
    using Observer=void(*)(const void*);
    using Finished=void(*)() noexcept;
private:
    struct Scope {CameraCallBoundary* owner;const void* viewport;void* renderer;bool eligible,captured;};
    inline static thread_local Scope* active_{};
    Scene scene_{};Current current_{};Observer observe_{};Finished finished_{};
    std::atomic<std::uint64_t> errors_{},nested_{},missed_{};
public:
    void configure(Scene scene,Current current,Observer observe,Finished finished=nullptr) {scene_=scene;current_=current;observe_=observe;finished_=finished;}
    void scene(void* renderer,void* viewport,const void* camera) {
        const int incoming=errno;
        auto* previous=active_;Scope scope{this,viewport,renderer,previous==nullptr,false};active_=&scope;
        if(previous)++nested_;
        struct Restore {
            Scope* previous;Scope& scope;std::atomic<std::uint64_t>& missed;Finished finished;
            ~Restore(){const int result=errno;if(scope.eligible && finished)finished();if(scope.eligible && !scope.captured)++missed;active_=previous;errno=result;}
        } restore{previous,scope,missed_,finished_};
        errno=incoming;scene_(renderer,viewport,camera);
    }
    void* current(const void* viewport,bool download,bool window,bool fullRes) {
        void* previous=current_(viewport,download,window,fullRes);
        const int result=errno;auto* scope=active_;
        if(scope && scope->owner==this && scope->eligible && !scope->captured && scope->viewport==viewport) {
            scope->captured=true;
            try{observe_(viewport);}catch(...){++errors_;}
        }
        errno=result;return previous;
    }
    std::uint64_t errors()const{return errors_;}
    void* renderer()const{return active_&&active_->owner==this&&active_->eligible?active_->renderer:nullptr;}
    std::uint64_t nested()const{return nested_;}
    std::uint64_t missed()const{return missed_;}
};
} // namespace rdrvr::engine
