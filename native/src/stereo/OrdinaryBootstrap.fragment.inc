// Included after the existing data-only CAS backend. No executable page is
// writable, no native text is replaced, and no warm install/uninstall exists.
// The packet producer is deliberately unavailable until semantic admission is
// connected; compiling this installer is not enabling an incomplete FPS path.
constexpr bool ordinaryPacketProducerEnabled=ordinaryMainStartupRequested();
// Cold configuration/install and dispatch publication are distinct phases.
// Never arm observation before the immutable original PFNs have been bound.
std::atomic<bool> ordinaryObserverColdPrepared{};
std::atomic<unsigned> ordinaryArmPrerequisites{},ordinaryArmResult{};
std::atomic<const char*> ordinaryColdStage{"NOT_ATTEMPTED"};
void disarmOrdinaryObserver()noexcept {
    if(!ordinaryPacketProducerEnabled)return;
    const int saved=errno;
    ordinaryObserverColdPrepared.store(false,std::memory_order_release);
    indexedObservation.disarm("OrdinaryBootstrap.fragment.inc:15");ordinaryCapture.disarm();
    if(auto* transaction=rdr95::ordinary_integration::transaction.load(std::memory_order_acquire))transaction->disarm();
    errno=saved;
}
bool armOrdinaryObserverAfterDispatch()noexcept {
    if(!ordinaryPacketProducerEnabled)return true;
    const int saved=errno;
    auto* transaction=rdr95::ordinary_integration::transaction.load(std::memory_order_acquire);
    const unsigned prerequisites=unsigned(ordinaryObserverColdPrepared.load())|
        unsigned(transaction!=nullptr)<<1|unsigned(transaction&&transaction->current())<<2|
        unsigned(!patchFault.load())<<3|unsigned(cameraRoutesInstalled.load())<<4|
        unsigned(executeLifetimeReady.load())<<5|unsigned(coldLifecycleReady.load())<<6|unsigned(nativeQueryHooksReady.load())<<7;
    ordinaryArmPrerequisites.store(prerequisites,std::memory_order_release);
    const bool ready=ordinaryObserverColdPrepared.load(std::memory_order_acquire)&&transaction&&transaction->current()&&
        !patchFault.load(std::memory_order_acquire)&&cameraRoutesInstalled.load(std::memory_order_acquire)&&
        executeLifetimeReady.load(std::memory_order_acquire)&&coldLifecycleReady.load(std::memory_order_acquire)&&
        nativeQueryHooksReady.load(std::memory_order_acquire);
    const bool armed=ready&&indexedObservation.arm();
    const bool result=armed&&ordinaryObserverColdPrepared.load(std::memory_order_acquire)&&transaction->current()&&
        !patchFault.load(std::memory_order_acquire)&&cameraRoutesInstalled.load(std::memory_order_acquire)&&
        executeLifetimeReady.load(std::memory_order_acquire)&&coldLifecycleReady.load(std::memory_order_acquire)&&
        nativeQueryHooksReady.load(std::memory_order_acquire);
    ordinaryArmResult.store(result?3:armed?2:ready?1:0,std::memory_order_release);
    if(!result)disarmOrdinaryObserver();
    errno=saved;return result;
}
struct OrdinaryDispatchFailure final {
    bool committed{};
    ~OrdinaryDispatchFailure(){if(!committed)disarmOrdinaryObserver();}
};

bool installOrdinaryObserverCold() noexcept {
    namespace ov=rdr95::ordinary_vtable;
    static std::mutex cold;
    static bool attempted{};
    std::lock_guard guard(cold);
    if(attempted)return false;
    attempted=true;ordinaryColdStage.store("ENTERED");
    OrdinaryDispatchFailure failure;
    ov::ImageApi api{nullptr,
        [](void*,std::uintptr_t a,void* p,std::size_t n)noexcept{return readMemory(nullptr,a,p,n);},
        [](void*,std::uintptr_t a,std::size_t n,unsigned flags)noexcept{
            Mapping region{};Dl_info owner{};
            if(!n||n>UINTPTR_MAX-a||!mapping(a,n,region)||!dladdr(reinterpret_cast<void*>(a),&owner)||
               reinterpret_cast<std::uintptr_t>(owner.dli_fbase)!=engineBase||!owner.dli_fname||enginePath!=owner.dli_fname)return false;
            const int required=((flags&4)?PROT_READ:0)|((flags&2)?PROT_WRITE:0)|((flags&1)?PROT_EXEC:0);
            return (region.protection&required)==required&&((flags&1)||!(region.protection&PROT_EXEC));
        },
        [](void*,std::uintptr_t a)noexcept->std::uintptr_t*{
            bool allowed=false;for(auto offset:ov::slots)if(a==engineBase+offset)allowed=true;
            if(!allowed)return nullptr;
            return reinterpret_cast<std::uintptr_t*>(a);
        },
        [](void*,std::uintptr_t a,std::uintptr_t expected,std::uintptr_t desired)noexcept{
            bool allowed=false;for(auto offset:ov::slots)if(a==engineBase+offset)allowed=true;
            if(!allowed||patchFault.load(std::memory_order_acquire))return ov::PatchResult::Unchanged;
            const auto native=engineBase+ov::nativeOffset,wrapper=ov::wrapperAddress();
            if(!((expected==native&&desired==wrapper)||(expected==wrapper&&desired==native)))return ov::PatchResult::Unchanged;
            if(replacePointer(a,expected,desired))return ov::PatchResult::Committed;
            return patchFault.load(std::memory_order_acquire)?ov::PatchResult::Indeterminate:ov::PatchResult::Unchanged;
        },
        [](void*,std::uintptr_t a)noexcept{
            Mapping code{};Dl_info owner{},ours{};std::uint32_t first{};
            if(a!=ov::wrapperAddress()||(a&3)||!mapping(a,4,code)||(code.protection&(PROT_READ|PROT_EXEC))!=(PROT_READ|PROT_EXEC)||
               !dladdr(reinterpret_cast<void*>(a),&owner)||!dladdr(reinterpret_cast<void*>(&bodyDraw),&ours)||
               !owner.dli_fbase||owner.dli_fbase!=ours.dli_fbase||reinterpret_cast<std::uintptr_t>(owner.dli_fbase)==engineBase||
               !readMemory(nullptr,a,&first,sizeof first))return false;
            // BTI c/jc or PACIASP/PACIBSP are valid indirect-call landings.
            return first==0xd503245f||first==0xd50324df||first==0xd503233f||first==0xd503237f;
        }};
    // Process-lifetime allocations intentionally survive partial publication.
    auto* t=new(std::nothrow)ov::Transaction(engineBase,api);
    if(!t){ordinaryColdStage.store("TRANSACTION_ALLOCATION_FAILED");return false;}
    auto* observer=new(std::nothrow)ov::Observer(*t,rdr95::OrdinaryDepthReplay::observer());
    if(!observer){ordinaryColdStage.store("OBSERVER_ALLOCATION_FAILED");return false;}
    if(!rdr95::ordinary_integration::publish(*t)){ordinaryColdStage.store("PUBLICATION_FAILED");return false;}
    const bool configured=ordinaryCapture.configure(rdr95::OrdinaryDepthReplay::consumer());
    const bool result=configured&&t->install(*observer);
    ordinaryColdStage.store(result?"INSTALLED":configured?"INSTALL_FAILED":"CAPTURE_CONFIGURATION_FAILED");
    if(!result){t->disarm();ordinaryCapture.disarm();}
    if(t->faulted())patchFault.store(true,std::memory_order_release);
    ordinaryObserverColdPrepared.store(result,std::memory_order_release);failure.committed=result;
    return result;
}
