#pragma once
#include <atomic>
#include <cerrno>
#include <utility>
namespace rdrvr::engine {
enum class HudFrameEvent {Enter,Begin,End,Abort,Leave};
// Qualifies the entire native UI interval inside one RenderFrame invocation.
// Nested frames and callbacks cannot attach HUD from another frame/renderer.
// Every original function executes exactly once, including on observer failure.
class HudFrameBoundary {
    struct State {HudFrameBoundary* owner{};void* renderer{};unsigned nested{};bool began{},ended{},aborted{},notifying{};};
    inline static thread_local State* current_{};
    using Observer=void(*)(HudFrameEvent,void*);
    Observer observer_{};
    std::atomic<unsigned long long> frames_{},begins_{},ends_{},aborts_{},errors_{},nested_{};
    bool notify(HudFrameEvent event,State& state) noexcept {
        const int saved=errno;state.notifying=true;bool good=true;
        try{if(observer_)observer_(event,state.renderer);}catch(...){++errors_;good=false;}
        state.notifying=false;errno=saved;return good;
    }
    void abort(State& s) noexcept {if(!s.aborted){s.aborted=true;++aborts_;notify(HudFrameEvent::Abort,s);}}
    State* eligible(){return current_&&current_->owner==this&&!current_->nested&&!current_->notifying&&!current_->aborted?current_:nullptr;}
public:
    void configure(Observer observer){observer_=observer;}
    template<class F> void frame(bool enabled,void* renderer,F original){
        if(current_){
            ++nested_;
            // A nested full renderer must not inherit an outer HUD target or
            // alpha route. Unwind the outer interval before executing it.
            if(current_->began&&!current_->ended&&!current_->notifying)current_->owner->abort(*current_);
            struct Nest {State& s;Nest(State& x):s(x){++s.nested;}~Nest(){--s.nested;}} nest(*current_);original(renderer);return;
        }
        if(!enabled){original(renderer);return;}
        State state{};state.owner=this;state.renderer=renderer;current_=&state;
        struct Scope {~Scope(){current_=nullptr;}} scope;
        ++frames_;if(!notify(HudFrameEvent::Enter,state))abort(state);
        try{original(renderer);}catch(...){abort(state);notify(HudFrameEvent::Leave,state);throw;}
        if(state.began&&!state.ended)abort(state);
        notify(HudFrameEvent::Leave,state);
    }
    template<class F,class... Args> void begin(bool exactCaller,F original,Args&&... args){
        original(std::forward<Args>(args)...);
        if(exactCaller)if(auto* s=eligible()){
            if(s->began||s->ended){abort(*s);return;}
            s->began=true;++begins_;if(!notify(HudFrameEvent::Begin,*s))abort(*s);
        }
    }
    template<class F,class... Args> void end(bool exactCaller,F original,Args&&... args){
        original(std::forward<Args>(args)...);
        if(exactCaller)if(auto* s=eligible()){
            if(!s->began||s->ended){abort(*s);return;}
            s->ended=true;++ends_;if(!notify(HudFrameEvent::End,*s))abort(*s);
        }
    }
    unsigned long long frames()const{return frames_;}unsigned long long begins()const{return begins_;}
    unsigned long long ends()const{return ends_;}unsigned long long aborts()const{return aborts_;}
    unsigned long long errors()const{return errors_;}unsigned long long nested()const{return nested_;}
};
}
