#include "FrozenCameraSplit.h"
#include "RelativePositionProof.h"
#include <algorithm>
#include <map>
#include <new>
#include <set>
#include <string>
#include <utility>

namespace rdrvr::engine {
namespace {
using Words = std::vector<std::uint32_t>;
using Id = std::uint32_t;
using Decorations = std::map<Id, Words>;
constexpr std::size_t maxInputWords = 512 * 1024 / 4;
constexpr std::size_t maxOutputWords = 1024 * 1024 / 4;
constexpr Id maxBound = 1'000'000;
struct Decline { CameraSplitStatus status; };
void require(bool value, CameraSplitStatus status = CameraSplitStatus::InvalidStructure) {
    if (!value) throw Decline{status};
}
void supported(bool value) { require(value, CameraSplitStatus::UnsupportedInterface); }
void access(bool value) { require(value, CameraSplitStatus::UnsupportedAccess); }
void append(Words& target, const Words& words) {
    require(words.size() <= maxOutputWords - target.size(), CameraSplitStatus::LimitExceeded);
    target.insert(target.end(), words.begin(), words.end());
}
void emit(Words& target, Id op, const Words& args) {
    require(args.size() < 65535 && args.size() + 1 <= maxOutputWords - target.size(), CameraSplitStatus::LimitExceeded);
    target.push_back((Id(args.size()) + 1) << 16 | op);
    target.insert(target.end(), args.begin(), args.end());
}
void name(Words& target, Id id, const char* value) {
    Words args{id};
    Id word = 0, shift = 0;
    for (;;) {
        const auto byte = static_cast<unsigned char>(*value++);
        word |= Id(byte) << shift;
        shift += 8;
        if (shift == 32 || !byte) { args.push_back(word); word = shift = 0; }
        if (!byte) break;
    }
    emit(target, 5, args);
}
struct Op { Id code; Words args; };
struct Type { Id code; Words args; };
struct Variable { Id id, type, storage; };
struct Constant { Id id, type, value; };
struct Module {
    std::span<const Id> source;
    std::vector<Op> ops;
    std::map<Id, Type> types;
    std::map<Id, std::string> names;
    std::map<std::pair<Id, Id>, std::string> members;
    std::map<Id, Decorations> dec;
    std::map<std::pair<Id, Id>, Decorations> memberDec;
    std::vector<Variable> variables;
    std::vector<Constant> constants; // Preserve instruction order for exact Python parity.
    std::map<Id, Id> values;
    std::set<Id> declarations, views;
    Id bound{}, stage{};
    Words interfaces;
    bool multiview{};

    void id(Id value) const { require(value && value < source[3]); }
    void declare(Id value) { id(value); require(declarations.insert(value).second); }
    // Names used by the audited layout are ASCII. Reject non-ASCII metadata
    // instead of silently accepting malformed UTF-8 as a trusted layout name.
    std::pair<std::string, std::size_t> stringAt(const Words& args, std::size_t start) {
        std::string result;
        for (std::size_t i = start; i < args.size(); ++i) {
            for (unsigned shift = 0; shift < 32; shift += 8) {
                const auto c = (args[i] >> shift) & 255;
                if (!c) {
                    if (shift < 24) require((args[i] >> (shift + 8)) == 0);
                    return {result, i + 1};
                }
                supported(c < 128);
                result.push_back(static_cast<char>(c));
            }
        }
        throw Decline{CameraSplitStatus::InvalidStructure};
    }
    explicit Module(std::span<const Id> input) : source(input) {
        require(source.size() >= 5);
        require(source.size() <= maxInputWords, CameraSplitStatus::LimitExceeded);
        require(source[0] == 0x07230203 && source[4] == 0 && source[3] != 0);
        supported(source[1] == 0x10300);
        require(source[3] <= maxBound, CameraSplitStatus::LimitExceeded);
        bound = source[3];
        unsigned entries = 0, memoryModels = 0;
        Id entryFunction = 0;
        std::set<Id> functions;
        bool inFunction = false;
        for (std::size_t p = 5; p < source.size();) {
            const Id count = source[p] >> 16, op = source[p] & 65535;
            require(count && count <= source.size() - p);
            Words a(source.begin() + p + 1, source.begin() + p + count);
            auto size = [&](std::size_t n) { require(a.size() >= n); };
            if (op == 5 || op == 6) {
                const auto start = op == 5 ? 1u : 2u;
                size(start + 1); id(a[0]);
                const auto s = stringAt(a, start); require(s.second == a.size());
                if (op == 5) require(names.emplace(a[0], s.first).second);
                else require(members.emplace(std::make_pair(a[0], a[1]), s.first).second);
            } else if (op == 15) {
                size(3); id(a[1]); supported(++entries == 1 && (a[0] == 0 || a[0] == 4));
                stage = a[0]; entryFunction = a[1]; const auto s = stringAt(a, 2);
                interfaces.assign(a.begin() + s.second, a.end());
                for (auto i : interfaces) id(i);
            } else if (op == 17) {
                require(a.size() == 1); if (a[0] == 4439) multiview = true;
            } else if (op == 14) {
                require(a.size() == 2 && ++memoryModels == 1);
                supported(a[0] == 0 && a[1] == 1);
            } else if (op == 54) {
                require(a.size() == 4 && !inFunction); id(a[1]);
                require(functions.insert(a[1]).second); inFunction = true;
            } else if (op == 56) {
                require(a.empty() && inFunction); inFunction = false;
            } else if (op >= 19 && op <= 33) {
                constexpr unsigned minima[]{1,1,3,2,3,3,8,1,2,3,2,1,2,3,2};
                size(minima[op - 19]); declare(a[0]);
                types.emplace(a[0], Type{op, Words(a.begin() + 1, a.end())});
            } else if (op == 59) {
                size(3); id(a[0]); declare(a[1]);
                variables.push_back({a[1], a[0], a[2]});
            } else if (op == 71) {
                size(2); id(a[0]);
                require(dec[a[0]].emplace(a[1], Words(a.begin() + 2, a.end())).second);
                if (a == Words{a[0], 11, 4440}) views.insert(a[0]);
            } else if (op == 72) {
                size(3); id(a[0]);
                require(memberDec[{a[0], a[1]}].emplace(a[2], Words(a.begin() + 3, a.end())).second);
            } else if (op == 73 || op == 74 || op == 75 || op == 332) {
                supported(false); // Grouped and ID-valued decorations are outside this contract.
            }
            // Every instruction indexed by the transformation has a minimum
            // length check even when unrelated to the selected uniform block.
            if (op == 43 || op == 61) size(3);
            if (op == 54 || op == 65 || op == 66 || op == 67 || op == 70 || op == 145) size(4);
            ops.push_back({op, std::move(a)}); p += count;
        }
        require(!inFunction && memoryModels == 1);
        supported(entries == 1 && functions.count(entryFunction) == 1);
        for (const auto& o : ops) if (o.code == 43 && o.args.size() == 3) {
            const auto& a = o.args; const auto it = types.find(a[0]);
            if (it != types.end() && it->second.code == 21 && it->second.args[0] == 32) {
                id(a[1]); require(values.emplace(a[1], a[2]).second);
                constants.push_back({a[1], a[0], a[2]});
            }
        }
    }
    const Type& type(Id value) const {
        const auto it = types.find(value); require(it != types.end()); return it->second;
    }
    bool is(Id value, Id code, const Words& args) const {
        const auto& t = type(value); return t.code == code && t.args == args;
    }
    Id value(Id constant) const {
        const auto it = values.find(constant);
        return it == values.end() ? UINT32_MAX : it->second;
    }
    Variable variable(const char* wanted) {
        Variable result{};
        for (auto v : variables) if (names[v.id] == wanted) {
            supported(result.id == 0); result = v;
        }
        supported(result.id != 0); return result;
    }
    Id block(Variable v) {
        supported(v.storage == 2);
        const auto& p = type(v.type); supported(p.code == 32 && p.args.size() == 2 && p.args[0] == 2);
        const auto b = p.args[1]; supported(type(b).code == 30 && dec[b] == Decorations{{2,{}}}); return b;
    }
    Id matrixVector(Id mat) const {
        const auto& m = type(mat); supported(m.code == 24 && m.args.size() == 2 && m.args[1] == 4);
        const auto& v = type(m.args[0]); supported(v.code == 23 && v.args.size() == 2 && v.args[1] == 4);
        supported(is(v.args[0], 22, {32})); return m.args[0];
    }
    Id fresh() { require(bound < maxBound, CameraSplitStatus::LimitExceeded); return bound++; }
};

struct Writer {
    Module& m;
    Words types, annotations, names;
    Id integer{}, view{};
    bool newView{true};
    explicit Writer(Module& module) : m(module) {}
    Id type(Id code, const Words& args) {
        for (const auto& [id, t] : m.types) if (t.code == code && t.args == args) return id;
        const auto id = m.fresh(); m.types.emplace(id, Type{code,args});
        Words a{id}; append(a,args); emit(types,code,a); return id;
    }
    Id constant(Id value) {
        for (auto c : m.constants) if (c.type == integer && c.value == value) return c.id;
        const auto id = m.fresh(); m.constants.push_back({id,integer,value});
        emit(types,43,{integer,id,value}); return id;
    }
    void getView() {
        if (!m.views.empty() || m.multiview) {
            supported(m.views.size() == 1 && m.multiview);
            view = *m.views.begin();
            const auto it = std::find_if(m.variables.begin(),m.variables.end(),[&](auto v){return v.id==view;});
            supported(it != m.variables.end());
            auto expected = Decorations{{11,{4440}}}; if (m.stage == 4) expected.emplace(14,Words{});
            supported(m.names[view] == "rdrvr_ViewIndex" && it->storage == 1 && m.is(it->type,32,{1,integer}) &&
                m.dec[view] == expected && std::find(m.interfaces.begin(),m.interfaces.end(),view) != m.interfaces.end());
            newView = false;
        } else {
            const auto p = type(32,{1,integer}); view = m.fresh(); emit(types,59,{p,view,1});
            viewAnnotations();
        }
    }
    void viewAnnotations() {
        name(names,view,"rdrvr_ViewIndex"); emit(annotations,71,{view,11,4440});
        if (m.stage == 4) emit(annotations,71,{view,14});
    }
};

// This bounded conversion deliberately does not correct native MVP with a
// previous camera. Every member2/4 access declines the first supported family.
CameraSplitStatus convertFrozen(Module& m, Words& result, CameraSplitReport& report, bool fragment=false) {
    supported(m.stage==(fragment?4u:0u)&&!m.multiview&&m.views.empty());
    for(const auto& v:m.variables) {
        const auto& d=m.dec[v.id];const auto set=d.find(34);
        supported(set==d.end()||set->second!=Words{3});
    }
    const auto v=m.variable("rage_matrices");const auto block=m.block(v);
    const auto members=m.type(block).args;
    supported(m.dec[v.id]==Decorations{{34,{0}},{33,{1}}});
    // A second variable may alias the same descriptor with a different name.
    // Reject it even if unused: every native-camera read must have one root.
    for(const auto& other:m.variables)if(other.id!=v.id) {
        const auto& d=m.dec[other.id];const auto set=d.find(34),binding=d.find(33);
        supported(set==d.end()||binding==d.end()||set->second!=Words{0}||binding->second!=Words{1});
    }
    supported(m.names[block]=="type.rage_matrices"&&members.size()==5);
    const auto matrix=members[0],vector=m.matrixVector(matrix);
    constexpr const char* names[]{"gWorldSingle","gWorldView","gWorldViewProj","gViewInverse","gTAAJitter"};
    for(Id i=0;i<5;++i) {
        auto layout=Decorations{{35,{i*64}}};if(i<4){layout[7]={16};layout[5]={};}
        supported(m.members[{block,i}]==names[i]&&members[i]==(i<4?matrix:vector)&&m.memberDec[{block,i}]==layout);
    }
    std::map<Id,Id> chains;std::set<Id> pointers;
    for(const auto& o:m.ops) {
        const auto& a=o.args;
        if((o.code==65||o.code==66||o.code==67||o.code==70)&&a[2]==v.id) {
            access(o.code==65&&a.size()>=4&&a.size()<=6);
            const auto member=m.value(a[3]);access((!fragment&&member==0)||member==1||member==3);
            Id pointee=matrix;
            for(std::size_t at=4;at<a.size();++at) {
                access(m.value(a[at])<4);pointee=at==4?vector:m.type(vector).args[0];
            }
            access(m.is(a[0],32,{2,pointee}));
            require(chains.emplace(a[1],member).second);pointers.insert(a[1]);
            if(member==0)++report.worldAccesses;
            if(member==1)++report.relativeAccesses;
            if(member==3)++report.inverseAccesses;
        }
    }
    // Tracked pointers may only be direct root access-chain results and loads.
    // No whole-block loads, copies, secondary chains, phi/calls/stores/escape.
    // Literal collisions conservatively decline instead of interpreting an
    // unrecognized operand grammar as proof that it is not a pointer use.
    bool function=false;
    for(const auto& o:m.ops) {
        if(o.code==54)function=true;if(!function)continue;
        const auto& a=o.args;
        for(std::size_t i=0;i<a.size();++i) {
            if((o.code==12&&i==3)||(o.code==88&&i==4)||(o.code==8&&i>=1)||
               (o.code==79&&i>=4)||(o.code==81&&i>=3)||(o.code==82&&i>=4))continue;
            if(a[i]==v.id)access(o.code==65&&i==2&&chains.count(a[1])!=0);
            if(pointers.count(a[i])) {
                if(o.code==65&&i==1)continue;
                access(o.code==61&&a.size()==3&&i==2);
            }
        }
    }
    require(fragment?(report.relativeAccesses||report.inverseAccesses):(report.relativeAccesses&&report.inverseAccesses),
+        fragment?CameraSplitStatus::UnsupportedAccess:CameraSplitStatus::UnsupportedPosition);
    // Validate load result type against the actual access-chain pointer.
    for(const auto& o:m.ops)if(o.code==61&&pointers.count(o.args[2])) {
        const auto chain=std::find_if(m.ops.begin(),m.ops.end(),[&](const Op& x){return x.code==65&&x.args[1]==o.args[2];});
        require(chain!=m.ops.end());access(m.is(chain->args[0],32,{2,o.args[0]}));
    }
    Writer w(m);w.integer=w.type(21,{32,1});
    const auto zero=w.constant(0),one=w.constant(1);
    const auto cameraBlock=m.fresh();emit(w.types,30,{cameraBlock,matrix,matrix});
    const auto pointer=w.type(32,{2,cameraBlock}),camera=m.fresh();emit(w.types,59,{pointer,camera,2});
    name(w.names,camera,"rdr95_FrozenCamera");name(w.names,cameraBlock,"type.rdr95_FrozenCamera");
    emit(w.annotations,71,{camera,34,3});emit(w.annotations,71,{camera,33,0});emit(w.annotations,71,{cameraBlock,2});
    for(Id i=0;i<2;++i){emit(w.annotations,72,{cameraBlock,i,35,i*64});emit(w.annotations,72,{cameraBlock,i,7,16});emit(w.annotations,72,{cameraBlock,i,5});}
    result.assign(m.source.begin(),m.source.begin()+5);bool annotations=false,types=false;
    for(const auto& o:m.ops) {
        if((o.code==71||o.code==72)&&!annotations){append(result,w.names);append(result,w.annotations);annotations=true;}
        if(o.code==54&&!types){append(result,w.types);types=true;}
        const auto it=o.code==65?chains.find(o.args[1]):chains.end();
        if(it!=chains.end()&&it->second!=0) {
            auto a=o.args;a[2]=camera;a[3]=it->second==1?zero:one;emit(result,o.code,a);
        }else emit(result,o.code,o.args);
    }
    require(annotations&&types);result[3]=m.bound;
    report.nativeVariable=v.id;report.cameraVariable=camera;report.nativeBlock=block;report.cameraBlock=cameraBlock;
    report.originalRelativePositionProved=!fragment;return CameraSplitStatus::Converted;
}
} // namespace
CameraSplitStatus splitFrozenCamera(std::span<const std::uint32_t> source,
    std::vector<std::uint32_t>& output,CameraSplitReport& report) noexcept {
    try {
        Module m(source);
        if(!detail::RelativePositionProof(source).run())return CameraSplitStatus::UnsupportedPosition;
        Words next;CameraSplitReport proof;
        const auto status=convertFrozen(m,next,proof);output.swap(next);report=proof;return status;
    }catch(const Decline& d){return d.status;}
    catch(const std::bad_alloc&){return CameraSplitStatus::AllocationFailed;}
    catch(...){return CameraSplitStatus::InvalidStructure;}
}
CameraSplitStatus splitFrozenFragmentCamera(std::span<const std::uint32_t> source,
    std::vector<std::uint32_t>& output,CameraSplitReport& report) noexcept {
    try {
        Module m(source);
        Words next;CameraSplitReport proof;
        const auto status=convertFrozen(m,next,proof,true);output.swap(next);report=proof;return status;
    }catch(const Decline& d){return d.status;}
    catch(const std::bad_alloc&){return CameraSplitStatus::AllocationFailed;}
    catch(...){return CameraSplitStatus::InvalidStructure;}
}
} // namespace rdrvr::engine
