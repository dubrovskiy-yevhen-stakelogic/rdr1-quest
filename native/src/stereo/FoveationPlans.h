#pragma once
// Fixed foveated rendering for the eye passes. Pure creation plans and density maps (no Vulkan calls, no
// globals): testable off-device with fake create infos.
//
// Two device backends, mutually exclusive per VkDevice (Vulkan forbids enabling fragmentDensityMap together with the
// KHR pipeline/attachment fragment shading rate features):
//  - VRS  (VK_KHR_fragment_shading_rate attachment, GameFragmentRate path, 8x8 texels, true radial),
//  - FDM  (VK_EXT_fragment_density_map, this file: R8G8_UNORM density maps, bands; see buildDensityMap).
// The density map image is never subsampled (VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT is not used): colour/depth images stay
// full size, later passes sample them unchanged (fragmentDensityMapNonSubsampledImages is required and enabled).
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rdrvr::engine::foveation {
enum class Api : unsigned {Vrs=0,Fdm=1,None=2};
inline const char* apiName(Api a) noexcept {return a==Api::Vrs?"VRS":a==Api::Fdm?"FDM":"NONE";}
inline const char* levelName(unsigned level) noexcept {return level==1?"LOW":level==2?"MEDIUM":level==3?"HIGH":level==4?"ULTRA":"OFF";}
// 5 applied levels (OFF, LOW, MEDIUM, HIGH, ULTRA); the GRAPHICS menu value 5 = AUTO picks one of them per frame.
constexpr unsigned levelCount=5;
constexpr unsigned levelUltra=4,menuAuto=5;

// ---------------------------------------------------------------- render pass classification
// Only single-subpass passes with >=1 colour AND a depth/stencil attachment (the eye scene passes: depth/collector,
// light, colour, water) are density-map capable. Depth-only (shadow cascades) and colour-only (post, HUD) passes stay
// exactly as the engine made them. Every criterion below is invariant inside a Vulkan render-pass compatibility class
// (load/store ops and layouts are NOT used), so compatible passes are either all patched or all untouched and
// pipelines created against one of them stay compatible with the others.
enum class PassSkip : unsigned {None,Invalid,Flags,PNext,Subpasses,Attachments,Samples,NoColor,NoDepth,Input,Resolve,Preserve,Dependencies,Count};
inline const char* passSkipName(PassSkip s) noexcept {
    switch(s){case PassSkip::None:return "NONE";case PassSkip::Invalid:return "INVALID";case PassSkip::Flags:return "FLAGS";case PassSkip::PNext:return "PNEXT";
    case PassSkip::Subpasses:return "SUBPASSES";case PassSkip::Attachments:return "ATTACHMENTS";case PassSkip::Samples:return "SAMPLES";case PassSkip::NoColor:return "DEPTH_ONLY";
    case PassSkip::NoDepth:return "COLOR_ONLY";case PassSkip::Input:return "INPUT_ATTACHMENTS";case PassSkip::Resolve:return "RESOLVE";case PassSkip::Preserve:return "PRESERVE";
    case PassSkip::Dependencies:return "DEPENDENCIES";default:return "COUNT";}
}
inline bool depthStencilFormat(VkFormat f) noexcept {
    switch(f){case VK_FORMAT_D16_UNORM:case VK_FORMAT_X8_D24_UNORM_PACK32:case VK_FORMAT_D32_SFLOAT:case VK_FORMAT_S8_UINT:
    case VK_FORMAT_D16_UNORM_S8_UINT:case VK_FORMAT_D24_UNORM_S8_UINT:case VK_FORMAT_D32_SFLOAT_S8_UINT:return true;default:return false;}
}
inline bool stencilFormat(VkFormat f) noexcept {
    return f==VK_FORMAT_S8_UINT||f==VK_FORMAT_D16_UNORM_S8_UINT||f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_D32_SFLOAT_S8_UINT;
}
struct PassShape {unsigned attachments{},colors{};bool depth{},loads{};};
// Any attachment whose previous contents are loaded (LOAD colour/depth, or LOAD stencil of a stencil format).
inline bool passLoads(const VkRenderPassCreateInfo& c,unsigned count) noexcept {
    if(!c.pAttachments)return false;
    for(unsigned i=0;i<count&&i<c.attachmentCount;++i){const auto& a=c.pAttachments[i];
        if(a.loadOp==VK_ATTACHMENT_LOAD_OP_LOAD||(stencilFormat(a.format)&&a.stencilLoadOp==VK_ATTACHMENT_LOAD_OP_LOAD))return true;}
    return false;
}
// Shape of any VkRenderPassCreateInfo pass (for counters); classifyDensityPass decides eligibility.
inline PassShape passShape(const VkRenderPassCreateInfo& c) noexcept {
    PassShape s{};s.attachments=c.attachmentCount;
    if(c.subpassCount&&c.pSubpasses){const auto& sp=c.pSubpasses[0];
        for(unsigned i=0;i<sp.colorAttachmentCount&&sp.pColorAttachments;++i)if(sp.pColorAttachments[i].attachment!=VK_ATTACHMENT_UNUSED)++s.colors;
        s.depth=sp.pDepthStencilAttachment&&sp.pDepthStencilAttachment->attachment!=VK_ATTACHMENT_UNUSED;}
    s.loads=passLoads(c,c.attachmentCount);return s;
}
constexpr unsigned maxPassAttachments=8; // + the density map
inline PassSkip classifyDensityPass(const VkRenderPassCreateInfo& c,PassShape& out) noexcept {
    out={};
    if(c.sType!=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO)return PassSkip::Invalid;
    if(c.flags)return PassSkip::Flags;
    if(c.pNext)return PassSkip::PNext;
    if(c.subpassCount!=1||!c.pSubpasses)return PassSkip::Subpasses;
    if(!c.attachmentCount||c.attachmentCount>maxPassAttachments||!c.pAttachments)return PassSkip::Attachments;
    for(unsigned i=0;i<c.attachmentCount;++i){const auto& a=c.pAttachments[i];
        if(a.format==VK_FORMAT_UNDEFINED)return PassSkip::Attachments;
        if(a.samples!=VK_SAMPLE_COUNT_1_BIT)return PassSkip::Samples;}
    const auto& s=c.pSubpasses[0];
    if(s.flags||s.pipelineBindPoint!=VK_PIPELINE_BIND_POINT_GRAPHICS)return PassSkip::Subpasses;
    if(s.inputAttachmentCount)return PassSkip::Input;
    if(s.preserveAttachmentCount)return PassSkip::Preserve;
    if(s.colorAttachmentCount>maxPassAttachments||(s.colorAttachmentCount&&!s.pColorAttachments))return PassSkip::Attachments;
    unsigned colors=0;
    for(unsigned i=0;i<s.colorAttachmentCount;++i){const auto r=s.pColorAttachments[i].attachment;
        if(r==VK_ATTACHMENT_UNUSED)continue;if(r>=c.attachmentCount||depthStencilFormat(c.pAttachments[r].format))return PassSkip::Attachments;++colors;}
    if(s.pResolveAttachments)for(unsigned i=0;i<s.colorAttachmentCount;++i)if(s.pResolveAttachments[i].attachment!=VK_ATTACHMENT_UNUSED)return PassSkip::Resolve;
    const bool depth=s.pDepthStencilAttachment&&s.pDepthStencilAttachment->attachment!=VK_ATTACHMENT_UNUSED;
    if(depth&&(s.pDepthStencilAttachment->attachment>=c.attachmentCount||!depthStencilFormat(c.pAttachments[s.pDepthStencilAttachment->attachment].format)))return PassSkip::Attachments;
    if(!colors)return depth?PassSkip::NoColor:PassSkip::Attachments;
    if(!depth)return PassSkip::NoDepth;
    if(c.dependencyCount&&!c.pDependencies)return PassSkip::Dependencies;
    out.attachments=c.attachmentCount;out.colors=colors;out.depth=true;out.loads=passLoads(c,c.attachmentCount);
    return PassSkip::None;
}

// ---------------------------------------------------------------- creation plans (immediate-call, borrowed pointers)
// Extra attachment = the density map, appended LAST (index == native attachment count), referenced only by
// VkRenderPassFragmentDensityMapCreateInfoEXT (never by a subpass). loadOp LOAD / storeOp DONT_CARE (VUID-02550/02551),
// layout FRAGMENT_DENSITY_MAP_OPTIMAL both ways, so the pass never transitions the map.
constexpr VkFormat densityFormat=VK_FORMAT_R8G8_UNORM;
class DensityPassPlan {
    std::array<VkAttachmentDescription,maxPassAttachments+1> attachments_{};
    VkRenderPassFragmentDensityMapCreateInfoEXT density_{};
    VkRenderPassCreateInfo info_{};
    PassShape shape_{};PassSkip skip_{PassSkip::Invalid};
public:
    DensityPassPlan()=default;
    DensityPassPlan(const DensityPassPlan&)=delete;
    DensityPassPlan& operator=(const DensityPassPlan&)=delete;
    bool prepare(const VkRenderPassCreateInfo& native) noexcept {
        skip_=classifyDensityPass(native,shape_);if(skip_!=PassSkip::None)return false;
        for(unsigned i=0;i<native.attachmentCount;++i)attachments_[i]=native.pAttachments[i];
        auto& m=attachments_[native.attachmentCount];m={};m.format=densityFormat;m.samples=VK_SAMPLE_COUNT_1_BIT;
        m.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;m.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        m.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;m.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        m.initialLayout=m.finalLayout=VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
        density_={};density_.sType=VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT;
        density_.fragmentDensityMapAttachment={native.attachmentCount,VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
        info_=native;info_.pNext=&density_;info_.attachmentCount=native.attachmentCount+1;info_.pAttachments=attachments_.data();
        return true;
    }
    const VkRenderPassCreateInfo* info()const noexcept{return skip_==PassSkip::None?&info_:nullptr;}
    PassSkip skip()const noexcept{return skip_;}
    const PassShape& shape()const noexcept{return shape_;}
};
// Appends one view (density map or VRS rate map) to a framebuffer of a patched pass.
class AppendedFramebufferPlan {
    std::array<VkImageView,maxPassAttachments+2> views_{};VkFramebufferCreateInfo info_{};bool valid_{};
public:
    AppendedFramebufferPlan()=default;
    AppendedFramebufferPlan(const AppendedFramebufferPlan&)=delete;
    AppendedFramebufferPlan& operator=(const AppendedFramebufferPlan&)=delete;
    bool prepare(const VkFramebufferCreateInfo& native,unsigned nativeCount,VkImageView map) noexcept {
        valid_=false;
        if(native.sType!=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO||native.pNext||native.flags||!native.renderPass||!map||!nativeCount||
           nativeCount>maxPassAttachments+1||native.attachmentCount!=nativeCount||!native.pAttachments||!native.width||!native.height||!native.layers)return false;
        for(unsigned i=0;i<nativeCount;++i){if(!native.pAttachments[i])return false;views_[i]=native.pAttachments[i];}
        views_[nativeCount]=map;info_=native;info_.attachmentCount=nativeCount+1;info_.pAttachments=views_.data();valid_=true;return true;
    }
    const VkFramebufferCreateInfo* info()const noexcept{return valid_?&info_:nullptr;}
};

// Device: appends VK_EXT_fragment_density_map and its feature struct (fragmentDensityMap + NonSubsampledImages,
// never Dynamic). Declines when the native request already enables KHR fragment shading rate (mutually exclusive)
// or carries its own density-map feature struct.
struct DensitySupport {
    bool extension{},fdm2{},offsetQcom{},khrRate{};
    VkBool32 fragmentDensityMap{},dynamic{},nonSubsampled{},deferred{};
    VkExtent2D texelMin{},texelMax{};VkBool32 invocations{};
    VkFormatFeatureFlags linearFeatures{},optimalFeatures{};
    bool linearImage{};VkExtent3D linearMaxExtent{};
    bool usable()const noexcept{
        return extension&&fragmentDensityMap&&nonSubsampled&&texelMin.width&&texelMin.height&&texelMax.width>=texelMin.width&&texelMax.height>=texelMin.height&&
            (linearFeatures&VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT)&&linearImage;
    }
};
class DensityDevicePlan {
    std::array<const char*,130> extensions_{};
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT feature_{};
    VkDeviceCreateInfo info_{};bool valid_{};const char* status_{"NOT_PREPARED"};
public:
    DensityDevicePlan()=default;
    DensityDevicePlan(const DensityDevicePlan&)=delete;
    DensityDevicePlan& operator=(const DensityDevicePlan&)=delete;
    bool prepare(const VkDeviceCreateInfo& native,const DensitySupport& support) noexcept {
        valid_=false;
        if(native.sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO||native.enabledExtensionCount>128||(native.enabledExtensionCount&&!native.ppEnabledExtensionNames)){status_="NATIVE_INVALID";return false;}
        if(!support.usable()){status_="UNSUPPORTED";return false;}
        unsigned count=0;bool density=false;
        for(unsigned i=0;i<native.enabledExtensionCount;++i){
            const char* name=native.ppEnabledExtensionNames[i];if(!name){status_="NATIVE_INVALID";return false;}
            unsigned length=0;while(length<256&&name[length])++length;if(!length||length==256){status_="NATIVE_INVALID";return false;}
            if(!std::strcmp(name,VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)){status_="NATIVE_KHR_RATE";return false;}
            extensions_[count++]=name;density|=!std::strcmp(name,VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
        }
        if(!density)extensions_[count++]=VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME;
        std::array<const VkBaseInStructure*,32> visited{};unsigned visitedCount=0;
        for(auto* p=static_cast<const VkBaseInStructure*>(native.pNext);p;p=p->pNext){
            if(visitedCount==visited.size()){status_="NATIVE_CHAIN";return false;}
            for(unsigned i=0;i<visitedCount;++i)if(visited[i]==p){status_="NATIVE_CHAIN";return false;}
            visited[visitedCount++]=p;
            if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT){status_="NATIVE_DENSITY_FEATURES";return false;}
            if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR){
                const auto* f=reinterpret_cast<const VkPhysicalDeviceFragmentShadingRateFeaturesKHR*>(p);
                if(f->pipelineFragmentShadingRate||f->primitiveFragmentShadingRate||f->attachmentFragmentShadingRate){status_="NATIVE_KHR_RATE";return false;}}
            if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV&&
               reinterpret_cast<const VkPhysicalDeviceShadingRateImageFeaturesNV*>(p)->shadingRateImage){status_="NATIVE_NV_RATE";return false;}
        }
        feature_={};feature_.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT;feature_.pNext=const_cast<void*>(native.pNext);
        feature_.fragmentDensityMap=VK_TRUE;feature_.fragmentDensityMapNonSubsampledImages=VK_TRUE;feature_.fragmentDensityMapDynamic=VK_FALSE;
        info_=native;info_.pNext=&feature_;info_.enabledExtensionCount=count;info_.ppEnabledExtensionNames=extensions_.data();
        valid_=true;status_="FEATURES_REQUESTED";return true;
    }
    const VkDeviceCreateInfo* info()const noexcept{return valid_?&info_:nullptr;}
    const char* status()const noexcept{return status_;}
};

// ---------------------------------------------------------------- density maps
// Texel size: 32 px (measured on the Quest 3 Adreno 740),
// clamped into the device's [minFragmentDensityTexelSize, maxFragmentDensityTexelSize]. The map extent is
// ceil(framebuffer / texel), i.e. between ceil(fb/maxTexel) (VUID-VkFramebufferCreateInfo-pAttachments-02555/02556) and
// ceil(fb/minTexel).
inline unsigned densityTexel(VkExtent2D minTexel,VkExtent2D maxTexel,unsigned preferred=32) noexcept {
    const unsigned lo=std::max(minTexel.width,minTexel.height),hi=std::min(maxTexel.width,maxTexel.height);
    if(!lo||!hi||lo>hi)return 0;
    return std::clamp(preferred,lo,hi);
}
inline VkExtent2D densityMapExtent(unsigned width,unsigned height,unsigned texel) noexcept {
    if(!texel||!width||!height)return {};
    return {(width+texel-1)/texel,(height+texel-1)/texel};
}
// The Adreno 740 driver does not apply the density per map texel with regular (non-subsampled)
// attachments: it shades each tiler bin (a full-width horizontal strip, ~190-580 px high depending
// on the pass's attachment bytes) at the FINEST density found inside it. A radial map therefore
// only reduces the top/bottom strips. Profiles are horizontal bands measured from the top and
// bottom edges (the radial ladder along the centre column; the horizontal periphery keeps full
// rate), in steps of 0.1463 x eye height (196 px of a 1340 px eye), scaled by the optical centre row:
// rows above the centre use the upper half-height, rows below the lower one. Encoding: the same
// value in R (x) and G (y); 255 = full density. LOW 1/2 outer band; MEDIUM 1/4, 1/2; HIGH 1/8, 1/4,
// 1/2 (the driver realises 2x1 / 2x2 fragments); ULTRA = the HIGH bands (ULTRA is a VRS level).
inline float bandDensity(unsigned level,double edge,double height) noexcept {
    const double k=0.1463*height;
    switch(level){
    case 1:return edge<k?.5f:1.f;
    case 2:return edge<k?.25f:edge<2*k?.5f:1.f;
    case 3:case 4:return edge<k?.125f:edge<2*k?.25f:edge<3*k?.5f:1.f;
    default:return 1.f;}
}
struct DensityMap {
    unsigned width{},height{};                 // map texels
    std::vector<std::uint8_t> rg;              // tightly packed R8G8, row-major
    std::array<std::uint64_t,4> counts{};      // texels at density 1, 1/2, 1/4, 1/8
};
// centerY: optical centre row as a fraction of the eye height from the top (0.5 = image centre), from the XR view FOV.
inline bool buildDensityMap(unsigned level,unsigned eyeWidth,unsigned eyeHeight,unsigned texel,float centerY,DensityMap& out) noexcept {
    if(!eyeWidth||!eyeHeight||eyeWidth>16384||eyeHeight>16384||!texel||texel>256||level>=levelCount)return false;
    if(!std::isfinite(centerY)||centerY<.2f||centerY>.8f)centerY=.5f;
    const auto extent=densityMapExtent(eyeWidth,eyeHeight,texel);
    try{
        DensityMap next{};next.width=extent.width;next.height=extent.height;next.rg.assign(std::size_t(extent.width)*extent.height*2,255);
        const double h=eyeHeight,cy=double(centerY)*h;
        const double upper=.5*h/cy,lower=.5*h/(h-cy); // edge distance scale for rows above / below the optical centre
        for(unsigned y=0;y<extent.height;++y){
            const unsigned y0=y*texel,y1=std::min(eyeHeight,y0+texel);
            float d=0.f; // a texel takes the finest density of the rows it covers: a band edge never coarsens a finer row
            for(unsigned row=y0;row<y1;++row){
                const double top=double(row)*upper,bottom=double(eyeHeight-1-row)*lower;
                d=std::max(d,bandDensity(level,std::min(top,bottom),h));
            }
            if(y0>=y1)d=1.f;
            const auto v=std::uint8_t(std::lround(std::clamp(d,0.f,1.f)*255.f));
            const unsigned cls=v>=255?0:v>=127?1:v>=63?2:3;
            for(unsigned x=0;x<extent.width;++x){auto* p=&next.rg[(std::size_t(y)*extent.width+x)*2];p[0]=p[1]=v;++next.counts[cls];}
        }
        out=std::move(next);return true;
    }catch(...){return false;}
}
inline bool buildFullDensityMap(unsigned width,unsigned height,DensityMap& out) noexcept {
    if(!width||!height||width>4096||height>4096)return false;
    try{DensityMap next{};next.width=width;next.height=height;next.rg.assign(std::size_t(width)*height*2,255);next.counts[0]=std::uint64_t(width)*height;out=std::move(next);return true;}
    catch(...){return false;}
}
// Optical centre row (fraction from the top) of an XR view: tan(up)/(tan(up)-tan(down)).
inline float opticalCenterY(float angleUp,float angleDown) noexcept {
    if(!std::isfinite(angleUp)||!std::isfinite(angleDown)||angleUp<=0.01f||angleDown>=-0.01f||angleUp>1.55f||angleDown<-1.55f)return .5f;
    const double u=std::tan(double(angleUp)),d=std::tan(double(angleDown));
    const double c=u/(u-d);return std::isfinite(c)&&c>.2&&c<.8?float(c):.5f;
}

struct SweepPlan {
    static constexpr unsigned window=64;
    static constexpr std::array<unsigned,5> seq{0,1,2,3,4};
    static unsigned level(std::uint64_t tick) noexcept {return seq[(tick/window)%seq.size()];}
};
static_assert(SweepPlan::seq.size()==levelCount);
// FOVEATION AUTO GPU budget for the engine frame span (CmdBegin->CmdEnd, microseconds): LOCK 24 (frameRate 2) 40.4 ms;
// ADAPTIVE (frameRate 3) 22.7 ms; SPACEWARP really synthesizing 24.0 ms; otherwise (AUTO, LOCK 36) 26.6 ms = 36 FPS; minus
// 0.9 ms when FXAA ran (the mailbox FXAA runs outside the span but on the same GPU).
// The ADAPTIVE governor steps up at cost p95 <= 25.0 ms (span + ~0.8 ms mailbox lanes, i.e. a
// span p95 ~24.2 ms) and drops at p90 > 26.5 ms; a 26.6 ms AUTO budget holds the mean span in 23.6..26.6 ms, exactly where the
// governor can never step up. 22.7 ms = upNs - lanes - ~1.5 ms jitter margin keeps AUTO below the step-up line.
inline std::uint32_t autoBudgetUs(unsigned frameRate,bool spaceWarpForced,bool fxaa) noexcept {
    const std::uint32_t budget=frameRate==2?40400u:frameRate==3?22700u:spaceWarpForced?24000u:26600u;
    return fxaa?budget-900u:budget;
}
struct AutoParams {
    unsigned floor{1},ceiling{4},window{8},upReadings{4},downReadings{72},blackoutFrames{12},staleFrames{72};
    std::uint32_t budgetUs{26600},downMarginUs{3000};
    std::uint64_t dwellNs{2000000000},upDwellNs{500000000};
};
// FOVEATION AUTO: one level step at a time from the mean of the newest `window` GPU spans. Up (more foveation) after
// `upReadings` means over budget, down after `downReadings` means below budget - margin (3 ms hysteresis, larger than
// one level's 0.5-1.5 ms, so a stationary scene has no limit cycle). Dwell: 2 s before a down step or a reversal,
// 0.5 s between consecutive up steps (LOW -> ULTRA in ~1.5 s). After every change the
// readings of `blackoutFrames` frames are dropped (readback latency + the previous level still in flight).
// step() is called once per world frame with the level applied in that frame and returns the level for the next one.
class AutoController {
public:
    static constexpr unsigned ringMax=8;
    struct Stats {
        std::uint64_t readings{},staleReadings{},changes{},up{},down{},reversals{},overAtCeiling{},underAtFloor{};
        std::uint32_t smoothedUs{},lastUs{};
        std::array<std::uint64_t,levelCount> levelFrames{};
        const char* status{"IDLE"};
        std::uint64_t lastChangeNs{};
    };
    // Entering AUTO: the dynamic state restarts (the counters in stats() are kept for the report).
    void reset(unsigned level,std::uint64_t nowNs) noexcept {
        level_=level<levelCount?level:0;ringCount_=ringNext_=0;sinceChange_=staleRun_=above_=below_=0;lastDir_=Dir::None;
        stats_.lastChangeNs=nowNs;stats_.status="SETTLING";stats_.smoothedUs=0;
    }
    unsigned step(unsigned current,bool fresh,std::uint32_t us,std::uint64_t nowNs,const AutoParams& p) noexcept {
        const unsigned top=std::min(p.ceiling,levelCount-1),bottom=std::min(p.floor,top);
        const unsigned window=std::clamp(p.window,1u,ringMax);
        // a level outside [floor, ceiling] (floor raised by the property, entry from OFF) is a change by itself
        const unsigned level=std::clamp(current,bottom,top);
        if(level!=current){changed(level,nowNs,Dir::None);return level;}
        level_=level;
        // count the frame (per level, and since the last change)
        ++stats_.levelFrames[level];++sinceChange_;
        // no new GPU span this frame
        if(!fresh){++stats_.staleReadings;if(++staleRun_>=p.staleFrames)stats_.status="STALE";return level;}
        staleRun_=0;++stats_.readings;stats_.lastUs=us;
        // blackout after a change: the readings are dropped
        if(sinceChange_<p.blackoutFrames){stats_.status="SETTLING";return level;}
        // ring of the newest `window` spans (a new window size restarts it)
        if(window!=ringWindow_){ringWindow_=window;ringCount_=ringNext_=0;}
        ring_[ringNext_]=us;ringNext_=(ringNext_+1)%window;if(ringCount_<window)++ringCount_;
        if(ringCount_<window){stats_.status="FILLING";return level;}
        // mean of the ring: over budget, below budget - margin, or hold
        std::uint64_t sum=0;for(unsigned i=0;i<window;++i)sum+=ring_[i];
        const auto mean=std::uint32_t(sum/window);stats_.smoothedUs=mean;
        if(mean>p.budgetUs){++above_;below_=0;stats_.status="OVER";}
        else if(std::uint64_t(mean)+p.downMarginUs<p.budgetUs){++below_;above_=0;stats_.status="UNDER";}
        else{above_=below_=0;stats_.status="HOLD";}
        const std::uint64_t since=nowNs>stats_.lastChangeNs?nowNs-stats_.lastChangeNs:0;
        // up after upReadings over-budget means and the dwell (dwellNs after a down step, else upDwellNs)
        if(above_>=p.upReadings){
            if(level>=top)++stats_.overAtCeiling;
            else if(since>=(lastDir_==Dir::Down?p.dwellNs:p.upDwellNs)){++stats_.up;changed(level+1,nowNs,Dir::Up);return level+1;}
        }
        // down after downReadings under-budget means and dwellNs
        if(below_>=p.downReadings){
            if(level<=bottom)++stats_.underAtFloor;
            else if(since>=p.dwellNs){++stats_.down;changed(level-1,nowNs,Dir::Down);return level-1;}
        }
        return level;
    }
    const Stats& stats()const noexcept{return stats_;}
    unsigned level()const noexcept{return level_;}
private:
    enum class Dir : std::uint8_t {None,Up,Down};
    // every change restarts the ring, the blackout and the readings
    void changed(unsigned next,std::uint64_t nowNs,Dir dir) noexcept {
        ++stats_.changes;ringCount_=ringNext_=0;sinceChange_=0;above_=below_=0;
        if(dir!=Dir::None){if(lastDir_!=Dir::None&&dir!=lastDir_)++stats_.reversals;lastDir_=dir;}
        stats_.lastChangeNs=nowNs;stats_.status=dir==Dir::Up?"UP":dir==Dir::Down?"DOWN":"CLAMPED";level_=next;
    }
    std::array<std::uint32_t,ringMax> ring_{};unsigned ringCount_{},ringNext_{},ringWindow_{ringMax};
    unsigned sinceChange_{},staleRun_{},above_{},below_{},level_{};Dir lastDir_{Dir::None};
    Stats stats_{};
};
} // namespace rdrvr::engine::foveation
