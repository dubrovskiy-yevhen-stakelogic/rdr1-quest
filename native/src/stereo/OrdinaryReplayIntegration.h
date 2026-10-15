#pragma once
#include "UniformIntegration.h"
#include "OrdinaryMainStartupRequest.h"
#include "OrdinaryDepthReplay.h"
#include "OrdinaryGateProbe.h"

namespace rdrvr::engine {
// One coordinator on the native recording thread. It outlives per-frame
// EarlyGameContext/Binding replacement; stale frame stamps are checked before
// touching any saved Binding observation. No per-object allocation here.
inline thread_local std::unique_ptr<rdr95::OrdinaryDepthReplay> ordinaryReplay;
inline thread_local bool ordinaryReplayFrameOpen{};
inline std::atomic<std::uint64_t> ordinaryReplayFrames{};
inline constexpr const char* ordinaryReplayWorkNames[]={"leftCalls","captured","rightCalls","replayed","duplicate","capacity",
    "nativeDeclined","packetDeclined","targetDeclined","emitDeclined","faults","reads","bytes"};
inline std::array<std::atomic<std::uint64_t>,std::size(ordinaryReplayWorkNames)> ordinaryReplayLastWork{};
inline bool ordinaryReplayBegin()noexcept {
    if(!ordinaryMainStartupRequested()||!cameraRuntime.ready()||!indexedObservation.enabled()||
       !cameraOwnsRecordingThread())return false;
    if(!ordinaryReplay)ordinaryReplay.reset(new(std::nothrow)rdr95::OrdinaryDepthReplay(
        indexedObservation,ordinaryCapture,cameraRuntime,gameOrdinaryReplayContext));
    ordinaryReplayFrameOpen=ordinaryReplay&&ordinaryReplay->begin();if(ordinaryReplayFrameOpen)rdr95::gateprobe::begin();return ordinaryReplayFrameOpen;
}
inline void ordinaryReplayEnd()noexcept {
    if(!ordinaryReplay)return;
    if(ordinaryReplayFrameOpen){
        const auto& s=ordinaryReplay->statistics();
        const std::uint64_t values[]={s.left,s.captured,s.right,s.replayed,s.duplicate,s.capacity,
            s.nativeDeclined,s.packetDeclined,s.targetDeclined,s.emitDeclined,s.faults,s.reads,s.bytes};
        static_assert(std::size(values)==std::size(ordinaryReplayWorkNames));
        for(unsigned i=0;i<std::size(values);++i)ordinaryReplayLastWork[i].store(values[i],std::memory_order_relaxed);
        ordinaryReplayFrames.fetch_add(1,std::memory_order_relaxed);ordinaryReplayFrameOpen=false;
    }
    rdr95::gateprobe::end();
    ordinaryReplay->end();
}
}
