#include "ChainQualification.h"
#include "LayerControl.h"
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#else
#include <dlfcn.h>
#endif
#ifdef RDR2_LAYER28_TESTING
namespace {PFN_vkVoidFunction testA{},testB{};}
extern "C" RDR_W28_EXPORT void rdrWorker28TestTerminators(PFN_vkVoidFunction a,PFN_vkVoidFunction b){testA=a;testB=b;}
#endif
namespace rdr2::layer28 {
bool qualifiedTerminator(PFN_vkVoidFunction p)noexcept{
 if(!p)return false;
#ifdef RDR2_LAYER28_TESTING
 if(p==testA||p==testB)return true;
#endif
#ifdef _WIN32
 HMODULE m{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(p),&m))return false;
 wchar_t path[32768]{},system[32768]{};auto n=GetModuleFileNameW(m,path,32768);auto s=GetSystemDirectoryW(system,32768);if(!n||n>=32768||!s||s>=32768)return false;
 const auto*base=std::wcsrchr(path,L'\\');if(!base||_wcsicmp(base+1,L"vulkan-1.dll")!=0)return false;
 return std::size_t(base-path)==s&&_wcsnicmp(path,system,s)==0;
#elif defined(__ANDROID__)
 Dl_info info{};if(!dladdr(reinterpret_cast<const void*>(p),&info)||!info.dli_fname||!info.dli_fbase)return false;
 const char*base=std::strrchr(info.dli_fname,'/');if(!base||std::strcmp(base+1,"libvulkan.so")!=0)return false;
 return std::strncmp(info.dli_fname,"/system/",8)==0||std::strncmp(info.dli_fname,"/apex/",6)==0;
#else
 return false;
#endif
}
}
