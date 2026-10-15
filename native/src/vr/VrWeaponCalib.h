#pragma once
// WEAPON CALIBRATION: user offsets / rotations of the VR-held weapon, per weapon CLASS
// (pistol, melee, ...) or per weapon TYPE (engine type id, e.g. one knife), stored in rdrvr/weapons.cfg next to vr.cfg with the
// same persistence contract as VrSettings (missing = first run, malformed = defaults + SAVE FAILED, atomic tmp+rename+fsync save,
// edits apply in memory even when the save fails). Applied LIVE by VrWeapon.h on top of the automatic placement:
//   rotation about the holding controller's grip point (yaw about aim up, then pitch about aim right, then roll about the barrel),
//   then the offset along aim right / up / forward. Units: millimetres and whole degrees (menu steps 5 mm / 2 deg).
// Threads: the menu edits on the XR thread; VrWeapon reads on the pose thread (any engine thread) - one short mutex per read.
// The "current weapon" (class, type id, name, holding hand) is published by VrWeapon on the engine main thread.
#include "VrSettings.h"
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

namespace rdrvr::vr::calib {

inline constexpr unsigned classCount=14; // engine::WeaponClass Unknown..Cannon (VrWeapon.h classKey order)
inline constexpr std::array<const char*,classCount> classKeys{"unknown","pistol","dualpistol","rifle","repeater","sniper","shotgun","thrown",
    "explosive","bow","melee","lasso","turret","cannon"};
inline constexpr std::array<const char*,classCount> classLabels{"UNKNOWN","PISTOL","DUAL PISTOL","RIFLE","REPEATER","SNIPER","SHOTGUN",
    "THROWN","EXPLOSIVE","BOW","MELEE","LASSO","TURRET","CANNON"};
enum Field : unsigned {OffsetX,OffsetY,OffsetZ,Pitch,Yaw,Roll,FieldCount};
inline constexpr std::array<const char*,FieldCount> fieldKeys{"x","y","z","pitch","yaw","roll"};
inline constexpr int offsetStepMm=5,angleStepDeg=2,offsetLimitMm=250,pitchYawLimitDeg=90,rollLimitDeg=180;
inline constexpr int fieldLimit(unsigned f) noexcept {return f<Pitch?offsetLimitMm:f==Roll?rollLimitDeg:pitchYawLimitDeg;}
inline constexpr int fieldStep(unsigned f) noexcept {return f<Pitch?offsetStepMm:angleStepDeg;}
// the WEAPON scope is per MODEL. The engine type id is an index inside its class enum (pistol 4 = Cattleman, rifle 4 = another gun):
// the stored "type.<key>" = class*1000 + id; -1 = no own entry (class only). Old "type.<id>" lines (< 1000) stay in the file, unused.
inline constexpr int typeKey(int kind,int id) noexcept {return kind>=0&&unsigned(kind)<classCount&&id>=0&&id<1000?kind*1000+id:-1;}

struct Entry {
    std::array<std::int16_t,FieldCount> v{}; // mm, mm, mm, deg, deg, deg
    constexpr bool zero() const noexcept {for(auto x:v)if(x)return false;return true;}
    constexpr bool operator==(const Entry&) const noexcept = default;
    float metres(unsigned f) const noexcept {return float(v[f])/1000.f;}
    float degrees(unsigned f) const noexcept {return float(v[f]);}
};
enum Source : unsigned {SourceNone,SourceClass,SourceType};
struct Effective {Entry entry{};Source source{SourceNone};};

// ---- what the menu shows: the weapon in a VR hand (or the last one held) ----
struct Current {int kind{-1};int typeId{};int hand{-1};bool held{};char name[32]{};};
inline std::mutex currentMutex;inline Current current{};
inline std::atomic<int> menuHoldHand{-1}; // VrHolsters: the hand that holds a weapon (the menu close guard ignores its grip)
inline void publishCurrent(int kind,int typeId,int hand,const char* name) noexcept {
    std::lock_guard lock(currentMutex);
    current.kind=kind;current.typeId=typeId;current.hand=hand;current.held=true;
    if(name&&*name){std::snprintf(current.name,sizeof current.name,"%s",name);}
}
inline void publishReleased() noexcept {std::lock_guard lock(currentMutex);current.held=false;current.hand=-1;}
inline Current currentWeapon() noexcept {std::lock_guard lock(currentMutex);return current;}
// "base_pistol_REVOLVER_Cattleman" -> "CATTLEMAN" (text after the last '_', upper case, panel glyphs, <= 16).
inline void shortName(const char* type,char* out,std::size_t size) noexcept {
    if(!size)return;out[0]=0;if(!type||!*type)return;
    const char* s=std::strrchr(type,'_');s=s&&s[1]?s+1:type;
    std::size_t n=0;
    for(;*s&&n+1<size&&n<16;++s){char c=*s;if(c>='a'&&c<='z')c=char(c-'a'+'A');
        if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c==' '||c=='.'||c=='-'))c='?';out[n++]=c;}
    out[n]=0;
}

namespace detail {
inline bool parseInt(std::string_view s,int& out) noexcept {
    if(s.empty())return false;const char* b=s.data();const char* e=b+s.size();if(*b=='+')++b;
    const auto r=std::from_chars(b,e,out);return r.ec==std::errc{}&&r.ptr==e;
}
inline bool parseEntry(std::string_view value,Entry& out) noexcept {
    Entry e{};unsigned n=0;
    while(!value.empty()){
        while(!value.empty()&&value.front()==' ')value.remove_prefix(1);
        if(value.empty())break;
        const auto sp=value.find(' ');const auto tok=value.substr(0,sp);value=sp==value.npos?std::string_view{}:value.substr(sp+1);
        int x{};if(n>=FieldCount||!parseInt(tok,x)||x<-fieldLimit(n)||x>fieldLimit(n))return false;
        e.v[n++]=std::int16_t(x);
    }
    if(n!=FieldCount)return false;
    out=e;return true;
}
} // namespace detail

class Store {
public:
    static constexpr unsigned maxTypes=64;static constexpr std::size_t maxFileBytes=8192;
    bool configurePath(std::string path) noexcept {
        ::rdrvr::vr::detail::PreserveErrno preserve;
        try{
            std::lock_guard lock(mutex_);
            if(loaded_||path.empty()||path.find('\0')!=std::string::npos)return false;
            const auto file=::rdrvr::vr::detail::utf8Path(path);
            if(!file.is_absolute()||file.filename()!="weapons.cfg")return false;
            path_=std::move(path);return true;
        }catch(...){return false;}
    }
    // Lazy, cheap when done: weapons.cfg next to the configured vr.cfg (TraceEntry configures vr.cfg before any hook runs).
    // Host / no path: loads defaults once vr settings are loaded (session only, persistenceOk false).
    void ensureLoaded() noexcept {
        if(loadedFlag_.load(std::memory_order_acquire))return;
        if(!::rdrvr::vr::vrSettings.loaded())return;
        try{
            bool configured{};{std::lock_guard lock(mutex_);configured=!path_.empty();}
            if(!configured){const auto vr=::rdrvr::vr::vrSettings.path();
                if(!vr.empty()){const auto slash=vr.find_last_of('/');if(slash!=std::string::npos)configurePath(vr.substr(0,slash+1)+"weapons.cfg");}}
        }catch(...){}
        loadOnce();
    }
    bool loadOnce() noexcept {
        ::rdrvr::vr::detail::PreserveErrno preserve;std::lock_guard lock(mutex_);
        if(loaded_)return loadResult_;
        loaded_=true;loadResult_=false;classes_={};types_={};typeIds_={};typeCount_=0;
        try{
            if(!path_.empty()){
                errno=0;auto* file=::rdrvr::vr::detail::openFile(::rdrvr::vr::detail::utf8Path(path_),false);
                if(!file)loadResult_=errno==ENOENT;
                else{
                    std::string data(maxFileBytes+1,'\0');
                    const auto size=std::fread(data.data(),1,data.size(),file);
                    const bool good=!std::ferror(file)&&size<=maxFileBytes;
                    const bool closed=std::fclose(file)==0;
                    loadResult_=good&&closed&&parseLocked(std::string_view(data.data(),size));
                    if(!loadResult_){classes_={};types_={};typeIds_={};typeCount_=0;}
                }
            }
        }catch(...){loadResult_=false;classes_={};types_={};typeIds_={};typeCount_=0;}
        persistenceOk_.store(loadResult_,std::memory_order_release);
        publishLocked();
        loadedFlag_.store(true,std::memory_order_release);
        return loadResult_;
    }
    bool loaded() const noexcept {return loadedFlag_.load(std::memory_order_acquire);}
    bool persistenceOk() const noexcept {return persistenceOk_.load(std::memory_order_acquire);}
    std::uint32_t saves() const noexcept {return saves_.load(std::memory_order_relaxed);}
    std::uint32_t saveFailures() const noexcept {return saveFailures_.load(std::memory_order_relaxed);}
    std::uint32_t generation() const noexcept {return generation_.load(std::memory_order_acquire);}
    std::uint32_t edits() const noexcept {return edits_.load(std::memory_order_relaxed);}
    // Pose thread: the type entry if the weapon has one, else its class entry.
    // Readers (pose thread, menu text) use the published copy under its own short mutex: a save (fsync) never blocks them.
    Effective effective(int kind,int typeId) const noexcept {
        std::lock_guard lock(pubMutex_);Effective e{};
        if(const int t=typeId>=0?findPub(typeId):-1;t>=0){e.entry=pub_.types[unsigned(t)];e.source=SourceType;return e;}
        if(kind>=0&&unsigned(kind)<classCount&&!pub_.classes[unsigned(kind)].zero()){e.entry=pub_.classes[unsigned(kind)];e.source=SourceClass;}
        return e;
    }
    bool hasType(int typeId) const noexcept {std::lock_guard lock(pubMutex_);return typeId>=0&&findPub(typeId)>=0;}
    // Menu: the entry an edit in this scope changes (WEAPON scope without its own entry yet: the class entry it will copy).
    Entry entryFor(int kind,int typeId,bool scopeType) const noexcept {
        std::lock_guard lock(pubMutex_);
        if(scopeType)if(const int t=findPub(typeId);t>=0)return pub_.types[unsigned(t)];
        return kind>=0&&unsigned(kind)<classCount?pub_.classes[unsigned(kind)]:Entry{};
    }
    // Menu (XR thread). scopeType: edit the weapon's own entry (created from the class entry on the first edit).
    // delta = steps (+1/-1); zero=true sets the field to 0. Returns false when nothing changed or the save failed.
    bool edit(int kind,int typeId,bool scopeType,unsigned field,int delta,bool zero=false) noexcept {
        ::rdrvr::vr::detail::PreserveErrno preserve;std::lock_guard lock(mutex_);
        if(!loaded_||field>=FieldCount||kind<0||unsigned(kind)>=classCount||(scopeType&&typeId<0))return false; // No model key
        Entry* e=nullptr;
        if(scopeType){int t=findTypeLocked(typeId);
            if(t<0){if(typeCount_>=maxTypes)return false;t=int(typeCount_++);typeIds_[unsigned(t)]=std::int16_t(typeId);types_[unsigned(t)]=classes_[unsigned(kind)];}
            e=&types_[unsigned(t)];}
        else e=&classes_[unsigned(kind)];
        const int lim=fieldLimit(field);int v=e->v[field];
        const int next=zero?0:std::clamp(v+delta*fieldStep(field),-lim,lim);
        if(next==v)return false;
        e->v[field]=std::int16_t(next);edits_.fetch_add(1,std::memory_order_relaxed);
        publishLocked(); // live: the next pose uses it, before the file is written
        return saveLocked();
    }
    // RESET: the class entry (scopeType false) or the weapon's own entry is removed (the weapon falls back to its class).
    bool reset(int kind,int typeId,bool scopeType) noexcept {
        ::rdrvr::vr::detail::PreserveErrno preserve;std::lock_guard lock(mutex_);
        if(!loaded_||kind<0||unsigned(kind)>=classCount)return false;
        if(scopeType){const int t=findTypeLocked(typeId);if(t<0)return false;
            for(unsigned i=unsigned(t);i+1<typeCount_;++i){types_[i]=types_[i+1];typeIds_[i]=typeIds_[i+1];}
            --typeCount_;types_[typeCount_]={};typeIds_[typeCount_]=0;}
        else{if(classes_[unsigned(kind)].zero())return false;classes_[unsigned(kind)]={};}
        resets_.fetch_add(1,std::memory_order_relaxed);publishLocked();
        return saveLocked();
    }
    std::string report() const {
        std::lock_guard lock(mutex_);char b[160];
        std::string out="{\"loaded\":";out+=loaded_?"true":"false";
        out+=",\"persistenceOk\":";out+=persistenceOk_.load()?"true":"false";
        out+=",\"path\":\"";out+=path_.empty()?"":"weapons.cfg";out+='"';
        std::snprintf(b,sizeof b,",\"saves\":%u,\"saveFailures\":%u,\"edits\":%u,\"resets\":%u,\"types\":%u,\"entries\":{",saves_.load(),saveFailures_.load(),
            edits_.load(),resets_.load(),typeCount_);out+=b;
        bool first=true;
        const auto put=[&](const std::string& key,const Entry& e){
            std::snprintf(b,sizeof b,"%s\"%s\":[%d,%d,%d,%d,%d,%d]",first?"":",",key.c_str(),e.v[0],e.v[1],e.v[2],e.v[3],e.v[4],e.v[5]);out+=b;first=false;};
        for(unsigned k=0;k<classCount;++k)if(!classes_[k].zero())put(std::string("class.")+classKeys[k],classes_[k]);
        for(unsigned t=0;t<typeCount_;++t)put("type."+std::to_string(typeIds_[t]),types_[t]);
        out+="}}";return out;
    }
    // Host tests only.
    void resetForTest() noexcept {std::lock_guard lock(mutex_);path_.clear();loaded_=loadResult_=false;classes_={};types_={};typeIds_={};typeCount_=0;publishLocked();
        loadedFlag_.store(false);persistenceOk_.store(true);saves_.store(0);saveFailures_.store(0);edits_.store(0);resets_.store(0);}
private:
    mutable std::mutex mutex_;std::string path_;bool loaded_{},loadResult_{};
    std::array<Entry,classCount> classes_{};std::array<Entry,maxTypes> types_{};std::array<std::int16_t,maxTypes> typeIds_{};unsigned typeCount_{};
    std::atomic<bool> loadedFlag_{false},persistenceOk_{true};
    std::atomic<std::uint32_t> saves_{0},saveFailures_{0},edits_{0},resets_{0},generation_{0};
    int findTypeLocked(int typeId) const noexcept {for(unsigned t=0;t<typeCount_;++t)if(typeIds_[t]==typeId)return int(t);return -1;}
    struct Published {std::array<Entry,classCount> classes{};std::array<Entry,maxTypes> types{};std::array<std::int16_t,maxTypes> ids{};unsigned count{};};
    mutable std::mutex pubMutex_;Published pub_{};
    int findPub(int typeId) const noexcept {for(unsigned t=0;t<pub_.count;++t)if(pub_.ids[t]==typeId)return int(t);return -1;}
    void publishLocked() noexcept { // caller holds mutex_
        Published p{};p.classes=classes_;p.types=types_;p.ids=typeIds_;p.count=typeCount_;
        {std::lock_guard lock(pubMutex_);pub_=p;}generation_.fetch_add(1,std::memory_order_acq_rel);
    }
    // weapons.cfg: version=1; "class.<key>=x y z pitch yaw roll" / "type.<id>=..." (mm, mm, mm, deg, deg, deg); '#' comments;
    // every key at most once; anything unknown or out of range rejects the whole file.
    bool parseLocked(std::string_view text) noexcept {
        bool version{};unsigned seenClass{};
        while(!text.empty()){
            const auto eol=text.find('\n');const auto line=::rdrvr::vr::detail::trim(text.substr(0,eol));
            text=eol==text.npos?std::string_view{}:text.substr(eol+1);
            if(line.empty()||line.front()=='#')continue;
            const auto eq=line.find('=');if(eq==line.npos)return false;
            const auto key=::rdrvr::vr::detail::trim(line.substr(0,eq)),value=::rdrvr::vr::detail::trim(line.substr(eq+1));
            if(key=="version"){int v{};if(version||!detail::parseInt(value,v)||v!=1)return false;version=true;continue;}
            Entry e{};if(!detail::parseEntry(value,e))return false;
            if(key.substr(0,6)=="class."){const auto name=key.substr(6);unsigned k=0;while(k<classCount&&name!=classKeys[k])++k;
                if(k==classCount||(seenClass>>k&1u))return false;seenClass|=1u<<k;classes_[k]=e;continue;}
            if(key.substr(0,5)=="type."){int id{};if(!detail::parseInt(key.substr(5),id)||id<-32768||id>32767||findTypeLocked(id)>=0||typeCount_>=maxTypes)return false;
                typeIds_[typeCount_]=std::int16_t(id);types_[typeCount_]=e;++typeCount_;continue;}
            return false;
        }
        return version;
    }
    bool saveLocked() noexcept {
        persistenceOk_.store(false,std::memory_order_release);
        if(!loaded_||path_.empty()){saveFailures_.fetch_add(1,std::memory_order_relaxed);return false;}
        std::filesystem::path temporary;std::FILE* file{};
        try{
            const auto target=::rdrvr::vr::detail::utf8Path(path_);temporary=target;temporary+=".tmp";
            file=::rdrvr::vr::detail::openFile(temporary,true);
            if(file){
                bool written=std::fprintf(file,"version=1\n")>0;
                const auto line=[&](const std::string& key,const Entry& e){
                    written=written&&std::fprintf(file,"%s=%d %d %d %d %d %d\n",key.c_str(),e.v[0],e.v[1],e.v[2],e.v[3],e.v[4],e.v[5])>0;};
                for(unsigned k=0;k<classCount;++k)if(!classes_[k].zero())line(std::string("class.")+classKeys[k],classes_[k]);
                for(unsigned t=0;t<typeCount_;++t)line("type."+std::to_string(typeIds_[t]),types_[t]);
                written=written&&::rdrvr::vr::detail::durableFlush(file);
                const bool closed=std::fclose(file)==0;file=nullptr;
                std::error_code renamed;
                if(written&&closed){std::filesystem::rename(temporary,target,renamed);
                    if(!renamed){persistenceOk_.store(true,std::memory_order_release);saves_.fetch_add(1,std::memory_order_relaxed);return true;}}
            }
        }catch(...){}
        if(file)std::fclose(file);
        if(!temporary.empty()){std::error_code ignored;std::filesystem::remove(temporary,ignored);}
        saveFailures_.fetch_add(1,std::memory_order_relaxed);return false;
    }
};
inline Store store;

// Menu scope (XR thread): false = edits go to the weapon's CLASS, true = to this weapon TYPE only.
// default THIS WEAPON ONLY (a CLASS edit made with any handgun moves every pistol-class gun, the Cattleman too).
inline std::atomic<bool> scopeType{true};

} // namespace rdrvr::vr::calib
