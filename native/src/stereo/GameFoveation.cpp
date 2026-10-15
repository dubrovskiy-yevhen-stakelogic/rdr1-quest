#include "GameFoveation.h"
#include "EngineTargets.h"
#include "CameraAdapter.h"
#include "StereoReplay.h"
#include "StereoTestMode.h"           // STEREO TEST 17 FOVEATION SWEEP (read only)
#include "GpuLite.h"                  // FOVEATION AUTO input
#include "runtime/GraphicsSettings.h"
#include "runtime/FoveationStatus.h"
#include "runtime/AntiAliasingStatus.h"
#ifdef RDRVR_FRAGMENT_RATE
#include "GameFragmentRate.h"
#endif
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <vector>
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine {
namespace {
using namespace foveation;
static_assert(stereo::testModeFoveationSweep==17,"STEREO TEST 17 = FOVEATION SWEEP");
static_assert(runtime::GraphicsSettings::foveationLevels==menuAuto+1,"graphics.cfg foveation 0..5 (4 ULTRA, 5 AUTO)");
#ifdef RDRVR_FRAGMENT_RATE
static_assert(levelCount==runtime::fragmentRateLevelCount,"VRS level views and the registry levels must match");
static_assert(std::is_same_v<decltype(LevelViews{}.view),FragmentRateLevelViews>);
#endif
struct Errno {int value{errno};~Errno(){errno=value;}};
Registry registry;
// ---- device state (never touched by the begin path)
std::mutex stateMutex;
VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};std::uint32_t family{};std::mutex* queueMutex{};
std::atomic<VkDevice> densityDevice{};             // FDM device: pass/framebuffer hooks patch only this one
std::atomic<unsigned> activeApi{unsigned(Api::None)};
Api requestedApi{Api::Vrs};const char* requestSource="DEFAULT";
DensitySupport support{};bool queried{};unsigned texel{};
const char* status="NOT_REQUESTED";const char* planStatus="NOT_PREPARED";VkResult rejectedResult{VK_SUCCESS};
std::atomic<const char*> lastError{"NONE"}; // Written by the create hooks without stateMutex; diagnostic only
bool patchingDisabled{};                      // A patched vkCreateRenderPass failed before any pass was patched
// ---- density maps (FDM): immutable linear host-filled R8G8_UNORM images in FRAGMENT_DENSITY_MAP_OPTIMAL
struct DensityImage {VkExtent2D extent{};unsigned level{};int centerKey{};VkImage image{};VkDeviceMemory memory{};VkImageView view{};std::size_t bytes{};
    std::array<std::uint64_t,4> counts{};bool ready{};};
std::vector<std::unique_ptr<DensityImage>> images;constexpr std::size_t maxImages=64;
// The all-255 map of every framebuffer of a patched pass, sized ceil(maxFramebuffer/minTexel), made ready at
// device creation; pass patching (densityDevice) is armed only after it. No GPU round trip inside an engine create call.
DensityImage* universal{};std::atomic<VkImageView> universalView{};
struct Quarantine {VkCommandPool pool{};VkFence fence{};};std::vector<Quarantine> quarantined; // fence timeouts: never freed while possibly in use
// ---- counters
std::atomic<std::uint64_t> passesPatched{},passFailures{},framebuffersPatched{},framebufferFailures{},uploads{},uploadFailures{},uploadBatches{},uploadBytes{};
std::array<std::atomic<std::uint64_t>,unsigned(PassSkip::Count)> passesSkipped{};
std::atomic<std::uint64_t> shapeColorDepth{},shapeColorOnly{},shapeDepthOnly{},shapeOther{},shapeLoads{};
// ---- eyes / level (render thread)
VkExtent2D eyeExtent{};std::array<VkImage,Registry::maxEye1Images> eye1{};unsigned eye1Count{};int eyeCenterKey{-1};float eyeCenterY{.5f};bool eyesReady{};
std::atomic<int> menuLevel{},propertyLevel{-1};std::atomic<unsigned> appliedLevel{};std::atomic<bool> loadAllowed{};
std::uint64_t frameTick{};std::atomic<std::uint64_t> levelChanges{},eyeUpdates{},viewFailures{},frames{};
LevelViews currentViews{};
// ---- level selection: decided at the end of gameFoveationFrame (pending), applied at the start of the next
// world frame, so the frame-end byState signature (GameXr stereoGpuState) names exactly the level the next frame records.
enum class LevelSource : unsigned {Menu,Property,Auto,Sweep};
const char* levelSourceName(unsigned s) noexcept {return s==1?"PROPERTY":s==2?"AUTO":s==3?"SWEEP":"MENU";}
unsigned pending{},pendingSource{};                        // render thread only
std::atomic<unsigned> appliedSource{},menuFrameRate{};std::atomic<int> autoFloor{1},nextLevel{-1};std::atomic<bool> autoActive{};
std::array<std::atomic<std::uint64_t>,levelCount> levelFrames{};std::atomic<std::uint64_t> sweepFrames{};
std::mutex dynamicMutex;                                    // controller/params/lastSerial: render thread + report
AutoController controller;AutoParams autoParams;std::uint64_t lastSerial{},autoEntries{};
std::uint64_t steadyNs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
// GpuLite adapter (the only place that touches group B1's span): serial +1 per new engine-frame CmdBegin->CmdEnd span
// (0 = none yet), us = the newest span. p90 only for the report (a percentile sorts the window).
struct Lite {std::uint64_t serial;std::uint32_t us,p90;};
Lite liteReading() noexcept {return {stereo::gpuLiteSerial(),stereo::gpuLiteFrameUs(),0};}

int propertyDigit(const char* name,int fallback) noexcept {
#ifdef __ANDROID__
    char v[PROP_VALUE_MAX]{};
    if(__system_property_get(name,v)>0&&v[0]>='0'&&v[0]<='9'&&!v[1])return v[0]-'0';
#else
    (void)name;
#endif
    return fallback;
}
bool propertyText(const char* name,char (&out)[92]) noexcept {
    out[0]=0;
#ifdef __ANDROID__
    return __system_property_get(name,out)>0;
#else
    (void)name;return false;
#endif
}
void logLine(const char* text) noexcept {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO,"RdrVrFoveation","%s",text);
#else
    (void)text;
#endif
}
void registeredThunk(VkDevice d,const VkFramebufferCreateInfo* c,VkFramebuffer fb,VkResult r){stereo::framebufferCreated(d,c,fb,r);}
void unregisteredThunk(VkDevice d,VkFramebuffer fb){stereo::framebufferDestroyed(d,fb);}
VkImageView translateThunk(VkImageView left) noexcept {return registry.translate(left);}
// Re-publishing the translator bumps the stereo framebuffer generation: cached right framebuffers resolved
// with an old left->right pair are re-resolved.
void pairsChangedThunk(){stereo::setPassThroughTranslator(&translateThunk);}

// ---------------------------------------------------------------- level decision (render thread)
// FOVEATION AUTO: one AutoController step per world frame on the newest GPU span; advance=false only for the very first
// frame (no step, the entry level is applied directly).
unsigned autoDecide(bool advance) noexcept {
    const auto now=steadyNs();
    std::lock_guard lock(dynamicMutex);
    autoParams.ceiling=levelCount-1;autoParams.floor=std::min(unsigned(std::max(0,autoFloor.load(std::memory_order_relaxed))),autoParams.ceiling);
    autoParams.budgetUs=autoBudgetUs(menuFrameRate.load(std::memory_order_relaxed),runtime::spaceWarpMenuForced.load(std::memory_order_relaxed)!=0,
                                     runtime::antiAliasingEffective.load(std::memory_order_relaxed)==1);
    const unsigned current=appliedLevel.load(std::memory_order_relaxed);
    const unsigned entry=std::clamp(current,autoParams.floor,autoParams.ceiling);
    if(!autoActive.load(std::memory_order_relaxed)){controller.reset(entry,now);autoActive.store(true,std::memory_order_relaxed);++autoEntries;}
    if(!advance)return entry;
    const Lite lite=liteReading();
    const bool fresh=lite.serial!=0&&lite.serial!=lastSerial;lastSerial=lite.serial;
    return controller.step(current,fresh,lite.us,now,autoParams);
}
unsigned decideLevel(bool advance) noexcept {
    if(stereo::testMode.load(std::memory_order_relaxed)==stereo::testModeFoveationSweep){
        pendingSource=unsigned(LevelSource::Sweep);autoActive.store(false,std::memory_order_relaxed);return SweepPlan::level(frameTick);}
    const int property=propertyLevel.load(std::memory_order_relaxed);
    if(property>=0){pendingSource=unsigned(LevelSource::Property);autoActive.store(false,std::memory_order_relaxed);return unsigned(property)<levelCount?unsigned(property):0;}
    const unsigned menu=unsigned(std::max(0,menuLevel.load(std::memory_order_relaxed)));
    if(menu==menuAuto){pendingSource=unsigned(LevelSource::Auto);return autoDecide(advance);}
    pendingSource=unsigned(LevelSource::Menu);autoActive.store(false,std::memory_order_relaxed);
    return std::min(menu,levelCount-1);
}

// ---------------------------------------------------------------- density images (stateMutex held)
void releaseImage(DensityImage& m) noexcept {
    if(m.view)vkDestroyImageView(device,m.view,nullptr);
    if(m.image)vkDestroyImage(device,m.image,nullptr);
    if(m.memory)vkFreeMemory(device,m.memory,nullptr);
    m.view={};m.image={};m.memory={};m.ready=false;
}
bool createHostImage(DensityImage& m,const DensityMap& pixels) noexcept {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=densityFormat;
    ci.extent={pixels.width,pixels.height,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_LINEAR;
    ci.usage=VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.initialLayout=VK_IMAGE_LAYOUT_PREINITIALIZED;
    if(vkCreateImage(device,&ci,nullptr,&m.image)!=VK_SUCCESS||!m.image){lastError="CREATE_IMAGE";m.image={};return false;}
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,m.image,&req);
    VkPhysicalDeviceMemoryProperties mem{};vkGetPhysicalDeviceMemoryProperties(physical,&mem);
    constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    unsigned type=mem.memoryTypeCount;
    for(unsigned i=0;i<mem.memoryTypeCount&&i<VK_MAX_MEMORY_TYPES;++i)if((req.memoryTypeBits&(1u<<i))&&(mem.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
    if(!req.size||req.size>(64u<<20)||type==mem.memoryTypeCount){lastError="MEMORY_TYPE";return false;}
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
    if(vkAllocateMemory(device,&ai,nullptr,&m.memory)!=VK_SUCCESS||!m.memory){lastError="ALLOCATE_MEMORY";m.memory={};return false;}
    if(vkBindImageMemory(device,m.image,m.memory,0)!=VK_SUCCESS){lastError="BIND_MEMORY";return false;}
    VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT,0,0};VkSubresourceLayout layout{};vkGetImageSubresourceLayout(device,m.image,&sub,&layout);
    const std::uint64_t rowBytes=std::uint64_t(pixels.width)*2;
    if(layout.rowPitch<rowBytes||layout.offset+layout.rowPitch*(pixels.height-1)+rowBytes>req.size){lastError="SUBRESOURCE_LAYOUT";return false;}
    void* raw{};if(vkMapMemory(device,m.memory,0,req.size,0,&raw)!=VK_SUCCESS||!raw){lastError="MAP_MEMORY";return false;}
    for(unsigned y=0;y<pixels.height;++y)std::memcpy(static_cast<std::uint8_t*>(raw)+layout.offset+layout.rowPitch*y,pixels.rg.data()+rowBytes*y,std::size_t(rowBytes));
    vkUnmapMemory(device,m.memory);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=m.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=densityFormat;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(vkCreateImageView(device,&vi,nullptr,&m.view)!=VK_SUCCESS||!m.view){lastError="CREATE_VIEW";m.view={};return false;}
    m.extent={pixels.width,pixels.height};m.bytes=std::size_t(req.size);m.counts=pixels.counts;return true;
}
// One submission on the private XR queue: PREINITIALIZED -> FRAGMENT_DENSITY_MAP_OPTIMAL for every new image, fence
// completion before any framebuffer can reference them (same scheme as the VRS rate maps).
enum class Upload {Ok,Failed,TimedOut};
Upload transition(const std::vector<DensityImage*>& batch) noexcept {
    if(batch.empty())return Upload::Ok;
    if(!queue||!queueMutex){lastError="NO_PRIVATE_QUEUE";return Upload::Failed;}
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;pi.queueFamilyIndex=family;
    Quarantine q{};if(vkCreateCommandPool(device,&pi,nullptr,&q.pool)!=VK_SUCCESS){lastError="COMMAND_POOL";return Upload::Failed;}
    auto drop=[&](const char* why){lastError=why;if(q.fence)vkDestroyFence(device,q.fence,nullptr);vkDestroyCommandPool(device,q.pool,nullptr);return Upload::Failed;};
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=q.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
    VkCommandBuffer cmd{};if(vkAllocateCommandBuffers(device,&ai,&cmd)!=VK_SUCCESS||!cmd)return drop("COMMAND_BUFFER");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkBeginCommandBuffer(cmd,&bi)!=VK_SUCCESS)return drop("COMMAND_BEGIN");
    std::vector<VkImageMemoryBarrier> barriers;
    try{for(auto* m:batch){VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;
        b.dstAccessMask=VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;b.oldLayout=VK_IMAGE_LAYOUT_PREINITIALIZED;b.newLayout=VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=m->image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barriers.push_back(b);}}
    catch(...){return drop("BARRIER_ALLOCATION");}
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT,0,0,nullptr,0,nullptr,std::uint32_t(barriers.size()),barriers.data());
    if(vkEndCommandBuffer(cmd)!=VK_SUCCESS)return drop("COMMAND_END");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if(vkCreateFence(device,&fi,nullptr,&q.fence)!=VK_SUCCESS){q.fence={};return drop("FENCE");}
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;
    VkResult waited=VK_ERROR_UNKNOWN;
    {std::lock_guard guard(*queueMutex);
        if(vkQueueSubmit(queue,1,&si,q.fence)!=VK_SUCCESS)return drop("SUBMIT");
        waited=vkWaitForFences(device,1,&q.fence,VK_TRUE,2000000000ULL);}
    ++uploadBatches;
    if(waited!=VK_SUCCESS){lastError="FENCE_WAIT";try{quarantined.push_back(q);}catch(...){}return Upload::TimedOut;}
    vkDestroyFence(device,q.fence,nullptr);vkDestroyCommandPool(device,q.pool,nullptr);
    for(auto* m:batch){m->ready=true;uploadBytes+=m->bytes;}
    uploads+=batch.size();return Upload::Ok;
}
// Host fill + fenced transition of one map; on failure the image is released, or leaked when the GPU may still touch it.
DensityImage* upload(unsigned level,int centerKey,const DensityMap& pixels) noexcept {
    if(images.size()>=maxImages){lastError="MAP_CACHE_FULL";++uploadFailures;return nullptr;}
    std::unique_ptr<DensityImage> next;
    try{next=std::make_unique<DensityImage>();}catch(...){return nullptr;}
    next->level=level;next->centerKey=centerKey;
    const bool created=createHostImage(*next,pixels);
    const auto result=created?transition({next.get()}):Upload::Failed;
    if(result!=Upload::Ok){
        ++uploadFailures;
        if(result==Upload::TimedOut)(void)next.release(); // a timed-out transition may still reference it: leak it
        else releaseImage(*next);
        return nullptr;}
    try{images.push_back(std::move(next));}catch(...){return nullptr;}
    return images.back().get();
}
// Band map of a level (1..3) for an eye extent / quantized optical centre. Frame hook only (render thread).
DensityImage* levelImage(unsigned level,VkExtent2D eye,int centerKey) noexcept {
    if(!texel||!level||level>=levelCount||!eye.width||!eye.height)return nullptr;
    const auto extent=densityMapExtent(eye.width,eye.height,texel);
    for(auto& m:images)if(m->ready&&m->level==level&&m->extent.width==extent.width&&m->extent.height==extent.height&&m->centerKey==centerKey)return m.get();
    DensityMap pixels;
    if(!buildDensityMap(level,eye.width,eye.height,texel,centerKey/64.f,pixels)){lastError="MAP_BUILD";++uploadFailures;return nullptr;}
    return upload(level,centerKey,pixels);
}
}

// ================================================================= public
foveation::Registry& foveationRegistry() noexcept {return registry;}
int gameFoveationNextLevel() noexcept {
    return Api(activeApi.load(std::memory_order_acquire))==Api::None?-1:nextLevel.load(std::memory_order_relaxed);
}
bool gameFoveationAutoActive() noexcept {
    return Api(activeApi.load(std::memory_order_acquire))!=Api::None&&autoActive.load(std::memory_order_relaxed);
}
bool foveationBegin(const VkRenderPassBeginInfo& in,VkRenderPassBeginInfo& out,int* level) noexcept {Errno preserve;return registry.begin(in,out,level);}
VkImageView foveationRightView(VkImageView left) noexcept {return registry.translate(left);}

foveation::Api gameFoveationApiRequest() noexcept {
    std::lock_guard lock(stateMutex);
    const auto snapshot=runtime::graphicsSettings.snapshot();
    runtime::foveationStartupApi.store(int(snapshot.requested.foveationApi),std::memory_order_relaxed);
    char value[92]{};
    if(propertyText("debug.rdrvr.foveation.api",value)&&value[0]){
        const std::string_view v(value);bool known=true;
        if(v=="vrs"||v=="VRS"||v=="0")requestedApi=Api::Vrs;
        else if(v=="fdm"||v=="FDM"||v=="1")requestedApi=Api::Fdm;
        else if(v=="none"||v=="NONE"||v=="off"||v=="2")requestedApi=Api::None;
        else known=false;
        if(known){requestSource="PROPERTY";runtime::foveationApiProperty.store(true,std::memory_order_relaxed);return requestedApi;}
    }
    const unsigned api=snapshot.requested.foveationApi;
    requestedApi=api==1?Api::Fdm:api==2?Api::None:Api::Vrs;requestSource=snapshot.loaded?"GRAPHICS_CFG":"DEFAULT";
    return requestedApi;
}
void gameFoveationQuery(VkPhysicalDevice gpu) noexcept {
    std::lock_guard lock(stateMutex);
    if(!gpu)return;
    try{
        DensitySupport s{};
        unsigned count{};if(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&count,nullptr)!=VK_SUCCESS||!count||count>1024){planStatus="EXTENSION_QUERY_FAILED";return;}
        std::vector<VkExtensionProperties> ext(count);if(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&count,ext.data())!=VK_SUCCESS){planStatus="EXTENSION_QUERY_FAILED";return;}
        for(unsigned i=0;i<count&&i<ext.size();++i){const char* n=ext[i].extensionName;
            s.extension|=!std::strcmp(n,VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);s.fdm2|=!std::strcmp(n,VK_EXT_FRAGMENT_DENSITY_MAP_2_EXTENSION_NAME);
            s.offsetQcom|=!std::strcmp(n,"VK_QCOM_fragment_density_map_offset");s.khrRate|=!std::strcmp(n,VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);}
        if(s.extension){
            VkPhysicalDeviceFragmentDensityMap2FeaturesEXT f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_2_FEATURES_EXT};
            VkPhysicalDeviceFragmentDensityMapFeaturesEXT f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};if(s.fdm2)f.pNext=&f2;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&f;vkGetPhysicalDeviceFeatures2(gpu,&features);
            s.fragmentDensityMap=f.fragmentDensityMap;s.dynamic=f.fragmentDensityMapDynamic;s.nonSubsampled=f.fragmentDensityMapNonSubsampledImages;s.deferred=s.fdm2?f2.fragmentDensityMapDeferred:VK_FALSE;
            VkPhysicalDeviceFragmentDensityMapPropertiesEXT p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&p;vkGetPhysicalDeviceProperties2(gpu,&properties);
            s.texelMin=p.minFragmentDensityTexelSize;s.texelMax=p.maxFragmentDensityTexelSize;s.invocations=p.fragmentDensityInvocations;
            VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(gpu,densityFormat,&fp);s.linearFeatures=fp.linearTilingFeatures;s.optimalFeatures=fp.optimalTilingFeatures;
            VkImageFormatProperties ip{};
            s.linearImage=vkGetPhysicalDeviceImageFormatProperties(gpu,densityFormat,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_LINEAR,VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT,0,&ip)==VK_SUCCESS&&
                ip.maxExtent.width>=64&&ip.maxExtent.height>=64&&ip.maxArrayLayers>=1&&(ip.sampleCounts&VK_SAMPLE_COUNT_1_BIT);
            s.linearMaxExtent=ip.maxExtent;
        }
        support=s;texel=densityTexel(s.texelMin,s.texelMax);queried=true;
        if(physical==VK_NULL_HANDLE)physical=gpu;
    }catch(...){planStatus="QUERY_EXCEPTION";}
}
bool gameFoveationDensityPlan(const VkDeviceCreateInfo& native,DensityDevicePlan& plan) noexcept {
    std::lock_guard lock(stateMutex);
    if(!queried){planStatus="NOT_QUERIED";return false;}
    if(!texel){planStatus="TEXEL_SIZE";return false;}
    const bool ok=plan.prepare(native,support);planStatus=plan.status();return ok;
}
void gameFoveationDensityRejected(VkResult result) noexcept {
    std::lock_guard lock(stateMutex);status="FDM_DEVICE_REJECTED";rejectedResult=result;
    logLine("FDM device request rejected; device created with the original request");
}
void gameFoveationDeviceCreated(Api active,VkPhysicalDevice gpu,VkDevice d,std::uint32_t fam,std::uint32_t queueIndex,std::mutex* qMutex) noexcept {
    std::lock_guard lock(stateMutex);
    runtime::foveationActiveApi.store(int(active),std::memory_order_relaxed);runtime::foveationApiNote.store(nullptr,std::memory_order_relaxed);
    activeApi.store(unsigned(active),std::memory_order_release);
    if(active==Api::None||!d){
        if(std::string_view(status)=="NOT_REQUESTED")status=active==Api::None?"NO_API":"NO_DEVICE";
        if(requestedApi!=Api::None)runtime::foveationApiNote.store(requestedApi==Api::Fdm&&rejectedResult!=VK_SUCCESS?"FDM REJECTED":"N/A",std::memory_order_relaxed);
        return;}
    RegistryRaw raw{vkCreateFramebuffer,vkDestroyFramebuffer,&registeredThunk,&unregisteredThunk,&pairsChangedThunk};
    if(!registry.configure(d,active,raw)){status="REGISTRY_CONFIGURE_FAILED";activeApi.store(unsigned(Api::None));runtime::foveationActiveApi.store(int(Api::None));return;}
    physical=gpu;device=d;family=fam;queueMutex=qMutex;queue={};vkGetDeviceQueue(d,fam,queueIndex,&queue);
    stereo::setPassThroughTranslator(&translateThunk);
    if(active==Api::Fdm){
        // Without the universal map no framebuffer of a patched pass could be created: then no pass is patched (the
        // feature stays enabled on the device, which changes nothing by itself).
        if(queue&&texel){
            VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(gpu,&props);
            const unsigned tw=std::max(1u,support.texelMin.width),th=std::max(1u,support.texelMin.height);
            unsigned w=(std::max(1u,props.limits.maxFramebufferWidth)+tw-1)/tw,h=(std::max(1u,props.limits.maxFramebufferHeight)+th-1)/th;
            if(support.linearMaxExtent.width)w=std::min(w,support.linearMaxExtent.width);
            if(support.linearMaxExtent.height)h=std::min(h,support.linearMaxExtent.height);
            w=std::min(w,4096u);h=std::min(h,4096u);
            DensityMap pixels;
            if(buildFullDensityMap(w,h,pixels))universal=upload(0,0,pixels);
            if(universal)universalView.store(universal->view,std::memory_order_release);
        }
        if(universal)densityDevice.store(d,std::memory_order_release);
        status=!queue||!texel?"FDM_NO_QUEUE":universal?"FDM_READY":"FDM_UNIVERSAL_MAP_FAILED";
        if(!universal)runtime::foveationApiNote.store("NO MAP",std::memory_order_relaxed);
        char line[160];std::snprintf(line,sizeof line,"FDM device %s texel=%u (min %ux%u max %ux%u) nonSubsampled=%u dynamic=%u",status,
            texel,support.texelMin.width,support.texelMin.height,support.texelMax.width,support.texelMax.height,unsigned(support.nonSubsampled),unsigned(support.dynamic));logLine(line);
    }else{
        status="VRS_READY";
        if(requestedApi==Api::Fdm)runtime::foveationApiNote.store(std::string_view(planStatus)=="UNSUPPORTED"?"FDM N/A":"FDM DECLINED",std::memory_order_relaxed);
    }
}
VkResult gameFoveationCreatePass(PFN_vkCreateRenderPass original,VkDevice d,const VkRenderPassCreateInfo* c,const VkAllocationCallbacks* a,VkRenderPass* out) noexcept {
    if(!c||!out||!d||densityDevice.load(std::memory_order_acquire)!=d)return original(d,c,a,out);
    const int incoming=errno;
    DensityPassPlan plan;
    if(!plan.prepare(*c)){
        const auto skip=unsigned(plan.skip());if(skip<passesSkipped.size())++passesSkipped[skip];
        errno=incoming;const auto r=original(d,c,a,out);const int saved=errno;
        if(r==VK_SUCCESS&&*out)registry.passCreated(d,*out,0,passShape(*c));
        errno=saved;return r;
    }
    errno=incoming;auto r=original(d,plan.info(),a,out);int saved=errno;
    if(r==VK_SUCCESS&&*out){registry.passCreated(d,*out,c->attachmentCount,plan.shape());++passesPatched;errno=saved;return r;}
    // The driver refused the patched pass. An unpatched member would break its compatibility class (pipelines and
    // framebuffers of patched siblings). Before any pass was patched nothing can depend on patching: stop patching on
    // this device and create the original pass. Otherwise fail the call like the VRS bridge does.
    ++passFailures;lastError="PASS_CREATE";
    bool disable=false;
    {std::lock_guard lock(stateMutex);if(!passesPatched.load()&&!patchingDisabled){patchingDisabled=disable=true;status="FDM_PATCHING_DISABLED";
        runtime::foveationApiNote.store("PASS FAILED",std::memory_order_relaxed);}}
    if(disable){
        densityDevice.store(VK_NULL_HANDLE,std::memory_order_release);
        errno=incoming;r=original(d,c,a,out);saved=errno;
        if(r==VK_SUCCESS&&*out)registry.passCreated(d,*out,0,passShape(*c));
        errno=saved;return r;}
    *out=VK_NULL_HANDLE;errno=saved;return r==VK_SUCCESS?VK_ERROR_INITIALIZATION_FAILED:r;
}
void gameFoveationPassObserved(VkDevice d,const VkRenderPassCreateInfo* c,VkRenderPass pass,VkResult result) noexcept {
    if(result!=VK_SUCCESS||!pass||!c||activeApi.load(std::memory_order_relaxed)==unsigned(Api::None))return;
    Errno preserve;
    const auto shape=passShape(*c);
    if(shape.colors&&shape.depth)++shapeColorDepth;else if(shape.colors)++shapeColorOnly;else if(shape.depth)++shapeDepthOnly;else ++shapeOther;
    if(shape.loads)++shapeLoads;
#ifdef RDRVR_FRAGMENT_RATE
    if(activeApi.load(std::memory_order_relaxed)==unsigned(Api::Vrs)){
        runtime::FragmentRateRouting::Pass converted{};
        registry.passCreated(d,pass,gameFragmentRateBridge().routing().pass(d,pass,converted)?converted.attachments:0,shape);
    }
#endif
}
void gameFoveationPassDestroyed(VkDevice d,VkRenderPass pass) noexcept {Errno preserve;registry.passDestroyed(d,pass);}
VkResult gameFoveationCreateFramebuffer(PFN_vkCreateFramebuffer original,VkDevice d,const VkFramebufferCreateInfo* c,const VkAllocationCallbacks* a,VkFramebuffer* out) noexcept {
    if(!c||!out||!d||densityDevice.load(std::memory_order_acquire)!=d)return original(d,c,a,out);
    const int incoming=errno;
    unsigned native=0;
    if(!registry.foveatedPass(d,c->renderPass,native)){errno=incoming;return original(d,c,a,out);}
    // Always the universal all-255 map (ready before densityDevice was armed): no lock, no upload here.
    const VkImageView map=universalView.load(std::memory_order_acquire);
    AppendedFramebufferPlan plan;
    if(!map||!plan.prepare(*c,native,map)){
        // Only an imageless / pNext framebuffer of a patched pass gets here (the engine creates none).
        // A patched pass can never take the original attachment list
        // (VUID-VkFramebufferCreateInfo-attachmentCount-00876).
        ++framebufferFailures;lastError=map?"FRAMEBUFFER_SHAPE":"UNIVERSAL_MAP_UNAVAILABLE";*out=VK_NULL_HANDLE;errno=incoming;return VK_ERROR_INITIALIZATION_FAILED;
    }
    errno=incoming;const auto r=original(d,plan.info(),a,out);const int saved=errno;
    if(r==VK_SUCCESS&&*out)++framebuffersPatched;else ++framebufferFailures;
    errno=saved;return r;
}
void gameFoveationFramebufferCreated(VkDevice d,const VkFramebufferCreateInfo* c,VkFramebuffer fb,VkResult result) noexcept {
    if(result!=VK_SUCCESS||!fb||activeApi.load(std::memory_order_relaxed)==unsigned(Api::None))return;
    Errno preserve;registry.framebufferCreated(d,c,fb);
}
void gameFoveationFramebufferDestroyed(VkDevice d,VkFramebuffer fb) noexcept {
    if(activeApi.load(std::memory_order_relaxed)==unsigned(Api::None))return;
    Errno preserve;registry.framebufferDestroyed(d,fb);
}
void gameFoveationViewCreated(VkDevice d,const VkImageViewCreateInfo* c,VkImageView view,VkResult result) noexcept {
    if(result!=VK_SUCCESS||!c||!view||activeApi.load(std::memory_order_relaxed)==unsigned(Api::None))return;
    Errno preserve;registry.viewCreated(d,view,c->image);
}
void gameFoveationViewDestroyed(VkDevice d,VkImageView view) noexcept {
    if(activeApi.load(std::memory_order_relaxed)==unsigned(Api::None))return;
    Errno preserve;registry.viewDestroyed(d,view);
}
void gameFoveationFrame(const EngineEyeTargets& targets,const StereoCameraPlan& plan,std::mutex& qMutex,std::uint64_t retiredFrame) noexcept {
    const auto api=Api(activeApi.load(std::memory_order_acquire));
    if(api==Api::None)return;
    (void)qMutex; // the private queue mutex was bound at device creation (gameFoveationDeviceCreated)
    Errno preserve;++frames;
    registry.setFrame(plan.sceneId,retiredFrame); // Variant LRU stamps + eviction watermark
    try{
        // ---- level: STEREO TEST 17 SWEEP > debug.rdrvr.foveation=<0..4> > GRAPHICS menu (0..4 fixed, 5 AUTO).
        // Menu every 8 world frames, properties every 32. The level decided at the end of the previous frame applies now.
        if((frameTick&7)==0){
            const auto requested=runtime::graphicsSettings.snapshot().requested;
            menuLevel.store(int(std::min(requested.foveation,runtime::GraphicsSettings::foveationLevels-1)),std::memory_order_relaxed);
            menuFrameRate.store(requested.frameRate,std::memory_order_relaxed);
            if((frameTick&31)==0){
                const int p=propertyDigit("debug.rdrvr.foveation",-1);propertyLevel.store(p>=0&&p<int(levelCount)?p:-1,std::memory_order_relaxed);
                runtime::foveationLevelProperty.store(propertyLevel.load(std::memory_order_relaxed),std::memory_order_relaxed);
                const int f=propertyDigit("debug.rdrvr.foveation.autofloor",1);autoFloor.store(f>=0&&f<int(levelCount)?f:1,std::memory_order_relaxed);
                loadAllowed.store(propertyDigit("debug.rdrvr.foveation.fdmload",0)==1,std::memory_order_relaxed);
                registry.setAllowLoad(loadAllowed.load(std::memory_order_relaxed));
            }
        }
        if(frameTick==0)pending=decideLevel(false);
        const unsigned applied=pending<levelCount?pending:0;
        if(applied!=appliedLevel.load(std::memory_order_relaxed)||frameTick==0){appliedLevel.store(applied,std::memory_order_relaxed);registry.setLevel(applied);++levelChanges;}
        appliedSource.store(pendingSource,std::memory_order_relaxed);
        ++levelFrames[applied];if(pendingSource==unsigned(LevelSource::Sweep))++sweepFrames;
        runtime::foveationLiveLevel.store(int(applied),std::memory_order_relaxed);
        ++frameTick;
        pending=decideLevel(true);nextLevel.store(int(pending),std::memory_order_relaxed);
        // ---- eyes
        if(targets.eyeCount()!=2)return;
        const auto* color=targets.image(0);if(!color||!color->image.image)return;
        const VkExtent2D extent{color->image.extent.width,color->image.extent.height};
        std::array<VkImage,Registry::maxEye1Images> right{};unsigned rightCount=0;
        for(auto role:{EngineTargetRole::Color,EngineTargetRole::Depth,EngineTargetRole::ShadowCollector,EngineTargetRole::LightAccumulation,
                       EngineTargetRole::SampledDepth,EngineTargetRole::WaterDepth,EngineTargetRole::Refraction}){
            const auto* image=targets.image(1,role);if(image&&image->image.image&&rightCount<right.size())right[rightCount++]=image->image.image;}
        const float cy=opticalCenterY(plan.tracking.fov[0].up,plan.tracking.fov[0].down);
        const int centerKey=int(std::lround(cy*64));
        LevelViews views{};
        std::lock_guard lock(stateMutex);
        if(api==Api::Fdm){
            if(eyesReady&&extent.width==eyeExtent.width&&extent.height==eyeExtent.height&&centerKey==eyeCenterKey&&rightCount==eye1Count&&
               std::equal(right.begin(),right.begin()+rightCount,eye1.begin()))return; // unchanged: nothing to do
            const VkImageView full=universalView.load(std::memory_order_acquire);if(!full){++viewFailures;return;}
            views.view[0][0]=views.view[1][0]=full; // level 0 = the map every patched framebuffer was created with
            for(unsigned level=1;level<levelUltra;++level){
                auto* m=levelImage(level,extent,centerKey);if(!m){++viewFailures;return;}
                views.view[0][level]=views.view[1][level]=m->view;}
            for(auto& eye:views.view)eye[levelUltra]=eye[levelUltra-1]; // FDM ULTRA = the HIGH bands
        }
#ifdef RDRVR_FRAGMENT_RATE
        else if(api==Api::Vrs){
            if(!gameFragmentRateLevelViews(extent.width,extent.height,views.view)){++viewFailures;return;}
        }
#endif
        else return;
        registry.setEyes(extent,right.data(),rightCount,views);
        if(!(views==currentViews)||!eyesReady){currentViews=views;++eyeUpdates;}
        eyeExtent=extent;eye1=right;eye1Count=rightCount;eyeCenterKey=centerKey;eyeCenterY=cy;eyesReady=true;
        foveationArmed.store(true,std::memory_order_release);
    }catch(...){++viewFailures;}
}
void gameFoveationDestroy(VkDevice d) noexcept {
    Errno preserve;
    std::lock_guard lock(stateMutex);
    if(!device||d!=device)return;
    foveationArmed.store(false,std::memory_order_release);densityDevice.store(VK_NULL_HANDLE,std::memory_order_release);
    nextLevel.store(-1,std::memory_order_relaxed);runtime::foveationLiveLevel.store(-1,std::memory_order_relaxed);
    stereo::setPassThroughTranslator(nullptr);
    const auto idle=vkDeviceWaitIdle(d);
    registry.clear(d);
    if(idle==VK_SUCCESS||idle==VK_ERROR_DEVICE_LOST){
        for(auto& m:images)releaseImage(*m);
        for(auto& q:quarantined){if(q.fence)vkDestroyFence(d,q.fence,nullptr);if(q.pool)vkDestroyCommandPool(d,q.pool,nullptr);}
        status="DEVICE_DESTROYED";
    }else status="SHUTDOWN_QUARANTINED";
    images.clear();quarantined.clear();universal=nullptr;universalView.store(VK_NULL_HANDLE,std::memory_order_release);patchingDisabled=false;
    activeApi.store(unsigned(Api::None),std::memory_order_release);runtime::foveationActiveApi.store(-1,std::memory_order_relaxed);
    device={};queue={};queueMutex={};eyesReady=false;eyeExtent={};eye1Count=0;eyeCenterKey=-1;currentViews={};
}
std::string gameFoveationReport() {
    Errno preserve;
    std::ostringstream o;
    const auto api=Api(activeApi.load(std::memory_order_relaxed));
    const auto& c=registry.counters;const auto sizes=registry.sizes();
    const unsigned level=appliedLevel.load(std::memory_order_relaxed);
    // AUTO controller snapshot first (dynamicMutex is never held together with stateMutex)
    AutoController::Stats dyn{};AutoParams dynParams{};std::uint64_t dynSerial{},dynEntries{};
    {std::lock_guard dynamic(dynamicMutex);dyn=controller.stats();dynParams=autoParams;dynSerial=lastSerial;dynEntries=autoEntries;}
    const bool dynActive=autoActive.load(std::memory_order_relaxed);
    const std::uint32_t liteP90=stereo::gpuLiteP90Us();const std::uint64_t nowNs=steadyNs();
    const unsigned mapLevel=std::min(level,levelUltra-1); // FDM: ULTRA uses the HIGH band map
    std::lock_guard lock(stateMutex);
    std::size_t fullMaps=0,levelMaps=0,bytes=0;VkExtent2D mapExtent{};std::array<std::uint64_t,4> counts{};
    for(const auto& m:images){if(!m->ready)continue;bytes+=m->bytes;if(m->level)++levelMaps;else ++fullMaps;
        if(m->level==mapLevel&&eyesReady&&m->extent.width==densityMapExtent(eyeExtent.width,eyeExtent.height,texel).width){mapExtent=m->extent;if(m->level)counts=m->counts;}}
    if(!mapExtent.width&&eyesReady)mapExtent=densityMapExtent(eyeExtent.width,eyeExtent.height,api==Api::Fdm?texel:8u);
    char hex[2][16];std::snprintf(hex[0],sizeof hex[0],"0x%08x",unsigned(support.linearFeatures));std::snprintf(hex[1],sizeof hex[1],"0x%08x",unsigned(support.optimalFeatures));
    o<<"{\"requestedApi\":\""<<apiName(requestedApi)<<"\",\"requestSource\":\""<<requestSource<<"\",\"activeApi\":\""<<apiName(api)<<"\",\"status\":\""<<status
     <<"\",\"planStatus\":\""<<planStatus<<"\",\"rejectedResult\":"<<int(rejectedResult)<<",\"lastError\":\""<<lastError.load()<<"\""<<",\"patchingDisabled\":"<<(patchingDisabled?"true":"false")
     <<",\"enabled\":"<<(api!=Api::None&&eyesReady&&level?"true":"false")<<",\"armed\":"<<(foveationArmed.load()?"true":"false")
     <<",\"level\":"<<level<<",\"levelName\":\""<<levelName(level)<<"\",\"levelSource\":\""<<levelSourceName(appliedSource.load())<<"\",\"menuLevel\":"<<menuLevel.load()
     <<",\"menuValue\":"<<menuLevel.load()<<",\"nextLevel\":"<<nextLevel.load()<<",\"liveLevel\":"<<runtime::foveationLiveLevel.load(std::memory_order_relaxed)
     <<",\"levelFrames\":["<<levelFrames[0].load()<<","<<levelFrames[1].load()<<","<<levelFrames[2].load()<<","<<levelFrames[3].load()<<","<<levelFrames[4].load()<<"]"
     <<",\"propertyLevel\":"<<propertyLevel.load()<<",\"levelChanges\":"<<levelChanges.load()<<",\"fdmLoadAllowed\":"<<(loadAllowed.load()?"true":"false")
     <<",\"fdmUltraIsHigh\":"<<(api==Api::Fdm?"true":"false")
     <<",\"sweep\":{\"active\":"<<(stereo::testMode.load(std::memory_order_relaxed)==stereo::testModeFoveationSweep?"true":"false")<<",\"window\":"<<SweepPlan::window
     <<",\"frames\":"<<sweepFrames.load()<<",\"windows\":"<<sweepFrames.load()/SweepPlan::window<<"}"
     <<",\"dynamic\":{\"active\":"<<(dynActive?"true":"false")<<",\"status\":\""<<(dynActive&&!dynSerial?"NO_GPU_DATA":dyn.status)<<"\",\"entries\":"<<dynEntries
     <<",\"floor\":"<<dynParams.floor<<",\"ceiling\":"<<dynParams.ceiling<<",\"budgetUs\":"<<dynParams.budgetUs<<",\"downMarginUs\":"<<dynParams.downMarginUs
     <<",\"smoothedUs\":"<<dyn.smoothedUs<<",\"lastUs\":"<<dyn.lastUs<<",\"p90Us\":"<<liteP90<<",\"liteSerial\":"<<dynSerial
     <<",\"readings\":"<<dyn.readings<<",\"staleReadings\":"<<dyn.staleReadings<<",\"changes\":"<<dyn.changes<<",\"up\":"<<dyn.up<<",\"down\":"<<dyn.down
     <<",\"reversals\":"<<dyn.reversals<<",\"overAtCeiling\":"<<dyn.overAtCeiling<<",\"underAtFloor\":"<<dyn.underAtFloor
     <<",\"levelFrames\":["<<dyn.levelFrames[0]<<","<<dyn.levelFrames[1]<<","<<dyn.levelFrames[2]<<","<<dyn.levelFrames[3]<<","<<dyn.levelFrames[4]<<"]"
     <<",\"lastChangeAgoMs\":"<<(dyn.lastChangeNs&&nowNs>dyn.lastChangeNs?std::int64_t((nowNs-dyn.lastChangeNs)/1000000u):std::int64_t(-1))<<"}"
     <<",\"supported\":"<<(support.usable()?"true":"false")<<",\"texelSize\":"<<(api==Api::Vrs?8u:texel)
     <<",\"fdmDynamic\":"<<support.dynamic<<",\"nonSubsampled\":"<<support.nonSubsampled // Also device.dynamic ("dynamic" itself is the AUTO block)
     <<",\"device\":{\"queried\":"<<(queried?"true":"false")<<",\"fdmExtension\":"<<(support.extension?"true":"false")<<",\"fdm2\":"<<(support.fdm2?"true":"false")
     <<",\"offsetQcom\":"<<(support.offsetQcom?"true":"false")<<",\"khrFragmentShadingRate\":"<<(support.khrRate?"true":"false")
     <<",\"fragmentDensityMap\":"<<support.fragmentDensityMap<<",\"dynamic\":"<<support.dynamic<<",\"nonSubsampled\":"<<support.nonSubsampled<<",\"deferred\":"<<support.deferred
     <<",\"texelMin\":["<<support.texelMin.width<<","<<support.texelMin.height<<"],\"texelMax\":["<<support.texelMax.width<<","<<support.texelMax.height<<"]"
     <<",\"invocations\":"<<support.invocations<<",\"r8g8LinearFeatures\":\""<<hex[0]<<"\",\"r8g8OptimalFeatures\":\""<<hex[1]<<"\",\"linearDensityImage\":"<<(support.linearImage?"true":"false")<<"}"
     <<",\"eyeExtent\":["<<eyeExtent.width<<","<<eyeExtent.height<<"],\"mapExtent\":["<<mapExtent.width<<","<<mapExtent.height<<"],\"centerY\":"<<eyeCenterY
     <<",\"levelMapTexels\":{\"full\":"<<counts[0]<<",\"half\":"<<counts[1]<<",\"quarter\":"<<counts[2]<<",\"eighth\":"<<counts[3]<<"}"
     <<",\"uploads\":"<<uploads.load()<<",\"uploadBatches\":"<<uploadBatches.load()<<",\"uploadFailures\":"<<uploadFailures.load()<<",\"uploadBytes\":"<<uploadBytes.load()
     <<",\"maps\":{\"full\":"<<fullMaps<<",\"level\":"<<levelMaps<<",\"bytes\":"<<bytes<<",\"quarantined\":"<<quarantined.size()
     <<",\"universal\":["<<(universal?universal->extent.width:0u)<<","<<(universal?universal->extent.height:0u)<<"]}"
     <<",\"passesPatched\":"<<(api==Api::Fdm?passesPatched.load():c.passesFoveated.load())<<",\"passFailures\":"<<passFailures.load()<<",\"passesSkipped\":{";
    bool first=true;for(unsigned i=1;i<passesSkipped.size();++i){const auto n=passesSkipped[i].load();if(!n)continue;o<<(first?"":",")<<"\""<<passSkipName(PassSkip(i))<<"\":"<<n;first=false;}
    o<<"},\"passShapes\":{\"colorDepth\":"<<shapeColorDepth.load()<<",\"colorOnly\":"<<shapeColorOnly.load()<<",\"depthOnly\":"<<shapeDepthOnly.load()<<",\"other\":"<<shapeOther.load()<<",\"loading\":"<<shapeLoads.load()<<"}"
     <<",\"passesTracked\":"<<c.passes.load()<<",\"framebuffersPatched\":"<<(api==Api::Fdm?framebuffersPatched.load():c.framebuffersFoveated.load())<<",\"framebufferFailures\":"<<framebufferFailures.load()
     <<",\"framebuffersTracked\":"<<c.framebuffers.load()<<",\"eyeFramebuffers\":"<<registry.eyeFramebuffers()
     <<",\"begins\":{\"total\":"<<c.begins.load()<<",\"eye\":["<<c.eyeBegins[0].load()<<","<<c.eyeBegins[1].load()<<"],\"substituted\":["<<c.substituted[0].load()<<","<<c.substituted[1].load()
     <<"],\"same\":["<<c.same[0].load()<<","<<c.same[1].load()<<"],\"byLevel\":["<<c.levelBegins[0].load()<<","<<c.levelBegins[1].load()<<","<<c.levelBegins[2].load()
     <<","<<c.levelBegins[3].load()<<","<<c.levelBegins[4].load()<<"],\"eyeClearPasses\":"<<c.clearEye.load()<<",\"eyeLoadPasses\":"<<c.loadEye.load()<<",\"loadKeptFull\":"<<c.loadFull.load()
     <<",\"nonEye\":"<<c.nonEye.load()<<",\"unknown\":"<<c.unknown.load()<<",\"unpatchedEye\":"<<c.unpatchedEye.load()<<",\"notReady\":"<<c.notReady.load()<<",\"pNext\":"<<c.pNext.load()<<"}"
     <<",\"variants\":{\"created\":"<<c.variantsCreated.load()<<",\"destroyed\":"<<c.variantsDestroyed.load()<<",\"failures\":"<<c.variantFailures.load()<<",\"full\":"<<c.variantsFull.load()
     <<",\"evicted\":"<<c.variantsEvicted.load()<<",\"live\":"<<sizes.variants<<"},\"passMismatch\":"<<c.passMismatch.load()<<",\"pairChanges\":"<<c.pairChanges.load()
     <<",\"rightViewsTranslated\":"<<c.translated.load()<<",\"eyeUpdates\":"<<eyeUpdates.load()<<",\"viewFailures\":"<<viewFailures.load()<<",\"frames\":"<<frames.load()
     <<",\"registry\":{\"passes\":"<<sizes.passes<<",\"framebuffers\":"<<sizes.framebuffers<<",\"views\":"<<sizes.views<<"}}";
    return o.str();
}
}
