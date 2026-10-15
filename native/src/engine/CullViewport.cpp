#include "CullViewport.h"
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
bool same(const StockCamera& a,const StockCamera& b){return a.world==b.world&&a.projection==b.projection&&a.nearClip==b.nearClip&&a.farClip==b.farClip;}
bool decode(const ViewportStorage& v,StockCamera& out){return decodeCamera(std::span<const std::byte>(v.bytes).subspan(cameraReadOffset,cameraReadBytes),out);}
bool perspectiveMetadata(ViewportStorage& v,const StockCamera& camera){
    const auto& p=camera.projection;
    if(p[0]<=0||std::abs(p[5])<0.0001f||p[8]!=0||p[9]!=0)return false;
    const float vertical=1/std::abs(p[5]),horizontal=1/p[0];
    const float fov=2*std::atan(vertical)*180/3.14159265358979323846f;
    const float aspect=horizontal/vertical;
    if(!std::isfinite(fov)||!std::isfinite(aspect)||aspect<=0)return false;
    // Private clone only. SetProjection changes +1c0 and +370 but leaves these
    // fields stale. spdViewport::Set copies +348/+34c into every portal shaft;
    // SetX/Y then uses them instead of the matrix to construct side planes.
    const std::array<float,10> fields{fov,aspect,camera.nearClip,camera.farClip,
        vertical,horizontal,1,p[5]<0?-1.f:1.f,0,0};
    std::memcpy(v.bytes.data()+0x338,fields.data(),sizeof(fields));
    return true;
}
}
bool CullViewport::destroy(std::unique_ptr<ViewportStorage>& p) noexcept {
    if(!p)return true;
    try {if(!api_.viewport.isDetached(p.get()))throw 0;api_.viewport.destroy(p.get());p.reset();return true;}
    catch(...){p.release();poisoned_=true;return false;}
}
CullViewport::~CullViewport(){
    Errno preserve;
    // A pending engine borrower must not receive a dangling pointer if its
    // adapter is torn down early. Retain the heap block instead of freeing it.
    if(pending_ || !owner()){storage_.release();return;}
    destroy(storage_);
}
bool CullViewport::prepare(const void* source,const StereoCullPlan& plan,bool stereoPerspective) noexcept {
    Errno preserve;
    if(!api_ || !source || source==storage_.get() || pending_ || poisoned_ ||
       (owner_!=std::thread::id{}&&!owner()) || !plan.sceneId || plan.sceneId<=last_ ||
       !plan.tracking.frameId || plan.tracking.displayTime<=0)return false;
    // Validate the requested camera using exactly the same audited decoder as
    // retail viewports; do not feed NaNs or a non-rigid camera to engine code.
    ViewportStorage encoded{};std::memcpy(encoded.bytes.data()+0x40,plan.camera.world.data(),64);
    std::memcpy(encoded.bytes.data()+0x1c0,plan.camera.projection.data(),64);
    std::memcpy(encoded.bytes.data()+0x340,&plan.camera.nearClip,4);std::memcpy(encoded.bytes.data()+0x344,&plan.camera.farClip,4);
    StockCamera checked{};if(!decode(encoded,checked)||!same(checked,plan.camera))return false;
    std::unique_ptr<ViewportStorage> candidate;bool constructed=false;
    try {
        candidate=std::make_unique<ViewportStorage>();
        if(!api_.viewport.isDetached(candidate.get()))return false;
        api_.viewport.copy(candidate.get(),source);constructed=true;
        StockCamera stock{};if(!decode(*candidate,stock))throw 0;
        // Lower near first so every intermediate near/far pair is ordered,
        // including a new near plane beyond the old far plane.
        api_.nearClip(candidate.get(),std::min(stock.nearClip,plan.camera.nearClip));
        api_.farClip(candidate.get(),plan.camera.farClip);
        api_.nearClip(candidate.get(),plan.camera.nearClip);
        api_.viewport.camera(candidate.get(),plan.camera.world);
        if(stereoPerspective&&!perspectiveMetadata(*candidate,plan.camera))throw 0;
        api_.viewport.projection(candidate.get(),plan.camera.projection);
        if(!api_.viewport.isDetached(candidate.get()) || !decode(*candidate,checked) || !same(checked,plan.camera))throw 0;
        if(!destroy(storage_))throw 0;
        storage_=std::move(candidate);owner_=std::this_thread::get_id();prepared_=plan.sceneId;return true;
    }catch(...){if(constructed)destroy(candidate);return false;}
}
const void* CullViewport::lease(std::uint64_t serial) noexcept {
    Errno preserve;
    if(!owner() || !ready() || pending_ || serial!=prepared_ || serial<=last_)return nullptr;
    try {if(!api_.viewport.isDetached(storage_.get()))return nullptr;}catch(...){return nullptr;}
    pending_=last_=serial;return storage_.get();
}
bool CullViewport::retire(std::uint64_t serial) noexcept {
    Errno preserve;
    if(!owner() || !serial || pending_!=serial)return false;
    try {if(!api_.viewport.isDetached(storage_.get()))return false;}catch(...){return false;}
    pending_=0;return true;
}
bool CullViewport::reset() noexcept {
    Errno preserve;if(!owner() || pending_)return false;
    bool ok=destroy(storage_);if(ok)prepared_=0;return ok;
}
} // namespace rdrvr::engine
