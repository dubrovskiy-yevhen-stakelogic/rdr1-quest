#include "NativeEyeResolution.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <sstream>

namespace rdrvr::engine {
NativeEyeResolution nativeEyeResolution;
namespace {
struct PreserveErrno {int value{errno};~PreserveErrno(){errno=value;}};
}
bool NativeEyeResolution::valid(Extent e) noexcept {
    return e.width>=16&&e.height>=16&&e.width<=8192&&e.height<=8192;
}
bool NativeEyeResolution::configure(Api api) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(configured_||started_||!api.maxScreen||!api.resize||!api.rebuild||!api.read||!api.vegetationSettingsSlot||
       !api.stockWidthSlot||!api.stockHeightSlot)return false;
    api_=api;configured_=true;return true;
}
void NativeEyeResolution::installed(bool value) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    installed_=value&&configured_;if(!installed_)active_={};
}
bool NativeEyeResolution::request(std::uint32_t w,std::uint32_t h) noexcept {
    PreserveErrno preserve;++requests_;const Extent proposed{w,h};
    if(!valid(proposed))return false;
    std::lock_guard lock(mutex_);
    if(request_)return request_==proposed;
    if(started_){++lateRequests_;return false;}
    request_=proposed;return true;
}
NativeEyeResolution::Extent NativeEyeResolution::requestedExtent() const noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    return installed_&&!fault_?request_:Extent{};
}
void* NativeEyeResolution::trackedDepth() const noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);return depthGood_?depth_:nullptr;
}
void NativeEyeResolution::depthCreated(void* p,std::uint32_t w,std::uint32_t h,bool verified) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    depth_=p;depthAbsent_=false;depthGood_=installed_&&request_&&p&&verified&&valid({w,h})&&w>=request_.width&&h>=request_.height;
    ++depthGeneration_;++completedGeneration_;active_={};
    if(!depthGood_)++verificationFailures_;
}
void NativeEyeResolution::displayDepthNotRequired(bool verified) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    depth_=nullptr;depthAbsent_=installed_&&request_&&verified;depthGood_=depthAbsent_;
    ++depthGeneration_;++completedGeneration_;active_={};
    if(!depthGood_)++verificationFailures_;
}
NativeEyeResolution::InitialScope::InitialScope(NativeEyeResolution& owner,Family family,void* identity):previous_(initial_) {
    PreserveErrno preserve;state_.owner=&owner;state_.family=family;state_.identity=identity;state_.previous=previous_;
    bool recursive=false;
    for(auto* s=previous_;s;s=s->previous)if(s->owner==&owner&&s->family==family){s->fault=true;recursive=true;}
    ++owner.scopeCalls_;
    {
        std::lock_guard lock(owner.mutex_);owner.started_=true;
        const auto index=static_cast<unsigned>(family);
        if(index<5){
            if(family==Family::Renderer&&identity){
                if(owner.renderer_&&owner.renderer_!=identity){owner.completedMask_=0;owner.active_={};}
                owner.renderer_=identity;
            }
            owner.completedMask_&=~(1u<<index);owner.active_={};
            ++owner.inFlight_[index];
            state_.depthGeneration=owner.depthGeneration_;
            state_.admitted=owner.installed_&&owner.request_&&owner.depthGood_&&!owner.fault_&&!recursive;
        }
        state_.fault=recursive;
    }
    initial_=&state_;
}
NativeEyeResolution::InitialScope::~InitialScope(){
    PreserveErrno preserve;if(!finished_){state_.fault=true;finish();}initial_=previous_;
}
bool NativeEyeResolution::InitialScope::finish() noexcept {
    PreserveErrno preserve;if(finished_)return !state_.fault;finished_=true;
    auto& owner=*state_.owner;std::lock_guard lock(owner.mutex_);
    const auto index=static_cast<unsigned>(state_.family);
    if(index>=5)return false;
    if(owner.inFlight_[index])--owner.inFlight_[index];
    // Grass has an audited disabled-feature path which creates no fullscreen
    // targets and never reaches GetMaxScreenSize. The native resize function
    // checks the same feature before touching this optional family.
    const bool skipped=state_.admitted&&!state_.fault&&!state_.sawMax&&state_.family==Family::Grass;
    const bool completed=state_.admitted&&!state_.fault&&state_.sawMax&&
        state_.depthGeneration==owner.depthGeneration_&&!owner.inFlight_[index];
    if(completed){owner.completedMask_|=1u<<index;owner.capacity_[index]=state_.reserved;++owner.completedGeneration_;}
    else if(!skipped&&(state_.admitted||state_.fault)){++owner.scopeFaults_;owner.fault_=true;owner.active_={};}
    state_.fault=!completed&&!skipped;return completed||skipped;
}
void NativeEyeResolution::maxScreen(int* w,int* h,Family site){
    const int incoming=errno;errno=incoming;api_.maxScreen(w,h);const int result=errno;++maxCalls_;
    auto* s=initial_;
    if(s&&s->owner==this&&site!=Family::None){
        std::lock_guard lock(mutex_);
        if(s->admitted&&!s->fault&&s->family==site&&w&&h&&w!=h&&
           valid({static_cast<std::uint32_t>(*w),static_cast<std::uint32_t>(*h)})&&
           s->depthGeneration==depthGeneration_&&depthGood_){
            s->sawMax=true;
            *w=std::max(*w,static_cast<int>(request_.width));
            *h=std::max(*h,static_cast<int>(request_.height));
            s->reserved={static_cast<std::uint32_t>(*w),static_cast<std::uint32_t>(*h)};++expandedCalls_;
        }else if(s->admitted){s->fault=true;++declined_;}
    }
    errno=result;
}
unsigned NativeEyeResolution::requiredMask() const noexcept {
    PreserveErrno preserve;
    if(!api_.read||!api_.vegetationSettingsSlot)return 0;
    try{
        std::uintptr_t manager{};
        if(!api_.read(nullptr,api_.vegetationSettingsSlot,&manager,sizeof(manager)))return 0;
        if(!manager)return 19;
        if(manager%8||manager>UINTPTR_MAX-0xb1da)return 0;
        std::array<unsigned char,2> flags{};
        if(!api_.read(nullptr,manager+0xb1d8,flags.data(),flags.size()))return 0;
        return flags[0]&&!flags[1]?31u:19u;
    }catch(...){return 0;}
}
bool NativeEyeResolution::capacityReadyLocked(void* renderer,unsigned required) const noexcept {
    return required&&installed_&&!fault_&&request_&&depthGood_&&renderer&&renderer_==renderer&&(completedMask_&required)==required&&
        std::all_of(inFlight_.begin(),inFlight_.end(),[](unsigned v){return v==0;});
}
bool NativeEyeResolution::rendererScale(void* renderer,Extent* actual) const noexcept {
    PreserveErrno preserve;
    const auto p=reinterpret_cast<std::uintptr_t>(renderer);
    if(!p||p%8||p>UINTPTR_MAX-0x5cc||!api_.read)return false;
    std::array<std::byte,0x6c> state{};
    try{
        if(!api_.read(nullptr,p+0x560,state.data(),state.size()))return false;
        float scale{},cap{};Extent size{};
        std::memcpy(&scale,state.data()+4,4);std::memcpy(&cap,state.data()+0x68,4);
        std::memcpy(&size.width,state.data()+8,4);std::memcpy(&size.height,state.data()+12,4);
        if(scale!=1.0f||cap!=1.0f||!valid(size))return false;
        if(actual)*actual=size;return true;
    }catch(...){return false;}
}
bool NativeEyeResolution::canResize(void* renderer,bool coordinates) const noexcept {
    if(!coordinates)return false;
    Extent stock{};if(!stockExtent(stock))return false;
    const auto required=requiredMask();
    {std::lock_guard lock(mutex_);if(!capacityReadyLocked(renderer,required)||!fitsCapacityLocked(stock,required))return false;}
    return rendererScale(renderer);
}
bool NativeEyeResolution::fitsCapacityLocked(Extent extent,unsigned required) const noexcept {
    if(!required||!valid(extent))return false;
    for(unsigned i=0;i<capacity_.size();++i)if((required&(1u<<i))&&
        (extent.width>capacity_[i].width||extent.height>capacity_[i].height))return false;
    return true;
}
bool NativeEyeResolution::stockExtent(Extent& out) const noexcept {
    PreserveErrno preserve;if(!api_.read||!api_.stockWidthSlot||!api_.stockHeightSlot||
        api_.stockWidthSlot%4||api_.stockHeightSlot%4)return false;
    try{Extent size{};
        if(api_.stockWidthSlot<=UINTPTR_MAX-8&&api_.stockHeightSlot==api_.stockWidthSlot+4){
            if(!api_.read(nullptr,api_.stockWidthSlot,&size,sizeof(size)))return false;
        }else if(!api_.read(nullptr,api_.stockWidthSlot,&size.width,4)||!api_.read(nullptr,api_.stockHeightSlot,&size.height,4))return false;
        if(!valid(size))return false;out=size;return true;
    }catch(...){return false;}
}
void NativeEyeResolution::resize(void* renderer,int w,int h,bool exact){
    const int incoming=errno;
    const auto* update=updating_;
    const bool ownUpdate=update&&update->owner==this&&update->renderer==renderer;
    bool coordinates=ownUpdate&&update->coordinateReady&&update->world;
    // A false-world or lost-coordinate update may not resurrect the previous
    // frame's active extent. Only a genuinely external qualified resize can
    // retain an already active world's dimensions.
    if(!update){std::lock_guard lock(mutex_);coordinates=bool(active_)&&renderer_==renderer;}
    const bool full=exact&&canResize(renderer,coordinates);
    const auto required=requiredMask();
    {std::lock_guard lock(mutex_);if(renderer_==renderer){active_={};normalized_={};
        if(!full&&(completedMask_&required)==required&&required&&!fitsCapacityLocked({static_cast<std::uint32_t>(w),static_cast<std::uint32_t>(h)},required)){
            completedMask_=0;fault_=true;
        }}
     if(full){w=static_cast<int>(request_.width);h=static_cast<int>(request_.height);}}
    errno=incoming;api_.resize(renderer,w,h);const int result=errno;
    if(full)++resizes_;
    if(ownUpdate&&exact){updating_->resized=true;if(update->coordinateReady&&!update->world)++stockResizes_;}
    errno=result;
}
void NativeEyeResolution::rebuild(void* renderer,int w,int h,bool exact){
    const int incoming=errno;bool full=false;
    {
        std::lock_guard lock(mutex_);
        // AA rebuild is a fresh allocation. Keep it large even before optional
        // vegetation is initialized; otherwise a later full-family resize
        // would grow that newly replaced backing in an in-flight frame.
        full=exact&&installed_&&!fault_&&request_&&depthGood_&&renderer_==renderer&&
            (completedMask_&19)==19;
        if(full){w=std::max(w,static_cast<int>(request_.width));h=std::max(h,static_cast<int>(request_.height));active_={};}
        else if(renderer_==renderer){active_={};completedMask_=0;fault_=true;}
    }
    errno=incoming;api_.rebuild(renderer,w,h);const int result=errno;
    if(full){++rebuilds_;std::lock_guard lock(mutex_);++completedGeneration_;}
    errno=result;
}
void NativeEyeResolution::finishUpdate(void* renderer,bool coordinates,bool world){
    PreserveErrno preserve;
    Extent stock{};
    if(!stockExtent(stock)){++stockFailures_;std::lock_guard lock(mutex_);if(renderer_==renderer)active_={};return;}
    if(!canResize(renderer,coordinates)){++declined_;std::lock_guard lock(mutex_);if(renderer_==renderer)active_={};return;}
    Extent expected=stock,actual{};std::uint64_t generation{};bool normalized=false;
    const auto required=requiredMask();
    {std::lock_guard lock(mutex_);if(world)expected=request_;
     if(!fitsCapacityLocked(expected,required)){++stockFailures_;active_={};return;}
     generation=completedGeneration_;normalized=normalized_==expected&&normalizedGeneration_==generation;}
    if(!rendererScale(renderer,&actual)){++verificationFailures_;std::lock_guard lock(mutex_);active_={};return;}
    const bool alreadyResized=updating_&&updating_->owner==this&&updating_->renderer==renderer&&updating_->resized;
    if((!normalized||actual!=expected)&&!(alreadyResized&&actual==expected)){
        // Every participating target was allocated at least this large before
        // first use. Native resize retains the standard image-data retirement.
        api_.resize(renderer,static_cast<int>(expected.width),static_cast<int>(expected.height));
        if(world)++resizes_;else ++stockResizes_;
    }
    if(!rendererScale(renderer,&actual)||actual!=expected){
        ++verificationFailures_;std::lock_guard lock(mutex_);active_={};fault_=true;return;
    }
    const auto currentRequired=requiredMask();std::lock_guard lock(mutex_);
    if(capacityReadyLocked(renderer,currentRequired)&&fitsCapacityLocked(expected,currentRequired)&&completedGeneration_==generation){
        if(world){if(active_!=expected||activeGeneration_!=generation)++activations_;active_=expected;activeGeneration_=generation;}
        else{if(!normalized)++stockNormalizations_;active_={};}
        normalized_=expected;normalizedGeneration_=generation;
    }
}
NativeEyeResolution::Extent NativeEyeResolution::activeExtent(void* renderer) const noexcept {
    PreserveErrno preserve;const auto required=requiredMask();std::lock_guard lock(mutex_);
    return capacityReadyLocked(renderer,required)&&activeGeneration_==completedGeneration_?active_:Extent{};
}
std::string NativeEyeResolution::report() const {
    PreserveErrno preserve;const auto required=requiredMask();std::lock_guard lock(mutex_);std::ostringstream o;
    o<<"{\"policy\":\"initial-native-eye-capacity\",\"installed\":"<<installed_
     <<",\"requestedWidth\":"<<request_.width<<",\"requestedHeight\":"<<request_.height
     <<",\"activeWidth\":"<<active_.width<<",\"activeHeight\":"<<active_.height
     <<",\"desiredWorld\":"<<desiredWorld_<<",\"normalizedWidth\":"<<normalized_.width<<",\"normalizedHeight\":"<<normalized_.height
     <<",\"depthReady\":"<<depthGood_<<",\"depthAbsent\":"<<depthAbsent_<<",\"depthGeneration\":"<<depthGeneration_
     <<",\"completedMask\":"<<completedMask_<<",\"requiredMask\":"<<required
     <<",\"fault\":"<<fault_<<",\"requests\":"<<requests_<<",\"lateRequests\":"<<lateRequests_
     <<",\"initialScopes\":"<<scopeCalls_<<",\"maxScreenCalls\":"<<maxCalls_<<",\"expandedCalls\":"<<expandedCalls_
     <<",\"scopeFaults\":"<<scopeFaults_<<",\"updates\":"<<updates_<<",\"resizes\":"<<resizes_
     <<",\"rebuilds\":"<<rebuilds_<<",\"activations\":"<<activations_<<",\"declined\":"<<declined_
     <<",\"stockResizes\":"<<stockResizes_<<",\"stockNormalizations\":"<<stockNormalizations_<<",\"stockFailures\":"<<stockFailures_
     <<",\"verificationFailures\":"<<verificationFailures_<<'}';return o.str();
}
}
