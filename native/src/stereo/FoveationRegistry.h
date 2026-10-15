#pragma once
// Per-begin framebuffer substitution for the eye passes (both backends).
//
// Render passes are patched at creation (a foveation attachment appended LAST: the KHR rate map
// for VRS, the density map for FDM); every framebuffer of a patched pass is created with a uniform
// full-rate / full-density map. At vkCmdBeginRenderPass an EYE framebuffer (extent == the eye
// extent, pass with a depth attachment) is replaced by a mod-owned VARIANT framebuffer: same
// native views, only the last view = the map of the current level for that eye, created against
// the render pass being begun (alive by the engine's own begin; the creation-time pass may already
// be destroyed). Variants are created lazily (at most maxVariants per engine
// framebuffer; the least recently used one is evicted once the frame that last used it has retired
// on the GPU), registered with the stereo replay (so the right-eye replay resolves them like
// engine framebuffers) and destroyed with their engine framebuffer. Changing the level therefore
// needs no re-upload, no pass recreation and no GPU synchronisation: the next begin simply picks
// another variant. Same render pass object => pipeline and secondary-inheritance compatibility
// hold.
//
// FDM only: a pass instance that LOADs any attachment keeps the full-density map (the Adreno 740 rendered black bands
// for loaded content in reduced-density bins, Gothic port 0.0.28); debug.rdrvr.foveation.fdmload=1 lifts this.
//
// The right-eye replay reuses the left variant's foveation view (an unregistered "pass-through" attachment there);
// translate() maps a left-eye level view to the right-eye one (VRS radial maps differ per eye; FDM bands do not).
// translate() is lock-free: StereoReplay calls it under its cold mutex while begin() may call into StereoReplay.
#include "FoveationPlans.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rdrvr::engine::foveation {
struct RegistryRaw {
    PFN_vkCreateFramebuffer createFramebuffer{};PFN_vkDestroyFramebuffer destroyFramebuffer{};
    void (*registered)(VkDevice,const VkFramebufferCreateInfo*,VkFramebuffer,VkResult){}; // stereo::framebufferCreated
    void (*unregistered)(VkDevice,VkFramebuffer){};                                       // stereo::framebufferDestroyed
    void (*pairsChanged)(){};  // A left->right translation changed: cached right framebuffers must be re-resolved
    explicit operator bool()const noexcept{return createFramebuffer&&destroyFramebuffer;}
};
// view[eye][level]; level 0 = the uniform map every framebuffer of that extent was created with.
struct LevelViews {std::array<std::array<VkImageView,levelCount>,2> view{};bool operator==(const LevelViews&)const=default;};
class Registry {
public:
    static constexpr unsigned maxVariants=6,maxEye1Images=16,maxPairs=32;
    struct Counters {
        std::atomic<std::uint64_t> passes{},passesFoveated{},framebuffers{},framebuffersFoveated{},begins{},eyeBegins[2]{},substituted[2]{},same[2]{},
            loadFull{},clearEye{},loadEye{},notReady{},nonEye{},unknown{},unpatchedEye{},pNext{},variantsCreated{},variantFailures{},variantsDestroyed{},
            variantsFull{},variantsEvicted{},translated{},eyeGeneration{},pairChanges{},passMismatch{};
        std::array<std::atomic<std::uint64_t>,levelCount> levelBegins{}; // Eye begins by the level read at begin
    };
    // Every level (and the routing-map OFF in VRS) can hold a live variant per engine framebuffer, + 1 spare.
    static_assert(maxVariants>levelCount);
private:
    struct Pass {VkDevice device{};unsigned nativeCount{};bool depth{},loads{};};
    struct Variant {VkImageView map{};VkFramebuffer fb{};std::uint64_t lastUse{};}; // lastUse: frame serial of the last begin
    struct Fb {
        VkDevice device{};VkRenderPass pass{};std::uint32_t width{},height{},layers{};unsigned count{};
        std::array<VkImageView,maxPassAttachments+1> views{};VkImageView map{};bool depth{};
        int eye{-1};std::uint64_t eyeGeneration{};bool countedEye{};
        std::array<Variant,maxVariants> variants{};unsigned variantCount{};
    };
    mutable std::mutex mutex_;
    VkDevice device_{};Api api_{Api::None};RegistryRaw raw_{};
    std::unordered_map<VkRenderPass,Pass> passes_;
    std::unordered_map<VkFramebuffer,Fb> fbs_;
    std::unordered_map<VkImageView,VkImage> views_;
    VkExtent2D eyeExtent_{};std::array<VkImage,maxEye1Images> eye1_{};unsigned eye1Count_{};std::uint64_t eyeGeneration_{};
    LevelViews levels_{};
    std::atomic<unsigned> level_{};std::atomic<bool> allowLoad_{};
    // Frame serial of the latest world frame hook and the GPU retirement watermark (same units: renderFrameId).
    // A begin inside frame F before its hook is stamped F-1, so a variant is evictable when lastUse+2 <= retired.
    std::atomic<std::uint64_t> frame_{},retired_{};
    // lock-free translation table (append-only per device)
    std::array<std::atomic<VkImageView>,maxPairs> pairLeft_{},pairRight_{};std::atomic<unsigned> pairCount_{};
    std::atomic<std::uint64_t> eyeFramebuffers_{};
    int resolveEye(const Fb& r) const {
        for(unsigned i=0;i<r.count;++i){const auto v=views_.find(r.views[i]);if(v==views_.end())continue;
            for(unsigned k=0;k<eye1Count_;++k)if(eye1_[k]==v->second)return 1;}
        return 0;
    }
    // True when the table changed (a new pair, or a new right view for a known left view).
    bool addPairLocked(VkImageView left,VkImageView right) noexcept {
        if(!left||!right||left==right)return false;
        const unsigned n=pairCount_.load(std::memory_order_relaxed);
        for(unsigned i=0;i<n;++i)if(pairLeft_[i].load(std::memory_order_relaxed)==left){
            if(pairRight_[i].load(std::memory_order_relaxed)==right)return false;
            pairRight_[i].store(right,std::memory_order_release);return true;}
        if(n>=maxPairs)return false;
        pairRight_[n].store(right,std::memory_order_relaxed);pairLeft_[n].store(left,std::memory_order_relaxed);pairCount_.store(n+1,std::memory_order_release);
        return true;
    }
public:
    Counters counters;
    bool configure(VkDevice device,Api api,RegistryRaw raw) noexcept {
        std::lock_guard lock(mutex_);
        if(device_||!device||api==Api::None||!raw)return false;
        device_=device;api_=api;raw_=raw;return true;
    }
    VkDevice device()const noexcept{std::lock_guard lock(mutex_);return device_;}
    Api api()const noexcept{std::lock_guard lock(mutex_);return api_;}
    void setLevel(unsigned level) noexcept {level_.store(level<levelCount?level:0,std::memory_order_relaxed);}
    unsigned level()const noexcept{return level_.load(std::memory_order_relaxed);}
    void setAllowLoad(bool v) noexcept {allowLoad_.store(v,std::memory_order_relaxed);}
    void setFrame(std::uint64_t frame,std::uint64_t retired) noexcept {frame_.store(frame,std::memory_order_relaxed);retired_.store(retired,std::memory_order_relaxed);}
    bool allowLoad()const noexcept{return allowLoad_.load(std::memory_order_relaxed);}
    // ---- creation observers (any thread)
    void passCreated(VkDevice d,VkRenderPass pass,unsigned nativeCount,const PassShape& shape) noexcept {
        if(!pass)return;
        try{std::lock_guard lock(mutex_);if(d!=device_||!device_)return;
            passes_[pass]=Pass{d,nativeCount,shape.depth,shape.loads};++counters.passes;if(nativeCount)++counters.passesFoveated;}catch(...){}
    }
    void passDestroyed(VkDevice d,VkRenderPass pass) noexcept {std::lock_guard lock(mutex_);if(d==device_&&device_)passes_.erase(pass);}
    bool foveatedPass(VkDevice d,VkRenderPass pass,unsigned& nativeCount) const noexcept {
        std::lock_guard lock(mutex_);if(d!=device_||!device_)return false;
        const auto p=passes_.find(pass);if(p==passes_.end()||!p->second.nativeCount)return false;nativeCount=p->second.nativeCount;return true;
    }
    void viewCreated(VkDevice d,VkImageView view,VkImage image) noexcept {
        if(!view||!image)return;try{std::lock_guard lock(mutex_);if(d==device_&&device_)views_[view]=image;}catch(...){}
    }
    void viewDestroyed(VkDevice d,VkImageView view) noexcept {std::lock_guard lock(mutex_);if(d==device_&&device_)views_.erase(view);}
    // finalInfo: exactly what the driver received (map appended when the pass is foveated).
    void framebufferCreated(VkDevice d,const VkFramebufferCreateInfo* c,VkFramebuffer fb) noexcept {
        if(!c||!fb||(c->flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)||c->attachmentCount>maxPassAttachments+1||(c->attachmentCount&&!c->pAttachments))return;
        try{std::lock_guard lock(mutex_);if(d!=device_||!device_)return;
            Fb r{};r.device=d;r.pass=c->renderPass;r.width=c->width;r.height=c->height;r.layers=c->layers;r.count=c->attachmentCount;
            const auto p=passes_.find(c->renderPass);
            if(p!=passes_.end()){r.depth=p->second.depth;
                if(p->second.nativeCount&&c->attachmentCount==p->second.nativeCount+1){
                    r.count=p->second.nativeCount;r.map=c->pAttachments[r.count];++counters.framebuffersFoveated;}}
            for(unsigned i=0;i<r.count;++i)r.views[i]=c->pAttachments[i];
            fbs_[fb]=r;++counters.framebuffers;
        }catch(...){}
    }
    // Destroys the variants of an engine framebuffer (the engine destroys its own one only when unused by the GPU,
    // and a variant is used in exactly the command buffers where the engine framebuffer would have been).
    void framebufferDestroyed(VkDevice d,VkFramebuffer fb) noexcept {
        std::array<Variant,maxVariants> doomed{};unsigned n=0;RegistryRaw raw{};
        {std::lock_guard lock(mutex_);if(d!=device_||!device_)return;
            const auto f=fbs_.find(fb);if(f==fbs_.end())return;
            n=f->second.variantCount;doomed=f->second.variants;raw=raw_;fbs_.erase(f);}
        for(unsigned i=0;i<n;++i){if(raw.unregistered)raw.unregistered(d,doomed[i].fb);raw.destroyFramebuffer(d,doomed[i].fb,nullptr);++counters.variantsDestroyed;}
    }
    // ---- eyes (render thread, once per world frame when anything changed)
    void setEyes(VkExtent2D extent,const VkImage* eye1,unsigned count,const LevelViews& views) noexcept {
        bool pairs=false;void (*notify)()=nullptr;
        {std::lock_guard lock(mutex_);
            bool changed=extent.width!=eyeExtent_.width||extent.height!=eyeExtent_.height||count!=eye1Count_||!(views==levels_);
            for(unsigned i=0;i<count&&i<maxEye1Images;++i)changed|=eye1_[i]!=eye1[i];
            if(!changed)return;
            eyeExtent_=extent;eye1Count_=std::min(count,maxEye1Images);for(unsigned i=0;i<eye1Count_;++i)eye1_[i]=eye1[i];
            levels_=views;++eyeGeneration_;++counters.eyeGeneration;
            for(unsigned l=0;l<levelCount;++l)pairs|=addPairLocked(views.view[0][l],views.view[1][l]);
            notify=raw_.pairsChanged;}
        if(pairs){++counters.pairChanges;if(notify)notify();} // outside the lock: the callback touches the stereo replay
    }
    VkExtent2D eyeExtent()const noexcept{std::lock_guard lock(mutex_);return eyeExtent_;}
    // Lock-free: left-eye level view -> right-eye view of the same level (identity when unknown).
    VkImageView translate(VkImageView left) noexcept {
        const unsigned n=pairCount_.load(std::memory_order_acquire);
        for(unsigned i=0;i<n;++i)if(pairLeft_[i].load(std::memory_order_relaxed)==left){++counters.translated;return pairRight_[i].load(std::memory_order_acquire);}
        return left;
    }
    // ---- vkCmdBeginRenderPass (engine recording threads). True: use out (framebuffer replaced).
    // usedLevel (optional) = the level whose map the eye pass begins with (variant or its own map), -1 otherwise.
    bool begin(const VkRenderPassBeginInfo& in,VkRenderPassBeginInfo& out,int* usedLevel=nullptr) noexcept {
        ++counters.begins;if(usedLevel)*usedLevel=-1;int mapLevel=-1;
        VkDevice device{};VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};std::array<VkImageView,maxPassAttachments+1> list{};
        VkImageView wanted{};int eye{};RegistryRaw raw{};Variant evicted{};
        const std::uint64_t frame=frame_.load(std::memory_order_relaxed),retired=retired_.load(std::memory_order_relaxed);
        try{
            std::lock_guard lock(mutex_);
            if(!device_)return false;
            const auto f=fbs_.find(in.framebuffer);if(f==fbs_.end()){++counters.unknown;return false;}
            auto& r=f->second;
            const bool eyeExtent=eyeExtent_.width&&r.width==eyeExtent_.width&&r.height==eyeExtent_.height&&r.layers==1&&r.depth;
            if(!eyeExtent){++counters.nonEye;return false;}
            if(!r.map){++counters.unpatchedEye;return false;}
            if(in.pNext){++counters.pNext;return false;}
            if(r.eye<0||r.eyeGeneration!=eyeGeneration_){r.eye=resolveEye(r);r.eyeGeneration=eyeGeneration_;}
            eye=r.eye;++counters.eyeBegins[eye];
            if(!r.countedEye){r.countedEye=true;eyeFramebuffers_.fetch_add(1,std::memory_order_relaxed);}
            const unsigned level=level_.load(std::memory_order_relaxed);
            ++counters.levelBegins[level<levelCount?level:0];
            // The begun pass must be a foveated member of the framebuffer's class (same native attachment count):
            // a variant is created against it below.
            const auto p=passes_.find(in.renderPass);
            if(p==passes_.end()||p->second.nativeCount!=r.count){++counters.passMismatch;return false;}
            const bool loads=p->second.loads;
            if(loads)++counters.loadEye;else ++counters.clearEye;
            wanted=levels_.view[eye][level];
            if(api_==Api::Fdm&&level&&loads&&!allowLoad_.load(std::memory_order_relaxed)){wanted=levels_.view[eye][0];++counters.loadFull;}
            if(!wanted){++counters.notReady;return false;}
            mapLevel=wanted==levels_.view[eye][0]?0:int(level<levelCount?level:0);
            if(wanted==r.map){++counters.same[eye];if(usedLevel)*usedLevel=mapLevel;return false;}
            for(unsigned i=0;i<r.variantCount;++i)if(r.variants[i].map==wanted){
                auto& v=r.variants[i];v.lastUse=std::max(v.lastUse,frame);out=in;out.framebuffer=v.fb;++counters.substituted[eye];if(usedLevel)*usedLevel=mapLevel;return true;}
            if(r.variantCount>=maxVariants){
                // Evict the least recently used variant whose last frame has retired on the GPU (never one that a
                // pending command buffer may still reference); none evictable -> no foveation for this begin.
                unsigned victim=maxVariants;
                for(unsigned i=0;i<r.variantCount;++i)if(r.variants[i].lastUse+2<=retired&&(victim==maxVariants||r.variants[i].lastUse<r.variants[victim].lastUse))victim=i;
                if(victim==maxVariants){++counters.variantsFull;return false;}
                evicted=r.variants[victim];r.variants[victim]=r.variants[--r.variantCount];r.variants[r.variantCount]={};
            }
            device=r.device;raw=raw_;
            for(unsigned i=0;i<r.count;++i)list[i]=r.views[i];list[r.count]=wanted;
            // Created against the pass being begun (alive and compatible by the engine's own begin), not the creation-time r.pass.
            ci.renderPass=in.renderPass;ci.attachmentCount=r.count+1;ci.width=r.width;ci.height=r.height;ci.layers=r.layers;
        }catch(...){return false;}
        if(evicted.fb){if(raw.unregistered)raw.unregistered(device,evicted.fb);raw.destroyFramebuffer(device,evicted.fb,nullptr);++counters.variantsDestroyed;++counters.variantsEvicted;}
        ci.pAttachments=list.data();
        if(!ci.renderPass){++counters.variantFailures;return false;}
        VkFramebuffer created{};
        if(raw.createFramebuffer(device,&ci,nullptr,&created)!=VK_SUCCESS||!created){++counters.variantFailures;return false;}
        if(raw.registered)raw.registered(device,&ci,created,VK_SUCCESS);
        ++counters.variantsCreated;
        VkFramebuffer use=created,discard{};
        {
            std::lock_guard lock(mutex_);
            const auto f=fbs_.find(in.framebuffer);
            if(f==fbs_.end()||f->second.variantCount>=maxVariants)discard=created,use=VK_NULL_HANDLE;
            else{auto& r=f->second;
                for(unsigned i=0;i<r.variantCount;++i)if(r.variants[i].map==wanted){discard=created;use=r.variants[i].fb;break;} // a concurrent begin won
                if(!discard)r.variants[r.variantCount++]={wanted,created,frame};}
        }
        if(discard){if(raw.unregistered)raw.unregistered(device,discard);raw.destroyFramebuffer(device,discard,nullptr);++counters.variantsDestroyed;}
        if(!use)return false;
        out=in;out.framebuffer=use;++counters.substituted[eye];if(usedLevel)*usedLevel=mapLevel;return true;
    }
    std::uint64_t eyeFramebuffers()const noexcept{return eyeFramebuffers_.load(std::memory_order_relaxed);}
    struct Sizes {std::size_t passes{},framebuffers{},views{},variants{};};
    Sizes sizes()const noexcept{std::lock_guard lock(mutex_);Sizes s{passes_.size(),fbs_.size(),views_.size(),0};for(const auto& [k,v]:fbs_)s.variants+=v.variantCount;return s;}
    // Device lifecycle owner, after vkDeviceWaitIdle: destroys every variant and forgets the device.
    void clear(VkDevice d) noexcept {
        std::vector<Variant> doomed;RegistryRaw raw{};
        {std::lock_guard lock(mutex_);if(d!=device_||!device_)return;
            try{for(const auto& [k,v]:fbs_)for(unsigned i=0;i<v.variantCount;++i)doomed.push_back(v.variants[i]);}catch(...){}
            raw=raw_;passes_.clear();fbs_.clear();views_.clear();eyeExtent_={};eye1Count_=0;levels_={};device_={};api_=Api::None;raw_={};
            // The caller unhooked the stereo translator first; a stale table must not survive into a new device.
            pairCount_.store(0,std::memory_order_release);
            for(unsigned i=0;i<maxPairs;++i){pairLeft_[i].store(VK_NULL_HANDLE,std::memory_order_relaxed);pairRight_[i].store(VK_NULL_HANDLE,std::memory_order_relaxed);}
            frame_.store(0,std::memory_order_relaxed);retired_.store(0,std::memory_order_relaxed);}
        for(const auto& v:doomed){if(raw.unregistered)raw.unregistered(d,v.fb);raw.destroyFramebuffer(d,v.fb,nullptr);++counters.variantsDestroyed;}
    }
};
} // namespace rdrvr::engine::foveation
