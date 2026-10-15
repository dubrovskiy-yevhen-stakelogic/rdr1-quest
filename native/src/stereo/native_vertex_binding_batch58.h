#pragma once
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string>

namespace rdrvr::experimental {
// Combine binding updates only inside one native BindInputLayout invocation.
// No desired binding or buffer pointer survives the lexical input scope.
template<class Command, class Buffer, class Size, class Function>
class NativeVertexBindingBatch58 {
public:
    ~NativeVertexBindingBatch58() {
        if(localCounters_.owner==this){publish();localCounters_={};}
    }
    class Scope;
    struct Stats { std::uint64_t inputScopes{}, sourceCalls{}, emittedCalls{}, sourceBindings{}, emittedBindings{}, fallbacks{}; bool enabled{}, stable{}; };
private:
    static constexpr unsigned capacity = 32;
    inline static thread_local Scope* active_{};
    // Recording threads must finish before their owner is destroyed. The live
    // integration uses one owner with process lifetime.
    struct Counters { NativeVertexBindingBatch58* owner{}; Stats values{}; };
    inline static thread_local Counters localCounters_{};
    std::atomic<bool> enabled_{}, terminal_{};
    std::atomic<std::uint64_t> sequence_{};
    std::atomic<std::uint64_t> scopes_{}, sourceCalls_{}, emittedCalls_{}, sourceBindings_{}, emittedBindings_{}, fallbacks_{};
public:
    void enable() noexcept {
        if (!terminal_.load(std::memory_order_acquire)) {
            enabled_.store(true, std::memory_order_release);
            if (terminal_.load(std::memory_order_acquire)) enabled_.store(false, std::memory_order_release);
        }
    }
    bool enabled() const noexcept {return enabled_.load(std::memory_order_acquire);}
    void disable() noexcept {
        boundary(); publish(); terminal_.store(true, std::memory_order_release); enabled_.store(false, std::memory_order_release);
    }
    // Called at command completion, and at most once per 128 input scopes.
    // Counter updates in an individual input scope remain ordinary TLS writes.
    void publish() noexcept {
        auto& block=localCounters_;auto& s=block.values;
        if(block.owner!=this||!s.inputScopes)return;
        auto seq=sequence_.load(std::memory_order_relaxed);
        if((seq&1)||seq>=UINT64_MAX-2||!sequence_.compare_exchange_strong(seq,seq+1,std::memory_order_acq_rel))return;
        scopes_.store(scopes_.load(std::memory_order_relaxed)+s.inputScopes,std::memory_order_relaxed);
        sourceCalls_.store(sourceCalls_.load(std::memory_order_relaxed)+s.sourceCalls,std::memory_order_relaxed);
        emittedCalls_.store(emittedCalls_.load(std::memory_order_relaxed)+s.emittedCalls,std::memory_order_relaxed);
        sourceBindings_.store(sourceBindings_.load(std::memory_order_relaxed)+s.sourceBindings,std::memory_order_relaxed);
        emittedBindings_.store(emittedBindings_.load(std::memory_order_relaxed)+s.emittedBindings,std::memory_order_relaxed);
        fallbacks_.store(fallbacks_.load(std::memory_order_relaxed)+s.fallbacks,std::memory_order_relaxed);
        sequence_.store(seq+2,std::memory_order_release);s={};
    }
    void recordGroup(std::uint64_t sourceCalls,std::uint64_t emittedCalls,std::uint64_t sourceBindings,std::uint64_t emittedBindings,std::uint64_t fallbacks=0) noexcept {
        auto& block=localCounters_;
        if(block.owner!=this) {
            if(block.owner)block.owner->publish();
            if(block.values.inputScopes)return;
            block.owner=this;
        }
        auto& counts=block.values;
        ++counts.inputScopes;counts.sourceCalls+=sourceCalls;counts.emittedCalls+=emittedCalls;
        counts.sourceBindings+=sourceBindings;counts.emittedBindings+=emittedBindings;counts.fallbacks+=fallbacks;
        if(counts.inputScopes>=128)publish();
    }
    Stats snapshot() const noexcept {
        Stats s{};
        for(unsigned attempt=0;attempt<3;++attempt) {
            const auto seq=sequence_.load(std::memory_order_acquire);if(seq&1)continue;
            s={scopes_.load(std::memory_order_relaxed), sourceCalls_.load(std::memory_order_relaxed),
                emittedCalls_.load(std::memory_order_relaxed), sourceBindings_.load(std::memory_order_relaxed),
                emittedBindings_.load(std::memory_order_relaxed), fallbacks_.load(std::memory_order_relaxed),
                enabled_.load(std::memory_order_acquire),false};
            if(seq==sequence_.load(std::memory_order_acquire)){s.stable=true;break;}
        }
        return s;
    }
    std::string report() const {
        const auto s=snapshot();
        return "{\"stable\":"+std::string(s.stable?"true":"false")+",\"enabled\":"+std::string(s.enabled?"true":"false")+",\"inputScopes\":"+std::to_string(s.inputScopes)+
            ",\"sourceCalls\":"+std::to_string(s.sourceCalls)+",\"emittedCalls\":"+std::to_string(s.emittedCalls)+
            ",\"sourceBindings\":"+std::to_string(s.sourceBindings)+",\"emittedBindings\":"+std::to_string(s.emittedBindings)+
            ",\"fallbacks\":"+std::to_string(s.fallbacks)+"}";
    }
    class Scope {
        friend class NativeVertexBindingBatch58;
        NativeVertexBindingBatch58& owner_;
        Scope* previous_{};
        Command command_{};
        Function route_{};
        std::array<Buffer,capacity> buffers_;
        std::array<Size,capacity> offsets_;
        std::uint32_t pending_{};
        std::uint64_t sourceCalls_{}, emittedCalls_{}, sourceBindings_{}, emittedBindings_{}, fallbacks_{};
        bool qualified_{}, busy_{}, finished_{};
        void drain() noexcept {
            if (busy_ || !pending_) return;
            const auto mask=pending_; pending_=0; busy_=true;
            // Sparse updates remain sparse: never bind a slot absent from the
            // original calls. An original update is fully copied before return.
            for(unsigned first=0;first<capacity;) {
                if (!(mask&(std::uint32_t(1)<<first))) { ++first; continue; }
                unsigned end=first+1;
                while(end<capacity && (mask&(std::uint32_t(1)<<end))) ++end;
                ++emittedCalls_; emittedBindings_+=end-first;
                route_(command_,first,end-first,buffers_.data()+first,offsets_.data()+first);
                first=end;
            }
            busy_=false;
        }
    public:
        explicit Scope(NativeVertexBindingBatch58& owner, bool supported=true) noexcept : owner_(owner),previous_(active_) {
            if(previous_) { previous_->drain(); previous_->qualified_=false; }
            qualified_=supported && !previous_ && owner.enabled_.load(std::memory_order_acquire);
            active_=this;
        }
        void finish() noexcept {
            if(finished_)return;
            drain();finished_=true;
            if(sourceCalls_) {
                owner_.recordGroup(sourceCalls_,emittedCalls_,sourceBindings_,emittedBindings_,fallbacks_);
            }
        }
        ~Scope() { finish(); if(active_==this)active_=previous_; }
        Scope(const Scope&)=delete;
    };
    // Consumers and lifecycle transitions flush before forwarding. Afterwards
    // the rest of this input invocation uses exact original calls.
    void boundary() noexcept {
        auto* s=active_;
        if(s) { s->drain(); s->qualified_=false; }
    }
    void flush() noexcept { if(auto* s=active_)s->drain(); }
    void bind(Function next,Command command,unsigned first,unsigned count,const Buffer* buffers,const Size* offsets) noexcept {
        auto* s=active_;
        if(!s || &s->owner_!=this || !s->qualified_ || s->busy_ || s->finished_ ||
           !enabled_.load(std::memory_order_acquire)) {
            if(s) { s->drain(); s->qualified_=false; }
            next(command,first,count,buffers,offsets);return;
        }
        ++s->sourceCalls_;s->sourceBindings_+=count;
        const auto fallback=[&] {
            s->drain();s->qualified_=false;++s->fallbacks_;++s->emittedCalls_;s->emittedBindings_+=count;
            next(command,first,count,buffers,offsets);
        };
        if(!command || !count || first>=capacity || count>capacity-first || !buffers || !offsets) { fallback();return; }
        for(unsigned i=0;i<count;++i)if(!buffers[i]) { fallback();return; }
        if(s->pending_ && (s->command_!=command || s->route_!=next)) {
            s->drain();
            if(!s->qualified_) { fallback();return; }
        }
        s->command_=command;s->route_=next;
        for(unsigned i=0;i<count;++i) {
            const auto slot=first+i;s->buffers_[slot]=buffers[i];s->offsets_[slot]=offsets[i];
            s->pending_|=std::uint32_t(1)<<slot;
        }
    }
};
}
