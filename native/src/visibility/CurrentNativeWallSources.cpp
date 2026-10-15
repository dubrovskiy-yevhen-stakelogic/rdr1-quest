#include "CurrentNativeWallSources.h"
#include "engine/OwnedStaticVertices.h"
#include <cerrno>
#include <cstring>

namespace rdrvr::experimental {
namespace {struct PreserveErrno {int value{errno};~PreserveErrno(){errno=value;}};}
void CurrentNativeWallSources::configure(Native::Api api,Witness::Api witness,Registry* registry)noexcept {
    if(std::this_thread::get_id()!=thread_||object_||primitive_)return;
    end();source_.configure(api);witness_=witness;registry_=registry;
    meshes_=api.triangles.ownedOccluders;vertices_=api.triangles.ownedVertices;
}
bool CurrentNativeWallSources::configured()const noexcept {
    return source_.ready()&&registry_&&meshes_&&witness_.completeCodeProof&&witness_.shadowBufferEnabledSlot;
}
bool CurrentNativeWallSources::current()const noexcept {
    return std::this_thread::get_id()==thread_&&frame_.valid()&&cones_&&cones_->ready(frame_)&&
        meshes_&&meshes_->invalidationSerial()==meshSerial_&&
        (!vertices_||vertices_->invalidationSerial()==vertexSerial_);
}
bool CurrentNativeWallSources::begin(Frame frame,VkDevice device,const std::array<Native::Camera,2>& cameras,
        unsigned width,unsigned height,Cones& cones,std::span<const CurrentWallSeeds::Selection> selected,
        std::span<const WallHintExchange::Hint> hints)noexcept {
    PreserveErrno preserve;
    if(std::this_thread::get_id()!=thread_||object_||primitive_)return false;
    end();stats_={};patches_={};
    if(!configured()||!device||!frame.valid()||!cones.ready(frame)||selected.size()>32||hints.size()>32||
       !projection_.configure(cameras,width,height)||!source_.begin(frame.scene,{})||!seeds_.begin(frame,cones,selected))return false;
    frame_=frame;device_=device;cones_=&cones;cameras_=cameras;width_=width;height_=height;
    meshSerial_=meshes_->invalidationSerial();vertexSerial_=vertices_?vertices_->invalidationSerial():0;
    selectedCount_=unsigned(selected.size());hintCount_=unsigned(hints.size());
    for(unsigned i=0;i<selectedCount_;++i)selected_[i]=selected[i];
    for(unsigned i=0;i<hintCount_;++i)hints_[i]=hints[i];
    return current();
}
void CurrentNativeWallSources::target(const Witness::Target& target)noexcept {
    if(std::this_thread::get_id()!=thread_)return;
    target_={};if(target.verified&&target.width==width_&&target.height==height_)target_=target;
}
void CurrentNativeWallSources::end()noexcept {
    if(std::this_thread::get_id()!=thread_||object_||primitive_)return;
    source_.end();frame_={};device_={};cones_=nullptr;target_={};geometry_={};
    selectedCount_=hintCount_=proofCount_=0;
}
bool CurrentNativeWallSources::needsGeometryProof()const noexcept {
    return std::this_thread::get_id()==thread_&&object_&&object_->owner_==this&&object_->valid_&&!object_->completed_&&current();
}
CurrentNativeWallSources::Object::Object(CurrentNativeWallSources& r,const void* info,bool exact)noexcept {
    PreserveErrno preserve;
    if(std::this_thread::get_id()!=r.thread_)return;
    owner_=&r;previous_=r.object_;r.object_=this;info_=info;
    if(!exact||previous_||!info||!r.current())return;
    const auto record=reinterpret_cast<std::uintptr_t>(info);
    bool wanted=false;for(unsigned i=0;i<r.selectedCount_;++i)wanted|=r.selected_[i].record==record;
    if(!wanted)return;
    ++r.stats_.selectedObjects;r.proofCount_=0;
    borrow_.emplace(r.source_,r.frame_.scene,info,true);
    if(!r.source_.inspect(r.frame_.scene,info,value_)||
       !CurrentWallPatchBuilder::wants({r.hints_.data(),r.hintCount_},value_.node,value_.drawable)){
        ++r.stats_.objectDeclined;return;
    }
    ++r.stats_.inspected;valid_=true;draw_.emplace(r.seeds_,record,true);
}
CurrentNativeWallSources::Object::~Object(){
    draw_.reset();borrow_.reset();if(owner_)owner_->object_=previous_;
}
void CurrentNativeWallSources::Object::complete()noexcept {
    PreserveErrno preserve;
    if(!owner_||std::this_thread::get_id()!=owner_->thread_||owner_->object_!=this||completed_)return;
    completed_=true;auto& r=*owner_;
    if(!valid_||!draw_||!r.current())return;
    Native::Object fresh{};
    if(!r.source_.inspect(r.frame_.scene,info_,fresh)||!Native::sameObject(fresh,value_)){
        ++r.stats_.changed;return;
    }
    // BindInputLayout's pinned success continuation emits the indexed primitive
    // unconditionally. Complete is called only AFTER the original DrawCommon.
    publishing_=true;draw_->complete(true);publishing_=false;++r.stats_.completed;
}
bool CurrentNativeWallSources::admitsCompletedSource(Frame frame,const Cones::SourceKey& key)const noexcept {
    if(std::this_thread::get_id()!=thread_||frame!=frame_||!object_||!object_->valid_||!object_->completed_||!object_->publishing_||!current()||
       key.object!=object_->value_.node||key.record!=reinterpret_cast<std::uintptr_t>(object_->info_)||
       key.transform!=key.record||key.transformGeneration!=frame_.scene)return false;
    for(unsigned i=0;i<proofCount_;++i){const auto& proof=proofs_[i];if(proof.key!=key)continue;
        Registry::Snapshot now{};
        return registry_->query(device_,proof.pipeline.pipeline,now)&&now.deviceEpoch==proof.pipeline.deviceEpoch&&
            now.pipelineGeneration==proof.pipeline.pipelineGeneration&&
            now.vertexShaderGeneration==proof.pipeline.vertexShaderGeneration&&
            now.fragmentShaderGeneration==proof.pipeline.fragmentShaderGeneration;
    }
    return false;
}
CurrentNativeWallSources::Geometry::Geometry(CurrentNativeWallSources& r,const void* ptr,bool exact)noexcept {
    if(!r.needsGeometryProof())return;owner_=&r;previous_=r.geometry_;
    Witness::geometry(r.witness_,ptr,exact,r.geometry_);if(!r.geometry_.valid)++r.stats_.geometryDeclined;
}
CurrentNativeWallSources::Geometry::~Geometry(){if(owner_)owner_->geometry_=previous_;}
CurrentNativeWallSources::Primitive::Primitive(CurrentNativeWallSources& r,void* context,bool indexed,int mode,int first,int count,int base)noexcept {
    if(!r.needsGeometryProof())return;owner_=&r;previous_=r.primitive_;r.primitive_=this;
    context_=context;indexed_=indexed;mode_=mode;first_=first;count_=count;base_=base;
}
CurrentNativeWallSources::Primitive::~Primitive(){if(owner_)owner_->primitive_=previous_;}
void CurrentNativeWallSources::prepared(void* context,const Witness::Binding& binding,bool exact)noexcept {
    if(!needsGeometryProof())return;PreserveErrno preserve;++stats_.primitives;
    auto* primitive=primitive_;
    if(!primitive||primitive->previous_||primitive->observed_||primitive->context_!=context||!primitive->indexed_||
       primitive->first_||primitive->base_||!geometry_.valid||primitive->mode_<0||
       unsigned(primitive->mode_)!=geometry_.mode||primitive->count_<=0||unsigned(primitive->count_)!=geometry_.indexCount||!exact){
        ++stats_.primitiveDeclined;return;
    }
    primitive->observed_=true;Witness::Snapshot actual{};
    if(!Witness::captureState(witness_,context,binding,target_,true,actual)){++stats_.stateDeclined;return;}
    Registry::Snapshot pipeline{};
    if(!registry_->query(device_,reinterpret_cast<VkPipeline>(actual.pipeline),pipeline)){++stats_.pipelineDeclined;return;}
    const auto& cert=pipeline.certificate;
    if(!cert.candidateSafe||!cert.opaqueDepth||cert.binding||cert.offset||
       cert.position!=engine::StaticOcclusionCertificate::PositionModel::RelativeAffineXYZ1){++stats_.pipelineDeclined;return;}
    const auto viewport=(cert.requiredDynamic&engine::StaticOcclusionCertificate::Viewport)?
        VkViewport{actual.viewport[0],actual.viewport[1],actual.viewport[2],actual.viewport[3],actual.viewport[4],actual.viewport[5]}:cert.viewport;
    const auto scissor=(cert.requiredDynamic&engine::StaticOcclusionCertificate::Scissor)?
        VkRect2D{{actual.scissorOffset[0],actual.scissorOffset[1]},{actual.scissorExtent[0],actual.scissorExtent[1]}}:cert.scissor;
    if(viewport.x!=0||viewport.width!=float(width_)||viewport.minDepth!=0||viewport.maxDepth!=1||
       !((viewport.y==0&&viewport.height==float(height_))||(viewport.y==float(height_)&&viewport.height==-float(height_)))||
       scissor.offset.x||scissor.offset.y||scissor.extent.width!=width_||scissor.extent.height!=height_){++stats_.stateDeclined;return;}
    if(!Witness::captureMatrices(witness_,context,binding,actual)){++stats_.matrixDeclined;return;}
    engine::NativeGeometryBindingReceipt receipt;
    if(!Witness::matchesGeometry(witness_,geometry_,actual,cert.format==VK_FORMAT_R32G32B32A32_SFLOAT?16u:12u,cert.stride,&receipt)){
        ++stats_.bindingDeclined;return;
    }
    if(!source_.qualifyPacketTransform(frame_.scene,object_->value_,cameras_[0],actual.matrices,receipt)){
        ++stats_.matrixDeclined;return;
    }
    auto faces=Cones::Raster::Faces::Both;
    if(cert.cull==VK_CULL_MODE_FRONT_AND_BACK)return;
    if(cert.cull!=VK_CULL_MODE_NONE){bool positive=cert.frontFace==VK_FRONT_FACE_CLOCKWISE;
        if(viewport.height<0)positive=!positive;if(cert.cull==VK_CULL_MODE_FRONT_BIT)positive=!positive;
        faces=positive?Cones::Raster::Faces::PositiveArea:Cones::Raster::Faces::NegativeArea;
    }
    Native::Packet packet;
    if(!source_.capturePacketQualified(frame_.scene,receipt,faces,packet)||
       packet.vertexHandle!=actual.vertexBuffer||packet.indexHandle!=actual.indexBuffer||actual.vertexOffset){++stats_.packetDeclined;return;}
    const auto record=reinterpret_cast<std::uintptr_t>(object_->info_);
    // LOD is used only as a comparable current scalar, never a memory address.
    std::uint32_t flags{};std::memcpy(&flags,static_cast<const std::byte*>(object_->info_)+0xac,4);
    Cones::SourceKey live{object_->value_.node,0,actual.pipeline,(flags>>10)&7u,record,0,pipeline.pipelineGeneration,frame_.scene,record};
    const auto count=CurrentWallPatchBuilder::copy(frame_.scene,packet,*meshes_,projection_,
        {hints_.data(),hintCount_},live,object_->value_.drawable,copied_,patches_);
    for(unsigned i=0;i<count&&proofCount_<proofs_.size();++i){
        if(object_->draw_->observe(copied_[i])){proofs_[proofCount_++]={copied_[i].key,pipeline};++stats_.copied;}
    }
}
}
