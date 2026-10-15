#pragma once
// Stereo replay: vertex-shader clip-space correction.
//
// Every retail RDR vertex shader writes gl_Position (a plain Output variable
// decorated BuiltIn Position, stored exactly by OpStore) from the native
// rage_matrices block (set 0, binding 1: gWorldSingle, gWorldView,
// gWorldViewProj, gViewInverse). The LEFT eye is drawn natively with those
// matrices. The RIGHT eye is produced by replaying the same Vulkan commands
// with identical per-draw constants; only clip space must change. Because
// clip = P*V*world is linear, clipR = C * clipL with
//   C = relClipR * T(cL - cR) * inverse(relClipL)
// is exact for every shader whose Position is any linear chain of those
// matrices. This transform appends `Position = pc.m * Position` to every
// OpStore into the Position variable, reading `m` from a 64-byte vertex-stage
// push constant block. Native draws push identity; the replay pushes C.
//
// The transform is purely structural: one entry point (Vertex), no function
// calls, SPIR-V 1.3. Any unexpected structure declines and the module is left
// unchanged (the caller keeps the native bytes).
#include <cstdint>
#include <cstring>
#include <vector>

namespace rdrvr::engine::stereo {
enum class PatchStatus {
    Patched, NotVertex, NoPositionVariable, NoCameraMatrices, Invalid, MultipleEntryPoints,
    HasFunctionCalls, HasPushConstants, NoStores, TooLarge
};
struct PatchReport {
    PatchStatus status{PatchStatus::Invalid};
    std::uint32_t positionVariable{},stores{},cameraMembers{},set0Bindings{},defSiteValues{},usesRewritten{},storeFallbacks{};
    bool createdVec4{},createdMat4{},createdFloat{},createdInt{};
};
namespace detail {
enum Op : std::uint32_t {
    OpName=5,OpEntryPoint=15,OpTypeInt=21,OpTypeFloat=22,OpTypeVector=23,OpTypeMatrix=24,
    OpTypeStruct=30,OpTypePointer=32,OpTypeFunction=33,OpConstant=43,OpFunction=54,OpFunctionCall=57,
    OpVariable=59,OpLoad=61,OpStore=62,OpAccessChain=65,OpDecorate=71,OpMemberDecorate=72,
    OpMatrixTimesVector=145,OpTypeImage=25,OpTypeSampler=26,OpTypeSampledImage=27,OpTypeArray=28,
    OpTypeRuntimeArray=29,OpTypeVoid=19,OpTypeBool=20,OpConstantTrue=41,OpConstantFalse=42,
    OpConstantComposite=44,OpConstantNull=46,OpSpecConstant=50,OpSpecConstantTrue=48,OpSpecConstantFalse=49,
    OpSpecConstantComposite=51,OpSpecConstantOp=52,OpUndef=1,OpExtInstImport=11,OpMemoryModel=14,
    OpExecutionMode=16,OpCapability=17,OpExtension=10,OpSource=3,OpSourceExtension=4,OpString=7,
    OpMemberName=6,OpLine=8,OpPhi=245,OpLabel=248,OpDecorationGroup=73,OpGroupDecorate=74,OpGroupMemberDecorate=75,OpModuleProcessed=330
};
constexpr std::uint32_t StorageClassPushConstant=9,StorageClassOutput=3;
constexpr std::uint32_t DecorationBlock=2,DecorationColMajor=5,DecorationMatrixStride=7,DecorationBuiltIn=11,
    DecorationBinding=33,DecorationDescriptorSet=34,DecorationOffset=35;
constexpr std::uint32_t BuiltInPosition=0,ExecutionModelVertex=0;
inline std::uint32_t word(std::uint32_t op,std::uint32_t count){return (count<<16)|op;}
inline bool isTypeOrConstant(std::uint32_t op){
    switch(op){
    case OpTypeVoid:case OpTypeBool:case OpTypeInt:case OpTypeFloat:case OpTypeVector:case OpTypeMatrix:case OpTypeImage:
    case OpTypeSampler:case OpTypeSampledImage:case OpTypeArray:case OpTypeRuntimeArray:case OpTypeStruct:case OpTypePointer:
    case OpTypeFunction:case OpConstantTrue:case OpConstantFalse:case OpConstant:case OpConstantComposite:case OpConstantNull:
    case OpSpecConstantTrue:case OpSpecConstantFalse:case OpSpecConstant:case OpSpecConstantComposite:case OpSpecConstantOp:
    case OpVariable:case OpUndef:return true;
    default:return false;
    }
}
}
// source: SPIR-V words (header included). output receives the patched module on
// Patched; it is left unchanged otherwise. source may not alias output.
inline PatchStatus patchEyePosition(const std::uint32_t* source,std::size_t words,std::vector<std::uint32_t>& output,PatchReport& report) noexcept {
    using namespace detail;
    report=PatchReport{};
    try {
        if(!source||words<5||words>(1u<<20))return report.status=PatchStatus::TooLarge;
        if(source[0]!=0x07230203u)return report.status=PatchStatus::Invalid;
        std::uint32_t bound=source[3];
        if(!bound||bound>1000000)return report.status=PatchStatus::Invalid;
        // Pass 1: structure scan.
        struct Inst {std::size_t at;std::uint32_t op,count;};
        std::vector<Inst> insts;insts.reserve(words/4+8);
        std::size_t i=5;
        while(i<words){
            const std::uint32_t count=source[i]>>16,op=source[i]&0xffffu;
            if(!count||i+count>words)return report.status=PatchStatus::Invalid;
            insts.push_back({i,op,count});i+=count;
        }
        unsigned entryPoints=0;bool vertex=false;std::uint32_t positionVar=0,positionType=0;
        std::uint32_t floatType=0,vec4Type=0,mat4Type=0,int32Type=0;
        std::uint32_t cameraVar=0;bool cameraSet0=false,cameraBinding1=false;
        std::uint32_t firstTypeIndex=UINT32_MAX,firstFunctionIndex=UINT32_MAX,firstAnnotationIndex=UINT32_MAX;
        std::vector<std::pair<std::uint32_t,std::uint32_t>> dsDecor,bindDecor; // (id, value)
        std::vector<std::uint32_t> constZeroCandidates; // ids of integer constant 0 (any int type)
        struct VarInfo {std::uint32_t id,type,storage;};
        std::vector<VarInfo> vars;
        std::vector<std::pair<std::uint32_t,std::uint32_t>> pointerTypes; // (ptrTypeId, pointee) with storage
        std::vector<std::uint32_t> pointerStorage;
        for(std::size_t k=0;k<insts.size();++k){
            const auto& in=insts[k];const auto* a=source+in.at+1;const std::uint32_t n=in.count-1;
            switch(in.op){
            case OpEntryPoint:++entryPoints;if(n>=1&&a[0]==ExecutionModelVertex)vertex=true;break;
            case OpFunction:if(firstFunctionIndex==UINT32_MAX)firstFunctionIndex=std::uint32_t(k);break;
            case OpFunctionCall:return report.status=PatchStatus::HasFunctionCalls;
            case OpDecorate:
                if(firstAnnotationIndex==UINT32_MAX)firstAnnotationIndex=std::uint32_t(k);
                if(n>=3&&a[1]==DecorationBuiltIn&&a[2]==BuiltInPosition){if(positionVar)return report.status=PatchStatus::Invalid;positionVar=a[0];}
                if(n>=3&&a[1]==DecorationDescriptorSet)dsDecor.push_back({a[0],a[2]});
                if(n>=3&&a[1]==DecorationBinding)bindDecor.push_back({a[0],a[2]});
                break;
            case OpMemberDecorate:
                if(firstAnnotationIndex==UINT32_MAX)firstAnnotationIndex=std::uint32_t(k);
                if(n>=4&&a[2]==DecorationBuiltIn&&a[3]==BuiltInPosition)return report.status=PatchStatus::Invalid; // gl_PerVertex form unsupported
                break;
            case OpDecorationGroup:case OpGroupDecorate:case OpGroupMemberDecorate:return report.status=PatchStatus::Invalid;
            case OpTypeFloat:if(n>=2&&a[1]==32&&!floatType)floatType=a[0];break;
            case OpTypeInt:if(n>=3&&a[1]==32&&!int32Type)int32Type=a[0];break;
            case OpTypeVector:if(n>=3&&a[1]==floatType&&floatType&&a[2]==4&&!vec4Type)vec4Type=a[0];break;
            case OpTypeMatrix:if(n>=3&&vec4Type&&a[1]==vec4Type&&a[2]==4&&!mat4Type)mat4Type=a[0];break;
            case OpTypePointer:if(n>=3){pointerTypes.push_back({a[0],a[2]});pointerStorage.push_back(a[1]);}break;
            case OpConstant:
                // integer constant zero of a 32-bit int type (signed or unsigned)
                if(n>=3&&a[2]==0){constZeroCandidates.push_back(a[1]);constZeroCandidates.push_back(a[0]);}
                break;
            case OpVariable:
                if(n>=3){vars.push_back({a[1],a[0],a[2]});if(a[2]==StorageClassPushConstant)return report.status=PatchStatus::HasPushConstants;}
                break;
            default:break;
            }
            if(firstTypeIndex==UINT32_MAX&&isTypeOrConstant(in.op))firstTypeIndex=std::uint32_t(k);
        }
        if(entryPoints!=1)return report.status=PatchStatus::MultipleEntryPoints;
        if(!vertex)return report.status=PatchStatus::NotVertex;
        if(!positionVar)return report.status=PatchStatus::NoPositionVariable;
        if(firstTypeIndex==UINT32_MAX||firstFunctionIndex==UINT32_MAX||firstFunctionIndex<firstTypeIndex)return report.status=PatchStatus::Invalid;
        if(firstAnnotationIndex==UINT32_MAX)firstAnnotationIndex=firstTypeIndex; // no annotations at all: insert before types
        // Position variable: Output storage, pointer to vec4.
        for(const auto& v:vars)if(v.id==positionVar){
            if(v.storage!=StorageClassOutput)return report.status=PatchStatus::Invalid;
            positionType=v.type;
        }
        if(!positionType)return report.status=PatchStatus::Invalid;
        {
            bool ok=false;
            for(std::size_t p=0;p<pointerTypes.size();++p)if(pointerTypes[p].first==positionType&&pointerStorage[p]==StorageClassOutput&&pointerTypes[p].second==vec4Type&&vec4Type)ok=true;
            if(!ok)return report.status=PatchStatus::Invalid;
        }
        // Camera block: a variable with DescriptorSet 0 and Binding 1.
        for(const auto& v:vars){
            bool s0=false,b1=false;
            for(const auto& d:dsDecor)if(d.first==v.id&&d.second==0)s0=true;
            for(const auto& b:bindDecor)if(b.first==v.id&&b.second==1)b1=true;
            if(s0&&b1){cameraVar=v.id;cameraSet0=s0;cameraBinding1=b1;}
        }
        for(const auto& v:vars){bool s0=false;for(const auto& d:dsDecor)if(d.first==v.id&&d.second==0)s0=true;
            if(s0)for(const auto& b:bindDecor)if(b.first==v.id&&b.second<32)report.set0Bindings|=1u<<b.second;}
        if(!cameraVar||!cameraSet0||!cameraBinding1)return report.status=PatchStatus::NoCameraMatrices;
        // Which members are read: only shaders using members 0/1/2 (world/clip
        // chains) get the correction. gViewInverse-only shaders keep NDC output.
        // Member index constants: resolve OpConstant values.
        std::uint32_t memberMask=0;
        for(const auto& in:insts){
            if(in.op!=OpAccessChain||in.count<5)continue;
            const auto* a=source+in.at+1;
            if(a[2]!=cameraVar)continue;
            const std::uint32_t indexId=a[3];
            for(const auto& c:insts)if(c.op==OpConstant&&c.count>=4&&source[c.at+2]==indexId){const auto value=source[c.at+3];if(value<32)memberMask|=1u<<value;}
        }
        report.cameraMembers=memberMask;
        if(!(memberMask&0x7u))return report.status=PatchStatus::NoCameraMatrices;
        // Stores into Position (direct only; component access chains unsupported).
        std::uint32_t stores=0;
        for(const auto& in:insts){
            if(in.op==OpAccessChain&&in.count>=4&&source[in.at+3]==positionVar)return report.status=PatchStatus::Invalid;
            if(in.op==OpStore&&in.count>=3&&source[in.at+1]==positionVar)++stores;
        }
        if(!stores)return report.status=PatchStatus::NoStores;
        report.stores=stores;report.positionVariable=positionVar;
        // Allocate new ids.
        auto fresh=[&]{return bound++;};
        std::uint32_t newFloat=floatType,newVec4=vec4Type,newMat4=mat4Type,newInt=int32Type,newZero=0;
        if(!newFloat){newFloat=fresh();report.createdFloat=true;}
        if(!newVec4){newVec4=fresh();report.createdVec4=true;}
        if(!newMat4){newMat4=fresh();report.createdMat4=true;}
        if(!newInt){newInt=fresh();report.createdInt=true;}
        // Prefer an existing constant 0 of the chosen int type.
        for(std::size_t c=0;c+1<constZeroCandidates.size();c+=2)if(constZeroCandidates[c+1]==newInt){newZero=constZeroCandidates[c];break;}
        const bool createZero=!newZero;if(createZero)newZero=fresh();
        const std::uint32_t pcStruct=fresh(),pcPtrStruct=fresh(),pcVar=fresh(),pcPtrMat=fresh();
        // Emit.
        output.clear();output.reserve(words+64+stores*16);
        for(std::size_t h=0;h<5;++h)output.push_back(source[h]);
        output[3]=bound;
        auto emit=[&](std::initializer_list<std::uint32_t> ws){const std::uint32_t op=*ws.begin();std::size_t n=ws.size();
            output.push_back(word(op,std::uint32_t(n)));bool first=true;for(auto w:ws){if(first){first=false;continue;}output.push_back(w);}};
        // Definition sites of every value stored to Position (inside the function body).
        // Ops with (result type, result id) layout: all value-producing ops used in vertex shaders.
        auto resultId=[&](const Inst& in)->std::uint32_t{
            if(in.count<3)return 0;const auto op=in.op;
            if(op==OpStore||op==OpDecorate||op==OpMemberDecorate||op==OpName||op==OpMemberName||op==OpEntryPoint||op==OpLine||op==OpLabel||op==OpFunction||op==OpVariable)return 0;
            if(op>=19&&op<=39)return 0; // OpTypeXXX
            return source[in.at+2];};
        std::vector<std::uint32_t> posValues;
        for(const auto& in:insts)if(in.op==OpStore&&in.count>=3&&source[in.at+1]==positionVar){const auto v=source[in.at+2];if(std::find(posValues.begin(),posValues.end(),v)==posValues.end())posValues.push_back(v);}
        struct DefSite {std::uint32_t value,corrected;std::size_t index;};
        std::vector<DefSite> defSites;std::vector<std::uint32_t> storeOnlyValues;
        for(const auto v:posValues){
            std::size_t found=SIZE_MAX;
            for(std::size_t k=firstFunctionIndex;k<insts.size();++k)if(resultId(insts[k])==v){found=k;break;}
            if(found==SIZE_MAX){storeOnlyValues.push_back(v);continue;}
            // insert after the phi group when the definition is a phi
            std::size_t at=found;if(insts[at].op==OpPhi)while(at+1<insts.size()&&insts[at+1].op==OpPhi)++at;
            defSites.push_back({v,fresh(),at});
        }
        report.defSiteValues=std::uint32_t(defSites.size());report.storeFallbacks=std::uint32_t(storeOnlyValues.size());
        auto correctedFor=[&](std::uint32_t v)->std::uint32_t{for(const auto& d:defSites)if(d.value==v)return d.corrected;return 0;};
        // Is word w of instruction `in` an id operand that may carry a value (never a literal, type, result or label)?
        auto idOperand=[&](const Inst& in,std::uint32_t w,std::uint32_t rid)->bool{
            const auto op=in.op;
            if(op==OpVariable||op==OpLabel||op==OpFunction||op==OpLine||op==OpDecorate||op==OpName)return false;
            if(rid){if(w<=2)return false;}
            switch(op){
            case 81:return w==3;                 // OpCompositeExtract: composite, then literal indices
            case 82:return w==3||w==4;           // OpCompositeInsert: object, composite, literals
            case 79:return w==3||w==4;           // OpVectorShuffle: v1, v2, literals
            case 12:return w>=5;                 // OpExtInst: set, literal instruction, operands
            case OpLoad:return w==3;             // ptr, [memory access literal]
            case OpStore:return w==2;            // ptr, value, [memory access literal]
            case 250:return w==1;                // OpSwitch: selector, default label, literal/label pairs
            case 252:return w==1;                // OpBranchConditional: condition, labels, [weights]
            case 246:case 247:return false;      // OpLoopMerge / OpSelectionMerge
            case OpPhi:return w>=3&&((w-3)%2==0);// (value,label) pairs
            case 87:case 88:case 89:case 90:case 95:case 96:case 100:return w==3||w==4; // image sample/fetch: image, coordinate, [mask...]
            case 97:return w==3||w==4;           // OpImageGather
            case 98:case 99:return w==3||w==4;   // OpImageRead/Write
            case 191:return w==3;                // OpArrayLength: struct, literal
            default:return true;
            }};
        for(std::size_t k=0;k<insts.size();++k){
            if(k==firstAnnotationIndex){
                emit({OpDecorate,pcStruct,DecorationBlock});
                emit({OpMemberDecorate,pcStruct,0,DecorationOffset,0});
                emit({OpMemberDecorate,pcStruct,0,DecorationColMajor});
                emit({OpMemberDecorate,pcStruct,0,DecorationMatrixStride,16});
            }
            if(k==firstFunctionIndex){
                if(report.createdFloat)emit({OpTypeFloat,newFloat,32});
                if(report.createdVec4)emit({OpTypeVector,newVec4,newFloat,4});
                if(report.createdMat4)emit({OpTypeMatrix,newMat4,newVec4,4});
                if(report.createdInt)emit({OpTypeInt,newInt,32,1});
                if(createZero)emit({OpConstant,newInt,newZero,0});
                emit({OpTypeStruct,pcStruct,newMat4});
                emit({OpTypePointer,pcPtrStruct,StorageClassPushConstant,pcStruct});
                emit({OpVariable,pcPtrStruct,pcVar,StorageClassPushConstant});
                emit({OpTypePointer,pcPtrMat,StorageClassPushConstant,newMat4});
            }
            const auto& in=insts[k];
            if(in.op==OpStore&&in.count>=3&&source[in.at+1]==positionVar&&std::find(storeOnlyValues.begin(),storeOnlyValues.end(),source[in.at+2])!=storeOnlyValues.end()){
                // Fallback (value defined outside the function, e.g. a constant): correct at the store.
                const std::uint32_t value=source[in.at+2];
                const std::uint32_t chain=fresh(),matrix=fresh(),corrected=fresh();
                emit({OpAccessChain,pcPtrMat,chain,pcVar,newZero});
                emit({OpLoad,newMat4,matrix,chain});
                emit({OpMatrixTimesVector,newVec4,corrected,matrix,value});
                output.push_back(word(OpStore,in.count));
                output.push_back(positionVar);output.push_back(corrected);
                for(std::uint32_t extra=3;extra<in.count;++extra)output.push_back(source[in.at+extra]);
                continue;
            }
            // Copy the instruction, substituting corrected ids for operands (not result ids) inside the function.
            const bool inFunction=k>firstFunctionIndex;
            const std::uint32_t rid=inFunction?resultId(in):0;
            output.push_back(source[in.at]);
            for(std::uint32_t w=1;w<in.count;++w){
                std::uint32_t v=source[in.at+w];
                if(inFunction&&!defSites.empty()&&idOperand(in,w,rid)){const auto c=correctedFor(v);if(c){v=c;++report.usesRewritten;}}
                output.push_back(v);
            }
            for(const auto& d:defSites)if(d.index==k){
                const std::uint32_t chain=fresh(),matrix=fresh();
                emit({OpAccessChain,pcPtrMat,chain,pcVar,newZero});
                emit({OpLoad,newMat4,matrix,chain});
                emit({OpMatrixTimesVector,newVec4,d.corrected,matrix,d.value});
            }
        }
        output[3]=bound;
        return report.status=PatchStatus::Patched;
    }catch(...){return report.status=PatchStatus::Invalid;}
}
inline const char* patchStatusName(PatchStatus s){
    switch(s){
    case PatchStatus::Patched:return "PATCHED";case PatchStatus::NotVertex:return "NOT_VERTEX";
    case PatchStatus::NoPositionVariable:return "NO_POSITION";case PatchStatus::NoCameraMatrices:return "NO_CAMERA";
    case PatchStatus::Invalid:return "INVALID";case PatchStatus::MultipleEntryPoints:return "MULTIPLE_ENTRY";
    case PatchStatus::HasFunctionCalls:return "FUNCTION_CALLS";case PatchStatus::HasPushConstants:return "PUSH_CONSTANTS";
    case PatchStatus::NoStores:return "NO_STORES";case PatchStatus::TooLarge:return "TOO_LARGE";
    }
    return "?";
}
}
