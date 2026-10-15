#pragma once
// COLOUR RESTARTS OFF (lever bit 0x1).
// librdr 1.58 arm64, checked with NDK 27.2 llvm-objdump/relocs:
//  * d11::VkContext_c::BindDepthStencilState @0x18af050: adrp/ldr GOT 0x2ddcaf8 (R_AARCH64_GLOB_DAT d11::GfxDevice_c::ms_Instance
//    = bss 0x4e292f60) -> ldr x8,[x8] (the VkDevice_c) -> ldrb w8,[x8,#0x13d6] -> cbz. With the byte set the function only stores
//    the new state and sets dirty 0x20; with it clear, a stencil-test-enabled state (+0x3d) bound after one without stencil
//    test also sets dirty 0x400 (@0x18af098..0x18af0a0), which makes FlushFrameBuffer 0x18add18 restart the same-framebuffer
//    LOAD render pass at every colour-stage boundary.
//  * The byte is VkConfig+0xb6 copied at VkDevice_c::Initialise; its only source is PARAM_UseDepthCopy (grcDevice::InitClass
//    @0x1ab99c8: ldr [GOT 0x2de0870]; ldr +0x18; cbz; strb #1,[sp,#0x10e]). Quest's device profile does not set it.
//  * No other instruction of the module uses #0x13d6 (grep of the full disassembly: one hit, 0x18af054).
// Lever: the byte is 1 for the duration of the LEFT colour stage (StereoReplay.cpp beginRecording/endRecording of Stage::Color,
// render thread) and of every Color stage of the stereo pass (beginStereo/endStereo from GameXr stereoGpuStage, which
// EngineStereoPass calls around each eye stage: eye 1 always, eye 0 when the left eye is not reused), compare-and-restore.
// A replayed right colour stage mirrors the merged left tape (raw Vulkan: the byte is not read); a right colour stage the
// replay did not handle (stereo off, MASK_NATIVE, COLOR_TAPE, CLONE_FLAGGED, REPLAY_DECLINED/FAULTED) is drawn natively by
// the engine and merges like the left one, so both eyes of a CR frame have the same restarts. The light
// stage stays native unless LR (below). Before the first write: code bytes + GOT proven at install (configure), then on the render thread the device pointer
// and the byte are read through the guarded reader once (and again whenever ms_Instance changes): byte 1 = the profile already
// has it ("native 1", nothing is written), byte > 1 = layout mismatch (disabled). Any failure leaves the lever disabled with a
// reason; nothing is ever written then.
// Report graphics.colorRestart{level, native, verified, disabledReason, scopeWrites, restores, restoreMisses, foreign, scopes,
// nested, stereo{scopes, writes, restores, rightNative, rightNativeMerged}, light{...}, colorPassesPerFrame{on, off}}.
//
// LR LIGHT RESTARTS OFF (lever bit 0x100): the same byte, compare-and-restore, held through the native light
// stages (RenderLights: every stencil-path light re-enables stencil test -> dirty 0x400 -> a same-framebuffer
// LOAD restart). Same engine proof as CR (one reader of the byte), so LR is usable exactly when CR is. Two
// halves:
//  * primary (beginLight(.., stereoPass=false ..)): the exact LEFT RenderLights of the primary path (GameXr gamePrimaryLights).
//    Its first pass is the engine's C:L D:L S:C stencil-clear pass (previous framebuffer = the G-buffer), so a merged pass that
//    the driver runs binned still loads colour/depth and clears stencil.
//  * stereo pass (stereoPass=true: EngineStereoPass Lighting via GameXr stereoGpuStage = the right eye, and the left eye when
//    the stereo pass redraws it), only with debug.rdrvr.lr.right=1 (default 0). Hazard: the right
//    light region also holds 2 C:D D:D S:D passes per frame: FlushFrameBuffer
//    picks DONT_CARE when ctx+0x30b0 == 0 and the "load next pass" bss byte librdr+0x4e6933cc is clear (see SunStagger.h).
//    DONT_CARE is only safe while the pass runs direct; a merged ~170-draw pass may run binned. So the stereo half also
//    sets that byte (compare-and-swap 0->1, never cleared by the mod) at the stage begin: the FIRST pass the stage begins then LOADs (flushReset @0x18ae0a8
//    clears it at that pass begin: light.right.preserve.consumed); if no pass took it, it stays set (preserve.pending: the next pass LOADs).
//    A second DONT_CARE pass is not covered, so lr.right defaults to 0.
// The byte write and the preserve proof add no API calls. Report colorRestart.light{scopes[2], wanted, writes[2], restores,
// restoreMisses, foreign, nested, stereoSkipped, right{verified, reason, preserve{writes, consumed, pending, foreign}}, census{..}}.
// Light-stage census, both phases. Between beginLight and endLight the RENDER
// thread's vkCmdBeginRenderPass / vkCmdEndRenderPass / vkCmdPipelineBarrier (IndexedDispatch hooks,
// one relaxed load per call while no light scope is open) are counted per half and phase (off = native restarts, on = the lever's byte
// held the stage): passes, and barrier calls by position: gap = between two passes of the stage (a restart gap of the native
// phase), inPass = inside an open pass (self-dependency barriers; more per stage with LR = a gap barrier that lands inside the
// merged pass), edge = before the first / after the last pass. gapLayout = image barriers with oldLayout != newLayout in the
// gaps: off-phase gapLayout > 0 = a layout transition sits in a restart gap, so LR is safe only if the on phase still shows it
// per stage (the engine ends the pass for it itself) and inPass does not grow; on-phase passes/stage shows the merge.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace rdrvr::engine::colorrestart {
// ---- librdr 1.58 arm64 (module-relative) ----
inline constexpr std::uintptr_t gfxDeviceGotOffset=0x2ddcaf8,gfxDeviceOffset=0x4e292f60; // GLOB_DAT -> bss pointer variable
inline constexpr std::uintptr_t depthCopyField=0x13d6;  // VkDevice_c byte (object size 0x13d8)
inline constexpr std::uintptr_t bindDsOffset=0x18af050; // d11::VkContext_c::BindDepthStencilState (head: device byte test)
inline constexpr std::array<unsigned char,24> bindDsFingerprint{
#include "gamecode/EngineColorRestart_18af050_24_8d.inc"
};
inline constexpr std::uintptr_t bindDsRuleOffset=0x18af090; // ldrb [new,#0x3d]; cbz; ldr/orr #0x400/str dirty
inline constexpr std::array<unsigned char,20> bindDsRuleFingerprint{
#include "gamecode/EngineColorRestart_18af090_20_8d.inc"
};
inline constexpr std::uintptr_t initClassOffset=0x1ab99c8;  // grcDevice::InitClass: PARAM_UseDepthCopy -> config byte
inline constexpr std::array<unsigned char,20> initClassFingerprint{
#include "gamecode/EngineColorRestart_1ab99c8_20_8d.inc"
};
// LR stereo half: the engine "load next pass" byte and its FlushFrameBuffer reader/selector/reset (values = SunStagger.h
// stagger::preserveFlagOffset/flushPage/flushRead/flushSelect/flushReset, re-proven here so LR does not depend on the stagger install).
inline constexpr std::uintptr_t preserveFlagOffset=0x4e6933cc;                // bss byte (VkContext_c "LOAD the next pass")
inline constexpr std::uintptr_t flushPageOffset=0x18add9c;                    // adrp x22,0x4e693000
inline constexpr std::array<unsigned char,4> flushPageFingerprint{54,111,38,208};
inline constexpr std::uintptr_t flushReadOffset=0x18addcc;                    // ldr x9,[x19,#0x30b0]; ldrb w10,[x22,#0x3cc]
inline constexpr std::array<unsigned char,8> flushReadFingerprint{105,90,88,249,202,50,79,57};
inline constexpr std::uintptr_t flushSelectOffset=0x18adf78;                  // load = (prev fb || byte) ? LOAD : DONT_CARE
inline constexpr std::array<unsigned char,20> flushSelectFingerprint{
#include "gamecode/EngineColorRestart_18adf78_20_8d.inc"
};
inline constexpr std::uintptr_t flushResetOffset=0x18ae0a8;                   // strb wzr,[x22,#0x3cc] (every pass begin)
inline constexpr std::array<unsigned char,4> flushResetFingerprint{223,50,15,57};

using Reader=bool(*)(void* context,std::uintptr_t address,void* out,std::size_t size);
struct Stats {
    std::atomic<bool> configured{},verified{},disabled{};
    std::atomic<const char*> disabledReason{"unconfigured"};
    std::atomic<int> native{-1};                       // byte value at the first check (-1 not read yet)
    std::atomic<std::uint64_t> scopes{},scopeWrites{},restores{},restoreMisses{},foreign{},deviceChanges{},wantedScopes{};
    std::atomic<std::uint64_t> passesOn{},framesOn{},passesOff{},framesOff{};
    // Stereo pass Color stages opened with CR wanted (both eyes), the byte writes/restores of their own scopes, scopes opened
    // inside the other scope (byte already set), right colour stages drawn natively (replay did not handle them) while CR
    // was wanted, and of those the ones that ran with the byte set by a CR scope (rightNative - rightNativeMerged = eye mismatch).
    std::atomic<std::uint64_t> stereoScopes{},stereoWrites{},stereoRestores{},nested{},rightNative{},rightNativeMerged{};
    std::atomic<unsigned> lastLevel{};
    // LR: light stages seen [0 primary left, 1 stereo pass] (also with LR off: proves the GameXr wiring), the ones with LR
    // wanted, byte writes per half, restores, misses, foreign byte, nested in another scope, stereo-pass stages left native
    // because lr.right=0; stereo half: preserve-byte proof and its writes / consumed by a pass begin / still pending at the end / foreign.
    std::array<std::atomic<std::uint64_t>,2> lightScopes{},lightWrites{};
    std::atomic<std::uint64_t> lightWanted{},lightRestores{},lightRestoreMisses{},lightForeign{},lightNested{},lightStereoSkipped{};
    std::atomic<bool> preserveVerified{};std::atomic<const char*> preserveReason{"unconfigured"};
    std::atomic<std::uint64_t> preserveWrites{},preserveConsumed{},preservePending{},preserveForeign{};
};
inline Stats stats;
struct Config {std::uintptr_t deviceVar{};Reader read{};std::uintptr_t preserve{};}; // preserve: LR "load next pass" byte (0 = unproven)
inline Config config;                                 // written once before configured is released
inline std::uintptr_t checkedDevice{};                // render thread: device pointer whose byte was proven readable
inline std::uintptr_t scopeAddress{};                 // render thread: byte written by the open LEFT scope (0 = none)
inline std::uintptr_t stereoScopeAddress{};           // render thread: byte written by the open stereo-pass Color scope (0 = none)
inline bool stereoWanted{},stereoMerged{};            // render thread: open stereo scope wanted CR / the byte is ours during it
inline std::uintptr_t lightScopeAddress{};            // LR, render thread: byte written by the open light scope (0 = none)
inline std::uintptr_t lightPreserveAddress{};         // LR, render thread: preserve byte written by the open stereo light scope
inline bool preserveChecked{};                        // LR, render thread: preserve byte proven readable
// LR census: [half primary/stereo][phase off/on] totals; the open scope lives on the
// thread that opened it (censusHere), so the recording hooks of other threads stop at the TLS flag.
struct CensusRow {std::atomic<std::uint64_t> stages{},passes{},gapBarriers{},gapLayout{},inPass{},edge{};};
inline std::array<std::array<CensusRow,2>,2> census;
struct CensusScope {unsigned half{};bool merged{},inPass{};std::uint64_t passes{},pending{},pendingLayout{},gapBarriers{},gapLayout{},inPassBarriers{},edge{};};
inline CensusScope censusScope;                       // render thread (only touched where censusHere is set)
inline std::atomic<bool> censusOpen{};                // a light scope is open on some thread (hook fast path)
inline thread_local bool censusHere{};                // ... on this one

inline void disable(const char* why) noexcept {stats.disabledReason.store(why,std::memory_order_relaxed);stats.disabled.store(true,std::memory_order_relaxed);}
// Install time (TraceEntry, before any colour stage): code = exec fingerprint check, read = guarded memory read.
template<class Code> bool configure(std::uintptr_t base,Code code,Reader read) noexcept {
    std::uintptr_t got{};
    const char* why=nullptr;
    if(!code(bindDsOffset,bindDsFingerprint)||!code(bindDsRuleOffset,bindDsRuleFingerprint))why="bindDepthStencilCode";
    else if(!code(initClassOffset,initClassFingerprint))why="initClassCode";
    else if(!read||!read(nullptr,base+gfxDeviceGotOffset,&got,sizeof got)||got!=base+gfxDeviceOffset)why="got";
    // LR stereo half: FlushFrameBuffer must read the preserve byte at that address and pick LOAD for it, and reset it per pass
    const bool flush=code(flushPageOffset,flushPageFingerprint)&&code(flushReadOffset,flushReadFingerprint)&&
        code(flushSelectOffset,flushSelectFingerprint)&&code(flushResetOffset,flushResetFingerprint);
    config={base+gfxDeviceOffset,read,flush?base+preserveFlagOffset:0};
    stats.preserveVerified.store(flush,std::memory_order_relaxed);stats.preserveReason.store(flush?"":"flushCode",std::memory_order_relaxed);
    stats.configured.store(true,std::memory_order_release);
    if(why){disable(why);return false;}
    stats.verified.store(true,std::memory_order_release);stats.disabledReason.store("",std::memory_order_relaxed);
    return true;
}
inline bool usable() noexcept {
    return stats.verified.load(std::memory_order_acquire)&&!stats.disabled.load(std::memory_order_relaxed)&&stats.native.load(std::memory_order_relaxed)!=1;}
// Render thread: the device byte address, proven through the guarded reader whenever the device pointer changes. 0 = unusable.
inline std::uintptr_t byteAddress() noexcept {
    if(!usable()||!config.deviceVar||!config.read)return 0;
    std::uintptr_t device{};
    std::memcpy(&device,reinterpret_cast<const void*>(config.deviceVar),sizeof device); // module bss (GOT-proven): plain load
    if(!device)return 0; // device not created yet: try again later
    if(device!=checkedDevice){ // first use / new device: prove the byte through the guarded reader
        unsigned char v{};
        if(!config.read(nullptr,device+depthCopyField,&v,1)){disable("deviceRead");return 0;}
        if(checkedDevice)stats.deviceChanges.fetch_add(1,std::memory_order_relaxed);
        stats.native.store(int(v),std::memory_order_relaxed);
        if(v==1){disable("native 1");return 0;}   // the profile already merges: nothing to do
        if(v>1){disable("byte");return 0;}
        checkedDevice=device;
    }
    return device+depthCopyField;
}
inline bool exchangeByte(std::uintptr_t address,unsigned char expected,unsigned char desired) noexcept {
#if defined(__cpp_lib_atomic_ref)
    return std::atomic_ref<unsigned char>(*reinterpret_cast<unsigned char*>(address)).compare_exchange_strong(expected,desired,std::memory_order_relaxed);
#else
    return __atomic_compare_exchange_n(reinterpret_cast<unsigned char*>(address),&expected,desired,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
#endif
}
inline unsigned char loadByte(std::uintptr_t address) noexcept { // LR (preserve byte observation)
#if defined(__cpp_lib_atomic_ref)
    return std::atomic_ref<unsigned char>(*reinterpret_cast<unsigned char*>(address)).load(std::memory_order_relaxed);
#else
    return __atomic_load_n(reinterpret_cast<unsigned char*>(address),__ATOMIC_RELAXED);
#endif
}
// beginRecording(Stage::Color) [render]: wanted = CR in force for this frame.
inline void begin(bool wanted) noexcept {
    stats.scopes.fetch_add(1,std::memory_order_relaxed);stats.lastLevel.store(wanted?1u:0u,std::memory_order_relaxed);
    if(!wanted){if(!checkedDevice&&usable())(void)byteAddress();return;} // read-only probe: report native 0/1 even with the lever off
    if(scopeAddress)return; // a scope is already open (never nested on purpose)
    stats.wantedScopes.fetch_add(1,std::memory_order_relaxed);
    if(stereoScopeAddress){stats.nested.fetch_add(1,std::memory_order_relaxed);return;} // a stereo Color scope owns the byte already
    const auto address=byteAddress();if(!address)return;
    if(exchangeByte(address,0,1)){scopeAddress=address;stats.scopeWrites.fetch_add(1,std::memory_order_relaxed);}
    else stats.foreign.fetch_add(1,std::memory_order_relaxed); // not 0 any more: someone else owns it, leave it
}
// endRecording(Stage::Color) [render], first statement of every path: compare-and-restore. Returns whether this scope wrote.
inline bool end() noexcept {
    const bool wrote=scopeAddress!=0;
    if(wrote){
        if(exchangeByte(scopeAddress,1,0))stats.restores.fetch_add(1,std::memory_order_relaxed);
        else stats.restoreMisses.fetch_add(1,std::memory_order_relaxed); // someone wrote meanwhile: their value stays
        scopeAddress=0;
    }
    return wrote;
}
// Stereo pass Color stage [render] (GameXr stereoGpuStage begin, both eyes, replayed or native): wanted = CR in force.
inline void beginStereo(bool wanted) noexcept {
    stereoWanted=wanted;stereoMerged=false;
    if(!wanted||stereoScopeAddress)return; // stereoScopeAddress: the previous stage's end was missed (never: GpuEnd destructor)
    stats.stereoScopes.fetch_add(1,std::memory_order_relaxed);
    if(scopeAddress){stats.nested.fetch_add(1,std::memory_order_relaxed);stereoMerged=true;return;} // left scope still open: byte already set
    const auto address=byteAddress();if(!address)return;
    if(exchangeByte(address,0,1)){stereoScopeAddress=address;stereoMerged=true;stats.stereoWrites.fetch_add(1,std::memory_order_relaxed);}
    else stats.foreign.fetch_add(1,std::memory_order_relaxed); // not 0: someone else owns it, leave it
}
// Stereo pass Color stage end [render] (GameXr stereoGpuStage end, every path): compare-and-restore; rightNative = this was
// the right eye's colour stage and the replay did not handle it (the engine drew it natively).
inline void endStereo(bool rightNative) noexcept {
    if(stereoScopeAddress){
        if(exchangeByte(stereoScopeAddress,1,0))stats.stereoRestores.fetch_add(1,std::memory_order_relaxed);
        else stats.restoreMisses.fetch_add(1,std::memory_order_relaxed); // someone wrote meanwhile: their value stays
        stereoScopeAddress=0;
    }
    if(stereoWanted&&rightNative){stats.rightNative.fetch_add(1,std::memory_order_relaxed);if(stereoMerged)stats.rightNativeMerged.fetch_add(1,std::memory_order_relaxed);}
    stereoWanted=false;stereoMerged=false;
}
// LR [render]: the preserve byte address, proven readable through the guarded reader at the first use (0 = unusable).
inline std::uintptr_t preserveAddress() noexcept {
    if(!stats.preserveVerified.load(std::memory_order_relaxed)||!config.preserve)return 0;
    if(!preserveChecked){
        unsigned char v{};const char* why=!config.read||!config.read(nullptr,config.preserve,&v,1)?"preserveRead":v>1?"preserveByte":nullptr;
        if(why){stats.preserveReason.store(why,std::memory_order_relaxed);stats.preserveVerified.store(false,std::memory_order_relaxed);return 0;}
        preserveChecked=true;
    }
    return config.preserve;
}
inline bool lightWired() noexcept {return stats.lightScopes[0].load(std::memory_order_relaxed)+stats.lightScopes[1].load(std::memory_order_relaxed)>0;}
inline bool stereoLightUsable() noexcept {return usable()&&stats.preserveVerified.load(std::memory_order_relaxed);}
// ---- LR census ----
// [render] publish the open scope (trailing barriers after the last pass are edge barriers).
inline void censusClose() noexcept {
    if(!censusHere)return;
    const auto& s=censusScope;auto& r=census[s.half][s.merged?1:0];
    const auto add=[](std::atomic<std::uint64_t>& a,std::uint64_t v){if(v)a.fetch_add(v,std::memory_order_relaxed);};
    add(r.stages,1);add(r.passes,s.passes);add(r.gapBarriers,s.gapBarriers);add(r.gapLayout,s.gapLayout);add(r.inPass,s.inPassBarriers);
    add(r.edge,s.edge+s.pending);
    censusHere=false;censusOpen.store(false,std::memory_order_relaxed);
}
inline void censusBegin(unsigned half,bool merged) noexcept {
    censusClose();censusScope=CensusScope{};censusScope.half=half;censusScope.merged=merged;
    censusHere=true;censusOpen.store(true,std::memory_order_relaxed);
}
// IndexedDispatch vkCmdBeginRenderPass (begin=true, before the pass) / vkCmdEndRenderPass (false) [any recording thread]:
// a barrier batch waiting since the previous pass of the stage becomes a gap batch when another pass of the stage begins.
inline void censusPass(bool begin) noexcept {
    if(!censusOpen.load(std::memory_order_relaxed)||!censusHere)return;
    auto& s=censusScope;
    if(!begin){s.inPass=false;return;}
    if(s.passes){s.gapBarriers+=s.pending;s.gapLayout+=s.pendingLayout;}else s.edge+=s.pending;
    s.pending=0;s.pendingLayout=0;++s.passes;s.inPass=true;
}
// IndexedDispatch vkCmdPipelineBarrier [any recording thread]; ImageBarrier = VkImageMemoryBarrier (oldLayout/newLayout).
template<class ImageBarrier> inline void censusBarrier(std::uint32_t images,const ImageBarrier* image) noexcept {
    if(!censusOpen.load(std::memory_order_relaxed)||!censusHere)return;
    auto& s=censusScope;
    if(s.inPass){++s.inPassBarriers;return;}
    ++s.pending;
    if(image)for(std::uint32_t k=0;k<images;++k)if(image[k].oldLayout!=image[k].newLayout)++s.pendingLayout;
}
// LR light stage begin [render]. stereoPass=false: the exact primary left RenderLights (GameXr gamePrimaryLights, before
// original()); true: an EngineStereoPass Lighting stage (GameXr stereoGpuStage begin). wanted = LR in force for this frame,
// stereoHalf = debug.rdrvr.lr.right=1 (the stereo half also needs the preserve-byte proof). Returns whether the lever's byte holds the stage.
inline bool lightByte(unsigned half,bool stereoPass,bool wanted,bool stereoHalf) noexcept {
    if(!wanted)return false;
    stats.lightWanted.fetch_add(1,std::memory_order_relaxed);
    std::uintptr_t preserve{};
    if(stereoPass){preserve=stereoHalf?preserveAddress():0;if(!preserve){stats.lightStereoSkipped.fetch_add(1,std::memory_order_relaxed);return false;}}
    if(lightScopeAddress||scopeAddress||stereoScopeAddress){stats.lightNested.fetch_add(1,std::memory_order_relaxed);return false;} // never expected
    const auto address=byteAddress();if(!address)return false;
    if(!exchangeByte(address,0,1)){stats.lightForeign.fetch_add(1,std::memory_order_relaxed);return false;} // not 0: someone else owns it
    lightScopeAddress=address;stats.lightWrites[half].fetch_add(1,std::memory_order_relaxed);
    if(!preserve)return true;
    // the stage's first pass LOADs instead of DONT_CARE; already 1 (CopyTexture asked for it) = the same effect, not the lever's
    if(exchangeByte(preserve,0,1)){lightPreserveAddress=preserve;stats.preserveWrites.fetch_add(1,std::memory_order_relaxed);}
    else stats.preserveForeign.fetch_add(1,std::memory_order_relaxed);
    return true;
}
inline void beginLight(bool stereoPass,bool wanted,bool stereoHalf) noexcept {
    const unsigned half=stereoPass?1u:0u;
    stats.lightScopes[half].fetch_add(1,std::memory_order_relaxed);
    censusBegin(half,lightByte(half,stereoPass,wanted,stereoHalf)); // Census of this stage, both phases
}
// LR light stage end [render], first statement of every end path (GameXr End destructor / stereoGpuStage end):
// compare-and-restore of the device byte. The preserve byte is only observed, never cleared: 0 = a pass begin took it
// (consumed), 1 = still pending, so the next pass LOADs (always safe; clearing could drop an engine CopyTexture request).
inline void endLight() noexcept {
    if(lightScopeAddress){
        if(exchangeByte(lightScopeAddress,1,0))stats.lightRestores.fetch_add(1,std::memory_order_relaxed);
        else stats.lightRestoreMisses.fetch_add(1,std::memory_order_relaxed); // someone wrote meanwhile: their value stays
        lightScopeAddress=0;
    }
    if(lightPreserveAddress){
        (loadByte(lightPreserveAddress)?stats.preservePending:stats.preserveConsumed).fetch_add(1,std::memory_order_relaxed);
        lightPreserveAddress=0;
    }
    censusClose();
}
// colorRestart.light.census{hooked, primary|stereo:{off|on:{stages, passes, perStage, gapBarriers, gapLayout, inPass, edge}}}.
// hooked = the IndexedDispatch calls are in (a light stage always begins >= 1 pass); 0 with stages > 0 = those calls are missing.
inline std::string censusReport() {
    std::ostringstream o;std::uint64_t passes=0;
    for(const auto& h:census)for(const auto& r:h)passes+=r.passes.load(std::memory_order_relaxed);
    o<<"\"census\":{\"hooked\":"<<(passes?1:0);
    static constexpr const char* halves[2]{"primary","stereo"};static constexpr const char* phases[2]{"off","on"};
    for(unsigned h=0;h<2;++h){o<<",\""<<halves[h]<<"\":{";
        for(unsigned m=0;m<2;++m){const auto& r=census[h][m];const auto n=r.stages.load(),p=r.passes.load();
            o<<(m?",":"")<<'"'<<phases[m]<<"\":{\"stages\":"<<n<<",\"passes\":"<<p<<",\"perStage\":"<<(n?double(p)/double(n):-1.0)
             <<",\"gapBarriers\":"<<r.gapBarriers.load()<<",\"gapLayout\":"<<r.gapLayout.load()<<",\"inPass\":"<<r.inPass.load()<<",\"edge\":"<<r.edge.load()<<'}';}
        o<<'}';}
    o<<'}';return o.str();
}
// Left colour tape passes of a valid recording (census), split by whether the byte was set for it.
inline void notePasses(bool on,unsigned passes) noexcept {
    if(!passes)return;
    (on?stats.passesOn:stats.passesOff).fetch_add(passes,std::memory_order_relaxed);(on?stats.framesOn:stats.framesOff).fetch_add(1,std::memory_order_relaxed);
}
inline const char* reason() noexcept {const char* r=stats.disabledReason.load(std::memory_order_relaxed);return r?r:"";}
inline std::string report() {
    const auto& s=stats;std::ostringstream o;
    const auto mean=[](std::uint64_t a,std::uint64_t n){return n?double(a)/double(n):-1.0;};
    o<<"\"colorRestart\":{\"level\":"<<s.lastLevel.load()<<",\"native\":"<<s.native.load()<<",\"verified\":"<<(s.verified.load()?1:0)
     <<",\"usable\":"<<(usable()?1:0)<<",\"disabledReason\":\""<<reason()<<"\",\"scopes\":"<<s.scopes.load()<<",\"wantedScopes\":"<<s.wantedScopes.load()
     <<",\"scopeWrites\":"<<s.scopeWrites.load()<<",\"restores\":"<<s.restores.load()<<",\"restoreMisses\":"<<s.restoreMisses.load()
     <<",\"foreign\":"<<s.foreign.load()<<",\"deviceChanges\":"<<s.deviceChanges.load()<<",\"nested\":"<<s.nested.load()
     <<",\"stereo\":{\"scopes\":"<<s.stereoScopes.load()<<",\"writes\":"<<s.stereoWrites.load()<<",\"restores\":"<<s.stereoRestores.load()
     <<",\"rightNative\":"<<s.rightNative.load()<<",\"rightNativeMerged\":"<<s.rightNativeMerged.load()<<"}"
     // LR: light stages [primary left, stereo pass]; writes per half prove the merge is in force
     <<",\"light\":{\"scopes\":["<<s.lightScopes[0].load()<<','<<s.lightScopes[1].load()<<"],\"wired\":"<<(lightWired()?1:0)
     <<",\"wanted\":"<<s.lightWanted.load()<<",\"writes\":["<<s.lightWrites[0].load()<<','<<s.lightWrites[1].load()<<"],\"restores\":"<<s.lightRestores.load()
     <<",\"restoreMisses\":"<<s.lightRestoreMisses.load()<<",\"foreign\":"<<s.lightForeign.load()<<",\"nested\":"<<s.lightNested.load()
     <<",\"stereoSkipped\":"<<s.lightStereoSkipped.load()<<",\"right\":{\"verified\":"<<(s.preserveVerified.load()?1:0)
     <<",\"reason\":\""<<[&]{const char* r=s.preserveReason.load();return r?r:"";}()<<"\",\"preserve\":{\"writes\":"<<s.preserveWrites.load()
     <<",\"consumed\":"<<s.preserveConsumed.load()<<",\"pending\":"<<s.preservePending.load()<<",\"foreign\":"<<s.preserveForeign.load()<<"}},"
     <<censusReport()<<"}"
     <<",\"colorPassesPerFrame\":{\"on\":"<<mean(s.passesOn.load(),s.framesOn.load())<<",\"framesOn\":"<<s.framesOn.load()
     <<",\"off\":"<<mean(s.passesOff.load(),s.framesOff.load())<<",\"framesOff\":"<<s.framesOff.load()<<"}}";
    return o.str();
}
// Host tests.
inline void resetForTests() noexcept {
    stats.configured=false;stats.verified=false;stats.disabled=false;stats.disabledReason="unconfigured";stats.native=-1;
    for(auto* a:{&stats.scopes,&stats.scopeWrites,&stats.restores,&stats.restoreMisses,&stats.foreign,&stats.deviceChanges,&stats.wantedScopes,
                 &stats.passesOn,&stats.framesOn,&stats.passesOff,&stats.framesOff,&stats.stereoScopes,&stats.stereoWrites,
                 &stats.stereoRestores,&stats.nested,&stats.rightNative,&stats.rightNativeMerged,
                 &stats.lightScopes[0],&stats.lightScopes[1],&stats.lightWrites[0],&stats.lightWrites[1],&stats.lightWanted,&stats.lightRestores,
                 &stats.lightRestoreMisses,&stats.lightForeign,&stats.lightNested,&stats.lightStereoSkipped,
                 &stats.preserveWrites,&stats.preserveConsumed,&stats.preservePending,&stats.preserveForeign})a->store(0);
    stats.preserveVerified=false;stats.preserveReason="unconfigured";
    config=Config{};checkedDevice=0;scopeAddress=0;stereoScopeAddress=0;stereoWanted=false;stereoMerged=false;
    lightScopeAddress=0;lightPreserveAddress=0;preserveChecked=false;
    for(auto& h:census)for(auto& r:h)for(auto* a:{&r.stages,&r.passes,&r.gapBarriers,&r.gapLayout,&r.inPass,&r.edge})a->store(0);
    censusScope=CensusScope{};censusHere=false;censusOpen=false;
}
} // namespace rdrvr::engine::colorrestart
