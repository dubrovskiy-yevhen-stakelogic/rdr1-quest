// Render-thread per-eye TAA. See TaaPass.h for the threading contract.
//
// Per eye (EngineStereoPass, engine frame command buffer, outside any engine render pass, before hands + capture copy):
//   1. every fallible step first (mode, pipelines, history images, views, sets, framebuffers, push constants); any miss
//      = this eye is skipped with nothing recorded (its history then ages out: Reset::Gap),
//   2. eye colour: received layout -> SHADER_READ_ONLY; eye depth: -> DEPTH_STENCIL_READ_ONLY (SAMPLED) or a full copy
//      into a private sampled depth (COPY, when the eye depth has no SAMPLED usage),
//   3. pass R (resolve) into the history slot `out` (full eye, RGBA16F + RG16F, DONT_CARE/STORE),
//   4. pass A (apply) into the eye colour (LOAD/STORE, blend, b = 0 discarded) unless the eye was reset this frame
//      (then b = 0 everywhere and the eye is left untouched; debug bit 2 runs it anyway to prove the discard path),
//   5. every engine image back to its received layout, then the engine's bound state restored (EngineRestore).
// TAA LITE = 3. with taa_lite.frag into a history of the eye's own format (one image, no aux) over a centred area,
//   4. with the "copy" apply (no blend, eye DONT_CARE -> STORE, area only); TAA FULL also uses the copy apply (alpha 1 = the
//   same RGB as its blend). The image barriers of 2.-5. are batched into 3 vkCmdPipelineBarrier calls per eye.
// debug.rdrvr.vr.taa.lite.fp16 1 = pass R of TAA LITE through a 2nd pipeline of taa_lite.frag specialised kHalf (colour math
//   in fp16; built by the worker on the first request, fp32 until then). Same history / pass / layout: switching never resets.
#include "TaaPass.h"
#include "TaaShaders.h"
#include "runtime/AntiAliasingStatus.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <vector>
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::taa {
namespace {
struct Errno {int value{errno};~Errno(){errno=value;}};
std::uint64_t steadyNs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void logLine(const char* what,const char* detail) noexcept {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_WARN,"RdrVrTaa","%s: %s",what,detail?detail:"");
#else
    (void)what;(void)detail;
#endif
}
bool propertyText(const char* name,char* out,std::size_t size) noexcept {
#ifdef __ANDROID__
    char value[PROP_VALUE_MAX]{};
    if(__system_property_get(name,value)<=0||!value[0])return false;
    std::snprintf(out,size,"%s",value);return true;
#else
    (void)name;(void)out;(void)size;return false;
#endif
}
int propertyInt(const char* name,int fallback) noexcept {
    char text[96]{};if(!propertyText(name,text,sizeof text))return fallback;
    char* end{};const long v=std::strtol(text,&end,10);
    return end!=text&&*end==0?int(v):fallback;
}

constexpr VkFormat historyFormat=VK_FORMAT_R16G16B16A16_SFLOAT,auxFormat=VK_FORMAT_R16_SFLOAT;
constexpr unsigned applySlots=4,maxCacheEntries=32,maxSets=64;
constexpr std::uint64_t idleReleaseCalls=600; // ~8 s of eye calls with TAA not requested: the history memory is released
constexpr VkAccessFlags anyAccess=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;

enum class DepthMode : unsigned {None,Sampled,Copy,Count};
const char* depthName(DepthMode d) noexcept {return d==DepthMode::Sampled?"SAMPLED":d==DepthMode::Copy?"COPY":"NONE";}
enum Skip : unsigned {SkipDisabled,SkipBuilding,SkipDevice,SkipPlan,SkipColor,SkipFormat,SkipDepth,SkipRestore,SkipCache,SkipMemory,SkipCount};
constexpr const char* skipNames[SkipCount]{"disabled","building","device","plan","color","format","depth","restore","cache","memory"};

bool hasStencil(VkFormat f) noexcept {return f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_D32_SFLOAT_S8_UINT||f==VK_FORMAT_D16_UNORM_S8_UINT||f==VK_FORMAT_S8_UINT;}
bool depthFormat(VkFormat f) noexcept {
    return f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_X8_D24_UNORM_PACK32||f==VK_FORMAT_D32_SFLOAT||f==VK_FORMAT_D32_SFLOAT_S8_UINT||
        f==VK_FORMAT_D16_UNORM||f==VK_FORMAT_D16_UNORM_S8_UINT;
}
VkImageAspectFlags depthAspects(VkFormat f) noexcept {return VK_IMAGE_ASPECT_DEPTH_BIT|(hasStencil(f)?VK_IMAGE_ASPECT_STENCIL_BIT:0);}
bool colorLayoutOk(VkImageLayout l) noexcept {
    return l==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL||l==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL||l==VK_IMAGE_LAYOUT_GENERAL||l==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}
bool depthLayoutOk(VkImageLayout l) noexcept {
    return l==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL||l==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL||l==VK_IMAGE_LAYOUT_GENERAL||
        l==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL||l==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL||
        l==VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL||l==VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
}
// Working domain of the eye format: 0 = SRGB (sampled values are linear: sqrt -> perceptual, the attachment encodes),
// 1 = UNORM (stored values are already display-encoded), -1 = unsupported (HDR float eyes: TAA would clip them).
int domainFor(VkFormat f) noexcept {
    switch(f){
    case VK_FORMAT_B8G8R8A8_SRGB:case VK_FORMAT_R8G8B8A8_SRGB:return 0;
    case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_A2B10G10R10_UNORM_PACK32:return 1;
    default:return -1;
    }
}
struct VkFailure {const char* what;};
void check(VkResult r,const char* what) {if(r!=VK_SUCCESS)throw VkFailure{what};}
std::uint32_t memoryType(VkPhysicalDevice physical,std::uint32_t bits,VkMemoryPropertyFlags preferred) noexcept {
    VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(physical,&p);
    for(unsigned pass=0;pass<2;++pass)
        for(std::uint32_t i=0;i<p.memoryTypeCount&&i<VK_MAX_MEMORY_TYPES;++i){
            if(!(bits&(1u<<i)))continue;
            if(pass==0&&(p.memoryTypes[i].propertyFlags&preferred)!=preferred)continue;
            return i;
        }
    return UINT32_MAX;
}
VkShaderModule shaderModule(VkDevice d,const std::uint32_t* code,std::size_t bytes,const char* what) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=bytes;ci.pCode=code;
    VkShaderModule s{};check(vkCreateShaderModule(d,&ci,nullptr,&s),what);return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shared GPU objects (worker-built, immutable once published).
struct Base {
    VkSampler nearest{},linear{};
    VkDescriptorSetLayout engineLayout{},historyLayout{};VkPipelineLayout resolveLayout{},applyLayout{};VkDescriptorPool pool{};
    VkShaderModule vertex{},resolve{},apply{},lite{}; // + lite (taa_lite.frag)
    VkRenderPass resolvePass{};VkPipeline resolvePipeline{};
    VkQueryPool queries{};double timestampPeriod{};std::uint64_t timestampMask{};
    std::uint64_t buildNs{};
};
struct ApplySlot {
    std::atomic<int> status{0}; // 0 free, 1 requested, 2 ready, 3 failed
    VkFormat color{};VkRenderPass pass{};VkPipeline pipeline{};std::uint64_t buildNs{};
    VkRenderPass copyPass{},litePass{};VkPipeline copyPipeline{},litePipeline{};
    std::atomic<int> halfStatus{0};VkPipeline litePipelineHalf{}; // fp16 LITE resolve: 0 none, 1 requested, 2 ready, 3 failed
};
struct OwnImage {VkImage image{};VkDeviceMemory memory{};VkImageView view{};VkFormat format{};VkExtent2D extent{};std::uint64_t lastUse{};VkDeviceSize size{};};
struct History {
    std::array<OwnImage,2> color{},aux{};std::array<VkFramebuffer,2> framebuffer{};std::array<VkDescriptorSet,2> set{};
    VkExtent2D extent{};bool initialized{};std::uint64_t lastUse{};
    bool lite{};VkFormat format{}; // TAA LITE history (colour only, in the eye format `format`; no aux; binding 1 = colour again)
    bool exists() const noexcept {return color[0].image!=VK_NULL_HANDLE;}
};
struct ColorView {VkImage image{};std::uint64_t generation{};VkFormat format{};VkImageView view{};std::uint64_t lastUse{};};
struct DepthView {VkImage image{};std::uint64_t generation{};VkImageView view{};std::uint64_t lastUse{};};
struct EngineSet {VkImageView color{},depth{};VkDescriptorSet set{};std::uint64_t lastUse{};};
struct Framebuffer {VkRenderPass pass{};VkImageView color{};VkFramebuffer framebuffer{};std::uint64_t lastUse{};};
struct Dead {std::uint64_t frame{};VkFramebuffer framebuffer{};VkImageView view{};VkImage image{};VkDeviceMemory memory{};VkDescriptorSet set{};};
struct Counters {
    std::atomic<std::uint64_t> calls{},eyes{},applied{},validEyes{},frames{};
    std::array<std::atomic<std::uint64_t>,2> eyesPerEye{};
    std::array<std::atomic<std::uint64_t>,SkipCount> skips{};
    std::array<std::atomic<std::uint64_t>,unsigned(Reset::Count)> resets{};
    std::array<std::atomic<std::uint64_t>,unsigned(DepthMode::Count)> modes{};
    std::atomic<unsigned> lastMode[2]{},lastReset[2]{};
    std::atomic<std::uint64_t> recentValid{},recentWindow{}; // last 256 eye runs (bit history)
    std::atomic<std::uint64_t> views{},framebuffers{},sets{},imageDrops{},graveyard{},graveyardReleased{},histories{},historyReleases{},copyImages{};
    std::atomic<std::uint64_t> historyBytes{};
    std::atomic<std::uint64_t> timed{},gpuLastNs{},gpuAvgNs{},gpuUnavailable{};
    std::atomic<std::uint64_t> restoreCalls{},lastMotionMm{},lastMotionMilliDeg{};
    std::atomic<int> requested{-1};
    // TAA FULL: eye runs in FULL / with the sharpen flag; the jitter each eye was resolved with
    std::atomic<std::uint64_t> fullEyes{},sharpenEyes{},jitteredEyes{},unjitteredFullEyes{};
    std::atomic<std::uint32_t> lastJitterIndex[2]{},lastJitterPacked[2]{};std::atomic<bool> lastJitterOn[2]{};
    std::atomic<std::uint64_t> lastJitterScene{};
    // TAA LITE eye runs; GPU time per row value (0 OFF unused, 1 TAA, 2 TAA FULL, 3 TAA LITE) and the mode of the last sample
    std::atomic<std::uint64_t> liteEyes{};
    // LITE eye runs with the fp16 pipeline / with fp16 requested but the fp32 pipeline (building, failed); GPU EMA per variant
    std::atomic<std::uint64_t> liteHalfEyes{},liteHalfFallback{};std::array<std::atomic<std::uint64_t>,2> gpuLiteAvg{},gpuLiteTimed{};
    std::array<std::atomic<std::uint64_t>,4> gpuAvgByMode{},gpuTimedByMode{};std::atomic<int> gpuLastMode{-1};
    // AppSW TAA -> LITE override (effective row of the last render frame, latch inputs, engagements, frames under it)
    std::atomic<int> effective{-1},appswEnabled{-1};std::atomic<bool> appswSynthesizing{},appswActive{};
    std::atomic<std::uint64_t> appswOverrides{},appswFrames{};
};
struct Module {
    std::atomic<int> state{int(State::Off)};
    std::atomic<const char*> reason{"NOT_CONFIGURED"};
    std::atomic<bool (*)(VkCommandBuffer) noexcept> restoreReady{nullptr};
    std::atomic<void (*)(VkCommandBuffer) noexcept> restore{nullptr};
    std::atomic<bool> configured{false};
    VkPhysicalDevice physical{};VkDevice device{};std::uint32_t family{},queueIndex{};std::mutex* queueMutex{};
    // tuning (configure / tests write; render thread reads)
    std::mutex tuningMutex;Tuning tuning{};std::atomic<int> gpuTiming{1};bool testOverride{};std::atomic<std::uint32_t> liveDebug{0};
    std::uint64_t propertyPoll{};
    // worker
    std::thread worker;std::mutex workMutex;std::condition_variable workCv;std::atomic<bool> stop{false};
    Base base{};std::atomic<bool> baseReady{false};
    std::array<ApplySlot,applySlots> slots{};
    // render-thread state (cacheMutex shared with imageDestroyed only)
    std::mutex cacheMutex;
    std::vector<ColorView> colors;std::vector<DepthView> depths;std::vector<EngineSet> engineSets;std::vector<Framebuffer> framebuffers;std::vector<Dead> dead;
    std::array<History,2> history{};std::array<OwnImage,2> copy{};bool copyUnavailable{};
    std::array<EyeView,2> views{};std::array<bool,2> toggled{};std::uint64_t idleCalls{};
    std::array<std::pair<VkFormat,VkFormatFeatureFlags>,8> formatCache{};
    std::atomic<std::uint64_t> retired{};
    std::array<std::uint64_t,2> fullOkFrame{}; // Render frame of each eye's last successful TAA FULL run (jitter gate)
    // timestamps: 4 slots x {eye0 begin,end, eye1 begin,end}
    std::array<std::uint64_t,4> querySlotFrame{};std::array<bool,4> querySlotPending{};std::uint64_t timedSequence{};unsigned frameQuerySlot{~0u};
    std::array<int,4> querySlotMode{}; // Row value the slot was recorded with
    std::array<int,4> querySlotHalf{}; // -1 unset, 0 fp32, 1 fp16 LITE resolve (both eyes), 2 mixed (not averaged)
    std::uint64_t timingFrame{~0ull};
    // AppSW override latch (render thread; probe/testValue any thread): re-read at most once per second, mode per render frame
    std::atomic<bool (*)() noexcept> synthesizingProbe{nullptr};std::atomic<int> appswTestValue{-1};std::atomic<bool> appswForce{false};
    std::uint64_t appswNextNs{},appswFrame{~0ull};bool appswEnabled{true},appswSynthesizing{};int appswMode{-1};
    Counters counters;
};
Module& module() noexcept {static Module* instance=new Module;return *instance;} // never destroyed: hooks may run during exit

void setState(State s,const char* why) noexcept {
    auto& m=module();m.reason.store(why,std::memory_order_relaxed);m.state.store(int(s),std::memory_order_release);
    if(s==State::Failed)logLine("TAA disabled",why);
}
Tuning tuningSnapshot(Module& m) noexcept {
    Tuning t{};{std::lock_guard lock(m.tuningMutex);t=m.tuning;}
    if(!m.testOverride)t.debug=m.liveDebug.load(std::memory_order_relaxed);
    return t;
}
// TAA LITE levers (also re-read every 64 frames; an area change resets the history: resize).
void readLiteProperties(Tuning& t) noexcept {
    t.liteArea=float(std::clamp(propertyInt("debug.rdrvr.vr.taa.lite.area",70),30,100))/100.f;
    t.liteCubic=propertyInt("debug.rdrvr.vr.taa.lite.cubic",1)!=0; // Default 1 (Catmull-Rom history)
    {const int k=propertyInt("debug.rdrvr.vr.taa.lite.subpx",-1); // unset -> the menu's TAA sharpness (k 1 / 2 / 4)
     const int s=runtime::taaSharpness.load(std::memory_order_relaxed);
     t.liteSubpx=k>=0?float(std::clamp(k,0,400))/100.f:s==2?4.f:s==1?2.f:1.f;}
    t.liteDepth=propertyInt("debug.rdrvr.vr.taa.lite.depth",1)!=0;
    t.liteFp16=propertyInt("debug.rdrvr.vr.taa.lite.fp16",1)!=0; // Default 1 (about 0.5 ms less GPU time than fp32); 0 = the fp32 pipeline
}
void readProperties(Module& m) noexcept {
    Tuning t{};
    t.nearMetres=float(std::clamp(propertyInt("debug.rdrvr.vr.taa.near",100),0,1000))/100.f;
    t.clipGamma=float(std::clamp(propertyInt("debug.rdrvr.vr.taa.clip",125),25,10000))/100.f;
    t.cutMetres=float(std::clamp(propertyInt("debug.rdrvr.vr.taa.cutcm",50),1,100000))/100.f;
    t.cutDegrees=float(std::clamp(propertyInt("debug.rdrvr.vr.taa.cutdeg",5),1,180));
    t.nMaxStill=float(std::clamp(propertyInt("debug.rdrvr.vr.taa.nmax",10),2,64));t.nMaxMoving=std::min(t.nMaxMoving,t.nMaxStill);
    t.sharpen=float(std::clamp(propertyInt("debug.rdrvr.vr.taa.sharp",50),0,200))/100.f;
    t.depthTestMetres=float(std::clamp(propertyInt("debug.rdrvr.vr.taa.depthtest",60),0,10000));
    t.taps=propertyInt("debug.rdrvr.vr.taa.taps",5)==9?9u:5u;
    t.area=float(std::clamp(propertyInt("debug.rdrvr.vr.taa.area",100),30,100))/100.f;
    // TAA FULL: jitter amplitude in % (100 = +-0.5 px, 0 = no jitter: TAA FULL then resolves like TAA + sharpen),
    // Halton sequence length (2..64, default 8). The amplitude is also re-read every 64 frames (live kill switch).
    t.jitterAmplitude=float(std::clamp(propertyInt("debug.rdrvr.vr.taa.jitter",100),0,200))/100.f;
    t.jitterLength=std::uint32_t(std::clamp(propertyInt("debug.rdrvr.vr.taa.jitterlen",8),2,64));
    readLiteProperties(t);
    {std::lock_guard lock(m.tuningMutex);m.tuning=t;}
    m.gpuTiming.store(propertyInt("debug.rdrvr.vr.taa.gpu",1)?1:0,std::memory_order_relaxed);
}
void pollLiveProperties(Module& m) noexcept { // render thread, eye 0, every 64 frames
    if((m.propertyPoll++&63)!=0)return;
    if(!m.testOverride){
        m.liveDebug.store(std::uint32_t(std::clamp(propertyInt("debug.rdrvr.vr.taa.debug",0),0,15)),std::memory_order_relaxed);
        const float amplitude=float(std::clamp(propertyInt("debug.rdrvr.vr.taa.jitter",100),0,200))/100.f;
        Tuning lite{};readLiteProperties(lite);
        std::lock_guard lock(m.tuningMutex);m.tuning.jitterAmplitude=amplitude;
        m.tuning.liteArea=lite.liteArea;m.tuning.liteCubic=lite.liteCubic;m.tuning.liteDepth=lite.liteDepth;m.tuning.liteSubpx=lite.liteSubpx;
        m.tuning.liteFp16=lite.liteFp16;
    }
#ifdef __ANDROID__
    runtime::taaPostFxaa.store(propertyInt("debug.rdrvr.vr.taa.fxaa",0)?1:0,std::memory_order_relaxed);
#endif
}

// ---------------------------------------------------------------------------------------------------------------------
// Worker: base objects, the resolve pipeline, apply pipelines per eye colour format.
void destroyBase(VkDevice d,Base& b) noexcept {
    if(!d){b={};return;}
    if(b.queries)vkDestroyQueryPool(d,b.queries,nullptr);
    if(b.resolvePipeline)vkDestroyPipeline(d,b.resolvePipeline,nullptr);
    if(b.resolvePass)vkDestroyRenderPass(d,b.resolvePass,nullptr);
    for(auto s:{b.vertex,b.resolve,b.apply,b.lite})if(s)vkDestroyShaderModule(d,s,nullptr);
    if(b.pool)vkDestroyDescriptorPool(d,b.pool,nullptr);
    if(b.resolveLayout)vkDestroyPipelineLayout(d,b.resolveLayout,nullptr);
    if(b.applyLayout)vkDestroyPipelineLayout(d,b.applyLayout,nullptr);
    if(b.engineLayout)vkDestroyDescriptorSetLayout(d,b.engineLayout,nullptr);
    if(b.historyLayout)vkDestroyDescriptorSetLayout(d,b.historyLayout,nullptr);
    if(b.nearest)vkDestroySampler(d,b.nearest,nullptr);
    if(b.linear)vkDestroySampler(d,b.linear,nullptr);
    b={};
}
// Shared fixed-function state of both full-screen pipelines.
struct FullScreen {
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkDynamicState dynamic[2]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stages[2]{};
    FullScreen(VkShaderModule vertex,VkShaderModule fragment) noexcept {
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;vp.viewportCount=vp.scissorCount=1;
        rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
        ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;ds.depthCompareOp=VK_COMPARE_OP_ALWAYS;
        dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
        for(auto& s:stages){s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;s.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fragment;
    }
    VkGraphicsPipelineCreateInfo info(VkPipelineLayout layout,VkRenderPass pass,const VkPipelineColorBlendStateCreateInfo* blend) const noexcept {
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;
        pi.pInputAssemblyState=&ia;pi.pViewportState=&vp;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pDepthStencilState=&ds;
        pi.pColorBlendState=blend;pi.pDynamicState=&dy;pi.layout=layout;pi.renderPass=pass;return pi;
    }
};
void dependencies(std::array<VkSubpassDependency,2>& deps) noexcept {
    deps[0]={};deps[0].srcSubpass=VK_SUBPASS_EXTERNAL;deps[0].dstSubpass=0;deps[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;deps[0].srcAccessMask=anyAccess;
    deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
    deps[1]={};deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;deps[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;deps[1].dstAccessMask=anyAccess;
}
void buildBase(Module& m) {
    const auto start=steadyNs();
    const VkDevice d=m.device;Base b{};
    try{
        const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        for(VkFormat f:{historyFormat,auxFormat}){VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(m.physical,f,&p);
            if((p.optimalTilingFeatures&need)!=need)throw VkFailure{"HISTORY_FORMAT"};}
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_NEAREST;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=0;
        check(vkCreateSampler(d,&si,nullptr,&b.nearest),"SAMPLER");
        si.magFilter=si.minFilter=VK_FILTER_LINEAR;check(vkCreateSampler(d,&si,nullptr,&b.linear),"SAMPLER");
        const VkDescriptorSetLayoutBinding bindings[2]{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
                                                        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=2;dl.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(d,&dl,nullptr,&b.engineLayout),"SET_LAYOUT");
        check(vkCreateDescriptorSetLayout(d,&dl,nullptr,&b.historyLayout),"SET_LAYOUT");
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(Push)};
        const VkDescriptorSetLayout resolveSets[2]{b.engineLayout,b.historyLayout};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=2;pl.pSetLayouts=resolveSets;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(d,&pl,nullptr,&b.resolveLayout),"PIPELINE_LAYOUT");
        pl.setLayoutCount=1;pl.pSetLayouts=&b.historyLayout;check(vkCreatePipelineLayout(d,&pl,nullptr,&b.applyLayout),"PIPELINE_LAYOUT");
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*maxSets};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dp.maxSets=maxSets;dp.poolSizeCount=1;dp.pPoolSizes=&poolSize;
        check(vkCreateDescriptorPool(d,&dp,nullptr,&b.pool),"DESCRIPTOR_POOL");
        b.vertex=shaderModule(d,shaders::vertex,sizeof(shaders::vertex),"VERTEX_SHADER");
        b.resolve=shaderModule(d,shaders::resolve,sizeof(shaders::resolve),"RESOLVE_SHADER");
        b.apply=shaderModule(d,shaders::apply,sizeof(shaders::apply),"APPLY_SHADER");
        b.lite=shaderModule(d,shaders::lite,sizeof(shaders::lite),"LITE_SHADER");
        // Resolve pass: history colour + aux, whole eye, DONT_CARE -> STORE, UNDEFINED -> SHADER_READ_ONLY.
        std::array<VkAttachmentDescription,2> a{};
        for(unsigned i=0;i<2;++i){a[i].format=i?auxFormat:historyFormat;a[i].samples=VK_SAMPLE_COUNT_1_BIT;a[i].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;a[i].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a[i].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a[i].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;a[i].finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;}
        const VkAttachmentReference refs[2]{{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=2;sub.pColorAttachments=refs;
        std::array<VkSubpassDependency,2> deps{};dependencies(deps);
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=2;rp.pAttachments=a.data();rp.subpassCount=1;rp.pSubpasses=&sub;
        rp.dependencyCount=2;rp.pDependencies=deps.data();
        check(vkCreateRenderPass(d,&rp,nullptr,&b.resolvePass),"RESOLVE_PASS");
        VkPipelineColorBlendAttachmentState write[2]{};
        write[0].colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        write[1]=write[0]; // identical states: independentBlend is not required (the RG16F attachment ignores B/A)
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=2;cb.pAttachments=write;
        const FullScreen fs(b.vertex,b.resolve);const auto pi=fs.info(b.resolveLayout,b.resolvePass,&cb);
        check(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&pi,nullptr,&b.resolvePipeline),"RESOLVE_PIPELINE");
        if(m.gpuTiming.load(std::memory_order_relaxed)){
            std::uint32_t count{};vkGetPhysicalDeviceQueueFamilyProperties(m.physical,&count,nullptr);
            std::vector<VkQueueFamilyProperties> families(count);vkGetPhysicalDeviceQueueFamilyProperties(m.physical,&count,families.data());
            VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(m.physical,&props);
            if(m.family<count&&families[m.family].timestampValidBits&&props.limits.timestampPeriod>0){
                const auto bits=families[m.family].timestampValidBits;b.timestampMask=bits>=64?UINT64_MAX:(1ull<<bits)-1;b.timestampPeriod=props.limits.timestampPeriod;
                VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};q.queryType=VK_QUERY_TYPE_TIMESTAMP;q.queryCount=16;
                if(vkCreateQueryPool(d,&q,nullptr,&b.queries)!=VK_SUCCESS)b.queries={};
            }
        }
    }catch(const VkFailure&){destroyBase(d,b);throw;}
    catch(...){destroyBase(d,b);throw VkFailure{"EXCEPTION"};}
    b.buildNs=steadyNs()-start;m.base=b;
}
void destroySlot(VkDevice d,ApplySlot& s) noexcept {
    if(d){for(auto p:{s.pipeline,s.copyPipeline,s.litePipeline,s.litePipelineHalf})if(p)vkDestroyPipeline(d,p,nullptr);
        for(auto p:{s.pass,s.copyPass,s.litePass})if(p)vkDestroyRenderPass(d,p,nullptr);}
    s.pipeline=s.copyPipeline=s.litePipeline=s.litePipelineHalf={};s.pass=s.copyPass=s.litePass={};s.halfStatus.store(0,std::memory_order_release);
}
// One colour attachment of `format`, one subpass, the shared external dependencies.
VkRenderPass singlePass(VkDevice d,VkFormat format,VkAttachmentLoadOp load,VkImageLayout initial,VkImageLayout final,const char* what) {
    VkAttachmentDescription a{};a.format=format;a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=load;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;a.initialLayout=initial;a.finalLayout=final;
    const VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
    std::array<VkSubpassDependency,2> deps{};dependencies(deps);
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&a;rp.subpassCount=1;rp.pSubpasses=&sub;
    rp.dependencyCount=2;rp.pDependencies=deps.data();
    VkRenderPass pass{};check(vkCreateRenderPass(d,&rp,nullptr,&pass),what);return pass;
}
// Apply pass {eye colour LOAD/STORE} + blended pipeline for one eye colour format.
void buildSlot(Module& m,ApplySlot& s) {
    const auto start=steadyNs();const VkDevice d=m.device;const Base& b=m.base;
    try{
        VkFormatProperties cp{};vkGetPhysicalDeviceFormatProperties(m.physical,s.color,&cp);
        const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if((cp.optimalTilingFeatures&need)!=need)throw VkFailure{"COLOR_FORMAT"};
        VkAttachmentDescription a{};a.format=s.color;a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
        std::array<VkSubpassDependency,2> deps{};dependencies(deps);
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&a;rp.subpassCount=1;rp.pSubpasses=&sub;
        rp.dependencyCount=2;rp.pDependencies=deps.data();
        check(vkCreateRenderPass(d,&rp,nullptr,&s.pass),"APPLY_PASS");
        // eye = TAA * b + eye * (1 - b), RGB only (the eye's alpha is never touched).
        VkPipelineColorBlendAttachmentState blend{};blend.blendEnable=VK_TRUE;
        blend.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;blend.colorBlendOp=VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;blend.alphaBlendOp=VK_BLEND_OP_ADD;
        blend.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&blend;
        const FullScreen fs(b.vertex,b.apply);const auto pi=fs.info(b.applyLayout,s.pass,&cb);
        check(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&pi,nullptr,&s.pipeline),"APPLY_PIPELINE");
        // "copy" apply = the same shader without blending into a DONT_CARE eye (TAA FULL: the same RGB as
        // the blend with alpha 1; TAA LITE: the LITE history copied). Writes RGBA (the eye alpha becomes 1: the
        // projection layer ignores it).
        s.copyPass=singlePass(d,s.color,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,"COPY_PASS");
        VkPipelineColorBlendAttachmentState write{};
        write.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo wb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};wb.attachmentCount=1;wb.pAttachments=&write;
        const auto ci=fs.info(b.applyLayout,s.copyPass,&wb);
        check(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&ci,nullptr,&s.copyPipeline),"COPY_PIPELINE");
        // TAA LITE resolve: the history is an image of the eye format (needs linear filtering for the bilinear history tap).
        if((cp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)&&b.lite){
            s.litePass=singlePass(d,s.color,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,"LITE_PASS");
            const FullScreen ls(b.vertex,b.lite);const auto li=ls.info(b.resolveLayout,s.litePass,&wb);
            check(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&li,nullptr,&s.litePipeline),"LITE_PIPELINE");
        }
    }catch(const VkFailure&){destroySlot(d,s);throw;}
    catch(...){destroySlot(d,s);throw VkFailure{"EXCEPTION"};}
    s.buildNs=steadyNs()-start;
}
// The fp16 LITE resolve = the LITE pipeline with taa_lite.frag specialised kHalf = true (same module, pass, layout, blend).
// Built on the first request (debug.rdrvr.vr.taa.lite.fp16 1); failure = status 3, the fp32 pipeline keeps running.
void buildLiteHalf(Module& m,ApplySlot& s) noexcept {
    const Base& b=m.base;VkPipeline p{};
    if(s.litePass&&b.lite){
        const VkBool32 half=VK_TRUE;const VkSpecializationMapEntry entry{liteHalfSpecId,0,sizeof half};
        const VkSpecializationInfo spec{1,&entry,sizeof half,&half};
        VkPipelineColorBlendAttachmentState write{};
        write.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo wb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};wb.attachmentCount=1;wb.pAttachments=&write;
        FullScreen fs(b.vertex,b.lite);fs.stages[1].pSpecializationInfo=&spec;const auto pi=fs.info(b.resolveLayout,s.litePass,&wb);
        if(vkCreateGraphicsPipelines(m.device,VK_NULL_HANDLE,1,&pi,nullptr,&p)!=VK_SUCCESS)p={};
    }
    s.litePipelineHalf=p;s.halfStatus.store(p?2:3,std::memory_order_release);
    if(!p)logLine("TAA LITE fp16 pipeline failed","taa");
}
bool anyRequested(Module& m) noexcept {
    for(auto& s:m.slots){const int st=s.status.load(std::memory_order_acquire);
        if(st==1||(st==2&&s.halfStatus.load(std::memory_order_acquire)==1))return true;} // + fp16 LITE requests
    return false;
}
void workerMain() noexcept {
    auto& m=module();
    try{buildBase(m);m.baseReady.store(true,std::memory_order_release);setState(State::Ready,"OK");}
    catch(const VkFailure& f){setState(State::Failed,f.what);return;}
    catch(...){setState(State::Failed,"BASE_EXCEPTION");return;}
    while(!m.stop.load(std::memory_order_acquire)){
        for(auto& s:m.slots){
            if(m.stop.load(std::memory_order_acquire))break;
            if(s.status.load(std::memory_order_acquire)==2&&s.halfStatus.load(std::memory_order_acquire)==1){buildLiteHalf(m,s);continue;}
            if(s.status.load(std::memory_order_acquire)!=1)continue;
            try{buildSlot(m,s);s.status.store(2,std::memory_order_release);}
            catch(const VkFailure& f){s.status.store(3,std::memory_order_release);logLine("TAA apply pipeline failed",f.what);}
            catch(...){s.status.store(3,std::memory_order_release);}
        }
        std::unique_lock lock(m.workMutex);
        m.workCv.wait_for(lock,std::chrono::milliseconds(50),[&]{return m.stop.load(std::memory_order_acquire)||anyRequested(m);});
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Render thread helpers (cacheMutex held unless noted).
VkFormatFeatureFlags formatFeatures(Module& m,VkFormat f) noexcept {
    for(auto& [format,features]:m.formatCache)if(format==f)return features;
    VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(m.physical,f,&p);
    for(auto& entry:m.formatCache)if(entry.first==VK_FORMAT_UNDEFINED){entry={f,p.optimalTilingFeatures};break;}
    return p.optimalTilingFeatures;
}
ApplySlot* applySlot(Module& m,VkFormat color,bool& failed) noexcept {
    failed=false;
    for(auto& s:m.slots){
        const int st=s.status.load(std::memory_order_acquire);
        if(st==0||s.color!=color)continue;
        if(st==3)failed=true;
        return st==2?&s:nullptr;
    }
    for(auto& s:m.slots)if(s.status.load(std::memory_order_acquire)==0){s.color=color;s.status.store(1,std::memory_order_release);m.workCv.notify_one();return nullptr;}
    failed=true;return nullptr;
}
void destroyDead(VkDevice d,Module& m,const Dead& x) noexcept {
    if(x.framebuffer)vkDestroyFramebuffer(d,x.framebuffer,nullptr);
    if(x.set&&m.base.pool)vkFreeDescriptorSets(d,m.base.pool,1,&x.set);
    if(x.view)vkDestroyImageView(d,x.view,nullptr);
    if(x.image)vkDestroyImage(d,x.image,nullptr);
    if(x.memory)vkFreeMemory(d,x.memory,nullptr);
}
void bury(Module& m,const Dead& x,std::uint64_t retired) {
    if(x.frame<=retired){destroyDead(m.device,m,x);return;}
    m.dead.push_back(x);m.counters.graveyard.fetch_add(1,std::memory_order_relaxed);
}
void releaseDeferred(Module& m,std::uint64_t retired) noexcept {
    auto& v=m.dead;std::size_t keep=0;
    for(auto& x:v){if(x.frame<=retired){destroyDead(m.device,m,x);m.counters.graveyardReleased.fetch_add(1,std::memory_order_relaxed);}else v[keep++]=x;}
    v.resize(keep);
}
void dropFramebuffersOf(Module& m,VkImageView view,std::uint64_t retired) {
    auto& v=m.framebuffers;std::size_t keep=0;
    for(auto& f:v){if(f.color==view){Dead x{};x.frame=f.lastUse;x.framebuffer=f.framebuffer;bury(m,x,retired);}else v[keep++]=f;}
    v.resize(keep);
}
void dropSetsOf(Module& m,VkImageView view,std::uint64_t retired) {
    auto& v=m.engineSets;std::size_t keep=0;
    for(auto& s:v){if(s.color==view||s.depth==view){Dead x{};x.frame=s.lastUse;x.set=s.set;bury(m,x,retired);}else v[keep++]=s;}
    v.resize(keep);
}
void releaseOwn(Module& m,OwnImage& o,std::uint64_t lastUse,std::uint64_t retired) {
    if(!o.image)return;
    Dead x{};x.frame=lastUse;x.view=o.view;x.image=o.image;x.memory=o.memory;bury(m,x,retired);
    m.counters.historyBytes.fetch_sub(std::min<std::uint64_t>(o.size,m.counters.historyBytes.load(std::memory_order_relaxed)),std::memory_order_relaxed);o={};
}
void releaseHistory(Module& m,History& h,std::uint64_t retired) {
    if(!h.exists())return;
    for(unsigned i=0;i<2;++i){
        Dead x{};x.frame=h.lastUse;x.framebuffer=h.framebuffer[i];x.set=h.set[i];bury(m,x,retired);
        releaseOwn(m,h.color[i],h.lastUse,retired);releaseOwn(m,h.aux[i],h.lastUse,retired);
    }
    h={};m.counters.historyReleases.fetch_add(1,std::memory_order_relaxed);
}
bool createImage(Module& m,OwnImage& o,VkFormat format,VkExtent2D extent,VkImageUsageFlags usage,VkImageAspectFlags aspect) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={extent.width,extent.height,1};
    ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;ci.usage=usage;
    OwnImage n{};n.format=format;n.extent=extent;
    if(vkCreateImage(m.device,&ci,nullptr,&n.image)!=VK_SUCCESS)return false;
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(m.device,n.image,&req);
    const auto type=memoryType(m.physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=req.size;a.memoryTypeIndex=type;
    if(type==UINT32_MAX||vkAllocateMemory(m.device,&a,nullptr,&n.memory)!=VK_SUCCESS||vkBindImageMemory(m.device,n.image,n.memory,0)!=VK_SUCCESS){
        if(n.memory)vkFreeMemory(m.device,n.memory,nullptr);vkDestroyImage(m.device,n.image,nullptr);return false;}
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=n.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
    vi.subresourceRange={aspect,0,1,0,1};
    if(vkCreateImageView(m.device,&vi,nullptr,&n.view)!=VK_SUCCESS){vkFreeMemory(m.device,n.memory,nullptr);vkDestroyImage(m.device,n.image,nullptr);return false;}
    n.size=req.size;m.counters.historyBytes.fetch_add(req.size,std::memory_order_relaxed);
    o=n;return true;
}
// Both history slots of one eye (colour + aux, framebuffers, sets). On failure everything created so far is released.
// lite = the TAA LITE history (one colour image per slot in the eye format `liteFormat`, framebuffer of `litePass`, no
// aux; the set binds the colour view at both bindings). A TAA <-> TAA LITE switch recreates the history (Reset::Resize).
bool ensureHistory(Module& m,History& h,VkExtent2D extent,std::uint64_t frame,std::uint64_t retired,bool& recreated,
                   bool lite=false,VkFormat liteFormat=VK_FORMAT_UNDEFINED,VkRenderPass litePass=VK_NULL_HANDLE) {
    recreated=false;
    if(h.exists()&&h.extent.width==extent.width&&h.extent.height==extent.height&&h.lite==lite&&(!lite||h.format==liteFormat)){h.lastUse=frame;return true;}
    releaseHistory(m,h,retired);recreated=true;
    History n{};n.extent=extent;n.lastUse=frame;n.lite=lite;n.format=lite?liteFormat:historyFormat;
    const VkImageUsageFlags usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    bool ok=true;
    for(unsigned i=0;i<2&&ok;++i)ok=lite?createImage(m,n.color[i],liteFormat,extent,usage,VK_IMAGE_ASPECT_COLOR_BIT):
        createImage(m,n.color[i],historyFormat,extent,usage,VK_IMAGE_ASPECT_COLOR_BIT)&&createImage(m,n.aux[i],auxFormat,extent,usage,VK_IMAGE_ASPECT_COLOR_BIT);
    for(unsigned i=0;i<2&&ok;++i){
        const VkImageView views[2]{n.color[i].view,lite?n.color[i].view:n.aux[i].view};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=lite?litePass:m.base.resolvePass;fi.attachmentCount=lite?1:2;fi.pAttachments=views;
        fi.width=extent.width;fi.height=extent.height;fi.layers=1;
        ok=vkCreateFramebuffer(m.device,&fi,nullptr,&n.framebuffer[i])==VK_SUCCESS;
        if(!ok)break;
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=m.base.pool;da.descriptorSetCount=1;da.pSetLayouts=&m.base.historyLayout;
        ok=vkAllocateDescriptorSets(m.device,&da,&n.set[i])==VK_SUCCESS;
        if(!ok)break;
        const VkDescriptorImageInfo infos[2]{{m.base.linear,n.color[i].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{m.base.linear,views[1],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[2]{};
        for(unsigned k=0;k<2;++k){writes[k]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[k].dstSet=n.set[i];writes[k].dstBinding=k;writes[k].descriptorCount=1;
            writes[k].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[k].pImageInfo=&infos[k];}
        vkUpdateDescriptorSets(m.device,2,writes,0,nullptr);
    }
    if(!ok){
        for(unsigned i=0;i<2;++i){
            if(n.framebuffer[i])vkDestroyFramebuffer(m.device,n.framebuffer[i],nullptr);
            if(n.set[i])vkFreeDescriptorSets(m.device,m.base.pool,1,&n.set[i]);
            for(auto* o:{&n.color[i],&n.aux[i]})if(o->image){vkDestroyImageView(m.device,o->view,nullptr);vkDestroyImage(m.device,o->image,nullptr);vkFreeMemory(m.device,o->memory,nullptr);
                m.counters.historyBytes.fetch_sub(std::min<std::uint64_t>(o->size,m.counters.historyBytes.load(std::memory_order_relaxed)),std::memory_order_relaxed);}
        }
        return false;
    }
    h=n;m.counters.histories.fetch_add(1,std::memory_order_relaxed);return true;
}
// Private sampled copy of an eye depth without SAMPLED usage (COPY mode), one per eye.
bool ensureCopy(Module& m,OwnImage& o,VkFormat format,VkExtent2D extent,std::uint64_t frame,std::uint64_t retired) {
    if(o.image&&o.format==format&&o.extent.width==extent.width&&o.extent.height==extent.height){o.lastUse=frame;return true;}
    dropSetsOf(m,o.view,retired);releaseOwn(m,o,o.lastUse,retired);
    OwnImage n{};
    if(!createImage(m,n,format,extent,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_IMAGE_ASPECT_DEPTH_BIT))return false;
    n.lastUse=frame;o=n;m.counters.copyImages.fetch_add(1,std::memory_order_relaxed);return true;
}
VkImageView colorView(Module& m,const hands::EyeTarget& t,std::uint64_t frame) {
    for(auto& c:m.colors)if(c.image==t.image&&c.generation==t.generation&&c.format==t.format){c.lastUse=frame;return c.view;}
    if(m.colors.size()>=maxCacheEntries)return VK_NULL_HANDLE;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=t.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=t.format;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView view{};if(vkCreateImageView(m.device,&vi,nullptr,&view)!=VK_SUCCESS)return VK_NULL_HANDLE;
    m.colors.push_back({t.image,t.generation,t.format,view,frame});m.counters.views.fetch_add(1,std::memory_order_relaxed);return view;
}
VkImageView depthView(Module& m,const hands::EyeTarget& t,std::uint64_t frame) {
    for(auto& e:m.depths)if(e.image==t.image&&e.generation==t.generation){e.lastUse=frame;return e.view;}
    if(m.depths.size()>=maxCacheEntries)return VK_NULL_HANDLE;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=t.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=t.format;
    vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    VkImageView view{};if(vkCreateImageView(m.device,&vi,nullptr,&view)!=VK_SUCCESS)return VK_NULL_HANDLE;
    m.depths.push_back({t.image,t.generation,view,frame});m.counters.views.fetch_add(1,std::memory_order_relaxed);return view;
}
// {eye colour, eye depth (DEPTH_STENCIL_READ_ONLY_OPTIMAL)} for the resolve pass.
VkDescriptorSet engineSet(Module& m,VkImageView color,VkImageView depth,std::uint64_t frame) {
    for(auto& s:m.engineSets)if(s.color==color&&s.depth==depth){s.lastUse=frame;return s.set;}
    if(m.engineSets.size()>=maxCacheEntries)return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=m.base.pool;da.descriptorSetCount=1;da.pSetLayouts=&m.base.engineLayout;
    VkDescriptorSet set{};if(vkAllocateDescriptorSets(m.device,&da,&set)!=VK_SUCCESS)return VK_NULL_HANDLE;
    const VkDescriptorImageInfo infos[2]{{m.base.nearest,color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{m.base.nearest,depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for(unsigned k=0;k<2;++k){writes[k]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[k].dstSet=set;writes[k].dstBinding=k;writes[k].descriptorCount=1;
        writes[k].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[k].pImageInfo=&infos[k];}
    vkUpdateDescriptorSets(m.device,2,writes,0,nullptr);
    m.engineSets.push_back({color,depth,set,frame});m.counters.sets.fetch_add(1,std::memory_order_relaxed);return set;
}
VkFramebuffer applyFramebuffer(Module& m,VkRenderPass pass,VkImageView color,VkExtent2D extent,std::uint64_t frame) {
    for(auto& f:m.framebuffers)if(f.pass==pass&&f.color==color){f.lastUse=frame;return f.framebuffer;}
    if(m.framebuffers.size()>=maxCacheEntries)return VK_NULL_HANDLE;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=pass;fi.attachmentCount=1;fi.pAttachments=&color;
    fi.width=extent.width;fi.height=extent.height;fi.layers=1;
    VkFramebuffer fb{};if(vkCreateFramebuffer(m.device,&fi,nullptr,&fb)!=VK_SUCCESS)return VK_NULL_HANDLE;
    m.framebuffers.push_back({pass,color,fb,frame});m.counters.framebuffers.fetch_add(1,std::memory_order_relaxed);return fb;
}
// Timestamps (GPU estimate): one timed render frame in 16, ring of 4 slots of 4 queries (eye x begin/end).
void readTimestamps(Module& m,std::uint64_t frame) noexcept {
    const auto& b=m.base;if(!b.queries)return;
    for(unsigned slot=0;slot<4;++slot){
        if(!m.querySlotPending[slot])continue;
        struct Value {std::uint64_t ticks,available;};std::array<Value,4> v{};
        const auto r=vkGetQueryPoolResults(m.device,b.queries,slot*4,4,sizeof(v),v.data(),sizeof(Value),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if(r!=VK_SUCCESS&&r!=VK_NOT_READY)continue;
        double ns=0;bool any=false,waiting=false;
        for(unsigned e=0;e<2;++e){
            if(v[2*e].available&&v[2*e+1].available){ns+=double((v[2*e+1].ticks-v[2*e].ticks)&b.timestampMask)*b.timestampPeriod;any=true;}
            else if(v[2*e].available||v[2*e+1].available)waiting=true;
        }
        if(waiting||(!any&&frame<m.querySlotFrame[slot]+64))continue;
        m.querySlotPending[slot]=false;
        if(!any||!(ns>=0)||ns>1e8){m.counters.gpuUnavailable.fetch_add(1,std::memory_order_relaxed);continue;}
        const auto value=std::uint64_t(ns);m.counters.gpuLastNs.store(value,std::memory_order_relaxed);m.counters.timed.fetch_add(1,std::memory_order_relaxed);
        // The running average restarts when the row changes (gpu.mode); a per-row average is kept as well (gpu.byMode)
        const int mode=m.querySlotMode[slot];auto& k=m.counters;
        const bool same=k.gpuLastMode.exchange(mode,std::memory_order_relaxed)==mode;
        const auto avg=same?k.gpuAvgNs.load(std::memory_order_relaxed):0;
        k.gpuAvgNs.store(avg?(avg*7+value)/8:value,std::memory_order_relaxed);
        if(mode>=0&&mode<4){const auto a=k.gpuAvgByMode[unsigned(mode)].load(std::memory_order_relaxed);
            k.gpuAvgByMode[unsigned(mode)].store(a?(a*7+value)/8:value,std::memory_order_relaxed);k.gpuTimedByMode[unsigned(mode)].fetch_add(1,std::memory_order_relaxed);}
        const int half=m.querySlotHalf[slot]; // LITE fp32 / fp16 resolve averaged separately (mixed frames are not averaged)
        if(mode==runtime::antiAliasingRowTaaLite&&(half==0||half==1)){const auto a=k.gpuLiteAvg[unsigned(half)].load(std::memory_order_relaxed);
            k.gpuLiteAvg[unsigned(half)].store(a?(a*7+value)/8:value,std::memory_order_relaxed);k.gpuLiteTimed[unsigned(half)].fetch_add(1,std::memory_order_relaxed);}
    }
}
int requestedMode() noexcept { // TAA / TAA FULL run as TAA LITE (any source: cfg, property, menu)
    const int r=runtime::antiAliasingRequested.load(std::memory_order_relaxed);
    return r==runtime::antiAliasingRowTaa||r==runtime::antiAliasingRowTaaFull?runtime::antiAliasingRowTaaLite:r;}
bool taaMode(int mode) noexcept {
    return mode==runtime::antiAliasingRowTaa||mode==runtime::antiAliasingRowTaaFull||mode==runtime::antiAliasingRowTaaLite;}
bool fullMode(int mode) noexcept {return mode==runtime::antiAliasingRowTaaFull;}
bool liteMode(int mode) noexcept {return mode==runtime::antiAliasingRowTaaLite;}
const char* modeName(int mode) noexcept {
    return mode==runtime::antiAliasingRowTaa?"TAA":mode==runtime::antiAliasingRowTaaFull?"TAA FULL":mode==runtime::antiAliasingRowTaaLite?"TAA LITE":mode==0?"OFF":"NONE";}
// The row value this render frame runs for (TaaPass.h appSwEffectiveMode). Evaluated once per render frame (eye 0, or the
// first eye of a new frame) so both eyes use one mode; debug.rdrvr.vr.taa.appswlite and the AppSW probe are sampled at most
// once per second (a switch dwells >= 1 s; synthesizing() itself drops only after 72 XR frames without warp info). A change of
// the result takes recordEye's row-change path: ensureHistory (lite flag / format) + margins -> Reset::Resize on both eyes.
int frameMode(Module& m,const hands::EyeCall& call,int requested) noexcept {
    if(call.eye!=0&&call.renderFrame==m.appswFrame)return m.appswMode;
    auto& k=m.counters;const auto now=steadyNs();
    if(m.appswForce.exchange(false,std::memory_order_acq_rel)||now>=m.appswNextNs){m.appswNextNs=now+1000000000ull;
        const int test=m.appswTestValue.load(std::memory_order_relaxed);
        m.appswEnabled=(test>=0?test:propertyInt("debug.rdrvr.vr.taa.appswlite",1))!=0;
        const auto probe=m.synthesizingProbe.load(std::memory_order_acquire);m.appswSynthesizing=probe&&probe();
        k.appswEnabled.store(m.appswEnabled?1:0,std::memory_order_relaxed);k.appswSynthesizing.store(m.appswSynthesizing,std::memory_order_relaxed);}
    const int mode=appSwEffectiveMode(requested,m.appswSynthesizing,m.appswEnabled);const bool active=mode!=requested;
    if(active&&!k.appswActive.load(std::memory_order_relaxed))k.appswOverrides.fetch_add(1,std::memory_order_relaxed);
    if(active)k.appswFrames.fetch_add(1,std::memory_order_relaxed);
    k.appswActive.store(active,std::memory_order_relaxed);k.effective.store(mode,std::memory_order_relaxed);
    m.appswFrame=call.renderFrame;m.appswMode=mode;return mode;
}
// One vkCmdPipelineBarrier for several images: each call costs a full drain + cache flush on the
// tiled GPU (one call per image would be ten per frame). The image barriers use ALL_COMMANDS scopes.
struct Barriers {
    std::array<VkImageMemoryBarrier,6> b{};unsigned n{};
    void add(VkImage image,VkImageAspectFlags aspect,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst) noexcept {
        if(!image||n>=b.size())return;
        auto& x=b[n++];x={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};x.image=image;x.oldLayout=from;x.newLayout=to;x.srcAccessMask=src;x.dstAccessMask=dst;
        x.srcQueueFamilyIndex=x.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;x.subresourceRange={aspect,0,1,0,1};
    }
    void flush(VkCommandBuffer c) noexcept {
        if(n)vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,n,b.data());n=0;
    }
};
// TAA LITE area: centred rect, margins rounded down to 16 px (render-area granularity of the tiler; the periphery stays raw).
std::uint32_t liteMargins(std::uint32_t width,std::uint32_t height,float area) noexcept {
    const auto m=areaMargins(width,height,area);return (m&0xFFF0u)|((m>>16)&0xFFF0u)<<16;
}
// The jitter gate (TaaJitter.h): jitter only while TAA FULL really ran on both eyes of the last frame.
void publishJitter(const Tuning& t,bool on) noexcept {
    jitter::published.amplitudeMilli.store(std::uint32_t(std::lround(double(t.jitterAmplitude)*1000.0)),std::memory_order_relaxed);
    jitter::published.length.store(t.jitterLength,std::memory_order_relaxed);
    jitter::published.enabled.store(on&&t.jitterAmplitude>0.f?1u:0u,std::memory_order_relaxed);
}
bool hookEntry(const hands::EyeCall& call) noexcept {return ::rdrvr::vr::taa::recordEye(call);} // qualified: hands::recordEye is found by ADL
void publishMenu(Module& m) noexcept {
    const auto st=State(m.state.load(std::memory_order_acquire));
    int v=2;
    if(st==State::Killed)v=0;else if(st==State::Building||st==State::Off)v=1;else if(st==State::Failed)v=3;
    else{const auto& k=m.counters;
        if(k.lastMode[0].load(std::memory_order_relaxed)==unsigned(DepthMode::None)&&k.skips[SkipDepth].load(std::memory_order_relaxed))v=4;
        if(k.skips[SkipFormat].load(std::memory_order_relaxed)&&!k.eyes.load(std::memory_order_relaxed))v=5;}
    if(m.counters.appswActive.load(std::memory_order_relaxed))v|=menuStateAppSwLite; // "TAA > LITE (APPSW)"
    runtime::taaMenuState.store(v,std::memory_order_relaxed);
}
} // namespace

// =====================================================================================================================
const char* stateName(State s) noexcept {
    switch(s){case State::Off:return "OFF";case State::Killed:return "KILLED";case State::Building:return "BUILDING";
    case State::Ready:return "READY";case State::Failed:return "FAILED";default:return "UNKNOWN";}
}
State state() noexcept {return State(module().state.load(std::memory_order_acquire));}
void setEngineRestore(hands::EngineRestore r) noexcept {
    auto& m=module();m.restoreReady.store(r.ready,std::memory_order_release);m.restore.store(r.restore,std::memory_order_release);
}
void setTuningForTests(const Tuning& t,int gpu) noexcept {
    auto& m=module();{std::lock_guard lock(m.tuningMutex);m.tuning=t;}
    m.gpuTiming.store(gpu?1:0,std::memory_order_relaxed);m.testOverride=true;
}
// AppSW TAA -> LITE override (TaaPass.h)
void setSynthesizingProbe(bool (*probe)() noexcept) noexcept {module().synthesizingProbe.store(probe,std::memory_order_release);}
int effectiveMode() noexcept {return module().counters.effective.load(std::memory_order_relaxed);}
bool appSwOverrideActive() noexcept {return module().counters.appswActive.load(std::memory_order_relaxed);}
int signatureDigit() noexcept {
    const int mode=effectiveMode();return state()==State::Ready&&mode>=0&&mode<=runtime::antiAliasingRowTaaLite?mode:0;}
void setAppSwLiteForTests(int value) noexcept {
    auto& m=module();m.appswTestValue.store(value,std::memory_order_relaxed);m.appswForce.store(true,std::memory_order_release);}
void configure(VkPhysicalDevice physical,VkDevice device,std::uint32_t family,std::uint32_t queueIndex,std::mutex* queueMutex) noexcept {
    Errno preserve;auto& m=module();
    if(!m.configured.load(std::memory_order_acquire)){
        if(!physical||!device)return;
        char kill[96]{};
        if(propertyText("debug.rdrvr.vr.taa",kill,sizeof kill)&&kill[0]=='0'){
            setState(State::Killed,"PROPERTY");m.configured.store(true,std::memory_order_release);publishMenu(m);return;
        }
        if(!m.testOverride){readProperties(m);m.propertyPoll=0;pollLiveProperties(m);}
        m.physical=physical;m.device=device;m.family=family;m.queueIndex=queueIndex;m.queueMutex=queueMutex;
        m.stop.store(false,std::memory_order_release);
        setState(State::Building,"BUILDING");
        try{m.worker=std::thread(workerMain);}
        catch(...){setState(State::Failed,"THREAD");m.configured.store(true,std::memory_order_release);publishMenu(m);return;}
        eyeHook.store(&hookEntry,std::memory_order_release);
        m.configured.store(true,std::memory_order_release);
    }
    publishMenu(m);
}
void imageDestroyed(VkDevice device,VkImage image) noexcept {
    Errno preserve;auto& m=module();
    if(!image||!m.configured.load(std::memory_order_acquire)||device!=m.device)return;
    try{
        std::lock_guard lock(m.cacheMutex);
        const auto retired=m.retired.load(std::memory_order_acquire);
        for(std::size_t i=0;i<m.colors.size();){
            auto& c=m.colors[i];if(c.image!=image){++i;continue;}
            dropFramebuffersOf(m,c.view,retired);dropSetsOf(m,c.view,retired);
            Dead x{};x.frame=c.lastUse;x.view=c.view;bury(m,x,retired);
            m.colors.erase(m.colors.begin()+std::ptrdiff_t(i));m.counters.imageDrops.fetch_add(1,std::memory_order_relaxed);
        }
        for(std::size_t i=0;i<m.depths.size();){
            auto& e=m.depths[i];if(e.image!=image){++i;continue;}
            dropSetsOf(m,e.view,retired);
            Dead x{};x.frame=e.lastUse;x.view=e.view;bury(m,x,retired);
            m.depths.erase(m.depths.begin()+std::ptrdiff_t(i));m.counters.imageDrops.fetch_add(1,std::memory_order_relaxed);
        }
        releaseDeferred(m,retired);
    }catch(...){}
}
void shutdownAfterIdle(VkDevice device) noexcept {
    Errno preserve;auto& m=module();
    if(!m.configured.load(std::memory_order_acquire)||device!=m.device)return;
    eyeHook.store(nullptr,std::memory_order_release);
    jitter::published.enabled.store(0,std::memory_order_relaxed); // Never jitter without the TAA pass
    m.stop.store(true,std::memory_order_release);m.workCv.notify_all();
    try{if(m.worker.joinable())m.worker.join();}catch(...){}
    try{
        std::lock_guard lock(m.cacheMutex);const VkDevice d=m.device;
        for(auto& f:m.framebuffers)vkDestroyFramebuffer(d,f.framebuffer,nullptr);
        for(auto& c:m.colors)vkDestroyImageView(d,c.view,nullptr);
        for(auto& e:m.depths)vkDestroyImageView(d,e.view,nullptr);
        for(auto& x:m.dead){Dead y=x;y.set={};destroyDead(d,m,y);} // sets go with the pool
        for(auto& h:m.history)if(h.exists())for(unsigned i=0;i<2;++i){
            vkDestroyFramebuffer(d,h.framebuffer[i],nullptr);
            for(auto* o:{&h.color[i],&h.aux[i]})if(o->image){vkDestroyImageView(d,o->view,nullptr);vkDestroyImage(d,o->image,nullptr);vkFreeMemory(d,o->memory,nullptr);}
        }
        for(auto& o:m.copy)if(o.image){vkDestroyImageView(d,o.view,nullptr);vkDestroyImage(d,o.image,nullptr);vkFreeMemory(d,o.memory,nullptr);}
        m.framebuffers.clear();m.colors.clear();m.depths.clear();m.engineSets.clear();m.dead.clear();m.history={};m.copy={};m.copyUnavailable=false;
        m.views={};m.toggled={};
        for(auto& s:m.slots){destroySlot(d,s);s.status.store(0,std::memory_order_release);s.color={};}
        destroyBase(d,m.base);m.baseReady.store(false,std::memory_order_release);
        m.formatCache={};m.querySlotPending={};m.querySlotFrame={};m.frameQuerySlot=~0u;m.timingFrame=~0ull;
        m.counters.historyBytes.store(0,std::memory_order_relaxed);
    }catch(...){}
    m.physical={};m.device={};m.queueMutex=nullptr;
    m.appswFrame=~0ull;m.appswNextNs=0;m.appswMode=-1;m.counters.appswActive.store(false,std::memory_order_relaxed);m.counters.effective.store(-1,std::memory_order_relaxed);
    setState(State::Off,"SHUTDOWN");m.configured.store(false,std::memory_order_release);
    runtime::taaMenuState.store(-1,std::memory_order_relaxed);
}
bool waitIdleForTests(unsigned milliseconds) noexcept {
    auto& m=module();const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
    while(std::chrono::steady_clock::now()<end){
        const auto st=State(m.state.load(std::memory_order_acquire));
        if(st==State::Failed)return true;
        if(m.baseReady.load(std::memory_order_acquire)&&!anyRequested(m))return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
bool recordEye(const hands::EyeCall& call) noexcept {
    Errno preserve;auto& m=module();auto& k=m.counters;
    k.calls.fetch_add(1,std::memory_order_relaxed);
    bool cacheLocked=false;
    const auto drainIdle=[&]{
        if(cacheLocked||call.eye!=0||!call.device||call.device!=m.device||!m.configured.load(std::memory_order_acquire))return;
        std::unique_lock idle(m.cacheMutex,std::try_to_lock);if(idle.owns_lock()&&!m.dead.empty())releaseDeferred(m,call.retiredFrame);
    };
    // Any skipped eye stops the TAA FULL jitter from the next scene on (an un-resolved jittered eye would shake).
    const auto skip=[&](Skip why){k.skips[why].fetch_add(1,std::memory_order_relaxed);jitter::published.enabled.store(0,std::memory_order_relaxed);
        try{drainIdle();}catch(...){}return false;};
    try{
        if(call.device&&call.device==m.device){
            auto seen=m.retired.load(std::memory_order_relaxed);
            while(call.retiredFrame>seen&&!m.retired.compare_exchange_weak(seen,call.retiredFrame,std::memory_order_release,std::memory_order_relaxed)){}
        }
        if(call.eye==0)pollLiveProperties(m);
        const int requested=requestedMode();k.requested.store(requested,std::memory_order_relaxed);
        const int mode=frameMode(m,call,requested); // AppSW TAA -> TAA LITE (session-only); a change resets below (resize)
        if(!taaMode(mode)){
            jitter::published.enabled.store(0,std::memory_order_relaxed);
            // TAA off: the next TAA frame starts from scratch (Reset::Toggle); the history memory goes after a while.
            for(unsigned e=0;e<2;++e)if(m.views[e].valid){m.views[e].valid=false;m.toggled[e]=true;}
            if(call.eye==0&&call.device==m.device&&++m.idleCalls>idleReleaseCalls&&(m.history[0].exists()||m.history[1].exists()||m.copy[0].image||m.copy[1].image)){
                std::unique_lock lock(m.cacheMutex,std::try_to_lock);
                if(lock.owns_lock()){for(auto& h:m.history)releaseHistory(m,h,call.retiredFrame);
                    for(unsigned e=0;e<2;++e){dropSetsOf(m,m.copy[e].view,call.retiredFrame);releaseOwn(m,m.copy[e],m.copy[e].lastUse,call.retiredFrame);}}
            }
            return skip(SkipDisabled);
        }
        m.idleCalls=0;
        if(!m.baseReady.load(std::memory_order_acquire)||m.state.load(std::memory_order_acquire)!=int(State::Ready))return skip(SkipBuilding);
        if(!call.device||call.device!=m.device||call.family!=m.family||!call.command||call.eye>1)return skip(SkipDevice);
        if(!call.plan||!(call.plan->unitsPerMetre>0.f)||!std::isfinite(call.plan->unitsPerMetre)||!call.renderFrame)return skip(SkipPlan);
        const auto& color=call.color;
        if(!color.image||!color.extent.width||!color.extent.height||color.samples!=VK_SAMPLE_COUNT_1_BIT||!color.layers||!color.mips||
           !(color.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)||!(color.usage&VK_IMAGE_USAGE_SAMPLED_BIT)||!colorLayoutOk(color.layout))return skip(SkipColor);
        const int domain=domainFor(color.format);
        if(domain<0)return skip(SkipFormat);
        const auto& depth=call.depth;
        const bool depthUsable=depth.image&&depthFormat(depth.format)&&depth.samples==VK_SAMPLE_COUNT_1_BIT&&depthLayoutOk(depth.layout)&&
            depth.extent.width==color.extent.width&&depth.extent.height==color.extent.height;
        const auto depthFeatures=depthUsable?formatFeatures(m,depth.format):0;
        const bool canSample=depthUsable&&(depth.usage&VK_IMAGE_USAGE_SAMPLED_BIT)&&(depthFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        const bool canCopy=depthUsable&&(depth.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)&&!m.copyUnavailable&&
            (depthFeatures&(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))==
            (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
        const DepthMode depthMode=canSample?DepthMode::Sampled:canCopy?DepthMode::Copy:DepthMode::None;
        const unsigned eye=call.eye;
        if(depthMode==DepthMode::None){k.lastMode[eye].store(unsigned(DepthMode::None),std::memory_order_relaxed);return skip(SkipDepth);}
        const auto ready=m.restoreReady.load(std::memory_order_acquire);const auto restore=m.restore.load(std::memory_order_acquire);
        if(!ready||!restore||!ready(call.command))return skip(SkipRestore);
        bool failed=false;ApplySlot* slot=applySlot(m,color.format,failed);
        if(!slot&&failed){setState(State::Failed,"APPLY_PIPELINE");return skip(SkipBuilding);}
        if(!slot)return skip(SkipBuilding);
        // ---- per-eye view and reset rules ----
        const Tuning t=tuningSnapshot(m);
        const auto& plan=*call.plan;
        // plan.projection is the UN-jittered projection (EyeViewports keeps the jitter in the clones only): the reprojection and
        // the history stay un-jittered; the jitter this eye was drawn with comes from the issuer (TaaJitter.h).
        const DMat4 world=hands::widen(plan.world[eye]),clip=eyeClip(plan.projection[eye]);
        EyeView& view=m.views[eye];
        Motion motion{};
        const bool full=fullMode(mode),lite=liteMode(mode);
        if(lite&&!slot->litePipeline)return skip(SkipFormat); // The eye format cannot be a filtered LITE history
        // fp16 LITE resolve (debug.rdrvr.vr.taa.lite.fp16 1): the first request queues its pipeline on the worker; until it is
        // ready (or if it failed) the fp32 pipeline runs. Both write the same history format: a switch needs no reset.
        bool half=false;VkPipeline litePipeline=slot->litePipeline;
        if(lite&&t.liteFp16){
            const int hs=slot->halfStatus.load(std::memory_order_acquire);
            if(hs==0){slot->halfStatus.store(1,std::memory_order_release);m.workCv.notify_one();}
            else if(hs==2&&slot->litePipelineHalf){half=true;litePipeline=slot->litePipelineHalf;}
        }
        jitter::published.extent[eye].store(jitter::packExtent(color.extent.width,color.extent.height),std::memory_order_relaxed);
        jitter::Sample drawn{};jitter::issuer.find(plan.sceneId,drawn);
        if(drawn.on&&drawn.extent[eye]!=jitter::packExtent(color.extent.width,color.extent.height)){drawn.on=false;drawn.px={};} // NDC offset for another size
        const std::uint32_t jitterPacked=drawn.on?packJitter(drawn.px[0],drawn.px[1]):0u;
        // TAA FULL resolves the whole eye (an un-resolved border would keep the jitter): debug.rdrvr.vr.taa.area applies to TAA only.
        // TAA LITE has its own centred area (debug.rdrvr.vr.taa.lite.area, default 70 % of width and height = ~50 % of the pixels).
        const std::uint32_t margins=full?0u:lite?liteMargins(color.extent.width,color.extent.height,t.liteArea):areaMargins(color.extent.width,color.extent.height,t.area);
        Reset reason=resetReason(view,call.renderFrame,world,color.extent.width,color.extent.height,std::int32_t(color.format),t,plan.unitsPerMetre,&motion,margins);
        if(reason==Reset::First&&m.toggled[eye])reason=Reset::Toggle;
        // ---- caches (try_lock: never wait on the render thread; imageDestroyed holds this mutex across driver destroys) ----
        std::unique_lock lock(m.cacheMutex,std::try_to_lock);
        if(!lock.owns_lock())return skip(SkipCache);
        cacheLocked=true;
        if(eye==0||m.dead.size()>16)releaseDeferred(m,call.retiredFrame);
        History& h=m.history[eye];
        bool recreated=false;
        if(!ensureHistory(m,h,color.extent,call.renderFrame,call.retiredFrame,recreated,lite,color.format,slot->litePass)){view.valid=false;return skip(SkipMemory);}
        if(recreated&&reason==Reset::None)reason=Reset::Resize;
        OwnImage& copy=m.copy[eye];
        if(depthMode==DepthMode::Copy&&!ensureCopy(m,copy,depth.format,color.extent,call.renderFrame,call.retiredFrame)){m.copyUnavailable=true;return skip(SkipMemory);}
        const VkImageView colorAttachment=colorView(m,color,call.renderFrame);
        if(!colorAttachment)return skip(SkipCache);
        const VkImageView depthSampled=depthMode==DepthMode::Sampled?depthView(m,depth,call.renderFrame):copy.view;
        if(!depthSampled)return skip(SkipCache);
        const VkDescriptorSet set=engineSet(m,colorAttachment,depthSampled,call.renderFrame);
        if(!set)return skip(SkipCache);
        // TAA FULL and TAA LITE write every pixel of their render area -> the "copy" apply (no blend, eye DONT_CARE)
        const bool copyApply=full||lite;
        const VkRenderPass applyPass=copyApply?slot->copyPass:slot->pass;const VkPipeline applyPipeline=copyApply?slot->copyPipeline:slot->pipeline;
        const VkFramebuffer applyFb=applyFramebuffer(m,applyPass,colorAttachment,color.extent,call.renderFrame);
        if(!applyFb)return skip(SkipCache);
        Push push{};
        // Sharpen in TAA FULL (debug bit 3: plain TAA too).
        const std::uint32_t sharpenFlag=!lite&&(full||(t.debug&debugSharpenTaa))&&t.sharpen>0?flagSharpen:0;
        const auto flagsFor=[&](bool reset){
            if(lite)return (reset?flagReset:0)|(domain==1?flagUnorm:0)|flagLite|(t.liteCubic?flagLiteCubic:0)|(t.liteDepth?0:flagLiteNoDepth);
            return (reset?flagReset:0)|((t.debug&debugPure)?flagPure:0)|(domain==1?flagUnorm:0)|sharpenFlag|(t.taps==9?flagTaps9:0)|(full?flagFull:0);};
        if(!buildPush(reason==Reset::None?&view:nullptr,world,clip,plan.unitsPerMetre,color.extent.width,color.extent.height,flagsFor(reason!=Reset::None),t,push,margins,jitterPacked)){
            reason=Reset::Matrix;
            if(!buildPush(nullptr,world,clip,plan.unitsPerMetre,color.extent.width,color.extent.height,flagsFor(true),t,push,margins,jitterPacked)){view.valid=false;return skip(SkipPlan);}
        }
        if(lite)push.range[1]=t.liteSubpx; // LITE has no depth test; range.y = the sub-pixel weight k
        lock.unlock();cacheLocked=false;
        const std::uint32_t in=reason==Reset::None?view.parity:(view.valid?view.parity:1u),out=in^1u;
        // TAA FULL: the apply pass always runs (a reset eye still has to be un-jittered).
        // TAA LITE like TAA: a reset eye has b = 0 everywhere = the eye itself, nothing to apply.
        const bool apply=full||reason==Reset::None||(t.debug&debugApplyOnReset)||(!lite&&(t.debug&debugPure));
        // ---- recording (no failure past this point) ----
        const VkCommandBuffer c=call.command;const auto& b=m.base;
        bool timed=false;unsigned qslot=0;
        if(b.queries&&m.gpuTiming.load(std::memory_order_relaxed)){
            if(m.timingFrame!=call.renderFrame){m.timingFrame=call.renderFrame;m.frameQuerySlot=~0u;readTimestamps(m,call.renderFrame);
                if(call.renderFrame%16==0){qslot=unsigned(m.timedSequence%4);
                    if(!m.querySlotPending[qslot]){m.frameQuerySlot=qslot;++m.timedSequence;m.querySlotPending[qslot]=true;m.querySlotFrame[qslot]=call.renderFrame;
                        m.querySlotHalf[qslot]=-1;
                        vkCmdResetQueryPool(c,b.queries,qslot*4,4);}}}
            if(m.frameQuerySlot!=~0u){timed=true;qslot=m.frameQuerySlot;}
        }
        if(timed){m.querySlotMode[qslot]=mode;
            auto& qh=m.querySlotHalf[qslot];qh=qh<0||qh==int(half)?int(half):2;}
        // The image barriers are batched (3 vkCmdPipelineBarrier calls per eye with SAMPLED depth; same scopes).
        Barriers bar{};
        const auto aspects=depthAspects(depth.format);
        bar.add(color.image,VK_IMAGE_ASPECT_COLOR_BIT,color.layout,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,anyAccess,VK_ACCESS_SHADER_READ_BIT);
        if(depthMode==DepthMode::Sampled)
            bar.add(depth.image,aspects,depth.layout,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,anyAccess,VK_ACCESS_SHADER_READ_BIT);
        else{
            bar.add(depth.image,aspects,depth.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,anyAccess,VK_ACCESS_TRANSFER_READ_BIT);
            bar.add(copy.image,aspects,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,anyAccess,VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        bar.flush(c);
        // begin timestamp after the first full barrier: the tail of the eye's own colour pass is not counted as TAA
        if(timed)vkCmdWriteTimestamp(c,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,b.queries,qslot*4+2*eye);
        if(depthMode==DepthMode::Copy){
            VkImageCopy region{};region.srcSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};region.dstSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
            region.extent={color.extent.width,color.extent.height,1};
            vkCmdCopyImage(c,depth.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,copy.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            bar.add(copy.image,aspects,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
            bar.add(depth.image,aspects,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,depth.layout,VK_ACCESS_TRANSFER_READ_BIT,anyAccess);
        }
        if(!h.initialized){ // the never-written slot is bound (not read: reset) and must be in its descriptor layout
            for(unsigned i=0;i<2;++i)for(const auto* o:{&h.color[i],&h.aux[i]})
                bar.add(o->image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,0,VK_ACCESS_SHADER_READ_BIT); // LITE: no aux (skipped)
            h.initialized=true;
        }
        bar.flush(c);
        const VkViewport viewport{0,0,float(color.extent.width),float(color.extent.height),0,1};
        const std::uint32_t mx=margins&0xFFFFu,my=margins>>16;
        const VkRect2D area{{std::int32_t(mx),std::int32_t(my)},{color.extent.width-2*mx,color.extent.height-2*my}}; // whole eye unless an area lever
        { // Pass R: resolve into history slot `out` (TAA LITE: taa_lite.frag into the eye-format history)
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=lite?slot->litePass:b.resolvePass;begin.framebuffer=h.framebuffer[out];begin.renderArea=area;
            vkCmdBeginRenderPass(c,&begin,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(c,0,1,&viewport);vkCmdSetScissor(c,0,1,&area);
            vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,lite?litePipeline:b.resolvePipeline); // litePipeline = fp32 or fp16
            const VkDescriptorSet sets[2]{set,h.set[in]};
            vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,b.resolveLayout,0,2,sets,0,nullptr);
            vkCmdPushConstants(c,b.resolveLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
            vkCmdDraw(c,3,1,0,0);
            vkCmdEndRenderPass(c);
        }
        if(depthMode==DepthMode::Sampled)
            bar.add(depth.image,aspects,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,depth.layout,VK_ACCESS_SHADER_READ_BIT,anyAccess);
        if(apply){ // pass A: TAA: eye = mix(eye, TAA, b), b = 0 discarded; TAA FULL / TAA LITE: plain write of the area (copy pipeline)
            bar.add(color.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            bar.flush(c);
            VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=applyPass;begin.framebuffer=applyFb;begin.renderArea=area;
            vkCmdBeginRenderPass(c,&begin,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(c,0,1,&viewport);vkCmdSetScissor(c,0,1,&area);
            vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,applyPipeline);
            vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,b.applyLayout,0,1,&h.set[out],0,nullptr);
            vkCmdPushConstants(c,b.applyLayout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
            vkCmdDraw(c,3,1,0,0);
            vkCmdEndRenderPass(c);
            bar.add(color.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,color.layout,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,anyAccess);
        }else bar.add(color.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,color.layout,VK_ACCESS_SHADER_READ_BIT,anyAccess);
        bar.flush(c);
        if(timed)vkCmdWriteTimestamp(c,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,b.queries,qslot*4+2*eye+1);
        restore(c);k.restoreCalls.fetch_add(1,std::memory_order_relaxed);
        // ---- bookkeeping ----
        view.valid=true;view.frame=call.renderFrame;view.world=world;view.clip=clip;view.width=color.extent.width;view.height=color.extent.height;
        view.format=std::int32_t(color.format);view.parity=out;view.margins=margins;m.toggled[eye]=false;
        k.eyes.fetch_add(1,std::memory_order_relaxed);k.eyesPerEye[eye].fetch_add(1,std::memory_order_relaxed);
        if(eye==0)k.frames.fetch_add(1,std::memory_order_relaxed);
        if(apply)k.applied.fetch_add(1,std::memory_order_relaxed);
        k.resets[unsigned(reason)].fetch_add(1,std::memory_order_relaxed);k.lastReset[eye].store(unsigned(reason),std::memory_order_relaxed);
        const bool valid=reason==Reset::None;
        if(valid)k.validEyes.fetch_add(1,std::memory_order_relaxed);
        k.recentValid.store((k.recentValid.load(std::memory_order_relaxed)<<1)|(valid?1u:0u),std::memory_order_relaxed);
        k.recentWindow.store(std::min<std::uint64_t>(k.recentWindow.load(std::memory_order_relaxed)+1,64),std::memory_order_relaxed);
        k.modes[unsigned(depthMode)].fetch_add(1,std::memory_order_relaxed);k.lastMode[eye].store(unsigned(depthMode),std::memory_order_relaxed);
        k.lastMotionMm.store(std::uint64_t(std::min(motion.metres*1000.0,1e12)),std::memory_order_relaxed);
        k.lastMotionMilliDeg.store(std::uint64_t(std::min(motion.degrees*1000.0,1e12)),std::memory_order_relaxed);
        // TAA FULL bookkeeping + the jitter gate (both eyes of this render frame resolved -> jitter the next scenes)
        if(sharpenFlag)k.sharpenEyes.fetch_add(1,std::memory_order_relaxed);
        if(lite)k.liteEyes.fetch_add(1,std::memory_order_relaxed);
        if(half)k.liteHalfEyes.fetch_add(1,std::memory_order_relaxed);else if(lite&&t.liteFp16)k.liteHalfFallback.fetch_add(1,std::memory_order_relaxed);
        k.lastJitterOn[eye].store(drawn.on,std::memory_order_relaxed);k.lastJitterIndex[eye].store(drawn.index,std::memory_order_relaxed);
        k.lastJitterPacked[eye].store(jitterPacked,std::memory_order_relaxed);k.lastJitterScene.store(plan.sceneId,std::memory_order_relaxed);
        if(drawn.on)k.jitteredEyes.fetch_add(1,std::memory_order_relaxed);
        if(full){
            k.fullEyes.fetch_add(1,std::memory_order_relaxed);if(!drawn.on)k.unjitteredFullEyes.fetch_add(1,std::memory_order_relaxed);
            m.fullOkFrame[eye]=call.renderFrame;
            publishJitter(t,m.fullOkFrame[0]==call.renderFrame&&m.fullOkFrame[1]==call.renderFrame);
        }else publishJitter(t,false);
        recordedEyeKey.store((call.renderFrame<<1)|eye,std::memory_order_relaxed); // ARC: gpu.split "taa" bucket of the hands pass
        return true;
    }catch(...){
        setState(State::Failed,"RECORD_EXCEPTION");return false;
    }
}

std::string report() {
    auto& m=module();auto& k=m.counters;std::ostringstream o;
    const bool baseReady=m.baseReady.load(std::memory_order_acquire);
    const auto load=[](const std::atomic<std::uint64_t>& a){return a.load(std::memory_order_relaxed);};
    const int requested=k.requested.load(std::memory_order_relaxed);
    const int mode=k.effective.load(std::memory_order_relaxed); // active / full / lite / sharpen describe the mode that runs
    Tuning t{};{std::lock_guard lock(m.tuningMutex);t=m.tuning;}
    const auto eyes=load(k.eyes),window=load(k.recentWindow);
    std::uint64_t recentBits=load(k.recentValid);if(window<64)recentBits&=(1ull<<window)-1;
    unsigned recent=0;for(std::uint64_t v=recentBits;v;v&=v-1)++recent;
    o<<"{\"state\":\""<<stateName(state())<<"\",\"reason\":\""<<m.reason.load(std::memory_order_relaxed)<<"\""
     <<",\"requested\":"<<requested<<",\"mode\":\""<<modeName(requested)<<"\"" // The requested row (menu / debug.rdrvr.aa)
     // The row the pass runs for (AppSW TAA -> TAA LITE override: debug.rdrvr.vr.taa.appswlite, latch re-read <= 1 Hz)
     <<",\"effectiveMode\":"<<mode<<",\"effective\":\""<<modeName(mode)<<"\",\"appswLite\":{\"enabled\":"<<k.appswEnabled.load(std::memory_order_relaxed)
     <<",\"probe\":"<<(m.synthesizingProbe.load(std::memory_order_relaxed)!=nullptr?1:0)<<",\"synthesizing\":"<<(k.appswSynthesizing.load(std::memory_order_relaxed)?1:0)
     <<",\"overrideActive\":"<<(k.appswActive.load(std::memory_order_relaxed)?1:0)<<",\"overrideCount\":"<<load(k.appswOverrides)
     <<",\"overrideFrames\":"<<load(k.appswFrames)<<"}"
     <<",\"active\":"<<(taaMode(mode)?1:0)<<",\"full\":"<<(fullMode(mode)?1:0)
     <<",\"lite\":"<<(liteMode(mode)?1:0)<<",\"liteEyes\":"<<load(k.liteEyes)
     // fp16 LITE resolve: property, eye runs on the fp16 pipeline / requested but fp32 (building, failed), GPU EMA per variant
     // (keys unlike "eyes" / "timed" / "avgUs": the host test reads the first occurrence of those)
     <<",\"liteFp16\":{\"on\":"<<(t.liteFp16?1:0)<<",\"halfEyes\":"<<load(k.liteHalfEyes)<<",\"halfFallback\":"<<load(k.liteHalfFallback)
     <<",\"gpuUsFp32Fp16\":["<<double(load(k.gpuLiteAvg[0]))/1000.0<<','<<double(load(k.gpuLiteAvg[1]))/1000.0<<"]"
     <<",\"gpuSamplesFp32Fp16\":["<<load(k.gpuLiteTimed[0])<<','<<load(k.gpuLiteTimed[1])<<"]}"
     <<",\"sharpen\":"<<((fullMode(mode)||(taaMode(mode)&&((m.testOverride?t.debug:m.liveDebug.load(std::memory_order_relaxed))&debugSharpenTaa)))&&t.sharpen>0?1:0)
     <<",\"sharpenEyes\":"<<load(k.sharpenEyes)
     <<",\"postFxaa\":"<<runtime::taaPostFxaa.load(std::memory_order_relaxed)<<",\"menuState\":"<<runtime::taaMenuState.load(std::memory_order_relaxed)
     <<",\"hook\":"<<(eyeHook.load(std::memory_order_relaxed)!=nullptr)
     <<",\"restoreHooks\":"<<(m.restoreReady.load(std::memory_order_relaxed)&&m.restore.load(std::memory_order_relaxed))
     <<",\"debug\":"<<(m.testOverride?t.debug:m.liveDebug.load(std::memory_order_relaxed))
     <<",\"tuning\":{\"clip\":"<<t.clipGamma<<",\"nMax\":["<<t.nMaxStill<<','<<t.nMaxMoving<<"],\"motionPx\":"<<t.motionPx<<",\"nearM\":"<<t.nearMetres
     <<",\"depthTestM\":"<<t.depthTestMetres<<",\"sharpenAmount\":"<<t.sharpen<<",\"cutM\":"<<t.cutMetres<<",\"cutDeg\":"<<t.cutDegrees<<",\"maxGap\":"<<t.maxGap
     <<",\"taps\":"<<t.taps<<",\"area\":"<<t.area<<",\"jitterAmplitude\":"<<t.jitterAmplitude<<",\"jitterLength\":"<<t.jitterLength
     <<",\"liteArea\":"<<t.liteArea<<",\"liteHistory\":\""<<(t.liteCubic?"catmull-rom":"bilinear")<<"\",\"liteDepth\":"<<(t.liteDepth?1:0)<<",\"liteSubpx\":"<<t.liteSubpx<<"}"
     <<",\"calls\":"<<load(k.calls)<<",\"frames\":"<<load(k.frames)<<",\"eyes\":"<<eyes<<",\"eyesPerEye\":["<<load(k.eyesPerEye[0])<<','<<load(k.eyesPerEye[1])<<"]"
     <<",\"applied\":"<<load(k.applied);
    { // TAA FULL: jitter gate and the offsets the last eyes were resolved with (px = framebuffer pixels, y down)
        const auto px=[&](unsigned e,int axis){const auto v=k.lastJitterPacked[e].load(std::memory_order_relaxed);
            return double(std::int16_t(std::uint16_t(axis?v>>16:v&0xFFFFu)))/8192.0;};
        const auto last=jitter::issuer.last();
        o<<",\"jitter\":{\"enabled\":"<<jitter::published.enabled.load(std::memory_order_relaxed)
         <<",\"amplitudeMilli\":"<<jitter::published.amplitudeMilli.load(std::memory_order_relaxed)<<",\"length\":"<<jitter::published.length.load(std::memory_order_relaxed)
         <<",\"sequence\":\"halton23\",\"eyes\":\"same px both eyes\""
         <<",\"issued\":"<<jitter::issuer.issued()<<",\"jitteredScenes\":"<<jitter::issuer.jittered()<<",\"reused\":"<<jitter::issuer.reused()
         <<",\"found\":"<<jitter::issuer.found()<<",\"missing\":"<<jitter::issuer.missing()
         <<",\"lastIssued\":{\"scene\":"<<last.scene<<",\"on\":"<<(last.on?1:0)<<",\"index\":"<<last.index<<",\"px\":["<<last.px[0]<<','<<last.px[1]<<"]}"
         <<",\"lastScene\":"<<load(k.lastJitterScene)
         <<",\"eyeOn\":["<<(k.lastJitterOn[0].load(std::memory_order_relaxed)?1:0)<<','<<(k.lastJitterOn[1].load(std::memory_order_relaxed)?1:0)<<"]"
         <<",\"eyeIndex\":["<<k.lastJitterIndex[0].load(std::memory_order_relaxed)<<','<<k.lastJitterIndex[1].load(std::memory_order_relaxed)<<"]"
         <<",\"eyePx\":[["<<px(0,0)<<','<<px(0,1)<<"],["<<px(1,0)<<','<<px(1,1)<<"]]"
         <<",\"fullEyes\":"<<load(k.fullEyes)<<",\"jitteredEyes\":"<<load(k.jitteredEyes)<<",\"unjitteredFullEyes\":"<<load(k.unjitteredFullEyes)<<"}";
    }
    o
     <<",\"historyValidPercent\":"<<(eyes?100.0*double(load(k.validEyes))/double(eyes):0.0)
     <<",\"recentValidPercent\":"<<(window?100.0*double(recent)/double(window):0.0)
     <<",\"resets\":{";
    for(unsigned i=0;i<unsigned(Reset::Count);++i)o<<(i?",":"")<<'"'<<resetNames[i]<<"\":"<<load(k.resets[i]);
    o<<"},\"lastReset\":[\""<<resetNames[std::min(k.lastReset[0].load(std::memory_order_relaxed),unsigned(Reset::Count)-1)]<<"\",\""
     <<resetNames[std::min(k.lastReset[1].load(std::memory_order_relaxed),unsigned(Reset::Count)-1)]<<"\"]"
     <<",\"lastMotion\":{\"mm\":"<<load(k.lastMotionMm)<<",\"milliDeg\":"<<load(k.lastMotionMilliDeg)<<"}"
     <<",\"skips\":{";
    for(unsigned i=0;i<SkipCount;++i)o<<(i?",":"")<<'"'<<skipNames[i]<<"\":"<<load(k.skips[i]);
    o<<"},\"depthMode\":[\""<<depthName(DepthMode(k.lastMode[0].load(std::memory_order_relaxed)))<<"\",\""<<depthName(DepthMode(k.lastMode[1].load(std::memory_order_relaxed)))<<"\"]"
     <<",\"modes\":{\"none\":"<<load(k.modes[0])<<",\"sampled\":"<<load(k.modes[1])<<",\"copy\":"<<load(k.modes[2])<<"}"
     <<",\"gpu\":{\"timed\":"<<load(k.timed)<<",\"lastUs\":"<<double(load(k.gpuLastNs))/1000.0<<",\"avgUs\":"<<double(load(k.gpuAvgNs))/1000.0
     <<",\"unavailable\":"<<load(k.gpuUnavailable)<<",\"pool\":"<<(baseReady&&m.base.queries!=VK_NULL_HANDLE)<<",\"scope\":\"both eyes, resolve+apply(+depth copy)\""
     // avgUs restarts when the row changes; mode = the row of avgUs/lastUs; byMode = running average per row
     <<",\"mode\":\""<<modeName(k.gpuLastMode.load(std::memory_order_relaxed))<<"\",\"byMode\":{";
    for(int r:{runtime::antiAliasingRowTaaLite,runtime::antiAliasingRowTaa,runtime::antiAliasingRowTaaFull})
        o<<(r==runtime::antiAliasingRowTaaLite?"":",")<<'"'<<modeName(r)<<"\":{\"avgUs\":"<<double(load(k.gpuAvgByMode[unsigned(r)]))/1000.0
         <<",\"timed\":"<<load(k.gpuTimedByMode[unsigned(r)])<<"}";
    o<<"}}"
     <<",\"memoryMB\":"<<double(load(k.historyBytes))/1048576.0 // history (2 slots x RGBA16F + R16F per eye; TAA LITE: 2 x eye format) + COPY depths
     <<",\"objects\":{\"views\":"<<load(k.views)<<",\"framebuffers\":"<<load(k.framebuffers)<<",\"engineSets\":"<<load(k.sets)
     <<",\"histories\":"<<load(k.histories)<<",\"historyReleases\":"<<load(k.historyReleases)<<",\"copyDepth\":"<<load(k.copyImages)
     <<",\"imageDrops\":"<<load(k.imageDrops)<<",\"deferred\":"<<load(k.graveyard)<<",\"deferredReleased\":"<<load(k.graveyardReleased)<<"}"
     <<",\"restores\":"<<load(k.restoreCalls)
     <<",\"buildMs\":"<<(baseReady?double(m.base.buildNs)/1e6:0.0)<<",\"pipelines\":[";
    bool first=true;
    for(auto& s:m.slots){const int st=s.status.load(std::memory_order_acquire);if(!st)continue;
        o<<(first?"":",")<<"{\"color\":"<<int(s.color)<<",\"status\":"<<st<<",\"buildMs\":"<<(st==2?double(s.buildNs)/1e6:0.0)
         <<",\"halfPipeline\":"<<s.halfStatus.load(std::memory_order_acquire)<<"}";first=false;} // fp16 LITE pipeline: 0 none, 1 requested, 2 ready, 3 failed
    o<<"]}";
    return o.str();
}
} // namespace rdrvr::vr::taa
