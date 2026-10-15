#include "OrdinaryPacketCapture.h"
#include <cerrno>
#include <new>

namespace rdr95 {
namespace {struct Errno {int value=errno;~Errno(){errno=value;}};}
OrdinaryPacketCapture::OrdinaryPacketCapture(NativeIndexedObservation& observation,UniformCaptureControl& control,const UniformCaptureSink& sink,RecordingThread recordingThread)noexcept:
    observation_(observation),control_(control),sink_(sink),recordingThread_(recordingThread){}
OrdinaryPacketCapture::~OrdinaryPacketCapture()=default;
bool OrdinaryPacketCapture::configure(Api api)noexcept {
    Errno keep;
    if(configured_.exchange(true,std::memory_order_acq_rel)||stopped_.load(std::memory_order_acquire)||!api.completed||!recordingThread_)return false;
    auto storage=std::unique_ptr<Storage>(new(std::nothrow)Storage);
    if(!storage)return false;
    storage_=std::move(storage);api_=api;ready_.store(true,std::memory_order_release);return true;
}
void OrdinaryPacketCapture::revoke()noexcept {
    auto value=revocation_.load(std::memory_order_relaxed);
    while(value){const auto next=value==UINT64_MAX?0:value+1;
        if(revocation_.compare_exchange_weak(value,next,std::memory_order_acq_rel))break;}
}
void OrdinaryPacketCapture::disarm()noexcept {Errno keep;stopped_.store(true,std::memory_order_release);revoke();}
ordinary_vtable::Observer::Api OrdinaryPacketCapture::observer()noexcept {return {this,before,after};}
std::uint64_t OrdinaryPacketCapture::before(void* context,const Body& body,const Call& call)noexcept {
    Errno keep;auto& self=*static_cast<OrdinaryPacketCapture*>(context);
    if(active_){active_->failed_=true;active_->control_.revoke();return 0;}
    // The SAME actual recording-owner predicate as UniformCaptureControl is
    // checked before reading or changing any thread-confined recorder state.
    if(!self.ready_.load(std::memory_order_acquire)||self.stopped_.load(std::memory_order_acquire))return 0;
    if(!self.recordingThread_()){self.revoke();return 0;}
    if(!self.storage_||!self.api_.completed||
       !self.observation_.enabled()||call.bits||call.lod<0||call.lod>=4)return 0;
    // Only the same thread may own an instance. configure is cold; recording
    // callers must use the same actual owner as UniformCaptureControl.
    if(self.ownerChosen_&&self.owner_!=std::this_thread::get_id()){self.revoke();return 0;}
    self.producer_.emplace(self.control_,true);
    if(!self.control_.enabled()){self.producer_.reset();return 0;}
    if(!self.ownerChosen_){self.owner_=std::this_thread::get_id();self.ownerChosen_=true;}
    if(self.serial_==UINT64_MAX){self.producer_.reset();return 0;}
    self.currentSerial_=++self.serial_;self.body_=body;self.call_=call;
    self.command_=VK_NULL_HANDLE;self.mirror_=nullptr;self.count_=0;self.failed_=false;self.publishing_=false;
    self.effects_=self.observation_.effects();self.revocationAtBegin_=self.revocation_.load(std::memory_order_acquire);
    if(!self.effects_||!self.revocationAtBegin_){self.producer_.reset();return 0;}
    active_=&self;return self.currentSerial_;
}
bool OrdinaryPacketCapture::current()noexcept {
    return active_==this&&!failed_&&!stopped_.load(std::memory_order_acquire)&&
        currentSerial_&&serial_==currentSerial_&&revocationAtBegin_&&
        revocation_.load(std::memory_order_acquire)==revocationAtBegin_&&
        observation_.enabled()&&effects_&&observation_.effects()==effects_&&control_.enabled();
}
void OrdinaryPacketCapture::indexed(VkCommandBuffer command,Mirror::Indexed indexed)noexcept {
    Errno keep;if(active_)active_->capture(command,indexed);
}
void OrdinaryPacketCapture::invalidateCurrent()noexcept {Errno keep;if(active_)active_->failed_=true;}
void OrdinaryPacketCapture::capture(VkCommandBuffer command,Mirror::Indexed indexed)noexcept {
    if(publishing_||!current()||!command||!indexed.count||!indexed.instances||count_==Mirror::maxPackets){failed_=true;return;}
    auto* observed=observation_.current(command);
    if(!observed||(command_&&(command_!=command||mirror_!=observed))){failed_=true;return;}
    if(!command_){command_=command;mirror_=observed;}
    if(!observed->snapshotOwnedIndexed(command,indexed,sink_,storage_->packets[count_])||
       observation_.current(command)!=observed||!current()){failed_=true;return;}
    ++count_;
}
void OrdinaryPacketCapture::after(void* context,std::uint64_t serial,const Body& body,const Call& call,bool completed)noexcept {
    Errno keep;auto& self=*static_cast<OrdinaryPacketCapture*>(context);
    // TLS proves which thread physically owns this open scope, independently
    // of a recording predicate that may have been revoked during original.
    // Foreign calls only revoke atomically. The owning matched scope ALWAYS
    // closes below, including when native recording ownership has ended.
    if(active_!=&self){self.revoke();return;}
    if(serial!=self.currentSerial_){self.failed_=true;return;}
    const bool recording=self.recordingThread_&&self.recordingThread_();
    const bool same=body.container==self.body_.container&&body.record==self.body_.record&&
        body.drawable==self.body_.drawable&&body.addressPoint==self.body_.addressPoint&&
        call.drawable==self.call_.drawable&&call.world==self.call_.world&&call.previous==self.call_.previous&&
        call.technique==self.call_.technique&&call.lod==self.call_.lod&&call.bits==self.call_.bits&&call.caller==self.call_.caller;
    if(recording&&completed&&same&&self.count_&&self.current()&&self.observation_.current(self.command_)==self.mirror_){
        const auto packets=std::span<const Mirror::OwnedPacket>(self.storage_->packets.data(),self.count_);
        // Validate all retained owned bytes before exposing any part. This
        // performs no Vulkan emission and never substitutes for RIGHT admission.
        if(self.mirror_->preflightOwned(packets)&&self.current()){
            self.publishing_=true;
            self.api_.completed(self.api_.context,body,call,*self.mirror_,self.command_,packets);
        }
    }
    self.failed_=true;self.currentSerial_=0;self.count_=0;self.command_=VK_NULL_HANDLE;self.mirror_=nullptr;
    self.producer_.reset();active_=nullptr;
}
}
