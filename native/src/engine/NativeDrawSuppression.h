#pragma once
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include "diagnostics/DrawThreadContext.h"

namespace rdrvr::engine {
enum class NativeDrawKind : unsigned {
    Draw,Indexed,Instanced,IndexedInstanced,Count
};
// Suppress only final native primitive submission inside a matched local-body
// DrawCommon call. Keep creature updates, matrix preparation, shader callbacks
// and grmGeometryQB's skinning task exchange/wait exactly as the engine calls
// them. This is not a scene-wide visibility or actor-animation flag.
class NativeDrawSuppression {
public:
    static constexpr unsigned count=static_cast<unsigned>(NativeDrawKind::Count);
    static constexpr unsigned allMask=(1u<<count)-1;
    using DrawFn=void(*)(void*,int,int,int);
    using IndexedFn=void(*)(void*,int,int,int,int);
    using InstancedFn=void(*)(void*,int,int,int,int,int);
    using IndexedInstancedFn=void(*)(void*,int,int,int,int,int,int);
private:
    struct State {NativeDrawSuppression* owner{};bool good{true};std::array<std::uint64_t,count> skipped{};};
    // The innermost scope lives in diagnostics::drawThreadContext; hidden_ counts live
    // hiding scopes on all threads, so skip() and visible objects need no TLS while it is 0.
    static State* active()noexcept{return static_cast<State*>(diagnostics::drawThreadContext.suppression);}
    static void activate(State* s)noexcept{diagnostics::drawThreadContext.suppression=s;}
    std::atomic<unsigned> hidden_{};
    std::atomic<unsigned> installed_{};
    std::array<std::atomic<std::uint64_t>,count> skipped_{};
    std::atomic<std::uint64_t> scopes_{},rejected_{};
public:
    class Scope {
        State state_{};State* previous_{};bool hidden_{};
    public:
        Scope(NativeDrawSuppression& owner,bool hide=true):previous_(active()),hidden_(hide){
            state_.owner=&owner;
            if(hide){owner.hidden_.fetch_add(1,std::memory_order_relaxed);state_.good=owner.ready();activate(&state_);if(state_.good)++owner.scopes_;else ++owner.rejected_;}
            else activate(nullptr); // A visible nested object must not inherit suppression.
        }
        ~Scope(){activate(previous_);if(hidden_){const int result=errno;
            for(unsigned i=0;i<count;++i)if(state_.skipped[i])state_.owner->skipped_[i].fetch_add(state_.skipped[i],std::memory_order_relaxed);
            state_.owner->hidden_.fetch_sub(1,std::memory_order_relaxed);errno=result;}}
        Scope(const Scope&)=delete;Scope& operator=(const Scope&)=delete;
        bool good()const{return state_.good;}
        std::uint64_t skipped()const{std::uint64_t total{};for(auto n:state_.skipped)total+=n;return total;}
    };
    void installed(NativeDrawKind kind,bool yes=true) noexcept {
        const unsigned n=static_cast<unsigned>(kind);if(n>=count)return;
        if(yes)installed_.fetch_or(1u<<n,std::memory_order_release);else installed_.fetch_and(~(1u<<n),std::memory_order_release);
    }
    bool ready()const noexcept{return installed_.load(std::memory_order_acquire)==allMask;}
    // A hiding scope is live on some thread. False => no thread suppresses anything, so a
    // visible scope (which only clears an inherited suppression) has nothing to clear.
    bool anyHidden()const noexcept{return hidden_.load(std::memory_order_relaxed)!=0;}
    unsigned installedMask()const noexcept{return installed_.load(std::memory_order_acquire);}
    bool skip(NativeDrawKind kind) noexcept {
        if(!anyHidden())return false; // Per primitive draw, no TLS while nothing is hidden
        auto* s=active();if(!s||s->owner!=this)return false;
        const unsigned n=static_cast<unsigned>(kind);
        if(!s->good||n>=count||!ready()){s->good=false;return false;}
        ++s->skipped[n];return true;
    }
    std::uint64_t skipped(NativeDrawKind kind)const noexcept {const unsigned n=static_cast<unsigned>(kind);return n<count?skipped_[n].load(std::memory_order_relaxed):0;}
    std::uint64_t skipped()const noexcept {std::uint64_t n{};for(const auto& v:skipped_)n+=v.load(std::memory_order_relaxed);return n;}
    std::uint64_t scopes()const noexcept{return scopes_.load(std::memory_order_relaxed);}
    std::uint64_t rejected()const noexcept{return rejected_.load(std::memory_order_relaxed);}
};
inline NativeDrawSuppression nativeDrawSuppression;
}
