#pragma once
#include "CameraAdapter.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace rdrvr::experimental {
// Diagnostic only. The complete original ModelDraw always runs. Native input
// pointers are borrowed only inside its exact caller and selected eye lifetime.
class NativeModelProbe69 {
public:
    using Test=int(*)(const void*);
    enum Counter : unsigned { Models, Single, Multi, ForeignViewport, Recording,
        CustomCallback, NullCallback, NullBounds, Qualified, InvalidBounds,
        InvalidMatrix, NativeOutside, NativePartial, NativeInside,
        PrivateSidesOutside, PrivateNearOutside, AdditionalSideOutside,
        AdditionalNearOutside, PrivateRetainedNativeOutside, Budget, Count };
    static constexpr unsigned sampleCapacity=16, maxModels=8192;
    inline static constexpr const char* names[Count]={"models","single","multi","foreignViewport","recording",
        "customCallback","nullCallback","nullBounds","qualified","invalidBounds",
        "invalidMatrix","nativeOutside","nativePartial","nativeInside",
        "privateSidesOutside","privateNearOutside","additionalSideOutside",
        "additionalNearOutside","privateRetainedNativeOutside","budget"};
    struct Sample {
        std::array<float,8> bounds{};
        std::array<float,16> world{},nativePlanes{};
        int nativeResult{};unsigned privateSides{},privateNear{},caller{};
    };
    struct Receipt {
        std::uint64_t scene{},trackingFrame{},nonce{},generation{},retailOrdinal{},eye{},stage{},sampleCount{};
        std::array<std::uint64_t,Count> counts{};
        std::array<double,16> clipWorld{};
        std::array<Sample,sampleCapacity> samples{};
    };
private:
    struct State {
        NativeModelProbe69* owner{};const void* viewport{};bool enabled{};
        Receipt receipt{};
    };
    inline static thread_local State* active_{};
    // Enabled Eyes alive on any thread. model() returns before touching the thread_local active_
    // (dynamic TLS descriptor) while it is 0, which is every model draw outside a debug.rdrvr.render_ablation mode-12 lease.
    inline static std::atomic<unsigned> liveEyes_{};
    struct Published {
        std::atomic<std::uint64_t> sequence{};
        std::array<std::atomic<std::uint64_t>,(sizeof(Receipt)+7)/8> words{};
    };
    std::array<Published,4> published_{};
    std::uintptr_t current_{},callback_{},commands_{},expectedCallback_{};
    Test test_{};
    struct Error {int value=errno;~Error(){errno=value;}};
    static std::uintptr_t pointer(std::uintptr_t address)noexcept {
        std::uintptr_t out{};std::memcpy(&out,reinterpret_cast<const void*>(address),sizeof(out));return out;
    }
    static bool clip(const engine::StereoCameraPlan& plan,unsigned eye,std::array<double,16>& out)noexcept {
        if(eye>1||!plan.sceneId||!plan.tracking.frameId)return false;
        const auto& p=plan.projection[eye];const auto& v=plan.view[eye];
        for(float x:p)if(!std::isfinite(x))return false;
        for(float x:v)if(!std::isfinite(x))return false;
        if(p[11]!=-1||p[0]<=0||std::abs(p[5])<0.0001||v[3]!=0||v[7]!=0||v[11]!=0||v[15]!=1)return false;
        out={};for(unsigned col=0;col<4;++col)for(unsigned row=0;row<4;++row)
            for(unsigned k=0;k<4;++k)out[col*4+row]+=double(p[k*4+row])*v[col*4+k];
        return true;
    }
    void publish(const Receipt& value)noexcept {
        auto& slot=published_[unsigned(value.eye)*2+unsigned(value.stage)];
        auto seq=slot.sequence.load(std::memory_order_relaxed);
        if(seq&1||seq>UINT64_MAX-2||!slot.sequence.compare_exchange_strong(seq,seq+1,std::memory_order_acq_rel))return;
        std::array<std::byte,((sizeof(Receipt)+7)/8)*8> bytes{};
        std::memcpy(bytes.data(),&value,sizeof(value));
        for(unsigned i=0;i<slot.words.size();++i){std::uint64_t word{};
            std::memcpy(&word,bytes.data()+i*8,8);slot.words[i].store(word,std::memory_order_relaxed);}
        slot.sequence.store(seq+2,std::memory_order_release);
    }
    static std::string hex(const void* data,std::size_t size) {
        static constexpr char digits[]="0123456789abcdef";const auto* bytes=static_cast<const unsigned char*>(data);
        std::string out(size*2,'0');for(std::size_t i=0;i<size;++i){out[2*i]=digits[bytes[i]>>4];out[2*i+1]=digits[bytes[i]&15];}return out;
    }
public:
    void configure(std::uintptr_t current,std::uintptr_t callback,std::uintptr_t commands,std::uintptr_t expected,Test test)noexcept {
        current_=current;callback_=callback;commands_=commands;expectedCallback_=expected;test_=test;
    }
    class Eye {
        State state_{};State* previous_{};
    public:
        Eye(NativeModelProbe69& owner,const void* viewport,const engine::StereoCameraPlan* plan,
            unsigned eye,unsigned stage,bool eligible,std::uint64_t nonce,std::uint64_t generation,std::uint64_t retailOrdinal=0)noexcept:previous_(active_) {
            Error preserve;state_.owner=&owner;state_.viewport=viewport;
            auto& r=state_.receipt;r.eye=eye;r.stage=stage;r.nonce=nonce;r.generation=generation;r.retailOrdinal=retailOrdinal;
            state_.enabled=eligible&&!previous_&&viewport&&plan&&eye<2&&stage<2&&nonce&&generation&&
                owner.current_&&owner.callback_&&owner.commands_&&owner.test_&&clip(*plan,eye,r.clipWorld);
            if(state_.enabled){r.scene=plan->sceneId;r.trackingFrame=plan->tracking.frameId;liveEyes_.fetch_add(1,std::memory_order_relaxed);}
            active_=&state_;
        }
        ~Eye(){Error preserve;active_=previous_;if(state_.enabled){state_.owner->publish(state_.receipt);liveEyes_.fetch_sub(1,std::memory_order_relaxed);}}
        Eye(const Eye&)=delete;Eye& operator=(const Eye&)=delete;
    };
    static unsigned liveEyes()noexcept {return liveEyes_.load(std::memory_order_relaxed);}
    void model(const void* model,unsigned caller,bool exact)noexcept {
        if(!liveEyes_.load(std::memory_order_relaxed))return; // No enabled Eye anywhere
        auto* s=active_;if(!s||s->owner!=this||!s->enabled||!exact)return;
        Error preserve;auto& r=s->receipt;auto& n=r.counts;
        ++n[Models];++n[caller==0?Single:Multi];
        if(n[Models]>maxModels){++n[Budget];return;}
        const auto current=pointer(current_),callback=pointer(callback_),commands=pointer(commands_);
        if(current!=reinterpret_cast<std::uintptr_t>(s->viewport)){++n[ForeignViewport];return;}
        if(commands){++n[Recording];return;}
        if(!callback){++n[NullCallback];return;}
        if(callback!=expectedCallback_){++n[CustomCallback];return;}
        // This exact native body calls the verified default callback with
        // model+18 before shader preparation. Its 32-byte input is now live.
        const auto address=pointer(reinterpret_cast<std::uintptr_t>(model)+0x18);
        if(!address){++n[NullBounds];return;}
        Sample sample{};sample.caller=caller;
        std::memcpy(sample.bounds.data(),reinterpret_cast<const void*>(address),32);
        std::memcpy(sample.world.data(),reinterpret_cast<const std::byte*>(s->viewport)+0x80,64);
        std::memcpy(sample.nativePlanes.data(),reinterpret_cast<const std::byte*>(s->viewport)+0x2c0,64);
        for(unsigned k=0;k<3;++k)if(!std::isfinite(sample.bounds[k])||!std::isfinite(sample.bounds[k+4])||
            sample.bounds[k]>sample.bounds[k+4]){++n[InvalidBounds];return;}
        for(float x:sample.world)if(!std::isfinite(x)){++n[InvalidMatrix];return;}
        if(sample.world[3]!=0||sample.world[7]!=0||sample.world[11]!=0||sample.world[15]!=1){++n[InvalidMatrix];return;}
        ++n[Qualified];sample.nativeResult=test_(sample.bounds.data());
        if(sample.nativeResult==0)++n[NativeOutside];else if(sample.nativeResult==1)++n[NativePartial];
        else if(sample.nativeResult==2)++n[NativeInside];else {++n[InvalidMatrix];return;}
        std::array<double,16> matrix{};
        for(unsigned c=0;c<4;++c)for(unsigned row=0;row<4;++row)for(unsigned k=0;k<4;++k)
            matrix[c*4+row]+=r.clipWorld[k*4+row]*sample.world[c*4+k];
        for(unsigned plane=0;plane<5;++plane){
            double upper=0,magnitude=1;
            for(unsigned k=0;k<4;++k){
                const double q=plane<4?matrix[k*4+3]+(plane&1?-1:1)*matrix[k*4+plane/2]:matrix[k*4+2];
                const double x=k==3?1:sample.bounds[k+(q>=0?4:0)];
                upper+=q*x;magnitude+=std::abs(q*x);
            }
            // Ignore numerical boundary disagreements; this is an opportunity
            // estimate, never authority to omit a native model or geometry.
            if(std::isfinite(upper)&&upper<-magnitude*0.00002){
                if(plane<4)sample.privateSides|=1u<<plane;else sample.privateNear=1;
            }
        }
        if(sample.privateSides)++n[PrivateSidesOutside];if(sample.privateNear)++n[PrivateNearOutside];
        if(sample.nativeResult&&sample.privateSides)++n[AdditionalSideOutside];
        if(sample.nativeResult&&sample.privateNear)++n[AdditionalNearOutside];
        if(!sample.privateSides&&sample.nativeResult==0)++n[PrivateRetainedNativeOutside];
        // Keep initial examples and examples that would add a rejection. No
        // source pointer escapes; only owned numerical fields are published.
        if(r.sampleCount<sampleCapacity&&(r.sampleCount<8||(sample.nativeResult&&(sample.privateSides||sample.privateNear))))
            r.samples[r.sampleCount++]=sample;
    }
    bool snapshot(unsigned index,Receipt& out,std::uint64_t& serial)const noexcept {
        if(index>=published_.size())return false;const auto& slot=published_[index];
        for(unsigned attempt=0;attempt<3;++attempt){const auto first=slot.sequence.load(std::memory_order_acquire);
            if(!first||(first&1))continue;
            std::array<std::byte,((sizeof(Receipt)+7)/8)*8> bytes{};
            for(unsigned i=0;i<slot.words.size();++i){const auto word=slot.words[i].load(std::memory_order_relaxed);std::memcpy(bytes.data()+i*8,&word,8);}
            std::atomic_thread_fence(std::memory_order_acquire);
            if(slot.sequence.load(std::memory_order_acquire)!=first)continue;
            std::memcpy(&out,bytes.data(),sizeof(out));serial=first/2;return true;
        }return false;
    }
    std::string report()const {
        Error preserve;std::string out="{\"readOnly\":true,\"receipts\":[";
        for(unsigned i=0;i<4;++i){if(i)out+=',';Receipt r{};std::uint64_t serial{};const bool stable=snapshot(i,r,serial);
            out+="{\"stable\":"+std::string(stable?"true":"false")+",\"serial\":"+std::to_string(serial);
            if(stable){out+=",\"scene\":"+std::to_string(r.scene)+",\"trackingFrame\":"+std::to_string(r.trackingFrame)+
                ",\"nonce\":"+std::to_string(r.nonce)+",\"generation\":"+std::to_string(r.generation)+",\"retailOrdinal\":"+std::to_string(r.retailOrdinal)+
                ",\"eye\":"+std::to_string(r.eye)+",\"stage\":"+std::to_string(r.stage)+",\"counts\":{";
                for(unsigned k=0;k<Count;++k){if(k)out+=',';out+='"'+std::string(names[k])+"\":"+std::to_string(r.counts[k]);}
                out+="},\"clipWorldHex\":\""+hex(r.clipWorld.data(),sizeof(r.clipWorld))+"\",\"samples\":[";
                const auto count=std::min<std::uint64_t>(r.sampleCount,sampleCapacity);
                for(unsigned k=0;k<count;++k){if(k)out+=',';const auto& a=r.samples[k];
                    out+="{\"boundsHex\":\""+hex(a.bounds.data(),32)+"\",\"worldHex\":\""+hex(a.world.data(),64)+
                        "\",\"nativePlanesHex\":\""+hex(a.nativePlanes.data(),64)+"\",\"nativeResult\":"+std::to_string(a.nativeResult)+
                        ",\"privateSides\":"+std::to_string(a.privateSides)+",\"privateNear\":"+std::to_string(a.privateNear)+
                        ",\"caller\":"+std::to_string(a.caller)+"}";}
                out+=']';}
            out+='}';}
        return out+"] ,\"fpsClaim\":null}";
    }
};
static_assert(std::is_trivially_copyable_v<NativeModelProbe69::Receipt>);
inline NativeModelProbe69 nativeModelProbe69;
inline std::atomic<bool> nativeModelProbeReady69{};
}
