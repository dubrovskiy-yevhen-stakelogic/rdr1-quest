#pragma once
#include <cstdint>

namespace rdrvr::engine {
// The stock query describes one camera and advances a shared three-query ring.
// An explicit water-eye scope uses conservative visibility, never that stale
// camera's coverage. This does not enable water or isolate its other callbacks.
class WaterQueryBoundary {
public:
    struct Api {
        bool (*visible)(void*,unsigned,unsigned*){};
        bool (*begin)(void*){};
        void (*end)(void*){};
        explicit operator bool()const{return visible&&begin&&end;}
    };
private:
    enum class Phase {None,Visible,Open,Complete};
    struct State {
        WaterQueryBoundary* owner{};void* query{};unsigned pixels{};
        Phase phase{};bool failed{};
    };
    inline static thread_local State* active_{};
    Api api_{};
    static bool complete(const State& s){return !s.failed&&(s.phase==Phase::None||s.phase==Phase::Complete);}
public:
    class Eye {
        State state_{};State* previous_{};
    public:
        Eye(WaterQueryBoundary& owner,void* query,unsigned width,unsigned height):previous_(active_){
            state_.owner=&owner;state_.query=query;
            state_.failed=!owner.api_||!query||!width||!height||width>8192||height>8192;
            if(!state_.failed)state_.pixels=width*height;
            if(previous_){previous_->failed=true;state_.failed=true;}
            active_=&state_;
        }
        ~Eye(){active_=previous_;}
        Eye(const Eye&)=delete;Eye& operator=(const Eye&)=delete;
        bool good()const{return complete(state_);}
        bool open()const{return !state_.failed&&state_.phase==Phase::Open;}
    };
    void configure(Api api){api_=api;}
    bool visible(void* query,unsigned threshold,unsigned* samples,bool pinnedWaterCaller){
        auto* s=active_;
        if(!s||s->owner!=this)return api_.visible(query,threshold,samples);
        // Pinned PreWater sets x2 to its own aligned stack count. Validate the
        // call boundary before touching it; never read/write the query object.
        if(s->failed||!pinnedWaterCaller||query!=s->query||s->phase!=Phase::None||
           !samples||reinterpret_cast<std::uintptr_t>(samples)%alignof(unsigned)||
           (threshold!=100&&threshold!=1225)||s->pixels<=threshold){s->failed=true;return false;}
        *samples=s->pixels;s->phase=Phase::Visible;return true;
    }
    bool begin(void* query,bool pinnedWaterCaller){
        auto* s=active_;
        if(!s||s->owner!=this)return api_.begin(query);
        if(s->failed||!pinnedWaterCaller||query!=s->query||s->phase!=Phase::Visible){s->failed=true;return false;}
        s->phase=Phase::Open;return true;
    }
    void end(void* query,bool pinnedWaterCaller){
        auto* s=active_;
        if(!s||s->owner!=this){api_.end(query);return;}
        if(s->failed||!pinnedWaterCaller||query!=s->query||s->phase!=Phase::Open){s->failed=true;return;}
        s->phase=Phase::Complete;
    }
};
}
