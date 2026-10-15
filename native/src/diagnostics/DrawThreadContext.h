#pragma once
// The per-thread DrawCommon state of BodyDrawFilter, NativeDrawSuppression and
// DrawStageTimings in ONE thread_local. librdrvr is dlopen'ed, so every distinct
// thread_local costs its own TLSDESC resolver call per function; one struct lets the
// compiler resolve it once per function. Trivial and constant-initialized (no TLS guard).
// Each owner reads/writes only its own field and keeps its own nesting discipline.
namespace rdrvr::diagnostics {
struct DrawThreadContext {void* bodyFilter{};void* suppression{};void* stageTimings{};};
inline thread_local DrawThreadContext drawThreadContext;
}
