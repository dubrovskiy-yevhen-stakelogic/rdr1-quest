#pragma once
// STEREO TEST 20 "R EYE MERGE PASSES".
// Pure, host-testable merge logic of the right-eye colour replay: SPIR-V texture-reference scan, per-boundary merge
// decision (rules M1-M9), droppable gap barriers (M7) and the report counters. StereoBoundary.h includes
// this header and compiles against these names.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rdrvr::engine::stereo::merge {
// One bindless texture index read by a shader: (set-0 uniform block Binding, OpMemberDecorate Offset) of the uint
// member that indexes g_textures2D / g_texturesCUBE.
struct TexRef {std::uint16_t binding{},offset{};};
// count <= 32 refs, sorted by (binding, offset), unique; unknown = some image-array access could not be traced, the array
// escaped (loaded/stored/copied/passed whole), more than 32 refs, or the module did not parse; valid = scanned (parsed).
// !valid always comes with unknown=true, so callers only need to test `unknown`.
struct TexScan {std::array<TexRef,32> refs{};std::uint8_t count{};bool unknown{},valid{};};
// Optional per-module counters (host test, diagnostics): image-array access chains, how many of them could not be
// traced, and whole-array uses other than an access-chain base (each of the last two sets TexScan::unknown).
struct TexScanCounts {std::uint32_t accesses{},unknownAccesses{},escapes{};};

namespace detail {
struct SpvId {std::uint32_t def{},set{UINT32_MAX},binding{UINT32_MAX};bool texArray{};}; // def = word offset (0 = none)
}

// SPIR-V words of one module -> its texture refs:
//  - image-array variables: OpVariable UniformConstant, DescriptorSet != 0, pointee OpTypeArray/OpTypeRuntimeArray (28/29)
//    of OpTypeImage (25);
//  - every OpAccessChain/OpInBoundsAccessChain (65/66) on such a variable is one access; its index is traced through
//    OpBitcast/OpUConvert/OpSConvert/OpCopyObject/OpConvert* (124,113,114,83,109-112; at most 8 hops) to an OpLoad (61) of
//    an access chain with exactly one OpConstant index on a DescriptorSet-0 Uniform variable -> (Binding, member Offset);
//  - any other outcome marks the module unknown.
// Conservative cases (each only ever adds `unknown`; none fires on a 2048-module shader corpus):
//  - a missing Binding decoration or a value above 0xffff is untraceable;
//  - the whole image array used other than as an access-chain base (OpLoad/OpStore/OpCopyMemory*/OpCopyObject/OpBitcast/
//    OpPtrAccessChain/OpFunctionCall argument/OpSelect/OpPhi, or an access chain without index) counts as an escape;
//  - OpGroupDecorate/OpGroupMemberDecorate (decorations this scan does not follow) make the module unparsed.
// Cost: one id table (16 bytes x Bound; corpus max Bound 1028) and two linear passes; member offsets are looked up lazily
// in the annotation range. Never throws (a failed allocation leaves unknown=true, valid=false).
inline void scanTextureRefs(const std::uint32_t* code,std::size_t words,TexScan& out,TexScanCounts* counts=nullptr) noexcept {
    out=TexScan{};out.unknown=true;
    if(counts)*counts=TexScanCounts{};
    if(!code||words<5||words>UINT32_MAX||code[0]!=0x07230203u)return;
    const std::uint32_t bound=code[3];
    if(bound<2||bound>(1u<<22))return;
    std::vector<detail::SpvId> ids;
    try{ids.resize(bound);}catch(...){return;}
    std::size_t annotBegin=words,annotEnd=0;
    // Pass 1: instruction framing, decorations, definitions of the ids the trace follows.
    for(std::size_t i=5;i<words;){
        const std::uint32_t count=code[i]>>16,op=code[i]&0xffffu;
        if(!count||count>words-i)return; // truncated / malformed
        const std::uint32_t* a=code+i+1;const std::uint32_t n=count-1;
        std::uint32_t id=0;
        switch(op){
        case 71: // OpDecorate target decoration literal
            if(n>=3&&a[0]<bound){if(a[1]==34u)ids[a[0]].set=a[2];else if(a[1]==33u)ids[a[0]].binding=a[2];}
            annotBegin=std::min(annotBegin,i);annotEnd=std::max(annotEnd,i+count);break;
        case 72: // OpMemberDecorate (Offset looked up lazily)
            annotBegin=std::min(annotBegin,i);annotEnd=std::max(annotEnd,i+count);break;
        case 74:case 75:return; // OpGroupDecorate / OpGroupMemberDecorate
        case 21:case 22:case 23:case 24:case 25:case 26:case 27:case 28:case 29:case 30:case 32: // OpType* (result id first)
            if(n>=1)id=a[0];break;
        case 43:case 59:case 61:case 65:case 66:case 83:case 109:case 110:case 111:case 112:case 113:case 114:case 124:
            if(n>=2)id=a[1];break; // result type, result id
        default:break;
        }
        if(id){if(id>=bound)return;ids[id].def=static_cast<std::uint32_t>(i);}
        i+=count;
    }
    struct Def {std::uint32_t op{},n{};const std::uint32_t* a{};};
    const auto def=[&](std::uint32_t id) noexcept {
        Def d{};
        if(id&&id<bound&&ids[id].def){const std::uint32_t at=ids[id].def;d.op=code[at]&0xffffu;d.n=(code[at]>>16)-1u;d.a=code+at+1;}
        return d;};
    const auto memberOffset=[&](std::uint32_t type,std::uint32_t member,std::uint32_t& offset) noexcept {
        bool found=false;
        for(std::size_t i=annotBegin;i<annotEnd;){
            const std::uint32_t count=code[i]>>16,op=code[i]&0xffffu;const std::uint32_t* a=code+i+1;
            if(op==72u&&count>=5u&&a[0]==type&&a[1]==member&&a[2]==35u){offset=a[3];found=true;} // last one wins
            i+=count;
        }
        return found;};
    // Image-array variables (flag per id; only OpVariable ids can be set).
    for(std::uint32_t v=1;v<bound;++v){
        const Def var=def(v);
        if(var.op!=59u||var.n<3||var.a[2]!=0u)continue;                // OpVariable, UniformConstant
        if(ids[v].set==0u||ids[v].set==UINT32_MAX)continue;            // DescriptorSet != 0 (absent counts as 0)
        const Def ptr=def(var.a[0]);if(ptr.op!=32u||ptr.n<3)continue;  // OpTypePointer
        const Def arr=def(ptr.a[2]);if((arr.op!=28u&&arr.op!=29u)||arr.n<2)continue;
        ids[v].texArray=def(arr.a[1]).op==25u;                          // of OpTypeImage
    }
    const auto isTex=[&](std::uint32_t id) noexcept {return id<bound&&ids[id].texArray;};
    const auto trace=[&](std::uint32_t idx,TexRef& ref) noexcept {
        for(unsigned depth=0;depth<=8;++depth){ // trace(idx,depth) gives up at depth > 8
            const Def d=def(idx);
            switch(d.op){
            case 124:case 113:case 114:case 83:case 112:case 111:case 110:case 109:
                if(d.n<3)return false;
                idx=d.a[2];continue;
            case 61:{ // OpLoad type id pointer
                if(d.n<3)return false;
                const Def chain=def(d.a[2]);
                if((chain.op!=65u&&chain.op!=66u)||chain.n!=4u)return false;              // access chain with exactly one index
                const std::uint32_t base=chain.a[2];const Def var=def(base);
                if(var.op!=59u||var.n<3||var.a[2]!=2u||ids[base].set!=0u)return false;  // DescriptorSet-0 Uniform variable
                const Def member=def(chain.a[3]);if(member.op!=43u||member.n<3)return false; // OpConstant member index
                const Def ptr=def(var.a[0]);if(ptr.op!=32u||ptr.n<3)return false;
                std::uint32_t offset=0;
                if(!memberOffset(ptr.a[2],member.a[2],offset))return false;
                const std::uint32_t binding=ids[base].binding;
                if(binding>0xffffu||offset>0xffffu)return false;                            // also a missing Binding
                ref=TexRef{static_cast<std::uint16_t>(binding),static_cast<std::uint16_t>(offset)};
                return true;}
            default:return false;
            }
        }
        return false;};
    bool overflow=false;std::uint32_t accesses=0,unknownAccesses=0,escapes=0;
    const auto add=[&](TexRef r) noexcept {
        for(unsigned k=0;k<out.count;++k)if(out.refs[k].binding==r.binding&&out.refs[k].offset==r.offset)return;
        if(out.count>=out.refs.size()){overflow=true;return;}
        out.refs[out.count]=r;++out.count;};
    // Pass 2: accesses and escapes.
    for(std::size_t i=5;i<words;){
        const std::uint32_t count=code[i]>>16,op=code[i]&0xffffu;const std::uint32_t* a=code+i+1;const std::uint32_t n=count-1;
        i+=count;
        switch(op){
        case 65:case 66: // OpAccessChain / OpInBoundsAccessChain type id base index...
            if(n>=4&&isTex(a[2])){++accesses;TexRef r{};if(trace(a[3],r))add(r);else ++unknownAccesses;}
            else if(n==3&&isTex(a[2]))++escapes;
            break;
        case 61:case 67:case 70:case 83:case 124: // OpLoad / OpPtrAccessChain / OpInBoundsPtrAccessChain / OpCopyObject / OpBitcast
            if(n>=3&&isTex(a[2]))++escapes;
            break;
        case 62:case 63:case 64: // OpStore / OpCopyMemory / OpCopyMemorySized
            if(n>=2&&(isTex(a[0])||isTex(a[1])))++escapes;
            break;
        case 57: // OpFunctionCall type id function args...
            for(std::uint32_t k=3;k<n;++k)if(isTex(a[k]))++escapes;
            break;
        case 169: // OpSelect type id condition a b
            if(n>=5&&(isTex(a[3])||isTex(a[4])))++escapes;
            break;
        case 245: // OpPhi type id (value parent)*
            for(std::uint32_t k=2;k<n;k+=2)if(isTex(a[k]))++escapes;
            break;
        default:break;
        }
    }
    std::sort(out.refs.begin(),out.refs.begin()+out.count,
              [](TexRef x,TexRef y){return x.binding!=y.binding?x.binding<y.binding:x.offset<y.offset;});
    out.valid=true;out.unknown=overflow||unknownAccesses||escapes;
    if(counts)*counts=TexScanCounts{accesses,unknownAccesses,escapes};
}

// Per replayed pass (SkeletonEntry::mergeBits): a draw samples an attachment of the current framebuffer / a check
// was inconclusive (unknown pipeline, non-dynamic binding, ring out of range).
enum PassBits : std::uint8_t {BitSamples=1u,BitUnknown=2u};
// Decision per pass entry k (boundary between pass k-1 and k). Merge = no End/Begin before pass k; First = out[0];
// every other value keeps the boundary and names the first failed rule (report key in keepName).
enum class Keep : std::uint8_t {Merge,First,Twin,Fb,Pass,Area,Loads,Stores,Gap,Sample,SampleUnknown,Cap,Count};
constexpr const char* keepName(Keep k) noexcept {
    switch(k){case Keep::Merge:return "merge";case Keep::First:return "first";case Keep::Twin:return "twin";case Keep::Fb:return "fb";
    case Keep::Pass:return "pass";case Keep::Area:return "area";case Keep::Loads:return "loads";case Keep::Stores:return "stores";
    case Keep::Gap:return "gap";case Keep::Sample:return "sample";case Keep::SampleUnknown:return "sampleUnknown";case Keep::Cap:return "cap";
    default:return "?";}
}
struct PassInfo {
    VkFramebuffer fb{};       // resolved right framebuffer (M2)
    VkRenderPass rp{};        // render pass handle (M3)
    VkRect2D area{};          // renderArea (M4)
    bool loadsAll{},storesAll{}; // PassLoadsAll (M5, this pass) / PassStoresAll (M6, the previous pass)
    std::uint32_t draws{};    // draws recorded in this pass (M9)
    std::uint8_t bits{};      // PassBits (M8)
    bool gapDroppable{},special{}; // every entry since the previous pass is droppable (M7) / SPLIT or EXTRA twin entry (M1)
};
constexpr unsigned defaultCap=1400;            // M9 default: never more than ~1400 draws in one merged pass
constexpr unsigned minCap=50,maxCap=1450;      // clamp of debug.rdrvr.stereoreplay.mergecap
// out[0..n-1]: out[0]=First; for k>0 the first failed rule in the order M1..M9 (or Merge):
//   M1 Twin    pass k-1 or k is a SPLIT/EXTRA entry (special)
//   M2 Fb      different framebuffer, or either one VK_NULL_HANDLE (unresolved)
//   M3 Pass    different render pass handle, or either one VK_NULL_HANDLE
//   M4 Area    different renderArea (offset and extent)
//   M5 Loads   pass k does not load every attachment
//   M6 Stores  pass k-1 does not store every attachment
//   M7 Gap     an entry between pass k-1 and k is not droppable (pass k's gapDroppable)
//   M8 Sample / SampleUnknown   pass k-1 or k has a draw sampling an attachment / an inconclusive check (Sample wins).
//                               k-1 counts too: a later write into the subpass that sampled the attachment
//                               is a feedback loop (VUID-vkCmdDraw-None-12338/12339); the boundary's
//                               store/load separates them.
//   M9 Cap     the open group's draws + draws(k) > cap
// The group draw counter restarts at every kept boundary, so a pass with more draws than the cap stays alone.
// cap 0 means defaultCap; any other value is used as given (the caller clamps the property to minCap..maxCap).
inline void decide(const PassInfo* passes,unsigned n,unsigned cap,Keep* out) noexcept {
    if(!passes||!out||!n)return;
    if(!cap)cap=defaultCap;
    out[0]=Keep::First;
    std::uint64_t group=passes[0].draws;
    for(unsigned k=1;k<n;++k){
        const PassInfo& a=passes[k-1];const PassInfo& b=passes[k];
        Keep r=Keep::Merge;
        if(a.special||b.special)r=Keep::Twin;
        else if(a.fb!=b.fb||a.fb==VK_NULL_HANDLE)r=Keep::Fb;
        else if(a.rp!=b.rp||a.rp==VK_NULL_HANDLE)r=Keep::Pass;
        else if(a.area.offset.x!=b.area.offset.x||a.area.offset.y!=b.area.offset.y
                ||a.area.extent.width!=b.area.extent.width||a.area.extent.height!=b.area.extent.height)r=Keep::Area;
        else if(!b.loadsAll)r=Keep::Loads;
        else if(!a.storesAll)r=Keep::Stores;
        else if(!b.gapDroppable)r=Keep::Gap;
        else if((a.bits|b.bits)&BitSamples)r=Keep::Sample;         // The sampling pass ends its group too
        else if((a.bits|b.bits)&BitUnknown)r=Keep::SampleUnknown;
        else if(group+b.draws>cap)r=Keep::Cap;
        out[k]=r;
        group=r==Keep::Merge?group+b.draws:b.draws;
    }
}
// Draws per merged group of a decide() result (a group starts at every out[k] != Merge). Writes at most `max` sizes and
// returns the total group count (lastGroups / groupHist / maxGroupDraws feed).
inline unsigned groupSizes(const PassInfo* passes,const Keep* decisions,unsigned n,std::uint32_t* sizes,unsigned max) noexcept {
    if(!passes||!decisions||!n)return 0;
    unsigned groups=0;std::uint64_t open=0;
    for(unsigned k=0;k<n;++k){
        if(k&&decisions[k]!=Keep::Merge){if(sizes&&groups<max)sizes[groups]=static_cast<std::uint32_t>(std::min<std::uint64_t>(open,UINT32_MAX));++groups;open=0;}
        open+=passes[k].draws;
    }
    if(sizes&&groups<max)sizes[groups]=static_cast<std::uint32_t>(std::min<std::uint64_t>(open,UINT32_MAX));
    return groups+1;
}
// M7 for one gap PipelineBarrier: no memory or buffer barriers, and every image barrier is a layout no-op
// (oldLayout == newLayout, no queue-family transfer) on either one of the own (left framebuffer attachment) images or
// an image that emitOutsideOp drops anyway: kept(image,ctx) is the existing eye/foreign test (true = emitOutsideOp
// would emit a barrier for that image, i.e. an eye image in maps or a foreign target) and must return false.
// kept == nullptr: only own images are droppable. n == 0 with no memory/buffer barriers (an execution-only barrier,
// which emitOutsideOp also drops) is droppable.
inline bool barrierDroppable(const VkImageMemoryBarrier* barriers,std::uint32_t n,std::uint32_t memory,std::uint32_t buffers,
                             const VkImage* own,unsigned nOwn,bool (*kept)(VkImage,const void*),const void* ctx) noexcept {
    if(memory||buffers)return false;
    if(n&&!barriers)return false;
    for(std::uint32_t i=0;i<n;++i){
        const VkImageMemoryBarrier& b=barriers[i];
        if(b.oldLayout!=b.newLayout)return false;
        if(b.srcQueueFamilyIndex!=b.dstQueueFamilyIndex)return false; // an ownership transfer is never a no-op
        if(b.image==VK_NULL_HANDLE)return false;
        bool isOwn=false;
        if(own)for(unsigned k=0;k<nOwn;++k)if(own[k]==b.image){isOwn=true;break;}
        if(isOwn)continue;
        if(!kept||kept(b.image,ctx))return false;
    }
    return true;
}
// groupHist buckets: <=49, 50-399, 400-799, 800-1199, 1200..cap draws per merged group.
constexpr unsigned groupHistBuckets=5;
constexpr unsigned groupHistBucket(std::uint32_t draws) noexcept {return draws<=49?0u:draws<=399?1u:draws<=799?2u:draws<=1199?3u:4u;}
// Report stereoReplay.boundaryTest.merge. Writers: render thread / worker (relaxed atomics).
struct Stats {
    static constexpr unsigned lastMax=16;
    std::atomic<std::uint64_t> jobs{},passes{},boundaries{},merged{},groups{},samplingPasses{},ringReads{},aliasExtra{};
    std::atomic<std::uint32_t> maxGroupDraws{};
    std::array<std::atomic<std::uint64_t>,groupHistBuckets> groupHist{};
    std::array<std::atomic<std::uint64_t>,unsigned(Keep::Count)> kept{}; // indexed by Keep (Merge/First unused)
    std::atomic<std::uint64_t> keptInline{},keptIndexChanged{},keptNoIndex{}; // kept.inline / indexChanged / noIndex
    std::array<std::atomic<std::uint32_t>,lastMax> lastGroups{},lastSampling{},attachIdx{}; // last frame
    std::atomic<std::uint32_t> lastGroupCount{},lastSamplingCount{},attachIdxCount{};
    // per pass of the last merging job, its sampling bits (0 none, 1 samples an attachment, 2 unknown, 3 both)
    static constexpr unsigned passBitsMax=32;
    std::array<std::atomic<std::uint8_t>,passBitsMax> lastPassBits{};std::atomic<std::uint32_t> lastPassCount{};
    struct Scan {std::atomic<std::uint64_t> modules{},withRefs{},unknown{},pipelinesOverflow{};std::atomic<std::uint32_t> maxRefs{};} scan;
    void noteKept(Keep k) noexcept {if(k!=Keep::Merge&&k!=Keep::First&&k<Keep::Count)kept[unsigned(k)].fetch_add(1,std::memory_order_relaxed);}
    void noteGroup(std::uint32_t draws) noexcept {
        groups.fetch_add(1,std::memory_order_relaxed);groupHist[groupHistBucket(draws)].fetch_add(1,std::memory_order_relaxed);
        auto seen=maxGroupDraws.load(std::memory_order_relaxed);
        while(draws>seen&&!maxGroupDraws.compare_exchange_weak(seen,draws,std::memory_order_relaxed)){}
    }
    // scan{modules, withRefs, unknown, maxRefs} for one scanned shader module (pipelinesOverflow is the caller's).
    void noteScan(const TexScan& s) noexcept {
        scan.modules.fetch_add(1,std::memory_order_relaxed);
        if(s.count)scan.withRefs.fetch_add(1,std::memory_order_relaxed);
        if(s.unknown)scan.unknown.fetch_add(1,std::memory_order_relaxed);
        const std::uint32_t refs=s.count;auto seen=scan.maxRefs.load(std::memory_order_relaxed);
        while(refs>seen&&!scan.maxRefs.compare_exchange_weak(seen,refs,std::memory_order_relaxed)){}
    }
    std::string json() const {
        const auto u=[](std::uint64_t v){return std::to_string(v);};
        const auto list=[](const std::array<std::atomic<std::uint32_t>,lastMax>& a,std::uint64_t n,bool quoted){
            std::string s=quoted?"\"":"[";
            for(unsigned i=0;i<n&&i<lastMax;++i){if(i)s+=',';s+=std::to_string(a[i].load(std::memory_order_relaxed));}
            return s+(quoted?"\"":"]");};
        const auto ld=[](const auto& v){return static_cast<std::uint64_t>(v.load(std::memory_order_relaxed));};
        std::string o="{\"jobs\":"+u(ld(jobs))+",\"passes\":"+u(ld(passes))+",\"boundaries\":"+u(ld(boundaries))+",\"merged\":"+u(ld(merged))
            +",\"groups\":"+u(ld(groups))+",\"maxGroupDraws\":"+u(ld(maxGroupDraws))+",\"groupHist\":[";
        for(unsigned i=0;i<groupHistBuckets;++i){if(i)o+=',';o+=u(ld(groupHist[i]));}
        o+="],\"kept\":{";
        for(unsigned k=unsigned(Keep::Twin);k<unsigned(Keep::Count);++k){o+='"';o+=keepName(Keep(k));o+="\":"+u(ld(kept[k]))+",";}
        o+="\"inline\":"+u(ld(keptInline))+",\"indexChanged\":"+u(ld(keptIndexChanged))+",\"noIndex\":"+u(ld(keptNoIndex))+"}"
            +",\"samplingPasses\":"+u(ld(samplingPasses))+",\"ringReads\":"+u(ld(ringReads))
            +",\"lastGroups\":"+list(lastGroups,ld(lastGroupCount),true)+",\"lastSampling\":"+list(lastSampling,ld(lastSamplingCount),true)
            +",\"attachIdx\":"+list(attachIdx,ld(attachIdxCount),false)+",\"aliasExtra\":"+u(ld(aliasExtra))
            +",\"lastPassBits\":\""+[&]{std::string b;const auto n=ld(lastPassCount);for(unsigned i=0;i<n&&i<passBitsMax;++i)b+=char('0'+(lastPassBits[i].load(std::memory_order_relaxed)&3u));return b;}()+"\""
            +",\"scan\":{\"modules\":"+u(ld(scan.modules))+",\"withRefs\":"+u(ld(scan.withRefs))+",\"unknown\":"+u(ld(scan.unknown))
            +",\"maxRefs\":"+u(ld(scan.maxRefs))+",\"pipelinesOverflow\":"+u(ld(scan.pipelinesOverflow))+"}}";
        return o;
    }
};
}
