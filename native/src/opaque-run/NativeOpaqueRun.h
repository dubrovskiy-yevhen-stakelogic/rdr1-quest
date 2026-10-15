#pragma once
#include <cstdint>
#include <exception>
#include <limits>
#include <thread>

namespace rdrvr::experimental {
// Observer-only cache authority for consecutive SKIPPED ordinary
// calls in the actual inlined DrawContainer loops. No native/resource calls,
// memory reads, mutexes, allocations or skip decision are made here.
// Construct/configure on the render owner (a thread_local route object works).
// The issuer and Api context must outlive every Ticket. Configure is quiescent.
class NativeOpaqueRun {
public:
    using Address=std::uintptr_t;
    static constexpr Address Loop0Return=0x2a296c8,Loop1Return=0x2a29768;
    static constexpr Address PreBucketReturn=0x2a29478,PostBucketReturn=0x2a295bc;
    struct Frame {
        std::uint64_t scene{},pose{};unsigned stage{};Address owner{};
        bool operator==(const Frame&)const=default;
        bool valid()const noexcept{return scene&&pose&&stage<4&&owner;}
    };
    struct Api {
        void* context{};
        // Exact installed DrawContainer + datCallback + DrawCommon coverage,
        // selected native render owner, and current scene/stage. Read-only and
        // non-calling. This is NOT resource/geometry/material/image authority.
        bool(*current)(void*,const Frame&)noexcept{};
        // Zero means unavailable; every disable/rearm changes this monotonic
        // epoch. A readiness bool alone permits disarm/rearm cache ABA.
        std::uint64_t(*coverage)(void*)noexcept{};
    };
    class Invocation;class Callback;class Draw;
    class Ticket {
        friend class NativeOpaqueRun;
        NativeOpaqueRun* owner_{};Frame frame_{};Address container_{},loop_{};
        std::uint64_t invocation_{},bucket_{},serial_{};
    public:
        Ticket()noexcept=default;
        // Copies are opaque identities, not ownership. No field can be forged.
        explicit operator bool()const noexcept{return owner_&&serial_;}
        bool current()const noexcept;
        bool sameRun(const Ticket& other)const noexcept;
    };
    NativeOpaqueRun()noexcept=default;
    NativeOpaqueRun(const NativeOpaqueRun&)=delete;
    NativeOpaqueRun&operator=(const NativeOpaqueRun&)=delete;
    bool configure(Api api)noexcept {
        if(!owned()||invocation_||draw_||callback_)return false;
        advance();api_=api;return !exhausted_&&api.current&&api.coverage;
    }
    // BEFORE a stage change, unexpected native route, disarm or nested render.
    // No previous invocation may become valid again on the same addresses.
    void invalidate()noexcept;
    class Invocation {
        friend class NativeOpaqueRun;friend class Callback;friend class Draw;
        NativeOpaqueRun* owner_{};Invocation* previous_{};Frame frame_{};
        Address container_{},loop_{};std::uint64_t identity_{},bucket_{},coverage_{};
        bool fault_{},bucketReady_{};
    public:
        Invocation(NativeOpaqueRun& s,Frame frame,Address container)noexcept;
        ~Invocation();
        Invocation(const Invocation&)=delete;Invocation&operator=(const Invocation&)=delete;
        explicit operator bool()const noexcept;
    };
    class Callback {
        friend class NativeOpaqueRun;
        NativeOpaqueRun* owner_{};Invocation* invocation_{};Callback* previous_{};
        Address offset_{};int exceptions_{};bool eligible_{};
    public:
        // Construct BEFORE datCallback::Call, even when its callback is null.
        // Only a successfully returned exact pre-bucket call opens a bucket.
        Callback(NativeOpaqueRun& s,Address returnOffset)noexcept;
        ~Callback();
        Callback(const Callback&)=delete;Callback&operator=(const Callback&)=delete;
    };
    class Draw {
        friend class NativeOpaqueRun;
        NativeOpaqueRun* owner_{};Invocation* invocation_{};Draw* previous_{};
        bool eligible_{},completed_{},skipped_{},retained_{};std::uint64_t serial_{};int exceptions_{};
    public:
        Draw(NativeOpaqueRun& s,Address container,Address returnOffset)noexcept;
        ~Draw();
        Draw(const Draw&)=delete;Draw&operator=(const Draw&)=delete;
        Ticket ticket()const noexcept;
        // Call only after all ordinary skip proofs succeed. False means original
        // must execute. Keeps this run only while the issued identity is current.
        bool commitSkipped()noexcept;
        // Mandatory BEFORE calling the retained original, never after it.
        // Destructor fallback also invalidates but cannot repair a missed hook.
        void beforeOriginal()noexcept;
    };
private:
    friend struct NativeOpaqueRunTestAccess;
    const std::thread::id thread_=std::this_thread::get_id();
    Api api_{};std::uint64_t serial_{};bool exhausted_{};
    Invocation* invocation_{};Callback* callback_{};Draw* draw_{};
    bool owned()const noexcept{return thread_==std::this_thread::get_id();}
    std::uint64_t advance()noexcept {
        if(exhausted_||serial_>=std::numeric_limits<std::uint64_t>::max()-1){
            exhausted_=true;serial_=std::numeric_limits<std::uint64_t>::max();return 0;
        }
        return ++serial_;
    }
    bool live()const noexcept;
    bool current(const Ticket&)const noexcept;
};
inline bool NativeOpaqueRun::Ticket::current()const noexcept{return owner_&&owner_->current(*this);}
inline bool NativeOpaqueRun::Ticket::sameRun(const Ticket& other)const noexcept {
    return owner_==other.owner_&&serial_==other.serial_&&invocation_==other.invocation_&&
        bucket_==other.bucket_&&loop_==other.loop_&&frame_==other.frame_&&container_==other.container_&&
        current()&&other.current();
}
inline bool NativeOpaqueRun::live()const noexcept {
    return owned()&&!exhausted_&&invocation_&&!invocation_->fault_&&
        invocation_->identity_&&invocation_->coverage_&&api_.current&&api_.coverage&&
        api_.coverage(api_.context)==invocation_->coverage_&&api_.current(api_.context,invocation_->frame_)&&
        api_.coverage(api_.context)==invocation_->coverage_&&!invocation_->fault_;
}
inline void NativeOpaqueRun::invalidate()noexcept {
    if(!owned())return;advance();if(invocation_){invocation_->fault_=true;invocation_->bucketReady_=false;}
}
inline NativeOpaqueRun::Invocation::Invocation(NativeOpaqueRun& s,Frame frame,Address container)noexcept {
    if(!s.owned())return;
    owner_=&s;previous_=s.invocation_;frame_=frame;container_=container;
    if(previous_){previous_->fault_=true;previous_->bucketReady_=false;}
    identity_=s.advance();coverage_=s.api_.coverage?s.api_.coverage(s.api_.context):0;s.invocation_=this;
    fault_=previous_||s.draw_||s.callback_||!frame.valid()||!container||!identity_||!coverage_||
        !s.api_.current||!s.api_.current(s.api_.context,frame);
}
inline NativeOpaqueRun::Invocation::~Invocation(){
    if(!owner_)return;auto& s=*owner_;s.advance();fault_=true;bucketReady_=false;
    if(s.invocation_==this)s.invocation_=previous_;else if(s.invocation_)s.invocation_->fault_=true;
}
inline NativeOpaqueRun::Invocation::operator bool()const noexcept{return owner_&&owner_->invocation_==this&&owner_->live();}
inline NativeOpaqueRun::Callback::Callback(NativeOpaqueRun& s,Address offset)noexcept {
    if(!s.owned()||!s.invocation_)return;
    owner_=&s;invocation_=s.invocation_;previous_=s.callback_;offset_=offset;exceptions_=std::uncaught_exceptions();
    s.advance();invocation_->bucketReady_=false;invocation_->loop_=0;
    eligible_=s.live()&&!previous_&&!s.draw_&&(offset==PreBucketReturn||offset==PostBucketReturn);
    if(previous_||s.draw_)invocation_->fault_=true;
    s.callback_=this;
}
inline NativeOpaqueRun::Callback::~Callback(){
    if(!owner_)return;auto& s=*owner_;
    const bool same=s.callback_==this&&s.invocation_==invocation_;
    if(s.callback_==this)s.callback_=previous_;
    if(!same)return;
    if(std::uncaught_exceptions()!=exceptions_){s.invalidate();return;}
    if(eligible_&&offset_==PreBucketReturn&&s.live()){
        invocation_->bucket_=s.advance();invocation_->bucketReady_=invocation_->bucket_!=0;
    }
}
inline NativeOpaqueRun::Draw::Draw(NativeOpaqueRun& s,Address container,Address offset)noexcept {
    if(!s.owned()||!s.invocation_)return;
    owner_=&s;invocation_=s.invocation_;previous_=s.draw_;exceptions_=std::uncaught_exceptions();
    if(previous_||s.callback_){s.invalidate();}
    const bool exact=(offset==Loop0Return||offset==Loop1Return)&&container==invocation_->container_;
    if(!exact){s.advance();invocation_->loop_=0;}
    else if(invocation_->loop_!=offset){s.advance();invocation_->loop_=offset;}
    eligible_=exact&&!previous_&&!s.callback_&&s.live()&&invocation_->bucketReady_;
    s.draw_=this;serial_=s.serial_;
}
inline NativeOpaqueRun::Ticket NativeOpaqueRun::Draw::ticket()const noexcept {
    Ticket t;if(!owner_||!eligible_||completed_||retained_)return t;
    auto& s=*owner_;if(s.draw_!=this||!s.live()||s.callback_||s.invocation_!=invocation_||
        !invocation_->bucketReady_||s.serial_!=serial_)return t;
    t.owner_=&s;t.frame_=invocation_->frame_;t.container_=invocation_->container_;t.loop_=invocation_->loop_;
    t.invocation_=invocation_->identity_;t.bucket_=invocation_->bucket_;t.serial_=serial_;return t;
}
inline bool NativeOpaqueRun::current(const Ticket& t)const noexcept {
    if(!owned())return false;
    return t.owner_==this&&t.serial_&&t.serial_==serial_&&live()&&draw_&&draw_->eligible_&&
        !draw_->retained_&&!draw_->completed_&&!callback_&&invocation_->bucketReady_&&
        t.frame_==invocation_->frame_&&t.container_==invocation_->container_&&
        t.loop_==invocation_->loop_&&t.invocation_==invocation_->identity_&&t.bucket_==invocation_->bucket_&&
        t.serial_==serial_&&!invocation_->fault_;
}
inline bool NativeOpaqueRun::Draw::commitSkipped()noexcept {
    const auto t=ticket();if(!t.current())return false;
    completed_=skipped_=true;return true;
}
inline void NativeOpaqueRun::Draw::beforeOriginal()noexcept {
    if(!owner_||retained_)return;retained_=true;completed_=true;owner_->advance();
}
inline NativeOpaqueRun::Draw::~Draw(){
    if(!owner_)return;auto& s=*owner_;
    if(std::uncaught_exceptions()!=exceptions_)s.invalidate();
    else if(!completed_||!skipped_){if(!retained_)s.advance();}
    if(s.draw_==this)s.draw_=previous_;else if(s.invocation_)s.invocation_->fault_=true;
}
}
