#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <utility>

namespace rdrvr::engine {
inline std::atomic<bool> primaryEyeReuseLatest{};
// Select a real frozen left camera only for the native main depth/light/color
// interval. RenderScene and all of its updates still execute once. Native
// targets remain native targets; the consumer must validate/copy the completed
// left image before drawing the right eye.
class PrimaryEyeBoundary {
public:
    enum class Stage {Depth,Color};
    struct Selection {
        const void* eyeViewport{};
        const void* lightingViewport{}; // Conservative union, never the stock camera.
        std::uint64_t sceneId{};
    };
    using Begin=bool(*)(void*,void* renderer,const void* stock,Selection&);
    using Validate=bool(*)(void*,void* renderer,const void* stock,const Selection&);
    using Restore=bool(*)(void*,bool complete);
private:
    enum class Phase {Idle,Setup,Depth,Shaft,Light,Color,Fault,Finished};
    struct State {
        PrimaryEyeBoundary* owner{};void* renderer{};const void* stock{};
        Selection selection{};Phase phase{Phase::Idle};
        bool eligible{},attempted{},selected{},restored{true},complete{},inside{};
    };
    inline static thread_local State* active_{};
    Begin begin_{};Validate validate_{};Restore restore_{};void* context_{};
    std::atomic<std::uint64_t> started_{},completed_{},declined_{},faults_{},restoreFailures_{};
    State* current(){return active_&&active_->owner==this?active_:nullptr;}
    bool restore(State& s) noexcept {
        if(!s.attempted)return s.restored;
        s.attempted=false; // No retry of a partially failed engine restore.
        bool good=false;
        try{good=restore_&&restore_(context_,s.complete);}catch(...){}
        s.selected=false;s.restored=good;if(!good)++restoreFailures_;
        return good;
    }
    void fault(State& s) noexcept {
        if(s.phase!=Phase::Fault){s.phase=Phase::Fault;++faults_;}
        s.complete=false;restore(s);
    }
    bool validate(State& s) noexcept {
        try{if(s.selected&&validate_&&validate_(context_,s.renderer,s.stock,s.selection))return true;}catch(...){}
        fault(s);return false;
    }
    State* qualified(void* renderer,const void* stock,bool exact,Phase expected) noexcept {
        auto* s=current();
        if(!exact||!s||!s->eligible||!s->selected)return nullptr;
        if(s->inside||s->renderer!=renderer||s->stock!=stock||s->phase!=expected){fault(*s);return nullptr;}
        return validate(*s)?s:nullptr;
    }
    struct Errno {int value{errno};~Errno(){errno=value;}};
public:
    class Scene {
        State state_{};State* previous_{};
    public:
        Scene(PrimaryEyeBoundary& owner,void* renderer,const void* stock):previous_(active_){
            Errno preserve;
            if(previous_)previous_->owner->fault(*previous_);
            state_.owner=&owner;state_.renderer=renderer;state_.stock=stock;
            state_.eligible=!previous_&&renderer&&stock;active_=&state_;
        }
        ~Scene(){Errno preserve;if(state_.selected&&state_.phase!=Phase::Color)state_.owner->fault(state_);state_.owner->restore(state_);active_=previous_;}
        Scene(const Scene&)=delete;Scene& operator=(const Scene&)=delete;
    };
    void configure(Begin begin,Validate validate,Restore restore,void* context=nullptr){begin_=begin;validate_=validate;restore_=restore;context_=context;}

    // The exact RenderScene SetupDepthInfo call is the first modified boundary.
    // Do not activate in SetCurrent's observer: RenderScene immediately writes
    // its native camera into the then-current viewport after that observer.
    template<class Native> void setup(void* renderer,bool exact,Native&& native){
        auto* s=current();bool primary=false;
        const int incoming=errno;
        if(exact&&s&&s->eligible&&s->renderer==renderer&&s->phase==Phase::Idle&&begin_&&validate_&&restore_){
            s->attempted=true;s->restored=false;
            try{
                Selection selection{};
                if(begin_(context_,renderer,s->stock,selection)&&selection.eyeViewport&&selection.lightingViewport&&selection.sceneId&&
                   selection.eyeViewport!=s->stock&&selection.lightingViewport!=s->stock){
                    s->selection=selection;s->selected=true;
                    if(validate(*s)){s->phase=Phase::Setup;++started_;primary=true;}
                }else{++declined_;s->phase=Phase::Fault;restore(*s);}
            }catch(...){fault(*s);}
        }else if(exact&&s&&s->selected)fault(*s);
        errno=incoming;
        try{std::forward<Native>(native)();}catch(...){Errno preserve;if(primary)fault(*s);throw;}
        const int result=errno;if(primary)validate(*s);errno=result;
    }

    // Callable executes the original draw exactly once. Its bool additionally
    // reports body/profile verification; false invalidates reuse, never asks
    // the coordinator to re-execute updates. Mark Color before returning to
    // RetailSceneBoundary, whose consumer runs immediately after native color.
    template<class Native> bool draw(Stage stage,void* renderer,void* stock,bool exact,Native&& native){
        const int incoming=errno;
        auto* s=qualified(renderer,stock,exact,stage==Stage::Depth?Phase::Setup:Phase::Light);
        const void* viewport=s?s->selection.eyeViewport:stock;
        if(s)s->inside=true;
        bool good{};
        errno=incoming;
        try{good=std::forward<Native>(native)(viewport,s!=nullptr);}catch(...){Errno preserve;if(s){s->inside=false;fault(*s);}throw;}
        const int result=errno;
        if(s){s->inside=false;if(!good||!s->selected)fault(*s);
            else if(validate(*s)){s->phase=stage==Stage::Depth?Phase::Depth:Phase::Color;
                if(stage==Stage::Color){s->complete=true;++completed_;}}}
        errno=result;return good;
    }

    // Only the exact stack-shaft Set in RenderScene is redirected. Depth and
    // sagWorld::DrawStart construct their own eye shafts from their viewport
    // arguments; unrelated reflection/shadow shaft calls remain unchanged.
    template<class Native> void shaft(const void* stock,const void* camera,bool exact,Native&& native){
        const int incoming=errno;
        auto* s=current();s=qualified(s?s->renderer:nullptr,stock,exact,Phase::Depth);
        const void* viewport=stock;const void* matrix=camera;
        if(s){const auto p=reinterpret_cast<std::uintptr_t>(s->selection.lightingViewport);
            if(p>UINTPTR_MAX-0x40){fault(*s);s=nullptr;}
            else{viewport=s->selection.lightingViewport;matrix=reinterpret_cast<const void*>(p+0x40);}}
        errno=incoming;
        try{std::forward<Native>(native)(viewport,matrix);}catch(...){Errno preserve;if(s)fault(*s);throw;}
        const int result=errno;if(s&&validate(*s))s->phase=Phase::Shaft;errno=result;
    }
    template<class Native> void lights(bool exact,Native&& native){
        const int incoming=errno;
        auto* s=current();s=qualified(s?s->renderer:nullptr,s?s->stock:nullptr,exact,Phase::Shaft);
        errno=incoming;
        try{std::forward<Native>(native)();}catch(...){Errno preserve;if(s)fault(*s);throw;}
        const int result=errno;if(s&&validate(*s))s->phase=Phase::Light;errno=result;
    }
    const Selection* ready(void* renderer,const void* stock) noexcept {
        Errno preserve;auto* s=qualified(renderer,stock,true,Phase::Color);return s?&s->selection:nullptr;
    }
    // Observation only: native target binds may occur inside the original draw.
    // Once Color returns, the consumer can render RIGHT and must no longer be
    // mistaken for a native-left bind. Do not invoke validation/engine calls
    // from a target-bind observer; the caller verifies its resource identity.
    const Selection* nativeSelection() const noexcept {
        const auto* s=active_;
        if(!s||s->owner!=this||!s->eligible||!s->selected)return nullptr;
        switch(s->phase){
        case Phase::Setup:case Phase::Depth:case Phase::Shaft:case Phase::Light:return &s->selection;
        default:return nullptr;
        }
    }
    // Call after the consumer has copied native left and completed/cancelled
    // right. Scene's destructor also restores after early returns/unwinding.
    bool finish() noexcept {Errno preserve;auto* s=current();if(!s)return true;
        if(s->selected&&s->phase!=Phase::Color)fault(*s);
        const bool good=restore(*s);if(good&&s->phase!=Phase::Fault)s->phase=Phase::Finished;return good;}
    bool restorationGood() noexcept {Errno preserve;auto* s=current();return !s||s->restored;}
    std::uint64_t started()const{return started_;}
    std::uint64_t completed()const{return completed_;}
    std::uint64_t declined()const{return declined_;}
    std::uint64_t faults()const{return faults_;}
    std::uint64_t restoreFailures()const{return restoreFailures_;}
};
}
