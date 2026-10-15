#pragma once
#include "FrameTimings.h"
#if defined(__ANDROID__) || defined(__linux__)
#include <time.h>
#endif

namespace rdrvr::diagnostics {
// Scheduled user + kernel time of this thread only. Child workers, GPU work,
// descheduling and blocking waits are excluded. Unsupported clocks return N/A.
inline std::uint64_t threadCpuNs() noexcept {
    const int saved=errno;std::uint64_t value{};
#if defined(__ANDROID__) || defined(__linux__)
    timespec ts{};
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts)==0&&ts.tv_sec>=0&&ts.tv_nsec>=0&&ts.tv_nsec<1'000'000'000)
        value=static_cast<std::uint64_t>(ts.tv_sec)*1'000'000'000ull+static_cast<std::uint64_t>(ts.tv_nsec);
#endif
    errno=saved;return value;
}
class ThreadCpuScope {
public:
    using Clock=std::uint64_t(*)() noexcept;
private:
    inline static thread_local unsigned ownerToken_{};
    DurationWindow* window_{};Clock clock_;const void* owner_{&ownerToken_};std::uint64_t ticket_{profilerTicket()},start_{};
public:
    explicit ThreadCpuScope(DurationWindow* window,Clock clock=threadCpuNs) noexcept:window_(window),clock_(clock){
        const int saved=errno;if(!(ticket_&1))window_=nullptr;if(window_&&clock_)start_=clock_();errno=saved;
    }
    explicit ThreadCpuScope(DurationWindow& window,Clock clock=threadCpuNs) noexcept:ThreadCpuScope(&window,clock){}
    ThreadCpuScope(const ThreadCpuScope&)=delete;
    ThreadCpuScope& operator=(const ThreadCpuScope&)=delete;
    ~ThreadCpuScope(){finish();}
    void finish() noexcept {
        if(!window_)return;const int saved=errno;
        if(start_&&clock_&&owner_==&ownerToken_&&profilerTicketValid(ticket_))window_->elapsed(start_,clock_());
        window_=nullptr;errno=saved;
    }
};
struct ThreadCpuTimings {
    DurationWindow nativeSceneInclusive,stereoDraw,stockDepth,stockLight,stockColor,
        leftDepth,leftLight,leftColor,rightDepth,rightLight,rightColor;
    std::string report()const{
        std::ostringstream out;
        out<<"{\"units\":\"nanoseconds\",\"clock\":\"thread-cpu\",\"includesDriverWaits\":false,\"includesWorkerThreads\":false";
        for(const auto& [name,window]:std::array<std::pair<const char*,const DurationWindow*>,11>{{
            {"nativeSceneInclusive",&nativeSceneInclusive},{"stereoDraw",&stereoDraw},
            {"stockDepth",&stockDepth},{"stockLight",&stockLight},{"stockColor",&stockColor},
            {"leftDepth",&leftDepth},{"leftLight",&leftLight},{"leftColor",&leftColor},
            {"rightDepth",&rightDepth},{"rightLight",&rightLight},{"rightColor",&rightColor}}})
            out<<",\""<<name<<"\":"<<window->snapshot().json();
        return out.str()+'}';
    }
};
inline ThreadCpuTimings threadCpuTimings;
}
