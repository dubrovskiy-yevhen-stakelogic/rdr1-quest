#include "StaticStereoOcclusion.h"
#include <array>
#include <cerrno>
#include <cmath>
#include <mutex>

namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
std::uint64_t mix(std::uint64_t n) noexcept {
    n^=n>>30;n*=0xbf58476d1ce4e5b9ULL;n^=n>>27;n*=0x94d049bb133111ebULL;return n^(n>>31);
}
bool finiteBounds(const StaticStereoOcclusion::Object& o)noexcept{
    const auto& a=o.bounds.minimum;const auto& b=o.bounds.maximum;
    if(!std::isfinite(a.x)||!std::isfinite(a.y)||!std::isfinite(a.z)||
       !std::isfinite(b.x)||!std::isfinite(b.y)||!std::isfinite(b.z)||a.x>b.x||a.y>b.y||a.z>b.z)return false;
    for(auto x:o.world)if(!std::isfinite(x))return false;
    const auto& e=o.worldError;
    if(!std::isfinite(e.x)||!std::isfinite(e.y)||!std::isfinite(e.z)||e.x<0||e.y<0||e.z<0)return false;
    return true;
}
}
struct StaticStereoOcclusion::Impl {
    enum class Phase {Idle,LeftDepth,LeftColor,Sealed,RightDepth,RightColor,Fault};
    struct Entry {
        std::uint64_t epoch{};Object object{};
        // A changed object invalidates both passes. A shader/primitive failure
        // invalidates only that pass: color cannot revoke a complete depth proof.
        bool bad{},depth{},color{},queried{},hidden{};
        unsigned badPasses{};
        bool eligible(Pass p)const noexcept {
            return !bad&&!(badPasses&(1u<<unsigned(p)))&&(p==Pass::Depth?depth:color);
        }
    };
    struct Call {std::uint64_t epoch{};DrawKey key{};unsigned object{};Pass pass{};bool complete{},bad{};};
    std::array<Entry,MaxObjects> objects{};
    std::array<Call,MaxCalls> calls{};
    std::uint64_t frame{},epoch{};std::thread::id thread{};Phase phase{Phase::Idle};
    Reject query{};void* user{};Stats stats{},published{};mutable std::mutex mutex;
    bool left()const noexcept{return phase==Phase::LeftDepth||phase==Phase::LeftColor;}
    Pass pass()const noexcept{return phase==Phase::LeftDepth||phase==Phase::RightDepth?Pass::Depth:Pass::Color;}
    void publish()noexcept{std::lock_guard lock(mutex);published=stats;}
    unsigned object(const Object& o,bool create)noexcept{
        const auto first=unsigned(mix(o.node)^mix(o.drawable))&(MaxObjects-1);
        for(unsigned p=0;p<64;++p){const auto index=(first+p)&(MaxObjects-1);auto& e=objects[index];
            if(e.epoch!=epoch){if(!create)return MaxObjects;e={};e.epoch=epoch;e.object=o;return index;}
            if(e.object.node==o.node&&e.object.drawable==o.drawable){
                if(!NativeStaticOcclusionSource::sameObject(e.object,o)){e.bad=true;++stats.changedObjects;return MaxObjects;}
                return index;
            }
        }
        if(create)++stats.budgetStops;return MaxObjects;
    }
    unsigned call(const DrawKey& k,Pass pass,bool create)noexcept{
        const auto first=unsigned(mix(k.info)^mix(k.container)^mix(std::uint64_t(k.fadeBits)<<32|k.flags)^mix(k.extra)^mix(unsigned(k.commands))^
                                  mix(unsigned(k.instance))^mix(unsigned(pass)))&(MaxCalls-1);
        for(unsigned p=0;p<64;++p){const auto index=(first+p)&(MaxCalls-1);auto& c=calls[index];
            if(c.epoch!=epoch){if(!create)return MaxCalls;c={};c.epoch=epoch;c.key=k;c.pass=pass;c.object=MaxObjects;return index;}
            if(c.pass==pass&&c.key==k)return index;
        }
        if(create)++stats.budgetStops;return MaxCalls;
    }
};
StaticStereoOcclusion::StaticStereoOcclusion():impl_(std::make_unique<Impl>()){}
StaticStereoOcclusion::~StaticStereoOcclusion()=default;
bool StaticStereoOcclusion::ownerThread()const noexcept{return impl_->thread==std::this_thread::get_id();}
void StaticStereoOcclusion::configure(Reject query,void* user)noexcept{
    Errno preserve;if(impl_->phase!=Impl::Phase::Idle&&!ownerThread())return;
    end();impl_->query=query;impl_->user=user;
}
bool StaticStereoOcclusion::begin(std::uint64_t frame)noexcept{
    Errno preserve;auto& i=*impl_;
    if(ObjectScope::current_){poison(*ObjectScope::current_);++i.stats.invalidStages;return false;}
    if(i.phase!=Impl::Phase::Idle){++i.stats.invalidStages;i.phase=Impl::Phase::Fault;return false;}
    if(!frame||!i.query)return false;
    if(++i.epoch==0){for(auto& e:i.objects)e.epoch=0;for(auto& c:i.calls)c.epoch=0;++i.epoch;}
    i.frame=frame;i.thread=std::this_thread::get_id();i.phase=Impl::Phase::LeftDepth;++i.stats.frames;return true;
}
bool StaticStereoOcclusion::visitSourceTargets(std::uint64_t frame,TargetVisitor visitor,void* user)const noexcept{
    Errno preserve;const auto& i=*impl_;
    if(!ownerThread()||!visitor||!frame||frame!=i.frame||i.phase!=Impl::Phase::LeftDepth||ObjectScope::current_)return false;
    for(const auto& e:i.objects)if(e.epoch==i.epoch&&!e.bad&&e.object.valid)visitor(user,e.object);
    return true;
}
bool StaticStereoOcclusion::leftColor(std::uint64_t frame)noexcept{
    Errno preserve;auto& i=*impl_;
    if(!ownerThread())return false;
    if(frame!=i.frame||i.phase!=Impl::Phase::LeftDepth||ObjectScope::current_){
        if(ObjectScope::current_)poison(*ObjectScope::current_);++i.stats.invalidStages;i.phase=Impl::Phase::Fault;return false;}
    i.phase=Impl::Phase::LeftColor;return true;
}
bool StaticStereoOcclusion::seal(std::uint64_t frame)noexcept{
    Errno preserve;auto& i=*impl_;
    if(!ownerThread())return false;
    if(frame!=i.frame||i.phase!=Impl::Phase::LeftColor||ObjectScope::current_){
        if(ObjectScope::current_)poison(*ObjectScope::current_);++i.stats.invalidStages;i.phase=Impl::Phase::Fault;return false;}
    i.phase=Impl::Phase::Sealed;
    for(const auto& e:i.objects)if(e.epoch==i.epoch){
        const bool depth=e.eligible(Pass::Depth),color=e.eligible(Pass::Color);
        i.stats.eligibleObjects+=depth||color;i.stats.eligibleDepth+=depth;i.stats.eligibleColor+=color;
    }
    i.publish();return true;
}
bool StaticStereoOcclusion::colorMayBeHidden(std::uint64_t frame,const Object& fresh)noexcept{
    Errno preserve;auto& i=*impl_;
    if(!ownerThread()||frame!=i.frame||i.phase!=Impl::Phase::LeftColor||!fresh.valid||fresh.frame!=frame)return true;
    const auto oi=i.object(fresh,false);if(oi==MaxObjects)return true;
    auto& e=i.objects[oi];if(!e.eligible(Pass::Depth))return true;
    if(!e.queried){++i.stats.queries;e.hidden=i.query(i.user,frame,fresh);e.queried=true;}
    else ++i.stats.queryHits;
    return e.hidden;
}
bool StaticStereoOcclusion::beginRight(std::uint64_t frame,Pass pass)noexcept{
    Errno preserve;auto& i=*impl_;
    if(!ownerThread())return false;
    const bool order=(pass==Pass::Depth&&i.phase==Impl::Phase::Sealed)||
                     (pass==Pass::Color&&i.phase==Impl::Phase::RightDepth);
    if(frame!=i.frame||!order||ObjectScope::current_){
        if(ObjectScope::current_)poison(*ObjectScope::current_);++i.stats.invalidStages;i.phase=Impl::Phase::Fault;return false;}
    i.phase=pass==Pass::Depth?Impl::Phase::RightDepth:Impl::Phase::RightColor;return true;
}
StaticStereoOcclusion::ObjectScope::ObjectScope(StaticStereoOcclusion& owner,const DrawKey& key,const Object& object,bool qualified)noexcept:
    owner_(&owner),previous_(current_){
    Errno preserve;auto& i=*owner.impl_;
    if(!owner.ownerThread()||!i.left())return;
    active_=true;epoch_=i.epoch;pass_=i.pass();current_=this;++i.stats.leftScopes;
    if(previous_){owner.poison(*previous_);safe_=false;++i.stats.nested;}
    // Invalid repeated records must poison an earlier proof of that exact call.
    call_=i.call(key,i.pass(),false);
    if(!qualified||!object.valid||object.frame!=i.frame||!object.node||!object.drawable||!key.container||!key.info||!finiteBounds(object)){
        safe_=false;++i.stats.declined;
        if(call_<MaxCalls){auto& c=i.calls[call_];c.bad=true;if(c.object<MaxObjects)i.objects[c.object].badPasses|=1u<<unsigned(pass_);}
        return;
    }
    object_=i.object(object,true);
    if(object_==MaxObjects){safe_=false;return;}
    if(!safe_)i.objects[object_].badPasses|=1u<<unsigned(pass_);
    if(call_==MaxCalls)call_=i.call(key,i.pass(),true);
    if(call_==MaxCalls){i.objects[object_].badPasses|=1u<<unsigned(pass_);safe_=false;return;}
    auto& c=i.calls[call_];
    if(c.object!=MaxObjects&&c.object!=object_){i.objects[c.object].bad=true;i.objects[object_].bad=true;c.bad=true;safe_=false;}
    c.object=object_;
}
StaticStereoOcclusion::ObjectScope::~ObjectScope(){
    if(active_){owner_->finish(*this,false);current_=previous_;}
}
void StaticStereoOcclusion::ObjectScope::complete()noexcept{if(active_)owner_->finish(*this,true);}
void StaticStereoOcclusion::poison(ObjectScope& scope)noexcept{
    if(!scope.active_||!scope.owner_)return;
    scope.safe_=false;auto& i=*scope.owner_->impl_;
    if(scope.epoch_==i.epoch&&scope.object_<MaxObjects)i.objects[scope.object_].badPasses|=1u<<unsigned(scope.pass_);
}
void StaticStereoOcclusion::finish(ObjectScope& scope,bool completed)noexcept{
    Errno preserve;if(scope.finished_)return;scope.finished_=true;auto& i=*impl_;
    if(scope.epoch_!=i.epoch||!ownerThread()||!i.left()||ObjectScope::current_!=&scope){poison(scope);return;}
    const bool good=completed&&scope.safe_&&scope.draws_&&scope.object_<MaxObjects&&scope.call_<MaxCalls;
    if(scope.object_<MaxObjects&&!good)i.objects[scope.object_].badPasses|=1u<<unsigned(scope.pass_);
    if(scope.call_<MaxCalls){auto& c=i.calls[scope.call_];c.complete|=good;c.bad|=!good;}
    if(good){++i.stats.completedScopes;auto& e=i.objects[scope.object_];if(i.pass()==Pass::Depth)e.depth=true;else e.color=true;}
}
void StaticStereoOcclusion::observe(bool safe)noexcept{
    auto* scope=ObjectScope::current_;
    if(!scope||scope->owner_!=this||!scope->active_)return;
    auto& i=*impl_;if(scope->epoch_!=i.epoch||scope->finished_||!i.left()){poison(*scope);return;}
    ++scope->draws_;++i.stats.primitives;
    if(!safe){++i.stats.unsafePrimitives;poison(*scope);}
}
bool StaticStereoOcclusion::candidate(std::uint64_t frame,Pass pass,const DrawKey& key)const noexcept{
    Errno preserve;auto& i=*impl_;
    if(!ownerThread()||ObjectScope::current_||frame!=i.frame||
       !((pass==Pass::Depth&&i.phase==Impl::Phase::RightDepth)||(pass==Pass::Color&&i.phase==Impl::Phase::RightColor)))return false;
    const auto ci=i.call(key,pass,false);if(ci==MaxCalls)return false;
    const auto& c=i.calls[ci];if(!c.complete||c.bad||c.object>=MaxObjects)return false;
    const auto& e=i.objects[c.object];
    // A cached visible result can only decline the optimization. The native
    // draw remains intact, even if its object changed after LEFT. Hidden and
    // unknown entries still require fresh metadata and per-pass quiescence.
    return e.epoch==i.epoch&&e.eligible(pass)&&(!e.queried||e.hidden);
}
bool StaticStereoOcclusion::reject(std::uint64_t frame,Pass pass,const DrawKey& key,const Object& object,Reject liveCheck,void* liveUser)noexcept{
    Errno preserve;auto& i=*impl_;
    if(!ownerThread()||ObjectScope::current_||frame!=i.frame||!object.valid||object.frame!=frame||
       !((pass==Pass::Depth&&i.phase==Impl::Phase::RightDepth)||(pass==Pass::Color&&i.phase==Impl::Phase::RightColor)))return false;
    const auto ci=i.call(key,pass,false);if(ci==MaxCalls){++i.stats.declined;return false;}
    auto& c=i.calls[ci];if(!c.complete||c.bad||c.object>=MaxObjects){++i.stats.declined;return false;}
    auto& e=i.objects[c.object];
    if(e.epoch==i.epoch&&!NativeStaticOcclusionSource::sameObject(e.object,object)){
        e.bad=true;++i.stats.changedObjects;
    }
    if(e.epoch!=i.epoch||!e.eligible(pass)){
        ++i.stats.declined;return false;}
    if(!e.queried){++i.stats.queries;e.hidden=i.query(i.user,frame,object);e.queried=true;}
    else ++i.stats.queryHits;
    if(!e.hidden)return false;
    if(liveCheck&&!liveCheck(liveUser,frame,object)){
        e.bad=true;++i.stats.declined;return false;
    }
    if(pass==Pass::Depth)++i.stats.skippedDepth;else ++i.stats.skippedColor;return true;
}
void StaticStereoOcclusion::end()noexcept{
    Errno preserve;auto& i=*impl_;
    if(i.phase==Impl::Phase::Idle)return;
    if(!ownerThread())return;
    if(ObjectScope::current_&&ObjectScope::current_->owner_==this)poison(*ObjectScope::current_);
    i.phase=Impl::Phase::Idle;i.frame=0;i.publish();
}
StaticStereoOcclusion::Stats StaticStereoOcclusion::snapshot()const noexcept{
    Errno preserve;std::lock_guard lock(impl_->mutex);return impl_->published;
}
}
