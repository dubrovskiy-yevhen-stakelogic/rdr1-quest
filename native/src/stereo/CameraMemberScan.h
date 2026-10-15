#pragma once
// SPIR-V scan: which set-0 uniform blocks of a shader hold camera-dependent members.
// Entries (binding, member byte offset, kind) drive the right-eye uniform rewrite in StereoReplay.
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>
#include <functional>
#include <algorithm>
namespace rdrvr::engine::stereo {
enum class CameraMember : std::uint8_t { None=0, RageMatrices=1, WvpInverse=2, CameraMatrix=3, PrevViewProj=4, DepthMapSampler=5, ShadowCollectorSampler=6, LightBufferSampler=7 };
struct CameraMemberEntry { std::uint32_t binding{},offset{},blockSize{}; CameraMember kind{}; };
struct ShaderUniformScan { bool valid{}; bool vertex{}; std::uint8_t count{}; CameraMemberEntry entries[12]{}; };
inline void scanCameraMembers(const std::uint32_t* source,std::size_t words,ShaderUniformScan& out) noexcept {
    out=ShaderUniformScan{};
    if(!source||words<5||source[0]!=0x07230203u)return;
    struct Inst {std::size_t at;std::uint32_t op,count;};
    std::vector<Inst> insts;std::size_t i=5;
    while(i<words){const std::uint32_t count=source[i]>>16,op=source[i]&0xffffu;if(!count||i+count>words)return;insts.push_back({i,op,count});i+=count;}
    auto str=[&](std::size_t at,std::uint32_t n)->std::string_view{const char* s=reinterpret_cast<const char*>(source+at);std::size_t len=0;const std::size_t max=std::size_t(n)*4;while(len<max&&s[len])++len;return {s,len};};
    struct MemberName {std::uint32_t type,member;std::string_view name;};
    struct MemberOffset {std::uint32_t type,member,offset;};
    std::vector<MemberName> names;std::vector<MemberOffset> offsets;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> ds,bd,ptr; // (var,set) (var,binding) (ptrType,pointee)
    struct TypeInfo {std::uint32_t op{},a0{},a1{},a2{};std::uint32_t arrayStride{};std::vector<std::uint32_t> members;};
    std::vector<std::pair<std::uint32_t,TypeInfo>> types;std::vector<std::pair<std::uint32_t,std::uint32_t>> constants; // (id,value)
    std::vector<std::pair<std::uint32_t,std::uint32_t>> arrayStrides; // (type,stride)
    struct Var {std::uint32_t id,ptrType,storage;};std::vector<Var> vars;
    for(const auto& in:insts){const auto* a=source+in.at+1;const std::uint32_t n=in.count-1;
        switch(in.op){
        case 15:if(n>=1&&a[0]==0)out.vertex=true;break;                       // OpEntryPoint Vertex
        case 6:if(n>=3)names.push_back({a[0],a[1],str(in.at+3,n-2)});break;  // OpMemberName
        case 72:if(n>=4&&a[2]==35)offsets.push_back({a[0],a[1],a[3]});break; // OpMemberDecorate Offset
        case 71:if(n>=3&&a[1]==34)ds.push_back({a[0],a[2]});if(n>=3&&a[1]==33)bd.push_back({a[0],a[2]});if(n>=3&&a[1]==6)arrayStrides.push_back({a[0],a[2]});break;
        case 32:if(n>=3)ptr.push_back({a[0],a[2]});break;                     // OpTypePointer id storage pointee
        case 59:if(n>=3)vars.push_back({a[1],a[0],a[2]});break;              // OpVariable type id storage
        case 21:case 22:if(n>=2)types.push_back({a[0],{in.op,a[1],0,0}});break;                       // OpTypeInt/Float id width
        case 23:case 24:if(n>=3)types.push_back({a[0],{in.op,a[1],a[2],0}});break;                    // OpTypeVector/Matrix id comp count
        case 28:if(n>=3)types.push_back({a[0],{in.op,a[1],a[2],0}});break;                            // OpTypeArray id elem lengthId
        case 30:{TypeInfo t{in.op,0,0,0};for(std::uint32_t k=1;k<n;++k)t.members.push_back(a[k]);types.push_back({a[0],t});break;} // OpTypeStruct
        case 43:if(n>=3)constants.push_back({a[1],a[2]});break;                                       // OpConstant type id value
        default:break;}
    }
    // Byte size of a type (std140-ish: what OpMemberDecorate Offset + member size yields).
    std::function<std::uint32_t(std::uint32_t,unsigned)> sizeOf=[&](std::uint32_t id,unsigned depth)->std::uint32_t{
        if(depth>8)return 0;const TypeInfo* t{};for(const auto& e:types)if(e.first==id)t=&e.second;if(!t)return 0;
        switch(t->op){
        case 21:case 22:return t->a0/8;
        case 23:return sizeOf(t->a0,depth+1)*t->a1;
        case 24:return 16*t->a1; // column-major mat with 16-byte stride (MatrixStride 16 in this corpus)
        case 28:{std::uint32_t len=0;for(const auto& c:constants)if(c.first==t->a1)len=c.second;std::uint32_t stride=0;for(const auto& st:arrayStrides)if(st.first==id)stride=st.second;if(!stride)stride=sizeOf(t->a0,depth+1);return stride*len;}
        case 30:{std::uint32_t best=0;for(std::uint32_t m=0;m<t->members.size();++m){std::uint32_t off=0;for(const auto& o:offsets)if(o.type==id&&o.member==m)off=o.offset;best=std::max(best,off+sizeOf(t->members[m],depth+1));}return best;}
        default:return 0;}};
    for(const auto& v:vars){
        if(v.storage!=2)continue; // Uniform
        std::uint32_t set=UINT32_MAX,binding=UINT32_MAX;
        for(const auto& d:ds)if(d.first==v.id)set=d.second;
        for(const auto& b:bd)if(b.first==v.id)binding=b.second;
        if(set!=0||binding==UINT32_MAX)continue;
        std::uint32_t st=0;for(const auto& p:ptr)if(p.first==v.ptrType)st=p.second;
        if(!st)continue;
        auto add=[&](CameraMember kind,std::uint32_t member){
            std::uint32_t off=UINT32_MAX;for(const auto& o:offsets)if(o.type==st&&o.member==member)off=o.offset;
            if(off==UINT32_MAX||out.count>=12)return;
            for(unsigned k=0;k<out.count;++k)if(out.entries[k].binding==binding&&out.entries[k].kind==kind)return;
            out.entries[out.count++]={binding,kind==CameraMember::RageMatrices?0u:off,(sizeOf(st,0)+15u)&~15u,kind};};
        for(const auto& m:names){
            if(m.type!=st)continue;
            if(m.name=="gViewInverse"&&binding==1)add(CameraMember::RageMatrices,m.member);
            else if(m.name=="WorldViewProjInverse")add(CameraMember::WvpInverse,m.member);
            else if(m.name=="gCameraMatrix")add(CameraMember::CameraMatrix,m.member);
            else if(m.name=="gPrevViewProj")add(CameraMember::PrevViewProj,m.member);
            else if(m.name=="DepthMapSampler")add(CameraMember::DepthMapSampler,m.member);
            else if(m.name=="ShadowCollectorSampler")add(CameraMember::ShadowCollectorSampler,m.member);
            else if(m.name=="LightBufferSampler")add(CameraMember::LightBufferSampler,m.member);
        }
    }
    out.valid=true;
}
}
