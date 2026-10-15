#pragma once
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>

namespace rdrvr::engine {
// Only these audited stateful operations are shared. Shader setup, creature
// PreDraw, matrix upload, sorting, and draw commands are NOT suppressed here.
enum class ReplayUpdate {BlendShapes,IndoorContribution,CharacterVisibility};
class ReplayUpdates {
public:
    using Original=void(*)(void*);
    using Cloud=bool(*)(void*,const void*,bool,void**);
    using TreeTimer=void(*)();
private:
    struct Entry {void* object{};ReplayUpdate kind{};};
    struct State {
        ReplayUpdates* owner{};void* renderer{};
        std::array<Entry,4096> entries{};
        bool replay{},failed{};
        unsigned shared{},newBlendShapes{};
    };
    inline static thread_local State* active_{};
    inline static thread_local unsigned depth_{};
    std::array<Original,3> originals_{};
    Cloud cloud_{};
    TreeTimer treeTimerBegin_{};
    std::array<std::atomic<std::uint64_t>,2> treeTimersSkipped_{};
    static Entry* entry(State& state,ReplayUpdate kind,void* object){
        auto index=((reinterpret_cast<std::uintptr_t>(object)>>4)^static_cast<unsigned>(kind)*0x9e3779b9u)%state.entries.size();
        for(unsigned i=0;i<state.entries.size();++i){auto& slot=state.entries[index];
            if(!slot.object||(slot.object==object&&slot.kind==kind))return &slot;
            index=(index+1)%state.entries.size();
        }
        return nullptr;
    }
public:
    class Scene {
        State* previous_{};bool owns_{};
    public:
        Scene(ReplayUpdates& owner,void* renderer):previous_(active_){
            const int saved=errno;
            if(depth_++==0&&renderer){
                static thread_local State storage;
                storage={};storage.owner=&owner;storage.renderer=renderer;active_=&storage;owns_=true;
            }else{if(previous_)previous_->failed=true;active_=nullptr;}
            errno=saved;
        }
        ~Scene(){active_=previous_;--depth_;}
        Scene(const Scene&)=delete;Scene& operator=(const Scene&)=delete;
        bool good()const{return owns_&&active_&&!active_->failed;}
    };
    class Eye {
        State* state_{};
    public:
        Eye(ReplayUpdates& owner,void* renderer){
            auto* state=active_;
            if(state&&state->owner==&owner&&state->renderer==renderer&&!state->replay&&!state->failed){state_=state;state->replay=true;}
            else if(state)state->failed=true;
        }
        ~Eye(){if(state_)state_->replay=false;}
        Eye(const Eye&)=delete;Eye& operator=(const Eye&)=delete;
        explicit operator bool()const{return state_!=nullptr;}
        bool good()const{return state_&&!state_->failed;}
    };
    void configure(std::array<Original,3> originals){originals_=originals;}
    void configureCloud(Cloud cloud){cloud_=cloud;}
    void configureTreeTimer(TreeTimer begin){treeTimerBegin_=begin;}
    bool skipTreeTimer(bool end){
        auto* state=active_;
        if(state&&state->owner==this&&state->replay){++treeTimersSkipped_[unsigned(end)];return true;}
        return false;
    }
    void treeTimerBegin(){
        // Native stock rendering owns the 45-query pool. Extra eye draws must
        // neither pop queries nor end the last stock query a second time.
        if(!skipTreeTimer(false))treeTimerBegin_();
    }
    std::uint64_t treeTimersSkipped(bool end)const{return treeTimersSkipped_[unsigned(end)].load();}
    bool cloud(void* controller,const void* parameters,bool frozen,void** textures){
        auto* state=active_;
        const bool ours=state&&state->owner==this;
        // SetupShader still uploads per-draw constants and returns its original
        // bool. Only its time-integration branch is disabled during eye replay.
        try{return cloud_(controller,parameters,frozen||(ours&&state->replay),textures);}
        catch(...){if(ours)state->failed=true;throw;}
    }
    bool good(void* renderer)const{return active_&&active_->owner==this&&active_->renderer==renderer&&!active_->failed;}
    void invoke(ReplayUpdate kind,void* object){
        const auto index=static_cast<unsigned>(kind);auto original=originals_[index];
        auto* state=active_;
        if(!state||state->owner!=this){original(object);return;}
        auto* slot=object?entry(*state,kind,object):nullptr;
        if(state->replay){
            if(state->failed||!slot){state->failed=true;return;}
            if(slot->object){++state->shared;return;}
            // A creature newly exposed by the wider stereo frustum may need
            // its first morph update. Scene-wide interpolation/queries must
            // already have run in the stock pass; never start them per eye.
            if(kind!=ReplayUpdate::BlendShapes){state->failed=true;return;}
            slot->object=object;slot->kind=kind;++state->newBlendShapes;
            try{original(object);}catch(...){state->failed=true;throw;}
        }else{
            // Preserve ALL stock invocations, even repeated calls for an
            // object. Recording a key is not permission to change mono code.
            // Reserve before entering engine code: it may recursively invoke
            // another audited update whose hash collides with this key.
            if(slot){slot->object=object;slot->kind=kind;}else state->failed=true;
            try{original(object);}catch(...){state->failed=true;throw;}
        }
    }
};
} // namespace rdrvr::engine
