#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <span>
#include <thread>

namespace rdrvr::engine {
// Converted layouts are a device-lifetime contract. All native graphics calls,
// including menus, must use the matching BindInputLayout/GetOrCreate/bind hooks.
// Unknown creation shapes retain their original layout and original behavior.
class DynamicUniformDescriptors {
public:
    static constexpr unsigned slots=16,sets=7;
    // PRIVATE capture: observes bytes only in the real, synchronous translated
    // bind. This is not a native pointer lease or a descriptor replay permission.
    struct CaptureCommand {
        VkDevice device{};VkCommandPool pool{};VkCommandBuffer command{};
        std::uint64_t owner{},allocation{},recording{};
        bool operator==(const CaptureCommand&)const=default;
    };
    struct CaptureRange {
        std::uint32_t set{},binding{},arrayElement{},dynamicIndex{};
        VkDescriptorSet descriptor{};VkDescriptorSetLayout descriptorLayout{};
        std::uint64_t layoutGeneration{};VkShaderStageFlags stages{};
        VkDescriptorBindingFlags flags{};
        VkBuffer buffer{};VkDeviceSize baseOffset{},effectiveOffset{},range{};
        bool present{};
    };
    struct CaptureSet {
        VkDescriptorSet descriptor{};VkDescriptorSetLayout layout{};
        std::uint64_t generation{};unsigned firstRange{},rangeCount{};
        bool converted{};
    };
    class CaptureBatch final {
        friend class DynamicUniformDescriptors;
        DynamicUniformDescriptors* owner_{};void* scope_{};void* state_{};
        std::thread::id thread_{};std::uint64_t epoch_{},serial_{},commandEpoch_{},drawActivity_{};
        std::uintptr_t frame_{},wrapper_{},mapped_{};std::uint64_t cursor_{};
        CaptureCommand command_{};VkPipelineLayout pipelineLayout_{};
        unsigned first_{},count_{},rangeCount_{};bool active_{};
        std::array<CaptureSet,sets> sets_{};
        std::array<CaptureRange,sets*slots> ranges_{};
        CaptureBatch()=default;
    public:
        CaptureBatch(const CaptureBatch&)=delete;
        CaptureBatch& operator=(const CaptureBatch&)=delete;
        CaptureCommand command()const noexcept{return command_;}
        VkPipelineLayout pipelineLayout()const noexcept{return pipelineLayout_;}
        unsigned firstSet()const noexcept{return first_;}
        std::span<const CaptureSet> boundSets()const noexcept{return {sets_.data(),count_};}
        std::span<const CaptureRange> ranges()const noexcept{return {ranges_.data(),rangeCount_};}
        // Exact-size copy into sink-owned memory. No source pointer escapes.
        // Valid only during prepare; copying the metadata creates no capability.
        bool copy(unsigned index,void* destination,std::size_t exactSize)const noexcept;
        bool current()const noexcept;
    };
    struct CaptureObserver {
        void* context{};
        bool(*prepare)(void*,const CaptureBatch&)noexcept{};
        // Called after the raw bind. Commit only publishes a sink's already-owned
        // bytes; false discards its pending batch. Must not reenter this owner.
        void(*finish)(void*,const CaptureBatch&,bool committed)noexcept{};
        void(*invalidate)(void*,const CaptureCommand&)noexcept{};
        // Optional lexical producer predicate. Null keeps the default observer
        // behavior. False bypasses CaptureBatch/ring reads and copying entirely.
        // May revoke/reenter, but cannot destroy this live owner or DrawScope.
        bool(*enabled)(void*)noexcept{};
        explicit operator bool()const noexcept{return prepare&&finish&&invalidate;}
    };
    struct Api {
        PFN_vkCreateDescriptorPool createPool{};PFN_vkDestroyDescriptorPool destroyPool{};
        PFN_vkResetDescriptorPool resetPool{};PFN_vkAllocateDescriptorSets allocate{};
        PFN_vkUpdateDescriptorSets update{};PFN_vkDestroyDescriptorSetLayout destroyLayout{};
        explicit operator bool()const noexcept{return createPool&&destroyPool&&resetPool&&allocate&&update&&destroyLayout;}
    };
    struct Limits {std::uint32_t dynamicUniform{},maxSets{},alignment{},maxRange{};};
    using NativeOriginal=std::uintptr_t(*)(void*,void*,const void*,const void*,void*,void*,int);
    class LayoutScope {
        friend class DynamicUniformDescriptors;
        DynamicUniformDescriptors* owner_{};LayoutScope* previous_{};unsigned converted_{};bool qualified_{};
    public:
        LayoutScope(DynamicUniformDescriptors&,bool qualified)noexcept;
        ~LayoutScope();LayoutScope(const LayoutScope&)=delete;
    };
    struct UniformBinding {
        VkCommandBuffer command{};VkDescriptorSet descriptor{};VkBuffer buffer{};
        std::uint64_t offset{};std::uint32_t range{};bool valid{};
    };
    class DrawScope {
        friend class DynamicUniformDescriptors;
        struct State;alignas(std::max_align_t) std::array<std::byte,2048> storage_;State* state_{};DrawScope* previous_{};
        DynamicUniformDescriptors* owner_{};bool good_{true},qualified_{};
        void* context_{};std::uint32_t dirty_{};
    public:
        // context is the pinned live VkContext_c argument. No borrowed argument
        // escapes this scope. A false good() MUST make BindInputLayout return false.
        DrawScope(DynamicUniformDescriptors&,void* context,bool qualified)noexcept;
        ~DrawScope();bool good()const noexcept{return good_;}
        // Actual GRAPHICS set0/binding1 issued during this exact live scope.
        // No previous-scope inference or extra driver call; absent/foreign binds
        // simply cannot certify an occluder. Query only at a native draw witness.
        bool matrixBinding(UniformBinding&)const noexcept;
        DrawScope(const DrawScope&)=delete;
    };
    DynamicUniformDescriptors();~DynamicUniformDescriptors();
    bool configure(VkDevice,Api,Limits)noexcept;
    // Set once before configure; callbacks and owner outlive every live command.
    bool observeCaptures(CaptureObserver)noexcept;
    bool ready()const noexcept;
    // Private opt-in. The caller must prove complete create/destroy coverage
    // BEFORE publishing any buffer lifetime. Unknown buffers keep frame reset.
    bool enablePersistentBuffers(VkDevice)noexcept;
    void disablePersistentBuffers(VkDevice)noexcept;
    void bufferCreated(VkDevice,VkBuffer,VkDeviceSize,VkBufferUsageFlags)noexcept;
    void bufferDestroyed(VkDevice,VkBuffer)noexcept;
    // Pinned native VkDescriptorSetLayoutData_c vectors: this is a creation-time
    // structural proof, never a cache keyed by mutable reflection pointers.
    bool nativeLayoutEligible(const void* vertexVector,const void* pixelVector)const noexcept;
    struct Stats {bool ready{};std::uint64_t calls{},hits{},faults{};};
    Stats snapshot()const noexcept;
    VkResult createLayout(PFN_vkCreateDescriptorSetLayout,VkDevice,const VkDescriptorSetLayoutCreateInfo*,const VkAllocationCallbacks*,VkDescriptorSetLayout*)noexcept;
    void destroyLayout(PFN_vkDestroyDescriptorSetLayout,VkDevice,VkDescriptorSetLayout,const VkAllocationCallbacks*)noexcept;
    std::uintptr_t nativeSet(NativeOriginal,void* context,void* nativePool,const void* layout,const void* constants,void* vs,void* ps,int type)noexcept;
    void bind(PFN_vkCmdBindDescriptorSets,VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,std::uint32_t first,std::uint32_t count,const VkDescriptorSet*,std::uint32_t dynamicCount,const std::uint32_t*)noexcept;
    // Foreign binds or ExecuteCommands invalidate native descriptor state.
    // Seed the next qualified native BindInputLayout even if its dirty bit is 0.
    void commandStateInvalidated(VkCommandBuffer)noexcept;
    // Call only after the corresponding Vulkan operation succeeded. Vulkan's
    // non-pending command-buffer lifetime rule proves old exclusive sets idle.
    void commandAllocated(VkDevice,VkCommandPool,VkCommandBuffer)noexcept;
    void commandBegun(VkCommandBuffer,VkResult)noexcept;
    void commandEnded(VkCommandBuffer,VkResult)noexcept;
    void commandReset(VkCommandBuffer,VkResult)noexcept;
    void commandFreed(VkDevice,VkCommandPool,VkCommandBuffer)noexcept;
    void poolReset(VkDevice,VkCommandPool,VkResult)noexcept;
    void poolDestroyed(VkDevice,VkCommandPool)noexcept;
    void shutdownAfterIdle(VkDevice)noexcept;
    // Use only immediately before destroying a device whose completion could
    // not be established. Forget handles without issuing Vulkan destruction.
    void abandonDevice(VkDevice)noexcept;
    std::string report()const;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
    // Every thread_local of this class (layout/draw scope, draw activity, last context, the
    // debug.rdrvr.dud.fast caches) in one trivially destructible block: one __tlsdesc lookup per entry point.
    struct Tls;static thread_local Tls tls_;
    bool captureCurrent(const CaptureBatch&)const noexcept;
};
extern DynamicUniformDescriptors dynamicUniformDescriptors;
}
