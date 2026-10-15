#pragma once
#include "SelfMemoryReader.h"
#include <cerrno>
namespace rdrvr::diagnostics {
// A process-lifetime facade over one guarded reader per calling thread. Camera,
// render and diagnostics work never compete for one pipe mutex. Facades on the
// same thread share that channel; reads contain no callbacks or reentrant work.
// Thread exit destroys the channel and closes both descriptors.
class ThreadMemoryReader {
    struct Channel {
        SelfMemoryReader reader;bool attempted{};
        bool ensure(){if(!attempted){attempted=true;return reader.initialize();}return reader.ready();}
    };
    static Channel& channel(){static thread_local Channel value;return value;}
    std::atomic<bool> ready_{};std::atomic<int> error_{};
    std::atomic<const char*> backend_{"NOT_READY"};
public:
    bool initialize(){
        if(ready_)return true;
        auto& local=channel();
        // Recovery of a quarantined channel requires explicit initialization.
        local.attempted=true;const bool good=local.reader.initialize();
        backend_=local.reader.backend();
        error_=local.reader.error();ready_=good;return good;
    }
    bool read(std::uintptr_t source,void* target,std::size_t size){
        if(!ready_){error_=EBADF;return false;}
        auto& local=channel();
        if(!local.ensure()){error_=local.reader.error();return false;}
        const bool good=local.reader.read(source,target,size);error_=local.reader.error();
        // Never silently retry a copy after the underlying channel is uncertain.
        if(!local.reader.ready())ready_=false;
        return good;
    }
    bool readv(const ReadSegment* segments,std::size_t count){
        if(!ready_){error_=EBADF;return false;}
        auto& local=channel();
        if(!local.ensure()){error_=local.reader.error();return false;}
        const bool good=local.reader.readv(segments,count);error_=local.reader.error();
        if(!local.reader.ready())ready_=false;
        return good;
    }
    bool ready()const{return ready_.load();}
    int error()const{return error_.load();}
    const char* backend()const{return backend_.load();}
};
}
