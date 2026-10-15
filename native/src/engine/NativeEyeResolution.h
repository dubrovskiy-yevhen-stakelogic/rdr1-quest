#pragma once
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace rdrvr::engine {
// Reserve the complete native eye family at its initial creation. The stock
// window and gameplay/LOD dimensions remain unchanged. Later qualified DRS
// resizes use this capacity; this is not permission for arbitrary VMA growth.
class NativeEyeResolution {
public:
    struct Extent {
        std::uint32_t width{},height{};
        explicit operator bool() const noexcept {return width&&height;}
        bool operator==(const Extent&) const = default;
    };
    enum class Family : unsigned {Renderer,Postfx,Grass,Trees,Reflection,None};
    using MaxScreen=void(*)(int*,int*);
    using Resize=void(*)(void*,int,int);
    using Reader=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    struct Api {MaxScreen maxScreen{};Resize resize{},rebuild{};Reader read{};
        std::uintptr_t vegetationSettingsSlot{},stockWidthSlot{},stockHeightSlot{};};
private:
    struct InitialState {
        NativeEyeResolution* owner{};Family family{Family::None};void* identity{};
        InitialState* previous{};Extent reserved{};std::uint64_t depthGeneration{};bool admitted{},sawMax{},fault{};
    };
    struct UpdateState {NativeEyeResolution* owner{};void* renderer{};bool coordinateReady{},world{},resized{};};
    inline static thread_local InitialState* initial_{};
    inline static thread_local UpdateState* updating_{};
    Api api_{};
    mutable std::mutex mutex_;
    Extent request_{},active_{},normalized_{};
    void* renderer_{};void* depth_{};
    std::uint64_t depthGeneration_{},completedGeneration_{},activeGeneration_{},normalizedGeneration_{};
    std::array<unsigned,5> inFlight_{};
    std::array<Extent,5> capacity_{};
    unsigned completedMask_{};
    bool configured_{},installed_{},started_{},depthGood_{},depthAbsent_{},fault_{};
    std::atomic<std::uint64_t> requests_{},lateRequests_{},scopeCalls_{},maxCalls_{},expandedCalls_{},scopeFaults_{},updates_{},resizes_{},rebuilds_{},activations_{},declined_{},verificationFailures_{};
    std::atomic<std::uint64_t> stockResizes_{},stockNormalizations_{},stockFailures_{};
    std::atomic<bool> desiredWorld_{};
    static bool valid(Extent) noexcept;
    unsigned requiredMask() const noexcept;
    bool capacityReadyLocked(void*,unsigned required) const noexcept;
    bool rendererScale(void*,Extent* actual=nullptr) const noexcept;
    bool canResize(void*,bool coordinates) const noexcept;
    bool fitsCapacityLocked(Extent,unsigned required) const noexcept;
    bool stockExtent(Extent&) const noexcept;
    void finishUpdate(void*,bool,bool);
public:
    class InitialScope {
        InitialState state_{};InitialState* previous_{};bool finished_{};
    public:
        InitialScope(NativeEyeResolution&,Family,void* identity=nullptr);
        ~InitialScope();
        InitialScope(const InitialScope&)=delete;
        InitialScope& operator=(const InitialScope&)=delete;
        // Call only after the original whole owner function returns normally.
        bool finish() noexcept;
    };
    bool configure(Api) noexcept;
    void installed(bool allHooksInstalled) noexcept;
    bool request(std::uint32_t width,std::uint32_t height) noexcept;
    // Empty until the complete hook transaction is installed. Safe for the
    // initial display-depth hook; never reads independently published atomics.
    Extent requestedExtent() const noexcept;
    void* trackedDepth() const noexcept;
    void depthCreated(void* surface,std::uint32_t width,std::uint32_t height,bool verified) noexcept;
    // Only the pinned post-CreateBackbuffers proof may assert this: the native
    // display is configured without depth and all its produced framebuffers
    // retain no depth. Other world depths still require initial family capacity.
    void displayDepthNotRequired(bool verified) noexcept;
    void maxScreen(int*,int*,Family exactSite);
    void resize(void*,int,int,bool exactCaller);
    void rebuild(void*,int,int,bool exactCaller);
    template<class Native> void update(void* renderer,bool exactCaller,bool coordinateReady,bool world,Native&& native){
        if(!exactCaller||updating_){std::forward<Native>(native)();return;}
        const int incomingErrno=errno;
        UpdateState state{this,renderer,coordinateReady,world,false};
        struct Restore {UpdateState* previous;~Restore(){updating_=previous;}} restore{updating_};
        updating_=&state;++updates_;
        desiredWorld_.store(world,std::memory_order_relaxed);
        if(!world||!coordinateReady){std::lock_guard lock(mutex_);active_={};}
        errno=incomingErrno;
        std::forward<Native>(native)(); // Native DRS and its bookkeeping once.
        finishUpdate(renderer,coordinateReady,world);
    }
    Extent activeExtent(void* renderer) const noexcept;
    std::string report() const;
};
extern NativeEyeResolution nativeEyeResolution;
}
