#include "PlannerReplaySnapshot.h"
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace rdrvr::engine {
static_assert(std::endian::native==std::endian::little);
namespace {std::string key(std::string_view a,std::string_view b){return std::string(a)+"/"+std::string(b);}}
void PlannerReplaySnapshot::field(std::string_view name,unsigned kind,const void* data,std::size_t count,std::size_t size){
    if(name.empty()||name.size()>65535||kind>4||count>UINT32_MAX||!size||count>MaxBytes/size)throw false;
    const auto n=count*size,total=std::size_t(8)+name.size()+n;
    if(total>MaxBytes-bytes_.size()||(n&&!data))throw false;
    const auto start=bytes_.size();bytes_.resize(start+total);auto*p=bytes_.data()+start;
    const auto nameSize=std::uint16_t(name.size());const auto elements=std::uint32_t(count);
    std::memcpy(p,&nameSize,2);p[2]=std::byte(kind);p[3]=std::byte{};std::memcpy(p+4,&elements,4);
    std::memcpy(p+8,name.data(),name.size());if(n)std::memcpy(p+8+name.size(),data,n);
}
void PlannerReplaySnapshot::u64(std::string_view k,std::uint64_t v){field(k,2,&v,1,8);}
void PlannerReplaySnapshot::u32(std::string_view k,std::span<const std::uint32_t>v){field(k,1,v.data(),v.size(),4);}
void PlannerReplaySnapshot::f32(std::string_view k,std::span<const float>v){field(k,3,v.data(),v.size(),4);}
void PlannerReplaySnapshot::f64(std::string_view k,std::span<const double>v){field(k,4,v.data(),v.size(),8);}
void PlannerReplaySnapshot::entry(std::string_view k,const PriorityOcclusionSources::Entry&e){
    const double d[]{e.area,e.nearW,e.eye[0].nearDepth,e.eye[1].nearDepth};f64(key(k,"depthArea"),d);
    const std::uint32_t meta[]{e.source,e.item,e.patch,e.potential,e.backfaceRejected,e.clipRejected};u32(key(k,"meta"),meta);
    for(unsigned eye=0;eye<2;++eye){const auto&q=e.eye[eye];const std::uint32_t values[]{q.x0,q.x1,q.y0,q.y1,q.cx0,q.cx1,q.cy0,q.cy1,q.known,q.empty};u32(key(k,"eye"+std::to_string(eye)),values);}
}
void PlannerReplaySnapshot::grid(std::string_view k,const PriorityOcclusionSources&p){
    std::array<float,8192> cells{};for(unsigned e=0;e<2;++e)for(unsigned y=0;y<64;++y)for(unsigned x=0;x<64;++x)cells[e*4096+y*64+x]=p.raster_->sourceCellDepth(p.frame_,e,x,y);
    f32(k,cells);
}
void PlannerReplaySnapshot::rasterState(std::string_view k,const ConservativeStereoRaster&r){
    const auto&b=r.budget_;const std::uint32_t budget[]{b.triangles,b.rasterCells,b.candidates,b.queryCells};u32(key(k,"budget"),budget);
    for(unsigned eye=0;eye<2;++eye){auto prefix=key(k,std::to_string(eye));const auto&e=r.errors_[eye];const double error[]{e.coverageCells,e.depth};f64(key(prefix,"errors"),error);
        f64(key(prefix,"matrix"),r.views_[eye]);f32(key(prefix,"tiles"),r.sourceTiles_[eye]);u64(key(prefix,"dirtyTiles"),r.sourceTileDirty_[eye]);
        field(key(prefix,"tileMaxCount"),0,r.sourceTileMaxCount_[eye].data(),r.sourceTileMaxCount_[eye].size(),1);}
    const auto&s=r.stats_;
#define RASTER(n) u64(key(k,#n),s.n)
    RASTER(triangles);RASTER(invalidTriangles);RASTER(culledFaces);RASTER(clippedTriangles);RASTER(rasterCells);RASTER(coveredCells);RASTER(depthUpdates);
    u64(key(k,"uniqueCoveredCells0"),s.uniqueCoveredCells[0]);u64(key(k,"uniqueCoveredCells1"),s.uniqueCoveredCells[1]);RASTER(sourceTileRebuilds);
    RASTER(candidates);RASTER(rejected);RASTER(oneEyeOnly);RASTER(invalidCandidates);RASTER(queryCells);RASTER(rasterBudgetStops);RASTER(queryBudgetStops);
    RASTER(staleCalls);RASTER(preparations);RASTER(preparedEyes);RASTER(earlyClipped);RASTER(earlyCulled);RASTER(earlyEmpty);RASTER(preparationCells);RASTER(preparationBudgetBypass);
#undef RASTER
}
void PlannerReplaySnapshot::stats(std::string_view k,const PriorityOcclusionSources::Stats&s){
#define VALUE(n) u64(key(k,#n),s.n)
    VALUE(offered);VALUE(sources);VALUE(resolvedPackets);VALUE(meshQueries);VALUE(meshHits);VALUE(meshMisses);VALUE(availablePatches);VALUE(invalid);
    VALUE(sourceOverflow);VALUE(queueDropped);VALUE(queueHighWater);VALUE(earlyTrials);VALUE(earlyTrialPatches);VALUE(earlyFullCoverage);
    VALUE(objectBoundQueries);VALUE(earlyObjectDrops);VALUE(envelopeQueries);VALUE(envelopeHits);VALUE(targets);VALUE(retiredTargets);VALUE(invalidTargets);
    VALUE(targetOverflow);VALUE(targetQueries);VALUE(targetWork);VALUE(targetBudgetStops);VALUE(targetRejectedBranches);VALUE(unreachableTargets);
    VALUE(reachabilityCells);VALUE(reachabilityBudgetStops);VALUE(reachabilityPreflightStops);VALUE(coveredNodes);VALUE(coveredPatches);VALUE(tileQueries);
    VALUE(smallBoundsDropped);VALUE(backfaceBoundsDropped);VALUE(clipDropped);VALUE(clipNodeDropped);VALUE(nodeExpansions);VALUE(leafExpansions);
    VALUE(patchCandidates);VALUE(selectedPatches);VALUE(transformedVertices);VALUE(prepared);VALUE(errorRejected);VALUE(rasterAccepted);VALUE(sourceTriangles);
    VALUE(expansionStops);VALUE(patchStops);VALUE(stale);VALUE(mutations);
#undef VALUE
}
void PlannerReplaySnapshot::mesh(std::string_view k,const OwnedStaticOccluders::Snapshot&m){
    u64(key(k,"epoch"),m.epoch);u64(key(k,"generation"),m.generation);
    std::vector<float> pos;pos.reserve(m.positions.size()*3);for(auto&v:m.positions){pos.push_back(v.x);pos.push_back(v.y);pos.push_back(v.z);}f32(key(k,"positions"),pos);
    std::vector<std::uint32_t> ints;ints.reserve(m.patches.size()*3);for(auto&p:m.patches){ints.push_back(p.firstIndex);ints.push_back(p.sourceTriangleCount);ints.push_back(p.count);}u32(key(k,"patches"),ints);
    ints.assign(m.patchIndices.begin(),m.patchIndices.end());u32(key(k,"indices"),ints);ints.assign(m.patchOrder.begin(),m.patchOrder.end());u32(key(k,"order"),ints);
    pos.clear();ints.clear();pos.reserve(m.nodes.size()*15);ints.reserve(m.nodes.size()*2);
    for(auto&n:m.nodes){for(auto*v:{&n.minimum,&n.maximum,&n.maxPatchExtent,&n.patchMinMaximum,&n.patchMaxMinimum}){pos.push_back(v->x);pos.push_back(v->y);pos.push_back(v->z);}ints.push_back(n.first);ints.push_back(n.count);}
    f32(key(k,"nodeVectors"),pos);u32(key(k,"nodeLinks"),ints);
}
void PlannerReplaySnapshot::fail()noexcept{std::vector<std::byte>().swap(bytes_);cache_=nullptr;state_.store(State::Failed,std::memory_order_release);}
bool PlannerReplaySnapshot::captureBefore(PriorityOcclusionSources&p,const std::array<NativeStaticOcclusionSource::Camera,2>& cameras,
                                         unsigned w,unsigned h,std::uint64_t epoch,std::string_view buildId)noexcept{
    State idle=State::Idle;if(!state_.compare_exchange_strong(idle,State::Capturing,std::memory_order_acq_rel))return false;
    try{
        if(!epoch||buildId.size()!=64||!p.frame_||p.built_||!p.cache_||!p.raster_||!p.projection_||p.cache_->invalidationSerial()!=p.serial_||p.budget_.queue<p.budget_.sources)throw false;
        cache_=p.cache_;serial_=p.serial_;frame_.store(p.frame_,std::memory_order_release);bytes_.reserve(MaxBytes);
        constexpr char magic[]{'R','D','R','O','C','C','0','1'};bytes_.resize(8);std::memcpy(bytes_.data(),magic,8);
        const auto cacheEpoch=cache_->statistics().epoch;
        u64("schema",1);field("buildId",0,buildId.data(),buildId.size(),1);u64("frame",p.frame_);u64("profilerEpoch",epoch);u64("cacheSerial",serial_);u64("cacheEpoch",cacheEpoch);
        const std::uint32_t extent[]{w,h};u32("eyeExtent",extent);
        const auto&b=p.budget_;const std::uint32_t budget[]{b.sources,b.queue,b.expansions,b.patches,b.targetQueries,b.reachabilityCells,b.earlyCoverage};u32("budget",budget);
        const std::uint32_t state[]{p.sourceCount_,p.targetCount_,p.activeTargets_,p.earlyTrialUsed_,p.earlySource_,p.earlyPatchMask_};u32("state",state);f64("maxTargetDepth",p.maxTargetDepth_);
        for(unsigned eye=0;eye<2;++eye){auto prefix="camera/"+std::to_string(eye);f32(key(prefix,"relativeClip"),cameras[eye].relativeClip);f32(key(prefix,"inverseView"),cameras[eye].inverseView);f32(key(prefix,"fullClip"),cameras[eye].fullClip);u64(key(prefix,"fullClipReady"),cameras[eye].fullClipReady);}
        stats("initialStats",p.stats_);grid("initialGrid",p);rasterState("initialRaster",*p.raster_);
        for(unsigned eye=0;eye<2;++eye)f64("projection/"+std::to_string(eye),p.projection_->matrices()[eye]);
        struct MeshKey {OwnedStaticOccluders::Identity id;std::uint64_t generation;};std::vector<MeshKey> meshes;meshes.reserve(p.sourceCount_);
        for(unsigned i=0;i<p.sourceCount_;++i){const auto&packet=p.sources_[i];auto prefix="source/"+std::to_string(i);entry(key(prefix,"root"),p.roots_[i]);
            u64(key(prefix,"clipReady"),p.clipReady_[i]);
            if(p.clipReady_[i]){f64(key(prefix,"localClip0"),p.localClip_[i][0]);f64(key(prefix,"localClip1"),p.localClip_[i][1]);}
            f64(key(prefix,"world"),packet.world);const double error[]{packet.worldError.x,packet.worldError.y,packet.worldError.z};f64(key(prefix,"worldError"),error);
            const std::uint32_t flags[]{bool(packet),packet.pending(),unsigned(packet.faces)};u32(key(prefix,"flags"),flags);
            u64(key(prefix,"vertexHandle"),packet.vertexHandle);u64(key(prefix,"indexHandle"),packet.indexHandle);
            u64(key(prefix,"packetEpoch"),packet.pending()?packet.epoch_:packet.mesh.epoch);
            u64(key(prefix,"packetSerial"),packet.serial_);
            const auto&id=packet.identity();const auto&v=id.vertices;
            const std::uint64_t identity[]{v.vertexBuffer,v.nativeBuffer,v.allocation,v.fvf,v.gpuBuffer,v.byteSize,v.vertexCount,v.stride,id.indexBuffer,id.nativeIndexBuffer,id.indexAllocation,id.indexGpuBuffer,id.indexByteSize,id.indexCount};field(key(prefix,"identity"),2,identity,std::size(identity),8);field(key(prefix,"fvfBytes"),0,v.fvfBytes.data(),v.fvfBytes.size(),1);
            OwnedStaticOccluders::Snapshot queried;const auto*m=&packet.mesh;
            if(packet.pending()){if(cache_->querySnapshot(cacheEpoch,id,queried))m=&queried;}
            if(!*m||!cache_->current(*m)){u64(key(prefix,"mesh"),UINT64_MAX);continue;}
            unsigned at=0;while(at<meshes.size()&&!(meshes[at].id==m->identity&&meshes[at].generation==m->generation))++at;
            u64(key(prefix,"mesh"),at);if(at==meshes.size()){mesh("mesh/"+std::to_string(at),*m);meshes.push_back({m->identity,m->generation});}
        }
        u64("meshCount",meshes.size());
        for(unsigned i=0;i<p.targetCount_;++i){PriorityOcclusionSources::Entry e{};e.eye=p.targets_[i].eye;entry("target/"+std::to_string(i),e);u64("target/"+std::to_string(i)+"/active",p.targets_[i].active);}
        if(cache_->invalidationSerial()!=serial_)throw false;
        return true;
    }catch(...){fail();return false;}
}
bool PlannerReplaySnapshot::metadata(std::span<const Counter> counters)noexcept{
    if(state()!=State::Capturing||counters.size()>128)return false;
    try{u64("diagnostic/performanceExcluded",1);
        for(const auto& c:counters){if(c.name.empty()||c.name.size()>96)throw false;u64("diagnostic/"+std::string(c.name),c.value);}
        return true;
    }catch(...){fail();return false;}
}
void PlannerReplaySnapshot::captureAfter(const PriorityOcclusionSources&p)noexcept{
    if(state()!=State::Capturing)return;
    try{if(p.frame_!=frame()||!p.built_||p.cache_!=cache_||cache_->invalidationSerial()!=serial_)throw false;
        stats("finalStats",p.stats_);grid("finalGrid",p);rasterState("finalRaster",*p.raster_);u64("complete",1);cache_=nullptr;state_.store(State::Ready,std::memory_order_release);
    }catch(...){fail();}
}
std::vector<std::byte> PlannerReplaySnapshot::take()noexcept{
    State ready=State::Ready;if(!state_.compare_exchange_strong(ready,State::Taken,std::memory_order_acq_rel))return {};
    return std::move(bytes_);
}
}
