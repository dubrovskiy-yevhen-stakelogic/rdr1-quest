#pragma once
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rdr95 {
// Copied comparison of the selected ordinary material path. This is neither
// a native memory lease nor shader/geometry/replay admission. The caller uses
// its actual qualified physical reader during EACH synchronous operation.
class OrdinarySinglePass final {
public:
    using Address=std::uintptr_t;
    static_assert(sizeof(Address)==8,"Pinned native layout requires 64 bits");
    struct Boundary {
        std::uint64_t borrow{},scene{},resources{},effects{};
        bool operator==(const Boundary&)const=default;
    };
    struct Call {
        Address drawable{};int lod{},technique{};Address bits{};
        bool operator==(const Call&)const=default;
    };
    struct Api {
        void* context{};
        bool(*read)(void*,Address,void*,std::size_t)noexcept{};
        bool(*boundary)(void*,Boundary&)noexcept{};
        std::array<Address,3> drawableTypes{};Address geometryType{};
        Address forceShader{},shaderSorter{},commandBuffer{};
        Address forcedGroup{},litGroup{},unlitGroup{},lighting{};
        Address fallbackGroup{},fallbackEnabled{},currentFade{},shadowBuffer{};
        Address modelCullSlot{},modelCullFunction{};
        bool operator==(const Api&)const=default;
    };
    struct Description {
        Address holder{},models{},model{},geometries{},geometry{},materialIndices{};
        Address group{},materials{},material{},effect{},techniques{},technique{},pass{};
        unsigned selectedTechnique{};std::uint16_t materialIndex{};
        std::uint8_t geometryFlags{};std::array<std::byte,6> passBytes{};
        Address modelBounds{},modelCull{};
        alignas(16) std::array<float,8> bounds{};
        bool operator==(const Description&)const=default;
    };
    enum class Failure {None,Unqualified,Read,Budget,Shape,Callback,Bucket,Selector,PendingAlias,Pass,Changed};
    struct Stats {unsigned reads{},bytes{},ranges{};Failure failure{};};
private:
    static constexpr unsigned MaxRanges=64,MaxBytes=512;
    struct Range {Address address{};std::uint16_t offset{},size{};};
public:
    class Snapshot {
        friend class OrdinarySinglePass;
        Api api_{};Call call_{};Boundary boundary_{};Description description_{};
        std::array<Range,MaxRanges> ranges_{};
        std::array<std::byte,MaxBytes> bytes_{};unsigned count_{},used_{};bool valid_{};
    public:
        Snapshot()=default;
        bool describe(Description& out)const noexcept {if(!valid_)return false;out=description_;return true;}
    };
    static bool capture(const Api&,const Call&,Snapshot&,Stats&)noexcept;
    static bool matches(const Api&,const Call&,const Snapshot&,Stats&)noexcept;
private:
    struct Errno {int* cell=&errno;int saved=*cell;~Errno(){*cell=saved;}};
    static bool range(Address p,std::size_t n)noexcept{return p&&n&&n<=UINTPTR_MAX-p;}
    static bool qualified(const Api& a,const Call& c)noexcept {
        return a.read&&a.boundary&&a.drawableTypes[0]&&a.drawableTypes[1]&&a.geometryType&&
            a.forceShader&&a.shaderSorter&&a.commandBuffer&&a.forcedGroup&&a.litGroup&&a.unlitGroup&&
            a.lighting&&a.fallbackGroup&&a.fallbackEnabled&&a.currentFade&&a.shadowBuffer&&
            c.drawable&&c.lod>=0&&c.lod<4&&c.technique>=0&&c.technique<=255&&!c.bits;
    }
    static bool boundary(const Api& a,Boundary& b)noexcept {
        return a.boundary(a.context,b)&&b.borrow&&b.scene&&b.resources&&b.effects;
    }
    static bool fail(Stats& s,Failure f)noexcept{s.failure=f;return false;}
    static bool read(const Api& a,Address p,void* out,unsigned n,Stats& s)noexcept {
        if(!range(p,n))return fail(s,Failure::Read);
        ++s.reads;s.bytes+=n;return a.read(a.context,p,out,n)||fail(s,Failure::Read);
    }
    struct Reader {
        const Api& api;Snapshot& saved;Stats& stats;
        bool copy(Address p,void* out,unsigned n)noexcept {
            if(!n||n>16||saved.count_==MaxRanges||n>MaxBytes-saved.used_)return fail(stats,Failure::Budget);
            if(!read(api,p,out,n,stats))return false;
            // An exact repeated field must still match its first observation.
            // Keep the original parent-before-child position in the journal.
            for(unsigned i=0;i<saved.count_;++i){const auto& r=saved.ranges_[i];
                if(r.address==p&&r.size==n)return std::memcmp(out,saved.bytes_.data()+r.offset,n)==0||fail(stats,Failure::Changed);
            }
            saved.ranges_[saved.count_++]={p,static_cast<std::uint16_t>(saved.used_),static_cast<std::uint16_t>(n)};
            std::memcpy(saved.bytes_.data()+saved.used_,out,n);saved.used_+=n;return true;
        }
        template<class T>bool at(Address p,unsigned offset,T& out)noexcept {
            if(!p||offset>UINTPTR_MAX-p)return fail(stats,Failure::Read);
            return copy(p+offset,&out,sizeof out);
        }
    };
    template<class T,std::size_t N>static T field(const std::array<std::byte,N>& b,unsigned offset)noexcept {
        T v{};std::memcpy(&v,b.data()+offset,sizeof v);return v;
    }
    static bool compare(const Api& a,const Snapshot& saved,Stats& s)noexcept {
        std::array<std::byte,16> now{};
        for(unsigned i=0;i<saved.count_;++i){const auto& r=saved.ranges_[i];
            if(!read(a,r.address,now.data(),r.size,s))return false;
            if(std::memcmp(now.data(),saved.bytes_.data()+r.offset,r.size))return fail(s,Failure::Changed);
        }
        s.ranges=saved.count_;return true;
    }
};

inline bool OrdinarySinglePass::capture(const Api& a,const Call& call,Snapshot& out,Stats& stats)noexcept {
    Errno preserve;stats={};if(!qualified(a,call))return fail(stats,Failure::Unqualified);
    const Api initialApi=a;const Call initialCall=call;
    Snapshot result;Boundary first{};
    if(!boundary(a,first))return fail(stats,Failure::Unqualified);
    Reader r{a,result,stats};auto& d=result.description_;
    Address type{},value{};
    if(!r.at(call.drawable,0,type))return false;
    bool supportedType=false;
    for(auto allowed:a.drawableTypes)if(allowed&&type==allowed)supportedType=true;
    if(!supportedType)return fail(stats,Failure::Shape);
    if(!r.at(a.forceShader,0,value)||value||!r.at(a.shaderSorter,0,value)||value||!r.at(a.commandBuffer,0,value)||value)
        return stats.failure==Failure::None?fail(stats,Failure::Callback):false;
    std::uint16_t count{};
    if(!r.at(call.drawable,0x50+unsigned(call.lod)*8,d.holder)||!d.holder||
       !r.at(d.holder,0,d.models)||!r.at(d.holder,8,count)||count!=1||!d.models||
       !r.at(d.models,0,d.model)||!d.model)return fail(stats,stats.failure==Failure::None?Failure::Shape:stats.failure);
    std::array<std::byte,16> modelLinks{};std::array<std::byte,3> modelKind{};
    if(!r.at(d.model,8,d.geometries)||!r.at(d.model,0x18,modelLinks)||!r.at(d.model,0x2d,modelKind))return false;
    d.materialIndices=field<Address>(modelLinks,8);
    d.modelBounds=field<Address>(modelLinks,0);
    if(field<std::uint8_t>(modelKind,0)||field<std::uint16_t>(modelKind,1)!=1||!d.geometries||!d.materialIndices)
        return fail(stats,Failure::Shape);
    // Native Model::Draw calls its model cull callback before the one-geometry
    // fast path. Retain the exact callback and immutable AABB, then call it for
    // the RIGHT camera outside the native Read before emitting cached work.
    if(d.modelBounds){
        if(!a.modelCullSlot||!a.modelCullFunction||!r.at(a.modelCullSlot,0,d.modelCull))
            return fail(stats,Failure::Callback);
        if(d.modelCull && d.modelCull!=a.modelCullFunction)return fail(stats,Failure::Callback);
        if(d.modelCull){
            std::array<float,4> low{},high{};
            if(!r.at(d.modelBounds,0,low)||!r.at(d.modelBounds,16,high))return false;
            for(unsigned i=0;i<4;++i){
                if(!std::isfinite(low[i])||!std::isfinite(high[i])||(i<3&&low[i]>high[i]))return fail(stats,Failure::Shape);
                d.bounds[i]=low[i];d.bounds[i+4]=high[i];
            }
        }
    }
    if(!r.at(d.geometries,0,d.geometry)||!d.geometry||!r.at(d.geometry,0,type)||type!=a.geometryType||
       !r.at(d.geometry,0x14,d.geometryFlags)||!r.at(d.materialIndices,0,d.materialIndex))return fail(stats,stats.failure==Failure::None?Failure::Shape:stats.failure);
    if(!r.at(call.drawable,0x10,d.group)||!d.group||!r.at(d.group,0x40,d.materials)||
       !r.at(d.group,0x48,count)||!count||d.materialIndex>=count||!d.materials||
       !r.at(d.materials,unsigned(d.materialIndex)*8,d.material)||!d.material)return fail(stats,stats.failure==Failure::None?Failure::Shape:stats.failure);
    std::uint8_t bucket{};
    if(!r.at(d.material,0x19,bucket))return false;
    // Native DrawSingle forwards Call.technique to this BUCKET comparison.
    // grmShader::Draw later passes explicit effect technique zero to BeginDraw.
    if(bucket!=call.technique)return fail(stats,Failure::Bucket);
    if(!r.at(d.material,0x10,d.effect)||!d.effect)return fail(stats,stats.failure==Failure::None?Failure::Shape:stats.failure);
    std::array<std::byte,12> table{};
    if(!r.at(d.effect,0x10,table))return false;
    d.techniques=field<Address>(table,0);const auto n=field<std::uint16_t>(table,8),capacity=field<std::uint16_t>(table,10);
    if(!d.techniques||!n||n>capacity||capacity>256||!range(d.techniques,std::size_t(capacity)*32))return fail(stats,Failure::Selector);
    int group{};if(!r.at(a.forcedGroup,0,group))return false;
    if(group==-1){std::uint32_t lit{};if(!r.at(a.lighting,0,lit)||!r.at(lit?a.litGroup:a.unlitGroup,0,group))return false;}
    unsigned chosen{};
    auto choose=[&](int g)noexcept {std::uint8_t t{};if(g<0||g>=64)return fail(stats,Failure::Selector);
        if(!r.at(d.effect,0x50+unsigned(g)*2,t))return false;if(t>n)return fail(stats,Failure::Selector);chosen=t;return true;};
    auto hasPass=[&](bool& yes)noexcept {yes=false;if(!chosen)return true;std::uint16_t passes{};
        if(!r.at(d.techniques,(chosen-1)*32+0x18,passes))return false;yes=passes!=0;return true;};
    bool passes{};if(!choose(group)||!hasPass(passes))return false;
    if(!passes){int fallback{};if(!r.at(a.fallbackGroup,0,fallback))return false;
        if(fallback!=-1&&(!choose(fallback)||!hasPass(passes)))return false;
        if(!passes){std::uint8_t enabled{};if(!r.at(a.fallbackEnabled,0,enabled))return false;
            if(enabled){int lit{};if(!r.at(a.litGroup,0,lit)||!choose(lit))return false;}
        }
    }
    if(!chosen)return fail(stats,Failure::Selector);
    float fade{};if(!r.at(a.currentFade,0,fade))return false;
    if(!std::isfinite(fade))return fail(stats,Failure::Selector);
    if(fade==1.f){Address aliases{};if(!r.at(d.effect,0x180,aliases))return false;
        if(aliases){int t{};if(!r.at(aliases,(chosen-1)*4,t))return false;
            if(t<0||t>n)return fail(stats,Failure::Selector);if(t)chosen=unsigned(t);}
    }
    std::uint8_t shadow{};if(!r.at(a.shadowBuffer,0,shadow))return false;
    if(shadow&&(d.geometryFlags&1)){Address aliases{};std::uint16_t size{};int t{};
        if(!r.at(d.effect,0x170,aliases)||!r.at(d.effect,0x178,size)||!aliases||chosen>size||
           !r.at(aliases,(chosen-1)*4,t))return fail(stats,stats.failure==Failure::None?Failure::Selector:stats.failure);
        if(!t)return fail(stats,Failure::PendingAlias);
        if(t!=-1){if(t<1||t>n)return fail(stats,Failure::Selector);chosen=unsigned(t);}
    }
    d.selectedTechnique=chosen;d.technique=d.techniques+(chosen-1)*32;
    if(!r.at(d.technique,0x10,table))return false;
    d.pass=field<Address>(table,0);const auto passCount=field<std::uint16_t>(table,8),passCapacity=field<std::uint16_t>(table,10);
    if(passCount!=1||passCount>passCapacity||passCapacity>256||!range(d.pass,std::size_t(passCapacity)*6))return fail(stats,Failure::Pass);
    if(!r.at(d.pass,0,d.passBytes))return false;
    if((field<std::uint16_t>(d.passBytes,4)>>10)&15u)return fail(stats,Failure::Pass);
    // Verify selected parents before children again. No native pointer is used
    // after a mismatching parent, including on the later RIGHT comparison.
    if(!compare(a,result,stats))return false;
    Boundary final{};if(!boundary(a,final)||final!=first||a!=initialApi||call!=initialCall)return fail(stats,Failure::Changed);
    result.api_=a;result.call_=call;result.boundary_=first;result.valid_=true;out=result;return true;
}
inline bool OrdinarySinglePass::matches(const Api& a,const Call& call,const Snapshot& saved,Stats& stats)noexcept {
    Errno preserve;stats={};if(!saved.valid_||!qualified(a,call)||a!=saved.api_||call!=saved.call_)return fail(stats,Failure::Unqualified);
    Boundary first{};if(!boundary(a,first)||first.scene!=saved.boundary_.scene||first.resources!=saved.boundary_.resources||first.effects!=saved.boundary_.effects)
        return fail(stats,Failure::Changed);
    if(!compare(a,saved,stats))return false;
    Boundary final{};if(!boundary(a,final)||final!=first||a!=saved.api_||call!=saved.call_)return fail(stats,Failure::Changed);
    return true;
}
}
