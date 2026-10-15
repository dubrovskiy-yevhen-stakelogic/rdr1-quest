#pragma once
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace rdr95 {
// Actual resolved native state AFTER the DrawCommon prefix. No prediction of
// the previous object's PSO, material selection, camera, target or skip grant.
class OrdinaryFixedDepth final {
public:
 using Address=std::uintptr_t;static_assert(sizeof(Address)==8);
 struct Boundary {Address gfx{},vk{};std::uint64_t borrow{},scene{},resources{},effects{};bool operator==(const Boundary&)const=default;};
 struct Api {void* context{};bool(*read)(void*,Address,void*,std::size_t)noexcept{};bool(*boundary)(void*,Boundary&)noexcept{};
  Address gfxType{},vkType{},rasterType{},depthType{},pendingRaster{},pendingDepth{},readyFlags{},factoryDirty{};bool operator==(const Api&)const=default;};
 struct Description {Address raster{},depth{},command{};std::uint32_t rasterHash{},depthHash{};
  std::array<std::byte,28> rasterBytes{};std::array<std::byte,44> depthBytes{};bool operator==(const Description&)const=default;};
 class Snapshot {friend class OrdinaryFixedDepth;Api api_{};Boundary boundary_{};Description state_{};bool valid_{};
 public:bool describe(Description&d)const noexcept{if(!valid_)return false;d=state_;return true;}};
 enum class Failure {None,Owner,Read,Type,Coherence,Pending,State,Changed};
 struct Stats {unsigned reads{},bytes{};Failure failure{};};
 static bool capture(const Api&,Snapshot&,Stats&)noexcept;
 static bool matches(const Api&,const Snapshot&,Stats&)noexcept;
private:
 struct Errno {int*p=&errno;int saved=*p;~Errno(){*p=saved;}};
 static bool fail(Stats&s,Failure f)noexcept{s.failure=f;return false;}
 template<class T,std::size_t N>static T at(const std::array<std::byte,N>&b,unsigned n)noexcept{T v{};std::memcpy(&v,b.data()+n,sizeof v);return v;}
};
inline bool OrdinaryFixedDepth::capture(const Api&a,Snapshot&out,Stats&s)noexcept {
 Errno preserve;s={};const auto initial=a;Boundary b{};
 if(!a.read||!a.boundary||!a.gfxType||!a.vkType||!a.rasterType||!a.depthType||!a.pendingRaster||!a.pendingDepth||!a.readyFlags||!a.factoryDirty||
    !a.boundary(a.context,b)||!b.borrow||!b.scene||!b.resources||!b.effects||!b.gfx||!b.vk||b.gfx>UINTPTR_MAX-0x2b4||b.vk>UINTPTR_MAX-0x32e0)return fail(s,Failure::Owner);
 auto read=[&](Address p,auto&v)noexcept {if(!p||sizeof v>UINTPTR_MAX-p)return fail(s,Failure::Read);++s.reads;s.bytes+=sizeof v;return a.read(a.context,p,&v,sizeof v)||fail(s,Failure::Read);};
 auto type=[&](Address p,Address expected)noexcept{Address v{};return read(p,v)&&(v==expected||fail(s,Failure::Type));};
 Snapshot q;auto&d=q.state_;Address link{},gfxFramebuffer{},wrapper{};std::array<Address,2> framebuffer{},states{};
 std::array<std::byte,28> gfxStates{};std::array<std::byte,32> raster{};std::array<std::byte,48> depth{};
 std::array<std::uint8_t,4> ready{};std::uint8_t factory{};std::uint32_t queries{},dirty{},clears{};
 if(!type(b.gfx,a.gfxType)||!read(b.gfx+0x128,link)||link!=b.vk||!type(b.vk,a.vkType))return fail(s,s.failure==Failure::None?Failure::Type:s.failure);
 if(!read(b.gfx+0x158,gfxFramebuffer)||!read(b.gfx+0x298,gfxStates)||!read(b.vk+0x3088,queries)||!read(b.vk+0x30a0,wrapper)||
    !read(b.vk+0x30b0,framebuffer)||!read(b.vk+0x30d8,states)||!read(b.vk+0x32dc,dirty)||!read(b.vk+0x32a8,clears)||
    !read(a.pendingRaster,d.rasterBytes)||!read(a.pendingDepth,d.depthBytes)||!read(a.readyFlags,ready)||!read(a.factoryDirty,factory))return false;
 if(factory||clears||(dirty&0x401)||queries||!framebuffer[0]||framebuffer[0]!=framebuffer[1]||framebuffer[0]!=gfxFramebuffer)return fail(s,Failure::Coherence);
 for(auto v:ready)if(v!=1)return fail(s,Failure::Pending);
 if(!wrapper||wrapper>UINTPTR_MAX-0x40||!read(wrapper+0x38,d.command)||!d.command)return fail(s,s.failure==Failure::None?Failure::Coherence:s.failure);
 d.raster=at<Address>(gfxStates,0);d.depth=at<Address>(gfxStates,16);d.rasterHash=at<std::uint32_t>(gfxStates,8);d.depthHash=at<std::uint32_t>(gfxStates,24);
 if(d.raster!=states[1]||d.depth!=states[0])return fail(s,Failure::Coherence);
 if(!type(d.raster,a.rasterType)||d.raster>UINTPTR_MAX-0x30||!read(d.raster+0x10,raster)||
    !type(d.depth,a.depthType)||d.depth>UINTPTR_MAX-0x40||!read(d.depth+0x10,depth))return false;
 if(at<std::uint32_t>(raster,0)!=d.rasterHash||at<std::uint32_t>(depth,0)!=d.depthHash||
    std::memcmp(raster.data()+4,d.rasterBytes.data(),28)||std::memcmp(depth.data()+4,d.depthBytes.data(),44))return fail(s,Failure::Pending);
 // Existing plain-depth native descriptor subset; source actual PSO still
 // needs its separate PlainDepthCertificate and no pass-state overrides.
 if(at<std::uint32_t>(d.rasterBytes,0)!=2||std::uint8_t(d.rasterBytes[24])!=1||
    at<float>(d.rasterBytes,12)!=0||at<float>(d.rasterBytes,16)!=0||at<float>(d.rasterBytes,20)!=0)return fail(s,Failure::State);
 const auto compare=at<std::uint32_t>(d.depthBytes,0);
 if(std::uint8_t(d.depthBytes[0x28])!=1||std::uint8_t(d.depthBytes[0x27])>1||std::uint8_t(d.depthBytes[0x29])||compare<1||compare>3)return fail(s,Failure::State);
 Address finalLink{};Boundary end{};
 if(!type(b.gfx,a.gfxType)||!read(b.gfx+0x128,finalLink)||finalLink!=link||!type(b.vk,a.vkType)||!a.boundary(a.context,end)||b!=end||a!=initial)return fail(s,s.failure==Failure::None?Failure::Changed:s.failure);
 q.api_=a;q.boundary_=b;q.valid_=true;out=q;return true;
}
inline bool OrdinaryFixedDepth::matches(const Api&a,const Snapshot&q,Stats&s)noexcept {
 Errno preserve;s={};if(!q.valid_||a!=q.api_)return fail(s,Failure::Owner);Snapshot now;
 if(!capture(a,now,s))return false;
 const auto&b=now.boundary_;const auto&old=q.boundary_;
 // Actual framebuffer/viewport may differ for the destination eye; capture
 // rechecks its coherence. Eye target compatibility is a separate qualifier.
 return b.gfx==old.gfx&&b.vk==old.vk&&b.scene==old.scene&&b.resources==old.resources&&b.effects==old.effects&&now.state_==q.state_?true:fail(s,Failure::Changed);
}
}
