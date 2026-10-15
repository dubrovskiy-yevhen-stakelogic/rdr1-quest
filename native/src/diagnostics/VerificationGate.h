#pragma once
#include <atomic>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cerrno>
#include <sstream>

namespace rdrvr::diagnostics {
// Android may restore NativeActivity without visiting the launcher gate.
// Wait for the existing hash worker, never under the trace/report mutex.
class VerificationGate {
    std::atomic<bool> ready_{};
    std::mutex mutex_;std::condition_variable condition_;
    std::atomic<std::uint64_t> waits_{},timeouts_{},lastWaitNs_{};
public:
    bool ready()const noexcept{return ready_.load(std::memory_order_acquire);}
    void markVerified(){
        const int saved=errno;
        {std::lock_guard lock(mutex_);ready_.store(true,std::memory_order_release);}
        condition_.notify_all();errno=saved;
    }
    bool waitFor(std::chrono::milliseconds timeout){
        const int saved=errno;
        if(ready())return true;
        const auto start=std::chrono::steady_clock::now();++waits_;
        std::unique_lock lock(mutex_);
        const bool result=condition_.wait_for(lock,timeout,[this]{return ready();});
        lastWaitNs_=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
        if(!result)++timeouts_;lock.unlock();errno=saved;return result;
    }
    std::string report()const{
        std::ostringstream o;o<<"{\"waits\":"<<waits_<<",\"timeouts\":"<<timeouts_<<",\"lastWaitNs\":"<<lastWaitNs_<<'}';return o.str();
    }
};
}
