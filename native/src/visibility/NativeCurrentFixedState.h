#pragma once
#include "NativeSelectedLodGeometry.h"
#include "NativeCurrentContextSnapshot.h"
#include <cfenv>
#include <optional>
#include "NativeTechniqueSelection.h"
namespace rdr95 {
// PRIVATE per-hidden-call qualifier. It reads current native inputs and proves
// this narrow path does NOT change fixed state before its draws. It is NOT a
// visibility/material lifetime/query/target receipt and cannot authorize a skip
// alone. Root composes these obligations at the short drawAllowed boundary.
class NativeCurrentFixedState {
public:
 struct Api {
  rdr95::Api memory{};
  Address gfxVtable{},vkVtable{},rasterVtable{},depthVtable{};
  Address pendingRaster{},pendingDepth{},readyFlags{},defaults{},shadow{};
  Address globalBias{},markDynamic{},factoryDirty{};
  const NativeTechniqueSelection::Api* selection{};
 };
 struct Call {Address gfx{},vk{},container{},record{};unsigned command{},bucket{};float fade{};unsigned flags{},extra{};};
 enum class Failure {None,Owner,Material,Read,Identity,Pending,Transition,Framebuffer,QueryPending,Raster,Depth,Numeric};
 struct Result {bool fixedStateOnly{};Failure failure{Failure::None};Address command{},framebuffer{},raster{},depth{};unsigned reads{},bytes{};NativeCurrentContextSnapshot snapshot{};};
 static bool capture(const Api&,NativeCurrentRecordProvider&,const NativeSelectedLodGeometry::Api&,const NativeSelectedLodGeometry::Output&,const Call&,Result&)noexcept;
private:
 template<class T,std::size_t N>static T get(const std::array<std::byte,N>&b,unsigned o){T x{};std::memcpy(&x,b.data()+o,sizeof x);return x;}
 static bool numeric(float f){return std::isfinite(f)&&(f==0||std::isnormal(f));}
};
inline bool NativeCurrentFixedState::capture(const Api&a,NativeCurrentRecordProvider&owner,const NativeSelectedLodGeometry::Api&ga,const NativeSelectedLodGeometry::Output& graph,const Call& c,Result& out)noexcept {
 out={};auto fail=[&](Failure f){out.fixedStateOnly=false;out.failure=f;return false;};
 // Exact command branches: ordinary kind0 has no node/creature-specific callback
 // for command0/1; command3 also requires the type0/node exclusions below.
 if(!a.memory.read||!a.gfxVtable||!a.vkVtable||!a.rasterVtable||!a.depthVtable||!a.pendingRaster||!a.pendingDepth||!a.readyFlags||!a.defaults||!a.shadow||!a.globalBias||!a.markDynamic||!a.factoryDirty||!c.gfx||!c.vk||c.record!=graph.record||!c.container||(c.command!=0&&c.command!=1&&c.command!=3)||c.bucket>=32||!NativeSelectedLodGeometry::current(owner,ga,graph))return fail(Failure::Owner);
 // Sealed aggregate was derived from every current material during complete
 // graph capture. O(1) native owner + shader serial above revokes it; no per-part
 // shader or material traversal is repeated for a hidden draw.
 if(!owner.ready()||c.container!=owner.ready()->container)return fail(Failure::Identity);
 std::optional<NativeTechniqueSelection::SharedGlobals> sharedSelection;
 if(!graph.shaderCertified()){
  if(!a.selection||!ga.technique)return fail(Failure::Material);std::uint32_t nativeFlags{};
  if(!a.memory.read(c.record+0xac,&nativeFlags,4,a.memory.user))return fail(Failure::Read);
  if(graph.parts.size()>1){NativeTechniqueSelection::Failure why;sharedSelection.emplace();
   if(!NativeTechniqueSelection::beginShared(*a.selection,*sharedSelection,why))return fail(Failure::Material);
  }
  for(const auto&part:graph.parts){std::uint8_t geometryFlags{};if(!a.memory.read(part.geometry+0x14,&geometryFlags,1,a.memory.user))return fail(Failure::Read);
   NativeTechniqueSelection::Snapshot choice;NativeTechniqueSelection::Failure why;
   if(!NativeTechniqueSelection::captureImpl(*a.selection,part.material,nativeFlags,geometryFlags,choice,why,sharedSelection?&*sharedSelection:nullptr))return fail(Failure::Material);
   const auto* proof=ga.technique(ga.context,part.material,choice.technique());
   if(!proof||!proof->covers(choice.technique())||proof->material()!=part.material||proof->effect()!=choice.effect()||proof->positionLocation()!=0||!proof->noRenderStateOverrides()||!ColdMaterialVariants::current(*ga.materials,*proof))return fail(Failure::Material);
  }
 }else if(!graph.passesInherited())return fail(Failure::Material);
 auto read=[&](Address p,void*v,std::size_t n){if(!p||n>UINT64_MAX-p)return false;++out.reads;out.bytes+=unsigned(n);return a.memory.read(p,v,n,a.memory.user);};
 auto load=[&](Address p,auto&v){return read(p,&v,sizeof v);};
 Address vt{};if(!load(c.gfx,vt)||vt!=a.gfxVtable||!load(c.vk,vt)||vt!=a.vkVtable)return fail(Failure::Identity);
 std::array<std::byte,0xb0> record{};std::array<std::byte,0x198> gfx{};std::array<std::byte,0x68> vk{};std::array<std::byte,28> raster{};std::array<std::byte,44> depth{};std::array<std::byte,32> rasterState{};std::array<std::byte,48> depthState{};
 std::array<std::uint8_t,4> ready{};Address node{},buckets{},wrapper{};std::uint32_t nodeFlags{},defaultStencil{},shadowStencil{},dirty{},clears{},pendingQueries{};std::uint8_t mark{},factory{};float objectBias{},bucketBias{},globalBias{},containerBias{};
 // No whole VkContext copy: only stable header windows and current command owner.
 if(!load(c.record,record)||!read(c.gfx+0x128,gfx.data(),gfx.size())||!read(c.vk+0x3080,vk.data(),vk.size())||!load(a.pendingRaster,raster)||!load(a.pendingDepth,depth)||!load(a.readyFlags,ready)||!load(a.defaults+11*4,defaultStencil)||!load(a.shadow+20*4,shadowStencil)||!load(a.markDynamic,mark)||!load(a.factoryDirty,factory)||!load(a.globalBias,globalBias)||!load(c.container,buckets)||!load(c.container+0x20,containerBias)||!load(c.vk+0x32dc,dirty)||!load(c.vk+0x32a8,clears))return fail(Failure::Read);
 node=get<Address>(record,0x88);objectBias=get<float>(record,0xa8);const auto flags=get<std::uint32_t>(record,0xac);
 if(get<Address>(record,0x80)!=graph.drawable||get<Address>(record,0x98)||(flags&0xe000)||get<Address>(record,0x90)!=graph.transforms.matrixSet||!node||!load(node,vt)||vt!=a.memory.instanceVtable||!load(node+0x2c,nodeFlags)||(nodeFlags&0x100400))return fail(Failure::Identity);
 // The exact type0 vtable has no highlight/frag/creature path. Every bucket's
 // bias lookup is current, including the native bucket8 -> bias bucket5 alias.
 if(!load(buckets+(c.bucket==8?5:c.bucket)*0x70+0xc,bucketBias))return fail(Failure::Read);
 for(auto r:ready)if(r!=1)return fail(Failure::Pending);
 if(std::fegetround()!=FE_TONEAREST||!numeric(objectBias)||!numeric(bucketBias)||!numeric(globalBias)||!numeric(containerBias))return fail(Failure::Numeric);
 volatile float first=objectBias+bucketBias;volatile float combined=first+globalBias;
 if(!numeric(first)||!numeric(combined))return fail(Failure::Numeric);
 // Do not predict a future hash-cached bind. No bias setter, no stencil change,
 // and no pending/default/shadow mismatch are admitted at this first boundary.
 const auto stencil=(mark&&(nodeFlags&0x40222402u))?1u:unsigned(defaultStencil!=0);
 if(combined!=containerBias||stencil||defaultStencil||shadowStencil||std::uint8_t(depth[0x29]))return fail(Failure::Transition);
 if(factory||clears||(dirty&0x401)||get<Address>(gfx,0)!=c.vk)return fail(Failure::Framebuffer);
 out.framebuffer=get<Address>(vk,0x30); // native VkContext+30b0 active pass owner
 if(!out.framebuffer||out.framebuffer!=get<Address>(vk,0x38)||out.framebuffer!=get<Address>(gfx,0x30))return fail(Failure::Framebuffer);
 pendingQueries=get<std::uint32_t>(vk,8);if(pendingQueries)return fail(Failure::QueryPending);
 wrapper=get<Address>(vk,0x20);if(!wrapper||!load(wrapper+0x38,out.command)||!out.command)return fail(Failure::Identity);
 out.raster=get<Address>(gfx,0x170);out.depth=get<Address>(gfx,0x180);
 if(!out.raster||!out.depth||out.raster!=get<Address>(vk,0x60)||out.depth!=get<Address>(vk,0x58))return fail(Failure::Identity);
 // Type first, then exactly the existing hash+descriptor byte union. These
 // remain fresh per-call reads; no descriptor lifetime or run cache is implied.
 if(!load(out.raster,vt)||vt!=a.rasterVtable||!load(out.raster+0x10,rasterState)||get<std::uint32_t>(rasterState,0)!=get<std::uint32_t>(gfx,0x178)||!load(out.depth,vt)||vt!=a.depthVtable||!load(out.depth+0x10,depthState)||get<std::uint32_t>(depthState,0)!=get<std::uint32_t>(gfx,0x188))return fail(Failure::Identity);
 if(std::memcmp(rasterState.data()+4,raster.data(),raster.size())||std::memcmp(depthState.data()+4,depth.data(),depth.size()))return fail(Failure::Pending);
 if(get<std::uint32_t>(raster,0)!=2||std::uint8_t(raster[24])!=1||get<float>(raster,12)!=0||get<float>(raster,16)!=0||get<float>(raster,20)!=0)return fail(Failure::Raster);
 const auto compare=get<std::uint32_t>(depth,0);
 if(std::uint8_t(depth[0x28])!=1||std::uint8_t(depth[0x27])>1||std::uint8_t(depth[0x29])||compare<1||compare>3)return fail(Failure::Depth);
 // active queries, camera/raster extent/depth interval, command generation and
 // geometric hidden result are independently required by root drawAllowed.
 if(sharedSelection){NativeTechniqueSelection::Failure why;
  if(!a.selection||!NativeTechniqueSelection::finishShared(*a.selection,*sharedSelection,why))return fail(Failure::Material);
 }
 if(!NativeSelectedLodGeometry::current(owner,ga,graph))return fail(Failure::Owner);
 out.snapshot.capture(c,a.memory.user,a.gfxVtable,a.vkVtable,gfx,vk,wrapper,out.framebuffer,out.command);
 out.fixedStateOnly=true;return true;
}
} // namespace rdr95
