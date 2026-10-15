#pragma once
#include <atomic>
#include <cerrno>
#include <cstdint>

namespace rdrvr::engine {
// The native color draw ends grass visibility by zeroing its two list counts.
// Keep that cleanup pending only across the qualified MAIN color call and its
// synchronous eye consumer. Finish BEFORE returning to RenderScene: its later
// semaphore signal allows the next scene's visibility producer to run.
// No list entries, counts, allocations, or visibility computations are changed.
class ReplayGrassVisibility {
public:
    using Original=void(*)(void*);
private:
    struct State {
        ReplayGrassVisibility* owner{};Original original{};void* manager{};
        bool failed{},finished{};
    };
    inline static thread_local State* active_{};
    std::atomic<Original> original_{};
    std::atomic<bool> installed_{};
    std::atomic<std::uint64_t> held_{},cleanups_{},passed_{},faults_{};
    static void fault(State& state){
        if(!state.failed){state.failed=true;++state.owner->faults_;}
    }
    // Detach before calling native code, so exceptions/reentrancy cannot cause
    // a second cleanup from finish() or the destructor.
    static void* flush(State& state) noexcept {
        auto* manager=state.manager;state.manager=nullptr;
        if(manager){
            const int saved=errno;
            ++state.owner->cleanups_;
            try{state.original(manager);}catch(...){fault(state);}
            errno=saved;
        }
        return manager;
    }
public:
    bool configure(Original original){
        if(!original)return false;
        Original empty{};
        return original_.compare_exchange_strong(empty,original)||empty==original;
    }
    bool installed(bool value=true){
        const bool accepted=value&&original_.load()!=nullptr;installed_.store(accepted,std::memory_order_release);return accepted;
    }
    bool ready()const{return installed_.load(std::memory_order_acquire)&&original_.load()!=nullptr;}
    bool scopeActive()const{return active_&&active_->owner==this&&!active_->finished;}
    bool good()const{return ready()&&scopeActive()&&!active_->failed;}
    class Scope {
        State state_{};State* previous_{};bool installed_{};
    public:
        explicit Scope(ReplayGrassVisibility& owner,bool qualified=true):previous_(active_){
            if(!qualified)return;
            state_.owner=&owner;state_.original=owner.original_.load();
            if(previous_){
                // A nested main color boundary is outside the pinned contract.
                // Release the parent before allowing nested native work.
                fault(*previous_);flush(*previous_);fault(state_);
            }
            if(!owner.ready())fault(state_);
            active_=&state_;installed_=true;
        }
        ~Scope(){if(installed_){finish();active_=previous_;}}
        Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
        bool good()const{return installed_&&!state_.failed;}
        bool finish() noexcept {
            if(!installed_)return false;
            if(!state_.finished){state_.finished=true;flush(state_);}
            return !state_.failed;
        }
    };
    void endUsingVis(void* manager,bool exactCaller=true){
        const auto original=original_.load();
        if(!original){++faults_;return;}
        auto* state=active_;
        if(!state||state->owner!=this||state->finished){++passed_;original(manager);return;}
        if(!state->failed&&ready()&&exactCaller&&manager&&(!state->manager||state->manager==manager)){
            state->manager=manager;++held_;return;
        }
        fault(*state);
        // Unexpected caller or manager: stop retention immediately. If this
        // call itself targets the held manager, flush already performed its
        // cleanup; forwarding the same call again would clean it twice.
        auto* released=flush(*state);
        if(released!=manager||!released){++passed_;original(manager);}
    }
    std::uint64_t heldCalls()const{return held_.load();}
    std::uint64_t cleanups()const{return cleanups_.load();}
    std::uint64_t passthroughCalls()const{return passed_.load();}
    std::uint64_t faults()const{return faults_.load();}
};
} // namespace rdrvr::engine

