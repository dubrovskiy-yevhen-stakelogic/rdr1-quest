#include "OwnedIndexedReplay.h"
#include <algorithm>
#include <cerrno>
#include <new>
#include <thread>

namespace rdr95 {
namespace {struct Errno {int value=errno;~Errno(){errno=value;}};}
struct OwnedIndexedReplay::Impl {
    Mirror& mirror;NativeCameraRuntime& runtime;
    const std::thread::id owner=std::this_thread::get_id();bool busy{};
    // Cold storage, reused for every object. Never allocate while recording.
    std::array<NativeCameraRuntime::PacketTuple,Mirror::maxPackets> parts{};
    NativeCameraRuntime::PacketTuple incoming{};
    Impl(Mirror& m,NativeCameraRuntime& r):mirror(m),runtime(r){}
    static bool tuple(const Mirror::Description& from,NativeCameraRuntime::PacketTuple& to)noexcept {
        if(!from.pipeline||!from.restoreLayout||!from.variantSerial||!from.nativeLayoutGeneration||(from.setMask&~127u))return false;
        NativeCameraRuntime::PacketTuple out;out.pipeline=from.pipeline;out.restoreLayout=from.restoreLayout;out.setMask=from.setMask;
        out.expectedVariantSerial=from.variantSerial;out.expectedNativeLayoutGeneration=from.nativeLayoutGeneration;
        for(unsigned i=0;i<Mirror::maxSets;++i)if(from.setMask&(1u<<i)){
            const auto& a=from.sets[i];auto& b=out.descriptors[i];
            if(!a.handle||a.count>b.offsets.size())return false;
            b.handle=a.handle;b.count=a.count;
            std::copy_n(a.offsets.begin(),a.count,b.offsets.begin());
        }
        to=out;return true;
    }
};
OwnedIndexedReplay::OwnedIndexedReplay(Mirror& m,NativeCameraRuntime& r)noexcept:impl_(new(std::nothrow) Impl(m,r)){}
OwnedIndexedReplay::~OwnedIndexedReplay()=default;
OwnedIndexedReplay::Result OwnedIndexedReplay::emit(std::span<const Mirror::OwnedPacket> packets)noexcept {
    return emit(packets,{});
}
OwnedIndexedReplay::Result OwnedIndexedReplay::emit(std::span<const Mirror::OwnedPacket> packets,Guard guard)noexcept {
    Errno keep;if(!impl_||impl_->busy||impl_->owner!=std::this_thread::get_id()||packets.empty()||packets.size()>Mirror::maxPackets)return Result::Declined;
    auto& s=*impl_;s.busy=true;struct Done {bool& busy;~Done(){busy=false;}}done{s.busy};
    if(!s.mirror.preflightOwned(packets))return Result::Declined;
    Mirror::Description incoming;if(!s.mirror.describeIncoming(incoming)||!Impl::tuple(incoming,s.incoming))return Result::Declined;
    for(unsigned i=0;i<packets.size();++i){Mirror::Description part;
        if(!s.mirror.describeOwned(packets[i],part)||!(part.command==incoming.command)||part.pass!=incoming.pass||part.subpass!=incoming.subpass||
           part.issuer!=incoming.issuer||part.session!=incoming.session||!Impl::tuple(part,s.parts[i]))return Result::Declined;
    }
    struct Context {
        Mirror& mirror;std::span<const Mirror::OwnedPacket> packets;const Mirror::Description& incoming;
        std::uint64_t observed{};Result result=Result::Declined;
        Guard guard;
        static bool current(void* p)noexcept {
            auto& c=*static_cast<Context*>(p);
            // External code may re-enter the mirror. Recheck the replay's pure state
            // AFTER it before allowing the next raw command.
            return c.mirror.ownedSequenceCurrent(c.incoming,c.observed)&&
                (!c.guard.current||c.guard.current(c.guard.context))&&
                c.mirror.ownedSequenceCurrent(c.incoming,c.observed);
        }
        static bool emit(void* p,NativeCameraRuntime::IndexedPacketPlan& plan)noexcept {
            auto& c=*static_cast<Context*>(p);
            Mirror::Execution execution;
            execution.context=&plan;
            execution.current=[](void* q)noexcept{return static_cast<NativeCameraRuntime::IndexedPacketPlan*>(q)->current();};
            execution.drawIndexed=[](void* q,unsigned part,Mirror::Indexed args)noexcept{
                return static_cast<NativeCameraRuntime::IndexedPacketPlan*>(q)->drawIndexed(part,args.count,args.instances,args.first,args.vertex,args.base);};
            c.result=c.mirror.emitOwnedSequence(c.packets,execution);
            return c.result==Result::Emitted;
        }
    } context{s.mirror,packets,incoming,s.mirror.observationRevision_,Result::Declined,guard};
    const auto result=s.runtime.withIndexedSequencePlan(incoming.command.buffer,{s.parts.data(),packets.size()},s.incoming,&context,Context::emit,Context::current);
    if(result==NativeCameraRuntime::PacketResult::Declined)return Result::Declined;
    return result==NativeCameraRuntime::PacketResult::Completed&&context.result==Result::Emitted?Result::Emitted:Result::EmittedFaulted;
}
}
