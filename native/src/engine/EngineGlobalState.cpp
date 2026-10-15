#include "diagnostics/FrameTimings.h"
#include "diagnostics/GuardedReadVector.h"
#include "EngineGlobalState.h"
#include <cerrno>
#include <cstring>
#include <sstream>
namespace rdrvr::engine {
namespace {struct Errno {int value{errno};~Errno(){errno=value;}};}
bool EngineGlobalState::owner()const{return api_&&owner_==std::this_thread::get_id()&&api_.ownsThread();}
bool EngineGlobalState::signatures(int count,std::array<Signature,240>& out)const{
    if(count<0||count>240)return false;if(!count)return true;
    // The pinned ELF owns contiguous descriptor/mapping tables. Batch guarded
    // copies, never raw dereference. Restore takes a fresh complete snapshot
    // and checks every descriptor/mapping before ANY restore write.
    std::array<unsigned char,72*240> descriptors{};
    std::array<unsigned char,16*240> mappings{};
    if(!api_.read(api_.user,api_.globals,descriptors.data(),(count-1)*72+2)||
       !api_.read(api_.user,api_.buffers,mappings.data(),(count-1)*16+12))return false;
    for(int i=0;i<count;++i){auto& s=out[i];s.type=descriptors[i*72];s.count=descriptors[i*72+1];
        std::memcpy(&s.buffer,mappings.data()+i*16,8);std::memcpy(&s.offset,mappings.data()+i*16+8,4);}
    return true;
}
bool EngineGlobalState::header(std::int32_t& count){
    std::uintptr_t command{};
    status_="COMMAND_BUFFER";
    if(!api_.read(api_.user,api_.commandBuffer,&command,sizeof(command))||command)return false;
    status_="GLOBAL_COUNT";
    return api_.read(api_.user,api_.globals+0x4380,&count,sizeof(count))&&count>=0&&count<=240;
}
bool EngineGlobalState::inspect(Entry& e,const Signature& signature,bool capture,bool readValue){
    const std::array<unsigned char,2> desc{signature.type,signature.count};
    const auto& map=signature;unsigned char unit{};
    inspectedId_=e.id;inspectedType_=inspectedCount_=inspectedUnit_=inspectedSize_=0;inspectedOffset_=0;status_="DESCRIPTOR";
    inspectedType_=desc[0];inspectedCount_=desc[1];
    if(desc[0]>=16||desc[0]==6||!desc[1])return false;
    status_="TYPE_SIZE";
    unit=typeSizes_[desc[0]];
    inspectedUnit_=unit;if(!unit||unit%4)return false;
    status_="BUFFER_MAPPING";
    inspectedOffset_=map.offset;if(!map.buffer||map.offset<0)return false;
    BufferView* view=nullptr;
    for(unsigned i=0;i<bufferCount_;++i)if(buffers_[i].address==map.buffer){view=&buffers_[i];break;}
    if(!view){
        if(bufferCount_==buffers_.size())return false;
        std::array<std::uintptr_t,4> buffer{};std::uint32_t size{};
        status_="CPU_BUFFER";
        if(!api_.read(api_.user,map.buffer,buffer.data(),sizeof(buffer))||!buffer[0]||!buffer[1]||(buffer[3]&&buffer[3]!=buffer[0]))return false;
        status_="BUFFER_SIZE";
        if(buffer[1]>UINTPTR_MAX-0x1c||!api_.read(api_.user,buffer[1]+0x18,&size,sizeof(size)))return false;
        buffers_[bufferCount_]={map.buffer,buffer[0],buffer[1],buffer[3],size};view=&buffers_[bufferCount_++];
    }
    const auto size=view->size;
    inspectedSize_=size;if(!size||size>65536)return false;
    const auto length=unsigned(unit)*desc[1];status_="BUFFER_RANGE";
    if(unsigned(map.offset)>size||length>size-unsigned(map.offset))return false;
    if(!capture)return e.type==desc[0]&&e.count==desc[1]&&e.buffer==map.buffer&&e.offset==map.offset&&
        e.data==view->data&&e.layout==view->layout&&e.bytes==size&&e.value.size()*sizeof(float)==length;
    e.type=desc[0];e.count=desc[1];e.buffer=map.buffer;e.offset=map.offset;e.data=view->data;e.layout=view->layout;e.bytes=size;
    e.value.resize(length/sizeof(float));
    status_="BUFFER_READ";
    if(e.data>UINTPTR_MAX-size)return false;
    return !readValue||api_.read(api_.user,e.data+unsigned(e.offset),e.value.data(),length);
}
// Every captured value in one vectored guarded read (all ranges were validated by
// inspect()). On failure, the per-entry reads run again in order, and the report state
// (failing entry, numeric/sampler counts before it) is exactly that of the former
// one-read-per-entry capture.
bool EngineGlobalState::readValues(){
    if(entries_.empty())return true;
    std::vector<diagnostics::ReadSegment> segments;segments.reserve(entries_.size());
    for(auto& e:entries_)segments.push_back({e.data+unsigned(e.offset),e.value.data(),e.value.size()*sizeof(float)});
    if(diagnostics::guardedReadVector.read(api_.read,api_.user,segments.data(),segments.size()))return true;
    for(std::size_t k=0;k<entries_.size();++k){
        auto& e=entries_[k];
        if(api_.read(api_.user,e.data+unsigned(e.offset),e.value.data(),e.value.size()*sizeof(float)))continue;
        inspectedId_=e.id;inspectedType_=unsigned(e.type);inspectedCount_=unsigned(e.count);inspectedUnit_=typeSizes_[unsigned(e.type)&15];
        inspectedOffset_=e.offset;inspectedSize_=e.bytes;status_="BUFFER_READ";
        unsigned samplers{};for(int i=0;i<e.id-1;++i)if(signatures_[i].type==11&&signatures_[i].buffer)++samplers;
        samplerCount_=samplers;entries_.resize(k);return false;
    }
    return true;
}
bool EngineGlobalState::capture() noexcept {
    diagnostics::WallScope timing(diagnostics::frameTimings.globalCapture);
    Errno preserve;
    try{
        status_="OWNER_OR_ACTIVE";inspectedId_=0;inspectedType_=inspectedCount_=inspectedUnit_=inspectedSize_=0;inspectedOffset_=0;
        if(!owner()||active_||failed_||!header(count_))return false;
        entries_.clear();entries_.reserve(count_);std::size_t bytes{};samplerCount_=0;
        bufferCount_=0;status_="SIGNATURE";if(!signatures(count_,signatures_))return false;
        status_="TYPE_SIZE";if(!api_.read(api_.user,api_.typeSizes,typeSizes_.data(),typeSizes_.size()))return false;
        for(int i=0;i<count_;++i){
            inspectedId_=i+1;auto& s=signatures_[i];
            if(s.type==6||!s.buffer)continue; // textures and unused numeric globals
            if(s.type==11){
                // Pinned grcParameter::Load consumes/discards samplerstate data
                // (0x1ae57d8); it has no numeric value at descriptor +0x30.
                // A non-null GlobalToBuffer slot does not make it numeric.
                // Retain its signature for the all-before-write restore check.
                unsigned char unit{};status_="SAMPLER_TYPE_SIZE";
                inspectedType_=s.type;inspectedCount_=s.count;inspectedUnit_=inspectedSize_=0;inspectedOffset_=0;
                unit=typeSizes_[s.type];if(unit)return false;
                ++samplerCount_;continue;
            }
            Entry entry{};entry.id=i+1;if(!inspect(entry,s,true,false))return false;
            bytes+=entry.value.size()*sizeof(float);if(bytes>1024*1024)return false;
            entries_.push_back(std::move(entry));
        }
        if(!readValues())return false;
        active_=true;status_="READY";return true;
    }catch(...){return false;}
}
std::string EngineGlobalState::report()const {
    std::ostringstream o;o<<"{\"state\":\""<<status_<<"\",\"globals\":"<<count_<<",\"id\":"<<inspectedId_
        <<",\"type\":"<<inspectedType_<<",\"count\":"<<inspectedCount_<<",\"unit\":"<<inspectedUnit_
        <<",\"offset\":"<<inspectedOffset_<<",\"bufferBytes\":"<<inspectedSize_
        <<",\"numericEntries\":"<<entries_.size()<<",\"samplerEntries\":"<<samplerCount_<<'}';return o.str();
}
bool EngineGlobalState::restore() noexcept {
    Errno preserve;if(!active_)return !failed_;
    diagnostics::WallScope timing(diagnostics::frameTimings.globalRestore);
    try{
        if(!owner())return false;
        std::int32_t count{};
        // Validate the entire set before writing: shader reload/reallocation is
        // not permission to write old values into a newly reused object.
        bool valid=header(count)&&count==count_;
        std::array<Signature,240> current{};valid=signatures(count_,current)&&valid;
        std::array<unsigned char,16> sizes{};
        valid=api_.read(api_.user,api_.typeSizes,sizes.data(),sizes.size())&&valid;
        valid=(sizes==typeSizes_)&&valid;bufferCount_=0;
        for(int i=0;i<count_;++i)valid=(current[i]==signatures_[i])&&valid;
        for(auto& e:entries_)valid=inspect(e,current[e.id-1],false)&&valid;
        active_=false;if(!valid){failed_=true;return false;}
        for(const auto& e:entries_){
            try{api_.set(e.id,e.value.data(),e.count,e.type);}catch(...){failed_=true;}
        }
        entries_.clear();return !failed_;
    }catch(...){active_=false;failed_=true;return false;}
}
}
