#pragma once
#ifdef __ANDROID__
#include <jni.h>
#include <android/log.h>
#endif
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdexcept>
#include <string>
#include <cstdint>
namespace rdrvr::runtime {
inline void checkXr(XrResult r, const char* operation) {
    if (XR_FAILED(r)) throw std::runtime_error(std::string(operation)+" xr="+std::to_string(r));
}
inline void checkVk(VkResult r, const char* operation) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(operation)+" vk="+std::to_string(r));
}
template<class T> T proc(XrInstance instance, const char* name) {
    PFN_xrVoidFunction p{};
    checkXr(xrGetInstanceProcAddr(instance,name,&p),name);
    if (!p) throw std::runtime_error(std::string("missing ")+name);
    return reinterpret_cast<T>(p);
}
} // namespace rdrvr::runtime
