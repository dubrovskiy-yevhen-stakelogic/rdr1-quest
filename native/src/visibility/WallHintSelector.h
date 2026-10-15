#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
namespace rdrvr::engine {
// PRIVATE: asynchronous SOURCE HINTS, never visibility or native permission.
// Every hint must be matched to the current actually admitted native primitive.
class WallHintSelector {
public:
 struct Key {std::uint64_t identity{},generation{};bool operator==(const Key&)const=default;};
 struct XYZ {float x{},y{},z{};};
 struct Patch {std::uint32_t first{};std::uint8_t count{};};
 struct Node {XYZ minimum{},maximum{};std::uint32_t first{},count{};};
 using Matrix=std::array<double,16>;
 enum class Faces:std::uint8_t{Unknown,Both,PositiveArea,NegativeArea};
 struct MeshInput {Key geometry{};std::span<const XYZ> positions;std::span<const Patch> patches;std::span<const std::uint16_t> indices,order;std::span<const Node> nodes;};
 struct ObjectInput {Key object{},geometry{};Matrix world{};Faces faces{Faces::Unknown};};
 struct Hint {std::uint64_t objectIdentity{},objectGeneration{},geometryIdentity{},geometryGeneration{};std::uint32_t patch{};bool operator==(const Hint&)const=default;};
 static constexpr unsigned MaxHints=32,MaxObjects=4096,MaxMeshes=4096;
 struct Packet {std::array<Hint,MaxHints> hints{};unsigned count{};};
 struct Stats {std::uint64_t rays{},nodeTests{},patchBoxTests{},polygonTests{},hits{},budgetStops{};};
 struct Data;
 class Scene {friend class WallHintSelector;std::shared_ptr<const Data> data_;public:explicit operator bool()const noexcept{return bool(data_);}std::size_t bytes()const noexcept;bool hasGeometry(Key)const noexcept;};
 // Cold, copied CPU data only. This does not acquire/retain a native/GPU lease.
 // Geometry spans must be protected from mutation while being copied.
 // Optional reuse shares existing immutable geometry with EXACT matching keys.
 // For a reused key, MeshInput spans may be empty; never reuse a key for changed
 // vertex/index/BVH data. Object transforms/parts are always rebuilt fresh.
 // Repeated object/geometry keys may describe distinct transforms or faces.
 // Identical copied inputs collapse; hints never transfer a transform to a draw.
 static bool prepare(std::span<const MeshInput>,std::span<const ObjectInput>,Scene&,const Scene* reuse=nullptr)noexcept;
 // Nonblocking pure CPU worker. matrices are a camera sample used for choosing
 // hints; its pose/visibility/depth are NOT present in returned Packet.
 // grid=3 ->18 rays, grid=5 ->50 rays. Work exhaustion drops remaining hints.
 static Packet select(const Scene&,const std::array<Matrix,2>&,Stats&,unsigned grid=3)noexcept;
};
}
