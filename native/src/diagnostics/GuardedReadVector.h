#pragma once
// Cheaper guarded reads.
// - DirectReadRanges: address ranges that stay mapped and readable for the process
//   lifetime (the pinned engine's PT_LOAD segments, including its 1.2 GB static
//   heap in .bss, verified against /proc/self/maps once). A read entirely inside one
//   range is a plain copy; everything else stays kernel-checked (process_vm_readv).
// - ReadSegment/GuardedReadVector: many (source,target,size) copies in one call.
//   All-or-nothing: false when any segment is unreadable (targets may then be
//   partially written). Consumers holding the registered scalar reader batch through
//   its registered vector form; any other reader falls back to one call per segment.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace rdrvr::diagnostics {
struct ReadSegment {std::uintptr_t source{};void* target{};std::size_t size{};};
using ScalarReader=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using VectorReader=bool(*)(const ReadSegment*,std::size_t);

class GuardedReadVector {
    std::atomic<ScalarReader> scalar_{};std::atomic<VectorReader> vector_{};
public:
    void configure(ScalarReader scalar,VectorReader vector)noexcept{
        vector_.store(vector,std::memory_order_relaxed);scalar_.store(scalar,std::memory_order_release);
    }
    bool vectored(ScalarReader read)const noexcept{return read&&read==scalar_.load(std::memory_order_acquire)&&vector_.load(std::memory_order_relaxed);}
    bool read(ScalarReader read,void* user,const ReadSegment* s,std::size_t n)const{
        if(!n)return true;
        if(!read||!s)return false;
        if(read==scalar_.load(std::memory_order_acquire))
            if(const auto v=vector_.load(std::memory_order_relaxed))return v(s,n);
        for(std::size_t i=0;i<n;++i)if(!read(user,s[i].source,s[i].target,s[i].size))return false;
        return true;
    }
};
inline GuardedReadVector guardedReadVector;

class DirectReadRanges {
public:
    static constexpr unsigned capacity=16; // the pinned librdr has 8 readable PT_LOAD segments
    struct Range {std::uintptr_t begin{},end{};};
private:
    // Immutable after publication: fields are written before count_ (release).
    std::array<Range,capacity> ranges_{};std::uintptr_t low_{},high_{};std::atomic<unsigned> count_{};
public:
    // readable: [begin,end) of every readable mapping (any order). True when [b,e) is
    // covered without a gap.
    static bool covered(std::uintptr_t b,std::uintptr_t e,std::vector<Range> readable){
        if(b>=e)return false;
        std::sort(readable.begin(),readable.end(),[](const Range& x,const Range& y){return x.begin<y.begin;});
        std::uintptr_t at=b;
        for(const auto& r:readable){
            if(r.end<=at)continue;
            if(r.begin>at)return false;
            at=r.end;if(at>=e)return true;
        }
        return false;
    }
    // Publish once (a second call is refused). Ranges must already be validated.
    bool publish(const std::vector<Range>& ranges)noexcept{
        if(count_.load(std::memory_order_acquire)||ranges.empty()||ranges.size()>capacity)return false;
        std::uintptr_t low=UINTPTR_MAX,high=0;
        for(std::size_t i=0;i<ranges.size();++i){
            if(ranges[i].begin>=ranges[i].end)return false;
            ranges_[i]=ranges[i];low=std::min(low,ranges[i].begin);high=std::max(high,ranges[i].end);
        }
        low_=low;high_=high;count_.store(unsigned(ranges.size()),std::memory_order_release);return true;
    }
    bool contains(std::uintptr_t p,std::size_t size)const noexcept{
        const unsigned n=count_.load(std::memory_order_acquire);
        if(!n||p<low_||p>=high_)return false;
        for(unsigned i=0;i<n;++i){const auto& r=ranges_[i];if(p>=r.begin&&p<r.end&&size<=r.end-p)return true;}
        return false;
    }
    unsigned count()const noexcept{return count_.load(std::memory_order_acquire);}
    std::uint64_t bytes()const noexcept{std::uint64_t b{};const unsigned n=count();for(unsigned i=0;i<n;++i)b+=ranges_[i].end-ranges_[i].begin;return b;}
};
} // namespace rdrvr::diagnostics
