#pragma once
#include "CameraAdapter.h"
#include <atomic>
#include <string>

namespace rdrvr::engine {
class SceneCameraBridge {
    runtime::TrackedViewsMailbox& views_;
    mutable std::mutex mutex_;
    CameraAdapter adapter_;
    std::uint64_t scenes_{},decoded_{},prepared_{},missingViews_{},badCamera_{},badPair_{};
    std::uint64_t firstThread_{},otherThreadScenes_{};
    std::atomic<std::uint64_t> busy_{};
    StereoCameraPlan last_{};
    StockCamera stock_{};
public:
    using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    explicit SceneCameraBridge(runtime::TrackedViewsMailbox& views):views_(views){}
    bool observe(std::uintptr_t viewport,std::uint64_t timeNs,std::uint64_t thread,
                 Reader read,void* context,StereoCameraPlan* prepared=nullptr);
    std::string report() const;
};
} // namespace rdrvr::engine
