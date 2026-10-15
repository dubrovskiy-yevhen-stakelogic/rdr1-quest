#pragma once
// TELEPORT. Read the local player's position/heading on the engine main thread, save spots to
// <files>/rdrvr/teleport-spots.txt, and teleport the player to a saved spot or to coordinates.
//
//  - runs ONLY inside the mod's wrapper of sagShellDerived::UpdateInputs (GOT 0x2e499c0) when it returns to
//    sagShellDerived::Execute (0x23a0340): the main game thread, after PerformDMZ/WaitForRenderThread and before
//    the next simulation step - the same thread and lock state the engine's own scripts teleport from.
//  - teleport = sagActorScript::TeleportActorWithHeading(sagGuid, scrVector, float headingDeg, bool, bool, bool)
//    (0x1caafb8), the TELEPORT_ACTOR_WITH_HEADING script command. For the player actor it calls
//    TeleportActorToMatrix, which (>= 25 m) sets g_playerTeleporting -> sagShellDerived::UpdateGameState switches
//    GAMEDATA to state 5 (load) and back to 3 once sagStreamer::IsWorldLoadedPriority(6): the engine streams the
//    destination itself. Called with (false,false,false): stop current anim + nav soft reset, no 0x80 flag.
//  - player = PLAYERMGR+0xa20 (sagPlayer, vtable checked, active +0x5f1), actor GUID +0x5f4 resolved through
//    aGuidGeneral<aActor>::sm_Manager/sm_ManagerSlots (index/generation), actor+0xb0 -> +0x18 -> Matrix34
//    (rows a,b,c,d at 16-byte stride, Y up, position = d, heading = atan2(c.x, c.z) degrees).
//  - refused unless GAMEDATA state 3, no teleport in progress (g_playerTeleporting 0), player actor flags
//    (+0x118 & 3) == 3, alive (actor+0x60 -> +0x20 > 0), no control locks (player +0x104/+0x10c/+0x10d) and
//    on foot (player+0x5d4 == 2; mounted/vehicle states are refused, not dismounted).
// Menu: one GRAPHICS row TELEPORT (the panel holds 25 lines): stick left/right picks SAVE SPOT / SPOT 1..N,
// A executes. Properties (polled once per second on the main thread, first value = baseline, a CHANGE triggers):
//   debug.rdrvr.teleport       "<n>" saved spot n (1-based) | "x,y,z[,heading]" ; an optional ":tag" suffix lets the
//                              same target be requested again ("3:a", "3:b").
//   debug.rdrvr.teleport.save  any new non-empty value other than "0" saves the current spot (label PROP).
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include "GfxProperty.h" // debug.rdrvr.gfx live GRAPHICS overrides (polled below)
#include <mutex>
#include <string>
#include <vector>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#include <unistd.h>
#endif

namespace rdrvr::teleport {

// ---- ELF identities (librdr.so, sha in TraceSymbols; file offset = vaddr - 0x4000 for .text) ----
inline constexpr std::uintptr_t withHeadingOffset=0x1caafb8;        // sagActorScript::TeleportActorWithHeading
inline constexpr std::uintptr_t toMatrixOffset=0x1caaa8c;           // sagActorScript::TeleportActorToMatrix
inline constexpr std::uintptr_t toMatrixGotOffset=0x2e33830;        // its JUMP_SLOT (WithHeading calls it via PLT)
inline constexpr std::uintptr_t updateInputsOffset=0x23a0a28;       // sagShellDerived::UpdateInputs()
inline constexpr std::uintptr_t updateInputsGotOffset=0x2e499c0;    // its JUMP_SLOT
inline constexpr std::uintptr_t updateInputsReturnOffset=0x23a0340; // return address of the call in sagShellDerived::Execute
inline constexpr std::uintptr_t updateGameStateOffset=0x23a04c0;    // sagShellDerived::UpdateGameState(bool&)
// Completion of a long (>= 25 m) player teleport. UpdateGameState only moves g_playerTeleporting=1 to GAMEDATA state 5
// (and later clears the flag once IsWorldLoadedPriority(6)) while the HUD fade is OUT (hudManager+0x25835 bit 0x80 /
// +0x2583c bit 0, set by hudManager::FadeOut): scripts fade out, teleport, wait, fade in. Without a fade the flag stays 1
// forever. The mod therefore owns the flag it caused: it polls the same engine query and clears it.
inline constexpr std::uintptr_t worldLoadedOffset=0x2ab4714;        // sagStreamer::IsWorldLoadedPriority(int) const query
inline constexpr std::uintptr_t streamerUpdateOffset=0x2ab4558;     // sagStreamer::Update() = clear +0x11d (one-frame flag)
inline constexpr std::uintptr_t streamerOffset=0x4e8a62d0,streamerGotOffset=0x2e01468; // sagStreamer::smInstance (pointer)
inline constexpr int worldLoadedPriority=6;                         // the value UpdateGameState passes
inline constexpr std::uintptr_t gameDataOffset=0x4e75c4b8,gameDataGotOffset=0x2ddc550;                 // GAMEDATA (state u32 at +0)
inline constexpr std::uintptr_t playerTeleportingOffset=0x4e75d36b,playerTeleportingGotOffset=0x2e01518; // g_playerTeleporting (u8)
inline constexpr std::uintptr_t playerManagerGotOffset=0x2e014e0;   // PLAYERMGR
inline constexpr std::uintptr_t actorManagerGotOffset=0x2deaba8,actorSlotsGotOffset=0x2deabb0; // aGuidGeneral<aActor>
// Exact bytes (whole TeleportActorWithHeading and UpdateInputs, ToMatrix/UpdateGameState heads, the Execute call site).
inline constexpr std::array<unsigned char,324> withHeadingFingerprint{
#include "gamecode/Teleport_1caafb8_324_8d.inc"
};
inline constexpr std::array<unsigned char,128> toMatrixFingerprint{
#include "gamecode/Teleport_1caaa8c_128_8d.inc"
};
inline constexpr std::array<unsigned char,228> updateInputsFingerprint{
#include "gamecode/Teleport_23a0a28_228_8d.inc"
};
inline constexpr std::array<unsigned char,4> updateInputsSiteFingerprint{225,136,35,148};
// Fingerprints: the whole UpdateGameState (the fade-gated g_playerTeleporting -> state 5 -> clear path),
// IsWorldLoadedPriority, sagStreamer::Update.
inline constexpr std::array<unsigned char,336> updateGameStateFingerprint{
#include "gamecode/Teleport_23a04c0_336_8d.inc"
};
inline constexpr std::array<unsigned char,476> worldLoadedFingerprint{
#include "gamecode/Teleport_2ab4714_476_8d.inc"
};
inline constexpr std::array<unsigned char,8> streamerUpdateFingerprint{31,116,4,57,192,3,95,214};

// ---- shared state ----
struct Spot {double x{},y{},z{},heading{};std::string label;bool hasView{};double viewYaw{},viewPitch{};}; // view = rendered view yaw/pitch (deg)
using TeleportFn=void(*)(std::uint32_t guid,float x,float y,float z,float headingDegrees,bool keepAnim,bool keepItem,bool flag80);
using WorldLoadedFn=bool(*)(void* streamer,int priority);
struct EngineApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};
    std::uintptr_t playerManager{},playerManagerTable{},playerTable{},actorManagerGlobal{},actorSlotsGlobal{};
    std::uintptr_t gameData{},playerTeleporting{};
    TeleportFn teleport{};
    std::uintptr_t streamer{};WorldLoadedFn worldLoaded{}; // streamer = address of sagStreamer::smInstance (a pointer)
};
enum class Feedback : unsigned {None,SavePending,Saved,SaveFailed,TpPending,TpLoading,TpDone,TpFailed};
struct Request {int kind{};int spot{};double x{},y{},z{},heading{};bool hasHeading{};bool hasView{};double viewYaw{};const char* source{""};std::uint64_t ns{};}; // kind 1 spot, 2 coords
struct Last {
    bool valid{};std::string source;int spot{};double tx{},ty{},tz{},th{};std::string result{"NONE"},reason;
    double ax{},ay{},az{},ah{},error{-1},distance{};bool loading{};std::uint64_t requestNs{},executeNs{},settleNs{};
    // Flag ownership / completion
    bool ownsFlag{},flagCleared{},worldLoaded{},engineFlow{};double hError{-1};std::uint64_t loadedNs{};unsigned loadPolls{};
    bool hasView{};double viewTarget{};bool viewRequested{};
};
struct State {
    std::mutex mutex;
    std::string directory,file;bool loaded{},fileOk{true};
    std::vector<Spot> spots;
    EngineApi api{};
    Request pending{};bool hasPending{};
    Last last{};
    std::string lastProperty,lastSaveProperty,lastViewProperty;bool propertyBaseline{};
    std::string lastGfxProperty,lastGfxResult;
    std::string engineState{"NOT_CONFIGURED"};
    std::uint64_t mainThread{};
};
inline State state;
inline std::atomic<bool> engineReady{},hookInstalled{};
inline std::atomic<std::uint64_t> qualifiedCalls{},otherCalls{},exceptions{};
inline std::atomic<unsigned> requests{},done{},failures{},saves{},saveFailures{},spotCount{};
// player sample (main thread writes, any thread reads; torn reads only affect the report/menu text)
inline std::atomic<bool> playerValid{};inline std::atomic<float> playerX{},playerY{},playerZ{},playerHeading{};
inline std::atomic<int> playerFoot{-1},gameState{-1},teleportingFlag{-1};
inline std::atomic<std::uint64_t> playerSamples{},playerSampleNs{};
inline std::atomic<const char*> playerReason{"NO SAMPLE"};
// menu (XR thread) -> main thread
inline std::atomic<int> menuCursor{0};              // 0 = SAVE SPOT, k = SPOT k
inline std::atomic<int> menuSaveRequest{0},menuTeleportRequest{0};
// CHEATS > TELEPORT TO (user: "make a cheat teleport to Blackwater"): fixed town spots, stick picks, A goes.
// Coordinates from public RDR1 trainers (connor-ms/RDR-Trainer src/mods/teleport.c; Armadillo matches our saloon spot, same world space).
struct Town {const char* name;double x,y,z;};
inline constexpr Town towns[]{{"BLACKWATER",711.18,78.31,1252.763},{"ARMADILLO",-2175.62,16.31,2613.50},{"ARMADILLO SALOON",-2186.040,16.409,2608.610},
    {"THIEVES LANDING",111.55,73.29,2318.82},{"BEECHERS HOPE",-83.45,117.68,1374.10},{"MACFARLANES RANCH",-887.08,90.19,2420.53},
    {"MANZANITA POST",-428.16,151.34,1615.59},{"TUMBLEWEED",-4007.25,28.46,2935.45},{"PLAINVIEW",-3126.40,43.57,3724.13},
    {"FORT MERCER",-2622.53,68.08,3390.51},{"CHUPAROSA",-2714.70,32.37,4251.90},{"ESCALERA",-4279.04,18.07,4447.64},
    {"CASA MADRUGADA",-788.78,13.04,3729.81},{"EL PRESIDIO",-698.10,63.25,3323.25},{"TORQUEMADA",376.66,76.30,3459.57}};
inline constexpr int townCount=int(sizeof towns/sizeof towns[0]);
inline std::atomic<int> cheatTown{0},cheatTownRequest{0}; // request = index + 1
inline std::atomic<Feedback> feedback{Feedback::None};inline std::atomic<int> feedbackSpot{0};
inline std::atomic<const char*> feedbackReason{""};
// Rendered VIEW yaw. The view = engine game camera (camGlueCMXGame eulers, the snap-turn setter) x recentred HMD pose
// (CameraAdapter: plan.world = baseWorld x inverseYaw x eye). The input thread (GameXr mapHorizontalLook, the snap-turn owner)
// publishes the current world view yaw/pitch and executes view requests by turning the engine camera in closed loop.
// Convention: forward f = -column 2 of plan.world[0]; yaw = atan2(f.x, f.z) deg, pitch = asin(f.y) deg.
inline std::atomic<bool> viewValid{};inline std::atomic<float> viewYaw{},viewPitch{};inline std::atomic<std::uint64_t> viewNs{};
inline std::atomic<unsigned> viewRequestSeq{};inline std::atomic<float> viewRequestYaw{};
inline std::atomic<const char*> viewState{"IDLE"};inline std::atomic<float> viewAfterYaw{},viewErrorDeg{-1.f};
inline std::atomic<unsigned> viewSteps{},viewRequests{},viewDone{},viewFailures{},viewSignFlips{};
inline std::atomic<bool> viewAvailable{}; // snap-turn path configured
// Plan head-sample stamp of the last published view, and the main-thread turn executor registered by GameXr.
inline std::atomic<std::uint64_t> viewHeadNs{};
using ViewTurnFn=int(*)(float radians,std::uint64_t& turnNs) noexcept;
inline std::atomic<ViewTurnFn> viewTurn{};
inline std::atomic<unsigned> viewWaits{},viewDeclines{};
inline float wrapDegrees(float d) noexcept {d=std::fmod(d+180.f,360.f);if(d<0)d+=360.f;return d-180.f;}
inline void publishView(float yawDeg,float pitchDeg,std::uint64_t now,std::uint64_t headNs=0) noexcept {
    if(!std::isfinite(yawDeg)||!std::isfinite(pitchDeg))return;
    viewHeadNs.store(headNs,std::memory_order_relaxed);
    viewYaw.store(yawDeg,std::memory_order_relaxed);viewPitch.store(pitchDeg,std::memory_order_relaxed);
    viewNs.store(now,std::memory_order_relaxed);viewValid.store(true,std::memory_order_release);}
inline void requestView(float yawDeg) noexcept {
    viewRequestYaw.store(wrapDegrees(yawDeg));viewRequests.fetch_add(1);viewState.store("PENDING");viewRequestSeq.fetch_add(1,std::memory_order_release);}
// Input-thread state machine. step() returns the camera yaw delta (radians) to apply now, 0 = nothing. The sign between the
// engine euler yaw and the measured view yaw is learnt from the first applied step (flipped if the view moved the other way).
struct ViewController {
    unsigned seq{};float target{},sign{1},lastApplied{},yawBefore{};unsigned steps{};bool active{};std::uint64_t start{};
    float step(float current,std::uint64_t now) noexcept {
        const unsigned want=viewRequestSeq.load(std::memory_order_acquire);
        if(want!=seq){seq=want;target=viewRequestYaw.load();active=true;steps=0;lastApplied=0;start=now;viewState.store("TURNING");}
        if(!active)return 0;
        if(lastApplied!=0){const float observed=wrapDegrees(current-yawBefore);
            if(std::fabs(observed)>0.3f*std::fabs(lastApplied)&&observed*lastApplied<0){sign=-sign;viewSignFlips.fetch_add(1);}
            lastApplied=0;}
        const float err=wrapDegrees(target-current);
        if(std::fabs(err)<1.f){active=false;viewAfterYaw.store(current);viewErrorDeg.store(std::fabs(err));viewDone.fetch_add(1);viewState.store("DONE");return 0;}
        if(steps>=12||now-start>20000000000ull){active=false;viewAfterYaw.store(current);viewErrorDeg.store(std::fabs(err));viewFailures.fetch_add(1);viewState.store("FAILED");return 0;}
        const float d=std::fmax(-80.f,std::fmin(80.f,err));
        yawBefore=current;lastApplied=d;++steps;viewSteps.fetch_add(1);
        return sign*d*0.017453292519943295f;
    }
    void notApplied() noexcept {lastApplied=0;if(steps)--steps;}
    std::uint64_t lastTurnNs{}; // diagnostics::monotonicNs stamp of the last applied native turn
};
inline ViewController viewController; // Main thread only (gameFrame)
// One closed-loop step per frame on the main thread; measures only views rendered from a head sample taken after the
// last turn (otherwise the in-flight frame would be read as "turn had no effect").
inline void viewFrame(std::uint64_t now) noexcept {
    const auto turn=viewTurn.load(std::memory_order_acquire);if(!turn)return;
    if(!viewValid.load(std::memory_order_acquire))return;
    const auto head=viewHeadNs.load(std::memory_order_relaxed);
    if(viewController.lastTurnNs&&head<=viewController.lastTurnNs&&now-viewController.lastTurnNs<1000000000ull){ // un-anchored plans: proceed after 1 s
        if(viewController.active)viewWaits.fetch_add(1,std::memory_order_relaxed);return;}
    const float d=viewController.step(viewYaw.load(std::memory_order_relaxed),now);
    if(!d)return;
    std::uint64_t turnNs{};const int r=turn(d,turnNs);
    if(r==1)viewController.lastTurnNs=turnNs;
    else{viewController.notApplied();if(r<0)viewDeclines.fetch_add(1,std::memory_order_relaxed);}
}

inline std::uint64_t monotonicNs() noexcept {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline bool finiteCoord(double v) noexcept {return std::isfinite(v)&&std::fabs(v)<100000.0;}

// ---- spots file (call with state.mutex held) ----
inline void loadSpotsLocked() {
    state.spots.clear();state.loaded=true;
    if(state.file.empty())return;
    std::FILE* f=std::fopen(state.file.c_str(),"r");
    if(!f){spotCount.store(0);return;}
    char line[256];
    while(std::fgets(line,sizeof line,f)){
        if(line[0]=='#'||line[0]=='\n'||line[0]=='\r')continue;
        int n{};double x{},y{},z{},h{};char label[96]{};
        const int got=std::sscanf(line,"%d %lf %lf %lf %lf %95s",&n,&x,&y,&z,&h,label);
        if(got<5||!finiteCoord(x)||!finiteCoord(y)||!finiteCoord(z)||!std::isfinite(h))continue;
        Spot spot{x,y,z,h,got>=6?std::string(label):std::string()};
        if(const char* v=std::strstr(line,"view=")){double vy{};if(std::sscanf(v+5,"%lf",&vy)==1&&std::isfinite(vy)){spot.hasView=true;spot.viewYaw=vy;}}
        if(const char* v=std::strstr(line,"pitch=")){double vp{};if(std::sscanf(v+6,"%lf",&vp)==1&&std::isfinite(vp))spot.viewPitch=vp;}
        state.spots.push_back(spot);
    }
    std::fclose(f);spotCount.store(unsigned(state.spots.size()));
}
inline bool appendSpotLocked(const Spot& s) {
    if(state.file.empty())return false;
    std::FILE* f=std::fopen(state.file.c_str(),"a");if(!f)return false;
    const int n=s.hasView?std::fprintf(f,"%u %.3f %.3f %.3f %.2f %s view=%.2f pitch=%.2f\n",unsigned(state.spots.size()+1),s.x,s.y,s.z,s.heading,s.label.c_str(),s.viewYaw,s.viewPitch)
                         :std::fprintf(f,"%u %.3f %.3f %.3f %.2f %s\n",unsigned(state.spots.size()+1),s.x,s.y,s.z,s.heading,s.label.c_str());
    const bool ok=n>0&&std::fflush(f)==0;std::fclose(f);
    if(ok){state.spots.push_back(s);spotCount.store(unsigned(state.spots.size()));}
    return ok;
}
// Called once at startup with <files>/rdrvr (the graphics.cfg directory).
inline void configureDirectory(const std::string& directory) noexcept {
    try{std::lock_guard lock(state.mutex);state.directory=directory;state.file=directory+"/teleport-spots.txt";loadSpotsLocked();}catch(...){}
}

// ---- menu (XR thread) ----
inline void menuInput(int adjust,bool activate) noexcept {
    const int n=int(spotCount.load(std::memory_order_relaxed));
    int cursor=menuCursor.load(std::memory_order_relaxed);if(cursor>n)cursor=n;if(cursor<0)cursor=0;
    if(adjust){cursor+=adjust;if(cursor<0)cursor=0;if(cursor>n)cursor=n;menuCursor.store(cursor);return;}
    if(!activate)return;
    if(!engineReady.load(std::memory_order_acquire))return;
    if(cursor==0){menuSaveRequest.store(1);feedback.store(Feedback::SavePending);feedbackSpot.store(0);}
    else{menuTeleportRequest.store(cursor);feedback.store(Feedback::TpPending);feedbackSpot.store(cursor);}
}
// Panel glyphs: A-Z 0-9 space . : / - % ? < > only.
inline void menuText(char* out,std::size_t size) noexcept {
    if(!out||!size)return;
    const int n=int(spotCount.load(std::memory_order_relaxed));
    int cursor=menuCursor.load(std::memory_order_relaxed);if(cursor>n)cursor=n;if(cursor<0)cursor=0;
    const auto fb=feedback.load(std::memory_order_relaxed);const int fs=feedbackSpot.load(std::memory_order_relaxed);
    const char* reason=feedbackReason.load(std::memory_order_relaxed);
    const char* unavailable=engineReady.load(std::memory_order_relaxed)?"":" - UNAVAILABLE";
    if(cursor==0){
        if(fb==Feedback::SavePending)std::snprintf(out,size,"SAVE SPOT - SAVING");
        else if(fb==Feedback::Saved)std::snprintf(out,size,"SAVE SPOT - SAVED SPOT %d",fs);
        else if(fb==Feedback::SaveFailed)std::snprintf(out,size,"SAVE FAILED: %s",reason?reason:"");
        else if(n)std::snprintf(out,size,"SAVE SPOT - %d SAVED%s",n,unavailable);
        else std::snprintf(out,size,"SAVE SPOT%s",unavailable);
        return;
    }
    const bool mine=fs==cursor;
    if(mine&&fb==Feedback::TpPending)std::snprintf(out,size,"SPOT %d/%d - PENDING",cursor,n);
    else if(mine&&fb==Feedback::TpLoading)std::snprintf(out,size,"SPOT %d/%d - LOADING",cursor,n);
    else if(mine&&fb==Feedback::TpDone)std::snprintf(out,size,"SPOT %d/%d - DONE",cursor,n);
    else if(mine&&fb==Feedback::TpFailed)std::snprintf(out,size,"SPOT %d: %s",cursor,reason?reason:"FAILED");
    else std::snprintf(out,size,"SPOT %d/%d%s",cursor,n,unavailable);
}

// ---- engine (main thread) ----
#if defined(__ANDROID__)
inline void configureEngine(const EngineApi& api) noexcept {
    try{std::lock_guard lock(state.mutex);state.api=api;state.engineState="WAITING_FOR_MAIN_THREAD";}catch(...){}
    engineReady.store(api.read&&api.teleport&&api.playerManager&&api.actorManagerGlobal&&api.actorSlotsGlobal&&api.gameData&&api.playerTeleporting&&api.streamer&&api.worldLoaded,std::memory_order_release);
}
inline void engineUnavailable(const char* why) noexcept {
    engineReady.store(false,std::memory_order_release);
    try{std::lock_guard lock(state.mutex);state.engineState=why;}catch(...){}
}
inline void markHookInstalled() noexcept {hookInstalled.store(true,std::memory_order_release);}

namespace detail {
struct Player {std::uintptr_t player{},actor{};std::uint32_t guid{};float x{},y{},z{},heading{};std::uint32_t foot{};int gameState{-1};int teleporting{-1};};
template<class T> inline bool get(const EngineApi& a,std::uintptr_t p,std::size_t o,T& out) noexcept {
    return a.read&&p&&p<=UINTPTR_MAX-o&&p+o<=UINTPTR_MAX-sizeof(T)&&a.read(nullptr,p+o,&out,sizeof(T));}
// Returns nullptr on success, else the refusal reason. 'strict' = the teleport gate (locks / foot / alive).
inline const char* samplePlayer(const EngineApi& a,Player& p,bool strict) noexcept {
    std::uint32_t gs{};std::uint8_t tele{};
    if(!get(a,a.gameData,0,gs))return "NO GAMEDATA";
    p.gameState=int(gs);
    if(get(a,a.playerTeleporting,0,tele))p.teleporting=tele;
    std::uintptr_t table{},manager{},slots{};std::uint16_t count{},generation{};std::uint8_t active{};
    if(!get(a,a.playerManager,0,table)||table!=a.playerManagerTable||!get(a,a.playerManager,0xa20,p.player)||!p.player||
       !get(a,p.player,0,table)||table!=a.playerTable||!get(a,p.player,0x5f1,active)||active!=1||!get(a,p.player,0x5f4,p.guid))return "NO PLAYER";
    if(!get(a,a.actorManagerGlobal,0,manager)||!get(a,a.actorSlotsGlobal,0,slots)||!slots||slots%8||!get(a,manager,0,count))return "NO ACTOR REGISTRY";
    const std::uintptr_t index=p.guid&0xffffu;
    if(index>=count||!get(a,slots,index*16,p.actor)||!p.actor||!get(a,slots,index*16+8,generation)||generation!=(p.guid>>16))return "ACTOR GENERATION";
    std::uintptr_t component{},matrix{};float m[16]{};
    if(!get(a,p.actor,0xb0,component)||!get(a,component,0x18,matrix)||!matrix||!get(a,matrix,0,m))return "NO MATRIX";
    for(float v:{m[0],m[2],m[8],m[10],m[12],m[13],m[14]})if(!std::isfinite(v))return "BAD MATRIX";
    p.x=m[12];p.y=m[13];p.z=m[14];p.heading=float(std::atan2(double(m[8]),double(m[10]))*57.29577951308232);
    if(!get(a,p.player,0x5d4,p.foot))return "NO PLAYER STATE";
    if(!strict)return nullptr;
    std::uint32_t flags{};std::uint16_t locks{};std::uint8_t lock1{},lock2{};std::uintptr_t health{};float hp{};
    if(gs!=3)return "NOT IN GAME";
    if(tele)return "TELEPORT IN PROGRESS";
    if(!get(a,p.actor,0x118,flags)||(flags&3u)!=3u)return "NOT PLAYER ACTOR";
    if(!get(a,p.actor,0x60,health)||!get(a,health,0x20,hp)||!std::isfinite(hp)||hp<=0)return "NOT ALIVE";
    if(!get(a,p.player,0x104,locks)||!get(a,p.player,0x10c,lock1)||!get(a,p.player,0x10d,lock2))return "NO LOCKS";
    if(locks||lock1||lock2)return "CONTROL LOCKED";
    if(p.foot!=2)return "NOT ON FOOT";
    return nullptr;
}
inline void publish(const Player& p,const char* reason,std::uint64_t now) noexcept {
    gameState.store(p.gameState,std::memory_order_relaxed);teleportingFlag.store(p.teleporting,std::memory_order_relaxed);
    if(reason){playerValid.store(false,std::memory_order_relaxed);playerReason.store(reason,std::memory_order_relaxed);return;}
    playerX.store(p.x,std::memory_order_relaxed);playerY.store(p.y,std::memory_order_relaxed);playerZ.store(p.z,std::memory_order_relaxed);
    playerHeading.store(p.heading,std::memory_order_relaxed);playerFoot.store(int(p.foot),std::memory_order_relaxed);
    playerSampleNs.store(now,std::memory_order_relaxed);playerValid.store(true,std::memory_order_release);playerReason.store("OK",std::memory_order_relaxed);
    playerSamples.fetch_add(1,std::memory_order_relaxed);
}
inline std::string propertyValue(const char* name) {
    char value[PROP_VALUE_MAX]{};const int saved=errno;const int n=__system_property_get(name,value);errno=saved;
    return n>0?std::string(value,std::size_t(n)):std::string();
}
// "<n>[:tag]" or "x,y,z[,heading][:tag]"
inline bool parseRequest(const std::string& raw,Request& r) {
    std::string v=raw.substr(0,raw.find(':'));
    if(v.empty())return false;
    if(v.find(',')!=std::string::npos){
        double c[5]{};int k=0;const char* s=v.c_str();
        while(k<5&&*s){char* end{};c[k]=std::strtod(s,&end);if(end==s)return false;++k;s=end;if(*s==',')++s;else break;}
        if(k<3||*s)return false;
        r.kind=2;r.x=c[0];r.y=c[1];r.z=c[2];r.hasHeading=k>=4;r.heading=k>=4?c[3]:0;r.hasView=k==5;r.viewYaw=k==5?c[4]:0;
        if(r.hasView&&!std::isfinite(r.viewYaw))return false;
        return finiteCoord(r.x)&&finiteCoord(r.y)&&finiteCoord(r.z)&&std::isfinite(r.heading);
    }
    char* end{};const long n=std::strtol(v.c_str(),&end,10);
    if(*end||n<1||n>100000)return false;
    r.kind=1;r.spot=int(n);return true;
}
inline void fail(const char* reason,int spot) {
    failures.fetch_add(1);feedbackReason.store(reason);feedbackSpot.store(spot);feedback.store(Feedback::TpFailed);
    state.last.result="FAILED";state.last.reason=reason;
}
} // namespace detail

// Main thread, once per sagShellDerived::Execute iteration (qualified = the call from Execute).
inline void gameFrame(bool qualified) noexcept {
    if(!qualified){otherCalls.fetch_add(1,std::memory_order_relaxed);return;}
    const auto calls=qualifiedCalls.fetch_add(1,std::memory_order_relaxed)+1;
    if(!engineReady.load(std::memory_order_acquire))return;
    try{
        using namespace detail;
        const auto now=monotonicNs();
        viewFrame(now); // View restore needs no controller
        std::unique_lock lock(state.mutex,std::try_to_lock);
        if(!lock.owns_lock())return; // report/menu copy in progress: next frame
        auto& s=state;const auto& a=s.api;
        if(!s.mainThread){s.mainThread=std::uint64_t(gettid());s.engineState="MAIN_THREAD";}
        // 1. properties, once per second
        static std::uint64_t nextPoll=0;
        if(now>=nextPoll){
            nextPoll=now+1'000'000'000ull;
            const auto tp=propertyValue("debug.rdrvr.teleport"),save=propertyValue("debug.rdrvr.teleport.save"),view=propertyValue("debug.rdrvr.teleport.view"),gfx=propertyValue("debug.rdrvr.gfx");
            if(!s.propertyBaseline){s.propertyBaseline=true;s.lastProperty=tp;s.lastSaveProperty=save;s.lastViewProperty=view;s.lastGfxProperty=gfx;}
            else{
                if(view!=s.lastViewProperty){s.lastViewProperty=view; // Rotate the view only, "<yawDeg>[:tag]"
                    const std::string v=view.substr(0,view.find(':'));char* end{};const double yaw=v.empty()?0:std::strtod(v.c_str(),&end);
                    if(!v.empty()&&end&&!*end&&std::isfinite(yaw))requestView(float(yaw));}
                if(tp!=s.lastProperty){s.lastProperty=tp;Request r{};
                    if(!tp.empty()&&tp!="0"){
                        requests.fetch_add(1);
                        if(parseRequest(tp,r)){r.source="PROP";r.ns=now;s.pending=r;s.hasPending=true;feedbackSpot.store(r.kind==1?r.spot:0);feedback.store(Feedback::TpPending);}
                        else{s.last=Last{};s.last.valid=true;s.last.source="PROP";s.last.requestNs=now;fail("BAD PROPERTY",0);}
                    }
                }
                if(gfx!=s.lastGfxProperty){s.lastGfxProperty=gfx;if(!gfx.empty()&&gfx!="0")gfxprop::apply(gfx,s.lastGfxResult);}
                if(save!=s.lastSaveProperty){s.lastSaveProperty=save;if(!save.empty()&&save!="0")menuSaveRequest.store(2);}
            }
        }
        // 2. menu requests
        if(const int k=menuTeleportRequest.exchange(0)){requests.fetch_add(1);Request r{};r.kind=1;r.spot=k;r.source="MENU";r.ns=now;s.pending=r;s.hasPending=true;}
        if(const int t=cheatTownRequest.exchange(0);t>=1&&t<=townCount){
            requests.fetch_add(1);const auto& w=towns[t-1];Request r{};r.kind=2;r.x=w.x;r.y=w.y;r.z=w.z;r.source="CHEAT";r.ns=now;s.pending=r;s.hasPending=true;
            feedbackSpot.store(0);feedback.store(Feedback::TpPending);}
        const int saveRequest=menuSaveRequest.exchange(0);
        // 3. sample (every 4th frame, every frame while a request/settle is active)
        const bool busy=s.hasPending||saveRequest||(s.last.valid&&s.last.result=="EXECUTED");
        if(!busy&&(calls&3u))return;
        Player p{};const char* reason=samplePlayer(a,p,false);publish(p,reason,now);
        // 4. save
        if(saveRequest){
            if(reason){saveFailures.fetch_add(1);feedbackReason.store(reason);feedback.store(Feedback::SaveFailed);}
            else{
                char label[48];std::snprintf(label,sizeof label,"%s-%lld",saveRequest==2?"PROP":"MENU",static_cast<long long>(std::time(nullptr)));
                Spot spot{p.x,p.y,p.z,p.heading,label};
                if(viewValid.load(std::memory_order_acquire)&&now-viewNs.load()<2000000000ull){spot.hasView=true;spot.viewYaw=viewYaw.load();spot.viewPitch=viewPitch.load();}
                if(appendSpotLocked(spot)){saves.fetch_add(1);s.fileOk=true;feedbackSpot.store(int(s.spots.size()));feedback.store(Feedback::Saved);}
                else{s.fileOk=false;saveFailures.fetch_add(1);feedbackReason.store("FILE");feedback.store(Feedback::SaveFailed);}
            }
        }
        // 5. Settle tracking of the last executed teleport
        if(s.last.valid&&s.last.result=="EXECUTED"){
            auto& l=s.last;
            if(p.teleporting==1&&!l.loading){l.loading=true;feedback.store(Feedback::TpLoading);}
            if(p.gameState==5)l.engineFlow=true; // a fade let the engine take over: it clears the flag itself
            const auto age=now-l.executeNs;
            // The flag the mod caused, no engine load state: poll the engine's own "destination streamed" query, then clear it.
            if(l.ownsFlag&&!l.flagCleared&&p.teleporting==1&&p.gameState==3){
                std::uintptr_t streamer{};bool loaded=false;
                if(get(a,a.streamer,0,streamer)&&streamer){++l.loadPolls;loaded=a.worldLoaded(reinterpret_cast<void*>(streamer),worldLoadedPriority);}
                if(loaded||age>60'000'000'000ull){
                    *reinterpret_cast<volatile std::uint8_t*>(a.playerTeleporting)=0; // main thread, same as UpdateGameState's clear
                    l.flagCleared=true;l.worldLoaded=loaded;l.loadedNs=now;p.teleporting=0;
                }
            }
            if(!reason){const double hx=p.x-l.tx,hz=p.z-l.tz;l.hError=std::sqrt(hx*hx+hz*hz);}
            const bool arrived=!reason&&l.hError>=0&&l.hError<2.0;   // ground snap changes Y by ~1.5 m: horizontal check
            const bool idle=p.teleporting==0&&p.gameState==3&&!reason;
            const bool timeout=age>120'000'000'000ull;
            const auto finish=[&](const char* failure){
                l.settleNs=now;l.ax=p.x;l.ay=p.y;l.az=p.z;l.ah=p.heading;
                const double dx=p.x-l.tx,dy=p.y-l.ty,dz=p.z-l.tz;l.error=std::sqrt(dx*dx+dy*dy+dz*dz);
                if(failure){l.result="FAILED";l.reason=failure;failures.fetch_add(1);feedbackReason.store(failure);feedback.store(Feedback::TpFailed);}
                else{l.result="DONE";if(l.flagCleared&&!l.worldLoaded)l.reason="LOAD TIMEOUT";done.fetch_add(1);feedback.store(Feedback::TpDone);
                    if(l.hasView){requestView(float(l.viewTarget));l.viewRequested=true;}} // Then restore the rendered view
            };
            if(idle&&arrived)finish(nullptr);
            else if(idle&&age>3'000'000'000ull)finish("NOT AT TARGET");
            else if(timeout){
                if(l.ownsFlag&&!l.flagCleared&&p.gameState!=5){*reinterpret_cast<volatile std::uint8_t*>(a.playerTeleporting)=0;l.flagCleared=true;}
                finish(reason?reason:"SETTLE TIMEOUT");
            }
        }
        // 6. pending teleport
        if(!s.hasPending)return;
        Request r=s.pending;
        if(r.kind==1&&r.spot>int(s.spots.size()))loadSpotsLocked(); // a pushed/edited spots file
        if(r.kind==1&&(r.spot<1||r.spot>int(s.spots.size()))){s.hasPending=false;s.last=Last{};s.last.valid=true;s.last.source=r.source;s.last.spot=r.spot;s.last.requestNs=r.ns;fail("NO SUCH SPOT",r.spot);return;}
        const char* gate=samplePlayer(a,p,true);
        if(gate){
            // transient states wait up to 15 s (loading, another teleport, menu/pause transitions); the rest refuse at once
            const bool transient=!std::strcmp(gate,"NOT IN GAME")||!std::strcmp(gate,"TELEPORT IN PROGRESS")||!std::strcmp(gate,"NO PLAYER")||
                (s.last.valid&&s.last.result=="EXECUTED");
            if(transient&&now-r.ns<15'000'000'000ull)return;
            s.hasPending=false;s.last=Last{};s.last.valid=true;s.last.source=r.source;s.last.spot=r.kind==1?r.spot:0;s.last.requestNs=r.ns;
            fail(gate,r.kind==1?r.spot:0);return;
        }
        if(s.last.valid&&s.last.result=="EXECUTED")return; // previous one still settling
        s.hasPending=false;
        Last l{};l.valid=true;l.source=r.source;l.spot=r.kind==1?r.spot:0;l.requestNs=r.ns;
        if(r.kind==1){const auto& spot=s.spots[std::size_t(r.spot-1)];l.tx=spot.x;l.ty=spot.y;l.tz=spot.z;l.th=spot.heading;l.hasView=spot.hasView;l.viewTarget=spot.viewYaw;}
        else{l.tx=r.x;l.ty=r.y;l.tz=r.z;l.th=r.hasHeading?r.heading:p.heading;l.hasView=r.hasView;l.viewTarget=r.viewYaw;}
        {const double dx=l.tx-p.x,dy=l.ty-p.y,dz=l.tz-p.z;l.distance=std::sqrt(dx*dx+dy*dy+dz*dz);}
        l.executeNs=now;l.result="EXECUTED";s.last=l;
        feedbackSpot.store(l.spot);feedback.store(Feedback::TpPending);
        a.teleport(p.guid,float(l.tx),float(l.ty),float(l.tz),float(l.th),false,false,false);
        {std::uint8_t flag{};if(get(a,a.playerTeleporting,0,flag)&&flag){s.last.ownsFlag=true;s.last.loading=true;feedback.store(Feedback::TpLoading);}} // gate saw 0 before
    }catch(...){exceptions.fetch_add(1);}
}
#else
inline void gameFrame(bool) noexcept {}
#endif

// ---- report (any thread) ----
inline std::string jsonSafe(const std::string& v) {std::string o;o.reserve(v.size());for(char c:v)o+=(c=='"'||c=='\\'||static_cast<unsigned char>(c)<0x20)?'?':c;return o;}
inline std::string report() {
    std::string out;out.reserve(2048);char b[1024];
    std::lock_guard lock(state.mutex);const auto& s=state;
    std::snprintf(b,sizeof b,"{\"ready\":%s,\"hookInstalled\":%s,\"engineState\":\"%s\",\"mainThread\":%llu,\"qualifiedCalls\":%llu,\"otherCalls\":%llu,\"exceptions\":%llu,",
        engineReady.load()?"true":"false",hookInstalled.load()?"true":"false",s.engineState.c_str(),(unsigned long long)s.mainThread,
        (unsigned long long)qualifiedCalls.load(),(unsigned long long)otherCalls.load(),(unsigned long long)exceptions.load());out+=b;
    const auto age=playerSampleNs.load()?(monotonicNs()-playerSampleNs.load())/1'000'000ull:0ull;
    std::snprintf(b,sizeof b,"\"player\":{\"valid\":%s,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"heading\":%.2f,\"footState\":%d,\"gameState\":%d,\"teleporting\":%d,\"samples\":%llu,\"ageMs\":%llu,\"reason\":\"%s\",",
        playerValid.load()?"true":"false",double(playerX.load()),double(playerY.load()),double(playerZ.load()),double(playerHeading.load()),playerFoot.load(),
        gameState.load(),teleportingFlag.load(),(unsigned long long)playerSamples.load(),(unsigned long long)age,playerReason.load());out+=b;
    {const auto vns=viewNs.load();std::snprintf(b,sizeof b,"\"viewValid\":%s,\"viewYaw\":%.2f,\"viewPitch\":%.2f,\"viewAgeMs\":%.0f},",
        viewValid.load()?"true":"false",double(viewYaw.load()),double(viewPitch.load()),vns?double(monotonicNs()-vns)/1e6:-1.0);out+=b;}
    std::snprintf(b,sizeof b,"\"file\":\"%s\",\"fileOk\":%s,\"spots\":[",jsonSafe(s.file).c_str(),s.fileOk?"true":"false");out+=b;
    for(std::size_t i=0;i<s.spots.size();++i){const auto& p=s.spots[i];
        std::snprintf(b,sizeof b,"%s{\"n\":%u,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"heading\":%.2f,\"label\":\"%s\"}",i?",":"",unsigned(i+1),p.x,p.y,p.z,p.heading,jsonSafe(p.label).c_str());out+=b;}
    const auto& l=s.last;const auto ms=[](std::uint64_t a,std::uint64_t z){return a&&z>=a?double(z-a)/1e6:-1.0;};
    std::snprintf(b,sizeof b,"],\"lastTeleport\":{\"valid\":%s,\"source\":\"%s\",\"spot\":%d,\"target\":[%.3f,%.3f,%.3f,%.2f],\"result\":\"%s\",\"reason\":\"%s\",",
        l.valid?"true":"false",jsonSafe(l.source).c_str(),l.spot,l.tx,l.ty,l.tz,l.th,jsonSafe(l.result).c_str(),jsonSafe(l.reason).c_str());out+=b;
    std::snprintf(b,sizeof b,"\"distance\":%.2f,\"loading\":%s,\"after\":[%.3f,%.3f,%.3f,%.2f],\"errorM\":%.3f,\"queueMs\":%.1f,\"settleMs\":%.1f,\"ageS\":%.1f,",
        l.distance,l.loading?"true":"false",l.ax,l.ay,l.az,l.ah,l.error,ms(l.requestNs,l.executeNs),ms(l.executeNs,l.settleNs),
        l.executeNs?double(monotonicNs()-l.executeNs)/1e9:-1.0);out+=b;
    std::snprintf(b,sizeof b,"\"hasView\":%s,\"viewTarget\":%.2f,\"viewRequested\":%s,\"viewState\":\"%s\",\"viewAfter\":%.2f,\"viewErrorDeg\":%.2f,",
        l.hasView?"true":"false",l.viewTarget,l.viewRequested?"true":"false",l.viewRequested?viewState.load():"NONE",
        l.viewRequested?double(viewAfterYaw.load()):0.0,l.viewRequested?double(viewErrorDeg.load()):-1.0);out+=b;
    std::snprintf(b,sizeof b,"\"horizontalErrorM\":%.3f,\"ownsFlag\":%s,\"flagCleared\":%s,\"worldLoaded\":%s,\"engineFlow\":%s,\"loadPolls\":%u,\"loadWaitMs\":%.1f},",
        l.hError,l.ownsFlag?"true":"false",l.flagCleared?"true":"false",l.worldLoaded?"true":"false",l.engineFlow?"true":"false",l.loadPolls,ms(l.executeNs,l.loadedNs));out+=b;
    std::snprintf(b,sizeof b,"\"view\":{\"available\":%s,\"state\":\"%s\",\"target\":%.2f,\"after\":%.2f,\"errorDeg\":%.2f,\"requests\":%u,\"done\":%u,\"failures\":%u,\"steps\":%u,\"signFlips\":%u,\"waits\":%u,\"declines\":%u,\"executor\":%s,\"property\":\"%s\"},\"pending\":%s,\"requests\":%u,\"done\":%u,\"failures\":%u,\"saves\":%u,\"saveFailures\":%u,\"property\":\"%s\",\"saveProperty\":\"%s\",",
        viewAvailable.load()?"true":"false",viewState.load(),double(viewRequestYaw.load()),double(viewAfterYaw.load()),double(viewErrorDeg.load()),
        viewRequests.load(),viewDone.load(),viewFailures.load(),viewSteps.load(),viewSignFlips.load(),viewWaits.load(),viewDeclines.load(),viewTurn.load()?"true":"false",jsonSafe(s.lastViewProperty).c_str(),
        s.hasPending?"true":"false",requests.load(),done.load(),failures.load(),saves.load(),saveFailures.load(),jsonSafe(s.lastProperty).c_str(),jsonSafe(s.lastSaveProperty).c_str());out+=b;
    std::snprintf(b,sizeof b,"\"gfxProperty\":{\"value\":\"%s\",\"applies\":%u,\"applied\":%u,\"rejected\":%u,\"last\":\"%s\"}}",jsonSafe(s.lastGfxProperty).c_str(),
        gfxprop::applies.load(),gfxprop::appliedPairs.load(),gfxprop::rejectedPairs.load(),jsonSafe(s.lastGfxResult).c_str());out+=b;
    return out;
}
} // namespace rdrvr::teleport
