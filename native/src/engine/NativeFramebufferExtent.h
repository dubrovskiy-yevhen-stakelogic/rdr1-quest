#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace rdrvr::engine {
// Initial display depth keeps full capacity and logical size. The pinned native
// framebuffer constructor otherwise chooses MAX(attachments), which is invalid
// when that depth accompanies a smaller Android swapchain image. Correct only
// framebuffers retaining a depth created by this boundary, before publication.
class NativeFramebufferExtent {
public:
    using DepthCreate=bool(*)(void*,const void*);
    using FramebufferCtor=void(*)(void*,const void*,std::uint32_t);
    struct Api {
        DepthCreate depthCreate{};
        FramebufferCtor framebufferCtor{};
        bool (*requestedExtent)(void*,std::array<std::uint32_t,2>&){};
        void (*depthCreated)(void*,void*,std::uint32_t,std::uint32_t,bool){};
        void* user{};
        std::uintptr_t depthVtable{},colorVtable{},textureVtable{},framebufferVtable{};
        const void* displayDepthName{};
        std::uintptr_t displayVtable{};
    };
    struct Stats {
        bool ready{},installed{},fault{};
        std::uint64_t depthCalls{},depthQualified{},depthEnlarged{},depthFailures{};
        std::uint64_t framebufferCalls{},trackedFramebuffers{},clampedFramebuffers{};
        std::uint32_t lastWidth{},lastHeight{};
        std::uint64_t depthCallerMatched{},depthNameMatched{},depthRequestReady{};
        std::uint64_t displayChecks{},displayAbsent{},displayPresent{},displayRejected{};
    };
    bool configure(Api) noexcept;
    void installed(bool value) noexcept;
    bool ready()const noexcept{return ready_.load(std::memory_order_acquire);}
    bool faulted()const noexcept{return fault_.load(std::memory_order_acquire);}
    bool createDepth(void* surface,const void* descriptor,bool qualifiedDeviceCaller);
    // Only after the pinned initial CreateBackbuffers has returned. Some native
    // display configurations deliberately create color-only swapchain buffers;
    // their world depth is reserved by Renderer Init instead.
    bool confirmNoDisplayDepth(void* display,bool exactInitialCaller) noexcept;
    void constructFramebuffer(void* framebuffer,const void* descriptor,std::uint32_t hash);
    // Call only after all native users have stopped, before native device
    // teardown. Records own no native reference and are never dereferenced.
    void resetAfterIdle() noexcept;
    Stats snapshot()const noexcept;
    std::string report()const;
private:
    struct Depth {void* surface{};void* texture{};};
    Api api_{};
    std::atomic<bool> ready_{},installed_{},fault_{};
    std::atomic<std::uint64_t> depthCalls_{},depthQualified_{},depthEnlarged_{},depthFailures_{};
    std::atomic<std::uint64_t> framebufferCalls_{},trackedFramebuffers_{},clampedFramebuffers_{};
    std::atomic<std::uint32_t> lastWidth_{},lastHeight_{};
    std::atomic<std::uint64_t> depthCallerMatched_{},depthNameMatched_{},depthRequestReady_{};
    std::atomic<std::uint64_t> displayChecks_{},displayAbsent_{},displayPresent_{},displayRejected_{};
    std::mutex mutex_;std::vector<Depth> depths_;
};
inline NativeFramebufferExtent nativeFramebufferExtent;
}
