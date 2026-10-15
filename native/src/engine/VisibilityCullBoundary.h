#pragma once
#include "CullViewportPool.h"
#include <cerrno>
#include <limits>
#include <array>
#include <atomic>

namespace rdrvr::engine {
// Narrow ABI boundaries, with argument substitution only. No global current
// viewport writes and no temporary toggles of asynchronous occlusion globals.
class VisibilityCullBoundary {
public:
    using Prepare=void(*)(void*,void*,const void*);
    using Worker=void(*)(void*,void*);
    using Getter=const void*(*)(const void*,int);
    using Signal=void(*)(void*);
    using Select=const void*(*)(void*,void*,const void*);
    using Borrow=CullViewportPool::Borrow(*)(const void*);
    using Complete=void(*)(void*) noexcept;
    using Read=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    // Static function: request, full world's begin/end, use room shafts, special list.
    using Cached=void(*)(void*,void*,void*,bool,bool);
    using Qualified=void(*)(const void*,const void*,bool) noexcept;
    using Policy=bool(*)() noexcept;
    // Optional diagnostic consumer (occluder quad test). Runs only in the
    // audited MAIN union scope, AFTER native CreateVisibleShaftList returned and
    // before CreateVisibilityListPre reaches AddToDrawList (retail +2abbbe4).
    using AfterCached=void(*)(void* request,const void* viewport,bool rooms,bool special) noexcept;
private:
    Prepare prepare_{};Worker worker_{};Getter getter_{};Signal signal_{};
    Select select_{};Borrow borrow_{};Complete complete_{};Read read_{};
    Cached cached_{};Qualified qualified_{};Policy policy_{};
    AfterCached afterCached_{};
    std::array<std::atomic<std::uint64_t>,2> nativeTraversalCalls_{};
    std::atomic<std::uint32_t> roomRecordsLast_{},roomRecordsMax_{};
    std::atomic<std::uint64_t> roomRecordReadFailures_{};
    struct Active {const VisibilityCullBoundary* owner{};const void* viewport{};void* request{};};
    inline static thread_local Active active_{nullptr,nullptr,nullptr};
public:
    std::uint64_t traversalCalls(bool rooms)const{return nativeTraversalCalls_[unsigned(rooms)].load(std::memory_order_relaxed);}
    std::uint32_t roomRecordsLast()const{return roomRecordsLast_.load(std::memory_order_relaxed);}
    std::uint32_t roomRecordsMax()const{return roomRecordsMax_.load(std::memory_order_relaxed);}
    std::uint64_t roomRecordReadFailures()const{return roomRecordReadFailures_.load(std::memory_order_relaxed);}
    void configure(Prepare prepare,Worker worker,Getter getter,Signal signal,Select select,Borrow borrow,Complete complete,Read read){
        prepare_=prepare;worker_=worker;getter_=getter;signal_=signal;select_=select;borrow_=borrow;complete_=complete;read_=read;
    }
    void configureCached(Cached cached,Qualified qualified,Policy policy){cached_=cached;qualified_=qualified;policy_=policy;}
    // Must be set before the cached GOT route is installed.
    void configureAfterCached(AfterCached after){afterCached_=after;}
    void prepare(void* renderer,void* viewport,const void* camera){
        const int incoming=errno;const void* selected{};
        try{if(select_)selected=select_(renderer,viewport,camera);}catch(...){}
        errno=incoming;prepare_(renderer,selected?const_cast<void*>(selected):viewport,selected?nullptr:camera);
    }
    void worker(void* manager,void* request){
        const int incoming=errno;const void* viewport{};std::uint32_t index{};
        const auto address=reinterpret_cast<std::uintptr_t>(request);
        auto getBorrow=[&]() -> CullViewportPool::Borrow {
            try{if(address&&address<=std::numeric_limits<std::uintptr_t>::max()-0xd0&&read_&&borrow_&&
                   read_(nullptr,address+0xcc,&index,4)&&index==13&&read_(nullptr,address+0x78,&viewport,sizeof(viewport)))return borrow_(viewport);
            }catch(...){}
            return {};
        };
        auto borrowed=getBorrow();
        struct Scope {Active previous;~Scope(){active_=previous;}} scope{active_};
        active_={this,borrowed.viewport(),request};errno=incoming;
        try{worker_(manager,request);}catch(...){const int result=errno;if(borrowed&&qualified_)qualified_(borrowed.viewport(),nullptr,false);errno=result;throw;}
    }
    const void* viewport(const void* manager,int channel,bool auditedCallsite){
        if(auditedCallsite&&channel==0&&active_.owner==this&&active_.viewport)return active_.viewport;
        return getter_(manager,channel);
    }
    void signal(void* thread,bool auditedCallsite){
        const int incoming=errno;if(auditedCallsite&&complete_)complete_(thread);errno=incoming;signal_(thread);
    }
    void cached(void* request,void* begin,void* end,bool rooms,bool special,bool auditedCallsite){
        const auto scope=active_;const int incoming=errno;
        if(!auditedCallsite||scope.owner!=this||!scope.viewport||scope.request!=request){cached_(request,begin,end,rooms,special);return;}
        const bool eligible=!special&&policy_&&policy_();
        errno=incoming;
        try{
            // Preserve the native room path. Its bool also controls building
            // request+18/+30 room entries and their dynamic-object caches;
            // forcing false skips those at retail +2a73234 -> +2a73334.
            // The private union viewport also needs matching cached tangents;
            // SetProjection alone does not update the native shaft side planes.
            cached_(request,begin,end,rooms,special);
        }catch(...){const int result=errno;if(qualified_)qualified_(scope.viewport,nullptr,false);errno=result;throw;}
        const int result=errno;
        if(eligible){
            nativeTraversalCalls_[unsigned(rooms)].fetch_add(1,std::memory_order_relaxed);
            // This argument means the camera starts in a room. Outdoor entry
            // through a doorway can also populate the request's room records.
            // Keep that distinct from the native boolean in diagnostics.
            std::uint32_t count{};bool ok=false;
            try{ok=read_&&read_(nullptr,reinterpret_cast<std::uintptr_t>(request)+0x30,&count,4)&&count<=32;}catch(...){}
            if(ok){roomRecordsLast_.store(count,std::memory_order_relaxed);
                auto old=roomRecordsMax_.load(std::memory_order_relaxed);
                while(old<count&&!roomRecordsMax_.compare_exchange_weak(old,count,std::memory_order_relaxed)){}
            }else roomRecordReadFailures_.fetch_add(1,std::memory_order_relaxed);
        }
        if(afterCached_)afterCached_(request,scope.viewport,rooms,special);
        if(qualified_)qualified_(scope.viewport,static_cast<std::byte*>(request)+0x150,eligible);
        errno=result;
    }
};
} // namespace rdrvr::engine
