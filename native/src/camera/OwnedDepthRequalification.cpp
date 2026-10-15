#include "OwnedDepthRequalification.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace rdr95 {
namespace {struct Errno {int value{errno};~Errno(){errno=value;}};}
bool OwnedDepthRequalification::route(unsigned eye,CameraMainRoute::PacketScopeStamp& out)noexcept {
    const auto stamp=CameraMainRoute::packetStamp();
    if(!CameraMainRoute::packetCurrent(stamp)||stamp.pass->description_.stage!=CameraMainRoute::Stage::Depth||
       stamp.pass->description_.eye!=eye)return false;
    out=stamp;return true;
}
bool OwnedDepthRequalification::extent(const Views& views,unsigned eye,const CameraMainRoute::PacketScopeStamp& stamp,Extent& out)noexcept {
    if(!CameraMainRoute::packetCurrent(stamp)||!views.matches(eye)||!CameraMainRoute::packetCurrent(stamp))return false;
    const auto description=stamp.pass->description_;
    const auto& plan=views.plan();
    if(plan.sceneId!=description.scene||plan.tracking.frameId!=description.pose)return false;
    const auto* bytes=static_cast<const std::byte*>(views.viewport(eye));if(!bytes)return false;
    Extent value{};std::memcpy(&value,bytes+0x330,sizeof value);
    CameraUniformOwner::Bytes camera{};std::memcpy(camera.data(),bytes+0xc0,64);std::memcpy(camera.data()+64,bytes+0x140,64);
    if(!value.width||!value.height||value.width>8192||value.height>8192||camera!=description.camera||!views.matches(eye))return false;
    // readCurrent/ownsThread are the existing ViewportApi callbacks. They may
    // invalidate selection. Check the current owned clone again after them;
    // do not dereference a clone captured before them if it was replaced.
    if(!CameraMainRoute::packetCurrent(stamp)||views.viewport(eye)!=bytes||
       views.plan().sceneId!=description.scene||views.plan().tracking.frameId!=description.pose)return false;
    Extent final{};std::memcpy(&final,bytes+0x330,sizeof final);
    if(final.width!=value.width||final.height!=value.height||
       std::memcmp(bytes+0xc0,camera.data(),64)||std::memcmp(bytes+0x140,camera.data()+64,64))return false;
    out=value;return true;
}
bool OwnedDepthRequalification::full(const VkViewport& v,const VkRect2D& s,Extent e)noexcept {
    return v.x==0.f&&v.y==float(e.height)&&v.width==float(e.width)&&v.height==-float(e.height)&&
        std::isfinite(v.minDepth)&&std::isfinite(v.maxDepth)&&v.minDepth>=0.f&&v.maxDepth<=1.f&&v.minDepth<=v.maxDepth&&
        s.offset.x==0&&s.offset.y==0&&s.extent.width==e.width&&s.extent.height==e.height;
}
bool OwnedDepthRequalification::local(const Mirror& m,const Mirror::OwnedPacket& packet,std::uint64_t observed)noexcept {
    if(std::this_thread::get_id()!=m.owner_||!m.cold_||!m.recording_||m.failed_.load(std::memory_order_acquire)||
       m.retired_.load(std::memory_order_acquire)||m.observationRevision_!=observed)return false;
    const auto& p=packet.packet_;
    return p.issuer_==m.issuer_&&p.session_==m.session_&&p.command_==m.command_&&p.pass_==m.pass_&&p.subpass_==m.subpass_;
}
bool OwnedDepthRequalification::proof(Mirror& m,const Mirror::OwnedPacket& packet,const Registry::PlainDepthCertificate& cert)noexcept {
    Registry::PlainDepthDescription depth;Mirror::Description actual;
    // This private read-only predicate is also valid during controlled
    // emission. It does not recursively invoke the public admission path.
    if(!cert.resolve(depth)||!m.complete(packet.packet_.state_))return false;
    m.describeState(packet.packet_.state_,packet.mask_,actual);
    const auto& v=depth.draw.variant;
    if(actual.pipeline!=v.nativePipeline||actual.restoreLayout!=v.restoreLayout||actual.variantSerial!=v.serial||
       actual.nativeLayoutGeneration!=depth.draw.nativeLayoutGeneration||actual.command.device!=depth.device||
       actual.command.deviceGeneration!=v.deviceGeneration||actual.pass!=depth.renderPass||actual.subpass!=depth.subpass||
       !depth.requiredOwnedSets||actual.setMask!=depth.requiredOwnedSets||packet.mask_!=depth.requiredOwnedSets)return false;
    for(unsigned set=0;set<Mirror::maxSets;++set)if(depth.requiredOwnedSets&(1u<<set)){
        UniformCaptureSink::SourceBinding source;CommandUniformSets::Binding owned;
        if(!packet.uniforms_.resolve(set,source,owned)||
           (owned.presentBindings&depth.requiredOwnedBindings[set])!=depth.requiredOwnedBindings[set])return false;
        const auto& saved=packet.packet_.state_.sets[set];
        if(owned.device!=actual.command.device||owned.command!=actual.command.buffer||owned.descriptor!=saved.handle||
           owned.count!=saved.count||owned.count>owned.offsets.size()||
           !std::equal(owned.offsets.begin(),owned.offsets.begin()+owned.count,saved.offsets.begin()))return false;
    }
    return cert.current();
}
bool OwnedDepthRequalification::captureLeft(Mirror& m,const Mirror::OwnedPacket& packet,
    const Registry::PlainDepthCertificate& cert,const Views& views,Source& out)noexcept {
    Errno keep;CameraMainRoute::PacketScopeStamp stamp;
    if(!m.good()||!route(0,stamp))return false;
    const auto observed=m.observationRevision_;
    Extent e;if(!extent(views,0,stamp,e)||!m.ownedCurrent(packet)||!proof(m,packet,cert)||
       !full(packet.packet_.state_.viewport,packet.packet_.state_.scissor,e)||
       !local(m,packet,observed)||!CameraMainRoute::packetCurrent(stamp))return false;
    Source value;value.packet_=packet;value.certificate_=cert;value.extent_=e;
    value.scene_=stamp.pass->description_.scene;value.pose_=stamp.pass->description_.pose;out=value;return true;
}
bool OwnedDepthRequalification::requalifyRight(Mirror& m,const Source& source,const Views& views,PreparedRight& out)noexcept {
    Errno keep;CameraMainRoute::PacketScopeStamp stamp;
    if(!m.good()||!source.scene_||!source.pose_||!route(1,stamp)||stamp.pass->description_.scene!=source.scene_||
       stamp.pass->description_.pose!=source.pose_)return false;
    const auto observed=m.observationRevision_;
    Extent e;if(!extent(views,1,stamp,e)||!m.ownedCurrent(source.packet_)||!proof(m,source.packet_,source.certificate_)||
       !full(m.state_.viewport,m.state_.scissor,e)||
       std::memcmp(&m.state_.viewport.minDepth,&source.packet_.packet_.state_.viewport.minDepth,2*sizeof(float))||
       !local(m,source.packet_,observed)||!CameraMainRoute::packetCurrent(stamp))return false;
    PreparedRight value;value.mirror_=&m;value.packet_=source.packet_;value.certificate_=source.certificate_;
    value.route_=stamp;value.observed_=observed;
    value.packet_.packet_.state_.viewport=m.state_.viewport;value.packet_.packet_.state_.scissor=m.state_.scissor;
    if(!value.current())return false;out=value;return true;
}
bool OwnedDepthRequalification::PreparedRight::current()const noexcept {
    // Route TLS comparison is checked before the saved Pass pointer is read.
    if(!mirror_||!CameraMainRoute::packetCurrent(route_)||route_.pass->description_.eye!=1||
       route_.pass->description_.stage!=CameraMainRoute::Stage::Depth||!local(*mirror_,packet_,observed_))return false;
    return true;
}
bool OwnedDepthRequalification::PreparedRight::resolve(Mirror::OwnedPacket& out)const noexcept {
    Errno keep;if(!current()||!mirror_->good()||!proof(*mirror_,packet_,certificate_)||!current())return false;out=packet_;return true;
}
}
