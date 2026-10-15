#include "NativeStaticTriangles.h"
#include "OwnedStaticVertices.h"
#include "OwnedStaticOccluders.h"
#include "NativeGeometryBindingReceipt.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>

namespace rdrvr::engine {
namespace {
template<class T,std::size_t N>T at(const std::array<std::byte,N>& bytes,std::size_t off) noexcept {
    T value{};std::memcpy(&value,bytes.data()+off,sizeof(value));return value;
}
bool fits(std::uintptr_t p,std::size_t n) noexcept {return p&&n&&p<=UINTPTR_MAX-n;}
bool affine(const ConservativeStereoRaster::Matrix& m) noexcept {
    for(double v:m)if(!std::isfinite(v))return false;
    return m[12]==0&&m[13]==0&&m[14]==0&&m[15]==1;
}
}
void NativeStaticTriangles::configure(Api api) noexcept {
    end();api_=api;ready_=api.read&&api.completeCodeProof&&api.vertexVtable&&api.indexVtable&&api.bufferVtable&&
        api.vertexVtable!=api.indexVtable&&api.vertexVtable!=api.bufferVtable&&api.indexVtable!=api.bufferVtable;
}
bool NativeStaticTriangles::begin(std::uint64_t frame,Budget budget) noexcept {
    end();stats_={};budget_=budget;
    if(!ready_||!frame||!budget.draws||!budget.triangles||!budget.bytes)return false;
    frame_=frame;meshSerial_=api_.ownedOccluders?api_.ownedOccluders->invalidationSerial():0;return true;
}
void NativeStaticTriangles::clear(Snapshot& out) noexcept {
    out.count=out.patchCount=out.sourceTriangleCount=0;out.completeDraw=false;
    out.frame=0;out.worldError={};out.vertexHandle=out.indexHandle=0;
}
void NativeStaticTriangles::end() noexcept {frame_=meshSerial_=0;clear(snapshot_);}
bool NativeStaticTriangles::canCapture(std::uint64_t frame)const noexcept {
    return ready_&&frame&&frame==frame_&&stats_.calls<budget_.draws&&
        stats_.copiedTriangles+stats_.copiedPatches<budget_.triangles&&stats_.bytes<budget_.bytes;
}
bool NativeStaticTriangles::canCapturePacket(std::uint64_t frame)const noexcept {
    return ready_&&frame&&frame==frame_&&api_.ownedOccluders&&api_.ownedEpoch&&
        stats_.packetCalls<budget_.packetDraws&&stats_.bytes<budget_.bytes;
}
bool NativeStaticTriangles::worldErrorBound(const ConservativeStereoRaster::Matrix& m,
        const ConservativeStereoRaster::Vec3& localAbs,ConservativeStereoRaster::Vec3& out) noexcept {
    out={};if(!affine(m))return false;
    const std::array<double,4> a{localAbs.x,localAbs.y,localAbs.z,1};
    for(double x:a)if(!std::isfinite(x)||x<0)return false;
    for(double x:m)if(!std::isfinite(x)||double(static_cast<float>(x))!=x)return false;
    // A full ULP covers directed/truncating shader arithmetic as well as nearest.
    constexpr double u=0x1p-23,gamma=(8*u)/(1-8*u);
    constexpr double tiny=std::numeric_limits<float>::min();
    std::array<double,3> errors{};
    for(unsigned row=0;row<3;++row){
        double magnitude=0,flush=8*tiny;
        for(unsigned k=0;k<4;++k){const double c=std::abs(m[row*4+k]);
            magnitude+=c*a[k];
            if(c<tiny)flush+=c*a[k]; // A denormal matrix input can be flushed.
            if(k<3)flush+=c*std::min(a[k],tiny); // Some local coordinate may be denormal.
        }
        if(!std::isfinite(magnitude)||magnitude>double(std::numeric_limits<float>::max())/(1+gamma))return false;
        // Double accumulation is rounded outward well beyond its own four-add
        // error; it is tiny compared with the explicit float32 gamma bound.
        errors[row]=std::nextafter((gamma*magnitude+flush)*(1+32*std::numeric_limits<double>::epsilon()),
            std::numeric_limits<double>::infinity());
    }
    out={errors[0],errors[1],errors[2]};return true;
}
bool NativeStaticTriangles::read(std::uintptr_t p,void* out,std::size_t n) noexcept {
    if(!fits(p,n)){++stats_.badRange;return false;}
    if(n>budget_.bytes-stats_.bytes){++stats_.budgetStops;return false;}
    stats_.bytes+=n;++stats_.reads;
    bool ok=false;const int saved=errno;
    try {ok=api_.read(api_.context,p,out,n);}catch(...){ok=false;}
    errno=saved;if(!ok)++stats_.readFailures;return ok;
}
bool NativeStaticTriangles::capturePacket(std::uint64_t frame,const Draw& draw,Packet& result) noexcept {
    result.reset();
    if(!ready_||!frame||frame!=frame_){++stats_.stale;return false;}
    if(!canCapturePacket(frame)){++stats_.budgetStops;return false;}
    ++stats_.packetCalls;
    using Faces=ConservativeStereoRaster::Faces;
    if(!draw.qualified||!affine(draw.world)||draw.position!=Position::CertifiedXYZ1||
       (draw.faces!=Faces::Both&&draw.faces!=Faces::PositiveArea&&draw.faces!=Faces::NegativeArea)) {
        ++stats_.unqualified;return false;
    }
    std::array<std::byte,0x40> vb{},vbAfter{};
    std::array<std::byte,0x28> ib{},ibAfter{},gpuV{},gpuI{},gpuVAfter{},gpuIAfter{};
    std::array<std::byte,16> fvf{},fvfAfter{};
    const auto v=reinterpret_cast<std::uintptr_t>(draw.vertexBuffer);
    const auto i=reinterpret_cast<std::uintptr_t>(draw.indexBuffer);
    if(!read(v,vb.data(),vb.size())||!read(i,ib.data(),ib.size()))return false;
    if(at<std::uintptr_t>(vb,0)!=api_.vertexVtable||at<std::uintptr_t>(ib,0)!=api_.indexVtable) {
        ++stats_.badOwner;return false;
    }
    const auto flags=at<std::uint8_t>(vb,0xb);
    const auto vp=at<std::uintptr_t>(vb,0x20),ip=at<std::uintptr_t>(ib,0x10);
    const auto vgpu=at<std::uintptr_t>(vb,0x38),igpu=at<std::uintptr_t>(ib,0x18);
    const bool owned=flags==1&&!vp;
    if((!owned&&(flags!=8||!vp))||at<std::uint8_t>(vb,0xa)||at<std::uint32_t>(ib,0x20)||!ip) {
        ++stats_.unmapped;return false;
    }
    const auto f=at<std::uintptr_t>(vb,0x30);
    if(!read(vgpu,gpuV.data(),gpuV.size())||!read(igpu,gpuI.data(),gpuI.size())||!read(f,fvf.data(),fvf.size()))return false;
    if(at<std::uintptr_t>(gpuV,0)!=api_.bufferVtable||at<std::uintptr_t>(gpuI,0)!=api_.bufferVtable||
       !at<std::uintptr_t>(gpuV,0x10)||!at<std::uintptr_t>(gpuV,0x18)||
       !at<std::uintptr_t>(gpuI,0x10)||!at<std::uintptr_t>(gpuI,0x18)||
       at<std::uint32_t>(gpuV,0x24)!=0||at<std::uint32_t>(gpuI,0x24)!=1) {
        ++stats_.badOwner;return false;
    }
    const auto vertexCount=at<std::uint16_t>(vb,8);
    const auto stride=at<std::uint32_t>(vb,0x18);
    const auto format=at<std::uint64_t>(fvf,8)&15;
    if(!(at<std::uint32_t>(fvf,0)&1)||(format!=6&&format!=7)||stride<((format==7)?16u:12u)||
       stride>256||at<std::uint8_t>(fvf,4)!=stride||
       (draw.expectedPositionBytes&&draw.expectedPositionBytes!=((format==7)?16u:12u))||
       (draw.expectedStride&&draw.expectedStride!=stride)) {++stats_.unsupportedFormat;return false;}
    const auto indexCount=at<std::uint32_t>(ib,8);
    if(!vertexCount||!draw.indexCount||draw.indexCount%3||draw.indexCount>indexCount||
       std::uint64_t(vertexCount)*stride>at<std::uint32_t>(gpuV,0x20)||
       std::uint64_t(indexCount)*2>at<std::uint32_t>(gpuI,0x20)) {++stats_.badRange;return false;}
    const OwnedStaticVertices::Identity identity{v,vgpu,at<std::uintptr_t>(gpuV,0x18),f,
        at<std::uint64_t>(gpuV,0x10),fvf,at<std::uint32_t>(gpuV,0x20),vertexCount,stride};
    const OwnedStaticOccluders::Identity meshIdentity{identity,i,igpu,at<std::uintptr_t>(gpuI,0x18),
        at<std::uint64_t>(gpuI,0x10),at<std::uint32_t>(gpuI,0x20),draw.indexCount};
    OwnedStaticOccluders::Snapshot mesh;++stats_.meshQueries;
    if(!api_.ownedOccluders->querySnapshot(api_.ownedEpoch,meshIdentity,mesh)||mesh.positions.size()!=vertexCount||mesh.nodes.empty()) {
        ++stats_.meshMisses;return false;
    }
    ++stats_.meshHits;
    // Full cold root bounds cover every referenced vertex of every patch. No
    // per-patch transform or per-corner copying happens on the draw hot path.
    const auto& root=mesh.nodes.front();
    const std::array<double,3> lo{root.minimum.x,root.minimum.y,root.minimum.z};
    const std::array<double,3> hi{root.maximum.x,root.maximum.y,root.maximum.z};
    std::array<double,3> magnitude{};
    for(unsigned k=0;k<3;++k){if(!std::isfinite(lo[k])||!std::isfinite(hi[k])||lo[k]>hi[k]){++stats_.badData;return false;}
        magnitude[k]=std::max(std::abs(lo[k]),std::abs(hi[k]));}
    ConservativeStereoRaster::Vec3 error{};
    if(!worldErrorBound(draw.world,{magnitude[0],magnitude[1],magnitude[2]},error)){++stats_.badData;return false;}
    // All native owners are still borrowed here. Snapshot owns copied memory,
    // but it does not excuse a replaced current backing/format or writer.
    if(!read(v,vbAfter.data(),vbAfter.size())||!read(i,ibAfter.data(),ibAfter.size())||
       !read(vgpu,gpuVAfter.data(),gpuVAfter.size())||!read(igpu,gpuIAfter.data(),gpuIAfter.size())||
       !read(f,fvfAfter.data(),fvfAfter.size()))return false;
    if(vb!=vbAfter||ib!=ibAfter||gpuV!=gpuVAfter||gpuI!=gpuIAfter||fvf!=fvfAfter||!api_.ownedOccluders->current(mesh)) {
        ++stats_.changedOwner;return false;
    }
    result.mesh=std::move(mesh);result.world=draw.world;result.worldError=error;result.faces=draw.faces;
    result.frame=frame;result.vertexHandle=identity.gpuBuffer;result.indexHandle=meshIdentity.indexGpuBuffer;
    ++stats_.packets;stats_.packetPatches+=result.mesh.patches.size();return true;
}
bool NativeStaticTriangles::capturePacketVerified(std::uint64_t frame,const Draw& draw,
        const NativeGeometryBindingReceipt& receipt,Packet& result) noexcept {
    result.reset();
    if(!ready_||!frame||frame!=frame_){++stats_.stale;return false;}
    if(!canCapturePacket(frame)){++stats_.budgetStops;return false;}
    ++stats_.packetCalls;
    using Faces=ConservativeStereoRaster::Faces;const auto& id=receipt.meshIdentity_;
    if(!receipt.valid_||receipt.frame_!=frame||!draw.qualified||!affine(draw.world)||
       draw.position!=Position::CertifiedXYZ1||reinterpret_cast<std::uintptr_t>(draw.vertexBuffer)!=id.vertices.vertexBuffer||
       reinterpret_cast<std::uintptr_t>(draw.indexBuffer)!=id.indexBuffer||draw.indexCount!=id.indexCount||
       draw.expectedPositionBytes!=receipt.positionBytes_||draw.expectedStride!=receipt.stride_||
       (draw.faces!=Faces::Both&&draw.faces!=Faces::PositiveArea&&draw.faces!=Faces::NegativeArea)){
        ++stats_.unqualified;return false;
    }
    // Preserve copied exact binding evidence now; most sources never win the
    // global selection. Their cache lookup and numerical root work are avoided.
    if(api_.ownedOccluders->invalidationSerial()!=meshSerial_){++stats_.changedOwner;return false;}
    result.identity_=id;result.cache_=api_.ownedOccluders;result.epoch_=api_.ownedEpoch;result.serial_=meshSerial_;result.pending_=true;
    result.world=draw.world;result.faces=draw.faces;result.frame=frame;
    result.vertexHandle=id.vertices.gpuBuffer;result.indexHandle=id.indexGpuBuffer;
    ++stats_.packets;++stats_.deferredPackets;return true;
}

bool NativeStaticTriangles::captureQualifiedPacket(std::uint64_t frame,const ConservativeStereoRaster::Matrix& world,
        ConservativeStereoRaster::Faces faces,const NativeGeometryBindingReceipt& receipt,Packet& result)noexcept {
    result.reset();using Faces=ConservativeStereoRaster::Faces;
    if(!ready_||!frame||frame!=frame_){++stats_.stale;return false;}
    if(!canCapturePacket(frame)){++stats_.budgetStops;return false;}++stats_.packetCalls;
    if(!receipt.valid_||!receipt.transformQualified_||receipt.frame_!=frame||
       (faces!=Faces::Both&&faces!=Faces::PositiveArea&&faces!=Faces::NegativeArea)){++stats_.unqualified;return false;}
    if(api_.ownedOccluders->invalidationSerial()!=meshSerial_){++stats_.changedOwner;return false;}
    const auto& id=receipt.meshIdentity_;result.identity_=id;result.cache_=api_.ownedOccluders;
    result.epoch_=api_.ownedEpoch;result.serial_=meshSerial_;result.pending_=true;
    result.world=world;result.faces=faces;result.frame=frame;
    result.vertexHandle=id.vertices.gpuBuffer;result.indexHandle=id.indexGpuBuffer;
    ++stats_.packets;++stats_.deferredPackets;return true;
}
bool NativeStaticTriangles::packetCurrent(std::uint64_t frame,const Packet& packet,const OwnedStaticOccluders& cache)noexcept {
    if(!frame||packet.frame!=frame||!packet)return false;
    if(packet.pending_)return packet.cache_==&cache&&packet.epoch_&&cache.invalidationSerial()==packet.serial_;
    return cache.current(packet.mesh);
}
bool NativeStaticTriangles::resolvePacket(std::uint64_t frame,Packet& packet,const OwnedStaticOccluders& cache)noexcept {
    if(!packetCurrent(frame,packet,cache)){packet.reset();return false;}
    if(!packet.pending_)return true;
    const auto& id=packet.identity_;OwnedStaticOccluders::Snapshot mesh;
    if(!cache.querySnapshot(packet.epoch_,id,mesh)||mesh.positions.size()!=id.vertices.vertexCount||mesh.nodes.empty()){
        packet.reset();return false;
    }
    const auto& root=mesh.nodes.front();
    const std::array<double,3> lo{root.minimum.x,root.minimum.y,root.minimum.z};
    const std::array<double,3> hi{root.maximum.x,root.maximum.y,root.maximum.z};std::array<double,3> absolute{};
    for(unsigned k=0;k<3;++k){if(!std::isfinite(lo[k])||!std::isfinite(hi[k])||lo[k]>hi[k]){packet.reset();return false;}
        absolute[k]=std::max(std::abs(lo[k]),std::abs(hi[k]));}
    ConservativeStereoRaster::Vec3 error{};
    if(!worldErrorBound(packet.world,{absolute[0],absolute[1],absolute[2]},error)||
       !cache.current(mesh)||cache.invalidationSerial()!=packet.serial_){packet.reset();return false;}
    packet.mesh=std::move(mesh);packet.worldError=error;packet.pending_=false;return true;
}
const NativeStaticTriangles::Snapshot& NativeStaticTriangles::capture(std::uint64_t frame,const Draw& draw) noexcept {
    return captureInto(frame,draw,snapshot_);
}
const NativeStaticTriangles::Snapshot& NativeStaticTriangles::captureInto(std::uint64_t frame,const Draw& draw,Snapshot& result_) noexcept {
    clear(result_);
    if(!ready_||!frame||frame!=frame_){++stats_.stale;return result_;}
    if(!canCapture(frame)){++stats_.budgetStops;return result_;}
    ++stats_.calls;
    using Faces=ConservativeStereoRaster::Faces;
    if(!draw.qualified||!affine(draw.world)||
       (draw.position!=Position::Homogeneous&&draw.position!=Position::CertifiedXYZ1)||
       (draw.faces!=Faces::Both&&draw.faces!=Faces::PositiveArea&&draw.faces!=Faces::NegativeArea)) {
        ++stats_.unqualified;return result_;
    }
    ++stats_.qualified;
    std::array<std::byte,0x40> vb{},vbAfter{};
    std::array<std::byte,0x28> ib{},ibAfter{},gpuV{},gpuI{},gpuVAfter{},gpuIAfter{};
    std::array<std::byte,16> fvf{},fvfAfter{};
    const auto v= reinterpret_cast<std::uintptr_t>(draw.vertexBuffer);
    const auto i= reinterpret_cast<std::uintptr_t>(draw.indexBuffer);
    if(!read(v,vb.data(),vb.size())||!read(i,ib.data(),ib.size()))return result_;
    if(at<std::uintptr_t>(vb,0)!=api_.vertexVtable||at<std::uintptr_t>(ib,0)!=api_.indexVtable) {
        ++stats_.badOwner;return result_;
    }
    const auto flags=at<std::uint8_t>(vb,0xb);
    const auto vp=at<std::uintptr_t>(vb,0x20),ip=at<std::uintptr_t>(ib,0x10);
    const auto vgpu=at<std::uintptr_t>(vb,0x38),igpu=at<std::uintptr_t>(ib,0x18);
    // bit3 records the lifetime-held map installed by ManualCreate; bit0/1
    // selects native dynamic/external buffers. A current lock may be a writer.
    const bool owned=flags==1&&!vp&&draw.position==Position::CertifiedXYZ1&&
        (api_.ownedVertices||api_.ownedOccluders)&&api_.ownedEpoch;
    if((!owned&&(flags!=8||!vp))||at<std::uint8_t>(vb,0xa)||at<std::uint32_t>(ib,0x20)||!ip) {
        ++stats_.unmapped;return result_;
    }
    const auto f=at<std::uintptr_t>(vb,0x30);
    if(!read(vgpu,gpuV.data(),gpuV.size())||!read(igpu,gpuI.data(),gpuI.size())||!read(f,fvf.data(),fvf.size()))return result_;
    // Actual VkBuffer type is +24 (0 vertex / 1 index), NOT an immutability
    // flag. The byte-size is +20. Type alone never certifies a static lifetime.
    if(at<std::uintptr_t>(gpuV,0)!=api_.bufferVtable||at<std::uintptr_t>(gpuI,0)!=api_.bufferVtable||
       !at<std::uintptr_t>(gpuV,0x10)||!at<std::uintptr_t>(gpuV,0x18)||
       !at<std::uintptr_t>(gpuI,0x10)||!at<std::uintptr_t>(gpuI,0x18)||
       at<std::uint32_t>(gpuV,0x24)!=0||at<std::uint32_t>(gpuI,0x24)!=1) {
        ++stats_.badOwner;return result_;
    }
    const auto vertexCount=at<std::uint16_t>(vb,8);
    const auto stride=at<std::uint32_t>(vb,0x18);
    const auto format=at<std::uint64_t>(fvf,8)&15;
    // Position channel zero is FIRST in both pinned FVF channel orders.
    // Only exact float3 / homogeneous float4 are supported. No guessed packed
    // decode, skin weights, secondary streams, or inferred half-float semantics.
    if(!(at<std::uint32_t>(fvf,0)&1)||(format!=6&&format!=7)||stride<((format==7)?16u:12u)||
       stride>256||at<std::uint8_t>(fvf,4)!=stride||
       (draw.expectedPositionBytes&&draw.expectedPositionBytes!=((format==7)?16u:12u))||
       (draw.expectedStride&&draw.expectedStride!=stride)) {++stats_.unsupportedFormat;return result_;}
    const auto indexCount=at<std::uint32_t>(ib,8);
    if(!vertexCount||!draw.indexCount||draw.indexCount%3||draw.indexCount>indexCount||
       std::uint64_t(vertexCount)*stride>at<std::uint32_t>(gpuV,0x20)||
       std::uint64_t(indexCount)*2>at<std::uint32_t>(gpuI,0x20)) {++stats_.badRange;return result_;}
    const OwnedStaticVertices::Identity identity{v,vgpu,at<std::uintptr_t>(gpuV,0x18),f,
        at<std::uint64_t>(gpuV,0x10),fvf,at<std::uint32_t>(gpuV,0x20),vertexCount,stride};
    auto headersMatch=[&]()noexcept {
        if(!read(v,vbAfter.data(),vbAfter.size())||!read(i,ibAfter.data(),ibAfter.size())||
           !read(vgpu,gpuVAfter.data(),gpuVAfter.size())||!read(igpu,gpuIAfter.data(),gpuIAfter.size())||
           !read(f,fvfAfter.data(),fvfAfter.size()))return false;
        if(vb!=vbAfter||ib!=ibAfter||gpuV!=gpuVAfter||gpuI!=gpuIAfter||fvf!=fvfAfter){++stats_.changedOwner;return false;}
        return true;
    };
    const auto remaining=budget_.triangles-stats_.copiedTriangles-stats_.copiedPatches;
    // A cold mesh is the exact submitted index range, not a hull or a prefix
    // borrowed from another draw using the same backing. It removes shared
    // interior edges and repeated per-frame index/position decoding.
    if(draw.position==Position::CertifiedXYZ1&&api_.ownedOccluders&&api_.ownedEpoch){
        OwnedStaticOccluders::Identity meshIdentity{identity,i,igpu,at<std::uintptr_t>(gpuI,0x18),
            at<std::uint64_t>(gpuI,0x10),at<std::uint32_t>(gpuI,0x20),draw.indexCount};
        OwnedStaticOccluders::Lease mesh;++stats_.meshQueries;
        const bool found=api_.ownedOccluders->query(api_.ownedEpoch,meshIdentity,mesh)&&mesh.positions.size()==vertexCount;
        // Cold patches preserve an oriented disk and its outer boundary, never
        // a convex hull. Identical XYZ endpoints use the same certified shader
        // and actual matrices in this draw. The raster's full convex error
        // envelope bounds the boundary. Winding degree therefore guarantees a
        // same-facing covering triangle even if an interior triangle flips;
        // keep the real face selection instead of treating it double-sided.
        if(found){
            ++stats_.meshHits;
            const auto count=std::min<std::uint64_t>({mesh.patches.size(),MaxTriangles,remaining});
            if(!count){++stats_.budgetStops;return result_;}
            ConservativeStereoRaster::Vec3 localAbs{};
            std::uint64_t bytes=0,represented=0;
            for(unsigned k=0;k<count;++k){bytes+=std::uint64_t(mesh.patches[k].count)*sizeof(OwnedStaticVertices::XYZ);
                represented+=mesh.patches[k].sourceTriangleCount;}
            if(represented>draw.indexCount/3){++stats_.badData;return result_;}
            if(bytes>budget_.bytes-stats_.bytes){++stats_.budgetStops;return result_;}
            stats_.bytes+=bytes;stats_.ownedBytes+=bytes;
            // Sparse triangle soups would pay a memo lookup/store without
            // reusing any vertex. Only use the direct-index memo when all
            // indices fit collision-free and at least half of the selected
            // corner references necessarily repeat a vertex.
            const bool reuseVertices=vertexCount<=transformed_.size()&&
                bytes/sizeof(OwnedStaticVertices::XYZ)>=std::uint64_t(vertexCount)*2;
            // Indices share one immutable cold lease and one exact world
            // matrix for this capture only. Tags expire on EVERY draw; neither
            // a pointer identity nor a previous-frame transform is authority.
            if(reuseVertices&&++transformEpoch_==0){for(auto& entry:transformed_)entry.epoch=0;++transformEpoch_;}
            for(unsigned k=0;k<count;++k){
                const auto& patch=mesh.patches[k];auto& out=result_.patches[k];
                if(patch.count<3||patch.count>out.vertex.size()||patch.firstIndex>mesh.patchIndices.size()||
                   patch.count>mesh.patchIndices.size()-patch.firstIndex){++stats_.badData;return result_;}
                out.count=patch.count;out.faces=draw.faces;
                for(unsigned corner=0;corner<patch.count;++corner){
                    const auto index=mesh.patchIndices[patch.firstIndex+corner];if(index>=mesh.positions.size()){++stats_.badData;return result_;}
                    auto& cached=transformed_[index&(transformed_.size()-1)];
                    if(reuseVertices&&cached.epoch==transformEpoch_&&cached.index==index){
                        out.vertex[corner]=cached.value;++stats_.reusedVertices;continue;
                    }
                    const auto& p=mesh.positions[index];const auto& m=draw.world;
                    if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z)){++stats_.badData;return result_;}
                    localAbs.x=std::max(localAbs.x,std::abs(double(p.x)));
                    localAbs.y=std::max(localAbs.y,std::abs(double(p.y)));
                    localAbs.z=std::max(localAbs.z,std::abs(double(p.z)));
                    auto& q=out.vertex[corner];q={m[0]*p.x+m[1]*p.y+m[2]*p.z+m[3],
                        m[4]*p.x+m[5]*p.y+m[6]*p.z+m[7],m[8]*p.x+m[9]*p.y+m[10]*p.z+m[11]};
                    if(!std::isfinite(q.x)||!std::isfinite(q.y)||!std::isfinite(q.z)){++stats_.badData;return result_;}
                    if(reuseVertices)cached={q,transformEpoch_,index};++stats_.transformedVertices;
                }
            }
            if(!headersMatch())return result_;
            if(!api_.ownedOccluders->current(mesh)){++stats_.changedOwner;return result_;}
            if(!worldErrorBound(draw.world,localAbs,result_.worldError)){++stats_.badData;return result_;}
            result_.patchCount=static_cast<std::uint32_t>(count);result_.completeDraw=count==mesh.patches.size();
            result_.sourceTriangleCount=static_cast<std::uint32_t>(represented);
            result_.frame=frame;result_.vertexHandle=identity.gpuBuffer;result_.indexHandle=meshIdentity.indexGpuBuffer;
            stats_.copiedPatches+=count;stats_.sourceTriangles+=result_.sourceTriangleCount;return result_;
        }
        ++stats_.meshMisses;
        // A failed query can mean an active index writer, invalidation or a
        // busy ownership lock. Raw IB fallback would bypass that lifetime
        // proof. Legacy mapping fallback is only used without the mesh feature.
        return result_;
    }
    OwnedStaticVertices::Lease lease;
    if(owned){
        ++stats_.ownedQueries;
        if(!api_.ownedVertices||!api_.ownedVertices->query(api_.ownedEpoch,identity,lease)||lease.positions.size()!=vertexCount){
            ++stats_.ownedMisses;return result_;
        }
        ++stats_.ownedHits;
    }
    const auto count=std::min<std::uint64_t>({draw.indexCount/3,MaxTriangles,remaining});
    if(!count){++stats_.budgetStops;return result_;}
    const auto n=static_cast<std::uint32_t>(count*3);
    if(!read(ip,indices_.data(),n*2))return result_;
    std::uint16_t lo=UINT16_MAX,hi=0;
    for(unsigned k=0;k<n;++k) {
        if(indices_[k]>=vertexCount){++stats_.badRange;return result_;}
        lo=std::min(lo,indices_[k]);hi=std::max(hi,indices_[k]);
    }
    const auto offset=std::size_t(lo)*stride,bytes=std::size_t(hi-lo+1)*stride;
    if(owned){
        const auto copied=std::uint64_t(n)*sizeof(OwnedStaticVertices::XYZ);
        if(copied>budget_.bytes-stats_.bytes){++stats_.budgetStops;return result_;}
        stats_.bytes+=copied;stats_.ownedBytes+=copied;
    }else{
        if(bytes>vertices_.size()||vp>UINTPTR_MAX-offset) {++stats_.badRange;return result_;}
        if(!read(vp+offset,vertices_.data(),bytes))return result_;
    }
    // Consistent identity/shape before and after all copies. This catches a
    // replaced backing; the caller's synchronous native lease still matters.
    if(!headersMatch())return result_;
    ConservativeStereoRaster::Vec3 localAbs{};
    for(unsigned tri=0;tri<count;++tri) {
        auto& out=result_.triangles[tri];out.faces=draw.faces;
        for(unsigned corner=0;corner<3;++corner) {
            std::array<float,4> p{0,0,0,1};
            if(owned){
                const auto& xyz=lease.positions[indices_[tri*3+corner]];p={xyz.x,xyz.y,xyz.z,1};
            }else{
                const auto atVertex=std::size_t(indices_[tri*3+corner]-lo)*stride;
                std::memcpy(p.data(),vertices_.data()+atVertex,format==7?16:12);
            }
            if(!std::isfinite(p[0])||!std::isfinite(p[1])||!std::isfinite(p[2])||
               (draw.position!=Position::CertifiedXYZ1&&p[3]!=1)) {
                ++stats_.badData;return result_;
            }
            const auto& m=draw.world;
            localAbs.x=std::max(localAbs.x,std::abs(double(p[0])));
            localAbs.y=std::max(localAbs.y,std::abs(double(p[1])));
            localAbs.z=std::max(localAbs.z,std::abs(double(p[2])));
            out.vertex[corner]={m[0]*p[0]+m[1]*p[1]+m[2]*p[2]+m[3],
                m[4]*p[0]+m[5]*p[1]+m[6]*p[2]+m[7],m[8]*p[0]+m[9]*p[1]+m[10]*p[2]+m[11]};
            const auto& q=out.vertex[corner];
            if(!std::isfinite(q.x)||!std::isfinite(q.y)||!std::isfinite(q.z)) {++stats_.badData;return result_;}
        }
    }
    if(!worldErrorBound(draw.world,localAbs,result_.worldError)){++stats_.badData;return result_;}
    if(owned&&!api_.ownedVertices->current(lease)){++stats_.changedOwner;return result_;}
    result_.count=static_cast<std::uint32_t>(count);result_.frame=frame;
    result_.sourceTriangleCount=result_.count;result_.completeDraw=result_.count==draw.indexCount/3;
    result_.vertexHandle=at<std::uint64_t>(gpuV,0x10);result_.indexHandle=at<std::uint64_t>(gpuI,0x10);
    stats_.copiedTriangles+=count;stats_.sourceTriangles+=count;return result_;
}
} // namespace rdrvr::engine
