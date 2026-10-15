#include "GraphicsSettings.h"
#include "AntiAliasingStatus.h"
#include "HeatWarning.h" // heat01
#include "GpuBoost.h" // boost01
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string_view>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace rdrvr::runtime {
GraphicsSettings graphicsSettings;
namespace {
struct PreserveErrno {int value{errno};~PreserveErrno(){errno=value;}};
std::string_view trim(std::string_view s) {
    while(!s.empty()&&(s.front()==' '||s.front()=='\t'||s.front()=='\r'))s.remove_prefix(1);
    while(!s.empty()&&(s.back()==' '||s.back()=='\t'||s.back()=='\r'))s.remove_suffix(1);
    return s;
}
struct ParseInfo {int legacyOcclusion{-1};unsigned version{};bool s02MaskClamped{};};
bool parse(std::string_view text,GraphicsSettings::Settings& out,ParseInfo& info) {
    GraphicsSettings::Settings result;ParseInfo parsed;
    unsigned seen{},version{},antiAliasing{};bool legacyProfiler{},modeProfiler{};
    while(!text.empty()){
        const auto eol=text.find('\n');
        auto line=trim(text.substr(0,eol));
        text=eol==text.npos?std::string_view{}:text.substr(eol+1);
        if(line.empty()||line.front()=='#')continue;
        const auto eq=line.find('=');if(eq==line.npos)return false;
        auto key=trim(line.substr(0,eq)),value=trim(line.substr(eq+1));
        if(key=="s02Mask"){ // Hex or decimal, clamped (not a plain decimal: handled before the generic conversion)
            constexpr unsigned bit=1u<<28;unsigned mask{};bool clamped{};
            if(!GraphicsSettings::parseS02Mask(value,mask,clamped)||(seen&bit))return false;
            seen|=bit;result.s02Mask=mask;parsed.s02MaskClamped=clamped;continue;
        }
        unsigned number{};const auto converted=std::from_chars(value.data(),value.data()+value.size(),number);
        if(value.empty()||converted.ec!=std::errc{}||converted.ptr!=value.data()+value.size())return false;
        unsigned bit{};
        if(key=="version"){bit=1;if(number<1||number>GraphicsSettings::cfgVersion)return false;version=number;}
        else if(key=="resolutionPercent"){bit=2;if(!GraphicsSettings::validResolution(number))return false;result.resolutionPercent=number;}
        else if(key=="shadows"){bit=4;if(number>1)return false;result.shadows=number!=0;}
        else if(key=="grass"){bit=8;if(number>1)return false;result.grass=number!=0;}
        else if(key=="profiler"){bit=16;if(number>1)return false;legacyProfiler=true;
            result.profilerMode=number?GraphicsSettings::ProfilerMode::Fps:GraphicsSettings::ProfilerMode::Off;}
        else if(key=="profilerMode"){bit=16;modeProfiler=true;result.profilerMode=static_cast<GraphicsSettings::ProfilerMode>(number);
            if(!GraphicsSettings::validProfilerMode(result.profilerMode))return false;}
        else if(key=="edgeMarginPercent"){bit=32;if(!GraphicsSettings::validEdgeMargin(number))return false;result.edgeMarginPercent=number;}
        else if(key=="occlusion"){bit=64;if(number>1)return false;parsed.legacyOcclusion=int(number);} // Legacy key: accepted, ignored (occlusionV2 below)
        else if(key=="occlusionV2"){bit=1u<<11;if(number>1)return false;result.occlusion=number!=0;}
        else if(key=="smallObjectsPx"){bit=128;if(!GraphicsSettings::validSmallObjects(number))return false;result.smallObjectsPx=number;}
        else if(key=="foveation"){bit=256;if(number>=GraphicsSettings::foveationLevels)return false;result.foveation=number;}
        else if(key=="foveationApi"){bit=512;if(number>=GraphicsSettings::foveationApis)return false;result.foveationApi=number;}
        else if(key=="antiAliasing"){bit=1024;if(number>=GraphicsSettings::legacyAntiAliasingModes)return false;antiAliasing=number;} // Range by version below
        else if(key=="frameRate"){bit=1u<<20;if(number>=GraphicsSettings::frameRateModes)return false;result.frameRate=number;}
        else if(key=="spaceWarp"){bit=1u<<21;if(number>=GraphicsSettings::spaceWarpModes)return false;result.spaceWarp=number?1u:0u;}
        else if(key=="shadowQuality"){bit=1u<<24;if(number>=GraphicsSettings::legacyShadowQualityLevels)return false;} // Row: accepted, ignored
        else if(key=="shadowUpdate"){bit=1u<<27;if(number>=GraphicsSettings::shadowUpdateLevels)return false;result.shadowUpdate=number;}
        else if(key=="shadowMap"){bit=1u<<25;if(number>=GraphicsSettings::shadowMapLevels)return false;result.shadowMap=number;}
        else if(key=="grassDistance"){bit=1u<<26;if(number>=GraphicsSettings::grassDistanceLevels)return false;result.grassDistance=number;}
        // bits 1<<11..1<<18 (1<<11 occlusionV2 above)
        else if(key=="postFx"){bit=1u<<12;if(number>=GraphicsSettings::postFxModes)return false;result.postFx=number;}
        else if(key=="textureFilter"){bit=1u<<13;if(number>=GraphicsSettings::textureFilterLevels)return false;result.textureFilter=number;}
        else if(key=="mipBias"){bit=1u<<14;if(number>=GraphicsSettings::mipBiasLevels)return false;result.mipBias=number;}
        else if(key=="sharpen"){bit=1u<<15;if(number>=GraphicsSettings::sharpenModes)return false;result.sharpen=number;}
        else if(key=="cascadeCull"){bit=1u<<16;if(number>=GraphicsSettings::cascadeCullLevels)return false;result.cascadeCull=number;}
        else if(key=="xrQueuePriority"){bit=1u<<17;if(number>=GraphicsSettings::xrQueuePriorities)return false;result.xrQueuePriority=number;}
        else if(key=="xrQueueVerified"){bit=1u<<18;if(number>1)return false;result.xrQueueVerified=number!=0;}
        else if(key=="taaSharpness"){bit=1u<<29;if(number>=GraphicsSettings::taaSharpnessModes)return false;result.taaSharpness=number;}
        else if(key=="lampShadows"){bit=1u<<19;if(number>=GraphicsSettings::lampShadowsPresets)return false;result.lampShadows=number;}
        else if(key=="heatWarning"){bit=1u<<30;if(number>1)return false;result.heatWarning=number!=0;} // heat01
        else if(key=="gpuBoost"){bit=1u<<22;if(number>=GraphicsSettings::gpuBoostModes)return false;result.gpuBoost=number;} // boost01
        else if(key=="water"){bit=1u<<23;if(number>1)return false;result.water=number!=0;} // w02 (bit still free: 1<<31)
        else return false;
        if(seen&bit)return false;seen|=bit;
    }
    // A version is required so a truncated/unrelated file cannot silently become
    // a valid configuration; missing individual options use current defaults.
    if(!(seen&1)||(version==1&&modeProfiler)||(version>=2&&legacyProfiler))return false;
    // version 4 = OFF/TAA/TAA FULL; versions 1..3 = the legacy six row values, mapped (FXAA* -> OFF, TAA / TAA SHARP -> TAA).
    if(version>=4){if(antiAliasing>=GraphicsSettings::antiAliasingModes)return false;result.antiAliasing=GraphicsSettings::antiAliasingLiteOnly(antiAliasing);} // TAA/FULL -> LITE
    else result.antiAliasing=GraphicsSettings::antiAliasingLiteOnly(GraphicsSettings::legacyAntiAliasing(antiAliasing));
    parsed.version=version;out=result;info=parsed;return true;
}
std::FILE* openFile(const std::filesystem::path& p,const char* mode) {
#ifdef _WIN32
    std::FILE* file{};
    return _wfopen_s(&file,p.c_str(),mode[0]=='r'?L"rb":L"wb")==0?file:nullptr;
#else
    return std::fopen(p.c_str(),mode);
#endif
}
std::filesystem::path utf8Path(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(),value.end()));
}
bool durableFlush(std::FILE* f) {
    if(std::fflush(f)!=0)return false;
#ifdef _WIN32
    return _commit(_fileno(f))==0;
#else
    return ::fsync(::fileno(f))==0;
#endif
}
bool replaceFile(const std::filesystem::path& from,const std::filesystem::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    return ::rename(from.c_str(),to.c_str())==0;
#endif
}
}
bool GraphicsSettings::validResolution(unsigned p) noexcept {
    return std::find(resolutionValues.begin(),resolutionValues.end(),p)!=resolutionValues.end();
}
bool GraphicsSettings::validEdgeMargin(unsigned p) noexcept {
    return std::find(edgeMarginValues.begin(),edgeMarginValues.end(),p)!=edgeMarginValues.end();
}
bool GraphicsSettings::validProfilerMode(ProfilerMode mode) noexcept {return static_cast<unsigned>(mode)<=2;}
bool GraphicsSettings::validSmallObjects(unsigned p) noexcept {
    return std::find(smallObjectsValues.begin(),smallObjectsValues.end(),p)!=smallObjectsValues.end();
}
bool GraphicsSettings::parseS02Mask(std::string_view text,unsigned& out,bool& clamped) noexcept {
    // Always hex, exactly like the debug.rdrvr.s02 property: optional 0x/0X, then 1..8 hex digits.
    if(text.size()>=2&&text[0]=='0'&&(text[1]=='x'||text[1]=='X'))text.remove_prefix(2);
    if(text.empty()||text.size()>8)return false;
    std::uint32_t v{};
    for(const char c:text){unsigned d{};
        if(c>='0'&&c<='9')d=unsigned(c-'0');else if(c>='a'&&c<='f')d=unsigned(c-'a'+10);else if(c>='A'&&c<='F')d=unsigned(c-'A'+10);else return false;
        v=v*16u+d;}
    out=unsigned(v)&s02MaskBits;clamped=(unsigned(v)&~s02MaskBits)!=0;return true;
}
void GraphicsSettings::migrateShipDefaults(Settings& s) noexcept { // Exactly the six ship keys
    const Settings d{};
    s.lampShadows=d.lampShadows;s.shadowUpdate=d.shadowUpdate;s.foveation=d.foveation;
    s.cascadeCull=d.cascadeCull;s.frameRate=d.frameRate;s.s02Mask=d.s02Mask;
}
const char* GraphicsSettings::foveationName(unsigned level) noexcept {
    switch(level){case 1:return "LOW";case 2:return "MEDIUM";case 3:return "HIGH";case 4:return "ULTRA";case 5:return "AUTO";default:return "OFF";}
}
const char* GraphicsSettings::foveationApiName(unsigned api) noexcept {return api==1?"FDM":api==2?"NONE":"VRS";}
const char* GraphicsSettings::antiAliasingName(unsigned mode) noexcept { // Includes the in-headset diagnostic modes
    switch(mode){case 1:return "TAA";case 2:return "TAA FULL";case 3:return "TAA LITE";default:return "OFF";}
}
const char* GraphicsSettings::frameRateName(unsigned m) noexcept {return m==1?"LOCK 36":m==2?"LOCK 24":m==3?"ADAPTIVE":"AUTO";}
const char* GraphicsSettings::spaceWarpName(unsigned m) noexcept {
    switch(m){case 1:return "ON";case 2:return "ON ZERO MV";case 3:return "ON FAR DEPTH";case 4:return "ON INVERT";case 5:return "ON FLIP Y";case 6:return "ON CAM ONLY";default:return "OFF";}
}
const char* GraphicsSettings::profilerModeName(ProfilerMode mode) noexcept {
    switch(mode){case ProfilerMode::Off:return "OFF";case ProfilerMode::Fps:return "FPS";case ProfilerMode::Detail:return "DETAIL";}
    return "OFF";
}
bool GraphicsSettings::configurePath(std::string path) noexcept {
    PreserveErrno preserve;
    try{
        std::lock_guard lock(mutex_);
        if(loaded_||path.empty()||path.find('\0')!=std::string::npos)return false;
        const std::filesystem::path file=utf8Path(path);
        if(!file.is_absolute()||file.filename()!="graphics.cfg")return false;
        path_=std::move(path);return true;
    }catch(...){return false;}
}
bool GraphicsSettings::loadOnce() noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(loaded_)return loadResult_;
    loaded_=true;settings_={};loadResult_=false;legacyOcclusion_=-1;
    cfgVersionLoaded_=0;cfgMigrated_=cfgMigrationSaved_=s02MaskClamped_=false;
    bool migrationFailed{};
    try{
        if(!path_.empty()){
            errno=0;auto* file=openFile(utf8Path(path_),"rb");
            if(!file){loadResult_=errno==ENOENT;}
            else {
                std::array<char,4097> data{};
                const auto size=std::fread(data.data(),1,data.size(),file);
                const bool good=!std::ferror(file)&&size<=4096;
                const bool closed=std::fclose(file)==0;
                ParseInfo info;
                loadResult_=good&&closed&&parse(std::string_view(data.data(),size),settings_,info);
                if(loadResult_){
                    legacyOcclusion_=info.legacyOcclusion;cfgVersionLoaded_=info.version;s02MaskClamped_=info.s02MaskClamped;
                    // One-time migration to the ship defaults; the save writes version=4, so it never repeats once saved.
                    // A version 3 file keeps its ship keys; only its antiAliasing is mapped (parse) and saved as v4.
                    if(info.version<cfgVersion){
                        if(info.version<cfgShipVersion)migrateShipDefaults(settings_);
                        cfgMigrated_=true;
                        cfgMigrationSaved_=saveLocked();migrationFailed=!cfgMigrationSaved_;
                    }
                }
            }
        }
    }catch(...){loadResult_=false;settings_={};legacyOcclusion_=-1;cfgVersionLoaded_=0;cfgMigrated_=cfgMigrationSaved_=s02MaskClamped_=false;}
    startupResolutionPercent_=settings_.resolutionPercent;startupXrQueuePriority_=settings_.xrQueuePriority;
    taaSharpness.store(int(settings_.taaSharpness),std::memory_order_relaxed);
    heatWarningSetting.store(settings_.heatWarning?1:0,std::memory_order_relaxed); // heat01
    gpuBoostSetting.store(int(settings_.gpuBoost),std::memory_order_relaxed); // boost01
    waterSetting.store(settings_.water?1:0,std::memory_order_relaxed); // w02
    // a migrated file that could not be written keeps the migrated values for this session (SAVE FAILED in the menu).
    persistenceOk_=loadResult_&&!migrationFailed;return loadResult_;
}
GraphicsSettings::Snapshot GraphicsSettings::snapshot() const noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    return {settings_,startupResolutionPercent_,applied_,loaded_,settings_.resolutionPercent!=startupResolutionPercent_,persistenceOk_,
            legacyOcclusion_,startupXrQueuePriority_,cfgVersionLoaded_,cfgMigrated_,cfgMigrationSaved_,s02MaskClamped_};
}
bool GraphicsSettings::saveLocked() noexcept {
    persistenceOk_=false;
    if(!loaded_||path_.empty())return false;
    std::filesystem::path temporary;
    std::FILE* file{};
    try{
        const auto target=utf8Path(path_);
        temporary=target;temporary+=".tmp";
        file=openFile(temporary,"wb");
        if(!file)return false;
        // Format version 4 (antiAliasing 0 OFF, 1 TAA, 2 TAA FULL).
        const int count=std::fprintf(file,"version=4\nresolutionPercent=%u\nshadows=%u\ngrass=%u\nprofilerMode=%u\nedgeMarginPercent=%u\nsmallObjectsPx=%u\nfoveation=%u\nfoveationApi=%u\nantiAliasing=%u\n",
            settings_.resolutionPercent,unsigned(settings_.shadows),unsigned(settings_.grass),unsigned(settings_.profilerMode),
            settings_.edgeMarginPercent,settings_.smallObjectsPx,settings_.foveation,settings_.foveationApi,settings_.antiAliasing);
        const int pacing=std::fprintf(file,"frameRate=%u\nspaceWarp=%u\n",settings_.frameRate,settings_.spaceWarp);
        const int shadowGrass=std::fprintf(file,"shadowUpdate=%u\nshadowMap=%u\ngrassDistance=%u\n",settings_.shadowUpdate,settings_.shadowMap,settings_.grassDistance);
        // postFx 2 (OFF FAST) is session-only and saved as 0, so a misbehaving OFF FAST cannot persist.
        const int r53=std::fprintf(file,"occlusionV2=%u\npostFx=%u\ntextureFilter=%u\nmipBias=%u\nsharpen=%u\ncascadeCull=%u\nxrQueuePriority=%u\nxrQueueVerified=%u\n",
            unsigned(settings_.occlusion),settings_.postFx==postFxOffFast?0u:settings_.postFx,settings_.textureFilter,settings_.mipBias,
            settings_.sharpen,settings_.cascadeCull,settings_.xrQueuePriority,unsigned(settings_.xrQueueVerified));
        const int ls01=std::fprintf(file,"lampShadows=%u\ntaaSharpness=%u\n",settings_.lampShadows,settings_.taaSharpness);
        const int ship01=std::fprintf(file,"s02Mask=0x%04x\n",settings_.s02Mask&s02MaskBits);
        const int heat01=settings_.heatWarning?1:std::fprintf(file,"heatWarning=0\n"); // heat01: default ON is not written (pre-heat01 builds still parse the file)
        const int boost01=settings_.gpuBoost==2?1:std::fprintf(file,"gpuBoost=%u\n",settings_.gpuBoost); // boost01: default 690 is not written (same reason)
        const int w02=settings_.water?1:std::fprintf(file,"water=0\n"); // w02: default ON is not written (same reason)
        const bool written=count>0&&pacing>0&&shadowGrass>0&&r53>0&&ls01>0&&ship01>0&&heat01>0&&boost01>0&&w02>0&&durableFlush(file);
        const bool closed=std::fclose(file)==0;file=nullptr;
        if(written&&closed&&replaceFile(temporary,target)){persistenceOk_=true;return true;}
    }catch(...){}
    if(file)std::fclose(file);
    if(!temporary.empty()){std::error_code ignored;std::filesystem::remove(temporary,ignored);}
    return false;
}
bool GraphicsSettings::save() noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);return saveLocked();
}
bool GraphicsSettings::setResolutionPercent(unsigned p) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||!validResolution(p))return false;
    settings_.resolutionPercent=p;return saveLocked();
}
bool GraphicsSettings::setFlag(bool Settings::*member,bool value) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_)return false;
    settings_.*member=value;return saveLocked();
}
bool GraphicsSettings::setShadows(bool v) noexcept {return setFlag(&Settings::shadows,v);}
bool GraphicsSettings::setGrass(bool v) noexcept {return setFlag(&Settings::grass,v);}
// The bool setter selects only the cheap overlay (Fps); detailed modes use setProfilerMode.
bool GraphicsSettings::setProfiler(bool v) noexcept {return setProfilerMode(v?ProfilerMode::Fps:ProfilerMode::Off);}
bool GraphicsSettings::setProfilerMode(ProfilerMode mode) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||!validProfilerMode(mode))return false;
    settings_.profilerMode=mode;return saveLocked();
}
bool GraphicsSettings::setOcclusion(bool v) noexcept {return setFlag(&Settings::occlusion,v);}
bool GraphicsSettings::setEdgeMarginPercent(unsigned p) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||!validEdgeMargin(p))return false;
    settings_.edgeMarginPercent=p;return saveLocked();
}
bool GraphicsSettings::setSmallObjectsPx(unsigned p) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||!validSmallObjects(p))return false;
    settings_.smallObjectsPx=p;return saveLocked();
}
bool GraphicsSettings::setFoveation(unsigned level) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||level>=foveationLevels)return false;
    settings_.foveation=level;return saveLocked();
}
bool GraphicsSettings::setFoveationApi(unsigned api) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||api>=foveationApis)return false;
    settings_.foveationApi=api;return saveLocked();
}
bool GraphicsSettings::setTaaSharpness(unsigned s) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||s>=taaSharpnessModes)return false;
    settings_.taaSharpness=s;taaSharpness.store(int(s),std::memory_order_relaxed);return saveLocked();
}
bool GraphicsSettings::setAntiAliasing(unsigned mode) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||mode>=antiAliasingModes)return false;
    settings_.antiAliasing=antiAliasingLiteOnly(mode);return saveLocked();
}
bool GraphicsSettings::setFrameRate(unsigned mode) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||mode>=frameRateModes)return false;
    settings_.frameRate=mode;return saveLocked();
}
bool GraphicsSettings::setSpaceWarp(unsigned mode) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||mode>=spaceWarpModes)return false;
    settings_.spaceWarp=mode;return saveLocked();
}
bool GraphicsSettings::setShadowUpdate(unsigned level) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||level>=shadowUpdateLevels)return false;
    settings_.shadowUpdate=level;return saveLocked();
}
bool GraphicsSettings::setShadowMap(unsigned level) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||level>=shadowMapLevels)return false;
    settings_.shadowMap=level;return saveLocked();
}
bool GraphicsSettings::setGrassDistance(unsigned level) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||level>=grassDistanceLevels)return false;
    settings_.grassDistance=level;return saveLocked();
}
const char* GraphicsSettings::shadowUpdateName(unsigned v) noexcept {
    return v==1?"1/2":v==2?"1/3":v==3?"ADAPTIVE":v==4?"INDOOR":v==5?"STAGGER":v==6?"STAGGER FAR":v==7?"STAGGER / NIGHT OFF":"EVERY";}
const char* GraphicsSettings::shadowMapName(unsigned v) noexcept {return v==1?"640":v==2?"512":"NATIVE";}
const char* GraphicsSettings::grassDistanceName(unsigned v) noexcept {return v==1?"75%":v==2?"50%":v==3?"35%":"FULL";}
// Names (panel glyphs only: A-Z 0-9 space . : / - % ? < >)
const char* GraphicsSettings::postFxName(unsigned v) noexcept {return v==1?"ON":v==2?"OFF FAST":"OFF";}
const char* GraphicsSettings::textureFilterName(unsigned v) noexcept {return v==1?"8X":v==2?"4X":v==3?"2X":v==4?"OFF":"NATIVE";}
const char* GraphicsSettings::mipBiasName(unsigned v) noexcept {return v==1?"0.25":v==2?"0.5":"0";}
const char* GraphicsSettings::sharpenName(unsigned v) noexcept {return v==1?"NORMAL":v==2?"QUALITY":v==3?"AUTO":"OFF";}
const char* GraphicsSettings::cascadeCullName(unsigned v) noexcept {return v==1?"X2":v==2?"X4":"NATIVE";}
const char* GraphicsSettings::xrQueuePriorityName(unsigned v) noexcept {return v==1?"PRIORITY":"NORMAL";}
const char* GraphicsSettings::lampShadowsName(unsigned v) noexcept {
    return v==1?"1/2":v==2?"1/2 NO CASCADE":v==3?"1/3 NO CASC CULL X2":v==4?"1/3 CAP 2":v==5?"OFF":"NATIVE";}
bool GraphicsSettings::setLevel(unsigned Settings::*member,unsigned value,unsigned count) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||value>=count)return false;
    settings_.*member=value;return saveLocked();
}
bool GraphicsSettings::setPostFx(unsigned v) noexcept {return setLevel(&Settings::postFx,v,postFxModes);}
bool GraphicsSettings::setTextureFilter(unsigned v) noexcept {return setLevel(&Settings::textureFilter,v,textureFilterLevels);}
bool GraphicsSettings::setMipBias(unsigned v) noexcept {return setLevel(&Settings::mipBias,v,mipBiasLevels);}
bool GraphicsSettings::setSharpen(unsigned v) noexcept {return setLevel(&Settings::sharpen,v,sharpenModes);}
bool GraphicsSettings::setCascadeCull(unsigned v) noexcept {return setLevel(&Settings::cascadeCull,v,cascadeCullLevels);}
bool GraphicsSettings::setXrQueuePriority(unsigned v) noexcept {return setLevel(&Settings::xrQueuePriority,v,xrQueuePriorities);}
bool GraphicsSettings::setXrQueueVerified(bool v) noexcept {return setFlag(&Settings::xrQueueVerified,v);}
bool GraphicsSettings::setLampShadows(unsigned v) noexcept {return setLevel(&Settings::lampShadows,v,lampShadowsPresets);}
bool GraphicsSettings::setHeatWarning(bool v) noexcept { // heat01: live (HeatWarning reads the published value every frame)
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_)return false;
    settings_.heatWarning=v;heatWarningSetting.store(v?1:0,std::memory_order_relaxed);return saveLocked();
}
bool GraphicsSettings::setGpuBoost(unsigned v) noexcept { // boost01: live (the GpuBoost worker reads the published value at 4 Hz)
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||v>=gpuBoostModes)return false;
    settings_.gpuBoost=v;gpuBoostSetting.store(int(v),std::memory_order_relaxed);return saveLocked();
}
bool GraphicsSettings::setWater(bool v) noexcept { // w02: live (GameXr reads the published value every stereo frame)
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_)return false;
    settings_.water=v;waterSetting.store(v?1:0,std::memory_order_relaxed);return saveLocked();
}
bool GraphicsSettings::setSunShadows(bool on,unsigned update) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||update>=shadowUpdateLevels)return false;
    settings_.shadows=on;settings_.shadowUpdate=update;return saveLocked();
}
bool GraphicsSettings::setGrassRow(bool on,unsigned distance) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_||distance>=grassDistanceLevels)return false;
    settings_.grass=on;settings_.grassDistance=distance;return saveLocked();
}
bool GraphicsSettings::restoreDefaults() noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(!loaded_)return false;
    settings_=Settings{};heatWarningSetting.store(settings_.heatWarning?1:0,std::memory_order_relaxed); // heat01
    gpuBoostSetting.store(int(settings_.gpuBoost),std::memory_order_relaxed); // boost01
    waterSetting.store(settings_.water?1:0,std::memory_order_relaxed);return saveLocked(); // w02
}
unsigned GraphicsSettings::postFx() const noexcept {return snapshot().requested.postFx;}
unsigned GraphicsSettings::textureFilter() const noexcept {return snapshot().requested.textureFilter;}
unsigned GraphicsSettings::mipBias() const noexcept {return snapshot().requested.mipBias;}
unsigned GraphicsSettings::sharpen() const noexcept {return snapshot().requested.sharpen;}
unsigned GraphicsSettings::cascadeCull() const noexcept {return snapshot().requested.cascadeCull;}
unsigned GraphicsSettings::xrQueuePriority() const noexcept {return snapshot().requested.xrQueuePriority;}
bool GraphicsSettings::xrQueueVerified() const noexcept {return snapshot().requested.xrQueueVerified;}
unsigned GraphicsSettings::lampShadows() const noexcept {return snapshot().requested.lampShadows;}
unsigned GraphicsSettings::s02Mask() const noexcept {return snapshot().requested.s02Mask;}
bool GraphicsSettings::heatWarning() const noexcept {return snapshot().requested.heatWarning;}        // heat01
unsigned GraphicsSettings::gpuBoost() const noexcept {return snapshot().requested.gpuBoost;}         // boost01
bool GraphicsSettings::water() const noexcept {return snapshot().requested.water;}                  // w02
int GraphicsSettings::legacyOcclusion() const noexcept {return snapshot().legacyOcclusion;}
unsigned GraphicsSettings::shadowUpdate() const noexcept {return snapshot().requested.shadowUpdate;}
unsigned GraphicsSettings::shadowMap() const noexcept {return snapshot().requested.shadowMap;}
unsigned GraphicsSettings::grassDistance() const noexcept {return snapshot().requested.grassDistance;}
void GraphicsSettings::recordAppliedExtent(std::uint32_t w,std::uint32_t h) noexcept {
    PreserveErrno preserve;std::lock_guard lock(mutex_);
    if(loaded_&&w>=16&&h>=16&&w<=8192&&h<=8192)applied_={w,h};
}
bool GraphicsSettings::shadows() const noexcept {return snapshot().requested.shadows;}
bool GraphicsSettings::grass() const noexcept {return snapshot().requested.grass;}
bool GraphicsSettings::profiler() const noexcept {return detailedProfiling();}
GraphicsSettings::ProfilerMode GraphicsSettings::profilerMode() const noexcept {return snapshot().requested.profilerMode;}
bool GraphicsSettings::fpsVisible() const noexcept {return profilerMode()!=ProfilerMode::Off;}
bool GraphicsSettings::detailedProfiling() const noexcept {return profilerMode()==ProfilerMode::Detail;}
unsigned GraphicsSettings::edgeMarginPercent() const noexcept {return snapshot().requested.edgeMarginPercent;}
bool GraphicsSettings::occlusion() const noexcept {return snapshot().requested.occlusion;}
unsigned GraphicsSettings::smallObjectsPx() const noexcept {return snapshot().requested.smallObjectsPx;}
unsigned GraphicsSettings::foveation() const noexcept {return snapshot().requested.foveation;}
unsigned GraphicsSettings::foveationApi() const noexcept {return snapshot().requested.foveationApi;}
unsigned GraphicsSettings::frameRate() const noexcept {return snapshot().requested.frameRate;}
unsigned GraphicsSettings::spaceWarp() const noexcept {return snapshot().requested.spaceWarp;}
unsigned GraphicsSettings::antiAliasing() const noexcept {return snapshot().requested.antiAliasing;}
}
