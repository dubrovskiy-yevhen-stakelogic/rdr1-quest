#include "OrdinaryDepthReplay.h"
#include "OrdinaryNativeSnapshot.h"
#include "OrdinaryGateProbe.h"
#include "SceneRouteTape.h"
#include <cerrno>
#include <new>
#include <thread>

namespace rdr95 {
namespace {struct KeepErrno{int saved=errno;~KeepErrno(){errno=saved;}};}
struct OrdinaryDepthReplay::Impl {
    static constexpr unsigned Capacity=1024,Probes=8;
    struct Key {
        std::uintptr_t container{},record{},drawable{},point{};int technique{},lod{};
        bool operator==(const Key&)const=default;
    };
    struct Entry {
        Key key;OrdinaryReplayFrame::Stamp frame;
        OrdinaryNativeSnapshot::Snapshot native;
        OwnedDepthRequalification::Source source;
        Mirror* mirror{};VkCommandBuffer command{};
        bool ready{},duplicate{},used{};
    };
    NativeIndexedObservation& observation;OrdinaryPacketCapture& capture;NativeCameraRuntime& runtime;
    ContextQuery query{};Observer::Api recorder;
    const std::thread::id owner=std::this_thread::get_id();
    std::array<Entry,Capacity> entries;
    OrdinaryNativeSnapshot native;OrdinaryReplayFrame::Stamp frame;
    Entry* pending{};std::uint64_t captureSerial{};
    Mirror* emitterMirror{};std::unique_ptr<OwnedIndexedReplay> emitter;
    Stats stats;bool enabled{};
    Impl(NativeIndexedObservation& o,OrdinaryPacketCapture& c,NativeCameraRuntime& r,ContextQuery q):
        observation(o),capture(c),runtime(r),query(q),recorder(c.observer()){}
    bool owned()const noexcept{return owner==std::this_thread::get_id();}
    bool current()const noexcept{return owned()&&enabled&&frame.current();}
    static Key key(const Body& b,const Call& c)noexcept {return {b.container,b.record,b.drawable,b.addressPoint,c.technique,c.lod};}
    static unsigned hash(const Key& k)noexcept {
        std::uint64_t h=k.container^(k.record>>4)^(k.drawable<<7)^(std::uint64_t(unsigned(k.technique))<<37)^unsigned(k.lod);
        h^=h>>30;h*=0xbf58476d1ce4e5b9ull;h^=h>>27;h*=0x94d049bb133111ebull;return unsigned(h^(h>>31))&(Capacity-1);
    }
    Entry* find(const Key& key,bool reserve)noexcept {
        Entry* vacant{};const auto start=hash(key);
        for(unsigned i=0;i<Probes;++i){auto& e=entries[(start+i)&(Capacity-1)];
            if(!e.frame.current()){if(!vacant)vacant=&e;continue;}
            if(e.key==key)return &e;
        }
        if(!reserve||!vacant)return nullptr;
        vacant->key=key;vacant->frame=frame;vacant->ready=vacant->duplicate=vacant->used=false;
        vacant->mirror=nullptr;vacant->command=VK_NULL_HANDLE;return vacant;
    }
    bool context(OrdinaryReplayContext& c)noexcept {
        return current()&&query&&query(c)&&current()&&c.scene==frame.scene()&&c.pose==frame.pose()&&native.context(c);
    }
    void count(const OrdinaryNativeSnapshot::Stats& n)noexcept {stats.reads+=n.reads;stats.bytes+=n.bytes;scene_routes::snapshot(unsigned(n.failure),n.detail,n.reads,n.bytes);}
    void cancel()noexcept {++stats.faults;OrdinaryReplayFrame::fault();}
};
OrdinaryDepthReplay::OrdinaryDepthReplay(NativeIndexedObservation& o,OrdinaryPacketCapture& c,NativeCameraRuntime& r,ContextQuery q)noexcept:
    impl_(new(std::nothrow)Impl(o,c,r,q)){}
OrdinaryDepthReplay::~OrdinaryDepthReplay(){end();}
bool OrdinaryDepthReplay::begin()noexcept {
    KeepErrno keep;if(!impl_||!impl_->owned()||callOwner_)return false;
    if(active_&&active_!=this){active_->end();return false;}
    auto& s=*impl_;s.frame=OrdinaryReplayFrame::current();s.stats={};s.pending=nullptr;s.captureSerial=0;
    s.enabled=s.frame.current()&&s.query&&s.recorder.before&&s.recorder.after;
    if(s.enabled)active_=this;return s.enabled;
}
void OrdinaryDepthReplay::end()noexcept {
    KeepErrno keep;if(!impl_||!impl_->owned())return;
    auto& s=*impl_;s.enabled=false;if(s.pending)s.pending->ready=false;
    if(active_==this)active_=nullptr;
    // callOwner survives until the matching after callback, which MUST close
    // the recorder's Producer even when frame/session selection was revoked.
}
const OrdinaryDepthReplay::Stats& OrdinaryDepthReplay::statistics()const noexcept {static const Stats empty;return impl_?impl_->stats:empty;}
OrdinaryDepthReplay::Observer::Api OrdinaryDepthReplay::observer()noexcept {return {nullptr,before,after,replay,fault};}
OrdinaryPacketCapture::Api OrdinaryDepthReplay::consumer()noexcept {return {nullptr,completed};}
std::uint64_t OrdinaryDepthReplay::before(void*,const Body& b,const Call& c)noexcept {
    KeepErrno keep;
    gateprobe::hit(gateprobe::Before);
    if(gateprobe::enabled){const auto mask=CameraMainRoute::diagnosticMask();
        for(unsigned i=0;i<7;++i)if(mask&(1u<<i))gateprobe::hit(static_cast<gateprobe::Counter>(gateprobe::RouteActive+i));}
    if(!active_||!active_->impl_||callOwner_)return 0;
    gateprobe::hit(gateprobe::BeforeActive);
    if(!OwnedDepthRequalification::selectedEye(0))return 0;
    gateprobe::hit(gateprobe::BeforeLeft);
    auto& s=*active_->impl_;if(!s.current())return 0;gateprobe::hit(gateprobe::BeforeCurrent);++s.stats.left;
    OrdinaryReplayContext context;if(!s.context(context))return 0;
    const auto key=Impl::key(b,c);auto* old=s.find(key,false);
    if(old){old->duplicate=true;old->ready=false;++s.stats.duplicate;return 0;}
    auto* entry=s.find(key,true);if(!entry){++s.stats.capacity;return 0;}
    OrdinaryNativeSnapshot::Stats checked;
    if(!s.native.capture(b,c,entry->native,checked)){s.count(checked);++s.stats.nativeDeclined;return 0;}
    s.count(checked);
    // Native Read has ended before opening the capture and invoking original.
    if(!s.current()||!entry->frame.current()||!entry->native.epoch.current())return 0;
    s.pending=entry;callOwner_=active_;
    const auto serial=s.recorder.before(s.recorder.context,b,c);
    if(!serial){s.pending=nullptr;callOwner_=nullptr;return 0;}
    s.captureSerial=serial;return serial;
}
void OrdinaryDepthReplay::after(void*,std::uint64_t serial,const Body& b,const Call& c,bool ok)noexcept {
    KeepErrno keep;auto* owner=callOwner_;if(!owner||!owner->impl_)return;
    auto& s=*owner->impl_;if(!s.owned())return;
    if(serial!=s.captureSerial){
        s.recorder.after(s.recorder.context,serial,b,c,false);
        if(s.pending)s.pending->ready=false;
        return; // Retain the real owner until its matching after closes Producer.
    }
    s.recorder.after(s.recorder.context,serial,b,c,ok&&s.current());
    s.pending=nullptr;s.captureSerial=0;callOwner_=nullptr;
}
void OrdinaryDepthReplay::completed(void*,const Body& b,const Call& c,Mirror& mirror,VkCommandBuffer command,std::span<const Mirror::OwnedPacket> packets)noexcept {
    KeepErrno keep;auto* owner=callOwner_;if(!owner||!owner->impl_)return;auto& s=*owner->impl_;
    auto* e=s.pending;
    if(!s.current()||!e||!e->frame.current()||e->duplicate||e->key!=Impl::key(b,c)||packets.size()!=1||
       !OwnedDepthRequalification::selectedEye(0))return;
    OrdinaryReplayContext context;if(!s.context(context))return;
    OrdinaryNativeSnapshot::Binding::OrdinaryEpoch epoch;OrdinaryNativeSnapshot::Stats checked;
    if(!s.native.matches(b,c,e->native,epoch,checked)){s.count(checked);++s.stats.nativeDeclined;return;}
    s.count(checked);
    // The completed packet is historical immutable data. Bind it to the ONE
    // selected native geometry; another draw or extra stream is not inferred.
    OrdinaryNativeSnapshot::Geometry expected;Mirror::OwnedGeometry geometry;Mirror::Description description;
    if(!e->native.geometry.describe(expected)||!mirror.describeOwnedGeometry(packets[0],geometry)||
       !(geometry.vertexMask&1u)||reinterpret_cast<std::uintptr_t>(geometry.vertices[0])!=expected.vertexBuffer||geometry.offsets[0]!=0||
       reinterpret_cast<std::uintptr_t>(geometry.index)!=expected.indexBuffer||geometry.indexOffset!=0||geometry.indexType!=VK_INDEX_TYPE_UINT16||
       geometry.draw.count!=expected.indexCount||geometry.draw.instances!=1||geometry.draw.first||geometry.draw.vertex||geometry.draw.base||
       !mirror.describeOwned(packets[0],description)||description.command.buffer!=command){++s.stats.packetDeclined;return;}
    rdrvr::engine::FrozenCameraCreationRegistry::PlainDepthCertificate certificate;
    rdrvr::engine::FrozenCameraCreationRegistry::PlainDepthDescription shape;
    if(!s.runtime.lookupPlainDepthCertificate(description.pipeline,certificate)||
       !certificate.resolve(shape)||shape.vertexBindingMask!=1u||
       !OwnedDepthRequalification::captureLeft(mirror,packets[0],certificate,*context.views,e->source)||
       !s.current()||!e->frame.current()||!epoch.current()||!e->native.epoch.current()){
        ++s.stats.packetDeclined;return;
    }
    if(!s.emitter){s.emitter.reset(new(std::nothrow)OwnedIndexedReplay(mirror,s.runtime));s.emitterMirror=&mirror;}
    if(!s.emitter||s.emitterMirror!=&mirror){++s.stats.packetDeclined;return;}
    e->mirror=&mirror;e->command=command;e->ready=true;++s.stats.captured;
}
OrdinaryDepthReplay::Observer::ReplayResult OrdinaryDepthReplay::replay(void*,const Body& b,const Call& c,Observer::ReplayGuard guard)noexcept {
    KeepErrno keep;using Result=Observer::ReplayResult;
    gateprobe::hit(gateprobe::Replay);
    if(!active_||!active_->impl_||callOwner_)return Result::Declined;
    gateprobe::hit(gateprobe::ReplayActive);
    if(!OwnedDepthRequalification::selectedEye(1))return Result::Declined;
    gateprobe::hit(gateprobe::ReplayRight);
    auto& s=*active_->impl_;if(!s.current())return Result::Declined;++s.stats.right;
    auto* e=s.find(Impl::key(b,c),false);
    if(!e||!e->ready||e->duplicate||e->used||!e->frame.current()||!s.emitter||s.emitterMirror!=e->mirror)return Result::Declined;
    e->used=true; // A second occurrence of the same native key is never replayed.
    OrdinaryReplayContext context;if(!s.context(context))return Result::Declined;
    OrdinaryNativeSnapshot::Binding::OrdinaryEpoch epoch;OrdinaryNativeSnapshot::Stats checked;
    if(!s.native.matches(b,c,e->native,epoch,checked)){s.count(checked);++s.stats.nativeDeclined;return Result::Declined;}
    s.count(checked);
    if(!s.native.visibleForCurrentEye(e->native)||!epoch.current())return Result::Declined;
    if(s.observation.current(e->command)!=e->mirror)return Result::Declined;
    OwnedDepthRequalification::PreparedRight prepared;Mirror::OwnedPacket packet;
    // Target camera/viewport callbacks and Registry/owned-byte validation are
    // outside Read. The guard used by raw PFNs below contains only liveness.
    if(!OwnedDepthRequalification::requalifyRight(*e->mirror,e->source,*context.views,prepared)||!prepared.resolve(packet)){
        ++s.stats.targetDeclined;return Result::Declined;
    }
    struct GuardContext {
        Impl& s;Impl::Entry& entry;OwnedDepthRequalification::PreparedRight& prepared;
        OrdinaryNativeSnapshot::Binding::OrdinaryEpoch epoch;Observer::ReplayGuard wrapper;OrdinaryReplayFrame::Stamp frame;
        static bool current(void* p)noexcept {
            auto& g=*static_cast<GuardContext*>(p);
            return g.frame.current()&&g.s.current()&&g.entry.frame.current()&&g.epoch.current()&&g.entry.native.epoch.current()&&g.prepared.current()&&
                g.wrapper.current&&g.wrapper.current(g.wrapper.context)&&
                g.frame.current()&&g.s.current()&&g.entry.frame.current()&&g.epoch.current()&&g.entry.native.epoch.current()&&g.prepared.current();
        }
    } admission{s,*e,prepared,epoch,guard,s.frame};
    const auto result=s.emitter->emit({&packet,1},{&admission,GuardContext::current});
    if(result==Mirror::Emission::Declined){++s.stats.emitDeclined;return Result::Declined;}
    if(result!=Mirror::Emission::Emitted||!GuardContext::current(&admission)){s.cancel();return Result::Faulted;}
    ++s.stats.replayed;return Result::Emitted;
}
void OrdinaryDepthReplay::fault(void*,const Body&,const Call&)noexcept {
    KeepErrno keep;if(active_&&active_->impl_&&active_->impl_->owned())active_->impl_->cancel();else OrdinaryReplayFrame::fault();
}
}
