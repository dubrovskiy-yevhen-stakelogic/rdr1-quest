#pragma once
#include <vulkan/vulkan.h>
#include <stdint.h>
#if defined(_WIN32)
#define RDR_W28_EXPORT __declspec(dllexport)
#else
#define RDR_W28_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
enum { RDR_WORKER28_ABI=1 };
typedef struct RdrWorker28Stats {
 uint32_t size,abi,enabled,qualified,mode,declineReason,producerSelected,drained;
 uint64_t deviceGeneration,recordings,queued,executed,admissions,closeRequests,unknownEscapes;
 uint64_t originalDraws,selectedProducer,workerGeneration,publications;
 uint64_t lastRecordQueued,lastRecordExecuted,lastRecordDraws,queuedPrefixAtFirstClose;
 char firstCloseApi[96];
} RdrWorker28Stats;
// Cold opt-in before the first instance; immutable after it. Environment opt-in
// is optional for desktop. Explicit Android activation calls ConfigureOnce.
RDR_W28_EXPORT uint32_t rdrWorker28ConfigureOnce(uint32_t abi,uint32_t enabled);
RDR_W28_EXPORT uint32_t rdrWorker28OwnsEntry(PFN_vkVoidFunction entry);
RDR_W28_EXPORT uint32_t rdrWorker28Prepare(VkDevice device);
RDR_W28_EXPORT uint32_t rdrWorker28SetRecordingMode(VkDevice device,uint32_t mode);
RDR_W28_EXPORT void* rdrWorker28GetWorkerNativeHandle(VkDevice device);
RDR_W28_EXPORT uint32_t rdrWorker28Drain(VkDevice device);
RDR_W28_EXPORT uint32_t rdrWorker28Snapshot(VkDevice device,RdrWorker28Stats* stats);
#ifdef __cplusplus
}
#endif
