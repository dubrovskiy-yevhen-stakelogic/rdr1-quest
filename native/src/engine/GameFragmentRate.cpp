#include "GameFragmentRate.h"
#include "runtime/FragmentRateImage.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <sstream>
#include <vector>
#include <android/log.h>

namespace rdrvr::engine {
namespace {
runtime::FragmentRateBridge bridge;
std::mutex mutex;
VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};std::uint32_t family{};
unsigned maxWidth{},maxHeight{};bool ready{};const char* status="NOT_REQUESTED";
std::uint64_t registered{},declined{};
bool featuresRequested{}; // The device plan enabled pipeline + attachment fragment shading rates
struct Map {
    unsigned width{},height{},eye{};int centerX{},centerY{},inner{},outer{}; // inner/outer: radii x1000 (0 = Conservative)
    int vScale{1000};std::array<std::uint64_t,3> tiles{}; // Vertical scale x1000 (cache key), 1x1/2x2/4x4 tile counts
    runtime::FragmentRateImage image;VkImageView view{};
    VkCommandPool pool{};VkFence fence{};
};
std::vector<std::unique_ptr<Map>> maps;
// 1 full-rate map + per projection centre 2 eyes x (routing conservative60/85 + LOW/MEDIUM/HIGH/ULTRA).
// 51 = 1 + 10 (first centre: 2 routing maps) + 5 x 8 (later centres that reuse the routing maps).
// The cache never evicts (MAP_CACHE_FULL).
constexpr unsigned maxMaps=51;
// Telemetry: the device's fragment shading rates and rate properties (vkGetPhysicalDeviceFragmentShadingRatesKHR).
std::array<VkPhysicalDeviceFragmentShadingRateKHR,16> rateList{};unsigned rateListCount{},rateListTotal{};
VkPhysicalDeviceFragmentShadingRatePropertiesKHR rateProps{};bool ratePropsValid{},rateLogged{};
void logRatesOnce() noexcept {
    if(rateLogged)return;rateLogged=true;
    char list[400]{};std::size_t used=0;
    for(unsigned i=0;i<rateListCount;++i){const auto& r=rateList[i];
        const int n=std::snprintf(list+used,sizeof list-used,"%s%ux%u:0x%x",i?" ":"",r.fragmentSize.width,r.fragmentSize.height,unsigned(r.sampleCounts));
        if(n<0||std::size_t(n)>=sizeof list-used)break;
        used+=std::size_t(n);}
    const auto& p=rateProps;
    __android_log_print(ANDROID_LOG_INFO,"RdrVrVRS","rates n=%u: %s maxSize %ux%u aspect %u texel %ux%u..%ux%u coverage %u nonTrivial %u",rateListTotal,list,
        p.maxFragmentSize.width,p.maxFragmentSize.height,p.maxFragmentSizeAspectRatio,p.minFragmentShadingRateAttachmentTexelSize.width,p.minFragmentShadingRateAttachmentTexelSize.height,
        p.maxFragmentShadingRateAttachmentTexelSize.width,p.maxFragmentShadingRateAttachmentTexelSize.height,p.maxFragmentShadingRateCoverageSamples,unsigned(p.fragmentShadingRateNonTrivialCombinerOps));
}
bool rateListed(unsigned w,unsigned h) noexcept {
    for(unsigned i=0;i<rateListCount;++i)if(rateList[i].fragmentSize.width==w&&rateList[i].fragmentSize.height==h&&(rateList[i].sampleCounts&VK_SAMPLE_COUNT_1_BIT))return true;
    return false;
}
runtime::FragmentRateImageApi imageApi(){return {vkGetPhysicalDeviceFormatProperties,vkGetPhysicalDeviceImageFormatProperties,vkGetPhysicalDeviceMemoryProperties,
    vkCreateImage,vkDestroyImage,vkGetImageMemoryRequirements,vkAllocateMemory,vkFreeMemory,vkBindImageMemory,vkMapMemory,vkUnmapMemory,vkGetImageSubresourceLayout,vkCreateImageView,vkDestroyImageView};}
bool upload(Map& target,const runtime::FragmentRateMap& pixels){
    if(!target.image.create(imageApi(),physical,device,pixels)){status=target.image.status();return false;}
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool.queueFamilyIndex=family;pool.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if(vkCreateCommandPool(device,&pool,nullptr,&target.pool)!=VK_SUCCESS){status="MAP_COMMAND_POOL_FAILED";return false;}
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocate.commandPool=target.pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
    VkCommandBuffer cmd{};if(vkAllocateCommandBuffers(device,&allocate,&cmd)!=VK_SUCCESS){status="MAP_COMMAND_BUFFER_FAILED";return false;}
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(cmd,&begin)!=VK_SUCCESS){status="MAP_COMMAND_BEGIN_FAILED";return false;}
    VkImageMemoryBarrier barrier{};
    if(!target.image.beginInitialization(1,barrier)){status="MAP_INITIALIZATION_FAILED";return false;}
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,0,0,nullptr,0,nullptr,1,&barrier);
    if(vkEndCommandBuffer(cmd)!=VK_SUCCESS){status="MAP_COMMAND_END_FAILED";return false;}
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if(vkCreateFence(device,&fence,nullptr,&target.fence)!=VK_SUCCESS){status="MAP_FENCE_FAILED";return false;}
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;
    if(vkQueueSubmit(queue,1,&submit,target.fence)!=VK_SUCCESS){status="MAP_SUBMIT_FAILED";return false;}
    // This queue belongs to the XR owner, not to the game's render submissions.
    // The immutable host-filled image is never written by another GPU queue.
    if(vkWaitForFences(device,1,&target.fence,VK_TRUE,2000000000ULL)!=VK_SUCCESS){status="MAP_WAIT_FAILED";return false;}
    if(!target.image.finishInitialization(1,true)){status="MAP_COMPLETION_FAILED";return false;}
    target.view=target.image.publishView();
    vkDestroyFence(device,target.fence,nullptr);target.fence={};vkDestroyCommandPool(device,target.pool,nullptr);target.pool={};return bool(target.view);
}
// FOVEATION levels 1..4 (LOW/MEDIUM/HIGH/ULTRA): 1x1 inside the inner ellipse, 2x2 inside the outer one,
// 4x4 beyond (fractions of the half extents around the optical centre; runtime::fragmentRateLevels). Level 0 is the
// full-rate map (maps[0]).
FragmentRateLevelViews levelViews{};unsigned levelWidth{},levelHeight{};std::uint64_t levelMapFailures{},ultraFallbackFrames{};
constexpr unsigned levelUltra=runtime::fragmentRateLevelCount-1;
static_assert(levelUltra==4&&runtime::fragmentRateLevels[3].inner==400&&runtime::fragmentRateLevels[3].outer==650,"HIGH must stay {400,650}");
VkImageView eyeMap(unsigned width,unsigned height,unsigned eye,runtime::FragmentRateCenter center,std::mutex& queueMutex,int inner=0,int outer=0,int vScale=1000){
    // The optical centre is quantized to 1/64 of the extent (key and map, in 1/1024 units): small projection
    // changes (EDGE MARGIN, FOV jitter) reuse the same maps instead of filling the map cache and the variant slots.
    const int x=int(std::lround(center.x*64))*16,y=int(std::lround(center.y*64))*16;
    for(const auto& map:maps)if(map->width==width&&map->height==height&&map->eye==eye&&map->centerX==x&&map->centerY==y&&map->inner==inner&&map->outer==outer&&map->vScale==vScale)return map->view;
    if(maps.size()>=maxMaps){status="MAP_CACHE_FULL";return VK_NULL_HANDLE;}
    runtime::FragmentRateMapSpec spec{width,height,8,8,1,runtime::FragmentRateProfile::Conservative};spec.centers[0]={float(x)/1024,float(y)/1024};
    spec.inner=inner/1000.;spec.outer=outer/1000.;spec.verticalScale=vScale/1000.;
    runtime::FragmentRateMap pixels;if(!runtime::buildFragmentRateMap(spec,pixels)){status="MAP_SPEC_DECLINED";return VK_NULL_HANDLE;}
    auto entry=std::make_unique<Map>();entry->width=width;entry->height=height;entry->eye=eye;entry->centerX=x;entry->centerY=y;entry->inner=inner;entry->outer=outer;
    entry->vScale=vScale;entry->tiles=pixels.tileCounts;
    auto* result=entry.get();maps.push_back(std::move(entry));
    // Failed entries remain cached and owned until device destruction: no
    // retries every frame, no freeing command resources after a fence timeout.
    std::lock_guard queueGuard(queueMutex);if(!upload(*result,pixels))return VK_NULL_HANDLE;
    return result->view;
}
}
runtime::FragmentRateBridge& gameFragmentRateBridge() noexcept{return bridge;}
bool gameFragmentRateDevicePlan(VkInstance instance,VkPhysicalDevice gpu,const VkDeviceCreateInfo& native,runtime::FragmentRateDevicePlan& plan) noexcept {
    std::lock_guard guard(mutex);
    try{
        if(device||!instance||!gpu){status="DEVICE_ALREADY_BOUND_OR_UNKNOWN";return false;}
        unsigned count{};if(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&count,nullptr)!=VK_SUCCESS||!count||count>1024){status="EXTENSION_QUERY_FAILED";return false;}
        std::vector<VkExtensionProperties> extensions(count);if(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&count,extensions.data())!=VK_SUCCESS||count>extensions.size()){status="EXTENSION_QUERY_FAILED";return false;}
        bool rate=false,pass2=false;for(const auto& ext:extensions){rate|=!std::strcmp(ext.extensionName,VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);pass2|=!std::strcmp(ext.extensionName,VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);}
        if(!rate||!pass2){status="EXTENSIONS_UNAVAILABLE";return false;}
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR support{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&support;vkGetPhysicalDeviceFeatures2(gpu,&features);
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR rateProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&rateProperties;vkGetPhysicalDeviceProperties2(gpu,&properties);
        const auto& rp=rateProperties;
        rateProps=rateProperties;rateProps.pNext=nullptr;ratePropsValid=true; // Reported even when rejected below
        if(rp.minFragmentShadingRateAttachmentTexelSize.width>8||rp.minFragmentShadingRateAttachmentTexelSize.height>8||rp.maxFragmentShadingRateAttachmentTexelSize.width<8||rp.maxFragmentShadingRateAttachmentTexelSize.height<8||
           !rp.fragmentShadingRateWithShaderDepthStencilWrites||!rp.fragmentShadingRateWithSampleMask||!rp.fragmentShadingRateWithShaderSampleMask||!rp.fragmentShadingRateNonTrivialCombinerOps){status="RATE_PROPERTIES_UNSUPPORTED";return false;}
        auto getRates=reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceFragmentShadingRatesKHR"));
        std::array<VkPhysicalDeviceFragmentShadingRateKHR,64> rates{};for(auto& r:rates)r.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
        count=rates.size();if(!getRates||getRates(gpu,&count,rates.data())!=VK_SUCCESS||count>rates.size()){status="RATE_ENUMERATION_FAILED";return false;}
        rateListTotal=count;rateListCount=std::min<unsigned>(count,unsigned(rateList.size())); // Report + one log line
        for(unsigned i=0;i<rateListCount;++i){rateList[i]=rates[i];rateList[i].pNext=nullptr;}
        logRatesOnce();
        unsigned mask=0;for(unsigned i=0;i<count;++i){const auto& r=rates[i];if(!(r.sampleCounts&VK_SAMPLE_COUNT_1_BIT)||r.fragmentSize.width!=r.fragmentSize.height)continue;
            if(r.fragmentSize.width==1)mask|=1;if(r.fragmentSize.width==2)mask|=2;if(r.fragmentSize.width==4)mask|=4;}
        if(mask!=7){status="RATE_SIZES_UNSUPPORTED";return false;}
        maxWidth=std::min(properties.properties.limits.maxFramebufferWidth,16384u);maxHeight=std::min(properties.properties.limits.maxFramebufferHeight,16384u);
        if(!maxWidth||!maxHeight||!plan.prepare(native,rate,pass2,support)){status="NATIVE_FEATURE_CHAIN_DECLINED";return false;}
        status="FEATURES_REQUESTED";featuresRequested=true;return true;
    }catch(...){status="CAPABILITY_EXCEPTION";return false;}
}
bool gameFragmentRateInitialize(VkPhysicalDevice gpu,VkDevice d,std::uint32_t queueFamily,std::uint32_t queueIndex) noexcept {
    std::lock_guard guard(mutex);
    try{
        if(device||!gpu||!d||!maxWidth||!maxHeight)return false;
        physical=gpu;device=d;family=queueFamily;vkGetDeviceQueue(device,family,queueIndex,&queue);
        if(!queue){status="PRIVATE_QUEUE_UNAVAILABLE";return false;}
        auto createPass2=reinterpret_cast<PFN_vkCreateRenderPass2KHR>(vkGetDeviceProcAddr(device,"vkCreateRenderPass2KHR"));
        runtime::FragmentRateBridgeApi api{vkCreateRenderPass,createPass2,vkDestroyRenderPass,vkCreateFramebuffer,vkCreateGraphicsPipelines,vkCreateImageView,vkDestroyImageView};
        if(!api){status="CREATION_API_UNAVAILABLE";return false;}
        maps.reserve(maxMaps);auto full=std::make_unique<Map>();full->width=maxWidth;full->height=maxHeight;full->eye=2;
        runtime::FragmentRateMap pixels{};pixels.width=(maxWidth+7)/8;pixels.height=(maxHeight+7)/8;pixels.layers=1;pixels.pixels.resize(std::size_t(pixels.width)*pixels.height,0);
        auto* entry=full.get();maps.push_back(std::move(full));if(!upload(*entry,pixels))return false;
        if(!bridge.configure(api,device,{8,8},entry->view,maxWidth,maxHeight)){status="BRIDGE_CONFIGURE_FAILED";return false;}
        ready=true;runtime::fragmentRateReadiness.store(1,std::memory_order_relaxed);status="FULL_RATE_READY";__android_log_print(ANDROID_LOG_INFO,"RdrVrVRS","ready full-map=%ux%u eye-profile=conservative60/85",maxWidth,maxHeight);return true;
    }catch(...){status="INITIALIZATION_EXCEPTION";return false;}
}
void gameFragmentRateEyes(const EngineEyeTargets& targets,const StereoCameraPlan& plan,std::mutex& queueMutex) noexcept {
    std::lock_guard guard(mutex);
    if(!ready||targets.eyeCount()!=2)return;
    try{
        bool bothReady=true;
        for(unsigned eye=0;eye<2;++eye){
            const auto* color=targets.image(eye);if(!color||color->image.device!=device){bothReady=false;continue;}
            runtime::Mat4 canonical=plan.projection[eye];if(canonical[5]>0){canonical[5]*=-1;canonical[9]*=-1;}
            runtime::FragmentRateCenter center{};if(!runtime::fragmentRateCenter(canonical,center)){++declined;bothReady=false;continue;}
            const unsigned width=color->image.extent.width,height=color->image.extent.height;
            auto map=bridge.routing().existingEyeMap(device,color->image.image,width,height,eye);
            // A framebuffer may already reference this immutable map. A new
            // map/profile takes effect only with a new native target generation.
            if(!map)map=eyeMap(width,height,eye,center,queueMutex);
            if(!map){++declined;bothReady=false;continue;}
            // The per-level maps of this eye (created once per extent/centre; immutable afterwards).
            if(levelWidth!=width||levelHeight!=height){levelViews={};levelWidth=width;levelHeight=height;}
            levelViews[eye][0]=maps.empty()?VK_NULL_HANDLE:maps[0]->view;
            for(unsigned level=1;level<runtime::fragmentRateLevelCount;++level){
                const auto& l=runtime::fragmentRateLevels[level];
                const auto view=eyeMap(width,height,eye,center,queueMutex,l.inner,l.outer,l.verticalScale);
                if(view)levelViews[eye][level]=view;else ++levelMapFailures;
            }
            for(auto role:{EngineTargetRole::Color,EngineTargetRole::Depth,EngineTargetRole::ShadowCollector,EngineTargetRole::LightAccumulation,EngineTargetRole::SampledDepth,EngineTargetRole::WaterDepth,EngineTargetRole::Refraction}){
                const auto* image=targets.image(eye,role);if(!image)continue;
                if(image->image.device==device&&image->image.extent.width==width&&image->image.extent.height==height&&image->image.layers==1&&
                   bridge.routing().addEye(device,image->image.image,width,height,eye,map))++registered;else {++declined;bothReady=false;}
            }
        }
        if(bothReady)status="EYE_MAPS_READY";
    }catch(...){++declined;status="EYE_MAP_EXCEPTION";}
}
bool gameFragmentRateLevelViews(unsigned width,unsigned height,FragmentRateLevelViews& out) noexcept {
    std::lock_guard guard(mutex);
    if(!ready||!width||width!=levelWidth||height!=levelHeight)return false;
    for(unsigned eye=0;eye<2;++eye)for(unsigned level=0;level<levelUltra;++level)if(!levelViews[eye][level])return false;
    out=levelViews;
    // Without both ULTRA maps (cache full / upload failure) ULTRA uses the HIGH maps of both eyes, so OFF..HIGH
    // never depend on the new level.
    if(!out[0][levelUltra]||!out[1][levelUltra]){for(auto& eye:out)eye[levelUltra]=eye[levelUltra-1];++ultraFallbackFrames;}
    return true;
}
bool gameFragmentRateLevelViews(unsigned width,unsigned height,std::array<std::array<VkImageView,4>,2>& out) noexcept {
    FragmentRateLevelViews all{};if(!gameFragmentRateLevelViews(width,height,all))return false;
    for(unsigned eye=0;eye<2;++eye)for(unsigned level=0;level<4;++level)out[eye][level]=all[eye][level];
    return true;
}
void gameFragmentRateDestroy(VkDevice d) noexcept {
    std::lock_guard guard(mutex);if(!device||device!=d)return;
    // No worker remains. Game called device destruction, so native references
    // must already be retired; this also covers failed startup uploads.
    const auto result=vkDeviceWaitIdle(d);
    if(result!=VK_SUCCESS&&result!=VK_ERROR_DEVICE_LOST){ready=false;status="SHUTDOWN_QUARANTINED";return;}
    bridge.clear(d);
    for(auto& map:maps){if(map->fence)vkDestroyFence(d,map->fence,nullptr);if(map->pool)vkDestroyCommandPool(d,map->pool,nullptr);map->image.destroyAfterDeviceIdle(d);}
    maps.clear();levelViews={};levelWidth=levelHeight=0;ready=false;runtime::fragmentRateReadiness.store(0,std::memory_order_relaxed);device={};physical={};queue={};status="DEVICE_DESTROYED";
}
std::string gameFragmentRateReport(){
    std::lock_guard guard(mutex);const auto c=bridge.routing().counts();std::ostringstream out;
    out<<"{\"status\":\""<<status<<"\",\"ready\":"<<(ready?"true":"false")<<",\"profile\":\"conservative60/85\",\"maps\":"<<maps.size()
       <<",\"imageRegistrationCalls\":"<<registered<<",\"eyeDeclines\":"<<declined<<",\"convertedPasses\":"<<bridge.convertedPasses()<<",\"unmodifiedPasses\":"<<bridge.fallbackPasses()
       <<",\"convertedPipelines\":"<<bridge.convertedPipelines()<<",\"pipelineDeclines\":"<<bridge.declinedPipelines()<<",\"fullFramebuffers\":"<<c.full<<",\"radialFramebuffers\":"<<c.radial
       <<",\"invalidFramebuffers\":"<<bridge.invalidFrames()<<",\"levelMapFailures\":"<<levelMapFailures<<",\"levelExtent\":["<<levelWidth<<","<<levelHeight<<"],\"pixelExtentChanged\":false";
    // Telemetry only
    out<<",\"mapCapacity\":"<<maxMaps<<",\"ultraFallbackFrames\":"<<ultraFallbackFrames
       <<",\"largeBatches\":"<<bridge.largeBatches()<<",\"largeBatchPipelines\":"<<bridge.largeBatchPipelines()<<",\"largeBatchRoutable\":"<<bridge.largeBatchRoutable()
       <<",\"largeBatchMax\":"<<bridge.largeBatchMax()<<",\"unroutedPipelines\":"<<bridge.unroutedPipelines()<<",\"rates\":[";
    for(unsigned i=0;i<rateListCount;++i){const auto& r=rateList[i];char mask[16];std::snprintf(mask,sizeof mask,"0x%x",unsigned(r.sampleCounts));
        out<<(i?",":"")<<'['<<r.fragmentSize.width<<','<<r.fragmentSize.height<<",\""<<mask<<"\"]";}
    out<<"],\"ratesTotal\":"<<rateListTotal;
    auto b=[](bool v){return v?"true":"false";};
    out<<",\"nonSquare1x\":{\"2x1\":"<<b(rateListed(2,1))<<",\"1x2\":"<<b(rateListed(1,2))<<",\"4x2\":"<<b(rateListed(4,2))<<",\"2x4\":"<<b(rateListed(2,4))<<'}';
    if(ratePropsValid){const auto& p=rateProps;char raster[16];std::snprintf(raster,sizeof raster,"0x%x",unsigned(p.maxFragmentShadingRateRasterizationSamples));
        out<<",\"rateProperties\":{\"maxFragmentSize\":["<<p.maxFragmentSize.width<<','<<p.maxFragmentSize.height<<"],\"maxAspect\":"<<p.maxFragmentSizeAspectRatio
           <<",\"texelMin\":["<<p.minFragmentShadingRateAttachmentTexelSize.width<<','<<p.minFragmentShadingRateAttachmentTexelSize.height
           <<"],\"texelMax\":["<<p.maxFragmentShadingRateAttachmentTexelSize.width<<','<<p.maxFragmentShadingRateAttachmentTexelSize.height
           <<"],\"texelAspect\":"<<p.maxFragmentShadingRateAttachmentTexelSizeAspectRatio<<",\"coverageSamples\":"<<p.maxFragmentShadingRateCoverageSamples
           <<",\"rasterSamples\":\""<<raster<<"\",\"layered\":"<<b(p.layeredShadingRateAttachments)<<",\"nonTrivialCombiner\":"<<b(p.fragmentShadingRateNonTrivialCombinerOps)
           <<",\"depthStencilWrites\":"<<b(p.fragmentShadingRateWithShaderDepthStencilWrites)<<",\"sampleMask\":"<<b(p.fragmentShadingRateWithSampleMask)
           <<",\"shaderSampleMask\":"<<b(p.fragmentShadingRateWithShaderSampleMask)<<'}';}
    else out<<",\"rateProperties\":null";
    // levels: tile counts of the maps really in levelViews (level 0 = the full-rate map over the eye extent)
    const std::uint64_t eyeTiles=std::uint64_t((levelWidth+7)/8)*((levelHeight+7)/8);
    out<<",\"levels\":[";
    for(unsigned level=0;level<runtime::fragmentRateLevelCount;++level){
        const auto& l=runtime::fragmentRateLevels[level];std::array<std::array<std::uint64_t,3>,2> tiles{};
        for(unsigned eye=0;eye<2;++eye){
            if(!level){tiles[eye]={eyeTiles,0,0};continue;}
            const VkImageView v=levelViews[eye][level];
            if(v)for(const auto& m:maps)if(m->view==v){tiles[eye]=m->tiles;break;}
        }
        out<<(level?",":"")<<"{\"level\":"<<level<<",\"name\":\""<<l.name<<"\",\"inner\":"<<l.inner<<",\"outer\":"<<l.outer<<",\"vScale\":"<<l.verticalScale
           <<",\"tiles\":[["<<tiles[0][0]<<','<<tiles[0][1]<<','<<tiles[0][2]<<"],["<<tiles[1][0]<<','<<tiles[1][1]<<','<<tiles[1][2]<<"]]"
           <<",\"fragmentsPermille\":["<<runtime::fragmentRatePermille(tiles[0])<<','<<runtime::fragmentRatePermille(tiles[1])<<"]}";
    }
    out<<"]}";return out.str();
}
runtime::FragmentRateCapabilities gameFragmentRateCapabilities() noexcept {
    runtime::FragmentRateCapabilities c{};
    try{std::lock_guard guard(mutex);
        c.ready=ready&&device&&bridge.configuredFor(device);c.device=device;c.status=status;
        c.pipeline=c.attachment=featuresRequested&&ready; // FragmentRateDevicePlan enables both or the device is not converted
        if(ratePropsValid){c.nonTrivialCombiner=rateProps.fragmentShadingRateNonTrivialCombinerOps!=VK_FALSE;c.maxFragmentSize=rateProps.maxFragmentSize;}
        for(unsigned i=0;i<rateListCount;++i){const auto& r=rateList[i];if(!(r.sampleCounts&VK_SAMPLE_COUNT_1_BIT)||r.fragmentSize.width!=r.fragmentSize.height)continue;
            for(unsigned k=0;k<3;++k)if(r.fragmentSize.width==(1u<<k))c.squareMask|=1u<<k;}
    }catch(...){c={};c.status="CAPABILITY_EXCEPTION";}
    return c;
}
}
