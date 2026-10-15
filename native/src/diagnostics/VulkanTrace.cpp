#include "VulkanTrace.h"
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>
namespace rdrvr::diagnostics {
namespace {
std::string address(std::uintptr_t p) {std::ostringstream s;s<<'"'<<"0x"<<std::hex<<p<<'"';return s.str();}
std::string quote(const std::string& value) {
    std::ostringstream s;s<<'"';const char* h="0123456789abcdef";
    for(unsigned char c:value) {
        if(c=='"'||c=='\\') s<<'\\'<<char(c);
        else if(c<32||c>=127) s<<"\\u00"<<h[c>>4]<<h[c&15];
        else s<<char(c);
    }
    s<<'"';return s.str();
}
struct Capture {
    TraceReader reader;bool complete{true};
    bool bytes(std::uintptr_t p,void* to,std::size_t size) {
        if(!p || size>65536 || p>UINTPTR_MAX-size || !reader.read || !reader.read(reader.context,p,to,size)) {complete=false;return false;}
        return true;
    }
    template<class T> bool get(const T* p,T& value) {return bytes(reinterpret_cast<std::uintptr_t>(p),&value,sizeof(value));}
    std::string string(const char* p) {
        std::string value;
        for(unsigned i=0;i<256;++i) {char c{};const auto base=reinterpret_cast<std::uintptr_t>(p);
            if(base>UINTPTR_MAX-i || !bytes(base+i,&c,1)) return "null";
            if(!c) return quote(value);value+=c;
        }
        complete=false;return "null";
    }
    std::string names(std::uint32_t count,const char* const* data) {
        if(count>128) {complete=false;return "null";}
        std::ostringstream s;s<<'[';
        std::array<const char*,128> pointers{};
        if(count && !bytes(reinterpret_cast<std::uintptr_t>(data),pointers.data(),count*sizeof(char*))) return "null";
        for(unsigned i=0;i<count;++i) {if(i)s<<',';s<<string(pointers[i]);}s<<']';return s.str();
    }
    std::string features(const VkPhysicalDeviceFeatures* p) {
        if(!p)return "null";VkPhysicalDeviceFeatures f{};if(!get(p,f))return "null";
        static_assert(sizeof(f)%sizeof(VkBool32)==0);
        std::array<VkBool32,sizeof(f)/sizeof(VkBool32)> words{};std::memcpy(words.data(),&f,sizeof(f));
        std::ostringstream s;s<<'[';for(unsigned i=0;i<words.size();++i){if(i)s<<',';s<<words[i];}s<<']';return s.str();
    }
    std::string chain(const void* pointer) {
        std::ostringstream s;s<<'[';std::array<std::uintptr_t,16> seen{};unsigned count=0;
        while(pointer) {
            const auto p=reinterpret_cast<std::uintptr_t>(pointer);
            if(count==seen.size()) {complete=false;break;}
            for(unsigned i=0;i<count;++i) if(seen[i]==p) {complete=false;s<<']';return s.str();}
            VkBaseInStructure node{};if(!bytes(p,&node,sizeof(node)))break;
            if(count)s<<',';seen[count++]=p;
            s<<"{\"sType\":"<<node.sType<<",\"address\":"<<address(p);
            std::size_t size{};
            switch(node.sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:size=sizeof(VkPhysicalDeviceFeatures2);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES:size=sizeof(VkPhysicalDeviceMultiviewFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:size=sizeof(VkPhysicalDeviceVulkan11Features);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:size=sizeof(VkPhysicalDeviceVulkan12Features);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:size=sizeof(VkPhysicalDeviceVulkan13Features);break;
            // Present in the device creation chain on Quest.
            // These fixed-size structs contain only sType/pNext and VkBool32 fields.
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES:size=sizeof(VkPhysicalDevice16BitStorageFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES:size=sizeof(VkPhysicalDeviceShaderFloat16Int8Features);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:size=sizeof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES:size=sizeof(VkPhysicalDeviceHostQueryResetFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES:size=sizeof(VkPhysicalDeviceImagelessFramebufferFeatures);break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES:size=sizeof(VkPhysicalDeviceDescriptorIndexingFeatures);break;
            case VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT:
                // Contains further arrays; identifying its header is not a full capture.
                break;
            default:break;
            }
            if(size) {
                std::array<unsigned char,1024> raw{};
                if(size>raw.size() || !bytes(p,raw.data(),size)) {complete=false;s<<",\"known\":false";}
                else {s<<",\"known\":true,\"rawHex\":\"";const char* h="0123456789abcdef";
                    for(unsigned i=0;i<size;++i)s<<h[raw[i]>>4]<<h[raw[i]&15];s<<'"';}
            } else {complete=false;s<<",\"known\":false";}
            s<<'}';pointer=node.pNext;
        }
        s<<']';return s.str();
    }
};
std::uint64_t clock(TraceOutput o) noexcept {try{return o.clockNs?o.clockNs():0;}catch(...){return 0;}}
template<class T> std::uintptr_t readHandle(T* pointer,TraceReader reader) {
    T value{};if(!pointer || !reader.read || !reader.read(reader.context,reinterpret_cast<std::uintptr_t>(pointer),&value,sizeof(value)))return 0;
    return reinterpret_cast<std::uintptr_t>(value);
}
}
TraceCapture captureInstance(const VkInstanceCreateInfo* pointer,TraceReader reader) {
    Capture c{reader};VkInstanceCreateInfo r{};
    if(!c.get(pointer,r) || r.sType!=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO)return {"null",false};
    std::ostringstream s;s<<"{\"flags\":"<<r.flags<<",\"layers\":"<<c.names(r.enabledLayerCount,r.ppEnabledLayerNames)
        <<",\"extensions\":"<<c.names(r.enabledExtensionCount,r.ppEnabledExtensionNames)<<",\"pNext\":"<<c.chain(r.pNext);
    if(r.pApplicationInfo) {VkApplicationInfo a{};
        if(c.get(r.pApplicationInfo,a) && a.sType==VK_STRUCTURE_TYPE_APPLICATION_INFO)
            s<<",\"apiVersion\":"<<a.apiVersion<<",\"applicationName\":"<<(a.pApplicationName?c.string(a.pApplicationName):"null")
             <<",\"applicationVersion\":"<<a.applicationVersion<<",\"engineName\":"<<(a.pEngineName?c.string(a.pEngineName):"null")
             <<",\"engineVersion\":"<<a.engineVersion
             <<",\"applicationPNext\":"<<c.chain(a.pNext);
        else {c.complete=false;s<<",\"apiVersion\":null";}
    } else s<<",\"apiVersion\":0";
    s<<'}';return {s.str(),c.complete};
}
TraceCapture captureDevice(const VkDeviceCreateInfo* pointer,TraceReader reader) {
    Capture c{reader};VkDeviceCreateInfo r{};
    if(!c.get(pointer,r) || r.sType!=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO)return {"null",false};
    std::ostringstream s;s<<"{\"flags\":"<<r.flags<<",\"layers\":"<<c.names(r.enabledLayerCount,r.ppEnabledLayerNames)
        <<",\"extensions\":"<<c.names(r.enabledExtensionCount,r.ppEnabledExtensionNames)<<",\"coreFeatureWords\":"<<c.features(r.pEnabledFeatures)
        <<",\"pNext\":"<<c.chain(r.pNext)<<",\"queues\":";
    std::array<VkDeviceQueueCreateInfo,32> queues{};
    if(!r.queueCreateInfoCount || r.queueCreateInfoCount>queues.size() || !c.bytes(reinterpret_cast<std::uintptr_t>(r.pQueueCreateInfos),queues.data(),r.queueCreateInfoCount*sizeof(queues[0]))) {
        c.complete=false;s<<"null";
    } else {
        s<<'[';for(unsigned i=0;i<r.queueCreateInfoCount;++i) {
            if(i)s<<',';auto& q=queues[i];if(q.sType!=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO)c.complete=false;
            s<<"{\"family\":"<<q.queueFamilyIndex<<",\"count\":"<<q.queueCount<<",\"flags\":"<<q.flags
             <<",\"pNext\":"<<c.chain(q.pNext)<<",\"priorities\":";
            std::array<float,64> priorities{};
            if(!q.queueCount || q.queueCount>priorities.size() || !c.bytes(reinterpret_cast<std::uintptr_t>(q.pQueuePriorities),priorities.data(),q.queueCount*sizeof(float))) {c.complete=false;s<<"null";}
            else {s<<'[';for(unsigned n=0;n<q.queueCount;++n){if(n)s<<',';if(std::isfinite(priorities[n]))s<<priorities[n];else {c.complete=false;s<<"null";}}s<<']';}
            s<<'}';
        }s<<']';
    }
    s<<'}';return {s.str(),c.complete};
}
void VulkanTrace::record(const char* name,const TraceCapture& capture,std::uint64_t started,std::uint64_t ended,VkResult result,
        std::uintptr_t input,std::uintptr_t output,std::uintptr_t allocator,std::uintptr_t object) noexcept {
    try {if(!output_.write){++failures_;return;}std::ostringstream s;
        s<<"{\"event\":\""<<name<<"\",\"threadId\":"<<(output_.threadId?output_.threadId():0)
         <<",\"startNs\":"<<started<<",\"endNs\":"<<ended<<",\"result\":"<<result
         <<",\"inputAddress\":"<<address(input)<<",\"outputHandle\":"<<address(output)
         <<",\"allocator\":"<<address(allocator)<<",\"object\":"<<address(object)
         <<",\"captureComplete\":"<<(capture.complete?"true":"false")<<",\"request\":"<<capture.json<<'}';
        output_.write(output_.context,s.str());
    } catch(...) { ++failures_; /* Diagnostics must never change the driver's outcome. */ }
}
VkResult VulkanTrace::createInstance(const VkInstanceCreateInfo* r,const VkAllocationCallbacks* a,VkInstance* out) noexcept {
    const int incoming=errno;
    const bool capture=instances_.fetch_add(1)<8;TraceCapture data{"null",false};
    if(capture)try {data=captureInstance(r,reader_);}catch(...){}
    const auto start=capture?clock(output_):0;errno=incoming;const auto result=api_.instance(r,a,out);const int saved=errno;
    if(capture)try {record("createInstance",data,start,clock(output_),result,reinterpret_cast<std::uintptr_t>(r),result==VK_SUCCESS?readHandle(out,reader_):0,reinterpret_cast<std::uintptr_t>(a),0);}catch(...){++failures_;}
    errno=saved;return result;
}
VkResult VulkanTrace::createDevice(VkPhysicalDevice p,const VkDeviceCreateInfo* r,const VkAllocationCallbacks* a,VkDevice* out) noexcept {
    const int incoming=errno;
    const bool capture=devices_.fetch_add(1)<8;TraceCapture data{"null",false};
    if(capture)try {data=captureDevice(r,reader_);}catch(...){}
    const auto start=capture?clock(output_):0;errno=incoming;const auto result=api_.device(p,r,a,out);const int saved=errno;
    if(capture)try {record("createDevice",data,start,clock(output_),result,reinterpret_cast<std::uintptr_t>(r),result==VK_SUCCESS?readHandle(out,reader_):0,reinterpret_cast<std::uintptr_t>(a),reinterpret_cast<std::uintptr_t>(p));}catch(...){++failures_;}
    errno=saved;return result;
}
void VulkanTrace::getDeviceQueue(VkDevice d,std::uint32_t family,std::uint32_t index,VkQueue* out) noexcept {
    const int incoming=errno;const bool capture=queues_.fetch_add(1)<16;const auto start=capture?clock(output_):0;errno=incoming;api_.queue(d,family,index,out);const int saved=errno;
    if(capture)try {TraceCapture data{"{\"family\":"+std::to_string(family)+",\"index\":"+std::to_string(index)+"}",true};
        record("getDeviceQueue",data,start,clock(output_),VK_SUCCESS,0,readHandle(out,reader_),0,reinterpret_cast<std::uintptr_t>(d));}catch(...){++failures_;}
    errno=saved;
}
VkResult VulkanTrace::queueSubmit(VkQueue q,std::uint32_t count,const VkSubmitInfo* submits,VkFence fence) noexcept {
    const int incoming=errno;const bool capture=submits_.fetch_add(1)<32;const auto start=capture?clock(output_):0;errno=incoming;const auto result=api_.submit(q,count,submits,fence);const int saved=errno;
    if(capture)try {TraceCapture data{"{\"submitCount\":"+std::to_string(count)+",\"contentsCaptured\":false}",false};
        record("queueSubmit",data,start,clock(output_),result,reinterpret_cast<std::uintptr_t>(submits),0,0,reinterpret_cast<std::uintptr_t>(q));}catch(...){++failures_;}
    errno=saved;return result;
}
VkResult VulkanTrace::enumeratePhysicalDevices(VkInstance instance,std::uint32_t* count,VkPhysicalDevice* devices) noexcept {
    const int incoming=errno;const bool capture=enumerations_.fetch_add(1)<16;
    std::uint32_t capacity{};bool capacityKnown=!devices;
    if(capture && devices)try {Capture c{reader_};capacityKnown=c.get(count,capacity);}catch(...){}
    const auto start=capture?clock(output_):0;errno=incoming;
    const auto result=api_.enumerate(instance,count,devices);const int saved=errno;
    if(capture)try {
        Capture c{reader_};std::uint32_t returned{};
        const bool countKnown=(result==VK_SUCCESS || result==VK_INCOMPLETE) && c.get(count,returned);
        c.complete=countKnown && capacityKnown;
        std::ostringstream s;s<<"{\"countOnly\":"<<(devices?"false":"true")<<",\"count\":";
        if(countKnown)s<<returned;else s<<"null";
        s<<",\"capacity\":";if(devices && capacityKnown)s<<capacity;else s<<"null";
        s<<",\"physicalDevices\":[";
        std::array<VkPhysicalDevice,32> values{};
        if(devices && countKnown && capacityKnown) {
            if(returned>capacity || returned>values.size())c.complete=false;
            else if(!returned || c.bytes(reinterpret_cast<std::uintptr_t>(devices),values.data(),returned*sizeof(values[0])))
                for(unsigned i=0;i<returned;++i){if(i)s<<',';s<<address(reinterpret_cast<std::uintptr_t>(values[i]));}
        }
        s<<"]}";
        record("enumeratePhysicalDevices",{s.str(),c.complete},start,clock(output_),result,
            reinterpret_cast<std::uintptr_t>(count),0,0,reinterpret_cast<std::uintptr_t>(instance));
    }catch(...){++failures_;}
    errno=saved;return result;
}
void VulkanTrace::destroyInstance(VkInstance instance,const VkAllocationCallbacks* allocator) noexcept {
    const int incoming=errno;const bool capture=instanceDestructions_.fetch_add(1)<8;
    const auto start=capture?clock(output_):0;errno=incoming;api_.destroyInstance(instance,allocator);const int saved=errno;
    if(capture)record("destroyInstance",{"{}",true},start,clock(output_),VK_SUCCESS,0,0,
        reinterpret_cast<std::uintptr_t>(allocator),reinterpret_cast<std::uintptr_t>(instance));
    errno=saved;
}
void VulkanTrace::destroyDevice(VkDevice device,const VkAllocationCallbacks* allocator) noexcept {
    const int incoming=errno;const bool capture=deviceDestructions_.fetch_add(1)<8;
    const auto start=capture?clock(output_):0;errno=incoming;api_.destroyDevice(device,allocator);const int saved=errno;
    if(capture)record("destroyDevice",{"{}",true},start,clock(output_),VK_SUCCESS,0,0,
        reinterpret_cast<std::uintptr_t>(allocator),reinterpret_cast<std::uintptr_t>(device));
    errno=saved;
}
void VulkanTrace::getPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physical,std::uint32_t* count,VkQueueFamilyProperties* properties) noexcept {
    const int incoming=errno;const bool capture=familyQueries_.fetch_add(1)<8;
    std::uint32_t capacity{};bool capacityKnown=!properties;
    if(capture && properties)try {Capture c{reader_};capacityKnown=c.get(count,capacity);}catch(...){}
    const auto start=capture?clock(output_):0;errno=incoming;api_.families(physical,count,properties);const int saved=errno;
    if(capture)try {
        Capture c{reader_};std::uint32_t returned{};const bool countKnown=c.get(count,returned);
        c.complete=countKnown && capacityKnown;
        std::ostringstream s;s<<"{\"countOnly\":"<<(properties?"false":"true")<<",\"count\":";
        if(countKnown)s<<returned;else s<<"null";
        s<<",\"capacity\":";if(properties && capacityKnown)s<<capacity;else s<<"null";
        s<<",\"families\":[";std::array<VkQueueFamilyProperties,32> values{};
        if(properties && countKnown && capacityKnown) {
            if(returned>capacity || returned>values.size())c.complete=false;
            else if(!returned || c.bytes(reinterpret_cast<std::uintptr_t>(properties),values.data(),returned*sizeof(values[0])))
                for(unsigned i=0;i<returned;++i){if(i)s<<',';const auto& v=values[i];
                    s<<"{\"index\":"<<i<<",\"flags\":"<<v.queueFlags<<",\"queueCount\":"<<v.queueCount
                     <<",\"timestampValidBits\":"<<v.timestampValidBits<<",\"minImageTransferGranularity\":["
                     <<v.minImageTransferGranularity.width<<','<<v.minImageTransferGranularity.height<<','<<v.minImageTransferGranularity.depth<<"]}";}
        }
        s<<"]}";
        record("getPhysicalDeviceQueueFamilyProperties",{s.str(),c.complete},start,clock(output_),VK_SUCCESS,
            reinterpret_cast<std::uintptr_t>(count),0,0,reinterpret_cast<std::uintptr_t>(physical));
    }catch(...){++failures_;}
    errno=saved;
}
} // namespace rdrvr::diagnostics
