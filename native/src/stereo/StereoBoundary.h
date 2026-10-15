#pragma once
// Render-pass boundary test (STEREO TEST 10/11/12) - pure helpers, no registries, no driver calls.
//
// Twins of an engine render pass P (TraceEntry rateCreatePass creates them through the fragment-rate bridge, so they
// carry the identical VRS attachment). Only attachment load/store ops and initial/final layouts differ from P; the
// subpass (references and their layouts) and the dependencies are copied unchanged. Vulkan render-pass compatibility
// ("Render Pass Compatibility") ignores exactly those fields, so
//  - every framebuffer created against P can begin a twin (VkRenderPassBeginInfo: renderPass compatible with the
//    framebuffer's creation pass, VUID-VkRenderPassBeginInfo-renderPass-00904),
//  - P's pipelines draw inside a twin (VUID-vkCmdDraw-renderPass-02684: current pass compatible with the pipeline's),
//  - a secondary recorded with inheritance renderPass = P, subpass 0, framebuffer F executes inside a twin begun on F
//    (vkCmdExecuteCommands: inheritance render pass compatible with the current one, same subpass index, same framebuffer).
// Rules (per native attachment; reference = its layout in subpass 0, P.final when the subpass does not reference it):
//  empty: loadOp = storeOp==STORE ? LOAD : DONT_CARE (stencil the same), storeOps unchanged, initial = final = P.final.
//         Begun right after P ended (the attachments are in P.final): nothing changes, contents survive where P stored.
//  head:  loadOps unchanged (CLEAR stays CLEAR: the head takes P's clear values), storeOp = stencilStoreOp = STORE,
//         initial unchanged, final = reference (no transition at the head's end).
//  tail:  loadOp = LOAD, stencilLoadOp = LOAD for formats with stencil (else DONT_CARE), storeOps unchanged,
//         initial = reference (exactly where the head left it), final = P.final.
// So head + tail leave every attachment with P's contents, P's store semantics and P's final layout, and neither the
// empty pass nor the tail has a CLEAR: clearValueCount 0 is valid for both (VUID-VkRenderPassBeginInfo-clearValueCount-00902).
// Layout transitions: a split adds none (head ends and tail starts in the reference layout; head starts and tail ends
// exactly like P). An empty twin would transition P.final -> reference -> P.final inside itself, which the copied
// external dependencies need not order after P's store; the EXTRA control is therefore only inserted for passes with
// PassFinalIsReference (no transition at all). The only ordering an inserted boundary needs is then store -> load/store
// of the same attachments: StereoReplay.cpp emitBoundaryBarrier (a global memory barrier between the two instances).
#include <vulkan/vulkan.h>
#include <array>
#include <atomic>
#include <cstdint>
#include "MergeScan.h" // STEREO TEST 20 merge rules M1-M9, texture-reference scan, report counters

namespace rdrvr::engine::stereo {
// Lock-free view of an engine render pass for the boundary test (StereoReplay.cpp twin table).
enum PassTwinFlags : std::uint32_t {
    PassStoresAll=1u,          // every attachment storeOp STORE (stencilStoreOp STORE for stencil formats): P can be its own head
    PassFinalIsReference=2u,   // every referenced attachment ends in its subpass layout: a tail can start where P ended
    PassLoadsAll=4u,           // Every attachment loadOp LOAD (stencilLoadOp LOAD for stencil formats): P continues
                               // exactly what the previous pass stored (merge rule M5)
};
struct PassTwins {VkRenderPass empty{},head{},tail{};std::uint32_t flags{};};
// STEREO TEST boundary variants armed for the current frame (latchBoundaryPhase, once per frame).
// BoundaryMergeR = STEREO TEST 20 window a (right-eye colour replay with merged LOAD passes, worker/secondaries
// splice only; StereoReplayAsync.cpp spliceJob).
enum BoundaryFlags : unsigned {BoundaryExtraR=1u,BoundarySplitR=2u,BoundarySplitL=4u,BoundaryExtraL=8u,BoundaryMergeR=16u};

namespace boundary {
constexpr unsigned maxAttachments=9; // the fragment-rate bridge converts 1..9 native attachments
constexpr unsigned prevDrawPasses=64; // per stage: draws of tape pass k in the previous replay (split point)
constexpr bool hasStencil(VkFormat f) noexcept {return f==VK_FORMAT_S8_UINT||f==VK_FORMAT_D16_UNORM_S8_UINT||f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_D32_SFLOAT_S8_UINT;}
constexpr bool hasDepth(VkFormat f) noexcept {return f==VK_FORMAT_D16_UNORM||f==VK_FORMAT_X8_D24_UNORM_PACK32||f==VK_FORMAT_D32_SFLOAT||f==VK_FORMAT_D16_UNORM_S8_UINT||f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_D32_SFLOAT_S8_UINT;}
// Split a pass after this many draws: half of the previous frame's draws of that pass, at least 8; 0 (no split) below 32.
constexpr unsigned splitPoint(unsigned previousDraws) noexcept {return previousDraws>=32?(previousDraws/2>8?previousDraws/2:8):0;}
// census (driver behaviour: a pass of <50 draws renders direct, 50..~1500 binned/GMEM, >=~1550 direct
// again): 0 = <50, 1 = 50..1399, 2 = 1400..1549 (flip band), 3 = >=1550 (report census.drawHist).
constexpr unsigned drawClass(unsigned draws) noexcept {return draws<50?0u:draws<1400?1u:draws<1550?2u:3u;}
constexpr char opLetter(VkAttachmentLoadOp op) noexcept {return op==VK_ATTACHMENT_LOAD_OP_LOAD?'L':op==VK_ATTACHMENT_LOAD_OP_CLEAR?'C':op==VK_ATTACHMENT_LOAD_OP_DONT_CARE?'D':'N';}
constexpr char opLetter(VkAttachmentStoreOp op) noexcept {return op==VK_ATTACHMENT_STORE_OP_STORE?'S':op==VK_ATTACHMENT_STORE_OP_DONT_CARE?'D':'N';}

// Layout of every attachment in subpass 0 (colour, depth/stencil, input, resolve). False for a shape the twins do not
// support (no subpass, more than one subpass, more than 9 attachments, one attachment referenced with two layouts).
inline bool referenceLayouts(const VkRenderPassCreateInfo& p,std::array<VkImageLayout,maxAttachments>& layout,std::array<bool,maxAttachments>& referenced) noexcept {
    layout.fill(VK_IMAGE_LAYOUT_UNDEFINED);referenced.fill(false);
    if(p.sType!=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO||p.subpassCount!=1||!p.pSubpasses||!p.attachmentCount||p.attachmentCount>maxAttachments||!p.pAttachments)return false;
    const auto& s=p.pSubpasses[0];bool ok=true;
    auto note=[&](const VkAttachmentReference& r){
        if(r.attachment==VK_ATTACHMENT_UNUSED)return;
        if(r.attachment>=p.attachmentCount){ok=false;return;}
        if(referenced[r.attachment]&&layout[r.attachment]!=r.layout){ok=false;return;}
        referenced[r.attachment]=true;layout[r.attachment]=r.layout;};
    if(s.colorAttachmentCount&&!s.pColorAttachments)return false;
    if(s.inputAttachmentCount&&!s.pInputAttachments)return false;
    for(std::uint32_t i=0;i<s.colorAttachmentCount;++i)note(s.pColorAttachments[i]);
    if(s.pResolveAttachments)for(std::uint32_t i=0;i<s.colorAttachmentCount;++i)note(s.pResolveAttachments[i]);
    for(std::uint32_t i=0;i<s.inputAttachmentCount;++i)note(s.pInputAttachments[i]);
    if(s.pDepthStencilAttachment)note(*s.pDepthStencilAttachment);
    return ok;
}
// Create infos of the three twins of P. Non-copyable: the infos point into this object.
struct TwinInfos {
    enum Kind : unsigned {Empty,Head,Tail,Count};
    std::array<std::array<VkAttachmentDescription,maxAttachments>,Count> attachments{};
    std::array<VkRenderPassCreateInfo,Count> infos{};
    std::array<VkImageLayout,maxAttachments> reference{};std::array<bool,maxAttachments> referenced{};
    std::uint32_t flags{}; // PassTwinFlags of P
    TwinInfos()=default;TwinInfos(const TwinInfos&)=delete;TwinInfos& operator=(const TwinInfos&)=delete;
};
// P's flags only (no twins needed): PassStoresAll / PassFinalIsReference. False: unsupported shape.
inline bool passFlags(const VkRenderPassCreateInfo& p,std::uint32_t& flags) noexcept {
    std::array<VkImageLayout,maxAttachments> ref{};std::array<bool,maxAttachments> used{};flags=0;
    if(!referenceLayouts(p,ref,used))return false;
    bool stores=true,finalRef=true,loads=true;
    for(std::uint32_t i=0;i<p.attachmentCount;++i){const auto& a=p.pAttachments[i];
        if(a.storeOp!=VK_ATTACHMENT_STORE_OP_STORE||(hasStencil(a.format)&&a.stencilStoreOp!=VK_ATTACHMENT_STORE_OP_STORE))stores=false;
        if(a.loadOp!=VK_ATTACHMENT_LOAD_OP_LOAD||(hasStencil(a.format)&&a.stencilLoadOp!=VK_ATTACHMENT_LOAD_OP_LOAD))loads=false;
        if(used[i]&&a.finalLayout!=ref[i])finalRef=false;}
    flags=(stores?PassStoresAll:0u)|(finalRef?PassFinalIsReference:0u)|(loads?PassLoadsAll:0u);return true;
}
inline bool deriveTwins(const VkRenderPassCreateInfo& p,TwinInfos& out) noexcept {
    if(!referenceLayouts(p,out.reference,out.referenced)||!passFlags(p,out.flags))return false;
    for(unsigned k=0;k<TwinInfos::Count;++k){
        auto& list=out.attachments[k];
        for(std::uint32_t i=0;i<p.attachmentCount;++i){
            const auto& src=p.pAttachments[i];auto a=src;
            const VkImageLayout ref=out.referenced[i]?out.reference[i]:src.finalLayout;
            switch(k){
            case TwinInfos::Empty:
                a.loadOp=src.storeOp==VK_ATTACHMENT_STORE_OP_STORE?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                a.stencilLoadOp=src.stencilStoreOp==VK_ATTACHMENT_STORE_OP_STORE?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                a.initialLayout=src.finalLayout;a.finalLayout=src.finalLayout;break;
            case TwinInfos::Head:
                a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;a.finalLayout=ref;break;
            default: // Tail
                a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;a.stencilLoadOp=hasStencil(src.format)?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                a.initialLayout=ref;break;
            }
            list[i]=a;
        }
        auto& ci=out.infos[k];ci=p;ci.pAttachments=list.data(); // pNext (multiview), subpasses and dependencies unchanged
    }
    return true;
}
}

// ---- STEREO TEST 20 "R EYE MERGE PASSES" registry/runtime side (defined in StereoReplay.cpp; the pure
// rules live in MergeScan.h). [render] = render thread only (engine texture objects); [any] = takes coldMutex.
namespace merge {
Stats& mergeStats() noexcept;                                           // report stereoReplay.boundaryTest.merge
std::uint32_t leftTextureIndex(std::uintptr_t texture) noexcept;        // [render] engine texture +0x98 -> +0x58; UINT32_MAX none
bool pipelineTextures(VkPipeline pipeline,TexScan& out) noexcept;       // [any] union of the stage scans (patchShaderCreate)
// [any] set-0 dynamic offset index of every set-0 binding of the pipeline layout (0xff: not dynamic / binding >= 32)
bool dynamicSlots(VkPipelineLayout layout,std::array<std::uint8_t,32>& slotOfBinding) noexcept;
unsigned framebufferImages(VkFramebuffer leftFb,std::array<VkImage,9>& images) noexcept; // [any] registered attachment images
// [any] bindless elements (texture binding of every registered bindless set) whose written view's image is one of `images`;
// returns the count, or aliasOverflow (> 16) when more exist (the caller treats the framebuffer as unverifiable).
constexpr unsigned aliasOverflow=17;
unsigned aliasIndices(const VkImage* images,unsigned count,std::array<std::uint32_t,16>& out) noexcept;
bool enabled() noexcept;   // debug.rdrvr.stereoreplay.merge (default 1), re-read every 64th call
unsigned cap() noexcept;   // debug.rdrvr.stereoreplay.mergecap (default defaultCap), clamped minCap..maxCap
void setForTests(int enabled,unsigned cap) noexcept; // enabled -1: property again; cap 0: property again
// Splice safety net (StereoReplayAsync.cpp): decide() said Merge but the splice's own M1-M9 re-check kept the boundary;
// gap barrier calls not emitted because they sat between two merged passes.
inline std::atomic<std::uint64_t> decideOverrides{},gapEntriesSkipped{};
}
}
