#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <thread>
#include <vector>
#ifdef __ANDROID__
#include <time.h>
#endif
namespace rdr95::scene_routes {
inline std::uint64_t cpuNow()noexcept{
 const int saved=errno;std::uint64_t n{};
#ifdef __ANDROID__
 timespec t{};if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)==0)n=std::uint64_t(t.tv_sec)*1000000000ull+t.tv_nsec;
#endif
 errno=saved;return n;
}
struct Event {
 // kind1 Body,2 observer,3 indexed,4 nonindexed,5 stage,6 scope-exit,7 native snapshot result.
 unsigned kind{},phase{},parent{};bool pinned{},selected{};
 std::uint64_t start{},end{};std::array<std::uint64_t,8> args{};
 std::array<unsigned char,176> record{};
};
inline std::atomic<unsigned> state{}; // 0 unavailable,1 active,2 finished
inline std::atomic<std::uint64_t> foreignCalls{};
inline thread_local bool active{},materialActive{};
inline thread_local unsigned phase=4,parent{};
inline std::vector<Event> events;
inline std::uint64_t scene{},pose{},image{};
inline bool overflow{},completed{},balanced{};
inline constexpr unsigned Limit=65536;
inline bool owned()noexcept {
 // active is only ever true on the recording thread while state==1 (begin() sets
 // active before state=1, finish() clears it before state=2): no TLS access otherwise.
 if(state.load(std::memory_order_relaxed)!=1)return false;
 if(active)return true;
 foreignCalls.fetch_add(1,std::memory_order_relaxed);
 return false;
}
inline unsigned add(Event e)noexcept{
 if(!active)return 0;
 if(events.size()>=Limit){overflow=true;return 0;}
 try{e.phase=phase;e.parent=parent;e.start=cpuNow();events.push_back(e);return unsigned(events.size());}
 catch(...){overflow=true;return 0;}
}
inline void begin(std::uint64_t s,std::uint64_t p,std::uint64_t base)noexcept{
 if(state.load()!=0||active)return;
 scene=s;pose=p;image=base;phase=0;parent=0;active=true;
 state.store(1,std::memory_order_release);Event e;e.kind=5;add(e);
}
inline void stage(unsigned value)noexcept {if(!active)return;phase=value;Event e;e.kind=5;add(e);}
inline void finish(bool good)noexcept {
 if(!active)return;
 Event e;e.kind=6;add(e);balanced=parent==0&&!materialActive;completed=good;active=false;
 state.store(2,std::memory_order_release);
}
class Body {
 unsigned index_{},previous_{};
public:
 Body(std::uint64_t container,const void* record,std::uint64_t caller,int commands,int instance,float fade,unsigned flags,unsigned extra,bool pinned,bool selected)noexcept{
  if(!owned())return;Event e;e.kind=1;e.pinned=pinned;e.selected=selected;
  std::uint32_t fadeBits{};std::memcpy(&fadeBits,&fade,4);
  e.args={container,reinterpret_cast<std::uintptr_t>(record),caller,std::uint32_t(commands),std::uint32_t(instance),fadeBits,flags,extra};
  // The same synchronous const renDrawBucketInfo argument the native call reads.
  // No following drawable/model/resource pointers are dereferenced here.
  if(pinned&&record)std::memcpy(e.record.data(),record,e.record.size());
  previous_=parent;index_=add(e);if(index_)parent=index_;
 }
 ~Body(){if(index_&&active){events[index_-1].end=cpuNow();if(parent!=index_)overflow=true;parent=previous_;}}
};
inline void observer(std::uint64_t drawable,std::uint64_t world,std::uint64_t previous,std::uint64_t caller,bool valid)noexcept{
 if(!owned())return;Event e;e.kind=2;e.selected=valid;e.args={drawable,world,previous,caller};add(e);
}
inline void draw(bool indexed,std::uint64_t command,unsigned count,unsigned instances,unsigned first,int vertex,unsigned base,bool selected)noexcept{
 if(!owned())return;Event e;e.kind=indexed?3:4;e.selected=selected;e.args={command,count,instances,first,std::uint32_t(vertex),base};add(e);
}
inline void snapshot(unsigned failure,unsigned detail,std::uint64_t reads,std::uint64_t bytes)noexcept{
 if(!active)return;Event e;e.kind=7;e.args={failure,detail,reads,bytes};add(e);
}
// Exact material adapter inputs; active only during the existing one-frame tape.
inline void materialBegin(const std::array<std::uint64_t,15>& api,std::uint64_t drawable,int lod,int technique,std::uint64_t bits)noexcept{
 if(!active)return;if(materialActive){overflow=true;return;}materialActive=true;Event e;e.kind=8;e.args={drawable,std::uint32_t(lod),std::uint32_t(technique),bits};
 std::memcpy(e.record.data(),api.data(),sizeof api);add(e);
}
inline void materialRead(std::uint64_t address,const void* bytes,unsigned size,bool ok)noexcept{
 if(!active||!materialActive)return;if(size>16){overflow=true;return;}Event e;e.kind=9;e.args={address,size,ok?1ull:0ull};
 if(ok)std::memcpy(e.record.data(),bytes,size);add(e);
}
inline void materialBoundary(std::uint64_t borrow,std::uint64_t s,std::uint64_t resources,std::uint64_t effects,bool ok)noexcept{
 if(!active||!materialActive)return;Event e;e.kind=10;e.selected=ok;e.args={borrow,s,resources,effects};add(e);
}
inline void materialEnd(bool ok,unsigned failure,unsigned reads,unsigned bytes,unsigned ranges)noexcept{
 if(!active||!materialActive)return;Event e;e.kind=11;e.args={ok?1ull:0ull,failure,reads,bytes,ranges};add(e);materialActive=false;
}
inline std::string report(){
 const auto current=state.load(std::memory_order_acquire);if(current!=2)return "{\"state\":"+std::to_string(current)+"}";
 static constexpr char hex[]="0123456789abcdef";std::ostringstream o;
 o<<"{\"schema\":\"rdrvr.scene-routes.v1\",\"scene\":"<<scene<<",\"pose\":"<<pose<<",\"image\":"<<image
 <<",\"complete\":"<<(completed?"true":"false")<<",\"balanced\":"<<(balanced?"true":"false")<<",\"overflow\":"<<(overflow?"true":"false")
 <<",\"foreignCalls\":"<<foreignCalls.load()<<",\"clock\":\"CLOCK_THREAD_CPUTIME_ID\",\"events\":[";
 for(unsigned i=0;i<events.size();++i){if(i)o<<',';const auto&e=events[i];o<<'['<<e.kind<<','<<e.phase<<','<<e.parent<<','<<(e.pinned?1:0)<<','<<(e.selected?1:0)<<','<<e.start<<','<<e.end;
  for(auto n:e.args)o<<",\""<<n<<'\"';o<<",\"";unsigned bytes=e.kind==1&&e.pinned?176:e.kind==8?120:e.kind==9&&e.args[2]?unsigned(e.args[1]):0;for(unsigned j=0;j<bytes;++j){const auto b=e.record[j];o<<hex[b>>4]<<hex[b&15];}o<<"\"]";}
 o<<"]}";return o.str();
}
}
