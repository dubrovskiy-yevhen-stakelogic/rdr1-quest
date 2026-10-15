#pragma once
#include "DynamicUniformDescriptors.h"
#include "CommandUniformSets.h"
#include <array>
#include <atomic>
#include <memory>

namespace rdr95 {
// Connects only real synchronous DUD capture callbacks to owned GPU snapshots.
// It does not skip an object or admit a render packet. Source identities let the
// packet recorder match actual emitted native bindings before replacing them.
class UniformCaptureSink final {
public:
    using DUD=rdrvr::engine::DynamicUniformDescriptors;
    using RecordingThread=bool(*)()noexcept;
    static constexpr unsigned MaxLayouts=128,MaxCommands=128;
    struct SourceBinding {
        VkDescriptorSet native{};VkDescriptorSetLayout layout{};
        std::uint64_t layoutGeneration{};
        unsigned count{};std::array<std::uint32_t,DUD::slots> offsets{};
    };
    class Ticket {
        friend class UniformCaptureSink;
        DUD::CaptureCommand command_{};VkPipelineLayout layout_{};unsigned mask_{};
        std::array<SourceBinding,DUD::sets> sources_{};
        std::array<CommandUniformSets::Ticket,DUD::sets> sets_{};
    public:
        Ticket()=default;
        DUD::CaptureCommand command()const noexcept{return command_;}
        VkPipelineLayout nativePipelineLayout()const noexcept{return layout_;}
        unsigned mask()const noexcept{return mask_;}
        // Valid after other binds, but only in the still-current arena recording.
        bool resolve(unsigned set,SourceBinding&,CommandUniformSets::Binding&)const noexcept;
    };
    UniformCaptureSink(CommandUniformArena&,CommandUniformSets&,RecordingThread)noexcept;
    ~UniformCaptureSink();
    UniformCaptureSink(const UniformCaptureSink&)=delete;UniformCaptureSink& operator=(const UniformCaptureSink&)=delete;
    DUD::CaptureObserver observer()noexcept;
    // Only currently observed native bindings. Any foreign/failed bind or
    // lifecycle invalidation clears eligibility; saved owned tickets remain
    // governed by their exact arena lifecycle, not subsequent native binds.
    bool current(VkCommandBuffer,unsigned requiredSetMask,Ticket&)const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
    static bool prepare(void*,const DUD::CaptureBatch&)noexcept;
    static void finish(void*,const DUD::CaptureBatch&,bool)noexcept;
    static void invalidate(void*,const DUD::CaptureCommand&)noexcept;
};
}
