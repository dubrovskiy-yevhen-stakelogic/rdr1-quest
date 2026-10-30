#pragma once
// REIN ROPE: the reins are grabbed with the VR hands and the arms follow the real hands. On a horse with REINS on
// both IK arms follow their controllers (VrBody), so the game's rein rope has to come to the hand(s) that hold the reins instead of the
// hands going to the native riding pose. Engine facts (librdr 1.58 variant 1, disassembly):
//  - sagDynObjLODManager = [[GOT 0x2e01ee8]] (GLOB_DAT sagSingleton<sagDynObjLODManager>::sm_Instance 0x4e75ebd8, GetInstance 0x2899444).
//    UpdateConstraintsEnd 0x289a798: the objects = 8-byte pointers at +0x10, count s32 at +0x810 (<= 256), active byte obj+0xa1, vt+0xc0.
//  - sagVerletRein vtable 0x2db8300 (address point +0x10): +0x8 typeinfo 0x2db8498, slot +0xc0 (0x2db83d0) UpdateConstraints 0x28a1ea8,
//    slot +0xd0 (0x2db83e0) UpdatePostIntegrateConstraints 0x28a44d4. These four words are the variant proof (verifyEngine): any
//    mismatch (variant 2 = shifted .text / .data) -> UNAVAILABLE, nothing is read or written.
//  - rein: +0xd0 phVerletCloth*, +0x138 holder crSkeleton*, +0x160 holder sagGuid (u16 index, u16 generation = the player's u32 guid),
//    +0x17c / +0x180 wrist_l / wrist_r bone index, +0x190 ReinAttachment (0 NONE, 1 LEFT, 2 RIGHT, 3 BOTH, 4 RELEASE), +0x1c0..+0x1cc the
//    held vertex indices, +0x210 / +0x220 / +0x230 / +0x240 the held-vertex world targets.
//  - UpdateConstraints (main thread, sagShellDerived::Update 0x23a355c, AFTER the post-physics pass = hook A) recomputes the targets
//    from the holder's LIVE globals (crSkeleton+0x28): 1 = wrist_l x [0x4e808620] / [..630] -> +0x210 / +0x220 (copied to +0x230 / +0x240),
//    2 = wrist_r x [..650] / [..640], 3 = wrist_r -> +0x230 / +0x210 and wrist_l -> +0x220 / +0x240 (0x28a2040, 0x28a26a4, 0x28a26e4);
//    0 (or no valid holder / wrist) = 0x28a206c: +0x210 / +0x220 from the HORSE (GetReinAttachmentPoints of its vehicle component, else
//    its bone +0x188) = the rope on the saddle. UpdatePostIntegrateConstraints 0x28a44d4 (verlet task) copies +0x210 / +0x220 into the
//    vertices +0x1c0 / +0x1c4 in EVERY mode, in mode 3 also +0x230 / +0x240 into +0x1c8 / +0x1cc. So the rope follows the IK'd wrists of
//    the same frame once the mode names the holding hand(s).
//  - pins: SetHolderBones 0x28a10f8 pins for the CURRENT +0x190: 3 = DynamicPin +0x1c8 / +0x1c0 / +0x1c4 / +0x1cc, else +0x1c0 / +0x1c4
//    pinned, +0x1c8 / +0x1cc unpinned (DynamicPinVertex 0x1ed4bc0: bit v of the words at [cloth+0x70] + the edge flags). It runs on EVERY
//    SetAttachToHolder 0x28a0ddc with a non-zero result (mode = keep && cur ? cur : new; mode 0 only unpins +0x1c8 / +0x1cc) and on every
//    successful SetHolder 0x28a0bc0 (event 0 at mount: aniRide::Mount / MountCompletion / UpdateRideMachine; the deferred holder +0x164).
//    OnEvent 0x28a063c: events 1 LEFT / 9 RIGHT / 2 BOTH = SetAttachToHolder(mode, keep 0) = what ReinAttachmentTask::AttachReinsToHands
//    0x22f4ca4 emits for the gait handedness 1 / 2 / 3 (aniReins::OnGaitTransition 0x22fd4d4 / 0x22fd1d0); event 5 = LEFT keep 1 only from
//    rdr2CutsceneAnimatedHorseEntity::AttachReinsToActor 0x2409b38; event 7 = NONE (dismount). So a gait change sets the gait's own mode
//    with matching pins (the mod re-applies its mode the next frame); a native BOTH, a SetHolder or a cutscene event 5 under the mod's 3
//    pins all four: BOTH is ADOPTED and stays until the next keep-0 event (one-hand wishes count stuckBoth meanwhile).
//    A pinned vertex nobody writes stays where it is in WORLD space (the horse rides away from it): 0 / 1 / 2 are written only while the
//    +0x1c8 / +0x1cc pin bits are clear, and a mod-written 0 / 1 / 2 that finds them pinned (a native BOTH racing the store) is repaired to
//    3, which drives all four. Mode 3 over a native 1 / 2 leaves +0x1c8 / +0x1cc unpinned (placed every frame after the integration).
// The mod writes ONLY the int at +0x190, from hook A (post-physics path, under VrBody's busy guard: the previous frame's verlet task was
// waited at 0x23a363c, this frame's constraints run after hook A), never calls the engine; every write follows this frame's identity
// checks (vtable, holder guid, holder skeleton, still listed by the manager) and is read back (another writer in between = native).
// Native events may set the mode too: last writer wins - a value the mod did not write is adopted as the new native value and the wanted
// mode is re-applied in the same hook. Native 0 (NONE) / 4 (RELEASE) = the rope is not in the hands: never touched.
// No VR hand on the reins -> the native value back; .reins.ropefree 1 = NONE instead (the rope on the saddle until a VR hand takes it; a
// mod-written NONE is never "restored" - a dismount's own NONE looks the same). A rein lost while overriding gets the native value back if
// it still reads as the player's with our value, else the override is remembered and resumed if the same rein returns with our value.
// Levers debug.rdrvr.vr.reins.rope: 0 never touch, 1 (default) LEFT / RIGHT / BOTH, 2 = no BOTH (two hands -> LEFT); .reins.ropefree.
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace rdrvr::vr::reinrope {
inline constexpr std::uintptr_t managerInstanceGotOffset=0x2e01ee8,managerInstanceOffset=0x4e75ebd8, // GLOB_DAT sm_Instance (.bss)
    reinVtableOffset=0x2db8300,reinTypeinfoSlotOffset=0x2db8308,reinTypeinfoOffset=0x2db8498,          // _ZTV / _ZTI13sagVerletRein
    setAttachToHolderOffset=0x28a0ddc,                                                                  // sagVerletRein::SetAttachToHolder
    reinConstraintsSlotOffset=0x2db83d0,reinConstraintsOffset=0x28a1ea8,                                // vt+0xc0 UpdateConstraints
    reinPostIntegrateSlotOffset=0x2db83e0,reinPostIntegrateOffset=0x28a44d4;                            // vt+0xd0 UpdatePostIntegrateConstraints
inline constexpr std::uintptr_t vtableAddressPoint=0x10;
namespace off {
inline constexpr std::size_t mgrObjects=0x10,mgrCount=0x810,objActive=0xa1;
inline constexpr std::size_t reinCloth=0xd0,reinHolderSkel=0x138,reinHolderGuid=0x160,reinWristL=0x17c,reinWristR=0x180,reinMode=0x190,
    reinHeld=0x1c0,reinTarget=0x210,reinBlock=0x250;
inline constexpr std::size_t clothPinBits=0x70,clothVertexCount=0x124;
} // namespace off
inline constexpr int maxObjects=256; // (0x810 - 0x10) / 8
inline constexpr std::uint64_t rescanNs=500'000'000ull,ghostNs=60'000'000'000ull; // a lost override is resumable for 60 s

using ReadFn=bool(*)(void*,std::uintptr_t,void*,std::size_t);
enum State : unsigned {StateOff,StateUnavailable,StateNotFound,StateFound};
inline constexpr std::array<const char*,4> stateNames{"OFF","UNAVAILABLE","NOT_FOUND","FOUND"};

struct Stats {
    std::atomic<std::uint64_t> steps{},scans{},scanObjects{},reinsSeen{},found{},lost{},readFails{},overrides{},restores{},writes{},
        nativeChanges{},adopted{},pinBlocked{},pinUnknown{},skippedNative{},farFrames{},bothFrames{},
        // one-hand / NONE wishes held in a pinned BOTH, pinned +0x1c8/+0x1cc under our 0/1/2 repaired to 3, a store another
        // writer replaced at once, reins lost while overriding (+ restored then / remembered / resumed), NONE overrides left as they are,
        // frames a held hand was dropped because its arm is not IK'd to the controller
        stuckBoth{},stuckFixes{},pinRepairs{},raceWrites{},lostOverriding{},lostRestored{},ghosts{},ghostResumed{},freeLeft{},heldMasked{};
    std::atomic<int> modeNative{-1},modeCurrent{-1},modeWritten{-1},held{},heldRaw{},active{-1};
    std::atomic<bool> overriding{},freeNone{};std::atomic<std::uintptr_t> rein{};
    std::array<std::atomic<float>,2> wristCm{},wristCmMax{}; // |+0x210 / +0x220 - the wrist that set them| (previous frame's IK'd wrist)
};
inline Stats stats{};
inline std::atomic<unsigned> state{StateUnavailable}; // until verifyEngine (VrBody install) passed
inline std::atomic<const char*> verifyWhy{"NOT VERIFIED"};
inline std::atomic<bool> available{};
inline int policy=1; // debug.rdrvr.vr.reins.rope (VrBody startup)
inline bool fixStuck=true; // debug.rdrvr.vr.reins.ropefix (0 = wait for the game's event)
inline void bump(std::atomic<std::uint64_t>& c,std::uint64_t n=1) noexcept {c.fetch_add(n,std::memory_order_relaxed);}
inline bool overriding() noexcept {return stats.overriding.load(std::memory_order_relaxed);}

// Install (TraceEntry -> VrBody install): got(slot, target) = *(base+slot) == base+target.
template<class Got> inline bool verifyEngine(int lever,Got&& got) noexcept {
    policy=lever;
#ifdef __ANDROID__
    {char v[PROP_VALUE_MAX]{};fixStuck=!(__system_property_get("debug.rdrvr.vr.reins.ropefix",v)>0&&v[0]=='0');}
#endif
    if(lever<=0){state.store(StateOff,std::memory_order_relaxed);verifyWhy.store("PROPERTY",std::memory_order_relaxed);return false;}
    struct Word {std::uintptr_t slot,target;const char* name;};
    const Word words[]{{managerInstanceGotOffset,managerInstanceOffset,"GOT sagDynObjLODManager::sm_Instance"},
        {reinTypeinfoSlotOffset,reinTypeinfoOffset,"VTABLE sagVerletRein typeinfo"},
        {reinConstraintsSlotOffset,reinConstraintsOffset,"VTABLE sagVerletRein::UpdateConstraints"},
        {reinPostIntegrateSlotOffset,reinPostIntegrateOffset,"VTABLE sagVerletRein::UpdatePostIntegrateConstraints"}};
    for(const auto& w:words){bool ok=false;try{ok=got(w.slot,w.target);}catch(...){ok=false;}
        if(!ok){verifyWhy.store(w.name,std::memory_order_relaxed);state.store(StateUnavailable,std::memory_order_relaxed);return false;}}
    verifyWhy.store("OK",std::memory_order_relaxed);available.store(true,std::memory_order_release);state.store(StateNotFound,std::memory_order_relaxed);
    return true;
}

struct Input {
    ReadFn read{};std::uintptr_t base{};
    std::uint32_t guid{};std::uintptr_t skel{};const float* globals{};std::uint32_t bones{};float upm{1.f};
    bool want{};      // on a horse with REINS on and an arm following its controller
    unsigned held{};  // reins::holds of the hands whose arm is IK'd to the controller: bit 0 VR left, bit 1 VR right
    unsigned heldRaw{}; // reins::holds before that mask (report)
    bool freeNone{};  // .reins.ropefree: no VR hand on the reins -> NONE (the saddle) instead of the native value
    std::uint64_t now{};
};
struct Block {
    alignas(16) unsigned char b[off::reinBlock]{};
    template<class T> T at(std::size_t o) const noexcept {T v{};std::memcpy(&v,b+o,sizeof v);return v;}
    std::array<float,3> vec(std::size_t o) const noexcept {return {at<float>(o),at<float>(o+4),at<float>(o+8)};}
};

// One writer: the thread that runs the player's hook A (VrBody busy guard).
class Rope {
    std::uintptr_t rein_{};int index_{-1};std::uint64_t nextScanNs_{},lastFixNs_{};
    bool overriding_{};int native_{-1},written_{-1};
    bool haveWrist_{};int wristMode_{};std::array<std::array<float,3>,2> wrist_{}; // [0] wrist_r, [1] wrist_l after last frame's IK
    struct Ghost {std::uintptr_t rein{};int native{-1},written{-1};std::uint64_t ns{};} ghost_{}; // an override lost without its restore
    template<class T> static bool get(const Input& in,std::uintptr_t p,T& out) noexcept {
        return in.read&&p&&p<=UINTPTR_MAX-sizeof(T)&&in.read(nullptr,p,&out,sizeof(T));}
    static bool manager(const Input& in,std::uintptr_t& mgr,int& n) noexcept {
        std::int32_t c{};
        if(!get(in,in.base+managerInstanceOffset,mgr)||!mgr||!get(in,mgr+off::mgrCount,c)||c<0||c>maxObjects)return false;
        n=c;return true;}
    bool readRein(const Input& in,std::uintptr_t r,Block& b) const noexcept {return r&&r<=UINTPTR_MAX-off::reinBlock&&in.read(nullptr,r,b.b,sizeof b.b);}
    // the player's rein: exact sagVerletRein vtable, holder = the player (guid) and its crSkeleton, a sane mode
    static bool identity(const Input& in,const Block& b) noexcept {
        const int mode=b.at<std::int32_t>(off::reinMode);
        return b.at<std::uintptr_t>(0)==in.base+reinVtableOffset+vtableAddressPoint&&b.at<std::uint32_t>(off::reinHolderGuid)==in.guid&&
               b.at<std::uintptr_t>(off::reinHolderSkel)==in.skel&&in.skel&&mode>=0&&mode<=4&&b.at<std::uintptr_t>(off::reinCloth);}
    bool listed(const Input& in) const noexcept { // still owned by the manager (a freed rein is no longer in its list)
        std::uintptr_t mgr{},slot{};int n{};
        return index_>=0&&manager(in,mgr,n)&&index_<n&&get(in,mgr+off::mgrObjects+std::uintptr_t(index_)*8,slot)&&slot==rein_;}
    bool scan(const Input& in,Block& b) noexcept {
        bump(stats.scans);std::uintptr_t mgr{};int n{};if(!manager(in,mgr,n)){bump(stats.readFails);return false;}
        stats.scanObjects.store(std::uint64_t(n),std::memory_order_relaxed);if(!n)return false;
        std::array<std::uintptr_t,maxObjects> objects{};
        if(!in.read(nullptr,mgr+off::mgrObjects,objects.data(),std::size_t(n)*8)){bump(stats.readFails);return false;}
        const std::uintptr_t vt=in.base+reinVtableOffset+vtableAddressPoint;std::uint64_t reins=0;
        for(int i=0;i<n;++i){const auto o=objects[std::size_t(i)];std::uintptr_t v{};if(!o||!get(in,o,v)||v!=vt)continue;++reins;
            if(readRein(in,o,b)&&identity(in,b)){rein_=o;index_=i;stats.reinsSeen.store(reins,std::memory_order_relaxed);bump(stats.found);return true;}}
        stats.reinsSeen.store(reins,std::memory_order_relaxed);return false;
    }
    // pin bits of the two mode-3-only held vertices (+0x1c8 / +0x1cc): 0 unknown (read failed = unsafe), 1 both clear, 2 any set.
    // SetHeldVertices 0x28a1388 / SetDistanceLod 0x28a4318 put them at n/2-2 / n/2+1 and pin the rope ends 1 / n-2 to the
    // horse; a short rope LOD (n = 6 / 7) makes them the rope ends - always pinned, moved by the horse pin points: unknown (never a
    // single-hand write, never a repair to 3 that would pull the rope ends to the wrists)
    static int pins(const Input& in,const Block& b) noexcept {
        const auto cloth=b.at<std::uintptr_t>(off::reinCloth);std::uintptr_t bits{};std::int32_t n{};
        if(!get(in,cloth+off::clothPinBits,bits)||!bits||!get(in,cloth+off::clothVertexCount,n)||n<=0||n>4096)return 0;
        const int v2=b.at<std::int32_t>(off::reinHeld+8),v3=b.at<std::int32_t>(off::reinHeld+12);
        if(v2<2||v3<2||v2>n-3||v3>n-3)return 0;
        std::uint32_t w2{},w3{};
        if(!get(in,bits+std::uintptr_t(unsigned(v2)>>5u)*4,w2)||!get(in,bits+std::uintptr_t(unsigned(v3)>>5u)*4,w3))return 0;
        return ((w2>>(unsigned(v2)&31u))&1u)||((w3>>(unsigned(v3)&31u))&1u)?2:1;
    }
    void drop() noexcept {rein_=0;index_=-1;overriding_=false;haveWrist_=false;native_=written_=-1;stats.overriding.store(false,std::memory_order_relaxed);
        stats.rein.store(0,std::memory_order_relaxed);stats.modeNative.store(-1,std::memory_order_relaxed);stats.modeCurrent.store(-1,std::memory_order_relaxed);
        stats.modeWritten.store(-1,std::memory_order_relaxed);}
    static void store(std::uintptr_t r,int v) noexcept {*reinterpret_cast<volatile std::int32_t*>(r+off::reinMode)=v;bump(stats.writes);}
    // the store, then +0x190 read back - a native SetAttachToHolder on another thread between this frame's read and our
    // store (or right after it) wins: its value is the new native one (false); `after` = the mode the constraints will see
    bool write(const Input& in,int v,int& after) noexcept {
        store(rein_,v);std::int32_t back=v;
        if(get(in,rein_+off::reinMode,back)&&back!=v){native_=back;overriding_=false;written_=-1;after=back;bump(stats.raceWrites);return false;}
        after=v;return true;}
    // an overridden rein that failed this frame's checks (holder cleared at 0x28a0cc8 / 0x28a0d28, popped from the manager's
    // list 0x289a6e4 for its LOD limit, a failed read) and was not found again: the native value back if it still reads as the player's
    // rein holding OUR value (vtable, mode, holder = the player or cleared, the pin rule; listed() not required - a popped rein is alive),
    // else remembered for resume(). A mod-written NONE is never restored (see step).
    void lose(const Input& in,std::uintptr_t r) noexcept {
        if(!overriding_)return;
        bump(stats.lostOverriding);
        if(written_==0){bump(stats.freeLeft);return;}
        Block g{};
        if(readRein(in,r,g)&&g.at<std::uintptr_t>(0)==in.base+reinVtableOffset+vtableAddressPoint&&g.at<std::int32_t>(off::reinMode)==written_&&
           g.at<std::uintptr_t>(off::reinCloth)&&native_>=1&&native_<=3){
            const auto guid=g.at<std::uint32_t>(off::reinHolderGuid);
            if((guid==in.guid||(!guid&&!g.at<std::uintptr_t>(off::reinHolderSkel)))&&(native_==3||pins(in,g)==1)){
                store(r,native_);bump(stats.restores);bump(stats.lostRestored);return;}
        }
        ghost_=Ghost{r,native_,written_,in.now};bump(stats.ghosts);
    }
    // a rein a scan found (no state of its own): the lost override of the SAME rein resumes while it still holds our value (it would
    // otherwise read our last store as the game's native value and never put the real one back)
    void resume(const Input& in,const Block& b) noexcept {
        if(!ghost_.rein)return;
        const bool same=ghost_.rein==rein_,young=in.now>=ghost_.ns&&in.now-ghost_.ns<ghostNs;
        if(same&&young&&b.at<std::int32_t>(off::reinMode)==ghost_.written&&ghost_.native>=1&&ghost_.native<=3){
            overriding_=true;native_=ghost_.native;written_=ghost_.written;bump(stats.ghostResumed);}
        if(same||!young)ghost_=Ghost{};
    }
public:
    void step(const Input& in) noexcept {
        if(policy<=0||!available.load(std::memory_order_acquire)||!in.read||!in.base)return;
        bump(stats.steps);stats.held.store(int(in.held&3u),std::memory_order_relaxed);stats.heldRaw.store(int(in.heldRaw&3u),std::memory_order_relaxed);
        stats.freeNone.store(in.freeNone,std::memory_order_relaxed);if((in.heldRaw&~in.held)&3u)bump(stats.heldMasked);
        if(!in.want&&!overriding_){if(rein_)drop();state.store(StateNotFound,std::memory_order_relaxed);return;}
        Block b{};bool fresh=false; // fresh = found by a scan this step without state of its own (a lost override may resume on it)
        if(rein_&&!(readRein(in,rein_,b)&&identity(in,b)&&listed(in))){
            // one immediate rescan: a compacted manager list only moved the index (the same rein keeps its native / override state);
            // anything else (dismount, a new holder, popped / freed) = lost: an override is put back first if that is still safe (lose)
            const std::uintptr_t prev=rein_;rein_=0;index_=-1;
            const bool again=scan(in,b);
            if(!(again&&rein_==prev)){const auto r=rein_;const int i=index_;bump(stats.lost);lose(in,prev);drop();rein_=r;index_=i;fresh=again;}
            nextScanNs_=in.now+rescanNs;
        }
        if(!rein_){
            if(!in.want||(nextScanNs_&&in.now<nextScanNs_)){state.store(StateNotFound,std::memory_order_relaxed);return;}
            nextScanNs_=in.now+rescanNs;
            if(!scan(in,b)){state.store(StateNotFound,std::memory_order_relaxed);return;}
            fresh=true;
        }
        if(fresh)resume(in,b);
        state.store(StateFound,std::memory_order_relaxed);stats.rein.store(rein_,std::memory_order_relaxed);
        stats.active.store(int(b.at<std::uint8_t>(off::objActive)),std::memory_order_relaxed);
        int cur=b.at<std::int32_t>(off::reinMode);
        // the rope targets of the previous frame vs the IK'd wrist that set them (+0x210: LEFT -> wrist_l, else wrist_r; +0x220: RIGHT ->
        // wrist_r, else wrist_l): the rope really sits in the VR fist
        if(haveWrist_&&wristMode_>=1&&wristMode_<=3){
            const float s=100.f/std::max(in.upm,1e-3f);
            for(int k=0;k<2;++k){const auto t=b.vec(off::reinTarget+std::size_t(k)*16);
                const auto& w=wrist_[k==0?(wristMode_==1?1:0):(wristMode_==2?0:1)];
                const float cm=std::sqrt((t[0]-w[0])*(t[0]-w[0])+(t[1]-w[1])*(t[1]-w[1])+(t[2]-w[2])*(t[2]-w[2]))*s;
                if(std::isfinite(cm)){stats.wristCm[std::size_t(k)].store(cm,std::memory_order_relaxed);
                    float m=stats.wristCmMax[std::size_t(k)].load(std::memory_order_relaxed);
                    while(cm>m&&!stats.wristCmMax[std::size_t(k)].compare_exchange_weak(m,cm)){}
                    if(k==0&&cm>25.f)bump(stats.farFrames);}}
        }
        // ---- native value: anything the mod did not write is the game's ----
        if(overriding_){
            if(cur!=written_){native_=cur;overriding_=false;bump(stats.nativeChanges);}
            // a SetHolder (event 0) / cutscene event 5 / native BOTH re-ran SetHolderBones under our 3: all four pinned, only 3 is safe now
            else if(written_==3&&native_!=3&&pins(in,b)==2){native_=3;overriding_=false;written_=-1;bump(stats.adopted);}
            // under our 0 / 1 / 2 nothing drives +0x1c8 / +0x1cc - pinned (a native BOTH racing our store, checked every
            // frame, not only on a change) they would freeze in world space: 3 drives all four, the game evidently set up BOTH
            else if(written_>=0&&written_<=2&&pins(in,b)==2){int a=cur;if(write(in,3,a)){native_=3;overriding_=false;written_=-1;bump(stats.pinRepairs);}cur=a;}
        }else{if(native_>=0&&cur!=native_)bump(stats.nativeChanges);native_=cur;}
        int want=-1; // the wanted mode, -1 = none (an override goes back to the native value)
        if(in.want){const unsigned m=in.held&3u;want=m==1u?1:m==2u?2:m==3u?(policy==2?1:3):in.freeNone?0:-1;}
        int target=-1,after=cur; // after = the mode UpdateConstraints will use this frame
        if(native_<1||native_>3){if(want>=1)bump(stats.skippedNative);overriding_=false;written_=-1;} // NONE / RELEASE: not in the hands natively
        else if(want>=0)target=want;
        // no hand on the reins / leaving: the native value back (only if the mod changed it) - except our NONE: a dismount's NONE (event 7)
        // looks the same, a "restore" could put the rope into the hand of a rider walking away; the game's next event sets it anyway
        else if(overriding_&&written_==0){overriding_=false;written_=-1;bump(stats.freeLeft);}
        else if(overriding_)target=native_;
        if(target>=0&&target!=cur&&target!=3){const int p=pins(in,b); // a pinned +0x1c8/+0x1cc would freeze in 0 / 1 / 2
            // One hand let go while the rope is pinned in BOTH: it stays pinned until the game's next keep-0 event (a gait change). So: the game's own re-attach, exactly what its rein
            // events 1 / 9 / 2 do - SetAttachToHolder(mode, holder guid, false, false) 0x28a0ddc - re-pins for the new mode (0 unpins
            // +0x1c8 / +0x1cc: the rope back on the saddle). Hook A is the window the mode writes already use (the verlet task waited).
            // At most every 200 ms; .reins.ropefix 0 = off (wait for the game).
            if(p==2&&cur==3&&fixStuck&&(!lastFixNs_||in.now<lastFixNs_||in.now-lastFixNs_>=200'000'000ull)){lastFixNs_=in.now;
                using Fn=std::uint64_t(*)(void*,std::int32_t,std::uint32_t,bool,bool);
                reinterpret_cast<Fn>(in.base+setAttachToHolderOffset)(reinterpret_cast<void*>(rein_),target,b.at<std::uint32_t>(off::reinHolderGuid),false,false);
                bump(stats.stuckFixes);std::int32_t back=cur;
                if(get(in,rein_+off::reinMode,back)&&back==target){after=target;cur=target;
                    if(target!=native_){if(!overriding_)bump(stats.overrides);overriding_=true;written_=target;}else{overriding_=false;written_=-1;}
                    target=-1;}}
            if(target>=0&&p!=1){bump(p?stats.pinBlocked:stats.pinUnknown);if(p==2&&cur==3)bump(stats.stuckBoth);target=-1;}}
        if(target>=0&&(target==cur||write(in,target,after))){
            if(target!=native_){if(!overriding_)bump(stats.overrides);overriding_=true;written_=target;}
            else{if(overriding_)bump(stats.restores);overriding_=false;written_=-1;}
        }
        if(want==3&&after==3)bump(stats.bothFrames);
        stats.modeNative.store(native_,std::memory_order_relaxed);stats.modeCurrent.store(after,std::memory_order_relaxed);
        stats.modeWritten.store(overriding_?written_:-1,std::memory_order_relaxed);stats.overriding.store(overriding_,std::memory_order_relaxed);
        // this frame's IK'd wrists (hook A wrote them before this step): the next step's rope check
        haveWrist_=false;
        const int wr=b.at<std::int32_t>(off::reinWristR),wl=b.at<std::int32_t>(off::reinWristL);
        if(in.globals&&wr>=0&&wl>=0&&std::uint32_t(wr)<in.bones&&std::uint32_t(wl)<in.bones){
            const float* r=in.globals+std::size_t(wr)*16+12;const float* l=in.globals+std::size_t(wl)*16+12;
            wrist_[0]={r[0],r[1],r[2]};wrist_[1]={l[0],l[1],l[2]};wristMode_=after;haveWrist_=true;}
        if(!in.want&&!overriding_)drop(); // restored on the way out (dismount / REINS off / BODY off): the next mount scans again
    }
};
inline Rope rope{};

inline std::string report() {
    char b[1600];
    const unsigned s=state.load();
    std::snprintf(b,sizeof b,"{\"state\":\"%s\",\"verify\":\"%s\",\"policy\":%d,\"free\":%s,\"rein\":\"0x%llx\",\"active\":%d,\"held\":%d,\"heldRaw\":%d,"
        "\"heldMasked\":%llu,\"mode\":{\"native\":%d,"
        "\"current\":%d,\"written\":%d,\"overriding\":%s},\"overrides\":%llu,\"restores\":%llu,\"writes\":%llu,\"nativeChanges\":%llu,\"adopted\":%llu,"
        "\"pinBlocked\":%llu,\"pinUnknown\":%llu,\"stuckBoth\":%llu,\"stuckFixes\":%llu,\"pinRepairs\":%llu,\"raceWrites\":%llu,\"freeLeft\":%llu,"
        "\"lostOverriding\":%llu,\"lostRestored\":%llu,\"ghosts\":%llu,\"ghostResumed\":%llu,"
        "\"skippedNative\":%llu,\"bothFrames\":%llu,\"scans\":%llu,\"scanObjects\":%llu,\"reinsSeen\":%llu,"
        "\"found\":%llu,\"lost\":%llu,\"readFails\":%llu,\"steps\":%llu,\"wristCm\":[%.1f,%.1f],\"wristCmMax\":[%.1f,%.1f],\"farFrames\":%llu}",
        stateNames[s<stateNames.size()?s:0],verifyWhy.load(),policy,stats.freeNone.load()?"true":"false",(unsigned long long)stats.rein.load(),
        stats.active.load(),stats.held.load(),stats.heldRaw.load(),(unsigned long long)stats.heldMasked.load(),
        stats.modeNative.load(),stats.modeCurrent.load(),stats.modeWritten.load(),stats.overriding.load()?"true":"false",
        (unsigned long long)stats.overrides.load(),(unsigned long long)stats.restores.load(),(unsigned long long)stats.writes.load(),
        (unsigned long long)stats.nativeChanges.load(),(unsigned long long)stats.adopted.load(),(unsigned long long)stats.pinBlocked.load(),
        (unsigned long long)stats.pinUnknown.load(),(unsigned long long)stats.stuckBoth.load(),(unsigned long long)stats.stuckFixes.load(),(unsigned long long)stats.pinRepairs.load(),
        (unsigned long long)stats.raceWrites.load(),(unsigned long long)stats.freeLeft.load(),(unsigned long long)stats.lostOverriding.load(),
        (unsigned long long)stats.lostRestored.load(),(unsigned long long)stats.ghosts.load(),(unsigned long long)stats.ghostResumed.load(),
        (unsigned long long)stats.skippedNative.load(),(unsigned long long)stats.bothFrames.load(),
        (unsigned long long)stats.scans.load(),(unsigned long long)stats.scanObjects.load(),(unsigned long long)stats.reinsSeen.load(),
        (unsigned long long)stats.found.load(),(unsigned long long)stats.lost.load(),(unsigned long long)stats.readFails.load(),
        (unsigned long long)stats.steps.load(),double(stats.wristCm[0].load()),double(stats.wristCm[1].load()),double(stats.wristCmMax[0].load()),
        double(stats.wristCmMax[1].load()),(unsigned long long)stats.farFrames.load());
    return b;
}
} // namespace rdrvr::vr::reinrope
