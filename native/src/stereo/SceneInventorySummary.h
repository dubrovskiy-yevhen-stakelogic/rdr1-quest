#pragma once
#include "NativeAdmissionDecoder.h"
#include <bit>
#include <sstream>
namespace rdr95::scene_input {
// Canonical field digest: no padding, host pointers, or native calls. NOT a security hash.
inline std::uint64_t inventoryDigest(const Inventory& v){
 std::uint64_t h=14695981039346656037ull;
 auto u=[&](std::uint64_t x){for(unsigned i=0;i<8;++i){h^=static_cast<unsigned char>(x>>(8*i));h*=1099511628211ull;}};
 auto f=[&](float x){u(std::bit_cast<std::uint32_t>(x));};
 u(v.complete);u(unsigned(v.error));u(v.reads);u(v.bytes);
 const auto&r=v.ready;u(r.manager);u(r.request);u(r.container);u(r.records);u(r.buckets);u(r.instanceList);u(r.recordCount);u(r.bucketCount);u(r.nativeFrame);
 for(auto n:v.families)u(n);u(v.records.size());u(v.items.size());u(v.groups.size());
 for(const auto&r:v.records){u(r.address);u(r.drawable);u(r.node);u(r.matrixSet);u(r.creature);
  for(float x:r.world)f(x);for(float x:r.previousWorld)f(x);for(float x:r.minimum)f(x);for(float x:r.maximum)f(x);
  u(r.flags);u(r.ambientOcclusion);u(r.index);u(r.lod);u(r.modelCount);u(r.rigidModels);u(r.skinnedModels);u(r.bucketReferences);u(r.instanceReferences);
  u(unsigned(r.family));u(unsigned(r.modelFamily));u(r.ordinaryFields);u(r.worldFinite);u(r.nodeBoundsCurrent);u(r.drawMethodKnown);u(r.nativeNodeExclusion);u(r.ordinaryStateOnlyCandidate);}
 for(const auto&i:v.items){u(i.record);u(i.bucket);u(i.flags);f(i.fade);f(i.secondaryFade);}
 for(const auto&g:v.groups){u(g.group);u(g.lod);u(g.drawable);u(g.records.size());for(auto n:g.records)u(n);}
 return h;
}
inline std::string inventorySummary(const Inventory& v){
 std::ostringstream o;o<<"{\"complete\":"<<(v.complete?"true":"false")<<",\"error\":"<<unsigned(v.error)
 <<",\"reads\":"<<v.reads<<",\"bytes\":"<<v.bytes<<",\"records\":"<<v.records.size()<<",\"items\":"<<v.items.size()<<",\"groups\":"<<v.groups.size()
 <<",\"digest\":\""<<inventoryDigest(v)<<"\",\"families\":[";
 for(unsigned i=0;i<v.families.size();++i){if(i)o<<',';o<<v.families[i];}o<<"],\"recordDetails\":[";
 for(unsigned i=0;i<v.records.size();++i){if(i)o<<',';const auto&r=v.records[i];
  o<<'['<<r.index<<','<<unsigned(r.family)<<','<<unsigned(r.modelFamily)<<','<<r.flags<<','<<r.lod<<','<<r.rigidModels<<','<<r.skinnedModels<<','<<r.bucketReferences<<','<<r.instanceReferences<<','<<r.nodeBoundsCurrent<<','<<r.drawMethodKnown<<','<<r.ordinaryStateOnlyCandidate<<']';}
 o<<"]}";return o.str();
}
}
