#pragma once
#include "WallHintExchange.h"
#include "engine/ConservativeWallCones.h"
#include "engine/NativeStaticTriangles.h"
#include "engine/StaticOcclusionProjection.h"
#include <algorithm>
#include <cmath>
#include <span>

namespace rdrvr::experimental {
// PRIVATE replacement for the late PriorityOcclusionSources enqueue/build.
// It consumes the EXISTING actual native depth witness packet and copies only
// explicitly selected original cold patches. No BVH/triangle scan, new hull,
// physics ray, previous depth or material guess is allowed in this path.
class CurrentWallPatchBuilder {
public:
    using Cones=engine::ConservativeWallCones;
    using Triangles=engine::NativeStaticTriangles;
    using Cache=engine::OwnedStaticOccluders;
    using Projection=engine::StaticOcclusionProjection;
    struct Stats {unsigned witnessPackets{},unselected{},cacheDeclined{},identityDeclined{},patches{},vertices{},errorDeclined{};};
    static bool wants(std::span<const WallHintExchange::Hint> hints,
            std::uint64_t object,std::uint64_t objectGeneration)noexcept {
        if(hints.size()>WallHintExchange::Capacity)return false;
        for(const auto& h:hints)if(h.valid()&&h.object==object&&h.objectGeneration==objectGeneration)return true;
        return false;
    }
    // The current owner supplies object/material/LOD/transform/record evidence
    // from its native witness, NOT from the worker hint. Geometry key convention:
    // identity = OwnedStaticOccluders epoch, generation = immutable mesh generation.
    // Both are copied from the successfully resolved current witness packet.
    static unsigned copy(std::uint64_t nativeFrame,Triangles::Packet& packet,const Cache& cache,
            const Projection& projection,std::span<const WallHintExchange::Hint> hints,
            Cones::SourceKey live,std::uint64_t objectGeneration,
            std::span<Cones::Source> output,Stats& stats)noexcept {
        ++stats.witnessPackets;
        if(!nativeFrame||!live.object||!live.material||!live.materialGeneration||!live.transform||
           !live.transformGeneration||!live.record||!objectGeneration||output.empty()||
           !wants(hints,live.object,objectGeneration)){++stats.unselected;return 0;}
        if(!Triangles::resolvePacket(nativeFrame,packet,cache)){
            ++stats.cacheDeclined;return 0;
        }
        if(packet.faces==Cones::Raster::Faces::Unknown||
           packet.vertexHandle!=packet.mesh.identity.vertices.gpuBuffer||
           packet.indexHandle!=packet.mesh.identity.indexGpuBuffer){++stats.identityDeclined;return 0;}
        const auto& mesh=packet.mesh;live.geometry=mesh.epoch;live.geometryGeneration=mesh.generation;
        const auto& m=packet.world;
        for(double x:m)if(!std::isfinite(x)){++stats.identityDeclined;return 0;}
        if(m[12]!=0||m[13]!=0||m[14]!=0||m[15]!=1){++stats.identityDeclined;return 0;}
        unsigned count=0;
        for(const auto& hint:hints){
            if(hint.object!=live.object||hint.objectGeneration!=objectGeneration||
               hint.geometry!=mesh.epoch||hint.geometryGeneration!=mesh.generation)continue;
            if(count>=output.size()||count==Cones::MaxWalls)break;
            if(hint.patch>=mesh.patches.size()){++stats.identityDeclined;continue;}
            bool duplicate=false;for(unsigned i=0;i<count;++i)duplicate|=output[i].key.patch==hint.patch;
            if(duplicate)continue;
            const auto& patch=mesh.patches[hint.patch];
            if(patch.count<3||patch.count>Cones::Raster::MaxPatchVertices||
               patch.firstIndex>mesh.patchIndices.size()||patch.count>mesh.patchIndices.size()-patch.firstIndex){++stats.identityDeclined;continue;}
            Cones::Source next;next.key=live;next.key.patch=hint.patch;
            next.polygon.count=patch.count;next.polygon.faces=packet.faces;
            bool valid=true;
            for(unsigned i=0;i<patch.count;++i){
                const auto index=mesh.patchIndices[patch.firstIndex+i];
                if(index>=mesh.positions.size()){valid=false;break;}
                const auto& v=mesh.positions[index];auto& w=next.polygon.vertex[i];
                w={m[0]*v.x+m[1]*v.y+m[2]*v.z+m[3],
                   m[4]*v.x+m[5]*v.y+m[6]*v.z+m[7],
                   m[8]*v.x+m[9]*v.y+m[10]*v.z+m[11]};
                ++stats.vertices;
            }
            if(!valid){++stats.identityDeclined;continue;}
            if(!projection.errorsSource({next.polygon.vertex.data(),patch.count},packet.worldError,next.errors)){
                ++stats.errorDeclined;continue;
            }
            output[count++]=next;++stats.patches;
        }
        if(!Triangles::packetCurrent(nativeFrame,packet,cache)){++stats.cacheDeclined;return 0;}
        return count;
    }
};
}
