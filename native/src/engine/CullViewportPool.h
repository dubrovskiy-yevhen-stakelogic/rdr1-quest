#pragma once
#include "CullViewport.h"
#include "VisibilityCameraBridge.h"
#include <array>
#include <mutex>

namespace rdrvr::engine {
// PrepareMainVisibilityList stores the viewport in an asynchronous request.
// The render thread must report completion AFTER DrawEnd, never at Scene exit.
// Completion only marks a slot: engine destruction remains on its creator.
class CullViewportPool {
    struct Slot {
        std::unique_ptr<CullViewport> camera;
        VisibilityTicket ticket{};
        const void* viewport{};
        unsigned readers{};
        bool complete{};
        const void* shaft{};unsigned visibility{}; // 0 unseen, 1 complete, 2 rejected
    };
    struct State {
        explicit State(CullViewportApi value):api(value){}
        CullViewportApi api;
        std::mutex mutex;
        std::array<Slot,4> slots{};
        std::thread::id owner{};
        std::uint64_t last{};
    };
    std::shared_ptr<State> state_;
    static unsigned collect(State&) noexcept;
public:
    class Borrow {
        friend class CullViewportPool;
        std::shared_ptr<State> state_;
        unsigned slot_{};
        VisibilityTicket ticket_{};
        const void* viewport_{};
        Borrow(std::shared_ptr<State>,unsigned,const Slot&);
    public:
        Borrow()=default;
        ~Borrow();
        Borrow(const Borrow&)=delete;
        Borrow& operator=(const Borrow&)=delete;
        Borrow(Borrow&&) noexcept;
        Borrow& operator=(Borrow&&)=delete;
        explicit operator bool()const{return viewport_!=nullptr;}
        const void* viewport()const{return viewport_;}
        const VisibilityTicket& ticket()const{return ticket_;}
    };
    explicit CullViewportPool(CullViewportApi api):state_(std::make_shared<State>(api)){}
    CullViewportPool(const CullViewportPool&)=delete;
    CullViewportPool& operator=(const CullViewportPool&)=delete;
    // Returns immutable engine storage. A failed later engine call does NOT
    // release it: an asynchronous consumer might already have received it.
    const void* lease(const void* source,const VisibilityTicket&) noexcept;
    Borrow borrow(const void* viewport) noexcept;
    std::uint64_t serial(std::uintptr_t renderer,std::uint32_t marker)const noexcept;
    void visibility(const void* viewport,const void* shaft,bool success) noexcept;
    const void* shaft(std::uintptr_t renderer,std::uint64_t serial)const noexcept;
    const void* viewport(std::uintptr_t renderer,std::uint64_t serial)const noexcept;
    bool stereoCull(std::uintptr_t renderer,std::uint64_t serial)const noexcept;
    bool complete(std::uintptr_t renderer,std::uint64_t serial) noexcept;
    unsigned collect() noexcept;
    unsigned pending()const noexcept;
};
} // namespace rdrvr::engine
