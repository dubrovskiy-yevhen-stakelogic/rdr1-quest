#include "CommandResources.h"
#include <cerrno>
#include <limits>
#include <sstream>
namespace rdrvr::engine {
namespace {struct Errno {int value{errno};~Errno(){errno=value;}};}
void CommandResources::invalidate(Buffer& b) noexcept {
    b.state=State::Invalid;b.owner={};++invalidated_;
    if(invalidated)invalidated(b.record.command);
}
void CommandResources::erasePool(VkCommandPool p) noexcept {
    for(auto i=buffers_.begin();i!=buffers_.end();)if(i->second.record.pool==p){invalidate(i->second);i=buffers_.erase(i);}else ++i;
    pools_.erase(p);
}
bool CommandResources::bind(VkDevice d,void(*callback)(VkCommandBuffer) noexcept){std::lock_guard lock(mutex_);if(!d || device_)return false;device_=d;invalidated=callback;return true;}
void CommandResources::poolCreated(VkDevice d,const VkCommandPoolCreateInfo* c,VkCommandPool p,VkResult r) noexcept {
    Errno preserve;
    try{std::lock_guard lock(mutex_);if(!device_ || d!=device_ || r!=VK_SUCCESS || !p)return;
        erasePool(p);
        if(!c || c->sType!=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO || c->pNext ||
           (c->flags&~(VK_COMMAND_POOL_CREATE_TRANSIENT_BIT|VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT)) || pools_.size()>=poolCapacity){++rejected_;return;}
        pools_.emplace(p,Pool{c->queueFamilyIndex});
    }catch(...){}
}
void CommandResources::poolDestroyed(VkDevice d,VkCommandPool p) noexcept {Errno preserve;std::lock_guard lock(mutex_);if(d==device_)erasePool(p);}
void CommandResources::allocated(VkDevice d,const VkCommandBufferAllocateInfo* c,const VkCommandBuffer* commands,VkResult r) noexcept {
    Errno preserve;
    try{std::lock_guard lock(mutex_);if(!device_ || d!=device_ || r!=VK_SUCCESS || !c || !commands)return;
        if(c->commandBufferCount>bufferCapacity){for(auto& [cmd,b]:buffers_)invalidate(b);buffers_.clear();++rejected_;return;}
        // A successful allocation can reuse handles, including after unsupported
        // pool recreation. Remove stale identities before admitting the new set.
        for(std::uint32_t i=0;i<c->commandBufferCount;++i){auto old=buffers_.find(commands[i]);if(old!=buffers_.end()){invalidate(old->second);buffers_.erase(old);}}
        const auto pool=pools_.find(c->commandPool);
        if(c->sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO || c->pNext || pool==pools_.end() ||
           !c->commandBufferCount || c->commandBufferCount>bufferCapacity-buffers_.size() ||
           (c->level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY && c->level!=VK_COMMAND_BUFFER_LEVEL_SECONDARY) ||
           c->commandBufferCount>std::numeric_limits<std::uint64_t>::max()-generation_){++rejected_;return;}
        for(std::uint32_t i=0;i<c->commandBufferCount;++i){if(!commands[i])continue;
            buffers_.emplace(commands[i],Buffer{{commands[i],c->commandPool,d,pool->second.family,++generation_,0},c->level,State::Initial,{}});}
    }catch(...){/* A missing/partial registry never licenses an unknown command. */}
}
void CommandResources::freed(VkDevice d,VkCommandPool p,std::uint32_t count,const VkCommandBuffer* commands) noexcept {
    Errno preserve;std::lock_guard lock(mutex_);if(d!=device_ || !commands)return;
    if(count>bufferCapacity){erasePool(p);++rejected_;return;}
    for(std::uint32_t i=0;i<count;++i){auto it=buffers_.find(commands[i]);if(it!=buffers_.end() && it->second.record.pool==p){invalidate(it->second);buffers_.erase(it);}}
}
void CommandResources::begun(VkCommandBuffer cmd,const VkCommandBufferBeginInfo* c,VkResult r) noexcept {
    Errno preserve;std::lock_guard lock(mutex_);auto it=buffers_.find(cmd);if(it==buffers_.end())return;
    auto& b=it->second;
    // A failed begin cannot serve as evidence that an old recording survived.
    invalidate(b);
    if(r!=VK_SUCCESS || !c || c->sType!=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO || c->pNext || c->pInheritanceInfo ||
       b.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY || (c->flags&~VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) ||
       recording_==std::numeric_limits<std::uint64_t>::max()){++rejected_;return;}
    b.state=State::Recording;b.owner=std::this_thread::get_id();b.record.recording=++recording_;
}
void CommandResources::ended(VkCommandBuffer cmd,VkResult r) noexcept {
    Errno preserve;std::lock_guard lock(mutex_);auto it=buffers_.find(cmd);if(it==buffers_.end())return;
    auto& b=it->second;
    if(r==VK_SUCCESS && b.state==State::Recording && b.owner==std::this_thread::get_id()){b.state=State::Executable;b.owner={};}
    else invalidate(b);
}
void CommandResources::reset(VkCommandBuffer cmd,VkResult r) noexcept {
    Errno preserve;std::lock_guard lock(mutex_);auto it=buffers_.find(cmd);if(it==buffers_.end())return;
    invalidate(it->second);if(r==VK_SUCCESS)it->second.state=State::Initial;
}
void CommandResources::poolReset(VkDevice d,VkCommandPool p,VkResult r) noexcept {
    Errno preserve;std::lock_guard lock(mutex_);if(d!=device_)return;
    for(auto& [cmd,b]:buffers_)if(b.record.pool==p){invalidate(b);if(r==VK_SUCCESS)b.state=State::Initial;}
}
bool CommandResources::recording(VkDevice d,VkCommandBuffer cmd,std::uint32_t family,CommandRecord& out) noexcept {
    Errno preserve;std::lock_guard lock(mutex_);auto i=buffers_.find(cmd);
    if(!device_ || d!=device_ || i==buffers_.end() || i->second.state!=State::Recording ||
       i->second.owner!=std::this_thread::get_id() || i->second.record.family!=family){++misses_;return false;}
    out=i->second.record;return true;
}
void CommandResources::clear(VkDevice d) noexcept {Errno preserve;std::lock_guard lock(mutex_);if(d!=device_)return;for(auto& [cmd,b]:buffers_)invalidate(b);buffers_.clear();pools_.clear();device_={};}
std::string CommandResources::report() const {std::lock_guard lock(mutex_);std::ostringstream s;s<<"{\"pools\":"<<pools_.size()<<",\"buffers\":"<<buffers_.size()<<",\"recordings\":"<<recording_<<",\"invalidated\":"<<invalidated_<<",\"rejected\":"<<rejected_<<",\"lookupMisses\":"<<misses_<<'}';return s.str();}
}
