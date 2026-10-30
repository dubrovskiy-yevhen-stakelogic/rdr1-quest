// Render-thread sniper scope lens. See ScopePass.h for the design and the threading contract.
//
// Per render frame (first eye call): the scope of the plan's VR weapon frame in WORLD units - eyepiece centre on the scope axis,
// the axis (= barrel direction), right / up of the rifle, disc radius, tan of the content half-angle - and the SOURCE eye
// (the one farther from the axis whose image contains the barrel's vanishing point).
// Per eye (EngineStereoPass, engine frame command buffer, outside any engine render pass, after TAA, before hands + capture):
//   1. every fallible step first (frame, pipelines, lens image, view, framebuffer); any miss = nothing recorded for this eye,
//   2. source eye: colour -> TRANSFER_SRC, the square around the vanishing point copied into the lens image (eye format,
//      256 x 256), lens -> SHADER_READ_ONLY; NEAR MASK (lever .near > 0, eye depth known and copyable): the eye depth
//      (both aspects) -> TRANSFER_SRC, the same square's DEPTH aspect copied into the lens depth image (engine depth format,
//      UNDEFINED -> TRANSFER_DST -> DEPTH_STENCIL_READ_ONLY), the eye depth back to its received layout,
//   3. colour -> COLOR_ATTACHMENT, one private render pass (LOAD/STORE, render area = the disc's pixel bounds): one quad whose
//      corners are the disc's bounding square in this eye's clip space; scope.frag maps each disc point to its world direction
//      and that direction into the copy (the source eye's world -> clip of the copy's frame); with the near mask one depth gather
//      per pixel fades a source pixel nearer than the near distance (the rifle's own parts) to the dark glass,
//   4. colour back to its received layout, the engine's bound state restored (EngineRestore).
#include "ScopePass.h"
#include "VrWeaponCalib.h" // SCOPE LENS per sniper model (WEAPON CALIBRATION > SAVE FOR: SCOPE LENS)
#include "ScopeShaders.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>
#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::scope {
namespace {
using hands::DMat4;using hands::DVec3;
struct Errno {int value{errno};~Errno(){errno=value;}};
void logLine(const char* what,const char* detail) noexcept {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_WARN,"RdrVrScope","%s: %s",what,detail?detail:"");
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

// The copied square (px); the small lens copies only what its content needs (~120..240 px of a 3S eye), FULL ZOOM up to the
// whole square (~ +-11 deg of the source eye around the vanishing point = a 4x picture of +-45 deg). 512: the private colour + depth
// images (one each, built once) cost ~3 MB; the copy itself is never larger than the content.
constexpr std::uint32_t lensSize=512;
constexpr unsigned maxCacheEntries=16;
constexpr VkAccessFlags anyAccess=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
struct VkFailure {const char* what;};
void check(VkResult r,const char* what) {if(r!=VK_SUCCESS)throw VkFailure{what};}
bool colorLayoutOk(VkImageLayout l) noexcept {
    return l==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL||l==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL||l==VK_IMAGE_LAYOUT_GENERAL||l==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}
// NEAR MASK: the eye depth facts of HandsPass / TaaPass (formats, both aspects in a barrier of a depth/stencil image, the
// layouts the engine hands over).
bool hasStencil(VkFormat f) noexcept {return f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_D32_SFLOAT_S8_UINT||f==VK_FORMAT_D16_UNORM_S8_UINT||f==VK_FORMAT_S8_UINT;}
bool depthFormat(VkFormat f) noexcept {
    return f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_X8_D24_UNORM_PACK32||f==VK_FORMAT_D32_SFLOAT||f==VK_FORMAT_D32_SFLOAT_S8_UINT||
        f==VK_FORMAT_D16_UNORM||f==VK_FORMAT_D16_UNORM_S8_UINT;
}
VkImageAspectFlags depthAspects(VkFormat f) noexcept {return VK_IMAGE_ASPECT_DEPTH_BIT|(hasStencil(f)?VK_IMAGE_ASPECT_STENCIL_BIT:0);}
bool depthLayoutOk(VkImageLayout l) noexcept {
    return l==VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL||l==VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL||l==VK_IMAGE_LAYOUT_GENERAL||
        l==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL||l==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL||
        l==VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL||l==VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
}
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

struct Tuning {
    float zoom{4.f},fwdM{-.01f},upM{.02f},rightM{-.022f},radiusM{.012f},reliefM{.08f},bright{1.f};int reticle{1};bool show{true};
    float boxInM{.015f},boxOutM{.035f},boxBackM{.35f}; // EYE BOX (exit pupil): full picture within boxIn of the axis, dark past boxOut
    float nearM{1.5f}; // NEAR MASK: source pixels nearer than this (metres from the source eye) fade to dark glass; 0 = off
    // FULL ZOOM (menu SCOPE LENS > FULL ZOOM): the eye nearer to the scope axis gets the picture over its view while it is at most
    // fullBack behind the eyepiece (or fullFront in front of it: the face pushed into the rifle) and fullLat off the axis; both fade
    // out over fullFade; fullFov = the picture's half-angle (capped so the content fits the copied square)
    float fullBackM{.06f},fullFrontM{.04f},fullLatM{.025f},fullFadeM{.025f},fullFovDeg{50.f};
    bool fullOn{}; // the menu key, read every frame (recordEye)
};
void readTuning(Tuning& t) noexcept {
    t.zoom=float(std::clamp(propertyInt("debug.rdrvr.vr.scope.zoom",40),15,150))/10.f;
    t.reliefM=float(std::clamp(propertyInt("debug.rdrvr.vr.scope.relief",8),2,30))/100.f;
    t.reticle=std::clamp(propertyInt("debug.rdrvr.vr.scope.reticle",1),0,1);
    t.bright=float(std::clamp(propertyInt("debug.rdrvr.vr.scope.bright",100),10,300))/100.f;
    // debug.rdrvr.vr.scope.eyebox <in mm> (15; out = in + 20; 0 = off: every eye sees the picture)
    {const int in=std::clamp(propertyInt("debug.rdrvr.vr.scope.eyebox",0),0,200);t.boxInM=float(in)/1000.f;t.boxOutM=float(in+20)/1000.f;}
    t.show=propertyInt("debug.rdrvr.vr.scope.show",1)!=0;
    // debug.rdrvr.vr.scope.near <cm> (150; 0 = off: no depth copy, no mask)
    t.nearM=float(std::clamp(propertyInt("debug.rdrvr.vr.scope.near",150),0,5000))/100.f;
    // "fwdCm upCm radiusMm [rightCm]" from the rifle MODEL origin projected onto the bore: +fwd to the muzzle, up / right along the
    // muzzle locator +Y / +X (the same in either hand); default 6 5 18 0
    char text[96]{};float v[4]{-1.f,2.f,12.f,-2.2f}; // the live session's last values; the SCOPE LENS page fine-tunes per rifle
    if(propertyText("debug.rdrvr.vr.scope.lens",text,sizeof text)){ // garbage keeps the defaults
        float r[4]{};int n=0;const char* p=text;
        while(*p&&n<4){char* end{};const float x=std::strtof(p,&end);if(end==p||!std::isfinite(x))break;r[n++]=x;p=end;while(*p==' '||*p==',')++p;}
        if(n>=3&&!*p)for(int i=0;i<n;++i)v[i]=r[i];
    }
    t.fwdM=std::clamp(v[0],-60.f,60.f)/100.f;t.upM=std::clamp(v[1],-20.f,20.f)/100.f;t.radiusM=std::clamp(v[2],5.f,60.f)/1000.f;
    t.rightM=std::clamp(v[3],-20.f,20.f)/100.f;
    // debug.rdrvr.vr.scope.full "backCm latCm fadeCm fovDeg" (default "6 2.5 2.5 50"; garbage keeps the defaults)
    float u[4]{-1.f,3.f,2.5f,50.f}; // backCm -1 = the menu's FULL ZOOM DIST; lateral 3 cm
    if(propertyText("debug.rdrvr.vr.scope.full",text,sizeof text)){
        float r[4]{};int n=0;const char* p=text;
        while(*p&&n<4){char* end{};const float x=std::strtof(p,&end);if(end==p||!std::isfinite(x))break;r[n++]=x;p=end;while(*p==' '||*p==',')++p;}
        if(n>=1&&!*p)for(int i=0;i<n;++i)u[i]=r[i];
    }
    t.fullBackM=u[0]<0.f?-1.f:std::clamp(u[0],0.f,30.f)/100.f;t.fullLatM=std::clamp(u[1],0.f,10.f)/100.f;t.fullFadeM=std::clamp(u[2],.2f,10.f)/100.f;
    t.fullFovDeg=std::clamp(u[3],10.f,75.f);
}

// params: x flags (pushReticle | pushPicture | pushNearMask | pushFull), y brightness, z depth of the near distance, w rim start;
// up.w = depth of the near distance's soft end (scope.frag).
// pushFull: the quad is the disc's square scaled by params.w (disc coordinates up to +-w, the rim starts at a fixed 0.9), flag
// bits 4..11 = the surround's black alpha x 255 (outside the disc), bits 12..19 = the reticle's line width x 255 (scope.vert / .frag).
struct Push {float corner[16];float axis[4];float right[4];float up[4];float params[4];};
static_assert(sizeof(Push)==128);
constexpr unsigned pushReticle=1,pushPicture=2,pushNearMask=4,pushFull=8;
constexpr double nearSoftEnd=1.2; // the mask's soft edge: dark at the near distance, clear at 1.2 x


struct Slot {VkFormat format{};VkRenderPass pass{};VkPipeline pipeline{};};
struct ColorView {VkImage image{};std::uint64_t generation{};VkFormat format{};VkImageView view{};std::uint64_t lastUse{};};
struct Framebuffer {VkRenderPass pass{};VkImageView color{};VkFramebuffer framebuffer{};VkExtent2D extent{};std::uint64_t lastUse{};};
struct Dead {std::uint64_t frame{};VkFramebuffer framebuffer{};VkImageView view{};VkImage image{};VkDeviceMemory memory{};VkRenderPass pass{};VkPipeline pipeline{};
    VkDescriptorSet set{};};
// The lens copy and its own descriptor set (a format change makes a new pair; the old one is freed once its frames retired).
// set = {colour copy, colour copy (binding 1 bound, not read)}; depthSet = {colour copy, depth copy} once the source eye's depth
// could be copied (private image in the engine depth format, 256 x 256, DEPTH aspect view; replaced with the colour copy or on a
// depth format change).
struct Lens {VkImage image{};VkDeviceMemory memory{};VkImageView view{};VkFormat format{};VkDescriptorSet set{};bool initialized{};
    VkImage depthImage{};VkDeviceMemory depthMemory{};VkImageView depthView{};VkFormat depthFormat{};VkDescriptorSet depthSet{};};

// The scope of one render frame (WORLD units).
struct FrameScope {
    std::uint64_t renderFrame{~0ull};bool valid{};int source{-1};
    DVec3 centre{},axis{},right{},up{};double radius{},tanHalf{},upm{1};
    std::array<double,2> eyeAxisM{};
    std::array<double,2> eyeBackM{}; // how far each eye is behind the lens along the axis (metres; < 0 = in front)
    // FULL ZOOM: fullEye (-1 none) gets the picture blended by fullS (0..1) toward fullTanApp (tan of its half-angle) showing
    // fullTanContent (tan of the content half-angle); copyTan = the content half-angle the source eye's copy must hold
    int fullEye{-1};double fullS{},fullTanApp{},fullTanContent{},copyTan{};
};
// The last copy: source eye world -> clip of its frame, the copied pixel square.
// depth = the source eye's depth of the same square was copied too; depthDark / depthClear = the engine depth of the points at
// the near distance / its soft end along the scope axis from the source eye (that frame's worldClip).
struct Copy {bool valid{};std::uint64_t renderFrame{};unsigned eye{};DMat4 worldClip{};std::int32_t x{},y{};std::uint32_t w{},h{};VkExtent2D extent{};
    bool depth{};float depthDark{},depthClear{};};

enum Skip : unsigned {SkipKilled,SkipHidden,SkipNoFrame,SkipSpace,SkipDevice,SkipPlan,SkipColor,SkipRestore,SkipBuild,SkipCache,SkipOffscreen,SkipCount};
constexpr const char* skipNames[SkipCount]{"killed","hidden","noFrame","otherSpace","device","plan","color","restore","build","cache","offscreen"};
struct Counters {
    std::atomic<std::uint64_t> calls{},frames{},eyes{},copies{},copyNoSource{},copyNoTransfer{},previousCopy{},darkGlass{},published{},clamped{},modelAnchor{},gripAnchor{},eyeBoxDark{};
    std::array<std::atomic<std::uint64_t>,2> eyesPerEye{},sourceEye{};
    std::array<std::atomic<std::uint64_t>,SkipCount> skips{};
    std::atomic<std::uint32_t> lastRect{};std::atomic<std::uint32_t> lastAxisMm[2]{};std::atomic<int> lastSource{-1};
    std::atomic<std::uint64_t> views{},framebuffers{},imageDrops{},deferred{},deferredReleased{},restores{};
    // NEAR MASK: depthCopies = source copies with the depth square; depthUnavailable = source copies without it (lever on) by
    // reason: unknown (no eye depth), unusable (format / samples / layout / usage / extent), noImage (private depth image or set not
    // possible for that format), threshold (the near distance has no finite depth); nearMaskEyes = eye draws with the mask on.
    std::atomic<std::uint64_t> depthCopies{},depthUnavailable{},depthUnknown{},depthUnusable{},depthNoImage{},depthThreshold{},nearMaskEyes{},depthImages{};
    std::atomic<float> lastDepthDark{},lastDepthClear{};std::atomic<int> reversedZ{-1};std::atomic<int> lastDepthFormat{};
    // FULL ZOOM: frames with an eye in the full zone, eye draws with the full picture, frames whose picture was made smaller to
    // fit the copied square; the last blend (x 1000) and picture half-angle (deg x 10)
    std::atomic<std::uint64_t> fullFrames{},fullEyes{},fullCapped{};std::atomic<std::uint32_t> lastFullS{},lastFullFov{};std::atomic<int> lastFullEye{-1};
    // why a frame with the menu key on had no full eye - the near eye is the source
    // (wrongSource), outside the zone (outside), the content does not fit / project (noFit); the near eye's closest approach to the
    // axis while 0..30 cm behind the eyepiece (mm) with its distance behind (mm), and the last near eye lateral / behind (mm)
    std::atomic<std::uint64_t> fullWrongSource{},fullOutside{},fullNoFit{};
    std::atomic<std::uint32_t> fullBestLatMm{0xffffffffu};std::atomic<int> fullBestBackMm{},fullLastLatMm{-1},fullLastBackMm{};
};
struct Module {
    std::atomic<int> state{int(State::Off)};
    std::atomic<const char*> reason{"NOT_CONFIGURED"};
    std::atomic<bool (*)(VkCommandBuffer) noexcept> restoreReady{nullptr};
    std::atomic<void (*)(VkCommandBuffer) noexcept> restore{nullptr};
    std::atomic<bool> configured{false};
    VkPhysicalDevice physical{};VkDevice device{};std::uint32_t family{};
    hands::WeaponBoard frames; // main thread -> render thread, keyed by the plan's head sample
    mutable std::mutex tuningMutex;Tuning tuning{};std::uint64_t poll{};
    // render-thread Vulkan state (cacheMutex shared with imageDestroyed / shutdownAfterIdle)
    std::mutex cacheMutex;
    bool baseBuilt{};VkSampler sampler{};VkDescriptorSetLayout setLayout{};VkPipelineLayout layout{};VkDescriptorPool pool{};
    VkShaderModule vertex{},fragment{};
    std::array<Slot,2> slots{};Lens lens{};VkSampler nearest{};VkFormat depthFailed{VK_FORMAT_UNDEFINED};
    std::vector<ColorView> colors;std::vector<Framebuffer> framebuffers;std::vector<Dead> dead;
    std::atomic<std::uint64_t> retired{};
    FrameScope frame{};Copy copy{};
    Counters counters;
};
Module& module() noexcept {static Module* instance=new Module;return *instance;} // never destroyed: hooks may run during exit
void bump(std::atomic<std::uint64_t>& a) noexcept {a.fetch_add(1,std::memory_order_relaxed);}
void setState(State s,const char* why) noexcept {
    auto& m=module();m.reason.store(why,std::memory_order_relaxed);m.state.store(int(s),std::memory_order_release);
    if(s==State::Failed)logLine("scope disabled",why);
}
Tuning tuningSnapshot(const Module& m) noexcept {std::lock_guard lock(m.tuningMutex);return m.tuning;}

// ---- objects ----
void destroyDead(VkDevice d,VkDescriptorPool pool,const Dead& x) noexcept {
    if(x.set&&pool)vkFreeDescriptorSets(d,pool,1,&x.set);
    if(x.framebuffer)vkDestroyFramebuffer(d,x.framebuffer,nullptr);
    if(x.pipeline)vkDestroyPipeline(d,x.pipeline,nullptr);
    if(x.pass)vkDestroyRenderPass(d,x.pass,nullptr);
    if(x.view)vkDestroyImageView(d,x.view,nullptr);
    if(x.image)vkDestroyImage(d,x.image,nullptr);
    if(x.memory)vkFreeMemory(d,x.memory,nullptr);
}
void bury(Module& m,const Dead& x,std::uint64_t retired) {
    if(x.frame<=retired){destroyDead(m.device,m.pool,x);return;}
    m.dead.push_back(x);bump(m.counters.deferred);
}
void releaseDeferred(Module& m,std::uint64_t retired) noexcept {
    auto& v=m.dead;std::size_t keep=0;
    for(auto& x:v){if(x.frame<=retired){destroyDead(m.device,m.pool,x);bump(m.counters.deferredReleased);}else v[keep++]=x;}
    v.resize(keep);
}
void destroyBase(Module& m) noexcept {
    const VkDevice d=m.device;if(!d)return;
    if(m.pool)vkDestroyDescriptorPool(d,m.pool,nullptr);
    if(m.layout)vkDestroyPipelineLayout(d,m.layout,nullptr);
    if(m.setLayout)vkDestroyDescriptorSetLayout(d,m.setLayout,nullptr);
    if(m.sampler)vkDestroySampler(d,m.sampler,nullptr);
    if(m.nearest)vkDestroySampler(d,m.nearest,nullptr);
    if(m.vertex)vkDestroyShaderModule(d,m.vertex,nullptr);
    if(m.fragment)vkDestroyShaderModule(d,m.fragment,nullptr);
    m.pool={};m.layout={};m.setLayout={};m.sampler={};m.nearest={};m.vertex=m.fragment={};m.baseBuilt=false;
}
VkShaderModule shaderModule(VkDevice d,const std::uint32_t* code,std::size_t bytes,const char* what) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=bytes;ci.pCode=code;
    VkShaderModule s{};check(vkCreateShaderModule(d,&ci,nullptr,&s),what);return s;
}
// Samplers (linear colour, nearest depth gathers), set layout (binding 0 = the lens copy, binding 1 = its depth copy), pipeline layout
// (128 B push, both stages), pool (two sets per lens image), shaders.
void buildBase(Module& m) {
    const VkDevice d=m.device;
    try{
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};si.magFilter=si.minFilter=VK_FILTER_LINEAR;si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;si.maxLod=0;
        check(vkCreateSampler(d,&si,nullptr,&m.sampler),"SAMPLER");
        si.magFilter=si.minFilter=VK_FILTER_NEAREST; // depth gathers (a depth format need not support linear filtering)
        check(vkCreateSampler(d,&si,nullptr,&m.nearest),"SAMPLER");
        const VkDescriptorSetLayoutBinding bindings[2]{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
                                                       {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=2;dl.pBindings=bindings;
        check(vkCreateDescriptorSetLayout(d,&dl,nullptr,&m.setLayout),"SET_LAYOUT");
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(Push)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&m.setLayout;
        pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(d,&pl,nullptr,&m.layout),"PIPELINE_LAYOUT");
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,16};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dp.maxSets=8;dp.poolSizeCount=1;dp.pPoolSizes=&size;
        check(vkCreateDescriptorPool(d,&dp,nullptr,&m.pool),"DESCRIPTOR_POOL");
        m.vertex=shaderModule(d,shaders::vertex,sizeof(shaders::vertex),"VERTEX_SHADER");
        m.fragment=shaderModule(d,shaders::fragment,sizeof(shaders::fragment),"FRAGMENT_SHADER");
        m.baseBuilt=true;
    }catch(...){destroyBase(m);throw;}
}
// Render pass {eye colour LOAD/STORE, COLOR_ATTACHMENT_OPTIMAL in and out} + the alpha-blended lens pipeline (RGB only).
void buildSlot(Module& m,Slot& s,VkFormat format) {
    const VkDevice d=m.device;Slot n{};n.format=format;
    try{
        VkAttachmentDescription a{};a.format=format;a.samples=VK_SAMPLE_COUNT_1_BIT;a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        const VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
        std::array<VkSubpassDependency,2> deps{};
        deps[0].srcSubpass=VK_SUBPASS_EXTERNAL;deps[0].dstSubpass=0;deps[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;deps[0].srcAccessMask=anyAccess;
        deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;
        deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;deps[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT;deps[1].dstAccessMask=anyAccess;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&a;rp.subpassCount=1;rp.pSubpasses=&sub;
        rp.dependencyCount=2;rp.pDependencies=deps.data();
        check(vkCreateRenderPass(d,&rp,nullptr,&n.pass),"RENDER_PASS");
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthCompareOp=VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendAttachmentState blend{};blend.blendEnable=VK_TRUE;
        blend.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;blend.colorBlendOp=VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;blend.alphaBlendOp=VK_BLEND_OP_ADD;
        blend.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=1;cb.pAttachments=&blend;
        const VkDynamicState dynamic[2]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
        VkPipelineShaderStageCreateInfo stages[2]{};
        for(auto& st:stages){st.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;st.pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=m.vertex;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=m.fragment;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;pi.pVertexInputState=&vi;
        pi.pInputAssemblyState=&ia;pi.pViewportState=&vp;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pDepthStencilState=&ds;
        pi.pColorBlendState=&cb;pi.pDynamicState=&dy;pi.layout=m.layout;pi.renderPass=n.pass;
        check(vkCreateGraphicsPipelines(d,VK_NULL_HANDLE,1,&pi,nullptr,&n.pipeline),"PIPELINE");
    }catch(...){if(n.pass)vkDestroyRenderPass(d,n.pass,nullptr);throw;}
    s=n;
}
Slot* slotFor(Module& m,VkFormat format,std::uint64_t frame,std::uint64_t retired) {
    for(auto& s:m.slots)if(s.pipeline&&s.format==format)return &s;
    Slot* free=nullptr;for(auto& s:m.slots)if(!s.pipeline){free=&s;break;}
    if(!free){free=&m.slots[1];Dead x{};x.frame=frame;x.pass=free->pass;x.pipeline=free->pipeline;bury(m,x,retired);*free=Slot{};} // a third format: replace
    buildSlot(m,*free,format);return free;
}
// The private lens image in the eye format (TRANSFER_DST | SAMPLED) with its own set. A format change recreates both.
// the depth copy of the lens (image, view, memory, its set) retires with the frames that used it; the last copy loses its depth.
void buryLensDepth(Module& m,std::uint64_t frame,std::uint64_t retired) {
    auto& l=m.lens;
    if(l.depthImage||l.depthSet){Dead x{};x.frame=frame;x.view=l.depthView;x.image=l.depthImage;x.memory=l.depthMemory;x.set=l.depthSet;bury(m,x,retired);}
    l.depthImage={};l.depthMemory={};l.depthView={};l.depthFormat={};l.depthSet={};m.copy.depth=false;
}
bool ensureLens(Module& m,VkFormat format,std::uint64_t frame,std::uint64_t retired) {
    if(m.lens.image&&m.lens.format==format)return true;
    if(m.lens.image){buryLensDepth(m,frame,retired); // its set holds the old colour view
        Dead x{};x.frame=frame;x.view=m.lens.view;x.image=m.lens.image;x.memory=m.lens.memory;x.set=m.lens.set;bury(m,x,retired);
        m.lens={};m.copy.valid=false;}
    VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(m.physical,format,&p);
    const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    if((p.optimalTilingFeatures&need)!=need)return false;
    Lens n{};n.format=format;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={lensSize,lensSize,1};
    ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;ci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    if(vkCreateImage(m.device,&ci,nullptr,&n.image)!=VK_SUCCESS)return false;
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(m.device,n.image,&req);
    const auto type=memoryType(m.physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=req.size;a.memoryTypeIndex=type;
    if(type==UINT32_MAX||vkAllocateMemory(m.device,&a,nullptr,&n.memory)!=VK_SUCCESS||vkBindImageMemory(m.device,n.image,n.memory,0)!=VK_SUCCESS){
        if(n.memory)vkFreeMemory(m.device,n.memory,nullptr);vkDestroyImage(m.device,n.image,nullptr);return false;}
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=n.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=m.pool;da.descriptorSetCount=1;da.pSetLayouts=&m.setLayout;
    if(vkCreateImageView(m.device,&vi,nullptr,&n.view)!=VK_SUCCESS||vkAllocateDescriptorSets(m.device,&da,&n.set)!=VK_SUCCESS){
        if(n.view)vkDestroyImageView(m.device,n.view,nullptr);vkFreeMemory(m.device,n.memory,nullptr);vkDestroyImage(m.device,n.image,nullptr);return false;}
    const VkDescriptorImageInfo infos[2]{{m.sampler,n.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{m.nearest,n.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2]{};
    for(unsigned i=0;i<2;++i){w[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w[i].dstSet=n.set;w[i].dstBinding=i;w[i].descriptorCount=1;
        w[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[i].pImageInfo=&infos[i];}
    vkUpdateDescriptorSets(m.device,2,w,0,nullptr); // a fresh set: never used by a command buffer yet
    m.lens=n;return true;
}
// NEAR MASK: the private depth copy of the lens in the engine depth `format` (TRANSFER_DST | SAMPLED | DEPTH_STENCIL_ATTACHMENT
// and the matching format features, as the TAA depth copy) + the set {colour copy, depth copy}. Built once per depth format; a format
// whose image cannot be made is not retried (no per-frame allocation attempts) and the lens draws without the mask.
bool ensureLensDepth(Module& m,VkFormat format,std::uint64_t frame,std::uint64_t retired) {
    auto& l=m.lens;
    if(l.depthImage&&l.depthFormat==format)return true;
    if(format==m.depthFailed||!l.image)return false;
    if(l.depthImage)buryLensDepth(m,frame,retired);
    const auto fail=[&](){m.depthFailed=format;return false;};
    VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(m.physical,format,&p);
    const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if((p.optimalTilingFeatures&need)!=need)return fail();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={lensSize,lensSize,1};
    ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;ci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImage image{};VkDeviceMemory memory{};VkImageView view{};VkDescriptorSet set{};
    if(vkCreateImage(m.device,&ci,nullptr,&image)!=VK_SUCCESS)return fail();
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(m.device,image,&req);
    const auto type=memoryType(m.physical,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=req.size;a.memoryTypeIndex=type;
    if(type==UINT32_MAX||vkAllocateMemory(m.device,&a,nullptr,&memory)!=VK_SUCCESS||vkBindImageMemory(m.device,image,memory,0)!=VK_SUCCESS){
        if(memory)vkFreeMemory(m.device,memory,nullptr);vkDestroyImage(m.device,image,nullptr);return fail();}
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
    vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}; // a sampled view of a depth/stencil image holds one aspect
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=m.pool;da.descriptorSetCount=1;da.pSetLayouts=&m.setLayout;
    if(vkCreateImageView(m.device,&vi,nullptr,&view)!=VK_SUCCESS||vkAllocateDescriptorSets(m.device,&da,&set)!=VK_SUCCESS){
        if(view)vkDestroyImageView(m.device,view,nullptr);vkFreeMemory(m.device,memory,nullptr);vkDestroyImage(m.device,image,nullptr);return fail();}
    const VkDescriptorImageInfo infos[2]{{m.sampler,l.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{m.nearest,view,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet w[2]{};
    for(unsigned i=0;i<2;++i){w[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w[i].dstSet=set;w[i].dstBinding=i;w[i].descriptorCount=1;
        w[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;w[i].pImageInfo=&infos[i];}
    vkUpdateDescriptorSets(m.device,2,w,0,nullptr); // a fresh set
    l.depthImage=image;l.depthMemory=memory;l.depthView=view;l.depthFormat=format;l.depthSet=set;m.copy.depth=false;
    bump(m.counters.depthImages);return true;
}
VkImageView colorView(Module& m,const hands::EyeTarget& t,std::uint64_t frame) {
    for(auto& c:m.colors)if(c.image==t.image&&c.generation==t.generation&&c.format==t.format){c.lastUse=frame;return c.view;}
    if(m.colors.size()>=maxCacheEntries)return VK_NULL_HANDLE;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=t.image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=t.format;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView view{};if(vkCreateImageView(m.device,&vi,nullptr,&view)!=VK_SUCCESS)return VK_NULL_HANDLE;
    m.colors.push_back({t.image,t.generation,t.format,view,frame});bump(m.counters.views);return view;
}
VkFramebuffer framebuffer(Module& m,VkRenderPass pass,VkImageView color,VkExtent2D extent,std::uint64_t frame) {
    for(auto& f:m.framebuffers)if(f.pass==pass&&f.color==color&&f.extent.width==extent.width&&f.extent.height==extent.height){f.lastUse=frame;return f.framebuffer;}
    if(m.framebuffers.size()>=maxCacheEntries)return VK_NULL_HANDLE;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=pass;fi.attachmentCount=1;fi.pAttachments=&color;
    fi.width=extent.width;fi.height=extent.height;fi.layers=1;
    VkFramebuffer fb{};if(vkCreateFramebuffer(m.device,&fi,nullptr,&fb)!=VK_SUCCESS)return VK_NULL_HANDLE;
    m.framebuffers.push_back({pass,color,fb,extent,frame});bump(m.counters.framebuffers);return fb;
}

// ---- geometry (double, WORLD units) ----
DVec3 add(const DVec3& a,const DVec3& b) noexcept {return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
DVec3 sub(const DVec3& a,const DVec3& b) noexcept {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
DVec3 mul(const DVec3& a,double s) noexcept {return {a[0]*s,a[1]*s,a[2]*s};}
bool finiteD(const DVec3& v) noexcept {return std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]);}
// World -> Vulkan clip of eye e: F * P_e * inverse(world_e) (plan.world = the eye camera in WORLD, rigid).
DMat4 worldClip(const hands::EyePlan& plan,unsigned e) noexcept {
    return hands::multiply(hands::vulkanProjection(plan.projection[e]),hands::rigidInverse(hands::widen(plan.world[e])));}
// Clip of a world direction (a point at infinity).
std::array<double,4> clipOfDirection(const DMat4& c,const DVec3& v) noexcept {return hands::transform(c,v[0],v[1],v[2],0.0);}
// Pixel of a direction in an eye of `extent` (positive Vulkan viewport, y down). False behind the eye.
bool pixelOf(const DMat4& c,const DVec3& v,VkExtent2D extent,double& x,double& y) noexcept {
    const auto p=clipOfDirection(c,v);if(!(p[3]>1e-6))return false;
    x=(p[0]/p[3]*0.5+0.5)*extent.width;y=(p[1]/p[3]*0.5+0.5)*extent.height;return std::isfinite(x)&&std::isfinite(y);
}
// The four content directions at the disc's diagonal corners (the copied square must hold all of them).
std::array<DVec3,4> contentCorners(const FrameScope& f,double t) noexcept {
    std::array<DVec3,4> r{};unsigned i=0;
    for(double sy:{-1.0,1.0})for(double sx:{-1.0,1.0})r[i++]=add(f.axis,add(mul(f.right,sx*t),mul(f.up,sy*t)));
    return r;
}
// The scope of this render frame from the VR weapon frame of the plan's head sample.
Skip computeFrame(Module& m,const hands::EyeCall& call,const Tuning& t,FrameScope& f) noexcept {
    f=FrameScope{};f.renderFrame=call.renderFrame;
    const auto& plan=*call.plan;hands::WeaponFrame w{};
    if(!plan.headSampledNs||!m.frames.find(plan.headSampledNs,w))return SkipNoFrame;
    if(w.referenceSpace&&plan.referenceSpace&&w.referenceSpace!=plan.referenceSpace)return SkipSpace;
    const double upm=w.unitsPerMetre>0.f?double(w.unitsPerMetre):double(plan.unitsPerMetre);
    const DVec3 muzzle{w.muzzle[0],w.muzzle[1],w.muzzle[2]};DVec3 d{w.direction[0],w.direction[1],w.direction[2]};
    if(!finiteD(muzzle)||!hands::normalizeD(d))return SkipNoFrame;
    const DMat4 map=hands::widen(w.map); // the gun's LOCAL metres -> WORLD
    const auto g4=hands::transform(map,w.grip.position[0],w.grip.position[1],w.grip.position[2]);const DVec3 grip{g4[0],g4[1],g4[2]};
    // the anchor is the engine-drawn rifle MODEL (origin projected onto the bore, up = muzzle locator +Y) - the controller grip
    // is not rigid with the drawn rifle (wrist anchor, class offset, calibration, two-hand bend). Fallback: the controller grip anchor.
    DVec3 ref=grip,u{};bool model=false;
    {const DVec3 b{w.modelOrigin[0],w.modelOrigin[1],w.modelOrigin[2]};DVec3 gu{w.gunUp[0],w.gunUp[1],w.gunUp[2]};const DVec3 bm=sub(b,muzzle);
     if(finiteD(b)&&finiteD(gu)&&hands::normalizeD(gu)&&std::sqrt(hands::dotD(bm,bm))<1.5*upm){ref=b;u=gu;model=true;}}
    const DVec3 p0=add(muzzle,mul(d,hands::dotD(sub(ref,muzzle),d)));
    if(!model){u=sub(p0,ref);
        if(!(std::sqrt(hands::dotD(u,u))>.01*upm))u=hands::transformDirection(map,hands::rotateD(w.aim.orientation,{0,1,0}));}
    bump(model?m.counters.modelAnchor:m.counters.gripAnchor);
    u=sub(u,mul(d,hands::dotD(u,d)));
    if(!hands::normalizeD(u))return SkipNoFrame;
    DVec3 r=hands::crossD(d,u);if(!hands::normalizeD(r))return SkipNoFrame;
    u=hands::crossD(r,d);
    f.axis=d;f.right=r;f.up=u;
    // + the held rifle's own SCOPE LENS calibration (weapons.cfg type.<20000 + key>: right / up / forward mm, size mm)
    double fwdM=t.fwdM,upM=t.upM,rightM=t.rightM,radiusM=t.radiusM,zoom=t.zoom;
    {const auto c=calib::currentWeapon();
     if(c.held&&c.kind==5){const auto e=calib::store.typeEntry(calib::lensKey(c.typeId));
        rightM+=e.v[calib::OffsetX]/1000.0;upM+=e.v[calib::OffsetY]/1000.0;fwdM+=e.v[calib::OffsetZ]/1000.0;
        radiusM=std::clamp(radiusM+e.v[calib::Pitch]/1000.0,.004,.06);
        zoom=std::clamp(zoom+e.v[calib::Yaw]/10.0,1.5,15.0);}} // SCOPE LENS > ZOOM (tenths)
    f.centre=add(p0,add(mul(d,fwdM*upm),add(mul(u,upM*upm),mul(r,rightM*upm))));
    f.radius=radiusM*upm;f.upm=upm;
    f.tanHalf=std::tan(std::atan(radiusM/double(t.reliefM))/zoom);
    if(!finiteD(f.centre)||!(f.radius>0.0)||!std::isfinite(f.tanHalf))return SkipNoFrame;
    // Source eye: the barrel's vanishing point must be in its image; of two candidates the one farther from the axis (the other
    // one looks along the scope tube, which covers that direction).
    std::array<bool,2> sees{};
    for(unsigned e=0;e<2;++e){
        const DVec3 eye{plan.world[e][12],plan.world[e][13],plan.world[e][14]};
        const DVec3 v=sub(eye,f.centre);const DVec3 perp=sub(v,mul(d,hands::dotD(v,d)));
        f.eyeBackM[e]=-hands::dotD(v,d)/upm; // (metres)
        f.eyeAxisM[e]=std::sqrt(hands::dotD(perp,perp))/upm;
        double x{},y{};const auto c=worldClip(plan,e);
        sees[e]=pixelOf(c,d,{call.color.extent.width,call.color.extent.height},x,y)&&x>=0&&y>=0&&x<double(call.color.extent.width)&&y<double(call.color.extent.height);
    }
    const int farther=f.eyeAxisM[1]>f.eyeAxisM[0]?1:0;
    f.source=sees[unsigned(farther)]?farther:sees[unsigned(1-farther)]?1-farther:-1;
    f.copyTan=f.tanHalf;
    // FULL ZOOM: the eye nearer to the axis (never the source: the copy comes from the other eye's image), its blend from the
    // distance behind / off the eyepiece. The picture keeps the rifle's magnification: content half-angle = picture half-angle / zoom,
    // made smaller (the picture with it) when that content would not fit the copied square of the source eye.
    if(t.fullOn){const unsigned near=unsigned(1-farther);auto& k=m.counters;
        const int latMm=int(std::min(f.eyeAxisM[near]*1000.0,1e6)),backMm=int(std::clamp(f.eyeBackM[near]*1000.0,-1e6,1e6));
        k.fullLastLatMm.store(latMm,std::memory_order_relaxed);k.fullLastBackMm.store(backMm,std::memory_order_relaxed);
        if(backMm>=0&&backMm<=300&&std::uint32_t(latMm)<k.fullBestLatMm.load(std::memory_order_relaxed)){
            k.fullBestLatMm.store(std::uint32_t(latMm),std::memory_order_relaxed);k.fullBestBackMm.store(backMm,std::memory_order_relaxed);}
        if(f.source<0||f.source==int(near))bump(k.fullWrongSource);}
    if(t.fullOn&&f.source>=0&&f.source!=1-farther){
        const unsigned near=unsigned(1-farther);
        const double back=f.eyeBackM[near],lat=f.eyeAxisM[near],fade=std::max(double(t.fullFadeM),1e-3);
        const auto ramp=[](double x){x=std::clamp(x,0.0,1.0);return x*x*(3.0-2.0*x);};
        const double sBack=back>=0.0?1.0-ramp((back-double(t.fullBackM))/fade):1.0-ramp((-back-double(t.fullFrontM))/fade);
        const double s=sBack*(1.0-ramp((lat-double(t.fullLatM))/fade));
        if(!(s>1e-3))bump(m.counters.fullOutside);
        if(s>1e-3){
            double tanApp=std::tan(double(t.fullFovDeg)*3.14159265358979323846/180.0),tanContent=std::tan(std::atan(tanApp)/zoom);
            const auto c=worldClip(plan,unsigned(f.source));const VkExtent2D extent{call.color.extent.width,call.color.extent.height};
            double px{},py{};bool ok=pixelOf(c,d,extent,px,py);
            const double room=double(lensSize)/2.0-4.0; // the copy adds 3 px around the content
            // the pixel size is projective in tan (not linear away from the eye centre): re-measure after a cap
            bool capped=false;
            for(int pass=0;pass<3&&ok;++pass){
                double half=0.0;
                for(const auto& v:contentCorners(f,tanContent)){double x{},y{};if(!pixelOf(c,v,extent,x,y)){ok=false;break;}
                    half=std::max(half,std::max(std::abs(x-px),std::abs(y-py)));}
                if(!ok||half<=room)break;
                tanContent*=room/half*.98;capped=true;
            }
            if(ok&&capped){tanApp=std::tan(std::atan(tanContent)*zoom);bump(m.counters.fullCapped);}
            if(ok&&tanContent>f.tanHalf&&std::isfinite(tanApp)&&tanApp>0.0){
                f.fullEye=int(near);f.fullS=s;f.fullTanApp=tanApp;f.fullTanContent=tanContent;f.copyTan=tanContent;
                bump(m.counters.fullFrames);}
            else bump(m.counters.fullNoFit);
        }
    }
    f.valid=true;return SkipCount;
}
// Batched image barriers (one vkCmdPipelineBarrier per batch, ALL_COMMANDS scopes).
struct Barriers {
    std::array<VkImageMemoryBarrier,6> b{};unsigned n{};
    void add(VkImage image,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst,VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT) noexcept {
        if(!image||n>=b.size())return;
        auto& x=b[n++];x={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};x.image=image;x.oldLayout=from;x.newLayout=to;x.srcAccessMask=src;x.dstAccessMask=dst;
        x.srcQueueFamilyIndex=x.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;x.subresourceRange={aspect,0,1,0,1};
    }
    void flush(VkCommandBuffer c) noexcept {
        if(n)vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,n,b.data());n=0;
    }
};
bool hookEntry(const hands::EyeCall& call) noexcept {return ::rdrvr::vr::scope::recordEye(call);}
} // namespace

// =====================================================================================================================
State state() noexcept {return State(module().state.load(std::memory_order_acquire));}
void publish(const hands::WeaponFrame& f) noexcept {Errno preserve;auto& m=module();m.frames.publish(f);bump(m.counters.published);}
void setEngineRestore(hands::EngineRestore r) noexcept {
    auto& m=module();m.restoreReady.store(r.ready,std::memory_order_release);m.restore.store(r.restore,std::memory_order_release);
}
void configure(VkPhysicalDevice physical,VkDevice device,std::uint32_t family) noexcept {
    Errno preserve;auto& m=module();
    if(m.configured.load(std::memory_order_acquire)||!physical||!device)return;
    char kill[96]{};
    if(propertyText("debug.rdrvr.vr.scope",kill,sizeof kill)&&kill[0]=='0'){setState(State::Killed,"PROPERTY");m.configured.store(true,std::memory_order_release);return;}
    {Tuning t{};readTuning(t);std::lock_guard lock(m.tuningMutex);m.tuning=t;}
    m.physical=physical;m.device=device;m.family=family;
    setState(State::Ready,"OK"); // objects are built at the first eye call (render thread)
    eyeHook.store(&hookEntry,std::memory_order_release);
    m.configured.store(true,std::memory_order_release);
}
void imageDestroyed(VkDevice device,VkImage image) noexcept {
    Errno preserve;auto& m=module();
    if(!image||!m.configured.load(std::memory_order_acquire)||device!=m.device)return;
    try{
        std::lock_guard lock(m.cacheMutex);
        const auto retired=m.retired.load(std::memory_order_acquire);
        for(std::size_t i=0;i<m.colors.size();){
            auto& c=m.colors[i];if(c.image!=image){++i;continue;}
            auto& v=m.framebuffers;std::size_t keep=0;
            for(auto& f:v){if(f.color==c.view){Dead x{};x.frame=f.lastUse;x.framebuffer=f.framebuffer;bury(m,x,retired);}else v[keep++]=f;}
            v.resize(keep);
            Dead x{};x.frame=c.lastUse;x.view=c.view;bury(m,x,retired);
            m.colors.erase(m.colors.begin()+std::ptrdiff_t(i));bump(m.counters.imageDrops);
        }
        releaseDeferred(m,retired);
    }catch(...){}
}
void shutdownAfterIdle(VkDevice device) noexcept {
    Errno preserve;auto& m=module();
    if(!m.configured.load(std::memory_order_acquire)||device!=m.device)return;
    eyeHook.store(nullptr,std::memory_order_release);
    try{
        std::lock_guard lock(m.cacheMutex);const VkDevice d=m.device;
        for(auto& f:m.framebuffers)vkDestroyFramebuffer(d,f.framebuffer,nullptr);
        for(auto& c:m.colors)vkDestroyImageView(d,c.view,nullptr);
        for(auto& x:m.dead){Dead y=x;y.set={};destroyDead(d,VK_NULL_HANDLE,y);} // sets go with the pool
        for(auto& s:m.slots){if(s.pipeline)vkDestroyPipeline(d,s.pipeline,nullptr);if(s.pass)vkDestroyRenderPass(d,s.pass,nullptr);s=Slot{};}
        if(m.lens.image){vkDestroyImageView(d,m.lens.view,nullptr);vkDestroyImage(d,m.lens.image,nullptr);vkFreeMemory(d,m.lens.memory,nullptr);}
        if(m.lens.depthImage){vkDestroyImageView(d,m.lens.depthView,nullptr);vkDestroyImage(d,m.lens.depthImage,nullptr);vkFreeMemory(d,m.lens.depthMemory,nullptr);}
        m.lens={};m.depthFailed=VK_FORMAT_UNDEFINED;m.framebuffers.clear();m.colors.clear();m.dead.clear();m.copy={};m.frame=FrameScope{};
        destroyBase(m);
    }catch(...){}
    m.physical={};m.device={};
    setState(State::Off,"SHUTDOWN");m.configured.store(false,std::memory_order_release);
}

bool recordEye(const hands::EyeCall& call) noexcept {
    Errno preserve;auto& m=module();auto& k=m.counters;
    bump(k.calls);
    bool cacheLocked=false;
    const auto skip=[&](Skip why){bump(k.skips[why]);
        if(!cacheLocked&&call.eye==0&&call.device&&call.device==m.device){ // the graveyard drains while nothing is drawn
            try{std::unique_lock idle(m.cacheMutex,std::try_to_lock);if(idle.owns_lock()&&!m.dead.empty())releaseDeferred(m,call.retiredFrame);}catch(...){}}
        return false;};
    try{
        if(m.state.load(std::memory_order_acquire)!=int(State::Ready))return skip(SkipKilled);
        if(call.device&&call.device==m.device){
            auto seen=m.retired.load(std::memory_order_relaxed);
            while(call.retiredFrame>seen&&!m.retired.compare_exchange_weak(seen,call.retiredFrame,std::memory_order_release,std::memory_order_relaxed)){}
        }
        if(call.eye==0&&(m.poll++&63u)==0){Tuning t{};readTuning(t);std::lock_guard lock(m.tuningMutex);m.tuning=t;}
        Tuning t=tuningSnapshot(m);t.fullOn=::rdrvr::vr::vrSettings.requested().scopeFullZoom(); // lock-free
        if(t.fullBackM<0.f)t.fullBackM=::rdrvr::vr::vrSettings.requested().scopeFullDistM(); // SCOPE LENS > FULL ZOOM DIST
        if(!call.device||call.device!=m.device||call.family!=m.family||!call.command||call.eye>1)return skip(SkipDevice);
        if(!call.plan||!(call.plan->unitsPerMetre>0.f)||!std::isfinite(call.plan->unitsPerMetre)||!call.renderFrame)return skip(SkipPlan);
        const auto& color=call.color;
        if(!color.image||!color.extent.width||!color.extent.height||color.samples!=VK_SAMPLE_COUNT_1_BIT||!color.layers||!color.mips||
           !(color.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)||!colorLayoutOk(color.layout))return skip(SkipColor);
        const auto ready=m.restoreReady.load(std::memory_order_acquire);const auto restore=m.restore.load(std::memory_order_acquire);
        if(!ready||!restore)return skip(SkipRestore);
        // ---- objects (render thread; built once, before any scope frame exists: no hitch when the rifle comes up) ----
        std::unique_lock lock(m.cacheMutex,std::try_to_lock);
        if(!lock.owns_lock())return skip(SkipCache);
        cacheLocked=true;
        if(call.eye==0||m.dead.size()>16)releaseDeferred(m,call.retiredFrame);
        Slot* slot=nullptr;
        try{
            if(!m.baseBuilt)buildBase(m);
            slot=slotFor(m,color.format,call.renderFrame,call.retiredFrame);
        }catch(const VkFailure& f){setState(State::Failed,f.what);return skip(SkipBuild);}
        if(!ensureLens(m,color.format,call.renderFrame,call.retiredFrame)){setState(State::Failed,"LENS_IMAGE");return skip(SkipBuild);}
        // ---- this render frame's scope ----
        if(m.frame.renderFrame!=call.renderFrame){
            const Skip why=t.show?computeFrame(m,call,t,m.frame):SkipHidden;
            if(why!=SkipCount){m.frame.valid=false;m.frame.renderFrame=call.renderFrame;bump(k.skips[why]);}
            else{bump(k.frames);if(m.frame.source>=0)bump(k.sourceEye[unsigned(m.frame.source)]);k.lastSource.store(m.frame.source,std::memory_order_relaxed);
                for(unsigned e=0;e<2;++e)k.lastAxisMm[e].store(std::uint32_t(std::min(m.frame.eyeAxisM[e]*1000.0,1e9)),std::memory_order_relaxed);}
        }
        const FrameScope& f=m.frame;
        if(!f.valid){lock.unlock();cacheLocked=false;return false;} // counted once per frame above
        if(!ready(call.command))return skip(SkipRestore);
        const auto& plan=*call.plan;const unsigned eye=call.eye;
        const DMat4 clip=worldClip(plan,eye);
        // FULL ZOOM: this eye's disc. A disc perpendicular to the axis looks the same from the eye at any distance once its size
        // scales with it, so the full eye's disc sits 0.5 m out (past any near plane) and is blended in ANGLES: from the eyepiece disc
        // as this eye sees it (s = 0: the very same picture) to a picture of fullTanApp centred on the barrel direction (s = 1: the
        // reticle = where the barrel points); the content half-angle blends the same way (the magnification stays the rifle's zoom).
        // The quad reaches ~80 deg around the axis: outside the disc the eye fades to black with s (the scope's surround).
        DVec3 centre=f.centre;double radius=f.radius,tanE=f.tanHalf,quad=1.0,lineScale=1.0,surround=0.0;
        // the full eye is never the source (no copy of its own), so it draws from m.copy - the same test as `picture`
        // below. Without a usable copy (source eye not copyable, first frames) it keeps the normal lens instead of a black eye.
        const bool priorCopy=m.copy.valid&&m.lens.initialized&&(m.copy.renderFrame==call.renderFrame||
            (m.copy.renderFrame<call.renderFrame&&call.renderFrame-m.copy.renderFrame<=3&&f.source==1&&eye==0));
        const bool full=int(eye)==f.fullEye&&f.fullS>0.0&&int(eye)!=f.source&&priorCopy;
        if(full){
            const double s=f.fullS;
            const DVec3 eyePos{plan.world[eye][12],plan.world[eye][13],plan.world[eye][14]};
            const DVec3 rel=sub(f.centre,eyePos);const double ahead=hands::dotD(rel,f.axis),along=std::max(ahead,.01*f.upm);
            // tan offset of the eyepiece centre. g fades the eyepiece's own disc (size + offset) to a point over the last
            // 2 cm before the eye (none at / in front of the eye: the face pushed into the rifle), so the zone's FRONT fade shrinks the
            // picture to nothing instead of ending on a ~50 deg disc (atan(radius / 1 cm)) that then vanished in one frame.
            const double g=std::clamp(ahead/(.02*f.upm),0.0,1.0);
            const DVec3 lateral=mul(sub(rel,mul(f.axis,ahead)),g/along);
            const double appEye=std::atan(f.radius/along),app0=appEye*g,app=app0*(1.0-s)+std::atan(f.fullTanApp)*s,dist=.5*f.upm;
            centre=add(eyePos,mul(add(f.axis,mul(lateral,1.0-s)),dist));
            radius=dist*std::tan(app);
            tanE=std::tan(std::atan(f.tanHalf)*(1.0-s)+std::atan(f.fullTanContent)*s);
            quad=std::max(1.0,5.7/std::tan(app));
            lineScale=std::clamp(appEye/app,1.0/255.0,1.0);surround=s;
        }
        // The disc's bounding square in this eye's clip space and its pixel bounds (render area).
        std::array<std::array<double,4>,4> corner{};bool behind=false;
        double minX=1e30,minY=1e30,maxX=-1e30,maxY=-1e30;
        {const double sx[4]{-1,1,1,-1},sy[4]{-1,-1,1,1};
         for(unsigned i=0;i<4;++i){const DVec3 p=add(centre,add(mul(f.right,sx[i]*radius*quad),mul(f.up,sy[i]*radius*quad)));
             corner[i]=hands::transform(clip,p[0],p[1],p[2]);
             if(!(corner[i][3]>1e-6)){behind=true;continue;}
             const double x=(corner[i][0]/corner[i][3]*0.5+0.5)*color.extent.width,y=(corner[i][1]/corner[i][3]*0.5+0.5)*color.extent.height;
             minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);}}
        VkRect2D area{{0,0},color.extent};
        if(!behind){
            const auto x0=std::int32_t(std::clamp(std::floor(minX)-2,0.0,double(color.extent.width))),x1=std::int32_t(std::clamp(std::ceil(maxX)+2,0.0,double(color.extent.width)));
            const auto y0=std::int32_t(std::clamp(std::floor(minY)-2,0.0,double(color.extent.height))),y1=std::int32_t(std::clamp(std::ceil(maxY)+2,0.0,double(color.extent.height)));
            if(x1<=x0||y1<=y0)return skip(SkipOffscreen);
            area={{x0,y0},{std::uint32_t(x1-x0),std::uint32_t(y1-y0)}};
        }else{bool all=true;for(const auto& c:corner)all=all&&!(c[3]>1e-6);if(all)return skip(SkipOffscreen);}
        // Copy plan: this eye is the source and can be read by a transfer.
        bool doCopy=false,doDepth=false;Copy next{};
        if(int(eye)==f.source){
            if(!(color.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT))bump(k.copyNoTransfer);
            else{
                double px{},py{};bool ok=pixelOf(clip,f.axis,color.extent,px,py);
                double bx0=px,bx1=px,by0=py,by1=py;
                for(const auto& v:contentCorners(f,f.copyTan)){double x{},y{};if(!pixelOf(clip,v,color.extent,x,y)){ok=false;break;}
                    bx0=std::min(bx0,x);bx1=std::max(bx1,x);by0=std::min(by0,y);by1=std::max(by1,y);}
                if(ok){
                    const double W=color.extent.width,H=color.extent.height,S=lensSize;
                    // centred on the vanishing point, at most S x S, inside the eye
                    double hx=std::max(px-bx0,bx1-px)+3,hy=std::max(py-by0,by1-py)+3;
                    if(hx>S/2||hy>S/2)bump(k.clamped);
                    hx=std::min(hx,S/2);hy=std::min(hy,S/2);
                    // floor / ceil of a fractional centre spans S + 1 at the clamp - the far edge stays within S of the near one
                    const auto x0=std::int32_t(std::clamp(std::floor(px-hx),0.0,W)),x1=std::int32_t(std::min(std::clamp(std::ceil(px+hx),0.0,W),double(x0)+S));
                    const auto y0=std::int32_t(std::clamp(std::floor(py-hy),0.0,H)),y1=std::int32_t(std::min(std::clamp(std::ceil(py+hy),0.0,H),double(y0)+S));
                    if(x1>x0&&y1>y0&&std::uint32_t(x1-x0)<=lensSize&&std::uint32_t(y1-y0)<=lensSize){
                        next.valid=true;next.renderFrame=call.renderFrame;next.eye=eye;next.worldClip=clip;next.x=x0;next.y=y0;
                        next.w=std::uint32_t(x1-x0);next.h=std::uint32_t(y1-y0);next.extent=color.extent;doCopy=true;
                    }
                }
            }
            if(!doCopy)bump(k.copyNoSource);
            // NEAR MASK: the source eye's depth of the same square (DEPTH aspect, engine format) and the engine depth of the points
            // at the near distance / its soft end on the scope axis from this eye (worldClip = F * P_e * inverse(world_e) maps a WORLD
            // point to the clip whose z / w IS the engine depth: forward Z, near 0 far 1 (hands.frag / TAA). The shader ramps between
            // the two values whichever way they are ordered, so a reversed Z would mask the same pixels.
            if(doCopy&&t.nearM>0.f){
                const auto& depth=call.depth;
                if(!depth.image)bump(k.depthUnknown);
                else if(!depthFormat(depth.format)||depth.samples!=VK_SAMPLE_COUNT_1_BIT||!depthLayoutOk(depth.layout)||!(depth.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)||
                        !depth.layers||!depth.mips||depth.extent.width!=color.extent.width||depth.extent.height!=color.extent.height||next.w<2||next.h<2)
                    bump(k.depthUnusable);
                else if(!ensureLensDepth(m,depth.format,call.renderFrame,call.retiredFrame))bump(k.depthNoImage);
                else{
                    const DVec3 eyePos{plan.world[eye][12],plan.world[eye][13],plan.world[eye][14]};
                    double z[2]{};bool finite=finiteD(eyePos);
                    for(unsigned i=0;i<2&&finite;++i){
                        const DVec3 p=add(eyePos,mul(f.axis,double(t.nearM)*(i?nearSoftEnd:1.0)*f.upm));
                        const auto q=hands::transform(clip,p[0],p[1],p[2]);
                        finite=q[3]>1e-6&&std::isfinite(z[i]=q[2]/q[3]);
                    }
                    if(!finite||float(z[0])==float(z[1]))bump(k.depthThreshold);
                    else{
                        next.depth=true;next.depthDark=float(z[0]);next.depthClear=float(z[1]);doDepth=true;
                        k.lastDepthDark.store(next.depthDark,std::memory_order_relaxed);k.lastDepthClear.store(next.depthClear,std::memory_order_relaxed);
                        k.reversedZ.store(z[1]<z[0]?1:0,std::memory_order_relaxed);k.lastDepthFormat.store(int(depth.format),std::memory_order_relaxed);
                    }
                }
                if(!doDepth)bump(k.depthUnavailable);
            }
        }
        const VkImageView colorAttachment=colorView(m,color,call.renderFrame);
        if(!colorAttachment)return skip(SkipCache);
        const VkFramebuffer fb=framebuffer(m,slot->pass,colorAttachment,color.extent,call.renderFrame);
        if(!fb)return skip(SkipCache);
        // The copy this eye draws from: this frame's, or (left eye, source = right eye) the previous frame's.
        const Copy& use=doCopy?next:m.copy;
        const bool previous=use.renderFrame<call.renderFrame&&call.renderFrame-use.renderFrame<=3&&f.source==1&&eye==0;
        const bool picture=use.valid&&(use.renderFrame==call.renderFrame||previous)&&(m.lens.initialized||doCopy);
        Push push{};
        for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)push.corner[i*4+j]=float(corner[i][j]);
        if(picture){
            const double W=use.extent.width,H=use.extent.height,S=lensSize;
            const double ax=W/(2*S),bx=(W/2-double(use.x))/S,ay=H/(2*S),by=(H/2-double(use.y))/S;
            const auto uvw=[&](const DVec3& v,float* out){const auto c=clipOfDirection(use.worldClip,v);
                out[0]=float(c[0]*ax+c[3]*bx);out[1]=float(c[1]*ay+c[3]*by);out[2]=float(c[3]);};
            uvw(f.axis,push.axis);uvw(mul(f.right,tanE),push.right);uvw(mul(f.up,tanE),push.up);
            push.axis[3]=float(double(use.w)/S);push.right[3]=float(double(use.h)/S);
            if(!doCopy)bump(k.previousCopy);
        }else bump(k.darkGlass);
        // EYE BOX: like a real scope, the picture is only there for an eye right behind the eyepiece
        // (within boxIn of the axis, behind the lens, not farther than boxBack); past boxOut the glass is dark. Hides the eye-image
        // crop's limits: an eye off the axis would see a view that is not the barrel's / the rifle's own parts.
        float box=1.f;
        if(t.boxInM>0.f){const double lat=f.eyeAxisM[eye],back=f.eyeBackM[eye];
            const double a=std::clamp((lat-double(t.boxInM))/std::max(double(t.boxOutM-t.boxInM),1e-4),0.0,1.0);
            box=back<-.01||back>double(t.boxBackM)?0.f:float(1.0-a*a*(3.0-2.0*a));
            // the FULL ZOOM zone is its own (wider) eye box: inside it (s = 1) the picture is never dimmed - without
            // this the zone's 1.5..2.5 cm off-axis band was dimmed up to half and 1..4 cm in front of the eyepiece was a black eye
            if(full)box=std::max(box,float(f.fullS));
            if(box<.02f)bump(k.eyeBoxDark);}
        // NEAR MASK: the copy drawn from has its depth square (m.copy.depth is cleared when the depth image is replaced).
        const bool shown=picture&&box>.02f;
        const bool nearMask=shown&&use.depth&&t.nearM>0.f&&m.lens.depthSet;
        unsigned flags=(t.reticle?pushReticle:0u)|(shown?pushPicture:0u);
        if(nearMask){flags|=pushNearMask;push.params[2]=use.depthDark;push.up[3]=use.depthClear;bump(k.nearMaskEyes);}
        push.params[3]=.86f;
        if(full){ // scope.vert scales the disc coordinates by params.w; surround / line width in the flag bits (exact in a float)
            flags|=pushFull|(unsigned(std::lround(std::clamp(surround,0.0,1.0)*255.0))<<4)|(unsigned(std::max(1l,std::lround(lineScale*255.0)))<<12);
            push.params[3]=float(quad);bump(k.fullEyes);
            k.lastFullEye.store(int(eye),std::memory_order_relaxed);k.lastFullS.store(std::uint32_t(f.fullS*1000.0),std::memory_order_relaxed);
            k.lastFullFov.store(std::uint32_t(std::atan(radius/(.5*f.upm))*1800.0/3.14159265358979323846),std::memory_order_relaxed);
        }
        push.params[0]=float(flags);push.params[1]=t.bright*box;
        if(doCopy){m.copy=next;}
        const bool initLens=!m.lens.initialized;m.lens.initialized=true;
        const VkImage lensImage=m.lens.image;const VkDescriptorSet lensSet=nearMask?m.lens.depthSet:m.lens.set;
        const VkImage lensDepth=doDepth?m.lens.depthImage:VK_NULL_HANDLE;const VkImageAspectFlags lensDepthAspects=depthAspects(m.lens.depthFormat);
        lock.unlock();cacheLocked=false;
        // ---- recording (no failure past this point) ----
        const VkCommandBuffer c=call.command;Barriers bar{};
        if(doCopy){
            bar.add(color.image,color.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,anyAccess,VK_ACCESS_TRANSFER_READ_BIT);
            bar.add(lensImage,initLens?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
            // the engine depth (both aspects of a depth/stencil image, as HandsPass / TaaPass) received layout -> TRANSFER_SRC and
            // back; the private depth copy from UNDEFINED every time (as the TAA depth copy: only the copied w x h is read, scope.frag)
            const auto depthAspect=depthAspects(call.depth.format);
            if(lensDepth){
                bar.add(call.depth.image,call.depth.layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,anyAccess,VK_ACCESS_TRANSFER_READ_BIT,depthAspect);
                bar.add(lensDepth,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,lensDepthAspects);
            }
            bar.flush(c);
            VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            region.srcOffset={next.x,next.y,0};region.dstOffset={0,0,0};region.extent={next.w,next.h,1};
            vkCmdCopyImage(c,color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,lensImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
            bar.add(lensImage,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
            bar.add(color.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
            if(lensDepth){
                region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
                vkCmdCopyImage(c,call.depth.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,lensDepth,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
                bar.add(lensDepth,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
                        lensDepthAspects);
                bar.add(call.depth.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,call.depth.layout,VK_ACCESS_TRANSFER_READ_BIT,anyAccess,depthAspect);
                bump(k.depthCopies);
            }
            bump(k.copies);k.lastRect.store((next.w<<16)|next.h,std::memory_order_relaxed);
        }else{
            if(initLens)bar.add(lensImage,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,0,VK_ACCESS_SHADER_READ_BIT); // bound, not read (dark glass)
            bar.add(color.image,color.layout,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,anyAccess,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        }
        bar.flush(c);
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=slot->pass;begin.framebuffer=fb;begin.renderArea=area;
        vkCmdBeginRenderPass(c,&begin,VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0,0,float(color.extent.width),float(color.extent.height),0,1};
        vkCmdSetViewport(c,0,1,&viewport);vkCmdSetScissor(c,0,1,&area);
        vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,slot->pipeline);
        vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,m.layout,0,1,&lensSet,0,nullptr);
        vkCmdPushConstants(c,m.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
        vkCmdDraw(c,6,1,0,0);
        vkCmdEndRenderPass(c);
        bar.add(color.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,color.layout,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,anyAccess);
        bar.flush(c);
        restore(c);bump(k.restores);
        bump(k.eyes);bump(k.eyesPerEye[eye]);
        return true;
    }catch(...){
        setState(State::Failed,"RECORD_EXCEPTION");return false;
    }
}

std::string report() {
    auto& m=module();auto& k=m.counters;std::ostringstream o;
    const auto load=[](const std::atomic<std::uint64_t>& a){return a.load(std::memory_order_relaxed);};
    const Tuning t=tuningSnapshot(m);
    const auto rect=k.lastRect.load(std::memory_order_relaxed);
    o<<"{\"state\":\""<<(state()==State::Off?"OFF":state()==State::Killed?"KILLED":state()==State::Ready?"READY":"FAILED")<<"\",\"reason\":\""
     <<m.reason.load(std::memory_order_relaxed)<<"\",\"hook\":"<<(eyeHook.load(std::memory_order_relaxed)!=nullptr)
     <<",\"tuning\":{\"zoom\":"<<t.zoom<<",\"fwdCm\":"<<t.fwdM*100.f<<",\"upCm\":"<<t.upM*100.f<<",\"rightCm\":"<<t.rightM*100.f<<",\"radiusMm\":"<<t.radiusM*1000.f
     <<",\"reliefCm\":"<<t.reliefM*100.f<<",\"reticle\":"<<t.reticle<<",\"bright\":"<<t.bright<<",\"show\":"<<(t.show?1:0)<<",\"nearCm\":"<<t.nearM*100.f<<"}"
     <<",\"native\":{\"state\":"<<nativeScope.load(std::memory_order_relaxed)<<",\"writes\":"<<load(nativeScopeWrites)
     <<",\"restores\":"<<load(nativeScopeRestores)<<"}"
     <<",\"published\":"<<load(k.published)<<",\"calls\":"<<load(k.calls)<<",\"frames\":"<<load(k.frames)<<",\"eyes\":"<<load(k.eyes)
     <<",\"eyesPerEye\":["<<load(k.eyesPerEye[0])<<','<<load(k.eyesPerEye[1])<<"],\"sourceEye\":["<<load(k.sourceEye[0])<<','<<load(k.sourceEye[1])<<"]"
     <<",\"lastSource\":"<<k.lastSource.load(std::memory_order_relaxed)
     <<",\"lastEyeAxisMm\":["<<k.lastAxisMm[0].load(std::memory_order_relaxed)<<','<<k.lastAxisMm[1].load(std::memory_order_relaxed)<<"]"
     <<",\"copies\":"<<load(k.copies)<<",\"previousCopy\":"<<load(k.previousCopy)<<",\"darkGlass\":"<<load(k.darkGlass)
     <<",\"copyNoSource\":"<<load(k.copyNoSource)<<",\"copyNoTransfer\":"<<load(k.copyNoTransfer)<<",\"clamped\":"<<load(k.clamped)<<",\"eyeBoxDark\":"<<load(k.eyeBoxDark)<<",\"modelAnchor\":"<<load(k.modelAnchor)<<",\"gripAnchor\":"<<load(k.gripAnchor)
     <<",\"lastCopyPx\":["<<(rect>>16)<<','<<(rect&0xffffu)<<"]"
     <<",\"near\":{\"depthCopies\":"<<load(k.depthCopies)<<",\"depthUnavailable\":"<<load(k.depthUnavailable)<<",\"unknown\":"<<load(k.depthUnknown)
     <<",\"unusable\":"<<load(k.depthUnusable)<<",\"noImage\":"<<load(k.depthNoImage)<<",\"threshold\":"<<load(k.depthThreshold)
     <<",\"nearMaskEyes\":"<<load(k.nearMaskEyes)<<",\"depthImages\":"<<load(k.depthImages)<<",\"depthFormat\":"<<k.lastDepthFormat.load(std::memory_order_relaxed)
     <<",\"reversedZ\":"<<k.reversedZ.load(std::memory_order_relaxed)<<",\"lastDepth\":["<<k.lastDepthDark.load(std::memory_order_relaxed)<<','
     <<k.lastDepthClear.load(std::memory_order_relaxed)<<"]}"
     <<",\"full\":{\"on\":"<<(::rdrvr::vr::vrSettings.requested().scopeFullZoom()?1:0)<<",\"backCm\":"<<(t.fullBackM<0.f?::rdrvr::vr::vrSettings.requested().scopeFullDistM():t.fullBackM)*100.f<<",\"latCm\":"<<t.fullLatM*100.f
     <<",\"fadeCm\":"<<t.fullFadeM*100.f<<",\"fovDeg\":"<<t.fullFovDeg<<",\"frames\":"<<load(k.fullFrames)<<",\"eyes\":"<<load(k.fullEyes)
     <<",\"capped\":"<<load(k.fullCapped)<<",\"lastEye\":"<<k.lastFullEye.load(std::memory_order_relaxed)
     <<",\"lastS\":"<<double(k.lastFullS.load(std::memory_order_relaxed))/1000.0<<",\"lastFovDeg\":"<<double(k.lastFullFov.load(std::memory_order_relaxed))/10.0
     <<",\"wrongSource\":"<<load(k.fullWrongSource)<<",\"outside\":"<<load(k.fullOutside)<<",\"noFit\":"<<load(k.fullNoFit)
     <<",\"bestLatMm\":"<<k.fullBestLatMm.load(std::memory_order_relaxed)<<",\"bestBackMm\":"<<k.fullBestBackMm.load(std::memory_order_relaxed)
     <<",\"lastLatMm\":"<<k.fullLastLatMm.load(std::memory_order_relaxed)<<",\"lastBackMm\":"<<k.fullLastBackMm.load(std::memory_order_relaxed)<<"}"
     <<",\"skips\":{";
    for(unsigned i=0;i<SkipCount;++i)o<<(i?",":"")<<'"'<<skipNames[i]<<"\":"<<load(k.skips[i]);
    o<<"},\"objects\":{\"views\":"<<load(k.views)<<",\"framebuffers\":"<<load(k.framebuffers)<<",\"imageDrops\":"<<load(k.imageDrops)
     <<",\"deferred\":"<<load(k.deferred)<<",\"deferredReleased\":"<<load(k.deferredReleased)<<"},\"restores\":"<<load(k.restores)<<"}";
    return o.str();
}
} // namespace rdrvr::vr::scope
