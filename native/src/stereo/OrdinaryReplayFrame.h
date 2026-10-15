#pragma once
#include <atomic>
#include <cstdint>

namespace rdr95 {
// Publication/cancellation for one ACTUAL selected native-left/private-right
// frame. This is not MAIN/object/resource admission. The Game adapter alone
// begins/ends it; replay code may only report a partial-emission fault.
class OrdinaryReplayFrame final {
    struct State {std::uint64_t serial{},scene{},pose{};bool faulted{};};
    inline static std::atomic<std::uint64_t> sequence_{};
    static State& state()noexcept {static thread_local State value;return value;}
public:
    class Stamp {
        friend class OrdinaryReplayFrame;
        std::uint64_t serial_{},scene_{},pose_{};
    public:
        Stamp()=default;
        bool current()const noexcept {
            const auto& s=state();
            return serial_&&s.serial==serial_&&s.scene==scene_&&s.pose==pose_&&!s.faulted;
        }
        std::uint64_t scene()const noexcept{return scene_;}
        std::uint64_t pose()const noexcept{return pose_;}
    };
    // One invocation per selected native LEFT depth. New globally unique
    // serial prevents same scene/pose and foreign-thread ABA.
    static bool begin(std::uint64_t scene,std::uint64_t pose)noexcept {
        auto& s=state();s={};if(!scene||!pose)return false;
        auto n=sequence_.load(std::memory_order_relaxed);
        while(n!=UINT64_MAX){
            if(sequence_.compare_exchange_weak(n,n+1,std::memory_order_relaxed)){
                s={n+1,scene,pose,false};return true;
            }
        }
        return false;
    }
    static Stamp current()noexcept {
        const auto& s=state();Stamp stamp;
        if(s.serial&&!s.faulted){stamp.serial_=s.serial;stamp.scene_=s.scene;stamp.pose_=s.pose;}
        return stamp;
    }
    static void fault()noexcept {state().faulted=true;}
    static bool good()noexcept {return !state().faulted;}
    static void end()noexcept {state().serial=0;state().scene=0;state().pose=0;}
};
}
