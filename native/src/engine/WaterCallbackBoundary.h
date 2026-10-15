#pragma once
#include "WaterQueryBoundary.h"
#include "WaterReplayState.h"
#include <optional>

namespace rdrvr::engine {
// Joins the two native callbacks into one guarded interval. An Eye exists for
// the color stage; state/query scopes exist only from PreWater to PostWater.
// Target/texture routes must surround this scope. This class installs no hooks.
class WaterCallbackBoundary {
public:
    using Original=void(*)(void*,void*);
    struct Api {
        Original pre{},post{};WaterReplayStateApi state{};WaterQueryBoundary* queries{};
        explicit operator bool()const{return pre&&post&&state&&queries;}
    };
private:
    enum class Phase {Idle,PreRunning,Depth,PostRunning,Complete};
    struct State {
        WaterCallbackBoundary* owner{};void* renderer{};void* request{};void* query{};
        unsigned width{},height{};Phase phase{};bool failed{};
        std::optional<WaterReplayState> journal;
        std::optional<WaterQueryBoundary::Eye> queries;
        bool good()const{return !failed&&(phase==Phase::Idle||phase==Phase::Complete);}
        bool close()noexcept{
            // Query failure never prevents restoring borrowed native CPU state.
            const bool queryOk=queries&&queries->good();
            const bool restored=journal&&journal->restore();
            queries.reset();journal.reset();
            return queryOk&&restored;
        }
    };
    inline static thread_local State* active_{};
    Api api_{};
public:
    class Eye {
        State state_{};State* previous_{};
    public:
        Eye(WaterCallbackBoundary& owner,void* renderer,void* request,void* query,unsigned width,unsigned height):previous_(active_){
            state_.owner=&owner;state_.renderer=renderer;state_.request=request;state_.query=query;
            state_.width=width;state_.height=height;
            state_.failed=!owner.api_||!renderer||!request||!query||!width||!height||width>8192||height>8192;
            if(previous_){previous_->failed=true;state_.failed=true;}
            active_=&state_;
        }
        ~Eye(){if(state_.journal)state_.close();active_=previous_;}
        Eye(const Eye&)=delete;Eye& operator=(const Eye&)=delete;
        bool good()const{return state_.good();}
    };
    void configure(Api api){api_=api;}
    void pre(void* renderer,void* request){
        auto* s=active_;
        if(!s||s->owner!=this){api_.pre(renderer,request);return;}
        if(s->failed||s->phase!=Phase::Idle||renderer!=s->renderer||request!=s->request){s->failed=true;return;}
        s->journal.emplace(api_.state);
        if(!s->journal->capture(renderer,request)){s->failed=true;s->journal.reset();return;}
        s->queries.emplace(*api_.queries,s->query,s->width,s->height);
        if(!s->queries->good()){s->failed=true;s->close();return;}
        s->phase=Phase::PreRunning;
        try{
            api_.pre(renderer,request);
            if(!s->queries->open()||s->failed){s->failed=true;s->close();return;}
            s->phase=Phase::Depth;
        }catch(...){s->failed=true;s->close();throw;}
    }
    void post(void* renderer,void* request){
        auto* s=active_;
        if(!s||s->owner!=this){api_.post(renderer,request);return;}
        if(s->failed||s->phase!=Phase::Depth||renderer!=s->renderer||request!=s->request){s->failed=true;return;}
        s->phase=Phase::PostRunning;
        try{
            api_.post(renderer,request);
            if(!s->close()){s->failed=true;return;}
            s->phase=Phase::Complete;
        }catch(...){s->failed=true;s->close();throw;}
    }
};
}
