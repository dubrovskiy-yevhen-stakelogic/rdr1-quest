#pragma once
#include "NativeAdmissionDecoder.h"
#include "EffectMutationEpoch.h"
#include "engine/ColdOcclusionRegistry.h"
#include <memory>
#include <vector>
#include <span>
#include <bitset>
namespace rdr95 {
// PRIVATE cold graph certificate. This is POSITION/SHADER proof, never a
// whole-draw skip permit. Current depth/stencil/query/callback side effects,
// complete geometry and a mutation-stable material owner remain independent.
class ColdMaterialVariants {
public:
 using Registry=rdrvr::engine::ColdOcclusionRegistry;
 using Certificate=rdrvr::engine::StaticOcclusionCertificate;
 struct Owner {std::uint64_t identity{},generation{};bool operator==(const Owner&)const=default;};
 struct Api {
  rdr95::Api memory{};Registry* registry{};VkDevice device{};
  Address vertexShaderVtable{},fragmentShaderVtable{},programVtable{},expectedMatrixBuffer{};
  void* ownerContext{};
  bool(*ownerCurrent)(void*,Owner,Address material,Address effect)noexcept{};
  std::uint64_t(*shaderSerial)(const Registry&)noexcept{};
  bool(*shaderSetCurrent)(const Registry&,std::uint64_t)noexcept{};
  const EffectMutationEpoch* effectEpoch{};
 };
 enum class Failure {None,Unqualified,Read,Budget,Array,Owner,Material,Module,Position,Effects,Constants,Pass,Program,Changed};
 struct Stats {unsigned reads{},bytes{},vertexVariants{},fragmentVariants{},techniques{},passes{},stateOverridePasses{};Failure failure{Failure::None};};
 class Receipt {
  friend class ColdMaterialVariants;
  Owner owner_{};Address material_{},effect_{},matrixBuffer_{};std::uint64_t serial_{};
  const EffectMutationEpoch* effectEpoch_{};std::uint64_t effectTicket_{};
  const Registry* registry_{};VkDevice device_{};unsigned positionLocation_{};
  std::vector<Registry::ShaderSnapshot> shaders_{};
  bool valid_{},noStateOverrides_{},all_{};std::bitset<256> techniques_{};
 public:
  Receipt()=default;Receipt(const Receipt&)=delete;Receipt&operator=(const Receipt&)=delete;
  Receipt(Receipt&& other)noexcept{*this=std::move(other);}
  Receipt&operator=(Receipt&& other)noexcept{if(this!=&other){all_=other.all_;techniques_=other.techniques_;effectEpoch_=other.effectEpoch_;effectTicket_=other.effectTicket_;owner_=other.owner_;material_=other.material_;effect_=other.effect_;matrixBuffer_=other.matrixBuffer_;serial_=other.serial_;registry_=other.registry_;device_=other.device_;positionLocation_=other.positionLocation_;shaders_=std::move(other.shaders_);valid_=other.valid_;noStateOverrides_=other.noStateOverrides_;other.reset();}return *this;}
  explicit operator bool()const{return valid_;}
  bool noRenderStateOverrides()const{return valid_&&noStateOverrides_;}
  bool covers(unsigned technique)const noexcept{return valid_&&technique&&technique<=256&&techniques_[technique-1];}
  bool allTechniques()const noexcept{return valid_&&all_;}
  Address material()const{return valid_?material_:0;}
  Address effect()const{return valid_?effect_:0;}
  unsigned positionLocation()const{return positionLocation_;}
  std::uint64_t shaderSerial()const{return serial_;}
  std::uint64_t effectTicket()const{return effectTicket_;}
  Owner owner()const{return owner_;}
  void reset(){valid_=false;shaders_.clear();}
 };
 static bool current(const Api& api,const Receipt& receipt)noexcept {
  return receipt.valid_&&api.effectEpoch&&api.effectEpoch==receipt.effectEpoch_&&api.effectEpoch->current(receipt.effectTicket_)&&api.registry==receipt.registry_&&api.device==receipt.device_&&api.expectedMatrixBuffer==receipt.matrixBuffer_&&api.ownerCurrent&&api.shaderSetCurrent&&
   api.shaderSetCurrent(*api.registry,receipt.serial_)&&api.ownerCurrent(api.ownerContext,receipt.owner_,receipt.material_,receipt.effect_);
 }
 static bool build(const Api&a,Owner o,Address m,Receipt&r,Stats&s){return buildImpl(a,o,m,{},r,s);}
 static bool buildTechniques(const Api&a,Owner o,Address m,std::span<const unsigned> techniques,Receipt&r,Stats&s){if(techniques.empty()){r.reset();s={};s.failure=Failure::Unqualified;return false;}return buildImpl(a,o,m,techniques,r,s);}
private:
 static bool buildImpl(const Api&,Owner,Address,std::span<const unsigned>,Receipt&,Stats&);
 struct Read {Address p{};std::vector<std::byte> bytes;};
 struct Array {Address p{};unsigned count{},capacity{};};
 template<class T,std::size_t N>static T field(const std::array<std::byte,N>& b,unsigned o){T t{};std::memcpy(&t,b.data()+o,sizeof t);return t;}
 template<std::size_t N>static Array array(const std::array<std::byte,N>& b,unsigned o){return {field<Address>(b,o),field<std::uint16_t>(b,o+8),field<std::uint16_t>(b,o+10)};}
 static bool range(Address p,std::size_t n){return p&&n&&n<=UINT64_MAX-p;}
 static bool valid(Array a,unsigned stride,unsigned maximum){return a.count<=a.capacity&&a.capacity<=maximum&&((!a.capacity&&!a.p)||(a.capacity&&range(a.p,std::size_t(a.capacity)*stride)));}
};
inline bool ColdMaterialVariants::buildImpl(const Api& api,Owner owner,Address material,std::span<const unsigned> selected,Receipt& out,Stats& stats){
 out.reset();stats={};Receipt result;std::vector<Read> journal;
 auto fail=[&](Failure f){stats.failure=f;return false;};
 auto read=[&](Address p,void* bytes,std::size_t n){
  if(!range(p,n)||n>32768||stats.bytes+n>512*1024||journal.size()>=4096)return fail(Failure::Budget);
  ++stats.reads;stats.bytes+=unsigned(n);if(!api.memory.read(p,bytes,n,api.memory.user))return fail(Failure::Read);
  journal.push_back({p,std::vector<std::byte>(static_cast<std::byte*>(bytes),static_cast<std::byte*>(bytes)+n)});return true;
 };
 auto get=[&](Address p,auto& value){return read(p,&value,sizeof value);};
 if(!api.memory.read||!api.effectEpoch||!api.registry||!api.device||!api.vertexShaderVtable||!api.fragmentShaderVtable||!api.programVtable||!api.expectedMatrixBuffer||!api.ownerCurrent||!api.shaderSerial||!api.shaderSetCurrent||!owner.identity||!owner.generation||!range(material,0x40))return fail(Failure::Unqualified);
 result.effectEpoch_=api.effectEpoch;result.effectTicket_=api.effectEpoch->ticket();if(!result.effectTicket_)return fail(Failure::Owner);
 result.serial_=api.shaderSerial(*api.registry);if(!result.serial_)return fail(Failure::Module);
 Address effect{};if(!get(material+0x10,effect)||!range(effect,0x188))return fail(Failure::Material);
 if(!api.ownerCurrent(api.ownerContext,owner,material,effect))return fail(Failure::Owner);
 std::array<std::byte,0x188> e{};if(!get(effect,e))return false;
 const auto programs=array(e,0),techniques=array(e,0x10),vertices=array(e,0x30),fragments=array(e,0x40);
 if(!valid(programs,8,256)||!valid(techniques,32,256)||!valid(vertices,128,256)||!valid(fragments,128,256)||!vertices.count||!fragments.count||!techniques.count||!programs.count)return fail(Failure::Array);
 std::vector<Address> vs(vertices.count),fs(fragments.count),program(programs.count);
 std::bitset<256> usedVs,usedFs,usedPrograms;result.all_=selected.empty();
 if(selected.empty()){usedVs.set();usedFs.set();usedPrograms.set();for(unsigned i=0;i<techniques.count;++i)result.techniques_.set(i);}
 else {for(auto t:selected){if(!t||t>techniques.count)return fail(Failure::Pass);result.techniques_.set(t-1);}
  for(unsigned t=0;t<techniques.count;++t)if(result.techniques_[t]){std::array<std::byte,32> technique{};if(!get(techniques.p+t*32,technique))return false;const auto passes=array(technique,0x10);if(!valid(passes,6,256)||!passes.count)return fail(Failure::Pass);
   for(unsigned j=0;j<passes.count;++j){std::array<std::byte,6> pass{};if(!get(passes.p+j*6,pass))return false;const auto vi=field<std::uint8_t>(pass,0),fi=field<std::uint8_t>(pass,1),pi=field<std::uint8_t>(pass,2);if(vi>=vertices.count||fi>=fragments.count||pi>=programs.count)return fail(Failure::Pass);usedVs.set(vi);usedFs.set(fi);usedPrograms.set(pi);}
  }
 }

 bool first=true;
 for(unsigned stage=0;stage<2;++stage){const auto list=stage?fragments:vertices;auto& objects=stage?fs:vs;
  for(unsigned i=0;i<list.count;++i){if(!(stage?usedFs:usedVs)[i])continue;std::array<std::byte,128> record{};if(!get(list.p+i*128,record))return false;const auto object=field<Address>(record,0x78);Address vt{};std::uint64_t raw{};
   if(!range(object,0xe0)||!get(object,vt)||vt!=(stage?api.fragmentShaderVtable:api.vertexShaderVtable)||!get(object+0xd8,raw)||!raw)return fail(Failure::Module);
   Registry::ShaderSnapshot shader{};const auto module=reinterpret_cast<VkShaderModule>(static_cast<std::uintptr_t>(raw));
   if(!api.registry->queryShader(api.device,module,shader))return fail(Failure::Module);const auto& c=shader.certificate;
   if(c.stage!=(stage?2u:1u)||!c.sideEffectFree||(stage&&!c.fragmentSafe))return fail(Failure::Effects);
   if(!stage){if(c.position!=Certificate::PositionModel::RelativeAffineXYZ1||!c.vertexCoverage||c.matrixSet!=0||c.matrixBinding!=1)return fail(Failure::Position);
    if(first){result.positionLocation_=c.positionLocation;first=false;}else if(result.positionLocation_!=c.positionLocation)return fail(Failure::Position);}
   // Reject any material/custom CB override in every possible shader variant.
   for(unsigned k=0x58;k<=(stage?0x5bu:0x5cu);++k)if(field<std::int8_t>(record,k)>=0)return fail(Failure::Constants);
   const auto begin=field<Address>(record,0x40),end=field<Address>(record,0x48),capacity=field<Address>(record,0x50);
   if(!begin&&!end&&!capacity){if(!stage)return fail(Failure::Constants);}
   else {if(!begin||end<begin||capacity<end||(end-begin)%8||(capacity-begin)%8||capacity-begin>16*8)return fail(Failure::Constants);
    const auto slots=(end-begin)/8;if(!stage&&slots<2)return fail(Failure::Constants);if(slots>1){Address cb{};if(!get(begin+8,cb)||(!stage?cb!=api.expectedMatrixBuffer:(cb&&cb!=api.expectedMatrixBuffer)))return fail(Failure::Constants);}}
   objects[i]=object;result.shaders_.push_back(shader);if(stage)++stats.fragmentVariants;else ++stats.vertexVariants;
  }
 }
 for(unsigned i=0;i<programs.count;++i){if(!usedPrograms[i])continue;Address p{},vt{};if(!get(programs.p+i*8,p)||!range(p,0x28)||!get(p,vt)||vt!=api.programVtable)return fail(Failure::Program);program[i]=p;}
 for(unsigned t=0;t<techniques.count;++t){if(!result.techniques_[t])continue;std::array<std::byte,32> technique{};if(!get(techniques.p+t*32,technique))return false;const auto passes=array(technique,0x10);
  if(!valid(passes,6,256)||!passes.count||stats.passes+passes.count>4096)return fail(Failure::Pass);++stats.techniques;
  for(unsigned j=0;j<passes.count;++j){std::array<std::byte,6> pass{};if(!get(passes.p+j*6,pass))return false;const auto vi=field<std::uint8_t>(pass,0),fi=field<std::uint8_t>(pass,1),pi=field<std::uint8_t>(pass,2);
   if(vi>=vs.size()||fi>=fs.size()||pi>=program.size())return fail(Failure::Pass);Address v{},f{};if(!get(program[pi]+0x18,v)||!get(program[pi]+0x20,f)||v!=vs[vi]||f!=fs[fi])return fail(Failure::Program);++stats.passes;if((field<std::uint16_t>(pass,4)>>10)&15)++stats.stateOverridePasses;
  }
 }
 // Publication verifies all module generations AND the same native graph. This
 // cold journal cannot replace the caller's material mutation/lifetime lease.
 for(const auto& s:result.shaders_)if(!api.registry->currentShader(s))return fail(Failure::Changed);
 for(const auto& r:journal){std::vector<std::byte> now(r.bytes.size());++stats.reads;stats.bytes+=unsigned(now.size());if(!api.memory.read(r.p,now.data(),now.size(),api.memory.user)||now!=r.bytes)return fail(Failure::Changed);}
 Address finalEffect{};if(!api.effectEpoch->current(result.effectTicket_)||!get(material+0x10,finalEffect)||finalEffect!=effect||!api.shaderSetCurrent(*api.registry,result.serial_)||!api.ownerCurrent(api.ownerContext,owner,material,effect))return fail(Failure::Changed);
 result.owner_=owner;result.material_=material;result.effect_=effect;result.matrixBuffer_=api.expectedMatrixBuffer;result.registry_=api.registry;result.device_=api.device;result.noStateOverrides_=stats.stateOverridePasses==0;result.valid_=true;
 out=std::move(result);return true;
}
} // namespace rdr95
