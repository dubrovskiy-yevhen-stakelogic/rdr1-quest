#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace rdrvr::engine::detail {
// Cold-only backward proof. This is deliberately not a general SPIR-V optimizer.
// It recognizes each component of WorldView * xyz1(WorldSingle * xyz1(input0)
// - ViewInverse[3]), including equivalent XYZ shuffles / float3 subtraction.
// No algebraic reassociation, data-dependent flow, alias writes, deformation,
// reduced-precision dependency, specialization or foreign matrix block qualifies.
class RelativePositionProof {
    static constexpr std::uint32_t absent=std::numeric_limits<std::uint32_t>::max();
    enum Value : std::uint8_t { Unknown,One,InputX,InputY,InputZ,InputW,WorldX,WorldY,WorldZ,WorldW,CameraX,CameraY,CameraZ,CameraW,RelativeX,RelativeY,RelativeZ,RelativeW,ClipX,ClipY,ClipZ,ClipW };
    struct Node {
        std::uint32_t at{},count{},op{},location{absent},set{absent},binding{absent},builtin{absent};
        bool reduced{},block{};
        std::array<Value,4> memo{};std::uint8_t computed{},active{};
    };
    struct Member {std::uint32_t type{},index{},offset{absent},stride{absent};bool col{},row{},reduced{};};
    struct Pointer {std::uint32_t variable{};std::array<std::uint32_t,4> index{};unsigned count{};};
    std::span<const std::uint32_t> words_;std::vector<Node> nodes_;std::vector<Member> members_;
    std::uint32_t position_{},input_{},uniform_{},checkedUniform_{};unsigned work_{};
    std::span<const std::uint32_t> args(std::uint32_t id) const noexcept {
        if(!id||id>=nodes_.size()||!nodes_[id].at)return {};
        const auto& n=nodes_[id];return words_.subspan(n.at+1,n.count-1);
    }
    unsigned opcode(std::uint32_t id) const noexcept {return id<nodes_.size()?nodes_[id].op:0;}
    bool precise(std::uint32_t id) const noexcept {return id&&id<nodes_.size()&&!nodes_[id].reduced;}
    bool scalar(std::uint32_t type) const noexcept {const auto a=args(type);return precise(type)&&opcode(type)==22&&a.size()==2&&a[1]==32;}
    unsigned lanes(std::uint32_t type) const noexcept {
        if(scalar(type))return 1;const auto a=args(type);
        return precise(type)&&opcode(type)==23&&a.size()==3&&scalar(a[1])&&a[2]>=2&&a[2]<=4?a[2]:0;
    }
    bool matrix(std::uint32_t type) const noexcept {const auto a=args(type);return precise(type)&&opcode(type)==24&&a.size()==3&&lanes(a[1])==4&&a[2]==4;}
    bool pointerType(std::uint32_t type,unsigned storage,std::uint32_t pointee) const noexcept {
        const auto a=args(type);return precise(type)&&opcode(type)==32&&a.size()==3&&a[1]==storage&&a[2]==pointee;
    }
    bool constant(std::uint32_t id,std::uint32_t& value) const noexcept {
        const auto a=args(id);if(!precise(id)||opcode(id)!=43||a.size()!=3)return false;
        const auto t=args(a[0]);if(opcode(a[0])!=21||t.size()!=3||t[1]!=32||t[2]>1)return false;
        value=a[2];return true;
    }
    bool pointer(std::uint32_t id,Pointer& out,unsigned depth=0) noexcept {
        if(++work_>20000||depth>32||!precise(id))return false;const auto a=args(id);
        if(opcode(id)==59&&a.size()==3){out.variable=id;return true;}
        if(opcode(id)==83&&a.size()==3&&args(a[2]).size()>=2&&a[0]==args(a[2])[0])return pointer(a[2],out,depth+1);
        if(opcode(id)!=65||a.size()<4||a.size()>7||!pointer(a[2],out,depth+1))return false;
        const auto var=args(out.variable);if(var.size()!=3)return false;
        const auto original=args(var[0]);if(opcode(var[0])!=32||original.size()!=3||original[1]!=var[2])return false;
        std::uint32_t type=original[2];
        // Re-evaluate the full constant path, proving the resulting pointer type.
        for(std::size_t i=3;i<a.size();++i){if(out.count==out.index.size()||!constant(a[i],out.index[out.count]))return false;++out.count;}
        for(unsigned i=0;i<out.count;++i){const auto t=args(type);const auto index=out.index[i];
            if(opcode(type)==30){if(index+1>=t.size())return false;type=t[index+1];}
            else if(opcode(type)==24||opcode(type)==23){if(t.size()!=3||index>=t[2])return false;type=t[1];}
            else return false;
        }
        return pointerType(a[0],var[2],type);
    }
    const Member* member(std::uint32_t type,unsigned index) const noexcept {
        for(const auto& m:members_)if(m.type==type&&m.index==index)return &m;return nullptr;
    }
    bool block(std::uint32_t variable) noexcept {
        if(checkedUniform_==variable)return true;const auto v=args(variable);if(v.size()!=3||v[2]!=2||!precise(variable))return false;
        const auto& d=nodes_[variable];if(d.set!=0||d.binding!=1)return false;
        const auto p=args(v[0]);if(opcode(v[0])!=32||p.size()!=3||p[1]!=2||!precise(v[0]))return false;
        const auto s=args(p[2]);if(opcode(p[2])!=30||s.size()!=6||!nodes_[p[2]].block||!precise(p[2]))return false;
        for(unsigned i=0;i<5;++i){const auto* m=member(p[2],i);if(!m||m->reduced||m->offset!=i*64)return false;
            if(i<4){if(!matrix(s[i+1])||!m->col||m->row||m->stride!=16)return false;}
            else if(lanes(s[i+1])!=4)return false;
        }
        if(uniform_&&uniform_!=variable)return false;uniform_=checkedUniform_=variable;return true;
    }
    bool matrixLoad(std::uint32_t id,unsigned index,unsigned depth=0) noexcept {
        if(++work_>20000||depth>32||!precise(id))return false;const auto a=args(id);
        if(opcode(id)==83&&a.size()==3&&args(a[2]).size()>=2&&a[0]==args(a[2])[0])return matrixLoad(a[2],index,depth+1);
        Pointer p{};return opcode(id)==61&&a.size()==3&&matrix(a[0])&&pointer(a[2],p)&&p.count==1&&p.index[0]==index&&block(p.variable);
    }
    bool xyz1(std::uint32_t id,Value base,unsigned depth) noexcept {
        const auto a=args(id);if(a.size()<2||lanes(a[0])!=4)return false;
        for(unsigned i=0;i<3;++i)if(component(id,i,depth+1)!=Value(unsigned(base)+i))return false;
        return component(id,3,depth+1)==One;
    }
    Value compute(std::uint32_t id,unsigned lane,unsigned depth) noexcept {
        const auto a=args(id);if(a.size()<2||lane>=lanes(a[0]))return Unknown;
        const auto op=opcode(id);
        if(op==43&&a.size()==3&&scalar(a[0])&&a[2]==0x3f800000)return One;
        if(op==83&&a.size()==3&&args(a[2]).size()>=2&&a[0]==args(a[2])[0])return component(a[2],lane,depth+1);
        if(op==61&&a.size()==3){Pointer p{};if(!pointer(a[2],p))return Unknown;const auto v=args(p.variable);if(v.size()!=3)return Unknown;
            if(v[2]==1&&!p.count&&nodes_[p.variable].location==0&&lanes(a[0])==4&&pointerType(v[0],1,a[0])&&precise(p.variable)){
                if(input_&&input_!=p.variable)return Unknown;input_=p.variable;return Value(unsigned(InputX)+lane);
            }
            if(p.count==2&&p.index[0]==3&&p.index[1]==3&&lanes(a[0])==4&&block(p.variable))return Value(unsigned(CameraX)+lane);
        }
        if(op==81&&a.size()==4&&scalar(a[0])){const auto source=args(a[2]);if(source.size()<2||a[3]>=lanes(source[0]))return Unknown;return component(a[2],a[3],depth+1);}
        if(op==80){unsigned offset{},selected{},selectedLane{};for(std::size_t i=2;i<a.size();++i){const auto v=args(a[i]);if(v.size()<2)return Unknown;const auto count=lanes(v[0]);if(!count||offset+count>4)return Unknown;
                if(lane>=offset&&lane<offset+count){selected=a[i];selectedLane=lane-offset;}offset+=count;}
            return offset==lanes(a[0])&&selected?component(selected,selectedLane,depth+1):Unknown;}
        if(op==79&&a.size()==4+lanes(a[0])){const auto l=args(a[2]),r=args(a[3]);if(l.size()<2||r.size()<2||lanes(l[0])<2||lanes(r[0])<2)return Unknown;
            const auto count=lanes(l[0]),index=a[4+lane];if(index<count)return component(a[2],index,depth+1);
            if(index<count+lanes(r[0]))return component(a[3],index-count,depth+1);return Unknown;}
        if(op==131&&a.size()==4){const auto l=args(a[2]),r=args(a[3]);if(l.size()<2||r.size()<2||l[0]!=a[0]||r[0]!=a[0])return Unknown;
            const auto left=component(a[2],lane,depth+1),right=component(a[3],lane,depth+1);
            if(left==Value(unsigned(WorldX)+lane)&&right==Value(unsigned(CameraX)+lane))return Value(unsigned(RelativeX)+lane);
        }
        if(op==145&&a.size()==4&&lanes(a[0])==4){
            if(matrixLoad(a[2],0)&&xyz1(a[3],InputX,depth+1))return Value(unsigned(WorldX)+lane);
            if(matrixLoad(a[2],1)&&xyz1(a[3],RelativeX,depth+1))return Value(unsigned(ClipX)+lane);
        }
        return Unknown;
    }
    Value component(std::uint32_t id,unsigned lane,unsigned depth) noexcept {
        if(++work_>20000||depth>48||lane>3||!precise(id))return Unknown;auto& n=nodes_[id];const auto bit=std::uint8_t(1u<<lane);
        if(n.active&bit)return Unknown;if(n.computed&bit)return n.memo[lane];n.active|=bit;
        const auto value=compute(id,lane,depth);n.active&=std::uint8_t(~bit);n.computed|=bit;n.memo[lane]=value;return value;
    }
    bool parse() {
        if(words_.size()<5||words_[3]<2||words_[3]>65536)return false;nodes_.resize(words_[3]);
        unsigned functions{},labels{},returns{},ends{},entry{},entryFunction{},function{};bool inFunction=false;
        for(std::size_t at=5;at<words_.size();){const auto count=words_[at]>>16,op=words_[at]&65535;if(!count||count>words_.size()-at)return false;const auto a=words_.subspan(at+1,count-1);
            std::uint32_t id{};
            if((op>=19&&op<=39)||op==7||op==11||op==248){if(a.empty())return false;id=a[0];}
            else if((op>=41&&op<=52)||op==54||op==55||op==59||op==61||op==65||op==66||op==79||op==80||op==81||op==83||op==131||op==145){if(a.size()<2)return false;id=a[1];}
            if(id){if(id>=nodes_.size()||nodes_[id].at)return false;auto& n=nodes_[id];n.at=std::uint32_t(at);n.count=count;n.op=op;}
            if(op==15){if(++entry!=1||a.size()<3||a[0]!=0)return false;entryFunction=a[1];}
            if(op==54){if(inFunction||++functions!=1||a.size()!=4)return false;inFunction=true;function=a[1];}
            if(op==248){if(!inFunction||++labels!=1)return false;}
            if(op==253){if(!inFunction||++returns!=1||!a.empty())return false;}
            if(op==56){if(!inFunction||++ends!=1||!a.empty())return false;inFunction=false;}
            if(op==57||op==63||op==64||op==66||op==67||op==70||op==73||op==74||op==75||op==245||op==246||op==247||(op>=249&&op<=252)||op==254||op==255)return false;
            if(op==71){if(a.size()<2||!a[0]||a[0]>=nodes_.size())return false;auto& n=nodes_[a[0]];const auto d=a[1];
                if(d==0||d==39||d==40)n.reduced=true;if(d==2)n.block=true;
                std::uint32_t* slot=d==30?&n.location:d==33?&n.binding:d==34?&n.set:d==11?&n.builtin:nullptr;
                if(slot){if(a.size()!=3||*slot!=absent)return false;*slot=a[2];}
            }
            if(op==72){if(a.size()<3||!a[0]||a[0]>=nodes_.size())return false;Member* m{};for(auto& existing:members_)if(existing.type==a[0]&&existing.index==a[1]){m=&existing;break;}
                if(!m){if(members_.size()==512)return false;members_.push_back({a[0],a[1]});m=&members_.back();}const auto d=a[2];
                if(d==0||d==39||d==40)m->reduced=true;if(d==4)m->row=true;if(d==5)m->col=true;
                if(d==35||d==7){auto& slot=d==35?m->offset:m->stride;if(a.size()!=4||slot!=absent)return false;slot=a[3];}
                if(d==11&&a.size()==4&&a[3]==0)return false; // output-block aliases not in this proof
            }
            at+=count;
        }
        if(inFunction||functions!=1||labels!=1||returns!=1||ends!=1||entry!=1||entryFunction!=function)return false;
        for(std::uint32_t id=1;id<nodes_.size();++id)if(nodes_[id].builtin==0){if(position_)return false;position_=id;}
        const auto p=args(position_);return position_&&opcode(position_)==59&&p.size()==3&&p[2]==3&&precise(position_);
    }
public:
    explicit RelativePositionProof(std::span<const std::uint32_t> words):words_(words){}
    bool run() {
        if(!parse())return false;std::uint32_t value{};unsigned stores{};bool inFunction=false;
        for(std::size_t at=5;at<words_.size();){const auto count=words_[at]>>16,op=words_[at]&65535;const auto a=words_.subspan(at+1,count-1);
            if(op==54)inFunction=true;if(op==56)inFunction=false;
            if(op==62){if(!inFunction||a.size()!=2)return false;Pointer p{};if(!pointer(a[0],p))return false;const auto v=args(p.variable);
                if(v.size()!=3||(v[2]!=3&&v[2]!=6&&v[2]!=7))return false;
                if(p.variable==position_){if(a[0]!=position_||p.count||++stores!=1)return false;value=a[1];}
            }at+=count;
        }
        if(stores!=1)return false;const auto v=args(value),p=args(position_);if(v.size()<2||lanes(v[0])!=4||!pointerType(p[0],3,v[0]))return false;
        for(unsigned i=0;i<4;++i)if(component(value,i,0)!=Value(unsigned(ClipX)+i))return false;
        return input_&&uniform_;
    }
};
}
