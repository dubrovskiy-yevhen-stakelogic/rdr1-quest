#pragma once
#include "OrdinaryDrawPins.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rdr95::ordinary_vtable {
using Address=std::uintptr_t;
inline constexpr std::array<Address,3> slots{0x2d01d28,0x2d0bef0,0x2dd5f20};
inline constexpr std::array<Address,3> addressPoints{0x2d01cf8,0x2d0bec0,0x2dd5ef0};
inline constexpr Address nativeOffset=0x1d9ac1c,callerOffset=0x2a28d50;
enum class PatchResult {Unchanged,Committed,Indeterminate};
class Observer;
struct ImageApi {
    void* context{};
    bool(*readCode)(void*,Address,void*,std::size_t)noexcept{};
    bool(*range)(void*,Address,std::size_t,unsigned elfFlags)noexcept{};
    // Cold validation returns the actual stable, aligned, readable non-executable
    // slot storage. Production must return the word at the supplied address.
    // A host image model may translate addresses. Storage must outlive wrappers.
    Address*(*resolveSlot)(void*,Address)noexcept{};
    PatchResult(*replacePointer)(void*,Address,Address expected,Address desired)noexcept{};
    bool(*wrapperEntry)(void*,Address)noexcept{};
};
// One-shot, externally serialized COLD lifecycle only. The CASes are not an
// atomic group: exposed wrappers only forward until the final ready CAS.
// Removal also requires exclusion of native callers. Never unload the Observer,
// Transaction, original module or wrapper while a fetched wrapper may execute.
class Transaction final {
public:
    using Publish=bool(*)(void*,Observer&)noexcept;
    Transaction(Address base,ImageApi)noexcept;
    Transaction(const Transaction&)=delete;
    bool install(Observer&,void* publishContext=nullptr,Publish publisher=nullptr)noexcept;
    bool remove()noexcept;
    void disarm()noexcept;
    std::uint64_t generation()noexcept;
    bool current()noexcept{return generation()!=0;}
    bool methodIsNative(Address observedAddressPoint,Address observedMethod)noexcept;
    Address original()const noexcept{return original_.load(std::memory_order_acquire);}
    Address caller()const noexcept{return base_+callerOffset;}
    Address base()const noexcept{return base_;}
    bool coveredAddressPoint(Address)const noexcept;
    bool faulted()const noexcept{return fault_.load(std::memory_order_acquire);}
    unsigned ownedMask()const noexcept{return owned_.load(std::memory_order_acquire);}
    unsigned installDiagnostic()const noexcept{return installDiagnostic_.load(std::memory_order_acquire);}
    Address diagnosticOffset()const noexcept{return diagnosticOffset_.load(std::memory_order_acquire);}
    Address diagnosticValue()const noexcept{return diagnosticValue_.load(std::memory_order_acquire);}
    bool memoryOrReplayPermission()const noexcept{return false;}
private:
    Address base_{};ImageApi api_{};std::array<Address*,slots.size()> words_{};
    std::atomic<Address> original_{};
    std::atomic<std::uint64_t> state_{2}; // odd is armed; disarm advances forever
    std::atomic<bool> attempted_{},fault_{};std::atomic<unsigned> owned_{};
    std::atomic<unsigned> installDiagnostic_{};
    std::atomic<Address> diagnosticOffset_{},diagnosticValue_{};
    bool verify()noexcept;bool slotsCurrent()const noexcept;bool rollback()noexcept;
    void fault()noexcept{fault_.store(true,std::memory_order_release);disarm();}
};
struct Body {Address container{},record{},drawable{},addressPoint{};};
struct Call {const void* drawable{};const void* world{};const void* previous{};int technique{},lod{};const void* bits{};Address caller{};};
// Integration constructs this around the ACTUAL original GameBody/DrawCommon.
// Copied identities do not prove MAIN/eye, storage lifetime, or object admission.
class BodyScope final {
    friend class Observer;
    inline static thread_local BodyScope* active_{};
    inline static thread_local std::uint64_t serial_{};
    BodyScope* previous_{};Body body_{};bool masked_{};
public:
    explicit BodyScope(Body)noexcept;
    ~BodyScope();
    BodyScope(const BodyScope&)=delete;BodyScope& operator=(const BodyScope&)=delete;
};
class Observer final {
public:
    using Original=void(*)(const void*,const void*,const void*,int,int,const void*);
    enum class ReplayResult {Declined,Emitted,Faulted};
    struct ReplayGuard {
        void* context{};
        bool(*current)(void*)noexcept{};
    };
    struct Api {
        void* context{};
        std::uint64_t(*before)(void*,const Body&,const Call&)noexcept{};
        void(*after)(void*,std::uint64_t,const Body&,const Call&,bool completed)noexcept{};
        // Optional trusted object consumer. Declined means ZERO commands were
        // emitted; Emitted/Faulted never falls back to the native body. The
        // caller must cancel the entire frame on fault, including post-return
        // loss of the exact wrapper/BodyScope. Both callbacks are mandatory.
        // This boundary itself grants no object/resource/other-eye admission.
        ReplayResult(*replay)(void*,const Body&,const Call&,ReplayGuard)noexcept{};
        void(*fault)(void*,const Body&,const Call&)noexcept{};
    };
    Observer(Transaction& t,Api api)noexcept:transaction_(t),api_(api){}
    void invoke(const Call&)const; // Native once on decline; never after any replay emission.
    Transaction& transaction()const noexcept{return transaction_;}
private:
    Transaction& transaction_;Api api_;
    inline static thread_local std::uint64_t activity_{};
    inline static thread_local unsigned depth_{};
};
// One immutable publication. Test publishers model the same before-first-CAS
// forwarding obligation; the production default is this concrete publication.
bool publish(void*,Observer&)noexcept;
Address wrapperAddress()noexcept;
}

// Six native arguments, no return-ABI guess. This ordinary compiled wrapper
// has standard compiler unwind data. It deliberately changes internal native LR.
extern "C" void rdr95_ordinary_vtable_draw(const void*,const void*,const void*,int,int,const void*);
