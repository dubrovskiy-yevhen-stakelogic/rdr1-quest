#pragma once
#include "EngineTargets.h"
#include <span>
namespace rdrvr::engine {
struct StockSceneTarget {const void* target{};EngineTargetRole role{};};
// Fixed, per-eye argument map. Original game objects stay unchanged. Attachment
// depth and shader depth deliberately resolve to different images of this eye.
// A caller must hold the simulation/render snapshot and keep shader/target hooks
// scoped to the audited main-world body; this class does not identify that body.
class EngineTextureMap {
    struct Entry {const void* stock{};EngineTargetRole role{};TargetImage original{};};
    struct Owned {const void* target{};EngineTargetRole role{};unsigned eye{};};
    EngineEyeTargets& targets_;PassResources& images_;PassSourceAbi memory_;VkDevice device_;
    bool (*ownsThread_)();const std::thread::id owner_{std::this_thread::get_id()};
    std::array<Entry,8> entries_{};std::size_t count_{};unsigned eye_{};
    // Identities only, valid under the same pending serial and generation.
    // Matching targets still go through destination's live metadata checks.
    std::array<Owned,14> owned_{};
    std::uint64_t serial_{},generation_{};
    bool valid() const;
    bool destination(EngineTargetRole,const void**,const TargetImage* original=nullptr) const;
public:
    EngineTextureMap(EngineEyeTargets& targets,PassResources& images,PassSourceAbi memory,VkDevice device,bool(*ownsThread)()):
        targets_(targets),images_(images),memory_(memory),device_(device),ownsThread_(ownsThread){}
    bool prepare(unsigned,std::span<const StockSceneTarget>) noexcept;
    bool texture(const void*,const void**) const noexcept;
    bool attachment(const void*,bool depth,const void**) const noexcept;
    static bool route(void* self,const void* source,const void** out){return static_cast<EngineTextureMap*>(self)->texture(source,out);}
};
}
