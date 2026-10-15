#include "SelfMemoryReader.h"
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <algorithm>
#include <sys/uio.h>
#include <cstdint>
namespace rdrvr::diagnostics {
namespace {
bool valid(std::uintptr_t source,const void* target,std::size_t size){
    return source&&target&&size&&size<=65536&&source<=UINTPTR_MAX-size;
}
ssize_t copySelf(std::uintptr_t source,void* target,std::size_t size){
    iovec local{target,size},remote{reinterpret_cast<void*>(source),size};
    // Resolve this process's PID here, including after fork; never retain a parent PID.
    ssize_t result;do{result=process_vm_readv(getpid(),&local,1,&remote,1,0);}while(result<0&&errno==EINTR);
    return result;
}
}
SelfMemoryReader::~SelfMemoryReader() {for(int fd:fds_)if(fd>=0)close(fd);}
bool SelfMemoryReader::initialize() {
    std::lock_guard guard(mutex_);
    if(ready_)return true;
    for(int& fd:fds_)if(fd>=0){close(fd);fd=-1;}
    vector_=false;
    // Probe only this process's own known storage. No foreign PID or unchecked native
    // pointer is accepted. A denied operation retains the proven pipe route.
    const std::uint64_t source=0x59730ade619284bcull;std::uint64_t target{};
    if(vectorAllowed_&&copySelf(reinterpret_cast<std::uintptr_t>(&source),&target,sizeof target)==sizeof target&&target==source){
        vector_=true;error_=0;ready_=true;return true;
    }
    if(pipe2(fds_,O_CLOEXEC|O_NONBLOCK)!=0){error_=errno;return false;}
    error_=0;ready_=true;return true;
}
bool SelfMemoryReader::read(std::uintptr_t source,void* target,std::size_t size) {
    if(!valid(source,target,size)){error_=EINVAL;return false;}
    if(!ready_){error_=EBADF;return false;}
    if(vector_){
        // No shared transfer buffer, pipe mutex, or 512-byte chunk loop.
        // EFAULT/partial-page reads are failures, never successful snapshots.
        const auto copied=copySelf(source,target,size);
        if(copied!=static_cast<ssize_t>(size)){error_=copied<0?errno:EFAULT;return false;}
        error_=0;return true;
    }
    std::lock_guard guard(mutex_);
    if(!ready_){error_=EBADF;return false;}
    auto* dest=static_cast<unsigned char*>(target);
    while(size) {
        // <= POSIX minimum PIPE_BUF, always drained before the next write.
        const auto chunk=std::min<std::size_t>(size,512);
        ssize_t written;
        do {written=::write(fds_[1],reinterpret_cast<const void*>(source),chunk);}while(written<0 && errno==EINTR);
        if(written<=0){error_=written<0?errno:EIO;return false;}
        ssize_t copied;
        do {copied=::read(fds_[0],dest,static_cast<std::size_t>(written));}while(copied<0 && errno==EINTR);
        if(copied!=written) {
            // Internal pipe state is uncertain: disable further reads, no reuse.
            error_=copied<0?errno:EIO;ready_=false;return false;
        }
        if(written!=static_cast<ssize_t>(chunk)){error_=EFAULT;return false;}
        source+=chunk;dest+=chunk;size-=chunk;
    }
    error_=0;return true;
}
bool SelfMemoryReader::readv(const ReadSegment* segments,std::size_t count) {
    if(!count){error_=0;return true;}
    if(!segments){error_=EINVAL;return false;}
    for(std::size_t i=0;i<count;++i)if(!valid(segments[i].source,segments[i].target,segments[i].size)){error_=EINVAL;return false;}
    if(!ready_){error_=EBADF;return false;}
    if(!vector_){
        for(std::size_t i=0;i<count;++i)if(!read(segments[i].source,segments[i].target,segments[i].size))return false;
        return true;
    }
    constexpr std::size_t batch=64; // well below IOV_MAX (1024)
    iovec local[batch],remote[batch];
    for(std::size_t i=0;i<count;){
        const auto n=std::min(batch,count-i);std::size_t total{};
        for(std::size_t j=0;j<n;++j){const auto& s=segments[i+j];
            local[j]={s.target,s.size};remote[j]={reinterpret_cast<void*>(s.source),s.size};total+=s.size;}
        // Remote and local iovecs pair one to one; a fault ends the transfer early.
        ssize_t copied;do{copied=process_vm_readv(getpid(),local,n,remote,n,0);}while(copied<0&&errno==EINTR);
        if(copied!=static_cast<ssize_t>(total)){error_=copied<0?errno:EFAULT;return false;}
        i+=n;
    }
    error_=0;return true;
}
}
