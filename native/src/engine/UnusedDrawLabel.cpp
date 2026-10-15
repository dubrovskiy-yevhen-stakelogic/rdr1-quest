#include "UnusedDrawLabel.h"
#include <atomic>
#include <mutex>

#if defined(__GNUC__)
#define RDRVR_LABEL_HIDDEN __attribute__((visibility("hidden")))
#else
#define RDRVR_LABEL_HIDDEN
#endif
extern "C" {
RDRVR_LABEL_HIDDEN std::atomic<std::uintptr_t> rdrvr_unused_label_original{};
RDRVR_LABEL_HIDDEN std::atomic<std::uintptr_t> rdrvr_unused_label_return{};
RDRVR_LABEL_HIDDEN std::atomic<std::uint64_t> rdrvr_unused_label_skipped{};
}
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(sizeof(std::atomic<std::uintptr_t>)==sizeof(std::uintptr_t));
static_assert(sizeof(std::atomic<std::uint64_t>)==sizeof(std::uint64_t));

#if defined(__aarch64__)
extern "C" RDRVR_LABEL_HIDDEN __attribute__((naked,noinline)) char*
rdrvr_unused_label_entry(char*,const char*,...) {
    // x16/x17 are the AAPCS64 veneer scratch registers. A decline leaves all
    // argument registers (including x8), SIMD registers, SP and LR untouched.
    // Only the accepted call changes caller-saved x9/x10 to count one elision.
    __asm__ volatile(
        "adrp x16, rdrvr_unused_label_return\n"
        "add x16, x16, :lo12:rdrvr_unused_label_return\n"
        "ldar x17, [x16]\n"
        "cmp x30, x17\n"
        "b.ne 2f\n"
        "cbz x0, 2f\n"
        "strb wzr, [x0]\n"
        "adrp x17, rdrvr_unused_label_skipped\n"
        "add x17, x17, :lo12:rdrvr_unused_label_skipped\n"
        "1: ldxr x9, [x17]\n"
        "add x9, x9, #1\n"
        "stxr w10, x9, [x17]\n"
        "cbnz w10, 1b\n"
        "ret\n"
        "2: adrp x16, rdrvr_unused_label_original\n"
        "add x16, x16, :lo12:rdrvr_unused_label_original\n"
        "ldar x16, [x16]\n"
        "br x16\n");
}
#endif

namespace rdrvr::engine {
bool UnusedDrawLabel::configure(std::uintptr_t original,std::uintptr_t exactReturn) noexcept {
    if(!original||!exactReturn||(original&3)||(exactReturn&3)||original==exactReturn)return false;
    static std::mutex configuration;
    try{
        std::lock_guard<std::mutex> lock(configuration);
        const auto existing=rdrvr_unused_label_original.load(std::memory_order_acquire);
        if(existing)return existing==original&&rdrvr_unused_label_return.load(std::memory_order_acquire)==exactReturn;
        rdrvr_unused_label_original.store(original,std::memory_order_release);
        rdrvr_unused_label_return.store(exactReturn,std::memory_order_release);
        return true;
    }catch(...){return false;}
}
bool UnusedDrawLabel::ready()const noexcept {
    return rdrvr_unused_label_return.load(std::memory_order_acquire)!=0;
}
std::uintptr_t UnusedDrawLabel::entry()const noexcept {
#if defined(__aarch64__)
    return ready()?reinterpret_cast<std::uintptr_t>(&rdrvr_unused_label_entry):0;
#else
    return 0; // No generic C/C++ forwarding substitute for a native ABI thunk.
#endif
}
std::uint64_t UnusedDrawLabel::skipped()const noexcept {
    return rdrvr_unused_label_skipped.load(std::memory_order_relaxed);
}
}
