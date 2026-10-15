#pragma once
// Vulkan side of SHADOW UPDATE 5 STAGGER / 6 STAGGER FAR (SunStagger.h holds every decision; this header only
// converts the Vulkan structs). Included by TraceEntry.cpp before IndexedDispatch.fragment.inc:
//  * passCreated / passDestroyed (rateCreatePass / rateDestroyPass): the depth-only single-attachment render passes with
//    their depth loadOp, initial layout and format (stagger::passTable).
//  * beginning (indexedForward_vkCmdBeginRenderPass, before the tape / driver): full exact renders learn the atlas extent
//    and the first pass's clear value; partial renders check the pass (LOAD, extent) and, with debug.rdrvr.sunstagger
//    AREA or for a CLEAR / DONT_CARE drawn-tile pass, return a copy whose renderArea is the drawn tile's
//    quadrant.
//  * begun (after the driver's vkCmdBeginRenderPass): records vkCmdClearAttachments(depth = learned clear value, rect =
//    quadrant of the drawn tile) through the raw driver entry (never on a stereo tape: the atlas is drawn once per frame).
//  * viewport (indexedForward_vkCmdSetViewport): quadrant learning on full renders, quadrant check on partial ones.
//  * PLAYER SHADOW WINDOW: begun() also sets the window scissor after the window clear; scissor()
//    (indexedForward_vkCmdSetScissor) clamps every engine scissor into the window while the window tile draws; restore()
//    (SunStagger windowEnd) puts the engine's last requested scissor (or the full atlas) back.
// Every function first tests the relaxed layerArmed gate (true only inside an exact atlas render at a stagger level),
// then the render thread; one relaxed load per engine call otherwise.
#include "ShadowUpdate.h"
#include "StereoReplay.h" // stereo::trackSetScissor / recordingFor for the raw window scissor
#include <vulkan/vulkan.h>
#include <cerrno>
#include <cstdint>
#include <type_traits>

namespace rdrvr::engine::shadowupdate::stagger::layer {
inline PFN_vkCmdClearAttachments rawClear{}; // = indexedRaw_vkCmdClearAttachments (TraceEntry, stereo raw binding)
inline void bindRawClear(PFN_vkCmdClearAttachments f) noexcept {rawClear=f;rawClearBound.store(f!=nullptr,std::memory_order_release);}
// PLAYER SHADOW WINDOW: scissor state of the window tile (render thread).
inline PFN_vkCmdSetScissor rawScissor{}; // = indexedRaw_vkCmdSetScissor (TraceEntry, stereo raw binding)
struct WindowScissor {VkCommandBuffer cmd{};bool touched{},engineSeen{};VkRect2D engine{};};
inline WindowScissor windowScissor;
inline VkRect2D toVk(const Rect& r) noexcept {return {{r.x,r.y},{std::uint32_t(r.w>0?r.w:0),std::uint32_t(r.h>0?r.h:0)}};}
inline Rect fromVk(const VkRect2D& r) noexcept {
    const auto w=r.extent.width>0x40000000u?0x40000000:int(r.extent.width),h=r.extent.height>0x40000000u?0x40000000:int(r.extent.height);
    return {r.offset.x,r.offset.y,w,h};
}
// Raw scissor of the window (never through the engine): the stereo state tracker (and a tape, should one record this
// command buffer) sees it like an engine scissor, so a later tracked-state restore re-emits the real value.
inline void rawSetScissor(VkCommandBuffer c,const VkRect2D& r) noexcept {
    if(!rawScissor||!c)return;
    if(auto* tape=::rdrvr::engine::stereo::recordingFor(c))::rdrvr::engine::stereo::recordSetScissor(*tape,c,0,1,&r);
    ::rdrvr::engine::stereo::trackSetScissor(c,0,1,&r);
    rawScissor(c,0,1,&r);
}
// windowEnd (render thread): the engine's last requested scissor (seen while clamped), else the full atlas = DisableScissor.
inline void restoreScissor() noexcept {
    auto& w=windowScissor;
    if(w.touched&&w.cmd&&rawScissor){
        const VkRect2D r=w.engineSeen?w.engine:VkRect2D{{0,0},{learned.width,learned.height}};
        const int saved=errno;rawSetScissor(w.cmd,r);errno=saved;
    }
    w=WindowScissor{};
}
inline void bindRawScissor(PFN_vkCmdSetScissor f) noexcept {
    rawScissor=f;scissorRestore=f?&restoreScissor:nullptr;rawScissorBound.store(f!=nullptr,std::memory_order_release);
}
// Engine vkCmdSetScissor: while the window tile draws, every rect is clamped into the window (nothing outside the window may
// change: the rest of the quadrant keeps its stale content, and the pass renderArea may be the window only).
inline const VkRect2D* scissor(VkCommandBuffer c,std::uint32_t first,std::uint32_t n,const VkRect2D* v,VkRect2D (&tmp)[4]) noexcept {
    if(!windowScissorArmed.load(std::memory_order_relaxed))return v;
    if(!v||!n||n>4||!onRenderThread())return v; // the atlas uses one viewport
    const Rect window=rt.windowRect;
    if(!first){windowScissor.engine=v[0];windowScissor.engineSeen=true;}
    for(std::uint32_t i=0;i<n;++i)tmp[i]=toVk(player::intersect(fromVk(v[i]),window));
    windowScissor.cmd=c;windowScissor.touched=true;add(playerCounters.scissorClamps);
    return tmp;
}
template<class H> inline std::uint64_t handleBits(H h) noexcept {
    if constexpr(std::is_pointer_v<H>)return std::uint64_t(reinterpret_cast<std::uintptr_t>(h));else return std::uint64_t(h);
}
inline bool depthFormat(VkFormat f) noexcept {
    return f==VK_FORMAT_D16_UNORM||f==VK_FORMAT_X8_D24_UNORM_PACK32||f==VK_FORMAT_D32_SFLOAT||f==VK_FORMAT_D16_UNORM_S8_UINT||
           f==VK_FORMAT_D24_UNORM_S8_UINT||f==VK_FORMAT_D32_SFLOAT_S8_UINT;
}
// Every vkCreateRenderPass (any thread): remember the depth-only single-attachment ones.
inline void passCreated(VkRenderPass rp,const VkRenderPassCreateInfo* c) noexcept {
    if(!rp||!c||c->attachmentCount!=1||!c->pAttachments||c->subpassCount!=1||!c->pSubpasses)return;
    const auto& sp=c->pSubpasses[0];
    if(sp.colorAttachmentCount||!sp.pDepthStencilAttachment||sp.pDepthStencilAttachment->attachment!=0||!depthFormat(c->pAttachments[0].format))return;
    passTable.created(handleBits(rp),int(c->pAttachments[0].loadOp),int(c->pAttachments[0].initialLayout),int(c->pAttachments[0].format));
}
inline void passDestroyed(VkRenderPass rp) noexcept {if(rp)passTable.destroyed(handleBits(rp));}
// Before the driver's vkCmdBeginRenderPass.
inline const VkRenderPassBeginInfo* beginning(VkCommandBuffer,const VkRenderPassBeginInfo* b,VkRenderPassBeginInfo& tmp) noexcept {
    if(!layerArmed.load(std::memory_order_relaxed))return b;
    if(!b||!onRenderThread())return b;
    const int saved=errno;
    const bool hasClear=b->clearValueCount>=1&&b->pClearValues;
    const Rect* area=passBeginning(handleBits(b->renderPass),b->renderArea.offset.x,b->renderArea.offset.y,b->renderArea.extent.width,
                                   b->renderArea.extent.height,hasClear,hasClear?b->pClearValues[0].depthStencil.depth:-1.f);
    errno=saved;
    if(!area)return b;
    tmp=*b;tmp.renderArea={{area->x,area->y},{std::uint32_t(area->w),std::uint32_t(area->h)}};
    return &tmp;
}
// After the driver's vkCmdBeginRenderPass: the drawn tile's quadrant clear as the first command of its pass.
inline void begun(VkCommandBuffer c,const VkRenderPassBeginInfo*,VkSubpassContents s) noexcept {
    if(!layerArmed.load(std::memory_order_relaxed))return;
    if(!onRenderThread())return;
    Rect rect{};float depth{};
    if(!passBegun(s==VK_SUBPASS_CONTENTS_INLINE,rawClear!=nullptr,rect,depth))return;
    const int saved=errno;
    VkClearAttachment attachment{};attachment.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;attachment.colorAttachment=0;
    attachment.clearValue.depthStencil.depth=depth;attachment.clearValue.depthStencil.stencil=0;
    VkClearRect area{};area.rect={{rect.x,rect.y},{std::uint32_t(rect.w),std::uint32_t(rect.h)}};area.baseArrayLayer=0;area.layerCount=1;
    rawClear(c,1,&attachment,1,&area);
    const bool window=windowClearPending(); // The player window's clear -> the window scissor for the tile's draws
    clearInserted();
    if(window&&rawScissor){const VkRect2D w=toVk(rt.windowRect);rawSetScissor(c,w);windowScissor.cmd=c;windowScissor.touched=true;}
    errno=saved;
}
inline void viewport(VkCommandBuffer,std::uint32_t first,std::uint32_t n,const VkViewport* v) noexcept {
    if(!layerArmed.load(std::memory_order_relaxed))return;
    if(!v||!n||first||!onRenderThread())return;
    viewportSeen(v->x,v->y,v->width,v->height);
}
// vkCreateGraphicsPipelines while the mod's partial call runs (first use of the LOAD /
// tracked-layout pass variant).
inline void pipelinesCreated(std::uint32_t n) noexcept {stagger::pipelinesCreated(n);}
} // namespace rdrvr::engine::shadowupdate::stagger::layer
