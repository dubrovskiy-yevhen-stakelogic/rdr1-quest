#pragma once
#include "EngineFramebuffers.h"
#include "RetailResources.h"
#include <mutex>
#include <string>

namespace rdrvr::engine {
// Normalize only verified native world framebuffer binds inside the selected
// primary-eye interval. This is the native target's intended full raster, not
// a temporary private framebuffer or a global-window override.
class NativePrimaryRaster {
public:
    struct Api {EngineFramebufferApi framebuffer{};RetailResourceApi resources{};};
private:
    Api api_{};mutable std::mutex mutex_;std::thread::id owner_{};
    bool active_{},failed_{};
    void *renderer_{},*gfx_{},*context_{},*collector_{},*postfx_{},*lights_{},*depth_{};
    std::array<void*,3> targets_{},surfaces_{};void* depthSurface_{};
    std::uint32_t width_{},height_{};
    std::uint64_t begins_{},declined_{},bounds_{},matched_{},corrected_{},unchanged_{},ignored_{},faults_{};
    std::array<std::uint64_t,3> roleMatches_{},roleCorrections_{};
    EngineRasterViewport beforeViewport_{},afterViewport_{};
    EngineRasterScissor beforeScissor_{},afterScissor_{};
    unsigned lastRole_{};const char* status_{"NOT_CONFIGURED"};
    bool identities() const;
    void fail(const char*) noexcept;
    void forget() noexcept;
public:
    void configure(Api) noexcept;
    bool begin(void* renderer,std::uint32_t width,std::uint32_t height) noexcept;
    // Observe AFTER existing HUD/native routing, never before its substitution.
    void bound(void* gfx,void* requested,bool nativeResult) noexcept;
    // Forget borrowed identities. Later native assignments own subsequent
    // targets; the stereo pass preserves the corrected final native raster.
    bool end() noexcept;
    bool good() const noexcept;
    std::string report() const;
};
inline NativePrimaryRaster nativePrimaryRaster;
}
