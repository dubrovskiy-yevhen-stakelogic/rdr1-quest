#pragma once
#include "FrozenCameraPipeline.h"
#include "RelativePositionProof.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace rdrvr::engine::detail {
// Cold, closed instruction alphabet. This is not a general SPIR-V side-effect
// analyzer. Unknown instructions, pointer operations and imports are rejected.
// Native input must be valid Vulkan SPIR-V. This bounded classifier is not
// a replacement for spirv-val; VkResult success alone does not prove validity.
// The successful factory's separate camera transform remains mandatory.
struct PlainDepthShader {
 struct Uniform {unsigned set{},binding{};};
 struct Result {std::vector<Uniform> uniforms;};
 static bool run(std::span<const std::uint32_t> w,VkShaderStageFlagBits stage,
                 unsigned setCount,std::string_view entry,Result& output)noexcept {
  try {
   if(!frozenCameraStageInterface(w,stage,setCount,entry)||w[3]>65536)return false;
   constexpr unsigned absent=UINT32_MAX;
   struct Node {unsigned pointer=UINT32_MAX,storage=UINT32_MAX,builtin=UINT32_MAX,set=UINT32_MAX,binding=UINT32_MAX;bool variable{};};
   std::vector<Node> nodes(w[3]);unsigned import{},functions{},labels{},returns{},ends{};bool inFunction=false;
   const auto id=[&](unsigned x){return x&&x<nodes.size();};
   const auto text=[&](std::span<const std::uint32_t> a,unsigned first,std::string_view expected){
    if(first>=a.size())return false;const auto* s=reinterpret_cast<const char*>(a.data()+first);const auto n=(a.size()-first)*4;
    const auto* end=static_cast<const char*>(std::memchr(s,0,n));return end&&std::string_view(s,std::size_t(end-s))==expected;
   };
   for(std::size_t at=5;at<w.size();){const auto n=w[at]>>16,op=w[at]&65535u;if(!n||n>w.size()-at)return false;
    const auto a=w.subspan(at+1,n-1);
    switch(op){
    // Metadata and exact, inert boilerplate seen in the saved native VS family.
    case 0:if(n!=1)return false;break;
    case 1:if(n!=3)return false;break; // Undef cannot form an accepted pointer.
    case 3:case 4:case 5:case 6:case 7:case 8:break;
    case 10:if(!text(a,0,"SPV_EXT_descriptor_indexing"))return false;break;
    case 11:if(a.empty()||!id(a[0])||import||!text(a,1,"GLSL.std.450"))return false;import=a[0];break;
    case 12:
     if(a.size()<5||!import||a[2]!=import)return false;
     // Exact pure math operations already present in saved VS5756. No pointer
     // output ExtInst (e.g. Modf/Frexp) or arbitrary instruction set is allowed.
     if(!((a[3]==43&&n==8)||(a[3]==46&&n==8)||(a[3]==62&&n==6)||(a[3]==79&&n==7)))return false;break;
    case 14:case 15:break; // Exact model/stage/entry already checked above.
    case 16:if(stage!=VK_SHADER_STAGE_FRAGMENT_BIT||n!=3||a[1]!=7)return false;break; // OriginUpperLeft only.
    case 17:if(n!=2||(a[0]!=1&&a[0]!=43&&a[0]!=5302))return false;break;
    case 19:case 20:if(n!=2)return false;break;
    case 21:if(n!=4||a[1]!=32)return false;break;
    case 22:if(n!=3||a[1]!=32)return false;break;
    case 23:case 24:case 28:if(n!=4)return false;break;
    case 30:case 33:if(n<2)return false;break;
    case 32:
     if(n!=4||!id(a[0])||(a[1]!=1&&a[1]!=2&&a[1]!=3&&a[1]!=6&&a[1]!=7))return false;
     if(nodes[a[0]].pointer!=absent)return false;nodes[a[0]].pointer=a[1];break;
    case 41:case 42:case 46:if(n!=3)return false;break;
    case 43:if(n!=4)return false;break;
    case 44:if(n<4)return false;break;
    case 54:if(n!=5||inFunction||++functions!=1)return false;inFunction=true;break;
    case 56:if(n!=1||!inFunction||++ends!=1)return false;inFunction=false;break;
    case 59:
     if((n!=4&&n!=5)||!id(a[0])||!id(a[1])||nodes[a[1]].variable||nodes[a[0]].pointer!=a[2])return false;
     // Fragment subset has no descriptors at all; straight-line color-only
     // arithmetic is harmless because the certified PSO has every color mask0.
     if(stage==VK_SHADER_STAGE_FRAGMENT_BIT&&a[2]==2)return false;
     nodes[a[1]].storage=a[2];nodes[a[1]].variable=true;break;
    case 61:if(n!=4||!id(a[2])||nodes[a[2]].storage==absent)return false;break;
    case 62:
     if(n!=3||!inFunction||!id(a[0]))return false;
     if(nodes[a[0]].storage!=3&&nodes[a[0]].storage!=6&&nodes[a[0]].storage!=7)return false;break;
    case 65:
     if(n<5||!id(a[0])||!id(a[1])||!id(a[2])||nodes[a[2]].storage==absent||nodes[a[0]].pointer!=nodes[a[2]].storage)return false;
     nodes[a[1]].storage=nodes[a[2]].storage;break;
    case 71:
     if(n<3||!id(a[0])||a[1]==3)return false; // BufferBlock is storage, never a UBO.
     if(a[1]==11||a[1]==33||a[1]==34){if(n!=4)return false;auto& node=nodes[a[0]];auto& value=a[1]==11?node.builtin:a[1]==33?node.binding:node.set;
      if(value!=absent)return false;value=a[2];}
     break;
    case 72:if(n<4||a[2]==11)return false;break; // No output-block BuiltIn aliases.
    case 79:if(n<6)return false;break;
    case 80:if(n<4)return false;break;
    case 81:if(n<5)return false;break;
    case 82:if(n<6)return false;break;
    case 83:
     if(n!=4||!id(a[0])||!id(a[1])||!id(a[2]))return false;
     if(nodes[a[0]].pointer!=absent){if(nodes[a[2]].storage!=nodes[a[0]].pointer)return false;nodes[a[1]].storage=nodes[a[2]].storage;}break;
    case 124:if(n!=4)return false;break; // Bitcast cannot enter accepted pointer map.
    case 129:case 131:case 133:case 142:case 145:case 148:case 194:case 199:if(n!=5)return false;break;
    case 248:if(n!=2||!inFunction||++labels!=1)return false;break;
    case 253:if(n!=1||!inFunction||++returns!=1)return false;break;
    default:return false; // Includes kill/demote, atomics/images/barriers/calls/loops/storage extensions.
    }
    at+=n;
   }
   if(inFunction||functions!=1||labels!=1||returns!=1||ends!=1)return false;
   Result result;
   for(const auto& node:nodes)if(node.variable){
    if(node.storage==3&&node.builtin!=absent&&(stage!=VK_SHADER_STAGE_VERTEX_BIT||node.builtin!=0))return false;
    if(node.storage==2){if(node.set==absent||node.binding==absent||node.set>=setCount||node.set==3)return false;result.uniforms.push_back({node.set,node.binding});}
   }
   if(stage==VK_SHADER_STAGE_VERTEX_BIT&&!RelativePositionProof(w).run())return false;
   output=std::move(result);return true;
  }catch(...){return false;}
 }
};
}
