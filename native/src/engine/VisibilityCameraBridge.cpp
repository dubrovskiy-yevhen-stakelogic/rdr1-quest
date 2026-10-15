#include "VisibilityCameraBridge.h"
#include "StereoTestMode.h" // near-clip test modes
#include <cstring>
#include <limits>
#include <sstream>

namespace rdrvr::engine {
namespace {
bool readCamera(std::uintptr_t viewport,std::uintptr_t matrix,VisibilityCameraBridge::Reader read,void* context,StockCamera& out){
    std::array<std::byte,cameraReadBytes> fields{};
    if(!read||!viewport||viewport>std::numeric_limits<std::uintptr_t>::max()-cameraReadOffset-cameraReadBytes||
       !read(context,viewport+cameraReadOffset,fields.data(),fields.size()))return false;
    if(matrix){
        if(matrix>std::numeric_limits<std::uintptr_t>::max()-64||!read(context,matrix,fields.data(),64))return false;
    }
    return decodeCamera(fields,out);
}
bool same(const StockCamera& a,const StockCamera& b){
    // Compare decoded numbers: Matrix34 padding is unspecified, and -0 == 0.
    return a.world==b.world&&a.projection==b.projection&&a.nearClip==b.nearClip&&a.farClip==b.farClip;
}
}
std::uint64_t VisibilityCameraBridge::begin(std::uintptr_t renderer,std::uint32_t marker,std::uintptr_t viewport,std::uintptr_t camera,
                                           std::uint64_t ns,Reader read,void* context,const HeadCameraAnchor* anchor,bool requireAnchor){
    std::unique_lock guard(mutex_,std::try_to_lock);if(!guard.owns_lock()){++busy_;return 0;}
    if(!renderer||next_==std::numeric_limits<std::uint64_t>::max()){++rejected_;return 0;}
    // Invalidate the old identity even when its replacement cannot be prepared.
    // Never resurrect an old stationary-camera pair after tracking loss.
    for(auto& slot:slots_)if(slot.ticket.renderer==renderer&&slot.ticket.marker==marker){slot.complete=false;slot.pending=false;slot.consumed=true;++rejected_;return 0;}
    VisibilityTicket ticket{};ticket.renderer=renderer;ticket.marker=marker;ticket.serial=++next_;
    if(!readCamera(viewport,camera,read,context,ticket.stock)){adapter_.invalidate();++badCamera_;return 0;}
    runtime::TrackedViews views{};
    if(!views_.snapshot(ns,views)){adapter_.invalidate();++badTracking_;return 0;}
    StockCamera base=ticket.stock;
    if(requireAnchor&&!anchor){adapter_.invalidate();++badTracking_;return 0;}
    if(anchor){
        if(!anchor->sampledNs||!anchor->presentationRevision||ns<anchor->sampledNs||ns-anchor->sampledNs>HeadCameraMailbox::maxAgeNs||
           !prepareHeadCamera(ticket.stock,*anchor,base)){adapter_.invalidate();++badTracking_;return 0;}
        if(!headAnchored_||anchorActor_!=anchor->actorGuid||anchorRevision_!=anchor->presentationRevision)adapter_.invalidate();
        headAnchored_=true;anchorActor_=anchor->actorGuid;anchorRevision_=anchor->presentationRevision;
        // Same provisional one-unit/metre scale as the world renderer.
        // STEREO TEST 22 NEAR CLIP STOCK / 23 NEAR CLIP 0.2 (far-terrain depth precision);
        // otherwise the 0.05 clamp. D24 forward Z: depth step ~ 2^-24*d^2/near (5 km: ~30 m at 0.05, ~7.5 m at 0.2).
        {const unsigned mode=stereo::testMode.load(std::memory_order_relaxed);
         if(mode==stereo::testModeNear20)base.nearClip=0.2f;
         else if(mode!=stereo::testModeNearStock)base.nearClip=std::min(base.nearClip,0.05f);}
    }else if(headAnchored_){adapter_.invalidate();headAnchored_=false;}
    // The anchor's rig origin (body point B, recenter height/yaw) survives every invalidate() above.
    if(adapter_.prepare(base,views,ticket.serial,1.f,ticket.eyes,anchor?&anchor->origin:nullptr)!=CameraResult::Ready||
       !prepareStereoCull(ticket.eyes,base.nearClip,base.farClip,ticket.cull)){++badTracking_;return 0;}
    if(anchor){ticket.eyes.headAnchored=true;ticket.eyes.actorGuid=anchor->actorGuid;
        ticket.eyes.presentationRevision=anchor->presentationRevision;ticket.eyes.headSampledNs=anchor->sampledNs;
        ticket.eyes.bodyCreature=anchor->bodyCreature;ticket.eyes.hatNode=anchor->hatNode;publishYaw(ticket.eyes,ns);}
    Slot* chosen=nullptr;
    for(auto& slot:slots_)if(!slot.pending&&(!chosen||slot.ticket.serial<chosen->ticket.serial))chosen=&slot;
    if(!chosen){++rejected_;return 0;}
    if(chosen->complete&&!chosen->consumed)++overwritten_;
    *chosen={ticket,true,false,false};++prepared_;if(ticket.eyes.headAnchored)++headPrepared_;return ticket.serial;
}
void VisibilityCameraBridge::finish(std::uint64_t serial,bool success) noexcept {
    if(!serial)return;
    // No engine or XR work occurs under this lock. Completion must not be lost
    // merely because the render thread is reading another slot.
    std::lock_guard guard(mutex_);
    for(auto& slot:slots_)if(slot.pending&&slot.ticket.serial==serial){slot.pending=false;slot.complete=success;if(success)++completed_;return;}
}
bool VisibilityCameraBridge::prepared(std::uint64_t serial,std::uintptr_t renderer,VisibilityTicket& out) const {
    std::lock_guard guard(mutex_);
    for(const auto& slot:slots_)if(serial&&slot.pending&&slot.ticket.serial==serial&&slot.ticket.renderer==renderer){out=slot.ticket;return true;}
    return false;
}
bool VisibilityCameraBridge::consume(std::uintptr_t renderer,std::uint32_t marker,std::uintptr_t viewport,std::uint64_t ns,
                                     Reader read,void* context,VisibilityTicket& out){
    std::unique_lock guard(mutex_,std::try_to_lock);if(!guard.owns_lock()){++busy_;return false;}
    Slot* selected=nullptr;
    for(auto& slot:slots_)if(slot.ticket.renderer==renderer&&slot.ticket.marker==marker){selected=&slot;break;}
    if(!selected||!selected->complete||selected->consumed){++missing_;return false;}
    selected->consumed=true;
    StockCamera camera{};runtime::TrackedViews latest{};const auto& ticket=selected->ticket;
    if(!readCamera(viewport,0,read,context,camera)||!same(camera,ticket.stock)){++badCamera_;return false;}
    const auto& frozen=ticket.eyes.tracking;
    if(ns<frozen.publishedNs||ns-frozen.publishedNs>runtime::TrackedViewsMailbox::maxAgeNs||
       !views_.snapshot(ns,latest)||latest.epochs!=frozen.epochs){++badTracking_;return false;}
    out=ticket;++matched_;return true;
}
std::string VisibilityCameraBridge::report()const{
    std::lock_guard guard(mutex_);std::ostringstream o;
    o<<"{\"mode\":\"OBSERVE_FROZEN_VISIBILITY\",\"engineCullWrites\":0,\"scaleCalibration\":\"UNVERIFIED\""
     <<",\"prepared\":"<<prepared_<<",\"completed\":"<<completed_<<",\"matched\":"<<matched_<<",\"missing\":"<<missing_
     <<",\"badCamera\":"<<badCamera_<<",\"badTracking\":"<<badTracking_<<",\"rejected\":"<<rejected_
     <<",\"overwritten\":"<<overwritten_<<",\"headPrepared\":"<<headPrepared_<<",\"busy\":"<<busy_
     <<",\"rigOriginPlans\":"<<adapter_.sharedPlans()<<",\"ownOriginLatches\":"<<adapter_.ownLatches()
     <<",\"staleRigOrigins\":"<<adapter_.staleShared()<<"}";return o.str();
}
void VisibilityCameraBridge::publishYaw(const StereoCameraPlan& plan,std::uint64_t ns) noexcept {
    // LOCAL->world rotation of this plan = world[0].rotation * inverse(R(q_eye0)) (same as mapControllerRay/worldFromLocal).
    const auto& q=plan.tracking.eyes[0].orientation;const auto& w=plan.world[0];
    const auto u=runtime::rotate({-q[0],-q[1],-q[2],q[3]},{0,0,1});
    const float x=w[0]*u[0]+w[4]*u[1]+w[8]*u[2],z=w[2]*u[0]+w[6]*u[1]+w[10]*u[2];
    if(!std::isfinite(x)||!std::isfinite(z)||x*x+z*z<0.25f)return; // tilted base: no horizontal heading
    const auto s=yawSequence_.load(std::memory_order_relaxed);
    yawSequence_.store(s+1,std::memory_order_relaxed);std::atomic_thread_fence(std::memory_order_release);
    yaw_.store(std::atan2(x,z),std::memory_order_relaxed);yawNs_.store(ns,std::memory_order_relaxed);
    yawOrigin_.store(plan.originSerial,std::memory_order_relaxed);
    yawSequence_.store(s+2,std::memory_order_release);
}
bool VisibilityCameraBridge::worldYaw(std::uint64_t now,float& psi,std::uint64_t& originSerial,std::uint64_t maxAgeNs) const noexcept {
    for(unsigned attempt=0;attempt<4;++attempt){
        const auto s=yawSequence_.load(std::memory_order_acquire);if(s&1u)continue;
        const float yaw=yaw_.load(std::memory_order_relaxed);const auto at=yawNs_.load(std::memory_order_relaxed);
        const auto origin=yawOrigin_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if(yawSequence_.load(std::memory_order_relaxed)!=s)continue;
        // The render stamp may trail or lead the main-thread clock read by a few microseconds.
        if(!s||!at||(now>at&&now-at>maxAgeNs)||(at>now&&at-now>maxAgeNs)||!std::isfinite(yaw))return false;
        psi=yaw;originSerial=origin;return true;
    }
    return false;
}
} // namespace rdrvr::engine
