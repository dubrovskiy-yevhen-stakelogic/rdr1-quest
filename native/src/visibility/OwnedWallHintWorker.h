#pragma once
#include "WallHintSelector.h"
#include "engine/OwnedStaticOccluders.h"
#include "WallHintExchange.h"
#include <memory>
namespace rdrvr::experimental {
// PRIVATE cold assembler + hint worker. No native reads/calls/GPU resources.
// One render producer. Destructor/stop require all Writer reservations closed.
class OwnedWallHintWorker {
public:
 using Selector=engine::WallHintSelector;using Mesh=engine::OwnedStaticOccluders;
 using Views=std::array<Selector::Matrix,2>;
 static constexpr unsigned MaxObjects=Selector::MaxObjects,JobSlots=3;
 struct Counters {std::uint64_t submitted{},declined{},coalesced{},processed{},built{},reused{},buildFailed{},published{},retiredEntries{},copiedMeshes{},copiedVertices{};};
 class Writer {
  friend class OwnedWallHintWorker;OwnedWallHintWorker*owner_{};unsigned slot_{};
  Writer(OwnedWallHintWorker*,unsigned)noexcept;
 public:
  Writer()=default;Writer(Writer&&)noexcept;Writer&operator=(Writer&&)noexcept;~Writer();
  Writer(const Writer&)=delete;Writer&operator=(const Writer&)=delete;
  explicit operator bool()const noexcept{return owner_!=nullptr;}
  // Reserve a Writer BEFORE acquiring/moving cache snapshots. On failure the
  // input is not consumed; accepted payloads are destroyed only on worker.
  // Repeated parts are legal; the worker collapses identical copied inputs.
  bool append(Selector::Key object,Mesh::Snapshot&&,const Selector::Matrix&world,Selector::Faces)noexcept;
  bool commit()noexcept;
 };
 explicit OwnedWallHintWorker(WallHintExchange&);
 ~OwnedWallHintWorker();OwnedWallHintWorker(const OwnedWallHintWorker&)=delete;
 // Level/world incarnation. Atomic revocation only, no payload destruction.
 void world(std::uint64_t)noexcept;
 Writer tryBegin(std::uint64_t world,std::uint64_t request,const Views&)noexcept;
 void stop()noexcept; // teardown only; never per frame
 Counters counters()const noexcept;
private:
 struct Impl;std::unique_ptr<Impl>impl_;
 void finish(unsigned,bool commit)noexcept;
};
}
