#include "RetailAllocatorQualification.h"
#if defined(__ANDROID__) && defined(__aarch64__)
#include "RetailAllocatorPins.h"
#include "CertificateChecks.h"
#include "Sha256.h"
#include "CachedCertificate.h"
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <limits.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace rdr2::worker28::retail_allocator {
namespace {
using namespace detail;
constexpr char LibcPath[]="/apex/com.android.runtime/lib64/bionic/libc.so";
constexpr std::uint8_t LibcBuildId[20]{0x80,0x26,0x81,0xf0,0x7c,0xa5,0xa0,0xc7,0xcd,0x62,0x24,0xd3,0x08,0x8e,0x4b,0x3a,0,0,0,0};
struct File {int fd{-1};~File(){if(fd>=0)::close(fd);}};
struct Handle {void* p{};~Handle(){if(p)::dlclose(p);}void* release()noexcept{auto*q=p;p=nullptr;return q;}};
struct Module {
 std::uintptr_t base{};std::array<Elf64_Phdr,64> headers{};std::size_t count{};bool found{};
};
bool plus(std::uintptr_t a,std::uint64_t b,std::uintptr_t&out) noexcept {
 if(b>UINTPTR_MAX-a)return false;out=a+static_cast<std::uintptr_t>(b);return true;
}
int findModule(dl_phdr_info*info,std::size_t,void*data) noexcept {
 auto&m=*static_cast<Module*>(data);
 if(static_cast<std::uintptr_t>(info->dlpi_addr)!=m.base)return 0;
 if(!info->dlpi_phdr||!info->dlpi_phnum||info->dlpi_phnum>m.headers.size())return 1;
 m.count=info->dlpi_phnum;std::memcpy(m.headers.data(),info->dlpi_phdr,m.count*sizeof(Elf64_Phdr));m.found=true;return 1;
}
bool mapped(std::uintptr_t address,std::size_t length,bool executable,const struct stat*file) noexcept {
 if(!length||length>UINTPTR_MAX-address)return false;
 auto*f=std::fopen("/proc/self/maps","r");if(!f)return false;
 char line[2048]{};bool result=false;
 while(std::fgets(line,sizeof(line),f)){
  unsigned long long a{},b{},offset{},inode{};unsigned devMajor{},devMinor{};char perm[5]{};
  if(std::sscanf(line,"%llx-%llx %4s %llx %x:%x %llu",&a,&b,perm,&offset,&devMajor,&devMinor,&inode)!=7)continue;
  if(a>address||address>=b)continue;
  result=address+length<=b&&perm[0]=='r'&&perm[1]!='w'&&(!executable||perm[2]=='x');
  if(result&&file)result=inode==static_cast<unsigned long long>(file->st_ino)&&devMajor==major(file->st_dev)&&devMinor==minor(file->st_dev);
  break;
 }
 std::fclose(f);return result;
}
const void* readable(const Module&m,std::uintptr_t rva,std::size_t n,bool code,const struct stat*file=nullptr) noexcept {
 std::uintptr_t start{},end{};if(!n||!plus(m.base,rva,start)||!plus(start,n,end))return nullptr;
 for(std::size_t i=0;i<m.count;++i){const auto&h=m.headers[i];std::uintptr_t hs{},he{};
  if(h.p_type!=PT_LOAD||!(h.p_flags&PF_R)||(code&&!(h.p_flags&PF_X))||!plus(m.base,h.p_vaddr,hs)||!plus(hs,h.p_memsz,he))continue;
  if(hs<=start&&end<=he&&mapped(start,n,code,file))return reinterpret_cast<const void*>(start);
 }
 return nullptr;
}
bool canonical(const char*path,char(&out)[PATH_MAX]) noexcept {return path&&::realpath(path,out)!=nullptr;}
bool moduleAt(std::uintptr_t address,Module&m,char(&path)[PATH_MAX]) noexcept {
 Dl_info info{};if(!address||!::dladdr(reinterpret_cast<void*>(address),&info)||!info.dli_fbase||!canonical(info.dli_fname,path))return false;
 m.base=reinterpret_cast<std::uintptr_t>(info.dli_fbase);::dl_iterate_phdr(findModule,&m);return m.found;
}
bool fileIdentity(int fd,struct stat&st) noexcept {
 if(::fstat(fd,&st)||!S_ISREG(st.st_mode)||static_cast<std::uint64_t>(st.st_size)!=FileSize||::lseek(fd,0,SEEK_SET)!=0)return false;
 Sha256 hash;std::array<std::uint8_t,65536> bytes{};
 for(std::uint64_t left=FileSize;left;){const auto n=static_cast<std::size_t>(left<bytes.size()?left:bytes.size());const auto got=::read(fd,bytes.data(),n);if(got<=0)return false;hash.update(bytes.data(),static_cast<std::size_t>(got));left-=static_cast<std::size_t>(got);}
 const auto digest=hash.finish();struct stat after{};
 return std::memcmp(digest.data(),FileDigest,digest.size())==0&&::fstat(fd,&after)==0&&st.st_dev==after.st_dev&&st.st_ino==after.st_ino&&st.st_size==after.st_size&&st.st_mtim.tv_sec==after.st_mtim.tv_sec&&st.st_mtim.tv_nsec==after.st_mtim.tv_nsec&&st.st_ctim.tv_sec==after.st_ctim.tv_sec&&st.st_ctim.tv_nsec==after.st_ctim.tv_nsec;
}
bool libcIdentity(const Module&m) noexcept {
 // Simpleperf stores build IDs padded to 20 bytes. Accept the actual 16-byte
 // note or the same exact 20-byte padded note, never an arbitrary prefix.
 unsigned matches=0;
 for(std::size_t i=0;i<m.count;++i){const auto&h=m.headers[i];if(h.p_type!=PT_NOTE)continue;
  if(h.p_memsz>65536)return false;const auto*p=static_cast<const std::uint8_t*>(readable(m,static_cast<std::uintptr_t>(h.p_vaddr),static_cast<std::size_t>(h.p_memsz),false));if(!p)return false;
  std::size_t at=0;while(at<static_cast<std::size_t>(h.p_memsz)){
   if(static_cast<std::size_t>(h.p_memsz)-at<12)return false;std::uint32_t v[3]{};std::memcpy(v,p+at,12);at+=12;
   const auto ns=static_cast<std::uint64_t>(v[0]+std::uint64_t(3))&~std::uint64_t(3),ds=static_cast<std::uint64_t>(v[1]+std::uint64_t(3))&~std::uint64_t(3);
   if(ns>h.p_memsz-at||ds>h.p_memsz-at-ns)return false;
   if(v[0]==4&&v[2]==NT_GNU_BUILD_ID&&std::memcmp(p+at,"GNU\0",4)==0){if(++matches!=1||(v[1]!=16&&v[1]!=20)||std::memcmp(p+at+ns,LibcBuildId,v[1])!=0)return false;}
   at+=static_cast<std::size_t>(ns+ds);
  }
 }
 return matches==1;
}
bool symbol(void*libc,const Module&m,const char*name,std::uintptr_t&value) noexcept {
 void*p=::dlsym(libc,name);Dl_info info{};char path[PATH_MAX]{};
 if(!p||::dlsym(RTLD_DEFAULT,name)!=p||!::dladdr(p,&info)||info.dli_fbase!=reinterpret_cast<void*>(m.base)||info.dli_saddr!=p||!info.dli_sname||std::strcmp(info.dli_sname,name)||!canonical(info.dli_fname,path)||std::strcmp(path,LibcPath))return false;
 value=reinterpret_cast<std::uintptr_t>(p);return value>=m.base&&readable(m,value-m.base,4,true);
}
struct Retained {std::mutex mutex;void*engine{};void*libc{};std::uintptr_t base{};char path[PATH_MAX]{};struct stat file{};
 CachedCertificate certificate;std::atomic<const CachedCertificate*> published{};};
Retained& retained(){static Retained value;return value;}
bool check(const VkAllocationCallbacks&c){
 if(!c.pfnAllocation||!c.pfnReallocation||!c.pfnFree||c.pfnInternalAllocation||c.pfnInternalFree)return false;
 auto&r=retained();if(const auto* cached=r.published.load(std::memory_order_acquire))return cached->matches(c);
 std::lock_guard<std::mutex>lock(r.mutex);
 if(const auto* cached=r.published.load(std::memory_order_acquire))return cached->matches(c);
 Module engine;char path[PATH_MAX]{};const auto allocation=reinterpret_cast<std::uintptr_t>(c.pfnAllocation);
 if(!moduleAt(allocation,engine,path)||!callbackAddresses(engine.base,allocation,reinterpret_cast<std::uintptr_t>(c.pfnReallocation),reinterpret_cast<std::uintptr_t>(c.pfnFree)))return false;
 const auto*leaf=std::strrchr(path,'/');if(!leaf||std::strcmp(leaf+1,"librdr.so"))return false;
 if(r.engine&&(r.base!=engine.base||std::strcmp(r.path,path)))return false;
 Handle engineHandle,libcHandle;File file;struct stat identity{};
 if(!r.engine){engineHandle.p=::dlopen(path,RTLD_NOW|RTLD_LOCAL|RTLD_NOLOAD);if(!engineHandle.p)return false;file.fd=::open(path,O_RDONLY|O_CLOEXEC);if(file.fd<0||!fileIdentity(file.fd,identity))return false;}
 else identity=r.file;
 const auto readEngine=[&](std::uintptr_t rva,std::size_t n,bool code)noexcept{return readable(engine,rva,n,code,&identity);};
 if(engine.count*sizeof(Elf64_Phdr)!=sizeof(PhdrBytes)||std::memcmp(engine.headers.data(),PhdrBytes,sizeof(PhdrBytes))||!imageBytes(readEngine))return false;
 libcHandle.p=::dlopen("libc.so",RTLD_NOW|RTLD_LOCAL);if(!libcHandle.p)return false;
 void*mallocPointer=::dlsym(libcHandle.p,"malloc");Module libc;char libcPath[PATH_MAX]{};
 if(!moduleAt(reinterpret_cast<std::uintptr_t>(mallocPointer),libc,libcPath)||std::strcmp(libcPath,LibcPath)||!libcIdentity(libc))return false;
 std::uintptr_t mallocAddress{},reallocAddress{},freeAddress{};
 if(!symbol(libcHandle.p,libc,"malloc",mallocAddress)||!symbol(libcHandle.p,libc,"realloc",reallocAddress)||!symbol(libcHandle.p,libc,"free",freeAddress))return false;
 if(!gotTargets(readEngine,engine.base,mallocAddress,reallocAddress,freeAddress))return false;
 CachedCertificate certificate;
 if(!certificate.capture(readEngine,engine.base,mallocAddress,reallocAddress,freeAddress)||!certificate.matches(c))return false;
 if(!r.engine){r.engine=engineHandle.release();r.libc=libcHandle.release();r.base=engine.base;std::memcpy(r.path,path,std::strlen(path)+1);r.file=identity;}
 r.certificate=certificate;r.published.store(&r.certificate,std::memory_order_release);
 return true;
}
}
bool qualify(const VkAllocationCallbacks&c) noexcept {try{return check(c);}catch(...){return false;}}
}
#else
namespace rdr2::worker28::retail_allocator {
bool qualify(const VkAllocationCallbacks&) noexcept {return false;}
}
#endif
