// Host C++20 regression; fake native input ABI, no headset needed.
#include "engine/NativePadDriver.h"
#include <cassert>
#include <cstring>
#include <iostream>
using namespace rdrvr::engine;
using namespace rdrvr::runtime;
struct Pad {std::uintptr_t table=0x2000;std::array<unsigned char,0x380> bytes{};} own,external;
struct System {
    std::uintptr_t table=0x1000;std::byte prefix[0x20]{};Pad* slots[4]{};
    std::byte middle[0x20]{};Pad* override{};void* touchOverride{};
} sys;
static_assert(offsetof(System,slots)==0x28&&offsetof(System,override)==0x68);
std::array<std::byte,0x50> info{};
bool registered{},steal{},badFilter{},hideReads{};
unsigned char& byte(Pad& p,std::size_t n){return reinterpret_cast<unsigned char*>(&p)[n];}
bool read(void*,std::uintptr_t p,void* out,std::size_t n){
    if(hideReads&&p==reinterpret_cast<std::uintptr_t>(&own)+0x360)return false;
    for(auto r:{std::pair<const void*,std::size_t>{&sys,sizeof(sys)},{&own,sizeof(own)},
        {&external,sizeof(external)},{info.data(),info.size()}}){
        const auto b=reinterpret_cast<std::uintptr_t>(r.first);
        if(p>=b&&p-b<=r.second&&n<=r.second-(p-b)){std::memcpy(out,reinterpret_cast<void*>(p),n);return true;}}
    return false;
}
void* system(){return &sys;}
void* pad(const void*,int i){return i==0&&sys.override?sys.override:sys.slots[i];}
void* getInfo(void*,int){return registered?info.data():nullptr;}
void connect(void*,int,int,int,const char*){sys.slots[1]=&own;registered=true;std::memcpy(info.data()+0x40,&sys.slots[1],sizeof(void*));}
void activate(void*,void*){}
void touch(void*,bool){}
void overridePad(void*,void* p){sys.override=static_cast<Pad*>(p);}
void button(void* p,int,int key,bool down){assert(p==&own);if(key==104||key==105)byte(own,0x360+key-104)=down;}
void axis(void* p,int,int,float,bool&){assert(p==&own);}
void filter(void*,const ControllerSample*,std::uint64_t,PadState& state){if(badFilter){state.triggers[1]=2.f;}}
void update(){
    for(auto* p:sys.slots)if(p){
        for(int i=0;i<2;++i){byte(*p,0xfd+i)=byte(*p,0x360+i)&&!byte(*p,0x4c+i);byte(*p,0x4c+i)=byte(*p,0x360+i);}
        byte(*p,0x300)=1;}
    if(steal)sys.override=&external;
}
int main(){
    ControllerMailbox box;NativePadDriver driver(box);sys.slots[0]=&external;
    driver.configure({read,system,pad,getInfo,connect,activate,touch,{button,axis},0x1000,0x2000,overridePad});
    driver.setPadFilter(filter);
    ControllerSample sample{};sample.epoch=1;sample.active=sample.focused=true;
    std::uint64_t now=1'000'000'000;
    auto tick=[&](float rt){now+=30'000'000;sample.publishedNs=now;++sample.sequence;sample.triggers[1]=rt;
        assert(box.publish(sample));errno=42;driver.beforeUpdate(&sys,now);update();driver.afterUpdate(&sys,now);assert(errno==42);};
    tick(0);tick(0);tick(1);
    auto json=driver.deliveryReport();
    assert(json.find("\"selected\":1,\"readable\":1")!=std::string::npos);
    assert(json.find("\"rawRt\":1")!=std::string::npos&&json.find("\"pressedRt\":1")!=std::string::npos);
    for(int i=0;i<100;++i){tick(1);assert(byte(own,0x4d)==1&&byte(own,0xfe)==0);}
    json=driver.deliveryReport();assert(json.find("\"total\":3,")!=std::string::npos); // no per-frame logging
    tick(0);assert(byte(own,0x4d)==0);
    badFilter=true;tick(1);assert(driver.filterRejected()==1&&byte(own,0x4d)==1); // fallback remains intact
    json=driver.deliveryReport();assert(json.find("\"requestedRt\":2.000,\"submittedRt\":1.000")!=std::string::npos);
    badFilter=false;tick(0);hideReads=true;tick(1);hideReads=false;
    json=driver.deliveryReport();assert(json.find("\"readable\":0,\"rawLt\":-1")!=std::string::npos);
    tick(0);steal=true;tick(1);
    json=driver.deliveryReport();assert(json.find("PAD_ROUTE_REJECTED")!=std::string::npos&&json.find("\"selected\":0,\"readable\":1")!=std::string::npos);
    assert(sys.override==&external); // diagnostics never steal/repair ownership
    steal=false;sys.override=nullptr;tick(0);
    for(int i=0;i<80;++i)tick(i%2?0:1);
    json=driver.deliveryReport();std::size_t pos=0;unsigned rows=0;
    while((pos=json.find("\"ns\":",pos))!=std::string::npos){++rows;++pos;}assert(rows==64);
    std::cout<<json<<'\n';
}
