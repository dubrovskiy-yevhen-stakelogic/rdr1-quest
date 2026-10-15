#pragma once
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace rdr95 {
// Fresh comparison of ONE earlier selected ordinary geometry. This stores
// no native pointer lease and does not admit replay or immutable buffer data.
class OrdinaryGeometrySnapshot final {
public:
 using Address=std::uintptr_t;static_assert(sizeof(Address)==8);
 struct Boundary {std::uint64_t borrow{},scene{},resources{},effects{},meshes{};bool operator==(const Boundary&)const=default;};
 struct Api {void* context{};bool(*read)(void*,Address,void*,std::size_t)noexcept{};bool(*boundary)(void*,Boundary&)noexcept{};
  Address geometryType{},vertexType{},indexType{},bufferType{},shadow{};bool operator==(const Api&)const=default;};
 struct Description {
  Address geometry{},declaration{},vertex{},index{},fvf{},nativeVertex{},nativeIndex{},vertexAllocation{},indexAllocation{};
  std::uint64_t vertexBuffer{},indexBuffer{};std::uint32_t vertexBytes{},indexBytes{},indexCount{},stride{};std::uint16_t vertexCount{};
  std::array<std::byte,16> format{};bool optimized{};bool operator==(const Description&)const=default;
 };
 enum class Failure {None,Owner,Read,Type,Shape,Changed};
 struct Stats {unsigned reads{},bytes{},ranges{};Failure failure{};};
private:
 struct Range {Address address{};std::uint16_t offset{},size{};};
public:
 class Snapshot {
  friend class OrdinaryGeometrySnapshot;Api api_{};Boundary boundary_{};Description description_{};
  std::array<Range,16> ranges_{};std::array<std::byte,512> bytes_{};unsigned count_{},used_{};bool valid_{};
 public:bool describe(Description& d)const noexcept {if(!valid_)return false;d=description_;return true;}
 };
 static bool capture(const Api&,Address selectedGeometry,Snapshot&,Stats&)noexcept;
 static bool matches(const Api&,Address selectedGeometry,const Snapshot&,Stats&)noexcept;
private:
 struct Errno {int* p=&errno;int value=*p;~Errno(){*p=value;}};
 static bool fail(Stats& s,Failure f)noexcept{s.failure=f;return false;}
 static bool read(const Api&a,Address p,void*out,unsigned n,Stats&s)noexcept {
  if(!p||!n||n>UINTPTR_MAX-p)return fail(s,Failure::Read);++s.reads;s.bytes+=n;
  return a.read(a.context,p,out,n)||fail(s,Failure::Read);
 }
 static bool boundary(const Api&a,Boundary&b)noexcept{return a.boundary(a.context,b)&&b.borrow&&b.scene&&b.resources&&b.effects&&b.meshes;}
 static bool sameFrame(const Boundary&a,const Boundary&b)noexcept{return a.scene==b.scene&&a.resources==b.resources&&a.effects==b.effects&&a.meshes==b.meshes;}
 template<class T,std::size_t N>static T at(const std::array<std::byte,N>&b,unsigned n)noexcept{T x{};std::memcpy(&x,b.data()+n,sizeof x);return x;}
 static bool compare(const Api&a,const Snapshot&q,Stats&s)noexcept {
  std::array<std::byte,192> bytes{};
  for(unsigned i=0;i<q.count_;++i){const auto&r=q.ranges_[i];if(!read(a,r.address,bytes.data(),r.size,s))return false;
   if(std::memcmp(bytes.data(),q.bytes_.data()+r.offset,r.size))return fail(s,Failure::Changed);}
  s.ranges=q.count_;return true;
 }
};
inline bool OrdinaryGeometrySnapshot::capture(const Api&a,Address selected,Snapshot&out,Stats&s)noexcept {
 Errno preserve;s={};const auto initial=a;Snapshot q;Boundary begin{};
 if(!selected||!a.read||!a.boundary||!a.geometryType||!a.vertexType||!a.indexType||!a.bufferType||!a.shadow||!boundary(a,begin))return fail(s,Failure::Owner);
 auto save=[&](Address p,auto& value)noexcept{
  constexpr unsigned n=sizeof value;
  if(q.count_==q.ranges_.size()||n>q.bytes_.size()-q.used_)return fail(s,Failure::Shape);
  if(!read(a,p,&value,n,s))return false;
  q.ranges_[q.count_++]={p,static_cast<std::uint16_t>(q.used_),static_cast<std::uint16_t>(n)};
  std::memcpy(q.bytes_.data()+q.used_,&value,n);q.used_+=n;return true;
 };
 auto type=[&](Address p,Address expected)noexcept{Address v{};return save(p,v)&&(v==expected||fail(s,Failure::Type));};
 auto& d=q.description_;d.geometry=selected;
 std::array<std::byte,192> g{};std::array<std::byte,64> vb{};std::array<std::byte,40> ib{},nv{},ni{};std::uint8_t shadow{};
 if(!type(selected,a.geometryType)||!save(selected,g)||at<Address>(g,0)!=a.geometryType||!save(a.shadow,shadow))return fail(s,s.failure==Failure::None?Failure::Type:s.failure);
 d.optimized=at<std::uint8_t>(g,0x14)&&shadow;
 if(at<std::uint8_t>(g,0x63)||at<Address>(g,0x88)||at<Address>(g,0xa0)||at<std::uint8_t>(g,0x62)!=3)return fail(s,Failure::Shape);
 // The optimized native branch may additionally bind stream1 from +b8.
 // This first subset owns exactly one vertex-buffer identity.
 if(d.optimized&&at<Address>(g,0xb8))return fail(s,Failure::Shape);
 d.declaration=at<Address>(g,d.optimized?0xa8:8);d.vertex=at<Address>(g,d.optimized?0xb0:0x18);d.index=at<Address>(g,0x38);d.indexCount=at<std::uint32_t>(g,0x58);
 if(!d.declaration||!d.indexCount||d.indexCount%3)return fail(s,Failure::Shape);
 if(!type(d.vertex,a.vertexType)||!type(d.index,a.indexType)||!save(d.vertex,vb)||!save(d.index,ib))return false;
 if(at<Address>(vb,0)!=a.vertexType||at<Address>(ib,0)!=a.indexType)return fail(s,Failure::Type);
 d.fvf=at<Address>(vb,0x30);d.nativeVertex=at<Address>(vb,0x38);d.nativeIndex=at<Address>(ib,0x18);
 if(at<std::uint8_t>(vb,0xa)||at<std::uint32_t>(ib,0x20)||!d.fvf)return fail(s,Failure::Shape);
 if(!type(d.nativeVertex,a.bufferType)||!type(d.nativeIndex,a.bufferType)||!save(d.nativeVertex,nv)||!save(d.nativeIndex,ni)||!save(d.fvf,d.format))return false;
 if(at<Address>(nv,0)!=a.bufferType||at<Address>(ni,0)!=a.bufferType)return fail(s,Failure::Type);
 if(at<std::uint32_t>(nv,0x24)||at<std::uint32_t>(ni,0x24)!=1)return fail(s,Failure::Shape);
 d.stride=at<std::uint32_t>(vb,0x18);d.vertexCount=at<std::uint16_t>(vb,8);const auto capacity=at<std::uint32_t>(ib,8);
 d.vertexBytes=at<std::uint32_t>(nv,0x20);d.indexBytes=at<std::uint32_t>(ni,0x20);const auto format=at<std::uint64_t>(d.format,8)&15;
 if(!d.vertexCount||!d.stride||d.stride>255||d.stride%4||!(at<std::uint32_t>(d.format,0)&1)||at<std::uint8_t>(d.format,4)!=d.stride||
    (format!=6&&format!=7)||d.stride<(format==6?12u:16u)||d.indexCount>capacity||std::uint64_t(capacity)*2>d.indexBytes||std::uint64_t(d.vertexCount)*d.stride>d.vertexBytes)return fail(s,Failure::Shape);
 d.vertexAllocation=at<Address>(nv,0x18);d.indexAllocation=at<Address>(ni,0x18);d.vertexBuffer=at<std::uint64_t>(nv,0x10);d.indexBuffer=at<std::uint64_t>(ni,0x10);
 if(!d.vertexAllocation||!d.indexAllocation||!d.vertexBuffer||!d.indexBuffer)return fail(s,Failure::Shape);
 Boundary end{};
 if(!compare(a,q,s)||!boundary(a,end)||begin!=end||a!=initial)return fail(s,s.failure==Failure::None?Failure::Changed:s.failure);
 q.api_=a;q.boundary_=begin;q.valid_=true;out=q;return true;
}
inline bool OrdinaryGeometrySnapshot::matches(const Api&a,Address selected,const Snapshot&q,Stats&s)noexcept {
 Errno preserve;s={};const auto initial=a;Boundary begin{},end{};
 if(!q.valid_||a!=q.api_||selected!=q.description_.geometry||!boundary(a,begin)||!sameFrame(begin,q.boundary_))return fail(s,Failure::Owner);
 // Parent ranges are compared before dereferencing any saved child address.
 if(!compare(a,q,s))return false;
 return boundary(a,end)&&begin==end&&a==initial?true:fail(s,Failure::Changed);
}
}
