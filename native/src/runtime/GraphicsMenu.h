#pragma once
#include "GraphicsMenuInput.h"
#include "GraphicsSettings.h"
#include "FoveationStatus.h"
#include "AntiAliasingStatus.h"
#include "diagnostics/ProfilerControl.h"
#include "StereoTestMode.h"
#include "Teleport.h"
#include "HeatWarning.h" // heat01
#include "GpuBoost.h" // boost01
#include "vr/VrMenuPages.h" // VR and CHEATS pages (candidate/vr)
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace rdrvr::runtime {
// 23 rows (+ TELEPORT), the stick help line is gone; still 25 lines.
// + LAMP SHADOWS, - CLOSE (B closes): 23 rows, 25 lines.
// heat01: + HEAT WARNING after PROFILER: 24 rows, 26 lines (PerfPanel height 414 -> 430 keeps the 16 px pitch).
// boost01: + GPU BOOST after HEAT WARNING: 25 rows, 27 lines (PerfPanel 430 -> 446).
// layout: 25 panel lines = the title/FPS line, 22 rows, the stick help and one status/restart line (PerfPanel:
// 25 lines at the 16 px pitch, 52 glyphs per line; glyphs A-Z 0-9 space . : / - % ? < > only, suffixes " - WORD").
// Row text = line(row,label,value) "%c %-16s%s"; values are clipped to the 34 columns the panel can draw.
// Three pages GRAPHICS / VR / CHEATS (X = previous, Y = next while open). text() is still the GRAPHICS page exactly as
// before (host tests compare it); the headset draws pageText(), whose line 0 is the page header with the FPS on the right.
// debug.rdrvr.vr.menu 0: one page, pageText() == text().
// ROOT + submenus (VrMenuPages.h): every open starts on ROOT (GRAPHICS / MOVEMENT / HANDS / WEAPONS / CHEATS /
// RECENTER), A opens the marked submenu, B in a submenu returns to ROOT, B on ROOT closes; X/Y jump between submenus.
// The GRAPHICS submenu is text() minus its title line (rows, input and graphics.cfg exactly as before).
class GraphicsMenu {
public:
    // release: the GRAPHICS page shows only user rows; STEREO TEST, TELEPORT, PROFILER, XR QUEUE, OCCLUSION (experimental)
    // FOVEATION API and EDGE MARGIN are hidden (their values stay as saved / defaults). Visible row i = enum row visibleRows[i].
    static constexpr bool hiddenRow(unsigned r) noexcept {
        return r==GraphicsMenuInput::StereoTest||r==GraphicsMenuInput::Teleport||r==GraphicsMenuInput::XrQueue||
               r==GraphicsMenuInput::Occlusion||r==GraphicsMenuInput::FoveationApi||r==GraphicsMenuInput::EdgeMargin;}
    static constexpr unsigned visibleCount() noexcept {unsigned n=0;for(unsigned r=0;r<unsigned(GraphicsMenuInput::Count);++r)n+=!hiddenRow(r);return n;}
    static constexpr unsigned rowAt(unsigned i) noexcept {for(unsigned r=0;r<unsigned(GraphicsMenuInput::Count);++r)if(!hiddenRow(r)&&!i--)return r;return unsigned(GraphicsMenuInput::Count);}
private:
    GraphicsMenuInput input_;
    unsigned page_{::rdrvr::vr::PageRoot};std::array<unsigned,::rdrvr::vr::PageCount> rowMemory_{}; // Current page, last row per page
    void showPage(unsigned page) noexcept { // Leave the current page (its row is remembered) and show `page`
        namespace vr=::rdrvr::vr;
        rowMemory_[page_]=input_.selected();
        if(page_!=vr::PageRoot)rowMemory_[vr::PageRoot]=vr::RootPage::rowOfPage(page_);
        page_=page;input_.select(std::min(rowMemory_[page_],rowsOf(page_)-1));
        vr::menuPage.store(int(page_),std::memory_order_relaxed);dirty_=true;
    }
    static const char* smallObjectsName(unsigned px)noexcept{
        switch(px){case 2:return "SKIP <2 PX";case 4:return "SKIP <4 PX";case 8:return "SKIP <8 PX";case 16:return "SKIP <16 PX";
                   case 24:return "SKIP <24 PX";case 32:return "SKIP <32 PX";default:return "ALL";}
    }
    bool dirty_{true};
    // Generic level row: the stick clamps at both ends, A wraps.
    static int cycle(int index,int adjust,int n) noexcept {return adjust?std::clamp(index+adjust,0,n-1):(index+1)%n;}
public:
    static constexpr unsigned panelLines=27,panelColumns=52,labelWidth=16,valueColumns=panelColumns-2-labelWidth; // heat01: 26 lines
    // SUN SHADOWS row: EVERY, 1/2, 1/3, ADAPTIVE, INDOOR, STAGGER, STAGGER FAR (shadowUpdate 0..6),
    // STAGGER / NIGHT OFF (7), then OFF.
    // GRASS row: FULL..35%, then OFF.
    // On OFF, A and stick-left return to the stored value (stick-right stays OFF).
    static constexpr unsigned sunShadowsOff=GraphicsSettings::shadowUpdateLevels,grassOff=GraphicsSettings::grassDistanceLevels;
    static constexpr unsigned sunShadowsIndex(const GraphicsSettings::Settings& v) noexcept {
        return !v.shadows?sunShadowsOff:v.shadowUpdate<GraphicsSettings::shadowUpdateLevels?v.shadowUpdate:3u;} // unknown -> ADAPTIVE
    static constexpr unsigned grassIndex(const GraphicsSettings::Settings& v) noexcept {
        return !v.grass?grassOff:v.grassDistance<GraphicsSettings::grassDistanceLevels?v.grassDistance:0u;}
    static constexpr int resolutionDefaultIndex() noexcept { // index of 100 % (RESOLUTION fallback for an unknown value)
        for(unsigned i=0;i<GraphicsSettings::resolutionValues.size();++i)if(GraphicsSettings::resolutionValues[i]==100)return int(i);
        return 0;}
    bool open()const noexcept{return input_.open();}
    unsigned selected()const noexcept{return input_.selected();}
    unsigned page()const noexcept{return page_;}
    static unsigned pageCount() noexcept {return ::rdrvr::vr::pagesEnabled()?unsigned(::rdrvr::vr::PageCount):1u;}
    bool atRoot()const noexcept{return pageCount()>1&&page_==::rdrvr::vr::PageRoot;}
    static unsigned rowsOf(unsigned page) noexcept {
        namespace vr=::rdrvr::vr;
        if(vr::isExtPage(page)){const auto* x=vr::extPage(page);return x?std::max(1u,x->rows()):1u;}
        return page==vr::PageRoot?vr::RootPage::rows:vr::VrPage::isVrPage(page)?vr::VrPage::rows(page):
            page==vr::PageCheats?vr::CheatsPage::rows():page==vr::PageCalibration?vr::CalibPage::rows:page==vr::PageHelp?vr::HelpPage::rows:page==vr::PageButtons?vr::ButtonsPage::rows:visibleCount();}
    bool takeDirty() noexcept {const bool d=dirty_;dirty_=false;return d;}
    void invalidate()noexcept{dirty_=dirty_||input_.open();input_.invalidate();::rdrvr::vr::menuOpen.store(false,std::memory_order_relaxed);}
    bool update(ControllerSample& input,GraphicsSettings& settings=graphicsSettings,::rdrvr::vr::VrSettingsStore& vrStore=::rdrvr::vr::vrSettings) noexcept {
        namespace vr=::rdrvr::vr;
        const bool paged=pageCount()>1;
        if(!paged)page_=vr::PageGraphics;else if(page_>=vr::PageCount)page_=vr::PageRoot;
        // The hand holding a VR weapon may keep its grip squeezed when the menu closes (the close guard ignores that grip).
        input_.ignoreGrip(vr::calib::menuHoldHand.load(std::memory_order_relaxed));
        auto e=input_.update(input,rowsOf(page_),paged&&page_!=vr::PageRoot);dirty_=dirty_||e.changed;
        // values change only with A / trigger; the stick only scrolls. CHEATS (weapon pick) and WEAPON CALIBRATION (+/- offsets)
        // keep a clear sideways push.
        e.adjust=0; // No page changes values with the stick (CHEATS / CALIBRATION too) - triggers / A only
        if(e.triggerStep&&page_!=vr::PageRoot)e.adjust=e.triggerStep; // Right trigger +1, left trigger -1 on every value page
        vr::menuOpen.store(input_.open(),std::memory_order_relaxed);
        if(e.opened&&paged){showPage(vr::PageRoot);vr::menuOpens.fetch_add(1,std::memory_order_relaxed);} // Every open shows ROOT
        if(!input_.open())return e.consume;
        vr::menuPage.store(int(page_),std::memory_order_relaxed);
        if(!paged&&(e.page||e.back))return e.consume; // menu kill switch: X/Y do nothing (B never reports back here)
        if(e.back){showPage(page_==vr::PageGiveWeapons?unsigned(vr::PageCheats):unsigned(vr::PageRoot));vr::menuBacks.fetch_add(1,std::memory_order_relaxed);return e.consume;}
        if(e.page){ // X = previous submenu, Y = next (from ROOT: Y = the first, X = the last); rows remembered
            const unsigned current=page_==vr::PageRoot?(e.page>0?vr::submenuCount-1:0u):page_-1;
            const unsigned next=(current+vr::submenuCount+(e.page>0?1u:vr::submenuCount-1))%vr::submenuCount;
            showPage(next+1);vr::pageSwitches.fetch_add(1,std::memory_order_relaxed);return e.consume;
        }
        if(page_==vr::PageRoot){ // A opens the marked submenu or recenters; the stick only selects
            if(e.activate){
                const unsigned row=input_.selected();
                if(row<vr::submenuCount){showPage(vr::RootPage::pageOfRow(row));vr::menuEnters.fetch_add(1,std::memory_order_relaxed);}
                else if(row==vr::RootPage::recenterRow)vr::requestRecenter();
                else if(row==vr::RootPage::resetRow&&vr::RootPage::resetPress()){ // RESET ALL: graphics.cfg, vr.cfg, controls.cfg
                    settings.restoreDefaults();vrStore.restoreDefaults();vr::controlmap::resetDefaults();}
                dirty_=true;
            }
            return e.consume;
        }
        if((e.adjust||e.activate)&&vr::VrPage::isVrPage(page_)){
            vr::VrPage::input(unsigned(vr::VrPage::keyAt(page_,input_.selected())),e.adjust,e.activate,vrStore);return e.consume;}
        if((e.adjust||e.activate)&&page_==vr::PageCheats){vr::CheatsPage::input(input_.selected(),e.adjust,e.activate);
            const int request=vr::pageRequest.exchange(-1,std::memory_order_acq_rel); // CHEATS > WEAPONS BY TYPE
            if(request>0&&unsigned(request)<vr::PageCount){showPage(unsigned(request));vr::menuEnters.fetch_add(1,std::memory_order_relaxed);}
            return e.consume;}
        if(vr::isExtPage(page_)){if(e.adjust||e.activate)if(const auto* x=vr::extPage(page_)){x->input(input_.selected(),e.adjust,e.activate);dirty_=true;}return e.consume;}
        if(page_==vr::PageCalibration){if(e.adjust||e.activate)vr::CalibPage::input(input_.selected(),e.adjust,e.activate);return e.consume;}
        if(page_==vr::PageHelp)return e.consume;
        if(page_==vr::PageButtons){if(e.adjust||e.activate)vr::ButtonsPage::input(input_.selected(),e.adjust,e.activate);return e.consume;}
        if(page_==vr::PageGraphics&&(e.adjust||e.activate)){
            const auto value=settings.snapshot().requested;
            const auto level=[&](unsigned current,unsigned n){return unsigned(cycle(int(current),e.adjust,int(n)));};
            switch(rowAt(input_.selected())){
            case GraphicsMenuInput::Resolution:{
                const auto& values=GraphicsSettings::resolutionValues;
                const auto found=std::find(values.begin(),values.end(),value.resolutionPercent);
                const int index=found==values.end()?resolutionDefaultIndex():static_cast<int>(found-values.begin());
                const int next=e.adjust?std::clamp(index+e.adjust,0,int(values.size())-1):(index+1)%int(values.size());
                settings.setResolutionPercent(values[next]);break;}
            case GraphicsMenuInput::SunShadows:{ // SUN SHADOWS + SHADOW UPDATE; OFF keeps the stored update rate
                const unsigned stored=value.shadowUpdate<GraphicsSettings::shadowUpdateLevels?value.shadowUpdate:0u;
                const unsigned current=sunShadowsIndex(value);
                if(current==sunShadowsOff&&(e.adjust<0||!e.adjust)){settings.setSunShadows(true,stored);break;} // A / stick-left on OFF -> the stored rate
                const unsigned next=level(current,sunShadowsOff+1);
                if(next==sunShadowsOff)settings.setSunShadows(false,stored);
                else settings.setSunShadows(true,next);break;}
            case GraphicsMenuInput::ShadowMap: // Cascade map extent at creation, applies after restart
                settings.setShadowMap(level(value.shadowMap,GraphicsSettings::shadowMapLevels));break;
            case GraphicsMenuInput::CascadeCull:
                settings.setCascadeCull(level(value.cascadeCull,GraphicsSettings::cascadeCullLevels));break;
            case GraphicsMenuInput::LampShadows:
                settings.setLampShadows(level(value.lampShadows,GraphicsSettings::lampShadowsPresets));break;
            case GraphicsMenuInput::Grass:{ // GRASS + GRASS DISTANCE; OFF keeps the stored distance
                const unsigned stored=value.grassDistance<GraphicsSettings::grassDistanceLevels?value.grassDistance:0u;
                const unsigned current=grassIndex(value);
                if(current==grassOff&&(e.adjust<0||!e.adjust)){settings.setGrassRow(true,stored);break;} // A / stick-left on OFF -> the stored distance
                const unsigned next=level(current,grassOff+1);
                if(next==grassOff)settings.setGrassRow(false,stored);
                else settings.setGrassRow(true,next);break;}
            case GraphicsMenuInput::Water: // w02: live; A toggles, triggers ON / OFF (debug.rdrvr.vr.water overrides it)
                settings.setWater(level(value.water?1u:0u,2u)!=0);break;
            case GraphicsMenuInput::EdgeMargin:{
                const auto& values=GraphicsSettings::edgeMarginValues;
                const auto found=std::find(values.begin(),values.end(),value.edgeMarginPercent);
                const int index=found==values.end()?0:int(found-values.begin());
                settings.setEdgeMarginPercent(values[std::clamp(index+(e.adjust?e.adjust:1),0,int(values.size())-1)]);break;}
            case GraphicsMenuInput::Occlusion:if(graphicsOcclusionAvailable.load(std::memory_order_acquire))settings.setOcclusion(!value.occlusion);break;
            case GraphicsMenuInput::SmallObjects:{
                const auto& values=GraphicsSettings::smallObjectsValues;
                const auto found=std::find(values.begin(),values.end(),value.smallObjectsPx);
                const int index=found==values.end()?0:int(found-values.begin());
                settings.setSmallObjectsPx(values[unsigned(cycle(index,e.adjust,int(values.size())))]);break;}
            case GraphicsMenuInput::Foveation:
                settings.setFoveation(level(value.foveation,GraphicsSettings::foveationLevels));break;
            case GraphicsMenuInput::FoveationApi: // Device creation, applies after restart
                settings.setFoveationApi(level(value.foveationApi,GraphicsSettings::foveationApis));break;
            case GraphicsMenuInput::TextureFilter:settings.setTextureFilter(level(value.textureFilter,GraphicsSettings::textureFilterLevels));break;
            case GraphicsMenuInput::MipBias:settings.setMipBias(level(value.mipBias,GraphicsSettings::mipBiasLevels));break;
            case GraphicsMenuInput::Sharpen:settings.setSharpen(level(value.sharpen,GraphicsSettings::sharpenModes));break;
            case GraphicsMenuInput::AntiAliasing: // Live (debug.rdrvr.aa overrides it)
                // OFF -> TAA LITE -> TAA LITE SHARP -> TAA LITE SHARPER (position = 0, or 1 + sharpness)
                {const unsigned pos=value.antiAliasing?1u+std::min(value.taaSharpness,GraphicsSettings::taaSharpnessModes-1):0u;
                 const unsigned next=level(pos,1u+GraphicsSettings::taaSharpnessModes);
                 if(next)settings.setTaaSharpness(next-1);
                 settings.setAntiAliasing(next?GraphicsSettings::antiAliasingTaaLite:0u);}break; // OFF <-> TAA LITE
            case GraphicsMenuInput::PostFx: // OFF / ON / OFF FAST (OFF FAST is saved as OFF)
                settings.setPostFx(level(value.postFx,GraphicsSettings::postFxModes));break;
            case GraphicsMenuInput::FrameRate:
                settings.setFrameRate(level(value.frameRate,GraphicsSettings::frameRateModes));break;
            case GraphicsMenuInput::SpaceWarp: // Live (debug.rdrvr.spacewarp overrides it)
                settings.setSpaceWarp(value.spaceWarp?0u:1u);break; // ON / OFF only (the bisection variants are gone from the menu)
            case GraphicsMenuInput::XrQueue: // Device creation, applies after restart (debug.rdrvr.queueprio overrides it)
                settings.setXrQueuePriority(level(value.xrQueuePriority,GraphicsSettings::xrQueuePriorities));break;
            case GraphicsMenuInput::Profiler:{ // FPS COUNTER (user): OFF / ON only DETAIL (cfg profilerMode 2, benches) -> OFF on the next press
                const unsigned m=unsigned(value.profilerMode);
                const unsigned next=m>1u?0u:e.adjust>0?1u:e.adjust<0?0u:(m?0u:1u);
                settings.setProfilerMode(static_cast<GraphicsSettings::ProfilerMode>(next));break;}
            case GraphicsMenuInput::HeatWarning: // heat01: live; A toggles, stick right ON / left OFF (debug.rdrvr.heatwarn overrides it)
                settings.setHeatWarning(level(value.heatWarning?1u:0u,2u)!=0);break;
            case GraphicsMenuInput::GpuBoost: // boost01: live; OFF / 640 / 690 (debug.rdrvr.boost overrides it)
                settings.setGpuBoost(level(value.gpuBoost,GraphicsSettings::gpuBoostModes));break;
            case GraphicsMenuInput::StereoTest:{const int mode=int(::rdrvr::engine::stereo::testMode.load());const int n=int(::rdrvr::engine::stereo::testModeCount);
                ::rdrvr::engine::stereo::testMode.store(unsigned(((e.adjust?mode+e.adjust:mode+1)%n+n)%n));break;}
            case GraphicsMenuInput::Teleport: ::rdrvr::teleport::menuInput(e.adjust,e.activate);break; // Stick = SAVE SPOT / SPOT N, A = do it
            case GraphicsMenuInput::Defaults:if(e.activate)settings.restoreDefaults();break; // Settings{} in one save
            default:break;
            }
            diagnostics::setProfiling(settings.detailedProfiling());
        }
        return e.consume;
    }
    std::string text(double newFps,double displayHz,unsigned width,unsigned height,const GraphicsSettings& settings=graphicsSettings)const {
        namespace st=::rdrvr::engine::stereo;
        const auto s=settings.snapshot();const auto& v=s.requested;
        const unsigned mode=st::testMode.load(std::memory_order_relaxed);
        std::string out;out.reserve(1600);
        char b[96];
        std::snprintf(b,sizeof b,"GRAPHICS  %.1f FPS  %.0f HZ  EYE %uX%u\n",newFps,displayHz,width,height);out+=b;
        const auto line=[&](unsigned row,const char* label,const char* value){
            if(hiddenRow(row))return;
            const char mark=rowAt(input_.selected())==row?'>':' ';
            if(*value)std::snprintf(b,sizeof b,"%c %-16s%.*s\n",mark,label,int(valueColumns),value);
            else std::snprintf(b,sizeof b,"%c %s\n",mark,label);
            out+=b;};
        char t[64];
        std::snprintf(t,sizeof t,"%u%%",v.resolutionPercent);line(GraphicsMenuInput::Resolution,"RESOLUTION",t);
        { // SUN SHADOWS = shadows + SHADOW UPDATE
          // STAGGER / STAGGER FAR: UNAVAILABLE when they act as EVERY; IN ROOM while the indoor gate holds the atlas
          // 7 STAGGER / NIGHT OFF runs STAGGER by day. sunNightMenu (SunNight.h): NIGHT while the gate holds the sun shadows
          // off (+ PROP when debug.rdrvr.sunnight engaged it, any row); for 7 otherwise NO CLOCK, PROP, IN ROOM or DAY.
            const int ready=shadowUpdateReady.load(std::memory_order_relaxed),indoor=cameraIndoorMenu.load(std::memory_order_relaxed);
            const bool nightOff=v.shadowUpdate==GraphicsSettings::shadowUpdateNightOff;
            const bool stagger=v.shadowUpdate==GraphicsSettings::shadowUpdateStagger||v.shadowUpdate==GraphicsSettings::shadowUpdateStaggerFar||nightOff;
            const int night=sunNightMenu.load(std::memory_order_relaxed),ns=night<0?-1:night&7;const bool nightProp=night>=0&&(night&8)!=0;
            const bool gateIn=sunStaggerGate.load(std::memory_order_relaxed)==1;
            const char* suffix=ns==2||ns==5?(nightProp?" - NIGHT PROP":" - NIGHT"):
                ready==0||(stagger&&sunStaggerReady.load(std::memory_order_relaxed)==0)?" - UNAVAILABLE":
                v.shadowUpdate==GraphicsSettings::shadowUpdateIndoor?(indoor==1?" - IN ROOM":indoor==0?" - OUTSIDE":" - UNKNOWN"):
                nightOff?(ns==3?" - NO CLOCK":nightProp?" - PROP":gateIn?" - IN ROOM":ns==1?" - DAY":""):
                stagger?(gateIn?" - IN ROOM":indoor==0?" - OUTSIDE":""):"";
            if(!v.shadows)std::snprintf(t,sizeof t,"OFF");
            else std::snprintf(t,sizeof t,"%s%s",GraphicsSettings::shadowUpdateName(v.shadowUpdate),suffix);
            line(GraphicsMenuInput::SunShadows,"SUN SHADOWS",t);
        }
        // SHADOW MAP applies when the engine creates the shadow target; pending while the saved level differs.
        const int mapStartup=shadowMapStartupLevel.load(std::memory_order_relaxed);const unsigned mapExtent=shadowMapCascadeExtent.load(std::memory_order_relaxed);
        const bool mapPending=mapStartup>=0&&unsigned(mapStartup)!=v.shadowMap;
        if(mapPending)std::snprintf(t,sizeof t,"%s - AT RESTART",GraphicsSettings::shadowMapName(v.shadowMap));
        else if(mapExtent)std::snprintf(t,sizeof t,"%s - %u PX",GraphicsSettings::shadowMapName(v.shadowMap),mapExtent);
        else std::snprintf(t,sizeof t,"%s",GraphicsSettings::shadowMapName(v.shadowMap));
        line(GraphicsMenuInput::ShadowMap,"SHADOW MAP",t);
        std::snprintf(t,sizeof t,"%s%s",GraphicsSettings::cascadeCullName(v.cascadeCull),
            cascadeCullReady.load(std::memory_order_relaxed)==0&&v.cascadeCull?" - UNAVAILABLE":"");
        line(GraphicsMenuInput::CascadeCull,"CASCADE CULL",t);
        { // LAMP SHADOWS preset (LampShadows.h); PROP while a debug.rdrvr.lamp*/spotcascade property overrides the row
          // 1/3 CAP 2 needs the point-light cap lever (lampCapMenu): UNAVAILABLE without it, SUSPENDED while a foreign value
          // holds the cap. OFF clears the slots itself and needs only the hooks.
            const int ready=lampShadowsReady.load(std::memory_order_relaxed),cap=lampCapMenu.load(std::memory_order_relaxed);
            const bool capRow=v.lampShadows==GraphicsSettings::lampShadowsCap2;
            std::snprintf(t,sizeof t,"%s%s",GraphicsSettings::lampShadowsName(v.lampShadows),
                lampShadowsProperty.load(std::memory_order_relaxed)>0?" - PROP":ready==0&&v.lampShadows?" - UNAVAILABLE":
                capRow&&cap==0?" - UNAVAILABLE":capRow&&cap==2?" - SUSPENDED":"");
            line(GraphicsMenuInput::LampShadows,"LAMP SHADOWS",t);
        }
        line(GraphicsMenuInput::Grass,"GRASS",v.grass?GraphicsSettings::grassDistanceName(v.grassDistance):"OFF"); // GRASS + GRASS DISTANCE
        { // w02: WATER (lakes / rivers in both eyes, ~1 ms GPU + ~1 ms CPU at the Blackwater lake); PROP while debug.rdrvr.vr.water overrides it
            const int prop=waterMenuProperty.load(std::memory_order_relaxed);const bool on=prop>=0?prop!=0:v.water;
            std::snprintf(t,sizeof t,"%s%s",on?"ON":"OFF",prop>=0?" - PROP":"");line(GraphicsMenuInput::Water,"WATER",t);
        }
        std::snprintf(t,sizeof t,"%u%%",v.edgeMarginPercent);line(GraphicsMenuInput::EdgeMargin,"EDGE MARGIN",t);
        line(GraphicsMenuInput::Occlusion,"OCCLUSION",graphicsOcclusionAvailable.load(std::memory_order_acquire)?(v.occlusion?"ON - EXPERIMENTAL":"OFF - EXPERIMENTAL"):"UNAVAILABLE");
        line(GraphicsMenuInput::SmallObjects,"SMALL OBJECTS",smallObjectsName(v.smallObjectsPx));
        // Pending = the saved API differs from the one the device was created with; otherwise the EFFECTIVE API,
        // with the reason when it is not the requested one and PROP when a debug property overrides the menu.
        const int activeApi=foveationActiveApi.load(std::memory_order_relaxed),startupApi=foveationStartupApi.load(std::memory_order_relaxed);
        const int levelProperty=foveationLevelProperty.load(std::memory_order_relaxed);const char* apiNote=foveationApiNote.load(std::memory_order_relaxed);
        const bool apiPending=activeApi>=0&&startupApi>=0&&unsigned(startupApi)!=v.foveationApi;
        { // AUTO shows the live level, STEREO TEST 17 (sweep) the level it applies
            const int live=foveationLiveLevel.load(std::memory_order_relaxed);
            const bool liveKnown=live>=0&&unsigned(live)<GraphicsSettings::foveationAuto;
            const bool sweep=mode==st::testModeFoveationSweep;
            char base[24];
            if(sweep&&liveKnown)std::snprintf(base,sizeof base,"%s",GraphicsSettings::foveationName(unsigned(live)));
            else if(levelProperty>=0)std::snprintf(base,sizeof base,"%s",GraphicsSettings::foveationName(unsigned(levelProperty)));
            else if(v.foveation==GraphicsSettings::foveationAuto&&liveKnown)std::snprintf(base,sizeof base,"AUTO %s",GraphicsSettings::foveationName(unsigned(live)));
            else std::snprintf(base,sizeof base,"%s",GraphicsSettings::foveationName(v.foveation));
            std::snprintf(t,sizeof t,"%s%s",base,sweep?" - SWEEP":levelProperty>=0?" - PROP":activeApi==2&&v.foveation?" - NO API":"");
            line(GraphicsMenuInput::Foveation,"FOVEATION",t);
        }
        if(activeApi<0||apiPending)std::snprintf(t,sizeof t,"%s%s",GraphicsSettings::foveationApiName(v.foveationApi),apiPending?" - AT RESTART":"");
        else std::snprintf(t,sizeof t,"%s%s%s%s",GraphicsSettings::foveationApiName(unsigned(activeApi)),
            foveationApiProperty.load(std::memory_order_relaxed)?" - PROP":"",apiNote?" - ":"",apiNote?apiNote:"");
        line(GraphicsMenuInput::FoveationApi,"FOVEATION API",t);
        { // Engine sampler knobs; STEREO TEST 18/19 drive them in A/B windows and the rows are ignored
            const int ready=textureFilterReady.load(std::memory_order_relaxed);
            const char* status=ready==0?" - UNAVAILABLE":ready==2?" - AT RESTART":"";
            std::snprintf(t,sizeof t,"%s%s",GraphicsSettings::textureFilterName(v.textureFilter),mode==st::testModeTextureFilterAB?" - A/B":status);
            line(GraphicsMenuInput::TextureFilter,"TEXTURE FILTER",t);
            std::snprintf(t,sizeof t,"%s%s",GraphicsSettings::mipBiasName(v.mipBias),mode==st::testModeMipBiasAB?" - A/B":status);
            line(GraphicsMenuInput::MipBias,"MIP BIAS",t);
        }
        { // Compositor sharpening through XR_FB_composition_layer_settings (AUTO also needs XR_META_automatic_layer_filter)
            const int layer=xrLayerSettingsEnabled.load(std::memory_order_relaxed),autoFilter=xrAutoLayerFilterEnabled.load(std::memory_order_relaxed);
            std::snprintf(t,sizeof t,"%s%s",GraphicsSettings::sharpenName(v.sharpen),layer==0||(v.sharpen==3&&autoFilter==0)?" - UNSUPPORTED":"");
            line(GraphicsMenuInput::Sharpen,"SHARPEN",t);
        }
        // What the mailbox really did with the last pair (fallback reason when FXAA was requested but copied).
        // aa_host_test compares this text byte for byte, parentheses included.
        char aaText[48];{
            const int effective=antiAliasingEffective.load(std::memory_order_relaxed),property=antiAliasingProperty.load(std::memory_order_relaxed);
            const unsigned shown=property>=0&&property<int(GraphicsSettings::antiAliasingModes)?unsigned(property):v.antiAliasing;
            if(shown>=GraphicsSettings::antiAliasingTaa){ // TAA / TAA SHARP + the render-thread TAA state (TaaPass.h)
                // bit 8 (0x100, vr::taa::menuStateAppSwLite) = AppSW runs the TAA row as TAA LITE (session only): "TAA > LITE (APPSW)"
                const int raw=taaMenuState.load(std::memory_order_relaxed),taa=raw<0?raw:raw&0xFF;
                const bool appswLite=raw>=0&&(raw&0x100)!=0&&shown==GraphicsSettings::antiAliasingTaa;
                const char* state=taa==0?" - UNAVAILABLE":taa==1?" - BUILDING":taa==3?" - FAILED":taa==4?" - NO DEPTH":taa==5?" - FORMAT":"";
                std::snprintf(aaText,sizeof aaText,"%s%s%s%s%s",GraphicsSettings::antiAliasingName(shown),GraphicsSettings::taaSharpnessSuffix(v.taaSharpness),property>=0?" (PROP)":"",appswLite?" > LITE (APPSW)":"",state);
            }
            else if(shown&&effective==2)std::snprintf(aaText,sizeof aaText,"%s (COPY: %s)",GraphicsSettings::antiAliasingName(shown),antiAliasingFallbackText(antiAliasingFallback.load(std::memory_order_relaxed)));
            else std::snprintf(aaText,sizeof aaText,"%s%s",GraphicsSettings::antiAliasingName(shown),property>=0?" (PROP)":"");
        }
        line(GraphicsMenuInput::AntiAliasing,"ANTI-ALIASING",aaText);
        { // Post FX after the stereo pair never reaches the headset; OFF = bind-only, OFF FAST = deep skip
            const int state=postFxMenuState.load(std::memory_order_relaxed),property=postFxMenuProperty.load(std::memory_order_relaxed);
            const unsigned shown=property>=0&&property<int(GraphicsSettings::postFxModes)?unsigned(property):v.postFx;
            std::snprintf(t,sizeof t,"%s%s",GraphicsSettings::postFxName(shown),mode==st::testModeSkipPostFx?" - TEST 16":property>=0?" - PROP":
                state==0&&shown!=1?" - UNAVAILABLE":state==2?" - CALIBRATING":state==4?" - FALLBACK":"");
            line(GraphicsMenuInput::PostFx,"POST FX",t);
        }
        // Effective frame lock (SPACEWARP forces LOCK 36) and the compositor's spacewarp_mode metric.
        const int swSupport=spaceWarpMenuSupport.load(std::memory_order_relaxed),swRuntime=spaceWarpMenuRuntime.load(std::memory_order_relaxed);
        const bool swOn=v.spaceWarp&&swSupport==1;
        const bool swForced=spaceWarpMenuForced.load(std::memory_order_relaxed)!=0;const char* swIssue=spaceWarpMenuIssue.load(std::memory_order_relaxed);
        {
            const int governor=frameRateGovernorHz.load(std::memory_order_relaxed); // ADAPTIVE shows its current target
            char rate[24];
            if(v.frameRate==GraphicsSettings::frameRateAdaptive&&governor>0)std::snprintf(rate,sizeof rate,"%s %d",GraphicsSettings::frameRateName(v.frameRate),governor);
            else std::snprintf(rate,sizeof rate,"%s",GraphicsSettings::frameRateName(v.frameRate));
            std::snprintf(t,sizeof t,"%s%s",rate,swForced?(v.frameRate!=1?" - 36 SPACEWARP":" - SPACEWARP"):swOn?" - SPACEWARP IDLE":"");
            line(GraphicsMenuInput::FrameRate,"FRAME RATE",t);
        }
        if(v.spaceWarp&&swSupport==0)std::snprintf(t,sizeof t,"%s - UNSUPPORTED",GraphicsSettings::spaceWarpName(v.spaceWarp));
        else if(swOn&&swIssue)std::snprintf(t,sizeof t,"%s - %s",GraphicsSettings::spaceWarpName(v.spaceWarp),swIssue); // e.g. ON - GPU OVER BUDGET
        else if(swOn&&swRuntime>=0)std::snprintf(t,sizeof t,"%s  RT %d",GraphicsSettings::spaceWarpName(v.spaceWarp),swRuntime);
        else std::snprintf(t,sizeof t,"%s",GraphicsSettings::spaceWarpName(v.spaceWarp));
        line(GraphicsMenuInput::SpaceWarp,"SPACEWARP",t);
        // XR QUEUE (device creation). AT RESTART while the saved value differs from the startup one; FALLBACK when
        // PRIORITY was requested at startup but the device runs the appended layout; PROP when a property forced a split.
        const int queueActive=xrQueueLayoutActive.load(std::memory_order_relaxed);
        const bool queuePending=v.xrQueuePriority!=s.startupXrQueuePriority;
        std::snprintf(t,sizeof t,"%s%s",GraphicsSettings::xrQueuePriorityName(v.xrQueuePriority),queuePending?" - AT RESTART":
            queueActive<0||(v.xrQueuePriority!=0)==(queueActive!=0)?"":v.xrQueuePriority?" - FALLBACK":" - PROP");
        line(GraphicsMenuInput::XrQueue,"XR QUEUE",t);
        line(GraphicsMenuInput::Profiler,"FPS COUNTER",v.profilerMode==GraphicsSettings::ProfilerMode::Off?"OFF":v.profilerMode==GraphicsSettings::ProfilerMode::Fps?"ON":"DETAIL");
        { // heat01: red HEADSET OVERHEATING lines (HeatWarning.h). PROP while debug.rdrvr.heatwarn overrides the row (2 = TEST, always
          // shown); HOT NOW while the thermal trigger is latched (shown when ON, suppressed when OFF).
            const int prop=heatWarning.property();const bool on=prop>=0?prop!=0:v.heatWarning;
            std::snprintf(t,sizeof t,"%s%s",prop==2?"TEST":on?"ON":"OFF",prop>=0?" - PROP":heatWarning.latched()?" - HOT NOW":"");
            line(GraphicsMenuInput::HeatWarning,"HEAT WARNING",t);
        }
        { // boost01: GPU BOOST (GpuBoost.h). NO HELPER = the kit's GPU_BOOST helper is not running (headset rebooted); ACTIVE = the
          // clock property holds the requested level; HOT 640 = 690 held at 640 while the overheat trigger is latched; OFF shows a
          // level still set in debug.oculus.gpuLevel (another tool such as QGO, or the helper for the 2 s until it clears it).
            const int prop=gpuBoost.property(),applied=gpuBoost.applied();
            const unsigned shown=prop==6?1u:prop==7?2u:prop==0?0u:v.gpuBoost;
            const int want=GpuBoost::levelOf(int(shown));
            char other[24]="";if(applied>=0)std::snprintf(other,sizeof other," - LEVEL %d SET",applied);
            const char* state=!want?other:!gpuBoost.helper()?" - NO HELPER":gpuBoost.heatCapped()?" - HOT 640":
                applied==gpuBoost.requested()&&applied>0?" - ACTIVE":" - WAIT";
            std::snprintf(t,sizeof t,"%s%s%s",GraphicsSettings::gpuBoostName(shown),prop>=0?" - PROP":"",state);
            line(GraphicsMenuInput::GpuBoost,"GPU BOOST",t);
        }
        line(GraphicsMenuInput::StereoTest,"STEREO TEST",st::testModeName(mode));
        ::rdrvr::teleport::menuText(t,sizeof t);line(GraphicsMenuInput::Teleport,"TELEPORT",t);
        line(GraphicsMenuInput::Defaults,"RESTORE DEFAULTS","");
        for(unsigned r=visibleCount()+1;r<unsigned(GraphicsMenuInput::Count);++r)out+='\n'; // Hidden rows keep the line count (w02: WATER took one)
        out+=s.persistenceOk?"SAVED  ":"SAVE FAILED - SESSION ONLY  ";
        out+=s.pendingRestart?"RESTART: RESOLUTION":apiPending?"RESTART: FOVEATION API":mapPending?"RESTART: SHADOW MAP":
            queuePending?"RESTART: XR QUEUE":"RES/API/MAP AT RESTART";
        return out;
    }
    // FPS HZ") GRAPHICS = text() lines 1..24 (+ " B: BACK" on the footer when it fits) ROOT /
    // MOVEMENT / HANDS / WEAPONS / CHEATS from VrMenuPages.h. Always 25 lines (heat01: GRAPHICS
    // 26; boost01: 27), <= 52 glyphs.
    std::string pageText(double newFps,double displayHz,unsigned width,unsigned height,const GraphicsSettings& settings=graphicsSettings,
                         const ::rdrvr::vr::VrSettingsStore& vrStore=::rdrvr::vr::vrSettings)const {
        namespace vr=::rdrvr::vr;
        if(pageCount()<2)return text(newFps,displayHz,width,height,settings);
        const unsigned page=page_<vr::PageCount?page_:unsigned(vr::PageRoot);
        char eye[32];std::snprintf(eye,sizeof eye,"EYE %uX%u",width,height); // GRAPHICS header, when it fits
        std::string out=vr::headerLine(page,newFps,displayHz,page==vr::PageGraphics?eye:nullptr);out+='\n';
        if(page==vr::PageRoot)vr::RootPage::text(out,input_.selected());
        else if(vr::VrPage::isVrPage(page))vr::VrPage::text(out,page,input_.selected(),vrStore);
        else if(page==vr::PageCheats)vr::CheatsPage::text(out,input_.selected());
        else if(page==vr::PageCalibration)vr::CalibPage::text(out,input_.selected());
        else if(page==vr::PageHelp)vr::HelpPage::text(out,input_.selected());
        else if(page==vr::PageButtons)vr::ButtonsPage::text(out,input_.selected());
        else if(vr::isExtPage(page)){if(const auto* x=vr::extPage(page))x->text(out,input_.selected());
            else{out+="> UNAVAILABLE\n";for(unsigned r=1;r<vr::menuBodyRows;++r)out+='\n';out+="B: BACK";}}
        else{ // GRAPHICS: text() lines 1..24; the footer gets "  B: BACK" when it fits
            const auto body=text(newFps,displayHz,width,height,settings);
            const auto first=body.find('\n');out+=first==std::string::npos?body:body.substr(first+1);
            const auto last=out.rfind('\n');
            static constexpr const char back[]="  B: BACK";
            if(last!=std::string::npos&&out.size()-last-1+std::strlen(back)<=vr::menuColumns)out+=back;
        }
        return out;
    }
};
}
