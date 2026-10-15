#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>

namespace rdr95 {
// A SHORT native metadata borrow. Never retain a Read across original native
// calls, callbacks, wait/join, rendering, or the stereo pair. Copied values may
// outlive it; their frame authority still requires current(savedGeneration).
// Covers only the installed resource/effect writers. Not a mutable node/pose
// lock, not a GPU-handle owner, and not a substitute for the scene/list proof.
class NativePhysicalCopyScope;
class NativeResourceGate {
    friend class NativePhysicalCopyScope;
public:
    enum Coverage : unsigned {QueueUpdate=1,EffectLoad=2,EffectShutdown=4,StreamingComplete=8,SchedulerShutdown=16,Required=31};
    class Read;
    class Writer;
    NativeResourceGate()=default;
    NativeResourceGate(const NativeResourceGate&)=delete;
    // Call only after complete code/slot proof. Rearming always revokes old
    // generations, even if the mask is unchanged. Does not reinstall hooks.
    bool setCoverage(unsigned mask)noexcept {
        coverage_.store(false,std::memory_order_release);
        advance();
        if(mask!=Required||fault_.load(std::memory_order_acquire))return false;
        coverage_.store(true,std::memory_order_release);return true;
    }
    void disarm()noexcept{setCoverage(0);}
    std::uint64_t ticket()const noexcept {
        if(!coverage_.load(std::memory_order_acquire)||fault_.load(std::memory_order_acquire)||unsigned(access_.load(std::memory_order_acquire)>>32))return 0;
        const auto value=generation_.load(std::memory_order_acquire);
        if(!value||!coverage_.load(std::memory_order_acquire)||unsigned(access_.load(std::memory_order_acquire)>>32))return 0;
        return value;
    }
    // Copied frame data remains valid during a QUIET writer. This is not a
    // memory borrow: only Read/borrowedOnThisThread authorizes native reads.
    bool frameRevisionCurrent(std::uint64_t value)const noexcept {
        return value&&coverage_.load(std::memory_order_acquire)&&!fault_.load(std::memory_order_acquire)
            &&generation_.load(std::memory_order_acquire)==value&&coverage_.load(std::memory_order_acquire);
    }
    bool current(std::uint64_t value)const noexcept{return frameRevisionCurrent(value);}
    bool faulted()const noexcept{return fault_.load(std::memory_order_acquire);}
    unsigned writers()const noexcept{return unsigned(access_.load(std::memory_order_acquire)>>32);}
    bool borrowedOnThisThread(std::uint64_t value)const noexcept;
    class Read {
        friend class NativeResourceGate;
        friend class NativePhysicalCopyScope;
        NativeResourceGate* owner_{};Read* previous_{};Read* root_{};
        std::thread::id thread_{};std::uint64_t generation_{};bool locked_{};
    public:
        explicit Read(NativeResourceGate& owner)noexcept:thread_(std::this_thread::get_id()) {
            advanceCopyStamp();
            const auto before=owner.ticket();if(!before)return;
            for(auto* r=readState_.top;r;r=r->previous_)if(r->owner_==&owner){
                if(!r->current())return;
                owner_=&owner;root_=r->root_;generation_=before;previous_=readState_.top;readState_.top=this;return;
            }
            auto state=owner.access_.load(std::memory_order_acquire);
            for(;;){
                if((state>>32)||std::uint32_t(state)==UINT32_MAX)return;
                if(owner.access_.compare_exchange_weak(state,state+1,std::memory_order_acquire,std::memory_order_relaxed))break;
            }
            locked_=true;owner_=&owner;root_=this;generation_=before;
            if(!owner.current(before)){owner.access_.fetch_sub(1,std::memory_order_release);locked_=false;owner_=nullptr;root_=nullptr;return;}
            previous_=readState_.top;readState_.top=this;
        }
        ~Read(){
            advanceCopyStamp();
            if(!owner_)return;
            // Scopes are lexical and thread-bound. Cross-thread destruction is
            // programmer misuse, not permission to unlock a foreign mutex.
            if(thread_!=std::this_thread::get_id()){owner_->fail();return;}
            if(readState_.top!=this){owner_->fail();return;}
            readState_.top=previous_;if(locked_)owner_->access_.fetch_sub(1,std::memory_order_release);
        }
        Read(const Read&)=delete;Read&operator=(const Read&)=delete;
        explicit operator bool()const noexcept{return current();}
        bool current()const noexcept{return owner_&&thread_==std::this_thread::get_id()&&root_&&root_->locked_&&owner_->current(generation_);}
        std::uint64_t generation()const noexcept{return current()?generation_:0;}
    };
    class Writer {
        NativeResourceGate* owner_{};Writer* previous_{};bool mutated_{};
        std::thread::id thread_{};
    public:
        enum class Mode { Mutation, Quiet };
        explicit Writer(NativeResourceGate& owner,Mode mode=Mode::Mutation)noexcept:owner_(&owner),thread_(std::this_thread::get_id()) {
            // Publish pending writer BEFORE lock wait. No new borrow/ticket can
            // be issued while the writer is waiting for existing short readers.
            owner.access_.fetch_add(WriterUnit,std::memory_order_acq_rel);if(mode==Mode::Mutation)markMutation();
            bool recursive=false;
            for(auto* w=writes_;w;w=w->previous_)if(w->owner_==&owner){recursive=true;break;}
            // Defensive misuse recovery: native original must still run once.
            // Drop this thread's read ownership and permanently disarm instead
            // of deadlocking. Every enclosing Read becomes invalid immediately.
            for(auto* r=readState_.top;r;r=r->previous_)if(r->owner_==&owner&&r->locked_){
                owner.fail();owner.access_.fetch_sub(1,std::memory_order_release);r->locked_=false;
            }
            // Do NOT serialize writers against each other. They retain native
            // lock order. Otherwise gate->nativeCrit versus nativeCrit->effect
            // writer would introduce a new ABBA deadlock. A Read never calls
            // native code/acquires native locks, so waiting ONLY for readers
            // adds no reverse native-lock dependency. Recursive native writers
            // already passed this same reader drain on the outer entry.
            if(!recursive)while(std::uint32_t(owner.access_.load(std::memory_order_acquire)))std::this_thread::yield();
            previous_=writes_;writes_=this;
        }
        ~Writer(){
            if(!owner_)return;
            if(thread_!=std::this_thread::get_id()||writes_!=this){owner_->fail();return;}
            writes_=previous_;
            owner_->access_.fetch_sub(WriterUnit,std::memory_order_release);
        }
        // Must execute BEFORE any native graph mutation/callback. Idempotent
        // for this scope; an inner effect writer may revoke independently.
        void markMutation()noexcept{if(owner_&&!mutated_){mutated_=true;owner_->advance();}}
        bool mutated()const noexcept{return mutated_;}
        Writer(const Writer&)=delete;Writer&operator=(const Writer&)=delete;
    };
private:
    static constexpr std::uint64_t WriterUnit=std::uint64_t{1}<<32;
    // A single atomic combines readers and pending writers, avoiding a
    // two-atomic store-buffer race on ARM. No lock allocation or exception.
    std::atomic<std::uint64_t> access_{};
    std::atomic<std::uint64_t> generation_{1};
    std::atomic<bool> coverage_{},fault_{};
    // Any Read nesting/end, including a failed attempt, permanently expires
    // a physical copy capability. This is thread-local activity, not a native
    // resource revision and never authorizes a semantic result.
    struct ThreadReadState {Read* top;NativePhysicalCopyScope* copy;std::uint64_t stamp;};
    inline static thread_local ThreadReadState readState_{};
    static void advanceCopyStamp()noexcept{if(readState_.stamp!=UINT64_MAX)++readState_.stamp;}
    inline static thread_local Writer* writes_{};
    void fail()noexcept{fault_.store(true,std::memory_order_release);coverage_.store(false,std::memory_order_release);advance();}
    void advance()noexcept {
        auto value=generation_.load(std::memory_order_relaxed);
        for(;;){
            if(!value)return;
            const auto next=value==std::numeric_limits<std::uint64_t>::max()?0:value+1;
            if(generation_.compare_exchange_weak(value,next,std::memory_order_acq_rel,std::memory_order_relaxed)){
                if(!next){fault_.store(true,std::memory_order_release);coverage_.store(false,std::memory_order_release);}return;
            }
        }
    }
};
inline bool NativeResourceGate::borrowedOnThisThread(std::uint64_t value)const noexcept {
    if(!current(value))return false;
    for(auto* r=readState_.top;r;r=r->previous_)if(r->owner_==this&&r->generation_==value&&r->current())return true;
    return false;
}
inline NativeResourceGate nativeResourceGate;
}
