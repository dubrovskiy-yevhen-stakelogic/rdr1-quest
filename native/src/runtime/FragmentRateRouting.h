#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace rdrvr::runtime {
// Metadata only, no Vulkan calls or handle ownership. Creation/destruction
// hooks must cover BOTH dispatch-table and GetProcAddr routes before binding.
// Maps remain alive through every referring framebuffer and its GPU commands.
class FragmentRateRouting {
public:
    struct Pass {unsigned attachments{};};
    struct Decision {bool converted{};VkImageView map{};int eye{-1};};
    struct Counts {std::uint64_t passDeclines{},viewDeclines{},eyeDeclines{},full{},radial{},mixed{};};
private:
    struct Eye {unsigned width{},height{},eye{};VkImageView map{};};
    struct View {VkImage image{};};
    mutable std::mutex mutex_;VkDevice device_{};VkImageView full_{};unsigned maximumWidth_{},maximumHeight_{};
    std::unordered_map<VkRenderPass,Pass> passes_;
    std::unordered_map<VkImageView,View> views_;
    std::unordered_map<VkImage,Eye> eyes_;
    Counts counts_{};
public:
    static constexpr std::size_t maxPasses=4096,maxViews=32768,maxEyes=64;
    bool bind(VkDevice device,VkImageView readyFullRateMap,unsigned maximumWidth,unsigned maximumHeight) noexcept {
        std::lock_guard lock(mutex_);
        if(device_||!device||!readyFullRateMap||!maximumWidth||!maximumHeight||maximumWidth>16384||maximumHeight>16384)return false;
        device_=device;full_=readyFullRateMap;maximumWidth_=maximumWidth;maximumHeight_=maximumHeight;return true;
    }
    // Register ONLY after successful creation with FragmentRatePassPlan.
    // False requires destroying the new pass and returning an allocation error;
    // an unrecorded transformed pass or incompatible fallback is never safe.
    bool addPass(VkDevice device,VkRenderPass pass,unsigned originalAttachments) noexcept {
        std::lock_guard lock(mutex_);
        if(device!=device_||!device_||!pass||!originalAttachments||originalAttachments>9||passes_.size()>=maxPasses||passes_.contains(pass)){++counts_.passDeclines;return false;}
        try{return passes_.emplace(pass,Pass{originalAttachments}).second;}catch(...){++counts_.passDeclines;return false;}
    }
    bool pass(VkDevice device,VkRenderPass pass,Pass& output)const noexcept {
        std::lock_guard lock(mutex_);if(device!=device_||!device_)return false;
        const auto found=passes_.find(pass);if(found==passes_.end())return false;output=found->second;return true;
    }
    void removePass(VkDevice device,VkRenderPass pass) noexcept {std::lock_guard lock(mutex_);if(device==device_)passes_.erase(pass);}
    bool addView(VkDevice device,const VkImageViewCreateInfo& info,VkImageView view) noexcept {
        std::lock_guard lock(mutex_);
        if(device!=device_||!device_)return false;
        const auto& r=info.subresourceRange;
        if(info.sType!=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO||info.pNext||info.flags||!info.image||!view||info.viewType!=VK_IMAGE_VIEW_TYPE_2D||
           r.baseMipLevel||r.levelCount!=1||r.baseArrayLayer||r.layerCount!=1||!r.aspectMask||
           (r.aspectMask&~(VK_IMAGE_ASPECT_COLOR_BIT|VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT))||views_.size()>=maxViews||views_.contains(view)){
            ++counts_.viewDeclines;return false;}
        try{return views_.emplace(view,View{info.image}).second;}catch(...){++counts_.viewDeclines;return false;}
    }
    void removeView(VkDevice device,VkImageView view) noexcept {std::lock_guard lock(mutex_);if(device==device_)views_.erase(view);}
    // Called for inspected engine-owned eye targets, never guessed from size,
    // view handles, creation order, a matching image name or current camera.
    bool addEye(VkDevice device,VkImage image,unsigned width,unsigned height,unsigned eye,VkImageView readyMap) noexcept {
        std::lock_guard lock(mutex_);
        if(device!=device_||!device_||!image||!width||!height||width>maximumWidth_||height>maximumHeight_||eye>=2||!readyMap){++counts_.eyeDeclines;return false;}
        const Eye next{width,height,eye,readyMap};const auto found=eyes_.find(image);
        if(found!=eyes_.end()){
            const auto& old=found->second;
            // Existing native framebuffers may retain this association. Never
            // silently redirect it to another extent/profile/eye generation.
            const bool same=old.width==width&&old.height==height&&old.eye==eye&&old.map==readyMap;
            if(!same)++counts_.eyeDeclines;return same;
        }
        if(eyes_.size()>=maxEyes){++counts_.eyeDeclines;return false;}
        try{return eyes_.emplace(image,next).second;}catch(...){++counts_.eyeDeclines;return false;}
    }
    void removeImage(VkDevice device,VkImage image) noexcept {
        std::lock_guard lock(mutex_);if(device!=device_||!device_)return;
        eyes_.erase(image);for(auto i=views_.begin();i!=views_.end();)if(i->second.image==image)i=views_.erase(i);else ++i;
    }
    VkImageView existingEyeMap(VkDevice device,VkImage image,unsigned width,unsigned height,unsigned eye)const noexcept {
        std::lock_guard lock(mutex_);if(!device_||device!=device_)return VK_NULL_HANDLE;
        const auto found=eyes_.find(image);if(found==eyes_.end())return VK_NULL_HANDLE;
        const auto& e=found->second;return e.width==width&&e.height==height&&e.eye==eye?e.map:VK_NULL_HANDLE;
    }
    Decision framebuffer(VkDevice device,const VkFramebufferCreateInfo& native) noexcept {
        std::lock_guard lock(mutex_);
        if(device!=device_||!device_)return {};
        const auto pass=passes_.find(native.renderPass);if(pass==passes_.end())return {};
        // A converted pass may NEVER be forwarded with its old attachment list.
        // Null map means reject the framebuffer call; no mismatched fallback.
        Decision result{true,VK_NULL_HANDLE,-1};
        if(native.sType!=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO||native.pNext||native.flags||native.layers!=1||
           !native.width||!native.height||native.width>maximumWidth_||native.height>maximumHeight_||
           native.attachmentCount!=pass->second.attachments||!native.pAttachments)return result;
        result.map=full_;const Eye* match=nullptr;
        for(unsigned i=0;i<native.attachmentCount;++i){
            const auto view=views_.find(native.pAttachments[i]);if(view==views_.end()){++counts_.full;return result;}
            const auto eye=eyes_.find(view->second.image);if(eye==eyes_.end()){++counts_.full;return result;}
            const auto& e=eye->second;
            if(e.width!=native.width||e.height!=native.height){++counts_.full;return result;}
            if(match&&(match->eye!=e.eye||match->map!=e.map)){++counts_.mixed;++counts_.full;return result;}
            match=&e;
        }
        if(match){result.map=match->map;result.eye=int(match->eye);++counts_.radial;}else ++counts_.full;
        return result;
    }
    Counts counts()const noexcept {std::lock_guard lock(mutex_);return counts_;}
    // Device lifecycle owner first retires GPU and destroys referencing objects.
    bool clear(VkDevice device) noexcept {std::lock_guard lock(mutex_);if(device!=device_||!device_)return false;
        passes_.clear();views_.clear();eyes_.clear();device_={};full_={};maximumWidth_=maximumHeight_=0;return true;}
};
}
