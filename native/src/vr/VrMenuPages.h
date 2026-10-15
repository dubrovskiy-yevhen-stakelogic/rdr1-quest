#pragma once
// The VR and CHEATS pages of the in-headset menu (GraphicsMenu.h owns the page switch and the GRAPHICS page),
// the page header line, and the CHEATS row registry.
// The menu is a ROOT page with submenus (all graphics options in GRAPHICS, cheats in CHEATS).
// Opening the menu (hold the left menu button) always shows ROOT:
//     GRAPHICS / MOVEMENT / HANDS / WEAPONS / CHEATS / WEAPON CALIBRATION / HUD / RECENTER
// stick up/down selects, A opens a submenu (RECENTER: A recenters), B in a submenu goes back to ROOT, B on ROOT closes.
// X/Y (extra) jump to the previous/next submenu. The VR keys are split over MOVEMENT (+ AUTO COVER),
// HANDS / WEAPONS (+ LASER SIGHT) and HUD, each VR key on exactly one of them (static_assert below).
// WEAPON CALIBRATION: offsets / rotations of the VR-held weapon per class or per weapon, live, saved in
// rdrvr/weapons.cfg (VrWeaponCalib.h). Either stick navigates (GraphicsMenuInput), so the hand holding the weapon stays free.
//
// Panel contract (PerfPanel): 25 lines x 52 glyphs, glyphs A-Z 0-9 space . : / - % ? < > only. Every page prints
// line 0 = header "MENU / CHEATS ... 36.0 FPS  72 HZ" (breadcrumb left, FPS right), lines 1..23 = rows (scrolling when a
// page has more), line 24 = footer (help / save state). Row text = "%c %-16s%.*s" exactly like the GRAPHICS rows.
//
// Threads: everything here runs on the XR thread (XrHarness::locateHands -> GraphicsMenu::update / pageText) except the
// registry's registerCheatRow (any thread, usually the main thread at startup) and the published atomics.
#include "VrSettings.h"
#include "VrWeaponCalib.h" // WEAPON CALIBRATION submenu
#include "VrControlMap.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>

namespace rdrvr::vr {

// PageRoot = the submenu list; the others are submenus (ROOT row r opens page r+1).
// WEAPON CALIBRATION and HUD come after CHEATS, so the report's page indices 0..4 stay stable. HUD
// is a VR-key page like MOVEMENT / HANDS (GraphicsMenu dispatches it through VrPage).
// + HOLSTERS (ROOT) and GIVE WEAPONS (last: NOT on ROOT, opened from CHEATS > WEAPONS BY TYPE, B goes back to CHEATS).
enum Page : unsigned {PageRoot,PageGraphics,PageMovement,PageHands,PageCheats,PageCalibration,PageHud,PageHelp,PageButtons,PageHolsters,PageGiveWeapons,PageCount};
inline constexpr std::array<const char*,PageCount> pageNames{"MENU","GRAPHICS","MOVEMENT","HANDS / WEAPONS","CHEATS","WEAPON CALIBRATION","HUD","CONTROLS","BUTTONS","HOLSTERS","GIVE WEAPONS"};
inline constexpr unsigned submenuCount=PageCount-2;
// pages whose content lives in an engine module (VrCheats.h registers HOLSTERS and GIVE WEAPONS at startup); XR thread.
struct ExtPage {unsigned (*rows)() noexcept{};void (*input)(unsigned row,int adjust,bool activate) noexcept{};void (*text)(std::string& out,unsigned selected){};};
inline std::array<std::atomic<const ExtPage*>,PageCount> extPages{};
inline std::atomic<int> pageRequest{-1}; // a row asks the menu to show another page (CHEATS > WEAPONS BY TYPE)
inline const ExtPage* extPage(unsigned page) noexcept {return page<PageCount?extPages[page].load(std::memory_order_acquire):nullptr;}
inline constexpr bool isExtPage(unsigned page) noexcept {return page==PageHolsters||page==PageGiveWeapons;}
inline constexpr unsigned menuLines=25,menuColumns=52,menuLabelWidth=16,menuValueColumns=menuColumns-2-menuLabelWidth,menuBodyRows=23;
// Published by GraphicsMenu for the report (page shown while the menu is open; -1 = never opened).
inline std::atomic<int> menuPage{-1};
inline std::atomic<bool> menuOpen{false};
inline std::atomic<std::uint32_t> pageSwitches{0};                 // X/Y jumps between submenus
inline std::atomic<std::uint32_t> menuOpens{0},menuEnters{0},menuBacks{0}; // Opens (ROOT), A on a ROOT submenu row, B in a submenu
// The VR and CHEATS pages exist unless "setprop debug.rdrvr.vr.menu 0" (then the menu is the
// GRAPHICS page alone).
inline bool pagesEnabled() noexcept {return enabled(Feature::Menu);}

inline bool panelGlyph(char c) noexcept {
    return (c>='A'&&c<='Z')||(c>='0'&&c<='9')||c==' '||c=='.'||c==':'||c=='/'||c=='-'||c=='%'||c=='?'||c=='<'||c=='>';}
inline bool panelText(const char* s) noexcept {if(!s)return false;for(;*s;++s)if(!panelGlyph(*s))return false;return true;}
// Replaces anything the panel cannot draw (lower case is upper-cased) so a module's text can never break the page.
inline void panelSanitize(char* s) noexcept {
    for(;*s;++s){if(*s>='a'&&*s<='z')*s=char(*s-'a'+'A');else if(!panelGlyph(*s))*s='?';}}

// ---- shared line builders ----
inline void appendRow(std::string& out,bool selected,const char* label,const char* value) {
    char b[96];
    const int labelLength=int(std::strlen(label));
    if(value&&*value&&labelLength>int(menuLabelWidth)) // A long label (WEAPON CALIBRATION) keeps one space before its value
        std::snprintf(b,sizeof b,"%c %s %.*s\n",selected?'>':' ',label,std::max(0,int(menuColumns)-3-labelLength),value);
    else if(value&&*value)std::snprintf(b,sizeof b,"%c %-16s%.*s\n",selected?'>':' ',label,int(menuValueColumns),value);
    else std::snprintf(b,sizeof b,"%c %s\n",selected?'>':' ',label);
    out+=b;
}
// Breadcrumb "MENU" / "MENU / CHEATS", then `extra` (e.g. "EYE 1680X1760" on GRAPHICS) when it fits, FPS on the right.
inline std::string headerLine(unsigned page,double fps,double hz,const char* extra=nullptr) {
    std::string left="MENU";
    if(page>PageRoot&&page<PageCount){left+=" / ";left+=pageNames[page];}
    char right[48];
    const double f=std::isfinite(fps)&&fps>=0&&fps<1e5?fps:0,h=std::isfinite(hz)&&hz>=0&&hz<1e5?hz:0;
    std::snprintf(right,sizeof right,"%.1f FPS  %.0f HZ",f,h);
    if(left.size()+2+std::strlen(right)>menuColumns)std::snprintf(right,sizeof right,"%.0f FPS",f);
    if(extra&&*extra&&panelText(extra)&&left.size()+2+std::strlen(extra)+2+std::strlen(right)<=menuColumns){left+="  ";left+=extra;}
    const std::size_t used=left.size()+std::strlen(right);
    left.append(used+2<=menuColumns?menuColumns-used:2,' ');
    left+=right;
    if(left.size()>menuColumns)left.resize(menuColumns);
    return left;
}
// Rows [first, first+menuBodyRows) of a page with `rows` rows so that `selected` is visible.
inline unsigned firstVisibleRow(unsigned selected,unsigned rows) noexcept {
    if(rows<=menuBodyRows||selected<menuBodyRows)return 0;
    return std::min(selected-menuBodyRows+1,rows-menuBodyRows);
}

inline constexpr std::array<Key,16> movementKeys{Key::Locomotion,Key::MoveSpeed,Key::TurnMode,Key::TeleportFade,Key::TeleportRange,Key::Roomscale,Key::Camera,
    Key::Cutscenes,Key::EyeHeight,Key::BodyUp,Key::BodyBack,Key::RideUp,Key::RideFwd,Key::BumpReactions,Key::Stumble,Key::AutoCover}; // BODY OFFSET UP / BACK under EYE HEIGHT; tp: TELEPORT FADE / RANGE after MOVE SPEED
// tp: MOVEMENT footer while LOCOMOTION is effectively TELEPORT and the cursor is on LOCOMOTION / TELEPORT FADE /
// TELEPORT RANGE (44 glyphs; the save-state text SAVE FAILED always wins). The roomscale tip waits for the device confirmation (STATUS).
inline constexpr const char* teleportFooter="L STICK FWD: AIM  RELEASE: GO  HOLD L3: WALK";
inline constexpr std::array<Key,21> handsKeys{Key::Body,Key::Hands,Key::Holsters,Key::WeaponGrip,Key::HolsterMarkers,Key::TwoHandAim,
    Key::DualWield,Key::ThrowWeapon,Key::GunSpin,Key::ReloadMode,Key::CylinderClose,Key::Chambers,Key::Hammer,Key::FanSpread,Key::LassoThrow,Key::LassoAssist,Key::Laser,
    Key::Melee,Key::MeleeSpeed,Key::MeleeFists,Key::FistBlock};
// HANDS / WEAPONS footer while the cursor is on LASSO THROW and the lasso is not owned
// (the row note NOT OWNED, published by the lasso module); SAVE FAILED always wins.
inline constexpr const char* lassoNotOwnedFooter="CHEATS > GIVE WEAPON > LASSO";
// HAMMER: what the selected style does (panel glyphs, <= 52 columns)
inline constexpr const char* hammerRealisticFooter="COCK HAMMER THEN TRIGGER  HELD TRIGGER/SLAP: FAN";
inline constexpr const char* hammerCasualFooter="TRIGGER FIRES  HAMMER TO FULL COCK: FAN";
// CHAMBERS: what REALISTIC does (panel glyphs, <= 52 columns)
inline constexpr const char* chambersRealisticFooter="ONLY THE ALIGNED ROUND FIRES  SWIPE CYLINDER: SPIN";
// WEAPON PRESET: row 0 of HANDS / WEAPONS is no key (VrSettings.h weaponPresetKeys; no packed bits). keyAt returns this pseudo key
// (> Key::Count, so every key-indexed path skips it) and VrPage handles it itself; the other HANDS rows move down by one.
inline constexpr Key weaponPresetRow=Key(keyCount+1);
inline constexpr std::array<const char*,PresetCount> weaponPresetFooters{"FINGER SPIN  MANUAL RELOAD  COCK THE HAMMER",
    "TRIGGER FIRES  B: GAME RELOAD  RELEASE DROPS","TRIGGERS / A: REALISTIC OR CASUAL"};
// HUD page: the game's HUD as VR panels (VrHud.h).
inline constexpr std::array<Key,4> hudKeys{Key::Hud,Key::HudSize,Key::QuestMarker,Key::Subtitles};
inline constexpr const char* hudHandFooter="MAP ON LEFT WRIST - LOOK AT IT  TEXT IN FRONT";
inline constexpr const char* hudHeadFooter="WHOLE HUD IN FRONT - FOLLOWS YOUR VIEW";
inline constexpr bool vrKeysPlacedOnce() noexcept {
    for(unsigned k=0;k<keyCount;++k){
        unsigned n=0;
        for(Key m:movementKeys)n+=unsigned(m)==k;
        for(Key h:handsKeys)n+=unsigned(h)==k;
        for(Key h:hudKeys)n+=unsigned(h)==k;
        if(n!=1)return false;
    }
    return movementKeys.size()<=menuBodyRows&&handsKeys.size()<=menuBodyRows&&hudKeys.size()<=menuBodyRows;
}
static_assert(vrKeysPlacedOnce(),"every VR key must be on exactly one VR submenu (MOVEMENT, HANDS / WEAPONS or HUD)");
static_assert(handsKeys.size()+1<=menuBodyRows,"HANDS / WEAPONS = WEAPON PRESET + handsKeys");

struct VrPage {
    static constexpr bool isVrPage(unsigned page) noexcept {return page==PageMovement||page==PageHands||page==PageHud;}
    static constexpr unsigned rows(unsigned page) noexcept {
        return page==PageMovement?unsigned(movementKeys.size()):page==PageHands?unsigned(handsKeys.size())+1u:page==PageHud?unsigned(hudKeys.size()):0u;}
    // Key on `row` of a VR submenu (Key::Count when out of range; HANDS row 0 = weaponPresetRow).
    static constexpr Key keyAt(unsigned page,unsigned row) noexcept {
        if(page==PageHands)return row==0?weaponPresetRow:row-1<handsKeys.size()?handsKeys[row-1]:Key::Count;
        return page==PageMovement&&row<movementKeys.size()?movementKeys[row]:
            page==PageHud&&row<hudKeys.size()?hudKeys[row]:Key::Count;}
    static const char* label(Key k) noexcept {return k==weaponPresetRow?"WEAPON PRESET":unsigned(k)<keyCount?info(k).label:"";}
    // Suffix by precedence: property kill switch / module PROP, UNAVAILABLE, FAILED, then the module note (rows that show it).
    static const char* suffix(Key k) noexcept {
        const auto& i=info(k);
        if(k==Key::Locomotion)return locomotionSuffix(vrSettings.requested()[Key::Locomotion]);
        if(keyKilled(k))return "PROP";
        const auto c=status::code(i.feature);
        if(c==status::Property)return "PROP";
        if(c==status::Unavailable)return "UNAVAILABLE";
        if(c==status::Failed)return "FAILED";
        if(c==status::Ready&&i.showNote){const char* n=status::noteOf(i.feature);if(n&&*n)return n;}
        return nullptr;
    }
    // LOCOMOTION - PROP when a kill switch overrides the REQUESTED mode; with an effective TELEPORT the
    // teleport module's status (UNAVAILABLE -> "UNAVAILABLE - FREE": the FREE path runs) and note;
    // otherwise the Loco status.
    static const char* locomotionSuffix(unsigned requested) noexcept {
        if(locomotionKilledFor(requested))return "PROP";
        if(locomotionEffective(requested)==2){
            const auto c=status::code(Feature::TpLoco);
            if(c==status::Property)return "PROP";
            if(c==status::Unavailable)return "UNAVAILABLE - FREE";
            if(c==status::Failed)return "FAILED";
            if(c==status::Ready){const char* n=status::noteOf(Feature::TpLoco);if(n&&*n)return n;}
            return nullptr;
        }
        const auto c=status::code(Feature::Loco);
        if(c==status::Property)return "PROP";
        if(c==status::Unavailable)return "UNAVAILABLE";
        if(c==status::Failed)return "FAILED";
        if(c==status::Ready){const char* n=status::noteOf(Feature::Loco);if(n&&*n)return n;}
        return nullptr;
    }
    // Value text of VR key index `key` (unsigned(Key)).
    static void value(unsigned key,const VrSettingsStore& store,char* out,std::size_t size) noexcept {
        if(!size)return;
        out[0]=0;
        if(key==unsigned(weaponPresetRow)){std::snprintf(out,size,"%s",weaponPresetNames[weaponPresetOf(store.requested())]);return;}
        if(key>=keyCount)return;
        const Key k=Key(key);
        const auto e=store.effective();
        const unsigned v=e[k];
        char base[24];
        const int mm=status::eyeHeightMm.load(std::memory_order_relaxed);
        if(k==Key::EyeHeight&&v==0&&mm>=500&&mm<=2500)std::snprintf(base,sizeof base,"AUTO %.2f",double(mm)/1000.0);
        else std::snprintf(base,sizeof base,"%s",info(k).names[v<info(k).count?v:0]);
        const char* s=k==Key::Locomotion?locomotionSuffix(store.requested()[k]):suffix(k);
        if(s)std::snprintf(out,size,"%s - %s",base,s);else std::snprintf(out,size,"%s",base);
        panelSanitize(out);
    }
    // XR thread. A key forced by a kill switch ignores input (the property wins, as the PROP suffix says).
    static void input(unsigned key,int adjust,bool activate,VrSettingsStore& store) noexcept {
        if(key==unsigned(weaponPresetRow)){ // Right trigger CASUAL, left trigger REALISTIC, A toggles (CUSTOM -> REALISTIC)
            if(!adjust&&!activate)return;
            const unsigned cur=weaponPresetOf(store.requested());
            const unsigned next=adjust>0?unsigned(PresetCasual):adjust<0?unsigned(PresetRealistic):cur==PresetRealistic?unsigned(PresetCasual):unsigned(PresetRealistic);
            if(next!=cur)store.applyWeaponPreset(next);
            return;}
        if(key>=keyCount||keyKilled(Key(key)))return;
        if(adjust||activate)store.adjust(Key(key),adjust);
    }
    static void text(std::string& out,unsigned page,unsigned selected,const VrSettingsStore& store) {
        char v[64];const unsigned n=rows(page);
        for(unsigned r=0;r<n;++r){const Key k=keyAt(page,r);value(unsigned(k),store,v,sizeof v);appendRow(out,r==selected,label(k),v);}
        for(unsigned r=n;r<menuBodyRows;++r)out+='\n';
        const Key sel=keyAt(page,selected);
        if(store.persistenceOk()&&page==PageMovement&&store.effective().teleportLocomotion()&&
           (sel==Key::Locomotion||sel==Key::TeleportFade||sel==Key::TeleportRange)){out+=teleportFooter;return;}
        if(store.persistenceOk()&&page==PageHands&&sel==Key::LassoThrow&&!keyKilled(Key::LassoThrow)){
            const char* note=status::noteOf(Feature::Lasso);
            if(note&&std::strcmp(note,"NOT OWNED")==0&&status::code(Feature::Lasso)==status::Ready){out+=lassoNotOwnedFooter;return;}}
        if(store.persistenceOk()&&page==PageHands&&sel==weaponPresetRow){out+=weaponPresetFooters[weaponPresetOf(store.requested())];return;}
        if(store.persistenceOk()&&page==PageHands&&sel==Key::Hammer&&!keyKilled(Key::Hammer)){
            out+=store.requested().hammerStyle()?hammerRealisticFooter:hammerCasualFooter;return;}
        if(store.persistenceOk()&&page==PageHands&&sel==Key::Chambers&&!keyKilled(Key::Chambers)&&store.requested().realisticChambers()){
            out+=chambersRealisticFooter;return;}
        if(store.persistenceOk()&&page==PageHud&&!keyKilled(Key::Hud)&&store.effective().hudMode()){
            out+=store.effective().hudMode()==2?hudHandFooter:hudHeadFooter;return;}
        out+=store.persistenceOk()?"SAVED  ":"SAVE FAILED - SESSION ONLY  ";
        out+="A/STICK: CHANGE  B: BACK";
    }
};

// ---- CHEATS page row registry ----
// A row is a label plus up to three callbacks, all called on the XR thread while the menu is open:
//   valueText(ctx, out, size)  value column text (panel glyphs; anything else is replaced by '?'), may be null
//   onAdjust(ctx, +1/-1)       stick right/left, may be null
//   onActivate(ctx)            A, may be null
// Callbacks must be cheap and must not block or touch the engine: engine work goes to the main-thread queue
// (VrMainThread, integration), state comes back through atomics. Rows are permanent once registered (no removal).
struct CheatRow {
    const char* label{};                                               // static storage, 1..16 panel glyphs
    void (*valueText)(void* context,char* out,std::size_t size) noexcept{};
    void (*onAdjust)(void* context,int direction) noexcept{};
    void (*onActivate)(void* context) noexcept{};
    void* context{};
};
class CheatRegistry {
public:
    static constexpr unsigned capacity=96;
    // Any thread. false: CHEATS killed by debug.rdrvr.vr.cheats 0, bad label, or full.
    bool add(const CheatRow& row) noexcept {
        if(killed(Feature::Cheats)||!row.label||!*row.label||std::strlen(row.label)>menuLabelWidth||!panelText(row.label))
            {rejected_.fetch_add(1,std::memory_order_relaxed);return false;}
        std::lock_guard lock(mutex_);
        const unsigned n=count_.load(std::memory_order_relaxed);
        if(n>=capacity){rejected_.fetch_add(1,std::memory_order_relaxed);return false;}
        rows_[n]=row;count_.store(n+1,std::memory_order_release);return true;
    }
    unsigned count() const noexcept {return count_.load(std::memory_order_acquire);}
    const CheatRow& row(unsigned i) const noexcept {return rows_[i<capacity?i:0];}
    std::uint32_t rejected() const noexcept {return rejected_.load(std::memory_order_relaxed);}
    // Host tests only (not safe against a concurrent reader).
    void resetForTest() noexcept {std::lock_guard lock(mutex_);count_.store(0);rows_={};rejected_.store(0);}
private:
    std::mutex mutex_;
    std::array<CheatRow,capacity> rows_{};
    std::atomic<unsigned> count_{0};
    std::atomic<std::uint32_t> rejected_{0};
};
inline CheatRegistry cheatRegistry;
inline bool registerCheatRow(const CheatRow& row) noexcept {return cheatRegistry.add(row);}
// CHEATS footer: the last action's result as static text in panel glyphs (e.g. "GIVEN: SCHOFIELD"), nullptr = help text.
inline std::atomic<const char*> cheatNote{nullptr};
inline std::atomic<std::uint32_t> cheatAdjusts{0},cheatActivations{0};

struct CheatsPage {
    static unsigned rows() noexcept {const unsigned n=cheatRegistry.count();return n?n:1u;}
    static void input(unsigned row,int adjust,bool activate) noexcept {
        if(killed(Feature::Cheats)||row>=cheatRegistry.count())return;
        const auto& r=cheatRegistry.row(row);
        if(adjust&&r.onAdjust){cheatAdjusts.fetch_add(1,std::memory_order_relaxed);r.onAdjust(r.context,adjust>0?1:-1);}
        if(activate&&r.onActivate){cheatActivations.fetch_add(1,std::memory_order_relaxed);r.onActivate(r.context);}
    }
    static void text(std::string& out,unsigned selected) {
        const unsigned n=cheatRegistry.count();
        if(!n){
            appendRow(out,true,killed(Feature::Cheats)?"DISABLED - PROP":"NOT YET","");
            for(unsigned r=1;r<menuBodyRows;++r)out+='\n';
        }else{
            const unsigned first=firstVisibleRow(selected,n);
            char v[64];
            for(unsigned r=first;r<first+menuBodyRows;++r){
                if(r>=n){out+='\n';continue;}
                const auto& row=cheatRegistry.row(r);v[0]=0;
                if(row.valueText){row.valueText(row.context,v,sizeof v);v[sizeof v-1]=0;panelSanitize(v);}
                appendRow(out,r==selected,row.label,v);
            }
        }
        char footer[80];const char* note=cheatNote.load(std::memory_order_acquire);
        char shown[48];std::snprintf(shown,sizeof shown,"%s",note&&*note?note:"A: DO  STICK: CHOOSE  B: BACK");panelSanitize(shown);
        if(n>menuBodyRows)std::snprintf(footer,sizeof footer,"%.40s  %u/%u",shown,selected+1,n);
        else std::snprintf(footer,sizeof footer,"%s",shown);
        footer[menuColumns]=0;out+=footer;
    }
};

// ---- WEAPON CALIBRATION page ----
// The weapon in a VR hand (or the last one held). Hold it in one hand and use EITHER stick (GraphicsMenuInput takes the stronger
// one): the gun moves live. Offsets 0.5 cm, rotations 2 deg; A on a value row zeroes it; RESET drops the scope's entry.
inline std::atomic<std::uint32_t> calibEdits{0},calibResets{0},calibScopeToggles{0};
struct CalibPage {
    enum Row : unsigned {RowWeapon,RowScope,RowX,RowY,RowZ,RowPitch,RowYaw,RowRoll,RowReset,RowCount};
    static constexpr unsigned rows=RowCount;
    static constexpr std::array<const char*,RowCount> labels{"WEAPON","SAVE FOR","OFFSET X","OFFSET Y","OFFSET Z","PITCH","YAW","ROLL","RESET"};
    static constexpr std::array<const char*,calib::FieldCount> directions{"RIGHT","UP","FORWARD","UP","LEFT","CW"};
    static void value(unsigned row,const calib::Current& c,char* out,std::size_t size) noexcept {
        if(!size)return;out[0]=0;
        const bool known=c.kind>=0&&unsigned(c.kind)<calib::classCount;
        const bool scopeType=calib::scopeType.load(std::memory_order_relaxed);
        if(row==RowWeapon){
            if(!known){std::snprintf(out,size,"NONE - DRAW A WEAPON");return;}
            char name[20];calib::shortName(c.name,name,sizeof name);
            std::snprintf(out,size,"%s%s%s%s",name,*name?" - ":"",calib::classLabels[unsigned(c.kind)],c.held?"":" - LAST");panelSanitize(out);return;
        }
        if(!known){std::snprintf(out,size,"-");return;}
        if(row==RowScope){if(scopeType)std::snprintf(out,size,"THIS WEAPON ONLY");else std::snprintf(out,size,"ALL %s",calib::classLabels[unsigned(c.kind)]);return;}
        if(row==RowReset){
            const bool own=calib::store.hasType(c.typeId);
            if(scopeType)std::snprintf(out,size,own?"A: THIS WEAPON":"NOTHING TO RESET");
            else std::snprintf(out,size,"A: ALL %s",calib::classLabels[unsigned(c.kind)]);
            return;}
        const unsigned f=row-RowX;if(f>=calib::FieldCount)return;
        const auto e=calib::store.entryFor(c.kind,c.typeId,scopeType);const int v=e.v[f];
        if(f<calib::Pitch)std::snprintf(out,size,"%s%d.%d CM%s%s",v<0?"-":"",std::abs(v)/10,std::abs(v)%10,v?"  ":"",v?directions[f]:"");
        else std::snprintf(out,size,"%d DEG%s%s",v,v?"  ":"",v?directions[f]:"");
        panelSanitize(out);
    }
    static void input(unsigned row,int adjust,bool activate) noexcept {
        const auto c=calib::currentWeapon();
        if(c.kind<0||unsigned(c.kind)>=calib::classCount)return;
        calib::store.ensureLoaded();
        const bool scopeType=calib::scopeType.load(std::memory_order_relaxed);
        if(row==RowScope&&(adjust||activate)){calib::scopeType.store(!scopeType,std::memory_order_relaxed);calibScopeToggles.fetch_add(1,std::memory_order_relaxed);return;}
        if(row==RowReset){if(activate&&calib::store.reset(c.kind,c.typeId,scopeType))calibResets.fetch_add(1,std::memory_order_relaxed);return;}
        if(row<RowX||row>RowRoll)return;
        const unsigned f=row-RowX;
        if(adjust){calib::store.edit(c.kind,c.typeId,scopeType,f,adjust>0?1:-1);calibEdits.fetch_add(1,std::memory_order_relaxed);}
        else if(activate){calib::store.edit(c.kind,c.typeId,scopeType,f,0,true);calibEdits.fetch_add(1,std::memory_order_relaxed);}
    }
    static void text(std::string& out,unsigned selected) {
        calib::store.ensureLoaded();
        const auto c=calib::currentWeapon();char v[64];
        for(unsigned r=0;r<rows;++r){value(r,c,v,sizeof v);appendRow(out,r==selected,labels[r],v);}
        out+='\n';
        out+="  HOLD THE WEAPON - EDIT WITH EITHER STICK\n";
        out+="  STICK </>: 0.5 CM / 2 DEG  A: ZERO ROW\n";
        for(unsigned r=rows+3;r<menuBodyRows;++r)out+='\n';
        out+=calib::store.persistenceOk()?"SAVED  ":"SAVE FAILED - SESSION ONLY  ";
        out+="B: BACK";
    }
};

// ---- CONTROLS page: how the VR controls work (read only; the stick scrolls) ----
struct HelpPage {
    static constexpr std::array<const char*,50> lines{ // + 2 WEAPON PRESET lines
        "\x03ON FOOT - DEFAULT BUTTONS, CHANGE THEM IN BUTTONS",
        "  A JUMP    B RELOAD / INTERACT    X WHISTLE",
        "  Y USE / MOUNT    L3 SPRINT    R3 CROUCH",
        "  LT + R3 DEAD EYE    RIGHT STICK TURN",
        "  EMPTY HAND AT THE MOUTH + GRIP: WHISTLE",
        "  HOLD THE LEFT MENU BUTTON: THIS MENU",
        "\x03HORSE",
        "  Y MOUNT / DISMOUNT    LEFT STICK STEER",
        "  A TAP REPEATEDLY: SPUR / GALLOP    B JUMP",
        "  CAMERA HEIGHT: MOVEMENT > HORSE CAM UP / FWD",
        "  REVOLVER, RIFLES AND THROWN WORK IN THE SADDLE",
        "\x03HOLSTERS - REACH AND GRIP TO DRAW",
        "  REVOLVER: RIGHT HIP    LASSO: LEFT HIP",
        "  KNIFE: BELT FRONT LEFT    THROWN: CHEST",
        "  RIFLE / REPEATER: BEHIND THE RIGHT SHOULDER",
        "  SHOTGUN: BEHIND THE LEFT SHOULDER",
        "  SNIPER: BEHIND THE HEAD",
        "  AN EMPTY BACK SLOT GIVES ANY OTHER LONG GUN",
        "  OPEN THE GRIP AT ITS HOLSTER: PUT AWAY",
        "  MENU > HOLSTERS: CHOOSE THE GUN FOR EACH ONE",
        "\x03WEAPONS",
        "  TRIGGER: FIRE    OTHER HAND ON THE BARREL: AIM",
        "  GRIP THE GUN WITH THE OTHER HAND: SWAP HANDS",
        "  THROW: RELEASE THE GRIP WHILE SWINGING",
        "  MENU > HANDS / WEAPONS > WEAPON PRESET",
        "  CASUAL: TRIGGER FIRES  B: GAME RELOAD  NO SPIN",
        "\x04 GUN SPIN",
        "  OPEN THE GRIP: THE GUN HANGS ON YOUR FINGER",
        "  TILT TO DROP IT, GRIP TO CATCH IT",
        "\x04 REVOLVER RELOAD - RELOAD MODE SINGLE / FULL",
        "  B: CYLINDER OUT",
        "  MUZZLE UP + SHAKE DOWN: EJECT THE SHELLS",
        "  FREE HAND GRIP AT CHEST / BELT: TAKE A ROUND",
        "  PUT IT INTO AN EMPTY CHAMBER",
        "  CLOSE: SHORT B, PUSH THE CYLINDER OR FLICK",
        "  LONG B: GAME RELOAD / AUTO-LOAD WHEN OPEN",
        "\x04 HAMMER REALISTIC - SINGLE-ACTION REVOLVERS",
        "  DRAG THE HAMMER BACK WITH THE FREE PALM",
        "  THEN TRIGGER    TRIGGER HELD + SLAP: FAN",
        "\x03LASSO",
        "  DRAW IT FROM ITS HOLSTER, SWING THE LOOP",
        "  AND RELEASE TOWARDS THE TARGET - ARC SHOWN",
        "  CAUGHT: FOLLOW THE GAME PROMPTS - A / X",
        "\x03MELEE",
        "  EMPTY HANDS: PUNCH WITH YOUR FISTS",
        "  RAISE BOTH FISTS TO BLOCK",
        "\x03MENU",
        "  STICK UP / DOWN: SELECT    A: CHANGE / OPEN",
        "  RIGHT TRIGGER: NEXT VALUE    LEFT TRIGGER: BACK",
        "  B: BACK / CLOSE    X / Y: PREVIOUS / NEXT PAGE"};
    static constexpr unsigned rows=unsigned(lines.size());
    static void text(std::string& out,unsigned selected) {
        const unsigned first=firstVisibleRow(selected,rows);
        unsigned shown=0;
        for(unsigned r=first;r<rows&&shown<menuBodyRows;++r,++shown){out+=r==selected?"> ":"  ";out+=lines[r];out+='\n';}
        for(;shown<menuBodyRows;++shown)out+='\n';
        out+="STICK: SCROLL  B: BACK";
    }
};

// ---- BUTTONS page: which Touch button does each action, on foot and on a horse ----
struct ButtonsPage {
    static constexpr unsigned resetRow=controlmap::LayoutCount*controlmap::buttonCount,rows=resetRow+1;
    static constexpr std::array<const char*,controlmap::LayoutCount*controlmap::buttonCount> labels{
        "JUMP","RELOAD/INTERACT","WHISTLE","USE / MOUNT","SPRINT","CROUCH",
        "HORSE SPUR","HORSE JUMP","HORSE X","HORSE DISMOUNT","HORSE L3","HORSE DEAD EYE"};
    static void input(unsigned row,int adjust,bool activate) noexcept {
        if(row==resetRow){if(activate)controlmap::resetDefaults();return;}
        if(adjust)controlmap::step(row,adjust);else if(activate)controlmap::step(row,1);
    }
    static void text(std::string& out,unsigned selected) {
        controlmap::loadOnce();const auto m=controlmap::current();
        for(unsigned r=0;r<resetRow;++r){const unsigned l=r/controlmap::buttonCount,b=r%controlmap::buttonCount;
            if(r==controlmap::buttonCount)out+="  ON A HORSE\n";else if(r==0)out+="  ON FOOT\n";
            appendRow(out,r==selected,labels[r],controlmap::buttonNames[m.physical[l][b]]);}
        appendRow(out,selected==resetRow,"RESET DEFAULTS","PRESS A");
        for(unsigned r=rows+2;r<menuBodyRows;++r)out+='\n';
        out+="TRIGGERS / A: CHANGE  TAKEN BUTTONS SWAP";
    }
};

// ---- ROOT page: the submenus in Page order, then RECENTER ----
struct RootPage {
    static constexpr unsigned recenterRow=submenuCount,resetRow=submenuCount+1,rows=submenuCount+2; // + RESET ALL SETTINGS
    static constexpr unsigned pageOfRow(unsigned row) noexcept {return row<submenuCount?row+1:unsigned(PageRoot);}
    static constexpr unsigned rowOfPage(unsigned page) noexcept {return page==PageGiveWeapons?PageCheats-1:page>PageRoot&&page<PageCount?page-1:0u;}
    static const char* label(unsigned row) noexcept {return row<submenuCount?pageNames[row+1]:row==recenterRow?"RECENTER":row==resetRow?"RESET ALL":"";}
    // RESET ALL (user): GRAPHICS, VR pages and BUTTONS back to the release defaults; A arms it, a second A within 4 s confirms.
    static std::uint64_t nowNs() noexcept {return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
    static inline std::atomic<std::uint64_t> resetArmedNs{0},resetDoneNs{0};
    static bool resetArmed() noexcept {const auto a=resetArmedNs.load(std::memory_order_relaxed);return a&&nowNs()-a<4'000'000'000ull;}
    // true = the caller resets now (second press); false = armed (first press)
    static bool resetPress() noexcept {if(resetArmed()){resetArmedNs.store(0);resetDoneNs.store(nowNs());return true;}resetArmedNs.store(nowNs());return false;}
    static void value(unsigned row,char* out,std::size_t size) noexcept {
        if(!size)return;
        out[0]=0;
        switch(pageOfRow(row)){
        case PageGraphics:std::snprintf(out,size,"RESOLUTION SHADOWS AA FPS");break;
        case PageMovement:std::snprintf(out,size,"WALK TELEPORT CAMERA CUTSCENES");break;
        case PageHelp:std::snprintf(out,size,"HOW TO PLAY IN VR");break;
        case PageHolsters:std::snprintf(out,size,"WHICH GUN IN EACH HOLSTER");break;
        case PageButtons:std::snprintf(out,size,"%s",controlmap::packed.load(std::memory_order_relaxed)?"CUSTOM":"DEFAULT");break;
        case PageHands:std::snprintf(out,size,"BODY HOLSTERS GUNS LASSO MELEE");break;
        case PageCheats:{const unsigned n=cheatRegistry.count();
            if(killed(Feature::Cheats))std::snprintf(out,size,"DISABLED - PROP");
            else if(!n)std::snprintf(out,size,"NONE YET");
            else std::snprintf(out,size,"%u CHEATS",n);
            break;}
        case PageHud:{
            const unsigned m=vrSettings.effective().hudMode();
            std::snprintf(out,size,"%s",keyKilled(Key::Hud)?"OFF - PROP":m==2?"HAND - MAP ON WRIST":m==1?"HEAD":"OFF");break;}
        case PageCalibration:{ // The weapon it would edit (fits 52 columns with the 18-glyph label)
            const auto c=calib::currentWeapon();
            if(c.kind<0||unsigned(c.kind)>=calib::classCount){std::snprintf(out,size,"NO WEAPON YET");break;}
            char name[20];calib::shortName(c.name,name,sizeof name);
            std::snprintf(out,size,"%.16s",*name?name:calib::classLabels[unsigned(c.kind)]);panelSanitize(out);break;}
        default:if(row==recenterRow)std::snprintf(out,size,"PRESS A");
            else if(row==resetRow){const auto d=resetDoneNs.load(std::memory_order_relaxed);
                std::snprintf(out,size,"%s",resetArmed()?"PRESS A AGAIN TO RESET":d&&nowNs()-d<3'000'000'000ull?"DONE - DEFAULTS RESTORED":"DEFAULT SETTINGS");}
            break;
        }
    }
    static void text(std::string& out,unsigned selected) {
        char v[48];
        for(unsigned r=0;r<rows;++r){value(r,v,sizeof v);appendRow(out,r==selected,label(r),v);}
        for(unsigned r=rows;r<menuBodyRows;++r)out+='\n';
        out+="STICK: SELECT  A: OPEN  B: CLOSE";
    }
};

// Report object for vr.menu (integration: GameXr report under "vr" -> "menu").
// page = Page index (0 ROOT, 1 GRAPHICS, 2 MOVEMENT, 3 HANDS / WEAPONS, 4 CHEATS; -1 = never opened).
inline std::string menuReport() {
    std::string out="{\"pagesEnabled\":";out+=pagesEnabled()?"true":"false";
    const int page=menuPage.load(std::memory_order_relaxed);
    out+=",\"page\":"+std::to_string(page);
    out+=",\"pageName\":\"";out+=page>=0&&page<int(PageCount)?pageNames[unsigned(page)]:"";out+='"';
    out+=",\"open\":";out+=menuOpen.load(std::memory_order_relaxed)?"true":"false";
    out+=",\"atRoot\":";out+=page==int(PageRoot)?"true":"false";
    out+=",\"opens\":"+std::to_string(menuOpens.load(std::memory_order_relaxed));
    out+=",\"enters\":"+std::to_string(menuEnters.load(std::memory_order_relaxed));
    out+=",\"backs\":"+std::to_string(menuBacks.load(std::memory_order_relaxed));
    out+=",\"pageSwitches\":"+std::to_string(pageSwitches.load(std::memory_order_relaxed));
    out+=",\"rows\":{\"root\":"+std::to_string(RootPage::rows)+",\"movement\":"+std::to_string(VrPage::rows(PageMovement))+
         ",\"hands\":"+std::to_string(VrPage::rows(PageHands))+",\"cheats\":"+std::to_string(cheatRegistry.count())+
         ",\"calibration\":"+std::to_string(CalibPage::rows)+'}';
    {
        const auto c=calib::currentWeapon();
        out+=",\"calibration\":{\"edits\":"+std::to_string(calibEdits.load(std::memory_order_relaxed))+",\"resets\":"+
             std::to_string(calibResets.load(std::memory_order_relaxed))+",\"scopeToggles\":"+std::to_string(calibScopeToggles.load(std::memory_order_relaxed))+
             ",\"scope\":\"";out+=calib::scopeType.load(std::memory_order_relaxed)?"WEAPON":"CLASS";
        out+="\",\"weapon\":{\"kind\":"+std::to_string(c.kind)+",\"typeId\":"+std::to_string(c.typeId)+",\"held\":";out+=c.held?"true":"false";
        char name[20];calib::shortName(c.name,name,sizeof name);out+=",\"name\":\"";out+=name;out+="\"},\"store\":"+calib::store.report()+'}';
    }
    out+=",\"killSwitchRead\":";out+=killSwitchRead.load(std::memory_order_relaxed)?"true":"false";
    out+=",\"recenterRequests\":"+std::to_string(recenterRequests.load(std::memory_order_relaxed));
    out+=",\"cheatRows\":"+std::to_string(cheatRegistry.count())+",\"cheatRowsRejected\":"+std::to_string(cheatRegistry.rejected());
    out+=",\"cheatAdjusts\":"+std::to_string(cheatAdjusts.load(std::memory_order_relaxed));
    out+=",\"cheatActivations\":"+std::to_string(cheatActivations.load(std::memory_order_relaxed));
    out+=",\"status\":{";
    for(unsigned f=1;f<featureCount;++f){ // Menu has no status slot of its own
        if(f>1)out+=',';
        const int c=status::code(Feature(f));const char* n=status::noteOf(Feature(f));
        out+='"';out+=featureNames[f];out+="\":{\"code\":"+std::to_string(c)+",\"note\":\"";out+=n?n:"";out+="\"}";
    }
    out+="},\"settings\":"+vrSettings.report()+'}';
    return out;
}

} // namespace rdrvr::vr
