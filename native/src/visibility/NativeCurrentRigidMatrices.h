#pragma once
#include "NativeReadRecord.h"
namespace rdr95 {
// PRIVATE exact current-model transform decoder. No installed hook or skip authority.
// The caller MUST keep the post-join native render-owner mutation-stable lease live.
// Thread identity and the read journal do not establish cross-thread pose quiescence.
class NativeCurrentRigidMatrices {
 using Matrix=std::array<float,12>;
 static float mul(float a,float b){volatile float r=a*b;return r;}
 static float add(float a,float b){volatile float r=a+b;return r;}
 static Matrix compose(const Matrix& parent,const Matrix& palette){
  Matrix out{};for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r){
   auto x=mul(parent[r],palette[c*3]);auto y=mul(parent[3+r],palette[c*3+1]);auto z=mul(parent[6+r],palette[c*3+2]);out[c*3+r]=add(z,add(y,x));
  }
  for(unsigned r=0;r<3;++r){auto x=mul(parent[r],palette[9]);auto y=mul(parent[3+r],palette[10]);auto z=mul(parent[6+r],palette[11]);out[9+r]=add(z,add(y,add(parent[9+r],x)));}return out;
 }
 static bool sameRecord(const Record& a,const Record& b){return a.address==b.address&&a.node==b.node&&a.nativeNodeExclusion==b.nativeNodeExclusion&&a.nodeBoundsCurrent==b.nodeBoundsCurrent&&a.recordDrawableTargetCurrent==b.recordDrawableTargetCurrent&&a.minimum==b.minimum&&a.maximum==b.maximum&&a.drawable==b.drawable&&a.matrixSet==b.matrixSet&&a.creature==b.creature&&a.flags==b.flags&&a.world==b.world&&a.previousWorld==b.previousWorld&&a.family==b.family&&a.modelCount==b.modelCount;}
public:
 struct Model {Address native{};std::uint16_t ordinal{};std::uint8_t paletteIndex{};Matrix modelToWorld{};};
 // Storage capacity is independent of the freshly decoded native count.
 // The legacy Output still has exactly the 128-model layout; warm callers may
 // request smaller scratch, with a fresh larger native count declining safely.
 template<std::size_t N> struct BoundedOutput {
  static_assert(N>0&&N<=128);
  std::uint64_t frame{};Address record{},drawable{},matrixSet{};std::uint16_t count{};std::array<Model,N> models{};bool valid{};
 };
 using Output128=BoundedOutput<128>;
 using Output=Output128;
 template<std::size_t N> static bool capture(NativeCurrentRecordProvider& owner,unsigned index,BoundedOutput<N>& out,void* paletteContext=nullptr,
                     bool(*paletteCurrent)(void*,Address)noexcept=nullptr,const NativeReadRecord* handoff=nullptr){
  out.valid=false;out.count=0;if(!owner.leased())return false;
  auto& d=owner.decoder_;Record record{};
  if(!(handoff?handoff->copy(owner,index,paletteContext,record):owner.record(index,record))||!record.ordinaryFields||!record.drawMethodKnown||record.creature||(record.flags&0xe000)||record.modelFamily!=ModelFamily::Rigid||(record.family!=Family::StaticCommon&&record.family!=Family::RigidMatrixSet))return false;
  // Production supplies cached-bank provenance before the first palette read.
  // The optional legacy fixture API retains its explicit external pose lease.
  if(record.matrixSet&&paletteCurrent&&!paletteCurrent(paletteContext,record.matrixSet))return false;
  Address holder{},ptr{};std::uint16_t count{};
  if(record.lod>=4||!d.at(record.drawable,0x50+record.lod*8,holder)||!holder||!d.at(holder,0,ptr)||!d.at(holder,8,count)||!count||count>N||count!=record.modelCount||!ptr)return false;
  std::array<Address,N> models{};if(!d.read(ptr,models.data(),count*sizeof(Address)))return false;
  std::array<std::uint8_t,N> paletteIndices{};
  std::array<std::array<float,12>,N> capturedRows{};
  std::uint8_t paletteCount{};std::array<float,3> translation{};
  if(record.matrixSet){if(!d.at(record.matrixSet,0,paletteCount)||!paletteCount||!d.at(record.matrixSet,0x10,translation))return false;for(auto v:translation)if(!std::isfinite(v))return false;}
  for(unsigned i=0;i<count;++i){
   if(!models[i])return false;std::array<std::uint8_t,3> fields{};Address field{};
   if(!AdmissionDecoder::plus(models[i],0x2b,field)||!d.read(field,fields.data(),fields.size())||fields[2])return false;
   paletteIndices[i]=fields[0];Model m{};m.native=models[i];m.ordinal=static_cast<std::uint16_t>(i);m.paletteIndex=fields[0];
   if(!record.matrixSet)m.modelToWorld=record.world;
   else {if(m.paletteIndex>=paletteCount)return false;std::array<float,12> rows{};Address slot{};
    if(!AdmissionDecoder::plus(record.matrixSet,0xdc+std::size_t(m.paletteIndex)*48,slot)||!d.read(slot,rows.data(),48))return false;
    capturedRows[i]=rows;Matrix palette{};for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)palette[c*3+r]=rows[r*4+c];for(unsigned r=0;r<3;++r)palette[9+r]=add(rows[r*4+3],translation[r]);
    if(!AdmissionDecoder::finite(palette))return false;m.modelToWorld=(record.flags&0x10000)?compose(record.world,palette):palette;
   }
   if(!AdmissionDecoder::finite(m.modelToWorld))return false;out.models[i]=m;
  }
  // Recheck structural inputs before publishing. A native mutation-stable lease is
  // still mandatory: these reads are NOT a substitute for a pose version or lock.
  Address finalHolder{},finalPtr{};std::uint16_t finalCount{};Record finalRecord{};
  if(!owner.record(index,finalRecord)||!sameRecord(record,finalRecord)||!d.at(record.drawable,0x50+record.lod*8,finalHolder)||finalHolder!=holder||!d.at(holder,0,finalPtr)||finalPtr!=ptr||!d.at(holder,8,finalCount)||finalCount!=count||!owner.current())return false;
  std::array<Address,N> finalModels{};if(!d.read(ptr,finalModels.data(),count*sizeof(Address)))return false;
  for(unsigned i=0;i<count;++i){if(finalModels[i]!=models[i])return false;std::array<std::uint8_t,3> fields{};Address field{};if(!AdmissionDecoder::plus(models[i],0x2b,field)||!d.read(field,fields.data(),fields.size())||fields[2]||fields[0]!=paletteIndices[i])return false;
   if(record.matrixSet){std::array<float,12> rows{};Address slot{};if(!AdmissionDecoder::plus(record.matrixSet,0xdc+std::size_t(paletteIndices[i])*48,slot)||!d.read(slot,rows.data(),48)||rows!=capturedRows[i])return false;}}
  if(record.matrixSet){std::uint8_t n{};std::array<float,3> t{};if(!d.at(record.matrixSet,0,n)||n!=paletteCount||!d.at(record.matrixSet,0x10,t)||t!=translation)return false;}
  if(record.matrixSet&&paletteCurrent&&!paletteCurrent(paletteContext,record.matrixSet))return false;
  if(handoff&&!handoff->leaseCurrent(owner,index,paletteContext))return false;
  out.frame=owner.frame_;out.record=record.address;out.drawable=record.drawable;out.matrixSet=record.matrixSet;out.count=count;out.valid=true;return true;
 }
 static bool capture(NativeCurrentRecordProvider& owner,unsigned index,Output& out,void* paletteContext=nullptr,
                     bool(*paletteCurrent)(void*,Address)noexcept=nullptr,const NativeReadRecord* handoff=nullptr){
  return capture<128>(owner,index,out,paletteContext,paletteCurrent,handoff);
 }
};
} // namespace rdr95
