#pragma once
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>
#endif

namespace rdrvr::experimental {
// Diagnostic render removal only. Every command names this process and expires.
// Sample the property at world entry; draw hooks read only the cached request.
class RenderAblation54 {
    struct State {
        RenderAblation54* owner{};
        std::uint64_t nonce{},generation{},deadline{},scene{},sampleMs{};
        std::uint64_t matchedBody{},suppressedBody{},forwardedBody{};
        std::uint64_t matchedModel{},suppressedModel{},forwardedModel{};
        std::uint64_t worldFrames{},worldCpuNs{},worldWallNs{},mixedFrames{},clockErrors{};
        unsigned pid{},requested{},mode{},reason{};
        unsigned keyPid{},keyMode{},keyTtl{};std::uint64_t keyNonce{};
        bool haveKey{},worldActive{},selectedPass{};unsigned passDepth{};
        unsigned idleScenes{}; // Scenes since the last property sample while nothing is armed
    };
    static thread_local State local_;
    std::atomic<bool> modelHook_{};
    std::atomic<std::uint64_t> propertySkips_{}; // Property reads skipped while idle (report hygiene)
    // True while any thread holds a valid request or an applied mode (set by frame()). False =>
    // body()/model() return false and vertexMergeAllowed() true without touching local_.
    std::atomic<bool> armed_{};
    std::atomic<std::uint64_t> sequence_{};
    std::array<std::atomic<std::uint64_t>,22> published_{};
    struct Errno {int incoming=errno;~Errno(){errno=incoming;}};
    static bool parse(const char* request,std::array<std::uint64_t,4>& values)noexcept {
        if(!request||!*request)return false;
        constexpr std::array<std::uint64_t,4> limits{12,UINT32_MAX,UINT64_MAX,10000};
        unsigned field{},length{};bool digit=false;
        for(const char* p=request;*p;++p){
            if(++length>91)return false;
            if(*p==':'){if(!digit||field==3)return false;++field;digit=false;continue;}
            if(*p<'0'||*p>'9')return false;
            const auto next=unsigned(*p-'0');
            if(values[field]>limits[field]/10||(values[field]==limits[field]/10&&next>limits[field]%10))return false;
            values[field]=values[field]*10+next;digit=true;
        }
        return field==3&&digit&&values[0]!=5&&values[0]!=7&&values[0]!=8&&values[0]!=9&&values[0]!=11;
    }
    static std::uint64_t now()noexcept {
#ifdef __ANDROID__
        timespec t{};if(clock_gettime(CLOCK_BOOTTIME,&t))return 0;
        return std::uint64_t(t.tv_sec)*1000+std::uint64_t(t.tv_nsec)/1000000;
#else
        return 0;
#endif
    }
    static std::uint64_t cpuNs()noexcept {
#ifdef __ANDROID__
        timespec t{};if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t))return 0;
        return std::uint64_t(t.tv_sec)*1000000000+std::uint64_t(t.tv_nsec);
#else
        return 0;
#endif
    }
    static std::uint64_t wallNs()noexcept {
#ifdef __ANDROID__
        timespec t{};if(clock_gettime(CLOCK_BOOTTIME,&t))return 0;
        return std::uint64_t(t.tv_sec)*1000000000+std::uint64_t(t.tv_nsec);
#else
        return 0;
#endif
    }
    void publish(State& s)noexcept {
        auto before=sequence_.load(std::memory_order_relaxed);
        if((before&1)||before>=UINT64_MAX-2||!sequence_.compare_exchange_strong(before,before+1,std::memory_order_acq_rel))return;
        const auto remaining=s.deadline>s.sampleMs?s.deadline-s.sampleMs:0;
        const std::array<std::uint64_t,22> values{s.pid,s.nonce,s.generation,s.requested,s.mode,s.reason,s.scene,s.sampleMs,
            remaining,s.matchedBody,s.suppressedBody,s.forwardedBody,s.matchedModel,s.suppressedModel,s.forwardedModel,
            unsigned(modelHook_.load(std::memory_order_acquire)),s.deadline,s.worldFrames,s.worldCpuNs,s.worldWallNs,s.mixedFrames,s.clockErrors};
        for(unsigned i=0;i<values.size();++i)published_[i].store(values[i],std::memory_order_release);
        sequence_.store(before+2,std::memory_order_release);
    }
    void expire(State& s,std::uint64_t ms)noexcept {
        if(s.deadline&&ms>=s.deadline){s.mode=0;s.reason=2;s.deadline=0;s.sampleMs=ms;publish(s);}
    }
    bool draw(bool model,bool exact,std::uint64_t ms)noexcept {
        auto& s=local_;if(s.owner!=this||!s.pid)return false;
        const bool whole=s.mode==(model?4u:3u);
        if(!exact&&!whole)return false;
        // The active ablation has an additional deadline check even if a world
        // call stalls. Control mode never pays a clock call at the draw boundary.
        if(s.mode&&s.mode<=4){
            if(ms)expire(s,ms);
            else {s.mode=0;s.reason=4;s.deadline=0;++s.clockErrors;publish(s);}
        }
        auto& matched=model?s.matchedModel:s.matchedBody;
        auto& suppressed=model?s.suppressedModel:s.suppressedBody;
        auto& forwarded=model?s.forwardedModel:s.forwardedBody;
        ++matched;const bool skip=s.mode==(model?2u:1u)||s.mode==(model?4u:3u);
        if(skip)++suppressed;else ++forwarded;return skip;
    }
public:
    class EyePass {
        RenderAblation54* owner_{};bool previous_{};
    public:
        EyePass(RenderAblation54& owner,bool selected,unsigned eye,bool depthOrColor)noexcept:owner_(&owner){
            auto& s=local_;if(s.owner!=&owner){s={};s.owner=&owner;}
            previous_=s.selectedPass;++s.passDepth;
            s.selectedPass=s.passDepth==1&&selected&&eye<2&&depthOrColor;
        }
        ~EyePass(){auto& s=local_;if(s.owner==owner_){--s.passDepth;s.selectedPass=previous_;}}
        EyePass(const EyePass&)=delete;
        EyePass& operator=(const EyePass&)=delete;
    };
    bool selectedPass()const noexcept{return local_.owner==this&&local_.selectedPass&&local_.passDepth==1;}
    class World {
        RenderAblation54* owner_{};std::uint64_t generation_{},cpu_{},wall_{};unsigned mode_{};
    public:
        explicit World(RenderAblation54& owner)noexcept {
            Errno preserve;auto& s=local_;if(s.owner!=&owner||s.worldActive)return;
            owner_=&owner;s.worldActive=true;generation_=s.generation;mode_=s.mode;
            cpu_=cpuNs();wall_=wallNs();
        }
        ~World(){
            Errno preserve;if(!owner_)return;auto& s=local_;s.worldActive=false;
            const auto cpu=cpuNs(),wall=wallNs();s.sampleMs=wall/1000000;
            owner_->expire(s,s.sampleMs);
            if(s.generation!=generation_||s.mode!=mode_)++s.mixedFrames;
            else if(cpu_&&wall_&&cpu>=cpu_&&wall>=wall_){++s.worldFrames;s.worldCpuNs+=cpu-cpu_;s.worldWallNs+=wall-wall_;}
            else ++s.clockErrors;
            owner_->publish(s);
        }
        World(const World&)=delete;
    };
    void modelHookReady(bool ready)noexcept {modelHook_.store(ready,std::memory_order_release);}
    // Testable control parser, also used unchanged by the Android sampler.
    void frame(unsigned pid,std::uint64_t scene,std::uint64_t ms,const char* request)noexcept {
        Errno preserve;auto& s=local_;if(s.owner!=this){s={};s.owner=this;}
        s.pid=pid;s.scene=scene;s.sampleMs=ms;
        std::array<std::uint64_t,4> command{};const bool parsed=parse(request,command);
        const auto mode=unsigned(command[0]),target=unsigned(command[1]),ttl=unsigned(command[3]);const auto nonce=command[2];
        const bool valid=parsed&&target==pid&&pid&&nonce&&ttl>=1000&&ms&&ms<=UINT64_MAX-ttl;
        if(valid){
            const bool changed=!s.haveKey||s.keyPid!=target||s.keyMode!=mode||s.keyNonce!=nonce||s.keyTtl!=ttl;
            if(changed){
                s.haveKey=true;s.keyPid=target;s.keyMode=mode;s.keyNonce=nonce;s.keyTtl=ttl;
                s.nonce=nonce;if(s.generation!=UINT64_MAX)++s.generation;
                s.requested=mode;s.mode=mode;s.reason=0;s.deadline=ms+ttl;
                s.matchedBody=s.suppressedBody=s.forwardedBody=0;
                s.matchedModel=s.suppressedModel=s.forwardedModel=0;
                s.worldFrames=s.worldCpuNs=s.worldWallNs=s.mixedFrames=s.clockErrors=0;
                if((mode==2||mode==4)&&!modelHook_.load(std::memory_order_acquire)){s.mode=0;s.reason=3;}
            }
        }else{s.mode=0;s.requested=0;s.reason=request&&*request?1:0;s.deadline=0;}
        expire(s,ms);publish(s);
        armed_.store(valid||s.mode!=0,std::memory_order_relaxed);
    }
    // While nothing is armed (and this thread already owns the state) the property is sampled on every
    // 16th scene only, saving one __system_property_get per frame; arming latency is <= 16 scenes.
    // Armed or not yet owned: every scene. Host-testable; the Android frame(scene) below is the only
    // production caller.
    bool propertySampleDue()noexcept {
        auto& s=local_;
        if(armed_.load(std::memory_order_relaxed)||s.owner!=this){if(s.owner==this)s.idleScenes=0;return true;}
        if(++s.idleScenes>=16){s.idleScenes=0;return true;}
        propertySkips_.fetch_add(1,std::memory_order_relaxed);return false;
    }
    std::uint64_t propertySkips()const noexcept {return propertySkips_.load(std::memory_order_relaxed);}
    void frame(std::uint64_t scene)noexcept {
#ifdef __ANDROID__
        if(!propertySampleDue())return;
        Errno preserve;char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.rdrvr.render_ablation",value);
        frame(unsigned(getpid()),scene,now(),value);
#else
        (void)scene;
#endif
    }
    // One read-only lease check per native Reset, never at per-instance Add.
    bool instanceFrequencyRequestedAt(std::uint64_t ms)const noexcept {
        const auto first=sequence_.load(std::memory_order_acquire);
        if(!ms||!first||(first&1))return false;
        const auto mode=published_[4].load(std::memory_order_acquire);
        const auto deadline=published_[16].load(std::memory_order_acquire);
        const auto last=sequence_.load(std::memory_order_acquire);
        return first==last&&!(last&1)&&mode==10&&deadline>ms;
    }
    bool instanceFrequencyRequested()const noexcept {return instanceFrequencyRequestedAt(now());}
    // Control.scene is the retail consumer ordinal, not the frozen pose sceneId.
    bool nativeModelProbeLeaseAt(std::uint64_t retailOrdinal,std::uint64_t ms,std::uint64_t& nonce,std::uint64_t& generation)const noexcept {
        Errno preserve;nonce=generation=0;
        const auto first=sequence_.load(std::memory_order_acquire);
        if(!first||(first&1)||published_[4].load(std::memory_order_relaxed)!=12)return false;
        const auto ns=published_[1].load(std::memory_order_relaxed),gs=published_[2].load(std::memory_order_relaxed);
        const auto publishedOrdinal=published_[6].load(std::memory_order_relaxed),deadline=published_[16].load(std::memory_order_relaxed);
        const auto sampled=published_[7].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if(sequence_.load(std::memory_order_acquire)!=first||!ms||ms<sampled||!ns||!gs||publishedOrdinal!=retailOrdinal||deadline<=ms)return false;
        nonce=ns;generation=gs;return true;
    }
    bool nativeModelProbeLease(std::uint64_t retailOrdinal,std::uint64_t& nonce,std::uint64_t& generation)const noexcept {
        if(published_[4].load(std::memory_order_acquire)!=12){nonce=generation=0;return false;}
        return nativeModelProbeLeaseAt(retailOrdinal,now(),nonce,generation);
    }
    // A quota key for observational logs; this never authorizes worker commands.
    std::uint64_t workerDiagnosticNonce74()const noexcept {
        const auto& s=local_;
        return s.owner==this&&s.pid&&s.requested==0&&s.mode==0&&!s.reason&&s.deadline>s.sampleMs?s.nonce:0;
    }
    bool armed()const noexcept {return armed_.load(std::memory_order_relaxed);}
    bool vertexMergeAllowed()const noexcept {return !armed()||local_.owner!=this||local_.mode!=6;}
    bool body(bool exact)noexcept {if(!armed())return false;Errno preserve;return draw(false,exact,(exact||local_.mode==3)&&local_.owner==this&&local_.mode&&local_.mode<=4?now():0);}
    bool model(bool exact)noexcept {if(!armed())return false;Errno preserve;return draw(true,exact,(exact||local_.mode==4)&&local_.owner==this&&local_.mode&&local_.mode<=4?now():0);}
    bool bodyAt(bool exact,std::uint64_t ms)noexcept {return draw(false,exact,ms);}
    bool modelAt(bool exact,std::uint64_t ms)noexcept {return draw(true,exact,ms);}
    std::string report()const {
        std::array<std::uint64_t,22> values{};bool stable=false;std::uint64_t sequence{};
        for(unsigned attempt=0;attempt<3&&!stable;++attempt){
            const auto first=sequence_.load(std::memory_order_acquire);if(!first||(first&1))continue;
            for(unsigned i=0;i<values.size();++i)values[i]=published_[i].load(std::memory_order_acquire);
            sequence=sequence_.load(std::memory_order_acquire);stable=first==sequence&&!(sequence&1);
        }
        static constexpr const char* names[]{"pid","nonce","generation","requestedMode","appliedMode","reason",
            "scene","sampleBoottimeMs","remainingMs","matchedBody","suppressedBody","forwardedBody",
            "matchedModel","suppressedModel","forwardedModel","modelHookReady","deadlineBoottimeMs",
            "worldFrames","worldCpuNs","worldWallNs","mixedFrames","clockErrors"};
        std::ostringstream out;out<<"{\"stable\":"<<(stable?"true":"false")<<",\"sequence\":"<<sequence;
        for(unsigned i=0;i<values.size();++i)out<<",\""<<names[i]<<"\":"<<values[i];out<<"}";return out.str();
    }
};
inline thread_local RenderAblation54::State RenderAblation54::local_{};
inline RenderAblation54 renderAblation54;
}
