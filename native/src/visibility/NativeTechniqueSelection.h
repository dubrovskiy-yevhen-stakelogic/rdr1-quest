#pragma once
#include "NativeAdmissionDecoder.h"
#include "EffectMutationEpoch.h"
#include <cfenv>
namespace rdr95 {
// Exact ordinary grmShader::Draw -> BeginDraw(drawType=0, explicitTechnique=0).
// Requires the synchronous current native owner borrow. No engine calls/writes.
// Missing packed-alias entry stays native so its lazy lookup can run normally.
class NativeTechniqueSelection {
 friend class NativeCurrentFixedState;
public:
 struct Api {rdr95::Api memory{};const EffectMutationEpoch* epoch{};Address forcedGroup{},litGroup{},unlitGroup{},stateShadow{},fallbackGroup{},fallbackEnabled{},shadowBuffer{},minimumFade{};};
 enum class Failure {None,Unqualified,Read,Group,Technique,PendingAlias,Numeric,Changed};
 class Snapshot {friend class NativeTechniqueSelection;Address material_{},effect_{};std::uint64_t epoch_{};unsigned technique_{};bool valid_{};public:explicit operator bool()const{return valid_;}Address material()const{return material_;}Address effect()const{return effect_;}unsigned technique()const{return technique_;}};
 static bool capture(const Api&a,Address material,std::uint32_t flags,std::uint8_t geometryFlags,Snapshot&out,Failure&failure){
  return captureImpl(a,material,flags,geometryFlags,out,failure,nullptr);
 }
private:
 // Function-local common inputs for ONE FixedState check, never a cache
 // between DrawCommon calls. Only that owning adapter can create/use it.
 // Material tables/aliases remain fresh for every part; the owner must call
 // finishShared after all parts and before publishing its FixedState result.
 struct SharedGlobals {
  struct Field {Address address{};unsigned size{};std::array<std::byte,4> bytes{};};
  const Api* api{};std::uint64_t epoch{};std::array<Field,8> fields{};
  decltype(rdr95::Api::read) read{};void* memory{};const EffectMutationEpoch* epochOwner{};
  std::array<Address,8> addresses{};
  int group{},lit{},unlit{},fallback{};std::uint32_t lighting{};
  std::uint8_t useFallback{},shadow{};float minimum{};
  bool ready{};
 };
 static std::array<Address,8> sharedAddresses(const Api&a){
  return {a.forcedGroup,a.litGroup,a.unlitGroup,a.stateShadow,a.fallbackGroup,a.fallbackEnabled,a.shadowBuffer,a.minimumFade};
 }
 static bool sharedApi(const Api&a,const SharedGlobals&s){
  return s.ready&&s.api==&a&&s.read==a.memory.read&&s.memory==a.memory.user&&
   s.epochOwner==a.epoch&&s.addresses==sharedAddresses(a);
 }
 static bool beginShared(const Api&a,SharedGlobals& s,Failure&failure){
  s={};failure=Failure::None;
  if(!a.memory.read||!a.epoch||!a.forcedGroup||!a.litGroup||!a.unlitGroup||!a.stateShadow||
     a.stateShadow>UINT64_MAX-4||!a.fallbackGroup||!a.fallbackEnabled||!a.shadowBuffer||!a.minimumFade||
     std::fegetround()!=FE_TONEAREST){failure=Failure::Unqualified;return false;}
  s.api=&a;s.read=a.memory.read;s.memory=a.memory.user;s.epochOwner=a.epoch;s.addresses=sharedAddresses(a);
  s.epoch=a.epoch->ticket();if(!s.epoch){failure=Failure::Changed;return false;}
  unsigned index=0;
  auto read=[&](Address p,auto& v){
   if(!p||sizeof(v)>4||sizeof(v)>UINT64_MAX-p||index==s.fields.size())return false;
   auto& f=s.fields[index++];f.address=p;f.size=sizeof(v);
   if(!a.memory.read(p,&v,sizeof(v),a.memory.user))return false;
   std::memcpy(f.bytes.data(),&v,sizeof(v));return true;
  };
  if(!read(a.forcedGroup,s.group)||!read(a.litGroup,s.lit)||!read(a.unlitGroup,s.unlit)||
     !read(a.stateShadow+4,s.lighting)||!read(a.fallbackGroup,s.fallback)||
     !read(a.fallbackEnabled,s.useFallback)||!read(a.shadowBuffer,s.shadow)||!read(a.minimumFade,s.minimum)){
   failure=Failure::Read;return false;
  }
  if(!std::isfinite(s.minimum)){failure=Failure::Numeric;return false;}
  s.ready=true;
  if(!sharedApi(a,s)||!a.epoch->current(s.epoch)){s.ready=false;failure=Failure::Changed;return false;}
  return true;
 }
 static bool finishShared(const Api&a,const SharedGlobals&s,Failure&failure){
  if(!sharedApi(a,s)||!a.epoch||!a.epoch->current(s.epoch)||std::fegetround()!=FE_TONEAREST){failure=Failure::Changed;return false;}
  for(const auto& f:s.fields){std::array<std::byte,4> value{};
   if(!a.memory.read(f.address,value.data(),f.size,a.memory.user)||std::memcmp(value.data(),f.bytes.data(),f.size)){
    failure=Failure::Changed;return false;
   }
  }
  if(!sharedApi(a,s)||!a.epoch->current(s.epoch)){failure=Failure::Changed;return false;}
  return true;
 }
 static bool captureImpl(const Api&a,Address material,std::uint32_t flags,std::uint8_t geometryFlags,Snapshot&out,Failure&failure,const SharedGlobals*shared){
  out={};failure=Failure::None;auto fail=[&](Failure f){failure=f;return false;};
  if(!a.memory.read||!a.epoch||!a.forcedGroup||!a.litGroup||!a.unlitGroup||!a.stateShadow||!a.fallbackGroup||!a.fallbackEnabled||!a.shadowBuffer||!a.minimumFade||!material||(flags&0xe000)||std::fegetround()!=FE_TONEAREST)return fail(Failure::Unqualified);
  struct R {Address at{};unsigned size{};std::array<std::byte,16> bytes{};};std::array<R,64> journal{};unsigned count=0;
  auto read=[&](Address p,auto&v){if(!p||sizeof(v)>16||count==journal.size()||sizeof(v)>UINT64_MAX-p)return false;auto&r=journal[count++];r.at=p;r.size=sizeof(v);if(!a.memory.read(p,&v,sizeof(v),a.memory.user))return false;std::memcpy(r.bytes.data(),&v,sizeof(v));return true;};
  auto ticket=a.epoch->ticket();if(!ticket)return fail(Failure::Changed);
  if(shared&&(!sharedApi(a,*shared)||shared->epoch!=ticket))return fail(Failure::Changed);
  Address effect{},techniques{};std::uint16_t n{},cap{};int group{},fallback{},lit{},unlit{};std::uint32_t lighting{};std::uint8_t useFallback{},shadow{};float minimum{};
  // These three adjacent fields occupy EXACTLY 12 bytes in the effect: do
  // not widen the readable span or follow the table until count/cap validate.
  std::array<std::byte,12> table{};
  if(!read(material+0x10,effect)||!effect||!read(effect+0x10,table))return fail(Failure::Read);
  std::memcpy(&techniques,table.data(),8);std::memcpy(&n,table.data()+8,2);std::memcpy(&cap,table.data()+10,2);
  if(!n||n>cap||cap>256||!techniques)return fail(Failure::Read);
  if(shared){group=shared->group;lit=shared->lit;unlit=shared->unlit;lighting=shared->lighting;
   fallback=shared->fallback;useFallback=shared->useFallback;shadow=shared->shadow;minimum=shared->minimum;}
  else if(!read(a.forcedGroup,group)||!read(a.litGroup,lit)||!read(a.unlitGroup,unlit)||!read(a.stateShadow+4,lighting)||!read(a.fallbackGroup,fallback)||!read(a.fallbackEnabled,useFallback)||!read(a.shadowBuffer,shadow)||!read(a.minimumFade,minimum))return fail(Failure::Read);
  if(!std::isfinite(minimum))return fail(Failure::Numeric);
  if(group==-1)group=lighting?lit:unlit;
  unsigned chosen=0;auto choose=[&](int g){if(g<0||g>=64)return false;std::uint8_t t{};if(!read(effect+0x50+g*2,t)||t>n)return false;chosen=t;return true;};
  // Repeated tests of one selected technique share its copied pass count
  // inside this function. Its single journal entry is still freshly checked
  // after alias selection; an intervening count change cannot publish a proof.
  unsigned checkedTechnique=0;bool checkedPass=false;
  auto hasPass=[&](){if(!chosen)return false;if(checkedTechnique==chosen)return checkedPass;
   std::uint16_t passes{};if(!read(techniques+(chosen-1)*32+0x18,passes))return false;
   checkedTechnique=chosen;checkedPass=passes!=0;return checkedPass;};
  if(!choose(group))return fail(Failure::Group);
  if(!hasPass()){if(fallback!=-1&&!choose(fallback))return fail(Failure::Group);if(!hasPass()&&useFallback&&!choose(lit))return fail(Failure::Group);}
  if(!chosen)return fail(Failure::Technique);
  // DrawCommon overwrites the previous global fade BEFORE BeginDraw.
  float value=float((flags>>17)&63)/64.f;value=minimum>value?minimum:value;volatile float rounded=1.f-value;float fade=rounded;if(flags&0x800000)fade=-fade;
  if(fade==1.f){Address aliases{};if(!read(effect+0x180,aliases))return fail(Failure::Read);if(aliases){int t{};if(!read(aliases+(chosen-1)*4,t)||t<0||t>int(n))return fail(Failure::Technique);if(t)chosen=unsigned(t);}}
  if(shadow&&(geometryFlags&1)){Address aliases{};std::uint16_t aliasCount{};int t{};if(!read(effect+0x170,aliases)||!read(effect+0x178,aliasCount)||!aliases||chosen>aliasCount||!read(aliases+(chosen-1)*4,t))return fail(Failure::Read);if(t==0)return fail(Failure::PendingAlias);if(t!=-1){if(t<1||t>int(n))return fail(Failure::Technique);chosen=unsigned(t);}}
  if(!hasPass())return fail(Failure::Technique);
  for(unsigned i=0;i<count;++i){std::array<std::byte,16> b{};const auto&r=journal[i];if(!a.memory.read(r.at,b.data(),r.size,a.memory.user)||std::memcmp(b.data(),r.bytes.data(),r.size))return fail(Failure::Changed);}
  if(!a.epoch->current(ticket))return fail(Failure::Changed);out.material_=material;out.effect_=effect;out.epoch_=ticket;out.technique_=chosen;out.valid_=true;return true;
 }
};
}
