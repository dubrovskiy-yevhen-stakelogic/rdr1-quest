#include "NativeOcclusionVertexCapture.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>

namespace rdrvr::engine {
thread_local NativeOcclusionVertexCapture::Scope* NativeOcclusionVertexCapture::current_{};
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
template<class T,std::size_t N>T at(const std::array<std::byte,N>& b,std::size_t offset=0)noexcept{
    T v{};std::memcpy(&v,b.data()+offset,sizeof v);return v;
}
bool fits(std::uintptr_t p,std::size_t n)noexcept{return p&&n&&p<=UINTPTR_MAX-n;}
template<std::size_t N>bool copy(const NativeOcclusionVertexCapture::Api& a,std::uintptr_t p,std::array<std::byte,N>& out)noexcept{
    if(!fits(p,N))return false;
    try{return a.read(a.context,p,out.data(),N);}catch(...){return false;}
}
}
void NativeOcclusionVertexCapture::configure(Api a,OwnedStaticVertices* cache,OwnedStaticOccluders* mesh)noexcept{
    api_=a;cache_=cache;meshes_=a.indexVtable?mesh:nullptr;
    ready_=cache&&a.read&&a.completeProof&&a.geometryVtable&&a.vertexVtable&&a.bufferVtable&&a.unlockReturn&&a.epoch;
}
NativeOcclusionVertexCapture::Scope::Scope(NativeOcclusionVertexCapture& owner,const void* geometry)noexcept:
    owner_(&owner),previous_(current_),geometry_(geometry){
    Errno preserve;current_=this;
    if(!owner.ready_||previous_)return;
    owner.scopes_.fetch_add(1,std::memory_order_relaxed);
    const auto address=reinterpret_cast<std::uintptr_t>(geometry);
    std::array<std::byte,8> type{};std::array<std::byte,0xc0> data{};
    valid_=copy(owner.api_,address,type)&&at<std::uintptr_t>(type)==owner.api_.geometryVtable&&
        copy(owner.api_,address,data)&&!at<std::uint8_t>(data,0x63)&&!at<std::uintptr_t>(data,0x88);
    // +a0 may name this very optimizer task. Its live Lock gives us the write
    // lease; native Wait/ordinary Draw completion is checked later for replay.
}
NativeOcclusionVertexCapture::Scope::~Scope(){current_=previous_;}
void NativeOcclusionVertexCapture::beforeUnlock(const void* vertex,std::uintptr_t caller)noexcept{
    Errno preserve;auto* scope=current_;
    if(!ready_||!scope||scope->owner_!=this||!scope->valid_||caller!=api_.unlockReturn)return;
    attempts_.fetch_add(1,std::memory_order_relaxed);
    const auto meshSerial=meshes_?meshes_->mutationSerial():0;
    const auto refuse=[&]{refused_.fetch_add(1,std::memory_order_relaxed);};
    std::array<std::byte,0xc0> geometry{};std::array<std::byte,0x40> vb{};std::array<std::byte,0x28> gpu{};
    const auto address=reinterpret_cast<std::uintptr_t>(vertex);
    if(!copy(api_,reinterpret_cast<std::uintptr_t>(scope->geometry_),geometry)||
       at<std::uintptr_t>(geometry)!=api_.geometryVtable||at<std::uint8_t>(geometry,0x63)||at<std::uintptr_t>(geometry,0x88)||
       at<std::uintptr_t>(geometry,0xb0)!=address||!copy(api_,address,vb)||
       at<std::uintptr_t>(vb)!=api_.vertexVtable||at<std::uint8_t>(vb,0xb)!=5||at<std::uint8_t>(vb,0xa)!=1||
       at<std::uintptr_t>(vb,0x20)) {refuse();return;}
    OwnedStaticVertices::Identity id{};
    id.vertexBuffer=address;id.nativeBuffer=at<std::uintptr_t>(vb,0x38);id.fvf=at<std::uintptr_t>(vb,0x30);
    id.vertexCount=at<std::uint16_t>(vb,8);id.stride=at<std::uint32_t>(vb,0x18);
    const auto mapped=at<std::uintptr_t>(vb,0x10);
    if(!copy(api_,id.nativeBuffer,gpu)||at<std::uintptr_t>(gpu)!=api_.bufferVtable||at<std::uint32_t>(gpu,0x24)!=0||
       !copy(api_,id.fvf,id.fvfBytes)){refuse();return;}
    id.gpuBuffer=at<std::uint64_t>(gpu,0x10);id.allocation=at<std::uintptr_t>(gpu,0x18);id.byteSize=at<std::uint32_t>(gpu,0x20);
    const auto bytes=std::uint64_t(id.vertexCount)*id.stride;
    // Exact full-buffer optimizer Lock; partial dynamic writes never enter here.
    if(!bytes||bytes>id.byteSize||bytes>std::numeric_limits<std::size_t>::max()||!fits(mapped,std::size_t(bytes))){refuse();return;}
    if(cache_->capture(api_.epoch,id,reinterpret_cast<const void*>(mapped),std::size_t(bytes),true))
        captured_.fetch_add(1,std::memory_order_relaxed);
    else refuse();
    if(!meshes_)return;
    meshAttempts_.fetch_add(1,std::memory_order_relaxed);
    const auto meshRefuse=[&]{meshRefused_.fetch_add(1,std::memory_order_relaxed);};
    // The pinned optimizer leaves this index buffer unchanged. Its resource
    // constructor retains the CPU map. Every writable Lock/Unlock is tracked
    // through its complete lifetime; headers alone do not prove quiescence.
    const auto indices=at<std::uintptr_t>(geometry,0x38);
    const auto count=at<std::uint32_t>(geometry,0x58);
    const auto rawMode=at<std::uint8_t>(geometry,0x62);
    std::array<std::byte,0x28> ib{},indexGpu{};
    if((rawMode<8?rawMode+1:4)!=4||!count||count%3||
       !copy(api_,indices,ib)||at<std::uintptr_t>(ib)!=api_.indexVtable||
       at<std::uint32_t>(ib,0x20)||count>at<std::uint32_t>(ib,8)){meshRefuse();return;}
    OwnedStaticOccluders::Identity meshId{};meshId.vertices=id;
    meshId.indexBuffer=indices;meshId.nativeIndexBuffer=at<std::uintptr_t>(ib,0x18);
    meshId.indexCount=count;
    const auto indexMapped=at<std::uintptr_t>(ib,0x10);
    if(!copy(api_,meshId.nativeIndexBuffer,indexGpu)||at<std::uintptr_t>(indexGpu)!=api_.bufferVtable||
       at<std::uint32_t>(indexGpu,0x24)!=1){meshRefuse();return;}
    meshId.indexAllocation=at<std::uintptr_t>(indexGpu,0x18);
    meshId.indexGpuBuffer=at<std::uint64_t>(indexGpu,0x10);
    meshId.indexByteSize=at<std::uint32_t>(indexGpu,0x20);
    const auto indexBytes=std::uint64_t(count)*sizeof(std::uint16_t);
    if(indexBytes>meshId.indexByteSize||!fits(indexMapped,std::size_t(indexBytes))){meshRefuse();return;}
    if(meshes_->capture(api_.epoch,meshId,reinterpret_cast<const void*>(mapped),std::size_t(bytes),
                       reinterpret_cast<const std::uint16_t*>(indexMapped),std::size_t(indexBytes),true,meshSerial))
        meshCaptured_.fetch_add(1,std::memory_order_relaxed);
    else meshRefuse();
}
void NativeOcclusionVertexCapture::beforeLock(const void* vertex)noexcept{
    Errno preserve;if(!ready_||!vertex)return;
    // Exact D11 Lock entry: this native base object is borrowed by the original
    // call. One pointer read, then lock-free membership miss for dynamic buffers.
    std::uintptr_t backing{};std::memcpy(&backing,static_cast<const std::byte*>(vertex)+0x38,sizeof backing);
    cache_->invalidate(backing);
    if(meshes_)meshes_->invalidate(backing);
}
void NativeOcclusionVertexCapture::beforeBackingDestroy(const void* backing)noexcept{
    Errno preserve;if(!ready_)return;
    cache_->invalidate(reinterpret_cast<std::uintptr_t>(backing));
    if(meshes_)meshes_->invalidate(reinterpret_cast<std::uintptr_t>(backing));
}
void NativeOcclusionVertexCapture::beforeIndexLock(const void* index)noexcept{
    Errno preserve;if(!ready_||!meshes_||!index)return;
    std::uintptr_t backing{};std::memcpy(&backing,static_cast<const std::byte*>(index)+0x18,sizeof backing);
    meshes_->beginIndexMutation(backing);
}
void NativeOcclusionVertexCapture::afterIndexUnlock(const void* index)noexcept{
    Errno preserve;if(!ready_||!meshes_||!index)return;
    std::uintptr_t backing{};std::memcpy(&backing,static_cast<const std::byte*>(index)+0x18,sizeof backing);
    meshes_->endIndexMutation(backing);
}
NativeOcclusionVertexCapture::Stats NativeOcclusionVertexCapture::stats()const noexcept{
    return {scopes_.load(std::memory_order_relaxed),attempts_.load(std::memory_order_relaxed),
        captured_.load(std::memory_order_relaxed),refused_.load(std::memory_order_relaxed),
        meshAttempts_.load(std::memory_order_relaxed),meshCaptured_.load(std::memory_order_relaxed),meshRefused_.load(std::memory_order_relaxed)};
}
} // namespace rdrvr::engine
