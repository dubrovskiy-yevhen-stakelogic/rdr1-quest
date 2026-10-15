#pragma once
#include "GuardedReadVector.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
namespace rdrvr::diagnostics {
// Kernel-checked copies from the process's own address space. Prefer one self
// readv; fall back to the private pipe backend when that operation is unavailable.
// Unreadable input becomes EFAULT, without an unchecked userspace dereference.
class SelfMemoryReader {
    std::mutex mutex_;
    int fds_[2]{-1,-1};
    std::atomic<int> error_{};
    std::atomic<bool> ready_{};
    const bool vectorAllowed_;
    std::atomic<bool> vector_{};
public:
    explicit SelfMemoryReader(bool vectorAllowed=true):vectorAllowed_(vectorAllowed){}
    ~SelfMemoryReader();
    SelfMemoryReader(const SelfMemoryReader&)=delete;
    SelfMemoryReader& operator=(const SelfMemoryReader&)=delete;
    bool initialize();
    bool read(std::uintptr_t source,void* target,std::size_t size);
    // All segments in one process_vm_readv per 64 segments (pipe backend: one
    // read per segment). Same per-segment limits as read(); any short copy fails all.
    bool readv(const ReadSegment* segments,std::size_t count);
    bool ready() const {return ready_.load();}
    int error() const {return error_.load();}
    const char* backend() const {return !ready_?"NOT_READY":vector_?"self-process-vm-readv":"private-pipe";}
};
}
