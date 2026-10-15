#include "NativeEarlyRuntimeBinding.h"
#include "OrdinaryIntegration.h"
#include "SceneInputTape.h"
#include "SceneRouteTape.h"

namespace rdrvr::experimental {

NativeMainVisibilityBoundary::Api NativeEarlyRuntimeBinding::ordinaryBoundaryApi()noexcept {
    if(!owned()||!configured_)return {};
    return {this,
        [](void*p,std::uint64_t frame)noexcept{auto&s=*static_cast<NativeEarlyRuntimeBinding*>(p);return frame==s.description_.frame.scene&&s.frameCurrent();},
        [](void*p,NativeMainVisibilityBoundary::Provider& provider)noexcept{
            auto&s=*static_cast<NativeEarlyRuntimeBinding*>(p);
            if(!s.owned()||!provider.current()||!provider.ready())return false;
            const auto revision=provider.revision(),recordRevision=provider.recordRevision();const auto ready=*provider.ready();
            const bool joined=mainJoined(&s,s.description_,ready)&&provider.current()&&provider.revision()==revision&&provider.recordRevision()==recordRevision;
            if(joined&&rdr95::scene_input::begin()){
                auto& tape=rdr95::scene_input::captured;const auto& d=s.description_;
                tape.scene=d.frame.scene;tape.pose=d.frame.pose;tape.width=d.width;tape.height=d.height;
                for(unsigned eye=0;eye<2;++eye){std::copy(d.cameras[eye].relativeClip.begin(),d.cameras[eye].relativeClip.end(),tape.cameras.begin()+eye*32);
                    std::copy(d.cameras[eye].inverseView.begin(),d.cameras[eye].inverseView.end(),tape.cameras.begin()+eye*32+16);}
                if(const auto* memory=s.production_.memory())tape.decode(*memory,ready.manager);
                tape.leaseCurrent=s.borrowed()&&provider.current()&&provider.revision()==revision&&provider.recordRevision()==recordRevision&&s.frameCurrent();
                rdr95::scene_input::publish();
                if(tape.leaseCurrent&&tape.inventory.complete&&!tape.overflow&&!tape.exception)
                    rdr95::scene_routes::begin(d.frame.scene,d.frame.pose,s.setup_.bootstrap->image());
            }
            return joined;
        },
        [](void*p,std::uint64_t frame)noexcept{auto&s=*static_cast<NativeEarlyRuntimeBinding*>(p);if(s.owned()&&frame==s.description_.frame.scene)s.end();},
        [](void*p,std::uint64_t frame)noexcept{auto&s=*static_cast<NativeEarlyRuntimeBinding*>(p);return frame==s.description_.frame.scene&&readBegin(&s,s.description_);},
        readEnd,nullptr};
}
NativeEarlyRuntimeBinding::OrdinaryRead::OrdinaryRead(NativeEarlyRuntimeBinding& s)noexcept {
    const int saved=errno;
    if(!s.owned()||!s.configured_||!s.started_||!s.joined_||s.ordinary_||s.readDepth_||
       !s.setup_.physicalCopies||!s.setup_.platform.geometryStorageCurrent||!s.setup_.ordinaryGeometryEpoch){errno=saved;return;}
    diagnostic_=2;geometry_=s.setup_.ordinaryGeometryEpoch->observeMetadata();
    if(!geometry_.current()){errno=saved;return;}
    diagnostic_=3;owner_=&s;s.ordinary_=this;
    // The active marker is published BEFORE any external callback. Recursive
    // scope construction declines; generic readBegin retires this attempt.
    const auto initial=s.readSerial_;
    if(s.frameCurrent()&&s.ordinary_==this&&s.readSerial_==initial&&!s.readDepth_){
        diagnostic_=4;s.advanceReadSerial();
        if(s.readSerial_&&s.readSerial_!=UINT64_MAX){
            const auto acquired=s.readSerial_;
            diagnostic_=5;s.read_.emplace(*s.setup_.resources);
            if(*s.read_&&s.read_->generation()==s.resourceTicket_&&s.frameCurrent()&&
               s.ordinary_==this&&s.readSerial_==acquired&&s.read_&&!s.readDepth_){
                diagnostic_=6;s.readDepth_=1;s.qualifiedSerial_=acquired;s.qualifiedRead_=std::addressof(*s.read_);
                s.copyScope_.activate(&s,*s.read_,acquired);serial_=acquired;++s.stats_.readBegins;
                if(!current())serial_=0;else diagnostic_=0;
            }
        }
    }
    if(!serial_&&s.ordinary_==this){s.ordinary_=nullptr;s.advanceReadSerial();s.readDepth_=0;s.read_.reset();}
    errno=saved;
}
NativeEarlyRuntimeBinding::OrdinaryRead::~OrdinaryRead(){
    const int saved=errno;
    // end/rebegin may have installed another scope at the same Read address.
    // The retired destructor must never close that newer root.
    if(owner_&&owner_->owned()&&owner_->ordinary_==this){
        owner_->ordinary_=nullptr;owner_->advanceReadSerial();owner_->readDepth_=0;owner_->read_.reset();
    }
    errno=saved;
}
bool NativeEarlyRuntimeBinding::OrdinaryRead::live()const noexcept {
    if(!owner_||!owner_->owned()||!serial_||serial_==UINT64_MAX)return false;
    const auto& s=*owner_;
    return geometry_.current()&&s.ordinary_==this&&s.readSerial_==serial_&&s.joined_&&
        drawQualificationLive(&s,s.read_?std::addressof(*s.read_):nullptr,serial_);
}
bool NativeEarlyRuntimeBinding::OrdinaryRead::current()const noexcept {
    const int saved=errno;
    bool result=false;
    if(live()){
        const auto& s=*owner_;
        if(s.setup_.platform.geometryStorageCurrent(s.setup_.platform.context,s.description_)&&live()&&s.frameCurrent()){
            // Final callback may synchronously revoke/rebegin. All terminal
            // semantic/physical checks occur after it; no callback follows.
            result=live()&&s.setup_.bootstrap->ready()&&s.setup_.resources->frameRevisionCurrent(s.resourceTicket_)&&
                s.setup_.effects->current(s.effectTicket_)&&s.setup_.registry->currentShaderSet(s.shaderTicket_)&&
                s.config_.geometry.cache->invalidationSerial()==s.meshTicket_&&
                s.setup_.list->sceneActive(s.renderer_,s.sceneViewport_)&&s.setup_.list->current(s.listTicket_);
        }
    }
    errno=saved;return result;
}
bool NativeEarlyRuntimeBinding::OrdinaryRead::identity(Identity& out)const noexcept {
    if(!current())return false;
    const auto& s=*owner_;out={serial_,s.description_.frame.scene,s.resourceTicket_,s.effectTicket_,s.shaderTicket_,s.meshTicket_};return true;
}
bool NativeEarlyRuntimeBinding::OrdinaryRead::semanticStamp(OrdinaryEpoch& out,Address vertex,Address index)const noexcept {
    const int saved=errno;
    if(!current()||!owner_->setup_.ordinaryColdOwners){errno=saved;return false;}
    const auto& s=*owner_;OrdinaryEpoch next;
    next.owner_=&s;next.frame_=s.ordinaryFrame_;next.resource_=s.resourceTicket_;next.effect_=s.effectTicket_;
    next.shader_=s.shaderTicket_;next.mesh_=s.meshTicket_;next.list_=s.listTicket_;
    next.geometry_=(vertex||index)?s.setup_.ordinaryGeometryEpoch->observePair(vertex,index):s.setup_.ordinaryGeometryEpoch->observe();
    // Full mapped slot ownership is checked ONCE at issuance, under Read.
    next.cold_=s.setup_.ordinaryColdOwners->observe();
    const bool ok=current()&&next.current();
    if(ok)out=next;errno=saved;return ok;
}
bool NativeEarlyRuntimeBinding::OrdinaryEpoch::current()const noexcept {
    if(!owner_||!owner_->owned()||!frame_||frame_==UINT64_MAX)return false;
    const auto& s=*owner_;
    return s.configured_&&s.started_&&s.joined_&&s.ordinaryFrame_==frame_&&s.setup_.bootstrap->ready()&&
        s.resourceTicket_==resource_&&s.effectTicket_==effect_&&s.shaderTicket_==shader_&&s.meshTicket_==mesh_&&
        geometry_.current()&&cold_.current()&&!s.setup_.resources->writers()&&s.setup_.resources->frameRevisionCurrent(resource_)&&
        s.setup_.effects->current(effect_)&&s.setup_.registry->currentShaderSet(shader_)&&
        s.config_.geometry.cache->invalidationSerial()==mesh_&&s.setup_.list->current(list_)&&
        cold_.current()&&s.setup_.resources->frameRevisionCurrent(resource_)&&!s.setup_.resources->writers()&&geometry_.current();
}
bool NativeEarlyRuntimeBinding::ordinaryCopy(void* p,Address a,void* out,std::size_t n)noexcept {
    if(!p)return false;auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);
    if(!s.owned()||!s.ordinary_||!s.ordinary_->live())return false;
    // Strict physical copy: Unavailable never enters a diagnostic pipe or
    // guarded read. Current semantic epochs are checked at boundary/commit.
    const bool copied=physicalCopy(&s,a,out,n)==NativeEarlyReadRouter::CopyResult::Copied;
    return copied&&s.ordinary_&&s.ordinary_->live();
}
bool NativeEarlyRuntimeBinding::ordinaryIdentity(void* p,OrdinaryRead::Identity& out)noexcept {
    if(!p)return false;const auto& s=*static_cast<const NativeEarlyRuntimeBinding*>(p);
    return s.owned()&&s.ordinary_&&s.ordinary_->identity(out);
}

NativeEarlyRuntimeBinding::NativeEarlyRuntimeBinding()noexcept:
    router_({this,guarded,resourceBorrow,listBorrow}){}
bool NativeEarlyRuntimeBinding::configure(const Setup& s){
    if(!owned()||started_||readDepth_)return false;
    configured_=false;config_={};production_.clear();cameraProof_={};cameraMemo_.reset();
    if(!s.bootstrap||!s.bootstrap->ready()||!s.resources||!s.list||!s.effects||!s.registry||!s.device||
       !s.world||!s.resourceGeneration||!s.owner.identity||!s.owner.generation||!s.matrixBuffer||
       !s.platform.guarded||!s.platform.frozenCurrent||!s.platform.cachedPass)return false;
    const auto* source=s.bootstrap->source();const auto* witness=s.bootstrap->witness();
    if(!source||!witness||!source->triangles.ownedOccluders||!source->triangles.ownedEpoch)return false;
    setup_=s;router_.copy_=s.physicalCopies?physicalCopy:nullptr;
    rdr95::NativeAdmissionProduction::Dependencies deps;
    deps.memory={s.physicalCopies?NativeEarlyReadRouter::physicalNativeRead:NativeEarlyReadRouter::nativeRead,&router_};
    deps.memory.ordinaryMethod=rdr95::ordinary_integration::method;
    deps.registry=s.registry;deps.device=s.device;deps.meshes=source->triangles.ownedOccluders;
    deps.meshEpoch=source->triangles.ownedEpoch;deps.effects=s.effects;deps.matrixBuffer=s.matrixBuffer;
    deps.owner=s.owner;deps.context=this;deps.frameCurrent=nativeFrame;deps.materialCurrent=material;deps.matrixSetCurrent=matrixSetCurrent;
    if(!production_.configure(s.bootstrap->image(),s.bootstrap->imageProof(),deps))return false;
    cameraApi_={&router_,s.physicalCopies?NativeEarlyReadRouter::physicalSourceRead:NativeEarlyReadRouter::sourceRead,s.bootstrap->image(),this,cameraOwner};
    if(!NativeCurrentCamera::prove(cameraApi_,cameraProof_))return false;
    config_.reads=*production_.memory();config_.geometry=*production_.geometry();config_.state=*production_.fixedState();
    config_.source=*source;config_.witness=*witness;config_.registry=s.registry;
    const auto sourceReader=s.physicalCopies?NativeEarlyReadRouter::physicalSourceRead:NativeEarlyReadRouter::sourceRead;
    config_.source.read=sourceReader;config_.source.borrowedRead=sourceReader;
    config_.source.context=&router_;config_.source.triangles.read=sourceReader;config_.source.triangles.context=&router_;
    config_.raster={&router_,sourceReader,config_.state.gfxVtable,config_.state.vkVtable,witness->framebufferVtable};
    config_.runtime={this,frame,currentPass,readBegin,readEnd,mainJoined,prepareJoinedRead,preparationTicket};
    configured_=true;return true;
}
bool NativeEarlyRuntimeBinding::start(Description& d,Address renderer,Address viewport)noexcept{
    if(!owned()||!configured_||started_||readDepth_||!renderer||!viewport||!d.frame.valid()||
       d.world!=setup_.world||d.resources!=setup_.resourceGeneration||d.device!=setup_.device||!d.width||!d.height||
       !setup_.list->sceneActive(renderer,viewport))return false;
    rdr95::NativeResourceGate::Read read(*setup_.resources);
    if(!read)return false;
    resourceTicket_=read.generation();effectTicket_=setup_.effects->ticket();shaderTicket_=setup_.registry->shaderMutationSerial();
    meshTicket_=config_.geometry.cache->invalidationSerial();
    if(!effectTicket_||!shaderTicket_||!meshTicket_)return false;
    if(ordinaryFrame_==UINT64_MAX)return false;
    ++ordinaryFrame_;
    description_=d;description_.frame.mutation=resourceTicket_;renderer_=renderer;sceneViewport_=viewport;
    listTicket_={};joined_=false;started_=true;cameraMemo_.reset();
    if(!frameCurrent()){end();return false;}
    d=description_;++stats_.starts;return true;
}
void NativeEarlyRuntimeBinding::end()noexcept{
    if(!owned())return;
    if(ordinaryFrame_!=UINT64_MAX)++ordinaryFrame_;
    ordinary_=nullptr;cameraMemo_.reset();advanceReadSerial();readDepth_=0;read_.reset();started_=joined_=false;listTicket_={};
    resourceTicket_=effectTicket_=shaderTicket_=meshTicket_=0;renderer_=sceneViewport_=0;description_={};
}
bool NativeEarlyRuntimeBinding::sameDescription(const Description& d)const noexcept{
    return d.frame==description_.frame&&d.world==description_.world&&d.resources==description_.resources&&
        d.device==description_.device&&d.width==description_.width&&d.height==description_.height;
}
bool NativeEarlyRuntimeBinding::frameCurrent()const noexcept{
    if(!owned()||!configured_||!started_||readSerial_==UINT64_MAX)return false;
    // No callback may retag this invocation by ending/restarting the Binding or
    // its Read. end()/readBegin/readEnd monotonically advance readSerial_. A
    // saturated serial is permanently ineligible, so same-address ABA fails.
    const auto serial=readSerial_,resource=resourceTicket_,effect=effectTicket_,shader=shaderTicket_,mesh=meshTicket_;
    const auto frame=description_.frame;
    const auto world=description_.world,resources=description_.resources;
    const auto device=description_.device;
    const auto width=description_.width,height=description_.height;
    const auto renderer=renderer_,viewport=sceneViewport_;
    const auto joined=joined_;const auto listTicket=listTicket_;
    const auto depth=readDepth_;const auto* read=read_?std::addressof(*read_):nullptr;
    auto epochsCurrent=[&]()noexcept{
        return setup_.bootstrap->ready()&&setup_.resources->frameRevisionCurrent(resource)&&
            setup_.effects->current(effect)&&setup_.registry->currentShaderSet(shader)&&
            config_.geometry.cache->invalidationSerial()==mesh&&
            setup_.list->sceneActive(renderer,viewport)&&(!joined||setup_.list->current(listTicket));
    };
    if(!epochsCurrent()||!setup_.platform.frozenCurrent(setup_.platform.context,description_,renderer,viewport))return false;
    // frozenCurrent is an external adapter: revalidate AFTER it, without a
    // second callback. Camera/configuration data can only be rebound through
    // configure/start/end; end's changed serial rejects those transitions too.
    return owned()&&configured_&&started_&&readSerial_==serial&&readDepth_==depth&&
        (read_?std::addressof(*read_):nullptr)==read&&joined_==joined&&
        resourceTicket_==resource&&effectTicket_==effect&&shaderTicket_==shader&&meshTicket_==mesh&&
        description_.frame==frame&&description_.world==world&&description_.resources==resources&&
        description_.device==device&&description_.width==width&&description_.height==height&&
        renderer_==renderer&&sceneViewport_==viewport&&epochsCurrent();
}
bool NativeEarlyRuntimeBinding::borrowed()const noexcept{
    return owned()&&readDepth_&&read_&&read_->current()&&setup_.resources->borrowedOnThisThread(resourceTicket_)&&frameCurrent();
}
bool NativeEarlyRuntimeBinding::qualifiedReadCurrent()const noexcept{
    return owned()&&configured_&&started_&&readDepth_==1&&qualifiedSerial_&&
        qualifiedSerial_!=UINT64_MAX&&qualifiedSerial_==readSerial_&&read_&&
        qualifiedRead_==std::addressof(*read_)&&read_->current()&&
        (!joined_||setup_.list->current(listTicket_));
}
bool NativeEarlyRuntimeBinding::guarded(void* p,Address a,void* out,std::size_t n)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);if(!s.owned())return false;++s.stats_.guardedReads;
    return s.setup_.platform.guarded&&s.setup_.platform.guarded(s.setup_.platform.context,a,out,n);
}
NativeEarlyReadRouter::CopyResult NativeEarlyRuntimeBinding::physicalCopy(void* p,Address a,void* out,std::size_t n)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);
    if(!s.owned())return NativeEarlyReadRouter::CopyResult::Unavailable;
    const auto* local=s.copyScope_.ownerState();
    if(!local)return NativeEarlyReadRouter::CopyResult::Unavailable;
    // Owner check precedes the cached TLS-state access. No semantic epoch
    // load or dynamic TLS resolver is needed for each owned field copy.
    if(!s.copyScope_.current(*local,&s,s.readSerial_,s.read_?std::addressof(*s.read_):nullptr)||s.readDepth_!=1)
        return NativeEarlyReadRouter::CopyResult::Unavailable;
    int* const cell=s.copyScope_.errnoCell();
    if(!cell)return NativeEarlyReadRouter::CopyResult::Unavailable;
    if(!a||!out||!n||n>UINTPTR_MAX-a)return NativeEarlyReadRouter::CopyResult::Failed;
    // Only this private callback owns preservation. Qualification above is
    // scalar-only after the owning-thread check; no foreign callback runs.
    // Save the current value, not the value present when Read was activated.
    const int saved=*cell;
    const auto* const source=reinterpret_cast<const void*>(a);
    if(s.setup_.smallPhysicalCopies){
        const auto copyFixed=[out,source]<std::size_t N>()noexcept{
#if defined(__clang__)
            // Unlike ordinary memcpy, this intrinsic cannot be merged back
            // into an out-of-line variable-width call by Clang SimplifyCFG.
            __builtin_memcpy_inline(out,source,N);
#else
            std::memcpy(out,source,N);
#endif
        };
        // Constant-width memcpy retains byte-copy aliasing/alignment rules.
        // Each case touches exactly n bytes; all lifetime/range checks above
        // and current errno restoration below are shared with the generic path.
        switch(n){
        case 8: copyFixed.template operator()<8>();break;
        case 4: copyFixed.template operator()<4>();break;
        case 16: copyFixed.template operator()<16>();break;
        case 1: copyFixed.template operator()<1>();break;
        case 2: copyFixed.template operator()<2>();break;
        default: std::memcpy(out,source,n);break;
        }
    }else std::memcpy(out,source,n);
    *cell=saved;
    return NativeEarlyReadRouter::CopyResult::Copied;
}
bool NativeEarlyRuntimeBinding::resourceBorrow(void* p)noexcept{
    const auto&s=*static_cast<const NativeEarlyRuntimeBinding*>(p);
    // Memory liveness only: this exact short Read physically excludes the
    // covered native resource writers. Frame/shader/mesh/camera validity is
    // checked at Read entry and result publication, not for each copied field.
    // current() still rejects disarm, changed generation and read misuse.
    return s.owned()&&s.readDepth_&&s.read_&&s.read_->current();
}
bool NativeEarlyRuntimeBinding::listBorrow(void* p,Address a,std::size_t n)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);
    return s.owned()&&s.joined_&&s.setup_.list->current(s.listTicket_)&&s.setup_.list->permitsListRead(a,n);
}
bool NativeEarlyRuntimeBinding::frame(void* p,const Description& d)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);return s.sameDescription(d)&&s.frameCurrent();
}
bool NativeEarlyRuntimeBinding::nativeFrame(void* p,std::uint64_t f)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);return f==s.description_.frame.scene&&(s.qualifiedReadCurrent()||s.frameCurrent());
}
bool NativeEarlyRuntimeBinding::material(void* p,rdr95::ColdMaterialVariants::Owner o,rdr95::Address m,rdr95::Address effect)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);rdr95::Address current{};
    return o==s.setup_.owner&&s.internalBorrow()&&m&&m<=UINT64_MAX-0x18&&effect&&
        s.router_.scopedRead(std::uintptr_t(m+0x10),&current,sizeof current)&&current==effect&&s.internalBorrow();
}
bool NativeEarlyRuntimeBinding::readBegin(void* p,const Description& d)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);
    if(!s.owned())return false;
    if(s.ordinary_){s.advanceReadSerial();++s.stats_.readDeclines;return false;}
    s.copyScope_.reset();
    if(!s.sameDescription(d)||!s.frameCurrent()||s.readDepth_==UINT_MAX){++s.stats_.readDeclines;return false;}
    s.advanceReadSerial();
    if(s.readDepth_){if(!s.borrowed()){++s.stats_.readDeclines;return false;}++s.readDepth_;++s.stats_.readBegins;return true;}
    s.read_.emplace(*s.setup_.resources);
    if(!*s.read_||s.read_->generation()!=s.resourceTicket_||!s.frameCurrent()){
        s.read_.reset();++s.stats_.readDeclines;return false;
    }
    s.readDepth_=1;
    if(s.readSerial_&&s.readSerial_!=UINT64_MAX){s.qualifiedSerial_=s.readSerial_;s.qualifiedRead_=std::addressof(*s.read_);}
    if(s.setup_.physicalCopies||s.setup_.semanticDrawQualification)s.copyScope_.activate(&s,*s.read_,s.readSerial_);
    ++s.stats_.readBegins;return true;
}
void NativeEarlyRuntimeBinding::readEnd(void* p)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);if(!s.owned())return;
    if(s.ordinary_){s.ordinary_=nullptr;s.advanceReadSerial();s.readDepth_=0;s.read_.reset();return;}
    if(!s.readDepth_)return;
    s.advanceReadSerial();if(!--s.readDepth_)s.read_.reset();
}
NativeEarlyRuntimeBinding::Controller::Native::PreparationTicket NativeEarlyRuntimeBinding::preparationTicket(void* p,const Description& d)noexcept{
    auto&s=*static_cast<NativeEarlyRuntimeBinding*>(p);Controller::Native::PreparationTicket ticket;
    if(!s.sameDescription(d)||!s.joined_||s.readDepth_!=1||!s.borrowed()||!s.readSerial_||s.readSerial_==UINT64_MAX)return ticket;
    const auto* ready=s.setup_.list->ready();if(!ready)return ticket;
    ticket.owner_=&s;ticket.read_=std::addressof(*s.read_);ticket.geometryContext_=s.config_.geometry.context;ticket.current_=preparationCurrent;
    ticket.serial_=s.readSerial_;ticket.frame_=d.frame.scene;ticket.resources_=d.resources;ticket.ready_=*ready;ticket.geometry_=s.config_.geometry;
    if(s.setup_.semanticDrawQualification&&drawQualificationLive(&s,ticket.read_,ticket.serial_)){
        auto& q=ticket.drawQualification_;q.owner_=&s;q.read_=ticket.read_;q.serial_=ticket.serial_;q.live_=drawQualificationLive;
    }
    if(s.setup_.platform.geometryStorageCurrent&&s.setup_.platform.geometryStorageCurrent(s.setup_.platform.context,d)){
        auto& storage=ticket.storage_;storage.owner_=&s;storage.read_=ticket.read_;storage.context_=s.config_.geometry.context;
        storage.serial_=ticket.serial_;storage.frame_=ticket.frame_;storage.current_=geometryStorageCurrent;
    }
    return ticket;
}
bool NativeEarlyRuntimeBinding::geometryStorageCurrent(const void* p,const void* read,std::uint64_t serial)noexcept{
    const auto&s=*static_cast<const NativeEarlyRuntimeBinding*>(p);
    return preparationCurrent(p,read,serial)&&s.setup_.bootstrap->ready()&&
        s.setup_.platform.geometryStorageCurrent&&s.setup_.platform.geometryStorageCurrent(s.setup_.platform.context,s.description_);
}
bool NativeEarlyRuntimeBinding::drawQualificationLive(const void* p,const void* read,std::uint64_t serial)noexcept{
    const auto& s=*static_cast<const NativeEarlyRuntimeBinding*>(p);
    // Owner-thread check MUST precede reading the cached owning TLS-state.
    if(!s.owned()||!s.configured_||!s.started_||!s.joined_||s.readDepth_!=1||
       !serial||serial==UINT64_MAX||s.readSerial_!=serial||!s.read_||std::addressof(*s.read_)!=read)return false;
    const auto* local=s.copyScope_.ownerState();
    return local&&s.copyScope_.current(*local,&s,serial,std::addressof(*s.read_))&&
        s.setup_.list->current(s.listTicket_);
}
bool NativeEarlyRuntimeBinding::preparationCurrent(const void* p,const void* read,std::uint64_t serial)noexcept{
    auto&s=*static_cast<const NativeEarlyRuntimeBinding*>(p);
    return s.owned()&&s.joined_&&s.readDepth_==1&&s.readSerial_==serial&&serial&&serial!=UINT64_MAX&&
        s.read_&&std::addressof(*s.read_)==read&&s.qualifiedReadCurrent()&&
        s.setup_.effects->current(s.effectTicket_)&&s.setup_.registry->currentShaderSet(s.shaderTicket_)&&
        s.config_.geometry.cache->invalidationSerial()==s.meshTicket_;
}
bool NativeEarlyRuntimeBinding::mainJoined(void* p,const Description& d,const rdr95::Ready& r)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);
    auto fail=[&]{++s.stats_.joinDeclines;return false;};
    if(!s.sameDescription(d)||!s.borrowed()||s.joined_||!r.manager||
       !s.setup_.list->joined(s.config_.reads,r.manager,rdr95::MainRequest,rdr95::MainDepthWaitReturn,d.frame.scene))return fail();
    const auto* a=s.setup_.list->ready();
    if(!a||a->manager!=r.manager||a->request!=r.request||a->container!=r.container||a->records!=r.records||a->buckets!=r.buckets||
       a->instanceList!=r.instanceList||a->recordCount!=r.recordCount||a->bucketCount!=r.bucketCount||a->nativeFrame!=r.nativeFrame){s.setup_.list->closeMain();return fail();}
    s.listTicket_=s.setup_.list->ticket();s.joined_=true;
    if(!s.frameCurrent())return fail();++s.stats_.joins;return true;
}
bool NativeEarlyRuntimeBinding::cameraOwner(void* p,const NativeCurrentCamera::Lease& l)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);
    return l==NativeCurrentCamera::Lease{s.description_.frame.scene,s.description_.frame.pose,s.setup_.owner.identity,s.resourceTicket_}&&s.internalBorrow();
}
void NativeEarlyRuntimeBinding::prepareJoinedRead(void* p,const Description& d,Address manager,unsigned request,Address offset)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);
    if(!s.sameDescription(d)||!s.frameCurrent()||s.readDepth_||request!=rdr95::MainRequest||offset!=rdr95::MainDepthWaitReturn)return;
    if(s.setup_.platform.prepareJoinedRead)s.setup_.platform.prepareJoinedRead(s.setup_.platform.context,d,manager,request,offset);
}
bool NativeEarlyRuntimeBinding::matrixSetCurrent(void* p,rdr95::Address body)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);
    return body&&s.internalBorrow()&&s.setup_.platform.matrixSetCurrent&&s.setup_.platform.matrixSetCurrent(s.setup_.platform.context,Address(body))&&s.internalBorrow();
}
bool NativeEarlyRuntimeBinding::cameraMemoCurrent(void* p,const CameraKey& key,const Controller::CurrentPass&)noexcept{
    const auto& s=*static_cast<const NativeEarlyRuntimeBinding*>(p);
    return key.identity[0]==reinterpret_cast<std::uintptr_t>(&s)&&key.identity[1]==s.description_.frame.scene&&
        key.identity[2]==s.description_.frame.pose&&key.identity[3]==s.resourceTicket_&&s.borrowed();
}
bool NativeEarlyRuntimeBinding::currentPass(void* p,const Description& d,Pass pass,Controller::CurrentPass& out)noexcept{
    auto& s=*static_cast<NativeEarlyRuntimeBinding*>(p);out={};
    if(!s.sameDescription(d)||!s.borrowed()||!s.joined_)return false;
    PassState value;if(!s.setup_.platform.cachedPass(s.setup_.platform.context,d,pass,value)||!value.viewport||!value.current.gfx||!value.current.vk||
        !value.current.target.verified||value.current.target.width!=d.width||value.current.target.height!=d.height)return false;
    unsigned eye{};
    switch(pass){case Pass::LeftDepth:case Pass::LeftColor:eye=0;break;case Pass::RightDepth:case Pass::RightColor:eye=1;break;default:return false;}
    const auto run=s.setup_.platform.opaqueRun?s.setup_.platform.opaqueRun(s.setup_.platform.context):NativeOpaqueRun::Ticket{};
    const CameraKey key{{reinterpret_cast<std::uintptr_t>(&s),d.frame.scene,d.frame.pose,s.resourceTicket_,
        d.frame.admission,d.world,d.resources,reinterpret_cast<std::uintptr_t>(d.device),unsigned(pass),
        value.viewport,s.setup_.matrixBuffer,value.current.gfx,value.current.vk,
        s.setup_.owner.identity,s.setup_.owner.generation,s.meshTicket_},
        value.current.target.colorSurfaces,value.current.target.depthSurface,d.width,d.height};
    Controller::CurrentPass cameraProof;
    if(run&&s.cameraMemo_.get(run,key,cameraProof)&&s.borrowed()&&run.current()&&
       (!value.current.imageTicket||value.current.imageTicket.current())){
        // Only the camera proof is reused. TLS/target metadata above is fresh,
        // and the returned run receipt is checked again at final skip commit.
        value.current.opaqueRun=run;out=value.current;++s.stats_.cameraReuses;return true;
    }
    ++s.stats_.cameraChecks;NativeCurrentCamera::Result result;
    const NativeCurrentCamera::Call call{{d.frame.scene,d.frame.pose,s.setup_.owner.identity,s.resourceTicket_},value.viewport,s.setup_.matrixBuffer,s.description_.cameras[eye]};
    if(!NativeCurrentCamera::capture(s.cameraApi_,s.cameraProof_,call,result)||!s.borrowed()||
       (value.current.imageTicket&&!value.current.imageTicket.current())){++s.stats_.cameraDeclines;return false;}
    if(run)s.cameraMemo_.populate(run,key,value.current,{&s,cameraMemoCurrent});
    out=value.current;return true;
}
}
