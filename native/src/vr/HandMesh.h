#pragma once
// File: "UXRH", u32 version 1, u32 vertexCount, u32 indexCount; vertexCount x 104-byte vertices
// {float position[4][3], normal[4][3], uv[2]} (poses open, grip, trigger, both); indexCount u16
// indices. The vertex bytes are uploaded unchanged as the vertex buffer (stride 104); the GPU
// blends the four poses (shaders/hands.vert). parse() never throws and never keeps a pointer past
// the span's lifetime (the span is .rodata).
#include "runtime/Math.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace rdrvr::vr::hands {
struct HandVertex {float position[4][3],normal[4][3],uv[2];};
static_assert(sizeof(HandVertex)==104);
inline constexpr std::uint32_t vertexStride=sizeof(HandVertex);
struct HandMesh {
    std::span<const unsigned char> vertexBytes{},indexBytes{};
    std::uint32_t vertexCount{},indexCount{};
    runtime::Vec3 lo{},hi{}; // model-space bounds over all four poses
    bool valid() const noexcept {return vertexCount&&indexCount;}
    // Same acceptance rules as the sandbox loader: <= 5000 vertices, <= 27000 indices (multiple of 3), |position| <= 2 m,
    // finite normals/uv, every index < vertexCount, exact file size.
    bool parse(std::span<const unsigned char> bytes,const char** why=nullptr) noexcept {
        const auto fail=[&](const char* reason){if(why)*why=reason;*this={};return false;};
        std::uint32_t header[4]{};
        if(bytes.size()<16)return fail("UXRH_SHORT");
        std::memcpy(header,bytes.data(),16);
        if(std::memcmp(bytes.data(),"UXRH",4)||header[1]!=1)return fail("UXRH_MAGIC");
        const std::uint32_t nv=header[2],ni=header[3];
        if(!nv||nv>5000||!ni||ni>27000||ni%3||bytes.size()!=16+std::size_t(nv)*vertexStride+std::size_t(ni)*2)return fail("UXRH_SIZE");
        runtime::Vec3 mn{1e9f,1e9f,1e9f},mx{-1e9f,-1e9f,-1e9f};
        for(std::uint32_t i=0;i<nv;++i){
            HandVertex v{};std::memcpy(&v,bytes.data()+16+std::size_t(i)*vertexStride,vertexStride);
            for(const auto& p:v.position)for(unsigned c=0;c<3;++c){
                if(!std::isfinite(p[c])||std::abs(p[c])>2)return fail("UXRH_POSITION");
                mn[c]=std::min(mn[c],p[c]);mx[c]=std::max(mx[c],p[c]);}
            for(const auto& n:v.normal)for(float x:n)if(!std::isfinite(x))return fail("UXRH_NORMAL");
            for(float x:v.uv)if(!std::isfinite(x))return fail("UXRH_UV");
        }
        const auto* indices=bytes.data()+16+std::size_t(nv)*vertexStride;
        for(std::uint32_t i=0;i<ni;++i){std::uint16_t index{};std::memcpy(&index,indices+2*std::size_t(i),2);if(index>=nv)return fail("UXRH_INDEX");}
        vertexBytes=bytes.subspan(16,std::size_t(nv)*vertexStride);indexBytes=bytes.subspan(16+std::size_t(nv)*vertexStride,std::size_t(ni)*2);
        vertexCount=nv;indexCount=ni;lo=mn;hi=mx;
        if(why)*why="OK";
        return true;
    }
    HandVertex vertex(std::uint32_t i) const noexcept {HandVertex v{};if(i<vertexCount)std::memcpy(&v,vertexBytes.data()+std::size_t(i)*vertexStride,vertexStride);return v;}
    std::uint16_t index(std::uint32_t i) const noexcept {std::uint16_t v{};if(i<indexCount)std::memcpy(&v,indexBytes.data()+2*std::size_t(i),2);return v;}
};
} // namespace rdrvr::vr::hands
