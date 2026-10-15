#pragma once
#include "WeaponAttachmentReader.h"

namespace rdrvr::engine {
struct SkeletonPoseEvidence {
    unsigned bones{};bool globalsMatchLocals{},rootMatchesModel{};
    float maxBasisError{},maxPositionError{},rootModelDistance{};
};
// Read-only evidence for the audited crSkeleton::Update layout/algorithm.
// A matching sample is NOT an ownership permit or an instruction to mutate
// bones. The native owner must supply the freshly resolved weapon skeleton.
class SkeletonPoseAudit {
public:
    static constexpr unsigned maxBones=64;
    struct Api {bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};};
private:
    Api api_;
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return api_.read&&p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&api_.read(api_.context,p+offset,&out,sizeof out);
    }
    struct Sample {
        std::uintptr_t data{},root{},locals{},globals{},parents{};std::uint32_t count{};std::uint16_t dataCount{};
        runtime::Mat4 rootMatrix{};std::array<runtime::Mat4,maxBones> local{},global{};
        std::array<std::int32_t,maxBones> parent{};
    };
    bool matrix(std::uintptr_t p,std::size_t offset,runtime::Mat4& out)const{
        WeaponNativeMatrix m{};if(!get(p,offset,m))return false;
        m.values[3]=m.values[7]=m.values[11]=0;m.values[15]=1;
        for(float x:m.values)if(!std::isfinite(x))return false;
        out=m.values;return true;
    }
    bool sample(std::uintptr_t skeleton,Sample& s)const{
        if(!get(skeleton,8,s.data)||!get(skeleton,0x10,s.root)||!get(skeleton,0x18,s.locals)||
           !get(skeleton,0x20,s.count)||!s.count||s.count>maxBones||!get(skeleton,0x28,s.globals)||
           !get(s.data,8,s.parents)||!get(s.data,0x30,s.dataCount)||s.count!=s.dataCount||!matrix(s.root,0,s.rootMatrix))return false;
        for(unsigned i=0;i<s.count;++i){
            if(!get(s.parents,i*4,s.parent[i])||!matrix(s.locals,i*0x60,s.local[i])||!matrix(s.globals,i*0x40,s.global[i]))return false;
            // Native Update visits children in order and uses already-built
            // parent matrices. Parent[0] is not read by its root branch.
            if(i&&(s.parent[i]<0||unsigned(s.parent[i])>=i))return false;
        }
        return true;
    }
    static bool same(const Sample& a,const Sample& b){
        if(a.data!=b.data||a.root!=b.root||a.locals!=b.locals||a.globals!=b.globals||a.parents!=b.parents||
           a.count!=b.count||a.dataCount!=b.dataCount||a.rootMatrix!=b.rootMatrix)return false;
        for(unsigned i=0;i<a.count;++i)if(a.local[i]!=b.local[i]||a.global[i]!=b.global[i]||a.parent[i]!=b.parent[i])return false;
        return true;
    }
public:
    explicit SkeletonPoseAudit(Api api):api_(api){}
    bool inspect(std::uintptr_t skeleton,const runtime::Mat4& model,SkeletonPoseEvidence& out)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            for(float x:model)if(!std::isfinite(x))return false;
            Sample a{},b{};if(!sample(skeleton,a)||!sample(skeleton,b)||!same(a,b))return false;
            std::array<runtime::Mat4,maxBones> expected{};
            expected[0]=runtime::multiply(a.rootMatrix,a.local[0]);
            runtime::Vec3 origin{};for(unsigned j=0;j<3;++j){origin[j]=expected[0][12+j];expected[0][12+j]=0;}
            // The retail implementation factors out the common root position
            // while propagating the tree, then adds it to every global bone.
            for(unsigned i=1;i<a.count;++i)expected[i]=runtime::multiply(expected[unsigned(a.parent[i])],a.local[i]);
            SkeletonPoseEvidence result{};result.bones=a.count;result.rootMatchesModel=true;
            // Native FMLA composition and the mod's scalar composition can
            // differ by rounding. Bound meaningful lanes, never pointer identity.
            for(unsigned col=0;col<4;++col)for(unsigned row=0;row<3;++row)
                if(std::abs(a.rootMatrix[col*4+row]-model[col*4+row])>(col==3?.001f:.0001f))result.rootMatchesModel=false;
            double rootDistance2{};
            for(unsigned j=0;j<3;++j){const double d=double(a.rootMatrix[12+j])-model[12+j];rootDistance2+=d*d;}
            result.rootModelDistance=float(std::sqrt(rootDistance2));
            if(!std::isfinite(result.rootModelDistance))return false;
            for(unsigned i=0;i<a.count;++i){
                for(unsigned j=0;j<3;++j)expected[i][12+j]+=origin[j];
                for(unsigned col=0;col<4;++col)for(unsigned row=0;row<3;++row){
                    const auto index=col*4+row;const float error=std::abs(expected[i][index]-a.global[i][index]);
                    if(!std::isfinite(error))return false;
                    auto& maximum=col==3?result.maxPositionError:result.maxBasisError;maximum=std::max(maximum,error);
                }
            }
            // Diagnostic tolerances, not proof of a transactional native pose.
            result.globalsMatchLocals=result.maxBasisError<=.0001f&&result.maxPositionError<=.001f;
            out=result;return true;
        }catch(...){return false;}
    }
};
}
