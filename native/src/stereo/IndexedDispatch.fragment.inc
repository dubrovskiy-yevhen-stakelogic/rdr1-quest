// vkGetDeviceProcAddr may return a different entry from the loader export.
// Publish immutable exact-forwarding thunks, never retarget a handed-out thunk.
template<class Fn> struct IndexedProcAliases {
    std::array<std::atomic<Fn>,4> raw{};
    Fn route(Fn incoming,Fn canonical,Fn canonicalWrapper,const std::array<Fn,4>& wrappers)noexcept {
        if(!incoming||!canonical)return nullptr;
        if(incoming==canonical||incoming==canonicalWrapper)return canonicalWrapper;
        for(unsigned i=0;i<raw.size();++i)if(incoming==wrappers[i])
            return raw[i].load(std::memory_order_acquire)?wrappers[i]:nullptr;
        for(unsigned i=0;i<raw.size();++i){
            auto value=raw[i].load(std::memory_order_acquire);
            if(value==incoming)return wrappers[i];
            if(!value&&raw[i].compare_exchange_strong(value,incoming,std::memory_order_acq_rel))return wrappers[i];
            if(value==incoming)return wrappers[i];
        }
        return nullptr; // Unknown further route: decline observation, preserve native.
    }
};
// Generated from saved exact OBJECT8 symbols by prepare_dispatch.py.
// Included in TraceEntry private namespace. Raw forwarding remains once.
// Default observation OFF; no timings, uniform reads or object admission.
constexpr unsigned indexedDispatchCount=37;
inline constexpr std::uintptr_t indexedSlot_vkCmdBindVertexBuffers=0x4e6940a8;
PFN_vkCmdBindVertexBuffers indexedRaw_vkCmdBindVertexBuffers{};
// record + track + next in one stereo function (one TLS resolve); `next` travels in stereo::threadHot.vertexNext
constexpr PFN_vkCmdBindVertexBuffers stereoRecordedBindVertexBuffers=&rdrvr::engine::stereo::hookBindVertexBuffers;
inline void indexedForward_vkCmdBindVertexBuffers(PFN_vkCmdBindVertexBuffers next,VkCommandBuffer c,uint32_t first,uint32_t n,const VkBuffer* b,const VkDeviceSize* o,std::uintptr_t caller){
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){rdrvr::engine::stereo::threadHot.vertexNext=next;nativeVertexStreamMerge58.bind(stereoRecordedBindVertexBuffers,caller,c,first,n,b,o);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    std::array<VkBuffer,8> savedBuffers;std::array<VkDeviceSize,8> savedOffsets;captured=stamp&&n<=8&&(!n||(b&&o));if(captured){std::copy_n(b,n,savedBuffers.data());std::copy_n(o,n,savedOffsets.data());}
    
    next(c,first,n,b,o);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.vertices(c,first,n,savedBuffers.data(),savedOffsets.data());}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdBindVertexBuffers> indexedAliases_vkCmdBindVertexBuffers;
__attribute__((noinline)) VKAPI_ATTR void VKAPI_CALL indexed_vkCmdBindVertexBuffers(VkCommandBuffer c,uint32_t first,uint32_t n,const VkBuffer* b,const VkDeviceSize* o){indexedForward_vkCmdBindVertexBuffers(indexedRaw_vkCmdBindVertexBuffers,c,first,n,b,o,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));}
template<unsigned I> __attribute__((noinline)) VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdBindVertexBuffers(VkCommandBuffer c,uint32_t first,uint32_t n,const VkBuffer* b,const VkDeviceSize* o){
    indexedForward_vkCmdBindVertexBuffers(indexedAliases_vkCmdBindVertexBuffers.raw[I].load(std::memory_order_acquire),c,first,n,b,o,reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
}
inline constexpr std::uintptr_t indexedSlot_vkCmdBindIndexBuffer=0x4e6940a0;
PFN_vkCmdBindIndexBuffer indexedRaw_vkCmdBindIndexBuffer{};
inline void indexedForward_vkCmdBindIndexBuffer(PFN_vkCmdBindIndexBuffer next,VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType t){
    rdrvr::engine::stereo::hookBindIndexBuffer(c,b,o,t);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,b,o,t);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,b,o,t);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.index(c,b,o,t);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdBindIndexBuffer> indexedAliases_vkCmdBindIndexBuffer;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType t){indexedForward_vkCmdBindIndexBuffer(indexedRaw_vkCmdBindIndexBuffer,c,b,o,t);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdBindIndexBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkIndexType t){
    indexedForward_vkCmdBindIndexBuffer(indexedAliases_vkCmdBindIndexBuffer.raw[I].load(std::memory_order_acquire),c,b,o,t);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdSetViewport=0x4e694050;
PFN_vkCmdSetViewport indexedRaw_vkCmdSetViewport{};
inline void indexedForward_vkCmdSetViewport(PFN_vkCmdSetViewport next,VkCommandBuffer c,uint32_t first,uint32_t n,const VkViewport* v){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordSetViewport(*stereoTape_,c,first,n,v);rdrvr::engine::stereo::trackSetViewport(c,first,n,v);
    ::rdrvr::engine::shadowupdate::stagger::layer::viewport(c,first,n,v); // Atlas tile quadrant (one relaxed load otherwise)
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,first,n,v);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    VkViewport savedValue;captured=stamp&&n==1&&v;if(captured)savedValue=*v;
    
    next(c,first,n,v);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.viewport(c,first,n,&savedValue);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdSetViewport> indexedAliases_vkCmdSetViewport;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdSetViewport(VkCommandBuffer c,uint32_t first,uint32_t n,const VkViewport* v){indexedForward_vkCmdSetViewport(indexedRaw_vkCmdSetViewport,c,first,n,v);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdSetViewport(VkCommandBuffer c,uint32_t first,uint32_t n,const VkViewport* v){
    indexedForward_vkCmdSetViewport(indexedAliases_vkCmdSetViewport.raw[I].load(std::memory_order_acquire),c,first,n,v);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdSetScissor=0x4e694058;
PFN_vkCmdSetScissor indexedRaw_vkCmdSetScissor{};
inline void indexedForward_vkCmdSetScissor(PFN_vkCmdSetScissor next,VkCommandBuffer c,uint32_t first,uint32_t n,const VkRect2D* v){
    VkRect2D stagScissor_[4];v=::rdrvr::engine::shadowupdate::stagger::layer::scissor(c,first,n,v,stagScissor_); // Player window clamp (one relaxed load otherwise)
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordSetScissor(*stereoTape_,c,first,n,v);rdrvr::engine::stereo::trackSetScissor(c,first,n,v);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,first,n,v);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    VkRect2D savedValue;captured=stamp&&n==1&&v;if(captured)savedValue=*v;
    
    next(c,first,n,v);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.scissor(c,first,n,&savedValue);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdSetScissor> indexedAliases_vkCmdSetScissor;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdSetScissor(VkCommandBuffer c,uint32_t first,uint32_t n,const VkRect2D* v){indexedForward_vkCmdSetScissor(indexedRaw_vkCmdSetScissor,c,first,n,v);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdSetScissor(VkCommandBuffer c,uint32_t first,uint32_t n,const VkRect2D* v){
    indexedForward_vkCmdSetScissor(indexedAliases_vkCmdSetScissor.raw[I].load(std::memory_order_acquire),c,first,n,v);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdSetLineWidth=0x4e694060;
PFN_vkCmdSetLineWidth indexedRaw_vkCmdSetLineWidth{};
inline void indexedForward_vkCmdSetLineWidth(PFN_vkCmdSetLineWidth next,VkCommandBuffer c,float v){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordSetLineWidth(*stereoTape_,c,v);rdrvr::engine::stereo::trackSetLineWidth(c,v);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,v);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,v);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.lineWidth(c,v);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdSetLineWidth> indexedAliases_vkCmdSetLineWidth;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdSetLineWidth(VkCommandBuffer c,float v){indexedForward_vkCmdSetLineWidth(indexedRaw_vkCmdSetLineWidth,c,v);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdSetLineWidth(VkCommandBuffer c,float v){
    indexedForward_vkCmdSetLineWidth(indexedAliases_vkCmdSetLineWidth.raw[I].load(std::memory_order_acquire),c,v);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdSetDepthBias=0x4e694068;
PFN_vkCmdSetDepthBias indexedRaw_vkCmdSetDepthBias{};
inline void indexedForward_vkCmdSetDepthBias(PFN_vkCmdSetDepthBias next,VkCommandBuffer c,float a,float b,float d){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordSetDepthBias(*stereoTape_,c,a,b,d);rdrvr::engine::stereo::trackSetDepthBias(c,a,b,d);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,a,b,d);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,a,b,d);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.depthBias(c,a,b,d);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdSetDepthBias> indexedAliases_vkCmdSetDepthBias;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdSetDepthBias(VkCommandBuffer c,float a,float b,float d){indexedForward_vkCmdSetDepthBias(indexedRaw_vkCmdSetDepthBias,c,a,b,d);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdSetDepthBias(VkCommandBuffer c,float a,float b,float d){
    indexedForward_vkCmdSetDepthBias(indexedAliases_vkCmdSetDepthBias.raw[I].load(std::memory_order_acquire),c,a,b,d);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdSetBlendConstants=0x4e694070;
PFN_vkCmdSetBlendConstants indexedRaw_vkCmdSetBlendConstants{};
inline void indexedForward_vkCmdSetBlendConstants(PFN_vkCmdSetBlendConstants next,VkCommandBuffer c,const float v[4]){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordSetBlendConstants(*stereoTape_,c,v);rdrvr::engine::stereo::trackSetBlendConstants(c,v);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,v);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    std::array<float,4> savedValue;captured=stamp&&v;if(captured)std::copy_n(v,4,savedValue.data());
    
    next(c,v);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.blendConstants(c,savedValue.data());}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdSetBlendConstants> indexedAliases_vkCmdSetBlendConstants;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdSetBlendConstants(VkCommandBuffer c,const float v[4]){indexedForward_vkCmdSetBlendConstants(indexedRaw_vkCmdSetBlendConstants,c,v);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdSetBlendConstants(VkCommandBuffer c,const float v[4]){
    indexedForward_vkCmdSetBlendConstants(indexedAliases_vkCmdSetBlendConstants.raw[I].load(std::memory_order_acquire),c,v);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdSetDepthBounds=0x4e694078;
PFN_vkCmdSetDepthBounds indexedRaw_vkCmdSetDepthBounds{};
inline void indexedForward_vkCmdSetDepthBounds(PFN_vkCmdSetDepthBounds next,VkCommandBuffer c,float a,float b){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordSetDepthBounds(*stereoTape_,c,a,b);rdrvr::engine::stereo::trackSetDepthBounds(c,a,b);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,a,b);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,a,b);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.depthBounds(c,a,b);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdSetDepthBounds> indexedAliases_vkCmdSetDepthBounds;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdSetDepthBounds(VkCommandBuffer c,float a,float b){indexedForward_vkCmdSetDepthBounds(indexedRaw_vkCmdSetDepthBounds,c,a,b);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdSetDepthBounds(VkCommandBuffer c,float a,float b){
    indexedForward_vkCmdSetDepthBounds(indexedAliases_vkCmdSetDepthBounds.raw[I].load(std::memory_order_acquire),c,a,b);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdSetStencilCompareMask=0x4e694080;
PFN_vkCmdSetStencilCompareMask indexedRaw_vkCmdSetStencilCompareMask{};
inline void indexedForward_vkCmdSetStencilCompareMask(PFN_vkCmdSetStencilCompareMask next,VkCommandBuffer c,VkStencilFaceFlags f,uint32_t v){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordSetStencilCompareMask(*stereoTape_,c,f,v);rdrvr::engine::stereo::trackSetStencilCompareMask(c,f,v);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,f,v);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,f,v);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.stencilCompare(c,f,v);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdSetStencilCompareMask> indexedAliases_vkCmdSetStencilCompareMask;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdSetStencilCompareMask(VkCommandBuffer c,VkStencilFaceFlags f,uint32_t v){indexedForward_vkCmdSetStencilCompareMask(indexedRaw_vkCmdSetStencilCompareMask,c,f,v);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdSetStencilCompareMask(VkCommandBuffer c,VkStencilFaceFlags f,uint32_t v){
    indexedForward_vkCmdSetStencilCompareMask(indexedAliases_vkCmdSetStencilCompareMask.raw[I].load(std::memory_order_acquire),c,f,v);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdSetStencilWriteMask=0x4e694088;
PFN_vkCmdSetStencilWriteMask indexedRaw_vkCmdSetStencilWriteMask{};
inline void indexedForward_vkCmdSetStencilWriteMask(PFN_vkCmdSetStencilWriteMask next,VkCommandBuffer c,VkStencilFaceFlags f,uint32_t v){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordSetStencilWriteMask(*stereoTape_,c,f,v);rdrvr::engine::stereo::trackSetStencilWriteMask(c,f,v);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,f,v);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,f,v);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.stencilWrite(c,f,v);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdSetStencilWriteMask> indexedAliases_vkCmdSetStencilWriteMask;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdSetStencilWriteMask(VkCommandBuffer c,VkStencilFaceFlags f,uint32_t v){indexedForward_vkCmdSetStencilWriteMask(indexedRaw_vkCmdSetStencilWriteMask,c,f,v);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdSetStencilWriteMask(VkCommandBuffer c,VkStencilFaceFlags f,uint32_t v){
    indexedForward_vkCmdSetStencilWriteMask(indexedAliases_vkCmdSetStencilWriteMask.raw[I].load(std::memory_order_acquire),c,f,v);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdSetStencilReference=0x4e694090;
PFN_vkCmdSetStencilReference indexedRaw_vkCmdSetStencilReference{};
inline void indexedForward_vkCmdSetStencilReference(PFN_vkCmdSetStencilReference next,VkCommandBuffer c,VkStencilFaceFlags f,uint32_t v){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordSetStencilReference(*stereoTape_,c,f,v);rdrvr::engine::stereo::trackSetStencilReference(c,f,v);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,f,v);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,f,v);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.stencilReference(c,f,v);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdSetStencilReference> indexedAliases_vkCmdSetStencilReference;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdSetStencilReference(VkCommandBuffer c,VkStencilFaceFlags f,uint32_t v){indexedForward_vkCmdSetStencilReference(indexedRaw_vkCmdSetStencilReference,c,f,v);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdSetStencilReference(VkCommandBuffer c,VkStencilFaceFlags f,uint32_t v){
    indexedForward_vkCmdSetStencilReference(indexedAliases_vkCmdSetStencilReference.raw[I].load(std::memory_order_acquire),c,f,v);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdPushConstants=0x4e694180;
PFN_vkCmdPushConstants indexedRaw_vkCmdPushConstants{};
inline void indexedForward_vkCmdPushConstants(PFN_vkCmdPushConstants next,VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags f,uint32_t o,uint32_t n,const void* b){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordPushConstants(*stereoTape_,c,l,f,o,n,b);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,l,f,o,n,b);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    std::array<std::uint8_t,128> savedBytes;captured=stamp&&n<=128&&(!n||b);if(captured&&n)std::memcpy(savedBytes.data(),b,n);
    const auto generation=indexedObservation.layoutBefore(c,l);
    next(c,l,f,o,n,b);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.push(c,l,generation,f,o,n,savedBytes.data());}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdPushConstants> indexedAliases_vkCmdPushConstants;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags f,uint32_t o,uint32_t n,const void* b){indexedForward_vkCmdPushConstants(indexedRaw_vkCmdPushConstants,c,l,f,o,n,b);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdPushConstants(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags f,uint32_t o,uint32_t n,const void* b){
    indexedForward_vkCmdPushConstants(indexedAliases_vkCmdPushConstants.raw[I].load(std::memory_order_acquire),c,l,f,o,n,b);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdBeginRenderPass=0x4e694188;
PFN_vkCmdBeginRenderPass indexedRaw_vkCmdBeginRenderPass{};
inline void indexedForward_vkCmdBeginRenderPass(PFN_vkCmdBeginRenderPass next,VkCommandBuffer c,const VkRenderPassBeginInfo* b,VkSubpassContents s){
    // An eye framebuffer is replaced by its foveation variant BEFORE the stereo tape records the begin, so the
    // right-eye replay resolves the variant (same render pass, last view = the map of the current level).
    int foveaLevel_=-1; // lowfreq: level of the begun eye pass's foveation map (-1: not a foveated eye pass)
    VkRenderPassBeginInfo foveated_;if(b&&::rdrvr::engine::foveationArmed.load(std::memory_order_relaxed)&&::rdrvr::engine::foveationBegin(*b,foveated_,&foveaLevel_))b=&foveated_;
    ::rdrvr::engine::lowfreq::passBegun(reinterpret_cast<std::uintptr_t>(c),foveaLevel_); // lowfreq: twins bind only in HIGH/ULTRA eye passes (one relaxed load until a twin exists)
    // sun atlas pass of a STAGGER render (learning / LOAD check / AREA renderArea); one relaxed load otherwise
    VkRenderPassBeginInfo stag_;b=::rdrvr::engine::shadowupdate::stagger::layer::beginning(c,b,stag_);
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordBeginRenderPass(*stereoTape_,c,b,s);
    rdrvr::engine::stereo::leftPassBegun(c,b,s);  // STEREO TEST 12: the left pass that may be split (one relaxed load when off)
    rdrvr::engine::stereo::gpuPassBeginning(c,b); // DETAIL GPU stage markers + per-pass timestamp, before the pass
    ::rdrvr::engine::colorrestart::censusPass(true); // LR census (EngineColorRestart.h): light-stage passes; one relaxed load otherwise
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,b,s);::rdrvr::engine::shadowupdate::stagger::layer::begun(c,b,s);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    VkRenderPassBeginInfo savedBegin;captured=stamp&&b;if(captured)savedBegin=*b;
    
    next(c,b,s);::rdrvr::engine::shadowupdate::stagger::layer::begun(c,b,s);const int saved=errno; // The drawn tile's quadrant clear
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.beginPass(c,&savedBegin,s);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdBeginRenderPass> indexedAliases_vkCmdBeginRenderPass;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo* b,VkSubpassContents s){indexedForward_vkCmdBeginRenderPass(indexedRaw_vkCmdBeginRenderPass,c,b,s);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdBeginRenderPass(VkCommandBuffer c,const VkRenderPassBeginInfo* b,VkSubpassContents s){
    indexedForward_vkCmdBeginRenderPass(indexedAliases_vkCmdBeginRenderPass.raw[I].load(std::memory_order_acquire),c,b,s);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdEndRenderPass=0x4e694198;
PFN_vkCmdEndRenderPass indexedRaw_vkCmdEndRenderPass{};
inline void indexedForward_vkCmdEndRenderPass(PFN_vkCmdEndRenderPass next,VkCommandBuffer c){
    ::rdrvr::engine::lowfreq::passEnded(reinterpret_cast<std::uintptr_t>(c)); // lowfreq: outside a pass the engine's own pipeline
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordEndRenderPass(*stereoTape_,c);
    ::rdrvr::engine::colorrestart::censusPass(false); // LR census: the pass ends (later barriers: gap / edge)
    // leftPassEnded (STEREO TEST 12, EXTRA variant) sits between the engine End and gpuPassEnded so that deferred
    // stage markers flush after the inserted empty twin
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c);rdrvr::engine::stereo::leftPassEnded(c);rdrvr::engine::stereo::gpuPassEnded(c);return;} // DETAIL GPU markers after the pass
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c);rdrvr::engine::stereo::leftPassEnded(c);rdrvr::engine::stereo::gpuPassEnded(c);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.endPass(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdEndRenderPass> indexedAliases_vkCmdEndRenderPass;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdEndRenderPass(VkCommandBuffer c){indexedForward_vkCmdEndRenderPass(indexedRaw_vkCmdEndRenderPass,c);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdEndRenderPass(VkCommandBuffer c){
    indexedForward_vkCmdEndRenderPass(indexedAliases_vkCmdEndRenderPass.raw[I].load(std::memory_order_acquire),c);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdNextSubpass=0x4e694190;
PFN_vkCmdNextSubpass indexedRaw_vkCmdNextSubpass{};
inline void indexedForward_vkCmdNextSubpass(PFN_vkCmdNextSubpass next,VkCommandBuffer c,VkSubpassContents s){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordNextSubpass(*stereoTape_,c,s);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,s);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,s);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.unknownState(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdNextSubpass> indexedAliases_vkCmdNextSubpass;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdNextSubpass(VkCommandBuffer c,VkSubpassContents s){indexedForward_vkCmdNextSubpass(indexedRaw_vkCmdNextSubpass,c,s);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdNextSubpass(VkCommandBuffer c,VkSubpassContents s){
    indexedForward_vkCmdNextSubpass(indexedAliases_vkCmdNextSubpass.raw[I].load(std::memory_order_acquire),c,s);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdDrawIndirect=0x4e6940c0;
PFN_vkCmdDrawIndirect indexedRaw_vkCmdDrawIndirect{};
inline void indexedForward_vkCmdDrawIndirect(PFN_vkCmdDrawIndirect next,VkCommandBuffer c,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t s){
    if(::rdrvr::engine::targetroute::discard(::rdrvr::engine::targetroute::Discard::DrawIndirect))return;
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordDrawIndirect(*stereoTape_,c,b,o,n,s);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,b,o,n,s);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,b,o,n,s);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdDrawIndirect> indexedAliases_vkCmdDrawIndirect;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdDrawIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t s){indexedForward_vkCmdDrawIndirect(indexedRaw_vkCmdDrawIndirect,c,b,o,n,s);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdDrawIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t s){
    indexedForward_vkCmdDrawIndirect(indexedAliases_vkCmdDrawIndirect.raw[I].load(std::memory_order_acquire),c,b,o,n,s);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdDrawIndexedIndirect=0x4e6940c8;
PFN_vkCmdDrawIndexedIndirect indexedRaw_vkCmdDrawIndexedIndirect{};
inline void indexedForward_vkCmdDrawIndexedIndirect(PFN_vkCmdDrawIndexedIndirect next,VkCommandBuffer c,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t s){
    if(::rdrvr::engine::targetroute::discard(::rdrvr::engine::targetroute::Discard::DrawIndexedIndirect))return;
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordDrawIndexedIndirect(*stereoTape_,c,b,o,n,s);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,b,o,n,s);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,b,o,n,s);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdDrawIndexedIndirect> indexedAliases_vkCmdDrawIndexedIndirect;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdDrawIndexedIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t s){indexedForward_vkCmdDrawIndexedIndirect(indexedRaw_vkCmdDrawIndexedIndirect,c,b,o,n,s);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdDrawIndexedIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,uint32_t n,uint32_t s){
    indexedForward_vkCmdDrawIndexedIndirect(indexedAliases_vkCmdDrawIndexedIndirect.raw[I].load(std::memory_order_acquire),c,b,o,n,s);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdDispatch=0x4e6940d0;
PFN_vkCmdDispatch indexedRaw_vkCmdDispatch{};
inline void indexedForward_vkCmdDispatch(PFN_vkCmdDispatch next,VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordUnsupported(*stereoTape_,c,"vkCmdDispatch");
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,x,y,z);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,x,y,z);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdDispatch> indexedAliases_vkCmdDispatch;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z){indexedForward_vkCmdDispatch(indexedRaw_vkCmdDispatch,c,x,y,z);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdDispatch(VkCommandBuffer c,uint32_t x,uint32_t y,uint32_t z){
    indexedForward_vkCmdDispatch(indexedAliases_vkCmdDispatch.raw[I].load(std::memory_order_acquire),c,x,y,z);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdDispatchIndirect=0x4e6940d8;
PFN_vkCmdDispatchIndirect indexedRaw_vkCmdDispatchIndirect{};
inline void indexedForward_vkCmdDispatchIndirect(PFN_vkCmdDispatchIndirect next,VkCommandBuffer c,VkBuffer b,VkDeviceSize o){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordUnsupported(*stereoTape_,c,"vkCmdDispatchIndirect");
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,b,o);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,b,o);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdDispatchIndirect> indexedAliases_vkCmdDispatchIndirect;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdDispatchIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o){indexedForward_vkCmdDispatchIndirect(indexedRaw_vkCmdDispatchIndirect,c,b,o);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdDispatchIndirect(VkCommandBuffer c,VkBuffer b,VkDeviceSize o){
    indexedForward_vkCmdDispatchIndirect(indexedAliases_vkCmdDispatchIndirect.raw[I].load(std::memory_order_acquire),c,b,o);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdCopyBuffer=0x4e6940e0;
PFN_vkCmdCopyBuffer indexedRaw_vkCmdCopyBuffer{};
inline void indexedForward_vkCmdCopyBuffer(PFN_vkCmdCopyBuffer next,VkCommandBuffer c,VkBuffer s,VkBuffer d,uint32_t n,const VkBufferCopy* r){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordCopyBuffer(*stereoTape_,c,s,d,n,r);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,s,d,n,r);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,s,d,n,r);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdCopyBuffer> indexedAliases_vkCmdCopyBuffer;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdCopyBuffer(VkCommandBuffer c,VkBuffer s,VkBuffer d,uint32_t n,const VkBufferCopy* r){indexedForward_vkCmdCopyBuffer(indexedRaw_vkCmdCopyBuffer,c,s,d,n,r);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdCopyBuffer(VkCommandBuffer c,VkBuffer s,VkBuffer d,uint32_t n,const VkBufferCopy* r){
    indexedForward_vkCmdCopyBuffer(indexedAliases_vkCmdCopyBuffer.raw[I].load(std::memory_order_acquire),c,s,d,n,r);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdCopyImage=0x4e6940e8;
PFN_vkCmdCopyImage indexedRaw_vkCmdCopyImage{};
inline void indexedForward_vkCmdCopyImage(PFN_vkCmdCopyImage next,VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageCopy* r){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordCopyImage(*stereoTape_,c,s,sl,d,dl,n,r);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,s,sl,d,dl,n,r);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,s,sl,d,dl,n,r);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdCopyImage> indexedAliases_vkCmdCopyImage;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdCopyImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageCopy* r){indexedForward_vkCmdCopyImage(indexedRaw_vkCmdCopyImage,c,s,sl,d,dl,n,r);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdCopyImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageCopy* r){
    indexedForward_vkCmdCopyImage(indexedAliases_vkCmdCopyImage.raw[I].load(std::memory_order_acquire),c,s,sl,d,dl,n,r);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdBlitImage=0x4e6940f0;
PFN_vkCmdBlitImage indexedRaw_vkCmdBlitImage{};
inline void indexedForward_vkCmdBlitImage(PFN_vkCmdBlitImage next,VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageBlit* r,VkFilter f){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordBlitImage(*stereoTape_,c,s,sl,d,dl,n,r,f);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,s,sl,d,dl,n,r,f);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,s,sl,d,dl,n,r,f);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdBlitImage> indexedAliases_vkCmdBlitImage;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdBlitImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageBlit* r,VkFilter f){indexedForward_vkCmdBlitImage(indexedRaw_vkCmdBlitImage,c,s,sl,d,dl,n,r,f);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdBlitImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageBlit* r,VkFilter f){
    indexedForward_vkCmdBlitImage(indexedAliases_vkCmdBlitImage.raw[I].load(std::memory_order_acquire),c,s,sl,d,dl,n,r,f);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdCopyBufferToImage=0x4e6940f8;
PFN_vkCmdCopyBufferToImage indexedRaw_vkCmdCopyBufferToImage{};
inline void indexedForward_vkCmdCopyBufferToImage(PFN_vkCmdCopyBufferToImage next,VkCommandBuffer c,VkBuffer b,VkImage d,VkImageLayout l,uint32_t n,const VkBufferImageCopy* r){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordCopyBufferToImage(*stereoTape_,c,b,d,l,n,r);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,b,d,l,n,r);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,b,d,l,n,r);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdCopyBufferToImage> indexedAliases_vkCmdCopyBufferToImage;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdCopyBufferToImage(VkCommandBuffer c,VkBuffer b,VkImage d,VkImageLayout l,uint32_t n,const VkBufferImageCopy* r){indexedForward_vkCmdCopyBufferToImage(indexedRaw_vkCmdCopyBufferToImage,c,b,d,l,n,r);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdCopyBufferToImage(VkCommandBuffer c,VkBuffer b,VkImage d,VkImageLayout l,uint32_t n,const VkBufferImageCopy* r){
    indexedForward_vkCmdCopyBufferToImage(indexedAliases_vkCmdCopyBufferToImage.raw[I].load(std::memory_order_acquire),c,b,d,l,n,r);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdCopyImageToBuffer=0x4e694100;
PFN_vkCmdCopyImageToBuffer indexedRaw_vkCmdCopyImageToBuffer{};
inline void indexedForward_vkCmdCopyImageToBuffer(PFN_vkCmdCopyImageToBuffer next,VkCommandBuffer c,VkImage s,VkImageLayout l,VkBuffer d,uint32_t n,const VkBufferImageCopy* r){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordCopyImageToBuffer(*stereoTape_,c,s,l,d,n,r);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,s,l,d,n,r);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,s,l,d,n,r);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdCopyImageToBuffer> indexedAliases_vkCmdCopyImageToBuffer;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdCopyImageToBuffer(VkCommandBuffer c,VkImage s,VkImageLayout l,VkBuffer d,uint32_t n,const VkBufferImageCopy* r){indexedForward_vkCmdCopyImageToBuffer(indexedRaw_vkCmdCopyImageToBuffer,c,s,l,d,n,r);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdCopyImageToBuffer(VkCommandBuffer c,VkImage s,VkImageLayout l,VkBuffer d,uint32_t n,const VkBufferImageCopy* r){
    indexedForward_vkCmdCopyImageToBuffer(indexedAliases_vkCmdCopyImageToBuffer.raw[I].load(std::memory_order_acquire),c,s,l,d,n,r);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdUpdateBuffer=0x4e694108;
PFN_vkCmdUpdateBuffer indexedRaw_vkCmdUpdateBuffer{};
inline void indexedForward_vkCmdUpdateBuffer(PFN_vkCmdUpdateBuffer next,VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize n,const void* p){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordUpdateBuffer(*stereoTape_,c,b,o,n,p);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,b,o,n,p);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,b,o,n,p);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdUpdateBuffer> indexedAliases_vkCmdUpdateBuffer;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdUpdateBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize n,const void* p){indexedForward_vkCmdUpdateBuffer(indexedRaw_vkCmdUpdateBuffer,c,b,o,n,p);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdUpdateBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize n,const void* p){
    indexedForward_vkCmdUpdateBuffer(indexedAliases_vkCmdUpdateBuffer.raw[I].load(std::memory_order_acquire),c,b,o,n,p);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdFillBuffer=0x4e694110;
PFN_vkCmdFillBuffer indexedRaw_vkCmdFillBuffer{};
inline void indexedForward_vkCmdFillBuffer(PFN_vkCmdFillBuffer next,VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize n,uint32_t v){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordFillBuffer(*stereoTape_,c,b,o,n,v);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,b,o,n,v);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,b,o,n,v);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdFillBuffer> indexedAliases_vkCmdFillBuffer;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdFillBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize n,uint32_t v){indexedForward_vkCmdFillBuffer(indexedRaw_vkCmdFillBuffer,c,b,o,n,v);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdFillBuffer(VkCommandBuffer c,VkBuffer b,VkDeviceSize o,VkDeviceSize n,uint32_t v){
    indexedForward_vkCmdFillBuffer(indexedAliases_vkCmdFillBuffer.raw[I].load(std::memory_order_acquire),c,b,o,n,v);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdClearColorImage=0x4e694118;
PFN_vkCmdClearColorImage indexedRaw_vkCmdClearColorImage{};
inline void indexedForward_vkCmdClearColorImage(PFN_vkCmdClearColorImage next,VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearColorValue* v,uint32_t n,const VkImageSubresourceRange* r){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordClearColorImage(*stereoTape_,c,i,l,v,n,r);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,i,l,v,n,r);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,i,l,v,n,r);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdClearColorImage> indexedAliases_vkCmdClearColorImage;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdClearColorImage(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearColorValue* v,uint32_t n,const VkImageSubresourceRange* r){indexedForward_vkCmdClearColorImage(indexedRaw_vkCmdClearColorImage,c,i,l,v,n,r);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdClearColorImage(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearColorValue* v,uint32_t n,const VkImageSubresourceRange* r){
    indexedForward_vkCmdClearColorImage(indexedAliases_vkCmdClearColorImage.raw[I].load(std::memory_order_acquire),c,i,l,v,n,r);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdClearDepthStencilImage=0x4e694120;
PFN_vkCmdClearDepthStencilImage indexedRaw_vkCmdClearDepthStencilImage{};
inline void indexedForward_vkCmdClearDepthStencilImage(PFN_vkCmdClearDepthStencilImage next,VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearDepthStencilValue* v,uint32_t n,const VkImageSubresourceRange* r){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordClearDepthStencilImage(*stereoTape_,c,i,l,v,n,r);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,i,l,v,n,r);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,i,l,v,n,r);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdClearDepthStencilImage> indexedAliases_vkCmdClearDepthStencilImage;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdClearDepthStencilImage(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearDepthStencilValue* v,uint32_t n,const VkImageSubresourceRange* r){indexedForward_vkCmdClearDepthStencilImage(indexedRaw_vkCmdClearDepthStencilImage,c,i,l,v,n,r);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdClearDepthStencilImage(VkCommandBuffer c,VkImage i,VkImageLayout l,const VkClearDepthStencilValue* v,uint32_t n,const VkImageSubresourceRange* r){
    indexedForward_vkCmdClearDepthStencilImage(indexedAliases_vkCmdClearDepthStencilImage.raw[I].load(std::memory_order_acquire),c,i,l,v,n,r);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdClearAttachments=0x4e694128;
PFN_vkCmdClearAttachments indexedRaw_vkCmdClearAttachments{};
inline void indexedForward_vkCmdClearAttachments(PFN_vkCmdClearAttachments next,VkCommandBuffer c,uint32_t n,const VkClearAttachment* a,uint32_t m,const VkClearRect* r){
    // Mandatory: the driver reads [passState+0x4e8]+0x100 unconditionally
    if(::rdrvr::engine::targetroute::discard(::rdrvr::engine::targetroute::Discard::ClearAttachments))return;
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordClearAttachments(*stereoTape_,c,n,a,m,r);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,n,a,m,r);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,n,a,m,r);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdClearAttachments> indexedAliases_vkCmdClearAttachments;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdClearAttachments(VkCommandBuffer c,uint32_t n,const VkClearAttachment* a,uint32_t m,const VkClearRect* r){indexedForward_vkCmdClearAttachments(indexedRaw_vkCmdClearAttachments,c,n,a,m,r);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdClearAttachments(VkCommandBuffer c,uint32_t n,const VkClearAttachment* a,uint32_t m,const VkClearRect* r){
    indexedForward_vkCmdClearAttachments(indexedAliases_vkCmdClearAttachments.raw[I].load(std::memory_order_acquire),c,n,a,m,r);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdResolveImage=0x4e694130;
PFN_vkCmdResolveImage indexedRaw_vkCmdResolveImage{};
inline void indexedForward_vkCmdResolveImage(PFN_vkCmdResolveImage next,VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageResolve* r){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordResolveImage(*stereoTape_,c,s,sl,d,dl,n,r);
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,s,sl,d,dl,n,r);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,s,sl,d,dl,n,r);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdResolveImage> indexedAliases_vkCmdResolveImage;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdResolveImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageResolve* r){indexedForward_vkCmdResolveImage(indexedRaw_vkCmdResolveImage,c,s,sl,d,dl,n,r);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdResolveImage(VkCommandBuffer c,VkImage s,VkImageLayout sl,VkImage d,VkImageLayout dl,uint32_t n,const VkImageResolve* r){
    indexedForward_vkCmdResolveImage(indexedAliases_vkCmdResolveImage.raw[I].load(std::memory_order_acquire),c,s,sl,d,dl,n,r);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdSetEvent=0x4e694138;
PFN_vkCmdSetEvent indexedRaw_vkCmdSetEvent{};
inline void indexedForward_vkCmdSetEvent(PFN_vkCmdSetEvent next,VkCommandBuffer c,VkEvent e,VkPipelineStageFlags f){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordUnsupported(*stereoTape_,c,"vkCmdSetEvent");
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,e,f);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,e,f);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdSetEvent> indexedAliases_vkCmdSetEvent;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdSetEvent(VkCommandBuffer c,VkEvent e,VkPipelineStageFlags f){indexedForward_vkCmdSetEvent(indexedRaw_vkCmdSetEvent,c,e,f);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdSetEvent(VkCommandBuffer c,VkEvent e,VkPipelineStageFlags f){
    indexedForward_vkCmdSetEvent(indexedAliases_vkCmdSetEvent.raw[I].load(std::memory_order_acquire),c,e,f);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdResetEvent=0x4e694140;
PFN_vkCmdResetEvent indexedRaw_vkCmdResetEvent{};
inline void indexedForward_vkCmdResetEvent(PFN_vkCmdResetEvent next,VkCommandBuffer c,VkEvent e,VkPipelineStageFlags f){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordUnsupported(*stereoTape_,c,"vkCmdResetEvent");
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,e,f);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,e,f);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdResetEvent> indexedAliases_vkCmdResetEvent;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdResetEvent(VkCommandBuffer c,VkEvent e,VkPipelineStageFlags f){indexedForward_vkCmdResetEvent(indexedRaw_vkCmdResetEvent,c,e,f);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdResetEvent(VkCommandBuffer c,VkEvent e,VkPipelineStageFlags f){
    indexedForward_vkCmdResetEvent(indexedAliases_vkCmdResetEvent.raw[I].load(std::memory_order_acquire),c,e,f);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdWaitEvents=0x4e694148;
PFN_vkCmdWaitEvents indexedRaw_vkCmdWaitEvents{};
inline void indexedForward_vkCmdWaitEvents(PFN_vkCmdWaitEvents next,VkCommandBuffer c,uint32_t n,const VkEvent* e,VkPipelineStageFlags s,VkPipelineStageFlags d,uint32_t m,const VkMemoryBarrier* mb,uint32_t b,const VkBufferMemoryBarrier* bb,uint32_t i,const VkImageMemoryBarrier* ib){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordUnsupported(*stereoTape_,c,"vkCmdWaitEvents");
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,n,e,s,d,m,mb,b,bb,i,ib);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,n,e,s,d,m,mb,b,bb,i,ib);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdWaitEvents> indexedAliases_vkCmdWaitEvents;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdWaitEvents(VkCommandBuffer c,uint32_t n,const VkEvent* e,VkPipelineStageFlags s,VkPipelineStageFlags d,uint32_t m,const VkMemoryBarrier* mb,uint32_t b,const VkBufferMemoryBarrier* bb,uint32_t i,const VkImageMemoryBarrier* ib){indexedForward_vkCmdWaitEvents(indexedRaw_vkCmdWaitEvents,c,n,e,s,d,m,mb,b,bb,i,ib);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdWaitEvents(VkCommandBuffer c,uint32_t n,const VkEvent* e,VkPipelineStageFlags s,VkPipelineStageFlags d,uint32_t m,const VkMemoryBarrier* mb,uint32_t b,const VkBufferMemoryBarrier* bb,uint32_t i,const VkImageMemoryBarrier* ib){
    indexedForward_vkCmdWaitEvents(indexedAliases_vkCmdWaitEvents.raw[I].load(std::memory_order_acquire),c,n,e,s,d,m,mb,b,bb,i,ib);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdPipelineBarrier=0x4e694150;
PFN_vkCmdPipelineBarrier indexedRaw_vkCmdPipelineBarrier{};
inline void indexedForward_vkCmdPipelineBarrier(PFN_vkCmdPipelineBarrier next,VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags f,uint32_t m,const VkMemoryBarrier* mb,uint32_t b,const VkBufferMemoryBarrier* bb,uint32_t i,const VkImageMemoryBarrier* ib){
    if(auto* stereoTape_=rdrvr::engine::stereo::recordingFor(c))rdrvr::engine::stereo::recordPipelineBarrier(*stereoTape_,c,s,d,f,m,mb,b,bb,i,ib);
    ::rdrvr::engine::colorrestart::censusBarrier(i,ib); // LR census: light-stage barriers by position, layout changes in restart gaps
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,s,d,f,m,mb,b,bb,i,ib);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,s,d,f,m,mb,b,bb,i,ib);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdPipelineBarrier> indexedAliases_vkCmdPipelineBarrier;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags f,uint32_t m,const VkMemoryBarrier* mb,uint32_t b,const VkBufferMemoryBarrier* bb,uint32_t i,const VkImageMemoryBarrier* ib){indexedForward_vkCmdPipelineBarrier(indexedRaw_vkCmdPipelineBarrier,c,s,d,f,m,mb,b,bb,i,ib);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdPipelineBarrier(VkCommandBuffer c,VkPipelineStageFlags s,VkPipelineStageFlags d,VkDependencyFlags f,uint32_t m,const VkMemoryBarrier* mb,uint32_t b,const VkBufferMemoryBarrier* bb,uint32_t i,const VkImageMemoryBarrier* ib){
    indexedForward_vkCmdPipelineBarrier(indexedAliases_vkCmdPipelineBarrier.raw[I].load(std::memory_order_acquire),c,s,d,f,m,mb,b,bb,i,ib);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdResetQueryPool=0x4e694168;
PFN_vkCmdResetQueryPool indexedRaw_vkCmdResetQueryPool{};
inline void indexedForward_vkCmdResetQueryPool(PFN_vkCmdResetQueryPool next,VkCommandBuffer c,VkQueryPool p,uint32_t f,uint32_t n){
    /* stereo replay: query/timestamp commands are not replayed */
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,p,f,n);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,p,f,n);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdResetQueryPool> indexedAliases_vkCmdResetQueryPool;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdResetQueryPool(VkCommandBuffer c,VkQueryPool p,uint32_t f,uint32_t n){indexedForward_vkCmdResetQueryPool(indexedRaw_vkCmdResetQueryPool,c,p,f,n);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdResetQueryPool(VkCommandBuffer c,VkQueryPool p,uint32_t f,uint32_t n){
    indexedForward_vkCmdResetQueryPool(indexedAliases_vkCmdResetQueryPool.raw[I].load(std::memory_order_acquire),c,p,f,n);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdWriteTimestamp=0x4e694170;
PFN_vkCmdWriteTimestamp indexedRaw_vkCmdWriteTimestamp{};
inline void indexedForward_vkCmdWriteTimestamp(PFN_vkCmdWriteTimestamp next,VkCommandBuffer c,VkPipelineStageFlagBits s,VkQueryPool p,uint32_t i){
    /* stereo replay: query/timestamp commands are not replayed */
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,s,p,i);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,s,p,i);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdWriteTimestamp> indexedAliases_vkCmdWriteTimestamp;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits s,VkQueryPool p,uint32_t i){indexedForward_vkCmdWriteTimestamp(indexedRaw_vkCmdWriteTimestamp,c,s,p,i);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdWriteTimestamp(VkCommandBuffer c,VkPipelineStageFlagBits s,VkQueryPool p,uint32_t i){
    indexedForward_vkCmdWriteTimestamp(indexedAliases_vkCmdWriteTimestamp.raw[I].load(std::memory_order_acquire),c,s,p,i);
}
inline constexpr std::uintptr_t indexedSlot_vkCmdCopyQueryPoolResults=0x4e694178;
PFN_vkCmdCopyQueryPoolResults indexedRaw_vkCmdCopyQueryPoolResults{};
inline void indexedForward_vkCmdCopyQueryPoolResults(PFN_vkCmdCopyQueryPoolResults next,VkCommandBuffer c,VkQueryPool p,uint32_t f,uint32_t n,VkBuffer b,VkDeviceSize o,VkDeviceSize s,VkQueryResultFlags flags){
    /* stereo replay: query/timestamp commands are not replayed */
    if constexpr(!rdrvr::engine::ordinaryMainStartupRequested()){next(c,p,f,n,b,o,s,flags);return;}
    const auto stamp=indexedObservation.beginCall(c);bool captured=bool(stamp);
    
    
    next(c,p,f,n,b,o,s,flags);const int saved=errno;
    if(indexedObservation.finishCall(stamp)){if(captured){indexedObservation.sideEffect(c);}else indexedObservation.unknownState(c);}
    errno=saved;
}
IndexedProcAliases<PFN_vkCmdCopyQueryPoolResults> indexedAliases_vkCmdCopyQueryPoolResults;
VKAPI_ATTR void VKAPI_CALL indexed_vkCmdCopyQueryPoolResults(VkCommandBuffer c,VkQueryPool p,uint32_t f,uint32_t n,VkBuffer b,VkDeviceSize o,VkDeviceSize s,VkQueryResultFlags flags){indexedForward_vkCmdCopyQueryPoolResults(indexedRaw_vkCmdCopyQueryPoolResults,c,p,f,n,b,o,s,flags);}
template<unsigned I> VKAPI_ATTR void VKAPI_CALL indexedAlias_vkCmdCopyQueryPoolResults(VkCommandBuffer c,VkQueryPool p,uint32_t f,uint32_t n,VkBuffer b,VkDeviceSize o,VkDeviceSize s,VkQueryResultFlags flags){
    indexedForward_vkCmdCopyQueryPoolResults(indexedAliases_vkCmdCopyQueryPoolResults.raw[I].load(std::memory_order_acquire),c,p,f,n,b,o,s,flags);
}
// Native extension pointer is requested through the intercepted GetProc.
// Retain its exact raw identity; never guess an extension ABI from a slot.
std::atomic<PFN_vkCmdSetDepthBoundsTestEnable> indexedRaw_depthEnable{};
VKAPI_ATTR void VKAPI_CALL indexedDepthEnableRaw(VkCommandBuffer c,VkBool32 v){
    const auto fn=indexedRaw_depthEnable.load(std::memory_order_acquire);
    if(fn)fn(c,v);else indexedObservation.disarm("IndexedDispatch.fragment.inc:380");
}
VKAPI_ATTR void VKAPI_CALL indexedDepthEnable(VkCommandBuffer c,VkBool32 v){
    const auto stamp=indexedObservation.beginCall(c);indexedDepthEnableRaw(c,v);const int saved=errno;
    if(indexedObservation.finishCall(stamp))indexedObservation.depthBoundsEnable(c,v);errno=saved;
}
PFN_vkVoidFunction indexedProc(PFN_vkVoidFunction original,const char* name){
    if(!original||!name||!cameraRoutesInstalled.load(std::memory_order_acquire))return original;
    if(!std::strcmp(name,"vkCmdSetDepthBoundsTestEnable")||!std::strcmp(name,"vkCmdSetDepthBoundsTestEnableEXT")){
        const auto fn=reinterpret_cast<PFN_vkCmdSetDepthBoundsTestEnable>(original);auto expected=fn;
        auto prior=indexedRaw_depthEnable.load(std::memory_order_acquire);
        if(!prior){prior=nullptr;indexedRaw_depthEnable.compare_exchange_strong(prior,fn,std::memory_order_acq_rel);}
        if(indexedRaw_depthEnable.load(std::memory_order_acquire)!=expected){indexedObservation.disarm("IndexedDispatch.fragment.inc:392");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(indexedDepthEnable);
    }
    if(!std::strcmp(name,"vkCmdBindVertexBuffers")){
        const auto route=indexedAliases_vkCmdBindVertexBuffers.route(reinterpret_cast<PFN_vkCmdBindVertexBuffers>(original),indexedRaw_vkCmdBindVertexBuffers,indexed_vkCmdBindVertexBuffers,
            {indexedAlias_vkCmdBindVertexBuffers<0>,indexedAlias_vkCmdBindVertexBuffers<1>,indexedAlias_vkCmdBindVertexBuffers<2>,indexedAlias_vkCmdBindVertexBuffers<3>});
        if(!route){nativeVertexStreamMerge58.disable();indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdBindVertexBuffers");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdBindIndexBuffer")){
        const auto route=indexedAliases_vkCmdBindIndexBuffer.route(reinterpret_cast<PFN_vkCmdBindIndexBuffer>(original),indexedRaw_vkCmdBindIndexBuffer,indexed_vkCmdBindIndexBuffer,
            {indexedAlias_vkCmdBindIndexBuffer<0>,indexedAlias_vkCmdBindIndexBuffer<1>,indexedAlias_vkCmdBindIndexBuffer<2>,indexedAlias_vkCmdBindIndexBuffer<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdBindIndexBuffer");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdSetViewport")){
        const auto route=indexedAliases_vkCmdSetViewport.route(reinterpret_cast<PFN_vkCmdSetViewport>(original),indexedRaw_vkCmdSetViewport,indexed_vkCmdSetViewport,
            {indexedAlias_vkCmdSetViewport<0>,indexedAlias_vkCmdSetViewport<1>,indexedAlias_vkCmdSetViewport<2>,indexedAlias_vkCmdSetViewport<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdSetViewport");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdSetScissor")){
        const auto route=indexedAliases_vkCmdSetScissor.route(reinterpret_cast<PFN_vkCmdSetScissor>(original),indexedRaw_vkCmdSetScissor,indexed_vkCmdSetScissor,
            {indexedAlias_vkCmdSetScissor<0>,indexedAlias_vkCmdSetScissor<1>,indexedAlias_vkCmdSetScissor<2>,indexedAlias_vkCmdSetScissor<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdSetScissor");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdSetLineWidth")){
        const auto route=indexedAliases_vkCmdSetLineWidth.route(reinterpret_cast<PFN_vkCmdSetLineWidth>(original),indexedRaw_vkCmdSetLineWidth,indexed_vkCmdSetLineWidth,
            {indexedAlias_vkCmdSetLineWidth<0>,indexedAlias_vkCmdSetLineWidth<1>,indexedAlias_vkCmdSetLineWidth<2>,indexedAlias_vkCmdSetLineWidth<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdSetLineWidth");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdSetDepthBias")){
        const auto route=indexedAliases_vkCmdSetDepthBias.route(reinterpret_cast<PFN_vkCmdSetDepthBias>(original),indexedRaw_vkCmdSetDepthBias,indexed_vkCmdSetDepthBias,
            {indexedAlias_vkCmdSetDepthBias<0>,indexedAlias_vkCmdSetDepthBias<1>,indexedAlias_vkCmdSetDepthBias<2>,indexedAlias_vkCmdSetDepthBias<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdSetDepthBias");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdSetBlendConstants")){
        const auto route=indexedAliases_vkCmdSetBlendConstants.route(reinterpret_cast<PFN_vkCmdSetBlendConstants>(original),indexedRaw_vkCmdSetBlendConstants,indexed_vkCmdSetBlendConstants,
            {indexedAlias_vkCmdSetBlendConstants<0>,indexedAlias_vkCmdSetBlendConstants<1>,indexedAlias_vkCmdSetBlendConstants<2>,indexedAlias_vkCmdSetBlendConstants<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdSetBlendConstants");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdSetDepthBounds")){
        const auto route=indexedAliases_vkCmdSetDepthBounds.route(reinterpret_cast<PFN_vkCmdSetDepthBounds>(original),indexedRaw_vkCmdSetDepthBounds,indexed_vkCmdSetDepthBounds,
            {indexedAlias_vkCmdSetDepthBounds<0>,indexedAlias_vkCmdSetDepthBounds<1>,indexedAlias_vkCmdSetDepthBounds<2>,indexedAlias_vkCmdSetDepthBounds<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdSetDepthBounds");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdSetStencilCompareMask")){
        const auto route=indexedAliases_vkCmdSetStencilCompareMask.route(reinterpret_cast<PFN_vkCmdSetStencilCompareMask>(original),indexedRaw_vkCmdSetStencilCompareMask,indexed_vkCmdSetStencilCompareMask,
            {indexedAlias_vkCmdSetStencilCompareMask<0>,indexedAlias_vkCmdSetStencilCompareMask<1>,indexedAlias_vkCmdSetStencilCompareMask<2>,indexedAlias_vkCmdSetStencilCompareMask<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdSetStencilCompareMask");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdSetStencilWriteMask")){
        const auto route=indexedAliases_vkCmdSetStencilWriteMask.route(reinterpret_cast<PFN_vkCmdSetStencilWriteMask>(original),indexedRaw_vkCmdSetStencilWriteMask,indexed_vkCmdSetStencilWriteMask,
            {indexedAlias_vkCmdSetStencilWriteMask<0>,indexedAlias_vkCmdSetStencilWriteMask<1>,indexedAlias_vkCmdSetStencilWriteMask<2>,indexedAlias_vkCmdSetStencilWriteMask<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdSetStencilWriteMask");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdSetStencilReference")){
        const auto route=indexedAliases_vkCmdSetStencilReference.route(reinterpret_cast<PFN_vkCmdSetStencilReference>(original),indexedRaw_vkCmdSetStencilReference,indexed_vkCmdSetStencilReference,
            {indexedAlias_vkCmdSetStencilReference<0>,indexedAlias_vkCmdSetStencilReference<1>,indexedAlias_vkCmdSetStencilReference<2>,indexedAlias_vkCmdSetStencilReference<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdSetStencilReference");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdPushConstants")){
        const auto route=indexedAliases_vkCmdPushConstants.route(reinterpret_cast<PFN_vkCmdPushConstants>(original),indexedRaw_vkCmdPushConstants,indexed_vkCmdPushConstants,
            {indexedAlias_vkCmdPushConstants<0>,indexedAlias_vkCmdPushConstants<1>,indexedAlias_vkCmdPushConstants<2>,indexedAlias_vkCmdPushConstants<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdPushConstants");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdBeginRenderPass")){
        const auto route=indexedAliases_vkCmdBeginRenderPass.route(reinterpret_cast<PFN_vkCmdBeginRenderPass>(original),indexedRaw_vkCmdBeginRenderPass,indexed_vkCmdBeginRenderPass,
            {indexedAlias_vkCmdBeginRenderPass<0>,indexedAlias_vkCmdBeginRenderPass<1>,indexedAlias_vkCmdBeginRenderPass<2>,indexedAlias_vkCmdBeginRenderPass<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdBeginRenderPass");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdEndRenderPass")){
        const auto route=indexedAliases_vkCmdEndRenderPass.route(reinterpret_cast<PFN_vkCmdEndRenderPass>(original),indexedRaw_vkCmdEndRenderPass,indexed_vkCmdEndRenderPass,
            {indexedAlias_vkCmdEndRenderPass<0>,indexedAlias_vkCmdEndRenderPass<1>,indexedAlias_vkCmdEndRenderPass<2>,indexedAlias_vkCmdEndRenderPass<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdEndRenderPass");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdNextSubpass")){
        const auto route=indexedAliases_vkCmdNextSubpass.route(reinterpret_cast<PFN_vkCmdNextSubpass>(original),indexedRaw_vkCmdNextSubpass,indexed_vkCmdNextSubpass,
            {indexedAlias_vkCmdNextSubpass<0>,indexedAlias_vkCmdNextSubpass<1>,indexedAlias_vkCmdNextSubpass<2>,indexedAlias_vkCmdNextSubpass<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdNextSubpass");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdDrawIndirect")){
        const auto route=indexedAliases_vkCmdDrawIndirect.route(reinterpret_cast<PFN_vkCmdDrawIndirect>(original),indexedRaw_vkCmdDrawIndirect,indexed_vkCmdDrawIndirect,
            {indexedAlias_vkCmdDrawIndirect<0>,indexedAlias_vkCmdDrawIndirect<1>,indexedAlias_vkCmdDrawIndirect<2>,indexedAlias_vkCmdDrawIndirect<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdDrawIndirect");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdDrawIndexedIndirect")){
        const auto route=indexedAliases_vkCmdDrawIndexedIndirect.route(reinterpret_cast<PFN_vkCmdDrawIndexedIndirect>(original),indexedRaw_vkCmdDrawIndexedIndirect,indexed_vkCmdDrawIndexedIndirect,
            {indexedAlias_vkCmdDrawIndexedIndirect<0>,indexedAlias_vkCmdDrawIndexedIndirect<1>,indexedAlias_vkCmdDrawIndexedIndirect<2>,indexedAlias_vkCmdDrawIndexedIndirect<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdDrawIndexedIndirect");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdDispatch")){
        const auto route=indexedAliases_vkCmdDispatch.route(reinterpret_cast<PFN_vkCmdDispatch>(original),indexedRaw_vkCmdDispatch,indexed_vkCmdDispatch,
            {indexedAlias_vkCmdDispatch<0>,indexedAlias_vkCmdDispatch<1>,indexedAlias_vkCmdDispatch<2>,indexedAlias_vkCmdDispatch<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdDispatch");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdDispatchIndirect")){
        const auto route=indexedAliases_vkCmdDispatchIndirect.route(reinterpret_cast<PFN_vkCmdDispatchIndirect>(original),indexedRaw_vkCmdDispatchIndirect,indexed_vkCmdDispatchIndirect,
            {indexedAlias_vkCmdDispatchIndirect<0>,indexedAlias_vkCmdDispatchIndirect<1>,indexedAlias_vkCmdDispatchIndirect<2>,indexedAlias_vkCmdDispatchIndirect<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdDispatchIndirect");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdCopyBuffer")){
        const auto route=indexedAliases_vkCmdCopyBuffer.route(reinterpret_cast<PFN_vkCmdCopyBuffer>(original),indexedRaw_vkCmdCopyBuffer,indexed_vkCmdCopyBuffer,
            {indexedAlias_vkCmdCopyBuffer<0>,indexedAlias_vkCmdCopyBuffer<1>,indexedAlias_vkCmdCopyBuffer<2>,indexedAlias_vkCmdCopyBuffer<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdCopyBuffer");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdCopyImage")){
        const auto route=indexedAliases_vkCmdCopyImage.route(reinterpret_cast<PFN_vkCmdCopyImage>(original),indexedRaw_vkCmdCopyImage,indexed_vkCmdCopyImage,
            {indexedAlias_vkCmdCopyImage<0>,indexedAlias_vkCmdCopyImage<1>,indexedAlias_vkCmdCopyImage<2>,indexedAlias_vkCmdCopyImage<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdCopyImage");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdBlitImage")){
        const auto route=indexedAliases_vkCmdBlitImage.route(reinterpret_cast<PFN_vkCmdBlitImage>(original),indexedRaw_vkCmdBlitImage,indexed_vkCmdBlitImage,
            {indexedAlias_vkCmdBlitImage<0>,indexedAlias_vkCmdBlitImage<1>,indexedAlias_vkCmdBlitImage<2>,indexedAlias_vkCmdBlitImage<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdBlitImage");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdCopyBufferToImage")){
        const auto route=indexedAliases_vkCmdCopyBufferToImage.route(reinterpret_cast<PFN_vkCmdCopyBufferToImage>(original),indexedRaw_vkCmdCopyBufferToImage,indexed_vkCmdCopyBufferToImage,
            {indexedAlias_vkCmdCopyBufferToImage<0>,indexedAlias_vkCmdCopyBufferToImage<1>,indexedAlias_vkCmdCopyBufferToImage<2>,indexedAlias_vkCmdCopyBufferToImage<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdCopyBufferToImage");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdCopyImageToBuffer")){
        const auto route=indexedAliases_vkCmdCopyImageToBuffer.route(reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(original),indexedRaw_vkCmdCopyImageToBuffer,indexed_vkCmdCopyImageToBuffer,
            {indexedAlias_vkCmdCopyImageToBuffer<0>,indexedAlias_vkCmdCopyImageToBuffer<1>,indexedAlias_vkCmdCopyImageToBuffer<2>,indexedAlias_vkCmdCopyImageToBuffer<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdCopyImageToBuffer");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdUpdateBuffer")){
        const auto route=indexedAliases_vkCmdUpdateBuffer.route(reinterpret_cast<PFN_vkCmdUpdateBuffer>(original),indexedRaw_vkCmdUpdateBuffer,indexed_vkCmdUpdateBuffer,
            {indexedAlias_vkCmdUpdateBuffer<0>,indexedAlias_vkCmdUpdateBuffer<1>,indexedAlias_vkCmdUpdateBuffer<2>,indexedAlias_vkCmdUpdateBuffer<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdUpdateBuffer");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdFillBuffer")){
        const auto route=indexedAliases_vkCmdFillBuffer.route(reinterpret_cast<PFN_vkCmdFillBuffer>(original),indexedRaw_vkCmdFillBuffer,indexed_vkCmdFillBuffer,
            {indexedAlias_vkCmdFillBuffer<0>,indexedAlias_vkCmdFillBuffer<1>,indexedAlias_vkCmdFillBuffer<2>,indexedAlias_vkCmdFillBuffer<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdFillBuffer");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdClearColorImage")){
        const auto route=indexedAliases_vkCmdClearColorImage.route(reinterpret_cast<PFN_vkCmdClearColorImage>(original),indexedRaw_vkCmdClearColorImage,indexed_vkCmdClearColorImage,
            {indexedAlias_vkCmdClearColorImage<0>,indexedAlias_vkCmdClearColorImage<1>,indexedAlias_vkCmdClearColorImage<2>,indexedAlias_vkCmdClearColorImage<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdClearColorImage");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdClearDepthStencilImage")){
        const auto route=indexedAliases_vkCmdClearDepthStencilImage.route(reinterpret_cast<PFN_vkCmdClearDepthStencilImage>(original),indexedRaw_vkCmdClearDepthStencilImage,indexed_vkCmdClearDepthStencilImage,
            {indexedAlias_vkCmdClearDepthStencilImage<0>,indexedAlias_vkCmdClearDepthStencilImage<1>,indexedAlias_vkCmdClearDepthStencilImage<2>,indexedAlias_vkCmdClearDepthStencilImage<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdClearDepthStencilImage");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdClearAttachments")){
        const auto route=indexedAliases_vkCmdClearAttachments.route(reinterpret_cast<PFN_vkCmdClearAttachments>(original),indexedRaw_vkCmdClearAttachments,indexed_vkCmdClearAttachments,
            {indexedAlias_vkCmdClearAttachments<0>,indexedAlias_vkCmdClearAttachments<1>,indexedAlias_vkCmdClearAttachments<2>,indexedAlias_vkCmdClearAttachments<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdClearAttachments");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdResolveImage")){
        const auto route=indexedAliases_vkCmdResolveImage.route(reinterpret_cast<PFN_vkCmdResolveImage>(original),indexedRaw_vkCmdResolveImage,indexed_vkCmdResolveImage,
            {indexedAlias_vkCmdResolveImage<0>,indexedAlias_vkCmdResolveImage<1>,indexedAlias_vkCmdResolveImage<2>,indexedAlias_vkCmdResolveImage<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdResolveImage");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdSetEvent")){
        const auto route=indexedAliases_vkCmdSetEvent.route(reinterpret_cast<PFN_vkCmdSetEvent>(original),indexedRaw_vkCmdSetEvent,indexed_vkCmdSetEvent,
            {indexedAlias_vkCmdSetEvent<0>,indexedAlias_vkCmdSetEvent<1>,indexedAlias_vkCmdSetEvent<2>,indexedAlias_vkCmdSetEvent<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdSetEvent");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdResetEvent")){
        const auto route=indexedAliases_vkCmdResetEvent.route(reinterpret_cast<PFN_vkCmdResetEvent>(original),indexedRaw_vkCmdResetEvent,indexed_vkCmdResetEvent,
            {indexedAlias_vkCmdResetEvent<0>,indexedAlias_vkCmdResetEvent<1>,indexedAlias_vkCmdResetEvent<2>,indexedAlias_vkCmdResetEvent<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdResetEvent");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdWaitEvents")){
        const auto route=indexedAliases_vkCmdWaitEvents.route(reinterpret_cast<PFN_vkCmdWaitEvents>(original),indexedRaw_vkCmdWaitEvents,indexed_vkCmdWaitEvents,
            {indexedAlias_vkCmdWaitEvents<0>,indexedAlias_vkCmdWaitEvents<1>,indexedAlias_vkCmdWaitEvents<2>,indexedAlias_vkCmdWaitEvents<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdWaitEvents");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdPipelineBarrier")){
        const auto route=indexedAliases_vkCmdPipelineBarrier.route(reinterpret_cast<PFN_vkCmdPipelineBarrier>(original),indexedRaw_vkCmdPipelineBarrier,indexed_vkCmdPipelineBarrier,
            {indexedAlias_vkCmdPipelineBarrier<0>,indexedAlias_vkCmdPipelineBarrier<1>,indexedAlias_vkCmdPipelineBarrier<2>,indexedAlias_vkCmdPipelineBarrier<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdPipelineBarrier");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdResetQueryPool")){
        const auto route=indexedAliases_vkCmdResetQueryPool.route(reinterpret_cast<PFN_vkCmdResetQueryPool>(original),indexedRaw_vkCmdResetQueryPool,indexed_vkCmdResetQueryPool,
            {indexedAlias_vkCmdResetQueryPool<0>,indexedAlias_vkCmdResetQueryPool<1>,indexedAlias_vkCmdResetQueryPool<2>,indexedAlias_vkCmdResetQueryPool<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdResetQueryPool");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdWriteTimestamp")){
        const auto route=indexedAliases_vkCmdWriteTimestamp.route(reinterpret_cast<PFN_vkCmdWriteTimestamp>(original),indexedRaw_vkCmdWriteTimestamp,indexed_vkCmdWriteTimestamp,
            {indexedAlias_vkCmdWriteTimestamp<0>,indexedAlias_vkCmdWriteTimestamp<1>,indexedAlias_vkCmdWriteTimestamp<2>,indexedAlias_vkCmdWriteTimestamp<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdWriteTimestamp");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    if(!std::strcmp(name,"vkCmdCopyQueryPoolResults")){
        const auto route=indexedAliases_vkCmdCopyQueryPoolResults.route(reinterpret_cast<PFN_vkCmdCopyQueryPoolResults>(original),indexedRaw_vkCmdCopyQueryPoolResults,indexed_vkCmdCopyQueryPoolResults,
            {indexedAlias_vkCmdCopyQueryPoolResults<0>,indexedAlias_vkCmdCopyQueryPoolResults<1>,indexedAlias_vkCmdCopyQueryPoolResults<2>,indexedAlias_vkCmdCopyQueryPoolResults<3>});
        if(!route){indexedObservation.disarm("PROC_ROUTE_CAPACITY:vkCmdCopyQueryPoolResults");return original;}
        return reinterpret_cast<PFN_vkVoidFunction>(route);
    }
    return original;
}
