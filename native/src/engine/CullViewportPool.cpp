#include "CullViewportPool.h"
#include <cerrno>
#include <limits>
#include <utility>

namespace rdrvr::engine {
namespace {struct Errno {int value{errno};~Errno(){errno=value;}};}
CullViewportPool::Borrow::Borrow(std::shared_ptr<State> state,unsigned index,const Slot& slot):
    state_(std::move(state)),slot_(index),ticket_(slot.ticket),viewport_(slot.viewport){}
CullViewportPool::Borrow::Borrow(Borrow&& other) noexcept:
    state_(std::move(other.state_)),slot_(other.slot_),ticket_(other.ticket_),viewport_(std::exchange(other.viewport_,nullptr)){}
CullViewportPool::Borrow::~Borrow(){
    Errno preserve;
    if(state_){std::lock_guard guard(state_->mutex);--state_->slots[slot_].readers;}
}
unsigned CullViewportPool::collect(State& state) noexcept {
    if(state.owner!=std::this_thread::get_id())return 0;
    unsigned retired{};
    for(auto& slot:state.slots)if(slot.viewport&&slot.complete&&!slot.readers&&slot.camera->retire(slot.ticket.serial)){
        slot.viewport=nullptr;slot.ticket={};slot.complete=false;slot.shaft=nullptr;slot.visibility=0;++retired;
    }
    return retired;
}
const void* CullViewportPool::lease(const void* source,const VisibilityTicket& ticket) noexcept {
    Errno preserve;
    try{
        auto& state=*state_;std::lock_guard guard(state.mutex);
        if(!source||!state.api||!ticket.renderer||!ticket.serial||ticket.serial<=state.last||
           ticket.cull.sceneId!=ticket.serial||ticket.eyes.sceneId!=ticket.serial||
           ticket.cull.tracking.frameId!=ticket.eyes.tracking.frameId||ticket.cull.tracking.epochs!=ticket.eyes.tracking.epochs||
           (state.owner!=std::thread::id{}&&state.owner!=std::this_thread::get_id()))return nullptr;
        collect(state);
        // Do not clone a borrowed clone, even when its original call finished.
        for(const auto& slot:state.slots)if(slot.camera&&slot.camera->viewport()==source)return nullptr;
        for(auto& slot:state.slots)if(!slot.viewport){
            if(!slot.camera)slot.camera=std::make_unique<CullViewport>(state.api);
            auto plan=ticket.cull;
            if(!ticket.stereoCull)plan.camera=ticket.stock;
            if(!slot.camera->prepare(source,plan,ticket.stereoCull))continue;
            auto* viewport=slot.camera->lease(ticket.serial);if(!viewport)continue;
            slot.ticket=ticket;slot.viewport=viewport;slot.complete=false;slot.shaft=nullptr;slot.visibility=0;
            state.owner=std::this_thread::get_id();state.last=ticket.serial;return viewport;
        }
    }catch(...){}
    return nullptr;
}
CullViewportPool::Borrow CullViewportPool::borrow(const void* viewport) noexcept {
    Errno preserve;
    try{
        auto state=state_;std::lock_guard guard(state->mutex);
        for(unsigned i=0;i<state->slots.size();++i){auto& slot=state->slots[i];
            if(viewport&&slot.viewport==viewport&&!slot.complete&&slot.readers<std::numeric_limits<unsigned>::max()){
                ++slot.readers;return Borrow(std::move(state),i,slot);
            }
        }
    }catch(...){}
    return {};
}
bool CullViewportPool::stereoCull(std::uintptr_t renderer,std::uint64_t serial)const noexcept {
    Errno preserve;
    try{std::lock_guard guard(state_->mutex);
        for(const auto& slot:state_->slots)if(slot.viewport&&!slot.complete&&slot.ticket.renderer==renderer&&slot.ticket.serial==serial)return slot.ticket.stereoCull;
    }catch(...){}
    return false;
}
bool CullViewportPool::complete(std::uintptr_t renderer,std::uint64_t serial) noexcept {
    Errno preserve;
    try{auto& state=*state_;std::lock_guard guard(state.mutex);
        for(auto& slot:state.slots)if(slot.viewport&&slot.ticket.renderer==renderer&&slot.ticket.serial==serial&&!slot.complete){slot.complete=true;return true;}
    }catch(...){}
    return false;
}
void CullViewportPool::visibility(const void* viewport,const void* shaft,bool success) noexcept {
    Errno preserve;
    try{std::lock_guard guard(state_->mutex);
        for(auto& slot:state_->slots)if(viewport&&slot.viewport==viewport&&!slot.complete){
            if(!success||!shaft||slot.visibility){slot.visibility=2;slot.shaft=nullptr;}
            else{slot.visibility=1;slot.shaft=shaft;}return;
        }
    }catch(...){}
}
const void* CullViewportPool::shaft(std::uintptr_t renderer,std::uint64_t serial)const noexcept {
    Errno preserve;
    try{std::lock_guard guard(state_->mutex);
        for(const auto& slot:state_->slots)if(slot.viewport&&!slot.complete&&slot.ticket.renderer==renderer&&slot.ticket.serial==serial&&slot.visibility==1)return slot.shaft;
    }catch(...){}
    return nullptr;
}
const void* CullViewportPool::viewport(std::uintptr_t renderer,std::uint64_t serial)const noexcept {
    Errno preserve;
    try{std::lock_guard guard(state_->mutex);
        for(const auto& slot:state_->slots)if(slot.viewport&&!slot.complete&&slot.ticket.renderer==renderer&&
            slot.ticket.serial==serial&&slot.visibility==1&&slot.ticket.stereoCull)return slot.viewport;
    }catch(...){}
    return nullptr;
}
std::uint64_t CullViewportPool::serial(std::uintptr_t renderer,std::uint32_t marker)const noexcept {
    Errno preserve;
    try{std::lock_guard guard(state_->mutex);std::uint64_t found{};
        for(const auto& slot:state_->slots)if(slot.viewport&&!slot.complete&&slot.ticket.renderer==renderer&&slot.ticket.marker==marker){
            if(found)return 0;found=slot.ticket.serial;
        }
        return found;
    }catch(...){return 0;}
}
unsigned CullViewportPool::collect() noexcept {Errno preserve;try{std::lock_guard guard(state_->mutex);return collect(*state_);}catch(...){return 0;}}
unsigned CullViewportPool::pending()const noexcept {
    Errno preserve;try{std::lock_guard guard(state_->mutex);unsigned count{};for(auto& slot:state_->slots)count+=slot.viewport!=nullptr;return count;}catch(...){return 4;}
}
} // namespace rdrvr::engine
