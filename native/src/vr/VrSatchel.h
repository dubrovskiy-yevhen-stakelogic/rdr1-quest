#pragma once
// SATCHEL: a VR menu ROOT row that opens the game's own satchel, by the game's own route.
//
// Engine facts (librdr.so 1.58 arm64):
//  - INVENTORY = DigitalGameAction(group 6 "GENERIC", "INVENTORY", Binding(13, pad), Binding(20, pad), ...) (static initializer 0x1ac74b8);
//    Android key 109 -> d11 pad 13 (AndroidGamepad_c::GamepadButtonEvent 0x198ca7c, jump table 0x129d724 -> strb +0x35d). No Touch button has it,
//    so it is a pulse of PadSelect (this row) or the hold of X (VrControls Mapper).
//  - Released -> fuiEvent '@GENERIC.INVENTORY*RELEASED' -> rdrHudScene::HandleInputs 0x29fa0f8 (branch 0x29fa174). This is the satchel, not
//    the weapon wheel. The game's own gates, in order: gdGameData state bit 4, hudManager+0x2586d, SaveGame_DoNotInterruptMe hidden, player valid,
//    no vehicle seat at +0xcb0, sagPlayer+0x104 / +0x10c / +0x10d clear, not in a scope or binoculars, hudManager+0xc548 clear, HudSceneOffline
//    visible, "SatchelMenu" hidden, rdrSatchel::IsEnabled (vtable +0x60; sanSatchelSetEnable writes it through +0x68), not 'beingbuckedoff' /
//    'meleegroundattack', no active physics inst (ragdoll), mover not busy, not diving. Then UINavigator::StackPush(SatchelMenu, true) 0x29fa7b0.
//  - Inside: rdrSatchel::HandleInputs 0x256c964: @UI.NAVIGATE_UP/DOWN (stick), @UI.ACCEPT*RELEASED = OnItemUsed (pad 0 A),
//    @UI.CANCEL*RELEASED (pad 1 B) or @GENERIC.INVENTORY*RELEASED = UIManager::StackPop("SatchelMenu"); tabs @UI.PREVIOUS_TAB / NEXT_TAB = pad 14 / 15
//    (the grips). PauseMenuScene can host it too (rdrSatchel::OnActivate 0x256bd8c branches on it); route PAUSE in the report.
//  - USE. All stock, from the code and the game's own data:
//    scene file (content_android.rpf, entry 0x3bb161c2): prompts sps_use {@UI.ACCEPT}, sps_cancel {@UI.CANCEL}, sps_info {@UI.ACTION_1}; ACCEPT = OnItemUsed,
//    ACTION_1 = ToggleDetailView. Bindings (0x1ac7d68..): ACTION_1 = pad 3 Y, ACTION_2 = pad 2 X (outfit track, not in this scene), so X has no job here.
//    Tabs: Consumables (mode 1, invGringoType, GetInvTypeID 1), Accessories (mode 2, invGringoAccessoryType, 2), Collectables (mode 3, invCollectableType, 5: skins, plants,
//    treasure). OnItemUsed 0x256d050: only ids 1 / 2 with a gringo show sps_use, so A on a skin is a no-op and the satchel stays open.
//    A usable item: hudManager+0xc548 = the item, +0xc550 = 0; hudManager::UpdateUseNowStatus 0x29e7f98 (sagShellDerived::Update): 0 Request type, 1 create the gringo,
//    2 script + anims loaded -> UINavigator::StackPop(null) = THE SATCHEL CLOSES ITSELF (0x29e81c0) -> 3 vtable +0x30 OnUseItem 0x2675560 -> 4 wait for inventory+0x458 clear
//    (EndUse), DeleteItem at 0. The item script applies the effect after the close (Meds: health, Snake Oil / Tonic / Moonshine: Dead Eye) and can refuse with a HUD help
//    message and no use: user_full_health, already_moonshine, pills_no_horse, item_bait_*, item_combat_fail, item_hogtie_fail.
//  - Display: the scene hierarchy lives in game data, so whether HudScene hides (IsMenuActive) is not provable from the code. An open satchel
//    forces PresentationContext::Menu like the pause scene (TraceEntry uiMenuUnlessCutscene): flat, Layout::Native = stock pad.
//
// Threads: request / notePulse on the XR thread; selectNow / noteHold on the native input thread (controls pad filter); step on the engine main
// thread (the NativeUiContext owner, every few game updates); atomics between. The pad press is the only way in: a direct StackPush from the mod's
// main-thread slot would race the UI update of TaskProcessHUD and skip the gates above.
// Lever (read at 4 Hz or so from the main thread): debug.rdrvr.vr.satchel 0 = row off; debug.rdrvr.vr.gamemenu.force 0 = no forced flat view.
// evidence (report only, nothing changes what the game does): pad edges around an open satchel (input thread), why it closed (main thread), GOT hooks of
// rdrSatchel::OnItemUsed and hudScript::SetHelpMessage (UI / script thread), the use state of hudManager (main thread). debug.rdrvr.vr.satchel.evidence 0 at boot = no hooks.
// the help message is decoded and shown in VR (VrGameMsg.h). A refused item: Apple 'Apples may only be used on horseback.' (count unchanged, the script tells the game the use failed:
// SetupInventoryScriptCommands 0x267fc7c writes the item's +0xa5 = success, invGringoItem::EndUse 0x2675694 decrements only with it and only when the type's +0x68 is clear).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "VrGameMsg.h"

namespace rdrvr::vr::satchel {
enum State : unsigned {Idle,WaitRelease,WaitOpen,Open,Refused,StateCount};
inline constexpr const char* stateNames[StateCount]{"IDLE","WAIT_RELEASE","WAIT_OPEN","OPEN","REFUSED"};
enum Route : unsigned {RouteNone,RouteRow,RouteHoldX,RoutePause,RouteOther,RouteCount};
inline constexpr const char* routeNames[RouteCount]{"","ROW","HOLD_X","PAUSE","OTHER"};
inline constexpr const char* sceneName="SatchelMenu"; // UIManager::GetUIObject / UIManager::StackPop name, the one the game itself uses
inline constexpr std::uint64_t pulseNs=200'000'000,          // Select held (>= 2 game updates even at 10 fps)
    openTimeoutNs=2'500'000'000,releaseTimeoutNs=10'000'000'000,holdWindowNs=30'000'000'000,refusedNoteNs=6'000'000'000;
inline std::atomic<unsigned> state{Idle},route{RouteNone};
inline std::atomic<bool> reopenWanted{false}; // the game refused the press after the VR menu closed; GraphicsMenu opens it again on this row so the reason is seen (XR thread takes it)
inline std::atomic<int> lever{1},visible{-1},enabled{-1},hudOffline{-1}; // -1 = the engine query never ran / the object does not exist
inline std::atomic<bool> armed{false},sent{false},selectWanted{false};
inline std::atomic<const char*> lastError{""};
inline std::atomic<std::uint64_t> requestNs{},waitNs{},selectUntilNs{},holdNs{},refusedNs{},openMs{};
inline std::atomic<std::uint64_t> opens{},pulses{},holds{},shown{},shownByRow{},shownByHold{},shownInPause{},closes{},alreadyOpen{},refused{},
    forcedNative{},forcedCutscene{},stuckAtBoot{};

// ---- evidence ----
inline constexpr std::uintptr_t itemUsedOffset=0x256d050u,itemUsedGotOffset=0x2e4f700u,selectedOffset=0x256d3dcu,helpOffset=0x29fbaccu,helpGotOffset=0x2e4f470u,
    hudInstanceOffset=0x4e6ed0d8u,gameActionInstanceOffset=0x2f88b78u; // atSingleton<hudManager> / atSingleton<rage::GameActionInput>::sm_Instance
inline constexpr std::array<unsigned char,24> itemUsedFingerprint{
#include "gamecode/VrSatchel_1838228_24_8h.inc"
};
inline constexpr std::array<unsigned char,24> selectedFingerprint{
#include "gamecode/VrSatchel_256d3dc_24_8h.inc"
};
inline constexpr std::array<unsigned char,24> helpFingerprint{
#include "gamecode/VrSatchel_1971ca4_24_8h.inc"
};
// rdrSatchel: tab mode (0x118a0, OnTabInit), detail view (0x118a6), IsUseButtonEnabled (0x118a7). Row +0x1e0 = invBaseItem, item +0x30 = type, type +8 = name, type vtable +0x70 = GetInvTypeID,
// invGringoItem +0xa0 = count (OnGetItemCount 0x2675a98). hudManager +0xc548 = the item in use, +0xc550 = its state.
inline constexpr std::uintptr_t modeField=0x118a0u,detailField=0x118a6u,useButtonField=0x118a7u,rowItemField=0x1e0u,itemTypeField=0x30u,typeNameField=0x8u,typeIdSlot=0x70u,itemCountField=0xa0u,
    hudUseItemField=0xc548u,hudUseStateField=0xc550u;
inline constexpr std::uint64_t padTailNs=1'500'000'000,rowTailNs=100'000'000,closeLookbackMs=3000,helpWindowNs=20'000'000'000,rowNoteNs=300'000'000'000;
inline std::atomic<bool> itemUsedHook{false},helpHook{false},useActive{false},useThisOpen{false};
inline std::atomic<std::uint64_t> shownNs{},closedNs{},itemUsedHits{},usesAccepted{},usesNotUsable{},usesBusy{},usesNoItem{},usesOtherTab{},helpsSeen{},usePointer{};
inline std::atomic<int> touchMode{-1}; // IsTouchInterfaceEnabled, main thread at the probe: 1 = rdrSatchel::HandleInputs skips OnItemUsed on A, so the hook cannot see it
enum PadSource : unsigned {SourceTouch,SourceRowPulse,SourceXHold,SourceMenuTap,SourceNeutral,SourceCount};
inline constexpr const char* padSourceNames[SourceCount]{"TOUCH","ROW_PULSE","X_HOLD","MENU_TAP","NOT_READY"};
inline constexpr unsigned padBits=13,padRingSize=12,closeRingSize=8;
inline constexpr const char* padNames[padBits]{"A","B","X","Y","START","L3","R3","LGRIP","RGRIP","LT","RT","UP","SELECT"}; // bit order of runtime::PadButton
inline std::array<std::atomic<std::uint64_t>,padRingSize> padRing{}; // ms << 16 | source << 9 | down << 8 | pad bit
inline std::atomic<std::uint64_t> padEvents{};
enum CloseWhy : unsigned {CloseUnknown,CloseUseItem,CloseCancel,CloseSelect,CloseStart,CloseANoUse,CloseWhyCount};
inline constexpr const char* closeWhyNames[CloseWhyCount]{"UNKNOWN","USE_ITEM","B_CANCEL","SELECT_INVENTORY","START","A_NO_USE"};
inline std::array<std::atomic<std::uint64_t>,closeRingSize> closeRing{}; // end ms mod 2^32 << 32 | open ms (24 bits) << 8 | why
inline std::array<std::atomic<std::uint64_t>,CloseWhyCount> closeCounts{};
inline std::atomic<std::uint64_t> closeEvents{};
enum UseResult : unsigned {UseNone,UseAccepted,UseNotUsable,UseBusy,UseNoItem,UseOtherTab,UseResultCount};
inline constexpr const char* useResultNames[UseResultCount]{"","ACCEPTED","NOT_USABLE","BUSY","NO_ITEM","OTHER_TAB"};
struct ItemUse {
    std::uint64_t ns{},popNs{},useNs{},doneNs{},pointer{};
    char item[28]{},help[28]{},helpText[64]{};
    int mode{-1},typeId{-1},count{-1},countLast{-1},useButton{-1},detail{-1},touch{-1};
    unsigned result{},states{};
};
inline constexpr unsigned useRingSize=16; // 6 is too small: a session with 7 uses loses the first one
inline ItemUse useRing[useRingSize]{}; // under useLock: the UI thread writes it, the main thread advances it, the report thread copies it
inline std::uint64_t useTotal{};
inline std::atomic<std::uint64_t> lastUseNs{};
inline std::atomic_flag useLock=ATOMIC_FLAG_INIT;
struct UseGuard {UseGuard() noexcept {while(useLock.test_and_set(std::memory_order_acquire)){}} ~UseGuard() {useLock.clear(std::memory_order_release);}};
inline void copyText(char* out,std::size_t size,const char* text) noexcept {
    std::size_t n=0;
    if(text)for(;n+1<size&&text[n];++n)out[n]=text[n]>=32&&text[n]<127&&text[n]!='"'&&text[n]!='\\'?text[n]:'?';
    out[n]=0;
}
// Input thread, every pad update, the buttons as they go to the game (after the layout and the holsters' filter).
inline void notePad(std::uint64_t now,std::uint32_t buttons,bool ready) noexcept {
    static std::uint32_t last=0;
    const std::uint32_t changed=(buttons^last)&((1u<<padBits)-1u);last=buttons;
    if(!changed)return;
    const auto closed=closedNs.load(std::memory_order_relaxed);
    if(visible.load(std::memory_order_relaxed)!=1&&state.load(std::memory_order_relaxed)!=WaitOpen&&!(closed&&now>=closed&&now-closed<padTailNs))return;
    const auto until=selectUntilNs.load(std::memory_order_relaxed);
    for(unsigned i=0;i<padBits;++i)if(changed&(1u<<i)){
        const unsigned source=!ready?SourceNeutral:i==12u?(until&&now<=until+rowTailNs?SourceRowPulse:SourceXHold):i==4u?SourceMenuTap:SourceTouch;
        const auto n=padEvents.fetch_add(1,std::memory_order_relaxed);
        padRing[n%padRingSize].store((now/1'000'000u)<<16|std::uint64_t(source)<<9|std::uint64_t((buttons>>i)&1u)<<8|i,std::memory_order_relaxed);
    }
}
// Main thread, at the close: a use the game accepted in this open closes the scene by itself; else the last release before the close (the *RELEASED events are what the scene reads).
// Only releases after the scene was seen: the row's own Select pulse ends just before the open and must not read as SELECT_INVENTORY.
inline unsigned closeReason(std::uint64_t now) noexcept {
    if(useThisOpen.exchange(false,std::memory_order_relaxed))return CloseUseItem;
    const auto n=padEvents.load(std::memory_order_relaxed);const auto nowMs=now/1'000'000u;const auto shownMs=shownNs.load(std::memory_order_relaxed)/1'000'000u;
    for(std::uint64_t k=0;k<padRingSize&&k<n;++k){
        const auto e=padRing[(n-1-k)%padRingSize].load(std::memory_order_relaxed);
        if(nowMs>(e>>16)+closeLookbackMs||(e>>16)<shownMs)break;
        if(e&0x100u)continue;
        switch(e&0xffu){case 0:return CloseANoUse;case 1:return CloseCancel;case 4:return CloseStart;case 12:return CloseSelect;default:break;}
    }
    return CloseUnknown;
}
// UI thread: one call of OnItemUsed, after the native ran.
inline void noteItemUsed(const ItemUse& use) noexcept {
    itemUsedHits.fetch_add(1,std::memory_order_relaxed);
    (use.result==UseAccepted?usesAccepted:use.result==UseBusy?usesBusy:use.result==UseNoItem?usesNoItem:use.result==UseOtherTab?usesOtherTab:usesNotUsable).fetch_add(1,std::memory_order_relaxed);
    if(use.result==UseAccepted){useThisOpen.store(true,std::memory_order_relaxed);lastUseNs.store(use.ns,std::memory_order_relaxed);}
    UseGuard guard;useRing[useTotal++%useRingSize]=use;
    if(use.result==UseAccepted){usePointer.store(use.pointer,std::memory_order_relaxed);useActive.store(true,std::memory_order_relaxed);}
}
// Under useLock: the newest accepted use, never just the newest entry. A second press of A while the script loads logs BUSY right after the accepted one.
inline ItemUse* lastAcceptedUse() noexcept {
    for(std::uint64_t k=useTotal,seen=0;k>0&&seen<useRingSize;--k,++seen){auto& use=useRing[(k-1)%useRingSize];if(use.result==UseAccepted)return &use;}
    return nullptr;
}
// Main thread, while a use runs: pointer / state of hudManager, the count of the item in use (usePointer). EndUse decrements before hudManager lets go,
// so the read that sees the pointer gone also sees the new count. The memory may be a freed pool slot by then (last item deleted); a guarded read, report only.
inline void useStep(std::uint64_t pointer,int useState,int count,std::uint64_t now) noexcept {
    UseGuard guard;
    auto* use=lastAcceptedUse();
    if(!use||use->doneNs){useActive.store(false,std::memory_order_relaxed);return;}
    if(count>=0)use->countLast=count;
    if(pointer!=use->pointer){use->doneNs=now;useActive.store(false,std::memory_order_relaxed);return;}
    if(useState>=0&&useState<8)use->states|=1u<<useState;
    if(useState>=3&&!use->popNs)use->popNs=now;
    if(useState>=4&&!use->useNs)use->useNs=now;
}
// Script thread: every HUD help message; kept while an accepted use still runs (the item scripts' refusals come before EndUse), so a later unrelated message is not blamed on the item.
// text = the key as the game passed it, shownText = the decoded text in panel glyphs.
inline void noteHelp(const char* text,const char* shownText,std::uint64_t now) noexcept {
    helpsSeen.fetch_add(1,std::memory_order_relaxed);
    const auto last=lastUseNs.load(std::memory_order_relaxed);
    if(!text||!last||now<last||now-last>helpWindowNs)return;
    UseGuard guard;
    auto* use=lastAcceptedUse();
    if(use&&!use->doneNs&&!use->help[0]){copyText(use->help,sizeof use->help,text);copyText(use->helpText,sizeof use->helpText,shownText);}
}
// XR thread: the SATCHEL row after a use, the game's own message if there was one. false = no recent use. The caller sanitizes it for the panel.
inline bool lastUseNote(char* out,std::size_t size,std::uint64_t now) noexcept {
    const auto last=lastUseNs.load(std::memory_order_relaxed);
    if(!last||now<last||now-last>rowNoteNs)return false;
    char item[sizeof ItemUse::item]{},help[sizeof ItemUse::helpText]{};
    {
        UseGuard guard;
        const auto* use=lastAcceptedUse();
        if(!use)return false;
        std::memcpy(item,use->item,sizeof item);
        if(use->helpText[0])std::memcpy(help,use->helpText,sizeof use->helpText);else std::memcpy(help,use->help,sizeof use->help);
    }
    if(!item[0])return false;
    std::snprintf(out,size,"LAST: %s",help[0]?help:item);
    for(char* c=out;*c;++c)*c=*c=='_'?' ':*c>='a'&&*c<='z'?char(*c-'a'+'A'):*c;
    return true;
}

inline const char* stateName() noexcept {const unsigned s=state.load(std::memory_order_relaxed);return s<StateCount?stateNames[s]:"?";}
inline bool available() noexcept {return lever.load(std::memory_order_relaxed)!=0;}
inline bool refusedRecently(std::uint64_t now) noexcept {
    const auto t=refusedNs.load(std::memory_order_relaxed);return t&&state.load(std::memory_order_relaxed)==Refused&&now>=t&&now-t<refusedNoteNs;}
// An open satchel forces the Menu presentation only after the scene was seen hidden once (armed) and the mod pressed Select this session (sent),
// so a visible bit that would stay set at boot forces nothing (same rule as the pause scene). force = gamemenu::force.
inline bool forcing(bool scene,int force) noexcept {return scene&&force&&armed.load(std::memory_order_relaxed)&&sent.load(std::memory_order_relaxed);}
// XR thread: A on the row. true = the caller closes the VR menu and calls notePulse once the buttons are released; false = just close it.
inline bool request(std::uint64_t now,bool pauseOpen) noexcept {
    opens.fetch_add(1,std::memory_order_relaxed);lastError.store("",std::memory_order_relaxed);
    if(visible.load(std::memory_order_relaxed)==1){alreadyOpen.fetch_add(1,std::memory_order_relaxed);return false;} // A Select press would CLOSE it
    if(pauseOpen){lastError.store("CLOSE THE GAME MENU FIRST",std::memory_order_relaxed);refused.fetch_add(1,std::memory_order_relaxed);
        refusedNs.store(now,std::memory_order_relaxed);state.store(Refused,std::memory_order_release);return false;} // The pause scene owns the input, the HUD never sees Select
    requestNs.store(now,std::memory_order_relaxed);state.store(WaitRelease,std::memory_order_release);return true;
}
// XR thread: the VR menu's close guard saw physical neutral.
inline void notePulse() noexcept {selectWanted.store(true,std::memory_order_release);}
// Input thread, every pad update: true = PadSelect is down in this update. The pulse starts on a ready pad only, so it cannot be lost to a re-arm.
inline bool selectNow(std::uint64_t now,bool ready) noexcept {
    if(ready&&selectWanted.load(std::memory_order_acquire)&&selectWanted.exchange(false,std::memory_order_acq_rel)){
        sent.store(true,std::memory_order_relaxed);pulses.fetch_add(1,std::memory_order_relaxed);selectUntilNs.store(now+pulseNs,std::memory_order_relaxed);
        waitNs.store(now,std::memory_order_relaxed);state.store(WaitOpen,std::memory_order_release);
    }
    const auto until=selectUntilNs.load(std::memory_order_relaxed);
    return until&&now<until;
}
// Input thread: the Mapper turned a hold of X into Select (it opens on the release of X).
inline void noteHold(std::uint64_t now) noexcept {sent.store(true,std::memory_order_relaxed);holds.fetch_add(1,std::memory_order_relaxed);holdNs.store(now,std::memory_order_relaxed);}
// Main thread. scene = SatchelMenu IsVisible(true), pauseScene = PauseMenuScene IsVisible(true) (the satchel hosted by the pause menu).
inline void step(bool scene,bool pauseScene,std::uint64_t now) noexcept {
    const int was=visible.exchange(scene?1:0,std::memory_order_relaxed);
    if(!scene)armed.store(true,std::memory_order_relaxed);
    auto s=state.load(std::memory_order_acquire);
    if(scene&&was!=1){
        shown.fetch_add(1,std::memory_order_relaxed);
        const auto h=holdNs.load(std::memory_order_relaxed);
        if(s==WaitOpen||s==WaitRelease){shownByRow.fetch_add(1,std::memory_order_relaxed);route.store(RouteRow,std::memory_order_relaxed);const auto w=waitNs.load(std::memory_order_relaxed);openMs.store(w&&now>=w?(now-w)/1'000'000u:0,std::memory_order_relaxed);}
        else if(pauseScene){shownInPause.fetch_add(1,std::memory_order_relaxed);route.store(RoutePause,std::memory_order_relaxed);}
        else if(h&&now>=h&&now-h<holdWindowNs){shownByHold.fetch_add(1,std::memory_order_relaxed);route.store(RouteHoldX,std::memory_order_relaxed);holdNs.store(0,std::memory_order_relaxed);}
        else route.store(RouteOther,std::memory_order_relaxed);
        lastError.store("",std::memory_order_relaxed);s=Open;state.store(Open,std::memory_order_release);
        shownNs.store(now,std::memory_order_relaxed);useThisOpen.store(false,std::memory_order_relaxed);
    }else if(!scene&&was==1){
        closes.fetch_add(1,std::memory_order_relaxed);
        if(s==Open){s=Idle;state.store(Idle,std::memory_order_release);}
        closedNs.store(now,std::memory_order_relaxed);
        const unsigned why=closeReason(now);closeCounts[why].fetch_add(1,std::memory_order_relaxed);
        const auto opened=shownNs.load(std::memory_order_relaxed);const std::uint64_t openedMs=opened&&now>=opened?std::min<std::uint64_t>((now-opened)/1'000'000u,0xffffffu):0;
        closeRing[closeEvents.fetch_add(1,std::memory_order_relaxed)%closeRingSize].store((now/1'000'000u)<<32|openedMs<<8|why,std::memory_order_relaxed);
    }
    const auto fail=[&](const char* why){selectWanted.store(false,std::memory_order_relaxed);lastError.store(why,std::memory_order_relaxed);refused.fetch_add(1,std::memory_order_relaxed);refusedNs.store(now,std::memory_order_relaxed);state.store(Refused,std::memory_order_release);};
    if(s==WaitRelease){const auto r=requestNs.load(std::memory_order_relaxed);if(now>=r&&now-r>releaseTimeoutNs)fail("BUTTONS NOT RELEASED");}
    else if(s==WaitOpen){
        const auto w=waitNs.load(std::memory_order_relaxed);
        if(now>=w&&now-w>pulseNs+openTimeoutNs){
            // The last read of the gates that can be read cheaply; the others (ragdoll, melee, scope, mover) stay in the generic text.
            fail(enabled.load(std::memory_order_relaxed)==0?"DISABLED BY THE GAME NOW":hudOffline.load(std::memory_order_relaxed)==0?"NOT ON THIS SCREEN":"GAME REFUSED - TRY STANDING");reopenWanted.store(true,std::memory_order_relaxed);
        }
    }
}
inline std::string report() {
    char b[800];const unsigned r=route.load();
    std::snprintf(b,sizeof b,"{\"lever\":%d,\"scene\":\"%s\",\"armed\":%s,\"sent\":%s,\"opens\":%llu,\"state\":\"%s\",\"lastError\":\"%s\",\"visible\":%d,\"route\":\"%s\",\"enabled\":%d,\"hudOffline\":%d,"
        "\"pulses\":%llu,\"holds\":%llu,\"shown\":%llu,\"shownByRow\":%llu,\"shownByHold\":%llu,\"shownInPause\":%llu,\"openMs\":%llu,\"closes\":%llu,\"alreadyOpen\":%llu,\"refused\":%llu,"
        "\"forcedNative\":%llu,\"forcedCutscene\":%llu,\"stuckAtBoot\":%llu",
        lever.load(),sceneName,armed.load()?"true":"false",sent.load()?"true":"false",(unsigned long long)opens.load(),stateName(),lastError.load(),visible.load(),
        r<RouteCount?routeNames[r]:"?",enabled.load(),hudOffline.load(),(unsigned long long)pulses.load(),(unsigned long long)holds.load(),(unsigned long long)shown.load(),
        (unsigned long long)shownByRow.load(),(unsigned long long)shownByHold.load(),(unsigned long long)shownInPause.load(),(unsigned long long)openMs.load(),(unsigned long long)closes.load(),
        (unsigned long long)alreadyOpen.load(),(unsigned long long)refused.load(),(unsigned long long)forcedNative.load(),(unsigned long long)forcedCutscene.load(),(unsigned long long)stuckAtBoot.load());
    std::string out=b;char e[420];
    const auto now=std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());const auto nowMs=now/1'000'000u;
    std::snprintf(e,sizeof e,",\"itemUsedHook\":%s,\"helpHook\":%s,\"itemUsedHits\":%llu,\"accepted\":%llu,\"notUsable\":%llu,\"busy\":%llu,\"noItem\":%llu,\"otherTab\":%llu,\"helpsSeen\":%llu,\"useActive\":%s,\"touchMode\":%d,\"padEvents\":%llu,\"pad\":[",
        itemUsedHook.load()?"true":"false",helpHook.load()?"true":"false",(unsigned long long)itemUsedHits.load(),(unsigned long long)usesAccepted.load(),(unsigned long long)usesNotUsable.load(),
        (unsigned long long)usesBusy.load(),(unsigned long long)usesNoItem.load(),(unsigned long long)usesOtherTab.load(),(unsigned long long)helpsSeen.load(),useActive.load()?"true":"false",touchMode.load(),(unsigned long long)padEvents.load());
    out+=e;
    const auto shown=shownNs.load()/1'000'000u;const auto pads=padEvents.load();
    for(std::uint64_t k=pads>padRingSize?pads-padRingSize:0,first=k;k<pads;++k){
        const auto v=padRing[k%padRingSize].load();const unsigned pad=unsigned(v&0xffu),source=unsigned((v>>9)&7u);const auto ms=v>>16;
        std::snprintf(e,sizeof e,"%s{\"ageMs\":%llu,\"sinceShownMs\":%lld,\"pad\":\"%s\",\"down\":%s,\"source\":\"%s\"}",k==first?"":",",(unsigned long long)(nowMs>=ms?nowMs-ms:0u),(long long)ms-(long long)shown,
            pad<padBits?padNames[pad]:"?",(v&0x100u)?"true":"false",source<SourceCount?padSourceNames[source]:"?");
        out+=e;
    }
    out+="],\"closeCounts\":{";
    for(unsigned i=0;i<CloseWhyCount;++i){std::snprintf(e,sizeof e,"%s\"%s\":%llu",i?",":"",closeWhyNames[i],(unsigned long long)closeCounts[i].load());out+=e;}
    out+="},\"closesLog\":[";
    const auto closeTotal=closeEvents.load();
    for(std::uint64_t k=closeTotal>closeRingSize?closeTotal-closeRingSize:0,first=k;k<closeTotal;++k){
        const auto v=closeRing[k%closeRingSize].load();const unsigned why=unsigned(v&0xffu);const auto end=v>>32;const auto wrapped=nowMs&0xffffffffu;
        std::snprintf(e,sizeof e,"%s{\"ageMs\":%llu,\"openMs\":%llu,\"why\":\"%s\"}",k==first?"":",",(unsigned long long)(wrapped>=end?wrapped-end:0u),(unsigned long long)((v>>8)&0xffffffu),why<CloseWhyCount?closeWhyNames[why]:"?");
        out+=e;
    }
    out+="],\"uses\":[";
    ItemUse copy[useRingSize];std::uint64_t total{};
    {UseGuard guard;total=useTotal;for(unsigned i=0;i<useRingSize;++i)copy[i]=useRing[i];}
    for(std::uint64_t k=total>useRingSize?total-useRingSize:0,first=k;k<total;++k){
        const auto& u=copy[k%useRingSize];const auto since=[&](std::uint64_t ns){return (long long)(ns&&ns>=u.ns?(ns-u.ns)/1'000'000u:-1);};
        std::snprintf(e,sizeof e,"%s{\"ageMs\":%llu,\"item\":\"%s\",\"typeId\":%d,\"tab\":%d,\"result\":\"%s\",\"useButton\":%d,\"detail\":%d,\"touch\":%d,\"count\":%d,\"countLast\":%d,\"states\":%u,\"closeMs\":%lld,\"useMs\":%lld,\"doneMs\":%lld,\"help\":\"%s\",\"helpText\":\"%s\"}",
            k==first?"":",",(unsigned long long)(u.ns&&now>=u.ns?(now-u.ns)/1'000'000u:0u),u.item,u.typeId,u.mode,u.result<UseResultCount?useResultNames[u.result]:"?",u.useButton,u.detail,u.touch,u.count,u.countLast,u.states,since(u.popNs),since(u.useNs),since(u.doneNs),u.help,u.helpText);
        out+=e;
    }
    out+="]";
    gamemsg::appendReport(out);
    out+="}";
    return out;
}
} // namespace rdrvr::vr::satchel
