#pragma once
// CUTSCENE PROPS / LIGHTS / SUN (MOVEMENT rows, SCRIPT / STEADY). The game hides objects and changes the lighting at shot changes, restaging what is visible in the 2D scene.
// In FIRST PERSON the player sees the whole set while the game restages it per shot.
// Everything here acts only while the composer runs FIRST PERSON (cutscene::engagedMode, plans guidFirstPerson / guidFirstPersonSub); CINEMATIC, SCREEN and gameplay stay stock.
//
// Engine facts (librdr.so 1.58 arm64, vaddr; all.s).
// PROPS. Event ids: rage::cutfEvent::c_cutsceneEventIdDisplayNames 0x2eb7bd8 (0x0e Hide Objects, 0x0f Show Objects, 0x17 Clear Anim, 0x2b Camera Cut, 0x2c / 0x2d removal bound on / off),
//   c_cutscenePlaybackEventIdDisplayNames 0x2eb9908 from 0x80 (0x80 Start of Scene, 0x81 End, 0x86 Update, 0x8e Set Clip, 0x8f Set Dual Clip, 0x9d Scene Orientation Changed).
//  - rdr2CutsceneAnimatedModelEntity::DispatchEvent 0x2406e30: Clear Anim -> rdr2CutSceneManager::HideObjectInScene 0x23f43f4 (bl 0x2407028, unless the scene time passed the end or reins hold it);
//    Set Clip / Set Dual Clip -> ShowObjectInScene (bl 0x2406f28) if entity +0x74 bit 2. So every model without a clip in the current section is hidden: the per-shot hide of the props.
//  - The same call from rdr2CutsceneAnimatedModelEntity::Hide 0x2407204 (bl 0x2407248) = prop event 0x10b, attributes WeaponOwner + WeaponSlot: a weapon prop the owner does not own. Stays stock.
//  - Entity +0x74: bit 0 visible, bit 1 has a clip (PreSceneUpdate 0x24072a8 updates the motion tree only then: without a clip the pose stays frozen), bit 2 may be shown.
//    A false from HideObjectInScene skips the state change (tbz at 0x240702c); the next Set Clip finds it not hidden and only sets bit 1.
//  - rdr2CutscenePropModelStreamer::Initialize 0x2403b40 hides every prop at the start, so a prop before its first clip has no pose yet and stays hidden (no pre-roll here).
//  - Other hide routes, only counted: rdr2CutsAssetMgrEntity::HideHiddenObjects 0x23f9f28 (camera cut, start, orientation; list entity+0xd5e0 set by Hide Objects: world objects the scene replaces,
//    un-hiding those would show both copies), rdr2CutsRemovalBoundsEntity::HideObjects / ShowObjects 0x2416114 / 0x2415fdc (removal bounds), HideProp 0x23fb090.
//  - A carried prop cannot be told apart by data: it is baked by its own clip. So STEADY keeps only a prop whose root bone (streamer +0x68 motion tree, +0x30 skeleton, +0x28 globals) stood still for
//    stillNs before its Clear Anim, not close to John's eye, not named for John, and with no actor's bone within nearActorM (a hand, a holster: the actor hides with his own Clear Anim, a kept copy would float;
//    list manager+0xb38 / +0xb40 of GetEntityByObjectId, entity +0x20 motion tree). A second prop of the same model (streamer +0x38 stub) with a clip that moves takes the kept copy's stock hide.
//  - The way back. Uninitialize 0x2403cb8 (Unload Models) shows a hidden prop only if streamer +0x18 bit 0 and leaves a visible one visible, so a kept prop must get its stock hide first:
//    releaseBeforeEndS before the scene's end time, and as soon as manager +0xb30 leaves Play 8 / Paused 9. Stop 0x23bcc50 (skip, script, death, load, the natural end) sets FadingOut 4 or Stopped 10 at least one frame
//    before DoStoppedState 0x23bfe1c -> Unload (12): both run in sagShellDerived::Update 0x23a24e0 (vptr+0x28 PreSceneUpdate @0x23a2b38) on the main thread, after UpdateCamera @0x23a27ac where this tick runs.
//    Should a streamer be uninitialized anyway (+0x10 bit 2 clear): the stock hide only when bit 0 is clear, the end state stays the stock one.
// LIGHTS. sagRenderer::CalculateCutsceneHashes 0x2a55218 (x2 scene, x3 shot, x4 next shot; hashes of the scene and camera+0x6a0 / +0x720 names set by DoCameraCut 0x265aad8). Consumers:
//  - UpdateLightsSpu 0x2a6d290 (StartUpdateLightTask 0x2990edc, matrix null): a placed light with (flags & 0x300) == 0x100 exists only if its hash is one of {scene, 0, shot, next shot, renderer+0x1fdc}.
//  - SetupCurrentSortedVisibleLights 0x2a34f84: light +0xd8 bit 7 + hash +0x84 must be 0, scene or shot.
//  So a union of all shots cannot pass either gate without patching both compares, it would stack several authored key lights on one room and churn the 5 lamp shadow slots. STEADY holds one shot's
//  hash (x3 and x4 replaced after the original): the first shot of the scene, again at a cut when John's eye left it by relatchMetres (another location), or while the held rig has no light at all.
//  The stock camera test IsCurrentCamera inside the function is untouched.
// SUN. The sun direction is not per shot: the only sun inputs are ltLightingMgr::Update 0x298c064 (time of day, rotated slowly between cuts, snapped by renderer+0x1fd4) and scene-hash gates
//  (RenderDirectionalLight 0x2a37724 zeroes the colour for ONE scene hash); no cut event or shot hash reaches them; log sunDeg 0 in every shadow delta (rows min / max below check it per shot).
//  What flips is the portal clip: renLightPrePassLightsMgr::FindRoom 0x2a36c30 (DrawShadowMapsIntoCollector 0x2a5455c) = QueryPointIntersectsRoom(viewport position + forward * (near + mgr+0x9c3c)),
//  mgr+0x6c28 the room, mgr+0x6c20 the room unless its +0xa0 bit 0; UseSunlightPortalClipping 0x2a39648 needs +0x6c20: with a room the collector is cleared and the sun comes only through its portals,
//  without one the sun comes through the roof (only part of the room casts). No per-shot state drives it, only the viewport: in 2D the shot camera, here the eye and its forward.
//  STEADY (render thread, after the original): the room at the exact eye position, direction free, and it changes only after the eye really moved (roomMoveM for roomHoldNs, or roomJumpM at once).
//  A held room is re-queried at its own position every frame: a room that streamed out is dropped at once, so no stale pointer can reach the frame.
// Hooks: GOT slots, whole-function prints + GOT words at install, any mismatch = none installed = stock. debug.rdrvr.vr.cutscene.steady 0 = no hooks.
// Levers (beat the rows, read twice a second): debug.rdrvr.vr.cutscene.props / .lights / .sun 0 SCRIPT, 1 STEADY. Report vr.cutscene.steady.
#include "VrCutscene.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::vr::cssteady {
namespace cs=::rdrvr::vr::cutscene;
using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
using HashesFn=bool(*)(void*,const void*,std::uint32_t*,std::uint32_t*,std::uint32_t*,bool);
using ObjectFn=bool(*)(void*,int);
using FindRoomFn=void(*)(void*,const void*);
using SetupFn=void(*)(void*,const void*,void*,const void*,const void*,bool);
using EntityFn=void(*)(void*,void*);
using HidePropFn=bool(*)(void*,std::uint32_t,bool);
using QueryFn=void*(*)(void*,const float*);
using LookupFn=void*(*)(const void*,int);
using EndTimeFn=float(*)(const void*);

struct Print {std::uintptr_t offset;std::uint32_t size;std::uint64_t fnv;};
inline constexpr Print hashesPrint{0x2a55218,0x1cc,0x3ded7f744ecc4042ull},hideObjPrint{0x23f43f4,0xc,0x59e714d9eb7c94d0ull},showObjPrint{0x23f4400,0xc,0xdbff56e2c20685a3ull},
    findRoomPrint{0x2a36c30,0xb4,0x4e43acb5c123155eull},setupPrint{0x2a34f84,0x6f4,0x9f30aa0bb1197643ull},hideHiddenPrint{0x23f9f28,0x18c,0xf5197b95e12e0016ull},hidePropPrint{0x23fb090,0x19c,0x5196d541a6addddbull},
    rbShowPrint{0x2415fdc,0x138,0x57bba3a5ffa9ee5aull},rbHidePrint{0x2416114,0x66c,0xc4b423f3dc777a35ull},queryPrint{0x1c0f3e8,0x8,0x4b920cf02143a2d2ull},entityPrint{0x23be20c,0x3c,0x62f31ce672b6ff6full},
    objectPrint{0x23be1e0,0x2c,0x2ee6f7d8b591ddb1ull},endTimePrint{0x23bd59c,0x94,0x136e5b9fd3d495e0ull};
// Never called, the layouts and call sites above depend on them.
inline constexpr std::array<Print,15> witnessPrints{{{0x240b768,0x1e0,0xfdec066f3d7d97efull},{0x23bcc50,0x150,0x22a0477a7af64cebull},{0x2406e30,0x254,0x1a2157cffa3e687eull},{0x240aecc,0x89c,0x8cf2113c50da9119ull},{0x2406240,0x838,0xf00a64ef44001d99ull},{0x23faa00,0x9c,0x2ca40544b60aa22full},
    {0x24040a4,0x5c,0xe27bc310cfb2663bull},{0x2404050,0x54,0x467bafd462c8061aull},{0x2403cb8,0x1a8,0xed4b3e18c94ea14dull},{0x24072a8,0x94,0x415a8e1510664fb5ull},{0x2407204,0xa0,0xd85ae902c9cff141ull},
    {0x2a6d290,0x368,0x6bff5d00d0cf2735ull},{0x2990edc,0x1a0,0xad856ecbad06f00dull},{0x2a39648,0x70,0xbbc553e3dfc0b2c0ull},{0x2a543cc,0x6bc,0x3028f302615a31b9ull}}};
inline constexpr std::uintptr_t hashesGot=0x2e5dab8,hideObjGot=0x2e4a5f8,showObjGot=0x2e4a600,findRoomGot=0x2e5f428,setupGot=0x2e5f3a0,hideHiddenGot=0x2e4a290,hidePropGot=0x2e4a2e0,rbShowGot=0x2e4aa50,rbHideGot=0x2e4aa60;
// Data GOT words -> globals (each a pointer to the instance)
inline constexpr std::uintptr_t managerGot=0x2dfff78,managerOffset=0x4e72eaa8,worldGot=0x2de1948,worldOffset=0x4e6f6cd8,pointMgrGot=0x2e023c0,pointMgrOffset=0x4e82dec8,spotMgrGot=0x2e023c8,spotMgrOffset=0x4e82e420;
inline constexpr std::uintptr_t lrClearAnim=0x240702c,lrWeaponSlot=0x240724c,propStreamerVtable=0x2d53488+0x10;
// rdr2CutsceneAnimated{Actor,Human,Horse}Entity: the entities that can carry a prop
inline constexpr std::array<std::uintptr_t,3> actorEntityVtables{0x2d53a10+0x10,0x2d53ae0+0x10,0x2d53bb0+0x10};
namespace off {
inline constexpr std::size_t managerAsset=0x17a0,managerTime=0xb74,managerState=0xb30,managerEntities=0xb38,managerEntityCount=0xb40,entityStride=0x18,assetStreamers=0x10,assetStreamerCount=0x290,assetHiddenList=0xd5e0,
    streamerObject=0x8,streamerFlags=0x10,streamerOwner=0x18,streamerStub=0x38,streamerHidden=0x4c,streamerTree=0x68,
    treeSkeleton=0x30,skelCount=0x20,skelGlobals=0x28,boneStride=0x40,boneOrigin=0x30,objectId=0x8,entityObject=0x8,entityTree=0x20,entityFlags=0x74,entityAttach0=0x3c,entityAttach1=0x4c,entityAttach2=0x5c,entityAttach3=0x6c,
    roomClip=0x6c20,roomAny=0x6c28,roomFlags=0xa0,portalClipOn=0xba6,portalClear=0xba7,queryOffset=0x9c3c,viewportPos=0x70,
    pointCount=0x1008,spotCount=0x808,lightList=0x8,lightHash=0x84,lightFlags=0xd8,visibleCount=0x1800;
inline constexpr unsigned maxStreamers=80,slotFlagTagged=0x80,slotUsableMask=0x5001,streamerInitialized=4;
inline constexpr int statePlay=8,statePaused=9;
}
inline constexpr std::uint64_t stillNs=500'000'000ull,roomHoldNs=300'000'000ull,rigGraceNs=1'000'000'000ull,liveGraceNs=1'000'000'000ull,hitchGraceNs=1'000'000'000ull;
inline constexpr float stillMoveM=.015f,nearEyeM=.6f,nearActorM=.25f,releaseBeforeEndS=.15f,relatchMetres=8.f,roomMoveM=.35f,roomJumpM=1.5f;
inline constexpr unsigned trackMax=48,trackBones=4,actorBonesMax=128,actorEntitiesMax=96,eventMax=96,cutMax=48,secondMax=96,namesMax=8,nameBytes=28;

struct Api {
    ReadFn read{};std::uintptr_t base{};
    HashesFn hashes{};ObjectFn hideObj{},showObj{};FindRoomFn findRoom{};SetupFn setup{};EntityFn hideHidden{},rbShow{},rbHide{};HidePropFn hideProp{};QueryFn query{};LookupFn entity{},object{};EndTimeFn endTime{};
    std::uintptr_t manager{},world{},pointMgr{},spotMgr{};
};
inline Api api{};
inline std::atomic<const char*> installState{"NOT_INSTALLED"},lastError{""};
inline std::atomic<bool> installed{};
inline std::atomic<int> leverProps{-1},leverLights{-1},leverSun{-1};

namespace detail {
inline std::uint64_t steadyNs() noexcept {timespec t{};clock_gettime(CLOCK_MONOTONIC,&t);return std::uint64_t(t.tv_sec)*1'000'000'000ull+std::uint64_t(t.tv_nsec);}
template<class T> inline T at(std::uintptr_t p,std::size_t o) noexcept {return *reinterpret_cast<const T*>(p+o);}
template<class T> inline void put(std::uintptr_t p,std::size_t o,T v) noexcept {*reinterpret_cast<T*>(p+o)=v;}
// Guarded read: main-thread tick over objects that the cutscene owns
template<class T> inline bool peek(std::uintptr_t p,std::size_t o,T& out) noexcept {return p&&api.read&&api.read(nullptr,p+o,&out,sizeof out);}
inline int leverOf(const char* name) noexcept {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX]{};if(__system_property_get(name,v)==1&&(v[0]=='0'||v[0]=='1'))return v[0]-'0';
#else
    (void)name;
#endif
    return -1;
}
inline std::uint64_t codeHash(std::uintptr_t address,std::uint32_t size) noexcept {
    std::uint64_t h=0xcbf29ce484222325ull;unsigned char chunk[256];
    for(std::uint32_t done=0;done<size;){const std::uint32_t n=std::min<std::uint32_t>(size-done,sizeof chunk);
        if(!api.read(nullptr,address+done,chunk,n))return 0;
        for(std::uint32_t i=0;i<n;++i)h=(h^chunk[i])*0x100000001b3ull;
        done+=n;}
    return h;
}
inline bool icontains(const char* s,const char* needle) noexcept {
    if(!s)return false;const std::size_t n=std::strlen(needle);
    for(;*s;++s){std::size_t i=0;while(i<n&&s[i]&&(s[i]|32)==(needle[i]|32))++i;if(i==n)return true;}
    return false;
}
inline void copyName(char* out,const char* in) noexcept {
    std::size_t i=0;
    if(in)for(;i+1<nameBytes&&in[i];++i){const char c=in[i];out[i]=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'?c:'_';}
    out[i]=0;
}
// cutfObject vtable +0x28 -> the object's name (0x23f9ff0, 0x240b2b4). Engine thread, a live object.
inline const char* objectName(std::uintptr_t object) noexcept {
    if(!object)return nullptr;
    const auto vt=at<std::uintptr_t>(object,0);if(!vt)return nullptr;
    const auto fn=reinterpret_cast<const char* const*(*)(std::uintptr_t)>(at<std::uintptr_t>(vt,0x28));
    const auto p=fn(object);return p?*p:nullptr;
}
inline float dist3(const float* a,const float* b) noexcept {const float x=a[0]-b[0],y=a[1]-b[1],z=a[2]-b[2];return std::sqrt(x*x+y*y+z*z);}
}

inline bool propsWanted() noexcept {const int l=leverProps.load(std::memory_order_relaxed);return l>=0?l==1:settings()[Key::CutsceneProps]==1;}
inline bool lightsWanted() noexcept {const int l=leverLights.load(std::memory_order_relaxed);return l>=0?l==1:settings()[Key::CutsceneLights]==1;}
inline bool sunWanted() noexcept {const int l=leverSun.load(std::memory_order_relaxed);return l>=0?l==1:settings()[Key::CutsceneSun]==1;}
inline bool firstPersonMode() noexcept {return enabled(Feature::Cutscene)&&settings().cutsceneMode()==cs::FirstPerson;}
// The composer engaged FIRST PERSON for a frame just now: the plan of the next draw is guidFirstPerson / guidFirstPersonSub.
// A cut often comes with a streaming hitch longer than engagedMaxAgeNs: inside a running FIRST PERSON cutscene that gap does not let one frame of the script's rig, clip or hide through.
inline bool liveFp() noexcept {
    if(cs::engagedMode.load(std::memory_order_relaxed)!=cs::FirstPerson)return false;
    const auto t=cs::engagedNs.load(std::memory_order_relaxed);const auto n=detail::steadyNs();
    if(!t)return false;
    if(n<t||n-t<=cs::engagedMaxAgeNs)return true;
    return n-t<=hitchGraceNs&&cs::inCutscene.load(std::memory_order_relaxed)&&firstPersonMode();
}

// ---- state ----
struct Tracked {int id{-1};std::uintptr_t stub{};unsigned bones{};float pos[trackBones][3]{},anchor[trackBones][3]{};std::uint64_t stillSinceNs{},seenNs{};bool pose{},kept{},hidden{},initialized{},showsAtEnd{};};
inline std::mutex trackLock;
inline std::array<Tracked,trackMax> tracked{}; // trackLock
inline float keptEndTime{-1.f}; // trackLock: the scene's end time, read at the first kept prop
enum Action : unsigned {ActHidden,ActKept,ActMoving,ActNearEye,ActNearActor,ActJohn,ActNoTrack,ActActor,ActWeaponSlot,ActOther,ActDouble,ActReleased,ActCount};
inline constexpr const char* actionNames[ActCount]{"HIDDEN","KEPT","STOCK_MOVING","STOCK_NEAR_EYE","STOCK_NEAR_ACTOR","STOCK_JOHN","STOCK_NO_TRACK","ACTOR","WEAPON_SLOT","OTHER","STOCK_DOUBLE","RELEASED"};
struct Event {std::uint32_t ms{};int id{};std::uint8_t action{};float stillS{},eyeM{},actorM{};char name[nameBytes]{};};
struct CutRow {std::uint32_t ms{},shot{},prevShot{},next{},held{};int lightsMin{-1},lightsMax{-1},heldLights{},hiddenProps{},keptProps{};float sun[3]{},sunMin[3]{},sunMax[3]{};
    std::uint32_t clipOnFrames{},clipOffFrames{},nativeOnFrames{},nativeFlips{},appliedFlips{};bool clipNow{},nativeNow{};};
struct ShotAccum {int lightsMin{-1},lightsMax{-1};bool sunValid{};float sun[3]{},sunMin[3]{},sunMax[3]{};std::uint32_t clipOnFrames{},clipOffFrames{},nativeOnFrames{},nativeFlips{},appliedFlips{};};
inline std::mutex rowLock;
inline std::array<Event,eventMax> events{};inline std::uint32_t eventNext{}; // rowLock
inline std::array<CutRow,cutMax> cutRows{};inline std::uint32_t cutNext{};  // rowLock
inline ShotAccum shot{}; // rowLock
inline float heldEye[3]{};inline std::uint64_t heldSinceNs{};inline bool heldEyeValid{}; // rowLock
inline std::array<std::array<char,nameBytes>,namesMax> hiddenListNames{}; // rowLock
inline std::uint64_t firstNs{}; // rowLock
inline std::atomic<std::uint32_t> sceneHash{},heldShot{},realShot{},realNext{};
inline std::atomic<std::uint64_t> realFromNullNs{};
inline std::atomic<int> visibleNow{},heldLightsNow{},sceneLightsNow{},anyLightsNow{},realLightsNow{},otherLightsNow{},untaggedNow{},hiddenPropsNow{},keptNow{},trackedNow{},posedNow{};
inline std::atomic<bool> nativeClipNow{},appliedClipNow{},rawRoomNow{};
inline std::atomic<std::uintptr_t> hiddenListLast{};inline std::atomic<int> hiddenListCount{};
struct Stats {std::atomic<std::uint64_t> hideCalls{},clearAnim{},weaponSlot{},actorHides{},kept{},resumed{},released{},doubles{},stockMoving{},stockNearEye{},stockNearActor{},stockJohn{},stockNoTrack{},shows{},
    hiddenCalls{},hiddenListChanges{},hideProps{},rbShows{},rbHides{},hashCalls{},overrides{},cuts{},scenes{},latches{},relatchMoved{},relatchEmpty{},
    roomCalls{},roomQueries{},nativeFlips{},appliedFlips{},roomChanges{},roomDrops{},setupCalls{},scans{};
    std::atomic<float> eyeQueryM{-1.f},queryOffset{-1.f};std::atomic<int> portalClipOn{-1},portalClear{-1};};
inline Stats stats{};
inline std::atomic<const char*> lastRelatch{"NONE"};
enum SecondField : unsigned {SecFrames,SecLights,SecHeld,SecOther,SecHides,SecKept,SecShows,SecCuts,SecNativeFlips,SecAppliedFlips,SecClipOn,SecNativeOn,SecCount};
inline constexpr const char* secondNames[SecCount]{"f","lights","held","other","hides","kept","shows","cuts","nativeFlips","appliedFlips","clipOn","nativeOn"};
struct SecondRow {std::atomic<std::uint32_t> second{~0u};std::array<std::atomic<std::uint32_t>,SecCount> v{};};
inline std::array<SecondRow,secondMax> seconds{};
inline std::atomic<std::uint32_t> secondNow{};inline std::uint64_t secondFirstNs{}; // render thread

namespace detail {
inline void secondTick(std::uint64_t now) noexcept {
    if(!secondFirstNs)secondFirstNs=now?now:1;
    const auto s=std::uint32_t((now-secondFirstNs)/1'000'000'000ull);auto& r=seconds[s%secondMax];
    if(r.second.load(std::memory_order_relaxed)!=s){for(auto& a:r.v)a.store(0,std::memory_order_relaxed);r.second.store(s,std::memory_order_relaxed);secondNow.store(s,std::memory_order_relaxed);}
}
inline void secondBump(SecondField f,std::uint32_t n=1) noexcept {seconds[secondNow.load(std::memory_order_relaxed)%secondMax].v[f].fetch_add(n,std::memory_order_relaxed);}
inline void secondMaxOf(SecondField f,std::uint32_t n) noexcept {auto& a=seconds[secondNow.load(std::memory_order_relaxed)%secondMax].v[f];if(n>a.load(std::memory_order_relaxed))a.store(n,std::memory_order_relaxed);}
inline void pushEvent(Action a,int id,const char* name,float stillS,float eyeM,float actorM) noexcept {
    const auto now=steadyNs();std::lock_guard lock(rowLock);if(!firstNs)firstNs=now;
    auto& e=events[eventNext++%eventMax];e=Event{};e.ms=std::uint32_t((now-firstNs)/1000000u);e.id=id;e.action=std::uint8_t(a);e.stillS=stillS;e.eyeM=eyeM;e.actorM=actorM;copyName(e.name,name);
}
}

// ---- PROPS: rdr2CutSceneManager::HideObjectInScene (main / the cutscene's update thread) ----
namespace detail {
// The stock result of a Clear Anim for an object it was skipped for: the object hidden + the entity state of 0x2407030..0x2407068.
inline void stockHide(std::uintptr_t manager,int id) noexcept {
    if(!api.hideObj(reinterpret_cast<void*>(manager),id))return;
    const auto entity=reinterpret_cast<std::uintptr_t>(api.entity(reinterpret_cast<const void*>(manager),id));if(!entity)return;
    put<std::uint8_t>(entity,off::entityFlags,at<std::uint8_t>(entity,off::entityFlags)&0xfeu);
    for(const auto o:{off::entityAttach0,off::entityAttach1,off::entityAttach2,off::entityAttach3})put<std::uint8_t>(entity,o,at<std::uint8_t>(entity,o)|1u);
}
// The nearest bone of any actor of the scene to the prop's tracked bones, -1 when none could be read (hidden actors count too: their Clear Anim may come first in the same event). Main thread, a few times per cut.
inline float nearestActorM(std::uintptr_t manager,const Tracked& t) noexcept {
    std::uintptr_t list{};std::uint16_t count{};float best=-1.f;
    if(!peek(manager,off::managerEntities,list)||!list||!peek(manager,off::managerEntityCount,count))return best;
    alignas(16) unsigned char bones[actorBonesMax*off::boneStride];
    for(unsigned i=0;i<count&&i<actorEntitiesMax;++i){
        std::uintptr_t e{},vt{},tree{},skeleton{},globals{};std::uint32_t n{};
        if(!peek(list,std::size_t(i)*off::entityStride,e)||!e||!peek(e,0,vt))continue;
        bool actor=false;for(const auto v:actorEntityVtables)actor=actor||vt==api.base+v;
        if(!actor)continue;
        if(!peek(e,off::entityTree,tree)||!tree||!peek(tree,off::treeSkeleton,skeleton)||!skeleton||!peek(skeleton,off::skelGlobals,globals)||!globals||!peek(skeleton,off::skelCount,n)||!n)continue;
        n=std::min(n,actorBonesMax);
        if(!api.read(nullptr,globals,bones,std::size_t(n)*off::boneStride))continue;
        for(unsigned b=0;b<n;++b){
            float o[3];std::memcpy(o,bones+std::size_t(b)*off::boneStride+off::boneOrigin,sizeof o);
            for(unsigned p=0;p<t.bones;++p){const float d=dist3(o,t.pos[p]);if(std::isfinite(d)&&(best<0.f||d<best))best=d;}
        }
    }
    return best;
}
inline Action decide(std::uintptr_t manager,int id,const char* name,float& stillS,float& eyeM,float& actorM) noexcept {
    const auto entity=reinterpret_cast<std::uintptr_t>(api.entity(reinterpret_cast<const void*>(manager),id));
    if(!entity)return ActOther;
    const auto isProp=reinterpret_cast<bool(*)(std::uintptr_t)>(at<std::uintptr_t>(at<std::uintptr_t>(entity,0),0xa0));
    if(!isProp(entity))return ActActor;
    if(icontains(name,"player")||icontains(name,"john")||icontains(name,"marston")||icontains(name,"hat"))return ActJohn;
    const auto john=cs::johnSnapshot();
    const auto now=steadyNs();
    std::lock_guard lock(trackLock);
    for(auto& t:tracked){
        if(t.id!=id)continue;
        if(!t.pose)return ActNoTrack;
        stillS=now>t.stillSinceNs?float(double(now-t.stillSinceNs)*1e-9):0.f;
        if(john.valid){
            const float e[3]{john.eye[0],john.eye[1],john.eye[2]};
            for(unsigned b=0;b<t.bones;++b){const float d=dist3(t.pos[b],e);if(eyeM<0.f||d<eyeM)eyeM=d;}
            if(eyeM>=0.f&&eyeM<nearEyeM)return ActNearEye;
        }
        if(now-t.stillSinceNs<stillNs)return ActMoving;
        actorM=nearestActorM(manager,t);
        if(actorM>=0.f&&actorM<nearActorM)return ActNearActor;
        if(!t.kept)keptNow.fetch_add(1,std::memory_order_relaxed);
        t.kept=true;
        if(keptEndTime<0.f)keptEndTime=api.endTime(reinterpret_cast<const void*>(manager));
        return ActKept;
    }
    return ActNoTrack;
}
}
inline bool hideObjectHook(void* manager,int id) noexcept {
    using namespace detail;
    const auto lr=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0))-api.base;
    const bool clear=lr==lrClearAnim,slot=lr==lrWeaponSlot;
    stats.hideCalls.fetch_add(1,std::memory_order_relaxed);
    if(clear)stats.clearAnim.fetch_add(1,std::memory_order_relaxed);
    if(slot)stats.weaponSlot.fetch_add(1,std::memory_order_relaxed);
    secondBump(SecHides);
    const auto entity=reinterpret_cast<std::uintptr_t>(api.entity(manager,id));
    const char* name=entity?objectName(at<std::uintptr_t>(entity,off::entityObject)):nullptr;
    float stillS=-1.f,eyeM=-1.f,actorM=-1.f;Action a=slot?ActWeaponSlot:clear?ActHidden:ActOther;
    if(clear&&liveFp()&&propsWanted())a=decide(reinterpret_cast<std::uintptr_t>(manager),id,name,stillS,eyeM,actorM);
    else if(clear&&entity&&!reinterpret_cast<bool(*)(std::uintptr_t)>(at<std::uintptr_t>(at<std::uintptr_t>(entity,0),0xa0))(entity))a=ActActor;
    switch(a){
        case ActKept:stats.kept.fetch_add(1,std::memory_order_relaxed);secondBump(SecKept);break;
        case ActMoving:stats.stockMoving.fetch_add(1,std::memory_order_relaxed);break;
        case ActNearEye:stats.stockNearEye.fetch_add(1,std::memory_order_relaxed);break;
        case ActNearActor:stats.stockNearActor.fetch_add(1,std::memory_order_relaxed);break;
        case ActJohn:stats.stockJohn.fetch_add(1,std::memory_order_relaxed);break;
        case ActNoTrack:stats.stockNoTrack.fetch_add(1,std::memory_order_relaxed);break;
        case ActActor:stats.actorHides.fetch_add(1,std::memory_order_relaxed);break;
        default:break;
    }
    pushEvent(a,id,name,stillS,eyeM,actorM);
    return a==ActKept?false:api.hideObj(manager,id);
}
inline bool showObjectHook(void* manager,int id) noexcept {
    stats.shows.fetch_add(1,std::memory_order_relaxed);detail::secondBump(SecShows);
    return api.showObj(manager,id);
}
// Only counted: world objects the scene replaces (entity+0xd5e0 = the list of the last Hide Objects event). Its names, when the list changes.
inline void hideHiddenHook(void* entity,void* manager) noexcept {
    using namespace detail;
    stats.hiddenCalls.fetch_add(1,std::memory_order_relaxed);
    const auto list=at<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(entity),off::assetHiddenList);
    if(list&&list!=hiddenListLast.exchange(list,std::memory_order_relaxed)){
        stats.hiddenListChanges.fetch_add(1,std::memory_order_relaxed);
        const auto ids=at<std::uintptr_t>(list,0);const int count=at<std::uint16_t>(list,8);hiddenListCount.store(count,std::memory_order_relaxed);
        std::lock_guard lock(rowLock);
        for(unsigned i=0;i<namesMax;++i){
            const char* n=nullptr;
            if(ids&&int(i)<count)n=objectName(reinterpret_cast<std::uintptr_t>(api.object(manager,at<std::int32_t>(ids,std::size_t(i)*4))));
            copyName(hiddenListNames[i].data(),n);
        }
    }
    api.hideHidden(entity,manager);
}
inline bool hidePropHook(void* entity,std::uint32_t guid,bool force) noexcept {stats.hideProps.fetch_add(1,std::memory_order_relaxed);return api.hideProp(entity,guid,force);}
inline void removalShowHook(void* entity,void* manager) noexcept {stats.rbShows.fetch_add(1,std::memory_order_relaxed);api.rbShow(entity,manager);}
inline void removalHideHook(void* entity,void* manager) noexcept {stats.rbHides.fetch_add(1,std::memory_order_relaxed);api.rbHide(entity,manager);}

// ---- LIGHTS: sagRenderer::CalculateCutsceneHashes (main thread, DMZ) ----
namespace detail {
inline void noteCut(std::uint32_t shotNew,std::uint32_t previous,std::uint32_t next) noexcept {
    stats.cuts.fetch_add(1,std::memory_order_relaxed);secondBump(SecCuts);
    const bool live=liveFp()&&lightsWanted();
    const auto john=live?cs::johnSnapshot():cs::John{};
    const auto now=steadyNs();
    std::lock_guard lock(rowLock);if(!firstNs)firstNs=now;
    auto& r=cutRows[cutNext++%cutMax];r=CutRow{};
    r.ms=std::uint32_t((now-firstNs)/1000000u);r.shot=shotNew;r.prevShot=previous;r.next=next;
    r.lightsMin=shot.lightsMin;r.lightsMax=shot.lightsMax;r.hiddenProps=hiddenPropsNow.load(std::memory_order_relaxed);r.keptProps=keptNow.load(std::memory_order_relaxed);
    r.heldLights=heldLightsNow.load(std::memory_order_relaxed);
    std::copy_n(shot.sun,3,r.sun);std::copy_n(shot.sunMin,3,r.sunMin);std::copy_n(shot.sunMax,3,r.sunMax);
    r.clipOnFrames=shot.clipOnFrames;r.clipOffFrames=shot.clipOffFrames;r.nativeOnFrames=shot.nativeOnFrames;r.nativeFlips=shot.nativeFlips;r.appliedFlips=shot.appliedFlips;
    r.clipNow=appliedClipNow.load(std::memory_order_relaxed);r.nativeNow=nativeClipNow.load(std::memory_order_relaxed);
    const bool sunValid=shot.sunValid;float sun[3];std::copy_n(shot.sun,3,sun);
    shot=ShotAccum{};
    if(sunValid){shot.sunValid=true;std::copy_n(sun,3,shot.sun);std::copy_n(sun,3,shot.sunMin);std::copy_n(sun,3,shot.sunMax);}
    // The held rig moves only with the location, or while it has no light at all
    const auto held=heldShot.load(std::memory_order_relaxed);
    if(live&&held&&held!=shotNew){
        const float e[3]{john.eye[0],john.eye[1],john.eye[2]};
        const bool moved=john.valid&&heldEyeValid&&dist3(e,heldEye)>relatchMetres;
        const bool empty=heldSinceNs&&now-heldSinceNs>rigGraceNs&&heldLightsNow.load(std::memory_order_relaxed)==0;
        if(moved||empty){
            heldShot.store(shotNew,std::memory_order_relaxed);heldSinceNs=now;
            if(john.valid){std::copy_n(e,3,heldEye);heldEyeValid=true;}
            (moved?stats.relatchMoved:stats.relatchEmpty).fetch_add(1,std::memory_order_relaxed);lastRelatch.store(moved?"MOVED":"EMPTY_RIG",std::memory_order_relaxed);
        }
    }
    r.held=heldShot.load(std::memory_order_relaxed);
}
inline void latchFirst(std::uint32_t real) noexcept {
    std::uint32_t expected=0;
    if(!heldShot.compare_exchange_strong(expected,real,std::memory_order_relaxed))return;
    stats.latches.fetch_add(1,std::memory_order_relaxed);
    const auto john=cs::johnSnapshot();const auto now=steadyNs();
    std::lock_guard lock(rowLock);heldSinceNs=now;heldEyeValid=john.valid;
    if(john.valid){heldEye[0]=john.eye[0];heldEye[1]=john.eye[1];heldEye[2]=john.eye[2];}
}
}
inline bool hashesHook(void* renderer,const void* matrix,std::uint32_t* a,std::uint32_t* b,std::uint32_t* c,bool copy) noexcept {
    using namespace detail;
    const bool result=api.hashes(renderer,matrix,a,b,c,copy);
    stats.hashCalls.fetch_add(1,std::memory_order_relaxed);
    if(!result||!a||!b||!c||!*a)return result;
    const auto scene=*a,sh=*b,next=*c;
    if(sceneHash.exchange(scene,std::memory_order_relaxed)!=scene){
        stats.scenes.fetch_add(1,std::memory_order_relaxed);
        heldShot.store(0,std::memory_order_relaxed);realShot.store(0,std::memory_order_relaxed);realFromNullNs.store(0,std::memory_order_relaxed);
    }
    // The call without a matrix always reads the camera's current shot. One with a matrix returns the renderer's old hash while IsCurrentCamera says no:
    // it counts only when no matrix-free call was seen lately, and never as the held hash this hook itself put into the renderer.
    const auto held=heldShot.load(std::memory_order_relaxed);const auto now=steadyNs();
    bool trust=false;
    if(!matrix){trust=true;realFromNullNs.store(now,std::memory_order_relaxed);}
    else if(copy&&sh!=held){const auto n=realFromNullNs.load(std::memory_order_relaxed);trust=!n||now-n>1'000'000'000ull;}
    if(trust&&sh){
        realNext.store(next,std::memory_order_relaxed);
        const auto prev=realShot.exchange(sh,std::memory_order_relaxed);
        if(prev!=sh)noteCut(sh,prev,next);
    }
    if(!firstPersonMode()||!lightsWanted()){heldShot.store(0,std::memory_order_relaxed);return result;}
    if(!liveFp())return result;
    if(!heldShot.load(std::memory_order_relaxed)){const auto r=realShot.load(std::memory_order_relaxed);if(r)latchFirst(r);}
    if(const auto h=heldShot.load(std::memory_order_relaxed)){
        if(*b!=h||*c!=h)stats.overrides.fetch_add(1,std::memory_order_relaxed);
        *b=h;*c=h;
    }
    return result;
}

// ---- SUN + light counts: render thread ----
namespace detail {
struct RoomHold {void* room{};float pos[3]{};void* candidate{};std::uint64_t candidateNs{};bool valid{},prevNative{},prevApplied{},primed{};void* prevNativeRoom{};};
inline RoomHold hold{}; // render thread
inline void scanList(std::uintptr_t list,int count,std::uint32_t scene,std::uint32_t heldHash,std::uint32_t real,int& heldN,int& sceneN,int& anyN,int& realN,int& otherN,int& untagged) noexcept {
    for(int i=0;i<count;++i){
        const auto light=at<std::uintptr_t>(list,std::size_t(i)*8);if(!light)continue;
        const auto flags=at<std::uint16_t>(light,off::lightFlags);
        if((flags&off::slotUsableMask)!=1u)continue;
        if(!(flags&off::slotFlagTagged)){++untagged;continue;}
        const auto h=at<std::uint32_t>(light,off::lightHash);
        if(h==0u)++anyN;else if(h==scene)++sceneN;else if(h==heldHash)++heldN;else if(h==real)++realN;else ++otherN;
    }
}
}
inline void setupHook(void* self,const void* state,void* array,const void* viewport,const void* shaft,bool mainList) noexcept {
    using namespace detail;
    api.setup(self,state,array,viewport,shaft,mainList);
    if(!mainList||!array||!liveFp())return;
    const auto calls=stats.setupCalls.fetch_add(1,std::memory_order_relaxed);
    const int visible=at<std::int32_t>(reinterpret_cast<std::uintptr_t>(array),off::visibleCount);
    visibleNow.store(visible,std::memory_order_relaxed);
    {std::lock_guard lock(rowLock);shot.lightsMin=shot.lightsMin<0?visible:std::min(shot.lightsMin,visible);shot.lightsMax=std::max(shot.lightsMax,visible);}
    secondMaxOf(SecLights,std::uint32_t(std::max(visible,0)));
    if(calls&7u)return;
    stats.scans.fetch_add(1,std::memory_order_relaxed);
    const auto scene=sceneHash.load(std::memory_order_relaxed),heldHash=heldShot.load(std::memory_order_relaxed),real=realShot.load(std::memory_order_relaxed);
    int heldN=0,sceneN=0,anyN=0,realN=0,otherN=0,untagged=0;
    if(const auto pm=at<std::uintptr_t>(api.pointMgr,0))scanList(pm+off::lightList,std::clamp(at<std::int32_t>(pm,off::pointCount),0,512),scene,heldHash,real,heldN,sceneN,anyN,realN,otherN,untagged);
    if(const auto sm=at<std::uintptr_t>(api.spotMgr,0))scanList(sm+off::lightList,std::clamp(at<std::int32_t>(sm,off::spotCount),0,256),scene,heldHash,real,heldN,sceneN,anyN,realN,otherN,untagged);
    heldLightsNow.store(heldN,std::memory_order_relaxed);sceneLightsNow.store(sceneN,std::memory_order_relaxed);anyLightsNow.store(anyN,std::memory_order_relaxed);
    realLightsNow.store(realN,std::memory_order_relaxed);otherLightsNow.store(otherN,std::memory_order_relaxed);untaggedNow.store(untagged,std::memory_order_relaxed);
    secondMaxOf(SecHeld,std::uint32_t(heldN));secondMaxOf(SecOther,std::uint32_t(otherN+realN));
}
inline void findRoomHook(void* lights,const void* viewport) noexcept {
    using namespace detail;
    api.findRoom(lights,viewport);
    const auto mgr=reinterpret_cast<std::uintptr_t>(lights);
    const auto now=steadyNs();
    if(!mgr||!viewport||!liveFp()){hold.valid=false;hold.candidate=nullptr;hold.primed=false;return;}
    secondTick(now);secondBump(SecFrames);stats.roomCalls.fetch_add(1,std::memory_order_relaxed);
    if(stats.queryOffset.load(std::memory_order_relaxed)<0.f){
        const float nearOffset=at<float>(mgr,off::queryOffset);
        stats.queryOffset.store(std::isfinite(nearOffset)&&nearOffset>=0.f?nearOffset:0.f,std::memory_order_relaxed);
        stats.portalClipOn.store(at<std::uint8_t>(mgr,off::portalClipOn),std::memory_order_relaxed);stats.portalClear.store(at<std::uint8_t>(mgr,off::portalClear),std::memory_order_relaxed);
    }
    const auto nativeRoom=at<void*>(mgr,off::roomAny);const bool nativeOn=at<void*>(mgr,off::roomClip)!=nullptr;
    bool appliedOn=nativeOn;
    if(sunWanted()){
        const auto world=at<void*>(api.world,0);
        if(world){
            const auto vp=reinterpret_cast<std::uintptr_t>(viewport);
            alignas(16) float point[4]{at<float>(vp,off::viewportPos),at<float>(vp,off::viewportPos+4),at<float>(vp,off::viewportPos+8),0.f};
            if(std::isfinite(point[0]+point[1]+point[2])){
                void* raw=api.query(world,point);stats.roomQueries.fetch_add(1,std::memory_order_relaxed);
                rawRoomNow.store(raw!=nullptr,std::memory_order_relaxed);
                if(hold.valid&&raw!=hold.room&&hold.room){
                    alignas(16) float at4[4]{hold.pos[0],hold.pos[1],hold.pos[2],0.f};
                    if(api.query(world,at4)!=hold.room){hold.valid=false;stats.roomDrops.fetch_add(1,std::memory_order_relaxed);}
                }
                if(!hold.valid||raw==hold.room){
                    if(!hold.valid)stats.roomChanges.fetch_add(1,std::memory_order_relaxed);
                    hold.valid=true;hold.room=raw;std::copy_n(point,3,hold.pos);hold.candidate=nullptr;hold.candidateNs=0;
                }else{
                    if(raw!=hold.candidate||!hold.candidateNs){hold.candidate=raw;hold.candidateNs=now;}
                    const float moved=dist3(point,hold.pos);
                    if(moved>roomJumpM||(moved>roomMoveM&&now-hold.candidateNs>roomHoldNs)){
                        hold.room=raw;std::copy_n(point,3,hold.pos);hold.candidate=nullptr;hold.candidateNs=0;stats.roomChanges.fetch_add(1,std::memory_order_relaxed);
                    }
                }
                // The two stores of FindRoom 0x2a36ca0 / 0x2a36cbc, for the held room. Its pointer came out of a query in this frame (raw, or the re-query above).
                put<void*>(mgr,off::roomAny,hold.room);
                const bool clip=hold.room&&!(at<std::uint8_t>(reinterpret_cast<std::uintptr_t>(hold.room),off::roomFlags)&1u);
                put<void*>(mgr,off::roomClip,clip?hold.room:nullptr);appliedOn=clip;
                const auto john=cs::johnSnapshot();
                if(john.valid){const float e[3]{john.eye[0],john.eye[1],john.eye[2]};stats.eyeQueryM.store(dist3(point,e),std::memory_order_relaxed);}
            }
        }
    }else hold.valid=false;
    nativeClipNow.store(nativeOn,std::memory_order_relaxed);appliedClipNow.store(appliedOn,std::memory_order_relaxed);
    if(appliedOn)secondBump(SecClipOn);
    if(nativeOn)secondBump(SecNativeOn);
    const bool nativeFlip=hold.primed&&nativeOn!=hold.prevNative,appliedFlip=hold.primed&&appliedOn!=hold.prevApplied;
    if(nativeFlip){stats.nativeFlips.fetch_add(1,std::memory_order_relaxed);secondBump(SecNativeFlips);}
    if(appliedFlip){stats.appliedFlips.fetch_add(1,std::memory_order_relaxed);secondBump(SecAppliedFlips);}
    hold.primed=true;hold.prevNative=nativeOn;hold.prevApplied=appliedOn;hold.prevNativeRoom=nativeRoom;
    std::lock_guard lock(rowLock);
    if(appliedOn)++shot.clipOnFrames;else ++shot.clipOffFrames;
    if(nativeOn)++shot.nativeOnFrames;
    if(nativeFlip)++shot.nativeFlips;
    if(appliedFlip)++shot.appliedFlips;
}
// shadowRenderInto (render thread): the sun direction the atlas is rendered with.
inline void noteSun(const void* sun) noexcept {
    if(!sun||!installed.load(std::memory_order_relaxed)||!liveFp())return;
    float s[3];std::memcpy(s,sun,sizeof s);
    if(!std::isfinite(s[0]+s[1]+s[2]))return;
    std::lock_guard lock(rowLock);
    if(!shot.sunValid){shot.sunValid=true;std::copy_n(s,3,shot.sunMin);std::copy_n(s,3,shot.sunMax);}
    for(int i=0;i<3;++i){shot.sun[i]=s[i];shot.sunMin[i]=std::min(shot.sunMin[i],s[i]);shot.sunMax[i]=std::max(shot.sunMax[i],s[i]);}
}

// ---- main thread, every PostCameraUpdate: prop poses (guarded reads, no engine calls), the kept props' way back to stock ----
inline void mainTick() noexcept {
    using namespace detail;
    if(!installed.load(std::memory_order_relaxed))return;
    const auto now=steadyNs();
    static unsigned polls=0;
    const unsigned tick=polls++;
    if((tick&31u)==0){leverProps.store(leverOf("debug.rdrvr.vr.cutscene.props"),std::memory_order_relaxed);leverLights.store(leverOf("debug.rdrvr.vr.cutscene.lights"),std::memory_order_relaxed);
        leverSun.store(leverOf("debug.rdrvr.vr.cutscene.sun"),std::memory_order_relaxed);}
    int keptCount=keptNow.load(std::memory_order_relaxed);
    const bool live=liveFp();
    if(!live&&!keptCount)return;
    // Guarded reads on the main thread: the bones every 4th tick are enough for stillNs; every tick only while a prop is kept (its way back must not miss the frames before Unload Models)
    const bool sample=(tick&3u)==0;
    if(!sample&&!keptCount)return;
    std::uintptr_t manager{};
    if(!peek(api.manager,0,manager)||!manager)return;
    const auto asset=manager+off::managerAsset;int streamers{};
    if(!peek(asset,off::assetStreamerCount,streamers)||streamers<=0||streamers>int(off::maxStreamers)){
        std::lock_guard lock(trackLock);for(auto& t:tracked)t=Tracked{};keptEndTime=-1.f;keptNow.store(0,std::memory_order_relaxed);trackedNow.store(0,std::memory_order_relaxed);return;}
    std::array<std::pair<int,Action>,trackMax> hides{};unsigned hideCount=0;
    {
        std::lock_guard lock(trackLock);
        // A new scene: its object ids start over
        {static std::uint32_t trackedScene{};const auto scene=sceneHash.load(std::memory_order_relaxed);
         if(scene!=trackedScene){trackedScene=scene;for(auto& t:tracked)t=Tracked{};keptEndTime=-1.f;keptNow.store(0,std::memory_order_relaxed);}}
        int hiddenN=0,posed=0,count=0;
        for(int i=0;i<streamers;++i){
            std::uintptr_t st{},vt{},object{},stub{},tree{},skeleton{},globals{};int id{};std::uint8_t hidden{},flags{},owner{};
            if(!peek(asset,off::assetStreamers+std::size_t(i)*8,st)||!st||!peek(st,0,vt)||vt!=api.base+propStreamerVtable)continue;
            if(!peek(st,off::streamerObject,object)||!peek(object,off::objectId,id))continue;
            peek(st,off::streamerStub,stub);peek(st,off::streamerHidden,hidden);if(hidden)++hiddenN;
            const bool initialized=peek(st,off::streamerFlags,flags)&&(flags&off::streamerInitialized);
            const bool showsAtEnd=peek(st,off::streamerOwner,owner)&&(owner&1u);
            Tracked* t=nullptr;Tracked* freeSlot=nullptr;
            for(auto& x:tracked){if(x.id==id){t=&x;break;}if(x.id<0&&!freeSlot)freeSlot=&x;}
            if(!t){if(!freeSlot)continue;t=freeSlot;*t=Tracked{};t->id=id;t->stillSinceNs=now;}
            t->stub=stub;t->seenNs=now;t->hidden=hidden;t->initialized=initialized;t->showsAtEnd=showsAtEnd;++count;
            if(!sample)continue;
            // Up to trackBones bones in world space: the root may stay while a child carries the motion
            std::uint32_t bones{};
            bool pose=peek(st,off::streamerTree,tree)&&tree&&peek(tree,off::treeSkeleton,skeleton)&&skeleton&&peek(skeleton,off::skelGlobals,globals)&&globals&&peek(skeleton,off::skelCount,bones)&&bones>0;
            const unsigned useBones=pose?std::min(bones,trackBones):0u;
            float m[trackBones][3]{};
            for(unsigned b=0;pose&&b<useBones;++b)pose=api.read(nullptr,globals+std::size_t(b)*16*sizeof(float)+12*sizeof(float),m[b],sizeof m[b])&&std::isfinite(m[b][0]+m[b][1]+m[b][2]);
            if(pose){
                bool moved=!t->pose||t->bones!=useBones;
                for(unsigned b=0;!moved&&b<useBones;++b)moved=dist3(m[b],t->anchor[b])>stillMoveM;
                if(moved){std::memcpy(t->anchor,m,sizeof m);t->stillSinceNs=now;}
                std::memcpy(t->pos,m,sizeof m);t->bones=useBones;++posed;
            }
            t->pose=pose;
        }
        hiddenPropsNow.store(hiddenN,std::memory_order_relaxed);trackedNow.store(count,std::memory_order_relaxed);
        if(sample)posedNow.store(posed,std::memory_order_relaxed);
        // Entities are the engine's own lookup: only for the props already kept, a short list
        float timeNow{};const bool haveTime=peek(manager,off::managerTime,timeNow);
        // Out of Play / Paused = a stop (skip, script, death, load, the natural end): the release comes before End of Scene and Unload Models
        std::int32_t state{};const bool haveState=peek(manager,off::managerState,state);
        const bool playing=haveState&&(state==off::statePlay||state==off::statePaused);
        const bool ending=!playing||(haveTime&&keptEndTime>=0.f&&timeNow>=keptEndTime-releaseBeforeEndS);
        // The composer left FIRST PERSON (a fallback, the row, R3) and stayed out: the view is no longer ours, the set goes back to the script's. A pause keeps it.
        static std::uint64_t lastLiveNs=0;
        if(live)lastLiveNs=now;
        const bool left=!live&&!(haveState&&state==off::statePaused)&&(!lastLiveNs||now-lastLiveNs>liveGraceNs);
        const bool stopped=!firstPersonMode()||!propsWanted()||left;
        int keptLeft=0;
        for(auto& y:tracked){
            if(y.id<0||!y.kept)continue;
            if(y.seenNs!=now||y.hidden){y.kept=false;continue;}
            // Uninitialized while kept (Unload Models came first): the stock end state of a prop that was hidden here
            if(!y.initialized){y.kept=false;if(!y.showsAtEnd)hides[hideCount++]={y.id,ActReleased};continue;}
            const auto entity=reinterpret_cast<std::uintptr_t>(api.entity(reinterpret_cast<const void*>(manager),y.id));
            std::uint8_t flags{};
            if(entity&&peek(entity,off::entityFlags,flags)&&(flags&2u)){y.kept=false;stats.resumed.fetch_add(1,std::memory_order_relaxed);continue;}
            if(!entity){y.kept=false;continue;}
            if(ending||stopped){y.kept=false;hides[hideCount++]={y.id,ActReleased};continue;}
            bool doubled=false;
            for(const auto& z:tracked){
                // Another copy of this model in use (it has a clip and moves): two would show. A copy resting elsewhere is another object of the set.
                if(z.id<0||z.id==y.id||z.kept||z.seenNs!=now||z.stub!=y.stub||!y.stub||z.hidden||now-z.stillSinceNs>=stillNs)continue;
                const auto ze=reinterpret_cast<std::uintptr_t>(api.entity(reinterpret_cast<const void*>(manager),z.id));
                std::uint8_t zf{};
                if(ze&&peek(ze,off::entityFlags,zf)&&(zf&2u)){doubled=true;break;}
            }
            if(doubled){y.kept=false;hides[hideCount++]={y.id,ActDouble};continue;}
            ++keptLeft;
        }
        keptNow.store(keptLeft,std::memory_order_relaxed);keptCount=keptLeft;
        if(!keptLeft&&(ending||stopped))keptEndTime=-1.f;
    }
    for(unsigned i=0;i<hideCount;++i){
        stockHide(manager,hides[i].first);
        (hides[i].second==ActDouble?stats.doubles:stats.released).fetch_add(1,std::memory_order_relaxed);
        const auto entity=reinterpret_cast<std::uintptr_t>(api.entity(reinterpret_cast<const void*>(manager),hides[i].first));
        pushEvent(hides[i].second,hides[i].first,entity?objectName(at<std::uintptr_t>(entity,off::entityObject)):nullptr,-1.f,-1.f,-1.f);
    }
}

template<class Replace> inline void install(ReadFn read,std::uintptr_t base,Replace&& replace) noexcept {
    using namespace detail;
    if(killed(Feature::Cutscene)){installState.store("KILLED",std::memory_order_relaxed);return;}
#if defined(__ANDROID__)
    {char v[PROP_VALUE_MAX]{};if(__system_property_get("debug.rdrvr.vr.cutscene.steady",v)==1&&v[0]=='0'){installState.store("OFF",std::memory_order_relaxed);return;}}
#endif
    if(!read){installState.store("NO READ",std::memory_order_relaxed);return;}
    api.read=read;api.base=base;
    const auto ok=[&](const Print& p){return codeHash(base+p.offset,p.size)==p.fnv;};
    const auto got=[&](std::uintptr_t slot,std::uintptr_t target){std::uintptr_t value{};return read(nullptr,base+slot,&value,sizeof value)&&value==base+target;};
    bool all=true;
    for(const auto& p:{hashesPrint,hideObjPrint,showObjPrint,findRoomPrint,setupPrint,hideHiddenPrint,hidePropPrint,rbShowPrint,rbHidePrint,queryPrint,entityPrint,objectPrint,endTimePrint})all=all&&ok(p);
    for(const auto& p:witnessPrints)all=all&&ok(p);
    all=all&&got(hashesGot,hashesPrint.offset)&&got(hideObjGot,hideObjPrint.offset)&&got(showObjGot,showObjPrint.offset)&&got(findRoomGot,findRoomPrint.offset)&&got(setupGot,setupPrint.offset)&&
        got(hideHiddenGot,hideHiddenPrint.offset)&&got(hidePropGot,hidePropPrint.offset)&&got(rbShowGot,rbShowPrint.offset)&&got(rbHideGot,rbHidePrint.offset)&&
        got(managerGot,managerOffset)&&got(worldGot,worldOffset)&&got(pointMgrGot,pointMgrOffset)&&got(spotMgrGot,spotMgrOffset);
    if(!all){installState.store("ENGINE MISMATCH",std::memory_order_relaxed);lastError.store("ENGINE MISMATCH",std::memory_order_relaxed);return;}
    api.hashes=reinterpret_cast<HashesFn>(base+hashesPrint.offset);api.hideObj=reinterpret_cast<ObjectFn>(base+hideObjPrint.offset);api.showObj=reinterpret_cast<ObjectFn>(base+showObjPrint.offset);
    api.findRoom=reinterpret_cast<FindRoomFn>(base+findRoomPrint.offset);api.setup=reinterpret_cast<SetupFn>(base+setupPrint.offset);api.hideHidden=reinterpret_cast<EntityFn>(base+hideHiddenPrint.offset);
    api.hideProp=reinterpret_cast<HidePropFn>(base+hidePropPrint.offset);api.rbShow=reinterpret_cast<EntityFn>(base+rbShowPrint.offset);api.rbHide=reinterpret_cast<EntityFn>(base+rbHidePrint.offset);
    api.query=reinterpret_cast<QueryFn>(base+queryPrint.offset);api.entity=reinterpret_cast<LookupFn>(base+entityPrint.offset);api.object=reinterpret_cast<LookupFn>(base+objectPrint.offset);api.endTime=reinterpret_cast<EndTimeFn>(base+endTimePrint.offset);
    api.manager=base+managerOffset;api.world=base+worldOffset;api.pointMgr=base+pointMgrOffset;api.spotMgr=base+spotMgrOffset;
    struct Route {std::uintptr_t slot,original,hook;};
    const Route routes[]{{hashesGot,base+hashesPrint.offset,reinterpret_cast<std::uintptr_t>(&hashesHook)},{hideObjGot,base+hideObjPrint.offset,reinterpret_cast<std::uintptr_t>(&hideObjectHook)},
        {showObjGot,base+showObjPrint.offset,reinterpret_cast<std::uintptr_t>(&showObjectHook)},{findRoomGot,base+findRoomPrint.offset,reinterpret_cast<std::uintptr_t>(&findRoomHook)},
        {setupGot,base+setupPrint.offset,reinterpret_cast<std::uintptr_t>(&setupHook)},{hideHiddenGot,base+hideHiddenPrint.offset,reinterpret_cast<std::uintptr_t>(&hideHiddenHook)},
        {hidePropGot,base+hidePropPrint.offset,reinterpret_cast<std::uintptr_t>(&hidePropHook)},{rbShowGot,base+rbShowPrint.offset,reinterpret_cast<std::uintptr_t>(&removalShowHook)},
        {rbHideGot,base+rbHidePrint.offset,reinterpret_cast<std::uintptr_t>(&removalHideHook)}};
    unsigned done=0;
    for(const auto& r:routes){if(!replace(base+r.slot,r.original,r.hook))break;++done;}
    if(done!=std::size(routes)){
        while(done){const auto& r=routes[--done];replace(base+r.slot,r.hook,r.original);}
        installState.store("PATCH FAILED",std::memory_order_relaxed);lastError.store("PATCH FAILED",std::memory_order_relaxed);return;}
    installed.store(true,std::memory_order_release);installState.store("READY",std::memory_order_relaxed);
}

inline std::string report() {
    using ull=unsigned long long;char b[1400];std::string o;
    const auto u=[](const std::atomic<std::uint64_t>& a){return ull(a.load(std::memory_order_relaxed));};
    const auto& s=stats;
    std::snprintf(b,sizeof b,"{\"install\":\"%s\",\"error\":\"%s\",\"live\":%d,\"rows\":{\"props\":%u,\"lights\":%u,\"sun\":%u},\"levers\":{\"props\":%d,\"lights\":%d,\"sun\":%d},"
        "\"props\":{\"on\":%d,\"hideCalls\":%llu,\"clearAnim\":%llu,\"weaponSlot\":%llu,\"actorHides\":%llu,\"kept\":%llu,\"keptNow\":%d,\"resumedByClip\":%llu,\"released\":%llu,\"doubles\":%llu,"
        "\"stockMoving\":%llu,\"stockNearEye\":%llu,\"stockNearActor\":%llu,\"stockJohn\":%llu,\"stockNoTrack\":%llu,\"shows\":%llu,\"hiddenNow\":%d,\"tracked\":%d,\"withPose\":%d,"
        "\"worldHide\":{\"calls\":%llu,\"listChanges\":%llu,\"listCount\":%d,\"hideProp\":%llu,\"removalShow\":%llu,\"removalHide\":%llu},\"listNames\":[",
        installState.load(std::memory_order_relaxed),lastError.load(std::memory_order_relaxed),liveFp()?1:0,settings()[Key::CutsceneProps],settings()[Key::CutsceneLights],settings()[Key::CutsceneSun],
        leverProps.load(),leverLights.load(),leverSun.load(),propsWanted()?1:0,u(s.hideCalls),u(s.clearAnim),u(s.weaponSlot),u(s.actorHides),u(s.kept),keptNow.load(),u(s.resumed),u(s.released),u(s.doubles),
        u(s.stockMoving),u(s.stockNearEye),u(s.stockNearActor),u(s.stockJohn),u(s.stockNoTrack),u(s.shows),hiddenPropsNow.load(),trackedNow.load(),posedNow.load(),
        u(s.hiddenCalls),u(s.hiddenListChanges),hiddenListCount.load(),u(s.hideProps),u(s.rbShows),u(s.rbHides));
    o=b;
    std::lock_guard lock(rowLock);
    {bool first=true;for(const auto& n:hiddenListNames){if(!n[0])continue;o+=first?"\"":",\"";o+=n.data();o+='"';first=false;}}
    o+="],\"eventColumns\":\"ms,name,id,action,stillS,eyeM,actorM\",\"events\":[";
    {const auto n=eventNext;
     for(std::uint32_t k=n>eventMax?n-eventMax:0,first=1;k<n;++k,first=0){const auto& e=events[k%eventMax];
        std::snprintf(b,sizeof b,"%s[%u,\"%s\",%d,\"%s\",%.2f,%.2f,%.2f]",first?"":",",e.ms,e.name,e.id,actionNames[e.action<ActCount?e.action:ActOther],double(e.stillS),double(e.eyeM),double(e.actorM));o+=b;}}
    std::snprintf(b,sizeof b,"]},\"lights\":{\"on\":%d,\"scene\":\"%08x\",\"held\":\"%08x\",\"real\":\"%08x\",\"realNext\":\"%08x\",\"scenes\":%llu,\"shotChanges\":%llu,\"latches\":%llu,\"relatchMoved\":%llu,\"relatchEmptyRig\":%llu,\"lastRelatch\":\"%s\","
        "\"overrides\":%llu,\"hashCalls\":%llu,\"visibleNow\":%d,\"heldHash\":%d,\"sceneHash\":%d,\"anyShot\":%d,\"realShotOnly\":%d,\"otherShots\":%d,\"untagged\":%d,\"keptBeyondShot\":%d},",
        lightsWanted()?1:0,sceneHash.load(),heldShot.load(),realShot.load(),realNext.load(),u(s.scenes),u(s.cuts),u(s.latches),u(s.relatchMoved),u(s.relatchEmpty),lastRelatch.load(),u(s.overrides),u(s.hashCalls),
        visibleNow.load(),heldLightsNow.load(),sceneLightsNow.load(),anyLightsNow.load(),realLightsNow.load(),otherLightsNow.load(),untaggedNow.load(),heldShot.load()!=realShot.load()?heldLightsNow.load():0);
    o+=b;
    std::snprintf(b,sizeof b,"\"sun\":{\"on\":%d,\"calls\":%llu,\"queries\":%llu,\"nativeFlips\":%llu,\"appliedFlips\":%llu,\"nativeClip\":%d,\"appliedClip\":%d,\"eyeRoom\":%d,\"roomChanges\":%llu,\"roomDrops\":%llu,"
        "\"queryEyeM\":%.2f,\"nearOffset\":%.3f,\"portalClipOn\":%d,\"portalClear\":%d,\"dirNow\":[%.4f,%.4f,%.4f]},",
        sunWanted()?1:0,u(s.roomCalls),u(s.roomQueries),u(s.nativeFlips),u(s.appliedFlips),nativeClipNow.load()?1:0,appliedClipNow.load()?1:0,rawRoomNow.load()?1:0,u(s.roomChanges),u(s.roomDrops),
        double(s.eyeQueryM.load()),double(s.queryOffset.load()),s.portalClipOn.load(),s.portalClear.load(),double(shot.sun[0]),double(shot.sun[1]),double(shot.sun[2]));
    o+=b;
    // One row per stock shot change. The ranges (lights, sun, clip) are of the shot that just ended; sun min / max equal = the sun did not move in it.
    o+="\"cutColumns\":\"ms,shot,prevShot,nextShot,heldShot,lightsMin,lightsMax,heldRigLights,hiddenProps,keptProps,sunMin,sunMax,clipOnFrames,clipOffFrames,nativeOnFrames,nativeFlips,appliedFlips,clipNow,nativeNow\",\"cuts\":[";
    {const auto n=cutNext;
     for(std::uint32_t k=n>cutMax?n-cutMax:0,first=1;k<n;++k,first=0){const auto& r=cutRows[k%cutMax];
        std::snprintf(b,sizeof b,"%s[%u,\"%08x\",\"%08x\",\"%08x\",\"%08x\",%d,%d,%d,%d,%d,[%.4f,%.4f,%.4f],[%.4f,%.4f,%.4f],%u,%u,%u,%u,%u,%d,%d]",first?"":",",r.ms,r.shot,r.prevShot,r.next,r.held,r.lightsMin,r.lightsMax,r.heldLights,r.hiddenProps,r.keptProps,
            double(r.sunMin[0]),double(r.sunMin[1]),double(r.sunMin[2]),double(r.sunMax[0]),double(r.sunMax[1]),double(r.sunMax[2]),r.clipOnFrames,r.clipOffFrames,r.nativeOnFrames,r.nativeFlips,r.appliedFlips,r.clipNow?1:0,r.nativeNow?1:0);o+=b;}}
    o+="],\"secondColumns\":\"s";
    for(const char* n:secondNames){o+=',';o+=n;}
    o+="\",\"seconds\":[";
    {const auto now=secondNow.load(std::memory_order_relaxed);bool first=true;
     for(std::uint32_t k=now>=secondMax?now-secondMax+1:0;k<=now;++k){const auto& r=seconds[k%secondMax];if(r.second.load(std::memory_order_relaxed)!=k)continue;
        o+=first?"[":",[";first=false;o+=std::to_string(k);for(const auto& a:r.v){o+=',';o+=std::to_string(a.load(std::memory_order_relaxed));}o+=']';}}
    o+="]}";
    return o;
}
} // namespace rdrvr::vr::cssteady
