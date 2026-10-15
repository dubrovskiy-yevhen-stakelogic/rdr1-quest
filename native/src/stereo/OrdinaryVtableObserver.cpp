#include "OrdinaryVtableObserver.h"
#include "OrdinaryGateProbe.h"
#include "SceneRouteTape.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#if defined(_MSC_VER)
#include <intrin.h>
#define NOINLINE __declspec(noinline)
#define HIDDEN
#else
#define NOINLINE __attribute__((noinline))
#define HIDDEN __attribute__((visibility("hidden")))
#endif

namespace rdr95::ordinary_vtable {
namespace {
struct Errno {int saved=errno;~Errno(){errno=saved;}};
std::atomic<Observer*> installed{};
bool baseValid(Address base)noexcept{return base&&!(base&7u)&&base<=UINTPTR_MAX-(slots.back()+sizeof(Address));}
Address slotLoad(Address* word)noexcept {
#if defined(__GNUC__)
    // NDK r27 libc++ has no std::atomic_ref. The same aligned native word is
    // loaded by the compiler atomic intrinsic, without constructing an object.
    static_assert(__atomic_always_lock_free(sizeof(Address),nullptr));
    return __atomic_load_n(word,__ATOMIC_ACQUIRE);
#else
    return std::atomic_ref<Address>(*word).load(std::memory_order_acquire);
#endif
}
}
static_assert(sizeof(Address)==8&&std::atomic<Address>::is_always_lock_free);
Address wrapperAddress()noexcept{return reinterpret_cast<Address>(&rdr95_ordinary_vtable_draw);}
Transaction::Transaction(Address base,ImageApi api)noexcept:base_(base),api_(api){}
bool Transaction::verify()noexcept {
    namespace pins=ordinary_call::pins;
    std::array<std::uint32_t,16> buffer{};
    for(const auto& pin:pins::all){
        diagnosticOffset_.store(pin.offset,std::memory_order_release);
        if(!api_.range(api_.context,base_+pin.offset,pin.count*4,5)){installDiagnostic_.store(11);return false;}
        for(std::size_t off=0;off<pin.count;){const auto n=std::min(buffer.size(),pin.count-off);
            diagnosticOffset_.store(pin.offset+4*off,std::memory_order_release);
            if(!api_.readCode(api_.context,base_+pin.offset+4*off,buffer.data(),4*n)){installDiagnostic_.store(12);return false;}
            if(std::memcmp(buffer.data(),pin.words+off,4*n)){installDiagnostic_.store(13);return false;}
            off+=n;
        }
    }
    for(unsigned i=0;i<slots.size();++i){const auto address=base_+slots[i];
        diagnosticOffset_.store(slots[i],std::memory_order_release);
        if(!api_.range(api_.context,address,8,4)){installDiagnostic_.store(14);return false;}
        auto* word=api_.resolveSlot(api_.context,address);
        if(!word||(reinterpret_cast<Address>(word)%alignof(Address))){installDiagnostic_.store(15);return false;}
        words_[i]=word;
        const auto value=slotLoad(word);diagnosticValue_.store(value>=base_?value-base_:value,std::memory_order_release);
        if(value!=base_+nativeOffset){installDiagnostic_.store(16);return false;}
    }
    for(unsigned i=0;i<words_.size();++i)for(unsigned j=0;j<i;++j)if(words_[i]==words_[j])return false;
    return true;
}
bool Transaction::slotsCurrent()const noexcept {
    for(auto* word:words_)if(!word||slotLoad(word)!=wrapperAddress())return false;
    return true;
}
void Transaction::disarm()noexcept {
    Errno keep;auto old=state_.load(std::memory_order_relaxed);
    while(old){const auto next=old>UINT64_MAX-2?0:((old&~std::uint64_t(1))+2);
        if(state_.compare_exchange_weak(old,next,std::memory_order_acq_rel))break;}
}
std::uint64_t Transaction::generation()noexcept {
    Errno keep;const auto before=state_.load(std::memory_order_acquire);
    if(!(before&1u)||faulted())return 0;
    if(!slotsCurrent()){fault();return 0;}
    return state_.load(std::memory_order_acquire)==before?before:0;
}
bool Transaction::rollback()noexcept {
    bool ok=true;
    for(unsigned i=unsigned(slots.size());i--;){const auto bit=1u<<i;
        if(!(owned_.load(std::memory_order_acquire)&bit))continue;
        const auto result=api_.replacePointer(api_.context,base_+slots[i],wrapperAddress(),original());
        if(result==PatchResult::Committed)owned_.fetch_and(~bit,std::memory_order_release);
        else ok=false;
    }
    if(!ok)fault();return ok;
}
bool Transaction::install(Observer& observer,void* context,Publish publisher)noexcept {
    Errno keep;
    if(attempted_.exchange(true,std::memory_order_acq_rel)||&observer.transaction()!=this||!baseValid(base_)||
       !api_.readCode||!api_.range||!api_.resolveSlot||!api_.replacePointer||!api_.wrapperEntry)return false;
    auto expected=state_.load(std::memory_order_acquire);
    installDiagnostic_.store(1,std::memory_order_release);
    if(!expected||(expected&1u)||faulted()){installDiagnostic_.store(2);return false;}
    if(!api_.wrapperEntry(api_.context,wrapperAddress())){installDiagnostic_.store(3);return false;}
    if(!verify())return false;
    installDiagnostic_.store(20,std::memory_order_release);
    original_.store(base_+nativeOffset,std::memory_order_release);
    // All three slots have the same exact original. Publication is immutable and
    // survives partial failure/removal, even if no slot remains owned.
    if(!(publisher?publisher:publish)(context,observer)){installDiagnostic_.store(21);return false;}
    for(unsigned i=0;i<slots.size();++i){
        installDiagnostic_.store(30+i,std::memory_order_release);
        const auto result=api_.replacePointer(api_.context,base_+slots[i],original(),wrapperAddress());
        if(result!=PatchResult::Unchanged)owned_.fetch_or(1u<<i,std::memory_order_release);
        if(result!=PatchResult::Committed){if(result==PatchResult::Indeterminate)fault();else disarm();rollback();return false;}
    }
    if(!slotsCurrent()||faulted()||!state_.compare_exchange_strong(expected,expected|1u,std::memory_order_acq_rel)){
        disarm();rollback();return false;
    }
    installDiagnostic_.store(100,std::memory_order_release);return true;
}
bool Transaction::remove()noexcept {Errno keep;disarm();return rollback();}
bool Transaction::coveredAddressPoint(Address point)const noexcept {
    if(!baseValid(base_))return false;
    for(auto offset:addressPoints)if(point==base_+offset)return true;
    return false;
}
bool Transaction::methodIsNative(Address point,Address method)noexcept {
    Errno keep;if(!baseValid(base_))return false;
    // Preserve the original strict check for unhooked tables, including unsupported derived tables;
    // this does NOT qualify them for the observer or for sector admission.
    if(method==base_+nativeOffset)return true;
    return method==wrapperAddress()&&coveredAddressPoint(point)&&original()==base_+nativeOffset&&current();
}
BodyScope::BodyScope(Body body)noexcept:previous_(active_),body_(body){
    if(serial_!=UINT64_MAX)++serial_;
    if(previous_){previous_->masked_=true;masked_=true;}
    active_=this;
}
BodyScope::~BodyScope(){if(serial_!=UINT64_MAX)++serial_;active_=previous_;}
bool publish(void*,Observer& observer)noexcept {
    if(!observer.transaction().original())return false;
    Observer* expected=nullptr;
    return installed.compare_exchange_strong(expected,&observer,std::memory_order_release,std::memory_order_acquire)||expected==&observer;
}
void Observer::invoke(const Call& call)const {
    const int incoming=errno;
    const auto original=reinterpret_cast<Original>(transaction_.original());
    if(!original)std::terminate(); // Cannot be reached by a legitimately installed slot.
    const auto depth=depth_;
    if(depth==UINT32_MAX)std::terminate();
    ++depth_;if(activity_!=UINT64_MAX)++activity_;
    struct Exit {unsigned& d;~Exit(){--d;}} exit{depth_};
    auto* source=BodyScope::active_;const auto bodySerial=BodyScope::serial_,activity=activity_;
    const auto generation=transaction_.generation();
    const auto valid=[&]()noexcept {
        return generation&&transaction_.generation()==generation&&depth_==1&&activity!=UINT64_MAX&&activity_==activity&&
            call.caller==transaction_.caller()&&source&&source==BodyScope::active_&&bodySerial!=UINT64_MAX&&BodyScope::serial_==bodySerial&&!source->masked_&&
            source->body_.container&&source->body_.record&&source->body_.record<=UINTPTR_MAX-0x40&&
            transaction_.coveredAddressPoint(source->body_.addressPoint)&&
            source->body_.drawable==reinterpret_cast<Address>(call.drawable)&&source->body_.record==reinterpret_cast<Address>(call.world)&&
            source->body_.record+0x40==reinterpret_cast<Address>(call.previous);
    };
    if(rdr95::scene_routes::active)rdr95::scene_routes::observer(reinterpret_cast<Address>(call.drawable),reinterpret_cast<Address>(call.world),reinterpret_cast<Address>(call.previous),call.caller,valid());
    if(gateprobe::enabled){
        using namespace gateprobe;
        hit(Invoke);sample(1,call.caller,transaction_.base());
        if(generation)hit(InvokeGeneration);
        if(call.caller==transaction_.caller())hit(InvokeCaller);
        if(source){
            hit(InvokeSource);if(!source->masked_)hit(InvokeUnmasked);
            if(source->body_.container)hit(InvokeBody);
            if(source->body_.drawable==reinterpret_cast<Address>(call.drawable))hit(InvokeDrawable);
            if(source->body_.record==reinterpret_cast<Address>(call.world))hit(InvokeWorld);
            if(source->body_.record+0x40==reinterpret_cast<Address>(call.previous))hit(InvokePrevious);
        }
        if(valid())hit(InvokeValid);
    }
    if(api_.replay&&api_.fault&&valid()){
        const Body body=source->body_;
        struct Check {
            const decltype(valid)& predicate;
            static bool current(void* value)noexcept {
                const int saved=errno;
                const bool result=static_cast<Check*>(value)->predicate();
                errno=saved;return result;
            }
        } check{valid};
        const auto outcome=api_.replay(api_.context,body,call,{&check,Check::current});
        if(outcome!=ReplayResult::Declined){
            if(outcome!=ReplayResult::Emitted||!valid())api_.fault(api_.context,body,call);
            errno=incoming;return;
        }
    }
    Body body{};std::uint64_t cookie{};bool completed=false;
    if(api_.before&&api_.after&&valid()){body=source->body_;cookie=api_.before(api_.context,body,call);}
    struct Finish {
        const Api& api;const Body& body;const Call& call;std::uint64_t cookie;bool& completed;const decltype(valid)& current;
        ~Finish(){if(cookie){const int saved=errno;api.after(api.context,cookie,body,call,completed&&current());errno=saved;}}
    } finish{api_,body,call,cookie,completed,valid};
    errno=incoming;original(call.drawable,call.world,call.previous,call.technique,call.lod,call.bits);completed=true;
}
static void dispatch(const Call& call){
    auto* observer=installed.load(std::memory_order_acquire);
    if(!observer)std::terminate(); // The slot transaction publishes before CAS.
    observer->invoke(call);
}
}
extern "C" HIDDEN NOINLINE void rdr95_ordinary_vtable_draw(const void* drawable,const void* world,const void* previous,int technique,int lod,const void* bits){
#if defined(_MSC_VER)
    const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#else
    const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_extract_return_addr(__builtin_return_address(0)));
#endif
    rdr95::ordinary_vtable::dispatch({drawable,world,previous,technique,lod,bits,caller});
}
