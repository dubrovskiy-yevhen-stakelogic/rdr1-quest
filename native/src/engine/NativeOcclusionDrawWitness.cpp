#include "NativeOcclusionDrawWitness.h"
#include <cerrno>
#include <cmath>
#include <cstring>

namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
bool fits(std::uintptr_t p,std::size_t n)noexcept{return p&&!(p&7)&&n&&p<=UINTPTR_MAX-n;}
template<class T>T read(std::uintptr_t p,std::size_t o=0)noexcept{T v;std::memcpy(&v,reinterpret_cast<const void*>(p+o),sizeof v);return v;}
bool objectEqual(float a,float b)noexcept{return std::isfinite(a)&&a==b;}
struct ClearFailedSnapshot {
    NativeOcclusionDrawWitness::Snapshot& out;bool complete{};
    ~ClearFailedSnapshot(){if(!complete)out={};}
};
}
bool NativeOcclusionDrawWitness::capture(const Api& a,void* live,const Binding& binding,
                                        const Target& target,bool qualified,Snapshot& out)noexcept {
    return captureState(a,live,binding,target,qualified,out)&&captureMatrices(a,live,binding,out);
}
bool NativeOcclusionDrawWitness::captureState(const Api& a,void* live,const Binding& binding,
                                             const Target& target,bool qualified,Snapshot& out)noexcept {
    Errno preserve;ClearFailedSnapshot guard{out};auto& next=out;
    next.valid=next.stateValid=next.matrices.valid=false;
    if(!qualified||!a.completeCodeProof||!a.contextVtable||!a.pipelineVtable||!a.framebufferVtable||!a.bufferVtable||
       !target.verified||!target.colorSurfaces[0]||!target.depthSurface||target.colorSurfaces[0]==target.depthSurface||
       target.width<16||target.height<16||target.width>8192||target.height>8192||!binding.valid)return false;
    const auto context=reinterpret_cast<std::uintptr_t>(live);
    if(!fits(context,0x3468)||read<std::uintptr_t>(context)!=a.contextVtable||read<std::uint32_t>(context,0x32dc))return false;
    next.context=context;next.frame=read<std::uintptr_t>(context,0x30a0);
    next.nativePipeline=read<std::uintptr_t>(context,0x30c8);
    next.framebuffer=read<std::uintptr_t>(context,0x30b0);next.renderPass=read<std::uintptr_t>(context,0x30a8);
    if(!fits(next.frame,0xb0)||!fits(next.nativePipeline,0x20)||!fits(next.framebuffer,0x100)||!next.renderPass||
       read<std::uintptr_t>(context,0x30b8)!=next.framebuffer||
       read<std::uintptr_t>(next.nativePipeline)!=a.pipelineVtable||read<std::uintptr_t>(next.framebuffer)!=a.framebufferVtable)return false;
    next.command=read<std::uintptr_t>(next.frame,0x38);next.pipeline=read<std::uintptr_t>(next.nativePipeline,0x18);
    if(!next.command||next.command!=binding.command||!next.pipeline||!read<std::uint32_t>(next.framebuffer,0xc)||
       read<std::uintptr_t>(next.framebuffer,0x18)!=target.depthSurface||
       read<std::uint32_t>(next.framebuffer,0xf8)!=target.width||read<std::uint32_t>(next.framebuffer,0xfc)!=target.height||
       read<std::uint32_t>(context,0x32d4)!=target.width||read<std::uint32_t>(context,0x32d8)!=target.height)return false;
    for(unsigned i=0;i<8;++i)if(read<std::uintptr_t>(next.framebuffer,0x20+i*8)!=target.colorSurfaces[i]||
                               target.colorSurfaces[i]==target.depthSurface)return false;
    next.viewport=read<std::array<float,6>>(context,0x3390);
    next.scissorOffset=read<std::array<std::int32_t,2>>(context,0x3380);
    next.scissorExtent=read<std::array<std::uint32_t,2>>(context,0x3388);
    for(float f:next.viewport)if(!std::isfinite(f))return false;
    // Binding input is complete, so these are native last-emitted raster/cache
    // fields, not pending SetViewport/SetScissor values at +0x3450/+0x3440.
    if(next.viewport[0]!=0||next.viewport[2]!=float(target.width)||next.viewport[4]!=0||next.viewport[5]!=1||
       !((next.viewport[1]==0&&next.viewport[3]==float(target.height))||
         (next.viewport[1]==float(target.height)&&next.viewport[3]==-float(target.height)))||
       next.scissorOffset!=std::array<std::int32_t,2>{0,0}||
       next.scissorExtent!=std::array<std::uint32_t,2>{target.width,target.height})return false;
    next.vertexBuffer=read<std::uint64_t>(context,0x32e8);next.vertexOffset=read<std::uint64_t>(context,0x3328);
    next.indexBuffer=read<std::uint64_t>(context,0x3368);
    if(!next.vertexBuffer||!next.indexBuffer||next.vertexOffset)return false;
    // The pinned native BindInputLayout issues index offset 0 / UINT16. Its
    // current-scope actual dynamic binding is required; no old descriptor is
    // guessed from the constant-buffer offset or mutable CPU shadow.
    if(read<std::uintptr_t>(context,0x30a0)!=next.frame||read<std::uintptr_t>(context,0x30c8)!=next.nativePipeline||
       read<std::uintptr_t>(context,0x30b0)!=next.framebuffer||read<std::uintptr_t>(context,0x30a8)!=next.renderPass||
       read<std::uint32_t>(context,0x32dc))return false;
    next.stateValid=true;guard.complete=true;return true;
}
bool NativeOcclusionDrawWitness::captureMatrices(const Api& a,void* live,const Binding& binding,Snapshot& out)noexcept {
    Errno preserve;ClearFailedSnapshot guard{out};out.valid=out.matrices.valid=false;
    const auto context=reinterpret_cast<std::uintptr_t>(live);
    if(!a.completeCodeProof||!a.contextVtable||!a.bufferVtable||!out.stateValid||!binding.valid||
       context!=out.context||!fits(context,0x3468)||read<std::uintptr_t>(context)!=a.contextVtable||
       binding.command!=out.command)return false;
    auto sameState=[&]()noexcept {
        return read<std::uintptr_t>(context,0x30a0)==out.frame&&read<std::uintptr_t>(context,0x30c8)==out.nativePipeline&&
            read<std::uintptr_t>(context,0x30b0)==out.framebuffer&&read<std::uintptr_t>(context,0x30b8)==out.framebuffer&&
            read<std::uintptr_t>(context,0x30a8)==out.renderPass&&!read<std::uint32_t>(context,0x32dc)&&
            read<std::uint64_t>(context,0x32e8)==out.vertexBuffer&&read<std::uint64_t>(context,0x3328)==out.vertexOffset&&
            read<std::uint64_t>(context,0x3368)==out.indexBuffer&&
            read<std::array<float,6>>(context,0x3390)==out.viewport&&
            read<std::array<std::int32_t,2>>(context,0x3380)==out.scissorOffset&&
            read<std::array<std::uint32_t,2>>(context,0x3388)==out.scissorExtent;
    };
    if(!NativeStaticMatrixWitness::capture({a.contextVtable,a.bufferVtable,true},live,binding,out.matrices)||!sameState())return false;
    out.valid=true;guard.complete=true;return true;
}
bool NativeOcclusionDrawWitness::geometry(const Api& a,const void* live,bool qualified,Geometry& out)noexcept {
    Errno preserve;out={};const auto p=reinterpret_cast<std::uintptr_t>(live);
    auto decline=[&](GeometryFailure why){out.failure=why;return false;};
    if(!qualified||!a.completeCodeProof||!a.geometryVtable||!a.shadowBufferEnabledSlot)return decline(GeometryFailure::Unqualified);
    if(!fits(p,8))return decline(GeometryFailure::Address);
    if(read<std::uintptr_t>(p)!=a.geometryVtable)return decline(GeometryFailure::Type);
    if(!fits(p,0xc0))return decline(GeometryFailure::Address);
    // Both native constructors set geometry+14 to 1. That byte alone does not
    // select a CPU/prepared shadow buffer: native Draw at 1b19160..68 combines
    // it with grmGeometry::sm_UseShadowBuffer. Follow that exact predicate.
    const bool optimized=read<std::uint8_t>(p,0x14)&&read<std::uint8_t>(a.shadowBufferEnabledSlot);
    if(read<std::uint8_t>(p,0x63))return decline(GeometryFailure::Ring);
    if(read<std::uintptr_t>(p,0x88))return decline(GeometryFailure::ExtraStream);
    if(read<std::uintptr_t>(p,0xa0))return decline(GeometryFailure::PendingTask);
    // Optimize frees ordinary declaration+8 and may free original VB+18.
    // Native ring-zero Draw instead reads position VB+b0 and declaration+a8.
    // Optional attribute VB+b8 is not a position stream; the emitted pipeline
    // certificate still requires position binding zero and the actual VB match.
    const auto vertices=read<std::uintptr_t>(p,optimized?0xb0:0x18),indices=read<std::uintptr_t>(p,0x38);
    const auto declaration=read<std::uintptr_t>(p,optimized?0xa8:8);
    if(optimized&&(!vertices||!declaration))return decline(GeometryFailure::ShadowBuffer);
    const auto count=read<std::uint32_t>(p,0x58);
    if(!fits(vertices,8))return decline(GeometryFailure::Vertices);
    if(!fits(indices,8))return decline(GeometryFailure::Indices);
    if(!count||count%3)return decline(GeometryFailure::IndexCount);
    if(!fits(declaration,8))return decline(GeometryFailure::Declaration);
    // The pinned grcDevice::DrawIndexedPrimitive converts its grcDrawMode
    // before BindPrimitiveType / VkContext::DrawIndexed (CSINC at +0x38).
    // Comparing the raw geometry byte to the leaf's enum rejects every usual
    // triangle list: raw 3 is d11 primitive 4.
    const unsigned rawMode=read<std::uint8_t>(p,0x62);
    const unsigned primitiveType=rawMode<8?rawMode+1:4;
    out={reinterpret_cast<const void*>(vertices),reinterpret_cast<const void*>(indices),count,primitiveType,true,GeometryFailure::None};return true;
}
bool NativeOcclusionDrawWitness::matchesGeometry(const Api& a,const Geometry& geometry,const Snapshot& actual,
                                                unsigned positionBytes,unsigned stride,
                                                NativeGeometryBindingReceipt* sourceReceipt)noexcept {
    Errno preserve;if(sourceReceipt)sourceReceipt->reset();
    if(!a.completeCodeProof||!a.vertexVtable||!a.indexVtable||!a.bufferVtable||
       !geometry.valid||geometry.mode!=4||!geometry.indexCount||geometry.indexCount%3||
       !actual.valid||!actual.vertexBuffer||!actual.indexBuffer||actual.vertexOffset||
       (positionBytes!=12&&positionBytes!=16)||stride<positionBytes||stride>256||stride%4)return false;
    const auto v=reinterpret_cast<std::uintptr_t>(geometry.vertices),i=reinterpret_cast<std::uintptr_t>(geometry.indices);
    // A short unknown native class must be rejected before derived fields.
    if(!fits(v,8)||!fits(i,8)||read<std::uintptr_t>(v)!=a.vertexVtable||read<std::uintptr_t>(i)!=a.indexVtable)return false;
    if(!fits(v,0x40)||!fits(i,0x28)||read<std::uint32_t>(v,0x18)!=stride)return false;
    const auto f=read<std::uintptr_t>(v,0x30),vb=read<std::uintptr_t>(v,0x38),ib=read<std::uintptr_t>(i,0x18);
    if(!fits(f,16)||!fits(vb,8)||!fits(ib,8)||
       read<std::uintptr_t>(vb)!=a.bufferVtable||read<std::uintptr_t>(ib)!=a.bufferVtable)return false;
    if(!fits(vb,0x28)||!fits(ib,0x28))return false;
    const unsigned format=unsigned(read<std::uint64_t>(f,8)&15);
    // Position is first in both pinned FVF channel orders. No packed decoding
    // or optional attribute stream can supply this certified XYZ position.
    if(!(read<std::uint32_t>(f)&1)||read<std::uint8_t>(f,4)!=stride||
       (format!=6&&format!=7)||positionBytes!=(format==7?16u:12u)||
       read<std::uint32_t>(vb,0x24)!=0||read<std::uint32_t>(ib,0x24)!=1||
       read<std::uint64_t>(vb,0x10)!=actual.vertexBuffer||read<std::uint64_t>(ib,0x10)!=actual.indexBuffer)return false;
    const unsigned vertices=read<std::uint16_t>(v,8),indices=read<std::uint32_t>(i,8);
    const auto vertexBytes=read<std::uint32_t>(vb,0x20),indexBytes=read<std::uint32_t>(ib,0x20);
    if(!vertices||geometry.indexCount>indices||std::uint64_t(vertices)*stride>vertexBytes||
       std::uint64_t(indices)*2>indexBytes)return false;
    if(sourceReceipt){
        const auto* lease=Source::borrowScope_;
        // Candidate geometry matching does not require a CPU-readable source.
        // Additional source-only predicates leave the optional receipt empty,
        // never turn a correctly bound candidate into an unsupported object.
        const auto flags=read<std::uint8_t>(v,0xb);
        const auto cpu=read<std::uintptr_t>(v,0x20);
        const auto vertexAllocation=read<std::uintptr_t>(vb,0x18),indexAllocation=read<std::uintptr_t>(ib,0x18);
        if(lease&&lease->qualified_&&lease->owner_&&lease->objectInspected_&&lease->generation_&&
           lease->frame_&&lease->owner_->frame_==lease->frame_&&lease->owner_->epoch_==lease->epoch_&&
           actual.context&&actual.command&&actual.matrices.valid&&
           actual.matrices.context==actual.context&&actual.matrices.command==actual.command&&
           !read<std::uint8_t>(v,0xa)&&((flags==1&&!cpu)||(flags==8&&cpu))&&
           !read<std::uint32_t>(i,0x20)&&read<std::uintptr_t>(i,0x10)&&vertexAllocation&&indexAllocation){
            const auto fvf=read<std::array<std::byte,16>>(f);
            sourceReceipt->meshIdentity_={{v,vb,vertexAllocation,f,actual.vertexBuffer,fvf,vertexBytes,vertices,stride},
                i,ib,indexAllocation,actual.indexBuffer,indexBytes,geometry.indexCount};
            sourceReceipt->owner_=lease->owner_;sourceReceipt->scope_=lease;sourceReceipt->scopeGeneration_=lease->generation_;
            sourceReceipt->inspectionGeneration_=lease->inspectionGeneration_;sourceReceipt->frame_=lease->frame_;sourceReceipt->context_=actual.context;sourceReceipt->command_=actual.command;
            sourceReceipt->positionBytes_=positionBytes;sourceReceipt->stride_=stride;sourceReceipt->valid_=true;
        }
    }
    return true;
}
bool NativeOcclusionDrawWitness::replayQuiescent(const Api& a,const void* live,const Source::Object& fresh,
                                                bool qualified)noexcept {
    Errno preserve;const auto info=reinterpret_cast<std::uintptr_t>(live);
    if(!qualified||!fresh.valid||!a.completeCodeProof||!a.drawableDraw||!fits(info,0xb0)||
       read<std::uintptr_t>(info,0x88)!=fresh.node||read<std::uintptr_t>(info,0x80)!=fresh.drawable||
       read<std::uintptr_t>(info,0x90)||read<std::uintptr_t>(info,0x98))return false;
    const auto flags=read<std::uint32_t>(info,0xac);
    const unsigned lod=(flags>>10)&7u;
    if((flags&0x1e000u)||lod>=4||!fits(fresh.drawable,0x70))return false;
    const auto table=read<std::uintptr_t>(fresh.drawable);
    if(!fits(table,0x38)||read<std::uintptr_t>(table,0x30)!=a.drawableDraw)return false;
    // Exact DrawCommon -> rmcDrawable::Draw -> rmcLodGroup::DrawSingle chain.
    const auto models=read<std::uintptr_t>(fresh.drawable,0x50+lod*8);
    if(!fits(models,0x10))return false;
    const unsigned modelCount=read<std::uint16_t>(models,8);
    const auto modelArray=read<std::uintptr_t>(models);
    if(!modelCount||modelCount>MaxReplayModels||!fits(modelArray,modelCount*8u))return false;
    unsigned geometries{};
    for(unsigned i=0;i<modelCount;++i){
        const auto model=read<std::uintptr_t>(modelArray,i*8u);
        if(!model)continue; // DrawSingle explicitly skips null model entries.
        if(!fits(model,0x30))return false;
        const unsigned count=read<std::uint16_t>(model,0x2e);
        const auto array=read<std::uintptr_t>(model,8);
        if(count>MaxReplayGeometries-geometries||(!count?false:!fits(array,count*8u)))return false;
        geometries+=count;
        for(unsigned j=0;j<count;++j){
            Geometry current{};
            if(!geometry(a,reinterpret_cast<const void*>(read<std::uintptr_t>(array,j*8u)),true,current))return false;
        }
        if(read<std::uint16_t>(model,0x2e)!=count||read<std::uintptr_t>(model,8)!=array)return false;
    }
    return geometries&&read<std::uintptr_t>(info,0x80)==fresh.drawable&&
        read<std::uint32_t>(info,0xac)==flags&&read<std::uintptr_t>(fresh.drawable,0x50+lod*8)==models&&
        read<std::uint16_t>(models,8)==modelCount&&read<std::uintptr_t>(models)==modelArray;
}
bool NativeOcclusionDrawWitness::object(const Api& a,std::uint64_t frame,const void* live,bool qualified,ObjectBorrow& out)noexcept {
    Errno preserve;out={};const auto info=reinterpret_cast<std::uintptr_t>(live);
    if(!qualified||!frame||!a.completeCodeProof||!a.staticVtable||!a.drawableDraw||!fits(info,0xb0))return false;
    if(read<std::uintptr_t>(info,0x90)||read<std::uintptr_t>(info,0x98)||(read<std::uint32_t>(info,0xac)&0x1e000u))return false;
    const auto node=read<std::uintptr_t>(info,0x88),drawable=read<std::uintptr_t>(info,0x80);
    if(!fits(node,8)||read<std::uintptr_t>(node)!=a.staticVtable||!fits(node,0xf8)||!fits(drawable,8))return false;
    if((read<std::uint32_t>(node,0x2c)&0x100400u)||
       (drawable!=read<std::uintptr_t>(node,0xe8)&&drawable!=read<std::uintptr_t>(node,0xf0)))return false;
    const auto table=read<std::uintptr_t>(drawable);
    if(!fits(table,0x38)||read<std::uintptr_t>(table,0x30)!=a.drawableDraw)return false;
    Source::Object value{};value.frame=frame;value.node=node;value.drawable=drawable;value.world[15]=1;
    for(unsigned col=0;col<4;++col)for(unsigned row=0;row<3;++row){
        const auto offset=col*16+row*4;const auto v=read<float>(info,offset);
        if(!objectEqual(v,read<float>(info,0x40+offset))||!objectEqual(v,read<float>(node,0x60+offset)))return false;
        value.world[row*4+col]=v;
    }
    std::array<double,3> lo{},hi{};
    for(unsigned i=0;i<3;++i){lo[i]=read<float>(node,0xa0+4*i);hi[i]=read<float>(node,0xb0+4*i);
        if(!std::isfinite(lo[i])||!std::isfinite(hi[i])||lo[i]>hi[i])return false;}
    value.bounds={{lo[0],lo[1],lo[2]},{hi[0],hi[1],hi[2]}};
    // Object.valid deliberately stays false until the owner finalizes its
    // conservative numeric error. ObjectBorrow.valid proves only copied ABI.
    out.object=value;out.valid=true;return true;
}
}
