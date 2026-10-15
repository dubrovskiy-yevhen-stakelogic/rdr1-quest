#pragma once
#include "CommandUniformArena.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace rdr95 {
// Immutable descriptor views of CommandUniformArena-owned byte snapshots.
// All layout inputs must be the actual successful converted native definitions.
// This does not select a game object or establish source CPU pointer lifetime.
class CommandUniformSets final {
public:
    static constexpr unsigned MaxBindings=16,MaxLayouts=128,SetsPerPool=128,MaxSets=4096;
    static constexpr std::uint8_t Absent=255;
    // Terminal raw Vulkan PFNs must not reenter this owner. Object/device
    // teardown requires stopped users, including every synchronous callback.
    struct Api {
        PFN_vkCreateDescriptorSetLayout createLayout{};PFN_vkDestroyDescriptorSetLayout destroyLayout{};
        PFN_vkCreateDescriptorPool createPool{};PFN_vkDestroyDescriptorPool destroyPool{};
        PFN_vkAllocateDescriptorSets allocate{};PFN_vkUpdateDescriptorSets update{};
        explicit operator bool()const noexcept;
    };
    class Layout {
        friend class CommandUniformSets;
        const CommandUniformSets* owner_{};std::uint64_t generation_{};unsigned index_{};
    public:
        Layout()=default;
    };
    struct UniformDefinition {
        std::uint32_t binding{},count{};VkDescriptorType type{};
        VkShaderStageFlags stages{};VkDescriptorBindingFlags flags{};
        bool operator==(const UniformDefinition&)const=default;
    };
    struct Binding {
        VkDevice device{};VkCommandBuffer command{};VkDescriptorSet descriptor{};
        VkDescriptorSetLayout compatibleLayout{};unsigned count{};
        std::array<std::uint32_t,MaxBindings> offsets{};
        std::array<UniformDefinition,MaxBindings> definitions{};
        std::uint32_t presentBindings{}; // Native binding-number bits, derived from retained nonempty owned ranges.
    };
    class Ticket {
        friend class CommandUniformSets;
        const CommandUniformSets* owner_{};std::uint64_t generation_{};unsigned set_{};
        CommandUniformArena::Ticket bytes_{};
        std::array<std::uint8_t,MaxBindings> mapping_{};
    public:
        Ticket()=default;
        bool resolveForRecording(Binding&)const noexcept;
    };
    struct Stats {unsigned layouts{},sets{},pools{};std::uint64_t hits{},declines{};bool ready{};};
    CommandUniformSets()noexcept;~CommandUniformSets();
    CommandUniformSets(const CommandUniformSets&)=delete;CommandUniformSets& operator=(const CommandUniformSets&)=delete;
    bool initialize(VkDevice,Api)noexcept;
    // Copies/sorts exact definitions, creates one retained compatible clone.
    // Only dynamic UBO[1] bindings with optional PARTIALLY_BOUND are supported.
    bool createLayout(const VkDescriptorSetLayoutCreateInfo&,Layout&)noexcept;
    // Mapping follows ascending binding number; Absent is allowed only for an
    // originally partially-bound binding. Each referenced slice is owned and
    // immutable. Descriptor writes always use base0; offsets carry slice start.
    bool prepare(const Layout&,const CommandUniformArena::Ticket&,
                 std::span<const std::uint8_t> mapping,Ticket&)noexcept;
    Stats stats()const noexcept;
    // Caller has stopped all users. Destroy these descriptor resources BEFORE
    // the arena buffers at device teardown. End/Submit never alter descriptors.
    bool shutdownAfterIdle(VkDevice,VkResult)noexcept;
    void abandonDevice(VkDevice)noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
    bool resolve(const Ticket&,Binding&)const noexcept;
};
}
