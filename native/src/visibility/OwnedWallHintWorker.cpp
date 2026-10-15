#include "OwnedWallHintWorker.h"
#include <atomic>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>
namespace rdrvr::experimental {
struct OwnedWallHintWorker::Impl {
 enum State:unsigned{Free,Writing,Ready,Working};
 struct Entry {Selector::Key object;Mesh::Snapshot mesh;Selector::Matrix world;Selector::Faces faces{};};
 struct Job {std::atomic<unsigned>state{Free};std::array<Entry,MaxObjects>entries;Views views{};std::uint64_t world{},request{};unsigned count{};bool commit{};};
 WallHintExchange&output;std::array<Job,JobSlots>jobs;std::thread::id producer{std::this_thread::get_id()};
 std::atomic<std::uint64_t>world{},signal{};std::atomic<bool>stopped{};
 std::uint64_t lastWorld{},lastRequest{};
 std::atomic<std::uint64_t>submitted{},declined{},coalesced{},processed{},built{},reused{},buildFailed{},published{},retiredEntries{},copiedMeshes{},copiedVertices{};
 std::thread thread;
 explicit Impl(WallHintExchange&o):output(o),thread([this]{run();}){}
 void wake(){signal.fetch_add(1,std::memory_order_release);signal.notify_one();}
 void retire(Job&j){for(unsigned k=0;k<j.count;++k)j.entries[k].mesh.reset();retiredEntries+=j.count;j.count=0;j.state.store(Free,std::memory_order_release);}
 struct Metadata{Selector::Key object,geometry;Selector::Matrix world;Selector::Faces faces;};
 struct KeyHash{std::size_t operator()(const Selector::Key&k)const noexcept{return std::size_t(k.identity^(k.generation*0x9e3779b97f4a7c15ull));}};
 struct KeyEq{bool operator()(const Selector::Key&a,const Selector::Key&b)const noexcept{return a==b;}};
 bool same(const Job&j,const std::vector<Metadata>&metadata){if(j.count!=metadata.size())return false;for(unsigned i=0;i<j.count;++i){auto&e=j.entries[i];auto&m=metadata[i];if(!(e.object==m.object)||!(Selector::Key{e.mesh.epoch,e.mesh.generation}==m.geometry)||e.faces!=m.faces||std::memcmp(e.world.data(),m.world.data(),sizeof(e.world)))return false;}return true;}
 bool assemble(const Job&job,Selector::Scene&scene,std::vector<Metadata>&metadata){
  struct Temporary{std::vector<Selector::XYZ>positions;std::vector<Selector::Patch>patches;std::vector<Selector::Node>nodes;};
  try{std::vector<Temporary>temp;temp.reserve(job.count);std::vector<Selector::MeshInput>meshes;meshes.reserve(job.count);std::vector<Selector::ObjectInput>objects;objects.reserve(job.count);std::vector<Metadata>next;next.reserve(job.count);std::unordered_map<Selector::Key,unsigned,KeyHash,KeyEq>seen;seen.reserve(job.count);
   for(unsigned i=0;i<job.count;++i){const auto&e=job.entries[i];const auto&s=e.mesh;Selector::Key key{s.epoch,s.generation};if(!s||!key.identity||!key.generation)return false;if(seen.emplace(key,unsigned(meshes.size())).second){if(scene.hasGeometry(key)){meshes.push_back({key});}else{temp.emplace_back();auto&t=temp.back();t.positions.reserve(s.positions.size());for(auto v:s.positions)t.positions.push_back({v.x,v.y,v.z});t.patches.reserve(s.patches.size());for(auto p:s.patches)t.patches.push_back({p.firstIndex,p.count});t.nodes.reserve(s.nodes.size());for(auto n:s.nodes)t.nodes.push_back({{n.minimum.x,n.minimum.y,n.minimum.z},{n.maximum.x,n.maximum.y,n.maximum.z},n.first,n.count});meshes.push_back({key,t.positions,t.patches,s.patchIndices,s.patchOrder,t.nodes});++copiedMeshes;copiedVertices+=s.positions.size();}}objects.push_back({e.object,key,e.world,e.faces});next.push_back({e.object,key,e.world,e.faces});}
   Selector::Scene fresh;if(!Selector::prepare(meshes,objects,fresh,&scene))return false;scene=std::move(fresh);metadata=std::move(next);return true;
  }catch(...){return false;}
 }
 void run()noexcept {
  Selector::Scene scene;std::vector<Metadata>metadata;std::uint64_t sceneWorld{};
  for(;;){auto observed=signal.load(std::memory_order_acquire);unsigned newest=JobSlots;std::uint64_t request=0;
   const auto activeWorld=world.load(std::memory_order_acquire);if(sceneWorld!=activeWorld){scene={};metadata.clear();sceneWorld=activeWorld;}
   for(unsigned i=0;i<JobSlots;++i)if(jobs[i].state.load(std::memory_order_acquire)==Ready){auto&j=jobs[i];if(!j.commit||j.world!=activeWorld||stopped.load(std::memory_order_acquire)){unsigned ready=Ready;if(j.state.compare_exchange_strong(ready,Working,std::memory_order_acq_rel))retire(j);continue;}if(newest==JobSlots||j.request>request){newest=i;request=j.request;}}
   if(stopped.load(std::memory_order_acquire))break;
   if(newest==JobSlots){signal.wait(observed,std::memory_order_acquire);continue;}
   for(unsigned i=0;i<JobSlots;++i)if(i!=newest&&jobs[i].state.load(std::memory_order_acquire)==Ready&&jobs[i].world==activeWorld&&jobs[i].request<=request){unsigned ready=Ready;if(jobs[i].state.compare_exchange_strong(ready,Working,std::memory_order_acq_rel)){++coalesced;retire(jobs[i]);}}
   auto&j=jobs[newest];unsigned ready=Ready;if(!j.state.compare_exchange_strong(ready,Working,std::memory_order_acq_rel))continue;
   ++processed;bool valid=false;if(scene&&sceneWorld==j.world&&same(j,metadata)){++reused;valid=true;}else if(j.count&&assemble(j,scene,metadata)){++built;valid=true;}else{++buildFailed;scene={};metadata.clear();}
   if(valid&&j.world==world.load(std::memory_order_acquire)&&!stopped.load(std::memory_order_acquire)){Selector::Stats stats;auto p=Selector::select(scene,j.views,stats);std::array<WallHintExchange::Hint,32>hints{};for(unsigned i=0;i<p.count;++i){auto&h=p.hints[i];hints[i]={h.objectIdentity,h.objectGeneration,h.geometryIdentity,h.geometryGeneration,h.patch};}if(output.publish(j.world,j.request,{hints.data(),p.count}))++published;}
   retire(j);
  }
  // Lifecycle contract: producer closed all Writers before stop. All accepted
  // shared payloads and the final scene/geometry copies die on this thread.
  for(auto&j:jobs)if(j.state.load(std::memory_order_acquire)!=Writing)retire(j);
 }
};
OwnedWallHintWorker::Writer::Writer(OwnedWallHintWorker*o,unsigned s)noexcept:owner_(o),slot_(s){}
OwnedWallHintWorker::Writer::Writer(Writer&&o)noexcept:owner_(o.owner_),slot_(o.slot_){o.owner_=nullptr;}
OwnedWallHintWorker::Writer&OwnedWallHintWorker::Writer::operator=(Writer&&o)noexcept{if(this!=&o){if(owner_)owner_->finish(slot_,false);owner_=o.owner_;slot_=o.slot_;o.owner_=nullptr;}return*this;}
OwnedWallHintWorker::Writer::~Writer(){if(owner_)owner_->finish(slot_,false);}
bool OwnedWallHintWorker::Writer::append(Selector::Key object,Mesh::Snapshot&&mesh,const Selector::Matrix&world,Selector::Faces faces)noexcept{if(!owner_||owner_->impl_->producer!=std::this_thread::get_id())return false;auto&j=owner_->impl_->jobs[slot_];if(j.count==MaxObjects||!object.identity||!object.generation||!mesh||!mesh.epoch||!mesh.generation||faces==Selector::Faces::Unknown||unsigned(faces)>3)return false;auto&e=j.entries[j.count++];e.object=object;e.world=world;e.faces=faces;e.mesh=std::move(mesh);return true;}
bool OwnedWallHintWorker::Writer::commit()noexcept{if(!owner_||owner_->impl_->producer!=std::this_thread::get_id())return false;auto*o=owner_;owner_=nullptr;o->finish(slot_,true);return true;}
OwnedWallHintWorker::OwnedWallHintWorker(WallHintExchange&o):impl_(std::make_unique<Impl>(o)){}
OwnedWallHintWorker::~OwnedWallHintWorker(){stop();}
void OwnedWallHintWorker::world(std::uint64_t world)noexcept{impl_->world.store(world,std::memory_order_release);impl_->output.scene(world);impl_->wake();}
OwnedWallHintWorker::Writer OwnedWallHintWorker::tryBegin(std::uint64_t world,std::uint64_t request,const Views&views)noexcept{auto&s=*impl_;if(s.producer!=std::this_thread::get_id()||!world||!request||world!=s.world.load(std::memory_order_acquire)||s.stopped.load(std::memory_order_acquire)||(s.lastWorld==world&&request<=s.lastRequest)){++s.declined;return{};}for(unsigned i=0;i<JobSlots;++i){unsigned free=Impl::Free;if(s.jobs[i].state.compare_exchange_strong(free,Impl::Writing,std::memory_order_acq_rel)){auto&j=s.jobs[i];j.count=0;j.views=views;j.world=world;j.request=request;j.commit=false;s.lastWorld=world;s.lastRequest=request;return Writer(this,i);}}++s.declined;return{};}
void OwnedWallHintWorker::finish(unsigned slot,bool commit)noexcept{auto&s=*impl_;auto&j=s.jobs[slot];j.commit=commit;if(commit)++s.submitted;j.state.store(Impl::Ready,std::memory_order_release);s.wake();}
void OwnedWallHintWorker::stop()noexcept{auto&s=*impl_;if(!s.stopped.exchange(true,std::memory_order_acq_rel)){s.wake();if(s.thread.joinable())s.thread.join();}}
OwnedWallHintWorker::Counters OwnedWallHintWorker::counters()const noexcept{const auto&s=*impl_;return {s.submitted.load(),s.declined.load(),s.coalesced.load(),s.processed.load(),s.built.load(),s.reused.load(),s.buildFailed.load(),s.published.load(),s.retiredEntries.load(),s.copiedMeshes.load(),s.copiedVertices.load()};}
}
