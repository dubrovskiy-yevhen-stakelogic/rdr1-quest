#pragma once
// the requested mask without a property is the graphics.cfg key s02Mask (GraphicsSettings, default 0x1F), stored in
// cfgMask by TraceEntry observeScene every RenderScene before sceneBegin(). debug.rdrvr.s02, when set to a valid value, still
// overrides it completely (0 = native); a cleared or invalid property falls back to the cfg mask, not to native.
//
// Properties (re-read every 64 RenderScenes from TraceEntry observeScene -> sceneBegin(), the
// LampShadows.h pattern; a cleared property (setprop <k> '') is "absent"): debug.rdrvr.s02 hex
// lever mask (optional 0x prefix), 0 = native; absent/invalid = graphics.cfg s02Mask. Bits below.
// debug.rdrvr.s02ab hex mask of levers that ALTERNATE: off in phase a, on in phase b, flipping
// every 60 latched frames (latchFrame, called where StereoReplay.cpp latchBoundaryPhase runs, so a
// frame is never split). Frame mask = |. Bits: 0x0001 CR colour restarts merged
// (EngineColorRestart.h) 0x0002 MG right-eye merge every frame (StereoReplay.cpp) 0x0004 PT post
// tail bind-only (PostFxSkip.h tail) 0x0008 NF post no-flush (PostFxSkip.h, TraceEntry suppress*)
// 0x0010 IR indoor reflection skip (IndoorRt.h) 0x0020 IT indoor tree-RT skip (diagnostic, never
// in a preset) 0x0100 LR light restarts merged 0x0400 EB EYE CULL A: skip MAIN objects outside
// BOTH eyes 0x0800 EC EYE CULL B: per-eye skip of MAIN draws outside THAT eye 0x0040 MD / 0x0080
// AO / 0x0200 SF: batch 2, accepted but inert here
// Debug.rdrvr.eyecull (decimal, every 64 scenes): 0 = EYE CULL off, 1 = A only (outside both), 2 =
// A + B (per eye), 3 = A + B SWAPPED (diagnostic, visible: each eye drops the OTHER eye's outside
// set; 0x1000 in the effective mask); a set value replaces 0xc00 of whichever mask is requested.
// Gates: LR / EB are gated by Gates' default member initialisers (lrGate/ecGate below), so
// GameXr's `s2::Gates g{}` covers them without a GameXr edit LR is "notWired" until the GameXr
// light-stage calls exist (colorRestart.light.wired). Timing: the mask a frame runs with (current)
// is taken at its RenderScene entry (sceneBegin) from the frame mask latched at the previous
// frame's end (latchFrame), the same point that sets the byState signature of the next frame's
// command buffer; so levers that act after the latch of their own frame (PT tail, NF post) still
// use their frame's mask. Signature (GameXr stereoGpuState, last graphics item): _x%04x = the
// EFFECTIVE mask of the next frame (requested & installed & gated; a requested lever disabled by a
// fingerprint or by its gate clears its bit) + "t" in the 2 transition frames.
#include "StereoTestMode.h"
#include "EngineColorRestart.h" // LR gate (colorrestart::usable / lightWired / stereoLightUsable)
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

namespace rdrvr::engine::s02 {
enum Bit : unsigned {CR=0x1u,MG=0x2u,PT=0x4u,NF=0x8u,IR=0x10u,IT=0x20u,MD=0x40u,AO=0x80u,LR=0x100u,SF=0x200u,ECBoth=0x400u,ECRight=0x800u,
    Diagnostic=0x1000u,LRRight=0x2000u};
inline constexpr unsigned bitCount=16;
inline constexpr const char* bitNames[bitCount]{"cr","mg","pt","nf","ir","it","md","ao","lr","sf","ecBoth","ecRight","diagnostic","lrRight","b14","b15"};
inline constexpr unsigned implementedMask=CR|MG|PT|NF|IR|IT|LR|ECBoth|ECRight; // MD / AO / SF are inert
inline constexpr unsigned diagnosticMask=IT;                 // visible diagnostics: 0x1000 while any of them is effective
inline constexpr unsigned userMask=0x0fffu;                  // bits a property may request
inline constexpr unsigned propertyInterval=64,abWindow=60,transitionFrames=2;
inline constexpr unsigned postTailSites=11,postTailDefault=0x7ffu;
inline constexpr unsigned bitIndex(unsigned bit) noexcept {unsigned i=0;while(i<bitCount&&!(bit&(1u<<i)))++i;return i;}

// ---- pure parsing (host-tested) ----
// Hex mask with an optional 0x/0X prefix, at most 8 digits. -1 = absent/empty, -2 = invalid (counted, treated as absent).
constexpr long long parseHex(const char* s) noexcept {
    if(!s||!*s)return -1;
    if(s[0]=='0'&&(s[1]=='x'||s[1]=='X'))s+=2;
    if(!*s)return -2;
    unsigned long long v=0;unsigned digits=0;
    for(;*s;++s){
        const char c=*s;unsigned d=0;
        if(c>='0'&&c<='9')d=unsigned(c-'0');else if(c>='a'&&c<='f')d=unsigned(c-'a'+10);else if(c>='A'&&c<='F')d=unsigned(c-'A'+10);else return -2;
        if(++digits>8)return -2;
        v=v*16+d;
    }
    return (long long)(v&0xffffffffull);
}
// Decimal 0..999 (debug.rdrvr.stereotest). -1 absent, -2 invalid.
constexpr int parseDecimal(const char* s) noexcept {
    if(!s||!*s)return -1;
    int v=0;unsigned digits=0;
    for(;*s;++s){if(*s<'0'||*s>'9'||++digits>3)return -2;v=v*10+(*s-'0');}
    return v;
}

// ---- alternator (render thread; host-tested) ----
struct Alternator {
    std::uint64_t frames{};  // Frames latched since the alternation mask was set (phase a first)
    unsigned ab{};           // The alternation mask the counter belongs to
    bool phaseB{};
    unsigned transition{};   // frames still carrying the 't' suffix
    unsigned frameMask{};    // levers in force for the next frame
    std::uint64_t flips{},changes{};
    // One call per frame (latchFrame). Returns the frame mask of the next frame.
    unsigned step(unsigned requested,unsigned abMask) noexcept {
        requested&=userMask;abMask&=userMask;
        if(abMask!=ab){ab=abMask;frames=0;}
        bool b=false;
        if(ab){b=((frames/abWindow)&1u)!=0;++frames;if(b!=phaseB)++flips;}
        phaseB=b;
        const unsigned next=(requested&~ab)|(b?ab:0u);
        if(next!=frameMask){frameMask=next;transition=transitionFrames;++changes;}
        else if(transition)--transition;
        return next;
    }
    bool inTransition() const noexcept {return transition>0;}
    char phaseChar() const noexcept {return ab?(phaseB?'b':'a'):'-';}
};

// ---- runtime state ----
inline std::atomic<int> propMask{-1},propAb{-1},propPostTail{-1},propStereoTest{-1}; // -1 absent, -2 invalid
inline std::atomic<int> cfgMask{-1}; // graphics.cfg s02Mask (-1 = not published yet: native)
inline std::atomic<std::uint64_t> sceneSerial{},propertyReads{},propertyInvalid{},latches{};
inline std::atomic<unsigned> frameMask{};      // latched at the previous frame's end (latchFrame)
inline std::atomic<unsigned> current{};        // the mask of the running frame (sceneBegin copies frameMask)
inline std::atomic<unsigned> effectiveMask{};  // signature mask of the next frame (publishEffective)
inline std::atomic<bool> transitionNow{};      // the next frame carries 't'
inline std::atomic<char> phaseNow{'-'};
inline std::atomic<std::uint64_t> flips{},maskChanges{};
inline Alternator alternator;                  // render thread only
inline std::array<std::atomic<std::uint64_t>,bitCount> effectiveFrames{};
inline std::atomic<std::uint64_t> signatureFrames{},transitionFramesSeen{};
// Install-time results (static strings) and per-frame disabled reasons (static strings, "" = none), per bit index.
inline std::array<std::atomic<const char*>,bitCount> fingerprint{};
inline std::array<std::atomic<const char*>,bitCount> disabledReason{};
inline std::atomic<unsigned> installedMask{};  // engine side installed and verified (per bit)
// stereotest property (render thread)
inline int lastStereoTestValue=-1;
inline unsigned stereoTestWritten=UINT32_MAX;
inline std::atomic<std::uint64_t> stereoTestApplied{};
// _so effective (TraceEntry occluderAfterCached -> GameXr signature): threshold px, from the property; -1 before the first frame
inline std::atomic<int> smallObjectsPx{-1};
inline std::atomic<bool> smallObjectsProperty{};
// debug.rdrvr.lr.right / debug.rdrvr.eyecull.count (decimal; -1 absent, -2 invalid; 1 = on), EYE CULL A testing gate
// (sceneBegin: the count property or 0x400 in the requested or alternation mask, so both phases cost the same CPU),
// the sceneSerial of the last eye-cull frame with tested objects (EyeCull.h; ecGate), and graphics.eyeCull appended
// after the levers' report object (EyeCull.h registers it).
inline std::atomic<int> propLrRight{-1},propEyeCullCount{-1};
inline std::atomic<bool> eyeCullTesting{};
inline std::atomic<std::uint64_t> eyeCullScene{};
inline constexpr std::uint64_t eyeCullLiveScenes=4;
using ExtraReport=std::string(*)();
inline std::atomic<ExtraReport> eyeCullReport{};
inline bool lrRight() noexcept {return propLrRight.load(std::memory_order_relaxed)!=0;} // Default ON; debug.rdrvr.lr.right 0 = off
// debug.rdrvr.eyecull (decimal 0..3; -1 absent, -2 invalid = absent): EYE CULL mode, see the bit list above.
inline std::atomic<int> propEyeCull{-1};
// The EYE CULL mode whose bits a non-zero cfg mask gets while debug.rdrvr.eyecull is unset (2 = A + B).
inline constexpr unsigned eyeCullDefaultMode=2;
inline int eyeCullProperty() noexcept {const int p=propEyeCull.load(std::memory_order_relaxed);return p>=0&&p<=3?p:-1;}
// The mode EYE CULL A / B act under: the property; unset = 2 (the requested bits decide; never the swap).
inline unsigned eyeCullMode() noexcept {const int p=eyeCullProperty();return p>=0?unsigned(p):2u;}
inline constexpr unsigned eyeCullBits(unsigned mode) noexcept {return (mode>=1?unsigned(ECBoth):0u)|(mode>=2?unsigned(ECRight):0u);}
// The cfg path's request (pure; host-tested with both defaults): LR with every cfg mask; eye cull only with a non-zero one.
inline constexpr unsigned cfgRequest(unsigned cfg,unsigned defaultMode) noexcept {return (cfg|LR|((cfg&userMask)?eyeCullBits(defaultMode):0u))&userMask;}

// A valid lever-mask property wins; otherwise the cfg mask; nothing published = native.
inline unsigned requestedMask() noexcept {
    // an explicit debug.rdrvr.eyecull replaces 0xc00 of whichever mask is requested; unset, a non-zero cfg mask gets the default's
    const int e=eyeCullProperty();const bool ecSet=e>=0;
    const auto ec=[&](unsigned m){return ecSet?(m&~unsigned(ECBoth|ECRight))|eyeCullBits(unsigned(e)):m;};
    const int p=propMask.load(std::memory_order_relaxed);if(p>=0)return ec(unsigned(p)&userMask)&userMask;
    // LIGHT RESTARTS OFF (LR, left half) is on with every cfg mask
    // debug.rdrvr.s02 still wins (0x01f = LR off for A/B, 0 = native).
    const int c=cfgMask.load(std::memory_order_relaxed);return c>=0?ec(cfgRequest(unsigned(c),eyeCullDefaultMode))&userMask:0u;}
inline const char* maskSource() noexcept {
    return propMask.load(std::memory_order_relaxed)>=0?"property":cfgMask.load(std::memory_order_relaxed)>=0?"cfg":"none";}
inline unsigned abMask() noexcept {const int p=propAb.load(std::memory_order_relaxed);return p>=0?unsigned(p)&userMask:0u;}
inline unsigned postTailMask() noexcept {const int p=propPostTail.load(std::memory_order_relaxed);return p>=0?unsigned(p)&postTailDefault:postTailDefault;}
inline bool wants(unsigned bit) noexcept {return (current.load(std::memory_order_relaxed)&bit)!=0;}
// LR shares CR's engine proof (the same device byte, EngineColorRestart.h configure), so CR's result is LR's as well.
inline void setFingerprint(unsigned bit,const char* value) noexcept {
    const unsigned i=bitIndex(bit);if(i<bitCount)fingerprint[i].store(value,std::memory_order_relaxed);
    if(bit==CR)fingerprint[bitIndex(LR)].store(value,std::memory_order_relaxed);}
inline void setInstalled(unsigned bit,bool on) noexcept {if(bit==CR)bit|=LR;
    if(on)installedMask.fetch_or(bit,std::memory_order_relaxed);else installedMask.fetch_and(~bit,std::memory_order_relaxed);}

// STEREO TEST mode from the property, edge-triggered (pure; host-tested). Returns true when testMode was written.
inline bool applyStereoTest(int value,std::atomic<unsigned>& mode,int& last,unsigned& written) noexcept {
    if(value==last)return false;
    last=value;
    if(value>=0&&unsigned(value)<stereo::testModeCount){mode.store(unsigned(value),std::memory_order_relaxed);written=unsigned(value);return true;}
    if(written!=UINT32_MAX&&mode.load(std::memory_order_relaxed)==written){mode.store(0,std::memory_order_relaxed);written=UINT32_MAX;return true;}
    written=UINT32_MAX;return false;
}

#if defined(__ANDROID__)
inline void readHexProperty(const char* name,std::atomic<int>& out) noexcept {
    char v[PROP_VALUE_MAX]{};const int n=__system_property_get(name,v);
    const long long p=n>0?parseHex(v):-1;
    if(p==-2)propertyInvalid.fetch_add(1,std::memory_order_relaxed);
    out.store(p>=0?int(p&0x7fffffff):(p==-2?-2:-1),std::memory_order_relaxed);
}
inline void readDecimalProperty(const char* name,std::atomic<int>& out) noexcept {
    char v[PROP_VALUE_MAX]{};const int n=__system_property_get(name,v);
    const int p=n>0?parseDecimal(v):-1;if(p==-2)propertyInvalid.fetch_add(1,std::memory_order_relaxed);
    out.store(p,std::memory_order_relaxed);
}
#endif
// EYE CULL A tests objects when counting is asked for or the 0x400 (EYE CULL A) or 0x800 (EYE CULL B) lever is
// requested / alternated (pure; host-tested).
inline bool eyeCullTestingFor(int countProperty,unsigned requested,unsigned ab) noexcept {return countProperty==1||((requested|ab)&(ECBoth|ECRight))!=0;}
// Every RenderScene (TraceEntry observeScene, render thread): properties every 64 scenes (scene 1, 65, ...), then the frame
// mask latched at the previous frame's end becomes the running frame's mask.
inline void sceneBegin() noexcept {
    const auto scene=sceneSerial.fetch_add(1,std::memory_order_relaxed)+1;
#if defined(__ANDROID__)
    if((scene-1)%propertyInterval==0){
        const int saved=errno;
        readHexProperty("debug.rdrvr.s02",propMask);readHexProperty("debug.rdrvr.s02ab",propAb);readHexProperty("debug.rdrvr.posttail",propPostTail);
        char v[PROP_VALUE_MAX]{};const int n=__system_property_get("debug.rdrvr.stereotest",v);
        const int t=n>0?parseDecimal(v):-1;if(t==-2)propertyInvalid.fetch_add(1,std::memory_order_relaxed);
        propStereoTest.store(t,std::memory_order_relaxed);
        if(applyStereoTest(t<0?-1:t,stereo::testMode,lastStereoTestValue,stereoTestWritten))stereoTestApplied.fetch_add(1,std::memory_order_relaxed);
        readDecimalProperty("debug.rdrvr.lr.right",propLrRight);readDecimalProperty("debug.rdrvr.eyecull.count",propEyeCullCount);
        readDecimalProperty("debug.rdrvr.eyecull",propEyeCull); // EYE CULL mode (> 3 = invalid -> default)
        if(propEyeCull.load(std::memory_order_relaxed)>3){propEyeCull.store(-2,std::memory_order_relaxed);propertyInvalid.fetch_add(1,std::memory_order_relaxed);}
        errno=saved;propertyReads.fetch_add(1,std::memory_order_relaxed);
    }
#else
    (void)scene;
#endif
    current.store(frameMask.load(std::memory_order_relaxed),std::memory_order_relaxed);
    eyeCullTesting.store(eyeCullTestingFor(propEyeCullCount.load(std::memory_order_relaxed),requestedMask(),abMask()),std::memory_order_relaxed);
}
// Once per stereo frame where StereoReplay.cpp latchBoundaryPhase runs (GameXr FinishPrimary, before the signature).
inline unsigned latchFrame() noexcept {
    latches.fetch_add(1,std::memory_order_relaxed);
    auto& a=alternator;const auto flipsBefore=a.flips,changesBefore=a.changes;
    const unsigned m=a.step(requestedMask(),abMask());
    frameMask.store(m,std::memory_order_relaxed);transitionNow.store(a.inTransition(),std::memory_order_relaxed);phaseNow.store(a.phaseChar(),std::memory_order_relaxed);
    if(a.flips!=flipsBefore)flips.fetch_add(a.flips-flipsBefore,std::memory_order_relaxed);
    if(a.changes!=changesBefore)maskChanges.fetch_add(a.changes-changesBefore,std::memory_order_relaxed);
    return m;
}

// ---- effective mask (GameXr stereoGpuState, after latchFrame) ----
// ok/why per implemented lever: true = the lever acts when requested (why ignored), false = it is disabled for `why`.
struct Gate {bool ok{};const char* why{""};};
// Gates from their own modules (read at GameXr stereoGpuState through Gates' default member initialisers):
// LR: installed (= CR's proof), the byte usable, and the GameXr light-stage calls wired (colorRestart.light.scopes > 0).
inline Gate lrGate() noexcept {
    if(!(installedMask.load(std::memory_order_relaxed)&LR))return {false,"notInstalled"};
    if(!colorrestart::usable())return {false,colorrestart::reason()};
    return colorrestart::lightWired()?Gate{true,""}:Gate{false,"notWired"};
}
// EB: the bodyDraw hook installed (EyeCull.h configure from TraceEntry's DrawCommon GOT result)
// and objects tested within the last few scenes (needs SMALL OBJECTS > 0 or
// debug.rdrvr.smallobjects.histogram 1: the eligibility comes from the SmallObjectCull token; and a ready eye plan).
inline Gate ecGate() noexcept {
    if(!(installedMask.load(std::memory_order_relaxed)&ECBoth))return {false,"notInstalled"};
    if(eyeCullMode()==0)return {false,"eyecull0"}; // Kill: debug.rdrvr.eyecull 0
    const auto seen=eyeCullScene.load(std::memory_order_relaxed),now=sceneSerial.load(std::memory_order_relaxed);
    return seen&&now>=seen&&now-seen<=eyeCullLiveScenes?Gate{true,""}:Gate{false,"notTesting"};
}
// EC (EYE CULL B): installed with EB (the same bodyDraw hook), mode >= 2, and live as EB.
inline Gate ecRightGate() noexcept {
    if(!(installedMask.load(std::memory_order_relaxed)&ECRight))return {false,"notInstalled"};
    if(eyeCullMode()<2)return {false,eyeCullMode()?"eyecull1":"eyecull0"};
    return ecGate();
}
inline bool lrRightGate() noexcept {return lrRight()&&colorrestart::stereoLightUsable();}
struct Gates {Gate cr,mg,pt,nf,ir,it;unsigned forced{}; // forced: bits in force without their mask bit (merge=2 alias)
    Gate lr{lrGate()},ec{ecGate()};bool lrRight{lrRightGate()}; // (lrRight: effective-only 0x2000 with LR)
    Gate ecr{ecRightGate()};bool ecSwap{eyeCullMode()==3u};};   // (ecSwap: debug.rdrvr.eyecull 3 = visible diagnostic -> 0x1000)
// Pure: frame mask -> effective mask; writes the per-bit reasons of requested-but-disabled levers into reasons (nullptr ok).
inline unsigned effectiveFor(unsigned mask,const Gates& g,const char** reasons=nullptr) noexcept {
    unsigned e=mask&implementedMask;
    const struct {unsigned bit;const Gate* gate;} rows[]{{CR,&g.cr},{MG,&g.mg},{PT,&g.pt},{NF,&g.nf},{IR,&g.ir},{IT,&g.it},{LR,&g.lr},{ECBoth,&g.ec},
        {ECRight,&g.ecr}};
    for(const auto& r:rows){
        const bool asked=(mask&r.bit)!=0,off=asked&&!r.gate->ok;
        if(off)e&=~r.bit;
        if(reasons)reasons[bitIndex(r.bit)]=off?r.gate->why:"";
    }
    if(reasons)for(unsigned i=0;i<bitCount;++i)if(((mask&~implementedMask)>>i)&1u)reasons[i]="notInThisBuild";
    e|=g.forced&implementedMask;
    if(e&diagnosticMask)e|=Diagnostic;
    if((e&ECRight)&&g.ecSwap)e|=Diagnostic; // EYE CULL SWAP drops visible objects (diagnostic rows apart)
    if((e&LR)&&g.lrRight)e|=LRRight; // byState rows of the LR stereo half stay apart (_x21..)
    return e;
}
// Stores the effective mask of the next frame and counts it (once per frame).
inline unsigned publishEffective(const Gates& g) noexcept {
    const char* reasons[bitCount]{};
    const unsigned e=effectiveFor(frameMask.load(std::memory_order_relaxed),g,reasons);
    effectiveMask.store(e,std::memory_order_relaxed);
    for(unsigned i=0;i<bitCount;++i){disabledReason[i].store(reasons[i]?reasons[i]:"",std::memory_order_relaxed);
        if((e>>i)&1u)effectiveFrames[i].fetch_add(1,std::memory_order_relaxed);}
    signatureFrames.fetch_add(1,std::memory_order_relaxed);
    if(transitionNow.load(std::memory_order_relaxed))transitionFramesSeen.fetch_add(1,std::memory_order_relaxed);
    return e;
}
// "_x%04x" (+ "t" in a transition frame); appended LAST to the graphics signature (gpuSetState cuts the graphics part to fit).
inline void formatSuffix(char* out,std::size_t size,unsigned effective,bool transition) noexcept {
    if(out&&size)std::snprintf(out,size,"_x%04x%s",effective&0xffffu,transition?"t":"");
}
inline void appendSignature(char* sig,std::size_t size,unsigned effective,bool transition) noexcept {
    if(!sig||!size)return;const std::size_t used=std::strlen(sig);
    if(used+1<size)formatSuffix(sig+used,size-used,effective,transition);
}

// ---- report (TraceEntry graphics) ----
inline std::string hex4(unsigned v) {char b[16];std::snprintf(b,sizeof b,"0x%04x",v&0xffffu);return b;}
inline std::string report(const std::string& extraFingerprints="") {
    std::ostringstream o;
    const auto str=[](const std::atomic<const char*>& a){const char* s=a.load(std::memory_order_relaxed);return s?s:"";};
    const int cfg=cfgMask.load(std::memory_order_relaxed);
    o<<"\"s02\":{\"mask\":\""<<hex4(requestedMask())<<"\",\"maskSource\":\""<<maskSource()<<"\",\"cfgMask\":"
     <<(cfg>=0?"\""+hex4(unsigned(cfg))+"\"":std::string("null"))<<",\"abMask\":\""<<hex4(abMask())<<"\",\"frameMask\":\""<<hex4(frameMask.load())
     <<"\",\"current\":\""<<hex4(current.load())<<"\",\"effective\":\""<<hex4(effectiveMask.load())<<"\",\"installed\":\""<<hex4(installedMask.load())
     <<"\",\"phase\":\""<<phaseNow.load()<<"\",\"transition\":"<<(transitionNow.load()?1:0)<<",\"flips\":"<<flips.load()<<",\"maskChanges\":"<<maskChanges.load()
     <<",\"latches\":"<<latches.load()<<",\"scenes\":"<<sceneSerial.load()<<",\"signatureFrames\":"<<signatureFrames.load()
     <<",\"transitionFrames\":"<<transitionFramesSeen.load()<<",\"abWindow\":"<<abWindow<<",\"effectiveFrames\":[";
    for(unsigned i=0;i<bitCount;++i)o<<(i?",":"")<<effectiveFrames[i].load();
    o<<"],\"properties\":{\"s02\":"<<propMask.load()<<",\"s02ab\":"<<propAb.load()<<",\"posttail\":"<<propPostTail.load()
     <<",\"postTailMask\":\""<<hex4(postTailMask())<<"\",\"stereotest\":"<<propStereoTest.load()<<",\"stereoTestApplied\":"<<stereoTestApplied.load()
     <<",\"lrRight\":"<<propLrRight.load()<<",\"eyeCullCount\":"<<propEyeCullCount.load()<<",\"eyeCullTesting\":"<<(eyeCullTesting.load()?1:0)
     <<",\"eyeCull\":"<<propEyeCull.load()<<",\"eyeCullMode\":"<<eyeCullMode()<<",\"eyeCullDefault\":"<<eyeCullDefaultMode
     <<",\"reads\":"<<propertyReads.load()<<",\"invalid\":"<<propertyInvalid.load()<<"},\"fingerprints\":{";
    bool first=true;
    for(unsigned i=0;i<bitCount;++i){const char* f=fingerprint[i].load();if(!f)continue;o<<(first?"":",")<<'"'<<bitNames[i]<<"\":\""<<f<<'"';first=false;}
    if(!extraFingerprints.empty())o<<(first?"":",")<<extraFingerprints;
    o<<"},\"disabledReason\":{";first=true;
    for(unsigned i=0;i<bitCount;++i){const std::string r=str(disabledReason[i]);if(r.empty())continue;o<<(first?"":",")<<'"'<<bitNames[i]<<"\":\""<<r<<'"';first=false;}
    o<<"}}";
    if(const auto extra=eyeCullReport.load(std::memory_order_relaxed))o<<','<<extra();
    return o.str();
}

// Host tests: back to the start state (properties absent).
inline void resetForTests() noexcept {
    propMask=-1;propAb=-1;propPostTail=-1;propStereoTest=-1;cfgMask=-1;frameMask=0;current=0;effectiveMask=0;transitionNow=false;phaseNow='-';
    flips=0;maskChanges=0;latches=0;sceneSerial=0;alternator=Alternator{};for(auto& e:effectiveFrames)e=0;signatureFrames=0;transitionFramesSeen=0;
    lastStereoTestValue=-1;stereoTestWritten=UINT32_MAX;
    propLrRight=-1;propEyeCullCount=-1;eyeCullTesting=false;eyeCullScene=0;
    propEyeCull=-1;
}
}
