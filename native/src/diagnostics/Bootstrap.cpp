#include <android/log.h>
#include <jni.h>
#include <atomic>
#include "contracts/Contracts.h"
#include "diagnostics/ProfilerControl.h"
#include "diagnostics/SaveTransfer.h" // save01
#ifdef RDRVR_BENCH_CONTROL
#include "runtime/BenchMenuInput.h"
#include "diagnostics/FrameTimings.h"
#endif
#ifdef RDRVR_PROBE
#include "engine/Observer.h"
#endif
#ifdef RDRVR_TRACE
#include "engine/TraceEntry.h"
#ifdef RDRVR_HEAD_CAMERA
#include "engine/PlannerReplayCapture.h"
#endif
#endif

extern "C" JNIEXPORT jboolean JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeProfilerEnabled(JNIEnv*,jclass) {
    return rdrvr::diagnostics::profilingEnabled()?JNI_TRUE:JNI_FALSE;
}

namespace {
std::atomic<unsigned> eventCount{0};
constexpr unsigned kLogLimit = 256;
#if defined(RDRVR_TRACE)
constexpr const char* mode = "vulkan-call-trace";
#elif defined(RDRVR_PROBE)
constexpr const char* mode = "read-only-observation";
#else
constexpr const char* mode = "noop";
#endif
}
extern "C" JNIEXPORT jboolean JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeBenchMenuA(JNIEnv*,jclass) {
#ifdef RDRVR_BENCH_CONTROL
    return rdrvr::runtime::benchMenuInput.request(rdrvr::diagnostics::monotonicNs());
#else
    return false;
#endif
}
extern "C" JNIEXPORT jboolean JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeEnableShaderCapture(JNIEnv*,jclass) {
#if defined(RDRVR_BENCH_CONTROL) && defined(RDRVR_TRACE)
    return rdrvr::engine::traceEnableShaderCapture();
#else
    return false;
#endif
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeTakeShader(JNIEnv* env,jclass) {
#if defined(RDRVR_BENCH_CONTROL) && defined(RDRVR_TRACE)
    try {
        auto bytes=rdrvr::engine::traceTakeShader();if(bytes.empty())return nullptr;
        auto result=env->NewByteArray(static_cast<jsize>(bytes.size()));
        if(result)env->SetByteArrayRegion(result,0,static_cast<jsize>(bytes.size()),reinterpret_cast<const jbyte*>(bytes.data()));
        return result;
    }catch(...) {return nullptr;}
#else
    (void)env;return nullptr;
#endif
}
extern "C" JNIEXPORT jboolean JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeRequestPlannerCapture(JNIEnv*,jclass) {
#if defined(RDRVR_BENCH_CONTROL) && defined(RDRVR_TRACE) && defined(RDRVR_HEAD_CAMERA)
    return rdrvr::engine::plannerReplayCapture.request();
#else
    return false;
#endif
}
extern "C" JNIEXPORT void JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeCancelPlannerCapture(JNIEnv*,jclass) {
#if defined(RDRVR_BENCH_CONTROL) && defined(RDRVR_TRACE) && defined(RDRVR_HEAD_CAMERA)
    rdrvr::engine::plannerReplayCapture.cancelPending();
#endif
}
extern "C" JNIEXPORT jint JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativePlannerCaptureState(JNIEnv*,jclass) {
#if defined(RDRVR_BENCH_CONTROL) && defined(RDRVR_TRACE) && defined(RDRVR_HEAD_CAMERA)
    return static_cast<jint>(rdrvr::engine::plannerReplayCapture.state());
#else
    return -1;
#endif
}
extern "C" JNIEXPORT jstring JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativePlannerCaptureReport(JNIEnv* env,jclass) {
#if defined(RDRVR_BENCH_CONTROL) && defined(RDRVR_TRACE) && defined(RDRVR_HEAD_CAMERA)
    try {return env->NewStringUTF(rdrvr::engine::plannerReplayCapture.report().c_str());}catch(...){}
#endif
    return env->NewStringUTF("{}");
}
extern "C" JNIEXPORT void JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeCompletePlannerCapture(JNIEnv*,jclass,jboolean writerExited) {
    (void)writerExited;
#if defined(RDRVR_BENCH_CONTROL) && defined(RDRVR_TRACE) && defined(RDRVR_HEAD_CAMERA)
    if(writerExited)rdrvr::engine::plannerReplayCapture.closeWriter();
    else rdrvr::engine::plannerReplayCapture.completeWrite();
#endif
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeTakePlannerCapture(JNIEnv* env,jclass) {
    (void)env;
#if defined(RDRVR_BENCH_CONTROL) && defined(RDRVR_TRACE) && defined(RDRVR_HEAD_CAMERA)
    try {
        auto bytes=rdrvr::engine::plannerReplayCapture.take();if(bytes.empty()||bytes.size()>32*1024*1024)return nullptr;
        auto result=env->NewByteArray(static_cast<jsize>(bytes.size()));
        if(result)env->SetByteArrayRegion(result,0,static_cast<jsize>(bytes.size()),reinterpret_cast<const jbyte*>(bytes.data()));
        return env->ExceptionCheck()?nullptr:result;
    }catch(...){}
#endif
    return nullptr;
}
extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) {
    __android_log_print(ANDROID_LOG_INFO, "RdrVr", "build=%s contracts=%u mode=%s gameplay_hooks=disabled",
                       RDRVR_BUILD_ID, rdrvr::kContractVersion, mode);
    rdrvr::diagnostics::savexfer::run(); // save01: export the private saves, apply <rdrvr>/save-import before the game opens them
    return JNI_VERSION_1_6;
}
#ifdef RDRVR_TRACE
extern "C" JNIEXPORT void JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeTraceConfigure(JNIEnv* env,jclass,jstring path) {
    if(!path)return;const char* p=env->GetStringUTFChars(path,nullptr);if(!p)return;
    try {rdrvr::engine::traceConfigure(p);}catch(...){}
    env->ReleaseStringUTFChars(path,p);
}
extern "C" JNIEXPORT void JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeTraceVerified(JNIEnv* env,jclass,jstring path,jstring sha) {
    if(!path||!sha)return;const char* p=env->GetStringUTFChars(path,nullptr);if(!p)return;
    const char* s=env->GetStringUTFChars(sha,nullptr);if(!s){env->ReleaseStringUTFChars(path,p);return;}
    try {rdrvr::engine::traceVerified(p,s);}catch(...){}
    env->ReleaseStringUTFChars(path,p);env->ReleaseStringUTFChars(sha,s);
}
extern "C" JNIEXPORT jstring JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeTraceReport(JNIEnv* env,jclass) {
    try {return env->NewStringUTF(rdrvr::engine::traceReport().c_str());}
    catch(...) {return env->NewStringUTF("{\"status\":\"REPORT_FAILED\"}");}
}
#endif
extern "C" JNIEXPORT jstring JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeProbe(JNIEnv* env,jclass,jstring path,jstring hash) {
#ifdef RDRVR_PROBE
    if(!path || !hash) return env->NewStringUTF("{\"status\":\"INVALID_ARGUMENT\"}");
    const char* p=env->GetStringUTFChars(path,nullptr);
    if(!p) return nullptr;
    const char* h=env->GetStringUTFChars(hash,nullptr);
    if(!h) {env->ReleaseStringUTFChars(path,p);return nullptr;}
    jstring result{};
    try {result=env->NewStringUTF(rdrvr::engine::observe(p,h).c_str());}
    catch(...) {result=env->NewStringUTF("{\"status\":\"NATIVE_EXCEPTION\",\"hooksEnabled\":false}");}
    env->ReleaseStringUTFChars(path,p); env->ReleaseStringUTFChars(hash,h);
    return result;
#else
    (void)path; (void)hash;
    return env->NewStringUTF("{\"status\":\"DISABLED\",\"hooksEnabled\":false}");
#endif
}
extern "C" JNIEXPORT void JNICALL
Java_org_rdrvr_bootstrap_RdrVrApplication_nativeEvent(JNIEnv* env, jclass, jint event, jstring activity) {
    const auto count = eventCount.fetch_add(1, std::memory_order_relaxed);
    if (count >= kLogLimit) return;
    const char* name = activity ? env->GetStringUTFChars(activity, nullptr) : nullptr;
    if (activity && !name) return; // leave pending JVM exception intact
    __android_log_print(ANDROID_LOG_INFO, "RdrVr", "lifecycle=%d activity=%s sequence=%u",
                       event, name ? name : "application", count);
    if (name) env->ReleaseStringUTFChars(activity, name);
    if (count + 1 == kLogLimit)
        __android_log_write(ANDROID_LOG_INFO, "RdrVr", "lifecycle log limit reached");
}
