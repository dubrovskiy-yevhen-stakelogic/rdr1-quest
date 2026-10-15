#pragma once
#include "EyeViewports.h"
#include "StereoVisibility.h"
#include <memory>

namespace rdrvr::engine {
struct CullViewportApi {
    ViewportApi viewport{};
    void (*nearClip)(void*,float){};
    void (*farClip)(void*,float){};
    explicit operator bool()const{return viewport && nearClip && farClip;}
};
// CPU visibility work borrows this address after PrepareVisibilityList returns.
// It is never installed as the drawing camera. Only the owning game thread may
// prepare/lease/retire it; retirement requires completion of ALL CPU consumers,
// including draw callbacks, not merely the visibility worker semaphore.
class CullViewport {
    CullViewportApi api_{};
    std::unique_ptr<ViewportStorage> storage_;
    std::thread::id owner_{};
    std::uint64_t prepared_{},pending_{},last_{};
    bool poisoned_{};
    bool owner()const{return owner_==std::this_thread::get_id();}
    bool destroy(std::unique_ptr<ViewportStorage>&) noexcept;
public:
    explicit CullViewport(CullViewportApi api):api_(api){}
    ~CullViewport();
    CullViewport(const CullViewport&)=delete;
    CullViewport& operator=(const CullViewport&)=delete;
    bool prepare(const void* source,const StereoCullPlan&,bool stereoPerspective=true) noexcept;
    const void* lease(std::uint64_t serial) noexcept;
    bool retire(std::uint64_t serial) noexcept;
    bool reset() noexcept;
    bool pending()const{return pending_!=0;}
    bool ready()const{return storage_ && prepared_ && !poisoned_;}
    const void* viewport()const{return ready()?storage_.get():nullptr;}
};
} // namespace rdrvr::engine
