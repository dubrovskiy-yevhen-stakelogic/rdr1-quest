#include "EngineTextureScope.h"
#include <cerrno>
namespace rdrvr::engine {
namespace {struct Errno {int value{errno};~Errno(){errno=value;}};}
bool EngineTextureScope::owner() const {return owner_==std::this_thread::get_id() && api_.ownsThread();}
bool EngineTextureScope::begin() noexcept {
    Errno preserve;
    try {if(!api_ || !map_ || !owner() || active_ || failed_ || count_)return false;active_=true;status_="READY";return true;}catch(...){return false;}
}
const void* EngineTextureScope::route(const void* effect,void* data,int id,const void* requested,bool global) {
    Errno preserve;
    if(!active_ || owner_!=std::this_thread::get_id())return requested;
    try{if(!api_.ownsThread())fail("TEXTURE_OWNER");}catch(...){fail("TEXTURE_OWNER_EXCEPTION");}
    const void* mapped=requested;
    if(!failed_){
        try {if(!map_(user_,requested,&mapped) || (requested && !mapped)){mapped=requested;fail("TEXTURE_MAPPING");}}
        catch(...){mapped=requested;fail("TEXTURE_MAPPING_EXCEPTION");}
    }
    std::size_t index=0;
    while(index<count_){const auto& e=entries_[index];
        if(e.global==global && e.id==id && (global || (e.effect==effect && e.data==data)))break;
        ++index;
    }
    if(index==count_){
        if(mapped==requested)return requested;
        if(count_==capacity){fail("TEXTURE_JOURNAL_CAPACITY");return requested;}
        ++count_;
    }
    // Update even when this call does not map: replaying an earlier requested
    // value would undo the game's later legitimate texture assignment.
    entries_[index]={effect,data,id,requested,global};return mapped;
}
void EngineTextureScope::global(int id,const void* texture) {
    // Exact implementation ignores id=0 and nullptr. Do not replace a journal
    // entry with nullptr, since replaying nullptr cannot restore the binding.
    const auto* mapped=(id && texture)?route(nullptr,nullptr,id,texture,true):texture;
    try{api_.global(id,mapped);}catch(...){fail("TEXTURE_GLOBAL_EXCEPTION");throw;}
}
void EngineTextureScope::instance(const void* effect,void* data,int id,const void* texture,bool force,bool unbind) {
    const auto* mapped=id?route(effect,data,id,texture,false):texture;
    try{api_.instance(effect,data,id,mapped,force,unbind);}catch(...){fail("TEXTURE_INSTANCE_EXCEPTION");throw;}
}
bool EngineTextureScope::finish() noexcept {
    Errno preserve;
    if(!active_)return !failed_;
    try{if(!owner())return false;}catch(...){return false;}
    active_=false;bool restored=true;
    // These are ORIGINAL functions, not the interposed hook entry points.
    // A null instance texture is a valid assignment; global nullptr was ignored.
    while(count_){const auto e=entries_[--count_];
        try{if(e.global)api_.global(e.id,e.requested);
            else api_.instance(e.effect,e.data,e.id,e.requested,true,true);
        }catch(...){restored=false;}
    }
    if(!restored)fail("TEXTURE_RESTORE_EXCEPTION");
    return !failed_;
}
EngineTextureScope::~EngineTextureScope(){finish();}
}
