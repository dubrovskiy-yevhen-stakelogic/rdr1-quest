#pragma once
#include "FrameTimings.h"
#include <cstring>
namespace rdrvr::diagnostics {
// Exact bounded event history; fixed storage and no formatting on the draw thread.
// calls: kernel-checked copies (one per process_vm_readv, a vectored call counts once);
// segments: copies served by them; direct: plain copies from always-mapped engine ranges.
struct ReadWork {std::uint64_t calls{},bytes{},failures{},direct{},segments{};};
inline thread_local ReadWork guardedReads;
class EventHistory {
public:
    struct Event {std::uint64_t time{},id{},duration{},generation{};std::uint32_t width{},height{};char state[64]{};ReadWork reads{};bool diagnosticCapture{};};
    static constexpr unsigned capacity=512;
private:
    mutable std::mutex mutex_;std::array<Event,capacity> rows_{};unsigned next_{},size_{};std::uint64_t total_{};
public:
    struct Rate {double hz{};unsigned events{},width{},height{};std::uint64_t ageNs{};};
    Event latest()const{std::lock_guard lock(mutex_);return size_?rows_[(next_+capacity-1)%capacity]:Event{};}
    Rate recent(const char* state,std::uint64_t now,std::uint64_t window=2'000'000'000ull)const{
        Rate out{};std::uint64_t first{},last{};
        const auto barrier=captureRateBarrierNs.load(std::memory_order_acquire);
        std::lock_guard lock(mutex_);
        for(unsigned i=0;i<size_;++i){const auto& e=rows_[(next_+capacity-size_+i)%capacity];
            if(e.diagnosticCapture||(barrier&&e.time<=barrier)||(state&&std::strcmp(e.state,state))||e.time>now)continue;
            out.width=e.width;out.height=e.height;out.ageNs=now-e.time;
            if(now-e.time>window)continue;if(!first)first=e.time;last=e.time;++out.events;
        }
        if(out.events>1&&last>first)out.hz=double(out.events-1)*1e9/double(last-first);
        return out;
    }
    void push(std::uint64_t id,const char* state,std::uint64_t duration=0,unsigned width=0,unsigned height=0,std::uint64_t generation=0,std::uint64_t now=monotonicNs(),ReadWork reads={}){
        Event e{now,id,duration,generation,width,height};e.reads=reads;
        e.diagnosticCapture=captureMeasurementExclusion.load(std::memory_order_acquire);std::strncpy(e.state,state,sizeof(e.state)-1);
        std::lock_guard lock(mutex_);rows_[next_]=e;next_=(next_+1)%capacity;if(size_<capacity)++size_;++total_;
    }
    std::string report()const{
        std::array<Event,capacity> copy;unsigned size,next;std::uint64_t total;
        {std::lock_guard lock(mutex_);copy=rows_;size=size_;next=next_;total=total_;}
        std::ostringstream o;o<<"{\"capacity\":"<<capacity<<",\"total\":"<<total<<",\"rows\":[";
        for(unsigned i=0;i<size;++i){const auto& e=copy[(next+capacity-size+i)%capacity];if(i)o<<',';
            o<<"{\"ns\":"<<e.time<<",\"id\":"<<e.id<<",\"durationNs\":"<<e.duration<<",\"state\":\""<<e.state
             <<"\",\"width\":"<<e.width<<",\"height\":"<<e.height<<",\"readCalls\":"<<e.reads.calls<<",\"readBytes\":"<<e.reads.bytes<<",\"readFailures\":"<<e.reads.failures<<",\"generation\":"<<e.generation<<",\"diagnosticCapture\":"<<(e.diagnosticCapture?"true":"false")<<'}';}
        return o.str()+"]}";
    }
};
inline EventHistory worldHistory,stereoHistory,presentHistory,worldFailureHistory;
class WorldEvent {
    ReadWork before_{guardedReads};
    std::uint64_t ticket_{profilerTicket()},start_{(ticket_&1)?monotonicNs():0},id_{};const char* result_{"EXCEPTION"};
public:
    explicit WorldEvent(std::uint64_t id):id_(id){}
    void result(const char* value){result_=value;}
    ~WorldEvent(){if(!profilerTicketValid(ticket_))return;const auto now=monotonicNs();worldHistory.push(id_,result_,now-start_,0,0,0,now,{guardedReads.calls-before_.calls,guardedReads.bytes-before_.bytes,guardedReads.failures-before_.failures});}
};
}
