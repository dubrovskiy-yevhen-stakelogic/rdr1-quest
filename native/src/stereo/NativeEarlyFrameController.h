#pragma once
#include "NativeMainVisibilityRoute.h"
#include "NativeCurrentAdmission.h"
#include "CurrentNativeWallSources.h"
#include "EarlyStaticVisibility.h"
#include "NativeCurrentPassRaster.h"
#include "NativeQueryState.h"
#include "NativeEarlyImageRegistry.h"
#include "opaque-run/NativeOpaqueRun.h"
#include "OwnedWallHintWorker.h"
#include "engine/StaticStereoOcclusion.h"
#include <optional>

namespace rdrvr::experimental {
// Composition of real native adapters. RuntimeApi must be supplied
// by the actual GameXr frozen-scene owner; no default/always-true permissions.
// This controller is not an installed renderer: it requires the platform
// bindings and a complete native owner lease.
class NativeEarlyFrameController {
public:
    using Mask=EarlyStaticVisibility;using Cones=Mask::Cones;using Frame=Cones::Frame;
    using Native=rdr95::NativeCurrentAdmission;using Provider=rdr95::NativeCurrentRecordProvider;
    using DrawKey=engine::StaticStereoOcclusion::DrawKey;
    using Sources=CurrentNativeWallSources;
    struct Description {
        Frame frame{};std::uint64_t world{},resources{};VkDevice device{};
        std::array<Sources::Native::Camera,2> cameras{};unsigned width{},height{};
    };
    struct CurrentPass {
        std::uintptr_t gfx{},vk{};Sources::Witness::Target target{};
        NativeEarlyImageRegistry::Ticket imageTicket{};
        NativeOpaqueRun::Ticket opaqueRun{};
    };
    struct RuntimeApi {
        void* context{};
        // Actual native frame/list/resource lease and frozen pose. Thread ID,
        // ready-bit, unchanged raw pointers or an epoch alone are insufficient.
        bool(*frameCurrent)(void*,const Description&)noexcept{};
        // Derive current Gfx/Vk and exact pass-owned targets, and prove the
        // pending CPU camera constants match this frozen eye.
        bool(*currentPass)(void*,const Description&,Mask::Pass,CurrentPass&)noexcept{};
        // Short real resource-gate lease. Never held across original/native
        // calls. A pending writer declines this optimization without waiting.
        bool(*readBegin)(void*,const Description&)noexcept{};
        void(*readEnd)(void*)noexcept{};
        // Production publishes the bounded joined-list lease only after the
        // original native wait and provider header validation. Optional for
        // standalone authored fixtures; the platform binding requires it.
        bool(*mainJoined)(void*,const Description&,const rdr95::Ready&)noexcept{};
        void(*prepareJoinedRead)(void*,const Description&,std::uintptr_t,unsigned,std::uintptr_t)noexcept{};
        // Reuse only the geometry just prepared under this exact continuous
        // resource read. Only the production binding can issue this ticket.
        Native::PreparationTicket(*preparationTicket)(void*,const Description&)noexcept{};
    };
    struct Configuration {
        rdr95::Api reads{};rdr95::NativeSelectedLodGeometry::Api geometry{};
        rdr95::NativeCurrentFixedState::Api state{};NativeCurrentPassRaster::Api raster{};
        Sources::Native::Api source{};Sources::Witness::Api witness{};Sources::Registry* registry{};
        RuntimeApi runtime{};
        // Optional same-call scope scheduling (default off). False keeps the
        // lazy path, including its failure/retry behavior.
        bool earlyDrawScope{false};
    };
    explicit NativeEarlyFrameController(Configuration config):config_(config),boundary_(config.reads),worker_(exchange_) {
        sources_.configure(config.source,config.witness,config.registry);
    }
    NativeEarlyFrameController(const NativeEarlyFrameController&)=delete;
    ~NativeEarlyFrameController(){end();worker_.stop();}
    bool begin(const Description& description)noexcept;
    void wait(engine::MainVisibilityWait original,void* manager,unsigned request,std::uintptr_t offset){
        boundary_.wait(reinterpret_cast<std::uintptr_t>(manager),request,offset,[&]{original(manager,request);});
    }
    NativeMainVisibilityBoundary& mainBoundary()noexcept{return boundary_;}
    bool enter(Mask::Pass)noexcept;
    bool leave()noexcept;
    bool good()const noexcept;
    bool active()const noexcept{return owned()&&inside_&&prepared_&&!revoked_;}
    // Copied route identity only. This never reads native memory or grants a
    // resource/frame permit; the Binding still validates those before use.
    bool routeFrame(std::uint64_t& scene,std::uint64_t& pose,unsigned& stage)const noexcept {
        if(!active())return false;
        scene=description_.frame.scene;pose=description_.frame.pose;stage=unsigned(pass_);return true;
    }
    bool depth()const noexcept{return active()&&(pass_==Mask::Pass::LeftDepth||pass_==Mask::Pass::RightDepth);}
    bool complete()const noexcept{return owned()&&prepared_&&mask_.complete()&&good();}
    void end()noexcept;
    bool needsGeometryProof()const noexcept{return sources_.needsGeometryProof();}
    Sources& sources()noexcept{return sources_;}
    template<class Observe>bool sourceRead(Observe&& observe)noexcept {
        if(!owned()||!needsGeometryProof()||!config_.runtime.readBegin(config_.runtime.context,description_))return false;
        struct End{RuntimeApi& api;~End(){api.readEnd(api.context);}} end{config_.runtime};
        if(!frameCurrent())return false;
        observe(sources_);return frameCurrent();
    }
    const Mask::Stats& stats()const noexcept{return mask_.stats();}
    const Cones::Stats& conesStats()const noexcept{return cones_.stats();}
    std::uint64_t conesSourceRevision()const noexcept{return cones_.validSourceRevision();}
    std::uint64_t conesBuildSerial()const noexcept{return cones_.buildSerial();}
    const Native::Stats& nativeStats()const noexcept{return native_.statistics();}
    class Object {
        friend class NativeEarlyFrameController;
        NativeEarlyFrameController* owner_{};Object* previous_{};DrawKey key_{};
        std::optional<Sources::Object> source_;bool skip_{};
        CurrentPass current_{};bool passCaptured_{},borrowed_{};Mask::Pass pass_{};
        engine::NativeQueryTicket query_{};bool queryCaptured_{};
        std::optional<Native::DrawScope> validation_;
    public:
        Object(NativeEarlyFrameController&,const DrawKey&,bool exactMain)noexcept;
        ~Object(){validation_.reset();if(borrowed_)owner_->config_.runtime.readEnd(owner_->config_.runtime.context);source_.reset();if(owner_)owner_->object_=previous_;}
        Object(const Object&)=delete;
        bool skip()const noexcept{return skip_;}
        void complete()noexcept{
            if(source_&&owner_->config_.runtime.readBegin(owner_->config_.runtime.context,owner_->description_)){
                source_->complete();owner_->config_.runtime.readEnd(owner_->config_.runtime.context);
            }
        }
    };
private:
    Configuration config_;NativeMainVisibilityBoundary boundary_;Native native_;
    Description description_{};Provider* provider_{};Cones cones_;Mask mask_;Sources sources_;
    WallHintExchange exchange_;OwnedWallHintWorker worker_;WallHintExchange::Packet hints_{};
    std::array<Mask::Candidate,Mask::Capacity> candidates_{};
    std::array<Native::Permit,Mask::Capacity> permits_{};
    std::array<CurrentWallSeeds::Selection,Cones::MaxWalls> selected_{};
    std::array<unsigned,Cones::MaxWalls> pinned_{};unsigned candidatesCount_{},selectedCount_{};
    engine::StaticOcclusionProjection projection_;std::thread::id thread_{std::this_thread::get_id()};
    Object* object_{};Mask::List list_{};Mask::Pass pass_{};
    bool started_{},prepared_{},inside_{},revoked_{},skipped_{};std::uint64_t workerWorld_{},request_{};
    bool owned()const noexcept{return thread_==std::this_thread::get_id();}
    bool frameCurrent()const noexcept;
    bool frameCurrentStrong()const noexcept;
    bool joined(Provider&)noexcept;
    bool drawAllowed(unsigned,Mask::Family,std::uint64_t,Mask::Pass)noexcept;
    bool recordCurrent(unsigned,std::uint64_t)noexcept;
    bool beginDrawValidation()noexcept;
    static rdr95::NativeCurrentFixedState::Call nativeCall(const CurrentPass& current,const DrawKey& key)noexcept {
        float fade{};static_assert(sizeof fade==sizeof key.fadeBits);std::memcpy(&fade,&key.fadeBits,sizeof fade);
        return {current.gfx,current.vk,key.container,key.info,unsigned(key.commands),unsigned(key.instance),fade,key.flags,key.extra};
    }
    void updateHints();
};
inline bool NativeEarlyFrameController::begin(const Description& d)noexcept {
    if(!owned()||object_||inside_)return false;end();
    if(!d.frame.valid()||!d.world||!d.resources||!d.device||!d.width||!d.height||!config_.runtime.frameCurrent||
       !config_.runtime.currentPass||!config_.runtime.readBegin||!config_.runtime.readEnd||
       !sources_.configured()||!config_.runtime.frameCurrent(config_.runtime.context,d)||
       !projection_.configure(d.cameras,d.width,d.height))return false;
    description_=d;revoked_=skipped_=false;
    const bool begun=boundary_.beginFrame(d.frame.scene,{this,
        [](void* p,std::uint64_t f)noexcept{auto& s=*static_cast<NativeEarlyFrameController*>(p);return s.owned()&&f==s.description_.frame.scene&&s.config_.runtime.frameCurrent(s.config_.runtime.context,s.description_);},
        [](void* p,Provider& provider)noexcept{return static_cast<NativeEarlyFrameController*>(p)->joined(provider);},
        [](void* p,std::uint64_t)noexcept{static_cast<NativeEarlyFrameController*>(p)->revoked_=true;},
        [](void* p,std::uint64_t)noexcept{auto& s=*static_cast<NativeEarlyFrameController*>(p);return s.config_.runtime.readBegin(s.config_.runtime.context,s.description_);},
        [](void* p)noexcept{auto& s=*static_cast<NativeEarlyFrameController*>(p);s.config_.runtime.readEnd(s.config_.runtime.context);},
        [](void* p,std::uintptr_t manager,unsigned request,std::uintptr_t offset,std::uint64_t)noexcept{auto& s=*static_cast<NativeEarlyFrameController*>(p);
            if(s.config_.runtime.prepareJoinedRead)s.config_.runtime.prepareJoinedRead(s.config_.runtime.context,s.description_,manager,request,offset);}});
    started_=begun;return begun;
}
inline bool NativeEarlyFrameController::frameCurrentStrong()const noexcept {
    if(!owned()||!started_||revoked_||!provider_)return false;
    const auto* provider=provider_;
    const auto revision=provider->revision(),record=provider->recordRevision();
    const auto generation=native_.generation_;
    if(!revision||!native_.frameCurrent()||
       !config_.runtime.frameCurrent(config_.runtime.context,description_))return false;
    // External callbacks must not replace the admitted list or record and
    // publish a result for the previous one, even at the same address.
    return owned()&&started_&&!revoked_&&provider_==provider&&
        native_.provider_==provider&&native_.generation_==generation&&
        provider->revision()==revision&&provider->recordRevision()==record;
}
inline bool NativeEarlyFrameController::frameCurrent()const noexcept {
    // This is only an internal, synchronous proof walk after the first full
    // record validation. The retained original runs only after this adapter
    // releases its Read; covered resource writers wait for that release.
    // Public completion and the final mask commit always call the strong path.
    if(owned()&&started_&&!revoked_&&provider_&&inside_&&object_&&!object_->previous_&&
       object_->borrowed_&&object_->passCaptured_&&object_->validation_&&pass_==object_->pass_&&
       native_.qualifiedDrawCurrent(*object_->validation_,nativeCall(object_->current_,object_->key_)))return true;
    return frameCurrentStrong();
}
inline bool NativeEarlyFrameController::joined(Provider& provider)noexcept {
    if(!owned()||!started_||prepared_||!provider.ready())return false;
    const auto& ready=*provider.ready();if(!ready.recordCount||ready.recordCount>Mask::Capacity)return false;
    if(config_.runtime.mainJoined&&!config_.runtime.mainJoined(config_.runtime.context,description_,ready))return false;
    provider_=&provider;list_={ready.container,ready.records,ready.recordCount};
    try {
        const auto& f=description_.frame;
        Native::PreparationScope preparation(native_,config_.runtime.preparationTicket?
            config_.runtime.preparationTicket(config_.runtime.context,description_):Native::PreparationTicket{});
        if(!native_.begin(provider,config_.geometry,{f.scene,f.scene,f.pose,description_.resources}))return false;
        if(!cones_.build(f,projection_.matrices(),{},{this,
            [](void* p,const Frame& f)noexcept{auto& s=*static_cast<NativeEarlyFrameController*>(p);return f==s.description_.frame&&s.frameCurrent();},
            [](void* p,const Frame& f,const Cones::SourceKey& key)noexcept{return static_cast<NativeEarlyFrameController*>(p)->sources_.admitsCompletedSource(f,key);}}))return false;
        candidatesCount_=selectedCount_=0;
        updateHints();
        // Without a currently represented wall hint this frame cannot hide
        // anything. Keep metadata for the worker, but do not project/revalidate
        // every candidate merely to query an empty coverage set.
        if(selectedCount_&&!native_.candidates([&](const Native::Candidate& c){
            if(c.index>=Mask::Capacity)return false;
            auto& out=candidates_[candidatesCount_++];out={};out.index=c.index;
            switch(c.family){case rdr95::Family::StaticCommon:out.family=Mask::Family::StaticCommon;break;
                case rdr95::Family::RigidMatrixSet:out.family=Mask::Family::RigidMatrixSet;break;
                default:out.family=Mask::Family::Unknown;break;}
            out.bounds={{c.bounds.minimum[0],c.bounds.minimum[1],c.bounds.minimum[2]},
                        {c.bounds.maximum[0],c.bounds.maximum[1],c.bounds.maximum[2]}};
            if(c.proofKind==Native::Bounds::ProofKind::ConditionalAffineXYZ&&
               projection_.errors(out.bounds,{c.axisError[0],c.axisError[1],c.axisError[2]},out.errors)){
                permits_[c.index]=c.permit;out.permit=std::uint64_t(c.index)+1;
            }
            return true;
        }))return false;
        if(!sources_.begin(f,description_.device,description_.cameras,description_.width,description_.height,
              cones_,{selected_.data(),selectedCount_},{hints_.hints.data(),hints_.count}))return false;
        prepared_=mask_.prepare(f,list_,{candidates_.data(),candidatesCount_},{pinned_.data(),selectedCount_},cones_,{this,
            [](void* p,Frame f,Mask::List l)noexcept{auto& s=*static_cast<NativeEarlyFrameController*>(p);return f==s.description_.frame&&l==s.list_&&s.frameCurrent();},
            [](void* p,Frame,Mask::List,unsigned i,Mask::Family,std::uint64_t token)noexcept{return static_cast<NativeEarlyFrameController*>(p)->recordCurrent(i,token);},
            [](void* p,Frame,Mask::List,unsigned i,Mask::Family family,std::uint64_t token,Mask::Pass pass)noexcept{return static_cast<NativeEarlyFrameController*>(p)->drawAllowed(i,family,token,pass);},
            [](void* p)noexcept{const auto& s=*static_cast<NativeEarlyFrameController*>(p);
                return s.object_&&!s.object_->previous_&&s.object_->passCaptured_&&s.object_->queryCaptured_&&
                    (!s.object_->current_.imageTicket||s.object_->current_.imageTicket.current())&&
                    (!s.object_->current_.opaqueRun||s.object_->current_.opaqueRun.current())&&
                    engine::nativeQueriesCurrent(s.object_->query_);},
            [](void* p)noexcept{const auto& s=*static_cast<NativeEarlyFrameController*>(p);
                const auto* object=s.object_;
                if(!object||object->previous_||!s.inside_||s.pass_!=object->pass_||
                   !s.frameCurrentStrong())return false;
                // The platform frame callback is the last external check.
                // A pass transition inside it must not publish this old draw.
                return s.object_==object&&!object->previous_&&s.inside_&&s.pass_==object->pass_;}});
        preparation.invalidate();
        // The original wait lies INSIDE primary depth. Enter only after its
        // current list and complete bounds have been accepted by the mask.
        return prepared_&&enter(Mask::Pass::LeftDepth);
    }catch(...){revoked_=true;return false;}
}
inline void NativeEarlyFrameController::updateHints() {
    if(workerWorld_!=description_.world){workerWorld_=description_.world;hints_={};worker_.world(workerWorld_);}
    WallHintExchange::Packet latest;if(exchange_.tryRead(workerWorld_,hints_.request,latest))hints_=latest;
    // Source metadata is complete native rigid geometry, independent of whether
    // the CURRENT material technique qualifies that object as a candidate.
    // Reserve first: all accepted shared snapshots are retired on the worker.
    auto writer=request_==UINT64_MAX?OwnedWallHintWorker::Writer{}:
        worker_.tryBegin(workerWorld_,++request_,projection_.matrices());
    unsigned submitted{};
    native_.sourceMetadata([&](const auto& source){
        if(writer&&submitted<OwnedWallHintWorker::MaxObjects){
            OwnedWallHintWorker::Selector::Matrix world{};world[15]=1;
            // Native compact columns -> selector row-major affine matrix.
            // Passing native column-major translation as row3 rejects the
            // entire worker scene as projective instead of moving the object.
            for(unsigned c=0;c<4;++c)for(unsigned r=0;r<3;++r)world[r*4+c]=source.world[c*3+r];
            // Snapshot is move-only. Obtain a second owned reference from the
            // cache only after reserving the worker slot; no vertex copy.
            engine::OwnedStaticOccluders::Snapshot copied;
            if(config_.geometry.cache&&config_.geometry.cache->querySnapshot(source.mesh->epoch,source.mesh->identity,copied)&&
               copied.generation==source.mesh->generation&&
               writer.append({source.node,source.drawable},std::move(copied),world,OwnedWallHintWorker::Selector::Faces::Both))++submitted;
        }
        for(unsigned h=0;h<hints_.count&&selectedCount_<selected_.size();++h){const auto& hint=hints_.hints[h];
            if(hint.object!=source.node||hint.objectGeneration!=source.drawable||hint.geometry!=source.mesh->epoch||
               hint.geometryGeneration!=source.mesh->generation)continue;
            const auto record=list_.records+source.index*Mask::RecordStride;
            const CurrentWallSeeds::Selection next{source.node,hint.geometry,hint.geometryGeneration,record,hint.patch};
            bool duplicate=false;for(unsigned i=0;i<selectedCount_;++i){const auto& prior=selected_[i];duplicate|=prior.object==next.object&&prior.geometry==next.geometry&&prior.geometryGeneration==next.geometryGeneration&&prior.record==next.record&&prior.patch==next.patch;}
            if(!duplicate){selected_[selectedCount_]=next;pinned_[selectedCount_++]=source.index;}
        }
        return true;
    });
    if(writer)writer.commit();
}
inline bool NativeEarlyFrameController::enter(Mask::Pass pass)noexcept {
    if(!owned()||inside_||!prepared_||!frameCurrent())return false;
    if(!mask_.enter(description_.frame,pass)){revoked_=true;return false;}
    pass_=pass;inside_=true;
    return true;
}
inline bool NativeEarlyFrameController::leave()noexcept {
    if(!owned())return false;if(!inside_)return !skipped_;inside_=false;
    if(!mask_.leave())revoked_=true;return good();
}
inline bool NativeEarlyFrameController::good()const noexcept {
    return owned()&&(!skipped_||(!revoked_&&!mask_.stats().poisoned&&frameCurrentStrong()));
}
inline void NativeEarlyFrameController::end()noexcept {
    if(!owned()||object_)return;
    if(inside_)leave();sources_.end();mask_.reset();cones_.reset();native_.end();boundary_.endFrame();
    provider_=nullptr;started_=prepared_=inside_=false;list_={};
}
inline NativeEarlyFrameController::Object::Object(NativeEarlyFrameController& s,const DrawKey& key,bool exact)noexcept {
    if(!s.owned())return;owner_=&s;previous_=s.object_;s.object_=this;key_=key;pass_=s.pass_;
    if(previous_||!exact||!s.inside_||!s.prepared_)return;
    const bool hinted=s.config_.earlyDrawScope&&s.mask_.knownHidden(key.container,key.info);
    // A stale hint can only cause an earlier proof attempt. On failed Read or
    // pass capture retain the original, with no native record walk or retry.
    // Successfully opening the scope does not mark its record checked/accepted.
    const bool attempt=!hinted||s.beginDrawValidation();
    skip_=attempt&&s.mask_.commonSkip(key.container,key.info);s.skipped_|=skip_;
    // Reuse validation only across the synchronous, read-only skip decision.
    // Source observation and the original native draw must never inherit it.
    validation_.reset();passCaptured_=false;
    if(borrowed_){s.config_.runtime.readEnd(s.config_.runtime.context);borrowed_=false;}
    if(!skip_&&s.pass_==Mask::Pass::LeftDepth){
        // The wait can precede the actual native target bind. Refresh only for
        // a selected source, at its draw entry, rather than trusting wait-time
        // framebuffer state or paying the camera/raster walk for every object.
        bool selected=false;
        for(unsigned i=0;i<s.selectedCount_;++i)selected|=s.selected_[i].record==key.info;
        if(selected&&s.config_.runtime.readBegin(s.config_.runtime.context,s.description_)){CurrentPass actual;
            if(s.config_.runtime.currentPass(s.config_.runtime.context,s.description_,s.pass_,actual)){
                s.sources_.target(actual.target);
                source_.emplace(s.sources_,reinterpret_cast<const void*>(key.info),true);
            }
            s.config_.runtime.readEnd(s.config_.runtime.context);
        }
    }
}
inline bool NativeEarlyFrameController::beginDrawValidation()noexcept {
    if(!object_||object_->previous_)return false;
    if(!object_->borrowed_){
        object_->borrowed_=config_.runtime.readBegin(config_.runtime.context,description_);
        if(!object_->borrowed_)return false;
    }
    if(!object_->validation_&&inside_&&
       config_.runtime.currentPass(config_.runtime.context,description_,pass_,object_->current_)){
        object_->passCaptured_=true;
        object_->validation_.emplace(native_,nativeCall(object_->current_,object_->key_),config_.runtime.preparationTicket?
            config_.runtime.preparationTicket(config_.runtime.context,description_):Native::PreparationTicket{});
    }
    return object_->passCaptured_&&object_->validation_.has_value();
}
inline bool NativeEarlyFrameController::recordCurrent(unsigned index,std::uint64_t token)noexcept {
    if(index>=Mask::Capacity||token!=std::uint64_t(index)+1)return false;
    if(object_&&!object_->previous_){
        // Keep the baseline lazy fallback when pass capture fails. The early
        // failed attempt never reaches this callback; its native draw is kept.
        beginDrawValidation();if(!object_->borrowed_)return false;
    }
    return native_.recordCurrent(index,permits_[index]);
}
inline bool NativeEarlyFrameController::drawAllowed(unsigned index,Mask::Family,std::uint64_t token,Mask::Pass pass)noexcept {
    if(!owned()||!object_||object_->previous_||!inside_||pass!=pass_||token!=std::uint64_t(index)+1||!frameCurrent())return false;
    const auto& key=object_->key_;
    if(key.info!=list_.records+index*Mask::RecordStride||key.container!=list_.container||key.commands<0||key.instance<0)return false;
    CurrentPass current=object_->current_;
    if(!object_->passCaptured_&&!config_.runtime.currentPass(config_.runtime.context,description_,pass,current))return false;
    rdr95::NativeCurrentFixedState::Result fixed;
    if(!native_.fixedState(config_.state,nativeCall(current,key),permits_[index],fixed))return false;
    engine::NativeQueryTicket query;
    if(!engine::nativeQueriesQuiet(engine::queryHandle(description_.device),fixed.command,query))return false;
    NativeCurrentPassRaster::Result raster;
    if(!NativeCurrentPassRaster::capture(config_.raster,{current.gfx,current.vk,fixed.framebuffer,fixed.command},current.target,raster,&fixed.snapshot))return false;
    if(!engine::nativeQueriesCurrent(query)||!frameCurrent())return false;
    // The final record/frame checks still follow this function. Retain the
    // exact query receipt and verify it after them, at the actual skip commit.
    object_->query_=query;object_->queryCaptured_=true;return true;
}
}
