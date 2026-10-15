#pragma once
#include "CameraAdapter.h"
#include "EyeScreenState.h"
#include "EyePerspectiveState.h"
#include "vr/TaaJitter.h" // TAA FULL: sub-pixel jitter of the clones' projection only
#include <thread>
#include <memory>

namespace rdrvr::engine {
// Opaque storage, bounded by the pinned copy constructor (last store +0x46c).
// There are no owning pointers in that constructor. Do not reuse for another SHA.
struct alignas(16) ViewportStorage { std::array<std::byte,0x470> bytes{}; };
struct ViewportApi {
    void (*copy)(void*,const void*){};
    void (*camera)(void*,const runtime::Mat4&){};
    void (*projection)(void*,const runtime::Mat4&){};
    void* (*current)(const void*,bool,bool,bool){};
    void (*upload)(void*,bool){};
    bool (*ownsThread)(){};
    void (*destroy)(void*){};
    bool (*readCurrent)(const void**){};
    bool isSelected(const void* p)const{const void* value{};return readCurrent && readCurrent(&value) && value==p;}
    bool isDetached(const void* p)const{const void* value{};return readCurrent && readCurrent(&value) && value!=p;}
    explicit operator bool() const {return copy && camera && projection && current && upload && ownsThread && destroy && readCurrent;}
};
// Render-thread object. Preparing clones never selects them as the engine camera.
// Activation only uploads matrices; target/viewport/scissor binding belongs to
// the pass adapter. SetCurrent(download=true) regenerates screen/window state;
// explicit matrix upload avoids changing that state at the pass boundary.
class EyeViewports {
    ViewportApi api_{};
    std::unique_ptr<std::array<ViewportStorage,2>> eyes_;
    std::thread::id owner_{};
    bool ready_{},active_{},poisoned_{};
    unsigned constructed_{};
    const void* previous_{};
    StereoCameraPlan plan_{};
    // the projection uploaded into each clone = plan_.projection jittered by jitter_ (identical when jitter_.on is false).
    std::array<runtime::Mat4,2> projection_{};
    ::rdrvr::vr::taa::jitter::Sample jitter_{};
    std::array<EyePerspectiveState,2> perspective_{};
    std::array<EyeScreenState,2> screen_{};
    bool screenPrepared_{};
    bool owner() const {return owner_==std::this_thread::get_id();}
    bool clear() noexcept;
public:
    explicit EyeViewports(ViewportApi api):api_(api){}
    ~EyeViewports();
    EyeViewports(const EyeViewports&)=delete;
    EyeViewports& operator=(const EyeViewports&)=delete;
    bool prepare(const void* stock,const StereoCameraPlan&);
    // Opt-in only after an independent-size target plan is qualified. The
    // default game path does not call this until pixel constants are routed.
    bool prepareScreen(std::uint32_t width,std::uint32_t height) noexcept;
    bool activate(unsigned eye);
    bool matches(unsigned eye) const noexcept;
    bool restore() noexcept;
    bool ready() const {return ready_ && !poisoned_;}
    // The UN-jittered plan (every consumer except the clones' own draw matrices uses this).
    const StereoCameraPlan& plan()const{return plan_;}
    // TAA FULL: what the clones were really drawn with.
    const runtime::Mat4& drawProjection(unsigned eye)const{return projection_[eye&1u];}
    const ::rdrvr::vr::taa::jitter::Sample& jitter()const{return jitter_;}
    // Regenerated native matrices of a prepared clone: relative clip (+0xc0,
    // P times inverse camera rotation) and camera transform (+0x40). Valid
    // only after that eye has been activated at least once this frame.
    bool cameraMatrices(unsigned eye,runtime::Mat4& relativeClip,runtime::Mat4& camera) const noexcept;
    const void* viewport(unsigned eye)const{return eye<2 && ready()?&(*eyes_)[eye]:nullptr;}
};
} // namespace rdrvr::engine
