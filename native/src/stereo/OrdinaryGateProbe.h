#pragma once
#include <array>
#include <atomic>
#include <cstdint>
namespace rdr95::gateprobe {
enum Counter { BodyCalls, BodyCurrent, BodyPinned, BodyStatic, BodyCovered,
 Invoke, InvokeGeneration, InvokeCaller, InvokeSource, InvokeUnmasked, InvokeBody,
 InvokeDrawable, InvokeWorld, InvokePrevious, InvokeValid,
 Before, BeforeActive, BeforeLeft, BeforeCurrent, Replay, ReplayActive, ReplayRight,
 RouteActive, RouteObject, RouteTiles, RouteDepth, RouteColor, RouteEye0, RouteEye1, Count };
inline constexpr const char* names[]={"bodyCalls","bodyCurrent","bodyPinned","bodyStatic","bodyCovered",
 "invoke","invokeGeneration","invokeCaller","invokeSource","invokeUnmasked","invokeBody",
 "invokeDrawable","invokeWorld","invokePrevious","invokeValid",
 "before","beforeActive","beforeLeft","beforeCurrent","replay","replayActive","replayRight",
 "routeActive","routeObject","routeTiles","routeDepth","routeColor","routeEye0","routeEye1"};
static_assert(std::size(names)==Count);
inline thread_local bool enabled{};
inline thread_local unsigned frames{};
inline thread_local std::array<std::uint64_t,Count> local{};
inline std::array<std::atomic<std::uint64_t>,Count> published{};
inline std::atomic<unsigned> publishedFrames{};
struct Bucket {std::uint64_t key{},count{};};
inline thread_local std::array<std::array<Bucket,16>,2> buckets{};
inline std::array<std::array<std::atomic<std::uint64_t>,32>,2> publishedBuckets{};
inline thread_local std::array<std::uint64_t,2> overflow{};
inline std::array<std::atomic<std::uint64_t>,2> publishedOverflow{};
inline void sample(unsigned kind,std::uint64_t address,std::uint64_t base)noexcept {
 if(!enabled||kind>=2)return;
 const auto key=address>=base&&address-base<0x4000000?address-base:UINT64_MAX;
 for(auto& b:buckets[kind]){if(!b.count||b.key==key){b.key=key;++b.count;return;}}
 ++overflow[kind];
}
inline thread_local std::array<std::uint64_t,32> bodyClasses{};
inline std::array<std::atomic<std::uint64_t>,32> publishedBodyClasses{};
inline void begin()noexcept { enabled=frames<32; }
inline void hit(Counter c)noexcept {if(enabled)++local[c];}
inline void end()noexcept {
 if(!enabled)return;
 enabled=false;
 for(unsigned i=0;i<Count;++i)published[i].store(local[i],std::memory_order_relaxed);
 for(unsigned k=0;k<2;++k){
  for(unsigned i=0;i<16;++i){publishedBuckets[k][2*i].store(buckets[k][i].key,std::memory_order_relaxed);publishedBuckets[k][2*i+1].store(buckets[k][i].count,std::memory_order_relaxed);}
  publishedOverflow[k].store(overflow[k],std::memory_order_relaxed);
 }
 for(unsigned i=0;i<32;++i)publishedBodyClasses[i].store(bodyClasses[i],std::memory_order_relaxed);
 publishedFrames.store(++frames,std::memory_order_release);
}
}
