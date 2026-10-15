#pragma once
#include "NativeMainVisibilityBoundary.h"
#include "GameMainVisibilityWait.h"

namespace rdrvr::experimental {
// The real shared GOT wrapper calls the GameXr-side function below. Selection
// belongs to this thread-local frozen primary-depth scope, not the shared LR:
// RIGHT and unrelated native scene waits execute original without publishing.
class NativeMainVisibilityRoute {
    inline static thread_local NativeMainVisibilityBoundary* active_{};
public:
    class PrimaryDepth {
        NativeMainVisibilityBoundary* previous_{};
        NativeMainVisibilityBoundary::PrimaryDepth depth_;
    public:
        explicit PrimaryDepth(NativeMainVisibilityBoundary& boundary)noexcept:
            previous_(active_),depth_(boundary){active_=&boundary;}
        ~PrimaryDepth(){active_=previous_;}
        PrimaryDepth(const PrimaryDepth&)=delete;
    };
    static void wait(engine::MainVisibilityWait original,void* manager,std::uint32_t request,std::uintptr_t returnOffset) {
        if(auto* boundary=active_){main_join35::event(main_join35::RouteSelected);boundary->wait(reinterpret_cast<std::uintptr_t>(manager),request,returnOffset,
            [&]{original(manager,request);});}
        else {main_join35::event(main_join35::RouteAbsent);original(manager,request);}
    }
};
}
