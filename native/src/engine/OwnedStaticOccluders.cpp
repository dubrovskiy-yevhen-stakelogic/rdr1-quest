#include "OwnedStaticOccluders.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <mutex>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace rdrvr::engine { namespace {
using Cache=OwnedStaticOccluders;using XYZ=Cache::XYZ;using Poly=std::vector<std::uint16_t>;
struct Errno {int value{errno};~Errno(){errno=value;}};
unsigned hash(std::uintptr_t x)noexcept{x^=x>>33;x*=0xff51afd7ed558ccdull;x^=x>>33;return unsigned(x)&(Cache::TableCapacity-1);}
// Exact nonoverlapping expansions: cold predicates only. All inputs are finite
// binary32, scaled by one exact power of two. Even the smallest triple product
// is >=2^-831, so binary64/FMA cannot underflow the exact determinant residual.
using Expansion=std::vector<double>;
void add(Expansion& e,double b){double q=b;Expansion out;out.reserve(e.size()+1);for(double a:e){const double x=q+a,v=x-q;
    const double error=(q-(x-v))+(a-v);if(error)out.push_back(error);q=x;}if(q||out.empty())out.push_back(q);e=std::move(out);}
Expansion difference(double a,double b){Expansion e;add(e,a);add(e,-b);return e;}
Expansion product(const Expansion& a,const Expansion& b){Expansion out;for(double x:a)for(double y:b){const double p=x*y,r=std::fma(x,y,-p);add(out,r);add(out,p);}return out;}
int sign(const Expansion& e){for(auto p=e.rbegin();p!=e.rend();++p)if(*p)return *p>0?1:-1;return 0;}
double coord(XYZ p,unsigned axis){return axis==0?p.x:axis==1?p.y:p.z;}
double scale(const XYZ* v,unsigned n){double m=0;for(unsigned i=0;i<n;++i)m=std::max({m,std::abs(double(v[i].x)),std::abs(double(v[i].y)),std::abs(double(v[i].z))});return m?std::ldexp(1.,-std::ilogb(m)):1;}
int orient2(XYZ a,XYZ b,XYZ c,unsigned x,unsigned y){XYZ v[]{a,b,c};const double s=scale(v,3);
    const double ux=coord(a,x)*s-coord(c,x)*s,uy=coord(a,y)*s-coord(c,y)*s;
    const double vx=coord(b,x)*s-coord(c,x)*s,vy=coord(b,y)*s-coord(c,y)*s;
    const double left=ux*vy,right=uy*vx,det=left-right;
    const double error=64*std::numeric_limits<double>::epsilon()*(std::abs(left)+std::abs(right));
    // Wide forward-error filter; cancellation goes to the exact predicate.
    // Product underflow is impossible for the scaled normal-binary32 inputs.
    if(std::abs(det)>error)return det>0?1:-1;
    if(!left&&!right)return 0;
    const auto ax=difference(coord(a,x)*s,coord(c,x)*s),ay=difference(coord(a,y)*s,coord(c,y)*s);
    const auto bx=difference(coord(b,x)*s,coord(c,x)*s),by=difference(coord(b,y)*s,coord(c,y)*s);
    auto p=product(ax,by);for(double q:product(ay,bx))add(p,-q);return sign(p);}
bool coplanar(XYZ a,XYZ b,XYZ c,XYZ d){
    if((a.x==b.x&&a.x==c.x&&a.x==d.x)||(a.y==b.y&&a.y==c.y&&a.y==d.y)||(a.z==b.z&&a.z==c.z&&a.z==d.z))return true;
    XYZ v[]{a,b,c,d};const double s=scale(v,4);Expansion q[3][3];for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)q[i][j]=difference(coord(v[i],j)*s,coord(d,j)*s);
    Expansion result;constexpr unsigned perms[6][3]{{0,1,2},{1,2,0},{2,0,1},{0,2,1},{2,1,0},{1,0,2}};
    for(unsigned i=0;i<6;++i){auto p=product(product(q[0][perms[i][0]],q[1][perms[i][1]]),q[2][perms[i][2]]);for(double x:p)add(result,i<3?x:-x);}return sign(result)==0;
}
bool projection(const Poly& p,const std::vector<XYZ>& v,unsigned& x,unsigned& y,int& orientation){
    constexpr unsigned axes[3][2]{{0,1},{0,2},{1,2}};for(const auto& a:axes)for(unsigned i=1;i+1<p.size();++i){const int s=orient2(v[p[0]],v[p[i]],v[p[i+1]],a[0],a[1]);if(s){x=a[0];y=a[1];orientation=s;return true;}}return false;
}
bool simplify(const Poly& boundary,const std::vector<XYZ>& v,Poly& corners){unsigned x{},y{};int direction{};if(!projection(boundary,v,x,y,direction))return false;
    corners.clear();for(unsigned i=0;i<boundary.size();++i){const int s=orient2(v[boundary[(i+boundary.size()-1)%boundary.size()]],v[boundary[i]],v[boundary[(i+1)%boundary.size()]],x,y);
        if(s&&s!=direction)return false;if(s)corners.push_back(boundary[i]);}
    return corners.size()>=3&&corners.size()<=Cache::MaxPatchVertices;
}
struct Edge {std::uint16_t a{},b{};unsigned triangle{};};
std::pair<unsigned,unsigned> key(const Edge&e){return std::minmax(unsigned(e.a),unsigned(e.b));}
bool merge(const Poly& a,const Poly& b,const std::vector<XYZ>& v,Poly& result){
    if(a.size()+b.size()>2*Cache::MaxBoundaryVertices)return false;
    unsigned x{},y{};int direction{};if(!projection(a,v,x,y,direction))return false;
    int otherDirection=0;for(unsigned i=1;i+1<b.size()&&!otherDirection;++i)otherDirection=orient2(v[b[0]],v[b[i]],v[b[i+1]],x,y);
    if(otherDirection!=direction)return false;
    unsigned middle=1;while(middle+1<a.size()&&!orient2(v[a[0]],v[a[middle]],v[a[middle+1]],x,y))++middle;
    if(middle+1==a.size())return false;
    for(auto i:b)if(!coplanar(v[a[0]],v[a[middle]],v[a[middle+1]],v[i]))return false;
    std::vector<Edge> edges;edges.reserve(a.size()+b.size());for(const auto* p:{&a,&b})for(unsigned i=0;i<p->size();++i)edges.push_back({(*p)[i],(*p)[(i+1)%p->size()],0});
    std::sort(edges.begin(),edges.end(),[](const Edge& l,const Edge&r){return key(l)<key(r);});std::vector<Edge> outside;bool shared=false;
    for(unsigned i=0;i<edges.size();){unsigned end=i+1;while(end<edges.size()&&key(edges[end])==key(edges[i]))++end;
        if(end-i==1)outside.push_back(edges[i]);else if(end-i==2&&edges[i].a==edges[i+1].b&&edges[i].b==edges[i+1].a)shared=true;else return false;i=end;}
    if(!shared||outside.size()<3||outside.size()>Cache::MaxBoundaryVertices)return false;
    std::sort(outside.begin(),outside.end(),[](const Edge& l,const Edge&r){return l.a<r.a;});for(unsigned i=1;i<outside.size();++i)if(outside[i-1].a==outside[i].a)return false;
    result.clear();auto at=outside[0].a;for(unsigned i=0;i<outside.size();++i){if(i&&at==outside[0].a)return false;result.push_back(at);
        auto e=std::lower_bound(outside.begin(),outside.end(),at,[](const Edge& q,unsigned a){return q.a<a;});if(e==outside.end()||e->a!=at)return false;at=e->b;}
    if(at!=result[0])return false;Poly corners;if(!simplify(result,v,corners))return false;
    // Every edge belonged to exactly one input region or cancelled against its
    // opposite. One convex cycle proves the union; holes/overlaps never become
    // a filled hull. Original boundary vertices stay for later adjacent merges.
    return true;
}
struct BuildPatch {std::array<std::uint16_t,Cache::MaxPatchVertices> indices{};std::uint8_t count{};std::uint32_t sourceTriangleCount{};double area{};};
bool preprocess(const std::vector<XYZ>& v,std::span<const std::uint16_t> input,std::vector<BuildPatch>& out){
    std::vector<std::uint16_t> order(v.size()),canonical(v.size());std::iota(order.begin(),order.end(),std::uint16_t(0));
    auto less=[&](unsigned a,unsigned b){if(v[a].x!=v[b].x)return v[a].x<v[b].x;if(v[a].y!=v[b].y)return v[a].y<v[b].y;return v[a].z<v[b].z;};
    std::sort(order.begin(),order.end(),less);std::uint16_t root=order[0];for(unsigned n=0;n<order.size();++n){if(n&&less(order[n-1],order[n]))root=order[n];canonical[order[n]]=root;}
    const unsigned count=unsigned(input.size()/3);std::vector<Poly> regions(count);std::vector<unsigned> parent(count),represented(count,1);std::iota(parent.begin(),parent.end(),0u);std::vector<Edge> edges;edges.reserve(input.size());
    for(unsigned i=0;i<count;++i){for(unsigned k=0;k<3;++k){if(input[3*i+k]>=v.size())return false;regions[i].push_back(canonical[input[3*i+k]]);}for(unsigned k=0;k<3;++k)edges.push_back({regions[i][k],regions[i][(k+1)%3],i});}
    std::sort(edges.begin(),edges.end(),[](const Edge&a,const Edge&b){return key(a)<key(b);});std::vector<std::pair<unsigned,unsigned>> adjacent;
    for(unsigned i=0;i<edges.size();){unsigned end=i+1;while(end<edges.size()&&key(edges[end])==key(edges[i]))++end;
        if(end-i==2&&edges[i].a!=edges[i].b&&edges[i].a==edges[i+1].b&&edges[i].b==edges[i+1].a)adjacent.emplace_back(edges[i].triangle,edges[i+1].triangle);i=end;}
    auto find=[&](unsigned i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;};
    // Fixed passes/region boundary cap bound cold work independently of topology.
    for(unsigned pass=0;pass<8;++pass){bool changed=false;for(auto [a,b]:adjacent){a=find(a);b=find(b);if(a==b)continue;Poly combined;
            if(merge(regions[a],regions[b],v,combined)){regions[a]=std::move(combined);regions[b].clear();parent[b]=a;represented[a]+=represented[b];changed=true;}}
        if(!changed)break;}
    out.clear();out.reserve(count);for(unsigned i=0;i<count;++i)if(parent[i]==i){const auto& boundary=regions[i];Poly corners;
        if(boundary.size()==3)corners=boundary;else if(!simplify(boundary,v,corners))return false;
        BuildPatch patch;patch.count=std::uint8_t(corners.size());patch.sourceTriangleCount=represented[i];std::copy(corners.begin(),corners.end(),patch.indices.begin());double normal[3]{};const auto&a=v[corners[0]];for(unsigned k=1;k+1<corners.size();++k){const auto&b=v[corners[k]];const auto&c=v[corners[k+1]];double u[]{double(b.x)-a.x,double(b.y)-a.y,double(b.z)-a.z},w[]{double(c.x)-a.x,double(c.y)-a.y,double(c.z)-a.z};for(unsigned j=0;j<3;++j)normal[j]+=u[(j+1)%3]*w[(j+2)%3]-u[(j+2)%3]*w[(j+1)%3];}patch.area=normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2];out.push_back(patch);}
    std::stable_sort(out.begin(),out.end(),[](const BuildPatch&a,const BuildPatch&b){return a.area>b.area;});
    return !out.empty();
}
template<class T>T field(const std::array<std::byte,16>& a,unsigned offset){T value{};std::memcpy(&value,a.data()+offset,sizeof(value));return value;}
bool valid(const Cache::Identity& i){const auto&v=i.vertices;const auto format=field<std::uint64_t>(v.fvfBytes,8)&15;
    return v.vertexBuffer&&v.nativeBuffer&&v.allocation&&v.gpuBuffer&&v.fvf&&v.vertexCount&&v.vertexCount<=65535&&v.stride<=255&&v.stride%4==0&&
        (format==6||format==7)&&v.stride>=(format==6?12u:16u)&&(field<std::uint32_t>(v.fvfBytes,0)&1)&&field<std::uint8_t>(v.fvfBytes,4)==v.stride&&
        std::uint64_t(v.vertexCount)*v.stride<=v.byteSize&&i.indexBuffer&&i.nativeIndexBuffer&&i.indexAllocation&&i.indexGpuBuffer&&
        i.nativeIndexBuffer!=v.nativeBuffer&&i.indexCount&&i.indexCount%3==0&&i.indexCount/3<=Cache::MaxInputTriangles&&std::uint64_t(i.indexCount)*2<=i.indexByteSize;}
bool coordinate(float value){const auto bits=std::bit_cast<std::uint32_t>(value)&0x7fffffffu;
    // Do not let a process FTZ mode collapse distinct input positions during
    // exact welding/predicates. Unknown denormals retain the native draw.
    return bits<0x7f800000u&&(!bits||bits>=0x00800000u);}
struct ResidentBudget {std::atomic<std::uint64_t> bytes{};};
void buildBvh(const std::vector<XYZ>& positions,const std::vector<Cache::LocalPatch>& patches,
              const std::vector<std::uint16_t>& indices,std::vector<Cache::BvhNode>& nodes,
              std::vector<std::uint16_t>& order){
    struct Bounds {XYZ minimum{},maximum{},patchExtent{},patchMinMaximum{},patchMaxMinimum{};};
    const auto outwardSpan=[](float lo,float hi){
        const double delta=double(hi)-double(lo);
        const float infinity=std::numeric_limits<float>::infinity();
        return delta>std::numeric_limits<float>::max()?infinity:
            std::nextafter(static_cast<float>(delta),infinity);
    };
    std::vector<Bounds> bounds(patches.size());
    for(unsigned i=0;i<patches.size();++i){const auto& p=patches[i];auto& b=bounds[i];
        b.minimum=b.maximum=positions[indices[p.firstIndex]];
        for(unsigned k=1;k<p.count;++k){const auto& v=positions[indices[p.firstIndex+k]];
            b.minimum={std::min(b.minimum.x,v.x),std::min(b.minimum.y,v.y),std::min(b.minimum.z,v.z)};
            b.maximum={std::max(b.maximum.x,v.x),std::max(b.maximum.y,v.y),std::max(b.maximum.z,v.z)};}
        b.patchExtent={outwardSpan(b.minimum.x,b.maximum.x),outwardSpan(b.minimum.y,b.maximum.y),outwardSpan(b.minimum.z,b.maximum.z)};
        b.patchMinMaximum=b.minimum;b.patchMaxMinimum=b.maximum;}
    order.resize(patches.size());std::iota(order.begin(),order.end(),std::uint16_t(0));
    nodes.clear();nodes.reserve(Cache::bvhNodeCapacity(patches.size()));nodes.emplace_back();
    const auto build=[&](auto&& self,unsigned node,unsigned first,unsigned count)->void{
        auto b=bounds[order[first]];for(unsigned i=1;i<count;++i){const auto& q=bounds[order[first+i]];
            b.minimum={std::min(b.minimum.x,q.minimum.x),std::min(b.minimum.y,q.minimum.y),std::min(b.minimum.z,q.minimum.z)};
            b.maximum={std::max(b.maximum.x,q.maximum.x),std::max(b.maximum.y,q.maximum.y),std::max(b.maximum.z,q.maximum.z)};
            b.patchExtent={std::max(b.patchExtent.x,q.patchExtent.x),std::max(b.patchExtent.y,q.patchExtent.y),std::max(b.patchExtent.z,q.patchExtent.z)};
            b.patchMinMaximum={std::max(b.patchMinMaximum.x,q.patchMinMaximum.x),std::max(b.patchMinMaximum.y,q.patchMinMaximum.y),std::max(b.patchMinMaximum.z,q.patchMinMaximum.z)};
            b.patchMaxMinimum={std::min(b.patchMaxMinimum.x,q.patchMaxMinimum.x),std::min(b.patchMaxMinimum.y,q.patchMaxMinimum.y),std::min(b.patchMaxMinimum.z,q.patchMaxMinimum.z)};}
        const float infinity=std::numeric_limits<float>::infinity();
        const XYZ minUpper{std::nextafter(b.patchMinMaximum.x,infinity),std::nextafter(b.patchMinMaximum.y,infinity),std::nextafter(b.patchMinMaximum.z,infinity)};
        const XYZ maxLower{std::nextafter(b.patchMaxMinimum.x,-infinity),std::nextafter(b.patchMaxMinimum.y,-infinity),std::nextafter(b.patchMaxMinimum.z,-infinity)};
        nodes[node]={b.minimum,b.maximum,first,count,b.patchExtent,minUpper,maxLower};if(count<=Cache::MaxBvhLeafPatches)return;
        unsigned axis=0;for(unsigned k=1;k<3;++k)
            if(coord(b.maximum,k)-coord(b.minimum,k)>coord(b.maximum,axis)-coord(b.minimum,axis))axis=k;
        const unsigned left=count/2;
        std::nth_element(order.begin()+first,order.begin()+first+left,order.begin()+first+count,[&](unsigned a,unsigned c){
            const double x=coord(bounds[a].minimum,axis)+coord(bounds[a].maximum,axis);
            const double y=coord(bounds[c].minimum,axis)+coord(bounds[c].maximum,axis);
            return x!=y?x<y:a<c;});
        const unsigned child=unsigned(nodes.size());nodes.resize(nodes.size()+2);
        nodes[node].first=child;nodes[node].count=0;
        self(self,child,first,left);self(self,child+1,first+left,count-left);
    };
    build(build,0,0,unsigned(patches.size()));nodes.shrink_to_fit();order.shrink_to_fit();
}
}
struct OwnedStaticOccluders::SharedData {
    std::vector<XYZ> positions;std::vector<LocalPatch> patches;
    std::vector<std::uint16_t> patchIndices,patchOrder;std::vector<BvhNode> nodes;
    std::shared_ptr<ResidentBudget> budget;std::uint64_t accountedBytes{};mutable std::atomic<bool> current{};
    ~SharedData(){if(accountedBytes)budget->bytes.fetch_sub(accountedBytes,std::memory_order_release);}
};
struct OwnedStaticOccluders::Impl {
    struct Record {Identity identity{};std::shared_ptr<const SharedData> data;std::uint64_t generation{},bytes{};};
    struct Member {std::atomic<std::uintptr_t> key{};std::atomic<unsigned> references{},writers{},pending{};};
    mutable std::shared_mutex mutex;std::array<Record,MaxEntries> records{};std::array<Member,TableCapacity> members{};std::array<unsigned,TableCapacity> lookup{};
    std::shared_ptr<ResidentBudget> budget=std::make_shared<ResidentBudget>();
    std::atomic<std::uint64_t> epoch{},serial{},mutation{};std::atomic<unsigned> pendingWriters{};std::atomic<bool> quarantine{};std::uint64_t incarnation{},generation{},entries{},bytes{},captures{},invalidations{},evictions{},declined{},inputTriangles{},patches{},mergedTriangles{};
    Member* member(std::uintptr_t key){unsigned at=hash(key);for(unsigned n=0;n<MaxProbe;++n,at=(at+1)&(TableCapacity-1))if(members[at].key.load(std::memory_order_acquire)==key)return &members[at];return nullptr;}
    Member* reserve(std::uintptr_t key){if(auto*m=member(key)){m->references.fetch_add(1,std::memory_order_release);return m;}unsigned at=hash(key);
        for(unsigned n=0;n<MaxProbe;++n,at=(at+1)&(TableCapacity-1)){auto&m=members[at];if(m.references.load(std::memory_order_relaxed)||m.pending.load(std::memory_order_relaxed))continue;m.references.store(1,std::memory_order_release);m.key.store(key,std::memory_order_release);return &m;}return nullptr;}
    unsigned slot(const Identity&i)const{return hash(i.vertices.nativeBuffer^(i.nativeIndexBuffer<<1));}
    void drop(unsigned index){auto&r=records[index];if(!r.generation)return;
        // Published grids can outlive this record via an immutable Snapshot.
        // Revoke their authorization BEFORE dropping mutation membership: a
        // later native write may legitimately find this backing untracked.
        serial.fetch_add(1,std::memory_order_acq_rel);
        r.data->current.store(false,std::memory_order_release);
        for(auto key:{r.identity.vertices.nativeBuffer,r.identity.nativeIndexBuffer})if(auto*m=member(key))m->references.fetch_sub(1,std::memory_order_release);
        auto at=slot(r.identity);for(unsigned n=0;n<MaxProbe;++n,at=(at+1)&(TableCapacity-1))if(lookup[at]==index+1)lookup[at]=0;
        bytes-=r.bytes;--entries;r={};}
    void erase(std::uintptr_t key){for(unsigned i=0;i<MaxEntries;++i)if(records[i].generation&&(records[i].identity.vertices.nativeBuffer==key||records[i].identity.nativeIndexBuffer==key))drop(i);}
    void reset(){for(auto&r:records){if(r.data)r.data->current.store(false,std::memory_order_release);r={};}for(auto&m:members){m.references.store(0,std::memory_order_release);m.writers.store(0,std::memory_order_release);m.key.store(0,std::memory_order_release);}lookup.fill(0);bytes=entries=0;quarantine.store(false);}
    unsigned oldest()const{unsigned index=MaxEntries;auto g=UINT64_MAX;for(unsigned i=0;i<MaxEntries;++i)if(records[i].generation&&records[i].generation<g){index=i;g=records[i].generation;}return index;}
};
OwnedStaticOccluders::Snapshot::Snapshot(Snapshot&& other)noexcept{*this=std::move(other);}
OwnedStaticOccluders::Snapshot& OwnedStaticOccluders::Snapshot::operator=(Snapshot&& other)noexcept{
    if(this!=&other){reset();data_=std::move(other.data_);identity=other.identity;epoch=other.epoch;generation=other.generation;
        byteSize=other.byteSize;incarnation_=other.incarnation_;serial_=other.serial_;positions=other.positions;patches=other.patches;
        patchIndices=other.patchIndices;patchOrder=other.patchOrder;nodes=other.nodes;other.reset();}return *this;
}
void OwnedStaticOccluders::Snapshot::reset()noexcept{
    positions={};patches={};patchIndices={};patchOrder={};nodes={};identity={};epoch=generation=byteSize=incarnation_=serial_=0;data_.reset();
}
OwnedStaticOccluders::Lease::Lease(Lease&& other)noexcept{*this=std::move(other);}
OwnedStaticOccluders::Lease& OwnedStaticOccluders::Lease::operator=(Lease&& other)noexcept{if(this!=&other){reset();identity=other.identity;positions=other.positions;patches=other.patches;patchIndices=other.patchIndices;epoch=other.epoch;generation=other.generation;incarnation_=other.incarnation_;serial_=other.serial_;owner_=other.owner_;lock_=std::move(other.lock_);other.reset();}return *this;}
void OwnedStaticOccluders::Lease::reset()noexcept{identity={};positions={};patches={};patchIndices={};epoch=generation=incarnation_=serial_=0;owner_=nullptr;lock_={};}
OwnedStaticOccluders::OwnedStaticOccluders():impl_(std::make_unique<Impl>()){}OwnedStaticOccluders::~OwnedStaticOccluders()=default;
OwnedStaticOccluders ownedStaticOccluders;
bool OwnedStaticOccluders::bind(std::uint64_t epoch)noexcept{Errno preserve;if(!epoch)return false;try{auto&s=*impl_;std::unique_lock lock(s.mutex);if(auto current=s.epoch.load())return current==epoch;s.serial.fetch_add(1);s.mutation.fetch_add(1);s.reset();++s.incarnation;s.epoch.store(epoch,std::memory_order_release);return true;}catch(...){return false;}}
void OwnedStaticOccluders::clear()noexcept{Errno preserve;auto&s=*impl_;s.serial.fetch_add(1);s.mutation.fetch_add(1);try{std::unique_lock lock(s.mutex);s.epoch.store(0,std::memory_order_release);++s.incarnation;s.reset();}catch(...){}}
bool OwnedStaticOccluders::capture(std::uint64_t epoch,const Identity&id,const void*mapped,std::size_t vertexBytes,const std::uint16_t*indices,std::size_t indexBytes,bool qualified,std::uint64_t expectedMutationSerial)noexcept{
    Errno preserve;try{auto&s=*impl_;std::unique_lock lock(s.mutex);if(!epoch||s.epoch.load()!=epoch||s.mutation.load()!=expectedMutationSerial)return false;
        if(id.vertices.nativeBuffer)s.erase(id.vertices.nativeBuffer);
        const auto vb=std::uint64_t(id.vertices.vertexCount)*id.vertices.stride,ib=std::uint64_t(id.indexCount)*2;
        if(s.quarantine.load()||s.pendingWriters.load()||!qualified||!valid(id)||!mapped||!indices||vb>vertexBytes||ib>indexBytes||reinterpret_cast<std::uintptr_t>(mapped)>UINTPTR_MAX-vb||reinterpret_cast<std::uintptr_t>(indices)>UINTPTR_MAX-ib){++s.declined;return false;}
        if(auto*m=s.member(id.nativeIndexBuffer);m&&m->writers.load()){++s.declined;return false;}
        Impl::Member* vm=s.reserve(id.vertices.nativeBuffer),*im=vm?s.reserve(id.nativeIndexBuffer):nullptr;
        if(!vm||!im){if(vm)vm->references.fetch_sub(1);++s.declined;return false;}
        const auto mutation=s.mutation.load(std::memory_order_acquire);
        try{
            Impl::Record candidate;candidate.identity=id;auto data=std::make_shared<SharedData>();data->budget=s.budget;data->positions.resize(id.vertices.vertexCount);
            const auto* source=static_cast<const std::byte*>(mapped);for(unsigned i=0;i<id.vertices.vertexCount;++i){std::memcpy(&data->positions[i],source+std::size_t(i)*id.vertices.stride,sizeof(XYZ));const auto&v=data->positions[i];if(!coordinate(v.x)||!coordinate(v.y)||!coordinate(v.z))throw false;}
            std::vector<std::uint16_t> copied(id.indexCount);std::memcpy(copied.data(),indices,std::size_t(ib));std::vector<BuildPatch> built;if(!preprocess(data->positions,copied,built))throw false;
            data->patches.reserve(built.size());data->patchIndices.reserve(copied.size());for(const auto&p:built){data->patches.push_back({std::uint32_t(data->patchIndices.size()),std::uint16_t(p.sourceTriangleCount),p.count,0});data->patchIndices.insert(data->patchIndices.end(),p.indices.begin(),p.indices.begin()+p.count);}
            data->positions.shrink_to_fit();data->patches.shrink_to_fit();data->patchIndices.shrink_to_fit();
            buildBvh(data->positions,data->patches,data->patchIndices,data->nodes,data->patchOrder);
            candidate.bytes=sizeof(SharedData)+data->positions.capacity()*sizeof(XYZ)+data->patches.capacity()*sizeof(LocalPatch)+
                (data->patchIndices.capacity()+data->patchOrder.capacity())*sizeof(std::uint16_t)+data->nodes.capacity()*sizeof(BvhNode);
            if(candidate.bytes>MaxBytes)throw false;
            // Eviction cannot reclaim a packet's shared snapshot. Account all
            // live immutable payloads, including invalidated/cleared snapshots.
            while(s.entries==MaxEntries||s.budget->bytes.load(std::memory_order_acquire)+candidate.bytes>MaxBytes){const unsigned victim=s.oldest();if(victim==MaxEntries)throw false;s.drop(victim);++s.evictions;}
            auto at=s.slot(id);unsigned target=TableCapacity;for(unsigned n=0;n<MaxProbe;++n,at=(at+1)&(TableCapacity-1))if(!s.lookup[at]){target=at;break;}if(target==TableCapacity)throw false;
            if(s.mutation.load(std::memory_order_acquire)!=mutation||s.pendingWriters.load()||im->writers.load()||s.quarantine.load())throw false;
            unsigned index=0;while(index<MaxEntries&&s.records[index].generation)++index;candidate.generation=++s.generation;s.bytes+=candidate.bytes;++s.entries;++s.captures;
            data->accountedBytes=candidate.bytes;s.budget->bytes.fetch_add(candidate.bytes,std::memory_order_release);data->current.store(true,std::memory_order_release);candidate.data=std::move(data);
            s.inputTriangles+=id.indexCount/3;s.patches+=candidate.data->patches.size();s.mergedTriangles+=id.indexCount/3-candidate.data->patches.size();s.records[index]=std::move(candidate);s.lookup[target]=index+1;return true;
        }catch(...){vm->references.fetch_sub(1,std::memory_order_release);im->references.fetch_sub(1,std::memory_order_release);++s.declined;return false;}
    }catch(...){return false;}}void OwnedStaticOccluders::invalidate(std::uintptr_t key)noexcept{
    Errno preserve;if(!key)return;auto&s=*impl_;if(!s.epoch.load(std::memory_order_acquire))return;
    auto*m=s.member(key);if(!m||m->references.load(std::memory_order_acquire)<=m->writers.load(std::memory_order_acquire))return;
    m->pending.fetch_add(1,std::memory_order_acq_rel);s.mutation.fetch_add(1,std::memory_order_acq_rel);s.serial.fetch_add(1,std::memory_order_acq_rel);
    try{std::unique_lock lock(s.mutex);s.erase(key);++s.invalidations;}catch(...){s.quarantine.store(true);}m->pending.fetch_sub(1,std::memory_order_release);
}
void OwnedStaticOccluders::beginIndexMutation(std::uintptr_t key)noexcept{
    Errno preserve;if(!key)return;auto&s=*impl_;if(!s.epoch.load(std::memory_order_acquire))return;
    auto*tracked=s.member(key);if(tracked&&tracked->references.load(std::memory_order_acquire)>tracked->writers.load(std::memory_order_acquire)){
        tracked->pending.fetch_add(1,std::memory_order_acq_rel);s.serial.fetch_add(1,std::memory_order_acq_rel);
    }else tracked=nullptr;
    s.pendingWriters.fetch_add(1,std::memory_order_acq_rel);s.mutation.fetch_add(1,std::memory_order_acq_rel);
    try{std::unique_lock lock(s.mutex);if(s.epoch.load()){auto*m=s.reserve(key);if(!m)s.quarantine.store(true);else{m->writers.fetch_add(1,std::memory_order_release);if(m->references.load()>m->writers.load())s.erase(key);}++s.invalidations;}}catch(...){s.quarantine.store(true);}
    if(tracked)tracked->pending.fetch_sub(1,std::memory_order_release);s.pendingWriters.fetch_sub(1,std::memory_order_release);
}
void OwnedStaticOccluders::endIndexMutation(std::uintptr_t key)noexcept{
    Errno preserve;if(!key)return;auto&s=*impl_;if(!s.epoch.load(std::memory_order_acquire))return;auto*known=s.member(key);if(!known||!known->writers.load(std::memory_order_acquire))return;s.mutation.fetch_add(1,std::memory_order_acq_rel);
    try{std::unique_lock lock(s.mutex);auto*m=s.member(key);if(!m||!m->writers.load())return;m->writers.fetch_sub(1,std::memory_order_release);m->references.fetch_sub(1,std::memory_order_release);}catch(...){s.quarantine.store(true);}
}bool OwnedStaticOccluders::query(std::uint64_t epoch,const Identity&id,Lease&out)const noexcept{Errno preserve;out.reset();try{auto&s=*impl_;if(!epoch||s.epoch.load(std::memory_order_acquire)!=epoch||s.quarantine.load())return false;std::shared_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock()||s.epoch.load()!=epoch||s.quarantine.load())return false;
    const auto querySerial=s.serial.load(std::memory_order_acquire);
    for(auto key:{id.vertices.nativeBuffer,id.nativeIndexBuffer})if(auto*m=s.member(key);m&&(m->pending.load()||m->writers.load()))return false;
    auto at=s.slot(id);for(unsigned n=0;n<MaxProbe;++n,at=(at+1)&(TableCapacity-1)){const auto index=s.lookup[at];if(!index)continue;const auto&r=s.records[index-1];if(r.identity!=id||!r.generation)continue;
        out.identity=id;out.epoch=epoch;out.generation=r.generation;out.positions=r.data->positions;out.patches=r.data->patches;out.patchIndices=r.data->patchIndices;out.owner_=&s;out.incarnation_=s.incarnation;out.serial_=querySerial;out.lock_=std::move(lock);return true;}return false;}catch(...){return false;}}
bool OwnedStaticOccluders::querySnapshot(std::uint64_t epoch,const Identity&id,Snapshot&out)const noexcept{
    Errno preserve;out.reset();try{auto&s=*impl_;if(!epoch||s.epoch.load(std::memory_order_acquire)!=epoch||s.quarantine.load())return false;
        std::shared_lock lock(s.mutex,std::try_to_lock);if(!lock.owns_lock()||s.epoch.load()!=epoch||s.quarantine.load())return false;
        const auto serial=s.serial.load(std::memory_order_acquire);
        for(auto key:{id.vertices.nativeBuffer,id.nativeIndexBuffer})if(auto*m=s.member(key);m&&(m->pending.load()||m->writers.load()))return false;
        auto at=s.slot(id);for(unsigned n=0;n<MaxProbe;++n,at=(at+1)&(TableCapacity-1)){const auto index=s.lookup[at];if(!index)continue;
            const auto&r=s.records[index-1];if(r.identity!=id||!r.generation)continue;
            out.data_=r.data;out.identity=id;out.epoch=epoch;out.generation=r.generation;out.byteSize=r.bytes;
            out.incarnation_=s.incarnation;out.serial_=serial;out.positions=r.data->positions;out.patches=r.data->patches;
            out.patchIndices=r.data->patchIndices;out.patchOrder=r.data->patchOrder;out.nodes=r.data->nodes;
            if(current(out))return true;out.reset();return false;}return false;
    }catch(...){out.reset();return false;}
}
bool OwnedStaticOccluders::current(const Snapshot&s)const noexcept{
    if(!s.data_||!s.data_->current.load(std::memory_order_acquire)||s.data_->budget!=impl_->budget||s.epoch!=impl_->epoch.load(std::memory_order_acquire)||
       impl_->quarantine.load()||s.serial_!=impl_->serial.load(std::memory_order_acquire))return false;
    for(auto key:{s.identity.vertices.nativeBuffer,s.identity.nativeIndexBuffer})if(auto*m=impl_->member(key);m&&(m->pending.load()||m->writers.load()))return false;
    return s.serial_==impl_->serial.load(std::memory_order_acquire);
}
bool OwnedStaticOccluders::current(const Lease&l)const noexcept{
    if(!l.lock_.owns_lock()||l.owner_!=impl_.get()||l.epoch!=impl_->epoch.load()||l.incarnation_!=impl_->incarnation||impl_->quarantine.load()||l.serial_!=impl_->serial.load(std::memory_order_acquire))return false;
    for(auto key:{l.identity.vertices.nativeBuffer,l.identity.nativeIndexBuffer})if(auto*m=impl_->member(key);m&&(m->pending.load()||m->writers.load()))return false;return true;
}
std::uint64_t OwnedStaticOccluders::invalidationSerial()const noexcept{return impl_->serial.load(std::memory_order_acquire);}
std::uint64_t OwnedStaticOccluders::mutationSerial()const noexcept{return impl_->mutation.load(std::memory_order_acquire);}
OwnedStaticOccluders::Statistics OwnedStaticOccluders::statistics()const noexcept{Errno preserve;try{auto&s=*impl_;std::shared_lock lock(s.mutex);return {s.epoch.load(),s.entries,s.bytes,s.captures,s.invalidations,s.evictions,s.declined,s.inputTriangles,s.patches,s.mergedTriangles,s.budget->bytes.load(std::memory_order_acquire)};}catch(...){return {};}}
}








