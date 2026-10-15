#pragma once
#include <atomic>
#include <cmath>
#include <sstream>
namespace rdrvr::runtime {
struct ResolutionTelemetry {
    std::atomic<float> nativeScale{-1};
    std::atomic<unsigned> recommendedWidth{},recommendedHeight{};
    std::string report()const{
        std::ostringstream o;o<<"{\"nativeScale\":"<<nativeScale.load(std::memory_order_relaxed)
          <<",\"recommendedEyeWidth\":"<<recommendedWidth.load(std::memory_order_relaxed)
          <<",\"recommendedEyeHeight\":"<<recommendedHeight.load(std::memory_order_relaxed)<<'}';return o.str();
    }
};
inline ResolutionTelemetry resolutionTelemetry;
}
