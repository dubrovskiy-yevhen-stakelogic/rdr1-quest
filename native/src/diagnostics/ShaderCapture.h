#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace rdrvr::diagnostics {
// Opt-in startup observer (nativeEnableShaderCapture). All storage is bounded and allocated before native
// initialization. The driver call is never skipped, retried or given altered data.
class ShaderCapture {
public:
    static constexpr std::size_t maxModuleBytes=512*1024, maxBytes=32*1024*1024;
    static constexpr unsigned maxModules=2048, maxAttempts=8192;
    using Read=bool(*)(void*,std::uintptr_t,void*,std::size_t);
    bool enable() noexcept {
        const int saved=errno;
        bool ok=false;
        try {
            std::lock_guard guard(mutex_);
            if(!enabled_) {
                auto arena=std::make_unique<unsigned char[]>(maxBytes);
                auto scratch=std::make_unique<unsigned char[]>(maxModuleBytes);
                arena_=std::move(arena);scratch_=std::move(scratch);
                enabled_.store(true,std::memory_order_release);
            }
            ok=true;
        }catch(...) { }
        errno=saved;return ok;
    }
    VkResult invoke(PFN_vkCreateShaderModule original,VkDevice device,const VkShaderModuleCreateInfo* info,
                    const VkAllocationCallbacks* allocator,VkShaderModule* output,Read read,void* context) noexcept {
        const auto result=original(device,info,allocator,output);
        const int saved=errno;
        if(enabled_.load(std::memory_order_acquire)) {
            ++calls_;
            if(result==VK_SUCCESS)observe(info,read,context);
            else ++failedCreates_;
        }
        errno=saved;return result;
    }
    // Runs only on the writer thread. Draining does not reset lifetime budgets
    // or discard dedup identities. File IO/hash computation never runs in invoke.
    std::vector<unsigned char> take() {
        std::lock_guard guard(mutex_);
        if(taken_==count_)return {};
        const auto& entry=entries_[taken_];
        std::vector<unsigned char> result(arena_.get()+entry.offset,arena_.get()+entry.offset+entry.bytes);
        ++taken_;return result;
    }
    std::string report() {
        std::lock_guard guard(mutex_);std::ostringstream out;
        out<<"{\"enabled\":"<<(enabled_?"true":"false")<<",\"calls\":"<<calls_
           <<",\"failedCreates\":"<<failedCreates_<<",\"attempts\":"<<attempts_
           <<",\"modules\":"<<count_<<",\"bytes\":"<<used_<<",\"taken\":"<<taken_
           <<",\"duplicates\":"<<duplicates_<<",\"rejected\":"<<rejected_
           <<",\"busy\":"<<busy_<<",\"budgetSkipped\":"<<budgetSkipped_
           <<",\"exceptions\":"<<exceptions_<<",\"maxBytes\":"<<maxBytes
           <<",\"maxModules\":"<<maxModules<<",\"maxModuleBytes\":"<<maxModuleBytes<<"}";
        return out.str();
    }
private:
    struct Entry {std::uint64_t hash{};std::size_t offset{},bytes{};};
    void observe(const VkShaderModuleCreateInfo* source,Read read,void* context) noexcept {
        try {
            std::unique_lock guard(mutex_,std::try_to_lock);
            if(!guard.owns_lock()){++busy_;return;}
            if(attempts_==maxAttempts || count_==maxModules || used_==maxBytes){++budgetSkipped_;return;}
            ++attempts_;
            VkShaderModuleCreateInfo info{};
            if(!source || !read || !read(context,reinterpret_cast<std::uintptr_t>(source),&info,sizeof(info)) ||
               info.sType!=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO || info.pNext || info.flags || !info.pCode ||
               info.codeSize<20 || info.codeSize%4 || info.codeSize>maxModuleBytes) {++rejected_;return;}
            const auto address=reinterpret_cast<std::uintptr_t>(info.pCode);
            if(address>UINTPTR_MAX-info.codeSize){++rejected_;return;}
            // The guarded pipe reader deliberately accepts at most 64 KiB.
            for(std::size_t offset=0;offset<info.codeSize;) {
                const auto bytes=std::min<std::size_t>(65536,info.codeSize-offset);
                if(!read(context,address+offset,scratch_.get()+offset,bytes)){++rejected_;return;}
                offset+=bytes;
            }
            std::uint32_t magic{};std::memcpy(&magic,scratch_.get(),sizeof(magic));
            if(magic!=0x07230203){++rejected_;return;}
            std::uint64_t hash=14695981039346656037ULL;
            for(std::size_t i=0;i<info.codeSize;++i){hash^=scratch_[i];hash*=1099511628211ULL;}
            for(unsigned i=0;i<count_;++i) {
                const auto& e=entries_[i];
                if(e.hash==hash && e.bytes==info.codeSize && !std::memcmp(arena_.get()+e.offset,scratch_.get(),e.bytes)){++duplicates_;return;}
            }
            if(info.codeSize>maxBytes-used_){++budgetSkipped_;return;}
            std::memcpy(arena_.get()+used_,scratch_.get(),info.codeSize);
            entries_[count_++]={hash,used_,info.codeSize};used_+=info.codeSize;
        }catch(...) {++exceptions_;}
    }
    std::atomic<bool> enabled_{};
    std::mutex mutex_;
    std::unique_ptr<unsigned char[]> arena_,scratch_;
    std::array<Entry,maxModules> entries_{};
    std::size_t used_{};
    unsigned count_{},taken_{},attempts_{},duplicates_{},rejected_{};
    std::atomic<std::uint64_t> calls_{},failedCreates_{},busy_{},budgetSkipped_{},exceptions_{};
};
// A device-specific get-proc pointer may differ from the global loader dispatch.
// In that case leave it untouched rather than silently redirect to another call.
inline PFN_vkVoidFunction shaderCaptureProc(PFN_vkVoidFunction result,const char* name,
        PFN_vkCreateShaderModule original,PFN_vkCreateShaderModule wrapper) noexcept {
    return result && original && result==reinterpret_cast<PFN_vkVoidFunction>(original) && name &&
        !std::strcmp(name,"vkCreateShaderModule")?reinterpret_cast<PFN_vkVoidFunction>(wrapper):result;
}
} // namespace rdrvr::diagnostics
