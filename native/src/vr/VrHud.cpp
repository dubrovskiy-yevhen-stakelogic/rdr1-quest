// HUD: see VrHud.h. Render thread = producer (copy of the cleared stock HUD target into a 3-slot ring, on the game
// queue right behind the game's own submission), XR thread = consumer (ring slot -> one XR swapchain -> head / wrist quads).
#include "VrHud.h"
#include "VrSettings.h"
#include "VrForearm.h"
#include "runtime/XrPlatform.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
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

namespace rdrvr::vr::hud {
namespace {
struct PreserveErrno {int v{errno};~PreserveErrno(){errno=v;}};
std::uint64_t nowNs() noexcept {return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
constexpr unsigned slotCount=3;
constexpr float pi=3.14159265358979f;
constexpr std::uint32_t ammoW=256,ammoH=64; // AMMO ON GUN strip, RGBA8 = 65536 bytes (the vkCmdUpdateBuffer limit)

// ---- properties (XR thread every 64 frames, render thread once at bind) ----
struct Props {
    std::atomic<int> scale{50},every{1},alpha{0},deg{0},dist{140},down{10},follow{1},gaze{1},maxAge{400},dump{0},forceMode{0},wristAlpha{200},wristArm{1},wristFace{1},wristTilt{70},wristFacing{1},subtitles{1},subDown{20};
    std::array<std::atomic<int>,4> subBand{}; // SUBTITLES band x0,y0,x1,y1 per mille of the HUD image
    std::array<std::atomic<int>,4> radar{};std::array<std::atomic<int>,3> wrist{};
    std::array<std::atomic<int>,8> mapfix{}; // Source x0,y0,x1,y1 + destination x0,y0,x1,y1 per mille; mapfix[0] < 0 = off
    // PANEL: every Flash movie (help box, subtitles, objectives, prompts, ammo) is drawn 1:1 into the top-left 1374x720
    // (sm_GlobalWidth/Height) of the 1680x1760 HUD target (grcDevice::SetWindowFullRes), only the radar uses the whole target. The head
    // panel shows that Flash area (flash = w,h per mille of the image, 0 = whole image) and the radar as its own square quad on the panel
    // (radarpanel = left, bottom, size in per mille of the panel HEIGHT).
    std::array<std::atomic<int>,2> flash{};std::array<std::atomic<int>,3> radarPanel{};
    // AMMO ON GUN: along, across, lift, width (mm) of the strip beside the barrel; crop = stock ammo element rect (per mille, -1 = digits)
    std::array<std::atomic<int>,4> ammoPlace{},ammoCrop{};std::atomic<int> ammoTilt{50},ammoFace{1};
    Props(){radar[0]=10;radar[1]=660;radar[2]=350;radar[3]=990;wrist[0]=130;wrist[1]=-15;wrist[2]=100;
        for(auto& m:mapfix)m=-1;
        ammoPlace[0]=20;ammoPlace[1]=40;ammoPlace[2]=25;ammoPlace[3]=50; // default place of the ammo strip
        for(auto& a:ammoCrop)a=-1;
        flash[0]=818;flash[1]=409;radarPanel[0]=50;radarPanel[1]=50;radarPanel[2]=340;
        subBand[0]=0;subBand[1]=250;subBand[2]=818;subBand[3]=409;}
};
Props props;
bool readProp(const char* name,char* out,std::size_t size) noexcept {
#ifdef __ANDROID__
    char b[PROP_VALUE_MAX]{};const int n=__system_property_get(name,b);if(n<=0)return false;std::snprintf(out,size,"%s",b);return true;
#else
    (void)name;(void)out;(void)size;return false;
#endif
}
void propInt(const char* name,std::atomic<int>& v,int lo,int hi,int def) noexcept {
    char b[96]{};if(!readProp(name,b,sizeof b)){v.store(def,std::memory_order_relaxed);return;}
    char* end{};const long x=std::strtol(b,&end,10);v.store(end!=b&&x>=lo&&x<=hi?int(x):def,std::memory_order_relaxed);
}
template<std::size_t N> void propList(const char* name,std::array<std::atomic<int>,N>& v,const std::array<int,N>& def,int lo,int hi) noexcept {
    char b[96]{};std::array<int,N> parsed=def;
    if(readProp(name,b,sizeof b)){const char* s=b;bool good=true;
        for(std::size_t i=0;i<N&&good;++i){char* end{};const long x=std::strtol(s,&end,10);good=end!=s&&x>=lo&&x<=hi;if(good)parsed[i]=int(x);s=end;if(*s==',')++s;}
        if(!good)parsed=def;}
    for(std::size_t i=0;i<N;++i)v[i].store(parsed[i],std::memory_order_relaxed);
}
void readProps() noexcept {
    PreserveErrno keep;
    propInt("debug.rdrvr.vr.hud.scale",props.scale,25,100,50);propInt("debug.rdrvr.vr.hud.every",props.every,1,4,1);
    propInt("debug.rdrvr.vr.hud.alpha",props.alpha,0,1,0);propInt("debug.rdrvr.vr.hud.deg",props.deg,0,120,0);
    propInt("debug.rdrvr.vr.hud.dist",props.dist,40,500,140);propInt("debug.rdrvr.vr.hud.down",props.down,-30,45,10);
    propInt("debug.rdrvr.vr.hud.follow",props.follow,0,1,1);propInt("debug.rdrvr.vr.hud.gaze",props.gaze,0,1,1);
    propInt("debug.rdrvr.vr.hud.maxage",props.maxAge,50,5000,400);
    propList<4>("debug.rdrvr.vr.hud.radar",props.radar,{10,660,350,990},0,1000);
    if(props.radar[2].load()<=props.radar[0].load()+20||props.radar[3].load()<=props.radar[1].load()+20){
        props.radar[0]=10;props.radar[1]=660;props.radar[2]=350;props.radar[3]=990;}
    propList<3>("debug.rdrvr.vr.hud.wrist",props.wrist,{130,-15,100},-300,300);
    if(props.wrist[2].load()<30)props.wrist[2]=95;
    propList<8>("debug.rdrvr.vr.hud.mapfix",props.mapfix,{-1,-1,-1,-1,-1,-1,-1,-1},-1,1000); // Off by default (TraceEntry RADAR FIX); fallback 44,291,246,387,54,709,301,945
    {const auto& m=props.mapfix;
     if(m[0].load()<0||m[2].load()<=m[0].load()+5||m[3].load()<=m[1].load()+5||m[6].load()<=m[4].load()+5||m[7].load()<=m[5].load()+5)
         for(auto& x:props.mapfix)x.store(-1,std::memory_order_relaxed);}
    propInt("debug.rdrvr.vr.hud.dump",props.dump,0,1000000000,0);
    propList<2>("debug.rdrvr.vr.hud.flash",props.flash,{818,409},0,1000);
    propList<3>("debug.rdrvr.vr.hud.radarpanel",props.radarPanel,{50,50,340},0,1000);
    propInt("debug.rdrvr.vr.hud.mode",props.forceMode,0,2,0);
    propInt("debug.rdrvr.vr.hud.wristalpha",props.wristAlpha,100,400,200);
    propInt("debug.rdrvr.vr.hud.wristarm",props.wristArm,0,1,1);
    propInt("debug.rdrvr.vr.hud.wristface",props.wristFace,0,1,1);
    propInt("debug.rdrvr.vr.hud.subtitles",props.subtitles,0,1,1);propInt("debug.rdrvr.vr.hud.subdown",props.subDown,-30,60,20);
    propList<4>("debug.rdrvr.vr.hud.subband",props.subBand,{0,250,818,409},0,1000);
    if(props.subBand[2].load()<=props.subBand[0].load()+20||props.subBand[3].load()<=props.subBand[1].load()+10){
        props.subBand[0]=0;props.subBand[1]=250;props.subBand[2]=818;props.subBand[3]=409;}
    propInt("debug.rdrvr.vr.hud.wristtilt",props.wristTilt,0,100,70);propInt("debug.rdrvr.vr.hud.wristfacing",props.wristFacing,0,2,1);
    propList<4>("debug.rdrvr.vr.hud.ammo",props.ammoPlace,{50,45,0,60},-300,300);
    if(props.ammoPlace[3].load()<20)props.ammoPlace[3]=60;
    propList<4>("debug.rdrvr.vr.hud.ammocrop",props.ammoCrop,{12,259,107,293},-1,1000); // the stock ammo counter
    {const auto& a=props.ammoCrop;
     if(a[0].load()<0||a[2].load()<=a[0].load()+5||a[3].load()<=a[1].load()+5)for(auto& x:props.ammoCrop)x.store(-1,std::memory_order_relaxed);}
    propInt("debug.rdrvr.vr.hud.ammotilt",props.ammoTilt,0,100,50);propInt("debug.rdrvr.vr.hud.ammoface",props.ammoFace,0,1,1);
}

// ---- telemetry ----
struct Stats {
    std::atomic<std::uint64_t> wanted{},begun{},recorded{},cancelled{},published{},consumed{},headLayers{},wristLayers{},gazeHidden{},
        staleHidden{},noGrip{},xrFrames{},ringBuilds{},swapchainBuilds{},pendingMissed{},alphaNormalized{},alphaUnsupported{},alphaTemporary{},
        alphaReadFailures{},submitCpuNs{},submitCalls{},clearAlphaDropped{},clearAlphaTaken{},mapLayers{},dumps{},dumpFailures{},wristOnArm{},subtitleFrames{},subtitleLayers{},
        ammoLayers{},ammoHidden{},ammoUploads{};
    std::array<std::atomic<std::uint64_t>,unsigned(Skip::Count)> skips{};
    std::atomic<std::uint64_t> lastPublishedNs{},lastFrame{},lastLayout{},lastSourceW{},lastSourceH{},lastFormat{},ringW{},ringH{},swapW{},swapH{},swapFormat{};
    std::atomic<std::int64_t> radar[4]{};
    std::atomic<const char*> lastError{""},lastSkip{""};
    std::atomic<int> errors{};
};
Stats stats;
constexpr const char* skipNames[unsigned(Skip::Count)]{"noWorld","notWorld","apis","source","binding","format","clear","ring","busy","layout","lock","alpha"};
std::atomic<int> stateValue{int(State::Off)};
void setState(State s) noexcept {stateValue.store(int(s),std::memory_order_release);}
void fail(const char* why) noexcept {
    stats.lastError.store(why,std::memory_order_relaxed);stats.errors.fetch_add(1,std::memory_order_relaxed);setState(State::Failed);
    status::publish(Feature::Hud,status::Failed,nullptr);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_ERROR,"RdrVrHud","HUD disabled: %s",why);
#endif
}

// ---- device objects + ring (mutex shared by render thread and XR thread; render thread only try_locks) ----
struct Slot {
    VkImage image{};VkDeviceMemory memory{};
    VkCommandBuffer produce{};VkFence produceFence{};bool produceSubmitted{};
    VkCommandBuffer consume{};VkFence consumeFence{};bool reading{};
    std::uint64_t sequence{},frame{},publishedNs{};bool subtitles{};
};
struct Shared {
    std::mutex mutex;
    VkPhysicalDevice physical{};VkDevice device{};std::uint32_t family{};
    VkCommandPool producePool{},consumePool{};std::uint32_t consumeFamily{UINT32_MAX};
    VkExtent2D extent{};VkFormat format{};bool linear{};std::uint64_t generation{};
    std::array<Slot,slotCount> slots{};std::uint64_t sequence{};
};
Shared shared;

// ---- producer state (render thread only) ----
struct Producer {Source active{};bool armed{};Source pending{};VkCommandBuffer pendingCommand{};bool hasPending{},clearAlpha{};std::uint64_t counter{};};
std::atomic<bool> clearAlphaHookedFlag{};std::atomic<int> clearAlphaFingerprint{-1};
Producer producer;
bool fenceDone(VkDevice d,VkFence f) noexcept;
// HUD dump (render thread, shared.mutex held): one host-visible copy of a ring slot -> rdrvr/hud-dump.raw (+ .json)
struct Dump {VkBuffer buffer{};VkDeviceMemory memory{};VkDeviceSize size{};void* mapped{};int slot{-1};std::uint32_t w{},h{};VkFormat format{};int lastRequest{0};};
Dump dump;
constexpr const char* dumpDir="/sdcard/Android/data/com.rockstargames.rdr/files/rdrvr";
void destroyDump() noexcept {
    if(dump.mapped&&dump.memory)vkUnmapMemory(shared.device,dump.memory);
    if(dump.buffer)vkDestroyBuffer(shared.device,dump.buffer,nullptr);
    if(dump.memory)vkFreeMemory(shared.device,dump.memory,nullptr);
    const int last=dump.lastRequest;dump=Dump{};dump.lastRequest=last;
}
bool ensureDumpBuffer(VkDeviceSize size) noexcept {
    if(dump.buffer&&dump.size>=size)return true;
    destroyDump();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(shared.device,&bi,nullptr,&dump.buffer)!=VK_SUCCESS){dump.buffer={};return false;}
    VkMemoryRequirements r{};vkGetBufferMemoryRequirements(shared.device,dump.buffer,&r);
    VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(shared.physical,&p);
    std::uint32_t type=UINT32_MAX;const VkMemoryPropertyFlags want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for(std::uint32_t i=0;i<p.memoryTypeCount&&i<VK_MAX_MEMORY_TYPES;++i)if((r.memoryTypeBits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&want)==want){type=i;break;}
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=type;
    if(type==UINT32_MAX||vkAllocateMemory(shared.device,&a,nullptr,&dump.memory)!=VK_SUCCESS||vkBindBufferMemory(shared.device,dump.buffer,dump.memory,0)!=VK_SUCCESS||
       vkMapMemory(shared.device,dump.memory,0,VK_WHOLE_SIZE,0,&dump.mapped)!=VK_SUCCESS){destroyDump();return false;}
    dump.size=size;return true;
}
void finishDump() noexcept {
    if(dump.slot<0)return;
    auto& s=shared.slots[unsigned(dump.slot)];
    if(s.produceSubmitted&&!fenceDone(shared.device,s.produceFence))return;
    dump.slot=-1;
    const std::string base=std::string(dumpDir)+"/hud-dump";bool ok=false;
    const std::size_t bytes=std::size_t(dump.w)*dump.h*4u;
    if(dump.mapped)if(FILE* f=std::fopen((base+".raw").c_str(),"wb")){ok=std::fwrite(dump.mapped,1,bytes,f)==bytes;std::fclose(f);}
    char j[256];std::snprintf(j,sizeof j,"{\"width\":%u,\"height\":%u,\"format\":%u,\"request\":%d}",dump.w,dump.h,unsigned(dump.format),dump.lastRequest);
    if(FILE* f=std::fopen((base+".json").c_str(),"wb")){std::fwrite(j,1,std::strlen(j),f);std::fclose(f);}
    (ok?stats.dumps:stats.dumpFailures).fetch_add(1,std::memory_order_relaxed);
}

// ---- alpha route ----
AlphaApi alphaApi{};thread_local bool alphaThread{}; // alphaLiveCount inline in VrHud.h

bool fenceDone(VkDevice d,VkFence f) noexcept {return f&&vkGetFenceStatus(d,f)==VK_SUCCESS;}
std::uint32_t memoryType(VkPhysicalDevice physical,std::uint32_t bits,VkMemoryPropertyFlags preferred) noexcept {
    VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(physical,&p);
    for(int pass=0;pass<2;++pass)for(std::uint32_t i=0;i<p.memoryTypeCount&&i<VK_MAX_MEMORY_TYPES;++i){
        if(!(bits&(1u<<i)))continue;
        if(pass==0&&(p.memoryTypes[i].propertyFlags&preferred)!=preferred)continue;
        return i;}
    return UINT32_MAX;
}
void barrier(VkCommandBuffer c,VkImage image,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst,
             VkPipelineStageFlags srcStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VkPipelineStageFlags dstStage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT) noexcept {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=from;b.newLayout=to;b.srcAccessMask=src;b.dstAccessMask=dst;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(c,srcStage,dstStage,0,0,nullptr,0,nullptr,1,&b);
}
// Slot images of the current extent/format (shared.mutex held). Every slot must be idle (no producer copy, no consumer read).
bool slotsIdle() noexcept {
    for(auto& s:shared.slots){
        if(s.reading&&fenceDone(shared.device,s.consumeFence))s.reading=false;
        if(s.reading||(s.produceSubmitted&&!fenceDone(shared.device,s.produceFence)))return false;
    }
    return true;
}
void destroyImages() noexcept {
    for(auto& s:shared.slots){
        if(s.image)vkDestroyImage(shared.device,s.image,nullptr);
        if(s.memory)vkFreeMemory(shared.device,s.memory,nullptr);
        s.image={};s.memory={};s.produceSubmitted=false;s.sequence=s.frame=s.publishedNs=0;
    }
    shared.extent={};shared.format=VK_FORMAT_UNDEFINED;
}
// Render thread (mutex held). false + skip/fail on refusal.
bool ensureRing(VkExtent2D extent,VkFormat format) noexcept {
    auto& d=shared;
    if(!d.producePool){
        VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};p.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;p.queueFamilyIndex=d.family;
        if(vkCreateCommandPool(d.device,&p,nullptr,&d.producePool)!=VK_SUCCESS){d.producePool={};fail("PRODUCE_POOL");return false;}
        for(auto& s:d.slots){
            VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=d.producePool;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;
            VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if(vkAllocateCommandBuffers(d.device,&a,&s.produce)!=VK_SUCCESS||vkCreateFence(d.device,&f,nullptr,&s.produceFence)!=VK_SUCCESS){fail("PRODUCE_OBJECTS");return false;}
        }
    }
    if(d.extent.width==extent.width&&d.extent.height==extent.height&&d.format==format&&d.slots[0].image)return true;
    if(!slotsIdle()){skip(Skip::Ring);return false;}
    destroyImages();
    VkFormatProperties fp{};vkGetPhysicalDeviceFormatProperties(d.physical,format,&fp);
    const auto need=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if((fp.optimalTilingFeatures&need)!=need){skip(Skip::Format);return false;}
    d.linear=(fp.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)!=0;
    for(auto& s:d.slots){
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={extent.width,extent.height,1};
        ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(d.device,&ci,nullptr,&s.image)!=VK_SUCCESS){s.image={};destroyImages();fail("RING_IMAGE");return false;}
        VkMemoryRequirements r{};vkGetImageMemoryRequirements(d.device,s.image,&r);
        const auto type=memoryType(d.physical,r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=type;
        if(type==UINT32_MAX||vkAllocateMemory(d.device,&a,nullptr,&s.memory)!=VK_SUCCESS||vkBindImageMemory(d.device,s.image,s.memory,0)!=VK_SUCCESS){
            destroyImages();fail("RING_MEMORY");return false;}
    }
    d.extent=extent;d.format=format;++d.generation;stats.ringBuilds.fetch_add(1,std::memory_order_relaxed);
    stats.ringW.store(extent.width,std::memory_order_relaxed);stats.ringH.store(extent.height,std::memory_order_relaxed);
    return true;
}
bool acceptedLayout(VkImageLayout l) noexcept {
    return l==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL||l==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL||l==VK_IMAGE_LAYOUT_GENERAL||l==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}
bool acceptedFormat(VkFormat f) noexcept {
    return f==VK_FORMAT_B8G8R8A8_SRGB||f==VK_FORMAT_R8G8B8A8_SRGB||f==VK_FORMAT_B8G8R8A8_UNORM||f==VK_FORMAT_R8G8B8A8_UNORM;
}
bool srgb(VkFormat f) noexcept {return f==VK_FORMAT_B8G8R8A8_SRGB||f==VK_FORMAT_R8G8B8A8_SRGB;}

// ---- consumer state (XR thread only) ----
struct Consumer {
    XrSession session{};XrSwapchain swapchain{};std::vector<XrSwapchainImageVulkan2KHR> images;VkFormat format{};
    std::uint64_t generation{};std::uint32_t width{},height{},headW{},headH{},strip{};
    std::uint64_t displayedSequence{},displayedNs{},displayedGeneration{};unsigned displayedMode{};bool valid{};
    // head quad lazy follow
    bool init{},following{};float yaw{};std::uint64_t lastNs{};
    std::uint64_t frames{};
    std::array<std::int32_t,4> radarPx{};
    bool mapShown{};std::array<float,4> mapDst{}; // Dst rect (0..1 of the HUD image) of the last blitted mapfix region
    int rollAxis{-1};std::uint64_t rollPicks{};
    std::array<float,3> diagElbow{},diagWrist{},diagLocal{};float diagLen{};std::uint64_t diagAgeMs{};bool diagOnArm{};std::uint64_t faceFrames{};
    float wristFade{},diagFacing{},diagGaze{}; // HAND: eased wrist map opacity (0..1), last facing / gaze cosines
    std::uint32_t panelW{},panelH{};bool panelRadar{},subtitles{}; // PANEL: head image rect (Flash area) of the last copy; radar square valid
    // AMMO ON GUN: device buffer of the digits strip (rewritten by vkCmdUpdateBuffer only when the text changes), the strip's place in the
    // swapchain [W, W+ammoW) x [S, S+ammoH) and what the last released image holds there (ammoRect = x, y, w, h)
    VkBuffer ammoBuffer{};VkDeviceMemory ammoMemory{};bool ammoFailed{},ammoText{},ammoDrawn{};std::int64_t ammoKey{INT64_MIN};
    std::uint32_t ammoX{},ammoY{};std::array<std::int32_t,4> ammoRect{};std::array<std::uint32_t,ammoW*ammoH> ammoPixels{};
};
Consumer consumer;

// quaternion helpers (x,y,z,w)
struct Q {float x,y,z,w;};
Q mul(Q a,Q b) noexcept {return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};}
void rot(Q q,const float v[3],float out[3]) noexcept {
    const float tx=2*(q.y*v[2]-q.z*v[1]),ty=2*(q.z*v[0]-q.x*v[2]),tz=2*(q.x*v[1]-q.y*v[0]);
    out[0]=v[0]+q.w*tx+(q.y*tz-q.z*ty);out[1]=v[1]+q.w*ty+(q.z*tx-q.x*tz);out[2]=v[2]+q.w*tz+(q.x*ty-q.y*tx);
}
float wrapAngle(float a) noexcept {while(a>pi)a-=2*pi;while(a<-pi)a+=2*pi;return a;}
bool finite3(const float v[3]) noexcept {return std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]);}
float smooth(float a,float b,float x) noexcept {const float t=std::clamp((x-a)/(b-a),0.f,1.f);return t*t*(3.f-2.f*t);}
float dot3(const float a[3],const float b[3]) noexcept {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
bool normalize3(float v[3],float minLen) noexcept {const float l=std::sqrt(dot3(v,v));if(!(l>minLen))return false;for(unsigned i=0;i<3;++i)v[i]/=l;return true;}

void destroySwapchain() noexcept {
    if(consumer.swapchain)xrDestroySwapchain(consumer.swapchain);
    consumer.swapchain={};consumer.images.clear();consumer.valid=false;consumer.generation=0;consumer.displayedSequence=0;consumer.ammoDrawn=false;
}
// XR thread: (re)create the [HUD | wrist square] swapchain for the ring generation.
bool ensureSwapchain(XrSession session,VkExtent2D ring,VkFormat ringFormat,std::uint64_t generation) {
    if(consumer.swapchain&&consumer.session==session&&consumer.generation==generation)return true;
    destroySwapchain();consumer.session=session;
    std::uint32_t n{};runtime::checkXr(xrEnumerateSwapchainFormats(session,0,&n,nullptr),"hud format count");
    if(!n||n>256)throw std::runtime_error("hud format count");
    std::vector<std::int64_t> formats(n);runtime::checkXr(xrEnumerateSwapchainFormats(session,n,&n,formats.data()),"hud formats");
    VkFormat chosen=VK_FORMAT_UNDEFINED;
    const VkFormat order[3]{srgb(ringFormat)?ringFormat:VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_R8G8B8A8_SRGB};
    for(VkFormat f:order){
        VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(shared.physical,f,&p);
        if(std::find(formats.begin(),formats.end(),std::int64_t(f))!=formats.end()&&(p.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT)){chosen=f;break;}
    }
    if(chosen==VK_FORMAT_UNDEFINED)throw std::runtime_error("NO_SRGB_SWAPCHAIN_FORMAT");
    consumer.headW=ring.width;consumer.headH=ring.height;consumer.strip=std::max(32u,std::uint32_t(std::lround(0.35*double(ring.height))));
    // [HUD | wrist S | map S], the AMMO ON GUN strip under the wrist square
    consumer.width=std::max(ring.width+2*consumer.strip,ring.width+ammoW);consumer.height=std::max(ring.height,consumer.strip+ammoH);
    consumer.ammoX=ring.width;consumer.ammoY=consumer.strip;
    XrSwapchainCreateInfo c{XR_TYPE_SWAPCHAIN_CREATE_INFO};c.usageFlags=XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT|XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    c.format=chosen;c.sampleCount=c.faceCount=c.arraySize=c.mipCount=1;c.width=consumer.width;c.height=consumer.height;
    runtime::checkXr(xrCreateSwapchain(session,&c,&consumer.swapchain),"hud swapchain");
    runtime::checkXr(xrEnumerateSwapchainImages(consumer.swapchain,0,&n,nullptr),"hud image count");
    if(!n||n>16)throw std::runtime_error("hud image count");
    consumer.images.assign(n,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    runtime::checkXr(xrEnumerateSwapchainImages(consumer.swapchain,n,&n,reinterpret_cast<XrSwapchainImageBaseHeader*>(consumer.images.data())),"hud images");
    consumer.format=chosen;consumer.generation=generation;
    stats.swapchainBuilds.fetch_add(1,std::memory_order_relaxed);stats.swapW.store(consumer.width,std::memory_order_relaxed);
    stats.swapH.store(consumer.height,std::memory_order_relaxed);stats.swapFormat.store(std::uint64_t(chosen),std::memory_order_relaxed);
    return true;
}
// radar rect (pixels of the ring image) from debug.rdrvr.vr.hud.radar (per mille)
std::array<std::int32_t,4> radarRect(std::uint32_t w,std::uint32_t h) noexcept {
    auto px=[](int pm,std::uint32_t size){return std::int32_t(std::clamp<long>(std::lround(double(pm)*double(size)/1000.0),0,long(size)));};
    std::array<std::int32_t,4> r{px(props.radar[0].load(),w),px(props.radar[1].load(),h),px(props.radar[2].load(),w),px(props.radar[3].load(),h)};
    if(r[2]-r[0]<8||r[3]-r[1]<8)r={0,0,std::int32_t(w),std::int32_t(h)};
    return r;
}
void addBlit(std::vector<VkImageBlit>& out,std::int32_t sx0,std::int32_t sy0,std::int32_t sx1,std::int32_t sy1,std::int32_t dx0,std::int32_t dy0,std::int32_t dx1,std::int32_t dy1) {
    if(sx1<=sx0||sy1<=sy0||dx1<=dx0||dy1<=dy0)return;
    VkImageBlit b{};b.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};b.dstSubresource=b.srcSubresource;
    b.srcOffsets[0]={sx0,sy0,0};b.srcOffsets[1]={sx1,sy1,1};b.dstOffsets[0]={dx0,dy0,0};b.dstOffsets[1]={dx1,dy1,1};out.push_back(b);
}
// 1:1 blits of the whole W x H image except the hole rects (x0,y0,x1,y1): horizontal slabs at every hole edge, minus the covered x runs.
void complementBlits(std::vector<VkImageBlit>& out,std::int32_t W,std::int32_t H,const std::vector<std::array<std::int32_t,4>>& holes) {
    std::vector<std::int32_t> ys{0,H};
    for(const auto& h:holes){ys.push_back(std::clamp(h[1],0,H));ys.push_back(std::clamp(h[3],0,H));}
    std::sort(ys.begin(),ys.end());ys.erase(std::unique(ys.begin(),ys.end()),ys.end());
    for(std::size_t k=0;k+1<ys.size();++k){
        const std::int32_t ya=ys[k],yb=ys[k+1];if(yb<=ya)continue;
        std::vector<std::pair<std::int32_t,std::int32_t>> xs;
        for(const auto& h:holes)if(h[1]<yb&&h[3]>ya&&h[2]>h[0])xs.push_back({std::clamp(h[0],0,W),std::clamp(h[2],0,W)});
        std::sort(xs.begin(),xs.end());std::int32_t x=0;
        for(const auto& iv:xs){if(iv.first>x)addBlit(out,x,ya,iv.first,yb,x,ya,iv.first,yb);x=std::max(x,iv.second);}
        if(x<W)addBlit(out,x,ya,W,yb,x,ya,W,yb);
    }
}
// rotation matrix (columns = local x, y, z in the parent frame) -> quaternion
Q quatFromColumns(const float x[3],const float y[3],const float z[3]) noexcept {
    const float m00=x[0],m11=y[1],m22=z[2],tr=m00+m11+m22;Q q{};
    if(tr>0){const float t=std::sqrt(tr+1.f)*2.f;q.w=.25f*t;q.x=(y[2]-z[1])/t;q.y=(z[0]-x[2])/t;q.z=(x[1]-y[0])/t;}
    else if(m00>m11&&m00>m22){const float t=std::sqrt(1.f+m00-m11-m22)*2.f;q.w=(y[2]-z[1])/t;q.x=.25f*t;q.y=(y[0]+x[1])/t;q.z=(z[0]+x[2])/t;}
    else if(m11>m22){const float t=std::sqrt(1.f+m11-m00-m22)*2.f;q.w=(z[0]-x[2])/t;q.x=(y[0]+x[1])/t;q.y=.25f*t;q.z=(z[1]+y[2])/t;}
    else{const float t=std::sqrt(1.f+m22-m00-m11)*2.f;q.w=(x[1]-y[0])/t;q.x=(z[0]+x[2])/t;q.y=(z[1]+y[2])/t;q.z=.25f*t;}
    const float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);if(n>1e-6f){q.x/=n;q.y/=n;q.z/=n;q.w/=n;}else q={0,0,0,1};
    return q;
}

// ---- AMMO ON GUN (the Vice City VR wrist AMMO panel idea: the counter lies along the gun in the holding hand) ----
// Main thread noteGun() -> XR thread: the held weapon's barrel in LOCAL + its engine clip / pool. The consumer draws "clip/pool" with a
// 5x7 pixel font into a device buffer (vkCmdUpdateBuffer, only when the text changes), copies it into the ammo strip of the HUD swapchain
// in the same consume command buffer and places that strip as a quad beside the barrel.
struct GunState {float muzzle[3]{},dir[3]{},up[3]{},base[3]{};int hand{-1};bool firearm{};int clip{-1},pool{-1};bool infinite{};std::uint64_t ns{};};
std::mutex gunMutex;GunState gun;
GunState gunSnapshot() noexcept {std::lock_guard lock(gunMutex);return gun;}
bool ammoWanted(unsigned m) noexcept {return m!=Off&&settings().hudAmmo();}
// 5x7 glyphs, bit 4 = left column: '0'..'9', '/'
constexpr std::uint8_t ammoGlyphs[11][7]{
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},{0x01,0x01,0x02,0x04,0x08,0x10,0x10}};
// "clip/pool" ("clip" alone when the pool is unlimited / unknown), centred, premultiplied: cream ink (red at 0) on a dark outline.
bool drawAmmoText(int clip,int pool,bool infinite,bool bgra) noexcept {
    auto& px=consumer.ammoPixels;px.fill(0u);
    if(clip<0)return false;
    char text[16]{};
    const int n=infinite||pool<0?std::snprintf(text,sizeof text,"%d",std::min(clip,9999)):std::snprintf(text,sizeof text,"%d/%d",std::min(clip,9999),std::min(pool,9999));
    if(n<=0||n>=int(sizeof text))return false;
    const int s=std::max(1,std::min((int(ammoH)-4)/9,(int(ammoW)-4)/(6*n)));
    const int x0=(int(ammoW)-(6*n-1)*s)/2,y0=(int(ammoH)-7*s)/2,b=std::max(1,s/3);
    const auto pack=[bgra](unsigned r,unsigned g,unsigned bl,unsigned a){
        return bgra?std::uint32_t(bl|g<<8|r<<16|a<<24):std::uint32_t(r|g<<8|bl<<16|a<<24);};
    const std::uint32_t edge=pack(0,0,0,190),ink=clip==0?pack(235,80,64,255):pack(244,236,214,255);
    for(int pass=0;pass<2;++pass)for(int i=0;i<n;++i){
        const char ch=text[i];const int gi=ch=='/'?10:(ch>='0'&&ch<='9'?ch-'0':-1);if(gi<0)continue;
        const int e=pass==0?b:0;const std::uint32_t colour=pass==0?edge:ink;
        for(int row=0;row<7;++row)for(int col=0;col<5;++col){
            if(!((ammoGlyphs[gi][row]>>(4-col))&1u))continue;
            const int gx=x0+(i*6+col)*s,gy=y0+row*s;
            for(int y=std::max(0,gy-e);y<std::min(int(ammoH),gy+s+e);++y)
                for(int x=std::max(0,gx-e);x<std::min(int(ammoW),gx+s+e);++x)px[std::size_t(y)*ammoW+std::size_t(x)]=colour;
        }
    }
    return true;
}
void destroyAmmoBuffer(VkDevice d) noexcept {
    if(d&&consumer.ammoBuffer)vkDestroyBuffer(d,consumer.ammoBuffer,nullptr);
    if(d&&consumer.ammoMemory)vkFreeMemory(d,consumer.ammoMemory,nullptr);
    consumer.ammoBuffer={};consumer.ammoMemory={};consumer.ammoKey=INT64_MIN;
}
// XR thread. A failure only turns AMMO ON GUN off (the HUD stays).
bool ensureAmmoBuffer() noexcept {
    if(consumer.ammoBuffer)return true;
    if(consumer.ammoFailed||!shared.device)return false;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=VkDeviceSize(ammoW)*ammoH*4u;
    bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    if(vkCreateBuffer(shared.device,&bi,nullptr,&consumer.ammoBuffer)!=VK_SUCCESS){consumer.ammoBuffer={};consumer.ammoFailed=true;return false;}
    VkMemoryRequirements r{};vkGetBufferMemoryRequirements(shared.device,consumer.ammoBuffer,&r);
    const auto type=memoryType(shared.physical,r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=r.size;a.memoryTypeIndex=type;
    VkDeviceMemory memory{};const bool allocated=type!=UINT32_MAX&&vkAllocateMemory(shared.device,&a,nullptr,&memory)==VK_SUCCESS;
    if(allocated)consumer.ammoMemory=memory;
    if(!allocated||vkBindBufferMemory(shared.device,consumer.ammoBuffer,consumer.ammoMemory,0)!=VK_SUCCESS){
        destroyAmmoBuffer(shared.device);consumer.ammoFailed=true;return false;}
    consumer.ammoKey=INT64_MIN;return true;
}
// The ammo quad: beside the held firearm on the OUTER side of the holding hand (right of a right-hand gun), lying along the barrel - read
// across the gun with its top towards the muzzle - turned about the barrel to the eyes (ammoface) and tilted towards them (ammotilt %).
// Hidden without a fresh (250 ms) firearm frame.
void ammoLayer(XrSpace local,const float eye[3],std::uint64_t now,Layers& out) noexcept {
    if(!consumer.ammoDrawn||!consumer.swapchain||consumer.ammoRect[2]<=0||consumer.ammoRect[3]<=0)return;
    const GunState g=gunSnapshot();
    if(!g.ns||!g.firearm||g.hand<0||g.hand>1||(now>g.ns&&now-g.ns>250'000'000ull)){stats.ammoHidden.fetch_add(1,std::memory_order_relaxed);return;}
    float d[3]{g.dir[0],g.dir[1],g.dir[2]};if(!normalize3(d,.5f))return;
    const float ud=dot3(g.up,d);float u[3]{g.up[0]-d[0]*ud,g.up[1]-d[1]*ud,g.up[2]-d[2]*ud};if(!normalize3(u,.2f))return;
    const float rt[3]{d[1]*u[2]-d[2]*u[1],d[2]*u[0]-d[0]*u[2],d[0]*u[1]-d[1]*u[0]}; // gun right = barrel x up
    const float side=g.hand==0?-1.f:1.f;
    const float along=float(props.ammoPlace[0].load(std::memory_order_relaxed))/1000.f,across=float(props.ammoPlace[1].load(std::memory_order_relaxed))/1000.f;
    const float lift=float(props.ammoPlace[2].load(std::memory_order_relaxed))/1000.f,size=float(std::clamp(props.ammoPlace[3].load(std::memory_order_relaxed),20,200))/1000.f;
    float c[3];for(unsigned i=0;i<3;++i)c[i]=g.base[i]+d[i]*along+rt[i]*side*across+u[i]*lift;
    if(!finite3(c))return;
    float te[3]{eye[0]-c[0],eye[1]-c[1],eye[2]-c[2]};if(!normalize3(te,.02f))return;
    // face 1 = upright beside the gun -
    // on the INNER side of the bore near the grip (along / across / lift from the bore point level with the model origin), the quad
    // facing back along the barrel (to the shooter), image up = WORLD up across the barrel: it never rolls with the gun; face 2 = the strip along the gun below
    if(props.ammoFace.load(std::memory_order_relaxed)!=2){
        float cc[3];for(unsigned i=0;i<3;++i)cc[i]=g.base[i]+d[i]*along-rt[i]*side*across+u[i]*lift;
        if(!finite3(cc))return;
        const float zz[3]{-d[0],-d[1],-d[2]};
        float y[3]{-zz[1]*zz[0],1.f-zz[1]*zz[1],-zz[1]*zz[2]};if(!normalize3(y,.1f)){for(unsigned i=0;i<3;++i)y[i]=u[i];}
        const float x[3]{y[1]*zz[2]-y[2]*zz[1],y[2]*zz[0]-y[0]*zz[2],y[0]*zz[1]-y[1]*zz[0]};
        const Q o=quatFromColumns(x,y,zz);for(unsigned i=0;i<3;++i)c[i]=cc[i];
        auto& q=out.ammo;q=XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;q.space=local;q.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain=consumer.swapchain;q.subImage.imageRect={{consumer.ammoRect[0],consumer.ammoRect[1]},{consumer.ammoRect[2],consumer.ammoRect[3]}};
        q.pose.orientation={o.x,o.y,o.z,o.w};q.pose.position={c[0],c[1],c[2]};
        q.size={size,size*float(consumer.ammoRect[3])/float(consumer.ammoRect[2])};
        out.ammoValid=true;stats.ammoLayers.fetch_add(1,std::memory_order_relaxed);return;}
    float z[3]{u[0],u[1],u[2]};
    {const float k=dot3(te,d);float ep[3]{te[0]-d[0]*k,te[1]-d[1]*k,te[2]-d[2]*k};
        if(normalize3(ep,.1f))for(unsigned i=0;i<3;++i)z[i]=ep[i];}
    {const float t=float(std::clamp(props.ammoTilt.load(std::memory_order_relaxed),0,100))/100.f;
     float z2[3]{z[0]+(te[0]-z[0])*t,z[1]+(te[1]-z[1])*t,z[2]+(te[2]-z[2])*t};if(normalize3(z2,.1f))for(unsigned i=0;i<3;++i)z[i]=z2[i];}
    if(dot3(z,te)<0.f)for(float& v:z)v=-v; // seen from below (ammoface 0, gun rolled over): 180 deg about the barrel, still readable
    const float dz=dot3(d,z);float y[3]{d[0]-z[0]*dz,d[1]-z[1]*dz,d[2]-z[2]*dz};if(!normalize3(y,.1f))return; // image up = towards the muzzle
    const float x[3]{y[1]*z[2]-y[2]*z[1],y[2]*z[0]-y[0]*z[2],y[0]*z[1]-y[1]*z[0]};                                 // image right = y x z
    const Q o=quatFromColumns(x,y,z);
    auto& q=out.ammo;q=XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;q.space=local;q.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain=consumer.swapchain;q.subImage.imageRect={{consumer.ammoRect[0],consumer.ammoRect[1]},{consumer.ammoRect[2],consumer.ammoRect[3]}};
    q.pose.orientation={o.x,o.y,o.z,o.w};q.pose.position={c[0],c[1],c[2]};
    q.size={size,size*float(consumer.ammoRect[3])/float(consumer.ammoRect[2])};
    out.ammoValid=true;stats.ammoLayers.fetch_add(1,std::memory_order_relaxed);
}
} // namespace

const char* stateName(State s) noexcept {
    switch(s){case State::Killed:return "KILLED";case State::Idle:return "IDLE";case State::Ready:return "READY";case State::Failed:return "FAILED";default:return "OFF";}
}
State state() noexcept {return State(stateValue.load(std::memory_order_acquire));}
unsigned mode() noexcept {
    if(killed(Feature::Hud)||state()==State::Failed)return Off;
    if(const int f=props.forceMode.load(std::memory_order_relaxed);f==1||f==2)return unsigned(f);
    return settings().hudMode();
}
bool subtitlesWanted() noexcept {
    return mode()!=Off&&!settings().subtitlesOff()&&props.subtitles.load(std::memory_order_relaxed)!=0;
}
void bind(VkPhysicalDevice physical,VkDevice device,std::uint32_t family) noexcept {
    readProps();
    if(killed(Feature::Hud)){setState(State::Killed);status::publish(Feature::Hud,status::Property,nullptr);return;}
    std::lock_guard lock(shared.mutex);
    if(shared.device&&shared.device!=device)return; // one game device per process
    shared.physical=physical;shared.device=device;shared.family=family;
    if(state()!=State::Failed){setState(State::Idle);status::publish(Feature::Hud,status::Ready,nullptr);}
}
void skip(Skip why) noexcept {
    const unsigned i=unsigned(why);if(i>=unsigned(Skip::Count))return;
    stats.skips[i].fetch_add(1,std::memory_order_relaxed);stats.lastSkip.store(skipNames[i],std::memory_order_relaxed);
}
bool wanted(std::uint64_t renderFrameId) noexcept {
    if(producer.armed)cancel();
    if(!shared.device||mode()==Off)return false;
    if(!clearAlphaHooked()){skip(Skip::Apis);return false;}
    const unsigned every=unsigned(std::max(1,props.every.load(std::memory_order_relaxed)));
    if(every>1&&(producer.counter++%every)!=0)return false;
    stats.wanted.fetch_add(1,std::memory_order_relaxed);(void)renderFrameId;return true;
}
void begun(const Source& s) noexcept {
    producer.active=s;producer.armed=true;stats.begun.fetch_add(1,std::memory_order_relaxed);
    stats.lastSourceW.store(s.extent.width,std::memory_order_relaxed);stats.lastSourceH.store(s.extent.height,std::memory_order_relaxed);
    stats.lastFormat.store(std::uint64_t(s.format),std::memory_order_relaxed);
}
bool active() noexcept {return producer.armed;}
const Source& activeSource() noexcept {return producer.active;}
void recorded(VkCommandBuffer command) noexcept {
    if(!producer.armed)return;
    producer.armed=false;if(!command){stats.cancelled.fetch_add(1,std::memory_order_relaxed);return;}
    if(producer.hasPending)stats.pendingMissed.fetch_add(1,std::memory_order_relaxed); // the previous HUD frame was never seen submitted
    producer.pending=producer.active;producer.pendingCommand=command;producer.hasPending=true;producer.clearAlpha=true;stats.recorded.fetch_add(1,std::memory_order_relaxed);
}
void cancel() noexcept {if(producer.armed){producer.armed=false;stats.cancelled.fetch_add(1,std::memory_order_relaxed);}alphaEnd();}
void submitted(VkQueue queue,std::uint32_t count,const VkSubmitInfo* submits,VkResult result,LayoutReader readLayout) noexcept {
    if(!producer.hasPending||!queue)return;
    PreserveErrno keep;
    bool found=false;
    for(std::uint32_t i=0;i<count&&submits&&!found;++i)for(std::uint32_t j=0;j<submits[i].commandBufferCount&&!found;++j)
        found=submits[i].pCommandBuffers&&submits[i].pCommandBuffers[j]==producer.pendingCommand;
    if(!found)return; // a different submission of this frame; the HUD command buffer follows
    producer.hasPending=false;
    if(result!=VK_SUCCESS||mode()==Off){stats.pendingMissed.fetch_add(1,std::memory_order_relaxed);return;}
    const auto t0=nowNs();
    const Source s=producer.pending;VkImageLayout layout{};
    if(!acceptedFormat(s.format)){skip(Skip::Format);return;}
    if(!readLayout||!readLayout(s.texture,s.image,s.generation,layout)||!acceptedLayout(layout)){skip(Skip::Layout);return;}
    stats.lastLayout.store(std::uint64_t(layout),std::memory_order_relaxed);
    std::unique_lock lock(shared.mutex,std::try_to_lock);
    if(!lock.owns_lock()){skip(Skip::Lock);return;}
    if(!shared.device||state()==State::Failed)return;
    const int scale=std::clamp(props.scale.load(std::memory_order_relaxed),25,100);
    const VkExtent2D ext{std::max(16u,std::uint32_t(s.extent.width*unsigned(scale)/100u)),std::max(16u,std::uint32_t(s.extent.height*unsigned(scale)/100u))};
    if(!ensureRing(ext,s.format))return;
    finishDump(); // A requested dump whose copy finished is written before its slot can be reused
    // Oldest slot that no copy writes and no consumer reads; never wait.
    int pick=-1;
    for(unsigned i=0;i<slotCount;++i){auto& x=shared.slots[i];
        if(x.reading&&fenceDone(shared.device,x.consumeFence))x.reading=false;
        if(x.reading||(x.produceSubmitted&&!fenceDone(shared.device,x.produceFence)))continue;
        if(pick<0||x.sequence<shared.slots[unsigned(pick)].sequence)pick=int(i);}
    if(pick<0){skip(Skip::Busy);return;}
    auto& slot=shared.slots[unsigned(pick)];
    VkCommandBuffer c=slot.produce;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(vkResetCommandBuffer(c,0)!=VK_SUCCESS||vkBeginCommandBuffer(c,&bi)!=VK_SUCCESS){fail("PRODUCE_BEGIN");return;}
    // Source: the engine-tracked layout at the end of the submitted command buffer -> TRANSFER_SRC -> back (the engine's
    // texture+0xa0 stays true). Ring slot: UNDEFINED (whole image overwritten) -> TRANSFER_DST -> TRANSFER_SRC (consumer layout).
    barrier(c,s.image,layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(c,slot.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit b{};b.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};b.dstSubresource=b.srcSubresource;
    b.srcOffsets[1]={std::int32_t(s.extent.width),std::int32_t(s.extent.height),1};b.dstOffsets[1]={std::int32_t(ext.width),std::int32_t(ext.height),1};
    vkCmdBlitImage(c,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&b,
                   shared.linear&&(ext.width!=s.extent.width||ext.height!=s.extent.height)?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
    barrier(c,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,layout,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier(c,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    { // Dump request (debug.rdrvr.vr.hud.dump changed): this slot -> host buffer, written by a later submitted() once its fence is done
        const int req=props.dump.load(std::memory_order_relaxed);
        if(req&&req!=dump.lastRequest&&dump.slot<0){
            dump.lastRequest=req;
            if(ensureDumpBuffer(VkDeviceSize(ext.width)*ext.height*4u)){
                VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={ext.width,ext.height,1};
                vkCmdCopyImageToBuffer(c,slot.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dump.buffer,1,&region);
                VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};hb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;hb.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
                hb.srcQueueFamilyIndex=hb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;hb.buffer=dump.buffer;hb.size=VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&hb,0,nullptr);
                dump.slot=pick;dump.w=ext.width;dump.h=ext.height;dump.format=s.format;
            }else stats.dumpFailures.fetch_add(1,std::memory_order_relaxed);
        }
    }
    if(vkEndCommandBuffer(c)!=VK_SUCCESS){fail("PRODUCE_END");return;}
    if(vkResetFences(shared.device,1,&slot.produceFence)!=VK_SUCCESS){fail("PRODUCE_FENCE");return;}
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&c;
    // The caller is inside the game's vkQueueSubmit hook on this queue: queue external synchronization is already held.
    if(vkQueueSubmit(queue,1,&si,slot.produceFence)!=VK_SUCCESS){slot.produceSubmitted=false;if(dump.slot==pick)dump.slot=-1;fail("PRODUCE_SUBMIT");return;}
    slot.produceSubmitted=true;slot.sequence=++shared.sequence;slot.frame=s.frame;slot.publishedNs=nowNs();slot.subtitles=s.subtitles;
    if(s.subtitles)stats.subtitleFrames.fetch_add(1,std::memory_order_relaxed);
    stats.published.fetch_add(1,std::memory_order_relaxed);stats.lastPublishedNs.store(slot.publishedNs,std::memory_order_relaxed);
    stats.lastFrame.store(s.frame,std::memory_order_relaxed);
    if(state()==State::Idle)setState(State::Ready);
    stats.submitCpuNs.fetch_add(nowNs()-t0,std::memory_order_relaxed);stats.submitCalls.fetch_add(1,std::memory_order_relaxed);
}

void setClearAlphaHooked(bool v) noexcept {clearAlphaHookedFlag.store(v,std::memory_order_release);if(v)clearAlphaFingerprint.store(1,std::memory_order_relaxed);}
bool clearAlphaHooked() noexcept {return clearAlphaHookedFlag.load(std::memory_order_acquire);}
void noteClearAlphaFingerprint(bool good) noexcept {clearAlphaFingerprint.store(good?1:0,std::memory_order_relaxed);}
bool takeClearAlpha() noexcept {
    if(!producer.hasPending||!producer.clearAlpha)return false;
    producer.clearAlpha=false;stats.clearAlphaTaken.fetch_add(1,std::memory_order_relaxed);return true;
}
void clearAlphaDone(std::uint64_t dropped) noexcept {
    if(dropped){stats.clearAlphaDropped.fetch_add(1,std::memory_order_relaxed);return;}
    if(producer.hasPending){producer.hasPending=false;skip(Skip::Alpha);} // alpha = 1 everywhere: an opaque quad, never shown
}
void configureAlpha(AlphaApi api) noexcept {alphaApi=api;}
bool alphaWanted() noexcept {
    return props.alpha.load(std::memory_order_relaxed)==1&&alphaApi.boundFramebuffer&&alphaApi.invoke;}
void alphaBegin() noexcept {
    if(alphaThread||!producer.armed||!alphaWanted())return;
    alphaThread=true;alphaLiveCount.fetch_add(1,std::memory_order_relaxed);
}
void alphaEnd() noexcept {if(!alphaThread)return;alphaThread=false;alphaLiveCount.fetch_sub(1,std::memory_order_relaxed);}
void alphaFlush(void (*original)(),bool colorDraw) {
    if(!alphaThread||!colorDraw||!producer.armed){original();return;}
    const int incoming=errno;std::uintptr_t bound{};const bool readable=alphaApi.boundFramebuffer(bound);errno=incoming;
    if(!readable){stats.alphaReadFailures.fetch_add(1,std::memory_order_relaxed);original();return;}
    if(bound!=producer.active.framebuffer){stats.alphaTemporary.fetch_add(1,std::memory_order_relaxed);original();return;}
    if(alphaApi.invoke(original))stats.alphaNormalized.fetch_add(1,std::memory_order_relaxed);
    else stats.alphaUnsupported.fetch_add(1,std::memory_order_relaxed);
}

bool xrFrame(XrSession session,VkQueue queue,std::uint32_t queueFamily,XrSpace local,const Pose views[2],const Pose& leftGrip,
             std::uint64_t now,Layers& out) noexcept {
    PreserveErrno keep;
    out.headValid=out.wristValid=out.mapValid=out.ammoValid=false;
    if((consumer.frames++&63)==0)readProps();
    stats.xrFrames.fetch_add(1,std::memory_order_relaxed);
    const unsigned m=mode();
    if(m==Off||!session||!queue||!views||!views[0].valid||!views[1].valid)return false;
    try{
        // ---- newest finished ring slot -> XR swapchain (never waits for the game GPU) ----
        int pick=-1;VkExtent2D ext{};VkFormat format{};std::uint64_t generation{};
        {
            std::lock_guard lock(shared.mutex);
            if(!shared.device||!shared.slots[0].image)return false;
            if(!shared.consumePool||shared.consumeFamily!=queueFamily){
                if(shared.consumePool)return false; // family changed: never expected (one XR binding queue)
                VkCommandPoolCreateInfo p{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};p.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;p.queueFamilyIndex=queueFamily;
                if(vkCreateCommandPool(shared.device,&p,nullptr,&shared.consumePool)!=VK_SUCCESS){shared.consumePool={};fail("CONSUME_POOL");return false;}
                shared.consumeFamily=queueFamily;
                for(auto& s:shared.slots){
                    VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};a.commandPool=shared.consumePool;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;
                    VkFenceCreateInfo f{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                    if(vkAllocateCommandBuffers(shared.device,&a,&s.consume)!=VK_SUCCESS||vkCreateFence(shared.device,&f,nullptr,&s.consumeFence)!=VK_SUCCESS){fail("CONSUME_OBJECTS");return false;}
                }
            }
            for(unsigned i=0;i<slotCount;++i){auto& s=shared.slots[i];
                if(s.reading&&fenceDone(shared.device,s.consumeFence))s.reading=false;}
            const bool sameGeneration=consumer.generation==shared.generation&&consumer.swapchain;
            for(unsigned i=0;i<slotCount;++i){const auto& s=shared.slots[i];
                if(s.reading||!s.produceSubmitted||!s.sequence||(sameGeneration&&s.sequence<=consumer.displayedSequence))continue;
                if(!fenceDone(shared.device,s.produceFence))continue;
                if(pick<0||s.sequence>shared.slots[unsigned(pick)].sequence)pick=int(i);}
            // Every consumer copy of a slot is finished before the slot is read again (its fence is reset only here).
            if(pick>=0){auto& s=shared.slots[unsigned(pick)];
                if(vkResetFences(shared.device,1,&s.consumeFence)!=VK_SUCCESS){fail("CONSUME_FENCE");return false;}
                s.reading=true;}
            ext=shared.extent;format=shared.format;generation=shared.generation;
        }
        if(pick>=0){
            auto& s=shared.slots[unsigned(pick)]; // reading=true: the producer neither writes nor rebuilds this slot
            ensureSwapchain(session,ext,format,generation);
            std::uint32_t index{};XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            runtime::checkXr(xrAcquireSwapchainImage(consumer.swapchain,&ai,&index),"hud acquire");
            if(index>=consumer.images.size())throw std::runtime_error("hud image index");
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wi.timeout=XR_INFINITE_DURATION;
            const auto waited=xrWaitSwapchainImage(consumer.swapchain,&wi);
            if(waited!=XR_SUCCESS&&waited!=XR_SESSION_LOSS_PENDING)throw std::runtime_error("hud wait");
            const VkImage target=consumer.images[index].image;VkCommandBuffer c=s.consume;
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            runtime::checkVk(vkResetCommandBuffer(c,0),"hud consume reset");runtime::checkVk(vkBeginCommandBuffer(c,&bi),"hud consume begin");
            // The slot copy finished on the game queue (fence observed on the CPU); make its writes visible to this queue's reads.
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};mb.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;mb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&mb,0,nullptr,0,nullptr);
            barrier(c,target,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            const auto W=std::int32_t(ext.width),H=std::int32_t(ext.height);
            std::vector<VkImageBlit> head,wrist;
            const auto r=radarRect(ext.width,ext.height);consumer.radarPx=r;
            for(unsigned i=0;i<4;++i)stats.radar[i].store(r[i],std::memory_order_relaxed);
            const auto S=std::int32_t(consumer.strip);
            // mapfix: the misplaced minimap interior (src) is cut out of the head image and scaled into the map square [W+S, W+2S)
            std::array<std::int32_t,4> ms{};bool mapfix=props.mapfix[0].load(std::memory_order_relaxed)>=0;
            if(mapfix){
                auto px=[](int pm,std::int32_t size){return std::int32_t(std::clamp<long>(std::lround(double(pm)*double(size)/1000.0),0,long(size)));};
                ms={px(props.mapfix[0].load(),W),px(props.mapfix[1].load(),H),px(props.mapfix[2].load(),W),px(props.mapfix[3].load(),H)};
                for(unsigned i=0;i<4;++i)consumer.mapDst[i]=float(std::clamp(props.mapfix[4+i].load(),0,1000))/1000.f;
                mapfix=ms[2]-ms[0]>=4&&ms[3]-ms[1]>=4;
            }
            const bool subs=s.subtitles;
            if(subs)mapfix=false;
            // AMMO ON GUN, crop source (debug.rdrvr.vr.hud.ammocrop): the stock ammo element is cut out of the head image and scaled into
            // the ammo strip; else the strip gets the clip / pool digits after the blits
            const bool ammoOn=ammoWanted(m)&&!subs;std::array<std::int32_t,4> ac{};
            bool ammoCrop=ammoOn&&props.ammoCrop[0].load(std::memory_order_relaxed)>=0;
            if(ammoCrop){
                auto px=[](int pm,std::int32_t size){return std::int32_t(std::clamp<long>(std::lround(double(pm)*double(size)/1000.0),0,long(size)));};
                ac={px(props.ammoCrop[0].load(),W),px(props.ammoCrop[1].load(),H),px(props.ammoCrop[2].load(),W),px(props.ammoCrop[3].load(),H)};
                ammoCrop=ac[2]-ac[0]>=4&&ac[3]-ac[1]>=4;
            }
            consumer.ammoDrawn=false;
            std::vector<std::array<std::int32_t,4>> holes;
            if(m==Hand)holes.push_back(r);
            if(mapfix)holes.push_back(ms);
            if(ammoCrop)holes.push_back(ac);
            // PANEL: the head image is the Flash area only; the radar goes into the square [W, W+S) in HEAD and HAND
            const int fw=props.flash[0].load(std::memory_order_relaxed),fh=props.flash[1].load(std::memory_order_relaxed);
            const bool panel=fw>=100&&fh>=100&&!mapfix&&!subs;
            if(subs){ // SUBTITLES: only the band, 1:1 into the top-left of the swapchain (the head quad shows that rect)
                auto px=[](int pm,std::int32_t size){return std::int32_t(std::clamp<long>(std::lround(double(pm)*double(size)/1000.0),0,long(size)));};
                std::int32_t b0=px(props.subBand[0].load(),W),b1=px(props.subBand[1].load(),H),b2=px(props.subBand[2].load(),W),b3=px(props.subBand[3].load(),H);
                if(b2-b0<16||b3-b1<8){b0=0;b1=0;b2=W;b3=H;}
                addBlit(head,b0,b1,b2,b3,0,0,b2-b0,b3-b1);
                consumer.panelW=std::uint32_t(b2-b0);consumer.panelH=std::uint32_t(b3-b1);consumer.panelRadar=false;
            }
            else if(panel){
                const std::int32_t pw=std::clamp<std::int32_t>(std::int32_t(std::lround(double(fw)*W/1000.0)),16,W),ph=std::clamp<std::int32_t>(std::int32_t(std::lround(double(fh)*H/1000.0)),16,H);
                consumer.panelW=std::uint32_t(pw);consumer.panelH=std::uint32_t(ph);
                holes.clear();
                // radar rect pixels inside the Flash area would show twice: cut them (normally the radar lies below the Flash area)
                if(r[1]<ph&&r[0]<pw)holes.push_back({r[0],r[1],std::min(r[2],pw),std::min(r[3],ph)});
                if(ammoCrop&&ac[1]<ph&&ac[0]<pw)holes.push_back({ac[0],ac[1],std::min(ac[2],pw),std::min(ac[3],ph)}); // shown on the gun instead
                if(!holes.empty()){
                    VkClearColorValue zero{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                    vkCmdClearColorImage(c,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&range);
                    VkMemoryBarrier cb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};cb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;cb.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&cb,0,nullptr,0,nullptr);
                    complementBlits(head,pw,ph,holes);
                }else addBlit(head,0,0,pw,ph,0,0,pw,ph);
                addBlit(wrist,r[0],r[1],r[2],r[3],W,0,W+S,S);
                consumer.panelRadar=true;
            }else{consumer.panelW=std::uint32_t(W);consumer.panelH=std::uint32_t(H);consumer.panelRadar=false;}
            if(panel||subs){}
            else if(!holes.empty()){
                // Head part without the holes: transparent everywhere, then the complement bands (zero extra rendering).
                VkClearColorValue zero{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                vkCmdClearColorImage(c,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&range);
                VkMemoryBarrier cb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};cb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;cb.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&cb,0,nullptr,0,nullptr);
                complementBlits(head,W,H,holes);
            }else addBlit(head,0,0,W,H,0,0,W,H);
            if(m==Hand&&!panel&&!subs)addBlit(wrist,r[0],r[1],r[2],r[3],W,0,W+S,S);
            if(mapfix)addBlit(wrist,ms[0],ms[1],ms[2],ms[3],W+S,0,W+2*S,S); // same scaled blit call (LINEAR)
            if(ammoCrop){ // the cropped stock ammo element into the ammo strip (aspect kept, top-left)
                const double k=std::min(double(ammoW)/double(ac[2]-ac[0]),double(ammoH)/double(ac[3]-ac[1]));
                const std::int32_t dw=std::clamp<std::int32_t>(std::int32_t(std::lround(double(ac[2]-ac[0])*k)),1,std::int32_t(ammoW));
                const std::int32_t dh=std::clamp<std::int32_t>(std::int32_t(std::lround(double(ac[3]-ac[1])*k)),1,std::int32_t(ammoH));
                const auto ax=std::int32_t(consumer.ammoX),ay=std::int32_t(consumer.ammoY);
                addBlit(wrist,ac[0],ac[1],ac[2],ac[3],ax,ay,ax+dw,ay+dh);
                consumer.ammoRect={ax,ay,dw,dh};consumer.ammoDrawn=true;
            }
            consumer.mapShown=mapfix;consumer.subtitles=subs;
            if(!head.empty())vkCmdBlitImage(c,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,std::uint32_t(head.size()),head.data(),VK_FILTER_NEAREST);
            if(!wrist.empty())vkCmdBlitImage(c,s.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,std::uint32_t(wrist.size()),wrist.data(),
                                              shared.linear?VK_FILTER_LINEAR:VK_FILTER_NEAREST);
            if(ammoOn&&!ammoCrop&&ensureAmmoBuffer()){ // AMMO ON GUN digits: buffer (rewritten only when the text changed) -> the ammo strip
                const GunState g=gunSnapshot();
                const bool bgra=consumer.format==VK_FORMAT_B8G8R8A8_SRGB||consumer.format==VK_FORMAT_B8G8R8A8_UNORM;
                const int clip=g.firearm?std::clamp(g.clip,-1,9999):-1,pool=g.infinite?-1:std::clamp(g.pool,-1,9999);
                const std::int64_t key=(std::int64_t(clip+1)*10001+std::int64_t(pool+1))*2+(bgra?1:0);
                if(key!=consumer.ammoKey){
                    // WAR against the previous consume copy: covered by the ALL_COMMANDS barrier at the start of this command buffer
                    consumer.ammoText=drawAmmoText(clip,pool,g.infinite,bgra);
                    vkCmdUpdateBuffer(c,consumer.ammoBuffer,0,VkDeviceSize(ammoW)*ammoH*4u,consumer.ammoPixels.data());
                    VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};bb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;bb.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
                    bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;bb.buffer=consumer.ammoBuffer;bb.size=VK_WHOLE_SIZE;
                    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&bb,0,nullptr);
                    consumer.ammoKey=key;stats.ammoUploads.fetch_add(1,std::memory_order_relaxed);
                }
                if(consumer.ammoText){
                    VkMemoryBarrier wb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};wb.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;wb.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                    vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&wb,0,nullptr,0,nullptr);
                    VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                    region.imageOffset={std::int32_t(consumer.ammoX),std::int32_t(consumer.ammoY),0};region.imageExtent={ammoW,ammoH,1};
                    vkCmdCopyBufferToImage(c,consumer.ammoBuffer,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
                    consumer.ammoRect={std::int32_t(consumer.ammoX),std::int32_t(consumer.ammoY),std::int32_t(ammoW),std::int32_t(ammoH)};
                    consumer.ammoDrawn=true;
                }
            }
            barrier(c,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            runtime::checkVk(vkEndCommandBuffer(c),"hud consume end");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&c;
            runtime::checkVk(vkQueueSubmit(queue,1,&si,s.consumeFence),"hud consume submit");
            // The runtime orders its use after work already submitted to the binding queue; no CPU wait before release.
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};runtime::checkXr(xrReleaseSwapchainImage(consumer.swapchain,&ri),"hud release");
            consumer.displayedSequence=s.sequence;consumer.displayedNs=s.publishedNs;consumer.displayedGeneration=generation;consumer.displayedMode=m;consumer.valid=true;
            stats.consumed.fetch_add(1,std::memory_order_relaxed);
        }
    }catch(const std::exception& e){
        destroySwapchain();fail("XR_COPY_FAILED"); // FAILED latch: a slot left `reading` is never reused (the producer stops too)
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_ERROR,"RdrVrHud","HUD XR copy: %s",e.what());
#endif
        return false;
    }catch(...){destroySwapchain();fail("XR_COPY_FAILED");return false;}
    if(!consumer.valid||!consumer.swapchain)return false;
    const auto maxAge=std::uint64_t(std::max(50,props.maxAge.load(std::memory_order_relaxed)))*1000000ull;
    if(now>consumer.displayedNs&&now-consumer.displayedNs>maxAge){stats.staleHidden.fetch_add(1,std::memory_order_relaxed);return false;}
    // ---- head quad: lazy yaw follow (or head-locked) in LOCAL ----
    const float center[3]{(views[0].p[0]+views[1].p[0])*.5f,(views[0].p[1]+views[1].p[1])*.5f,(views[0].p[2]+views[1].p[2])*.5f};
    const Q head{views[0].q[0],views[0].q[1],views[0].q[2],views[0].q[3]};
    const float fwd0[3]{0,0,-1};float fwd[3];rot(head,fwd0,fwd);
    const float dist=float(std::clamp(props.dist.load(std::memory_order_relaxed),40,500))/100.f;
    const bool subs=consumer.subtitles; // SUBTITLES frame: the band quad lower (subdown), nothing else
    const float down=float(subs?std::clamp(props.subDown.load(std::memory_order_relaxed),-30,60):std::clamp(props.down.load(std::memory_order_relaxed),-30,45))*pi/180.f;
    const int degProp=props.deg.load(std::memory_order_relaxed);
    const float deg=degProp>0?float(degProp):settings().hudWidthDeg();
    const float width=2.f*dist*std::tan(std::clamp(deg,10.f,120.f)*pi/360.f);
    const float headYaw=std::atan2(-fwd[0],-fwd[2]);
    const float dt=consumer.lastNs&&now>consumer.lastNs?std::min(0.1f,float(double(now-consumer.lastNs)*1e-9)):0.f;consumer.lastNs=now;
    Q base{};
    if(props.follow.load(std::memory_order_relaxed)){
        if(!consumer.init||!std::isfinite(consumer.yaw)){consumer.yaw=headYaw;consumer.init=true;consumer.following=false;}
        const float d=wrapAngle(headYaw-consumer.yaw);
        if(std::fabs(d)>pi*0.5f)consumer.yaw=headYaw; // recenter / snap: jump
        else{
            if(std::fabs(d)>20.f*pi/180.f)consumer.following=true;
            if(consumer.following){consumer.yaw=wrapAngle(consumer.yaw+d*std::min(1.f,dt*5.f));if(std::fabs(d)<1.5f*pi/180.f)consumer.following=false;}
        }
        base={0,std::sin(consumer.yaw*.5f),0,std::cos(consumer.yaw*.5f)};
    }else{base=head;consumer.init=false;}
    const Q tilt{std::sin(-down*.5f),0,0,std::cos(-down*.5f)};
    const Q orient=mul(base,tilt);
    const float offset[3]{0,0,-dist};float o[3];rot(orient,offset,o);
    const float hp[3]{center[0]+o[0],center[1]+o[1],center[2]+o[2]};
    if(!finite3(hp)||!std::isfinite(orient.w))return false;
    auto& h=out.head;h=XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    h.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // premultiplied (cleared to 0, native blending)
    h.space=local;h.eyeVisibility=XR_EYE_VISIBILITY_BOTH;h.subImage.swapchain=consumer.swapchain;
    const std::uint32_t pw=consumer.panelW?consumer.panelW:consumer.headW,ph=consumer.panelH?consumer.panelH:consumer.headH;
    h.subImage.imageRect={{0,0},{std::int32_t(pw),std::int32_t(ph)}};
    h.pose.orientation={orient.x,orient.y,orient.z,orient.w};h.pose.position={hp[0],hp[1],hp[2]};
    h.size={width,width*float(ph)/float(std::max(1u,pw))};
    out.headValid=true;stats.headLayers.fetch_add(1,std::memory_order_relaxed);
    if(!subs&&ammoWanted(m))ammoLayer(local,center,now,out); // AMMO ON GUN (HEAD and HAND)
    if(subs){stats.subtitleLayers.fetch_add(1,std::memory_order_relaxed);return true;}
    const float headH=h.size.height;
    // mapfix layer: a quad over the dst rect, 3 mm in front of its parent (head quad in HEAD, wrist radar in HAND)
    auto mapLayer=[&](const Q& parent,const float centre[3],float w,float hgt,float u0,float v0,float u1,float v1){
        const float du=u1-u0,dv=v1-v0;if(!(du>.001f&&dv>.001f))return;
        const auto& d=consumer.mapDst;
        const float cx=((d[0]+d[2])*.5f-u0)/du-.5f,cy=.5f-((d[1]+d[3])*.5f-v0)/dv;
        const float off[3]{cx*w,cy*hgt,.003f};float o2[3];rot(parent,off,o2);
        const float mp[3]{centre[0]+o2[0],centre[1]+o2[1],centre[2]+o2[2]};if(!finite3(mp))return;
        auto& q=out.map;q=XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;q.space=local;q.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain=consumer.swapchain;q.subImage.imageRect={{std::int32_t(consumer.headW+consumer.strip),0},{std::int32_t(consumer.strip),std::int32_t(consumer.strip)}};
        q.pose.orientation={parent.x,parent.y,parent.z,parent.w};q.pose.position={mp[0],mp[1],mp[2]};
        q.size={std::max(.001f,(d[2]-d[0])/du*w),std::max(.001f,(d[3]-d[1])/dv*hgt)};
        out.mapValid=true;stats.mapLayers.fetch_add(1,std::memory_order_relaxed);
    };
    if(consumer.mapShown&&consumer.displayedMode==Head&&m==Head)mapLayer(orient,hp,width,headH,0.f,0.f,1.f,1.f);
    // PANEL: the radar square on the head panel (HEAD), left / bottom / size in per mille of the panel height, 2 mm in front
    if(consumer.panelRadar&&consumer.displayedMode==Head&&m==Head){
        const float L=float(props.radarPanel[0].load(std::memory_order_relaxed))/1000.f*headH,B=float(props.radarPanel[1].load(std::memory_order_relaxed))/1000.f*headH;
        const float sz=std::max(.01f,float(props.radarPanel[2].load(std::memory_order_relaxed))/1000.f*headH);
        const float off[3]{-width*.5f+L+sz*.5f,-headH*.5f+B+sz*.5f,.002f};float o2[3];rot(orient,off,o2);
        const float rp[3]{hp[0]+o2[0],hp[1]+o2[1],hp[2]+o2[2]};
        if(finite3(rp)){
            auto& q=out.map;q=XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;q.space=local;q.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
            q.subImage.swapchain=consumer.swapchain;q.subImage.imageRect={{std::int32_t(consumer.headW),0},{std::int32_t(consumer.strip),std::int32_t(consumer.strip)}};
            q.pose.orientation={orient.x,orient.y,orient.z,orient.w};q.pose.position={rp[0],rp[1],rp[2]};q.size={sz,sz};
            out.mapValid=true;stats.mapLayers.fetch_add(1,std::memory_order_relaxed);
        }
    }
    // ---- wrist quad (HAND): radar square on the back of the LEFT wrist ----
    if(consumer.displayedMode==Hand&&m==Hand){
        if(!leftGrip.valid){stats.noGrip.fetch_add(1,std::memory_order_relaxed);consumer.wristFade=0;return true;}
        const Q g{leftGrip.q[0],leftGrip.q[1],leftGrip.q[2],leftGrip.q[3]};
        // The VrArmIk grip convention (the body's hands sit right on it): hand forward = aim forward in grip (0,-.71,-.71), +X leaves
        // the LEFT palm (back of the left hand = -X); the wrist is 7 cm behind the grip origin along forward and 3 cm towards the back.
        // quad: x (image right) = forward (towards the fingers), normal = -X (out of the back of the hand), y = normal x right.
        // Placement: INNER side of the forearm, centred on it, behind the wrist: lift < 0 = palm side (+X, normal +X), lift >= 0 = back
        // of the hand (-X). The wrist joint centre sits 3 cm towards the back of the hand; the quad is centred on the forearm axis there.
        constexpr float k=.70710678f;
        const float along=float(props.wrist[0].load(std::memory_order_relaxed))/1000.f,lift=float(props.wrist[1].load(std::memory_order_relaxed))/1000.f;
        const float size=float(std::clamp(props.wrist[2].load(std::memory_order_relaxed),30,300))/1000.f;
        const bool palm=lift<0;const float nx=palm?1.f:-1.f;
        const float fx[3]{0,-k,-k},fz[3]{nx,0,0},fy[3]{0,nx*k,-nx*k}; // fy = fz x fx
        const Q wq=mul(g,quatFromColumns(fx,fy,fz));
        // forearm axis = -forward*along - X*0.03 (wrist depth); surface offset |lift| + forearm radius (3 cm) along the normal
        const float off=-.03f+nx*(std::fabs(lift)+.03f);
        float local3[3]{off,k*along,k*along};
        Q wq2=wq;bool onArm=false;
        // quad axes in the grip frame (x = towards the fingers, z = normal) and the skin side normal (before any turn to the eyes)
        float ax[3]{fx[0],fx[1],fx[2]},az[3]{fz[0],fz[1],fz[2]},sideN[3]{fz[0],fz[1],fz[2]};
        float eg[3]; // eye centre in the grip frame
        {const Q gi{-g.x,-g.y,-g.z,g.w};const float ce[3]{center[0]-leftGrip.p[0],center[1]-leftGrip.p[1],center[2]-leftGrip.p[2]};rot(gi,ce,eg);}
        { // Sit on the IK'd body forearm published by VrBody (grip frame), so the map stays on the drawn arm - it twists only
          // by the body's roll share and does not follow wrist flexion. Stale (> 200 ms) / missing -> the rigid controller mapping above.
            const auto fa=forearm::snapshot();
            consumer.diagElbow=fa.elbow;consumer.diagWrist=fa.wrist;consumer.diagAgeMs=fa.ns&&now>=fa.ns?(now-fa.ns)/1000000u:999999u;
            if(props.wristArm.load(std::memory_order_relaxed)&&fa.valid&&fa.ns&&now>=fa.ns&&now-fa.ns<200'000'000ull){
                const float e[3]{fa.elbow[0],fa.elbow[1],fa.elbow[2]},wr[3]{fa.wrist[0],fa.wrist[1],fa.wrist[2]};
                float d[3]{wr[0]-e[0],wr[1]-e[1],wr[2]-e[2]};const float dl=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
                if(dl>.05f&&dl<.6f&&fa.haveRoll){
                    for(float& x:d)x/=dl;
                    // the roll bone axis most perpendicular to the forearm, signed towards the requested side (palm +X / back -X of the
                    // left grip); kept (hysteresis) until it is > 100 deg off that side, so the map stays glued to one spot of the skin
                    const auto& R=fa.roll;const float s0=palm?1.f:-1.f;
                    auto axis=[&](int i,float out[3]){out[0]=R[std::size_t(i)];out[1]=R[std::size_t(3+i)];out[2]=R[std::size_t(6+i)];};
                    auto score=[&](int i){float a[3];axis(i,a);return std::fabs(a[0]*d[0]+a[1]*d[1]+a[2]*d[2]);};
                    int alongAxis=0;for(int i=1;i<3;++i)if(score(i)>score(alongAxis))alongAxis=i;
                    int& pick=consumer.rollAxis;
                    auto sideDot=[&](int code){float a[3];axis(code>>1,a);return (code&1?-1.f:1.f)*a[0]*s0;}; // code = axis*2 + negate
                    if(pick<0||(pick>>1)==alongAxis||sideDot(pick)<-.17f){
                        pick=-1;float best=-2.f;
                        for(int i=0;i<3;++i)if(i!=alongAxis)for(int ng=0;ng<2;++ng){const float v=sideDot(i*2+ng);if(v>best){best=v;pick=i*2+ng;}}
                        consumer.rollPicks++;
                    }
                    float n[3];axis(pick>>1,n);if(pick&1)for(float& x:n)x=-x;
                    for(unsigned i=0;i<3;++i)sideN[i]=n[i];
                    // the quad turns towards the eyes about the forearm axis
                    // (a quad seen from behind / edge-on vanishes); the bone axis only picks the side when the eye lies along the arm.
                    {const float ev[3]{eg[0]-wr[0],eg[1]-wr[1],eg[2]-wr[2]};const float ed=ev[0]*d[0]+ev[1]*d[1]+ev[2]*d[2];
                     float ep[3]{ev[0]-d[0]*ed,ev[1]-d[1]*ed,ev[2]-d[2]*ed};const float el=std::sqrt(ep[0]*ep[0]+ep[1]*ep[1]+ep[2]*ep[2]);
                     if(el>.02f&&props.wristFace.load(std::memory_order_relaxed)){for(unsigned i=0;i<3;++i)n[i]=ep[i]/el;consumer.faceFrames++;}}
                    const float nd=n[0]*d[0]+n[1]*d[1]+n[2]*d[2];
                    float z[3]{n[0]-d[0]*nd,n[1]-d[1]*nd,n[2]-d[2]*nd};const float zl=std::sqrt(z[0]*z[0]+z[1]*z[1]+z[2]*z[2]);
                    if(zl>.2f){
                        for(float& x:z)x/=zl;
                        const float y[3]{z[1]*d[2]-z[2]*d[1],z[2]*d[0]-z[0]*d[2],z[0]*d[1]-z[1]*d[0]};
                        // the quad's near edge at the drawn wrist joint (centre size/2 + 1 cm
                        // behind it); "along" only shifts that (along - 130 mm, default 0)
                        const float back=std::clamp(size*.5f+.01f+(along-.13f),0.f,dl);
                        const float r=.03f+std::fabs(lift);
                        for(unsigned i=0;i<3;++i)local3[i]=wr[i]-d[i]*back+z[i]*r;
                        wq2=mul(g,quatFromColumns(d,y,z));onArm=true;
                        for(unsigned i=0;i<3;++i){ax[i]=d[i];az[i]=z[i];}
                    }
                }
            }
            if(onArm)stats.wristOnArm.fetch_add(1,std::memory_order_relaxed);
            consumer.diagOnArm=onArm;consumer.diagLocal={local3[0],local3[1],local3[2]};
            {const float dx=fa.wrist[0]-fa.elbow[0],dy=fa.wrist[1]-fa.elbow[1],dz=fa.wrist[2]-fa.elbow[2];consumer.diagLen=std::sqrt(dx*dx+dy*dy+dz*dz);}
        }
        // the quad also tilts towards the eyes about its own cross axis
        // (debug.rdrvr.vr.hud.wristtilt % of the remaining angle, default 70; the forearm turn above only rolls it), so it is read
        // nearly face-on while it still lies along the arm (image right stays towards the fingers).
        float facing=1.f,across=0.f;bool facingValid=false;
        {float te[3]{eg[0]-local3[0],eg[1]-local3[1],eg[2]-local3[2]};
         if(normalize3(te,.02f)){
            facing=dot3(sideN,te);across=std::fabs(dot3(ax,te));facingValid=true;
            const float t=float(std::clamp(props.wristTilt.load(std::memory_order_relaxed),0,100))/100.f;
            float z2[3]{az[0]+(te[0]-az[0])*t,az[1]+(te[1]-az[1])*t,az[2]+(te[2]-az[2])*t};
            if(t>0.f&&normalize3(z2,.1f)){
                const float xz=dot3(ax,z2);float x2[3]{ax[0]-z2[0]*xz,ax[1]-z2[1]*xz,ax[2]-z2[2]*xz};
                if(normalize3(x2,.2f)){const float y2[3]{z2[1]*x2[2]-z2[2]*x2[1],z2[2]*x2[0]-z2[0]*x2[2],z2[0]*x2[1]-z2[1]*x2[0]};wq2=mul(g,quatFromColumns(x2,y2,z2));}
            }
         }}
        float w[3];rot(g,local3,w);
        const float wp[3]{leftGrip.p[0]+w[0],leftGrip.p[1]+w[1],leftGrip.p[2]+w[2]};
        const Q wqUse=wq2;
        if(!finite3(wp))return true;
        // Reveal: fades in (~0.1 s) while looked at (gaze cone ~57..44 deg, <= 0.95 m) AND while the wrist is turned towards the face
        // debug.rdrvr.vr.hud.wristfacing 1 (default) = the forearm lies across the view like a
        // watch check (fades out as it points at the eyes, |cos| .6 -> .85: the map edge-on), 2 = the requested skin side (palm /
        // back) faces the eyes, 0 = gaze only.
        float target=1.f;
        if(props.gaze.load(std::memory_order_relaxed)){
            const float v[3]{wp[0]-center[0],wp[1]-center[1],wp[2]-center[2]};
            const float len=std::sqrt(dot3(v,v));const float c=len>0.01f?dot3(v,fwd)/len:1.f;consumer.diagGaze=c;
            target*=smooth(.55f,.72f,c)*(1.f-smooth(.85f,.95f,len));
        }
        const int facingMode=props.wristFacing.load(std::memory_order_relaxed);
        consumer.diagFacing=facingMode==2?facing:across;
        if(facingValid&&facingMode==1)target*=1.f-smooth(.6f,.85f,across);
        else if(facingValid&&facingMode==2)target*=smooth(-.15f,.25f,facing);
        consumer.wristFade+=(target-consumer.wristFade)*std::min(1.f,dt*10.f);
        if(!std::isfinite(consumer.wristFade))consumer.wristFade=0;
        if(consumer.wristFade<.03f){stats.gazeHidden.fetch_add(1,std::memory_order_relaxed);return true;}
        auto& x=out.wrist;x=XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        x.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;x.space=local;x.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
        x.subImage.swapchain=consumer.swapchain;x.subImage.imageRect={{std::int32_t(consumer.headW),0},{std::int32_t(consumer.strip),std::int32_t(consumer.strip)}};
        x.pose.orientation={wqUse.x,wqUse.y,wqUse.z,wqUse.w};x.pose.position={wp[0],wp[1],wp[2]};x.size={size,size};
        out.wristValid=true;stats.wristLayers.fetch_add(1,std::memory_order_relaxed);
        out.wristAlphaScale=float(props.wristAlpha.load(std::memory_order_relaxed))/100.f*std::min(1.f,consumer.wristFade); // x fade (colour scale)
        if(consumer.mapShown){const auto& r=consumer.radarPx;const float W=float(std::max(1u,consumer.headW)),H=float(std::max(1u,consumer.headH));
            mapLayer(wqUse,wp,size,size,float(r[0])/W,float(r[1])/H,float(r[2])/W,float(r[3])/H);}
    }
    return true;
}
void xrSessionEnded() noexcept {destroySwapchain();consumer.session={};consumer.init=false;}
void noteGun(const float map[16],const float muzzle[3],const float dir[3],const float up[3],const float modelOrigin[3],
             const float gripLocal[3],const float aimLocalQ[4],int hand,bool firearm,int clip,int pool,bool infinite) noexcept {
    if(!map||!muzzle||!dir||!up||!modelOrigin||!gripLocal||!aimLocalQ)return;
    // map columns = upm x the LOCAL axes in world (uniform scale): local = cols^T (world - t) / upm^2, a direction cols^T d / upm
    const float s2=map[0]*map[0]+map[1]*map[1]+map[2]*map[2];if(!std::isfinite(s2)||!(s2>1e-8f))return;
    const float s=std::sqrt(s2);
    const auto point=[&](const float w[3],float o[3]){const float v[3]{w[0]-map[12],w[1]-map[13],w[2]-map[14]};
        for(unsigned k=0;k<3;++k)o[k]=(map[4*k]*v[0]+map[4*k+1]*v[1]+map[4*k+2]*v[2])/s2;};
    const auto direction=[&](const float w[3],float o[3]){for(unsigned k=0;k<3;++k)o[k]=(map[4*k]*w[0]+map[4*k+1]*w[1]+map[4*k+2]*w[2])/s;};
    GunState st{};point(muzzle,st.muzzle);direction(dir,st.dir);
    if(!finite3(st.muzzle)||!normalize3(st.dir,.5f))return;
    direction(up,st.up);
    if(!normalize3(st.up,.5f)){const Q a{aimLocalQ[0],aimLocalQ[1],aimLocalQ[2],aimLocalQ[3]};const float y0[3]{0,1,0};rot(a,y0,st.up);} // no locator up
    float ref[3]{gripLocal[0],gripLocal[1],gripLocal[2]};
    if(modelOrigin[0]!=0.f||modelOrigin[1]!=0.f||modelOrigin[2]!=0.f){ // the drawn MODEL origin (as the scope lens), not the controller grip
        float mo[3];point(modelOrigin,mo);const float dm[3]{mo[0]-st.muzzle[0],mo[1]-st.muzzle[1],mo[2]-st.muzzle[2]};
        if(finite3(mo)&&dot3(dm,dm)<1.5f*1.5f)for(unsigned i=0;i<3;++i)ref[i]=mo[i];}
    const float rm[3]{ref[0]-st.muzzle[0],ref[1]-st.muzzle[1],ref[2]-st.muzzle[2]};const float k=dot3(rm,st.dir);
    for(unsigned i=0;i<3;++i)st.base[i]=st.muzzle[i]+st.dir[i]*k; // the bore point level with the model origin / grip
    if(!finite3(st.base))return;
    st.hand=hand;st.firearm=firearm;st.clip=clip;st.pool=pool;st.infinite=infinite;st.ns=nowNs();
    std::lock_guard lock(gunMutex);gun=st;
}
void shutdownAfterIdle(VkDevice d) noexcept {
    std::lock_guard lock(shared.mutex);
    if(!d||d!=shared.device)return;
    destroyDump();destroyImages();destroyAmmoBuffer(d);consumer.ammoFailed=false;consumer.ammoDrawn=false;
    for(auto& s:shared.slots){
        if(s.produceFence)vkDestroyFence(d,s.produceFence,nullptr);
        if(s.consumeFence)vkDestroyFence(d,s.consumeFence,nullptr);
        s=Slot{};
    }
    if(shared.producePool)vkDestroyCommandPool(d,shared.producePool,nullptr);
    if(shared.consumePool)vkDestroyCommandPool(d,shared.consumePool,nullptr);
    shared.producePool=shared.consumePool={};shared.consumeFamily=UINT32_MAX;shared.device={};shared.physical={};
    producer=Producer{};
    if(state()!=State::Failed&&state()!=State::Killed)setState(State::Off);
}
std::string report() {
    std::ostringstream o;const auto now=nowNs();const auto last=stats.lastPublishedNs.load();const GunState gs=gunSnapshot();
    const char* modes[3]{"OFF","HEAD","HAND"};const unsigned m=mode();
    o<<"{\"state\":\""<<stateName(state())<<"\",\"mode\":\""<<modes[m<3?m:0]<<"\",\"killed\":"<<(killed(Feature::Hud)?"true":"false")
     <<",\"frames\":"<<stats.published.load()<<",\"lastAgeMs\":"<<(last&&now>last?double(now-last)/1e6:-1.0)<<",\"lastFrame\":"<<stats.lastFrame.load()
     <<",\"wanted\":"<<stats.wanted.load()<<",\"begun\":"<<stats.begun.load()<<",\"recorded\":"<<stats.recorded.load()<<",\"cancelled\":"<<stats.cancelled.load()
     <<",\"pendingMissed\":"<<stats.pendingMissed.load()<<",\"consumed\":"<<stats.consumed.load()<<",\"headLayers\":"<<stats.headLayers.load()
     <<",\"wristLayers\":"<<stats.wristLayers.load()<<",\"gazeHidden\":"<<stats.gazeHidden.load()<<",\"staleHidden\":"<<stats.staleHidden.load()
     <<",\"noGrip\":"<<stats.noGrip.load()<<",\"xrFrames\":"<<stats.xrFrames.load()
     <<",\"source\":{\"w\":"<<stats.lastSourceW.load()<<",\"h\":"<<stats.lastSourceH.load()<<",\"format\":"<<stats.lastFormat.load()<<",\"layout\":"<<stats.lastLayout.load()<<'}'
     <<",\"rect\":{\"w\":"<<stats.ringW.load()<<",\"h\":"<<stats.ringH.load()<<",\"scale\":"<<props.scale.load()<<'}'
     <<",\"radarRect\":["<<stats.radar[0].load()<<','<<stats.radar[1].load()<<','<<stats.radar[2].load()<<','<<stats.radar[3].load()<<']'
     <<",\"radarPermille\":["<<props.radar[0].load()<<','<<props.radar[1].load()<<','<<props.radar[2].load()<<','<<props.radar[3].load()<<']'
     <<",\"swapchain\":{\"w\":"<<stats.swapW.load()<<",\"h\":"<<stats.swapH.load()<<",\"format\":"<<stats.swapFormat.load()<<",\"builds\":"<<stats.swapchainBuilds.load()<<'}'
     <<",\"ringBuilds\":"<<stats.ringBuilds.load()
     <<",\"clearAlpha\":{\"hooked\":"<<(clearAlphaHooked()?"true":"false")<<",\"fingerprint\":"<<clearAlphaFingerprint.load()
     <<",\"taken\":"<<stats.clearAlphaTaken.load()<<",\"dropped\":"<<stats.clearAlphaDropped.load()<<'}'
     <<",\"submitCpuUs\":"<<(stats.submitCalls.load()?double(stats.submitCpuNs.load())/double(stats.submitCalls.load())/1e3:0.0)
     <<",\"props\":{\"every\":"<<props.every.load()<<",\"alpha\":"<<props.alpha.load()<<",\"deg\":"<<props.deg.load()<<",\"dist\":"<<props.dist.load()
     <<",\"down\":"<<props.down.load()<<",\"follow\":"<<props.follow.load()<<",\"gaze\":"<<props.gaze.load()<<",\"maxAge\":"<<props.maxAge.load()
     <<",\"wrist\":["<<props.wrist[0].load()<<','<<props.wrist[1].load()<<','<<props.wrist[2].load()<<"]"
     <<",\"mapfix\":["<<props.mapfix[0].load()<<','<<props.mapfix[1].load()<<','<<props.mapfix[2].load()<<','<<props.mapfix[3].load()<<','
     <<props.mapfix[4].load()<<','<<props.mapfix[5].load()<<','<<props.mapfix[6].load()<<','<<props.mapfix[7].load()<<"],\"dump\":"<<props.dump.load()<<"}"
     <<",\"subtitles\":{\"wanted\":"<<(subtitlesWanted()?"true":"false")<<",\"frames\":"<<stats.subtitleFrames.load()<<",\"layers\":"<<stats.subtitleLayers.load()
     <<",\"band\":["<<props.subBand[0].load()<<','<<props.subBand[1].load()<<','<<props.subBand[2].load()<<','<<props.subBand[3].load()<<"],\"down\":"<<props.subDown.load()<<'}'
     <<",\"ammo\":{\"on\":"<<(settings().hudAmmo()?1:0)<<",\"layers\":"<<stats.ammoLayers.load()<<",\"hidden\":"<<stats.ammoHidden.load()
     <<",\"uploads\":"<<stats.ammoUploads.load()<<",\"failed\":"<<(consumer.ammoFailed?1:0)<<",\"drawn\":"<<(consumer.ammoDrawn?1:0)
     <<",\"gunAgeMs\":"<<(gs.ns&&now>gs.ns?double(now-gs.ns)/1e6:-1.0)<<",\"clip\":"<<gs.clip<<",\"pool\":"<<gs.pool<<",\"infinite\":"<<(gs.infinite?1:0)
     <<",\"hand\":"<<gs.hand<<",\"firearm\":"<<(gs.firearm?1:0)
     <<",\"place\":["<<props.ammoPlace[0].load()<<','<<props.ammoPlace[1].load()<<','<<props.ammoPlace[2].load()<<','<<props.ammoPlace[3].load()<<']'
     <<",\"crop\":["<<props.ammoCrop[0].load()<<','<<props.ammoCrop[1].load()<<','<<props.ammoCrop[2].load()<<','<<props.ammoCrop[3].load()
     <<"],\"tilt\":"<<props.ammoTilt.load()<<",\"face\":"<<props.ammoFace.load()<<'}'
     <<",\"mapLayers\":"<<stats.mapLayers.load()<<",\"wristOnArm\":"<<stats.wristOnArm.load()<<",\"rollAxis\":"<<consumer.rollAxis<<",\"rollPicks\":"<<consumer.rollPicks
     <<",\"wristDiag\":{\"fade\":"<<consumer.wristFade<<",\"facing\":"<<consumer.diagFacing<<",\"gaze\":"<<consumer.diagGaze<<",\"tilt\":"<<props.wristTilt.load()<<",\"faceFrames\":"<<consumer.faceFrames<<",\"onArm\":"<<(consumer.diagOnArm?1:0)<<",\"ageMs\":"<<consumer.diagAgeMs<<",\"forearmM\":"<<consumer.diagLen
     <<",\"elbow\":["<<consumer.diagElbow[0]<<','<<consumer.diagElbow[1]<<','<<consumer.diagElbow[2]<<"],\"wrist\":["<<consumer.diagWrist[0]<<','<<consumer.diagWrist[1]<<','<<consumer.diagWrist[2]
     <<"],\"quad\":["<<consumer.diagLocal[0]<<','<<consumer.diagLocal[1]<<','<<consumer.diagLocal[2]<<"]}"<<",\"dumps\":"<<stats.dumps.load()<<",\"dumpFailures\":"<<stats.dumpFailures.load()
     <<",\"alpha\":{\"live\":"<<(alphaLive()?"true":"false")<<",\"normalized\":"<<stats.alphaNormalized.load()<<",\"unsupported\":"<<stats.alphaUnsupported.load()
     <<",\"temporary\":"<<stats.alphaTemporary.load()<<",\"readFailures\":"<<stats.alphaReadFailures.load()<<'}'
     <<",\"skips\":{";
    for(unsigned i=0;i<unsigned(Skip::Count);++i)o<<(i?",":"")<<'"'<<skipNames[i]<<"\":"<<stats.skips[i].load();
    o<<"},\"lastSkip\":\""<<stats.lastSkip.load()<<"\",\"errors\":"<<stats.errors.load()<<",\"lastError\":\""<<stats.lastError.load()<<"\"}";
    return o.str();
}
}
