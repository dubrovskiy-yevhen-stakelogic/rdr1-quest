#include "PlannerReplayCapture.h"
#include "diagnostics/FrameTimings.h"
#include <sstream>
#include <algorithm>
namespace rdrvr::engine {
PlannerReplayCapture plannerReplayCapture{120};
thread_local PlannerReplayCapture::Scene* PlannerReplayCapture::current_{};
PlannerReplayCapture::Scene::Scene(PlannerReplayCapture& owner)noexcept{
    // Disabled FPS/OFF path: one atomic read, no clock, TLS access or allocation.
    const auto state=owner.state();if(state!=State::Armed&&state!=State::Claimed)return;
    owner_=&owner;previous_=current_;current_=this;
}
PlannerReplayCapture::Scene::~Scene(){if(!owner_)return;if(claimed_)owner_->finish(*this);current_=previous_;}
bool PlannerReplayCapture::request()noexcept{
    if(writerClosed_.load(std::memory_order_acquire))return false;
    const auto ticket=diagnostics::profilerTicket();
    State idle=State::Idle;if(!state_.compare_exchange_strong(idle,State::Arming,std::memory_order_acq_rel))return false;
    // Loading saved settings can legitimately change FPS/DETAIL/OFF after the
    // explicit request. Bind its measurement epoch only when a frame is claimed.
    requestedEpoch_.store(ticket,std::memory_order_relaxed);
    state_.store(State::Armed,std::memory_order_seq_cst);
    if(writerClosed_.load(std::memory_order_seq_cst)){cancelPending();return false;}
    return true;
}
bool PlannerReplayCapture::cancelPending()noexcept{
    State armed=State::Armed;
    // This failed-CAS load is part of the Armed/closed SC handshake too:
    // closeWriter must either cancel Armed or force the publisher to see closed.
    if(!state_.compare_exchange_strong(armed,State::Failed,std::memory_order_seq_cst))return false;
    failure_.store(Failure::Cancelled,std::memory_order_release);return true;
}
bool PlannerReplayCapture::claim(std::uint64_t f,std::uint64_t epoch)noexcept{
    if(state()!=State::Armed)return false;
    if(!f||!current_||current_->owner_!=this||current_->previous_||current_->claimed_)return false;
    // Runtime's optional detailedEpoch was sampled earlier than this call and
    // may be stale during a menu/startup settings transition. The measurement
    // CAS below uses the actual current ticket and remains the sole authority.
    (void)epoch;
    State armed=State::Armed;
    // An explicit request waits through qualified stereo-world frames only.
    // Menus/unready/nested/foreign scenes never reach this counter. No timers,
    // sleeping or engine work is introduced, and no-request path returns above.
    if(warmupCompleted_.load(std::memory_order_relaxed)<warmupRequired_){
        warmupCompleted_.fetch_add(1,std::memory_order_relaxed);return false;
    }
    if(!state_.compare_exchange_strong(armed,State::Claimed,std::memory_order_acq_rel))return false;
    const auto currentTicket=diagnostics::profilerTicket();
    const auto capture=diagnostics::beginCaptureMeasurements(f,currentTicket,diagnostics::monotonicNs());
    if(!capture){
        // No measurement ownership, no native/codec snapshot and no frame has
        // been claimed. Defer this SAME explicit request to a later eligible
        // frame; writer cancellation/timeout still makes it terminal. Never
        // rearm after a successful claim or an attempted snapshot.
        preCaptureDeferrals_.fetch_add(1,std::memory_order_relaxed);
        state_.store(State::Armed,std::memory_order_seq_cst);
        if(writerClosed_.load(std::memory_order_seq_cst))cancelPending();
        return false;
    }
    boundEpoch_.store(currentTicket,std::memory_order_relaxed);
    ownsMeasurementExclusion_.store(true,std::memory_order_release);
    frame_.store(f,std::memory_order_relaxed);captureEpoch_.store(capture,std::memory_order_relaxed);current_->claimed_=true;return true;
}
bool PlannerReplayCapture::claimed(std::uint64_t f)const noexcept{
    return state()==State::Claimed&&f&&f==frame_.load(std::memory_order_relaxed)&&current_&&current_->owner_==this&&current_->claimed_;
}
void PlannerReplayCapture::before(std::uint64_t f,PriorityOcclusionSources& planner,const std::array<NativeStaticOcclusionSource::Camera,2>& cameras,
                                  unsigned w,unsigned h,std::string_view buildId,std::span<const PlannerReplaySnapshot::Counter> metadata)noexcept{
    if(!claimed(f)||before_)return;
    if(diagnostics::profilerTicket()!=captureEpoch_.load(std::memory_order_relaxed)){failure_=Failure::ProfilerChanged;return;}
    std::array<PlannerReplaySnapshot::Counter,128> counters{};
    if(metadata.size()>counters.size()-2){failure_=Failure::Before;return;}
    std::copy(metadata.begin(),metadata.end(),counters.begin());
    counters[metadata.size()]={"warmupRequired",warmupRequired_};
    counters[metadata.size()+1]={"warmupCompleted",warmupCompleted()};
    before_=snapshot_.captureBefore(planner,cameras,w,h,captureEpoch_,buildId)&&snapshot_.metadata({counters.data(),metadata.size()+2});
    if(!before_)failure_=Failure::Before;
}
void PlannerReplayCapture::abandonFrame(std::uint64_t f)noexcept{
    if(!claimed(f))return;snapshot_.abandon();failure_=Failure::Before;
}
void PlannerReplayCapture::after(std::uint64_t f,const PriorityOcclusionSources& planner)noexcept{
    if(!claimed(f)||!before_)return;
    snapshot_.captureAfter(planner);
    if(snapshot_.state()!=PlannerReplaySnapshot::State::Ready)failure_=Failure::After;
}
void PlannerReplayCapture::finish(Scene&)noexcept{
    bool good=snapshot_.state()==PlannerReplaySnapshot::State::Ready&&diagnostics::profilerTicket()==captureEpoch_.load(std::memory_order_relaxed);
    if(!good){snapshot_.abandon();if(failure_==Failure::None)failure_=Failure::Incomplete;}
    if(!good&&ownsMeasurementExclusion_.exchange(false,std::memory_order_acq_rel))
        diagnostics::endCaptureMeasurements(diagnostics::monotonicNs());
    // Ready publishes bytes; exclusion remains until async writer's finally.
    // This also excludes scheduling/copy/hash/fsync overhead from live rates.
    // Four seq_cst handshake operations prevent ARM store-buffering: either
    // reporter sees Ready or producer sees writerClosed; both cannot see old.
    state_.store(good?State::Ready:State::Failed,std::memory_order_seq_cst);
    if(good&&writerClosed_.load(std::memory_order_seq_cst))completeWrite();
}
std::vector<std::byte> PlannerReplayCapture::take()noexcept{
    State ready=State::Ready;if(!state_.compare_exchange_strong(ready,State::Taken,std::memory_order_acq_rel))return {};
    auto bytes=snapshot_.take();byteSize_.store(bytes.size(),std::memory_order_release);return bytes;
}
void PlannerReplayCapture::closeWriter()noexcept{
    writerClosed_.store(true,std::memory_order_seq_cst);
    cancelPending();completeWrite();
    // If still Claimed, finish() observes writerClosed after publishing Ready.
    // Thus no READY/worker-exit race can leave measurement exclusion held.
}
void PlannerReplayCapture::completeWrite()noexcept{
    auto value=state_.load(std::memory_order_seq_cst);
    if(value==State::Ready){
        // No consumer took ownership (OOM before take, reporter exit, etc.).
        if(!state_.compare_exchange_strong(value,State::Failed,std::memory_order_acq_rel)){
            if(value!=State::Taken)return;
        }else{snapshot_.abandon();failure_.store(Failure::Cancelled,std::memory_order_release);}
    }else if(value!=State::Taken)return;
    if(ownsMeasurementExclusion_.exchange(false,std::memory_order_acq_rel))
        diagnostics::endCaptureMeasurements(diagnostics::monotonicNs());
}
std::string PlannerReplayCapture::report()const{
    std::ostringstream out;out<<"{\"oneShot\":true,\"maxBytes\":"<<PlannerReplaySnapshot::MaxBytes<<",\"state\":"<<unsigned(state())
       <<",\"failure\":"<<unsigned(failure_.load(std::memory_order_acquire))<<",\"frame\":"<<frame_.load(std::memory_order_relaxed)
       <<",\"requestEpochInformational\":true,\"claimProfilerEpoch\":"<<boundEpoch_.load(std::memory_order_relaxed)
       <<",\"preCaptureDeferrals\":"<<preCaptureDeferrals_.load(std::memory_order_relaxed)
       <<",\"captureGeneration\":1,\"requestedProfilerEpoch\":"<<requestedEpoch_.load(std::memory_order_relaxed)<<",\"captureProfilerEpoch\":"<<captureEpoch_.load(std::memory_order_relaxed)
       <<",\"exclusionScope\":\"native-scene-through-reporter-write\",\"measurementExclusionActive\":"<<(ownsMeasurementExclusion_.load()?"true":"false")
       <<",\"exclusionBeginNs\":"<<diagnostics::captureBeginNs.load()<<",\"exclusionEndNs\":"<<diagnostics::captureEndNs.load()
       <<",\"warmupRequired\":"<<warmupRequired_<<",\"warmupCompleted\":"<<warmupCompleted()
       <<",\"bytesTaken\":"<<byteSize_.load(std::memory_order_relaxed)<<",\"performanceExcluded\":"<<(frame_.load()?"true":"false")<<'}';return out.str();
}
}
