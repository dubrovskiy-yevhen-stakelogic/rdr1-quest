// Each diagnostic lease is lexical to the actual selected native eye/pass.
struct CameraGamePass {
 struct Ticket69 {const StereoCameraPlan* plan{};std::uint64_t nonce{},generation{},retailOrdinal{};bool qualified{};};
 static Ticket69 ticket69(bool selected,unsigned eye,rdr95::CameraMainRoute::Stage stage,const void* viewport,std::uint64_t scene,std::uint64_t tracking)noexcept {
  Ticket69 out{};if(!selected||eye>1||!experimental::nativeModelProbeReady69.load(std::memory_order_acquire)||
    (stage!=rdr95::CameraMainRoute::Stage::Depth&&stage!=rdr95::CameraMainRoute::Stage::Color))return out;
  out.retailOrdinal=retailScenes.load(std::memory_order_acquire);
  if(!experimental::renderAblation54.nativeModelProbeLease(out.retailOrdinal,out.nonce,out.generation))return out;
  const auto* views=gameEyeViewports();
  if(!views||!views->ready()||!views->matches(eye)||views->viewport(eye)!=viewport||
    views->plan().sceneId!=scene||views->plan().tracking.frameId!=tracking||!gameGraphicsEyeQualified(viewport))return out;
  out.plan=&views->plan();out.qualified=true;return out;
 }
 Ticket69 ticket;
 experimental::RenderAblation54::EyePass ablation54;
 experimental::NativeModelProbe69::Eye modelProbe69;
 CameraGamePass(bool selected,unsigned eye,rdr95::CameraMainRoute::Stage stage,const void* viewport,std::uint64_t scene,std::uint64_t tracking)noexcept:
  ticket(ticket69(selected,eye,stage,viewport,scene,tracking)),
  ablation54(experimental::renderAblation54,selected,eye,stage==rdr95::CameraMainRoute::Stage::Depth||stage==rdr95::CameraMainRoute::Stage::Color),
  modelProbe69(experimental::nativeModelProbe69,viewport,ticket.plan,eye,stage==rdr95::CameraMainRoute::Stage::Depth?0u:1u,ticket.qualified,ticket.nonce,ticket.generation,ticket.retailOrdinal) {}
};
