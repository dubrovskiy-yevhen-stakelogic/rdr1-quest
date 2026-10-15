#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <span>
#include <algorithm>
namespace rdrvr::runtime {
// A present-queue fence covers a different queue only after a semaphore from
// the exact captured command submission was consumed by that present. Neither
// a signaled old fence nor a submission on an unrelated queue is evidence.
struct SubmitCompletion {
    VkQueue queue{};
    std::array<VkSemaphore,16> signals{};
    unsigned count{};
    void observe(VkQueue q,const VkSubmitInfo& submit) noexcept {
        queue=q;count=0;signals={};
        if(submit.pNext||submit.signalSemaphoreCount>signals.size()||
           (submit.signalSemaphoreCount&&!submit.pSignalSemaphores))return;
        for(unsigned i=0;i<submit.signalSemaphoreCount;++i)
            if(submit.pSignalSemaphores[i])signals[count++]=submit.pSignalSemaphores[i];
    }
    bool covered(VkQueue present,std::span<const VkSemaphore> waits)const noexcept {
        if(!queue)return false;
        if(queue==present)return true;
        for(unsigned i=0;i<count;++i)
            if(std::find(waits.begin(),waits.end(),signals[i])!=waits.end())return true;
        return false;
    }
};
}
