#pragma once
// VR main-thread command queue + executor.
//
// Producers (input thread = NativePadDriver filter, XR thread = VR menu, any other thread) post small POD
// commands into a bounded lock-free MPMC ring; the engine main thread drains it inside the mod's wrapper of
// sagShellDerived::UpdateInputs (GOT 0x2e499c0), only for the call that returns into sagShellDerived::Execute
// (0x23a0340): after ioInput::Update, before the next simulation step, the same thread/lock state the
// engine's own player/inventory code runs on (the main thread). Modules register a per-frame tick (polls,
// publications) and a command handler.
// No locks, no allocation on the hot path; a full ring drops the command (counted), old commands expire (counted).
// Integration: the UpdateInputs wrapper calls run(caller==engineBase+0x23a0340, monotonic ns) AFTER the original and
// markInstalled() once the GOT hook is in place. The executor must not depend on the teleport fingerprints.
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>

namespace rdrvr::vr {

inline std::uint64_t nowNs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Bounded lock-free MPMC queue (D. Vyukov's sequence-per-cell ring). T must be trivially copyable.
template<class T,std::size_t N> class LockFreeQueue {
    static_assert(N>=2&&(N&(N-1))==0,"power of two");
    static_assert(std::is_trivially_copyable_v<T>);
    struct Cell {std::atomic<std::size_t> sequence{};T value{};};
    std::array<Cell,N> cells_{};
    alignas(64) std::atomic<std::size_t> enqueue_{0};
    alignas(64) std::atomic<std::size_t> dequeue_{0};
public:
    LockFreeQueue() noexcept {for(std::size_t i=0;i<N;++i)cells_[i].sequence.store(i,std::memory_order_relaxed);}
    LockFreeQueue(const LockFreeQueue&)=delete;LockFreeQueue& operator=(const LockFreeQueue&)=delete;
    bool push(const T& value) noexcept {
        std::size_t position=enqueue_.load(std::memory_order_relaxed);
        for(;;){
            Cell& cell=cells_[position&(N-1)];
            const std::size_t sequence=cell.sequence.load(std::memory_order_acquire);
            const auto diff=static_cast<std::intptr_t>(sequence)-static_cast<std::intptr_t>(position);
            if(diff==0){
                if(enqueue_.compare_exchange_weak(position,position+1,std::memory_order_relaxed)){
                    cell.value=value;cell.sequence.store(position+1,std::memory_order_release);return true;
                }
            }else if(diff<0)return false; // full
            else position=enqueue_.load(std::memory_order_relaxed);
        }
    }
    bool pop(T& out) noexcept {
        std::size_t position=dequeue_.load(std::memory_order_relaxed);
        for(;;){
            Cell& cell=cells_[position&(N-1)];
            const std::size_t sequence=cell.sequence.load(std::memory_order_acquire);
            const auto diff=static_cast<std::intptr_t>(sequence)-static_cast<std::intptr_t>(position+1);
            if(diff==0){
                if(dequeue_.compare_exchange_weak(position,position+1,std::memory_order_relaxed)){
                    out=cell.value;cell.sequence.store(position+N,std::memory_order_release);return true;
                }
            }else if(diff<0)return false; // empty
            else position=dequeue_.load(std::memory_order_relaxed);
        }
    }
    static constexpr std::size_t capacity(){return N;}
};

namespace mainthread {
// Module ids index the handler table. Cheats take id 2; MELEE (tick only) id 3; BODY (tick only) id 4;
// THROW (tick only = the flight; its ops OpThrow / OpCatch go through the holsters module) id 5;
// RELOAD 6 (ops acks, fires, reconcile), ARC 7 (DEBUG producer, live levers),
// LASSO 8 (shown box, LASSO arc), TELEPORT 9 LAST (state machine, TELEPORT arc, warp: every other tick of the frame sees the pre-warp
// state). Tick order = id order, commands after all ticks. 10..15 free.
enum Module : std::uint16_t {ModuleNone=0,ModuleHolsters=1,ModuleCheats=2,ModuleMelee=3,ModuleBody=4,ModuleThrow=5,
    ModuleReload=6,ModuleArc=7,ModuleLasso=8,ModuleTeleport=9,ModuleMarker=10,ModuleMotionActors=11,ModuleCount=16};
inline constexpr std::array<Module,11> namedModules{ModuleHolsters,ModuleCheats,ModuleMelee,ModuleBody,ModuleThrow,ModuleReload,ModuleArc,
    ModuleLasso,ModuleTeleport,ModuleMarker,ModuleMotionActors};
inline constexpr bool moduleIdsUnique() noexcept {
    for(std::size_t i=0;i<namedModules.size();++i){if(namedModules[i]==ModuleNone||namedModules[i]>=ModuleCount)return false;
        for(std::size_t j=i+1;j<namedModules.size();++j)if(namedModules[i]==namedModules[j])return false;}
    return true;
}
static_assert(moduleIdsUnique(),"module ids unique and < ModuleCount");
static_assert(ModuleReload<ModuleArc&&ModuleArc<ModuleLasso&&ModuleLasso<ModuleTeleport,"tick order: reload, arc, lasso, teleport last");
struct Command {
    std::uint16_t module{},op{};
    std::uint32_t sequence{};           // assigned by post()
    std::int32_t arg[4]{};
    float value[4]{};
    std::uint64_t postedNs{};           // assigned by post() when zero
    std::uint32_t maxAgeMs{};           // 0 = defaultMaxAgeMs
};
inline constexpr std::uint32_t defaultMaxAgeMs=1000;
struct Frame {std::uint64_t now{},index{};};
using Tick=void(*)(const Frame&) noexcept;
using Execute=void(*)(const Command&,const Frame&) noexcept;
struct Handler {std::atomic<Tick> tick{};std::atomic<Execute> execute{};std::atomic<const char*> name{};};

inline std::array<Handler,ModuleCount> handlers{};
inline LockFreeQueue<Command,64> queue{};
inline std::atomic<bool> installed{};
inline std::atomic<std::uint32_t> nextSequence{1};
inline std::atomic<std::uint64_t> posted{},dropped{},executed{},unhandled{},expired{},frames{},otherCalls{},exceptions{},lastFrameNs{};
inline std::array<std::atomic<std::uint64_t>,16> tickNsSum{},tickNsMax{},tickCount{};
inline std::atomic<std::uint64_t> duplicateRegistrations{}; // Refused registrations over an occupied slot
// ARC ledger window: the Frame::index of the run() in progress (main thread), 0 outside run(). Code running before run() in the
// same Execute iteration (the laser at PostCameraUpdate) belongs to window frames + 1.
inline std::atomic<std::uint64_t> runningFrame{};
inline constexpr unsigned maxPerFrame=16;
namespace detail {inline std::atomic_flag registerLock=ATOMIC_FLAG_INIT;}

// Startup (any thread, before or after install). Returns false for a bad id.
// True for an empty slot, for the same (tick, execute) pair again (idempotent re-install) and for
// (nullptr, nullptr) (clears the slot); any other registration over an occupied slot returns false, keeps the first handler and bumps
// duplicateRegistrations.
inline bool registerModule(Module module,const char* name,Tick tick,Execute execute) noexcept {
    if(module==ModuleNone||module>=ModuleCount)return false;
    while(detail::registerLock.test_and_set(std::memory_order_acquire)){}
    struct Unlock{~Unlock(){detail::registerLock.clear(std::memory_order_release);}} unlock;
    auto& h=handlers[module];
    const Tick oldTick=h.tick.load(std::memory_order_acquire);const Execute oldExecute=h.execute.load(std::memory_order_acquire);
    const bool clearing=!tick&&!execute,empty=!oldTick&&!oldExecute,same=oldTick==tick&&oldExecute==execute;
    if(!clearing&&!empty&&!same){duplicateRegistrations.fetch_add(1,std::memory_order_relaxed);return false;}
    // Both entry points are published with release - run() acquires each one separately, so the module state written
    // before registerModule() happens-before the first tick()/execute() regardless of which pointer it observes first.
    h.name.store(clearing?nullptr:name,std::memory_order_relaxed);h.execute.store(execute,std::memory_order_release);
    h.tick.store(tick,std::memory_order_release);return true;
}
inline void markInstalled() noexcept {installed.store(true,std::memory_order_release);}
inline bool executorInstalled() noexcept {return installed.load(std::memory_order_acquire);}
// Any thread. Returns the assigned sequence, 0 when the ring is full or the module id is invalid.
inline std::uint32_t post(Command command,std::uint64_t now=0) noexcept {
    if(command.module==ModuleNone||command.module>=ModuleCount){dropped.fetch_add(1,std::memory_order_relaxed);return 0;}
    std::uint32_t sequence=nextSequence.fetch_add(1,std::memory_order_relaxed);if(!sequence)sequence=nextSequence.fetch_add(1,std::memory_order_relaxed);
    command.sequence=sequence;if(!command.postedNs)command.postedNs=now?now:nowNs();
    if(!queue.push(command)){dropped.fetch_add(1,std::memory_order_relaxed);return 0;}
    posted.fetch_add(1,std::memory_order_relaxed);return sequence;
}
// Main thread only: the UpdateInputs wrapper, after the original. qualified = return address is Execute's call site.
inline void run(bool qualified,std::uint64_t now) noexcept {
    if(!qualified){otherCalls.fetch_add(1,std::memory_order_relaxed);return;}
    const Frame frame{now,frames.fetch_add(1,std::memory_order_relaxed)+1};
    lastFrameNs.store(now,std::memory_order_relaxed);
    runningFrame.store(frame.index,std::memory_order_relaxed);
    struct Done{~Done(){runningFrame.store(0,std::memory_order_relaxed);}} done;
    for(unsigned i=0;i<ModuleCount;++i){auto& h=handlers[i];
        const Tick tick=h.tick.load(std::memory_order_acquire);
        if(tick){const auto t0=nowNs(); // Per-module tick cost (report modules[].tickUs / maxUs)
            try{tick(frame);}catch(...){exceptions.fetch_add(1,std::memory_order_relaxed);}
            const auto d=nowNs()-t0;tickNsSum[i].fetch_add(d,std::memory_order_relaxed);tickCount[i].fetch_add(1,std::memory_order_relaxed);
            if(d>tickNsMax[i].load(std::memory_order_relaxed))tickNsMax[i].store(d,std::memory_order_relaxed);}
    }
    Command command{};
    for(unsigned n=0;n<maxPerFrame&&queue.pop(command);++n){
        const std::uint64_t maxAge=std::uint64_t(command.maxAgeMs?command.maxAgeMs:defaultMaxAgeMs)*1'000'000ull;
        if(now>command.postedNs&&now-command.postedNs>maxAge){expired.fetch_add(1,std::memory_order_relaxed);continue;}
        const Execute execute=command.module<ModuleCount?handlers[command.module].execute.load(std::memory_order_acquire):nullptr;
        if(!execute){unhandled.fetch_add(1,std::memory_order_relaxed);continue;}
        try{execute(command,frame);executed.fetch_add(1,std::memory_order_relaxed);}
        catch(...){exceptions.fetch_add(1,std::memory_order_relaxed);}
    }
}
inline std::string report() {
    char b[512];
    const auto last=lastFrameNs.load(std::memory_order_relaxed);
    std::snprintf(b,sizeof b,"{\"installed\":%s,\"frames\":%llu,\"otherCalls\":%llu,\"posted\":%llu,\"dropped\":%llu,\"executed\":%llu,"
        "\"unhandled\":%llu,\"expired\":%llu,\"exceptions\":%llu,\"lastFrameAgeMs\":%.1f,\"duplicateRegistrations\":%llu",
        installed.load()?"true":"false",(unsigned long long)frames.load(),(unsigned long long)otherCalls.load(),(unsigned long long)posted.load(),
        (unsigned long long)dropped.load(),(unsigned long long)executed.load(),(unsigned long long)unhandled.load(),(unsigned long long)expired.load(),
        (unsigned long long)exceptions.load(),last?double(nowNs()-last)/1e6:-1.0,(unsigned long long)duplicateRegistrations.load());
    std::string out=b;out+=",\"modules\":[";bool first=true; // The registered modules in tick order
    for(unsigned i=1;i<ModuleCount;++i){const auto& h=handlers[i];
        if(!h.tick.load(std::memory_order_relaxed)&&!h.execute.load(std::memory_order_relaxed))continue;
        const char* n=h.name.load(std::memory_order_relaxed);
        out+=first?"":",";first=false;out+="{\"id\":"+std::to_string(i)+",\"name\":\"";out+=n?n:"";
        const auto c=tickCount[i].load(std::memory_order_relaxed);
        out+="\",\"tickUs\":"+std::to_string(c?double(tickNsSum[i].load(std::memory_order_relaxed))/double(c)/1000.0:0.0)+",\"maxUs\":"+std::to_string(double(tickNsMax[i].load(std::memory_order_relaxed))/1000.0)+"}";}
    return out+"]}";
}
} // namespace mainthread
} // namespace rdrvr::vr
