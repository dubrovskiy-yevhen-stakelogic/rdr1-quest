#pragma once
#include "NativeAdmissionProduction.h"
#include "NativeJoinedListOwner.h"
#include "NativeResourceGate.h"
#include "NativePhysicalCopyScope.h"
#include "NativeEarlyBootstrap.h"
#include "NativeEarlyReadRouter.h"
#include "NativeEarlyFrameController.h"
#include "NativeCurrentCamera.h"
#include "opaque-run/NativeOpaqueCameraMemo.h"
#include "opaque-run/NativeEarlyColdOwners.h"
#include "NativeOrdinaryGeometryEpoch.h"
#include <optional>

namespace rdrvr::experimental {
// Platform binding. No native call, hook installation or GPU work.
// The Game adapter caches Gfx/Vk/target at the actual pass entry BEFORE taking
// Read; callbacks below only compare that frozen current-frame state. Lifetime
// of this object/configuration must exceed the consuming controller.
class NativeEarlyRuntimeBinding {
public:
    using Controller=NativeEarlyFrameController;
    using Description=Controller::Description;
    using Pass=Controller::Mask::Pass;
    using Registry=Controller::Sources::Registry;
    using Cache=rdr95::NativeSelectedLodGeometry::Cache;
    using Address=std::uintptr_t;
    struct PassState {
        Controller::CurrentPass current{};
        Address viewport{};
    };
    struct Platform {
        void* context{};
        bool(*guarded)(void*,Address,void*,std::size_t)noexcept{};
        // O(1), no native calls/locks/clock: compare session/device epoch,
        // retained frozen pose IDs and scene selection. Stage-entry and final
        // presentation retain the existing real pose-age checks outside Read.
        // An equal address alone is not a generation.
        bool(*frozenCurrent)(void*,const Description&,Address renderer,Address sceneViewport)noexcept{};
        // Called under short Read. Returns cached CURRENT pass metadata only;
        // never calls currentGfx/TLS native accessors or any original function.
        bool(*cachedPass)(void*,const Description&,Pass,PassState&)noexcept{};
        // Optional cached-palette preparation: AFTER original MAIN Wait and
        // BEFORE the mod's Read. Acquires native pool Crit before its own short Read.
        void(*prepareJoinedRead)(void*,const Description&,Address,unsigned,Address)noexcept{};
        // Exact prepared-bank body membership, queried only under the mod's Read.
        // Null leaves all MatrixSet candidates native; StaticCommon still works.
        bool(*matrixSetCurrent)(void*,Address)noexcept{};
        // Exact installed owner/optimizer hooks + current native route.
        // Must perform no native call. Null disables compact storage reuse.
        bool(*geometryStorageCurrent)(void*,const Description&)noexcept{};
        // Actual DrawContainer/bucket/DrawCommon route, never a guessed LR.
        // Null keeps the fully fresh camera path on every currentPass.
        NativeOpaqueRun::Ticket(*opaqueRun)(void*)noexcept{};
    };
    struct Setup {
        NativeEarlyBootstrap* bootstrap{};
        rdr95::NativeResourceGate* resources{};
        rdr95::NativeJoinedListOwner* list{};
        const rdr95::EffectMutationEpoch* effects{};
        Registry* registry{};
        VkDevice device{};
        // Stable configuration identities, NOT private eye-target generation
        // or per-frame resource mutation ticket. Reconfigure after replacement.
        std::uint64_t world{},resourceGeneration{};
        rdr95::ColdMaterialVariants::Owner owner{};
        Address matrixBuffer{};
        Platform platform{};
        // PRIVATE physical-copy switch. OFF keeps the generic reader.
        bool physicalCopies{};
        // PRIVATE isolated switch; only exact DrawScope internal liveness.
        bool semanticDrawQualification{};
        // Optional copy codegen only; grants no read or lifetime authority.
        bool smallPhysicalCopies{};
        NativeOrdinaryGeometryEpoch* ordinaryGeometryEpoch{}; // Actual installed geometry content observer; mandatory for OrdinaryRead.
        NativeEarlyColdOwners* ordinaryColdOwners{}; // Actual installed owner, optional.
    };
    // Reuse the actual post-original MAIN boundary provider; no second
    // planner, palette preparation, Controller::Object or source traversal.
    NativeMainVisibilityBoundary::Api ordinaryBoundaryApi()noexcept;
    class OrdinaryRead;
    class OrdinaryEpoch final {
        friend class OrdinaryRead;
        const NativeEarlyRuntimeBinding* owner_{};
        std::uint64_t frame_{},resource_{},effect_{},shader_{},mesh_{};
        rdr95::NativeJoinedListOwner::Ticket list_{};
        NativeEarlyColdOwners::Observation cold_{};
        NativeOrdinaryGeometryEpoch::Observation geometry_{};
    public:
        // Scalar semantic cancellation only, not a read or GPU resource lease.
        // No callback, native memory load, mutex or active Read requirement.
        bool current()const noexcept;
    };
    // Exact synchronous root Read for the lower ordinary boundary. No
    // Controller::Object, record decoding, raster, camera or visibility work.
    // Covered writers stay excluded until destruction. This is NOT complete
    // graph-writer coverage or a native/GPU replay permission. Release BEFORE
    // native calls, callbacks, raw Vulkan emission, resource writers or waits.
    class OrdinaryRead final {
        friend class NativeEarlyRuntimeBinding;
        NativeEarlyRuntimeBinding* owner_{};std::uint64_t serial_{};unsigned diagnostic_{1};
        NativeOrdinaryGeometryEpoch::MetadataObservation geometry_{};
        bool live()const noexcept;
    public:
        struct Identity {
            std::uint64_t borrow{},scene{},resources{},effects{},shaders{},meshes{};
            bool operator==(const Identity&)const=default;
        };
        unsigned diagnostic()const noexcept{return diagnostic_;}
        explicit OrdinaryRead(NativeEarlyRuntimeBinding&)noexcept;
        ~OrdinaryRead();
        OrdinaryRead(const OrdinaryRead&)=delete;
        OrdinaryRead& operator=(const OrdinaryRead&)=delete;
        bool current()const noexcept;
        bool identity(Identity&)const noexcept;
        bool semanticStamp(OrdinaryEpoch&,Address vertex=0,Address index=0)const noexcept;
    };
    // Stable context across fresh LEFT/RIGHT scopes: pass Binding*, never the
    // address of a stack OrdinaryRead. Outside its exact scope these decline.
    static bool ordinaryCopy(void*,Address,void*,std::size_t)noexcept;
    static bool ordinaryIdentity(void*,OrdinaryRead::Identity&)noexcept;
    NativeEarlyRuntimeBinding()noexcept;
    NativeEarlyRuntimeBinding(const NativeEarlyRuntimeBinding&)=delete;
    ~NativeEarlyRuntimeBinding(){end();}
    // Cold only, no active frame/Read/controller operation. Exact bootstrap
    // proofs and camera function bytes are mandatory, with guarded reads.
    bool configure(const Setup&);
    const Controller::Configuration* configuration()const noexcept{return configured_?&config_:nullptr;}
    // Called before controller.begin. Supplies the real resource mutation
    // ticket, preserving scene/pose/admission as distinct caller identities.
    bool start(Description&,Address renderer,Address sceneViewport)noexcept;
    // controller.end must precede this call; Game closes list separately at
    // FinishPrimary BEFORE native color consumer returns toward Signal(+70).
    void end()noexcept;
    bool frameCurrent()const noexcept;
    bool borrowed()const noexcept;
    bool read(Address address,void* output,std::size_t bytes)const noexcept{return router_.scopedRead(address,output,bytes);}
    const rdr95::NativeAdmissionProduction& production()const noexcept{return production_;}
    struct Stats {std::uint64_t starts{},readBegins{},readDeclines{},joins{},joinDeclines{},cameraChecks{},cameraDeclines{},guardedReads{},cameraReuses{};};
    const Stats& statistics()const noexcept{return stats_;}
private:
    struct CameraKey {
        std::array<std::uint64_t,16> identity{};
        std::array<std::uintptr_t,8> colors{};std::uintptr_t depth{};
        unsigned width{},height{};
        bool operator==(const CameraKey&)const=default;
    };
    NativeOpaqueCameraMemo<CameraKey,Controller::CurrentPass> cameraMemo_;
    static bool cameraMemoCurrent(void*,const CameraKey&,const Controller::CurrentPass&)noexcept;
    Setup setup_{};NativeEarlyReadRouter router_;rdr95::NativeAdmissionProduction production_;
    Controller::Configuration config_{};NativeCurrentCamera::Api cameraApi_{};NativeCurrentCamera::CodeProof cameraProof_{};
    Description description_{};Address renderer_{},sceneViewport_{};
    std::uint64_t resourceTicket_{},effectTicket_{},shaderTicket_{},meshTicket_{};
    rdr95::NativeJoinedListOwner::Ticket listTicket_{};
    std::optional<rdr95::NativeResourceGate::Read> read_;
    rdr95::NativePhysicalCopyScope copyScope_;
    std::uint64_t readSerial_{},qualifiedSerial_{};
    const rdr95::NativeResourceGate::Read* qualifiedRead_{};
    OrdinaryRead* ordinary_{};std::uint64_t ordinaryFrame_{};
    unsigned readDepth_{};bool configured_{},started_{},joined_{};
    std::thread::id thread_{std::this_thread::get_id()};Stats stats_{};
    bool owned()const noexcept{return thread_==std::this_thread::get_id();}
    // Internal lifetime/coherence check, NOT a final frame/visibility permit.
    // Entry and publication still call strong frameCurrent()/borrowed().
    bool qualifiedReadCurrent()const noexcept;
    bool internalBorrow()const noexcept{return qualifiedReadCurrent()||borrowed();}
    bool sameDescription(const Description&)const noexcept;
    static bool guarded(void*,Address,void*,std::size_t)noexcept;
    static NativeEarlyReadRouter::CopyResult physicalCopy(void*,Address,void*,std::size_t)noexcept;
    static bool resourceBorrow(void*)noexcept;
    static bool listBorrow(void*,Address,std::size_t)noexcept;
    static bool frame(void*,const Description&)noexcept;
    static bool nativeFrame(void*,std::uint64_t)noexcept;
    static bool material(void*,rdr95::ColdMaterialVariants::Owner,rdr95::Address,rdr95::Address)noexcept;
    static bool readBegin(void*,const Description&)noexcept;
    static void readEnd(void*)noexcept;
    void advanceReadSerial()noexcept{copyScope_.reset();qualifiedSerial_=0;qualifiedRead_=nullptr;if(readSerial_!=UINT64_MAX)++readSerial_;}
    static Controller::Native::PreparationTicket preparationTicket(void*,const Description&)noexcept;
    static bool preparationCurrent(const void*,const void*,std::uint64_t)noexcept;
    static bool drawQualificationLive(const void*,const void*,std::uint64_t)noexcept;
    static bool geometryStorageCurrent(const void*,const void*,std::uint64_t)noexcept;
    static bool mainJoined(void*,const Description&,const rdr95::Ready&)noexcept;
    static bool currentPass(void*,const Description&,Pass,Controller::CurrentPass&)noexcept;
    static void prepareJoinedRead(void*,const Description&,Address,unsigned,Address)noexcept;
    static bool matrixSetCurrent(void*,rdr95::Address)noexcept;
    static bool cameraOwner(void*,const NativeCurrentCamera::Lease&)noexcept;
};
}
