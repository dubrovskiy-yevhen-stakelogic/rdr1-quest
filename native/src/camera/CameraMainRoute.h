#pragma once
#include "CameraDrawAdapter.h"
#include <atomic>
#include <limits>

namespace rdr95 {
// PRIVATE lexical issuer. Caller supplies already selected eye-camera bytes
// from the owned native viewport and the exact MAIN DrawCommon/tiles routes.
// This changes GPU camera selection only, never CPU skip/read permission.
class NativeCameraRuntime;
class OwnedDepthRequalification;
class CameraMainRoute final {
    friend class NativeCameraRuntime;
    friend class OwnedDepthRequalification;
public:
    enum class Stage { Depth, Color, Unsupported };
    struct Description {
        std::uint64_t scene{},pose{},viewGeneration{};
        unsigned eye{};
        Stage stage=Stage::Unsupported;
        CameraUniformOwner::Bytes camera{};
    };
    class Pass;
    class Tiles;
    class Object;
private:
    inline static thread_local std::uint64_t activity_=1;
    static void activity()noexcept{if(activity_!=UINT64_MAX)++activity_;}
    inline static thread_local Pass* active_{};
    inline static thread_local unsigned depth_{};
    inline static thread_local std::uint64_t recordingEpoch_=1;
    inline static std::atomic<std::uint64_t> generation_{};
    static std::uint64_t next() noexcept {
        auto n=generation_.load(std::memory_order_relaxed);
        while(n!=std::numeric_limits<std::uint64_t>::max())
            if(generation_.compare_exchange_weak(n,n+1,std::memory_order_relaxed))return n+1;
        return 0;
    }
public:
    class Pass final {
        friend class CameraMainRoute;
        friend class OwnedDepthRequalification;
        friend class Tiles;
        friend class Object;
        Pass* previous_{};
        CameraUniformOwner* owner_{};
        Description description_{};
        CameraUniformOwner::PassKey key_{};
        VkCommandBuffer cachedCommand_{};
        CameraUniformOwner::Ticket cachedTicket_{};
        std::uint64_t cachedRecordingEpoch_{};
        bool tiles_{},objectExact_{};
        unsigned objects_{},tileDepth_{};
    public:
        Pass(CameraUniformOwner& owner,const Description& description,bool selected) noexcept:
            Pass(&owner,description,selected) {}
        Pass(CameraUniformOwner* owner,const Description& description,bool selected) noexcept:
            previous_(active_),description_(description) {
            // Any nested stage, including unselected lighting/shadow/reflection,
            // masks the outer scope. It cannot inherit the outer camera.
            activity();active_=nullptr;++depth_;
            if(depth_!=1||!owner||!selected||!description.scene||!description.pose||!description.viewGeneration||
               description.eye>=2||(description.stage!=Stage::Depth&&description.stage!=Stage::Color))return;
            const auto serial=next();if(!serial)return;
            key_={serial,description.pose,description.eye};owner_=owner;active_=this;
        }
        ~Pass(){activity();active_=previous_;--depth_;}
        Pass(const Pass&)=delete;
        Pass& operator=(const Pass&)=delete;
        bool active()const noexcept{return active_==this;}
    };
    class Tiles final {
        Pass* pass_{};bool previous_{};
    public:
        // The TraceEntry exact pass0/return-PC/vtable test is the producer.
        explicit Tiles(bool exact) noexcept:pass_(active_) {
            activity();if(pass_){previous_=pass_->tiles_;++pass_->tileDepth_;pass_->tiles_=exact&&pass_->tileDepth_==1;}
        }
        ~Tiles(){activity();if(pass_){--pass_->tileDepth_;pass_->tiles_=previous_;}}
        Tiles(const Tiles&)=delete;
        Tiles& operator=(const Tiles&)=delete;
    };
    class Object final {
        Pass* pass_{};bool previous_{};
    public:
        // Construct around EVERY DrawCommon, even an unknown/non-MAIN call.
        explicit Object(bool exactMain) noexcept:pass_(active_) {
            activity();if(pass_){previous_=pass_->objectExact_;++pass_->objects_;
                pass_->objectExact_=exactMain&&pass_->objects_==1;}
        }
        ~Object(){activity();if(pass_){--pass_->objects_;pass_->objectExact_=previous_;}}
        Object(const Object&)=delete;
        Object& operator=(const Object&)=delete;
    };
    // Notify on every actual command Begin/Reset/Free and pool retirement,
    // before another draw. Missing notification only leaves a stale Ticket:
    // CameraDrawAdapter still rejects it using actual recording identity.
    static void recordingChanged() noexcept {
        // Includes lifecycle calls made while an unselected nested Pass masks
        // active_. The suspended outer pass will refresh on its next draw.
        if(recordingEpoch_!=std::numeric_limits<std::uint64_t>::max())++recordingEpoch_;
    }
    static unsigned diagnosticMask() noexcept {
        const auto* p=active_;if(!p)return 0;
        return 1u | ((p->objectExact_&&p->objects_==1)?2u:0u) | (p->tiles_?4u:0u) |
            (p->description_.stage==Stage::Depth?8u:0u) | (p->description_.stage==Stage::Color?16u:0u) |
            (p->description_.eye==0?32u:0u) | (p->description_.eye==1?64u:0u);
    }
    static bool selectedMain() noexcept {
        const auto* p=active_;
        return p&&p->objectExact_&&p->objects_==1&&
            (p->description_.stage!=Stage::Depth||p->tiles_);
    }
private:
    struct PacketScopeStamp {Pass* pass{};std::uint64_t activity{},recording{};CameraUniformOwner::PassKey key{};};
    static PacketScopeStamp packetStamp()noexcept{return {active_,activity_,recordingEpoch_,active_?active_->key_:CameraUniformOwner::PassKey{}};}
    static bool packetCurrent(const PacketScopeStamp& s)noexcept{return selectedMain()&&s.pass&&s.pass==active_&&activity_!=UINT64_MAX&&
        s.activity==activity_&&s.recording==recordingEpoch_&&s.key==active_->key_;}
    // Never export a Request: its camera Ticket deliberately outlives lexical
    // MAIN scope for GPU execution, and must not be reused in a later shadow.
    static bool request(VkCommandBuffer command,CameraDrawAdapter::Request& out) noexcept {
        auto* p=active_;
        if(!command||!selectedMain()||recordingEpoch_==std::numeric_limits<std::uint64_t>::max())return false;
        if(p->cachedCommand_!=command||p->cachedRecordingEpoch_!=recordingEpoch_){
            CameraUniformOwner::Ticket ticket;
            if(!p->owner_->prepare(command,p->key_,p->description_.camera,ticket))return false;
            p->cachedTicket_=ticket;p->cachedCommand_=command;p->cachedRecordingEpoch_=recordingEpoch_;
        }
        out={p->cachedTicket_,p->key_};return true;
    }
public:
    // Issue and consume synchronously; no transferable lexical permission.
    static bool draw(CameraDrawAdapter& adapter,VkCommandBuffer command,
                     std::uint32_t count,std::uint32_t instances,std::uint32_t first,std::uint32_t base) noexcept {
        CameraDrawAdapter::Request value;
        const bool qualified=request(command,value);
        return adapter.draw(command,count,instances,first,base,qualified?&value:nullptr);
    }
    static bool drawIndexed(CameraDrawAdapter& adapter,VkCommandBuffer command,
                     std::uint32_t count,std::uint32_t instances,std::uint32_t first,std::int32_t vertex,std::uint32_t base) noexcept {
        CameraDrawAdapter::Request value;
        const bool qualified=request(command,value);
        return adapter.drawIndexed(command,count,instances,first,vertex,base,qualified?&value:nullptr);
    }
};
} // namespace rdr95
