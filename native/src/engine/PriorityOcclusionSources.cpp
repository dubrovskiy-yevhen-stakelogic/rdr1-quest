#include "PriorityOcclusionSources.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

namespace rdrvr::engine {
namespace {
struct FloatPlanningCache {const void* owner{};std::uint64_t epoch{};float rows[2][5][4]{};bool common[5]{};};
thread_local FloatPlanningCache floatPlanningCache;
bool finite(double v)noexcept{return std::isfinite(v)&&std::abs(v)<0x1p60;}
bool vectorFinite(const ConservativeStereoRaster::Vec3&v)noexcept{return finite(v.x)&&finite(v.y)&&finite(v.z);}
}
void PriorityOcclusionSources::reset()noexcept {
    for(unsigned i=0;i<sourceCount_;++i)sources_[i].reset();
    earlyTrialUsed_=false;earlySource_=MaxSources;earlyPatchMask_=0;sourceCount_=queued_=freeCount_=0;targetCount_=activeTargets_=0;maxTargetDepth_={};projection_=nullptr;raster_=nullptr;cache_=nullptr;frame_=serial_=0;built_=false;
}
bool PriorityOcclusionSources::begin(std::uint64_t frame,const StaticOcclusionProjection&projection,Raster&raster,
                                    const OwnedStaticOccluders*cache,Budget budget)noexcept {
    reset();stats_={};floatPlanningCache.owner=nullptr;
    if(!++envelopeEpoch_){for(auto&e:envelopes_)e.epoch=0;++envelopeEpoch_;}
    if(!frame||!cache||!budget.sources||budget.sources>MaxSources||!budget.queue||budget.queue>MaxQueue||
       !budget.expansions||!budget.patches||!budget.targetQueries||!budget.reachabilityCells)return false;
    frame_=frame;projection_=&projection;raster_=&raster;cache_=cache;serial_=cache->invalidationSerial();budget_=budget;
    for(unsigned i=0;i<MaxQueue;++i)free_[i]=MaxQueue-1-i;freeCount_=MaxQueue;return true;
}
bool PriorityOcclusionSources::better(const Entry&a,const Entry&b)const noexcept {
    if(a.area!=b.area)return a.area>b.area;
    if(a.nearW!=b.nearW)return a.nearW<b.nearW;
    if(a.source!=b.source){const auto&pa=sources_[a.source];const auto&pb=sources_[b.source];
    if(pa.vertexHandle!=pb.vertexHandle)return pa.vertexHandle<pb.vertexHandle;
    if(pa.indexHandle!=pb.indexHandle)return pa.indexHandle<pb.indexHandle;
    for(unsigned i=0;i<16;++i)if(pa.world[i]!=pb.world[i])return pa.world[i]<pb.world[i];
    if(pa.faces!=pb.faces)return unsigned(pa.faces)<unsigned(pb.faces);
    if(pa.mesh.generation!=pb.mesh.generation)return pa.mesh.generation<pb.mesh.generation;}
    if(a.patch!=b.patch)return a.patch;
    return a.item<b.item;
}
bool PriorityOcclusionSources::heapBetter(unsigned a,unsigned b,bool maximum)const noexcept {
    return maximum?better(entries_[a],entries_[b]):better(entries_[b],entries_[a]);
}
void PriorityOcclusionSources::swapHeap(unsigned a,unsigned b,bool maximum)noexcept {
    auto&heap=maximum?maxHeap_:minHeap_;std::swap(heap[a],heap[b]);
    if(maximum){entries_[heap[a]].maxPos=a;entries_[heap[b]].maxPos=b;}
    else{entries_[heap[a]].minPos=a;entries_[heap[b]].minPos=b;}
}
void PriorityOcclusionSources::repair(unsigned p,bool maximum)noexcept {
    auto&heap=maximum?maxHeap_:minHeap_;
    if(p&&heapBetter(heap[p],heap[(p-1)/2],maximum)){
        while(p){const auto parent=(p-1)/2;if(!heapBetter(heap[p],heap[parent],maximum))break;swapHeap(p,parent,maximum);p=parent;}
        return;
    }
    for(;;){unsigned next=p,left=p*2+1,right=left+1;
        if(left<queued_&&heapBetter(heap[left],heap[next],maximum))next=left;
        if(right<queued_&&heapBetter(heap[right],heap[next],maximum))next=right;
        if(next==p)return;swapHeap(p,next,maximum);p=next;
    }
}
void PriorityOcclusionSources::remove(unsigned slot)noexcept {
    const unsigned last=queued_-1,maximum=entries_[slot].maxPos,minimum=entries_[slot].minPos;
    if(maximum!=last)swapHeap(maximum,last,true);
    if(minimum!=last)swapHeap(minimum,last,false);
    queued_=last;
    if(maximum<queued_)repair(maximum,true);
    if(minimum<queued_)repair(minimum,false);
    free_[freeCount_++]=slot;
}
bool PriorityOcclusionSources::push(const Entry&incoming)noexcept {
    if(!incoming.potential){if(incoming.clipRejected){if(incoming.patch)++stats_.clipDropped;else ++stats_.clipNodeDropped;}else if(incoming.backfaceRejected)++stats_.backfaceBoundsDropped;else ++stats_.smallBoundsDropped;return false;}
    if(queued_==budget_.queue){
        ++stats_.queueDropped;
        if(!better(incoming,entries_[minHeap_[0]]))return false;
        remove(minHeap_[0]);
    }
    if(!freeCount_)return false;
    const auto slot=free_[--freeCount_];entries_[slot]=incoming;
    entries_[slot].maxPos=entries_[slot].minPos=queued_;
    maxHeap_[queued_]=minHeap_[queued_]=slot;++queued_;
    repair(queued_-1,true);repair(queued_-1,false);
    stats_.queueHighWater=std::max(stats_.queueHighWater,std::uint64_t(queued_));return true;
}
PriorityOcclusionSources::Entry PriorityOcclusionSources::pop()noexcept {
    const auto slot=maxHeap_[0];const auto result=entries_[slot];remove(slot);return result;
}
bool PriorityOcclusionSources::rank(unsigned source,unsigned item,bool patch,Entry&out)noexcept {
    prepareClip(source);
    const auto&packet=sources_[source];const auto&mesh=packet.mesh;
    std::array<std::array<double,3>,Raster::MaxPatchVertices> points;unsigned count{};
    double lo[3]{},extent[3]{};
    if(patch){
        if(item>=mesh.patches.size())return false;const auto&p=mesh.patches[item];
        if(p.count<3||p.count>Raster::MaxPatchVertices||p.firstIndex>mesh.patchIndices.size()||p.count>mesh.patchIndices.size()-p.firstIndex)return false;
        count=p.count;
        for(unsigned k=0;k<count;++k){const auto index=mesh.patchIndices[p.firstIndex+k];if(index>=mesh.positions.size())return false;const auto&v=mesh.positions[index];points[k]={v.x,v.y,v.z};if(!finite(v.x)||!finite(v.y)||!finite(v.z))return false;}
    }else{
        if(item>=mesh.nodes.size())return false;const auto&node=mesh.nodes[item];
        lo[0]=node.minimum.x;lo[1]=node.minimum.y;lo[2]=node.minimum.z;
        const double hi[]{node.maximum.x,node.maximum.y,node.maximum.z};
        for(unsigned axis=0;axis<3;++axis){if(!finite(lo[axis])||!finite(hi[axis])||lo[axis]>hi[axis])return false;extent[axis]=hi[axis]-lo[axis];}
        count=8;
    }
    out={};out.source=source;out.item=item;out.patch=patch;out.nearW=std::numeric_limits<double>::infinity();unsigned backfacingEyes=0,clippedEyes=0;
    for(unsigned eye=0;eye<2;++eye){
        const auto&m=localClip_[source][eye];double minX=1e100,minY=1e100,maxX=-1e100,maxY=-1e100,minW=1e100,minDepth=1,maxDepth=0;bool uncertain=false,backfacing=false,clipped=false;double firstX[3]{},firstY[3]{};
        double base[4]{},dx[4]{},dy[4]{},dz[4]{};
        if(!patch){
            double low[5],high[5];
            for(unsigned row=0;row<4;++row){const auto*r=&m[row*4];base[row]=r[0]*lo[0]+r[1]*lo[1]+r[2]*lo[2]+r[3];dx[row]=r[0]*extent[0];dy[row]=r[1]*extent[1];dz[row]=r[2]*extent[2];
                const double middle=base[row]+(dx[row]+dy[row]+dz[row])*.5,radius=(std::abs(dx[row])+std::abs(dy[row])+std::abs(dz[row]))*.5;low[row]=middle-radius;high[row]=middle+radius;
            }
            const double dbase=base[3]-base[2],ddx=dx[3]-dx[2],ddy=dy[3]-dy[2],ddz=dz[3]-dz[2];
            const double middle=dbase+(ddx+ddy+ddz)*.5,radius=(std::abs(ddx)+std::abs(ddy)+std::abs(ddz))*.5;low[4]=middle-radius;high[4]=middle+radius;
            for(unsigned row=0;row<5;++row)if(!finite(low[row])||!finite(high[row]))uncertain=true;
            // A crossing NODE may contain a valid descendant. A wholly outside
            // node cannot contribute to the strict source raster.
            if(!uncertain&&(high[3]<=1e-8||high[2]<=0||high[4]<=0)){
                auto&e=out.eye[eye];e.known=e.empty=true;++clippedEyes;continue;
            }
            // Each aggregate selects ONE REAL extremal vertex in every
            // descendant patch. For an axis with positive c choose min-axis,
            // otherwise max-axis; other coordinates use the full node box.
            // An upper bound <= the rejected clip plane proves every patch has
            // an unusable vertex in this eye. This only omits source work.
            const auto&node=mesh.nodes[item];
            const double maxMin[]{node.patchMinMaximum.x,node.patchMinMaximum.y,node.patchMinMaximum.z};
            const double minMax[]{node.patchMaxMinimum.x,node.patchMaxMinimum.y,node.patchMaxMinimum.z};
            bool witnessKnown=true;
            for(unsigned axis=0;axis<3;++axis){
                const double lower=std::nextafter(float(lo[axis]),-std::numeric_limits<float>::infinity());
                const double upper=std::nextafter(float(lo[axis]+extent[axis]),std::numeric_limits<float>::infinity());
                if(!finite(maxMin[axis])||!finite(minMax[axis])||maxMin[axis]<lower||maxMin[axis]>upper||minMax[axis]<lower||minMax[axis]>upper)witnessKnown=false;
            }
            const auto allPatchesClipped=[&](unsigned plane,double threshold){
                if(!witnessKnown||!finite(low[plane])||low[plane]>threshold)return false;
                double coefficients[4];for(unsigned k=0;k<4;++k)coefficients[k]=plane<4?m[plane*4+k]:m[12+k]-m[8+k];
                for(unsigned axis=0;axis<3;++axis){
                    double upper=coefficients[3],scale=1+std::abs(upper);
                    for(unsigned k=0;k<3;++k){const auto c=coefficients[k];const auto v=k==axis?(c>=0?maxMin[k]:minMax[k]):(c>=0?lo[k]+extent[k]:lo[k]);
                        const double term=c*v;upper+=term;scale+=std::abs(term);}
                    if(finite(upper)&&finite(scale)&&upper+256*std::numeric_limits<double>::epsilon()*scale<=threshold)return true;
                }return false;
            };
            if(allPatchesClipped(3,1e-8)||allPatchesClipped(2,0)||allPatchesClipped(4,0)){
                auto&e=out.eye[eye];e.known=e.empty=true;++clippedEyes;continue;
            }
            if(low[3]<=1e-8||low[2]<=0||low[4]<=0)uncertain=true;
            if(!uncertain){
                const double inverseNear=1/low[3],inverseFar=1/high[3];minW=low[3];
                minX=(low[0]*(low[0]<0?inverseNear:inverseFar)+1)*32;maxX=(high[0]*(high[0]>0?inverseNear:inverseFar)+1)*32;
                minY=(low[1]*(low[1]<0?inverseNear:inverseFar)+1)*32;maxY=(high[1]*(high[1]>0?inverseNear:inverseFar)+1)*32;
                minDepth=std::max(low[2]*inverseFar,1-high[4]*inverseNear);
                if(!finite(minX)||!finite(maxX)||!finite(minY)||!finite(maxY)||!finite(minDepth))uncertain=true;
            }
        }
        else for(unsigned corner=0;corner<count;++corner){double q[4];
            if(patch){const auto&v=points[corner];for(unsigned row=0;row<4;++row){const auto*r=&m[row*4];q[row]=r[0]*v[0]+r[1]*v[1]+r[2]*v[2]+r[3];}}
            else for(unsigned row=0;row<4;++row)q[row]=base[row]+((corner&1)?dx[row]:0)+((corner&2)?dy[row]:0)+((corner&4)?dz[row]:0);
            if(!finite(q[0])||!finite(q[1])||!finite(q[2])||!finite(q[3])||q[3]<=1e-8||q[2]<=0||q[2]>=q[3]){clipped=true;break;}
            const double reciprocal=1/q[3],px=(q[0]*reciprocal+1)*32,py=(q[1]*reciprocal+1)*32;
            minW=std::min(minW,q[3]);minDepth=std::min(minDepth,q[2]*reciprocal);maxDepth=std::max(maxDepth,q[2]*reciprocal);
            if(!finite(px)||!finite(py)||std::abs(px)>1e9||std::abs(py)>1e9){clipped=true;break;}
            minX=std::min(minX,px);minY=std::min(minY,py);maxX=std::max(maxX,px);maxY=std::max(maxY,py);
            if(patch&&corner<3){firstX[corner]=px;firstY[corner]=py;
                if(corner==2&&packet.faces!=Raster::Faces::Both&&!uncertain){
                    const double a=(firstX[1]-firstX[0])*(firstY[2]-firstY[0]),b=(firstY[1]-firstY[0])*(firstX[2]-firstX[0]),area=a-b;
                    const double roundoff=128*std::numeric_limits<double>::epsilon()*(1+std::abs(a)+std::abs(b));
                    if(std::isfinite(area)&&((area < -roundoff&&packet.faces==Raster::Faces::PositiveArea)||(area > roundoff&&packet.faces==Raster::Faces::NegativeArea))){backfacing=true;break;}
                }
            }
        }
        // Only crossing BOUNDS retain uncertain priority. An exact patch with
        // any vertex outside the current raster contract cannot contribute in
        // that eye, and must not consume the global source patch budget.
        if(clipped){auto&e=out.eye[eye];e.known=e.empty=true;++clippedEyes;continue;}
        if(backfacing){out.eye[eye].known=out.eye[eye].empty=true;out.nearW=std::min(out.nearW,minW);++backfacingEyes;continue;}
        if(uncertain){out.area+=4096;out.nearW=0;out.potential=true;continue;}
        const double width=std::max(0.,std::min(64.,maxX)-std::max(0.,minX));
        const double height=std::max(0.,std::min(64.,maxY)-std::max(0.,minY));
        out.area+=width*height;out.nearW=std::min(out.nearW,minW);
                bool canFit=width+2e-9>=1&&height+2e-9>=1;
        if(canFit&&!patch){
            // Every descendant patch has these outward local axis extents.
            // Bound its projective diameter, not the whole node's footprint:
            // |X1/W1-X2/W2| <= (deltaX+max|X/W|*deltaW)/minW.
            // This only avoids potential source work; unknown keeps the node.
            const auto&maximumExtent=mesh.nodes[item].maxPatchExtent;
            const double maximum[]{maximumExtent.x,maximumExtent.y,maximumExtent.z};double delta[3]{};
            bool boundKnown=true;
            for(unsigned axis=0;axis<3;++axis)if(!std::isfinite(maximum[axis])||maximum[axis]<0)boundKnown=false;
            if(boundKnown){
                for(unsigned axis=0;axis<3;++axis){delta[0]+=std::abs(m[axis])*maximum[axis];delta[1]+=std::abs(m[4+axis])*maximum[axis];delta[2]+=std::abs(m[12+axis])*maximum[axis];}
                const double wScale=1+std::abs(base[3])+std::abs(dx[3])+std::abs(dy[3])+std::abs(dz[3]);
                const double safeW=minW-128*std::numeric_limits<double>::epsilon()*wScale;
                if(safeW>1e-8){
                    const double rx=std::max(std::abs(minX/32-1),std::abs(maxX/32-1)),ry=std::max(std::abs(minY/32-1),std::abs(maxY/32-1));
                    const double wx=32*(delta[0]+rx*delta[2])/safeW,wy=32*(delta[1]+ry*delta[2])/safeW;
                    const double widen=1+256*std::numeric_limits<double>::epsilon();
                    if(std::isfinite(wx)&&std::isfinite(wy))canFit=wx*widen+1e-9>=1&&wy*widen+1e-9>=1;
                }
            }
        }
        if(canFit)out.potential=true;
        auto&e=out.eye[eye];e.known=true;e.empty=width<=0||height<=0;e.nearDepth=std::max(0.,(patch?maxDepth:minDepth)-1e-9);
        auto tile=[](double x){return static_cast<unsigned char>(std::min(unsigned(std::clamp(x,0.,64.)/8),7u));};
        e.x0=tile(minX-1e-9);e.x1=tile(maxX+1e-9);e.y0=tile(minY-1e-9);e.y1=tile(maxY+1e-9);
        auto cell=[](double x){return static_cast<unsigned char>(std::clamp(x,0.,63.));};
        e.cx0=cell(minX-1e-9);e.cx1=cell(maxX+1e-9);e.cy0=cell(minY-1e-9);e.cy1=cell(maxY+1e-9);
    }
    out.backfaceRejected=backfacingEyes==2;
    out.clipRejected=clippedEyes>0&&out.eye[0].known&&out.eye[0].empty&&out.eye[1].known&&out.eye[1].empty;
    if(out.eye[0].known&&out.eye[0].empty&&out.eye[1].known&&out.eye[1].empty){out.potential=false;out.area=0;out.nearW=0;return true;}
    if(!std::isfinite(out.area)||!std::isfinite(out.nearW)){out.area=8192;out.nearW=0;out.potential=true;}
    return true;
}
bool PriorityOcclusionSources::planBounds(const Raster::Bounds&bounds,Entry&out)const noexcept {
    // These cached nominal rectangles schedule sources only. They are NEVER
    // returned as a native-hidden proof, nor substituted for candidate errors.
    ++stats_.envelopeQueries;
    if(!vectorFinite(bounds.minimum)||!vectorFinite(bounds.maximum)||bounds.minimum.x>bounds.maximum.x||bounds.minimum.y>bounds.maximum.y||bounds.minimum.z>bounds.maximum.z)return false;
    const double values[]{bounds.minimum.x,bounds.minimum.y,bounds.minimum.z,bounds.maximum.x,bounds.maximum.y,bounds.maximum.z};
    std::uint64_t hash=0xcbf29ce484222325ull;for(double v:values){hash^=std::bit_cast<std::uint64_t>(v);hash*=0x100000001b3ull;hash^=hash>>32;}
    Envelope*replace=&envelopes_[unsigned(hash)&(EnvelopeSlots-1)];const auto&e=*replace;
    if(e.epoch==envelopeEpoch_&&e.bounds.minimum.x==values[0]&&e.bounds.minimum.y==values[1]&&e.bounds.minimum.z==values[2]&&e.bounds.maximum.x==values[3]&&e.bounds.maximum.y==values[4]&&e.bounds.maximum.z==values[5]){out=e.entry;++stats_.envelopeHits;return true;}
    out={};out.nearW=1e100;
    if(floatPlanningCache.owner!=this||floatPlanningCache.epoch!=envelopeEpoch_){
        for(unsigned eye=0;eye<2;++eye){const auto&m=projection_->matrices()[eye];for(unsigned row=0;row<5;++row)for(unsigned k=0;k<4;++k)
            floatPlanningCache.rows[eye][row][k]=float(row<4?m[row*4+k]:m[12+k]-m[8+k]);}
        const auto&left=projection_->matrices()[0];const auto&right=projection_->matrices()[1];
        for(unsigned row=0;row<5;++row){bool same=true;for(unsigned k=0;k<4;++k){
            const double a=row<4?left[row*4+k]:left[12+k]-left[8+k];
            const double b=row<4?right[row*4+k]:right[12+k]-right[8+k];
            same=same&&std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);
        }floatPlanningCache.common[row]=same;}
        floatPlanningCache.owner=this;floatPlanningCache.epoch=envelopeEpoch_;
    }
    const float center[]{float((values[0]+values[3])*.5),float((values[1]+values[4])*.5),float((values[2]+values[5])*.5)};
    const float half[]{float((values[3]-values[0])*.5),float((values[4]-values[1])*.5),float((values[5]-values[2])*.5)};
    float savedLow[5]{},savedHigh[5]{},savedInverseNear{},savedInverseFar{};bool savedRowFinite[5]{},savedW{};
    for(unsigned eye=0;eye<2;++eye){const auto&m=floatPlanningCache.rows[eye];float low[5],high[5];bool uncertain=false;
        for(unsigned row=0;row<5;++row){if(eye&&floatPlanningCache.common[row]){low[row]=savedLow[row];high[row]=savedHigh[row];uncertain=uncertain||!savedRowFinite[row];continue;}const auto*r=m[row];
            const float middle=r[0]*center[0]+r[1]*center[1]+r[2]*center[2]+r[3];
            const float radius=std::abs(r[0])*half[0]+std::abs(r[1])*half[1]+std::abs(r[2])*half[2];
            low[row]=middle-radius;high[row]=middle+radius;const bool known=finite(low[row])&&finite(high[row]);if(!known)uncertain=true;
            if(!eye){savedLow[row]=low[row];savedHigh[row]=high[row];savedRowFinite[row]=known;}
        }
        // A finite whole-outside object bound can be declined before the
        // optional native packet receipt. Widen float arithmetic; boundaries,
        // overflow and uncertain rows stay on the unknown path.
        const auto outside=[&](unsigned row,float threshold){
            if(uncertain||high[row]>threshold)return false;
            const auto*r=m[row];float scale=1.f+std::abs(r[3]);
            for(unsigned axis=0;axis<3;++axis)scale+=std::abs(r[axis])*(std::abs(center[axis])+half[axis]);
            const float widened=high[row]+256*std::numeric_limits<float>::epsilon()*scale;
            return finite(scale)&&finite(widened)&&widened<=threshold;
        };
        if(outside(3,1e-8f)||outside(2,0.f)||outside(4,0.f)){auto&e=out.eye[eye];e.known=e.empty=true;continue;}
        if(uncertain||low[3]<=1e-8||low[2]<=0||low[4]<=0){out.area+=4096;out.nearW=0;out.potential=true;continue;}
        const bool sharedW=eye&&floatPlanningCache.common[3]&&savedW;
        const float inverseNear=sharedW?savedInverseNear:1.f/low[3],inverseFar=sharedW?savedInverseFar:1.f/high[3];
        if(!eye){savedInverseNear=inverseNear;savedInverseFar=inverseFar;savedW=true;}
        const double minX=(low[0]*(low[0]<0?inverseNear:inverseFar)+1.f)*32.f;
        const double maxX=(high[0]*(high[0]>0?inverseNear:inverseFar)+1.f)*32.f;
        const double minY=(low[1]*(low[1]<0?inverseNear:inverseFar)+1.f)*32.f;
        const double maxY=(high[1]*(high[1]>0?inverseNear:inverseFar)+1.f)*32.f;
        const double minDepth=std::max(low[2]*inverseFar,1.f-high[4]*inverseNear);
        if(!finite(minX)||!finite(maxX)||!finite(minY)||!finite(maxY)||!finite(minDepth)){out.area+=4096;out.nearW=0;out.potential=true;continue;}
        const double width=std::max(0.,std::min(64.,maxX)-std::max(0.,minX)),height=std::max(0.,std::min(64.,maxY)-std::max(0.,minY));
        out.area+=width*height;out.nearW=std::min(out.nearW,double(low[3]));out.potential=out.potential||(width+2e-9>=1&&height+2e-9>=1);
        auto&e=out.eye[eye];e.known=true;e.empty=width<=0||height<=0;e.nearDepth=std::max(0.,minDepth-1e-9);
        auto cell=[](double v){return static_cast<unsigned char>(std::clamp(v,0.,63.));};
        e.cx0=cell(minX-1e-9);e.cx1=cell(maxX+1e-9);e.cy0=cell(minY-1e-9);e.cy1=cell(maxY+1e-9);e.x0=e.cx0/8;e.x1=e.cx1/8;e.y0=e.cy0/8;e.y1=e.cy1/8;
    }
    replace->bounds=bounds;replace->entry=out;replace->epoch=envelopeEpoch_;return true;
}
bool PriorityOcclusionSources::coverageComplete()const noexcept {
    return budget_.earlyCoverage&&frame_&&cache_&&raster_&&cache_->invalidationSerial()==serial_&&
           raster_->stats().uniqueCoveredCells[0]==4096&&raster_->stats().uniqueCoveredCells[1]==4096;
}
bool PriorityOcclusionSources::mayContribute(const Raster::Bounds&bounds)const noexcept {
    if(coverageComplete())return false;
    if(!frame_||built_||!projection_)return true;++stats_.objectBoundQueries;Entry envelope;
    if(!planBounds(bounds,envelope)||envelope.potential)return true;
    ++stats_.earlyObjectDrops;return false;
}
bool PriorityOcclusionSources::addTarget(const Raster::Bounds&bounds,const Raster::Vec3&worldError)noexcept {
    if(coverageComplete())return false;
    if(!frame_||built_){++stats_.stale;return false;}
    if(targetCount_==MaxTargets){++stats_.targetOverflow;return false;}
    Entry envelope;
    if(!vectorFinite(worldError)||worldError.x<0||worldError.y<0||worldError.z<0||!planBounds(bounds,envelope)||
       !envelope.eye[0].known||!envelope.eye[1].known||envelope.eye[0].empty||envelope.eye[1].empty){++stats_.invalidTargets;return false;}
    // Target planning may stop source work early. Actual native candidates
    // still use their exact expanded bounds/error proof later in Raster.hidden.
    Target target;target.eye=envelope.eye;target.active=true;
    targets_[targetCount_++]=target;++activeTargets_;++stats_.targets;
    for(unsigned eye=0;eye<2;++eye)maxTargetDepth_[eye]=std::max(maxTargetDepth_[eye],target.eye[eye].nearDepth);
    return true;
}
void PriorityOcclusionSources::prepareClip(unsigned source)noexcept {
    if(clipReady_[source])return;clipReady_[source]=true;
    for(unsigned eye=0;eye<2;++eye){const auto&v=projection_->matrices()[eye];auto&m=localClip_[source][eye];const auto&w=sources_[source].world;
        for(unsigned row=0;row<4;++row)for(unsigned column=0;column<4;++column){m[row*4+column]=0;for(unsigned k=0;k<4;++k)m[row*4+column]+=v[row*4+k]*w[k*4+column];}}
}
bool PriorityOcclusionSources::enqueue(Packet&&packet)noexcept {return enqueueImpl(std::move(packet),nullptr);}
bool PriorityOcclusionSources::enqueue(Packet&&packet,const Raster::Bounds&bounds)noexcept {return enqueueImpl(std::move(packet),&bounds);}
bool PriorityOcclusionSources::enqueueImpl(Packet&&packet,const Raster::Bounds*bounds)noexcept {
    ++stats_.offered;
    if(!frame_||built_){++stats_.stale;return false;}
    if(!packet||packet.frame!=frame_||(packet.pending()&&!bounds)||
       (!packet.pending()&&(packet.mesh.nodes.empty()||packet.mesh.patchOrder.size()!=packet.mesh.patches.size()))||
       !packet.vertexHandle||!packet.indexHandle||packet.vertexHandle!=packet.identity().vertices.gpuBuffer||packet.indexHandle!=packet.identity().indexGpuBuffer||
       packet.faces==Raster::Faces::Unknown||unsigned(packet.faces)>unsigned(Raster::Faces::NegativeArea)||
       !vectorFinite(packet.worldError)||packet.worldError.x<0||packet.worldError.y<0||packet.worldError.z<0){++stats_.invalid;return false;}
    for(double v:packet.world)if(!finite(v)||double(float(v))!=v){++stats_.invalid;return false;}
    if(packet.world[12]!=0||packet.world[13]!=0||packet.world[14]!=0||packet.world[15]!=1){++stats_.invalid;return false;}
    if(cache_->invalidationSerial()!=serial_||!NativeStaticTriangles::packetCurrent(frame_,packet,*cache_)){++stats_.mutations;return false;}
    if(sourceCount_==budget_.sources){++stats_.sourceOverflow;return false;}
    const auto source=sourceCount_++;sources_[source]=std::move(packet);
    clipReady_[source]=false;Entry root;
    if(!(bounds?planBounds(*bounds,root):rank(source,0,false,root))){sources_[source].reset();--sourceCount_;++stats_.invalid;return false;}
    root.source=source;root.item=0;root.patch=false;
    if(!root.potential){push(root);sources_[source].reset();--sourceCount_;return false;}
    roots_[source]=root;++stats_.sources;
    if(!sources_[source].pending())stats_.availablePatches+=sources_[source].mesh.patches.size();
    // Optional bounded seed: an actual cheap submitted mesh, never its broad
    // rectangle, supplies depth through the SAME strict transform/error/raster.
    // Complete coverage permits omitting later refinement only; it cannot make
    // an unproven native-hidden verdict. Partial seeds remain in the global queue.
    bool coversBoth=true;for(const auto&e:root.eye)coversBoth=coversBoth&&e.known&&!e.empty&&e.cx0==0&&e.cy0==0&&e.cx1==63&&e.cy1==63;
    const auto indexCount=sources_[source].identity().indexCount;
    if(budget_.earlyCoverage&&!earlyTrialUsed_&&coversBoth&&indexCount>=3&&indexCount<=6&&indexCount%3==0){
        earlyTrialUsed_=true;++stats_.earlyTrials;auto&candidate=sources_[source];bool available=true;
        if(candidate.pending()){
            ++stats_.meshQueries;available=NativeStaticTriangles::resolvePacket(frame_,candidate,*cache_);
            if(available){++stats_.meshHits;++stats_.resolvedPackets;stats_.availablePatches+=candidate.mesh.patches.size();}else {++stats_.meshMisses;sources_[source].reset();--sourceCount_;--stats_.sources;roots_[source]={};return false;}
        }
        if(available&&candidate.mesh.patches.size()<=2&&NativeStaticTriangles::packetCurrent(frame_,candidate,*cache_)){
            earlySource_=source;
            for(unsigned i=0;i<candidate.mesh.patches.size()&&stats_.selectedPatches<budget_.patches;++i){Entry patch;patch.source=source;patch.item=i;patch.patch=true;
                earlyPatchMask_|=1u<<i;++stats_.earlyTrialPatches;submit(patch);
            }
            if(coverageComplete())++stats_.earlyFullCoverage;
        }
    }
    // Default capacity holds every admitted root. Children do not exist until
    // build, so defer both root heaps instead of 2*log(N) work per admission.
    if(budget_.queue>=budget_.sources)return true;
    return push(root);
}
bool PriorityOcclusionSources::submit(const Entry&entry)noexcept {
    const auto&packet=sources_[entry.source];const auto&mesh=packet.mesh;
    if(entry.item>=mesh.patches.size()){++stats_.invalid;return false;}
    const auto&p=mesh.patches[entry.item];Raster::Patch patch;patch.faces=packet.faces;patch.count=p.count;
    if(p.count<3||p.count>patch.vertex.size()||p.firstIndex>mesh.patchIndices.size()||p.count>mesh.patchIndices.size()-p.firstIndex){++stats_.invalid;return false;}
    for(unsigned corner=0;corner<p.count;++corner){const auto index=mesh.patchIndices[p.firstIndex+corner];if(index>=mesh.positions.size()){++stats_.invalid;return false;}
        const auto&v=mesh.positions[index];const auto&m=packet.world;auto&q=patch.vertex[corner];
        q={m[0]*v.x+m[1]*v.y+m[2]*v.z+m[3],m[4]*v.x+m[5]*v.y+m[6]*v.z+m[7],m[8]*v.x+m[9]*v.y+m[10]*v.z+m[11]};
        ++stats_.transformedVertices;if(!vectorFinite(q)){++stats_.invalid;return false;}
    }
    ++stats_.selectedPatches;stats_.sourceTriangles+=p.sourceTriangleCount;
    Raster::PreparedPatch prepared;if(!raster_->prepare(frame_,patch,prepared))return false;++stats_.prepared;
    std::array<Raster::Errors,2>errors{};
    if(!projection_->errorsSource({patch.vertex.data(),patch.count},packet.worldError,errors)){++stats_.errorRejected;return false;}
    if(!raster_->submit(frame_,prepared,errors))return false;++stats_.rasterAccepted;return true;
}
bool PriorityOcclusionSources::chargeTarget()noexcept {
    if(stats_.targetWork>=budget_.targetQueries){if(!stats_.targetBudgetStops)++stats_.targetBudgetStops;return false;}
    ++stats_.targetWork;return true;
}
bool PriorityOcclusionSources::coveredEye(const Entry::Eye&e,unsigned eye,bool budgeted)noexcept {
    if(!e.known)return false;if(e.empty)return true;
    auto charge=[&]{
        if(!budgeted)return true;
        if(!chargeTarget())return false;
        ++stats_.targetQueries;return true;
    };
    for(unsigned y=e.y0;y<=e.y1;++y)for(unsigned x=e.x0;x<=e.x1;++x){
        if(!charge())return false;++stats_.tileQueries;
        if(raster_->sourceTileDepth(frame_,eye,x,y)<e.nearDepth)continue;
        const unsigned x0=std::max(unsigned(e.cx0),x*8),x1=std::min(unsigned(e.cx1),x*8+7),y0=std::max(unsigned(e.cy0),y*8),y1=std::min(unsigned(e.cy1),y*8+7);
        if(x0==x*8&&x1==x*8+7&&y0==y*8&&y1==y*8+7)return false;
        for(unsigned cy=y0;cy<=y1;++cy)for(unsigned cx=x0;cx<=x1;++cx){
            if(!charge()||!(raster_->sourceCellDepth(frame_,eye,cx,cy)<e.nearDepth))return false;
        }
    }
    return true;
}
bool PriorityOcclusionSources::covered(const Entry&entry)noexcept {
    if(!stats_.rasterAccepted)return false;
    return coveredEye(entry.eye[0],0,false)&&coveredEye(entry.eye[1],1,false);
}
bool PriorityOcclusionSources::needed(const Entry&entry)const noexcept {
    if(!activeTargets_)return false;
    for(unsigned eye=0;eye<2;++eye){const auto&e=entry.eye[eye];
        if(!e.known||(!e.empty&&e.nearDepth<maxTargetDepth_[eye]))return true;
    }
    return false;
}
void PriorityOcclusionSources::retireTargets(const Entry&changed)noexcept {
    if(stats_.targetWork>=budget_.targetQueries)return;
    bool retired=false;
    for(unsigned i=0;i<targetCount_;++i){if(!chargeTarget())break;auto&t=targets_[i];if(!t.active)continue;bool overlap=false;
        for(unsigned eye=0;eye<2;++eye){const auto&a=t.eye[eye];const auto&b=changed.eye[eye];
            if(!b.known||(!b.empty&&a.cx0<=b.cx1&&b.cx0<=a.cx1&&a.cy0<=b.cy1&&b.cy0<=a.cy1)){overlap=true;break;}}
        if(!overlap)continue;
        if(coveredEye(t.eye[0],0,true)&&coveredEye(t.eye[1],1,true)){t.active=false;--activeTargets_;++stats_.retiredTargets;retired=true;}
        if(stats_.targetWork>=budget_.targetQueries)break;
    }
    if(retired)updateTargetDepths(true);
}void PriorityOcclusionSources::updateTargetDepths(bool budgeted)noexcept {
    std::array<double,2>maximum{};
    for(unsigned i=0;i<targetCount_;++i){if(budgeted&&!chargeTarget())return;
        if(targets_[i].active)for(unsigned eye=0;eye<2;++eye)maximum[eye]=std::max(maximum[eye],targets_[i].eye[eye].nearDepth);}
    // If budget exhausted, retaining the old larger maxima only schedules
    // extra source work. Never publish a partial maximum over active targets.
    maxTargetDepth_=maximum;
}
void PriorityOcclusionSources::reachableTargets()noexcept {
    // The fill below returns before touching targets if source rectangles
    // alone exhaust this budget. Detect that exact no-op up front: no map clear,
    // no partial fill, and all target state remains unchanged.
    std::uint64_t sourceCells=0;
    for(unsigned i=0;i<sourceCount_;++i)for(unsigned eye=0;eye<2;++eye){const auto&e=roots_[i].eye[eye];if(e.known&&e.empty)continue;
        const auto cells=e.known?unsigned(e.cx1-e.cx0+1)*unsigned(e.cy1-e.cy0+1):4096u;
        sourceCells+=cells;
        if(sourceCells>budget_.reachabilityCells-stats_.reachabilityCells){++stats_.reachabilityBudgetStops;++stats_.reachabilityPreflightStops;return;}
    }
    for(auto&eye:possibleDepth_)eye.fill(1.);
    auto charge=[&]{if(stats_.reachabilityCells>=budget_.reachabilityCells){++stats_.reachabilityBudgetStops;return false;}++stats_.reachabilityCells;return true;};
    // Optimistic coverage: a root's entire bounding rectangle is treated as
    // solid at its NEAREST possible depth, ignoring holes/facing/materials.
    // Unknown projection contributes the entire viewport at depth zero.
    // This can retain impossible targets, never certify a native hidden draw.
    for(unsigned i=0;i<sourceCount_;++i)for(unsigned eye=0;eye<2;++eye){const auto&e=roots_[i].eye[eye];if(e.known&&e.empty)continue;
        const unsigned x0=e.known?e.cx0:0,x1=e.known?e.cx1:63,y0=e.known?e.cy0:0,y1=e.known?e.cy1:63;
        // Preserve the SAME precision as target nominal depth. Rounding only
        // sources downward to float creates artificial self-occlusion potential.
        const double optimistic=e.known?e.nearDepth:0;
        const unsigned width=x1-x0+1,cells=width*(y1-y0+1);
        if(cells>budget_.reachabilityCells-stats_.reachabilityCells){++stats_.reachabilityBudgetStops;return;}
        stats_.reachabilityCells+=cells;
        // Source fill cannot exclude targets until every admitted root is in
        // the union. Charge the whole rectangle first; inner rows vectorize.
        for(unsigned y=y0;y<=y1;++y){auto*row=&possibleDepth_[eye][y*64+x0];for(unsigned x=0;x<width;++x)row[x]=std::min(row[x],optimistic);}

    }
    for(unsigned i=0;i<targetCount_;++i){auto&t=targets_[i];bool possible=true;
        for(unsigned eye=0;eye<2&&possible;++eye){const auto&e=t.eye[eye];
            for(unsigned y=e.cy0;y<=e.cy1&&possible;++y)for(unsigned x=e.cx0;x<=e.cx1;++x){
                if(!charge()){
                    // Budget is not evidence. Revert the planning exclusions;
                    // all actual candidate/native visibility rules are intact.
                    for(unsigned k=0;k<targetCount_;++k)targets_[k].active=true;
                    activeTargets_=targetCount_;stats_.unreachableTargets=0;updateTargetDepths();return;
                }
                if(!(possibleDepth_[eye][y*64+x]<e.nearDepth)){possible=false;break;}
            }
        }
        if(!possible){t.active=false;--activeTargets_;++stats_.unreachableTargets;}
    }
    updateTargetDepths();
}
bool PriorityOcclusionSources::build(std::uint64_t frame)noexcept {
    if(!frame||frame!=frame_||built_){++stats_.stale;return false;}built_=true;bool added=stats_.rasterAccepted!=0;
    if(cache_->invalidationSerial()!=serial_){++stats_.mutations;return false;}
    if(coverageComplete())return added;
    if(budget_.queue>=budget_.sources){
        queued_=sourceCount_;freeCount_=MaxQueue-queued_;
        for(unsigned i=0;i<queued_;++i){entries_[i]=roots_[i];entries_[i].maxPos=entries_[i].minPos=i;maxHeap_[i]=minHeap_[i]=i;}
        auto down=[&](unsigned p,bool maximum){auto&heap=maximum?maxHeap_:minHeap_;for(;;){unsigned best=p,left=p*2+1,right=left+1;
            if(left<queued_&&heapBetter(heap[left],heap[best],maximum))best=left;
            if(right<queued_&&heapBetter(heap[right],heap[best],maximum))best=right;
            if(best==p)return;swapHeap(p,best,maximum);p=best;}};
        for(unsigned i=queued_/2;i;--i){down(i-1,true);down(i-1,false);}
        stats_.queueHighWater=std::max(stats_.queueHighWater,std::uint64_t(queued_));
    }
    if(!activeTargets_){stats_.targetRejectedBranches+=queued_;return added;}reachableTargets();
    while(queued_){
        if(!activeTargets_){stats_.targetRejectedBranches+=queued_;break;}
        if(cache_->invalidationSerial()!=serial_){++stats_.mutations;break;}
        if(stats_.selectedPatches>=budget_.patches){++stats_.patchStops;break;}
        const auto entry=pop();auto&packet=sources_[entry.source];
        if(!NativeStaticTriangles::packetCurrent(frame_,packet,*cache_)){++stats_.mutations;continue;}
        if(!needed(entry)){++stats_.targetRejectedBranches;continue;}
        if(covered(entry)){if(entry.patch)++stats_.coveredPatches;else ++stats_.coveredNodes;continue;}
        if(packet.pending()){
            ++stats_.meshQueries;
            if(!NativeStaticTriangles::resolvePacket(frame_,packet,*cache_)){++stats_.meshMisses;continue;}
            ++stats_.meshHits;++stats_.resolvedPackets;stats_.availablePatches+=packet.mesh.patches.size();
            // Coarse object envelopes admitted the deferred packet. Qualify its
            // actual cold root before spending expansion/patch budget.
            Entry actualRoot;
            if(!rank(entry.source,0,false,actualRoot)){++stats_.invalid;continue;}
            if(!actualRoot.potential){push(actualRoot);continue;}
            if(!needed(actualRoot)){++stats_.targetRejectedBranches;continue;}
            if(covered(actualRoot)){++stats_.coveredNodes;continue;}
        }
        const auto&mesh=packet.mesh;
        if(mesh.nodes.empty()||mesh.patchOrder.size()!=mesh.patches.size()){++stats_.invalid;continue;}
        if(entry.patch){if(entry.source==earlySource_&&entry.item<2&&(earlyPatchMask_&(1u<<entry.item)))continue;const bool changed=submit(entry);added=changed||added;if(changed)retireTargets(entry);continue;}
        if(stats_.nodeExpansions>=budget_.expansions){++stats_.expansionStops;break;}++stats_.nodeExpansions;
        if(entry.item>=mesh.nodes.size()){++stats_.invalid;continue;}const auto&node=mesh.nodes[entry.item];
        if(node.count){
            ++stats_.leafExpansions;
            if(node.count>OwnedStaticOccluders::MaxBvhLeafPatches||node.first>mesh.patchOrder.size()||node.count>mesh.patchOrder.size()-node.first){++stats_.invalid;continue;}
            for(unsigned k=0;k<node.count;++k){Entry child;const auto patch=mesh.patchOrder[node.first+k];++stats_.patchCandidates;
                if(rank(entry.source,patch,true,child))push(child);else ++stats_.invalid;}
        }else{
            if(node.first>=mesh.nodes.size()||mesh.nodes.size()-node.first<2){++stats_.invalid;continue;}
            for(unsigned k=0;k<2;++k){Entry child;if(rank(entry.source,node.first+k,false,child))push(child);else ++stats_.invalid;}
        }
    }
    // Mutation invalidates authorization to use ANY previously produced cell;
    // integration also checks the same serial immediately before RIGHT reject.
    return added&&cache_->invalidationSerial()==serial_;
}
}
