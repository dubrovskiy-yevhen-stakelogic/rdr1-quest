#pragma once
// Secondary-command-buffer variant of the stereo replay (debug.rdrvr.stereoreplay.async).
//  0 (default): the right eye is replayed inline into the engine's primary command buffer (StereoReplay.cpp).
//  2 (default on the device): as 1, but the secondaries are recorded on a persistent worker thread. Jobs are
//               launched at the start of each LEFT stage with predicted FrameMaps and STREAM: the worker records every
//               op as soon as the render thread has recorded it (published offset); endRecording finalizes the job and
//               the right-eye stage splices it (wait for the last chunk / record the tail / inline on a mismatch).
//  1 (Step 1):  every tape render pass is recorded into its own secondary command buffer
//               (RENDER_PASS_CONTINUE, inheritance = tape pass + resolved right framebuffer) and spliced into the
//               primary with vkCmdBeginRenderPass(SECONDARY_COMMAND_BUFFERS) + vkCmdExecuteCommands; everything
//               outside render passes stays in the primary. Recording still happens on the render thread, inside
//               the replay call; any unsupported tape content declines the job and the inline replay runs.
// The job is split prepare (render thread) / record (no primary, tracked state, texture objects or DUD) /
// splice (render thread) so that Step 2 can move record to a worker thread.
#include "StereoReplay.h"
#include <string>

namespace rdrvr::engine::stereo {
unsigned asyncMode() noexcept;                    // 1 only when the property (or the test override) is exactly 1
void setAsyncModeForTests(unsigned mode) noexcept; // host tests: overrides the property; UINT32_MAX restores it
// Command pool / secondary command buffer entry points. Default: loader exports (bypass the engine's dispatch
// slots and every capture observer, like EyeDump/UniformIntegration). Host tests install fakes.
struct AsyncDeviceFunctions {
    PFN_vkCreateCommandPool createCommandPool{};PFN_vkDestroyCommandPool destroyCommandPool{};PFN_vkResetCommandPool resetCommandPool{};
    PFN_vkAllocateCommandBuffers allocateCommandBuffers{};PFN_vkBeginCommandBuffer beginCommandBuffer{};PFN_vkEndCommandBuffer endCommandBuffer{};
};
void setAsyncDeviceFunctionsForTests(const AsyncDeviceFunctions* functions) noexcept; // nullptr: loader exports
std::uint64_t asyncFallbacksForTests() noexcept;
// Worker mode (async=2) test hooks: a paused worker leaves launched jobs Queued (splice takes them over).
struct AsyncWorkerCounters {std::uint64_t launched,takeovers,predictionMismatches,workerJobs,joinWaits,discarded,floorViolations,jobs,fallbacks,tails,
    relaunched,cancelled,ringTight,ringPressureJobs;};
std::uint64_t asyncRegionTopForTests() noexcept; // bottom of the right-eye blocks of the current worker frame (0: no region)
AsyncWorkerCounters asyncWorkerCountersForTests() noexcept;
void setWorkerPausedForTests(bool paused) noexcept;
void setWorkerDelayForTests(unsigned microseconds) noexcept;   // the worker holds each job this long before recording
// Streaming cadence (device: debug.rdrvr.stereoreplay.publish/wake/poll). publish==0 restores the defaults.
void setStreamTuningForTests(unsigned publish,unsigned wake,unsigned pollUs,unsigned streamMask=3) noexcept;
void waitWorkerForTests(bool untilRunning) noexcept;          // until a job runs on the worker / until the worker caught up (nothing runnable)
std::string asyncReport(); // {"mode","jobs","fallbacks","secondaries","executeCalls","lastDecline",...}
}
