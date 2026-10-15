#include "ColdRigidDrawBounds.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace rdrvr::engine {
namespace {
using C=ColdRigidDrawBounds;
bool key(C::Key k)noexcept{return k.identity&&k.generation;}
bool asset(const C::Asset& a)noexcept{return key(a.drawable)&&key(a.dependencies);}
bool pose(const C::Pose& p)noexcept{return p.scene&&p.frame&&p.owner&&p.revision;}
double up(double a)noexcept{return std::nextafter(a,std::numeric_limits<double>::infinity());}
double down(double a)noexcept{return std::nextafter(a,-std::numeric_limits<double>::infinity());}
bool point(C::XYZ p)noexcept{return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
}
void C::reset()noexcept {valid_=false;count_=0;asset_={};authority_={};failure_=Failure::None;proofKind_=ProofKind::None;}
bool C::build(const Asset& a,std::span<const Model> models,std::span<const Geometry> geometries,Authority authority)noexcept {
    reset();auto fail=[&](Failure why){failure_=why;return false;};
    if(!asset(a)||!authority.current||!authority.complete||!authority.current(authority.context,a,nullptr))return fail(Failure::Authority);
    if(models.empty()||models.size()>MaxModels||geometries.empty()||geometries.size()>MaxParts)return fail(Failure::Manifest);
    std::array<unsigned,MaxModels> starts{};std::array<bool,MaxParts> seen{};
    unsigned parts=0;
    for(unsigned i=0;i<models.size();++i){
        const auto& m=models[i];
        if(m.ordinal!=i||m.position!=Position::RigidXYZ1||!m.partCount||m.partCount>MaxParts-parts||m.paletteIndex>=MaxModels)return fail(Failure::Manifest);
        starts[i]=parts;parts+=m.partCount;locals_[i].paletteIndex=m.paletteIndex;
        locals_[i].bounds.minimum.fill(std::numeric_limits<double>::infinity());
        locals_[i].bounds.maximum.fill(-std::numeric_limits<double>::infinity());
    }
    if(parts!=geometries.size()||!authority.complete(authority.context,a,models,geometries))return fail(Failure::Manifest);
    std::uint64_t scanned=0;
    for(const auto& g:geometries){
        if(g.model>=models.size()||g.part>=models[g.model].partCount||!key(g.geometry)||!key(g.positionCertificate)||g.positions.empty()||g.indices.empty())return fail(Failure::Geometry);
        const auto ordinal=starts[g.model]+g.part;
        if(seen[ordinal])return fail(Failure::Manifest);seen[ordinal]=true;
        if(g.indices.size()>MaxIndices-scanned)return fail(Failure::Budget);scanned+=g.indices.size();
        auto& b=locals_[g.model].bounds;
        for(const auto index:g.indices){
            if(index>=g.positions.size())return fail(Failure::Geometry);
            const auto p=g.positions[index];if(!point(p))return fail(Failure::NonFinite);
            const double xyz[3]{p.x,p.y,p.z};
            for(unsigned k=0;k<3;++k){b.minimum[k]=std::min(b.minimum[k],xyz[k]);b.maximum[k]=std::max(b.maximum[k],xyz[k]);}
        }
    }
    for(unsigned i=0;i<parts;++i)if(!seen[i])return fail(Failure::Manifest);
    if(!authority.current(authority.context,a,nullptr))return fail(Failure::Changed);
    asset_=a;authority_=authority;count_=std::uint32_t(models.size());valid_=true;failure_=Failure::None;proofKind_=ProofKind::ShaderCertified;return true;
}
bool C::current(const Pose& p)const noexcept {return valid_&&pose(p)&&authority_.current&&authority_.current(authority_.context,asset_,&p);}
bool C::buildPrebounded(const Asset& a,std::span<const Model> models,std::span<const PreboundedGeometry> geometries,Authority authority)noexcept {
    reset();auto fail=[&](Failure why){failure_=why;return false;};
    if(!asset(a)||!authority.current||!authority.completePrebounded||!authority.current(authority.context,a,nullptr))return fail(Failure::Authority);
    if(models.empty()||models.size()>MaxModels||geometries.empty()||geometries.size()>MaxParts)return fail(Failure::Manifest);
    std::array<unsigned,MaxModels> starts{};std::array<bool,MaxParts> seen{};unsigned parts=0;
    for(unsigned i=0;i<models.size();++i){const auto& m=models[i];
        if(m.ordinal!=i||m.position!=Position::RigidXYZ1||!m.partCount||m.partCount>MaxParts-parts||m.paletteIndex>=MaxModels)return fail(Failure::Manifest);
        starts[i]=parts;parts+=m.partCount;locals_[i].paletteIndex=m.paletteIndex;
        locals_[i].bounds.minimum.fill(std::numeric_limits<double>::infinity());
        locals_[i].bounds.maximum.fill(-std::numeric_limits<double>::infinity());
    }
    if(parts!=geometries.size()||!authority.completePrebounded(authority.context,a,models,geometries))return fail(Failure::Manifest);
    std::uint64_t indices=0;
    for(const auto& g:geometries){
        if(g.model>=models.size()||g.part>=models[g.model].partCount||!key(g.geometry)||!key(g.positionCertificate)||!g.indexCount||g.indexCount%3)return fail(Failure::Geometry);
        const auto ordinal=starts[g.model]+g.part;if(seen[ordinal])return fail(Failure::Manifest);seen[ordinal]=true;
        if(g.indexCount>MaxIndices-indices)return fail(Failure::Budget);indices+=g.indexCount;
        auto& b=locals_[g.model].bounds;
        for(unsigned k=0;k<3;++k){
            if(!std::isfinite(g.bounds.minimum[k])||!std::isfinite(g.bounds.maximum[k])||g.bounds.minimum[k]>g.bounds.maximum[k])return fail(Failure::NonFinite);
            b.minimum[k]=std::min(b.minimum[k],g.bounds.minimum[k]);b.maximum[k]=std::max(b.maximum[k],g.bounds.maximum[k]);
        }
    }
    for(unsigned i=0;i<parts;++i)if(!seen[i])return fail(Failure::Manifest);
    if(!authority.current(authority.context,a,nullptr))return fail(Failure::Changed);
    asset_=a;authority_=authority;count_=std::uint32_t(models.size());valid_=true;failure_=Failure::None;proofKind_=ProofKind::ShaderCertified;return true;
}
bool C::buildGeometryPrebounded(const Asset& a,std::span<const Model> models,
        std::span<const GeometryPrebound> geometries,GeometryAuthority authority)noexcept {
    reset();auto fail=[&](Failure why){failure_=why;return false;};
    if(!asset(a)||!authority.current||!authority.complete||!authority.current(authority.context,a,nullptr))return fail(Failure::Authority);
    if(models.empty()||models.size()>MaxModels||geometries.empty()||geometries.size()>MaxParts)return fail(Failure::Manifest);
    std::array<unsigned,MaxModels> starts{};std::array<bool,MaxParts> seen{};unsigned parts=0;
    for(unsigned i=0;i<models.size();++i){const auto& m=models[i];
        if(m.ordinal!=i||m.position!=Position::RigidXYZ1||!m.partCount||m.partCount>MaxParts-parts||m.paletteIndex>=MaxModels)return fail(Failure::Manifest);
        starts[i]=parts;parts+=m.partCount;locals_[i].paletteIndex=m.paletteIndex;
        locals_[i].bounds.minimum.fill(std::numeric_limits<double>::infinity());
        locals_[i].bounds.maximum.fill(-std::numeric_limits<double>::infinity());
    }
    if(parts!=geometries.size()||!authority.complete(authority.context,a,models,geometries))return fail(Failure::Manifest);
    std::uint64_t indices=0;
    for(const auto& g:geometries){
        if(g.model>=models.size()||g.part>=models[g.model].partCount||!key(g.geometry)||!key(g.xyzLayout)||!g.indexCount||g.indexCount%3)return fail(Failure::Geometry);
        const auto ordinal=starts[g.model]+g.part;if(seen[ordinal])return fail(Failure::Manifest);seen[ordinal]=true;
        if(g.indexCount>MaxIndices-indices)return fail(Failure::Budget);indices+=g.indexCount;
        auto& b=locals_[g.model].bounds;
        for(unsigned k=0;k<3;++k){
            if(!std::isfinite(g.bounds.minimum[k])||!std::isfinite(g.bounds.maximum[k])||g.bounds.minimum[k]>g.bounds.maximum[k])return fail(Failure::NonFinite);
            b.minimum[k]=std::min(b.minimum[k],g.bounds.minimum[k]);b.maximum[k]=std::max(b.maximum[k],g.bounds.maximum[k]);
        }
    }
    for(unsigned i=0;i<parts;++i)if(!seen[i])return fail(Failure::Manifest);
    if(!authority.current(authority.context,a,nullptr))return fail(Failure::Changed);
    asset_=a;authority_={authority.context,authority.current,nullptr,nullptr};
    count_=std::uint32_t(models.size());valid_=true;failure_=Failure::None;proofKind_=ProofKind::ConditionalAffineXYZ;return true;
}
bool C::local(std::uint32_t model,Bounds& out)const noexcept {if(!valid_||model>=count_)return false;out=locals_[model].bounds;return true;}
bool C::update(const Pose& p,std::span<const ModelTransform> transforms,Result& out)const noexcept {
    out={};auto fail=[&](Failure why){failure_=why;return false;};
    if(!current(p))return fail(Failure::Authority);
    if(transforms.size()!=count_)return fail(Failure::Transform);
    Result r{};r.bounds.minimum.fill(std::numeric_limits<double>::infinity());r.bounds.maximum.fill(-std::numeric_limits<double>::infinity());
    // Full ULP: no round-to-nearest assumption for shader float arithmetic.
    constexpr double u=0x1p-23,gamma=8*u/(1-8*u);
    constexpr double flushGuard=16*double(std::numeric_limits<float>::min());
    constexpr double safeMagnitude=double(std::numeric_limits<float>::max())/16;
    for(unsigned model=0;model<count_;++model){
        const auto& t=transforms[model];const auto& local=locals_[model];
        if(t.model!=model||t.paletteIndex!=local.paletteIndex)return fail(Failure::Transform);
        for(unsigned k=0;k<16;++k)if(!std::isfinite(t.world[k])||!std::isfinite(t.coefficientError[k])||t.coefficientError[k]<0)return fail(Failure::NonFinite);
        if(t.world[3]!=0||t.world[7]!=0||t.world[11]!=0||t.world[15]!=1||t.coefficientError[3]!=0||t.coefficientError[7]!=0||t.coefficientError[11]!=0||t.coefficientError[15]!=0)return fail(Failure::Transform);
        for(unsigned row=0;row<3;++row){
            double lo=t.world[12+row],hi=lo,sum=std::abs(lo),uncertainty=t.coefficientError[12+row];
            for(unsigned column=0;column<3;++column){
                const auto index=column*4+row;const double c=t.world[index];
                const double low=local.bounds.minimum[column],high=local.bounds.maximum[column];
                const double extent=std::max(std::abs(low),std::abs(high));
                lo=down(lo+down(c*(c>=0?low:high)));
                hi=up(hi+up(c*(c>=0?high:low)));
                sum=up(sum+up(std::abs(c)*extent));
                uncertainty=up(uncertainty+up(t.coefficientError[index]*extent));
                // Shader input DAZ can zero a subnormal coordinate BEFORE a
                // very large multiplication. A constant output FTZ guard does
                // not bound that loss. Include every possible coordinate in
                // the local interval, plus a possibly subnormal coefficient.
                const double normalMinimum=std::numeric_limits<float>::min();
                const double coefficientMagnitude=up(std::abs(c)+t.coefficientError[index]);
                uncertainty=up(uncertainty+up(coefficientMagnitude*std::min(extent,normalMinimum)));
                if(std::abs(c)-t.coefficientError[index]<normalMinimum)
                    uncertainty=up(uncertainty+up(std::min(coefficientMagnitude,normalMinimum)*extent));
            }
            if(!std::isfinite(lo)||!std::isfinite(hi)||!std::isfinite(sum)||!std::isfinite(uncertainty)||sum+uncertainty>=safeMagnitude)return fail(Failure::NonFinite);
            const double error=up(uncertainty+up(gamma*up(sum+uncertainty))+flushGuard);
            if(!std::isfinite(error))return fail(Failure::NonFinite);
            r.bounds.minimum[row]=std::min(r.bounds.minimum[row],lo);r.bounds.maximum[row]=std::max(r.bounds.maximum[row],hi);
            r.axisError[row]=std::max(r.axisError[row],error);
        }
    }
    if(!current(p))return fail(Failure::Changed);
    r.worldError=std::max({r.axisError[0],r.axisError[1],r.axisError[2]});r.models=count_;r.asset=asset_;r.pose=p;r.valid=true;r.proofKind=proofKind_;
    out=r;failure_=Failure::None;return true;
}
}
