#pragma once
#include "OrdinaryReplayContext.h"
#include "OrdinaryReplayFrame.h"
#include "OrdinaryVtableObserver.h"
#include "NativeEarlyRuntimeBinding.h"
#include "NativeProductionOffsets.h"
#include "OrdinarySinglePass.h"
#include "OrdinaryModelCull.h"
#include "OrdinaryGeometrySnapshot.h"
#include "OrdinaryFixedDepth.h"
#include "OrdinaryCanonicalState.h"
#include "SceneRouteTape.h"
#include <cmath>
#include <utility>

namespace rdr95 {
// The actual synchronous lower-ordinary adapter. Every operation creates and
// closes its own short Read. No native pointer is dereferenced by current().
// Callers MUST keep this adapter at a stable address across LEFT/RIGHT; the
// helpers use its exact API/context identity. No capture or GPU call under Read.
class OrdinaryNativeSnapshot final {
public:
    using Binding=rdrvr::experimental::NativeEarlyRuntimeBinding;
    using Body=ordinary_vtable::Body;
    using Call=ordinary_vtable::Call;
    using Geometry=OrdinaryGeometrySnapshot::Description;
    struct Snapshot {
        OrdinarySinglePass::Snapshot material;
        OrdinaryGeometrySnapshot::Snapshot geometry;
        OrdinaryFixedDepth::Snapshot fixed;
        std::array<std::byte,0xb0> record{};
        Binding::OrdinaryEpoch epoch;
        OrdinaryReplayFrame::Stamp frame;
    };
    enum class Failure {None,Context,Record,Canonical,Material,Geometry,Fixed,Epoch};
    struct Stats {std::uint64_t reads{},bytes{};Failure failure{};unsigned detail{};};
    bool context(const OrdinaryReplayContext& c)noexcept {
        if(active_||!c.binding||!c.views||!c.image||c.image>UINTPTR_MAX-0x4e6eb080||!c.gfx||!c.tlsSlot||!c.scene||!c.pose)return false;
        const auto f=OrdinaryReplayFrame::current();
        if(!f.current()||f.scene()!=c.scene||f.pose()!=c.pose)return false;
        context_=c;return true;
    }
    bool capture(const Body&,const Call&,Snapshot&,Stats&)noexcept;
    bool visibleForCurrentEye(const Snapshot&)noexcept;
    bool matches(const Body&,const Call&,const Snapshot&,Binding::OrdinaryEpoch&,Stats&)noexcept;
private:
    OrdinaryReplayContext context_{};bool active_{};
    Stats* stats_{};
    struct Read {
        OrdinaryNativeSnapshot& s;Binding::OrdinaryRead read;
        static Binding& prepare(OrdinaryNativeSnapshot& s,Stats& stats)noexcept {
            s.active_=true;s.stats_=&stats;return *s.context_.binding;
        }
        Read(OrdinaryNativeSnapshot& owner,Stats& stats)noexcept:s(owner),read(prepare(owner,stats)){}
        ~Read(){s.stats_=nullptr;s.active_=false;}
    };
    bool frame()const noexcept {
        const auto f=OrdinaryReplayFrame::current();
        return f.current()&&f.scene()==context_.scene&&f.pose()==context_.pose;
    }
    static bool copy(void* p,std::uintptr_t a,void* out,std::size_t n)noexcept {
        auto& s=*static_cast<OrdinaryNativeSnapshot*>(p);
        if(!s.active_||!s.stats_)return false;
        ++s.stats_->reads;s.stats_->bytes+=n;
        return Binding::ordinaryCopy(s.context_.binding,a,out,n);
    }
    bool identity(Binding::OrdinaryRead::Identity& out)const noexcept {
        return active_&&frame()&&Binding::ordinaryIdentity(context_.binding,out)&&out.scene==context_.scene;
    }
    bool nativeContext(std::uintptr_t& gfx,std::uintptr_t& vk,std::uint64_t& borrow)noexcept {
        Binding::OrdinaryRead::Identity i;std::uintptr_t point{};
        if(!identity(i)||!copy(this,context_.tlsSlot,&gfx,sizeof gfx)||gfx!=context_.gfx||
           gfx>UINTPTR_MAX-0x130||!copy(this,gfx,&point,sizeof point)||point!=context_.image+retail::gfxVtable||
           !copy(this,gfx+0x128,&vk,sizeof vk)||!vk||
           !copy(this,vk,&point,sizeof point)||point!=context_.image+retail::vkVtable)return false;
        borrow=i.borrow;return true;
    }
    OrdinarySinglePass::Api materialApi()noexcept;
    OrdinaryGeometrySnapshot::Api geometryApi()noexcept;
    OrdinaryFixedDepth::Api fixedApi()noexcept;
    OrdinaryCanonicalState::Api canonicalApi()noexcept;
    bool record(const Body&,const Call&,std::array<std::byte,0xb0>&)noexcept;
    static OrdinarySinglePass::Call call(const Call& c)noexcept {
        return {reinterpret_cast<std::uintptr_t>(c.drawable),c.lod,c.technique,reinterpret_cast<std::uintptr_t>(c.bits)};
    }
    static bool fail(Stats& s,Failure f,unsigned detail=0)noexcept{s.failure=f;s.detail=detail;return false;}
};
inline OrdinarySinglePass::Api OrdinaryNativeSnapshot::materialApi()noexcept {
    const auto b=context_.image;
    return {this,[](void* p,std::uintptr_t a,void* out,std::size_t n)noexcept{
        const bool ok=copy(p,a,out,n);scene_routes::materialRead(a,out,unsigned(n),ok);return ok;
    },[](void* p,OrdinarySinglePass::Boundary& out)noexcept{
        auto& s=*static_cast<OrdinaryNativeSnapshot*>(p);Binding::OrdinaryRead::Identity i{};
        const bool ok=s.identity(i);scene_routes::materialBoundary(i.borrow,i.scene,i.resources,i.effects,ok);
        if(!ok)return false;out={i.borrow,i.scene,i.resources,i.effects};return true;
    },{b+ordinary_vtable::addressPoints[0],b+ordinary_vtable::addressPoints[1],b+ordinary_vtable::addressPoints[2]},b+retail::geometryVtable,
      b+retail::forceShaderSlot,b+retail::shaderSorterSlot,b+retail::commandBufferSlot,
      b+retail::forcedGroup,b+retail::litGroup,b+retail::unlitGroup,b+retail::shadow+4,
      b+retail::fallbackGroup,b+retail::fallbackEnabled,
      b+0x4e6eb040,b+retail::shadowBufferSlot,b+0x2e7a3f0,b+0x1b12904}; // dynsym g_GlobalAlphaFadeVar, actual Common prefix value.
}
inline OrdinaryGeometrySnapshot::Api OrdinaryNativeSnapshot::geometryApi()noexcept {
    const auto b=context_.image;
    return {this,copy,[](void* p,OrdinaryGeometrySnapshot::Boundary& out)noexcept{
        auto& s=*static_cast<OrdinaryNativeSnapshot*>(p);Binding::OrdinaryRead::Identity i;
        if(!s.identity(i))return false;out={i.borrow,i.scene,i.resources,i.effects,i.meshes};return true;
    },b+retail::geometryVtable,b+retail::vertexVtable,b+retail::indexVtable,b+retail::bufferVtable,b+retail::shadowBufferSlot};
}
inline OrdinaryFixedDepth::Api OrdinaryNativeSnapshot::fixedApi()noexcept {
    const auto b=context_.image;
    return {this,copy,[](void* p,OrdinaryFixedDepth::Boundary& out)noexcept{
        auto& s=*static_cast<OrdinaryNativeSnapshot*>(p);Binding::OrdinaryRead::Identity i;
        std::uintptr_t gfx{},vk{};std::uint64_t borrow{};
        if(!s.nativeContext(gfx,vk,borrow)||!s.identity(i)||i.borrow!=borrow)return false;
        out={gfx,vk,i.borrow,i.scene,i.resources,i.effects};return true;
    },b+retail::gfxVtable,b+retail::vkVtable,b+retail::rasterVtable,b+retail::depthVtable,
      b+retail::pendingRaster,b+retail::pendingDepth,b+retail::readyFlags,b+retail::factoryDirty};
}
inline OrdinaryCanonicalState::Api OrdinaryNativeSnapshot::canonicalApi()noexcept {
    const auto b=context_.image;
    return {this,copy,[](void* p,OrdinaryCanonicalState::Boundary& out)noexcept{
        auto& s=*static_cast<OrdinaryNativeSnapshot*>(p);return s.nativeContext(out.gfx,out.vk,out.revision);
    },b,b+retail::gfxVtable,b+retail::vkVtable};
}
inline bool OrdinaryNativeSnapshot::record(const Body& b,const Call& c,std::array<std::byte,0xb0>& out)noexcept {
    bool supportedType=false;
    for(auto offset:ordinary_vtable::addressPoints)if(b.addressPoint==context_.image+offset)supportedType=true;
    if(!b.container||!b.record||b.record>UINTPTR_MAX-out.size()||!b.drawable||
       reinterpret_cast<std::uintptr_t>(c.world)!=b.record||reinterpret_cast<std::uintptr_t>(c.previous)!=b.record+0x40||
       reinterpret_cast<std::uintptr_t>(c.drawable)!=b.drawable||c.bits||c.caller!=context_.image+ordinary_vtable::callerOffset||
       !supportedType||
       !copy(this,b.record,out.data(),out.size()))return false;
    std::uintptr_t drawable{},matrices{},creature{},point{};std::uint32_t flags{};
    std::memcpy(&drawable,out.data()+0x80,8);std::memcpy(&matrices,out.data()+0x90,8);
    std::memcpy(&creature,out.data()+0x98,8);std::memcpy(&flags,out.data()+0xac,4);
    if(drawable!=b.drawable||matrices||creature||(flags&0x1e000u)||
       !copy(this,drawable,&point,sizeof point)||point!=b.addressPoint||std::memcmp(out.data(),out.data()+0x40,64))return false;
    for(unsigned n=0;n<64;n+=4){float f{};std::memcpy(&f,out.data()+n,4);if(!std::isfinite(f))return false;}
    return true;
}
inline bool OrdinaryNativeSnapshot::capture(const Body& body,const Call& c,Snapshot& out,Stats& stats)noexcept {
    stats={};if(active_||!context_.binding||!frame())return fail(stats,Failure::Context);
    const auto entered=OrdinaryReplayFrame::current();
    Snapshot saved;Read scope(*this,stats);if(!scope.read.current()||!entered.current())return fail(stats,Failure::Context,100+scope.read.diagnostic());
    if(!record(body,c,saved.record))return fail(stats,Failure::Record);
    if(!OrdinaryCanonicalState::clear(canonicalApi()))return fail(stats,Failure::Canonical);
    OrdinarySinglePass::Stats ms;OrdinarySinglePass::Description material;
    const auto ma=materialApi();
    scene_routes::materialBegin({ma.drawableTypes[0],ma.drawableTypes[1],ma.drawableTypes[2],ma.geometryType,
        ma.forceShader,ma.shaderSorter,ma.commandBuffer,ma.forcedGroup,ma.litGroup,ma.unlitGroup,ma.lighting,
        ma.fallbackGroup,ma.fallbackEnabled,ma.currentFade,ma.shadowBuffer},reinterpret_cast<std::uintptr_t>(c.drawable),c.lod,c.technique,reinterpret_cast<std::uintptr_t>(c.bits));
    const bool materialOk=OrdinarySinglePass::capture(ma,call(c),saved.material,ms);
    scene_routes::materialEnd(materialOk,unsigned(ms.failure),ms.reads,ms.bytes,ms.ranges);
    if(!materialOk||!saved.material.describe(material))return fail(stats,Failure::Material,unsigned(ms.failure));
    OrdinaryGeometrySnapshot::Stats gs;
    if(!OrdinaryGeometrySnapshot::capture(geometryApi(),material.geometry,saved.geometry,gs))return fail(stats,Failure::Geometry,unsigned(gs.failure));
    OrdinaryFixedDepth::Stats fs;
    if(!OrdinaryFixedDepth::capture(fixedApi(),saved.fixed,fs))return fail(stats,Failure::Fixed,unsigned(fs.failure));
    Geometry selected;
    if(!saved.geometry.describe(selected))return fail(stats,Failure::Geometry);
    if(!scope.read.semanticStamp(saved.epoch,selected.vertex,selected.index)||!entered.current())return fail(stats,Failure::Epoch);
    std::array<std::byte,0xb0> finalRecord;
    if(!record(body,c,finalRecord)||finalRecord!=saved.record)return fail(stats,Failure::Record);
    saved.frame=entered;
    if(!saved.epoch.current()||!saved.frame.current())return fail(stats,Failure::Epoch);
    out=std::move(saved);return true;
}
inline bool OrdinaryNativeSnapshot::visibleForCurrentEye(const Snapshot& saved)noexcept {
    if(active_||!frame()||!saved.frame.current()||!saved.epoch.current())return false;
    OrdinarySinglePass::Description material;
    if(!saved.material.describe(material))return false;
    if(!material.modelBounds||!material.modelCull)return true;
    if(material.modelCull!=context_.image+0x1b12904)return false;
    // Exact pinned ModelVisibleWithAABB is read-only and takes one AABB. It
    // reads the currently bound native viewport, just as the original call.
    // Unknown/culled results fall back to native DrawSingle; never force draw.
    return rdr95model::modelVisible(material,context_.image+0x1b12904)&&frame()&&saved.frame.current()&&saved.epoch.current();
}
inline bool OrdinaryNativeSnapshot::matches(const Body& body,const Call& c,const Snapshot& saved,Binding::OrdinaryEpoch& out,Stats& stats)noexcept {
    stats={};if(active_||!context_.binding||!frame()||!saved.frame.current()||!saved.epoch.current())return fail(stats,Failure::Context);
    Read scope(*this,stats);if(!scope.read.current())return fail(stats,Failure::Context);
    std::array<std::byte,0xb0> now;
    if(!record(body,c,now)||now!=saved.record)return fail(stats,Failure::Record);
    if(!OrdinaryCanonicalState::clear(canonicalApi()))return fail(stats,Failure::Canonical);
    OrdinarySinglePass::Stats ms;OrdinarySinglePass::Description material;
    if(!saved.material.describe(material)||!OrdinarySinglePass::matches(materialApi(),call(c),saved.material,ms))return fail(stats,Failure::Material,unsigned(ms.failure));
    OrdinaryGeometrySnapshot::Stats gs;
    if(!OrdinaryGeometrySnapshot::matches(geometryApi(),material.geometry,saved.geometry,gs))return fail(stats,Failure::Geometry,unsigned(gs.failure));
    OrdinaryFixedDepth::Stats fs;
    if(!OrdinaryFixedDepth::matches(fixedApi(),saved.fixed,fs))return fail(stats,Failure::Fixed,unsigned(fs.failure));
    Binding::OrdinaryEpoch final;
    Geometry selected;
    if(!saved.geometry.describe(selected))return fail(stats,Failure::Geometry);
    if(!scope.read.semanticStamp(final,selected.vertex,selected.index)||!saved.epoch.current()||!saved.frame.current())return fail(stats,Failure::Epoch);
    if(!record(body,c,now)||now!=saved.record)return fail(stats,Failure::Record);
    if(!final.current()||!saved.epoch.current()||!saved.frame.current())return fail(stats,Failure::Epoch);
    out=final;return true;
}
}
