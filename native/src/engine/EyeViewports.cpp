#include "EyeViewports.h"
#include <cerrno>
#include <cstring>

namespace rdrvr::engine {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
bool finite(const runtime::Mat4& matrix){for(float x:matrix)if(!std::isfinite(x))return false;return true;}
}
bool EyeViewports::clear() noexcept {
    ready_=false;screenPrepared_=false;
    try {
        while(constructed_){auto* p=&(*eyes_)[constructed_-1];if(!api_.isDetached(p))throw 0;
            api_.destroy(p);--constructed_;}
        eyes_.reset();return true;
    }catch(...){eyes_.release();constructed_=0;poisoned_=true;return false;}
}
EyeViewports::~EyeViewports() {
    Errno preserve;
    // Lifetime is strictly render-thread scoped. Never change a different thread's
    // current viewport during erroneous teardown.
    if(!owner() || !restore()){eyes_.release();return;}
    clear();
}
bool EyeViewports::prepare(const void* stock,const StereoCameraPlan& plan) {
    Errno preserve;
    if(!api_ || !stock || poisoned_ || active_ || (owner_!=std::thread::id{} && !owner()) || !api_.ownsThread())return false;
    if((eyes_ && (stock==&(*eyes_)[0] || stock==&(*eyes_)[1])) || !plan.sceneId || !plan.tracking.frameId)return false;
    std::array<EyePerspectiveState,2> perspective{};
    // TAA FULL (vr/TaaJitter.h): the clones get plan.projection moved by a sub-pixel offset (shear terms
    // only; same pixels for both eyes); plan_ keeps the un-jittered projection. A jittered matrix that fails the perspective
    // checks falls back to the plain one (no jitter this scene).
    auto jitter=::rdrvr::vr::taa::jitter::issuer.issue(plan.sceneId);
    std::array<runtime::Mat4,2> projection{plan.projection[0],plan.projection[1]};
    if(jitter.on)for(unsigned i=0;i<2;++i){
        projection[i]=::rdrvr::vr::taa::jitter::apply(plan.projection[i],jitter.px[0],jitter.px[1],jitter.extent[i]&0xFFFFu,jitter.extent[i]>>16);
        EyePerspectiveState probe{};
        if(!finite(projection[i])||!buildEyePerspectiveState(projection[i],plan.nearClip,plan.farClip,probe)){
            projection={plan.projection[0],plan.projection[1]};jitter.on=false;jitter.px={};
            ::rdrvr::vr::taa::jitter::issuer.cancel(plan.sceneId);break;}
    }
    for(unsigned i=0;i<2;++i)if(!finite(plan.world[i]) || !finite(plan.projection[i]) ||
        !validPose(plan.tracking.eyes[i]) || !runtime::usableFov(plan.tracking.fov[i]) ||
        !buildEyePerspectiveState(projection[i],plan.nearClip,plan.farClip,perspective[i]))return false;
    owner_=std::this_thread::get_id();if(!clear())return false;
    try {
        eyes_=std::make_unique<std::array<ViewportStorage,2>>();
        for(unsigned i=0;i<2;++i){auto* target=&(*eyes_)[i];api_.copy(target,stock);++constructed_;
            applyEyePerspectiveState(target->bytes,perspective[i]);
            api_.camera(target,plan.world[i]);api_.projection(target,projection[i]);}
        perspective_=perspective;plan_=plan;projection_=projection;jitter_=jitter;ready_=true;return true;
    }catch(...){clear();throw;}
}
bool EyeViewports::activate(unsigned eye) {
    Errno preserve;
    if(eye>1 || !ready() || !owner() || !api_.ownsThread())return false;
    // Obtain the original viewport only once; left -> right must restore stock,
    // not the left-eye clone. Pointer switch is separate from matrix upload.
    const void* old=api_.current(&(*eyes_)[eye],false,false,false);
    if(!active_){previous_=old;active_=true;}
    try{api_.upload(&(*eyes_)[eye],true);}catch(...){restore();throw;}
    return matches(eye);
}
bool EyeViewports::prepareScreen(std::uint32_t width,std::uint32_t height) noexcept {
    Errno preserve;
    try {
        if(!ready()||active_||!owner()||!api_.ownsThread())return false;
        std::array<EyeScreenState,2> next{};
        for(unsigned eye=0;eye<2;++eye)
            if(!api_.isDetached(&(*eyes_)[eye])||!buildEyeScreenState((*eyes_)[eye].bytes,width,height,next[eye]))return false;
        for(unsigned eye=0;eye<2;++eye)applyEyeScreenState((*eyes_)[eye].bytes,next[eye]);
        screen_=next;screenPrepared_=true;return true;
    }catch(...){return false;}
}
bool EyeViewports::matches(unsigned eye) const noexcept {
    Errno preserve;
    if(eye>1 || !ready() || !active_ || !owner())return false;
    try {
        if(!api_.ownsThread() || !api_.isSelected(&(*eyes_)[eye]))return false;
        runtime::Mat4 world{},projection{};
        std::memcpy(world.data(),(*eyes_)[eye].bytes.data()+0x40,64);
        std::memcpy(projection.data(),(*eyes_)[eye].bytes.data()+0x1c0,64);
        // Matrix34 padding is not part of the camera and need not be initialized.
        for(unsigned c=0;c<4;++c)for(unsigned r=0;r<3;++r)if(world[c*4+r]!=plan_.world[eye][c*4+r])return false;
        return projection==projection_[eye]&&matchesEyePerspectiveState((*eyes_)[eye].bytes,perspective_[eye])&&
            (!screenPrepared_||matchesEyeScreenState((*eyes_)[eye].bytes,screen_[eye]));
    }catch(...){return false;}
}
bool EyeViewports::cameraMatrices(unsigned eye,runtime::Mat4& relativeClip,runtime::Mat4& camera) const noexcept {
    if(eye>1||!ready()||!eyes_)return false;
    std::memcpy(relativeClip.data(),(*eyes_)[eye].bytes.data()+0xc0,64);
    std::memcpy(camera.data(),(*eyes_)[eye].bytes.data()+0x40,64);
    return finite(relativeClip)&&finite(camera);
}
bool EyeViewports::restore() noexcept {
    Errno preserve;
    if(!active_)return true;
    if(!owner())return false;
    try {
        if(!api_.ownsThread())return false;
        api_.current(previous_,false,false,false);
        if(previous_)api_.upload(const_cast<void*>(previous_),true);
        if(!api_.isSelected(previous_)){poisoned_=true;return false;}
        previous_=nullptr;active_=false;return true;
    }catch(...){poisoned_=true;return false;}
}
} // namespace rdrvr::engine
