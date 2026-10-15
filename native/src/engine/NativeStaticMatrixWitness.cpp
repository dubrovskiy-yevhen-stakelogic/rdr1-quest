#include "NativeStaticMatrixWitness.h"
#include <cmath>
#include <cstring>

namespace rdrvr::engine {
namespace {
template<class T>T read(std::uintptr_t p,std::size_t at=0)noexcept{T x;std::memcpy(&x,reinterpret_cast<const void*>(p+at),sizeof x);return x;}
bool fits(std::uintptr_t p,std::size_t bytes)noexcept{return p&&bytes&&p<=UINTPTR_MAX-bytes;}
bool finite(const NativeStaticMatrixWitness::Matrix& m)noexcept{for(float f:m)if(!std::isfinite(f))return false;return true;}
}
bool NativeStaticMatrixWitness::capture(const Api& api,void* nativeContext,const ActualBinding& b,Snapshot& out)noexcept {
    // A failed witness publishes no valid snapshot. Matrix tails are not a
    // value until valid=true, so do not clear then copy the same 256B twice.
    out.valid=false;out.command=out.descriptor=out.frame=out.context=0;
    out.buffer=out.offset=0;out.range=0;
    if(!api.completeCodeProof||!api.contextVtable||!api.bufferVtable||!b.valid||
       !b.command||!b.descriptor||!b.buffer||b.range<272||b.range>65536)return false;
    const auto context=reinterpret_cast<std::uintptr_t>(nativeContext);
    if(!fits(context,0x30a8)||read<std::uintptr_t>(context)!=api.contextVtable)return false;
    const auto frame=read<std::uintptr_t>(context,0x30a0);
    if(!fits(frame,0xb0)||read<std::uintptr_t>(frame,0x38)!=b.command)return false;
    const auto first=read<std::uintptr_t>(frame,0x60),last=read<std::uintptr_t>(frame,0x68),capacityEnd=read<std::uintptr_t>(frame,0x70);
    if(!fits(first,16)||last<first||capacityEnd<last||(last-first)%8||(last-first)/8<2||(last-first)/8>256)return false;
    const auto cb=read<std::uintptr_t>(first,8);
    if(!fits(cb,0x30))return false;
    const auto layout=read<std::uintptr_t>(cb,8),offset=read<std::uint64_t>(cb,0x28);
    if(!fits(layout,0x1c)||offset!=b.offset||read<std::int32_t>(layout,0x18)!=static_cast<std::int32_t>(b.range))return false;
    const auto wrapper=read<std::uintptr_t>(frame,0x98),mapped=read<std::uintptr_t>(frame,0xa0);
    const auto cursor=read<std::uint64_t>(frame,0xa8);
    if(!fits(wrapper,0x28)||read<std::uintptr_t>(wrapper)!=api.bufferVtable||read<std::uint64_t>(wrapper,0x10)!=b.buffer||!read<std::uintptr_t>(wrapper,0x18))return false;
    const auto capacity=read<std::uint32_t>(wrapper,0x20);
    if(!capacity||!mapped||cursor>capacity||offset>cursor||b.range>cursor-offset||offset>UINTPTR_MAX-mapped||!fits(mapped+std::uintptr_t(offset),256))return false;
    std::memcpy(out.matrices.data(),reinterpret_cast<const void*>(mapped+std::uintptr_t(offset)),sizeof out.matrices);
    for(const auto& m:out.matrices)if(!finite(m))return false;
    // Recheck the native values describing the copied upload. This is not a
    // substitute for the caller's owner lease or command-generation boundary.
    if(read<std::uintptr_t>(context,0x30a0)!=frame||read<std::uintptr_t>(frame,0x38)!=b.command||
       read<std::uintptr_t>(frame,0x60)!=first||read<std::uintptr_t>(first,8)!=cb||read<std::uintptr_t>(cb,8)!=layout||
       read<std::uint64_t>(cb,0x28)!=offset||read<std::uintptr_t>(frame,0x98)!=wrapper||
       read<std::uintptr_t>(frame,0xa0)!=mapped||read<std::uint64_t>(frame,0xa8)!=cursor)return false;
    out.command=b.command;out.descriptor=b.descriptor;out.frame=frame;out.buffer=b.buffer;
    out.offset=offset;out.range=b.range;out.context=context;out.valid=true;return true;
}
bool NativeStaticMatrixWitness::relativeMatches(const Snapshot& s,const NativeStaticOcclusionSource::Object& o,
        const NativeStaticOcclusionSource::Camera& camera)noexcept {
    if(!s.valid||!o.valid||s.matrices[1]!=camera.relativeClip||s.matrices[3]!=camera.inverseView)return false;
    for(unsigned col=0;col<4;++col)for(unsigned row=0;row<4;++row)
        if(double(s.matrices[0][col*4+row])!=o.world[row*4+col])return false;
    return true;
}
}
