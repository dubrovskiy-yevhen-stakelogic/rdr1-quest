#include "TreeTimerRouting.h"
namespace rdrvr::engine {
TreeTimerRouting treeTimerRouting;
extern "C" __attribute__((visibility("hidden"))) TreeTimerReturn rdrvrTreeTimerDispatch(const void* display,std::uintptr_t caller){
    return treeTimerRouting.dispatch(display,caller);
}
#if defined(__aarch64__)
// AAPCS64: two-register struct result (x0 value, x1 optional continuation).
// Keep the incoming LR and frame chain intact, including for the skipped end.
// RET is intentional: this is a verified caller continuation, not an indirect
// call to an arbitrary middle-of-function BTI landing pad. No text is modified.
extern "C" __attribute__((naked)) unsigned rdrvrTreeTimerBuffers(const void*){
    asm volatile(
        "bti c\n"
        "stp x29, x30, [sp, #-16]!\n"
        "mov x29, sp\n"
        "mov x1, x30\n"
        "bl rdrvrTreeTimerDispatch\n"
        "mov x16, x1\n"
        "ldp x29, x30, [sp], #16\n"
        "cbz x16, 1f\n"
        "ret x16\n"
        "1: ret\n");
}
#endif
}
