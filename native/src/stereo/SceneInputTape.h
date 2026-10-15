#pragma once
#include "SceneInventorySummary.h"
#include <atomic>
#include <cerrno>
#include <limits>
namespace rdr95::scene_input {
// One explicitly armed diagnostic inventory. No persistent render instrumentation,
// raw pointer dereference, native call, GPU call, file I/O, or skip permission.
// read delegates ONLY to the existing scoped reader under its existing join lease.
struct Read {Address address{};std::size_t size{},offset{};bool ok{};};
struct Method {Address point{},observed{},expected{};bool answer{};};
class Tape {
public:
 static constexpr std::size_t MaxBytes=8*1024*1024,MaxReads=131072,MaxMethods=65536;
 Api original{};std::vector<Read> reads;std::vector<unsigned char> bytes;std::vector<Method> methods;
 Inventory inventory;Address manager{};std::uint64_t scene{},pose{};unsigned width{},height{};
 std::array<float,64> cameras{};bool overflow{},leaseCurrent{},exception{};
 static bool copy(Address a,void*out,std::size_t n,void*p){
  auto&s=*static_cast<Tape*>(p);
  if(s.reads.size()>=MaxReads||n>MaxBytes-s.bytes.size()||!a||n>UINT64_MAX-a){s.overflow=true;return false;}
  const auto offset=s.bytes.size();s.bytes.resize(offset+n);
  const bool ok=s.original.read&&s.original.read(a,out,n,s.original.user);
  if(ok)std::memcpy(s.bytes.data()+offset,out,n);else s.bytes.resize(offset);
  s.reads.push_back({a,n,offset,ok});return ok;
 }
 static bool method(void*p,Address point,Address observed,Address expected)noexcept{
  auto&s=*static_cast<Tape*>(p);if(s.methods.size()>=MaxMethods){s.overflow=true;return false;}
  const bool answer=s.original.ordinaryMethod&&s.original.ordinaryMethod(s.original.methodContext,point,observed,expected);
  try{s.methods.push_back({point,observed,expected,answer});}catch(...){s.overflow=true;return false;}return answer;
 }
 void decode(Api api,Address root)noexcept{
  original=api;manager=root;api.read=copy;api.user=this;api.ordinaryMethod=method;api.methodContext=this;
  const int saved=errno;
  try{AdmissionDecoder decoder(api);decoder.decodeAfterMainDepthWait(root,MainRequest,MainDepthWaitReturn,true,inventory);}
  catch(...){exception=true;inventory.complete=false;}errno=saved;
 }
 std::string json()const{
  static constexpr char hex[]="0123456789abcdef";
  std::ostringstream o;o<<"{\"schema\":\"rdrvr.main-input-tape.v1\",\"scene\":"<<scene<<",\"pose\":"<<pose<<",\"width\":"<<width<<",\"height\":"<<height
   <<",\"manager\":"<<manager<<",\"instanceVtable\":"<<original.instanceVtable<<",\"drawableDraw\":"<<original.drawableDraw<<",\"drawableDrawMulti\":"<<original.drawableDrawMulti
   <<",\"overflow\":"<<(overflow?"true":"false")<<",\"leaseCurrent\":"<<(leaseCurrent?"true":"false")<<",\"exception\":"<<(exception?"true":"false")
   <<",\"expected\":"<<inventorySummary(inventory)<<",\"cameraBits\":[";
  for(unsigned i=0;i<cameras.size();++i){if(i)o<<',';o<<std::bit_cast<std::uint32_t>(cameras[i]);}o<<"],\"reads\":[";
  for(unsigned i=0;i<reads.size();++i){if(i)o<<',';const auto&r=reads[i];o<<'['<<r.address<<','<<r.size<<','<<(r.ok?"true":"false")<<",\"";
   if(r.ok)for(std::size_t j=0;j<r.size;++j){auto b=bytes[r.offset+j];o<<hex[b>>4]<<hex[b&15];}o<<"\"]";}
  o<<"],\"methods\":[";for(unsigned i=0;i<methods.size();++i){if(i)o<<',';const auto&m=methods[i];o<<'['<<m.point<<','<<m.observed<<','<<m.expected<<','<<(m.answer?"true":"false")<<']';}o<<"]}";return o.str();
 }
};
// State release publishes immutable vectors to reporting thread; no reset/rearm.
inline Tape captured;
inline std::atomic<unsigned> state{}; // 0 idle, 1 armed, 2 capturing, 3 published
inline void arm()noexcept{unsigned idle=0;state.compare_exchange_strong(idle,1,std::memory_order_release);}
inline bool begin()noexcept{unsigned armed=1;return state.compare_exchange_strong(armed,2,std::memory_order_acq_rel);}
inline void publish()noexcept{state.store(3,std::memory_order_release);}
inline std::string report(){auto s=state.load(std::memory_order_acquire);if(s==3)return captured.json();return "{\"state\":"+std::to_string(s)+"}";}
}
