#pragma once
// BUTTONS (user: "a full control map so people can change the buttons"): per layout (ON FOOT / HORSE) every action of the VR
// layout (VrControls.h Mapper) can be put on any of the six Touch buttons A B X Y L3 R3. Implemented as a permutation of the PHYSICAL
// Touch buttons into the LOGICAL ones the Mapper already understands (logical A = the jump on foot, logical B = reload / interact, ...),
// applied before the Mapper in the Foot / Mounted layouts only (menus, cutscenes and unknown contexts keep the stock pad). Every special
// rule keyed on a logical button (manual reload on B, the lasso face remaps, the teleport walk on L3, LT+R3 Dead Eye) keeps working on
// whatever physical button now carries it. Assigning a button that another action uses swaps the two (always a permutation).
// Stored in rdrvr/controls.cfg ("foot.A=B" ...: action key = physical button); malformed lines are skipped.
#include "VrControls.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace rdrvr::vr::controlmap {
inline constexpr unsigned buttonCount=6;
inline constexpr std::array<std::uint32_t,buttonCount> buttonBits{controls::bit::A,controls::bit::B,controls::bit::X,controls::bit::Y,controls::bit::L3,controls::bit::R3};
inline constexpr std::array<const char*,buttonCount> buttonNames{"A","B","X","Y","L3","R3"};
enum Layout : unsigned {Foot,Horse,LayoutCount};
inline constexpr std::array<const char*,LayoutCount> layoutCfg{"foot","horse"};
// The action each LOGICAL button means in that layout (the VrControls.h Mapper).
inline constexpr std::array<std::array<const char*,buttonCount>,LayoutCount> actionNames{{
    {"JUMP","RELOAD / INTERACT","WHISTLE","USE / MOUNT","SPRINT","CROUCH"},
    {"SPUR / GALLOP","JUMP","X BUTTON","DISMOUNT / USE","L3 - UNUSED","DEAD EYE (LT +)"}}};
// physical[layout][logical] = the physical button index that performs logical button `logical`
struct Map {std::array<std::array<std::uint8_t,buttonCount>,LayoutCount> physical{{{0,1,2,3,4,5},{0,1,2,3,4,5}}};};
inline std::atomic<std::uint64_t> packed{0}; // published Map (6 x 2 x 3 bits); 0 = identity (never valid otherwise: logical 0 -> physical 0 ...)
inline constexpr std::uint64_t pack(const Map& m) noexcept {std::uint64_t v=0;for(unsigned l=0;l<LayoutCount;++l)for(unsigned b=0;b<buttonCount;++b)v|=std::uint64_t(m.physical[l][b]&7u)<<((l*buttonCount+b)*3);return v;}
inline constexpr Map unpack(std::uint64_t v) noexcept {Map m{};for(unsigned l=0;l<LayoutCount;++l)for(unsigned b=0;b<buttonCount;++b)m.physical[l][b]=std::uint8_t((v>>((l*buttonCount+b)*3))&7u);return m;}
inline constexpr bool valid(const Map& m) noexcept {
    for(unsigned l=0;l<LayoutCount;++l){unsigned seen=0;for(unsigned b=0;b<buttonCount;++b){if(m.physical[l][b]>=buttonCount||(seen>>m.physical[l][b])&1u)return false;seen|=1u<<m.physical[l][b];}}
    return true;}
inline Map current() noexcept {const auto v=packed.load(std::memory_order_acquire);return v?unpack(v):Map{};}
inline std::atomic<std::uint64_t> remappedFrames{0},edits{0},saves{0},saveFailures{0};inline std::atomic<bool> loaded{false},loadOk{false};
// Input thread (GameXr controlsPadFilter), Foot / Mounted only: physical Touch buttons -> logical buttons.
inline std::uint32_t apply(std::uint32_t buttons,bool mounted) noexcept {
    const auto v=packed.load(std::memory_order_relaxed);if(!v)return buttons;
    const auto m=unpack(v);const auto& p=m.physical[mounted?Horse:Foot];
    std::uint32_t out=buttons;for(auto bb:buttonBits)out&=~bb;
    bool changed=false;
    for(unsigned logical=0;logical<buttonCount;++logical){if(p[logical]!=logical)changed=true;if(buttons&buttonBits[p[logical]])out|=buttonBits[logical];}
    if(changed)remappedFrames.fetch_add(1,std::memory_order_relaxed);
    return out;
}
// ---- storage (rdrvr/controls.cfg) ----
inline std::mutex fileMutex;inline std::string path;
inline void configurePath(std::string p) noexcept {std::lock_guard lock(fileMutex);path=std::move(p);}
inline bool save(const Map& m) noexcept {
    std::lock_guard lock(fileMutex);if(path.empty()){saveFailures.fetch_add(1);return false;}
    const auto tmp=path+".tmp";FILE* f=std::fopen(tmp.c_str(),"w");if(!f){saveFailures.fetch_add(1);return false;}
    std::fprintf(f,"version=1\n");
    for(unsigned l=0;l<LayoutCount;++l)for(unsigned b=0;b<buttonCount;++b)std::fprintf(f,"%s.%s=%s\n",layoutCfg[l],buttonNames[b],buttonNames[m.physical[l][b]]);
    const bool ok=std::fflush(f)==0&&std::fclose(f)==0&&std::rename(tmp.c_str(),path.c_str())==0;
    (ok?saves:saveFailures).fetch_add(1);return ok;
}
inline void loadOnce() noexcept {
    if(loaded.exchange(true))return;
    Map m{};std::string p;{std::lock_guard lock(fileMutex);p=path;}
    if(FILE* f=p.empty()?nullptr:std::fopen(p.c_str(),"r")){
        char line[96];
        while(std::fgets(line,sizeof line,f)){
            char key[32]{},val[8]{};if(std::sscanf(line," %31[^=]=%7s",key,val)!=2)continue;
            const char* dot=std::strchr(key,'.');if(!dot)continue;
            unsigned l=0;while(l<LayoutCount&&std::strncmp(key,layoutCfg[l],std::size_t(dot-key))!=0)++l;
            unsigned b=0;while(b<buttonCount&&std::strcmp(dot+1,buttonNames[b])!=0)++b;
            unsigned v=0;while(v<buttonCount&&std::strcmp(val,buttonNames[v])!=0)++v;
            if(l<LayoutCount&&b<buttonCount&&v<buttonCount)m.physical[l][b]=std::uint8_t(v);
        }
        std::fclose(f);
        if(!valid(m))m=Map{};else loadOk.store(true);
    }
    const auto v=pack(m);packed.store(v==pack(Map{})?0:v,std::memory_order_release);
}
// Menu (XR thread): row = layout * 6 + logical; step +1 / -1 cycles that action's physical button (the action holding the new button swaps).
inline void step(unsigned row,int dir) noexcept {
    if(row>=LayoutCount*buttonCount||!dir)return;
    loadOnce();auto m=current();const unsigned l=row/buttonCount,logical=row%buttonCount;
    const unsigned now=m.physical[l][logical],next=(now+buttonCount+(dir>0?1u:buttonCount-1u))%buttonCount;
    for(unsigned o=0;o<buttonCount;++o)if(m.physical[l][o]==next){m.physical[l][o]=std::uint8_t(now);break;}
    m.physical[l][logical]=std::uint8_t(next);
    const auto v=pack(m);packed.store(v==pack(Map{})?0:v,std::memory_order_release);edits.fetch_add(1);save(m);
}
inline void resetDefaults() noexcept {loadOnce();packed.store(0,std::memory_order_release);edits.fetch_add(1);save(Map{});}
inline std::string report() {
    const auto m=current();std::string o="{\"loadOk\":";o+=loadOk.load()?"true":"false";o+=",\"remapped\":";o+=packed.load()?"true":"false";
    o+=",\"frames\":"+std::to_string(remappedFrames.load())+",\"edits\":"+std::to_string(edits.load())+",\"saves\":"+std::to_string(saves.load())+",\"saveFailures\":"+std::to_string(saveFailures.load());
    for(unsigned l=0;l<LayoutCount;++l){o+=",\"";o+=layoutCfg[l];o+="\":\"";for(unsigned b=0;b<buttonCount;++b){o+=buttonNames[m.physical[l][b]];if(b+1<buttonCount)o+=' ';}o+='"';}
    return o+"}";
}
} // namespace rdrvr::vr::controlmap
