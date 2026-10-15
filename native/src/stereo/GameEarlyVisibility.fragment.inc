#include "SceneRouteTape.h"
// PRIVATE include in GameXr.cpp after the real resource/viewport owners exist.
// No engine original, TLS resolver or native lock is called under a Read.
using EarlyController=experimental::NativeEarlyFrameController;
using EarlyBinding=experimental::NativeEarlyRuntimeBinding;
using EarlyPass=EarlyController::Mask::Pass;
struct EarlyGameContext {
    std::unique_ptr<EarlyBinding> binding;
    std::unique_ptr<EarlyController> controller;
    std::unique_ptr<experimental::NativeMainVisibilityBoundary> ordinaryBoundary;
    rdr95::NativeCachedPaletteOwner palettes;
    experimental::NativeEarlyImageRegistry::Cache2 imageCache{earlyImageRegistry};
    EarlyController::Description description{};
    EarlyBinding::Stats bindingBaseline{};
    const EngineEyeTargets* rightTargets{};
    EyeViewports* views{};
    std::uintptr_t renderer{},stock{},matrixBuffer{},tlsSlot{},gfx{};
    std::uint64_t deviceEpoch{},coldOwnerEpoch{};
    std::thread::id owner{std::this_thread::get_id()};
    EarlyPass pass{};
    unsigned paletteCurrentTrue{},paletteCurrentFalse{};
    bool enabled{},stage{},ordinaryMode{};
};
static thread_local std::unique_ptr<EarlyGameContext> earlyGame;
static std::atomic<std::uint64_t> earlyScenes{},earlyJoinedScenes{},earlyCompleteScenes{},earlySkippedCalls{},earlySourceCalls{};
static std::atomic<const char*> earlyState{"NOT_STARTED"};
// Publish once at scene end. No strings, timers, locks or atomic increments in
// the per-object diagnostic path; FPS-only mode retains no detailed timers.
static constexpr const char* earlyWorkNames[]={"records","candidates","cold","warm","unsupported","budget",
    "fullValidations","drawReuses","preparationReuses","prepareReads","prepareBytes","keyProbes",
    "sourceInspected","sourcePrimitives","sourcePipelineDeclined","sourceStateDeclined","sourceMatrixDeclined",
    "sourceBindingDeclined","sourcePacketDeclined","sourceCompleted","recordHandoffs",
    "readBegins","readDeclines","cameraChecks","cameraDeclines","guardedReads","cameraReuses","fingerprintValidations","finalRecordReuses","familyUnknown","familyStaticCommon","familyRigidMatrixSet","familySkinnedMatrixSet","familyMixedMatrixSet","familyNativeInstanceList","familySpecialOrCreature","preFamily","preBounds","preExclusion","ordinaryRecords","unknownDrawMethods","nonfiniteWorlds","coldAttempts","warmAttempts","duplicates","indexBudget","prepareNone","prepareArguments","prepareEntryHandoff","prepareWarmRefresh","prepareColdLease","prepareColdCapture","prepareBoundsBuild","prepareReuseBefore","prepareReuseAfter","prepareNumericUpdate","prepareComplete","geometryNone","geometryUnqualified","geometryOwner","geometryRecord","geometryModel","geometryCallback","geometryMaterial","geometryGeometry","geometryCacheMiss","geometryChanged","geometryBudget","paletteFailure","paletteEntries","paletteCurrentTrue","paletteCurrentFalse","callbackNone","callbackForceRead","callbackSorterRead","callbackCommandRead","callbackForceActive","callbackSorterActive","callbackCommandActive","callbackModelCallback","callbackGlobalMask0","callbackGlobalMask1","callbackGlobalMask2","callbackGlobalMask3","callbackGlobalMask4","callbackGlobalMask5","callbackGlobalMask6","callbackGlobalMask7","modelCallbackDataChecks","modelCallbackNone","modelCallbackSlotMissing","modelCallbackReadDeclined","modelCallbackForeign","modelCallbackNull","modelCallbackPure","modelCallbackChanged","nodeBoundsStaticNone","nodeBoundsStaticMissingNodeOrApi","nodeBoundsStaticVtableRead","nodeBoundsStaticVtableMismatch","nodeBoundsStaticNodeRead","nodeBoundsStaticVptrChanged","nodeBoundsStaticDrawableMismatch","nodeBoundsStaticWorldMismatch","nodeBoundsStaticAabbNumeric","nodeBoundsRigidNone","nodeBoundsRigidMissingNodeOrApi","nodeBoundsRigidVtableRead","nodeBoundsRigidVtableMismatch","nodeBoundsRigidNodeRead","nodeBoundsRigidVptrChanged","nodeBoundsRigidDrawableMismatch","nodeBoundsRigidWorldMismatch","nodeBoundsRigidAabbNumeric","sourceCopied","sourceSelected","sourceObjectDeclined","sourceGeometryDeclined","sourcePrimitiveDeclined","sourceChanged","patchWitnessPackets","patchUnselected","patchCacheDeclined","patchIdentityDeclined","patchesCopied","patchVerticesCopied","patchErrorDeclined","seedObserved","seedAccepted","seedUnselected","seedUnfinished","seedInvalidated","seedOverflow","seedNested","maskAdmittedStaticCommon","maskAdmittedRigidMatrixSet","maskAdmittedSkinnedMatrixSet","maskAdmittedMixedMatrixSet","maskAdmittedNativeInstanceList","maskAdmittedSpecialOrCreature","maskAdmittedUnknown","maskPermittedStaticCommon","maskPermittedRigidMatrixSet","maskPermittedSkinnedMatrixSet","maskPermittedMixedMatrixSet","maskPermittedNativeInstanceList","maskPermittedSpecialOrCreature","maskPermittedUnknown","maskHiddenStaticCommon","maskHiddenRigidMatrixSet","maskHiddenSkinnedMatrixSet","maskHiddenMixedMatrixSet","maskHiddenNativeInstanceList","maskHiddenSpecialOrCreature","maskHiddenUnknown","maskSelectedSources","maskSourcePinned","maskUnrepresented","maskOneEye","maskInvalidCandidates","maskLookupMiss","maskPermitRefused","maskInvalidations","maskDrawDeclinedLeftDepth","maskDrawDeclinedLeftColor","maskDrawDeclinedRightDepth","maskDrawDeclinedRightColor","maskPrepared","maskPoisoned","conesSources","conesAdmissionDeclined","conesInvalidSources","conesSourceEyes","conesSourceClipped","conesSourceBackfaces","conesSourceDegenerate","conesCandidates","conesInvalidCandidates","conesEyeTests","conesPlaneTests","conesOneEye","conesBothEyes","conesStale","conesBudget","conesSourceRevision","conesBuildSerial","cacheNone","cacheQueryDeclined","cacheEmptyNodes","cacheSnapshotChanged","cachePrefixIncomplete","cacheRootNumeric","rigidVtableObserved","rigidVtableUnnormalized","rigidVtableOverflow","rigidVtableRva0","rigidVtableCount0","rigidVtableRva1","rigidVtableCount1","rigidVtableRva2","rigidVtableCount2","rigidVtableRva3","rigidVtableCount3"};
static experimental::EarlyWork45Publication<std::size(earlyWorkNames)> earlyWorkPublication;

static bool earlyGuarded(void*,std::uintptr_t address,void* bytes,std::size_t count)noexcept {
    const auto& m=retailResourceApi.memory;
    return m.read&&m.read(m.context,address,bytes,count);
}
static bool earlyFrozen(void* user,const EarlyController::Description& d,std::uintptr_t renderer,std::uintptr_t stock)noexcept {
    auto& s=*static_cast<EarlyGameContext*>(user);
    // Frozen camera data does not change when wall-clock age advances. Existing
    // PrimaryEyeBoundary and trackingCurrent still check pose age at pass and
    // submission boundaries. Never lock the XR mailbox for every field read.
    if(!s.enabled||s.renderer!=renderer||s.stock!=stock||s.views!=gameEyeViewports()||!s.views||!s.views->ready()||
       !sessionRunning.load(std::memory_order_acquire)||stopWorker.load(std::memory_order_acquire)||
       !runtime::graphicsOcclusionAvailable.load(std::memory_order_acquire)||
       sessionBinding.device!=d.device||d.resources!=s.deviceEpoch||d.world!=s.renderer)return false;
    const auto& plan=s.views->plan();
    return plan.sceneId==d.frame.scene&&plan.tracking.frameId==d.frame.pose&&
        cullDrawSerial==d.frame.scene&&cullDrawRenderer==renderer&&
        d.width==s.description.width&&d.height==s.description.height;
}
static bool earlyCurrentPass(void* user,const EarlyController::Description& d,EarlyPass pass,EarlyBinding::PassState& out)noexcept {
    out={};auto& s=*static_cast<EarlyGameContext*>(user);
    if(!s.binding||!s.binding->borrowed()||!s.stage||pass!=s.pass||!s.tlsSlot||!s.gfx||
       !earlyFrozen(user,d,s.renderer,s.stock))return false;
    auto memory=retailResourceApi.memory;memory.context=&s;
    memory.read=[](void* p,std::uintptr_t a,void* b,std::size_t n){return static_cast<EarlyGameContext*>(p)->binding->read(a,b,n);};
    using Capture=experimental::NativeEarlyPassTargets;
    Capture::Context current;
    if(!Capture::context(memory,s.tlsSlot,s.gfx,current))return false;
    const bool right=pass==EarlyPass::RightDepth||pass==EarlyPass::RightColor;
    const bool depth=pass==EarlyPass::LeftDepth||pass==EarlyPass::RightDepth;
    experimental::NativeEarlyTargets::Api targetApi{&passResources,d.device,memory,d.width,d.height,&s.imageCache};
    Capture::Selection selection{right,depth,retailResourceApi.collector,retailResourceApi.postfx};
    if(right){
        const auto* images=s.rightTargets;
        if(!images||!images->ready()||images->pendingSerial()!=d.frame.scene||images->eyeCount()!=2)return false;
        const auto role=depth?EngineTargetRole::ShadowCollector:EngineTargetRole::Color;
        selection.color=reinterpret_cast<std::uintptr_t>(images->target(1,role));
        selection.z=reinterpret_cast<std::uintptr_t>(images->target(1,EngineTargetRole::Depth));
        selection.expectedColor=images->image(1,role);selection.expectedDepth=images->image(1,EngineTargetRole::Depth);
    }
    NativeOcclusionDrawWitness::Target target{};
    experimental::NativeEarlyImageRegistry::Ticket imageTicket;
    if(!Capture::targets(targetApi,selection,target,imageTicket))return false;
    out.current={current.gfx,current.vk,target,imageTicket};out.viewport=reinterpret_cast<std::uintptr_t>(s.views->viewport(right?1:0));
    return out.viewport!=0&&imageTicket&&imageTicket.current();
}
static void earlyPrepareJoinedRead(void* user,const EarlyController::Description& d,std::uintptr_t manager,unsigned request,std::uintptr_t offset)noexcept {
    auto& s=*static_cast<EarlyGameContext*>(user);s.palettes.reset();s.paletteCurrentTrue=s.paletteCurrentFalse=0;
    if(!manager||request!=rdr95::MainRequest||offset!=rdr95::MainDepthWaitReturn||!s.binding||
       !s.binding->frameCurrent()||!experimental::nativeEarlyPaletteBootstrap.ready())return;
    auto api=experimental::nativeEarlyPaletteBootstrap.configuration();api.context=&s;api.resources=&rdr95::nativeResourceGate;
    api.frameCurrent=[](void* p,std::uint64_t frame)noexcept{auto& s=*static_cast<EarlyGameContext*>(p);
        return s.description.frame.scene==frame&&s.binding&&s.binding->frameCurrent();};
    api.read=[](void* p,std::uintptr_t address,void* bytes,std::size_t n)noexcept{
        auto& s=*static_cast<EarlyGameContext*>(p);
        // This callback is exclusive to the pinned current cache headers. The
        // palette owner already holds native Crit + its own Resource Read;
        // no arbitrary graph receives this memcpy permission.
        if(!address||!bytes||!n||n>UINTPTR_MAX-address||!s.binding||!s.binding->frameCurrent()||
           !rdr95::nativeResourceGate.borrowedOnThisThread(s.description.frame.mutation))return false;
        std::memcpy(bytes,reinterpret_cast<const void*>(address),n);return true;
    };
    s.palettes.prepare(api,d.frame.scene);
}
static bool earlyMatrixSetCurrent(void* user,std::uintptr_t body)noexcept {
    auto& s=*static_cast<EarlyGameContext*>(user);const bool current=s.palettes.matrixSetCurrent(body);
    if(current)++s.paletteCurrentTrue;else ++s.paletteCurrentFalse;return current;
}
static bool earlyGeometryStorageCurrent(void* user,const EarlyController::Description& d)noexcept {
    const auto& s=*static_cast<const EarlyGameContext*>(user);
    return s.coldOwnerEpoch&&experimental::nativeEarlyColdOwners.current(s.coldOwnerEpoch)&&
        experimental::nativeEarlyBootstrap.ready()&&earlyFrozen(user,d,s.renderer,s.stock);
}
static bool earlyStage(EarlyPass pass,const EngineEyeTargets* targets=nullptr)noexcept {
    if(!earlyGame||!earlyGame->enabled)return false;
    auto& s=*earlyGame;s.stage=false;s.rightTargets=targets;s.pass=pass;
    // TLSDESC may resolve lazily and acquire bionic locks. Do it before Read.
    s.tlsSlot=experimental::nativeEarlyBootstrap.currentGfxSlot();s.gfx=0;
    if(!s.tlsSlot||!earlyGuarded(nullptr,s.tlsSlot,&s.gfx,sizeof s.gfx)||!s.gfx)return false;
    s.stage=true;rdr95::scene_routes::stage(static_cast<unsigned>(pass));return true;
}
static EarlyController* earlyController()noexcept{return earlyGame&&earlyGame->enabled&&!earlyGame->ordinaryMode?earlyGame->controller.get():nullptr;}
static experimental::NativeMainVisibilityBoundary* earlyMainBoundary()noexcept {
    if(!earlyGame||!earlyGame->enabled)return nullptr;
    auto& s=*earlyGame;
    return s.ordinaryMode?s.ordinaryBoundary.get():(s.controller?&s.controller->mainBoundary():nullptr);
}
bool gameOrdinaryReplayContext(rdr95::OrdinaryReplayContext& out)noexcept {
    // TLS state + actual selected MAIN only; this is not a read/geometry grant.
    if(!earlyGame||!earlyGame->enabled||!earlyGame->ordinaryMode||!earlyGame->stage||
       earlyGame->owner!=std::this_thread::get_id()||!rdr95::CameraMainRoute::selectedMain())return false;
    const auto& s=*earlyGame;const auto frame=rdr95::OrdinaryReplayFrame::current();
    if(!frame.current()||frame.scene()!=s.description.frame.scene||frame.pose()!=s.description.frame.pose||
       !s.binding||!s.views||!s.gfx||!s.tlsSlot)return false;
    out={s.binding.get(),s.views,experimental::nativeEarlyBootstrap.image(),s.gfx,s.tlsSlot,frame.scene(),frame.pose()};
    return true;
}
static bool earlyGood()noexcept{const auto* c=earlyController();return !c||c->good();}
static bool earlyPairGood()noexcept{
    const auto* c=earlyController();if(!c||!c->good())return c==nullptr;
    unsigned skipped{};for(const auto n:c->stats().commonSkipped)skipped+=n;
    return !skipped||c->complete();
}
static void earlyLeave()noexcept {rdr95::scene_routes::stage(4);if(auto* c=earlyController())c->leave();if(earlyGame)earlyGame->stage=false;}
static void earlyEnd()noexcept {
    rdr95::scene_routes::finish(false);
    ordinaryReplayEnd();
    if(!earlyGame)return;auto& s=*earlyGame;
    if(s.enabled&&!s.ordinaryMode&&s.controller&&s.controller->stats().prepared){
        const auto& stats=s.controller->stats();unsigned skipped{},permitted{};
        for(unsigned i=0;i<4;++i)skipped+=stats.commonSkipped[i];
        for(const auto n:stats.permitted)permitted+=n;
        const auto& cone=s.controller->conesStats();
        const auto& n=s.controller->nativeStats();const auto& w=s.controller->sources().stats();
        const auto& patch=s.controller->sources().patchStats();const auto& seed=s.controller->sources().seedStats();
        const auto& b=s.binding->statistics();const auto& before=s.bindingBaseline;
        const std::uint64_t work[]={n.records,n.candidates,n.cold,n.warm,n.unsupported,n.budget,
            n.fullValidations,n.reusedValidations,n.preparationReuses,n.reads,n.bytes,n.keyProbes,
            w.inspected,w.primitives,w.pipelineDeclined,w.stateDeclined,w.matrixDeclined,w.bindingDeclined,w.packetDeclined,w.completed,
            n.recordHandoffs,b.readBegins-before.readBegins,b.readDeclines-before.readDeclines,
            b.cameraChecks-before.cameraChecks,b.cameraDeclines-before.cameraDeclines,b.guardedReads-before.guardedReads,b.cameraReuses-before.cameraReuses,n.fingerprintValidations,n.finalRecordReuses,n.families[0],n.families[1],n.families[2],n.families[3],n.families[4],n.families[5],n.families[6],n.preDeclines[0],n.preDeclines[1],n.preDeclines[2],n.ordinaryRecords,n.unknownDrawMethods,n.nonfiniteWorlds,n.coldAttempts,n.warmAttempts,n.duplicates,n.indexBudget,n.prepareStages[0],n.prepareStages[1],n.prepareStages[2],n.prepareStages[3],n.prepareStages[4],n.prepareStages[5],n.prepareStages[6],n.prepareStages[7],n.prepareStages[8],n.prepareStages[9],n.prepareStages[10],n.geometryFailures[0],n.geometryFailures[1],n.geometryFailures[2],n.geometryFailures[3],n.geometryFailures[4],n.geometryFailures[5],n.geometryFailures[6],n.geometryFailures[7],n.geometryFailures[8],n.geometryFailures[9],n.geometryFailures[10],unsigned(s.palettes.failure()),s.palettes.entries(),s.paletteCurrentTrue,s.paletteCurrentFalse,n.callbackDeclines[0],n.callbackDeclines[1],n.callbackDeclines[2],n.callbackDeclines[3],n.callbackDeclines[4],n.callbackDeclines[5],n.callbackDeclines[6],n.callbackDeclines[7],n.callbackGlobalMasks[0],n.callbackGlobalMasks[1],n.callbackGlobalMasks[2],n.callbackGlobalMasks[3],n.callbackGlobalMasks[4],n.callbackGlobalMasks[5],n.callbackGlobalMasks[6],n.callbackGlobalMasks[7],n.modelCallbackDataChecks,n.modelCallbackResults[0],n.modelCallbackResults[1],n.modelCallbackResults[2],n.modelCallbackResults[3],n.modelCallbackResults[4],n.modelCallbackResults[5],n.modelCallbackResults[6],n.nodeBoundsDeclines[0],n.nodeBoundsDeclines[1],n.nodeBoundsDeclines[2],n.nodeBoundsDeclines[3],n.nodeBoundsDeclines[4],n.nodeBoundsDeclines[5],n.nodeBoundsDeclines[6],n.nodeBoundsDeclines[7],n.nodeBoundsDeclines[8],n.nodeBoundsDeclines[9],n.nodeBoundsDeclines[10],n.nodeBoundsDeclines[11],n.nodeBoundsDeclines[12],n.nodeBoundsDeclines[13],n.nodeBoundsDeclines[14],n.nodeBoundsDeclines[15],n.nodeBoundsDeclines[16],n.nodeBoundsDeclines[17],w.copied,w.selectedObjects,w.objectDeclined,w.geometryDeclined,w.primitiveDeclined,w.changed,patch.witnessPackets,patch.unselected,patch.cacheDeclined,patch.identityDeclined,patch.patches,patch.vertices,patch.errorDeclined,seed.observed,seed.accepted,seed.unselected,seed.unfinished,seed.invalidated,seed.overflow,seed.nested,stats.admitted[0],stats.admitted[1],stats.admitted[2],stats.admitted[3],stats.admitted[4],stats.admitted[5],stats.admitted[6],stats.permitted[0],stats.permitted[1],stats.permitted[2],stats.permitted[3],stats.permitted[4],stats.permitted[5],stats.permitted[6],stats.hidden[0],stats.hidden[1],stats.hidden[2],stats.hidden[3],stats.hidden[4],stats.hidden[5],stats.hidden[6],stats.selectedSources,stats.sourcePinned,stats.unrepresented,stats.oneEye,stats.invalidCandidates,stats.lookupMiss,stats.permitRefused,stats.invalidations,stats.drawDeclined[0],stats.drawDeclined[1],stats.drawDeclined[2],stats.drawDeclined[3],unsigned(stats.prepared),unsigned(stats.poisoned),cone.sources,cone.admissionDeclined,cone.invalidSources,cone.sourceEyes,cone.sourceClipped,cone.sourceBackfaces,cone.sourceDegenerate,cone.candidates,cone.invalidCandidates,cone.eyeTests,cone.planeTests,cone.oneEye,cone.bothEyes,cone.stale,cone.budget,s.controller->conesSourceRevision(),s.controller->conesBuildSerial(),n.cacheDeclines[0],n.cacheDeclines[1],n.cacheDeclines[2],n.cacheDeclines[3],n.cacheDeclines[4],n.cacheDeclines[5],n.rigidVtableObserved,n.rigidVtableUnnormalized,n.rigidVtableOverflow,n.rigidVtableSamples[0].rva,n.rigidVtableSamples[0].count,n.rigidVtableSamples[1].rva,n.rigidVtableSamples[1].count,n.rigidVtableSamples[2].rva,n.rigidVtableSamples[2].count,n.rigidVtableSamples[3].rva,n.rigidVtableSamples[3].count};
        static_assert(std::size(work)==std::size(earlyWorkNames));
        earlyWorkPublication.publishPrepared(stats.prepared,s.description.frame.scene,s.description.frame.pose,
            work,stats.commonSkipped,s.controller->sources().stats().completed,permitted);
        experimental::nativeEarlyTelemetry.publish({s.description.frame.scene,stats.commonSkipped,s.controller->sources().stats().completed,permitted});
        earlySkippedCalls.fetch_add(skipped,std::memory_order_relaxed);earlySourceCalls.fetch_add(s.controller->sources().stats().completed,std::memory_order_relaxed);
        if(s.controller->mainBoundary().stats().joined)++earlyJoinedScenes;
        if(s.controller->complete())++earlyCompleteScenes;
        earlyState.store(stats.poisoned?"FRAME_REVOKED":skipped?"EARLY_SKIPS":"NATIVE_RETAINED",std::memory_order_relaxed);
    }
    if(s.ordinaryBoundary)s.ordinaryBoundary->endFrame();
    if(s.controller)s.controller->end();s.palettes.reset();if(s.binding)s.binding->end();s.enabled=s.stage=false;s.rightTargets=nullptr;
}
static void earlyFinish()noexcept {experimental::main_join35::publish(earlyGame?earlyGame->description.frame.scene:0);rdr95::scene_routes::finish(earlyPairGood()&&rdr95::OrdinaryReplayFrame::good());earlyEnd();experimental::nativeEarlyListOwner.closeMain();}
static bool earlyBegin(void* renderer,void* stock)noexcept {
    earlyEnd();
    const bool ordinary=ordinaryMainStartupRequested();
    if(!ordinary&&!runtime::graphicsSettings.occlusion()){earlyState.store("OCCLUSION_OFF",std::memory_order_relaxed);return false;}
    if(!runtime::graphicsOcclusionAvailable.load(std::memory_order_acquire)){earlyState.store("HOOKS_UNAVAILABLE",std::memory_order_relaxed);return false;}
    auto& boot=experimental::nativeEarlyBootstrap;
    auto* views=gameEyeViewports();
    if(!boot.ready()||!retailResourceApi||!retailResourceApi.ownsThread()||!views||!views->ready()||
       !experimental::nativeEarlyListOwner.sceneActive(reinterpret_cast<std::uintptr_t>(renderer),reinterpret_cast<std::uintptr_t>(stock))){earlyState.store("SCENE_UNAVAILABLE",std::memory_order_relaxed);return false;}
    try {
        if(!earlyGame)earlyGame=std::make_unique<EarlyGameContext>();auto& s=*earlyGame;
        const auto epoch=coldOcclusionRegistry.statistics().deviceEpoch;
        const auto extent=nativeEyeResolution.activeExtent(renderer);const auto& plan=views->plan();
        if(!epoch||!extent.width||!extent.height||!plan.tracking.frameId)return false;
        std::uintptr_t buffer{};
        {
            rdr95::NativeResourceGate::Read read(rdr95::nativeResourceGate);if(!read)return false;
            std::int32_t id{};
            if(!earlyGuarded(nullptr,boot.image()+experimental::NativeCurrentCamera::MatrixIdsRva,&id,sizeof id)||id<=0||id>240||
               !earlyGuarded(nullptr,boot.image()+experimental::NativeCurrentCamera::GlobalBuffersRva+std::uintptr_t(id-1)*16,&buffer,sizeof buffer)||!buffer)return false;
        }
        if(!s.binding||s.ordinaryMode!=ordinary||(!ordinary&&!s.controller)||(ordinary&&!s.ordinaryBoundary)||
           s.matrixBuffer!=buffer||s.deviceEpoch!=epoch||s.renderer!=reinterpret_cast<std::uintptr_t>(renderer)){
            // A real device/global-buffer replacement invalidates all cache
            // identities. Ordinary frames and alternating eye owners do not.
            s.ordinaryBoundary.reset();s.controller.reset();s.binding=std::make_unique<EarlyBinding>();s.ordinaryMode=ordinary;
            s.matrixBuffer=buffer;s.deviceEpoch=epoch;s.renderer=reinterpret_cast<std::uintptr_t>(renderer);
            EarlyBinding::Setup setup{&boot,&rdr95::nativeResourceGate,&experimental::nativeEarlyListOwner,&effectMutationEpoch,
                &coldOcclusionRegistry,sessionBinding.device,s.renderer,epoch,{s.renderer,epoch},buffer,
                // The opaque route proves synchronous render activity only.
                // Keep camera memo disabled until the deferred-worker audit
                // also establishes exclusive access to this exact context.
                {&s,earlyGuarded,earlyFrozen,earlyCurrentPass,
                 ordinary?nullptr:earlyPrepareJoinedRead,ordinary?nullptr:earlyMatrixSetCurrent,earlyGeometryStorageCurrent,nullptr}};
            // One physical lifetime check for this exact short Read. Covered
            // writers drain it; semantic epochs remain checked at skip commit.
            setup.physicalCopies=true;
            setup.smallPhysicalCopies=true;
            setup.semanticDrawQualification=!ordinary;
            setup.ordinaryColdOwners=ordinary?&experimental::nativeEarlyColdOwners:nullptr;
            setup.ordinaryGeometryEpoch=ordinary?&experimental::nativeOrdinaryGeometryEpoch:nullptr;
            if(!s.binding->configure(setup)){s.binding.reset();earlyState.store("BINDING_UNAVAILABLE",std::memory_order_relaxed);return false;}
            if(ordinary){
                const auto* memory=s.binding->production().memory();
                if(!memory){s.binding.reset();return false;}
                s.ordinaryBoundary=std::make_unique<experimental::NativeMainVisibilityBoundary>(*memory);
            }else{
                auto configuration=*s.binding->configuration();
                configuration.earlyDrawScope=true;
                s.controller=std::make_unique<EarlyController>(configuration);
            }
        }
        s.stock=reinterpret_cast<std::uintptr_t>(stock);s.views=views;s.description={};s.coldOwnerEpoch=experimental::nativeEarlyColdOwners.generation();
        auto& d=s.description;d.frame={plan.sceneId,plan.tracking.frameId,plan.sceneId,0};
        d.world=s.renderer;d.resources=epoch;d.device=sessionBinding.device;d.width=extent.width;d.height=extent.height;
        for(unsigned eye=0;eye<2;++eye){const auto* bytes=static_cast<const std::byte*>(views->viewport(eye));
            if(!bytes)return false;std::memcpy(d.cameras[eye].relativeClip.data(),bytes+0xc0,64);std::memcpy(d.cameras[eye].inverseView.data(),bytes+0x140,64);}
        s.bindingBaseline=s.binding->statistics();s.enabled=true;
        // Resolve TLS/cache actual Gfx BEFORE Binding::start takes any Read.
        const bool started=earlyStage(EarlyPass::LeftDepth)&&s.binding->start(d,s.renderer,s.stock);
        const bool begun=started&&(ordinary?
            s.ordinaryBoundary->beginFrame(d.frame.scene,s.binding->ordinaryBoundaryApi()):s.controller->begin(d));
        if(!begun||(ordinary&&!ordinaryReplayBegin())){
            earlyEnd();earlyState.store("FRAME_UNAVAILABLE",std::memory_order_relaxed);return false;
        }
        ++earlyScenes;earlyState.store("WAITING_FOR_MAIN",std::memory_order_relaxed);return true;
    }catch(...){earlyEnd();earlyState.store("SETUP_DECLINED",std::memory_order_relaxed);return false;}
}
