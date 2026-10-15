#include "NativeStaticOcclusionSource.h"
#include "NativeStaticMatrixWitness.h"
#include "NativeGeometryBindingReceipt.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>

namespace rdrvr::engine {
namespace {
template<class T,std::size_t N>T field(const std::array<std::byte,N>& b,std::size_t at)noexcept {
    T value{};std::memcpy(&value,b.data()+at,sizeof value);return value;
}
bool fits(std::uintptr_t p,std::size_t n)noexcept{return p&&n&&p<=UINTPTR_MAX-n;}
bool finite(const NativeStaticOcclusionSource::FloatMatrix& m)noexcept {
    for(float f:m)if(!std::isfinite(f))return false;return true;
}
bool affine(const NativeStaticOcclusionSource::FloatMatrix& m)noexcept {
    return finite(m)&&m[3]==0&&m[7]==0&&m[11]==0&&m[15]==1;
}
bool vectorSame(const ConservativeStereoRaster::Vec3& a,const ConservativeStereoRaster::Vec3& b)noexcept {
    return a.x==b.x&&a.y==b.y&&a.z==b.z;
}
bool boundError(const NativeStaticOcclusionSource::Matrix& m,const ConservativeStereoRaster::Bounds& bounds,
        ConservativeStereoRaster::Vec3& error)noexcept {
    // Invert only the affine 3x3 linear part. Rather than assume an accurate
    // inverse, bound its residual R=I-B*A and use ||R||/(1-||R||) correction.
    // Thus the local coordinate bounds enclose every point of the authored
    // world AABB; ill-conditioned/singular input declines instead of shrinking.
    double aug[3][6]{};
    for(unsigned r=0;r<3;++r){for(unsigned c=0;c<3;++c)aug[r][c]=m[r*4+c];aug[r][3+r]=1;}
    for(unsigned c=0;c<3;++c){unsigned pivot=c;
        for(unsigned r=c+1;r<3;++r)if(std::abs(aug[r][c])>std::abs(aug[pivot][c]))pivot=r;
        if(!std::isfinite(aug[pivot][c])||std::abs(aug[pivot][c])<1e-30)return false;
        for(unsigned k=0;k<6;++k)std::swap(aug[c][k],aug[pivot][k]);
        const double scale=aug[c][c];for(double& v:aug[c])v/=scale;
        for(unsigned r=0;r<3;++r)if(r!=c){const double x=aug[r][c];for(unsigned k=0;k<6;++k)aug[r][k]-=x*aug[c][k];}
    }
    constexpr double u=std::numeric_limits<double>::epsilon(),gamma=16*u/(1-16*u);
    double residual=0,normA=0,normB=0;
    for(unsigned r=0;r<3;++r){double rr=0,aa=0,bb=0;
        for(unsigned c=0;c<3;++c){double dot=0,mag=0;for(unsigned k=0;k<3;++k){const double x=aug[r][3+k]*m[k*4+c];dot+=x;mag+=std::abs(x);}
            rr+=std::abs((r==c?1.:0.)-dot)+gamma*(mag+1);
            aa+=std::abs(m[r*4+c]);bb+=std::abs(aug[r][3+c]);}
        residual=std::max(residual,rr);normA=std::max(normA,aa);normB=std::max(normB,bb);
    }
    residual=std::nextafter(residual*(1+gamma),std::numeric_limits<double>::infinity());
    if(!std::isfinite(residual)||residual>=1./64||!std::isfinite(normA)||!std::isfinite(normB)||normA*normB/(1-residual)>1e6)return false;
    const std::array<double,3> lo{bounds.minimum.x,bounds.minimum.y,bounds.minimum.z};
    const std::array<double,3> hi{bounds.maximum.x,bounds.maximum.y,bounds.maximum.z};
    std::array<double,3> span{},local{};double worldMax=0;
    for(unsigned k=0;k<3;++k){span[k]=std::max(std::abs(lo[k]-m[k*4+3]),std::abs(hi[k]-m[k*4+3]));worldMax=std::max(worldMax,span[k]);}
    const double correction=normB*residual/(1-residual)*worldMax;
    for(unsigned r=0;r<3;++r){double bound=correction;for(unsigned k=0;k<3;++k)bound+=std::abs(aug[r][3+k])*span[k];
        local[r]=std::nextafter(bound*(1+gamma),std::numeric_limits<double>::infinity());}
    return NativeStaticTriangles::worldErrorBound(m,{local[0],local[1],local[2]},error);
}
}
thread_local NativeStaticOcclusionSource::BorrowScope* NativeStaticOcclusionSource::borrowScope_{};
thread_local std::uint64_t NativeStaticOcclusionSource::borrowGeneration_{};
NativeStaticOcclusionSource::BorrowScope::BorrowScope(NativeStaticOcclusionSource& owner,std::uint64_t frame,const void* info,bool qualified)noexcept
    :owner_(&owner),previous_(borrowScope_),info_(info),frame_(frame),epoch_(owner.epoch_),
     qualified_(qualified&&!previous_&&owner.ready_&&owner.frame_==frame&&frame&&info&&owner.api_.borrowedRead){
    if(borrowGeneration_==UINT64_MAX)qualified_=false;
    else generation_=++borrowGeneration_;
    borrowScope_=this;
}
NativeStaticOcclusionSource::BorrowScope::~BorrowScope(){borrowScope_=previous_;}
void NativeStaticOcclusionSource::configure(Api api)noexcept {
    end();for(auto& entry:errors_)entry.valid=false;errorVictims_={};
    api_=api;triangles_.configure(api.triangles);
    ready_=api.read&&api.completeCodeProof&&api.staticVtable&&api.contextVtable&&api.drawableDraw&&
        api.staticVtable!=api.contextVtable&&api.triangles.bufferVtable&&triangles_.ready();
}
bool NativeStaticOcclusionSource::begin(std::uint64_t frame,Budget budget)noexcept {
    end();stats_={};budget_=budget;
    if(!ready_||!frame||!budget.objectReads||!budget.matrixReads||!budget.bytes||
       !triangles_.begin(frame,budget.triangles))return false;
    frame_=frame;return true;
}
void NativeStaticOcclusionSource::end()noexcept {
    frame_=0;result_.object={};NativeStaticTriangles::clear(result_.geometry);result_.completeDraw=false;triangles_.end();
    if(++epoch_==0)++epoch_;
}
bool NativeStaticOcclusionSource::canCapture(std::uint64_t frame)const noexcept {
    return ready_&&frame&&frame_==frame&&stats_.draws<budget_.matrixReads&&
        stats_.objects<budget_.objectReads&&triangles_.canCapture(frame);
}
bool NativeStaticOcclusionSource::canCapturePacket(std::uint64_t frame)const noexcept {
    return ready_&&frame&&frame_==frame&&stats_.packetDraws<budget_.packetMatrixReads&&
        stats_.objects<budget_.objectReads&&triangles_.canCapturePacket(frame);
}
bool NativeStaticOcclusionSource::read(std::uintptr_t p,void* out,std::size_t n)noexcept {
    return readWith(api_.read,p,out,n);
}
bool NativeStaticOcclusionSource::readObject(bool borrowed,std::uintptr_t p,void* out,std::size_t n)noexcept {
    return readWith(borrowed?api_.borrowedRead:api_.read,p,out,n,borrowed);
}
bool NativeStaticOcclusionSource::readWith(Read reader,std::uintptr_t p,void* out,std::size_t n,bool borrowed)noexcept {
    if(!fits(p,n)){++stats_.readFailures;return false;}
    if(!borrowed&&(stats_.guardedBytes>budget_.bytes||n>budget_.bytes-stats_.guardedBytes)){
        ++stats_.budgetStops;return false;
    }
    stats_.bytes+=n;++stats_.reads;const int incoming=errno;bool ok=false;
    if(borrowed){++stats_.borrowedReads;stats_.borrowedBytes+=n;}
    else stats_.guardedBytes+=n;
    try{ok=reader&&reader(api_.context,p,out,n);}catch(...){ok=false;}
    errno=incoming;if(!ok)++stats_.readFailures;return ok;
}
bool NativeStaticOcclusionSource::sameObject(const Object& a,const Object& b)noexcept {
    return a.valid&&b.valid&&a.frame==b.frame&&a.node==b.node&&a.drawable==b.drawable&&a.world==b.world&&
        vectorSame(a.bounds.minimum,b.bounds.minimum)&&vectorSame(a.bounds.maximum,b.bounds.maximum)&&vectorSame(a.worldError,b.worldError);
}
bool NativeStaticOcclusionSource::inspect(std::uint64_t frame,const void* info,Object& out)noexcept {
    out={};
    if(borrowScope_&&borrowScope_->owner_==this&&borrowScope_->info_==info){borrowScope_->objectInspected_=false;if(!++borrowScope_->inspectionGeneration_)borrowScope_->qualified_=false;}
    if(!ready_||!frame||frame_!=frame){++stats_.stale;return false;}
    if(stats_.objects>=budget_.objectReads){++stats_.budgetStops;return false;}
    ++stats_.objects;
    const auto* lease=borrowScope_;
    const bool borrowed=lease&&lease->owner_==this&&lease->qualified_&&lease->frame_==frame&&
        lease->epoch_==epoch_&&lease->info_==info;
    auto copy=[&](std::uintptr_t p,void* data,std::size_t n){return readObject(borrowed,p,data,n);};
    std::array<std::byte,0xb0> record{};std::array<std::byte,0xf8> node{};
    const auto recordAddress=reinterpret_cast<std::uintptr_t>(info);
    // In the live native lease reject creature/alternate records and foreign
    // node classes before copying their two unused matrices. Guarded fallback
    // retains a single bulk read rather than adding another system call.
    if(!fits(recordAddress,record.size())||
       !(borrowed?copy(recordAddress+0x80,record.data()+0x80,0x30):copy(recordAddress,record.data(),record.size())))return false;
    const auto instance=field<std::uintptr_t>(record,0x88);
    // Ordinary static drawable only. Creature, matrix set, fragment/cloth and
    // alternate command/transform records are not safe whole-object candidates.
    if(field<std::uintptr_t>(record,0x98))return refuseObject(ObjectRefusal::Creature);
    if(field<std::uintptr_t>(record,0x90))return refuseObject(ObjectRefusal::MatrixSet);
    if(field<std::uint32_t>(record,0xac)&0x1e000u)return refuseObject(ObjectRefusal::RecordFlags);
    // Even a valid DrawCommon can carry another node class. Its native-owned
    // vptr is readable; its body must not be treated as rdrDrawableInstance.
    std::uintptr_t nodeTable{};
    if(!copy(instance,&nodeTable,sizeof nodeTable))return false;
    if(nodeTable!=api_.staticVtable)return refuseObject(ObjectRefusal::NodeType);
    if(!copy(instance,node.data(),node.size()))return false;
    const auto drawable=field<std::uintptr_t>(record,0x80);
    if(field<std::uintptr_t>(node,0)!=api_.staticVtable)return refuseObject(ObjectRefusal::ChangedNodeType);
    if(field<std::uint32_t>(node,0x2c)&0x100400u)return refuseObject(ObjectRefusal::NodeFlags);
    const bool nodeDrawableCurrent=drawable&&(drawable==field<std::uintptr_t>(node,0xe8)||
        drawable==field<std::uintptr_t>(node,0xf0));
    // A mismatched drawable is usable only inside the pinned live
    // DrawCommon borrow, where the record supplies the emitted drawable.
    if(!drawable||(!nodeDrawableCurrent&&!borrowed))
        return refuseObject(ObjectRefusal::DrawableIdentity);
    std::uintptr_t drawableTable{},drawMethod{};
    if(!copy(drawable,&drawableTable,sizeof drawableTable)||!fits(drawableTable,0x38)||
       !copy(drawableTable+0x30,&drawMethod,sizeof drawMethod))return false;
    if(drawMethod!=api_.drawableDraw)return refuseObject(ObjectRefusal::DrawMethod);
    if(borrowed&&!copy(recordAddress,record.data(),0x80))return false;
    Object next{};next.frame=frame;next.node=instance;next.drawable=drawable;next.world[15]=1;
    // Native Matrix34 stores four padded column vectors. Info.Set copies only
    // xyz: padding bytes are unspecified and MUST NOT enter comparisons/math.
    for(unsigned column=0;column<4;++column)for(unsigned row=0;row<3;++row){
        const auto at=column*16+row*4;
        const float value=field<float>(record,at);
        if(!std::isfinite(value))return refuseObject(ObjectRefusal::NonFiniteWorld);
        if(value!=field<float>(record,0x40+at))return refuseObject(ObjectRefusal::RecordWorld);
        if(value!=field<float>(node,0x60+at))return refuseObject(ObjectRefusal::InstanceWorld);
        next.world[row*4+column]=value;
    }
    std::array<double,3> lo{},hi{};
    for(unsigned k=0;k<3;++k){lo[k]=field<float>(node,0xa0+k*4);hi[k]=field<float>(node,0xb0+k*4);
        if(!std::isfinite(lo[k])||!std::isfinite(hi[k])||lo[k]>hi[k])return refuseObject(ObjectRefusal::Bounds);}
    next.bounds={{lo[0],lo[1],lo[2]},{hi[0],hi[1],hi[2]}};
    if(!nodeDrawableCurrent){
        // The node AABB does not bound this record drawable. Infinite
        // metadata error prevents any bounds-based reject.
        // Actual source packets independently derive a finite error
        // from their current owned indexed mesh in resolvePacket.
        const double unavailable=std::numeric_limits<double>::infinity();
        next.worldError={unavailable,unavailable,unavailable};
        next.valid=true;out=next;++stats_.objectReady;
        borrowScope_->inspectedObject_=next;borrowScope_->objectInspected_=true;
        return true;
    }
    // Only the numerical inverse/error calculation is reused. Every lookup is
    // preceded by fresh native identity, flags, world and bounds validation.
    auto hash=std::uint64_t(next.node)^std::uint64_t(next.drawable>>3);
    hash^=hash>>29;hash*=0x9e3779b97f4a7c15ull;
    const auto set=unsigned(hash>>56);ErrorEntry* cached=nullptr;ErrorEntry* replacement=nullptr;
    for(unsigned way=0;way<4;++way){auto& entry=errors_[set*4+way];
        if(!entry.valid){if(!replacement)replacement=&entry;continue;}
        if(entry.node!=next.node||entry.drawable!=next.drawable)continue;
        // One slot per identity. Changed numerical inputs replace that slot;
        // they cannot leave multiple stale versions competing in the set.
        replacement=&entry;
        if(entry.world==next.world&&vectorSame(entry.bounds.minimum,next.bounds.minimum)&&
           vectorSame(entry.bounds.maximum,next.bounds.maximum))cached=&entry;
        break;
    }
    if(cached){
        next.worldError=cached->error;++stats_.errorCacheHits;
    }else{
        ++stats_.errorComputations;
        if(!boundError(next.world,next.bounds,next.worldError))return refuseObject(ObjectRefusal::NumericalError);
        if(!replacement){replacement=&errors_[set*4+(errorVictims_[set]&3)];++errorVictims_[set];}
        *replacement={next.node,next.drawable,next.world,next.bounds,next.worldError,true};
    }
    next.valid=true;
    out=next;++stats_.objectReady;
    if(borrowed){borrowScope_->inspectedObject_=next;borrowScope_->objectInspected_=true;}
    return true;
}
bool NativeStaticOcclusionSource::matrices(const Object& object,const Submitted& draw)noexcept {
    const auto context=reinterpret_cast<std::uintptr_t>(draw.context);
    std::uintptr_t table{},frame{};
    if(!read(context,&table,sizeof table)||table!=api_.contextVtable||!fits(context,0x30a8)||
       !read(context+0x30a0,&frame,sizeof frame)){++stats_.badUniforms;return false;}
    std::array<std::byte,0xb0> frameData{},frameAfter{};
    std::array<std::byte,0x30> cb{},cbAfter{};
    std::array<std::byte,0x1c> layout{};
    std::array<std::byte,0x28> backing{};
    if(!read(frame,frameData.data(),frameData.size()))return false;
    const auto first=field<std::uintptr_t>(frameData,0x60),last=field<std::uintptr_t>(frameData,0x68);
    const auto capacityEnd=field<std::uintptr_t>(frameData,0x70);
    const auto mapped=field<std::uintptr_t>(frameData,0xa0),wrapper=field<std::uintptr_t>(frameData,0x98);
    const auto cursor=field<std::uint64_t>(frameData,0xa8);
    if(!draw.command||field<std::uintptr_t>(frameData,0x38)!=draw.command||!first||last<first||capacityEnd<last||
       (last-first)%8||(last-first)/8<2||(last-first)/8>256||!mapped||!fits(first,16)) {
        ++stats_.badUniforms;return false;
    }
    std::uintptr_t constants{};
    if(!read(first+8,&constants,sizeof constants)||!read(constants,cb.data(),cb.size())||
       !read(field<std::uintptr_t>(cb,8),layout.data(),layout.size())||!read(wrapper,backing.data(),backing.size()))return false;
    const auto range=field<std::int32_t>(layout,0x18);
    const auto offset=field<std::uint64_t>(cb,0x28);
    const auto capacity=field<std::uint32_t>(backing,0x20);
    constexpr std::size_t MatrixBytes=272;
    if(range<static_cast<int>(MatrixBytes)||range>65536||field<std::uintptr_t>(backing,0)!=api_.triangles.bufferVtable||
       !field<std::uintptr_t>(backing,0x10)||!field<std::uintptr_t>(backing,0x18)||
       !capacity||cursor>capacity||offset>cursor||std::uint64_t(range)>cursor-offset||
       offset>UINTPTR_MAX-mapped||!fits(mapped+std::uintptr_t(offset),MatrixBytes)) {
        ++stats_.badUniforms;return false;
    }
    std::array<FloatMatrix,4> uploaded{};
    std::array<std::byte,MatrixBytes> raw{};
    if(!read(mapped+std::uintptr_t(offset),raw.data(),raw.size()))return false;
    std::memcpy(uploaded.data(),raw.data(),sizeof(uploaded));
    // Read the immutable uploaded bytes, never the mutable cb+0 CPU shadow.
    // World and camera matches are exact (including translations). Unknown
    // matrix conventions fail visible rather than broadening a float tolerance.
    bool good=affine(uploaded[0])&&affine(draw.camera.inverseView)&&finite(draw.camera.relativeClip);
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)
        good=good&&double(uploaded[0][col*4+row])==object.world[row*4+col];
    if(draw.position==Position::RelativeAffineXYZ1)
        good=good&&uploaded[1]==draw.camera.relativeClip&&uploaded[3]==draw.camera.inverseView;
    else if(draw.position==Position::DirectMvpXYZ1)
        good=good&&draw.camera.fullClipReady&&finite(draw.camera.fullClip)&&uploaded[2]==draw.camera.fullClip;
    else good=false;
    if(!good){++stats_.matrixMismatch;return false;}
    // Only fields determining descriptor/ring ownership participate. Native
    // diagnostic/padding fields in a frame are not identity evidence.
    std::uintptr_t frameNow{},constantsNow{};
    if(!read(context+0x30a0,&frameNow,sizeof frameNow)||!read(frame,frameAfter.data(),frameAfter.size())||
       !read(first+8,&constantsNow,sizeof constantsNow)||!read(constants,cbAfter.data(),cbAfter.size()))return false;
    if(frameNow!=frame||constantsNow!=constants||field<std::uintptr_t>(cbAfter,8)!=field<std::uintptr_t>(cb,8)||
       field<std::uint64_t>(cbAfter,0x28)!=offset||field<std::uintptr_t>(frameAfter,0x38)!=draw.command||
       field<std::uintptr_t>(frameAfter,0x60)!=first||field<std::uintptr_t>(frameAfter,0x68)!=last||
       field<std::uintptr_t>(frameAfter,0x70)!=capacityEnd||field<std::uintptr_t>(frameAfter,0x98)!=wrapper||
       field<std::uintptr_t>(frameAfter,0xa0)!=mapped||field<std::uint64_t>(frameAfter,0xa8)!=cursor) {
        ++stats_.badUniforms;return false;
    }
    return true;
}
bool NativeStaticOcclusionSource::matrices(const Object& object,const Submitted& draw,const NativeMatrixSnapshot& snapshot)noexcept {
    const auto* lease=borrowScope_;
    if(!lease||lease->owner_!=this||!lease->qualified_||lease->frame_!=frame_||
       lease->epoch_!=epoch_||lease->info_!=draw.info||!snapshot.valid||!snapshot.frame||
       !snapshot.descriptor||!snapshot.buffer||!draw.command||snapshot.command!=draw.command||
       snapshot.context!=reinterpret_cast<std::uintptr_t>(draw.context)||!snapshot.context||
       snapshot.range<272||snapshot.range>65536||snapshot.offset>UINT64_MAX-snapshot.range) {
        ++stats_.invalidMatrixSnapshots;++stats_.badUniforms;return false;
    }
    const auto& uploaded=snapshot.matrices;
    bool good=affine(uploaded[0])&&affine(draw.camera.inverseView)&&finite(draw.camera.relativeClip);
    for(const auto& matrix:uploaded)good=good&&finite(matrix);
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)
        good=good&&double(uploaded[0][col*4+row])==object.world[row*4+col];
    if(draw.position==Position::RelativeAffineXYZ1)
        good=good&&uploaded[1]==draw.camera.relativeClip&&uploaded[3]==draw.camera.inverseView;
    else if(draw.position==Position::DirectMvpXYZ1)
        good=good&&draw.camera.fullClipReady&&finite(draw.camera.fullClip)&&uploaded[2]==draw.camera.fullClip;
    else good=false;
    if(!good){++stats_.invalidMatrixSnapshots;++stats_.matrixMismatch;return false;}
    ++stats_.reusedMatrixSnapshots;return true;
}
const NativeStaticOcclusionSource::Snapshot& NativeStaticOcclusionSource::capture(std::uint64_t frame,const Object& object,const Submitted& draw)noexcept {
    return captureImpl(frame,object,draw,nullptr);
}
const NativeStaticOcclusionSource::Snapshot& NativeStaticOcclusionSource::capture(std::uint64_t frame,const Object& object,const Submitted& draw,const NativeMatrixSnapshot& matrices)noexcept {
    return captureImpl(frame,object,draw,&matrices);
}
bool NativeStaticOcclusionSource::capturePacket(std::uint64_t frame,const Object& object,const Submitted& draw,
        const NativeMatrixSnapshot& uploaded,Packet& result)noexcept {
    result.reset();
    if(!ready_||!frame||frame_!=frame||object.frame!=frame){++stats_.stale;return false;}
    if(!draw.qualified||!object.valid||draw.position==Position::Unknown||
       (draw.positionBytes!=12&&draw.positionBytes!=16)||draw.vertexStride<draw.positionBytes||draw.vertexStride>256) {
        ++stats_.unqualified;return false;
    }
    if(!canCapturePacket(frame)){++stats_.budgetStops;return false;}++stats_.packetDraws;
    Object live{};
    if(!inspect(frame,draw.info,live)||!sameObject(object,live)){++stats_.changedObject;return false;}
    if(!matrices(object,draw,uploaded))return false;
    NativeStaticTriangles::Draw source{draw.vertexBuffer,draw.indexBuffer,draw.indexCount,object.world,draw.faces,true,
        NativeStaticTriangles::Position::CertifiedXYZ1,draw.positionBytes,draw.vertexStride};
    if(!triangles_.capturePacket(frame,source,result))return false;
    Object after{};
    if(!inspect(frame,draw.info,after)||!sameObject(object,after)||
       !api_.triangles.ownedOccluders->current(result.mesh)){
        result.reset();++stats_.changedObject;return false;
    }
    // Release no native state here: result's shared storage is immutable and
    // independently owned. Later invalidation must still cancel its use.
    ++stats_.capturedPackets;return true;
}

bool NativeStaticOcclusionSource::qualifyPacketTransform(std::uint64_t frame,const Object& object,const Camera& camera,
        const NativeMatrixSnapshot& s,NativeGeometryBindingReceipt& receipt)noexcept {
    receipt.transformQualified_=false;
    if(!NativeStaticMatrixWitness::relativeMatches(s,object,camera))return false;
    const auto* lease=borrowScope_;
    // This source-only seal does not change the candidate relation result.
    if(!ready_||!frame||frame_!=frame||!receipt.valid_||!lease||lease->owner_!=this||!lease->qualified_||
       lease->frame_!=frame||lease->epoch_!=epoch_||!lease->objectInspected_||!sameObject(lease->inspectedObject_,object)||
       receipt.owner_!=this||receipt.scope_!=lease||receipt.scopeGeneration_!=lease->generation_||
       receipt.inspectionGeneration_!=lease->inspectionGeneration_||!lease->inspectionGeneration_||
       receipt.frame_!=frame||receipt.context_!=s.context||receipt.command_!=s.command||
       !s.frame||!s.descriptor||!s.buffer||!s.command||!s.context||s.range<272||s.range>65536||s.offset>UINT64_MAX-s.range)return true;
    if(!affine(s.matrices[0])||!affine(camera.inverseView)||!finite(camera.relativeClip))return true;
    for(const auto&m:s.matrices)if(!finite(m))return true;
    receipt.transformQualified_=true;return true;
}
bool NativeStaticOcclusionSource::capturePacketQualified(std::uint64_t frame,const NativeGeometryBindingReceipt& receipt,
        Raster::Faces faces,Packet& result)noexcept {
    result.reset();const auto* lease=borrowScope_;
    if(!ready_||!frame||frame_!=frame){++stats_.stale;return false;}
    if(!canCapturePacket(frame)){++stats_.budgetStops;return false;}
    if(!receipt.valid_||!receipt.transformQualified_||!lease||lease->owner_!=this||!lease->qualified_||
       lease->frame_!=frame||lease->epoch_!=epoch_||!lease->objectInspected_||
       receipt.owner_!=this||receipt.scope_!=lease||receipt.scopeGeneration_!=lease->generation_||
       receipt.inspectionGeneration_!=lease->inspectionGeneration_||!lease->inspectionGeneration_||receipt.frame_!=frame){
        ++stats_.invalidPacketReceipts;return false;
    }
    ++stats_.packetDraws;++stats_.reusedMatrixSnapshots;
    if(!triangles_.captureQualifiedPacket(frame,lease->inspectedObject_.world,faces,receipt,result))return false;
    ++stats_.capturedPackets;++stats_.verifiedPacketCaptures;return true;
}
bool NativeStaticOcclusionSource::capturePacketVerified(std::uint64_t frame,const Object& object,const Submitted& draw,
        const NativeMatrixSnapshot& uploaded,const NativeGeometryBindingReceipt& receipt,Packet& result)noexcept {
    result.reset();
    if(!ready_||!frame||frame_!=frame||object.frame!=frame){++stats_.stale;return false;}
    if(!canCapturePacket(frame)){++stats_.budgetStops;return false;}
    const auto* lease=borrowScope_;
    if(!draw.qualified||!object.valid||draw.position==Position::Unknown||!receipt.valid_||
       !lease||lease->owner_!=this||!lease->qualified_||lease->info_!=draw.info||lease->frame_!=frame||lease->epoch_!=epoch_||
       !lease->objectInspected_||!sameObject(lease->inspectedObject_,object)||
       receipt.owner_!=this||receipt.scope_!=lease||receipt.scopeGeneration_!=lease->generation_||!lease->generation_||
       receipt.frame_!=frame||receipt.context_!=reinterpret_cast<std::uintptr_t>(draw.context)||receipt.command_!=draw.command||
       receipt.positionBytes_!=draw.positionBytes||receipt.stride_!=draw.vertexStride){
        ++stats_.invalidPacketReceipts;return false;
    }
    ++stats_.packetDraws;
    if(!matrices(object,draw,uploaded))return false;
    NativeStaticTriangles::Draw source{draw.vertexBuffer,draw.indexBuffer,draw.indexCount,object.world,draw.faces,true,
        NativeStaticTriangles::Position::CertifiedXYZ1,draw.positionBytes,draw.vertexStride};
    if(!triangles_.capturePacketVerified(frame,source,receipt,result))return false;
    ++stats_.capturedPackets;++stats_.verifiedPacketCaptures;return true;
}
const NativeStaticOcclusionSource::Snapshot& NativeStaticOcclusionSource::captureImpl(std::uint64_t frame,const Object& object,const Submitted& draw,const NativeMatrixSnapshot* uploaded)noexcept {
    // Failed or exhausted captures publish no geometry. Do not clear/copy the
    // full bounded geometry array on every rejected native primitive.
    result_.object={};result_.geometry.count=result_.geometry.patchCount=result_.geometry.sourceTriangleCount=0;
    result_.geometry.completeDraw=false;result_.geometry.frame=0;
    result_.geometry.worldError={};result_.geometry.vertexHandle=result_.geometry.indexHandle=0;result_.completeDraw=false;
    if(!ready_||!frame||frame_!=frame||object.frame!=frame){++stats_.stale;return result_;}
    if(!draw.qualified||!object.valid||draw.position==Position::Unknown||
       (draw.positionBytes!=12&&draw.positionBytes!=16)||draw.vertexStride<draw.positionBytes||draw.vertexStride>256) {
        ++stats_.unqualified;return result_;
    }
    if(!canCapture(frame)){++stats_.budgetStops;return result_;}++stats_.draws;
    Object live{};
    if(!inspect(frame,draw.info,live)||!sameObject(object,live)){++stats_.changedObject;return result_;}
    if(!(uploaded?matrices(object,draw,*uploaded):matrices(object,draw)))return result_;
    NativeStaticTriangles::Draw source{draw.vertexBuffer,draw.indexBuffer,draw.indexCount,object.world,draw.faces,true,
        NativeStaticTriangles::Position::CertifiedXYZ1,draw.positionBytes,draw.vertexStride};
    const auto& snapshot=triangles_.captureInto(frame,source,result_.geometry);
    if(!snapshot.count&&!snapshot.patchCount)return result_;
    Object after{};
    if(!inspect(frame,draw.info,after)||!sameObject(object,after)){
        NativeStaticTriangles::clear(result_.geometry);++stats_.changedObject;return result_;
    }
    result_.object=object;
    result_.completeDraw=snapshot.completeDraw;
    ++stats_.captured;return result_;
}
}
