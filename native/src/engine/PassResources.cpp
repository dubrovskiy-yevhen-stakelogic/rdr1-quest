#include <map>
#include <tuple>
#include <vector>
#include <algorithm>
#include "PassResources.h"
#include <limits>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <array>
namespace rdrvr::engine {
namespace {
bool format(VkFormat f){return f==VK_FORMAT_R8G8B8A8_UNORM || f==VK_FORMAT_R8G8B8A8_SRGB || f==VK_FORMAT_B8G8R8A8_UNORM || f==VK_FORMAT_B8G8R8A8_SRGB || f==VK_FORMAT_R16G16B16A16_SFLOAT;}
bool depthFormat(VkFormat f){return f==VK_FORMAT_D16_UNORM || f==VK_FORMAT_D32_SFLOAT || f==VK_FORMAT_D24_UNORM_S8_UINT || f==VK_FORMAT_D32_SFLOAT_S8_UINT;}
}
PFN_vkVoidFunction imageProc(PFN_vkVoidFunction original,const char* name,PFN_vkCreateImage create,PFN_vkDestroyImage destroy) noexcept {
    if(!original || !name)return original;
    if(create && !std::strcmp(name,"vkCreateImage"))return reinterpret_cast<PFN_vkVoidFunction>(create);
    if(destroy && !std::strcmp(name,"vkDestroyImage"))return reinterpret_cast<PFN_vkVoidFunction>(destroy);
    return original;
}
bool PassResources::bind(VkDevice d){std::lock_guard guard(mutex_);if(!d || device_)return false;device_=d;return true;}
void PassResources::created(VkDevice d,const VkImageCreateInfo* c,VkImage image,VkResult result) noexcept {
    struct Restore {int value;~Restore(){errno=value;}} restore{errno};
    try {
        std::lock_guard guard(mutex_);
        if(!device_ || d!=device_ || result!=VK_SUCCESS || !image)return;
        // Remove a stale address even if a new incarnation is unsupported.
        images_.erase(image);
        if(!c || c->sType!=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO || c->pNext || c->flags ||
           c->imageType!=VK_IMAGE_TYPE_2D || c->tiling!=VK_IMAGE_TILING_OPTIMAL || c->sharingMode!=VK_SHARING_MODE_EXCLUSIVE ||
           !(((format(c->format) || c->format==VK_FORMAT_R32_SFLOAT) && (c->usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) ||
             (depthFormat(c->format) && (c->usage&VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) ||
           (c->usage&VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) || !c->extent.width || !c->extent.height || c->extent.width>8192 || c->extent.height>8192 ||
           c->extent.depth!=1 || !c->mipLevels || c->mipLevels>14 || !c->arrayLayers || c->arrayLayers>2048 || images_.size()>=capacity){++rejected_;return;}
        images_.emplace(image,ImageRecord{image,d,c->extent,c->format,c->usage,c->samples,c->arrayLayers,c->mipLevels,++generation_});++created_;
    }catch(...){/* Optional metadata must preserve a successful game allocation. */}
}
void PassResources::destroyed(VkDevice d,VkImage image) noexcept {std::lock_guard guard(mutex_);if(d==device_){destroyed_+=images_.erase(image);}}
bool PassResources::lookup(VkDevice d,VkImage image,ImageRecord& out) {
    std::lock_guard guard(mutex_);auto it=images_.find(image);
    if(!device_ || d!=device_ || it==images_.end()){++misses_;return false;}out=it->second;return true;
}
void PassResources::clear(VkDevice d) noexcept {std::lock_guard guard(mutex_);if(d==device_){images_.clear();device_={};}}
std::string PassResources::report() const {std::lock_guard guard(mutex_);std::ostringstream s;s<<"{\"liveImages\":"<<images_.size()<<",\"created\":"<<created_<<",\"destroyed\":"<<destroyed_<<",\"rejected\":"<<rejected_<<",\"lookupMisses\":"<<misses_;
    { // Live images by format x size x usage (leak hunt) and the 12 newest
        std::map<std::tuple<int,unsigned,unsigned,unsigned>,unsigned> kinds;std::vector<const ImageRecord*> all;
        for(const auto& [image,r]:images_){++kinds[{int(r.format),r.extent.width,r.extent.height,unsigned(r.usage)}];all.push_back(&r);}
        s<<",\"kinds\":[";bool first=true;
        for(const auto& [k,n]:kinds){if(!first)s<<',';first=false;s<<'['<<std::get<0>(k)<<','<<std::get<1>(k)<<','<<std::get<2>(k)<<','<<std::get<3>(k)<<','<<n<<']';}
        std::sort(all.begin(),all.end(),[](const ImageRecord* x,const ImageRecord* y){return x->generation>y->generation;});
        s<<"],\"newest\":[";for(std::size_t i=0;i<all.size()&&i<12;++i){if(i)s<<',';s<<'['<<all[i]->generation<<','<<int(all[i]->format)<<','<<all[i]->extent.width<<','<<all[i]->extent.height<<','<<unsigned(all[i]->usage)<<']';}
        s<<']';}
    s<<'}';return s.str();}
bool readTargetImage(PassResources& registry,VkDevice device,const PassSourceAbi& abi,std::uintptr_t target,TargetImage& out,const char** status) {
    auto stage=[&](const char* name){if(status)*status=name;};stage("ABI");
    if(!abi.read || !abi.targetVtable || !abi.textureVtable || !target || target%8)return false;
    auto read=[&]<class T>(std::uintptr_t base,std::size_t offset,T& value){return base && offset<=std::numeric_limits<std::uintptr_t>::max()-base && sizeof(value)-1<=std::numeric_limits<std::uintptr_t>::max()-(base+offset) && abi.read(abi.context,base+offset,&value,sizeof(value));};
    // Three guarded kernel copies instead of ten tiny reads. The highest
    // accessed byte is unchanged; no pointer or image metadata survives this
    // call. In particular, handle reuse still goes through the live registry.
    std::array<std::byte,0xac> targetFields{};
    std::array<std::byte,0xa4> textureFields{};
    auto field=[]<class T>(const auto& bytes,std::size_t offset,T& value){std::memcpy(&value,bytes.data()+offset,sizeof(value));};
    std::uintptr_t table{},texture{},data{};std::uint16_t width{},height{};
    std::uint32_t textureWidth{},textureHeight{},layout{};VkImage image{};
    stage("TARGET_VTABLE");if(!read(target,0,targetFields))return false;
    field(targetFields,0,table);if(table!=abi.targetVtable)return false;
    field(targetFields,0x60,texture);field(targetFields,0xa8,width);field(targetFields,0xaa,height);
    stage("TEXTURE_POINTER");if(!texture||texture%8)return false;
    stage("TEXTURE_VTABLE");if(!read(texture,0,textureFields))return false;
    field(textureFields,0,table);if(table!=abi.textureVtable)return false;
    field(textureFields,0x98,data);field(textureFields,0xa0,layout);
    field(textureFields,0x48,textureWidth);field(textureFields,0x4c,textureHeight);
    stage("TEXTURE_DATA");if(!data||data%8)return false;
    stage("IMAGE_HANDLE");if(!read(data,0x40,image)||!image)return false;
    ImageRecord record{};
    stage("IMAGE_REGISTRY");if(!registry.lookup(device,image,record))return false;
    stage("IMAGE_EXTENT");if(record.extent.width!=width||record.extent.height!=height||textureWidth!=width||textureHeight!=height)return false;
    stage("READY");
    out={record,static_cast<VkImageLayout>(layout),target,texture};return true;
}
bool readPassSource(PassResources& registry,VkDevice device,const PassSourceAbi& abi,std::uintptr_t target,std::uintptr_t context,PassSource& out) {
    if(!abi.read || !abi.contextVtable || !context || context%8)return false;
    auto read=[&]<class T>(std::uintptr_t base,std::size_t offset,T& value){return base && offset<=std::numeric_limits<std::uintptr_t>::max()-base && sizeof(value)-1<=std::numeric_limits<std::uintptr_t>::max()-(base+offset) && abi.read(abi.context,base+offset,&value,sizeof(value));};
    std::uintptr_t table{},wrapper{},pass{};VkCommandBuffer command{};TargetImage source{};
    if(!read(context,0,table) || table!=abi.contextVtable || !read(context,0x30a8,pass) || pass ||
       !read(context,0x30a0,wrapper) || !wrapper || wrapper%8 || !read(wrapper,0x38,command) || !command ||
       !readTargetImage(registry,device,abi,target,source))return false;
    const auto& record=source.image;const auto layout=source.layout;
    if(!format(record.format) || !(record.usage&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
       record.layers!=1 || record.samples!=VK_SAMPLE_COUNT_1_BIT || !(record.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
       (layout!=VK_IMAGE_LAYOUT_GENERAL && layout!=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        layout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && layout!=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL))return false;
    out={record,layout,command,target,source.texture};return true;
}
}
