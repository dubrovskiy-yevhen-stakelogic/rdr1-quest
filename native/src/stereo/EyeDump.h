#pragma once
// Diagnostic: dump right/left eye targets to files on request (trigger file in the app's rdrvr dir).
// Copies are recorded at the end of the right-eye color replay into a host-visible buffer and written
// to disk a few frames later. Loader exports are used for the private buffer (same as UniformIntegration).
// The same trigger (debug.rdrvr.eyedump <nonce>) also requests the AppSW dump (runtime/SpaceWarp.h: eye-dump-warp-*.raw +
// eye-dump-warp.json: motion vectors, AppSW depth, actor blocks); "warpRequest" in eye-dump.json = its number. The next
// produced pair is the pre-roll (its depth = -prevDepth), the pair after it is dumped (so not the frame of these eye targets).
#include "runtime/SpaceWarpActors.h"
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef __ANDROID__
#include <sys/stat.h>
#include <unistd.h>
#include <sys/system_properties.h>
#endif
namespace rdrvr::engine::stereo {
// an automatic request (e.g. GameXr on the K-th stereo frame of a cutscene, debug.rdrvr.eyedump.cutscene K) - same dump as the property.
inline std::atomic<bool> eyeDumpAutoRequest{false};
struct EyeDumpEntry {const char* name;VkImage image;std::uintptr_t texture;VkFormat format;VkExtent3D extent;bool depth;VkDeviceSize offset;VkDeviceSize bytes;};
class EyeDump {
public:
    static constexpr const char* dir="/sdcard/Android/data/com.rockstargames.rdr/files/rdrvr";
    bool armed{};unsigned pendingFrames{};unsigned pollCounter{};std::string lastError,lastTrigger;std::uint64_t dumps{},failures{};
    VkDevice device{};VkPhysicalDevice physical{};VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};VkDeviceSize capacity{};
    std::vector<EyeDumpEntry> entries;std::string meta;
    // Call once per frame (color replay start). Returns true when a dump should be recorded this frame.
    bool poll(){
        if(pendingFrames){if(--pendingFrames==0)flush();return false;}
        if(armed)return true;
        if(eyeDumpAutoRequest.exchange(false,std::memory_order_relaxed)){armed=true;return true;}
        if(++pollCounter%10)return false;
#ifdef __ANDROID__
        char value[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.eyedump",value)<=0||!value[0]||lastTrigger==value)return false;
        lastTrigger=value;armed=true;return true;
#else
        return false;
#endif
    }
    bool ensureBuffer(VkDevice d,VkPhysicalDevice p,VkDeviceSize need){
        if(buffer&&capacity>=need&&device==d)return true;
        if(buffer){::vkDestroyBuffer(device,buffer,nullptr);::vkFreeMemory(device,memory,nullptr);buffer={};memory={};mapped={};}
        device=d;physical=p;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=need;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(::vkCreateBuffer(d,&bi,nullptr,&buffer)!=VK_SUCCESS){lastError="createBuffer";return false;}
        VkMemoryRequirements req{};::vkGetBufferMemoryRequirements(d,buffer,&req);
        VkPhysicalDeviceMemoryProperties props{};::vkGetPhysicalDeviceMemoryProperties(p,&props);
        std::uint32_t type=UINT32_MAX;const VkMemoryPropertyFlags want=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for(std::uint32_t i=0;i<props.memoryTypeCount;++i)if((req.memoryTypeBits&(1u<<i))&&(props.memoryTypes[i].propertyFlags&want)==want){type=i;break;}
        if(type==UINT32_MAX){lastError="memoryType";return false;}
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        if(::vkAllocateMemory(d,&ai,nullptr,&memory)!=VK_SUCCESS){lastError="allocate";return false;}
        if(::vkBindBufferMemory(d,buffer,memory,0)!=VK_SUCCESS){lastError="bind";return false;}
        if(::vkMapMemory(d,memory,0,VK_WHOLE_SIZE,0,&mapped)!=VK_SUCCESS){lastError="map";return false;}
        capacity=need;return true;
    }
    static std::uint32_t texelBytes(VkFormat f,bool depth){
        if(depth)return 4;
        switch(f){case VK_FORMAT_R16G16B16A16_SFLOAT:return 8;case VK_FORMAT_R32G32B32A32_SFLOAT:return 16;case VK_FORMAT_R8_UNORM:case VK_FORMAT_R8_UINT:return 1;case VK_FORMAT_R16_SFLOAT:case VK_FORMAT_R8G8_UNORM:case VK_FORMAT_D16_UNORM:return 2;default:return 4;}
    }
    // Records the copies. `tracked(texture)` returns the engine-tracked layout. `barrier/copy` are raw PFNs.
    template<class Tracked,class Barrier,class Copy>
    bool record(VkCommandBuffer c,VkDevice d,VkPhysicalDevice p,std::vector<EyeDumpEntry> list,const std::string& metaJson,Tracked tracked,Barrier barrier,Copy copy){
        armed=false;entries.clear();VkDeviceSize total=0;
        for(auto& e:list){e.bytes=VkDeviceSize(e.extent.width)*e.extent.height*texelBytes(e.format,e.depth);e.offset=total;total+=(e.bytes+255)&~VkDeviceSize(255);}
        if(!ensureBuffer(d,p,total)){++failures;return false;}
        for(const auto& e:list){
            const VkImageAspectFlags aspect=e.depth?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageLayout old=tracked(e.texture);if(old==VK_IMAGE_LAYOUT_UNDEFINED||old==VK_IMAGE_LAYOUT_PREINITIALIZED)old=VK_IMAGE_LAYOUT_GENERAL;
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=e.image;
            b.subresourceRange={aspect|(e.depth&&(e.format==VK_FORMAT_D24_UNORM_S8_UINT||e.format==VK_FORMAT_D32_SFLOAT_S8_UINT)?VK_IMAGE_ASPECT_STENCIL_BIT:0u),0,1,0,1};
            b.oldLayout=old;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            barrier(c,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            VkBufferImageCopy region{};region.bufferOffset=e.offset;region.imageSubresource={aspect,0,0,1};region.imageExtent={e.extent.width,e.extent.height,1};
            copy(c,e.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&region);
            b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.newLayout=old;b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;
            barrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
            entries.push_back(e);
        }
        // One AppSW dump request per eye dump (claimed by the mailbox producer of the next published pair)
        const auto warp=::rdrvr::runtime::spacewarp::dumpRequests.fetch_add(1,std::memory_order_relaxed)+1;
        meta=metaJson+",\"warpRequest\":"+std::to_string(warp);pendingFrames=4;return true;
    }
    void flush(){
        if(!mapped){++failures;return;}
        std::string base=std::string(dir)+"/eye-dump";
        std::string json="{\"entries\":[";bool first=true;
        for(const auto& e:entries){
            std::string path=base+"-"+e.name+".raw";
            if(FILE* f=std::fopen(path.c_str(),"wb")){std::fwrite(static_cast<const char*>(mapped)+e.offset,1,std::size_t(e.bytes),f);std::fclose(f);}
            char line[256];std::snprintf(line,sizeof line,"%s{\"name\":\"%s\",\"format\":%u,\"width\":%u,\"height\":%u,\"depth\":%u,\"bytes\":%llu}",first?"":",",e.name,unsigned(e.format),e.extent.width,e.extent.height,e.depth?1u:0u,(unsigned long long)e.bytes);
            json+=line;first=false;
        }
        json+="],"+meta+"}";
        if(FILE* f=std::fopen((base+".json").c_str(),"wb")){std::fwrite(json.data(),1,json.size(),f);std::fclose(f);}
        ++dumps;
    }
};
}
