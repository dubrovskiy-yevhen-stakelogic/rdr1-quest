// Host regression (MSVC: cl /std:c++20 /EHsc /I<fake sys/system_properties.h> /ID:/RDRVRBuild/fast/generated /Inative/src /Inative/src/vr):
// HAND PHYSICS: the palm frame, the drive against a surface (a toy wall with the solver's allowed penetration), the contact estimate, the
// visual stop, the hand classes, the body state machine (park, in-place return), the pair filter, the collision flags, the gates, the own
// phInst vtable, the behaviour list writes on a fake simulator, the install refusal and whole ticks with substeps against a stub engine:
// the PALM composite build, a wall, prop wakes on contact, REACH WAKE against a stub level, doors, the impact guard and its refusals.
// DOOR SWING (the pure swing step, the leaf geometry, a stub pivot door driven by the palm), the prop unlock in CollideObjects, the NPC
// virtual body (pushOut, the command clip, press bumps, the open-hand touches) and the bump / press / slap gates. HAND PHYSICS v4: the leaf
// follow (pure: the leaf frame, the target, the open axis, the remap; a stub door that the tick opens, rests, poses and shuts), the root
// release of small props, the touch classifier and its requests, the grab (nearest part, the drive, the release debounce, a stub contact
// manager with exactly one release per hold) and the per-mode CPU time.
#include <chrono>
#include <limits>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "vr/VrHandPhys.h"
#include "vr/VrMeleeMath.h"
namespace P=rdrvr::vr::handphys;
namespace MM=rdrvr::vr::melee::math;
namespace GL=rdrvr::vr::grablink;
namespace D=rdrvr::vr::handphys::detail;
namespace MT=rdrvr::vr::mainthread;
namespace V=rdrvr::vr::handphys::v;
static int fails=0;
#define CHECK(c,msg) do{if(!(c)){std::printf("FAIL %s  (%s)\n",msg,#c);++fails;}else std::printf("ok   %s\n",msg);}while(0)
static bool near(float a,float b,float eps=1e-4f){return std::fabs(a-b)<=eps;}
static bool nearV(P::V3 a,P::V3 b,float eps=1e-4f){return near(a[0],b[0],eps)&&near(a[1],b[1],eps)&&near(a[2],b[2],eps);}
static bool nearRows(const P::Rows& a,const P::Rows& b,float eps){return nearV(a[0],b[0],eps)&&nearV(a[1],b[1],eps)&&nearV(a[2],b[2],eps);}
static constexpr std::uint64_t ms=1'000'000ull;
static const P::Rows identity{{{1,0,0},{0,1,0},{0,0,1}}};

// ---------- stub engine ----------
namespace stub {
alignas(16) static unsigned char sim[0x200],level[0x100],levelObjects[16*0x30],mover[0xa00],moverArch[0x80],propMgr[0x40];
alignas(16) static unsigned char colliders[16][0x300];
alignas(16) static unsigned char actorMgr[0x10],actorSlotTable[16*16],playerActor[0x100],npcActor[0x80],npcHealth[0x158];
// GLASS BLOOD: sagBloodManager::smp_Instance's object (+0x210 the switch), the camera position and fov scale words, TriggerPtFxMelee's calls
alignas(16) static unsigned char bloodMgr[0x220]{},creature[0xa40]{};static float camPos[3]{},fovScaleSqr{1.f};static bool bloodRule=true;
struct FxCall {const void* self{};const void* actor{};std::uint32_t fx{},fx2{};const void* mtx{};float pos[4]{},n[4]{},dir[4]{};};
static int fxCalls=0;static FxCall lastFx{};
alignas(16) static unsigned char clockMgr[0x40],contactMgr[0x40],constraints[4][0x150];
static int attaches=0,releases=0,fixedRotations=0,inertias=0,setStates=0,pathCalls=0,continueCalls=0,lastSetState=-1;static bool failAttach=false;
static int failAttachAt=-1; // GRAB WELD: the attach with this running count is refused (-1 none)
static void* lastFixedRotation=nullptr;
static std::uintptr_t levelColliders[16],behList[32],behSlots[16];static std::uint16_t behIdx[32];
static constexpr std::uintptr_t base=0x7a0000000000ull;
static float wallX=1.0f,halfDepth=.014f,allowedPen=.007f;static int adds=0,deletes=0,teleports=0,velocities=0,doorOpens=0,doorCloses=0,failAdd=0;static bool delayDelete=false;
static int addsUnguarded=0,forwards=0,pcForwards=0;static const void* lastIt=nullptr;static const void* pcLastIt=nullptr;
// GLASS: sm_CreateCount, sagFragInst::CollisionDamage (a call made; a pane created unless refused), its Collision
static std::int32_t glassCreateCount=0;static int glassCalls=0;static bool glassRefuse=false;alignas(16) static unsigned char lastCollision[0x60]{};static const void* lastGlassIt=nullptr;
static float* glassHealthDrop=nullptr;static float glassHealthDropBy=1.f; // a group health the call lowers (the count may be zeroed by UpdateBuffers in between)
static void collisionDamageFn(void*,const void* c,void* it){++glassCalls;std::memcpy(lastCollision,c,0x60);lastGlassIt=it;if(!glassRefuse)++glassCreateCount;if(glassHealthDrop)*glassHealthDrop-=glassHealthDropBy;}
static int fragClass(const void*){return 5;}
// GUN GLASS: SweptSphereMultipleExcludeInsts's stand-in: the prepared hits copied out (at most maxHits), the arguments recorded; probeThrow
// makes it throw
struct ProbeCall {float seg[8]{};float r{};int maxHits{},nExcl{},include{},typeExclude{};unsigned state{};std::vector<const void*> excl;};
static int probeCalls=0;static ProbeCall lastProbe{};alignas(16) static unsigned char probeHits[4*0x70]{};static int probeCount=0;static bool probeThrow=false;
static int probeLevelFn(const void* seg,float r,int maxHits,void* isect,int nExcl,const void* const* excl,int include,int typeExclude,unsigned char state){
    ++probeCalls;std::memcpy(lastProbe.seg,seg,32);lastProbe.r=r;lastProbe.maxHits=maxHits;lastProbe.nExcl=nExcl;lastProbe.include=include;
    lastProbe.typeExclude=typeExclude;lastProbe.state=state;lastProbe.excl.assign(excl,excl+nExcl);if(probeThrow)throw 1;
    const int n=std::min(probeCount,maxHits);std::memcpy(isect,probeHits,std::size_t(n)*0x70);return n;}
static void probeHit(int i,std::uintptr_t inst,P::V3 pos,P::V3 n,std::uint16_t part,std::uint16_t comp){
    unsigned char* h=probeHits+std::size_t(i)*0x70;std::memset(h,0,0x70);std::memcpy(h,&inst,8);const float p4[4]{pos[0],pos[1],pos[2],0},n4[4]{n[0],n[1],n[2],0};
    std::memcpy(h+0x10,p4,16);std::memcpy(h+0x20,n4,16);std::memcpy(h+0x64,&part,2);std::memcpy(h+0x6a,&comp,2);}
static std::uintptr_t addr(const void* p){return reinterpret_cast<std::uintptr_t>(p);}
template<class T> static void put(void* b,std::size_t o,T v){std::memcpy(static_cast<unsigned char*>(b)+o,&v,sizeof v);}
template<class T> static T at(const void* b,std::size_t o){T v;std::memcpy(&v,static_cast<const unsigned char*>(b)+o,sizeof v);return v;}
// BLOOD ROUTE: the blood_push rule (vtable word +0, draw list byte +0x37d), rmPtfxManager (+0x4f0 lists, +0x4f8 count), its 4 list objects
// (head +0), effect slots, CPtFxManager's low-res switch; skipCreate = GetEffectInst returning null
alignas(16) static unsigned char ruleBuf[0x400]{},rmBuf[0x500]{},listObjs[4][0x40]{},fxSlots[8][0x400]{};static std::uintptr_t listPtrs[4]{};
static std::uint8_t lowResByte=0;static int fxSlotNext=0;static bool skipCreate=false;static int seenListByte=-1;static std::uintptr_t rmPtr=0;
static void meleeFxFn(void* self,const void* actor,std::uint32_t fx,std::uint32_t fx2,const void* mtx,const void* pos,const void* n,const void* dir){
    ++fxCalls;lastFx.self=self;lastFx.actor=actor;lastFx.fx=fx;lastFx.fx2=fx2;lastFx.mtx=mtx;std::memcpy(lastFx.pos,pos,16);std::memcpy(lastFx.n,n,16);std::memcpy(lastFx.dir,dir,16);
    seenListByte=ruleBuf[0x37d];if(skipCreate)return;
    unsigned char* slot=fxSlots[fxSlotNext++&7];std::memset(slot,0,0x400);put<std::uintptr_t>(slot,0x170,addr(ruleBuf));
    const unsigned count=at<std::uint16_t>(rmBuf,0x4f8);const unsigned l=seenListByte<int(count)?unsigned(seenListByte):0u;
    put<std::uintptr_t>(slot,0,at<std::uintptr_t>(listObjs[l],0));put<std::uintptr_t>(listObjs[l],0,addr(slot));}
static void* ruleFn(void*,std::uint32_t h){return bloodRule&&h==P::bloodPushFx?ruleBuf:nullptr;}
static void bloodRouteSetup(std::uint8_t list,std::uint8_t lowRes){
    std::memset(ruleBuf,0,sizeof ruleBuf);put<std::uintptr_t>(ruleBuf,0,base+P::off::ruleStdVtable);ruleBuf[0x37d]=list;put<float>(ruleBuf,0x344,1.f);put<float>(ruleBuf,0x348,30.f);
    std::memset(rmBuf,0,sizeof rmBuf);std::memset(listObjs,0,sizeof listObjs);for(unsigned i=0;i<4;++i)listPtrs[i]=addr(listObjs[i]);
    put<std::uintptr_t>(rmBuf,0x4f0,addr(listPtrs));put<std::uint16_t>(rmBuf,0x4f8,4);rmPtr=addr(rmBuf);lowResByte=lowRes;skipCreate=false;seenListByte=-1;}
// RESETHOLD: the GOH slot table (count u16, 16-byte entries: gohBase +0, generation u16 +8)
static std::uint16_t gohCount=0;alignas(16) static unsigned char gohTable[16*16]{};
static bool read(void*,std::uintptr_t a,void* out,std::size_t n){
    const auto word=[&](std::uintptr_t v){if(n!=sizeof v)return false;std::memcpy(out,&v,n);return true;};
    if(a==base+P::off::gohCountVar)return word(addr(&gohCount));
    if(a==base+P::off::gohTableVar)return word(addr(gohTable));
    if(a==base+P::off::simGot)return word(base+P::off::simVar);
    if(a==base+P::off::simVar)return word(addr(sim));
    if(a==base+P::off::levelGot)return word(base+P::off::levelVar);
    if(a==base+P::off::levelVar)return word(addr(level));
    if(a==base+P::off::propGot)return word(base+P::off::propVar);
    if(a==base+P::off::propVar)return word(addr(propMgr));
    if(a==base+P::off::clothFlag||a==base+P::off::resCtorFlag){std::memset(out,0,n);return true;} // both 0: the box constructor allocates nothing
    if(a==base+P::off::actorManager)return word(addr(actorMgr));
    if(a==base+P::off::actorSlots)return word(addr(actorSlotTable));
    // sysTimeManager::sm_GlobalManager is an object at clockVar: its time float +0x3c is read in place (no pointer at clockVar)
    if(a==base+P::off::clockVar+0x3c&&n==4){std::memcpy(out,clockMgr+0x3c,4);return true;}
    if(a==base+P::off::createCount&&n==4){std::memcpy(out,&glassCreateCount,4);return true;} // bgGlassManager::sm_CreateCount
    if(a==base+P::off::bloodMgrVar)return word(addr(bloodMgr));
    if(a==base+P::off::camPosVar&&n==12){std::memcpy(out,camPos,12);return true;}
    if(a==base+P::off::fovScaleVar&&n==4){std::memcpy(out,&fovScaleSqr,4);return true;}
    if(a==base+P::off::ptFxMgrVar+P::off::ptFxLowRes&&n==1){std::memcpy(out,&lowResByte,1);return true;}
    if(a==base+P::off::rmPtfxVar)return word(rmPtr);
    if(a>=base&&a<base+0x60000000ull)return false;
    if(a<0x100000u)return false; // the test actor 0x4242 and other fake small addresses: unreadable, like an unmapped page
    std::memcpy(out,reinterpret_cast<const void*>(a),n);return true;}
static bool readZero(void*,std::uintptr_t,void* out,std::size_t n){std::memset(out,0,n);return true;}
static void nop1(void*){}
static void archCtor(void* arch){put<float>(arch,0x44,500.f);put<float>(arch,0x48,4.f*P::pi);}
static void setBound(void* arch,void* bound){put<std::uintptr_t>(arch,0x18,addr(bound));}
static void nopf(void*,float){}
static void nopff(void*,float,float){}
static void instCtor(void* inst){put<std::uintptr_t>(inst,0,base+P::off::instVtable+0x10);put<std::uint32_t>(inst,0x10,0xffffu);put<std::uintptr_t>(inst,8,0);put<std::uintptr_t>(inst,0x18,0);}
// sagInstData::sagInstData(phInst*) 0x28a8eec: vptr, 0, inst, u32 0xffff00ff (+0x20 not written)
static void instDataCtor(void* u,void* inst){put<std::uintptr_t>(u,0,base+P::off::instDataVtable+0x10);put<std::uintptr_t>(u,8,0);put<std::uintptr_t>(u,0x10,addr(inst));put<std::uint32_t>(u,0x18,0xffff00ffu);}
static void badInstDataCtor(void* u,void* inst){instDataCtor(u,inst);put<std::uint32_t>(u,0x18,0u);}
static void preApplyGame(const void* it){++forwards;lastIt=it;}
static void preComputeDefault(const void* it){++pcForwards;pcLastIt=it;}
static void instInit(void* inst,void* arch,const void* m){put<std::uintptr_t>(inst,8,addr(arch));std::memcpy(static_cast<unsigned char*>(inst)+0x20,m,64);}
static int addActive(void* simp,void* inst,bool){
    ++adds;if(at<std::uintptr_t>(simp,P::off::simPreApply)!=D::guardWord())++addsUnguarded;
    if(failAdd){--failAdd;return 0xffff;}
    for(unsigned i=0;i<8;++i)if(!at<std::uintptr_t>(levelObjects,i*0x30+0x20)){ // slots 0..7 for the hands, 8..15 for the props
        put<std::uintptr_t>(levelObjects,i*0x30+0x20,addr(inst));levelColliders[i]=addr(colliders[i]);std::memset(colliders[i],0,sizeof colliders[i]);
        put<std::uintptr_t>(colliders[i],0x10,addr(inst));put<std::uint16_t>(inst,0x10,std::uint16_t(i));
        const float in[4]{.01f,.003f,.013f,2.f};std::memcpy(colliders[i]+0xa0,in,16);return int(i);}
    return 0xffff;}
static void deleteObject(void*,int i,bool){++deletes;if(delayDelete)return;
    const auto inst=at<std::uintptr_t>(levelObjects,std::size_t(i)*0x30+0x20)&~std::uintptr_t(0xf);
    put<std::uintptr_t>(levelObjects,std::size_t(i)*0x30+0x20,0);levelColliders[i]=0;if(inst)put<std::uint16_t>(reinterpret_cast<void*>(inst),0x10,0xffff);}
static bool teleport(void*,void* inst,const void* m,const void*){++teleports;std::memcpy(static_cast<unsigned char*>(inst)+0x20,m,64);return true;}
static void setVelocity(void* col,const void* v){++velocities;std::memcpy(static_cast<unsigned char*>(col)+0x110,v,16);
    float m{};std::memcpy(&m,static_cast<unsigned char*>(col)+0xac,4);float q[4];std::memcpy(q,v,16);for(float& x:q)x*=m;q[3]=0.f;std::memcpy(static_cast<unsigned char*>(col)+0x130,q,16);}
// UpdateVelocityFromImpulseImp 0x1c659f8 with a zero impulse: +0x110 = momentum x inverse mass
static void impulseStep(void* col){float m{};std::memcpy(&m,static_cast<unsigned char*>(col)+0xac,4);float q[4];std::memcpy(q,static_cast<unsigned char*>(col)+0x130,16);
    for(float& x:q)x=m>0.f?x/m:0.f;q[3]=0.f;std::memcpy(static_cast<unsigned char*>(col)+0x110,q,16);}
static void setAngVelocity(void* col,const void* v){std::memcpy(static_cast<unsigned char*>(col)+0x120,v,16);}
alignas(16) static unsigned char doorHandle[16],doorInst[0xb0],doorIface[0x40];static std::uint32_t doorStateValue=0;
static int doorFastCloses=0;static float lastPusher[3]{};
static void* getDoor(void*,void* nav){put<std::uintptr_t>(doorHandle,0,addr(doorInst));put<std::uint32_t>(doorInst,0x58,doorStateValue);
    put<std::uintptr_t>(doorInst,0x78,addr(nav));return doorHandle;}
static void* doorGet(const void* h){return reinterpret_cast<void*>(at<std::uintptr_t>(h,0));}
static bool doorClosed(const void* d){return at<std::uint32_t>(d,0x58)<2;}
// propDoorInst::Open: state 0 only; the side byte from the pusher against the closed row c, StartOpenDerived's speed (t kept).
// A door without an interface keeps the state the test gives it.
static void doorOpen(void* d,const void* q,bool){++doorOpens;std::memcpy(lastPusher,q,12);
    if(doorStateValue!=0||!at<std::uintptr_t>(d,0x50))return;doorStateValue=2;put<std::uint32_t>(d,0x58,2u);
    if(const auto i=at<std::uintptr_t>(d,0x50)){float pos[3],rc[3];std::memcpy(pos,static_cast<unsigned char*>(d)+0x30,12);std::memcpy(rc,static_cast<unsigned char*>(d)+0x20,12);
        const float side=(pos[0]-lastPusher[0])*rc[0]+(pos[1]-lastPusher[1])*rc[1]+(pos[2]-lastPusher[2])*rc[2];
        put<std::uint8_t>(reinterpret_cast<void*>(i),8,side>0.f?1:0);put<float>(reinterpret_cast<void*>(i),0x20,.9f);}}
// propDoorInst::Close(false): state 4 only -> 5, StartCloseDerived's speed 2
static void doorClose(void* d,bool){++doorCloses;if(doorStateValue!=4)return;doorStateValue=5;put<std::uint32_t>(d,0x58,5u);
    if(const auto i=at<std::uintptr_t>(d,0x50))put<float>(reinterpret_cast<void*>(i),0x20,2.f);}
// The leaf posed from the phase as ContinueActionShared does it: closed rows rotated about the hinge (the open axis from the side byte:
// the leaf swings away from the pusher), the nav matrix written (pivot offsets 0).
static bool poseLeaf=false;
static void poseNav(){
    const auto i=at<std::uintptr_t>(doorInst,0x50);const auto nav=at<std::uintptr_t>(doorInst,0x78);if(!i||!nav||!poseLeaf)return;void* f=reinterpret_cast<void*>(i);
    const unsigned dir=at<std::uint8_t>(f,8)?1u:0u;const float t=std::clamp(at<float>(f,0x24),0.f,1.f);const float cap=at<float>(f,0x28+4*dir);if(cap<0.f)return;
    const float th=(doorStateValue==5?P::smooth(t):P::easeOpen(t))*cap;
    float cm[12];std::memcpy(cm,doorInst,48);const P::V3 a0{cm[0],cm[1],cm[2]},c0{cm[8],cm[9],cm[10]};
    const P::V3 k=V::normalized(V::cross(a0,dir?c0:V::mul(c0,-1.f)));const P::V3 a=V::rotate(V::mul(k,th),a0),c=V::rotate(V::mul(k,th),c0);
    const float rows[8]{a[0],a[1],a[2],0,c[0],c[1],c[2],0};
    std::memcpy(reinterpret_cast<unsigned char*>(nav)+0x20,rows,16);std::memcpy(reinterpret_cast<unsigned char*>(nav)+0x40,rows+4,16);}
static void doorSetStateFn(void* d,int st,bool b){++setStates;lastSetState=st;doorStateValue=unsigned(st);put<std::uint32_t>(d,0x58,unsigned(st));put<std::uint8_t>(d,0xa1,b?1:0);}
static void pathStateFn(void*){++pathCalls;}
static bool capOnContinue=false; // ContinueActionShared resolves a pending limit (+0x28[dir] < 0) to 1.6 rad, as GetOperationRanges does
static bool continueSharedFn(void*,void*){++continueCalls;
    if(capOnContinue)if(const auto i=at<std::uintptr_t>(doorInst,0x50)){void* f=reinterpret_cast<void*>(i);const unsigned dir=at<std::uint8_t>(f,8)?1u:0u;
        if(at<float>(f,0x28+4*dir)<0.f)put<float>(f,0x28+4*dir,1.6f);}
    poseNav();return true;}
// GRAB: phContactMgr::AttachObjects / ReleaseConstraint (a pool of 4), SetFixedRotation, phCollider::SetInertia
static void* attachFn(void* mgr,std::uint32_t,const void*,const void*,void* a,void* b,int ca,int,float,bool,void*){
    ++attaches;if(failAttach||mgr!=contactMgr||attaches==failAttachAt)return nullptr;
    for(auto& c:constraints)if(!at<std::uintptr_t>(c,0x8)){std::memset(c,0,sizeof c);put<std::uintptr_t>(c,0x8,addr(a));put<std::uintptr_t>(c,0x10,addr(b));
        put<std::uintptr_t>(c,0x18,0x5000);put<std::int32_t>(c,0xb0,ca);put<std::uint16_t>(c,0xbc,2);return c;}
    return nullptr;}
static void releaseConstraintFn(void*,void* c){++releases;put<std::uintptr_t>(c,0x8,0);put<std::uintptr_t>(c,0x10,0);}
static void fixedRotationFn(void* c){++fixedRotations;lastFixedRotation=c;put<std::uint16_t>(c,0xbc,std::uint16_t(at<std::uint16_t>(c,0xbc)|3u));}
static void setInertiaFn(void* col,const float* mass,P::Quad q){++inertias;float v[4];std::memcpy(v,q.v,16);v[3]=*mass;std::memcpy(static_cast<unsigned char*>(col)+0xa0,v,16);}
// phCollider::ApplyForce (the accumulator is consumed by the step, which the stub does not run: nothing is stored)
static int forces=0,lastForceComp=-9;static float lastForce[4]{},lastForcePos[4]{};
static void applyForceFn(void*,P::Quad f,P::Quad p,int comp){++forces;std::memcpy(lastForce,f.v,16);std::memcpy(lastForcePos,p.v,16);lastForceComp=comp;}
static void doorFastClose(void* d,bool){++doorFastCloses;doorStateValue=0;put<std::uint32_t>(d,0x58,0u);
    if(const auto i=at<std::uintptr_t>(d,0x50))put<float>(reinterpret_cast<void*>(i),0x24,0.f);}
// propDoorInst::Update of one frame for a pivot door: the cap resolved once, t by speed, state 2 -> 3 at 0.8, -> 4 at 1; closing -> 0 at 0
static void doorUpdate(float dt){
    const auto i=at<std::uintptr_t>(doorInst,0x50);if(!i)return;void* f=reinterpret_cast<void*>(i);
    const unsigned dir=at<std::uint8_t>(f,8)?1u:0u;float t=at<float>(f,0x24);const float sp=at<float>(f,0x20);
    const unsigned before=doorStateValue;
    if(doorStateValue==2||doorStateValue==3){if(at<float>(f,0x28+4*dir)<0.f)put<float>(f,0x28+4*dir,1.6f);
        t=std::min(t+sp*dt,1.f);if(t>=1.f)doorStateValue=4;else if(doorStateValue==2&&t>=.8f)doorStateValue=3;}
    else if(doorStateValue==5){t=std::max(t-sp*dt,0.f);if(t<=0.f)doorStateValue=0;}
    put<float>(f,0x24,t);
    if(before==2||before==3||before==5){const auto keep=doorStateValue;doorStateValue=before;poseNav();doorStateValue=keep;}
    put<std::uint32_t>(doorInst,0x58,doorStateValue);}
// fragInst::IsActivationDisabled / SetDisableActivation as the engine has them
static bool isActDisabled(const void* f){const auto w=at<std::uint32_t>(f,0x80);if(w&0x100u)return (w&0x80u)!=0;
    const auto t78=at<std::uintptr_t>(f,0x78);std::uintptr_t type=0;
    if(t78){if((at<std::uint32_t>(reinterpret_cast<void*>(t78),0xc)&0x30000000u)!=0x30000000u)return false;type=at<std::uintptr_t>(reinterpret_cast<void*>(t78),0);}
    else type=at<std::uintptr_t>(f,0x98);
    return type&&(at<std::uint16_t>(reinterpret_cast<void*>(type),0x1ba)&0x20u);}
static int unlockCalls=0;
static void setDisableAct(void* f,bool b){++unlockCalls;auto w=at<std::uint32_t>(f,0x80);w=b?(w|0x180u):((w&~0x180u)|0x100u);put<std::uint32_t>(f,0x80,w);}
// PALM: the box / composite as the engine lays them out (vptr, size +0x110; parts +0xa0, matrices +0xa8 = +0xb0, count +0xd0)
static int boxCtors=0,compInits=0,compExtents=0;
alignas(16) static unsigned char compArrays[2][0x200];static int compArrayNext=0;
static void boxCtor(void* box,short*,const void* size){++boxCtors;put<std::uintptr_t>(box,0,base+P::off::boxVtable+0x10);std::memcpy(static_cast<unsigned char*>(box)+0x110,size,16);}
static void boxSize(void* box,const void* size){std::memcpy(static_cast<unsigned char*>(box)+0x110,size,16);}
static void compCtor(void* c){put<std::uintptr_t>(c,0,base+P::off::compVtable+0x10);}
static void compInit(void* c,int n,bool){++compInits;auto* a=compArrays[compArrayNext++&1];std::memset(a,0,0x200);
    put<std::uintptr_t>(c,0xa0,addr(a));put<std::uintptr_t>(c,0xa8,addr(a+0x40));put<std::uintptr_t>(c,0xb0,addr(a+0x40));put<std::uint16_t>(c,0xd0,std::uint16_t(n));put<std::uint16_t>(c,0xd2,std::uint16_t(n));}
static void compSetBound(void* c,int i,void* b,bool){put<std::uintptr_t>(reinterpret_cast<void*>(at<std::uintptr_t>(c,0xa0)),std::size_t(i)*8,addr(b));}
static void compExtentsFn(void*,bool){++compExtents;}
static void setAngInertia(void* arch,const void* v){std::memcpy(static_cast<unsigned char*>(arch)+0x50,v,12);}
// Props: GetClassType of the fake vtables (also what the cull and the class words compare)
static int navClass(const void*){return P::classNav;}
static int deadClass(const void*){return P::classDeadBody;}
static int worldClass(const void*){return 0;}
static int forceActivations=0;static bool refuseActivation=false;
static int levelIndexOf(std::uintptr_t inst){for(int i=0;i<16;++i)if((at<std::uintptr_t>(levelObjects,std::size_t(i)*0x30+0x20)&~std::uintptr_t(0xf))==inst)return i;return -1;}
static void forceActivation(void* inst){++forceActivations;if(refuseActivation)return;const int i=levelIndexOf(addr(inst));
    if(i>=0)put<std::uintptr_t>(levelObjects,std::size_t(i)*0x30+0x20,at<std::uintptr_t>(levelObjects,std::size_t(i)*0x30+0x20)&~std::uintptr_t(3));}
// propPropIterator::StartCullSphere: the level's objects in state 0 / 1 whose bounding sphere meets the sphere, classes 5 and 9..12
static int culls=0;static float lastCullR=0.f;static P::V3 lastCull{};
static void propCull(void* it,const void* c,float r){++culls;float cc[3];std::memcpy(cc,c,12);lastCull={cc[0],cc[1],cc[2]};lastCullR=r;
    int n=0;put<std::uint32_t>(it,0x400,0);
    for(int i=0;i<16&&n<0x80;++i){const auto w=at<std::uintptr_t>(levelObjects,std::size_t(i)*0x30+0x20);const auto inst=w&~std::uintptr_t(0xf);if(!inst||(w&3u)>1)continue;
        float sp[4];std::memcpy(sp,levelObjects+std::size_t(i)*0x30,16);
        if(V::length(V::sub({sp[0],sp[1],sp[2]},lastCull))>=r+sp[3])continue;
        const auto vt=at<std::uintptr_t>(reinterpret_cast<void*>(inst),0);if(!vt)continue;
        const int cls=reinterpret_cast<int(*)(const void*)>(at<std::uintptr_t>(reinterpret_cast<void*>(vt),0x10))(reinterpret_cast<void*>(inst));
        if(cls==5||(cls>=9&&cls<=12))put<std::uintptr_t>(it,std::size_t(n++)*8,inst);}
    put<std::int32_t>(it,0x404,n);}
// NPC bump: the actor manager / slot table, the player and one creature, aActor::Broadcast
static int bumpsSent=0;static std::uintptr_t bumpActor=0;static std::uint32_t bumpMsg[2]{};
static void broadcastBump(void* a,const void* m){++bumpsSent;bumpActor=addr(a);std::memcpy(bumpMsg,m,8);}
static constexpr std::uint32_t playerGuid=(1u<<16)|3u,npcGuid=(2u<<16)|5u;
static void actors(){std::memset(actorMgr,0,sizeof actorMgr);std::memset(actorSlotTable,0,sizeof actorSlotTable);put<std::uint16_t>(actorMgr,0,16);
    put<std::uintptr_t>(actorSlotTable,3*16,addr(playerActor));put<std::uint16_t>(actorSlotTable,3*16+8,1);put<std::uint32_t>(playerActor,8,playerGuid);
    put<std::uintptr_t>(actorSlotTable,5*16,addr(npcActor));put<std::uint16_t>(actorSlotTable,5*16+8,2);bumpsSent=0;bumpActor=0;
    std::memset(npcHealth,0,sizeof npcHealth);put<std::uintptr_t>(npcActor,0x60,addr(npcHealth));put<float>(npcHealth,0x20,100.f);}
static void reset(){
    std::memset(sim,0,sizeof sim);std::memset(level,0,sizeof level);std::memset(levelObjects,0,sizeof levelObjects);std::memset(levelColliders,0,sizeof levelColliders);
    std::memset(behList,0,sizeof behList);std::memset(behSlots,0,sizeof behSlots);
    put<std::uintptr_t>(sim,P::off::simLevel,addr(level));put<std::uintptr_t>(sim,P::off::simBehList,addr(behList));put<std::uintptr_t>(sim,0x68,addr(behIdx));
    put<std::uint16_t>(sim,P::off::simBehCount,2);put<std::uint16_t>(sim,P::off::simBehCap,32);put<std::uintptr_t>(sim,P::off::simBehSlots,addr(behSlots));
    behList[0]=0x1111;behList[1]=0x2222; // two engine behaviours already there
    put<std::uint16_t>(level,P::off::levelCount,16);put<std::uintptr_t>(level,P::off::levelObjects,addr(levelObjects));put<std::uintptr_t>(level,P::off::levelColliders,addr(levelColliders));
    put<std::uint16_t>(level,P::off::levelActive,40);put<std::uint16_t>(level,P::off::levelCap,500);
    std::memset(mover,0,sizeof mover);std::memset(moverArch,0,sizeof moverArch);
    put<std::uintptr_t>(mover,0,base+P::off::moverSimVtable);put<std::uintptr_t>(mover,P::off::moverInst,base+P::off::moverInstVtable);
    put<std::uintptr_t>(mover,P::off::moverInst+P::off::instArch,addr(moverArch));put<std::uint32_t>(moverArch,0x20,0x10003u);put<std::uint32_t>(moverArch,0x24,0xc29ffbu);
    const float pos[3]{0.f,0.f,0.f};std::memcpy(mover+P::off::moverPos,pos,12);
    // the simulator's impact functors as phSimulator() / sagSimulator() leave them
    put<std::uintptr_t>(sim,0xc8,base+P::off::defaultPreCompute);put<std::uintptr_t>(sim,0xd8,0);put<std::uintptr_t>(sim,0xe0,base+P::off::impactThunk);
    put<std::uintptr_t>(sim,0xe8,base+P::off::preApplyGame);put<std::uintptr_t>(sim,0xf8,0);put<std::uintptr_t>(sim,0x100,base+P::off::impactThunk);
    put<std::uintptr_t>(sim,0x10,addr(contactMgr));std::memset(constraints,0,sizeof constraints);put<float>(clockMgr,0x3c,500.f);
    attaches=releases=fixedRotations=inertias=setStates=pathCalls=continueCalls=0;lastSetState=-1;failAttach=false;failAttachAt=-1;lastFixedRotation=nullptr;poseLeaf=false;
    adds=deletes=teleports=velocities=doorOpens=doorCloses=failAdd=0;doorFastCloses=0;unlockCalls=0;std::memset(doorIface,0,sizeof doorIface);put<std::uintptr_t>(doorInst,0x50,0);boxCtors=compInits=compExtents=0;forwards=0;lastIt=nullptr;pcForwards=0;pcLastIt=nullptr;delayDelete=false;forceActivations=0;refuseActivation=false;culls=0;doorStateValue=0;
}
static std::uintptr_t fakeStock[P::instTableWords]{};
static void bind(){
    P::Engine e{};e.base=base;e.read=&read;
    e.archCtor=&archCtor;e.setBound=&setBound;e.setMass=&nopf;e.setGravity=&nopf;e.sphereCtor=&nopf;e.capsuleCtor=&nop1;e.capsuleSize=&nopff;
    e.instCtor=&instCtor;e.instInit=&instInit;e.addActive=&addActive;e.deleteObject=&deleteObject;e.teleport=&teleport;
    e.setVelocity=&setVelocity;e.setAngVelocity=&setAngVelocity;e.getDoor=&getDoor;e.doorGet=&doorGet;e.doorClosed=&doorClosed;e.doorOpen=&doorOpen;
    e.instDataCtor=&instDataCtor;e.preApplyGame=&preApplyGame;e.preComputeDefault=&preComputeDefault;
    e.boxCtor=&boxCtor;e.boxSize=&boxSize;e.compCtor=&compCtor;e.compInit=&compInit;e.compSetBound=&compSetBound;e.compExtents=&compExtentsFn;e.setAngInertia=&setAngInertia;
    e.forceActivation=&forceActivation;e.doorClose=&doorClose;e.propCull=&propCull;e.broadcastBump=&broadcastBump;
    e.doorFastClose=&doorFastClose;e.isActDisabled=&isActDisabled;e.setDisableAct=&setDisableAct;
    e.navClassFn=addr(reinterpret_cast<const void*>(&navClass));e.deadClassFn=addr(reinterpret_cast<const void*>(&deadClass));
    e.doorSetState=&doorSetStateFn;e.pathState=&pathStateFn;e.continueShared=&continueSharedFn;
    e.attach=&attachFn;e.releaseConstraint=&releaseConstraintFn;e.fixedRotation=&fixedRotationFn;e.setInertia=&setInertiaFn;e.applyForce=&applyForceFn;
    e.collisionDamage=&collisionDamageFn;e.fragClassFn=addr(reinterpret_cast<const void*>(&fragClass));
    P::engine=e;D::buildTable();P::groupOk.fill(true);P::touchReady.store(true);GL::resetForTests();
    for(std::size_t i=0;i<P::instTableWords;++i)fakeStock[i]=base+0x1000+i*8;fakeStock[0]=0;
    P::buildInstTable(fakeStock,reinterpret_cast<std::uintptr_t>(&D::instNoDeactivate),P::instTable);
    for(auto& o:P::objects)o.compReady=false;
    P::ready.store(true);P::installState.store("INSTALLED");
}
static std::uint16_t count(){return at<std::uint16_t>(sim,P::off::simBehCount);}
static std::uintptr_t fnWord(const void* s){return at<std::uintptr_t>(s,P::off::simPreApply);}
// "Other" instances of pairs and props: a vtable whose +0x10 slot is GetClassType.
alignas(16) static void* navVtable[8]{},*deadVtable[8]{},*worldVtable[8]{};
struct Prop {alignas(16) unsigned char inst[0x200]{};unsigned char arch[0x40]{};};
static Prop props[8];static Prop otherProp;
static void initVtables(){navVtable[2]=reinterpret_cast<void*>(&navClass);deadVtable[2]=reinterpret_cast<void*>(&deadClass);worldVtable[2]=reinterpret_cast<void*>(&worldClass);}
// A prop at level slot `slot`: class, bounding sphere, state (0 active, 1 inactive), door byte, infinite mass bit.
static void placeProp(Prop& p,int slot,void** vt,P::V3 c,float r,unsigned state,bool door=false,bool infinite=false,std::uint32_t f80=0x3u){
    std::memset(p.inst,0,sizeof p.inst);put<std::uintptr_t>(p.inst,0,addr(vt));put<std::uintptr_t>(p.inst,8,addr(p.arch));put<std::uint32_t>(p.arch,0x20,0x1c0007u);
    put<std::uint32_t>(p.inst,0x80,f80);
    put<std::uint16_t>(p.inst,0x10,std::uint16_t(slot));put<std::uint8_t>(p.inst,0x1be,door?1:0);put<std::uint8_t>(p.inst,0x82,infinite?0x80:0);
    const float m[4]{c[0],c[1],c[2],0.f};std::memcpy(p.inst+0x50,m,16);
    const float sp[4]{c[0],c[1],c[2],r};std::memcpy(levelObjects+std::size_t(slot)*0x30,sp,16);
    put<std::uintptr_t>(levelObjects,std::size_t(slot)*0x30+0x20,addr(p.inst)|state);}
static unsigned propState(int slot){return unsigned(at<std::uintptr_t>(levelObjects,std::size_t(slot)*0x30+0x20)&3u);}
static void setPropState(int slot,unsigned st){auto w=at<std::uintptr_t>(levelObjects,std::size_t(slot)*0x30+0x20);put<std::uintptr_t>(levelObjects,std::size_t(slot)*0x30+0x20,(w&~std::uintptr_t(3))|st);}
static void clearProps(){for(int i=8;i<16;++i){std::memset(levelObjects+std::size_t(i)*0x30,0,0x30);}}
}

// ---------- toy wall: integrate, then the solver's push of (depth - allowed penetration), optionally capped (a limited solver) ----------
struct WallRun {float maxDepthPre{},minDepthSolved{1e9f},maxDepthSolved{},meanSolved{},stdSolved{};int holdSubsteps{},holdContacts{},releaseSubsteps{-1};bool pushedThrough{};};
static WallRun toyWall(bool newLaw,float pushCap){
    const float dt=1.f/72.f,wallX=1.f,pen=.007f;const P::V3 n{-1,0,0};
    P::ContactEst est{};float x=.9f;WallRun r{};std::vector<float> hold;
    const int inSteps=int(.4f/(1.f*dt))+1,holdSteps=int(2.f/dt),outSteps=int(.6f/dt);
    float target=.9f;bool outside=false;int sinceOut=-1;
    for(int k=0;k<inSteps+holdSteps+outSteps;++k){
        if(k<inSteps)target=std::min(1.3f,target+1.f*dt);else if(k>=inSteps+holdSteps)target-=1.f*dt;
        const P::V3 vel=newLaw?P::substepDrive({x,0,0},{target,0,0},dt,6.f,{0,0,0},est.n,est.age,false,.10f,1.5f,3.f).vel:
                               P::velocityTo({x,0,0},{target,0,0},dt,6.f).vel;
        const float expected=x+vel[0]*dt;x=expected;
        const float pre=x-wallX;r.maxDepthPre=std::max(r.maxDepthPre,pre);
        if(pre>pen){float push=pre-pen;if(pushCap>0.f)push=std::min(push,pushCap);x-=push;}
        const auto note=P::noteSubstep(est,{x-expected,0,0},{0,0,0},vel,.0005f,.05f);
        const float depth=x-wallX;
        if(k>=inSteps+18&&k<inSteps+holdSteps){r.holdSubsteps++;if(note.kind!=P::ContactNone)r.holdContacts++;hold.push_back(depth);
            r.minDepthSolved=std::min(r.minDepthSolved,depth);r.maxDepthSolved=std::max(r.maxDepthSolved,depth);}
        if(depth>.05f)r.pushedThrough=true;
        if(k>=inSteps+holdSteps&&!outside&&target<wallX+pen){outside=true;sinceOut=0;}
        if(outside){if(note.kind==P::ContactNone&&r.releaseSubsteps<0)r.releaseSubsteps=sinceOut;++sinceOut;}
    }
    double m=0;for(float h:hold)m+=h;m/=std::max<std::size_t>(hold.size(),1);double v=0;for(float h:hold)v+=(h-m)*(h-m);v/=std::max<std::size_t>(hold.size(),1);
    r.meanSolved=float(m);r.stdSolved=float(std::sqrt(v));(void)n;
    return r;
}

// ---------- give: an active prop in the broadphase all the time (the worst case for the drive). surface 0 = a static wall solved in full
// (depth - pen), 1 = a static wall solved by 20 % of (depth - pen) per substep (a bias solver), 2 = a light prop that takes 80 % of each push
// and moves away, until it reaches a backstop 30 cm on, where it stops like a wall. ----------
struct GiveRun {float maxDepth{},holdDepth{},maxGive{},giveAtStop{},propTravel{};int rises{},drops{};bool pushedThrough{};};
static GiveRun toyGive(int surface){
    const float dt=1.f/72.f,pen=.007f;const P::V3 n{-1,0,0};
    P::ContactEst est{};P::YieldTrack y{};float x=.9f,wall=1.f;const float stop=1.3f;GiveRun r{};float target=.9f;
    for(int k=0;k<int(3.f/dt);++k){
        target=std::min(2.f,target+1.f*dt);
        const P::V3 vel=P::substepDrive({x,0,0},{target,0,0},dt,6.f,{0,0,0},est.n,y.on?0u:est.age,y.give,.10f,1.5f,3.f).vel;
        const float expected=x+vel[0]*dt;x=expected;
        const float depth=x-wall;
        if(depth>pen){
            float push=depth-pen;
            if(surface==1)push*=.2f;
            if(surface==2&&wall<stop){const float moved=std::min(push*.8f,stop-wall);wall+=moved;push-=moved;}
            x-=push;}
        const auto note=P::noteSubstep(est,{x-expected,0,0},{0,0,0},vel,.0005f,.05f);
        const float adv=std::max(0.f,-V::dot(vel,est.n))*dt;const float g0=y.give;
        P::yieldStep(y,true,note.kind!=P::ContactNone||(est.age<255&&V::dot({x-expected,0,0},est.n)>P::yieldTouchM),V::dot(est.n,{x,0,0}),adv,.02f);
        if(y.give>g0)r.rises++;else if(g0>0.f&&y.give==0.f)r.drops++;
        r.maxGive=std::max(r.maxGive,y.give);
        const float d=x-wall;r.maxDepth=std::max(r.maxDepth,d);if(d>.05f)r.pushedThrough=true;
        if(surface==2&&wall>=stop&&r.giveAtStop==0.f)r.giveAtStop=-1.f;
        if(k==int(3.f/dt)-1){r.holdDepth=d;r.giveAtStop=y.give;}
    }
    r.propTravel=wall-1.f;(void)n;return r;
}

// ---------- the world: one frame = hook A targets, the tick, two substeps (PreUpdate, integrate against the wall, Update) ----------
static std::uint64_t simNs=10'000'000'000ull,simIndex=1000;
static P::V3 eye{0.f,1.6f,0.f};
static float squeezeNow=0.f;static P::Rows rowsNow{{{1,0,0},{0,1,0},{0,0,1}}};static P::V3 handVel{0,0,0};static std::uintptr_t actorAddr=0x4242;
// the arm's own velocity (HandTarget::velArm): the hand's velocity unless armOverride (the player walking: the arm still)
static bool armOverride=false;static P::V3 armVel{0,0,0};
// GUN GLASS: the right hand's gun fields; otherGun = the left hand holds the firearm (its muzzle / bore); bridged / on foot of the frame
static std::uint8_t gunNow=0,gunBusy=0,gunSource=1;static std::int8_t gunKind=-1;static P::V3 tipRelNow{},boreRelNow{},tipVelNow{};static bool tipOkNow=true;
static bool otherGun=false;static P::V3 otherTip{},otherBore{};static std::uint8_t bridgedNow=0,onFootNow=1;
static void publish(P::V3 anchor,P::HandClass cls,bool valid=true){
    P::Targets t{};t.serial=simIndex;t.ns=simNs;t.mover=stub::addr(stub::mover);t.actor=actorAddr;t.moverPos={0,0,0};t.eye=eye;t.up={0,1,0};t.upm=1.f;t.onFoot=onFootNow;
    t.bridged=bridgedNow;
    auto& h=t.hand[1];h.rel=anchor;h.vel=handVel;h.rows=rowsNow;h.cls=cls;h.valid=valid?1:0;h.squeeze=squeezeNow;h.velArm=armOverride?armVel:handVel;h.armOk=1;
    h.gun=gunNow;h.busy=gunBusy;h.source=gunSource;h.kind=gunKind;h.tipRel=tipRelNow;h.boreRel=boreRelNow;h.tipVelArm=tipVelNow;h.tipOk=tipOkNow?1:0;
    if(otherGun){auto& o=t.hand[0];o.valid=1;o.cls=P::ClassHeld;o.gun=1;o.source=2;o.kind=1;o.tipRel=otherTip;o.boreRel=otherBore;o.tipOk=1;o.rel=otherBore;}
    P::targets.store(t);}
static float moverSpeedZ=0.f; // the stub mover moves inside the step (as a collider does), between PreUpdate and Update
static bool wallOn=true;
// The stub door's leaf as a wall (its current plane, over its reach): the hand's side -1 (the closed side, -c) or +1 (+c); 0 = none.
static int leafSide=0;
static void leafWall(float* p){
    float m[16];std::memcpy(m,stub::otherProp.inst+0x20,64);
    const P::V3 a{m[0],m[1],m[2]},c{m[8],m[9],m[10]},o{m[12],m[13],m[14]};const P::V3 d=V::sub({p[0],p[1],p[2]},o);
    const float x=V::dot(d,a);if(x<0.f||x>.9f)return;const float z=V::dot(d,c);
    const float depth=leafSide<0?z+stub::halfDepth:stub::halfDepth-z;
    if(depth>stub::allowedPen&&depth<.2f){const float s=(depth-stub::allowedPen)*(leafSide<0?-1.f:1.f);p[0]+=c[0]*s;p[1]+=c[1]*s;p[2]+=c[2]*s;}}
static void substeps(){
    const float dt=1.f/72.f;
    for(int k=0;k<2;++k){
        for(unsigned h=0;h<2;++h)D::preUpdate(&P::objects[h].beh,dt);
        {float mp[3];std::memcpy(mp,stub::mover+P::off::moverPos,12);mp[2]+=moverSpeedZ*dt;std::memcpy(stub::mover+P::off::moverPos,mp,12);}
        for(unsigned h=0;h<2;++h){
            const auto inst=stub::addr(P::objects[h].inst);const auto idx=stub::at<std::uint16_t>(P::objects[h].inst,0x10);
            if(idx==0xffff)continue;
            float v[4];std::memcpy(v,stub::colliders[idx]+0x110,16);
            float p[3];std::memcpy(p,reinterpret_cast<const void*>(inst+0x50),12);
            for(int i=0;i<3;++i)p[i]+=v[i]*dt;
            // the wall at wallX (face of the body at p + halfDepth): pushed back to the allowed penetration
            const float depth=p[0]+stub::halfDepth-stub::wallX;
            if(wallOn&&!D::state().hand[h].ghost&&depth>stub::allowedPen)p[0]-=depth-stub::allowedPen;
            if(leafSide&&h==1&&!D::state().hand[h].ghost)leafWall(p);
            std::memcpy(reinterpret_cast<void*>(inst+0x50),p,12);std::memcpy(stub::colliders[idx]+0x100,p,12);}
        for(unsigned h=0;h<2;++h)D::update(&P::objects[h].beh,dt);}
}
static void frame(P::V3 anchor,P::HandClass cls=P::ClassBody,bool valid=true){
    simNs+=28*ms;++simIndex;publish(anchor,cls,valid);
    P::tick(MT::Frame{simNs,simIndex});
    substeps();}
static void staleFrame(){simNs+=28*ms;++simIndex;P::tick(MT::Frame{simNs,simIndex});substeps();} // no new hook A targets this frame
static P::HandSolved solvedRight(){P::Solved s{};P::solved.load(s);return s.hand[1];}
static void swingDoors(P::Levers& l){l.doorFollow=false;}
static P::V3 randUnit(std::mt19937& g){std::normal_distribution<float> d;return V::normalized({d(g),d(g),d(g)});}

// A strict JSON syntax check (objects, arrays, strings, numbers, true / false / null) for the report.
static bool jsonValue(const std::string& j,std::size_t& i);
static void jsonWs(const std::string& j,std::size_t& i){while(i<j.size()&&(j[i]==' '||j[i]=='\n'||j[i]=='\t'||j[i]=='\r'))++i;}
static bool jsonString(const std::string& j,std::size_t& i){if(i>=j.size()||j[i]!='"')return false;++i;
    while(i<j.size()&&j[i]!='"'){if(static_cast<unsigned char>(j[i])<0x20)return false;if(j[i]=='\\'){++i;if(i>=j.size())return false;}++i;}
    if(i>=j.size())return false;++i;return true;}
static bool jsonValue(const std::string& j,std::size_t& i){
    jsonWs(j,i);if(i>=j.size())return false;const char c=j[i];
    if(c=='{'){++i;jsonWs(j,i);if(i<j.size()&&j[i]=='}'){++i;return true;}
        for(;;){jsonWs(j,i);if(!jsonString(j,i))return false;jsonWs(j,i);if(i>=j.size()||j[i]!=':')return false;++i;if(!jsonValue(j,i))return false;jsonWs(j,i);
            if(i<j.size()&&j[i]==','){++i;continue;}if(i<j.size()&&j[i]=='}'){++i;return true;}return false;}}
    if(c=='['){++i;jsonWs(j,i);if(i<j.size()&&j[i]==']'){++i;return true;}
        for(;;){if(!jsonValue(j,i))return false;jsonWs(j,i);if(i<j.size()&&j[i]==','){++i;continue;}if(i<j.size()&&j[i]==']'){++i;return true;}return false;}}
    if(c=='"')return jsonString(j,i);
    for(const char* w:{"true","false","null"}){const std::size_t n=std::strlen(w);if(j.compare(i,n,w)==0){i+=n;return true;}}
    const std::size_t s0=i;if(j[i]=='-')++i;bool d=false;while(i<j.size()&&((j[i]>='0'&&j[i]<='9')||j[i]=='.'||j[i]=='e'||j[i]=='E'||j[i]=='+'||j[i]=='-')){d=true;++i;}
    return d&&i>s0;}
static bool jsonValid(const std::string& j){std::size_t i=0;if(!jsonValue(j,i))return false;jsonWs(j,i);return i==j.size();}
// ---------- RIGID CARRY: a 15 kg chair driven by carryTick (Opt: the gains, the two 8 kg hand bodies) ----------
// The chair: centre of mass at the body origin, inertia (1.4, 0.6, 1.4) kg m^2, grips on the backrest 24 cm apart, 49 cm above and 20 cm
// behind the centre of mass. Substeps of 13.9 ms, the hands' samples every 2 substeps (a 36 Hz frame) with the command extrapolated by
// their velocity as command() does, the line from the samples alone. Semi-implicit Euler: v += (F / M - g) dt, w += I^-1 tau dt.
namespace carrysim {
struct Body {P::V3 p{},v{},w{};P::Rows R{{{1,0,0},{0,1,0},{0,0,1}}};};
static const P::V3 gA{-.12f,.45f,-.2f},gB{.12f,.45f,-.2f},inertia{1.4f,.6f,1.4f};static constexpr float M=15.f,dt=.0139f;
static P::V3 toWorld(const Body& b,P::V3 l){return V::add(b.p,V::add(V::add(V::mul(b.R[0],l[0]),V::mul(b.R[1],l[1])),V::mul(b.R[2],l[2])));}
static P::V3 invI(const P::Rows& R,P::V3 t){P::V3 o{0,0,0};for(int i=0;i<3;++i)o=V::add(o,V::mul(R[i],V::dot(R[i],t)/inertia[i]));return o;}
static P::Rows yawRows(float a){const float c=std::cos(a),s=std::sin(a);return {{{c,0,-s},{0,1,0},{s,0,c}}};} // about +y: x -> (c, 0, -s)
static float yawOf(const P::Rows& R){return std::atan2(-R[0][2],R[0][0]);}
struct Hands {P::V3 a{},b{},va{},vb{};};
struct Trace {std::vector<float> t;std::vector<P::V3> com;std::vector<P::Rows> R;std::vector<P::V3> w;std::vector<P::CarryOut> out;};
// hands = the two hand bodies as point masses of mh kg on the grips (no gravity, as the engine's hand bodies), their velocity set before
// each step (the grip's now, or with predict carryGripVel), the ball joints rigid: after the chair's own step the joint impulse keeps the
// linear and angular momentum (about the chair's centre of mass) of chair and hands, the hands moving with the grips.
struct Opt {P::CarryGains g{};bool hands{},predict{};float mh{8.f};};
static P::V3 mat3(const float (&A)[6][6],int r0,int c0,P::V3 x){P::V3 o{};for(int i=0;i<3;++i)o[i]=A[r0+i][c0]*x[0]+A[r0+i][c0+1]*x[1]+A[r0+i][c0+2]*x[2];return o;}
static void solve6(float (&A)[6][7]){
    for(int c=0;c<6;++c){int pr=c;for(int r=c+1;r<6;++r)if(std::fabs(A[r][c])>std::fabs(A[pr][c]))pr=r;
        if(pr!=c)for(int k=0;k<7;++k)std::swap(A[c][k],A[pr][k]);
        for(int r=0;r<6;++r){if(r==c)continue;const float f=A[r][c]/A[c][c];for(int k=c;k<7;++k)A[r][k]-=f*A[c][k];}}
    for(int r=0;r<6;++r)A[r][6]/=A[r][r];}
static void jointSolve(Body& b,const std::array<P::V3,2>& r,const std::array<P::V3,2>& vh,float mh){
    float A[6][7]{};const float Mt=M+2.f*mh;
    P::V3 Pm=V::mul(b.v,M),L{0,0,0};
    for(int i=0;i<3;++i){const P::V3 e{i==0?1.f:0.f,i==1?1.f:0.f,i==2?1.f:0.f};const P::V3 Iw=V::add(V::add(V::mul(b.R[0],inertia[0]*V::dot(b.R[0],e)),
        V::mul(b.R[1],inertia[1]*V::dot(b.R[1],e))),V::mul(b.R[2],inertia[2]*V::dot(b.R[2],e)));
        for(int j=0;j<3;++j)A[3+j][3+i]+=Iw[j];
        L=V::add(L,V::mul(e,V::dot(Iw,b.w)));}
    for(int i=0;i<3;++i)A[i][i]=Mt;
    for(int k=0;k<2;++k){Pm=V::add(Pm,V::mul(vh[k],mh));L=V::add(L,V::mul(V::cross(r[k],vh[k]),mh));
        for(int i=0;i<3;++i){const P::V3 e{i==0?1.f:0.f,i==1?1.f:0.f,i==2?1.f:0.f};
            const P::V3 rx=V::cross(r[k],e);                       // [r]x e: column i of [r]x
            for(int j=0;j<3;++j){A[j][3+i]-=mh*rx[j];A[3+j][i]+=mh*rx[j];}
            const P::V3 c=V::cross(r[k],V::cross(e,r[k]));         // (|r|^2 1 - r r^T) e
            for(int j=0;j<3;++j)A[3+j][3+i]+=mh*c[j];}}
    for(int i=0;i<3;++i){A[i][6]=Pm[i];A[3+i][6]=L[i];}
    solve6(A);b.v={A[0][6],A[1][6],A[2][6]};b.w={A[3][6],A[4][6],A[5][6]};}
// hands(t) -> both hands' positions and velocities (world, the mover at rest at the origin)
template<class F> static Trace run(F hands,float T,unsigned seed=0,const Opt& opt=Opt{}){
    Trace tr{};Body b{};b.p={0,.75f,.65f};P::CarryState cs{};
    const P::V3 up{0,1,0};const P::CarryGains g=opt.g;
    std::mt19937 rng(seed);std::uniform_real_distribution<float> nd(-.003f,.003f);P::V3 na{},nb{},pna{},pnb{};
    P::carryJoin(cs,toWorld(b,gA),toWorld(b,gB),b.R,b.p,up,1.f);
    Hands frame{};float tf=-1.f;
    for(float t=0.f;t<T;t+=dt){
        const float tFrame=std::floor(t/(2.f*dt)+1e-4f)*2.f*dt;
        if(tFrame!=tf){tf=tFrame;frame=hands(tFrame);
            if(seed){pna=na;pnb=nb;na={nd(rng),nd(rng),nd(rng)};nb={nd(rng),nd(rng),nd(rng)};
                frame.a=V::add(frame.a,na);frame.b=V::add(frame.b,nb);
                frame.va=V::add(frame.va,V::mul(V::sub(na,pna),1.f/(2.f*dt)));frame.vb=V::add(frame.vb,V::mul(V::sub(nb,pnb),1.f/(2.f*dt)));}}
        const float ahead=t-tFrame;
        const P::V3 ca=V::add(frame.a,V::mul(frame.va,ahead)),cb=V::add(frame.b,V::mul(frame.vb,ahead));
        const P::CarryOut o=P::carryTick(cs,V::sub(frame.b,frame.a),V::mul(V::add(ca,cb),.5f),V::mul(V::add(frame.va,frame.vb),.5f),b.p,b.v,b.R,b.w,inertia,M,70.f,up,g,dt,.05f,1.f);
        // the two forces as the drive applies them: their sum and torque about the centre of mass equal F and tau
        const P::CoupleForces cf=P::coupleForces(o.fN,o.tauNm,b.p,.2f,1.f);
        P::V3 F{0,0,0},tau{0,0,0};for(int k=0;k<2;++k){F=V::add(F,cf.f[k]);tau=V::add(tau,V::cross(V::sub(cf.pos[k],b.p),cf.f[k]));}
        std::array<P::V3,2> rh{},vh{};
        if(opt.hands)for(int k=0;k<2;++k){const P::V3 gp=toWorld(b,k?gB:gA);rh[k]=V::sub(gp,b.p);const P::V3 vg=V::add(b.v,V::cross(b.w,rh[k]));
            vh[k]=opt.predict&&o.ok?P::carryGripVel(vg,gp,b.p,o.aPred,o.alpha,dt,1.f):vg;}
        b.v=V::add(b.v,V::mul(V::add(V::mul(F,1.f/M),{0,-9.81f,0}),dt));b.w=V::add(b.w,V::mul(invI(b.R,tau),dt));
        if(opt.hands)jointSolve(b,rh,vh,opt.mh);
        b.p=V::add(b.p,V::mul(b.v,dt));b.R=V::rotate(V::mul(b.w,dt),b.R);
        tr.t.push_back(t+dt);tr.com.push_back(b.p);tr.R.push_back(b.R);tr.w.push_back(b.w);tr.out.push_back(o);}
    return tr;}
static Hands still(float){Hands h{};h.a={-.12f,1.2f,.45f};h.b={.12f,1.2f,.45f};return h;}
// the hands turned by yaw a about the vertical through c (rate wy: their velocity)
static Hands turned(float a,P::V3 c,float wy){Hands h=still(0.f);const P::Rows Y=yawRows(a);
    const auto rot=[&](P::V3 x){const P::V3 r=V::sub(x,c);return V::add(c,{Y[0][0]*r[0]+Y[2][0]*r[2],r[1],Y[0][2]*r[0]+Y[2][2]*r[2]});};
    h.a=rot(h.a);h.b=rot(h.b);const P::V3 wv{0,wy,0};h.va=V::cross(wv,V::sub(h.a,c));h.vb=V::cross(wv,V::sub(h.b,c));return h;}
static float angleTo(const P::Rows& R,const P::Rows& want){return V::length(P::rotationBetween(R,want))*180.f/P::pi;}
}
// THROW: the open edge, the arm's estimate, the verdict (put-downs, drops, slow, stale), the pair window, the launch, the effect by mass,
// the flight's end, the levers
static void throwTests(){
    const std::uint64_t t0=10'000'000'000ull,tick=14*ms;const P::V3 up{0,1,0};
    // the open edge: armed at .50, two ticks under .30 confirm it; one zeroed tick and back is no edge
    {P::ThrowEdge e{};CHECK(!P::throwEdgeStep(e,.9f,t0)&&!P::throwEdgeStep(e,0.f,t0+tick)&&!P::throwEdgeStep(e,.9f,t0+2*tick)&&!P::throwEdgeStep(e,.9f,t0+3*tick),
           "throw edge: one zeroed tick and back: no edge");
     CHECK(!P::throwEdgeStep(e,.1f,t0+4*tick)&&P::throwEdgeStep(e,.05f,t0+5*tick)&&e.firstNs==t0+4*tick&&e.confirmNs==t0+5*tick,"throw edge: two ticks under .30 after .50: the edge (its first tick kept)");
     CHECK(!P::throwEdgeStep(e,.1f,t0+6*tick)&&!P::throwEdgeStep(e,.1f,t0+7*tick),"throw edge: once per arm");
     P::ThrowEdge e2{};CHECK(!P::throwEdgeStep(e2,.4f,t0)&&!P::throwEdgeStep(e2,.1f,t0+tick)&&!P::throwEdgeStep(e2,.1f,t0+2*tick),"throw edge: never armed (under .50): no edge");
     P::ThrowEdge e3{};P::throwEdgeStep(e3,.9f,t0);CHECK(!P::throwEdgeStep(e3,std::nanf(""),t0+tick)&&!P::throwEdgeStep(e3,.2f,t0+2*tick)&&P::throwEdgeStep(e3,.2f,t0+3*tick),
           "throw edge: an unread grip neither counts nor resets");}
    // the estimate: the peak and its contiguous neighbours at 85 % or more
    const auto trail=[&](const std::vector<P::V3>& vs,std::uint64_t end){P::ArmTrail t{};std::uint64_t ns=end-std::uint64_t(vs.size()-1)*tick;for(const auto& x:vs){t.push(x,ns);ns+=tick;}return t;};
    {const auto t=trail({{0,0,0},{1,0,-.5f},{2.6f,.3f,-1.f},{3.f,.4f,-1.2f},{2.7f,.2f,-1.f},{1.f,0,0},{.2f,0,0}},t0);
     const P::ThrowEst e=P::throwEstimate(t,t0-500*ms,t0);
     const P::V3 mean=V::mul(V::add(V::add(P::V3{2.6f,.3f,-1.f},P::V3{3.f,.4f,-1.2f}),P::V3{2.7f,.2f,-1.f}),1.f/3.f);
     CHECK(e.n==7&&near(e.peak,V::length(P::V3{3.f,.4f,-1.2f}),1e-4f)&&nearV(e.v,mean,1e-4f)&&near(e.peakAgeMs,float(3*14),1e-3f),"throw estimate: the peak, its 85 % neighbours averaged, the peak's age");
     CHECK(P::throwVerdict(e,up,2.f,1.5f,250.f)==P::ThrowGo,"throw verdict: forward 3 m/s peaked 42 ms before: a throw");
     CHECK(P::throwEstimate(t,t0+1,t0+2).n==0&&P::throwVerdict(P::throwEstimate(t,t0+1,t0+2),up,2.f,1.5f,250.f)==P::ThrowNoArm,"throw estimate: no sample in the window: no arm");}
    // put-downs and drops are no throws
    {const auto slow=trail({{0,0,0},{.3f,-.5f,0},{.4f,-.6f,0},{.2f,-.3f,0}},t0);
     CHECK(P::throwVerdict(P::throwEstimate(slow,t0-300*ms,t0),up,2.f,1.5f,250.f)==P::ThrowSlow,"throw verdict: a put-down at .6 m/s: slow");
     const auto down=trail({{0,0,0},{.5f,-2.5f,0},{.6f,-3.f,0},{.5f,-2.6f,0}},t0);
     CHECK(P::throwVerdict(P::throwEstimate(down,t0-300*ms,t0),up,2.f,1.5f,250.f)==P::ThrowDown,"throw verdict: 3 m/s mostly downward: down (a drop)");
     const auto lat=trail({{0,0,0},{1.2f,0,0},{1.1f,0,0}},t0);
     CHECK(P::throwVerdict(P::throwEstimate(lat,t0-300*ms,t0),up,2.f,1.5f,250.f)==P::ThrowSlow&&
           P::throwVerdict(P::throwEstimate(lat,t0-300*ms,t0),up,1.f,1.f,250.f)==P::ThrowGo,"throw verdict: a 1.2 m/s sideways swing: slow at the heavy rule, a throw at a 1 m/s rule");
     P::ThrowEst old{};old.n=3;old.peak=4.f;old.peakAgeMs=300.f;old.est=3.8f;old.v={3.8f,0,0};
     CHECK(P::throwVerdict(old,up,2.f,1.5f,250.f)==P::ThrowStale,"throw verdict: the peak 300 ms old: stale");
     P::ThrowEst lt{};lt.n=3;lt.peak=1.7f;lt.peakAgeMs=20.f;lt.est=1.3f;lt.v={1.3f,0,0};
     CHECK(P::throwVerdict(lt,up,2.f,1.5f,250.f)==P::ThrowSlow&&P::throwVerdict(lt,up,1.5f,1.2f,250.f)==P::ThrowGo,"throw verdict: 1.7 m/s: slow for a chair, a throw for a cup");}
    CHECK(near(P::throwTapA(9.f),P::throwTapMps)&&near(P::throwTapA(2.6f),P::throwTapMps)&&near(P::throwTapA(2.f),2.f)&&P::throwTapA(std::nanf(""))==0.f&&P::throwTapA(-1.f)==0.f,
          "throw tap: a cup's approach handed to the slap path at most throwTapMps (a 9 m/s cup taps like a hand slap)");
    // the pair: both hands within the window decide; one hand still closed past it: held
    {CHECK(P::throwPairState(t0,0,0,t0+50*ms,150.f)==P::ThrowPairWait&&P::throwPairState(t0,0,0,t0+151*ms,150.f)==P::ThrowPairHeld,"throw pair: the other hand closed: wait, held after 150 ms");
     CHECK(P::throwPairState(t0+80*ms,t0,t0+28*ms,t0+108*ms,150.f)==P::ThrowGo&&P::throwPairState(t0+200*ms,t0,t0+28*ms,t0+228*ms,150.f)==P::ThrowPairHeld,
           "throw pair: both open 80 ms apart: a throw; 200 ms apart: held");}
    // the launch: the estimate plus the mover, capped; written only when faster than the prop along it
    {const P::V3 l=P::throwLaunch({5,0,0},{1,0,0},7.f);const P::V3 c=P::throwLaunch({9,0,0},{0,0,0},7.f);
     CHECK(nearV(l,{6,0,0})&&near(V::length(c),7.f,1e-4f)&&nearV(P::throwLaunch({std::nanf(""),0,0},{1,0,0},7.f),{1,0,0}),"throw launch: the mover added, 7 m/s cap, a bad estimate dropped");
     CHECK(P::throwWrites({1,0,0},{4,0,0})&&!P::throwWrites({5,0,0},{4,0,0})&&P::throwWrites({0,3,0},{4,0,0})&&!P::throwWrites({1,0,0},{0,0,0}),
           "throw launch: written unless the prop already moves faster along it");}
    // the effect by mass: the KO scale, strong, the tap zones
    {CHECK(near(P::throwMassScale(8.f,8.f),.5f)&&near(P::throwMassScale(16.5f,8.f),.75f)&&near(P::throwMassScale(25.f,8.f),1.f)&&near(P::throwMassScale(59.5f,8.f),1.f),
           "throw mass scale: 8 kg .5, 16.5 kg .75, 25 and 59.5 kg 1");
     CHECK(P::throwStrong(59.5f,2.5f,2.5f,100.f)&&!P::throwStrong(59.5f,2.4f,2.5f,100.f)&&!P::throwStrong(8.f,9.f,2.5f,100.f),"throw strong: the chair from 2.5 m/s, an 8 kg prop never");
     CHECK(P::throwTapZone(0)&&P::throwTapZone(1)&&P::throwTapZone(2)&&!P::throwTapZone(3)&&!P::throwTapZone(4)&&!P::throwTapZone(-1),"throw tap: head, neck, chest only");}
    // the flight's end
    {std::uint64_t slow=0;
     CHECK(P::flightStep(slow,5.f,t0,t0+100*ms,2000.f,0)==P::FlightNone&&P::flightStep(slow,.4f,t0,t0+200*ms,2000.f,0)==P::FlightNone&&
           P::flightStep(slow,.4f,t0,t0+250*ms,2000.f,0)==P::FlightNone&&P::flightStep(slow,.4f,t0,t0+300*ms,2000.f,0)==P::FlightRest,"flight: under .5 m/s for 100 ms: rest");
     std::uint64_t s2=0;CHECK(P::flightStep(s2,.4f,t0,t0+10*ms,2000.f,0)==P::FlightNone&&P::flightStep(s2,3.f,t0,t0+90*ms,2000.f,0)==P::FlightNone&&s2==0&&
           P::flightStep(s2,.4f,t0,t0+150*ms,2000.f,0)==P::FlightNone,"flight: a bounce restarts the rest clock");
     std::uint64_t s3=0;CHECK(P::flightStep(s3,4.f,t0,t0+2000*ms,2000.f,0)==P::FlightTimeout&&P::flightStep(s3,4.f,t0,t0+10*ms,2000.f,2)==P::FlightHits,"flight: 2 s timeout, 2 hits done");}
    // the levers
    {P::Levers L{};CHECK(L.throwMode==2&&L.throwLight==1&&near(L.throwMps,2.f)&&near(L.throwMpsLight,1.5f)&&near(L.throwRelMps,1.5f)&&near(L.throwRelMpsLight,1.2f)&&
           near(L.throwWinMs,250.f)&&near(L.throwPairMs,150.f)&&near(L.throwMaxMps,7.f)&&near(L.throwMaxMpsLight,9.f)&&near(L.throwFlightMs,2000.f)&&near(L.throwHitMps,1.5f)&&
           near(L.throwStrongKgMps,100.f)&&near(L.throwKo,1.f),"throw levers: defaults");
     CHECK(P::sanitized(P::Levers{.throwMode=7}).throwMode==2&&near(P::sanitized(P::Levers{.throwMaxMps=50.f}).throwMaxMps,12.f),"throw levers: clamps");}
}
// KNOCKDOWN ROUTE: the table by side and frozen count, the levers; the frozen-mover UP message held for the other hand
static void knockTests(){
    {const auto r0=P::knockPlan(1,false,.86f,2,40.f,75.f),r1=P::knockPlan(1,false,-.85f,2,40.f,75.f),r2=P::knockPlan(1,true,.83f,2,40.f,75.f),
               r3=P::knockPlan(1,true,-1.f,2,40.f,75.f);
     CHECK(r0.row==P::KnockBehindFree&&r0.face==1&&r0.impulse<0.f&&r0.bullet==1,"knock R0 (behind, free): face 1, the impulse unchanged, along the push");
     CHECK(r1.row==P::KnockFrontFree&&r1.face==1&&near(r1.impulse,75.f)&&r1.bullet==-1,"knock R1 (front, free; pushfront 2): face 1, 75, toward the pusher");
     CHECK(r2.row==P::KnockBehindFrozen&&r2.face==0&&near(r2.impulse,40.f)&&r2.bullet==-1,"knock R2 (behind, frozen): face 0, 40, backward in the victim's frame");
     CHECK(r3.row==P::KnockFrontFrozen&&r3.face==1&&near(r3.impulse,75.f)&&r3.bullet==-1,"knock R3 (front, frozen): face 1, 75, forward in the victim's frame");
     // a side push takes the row of its dirFwd sign (the kick carries the sideways part)
     CHECK(P::knockPlan(1,false,.48f,2,40.f,75.f).row==P::KnockBehindFree&&P::knockPlan(1,true,.78f,2,40.f,75.f).row==P::KnockBehindFrozen&&
           P::knockPlan(1,false,-.3f,2,40.f,75.f).row==P::KnockFrontFree,"knock: a side push by the sign of dirFwd");
     // R1 levers: 1 = the device's face 0 / 40 along the push, 0 = the message unchanged
     const auto f1=P::knockPlan(1,false,-.85f,1,40.f,75.f),f0=P::knockPlan(1,false,-.85f,0,40.f,75.f);
     CHECK(f1.face==0&&near(f1.impulse,40.f)&&f1.bullet==1&&f0.face==-1&&f0.impulse<0.f&&f0.bullet==1,"knock R1: pushfront 1 (face 0, 40) and 0 (unchanged)");
     // pushknock 2: frozen movers take the free rows; 0 (or an unread dirFwd): no plan
     CHECK(P::knockPlan(2,true,.83f,2,40.f,75.f).row==P::KnockBehindFree&&P::knockPlan(2,true,-.83f,2,40.f,75.f).row==P::KnockFrontFree&&
           P::knockPlan(0,true,.83f,2,40.f,75.f).row==P::KnockNone&&P::knockPlan(1,false,std::nanf(""),2,40.f,75.f).row==P::KnockNone,
           "knock: pushknock 2 = the free rows for every mover, 0 = no plan");
     P::Levers L{};CHECK(L.pushKnock==2&&P::TouchRequest{}.pushKnock==2&&L.pushFrozenHold&&L.pushFront==2&&P::sanitized(P::Levers{.pushKnock=7}).pushKnock==2,
           "knock levers: defaults (pushknock 2: every knockdown message face 1), clamp");
     // pushfront 3: face 1, behindImp, along the push
     const auto f3=P::knockPlan(1,false,-.85f,3,40.f,75.f);
     CHECK(f3.row==P::KnockFrontFree&&f3.face==1&&near(f3.impulse,75.f)&&f3.bullet==1&&P::sanitized(P::Levers{.pushFront=9}).pushFront==3,"knock R1: pushfront 3 (face 1, 75, along the push), clamp 3");
     CHECK(L.pushKnockFlip==1&&P::TouchRequest{}.pushKnockFlip==1&&P::sanitized(P::Levers{.pushKnockFlip=5}).pushKnockFlip==1,
           "knock flip lever: default 1 (the forward part mirrored: the device table's setup), clamp");
     // KNOCK PICK: the parity by side (1 staggerFall from behind, 0 NMShot otherwise), the forced levers, the coin; the roll written
     CHECK(P::knockPickParity(1,.86f,.5f)==1&&P::knockPickParity(1,.5f,.5f)==1&&P::knockPickParity(1,.48f,.5f)==0&&P::knockPickParity(1,-.9f,.5f)==0&&
           P::knockPickParity(1,.05f,.5f)==0,"knock pick: staggerFall for dirFwd >= the limit (0.5 here), NMShot below it");
     {const float fw=P::Levers{}.pushKnockPickFwd;
      CHECK(near(fw,0.f)&&P::knockPickParity(1,.05f,fw)==1&&P::knockPickParity(1,.44f,fw)==1&&P::knockPickParity(1,0.f,fw)==1&&P::knockPickParity(1,-.05f,fw)==0&&
            P::knockPickParity(1,-.97f,fw)==0,"knock pick: default limit 0: the behind half (side pushes from behind included) staggerFall, the front half NMShot");}
     CHECK(P::knockPickParity(2,.9f,.5f)==0&&P::knockPickParity(3,-.9f,.5f)==1&&P::knockPickParity(0,.9f,.5f)==-1&&P::knockPickParity(1,std::nanf(""),.5f)==-1,
           "knock pick: 2 NMShot, 3 staggerFall, 0 or an unread dirFwd the coin");
     CHECK(P::knockPickNum(0x12345677,0)==0x12345676&&P::knockPickNum(0x12345676,1)==0x12345677&&P::knockPickNum(-1,0)==0x7ffffffe&&
           P::knockPickNum(0x7fffffff,1)%2==1&&P::knockPickNum(8,0)%2==0,"knock pick: the stored roll keeps its bits, its parity set, within 31 bits");
     {const auto k=P::knockPickPlan(P::KnockFrontFree,75.f);const auto k2=P::knockPickPlan(P::KnockBehindFree,std::nanf(""));
      CHECK(k.row==P::KnockFrontFree&&k.face==1&&near(k.impulse,75.f)&&k.bullet==-1&&k2.face==1&&near(k2.impulse,75.f)&&k2.bullet==-1,
            "knock pick: every row sends face 1, the behind size, the bullet reversed");}
     CHECK(P::knockPickChildOk(0x731220e8u,0x2900ca0u,0,0,2)&&P::knockPickChildOk(0x02d1adb2u,0x2900ca0u,0,1,2)&&!P::knockPickChildOk(0xbc3dfb68u,0x2900ca0u,0,1,2)&&
           !P::knockPickChildOk(0x9974d115u,0x2900ca0u,1,1,2)&&!P::knockPickChildOk(0x9974d115u,0x2900ca0u,0,1,3)&&!P::knockPickChildOk(0x12345678u,0x2900ca0u,0,0,2)&&
           !P::knockPickChildOk(0xbc3dfb68u,0x1cce3e4u,0,0,2),"knock pick: the tree words (mode 0, roll % 2, the child's remainder) checked per child");
     CHECK(P::knockNodeParity(0x02d1adb2u)==1&&P::knockNodeParity(0x9974d115u)==1&&P::knockNodeParity(0xbc3dfb68u)==0&&P::knockNodeParity(0x731220e8u)==0&&
           P::knockNodeParity(0x1u)==-1,"knock pick: the landed node's behaviour");
     CHECK(L.pushKnockPick==3&&P::TouchRequest{}.pushKnockPick==3&&near(L.pushKnockPickFwd,0.f)&&!L.pushKnockSpine&&L.pushKnockPickMsg==0&&P::sanitized(P::Levers{.pushKnockPick=9}).pushKnockPick==3&&
           P::sanitized(P::Levers{.pushKnockPickMsg=4}).pushKnockPickMsg==1&&P::TouchRequest{}.pushKnockPickMsg==0&&near(P::TouchRequest{}.pushKnockPickFwd,0.f),
           "knock pick levers: defaults (always staggerFall, limit 0, the route table's message), clamp");
     // KNOCK TOUGH / PICK FUNC / KNOCK OFF levers and helpers
     CHECK(L.pushKnockTough&&L.pushKnockPickFunc&&near(L.knockOffMs,1500.f)&&P::TouchRequest{}.pushKnockTough==1&&P::TouchRequest{}.pushKnockPickFunc==1&&
           near(P::TouchRequest{}.knockOffMs,1500.f)&&near(P::sanitized(P::Levers{.knockOffMs=9000.f}).knockOffMs,5000.f)&&near(P::sanitized(P::Levers{.knockOffMs=-3.f}).knockOffMs,0.f),
           "knock levers: tough window on, the pick's draw function on, hands off 1500 ms (0..5000)");
     CHECK(P::toughBit==0x8000u&&P::toughRestoreVerdict(true,true,0x1234u)==P::ToughRestored&&P::toughRestoreVerdict(true,true,0x9234u)==P::ToughScriptChanged&&
           P::toughRestoreVerdict(false,true,0x1234u)==P::ToughLost&&P::toughRestoreVerdict(true,false,0x1234u)==P::ToughLost,
           "knock tough: put back only on the same actor and health block while the bit is still clear (a script's own set is kept)");
     {// the replay step: x = lo * 0x5cdcfaa7 + hi (ConditionRandom::Match 0x2900dc0..0x2900de0), the roll lo & 0x7fffffff
      std::uint64_t ref=0x0123456789abcdefull;const std::uint64_t r1=std::uint64_t(0x89abcdefu)*0x5cdcfaa7ull+0x01234567ull;
      CHECK(P::replayStep(ref)==r1&&P::replayStep(0)==0&&P::replayStep(1)==0x5cdcfaa7ull,"pick func: the engine's replay multiply-with-carry");
      static std::uint64_t replay=0x0000000100000002ull;std::uint64_t shadow=replay;
      alignas(8) static unsigned char ctxV[0x40]{},ctxO[0x40]{};const std::uintptr_t victim=0x77770000ull,other=0x88880000ull;
      std::memcpy(ctxV+0x28,&victim,8);std::memcpy(ctxO+0x28,&other,8);
      P::pickFn.replay.store(stub::addr(&replay));P::pickFn.victim.store(victim);P::pickFn.parity.store(1);
      bool stock=true,forced=true;const auto vd0=P::pickFn.victimDraws.load(),od0=P::pickFn.otherDraws.load();
      for(int i=0;i<64;++i){shadow=P::replayStep(shadow);const int want=int(std::uint32_t(shadow)&0x7fffffffu);
          if(i&1){const int n=P::pickFnDraw(ctxV);forced=forced&&n%2==1&&(n|1)==(want|1);}
          else{const int n=P::pickFnDraw(ctxO);stock=stock&&n==want;}}
      CHECK(stock&&forced&&replay==shadow&&P::pickFn.victimDraws.load()==vd0+32&&P::pickFn.otherDraws.load()==od0+32,
            "pick func: other actors get the engine's own rolls, the victim's rolls take the parity, the state advances exactly as the engine's");
      P::pickFn.parity.store(0);const int n0=P::pickFnDraw(ctxV);P::pickFn.victim.store(0);const int n1=P::pickFnDraw(ctxV);shadow=P::replayStep(P::replayStep(shadow));
      CHECK(n0%2==0&&n1==int(std::uint32_t(shadow)&0x7fffffffu)&&P::pickFnDraw(nullptr)>=0,"pick func: parity 0; no victim = the stock roll; a null context is safe");
      P::pickFn.replay.store(0);CHECK(P::pickFnDraw(ctxV)==0,"pick func: no replay word = 0 (never installed without it)");P::pickFn.parity.store(-1);}
     {// GRAB SEAT: the seat point, the pull, the verdicts
      P::GrabPart part{};part.m[0]=part.m[5]=part.m[10]=part.m[15]=1.f;part.bmin={-.04f,-.04f,-.06f};part.bmax={.04f,.04f,.06f};
      const float inst[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, 1.f,2.f,3.f,1.f};
      auto sp=P::seatPoint(part,inst,{1.01f,2.f,3.f});CHECK(sp.ok&&near(sp.dist,0.f),"seat point: the palm inside the box: distance 0");
      sp=P::seatPoint(part,inst,{1.14f,2.f,3.f});CHECK(sp.ok&&near(sp.dist,.10f,1e-5f)&&nearV(sp.world,{1.04f,2.f,3.f},1e-5f),"seat point: 10 cm off the +x face: the face point");
      const float rot[16]{0,0,-1,0, 0,1,0,0, 1,0,0,0, 1.f,2.f,3.f,1.f}; // turned 90 deg about y: the box's z extent (6 cm) along world x
      sp=P::seatPoint(part,rot,{1.16f,2.f,3.f});CHECK(sp.ok&&near(sp.dist,.10f,1e-5f)&&nearV(sp.world,{1.06f,2.f,3.f},1e-5f),"seat point: a rotated instance");
      {P::GrabPart pr=part;const float half=std::sqrt(.5f);const float pm[16]{half,0,-half,0, 0,1,0,0, half,0,half,0, 0,0,0,1};std::memcpy(pr.m,pm,64);
       sp=P::seatPoint(pr,inst,{1.f,2.f,3.f});CHECK(sp.ok&&near(sp.dist,0.f),"seat point: a rotated part with the palm at its centre");}
      CHECK(!P::seatPoint(part,inst,{NAN,2.f,3.f}).ok,"seat point: NaN = none");
      const float tau=P::seatTauS(.10f);CHECK(near(tau,.05f),"seat: tau = 5 mm / creep 0.1 m/s = 50 ms");
      P::V3 v=P::seatPullVel({.16f,0,0},{0,0,0},{0,0,0},1.5f,tau);CHECK(near(V::length(v),1.5f,1e-4f)&&v[0]<0.f,"seat pull: 16 cm away: the cap (1.5 m/s) toward the palm");
      v=P::seatPullVel({.005f,0,0},{0,0,0},{0,0,1.f},1.5f,tau);CHECK(near(v[0],-.1f,1e-4f)&&near(v[2],1.f),"seat pull: at 5 mm about the creep speed, the hand's velocity added");
      v=P::seatPullVel({NAN,0,0},{0,0,0},{0,0,1.f},1.5f,0.f);CHECK(nearV(v,{0,0,1.f},1e-6f),"seat pull: NaN = the hand's velocity, tau 0 safe");
      CHECK(near(P::seatDoneFor(1.f/72.f,tau),.005f+9.8f/72.f*.05f,1e-5f)&&near(P::seatDoneFor(0.f,tau),.005f)&&near(P::seatDoneFor(1.f,tau),.02f),
            "seat: the seated distance = 5 mm + the gravity sag (g dt tau), at most 2 cm");
      // the pull integrated with gravity in each step (the solver adds it after the write), the hand still or moving 1 m/s
      for(const float dt:{1.f/240.f,1.f/72.f})for(const float hv:{0.f,1.f}){
          P::V3 x{.16f,0,0},b{0,0,0};const P::V3 vh{0,0,hv};float t=0.f;float rel=-1.f;bool seated=false;float vmax=0.f;
          for(int i=0;i<2000&&t<.35f;++i){v=P::seatPullVel(x,b,vh,1.5f,tau);vmax=std::max(vmax,V::length(V::sub(v,vh)));
              P::V3 vv=v;vv[1]-=9.8f*dt;x=V::add(x,V::mul(vv,dt));b=V::add(b,V::mul(vh,dt));t+=dt;
              if(P::seatStep(true,1.f,.3f,0,true,V::length(V::sub(x,b)),P::seatDoneFor(dt,tau),std::uint64_t(double(t)*1e9),350*ms)==P::SeatSeated){
                  seated=true;rel=V::length(V::sub(P::seatPullVel(x,b,vh,1.5f,tau),vh));break;}}
          std::printf("     seat pull dt %.4f s, hand %.0f m/s: seated %d after %.0f ms, arrival %.3f m/s, peak %.2f m/s\n",double(dt),double(hv),seated?1:0,double(t*1000.f),double(rel),double(vmax));
          CHECK(seated&&t<=.35f&&rel<=(dt>.01f?.25f:.15f)&&vmax<=1.5f+1e-4f,"seat pull: seated within 350 ms, a soft arrival, never over the cap");}
      // a cup blocked by a table: never seated, timeout, the velocity capped
      {const P::V3 x{.16f,0,0},b{0,0,0};P::SeatEnd e=P::SeatKeep;std::uint64_t t=0;float vmax=0.f;
       while(e==P::SeatKeep&&t<2000*ms){vmax=std::max(vmax,V::length(P::seatPullVel(x,b,{0,0,0},1.5f,tau)));t+=14*ms;
           e=P::seatStep(true,1.f,.3f,0,true,V::length(V::sub(x,b)),P::seatDoneFor(1.f/72.f,tau),t,350*ms);}
       CHECK(e==P::SeatTimeout&&t>=350*ms&&t<364*ms&&vmax<=1.5f+1e-4f,"seat: a blocked cup times out at grabOpenMs (then attached where it is)");}
      CHECK(P::seatStep(true,.2f,.3f,0,true,.1f,.005f,0,350*ms)==P::SeatCancelOpen&&P::seatStep(true,1.f,.3f,1,false,.1f,.005f,0,350*ms)==P::SeatCancelInactive&&
            P::seatStep(true,1.f,.3f,-1,false,.1f,.005f,0,350*ms)==P::SeatCancelGone&&P::seatStep(true,1.f,.3f,0,false,.1f,.005f,0,350*ms)==P::SeatCancelGone&&
            P::seatStep(false,1.f,.3f,0,true,0.f,.005f,0,350*ms)==P::SeatCancelMode&&P::seatStep(true,1.f,.3f,0,true,.004f,.005f,0,350*ms)==P::SeatSeated&&
            P::seatStep(true,1.f,.3f,0,true,.1f,.005f,10*ms,350*ms)==P::SeatKeep&&P::Levers{}.grabSeat,"seat: the verdicts in order; lever grabseat on by default");
      // the grip's open debounce (the hold's own rule): a dip keeps pulling and never attaches, open for openMs cancels, a fast open throws
      CHECK(P::seatStep(true,.1f,.3f,0,true,.001f,.005f,10*ms,350*ms,100*ms,350*ms,0.f)==P::SeatKeep&&
            P::seatStep(true,0.f,.3f,0,true,.1f,.005f,400*ms,350*ms,200*ms,350*ms,0.f)==P::SeatKeep&&
            P::seatStep(true,.1f,.3f,0,true,.1f,.005f,10*ms,350*ms,350*ms,350*ms,0.f)==P::SeatCancelOpen&&
            P::seatStep(true,.1f,.3f,0,true,.1f,.005f,10*ms,350*ms,0,350*ms,1.5f)==P::SeatThrow&&
            P::seatStep(true,.1f,.3f,0,true,.1f,.005f,10*ms,350*ms,0,350*ms,1.1f)==P::SeatKeep&&
            P::seatStep(true,.1f,.3f,-1,false,.1f,.005f,10*ms,350*ms,0,350*ms,1.5f)==P::SeatCancelGone&&
            P::seatStep(true,NAN,.3f,0,true,.1f,.005f,10*ms,350*ms,0,0,0.f)==P::SeatCancelOpen,
            "seat: a grip dip under .30 keeps pulling (no attach, no timeout), open for grabOpenMs cancels, 1.2 m/s while open throws, a gone prop first");}
     {// the hands-off table
      for(auto& k:P::knockOff){k.until.store(0);k.inst.store(0);k.actor.store(0);}
      const std::uint64_t t=5'000*ms;P::knockOff[2].inst.store(0x5000);P::knockOff[2].actor.store(0x6000);P::knockOff[2].until.store(t+1500*ms);
      CHECK(P::knockOffMatch(0x5000,0,t)==2&&P::knockOffMatch(0x1234,0x6000,t)==2&&P::knockOffMatch(0x1234,0x7000,t)==-1&&P::knockOffMatch(0x5000,0,t+1500*ms)==-1&&
            P::knockOffMatch(0x1234,0,t)==-1,"knock off: the creature's frag by instance or its user's actor, until the window ends");
      CHECK(near(P::knockOffToward({-1.f,0,0},{1,0,0}),1.f)&&near(P::knockOffToward({1.f,0,0},{1,0,0}),0.f)&&near(P::knockOffToward({0,0,2.f},{0,0,-3}),2.f)&&
            near(P::knockOffToward({0,5,0},{1,0,0}),0.f),"knock off: the hand's speed toward the pusher (against the push, horizontal)");
      P::knockOff[2].until.store(0);
      P::knockOffLive.store(0);P::knockOffSetUntil(2,t+1500*ms);P::knockOffSetUntil(0,t+1500*ms);
      CHECK(P::knockOffLive.load()==5u&&P::knockOff[2].until.load()==t+1500*ms,"knock off: an armed slot sets its live bit");
      P::knockOffSetUntil(2,0);CHECK(P::knockOffLive.load()==1u&&P::knockOff[2].until.load()==0,"knock off: an ended slot clears its live bit only");
      P::knockOffSetUntil(0,0);CHECK(P::knockOffLive.load()==0u,"knock off: no window: the filter's idle test reads 0");}
     {// KNOCK LANDED: the first node sample after the knockdown's message (a sample of the message's own tick came before it)
      struct S{std::uint16_t ms{};};std::array<S,4> tr{S{40},S{80},S{80},S{120}};
      CHECK(P::knockFirstAfter(tr,2,2,80)==-1&&P::knockFirstAfter(tr,3,2,80)==2&&P::knockFirstAfter(tr,4,2,80)==2,
            "knock landed: a sample at the message's own age taken before it is no landing; the next tick's is");
      CHECK(P::knockFirstAfter(tr,2,-1,80)==1&&P::knockFirstAfter(tr,1,-1,80)==-1&&P::knockFirstAfter(tr,4,4,80)==-1&&P::knockFirstAfter(tr,9,1,0)==1,
            "knock landed: no count recorded = the first at or after the age; a full ring at the message = none");}
     // the knockdown's message: the route table's unless lever pushknockpickmsg 1 and the pick forced
     {const auto r1=P::knockPlan(1,false,-.97f,2,40.f,75.f),r2=P::knockPlan(1,true,.9f,2,40.f,75.f),r0=P::knockPlan(1,false,.9f,2,40.f,75.f);
      const auto same=[](const P::KnockPlan& a,const P::KnockPlan& b){return a.row==b.row&&a.face==b.face&&a.bullet==b.bullet&&(a.impulse==b.impulse);};
      CHECK(same(P::knockMessagePlan(r1,-1,-.97f,75.f),r1)&&same(P::knockMessagePlan(r2,-1,.9f,75.f),r2)&&same(P::knockMessagePlan(r0,-1,.9f,75.f),r0),
            "knock message: a coin (or lever pushknockpickmsg 0): the route table's message for every row");
      const auto m2=P::knockMessagePlan(r2,1,.9f,75.f),mn=P::knockMessagePlan(P::KnockPlan{},0,-.5f,75.f);
      CHECK(m2.row==P::KnockBehindFrozen&&m2.face==1&&near(m2.impulse,75.f)&&m2.bullet==-1&&mn.row==P::KnockFrontFree&&mn.face==1&&
            same(P::knockMessagePlan(r1,0,std::nanf(""),75.f),r1),"knock message: forced with lever 1: the pick's message (face 1, 75, reversed); an unread dirFwd keeps the table's");}
     // the reversed bullet: the forward part in the victim's frame mirrored, the sideways and vertical parts and the size kept
     {const P::V3 c{0,0,1},b{.71f,.1f,.70f};const P::V3 m=P::knockBulletFlip(b,c,{0,1,0},1),r=P::knockBulletFlip(b,c,{0,1,0},0);
      CHECK(nearV(m,{.71f,.1f,-.70f},1e-5f)&&nearV(r,{-.71f,-.1f,-.70f},1e-5f)&&near(V::length(m),V::length(b),1e-5f),"knock bullet: flip 1 mirrors the forward part only, flip 0 reverses it all");
      const P::V3 c2{.6f,.3f,.8f};const P::V3 m2=P::knockBulletFlip(b,c2,{0,1,0},1);const P::V3 ch=V::normalized(P::V3{.6f,0,.8f});
      CHECK(near(V::dot(m2,ch),-V::dot(b,ch),1e-5f)&&near(m2[1],b[1],1e-5f)&&near(V::length(m2),V::length(b),1e-5f),"knock bullet: a tilted rowC is made horizontal first");
      CHECK(nearV(P::knockBulletFlip(b,{0,1,0},{0,1,0},1),{-.71f,-.1f,-.70f},1e-5f),"knock bullet: no horizontal facing: the whole impulse reversed");}
     // the frozen route: +0x848 now, or the push's own UP message (which clears +0x848)
     CHECK(P::knockFrozen(1,false)&&P::knockFrozen(0,true)&&P::knockFrozen(-1,true)&&!P::knockFrozen(0,false)&&!P::knockFrozen(-1,false),
           "knock: frozen = +0x848 > 0 or the push sent / held the frozen mover's message");}
    // the hold: a knockdown of the same push within the window drops the message; else it is due at the deadline; the lever off holds nothing
    {P::FrozenHoldSlot s{};const std::uint64_t t0=5'000'000'000ull;bool rep=true;
     CHECK(P::frozenHoldArm(s,0x55,t0,t0,150.f,&rep)&&!rep&&!P::frozenHoldDueNow(s,t0+100*ms),"frozen hold: armed, not due before 150 ms");
     CHECK(!P::frozenHoldDrop(s,0x55,t0+1)&&!P::frozenHoldDrop(s,0x56,t0)&&s.on,"frozen hold: another push or creature leaves it");
     CHECK(P::frozenHoldDrop(s,0x55,t0)&&!s.on&&!P::frozenHoldDueNow(s,t0+200*ms),"frozen hold: the push raised to a knockdown in the window: dropped, never sent");
     P::FrozenHoldSlot s2{};P::frozenHoldArm(s2,0x55,t0,t0,150.f);
     CHECK(!P::frozenHoldDueNow(s2,t0+149*ms)&&P::frozenHoldDueNow(s2,t0+150*ms),"frozen hold: no knockdown: sent at the deadline");
     CHECK(P::frozenHoldArm(s2,0x57,t0+10*ms,t0+10*ms,150.f,&rep)&&rep&&s2.guid==0x57,"frozen hold: arming a taken slot replaces it (reported)");
     // two slots per hand: the first free one; both taken: none (the message goes out at once, none is replaced)
     {P::FrozenHoldSlot a{},b{};CHECK(P::frozenHoldFree(a,b)==0,"frozen hold: the first slot free");a.on=true;CHECK(P::frozenHoldFree(a,b)==1,"frozen hold: the second slot");
      b.on=true;CHECK(P::frozenHoldFree(a,b)==-1,"frozen hold: both taken: nothing held");}
     P::FrozenHoldSlot s3{};CHECK(!P::frozenHoldArm(s3,0x55,t0,t0,0.f)&&!s3.on,"frozen hold: lever off (0 ms): nothing held, the message goes at once");}
}
static void carryTests(){
    namespace C=carrysim;
    // the frame: a level line holds z on up; a steep line keeps the last z continuously; a turn of the line
    {const P::CarryFrame f=P::carryFrameStep({},false,{1,0,0},{0,1,0},1.f);
     CHECK(nearV(f.x,{1,0,0})&&nearV(f.z,{0,1,0})&&nearV(f.y,{0,0,-1})&&near(V::dot(V::cross(f.x,f.y),f.z),1.f),"carry frame: a level line, z = up, right-handed");
     P::CarryFrame g=f;for(int i=0;i<60;++i)g=P::carryFrameStep(g,true,{0,1,.001f},{0,1,0},.25f);
     const P::CarryFrame g2=P::carryFrameStep(g,true,{0,1,-.001f},{0,1,0},.25f);
     CHECK(V::length(V::sub(g2.z,g.z))<.02f&&near(V::length(g2.z),1.f,1e-3f),"carry frame: through a vertical line the frame turns continuously");
     const P::V3 r=P::rotationBetween(P::carryRows(f),P::carryRows(P::carryFrameStep({},false,{std::cos(.3f),0,-std::sin(.3f)},{0,1,0},1.f)));
     CHECK(near(r[1],.3f,1e-3f)&&near(r[0],0.f,1e-3f),"carry frame: a line turned 0.3 rad about up turns the frame 0.3 rad about up");}
    // the couple: two forces with the sum F and the torque tau about the centre of mass
    {const P::V3 F{10,147,-3},tau{4,-20,7},com{1,2,3};const P::CoupleForces c=P::coupleForces(F,tau,com,.2f,1.f);
     P::V3 sf{0,0,0},st{0,0,0};for(int k=0;k<2;++k){sf=V::add(sf,c.f[k]);st=V::add(st,V::cross(V::sub(c.pos[k],com),c.f[k]));}
     CHECK(nearV(sf,F,1e-3f)&&nearV(st,tau,1e-3f)&&near(V::length(V::sub(c.pos[0],com)),.2f,1e-4f),"carry couple: the two forces sum to F with the torque tau");}
    // at rest under the hands: no error, the weight carried
    {const auto tr=C::run(C::still,1.f);const auto& o=tr.out.back();
     CHECK(V::length(V::sub(tr.com.back(),{0,.75f,.65f}))<.002f&&near(o.fN[1],15.f*9.81f,1.f)&&C::angleTo(tr.R.back(),C::yawRows(0.f))<.2f,"carry: at rest, the weight carried, no drift");}
    // a 10 cm step of the hands to the side
    {const auto tr=C::run([](float t){C::Hands h=C::still(t);if(t>=0.f){h.a[0]+=.1f;h.b[0]+=.1f;}return h;},1.2f);
     float over=0.f,e65=1.f;for(std::size_t i=0;i<tr.t.size();++i){over=std::max(over,tr.com[i][0]-.1f);if(tr.t[i]<=.651f)e65=std::fabs(tr.com[i][0]-.1f);}
     std::printf("     carry 10 cm step: overshoot %.1f %%, error at 0.65 s %.1f mm\n",double(over/.1f*100.f),double(e65*1000.f));
     CHECK(over<=.005f&&e65<=.005f,"carry: a 10 cm step: overshoot <= 5 %, settled within 5 mm by 0.65 s");}
    // a 30 deg turn of the hands' line about the vertical through their middle
    {const P::V3 c{0,1.2f,.45f};const float a=30.f*P::pi/180.f;
     const auto tr=C::run([&](float t){return t>=0.f?C::turned(a,c,0.f):C::still(t);},1.5f);
     float over=-90.f,e65=90.f;for(std::size_t i=0;i<tr.t.size();++i){const float y=C::yawOf(tr.R[i])*180.f/P::pi;over=std::max(over,y-30.f);if(tr.t[i]<=.651f)e65=std::fabs(y-30.f);}
     std::printf("     carry 30 deg step: overshoot %.2f deg, error at 0.65 s %.2f deg\n",double(over),double(e65));
     CHECK(over<=1.5f&&e65<=3.f,"carry: a 30 deg turn of the line: overshoot <= 5 %, within 3 deg by 0.65 s");}
    // walking at 3.6 m/s, then the mover stops within 30 ms
    {const auto hands=[](float t){C::Hands h=C::still(t);const float t1=1.f,ts=.03f;
        const float x=t<t1?3.6f*t:t<t1+ts?3.6f*t1+3.6f*(t-t1)-.5f*(3.6f/ts)*(t-t1)*(t-t1):3.6f*t1+.5f*3.6f*ts;
        const float vz=t<t1?3.6f:t<t1+ts?3.6f-(3.6f/ts)*(t-t1):0.f;h.a[2]+=x;h.b[2]+=x;h.va[2]=vz;h.vb[2]=vz;return h;};
     const auto tr=C::run(hands,2.5f);const float zEnd=.65f+3.6f*1.f+.5f*3.6f*.03f;
     float walkErr=0.f,peak=0.f,back=0.f;std::size_t ip=0;
     for(std::size_t i=0;i<tr.t.size();++i){const float z=tr.com[i][2];
        if(tr.t[i]>.5f&&tr.t[i]<.98f)walkErr=std::max(walkErr,std::fabs(z-(.65f+3.6f*tr.t[i])));
        if(tr.t[i]>1.f&&z-zEnd>peak){peak=z-zEnd;ip=i;}}
     for(std::size_t i=ip;i<tr.t.size();++i)back=std::min(back,tr.com[i][2]-zEnd);
     std::printf("     carry walk 3.6 m/s: error %.1f cm; the stop: peak %.1f cm past, then %.1f cm back past\n",double(walkErr*100.f),double(peak*100.f),double(-back*100.f));
     CHECK(walkErr<=.02f&&peak<=.20f&&back>=-.015f,"carry: a steady walk within 2 cm, the stop's swing <= 20 cm with no reversal over 1.5 cm");}
    // the player turning at 90 deg/s with the hands in front of him: the chair keeps up, no roll
    {const P::V3 c{0,1.2f,0};const float wy=P::pi*.5f;
     const auto tr=C::run([&](float t){return C::turned(wy*std::max(t,0.f),c,wy);},1.5f);
     float peak=0.f,endErr=0.f,roll=0.f,tPeak=0.f,tEnd=0.f;
     for(std::size_t i=0;i<tr.t.size();++i){const float e=C::angleTo(tr.R[i],C::yawRows(wy*tr.t[i]));peak=std::max(peak,e);if(tr.t[i]>1.2f)endErr=std::max(endErr,e);
        const float et=V::length(tr.out[i].theta)*180.f/P::pi;tPeak=std::max(tPeak,et);if(tr.t[i]>1.2f)tEnd=std::max(tEnd,et);
        const P::V3 x=V::normalized(V::sub(C::turned(wy*tr.t[i],c,wy).b,C::turned(wy*tr.t[i],c,wy).a));
        const P::V3 rv=P::rotationBetween(tr.R[i],C::yawRows(wy*tr.t[i]));roll=std::max(roll,std::fabs(V::dot(rv,x))*180.f/P::pi);}
     std::printf("     carry 90 deg/s turn: behind the hands' line peak %.1f deg, after 1.2 s %.1f deg (the 50 ms line filter alone %.1f); behind the target peak %.1f, after 1.2 s %.2f deg; roll %.2f deg\n",
        double(peak),double(endErr),double(wy*.05f*180.f/P::pi),double(tPeak),double(tEnd),double(roll));
     CHECK(tPeak<=5.f&&tEnd<=1.f&&endErr<=wy*.05f*180.f/P::pi+1.5f&&peak<=9.f&&roll<=3.f,
           "carry: a 90 deg/s turn: <= 5 deg behind the target, <= 1 deg after 1.2 s; behind the line by the line filter + 1.5 deg; roll <= 3 deg");}
    // +-3 mm of tracking noise on each hand (and its velocity): the chair stays still
    {const auto tr=C::run(C::still,3.f,7u);float ang=0.f,pos=0.f;
     for(std::size_t i=0;i<tr.t.size();++i)if(tr.t[i]>.5f){ang=std::max(ang,C::angleTo(tr.R[i],C::yawRows(0.f)));pos=std::max(pos,V::length(V::sub(tr.com[i],{0,.75f,.65f})));}
     std::printf("     carry +-3 mm noise: angle %.2f deg, centre of mass %.1f mm\n",double(ang),double(pos*1000.f));
     CHECK(ang<=1.5f&&pos<=.01f,"carry: +-3 mm hand noise: <= 1.5 deg and <= 1 cm");}
    // a swing: the hands circle the player at up to 6 rad/s (reached in 0.1 s)
    {const P::V3 c{0,1.2f,0};const auto ang=[](float t){return t<.1f?30.f*t*t:.3f+6.f*(t-.1f);};const auto rate=[](float t){return t<.1f?60.f*t:6.f;};
     const auto tr=C::run([&](float t){return C::turned(ang(std::max(t,0.f)),c,rate(std::max(t,0.f)));},.5f);
     float w25=0.f;for(std::size_t i=0;i<tr.t.size();++i)if(tr.t[i]<=.25f)w25=std::max(w25,tr.w[i][1]);
     std::printf("     carry swing to 6 rad/s: the chair's yaw rate %.2f rad/s by 0.25 s\n",double(w25));
     CHECK(w25>=4.f,"carry: a swing at 6 rad/s: the chair turns at 4 rad/s or more within 0.25 s");}
    // the fallback approach of a ball-jointed hand without the drive: first order, no overshoot, the mover's velocity kept
    {P::V3 x{0,0,0};const P::V3 base{1,0,0};float over=0.f;for(int i=0;i<100;++i){const P::V3 cmd={.1f+1.f*float(i+1)*.0139f,0,0};
        const auto d=P::followTo(x,cmd,.0139f,.1f,6.f,base);x=V::add(x,V::mul(d.vel,.0139f));over=std::max(over,x[0]-cmd[0]);}
     const auto d1=P::followTo({0,0,0},{.05f,0,0},.0139f,0.f,6.f,{0,0,0});
     CHECK(over<=1e-4f&&near(d1.vel[0],.05f/.0139f,1e-2f),"fallback: tau 100 ms approaches without overshoot; tau 0 = one substep (velocityTo)");}
    // the gains at a substep: wn dt and zeta wn dt at most 0.5 (the levers allow wn 30, zeta 3); the defaults unchanged
    {P::CarryGains hi{};hi.wn=30.f;hi.zeta=3.f;hi.wr=30.f;hi.zetaR=3.f;const P::CarryGains c=P::carryGainsFor(hi,.0139f),d=P::carryGainsFor(P::CarryGains{},.0139f);
     CHECK(c.wn*.0139f<=.5f+1e-4f&&c.zeta*c.wn*.0139f<=.5f+1e-4f&&c.wr*.0139f<=.5f+1e-4f&&c.zetaR*c.wr*.0139f<=.5f+1e-4f&&
           near(d.wn,10.f)&&near(d.zeta,1.f)&&near(d.wr,10.f)&&near(d.zetaR,1.4f),"carry gains: explicit-step limits at 13.9 ms, the defaults unchanged");
     C::Opt o{};o.g=hi;const P::V3 cc{0,1.2f,.45f};const float a=30.f*P::pi/180.f;
     const auto tr=C::run([&](float t){return t>=0.f?C::turned(a,cc,0.f):C::still(t);},1.5f,0u,o);
     float late=0.f,wMax=0.f;for(std::size_t i=0;i<tr.t.size();++i){wMax=std::max(wMax,V::length(tr.w[i]));if(tr.t[i]>1.f)late=std::max(late,std::fabs(C::yawOf(tr.R[i])*180.f/P::pi-30.f));}
     std::printf("     carry at the levers' top gains (wn 30, zeta 3): after 1 s %.2f deg off, spin max %.1f rad/s\n",double(late),double(wMax));
     CHECK(late<=1.f&&wMax<=20.f&&std::isfinite(late),"carry: the levers' top gains stay stable (no chatter, settled)");}
    // the upward force capped (the player's mover on the prop): F . up <= upMaxN
    {P::CarryState cs{};const C::Body b{};P::carryJoin(cs,C::toWorld(b,C::gA),C::toWorld(b,C::gB),b.R,b.p,{0,1,0},1.f);
     const P::CarryOut o=P::carryTick(cs,{.24f,0,0},{0,1.f,-.2f},{0,0,0},b.p,{0,0,0},b.R,{0,0,0},C::inertia,15.f,70.f,{0,1,0},P::CarryGains{},.0139f,.05f,1.f,15.f*9.81f);
     const P::CarryOut f=P::carryTick(cs,{.24f,0,0},{0,1.f,-.2f},{0,0,0},b.p,{0,0,0},b.R,{0,0,0},C::inertia,15.f,70.f,{0,1,0},P::CarryGains{},.0139f,.05f,1.f);
     CHECK(o.ok&&o.capUp&&o.fN[1]<=15.f*9.81f+1e-2f&&f.fN[1]>15.f*9.81f+50.f&&!f.capUp,"carry: the upward force at most the prop's weight while the player stands on it");
     CHECK(near(o.aPred[1],o.fN[1]/15.f-9.81f,1e-3f)&&nearV(o.alpha,P::inertiaSolve(b.R,C::inertia,o.tauNm),1e-4f),"carry: the predicted acceleration F / M - g and I^-1 tau");}
    // a resumed pair: the frame takes the hands' line at once, no frame spin
    {P::CarryState cs{};const C::Body b{};P::carryJoin(cs,C::toWorld(b,C::gA),C::toWorld(b,C::gB),b.R,b.p,{0,1,0},1.f);cs.resync=true;
     const P::V3 l{std::cos(.4f)*.24f,0,-std::sin(.4f)*.24f};
     P::carryTick(cs,l,{0,1.f,-.2f},{0,0,0},b.p,{0,0,0},b.R,{0,0,0},C::inertia,15.f,70.f,{0,1,0},P::CarryGains{},.0139f,.05f,1.f);
     CHECK(!cs.resync&&nearV(cs.wT,{0,0,0})&&nearV(cs.frame.x,V::normalized(l),1e-4f),"carry: a resumed pair resyncs its frame to the hands' line with no spin");}
    // the hand's velocity after the step: vGrip + (a + alpha x r) dt
    {const P::V3 v=P::carryGripVel({1,0,0},{0,1,0},{0,0,0},{0,2,0},{0,0,3},.01f,1.f);
     CHECK(nearV(v,{1.f-.03f,.02f,0},1e-5f),"carry grip velocity: vGrip + (a + alpha x r) dt");}
    // the two 8 kg hand bodies on the grips: the hands at the grip's velocity before the step add their mass and inertia to the chair (the
    // drive tuned for 15 kg underdamped); at the predicted velocity the chair moves as without them
    {const P::V3 cc{0,1.2f,.45f};const float a=30.f*P::pi/180.f;
     const auto turn=[&](const C::Opt& o){const auto tr=C::run([&](float t){return t>=0.f?C::turned(a,cc,0.f):C::still(t);},1.5f,0u,o);
        float over=-90.f;for(std::size_t i=0;i<tr.t.size();++i)over=std::max(over,C::yawOf(tr.R[i])*180.f/P::pi-30.f);return over;};
     const auto stop=[&](const C::Opt& o,float& peak,float& back){
        const auto hands=[](float t){C::Hands h=C::still(t);const float t1=1.f,ts=.03f;
            const float x=t<t1?3.6f*t:t<t1+ts?3.6f*t1+3.6f*(t-t1)-.5f*(3.6f/ts)*(t-t1)*(t-t1):3.6f*t1+.5f*3.6f*ts;
            const float vz=t<t1?3.6f:t<t1+ts?3.6f-(3.6f/ts)*(t-t1):0.f;h.a[2]+=x;h.b[2]+=x;h.va[2]=vz;h.vb[2]=vz;return h;};
        const auto tr=C::run(hands,2.5f,0u,o);const float zEnd=.65f+3.6f*1.f+.5f*3.6f*.03f;peak=0.f;back=0.f;std::size_t ip=0;
        for(std::size_t i=0;i<tr.t.size();++i){const float z=tr.com[i][2];if(tr.t[i]>1.f&&z-zEnd>peak){peak=z-zEnd;ip=i;}}
        for(std::size_t i=ip;i<tr.t.size();++i)back=std::min(back,tr.com[i][2]-zEnd);};
     C::Opt bare{},drag{},pred{};drag.hands=true;pred.hands=true;pred.predict=true;
     const float o0=turn(bare),o1=turn(drag),o2=turn(pred);float p0,b0,p1,b1,p2,b2;stop(bare,p0,b0);stop(drag,p1,b1);stop(pred,p2,b2);
     std::printf("     carry with 2 x 8 kg hand bodies: 30 deg turn overshoot %.2f / %.2f / %.2f deg, the stop %.1f / %.1f / %.1f cm past and %.1f / %.1f / %.1f cm back"
                 " (no hands / hands at the grip's velocity / predicted)\n",double(o0),double(o1),double(o2),double(p0*100.f),double(p1*100.f),double(p2*100.f),
                 double(-b0*100.f),double(-b1*100.f),double(-b2*100.f));
     CHECK(o2<=1.5f&&p2<=.20f&&b2>=-.015f&&o2<=o1+1e-3f,"carry with hand bodies: at the predicted grip velocity the turn and the stop keep the bare chair's bounds");}
}
int main(){
    stub::initVtables();
    // ---------- LIGHT GHOST (a closing hand passes through the light prop it closes on) ----------
    {const float nan=std::numeric_limits<float>::quiet_NaN();
     const auto st=[](float sq,float lag,bool fist,bool nav,bool door,float kg,bool lever){return P::lightGhostStart(P::lightGhostClosing(sq,lag,fist,lever),nav,door,kg,8.f);};
     CHECK(!st(.2f,0.f,false,true,false,.35f,true),"ghost: an open hand (.2) pushes a cup as before");
     CHECK(st(.4f,.1f,false,true,false,.35f,true),"ghost: a closing hand (.4, rising) on a 0.35 kg cup");
     CHECK(!st(.4f,.38f,false,true,false,.35f,true)&&!st(.55f,.55f,false,true,false,.35f,true),"ghost: fingers resting on the grip (.4 / .55 steady) push cups");
     CHECK(!st(.4f,-1.f,false,true,false,.35f,true),"ghost: no squeeze history yet");
     CHECK(!st(.7f,.2f,true,true,false,.35f,true),"ghost: a fist knocks cups");
     CHECK(!st(.4f,.1f,false,true,true,.35f,true),"ghost: never a door");
     CHECK(!st(.4f,.1f,false,false,false,.35f,true),"ghost: nav props only");
     CHECK(!st(.4f,.1f,false,true,false,10.f,true)&&!st(.4f,.1f,false,true,false,59.5f,true),"ghost: over grabRotKg (an inactive cup's 10 kg archetype, a chair) collides");
     CHECK(!st(.4f,.1f,false,true,false,-1.f,true)&&!st(.4f,.1f,false,true,false,0.f,true),"ghost: no active collider mass");
     CHECK(!st(nan,.1f,false,true,false,.35f,true),"ghost: NaN squeeze");
     CHECK(!st(.4f,.1f,false,true,false,.35f,false),"ghost: lever 0");
     CHECK(P::lightGhostKg(.35f,8.f)&&P::lightGhostKg(8.f,8.f)&&!P::lightGhostKg(59.5f,8.f)&&!P::lightGhostKg(0.f,8.f)&&!P::lightGhostKg(nan,8.f),
           "ghost: the light class for the skipped first-contact push (a chair keeps its kick)");
     // the lag: a 150 ms close from 0 to 1 at 72 Hz reads rising from its first tick; a steady .5 grip never does; NaN keeps the lag
     {float lag=-1.f;lag=P::squeezeLagStep(lag,0.f,0);bool rising=true;
      for(int i=1;i<=11;++i){const float sq=float(i)/11.f;rising=rising&&(sq<.30f||P::lightGhostClosing(sq,lag,false,true)||sq>.65f);lag=P::squeezeLagStep(lag,sq,13'888'889ull);}
      CHECK(rising,"ghost lag: a closing hand reads rising through the closing band");
      float steady=-1.f;bool any=false;for(int i=0;i<200;++i){const float sq=.5f+((i&1)?.01f:-.01f);any=any||P::lightGhostClosing(sq,steady,false,true);steady=P::squeezeLagStep(steady,sq,13'888'889ull);}
      CHECK(!any,"ghost lag: a steady half grip with 1 % noise never reads closing");
      CHECK(P::squeezeLagStep(.3f,nan,13'888'889ull)==.3f,"ghost lag: NaN keeps the lag");}
     const std::uint64_t ms=1'000'000ull,t0=50'000*ms,quiet=300*ms,hard=10'000*ms;
     P::LightGhost g{0x1000,t0,t0,P::GhostIntent};
     CHECK(P::lightGhostKeep(g,t0+299*ms,quiet,hard)==P::GhostKeep&&P::lightGhostKeep(g,t0+300*ms,quiet,hard)==P::GhostQuiet,"ghost keep: seen within the quiet window");
     g.seenNs=t0+9'900*ms;CHECK(P::lightGhostKeep(g,t0+10'000*ms,quiet,hard)==P::GhostHard,"ghost keep: the hard bound");
     P::LightGhost z{};CHECK(P::lightGhostKeep(z,t0,quiet,hard)!=P::GhostKeep,"ghost keep: an empty slot ends");
     // a reach through a cup: squeeze 0 -> .4 -> .7 (fist) -> .1 while the pair overlaps
     std::array<P::LightGhost,2> slots{};const std::uintptr_t cup=0xc0f;std::uint64_t now=t0;bool started=false;int drops=0,starts=0;
     const float squeeze[]{0.f,.1f,.25f,.4f,.55f,.7f,.9f,.9f,.6f,.3f,.1f,0.f};float lag=P::squeezeLagStep(-1.f,0.f,0);
     for(float sq:squeeze){now+=14*ms;const bool fist=sq>.65f;
        const bool start=P::lightGhostStart(P::lightGhostClosing(sq,lag,fist,true),true,false,.35f,8.f);lag=P::squeezeLagStep(lag,sq,14*ms);
        if(P::lightGhostFilter(slots,cup,start,now,&started))++drops;if(started)++starts;}
     CHECK(starts==1&&drops==9,"ghost: starts once at the first closing contact and drops every pair after it, through the fist and the open hand");
     CHECK(P::lightGhostKeep(slots[0],now+100*ms,quiet,hard)==P::GhostKeep&&P::lightGhostKeep(slots[0],now+300*ms,quiet,hard)==P::GhostQuiet,
           "ghost: contacts come back after the pair stopped overlapping for the quiet window");
     CHECK(!P::lightGhostFilter(slots,0xd00,false,now,&started)&&!started,"ghost: another prop collides");
     // a fixed-rotation hold ends with the hand still overlapping the cup: the release ghost drops the pair until it separates
     std::array<P::LightGhost,2> rel{};CHECK(P::lightGhostArm(rel,cup,now,P::GhostRelease)&&rel[0].why==P::GhostRelease,"ghost: armed at a light hold's end");
     bool all=true;for(int i=0;i<10;++i){now+=14*ms;all=all&&P::lightGhostFilter(rel,cup,false,now);}
     CHECK(all&&P::lightGhostKeep(rel[0],now+299*ms,quiet,hard)==P::GhostKeep,"ghost: the released cup's pair stays dropped while it overlaps the opening hand");
     // two slots: a third prop gives up the one seen longest ago
     std::array<P::LightGhost,2> two{};P::lightGhostArm(two,1,t0,P::GhostIntent);P::lightGhostArm(two,2,t0+ms,P::GhostIntent);P::lightGhostFilter(two,1,false,t0+2*ms);
     P::lightGhostArm(two,3,t0+3*ms,P::GhostIntent);CHECK((two[0].inst==1||two[1].inst==1)&&(two[0].inst==3||two[1].inst==3),"ghost: a full set gives up the slot seen longest ago");
     // an open hand's first contact still gets the push speed (contactDrive / yield unchanged)
     CHECK(!P::lightGhostIntent(.29f,false,true)&&P::lightGhostIntent(.30f,false,true)&&!P::lightGhostIntent(.5f,true,true),"ghost: the closing band is [.30, fist)");}
    // ---------- math ----------
    {const auto d=P::velocityTo({0,0,0},{.1f,0,0},.01f,6.f);CHECK(near(d.vel[0],6.f)&&d.clamped,"velocity clamped to 6 m/s");
     const auto e=P::velocityTo({0,0,0},{.01f,0,0},.01f,6.f);CHECK(near(e.vel[0],1.f)&&!e.clamped,"velocity (target - current) / dt");
     const auto z=P::velocityTo({0,0,0},{1,0,0},0.f,6.f);CHECK(nearV(z.vel,{0,0,0}),"no velocity for dt 0");
     const auto r=P::velocityTo({0,0,0},{.1f,0,0},.01f,6.f,{5,0,0});CHECK(near(r.vel[0],10.f)&&!r.clamped,"the clamp applies to the part relative to the mover");
     const auto rc=P::velocityTo({0,0,0},{.2f,0,0},.01f,6.f,{5,0,0});CHECK(near(rc.vel[0],11.f)&&rc.clamped,"mover velocity + 6 m/s");}
    {P::HandTarget t{};t.rel={.5f,0,0};t.vel={1,0,0};
     const auto c=P::command(t,{1,0,0},{2,0,0},.01f,.01f);CHECK(nearV(c,{1.f+.02f+.5f+.02f,0,0}),"command = mover + mover vel * step + rel + vel * (since + step)");
     const auto c2=P::command(t,{0,0,0},{0,0,0},1.f,.01f);CHECK(nearV(c2,{.5f+P::maxAheadS,0,0}),"extrapolation capped at maxAheadS");}
    {const auto w=P::spinTo({1,0,0},{0,1,0},.1f,100.f);CHECK(nearV(w,{0,0,1.5707963f/.1f},1e-3f),"spin: 90 deg about +z over dt");
     const auto c=P::spinTo({1,0,0},{0,1,0},.1f,5.f);CHECK(near(V::length(c),5.f,1e-4f),"spin clamped");
     CHECK(nearV(P::spinTo({0,0,1},{0,0,1},.01f,20.f),{0,0,0}),"aligned: no spin");}
    {float m[16];P::bodyMatrix({0,0,2},{1,2,3},m);
     const P::V3 a{m[0],m[1],m[2]},b{m[4],m[5],m[6]},c{m[8],m[9],m[10]};
     CHECK(nearV(b,{0,0,1})&&near(V::dot(a,b),0.f)&&near(V::dot(a,c),0.f)&&near(V::length(a),1.f),"body matrix: row b = the axis, orthonormal");
     CHECK(nearV(V::cross(a,b),c)&&near(m[12],1.f)&&near(m[13],2.f)&&near(m[14],3.f)&&m[3]==0.f&&m[15]==0.f,"right handed, row d = position");}
    CHECK(near(P::capsuleSegment(.12f,.03f),.06f)&&P::capsuleSegment(.04f,.03f)==0.f,"capsule segment = length - 2 r");
    CHECK(P::insideBody({.1f,1.3f,0},{0,1.6f,0},{0,1,0},1.f)&&!P::insideBody({.5f,1.3f,0},{0,1.6f,0},{0,1,0},1.f)&&!P::insideBody({0,2.0f,0},{0,1.6f,0},{0,1,0},1.f),"body cylinder");
    CHECK(nearV(P::chestPoint({0,1.6f,0},{0,2,0},1.f),{0,1.35f,0}),"chest = eye - 25 cm along up");
    {P::V3 v=P::targetVelocity({0,0,0},{0,0,0},{.1f,0,0},.1f,false);CHECK(nearV(v,{.5f,0,0}),"target velocity smoothed");
     CHECK(nearV(P::targetVelocity({1,0,0},{0,0,0},{.1f,0,0},.2f,false),{0,0,0}),"a gap over 100 ms restarts");}
    // ---------- the palm frame ----------
    {// grip frame: X = world X, Y = world Y, Z = world Z; aim forward -Z
     const P::V3 fa{1,0,0},fb{0,1,0},fc{0,0,1},aim{0,0,-1},grip{0,1,0};
     const auto r=P::handFrame(grip,fa,fb,fc,aim,true,.07f,.03f,1.f),l=P::handFrame(grip,fa,fb,fc,aim,false,.07f,.03f,1.f);
     CHECK(r.valid&&nearV(r.N,{-1,0,0})&&nearV(l.N,{1,0,0}),"palm normal: right = -grip X, left = +grip X");
     CHECK(nearV(V::cross(r.S,r.F),r.N,1e-5f)&&nearV(V::cross(l.S,l.F),l.N,1e-5f)&&nearV(r.F,{0,0,-1}),"S x F = N, F = the aim");
     CHECK(nearV(r.W,{.03f,1.f,.07f},1e-5f)&&nearV(r.B,V::add(r.W,V::mul(r.F,.05f)),1e-6f),"W = grip - F wristBack - N wristDepth, B = W + 5 cm F");
     const auto s=P::handFrame(grip,fa,fb,fc,{1,0,0},true,.07f,.03f,2.f);
     CHECK(s.valid&&nearV(s.F,V::normalized({0,-1,-1}),1e-5f)&&nearV(s.B,V::add(s.W,V::mul(s.F,.10f)),1e-5f),"an aim along the palm normal: the default aim (0, -.7071, -.7071), upm");}
    // ---------- rotations ----------
    {std::mt19937 g(7);std::uniform_real_distribution<float> ang(0.f,P::pi);bool okInt=true,okCap=true,okPi=true;
     for(int i=0;i<200;++i){const P::V3 k=randUnit(g);const float t=i<20?P::pi-1e-4f*float(i):ang(g);
        const P::Rows now=V::rotate(V::mul(randUnit(g),ang(g)),identity),want=V::rotate(V::mul(k,t),now);
        const P::V3 w=P::spinFrame(now,want,.01f,1e6f);const P::Rows got=V::rotate(V::mul(w,.01f),now);
        if(!nearRows(got,want,1e-3f))okInt=false;
        if(i<20&&!near(V::length(w)*.01f,t,2e-3f))okPi=false;
        const P::V3 c=P::spinFrame(now,want,.01f,20.f);if(V::length(c)>20.001f)okCap=false;}
     CHECK(okInt,"spinFrame: integrating w dt reaches the wanted rows (200 random rotations)");
     CHECK(okPi,"spinFrame: angles near pi");CHECK(okCap,"spinFrame: the cap holds");
     const P::V3 w=P::frameAngVel(identity,V::rotate({0,0,.1f},identity),.05f,{0,0,0},false);CHECK(nearV(w,{0,0,1.f},1e-3f),"frameAngVel: 0.1 rad over 50 ms = 2 rad/s, smoothed to half");}
    // ---------- the drive against a surface ----------
    {const P::V3 n{-1,0,0};
     const auto s=P::contactDrive({5,0,0},{0,0,0},n,0,false,.10f,1.5f,3.f);CHECK(near(s.vel[0],.10f)&&s.clipped&&near(s.approach,5.f),"static contact: into the surface clipped to 0.10 m/s");
     const auto d=P::contactDrive({5,0,0},{0,0,0},n,2,true,.10f,1.5f,3.f);CHECK(near(d.vel[0],1.5f)&&d.clipped,"dynamic contact: clipped to 1.5 m/s");
     const auto t=P::contactDrive({5,2,0},{0,0,0},n,0,false,.10f,1.5f,3.f);CHECK(near(t.vel[1],2.f)&&near(t.vel[0],.10f),"the part along the surface kept");
     const auto tc=P::contactDrive({0,5,0},{0,0,0},n,0,false,.10f,1.5f,3.f);CHECK(near(tc.vel[1],3.f)&&tc.tangentCapped,"the part along the surface capped at 3 m/s");
     const auto mb=P::contactDrive({0,6,0},{0,4,0},n,0,false,.10f,1.5f,3.f);CHECK(near(mb.vel[1],6.f)&&!mb.tangentCapped,"the cap relative to the mover");
     const auto out=P::contactDrive({-2,0,0},{0,0,0},n,0,false,.10f,1.5f,3.f);CHECK(near(out.vel[0],-2.f)&&!out.clipped,"moving out of the surface: untouched");
     const auto fr=P::contactDrive({5,0,0},{0,0,0},n,4,false,.10f,1.5f,3.f);CHECK(near(fr.vel[0],5.f)&&!fr.clipped,"no contact for 4 substeps: deadbeat");
     // pressed 30 cm past the surface while sliding along it at 1 m/s (72 Hz substep): the clip comes before the clamp
     const float dt=1.f/72.f;const P::V3 from{0,0,0},to{.30f,1.f*dt,0};
     const auto sd=P::substepDrive(from,to,dt,6.f,{0,0,0},n,0,false,.10f,1.5f,3.f);
     CHECK(near(sd.vel[0],.10f)&&near(sd.vel[1],1.f,1e-3f)&&sd.clipped&&!sd.clamped,"substep drive: pressed 30 cm, the 1 m/s slide kept, into the surface 0.10 m/s");
     const auto old=P::contactDrive(P::velocityTo(from,to,dt,6.f).vel,{0,0,0},n,0,false,.10f,1.5f,3.f);
     CHECK(old.vel[1]<.3f,"clamp before the clip: the slide shrinks to under 0.3 m/s");
     const auto fz=P::substepDrive(from,{.10f,0,0},.01f,6.f,{0,0,0},n,4,false,.10f,1.5f,3.f);CHECK(near(fz.vel[0],6.f)&&fz.clamped&&!fz.clipped,"substep drive, no contact: clamped to 6 m/s");
     const auto mz=P::substepDrive(from,{.2f,0,0},.01f,6.f,{5,0,0},n,4,false,.10f,1.5f,3.f);CHECK(near(mz.vel[0],11.f)&&mz.clamped,"substep drive: the clamp relative to the mover");}
    {P::ContactEst e{};auto r=P::noteSubstep(e,{-.002f,0,0},{0,0,0},{1,0,0},.0005f,.05f);
     CHECK(r.kind==P::ContactPush&&nearV(e.n,{-1,0,0})&&e.age==0&&r.prevAge==255,"push contact: n = the push");
     r=P::noteSubstep(e,{0,-.002f,0},{0,0,0},{1,0,0},.0005f,.05f);CHECK(r.flip&&nearV(e.n,V::normalized({-1,-1,0}),1e-5f),"a second normal 90 deg away: averaged, a flip");
     for(int i=0;i<5;++i)r=P::noteSubstep(e,{0,0,0},{0,0,0},{1,0,0},.0005f,.05f);CHECK(r.kind==P::ContactNone&&e.age==5,"no contact: age counts substeps");
     r=P::noteSubstep(e,{0,0,0},{-.2f,0,0},{1,0,0},.0005f,.05f);CHECK(r.kind==P::ContactDv&&nearV(e.n,{-1,0,0})&&e.age==0,"velocity contact: n = dv (an old normal not averaged)");
     auto e2=P::ContactEst{};r=P::noteSubstep(e2,{0,0,0},{.2f,0,0},{1,0,0},.0005f,.05f);CHECK(r.kind==P::ContactNone,"a dv along the set velocity is no contact");
     r=P::noteSubstep(e2,{.0004f,0,0},{0,0,0},{1,0,0},.0005f,.05f);CHECK(r.kind==P::ContactNone,"a push under 0.5 mm is no contact");}
    // ---------- the toy wall ----------
    {const auto nw=toyWall(true,0.f),nc=toyWall(true,.02f),ow=toyWall(false,.02f);
     std::printf("     toy wall: new law depth pre max %.2f mm, solved %.2f..%.2f mm, std %.3f mm, contacts %d/%d, release %d; old law (2 cm push cap) max %.1f mm\n",
        double(nw.maxDepthPre*1e3f),double(nw.minDepthSolved*1e3f),double(nw.maxDepthSolved*1e3f),double(nw.stdSolved*1e3f),nw.holdContacts,nw.holdSubsteps,nw.releaseSubsteps,double(ow.maxDepthPre*1e3f));
     CHECK(nw.minDepthSolved>=.0069f&&nw.maxDepthSolved<=.0085f,"new law: pressing 30 cm into a wall, the body rests 7..8.5 mm deep");
     CHECK(nw.stdSolved<.001f,"new law: no shaking (std under 1 mm)");
     CHECK(nw.holdContacts==nw.holdSubsteps,"new law: contact every substep");
     CHECK(nw.releaseSubsteps>=0&&nw.releaseSubsteps<=1,"new law: released within 1 substep of pulling out");
     CHECK(!nc.pushedThrough&&nc.maxDepthSolved<=.0085f,"new law with a solver limited to 2 cm per substep: holds");
     CHECK(ow.pushedThrough,"old law with the same solver: pushes through");}
    // ---------- give: an active prop next to a wall does not raise the push into the wall ----------
    {const auto w=toyGive(0),b=toyGive(1),pr=toyGive(2);
     std::printf("     give: wall max depth %.1f mm give %.2f; bias wall max depth %.1f mm give %.2f; prop travel %.0f cm, max give %.2f, end give %.2f, end depth %.1f mm, rises %d drops %d\n",
        double(w.maxDepth*1e3f),double(w.maxGive),double(b.maxDepth*1e3f),double(b.maxGive),double(pr.propTravel*1e2f),double(pr.maxGive),double(pr.giveAtStop),double(pr.holdDepth*1e3f),pr.rises,pr.drops);
     CHECK(!w.pushedThrough&&w.maxGive==0.f&&w.maxDepth<.0085f,"give: a wall with an active prop in the broadphase stays at creep");
     CHECK(!b.pushedThrough&&b.maxGive==0.f,"give: a bias-solved wall with an active prop in the broadphase stays at creep");
     CHECK(pr.propTravel>.29f&&pr.maxGive>=1.f,"give: a light prop that moves away raises the push to full");
     CHECK(!pr.pushedThrough&&pr.giveAtStop==0.f&&pr.drops>=1,"give: the prop stopped by a backstop drops the push back to creep, no push-through");}
    // ---------- visual stop ----------
    {P::VisualMem m{};const P::V3 n{-1,0,0};const float dt=1.f/36.f;
     auto o=P::visualPlane(m,true,n,-.986f,{1.05f,0,0},identity,identity,dt,.006f,.4f,.03f,30.f*P::pi/180.f,true);
     CHECK(nearV(o.offset,{-(1.05f-.986f+.006f),0,0},1e-5f),"growth at once: the hand stops at the solved plane + lift");
     o=P::visualPlane(m,true,n,-.986f,{1.05f,.3f,0},identity,identity,dt,.006f,.4f,.03f,30.f*P::pi/180.f,true);
     CHECK(nearV(o.offset,{-(1.05f-.986f+.006f),0,0},1e-5f),"sliding along the wall: only the normal part");
     const float h0=-o.offset[0];o=P::visualPlane(m,true,n,-.986f,{.9f,0,0},identity,identity,dt,.006f,.4f,.03f,30.f*P::pi/180.f,true);
     CHECK(near(-o.offset[0],h0*std::exp(-dt/.03f),1e-4f),"pulled out of the wall: the offset shrinks with tau 30 ms");
     for(int i=0;i<20;++i)o=P::visualPlane(m,false,n,0.f,{.9f,0,0},identity,identity,dt,.006f,.4f,.03f,30.f*P::pi/180.f,true);
     CHECK(V::length(o.offset)<1e-6f,"no contact: falls to 0");
     P::VisualMem c{};o=P::visualPlane(c,true,n,-.5f,{1.5f,0,0},identity,identity,dt,.006f,.4f,.03f,30.f*P::pi/180.f,true);CHECK(near(-o.offset[0],.4f),"capped at the leash");
     P::VisualMem rm{};const P::Rows tilted=V::rotate({0,0,1.f},identity);
     o=P::visualPlane(rm,true,n,-1.f,{1.f,0,0},tilted,identity,dt,.006f,.4f,.03f,30.f*P::pi/180.f,true);
     CHECK(near(V::length(o.rot),30.f*P::pi/180.f,1e-4f)&&o.rot[2]>0.f,"rotation offset R_solved R_cmd^T capped at 30 deg");
     const float r0=V::length(o.rot);o=P::visualPlane(rm,false,n,0.f,{1.f,0,0},tilted,identity,dt,.006f,.4f,.03f,30.f*P::pi/180.f,true);
     CHECK(near(V::length(o.rot),r0*std::exp(-dt/.04f),1e-4f),"out of contact: the rotation decays with tau 40 ms");
     CHECK(P::fresh(1000*ms,1000*ms-79*ms,80*ms)&&!P::fresh(1000*ms,1000*ms-81*ms,80*ms),"hold: a contact up to 80 ms old keeps the visual");}
    // ---------- classes ----------
    {P::ClassIn c{};c.onFoot=true;c.sourceFree=true;CHECK(P::classify(c)==P::ClassBody,"empty free hand on foot: a body");
     auto k=c;k.heldHere=true;k.weapon=true;k.knife=true;k.hold=1;CHECK(P::classify(k)==P::ClassHeld,"held knife: no body yet");
     auto horse=c;horse.onFoot=false;CHECK(P::classify(horse)==P::ClassNone,"horse: none");
     auto br=c;br.bridged=true;CHECK(P::classify(br)==P::ClassNone,"turn bridge: none for the AppSW box");
     br.bridged=false;CHECK(P::classify(br)==P::ClassBody,"the physics class is taken with bridged = false: a snap turn keeps the body");
     CHECK(P::bodyOnFoot(true,false,1,false)&&!P::bodyOnFoot(true,false,2,false)&&!P::bodyOnFoot(true,true,0,false)&&!P::bodyOnFoot(true,false,0,true),"on foot gate");}
    // ---------- state machine ----------
    {P::Machine m{};P::Levers l{};P::MachineIn in{};in.want=true;in.present=false;in.devM=1e9f;in.now=1'000*ms;
     CHECK(P::step(m,in,l)==P::ActInsert&&m.state==P::BodyTravel,"want -> insert, travel");
     in.present=true;in.now+=28*ms;in.devM=.06f;CHECK(P::step(m,in,l)==P::ActNone&&m.state==P::BodyTravel,"6 cm off: still travelling");
     in.now+=28*ms;in.devM=.04f;CHECK(P::step(m,in,l)==P::ActNone&&m.state==P::BodyLive,"under 5 cm -> live");
     in.devM=.39f;in.now+=700*ms;CHECK(P::step(m,in,l)==P::ActNone&&m.state==P::BodyLive,"39 cm: inside the leash");
     in.devM=.41f;in.now+=28*ms;CHECK(P::step(m,in,l)==P::ActNone,"past the leash: not yet");
     in.now+=560*ms;CHECK(P::step(m,in,l)==P::ActNone&&m.state==P::BodyLive,"560 ms: still live");
     in.now+=50*ms;CHECK(P::step(m,in,l)==P::ActPark&&m.state==P::BodyPark,"600 ms past the leash -> park");
     in.returnOk=true;in.now+=28*ms;CHECK(P::step(m,in,l)==P::ActNone&&m.state==P::BodyPark,"the target back at the parked body: waits 100 ms");
     in.now+=100*ms;CHECK(P::step(m,in,l)==P::ActUnpark&&m.state==P::BodyLive,"held 100 ms -> live in place");
     in.returnOk=false;in.devM=.5f;in.now+=28*ms;P::step(m,in,l);in.now+=650*ms;CHECK(P::step(m,in,l)==P::ActPark,"parked again");
     in.targetInBody=true;in.now+=28*ms;CHECK(P::step(m,in,l)==P::ActRespawn&&m.state==P::BodyTravel,"the target at the body -> from the chest");
     in.targetInBody=false;in.devM=0.f;in.now+=28*ms;P::step(m,in,l);in.devM=.5f;in.now+=28*ms;P::step(m,in,l);in.now+=650*ms;P::step(m,in,l);
     CHECK(m.state==P::BodyPark,"parked a third time");
     in.now+=5'100*ms;CHECK(P::step(m,in,l)==P::ActRemove&&m.state==P::BodyAbsent&&m.leashGhost,"a park over 5 s leaves the level");
     in.present=false;in.now+=28*ms;CHECK(P::step(m,in,l)==P::ActNone,"absent after a park: waits for the hand to come back");
     in.targetInBody=true;in.now+=28*ms;CHECK(P::step(m,in,l)==P::ActInsert,"hand back at the body -> insert");
     in.present=true;in.targetInBody=false;in.devM=.001f;in.now+=28*ms;P::step(m,in,l);CHECK(m.state==P::BodyLive,"live again");
     in.jump=true;in.now+=28*ms;CHECK(P::step(m,in,l)==P::ActRespawn&&m.state==P::BodyTravel,"eye jump -> respawn at the chest");
     in.jump=false;in.devM=.3f;in.now+=200*ms;P::step(m,in,l);CHECK(m.state==P::BodyTravel,"travel holds up to 300 ms");
     in.now+=150*ms;P::step(m,in,l);CHECK(m.state==P::BodyLive,"travel ends at 300 ms");
     in.want=false;in.now+=28*ms;CHECK(P::step(m,in,l)==P::ActGhost,"gate closed -> ghost");
     in.want=true;in.now+=28*ms;CHECK(P::step(m,in,l)==P::ActRespawn&&m.state==P::BodyTravel,"gate open again inside 1 s -> respawn");
     in.want=false;in.now+=28*ms;P::step(m,in,l);in.now+=1000*ms;CHECK(P::step(m,in,l)==P::ActRemove,"a long gate -> removed");
     CHECK(P::parkReturn({.05f,0,0},{0,0,0},{1,0,0},.15f,1.f)&&!P::parkReturn({-.02f,0,0},{0,0,0},{1,0,0},.15f,1.f)&&!P::parkReturn({.2f,0,0},{0,0,0},{1,0,0},.15f,1.f),
           "in-place return: within 15 cm, on the free side");}
    // ---------- gates, parts, doors, vtable ----------
    {CHECK(!P::partWant(.5f,false,false,true)&&P::partWant(.7f,false,false,true)&&P::partWant(.5f,true,false,true)&&!P::partWant(.4f,true,false,true),"fist: on over .65, off under .45");
     CHECK(!P::partWant(.9f,false,true,true)&&P::partWant(.1f,true,true,true),"the part kept while touching");CHECK(!P::partWant(.9f,false,false,false),"fist lever 0: open hand");}
    {CHECK(P::doorAction(0,false)==P::DoorDoOpen&&P::doorAction(1,false)==P::DoorLocked&&P::doorAction(2,false)==P::DoorBusy&&P::doorAction(3,false)==P::DoorBusy&&
           P::doorAction(4,false)==P::DoorOpened&&P::doorAction(4,true)==P::DoorDoClose&&P::doorAction(5,true)==P::DoorBusy,"door states 0..5");}
    {P::DoorGate g{};CHECK(g.allow(1,1000*ms)&&!g.allow(1,1500*ms)&&g.allow(2,1500*ms)&&g.allow(1,2001*ms),"one door open per second");}
    {P::WakeLru w{};w.note(1,1000*ms);CHECK(w.recent(1,1500*ms)&&!w.recent(1,2001*ms)&&!w.recent(2,1500*ms),"wake LRU: 1 s per prop");
     for(std::uintptr_t i=10;i<10+P::WakeLru::size;++i)w.note(i,1100*ms+i);CHECK(!w.recent(1,1500*ms)&&w.recent(10,1500*ms),"wake LRU: the oldest entry replaced");}
    {std::uintptr_t stock[P::instTableWords],out[P::instTableWords];for(std::size_t i=0;i<P::instTableWords;++i)stock[i]=0x1000+i;
     P::buildInstTable(stock,0xdead,out);bool same=true;for(std::size_t i=0;i<P::instTableWords;++i)if(i!=P::deactivateWord&&out[i]!=stock[i])same=false;
     CHECK(same&&out[P::deactivateWord]==0xdead&&out[1]==stock[1]&&P::deactivateWord==34,"own vtable: only slot +0x100 differs, the typeinfo kept");
     CHECK(!D::instNoDeactivate(nullptr,true),"PrepareForDeactivation of the own vtable: false");}
    {P::Levers l{};l.radiusM=5.f;l.lengthM=.01f;l.leashM=std::nanf("");l.shape=7;l.creepMps=-1.f;l.wakeMax=50;l.reachM=2.f;const auto s=P::sanitized(l);
     CHECK(near(s.radiusM,.08f)&&near(s.lengthM,.16f)&&near(s.leashM,.40f)&&s.shape==P::ShapePalm&&s.creepMps==0.f&&s.wakeMax==8&&near(s.reachM,.5f),"levers clamped, NaN -> default");}
    // ---------- reach sphere ----------
    {const auto s=P::reachSphere({0,1.3f,.5f},{0,0,2.f},{0,1.35f,0},.25f,.1f,1.f);
     CHECK(s.ok&&nearV(s.a,{0,1.3f,.5f})&&nearV(s.b,{0,1.3f,.7f})&&near(s.cullR,.35f)&&nearV(s.centre,{0,1.3f,.6f}),"reach: the palm to palm + vel * 100 ms, radius 25 cm");
     CHECK(P::reachGap(s,{0,1.3f,.95f},.04f)<0.f&&P::reachGap(s,{0,1.3f,1.0f},.04f)>0.f,"reach gap along the lead");
     const auto f=P::reachSphere({0,1.3f,1.5f},{0,0,0},{0,1.35f,0},.25f,.1f,1.f);CHECK(V::length(V::sub(f.a,{0,1.35f,0}))<=.9001f,"reach kept within 90 cm of the chest");
     const auto q=P::reachSphere({0,1.3f,.5f},{0,0,20.f},{0,1.35f,0},.25f,.1f,1.f);CHECK(near(V::length(V::sub(q.b,q.a)),.4f-.0f,.11f)&&V::length(V::sub(q.b,q.a))<=.4001f,"the lead capped at 40 cm");
     const auto st=P::reachSphere({0,1.3f,.5f},{0,0,0},{0,1.35f,0},.25f,.1f,1.f);
     CHECK(P::reachGap(st,{1.5f,1.3f,.5f},2.f)>0.f&&P::reachGap(st,{.6f,1.3f,.5f},2.f)<0.f,"a 2 m bounding sphere counts as 40 cm: 1.5 m off not in reach, 60 cm off in reach");}
    static_assert(P::handType(0x11003u,0)==0x810001u&&P::handType(0,0)==0x800001u&&P::handType(0x11003u,0x40u)==0x40u);
    static_assert(P::handInclude(0x1u,0)==0x801u&&P::handInclude(0x1u,0x5u)==0x5u);
    {P::PairIn p{};p.otherClass=0;CHECK(P::collide(p),"a prop collides");
     auto g=p;g.ghost=true;CHECK(!P::collide(g),"ghost: every pair dropped");
     auto pl=p;pl.player=true;CHECK(!P::collide(pl),"the player's mover dropped");
     auto mv=p;mv.otherClass=P::classMover;CHECK(!P::collide(mv),"NPC mover dropped by default");}

    // ---------- DOOR SWING (pure) ----------
    {const auto bits=[](float f){std::uint32_t u;std::memcpy(&u,&f,4);return u;};
     CHECK(bits(1.42f)==0x3fb5c28fu&&bits(-3.835f)==0xc07570a4u&&bits(3.409f)==0x405a2d0eu,"door ease: the engine's constants");
     CHECK(near(P::easeOpen(.8f),.9998f,2e-4f)&&P::easeOpen(0.f)==0.f&&near(P::smooth(.5f),.5f),"door ease: e(0.8) = 0.9998, smoothstep");
     bool inv=true,mono=true,invc=true;
     for(int i=0;i<=16;++i){const float t=.05f*float(i),e=P::easeOpen(t);
        if(!near(P::easeOpen(P::phaseFor(e,false)),e,1e-4f)||(t<=.6f&&!near(P::phaseFor(e,false),t,5e-4f)))inv=false;if(i&&e<=P::easeOpen(t-.05f))mono=false;}
     for(int i=0;i<=20;++i){const float t=.05f*float(i);if(!near(P::phaseFor(P::smooth(t),true),t,2e-4f))invc=false;}
     CHECK(inv&&mono&&invc,"phaseFor inverts the ease on [0, 0.8] and smoothstep on [0, 1]");
     CHECK(near(P::easeOpen(P::phaseFor(1.f,false)),P::easeOpen(.8f),1e-4f)&&P::phaseFor(1.f,false)<=.8f&&P::phaseFor(-1.f,false)<1e-5f,"phaseFor clamps to [0, e(0.8)]");}
    {const P::V3 mn{0,0,-.03f},mx{.9f,2.1f,.03f};
     const auto g=P::doorGeom({.8f,1.2f,-.007f},mn,mx,true,-1,1.f);
     CHECK(g.ok&&near(g.leverM,.8f,1e-3f)&&g.face==1,"door geometry: a palm at chest height 0.8 m from the hinge (1.44 m from its origin) is on the leaf");
     CHECK(P::doorGeom({.8f,1.2f,.007f},mn,mx,true,-1,1.f).face==-1&&P::doorGeom({.8f,1.2f,.007f},mn,mx,true,1,1.f).face==1,"door geometry: the face from the side and the open direction");
     CHECK(P::doorGeom({1.3f,1.2f,0},mn,mx,true,1,1.f).reject==P::DoorGeomBox&&P::doorGeom({.5f,1.2f,.5f},mn,mx,true,1,1.f).reject==P::DoorGeomBox&&
           P::doorGeom({.5f,2.5f,0},mn,mx,true,1,1.f).reject==P::DoorGeomHeight,"door geometry: past the leaf's end, 50 cm off it, above it: rejected");
     CHECK(P::doorGeom({.8f,1.2f,0},{},{},false,1,1.f).ok&&P::doorGeom({1.8f,1.2f,0},{},{},false,1,1.f).reject==P::DoorGeomLever&&
           P::doorGeom({.05f,1.2f,0},{},{},false,1,1.f).reject==P::DoorGeomLever,"door geometry without a box: lever 0.10..1.60 m");
     CHECK(near(P::doorGeom({.05f,1.f,.05f},mn,mx,true,1,1.f).leverM,.15f),"door geometry: the lever at least 15 cm");}
    {const P::Levers L{};const std::uint64_t t0=50'000*ms;const float dt=.028f;float tt=0.f;
     P::DoorSwing w{};w.startNs=t0;
     auto c=P::doorSwingStep(w,true,.02f,.8f,1,1.6f,dt,t0+28*ms,false,L,tt);
     CHECK(c==P::DoorCmdHold&&near(w.theta,std::atan2(.02f,.8f),1e-5f)&&near(P::easeOpen(tt)*1.6f,w.theta,2e-4f),"swing: a push turns the leaf to the palm, t from the ease");
     P::DoorSwing q{};q.startNs=t0;P::doorSwingStep(q,true,.5f,.8f,1,1.6f,dt,t0+28*ms,false,L,tt);
     CHECK(near(q.theta,6.f*dt,1e-5f),"swing: a deep push capped at 6 rad/s");
     P::DoorSwing f{};f.startNs=t0;std::uint64_t n=t0;for(int i=0;i<3;++i){n+=28*ms;P::doorSwingStep(f,true,.04f,.8f,1,1.6f,dt,n,false,L,tt);}
     const float th0=f.theta,om0=f.omega;int steps=0;for(;steps<200&&f.omega!=0.f;++steps){n+=28*ms;P::doorSwingStep(f,false,0.f,.8f,1,1.6f,dt,n,false,L,tt);}
     CHECK(om0>1.f&&f.theta>th0+.2f&&f.omega==0.f&&steps<200,"swing: a flick coasts on and stops");
     P::DoorSwing r{};r.startNs=t0;r.theta=.005f;c=P::doorSwingStep(r,true,.02f,.8f,-1,1.6f,dt,t0+28*ms,false,L,tt);
     CHECK(c==P::DoorCmdFastClose&&r.reversed,"swing: pushed shut from the far side: FastClose");
     P::DoorSwing lo{};lo.startNs=t0;lo.lastPushNs=t0;lo.theta=.3f;c=P::doorSwingStep(lo,false,0.f,.8f,1,1.6f,dt,t0+450*ms,false,L,tt);
     CHECK(c==P::DoorCmdHold&&lo.glide,"swing: idle 0.45 s under a third open: glides");
     n=t0+450*ms;int k=0;for(;k<40&&c==P::DoorCmdHold;++k){n+=28*ms;c=P::doorSwingStep(lo,false,0.f,.8f,1,1.6f,dt,n,false,L,tt);}
     CHECK(c==P::DoorCmdFastClose&&k>=6&&k<=12,"swing: the glide ends shut (FastClose)");
     P::DoorSwing hi{};hi.startNs=t0;hi.lastPushNs=t0;hi.theta=1.f;c=P::doorSwingStep(hi,false,0.f,.8f,1,1.6f,dt,t0+450*ms,false,L,tt);
     CHECK(c==P::DoorCmdFinish,"swing: idle past a third open: back to the engine (Finish)");
     P::DoorSwing hold{};hold.startNs=t0;hold.lastPushNs=t0;hold.theta=1.f;c=P::doorSwingStep(hold,false,0.f,.8f,1,1.6f,dt,t0+300*ms,false,L,tt);
     CHECK(c==P::DoorCmdHold,"swing: idle 0.3 s: still held");
     P::DoorSwing cl{};cl.startNs=t0;cl.lastPushNs=t0;cl.closing=true;cl.theta=.1f;c=P::doorSwingStep(cl,false,0.f,.8f,1,1.6f,dt,t0+450*ms,false,L,tt);
     CHECK(c==P::DoorCmdFinish&&near(P::smooth(tt)*1.6f,cl.theta,2e-4f),"swing: a closing hold released goes back to the engine; t from smoothstep");
     P::DoorSwing far{};far.startNs=t0;far.lastPushNs=t0+28*ms;far.theta=1.f;c=P::doorSwingStep(far,true,.01f,.8f,1,1.6f,dt,t0+56*ms,true,L,tt);
     CHECK(c==P::DoorCmdFinish,"swing: the player 3 m away releases the door");
     P::DoorSwing old{};old.startNs=t0;old.theta=1.f;c=P::doorSwingStep(old,true,.001f,.8f,1,1.6f,dt,t0+20'100*ms,false,L,tt);
     CHECK(c==P::DoorCmdFinish,"swing: a hold over 20 s is released");
     P::DoorSwing top{};top.startNs=t0;top.theta=1.59f;P::doorSwingStep(top,true,.3f,.8f,1,1.6f,dt,t0+28*ms,false,L,tt);
     CHECK(top.theta==1.6f&&top.omega==0.f&&tt<=.8f,"swing: stops at the cap, t at most 0.8");}
    // ---------- DOORS: the leaf follow (pure) ----------
    {const P::Rows closed{{{0,0,1},{0,1,0},{1,0,0}}};const P::V3 o{1,0,-.8f},mn{0,0,-.03f},mx{.9f,2.1f,.03f};
     const auto f=P::leafFrame(closed,closed,o,mn,mx,true,{0,1,0},1.f);
     CHECK(f.sure&&f.upright&&nearV(f.L,{0,0,1})&&nearV(f.L0,{0,0,1})&&near(f.width,.9f)&&near(f.halfZ,.03f)&&near(f.yMax,2.1f),"leaf: the frame of a closed door (L along +a, reach 0.9 m)");
     CHECK(!P::leafFrame(closed,closed,o,{-.02f,0,-.03f},{.02f,2.1f,.03f},true,{0,1,0},1.f).sure,"leaf: a box centred on the hinge is not sure");
     CHECK(!P::leafFrame(closed,closed,o,mn,mx,false,{0,1,0},1.f).sure&&!P::leafFrame(closed,closed,o,mn,mx,true,{1,0,0},1.f).sure,"leaf: no box, a hinge that is not upright: not sure");
     const P::Rows flip{{{0,0,-1},{0,1,0},{-1,0,0}}};const auto fs=P::leafFrame(flip,flip,o,{-.9f,0,-.03f},{0,2.1f,.03f},true,{0,1,0},1.f);
     CHECK(fs.sure&&fs.sx<0.f&&nearV(fs.L,{0,0,1}),"leaf: a box on the -x side: L = -a");
     const P::V3 kPush=P::openAxisPush(f.L0,f.c0,V::sub({.993f,1.2f,0},o));
     CHECK(nearV(kPush,{0,1,0}),"open axis from the push: the leaf swings away from the pusher");
     CHECK(nearV(P::openAxisPush(f.L0,f.c0,V::sub({1.01f,1.2f,0},o)),{0,-1,0}),"open axis from the push: the other side, the other way");
     const P::V3 L40=V::rotate(V::mul(kPush,.7f),f.L0);
     CHECK(nearV(P::openAxisMeasured(f.L0,L40),{0,1,0},1e-4f),"open axis measured from the turned leaf");
     const auto on=P::leafPalm(f,kPush,{.993f,1.2f,0},1.f);
     CHECK(on.on&&near(on.r,.8f,1e-3f)&&on.delta<0.f&&near(on.delta,std::atan2(-.007f,.8f),1e-4f),"leaf palm: on the leaf, the closed side (delta < 0)");
     CHECK(!P::leafPalm(f,kPush,{.993f,1.2f,.4f},1.f).on,"leaf palm: past the leaf's end (the frame or the wall beside it): off");
     CHECK(!P::leafPalm(f,kPush,{.993f,2.4f,0},1.f).on&&!P::leafPalm(f,kPush,{.993f,1.2f,-.75f},1.f).on,"leaf palm: above the leaf, at the hinge: off");
     CHECK(!P::leafPalm(f,kPush,{1.2f,1.2f,0},1.f).on,"leaf palm: 20 cm off the leaf's face: off");
     const float cl=P::leafClearance(.03f,.8f,1.f);CHECK(near(cl,std::atan(.044f/.8f),1e-5f),"leaf clearance: half thickness + 1.4 cm");
     CHECK(near(P::doorTarget(-1,0.f,.025f,cl,1.6f,6.f,.028f),.025f+cl,1e-5f),"door target: a push open turns the leaf ahead of the command by the clearance");
     CHECK(near(P::doorTarget(-1,.5f,-.2f,cl,1.6f,6.f,.028f),.5f),"door target: pulled back from the closed side: the leaf stays");
     CHECK(near(P::doorTarget(1,.5f,-.025f,cl,1.6f,6.f,.028f),.5f-.025f-cl,1e-5f)&&near(P::doorTarget(1,.5f,.3f,cl,1.6f,6.f,.028f),.5f),
           "door target: from the open side only shut, the clearance kept");
     CHECK(near(P::doorTarget(-1,.5f,1.f,cl,1.6f,6.f,.028f),.5f+6.f*.028f,1e-5f)&&near(P::doorTarget(-1,1.55f,1.f,cl,1.6f,6.f,.1f),.9998f*1.6f,1e-4f)&&
           P::doorTarget(1,.01f,-1.f,cl,1.6f,60.f,.1f)==0.f,"door target: the rate cap, clamped to [0, 0.9998 cap]");
     const float tl=.3f,rm=P::restRemapPhase(tl);
     CHECK(near(P::smooth(rm),P::easeOpen(tl),1e-4f)&&rm>tl,"rested remap: the closing phase of the same angle (no jump: ease 0.3 = 72 %, smoothstep 0.3 = 22 %)");
     // the raise's sweep: hinge at the origin, closed leaf along +z, opening about +y (towards +x), 0.9 m wide, rested at 30, raised to 45
     {const P::V3 h0{0,0,0},upY{0,1,0},L0{0,0,1},k{0,1,0};const float th=30.f*P::pi/180.f,dl=15.f*P::pi/180.f;
      const auto at=[](float deg,float r){const float a=deg*P::pi/180.f;return P::V3{r*std::sin(a),1.f,r*std::cos(a)};};
      CHECK(nearV(P::leafAt(L0,k,th),{.5f,0,std::cos(th)},1e-5f),"raise: the rested leaf's direction from the closed leaf and the open axis");
      CHECK(P::raiseClear(at(-40.f,.8f),h0,upY,L0,k,th,.9f,dl,1.f)&&P::raiseClear(at(-150.f,.6f),h0,upY,L0,k,th,.9f,dl,1.f),
            "raise: a mover behind the leaf (the side it opens away from) or across the doorway: clear");
      CHECK(!P::raiseClear(at(38.f,.7f),h0,upY,L0,k,th,.9f,dl,1.f)&&!P::raiseClear(at(70.f,.8f),h0,upY,L0,k,th,.9f,dl,1.f)&&
            !P::raiseClear(at(31.f,.5f),h0,upY,L0,k,th,.9f,dl,1.f),"raise: a mover in the sweep or touching its open face: blocked");
      CHECK(P::raiseClear(at(120.f,.8f),h0,upY,L0,k,th,.9f,dl,1.f)&&P::raiseClear(at(38.f,1.3f),h0,upY,L0,k,th,.9f,dl,1.f),
            "raise: a mover past the sweep or beyond the leaf's reach: clear");
      CHECK(!P::raiseClear(at(-90.f,.2f),h0,upY,L0,k,th,.9f,dl,1.f)&&!P::raiseClear(at(-40.f,.8f),h0,upY,L0,{1,0,0},th,.9f,dl,1.f),
            "raise: a mover at the hinge, or an open axis that is not the hinge: blocked");
      CHECK(!P::raiseClear(at(-38.f,.7f),h0,upY,L0,{0,-1,0},th,.9f,dl,1.f)&&P::raiseClear(at(40.f,.8f),h0,upY,L0,{0,-1,0},th,.9f,dl,1.f),
            "raise: a door opening the other way (axis -y): the sweep and the clear side mirrored");
      // a closing push: stopped where the leaf meets a mover in its closing wedge, never by one on the face it moves away from
      const float sh=2.f*P::pi/180.f,t90=90.f*P::pi/180.f;
      const auto ma=[&](float deg,float r){return P::moverAngle(at(deg,r),h0,upY,L0,k,.9f,1.f);};
      CHECK(near(P::closeStop(ma(15.f,.6f),t90,sh),(15.f+35.685f)*P::pi/180.f,2e-3f),"close stop: a mover in the doorway wedge stops the leaf at psi + asin(rho / r)");
      CHECK(P::closeStop(ma(120.f,.7f),t90,sh)==0.f&&P::closeStop(ma(-60.f,.7f),t90,sh)==0.f&&P::closeStop(ma(20.f,1.4f),t90,sh)==0.f,
            "close stop: a mover on the open face, behind the wall or past the reach: nothing stops the shut");
      CHECK(near(P::closeStop(ma(-10.f,.7f),t90,sh),(-10.f+30.f)*P::pi/180.f,2e-3f),
            "close stop: a mover across the frame line (-10 deg): the leaf never shuts on him");
      CHECK(P::closeStop(ma(0.f,.2f),t90,sh)==t90&&P::closeStop(P::MoverAngle{},t90,sh)==t90,"close stop: a mover at the hinge or no geometry: no closing at all");
      CHECK(near(P::closeStop(ma(5.f,.9f),.3f,sh),.3f,1e-6f),"close stop: never above the leaf's own angle");
      CHECK(near(P::openStop(ma(60.f,.7f),.3f),(60.f-30.f)*P::pi/180.f,2e-3f)&&std::isinf(P::openStop(ma(-60.f,.7f),.3f))&&std::isinf(P::openStop(ma(60.f,1.4f),.3f)),
            "open stop: a mover in the opening wedge stops the leaf before him; behind it or past the reach: no stop");
      CHECK(P::sweepMeets(ma(30.f,.6f),.2f,.8f)&&!P::sweepMeets(ma(30.f,.6f),.8f,1.2f)&&P::sweepMeets(P::MoverAngle{},.1f,.2f),"sweep: met in its wedge, not past it; no geometry: met");}}
    // ---------- DOORS: the player's approach and the mover's push (pure) ----------
    {CHECK(P::findDoorGate(true,false,true,true,true,0)==P::FindSuppress,"door gate: the automatic walk-in open of a door found on foot while live: suppressed");
     CHECK(P::findDoorGate(false,false,true,true,true,0)==P::FindNotFound&&P::findDoorGate(true,true,true,true,true,0)==P::FindLever&&
           P::findDoorGate(true,false,false,true,true,0)==P::FindNotLive&&P::findDoorGate(true,false,true,false,true,0)==P::FindNotFoot&&
           P::findDoorGate(true,false,true,true,false,0)==P::FindNoMode&&P::findDoorGate(true,false,true,true,true,3)==P::FindPrompt,
           "door gate: passed through without a door, lever doorauto 1, not live, on horseback, no mode word, the prompt mode");
     // a leaf closed along +z from the hinge at the origin (width 0.9 m), opening about -y; the mover walks along -x toward it on its closed side
     const P::V3 hinge{0,0,0},up{0,1,0},L0{0,0,1};const P::V3 k=P::openAxisPush(L0,{1,0,0},{.5f,0,.5f});
     CHECK(nearV(k,{0,-1,0},1e-4f),"body push: the leaf swings away from the mover");
     const auto at=[&](float x,float z){return P::moverAngle({x,0,z},hinge,up,L0,k,.9f,1.f);};
     const float hz=.028f,rho=P::doorMoverRadiusM; // a logged door box's leaf half thickness (bmin.z -0.028), the assumed capsule
     {const auto b0=P::bodyDoorTarget(at(.6f,.5f),0.f,1.6f,6.f,.028f,2.f*P::pi/180.f,1.f,hz);
      CHECK(!b0.inBand&&b0.target==0.f&&near(b0.gapM,.6f-rho-hz,1e-4f),"body push: the capsule 22 cm from the leaf face: no push, its gap");
      // the capsule held at the leaf's face (its centre at rho + halfZ from the centre plane): the leaf opens ahead of him
      const auto mFace=at(rho+hz,.5f);const auto bFace=P::bodyDoorTarget(mFace,0.f,1.6f,6.f,.028f,2.f*P::pi/180.f,1.f,hz);
      CHECK(bFace.inBand&&bFace.side==-1&&near(bFace.gapM,0.f,1e-4f)&&bFace.target>.5f*P::pi/180.f&&bFace.target<=6.f*.028f+1e-5f,
            "body push: a capsule the leaf's face stops (gap 0): opened ahead of him (over the 0.5 deg step), rate-capped");
      const auto mFar=at(rho+hz+.06f,.5f);
      CHECK(!P::bodyDoorTarget(mFar,0.f,1.6f,6.f,.028f,2.f*P::pi/180.f,1.f,hz).inBand,"body push: 6 cm from the face: past the 5 cm lead band");
      const auto m1=at(.37f,.5f);const auto b2=P::bodyDoorTarget(m1,.5f,1.6f,6.f,.028f,2.f*P::pi/180.f,1.f,hz);
      CHECK(!b2.inBand&&b2.target==.5f,"body push: a leaf already past him: nothing");
      const auto m3=at(.30f,.5f);const float want=m3.psi+std::asin((rho+hz+P::doorBodyClearM)/m3.r);
      const auto b3=P::bodyDoorTarget(m3,want-.01f,1.6f,6.f,.028f,2.f*P::pi/180.f,1.f,hz);
      CHECK(b3.inBand&&near(b3.target,want,1e-4f),"body push: the leaf's face keeps 3 cm from his capsule");
      const auto b4=P::bodyDoorTarget(at(.05f,.1f),0.f,1.6f,6.f,.028f,0.f,1.f,hz);
      CHECK(!b4.inBand,"body push: at the hinge: no push (the stuck fallback)");
      // from the open side: an open leaf at 60 deg pushed back, never below the shut angle + 1 deg
      const float th=60.f*P::pi/180.f;const P::V3 leafDir=P::leafAt(L0,k,th);const P::V3 n{leafDir[2],0,-leafDir[0]};
      const P::V3 p=V::add(V::mul(leafDir,.5f),V::mul(n,-.37f));const auto m5=P::moverAngle(p,hinge,up,L0,k,.9f,1.f);
      const auto b5=P::bodyDoorTarget(m5,th,1.6f,6.f,.028f,2.f*P::pi/180.f,1.f,hz);
      CHECK(m5.psi>th&&b5.inBand&&b5.side==1&&!b5.atFloor&&b5.target<th&&b5.target>=th-6.f*.028f-1e-5f,"body push: from the open face: pushed back toward closed, rate-capped");
      const float th6=4.f*P::pi/180.f;const P::V3 l6=P::leafAt(L0,k,th6);const P::V3 n6{l6[2],0,-l6[0]};
      const auto m6=P::moverAngle(V::add(V::mul(l6,.5f),V::mul(n6,-.36f)),hinge,up,L0,k,.9f,1.f);
      const auto b6=P::bodyDoorTarget(m6,th6,1.6f,60.f,.028f,2.f*P::pi/180.f,1.f,hz);
      CHECK(b6.inBand&&b6.side==1&&b6.atFloor&&near(b6.target,3.f*P::pi/180.f,1e-5f),"body push: pushed back, never below the shut angle + 1 deg (held at the floor)");
      CHECK(P::bodyGapBucket(-.01f)==0&&P::bodyGapBucket(.01f)==1&&P::bodyGapBucket(.07f)==3&&P::bodyGapBucket(.2f)==5&&P::bodyGapBucket(1.f)==6,"body push: the gap buckets");}
     P::StuckTimer st{};const std::uint64_t t0=900'000*ms;
     CHECK(!P::stuckStep(st,true,t0,1500.f)&&!P::stuckStep(st,true,t0+1000*ms/10*14,1500.f),"stuck: under 1.5 s at a door: nothing");
     {P::StuckTimer s2{};bool open=false;for(int i=0;i<60&&!open;++i)open=P::stuckStep(s2,true,t0+std::uint64_t(i)*28*ms,1500.f);
      CHECK(open,"stuck: 1.5 s at the same door: the stock Open");
      P::StuckTimer s3{};bool o3=false;for(int i=0;i<60;++i)o3=P::stuckStep(s3,i%10!=0,t0+std::uint64_t(i)*28*ms,1500.f)||o3;
      CHECK(!o3,"stuck: leaving the door every 10th tick restarts it");
      // a double door whose leaves the hook keeps by turns: each leaf's own timer
      P::StuckTimers ts{};bool o4=false;for(int i=0;i<60&&!o4;++i)o4=P::stuckStep(ts.of(i&1?0x10:0x20),true,t0+std::uint64_t(i)*28*ms,1500.f);
      CHECK(o4,"stuck: alternating leaves of a double door: each leaf's timer runs");
      P::StuckTimers t5{};t5.of(0x10);t5.of(0x20);auto& t30=t5.of(0x30);CHECK(t30.door==0x30&&!t30.sinceNs,"stuck: a third leaf takes the stalest slot");}}
    // ---------- GLASS (pure) ----------
    {CHECK(P::glassBin(.2f)==0&&P::glassBin(.7f)==1&&P::glassBin(1.9f)==3&&P::glassBin(2.5f)==4&&P::glassBin(5.f)==6&&P::glassBin(9.f)==7,"glass: the approach bins");
     P::GlassRate r{};const std::uint64_t t0=800'000*ms;
     CHECK(r.allow(0x40,t0)&&!r.allow(0x40,t0+300*ms)&&r.allow(0x40,t0+600*ms)&&!r.allow(0x40,t0+900*ms)&&r.allow(0x40,t0+1200*ms)&&r.allow(0x80,t0+300*ms),
           "glass rate: 500 ms per pane, at most 2 per second, other panes free");
     P::GlassTracks g{};
     CHECK(g.edge(0x40,1,100,t0,.1f,2.f)&&g.edge(0x40,1,101,t0+7*ms,.1f,2.f)&&g.edge(0x40,1,102,t0+14*ms,.1f,2.f)&&!g.edge(0x40,1,103,t0+21*ms,.1f,2.f),"glass edge: the first 3 substeps");
     CHECK(g.edge(0x40,1,200,t0+400*ms,.1f,2.f)&&g.edge(0x80,1,201,t0+407*ms,.1f,2.f),"glass edge: a new contact after 150 ms, another pane");
     // a hand resting on the frame (component 0) then punching the pane (component 2) of the same window: the pane's own edge
     CHECK(g.edge(0x90,0,300,t0+600*ms,.1f,2.f)&&!g.edge(0x90,0,304,t0+628*ms,.1f,2.f)&&g.edge(0x90,2,305,t0+635*ms,3.f,2.f),"glass edge: per component (the frame touched, the pane hit)");
     // a hand resting on a pane then hitting it: the approach rising past glassmps from under half of it
     CHECK(!g.edge(0x90,2,309,t0+663*ms,.2f,2.f)&&g.edge(0x90,2,310,t0+670*ms,3.f,2.f)&&!g.edge(0x90,2,311,t0+677*ms,3.f,2.f),"glass edge: a rise to 3 m/s after a quiet touch");
     CHECK(P::fragDamageDelayed(1u<<21|1u<<20,true,0)&&!P::fragDamageDelayed(1u<<21,true,-1)&&P::fragDamageDelayed(0,true,-1)&&!P::fragDamageDelayed(0,true,5)&&
           !P::fragDamageDelayed(0,false,-1),"glass: IsFragDamageDelayed (the +0x80 override bit 21 -> bit 20, else the type's +0x1ba sign)");}
    // ---------- UNFREEZE: size gates, mass, kick (pure) ----------
    {P::UnfreezeIn u{};u.flags12=8;u.typeOk=true;u.minMove=0.f;u.gohOk=true;u.gohF0=1;u.composite=true;u.compD4=0;u.partCount=1;u.partsActive=true;u.radiusM=.075f;
     u.massKg=10.f;u.live=true;u.age=0;u.depthM=.006f;
     CHECK(P::unfreezeWanted(u,.20f,50.f,.005f,4)==P::UnfreezeOk,"unfreeze: the 7.5 cm cup whose type reads 10 kg: made physical (size decides)");
     auto big=u;big.radiusM=.25f;auto parts=u;parts.partCount=6;auto cr=u;cr.compRadiusM=.3f;auto heavy=u;heavy.massKg=80.f;
     CHECK(P::unfreezeWanted(big,.20f,50.f,.005f,4)==P::UnfreezeTooBig&&P::unfreezeWanted(parts,.20f,50.f,.005f,4)==P::UnfreezeTooBig&&
           P::unfreezeWanted(cr,.20f,50.f,.005f,4)==P::UnfreezeTooBig&&P::unfreezeWanted(heavy,.20f,50.f,.005f,4)==P::UnfreezeHeavy,
           "unfreeze: too big (level sphere, parts, composite radius), data mass over the 50 kg cap");
     auto old=u;old.age=4;auto older=u;older.age=5;auto flick=u;flick.depthM=.0005f;flick.approach=.5f;auto touch=u;touch.depthM=.0005f;auto dead=u;dead.live=false;
     CHECK(P::unfreezeShallowWhy(old)==0&&P::unfreezeShallowWhy(older)==2&&P::unfreezeShallowWhy(flick)==0&&P::unfreezeShallowWhy(touch)==3&&P::unfreezeShallowWhy(dead)==1,
           "unfreeze press: 4 substeps old, a 0.5 m/s flick, a 0.5 mm touch, not live");
     CHECK(near(P::unfreezeMassKg(.075f,.35f,10.f),.35f)&&near(P::unfreezeMassKg(.03f,.35f,10.f),.15f)&&near(P::unfreezeMassKg(.2f,.35f,10.f),2.5f)&&
           near(P::unfreezeMassKg(.075f,.35f,.2f),.2f)&&near(P::unfreezeMassKg(.075f,0.f,10.f),10.f),"unfreeze mass: 0.35 kg at 7.5 cm, by volume in [0.15, 2.5], never above the collider's, 0 = kept");
     const auto k=P::unfreezeKickOf({0,0,0},{-.2f,0,0},{-.05f,.05f,0},{-1,0,0},2.f,2.f,.35f,1.f);
     CHECK(k.mps>0.f&&near(k.mps,std::min(2.f*1.3f*2.f/2.35f,3.f),1e-4f)&&k.vel[0]>0.f&&V::length(k.spin)>0.f&&V::length(k.spin)<=8.f+1e-4f,
           "unfreeze kick: away from the palm, the momentum share, a spin from a hit above the centre");
     CHECK(P::unfreezeKickOf({0,0,0},{-.2f,0,0},{0,0,0},{-1,0,0},0.f,2.f,.35f,1.f).mps==0.f,"unfreeze kick: no approach, no kick");}
    // ---------- GRAB: the grip drive of a ball-jointed hold (pure) ----------
    {const P::V3 up{0,1,0};
     // linear: a 1 cm error, no velocity error: M wn^2 e + the carried weight
     const auto g1=P::grabGripForce({.01f,0,0},{0,0,0},{0,0,0},{0,0,0},20.f,up,8.f,1.f,900.f,20.f,25.f,.5f,{0,0,0},1e9f,1.f,1.f);
     CHECK(near(g1.f[0],20.f*64.f*.01f,1e-3f)&&near(g1.f[1],20.f*9.81f,1e-2f)&&!g1.capped&&g1.vert==0,"grip drive: M wn^2 e across, the carried weight up");
     // damping on the grip's own velocity
     const auto g2=P::grabGripForce({0,0,0},{0,0,0},{0,0,0},{.5f,0,0},20.f,up,8.f,1.f,900.f,20.f,0.f,.5f,{0,0,0},1e9f,1.f,1.f);
     CHECK(near(g2.f[0],-20.f*2.f*8.f*.5f,1e-2f),"grip drive: 2 zeta wn M against the grip's velocity");
     // caps: across min(forceMax, M accel), down at downG M g, up at the carried weight's (g + accel); the slew per substep
     const auto g3=P::grabGripForce({1.f,-1.f,0},{0,0,0},{0,0,0},{0,0,0},59.5f,up,8.f,1.f,900.f,20.f,25.f,.5f,{0,0,0},1e9f,1.f,1.f);
     CHECK(near(g3.f[0],900.f,1e-2f)&&near(g3.f[1],-.5f*59.5f*9.81f,1e-2f)&&g3.vert==2&&g3.capped,"grip drive: the chair's caps: 900 N across, half its weight down");
     const auto g4=P::grabGripForce({0,1.f,0},{0,0,0},{0,0,0},{0,0,0},59.5f,up,8.f,1.f,900.f,20.f,25.f,.5f,{0,0,0},1e9f,1.f,1.f);
     CHECK(near(g4.f[1],.5f*59.5f*9.81f,1e-2f)&&g4.vert==1&&g4.f[1]<59.5f*9.81f,"grip drive: a 59.5 kg chair: up at half its weight (it tilts, never lifts)");
     const auto g4b=P::grabGripForce({0,1.f,0},{0,0,0},{0,0,0},{0,0,0},20.f,up,8.f,1.f,900.f,20.f,25.f,.5f,{0,0,0},1e9f,1.f,1.f);
     CHECK(near(g4b.f[1],20.f*(9.81f+20.f),1e-2f)&&g4b.vert==1,"grip drive: a 20 kg prop (under liftKg): up at its weight plus the acceleration (it lifts)");
     {bool lifts=false;for(float M=25.5f;M<400.f;M+=.5f)lifts=lifts||!(P::gripUpMaxN(M,25.f,20.f)<M*9.81f);
      CHECK(!lifts&&near(P::gripUpMaxN(30.f,25.f,20.f),25.f*9.81f,1e-3f)&&near(P::gripUpMaxN(200.f,25.f,20.f),25.f*29.81f,1e-2f),
            "grip drive: every prop over liftKg: the up cap under its weight (25 kg's weight at least, 25 kg's g + accel at most)");}
     const auto g5=P::grabGripForce({1.f,0,0},{0,0,0},{0,0,0},{0,0,0},59.5f,up,8.f,1.f,900.f,20.f,0.f,.5f,{100.f,0,0},40.f,1.f,1.f);
     CHECK(near(g5.fN[0],140.f,1e-2f)&&g5.slewed,"grip drive: its change per substep capped at grabslew");
     const auto g6=P::grabGripForce({.01f,0,0},{0,0,0},{0,0,0},{0,0,0},20.f,up,8.f,1.f,900.f,20.f,0.f,.5f,{0,0,0},1e9f,.5f,1.f);
     CHECK(near(g6.f[0],.5f*20.f*64.f*.01f,1e-3f),"grip drive: the stall scale");
     P::GripStall st{};float sc=1.f;for(int i=0;i<144;++i)sc=P::gripStallStep(st,.3f,0.f,1.f/144.f);
     CHECK(sc<.85f&&sc>=.5f,"grip stall: 30 cm off and not closing for a second: the force ramps down");
     for(int i=0;i<144;++i)sc=P::gripStallStep(st,.3f,.2f,1.f/144.f);CHECK(near(sc,1.f),"grip stall: closing again: back to full");
     // the swing mode of a prop held at its edge (effective mass at the grip M / 5): the velocity law on the centre of mass (the centre
     // does not move in this mode) keeps it going every 3-4 substeps; the grip drive damps it without a reversal
     const float M=59.5f,me=M/5.f,dt=1.f/144.f;
     float x=.02f,vx=0.f;int flipsOld=0;float ampOld=0.f;
     for(int k=0;k<40;++k){const float vWant=std::clamp(-x/dt,-6.f,6.f);const float F=std::clamp(.5f*M*(vWant-0.f)/dt,-900.f,900.f);const float x0=x;vx+=F/me*dt;x+=vx*dt;
        if(x0*x<0.f)++flipsOld;if(k>=20)ampOld=std::max(ampOld,std::fabs(x));}
     float y=.02f,vy=0.f;int flipsPd=0;bool mono=true;float prev=std::fabs(y);
     for(int k=0;k<40;++k){const auto gf=P::grabGripForce({0,0,0},{y,0,0},{0,0,0},{vy,0,0},M,{0,1,0},8.f,1.f,900.f,20.f,0.f,.5f,{0,0,0},1e9f,1.f,1.f);
        const float y0=y;vy+=gf.fN[0]/me*dt;y+=vy*dt;if(y0*y<0.f)++flipsPd;mono=mono&&std::fabs(y)<=prev+1e-6f;prev=std::fabs(y);}
     std::printf("     grip swing model: old law crossings %d, amplitude after 20 substeps %.2f mm; grip drive crossings %d, end %.2f mm\n",flipsOld,ampOld*1000.f,flipsPd,std::fabs(y)*1000.f);
     CHECK(flipsOld>=3&&ampOld>.01f,"grip drive: the old law's swing mode at M / 5 (saturated, undamped at the grip): it crosses over and over, its amplitude kept");
     CHECK(flipsPd==0&&mono&&std::fabs(y)<.01f,"grip drive: the grip drive at M / 5: no crossing, the swing decays");
     // the drawn hand at the grip: the leash, no rotation
     P::VisualMem vm{};vm.h=.3f;const auto vo=P::grabVisualGrip(vm,{.5f,0,0},{0,0,0},.25f);
     CHECK(nearV(vo.offset,{.25f,0,0})&&V::length(vo.rot)==0.f&&vm.h==0.f,"grab visual at the grip: within the leash, the controller's rotation");
     CHECK(nearV(P::gripFollowVel({1,0,0},{.01f,0,0},{0,0,0},.01f),{1.2f,0,0},1e-5f),"grip follow: the grip's velocity plus a fifth of the gap per substep");
     // ---------- TWO HANDS (pure) ----------
     {const P::V3 gA{-.2f,1.f,0},gB{.2f,1.f,0};
      CHECK(near(P::pairShare({0,.8f,0},gA,gB),.5f)&&near(P::pairShare({-.1f,.8f,0},gA,gB),.75f)&&near(P::pairShare({-.5f,.8f,0},gA,gB),.85f)&&
            near(P::pairShare({0,0,0},gA,gA),.5f),"pair share: the centre of mass along the grip line, kept in .15 .. .85, .5 without a line");
      const auto L0=P::pairLimits(59.5f,.5f,70.f,900.f,20.f,.5f),L1=P::pairLimits(59.5f,.5f,70.f,900.f,20.f,.5f);
      CHECK(near(L0.springKg,29.75f)&&near(L0.upMaxN,29.75f*29.81f,1e-2f)&&L0.upMaxN<=900.f&&near(L0.sideMaxN,595.f,1e-2f)&&
            L0.upMaxN+L1.upMaxN>2.f*59.5f*9.81f,"pair limits: the chair: each hand lifts 887 N, both twice its weight (it swings up), across 595 N each");
      const auto pl=P::pairTargets({-.3f,1.f,0},{.3f,1.f,0},{0,0,1},{0,0,-1},gA,gB,1.f);
      CHECK(pl.aimed&&near(V::length(V::sub(pl.target[1],pl.target[0])),.4f,1e-5f)&&nearV(V::mul(V::add(pl.target[0],pl.target[1]),.5f),{0,1.f,0},1e-5f)&&
            near(pl.stretch,.2f,1e-5f)&&nearV(pl.vTarget[0],{0,0,1},1e-5f),"pair targets: the grip spacing along the hands' line, centred, no stretch; a turn kept");
      const auto pr=P::pairTargets({0,1.f,-.3f},{0,1.f,.3f},{0,0,0},{0,0,0},gA,gB,1.f);
      CHECK(near(V::dot(V::normalized(V::sub(pr.target[1],pr.target[0])),{0,0,1}),1.f,1e-5f),"pair targets: the hands turned 90 degrees: the grip line turns with them");
      const auto pa=P::pairTargets({-.3f,1.f,0},{.3f,1.f,0},{-1,0,0},{1,0,0},gA,gB,1.f);
      CHECK(nearV(pa.vTarget[0],{0,0,0},1e-5f)&&nearV(pa.vTarget[1],{0,0,0},1e-5f),"pair targets: hands pulled apart: no stretch rate in the targets");
      const auto ps=P::pairTargets({0,1.f,0},{.05f,1.f,0},{0,0,0},{0,0,0},{0,1.f,0},{.05f,1.f,0},1.f);
      CHECK(!ps.aimed&&nearV(ps.target[0],{0,1.f,0},1e-5f)&&nearV(ps.target[1],{.05f,1.f,0},1e-5f),"pair targets: grips under 10 cm apart: their offsets about the middle (no aim)");
      // a pure translation, no caps: the pair's total force = the one-hand law over the whole mass
      const P::V3 up{0,1,0};
      const auto one=P::grabGripForce({.02f,1.01f,0},{0,1.f,0},{0,0,0},{0,0,0},20.f,up,8.f,1.f,1e6f,1e3f,20.f,.5f,{0,0,0},1e9f,1.f,1.f);
      const auto Lh=P::pairLimits(20.f,.5f,20.f,1e6f,1e3f,.5f);
      const auto pa0=P::grabGripForcePair({.02f,1.01f,0},{0,1.f,0},{0,0,0},{0,0,0},Lh,up,8.f,1.f,{0,0,0},1e9f,1.f,1.f);
      CHECK(nearV(V::add(pa0.f,pa0.f),one.f,1e-3f),"pair force: a translation without caps: both hands together = the one-hand law");
      // one hand lets go: the remaining one's force falls to the one-hand law at most grabslew per substep
      const auto pairUp=P::grabGripForcePair({0,2.f,0},{0,1.f,0},{0,0,0},{0,0,0},L0,up,8.f,1.f,{0,0,0},1e9f,1.f,1.f);
      const auto alone=P::grabGripForce({0,2.f,0},{0,1.f,0},{0,0,0},{0,0,0},59.5f,up,8.f,1.f,900.f,20.f,25.f,.5f,pairUp.fN,40.f,1.f,1.f);
      CHECK(near(pairUp.fN[1],L0.upMaxN,1e-2f)&&alone.slewed&&near(V::length(V::sub(alone.fN,pairUp.fN)),40.f,1e-2f),
            "pair force: up at the pair cap; the partner gone: the change spread by the slew cap");}
     // ---------- TWO HANDS: the join and the holsters' order (pure) ----------
     {CHECK(GL::joinCandidate(GL::HoldHeavy,.07f,.10f)&&GL::joinCandidate(GL::HoldCorpse,.10f,.10f)&&!GL::joinCandidate(GL::HoldHeavy,.11f,.10f)&&
            !GL::joinCandidate(GL::HoldLight,.02f,.10f)&&!GL::joinCandidate(GL::HoldNone,.02f,.10f)&&!GL::joinCandidate(GL::HoldHeavy,NAN,.10f),
            "join: within the join distance of a held chair or corpse; never a light fixed hold (two fixed links fight) or no hold");
      CHECK(GL::preLatch(true,true,true)==GL::PreJoin&&GL::preLatch(false,true,true)==GL::PreHeavyMask&&GL::preLatch(false,true,false)==GL::PreNone&&
            GL::preLatch(false,false,true)==GL::PreNone,"holsters: a join beats every socket; a free hand beside a held chair is masked (no knife draw); else the normal branches");
      CHECK(GL::grabBeforeSocket(true,true)&&!GL::grabBeforeSocket(true,false)&&!GL::grabBeforeSocket(false,true),"holsters: a ready grab beats the socket zones (lever grabfirst)");
      // the mask only beside the held chair: the free hand's distance to it, fresh, within heavymaskcm; grabfirst off by default
      {GL::resetForTests();const std::uint64_t t=5'000'000'000ull;
       CHECK(!GL::grabFirst.load()&&!P::Levers{}.grabFirst&&near(P::Levers{}.heavyMaskM,.30f),"holsters: grabfirst off by default (the sockets before a ready grab, as before the two-hand grab); mask 30 cm");
       CHECK(GL::heavyNear(120,.30f)&&GL::heavyNear(300,.30f)&&!GL::heavyNear(301,.30f)&&!GL::heavyNear(-1,.30f),"heavy mask: within 30 cm of the held chair, not beyond, not unmeasured");
       GL::setHoldKind(0,GL::HoldHeavy);
       CHECK(!GL::heavyOther(1,t),"heavy mask: the other hand holds a chair, this hand's distance never measured: no mask (draws work)");
       GL::setHeld(1,.12f,t);CHECK(GL::heavyOther(1,t)&&GL::heldMmAt(1,t)==120,"heavy mask: 12 cm from the held chair: masked");
       GL::setHeld(1,.60f,t);CHECK(!GL::heavyOther(1,t),"heavy mask: 60 cm from the held chair (the hip holster while the chair hangs at the other side): no mask, the knife draws");
       GL::setHeld(1,.12f,t);CHECK(!GL::heavyOther(1,t+GL::readyMaxNs+1),"heavy mask: a distance older than 100 ms (the handphys tick stopped): no mask");
       GL::setHeld(1,-1.f,t);CHECK(!GL::heavyOther(1,t)&&GL::heldMmAt(1,t)==-1,"heavy mask: the stamp cleared: no mask");
       GL::setHeld(1,.12f,t);GL::setHoldKind(0,GL::HoldLight);CHECK(!GL::heavyOther(1,t),"heavy mask: the other hand holds a cup (light): no mask");
       GL::setHoldKind(0,GL::HoldHeavy);GL::heavyMask.store(false);CHECK(!GL::heavyOther(1,t),"heavy mask: lever heavymask 0: no mask");
       // the holsters' order beside the chair: no ready grab -> masked; a ready grab of its own -> the grab, ahead of the sockets
       const bool heavyFree=true,ready=true;
       CHECK(GL::preLatch(false,heavyFree&&!ready,true)==GL::PreNone&&GL::grabBeforeSocket(ready,heavyFree||GL::grabFirst.load()),
             "holsters: beside the chair with a ready grab of its own: not masked, the grab beats the sockets");
       CHECK(GL::preLatch(false,heavyFree&&!false,true)==GL::PreHeavyMask,"holsters: beside the chair, nothing to grab: masked");
       CHECK(!GL::grabBeforeSocket(ready,false||GL::grabFirst.load()),"holsters: nothing held, a hand touching a prop at the belt: the socket draws (grabfirst off)");
       GL::resetForTests();}
      const float W=59.5f*9.81f;
      CHECK(P::gripUpMaxN(59.5f,25.f,20.f)<W&&near(P::gripUpMaxN(59.5f,25.f,20.f),292.f,1.f),"chair: one hand's up cap 292 N, under the chair's 584 N weight (it drags)");
      bool carries=true;for(float sh=.15f;sh<=.851f;sh+=.05f){const auto a=P::pairLimits(59.5f,sh,70.f,900.f,20.f,.5f),b=P::pairLimits(59.5f,1.f-sh,70.f,900.f,20.f,.5f);
          carries=carries&&a.upMaxN+b.upMaxN>=W&&near(a.springKg+b.springKg,59.5f,1e-3f);}
      CHECK(carries,"chair: a joined pair carries the whole weight at every share .15 .. .85 (lift2 70 kg)");}
     // ---------- TWO HANDS: two-point statics (pure) ----------
     {const P::V3 up{0,1,0};const float M=59.5f,Wn=59.5f*9.81f;
      const auto torqueAcross=[&](const P::PairCarry& c,P::V3 com,P::V3 a,P::V3 b){
          const P::V3 t=V::add(V::cross(V::sub(a,com),c.carry[0]),V::cross(V::sub(b,com),c.carry[1]));const P::V3 d=V::normalized(V::sub(a,b));
          return V::length(V::sub(t,V::mul(d,V::dot(d,t))));};
      {const P::V3 gA{-.2f,1.f,0},gB{.2f,1.f,0};
       const auto c0=P::pairCarry({0,.8f,0},gA,gB,up,M,70.f,900.f,1.f),c1=P::pairCarry({-.1f,.8f,0},gA,gB,up,M,70.f,900.f,1.f);
       CHECK(near(c0.carry[0][1],.5f*Wn,.5f)&&near(c1.carry[0][1],.75f*Wn,.5f)&&near(c1.carry[1][1],.25f*Wn,.5f)&&near(c1.carry[0][0],0.f,1e-3f),
             "statics: a level grip line: the horizontal lever law (.5 / .75 of the weight)");
       CHECK(nearV(V::add(c1.carry[0],c1.carry[1]),{0,Wn,0},.5f)&&torqueAcross(c1,{-.1f,.8f,0},gA,gB)<.5f,"statics: the carries sum to the weight, no torque about the centre of mass across the line");
       const auto cb=P::pairCarry({.4f,.8f,0},gA,gB,up,M,70.f,900.f,1.f);
       CHECK(cb.carry[0][1]<0.f&&cb.carry[1][1]>Wn&&torqueAcross(cb,{.4f,.8f,0},gA,gB)<.5f,"statics: the centre of mass beyond grip B: hand A pushes down (a cantilever)");}
      {const P::V3 gA{0,1.2f,0},gB{0,.9f,0},com{.2f,1.f,0};const auto c=P::pairCarry(com,gA,gB,up,M,70.f,900.f,1.f);
       const P::V3 f=V::sub(c.carry[0],V::mul({0,Wn,0},.5f));
       CHECK(near(f[1],0.f,1e-2f)&&near(V::length(f),Wn*.2f/.3f,1.f)&&near(c.coupleN,Wn*.2f/.3f,1.f)&&torqueAcross(c,com,gA,gB)<.5f&&V::length(c.carry[0])<900.f,
             "statics: a vertical grip line (two hands on one post): a horizontal couple W r / d, each hand under 900 N");
       const auto cc=P::pairCarry(com,gA,gB,up,M,70.f,400.f,1.f);
       CHECK(cc.clamped&&near(cc.coupleN,400.f-.5f*Wn,.5f),"statics: the couple kept within forceMax - W / 2");}
      {const auto cs=P::pairCarry({.3f,1.f,0},{0,1.f,0},{.05f,1.f,0},up,M,70.f,900.f,1.f);
       CHECK(cs.shortLine&&nearV(cs.carry[0],{0,.5f*Wn,0},1e-2f)&&near(cs.coupleN,0.f),"statics: grips under 10 cm apart: the even split, no couple");
       const auto cu=P::pairCarry({-10.f,80.f,0},{-20.f,100.f,0},{20.f,100.f,0},up,M,70.f,900.f,100.f);
       CHECK(near(cu.carry[0][1],.75f*Wn,.5f),"statics: the same law in world units (100 per metre)");}
      CHECK(near(P::pairShareH({-.1f,.5f,0},{-.2f,1.f,0},{.2f,.5f,0},up),.75f,1e-4f),"statics: the horizontal lever share ignores the grips' heights");
      // the pair force with the carry: the spring capped on its own, the total within 900 N (the carry kept), the guard
      {const auto L=P::pairLimits(M,.5f,70.f,900.f,20.f,.5f);
       CHECK(near(L.upSpringN,.5f*M*20.f,1e-2f)&&near(L.forceMaxN,900.f),"statics: the spring's up cap .5 lift accel (595 N), the hand's total cap");
       const P::V3 carry{0,.5f*Wn,0};
       const auto f0=P::grabGripForcePair({0,1.f,0},{0,1.f,0},{0,0,0},{0,0,0},L,up,8.f,1.f,{0,0,0},1e9f,1.f,1.f,&carry);
       CHECK(nearV(f0.fN,carry,1e-3f)&&!f0.capped,"statics: no error: the force is the carry");
       const auto f1=P::grabGripForcePair({2.f,1.f,0},{0,1.f,0},{0,0,0},{0,0,0},L,up,8.f,1.f,{0,0,0},1e9f,1.f,1.f,&carry);
       CHECK(f1.capped&&V::length(f1.fN)<=900.01f&&f1.fN[1]>=.5f*Wn-1e-2f&&f1.fN[0]>0.f,"statics: a long side pull: the spring capped, |F| within 900 N, the carry kept");
       const auto f2=P::grabGripForcePair({0,1.f,-1.f},{0,1.f,0},{0,0,0},{0,0,0},L,up,8.f,1.f,{0,0,0},1e9f,1.f,1.f,&carry);
       CHECK(near(f2.fN[2],-L.sideMaxN,1e-2f),"statics: the side cap on the spring part (595 N)");
       const P::V3 gN{0,0,-1};float trim=0.f;
       const auto f3=P::grabGripForcePair({0,1.f,-.05f},{0,1.f,0},{0,0,0},{0,0,0},L,up,8.f,1.f,{0,0,0},1e9f,1.f,1.f,&carry,&gN,&trim);
       CHECK(trim>0.f&&near(f3.fN[2],0.f,1e-3f)&&nearV(f3.fN,carry,1e-2f),"guard: the spring's push into the player's capsule removed");
       const auto f4=P::grabGripForcePair({0,1.f,.05f},{0,1.f,0},{0,0,0},{0,0,0},L,up,8.f,1.f,{0,0,0},1e9f,1.f,1.f,&carry,&gN,&trim);
       CHECK(trim==0.f&&f4.fN[2]>0.f,"guard: a pull away from the player is kept");
       const auto f5=P::grabGripForcePair({0,1.01f,0},{0,1.f,0},{0,0,0},{0,0,0},L,up,8.f,1.f,{0,0,0},1e9f,1.f,1.f);
       CHECK(near(f5.fN[1],std::min(L.upMaxN,L.springKg*.64f+L.carryKg*9.81f),1e-2f),"statics: no carry given: the vertical share law");}}
     // ---------- PASSIVE contact: the arm step, the shove's arm gate (pure) ----------
     {CHECK(near(P::armStepM(.5f,.2f,.01f),.005f,1e-6f)&&near(P::armStepM(.2f,.2f,.01f),.002f,1e-6f)&&P::armStepM(.15f,.2f,.01f)==0.f&&
            P::armStepM(-1.f,.2f,.01f)==0.f&&P::armStepM(NAN,.2f,.01f)==0.f,
            "passive: the arm's whole drive once it reaches the gate (0.5 m/s: 5 mm per 10 ms); a still or withdrawing arm: 0");
      CHECK(P::pressArmHeld(1000*ms,1000*ms)&&P::pressArmHeld(1000*ms,1299*ms)&&!P::pressArmHeld(1000*ms,1300*ms)&&!P::pressArmHeld(0,5*ms)&&!P::pressArmHeld(1000*ms,999*ms),
            "passive: an arm-driven press holds 300 ms after its last arm step");
      // graded push: an escalated press on the arm alone; a deliberate shove made while stepping in keeps the step (v10 edge a 2.71 = arm
      // 1.23 + walk 1.48); never above the measured approach; the turn (in a, not in arm or walk) grades nothing
      CHECK(near(P::passiveGradeA(2.71f,1.23f,1.48f,false),1.23f)&&near(P::passiveGradeA(2.71f,1.23f,1.48f,true),2.71f)&&
            near(P::passiveGradeA(1.5f,1.23f,1.48f,true),1.5f)&&near(P::passiveGradeA(3.f,1.f,-.5f,true),1.f)&&near(P::passiveGradeA(NAN,1.f,1.f,true),0.f),
            "passive: the graded approach: escalated = the arm's; deliberate = the arm's + the forward step, at most a; backing off adds nothing");
      // the arm velocity's jump test: 2 cm in 14 ms is 1.4 m/s (motion), 30 cm in 14 ms is 21 m/s (tracking lost and found)
      CHECK(!P::armJump({0,0,0},{.02f,0,0},.014f,1.f)&&P::armJump({0,0,0},{.3f,0,0},.014f,1.f)&&!P::armJump({0,0,0},{30.f,0,0},.1f,100.f)&&
            P::armJump({0,0,0},{30.f,0,0},.014f,100.f)&&!P::armJump({0,0,0},{.3f,0,0},0.f,1.f),"arm velocity: a step over 12 m/s restarts the estimate (world units)");
      // the guard's normal: a side contact across up; a contact on top of the capsule (the normal mostly up) is not used
      CHECK(nearV(P::guardNormal({0,.2f,-1.f},{0,1,0}),{0,0,-1},1e-5f)&&V::length(P::guardNormal({.01f,1.f,0},{0,1,0}))==0.f&&
            V::length(P::guardNormal({.4f,1.f,0},{0,1,0}))==0.f&&nearV(P::guardNormal({.6f,1.f,0},{0,1,0}),{1,0,0},1e-5f)&&V::length(P::guardNormal({0,0,0},{0,1,0}))==0.f,
            "guard: the normal across up only when its horizontal part is at least half of it");
      P::TouchIn ti{};ti.capKind=2;ti.a=2.f;ti.speed=2.f;ti.swing=0.f;ti.facing=.9f;ti.arm=1.f;
      CHECK(P::touchVerdict(ti,1.4f,1.2f,true,true,true,true).kind==P::TouchShove,"passive: a chest shove with the arm gate off");
      ti.armGate=true;ti.armIn=.3f;ti.armInMin=.9f;const auto tv=P::touchVerdict(ti,1.4f,1.2f,true,true,true,true);
      CHECK(tv.kind==P::TouchNone&&tv.shoveFail==P::ShoveArm,"passive: the approach from the player's walk (arm 0.3 m/s into the body): no shove (arm)");
      ti.armIn=1.f;CHECK(P::touchVerdict(ti,1.4f,1.2f,true,true,true,true).kind==P::TouchShove,"passive: the arm itself at 1 m/s into the body: a shove");
      CHECK(P::armBin(.05f)==0&&P::armBin(.35f)==3&&P::armBin(1.5f)==7&&P::armBin(NAN)==0,"passive: the arm histogram bins");}
     // ---------- PROPHIT (geometric, pure): the segment-box distance, the gate ----------
     {std::mt19937 rng(7);std::uniform_real_distribution<float> U(-1.f,1.f);float worst=0.f;int bad=0;
      for(int it=0;it<300;++it){P::Obb o{};o.c={U(rng)*.3f,U(rng)*.3f,U(rng)*.3f};o.h={.05f+.2f*std::fabs(U(rng)),.05f+.2f*std::fabs(U(rng)),.02f+.1f*std::fabs(U(rng))};
        const P::V3 r=V::normalized({U(rng),U(rng),U(rng)});const float ang=3.f*U(rng);
        const P::Rows R=V::rotate(V::mul(r,ang),identity);o.ax[0]=R[0];o.ax[1]=R[1];o.ax[2]=R[2];
        const P::V3 a{U(rng)*.8f,U(rng)*.8f,U(rng)*.8f},b{U(rng)*.8f,U(rng)*.8f,U(rng)*.8f};
        float brute=1e9f;for(int k=0;k<=2000;++k){const P::V3 p=V::add(a,V::mul(V::sub(b,a),float(k)/2000.f));brute=std::min(brute,V::length(V::sub(p,P::obbClamp(o,p))));}
        const float d=P::segObbDist(a,b,o).d;worst=std::max(worst,d-brute);if(d<brute-1e-4f||d-brute>.01f)++bad;}
      std::printf("     segment-box distance: 300 random rotated boxes, worst excess over the sampled reference %.2f mm\n",worst*1000.f);
      CHECK(bad==0,"prop hit geo: the segment-box distance within 1 cm of a 2000-sample reference, never under it");
      {float inst[16]{0,0,1,0, 0,1,0,0, -1,0,0,0, 1,2,3,1},part[16]{1,0,0,0, 0,1,0,0, 0,0,1,0, .1f,0,0,1};P::Obb o{};
       CHECK(P::partObb(inst,part,{-.1f,-.2f,-.3f},{.1f,.2f,.3f},o)&&nearV(o.c,{1.f,2.f,3.1f},1e-5f)&&nearV(o.ax[0],{0,0,1})&&nearV(o.h,{.1f,.2f,.3f})&&
             !P::partObb(inst,part,{1,1,1},{0,0,0},o),"prop hit geo: a part's box through its part matrix and the instance matrix; an empty box refused");}
      P::PropHitWhy f{};
      CHECK(P::propGeoPass(2.f,2.f,1.5f,1.f,1.5f,1.2f,.6f,f)&&f==P::PropHitOk&&!P::propGeoPass(2.f,.2f,1.5f,1.f,1.5f,1.2f,.6f,f)&&f==P::PropHitNotSwung&&
            !P::propGeoPass(2.f,2.f,.5f,.5f,1.5f,1.2f,.6f,f)&&f==P::PropHitNoArm&&!P::propGeoPass(1.f,2.f,2.f,2.f,1.5f,1.2f,.6f,f)&&f==P::PropHitSlow,
            "prop hit geo: the speeds: the approach, the swing (not the player's run), the arm (not the player's turn)");
      CHECK(!P::propGeoPass(2.f,2.f,2.f,.2f,1.5f,1.2f,.6f,f)&&f==P::PropHitNoArm,"prop hit geo: the arms fast across the normal (a lift during a turn): noArm");
      // the arm trail: the peaks of the window, along a normal, the age of that peak
      {P::ArmTrail tr{};const std::uint64_t t0=50'000*ms;
       tr.push({2.f,0,0},t0);tr.push({0,1.5f,0},t0+100*ms);tr.push({0,0,0},t0+300*ms);
       auto pk=tr.peak({1,0,0},t0+300*ms,400*ms);
       CHECK(near(pk.mag,2.f)&&near(pk.along,2.f)&&near(pk.alongAgeMs,300.f,.5f),"arm trail: a swing 300 ms before the touch: in the window (the prop trails the hands)");
       pk=tr.peak({0,0,1},t0+300*ms,400*ms);CHECK(near(pk.mag,2.f)&&near(pk.along,0.f),"arm trail: no motion along the normal: 0");
       pk=tr.peak({-1,0,0},t0+300*ms,400*ms);CHECK(near(pk.along,0.f),"arm trail: motion away from the creature: 0, never below");
       pk=tr.peak({1,0,0},t0+500*ms,400*ms);CHECK(near(pk.mag,1.5f)&&near(pk.along,0.f),"arm trail: the swing 500 ms old: out of the window");
       P::ArmTrail many{};for(int i=0;i<100;++i)many.push({float(i),0,0},t0+std::uint64_t(i)*ms);
       CHECK(near(many.peak({1,0,0},t0+99*ms,1000*ms).along,99.f)&&near(many.peak({1,0,0},t0+99*ms,1000*ms).mag,99.f),"arm trail: the newest 48 samples kept");}
      const std::uint64_t W=60*ms,C=700*ms;std::uint64_t t=100'000*ms;
      {P::PropGeoGate g{};auto v=g.step(5,1,t,true,false,false,P::PropHitNotSwung,W,C);
       CHECK(v.start&&v.judged&&v.why==P::PropHitNotSwung,"prop hit gate: walking into a creature with the chair (swing low): notSwung");
       v=g.step(5,1,t+30*ms,true,false,true,P::PropHitOk,W,C);CHECK(v.why==P::PropHitOk,"prop hit gate: slow at the touch start, fast 30 ms later: a hit");
       v=g.step(5,1,t+50*ms,true,false,true,P::PropHitOk,W,C);CHECK(v.why==P::PropHitNotEdge,"prop hit gate: the same touch: no second hit");
       g.step(5,1,t+100*ms,false,true,false,P::PropHitSlow,W,C);v=g.step(5,1,t+200*ms,true,false,true,P::PropHitOk,W,C);
       CHECK(v.start&&v.why==P::PropHitNotEdge,"prop hit gate: 100 ms apart: not a new edge");
       g.step(5,1,t+300*ms,false,true,false,P::PropHitSlow,W,C);v=g.step(5,1,t+500*ms,true,false,true,P::PropHitOk,W,C);
       CHECK(v.start&&v.why==P::PropHitCooldown,"prop hit gate: a second swing within 700 ms: cooldown");
       g.step(5,1,t+600*ms,false,true,false,P::PropHitSlow,W,C);v=g.step(5,1,t+900*ms,true,false,false,P::PropHitSlow,W,C);
       CHECK(v.start&&v.why==P::PropHitSlow,"prop hit gate: a slow rest on the creature: slow");
       v=g.step(5,1,t+1000*ms,true,false,true,P::PropHitOk,W,C);CHECK(v.why==P::PropHitNotEdge,"prop hit gate: fast 100 ms after the touch start: not an edge");
       v=g.step(5,1,t+1010*ms,false,false,true,P::PropHitOk,W,C);CHECK(!v.judged,"prop hit gate: within the shell's hysteresis: nothing");
       g.step(5,1,t+1100*ms,false,true,false,P::PropHitSlow,W,C);v=g.step(5,1,t+1300*ms,true,false,true,P::PropHitOk,W,C);
       CHECK(v.start&&v.why==P::PropHitOk,"prop hit gate: the chair pulled back 200 ms, swung again: a hit (the slow rest used nothing up)");
       v=g.step(6,1,t+1310*ms,true,false,true,P::PropHitOk,W,C);CHECK(v.why==P::PropHitOk,"prop hit gate: another creature: its own gate");
       v=g.step(6,1,t+2400*ms,true,false,true,P::PropHitOk,W,C);
       CHECK(v.start&&v.why==P::PropHitOk,"prop hit gate: a creature unseen for 300 ms counts as apart since then");}
      // two props held one per hand: prop 2 apart from the creature every tick never disarms prop 1's touch
      {P::PropGeoGate g{};CHECK(!g.known(7,1),"prop hit gate: an unseen pair is unknown");
       auto v=g.step(7,1,t,true,false,false,P::PropHitSlow,W,C);CHECK(g.known(7,1)&&v.start&&v.why==P::PropHitSlow,"prop hit gate: prop 1 touches, slow");
       g.step(7,2,t+5*ms,false,true,false,P::PropHitSlow,W,C);
       v=g.step(7,1,t+20*ms,true,false,true,P::PropHitOk,W,C);
       CHECK(!v.start&&v.why==P::PropHitOk,"prop hit gate: per (creature, prop): prop 2 apart from the creature does not end prop 1's touch");}
      // the segment-box distance at 12 iterations on bone-length segments (at most 60 cm): within 5 mm of the reference, never under it
      {std::mt19937 rng(11);std::uniform_real_distribution<float> U(-1.f,1.f);int bad=0;float worst=0.f;
       for(int it=0;it<300;++it){P::Obb o{};o.c={U(rng)*.3f,U(rng)*.3f,U(rng)*.3f};o.h={.03f+.2f*std::fabs(U(rng)),.03f+.2f*std::fabs(U(rng)),.02f+.1f*std::fabs(U(rng))};
         const P::Rows R=V::rotate(V::mul(V::normalized({U(rng),U(rng),U(rng)}),3.f*U(rng)),identity);o.ax[0]=R[0];o.ax[1]=R[1];o.ax[2]=R[2];
         const P::V3 a{U(rng)*.6f,U(rng)*.6f,U(rng)*.6f};const P::V3 b=V::add(a,V::mul(V::normalized({U(rng),U(rng),U(rng)}),.6f*std::fabs(U(rng))));
         float brute=1e9f;for(int k=0;k<=2000;++k){const P::V3 p=V::add(a,V::mul(V::sub(b,a),float(k)/2000.f));brute=std::min(brute,V::length(V::sub(p,P::obbClamp(o,p))));}
         const float d=P::segObbDist(a,b,o,12).d;worst=std::max(worst,d-brute);if(d<brute-1e-4f||d-brute>.005f)++bad;}
       std::printf("     segment-box distance, 12 iterations, segments up to 60 cm: worst excess %.2f mm\n",worst*1000.f);
       CHECK(bad==0,"prop hit geo: 12 iterations on bone-length segments: within 5 mm of the reference");}
      // the rejects (propGeoTick): a capsule whose axis is farther than rad + r + hyst from the part's centre, or a body sphere farther than
      // rad + R + hyst, never hides a (box, capsule) pair within hyst of each other (brute-force distance)
      {std::mt19937 rng(23);std::uniform_real_distribution<float> U(-1.f,1.f);int missed=0,rejected=0,near13=0;const float hyst=.13f;
       for(int it=0;it<3000;++it){P::Obb o{};o.c={U(rng)*.6f,U(rng)*.6f,U(rng)*.6f};o.h={.02f+.25f*std::fabs(U(rng)),.02f+.25f*std::fabs(U(rng)),.02f+.1f*std::fabs(U(rng))};
         const P::Rows R=V::rotate(V::mul(V::normalized({U(rng),U(rng),U(rng)}),3.f*U(rng)),identity);o.ax[0]=R[0];o.ax[1]=R[1];o.ax[2]=R[2];
         const P::V3 a{U(rng)*.8f,U(rng)*.8f,U(rng)*.8f};const P::V3 b=V::add(a,V::mul(V::normalized({U(rng),U(rng),U(rng)}),.5f*std::fabs(U(rng))));
         const float r=.05f+.12f*std::fabs(U(rng)),rad=V::length(o.h);
         const P::V3 bc=V::mul(V::add(a,b),.5f);const float R0=.5f*V::length(V::sub(b,a))+r;
         float brute=1e9f;for(int k=0;k<=400;++k){const P::V3 p=V::add(a,V::mul(V::sub(b,a),float(k)/400.f));brute=std::min(brute,V::length(V::sub(p,P::obbClamp(o,p))));}
         brute-=r;if(brute<=hyst)++near13;
         const bool capRej=V::length(V::sub(o.c,P::closestOnSegment(a,b,o.c)))>rad+r+hyst,sphRej=V::length(V::sub(o.c,bc))>rad+R0+hyst;
         if(capRej||sphRej){++rejected;if(brute<=hyst)++missed;}}
       std::printf("     prop hit rejects: 3000 random (box, capsule) pairs, %d rejected, %d within the shell + hysteresis\n",rejected,near13);
       CHECK(missed==0&&rejected>0&&near13>0,"prop hit geo: the sphere and capsule rejects never drop a pair within the shell + hysteresis");}}
     // ---------- NPC push tiers, face, impulse; prop hits (pure) ----------
     {const float fm=1.6f,fc=.01f,am=1.5f;
      // the device's shove edges (a, depth, arm): two light brushes, four firm thrusts
      CHECK(P::pushTier(false,1.22f,.002f,1.22f,fm,fc,am)==P::TierMedium&&P::pushTier(false,1.38f,.005f,.88f,fm,fc,am)==P::TierMedium,
            "push tier: the light shove edges of the log (1.22 / 1.38 m/s, 0.2 / 0.5 cm): medium (no message)");
      CHECK(P::pushTier(false,1.77f,.020f,1.77f,fm,fc,am)==P::TierFirm&&P::pushTier(false,2.56f,.013f,2.82f,fm,fc,am)==P::TierFirm&&
            P::pushTier(false,2.40f,.049f,2.64f,fm,fc,am)==P::TierFirm&&P::pushTier(false,1.81f,.054f,2.04f,fm,fc,am)==P::TierFirm,"push tier: the firm shove edges of the log: firm");
      CHECK(P::pushTier(true,3.f,.05f,3.f,fm,fc,am)==P::TierLight&&P::pushTier(false,3.f,.05f,.5f,fm,fc,am)==P::TierMedium,
            "push tier: an escalated press is light; a creature walking into a still palm (no arm speed) is no firm push");
      CHECK(!P::pushStrong(P::TierMedium,true,4.f)&&P::pushStrong(P::TierFirm,true,2.f)&&P::pushStrong(P::TierFirm,false,3.f)&&!P::pushStrong(P::TierFirm,false,2.9f),
            "push strong (KNCKBK): a firm pair or a firm 3 m/s, never a medium push");
      const P::V3 fwd{0,0,1};
      CHECK(P::pushFace(fwd,{0,0,1})==1&&P::pushFace(fwd,{0,0,-1})==0&&P::pushFace(fwd,{1,0,0})==1,"push face: from behind 1, from the front 0 (the engine punch's dot >= 0)");
      CHECK(nearV(P::pushImpulseVec({0,0,2},0.f,7.f),{0,0,7.f})&&nearV(P::pushImpulseVec({0,0,2},12.f,7.f),{0,0,12.f})&&nearV(P::pushImpulseVec({1,0,0},0.f,-1.f),{0,0,0}),
            "push impulse: along the push at the punch's value (or the lever's), never scaled by the approach");
      {const auto a=P::pushImpulse({0,0,2},0.f,0.f,40.f),b=P::pushImpulse({0,0,2},0.f,.005f,40.f),c=P::pushImpulse({0,0,2},0.f,7.f,40.f),d=P::pushImpulse({0,0,2},12.f,0.f,40.f);
       CHECK(nearV(a.v,{0,0,40.f})&&a.src==2&&nearV(b.v,{0,0,40.f})&&b.src==2&&nearV(c.v,{0,0,7.f})&&c.src==1&&nearV(d.v,{0,0,12.f})&&d.src==0,
             "push impulse: a punch k of 0 (no engine melee action) falls back to the kick's size along the push, never a zero bulletVel");}
      // the push kick: along the palm (a push from behind sends the creature forward, a side push sideways), into the body when the palm is
      // slow or moves away from it; k x max(a, 1), capped
      {const auto behind=P::pushKickPlan({0,0,1},{.3f,0,2.f},2.5f,20.f,150.f);
       CHECK(behind.src==0&&near(behind.mag,50.f)&&near(V::dot(behind.dir,V::normalized(P::V3{.3f,0,2.f})),1.f,1e-5f)&&near(behind.palmMps,V::length(P::V3{.3f,0,2.f}),1e-5f),
             "push kick: a palm from behind at 2.5 m/s: along the palm, 20 x 2.5 = 50 N s");
       const auto side=P::pushKickPlan({1,0,0},{1.6f,.5f,-.9f},1.8f,20.f,150.f);
       CHECK(side.src==0&&near(side.dir[1],0.f)&&side.dir[0]>.8f&&side.dir[2]<0.f,"push kick: a side push sweeping back: along the palm's horizontal motion (sideways)");
       const auto slow=P::pushKickPlan({0,0,1},{.2f,0,.3f},1.f,20.f,150.f),away=P::pushKickPlan({0,0,1},{2.f,0,-.5f},2.f,20.f,150.f);
       CHECK(slow.src==1&&nearV(slow.dir,{0,0,1})&&near(slow.mag,20.f)&&away.src==1&&nearV(away.dir,{0,0,1}),
             "push kick: a slow palm or one moving along / away from the body: into the body, at least 1 m/s worth");
       const auto big=P::pushKickPlan({0,0,1},{0,0,9.f},9.f,20.f,150.f),none=P::pushKickPlan({0,0,1},{0,0,2.f},2.f,0.f,150.f),bad=P::pushKickPlan({0,1,0},{0,0,2.f},2.f,20.f,150.f);
       CHECK(near(big.mag,150.f)&&none.mag==0.f&&none.src==2&&bad.mag==0.f&&bad.src==2,"push kick: capped at the max; no size or no horizontal push: none");}
      // the kick's arming gate: only on a standing creature (ragdoll word 0 at the push), once per two-hand shove
      {CHECK(P::pushKickGate(true,true,true,0,false,true,50.f,false)==P::KickGateArmed,"kick gate: a firm self-message shove of a standing human: armed");
       CHECK(P::pushKickGate(true,true,true,1,false,true,50.f,false)==P::KickGateAlready&&P::pushKickGate(true,true,true,-1,false,true,50.f,false)==P::KickGateAlready,
             "kick gate: a shove on a creature already down (ragdoll word set at the push) or with the word unread: no kick (no impact damage from an open hand)");
       CHECK(P::pushKickGate(true,true,true,0,true,true,50.f,false)==P::KickGatePair,"kick gate: the other hand's shove on the same creature armed a kick within 300 ms: no second kick");
       CHECK(P::pushKickGate(false,true,true,1,true,false,0.f,true)==P::KickGateOff&&P::pushKickGate(true,false,true,0,false,true,50.f,false)==P::KickGateHuman&&
             P::pushKickGate(true,true,false,0,false,true,50.f,false)==P::KickGatePrint&&P::pushKickGate(true,true,true,0,false,false,50.f,false)==P::KickGateNoComp&&
             P::pushKickGate(true,true,true,0,false,true,0.f,false)==P::KickGateNoMag&&P::pushKickGate(true,true,true,0,false,true,50.f,true)==P::KickGateHpLock,
             "kick gate: off, not a human, prints, no torso / instance, no size, hp lock");
       CHECK(P::pushKickFires(true,false,15'000'000ull,true,1)&&!P::pushKickFires(true,false,15'000'000ull,true,0)&&!P::pushKickFires(true,true,15'000'000ull,true,1)&&
             !P::pushKickFires(true,false,201'000'000ull,true,1)&&!P::pushKickFires(true,false,15'000'000ull,false,1)&&!P::pushKickFires(false,false,15'000'000ull,true,1),
             "kick fire: the first tick within 200 ms that reads the ragdoll word set; once");
       const std::uint32_t full[8]{3,3,3,3,3,3,3,3},some[8]{3,0,3,0,3,3,3,3},took[8]{3,9,3,0,3,3,3,3};
       CHECK(P::kickFreeSlots(full)==0&&P::kickFreeSlots(some)==2&&P::kickQueued(some,took)&&!P::kickQueued(some,some)&&!P::kickQueued(full,full),
             "kick slots: all 8 pending impulses in use = full (no call); a free slot taken by the call = queued, none = lost");
       CHECK(P::kickMgr==0x1a0&&P::kickSlotStride==0x30&&P::kickSlotWord==0x28,"kick slots: entEntityInst +0x1a0, 0x30 per slot, in-use word +0x28 (FindFreeSlot 0x2913a64)");}
      CHECK(near(P::propHitGateSpeed(4.f,.5f),.5f)&&near(P::propHitGateSpeed(4.f,5.f),4.f)&&P::propHitGateSpeed(NAN,5.f)==0.f&&
            P::propHitSlowWhy(4.f,.5f,3.f)==P::PropHitNotSwung&&P::propHitSlowWhy(2.f,.5f,3.f)==P::PropHitSlow&&P::propHitSlowWhy(2.f,5.f,3.f)==P::PropHitSlow,
            "prop hit gate speed: the smaller of the approach and the swing; a fast approach without a swing is notSwung");
      {const auto w0=P::propHitWalk(0,6),w1=P::propHitWalk(6,6),w2=P::propHitWalk(4,6),w3=P::propHitWalk(6,2),w4=P::propHitWalk(0,0);
       CHECK(w0.first==3&&w0.last==6&&w0.skipped==2&&!w1.last&&w2.first==5&&w2.last==6&&!w2.skipped&&w3.first==1&&w3.last==2&&!w3.skipped&&!w4.last,
             "prop hit ring walk: the unseen serials (the 4 newest kept, older ones counted skipped), a producer that started again");}
      {P::PropContact e{};const P::V3 n{1,0,0};
       D::mergePropContact(e,0x77,1,{1,0,0},n,{4.f,0,0},.01f);D::mergePropContact(e,0x77,1,{1.02f,0,0},n,{1.f,0,0},.03f);
       CHECK(e.mover==0x77&&near(e.vPoint[0],4.f)&&near(e.vn,4.f)&&near(e.depth,.03f)&&near(e.pos[0],1.f),
             "prop contact: the fastest contact into the capsule is kept (the first substep of a hit), the deepest depth noted");
       D::mergePropContact(e,0x77,1,{1,0,0},n,{6.f,0,0},.005f);
       CHECK(near(e.vPoint[0],6.f)&&near(e.depth,.03f),"prop contact: a faster later contact replaces it");
       P::PropContact f{};D::mergePropContact(f,0x77,1,{1,0,0},{0,0,0},{6.f,0,0},.005f);CHECK(!f.mover,"prop contact: no normal: nothing kept");}
      P::PropHitGate g{};const std::uint64_t t=1'000'000'000ull;const std::uint64_t cool=700'000'000ull,mS=1'000'000ull;
      CHECK(g.step(5,t,4.f,3.f,cool)==P::PropHitOk&&g.step(5,t+30*mS,6.f,3.f,cool)==P::PropHitNotEdge&&g.step(5,t+300*mS,6.f,3.f,cool)==P::PropHitCooldown&&
            g.step(5,t+900*mS,2.f,3.f,cool)==P::PropHitSlow&&g.step(5,t+1200*mS,5.f,3.f,cool)==P::PropHitOk&&g.step(6,t+1210*mS,5.f,3.f,cool)==P::PropHitOk,
            "prop hit gate: a contact edge at 3 m/s or more hits; a lasting contact, the per-creature cooldown and a slow swing do not; another creature is its own");
      CHECK(P::propSpeedBin(.5f)==0&&P::propSpeedBin(3.5f)==3&&P::propSpeedBin(7.f)==5,"prop hit: the approach bins");}
     // the hook's alternation detector: offsets flipping 3 cm frame to frame on a still hand
     P::GrabHookMem hm{};auto& HS=P::grabHook[0];const auto a0=HS.alt2.load(),s0=HS.altStill.load(),t0=HS.toggles.load(),r0=HS.altFrom[1].load();
     for(int i=0;i<6;++i){const float ox=(i&1)?.03f:0.f;P::noteGrabHook(0,hm,true,{ox,0,0},{ox,0,0},{ox,0,0},{0,0,0},{0,0,0},{0,0,0},std::uint64_t(2*i),0.f,.1f,1.f);}
     CHECK(HS.alt2.load()>=a0+4&&HS.altStill.load()>=s0+4&&HS.toggles.load()==t0+1&&HS.altFrom[1].load()>=r0+4&&HS.jumps[3].load()>=5,"grab hook: a two-position flip on a still hand: alternations, from the prop's grip");
     P::noteGrabHook(0,hm,false,{},{},{},{},{},{},0,0.f,0.f,1.f);CHECK(HS.toggles.load()==t0+2,"grab hook: the hold's end");}
    // ---------- UNFREEZE approach / contact speed (pure) ----------
    {CHECK(P::nearFire(.10f,1.f,.12f)&&!P::nearFire(.13f,1.f,.12f)&&P::nearFire(.02f,.1f,.12f)&&!P::nearFire(.03f,.1f,.12f)&&!P::nearFire(0.f,.05f,.12f),
           "approach: fires within closing x 120 ms (at least 2 cm), never under 0.1 m/s");
     CHECK(near(P::contactMps(.15f,.2f,2.f,1.f),.2f)&&near(P::contactMps(.4f,.5f,std::nanf(""),std::nanf("")),.4f)&&near(P::contactMps(0.f,0.f,3.f,3.f),0.f)&&
           near(P::contactMps(-1.f,.3f,.25f,std::nanf("")),.25f),"contact speed: the controller's closing, the set velocity only as far as the controller's speed bears it out");
     // the palm's reach toward the cup: fingers first 13 cm, palm first the 5 cm floor, the fist 7 cm along F
     const P::Rows rr{{{1,0,0},{0,1,0},{0,0,1}}};const auto open=P::handBoxes({0,0,0},rr,false,1.f),fist=P::handBoxes({0,0,0},rr,true,1.f);
     CHECK(near(P::nearReachM(open,{0,0,0},{0,1,0},1.f),.13f,1e-4f)&&near(P::nearReachM(open,{0,0,0},{0,0,1},1.f),P::nearPalmM)&&
           near(P::nearReachM(fist,{0,0,0},{0,1,0},1.f),.07f,1e-4f)&&near(P::nearReachM(open,{0,0,0},{0,0,0},1.f),P::nearPalmM),
           "approach reach: fingers first 13 cm, palm first the 5 cm floor, the fist 7 cm, no direction the floor");
     // a fingertip-first tap on a cup of 4 cm (its level sphere 7.5 cm) at 0.2 m/s: the flat 5 cm reach fires only after the fingers met
     // it (the gap at that contact 4.5 cm, over the 2.4 cm lead); the parts' reach fires 5 cm or more before the contact
     {const float real=.04f,sphere=.075f,mps=.2f;const float contactDist=.13f+real;
      const auto firesAt=[&](float reach){for(float d=.40f;d>0.f;d-=.001f)if(P::nearFire(d-sphere-reach,mps,.12f))return d;return 0.f;};
      const float flat=firesAt(P::nearPalmM),parts=firesAt(P::nearReachM(open,{0,0,0},{0,1,0},1.f));
      std::printf("     fingertip tap at 0.2 m/s: contact at %.1f cm, flat reach fires at %.1f cm, parts' reach at %.1f cm\n",contactDist*100.f,flat*100.f,parts*100.f);
      CHECK(flat<contactDist&&parts>contactDist+.05f,"approach reach: a slow fingertip tap: the flat reach fires after the contact, the parts' reach before it");}
     // the arming slots: the nearest props win whatever order the cull gives them
     {struct E {std::uintptr_t inst{};float gap{};};std::array<E,4> a{};
      const std::array<std::pair<std::uintptr_t,float>,7> cull{{{1,-.02f},{2,-.30f},{3,-.01f},{4,-.25f},{5,-.15f},{6,-.005f},{7,-.20f}}};
      int full=0;for(int pass=0;pass<3;++pass)for(const auto& c:cull){const int i=P::nearSlot(a,c.first,c.second);if(i<0){++full;continue;}a[unsigned(i)]={c.first,c.second};}
      bool want[8]{};for(const auto& e:a)if(e.inst<8)want[e.inst]=true;
      CHECK(want[2]&&want[4]&&want[5]&&want[7]&&!want[1]&&!want[3]&&!want[6]&&full>0,"approach slots: 7 cups in reach, 4 slots: the 4 deepest in the capsule kept, every pass");
      CHECK(P::nearSlot(a,4,.5f)>=0&&P::nearSlot(a,9,-.01f)<0&&P::nearSlot(a,9,-.5f)>=0,"approach slots: an armed one keeps its slot; a new one only displaces a farther one");}
     // the first contact with an approach prop: the push speed always, a kick only for a contact the solver met while it was fixed
     {const auto a=P::approachFirstContact(true,false,10,10),b=P::approachFirstContact(true,false,12,10),c=P::approachFirstContact(true,true,8,10),
           d=P::approachFirstContact(false,false,8,10);
      CHECK(a.yield&&a.kick&&b.yield&&!b.kick&&!c.yield&&!c.kick&&!d.yield&&!d.kick,
            "approach first contact: met while fixed: push speed + kick; met active: push speed; touched before or a press: nothing");}}
    // ---------- MELEE fist gate (pure): a grip squeezed past .65 during a fast swing never makes a fist ----------
    {const std::uint64_t t0=1000*ms,dt=11'111'111ull; // 90 Hz
     // a fast open-hand slap (2 m/s) whose grip crosses .65 for 6 samples: no fist on any sample
     {MM::FistGate g{};bool any=false;float x=0.f;
      for(int i=0;i<40;++i){x+=2.f*float(dt)*1e-9f;const float grip=i>=20&&i<26?.75f:.4f;any=any||g.step(grip,{x,1.2f,0},true,t0+std::uint64_t(i)*dt);}
      CHECK(!any,"fist gate: a grip spike to .75 for 66 ms in a 2 m/s swing: no fist");}
     // a fist clenched with the hand still: a fist at once, then a punch keeps it
     {MM::FistGate g{};float x=0.f;bool first=false,kept=true;
      for(int i=0;i<10;++i)g.step(.2f,{0,1.2f,0},true,t0+std::uint64_t(i)*dt);
      first=g.step(.9f,{0,1.2f,0},true,t0+10*dt);
      for(int i=11;i<40;++i){x+=3.f*float(dt)*1e-9f;kept=kept&&g.step(.9f,{x,1.2f,0},true,t0+std::uint64_t(i)*dt);}
      CHECK(first&&kept&&g.closeMps<MM::fistCloseMps&&g.closedMs(t0+39*dt)>300.f,"fist gate: clenched while still: a fist at once, kept through a 3 m/s punch");}
     // a fist clenched during a fast swing and held: a fist once it has been closed 180 ms
     {MM::FistGate g{};float x=0.f;int firstOk=-1;
      for(int i=0;i<60;++i){x+=2.f*float(dt)*1e-9f;const bool ok=g.step(i>=10?.9f:.3f,{x,1.2f,0},true,t0+std::uint64_t(i)*dt);if(ok&&firstOk<0)firstOk=i;}
      CHECK(firstOk>=10+16&&firstOk<=10+17,"fist gate: clenched mid-swing at 2 m/s: a fist after 180 ms closed");}
     // the same sample twice gives the same verdict (the newest sample is judged before the per-sample loop); the 300 ms grip minimum
     {MM::FistGate g{};for(int i=0;i<5;++i)g.step(.3f,{0,1.2f,0},true,t0+std::uint64_t(i)*dt);
      const bool v1=g.step(.9f,{0,1.2f,0},true,t0+5*dt),v2=g.step(.9f,{0,1.2f,0},true,t0+5*dt);
      CHECK(v1&&v2&&near(g.minGrip(t0+5*dt),.3f)&&near(g.minGrip(t0+5*dt+400*ms),-1.f),"fist gate: a repeated sample keeps its verdict; the lowest grip within 300 ms");}}
    // ---------- NPC open hand: the message plan (never the player, never damage), the classifier, the press hold ----------
    {const std::uint32_t victim=0x00020005u,fake=0x40000000u,player=0x00010003u;bool bad=false;
     for(unsigned want=0;want<256;++want)for(int z=0;z<2;++z)for(int f=0;f<2;++f)for(int vv=0;vv<2;++vv){
        const auto pl=P::openTouchPlan(want,vv?victim:0u,f?fake:0u,z!=0);
        if(pl.attackerGuid==player||pl.damage!=0.f||pl.msg>P::OpenMsgNone||(pl.msg==P::OpenMsgNone&&pl.attackerGuid)||(pl.msg!=P::OpenMsgNone&&!pl.attackerGuid))bad=true;}
     CHECK(!bad,"open hand: no request byte, zombie or guid state gives the player as attacker or any damage");
     CHECK(P::openTouchPlan(1,victim,fake,false).msg==P::OpenMsgSelf&&P::openTouchPlan(1,victim,fake,false).attackerGuid==victim&&
           P::openTouchPlan(1,victim,fake,true).msg==P::OpenMsgFake&&P::openTouchPlan(1,victim,0,true).msg==P::OpenMsgNone&&
           P::openTouchPlan(0,victim,fake,false).attackerGuid==fake&&P::openTouchPlan(2,victim,fake,false).msg==P::OpenMsgNone&&
           P::openTouchPlan(7,victim,fake,false).msg==P::OpenMsgNone,"open hand: self (a zombie: the fake guid, none without one), fake, none; an unknown byte = none");
     P::Levers l{};l.slapAttacker=9;l.pushAttacker=2;const auto sl=P::sanitized(l);
     CHECK(sl.slapAttacker==2&&sl.pushAttacker==2&&P::Levers{}.slapAttacker==P::OpenMsgSelf,"open hand: the levers clamp to fake / self / none (default self)");
     P::TouchIn t{};t.capKind=1;t.a=2.9f;t.speed=2.95f;t.facing=.18f;t.swing=0.f;t.arm=3.f;t.side=.2f;
     CHECK(P::touchVerdict(t,1.4f,1.2f,true,true,true).kind==P::TouchSlap,"classifier: the neck at 2.95 m/s, facing .18, swing 0, arm 3 m/s: a slap (the log's lone fast reject)");
     t.arm=0.f;CHECK(P::touchVerdict(t,1.4f,1.2f,true,true,true).slapFail==P::SlapSwing,"classifier: the same hit with a still arm (a creature walking in): no slap");
     t.arm=3.f;t.capKind=2;t.speed=3.f;t.side=.6f;CHECK(P::touchVerdict(t,1.4f,1.2f,true,true,true,false).slapFail==P::SlapFacing,"classifier (ungraded): the facing gate stays off the head / neck");
     CHECK(P::touchVerdict(t,1.4f,1.2f,true,true,true).slapFail==P::SlapZone,"classifier (graded): the chest is no slap zone");
     t.capKind=0;t.speed=1.6f;t.facing=.1f;CHECK(P::touchVerdict(t,1.4f,1.2f,true,true,false).slapFail==P::SlapFacing,"classifier: a slow edge-on head touch keeps the facing gate");
     CHECK(P::pressHeld(.3f,.02f)&&!P::pressWanted(.3f,.02f,~0ull,false,false)&&!P::pressHeld(.2f,.02f)&&!P::pressHeld(.9f,.005f),
           "press: starts at facing .5, holds down to .25 and 1 cm");
     CHECK(P::facingBin(-1.f)==0&&P::facingBin(1.f)==7&&P::facingBin(.3f)==5&&P::facingBin(std::nanf(""))==0,"press: the facing bins");}
    // ---------- props: the root release (pure) ----------
    {CHECK(P::fragType(0x1000,0x30000000u,0x2000,0x3000)==0x2000&&P::fragType(0x1000,0x10000000u,0x2000,0x3000)==0&&P::fragType(0,0,0x2000,0x3000)==0x3000,
           "release: the type as PrepareForActivation resolves it (+0x78 with both resident bits, else null; else +0x98)");
     P::ReleaseIn r{};r.f80=0x1;r.typeOk=true;r.minMove=0.f;r.radius=.05f;r.depth=.01f;r.live=true;r.age=0;
     CHECK(P::releaseWanted(r,.35f,.005f)==P::ReleaseOk,"release: a small inserted prop with a fixed root under a pressing palm");
     auto x=r;x.f80=0x3;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseNotNeeded,"release: bit 1 already set: nothing to do");
     x=r;x.f80=0x0;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseNotInserted,"release: not inserted");
     x=r;x.f80=0x181;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseOverrideOff,"release: override + disabled");
     x=r;x.flags12=0x8;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseNever,"release: never-activate (+0x12 bit 3)");
     x=r;x.typeOk=false;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseTypeNull,"release: a null type is never released");
     x=r;x.typeBits=0x20;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseTypeLocked,"release: the type locks activation (left to the unlock)");
     x=r;x.f80=0x101;x.typeBits=0x20;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseOk,"release: an override that allows it beats the type lock");
     x=r;x.minMove=-1.f;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseNeverMove,"release: minMove < 0 never moves by force");
     x=r;x.radius=.6f;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseTooBig,"release: a table (radius 0.6 m) stays");
     x=r;x.radius=0.f;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseTooBig,"release: an unread level sphere (radius 0) is of unknown size: never released");
     x=r;x.radius=-1.f;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseTooBig,"release: a radius below 0 (not read): never released");
     x=r;x.depth=.002f;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseShallow,"release: a palm only 2 mm past the contact plane");
     x=r;x.age=3;CHECK(P::releaseWanted(x,.35f,.005f)==P::ReleaseShallow,"release: no contact in the last 2 substeps");}
    // ---------- NPC open hand (pure) ----------
    {P::TouchIn t{};t.capKind=0;t.a=3.f;t.speed=3.f;t.swing=3.f;t.facing=.95f;t.side=.9f;t.tangent=0.f;
     CHECK(P::classifyTouch(t,2.2f,1.2f,true,true)==P::TouchSlap,"touch: a fast palm on the head from the side: SLAP");
     auto b=t;b.facing=-.8f;CHECK(P::classifyTouch(b,2.2f,1.2f,true,true)==P::TouchSlap,"touch: the back of the hand: SLAP");
     auto f=t;f.side=0.f;CHECK(P::classifyTouch(f,2.2f,1.2f,true,true)==P::TouchSlap,"touch: a fast palm in the face: SLAP (no side gate)");
     auto fs=f;fs.speed=fs.a=1.8f;fs.swing=1.8f;CHECK(P::classifyTouch(fs,2.2f,1.2f,true,true)==P::TouchShove,"touch: a palm in the face under the slap speed: SHOVE");
     auto c=t;c.capKind=2;c.side=0.f;c.speed=c.a=2.f;c.swing=2.f;CHECK(P::classifyTouch(c,2.2f,1.2f,true,true)==P::TouchShove,"touch: the chest face-on: SHOVE");
     auto cs=t;cs.capKind=2;cs.speed=cs.a=3.f;CHECK(P::classifyTouch(cs,2.2f,1.2f,true,true)==P::TouchShove,"touch: the chest from the side under 1.5 x slapmps: no slap");
     cs.speed=cs.a=4.5f;cs.swing=4.5f;CHECK(P::classifyTouch(cs,2.2f,1.2f,true,true,false)==P::TouchSlap,"touch (ungraded): the chest at twice slapmps: SLAP");
     CHECK(P::classifyTouch(cs,2.2f,1.2f,true,true)==P::TouchShove,"touch (graded): the chest from the side at twice slapmps: a push, never a slap");
     auto sl=t;sl.speed=sl.a=sl.swing=1.f;CHECK(P::classifyTouch(sl,2.2f,1.2f,true,true)==P::TouchNone,"touch: slow: none (a bump)");
     auto e=t;e.facing=.2f;CHECK(P::classifyTouch(e,2.2f,1.2f,true,true)==P::TouchSlap,"touch: edge-on but fast at the head: SLAP (no facing gate there)");
     auto ec=e;ec.capKind=2;ec.speed=ec.a=ec.swing=4.5f;CHECK(P::classifyTouch(ec,2.2f,1.2f,true,true)==P::TouchNone,"touch: edge-on at the chest (|facing| < 0.25): none");
     auto w=t;w.swing=0.f;CHECK(P::classifyTouch(w,2.2f,1.2f,true,true)==P::TouchNone,"touch: no arm swing (the creature walked in): none");
     auto g=t;g.side=.1f;g.tangent=2.f;g.a=2.4f;g.speed=3.f;CHECK(P::classifyTouch(g,2.2f,1.2f,true,true)==P::TouchSlap,"touch: along the surface: SLAP");
     auto h=t;h.horse=true;CHECK(P::classifyTouch(h,2.2f,1.2f,true,true)==P::TouchNone,"touch: a horse: none");
     CHECK(P::classifyTouch(t,2.2f,1.2f,false,true)==P::TouchNone&&P::classifyTouch(fs,2.2f,1.2f,true,false)==P::TouchNone,"touch: levers npcslap / npcshove 0");
     auto leg=f;leg.capKind=5;CHECK(P::classifyTouch(leg,2.2f,1.2f,true,true)==P::TouchNone,"touch: a leg: no shove");
     // the first failed gates; the edge decides at max(2.2, slapmps); a slower touch opens the window
     {const auto v0=P::touchVerdict(sl,1.4f,1.2f,true,true,true);CHECK(v0.kind==P::TouchNone&&v0.slapFail==P::SlapSpeed&&v0.slapZone&&v0.shoveFail==P::ShoveZone,"touch verdict: slow: the speed gates failed, in the slap zone");
      auto s2=t;s2.speed=s2.a=s2.swing=1.8f;CHECK(P::touchVerdict(s2,1.4f,1.2f,true,true,true).kind!=P::TouchSlap&&P::touchVerdict(s2,1.4f,1.2f,true,true,false).kind==P::TouchSlap,
          "touch verdict: 1.8 m/s is no slap at the edge (2.2), a slap in the window (slapmps 1.4)");
      CHECK(P::touchVerdict(w,1.4f,1.2f,true,true,true).slapFail==P::SlapSwing&&P::touchVerdict(leg,1.4f,1.2f,true,true,true).slapFail==P::SlapZone,"touch verdict: no arm swing, a leg");
      P::SlapWindow wn{};wn.open=true;wn.startNs=1000*ms;wn.peak=1.f;wn.zone=true;wn.facing=true;
      CHECK(P::slapWindowStep(wn,true,false,1.6f,.9f,1050*ms,1.4f,120.f)==0&&wn.open&&near(wn.peak,1.6f),"slap window: still on the body: the peak kept");
      CHECK(P::slapWindowStep(wn,false,false,0.f,0.f,1080*ms,1.4f,120.f)==1&&!wn.open,"slap window: the hand left the head at 1.6 m/s: a slap");
      wn=P::SlapWindow{};wn.open=true;wn.startNs=1000*ms;wn.zone=true;wn.facing=true;
      CHECK(P::slapWindowStep(wn,true,true,2.f,.9f,1020*ms,1.4f,120.f)==0&&P::slapWindowStep(wn,false,false,0.f,0.f,1040*ms,1.4f,120.f)==2,"slap window: a press in between: no slap");
      wn=P::SlapWindow{};wn.open=true;wn.startNs=1000*ms;wn.zone=true;wn.facing=true;wn.peak=.8f;
      CHECK(P::slapWindowStep(wn,true,false,.9f,.9f,1130*ms,1.4f,120.f)==2,"slap window: slow until it ends: nothing");
      P::PressAsk pa{};bool esc=false;for(int i=0;i<3;++i)esc=P::pressEscalate(pa,.025f,.5f,2000*ms+std::uint64_t(i)*28*ms,.06f)||esc;
      CHECK(esc&&pa.pushed,"press escalation: 7.5 cm asked within 400 ms, the arm driving in at 0.5 m/s: a push");
      P::PressAsk pb{};bool e2=false;for(int i=0;i<12;++i)e2=P::pressEscalate(pb,.005f,.5f,3000*ms+std::uint64_t(i)*100*ms,.06f)||e2;
      CHECK(!e2,"press escalation: a slow press (2 cm per 400 ms): no push");
      P::PressAsk pc{};int pushes=0;for(int i=0;i<20;++i)pushes+=P::pressEscalate(pc,.025f,.5f,4000*ms+std::uint64_t(i)*28*ms,.06f)?1:0;
      CHECK(pushes==1,"press escalation: one push per touch");
      // a creature walking into a palm held still: a deep press asks for the push, the arm does not move into it: pressed, never shoved
      P::PressAsk pd{};bool e4=false,still=false;for(int i=0;i<14;++i){e4=P::pressEscalate(pd,.025f,0.f,5000*ms+std::uint64_t(i)*28*ms,.06f)||e4;still=still||pd.still;}
      CHECK(!e4&&still,"press escalation: a still arm (the creature walked in): no push, counted still");
      // a slow steady push (0.2 m/s, under the 0.3 m/s gate): the arm's travel over the window decides
      P::PressAsk pe{};bool e5=false;for(int i=0;i<14&&!e5;++i)e5=P::pressEscalate(pe,.025f,.2f,6000*ms+std::uint64_t(i)*28*ms,.06f);
      CHECK(e5&&pe.armM>=.03f-1e-4f,"press escalation: a steady 0.2 m/s push: a push once the arm moved 3 cm");}
     CHECK(P::pressWanted(.8f,.02f,100*ms,false,false)&&!P::pressWanted(.8f,.02f,99*ms,false,false)&&P::pressWanted(.8f,.02f,0,true,false)&&
           !P::pressWanted(.4f,.02f,200*ms,false,false)&&!P::pressWanted(.8f,.009f,200*ms,false,false)&&!P::pressWanted(.8f,.02f,120*ms,false,true)&&
           P::pressWanted(.8f,.02f,150*ms,false,true)&&P::pressWanted(.8f,.03f,0,false,false),"press: facing >= 0.5, 1 cm deep, 100 ms (150 ms after a slap), both hands or 3 cm at once");
     CHECK(P::slapReaction(.8f)==2&&P::slapReaction(-.8f)==3&&P::slapReaction(.1f)==1&&P::shoveReaction(0.f,false)==7&&P::shoveReaction(.8f,true)==5&&
           P::shoveReaction(-.8f,true)==6&&P::shoveReaction(0.f,true)==4,"touch reactions: UP_RGT / UP_LFT / UP_FOR, SHOVED, KNCKBK by side");
     CHECK(near(P::shoveDistanceM(1.2f,false),.104f,1e-4f)&&near(P::shoveDistanceM(10.f,false),.45f)&&near(P::shoveDistanceM(0.f,false),.05f)&&
           near(P::shoveDistanceM(3.f,true),.32f*1.6f,1e-4f),"shove distance: 8 cm + 12 cm per m/s over 1, 5..45 cm, x1.6 strong");
     // ---------- graded push (lever pushgrade) ----------
     {// the device's five slap edges of two-hand chest / neck pushes (v9 log npcEdges): pushes now
      struct E {int cap;float a,speed,facing,side,tangent,swing,arm;};
      const E ev[5]{{2,2.54f,2.92f,.52f,.77f,1.45f,2.48f,2.78f},{1,2.28f,2.28f,.52f,-.89f,.06f,2.43f,2.44f},{1,3.18f,3.39f,.84f,-.65f,1.17f,2.94f,3.22f},
                    {1,3.53f,3.92f,.67f,-.5f,1.7f,3.42f,3.78f},{2,3.25f,3.55f,.67f,.68f,1.41f,3.34f,3.62f}};
      bool allShove=true,allOld=true;
      for(const auto& e:ev){P::TouchIn t{};t.capKind=e.cap;t.a=e.a;t.speed=e.speed;t.facing=e.facing;t.side=e.side;t.tangent=e.tangent;t.swing=e.swing;t.arm=e.arm;
        allShove=allShove&&P::touchVerdict(t,1.4f,1.2f,true,true,true).kind==P::TouchShove;
        allOld=allOld&&P::touchVerdict(t,1.4f,1.2f,true,true,true,false).kind==P::TouchSlap;}
      CHECK(allShove,"graded: the five v9 slap edges (palm driven into the neck / chest, a / speed 0.87..1.00) are shoves");
      CHECK(allOld,"ungraded (pushgrade 0): the same edges keep the v9 slap verdict");
      P::TouchIn hs{};hs.capKind=0;hs.a=.9f;hs.speed=2.6f;hs.facing=.5f;hs.side=.9f;hs.tangent=2.4f;hs.swing=2.4f;hs.arm=2.6f;
      CHECK(P::touchVerdict(hs,1.4f,1.2f,true,true,true).kind==P::TouchSlap,"graded: a tangential slap of the head stays a slap");
      P::TouchIn nk=hs;nk.capKind=1;CHECK(P::touchVerdict(nk,1.4f,1.2f,true,true,true).kind==P::TouchSlap,"graded: a tangential slap of the neck stays a slap");
      P::TouchIn nd=nk;nd.a=2.5f;nd.speed=2.6f;nd.facing=.8f;const auto vn=P::touchVerdict(nd,1.4f,1.2f,true,true,true);
      CHECK(vn.kind==P::TouchShove&&vn.slapFail==P::SlapPush,"graded: a palm driven into the neck: a push (slapFail push)");
      P::TouchIn hd=nd;hd.capKind=0;hd.side=.9f;CHECK(P::touchVerdict(hd,1.4f,1.2f,true,true,true).kind==P::TouchSlap,"graded: the head keeps its slap for a palm driven into it");
      P::TouchIn sc{};sc.capKind=2;sc.a=2.7f;sc.speed=3.f;sc.facing=.8f;sc.side=.9f;sc.tangent=1.f;sc.swing=2.4f;sc.arm=2.6f;
      CHECK(P::touchVerdict(sc,1.4f,1.2f,true,true,true).kind==P::TouchShove,"graded: a chest push from the creature's side (side 0.9, ratio 0.9, 3 m/s): a shove");
      CHECK(P::touchVerdict(sc,1.4f,1.2f,true,true,true,false).kind==P::TouchSlap,"ungraded: the same side push was a slap");
      P::TouchIn lo{};lo.capKind=2;lo.a=.74f;lo.speed=.97f;lo.facing=.33f;lo.swing=.57f;lo.arm=.68f;
      CHECK(P::touchVerdict(lo,1.4f,1.2f,true,true,true).kind==P::TouchNone,"graded: the slow chest touch of v9 (facing 0.33) stays nothing");
      // the pair rule and the gap bins
      CHECK(P::touchPair(true,false,0,1000*ms,150*ms)&&P::touchPair(false,true,900*ms,1000*ms,150*ms)&&!P::touchPair(false,true,800*ms,1000*ms,150*ms)&&
            !P::touchPair(false,false,990*ms,1000*ms,150*ms)&&!P::touchPair(false,true,1100*ms,1000*ms,150*ms),"pair: the other hand on the creature now, or its edge within 150 ms");
      CHECK(P::pairGapBin(10*ms)==0&&P::pairGapBin(30*ms)==1&&P::pairGapBin(60*ms)==2&&P::pairGapBin(120*ms)==3&&P::pairGapBin(200*ms)==4,"pair: the gap bins");
      // pair merge
      {const auto m=P::pushMerge(2.f,{0,0,1},1.f,{1,0,0});
       CHECK(near(m.aEff,2.5f)&&nearV(m.dir,P::v::normalized(P::V3{1,0,2}),1e-4f),"pair merge: aEff = max + 0.5 min, the approach-weighted direction");
       const auto c=P::pushMerge(2.f,{0,0,1},2.f,{0,0,-1});CHECK(near(c.aEff,3.f)&&nearV(c.dir,{0,0,1},1e-4f),"pair merge: opposed hands: the first hand's direction");
       const auto y=P::pushMerge(2.f,{0,.5f,1},2.f,{0,-.5f,1});CHECK(near(y.dir[1],0.f)&&nearV(y.dir,{0,0,1},1e-4f),"pair merge: horizontal");
       const auto s1=P::pushSingle(1.63f,{.6f,.2f,.8f});CHECK(near(s1.aEff,1.63f)&&near(P::v::length(s1.dir),1.f,1e-4f)&&s1.dir[1]==0.f,"single push: a and its horizontal direction");}
      // the response table
      CHECK(P::pushDecide(1.63f,true,2.8f)==P::PushStumble&&near(P::stumbleDistanceM(1.63f),.1556f,1e-4f),"response: v9 #1 (a 1.63, flew 178 cm): a 15.6 cm stumble");
      CHECK(P::pushDecide(2.03f,true,2.8f)==P::PushStumble&&near(P::stumbleDistanceM(2.03f),.2036f,1e-4f),"response: v9 #6 (a 2.03): a 20.4 cm stumble");
      CHECK(P::pushDecide(2.79f,true,2.8f)==P::PushStumble&&near(P::stumbleDistanceM(2.79f),.2948f,1e-4f)&&P::stumbleDistanceM(9.f)<=.30f+1e-6f,"response: 2.79: a 29.5 cm stumble, 30 cm cap");
      CHECK(P::pushDecide(2.8f,true,2.8f)==P::PushKnockdown&&near(P::knockdownMps(2.8f,2.8f),.5f),"response: 2.8 from a firm edge: a knockdown at 0.5 m/s");
      CHECK(P::pushDecide(3.25f,true,2.8f)==P::PushKnockdown&&near(P::knockdownMps(3.25f,2.8f),.77f,1e-4f)&&near(P::knockdownMps(9.f,2.8f),1.5f),"response: 3.25: 0.77 m/s, at most 1.5");
      CHECK(P::pushDecide(3.5f,false,2.8f)==P::PushStumble,"response: no firm edge (the arm did not thrust): a stumble at any aEff");
      CHECK(near(P::kickMassKg(80.f),80.f)&&near(P::kickMassKg(12.f),75.f)&&near(P::kickMassKg(std::nanf("")),75.f)&&near(P::kickMassKg(400.f),75.f),"kick mass: 40..150 kg, else 75");
      // v9 #6: the creature walked towards the player at 1.2 m/s, pushed away from the player (+z): the kick cancels the walk
      {const auto j=P::kickImpulse(75.f,.5f,{0,0,1},{0,0,-1.2f},150.f);CHECK(nearV(j,{0,0,127.5f},1e-2f),"kick: 75 kg x (0.5 + 1.2 m/s) = 127.5 N s along the push (v9 sent 40.7)");
       const auto c=P::kickImpulse(75.f,1.5f,{0,0,1},{0,0,-2.f},150.f);CHECK(near(P::v::length(c),150.f,1e-2f)&&c[2]>0.f,"kick: capped at 150 N s");
       const auto w=P::kickImpulse(75.f,.5f,{0,0,1},{1.f,0,.5f},150.f);CHECK(nearV(w,{-75.f,0,0},1e-2f),"kick: a sideways walk is cancelled, the forward part kept");
       const auto y=P::kickImpulse(75.f,.5f,{0,0,1},{0,3.f,0},150.f);CHECK(y[1]==0.f,"kick: horizontal");
       CHECK(P::v::length(P::kickImpulse(std::nanf(""),.5f,{0,0,1},{0,0,0},150.f))==0.f,"kick: an unusable mass: none");
       const auto f=P::kickImpulse(75.f,.62f,{0,0,1},{0,0,1.f},150.f);CHECK(nearV(f,{0,0,0},1e-4f),"kick: a body already moving along the push faster than vDes: no kick toward the pusher (was -28 N s)");
       const auto a=P::kickImpulse(75.f,.6f,{0,0,1},{0,0,1.4f},150.f);CHECK(a[2]>=0.f&&near(P::v::length(a),0.f,1e-3f),"kick: an NPC walking away at 1.4 m/s: none along the push (was -60 N s)");
       const auto s2=P::kickImpulse(75.f,.5f,{0,0,1},{.4f,0,1.f},150.f);CHECK(nearV(s2,{-30.f,0,0},1e-2f),"kick: faster along the push: only its sideways walk cancelled");}
      CHECK(P::kickAgeNs(250'000'000ull,230'000'000ull)==20'000'000ull&&P::kickAgeNs(5,9)==0u&&
            P::pushKickFires(true,false,P::kickAgeNs(250'000'000ull,230'000'000ull),true,1)&&!P::pushKickFires(true,false,250'000'000ull,true,1),
            "kick window: a pair upgraded 230 ms after the first hand's edge measures its 200 ms from the upgrade (ragdoll 20 ms later: kicked)");
      CHECK(!P::slapHoldDrops(true,7,7,false)&&!P::slapHoldDrops(true,0,7,false)&&P::slapHoldDrops(true,8,7,false)&&P::slapHoldDrops(true,7,7,true)&&
            P::slapHoldDrops(false,7,7,false),"held slap: kept through a re-touch or a press of the same head; dropped for another creature, the hand's own shove, touches off");
      CHECK(P::pushThrust(2.47f,2.57f,1.6f,1.5f)&&P::pushThrust(2.13f,2.68f,1.6f,1.5f)&&!P::pushThrust(2.71f,.05f,1.6f,1.5f)&&!P::pushThrust(1.41f,1.67f,1.6f,1.5f)&&
            !P::pushThrust(std::nanf(""),3.f,1.6f,1.5f),
            "thrust: the device's 2.47 / 2.13 m/s edges (0.5 / 0.9 cm deep) are thrusts; walking into the creature (arm 0.05) and a 1.41 approach are not");
      CHECK(P::kick2Wanted({0,0,.1f},{0,0,1},.5f)&&!P::kick2Wanted({0,0,.4f},{0,0,1},.5f)&&P::kick2Wanted({.5f,0,.5f},{0,0,1},.5f)&&P::kick2Wanted({0,0,-.3f},{0,0,1},.5f),
            "kick 2: the pelvis under 0.6 vDes along the push or over 0.4 m/s sideways");
      CHECK(P::kickImpulse(75.f,.5f,{0,0,1},{0,0,.1f},P::kick2CapNs)[2]<=P::kick2CapNs&&near(P::kickImpulse(75.f,.5f,{0,0,1},{0,0,-2.f},P::kick2CapNs)[2],100.f,1e-2f),"kick 2: capped at 100 N s");
      // KNOCK SERVO: the slot a kick took, the slot word's life (entPendingImpulse::Update 0x29133b0 as written: count - 1 stored first; an
      // inactive object only counts; active: a count >= 1 becomes -1, ~count < sm_iNumFrames 4 applies GetScale(~count) (1, 0, 0, 0), else
      // the slot is freed)
      {const std::uint32_t some[8]{3,0,3,0,3,3,3,3},took[8]{3,0,3,9,3,3,3,3};
       CHECK(P::kickQueuedSlot(some,took)==3&&P::kickQueuedSlot(some,some)==-1,"servo: the slot a kick took (the first free one now in use)");
       const auto upd=[](std::int32_t& w,bool active,int& applied){const std::int32_t c=w;w=c-1;if(!active)return;if(c>=1){w=-1;return;}
           const std::int32_t k=~c;if(k<4){if(k==0)++applied;}else w=0;};
       std::int32_t w=8;int applied=0;bool early=false;
       upd(w,false,applied);early=early||P::kickSlotConsumed(w); // queued on the message tick, the object still inactive
       upd(w,true,applied);early=early||P::kickSlotConsumed(w);    // the first active update: -1, nothing applied
       CHECK(!early&&applied==0&&w==-1,"servo: queued while inactive, then the first active update: -1, not applied, not consumed");
       upd(w,true,applied);CHECK(applied==1&&w==-2&&P::kickSlotConsumed(w),"servo: the second active update applies it (word -2: consumed)");
       for(int i=0;i<4;++i)upd(w,true,applied);CHECK(applied==1&&w==0&&P::kickSlotConsumed(w),"servo: applied once, the slot freed (0)");
       // the slot's outcome (hp::kickSlotOutcome): an early kick 1 queued while inactive that stays inactive is counted out at 0, unapplied
       {std::int32_t e=8;int ap=0;bool neg=false;P::KickSlotOutcome oc=P::SlotPending;
        for(int i=0;i<8&&oc==P::SlotPending;++i){upd(e,false,ap);neg=neg||e<0;oc=P::kickSlotOutcome(e,neg,false);}
        CHECK(e==0&&ap==0&&oc==P::SlotFreed&&P::kickSlotConsumed(e),"early kick: 8 inactive updates free the slot unapplied: Freed (kickSlotConsumed alone read it as landed)");}
       {std::int32_t e=8;int ap=0;bool neg=false;upd(e,false,ap);upd(e,false,ap);P::KickSlotOutcome oc=P::kickSlotOutcome(e,neg,false);
        upd(e,true,ap);neg=neg||e<0;const auto oc1=P::kickSlotOutcome(e,neg,false);upd(e,true,ap);neg=neg||e<0;const auto oc2=P::kickSlotOutcome(e,neg,false);
        CHECK(oc==P::SlotPending&&oc1==P::SlotPending&&oc2==P::SlotApplied&&ap==1,"early kick: the ragdoll starts after 2 inactive updates: -1 pending, then applied");
        for(int i=0;i<4;++i)upd(e,true,ap);CHECK(e==0&&P::kickSlotOutcome(e,true,false)==P::SlotApplied,"early kick: freed after a negative word: applied");}
       CHECK(P::kickSlotOutcome(0,false,true)==P::SlotApplied&&P::kickSlotOutcome(-3,false,false)==P::SlotApplied&&P::kickSlotOutcome(5,false,true)==P::SlotPending,
             "slot outcome: a kick queued on a ragdoll reads 0 as applied (as before); -2 or below applied; a count pending");
       // the trace rows: 2 rolling rows before the message, then one a tick until full; a kick stamped only on this tick's row
       {unsigned n=0;auto a=P::traceRowPlan(n,24,false);n=a.n;auto b=P::traceRowPlan(n,24,false);n=b.n;auto c=P::traceRowPlan(n,24,false);n=c.n;
        CHECK(a.index==0&&!a.shift&&b.index==1&&!b.shift&&c.index==1&&c.shift&&n==2,"trace: before the message the last 2 ticks kept (rolling)");
        auto d=P::traceRowPlan(n,24,true);CHECK(d.index==2&&d.n==3,"trace: from the message one row a tick");
        CHECK(P::traceRowPlan(24,24,true).index==-1&&P::traceRowPlan(24,24,true).n==24,"trace: full: no row");
        CHECK(P::traceRowIsTick(250,250'400'000ull)&&!P::traceRowIsTick(230,250'400'000ull)&&P::traceRowIsTick(32000,40'000'000'000ull),
              "trace: a servo kick stamps only this tick's row (an older row when the trace is full: none)");}}
      // the centre of mass of 5 bones (Dempster's segment fractions), a missing bone left out, no pelvis = none
      {const P::V3 pts[5]{{0,1,0},{0,1.4f,0},{0,1.7f,0},{.1f,.5f,0},{-.1f,.5f,0}};const bool all[5]{true,true,true,true,true};
       const auto c=P::comProxy(pts,all);const float y=(.281f*1.f+.316f*1.4f+.081f*1.7f+.161f*.5f*2.f);
       CHECK(c.ok&&near(c.weight,1.f,1e-3f)&&near(c.p[1],y,1e-3f)&&near(c.p[0],0.f,1e-4f),"servo: the centre of mass of pelvis, spine3, head and knees (weights sum 1)");
       const bool noKnees[5]{true,true,true,false,false};const auto c2=P::comProxy(pts,noKnees);
       CHECK(c2.ok&&near(c2.p[1],(.281f+.316f*1.4f+.081f*1.7f)/.678f,1e-3f),"servo: the knees missing: the rest renormalised");
       const bool noPelvis[5]{false,true,true,true,true},thin[5]{true,false,true,false,false};
       CHECK(!P::comProxy(pts,noPelvis).ok&&!P::comProxy(pts,thin).ok,"servo: no pelvis, or under half the body's weight: none");}
      // the servo step: the kick law under 100 N s a kick, 250 N s in all, 4 kicks after kick 1; stop once the body moves along the push
      {const P::V3 d{0,0,1};
       CHECK(P::servoStep(75.f,.5f,d,{0,0,.5f},150.f,0).why==P::ServoReached,"servo: moving along the push at vDes, no sideways slide: stop (reached)");
       const auto s1=P::servoStep(75.f,.5f,d,{0,0,-1.f},150.f,0);CHECK(s1.why==P::ServoRunning&&near(s1.j[2],100.f,1e-2f),"servo: moving toward the pusher at 1 m/s: 100 N s along the push (the per-kick cap)");
       const auto s2=P::servoStep(75.f,.5f,d,{.8f,0,.5f},150.f,0);CHECK(s2.why==P::ServoRunning&&near(s2.j[0],-60.f,1e-2f)&&near(s2.j[2],0.f,1e-3f),"servo: a 0.8 m/s sideways slide: cancelled (-60 N s), nothing along");
       const auto s3=P::servoStep(75.f,.5f,d,{0,0,-1.f},240.f,1);CHECK(s3.why==P::ServoRunning&&near(P::v::length(s3.j),10.f,1e-2f),"servo: 10 N s left of 250: that much");
       CHECK(P::servoStep(75.f,.5f,d,{0,0,-1.f},249.5f,1).why==P::ServoBudget&&P::servoStep(75.f,.5f,d,{0,0,-1.f},100.f,4).why==P::ServoMax,"servo: the budget spent, or 4 kicks: stop");
       CHECK(P::servoStep(75.f,.5f,d,{NAN,0,0},100.f,0).why==P::ServoReached,"servo: an unusable velocity: no kick");}
      // the loop's mechanics on a toy body at 27 ms frames (each kick applied 2 frames after it is queued, a step one whole frame after its kick
      // was applied, a step every frame after that): moving toward the pusher at 1.5 m/s and sideways at 0.8 m/s at the push, kick 1 on that
      // velocity; pull = the hips pitched along the forward velocity (a = 1.5 v along the facing, the staggerFall mechanism) or a constant
      // 2 m/s2 toward the pusher. Checks the caps and that no kick points toward the pusher; not a model of the device's outcome.
      {const float M=75.f,vDes=.5f,dtF=.027f;const P::V3 dir{0,0,1};
       const auto run=[&](bool feedback,float& spent,unsigned& kicks,bool& toward)->P::V3{
           P::V3 v{.8f,0,-1.5f};spent=0.f;kicks=0;toward=false;struct Pend{int at;P::V3 j;};Pend pend[8]{};int np=0;
           const P::V3 j1=P::kickImpulse(M,vDes,dir,v,150.f);pend[np++]={2,j1};spent+=P::v::length(j1);
           int consumed=-1;bool done=false;
           for(int f=1;f<=11;++f){ // 11 frames = 297 ms
               const float a=feedback?std::clamp(1.5f*v[2],-3.f,3.f):-2.f;v=P::v::add(v,P::V3{0,0,a*dtF});
               for(int i=0;i<np;++i)if(pend[i].at==f){v=P::v::add(v,P::v::mul(pend[i].j,1.f/M));if(i==np-1)consumed=f;}
               if(!done&&consumed>=0&&f>consumed){const auto st=P::servoStep(M,vDes,dir,v,spent,kicks);
                   if(st.why==P::ServoBudget||st.why==P::ServoMax)done=true;
                   else if(st.why==P::ServoRunning){toward=toward||P::v::dot(st.j,dir)<-1e-4f;pend[np++]={f+2,st.j};spent+=P::v::length(st.j);++kicks;consumed=-1;}}}
           return v;};
       float sp{};unsigned k{};bool tw{};const auto vf=run(true,sp,k,tw);
       CHECK(!P::kick2Wanted(vf,dir,vDes)&&sp<=250.f+1e-3f&&k<=P::servoMaxKicks&&!tw,"servo: a body whose hips pitch along its velocity ends moving along the push in 300 ms, within 250 N s");
       float sp2{};unsigned k2{};bool tw2{};const auto vc=run(false,sp2,k2,tw2);
       CHECK(sp2<=250.f+1e-3f&&k2<=P::servoMaxKicks&&!tw2&&vc[2]>0.f&&std::fabs(vc[0])<.4f,"servo: under a constant pull toward the pusher: re-kicked within the caps, still moving along the push at 300 ms");
       if(P::kick2Wanted(vf,dir,vDes)||sp>250.f||vc[2]<=0.f)std::printf("     servo v (%.2f, %.2f) spent %.1f kicks %u; constant pull v (%.2f, %.2f) spent %.1f kicks %u\n",
           double(vf[0]),double(vf[2]),double(sp),k,double(vc[0]),double(vc[2]),double(sp2),k2);}
      CHECK(P::Levers{}.pushKickServo&&!P::Levers{}.pushKickEarly&&P::Levers{}.grabWeld&&P::Levers{}.grabLightFf&&P::Levers{}.bloodHires&&P::Levers{}.grabSkipLight,
            "levers: pushkickservo 1, pushkickearly 0, grabweld 1, grablightff 1, bloodhires 1, grabskiplight 1 by default");
      P::Levers lv{};lv.pushDownMps=99.f;lv.slapHoldMs=-1.f;lv.pairMs=900.f;const auto ls=P::sanitized(lv);
      CHECK(near(ls.pushDownMps,10.f)&&ls.slapHoldMs==0.f&&near(ls.pairMs,300.f)&&P::Levers{}.pushGrade&&P::Levers{}.pushKick2,"graded levers: clamped, on by default");}
     const auto p1=P::pushPlan(.104f),p2=P::pushPlan(.45f);
     CHECK(p1.ticks==6&&near(p1.perTickM*6.f,.104f,1e-5f)&&p2.ticks==15&&p2.perTickM<=.0301f&&P::pushPlan(0.f).ticks==0,"shove plan: at least 6 ticks, at most 3 cm per tick");
     CHECK(near(P::pressStepM(.05f,.5f,.012f,0.f,.5f),.012f)&&near(P::pressStepM(.02f,.5f,.012f,0.f,.5f),.005f)&&P::pressStepM(.005f,.5f,.012f,0.f,.5f)==0.f&&
           near(P::pressStepM(.05f,.5f,.012f,.495f,.5f),.005f,1e-5f)&&P::pressStepM(.05f,.5f,.012f,.5f,.5f)==0.f,"press step: gain x (depth - 1 cm), 1.2 cm per tick, the 50 cm cap");
     CHECK(near(P::pressYaw({.3f,1.f,0},{0,1.f,0},{0,0,1},{0,1,0},1.f),-.045f,1e-5f)&&near(P::pressYaw({1.f,1.f,0},{0,1.f,0},{0,0,1},{0,1,0},1.f),-.05f)&&
           P::pressYaw({0,1.f,.3f},{0,1.f,0},{0,0,1},{0,1,0},1.f)==0.f,"press yaw: the push's torque about the centre, capped at 0.05 rad");
     CHECK(near(P::slapYaw(2.f,.5f,.25f),.12f)&&near(P::slapYaw(10.f,-.5f,.25f),-.25f),"slap yaw: 6 % of the tangential speed, the side's sign, capped");
     CHECK(nearV(P::horizontal({.6f,.8f,0},{0,1,0}),{1,0,0})&&V::length(P::horizontal({0,1,0},{0,1,0}))==0.f,"push direction: along the ground");
     static_assert(P::fakeGuid(0x0003)==(0x4003u<<16)&&P::fakeGuid(0x8000)==0xC0000000u&&P::fakeGuid(0x1234)==(0x5234u<<16));
     CHECK(P::fakeGuid(7)&&(P::fakeGuid(7)&0xffffu)==0&&(P::fakeGuid(7)>>16)!=7u,"fake guid: slot 0, another generation (never resolves)");
     // aGuidManager::Init writes slot 0's generation as 0x8000 (never allocated)
     CHECK(P::fakeGuid(0x8000)==0xC0000000u&&(P::fakeGuid(0x8000)>>16)!=0x8000u&&P::fakeGuid(0x4000)==0u,"fake guid: slot 0 at its constant generation 0x8000 -> 0xC0000000");
     {bool ok=true;for(unsigned g=0;g<0x10000u&&ok;++g){const auto f=P::fakeGuid(std::uint16_t(g));ok=!f||((f&0xffffu)==0&&(f>>16)!=g);}
      CHECK(ok,"fake guid: never slot 0's own generation, never another slot");}
     CHECK(P::pushGuard(0,0,true,0,0,0,0)==-1&&P::pushGuard(1,0,true,0,0,0,0)==0&&P::pushGuard(0,1,true,0,0,0,0)==0&&P::pushGuard(0,0,false,0,0,0,0)==1&&
           P::pushGuard(0,0,true,0,0,1,0)==P::PushWhyRagdoll&&P::pushGuard(0,0,true,0,0,0,0x10000)==3&&P::pushGuard(0,0,true,0,0,0,0x2000000)==3&&
           P::pushGuard(0,0,true,1,0,0,0)==4&&P::pushGuard(0,0,true,0,0,0,0x1)==-1,"push guard: human, the mover vtable, ragdoll, busy, flags");
     // the heading is no refusal: the offset is pre-rotated so the engine's own turn (ApplyTranslationOffset 0x288aac8) lands on the world vector
     {bool ok=true;std::mt19937 g(7);std::uniform_real_distribution<float> a(-3.14f,3.14f);
      for(int i=0;i<200&&ok;++i){const float h=a(g);const P::V3 w{a(g)*.01f,a(g)*.001f,a(g)*.01f};ok=nearV(P::engineHeadingTurn(P::preRotate(w,h),h),w,1e-6f);}
      CHECK(ok,"push: pre-rotated by the heading, the engine's turn lands on the world vector (200 headings)");
      CHECK(nearV(P::preRotate({.01f,0,0},0.f),{.01f,0,0})&&nearV(P::preRotate({.01f,0,0},1.5707963f),{0,0,.01f},1e-6f),"push: heading 0 untouched, 90 deg");
      // +0xb85 bit 0x4000000: ApplyTranslationOffset 0x288aac4 branches past the turn, so the offset goes in unturned
      {bool ok=true;std::mt19937 g2(11);std::uniform_real_distribution<float> b(-3.14f,3.14f);
       for(int i=0;i<50&&ok;++i){const float h=b(g2);const P::V3 w{b(g2)*.01f,0.f,b(g2)*.01f};const std::uint32_t f=P::moverNoTurnFlag|0x100u;
         ok=nearV(P::preRotate(w,h,f),w)&&nearV(P::engineHeadingTurn(P::preRotate(w,h,f),h,f),w,1e-6f)&&nearV(P::engineHeadingTurn(P::preRotate(w,h,0x100u),h,0x100u),w,1e-6f);}
       CHECK(ok,"push: a mover with the no-turn flag gets the world vector unturned, other flag bits still turned");}}}
    // ---------- GRAB (pure) ----------
    {const float m[16]{0,0,1,0, 0,1,0,0, -1,0,0,0, 1,2,3,1};
     CHECK(nearV(P::toLocal(m,{1,2,4}),{1,0,0})&&nearV(P::toLocal(m,{2,2,3}),{0,0,-1}),"grab: a point in a matrix's frame");
     CHECK(P::boxDistance({0,0,0},{-1,-1,-1},{1,1,1})==0.f&&near(P::boxDistance({2,0,0},{-1,-1,-1},{1,1,1}),1.f)&&near(P::boxDistance({2,2,0},{-1,-1,-1},{1,1,1}),std::sqrt(2.f)),"grab: the distance to a box");
     std::array<P::GrabPart,3> parts{};for(auto& p:parts){p.m[0]=p.m[5]=p.m[10]=p.m[15]=1.f;p.bmin={-.05f,-.05f,-.05f};p.bmax={.05f,.05f,.05f};}
     parts[1].m[12]=.5f;parts[2].m[13]=.5f;parts[2].bmax={0,0,0};parts[2].bmin={1,1,1}; // an empty part
     const auto n0=P::nearestPart(parts.data(),3,{.48f,.02f,0});CHECK(n0.index==1&&n0.dist==0.f,"nearest part: the part the palm is in (composite)");
     const auto n1=P::nearestPart(parts.data(),3,{0,.53f,0});CHECK(n1.index==0&&near(n1.dist,.48f,1e-4f),"nearest part: an empty part is skipped");
     const auto n2=P::nearestPart(parts.data(),1,{.08f,0,0});CHECK(n2.index==0&&near(n2.dist,.03f,1e-5f),"nearest part: a single bound is part 0");
     CHECK(nearV(P::grabDrive({0,0,0},{10,0,0},.5f),{.5f,0,0})&&nearV(P::grabDrive({1,0,0},{1.2f,0,0},.5f),{1.2f,0,0}),"grab drive: the velocity change capped by force * dt / mass");
     const std::uint64_t t0=700'000*ms;P::GrabHoldGate g{};
     CHECK(P::grabStep(g,.1f,0.f,0.f,0.f,.35f,350.f,t0)==P::GrabKeep&&P::grabStep(g,.1f,0.f,0.f,0.f,.35f,350.f,t0+300*ms)==P::GrabKeep,"grab release: a 0.3 s grip dip keeps the hold");
     CHECK(P::grabStep(g,.9f,0.f,0.f,0.f,.35f,350.f,t0+310*ms)==P::GrabKeep&&P::grabStep(g,.1f,0.f,0.f,0.f,.35f,350.f,t0+400*ms)==P::GrabKeep&&
           P::grabStep(g,.1f,0.f,0.f,0.f,.35f,350.f,t0+760*ms)==P::GrabOpen,"grab release: the grip closed again restarts the debounce; open 350 ms: released");
     P::GrabHoldGate g2{};CHECK(P::grabStep(g2,.1f,0.f,1.5f,0.f,.35f,350.f,t0)==P::GrabThrow,"grab release: an open hand moving 1.2 m/s or more: a throw at once");
     P::GrabHoldGate g3{};CHECK(P::grabStep(g3,.1f,.8f,0.f,0.f,.35f,350.f,t0)==P::GrabKeep&&P::grabStep(g3,.1f,.8f,0.f,0.f,.35f,350.f,t0+900*ms)==P::GrabKeep,"grab release: held while the trigger is pressed");
     P::GrabHoldGate g4{};CHECK(P::grabStep(g4,.9f,0.f,0.f,.4f,.35f,350.f,t0)==P::GrabKeep&&P::grabStep(g4,.9f,0.f,0.f,.4f,.35f,350.f,t0+160*ms)==P::GrabLeash,"grab release: over the leash for 150 ms");
     P::GrabHoldGate g5{};CHECK(P::grabStep(g5,.9f,0.f,0.f,.5f,.6f,350.f,t0,250*ms)==P::GrabKeep&&P::grabStep(g5,.9f,0.f,0.f,.7f,.6f,350.f,t0+10*ms,250*ms)==P::GrabKeep&&
           P::grabStep(g5,.9f,0.f,0.f,.7f,.6f,350.f,t0+200*ms,250*ms)==P::GrabKeep&&P::grabStep(g5,.9f,0.f,0.f,.7f,.6f,350.f,t0+270*ms,250*ms)==P::GrabLeash,
           "grab break: 50 cm stretch held; over 60 cm for 250 ms: a break");
     // ---------- heavy / two-hand holds: no throw, the open timer with hysteresis, the chord kept off by a grab ----------
     {P::GrabHoldGate h1{};
      CHECK(P::grabStep(h1,0.f,0.f,1.67f,0.f,.6f,350.f,t0,250*ms,true)==P::GrabKeep,"heavy hold: a zeroed grip with the hand at 1.67 m/s (the v9 'throws'): no throw");
      bool kept=true;for(int i=1;i<=12;++i)kept=kept&&P::grabStep(h1,0.f,0.f,1.5f,0.f,.6f,350.f,t0+std::uint64_t(i)*28*ms,250*ms,true)==P::GrabKeep;
      CHECK(kept&&P::grabStep(h1,0.f,0.f,0.f,0.f,.6f,350.f,t0+13*28*ms,250*ms,true)==P::GrabOpen,"heavy hold: open only after 350 ms under .30");
      {P::GrabHoldGate ht{};bool k2=true;for(int i=0;i<=12;++i)k2=k2&&P::grabStep(ht,.1f,.9f,0.f,0.f,.6f,350.f,t0+std::uint64_t(i)*28*ms,250*ms,true)==P::GrabKeep;
       CHECK(k2&&P::grabStep(ht,.1f,.9f,0.f,0.f,.6f,350.f,t0+13*28*ms,250*ms,true)==P::GrabOpen,"heavy hold: a finger resting on the trigger never keeps an open hand on the chair");}
      P::GrabHoldGate h2{};P::grabStep(h2,.9f,0.f,0.f,0.f,.6f,350.f,t0,250*ms,true);
      for(int i=1;i<=8;++i)P::grabStep(h2,.1f,0.f,0.f,0.f,.6f,350.f,t0+std::uint64_t(i)*28*ms,250*ms,true); // 224 ms under .30
      bool band=true;for(int i=9;i<=40;++i)band=band&&P::grabStep(h2,.4f,0.f,0.f,0.f,.6f,350.f,t0+std::uint64_t(i)*28*ms,250*ms,true)==P::GrabKeep;
      CHECK(band,"heavy hold: a grip between .30 and .50 neither counts nor releases (0.9 s)");
      bool rel=false;int k=41;for(;k<=50&&!rel;++k)rel=P::grabStep(h2,.1f,0.f,0.f,0.f,.6f,350.f,t0+std::uint64_t(k)*28*ms,250*ms,true)==P::GrabOpen;
      CHECK(rel&&k-41>=4&&k-41<=6,"heavy hold: the time under .30 adds up across the band (224 ms before, the rest after)");
      P::GrabHoldGate h3{};P::grabStep(h3,.9f,0.f,0.f,0.f,.6f,350.f,t0,250*ms,true);
      for(int i=1;i<=10;++i)P::grabStep(h3,.1f,0.f,0.f,0.f,.6f,350.f,t0+std::uint64_t(i)*28*ms,250*ms,true);
      P::grabStep(h3,.6f,0.f,0.f,0.f,.6f,350.f,t0+11*28*ms,250*ms,true);
      bool reset=true;for(int i=12;i<=22;++i)reset=reset&&P::grabStep(h3,.1f,0.f,0.f,0.f,.6f,350.f,t0+std::uint64_t(i)*28*ms,250*ms,true)==P::GrabKeep;
      CHECK(reset,"heavy hold: a regrip at .50 or more restarts the open time");
      P::GrabHoldGate h4{};CHECK(P::grabStep(h4,.1f,0.f,1.5f,0.f,.6f,350.f,t0,250*ms,false)==P::GrabThrow,"light hold: the throw rule unchanged");
      P::GrabHoldGate h5{};P::grabStep(h5,.9f,0.f,0.f,.7f,.6f,350.f,t0,250*ms,true);
      CHECK(P::grabStep(h5,.9f,0.f,0.f,.7f,.6f,350.f,t0+260*ms,250*ms,true)==P::GrabLeash,"heavy hold: the leash still breaks it");
      // the pair's caps: each hand may lift half the pair plus grabAccel; the carried weight stays at the share
      const auto a=P::pairLimits(59.5f,.15f,70.f,900.f,20.f,.5f,.5f),b=P::pairLimits(59.5f,.85f,70.f,900.f,20.f,.5f,.5f),o=P::pairLimits(59.5f,.15f,70.f,900.f,20.f,.5f);
      CHECK(near(a.upMaxN,.5f*59.5f*29.81f,.5f)&&!a.upByForce&&near(a.carryKg,.15f*59.5f,1e-3f)&&near(b.upMaxN,900.f)&&b.upByForce&&near(o.upMaxN,266.f,1.f),
            "pair caps (grabpairfull): 887 N for the 0.15 share hand (v9: 266 N), 900 N (grabForceMaxN) for the other, carry by share");
      CHECK(a.upMaxN+b.upMaxN>=59.5f*(9.81f+20.f)-1.f,"pair caps: the pair lifts the chair's weight plus grabAccel for any share");
      CHECK(near(P::pairSlewN(59.5f,1.f,8.f,20.f,.0139f,40.f),132.3f,.5f)&&near(P::pairSlewN(2.f,1.f,8.f,20.f,.0139f,40.f),40.f)&&near(P::pairSlewN(59.5f,1.f,8.f,20.f,std::nanf(""),40.f),40.f),
            "pair slew: 0.5 M 2 zeta wn accel dt (132 N at 59.5 kg, 13.9 ms), at least grabslew");
      // the chord: a grab keeps it off (the grips pass); otherwise both grips at 0.85 are zeroed
      {rdrvr::runtime::HoldChord c{};std::array<bool,2> held{};std::array<float,2> gr{1.f,1.f};const auto c0=rdrvr::runtime::theaterChordStats.consumed.load();
       rdrvr::runtime::theaterGripChord(c,held,gr,1000*ms,true);CHECK(gr[0]==1.f&&gr[1]==1.f,"chord: busy (a grab): the grips pass untouched");
       rdrvr::runtime::HoldChord c2{};std::array<bool,2> h2b{};std::array<float,2> g0{0.f,0.f};rdrvr::runtime::theaterGripChord(c2,h2b,g0,1000*ms,false);
       std::array<float,2> g1{.9f,.95f};rdrvr::runtime::theaterGripChord(c2,h2b,g1,1010*ms,false);
       CHECK(g1[0]==0.f&&g1[1]==0.f&&rdrvr::runtime::theaterChordStats.consumed.load()==c0+1&&rdrvr::runtime::theaterChordStats.consuming.load(),
             "chord: both grips at 0.85 without a grab: zeroed at once (what the v9 chair holds read as an open hand)");
       // HOLD-END GRIP LATCH: a chord that never armed (busy while both grips closed: a hold) passes the grips untouched once the busy ends;
       // a full release arms it and the next squeeze is zeroed, its owned bits set for the squeezed grips
       {namespace R=rdrvr::runtime;R::HoldChord c3{};std::array<bool,2> h3{};const auto con=R::theaterChordStats.consumed.load(),un=R::theaterChordStats.unarmedPassed.load();
        for(int i=0;i<10;++i){std::array<float,2> g{1.f,1.f};std::uint8_t own=9;R::theaterGripChord(c3,h3,g,(2000+10*i)*ms,true,2'000'000'000,&own);
            CHECK(g[0]==1.f&&g[1]==1.f&&own==0,"chord unarmed: busy (a hold): untouched, nothing owned");}
        bool untouched=true,owned=false;
        for(int i=0;i<30;++i){std::array<float,2> g{1.f,1.f};std::uint8_t own=9;R::theaterGripChord(c3,h3,g,(2100+10*i)*ms,false,2'000'000'000,&own);
            untouched=untouched&&g[0]==1.f&&g[1]==1.f&&!R::theaterChordStats.consuming.load();owned=owned||own!=0;}
        CHECK(untouched&&!owned&&R::theaterChordStats.consumed.load()==con&&R::theaterChordStats.unarmedPassed.load()==un+30,
              "chord unarmed: the hold ended with both grips closed: 30 samples pass untouched (no zero read as an open hand), counted");
        std::array<float,2> z{0.f,0.f};R::theaterGripChord(c3,h3,z,2500*ms,false);
        std::array<float,2> g2{.9f,.95f};std::uint8_t own2=0;R::theaterGripChord(c3,h3,g2,2510*ms,false,2'000'000'000,&own2);
        CHECK(g2[0]==0.f&&g2[1]==0.f&&own2==3&&R::theaterChordStats.consumed.load()==con+1,"chord: armed after a full release: the squeeze zeroed and owned (bits 0 and 1)");
        std::array<float,2> g3{.1f,1.f};std::uint8_t own3=0;R::theaterGripChord(c3,h3,g3,2520*ms,false,2'000'000'000,&own3);
        CHECK(g3[0]==0.f&&g3[1]==0.f&&own3==2,"chord: a partial release stays consumed until both are up; only the squeezed grip is owned");
        const auto f0=R::theaterChordStats.fired.load();std::array<float,2> zz{0.f,0.f};R::theaterGripChord(c3,h3,zz,2600*ms,false);bool fired=false;
        for(int i=0;i<=210;++i){std::array<float,2> g{1.f,1.f};fired=R::theaterGripChord(c3,h3,g,(2700+10*i)*ms,false)||fired;}
        CHECK(fired&&R::theaterChordStats.fired.load()==f0+1,"chord: both held 2 s after a full release fires once");}}
      {namespace gl=rdrvr::vr::grablink;gl::resetForTests();const std::uint64_t n=5000*ms;
       CHECK(!gl::grabBusy(n),"grab busy: nothing held, no candidate");
       gl::setHolding(1,true,n);CHECK(gl::grabBusy(n),"grab busy: a hand holds a prop");
       CHECK(gl::holdingAt(1,n+240*ms)&&!gl::holdingAt(1,n+260*ms)&&!gl::grabBusy(n+260*ms),"holding: a hold no tick saw for 250 ms is stale (the latches go back to the holsters)");
       gl::stampHolding(1,n+250*ms);CHECK(gl::holdingAt(1,n+400*ms),"holding: the tick's stamp keeps it fresh");
       gl::setHolding(1,false,0);CHECK(!gl::holdingAt(1,n)&&!gl::holding(1),"holding: released");gl::stampHolding(1,n);CHECK(!gl::holdingAt(1,n),"holding: a stamp never revives a released hold");
       gl::setReady(0,true,n);CHECK(gl::grabBusy(n+50*ms)&&!gl::grabBusy(n+150*ms),"grab busy: a fresh grab candidate (100 ms)");gl::setReady(0,false,n);
       gl::setJoin(1,true,n,.05f);CHECK(gl::grabBusy(n+20*ms),"grab busy: a join candidate");gl::resetForTests();
       CHECK(gl::latchNames[gl::LatchHeld]==std::string("held"),"latch: the held branch is named");}}
     // the drive: toward the hand's velocity, the horizontal cap min(900 N, M * 20), the lift cap min(M, 25 kg) * (g + 20), never down past M * 20
     {const auto f1=P::grabPropForce({1,0,0},{0,0,0},20.f,1.f/72.f,{0,1,0},.5f,900.f,20.f,25.f,1.f);
      CHECK(near(f1.f[0],400.f,1e-2f)&&f1.capped&&near(f1.f[1],0.f),"grab force: a 21 kg chair pulled at 1 m/s: 400 N across (M * 20 m/s2)");
      const auto f2=P::grabPropForce({.05f,0,0},{0,0,0},20.f,1.f/72.f,{0,1,0},.5f,900.f,20.f,25.f,1.f);
      CHECK(near(f2.f[0],.5f*20.f*.05f*72.f,1e-2f)&&!f2.capped,"grab force: a small error: gain * M * dv / dt, uncapped");
      const auto f3=P::grabPropForce({0,5,0},{0,0,0},40.f,1.f/72.f,{0,1,0},.5f,900.f,20.f,25.f,1.f);
      CHECK(near(f3.f[1],25.f*29.81f,1e-1f)&&f3.capped,"grab force: lifting a 40 kg table: at most 25 kg's weight and acceleration");
      const auto f4=P::grabPropForce({0,-5,0},{0,0,0},10.f,1.f/72.f,{0,1,0},.5f,900.f,20.f,25.f,1.f);
      CHECK(near(f4.f[1],-200.f,1e-2f),"grab force: down at most M * 20");
      CHECK(V::length(P::grabPropForce({1,0,0},{0,0,0},-1.f,1.f/72.f,{0,1,0},.5f,900.f,20.f,25.f,1.f).f)==0.f,"grab force: no mass: none");
      // GRAB LIGHT FF: a 0.35 kg cup on the velocity law alone (gain 0.5, 72 substeps per second, gravity) sinks g dt / gain = 0.273 m/s under
      // the hand (v16: 113 cm over 4.306 s); with its weight fed forward it stays (under 1 mm over 5 s); a hand step stays under the caps
      {const auto run=[](float ffKg,float seconds,float vWantY,float& vEnd){const float dt=1.f/72.f,m=.35f;float v=0.f,y=0.f;const int n=int(seconds*72.f+.5f);
           for(int i=0;i<n;++i){const auto f=P::grabPropForce({0,vWantY,0},{0,v,0},m,dt,{0,1,0},.5f,900.f,20.f,25.f,1.f,ffKg);v+=(f.f[1]/m-9.81f)*dt;y+=(v-vWantY)*dt;}
           vEnd=v;return y;};
       float v1{},v2{},v3{};const float sink=run(0.f,4.306f,0.f,v1),hold=run(.35f,5.f,0.f,v2);
       CHECK(near(-v1,.2725f,.003f)&&near(-sink,1.17f,.03f),"light ff: the velocity law alone sinks 0.273 m/s, 1.17 m over 4.306 s (the v16 cup: 1.13 m)");
       CHECK(std::fabs(hold)<.001f&&std::fabs(v2)<1e-3f,"light ff: the weight fed forward: under 1 mm in 5 s");
       const float step=run(.35f,1.f,.5f,v3);CHECK(std::isfinite(step)&&std::fabs(v3-.5f)<.01f&&std::fabs(step)<.02f,"light ff: a 0.5 m/s hand step followed, no runaway");
       const auto fc=P::grabPropForce({0,0,0},{0,0,0},.35f,1.f/72.f,{0,1,0},.5f,900.f,20.f,25.f,1.f,.35f);
       CHECK(near(fc.f[1],.35f*9.81f,1e-3f)&&!fc.capped,"light ff: at rest the force is the weight, under the up cap");
       const auto fb=P::grabPropForce({0,0,0},{0,0,0},40.f,1.f/72.f,{0,1,0},.5f,900.f,20.f,25.f,1.f,40.f);
       CHECK(fb.f[1]<=25.f*29.81f+1e-2f,"light ff: never past the up cap");}
      P::GrabMoveIn gm{};gm.on=gm.nav=gm.composite=gm.partsActive=true;gm.compD4=0;gm.massKg=21.25f;gm.radiusM=.78f;
      CHECK(P::grabMoveWhy(gm,40.f,1.f)==P::GrabMoveOk,"grab move: a 21 kg chair of 78 cm: may be released");
      auto gh=gm;gh.massKg=1044.f;auto gb=gm;gb.radiusM=1.31f;auto gd=gm;gd.door=true;auto gn=gm;gn.npcNear=true;auto gv=gm;gv.never=true;auto gc=gm;gc.compD4=1;
      CHECK(P::grabMoveWhy(gh,40.f,1.f)==P::GrabMoveHeavy&&P::grabMoveWhy(gb,40.f,1.f)==P::GrabMoveBig&&P::grabMoveWhy(gd,40.f,1.f)==P::GrabMoveClass&&
            P::grabMoveWhy(gn,40.f,1.f)==P::GrabMoveNpcNear&&P::grabMoveWhy(gv,40.f,1.f)==P::GrabMoveNever&&P::grabMoveWhy(gc,40.f,1.f)==P::GrabMoveBound,
            "grab move: heavy, big, a door, a creature near, never-activate, a BVH part");
      CHECK(P::holdBucket(100*ms)==0&&P::holdBucket(500*ms)==1&&P::holdBucket(2000*ms)==2&&P::holdBucket(5000*ms)==3&&P::holdBucket(20000*ms)==4&&
            P::lagBucket(.05f)==0&&P::lagBucket(.15f)==1&&P::lagBucket(.3f)==2&&P::lagBucket(.5f)==3&&P::lagBucket(.7f)==4,"grab buckets: hold length, lag");}
     P::VisualMem vm{};vm.h=.02f;const auto gv=P::grabVisual(vm,{1.1f,0,0},{1.f,0,0},identity,identity,.4f,.5f);
     CHECK(nearV(gv.offset,{.1f,0,0})&&vm.h==0.f&&V::length(gv.rot)==0.f,"grab visual: the hand at the solved body, the contact plane cleared");
     CHECK(nearV(P::grabVisual(vm,{2.f,0,0},{1.f,0,0},identity,identity,.4f,.5f).offset,{.4f,0,0}),"grab visual: capped at the leash");}
    // ---------- CPU per mode (pure) ----------
    {CHECK(P::cpuWindow(true,28*ms)==P::CpuCount&&P::cpuWindow(true,2*ms)==P::CpuShort&&P::cpuWindow(true,120*ms)==P::CpuLong&&P::cpuWindow(false,28*ms)==P::CpuModeChange,
           "cpu window: 5..100 ms frames of an unchanged mode");
     CHECK(P::cpuBucket(0)==0&&P::cpuBucket(2'600'000)==5&&P::cpuBucket(1'000'000'000ull)==P::cpuBuckets-1,"cpu bucket: 0.5 ms");
     std::uint64_t hb[P::cpuBuckets]{};hb[10]=50;hb[20]=40;hb[40]=10;
     CHECK(near(float(P::histPercentile(hb,P::cpuBuckets,.5,.5)),5.25f)&&near(float(P::histPercentile(hb,P::cpuBuckets,.5,.9)),10.25f)&&
           near(float(P::histPercentile(hb,P::cpuBuckets,.5,.99)),20.25f)&&P::histPercentile(hb,0,.5,.5)==0.0,"cpu percentiles from the histogram");}
    // ---------- props: the unlock predicate ----------
    {CHECK(P::unlockWanted(0,0,0,1,0),"unlock: an inactive prop with no override");
     CHECK(!P::unlockWanted(0x100,0,0,1,0)&&!P::unlockWanted(0x180,0,0,1,0),"unlock: never against an explicit override");
     CHECK(!P::unlockWanted(0,0x8,0,1,0)&&!P::unlockWanted(0,0,0x80,1,0)&&!P::unlockWanted(0,0,0,2,0)&&!P::unlockWanted(0,0,0,0,0)&&!P::unlockWanted(0,0,0,1,1),
           "unlock: never-activate, infinite mass, fixed, active, door: no");
     CHECK(P::unlockWanted(0x80,0x4,0x40,1,2),"unlock: other bits do not matter");}
    // ---------- NPC virtual body (pure) ----------
    {P::VBodies vb{};vb.count=1;auto& b=vb.b[0];b.guid=7;b.count=1;b.c[0]={{.1f,.9f,0},{.1f,1.5f,0},.06f,1,2};P::finishBody(b);
     CHECK(nearV(b.centre,{.1f,1.2f,0})&&near(b.radius,.36f,1e-4f),"virtual body: the sphere round its capsules");
     P::V3 c{.02f,1.2f,0};auto h=P::pushOut(c,c,identity,false,vb,0.f,1.f);
     CHECK(h.hit&&near(c[0],0.f,1e-4f)&&nearV(h.n,{-1,0,0},1e-4f)&&near(h.depth,.02f,1e-4f)&&h.cap==0&&h.kind==2&&nearV(h.point,{.04f,1.2f,0},1e-4f),
           "pushOut: the palm kept 5 mm outside the capsule, depth of the raw command");
     c={.02f,1.25f,0};P::pushOut(c,c,identity,false,vb,0.f,1.f);CHECK(near(c[1],1.25f)&&near(c[0],0.f,1e-4f),"pushOut: the part along the body kept");
     b.vel={-1,0,0};c={-.01f,1.2f,0};h=P::pushOut(c,c,identity,false,vb,.03f,1.f);
     CHECK(h.hit&&near(c[0],-.03f,1e-4f),"pushOut: a creature moving in pushes the hand out (capsule moved on by vel * age)");
     c={-.01f,1.2f,0};P::pushOut(c,c,identity,false,vb,1.f,1.f);CHECK(near(c[0],-.05f,1e-4f),"pushOut: the move capped at 50 ms");
     b.vel={0,0,0};c={1.2f,1.2f,0};h=P::pushOut(c,c,identity,false,vb,0.f,1.f);CHECK(!h.hit&&nearV(c,{1.2f,1.2f,0}),"pushOut: a body over 30 cm away skipped");
     c={.0f,.3f,0};P::Rows down{{{1,0,0},{0,-1,0},{0,0,-1}}};h=P::pushOut(c,c,down,false,vb,0.f,1.f);CHECK(!h.hit,"pushOut: below the capsule: free");
     c={.03f,1.05f,0};const P::V3 c0=c;h=P::pushOut(c,c,identity,true,vb,0.f,1.f);CHECK(h.hit&&c[0]<c0[0],"pushOut: the fist probe");
     c={.15f,1.2f,0};h=P::pushOut(c,{-.1f,1.2f,0},identity,false,vb,0.f,1.f);
     CHECK(h.hit&&near(c[0],0.f,1e-4f)&&nearV(h.n,{-1,0,0},1e-4f),"pushOut: a fast hand whose command is past the axis stops on the near side");
     c={-.05f,1.6f,0};h=P::pushOut(c,{-.05f,1.2f,0},identity,false,vb,0.f,1.f);CHECK(!h.hit&&nearV(c,{-.05f,1.6f,0}),"pushOut: a move that passes beside the capsule is not clipped");
     P::VBodies none{};c={.02f,1.2f,0};CHECK(!P::pushOut(c,c,identity,false,none,0.f,1.f).hit,"pushOut: no bodies");}
    // ---------- gates ----------
    {P::BumpGate g{};const std::uint64_t t=200'000*ms;bool e{},gt{};
     CHECK(g.update(9,true,t,&e,&gt)&&e&&!gt,"bump gate: a touch start sends, its edge");
     CHECK(!g.update(10,true,t+100*ms,&e,&gt)&&e&&gt,"bump gate: a second creature inside 250 ms: one edge, gated");
     CHECK(!g.update(10,true,t+128*ms,&e,&gt)&&!e&&!gt,"bump gate: the same touch the next tick: neither counted again");
     CHECK(g.update(10,true,t+300*ms,&e,&gt)&&!e,"bump gate: sent once the gate opens, no new edge");
     CHECK(!g.update(10,false,t+330*ms,&e,&gt)&&!e&&!g.seen(10),"bump gate: the touch ends");}
    {P::PressGate g{};const std::uint64_t t=300'000*ms;
     CHECK(g.update(4,true,t)==P::PressGate::Start&&g.update(4,true,t+28*ms)==P::PressGate::Send&&g.update(4,true,t+1900*ms)==P::PressGate::Send,"press gate: a bump every tick while pressing");
     CHECK(g.update(4,true,t+2000*ms)==P::PressGate::Rest&&g.update(4,true,t+2100*ms)==P::PressGate::None&&g.update(4,true,t+3400*ms)==P::PressGate::None,"press gate: 2 s, then a 1.5 s rest");
     CHECK(g.update(4,true,t+3600*ms)==P::PressGate::Start,"press gate: a held press starts again after the rest");
     CHECK(g.update(4,false,t+3700*ms)==P::PressGate::None&&g.update(4,true,t+3800*ms)==P::PressGate::Start,"press gate: a new press starts at once");}
    {P::SlapGate g{};const std::uint64_t t=400'000*ms;
     CHECK(g.allow(5,t)&&!g.allow(5,t+500*ms)&&!g.allow(6,t+100*ms)&&g.allow(6,t+400*ms)&&g.allow(5,t+900*ms),"slap gate: 800 ms per creature, 300 ms overall");}

    // ---------- install refusal ----------
    {const auto got=[](std::uintptr_t,std::uintptr_t){return true;};
     CHECK(!P::install(&stub::readZero,stub::base,got)&&std::strcmp(P::installState.load(),"ENGINE MISMATCH")==0&&!P::ready.load(),"a CORE print mismatch installs nothing");}

    // ---------- the behaviour list on a fake simulator ----------
    {stub::reset();const auto sim=stub::addr(stub::sim);
     alignas(16) static unsigned char inst[0x80]{};stub::put<std::uint16_t>(inst,0x10,0xffff);
     const void* beh=&P::objects[0].beh;
     CHECK(D::listAdd(sim,beh)&&stub::count()==3&&stub::behList[2]==stub::addr(beh)&&stub::at<std::uint8_t>(stub::sim,0x70)==1,"list add: appended, dirty");
     CHECK(D::listAdd(sim,beh)&&stub::count()==3,"list add twice: once");
     D::slotSet(sim,stub::addr(inst),beh);CHECK(stub::at<std::uint16_t>(inst,0x12)==0&&stub::behSlots[0]==0,"no slot for an instance outside the level");
     stub::put<std::uint16_t>(inst,0x10,5);D::slotSet(sim,stub::addr(inst),beh);
     CHECK(stub::behSlots[5]==stub::addr(beh)&&(stub::at<std::uint16_t>(inst,0x12)&0x100),"slot and flag 0x100 at the level index");
     D::slotClear(sim,stub::addr(inst),5,beh);CHECK(stub::behSlots[5]==0&&!(stub::at<std::uint16_t>(inst,0x12)&0x100),"ours cleared, flag off");
     stub::behList[3]=0x3333;stub::put<std::uint16_t>(stub::sim,0x72,4);
     D::listRemove(sim,beh);CHECK(stub::count()==3&&stub::behList[2]==0x3333&&stub::behList[0]==0x1111&&stub::behList[1]==0x2222,"list remove: the last entry fills the hole");}

    // ---------- whole ticks against the stub engine ----------
    stub::reset();stub::bind();P::resetForTests();P::modeForTests.store(P::ModeMeasure);
    frame({.5f,1.2f,0});
    CHECK(stub::count()==4&&!stub::adds,"MEASURE: both behaviours in the list, no bodies");
    P::modeForTests.store(P::ModeOn);
    // the PALM build
    {const auto& o=P::objects[1];const auto c=stub::addr(o.comp);
     CHECK(D::state().built&&D::state().builtShape==P::ShapePalm&&stub::boxCtors==6&&stub::compInits==2,"build: two composites of three boxes");
     const auto parts=stub::at<std::uintptr_t>(o.comp,0xa0),mats=stub::at<std::uintptr_t>(o.comp,0xa8);bool partsOk=true;
     for(unsigned i=0;i<3;++i){if(stub::at<std::uintptr_t>(reinterpret_cast<void*>(parts),i*8)!=stub::addr(o.box[i]))partsOk=false;
        float m[16];std::memcpy(m,reinterpret_cast<const void*>(mats+i*64),64);
        if(!nearV({m[12],m[13],m[14]},P::palmParts[i].offset)||m[0]!=1.f||m[5]!=1.f||m[10]!=1.f)partsOk=false;
        float sz[3];std::memcpy(sz,o.box[i]+0x110,12);if(!nearV({sz[0],sz[1],sz[2]},P::palmParts[i].size))partsOk=false;}
     CHECK(partsOk,"build: parts = the boxes, part matrices at their offsets, full box sizes");
     CHECK(stub::at<std::uintptr_t>(o.comp,0xc0)==stub::addr(o.partFlags)&&stub::at<std::uintptr_t>(o.comp,0xc8)==stub::addr(o.partFlags)&&
           o.partFlags[0][0]==~0u&&o.partFlags[1][1]==~0u&&o.partFlags[2][0]==0u,"build: part flags palm on, fingers on, fist off");
     CHECK(stub::at<std::uintptr_t>(o.arch,0x18)==c&&near(stub::at<float>(o.arch,0x50),2.f*4.f*P::palmInertiaPerKg[0])&&near(stub::at<float>(o.arch,0x48),30.f),
           "build: the composite is the bound, inertia m k I, max angular speed 30");
     CHECK((stub::at<std::uint16_t>(o.inst,0x12)&0x2)&&stub::at<std::uintptr_t>(o.inst,0)==stub::addr(&P::instTable[2])&&D::userOk(1),"build: CCD flag, the own vtable");
     CHECK(stub::at<std::uint32_t>(o.comp,0x9c)==0x40000u&&stub::at<std::uint32_t>(o.box[0],0x9c)==0x40000u,"build: reference counts pinned");}
    frame({.5f,1.2f,0});
    CHECK(stub::adds==1&&D::state().hand[1].present&&!D::state().hand[0].present,"ON: the right hand inserted, the left (no target) not");
    CHECK(stub::fnWord(stub::sim)==D::guardWord()&&!stub::addsUnguarded,"ON: preApplyGuard in the fn word before the first insert");
    for(int i=0;i<8;++i)frame({.5f,1.2f,0});
    {const auto s=solvedRight();CHECK(s.state==P::BodyLive&&nearV(s.solved,{.5f,1.2f,0},2e-3f)&&s.age==255,"free hand: the body reaches the target, live, no contact");}
    // a fast swing (12.5 m/s, twice the clamp): the body lags without a push - no contact, no park
    {const auto c0=P::stats.hand[1].contactSubsteps.load();
     for(int i=1;i<=8;++i)frame({.5f,1.2f,.35f*float(i)});
     CHECK(D::state().hand[1].devM>.15f&&D::state().hand[1].machine.state==P::BodyLive&&P::stats.hand[1].contactSubsteps.load()==c0,"fast swing: lags, stays live, no contact");
     for(int i=0;i<24;++i)frame({.5f,1.2f,0});}
    // the mover walking at 5 m/s
    {moverSpeedZ=5.f;for(int i=0;i<4;++i)frame({.5f,1.2f,0});
     CHECK(near(D::state().hand[1].moverVel[2],5.f,.3f),"mover velocity from the substep dt");
     moverSpeedZ=0.f;const float zero[3]{0,0,0};std::memcpy(stub::mover+P::off::moverPos,zero,12);
     for(int i=0;i<12;++i)frame({.5f,1.2f,0});
     CHECK(nearV(solvedRight().solved,{.5f,1.2f,0},2e-3f),"settled back");}
    // pressed 30 cm into the wall at x = 1 (body face at x + 14 mm): rests at the allowed penetration, no shaking
    {const auto s0=P::stats.substeps.load(),c0=P::stats.hand[1].contactSubsteps.load();
     for(int i=0;i<10;++i)frame({.5f+.08f*float(i),1.2f,0});
     std::vector<float> xs;const auto sA=P::stats.substeps.load(),cA=P::stats.hand[1].contactSubsteps.load();
     for(int i=0;i<36;++i){frame({1.3f,1.2f,0});xs.push_back(solvedRight().solved[0]);}
     float mn=1e9f,mx=-1e9f;for(float x:xs){mn=std::min(mn,x);mx=std::max(mx,x);}
     const float face=1.f-stub::halfDepth;
     std::printf("     wall press: body x %.4f..%.4f (face rest %.4f), contacts %llu of %llu substeps\n",double(mn),double(mx),double(face+stub::allowedPen),
        (unsigned long long)(P::stats.hand[1].contactSubsteps.load()-cA),(unsigned long long)(P::stats.substeps.load()-sA));
     CHECK(mn>=face+.0069f&&mx<=face+.0085f,"pressed 30 cm into the wall for 1 s: the body rests 7..8.5 mm deep");
     CHECK(P::stats.hand[1].contactSubsteps.load()-cA==P::stats.substeps.load()-sA,"contact every substep");
     CHECK(D::state().hand[1].machine.state==P::BodyLive&&P::stats.hand[1].creepClips.load()>0,"stays live (30 cm < the 40 cm leash), the drive creeps");
     const auto s=solvedRight();CHECK(nearV(s.n,{-1,0,0},1e-3f)&&s.age<=1&&near(s.plane,-s.solved[0],1e-5f)&&P::fresh(rdrvr::vr::nowNs(),s.contactNs,80*ms),"published: n, plane, age, contact clock");
     CHECK(P::stats.hand[1].normalFlips.load()==0,"no normal flips");(void)s0;(void)c0;}
    // pressed 30 cm into the wall, slid 70 cm along it at 1 m/s and back: the body slides with the hand, stays live and in contact
    {const auto p0=P::stats.hand[1].parkGhosts.load();float lag=0.f;
     for(int i=1;i<=25;++i){frame({1.3f,1.2f,.028f*float(i)});lag=std::max(lag,std::fabs(solvedRight().solved[2]-.028f*float(i)));}
     const auto s=solvedRight();
     std::printf("     wall slide: pressed 30 cm, 1 m/s along the wall: max lag %.1f cm, body x %.4f\n",double(lag*100.f),double(s.solved[0]));
     CHECK(lag<.01f&&D::state().hand[1].machine.state==P::BodyLive&&s.age<=1&&P::stats.hand[1].parkGhosts.load()==p0,"slide along the wall pressed 30 cm: follows within 1 cm, live, in contact");
     for(int i=24;i>=0;--i)frame({1.3f,1.2f,.028f*float(i)});
     CHECK(D::state().hand[1].machine.state==P::BodyLive&&near(solvedRight().solved[2],0.f,.01f),"slid back");}
    // a door met by a kept pair while pressing: Open; a locked one counted
    {auto& dp=stub::otherProp;stub::placeProp(dp,9,stub::navVtable,{1.1f,1.2f,0},.5f,1,true);
     const bool kept=D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,dp.inst,nullptr,nullptr);
     CHECK(kept&&D::state().hand[1].door.inst==stub::addr(dp.inst),"a door pair is kept and remembered");
     frame({1.3f,1.2f,0});
     CHECK(stub::doorOpens==1&&P::stats.doors.opens.load()==1,"pressing into a closed door: Open");
     D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,dp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});
     CHECK(stub::doorOpens==1&&P::stats.doors.gated.load()==1,"the same door again inside 1 s: gated");
     stub::doorStateValue=1;simNs+=1100*ms;D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,dp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});
     CHECK(P::stats.doors.locked.load()==1&&stub::doorOpens==1,"a locked door: counted, not opened");
     stub::doorStateValue=4;D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,dp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});
     CHECK(P::stats.doors.opened.load()==1&&!stub::doorCloses,"an open door: counted, not closed (doorclose 0)");
     stub::doorStateValue=0;stub::clearProps();}
    // DOOR SWING (lever doorfollow 0): a pivot door whose leaf is the wall plane x = 1, its hinge 80 cm to the side at floor height (1.44 m from the palm)
    {auto& dp=stub::otherProp;D::state().doors=P::DoorGate{};P::leverPatchForTests.store(&swingDoors);D::state().leversRead=false;
     const auto setupDoor=[&](std::uintptr_t vptr,std::uint32_t state){
        stub::placeProp(dp,9,stub::navVtable,{1.f,0.f,-.8f},.5f,1,true);
        const float rows[12]{0,0,1,0, 0,1,0,0, 1,0,0,0};std::memcpy(dp.inst+0x20,rows,48);
        static unsigned char bound[0x60]{};const float mx[4]{.9f,2.1f,.03f,0},mn[4]{0,0,-.03f,0};std::memcpy(bound+0x30,mx,16);std::memcpy(bound+0x40,mn,16);
        stub::put<std::uintptr_t>(dp.arch,0x18,stub::addr(bound));
        std::memset(stub::doorInst,0,sizeof stub::doorInst);std::memcpy(stub::doorInst+0x20,rows+8,12);const float org[3]{1.f,0.f,-.8f};std::memcpy(stub::doorInst+0x30,org,12);
        std::memset(stub::doorIface,0,sizeof stub::doorIface);stub::put<std::uintptr_t>(stub::doorIface,0,vptr);
        stub::put<float>(stub::doorIface,0x20,1.f);stub::put<float>(stub::doorIface,0x28,-1.f);stub::put<float>(stub::doorIface,0x2c,-1.f);
        stub::put<std::uintptr_t>(stub::doorInst,0x50,stub::addr(stub::doorIface));stub::doorStateValue=state;
        D::state().hand[0].door={};D::state().hand[1].door={};D::state().holds={};};
     const auto ifSpeed=[]{return stub::at<float>(stub::doorIface,0x20);};const auto ifT=[]{return stub::at<float>(stub::doorIface,0x24);};
     const auto pressDoor=[&](P::V3 at){D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,dp.inst,nullptr,nullptr);frame(at);stub::doorUpdate(.028f);};
     const auto away=[&](int n){for(int i=0;i<n;++i){frame({.5f,1.2f,0});stub::doorUpdate(.028f);}};
     auto& Ds=P::stats.doors;
     setupDoor(stub::base+P::off::pivotVptr,0);for(int i=0;i<4;++i)frame({1.02f,1.2f,0});
     const int o0=stub::doorOpens;const auto far0=Ds.far.load(),h0=Ds.swingHolds.load();
     pressDoor({1.02f,1.2f,0});
     CHECK(stub::doorOpens==o0+1&&Ds.swingHolds.load()==h0+1&&Ds.far.load()==far0&&stub::doorStateValue==2,"swing: a door whose hinge origin is 1.44 m from the palm opens and is held");
     CHECK(near(stub::lastPusher[0],solvedRight().solved[0],.01f)&&near(stub::lastPusher[1],1.2f,.01f)&&ifSpeed()==0.f,"swing: Open gets the palm as the pusher, then speed 0");
     CHECK(D::state().holds[0].active&&D::state().holds[0].dir&&near(D::state().holds[0].savedSpeed,.9f),"swing: the hold keeps StartOpen's speed and the side");
     const auto w0=Ds.phaseWrites.load();for(int i=0;i<6;++i)pressDoor({1.02f,1.2f,0});
     const auto& hd=D::state().holds[0];
     CHECK(Ds.phaseWrites.load()>=w0+4&&ifT()>0.f&&near(P::easeOpen(ifT())*1.6f,hd.swing.theta,2e-3f)&&hd.swing.theta>.1f,"swing: the palm's push writes the phase, the leaf follows");
     for(int i=0;i<14;++i)pressDoor({1.02f,1.2f,0});
     CHECK(hd.swing.theta>.35f*1.6f,"swing: pushed past a third open");
     const auto f0=Ds.finishes.load();away(70);
     CHECK(Ds.finishes.load()==f0+1&&!D::state().holds[0].active&&ifSpeed()==.9f,"swing: released past a third: the speed back, the engine finishes it");
     for(int i=0;i<12;++i)stub::doorUpdate(.028f);CHECK(stub::doorStateValue==4,"swing: then fully open (state 4)");
     // a short push, released under a third: glides shut, FastClose
     setupDoor(stub::base+P::off::pivotVptr,0);simNs+=1100*ms;for(int i=0;i<4;++i)frame({1.02f,1.2f,0});
     for(int i=0;i<2;++i)pressDoor({1.02f,1.2f,0});
     const auto g0=Ds.glides.load(),c0=Ds.fastCloses.load();away(90);
     CHECK(Ds.glides.load()==g0+1&&Ds.fastCloses.load()==c0+1&&stub::doorFastCloses>=1&&stub::doorStateValue==0&&!D::state().holds[0].active,"swing: released under a third: glides shut, FastClose");
     // the game takes the door (a speed written): dropped without a write
     setupDoor(stub::base+P::off::pivotVptr,0);simNs+=1100*ms;for(int i=0;i<4;++i)frame({1.02f,1.2f,0});
     for(int i=0;i<3;++i)pressDoor({1.02f,1.2f,0});
     stub::put<float>(stub::doorIface,0x20,.5f);const float tk=ifT();const auto dg=Ds.dropsGame.load();pressDoor({1.02f,1.2f,0});
     CHECK(Ds.dropsGame.load()==dg+1&&!D::state().holds[0].active&&ifSpeed()==.5f,"swing: a speed the game wrote: the hold dropped, nothing written");
     (void)tk;
     // lever doorpress on, the follow off: a hand near the door without a contact (a pair 10 cm off the leaf) never opens it
     {away(10);setupDoor(stub::base+P::off::pivotVptr,0);simNs+=1100*ms;for(int i=0;i<4;++i)frame({.88f,1.2f,0});
      const int o0=stub::doorOpens;const auto pc0=Ds.pressCands.load();
      pressDoor({.88f,1.2f,0});for(int i=0;i<6;++i){frame({.88f,1.2f,0});stub::doorUpdate(.028f);}
      CHECK(stub::doorOpens==o0&&!D::state().holds[0].active&&Ds.pressCands.load()==pc0&&D::state().lev.doorPress,
            "swing (doorfollow 0, doorpress 1): a pair 10 cm off the leaf, no contact: not opened, no press candidates");
      simNs+=1100*ms;}
     (void)tk;
     // a door opening by itself (state 2, the use button): taken over
     setupDoor(stub::base+P::off::pivotVptr,2);simNs+=1100*ms;stub::put<float>(stub::doorIface,0x20,.9f);stub::put<float>(stub::doorIface,0x24,.3f);stub::put<float>(stub::doorIface,0x28,1.6f);
     for(int i=0;i<4;++i)frame({1.02f,1.2f,0});
     const auto tk0=Ds.takeovers.load();pressDoor({1.02f,1.2f,0});
     CHECK(Ds.takeovers.load()==tk0+1&&ifSpeed()==0.f&&near(D::state().holds[0].swing.theta,P::easeOpen(.3f)*1.6f,1e-3f),"swing: an opening door taken over at its angle");
     away(25);
     // a door the game was opening, taken over under a third open: released, the engine finishes opening it (no glide shut)
     setupDoor(stub::base+P::off::pivotVptr,2);simNs+=1100*ms;stub::put<float>(stub::doorIface,0x20,.9f);stub::put<float>(stub::doorIface,0x24,.05f);stub::put<float>(stub::doorIface,0x28,1.6f);
     for(int i=0;i<4;++i)frame({1.02f,1.2f,0});
     {const auto tk1=Ds.takeovers.load();pressDoor({1.02f,1.2f,0});
      CHECK(Ds.takeovers.load()==tk1+1&&D::state().holds[0].swing.keepOpen&&D::state().holds[0].swing.theta<.35f*1.6f,"swing: a slow opening door taken over under a third");
      const auto f1=Ds.finishes.load(),g1=Ds.glides.load();away(90);
      CHECK(Ds.finishes.load()==f1+1&&Ds.glides.load()==g1&&!D::state().holds[0].active&&ifSpeed()==.9f,"swing: a taken-over opening door released: back to the engine, no glide");}
     // an anim door: a plain Open, no hold
     setupDoor(stub::base+P::off::animVptr,0);simNs+=1100*ms;for(int i=0;i<4;++i)frame({1.02f,1.2f,0});
     const int o1=stub::doorOpens;const auto a0=Ds.animOpens.load();pressDoor({1.02f,1.2f,0});
     CHECK(stub::doorOpens==o1+1&&Ds.animOpens.load()==a0+1&&!D::state().holds[0].active,"swing: an anim door gets a plain Open");
     // locked: no Open
     setupDoor(stub::base+P::off::pivotVptr,1);simNs+=1100*ms;const int o2=stub::doorOpens;const auto l0=Ds.locked.load();pressDoor({1.02f,1.2f,0});
     CHECK(stub::doorOpens==o2&&Ds.locked.load()==l0+1,"swing: a locked door: no Open");
     // the hand's body removed while it pushes (MEASURE): no push left behind, the door is not driven or taken over again
     setupDoor(stub::base+P::off::pivotVptr,0);simNs+=1100*ms;for(int i=0;i<4;++i)frame({1.02f,1.2f,0});
     for(int i=0;i<3;++i)pressDoor({1.02f,1.2f,0});
     {CHECK(D::state().holds[0].active,"removed while pushing: the door is held first");
      P::modeForTests.store(P::ModeMeasure);const auto tk2=Ds.takeovers.load(),pw2=Ds.phaseWrites.load(),sh2=Ds.swingHolds.load();const int o4=stub::doorOpens;
      for(int i=0;i<10;++i){frame({1.02f,1.2f,0});stub::doorUpdate(.028f);}
      CHECK(!D::state().hand[1].present&&!D::state().holds[0].active&&!D::state().holds[1].active&&Ds.takeovers.load()==tk2&&Ds.phaseWrites.load()==pw2&&
            Ds.swingHolds.load()==sh2&&stub::doorOpens==o4,"removed while pushing: no stale push drives the door");
      P::modeForTests.store(P::ModeOn);for(int i=0;i<12;++i)frame({1.02f,1.2f,0});}
     // SWING refused: the plain Open with the leaf gate and the palm
     setupDoor(stub::base+P::off::pivotVptr,0);simNs+=1100*ms;P::groupOk[P::GroupSwing]=false;
     const int o3=stub::doorOpens;const auto p0=Ds.plainOpens.load();pressDoor({1.02f,1.2f,0});
     CHECK(stub::doorOpens==o3+1&&Ds.plainOpens.load()==p0+1&&!D::state().holds[0].active,"swing refused: a plain Open from the palm");
     P::groupOk[P::GroupSwing]=true;
     stub::put<std::uintptr_t>(stub::doorInst,0x50,0);stub::doorStateValue=0;stub::clearProps();for(int i=0;i<4;++i)frame({1.3f,1.2f,0});
     P::leverPatchForTests.store(nullptr);D::state().leversRead=false;}
    // ---------- DOORS: the leaf follow (a stub pivot door whose leaf is posed from its phase; a wall on the leaf's current plane) ----------
    {auto& dp=stub::otherProp;D::state().doors=P::DoorGate{};auto& Ds=P::stats.doors;
     static const auto noBody=+[](P::Levers& l){l.doorBody=false;};P::leverPatchForTests.store(noBody);D::state().leversRead=false;
     static unsigned char leafBound[0x60]{};
     const auto setup=[&](std::uint32_t state,float t,float cap,std::uint8_t side,float speed){
        stub::placeProp(dp,9,stub::navVtable,{1.f,0.f,-.8f},.5f,1,true);
        const float rows[12]{0,0,1,0, 0,1,0,0, 1,0,0,0};std::memcpy(dp.inst+0x20,rows,48);
        const float mx[4]{.9f,2.1f,.03f,0},mn[4]{0,0,-.03f,0};std::memcpy(leafBound+0x30,mx,16);std::memcpy(leafBound+0x40,mn,16);
        stub::put<std::uintptr_t>(dp.arch,0x18,stub::addr(leafBound));
        std::memset(stub::doorInst,0,sizeof stub::doorInst);std::memcpy(stub::doorInst,rows,48);const float org[3]{1.f,0.f,-.8f};std::memcpy(stub::doorInst+0x30,org,12);
        std::memset(stub::doorIface,0,sizeof stub::doorIface);stub::put<std::uintptr_t>(stub::doorIface,0,stub::base+P::off::pivotVptr);
        stub::put<std::uint8_t>(stub::doorIface,8,side);stub::put<float>(stub::doorIface,0x20,speed);stub::put<float>(stub::doorIface,0x24,t);
        stub::put<float>(stub::doorIface,0x28,cap);stub::put<float>(stub::doorIface,0x2c,cap);
        stub::put<std::uintptr_t>(stub::doorInst,0x50,stub::addr(stub::doorIface));stub::put<std::uintptr_t>(stub::doorInst,0x78,stub::addr(dp.inst));
        stub::doorStateValue=state;stub::put<std::uint32_t>(stub::doorInst,0x58,state);stub::poseLeaf=true;stub::poseNav();
        D::state().hand[0].door={};D::state().hand[1].door={};D::state().holds={};D::state().rested={};};
     const auto ifSpeed=[]{return stub::at<float>(stub::doorIface,0x20);};const auto ifT=[]{return stub::at<float>(stub::doorIface,0x24);};
     const P::V3 o{1.f,0.f,-.8f},up{0,1.2f,0};
     const auto restedAny=[]{for(const auto& r:D::state().rested)if(r.door==stub::addr(stub::doorInst))return true;return false;};
     // the command on the leaf at angle phi, `into` metres past its face from the hand's side (side -1: the closed side, +1: the open side)
     const auto arc=[&](float phi,float into,int side){const P::V3 L{std::sin(phi),0,std::cos(phi)},c{std::cos(phi),0,-std::sin(phi)};
        return V::add(V::add(o,up),V::add(V::mul(L,.8f),V::mul(c,float(-side)*into)));};
     const auto step=[&](P::V3 at,bool pair){if(pair)D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,dp.inst,nullptr,nullptr);frame(at);stub::doorUpdate(.028f);};
     // the player's mover (the hand targets follow it: anchors are given in the world here)
     P::V3 moverAt{0,0,0};
     const auto setMover=[&](P::V3 p){moverAt=p;const float f3[3]{p[0],p[1],p[2]};std::memcpy(stub::mover+P::off::moverPos,f3,12);};
     const auto W=[&](P::V3 world){return V::sub(world,moverAt);};
     const auto fw=[&](P::V3 world){frame(W(world));stub::doorUpdate(.028f);};
     const auto stepW=[&](P::V3 world,bool pair){if(pair)D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,dp.inst,nullptr,nullptr);frame(W(world));stub::doorUpdate(.028f);};
     const auto away=[&](int n){for(int i=0;i<n;++i)fw({.5f,1.2f,0});};
     wallOn=false;
     // a closed door pushed beside its leaf (the frame / the wall in its plane): no door push at all (sampled with the part's gap)
     setup(0,0.f,-1.f,0,1.f);wallOn=true;for(int i=0;i<4;++i)frame({1.02f,1.2f,.4f});
     {const int o0=stub::doorOpens;const auto r0=Ds.refusedGap[6].load();step({1.02f,1.2f,.4f},true);
      CHECK(stub::doorOpens==o0&&Ds.refusedGap[6].load()==r0+1&&!D::state().holds[0].active&&D::state().doorRejects[0].nav,
            "follow: a hand on the wall beside the leaf: no Open, refused (no part on the leaf), sampled");}
     wallOn=false;stub::doorStateValue=0;for(int i=0;i<4;++i)frame({.5f,1.2f,0});
     // the palm on the closed leaf: the engine's own Open, no hold; the game's opening (2, 3) is never touched while the palm stays on it
     simNs+=2000*ms;setup(0,0.f,-1.f,0,1.f);leafSide=-1;for(int i=0;i<6;++i)frame(arc(0.f,.02f,-1));
     {const int o0=stub::doorOpens;const auto e0=Ds.events.load();step(arc(0.f,.02f,-1),true);
      CHECK(stub::doorOpens==o0+1&&!D::state().holds[0].active&&stub::doorStateValue==2&&Ds.events.load()==e0+1&&D::state().doorEvents[(D::state().doorEventNext+15)%16].end==P::DoorEndOpen,
            "follow: the palm on the closed leaf, its limit still pending: the engine's Open left to finish, no hold, an open event");
      const auto w0=Ds.phaseWrites.load(),b0=Ds.engineBusy.load();const int k0=stub::continueCalls,s0=stub::setStates;
      for(int i=0;i<60&&stub::doorStateValue!=4;++i)step(arc(0.f,.02f,-1),true);
      CHECK(stub::doorStateValue==4&&Ds.phaseWrites.load()==w0&&stub::continueCalls==k0&&stub::setStates==s0&&Ds.engineBusy.load()>b0&&ifSpeed()==.9f,
            "follow: the game's walk-in opening runs as stock (states 2 / 3 never written, its speed kept)");}
     // an open door pushed shut from its open face while the mover stands in its closing wedge (15 deg from the closed leaf, 0.6 m from
     // the hinge): a POSE4 hold, the leaf follows down to where it meets him (15 + asin(0.35 / 0.6) = 50.7 deg) and no further, no Close
     leafSide=0;away(6);simNs+=2000*ms;
     const auto moverAtDeg=[&](float deg,float r){const float a=deg*P::pi/180.f;return P::V3{1.f+r*std::sin(a),0.f,-.8f+r*std::cos(a)};};
     // the mover walked (0.1 m a frame, 3.6 m/s: the hand keeps up), the hand kept beside it, then settled
     const auto walkMover=[&](P::V3 to){for(int i=0;i<120;++i){const P::V3 d=V::sub(to,moverAt);const float l=V::length(d);if(l<1e-4f)break;
        setMover(l>.1f?V::add(moverAt,V::mul(d,.1f/l)):to);frame({.5f,1.2f,0});stub::doorUpdate(.028f);}
        for(int i=0;i<4;++i){frame({.5f,1.2f,0});stub::doorUpdate(.028f);}};
     {walkMover(moverAtDeg(15.f,.6f));setup(4,1.f,1.6f,1,1.f);const float th=P::easeOpen(1.f)*1.6f;
      for(int i=0;i<8;++i)fw(arc(th,-.30f,1));leafSide=1;for(int i=0;i<4;++i)fw(arc(th,-.05f,1));
      const auto p0=Ds.pose4Holds.load();const int c0=stub::doorCloses;stepW(arc(th,.02f,1),true);stepW(arc(th,.02f,1),true);
      CHECK(Ds.pose4Holds.load()==p0+1&&D::state().holds[0].active&&D::state().holds[0].mode==P::HoldPose4&&D::state().holds[0].q>0,"follow: an open door's open face: a POSE4 hold (q > 0)");
      float phi=th;const auto f0=Ds.floorHolds.load(); // the hand stops 13 deg past the stop (under the 45 cm that parks it)
      for(int i=0;i<40;++i){phi=std::max(.66f,phi-.06f);stepW(arc(phi,.02f,1),false);}
      const float deg=D::state().holds[0].theta*180.f/P::pi;
      CHECK(D::state().holds[0].active&&deg>49.5f&&deg<52.f&&Ds.floorHolds.load()>f0&&stub::doorCloses==c0&&stub::doorStateValue==4,
            "follow: the mover in the closing wedge: the leaf stops where it meets him (50.7 deg), no Close");
      // released: it rests at that angle; over 45 deg: no raise
      const auto r0=Ds.rests.load(),x0=Ds.raises.load();leafSide=0;away(20);
      CHECK(Ds.rests.load()==r0+1&&Ds.raises.load()==x0&&stub::doorStateValue==4&&restedAny(),"follow: released part-open: rested at its angle, no raise over 45 deg");}
     // the mover on the open side past the leaf (60 deg, 0.8 m: the face the closing leaf moves away from): pushed down to about 15 deg,
     // nothing stops it; released there, the mover is in the wedge the raise would sweep: no raise; walked behind the leaf (-40 deg): posed
     // to 45 deg once
     {walkMover(moverAtDeg(60.f,.8f));away(4);const float th=P::easeOpen(ifT())*1.6f;leafSide=0;for(int i=0;i<8;++i)fw(arc(th,-.30f,1));leafSide=1;for(int i=0;i<4;++i)fw(arc(th,-.05f,1));
      const int c1=stub::doorCloses;const auto f1=Ds.floorHolds.load();stepW(arc(th,.02f,1),true);stepW(arc(th,.02f,1),true);float phi=th;
      for(int i=0;i<30&&phi>.35f;++i){phi=std::max(.35f,phi-.06f);stepW(arc(phi,.02f,1),false);}
      for(int i=0;i<4;++i)stepW(arc(.35f,.02f,1),false);
      const float deg=P::easeOpen(ifT())*1.6f*180.f/P::pi;
      CHECK(deg>10.f&&deg<21.f&&stub::doorStateValue==4&&stub::doorCloses==c1&&Ds.floorHolds.load()==f1,"follow: a mover on the face the leaf moves away from: nothing stops the push");
      const auto x0=Ds.raises.load(),b0=Ds.raiseBlocked.load();leafSide=0;away(20);
      CHECK(restedAny()&&Ds.raises.load()==x0&&Ds.raiseBlocked.load()>b0,"raise: rested under 45 deg with the mover in the wedge the raise would sweep: blocked");
      walkMover(moverAtDeg(-40.f,.8f));for(int i=0;i<4;++i){frame({.5f,1.2f,0});stub::doorUpdate(.028f);}
      const float t45=P::phaseFor(45.f*P::pi/180.f/1.6f,false);
      CHECK(Ds.raises.load()==x0+1&&near(ifT(),t45,1e-4f)&&stub::doorStateValue==4,"raise: the mover out of that wedge: posed to 45 deg (state 4 cannot be walked open)");
      for(int i=0;i<10;++i){frame({.5f,1.2f,0});stub::doorUpdate(.028f);}
      CHECK(Ds.raises.load()==x0+1&&near(ifT(),t45,1e-4f),"raise: once, then it stays");}
     walkMover({0,0,0});walkMover({-1.f,0,0});
     // the mover out of the disc: the same door pushed shut, Close once at 2 deg; the same push cannot reopen it
     {setMover({-1.f,0,0});away(4);const float th=P::easeOpen(ifT())*1.6f;leafSide=0;for(int i=0;i<8;++i)fw(arc(th,-.30f,1));leafSide=1;for(int i=0;i<4;++i)fw(arc(th,-.05f,1));
      const int c0=stub::doorCloses,k0=stub::continueCalls;const auto s0=Ds.shutsPose4.load();
      stepW(arc(th,.02f,1),true);stepW(arc(th,.02f,1),true);float phi=th;
      for(int i=0;i<30&&stub::doorStateValue==4;++i){phi=std::max(0.f,phi-.06f);stepW(arc(phi,.02f,1),false);}
      CHECK(stub::continueCalls>k0+4&&Ds.pose4Moves.load()>=4,"follow: POSE4 moves the leaf through ContinueActionShared");
      CHECK(Ds.shutsPose4.load()==s0+1&&stub::doorCloses==c0+1&&(stub::doorStateValue==5||stub::doorStateValue==0),"follow: the mover outside the disc: pushed shut, Close once at 2 deg");
      for(int i=0;i<6;++i)fw(arc(0.f,.02f,1));
      const auto rg0=Ds.rearmGated.load();stepW(arc(0.f,.02f,1),true);
      CHECK(stub::doorStateValue==0&&stub::doorCloses==c0+1&&Ds.rearmGated.load()>=rg0,"follow: closed by the engine, the same push does not reopen it");}
     // one hold follows the hand to either face: the hand goes round the edge, the leaf turns the other way with it
     leafSide=0;away(6);simNs+=2000*ms;
     {setup(4,.4f,1.6f,1,1.f);const float th=P::easeOpen(.4f)*1.6f;
      for(int i=0;i<8;++i)fw(arc(th,-.30f,1));leafSide=1;for(int i=0;i<4;++i)fw(arc(th,-.05f,1));
      stepW(arc(th,.02f,1),true);stepW(arc(th,.02f,1),true);
      CHECK(D::state().holds[0].active&&D::state().holds[0].q>0,"side: a POSE4 hold from the open face");
      const auto f0=Ds.sideFlips.load();const float th0=D::state().holds[0].theta;
      leafSide=0;for(int i=0;i<3;++i)stepW(arc(th0,-.12f,-1),false);leafSide=-1;
      float phi=th0;for(int i=0;i<10;++i){phi+=.05f;stepW(arc(phi,.02f,-1),false);}
      CHECK(D::state().holds[0].active&&Ds.sideFlips.load()==f0+1&&D::state().holds[0].q<0&&D::state().holds[0].theta>th0+.2f,"side: the hand on the other face: the same hold opens the leaf with it");
      leafSide=0;away(20);}
     // a door the game is opening: a push on it does nothing (never a takeover)
     simNs+=2000*ms;
     {setup(2,.3f,1.6f,1,.9f);const float th=P::easeOpen(.3f)*1.6f;for(int i=0;i<4;++i)frame(W(arc(th,-.30f,1)));leafSide=1;for(int i=0;i<4;++i)frame(W(arc(th,-.05f,1)));
      const auto w0=Ds.phaseWrites.load(),b0=Ds.engineBusy.load();const float sp=ifSpeed();
      for(int i=0;i<3;++i){D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,dp.inst,nullptr,nullptr);frame(W(arc(th,.02f,1)));}
      CHECK(!D::state().holds[0].active&&Ds.phaseWrites.load()==w0&&ifSpeed()==sp&&Ds.engineBusy.load()>b0,"follow: a door the game is opening: never taken over");
      leafSide=0;away(40);}
     // the game takes a held door (another state): the hold dropped, nothing written
     simNs+=2000*ms;
     {setup(4,.5f,1.6f,1,1.f);const float th=P::easeOpen(.5f)*1.6f;for(int i=0;i<8;++i)fw(arc(th,-.30f,1));leafSide=1;for(int i=0;i<4;++i)fw(arc(th,-.05f,1));
      stepW(arc(th,.02f,1),true);stepW(arc(th,.02f,1),true);float phi=th;for(int i=0;i<3;++i){phi-=.06f;stepW(arc(phi,.02f,1),false);}
      CHECK(D::state().holds[0].active,"follow: held before the game takes it");
      stub::doorStateValue=5;stub::put<std::uint32_t>(stub::doorInst,0x58,5u);const float tk=ifT();const auto g0=Ds.dropsGame.load(),w0=Ds.phaseWrites.load();
      frame(W(arc(phi-.06f,.02f,1)));
      CHECK(Ds.dropsGame.load()==g0+1&&Ds.phaseWrites.load()==w0&&ifT()==tk&&!D::state().holds[0].active&&D::state().doorEvents[(D::state().doorEventNext+15)%16].end==P::DoorEndGame,
            "follow: the game changed the state: the hold dropped, nothing written, a game event");
      leafSide=0;away(20);}
     // the game's own close of a rested door: its phase remapped once (no jump)
     simNs+=2000*ms;
     {setup(4,.6f,1.6f,1,1.f);const float th=P::easeOpen(.6f)*1.6f;for(int i=0;i<8;++i)fw(arc(th,-.30f,1));leafSide=1;for(int i=0;i<4;++i)fw(arc(th,-.05f,1));
      stepW(arc(th,.02f,1),true);stepW(arc(th,.02f,1),true);float phi=th;for(int i=0;i<4;++i){phi-=.06f;stepW(arc(phi,.02f,1),false);}
      leafSide=0;away(20);
      CHECK(stub::doorStateValue==4&&restedAny(),"remap: a rested door");
      const float tl=ifT();const auto m0=Ds.restRemaps.load();
      stub::doorClose(stub::doorInst,false);frame(W({.5f,1.2f,0}));
      CHECK(Ds.restRemaps.load()==m0+1&&near(ifT(),P::restRemapPhase(tl),1e-4f)&&!restedAny(),"remap: the game's Close of a rested door gets the closing phase of its angle once");
      for(int i=0;i<4;++i){stub::doorUpdate(.028f);frame(W({.5f,1.2f,0}));}
      CHECK(Ds.restRemaps.load()==m0+1,"remap: once");
      for(int i=0;i<20;++i){stub::doorUpdate(.028f);frame(W({.5f,1.2f,0}));}}
     // lever doorrest 0: an open door is left to the engine (no hold)
     {static const auto noRest=+[](P::Levers& l){l.doorRest=false;l.doorBody=false;};P::leverPatchForTests.store(noRest);D::state().leversRead=false;
      away(4);simNs+=2000*ms;setup(4,.6f,1.6f,1,1.f);const float th=P::easeOpen(.6f)*1.6f;for(int i=0;i<8;++i)fw(arc(th,-.30f,1));leafSide=1;for(int i=0;i<4;++i)fw(arc(th,-.05f,1));
      const auto p0=Ds.pose4Holds.load();stepW(arc(th,.02f,1),true);stepW(arc(th,.02f,1),true);
      CHECK(Ds.pose4Holds.load()==p0&&!D::state().holds[0].active,"doorrest 0: an open door is not held");
      P::leverPatchForTests.store(noBody);D::state().leversRead=false;leafSide=0;away(4);}
     // the report carries the event ring
     {const auto r=P::report();CHECK(r.find("\"eventRing\":[{")!=std::string::npos&&r.find("\"refusedGapCm\":{")!=std::string::npos,"doors: the report's event ring and refused gaps");}
     // ---------- DOORS: a hand on a closed leaf opens it posed (its limit resolved by the first ContinueActionShared) ----------
     {setMover({-1.f,0,0});away(4);simNs+=2000*ms;setup(0,0.f,-1.f,0,1.f);stub::capOnContinue=true;leafSide=-1;for(int i=0;i<6;++i)fw(arc(0.f,.02f,-1));
      const auto cp0=Ds.closedPoses.load(),p0=Ds.pose4Holds.load();const int pc0=stub::pathCalls,o0=stub::doorOpens;
      stepW(arc(0.f,.02f,-1),true);
      CHECK(Ds.closedPoses.load()==cp0+1&&stub::doorOpens==o0+1&&stub::doorStateValue==4&&stub::lastSetState==4&&stub::pathCalls==pc0+1&&
            Ds.pose4Holds.load()==p0+1&&D::state().holds[0].active&&D::state().holds[0].mode==P::HoldPose4,
            "hand on a closed leaf: Open, the limit, the phase, SetState(4), the path state: a POSE4 hold at the hand");
      CHECK(near(stub::at<float>(stub::doorInst,0x84),510.f,1e-3f)&&stub::at<std::uint8_t>(stub::doorInst,0xa0)==0&&ifT()>0.f&&ifT()<.8f,
            "hand on a closed leaf: the engine's own 2 -> 4 step (+0x84 = clock + 10, +0xa0 = 0), an opening phase");
      float phi=D::state().holds[0].theta;const float th0=phi;for(int i=0;i<10;++i){phi+=.05f;stepW(arc(phi,.02f,-1),false);}
      CHECK(D::state().holds[0].active&&D::state().holds[0].theta>th0+.2f,"hand on a closed leaf: the leaf then follows the hand");
      leafSide=0;away(20);stub::capOnContinue=false;}
     // ---------- DOOR PRESS: the commanded parts drive the leaf; the face locked from the approach; the free edge; the mover through a hold ----------
     {// pure: the free edge, the side track, the side q, the target never backward with the side locked
      const P::Rows lr{{{1,0,0},{0,1,0},{0,0,1}}};const P::Leaf f=P::leafFrame(lr,lr,{0,0,0},{0,0,-.03f},{.9f,2.1f,.03f},true,{0,1,0},1.f);
      const P::Rows hr{{{0,0,-1},{0,1,0},{1,0,0}}}; // S = -z, F = +y, N = +x (the palm's thin axis along the leaf)
      {const P::HandBoxes hb=P::handBoxes({.93f,1.f,0.f},hr,false,1.f);const P::LeafTouch e=P::leafTouch(f,hb,1.f,true),o=P::leafTouch(f,hb,1.f,false);
       CHECK(f.sure&&e.ok&&e.gap>=0.f&&near(e.gap,.016f,1e-3f)&&!e.straddle&&o.ok&&o.gap<0.f,"door press: a palm 3 cm past the free edge: 1.6 cm from the edge (never inside); without the edge rule it reads inside");}
      {const P::HandBoxes hb=P::handBoxes({.91f,1.f,.03f},hr,false,1.f);const P::LeafTouch e=P::leafTouch(f,hb,1.f,true);
       CHECK(e.ok&&e.straddle&&near(e.ext,.014f,1e-4f)&&near(e.gap,.03f-.014f-.03f,1e-4f)&&e.side==1,"door press: a palm straddling the edge: its support capped at the palm's half thickness, pressing");}
      {P::DoorSideTrack t{};const auto lt=[](float gap,int side){P::LeafTouch x{};x.ok=true;x.gap=gap;x.side=side;return x;};const std::uintptr_t n=0x5550;
       t.note(n,lt(.08f,-1),1.f,100*ms);t.note(n,lt(.05f,-1),1.f,120*ms);t.note(n,lt(.02f,-1),1.f,140*ms);const bool a=t.side==-1;
       t.note(n,lt(-.01f,1),1.f,160*ms);const bool b=t.side==-1&&t.fresh(n,160*ms);
       const bool c1=!t.note(n,lt(.05f,1),1.f,180*ms,-1)&&!t.note(n,lt(.05f,1),1.f,200*ms,-1)&&t.side==-1;
       const bool c2=t.note(n,lt(.05f,1),1.f,220*ms,1)&&t.side==1;
       P::DoorSideTrack u{};u.note(n,lt(.02f,-1),1.f,100*ms);const bool d=!u.fresh(n,500*ms)&&!u.fresh(0x7770,100*ms);
       CHECK(a&&b&&c1&&c2&&d,"door press side: set outside, kept while the centre crosses inside, the other face needs two outside samples and the solved part there; 300 ms fresh, per door");}
      {const P::V3 k=P::openAxisPush(f.L0,f.c0,V::mul(f.c0,1.f)),k2=P::openAxisPush(f.L0,f.c0,V::mul(f.c0,-1.f));
       CHECK(P::doorQOf(f,k,1)==-1&&P::doorQOf(f,k2,-1)==-1,"door press: a closed leaf pushed from either face: q -1 (it opens away from the hand)");
       float th=.5f;bool mono=true;const float clr=.05f;for(const float dl:{-.06f,-.02f,0.f,.03f,.08f,.02f}){const float t=P::doorTarget(-1,th,dl,clr,1.6f,6.f,.028f);mono=mono&&t>=th;th=t;}
       CHECK(mono,"door press: with the side locked the command crossing the centre plane never moves the leaf back");}}
     // the lagging body (the solved hand held 3 cm off the leaf's face, its commanded part pressing): with doorpress 0 the solved-part gate never moves the leaf;
     // DOOR PRESS moves it on every push tick, no flips; a still hand: no moves, the hold kept; pulled back: an idle release, rested
     {auto& st=D::state();const float hd0=stub::halfDepth;
      const auto lagRun=[&](bool press,int& moves,int& holds,unsigned long long& flips,unsigned long long& wouldRefuse,float& thEnd,bool& mono){
        static bool pressNow=true;pressNow=press;
        P::leverPatchForTests.store(+[](P::Levers& l){l.doorBody=false;l.doorPress=pressNow;});st.leversRead=false;
        setMover({-1.f,0,0});leafSide=0;away(6);simNs+=2000*ms;setup(4,.5f,1.6f,1,1.f);const float th=P::easeOpen(.5f)*1.6f;
        for(int i=0;i<8;++i)fw(arc(th,-.30f,1));stub::halfDepth=.10f;leafSide=1;for(int i=0;i<2;++i)stepW(arc(th,-.10f,1),true);
        const auto m0=Ds.pose4Moves.load(),p0=Ds.pose4Holds.load(),f0=Ds.sideFlips.load(),v0=Ds.solvedGateWouldRefuse.load();
        float phi=th,last=1e9f;mono=true;
        for(int i=0;i<20;++i){phi-=.021f;stepW(arc(phi,0.f,1),i<2);const float t=P::easeOpen(ifT())*1.6f;mono=mono&&t<=last+1e-5f;last=t;}
        moves=int(Ds.pose4Moves.load()-m0);holds=int(Ds.pose4Holds.load()-p0);flips=Ds.sideFlips.load()-f0;wouldRefuse=Ds.solvedGateWouldRefuse.load()-v0;thEnd=P::easeOpen(ifT())*1.6f;
        return phi;};
      int mv0=0,h0=0,mv1=0,h1=0;unsigned long long fl0=0,fl1=0,w0=0,w1=0;float te0=0,te1=0;bool mono0=true,mono1=true;
      lagRun(false,mv0,h0,fl0,w0,te0,mono0);leafSide=0;away(20);
      const float phiEnd=lagRun(true,mv1,h1,fl1,w1,te1,mono1);
      CHECK(h0==0&&mv0==0,"door press 0: the solved part 3 cm off the face: no hold, the leaf never moves (the stall reproduced)");
      CHECK(h1==1&&mv1>=18&&fl1==0&&mono1&&w1>=15&&near(te1,phiEnd-P::partClearance(.03f,.0425f,.8f),.04f),
            "door press 1: one hold, the leaf moves on 18+ of 20 push ticks, no flips, only closing, the solved-part gate would have refused, the leaf at the hand plus its clearance");
      {stepW(arc(phiEnd,0.f,1),false); // the tick reads the last substeps' command: one frame behind the controller
       const auto m0=Ds.pose4Moves.load();for(int i=0;i<10;++i)stepW(arc(phiEnd,0.f,1),false);
       CHECK(Ds.pose4Moves.load()==m0&&st.holds[0].active,"door press: the hand still for 10 ticks: no move, the hold kept");}
      {const auto r0=Ds.rests.load(),i0=Ds.releasesIdle.load();leafSide=0;for(int i=0;i<20;++i)stepW(arc(phiEnd,-.20f,1),false);
       CHECK(!st.holds[0].active&&Ds.rests.load()==r0+1&&Ds.releasesIdle.load()==i0+1&&restedAny(),"door press: the hand pulled back 20 cm for 560 ms: an idle release, rested at its angle");}
      {const auto& e=st.doorEvents[(st.doorEventNext+15)%16];CHECK(e.set&&e.presses>=18&&e.flips==0&&e.maxLagCm>=2.f,"door press: the event (presses, no flips, the lag)");}
      stub::halfDepth=hd0;away(10);}
     // a closed leaf pressed 1.5 cm past its face with a lagging body: one Open, then a POSE4 hold that moves on every push tick
     {auto& st=D::state();const float hd0=stub::halfDepth;P::leverPatchForTests.store(noBody);st.leversRead=false;
      setMover({-1.f,0,0});away(4);simNs+=2000*ms;setup(0,0.f,-1.f,0,1.f);stub::capOnContinue=true;
      for(int i=0;i<6;++i)fw(arc(0.f,-.30f,-1));stub::halfDepth=.10f;leafSide=-1;for(int i=0;i<2;++i)stepW(arc(0.f,-.10f,-1),true);
      const int o0=stub::doorOpens;const auto p0=Ds.pose4Holds.load(),sh0=Ds.closedShallow.load();
      stepW(arc(0.f,-.0675f,-1),true);stepW(arc(0.f,-.0675f,-1),true); // the tick reads the command one frame late
      CHECK(stub::doorOpens==o0&&Ds.closedShallow.load()>=sh0+1,"door press: a closed leaf pressed 0.5 cm: not opened (under 1 cm), counted");
      stepW(arc(0.f,-.0575f,-1),true);stepW(arc(0.f,-.0575f,-1),true);
      CHECK(stub::doorOpens==o0+1&&Ds.pose4Holds.load()==p0+1&&st.holds[0].active&&st.holds[0].q<0,"door press: 1.5 cm past the closed face: one Open, a POSE4 hold (q -1)");
      const auto m0=Ds.pose4Moves.load();float phi=st.holds[0].theta;
      for(int i=0;i<11;++i){if(i<10)phi+=.03f;stepW(arc(phi,0.f,-1),i<2);} // the 11th tick reads the 10th push
      CHECK(Ds.pose4Moves.load()>=m0+10&&stub::doorOpens==o0+1,"door press: then the leaf moves on each of the next 10 push ticks");
      stub::halfDepth=hd0;leafSide=0;away(20);stub::capOnContinue=false;}
     // the free edge: a hand at r 0.91 m (1 cm past the edge) moving 1 cm a tick across the leaf: the leaf turns with it, no flips
     {auto& st=D::state();P::leverPatchForTests.store(noBody);st.leversRead=false;
      setMover({-1.f,0,0});away(4);simNs+=2000*ms;setup(4,.5f,1.6f,1,1.f);const float th=P::easeOpen(.5f)*1.6f;
      const auto edge=[&](float phi,float z){const P::V3 L{std::sin(phi),0,std::cos(phi)},c{std::cos(phi),0,-std::sin(phi)};return V::add(V::add(o,up),V::add(V::mul(L,.91f),V::mul(c,z)));};
      for(int i=0;i<6;++i)fw(edge(th,.30f));for(int i=0;i<2;++i)stepW(edge(th,.10f),true);
      const auto f0=Ds.sideFlips.load(),m0=Ds.pose4Moves.load(),e0=Ds.edgeStraddles.load();
      const P::V3 c0{std::cos(th),0,-std::sin(th)};P::V3 at=edge(th,.03f);
      for(int i=0;i<10;++i){stepW(at,i<2);at=V::sub(at,V::mul(c0,.01f));}
      const float te=P::easeOpen(ifT())*1.6f;
      CHECK(st.holds[0].active&&Ds.sideFlips.load()==f0&&Ds.pose4Moves.load()>=m0+8&&te<th-3.f*P::pi/180.f&&Ds.edgeStraddles.load()>e0,
            "door press: a hand on the free edge moving across: the leaf turns with it (8+ moves, over 3 deg), straddling presses, no flips");
      away(20);}
     // the leaf held back (the rate cap at its 0.5 rad/s floor) while the hand pushes on: the command over 2 cm through the leaf, the solved
     // part kept on the locked face by the leaf: the leaf keeps moving every tick (a through-touch press, rate capped), no flips
     {auto& st=D::state();static const auto slow=+[](P::Levers& l){l.doorBody=false;l.doorRateRadS=.5f;};P::leverPatchForTests.store(slow);st.leversRead=false;
      setMover({-1.f,0,0});leafSide=0;away(6);simNs+=2000*ms;setup(4,.5f,1.6f,1,1.f);const float th=P::easeOpen(.5f)*1.6f;
      for(int i=0;i<8;++i)fw(arc(th,-.30f,1));leafSide=1;for(int i=0;i<2;++i)stepW(arc(th,-.10f,1),true);
      float phi=th;for(int i=0;i<3;++i){phi-=.01f;stepW(arc(phi,0.f,1),true);}
      CHECK(st.holds[0].active&&st.holds[0].q>0,"door press through: a hold from the open face");
      const auto m0=Ds.pose4Moves.load(),t0=Ds.throughTouch.load(),f0=Ds.sideFlips.load();float gapMax=-1e9f;
      for(int i=0;i<4;++i){phi-=.05f;stepW(arc(phi,0.f,1),false);}
      for(int i=0;i<8;++i){stepW(arc(phi,0.f,1),false);const auto& tk=st.doorTicks[(st.doorTickNext+st.doorTicks.size()-1u)%st.doorTicks.size()];gapMax=std::max(gapMax,tk.cmdGapCm);}
      const auto moves=Ds.pose4Moves.load()-m0,touch=Ds.throughTouch.load()-t0;
      std::printf("  [info] door press through: moves %llu / 12, throughTouch %llu, cmdGap max %.1f cm\n",(unsigned long long)moves,(unsigned long long)touch,double(gapMax));
      CHECK(st.holds[0].active&&moves>=11&&touch>=4&&gapMax>2.f&&Ds.sideFlips.load()==f0,
            "door press through: the command 2+ cm through, the solved part on the face: the leaf moves on 11+ of 12 ticks (through-touch presses), no flips");
      bool ringPress3=false;for(const auto& x:st.doorTicks)ringPress3=ringPress3||(x.set&&x.press==3);
      CHECK(ringPress3,"door press through: the tick ring marks those ticks (press 3)");
      leafSide=0;away(20);}
     // a hand resting near a closed leaf (its fingers 2.5 cm off the face) without a contact: refused every tick, sampled at most once per 100 ms per hand and door;
     // refusedGapCm counts only contact pushes
     {auto& st=D::state();P::leverPatchForTests.store(noBody);st.leversRead=false;
      setMover({-1.f,0,0});away(4);simNs+=2000*ms;setup(0,0.f,-1.f,0,1.f);leafSide=0;for(int i=0;i<6;++i)fw(arc(0.f,-.30f,-1));for(int i=0;i<4;++i)fw(arc(0.f,-.09f,-1));
      std::uint64_t rg0=0;for(const auto& c:Ds.refusedGap)rg0+=c.load();
      const auto n0=Ds.startNoPress.load(),nn0=Ds.startNoPressNoted.load(),pc0=Ds.pressCands.load();const int o0=stub::doorOpens;
      const unsigned ring0=0;const auto tS=simNs;
      for(int i=0;i<10;++i)stepW(arc(0.f,-.09f,-1),true);
      std::uint64_t rg1=0;for(const auto& c:Ds.refusedGap)rg1+=c.load();
      unsigned ring1=0;for(const auto& x:st.doorTicks)ring1+=x.set&&x.ns>tS&&x.why==P::DtStartNoPress?1u:0u;
      const auto n=Ds.startNoPress.load()-n0,nn=Ds.startNoPressNoted.load()-nn0;
      std::printf("  [info] door press refused: %llu ticks, %llu noted, ring +%u\n",(unsigned long long)n,(unsigned long long)nn,ring1-ring0);
      CHECK(stub::doorOpens==o0&&n>=8&&nn>=1&&nn<=4&&ring1-ring0==nn&&rg1==rg0&&Ds.pressCands.load()>=pc0+8,
            "door press: a hand resting near a closed leaf: refused each tick, 1-4 samples in 280 ms, no refusedGapCm (no contact), counted as candidates");
      away(20);}
     // the mover's push on a held leaf whose hand does not press: posed through the hold (it stays valid); lever doorpress 0: refused
     {auto& st=D::state();P::leverPatchForTests.store(noBody);st.leversRead=false;
      setMover({-1.f,0,0});away(4);simNs+=2000*ms;setup(4,.3f,1.6f,1,1.f);const float th=P::easeOpen(.3f)*1.6f;
      for(int i=0;i<6;++i)fw(arc(th,-.30f,-1));leafSide=-1;for(int i=0;i<2;++i)stepW(arc(th,-.10f,-1),true);
      float phi=th;for(int i=0;i<4;++i){phi+=.02f;stepW(arc(phi,0.f,-1),i<3);}
      CHECK(st.holds[0].active&&st.holds[0].q<0,"door body via hold: a hold from the closed face");
      const float thH=st.holds[0].theta;for(int i=0;i<3;++i)stepW(arc(thH,-.085f,-1),false); // resting 1 to 2 cm off the face: kept, no press
      const bool keptNoPress=st.holds[0].active&&st.holds[0].pressNs!=simNs;
      const float a=thH-40.f*P::pi/180.f;const P::V3 mv{1.f+.6f*std::sin(a),0.f,-.8f+.6f*std::cos(a)};
      const auto v0=Ds.bodyViaHold.load(),r4=Ds.bodyRefused[4].load(),g4=Ds.dropsGame4.load();const float t0=ifT();
      st.lev.doorPress=true;D::bodyDoor(st,stub::addr(dp.inst),mv,simNs,.028f,false);
      const float t1=ifT();
      CHECK(keptNoPress&&Ds.bodyViaHold.load()==v0+1&&Ds.bodyRefused[4].load()==r4&&t1>t0&&st.holds[0].lastT==t1&&st.holds[0].active,
            "door body via hold: the mover's push poses the leaf through the hold (phase and hold record together)");
      stub::doorUpdate(.028f);stepW(arc(thH,-.085f,-1),false);
      CHECK(st.holds[0].active&&Ds.dropsGame4.load()==g4,"door body via hold: the hold still owns the door on the next tick (no game drop)");
      st.lev.doorPress=false;const float t2=ifT();D::bodyDoor(st,stub::addr(dp.inst),mv,simNs+1,.028f,false);
      CHECK(Ds.bodyRefused[4].load()==r4+1&&ifT()==t2,"door body via hold: lever doorpress 0: refused (handHeld), nothing written");
      st.lev.doorPress=true;leafSide=0;away(20);}
     {const auto r=P::report();CHECK(jsonValid(r)&&r.find("\"press\":{\"lever\":1")!=std::string::npos&&r.find("\"tickRing\":[{")!=std::string::npos&&
            r.find("\"maxLagCm\":")!=std::string::npos&&r.find("\"doorPress\":1")!=std::string::npos,"door press: the report parses (doors.press, the tick ring, the event fields, the lever)");}
     // ---------- DOORS: the FindDoor hook, the mover's push, the hand-back, the stuck fallback ----------
     {static unsigned char actorBlk[0x40]{},animBlk[0xac0]{},obj1[0x20]{},obj2[0x20]{},footObj[0x200]{};
      static std::uint32_t modeWord=0;
      stub::put<std::uintptr_t>(actorBlk,0x38,stub::addr(animBlk));stub::put<std::uintptr_t>(animBlk,0xab8,stub::addr(obj1));stub::put<std::uintptr_t>(obj1,0x18,stub::addr(obj2));
      P::playerFoot.store(stub::addr(footObj));
      D::findDoorOriginal.store(+[](void*,void* handle,void*,void*)->bool{stub::put<std::uintptr_t>(handle,0,stub::addr(stub::doorInst));return true;});
      D::openOriginal.store(&stub::doorOpen);D::closeOriginal.store(&stub::doorClose);
      static unsigned char handle[16]{};
      const auto hook=[&]{stub::put<std::uint32_t>(obj2,0x18,modeWord);return D::findDoorHook(footObj+0x1a8,handle,nullptr,actorBlk);};
      // the mover 1.2 m from the closed leaf on its closed side (x < 1), walking at 1.4 m/s toward it; the hands kept behind him
      simNs+=2000*ms;setup(0,0.f,-1.f,0,1.f);stub::capOnContinue=true;setMover({-.2f,0,-.35f});for(int i=0;i<4;++i){frame({-.3f,1.2f,0});stub::doorUpdate(.028f);}
      const auto s0=Ds.fdBy[P::FindSuppress].load();
      CHECK(!hook()&&Ds.fdBy[P::FindSuppress].load()==s0+1,"door hook: the automatic walk-in open while the hands are live: no door for the player");
      modeWord=7;CHECK(hook()&&Ds.fdBy[P::FindPrompt].load()>=1,"door hook: the prompt mode: passed through");modeWord=0;
      CHECK(hook()==false&&D::findDoorHook(footObj+0x10,handle,nullptr,actorBlk),"door hook: another caller (not the foot input): passed through");
      {static const auto autoOn=+[](P::Levers& l){l.doorBody=true;l.doorAuto=true;};P::leverPatchForTests.store(autoOn);D::state().leversRead=false;frame({-.3f,1.2f,0});
       CHECK(hook()&&Ds.fdBy[P::FindLever].load()>=1,"door hook: lever doorauto 1: stock");
       P::leverPatchForTests.store(nullptr);D::state().leversRead=false;frame({-.3f,1.2f,0});}
      // the mover walks into the closed leaf and its face (x = 1 - 0.03) stops his 0.35 m capsule at x = 0.62, as the engine's mover would
      const auto bo0=Ds.bodyOpens.load();const auto so00=Ds.stuckOpens.load();int i=0;const float faceStop=1.f-.03f-P::doorMoverRadiusM;
      for(;i<40&&stub::doorStateValue==0;++i){hook();setMover({std::min(moverAt[0]+.05f,faceStop),moverAt[1],moverAt[2]});frame({-.3f,1.2f,0});stub::doorUpdate(.028f);}
      CHECK(Ds.bodyOpens.load()==bo0+1&&stub::doorStateValue==4&&restedAny()&&moverAt[0]<=faceStop+1e-4f&&Ds.stuckOpens.load()==so00,
            "body push: walking into the closed leaf until its face stops him opens it posed just ahead of him (the leaf's thickness counted)");
      const float t1=ifT();const auto r0=Ds.bodyRaises.load();
      for(int k=0;k<10;++k){hook();setMover(V::add(moverAt,{.05f,0,0}));frame({-.3f,1.2f,0});stub::doorUpdate(.028f);}
      CHECK(Ds.bodyRaises.load()>r0&&ifT()>t1&&stub::doorStateValue==4,"body push: walking on: the leaf moves ahead of him");
      const float t2=ifT();for(int k=0;k<6;++k){hook();frame({-.3f,1.2f,0});stub::doorUpdate(.028f);}
      CHECK(near(ifT(),t2,1e-6f),"body push: standing still: the leaf stays");
      // an Open of that rested door (an NPC): handed back to the engine's opening
      {const auto hb0=Ds.handBacks.load();alignas(16) const float q[4]{2.f,0,0,0};D::doorOpenHook(stub::doorInst,q,false);
       CHECK(Ds.openOnOurs.load()>=1,"open hook: an Open of a rested door counted");
       frame({-.3f,1.2f,0});
       CHECK(Ds.handBacks.load()==hb0+1&&stub::lastSetState==2&&stub::doorStateValue==2&&!restedAny(),"open hook: handed back to the engine's opening (state 2)");}
      for(int k=0;k<40;++k)stub::doorUpdate(.028f);
      setMover({-1.f,0,0});away(6);
      // a closed pivot door whose leaf his capsule cannot reach (stopped 7 cm short: a wider real capsule): the stock Open after 1.5 s
      simNs+=2000*ms;setup(0,0.f,-1.f,0,1.f);setMover({faceStop-.07f,0,-.35f});
      {const auto so1=Ds.stuckOpens.load(),bo1=Ds.bodyOpens.load();const int o5=stub::doorOpens;
       for(int k=0;k<40;++k){hook();frame({-.3f,1.2f,0});}
       CHECK(Ds.stuckOpens.load()==so1&&stub::doorOpens==o5&&Ds.bodyGapHist[3].load()>0,"stuck: a posable leaf 7 cm ahead of his capsule: nothing under 1.5 s (its gap logged)");
       for(int k=0;k<30;++k){hook();frame({-.3f,1.2f,0});}
       CHECK(Ds.stuckOpens.load()==so1+1&&stub::doorOpens==o5+1&&Ds.bodyOpens.load()==bo1,"stuck: a posable leaf no push could open in 1.5 s: the stock Open");}
      for(int k=0;k<40;++k)stub::doorUpdate(.028f);
      setMover({-1.f,0,0});away(6);
      // walking back into a door that opened toward him: his closing push holds it at the floor; after 300 ms it is shut (the engine's
      // Close) and the closed leaf then opens away from him
      simNs+=2000*ms;{const float th=.17f,cap=1.6f;setup(4,P::phaseFor(th/cap,false),cap,0,1.f);
       float rows[12]{};std::memcpy(rows,dp.inst+0x20,48);const P::V3 Lt{rows[0],rows[1],rows[2]},L0{0,0,1};const float thNow=std::acos(std::clamp(V::dot(Lt,L0),-1.f,1.f));
       const P::V3 w=V::mul(V::sub(Lt,V::mul(L0,std::cos(thNow))),1.f/std::sin(thNow));const float ps=thNow+4.f*P::pi/180.f;
       setMover(V::add({1.f,0,-.8f},V::mul(V::add(V::mul(L0,std::cos(ps)),V::mul(w,std::sin(ps))),.6f)));
       const auto f0=Ds.bodyFlips.load(),bo2=Ds.bodyOpens.load();const int c5=stub::doorCloses;int k=0;
       for(;k<30&&Ds.bodyFlips.load()==f0;++k){hook();frame({-.3f,1.2f,0});stub::doorUpdate(.028f);}
       CHECK(Ds.bodyFlips.load()==f0+1&&stub::doorCloses==c5+1&&k>=10&&!restedAny(),"body push: a closing push held at the floor for 300 ms: the leaf shut (Close), no longer rested");
       for(int n=0;n<40&&Ds.bodyOpens.load()==bo2;++n){hook();frame({-.3f,1.2f,0});stub::doorUpdate(.028f);}
       CHECK(Ds.bodyOpens.load()==bo2+1&&stub::doorStateValue==4,"body push: the shut leaf then opens away from him");}
      for(int k=0;k<40;++k)stub::doorUpdate(.028f);
      setMover({-1.f,0,0});away(6);stub::capOnContinue=false;
      // an anim door the push cannot pose: the stock Open after 1.5 s at it
      simNs+=2000*ms;setup(0,0.f,-1.f,0,1.f);stub::put<std::uintptr_t>(stub::doorIface,0,stub::base+P::off::animVptr);setMover({.55f,0,-.35f});
      const auto so0=Ds.stuckOpens.load();const int o1=stub::doorOpens;
      for(int k=0;k<40;++k){hook();frame({-.3f,1.2f,0});}
      CHECK(Ds.stuckOpens.load()==so0&&stub::doorOpens==o1,"stuck: under 1.5 s at an anim door: nothing");
      for(int k=0;k<30;++k){hook();frame({-.3f,1.2f,0});}
      CHECK(Ds.stuckOpens.load()==so0+1&&stub::doorOpens==o1+1,"stuck: 1.5 s at an anim door the hook keeps from him: the stock Open");
      setMover({-1.f,0,0});away(6);
      D::findDoorOriginal.store(nullptr);D::openOriginal.store(nullptr);D::closeOriginal.store(nullptr);P::playerFoot.store(0);}
     P::leverPatchForTests.store(nullptr);D::state().leversRead=false;
     setMover({0,0,0});
     leafSide=0;stub::poseLeaf=false;wallOn=true;stub::put<std::uintptr_t>(stub::doorInst,0x50,0);stub::doorStateValue=0;stub::clearProps();for(int i=0;i<4;++i)frame({1.3f,1.2f,0});}
    // a prop met while touching: woken through ForceActivation; an active one counted; the LRU
    {auto& pp=stub::props[0];stub::placeProp(pp,10,stub::navVtable,{1.05f,1.2f,0},.1f,1);
     CHECK(D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr),"a prop pair is kept");
     frame({1.3f,1.2f,0});
     CHECK(stub::forceActivations==1&&stub::propState(10)==0&&P::stats.props.activated.load()==1,"pressing an inactive prop: ForceActivation, active");
     D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});
     CHECK(stub::forceActivations==1&&P::stats.props.alreadyActive.load()>=1,"an active prop: not woken again");
     stub::setPropState(10,1);D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});
     CHECK(stub::forceActivations==1&&P::stats.props.gated.load()>=1,"asleep again inside 1 s: gated");
     stub::placeProp(pp,10,stub::navVtable,{1.05f,1.2f,0},.1f,1,false,true);D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);
     simNs+=1100*ms;frame({1.3f,1.2f,0});CHECK(stub::forceActivations==1&&P::stats.props.infinite.load()==1,"infinite mass vs movers: not woken");
     stub::clearProps();}
    // the type's activation lock lifted on a kept pair (CollideObjects); explicit overrides, never-activate, doors, fixed props kept
    {static unsigned char lockType[0x200]{};stub::put<std::uint16_t>(lockType,0x1ba,0x20);D::state().unlockSeen={};
     auto& Pp=P::stats.props;const auto collide=[&](stub::Prop& q){return D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,q.inst,nullptr,nullptr);};
     auto& a=stub::props[0];stub::placeProp(a,10,stub::navVtable,{1.05f,1.2f,0},.1f,1);stub::put<std::uintptr_t>(a.inst,0x98,stub::addr(lockType));
     CHECK(stub::isActDisabled(a.inst),"unlock: a prop whose type locks activation");
     const auto u0=Pp.unlocks.load();
     CHECK(collide(a)&&Pp.unlocks.load()==u0+1&&stub::at<std::uint32_t>(a.inst,0x80)==0x103u&&!stub::isActDisabled(a.inst),"unlock: lifted on a kept pair (override bit 8 set, bit 7 clear)");
     const int k0=stub::unlockCalls;collide(a);CHECK(stub::unlockCalls==k0&&Pp.skipCached.load()>=1,"unlock: judged once");
     auto& b=stub::props[1];stub::placeProp(b,11,stub::navVtable,{1.05f,1.3f,0},.1f,1);stub::put<std::uintptr_t>(b.inst,0x98,stub::addr(lockType));stub::put<std::uint32_t>(b.inst,0x80,0x180u);
     collide(b);CHECK(stub::at<std::uint32_t>(b.inst,0x80)==0x180u&&Pp.skipOverride.load()>=1,"unlock: an explicit override (placement, script) kept");
     auto& c=stub::props[2];stub::placeProp(c,12,stub::navVtable,{1.05f,1.1f,0},.1f,1);stub::put<std::uintptr_t>(c.inst,0x98,stub::addr(lockType));stub::put<std::uint16_t>(c.inst,0x12,0x8);
     collide(c);CHECK(stub::at<std::uint32_t>(c.inst,0x80)==0x3u&&Pp.skipNever.load()>=1,"unlock: never-activate kept");
     auto& d=stub::props[3];stub::placeProp(d,13,stub::navVtable,{1.05f,1.2f,.1f},.1f,1,true);stub::put<std::uintptr_t>(d.inst,0x98,stub::addr(lockType));
     collide(d);CHECK(stub::at<std::uint32_t>(d.inst,0x80)==0x3u&&Pp.skipDoor.load()>=1,"unlock: a door kept");
     auto& e=stub::props[4];stub::placeProp(e,14,stub::navVtable,{1.05f,1.2f,-.1f},.1f,2);stub::put<std::uintptr_t>(e.inst,0x98,stub::addr(lockType));
     collide(e);CHECK(stub::at<std::uint32_t>(e.inst,0x80)==0x3u&&Pp.skipFixed.load()>=1,"unlock: a fixed prop kept");
     auto& f=stub::props[5];stub::placeProp(f,15,stub::navVtable,{1.05f,1.25f,-.1f},.1f,1,false,true);stub::put<std::uintptr_t>(f.inst,0x98,stub::addr(lockType));
     collide(f);
     CHECK(!(stub::at<std::uint32_t>(f.inst,0x80)&0x180u)&&Pp.skipInfinite.load()>=1,"unlock: infinite mass vs movers kept");
     auto& g=stub::props[6];stub::placeProp(g,8,stub::navVtable,{1.05f,1.15f,-.1f},.1f,1);stub::put<std::uintptr_t>(g.inst,0x98,stub::addr(lockType));
     D::state().hand[1].ghost=true;CHECK(!collide(g)&&stub::at<std::uint32_t>(g.inst,0x80)==0x3u,"unlock: a ghost hand's pairs are dropped: no unlock");D::state().hand[1].ghost=false;
     P::Levers lv=D::state().lev;D::state().lev.unlock=false;collide(g);CHECK(stub::at<std::uint32_t>(g.inst,0x80)==0x3u,"unlock: lever unlock 0");
     D::state().lev=lv;P::groupOk[P::GroupAct]=false;collide(g);CHECK(stub::at<std::uint32_t>(g.inst,0x80)==0x3u,"unlock: ACT refused");P::groupOk[P::GroupAct]=true;
     D::state().unlockSeen={};stub::clearProps();}
    // ---------- RELEASE: a small inactive nav prop whose root is fixed (+0x80 bit 1 clear) under a pressing palm ----------
    {static unsigned char relType[0x200]{};std::memset(relType,0,sizeof relType); // type bits 0, minMove 0
     auto& Pp=P::stats.props;auto& pp=stub::props[0];
     const auto press=[&]{D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});};
     stub::placeProp(pp,10,stub::navVtable,{1.05f,1.2f,0},.06f,1,false,false,0x1u);stub::put<std::uintptr_t>(pp.inst,0x98,stub::addr(relType));
     simNs+=1100*ms;{const int f0=stub::forceActivations;const auto r0=Pp.released.load(),a0=Pp.relActivated.load();press();
      CHECK(Pp.released.load()==r0+1&&stub::at<std::uint32_t>(pp.inst,0x80)==0x3u&&stub::forceActivations==f0+1&&stub::propState(10)==0&&Pp.relActivated.load()==a0+1,
            "release: f80 0x1 -> 0x3 (BreakApart's bit), ForceActivation once, the prop active");}
     // the type locks activation: no write, no wake (the unlock lifts the lock on a later pair)
     stub::put<std::uint16_t>(relType,0x1ba,0x20);stub::placeProp(pp,10,stub::navVtable,{1.05f,1.2f,0},.06f,1,false,false,0x1u);stub::put<std::uintptr_t>(pp.inst,0x98,stub::addr(relType));
     D::state().unlockSeen={};simNs+=1100*ms;
     {const int f0=stub::forceActivations;const auto l0=Pp.relBy[P::ReleaseTypeLocked].load();
      D::state().lev.unlock=false;press();D::state().lev.unlock=true;
      CHECK(Pp.relBy[P::ReleaseTypeLocked].load()==l0+1&&stub::at<std::uint32_t>(pp.inst,0x80)==0x1u&&stub::forceActivations==f0,"release: a type lock: no write, no wake");}
     stub::put<std::uint16_t>(relType,0x1ba,0);
     // a table (radius 60 cm): its root stays fixed, its wake skipped
     stub::placeProp(pp,10,stub::navVtable,{1.05f,1.2f,0},.6f,1,false,false,0x1u);stub::put<std::uintptr_t>(pp.inst,0x98,stub::addr(relType));simNs+=1100*ms;
     {const int f0=stub::forceActivations;const auto b0=Pp.relBy[P::ReleaseTooBig].load(),s0=Pp.skipRoot.load();press();
      CHECK(Pp.relBy[P::ReleaseTooBig].load()==b0+1&&Pp.skipRoot.load()==s0+1&&stub::at<std::uint32_t>(pp.inst,0x80)==0x1u&&stub::forceActivations==f0,"release: a table (radius 0.6 m) stays, no wasted wake");}
     // a null type (+0x78 not resident): never released
     {static unsigned char t78[0x20]{};stub::put<std::uint32_t>(t78,0xc,0x10000000u);stub::put<std::uintptr_t>(t78,0,stub::addr(relType));
      stub::placeProp(pp,10,stub::navVtable,{1.05f,1.2f,0},.06f,1,false,false,0x1u);stub::put<std::uintptr_t>(pp.inst,0x78,stub::addr(t78));simNs+=1100*ms;
      const auto n0=Pp.relBy[P::ReleaseTypeNull].load();press();
      CHECK(Pp.relBy[P::ReleaseTypeNull].load()==n0+1&&stub::at<std::uint32_t>(pp.inst,0x80)==0x1u,"release: a type that does not resolve is never released");}
     // lever release 0: the old wake (refused by the engine for a fixed root)
     stub::placeProp(pp,10,stub::navVtable,{1.05f,1.2f,0},.06f,1,false,false,0x1u);stub::put<std::uintptr_t>(pp.inst,0x98,stub::addr(relType));simNs+=1100*ms;
     {D::state().lev.release=false;const int f0=stub::forceActivations;press();D::state().lev.release=true;
      CHECK(stub::at<std::uint32_t>(pp.inst,0x80)==0x1u&&stub::forceActivations==f0+1,"release: lever release 0: no write, the plain wake");}
     stub::clearProps();}
    // ---------- UNFREEZE: a never-activate gohProp (+0x12 bit 3 from its description) under a pressing palm ----------
    {static unsigned char ufType[0x200]{},ud[0x30]{},goh[0x100]{},comp[0x100]{},compVt[0x100]{},partVt[0x100]{},part[0x40]{},bvhVt[0x100]{},partList[0x40]{};
     std::memset(ufType,0,sizeof ufType);std::memset(ud,0,sizeof ud);std::memset(goh,0,sizeof goh);std::memset(comp,0,sizeof comp);
     auto& Pp=P::stats.props;auto& pp=stub::props[0];
     const auto setup=[&](float mass,std::uint8_t d4,bool descFixed,bool bvhPart){
        stub::placeProp(pp,10,stub::navVtable,{1.05f,1.2f,0},.06f,1,false,false,0x3u);stub::put<std::uint16_t>(pp.inst,0x12,0x8);
        stub::put<std::uintptr_t>(pp.inst,0x98,stub::addr(ufType));stub::put<std::uintptr_t>(ufType,0x130,stub::addr(comp));
        stub::put<std::uintptr_t>(pp.inst,0x18,stub::addr(ud));stub::put<std::uintptr_t>(ud,0,stub::base+P::off::propUdVtable+0x10);stub::put<std::uint8_t>(ud,0x18,1);
        stub::put<std::uintptr_t>(ud,0x8,stub::addr(goh));stub::put<std::uintptr_t>(goh,0xe0,stub::addr(pp.inst));stub::put<std::uint8_t>(goh,0xf0,descFixed?1:0);
        stub::put<std::uint8_t>(comp,0x1c,0xc);stub::put<std::uintptr_t>(comp,0,stub::addr(compVt));stub::put<std::uintptr_t>(compVt,0xd0,stub::base+P::off::compCanActive);
        stub::put<std::uint8_t>(comp,0xd4,d4);stub::put<std::uint16_t>(comp,0xd2,1);stub::put<std::uintptr_t>(comp,0xa0,stub::addr(partList));
        stub::put<std::uintptr_t>(partList,0,stub::addr(part));stub::put<std::uintptr_t>(part,0,stub::addr(bvhPart?bvhVt:partVt));
        stub::put<std::uintptr_t>(partVt,0xd0,stub::base+P::off::partCanActive[0]);stub::put<std::uintptr_t>(bvhVt,0xd0,stub::base+0x1234);
        stub::put<float>(pp.arch,0x38,mass);D::state().ufSeen={};};
     // the palm pressed into the wall first (a hand coming from 80 cm away needs a few frames), then the pair with the prop
     const auto press=[&]{for(int i=0;i<5;++i)frame({1.3f,1.2f,0});D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});};
     setup(.3f,0,true,false);simNs+=1100*ms;
     {const int f0=stub::forceActivations;const auto d0=Pp.ufDone.load(),a0=Pp.ufActivated.load();press();
      CHECK(Pp.ufDone.load()==d0+1&&(stub::at<std::uint16_t>(pp.inst,0x12)&0x8u)==0&&stub::forceActivations==f0+1&&stub::propState(10)==0&&Pp.ufActivated.load()==a0+1,
            "unfreeze: a 300 g cup fixed by its description: the flag cleared, woken, active");}
     // RESETHOLD: the cup the palm made physical is held from the gringo's readiness reset while it is away from home
     {const std::uint32_t guid=(7u<<16)|3u;stub::gohCount=8;std::memset(stub::gohTable,0,sizeof stub::gohTable);
      stub::put<std::uintptr_t>(stub::gohTable,3*16,stub::addr(goh));stub::put<std::uint16_t>(stub::gohTable,3*16+8,7);
      CHECK(D::gohOf(guid)==stub::addr(goh)&&D::gohOf((8u<<16)|3u)==0&&D::gohOf((7u<<16)|9u)==0,"reset hold: a guid's GOH slot (generation checked, index within the count)");
      CHECK(D::heldHas(stub::addr(goh))&&D::holdLive.load()&&D::resetStats.adds.load()>=1,"reset hold: the cup made physical is in the published list");
      float pos[3];std::memcpy(pos,pp.inst+0x50,12);const float away[3]{pos[0]+.05f,pos[1],pos[2]};std::memcpy(pp.inst+0xb0,away,12);
      const auto h0=D::resetStats.held.load();
      CHECK(D::resetHeldFor(stub::base+P::off::readyPollRa,guid)&&D::resetStats.held.load()==h0+1,"reset hold: OnMsg's readiness poll of the cup 5 cm from home: held, no reset");
      CHECK(!D::resetHeldFor(stub::base+0x1234,guid),"reset hold: another caller of StartPropReset (a script command): the stock reset");
      CHECK(!D::resetHeldFor(stub::base+P::off::readyPollRa,(7u<<16)|4u),"reset hold: a prop the hands never moved: the stock reset");
      std::memcpy(pp.inst+0xb0,pos,12);
      CHECK(!D::resetHeldFor(stub::base+P::off::readyPollRa,guid),"reset hold: the held cup at home (OnMsg asks for a broken part): the stock reset");
      std::memcpy(pp.inst+0xb0,away,12);
      static int startCalls=0,commonCalls=0;
      D::startResetOriginal.store(+[](void*,std::uint32_t)->bool{++startCalls;return true;});D::commonResetOriginal.store(+[](std::uint32_t){++commonCalls;});
      {const int c0=startCalls;CHECK(D::startResetHook(nullptr,guid)&&startCalls==c0+1,"reset hook: a caller that is not the readiness poll: the original runs");}
      {const int c0=startCalls;CHECK(!D::startResetFor(stub::base+P::off::readyPollRa,nullptr,guid)&&startCalls==c0,
            "reset hook: the readiness poll of the held cup answers false (OnMsg keeps the ready byte, the hotspot stays usable), no reset");
       CHECK(D::startResetFor(stub::base+P::off::readyPollRa,nullptr,(7u<<16)|4u)&&startCalls==c0+1,"reset hook: the readiness poll of a prop not held: the original runs");}
      // a stock CommonReset of the held cup (a gringo deactivation, a script): counted with its distance, the original runs, the entry goes
      {const auto s0=D::resetStats.dropBy[D::DropStock].load(),o0=D::resetStats.commonOurs.load();const int c0=commonCalls;
       D::commonResetHook(guid);frame({1.3f,1.2f,0});
       CHECK(commonCalls==c0+1&&D::resetStats.commonOurs.load()==o0+1&&D::resetStats.dropBy[D::DropStock].load()==s0+1&&!D::heldHas(stub::addr(goh)),
             "reset hold: a stock CommonReset of the cup: counted, the original ran, the entry dropped");}
      D::startResetOriginal.store(nullptr);D::commonResetOriginal.store(nullptr);std::memset(pp.inst+0xb0,0,12);}
     // the 6 cm cup whose type reads 10 kg: made physical, its own collider set to the scaled 0.35 kg (never the archetype)
     {stub::setPropState(10,1);for(int i=0;i<40;++i)frame({.5f,1.2f,0});
      setup(10.f,0,true,false);simNs+=1100*ms;stub::levelColliders[10]=stub::addr(stub::colliders[10]);std::memset(stub::colliders[10],0,sizeof stub::colliders[10]);
      stub::put<std::uintptr_t>(stub::colliders[10],0x10,stub::addr(pp.inst));const float in[4]{.02f,.02f,.02f,10.f};std::memcpy(stub::colliders[10]+0xa0,in,16);
      const auto m0=Pp.ufMassSet.load();press();
      CHECK(Pp.ufMassSet.load()==m0+1&&near(stub::at<float>(stub::colliders[10],0xac),P::unfreezeMassKg(.06f,.35f,10.f),1e-4f)&&stub::propState(10)==0&&
            near(stub::at<float>(pp.arch,0x38),10.f),"unfreeze: a cup whose type reads 10 kg: active, its collider at 0.18 kg, the archetype untouched");
      // a reactivation copies the archetype's mass back: set again by the tick
      stub::put<float>(stub::colliders[10],0xac,10.f);const auto r0=Pp.ufMassReapplied.load();frame({.5f,1.2f,0});
      CHECK(Pp.ufMassReapplied.load()==r0+1&&stub::at<float>(stub::colliders[10],0xac)<1.f,"unfreeze: the collider's mass reset by a reactivation: set again");
      // a reactivation that gave it an articulated collider (type 1): never SetInertia on it
      stub::put<std::int32_t>(stub::colliders[10],0x8,1);stub::put<float>(stub::colliders[10],0xac,10.f);const auto r1=Pp.ufMassReapplied.load();frame({.5f,1.2f,0});
      CHECK(Pp.ufMassReapplied.load()==r1&&near(stub::at<float>(stub::colliders[10],0xac),10.f),"unfreeze: an articulated collider after a reactivation: its mass left alone");
      stub::put<std::int32_t>(stub::colliders[10],0x8,0);
      stub::levelColliders[10]=0;}
     // asleep again and untouched for a second: the flag back; touched in between: it waits
     stub::setPropState(10,1);for(int i=0;i<20;++i)frame({.5f,1.2f,0});
     CHECK((stub::at<std::uint16_t>(pp.inst,0x12)&0x8u)==0,"unfreeze: untouched under a second: the flag stays off");
     {const auto r0=Pp.ufRestored.load();for(int i=0;i<30;++i)frame({.5f,1.2f,0});
      CHECK(Pp.ufRestored.load()==r0+1&&(stub::at<std::uint16_t>(pp.inst,0x12)&0x8u)!=0,"unfreeze: asleep and untouched for a second: the flag back (SetPropFixed's write)");}
     // a prop the level dropped: forgotten without a write
     setup(.3f,0,true,false);simNs+=1100*ms;press();
     {const auto g0=Pp.ufDropped.load();std::memset(stub::levelObjects+10*0x30,0,0x30);stub::put<std::uint16_t>(pp.inst,0x12,0);frame({.5f,1.2f,0});
      CHECK(Pp.ufDropped.load()==g0+1&&stub::at<std::uint16_t>(pp.inst,0x12)==0,"unfreeze: a prop gone from the level: dropped, nothing written");}
     // the gates: a heavy prop, a BVH composite, a part that cannot become active, a script-fixed prop (no description bit)
     const auto refused=[&](float mass,std::uint8_t d4,bool desc,bool bvh,P::UnfreezeWhy why,const char* msg){setup(mass,d4,desc,bvh);simNs+=1100*ms;
        const int f0=stub::forceActivations;const auto b0=Pp.ufBy[why].load();press();
        CHECK(Pp.ufBy[why].load()==b0+1&&(stub::at<std::uint16_t>(pp.inst,0x12)&0x8u)!=0&&stub::forceActivations==f0&&stub::propState(10)==1,msg);};
     refused(80.f,0,true,false,P::UnfreezeHeavy,"unfreeze: a prop whose data reads 80 kg (over the 50 kg sanity cap) stays, no wake");
     refused(.3f,1,true,false,P::UnfreezeCompD4,"unfreeze: a composite with a BVH part stays");
     refused(.3f,0,true,true,P::UnfreezePartNotActive,"unfreeze: a part that cannot become active stays");
     refused(.3f,0,false,false,P::UnfreezeDescNotFixed,"unfreeze: fixed by a script (no description bit) stays");
     {setup(.3f,0,true,false);stub::put<float>(ufType,0x1c0,-1.f);simNs+=1100*ms;const auto b0=Pp.ufBy[P::UnfreezeMinMoveNeg].load();press();
      CHECK(Pp.ufBy[P::UnfreezeMinMoveNeg].load()==b0+1&&(stub::at<std::uint16_t>(pp.inst,0x12)&0x8u)!=0,"unfreeze: minMove < 0 (the stock release refuses it) stays");
      stub::put<float>(ufType,0x1c0,0.f);}
     // a verdict is kept: the same heavy prop is not read again
     {setup(80.f,0,true,false);simNs+=1100*ms;press();const auto b0=Pp.ufCached.load();press();
      CHECK(Pp.ufCached.load()==b0+1,"unfreeze: a kept verdict is not read again");}
     // lever unfreeze 0: never cleared, the restore of what was cleared
     {setup(.3f,0,true,false);simNs+=1100*ms;D::state().lev.unfreeze=false;const int f0=stub::forceActivations;press();D::state().lev.unfreeze=true;
      CHECK((stub::at<std::uint16_t>(pp.inst,0x12)&0x8u)!=0&&stub::forceActivations==f0,"unfreeze: lever 0: a never-activate prop is not woken");}
     {setup(.3f,0,true,false);simNs+=1100*ms;press();const auto r0=Pp.ufRestored.load();P::modeForTests.store(P::ModeMeasure);frame({.5f,1.2f,0});
      CHECK(Pp.ufRestored.load()==r0+1&&(stub::at<std::uint16_t>(pp.inst,0x12)&0x8u)!=0,"unfreeze: leaving ON restores every entry");
      P::modeForTests.store(P::ModeOn);for(int i=0;i<12;++i)frame({.5f,1.2f,0});}
     // REACH WAKE never wakes a never-activate prop (ForceActivation would be refused)
     {setup(.3f,0,true,false);const auto n0=P::stats.reach.skipNever.load();const int f0=stub::forceActivations;
      stub::placeProp(pp,10,stub::navVtable,V::add(solvedRight().solved,{.10f,0,0}),.04f,1,false,false,0x3u);stub::put<std::uint16_t>(pp.inst,0x12,0x8);
      simNs+=1100*ms;D::state().reachTick=0;D::state().reachLive=0;frame({.5f,1.2f,0});
      CHECK(P::stats.reach.skipNever.load()>=n0+1&&stub::forceActivations==f0,"reach wake: a never-activate prop is skipped (a still palm does not fire the approach)");}
     // the approach: a palm closing at 1 m/s makes the cup physical ahead of the contact (no wall: the hand moves freely)
     {wallOn=false;for(int i=0;i<6;++i)frame({.5f,1.2f,0});
      const auto movePp=[&](P::V3 c){const float m[4]{c[0],c[1],c[2],0.f},sp[4]{c[0],c[1],c[2],.04f};std::memcpy(pp.inst+0x50,m,16);std::memcpy(stub::levelObjects+10*0x30,sp,16);};
      setup(.3f,0,true,false);const P::V3 palm0=solvedRight().solved,c0=V::add(palm0,{.24f,0,0});movePp(c0);
      simNs+=1100*ms;D::state().reachTick=0;D::state().reachLive=0;
      const auto a0=Pp.ufNearArmed.load(),f0=Pp.ufApproachFired.load(),s0=Pp.ufSource[P::UfApproach].load();const int w0=stub::forceActivations;
      handVel={1.f,0,0};P::V3 tgt{.5f,1.2f,0};
      for(int i=0;i<10&&Pp.ufApproachFired.load()==f0;++i){tgt[0]+=.028f;frame(tgt);}
      handVel={0,0,0};const P::V3 palm1=solvedRight().solved;
      (void)a0;CHECK(Pp.ufApproachFired.load()==f0+1&&Pp.ufSource[P::UfApproach].load()==s0+1&&(stub::at<std::uint16_t>(pp.inst,0x12)&0x8u)==0&&
            stub::forceActivations==w0+1&&stub::propState(10)==0,"unfreeze approach: a palm closing at 1 m/s: armed by the reach query, made physical, active");
      CHECK(V::length(V::sub(c0,palm1))-.04f-P::nearPalmM>.0f,"unfreeze approach: fired with the palm still short of the cup (the first contact meets an active prop)");
      for(int i=0;i<4;++i)frame({.5f,1.2f,0});
      // a cup the palm leaves alone: no approach fires on a hand that opens away from it
      setup(.3f,0,true,false);movePp(V::add(solvedRight().solved,{.20f,0,0}));
      simNs+=1100*ms;D::state().reachTick=0;D::state().reachLive=0;const auto f1=Pp.ufApproachFired.load();
      handVel={-1.f,0,0};tgt={.5f,1.2f,0};for(int i=0;i<6;++i){tgt[0]-=.028f;frame(tgt);}handVel={0,0,0};
      CHECK(Pp.ufApproachFired.load()==f1&&(stub::at<std::uint16_t>(pp.inst,0x12)&0x8u)!=0,"unfreeze approach: a palm moving away: nothing fires");
      // lever unfreezenear 0: never armed
      D::state().lev.unfreezeNear=false;simNs+=1100*ms;D::state().reachTick=0;D::state().reachLive=0;const auto a1=Pp.ufNearArmed.load();
      handVel={1.f,0,0};tgt={.5f,1.2f,0};for(int i=0;i<4;++i){tgt[0]+=.028f;frame(tgt);}handVel={0,0,0};D::state().lev.unfreezeNear=true;
      CHECK(Pp.ufNearArmed.load()==a1&&(stub::at<std::uint16_t>(pp.inst,0x12)&0x8u)!=0,"unfreeze approach: lever unfreezenear 0: never armed");
      for(auto& n:D::state().hand[1].ufNear)n={};
      for(int i=0;i<6;++i)frame({.5f,1.2f,0});wallOn=true;stub::clearProps();for(int i=0;i<4;++i)frame({.5f,1.2f,0});}
     // the approach fired late (the fingers met the cup in the substeps before the tick that fires it): the first contact, already with an
     // active cup, gets the kick and the push speed; a first contact one frame after the fire (the solver met it active): the push speed
     for(int pass=0;pass<2;++pass){
      setup(10.f,0,true,false);simNs+=1100*ms;stub::levelColliders[10]=stub::addr(stub::colliders[10]);std::memset(stub::colliders[10],0,sizeof stub::colliders[10]);
      stub::put<std::uintptr_t>(stub::colliders[10],0x10,stub::addr(pp.inst));const float in[4]{.02f,.02f,.02f,10.f};std::memcpy(stub::colliders[10]+0xa0,in,16);
      for(int i=0;i<5;++i)frame({1.3f,1.2f,0});
      const P::V3 c=V::add(solvedRight().solved,{.10f,0,0});{const float m[4]{c[0],c[1],c[2],0.f},sp[4]{c[0],c[1],c[2],.04f};std::memcpy(pp.inst+0x50,m,16);std::memcpy(stub::levelObjects+10*0x30,sp,16);}
      auto& slot=D::state().hand[1].ufNear[0];slot={};slot.inst=stub::addr(pp.inst);slot.armedNs=simNs;slot.centre=c;slot.radius=.04f;slot.gap=-.1f;
      const auto f0=Pp.ufApproachFired.load(),y0=Pp.ufFirstYield.load(),k0=Pp.ufFirstKick.load(),kk0=Pp.ufKicks.load();
      handVel={.3f,0,0};
      D::state().cupLines={};D::state().cupLineNext=0;
      const auto lineOf=[&]{for(const auto& l:D::state().cupLines)if(l.inst==stub::addr(pp.inst))return &l;return static_cast<const P::CupLine*>(nullptr);};
      if(pass==0){D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});}
      else{frame({1.3f,1.2f,0});
           CHECK(Pp.ufApproachFired.load()==f0+1&&!lineOf(),"unfreeze approach: an activation the hand has not touched yet takes no timeline");
           D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});}
      CHECK(lineOf()&&lineOf()->src==P::UfApproach,"unfreeze approach: the timeline starts at the first contact");
      handVel={0,0,0};
      const float vx=stub::at<float>(stub::colliders[10],0x110);
      if(pass==0)CHECK(Pp.ufApproachFired.load()==f0+1&&Pp.ufFirstYield.load()==y0+1&&Pp.ufFirstKick.load()==k0+1&&Pp.ufKicks.load()==kk0+1&&vx>.1f,
                       "unfreeze approach, late: the contact met the fixed cup: on its first contact a kick from the hand's speed and the push speed");
      else CHECK(Pp.ufApproachFired.load()==f0+1&&Pp.ufFirstYield.load()==y0+1&&Pp.ufFirstKick.load()==k0&&Pp.ufKicks.load()==kk0,
                 "unfreeze approach, in time: the first contact met it active: the push speed, no kick");
      {const auto y1=Pp.ufFirstYield.load();D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});
       CHECK(Pp.ufFirstYield.load()==y1,"unfreeze approach: only the first contact");}
      stub::levelColliders[10]=0;for(auto& n:D::state().hand[1].ufNear)n={};for(int i=0;i<40;++i)frame({.5f,1.2f,0});}
     // a press that met the cup first: the hand's own contact speed (0.15 m/s, under the old 0.3 m/s floor) gives it a kick, the drive
     // into it the push speed; asleep again and met within the wake gap: woken at once, no gates
     {D::state().lev.unfreezeNear=false; // the press path alone (a closing palm would fire the approach first)
      setup(10.f,0,true,false);simNs+=1100*ms;stub::levelColliders[10]=stub::addr(stub::colliders[10]);std::memset(stub::colliders[10],0,sizeof stub::colliders[10]);
      stub::put<std::uintptr_t>(stub::colliders[10],0x10,stub::addr(pp.inst));const float in[4]{.02f,.02f,.02f,10.f};std::memcpy(stub::colliders[10]+0xa0,in,16);
      const auto k0=Pp.ufKicks.load();D::state().cups={};
      for(int i=0;i<5;++i)frame({1.3f,1.2f,0});
      handVel={.15f,0,0};D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});handVel={0,0,0};
      const float vx=stub::at<float>(stub::colliders[10],0x110);
      CHECK(Pp.ufKicks.load()==k0+1&&vx>.1f&&vx<.5f,"unfreeze press: a 0.15 m/s contact (under the old 0.3 m/s floor): a kick from the hand's own speed");
      for(int i=0;i<2;++i)frame({.5f,1.2f,0});
      const auto sl0=Pp.ufSleeps.load();stub::setPropState(10,1);frame({.5f,1.2f,0});
      CHECK(Pp.ufSleeps.load()==sl0+1,"unfreeze: a tracked cup asleep again: counted");
      const auto r0=Pp.ufRewakes.load(),g0=Pp.gatedLru.load();const int w0=stub::forceActivations;stub::put<float>(stub::colliders[10],0xac,10.f);
      handVel={.4f,0,0};for(int i=0;i<3;++i)frame({1.3f,1.2f,0});
      D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);frame({1.3f,1.2f,0});handVel={0,0,0};
      CHECK(Pp.ufRewakes.load()==r0+1&&Pp.gatedLru.load()==g0&&stub::forceActivations==w0+1&&stub::propState(10)==0&&stub::at<float>(stub::colliders[10],0xac)<1.f,
            "unfreeze: met again asleep within the wake gap: woken at once, its light mass set again");
      const auto* tr=D::cupTrace(D::state(),stub::addr(pp.inst),false);
      CHECK(tr&&tr->contacts>=1&&tr->by[P::UfPress]==1&&tr->by[P::UfRewake]==1&&tr->sleeps==1&&tr->touchedAsleep==1&&tr->kicks>=1,"unfreeze: the cup's trace: contacts, sources, sleeps, kicks");
      // tables under a resting palm (traces that never activate, each touched later) never evict an activated cup's trace
      {static unsigned char tables[40][0x100]{};const auto saved=D::state().cups;
       for(int i=0;i<40;++i){auto* t=D::cupTrace(D::state(),stub::addr(tables[i]),true);if(t)t->lastContactNs=simNs+std::uint64_t(i);}
       CHECK(D::cupTrace(D::state(),stub::addr(pp.inst),false)==tr,"cup traces: 40 tables touched after the cup: the cup's trace kept");
       D::state().cups=saved;}
      stub::levelColliders[10]=0;for(int i=0;i<40;++i)frame({.5f,1.2f,0});D::state().lev.unfreezeNear=true;}
     stub::put<std::uintptr_t>(pp.inst,0x18,0);stub::clearProps();for(int i=0;i<4;++i)frame({.5f,1.2f,0});}
    // pull out of the wall: released
    for(int i=0;i<10;++i)frame({.5f,1.2f,0});
    {const auto s=solvedRight();CHECK(s.age>6&&nearV(s.solved,{.5f,1.2f,0},3e-3f),"pulled out: free again");}
    // the fist part: squeeze over .65 toggles it (not while touching)
    {squeezeNow=.8f;frame({.5f,1.2f,0});CHECK(D::state().hand[1].fist&&P::objects[1].partFlags[2][0]==~0u&&P::objects[1].partFlags[1][0]==0u,"squeeze: the fist part on, the fingers off");
     squeezeNow=.5f;frame({.5f,1.2f,0});CHECK(D::state().hand[1].fist,"hysteresis: kept at .5");
     squeezeNow=0.f;frame({.5f,1.2f,0});CHECK(!D::state().hand[1].fist&&P::stats.hand[1].partToggles.load()==2,"open hand: the fingers back");}
    // stale targets: up to staleMs the body stays LIVE and keeps steering
    {for(int i=0;i<8;++i)staleFrame();
     CHECK(D::state().hand[1].machine.state==P::BodyLive&&P::stats.targetsHeld.load()>0,"targets 224 ms old: still live");
     for(int i=0;i<6;++i)staleFrame();
     CHECK(D::state().hand[1].machine.state==P::BodyGhost,"targets over 300 ms old: ghost");
     frame({.5f,1.2f,0});frame({.5f,1.2f,0});for(int i=0;i<10;++i)frame({.5f,1.2f,0});
     CHECK(D::state().hand[1].machine.state==P::BodyLive,"fresh again: live");}
    // pushed 45 cm into the wall: parked after 600 ms; the hand back next to it returns in place; or from the chest
    {const auto pg0=P::stats.hand[1].parkGhosts.load(),ir0=P::stats.hand[1].inPlaceReturns.load();for(int i=0;i<30;++i)frame({1.45f,1.2f,0});
     CHECK(D::state().hand[1].machine.state==P::BodyPark&&P::stats.hand[1].parkGhosts.load()==pg0+1,"45 cm past the wall for 600 ms -> park");
     CHECK(!D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,stub::props[1].inst,nullptr,nullptr),"a parked body drops its pairs");
     const P::V3 park=D::state().hand[1].parkPos;
     CHECK(nearV(D::state().hand[1].parkN,{-1,0,0},1e-3f)&&near(solvedRight().solved[0],park[0],1e-4f),"parked where it stopped, its contact normal kept");
     for(int i=0;i<3;++i)frame({1.45f,1.2f,0});CHECK(near(solvedRight().solved[0],park[0],1e-4f),"stands still while parked");
     const int tp=stub::teleports;for(int i=0;i<6;++i)frame({park[0]-.05f,1.2f,0});
     CHECK(D::state().hand[1].machine.state==P::BodyLive&&P::stats.hand[1].inPlaceReturns.load()==ir0+1&&stub::teleports==tp,"the hand back in front of the parked body: live in place, no teleport");
     for(int i=0;i<30;++i)frame({1.45f,1.2f,0});CHECK(D::state().hand[1].machine.state==P::BodyPark,"parked again");
     frame({.1f,1.3f,0});CHECK(stub::teleports==tp+1&&D::state().hand[1].machine.state==P::BodyTravel&&P::stats.hand[1].chestReturns.load()==1,"the hand at the body: from the chest");
     for(int i=0;i<12;++i)frame({.5f,1.2f,0});CHECK(D::state().hand[1].machine.state==P::BodyLive,"live again");}
    // ---------- REACH WAKE ----------
    {wallOn=false;stub::clearProps();for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});
     const P::V3 palm=solvedRight().solved;
     // near (10 cm), edge (gap 1 cm), far (60 cm), near but active, near door, near infinite mass, two more near ones
     stub::placeProp(stub::props[0],8,stub::navVtable,V::add(palm,{.10f,0,0}),.04f,1);
     stub::placeProp(stub::props[1],9,stub::navVtable,V::add(palm,{0,.30f,0}),.04f,1);
     stub::placeProp(stub::props[2],10,stub::navVtable,V::add(palm,{0,0,.60f}),.04f,1);
     stub::placeProp(stub::props[3],11,stub::navVtable,V::add(palm,{-.10f,0,0}),.04f,0);
     stub::placeProp(stub::props[4],12,stub::navVtable,V::add(palm,{0,-.10f,0}),.04f,1,true);
     stub::placeProp(stub::props[5],13,stub::deadVtable,V::add(palm,{0,.12f,0}),.04f,1,false,true);
     stub::placeProp(stub::props[6],14,stub::deadVtable,V::add(palm,{0,0,.15f}),.04f,1);
     stub::placeProp(stub::props[7],15,stub::navVtable,V::add(palm,{0,0,-.18f}),.04f,1);
     auto& R=P::stats.reach;const auto q0=R.queries.load(),w0=R.woken.load();const int f0=stub::forceActivations;
     D::state().reachTick=0;D::state().reachLive=0;frame({.5f,1.2f,.3f});
     CHECK(R.queries.load()==q0+1&&stub::culls>=1&&near(stub::lastCullR,.25f,.02f),"a live palm: one cull, radius 25 cm (still hand)");
     CHECK(R.woken.load()==w0+2&&stub::forceActivations==f0+2&&stub::propState(8)==0&&stub::propState(14)==0,"the two nearest inactive props woken (wakemax 2)");
     CHECK(stub::propState(15)==1&&R.rateLimited.load()>=1,"the third near one waits for the next query");
     CHECK(stub::propState(9)==1&&stub::propState(10)==1,"1 cm out of reach and 60 cm away: not woken");
     CHECK(R.already.load()>=1&&stub::propState(12)==1&&stub::propState(13)==1&&R.infinite.load()>=1&&R.skipped.load()>=1,"active, door, infinite mass: not woken");
     const auto q1=R.queries.load();for(int i=0;i<5;++i)frame({.5f,1.2f,.3f});
     CHECK(R.queries.load()==q1,"no query for the next 5 ticks");
     {const auto s0=R.still.load();frame({.5f,1.2f,.3f});CHECK(R.queries.load()==q1&&R.still.load()==s0+1,"the 6th tick, the palm still: no query (it found what it could)");}
     for(int i=0;i<5;++i)frame({.5f,1.2f,.36f});
     frame({.5f,1.2f,.36f});CHECK(R.queries.load()==q1+1&&stub::propState(15)==0,"the palm moved 6 cm: a query, the third prop woken");
     for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});
     // the woken props settle and sleep: no wake inside 1 s of their last one, then woken again
     const auto again=[&]{D::state().reachTick=0;D::state().reachLive=0;}; // the next tick queries
     stub::setPropState(8,1);const int f1=stub::forceActivations;const auto l0=R.lruHits.load();
     again();frame({.5f,1.2f,.3f});
     CHECK(stub::forceActivations==f1&&R.lruHits.load()>l0,"asleep again inside 1 s: the LRU holds it");
     {const auto q3=R.queries.load();for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
      CHECK(R.queries.load()==q3,"30 ticks of a still palm: no query");
      for(int i=0;i<30;++i)frame({.5f,1.2f,.3f+.012f*float(i+1)});
      CHECK(R.queries.load()==q3+5,"30 ticks moving 1.2 cm per tick: a query every 6 ticks");
      for(int i=0;i<12;++i)frame({.5f,1.2f,.3f});}
     simNs+=1100*ms;stub::setPropState(8,1);again();{const int f2=stub::forceActivations;frame({.5f,1.2f,.3f});
     CHECK(stub::forceActivations==f2+1&&stub::propState(8)==0,"after 1 s: woken again");}(void)f1;
     // a fast reach: the lead wakes a prop 40 cm ahead before the hand gets there
     stub::clearProps();stub::placeProp(stub::props[0],8,stub::navVtable,V::add(palm,{0,0,.40f}),.04f,1);
     stub::placeProp(stub::props[1],9,stub::navVtable,V::add(palm,{.30f,0,.10f}),.04f,1);
     handVel={0,0,2.f};simNs+=1100*ms;const auto w1=R.woken.load(),o1=R.outOfReach.load();frame({.5f,1.2f,.3f});again();for(int i=0;i<3;++i)frame({.5f,1.2f,.3f});
     CHECK(R.woken.load()==w1+1&&stub::propState(8)==0&&stub::lastCullR>.30f,"moving at 2 m/s: the prop 40 cm ahead woken (100 ms lead)");
     CHECK(stub::propState(9)==1&&R.outOfReach.load()>o1,"inside the cull sphere but 30 cm beside the reach: not woken");
     handVel={0,0,0};
     // levers: reachwake 0, a ghost hand: no query
     stub::setPropState(8,1);const auto q2=R.queries.load();
     frame({.5f,1.2f,.3f},P::ClassHeld);for(int i=0;i<4;++i)frame({.5f,1.2f,.3f},P::ClassHeld);
     CHECK(R.queries.load()==q2,"no live hand (holding an item): no query");
     for(int i=0;i<12;++i)frame({.5f,1.2f,.3f});
     std::printf("     reach wake: queries %llu, culls %llu, query avg %.2f us (host stub, cull + checks), max %.1f us\n",
        (unsigned long long)R.queries.load(),(unsigned long long)R.cullCalls.load(),double(R.queryNsSum.load())/double(std::max<std::uint64_t>(R.queries.load(),1))/1e3,double(R.queryNsMax.load())/1e3);
     stub::clearProps();wallOn=true;}
    // ---------- NPC bump ----------
    {P::BumpGate g{};const std::uint64_t t=100'000*ms;
     CHECK(g.update(7,true,t)&&!g.update(7,true,t+28*ms),"bump: one on the touch start");
     CHECK(g.update(7,true,t+1000*ms)&&g.update(7,true,t+2000*ms)&&!g.update(7,true,t+3000*ms)&&!g.update(7,true,t+5000*ms),"bump: at most 2 repeats 1 s apart");
     CHECK(!g.update(7,false,t+5100*ms)&&!g.update(7,true,t+6000*ms)&&g.update(7,true,t+7200*ms),"bump: a 2 s cooldown after the touch ends");
     CHECK(!g.update(8,true,t+7300*ms)&&g.update(8,true,t+7500*ms),"bump: 250 ms between any two bumps");
     CHECK(P::touchesCreature({.1f,1.f,0},{0,0,0},{0,1,0},false,1.f)&&!P::touchesCreature({.33f,1.f,0},{0,0,0},{0,1,0},false,1.f)&&
           P::touchesCreature({.33f,1.f,0},{0,0,0},{0,1,0},true,1.f)&&!P::touchesCreature({.1f,2.1f,0},{0,0,0},{0,1,0},false,1.f)&&
           !P::touchesCreature({.1f,.1f,0},{0,0,0},{0,1,0},false,1.f),"creature capsule: 0.32 m to start, 0.35 m to stay, 0.2..2 m above the feet");}
    {wallOn=false;stub::actors();actorAddr=stub::addr(stub::playerActor);for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});
     CHECK(P::nearWanted.load()&&P::nearScanTicks.load()==9,"a live body: VrMelee asked to scan every 9 ticks");
     const P::V3 palm=solvedRight().solved;
     const auto nearbyAt=[&](P::V3 feet,std::uint32_t guid){P::Nearby nb{};nb.ns=simNs+28*ms;nb.count=1;nb.c[0]={stub::addr(stub::npcActor),guid,feet,0};P::nearby.store(nb);};
     nearbyAt(V::add(palm,{.6f,-1.f,0}),stub::npcGuid);frame({.5f,1.2f,.3f});CHECK(!stub::bumpsSent,"a creature 60 cm away: no bump");
     // PASSIVE: the palm still (the player walked up to the creature): no bump
     {const auto fs0=P::stats.passive.fallbackSkipped.load();nearbyAt(V::add(palm,{.1f,-1.f,0}),stub::npcGuid);frame({.5f,1.2f,.3f});
      CHECK(!stub::bumpsSent&&P::stats.passive.fallbackSkipped.load()>fs0,"passive: a still palm at a creature (no virtual body): no bump");
      nearbyAt(V::add(palm,{.6f,-1.f,0}),stub::npcGuid);frame({.5f,1.2f,.3f});simNs+=3000*ms;}
     armOverride=true;armVel={.6f,0,0}; // the arm moves (0.6 m/s): the coarse touches bump
     nearbyAt(V::add(palm,{.1f,-1.f,0}),stub::npcGuid);frame({.5f,1.2f,.3f});
     CHECK(stub::bumpsSent==1&&stub::bumpActor==stub::addr(stub::npcActor)&&stub::bumpMsg[0]==0x2f&&stub::bumpMsg[1]==stub::playerGuid,
           "the palm on a creature: Broadcast(creature, {0x2f, the player's guid})");
     nearbyAt(V::add(palm,{.1f,-1.f,0}),stub::npcGuid);frame({.5f,1.2f,.3f});CHECK(stub::bumpsSent==1,"the touch goes on: no second bump inside 1 s");
     nearbyAt(V::add(palm,{.6f,-1.f,0}),stub::npcGuid);frame({.5f,1.2f,.3f});
     simNs+=3000*ms;stub::put<std::uint16_t>(stub::actorSlotTable,5*16+8,9);nearbyAt(V::add(palm,{.1f,-1.f,0}),stub::npcGuid);frame({.5f,1.2f,.3f});
     CHECK(stub::bumpsSent==1&&P::stats.npc.staleGuid.load()==1,"the creature's slot holds another generation: no bump");
     stub::put<std::uint16_t>(stub::actorSlotTable,5*16+8,2);nearbyAt(V::add(palm,{.6f,-1.f,0}),stub::npcGuid);frame({.5f,1.2f,.3f});
     simNs+=3000*ms;stub::put<std::uint32_t>(stub::npcHealth,0x150,0x40);stub::put<float>(stub::npcHealth,0x20,0.f);
     nearbyAt(V::add(palm,{.1f,-1.f,0}),stub::npcGuid);frame({.5f,1.2f,.3f});
     CHECK(stub::bumpsSent==1&&P::stats.npc.notAlive.load()==1,"a creature that died after the scan: no bump");
     armOverride=false;armVel={0,0,0};
     // ---------- NPC virtual body ----------
     stub::put<std::uint32_t>(stub::npcHealth,0x150,0u);stub::put<float>(stub::npcHealth,0x20,100.f);simNs+=3000*ms;
     P::Nearby none{};none.ns=1;P::nearby.store(none);
     const auto vbAt=[&](P::V3 axis,std::uint8_t kind,int race){P::VBodies vb{};vb.ns=simNs+28*ms;vb.count=1;auto& b=vb.b[0];
        b.actor=stub::addr(stub::npcActor);b.guid=stub::npcGuid;b.race=race;b.count=1;b.c[0]={{axis[0],axis[1]-.3f,axis[2]},{axis[0],axis[1]+.3f,axis[2]},.06f,1,kind};
        P::finishBody(b);P::vbodies.store(vb);};
     auto& N=P::stats.npc;
     const int bA=stub::bumpsSent;vbAt(V::add(palm,{.6f,0,0}),2,0);frame({.5f,1.2f,.3f});
     CHECK(D::state().vbFresh&&D::state().vb.count==1&&stub::bumpsSent==bA&&!D::state().hand[1].vhit.hit,"virtual body: loaded, a body 60 cm away: nothing");
     // PROPHIT: a held chair's contact with the creature's capsule (as the impact scan leaves it): a hit request at 3 m/s or more
     {auto& S=D::state();const auto g0=S.grab[1];const auto vb0=S.vb;const bool f0=S.vbFresh;
      static unsigned char moverInst[0x80]{},moverUd[0x30]{};stub::put<std::uintptr_t>(moverInst,0x18,stub::addr(moverUd));stub::put<std::uint8_t>(moverUd,0x18,0);
      stub::put<std::uintptr_t>(moverUd,0x8,stub::addr(stub::npcActor));stub::put<std::uint32_t>(stub::npcActor,8,stub::npcGuid);
      const P::Levers lv0=S.lev;S.lev.propHitGeo=false;S.lev.propHitMps=3.f;S.lev.propHitStrongMps=4.f; // the engine contact path
      S.grab[1]=P::GrabHold{};S.grab[1].active=true;S.grab[1].prop=0x5000;S.grab[1].cls=P::classNav;S.grab[1].propKg=59.5f;
      P::VBodies vb{};vb.count=1;vb.b[0].actor=stub::addr(stub::npcActor);vb.b[0].guid=stub::npcGuid;vb.b[0].race=0;S.vb=vb;S.vbFresh=true;
      const auto contact=[&](std::uintptr_t m,P::V3 vp){S.propContacts={};S.propContacts[0]=P::PropContact{m,1u,{1,1,0},{1,0,0},vp,.01f};};
      const auto newest=[]{P::PropHitRequest q{};const auto l=P::propHitLatest.load();if(l)P::propHitRing[l%P::propHitSlots].load(q);return q;};
      auto& PH=P::stats.propHit;const auto s0=PH.sent.load();const P::PropHitRequest r0=newest();
      contact(stub::addr(moverInst),{4.5f,0,0});D::propHitTick(S,simNs);const P::PropHitRequest r=newest();
      CHECK(D::propScanInst[1]==0x5000&&PH.sent.load()==s0+1&&r.serial!=r0.serial&&r.guid==stub::npcGuid&&r.strong==1&&near(r.speed,4.5f)&&nearV(r.dir,{1,0,0},1e-4f)&&
            r.hands==2,"prop hit: a 59.5 kg chair meeting the creature at 4.5 m/s: a strong hit along the swing (the scan armed)");
      simNs+=20*ms;contact(stub::addr(moverInst),{4.5f,0,0});const auto ne=PH.why[P::PropHitNotEdge].load();D::propHitTick(S,simNs);
      CHECK(PH.why[P::PropHitNotEdge].load()==ne+1&&PH.sent.load()==s0+1,"prop hit: the same contact a tick later: no second hit");
      simNs+=1000*ms;contact(stub::addr(moverInst),{2.f,0,0});const auto sl=PH.why[P::PropHitSlow].load();D::propHitTick(S,simNs);
      CHECK(PH.why[P::PropHitSlow].load()==sl+1&&PH.sent.load()==s0+1,"prop hit: a 2 m/s push of the chair: no hit");
      simNs+=1000*ms;contact(S.moverInstAddr,{5.f,0,0});const auto pl=PH.why[P::PropHitPlayer].load();D::propHitTick(S,simNs);
      CHECK(S.moverInstAddr&&PH.why[P::PropHitPlayer].load()==pl+1&&PH.sent.load()==s0+1,"prop hit: the player's own capsule: never");
      // a creature running at 4 m/s into the chair held still: the approach is its own speed, the chair is not swung
      {const auto ns0=PH.why[P::PropHitNotSwung].load();simNs+=1000*ms;S.vb.b[0].vel={-4.f,0,0};contact(stub::addr(moverInst),{0,0,0});D::propHitTick(S,simNs);
       CHECK(PH.why[P::PropHitNotSwung].load()==ns0+1&&PH.sent.load()==s0+1,"prop hit: a creature running into a still chair: no hit (notSwung)");
       S.vb.b[0].vel={0,0,0};}
      // the player running at 4.5 m/s with the chair held still in the hands: the swing against the player's mover is 0
      {const auto ns0=PH.why[P::PropHitNotSwung].load();const auto mv0=S.hand[1].moverVel;simNs+=1000*ms;S.hand[1].moverVel={4.5f,0,0};
       contact(stub::addr(moverInst),{4.5f,0,0});D::propHitTick(S,simNs);
       CHECK(PH.why[P::PropHitNotSwung].load()==ns0+1&&PH.sent.load()==s0+1,"prop hit: a chair carried at a run into a creature: no hit (notSwung)");
       S.hand[1].moverVel=mv0;}
      // the same run with the chair swung forward at 4 m/s on top: a hit
      {const auto mv0=S.hand[1].moverVel;simNs+=1000*ms;S.hand[1].moverVel={4.5f,0,0};contact(stub::addr(moverInst),{8.5f,0,0});D::propHitTick(S,simNs);
       CHECK(PH.sent.load()==s0+2&&near(newest().speed,8.5f),"prop hit: a chair swung at 4 m/s while running: a hit");S.hand[1].moverVel=mv0;}
      S.grab[1].propKg=4.f;D::propHitTick(S,simNs);CHECK(D::propScanInst[1]==0,"prop hit: a prop under prophitkg: not scanned");
      S.lev=lv0;S.grab[1]=g0;S.vb=vb0;S.vbFresh=f0;S.propContacts={};D::propScanInst={};}
     const int b0=stub::bumpsSent;const auto vs0=P::stats.hand[1].virtualSubsteps.load(),ps0=N.pressStarts.load(),sl0=N.slow.load();
     for(int i=0;i<4;++i){vbAt(V::add(palm,{.08f,0,0}),2,0);frame({.5f,1.2f,.3f});}
     {const auto sr=solvedRight();
      CHECK(near(sr.solved[0],palm[0]-.02f,3e-3f)&&sr.age<=1&&nearV(sr.n,{-1,0,0},1e-3f),"virtual body: the hand stops 5 mm outside the chest capsule, the contact published");
      CHECK(P::stats.hand[1].virtualSubsteps.load()>vs0&&near(D::state().hand[1].command[0],palm[0],3e-3f),"virtual body: the contact from the clip, the raw command kept");}
     CHECK(stub::bumpsSent==b0&&N.pressStarts.load()==ps0&&P::stats.passive.bumpSkippedPassive.load()>0,
           "passive: a still palm held in the body (the player walked into it): no press bump, no touch bump");
     CHECK(N.slow.load()==sl0+1,"press: a slow touch is no slap");
     // lever npcpassive 0: every tick of a palm in the body bumps (no arm gate)
     {static const auto old=+[](P::Levers& l){l.npcPassive=false;};P::leverPatchForTests.store(old);D::state().leversRead=false;
      for(int i=0;i<4;++i){vbAt(V::add(palm,{.08f,0,0}),2,0);frame({.5f,1.2f,.3f});}
      CHECK(stub::bumpsSent>=b0+3&&stub::bumpActor==stub::addr(stub::npcActor)&&stub::bumpMsg[0]==0x2f&&N.pressStarts.load()==ps0+1,"press (npcpassive 0): a bump every tick while pressing");
      const auto r0=N.pressRests.load();for(int i=0;i<72;++i){vbAt(V::add(palm,{.08f,0,0}),2,0);frame({.5f,1.2f,.3f});}
      const int b1=stub::bumpsSent;for(int i=0;i<10;++i){vbAt(V::add(palm,{.08f,0,0}),2,0);frame({.5f,1.2f,.3f});}
      CHECK(N.pressRests.load()==r0+1&&stub::bumpsSent==b1,"press (npcpassive 0): 2 s of bumps, then a rest");
      P::leverPatchForTests.store(nullptr);D::state().leversRead=false;}
     // ---------- open-hand touches (TouchRequest per hand) ----------
     const P::Rows palmX{{{0,1,0},{0,0,1},{1,0,0}}}; // the palm-out normal towards +x (the bodies)
     const auto vbSide=[&](P::V3 axis,std::uint8_t kind,P::V3 rowA){P::VBodies vb{};vb.ns=simNs+28*ms;vb.count=1;auto& b=vb.b[0];
        b.actor=stub::addr(stub::npcActor);b.guid=stub::npcGuid;b.race=0;b.count=1;b.rowA=rowA;b.c[0]={{axis[0],axis[1]-.3f,axis[2]},{axis[0],axis[1]+.3f,axis[2]},.06f,1,kind};
        P::finishBody(b);P::vbodies.store(vb);};
     const auto touchNow=[]{P::TouchRequest r{};P::touchRequest[1].load(r);return r;};
     const auto away=[&](int n){for(int i=0;i<n;++i){vbAt(V::add(palm,{.6f,0,0}),0,0);frame({.5f,1.2f,.3f});}};
     rowsNow=palmX;away(6);simNs+=1000*ms;
     {const auto t0=touchNow();const auto sl0=N.slaps.load();
      const auto sh0=N.slapHeld.load(),ss0=N.slapHeldSent.load();
      handVel={3.f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),0,{1,0,0});frame({.5f,1.2f,.3f});}handVel={0,0,0};
      CHECK(N.slapHeld.load()==sh0+1&&N.slaps.load()==sl0&&touchNow().serial==t0.serial,"touch: a slap verdict waits slapholdms for the other hand (nothing handed over yet)");
      CHECK(solvedRight().solved[0]<palm[0]+.03f,"touch: the slapping hand stops on the near side of the head");
      away(4);
      const auto r=touchNow();
      CHECK(N.slapHeldSent.load()==ss0+1&&N.slaps.load()==sl0+1&&r.serial>t0.serial&&r.kind==P::TouchSlap&&r.guid==stub::npcGuid&&r.actor==stub::addr(stub::npcActor)&&r.hand==1&&r.speed>=2.2f&&
            r.part==0&&nearV(r.n,{1,0,0},1e-3f)&&r.facing>.9f&&r.side>.9f&&r.pushGrade==1&&r.pair==0,"touch: a fast open hand on the head from the side: a slap once the hold passed with no second hand");
      CHECK(r.slapReact==1&&r.push==1&&near(r.pushMaxM,.025f)&&near(r.upm,1.f)&&r.attacker==P::OpenMsgSelf&&!r.windowed,"touch: the levers copied, the slap's message from the creature itself");}
     // the palm thrust into the chest face-on: a shove (strong at 3 m/s), its attacker the creature itself (no hostility)
     away(4);simNs+=1000*ms;
     {const auto sh0=N.shoves.load();handVel={3.f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),2,{0,0,1});frame({.5f,1.2f,.3f});}handVel={0,0,0};
      const auto r=touchNow();CHECK(N.shoves.load()==sh0+1&&r.kind==P::TouchShove&&r.strong==0&&std::fabs(r.side)<.1f&&r.attacker==1&&r.pushGrade==1&&near(r.a,3.f,.2f)&&
            near(r.pushDownMps,2.8f),"touch: the palm thrust into the chest: a shove (graded: VrMelee decides stumble / knockdown from aEff; never KNCKBK)");}
     // the palm into the face at 3 m/s: a slap (no side gate on the head)
     away(4);simNs+=1000*ms;
     {const auto sl0=N.slaps.load();handVel={3.f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),0,{0,0,1});frame({.5f,1.2f,.3f});}handVel={0,0,0};
      away(4);CHECK(N.slaps.load()==sl0+1&&touchNow().kind==P::TouchSlap,"touch: a fast palm into the face: a slap");}
     // a 1.8 m/s slap of the head (under the edge's 2.2): the window decides it when the hand leaves
     away(4);simNs+=1000*ms;
     {const auto w0=N.windows.load(),ws0=N.windowSlaps.load();handVel={1.8f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),0,{1,0,0});frame({.5f,1.2f,.3f});}
      CHECK(N.windows.load()==w0+1&&N.windowSlaps.load()==ws0,"touch: a 1.8 m/s touch of the head: the slap window opened");
      handVel={-1.5f,0,0};away(3);handVel={0,0,0};
      const auto r=touchNow();CHECK(N.windowSlaps.load()==ws0+1&&r.kind==P::TouchSlap&&r.windowed==1&&r.attacker==1,"touch: the hand left the head: the window's slap (never as the player: the creature itself)");}
     // the chest face-on at 2 m/s: a shove; again within 300 ms: gated (slaps and shoves share the gate)
     away(4);simNs+=1000*ms;
     {const auto sh0=N.shoves.load();handVel={2.f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),2,{0,0,1});frame({.5f,1.2f,.3f});}handVel={0,0,0};
      const auto r=touchNow();CHECK(N.shoves.load()==sh0+1&&r.kind==P::TouchShove&&r.strong==0&&r.capKind==2,"touch: the chest face-on at 2 m/s: a shove");
      const auto g0=N.touchGated.load();away(2);
      handVel={2.f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),2,{0,0,1});frame({.5f,1.2f,.3f});}handVel={0,0,0};
      CHECK(N.touchGated.load()==g0+1&&N.shoves.load()==sh0+1,"touch: a second touch inside the gate: none");}
     // edge-on (the palm along the body): no touch
     away(4);simNs+=1000*ms;
     {rowsNow=identity;away(4);const auto s0=N.slow.load(),k0=N.slaps.load()+N.shoves.load();
      handVel={3.f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),2,{1,0,0});frame({.5f,1.2f,.3f});}handVel={0,0,0};
      CHECK(N.slow.load()==s0+1&&N.slaps.load()+N.shoves.load()==k0,"touch: an edge-on hand on the chest (|facing| < 0.25): no slap, no shove");
      away(4);simNs+=1000*ms;const auto sl0=N.slaps.load(),hf0=N.slapHeadFast.load();
      handVel={3.f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),0,{1,0,0});frame({.5f,1.2f,.3f});}handVel={0,0,0};away(4);
      CHECK(N.slaps.load()==sl0+1&&N.slapHeadFast.load()==hf0+1&&touchNow().kind==P::TouchSlap,"touch: an edge-on hand fast at the head: a slap (no facing gate there)");
      rowsNow=palmX;away(6);}
     // a slow palm held in the chest: a press every tick after 100 ms
     simNs+=1000*ms;
     // (lever presspush 3 cm: the 2 cm press asks it within the window)
     {static const auto push3=+[](P::Levers& l){l.pressPushM=.03f;};P::leverPatchForTests.store(push3);D::state().leversRead=false;
      const auto p0=N.presses.load(),pt0=N.pressTicks.load(),e00=N.escalations.load(),st00=N.escalateStill.load();
      for(int i=0;i<3;++i){vbSide(V::add(palm,{.12f,0,0}),2,{0,0,1});frame({.5f,1.2f,.3f});}
      for(int i=0;i<8;++i){vbSide(V::add(palm,{.08f,0,0}),2,{0,0,1});frame({.5f,1.2f,.3f});}
      const auto r=touchNow();
      CHECK(N.presses.load()==p0+1&&N.pressTicks.load()>=pt0+3&&r.kind==P::TouchPress&&near(r.depthM,.02f,.004f)&&r.facing>.9f,"touch: a palm held 2 cm in the chest: a press every tick");
      CHECK(N.escalations.load()==e00&&(N.escalateStill.load()>st00||P::stats.passive.pressStepZeroArm.load()>0),
            "touch: the press with the arm still (the creature against a still palm): never escalated to a push");
      CHECK(near(r.armStepM,0.f)&&P::stats.passive.pressStepZeroArm.load()>0,"passive: the still palm's press carries no arm step (VrMelee moves the creature 0 cm)");
      // the arm pushing on at 0.5 m/s (past the 0.2 m/s gate): the press steps by the arm's whole drive (0.5 x 2 substeps of 1/72 s = 13.9 mm) and bumps
      {const int bb=stub::bumpsSent;const auto pa0=P::stats.passive.pressArmSent.load();armOverride=true;armVel={.5f,0,0};
       for(int i=0;i<3;++i){vbSide(V::add(palm,{.08f,0,0}),2,{0,0,1});frame({.5f,1.2f,.3f});}
       const auto ra=touchNow();armOverride=false;armVel={0,0,0};
       CHECK(ra.kind==P::TouchPress&&near(ra.armStepM,.5f*2.f/72.f,1e-3f)&&P::stats.passive.pressArmSent.load()>=pa0+2&&stub::bumpsSent>bb,
             "passive: a palm the arm drives in slowly: the press steps by the arm's drive and bumps");
       CHECK(near(P::pressArmStepM(.02f,ra.armStepM),ra.armStepM)&&near(P::pressArmStepM(.002f,ra.armStepM),.002f)&&near(P::pressArmStepM(.02f,-1.f),.02f),
             "passive: VrMelee's press step: the depth law's, at most the arm's");}
      // the arm's step flickering (0.5 m/s one frame, still the next): the press stays arm-driven for 300 ms, its bump gate never restarts
      {const auto ps0=N.pressStarts.load(),ph0=P::stats.passive.pressArmHeld.load();armOverride=true;
       for(int i=0;i<8;++i){armVel=i%2?P::V3{0,0,0}:P::V3{.5f,0,0};vbSide(V::add(palm,{.08f,0,0}),2,{0,0,1});frame({.5f,1.2f,.3f});}
       armOverride=false;armVel={0,0,0};
       CHECK(N.pressStarts.load()==ps0&&P::stats.passive.pressArmHeld.load()>ph0,"passive: a press whose arm step flickers: held arm-driven, no new press start");}
      P::leverPatchForTests.store(nullptr);D::state().leversRead=false;}
     // the hand carried into the chest at 3 m/s by the player's turn (the controller still in the room): no shove, no bump
     away(4);simNs+=1000*ms;
     {const auto sh0=N.shoves.load(),sa0=P::stats.passive.shoveRefusedArm.load();const auto t0=touchNow();const int bb=stub::bumpsSent;
      armOverride=true;armVel={0,0,0};handVel={3.f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),2,{0,0,1});frame({.5f,1.2f,.3f});}handVel={0,0,0};
      for(int i=0;i<3;++i){vbSide(V::add(palm,{.08f,0,0}),2,{0,0,1});frame({.5f,1.2f,.3f});}armOverride=false;
      CHECK(N.shoves.load()==sh0&&(P::stats.passive.shoveRefusedArm.load()>sa0||N.slow.load()>0)&&stub::bumpsSent==bb&&
            (touchNow().serial==t0.serial||touchNow().kind==P::TouchPress),"passive: a palm carried into the chest by the player's motion, the arm still: no shove, no bump");
      CHECK(touchNow().serial==t0.serial||near(touchNow().armStepM,0.f),"passive: ... and its press moves nothing");
      away(4);bool ring=false;for(const auto& r:D::state().passiveRing)ring=ring||(r.set&&r.turnMps>2.f&&r.armInMax<.1f&&r.bumpsSent==0&&near(r.armStepCm,0.f));
      CHECK(ring,"passive: the episode ring: the turn part 3 m/s, the arm still, no bump, no arm step");}
     // a slap whose contact stays and deepens: reclassified as a press
     away(4);simNs+=1000*ms;
     {const auto rc0=N.reclassified.load(),sl0=N.slaps.load();
      handVel={3.f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),0,{1,0,0});frame({.5f,1.2f,.3f});}handVel={0,0,0};
      for(int i=0;i<8;++i){vbSide(V::add(palm,{.08f-.004f*float(i),0,0}),0,{1,0,0});frame({.5f,1.2f,.3f});}
      CHECK(N.slaps.load()==sl0+1&&N.reclassified.load()==rc0+1&&(touchNow().kind==P::TouchPress||touchNow().escalated==1),
            "touch: a slap held and pressed deeper: a press (escalating to a push once it asked 6 cm)");}
     // a fist: no touch request (the fist path is VrMelee's punch)
     away(4);simNs+=1000*ms;
     {squeezeNow=.8f;away(2);const auto t0=touchNow();const auto ch0=N.closedHand.load();
      handVel={3.f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),0,{1,0,0});frame({.5f,1.2f,.3f});}handVel={0,0,0};
      for(int i=0;i<4;++i){vbSide(V::add(palm,{.08f,0,0}),2,{0,0,1});frame({.5f,1.2f,.3f});}
      CHECK(D::state().hand[1].fist&&N.closedHand.load()==ch0+1&&touchNow().serial==t0.serial,"touch: a closed fist asks for nothing");
      squeezeNow=0.f;away(6);}
     // a horse: the body and the press bumps, never a touch
     simNs+=3000*ms;
     {const auto hs0=N.horse.load();const auto t0=touchNow();const int b2=stub::bumpsSent;
      handVel={3.f,0,0};for(int i=0;i<2;++i){vbAt(V::add(palm,{.08f,0,0}),0,1);frame({.5f,1.2f,.3f});}handVel={0,0,0};
      for(int i=0;i<3;++i){vbAt(V::add(palm,{.08f,0,0}),0,1);frame({.5f,1.2f,.3f});}
      CHECK(N.horse.load()==hs0+1&&touchNow().serial==t0.serial&&stub::bumpsSent>b2,"horse: no touch, the press bumps");
      CHECK(P::nearHorses.load(),"horse: VrMelee asked to keep horses for the hands");}
     // a creature walking fast into a still open hand: no touch (a slap or a shove is the arm's own swing)
     {away(4);simNs+=3000*ms;const auto sw0=N.slow.load();const auto t0=touchNow();
      const auto vbMoving=[&](P::V3 axis){P::VBodies vb{};vb.ns=simNs+28*ms;vb.count=1;auto& b=vb.b[0];
         b.actor=stub::addr(stub::npcActor);b.guid=stub::npcGuid;b.race=0;b.count=1;b.vel={-3.f,0,0};b.rowA={0,0,1};
         b.c[0]={{axis[0],axis[1]-.3f,axis[2]},{axis[0],axis[1]+.3f,axis[2]},.06f,1,2};P::finishBody(b);P::vbodies.store(vb);};
      // published 23 cm out: moved on by vel * 50 ms (the extrapolation cap) it meets the hand 8 cm out, as the still bodies above
      for(int i=0;i<2;++i){vbMoving(V::add(palm,{.23f,0,0}));frame({.5f,1.2f,.3f});}
      CHECK(N.slow.load()==sw0+1&&touchNow().serial==t0.serial&&D::state().hand[1].vhitApproach>=2.2f,"touch: a creature walking into a still hand: none");
      away(4);}
     // TOUCH refused (VrMelee's side not ready): no requests
     {P::touchReady.store(false);simNs+=3000*ms;const auto t0=touchNow();
      handVel={3.f,0,0};for(int i=0;i<2;++i){vbSide(V::add(palm,{.08f,0,0}),0,{1,0,0});frame({.5f,1.2f,.3f});}handVel={0,0,0};
      CHECK(touchNow().serial==t0.serial,"touch: TOUCH not ready: no requests");P::touchReady.store(true);away(4);rowsNow=identity;away(4);}
     // lever npcbody 0: no clip
     {P::Levers lv=D::state().lev;D::state().lev.npcBody=false;const auto v1=P::stats.hand[1].virtualSubsteps.load();
      for(int i=0;i<3;++i){vbAt(V::add(palm,{.08f,0,0}),2,0);frame({.5f,1.2f,.3f});}
      CHECK(P::stats.hand[1].virtualSubsteps.load()==v1&&!D::state().vbFresh,"lever npcbody 0: no virtual body");D::state().lev=lv;}
     for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});
     actorAddr=0x4242;wallOn=true;}
    // ---------- REACH WAKE skips a prop whose root is fixed ----------
    {wallOn=false;stub::clearProps();for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});const P::V3 palm=solvedRight().solved;auto& R=P::stats.reach;
     stub::placeProp(stub::props[0],8,stub::navVtable,V::add(palm,{.10f,0,0}),.04f,1,false,false,0x1u);
     const auto s0=R.skipRoot.load();const int f0=stub::forceActivations;simNs+=1100*ms;D::state().reachTick=0;D::state().reachLive=0;frame({.5f,1.2f,.3f});
     CHECK(R.skipRoot.load()==s0+1&&stub::forceActivations==f0&&stub::propState(8)==1,"reach wake: a fixed root is not woken (ForceActivation would refuse it)");
     stub::clearProps();wallOn=true;}
    // ---------- KNOCK OFF: the collide filter drops (or counts) the hand's pairs with a knocked-down creature's frag within its window ----------
    {wallOn=false;stub::clearProps();for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});
     alignas(16) static void* creatureVt[0x50]{};creatureVt[2]=reinterpret_cast<void*>(&stub::fragClass);
     auto& cp=stub::props[3];stub::placeProp(cp,12,reinterpret_cast<void**>(creatureVt),V::add(solvedRight().solved,{.05f,0,0}),.3f,0);
     const auto pair=[&]{return D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,cp.inst,nullptr,nullptr);};
     const auto wp0=P::stats.hand[1].worldPairs.load(); // a creature frag pair counts as a world pair: put back for the report check below
     for(auto& k:P::knockOff){k.until.store(0);}
     CHECK(pair(),"knock off: a creature frag (class 5) pair is kept outside a window");
     const auto now0=D::state().lastTickNs;auto& k=P::knockOff[1];
     k.pairsEarly.store(0);k.pairsLate.store(0);k.towardMmsMax.store(0);k.armedNs.store(now0);k.inst.store(stub::addr(cp.inst));k.actor.store(0);
     k.dir[0].store(1.f);k.dir[1].store(0.f);k.dir[2].store(0.f);k.drop.store(true);
     k.until.store(now0+1500*ms);P::knockOffLive.store(0);
     {const auto d=P::stats.knockOffDrops.load(),kk=P::stats.knockOffKept.load();
      CHECK(pair()&&P::stats.knockOffDrops.load()==d&&P::stats.knockOffKept.load()==kk&&k.pairsEarly.load()==0,"knock off: no live bit: the filter skips the table (idle)");}
     P::knockOffSetUntil(1,now0+1500*ms);
     const auto d0=P::stats.knockOffDrops.load(),k0=P::stats.knockOffKept.load();
     D::state().hand[1].vset={-1.2f,0,0};
     CHECK(!pair()&&P::stats.knockOffDrops.load()==d0+1&&k.pairsEarly.load()==1&&k.towardMmsMax.load()==1200,
           "knock off: inside the window the pair is dropped, counted early, the hand's speed toward the pusher kept");
     k.drop.store(false);CHECK(pair()&&P::stats.knockOffKept.load()==k0+1&&k.pairsEarly.load()==2,"knock off: lever 0 keeps the pair and only counts it");
     P::knockOffSetUntil(1,now0);CHECK(pair()&&P::stats.knockOffDrops.load()==d0+1,"knock off: past the window the pair is kept");
     P::knockOffSetUntil(1,0);P::stats.hand[1].worldPairs.store(wp0);stub::clearProps();for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});wallOn=true;}
    // ---------- GLASS: a pane contact read from the hand's impact iterator (PreApplyImpacts' layout), the break call in the tick ----------
    {wallOn=false;stub::clearProps();stub::actors();actorAddr=stub::addr(stub::playerActor);for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});
     auto& G=P::stats.glass;auto& pane=stub::props[2];
     alignas(16) static void* paneVt[0x50]{};paneVt[2]=reinterpret_cast<void*>(&stub::fragClass);paneVt[0x218/8]=reinterpret_cast<void*>(stub::base+P::off::fnCollisionDamage);
     alignas(16) static unsigned char type[0x200]{},childList[0x20]{},child[0x20]{},groupList[0x20]{},group[0x60]{},mf[0x600]{},it[0x30]{};static std::uintptr_t arr[1]{};
     const P::V3 palm=solvedRight().solved;const P::V3 pt=V::add(palm,{.05f,0,0});
     const auto setup=[&]{stub::placeProp(pane,11,reinterpret_cast<void**>(paneVt),pt,.5f,2);stub::put<std::uintptr_t>(pane.inst,0x98,stub::addr(type));
        stub::put<std::uint8_t>(type,0x1b3,3);stub::put<std::uintptr_t>(type,0x100,stub::addr(childList));stub::put<std::uintptr_t>(childList,16,stub::addr(child));
        stub::put<std::uint8_t>(child,0x10,1);stub::put<std::uint8_t>(type,0x1b2,2);stub::put<std::uintptr_t>(type,0xf8,stub::addr(groupList));
        stub::put<std::uintptr_t>(groupList,8,stub::addr(group));stub::put<std::uint8_t>(group,0x53,2);stub::put<float>(group,0x54,1.f);stub::put<float>(group,0x58,1.f);
        std::memset(mf,0,sizeof mf);stub::put<std::uint8_t>(mf,0x5ac,1);
        const auto c=mf+0x10;stub::put<std::uint8_t>(c,0xce,1);stub::put<float>(c,0xc,.01f);stub::put<std::uintptr_t>(c,0xc0,stub::addr(mf));
        const float pa[4]{palm[0],palm[1],palm[2],0},pb[4]{pt[0],pt[1],pt[2],0},n[4]{1,0,0,0};std::memcpy(c+0x30,pa,16);std::memcpy(c+0x40,pb,16);std::memcpy(c+0x50,n,16);
        stub::put<std::uint16_t>(c,0xca,5);stub::put<std::uintptr_t>(mf,0x550,stub::addr(P::objects[1].inst));stub::put<std::uintptr_t>(mf,0x558,stub::addr(pane.inst));
        stub::put<std::uint8_t>(mf,0x5a9,2);arr[0]=stub::addr(mf);std::memset(it,0,sizeof it);stub::put<std::uintptr_t>(it,0,stub::addr(arr));
        stub::put<std::int32_t>(it,0x24,1);stub::put<std::uintptr_t>(it,0x10,stub::addr(P::objects[1].inst));
        D::state().hand[1].glassTracks={};for(auto& g:D::state().hand[1].glass)g=P::GlassHit{};}; // a new contact (the track runs on the real clock)
     // a fast hit (the controller at 3 m/s into the pane): the engine's break call with the projectile's Collision
     setup();const auto c0=stub::glassCalls;const auto d0=G.done.load();
     handVel={3.f,0,0};publish({.5f,1.2f,.3f},P::ClassBody);simNs+=28*ms;++simIndex;P::tick(MT::Frame{simNs,simIndex});
     for(unsigned h=0;h<2;++h)D::preUpdate(&P::objects[h].beh,1.f/72.f);D::preApplyGuard(it);for(unsigned h=0;h<2;++h)D::update(&P::objects[h].beh,1.f/72.f);
     {const auto& g=D::state().hand[1].glass[0];
      CHECK(g.pending&&g.comp==2&&g.element==5&&near(g.approach,3.f,.05f)&&nearV(g.n,{-1,0,0},1e-5f)&&!D::state().hand[1].glass[1].pending,
            "glass: the pane's contact read from the hand's iterator (component, element, the normal toward the hand, the approach)");}
     frame({.5f,1.2f,.3f});handVel={0,0,0};
     CHECK(stub::glassCalls==c0+1&&G.done.load()==d0+1&&stub::lastGlassIt==nullptr&&near(stub::at<float>(stub::lastCollision,0x34),100.f)&&
           stub::at<std::int32_t>(stub::lastCollision,0x38)==2&&stub::at<std::int32_t>(stub::lastCollision,0x3c)==5&&stub::at<std::uintptr_t>(stub::lastCollision,0x40)==0&&
           stub::at<std::uint8_t>(stub::lastCollision,0x54)==0xff&&near(stub::at<float>(stub::lastCollision,0),pt[0]),
           "glass: a 3 m/s hit: CollisionDamage(pane, Collision {pos, n, damage 100, comp 2, element 5, no other, 0xff}, null iterator), a pane made");
     {bool zero=true;for(int i=0x20;i<0x34;++i)zero=zero&&stub::lastCollision[i]==0;
      CHECK(stub::at<std::uint32_t>(stub::lastCollision,0x50)==stub::playerGuid&&near(stub::at<float>(stub::lastCollision,0x10),-1.f)&&zero,
            "glass: the Collision's perpetrator +0x50 = the player, the normal toward the hand, +0x20..+0x33 left empty as the projectile leaves them");}
     // the same pane again at once: the rate gate; a slow touch: never
     {const auto r0=G.refused[P::GlassRateLimited].load()+G.refused[P::GlassNotEdge].load();setup();handVel={3.f,0,0};publish({.5f,1.2f,.3f},P::ClassBody);
      simNs+=28*ms;++simIndex;P::tick(MT::Frame{simNs,simIndex});for(unsigned h=0;h<2;++h)D::preUpdate(&P::objects[h].beh,1.f/72.f);D::preApplyGuard(it);
      for(unsigned h=0;h<2;++h)D::update(&P::objects[h].beh,1.f/72.f);frame({.5f,1.2f,.3f});handVel={0,0,0};
      CHECK(stub::glassCalls==c0+1&&G.refused[P::GlassRateLimited].load()+G.refused[P::GlassNotEdge].load()>r0,"glass: the same pane again at once: refused (edge / rate)");}
     for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
     {const auto s0=G.refused[P::GlassSlow].load();setup();handVel={.5f,0,0};publish({.5f,1.2f,.3f},P::ClassBody);
      simNs+=28*ms;++simIndex;P::tick(MT::Frame{simNs,simIndex});for(unsigned h=0;h<2;++h)D::preUpdate(&P::objects[h].beh,1.f/72.f);D::preApplyGuard(it);
      for(unsigned h=0;h<2;++h)D::update(&P::objects[h].beh,1.f/72.f);frame({.5f,1.2f,.3f});handVel={0,0,0};
      CHECK(stub::glassCalls==c0+1&&G.refused[P::GlassSlow].load()==s0+1,"glass: a 0.5 m/s touch: never a break call");}
     // a group without the glass bit, a vtable whose +0x218 is another function: refused, no call
     for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
     {const auto g0=G.refused[P::GlassNotGlass].load();setup();stub::put<std::uint8_t>(group,0x53,0);handVel={3.f,0,0};publish({.5f,1.2f,.3f},P::ClassBody);
      simNs+=28*ms;++simIndex;P::tick(MT::Frame{simNs,simIndex});for(unsigned h=0;h<2;++h)D::preUpdate(&P::objects[h].beh,1.f/72.f);D::preApplyGuard(it);
      for(unsigned h=0;h<2;++h)D::update(&P::objects[h].beh,1.f/72.f);frame({.5f,1.2f,.3f});handVel={0,0,0};
      CHECK(stub::glassCalls==c0+1&&G.refused[P::GlassNotGlass].load()==g0+1,"glass: a group without the glass bit: no call");}
     for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
     {const auto v0=G.refused[P::GlassVt].load();setup();paneVt[0x218/8]=reinterpret_cast<void*>(stub::base+0x1234);handVel={3.f,0,0};publish({.5f,1.2f,.3f},P::ClassBody);
      simNs+=28*ms;++simIndex;P::tick(MT::Frame{simNs,simIndex});for(unsigned h=0;h<2;++h)D::preUpdate(&P::objects[h].beh,1.f/72.f);D::preApplyGuard(it);
      for(unsigned h=0;h<2;++h)D::update(&P::objects[h].beh,1.f/72.f);frame({.5f,1.2f,.3f});handVel={0,0,0};
      CHECK(stub::glassCalls==c0+1&&G.refused[P::GlassVt].load()==v0+1,"glass: the pane's vtable +0x218 is not sagFragInst::CollisionDamage: no call");
      paneVt[0x218/8]=reinterpret_cast<void*>(stub::base+P::off::fnCollisionDamage);}
     const auto hit=[&]{handVel={3.f,0,0};publish({.5f,1.2f,.3f},P::ClassBody);
        simNs+=28*ms;++simIndex;P::tick(MT::Frame{simNs,simIndex});for(unsigned h=0;h<2;++h)D::preUpdate(&P::objects[h].beh,1.f/72.f);D::preApplyGuard(it);
        for(unsigned h=0;h<2;++h)D::update(&P::objects[h].beh,1.f/72.f);frame({.5f,1.2f,.3f});handVel={0,0,0};};
     // damage the engine defers (IsFragDamageDelayed: the +0x80 override bits 21 / 20): refused, no call
     for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
     {const auto d0=G.refused[P::GlassDelayed].load();const int k0=stub::glassCalls;setup();
      stub::put<std::uint32_t>(pane.inst,0x80,stub::at<std::uint32_t>(pane.inst,0x80)|(1u<<21)|(1u<<20));hit();
      CHECK(stub::glassCalls==k0&&G.refused[P::GlassDelayed].load()==d0+1,"glass: delayed frag damage: no call (the projectile defers it)");}
     // no verified player (the perpetrator would be the last shooter): refused, no call
     for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
     {const auto n0=G.refused[P::GlassNoPlayer].load();const int k0=stub::glassCalls;setup();actorAddr=0x4242;hit();actorAddr=stub::addr(stub::playerActor);
      CHECK(stub::glassCalls==k0&&G.refused[P::GlassNoPlayer].load()==n0+1,"glass: no verified player guid: no call");}
     // a window whose frame (component 0, not glass) is met deeper in the same substep as its pane (component 2): the pane still breaks
     for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
     {alignas(16) static unsigned char mf2[0x600]{},child0[0x20]{},group0[0x60]{};
      const int k0=stub::glassCalls;const auto ng0=G.refused[P::GlassNotGlass].load();setup();
      stub::put<std::uintptr_t>(childList,0,stub::addr(child0));stub::put<std::uint8_t>(child0,0x10,0);stub::put<std::uintptr_t>(groupList,0,stub::addr(group0));
      std::memset(group0,0,sizeof group0);std::memcpy(mf2,mf,sizeof mf2);stub::put<std::uint8_t>(mf2,0x5a9,0);stub::put<std::uintptr_t>(mf2+0x10,0xc0,stub::addr(mf2));
      stub::put<float>(mf2+0x10,0xc,.03f);stub::put<std::uint8_t>(mf,0x5ab,2);stub::put<std::uintptr_t>(mf,0x110+8,stub::addr(mf2));
      hit();
      CHECK(stub::glassCalls==k0+1&&stub::at<std::int32_t>(stub::lastCollision,0x38)==2&&G.refused[P::GlassNotGlass].load()>=ng0,
            "glass: the frame met deeper than the pane: the pane's own hit still gets the call (component 2)");
      stub::put<std::uint8_t>(mf,0x5ab,0);}
     // no pane counted (sm_CreateCount zeroed in between) but the group's health fell: done, not noShatter
     for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
     {alignas(16) static unsigned char cache[0x200]{};alignas(16) static float health[4]{5.f,5.f,5.f,5.f};health[1]=5.f;
      const auto d0=G.done.load(),n0=G.noShatter.load();setup();stub::put<std::uintptr_t>(pane.inst,0x88,stub::addr(cache));stub::put<std::uintptr_t>(cache,0x138,stub::addr(health));
      stub::glassRefuse=true;stub::glassHealthDrop=&health[1];hit();stub::glassRefuse=false;stub::glassHealthDrop=nullptr;
      CHECK(G.done.load()==d0+1&&G.noShatter.load()==n0&&near(health[1],4.f),"glass: no pane counted but the group's health fell: done");
      stub::put<std::uintptr_t>(pane.inst,0x88,0);}
     CHECK(P::glassPaneBroke(true,false,-1.f,1.f)&&P::glassPaneBroke(false,true,-199.f,1.f)&&P::glassPaneBroke(false,true,-1.f,1.f)&&
           !P::glassPaneBroke(false,true,4.f,1.f)&&!P::glassPaneBroke(false,true,0.f,1.f)&&!P::glassPaneBroke(false,false,-199.f,1.f)&&!P::glassPaneBroke(false,true,NAN,1.f)&&
           !P::glassPaneBroke(false,true,-5.f,NAN),"glass: broken = a pane made or the group at its floor (1 -> -199); a health that only fell is a crack");
     // GLASS BLOOD: the punch blood drop on the bare hand that broke the pane (off until the engine words are bound: no call above)
     CHECK(stub::fxCalls==0&&G.bloodCalls.load()==0&&G.bloodRefused[P::BloodOff].load()>0,"blood: unbound (no TriggerPtFxMelee): refused off, no call");
     {static_assert(P::joaat("blood_push")==0xd3cd9f0eu&&P::joaat("BLOOD_PUSH")==0xd3cd9f0eu,"blood_push hash");
      P::engine.meleeFx=&stub::meleeFxFn;P::engine.ruleByHash=&stub::ruleFn;stub::put<std::uint8_t>(stub::bloodMgr,0x210,1);
      stub::put<std::uintptr_t>(stub::playerActor,0xb0,stub::addr(stub::creature));const P::V3 pa=solvedRight().solved;
      stub::camPos[0]=pa[0];stub::camPos[1]=pa[1]+.5f;stub::camPos[2]=pa[2]+1.f;stub::fovScaleSqr=1.f;stub::bloodRule=true;D::state().bloodGate={};
      const auto lv=D::state().lev;
      // the contact frame (lever bloodplace 0)
      P::leverPatchForTests.store([](P::Levers& l){l.bloodPlace=false;});{auto l=D::state().lev;l.bloodPlace=false;D::state().lev=l;}
      for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
      const auto d0=G.done.load(),b0=G.bloodCalls.load();const int f0=stub::fxCalls;setup();hit();
      CHECK(G.done.load()==d0+1&&G.bloodCalls.load()==b0+1&&stub::fxCalls==f0+1,"blood: a 3 m/s bare-hand break plays one drop (calls == done)");
      CHECK(stub::lastFx.fx==0xd3cd9f0eu&&stub::lastFx.fx2==0&&stub::lastFx.actor==stub::playerActor&&
            stub::addr(stub::lastFx.self)==stub::base+P::off::ptFxMeleeVar&&stub::addr(stub::lastFx.mtx)==stub::addr(P::objects[1].inst)+0x20,
            "blood: TriggerPtFxMelee(g_ptFxMelee, player, blood_push, 0, the hand body's matrix, ...)");
      {float m[16];std::memcpy(m,P::objects[1].inst+0x20,64);const P::V3 a{m[0],m[1],m[2]},b{m[4],m[5],m[6]},c{m[8],m[9],m[10]};
       const auto& L=stub::lastFx;const P::V3 nw=V::add(V::add(V::mul(a,L.n[0]),V::mul(b,L.n[1])),V::mul(c,L.n[2]));
       const P::V3 pw=V::add(V::add(V::mul(a,L.pos[0]),V::mul(b,L.pos[1])),V::mul(c,L.pos[2]));
       CHECK(nearV(nw,{1,0,0},1e-3f)&&near(pw[0],.05f,.02f)&&near(pw[1],0.f,.01f)&&near(pw[2],0.f,.01f),
             "blood: the local normal = out of the hand toward the pane (+x), the local point = the contact from the hand at the hit (5 cm along +x)");
       CHECK(near(L.dir[0],3.f,.05f)&&near(L.dir[1],0.f,.05f)&&near(L.dir[2],0.f,.05f)&&L.pos[3]==0.f&&L.dir[3]==0.f,"blood: dir = the hand's direction x 3 (16 bytes, w 0)");}
      {P::State::BloodSample x{};for(const auto& e:D::state().bloodSamples)if(e.set)x=e;
       CHECK(x.set&&x.hand==1&&x.rule&&near(x.camM,std::sqrt(1.25f),.1f),"blood: the sample ring (hand, rule, camera distance)");}
      // the same hand again within the cooldown: the pane breaks, no drop
      {auto l=D::state().lev;l.bloodCdMs=5000.f;D::state().lev=l;}
      P::leverPatchForTests.store([](P::Levers& l){l.bloodCdMs=5000.f;l.bloodPlace=false;});
      for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
      {const auto dd=G.done.load(),cd=G.bloodRefused[P::BloodCooldown].load();const int f1=stub::fxCalls;setup();hit();
       CHECK(G.done.load()==dd+1&&stub::fxCalls==f1&&G.bloodRefused[P::BloodCooldown].load()==cd+1,"blood: a second break within the cooldown: the glass breaks, no drop");}
      P::leverPatchForTests.store(nullptr);
      // direct gate checks (glassBlood on a recorded hit)
      P::GlassHit hh{};hh.pos=V::add(pa,{.05f,0,0});hh.hand=pa;hh.n={-1,0,0};hh.vel={3,0,0};
      auto& st=D::state();const auto tNow=simNs+1000*ms;
      st.bloodGate={};st.lev.bloodCdMs=600.f;
      CHECK(D::glassBlood(st,1,hh,tNow)==P::BloodOk&&D::glassBlood(st,1,hh,tNow+100*ms)==P::BloodCooldown&&D::glassBlood(st,0,hh,tNow+100*ms)==P::BloodOk,
            "blood: per-hand cooldown, the other hand free");
      CHECK(D::glassBlood(st,1,hh,tNow+700*ms)==P::BloodOk&&D::glassBlood(st,0,hh,tNow+800*ms)==P::BloodBudget,"blood: 3 drops per 2 s over both hands");
      st.bloodGate={};
      st.grab[1].active=true;CHECK(D::glassBlood(st,1,hh,tNow)==P::BloodHolding,"blood: a hand holding a prop: none");st.grab[1].active=false;
      st.seat[1].on=true;CHECK(D::glassBlood(st,1,hh,tNow)==P::BloodHolding,"blood: a hand pulling a light prop into the palm: none");st.seat[1]=P::State::SeatPull{};
      {const auto r0=G.bloodRefused[P::BloodNoPane].load();const int f5=stub::fxCalls;
       CHECK(D::glassBlood(st,1,hh,tNow,false)==P::BloodNoPane&&G.bloodRefused[P::BloodNoPane].load()==r0+1&&stub::fxCalls==f5,"blood: a crack (no pane, the group above its floor): none");}
      stub::put<std::uint8_t>(stub::bloodMgr,0x210,0);CHECK(D::glassBlood(st,1,hh,tNow)==P::BloodDisabled,"blood: SET_BLOOD_EFFECTS_ENABLE off: none");
      stub::put<std::uint8_t>(stub::bloodMgr,0x210,1);
      stub::bloodRule=false;CHECK(D::glassBlood(st,1,hh,tNow)==P::BloodNoRule,"blood: no effect rule loaded: none");stub::bloodRule=true;
      stub::camPos[2]=pa[2]+20.f;CHECK(D::glassBlood(st,1,hh,tNow)==P::BloodFar,"blood: the camera 20 m away (the engine's cull): none");stub::camPos[2]=pa[2]+1.f;
      stub::put<std::uintptr_t>(stub::playerActor,0xb0,0);CHECK(D::glassBlood(st,1,hh,tNow)==P::BloodNoCreature,"blood: no creature component: none");
      stub::put<std::uintptr_t>(stub::playerActor,0xb0,stub::addr(stub::creature));
      {P::GlassHit bad=hh;bad.n={NAN,0,0};CHECK(D::glassBlood(st,1,bad,tNow)==P::BloodNoPose,"blood: a NaN normal: none");}
      st.lev.blood=false;CHECK(D::glassBlood(st,1,hh,tNow)==P::BloodOff,"blood: lever blood 0: none");st.lev.blood=true;
      P::groupOk[P::GroupBlood]=false;CHECK(D::glassBlood(st,1,hh,tNow)==P::BloodOff,"blood: BLOOD prints refused: none");P::groupOk[P::GroupBlood]=true;
      st.lev.bloodPlace=false;
      {P::GlassHit deep=hh;deep.pos=V::add(pa,{.30f,0,0});const int f2=stub::fxCalls;st.bloodGate={};D::glassBlood(st,1,deep,tNow+5000*ms);
       float m[16];std::memcpy(m,P::objects[1].inst+0x20,64);const P::V3 a{m[0],m[1],m[2]},b{m[4],m[5],m[6]},c{m[8],m[9],m[10]};const auto& L=stub::lastFx;
       const P::V3 pw=V::add(V::add(V::mul(a,L.pos[0]),V::mul(b,L.pos[1])),V::mul(c,L.pos[2]));
       CHECK(stub::fxCalls==f2+1&&near(V::length(pw),.10f,1e-3f),"blood: a contact 30 cm from the hand: the local point clamped to 10 cm");}
      {P::GlassHit slow=hh;slow.vel={0,.1f,0};st.bloodGate={};D::glassBlood(st,1,slow,tNow+9000*ms);const auto& L=stub::lastFx;
       CHECK(near(L.dir[0],3.f,.05f)&&near(L.dir[1],0.f,.05f),"blood: a hand under 0.3 m/s: dir along the outward normal x 3");}
      st.lev.bloodPlace=true;
      // BLOOD PLACE: the drop just outside the hand's outline from the VR eye, sprayed out of it at bloodmps; the same call and matrix
      {st.bloodGate={};st.lev.bloodMps=.6f;const auto e0=G.bloodEyeFallback.load();const int f7=stub::fxCalls;
       const auto tB=tNow+20'000*ms;CHECK(D::glassBlood(st,1,hh,tB)==P::BloodOk&&stub::fxCalls==f7+1,"blood place: one call");
       float m[16];std::memcpy(m,P::objects[1].inst+0x20,64);const P::V3 a{m[0],m[1],m[2]},b{m[4],m[5],m[6]},c{m[8],m[9],m[10]},B{m[12],m[13],m[14]};
       const auto& L=stub::lastFx;const P::V3 pw=V::add(B,V::add(V::add(V::mul(a,L.pos[0]),V::mul(b,L.pos[1])),V::mul(c,L.pos[2])));
       const P::V3 nw=V::add(V::add(V::mul(a,L.n[0]),V::mul(b,L.n[1])),V::mul(c,L.n[2]));const P::V3 dw{L.dir[0],L.dir[1],L.dir[2]};
       const int th1=P::handThumb(1,st.hand[1].fist);const P::BloodPlace bp=P::bloodEyePlace(a,b,c,B,st.tg.eye,st.tg.up,st.hand[1].fist,.6f,st.upm,th1);
       CHECK(bp.ok&&near(L.pos[0],bp.f.pos[0],1e-5f)&&near(L.pos[1],bp.f.pos[1],1e-5f)&&near(L.pos[2],bp.f.pos[2],1e-5f)&&nearV(dw,bp.f.dir,1e-5f)&&
             stub::addr(stub::lastFx.mtx)==stub::addr(P::objects[1].inst)+0x20&&L.pos[3]==0.f&&L.dir[3]==0.f,
             "blood place: TriggerPtFxMelee gets bloodEyePlace's local point / normal and world velocity, still on the hand body's matrix");
       CHECK(!P::insideOutline(a,b,c,B,st.tg.eye,pw,st.hand[1].fist,st.upm,th1)&&V::length(V::sub(pw,B))<=.12f+1e-4f&&near(V::length(dw),.6f,1e-3f)&&
             V::dot(dw,st.tg.up)>0.f&&V::dot(nw,V::sub(st.tg.eye,pw))>0.f,
             "blood place: the point outside the hand's outline from the eye, within 12 cm, 0.6 m/s upward, the normal toward the eye");
       P::State::BloodSample x{};x=st.bloodSamples[(st.bloodSampleNext+st.bloodSamples.size()-1u)%st.bloodSamples.size()];
       CHECK(x.place==1&&x.occlNew==0&&x.occlOld==1&&near(x.eyeM,V::length(V::sub(st.tg.eye,pw)),1e-3f)&&G.bloodEyeFallback.load()==e0,
             "blood place: the sample (place 1, the new point clear of the outline, the contact point inside it, the eye distance)");
       // no usable eye: the contact frame, counted
       st.bloodGate={};st.tgUsable=false;const P::BloodFrame f0=P::bloodFrame(a,b,c,hh.pos,hh.hand,hh.n,hh.vel,st.upm);
       CHECK(D::glassBlood(st,1,hh,tB+5000*ms)==P::BloodOk&&near(stub::lastFx.pos[0],f0.pos[0],1e-5f)&&near(stub::lastFx.dir[0],f0.dir[0],1e-5f)&&
             G.bloodEyeFallback.load()==e0+1&&st.bloodSamples[(st.bloodSampleNext+st.bloodSamples.size()-1u)%st.bloodSamples.size()].place==2,
             "blood place: no usable eye: the contact frame (place 2), eyeFallback counted");
       st.tgUsable=true;st.bloodGate={};}
      // the open hand's thumb: the eye straight over the palm (N toward it), F forward, S across: the drop never goes to the thumb's side of
      // S (it would sit behind the rendered thumb); a fist has no thumb reach
      {const P::V3 S{1,0,0},F{0,0,-1},N{0,1,0},B{0,1,0},eye{0,1.5f,0},up{0,0,-1};
       CHECK(P::handThumb(1,false)==1&&P::handThumb(0,false)==-1&&P::handThumb(1,true)==0&&P::handThumb(0,true)==0,"blood place thumb: the right hand's on +S, the left's on -S, none for a fist");
       const float r0=P::handReach(S,F,N,S,false,1.f),rT=P::handReach(S,F,N,S,false,1.f,1),rO=P::handReach(S,F,N,V::mul(S,-1.f),false,1.f,1);
       CHECK(near(r0,.0425f,1e-5f)&&near(rT,.0775f,1e-5f)&&near(rO,.0425f,1e-5f),"blood place thumb: the reach on the thumb's side 3.5 cm more, the other side unchanged");
       const P::V3 q{.06f,1,0};CHECK(!P::insideOutline(S,F,N,B,eye,q,false,1.f,0)&&P::insideOutline(S,F,N,B,eye,q,false,1.f,1)&&!P::insideOutline(S,F,N,B,eye,q,false,1.f,-1),
             "blood place thumb: a point 6 cm out on +S is behind the right hand's thumb, clear of the left hand's side");
       const P::BloodPlace pr=P::bloodEyePlace(S,F,N,B,eye,up,false,.6f,1.f,1),pl=P::bloodEyePlace(S,F,N,B,eye,up,false,.6f,1.f,-1);
       CHECK(pr.ok&&pl.ok&&V::dot(pr.w,S)<.5f&&V::dot(pl.w,S)>-.5f&&!P::insideOutline(S,F,N,B,eye,pr.world,false,1.f,1)&&!P::insideOutline(S,F,N,B,eye,pl.world,false,1.f,-1),
             "blood place thumb: the right hand's drop never on +S, the left's never on -S, both clear of the outline with the thumb");}
      // a break that made no pane (noShatter) never plays a drop
      for(int i=0;i<40;++i)frame({.5f,1.2f,.3f});st.bloodGate={};
      {stub::glassCreateCount=0;const auto ns0=G.noShatter.load();const int f3=stub::fxCalls;setup();stub::glassRefuse=true;hit();stub::glassRefuse=false;
       CHECK(G.noShatter.load()==ns0+1&&stub::fxCalls==f3,"blood: no shatter: no drop");}
      // no pane counted: a crack (the group's health 5 -> 4, floor 1) is done without a drop; the group broken (1 -> -199) gets it
      for(int i=0;i<40;++i)frame({.5f,1.2f,.3f});st.bloodGate={};
      {alignas(16) static unsigned char cache2[0x200]{};alignas(16) static float health2[4]{};health2[1]=5.f;
       const auto d0=G.done.load(),np=G.bloodRefused[P::BloodNoPane].load();const int f6=stub::fxCalls;setup();
       stub::put<std::uintptr_t>(pane.inst,0x88,stub::addr(cache2));stub::put<std::uintptr_t>(cache2,0x138,stub::addr(health2));
       stub::glassRefuse=true;stub::glassHealthDrop=&health2[1];stub::glassHealthDropBy=1.f;hit();
       CHECK(G.done.load()==d0+1&&stub::fxCalls==f6&&G.bloodRefused[P::BloodNoPane].load()==np+1&&near(health2[1],4.f),
             "blood: a crack (no pane, the health above the floor): done, no drop (noPane)");
       for(int i=0;i<40;++i)frame({.5f,1.2f,.3f});st.bloodGate={};health2[1]=1.f;stub::glassHealthDropBy=200.f;setup();
       stub::put<std::uintptr_t>(pane.inst,0x88,stub::addr(cache2));hit();
       CHECK(G.done.load()==d0+2&&stub::fxCalls==f6+1&&near(health2[1],-199.f),"blood: no pane counted but the group broke (1 -> -199): the drop");
       stub::glassRefuse=false;stub::glassHealthDrop=nullptr;stub::glassHealthDropBy=1.f;stub::put<std::uintptr_t>(pane.inst,0x88,0);}
      // a firearm hand (ClassHeld) has no body: no break call, no drop
      for(int i=0;i<40;++i)frame({.5f,1.2f,.3f});
      {const int k0=stub::glassCalls,f4=stub::fxCalls;setup();for(int i=0;i<6;++i)frame({.5f,1.2f,.3f},P::ClassHeld);
       handVel={3.f,0,0};publish({.5f,1.2f,.3f},P::ClassHeld);simNs+=28*ms;++simIndex;P::tick(MT::Frame{simNs,simIndex});
       for(unsigned h=0;h<2;++h)D::preUpdate(&P::objects[h].beh,1.f/72.f);D::preApplyGuard(it);for(unsigned h=0;h<2;++h)D::update(&P::objects[h].beh,1.f/72.f);
       frame({.5f,1.2f,.3f},P::ClassHeld);handVel={0,0,0};
       CHECK(stub::glassCalls==k0&&stub::fxCalls==f4,"blood: a firearm hand (no body): no break call, no drop");
       for(int i=0;i<10;++i)frame({.5f,1.2f,.3f});}
      // BLOOD ROUTE: a rule in the low-res list with the low-res pass off is created in list 0 (the byte 0 for the call, put back after it),
      // the drop found at list 0's head and watched; a rule already in list 0, or list 1 with the low-res pass on: unrouted
      {auto& st2=D::state();const auto t2=simNs+60'000*ms;st2.lev.bloodHires=true;
       const auto drop=[&](std::uint64_t t){st2.bloodGate={};return D::glassBlood(st2,1,hh,t);};
       const auto last=[&]{P::State::BloodSample x{};const auto& b=st2.bloodSamples[(st2.bloodSampleNext+st2.bloodSamples.size()-1u)%st2.bloodSamples.size()];x=b;return x;};
       stub::bloodRouteSetup(1,0);const auto ro0=G.bloodRouted.load(),m0=G.bloodMade.load(),w0=P::ptfxWatch.arms.load();
       CHECK(drop(t2)==P::BloodOk&&stub::seenListByte==0&&stub::ruleBuf[0x37d]==1&&G.bloodRouted.load()==ro0+1&&G.bloodMade.load()==m0+1,
             "blood route: list 1, low-res off: the call sees list 0, the rule's byte back to 1 after it, routed, made");
       {const auto head=stub::at<std::uintptr_t>(stub::listObjs[0],0);const auto x=last();
        CHECK(head&&stub::at<std::uintptr_t>(reinterpret_cast<void*>(head),0x170)==stub::addr(stub::ruleBuf)&&!stub::at<std::uintptr_t>(stub::listObjs[1],0)&&
              P::ptfxWatch.inst.load()==head+0x20&&P::ptfxWatch.arms.load()==w0+1,"blood route: the slot at list 0's head holds the rule; the watch holds its instance (slot + 0x20)");
        CHECK(x.set&&x.list==1&&x.vtOk&&x.lowRes==0&&x.routed&&x.made==1&&near(x.cullR,1.f)&&near(x.cullD,30.f),"blood route: the sample (list 1, vt, low-res 0, routed, made, cull words)");
        // the watch (TraceEntry's CullEffect hook): only the watched instance before its deadline, eye scope and other apart
        const auto e0=P::ptfxWatch.seenEye.load(),o0=P::ptfxWatch.seenOther.load(),de0=P::ptfxWatch.droppedEye.load();
        const auto until=P::ptfxWatch.until.load();
        CHECK(P::ptfxWatchHitAt(P::ptfxWatch,head+0x20,until-1)&&!P::ptfxWatchHitAt(P::ptfxWatch,head+0x20,until)&&!P::ptfxWatchHitAt(P::ptfxWatch,head+0x28,until-1)&&
              !P::ptfxWatchHitAt(P::ptfxWatch,0,until-1),"blood watch: the watched instance only, before its deadline");
        P::ptfxWatchNote(P::ptfxWatch,true,false);P::ptfxWatchNote(P::ptfxWatch,true,true);P::ptfxWatchNote(P::ptfxWatch,false,false);
        CHECK(P::ptfxWatch.seenEye.load()==e0+2&&P::ptfxWatch.droppedEye.load()==de0+1&&P::ptfxWatch.seenOther.load()==o0+1,"blood watch: eye-scope calls, the culled ones and the others counted apart");}
       stub::bloodRouteSetup(2,0);CHECK(drop(t2+3000*ms)==P::BloodOk&&stub::seenListByte==0&&stub::ruleBuf[0x37d]==2&&last().routed,"blood route: list 2 (never drawn): routed");
       {stub::bloodRouteSetup(0,0);const auto d0=G.bloodRouteRefused[P::RouteDrawn].load();
        CHECK(drop(t2+6000*ms)==P::BloodOk&&stub::seenListByte==0&&!last().routed&&last().made==1&&G.bloodRouteRefused[P::RouteDrawn].load()==d0+1,"blood route: list 0: unrouted (drawn), made");}
       {stub::bloodRouteSetup(1,1);const auto d0=G.bloodRouteRefused[P::RouteDrawn].load();
        CHECK(drop(t2+9000*ms)==P::BloodOk&&stub::seenListByte==1&&!last().routed&&G.bloodRouteRefused[P::RouteDrawn].load()==d0+1,"blood route: list 1 with the low-res pass on: unrouted");}
       {stub::bloodRouteSetup(7,0);CHECK(drop(t2+12000*ms)==P::BloodOk&&stub::seenListByte==7&&!last().routed,"blood route: a list byte over the count (lands in list 0): unrouted");}
       {stub::bloodRouteSetup(1,0);stub::put<std::uintptr_t>(stub::ruleBuf,0,stub::base+0x1234);const auto v0=G.bloodRouteRefused[P::RouteVt].load();const int f9=stub::fxCalls;
        CHECK(drop(t2+15000*ms)==P::BloodOk&&stub::fxCalls==f9+1&&stub::seenListByte==1&&stub::ruleBuf[0x37d]==1&&G.bloodRouteRefused[P::RouteVt].load()==v0+1,
              "blood route: another rule vtable: unrouted (the drop still played), the byte untouched");}
       {stub::bloodRouteSetup(1,0);st2.lev.bloodHires=false;const auto l0=G.bloodRouteRefused[P::RouteLever].load();
        CHECK(drop(t2+18000*ms)==P::BloodOk&&stub::seenListByte==1&&G.bloodRouteRefused[P::RouteLever].load()==l0+1,"blood route: lever bloodhires 0: unrouted");st2.lev.bloodHires=true;}
       {stub::bloodRouteSetup(1,0);P::groupOk[P::GroupBloodRoute]=false;const auto g0=G.bloodRouteRefused[P::RouteGroup].load();
        CHECK(drop(t2+21000*ms)==P::BloodOk&&stub::seenListByte==1&&G.bloodRouteRefused[P::RouteGroup].load()==g0+1,"blood route: BLOODROUTE prints refused: unrouted, BLOOD still on");
        P::groupOk[P::GroupBloodRoute]=true;}
       {stub::bloodRouteSetup(1,0);stub::rmPtr=0;const auto r0=G.bloodRouteRefused[P::RouteRead].load();
        CHECK(drop(t2+24000*ms)==P::BloodOk&&stub::seenListByte==1&&G.bloodRouteRefused[P::RouteRead].load()==r0+1&&last().made==-1,"blood route: rmPtfxManager unread: unrouted, made unknown");}
       {stub::bloodRouteSetup(1,0);stub::skipCreate=true;const auto n0=G.bloodNotMade.load(),w1=P::ptfxWatch.arms.load();
        CHECK(drop(t2+27000*ms)==P::BloodOk&&G.bloodNotMade.load()==n0+1&&P::ptfxWatch.arms.load()==w1&&stub::ruleBuf[0x37d]==1&&last().made==0,
              "blood route: GetEffectInst gave none: notMade, no watch, the byte put back");}
       {stub::bloodRouteSetup(1,0);const int f8=stub::fxCalls;st2.bloodGate={};st2.bloodGate.note(1,t2+30000*ms);
        CHECK(D::glassBlood(st2,1,hh,t2+30000*ms+10*ms)==P::BloodCooldown&&stub::fxCalls==f8&&stub::seenListByte==-1&&stub::ruleBuf[0x37d]==1,
              "blood route: a cooldown refusal comes before any read or write");}
       CHECK(P::ptfxListDead(1,4,0)&&P::ptfxListDead(2,4,1)&&!P::ptfxListDead(1,4,1)&&!P::ptfxListDead(0,4,0)&&!P::ptfxListDead(3,4,0)&&!P::ptfxListDead(9,4,0)&&
             P::ptfxListOf(9,4)==0&&P::bloodRouteWhy(true,true,true,true,1,4,0)==P::RouteGo&&P::bloodRouteWhy(true,true,true,false,1,4,0)==P::RouteVt,
             "blood route: dead lists = 1 with the low-res pass off, 2; an index over the count is list 0");
       {const auto r=P::report();
        CHECK(jsonValid(r)&&r.find("\"route\":{\"lever\":1")!=std::string::npos&&r.find("\"watch\":{\"arms\":")!=std::string::npos&&r.find("\"list\":1,\"vt\":true")!=std::string::npos&&
              r.find("\"bloodHires\":1")!=std::string::npos,"blood route: the report (glass.blood.route, the sample fields, lever bloodHires) parses");}}
      // BLOOD LIFE: the drop's instance read on the tick while watched (a fake slot: its rule, the attach, two events (one emitter), the
      // element counts, the world position, the fade)
      {auto& st3=D::state();alignas(16) static unsigned char slot[0x400]{},ruleL[0x400]{},ev0[0x10]{},ev1[0x10]{},data[0x400]{},wobj[0x400]{};
       alignas(16) static void* evVt0[0x10]{};alignas(16) static void* evVt1[0x10]{};static std::uintptr_t evs[2]{};
       evVt0[0x60/8]=reinterpret_cast<void*>(stub::base+P::off::fnEmitterDraw);evVt1[0x60/8]=reinterpret_cast<void*>(stub::base+0x4321);
       stub::put<std::uintptr_t>(ev0,0,stub::addr(evVt0));stub::put<std::uintptr_t>(ev1,0,stub::addr(evVt1));evs[0]=stub::addr(ev0);evs[1]=stub::addr(ev1);
       stub::put<std::uint16_t>(ruleL,0x320,2);stub::put<std::uintptr_t>(ruleL,0x318,stub::addr(evs));
       const std::uintptr_t mtx=stub::addr(P::objects[1].inst)+0x20;
       stub::put<std::uintptr_t>(slot,0x170,stub::addr(ruleL));unsigned char* inst=slot+0x20;
       stub::put<std::int32_t>(inst,0x14c,2);stub::put<std::uintptr_t>(inst,0x170,mtx);stub::put<std::uintptr_t>(inst,0x158,stub::addr(data));
       stub::put<std::uint32_t>(data,0x28,0);stub::put<std::uint32_t>(data,0x2c,7);stub::put<std::uint32_t>(data,0x160+0x28,99); // the second event (not an emitter)
       stub::put<std::uintptr_t>(inst,0x330,stub::addr(wobj));const P::V3 at=V::add(st3.tg.eye,{0,0,.5f});const float wp[4]{at[0],at[1],at[2],1};std::memcpy(wobj+0x3b0,wp,16);
       stub::put<float>(inst,0x290,.8f);stub::put<float>(inst,0x250,1.f);
       const auto tL=simNs+200'000*ms;const auto last=[&]()->const P::State::BloodLife&{return st3.bloodLife[(st3.bloodLifeNext+3u)%4u];};
       D::bloodLifeArm(st3,1,stub::addr(slot),stub::addr(ruleL),mtx,tL);const auto& e=last();
       D::bloodLifeTick(st3,tL+10*ms);
       CHECK(e.set&&!e.stop&&e.reads==1&&e.mode==2&&e.attachOk&&e.events==2&&e.emitters==1&&e.maxCount==7&&e.liveReads==1&&near(e.eyeFirstM,.5f,1e-3f)&&near(e.fadeMin,.8f)&&
             near(e.alpha,1.f),"blood life: the slot's rule, the attach on the hand's matrix, 2 events (1 emitter, its count 7; the other's 99 not read), the eye distance, the fade");
       {const float wp2[4]{at[0],at[1]+.2f,at[2],1};std::memcpy(wobj+0x3b0,wp2,16);}
       stub::put<std::uint32_t>(data,0x2c,0);D::bloodLifeTick(st3,tL+20*ms);
       CHECK(e.reads==2&&near(e.travelM,.2f,1e-3f)&&e.maxCount==7&&e.liveReads==1,"blood life: the travel from the first read, the largest count kept");
       {const auto rc0=G.bloodLifeRecycled.load();stub::put<std::uintptr_t>(slot,0x170,stub::addr(stub::ruleBuf));D::bloodLifeTick(st3,tL+30*ms);
        CHECK(e.stop==2&&e.reads==2&&G.bloodLifeRecycled.load()==rc0+1,"blood life: the slot holds another rule: stopped (recycled)");}
       D::bloodLifeTick(st3,tL+40*ms);CHECK(e.reads==2,"blood life: stopped: no more reads");
       stub::put<std::uintptr_t>(slot,0x170,stub::addr(ruleL));stub::put<std::int32_t>(inst,0x14c,3);
       D::bloodLifeArm(st3,1,stub::addr(slot),stub::addr(ruleL),mtx,tL);D::bloodLifeTick(st3,tL+10*ms);
       CHECK(last().reads==1&&!last().attachOk&&last().mode==3,"blood life: another attach mode: read, attachOk false");stub::put<std::int32_t>(inst,0x14c,2);
       D::bloodLifeArm(st3,1,stub::addr(slot),stub::addr(ruleL),mtx,tL);D::bloodLifeTick(st3,tL+1600*ms);
       CHECK(last().stop==1&&last().reads==0,"blood life: past 1.5 s: expired, no read");
       {const auto fl0=G.bloodLifeFails.load();D::bloodLifeArm(st3,1,0x4242,stub::addr(ruleL),mtx,tL);D::bloodLifeTick(st3,tL+10*ms);
        CHECK(last().stop==3&&last().reads==0&&G.bloodLifeFails.load()==fl0+1,"blood life: an unreadable slot: stopped, counted");}
       P::groupOk[P::GroupBloodLife]=false;D::bloodLifeArm(st3,1,stub::addr(slot),stub::addr(ruleL),mtx,tL);D::bloodLifeTick(st3,tL+10*ms);
       CHECK(last().stop==4&&last().reads==0,"blood life: BLOODLIFE prints refused: no reads");
       {const int f0=stub::fxCalls;st3.bloodGate={};CHECK(D::glassBlood(st3,1,hh,tL+5000*ms)==P::BloodOk&&stub::fxCalls==f0+1,"blood life: BLOODLIFE refused: the drop still played");}
       P::groupOk[P::GroupBloodLife]=true;st3.bloodGate={};
       {const auto r=P::report();CHECK(jsonValid(r)&&r.find("\"life\":{\"group\":true")!=std::string::npos&&r.find("\"place\":{\"lever\":1")!=std::string::npos&&
             r.find("\"occlNew\":")!=std::string::npos&&r.find("\"bloodPlace\":1,\"bloodMps\":0.60")!=std::string::npos,"blood place / life: the report parses (place, life, sample fields, levers)");}}
      D::state().lev=lv;P::engine.meleeFx=nullptr;P::engine.ruleByHash=nullptr;}
     // ---------- GUN GLASS: a held pistol's muzzle sweep / barrel line, the free palm, judged by the hand path's glass checks ----------
     {auto& GG=P::stats.gun;auto& st=D::state();
      // pure: the gate order, the intersection layout, the approach sign, the segment distance
      {P::GunGateIn g{};g.lever=0;g.gun=true;g.held=true;g.onFoot=true;g.fresh=true;g.velOk=true;g.kind=1;g.placedNs=1;g.now=400*ms;g.sweepM=.05f;g.speedMps=3.f;g.mps=2.f;
       const auto w=[&](auto f){P::GunGateIn x=g;f(x);return P::gunGate(x);};
       CHECK(P::gunGate(g)==P::GunGo&&w([](P::GunGateIn& x){x.lever=-1;})==P::GunOff&&w([](P::GunGateIn& x){x.gun=false;})==P::GunNotGun&&
             w([](P::GunGateIn& x){x.kind=3;})==P::GunKind&&w([](P::GunGateIn& x){x.kind=3;x.longOk=true;})==P::GunGo&&w([](P::GunGateIn& x){x.held=false;})==P::GunClass&&
             w([](P::GunGateIn& x){x.onFoot=false;})==P::GunFoot&&w([](P::GunGateIn& x){x.bridged=true;})==P::GunBridged&&w([](P::GunGateIn& x){x.velOk=false;})==P::GunStale&&
             w([](P::GunGateIn& x){x.placedNs=x.now-100*ms;})==P::GunSettling&&w([](P::GunGateIn& x){x.busy=P::gunBusyShot;})==P::GunShot&&
             w([](P::GunGateIn& x){x.busyRecent=P::gunBusySpin;})==P::GunSpin&&w([](P::GunGateIn& x){x.busy=P::gunBusyReload;})==P::GunReload&&
             w([](P::GunGateIn& x){x.sweepM=.4f;})==P::GunLong&&w([](P::GunGateIn& x){x.speedMps=1.5f;})==P::GunSlow&&w([](P::GunGateIn& x){x.speedMps=1.7f;})==P::GunGo,
             "gun glass gate: off, notGun, kind (long guns with gunglasslong), class, foot, bridged, stale, settling, shot / spin / reload, long sweep, slow (0.8 x mps)");
       stub::probeHit(0,0x123450,{1,2,3},{0,0,1},0xffff,7);const P::ProbeHit ph=P::parseIsect(stub::probeHits);
       CHECK(ph.inst==0x123450&&nearV(ph.pos,{1,2,3})&&nearV(ph.n,{0,0,1})&&ph.part==-1&&ph.comp==7,"gun glass: an intersection: inst +0x0, pos +0x10, normal +0x20, part u16 +0x64 (0xffff none), component u16 +0x6a");
       P::V3 nOut{};const float a=P::gunApproach({3,0,0},{1,0,0},{1,0,0},{0,0,0},1.f,nOut);
       CHECK(near(a,3.f)&&nearV(nOut,{-1,0,0})&&near(P::segDistance({0,1,0},{-1,0,0},{1,0,0}),1.f)&&near(P::segDistance({3,0,0},{-1,0,0},{1,0,0}),2.f),
             "gun glass: the normal turned toward the grip, the approach along it; the segment distance");}
      // the world: the gun hand's muzzle 5 cm short of the pane, its bore 15 cm behind; the pane (component 2, part 5) in the level
      setup();stub::glassCreateCount=0;simNs+=3000*ms;P::engine.probeLevel=&stub::probeLevelFn;
      const P::V3 tip0=V::sub(pt,{.10f,0,0}),bore0=V::sub(tip0,{.15f,0,0});
      gunNow=1;gunKind=1;gunSource=2;gunBusy=0;tipOkNow=true;tipRelNow=tip0;boreRelNow=bore0;tipVelNow={0,0,0};
      const auto gunFrame=[&](P::V3 tip,P::V3 vel){tipRelNow=tip;boreRelNow=V::sub(tip,{.15f,0,0});tipVelNow=vel;frame({.5f,1.2f,.3f},P::ClassHeld);};
      const auto paneHit=[&]{stub::probeHit(0,stub::addr(pane.inst),pt,{1,0,0},5,2);stub::probeCount=1;};
      paneHit();
      {const int pc0=stub::probeCalls;const auto s0=GG.gated[P::GunSlow].load()+GG.gated[P::GunSettling].load();
       for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});
       CHECK(stub::probeCalls==pc0&&GG.gated[P::GunSlow].load()+GG.gated[P::GunSettling].load()>=s0+12,"gun glass: a still gun: no probe (settling, then slow)");}
      // lever 0 (the default): a 3 m/s whip into the pane is judged and counted, never broken, no blood
      {const int pc0=stub::probeCalls,k0=stub::glassCalls,f0=stub::fxCalls;const auto w0=GG.wouldBreak.load(),t0=GG.probes[0].load(),b0=GG.breaks.load();
       gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});
       CHECK(stub::probeCalls==pc0+1&&GG.probes[0].load()==t0+1&&GG.wouldBreak.load()==w0+1&&GG.breaks.load()==b0&&stub::glassCalls==k0&&stub::fxCalls==f0,
             "gun glass lever 0: the muzzle sweep meets the pane: would break (counted), no break call, no blood");
       const auto& L=stub::lastProbe;bool mover=false;for(const auto* e:L.excl)mover=mover||stub::addr(e)==st.moverInstAddr;
       CHECK(near(L.r,.025f)&&L.maxHits==4&&L.include==1&&L.typeExclude==0&&L.state==7&&mover&&near(L.seg[0],tip0[0],1e-4f)&&near(L.seg[4],tip0[0]+.084f,1e-4f)&&L.seg[3]==0.f,
             "gun glass: the probe: the sweep from the last muzzle to now, r 2.5 cm, 4 hits, include 1, state 7, the mover excluded");
       const auto& x=st.gunSamples[(st.gunSampleNext+st.gunSamples.size()-1u)%st.gunSamples.size()];
       CHECK(x.set&&x.src==0&&x.comp==2&&x.part==5&&near(x.approach,3.f,1e-3f)&&x.result==P::GlassOk&&x.lever==1,"gun glass: the sample (muzzle, component 2, part 5, 3 m/s, ok, lever 0)");}
      // lever 1: the break call with the projectile's Collision (the element from the part), the normal toward the grip; no blood
      {static const auto brk=+[](P::Levers& l){l.gunGlass=1;};P::leverPatchForTests.store(brk);st.leversRead=false;
       for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});
       const int k0=stub::glassCalls,f0=stub::fxCalls;const auto b0=GG.breaks.load(),d0=GG.done.load(),hd0=G.done.load();
       gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});
       CHECK(stub::glassCalls==k0+1&&GG.breaks.load()==b0+1&&GG.done.load()==d0+1&&G.done.load()==hd0&&stub::fxCalls==f0&&stub::lastGlassIt==nullptr,
             "gun glass lever 1: one break call (counted under gun, not the hand's), no blood, a null iterator");
       CHECK(stub::at<std::int32_t>(stub::lastCollision,0x38)==2&&stub::at<std::int32_t>(stub::lastCollision,0x3c)==5&&near(stub::at<float>(stub::lastCollision,0x34),100.f)&&
             stub::at<std::uint32_t>(stub::lastCollision,0x50)==stub::playerGuid&&stub::at<std::uint8_t>(stub::lastCollision,0x54)==0xff&&near(stub::at<float>(stub::lastCollision,0x10),-1.f),
             "gun glass lever 1: the Collision (component 2, element 5 from the part, damage 100, the player, 0xff, the normal toward the grip)");
       // the same pane on the next fast tick: no second call (edge / rate)
       gunFrame(V::add(tip0,{.168f,0,0}),{3,0,0});CHECK(stub::glassCalls==k0+1,"gun glass lever 1: the same pane on the next fast tick: no second call");
       P::leverPatchForTests.store(nullptr);st.leversRead=false;simNs+=3000*ms;}
      // lever 1: the gun hand buzzes only for a broken pane (a crack without a shatter: the call, no pulse)
      {namespace HX=rdrvr::vr::haptics;static const auto brk=+[](P::Levers& l){l.gunGlass=1;};P::leverPatchForTests.store(brk);st.leversRead=false;
       const auto tries=[]{return HX::posted.load()+HX::dropped.load();};
       for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});paneHit();
       const int k0=stub::glassCalls;const auto ns0=GG.noShatter.load(),h0=tries();
       stub::glassRefuse=true;gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});stub::glassRefuse=false;
       CHECK(stub::glassCalls==k0+1&&GG.noShatter.load()==ns0+1&&tries()==h0,"gun glass lever 1: a crack without a shatter: the call made, no haptic pulse");
       simNs+=3000*ms;for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});paneHit();const auto d0=GG.done.load(),h1=tries();
       gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});
       CHECK(GG.done.load()==d0+1&&tries()==h1+1,"gun glass lever 1: a broken pane: one haptic pulse");
       P::leverPatchForTests.store(nullptr);st.leversRead=false;simNs+=3000*ms;}
      // the exclusions: the mover, our hand bodies and the player's own instances (his ConfigureActorExcludeInsts list), no repeats; a hit on
      // one of his instances is counted as self
      {const int oc=st.ownCount;const auto ow=st.own;
       st.own={};st.own[0]=0x9990;st.own[1]=st.moverInstAddr;st.own[2]=0x9a00;st.ownCount=3;
       std::array<const void*,D::gunExclMax> ex{};const int n=D::gunExcludes(st,ex);
       bool mv=false,a=false,b=false;int movers=0;for(int i=0;i<n;++i){const auto p=stub::addr(ex[std::size_t(i)]);mv=mv||p==st.moverInstAddr;movers+=p==st.moverInstAddr?1:0;a=a||p==0x9990;b=b||p==0x9a00;}
       CHECK(mv&&a&&b&&movers==1&&n>=3&&n<=5&&D::gunOwnInst(st,0x9a00)&&D::gunOwnInst(st,st.moverInstAddr)&&!D::gunOwnInst(st,0x7770),
             "gun glass: the probe excludes the player's own instances (the mover once), and a hit on one is self");
       st.ownCount=0;std::array<const void*,D::gunExclMax> ex0{};const int n0=D::gunExcludes(st,ex0);
       CHECK(n0>=1&&n0<=3&&!D::gunOwnInst(st,0x9a00),"gun glass: no own list read yet: the mover and the hand bodies only");
       st.own=ow;st.ownCount=oc;}
      // gates: each refused before any probe, counted
      const auto gated=[&](P::GunWhy why,auto prep,auto undo){for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});prep();const int pc0=stub::probeCalls;const auto g0=GG.gated[why].load();
        gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});const bool ok=stub::probeCalls==pc0&&GG.gated[why].load()==g0+1;undo();return ok;};
      CHECK(gated(P::GunShot,[&]{gunBusy=P::gunBusyShot;},[&]{gunBusy=0;}),"gun glass gate: a shot this frame: no probe");
      CHECK(gated(P::GunSpin,[&]{gunBusy=P::gunBusySpin;},[&]{gunBusy=0;}),"gun glass gate: the spin keeps the gun: no probe");
      CHECK(gated(P::GunReload,[&]{gunBusy=P::gunBusyReload;},[&]{gunBusy=0;}),"gun glass gate: the cylinder open: no probe");
      {for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});gunBusy=P::gunBusyShot;gunFrame(tip0,{0,0,0});gunBusy=0;const int pc0=stub::probeCalls;const auto g0=GG.gated[P::GunShot].load();
       gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});CHECK(stub::probeCalls==pc0&&GG.gated[P::GunShot].load()==g0+1,"gun glass gate: a shot 28 ms ago (within 300 ms): no probe");}
      CHECK(gated(P::GunKind,[&]{gunKind=3;},[&]{gunKind=1;}),"gun glass gate: a rifle (gunglasslong 0): no probe");
      CHECK(gated(P::GunBridged,[&]{bridgedNow=1;},[&]{bridgedNow=0;}),"gun glass gate: a turn bridge: no probe");
      CHECK(gated(P::GunFoot,[&]{onFootNow=0;},[&]{onFootNow=1;}),"gun glass gate: not on foot: no probe");
      CHECK(gated(P::GunStale,[&]{tipOkNow=false;},[&]{tipOkNow=true;}),"gun glass gate: no tracking-space muzzle velocity: no probe");
      {for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});const int pc0=stub::probeCalls;const auto g0=GG.gated[P::GunLong].load();
       gunFrame(V::add(tip0,{.40f,0,0}),{3,0,0});CHECK(stub::probeCalls==pc0&&GG.gated[P::GunLong].load()==g0+1,"gun glass gate: a 40 cm sweep in one tick (a snap): no probe");}
      {gunNow=0;gunFrame(tip0,{0,0,0});gunNow=1;const int pc0=stub::probeCalls;const auto g0=GG.gated[P::GunSettling].load();
       gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});CHECK(stub::probeCalls==pc0&&GG.gated[P::GunSettling].load()==g0+1,"gun glass gate: a gun placed 28 ms ago: no probe");}
      {static const auto off=+[](P::Levers& l){l.gunGlass=-1;l.gunGlassFree=-1;};P::leverPatchForTests.store(off);st.leversRead=false;
       for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});const int pc0=stub::probeCalls;const auto t0=GG.ticks.load();gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});
       CHECK(stub::probeCalls==pc0&&GG.ticks.load()==t0,"gun glass: levers -1: nothing at all");P::leverPatchForTests.store(nullptr);st.leversRead=false;}
      // hits that are not a pane: the mover, another instance, a group without the glass bit: counted, never judged ok
      {for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});simNs+=3000*ms;
       alignas(16) static unsigned char other[0x100]{};alignas(16) static void* otherVt[8]{};otherVt[2]=reinterpret_cast<void*>(stub::base+0x777);stub::put<std::uintptr_t>(other,0,stub::addr(otherVt));
       stub::probeHit(0,st.moverInstAddr,pt,{1,0,0},0,0);stub::probeHit(1,stub::addr(other),pt,{1,0,0},0,0);stub::probeCount=2;
       const auto s0=GG.hits[2].load(),o0=GG.hits[3].load(),w0=GG.wouldBreak.load(),b0=GG.probes[1].load();
       gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});
       CHECK(GG.hits[2].load()>=s0+1&&GG.hits[3].load()>=o0+1&&GG.wouldBreak.load()==w0&&GG.probes[1].load()==b0+1,
             "gun glass: the mover and another instance: counted (self / other), no judge; then the barrel probe");
       const auto& L=stub::lastProbe;CHECK(near(L.r,.03f)&&near(L.seg[0],tip0[0]+.084f-.15f,1e-4f)&&near(L.seg[4],tip0[0]+.084f,1e-4f),"gun glass: the barrel probe from the bore point to the muzzle, r 3 cm");
       for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});simNs+=3000*ms;
       paneHit();stub::put<std::uint8_t>(group,0x53,0);const auto ng0=GG.judged[P::GlassNotGlass].load(),w1=GG.wouldBreak.load();
       gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});
       CHECK(GG.judged[P::GlassNotGlass].load()>=ng0+1&&GG.wouldBreak.load()==w1,"gun glass: a group without the glass bit: judged notGlass, no would-break");
       stub::put<std::uint8_t>(group,0x53,2);}
      // the empty hand while the left holds the pistol: its palm sweep (lever gunglassfree 0: would break), not near the gun's bore
      {gunNow=0;gunSource=1;otherGun=true;otherTip={-.3f,1.2f,.3f};otherBore={-.45f,1.2f,.3f};for(int i=0;i<12;++i){handVel={0,0,0};frame({.5f,1.2f,.3f},P::ClassHeld);}
       simNs+=3000*ms;paneHit();const int pc0=stub::probeCalls;const auto f0=GG.probes[2].load(),w0=GG.wouldBreak.load();
       handVel={3.f,0,0};frame({.584f,1.2f,.3f},P::ClassHeld);handVel={0,0,0};
       CHECK(stub::probeCalls==pc0+1&&GG.probes[2].load()==f0+1&&GG.wouldBreak.load()==w0+1&&near(stub::lastProbe.r,.04f),"gun glass free hand: the palm's sweep (r 4 cm) meets the pane: would break");
       for(int i=0;i<12;++i)frame({.5f,1.2f,.3f},P::ClassHeld);
       otherTip={.62f,1.2f,.3f};otherBore={.47f,1.2f,.3f};const int pc1=stub::probeCalls;const auto n0=GG.gated[P::GunNearGun].load();
       handVel={3.f,0,0};frame({.584f,1.2f,.3f},P::ClassHeld);handVel={0,0,0};
       CHECK(stub::probeCalls==pc1&&GG.gated[P::GunNearGun].load()==n0+1,"gun glass free hand: the palm within 15 cm of the bore (support / reload / fan): no probe");
       otherTip={-.3f,1.2f,.3f};otherBore={-.45f,1.2f,.3f};for(int i=0;i<12;++i)frame({.5f,1.2f,.3f},P::ClassHeld);
       gunSource=3;const int pc2=stub::probeCalls;const auto nf0=GG.gated[P::GunNotFree].load();
       handVel={3.f,0,0};frame({.584f,1.2f,.3f},P::ClassHeld);handVel={0,0,0};
       CHECK(stub::probeCalls==pc2&&GG.gated[P::GunNotFree].load()==nf0+1,"gun glass free hand: a support hand: no probe");
       gunSource=1;otherGun=false;for(int i=0;i<12;++i)frame({.5f,1.2f,.3f},P::ClassHeld);gunNow=1;gunSource=2;}
      // a throwing probe: counted, no crash; the prints refused or no probe function: no probe
      {for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});stub::probeThrow=true;const auto x0=GG.exceptions.load();gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});stub::probeThrow=false;
       CHECK(GG.exceptions.load()>=x0+1,"gun glass: an exception in the probe: counted, no crash");
       for(int i=0;i<12;++i)gunFrame(tip0,{0,0,0});P::groupOk[P::GroupGunGlass]=false;const int pc0=stub::probeCalls;gunFrame(V::add(tip0,{.084f,0,0}),{3,0,0});
       CHECK(stub::probeCalls==pc0,"gun glass: GUNGLASS prints refused: no probe");P::groupOk[P::GroupGunGlass]=true;}
      // the hand path is untouched: both hands with a pistol stay ClassHeld (no body)
      {P::ClassIn ci{};ci.onFoot=true;ci.sourceDerived=true;ci.heldHere=true;ci.weapon=true;ci.firearm=true;ci.placed=true;ci.hold=1;
       P::ClassIn cf{};cf.onFoot=true;cf.sourceFree=true;cf.weapon=true;cf.firearm=true;cf.hold=1;
       CHECK(P::classify(ci)==P::ClassHeld&&P::classify(cf)==P::ClassHeld,"gun glass: a pistol: both hands still ClassHeld (no hand body)");}
      {const auto r=P::report();CHECK(jsonValid(r)&&r.find("\"gunGlass\":{\"lever\":0,\"freeLever\":0")!=std::string::npos&&r.find("\"wouldBreak\":")!=std::string::npos&&r.find("\"excl\":{\"own\":")!=std::string::npos&&
             r.find("\"gunGlass\":0,\"gunGlassFree\":0,\"gunGlassMps\":2.00")!=std::string::npos&&r.find("\"src\":\"free\"")!=std::string::npos,"gun glass: the report parses (gunGlass, levers)");}
      gunNow=0;gunKind=-1;gunSource=1;tipRelNow={};boreRelNow={};tipVelNow={};stub::probeCount=0;P::engine.probeLevel=nullptr;for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});}
     stub::clearProps();actorAddr=0x4242;for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});}
    // ---------- GRAB ----------
    {wallOn=false;stub::clearProps();squeezeNow=0.f;for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});
     auto& G=P::stats.grab;auto& pp=stub::props[1];static unsigned char gbound[0x60]{};
     const auto place=[&](P::V3 at){stub::placeProp(pp,9,stub::navVtable,at,.05f,0);
        const float m[12]{1,0,0,0, 0,1,0,0, 0,0,1,0};std::memcpy(pp.inst+0x20,m,48);
        std::memset(gbound,0,sizeof gbound);const float mx[4]{.05f,.05f,.05f,0},mn[4]{-.05f,-.05f,-.05f,0};std::memcpy(gbound+0x30,mx,16);std::memcpy(gbound+0x40,mn,16);
        stub::put<std::uintptr_t>(pp.arch,0x18,stub::addr(gbound));
        // its collider (level +0x78[9]): the back pointer, a rigid type, 2 kg
        stub::levelColliders[9]=stub::addr(stub::colliders[9]);std::memset(stub::colliders[9],0,sizeof stub::colliders[9]);
        stub::put<std::uintptr_t>(stub::colliders[9],0x10,stub::addr(pp.inst));const float in[4]{.01f,.01f,.01f,2.f};std::memcpy(stub::colliders[9]+0xa0,in,16);};
     const auto touch=[&](P::V3 at,int n){for(int i=0;i<n;++i){D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);frame(at);}};
     const auto mass=[]{const auto i=stub::at<std::uint16_t>(P::objects[1].inst,0x10);return i<16?stub::at<float>(stub::colliders[i],0xac):-1.f;};
     const P::V3 palm=solvedRight().solved;place(V::add(palm,{.03f,0,0}));
     touch({.5f,1.2f,.3f},2);
     // the holsters take the press in the first frame the grip reaches 0.65, from a stamp of an earlier tick: an open hand is ready
     CHECK(GL::readyAt(1,simNs)&&D::state().grabCand[1]==stub::addr(pp.inst),"grab: an open hand at a prop: ready before the grip moves");
     {bool cached=false;for(const auto& c:D::state().grabCache[1])cached=cached||c.inst==stub::addr(pp.inst);CHECK(cached,"grab: the prop's parts cached for the hand");}
     // a quick squeeze 0 -> 0.8 in one frame: the holsters' rule (ready at the latch -> the grab branch) takes it, the next tick grabs
     {const auto l0=GL::latchBy[1][GL::LatchGrab].load();const auto a0=stub::attaches;
      squeezeNow=.8f;const bool r=GL::readyAt(1,simNs+5*ms);if(r){GL::noteLatch(1,GL::LatchGrab,.8f,simNs+5*ms);GL::press(1,simNs+5*ms);}
      touch({.5f,1.2f,.3f},1);
      CHECK(r&&GL::latchBy[1][GL::LatchGrab].load()==l0+1&&stub::attaches==a0+2&&D::state().grab[1].active,"grab: a 0 -> 0.8 squeeze in one frame: the latch takes the grab, held (welded: two attaches)");
      squeezeNow=.1f;for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});CHECK(!D::state().grab[1].active,"grab: (released)");
      for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});}
     // GRAB SEAT: a cup held by the fingertips (9 cm past the palm centre) is pulled into the palm before the attach
     {auto& K=P::stats.seat;squeezeNow=0.f;for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
      const auto hsv=solvedRight();const P::V3 F=V::normalized(hsv.rows[1]);const P::V3 B=hsv.solved;
      const auto seatPlace=[&](P::V3 c){place(c);float* rows=reinterpret_cast<float*>(stub::colliders[9]+0xd0);const float id[12]{1,0,0,0,0,1,0,0,0,0,1,0};std::memcpy(rows,id,48);
          std::memcpy(stub::colliders[9]+0x100,c.data(),12);stub::put<float>(stub::colliders[9],0xac,.35f);};
      const P::V3 c0=V::add(B,V::mul(F,.14f));seatPlace(c0);touch({.5f,1.2f,.3f},2);
      const auto a0=stub::attaches,f0=stub::fixedRotations;const auto s0=K.starts.load(),se0=K.seated.load();
      squeezeNow=.8f;GL::press(1,simNs+5*ms);touch({.5f,1.2f,.3f},1);
      CHECK(K.starts.load()==s0+1&&stub::attaches==a0&&!D::state().grab[1].active&&D::state().seat[1].on&&GL::holding(1),
            "seat: a fingertip cup 9 cm past the palm: pulled in, no attach yet, the hand holding for the holsters");
      {bool g=false;for(const auto& x:D::state().ghosts[1])g=g||x.inst==stub::addr(pp.inst);CHECK(g&&P::stats.ghost.starts[P::GhostSeat].load()>=1,"seat: a ghost on the cup while it comes in");}
      CHECK(K.velWrites.load()>0,"seat: the cup's collider velocity written each substep");
      // the cup arrives (the stub does not move props): put it at the palm, the next tick attaches once with fixed rotation
      const P::V3 B2=solvedRight().solved;seatPlace(V::add(B2,V::mul(F,.002f)));
      D::state().seat[1].seatLocal={0,0,0}; // the test cup's seat point: its centre, now 2 mm from the palm
      frame({.5f,1.2f,.3f});
      CHECK(K.seated.load()==se0+1&&stub::attaches==a0+2&&stub::fixedRotations==f0+1&&D::state().grab[1].active&&!D::state().seat[1].on&&
            D::state().grab[1].weld==2&&stub::lastFixedRotation==reinterpret_cast<void*>(D::state().grab[1].conRot),
            "seat: seated: welded (two AttachObjects, one SetFixedRotation on the second)");
      CHECK(D::state().grab[1].seatD0Cm>8.f&&D::state().grab[1].seatD0Cm<10.f&&D::state().grab[1].seatMs>=0.f&&D::state().grab[1].offsetCm>=0.f&&D::state().grab[1].offsetCm<2.f,
            "seat: the hold records the start distance (9 cm), the pull time, the offset at the attach (< 2 cm)");
      squeezeNow=.1f;for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});CHECK(!D::state().grab[1].active,"seat: (released)");
      {P::State::LightHold r{};for(const auto& x:D::state().lightHolds)if(x.set&&x.seatD0Cm>0.f)r=x;
       CHECK(r.set&&r.seatD0Cm>8.f&&r.offsetCm>=0.f&&r.maxOffCm>=r.offsetCm&&r.seatMs>=0.f,"seat: lightHolds keep offsetCm / maxOffCm / seatD0Cm / seatMs");}
      for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
      // a blocked cup: the timeout attaches it where it is
      {const auto t0=K.timeouts.load();const auto a1=stub::attaches;seatPlace(c0);squeezeNow=0.f;touch({.5f,1.2f,.3f},2);
       squeezeNow=.8f;GL::press(1,simNs+5*ms);touch({.5f,1.2f,.3f},1);
       for(int i=0;i<16&&K.timeouts.load()==t0;++i)frame({.5f,1.2f,.3f});
       CHECK(K.timeouts.load()==t0+1&&stub::attaches==a1+2&&D::state().grab[1].active&&D::state().grab[1].seatD0Cm>8.f,"seat: a cup that never arrives: attached (welded) at the timeout");
       squeezeNow=.1f;for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});}
      // a grip dip while pulling (one sample under .30): the pull rides through it, nothing cancelled or attached
      {const auto c1=K.cancels[0].load(),k1=K.dipsKept.load();const auto a1=stub::attaches;seatPlace(c0);squeezeNow=0.f;touch({.5f,1.2f,.3f},2);
       squeezeNow=.8f;GL::press(1,simNs+5*ms);touch({.5f,1.2f,.3f},1);squeezeNow=.1f;frame({.5f,1.2f,.3f});
       CHECK(K.cancels[0].load()==c1&&stub::attaches==a1&&D::state().seat[1].on&&GL::holding(1),"seat: a grip dip while pulling: still pulling, still holding");
       squeezeNow=.8f;frame({.5f,1.2f,.3f});
       CHECK(K.dipsKept.load()==k1+1&&D::state().seat[1].on&&K.cancels[0].load()==c1,"seat: the grip back: a dip ridden through");
       // the hand opens for good: cancelled once the grip stayed under .30 for grabOpenMs, nothing attached, the hold flags off
       squeezeNow=.1f;int n=0;for(;n<20&&D::state().seat[1].on;++n)frame({.5f,1.2f,.3f});
       CHECK(K.cancels[0].load()==c1+1&&stub::attaches==a1&&!D::state().seat[1].on&&!GL::holding(1)&&n>=12,
             "seat: the hand opened while pulling: cancelled after grabOpenMs, nothing attached, not holding");
       for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});}
      // the hand opens fast while pulling (a throw): attached at once, the hold's own gate throws it on the next tick
      {const auto th=K.thrown.load(),te=P::stats.throws.lightGrabThrows.load();const auto a1=stub::attaches;seatPlace(c0);squeezeNow=0.f;touch({.5f,1.2f,.3f},2);
       squeezeNow=.8f;GL::press(1,simNs+5*ms);touch({.5f,1.2f,.3f},1);
       CHECK(D::state().seat[1].on,"seat: (pulling)");
       squeezeNow=.1f;handVel={2.f,0,0};frame({.5f,1.2f,.3f});
       CHECK(K.thrown.load()==th+1&&stub::attaches==a1+2&&!D::state().seat[1].on,"seat: a 2 m/s open while pulling: attached (welded) for the throw");
       frame({.5f,1.2f,.3f});handVel={0,0,0};
       CHECK(!D::state().grab[1].active&&GL::lastEnd[1].load()==P::GrabEndThrow&&P::stats.grab.ends[P::GrabEndThrow].load()>=1,"seat: the hold then ends as a throw (grabStep)");
       (void)te;for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});}
      // lever grabseat 0: attached at once where it is (the old hold)
      {P::leverPatchForTests.store([](P::Levers& l){l.grabSeat=false;});D::state().leversRead=false;frame({.5f,1.2f,.3f});
       const auto s1=K.starts.load();const auto a1=stub::attaches;seatPlace(c0);squeezeNow=0.f;touch({.5f,1.2f,.3f},2);
       squeezeNow=.8f;GL::press(1,simNs+5*ms);touch({.5f,1.2f,.3f},1);
       CHECK(K.starts.load()==s1&&stub::attaches==a1+2&&D::state().grab[1].active&&D::state().grab[1].seatD0Cm<0.f,"seat: lever 0: attached (welded) at once");
       squeezeNow=.1f;for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});P::leverPatchForTests.store(nullptr);D::state().leversRead=false;for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});}
      squeezeNow=0.f;}
     // LIGHT GHOST through CollideObjects: the released 2 kg prop's release ghost ends once the pair is gone; an open hand's pair collides,
     // a closing hand's pair is dropped and the prop stays the grab candidate
     {const auto& Gh=P::stats.ghost;const auto r0=Gh.starts[P::GhostRelease].load();
      CHECK(r0>=1,"ghost: a fixed-rotation hold's end arms the release ghost");
      for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
      bool none=true;for(const auto& g:D::state().ghosts[1])none=none&&!g.inst;CHECK(none&&Gh.ends[P::GhostQuiet].load()>=1,"ghost: ends when the pair no longer overlaps");
      const auto d0=Gh.drops.load(),i0=Gh.starts[P::GhostIntent].load();
      squeezeNow=0.f;place(V::add(solvedRight().solved,{.03f,0,0}));touch({.5f,1.2f,.3f},2);
      CHECK(Gh.drops.load()==d0,"ghost: an open hand's pair with the prop collides");
      // fingers resting at .4 for a while (no rise): the pair collides, counted as resting
      {const auto r0=Gh.restingPairs.load();squeezeNow=.4f;for(int i=0;i<40;++i)frame({.5f,1.2f,.3f});
       place(V::add(solvedRight().solved,{.03f,0,0}));touch({.5f,1.2f,.3f},2);
       CHECK(Gh.drops.load()==d0&&Gh.starts[P::GhostIntent].load()==i0&&Gh.restingPairs.load()>r0,"ghost: a steady .4 grip pushes the prop (no ghost)");
       squeezeNow=0.f;for(int i=0;i<40;++i)frame({.5f,1.2f,.3f});}
      squeezeNow=.4f;touch({.5f,1.2f,.3f},2);
      CHECK(Gh.drops.load()>d0&&Gh.starts[P::GhostIntent].load()==i0+1&&D::state().grabCand[1]==stub::addr(pp.inst),
            "ghost: a closing hand's pair with a 2 kg prop is dropped, the prop stays the grab candidate");
      squeezeNow=0.f;for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});}
     // the ready stamp holds readyMaxNs after the last tick with a candidate (no pair any more), then expires
     {squeezeNow=0.f;place(V::add(solvedRight().solved,{.03f,0,0}));touch({.5f,1.2f,.3f},2);const auto t0=simNs;
      for(int i=0;i<3;++i)frame({.5f,1.2f,.3f}); // no CollideObjects: the pair is stale after 4 substeps
      CHECK(D::state().grabCand[1]==0&&GL::readyAt(1,simNs)&&simNs-t0<=100*ms,"grab: no candidate this tick, the ready stamp holds 100 ms");
      // the hold-over: a press now grabs the last candidate (its part again within 4 cm)
      const auto h0=G.holdOver.load();GL::press(1,simNs);frame({.5f,1.2f,.3f});
      CHECK(G.holdOver.load()==h0+1&&D::state().grab[1].active,"grab: a press in the hold-over grabs the last candidate");
      squeezeNow=.1f;for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});squeezeNow=0.f;
      for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});CHECK(!GL::readyAt(1,simNs),"grab: the stamp expired");}
     // a latch the grab did not get, then a ready 1 frame later: readyAfterLatch says how late
     {const auto b1=G.readyAfterLatch[1].load()+G.readyAfterLatch[2].load();const auto c10=G.latchClass[10].load();
      place(V::add(solvedRight().solved,{.03f,0,0}));D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr);
      GL::noteLatch(1,GL::LatchNative,.7f,simNs);simNs+=10*ms;touch({.5f,1.2f,.3f},1);
      CHECK(G.readyAfterLatch[1].load()+G.readyAfterLatch[2].load()==b1+1&&G.latchClass[10].load()==c10+1,"grab: a ready 38 ms after a native latch: counted, the class met before it");
      for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});}
     // the fingers first: the palm centre 10 cm off the prop along F is found by the finger point
     {const P::V3 b=solvedRight().solved;const P::V3 f=V::normalized(D::state().hand[1].solvedRows[1]);
      const auto pts=P::grabPoints(b,D::state().hand[1].solvedRows,1.f);CHECK(nearV(pts[2],V::add(b,V::mul(f,.12f)),1e-5f),"grab points: B, B + 6 cm F, B + 12 cm F");
      place(V::add(b,V::mul(f,.16f)));const auto m0=G.misses[P::MissFar].load();touch({.5f,1.2f,.3f},2);
      CHECK(D::state().grabCand[1]==stub::addr(pp.inst)&&G.misses[P::MissFar].load()==m0,"grab: a prop 16 cm along the fingers (11 cm from the palm box): the finger point finds it");
      place(V::add(b,{0,.4f,0}));touch({.5f,1.2f,.3f},2);
      CHECK(D::state().grabCand[1]==0&&G.misses[P::MissFar].load()>m0&&G.missDist[5].load()>=1,"grab: a prop 40 cm away: a far miss, its distance bucket");
      place(V::add(solvedRight().solved,{.03f,0,0}));}
     // a prop no grab can make physical: never ready (the grip stays the game's), judged once per instance
     {const auto i0=G.misses[P::MissInfinite].load(),n0=G.misses[P::MissNeverMove].load();
      place(V::add(solvedRight().solved,{.03f,0,0}));stub::put<std::uint8_t>(pp.inst,0x82,0x80);D::state().grabCache={};touch({.5f,1.2f,.3f},2);
      CHECK(D::state().grabCand[1]==0&&G.misses[P::MissInfinite].load()>i0,"grab: infinite mass vs movers: never a candidate");
      {const auto cf=G.cacheFills.load(),i1=G.misses[P::MissInfinite].load();touch({.5f,1.2f,.3f},2);touch({.5f,1.2f,.3f},2);
       CHECK(G.cacheFills.load()==cf&&G.misses[P::MissInfinite].load()>i1,"grab: a never verdict is kept: met again, judged no more");}
      static unsigned char fixedType[0x200]{};stub::put<float>(fixedType,0x1c0,-1.f);
      place(V::add(solvedRight().solved,{.03f,0,0}));stub::put<std::uint32_t>(pp.inst,0x80,0x1u);stub::put<std::uintptr_t>(pp.inst,0x98,stub::addr(fixedType));
      D::state().grabCache={};touch({.5f,1.2f,.3f},2);
      CHECK(D::state().grabCand[1]==0&&G.misses[P::MissNeverMove].load()>n0,"grab: a fixed root whose type never moves (minMove < 0): never a candidate");
      // furniture whose type never moves (a chair: 21 kg, type-locked, minMove < 0): grabbed on purpose, released, unlocked, woken; a ball joint
      {static unsigned char chairType[0x200]{},cComp[0x100]{},cCompVt[0x100]{},cPartVt[0x100]{},cPart[0x40]{},cList[0x40]{};
       stub::put<float>(chairType,0x1c0,-1.f);stub::put<std::uint16_t>(chairType,0x1ba,0x825);stub::put<std::uintptr_t>(chairType,0x130,stub::addr(cComp));
       stub::put<std::uint8_t>(cComp,0x1c,0xc);stub::put<std::uintptr_t>(cComp,0,stub::addr(cCompVt));stub::put<std::uintptr_t>(cCompVt,0xd0,stub::base+P::off::compCanActive);
       stub::put<std::uint8_t>(cComp,0xd4,0);stub::put<std::uint16_t>(cComp,0xd2,1);stub::put<std::uintptr_t>(cComp,0xa0,stub::addr(cList));
       stub::put<std::uintptr_t>(cList,0,stub::addr(cPart));stub::put<std::uintptr_t>(cPart,0,stub::addr(cPartVt));stub::put<std::uintptr_t>(cPartVt,0xd0,stub::base+P::off::partCanActive[0]);
       for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});D::state().grabCache={};place(V::add(solvedRight().solved,{.03f,0,0}));stub::setPropState(9,1);
       stub::put<std::uint32_t>(pp.inst,0x80,0x1u);stub::put<std::uintptr_t>(pp.inst,0x98,stub::addr(chairType));stub::put<float>(pp.arch,0x38,21.25f);
       stub::put<float>(stub::colliders[9],0xac,21.25f);
       const auto ok0=G.moveBy[P::GrabMoveOk].load();squeezeNow=.5f;touch({.5f,1.2f,.3f},2);
       CHECK(D::state().grabCand[1]==stub::addr(pp.inst)&&G.moveBy[P::GrabMoveOk].load()>ok0,"grab: a chair (type never moves): a candidate on purpose");
       const int fr0=stub::fixedRotations,u0=stub::unlockCalls;const auto mr0=G.moveReleased.load(),ma0=G.moveActivated.load();
       GL::press(1,simNs);touch({.5f,1.2f,.3f},1);
       CHECK(D::state().grab[1].active&&(stub::at<std::uint32_t>(pp.inst,0x80)&0x2u)&&G.moveReleased.load()==mr0+1&&G.moveActivated.load()==ma0+1&&
             stub::unlockCalls>u0&&stub::propState(9)==0&&stub::fixedRotations==fr0,"grab: the chair released (+0x80 bit 1), unlocked, active, held on a ball joint");
       squeezeNow=.1f;for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});squeezeNow=0.f;
       CHECK(!D::state().grab[1].active,"grab: the chair let go");
       // asleep and untouched for a second: its +0x80 bits as they were (the root fixed, no activation override)
       {const auto rs0=G.moveRestored.load();stub::setPropState(9,1);for(int i=0;i<20;++i)frame({.5f,1.2f,.3f});
        CHECK(G.moveRestored.load()==rs0&&(stub::at<std::uint32_t>(pp.inst,0x80)&0x2u),"grab: the released chair asleep under a second: still released");
        for(int i=0;i<30;++i)frame({.5f,1.2f,.3f});
        CHECK(G.moveRestored.load()==rs0+1&&(stub::at<std::uint32_t>(pp.inst,0x80)&P::movedBits)==0u,"grab: the chair asleep and untouched a second: its +0x80 bits back");}
       // a wake that fails: the release and the unlock rolled back at once
       {for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});D::state().grabCache={};place(V::add(solvedRight().solved,{.03f,0,0}));stub::setPropState(9,1);
        stub::put<std::uint32_t>(pp.inst,0x80,0x1u);stub::put<std::uintptr_t>(pp.inst,0x98,stub::addr(chairType));stub::put<float>(pp.arch,0x38,21.25f);
        squeezeNow=.5f;touch({.5f,1.2f,.3f},2);stub::refuseActivation=true;const auto rb0=G.moveRolledBack.load();
        GL::press(1,simNs);touch({.5f,1.2f,.3f},1);stub::refuseActivation=false;
        CHECK(!D::state().grab[1].active&&G.moveRolledBack.load()==rb0+1&&stub::at<std::uint32_t>(pp.inst,0x80)==0x1u,
              "grab: the chair's wake failed: +0x80 back as it was (no release bit, no activation override)");
        squeezeNow=0.f;for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});}
       // a creature's capsule beside the chair: still a candidate (startGrab judges it again), never cached as never-grabbable
       {D::state().grabCache={};P::VBodies vb{};vb.ns=simNs;vb.count=1;auto& bd=vb.b[0];bd.guid=0x777;bd.count=1;
        P::V3 c{};float rad{-1.f};D::checkProp(D::state(),stub::addr(pp.inst),c,rad);
        bd.c[0].a=V::add(c,{rad+.05f,-.5f,0});bd.c[0].b=V::add(c,{rad+.05f,.5f,0});bd.c[0].r=.1f;bd.centre=V::add(c,{rad+.05f,0,0});bd.radius=.6f;
        auto& S=D::state();const bool f0=S.vbFresh;const auto vb0=S.vb;S.vbFresh=true;S.vb=vb;const auto nn0=G.moveBy[P::GrabMoveNpcNear].load();
        const auto never=D::grabNever(S,stub::addr(pp.inst));
        CHECK(never==0&&G.moveBy[P::GrabMoveNpcNear].load()==nn0+1,"grab: a creature beside the chair: a candidate still (the creature is judged at the grab)");
        bd.c[0].a=V::add(c,{rad+.5f,-.5f,0});bd.c[0].b=V::add(c,{rad+.5f,.5f,0});S.vb=vb;const auto ok1=G.moveBy[P::GrabMoveOk].load();
        CHECK(D::grabNever(S,stub::addr(pp.inst))==0&&G.moveBy[P::GrabMoveOk].load()==ok1+1,"grab: a creature 35 cm from the chair's sphere: not near (capsule overlap test)");
        S.vbFresh=f0;S.vb=vb0;D::state().grabCache={};}
       stub::put<std::uintptr_t>(pp.inst,0x98,0);stub::put<float>(pp.arch,0x38,0.f);}
      for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});D::state().grabCache={};place(V::add(solvedRight().solved,{.03f,0,0}));}
     squeezeNow=.5f;touch({.5f,1.2f,.3f},2);
     CHECK(GL::readyAt(1,simNs)&&D::state().grabCand[1]==stub::addr(pp.inst)&&D::state().grabCandPart[1]==0,"grab: the grip closing at a prop within 4 cm: ready, part 0");
     const int a0=stub::attaches;GL::press(1,simNs);touch({.5f,1.2f,.3f},1);
     CHECK(stub::attaches==a0+2&&D::state().grab[1].active&&GL::holding(1)&&stub::fixedRotations>=1&&near(mass(),8.f)&&G.starts.load()>=1,
           "grab: the press holds the prop (welded: two AttachObjects, fixed rotation on the second, the held mass 8 kg)");
     {const auto& g=D::state().grab[1];
      CHECK(g.weld==2&&g.conRot&&g.conRot!=g.con&&g.flagsPos==2&&g.flagsRot==3&&stub::at<std::uint16_t>(reinterpret_cast<void*>(g.con),0xbc)==2&&
            stub::at<std::uint16_t>(reinterpret_cast<void*>(g.conRot),0xbc)==3,"weld: the position constraint keeps flags 2, the rotation constraint reads 3");}
     CHECK(!D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,pp.inst,nullptr,nullptr),"grab: the held prop's pair with that hand is dropped");
     // the drive: a force on the held prop at the grip each substep, toward where the hand is going
     {const int f0=stub::forces;const auto d0=G.driveApplied.load();frame({.5f,1.2f,.6f});
      CHECK(stub::forces>=f0+2&&G.driveApplied.load()>=d0+2&&stub::lastForce[2]>0.f&&stub::lastForceComp==0,"grab drive: ApplyForce on the held part, toward the hand's command");
      for(int i=0;i<3;++i)frame({.5f,1.2f,.3f});}
     {auto& S=D::state();auto& g=S.grab[1];const auto col=g.propCol;
      CHECK(g.levelIndex!=0xffff&&col,"grab drive: the prop's level index kept at the grab (the step reads only the level's arrays)");
      // two hands on the prop: the other hand's force of this substep in the accumulator is no stale one
      {float acc[4]{5.f,0,0,0};std::memcpy(reinterpret_cast<void*>(col+0x150),acc,16);const auto o0=S.grab[0];S.grab[0]=g;S.grab[0].applyNs=rdrvr::vr::nowNs();
       const int f0=stub::forces;const auto sh0=G.driveShared.load(),st0=G.driveStale.load();
       D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
       CHECK(stub::forces==f0+1&&G.driveShared.load()==sh0+1&&G.driveStale.load()==st0,"grab drive: both hands on one prop: each hand's force (no stale refusal)");
       S.grab[0]=o0;S.grab[0].active=false;
       // the drive's own force of the substep before, unconsumed (the step skipped the prop): stale
       g.accAfter={5.f,0,0};g.accStep=S.substep-1;
       D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
       CHECK(G.driveStale.load()==st0+1&&g.staleRun==1,"grab drive: its own force of the substep before still in the accumulator: stale");
       // another force there (the player's mover queues its weight on its ground): foreign, the drive goes on, the stale run ends
       {const auto fo0=G.driveForeign.load();const int f1=stub::forces;g.accAfter={5.f,0,0};g.accStep=S.substep-1;
        float acc2[4]{5.f,-700.f,0,0};std::memcpy(reinterpret_cast<void*>(col+0x150),acc2,16);
        D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
        CHECK(G.driveForeign.load()==fo0+1&&G.driveStale.load()==st0+1&&stub::forces==f1+1&&g.staleRun==0&&!g.driveOff&&g.accStep==S.substep,
              "grab drive: a foreign force in the accumulator: counted, the drive applies, no stale run");
        // the old rule (lever grabstaleown 0): any force left there is stale
        S.lev.grabStaleOwn=false;D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
        CHECK(G.driveStale.load()==st0+2,"grab drive: lever grabstaleown 0: a foreign force counts as stale");S.lev.grabStaleOwn=true;g.staleRun=0;
        // a foreign force over 3 x grabForceMaxN: the drive off
        const auto ru0=G.driveRunaway.load();float acc3[4]{0,-5000.f,0,0};std::memcpy(reinterpret_cast<void*>(col+0x150),acc3,16);
        D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
        CHECK(G.driveRunaway.load()==ru0+1&&g.driveOff&&g.rec.driveOffMs>=0.f,"grab drive: a foreign force over 3 x grabForceMaxN: the drive off");
        {float after[8]{};std::memcpy(after,reinterpret_cast<const void*>(col+0x150),32);bool zero=true;for(const float x:after)zero=zero&&x==0.f;
         CHECK(zero&&G.runawayCleared.load()>=1,"grab drive: a runaway accumulator is cleared (nothing launches the prop at the next step)");}
        g.driveOff=false;g.rec.driveOffMs=-1.f;
        // a torque over 3 x grabForceMaxN x 1 m: a runaway as well
        {const auto ru1=G.driveRunaway.load();float qt[8]{1.f,0,0,0,0,0,3000.f,0};std::memcpy(reinterpret_cast<void*>(col+0x150),qt,32);g.staleRun=0;
         D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
         CHECK(G.driveRunaway.load()==ru1+1&&g.driveOff,"grab drive: a torque over 3 x grabForceMaxN x 1 m: a runaway");g.driveOff=false;g.rec.driveOffMs=-1.f;}
        // the drive's own force unconsumed two substeps later (two skipped steps): still its own, nothing applied on top (no pile-up)
        {float qa[8]{5.f,0,0,0,0,0,1.f,0};std::memcpy(reinterpret_cast<void*>(col+0x150),qa,32);g.accAfter={5.f,0,0};g.accTauAfter={0,0,1.f};g.accStep=S.substep-2;
         g.staleRun=0;const int f2=stub::forces;const auto st1=G.driveStale.load(),sc0=G.staleCleared.load();
         for(int k=0;k<2;++k)D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
         CHECK(stub::forces==f2&&G.driveStale.load()==st1+2&&g.staleRun==2&&!g.driveOff,"grab drive: its own force left two substeps on: stale, no force on top");
         // its torque changed (another force at a point): not its own
         float qb[8]{5.f,0,0,0,0,0,9.f,0};std::memcpy(reinterpret_cast<void*>(col+0x150),qb,32);const auto fo1=G.driveForeign.load();
         D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
         CHECK(G.driveForeign.load()==fo1+1&&stub::forces==f2+1&&g.staleRun==0,"grab drive: the same force with another torque: foreign");
         // three own reads running: the drive off and its own force taken back
         std::memcpy(reinterpret_cast<void*>(col+0x150),qa,32);g.accAfter={5.f,0,0};g.accTauAfter={0,0,1.f};g.accStep=S.substep-1;g.staleRun=0;
         for(int k=0;k<3;++k)D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
         float after[8]{};std::memcpy(after,reinterpret_cast<const void*>(col+0x150),32);bool zero=true;for(const float x:after)zero=zero&&std::fabs(x)<1e-6f;
         CHECK(g.driveOff&&G.staleCleared.load()==sc0+1&&zero,"grab drive: three own reads: the drive off, its unconsumed force taken back");
         g.driveOff=false;g.rec.driveOffMs=-1.f;g.staleRun=0;}
        // two hands on one prop: an apply marks both hands (the other hand's next read finds the pair's own force)
        {std::memset(reinterpret_cast<void*>(col+0x150),0,32);const auto o1=S.grab[0];S.grab[0]=g;S.grab[0].applyNs=0;S.grab[0].accStep=0;S.grab[0].accAfter={};
         float qc[8]{3.f,4.f,0,0,0,0,0,0};std::memcpy(reinterpret_cast<void*>(col+0x150),qc,32);g.accAfter={3.f,4.f,0};g.accTauAfter={};g.accStep=0;
         D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
         CHECK(S.grab[0].accStep==S.substep&&nearV(S.grab[0].accAfter,g.accAfter)&&g.accStep==S.substep,"grab drive: two hands on one prop: the apply marks both hands");
         S.grab[0]=o1;S.grab[0].active=false;std::memset(reinterpret_cast<void*>(col+0x150),0,32);g.staleRun=0;}
        // the stale ring holds the events (kind 0 own, 1 foreign, 2 runaway)
        bool k0=false,k1=false,k2=false;for(const auto& q:S.staleRing){k0=k0||(q[8]==0&&q[3]==5);k1=k1||(q[8]==1&&q[4]==-700);k2=k2||(q[8]==2&&q[4]==-5000);}
        CHECK(k0&&k1&&k2&&S.staleN>=4,"grab drive: the stale ring: own, foreign, runaway with the accumulator's N");}
       // the player's mover standing on the held prop (its ground = the prop's level index and generation): the queued force and torque
       // are zeroed before the drive (lever grabgroundzero); another ground, or the lever off, leaves them (a foreign force)
       {static std::uint16_t gens[64]{};gens[g.levelIndex&63u]=7;std::uintptr_t old68{};std::memcpy(&old68,reinterpret_cast<const void*>(S.level+0x68),8);
        const std::uintptr_t ga=reinterpret_cast<std::uintptr_t>(gens);std::memcpy(reinterpret_cast<void*>(S.level+0x68),&ga,8);
        const bool mo=S.moverOk;S.moverOk=true;CHECK(S.moverInstAddr!=0,"grab drive: the stub's mover instance");
        const auto put16=[&](std::size_t o,std::uint16_t x){std::memcpy(reinterpret_cast<void*>(S.moverInstAddr+o),&x,2);};
        put16(0x6c0,g.levelIndex);put16(0x6c2,7);CHECK(!S.lev.grabGroundZero&&S.lev.grabGroundLift,"grab drive: grabgroundzero off, grabgroundlift on by default");
        S.lev.grabGroundZero=true;
        float q[8]{0,-785.f,0,0,1.f,2.f,3.f,0};std::memcpy(reinterpret_cast<void*>(col+0x150),q,32);g.staleRun=0;
        const auto gz0=G.groundZeroed.load(),gh0=G.groundHeld.load(),fo0=G.driveForeign.load();const int f0=stub::forces;
        D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
        float after[8]{};std::memcpy(after,reinterpret_cast<const void*>(col+0x150),32);bool zero=true;for(const float x:after)zero=zero&&x==0.f;
        CHECK(G.groundZeroed.load()==gz0+1&&G.groundHeld.load()==gh0+1&&G.driveForeign.load()==fo0&&stub::forces==f0+1&&zero&&g.rec.groundZeroed>=1,
              "grab drive: the player's mover on the held prop: its queued force and torque zeroed, the drive applies");
        put16(0x6c2,8);std::memcpy(reinterpret_cast<void*>(col+0x150),q,32);
        D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
        CHECK(G.groundZeroed.load()==gz0+1&&G.driveForeign.load()==fo0+1,"grab drive: another generation at that index: not the prop, the force left (foreign)");
        put16(0x6c2,7);S.lev.grabGroundZero=false;std::memcpy(reinterpret_cast<void*>(col+0x150),q,32);
        D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
        CHECK(G.groundZeroed.load()==gz0+1&&G.groundHeld.load()==gh0+2&&G.driveForeign.load()==fo0+2,"grab drive: lever grabgroundzero 0: counted, not zeroed");
        // the player's mover on the held prop: the drive's upward force at most the prop's weight (lever grabgroundlift)
        {std::memset(reinterpret_cast<void*>(col+0x150),0,32);g.staleRun=0;const auto gl0=G.groundLiftCapped.load();const int f1=stub::forces;
         D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,.6f,0}),1.f/72.f,1.f,S.hand[1]);
         const float cap=D::driveKgOf(S,g)*9.81f;
         CHECK(stub::forces==f1+1&&G.groundLiftCapped.load()==gl0+1&&stub::lastForce[1]<=cap+1e-2f&&g.rec.groundLift>=1,
               "grab drive: the player on the held prop: the hand's upward force capped at the prop's weight");
         S.lev.grabGroundLift=false;std::memset(reinterpret_cast<void*>(col+0x150),0,32);
         D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,.6f,0}),1.f/72.f,1.f,S.hand[1]);
         CHECK(G.groundLiftCapped.load()==gl0+1&&stub::lastForce[1]>cap,"grab drive: lever grabgroundlift 0: the upward force uncapped");S.lev.grabGroundLift=true;}
        S.lev.grabGroundZero=false;put16(0x6c0,0xffff);std::memcpy(reinterpret_cast<void*>(S.level+0x68),&old68,8);S.moverOk=mo;}
       std::memset(reinterpret_cast<void*>(col+0x150),0,16);g.staleRun=0;}
      // an articulated collider (type 1): refused by default (its ApplyForce runs the instance's damage notification)
      {stub::put<std::int32_t>(reinterpret_cast<void*>(col),0x8,1);const int f0=stub::forces;const auto ar0=G.driveArtRefused.load();
       D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
       CHECK(stub::forces==f0&&G.driveArtRefused.load()==ar0+1,"grab drive: an articulated collider: refused (lever grabdriveart 0)");
       stub::put<std::int32_t>(reinterpret_cast<void*>(col),0x8,0);}
      // the level's slot holding another instance (removed in between): nothing read through the stale pointer, no force
      {const auto idx0=g.levelIndex;g.levelIndex=std::uint16_t(idx0+1);const int f0=stub::forces;const auto nc0=G.driveNoCol.load();
       D::grabDriveStep(S,1,S.hand[1].solvedPos,V::add(S.hand[1].solvedPos,{0,0,.05f}),1.f/72.f,1.f,S.hand[1]);
       CHECK(stub::forces==f0&&G.driveNoCol.load()==nc0+1,"grab drive: the level slot no longer the prop's: no force");g.levelIndex=idx0;}}
     {P::Solved sv{};P::solved.load(sv);CHECK(sv.hand[1].grab==1,"grab: published for hook A (the hand at its solved pose)");}
     // a ball-jointed hold (a prop over grabrotkg): the force at the grip point, the hand body moving with the grip, the grip published
     {auto& S=D::state();auto& g=S.grab[1];const auto col=g.propCol;const auto saved=g;
      const float rows[12]{0,1,0,0,-1,0,0,0,0,0,1,0}; // the prop turned 90 degrees about z
      std::memcpy(reinterpret_cast<void*>(col+0xd0),rows,48);const P::V3 hp=S.hand[1].solvedPos;
      const float o[4]{hp[0]+.1f,hp[1],hp[2],0.f},zero[4]{};std::memcpy(reinterpret_cast<void*>(col+0x100),o,16);
      std::memcpy(reinterpret_cast<void*>(col+0x110),zero,16);std::memcpy(reinterpret_cast<void*>(col+0x120),zero,16);
      const P::Rows R{P::V3{0,1,0},P::V3{-1,0,0},P::V3{0,0,1}};
      g.ball=true;g.gripLocal=P::toFrame(R,{o[0],o[1],o[2]},hp);g.prevN={};g.stall={};g.rec.follow=0;g.rec.sepHist={};
      CHECK(nearV(P::fromFrame(R,{o[0],o[1],o[2]},g.gripLocal),hp,1e-5f),"grab ball: the grip in the collider's frame and back");
      const int f0=stub::forces;frame({.5f,1.2f,.32f});
      P::Solved sv{};P::solved.load(sv);
      CHECK(stub::forces>=f0+2&&nearV({stub::lastForcePos[0],stub::lastForcePos[1],stub::lastForcePos[2]},hp,1e-4f),"grab ball: the force applied at the grip point");
      CHECK(sv.hand[1].ball==1&&sv.hand[1].gripOk==1&&nearV(sv.hand[1].grip,hp,1e-4f)&&g.rec.follow>=2,"grab ball: the hand body follows the grip, the grip published for hook A");
      // the prop turned on: the grip turns with it (a spin about z at its centre of mass)
      const float w[4]{0,0,2.f,0};std::memcpy(reinterpret_cast<void*>(col+0x120),w,16);
      const auto gn=P::gripNow(col,g.gripLocal);
      CHECK(gn.ok&&nearV(gn.vGrip,V::cross({0,0,2.f},V::sub(hp,{o[0],o[1],o[2]})),1e-4f)&&nearV(gn.vCom,{0,0,0}),"grab ball: the grip's velocity from the collider's spin about its centre of mass");
      std::memcpy(reinterpret_cast<void*>(col+0x120),zero,16);
      // PROPHIT (geometric): the held prop's box (+-5 cm about its instance origin) swung into a creature's capsule 2 cm beyond its face
      {const auto lv0=S.lev;const auto kg0=g.propKg;const auto cls0=g.cls;const auto vb0=S.vb;const bool vf0=S.vbFresh;const auto mv0=S.hand[1].moverVel;
       g.propKg=59.5f;g.cls=P::classNav;S.lev.propHitGeo=true;S.lev.propHitMps=1.5f;S.lev.propHitStrongMps=2.5f;S.geoGate=P::PropGeoGate{};S.propPrev={};
       S.lev.propHitArmMs=400.f;S.lev.propHitArmDirMps=.6f;
       const P::V3 io{stub::at<float>(pp.inst,0x50),stub::at<float>(pp.inst,0x54),stub::at<float>(pp.inst,0x58)};
       std::uint64_t now=rdrvr::vr::nowNs();
       const auto body=[&](float x){P::VBodies vb{};vb.ns=now;vb.count=1;auto& b=vb.b[0];b.actor=stub::addr(stub::npcActor);b.guid=stub::npcGuid;b.race=0;b.count=1;
           b.c[0]={{io[0]+x,io[1]-.4f,io[2]},{io[0]+x,io[1]+.4f,io[2]},.06f,2,2};P::finishBody(b);S.vb=vb;S.vbFresh=true;};
       const auto vel=[&](float vx){const float v4[4]{vx,0,0,0};std::memcpy(reinterpret_cast<void*>(col+0x110),v4,16);};
       const auto newest=[]{P::PropHitRequest q{};const auto l=P::propHitLatest.load();if(l)P::propHitRing[l%P::propHitSlots].load(q);return q;};
       const auto armAt=[&](P::V3 v,std::uint64_t t){S.armTrail={};S.armTrail[1].push(v,t);};
       auto& Q=P::stats.propGeo;const auto s0=Q.sent.load();
       S.grabCache[1]={};body(.13f);vel(3.f);armAt({2.f,0,0},now);D::propGeoTick(S,now,{});
       const auto r=newest();
       CHECK(Q.sent.load()==s0+1&&r.guid==stub::npcGuid&&r.strong==1&&near(r.speed,3.f,.05f)&&nearV(r.dir,{1,0,0},1e-3f)&&r.hands==2&&Q.parts.load()>0,
             "prop hit geo: the chair's box swung at 3 m/s into a capsule 2 cm off its face: a strong hit along the swing (the player's strike)");
       {bool ring=false;for(const auto& x:S.geoRing)ring=ring||(x.set&&x.guid==stub::npcGuid&&x.why==P::PropHitOk&&x.strong&&near(x.distCm,2.f,.2f)&&near(x.arm,2.f));
        CHECK(ring,"prop hit geo: the ring holds the hit (2 cm from the capsule, the arm 2 m/s)");}
       // the cache entry the geometric hit made skipped grabNever's verdict; the first grab read of it judges it
       {P::GrabCache* ce=nullptr;for(auto& c:S.grabCache[1])if(c.inst==stub::addr(pp.inst))ce=&c;
        CHECK(ce&&!ce->judged,"prop hit geo: its cache entry made without grabNever's verdict");
        const auto f0=P::stats.grab.cacheFills.load();D::nearestPartOf(S,1,stub::addr(pp.inst),{io,io,io});
        CHECK(ce->judged&&P::stats.grab.cacheFills.load()==f0,"prop hit geo: a grab read of that entry judges it (no new fill)");}
       now+=20*ms;armAt({2.f,0,0},now);D::propGeoTick(S,now,{});CHECK(Q.sent.load()==s0+1,"prop hit geo: the same touch a tick later: no second hit");
       // apart 1 s (the cooldown passed), then carried into it at a run: the swing against the mover is 0
       body(.6f);now+=500*ms;D::propGeoTick(S,now,{});now+=600*ms;
       const auto ns0=Q.why[P::PropHitNotSwung].load();S.hand[1].moverVel={3.f,0,0};body(.13f);armAt({2.f,0,0},now);D::propGeoTick(S,now,{});
       CHECK(Q.sent.load()==s0+1&&Q.why[P::PropHitNotSwung].load()==ns0+1,"prop hit geo: a chair carried at a run into a creature: no hit (notSwung)");
       S.hand[1].moverVel=mv0;
       // apart again; a creature running into the chair held still: the approach is its own
       body(.6f);now+=500*ms;D::propGeoTick(S,now,{});now+=600*ms;
       {vel(0.f);body(.13f);S.vb.b[0].vel={-3.f,0,0};const auto sl0=Q.why[P::PropHitSlow].load()+Q.why[P::PropHitNotSwung].load();armAt({2.f,0,0},now);D::propGeoTick(S,now,{});
        CHECK(Q.sent.load()==s0+1&&Q.why[P::PropHitSlow].load()+Q.why[P::PropHitNotSwung].load()==sl0+1,"prop hit geo: a creature running into a still chair: no hit");}
       // apart; the chair moved by a turn of the player with the arms still (no arm speed): no hit
       body(.6f);now+=500*ms;D::propGeoTick(S,now,{});now+=600*ms;
       {vel(3.f);body(.13f);armAt({0,0,0},now);const auto na0=Q.why[P::PropHitNoArm].load();D::propGeoTick(S,now,{});
        CHECK(Q.sent.load()==s0+1&&Q.why[P::PropHitNoArm].load()==na0+1,"prop hit geo: the chair moving without the arms swinging: no hit (noArm)");}
       // apart; the chair lifted fast (arms 2 m/s up) while a turn carries it into the creature: the arms move across the normal: no hit
       body(.6f);now+=500*ms;D::propGeoTick(S,now,{});now+=600*ms;
       {vel(3.f);body(.13f);armAt({0,2.f,0},now);const auto na0=Q.why[P::PropHitNoArm].load(),nd0=Q.noArmDir.load();D::propGeoTick(S,now,{});
        CHECK(Q.sent.load()==s0+1&&Q.why[P::PropHitNoArm].load()==na0+1&&Q.noArmDir.load()==nd0+1,"prop hit geo: a lift during a turn into a creature: no hit (noArm, along the normal)");}
       // apart; the arms swung 300 ms before the chair reached the creature (the chair trails the hands), still since: a hit
       body(.6f);now+=500*ms;D::propGeoTick(S,now,{});now+=900*ms;
       {vel(3.f);body(.13f);armAt({2.f,0,0},now-300*ms);S.armTrail[1].push({0,0,0},now);const auto sent0=Q.sent.load();D::propGeoTick(S,now,{});
        bool ring=false;for(const auto& x:S.geoRing)ring=ring||(x.set&&x.why==P::PropHitOk&&near(x.armDir,2.f)&&near(x.armAgeMs,300.f,1.f));
        CHECK(Q.sent.load()==sent0+1&&ring,"prop hit geo: the swing's arm peak 300 ms before the touch: a hit (the ring: armDir 2, its age 300 ms)");}
       // apart; a swing whose arm peak is 500 ms old: no hit
       body(.6f);now+=500*ms;D::propGeoTick(S,now,{});now+=900*ms;
       {vel(3.f);body(.13f);armAt({2.f,0,0},now-500*ms);const auto na0=Q.why[P::PropHitNoArm].load(),sent0=Q.sent.load();D::propGeoTick(S,now,{});
        CHECK(Q.sent.load()==sent0&&Q.why[P::PropHitNoArm].load()==na0+1,"prop hit geo: the arms' swing 500 ms before the touch: no hit");}
       // a creature whose second capsule is 90 cm above the chair: that capsule is skipped before the distance search
       body(.6f);now+=500*ms;D::propGeoTick(S,now,{});now+=900*ms;
       {const auto cr0=Q.capRejects.load(),ct0=Q.capTests.load();
        P::VBodies vb{};vb.ns=now;vb.count=1;auto& b=vb.b[0];b.actor=stub::addr(stub::npcActor);b.guid=stub::npcGuid;b.race=0;b.count=2;
        b.c[0]={{io[0]+.13f,io[1]-.4f,io[2]},{io[0]+.13f,io[1]+.4f,io[2]},.06f,2,2};b.c[1]={{io[0]+.13f,io[1]+1.3f,io[2]},{io[0]+.13f,io[1]+1.5f,io[2]},.06f,2,2};
        P::finishBody(b);S.vb=vb;S.vbFresh=true;vel(0.f);armAt({0,0,0},now);D::propGeoTick(S,now,{});
        CHECK(Q.capRejects.load()>cr0&&Q.capTests.load()>ct0,"prop hit geo: a capsule far from the part skipped, the near one tested");}
       // apart; a swing 30 cm short of the creature: nothing judged
       body(.6f);now+=500*ms;D::propGeoTick(S,now,{});now+=600*ms;
       {vel(3.f);armAt({2.f,0,0},now);const auto t0=Q.touches.load();body(.45f);D::propGeoTick(S,now,{});
        CHECK(Q.touches.load()==t0&&Q.sent.load()==s0+2,"prop hit geo: a swing 30 cm short: no touch");}
       // an open hand has no path: no held prop, nothing armed
       {const auto a0=Q.armedTicks.load();const bool act=g.active;g.active=false;body(.13f);D::propGeoTick(S,now+600*ms,{});g.active=act;
        CHECK(Q.armedTicks.load()==a0&&Q.sent.load()==s0+2,"prop hit geo: no held prop (an open hand): nothing scanned, no hit");}
       // THROW: the same prop in flight (no hand on it). A light one (1 kg) at 6 m/s into a chest capsule: the slap's tap in the thrown slot
       // (thrown, at most throwTapMps, never the player as the attacker), the hand's own touch slot untouched, no strike; a 59.5 kg one: the
       // player's strike (thrown)
       {const bool act=g.active;g.active=false;auto& T=P::stats.throws;
        const auto apart=[&]{body(.6f);now+=500*ms;D::flightGeoTick(S,now,{});now+=1500*ms;};
        const auto flyKg=[&](float kg){for(unsigned i=0;i<2;++i)D::endFlight(S,i,P::FlightMode,now);
            D::startFlight(S,g.prop,S.sim,g.levelIndex,col,kg,.2f,1,2,6.f,now);};
        P::TouchRequest h0{};P::touchRequest[1].load(h0);P::TouchRequest t0r{};P::thrownTouchRequest.load(t0r);
        flyKg(1.f);apart();
        {const auto taps0=T.taps.load(),sent0=Q.sent.load();const auto hits0=T.hits.load();
         vel(6.f);body(.13f);S.vb.b[0].c[0].kind=2;P::finishBody(S.vb.b[0]);D::flightGeoTick(S,now,{});
         P::TouchRequest r{};P::thrownTouchRequest.load(r);P::TouchRequest h1{};P::touchRequest[1].load(h1);
         CHECK(S.flights[0].on||S.flights[1].on,"throw flight: a 1 kg flight started on the prop");
         CHECK(T.taps.load()==taps0+1&&r.serial!=t0r.serial&&r.thrown==1&&r.kind==P::TouchSlap&&near(r.a,P::throwTapMps)&&near(r.speed,P::throwTapMps)&&r.hand==1&&
               r.attacker!=0xffu&&r.guid==stub::npcGuid&&h1.serial==h0.serial&&T.hits.load()==hits0&&Q.sent.load()==sent0,
               "throw flight: a 1 kg prop at 6 m/s into a chest: one tap in the thrown slot (thrown, a = throwTapMps), the hand's slot untouched, no strike");
         if(!(T.taps.load()==taps0+1&&r.thrown==1))std::printf("     taps %llu->%llu refused %llu a %.2f thrown %u\n",(unsigned long long)taps0,(unsigned long long)T.taps.load(),
             (unsigned long long)T.tapRefused.load(),double(r.a),unsigned(r.thrown));}
        flyKg(59.5f);apart();
        {const auto hits0=T.hits.load();const auto l0=P::propHitLatest.load();vel(4.f);body(.13f);D::flightGeoTick(S,now,{});const auto r=newest();
         CHECK(T.hits.load()==hits0+1&&P::propHitLatest.load()!=l0&&r.thrown==1&&r.guid==stub::npcGuid&&near(r.kg,59.5f),"throw flight: a 59.5 kg prop at 4 m/s: the player's strike (thrown)");}
        for(unsigned i=0;i<2;++i)D::endFlight(S,i,P::FlightMode,now);g.active=act;}
       vel(0.f);g.propKg=kg0;g.cls=cls0;S.lev=lv0;S.vb=vb0;S.vbFresh=vf0;S.geoGate=P::PropGeoGate{};S.propPrev={};S.armTrail={};}
      // two hands on the ball-jointed prop: one pair plan per substep (both read it), half the mass per spring, the grip spacing kept; one
      // hand lets go: the pair ends at once
      {const auto g0=S.grab[0];const auto t0h=S.tg.hand[0];S.grab[0]=g;S.grab[0].gripLocal=V::add(g.gripLocal,{0,.3f,0});S.tg.hand[0]=S.tg.hand[1];
       const auto j0=G.pairJoins.load(),e0=G.pairEnds.load();++S.substep;const auto now=rdrvr::vr::nowNs();
       const auto* pc=D::pairStep(S,now,1.f/72.f,P::V3{0,0,0});
       CHECK(pc&&G.pairJoins.load()==j0+1&&near(pc->lim[0].springKg,.5f*g.propKg)&&near(pc->lim[1].springKg,.5f*g.propKg)&&V::finite(pc->plan.target[0])&&
             near(pc->plan.d,.3f,1e-4f)&&D::pairStep(S,now,1.f/72.f,P::V3{0,0,0})==pc,"two hands: a pair plan per substep, half the mass per hand, the grips' spacing");
       {const float wc=std::min(g.propKg,S.lev.grabLift2Kg)*9.81f;
        CHECK(pc->statics&&near(pc->carry.carry[0][1]+pc->carry.carry[1][1],wc,1e-2f)&&!pc->guard,"two hands: the statics carry (lever grabpairstatics): the weight split by the grips");
        S.playerContactStep=S.substep;S.playerContactN={0,0,1};++S.substep;const auto* pg=D::pairStep(S,now,1.f/72.f,P::V3{0,0,0});
        CHECK(pg&&pg->guard&&nearV(pg->guardN,{0,0,1}),"two hands: the player's capsule met this substep: the guard on");
        S.substep+=3;pg=D::pairStep(S,now,1.f/72.f,P::V3{0,0,0});CHECK(pg&&!pg->guard,"two hands: 3 substeps later: the guard off");S.playerContactStep=0;}
       S.grab[0].active=false;++S.substep;
       CHECK(!D::pairStep(S,now,1.f/72.f,P::V3{0,0,0})&&G.pairEnds.load()==e0+1,"two hands: one hand lets go: the pair ends, the other drives alone");
       {const auto c0=S.substep;const auto ps0=S.preSeen;S.preSeen=0;
        D::noteSubstep(S,1);const auto a1=S.substep;D::noteSubstep(S,0);const auto a0=S.substep;D::noteSubstep(S,1);const auto b1=S.substep;D::noteSubstep(S,0);const auto b0=S.substep;
        D::noteSubstep(S,0);const auto c0b=S.substep;D::noteSubstep(S,0);const auto c0c=S.substep;
        CHECK(a1==c0&&a0==c0&&b1==c0+1&&b0==c0+1&&c0b==c0+2&&c0c==c0+3,
              "two hands: the substep clock with hand 1 first in the list (both hands share a substep's plan), a lone hand moves it every call");
        S.substep=c0+10;S.preSeen=ps0;}
       S.grab[0]=g0;S.tg.hand[0]=t0h;}
      const float rid[12]{1,0,0,0,0,1,0,0,0,0,1,0};std::memcpy(reinterpret_cast<void*>(col+0xd0),rid,48);
      g=saved;for(int i=0;i<2;++i)frame({.5f,1.2f,.3f});}
     // grip dips keep it; an open hand 350 ms releases it, exactly once, the mass back
     const int r0=stub::releases;
     squeezeNow=.2f;for(int i=0;i<5;++i)frame({.5f,1.2f,.3f});squeezeNow=.6f;frame({.5f,1.2f,.3f});
     CHECK(D::state().grab[1].active&&stub::releases==r0,"grab: a 0.14 s grip dip keeps the hold");
     squeezeNow=.1f;for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});
     CHECK(!D::state().grab[1].active&&!GL::holding(1)&&stub::releases==r0+2&&near(mass(),2.f)&&G.ends[P::GrabEndOpen].load()>=1,"grab: opened 350 ms: both constraints released once, the mass back");
     for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});CHECK(stub::releases==r0+2,"grab: no second release");
     {P::State::LightHold r{};for(const auto& x:D::state().lightHolds)if(x.set)r=x;
      CHECK(r.set&&r.weld==2&&r.flagsPos==2&&r.flagsRot==3&&r.slipCm>=0.f&&r.offTrace[0]>=0.f,"weld: the light hold ring keeps weld 2, flags 2 / 3, the slip and the offset trace");}
     // a second hand on a light prop the other hand holds with fixed rotation: refused (two fixed links would fight)
     {auto& S=D::state();squeezeNow=.5f;touch({.5f,1.2f,.3f},2);
      S.grab[0]=P::GrabHold{};S.grab[0].active=true;S.grab[0].prop=stub::addr(pp.inst);S.grab[0].rec.fixedRot=true;
      const auto p0=G.refused[P::GrabPair].load();const int a1=stub::attaches;D::startGrab(S,1,simNs);
      CHECK(!S.grab[1].active&&stub::attaches==a1&&G.refused[P::GrabPair].load()==p0+1,"two hands: a second hand on a fixed-rotation hold: refused (pair), nothing attached");
      S.grab[0]=P::GrabHold{};for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});}
     // JOIN: the other hand holds the prop on a ball joint (heavy): this hand becomes a candidate within the join distance with no contact
     // pair (no CollideObjects call), not beyond it; a press then joins it
     {auto& S=D::state();squeezeNow=0.f;for(int i=0;i<8;++i)frame({.5f,1.2f,.3f}); // no kept pair left
      const auto& hs=S.hand[1];const float u=std::max(S.upm,1e-3f);
      const auto setOther=[&]{S.grab[0]=P::GrabHold{};S.grab[0].active=true;S.grab[0].prop=stub::addr(pp.inst);S.grab[0].sim=S.sim;S.grab[0].cls=P::classNav;
          S.grab[0].ball=true;GL::setHoldKind(0,GL::HoldHeavy);};
      const auto distAt=[&](P::V3 off){place(V::add(solvedRight().solved,off));return D::nearestPartOf(S,1,stub::addr(pp.inst),P::grabPoints(hs.solvedPos,hs.solvedRows,u)).dist/u;};
      setOther();
      // a spot 4..10 cm from the part: no plain candidate (grabcm 4), a join
      float d=-1.f;P::V3 off{};for(float x=.08f;x<.30f&&!(d>.045f&&d<.095f);x+=.005f){off={0,-x,0};d=distAt(off);}
      const auto jr0=G.joinReadyTicks.load();D::grabCandidate(S,1,simNs);
      CHECK(d>.045f&&d<.095f&&S.grabCand[1]==stub::addr(pp.inst)&&GL::joinAt(1,simNs)&&G.joinReadyTicks.load()==jr0+1,
            "join: 4.5..9.5 cm from the chair the other hand holds, no contact: this hand's candidate, join stamped");
      GL::setJoin(1,false,simNs,0.f);
      float d2=-1.f;for(float x=.15f;x<.6f&&!(d2>.15f);x+=.01f){off={0,-x,0};d2=distAt(off);}
      D::grabCandidate(S,1,simNs);
      CHECK(d2>.15f&&S.grabCand[1]==0&&!GL::joinAt(1,simNs),"join: 15 cm and more away: no candidate");
      CHECK(std::abs(GL::heldMmAt(1,simNs)-int(d2*1000.f))<=1&&GL::heavyOther(1,simNs)==(d2<=GL::heavyNearM.load()),
            "heavy mask: the tick publishes this hand's distance to the held chair; the mask follows it");
      {float d3=-1.f;for(float x=.3f;x<1.2f&&!(d3>.40f);x+=.02f){off={0,-x,0};d3=distAt(off);}
       D::grabCandidate(S,1,simNs);
       CHECK(d3>.40f&&!GL::heavyOther(1,simNs)&&GL::heldMmAt(1,simNs)>400,"heavy mask: over 40 cm from the held chair: published, no mask (holsters draw)");
       if(!(d3>.40f&&GL::heldMmAt(1,simNs)>400))std::printf("     d3 %.3f heldMm %d\n",double(d3),int(GL::heldMmAt(1,simNs)));}
      // the latch: the holsters' join branch presses, the tick grabs the same prop (the pair check passes for a ball joint)
      for(float x=.08f;x<.30f;x+=.005f){off={0,-x,0};d=distAt(off);if(d>.045f&&d<.095f)break;}
      stub::put<float>(stub::colliders[9],0xac,59.5f); // a chair's mass: a ball-jointed hold (over grabRotKg)
      setOther();const int a0=stub::attaches;const auto js0=G.joinStarts.load();
      D::grabCandidate(S,1,simNs); // the hand tick of hand 1 alone (the stub's hand 0 holds nothing: its own tick would end the fake hold)
      const bool j=GL::joinAt(1,simNs);if(j){GL::noteLatch(1,GL::LatchJoin,.8f,simNs);GL::press(1,simNs);}
      D::grabTick(S,1,simNs);
      CHECK(j&&S.grab[1].active&&S.grab[1].prop==stub::addr(pp.inst)&&stub::attaches==a0+1&&G.joinStarts.load()==js0+1&&GL::heldKind(1)!=GL::HoldNone,
            "join: the join latch presses, the free hand holds the chair the other hand holds (joined)");
      squeezeNow=.1f;S.grab[0]=P::GrabHold{};GL::setHoldKind(0,GL::HoldNone);for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});
      CHECK(!S.grab[1].active&&GL::heldKind(1)==GL::HoldNone,"join: (released, the hold kind cleared)");
      for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});place(V::add(solvedRight().solved,{.03f,0,0}));} // the light prop at the palm again
     // a throw: an open hand moving fast releases at once
     squeezeNow=.5f;touch({.5f,1.2f,.3f},2);GL::press(1,simNs);touch({.5f,1.2f,.3f},1);
     {const int r1=stub::releases;CHECK(D::state().grab[1].active,"grab: held again");
      squeezeNow=.1f;handVel={2.f,0,0};frame({.5f,1.2f,.3f});handVel={0,0,0};
      CHECK(!D::state().grab[1].active&&stub::releases==r1+2&&G.ends[P::GrabEndThrow].load()>=1,"grab: an open hand at 2 m/s: thrown at once (both constraints released)");}
     // THROW: a one-hand nav prop of propHitKg or more (14 kg, not ball-jointed: no heavy hold) let go fast by grabStep flies (a strike in
     // flight) only on the throw rule: a fast downward drop is refused (no flight), a forward throw at 3 m/s flies as a heavy flight
     {auto& S=D::state();auto& T=P::stats.throws;const auto ends=[&]{for(unsigned i=0;i<2;++i)D::endFlight(S,i,P::FlightMode,simNs);};
      const auto holdMid=[&]{for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});place(V::add(solvedRight().solved,{.03f,0,0}));
          squeezeNow=.5f;touch({.5f,1.2f,.3f},2);GL::press(1,simNs);touch({.5f,1.2f,.3f},1);
          S.grab[1].cls=P::classNav;S.grab[1].propKg=14.f;S.grab[1].ball=false;return S.grab[1].active;};
      CHECK(S.lev.throwMode==2&&S.lev.propHitKg<=14.f,"throw mid: lever throw 2 by default, propHitKg under 14 kg");
      CHECK(holdMid(),"throw mid: a 14 kg nav prop held in one hand");
      {const auto fl0=T.flights.load(),mr0=T.midGrabRefused.load(),mt0=T.midGrabThrows.load(),lg0=T.lightGrabThrows.load();
       handVel={0,-3.f,0};frame({.5f,1.2f,.3f});squeezeNow=.1f;frame({.5f,1.2f,.3f});handVel={0,0,0};
       const auto& d=S.throwDecides[(S.throwDecideNext+S.throwDecides.size()-1u)%S.throwDecides.size()];
       CHECK(!S.grab[1].active&&T.flights.load()==fl0&&T.midGrabRefused.load()==mr0+1&&T.midGrabThrows.load()==mt0&&T.lightGrabThrows.load()==lg0&&
             d.grabStep==1&&d.verdict==P::ThrowDown&&!S.flights[0].on&&!S.flights[1].on,
             "throw mid: a 14 kg prop dropped at 3 m/s downward: released by grabStep, refused (down), no flight, no strike");}
      CHECK(holdMid(),"throw mid: held again");
      {const auto fl0=T.flights.load(),mt0=T.midGrabThrows.load();
       handVel={3.f,0,0};frame({.5f,1.2f,.3f});squeezeNow=.1f;frame({.5f,1.2f,.3f});handVel={0,0,0};
       const auto& d=S.throwDecides[(S.throwDecideNext+S.throwDecides.size()-1u)%S.throwDecides.size()];
       const bool heavyFlight=(S.flights[0].on&&S.flights[0].heavy)||(S.flights[1].on&&S.flights[1].heavy);
       CHECK(!S.grab[1].active&&T.flights.load()==fl0+1&&T.midGrabThrows.load()==mt0+1&&d.grabStep==1&&d.verdict==P::ThrowGo&&heavyFlight,
             "throw mid: a 14 kg prop thrown forward at 3 m/s: the throw rule passes, a heavy flight");
       ends();}
      // the lever throw 1 (MEASURE): grabStep's release only, no flight whatever the prop
      CHECK(holdMid(),"throw mid: held (measure)");
      {S.lev.throwMode=1;const auto fl0=T.flights.load();handVel={3.f,0,0};frame({.5f,1.2f,.3f});squeezeNow=.1f;frame({.5f,1.2f,.3f});handVel={0,0,0};
       CHECK(!S.grab[1].active&&T.flights.load()==fl0,"throw mid: lever throw 1: no flight");S.lev.throwMode=2;ends();}
      squeezeNow=0.f;}
     // the break: a short stretch keeps the hold (no park of the held hand); the body held back over 60 cm for 250 ms breaks it
     for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});place(V::add(solvedRight().solved,{.03f,0,0}));
     squeezeNow=.5f;touch({.5f,1.2f,.3f},2);GL::press(1,simNs);touch({.5f,1.2f,.3f},1);
     {const int r1=stub::releases;for(int i=0;i<4;++i)frame({.5f,1.2f,.75f});
      CHECK(D::state().grab[1].active&&D::state().hand[1].machine.state==P::BodyLive,"grab: a 45 cm pull: still held, the hand not parked");
      for(int i=0;i<16;++i)frame({.5f,1.2f,3.5f});
      CHECK(!D::state().grab[1].active&&stub::releases==r1+2&&G.ends[P::GrabEndLeash].load()>=1,"grab: over 60 cm for 250 ms: a break, both released once");
      CHECK(D::state().holdRecords[(D::state().holdNext+7u)%8u].set&&D::state().holdRecords[(D::state().holdNext+7u)%8u].end==P::GrabEndLeash,"grab: the hold's record (end break)");}
     for(int i=0;i<20;++i)frame({.5f,1.2f,.3f});
     // the constraint no longer ours (the engine gave it to another pair): dropped without a release call
     place(V::add(solvedRight().solved,{.03f,0,0}));squeezeNow=.5f;touch({.5f,1.2f,.3f},2);GL::press(1,simNs);touch({.5f,1.2f,.3f},1);
     {const int r1=stub::releases;CHECK(D::state().grab[1].active,"grab: held (detach)");
      stub::put<std::uintptr_t>(reinterpret_cast<void*>(D::state().grab[1].con),0x8,0x1234);frame({.5f,1.2f,.3f});
      CHECK(!D::state().grab[1].active&&stub::releases==r1+1&&G.rotReleaseCalls.load()>=1&&G.ends[P::GrabEndDetached].load()>=1&&near(mass(),2.f),
            "grab: a position constraint that is not ours: no release call for it, the rotation constraint (still ours) released once, the mass back");
      stub::put<std::uintptr_t>(reinterpret_cast<void*>(stub::constraints),0x8,0);std::memset(stub::constraints,0,sizeof stub::constraints);}
     // the held prop deleted: phContactMgr::DeleteObject hands the manifold back (+0x18 = 0) and takes the constraint off the active list,
     // the instance words stay; only the owner's ReleaseConstraint puts it back in the pool: exactly one call, the mass back
     touch({.5f,1.2f,.3f},2);GL::press(1,simNs);touch({.5f,1.2f,.3f},1);
     {const int r1=stub::releases;CHECK(D::state().grab[1].active,"grab: held (prop deleted)");
      stub::put<std::uintptr_t>(reinterpret_cast<void*>(D::state().grab[1].con),0x18,0);frame({.5f,1.2f,.3f});
      CHECK(!D::state().grab[1].active&&!GL::holding(1)&&stub::releases==r1+2&&near(mass(),2.f),"grab: the held prop deleted (no manifold): one release call each, the mass back");
      for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});CHECK(stub::releases==r1+2,"grab: the deleted prop's constraints are released once");
      std::memset(stub::constraints,0,sizeof stub::constraints);}
     // a mode change while held: released before the body leaves
     touch({.5f,1.2f,.3f},2);GL::press(1,simNs);touch({.5f,1.2f,.3f},1);
     {const int r1=stub::releases;const int d0=stub::deletes;CHECK(D::state().grab[1].active,"grab: held (mode)");
      P::modeForTests.store(P::ModeMeasure);frame({.5f,1.2f,.3f});
      CHECK(!D::state().grab[1].active&&!GL::holding(1)&&stub::releases==r1+2&&stub::deletes>d0&&G.ends[P::GrabEndMode].load()>=1,"grab: a mode change: both released once, then the body removed");
      P::modeForTests.store(P::ModeOn);for(int i=0;i<12;++i)frame({.5f,1.2f,.3f});}
     // GRAB WELD: the rotation constraint's own ends, a refused second attach, lever grabweld 0 and the WELD prints
     {const auto hold=[&]{std::memset(stub::constraints,0,sizeof stub::constraints);place(V::add(solvedRight().solved,{.03f,0,0}));squeezeNow=.5f;touch({.5f,1.2f,.3f},2);
          GL::press(1,simNs);touch({.5f,1.2f,.3f},1);return D::state().grab[1].active;};
      const auto open=[&]{squeezeNow=.1f;for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});for(int i=0;i<12;++i)frame({.5f,1.2f,.3f});};
      // the rotation constraint given to another pair: Detached; the position constraint released once, the rotation constraint not
      {CHECK(hold()&&D::state().grab[1].weld==2,"weld: held (rotation detach)");const int r1=stub::releases;const auto rd0=G.rotDetached.load(),rc0=G.rotReleaseCalls.load();
       stub::put<std::uintptr_t>(reinterpret_cast<void*>(D::state().grab[1].conRot),0x10,0x4321);frame({.5f,1.2f,.3f});
       CHECK(!D::state().grab[1].active&&stub::releases==r1+1&&G.rotReleaseCalls.load()==rc0&&G.rotDetached.load()==rd0+1&&G.ends[P::GrabEndDetached].load()>=1,
             "weld: a rotation constraint that is not ours: Detached, the position constraint released once, the rotation one not");
       open();}
      // the rotation constraint disabled (+0x141): Disabled, both released once
      {CHECK(hold(),"weld: held (rotation disabled)");const int r1=stub::releases;const auto e0=G.ends[P::GrabEndDisabled].load();
       stub::put<std::uint8_t>(reinterpret_cast<void*>(D::state().grab[1].conRot),0x141,1);frame({.5f,1.2f,.3f});
       CHECK(!D::state().grab[1].active&&stub::releases==r1+2&&G.ends[P::GrabEndDisabled].load()==e0+1,"weld: a disabled rotation constraint: Disabled, both released once");
       for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});CHECK(stub::releases==r1+2,"weld: no double release");open();}
      // the second attach refused: the hold stays on the position constraint alone (flags 2, no SetFixedRotation)
      {const auto rr0=G.rotRefused.load();const int f0=stub::fixedRotations;stub::failAttachAt=stub::attaches+2;
       CHECK(hold()&&D::state().grab[1].weld==1&&!D::state().grab[1].conRot&&G.rotRefused.load()==rr0+1&&stub::fixedRotations==f0&&
             stub::at<std::uint16_t>(reinterpret_cast<void*>(D::state().grab[1].con),0xbc)==2,
             "weld: the second attach refused: held on the position constraint (flags 2), no SetFixedRotation, rotRefused");
       stub::failAttachAt=-1;const int r1=stub::releases;open();CHECK(stub::releases==r1+1,"weld: (position only) one release");}
      // lever grabweld 0: one constraint with SetFixedRotation (the v16 calls)
      {P::leverPatchForTests.store([](P::Levers& l){l.grabWeld=false;});D::state().leversRead=false;frame({.5f,1.2f,.3f});
       const int a0=stub::attaches,f0=stub::fixedRotations;const auto ro0=G.rotOnly.load();
       CHECK(hold()&&stub::attaches==a0+1&&stub::fixedRotations==f0+1&&D::state().grab[1].weld==0&&!D::state().grab[1].conRot&&D::state().grab[1].flagsPos==3&&
             stub::lastFixedRotation==reinterpret_cast<void*>(D::state().grab[1].con)&&G.rotOnly.load()==ro0+1,
             "weld: lever grabweld 0: one AttachObjects, SetFixedRotation on it (flags 3), as v16");
       const int r1=stub::releases;open();CHECK(stub::releases==r1+1,"weld: lever 0: one release");
       P::leverPatchForTests.store(nullptr);D::state().leversRead=false;frame({.5f,1.2f,.3f});}
      // the WELD prints refused: the same one-constraint hold
      {P::groupOk[P::GroupWeld]=false;const int a0=stub::attaches;
       CHECK(hold()&&stub::attaches==a0+1&&D::state().grab[1].weld==0,"weld: WELD prints refused: one constraint with SetFixedRotation");
       open();P::groupOk[P::GroupWeld]=true;}
      // PLAYER SKIP on a welded light hold: the cup in hand 1's slot (light); its contacts with the player's capsule switched off and counted
      // on the hold; the released cup lingers; lever grabskiplight 0: not served
      {auto& S=D::state();CHECK(hold()&&S.grab[1].weld==2,"skip light: held (welded)");frame({.5f,1.2f,.3f});const auto cup=S.grab[1].prop;const auto& K=P::stats.playerSkip;
       CHECK(S.skip.prop[1]==cup&&S.skip.light[1]&&!S.skip.prop[0]&&K.lightHolds.load()>=1,"skip light: the welded cup served in hand 1's slot");
       const bool mo=S.moverOk;S.moverOk=true;CHECK(S.moverInstAddr!=0,"skip light: the stub's mover instance");
       {alignas(16) static unsigned char it[0x40]{},arrA[16]{},m0[0x600]{};const P::ImpactLayout Ly{};const auto A=[](const void* p){return reinterpret_cast<std::uintptr_t>(p);};
        std::memset(it,0,sizeof it);std::memset(m0,0,sizeof m0);
        stub::put<std::uintptr_t>(it,Ly.arrA,A(arrA));stub::put<std::uintptr_t>(it,Ly.arrB,0);stub::put<std::int32_t>(it,Ly.countA,1);stub::put<std::int32_t>(it,Ly.countB,0);
        stub::put<std::uintptr_t>(it,0x10,cup);stub::put<std::uintptr_t>(arrA,0,A(m0));
        stub::put<std::uintptr_t>(m0,Ly.instA,S.moverInstAddr);stub::put<std::uintptr_t>(m0,Ly.instB,cup);m0[Ly.contactCount]=1;
        m0[Ly.contacts+Ly.active]=1;stub::put<float>(m0,Ly.contacts+Ly.depth,.02f);stub::put<std::uintptr_t>(m0,Ly.contacts+Ly.owner,A(m0));
        const auto c0=K.lightContacts.load();const int f0=stub::pcForwards;D::preComputeGuard(it);
        CHECK(!m0[Ly.contacts+Ly.active]&&K.lightContacts.load()==c0+1&&S.grab[1].capContacts==1&&S.grab[1].capDepthMm>0.f&&stub::pcForwards==f0+1,
              "skip light: the cup's contact with the player's capsule switched off, counted on the hold, the iterator forwarded");}
       S.moverOk=mo;squeezeNow=.1f;for(int i=0;i<20&&S.grab[1].active;++i)frame({.5f,1.2f,.3f});frame({.5f,1.2f,.3f});
       const int lk=P::skipLingerSlot(S.skip,cup);
       CHECK(!S.grab[1].active&&!S.skip.prop[1]&&lk>=0&&S.skip.linger[unsigned(lk)].light,"skip light: the released cup lingers in its own slot");
       {const auto& r=S.lightHolds[(S.lightHoldNext+3u)%4u];CHECK(r.set&&r.prop==cup&&r.capContacts==1,"skip light: the light hold ring keeps the capsule contacts");}
       for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});
       P::leverPatchForTests.store([](P::Levers& l){l.grabSkipLight=false;});S.leversRead=false;frame({.5f,1.2f,.3f});
       CHECK(hold()&&S.grab[1].weld==2,"skip light: lever 0: held");frame({.5f,1.2f,.3f});
       CHECK(!S.skip.prop[1]&&!S.skip.prop[0],"skip light: lever grabskiplight 0: the welded cup not served (heavy holds only)");
       open();P::leverPatchForTests.store(nullptr);S.leversRead=false;frame({.5f,1.2f,.3f});}
      {const auto r=P::report();
       CHECK(jsonValid(r)&&r.find("\"weld\":2,\"flagsPos\":2,\"flagsRot\":3,\"slipCm\":")!=std::string::npos&&r.find("\"offTrace\":[")!=std::string::npos&&
             r.find("\"capsule\":[")!=std::string::npos&&r.find("\"light\":{\"lever\":1,\"holds\":")!=std::string::npos&&r.find("\"grabSkipLight\":1")!=std::string::npos&&
             r.find("\"weld\":{\"lever\":1,\"group\":true")!=std::string::npos&&r.find("\"grabWeld\":1,\"grabLightFf\":1")!=std::string::npos,
             "weld: the report (lightHolds weld / flags / slipCm / offTrace / capsule, cups.weld, playerSkip.light, the levers) parses");}
      squeezeNow=0.f;std::memset(stub::constraints,0,sizeof stub::constraints);}
     // AttachObjects refused: a buzz, no hold; lever grab 0: not ready
     place(V::add(solvedRight().solved,{.03f,0,0}));touch({.5f,1.2f,.3f},2);stub::failAttach=true;
     {const auto p0=G.refused[P::GrabPool].load();GL::press(1,simNs);touch({.5f,1.2f,.3f},1);
      CHECK(G.refused[P::GrabPool].load()==p0+1&&!D::state().grab[1].active&&!GL::holding(1),"grab: AttachObjects refused: no hold");}
     stub::failAttach=false;
     {D::state().lev.grab=false;touch({.5f,1.2f,.3f},1);CHECK(!GL::readyAt(1,simNs),"grab: lever grab 0: never ready");D::state().lev.grab=true;}
     // a stale press (over 100 ms old): ignored
     {const auto s0=G.stalePresses.load();GL::press(1,simNs-300*ms);touch({.5f,1.2f,.3f},1);
      CHECK(G.stalePresses.load()==s0+1&&!D::state().grab[1].active,"grab: a press over 100 ms old is ignored");}
     // a simulator reset (forgetAll): only the pointer goes
     place(V::add(solvedRight().solved,{.03f,0,0}));touch({.5f,1.2f,.3f},2);GL::press(1,simNs);touch({.5f,1.2f,.3f},1);
     {const int r1=stub::releases;CHECK(D::state().grab[1].active,"grab: held (reset)");
      D::forgetAll(D::state());CHECK(!D::state().grab[1].active&&!GL::holding(1)&&stub::releases==r1,"grab: a simulator reset: no release call into the old pool");
      std::memset(stub::constraints,0,sizeof stub::constraints);}
     squeezeNow=0.f;stub::reset();stub::bind();P::resetForTests();P::modeForTests.store(P::ModeOn);for(int i=0;i<12;++i)frame({.5f,1.2f,.3f});wallOn=true;}

    // ---------- CPU per mode ----------
    {static std::uint64_t cpuNow=0;P::cpuClockForTests.store(+[]()->std::uint64_t{return cpuNow+=3'000'000ull;});
     auto& M=P::stats.byMode[P::ModeOn];const auto n0=M.cpuN.load(),b0=M.cpuHist[6].load();
     for(int i=0;i<20;++i)frame({.5f,1.2f,.3f});
     CHECK(M.cpuN.load()>=n0+18&&M.cpuHist[6].load()>=b0+18&&near(float(double(M.cpuNsMax.load())/1e6),3.f,.01f),"cpu: 3 ms of main-thread CPU per 28 ms frame, bucket 3.0..3.5 ms");
     const auto s0=M.shortDrops.load(),f0=M.frames.load();simNs+=0;++simIndex;publish({.5f,1.2f,.3f},P::ClassBody);simNs+=2*ms;P::tick(MT::Frame{simNs,simIndex});substeps();
     CHECK(M.shortDrops.load()==s0+1&&M.frames.load()==f0,"cpu: a 2 ms tick (a loading screen) is dropped");
     const auto l0=M.longDrops.load();simNs+=200*ms;frame({.5f,1.2f,.3f});
     CHECK(M.longDrops.load()==l0+1,"cpu: a 200 ms stall is dropped");
     auto& Mm=P::stats.byMode[P::ModeMeasure];const auto m0=Mm.cpuN.load();P::modeForTests.store(P::ModeMeasure);frame({.5f,1.2f,.3f});
     CHECK(Mm.cpuN.load()==m0,"cpu: the first tick after a mode change is not counted");
     frame({.5f,1.2f,.3f});CHECK(Mm.cpuN.load()==m0+1,"cpu: then counted in the new mode");
     P::modeForTests.store(P::ModeOn);for(int i=0;i<12;++i)frame({.5f,1.2f,.3f});
     const auto r=P::report();CHECK(r.find("\"cpuMsP50\":3.25")!=std::string::npos&&r.find("\"cpuFrames\":")!=std::string::npos,"cpu: the report's median");
     P::cpuClockForTests.store(nullptr);}
    // ---------- cost accounting: every sum belongs to the window it was spent in ----------
    {auto& M=P::stats.byMode[P::ModeOn];for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});
     const auto f0=M.frames.load(),t0=M.ticks.load(),s0=M.spans.load(),i0=M.idleTicks.load();
     for(int i=0;i<20;++i)frame({.5f,1.2f,.3f});
     CHECK(M.frames.load()==f0+20&&M.ticks.load()==t0+20&&M.spans.load()==s0+40&&M.idleTicks.load()==i0,"cost: 20 frames, 20 ticks, 40 substeps counted (2 per frame)");
     // ten 2 ms ticks (a loading screen): their tick and substep costs go to idle, not to the frames
     const auto f1=M.frames.load(),t1=M.ticks.load(),s1=M.spans.load(),i1=M.idleTicks.load(),n1=M.idleNsSum.load();
     for(int i=0;i<10;++i){++simIndex;publish({.5f,1.2f,.3f},P::ClassBody);simNs+=2*ms;P::tick(MT::Frame{simNs,simIndex});substeps();}
     CHECK(M.frames.load()==f1&&M.ticks.load()==t1&&M.spans.load()==s1&&M.idleTicks.load()==i1+10&&M.idleNsSum.load()>n1,"cost: 2 ms ticks go to idle (not divided by frames)");
     for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});
     const auto r=P::report();
     CHECK(r.find("\"ticksPerFrame\":1.00")!=std::string::npos&&r.find("\"sectionsUsPerFrame\":{\"upkeep\":")!=std::string::npos&&r.find("\"filterUsPerFrame\":")!=std::string::npos,
           "cost: the report's ticks per frame (1.00), sections, the filter");
     // the mover is verified for a new mover and every 16 ticks; in between reused
     {const auto u0=P::stats.moverReuse.load();for(int i=0;i<16;++i)frame({.5f,1.2f,.3f});
      CHECK(P::stats.moverReuse.load()>=u0+14&&P::stats.moverReuse.load()<=u0+15&&D::state().moverOk,"cost: the mover verified once per 16 ticks");
      // the reuse reads the vtable word every tick: a changed one is refused on the next tick, not 16 ticks later
      {const auto b0=P::stats.moverBad.load();stub::put<std::uintptr_t>(stub::mover,0,0x1234);frame({.5f,1.2f,.3f});
       CHECK(!D::state().moverOk&&P::stats.moverBad.load()==b0+1,"cost: a mover whose vtable changed is refused at the next tick");}
      stub::put<std::uintptr_t>(stub::mover,0,stub::base+P::off::moverSimVtable);for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});
      // held (stale) targets: no reuse, the full check every tick
      {for(int i=0;i<4;++i)staleFrame(); // 112 ms: past the 100 ms of fresh targets, inside the 300 ms they are held
       const auto u1=P::stats.moverReuse.load();for(int i=0;i<2;++i)staleFrame();
       CHECK(P::stats.moverReuse.load()==u1&&D::state().moverOk,"cost: held targets: the mover checked in full, never reused");
       for(int i=0;i<4;++i)frame({.5f,1.2f,.3f});}
      stub::put<std::uintptr_t>(stub::mover,0,0x1234);frame({.5f,1.2f,.3f});frame({.5f,1.2f,.3f});
      bool bad=false;for(int i=0;i<16&&!bad;++i){frame({.5f,1.2f,.3f});bad=!D::state().moverOk;}
      CHECK(bad&&P::stats.moverBad.load()>=1,"cost: a mover whose vtable changed is refused at its next check");
      stub::put<std::uintptr_t>(stub::mover,0,stub::base+P::off::moverSimVtable);for(int i=0;i<20;++i)frame({.5f,1.2f,.3f});
      CHECK(D::state().moverOk,"cost: verified again");}
     // no player for a while (no targets, the bodies gone): the tick returns at once
     {for(int i=0;i<60;++i)staleFrame();
      const auto np=P::stats.noPlayerTicks.load();for(int i=0;i<5;++i)staleFrame();
      CHECK(P::stats.noPlayerTicks.load()==np+5&&!P::nearWanted.load()&&!D::state().hand[1].present,"cost: no player and nothing of ours: the tick returns at once");
      for(int i=0;i<12;++i)frame({.5f,1.2f,.3f});CHECK(D::state().hand[1].present,"cost: the player back: the body back");}}
    // ---------- groups ----------
    {stub::reset();stub::bind();P::resetForTests();P::groupOk[P::GroupPalm]=false;P::modeForTests.store(P::ModeOn);frame({.5f,1.2f,0});
     CHECK(D::state().builtShape==P::ShapeCapsule&&!stub::boxCtors,"PALM refused: the capsule");
     P::Levers l{};P::levers.load(l);CHECK(l.shape==P::ShapeCapsule,"hook A reads the shape the bodies have");
     stub::reset();stub::bind();P::resetForTests();P::groupOk[P::GroupDeact]=false;P::groupOk[P::GroupCcd]=false;P::modeForTests.store(P::ModeOn);frame({.5f,1.2f,0});
     CHECK(stub::at<std::uintptr_t>(P::objects[1].inst,0)==stub::base+P::off::instVtable+0x10&&!(stub::at<std::uint16_t>(P::objects[1].inst,0x12)&0x2)&&D::userOk(1),
           "DEACT and CCD refused: the stock vtable, no CCD flag");
     stub::reset();stub::bind();P::resetForTests();P::groupOk[P::GroupAct]=false;P::modeForTests.store(P::ModeOn);
     for(int i=0;i<12;++i)frame({.5f,1.2f,.3f});
     stub::placeProp(stub::props[0],8,stub::navVtable,V::add(solvedRight().solved,{.1f,0,0}),.04f,1);const int f=stub::forceActivations;
     for(int i=0;i<6;++i)frame({.5f,1.2f,.3f});CHECK(stub::forceActivations==f&&!stub::culls,"ACT refused: no wakes, no culls");stub::clearProps();}
    // ---------- removal, resets, refusals (the old paths) ----------
    stub::reset();stub::bind();P::resetForTests();P::modeForTests.store(P::ModeOn);
    for(int i=0;i<3&&!D::state().hand[1].present;++i)frame({.1f,1.3f,0});
    frame({.1f,1.3f,0},P::ClassHeld);CHECK(D::state().hand[1].machine.state==P::BodyGhost,"held item -> ghost");
    for(int i=0;i<40;++i)frame({.1f,1.3f,0},P::ClassHeld);
    CHECK(!D::state().hand[1].present&&stub::deletes>=1,"held over 1 s -> removed from the level");
    for(int i=0;i<3;++i)frame({.1f,1.3f,0});
    CHECK(D::state().hand[1].present,"empty again: inserted");
    {const auto idx=stub::at<std::uint16_t>(P::objects[1].inst,0x10);stub::put<std::uintptr_t>(stub::levelObjects,std::size_t(idx)*0x30+0x20,0);stub::levelColliders[idx]=0;
     const auto resets=P::stats.simResets.load();frame({.1f,1.3f,0});
     CHECK(P::stats.simResets.load()==resets+1,"an engine reset is noticed");
     frame({.1f,1.3f,0});CHECK(D::state().hand[1].present,"and the body comes back");}
    {const auto idx=stub::at<std::uint16_t>(P::objects[1].inst,0x10);const auto i0=P::stats.inactive.load();
     stub::put<std::uintptr_t>(stub::levelObjects,std::size_t(idx)*0x30+0x20,stub::addr(P::objects[1].inst)|1u);frame({.1f,1.3f,0});
     CHECK(P::stats.inactive.load()==i0+1&&P::stats.inactiveLevelActive.load()==40&&P::stats.inactiveLevelCap.load()==500,"a body deactivated anyway: counted with the level's active count and cap");
     for(int i=0;i<12&&!D::state().hand[1].present;++i)frame({.1f,1.3f,0});CHECK(D::state().hand[1].present,"and inserted again");}
    {P::modeForTests.store(P::ModeMeasure);frame({.1f,1.3f,0});CHECK(!D::state().hand[1].present,"MEASURE again: bodies removed");
     P::modeForTests.store(P::ModeOn);stub::failAdd=1;const auto f0=P::stats.hand[1].insertFails.load();frame({.1f,1.3f,0});
     CHECK(P::stats.hand[1].insertFails.load()==f0+1&&!D::state().hand[1].present,"an insert refused by the engine is counted");
     for(int i=0;i<12&&!D::state().hand[1].present;++i)frame({.1f,1.3f,0});CHECK(D::state().hand[1].present,"and retried after 250 ms");}
    {stub::delayDelete=true;for(int i=0;i<40;++i)frame({.1f,1.3f,0},P::ClassHeld);
     const auto idx=stub::at<std::uint16_t>(P::objects[1].inst,0x10);
     CHECK(!D::state().hand[1].present&&D::state().hand[1].pendingIndex==idx&&stub::behSlots[idx]==stub::addr(&P::objects[1].beh),"delayed delete: the slot stays");
     CHECK(!D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,stub::otherProp.inst,nullptr,nullptr),"and its pairs are dropped");
     stub::delayDelete=false;stub::put<std::uintptr_t>(stub::levelObjects,std::size_t(idx)*0x30+0x20,0);stub::levelColliders[idx]=0;stub::put<std::uint16_t>(P::objects[1].inst,0x10,0xffff);
     frame({.1f,1.3f,0});CHECK(D::state().hand[1].pendingIndex==0xffff&&D::state().hand[1].present,"the level let it go: pending cleared, the body back");}
    P::modeForTests.store(P::ModeOff);frame({.1f,1.3f,0});
    CHECK(stub::count()==2&&!D::state().hand[1].present&&stub::behList[0]==0x1111&&stub::behList[1]==0x2222,"OFF: bodies removed, the list as it was");
    // ---------- heavy hold: the break, the pair's drive mass, the feed-forward, the player skip walk (pure) ----------
    {const std::uint64_t ms=1'000'000ull,t0=5'000'000'000ull;
     P::GrabHoldGate g{};bool kept=true;
     for(int i=0;i<=40;++i)kept=kept&&P::grabStep(g,1.f,0.f,0.f,1.9f,.6f,350.f,t0+std::uint64_t(i)*28*ms,250*ms,true,2.f)==P::GrabKeep;
     CHECK(kept,"heavy hold: 1.9 m behind the hands for 1.1 s (a swing, a walk): no break under 2 m");
     P::GrabHoldGate g2{};CHECK(P::grabStep(g2,1.f,0.f,0.f,2.1f,.6f,350.f,t0,250*ms,true,2.f)==P::GrabKeep&&
           P::grabStep(g2,1.f,0.f,0.f,2.1f,.6f,350.f,t0+260*ms,250*ms,true,2.f)==P::GrabSafety,"heavy hold: over 2 m for 250 ms: the safety break");
     P::GrabHoldGate g3{};CHECK(P::grabStep(g3,1.f,0.f,0.f,.9f,.6f,350.f,t0,250*ms,true,0.f)==P::GrabKeep&&
           P::grabStep(g3,1.f,0.f,0.f,.9f,.6f,350.f,t0+260*ms,250*ms,true,0.f)==P::GrabLeash,"heavy hold with lever grabheavyleash: the 60 cm leash as before");
     P::GrabHoldGate g4{};CHECK(P::grabStep(g4,.9f,0.f,0.f,.9f,.6f,350.f,t0,250*ms,false,2.f)==P::GrabKeep&&
           P::grabStep(g4,.9f,0.f,0.f,.9f,.6f,350.f,t0+260*ms,250*ms,false,2.f)==P::GrabLeash,"a light hold keeps its 60 cm leash");
     CHECK(near(P::pairDriveKg(59.5f,15.f),15.f)&&near(P::pairDriveKg(10.f,15.f),10.f)&&near(P::pairDriveKg(59.5f,0.f),59.5f)&&P::pairDriveKg(0.f,15.f)==0.f,
           "pair drive mass: the lighter of the prop and grabpairkg (0 = the prop's own)");
     const auto L=P::pairLimits(15.f,.5f,70.f,900.f,80.f,.5f,.5f,80.f);
     CHECK(near(L.sideMaxN,600.f,1e-2f)&&near(L.upSpringN,600.f,1e-2f)&&near(L.downMaxN,600.f,1e-2f)&&near(L.forceMaxN,900.f),"pair limits at 15 kg, 80 m/s^2: side / spring up / down 600 N, total 900 N");
     const auto L0=P::pairLimits(59.5f,.5f,70.f,900.f,20.f,.5f,.5f);
     CHECK(near(L0.downMaxN,.5f*.5f*59.5f*9.81f,1e-2f)&&near(L0.sideMaxN,595.f,1e-2f),"pair limits at the prop's own mass: the v11 caps (down 146 N, side 595 N)");
     CHECK(near(P::pairSlewN(15.f,1.f,8.f,80.f,.0139f,40.f),.5f*15.f*2.f*8.f*80.f*.0139f,1e-2f),"pair slew at 15 kg, 80 m/s^2: 133 N per substep");
     // the statics' clamp at 15 kg: never at a 24 cm grip line with the centre of mass 20 cm along it
     const P::V3 up{0,1,0};const auto cc=P::pairCarry({.32f,1.f,0},{0,1.f,0},{.24f,1.f,0},up,15.f,70.f,900.f,1.f);
     CHECK(!cc.clamped&&cc.coupleN<900.f-.5f*cc.wN,"statics at 15 kg: the couple stays inside the hand's cap");
     carryTests();knockTests();throwTests();
     // the command acceleration: a hand from 0 to 4 m/s over 50 ms (substeps of 13.9 ms)
     {P::CmdAccel ca{};P::V3 a{};float t=0.f;for(int i=0;i<12;++i){const float vel=std::min(t/.05f,1.f)*4.f;a=P::cmdAccelStep(ca,{vel,0,0},.0139f,.03f,80.f,1.f);t+=.0139f;}
      P::CmdAccel cb{};P::V3 big{};for(int i=0;i<10;++i)big=P::cmdAccelStep(cb,{float(i)*10.f,0,0},.0139f,.03f,80.f,1.f);
      P::CmdAccel cc2{};const P::V3 z=P::cmdAccelStep(cc2,{1,0,0},.0139f,.03f,80.f,1.f);
      CHECK(V::length(big)<=80.001f&&big[0]>60.f&&V::length(z)==0.f&&a[0]>=0.f&&a[0]<80.f,"command acceleration: averaged, capped at 80 m/s^2, 0 on the first sample");
      const P::V3 ff=P::pairFeedFwdN(15.f,{80.f,0,0},1.f);CHECK(near(ff[0],600.f,1e-2f),"feed-forward: 0.5 M a = 600 N per hand at 15 kg, 80 m/s^2");
      // the feed-forward stays inside the caps: a 600 N push along x on top of a lag across x
      const P::V3 carry{0,.5f*15.f*9.81f,0};
      const auto f=P::grabGripForcePair({.1f,1.f,0},{0,1.f,0},{0,0,0},{0,0,0},L,up,8.f,1.f,{0,0,0},1e9f,1.f,1.f,&carry,nullptr,nullptr,ff);
      CHECK(f.capped&&V::length(V::sub(f.fN,carry))<=600.01f&&V::length(f.fN)<=900.01f,"feed-forward: added before the caps (side 600 N, total 900 N)");
      const auto f0=P::grabGripForcePair({.1f,1.f,0},{0,1.f,0},{0,0,0},{0,0,0},L,up,8.f,1.f,{0,0,0},1e9f,1.f,1.f,&carry);
      CHECK(near(f0.fN[0],7.5f*64.f*.1f,1e-2f),"no feed-forward: the spring alone (0.5 M wn^2 e)");}
     // the guard trims only the position part: a prop moving into the player keeps its braking
     {const P::V3 carry{0,.5f*15.f*9.81f,0};const P::V3 gN{0,0,-1};float trim=0.f;
      const auto f=P::grabGripForcePair({0,1.f,-.05f},{0,1.f,0},{0,0,0},{0,0,1.f},L,up,8.f,1.f,{0,0,0},1e9f,1.f,1.f,&carry,&gN,&trim);
      CHECK(near(trim,7.5f*64.f*.05f,1e-2f)&&near(f.fN[2],-7.5f*16.f*1.f,1e-2f),"guard: the position part into the player trimmed, the damping (braking) kept");}
     // the player skip walk on a fake manifold layout: two manifolds, one with the player (a sub-manifold too), one with an NPC
     {alignas(16) static unsigned char it[0x40]{},arrA[16]{},m0[0x600]{},m1[0x600]{},sub[0x600]{},prop[16]{},player[16]{},npc[16]{};
      const P::ImpactLayout Ly{};const auto A=[](const void* p){return reinterpret_cast<std::uintptr_t>(p);};
      std::memset(m0,0,sizeof m0);std::memset(m1,0,sizeof m1);std::memset(sub,0,sizeof sub);
      stub::put<std::uintptr_t>(it,Ly.arrA,A(arrA));stub::put<std::uintptr_t>(it,Ly.arrB,0);stub::put<std::int32_t>(it,Ly.countA,2);stub::put<std::int32_t>(it,Ly.countB,0);
      stub::put<std::uintptr_t>(it,0x10,A(prop));stub::put<std::uintptr_t>(arrA,0,A(m0));stub::put<std::uintptr_t>(arrA,8,A(m1));
      const auto contact=[&](unsigned char* m,unsigned j,unsigned char* owner,float depth){const std::size_t c=Ly.contacts+j*Ly.stride;
        m[c+Ly.active]=1;stub::put<float>(m,c+Ly.depth,depth);stub::put<std::uintptr_t>(m,c+Ly.owner,A(owner));};
      stub::put<std::uintptr_t>(m0,Ly.instA,A(player));stub::put<std::uintptr_t>(m0,Ly.instB,A(prop));m0[Ly.subCount]=2;m0[Ly.contactCount]=2;
      contact(m0,0,m0,.03f);contact(m0,1,m0,.01f);
      stub::put<std::uintptr_t>(m0,Ly.subs+8,A(sub));stub::put<std::uintptr_t>(sub,Ly.instA,A(player));stub::put<std::uintptr_t>(sub,Ly.instB,A(prop));sub[Ly.contactCount]=1;contact(sub,0,sub,.05f);
      stub::put<std::uintptr_t>(m1,Ly.instA,A(prop));stub::put<std::uintptr_t>(m1,Ly.instB,A(npc));m1[Ly.contactCount]=1;contact(m1,0,m1,.02f);
      const auto rd=[](std::uintptr_t a,void* o,std::size_t n){std::memcpy(o,reinterpret_cast<const void*>(a),n);};
      const auto wr=[](std::uintptr_t a,const void* i,std::size_t n){std::memcpy(reinterpret_cast<void*>(a),i,n);};
      const auto o=P::skipPlayerContacts(A(it),A(prop),A(player),Ly,rd,wr);
      CHECK(o.contacts==3&&o.manifolds==1&&near(o.depthMax,.05f)&&!m0[Ly.contacts+Ly.active]&&!m0[Ly.contacts+Ly.stride+Ly.active]&&!sub[Ly.contacts+Ly.active]&&
            m1[Ly.contacts+Ly.active]==1,"player skip: the prop's contacts with the player off (the sub-manifold too), the NPC's kept");
      contact(m0,0,m0,.03f);const auto o2=P::skipPlayerContacts(A(it),A(npc),A(player),Ly,rd,wr);
      CHECK(o2.contacts==0&&m0[Ly.contacts+Ly.active]==1,"player skip: another instance's iterator touches nothing");
      const P::ImpactLayout off{P::off::itArrA,P::off::itArrB,P::off::itCountA,P::off::itCountB,P::off::mfSubs,P::off::mfSubCount,P::off::mfContacts,P::off::mfContactCount,
        P::off::ctStride,P::off::ctActive,P::off::ctDepth,P::off::ctOwner,P::off::mfInstA,P::off::mfInstB};
      CHECK(off.arrA==Ly.arrA&&off.countA==Ly.countA&&off.countB==Ly.countB&&off.subs==Ly.subs&&off.subCount==Ly.subCount&&off.contacts==Ly.contacts&&
            off.contactCount==Ly.contactCount&&off.stride==Ly.stride&&off.active==Ly.active&&off.owner==Ly.owner&&off.instA==Ly.instA&&off.instB==Ly.instB&&off.depth==Ly.depth,
            "player skip: the walk's layout is propScan's (DisableImpact 0x28b4290: +0xce, stride 0xd0, subs +0x110)");}}
    // ---------- push fix: the facing toward the body, the stroke peak, the class, the front message, the merged rest (pure) ----------
    {P::TouchIn t{};t.capKind=4;t.a=2.46f;t.speed=2.5f;t.swing=2.5f;t.facing=.36f;t.arm=2.57f;t.armGate=true;t.armIn=2.57f;t.armInMin=.9f;
     CHECK(P::touchVerdict(t,1.4f,1.2f,true,true,true,true).shoveFail==P::ShoveFacing,"facing: the v11 arm-capsule push (facing .36) failed the gate");
     t.facingBody=.85f;const auto tv=P::touchVerdict(t,1.4f,1.2f,true,true,true,true);
     CHECK(tv.kind==P::TouchShove&&tv.facingRescued,"facing: the palm toward the body's centre (.85) on an arm capsule: a shove");
     P::TouchIn c=t;c.capKind=2;c.facing=.2f;CHECK(P::touchVerdict(c,1.4f,1.2f,true,true,true,true).shoveFail==P::ShoveFacing,"facing: the chest keeps its own facing gate");
     P::TouchIn w=t;w.facingBody=.1f;CHECK(P::touchVerdict(w,1.4f,1.2f,true,true,true,true).shoveFail==P::ShoveFacing,"facing: a palm not toward the body stays refused");}
    {const std::uint64_t ms=1'000'000ull;P::StrokePeak p{};P::strokePeakStart(p,7u,2.37f,1000*ms);
     CHECK(P::strokePeakStep(p,true,true,2.6f,0.f,1030*ms,100*ms)==0&&P::strokePeakStep(p,true,true,3.03f,0.f,1060*ms,100*ms)==0&&
           P::strokePeakStep(p,true,true,1.f,0.f,1101*ms,100*ms)==1&&near(p.peak,3.03f),"stroke peak: the v11 stroke (edge 2.37, peak 3.03): a regrade at 100 ms");
     P::StrokePeak q{};P::strokePeakStart(q,7u,2.5f,0);CHECK(P::strokePeakStep(q,true,true,2.6f,0.f,50*ms,100*ms)==0&&P::strokePeakStep(q,false,false,0.f,0.f,60*ms,100*ms)==2,
           "stroke peak: under 10 % above the edge: no regrade");
     CHECK(P::pushDecide(3.03f,P::pushThrust(3.03f,2.5f,1.6f,1.5f),2.8f)==P::PushKnockdown&&P::pushDecide(2.37f,true,2.8f)==P::PushStumble,
           "stroke peak: a one-hand stroke peaking at 3.03 knocks down, its 2.37 edge alone stumbles");}
    {CHECK(P::pushClass(.82f,.3f)==P::PushBehind&&P::pushClass(-.62f,.5f)==P::PushFront&&P::pushClass(.1f,.9f)==P::PushSideA&&P::pushClass(-.2f,-.8f)==P::PushSideB,
           "push class: behind, front, along row a either way");
     const auto f1=P::pushFrontPlan(1,-.62f,40.f),f2=P::pushFrontPlan(2,-.62f,40.f),f0=P::pushFrontPlan(0,-.62f,40.f),fb=P::pushFrontPlan(1,.82f,40.f);
     CHECK(f1.face==0&&near(f1.impulse,40.f)&&f2.face==1&&f2.impulse<0.f&&f0.face<0&&fb.face<0&&fb.impulse<0.f,"front plan: lever 1 face 0 at 40, lever 2 face 1, lever 0 and a push from behind unchanged");
     // monotonic response: the escalated press's stumble never exceeds the shove's of the same force; the merged rest
     bool mono=true;float prev=0.f;for(float a=1.f;a<=2.8f;a+=.1f){const float d=P::stumbleDistanceM(a);mono=mono&&d>=prev;prev=d;}
     CHECK(mono&&near(P::stumbleDistanceM(P::passiveGradeA(1.34f,1.71f,0.f,false)),.1208f,1e-3f),"stumble: monotonic in a; the v11 light escalation (a 1.34) asks 12.1 cm, not 30");
     CHECK(near(P::stumbleRestM(1.88f,.106f),P::stumbleDistanceM(1.88f)-.106f,1e-5f)&&P::stumbleRestM(1.2f,.3f)==0.f,"merged rest: the larger push less what was applied, never negative");}
    // ---------- review fixes: the pushes graded over both hands, escalations, frozen movers, the side / behind message (pure) ----------
    {P::PushHands ph{};const P::V3 dx{1,0,0},dz{0,0,1};
     auto g=P::pushRegrade(ph,0.f,dx,0,P::PushInEdge,1.7f,dx);g=P::pushRegrade(ph,g.aEff,g.dir,1,P::PushInEdge,1.6f,dx);
     CHECK(near(g.aEff,2.5f,1e-4f),"pair: edges 1.7 and 1.6: aEff 2.5 (a stumble)");
     g=P::pushRegrade(ph,g.aEff,g.dir,0,P::PushInPeak,2.5f,dx);CHECK(near(g.aEff,2.5f+.8f,1e-4f),"pair: hand 0 peaks at 2.5: the pair rule 2.5 + 0.5 x 1.6");
     g=P::pushRegrade(ph,g.aEff,g.dir,1,P::PushInPeak,2.4f,dx);CHECK(near(g.aEff,3.7f,1e-4f)&&P::pushRedecide(P::PushInPeak,P::PushStumble,g.aEff,true,2.8f)==P::PushKnockdown,
           "pair: both peaks (2.5, 2.4): aEff 3.7, a firm pair knocks down (max alone gave 2.5)");
     const float before=g.aEff;g=P::pushRegrade(ph,g.aEff,g.dir,0,P::PushInPeak,1.f,dz);CHECK(near(g.aEff,before),"pair: a lower peak never lowers the push");
     P::PushHands one{};auto o=P::pushRegrade(one,0.f,dx,0,P::PushInEdge,1.88f,dx);
     const auto e=P::pushRegrade(one,o.aEff,o.dir,1,P::PushInEsc,1.86f,dz);
     CHECK(near(e.aEff,1.88f)&&nearV(e.dir,dx)&&one.hands==1u&&P::pushRedecide(P::PushInEsc,P::PushStumble,e.aEff,true,2.8f)==P::PushStumble&&
           P::pushMerge(1.88f,dx,1.86f,dx).aEff>=2.8f,"escalation of the other hand (guid 52: 1.88 + 1.86): max 1.88, a stumble (the pair rule gave 2.81, a knockdown)");
     const auto e2=P::pushRegrade(one,o.aEff,o.dir,0,P::PushInEsc,2.6f,dz);
     CHECK(near(e2.aEff,2.6f)&&P::pushRedecide(P::PushInEsc,P::PushStumble,2.6f,true,2.8f)==P::PushStumble&&P::pushRedecide(P::PushInEsc,P::PushKnockdown,1.f,false,2.8f)==P::PushKnockdown,
           "escalation: raises the push to its own a, never to a knockdown; a knockdown stays");
     CHECK(P::stumbleRestM(P::pushRegrade(one,1.88f,dx,0,P::PushInEsc,1.5f,dx).aEff,P::stumbleDistanceM(1.88f))==0.f,
           "escalation of the same touch weaker than its shove: nothing more after the shove's stumble (no second 24 cm)");}
    {const std::uint64_t ms=1'000'000ull;P::PressAsk pa{};
     P::pressEscalate(pa,.01f,.04f,1000*ms,.06f);P::pressEscalate(pa,.02f,1.9f,1028*ms,.06f);P::pressEscalate(pa,.02f,2.3f,1056*ms,.06f);const bool esc=P::pressEscalate(pa,.02f,1.2f,1084*ms,.06f);
     CHECK(esc&&near(P::escalatedA(pa),2.3f),"escalated press: graded on the press's peak drive (2.3), not the resting edge's arm 0.04");
     P::StrokePeak sp{};P::strokePeakStart(sp,3u,1.7f,1000*ms,1.2f);
     P::strokePeakStep(sp,true,true,2.2f,0.f,1030*ms,100*ms,2.1f);const int pk=P::strokePeakStep(sp,true,true,1.f,0.f,1101*ms,100*ms,.5f);
     CHECK(pk==1&&near(sp.armPeak,2.1f)&&!P::pushThrust(2.2f,1.2f,1.6f,1.5f)&&P::pushThrust(2.2f,sp.armPeak,1.6f,1.5f),
           "stroke peak: the arm's peak speed (2.1) makes a slow-start edge (arm 1.2) a thrust");}
    {CHECK(P::stumbleRoute(true,false,0,0,true,600.f)==P::StumbleQueue&&P::stumbleRoute(false,true,0,1,true,600.f)==P::StumbleMessage&&
           P::stumbleRoute(false,true,1,0,true,600.f)==P::StumbleWait&&P::stumbleRoute(false,true,0,1,false,600.f)==P::StumbleWait&&
           P::stumbleRoute(false,true,1,0,true,0.f)==P::StumbleDrop&&P::stumbleRoute(false,false,0,0,true,600.f)==P::StumbleDrop,
           "stumble route: free = offset, frozen (+0x848, the v11 guid 7 / 93) = the UP message, suspended (+0x846) = the wait, a ragdoll = dropped");
     CHECK(P::slapReaction(0.f)==1&&P::slapReaction(.8f)==2&&P::slapReaction(-.8f)==3,"frozen stumble: UP_FOR / UP_RGT / UP_LFT by side (no SHOVED, no fall)");
     const auto s1=P::pushSidePlan(1,.1f,.9f,1,75.f),s1n=P::pushSidePlan(1,.1f,-.9f,1,75.f),s2=P::pushSidePlan(2,.1f,.9f,1,75.f),s0=P::pushSidePlan(0,.1f,.9f,1,75.f);
     CHECK(s1.reaction==5&&s1n.reaction==6&&s2.reaction==6&&s0.reaction<0,"side plan: lever 1 KNCKBK_RGT for dirSide > 0, LFT for < 0, lever 2 swapped, lever 0 unchanged");
     const auto b2=P::pushSidePlan(0,.82f,.3f,2,75.f),b1=P::pushSidePlan(0,.82f,.3f,1,75.f),f2=P::pushSidePlan(0,-.62f,.3f,2,75.f);
     CHECK(near(b2.impulse,75.f)&&b1.impulse<0.f&&f2.impulse<0.f,"behind plan: a knockdown from behind on the fallback size takes 75; k read or a front push: unchanged");}
    {const std::uint64_t ms=1'000'000ull,q=300*ms;bool deep=false;
     CHECK(D::lingerVerdict(1000*ms,2400*ms,.01f,2500*ms,q)==0&&D::lingerVerdict(1000*ms,3400*ms,.05f,3500*ms,q,&deep)==0&&deep&&
           D::lingerVerdict(1000*ms,3400*ms,.01f,3500*ms,q)==1&&D::lingerVerdict(1000*ms,10900*ms,.2f,11000*ms,q)==2&&D::lingerVerdict(1000*ms,1100*ms,.2f,1500*ms,q)==1,
           "player skip linger: kept under 2 s while overlapping, past 2 s only while 3 cm or more deep, ended at 10 s or without an overlap");}
    // PLAYER SKIP slots: per hand the held prop (heavy or a welded light hold), two lingers, the oldest evicted by a third
    {P::SkipSlots k{};const std::uintptr_t cup=0x1000,chair=0x2000,mug=0x3000;const std::uint64_t ms=1'000'000ull;
     CHECK(P::skipSlotsStep(k,{chair,cup},{5,7},{false,true},100*ms)==0&&k.prop[0]==chair&&k.prop[1]==cup&&!k.light[0]&&k.light[1]&&k.index[1]==7&&
           P::skipHeldSlot(k,cup)==1&&P::skipHeldSlot(k,chair)==0&&P::skipHeldSlot(k,0)==-1,"skip slots: a chair in hand 0 and a welded cup in hand 1 both served");
     P::skipSlotsStep(k,{chair,0},{5,0xffff},{false,false},200*ms);
     CHECK(!k.prop[1]&&P::skipLingerSlot(k,cup)>=0&&k.linger[unsigned(P::skipLingerSlot(k,cup))].light&&k.linger[unsigned(P::skipLingerSlot(k,cup))].index==7&&
           k.linger[unsigned(P::skipLingerSlot(k,cup))].startNs==200*ms,"skip slots: the released cup lingers (light, its index, its start)");
     P::skipSlotsStep(k,{0,0},{0xffff,0xffff},{false,false},300*ms);
     CHECK(P::skipLingerSlot(k,cup)>=0&&P::skipLingerSlot(k,chair)>=0&&P::skipLingerSlot(k,cup)!=P::skipLingerSlot(k,chair),"skip slots: the chair lingers in the other slot");
     P::skipSlotsStep(k,{mug,0},{9,0xffff},{true,false},400*ms);const unsigned ev=P::skipSlotsStep(k,{0,0},{0xffff,0xffff},{false,false},500*ms);
     CHECK(ev==1&&P::skipLingerSlot(k,cup)<0&&P::skipLingerSlot(k,chair)>=0&&P::skipLingerSlot(k,mug)>=0,"skip slots: a third release evicts the oldest linger (the cup)");
     P::skipSlotsStep(k,{0,mug},{0xffff,9},{false,true},600*ms);CHECK(P::skipLingerSlot(k,mug)<0&&P::skipHeldSlot(k,mug)==1,"skip slots: a prop held again leaves its linger");
     P::SkipSlots q{};P::skipSlotsStep(q,{chair,chair},{5,5},{false,false},100*ms);P::skipSlotsStep(q,{0,0},{0xffff,0xffff},{false,false},200*ms);
     CHECK(P::skipLingerSlot(q,chair)>=0&&(q.linger[0].prop==0||q.linger[1].prop==0),"skip slots: a two-hand prop released lingers once");}
    // the welded prop's slip in the palm: a turned hand (prop turned with it) reads 0, a prop moved in the palm reads its move
    {const P::Rows r0{P::V3{1,0,0},P::V3{0,1,0},P::V3{0,0,1}};const P::V3 h{1.f,1.f,1.f},p0{1.f,.94f,1.f};const P::V3 l0=P::toFrame(r0,h,p0);
     const P::Rows r1{P::V3{1,0,0},P::V3{0,-1,0},P::V3{0,0,-1}}; // the hand turned 180 degrees about x: the cup now 6 cm above the palm
     const P::V3 p1{1.f,1.06f,1.f};
     CHECK(near(P::palmSlip(r1,h,p1,l0),0.f,1e-5f)&&near(P::palmSlip(r0,h,{1.f,.90f,1.f},l0),.04f,1e-5f)&&near(P::palmSlip(r1,h,{1.f,1.02f,1.f},l0),.04f,1e-5f),
           "palm slip: the cup turned over with the hand (12 cm higher in the world) reads 0; 4 cm moved in the palm reads 4 cm either way up");}
    {alignas(16) static unsigned char col[0x140]{};const float in[4]{3.f,1.f,3.f,59.5f};std::memcpy(col+0xa0,in,16);
     const float vel[4]{1.f,0.f,.5f,0.f};stub::setVelocity(col,vel);const float w[4]{0.f,2.f,0.f,0.f};stub::setAngVelocity(col,w);
     CHECK(D::setColliderMass(stub::addr(col),15.f)&&near(stub::at<float>(col,0xac),15.f),"held mass: 15 kg set");
     stub::impulseStep(col);CHECK(near(stub::at<float>(col,0x110),1.f,1e-4f)&&near(stub::at<float>(col,0x118),.5f,1e-4f)&&near(stub::at<float>(col,0x124),2.f),
           "held mass: the speed after the next impulse is kept (the momentum set again at 15 kg, not 3.97x)");
     CHECK(D::setColliderMass(stub::addr(col),59.5f)&&(stub::impulseStep(col),near(stub::at<float>(col,0x110),1.f,1e-4f)),"held mass restored: a two-hand throw keeps its speed");}
    {auto& st=D::state();const auto keep=st.heldMass;st.heldMass.set=true;st.heldMass.sim=0x1234;st.heldMass.level=0x5678;st.heldMass.kg0=59.5f;D::forgetAll(st);
     CHECK(st.heldMass.set&&st.heldMass.sim==0x1234&&near(st.heldMass.kg0,59.5f),"held mass: a dropped simulator keeps the record (restored if the same simulator and level return)");
     st.heldMass=keep;}
    // ---------- the impact guard and the inst user data ----------
    const auto restart=[]{stub::reset();stub::bind();P::resetForTests();P::modeForTests.store(P::ModeOn);};
    const auto refused=[](const char* why){return std::strcmp(P::installState.load(),why)==0&&!P::ready.load();};
    {restart();for(int i=0;i<3&&!D::state().hand[1].present;++i)frame({.5f,1.2f,0});
     alignas(16) static unsigned char it[0x30]{};const int f0=stub::forwards;
     stub::put<std::uintptr_t>(it,0x10,stub::addr(P::objects[1].inst));D::preApplyGuard(it);CHECK(stub::forwards==f0,"guard: our hand's iterator returns (no audio dispatch)");
     stub::put<std::uintptr_t>(it,0x10,stub::addr(stub::otherProp.inst));D::preApplyGuard(it);CHECK(stub::forwards==f0+1&&stub::lastIt==it,"guard: another instance goes to PreApplyImpacts_Game");
     stub::put<std::uintptr_t>(stub::sim,0xe8,0x1234);const int d0=stub::deletes;frame({.5f,1.2f,0});
     CHECK(!D::state().hand[1].present&&stub::deletes==d0+1&&refused("IMPACT FUNCTOR MISMATCH")&&stub::fnWord(stub::sim)==0x1234,"a foreign fn while a body is in: removed, refused, word left");
     frame({.5f,1.2f,0});CHECK(P::stats.mode.load()==P::ModeOff&&stub::count()==2&&!D::state().guardSim,"next tick OFF");
     CHECK(stub::at<std::uintptr_t>(stub::sim,P::off::simPreCompute)==stub::base+P::off::defaultPreCompute,"refused: the default PreComputeImpacts back");}
    {restart();for(int i=0;i<3&&!D::state().hand[1].present;++i)frame({.5f,1.2f,0});
     CHECK(stub::at<std::uintptr_t>(stub::sim,P::off::simPreCompute)==D::preComputeWord()&&D::state().skipOurs,"player skip: preComputeGuard in the + 0xc8 word with the guard");
     alignas(16) static unsigned char it[0x30]{};const int f0=stub::pcForwards;
     stub::put<std::uintptr_t>(it,0x10,stub::addr(stub::otherProp.inst));D::preComputeGuard(it);
     CHECK(stub::pcForwards==f0+1&&stub::pcLastIt==it,"player skip: every iterator goes on to DefaultPreComputeImpacts");
     P::leverPatchForTests.store(+[](P::Levers& l){l.grabPlayerSkip=false;});D::state().leversRead=false;frame({.5f,1.2f,0});
     CHECK(stub::at<std::uintptr_t>(stub::sim,P::off::simPreCompute)==stub::base+P::off::defaultPreCompute&&!D::state().skipOurs,"player skip: lever off: the default word back");
     P::leverPatchForTests.store(nullptr);D::state().leversRead=false;frame({.5f,1.2f,0});
     const auto r0=P::stats.playerSkip.restores.load();P::modeForTests.store(P::ModeMeasure);frame({.5f,1.2f,0});
     CHECK(stub::at<std::uintptr_t>(stub::sim,P::off::simPreCompute)==stub::base+P::off::defaultPreCompute&&P::stats.playerSkip.restores.load()==r0+1,"player skip: MEASURE: the default word back");}
    {restart();P::engine.instDataCtor=&stub::badInstDataCtor;frame({.5f,1.2f,0});
     CHECK(!stub::adds&&refused("USER DATA MISMATCH")&&!D::userOk(1),"user data with a wrong layout: no insert, refused");}
    {restart();for(int i=0;i<3&&!D::state().hand[1].present;++i)frame({.5f,1.2f,0});
     const auto r0=P::stats.guardRestores.load();P::modeForTests.store(P::ModeMeasure);frame({.5f,1.2f,0});
     CHECK(stub::fnWord(stub::sim)==stub::base+P::off::preApplyGame&&P::stats.guardRestores.load()==r0+1,"MEASURE, both out of the level: the game's fn back");}
    CHECK(!stub::addsUnguarded,"no insert ever without the guard in the simulator's fn word");
    restart();for(int i=0;i<3&&!D::state().hand[1].present;++i)frame({.5f,1.2f,0});
    stub::placeProp(stub::otherProp,9,stub::worldVtable,{3,3,3},.1f,2);
    D::collideObjects(&P::objects[1].beh,P::objects[1].inst,nullptr,stub::otherProp.inst,nullptr,nullptr); // one pair sample for the report
    {const auto r=P::report();
     CHECK(r.find("\"groups\":{\"core\":true,\"palm\":true")!=std::string::npos&&r.find("\"reachWake\":{\"queries\":")!=std::string::npos&&
           r.find("\"props\":{")!=std::string::npos&&r.find("\"doors\":{")!=std::string::npos&&r.find("\"deactRefused\":")!=std::string::npos&&
           r.find("\"shape\":\"PALM\"")!=std::string::npos&&r.find("\"worldPairs\":1")!=std::string::npos,"report: groups, shape, props, reach, doors, pairs by kind");
     CHECK(r.find("\"swing\":true")!=std::string::npos&&r.find("\"swingHolds\":")!=std::string::npos&&r.find("\"unlockSkips\":{")!=std::string::npos&&
           r.find("\"virtual\":{")!=std::string::npos&&r.find("\"touch\":{")!=std::string::npos&&r.find("\"doorSwing\":1")!=std::string::npos&&
           r.find("\"skippedBy\":{")!=std::string::npos,"report: door swing, unlock, virtual body, slap, levers");
     CHECK(r.front()=='{'&&r.back()=='}'&&r.find("\"stepSpanUsAvg\":")!=std::string::npos&&r.find("\"samples\":[{")!=std::string::npos,"report shape");
     int depth=0;bool ok=true;for(char c:r){if(c=='{'||c=='[')++depth;if(c=='}'||c==']')--depth;if(depth<0)ok=false;}CHECK(ok&&depth==0,"report braces balance");
     CHECK(jsonValid(r),"report: valid JSON");
     CHECK(r.find("\"approach\":{\"hooks\":")!=std::string::npos&&r.find("\"glass\":{\"on\":")!=std::string::npos&&r.find("\"cups\":{\"shallowBy\":")!=std::string::npos&&
           r.find("\"grabDrive\":{\"holdHist\":")!=std::string::npos&&r.find("\"npcEdges\":{\"windows\":")!=std::string::npos&&r.find("\"doorAuto\":0")!=std::string::npos&&
           r.find("\"doorgate\":")!=std::string::npos,"report: the approach, glass, cups, grab drive, npc edge sections, levers, groups");
     CHECK(r.find("\"blood\":{\"on\":")!=std::string::npos&&r.find("\"bloodCdMs\":")!=std::string::npos&&r.find("\"blood\":true")!=std::string::npos,
           "report: glass.blood, the blood levers, the blood group");
     CHECK(r.find("\"noPane\":")!=std::string::npos&&r.find("\"thrown\":")!=std::string::npos&&r.find("\"dipsKept\":")!=std::string::npos,
           "report: blood refused noPane, seat thrown / dipsKept");
     std::printf("%s\n",r.c_str());}
    // ---------- cost of the pure parts on this host (per call) ----------
    {const auto t0=std::chrono::steady_clock::now();P::V3 acc{};P::ContactEst e{};P::VisualMem vm{};
     for(int i=0;i<100000;++i){const auto d=P::contactDrive({float(i&7),1,0},{0,0,0},{-1,0,0},0,false,.1f,1.5f,3.f);acc=V::add(acc,d.vel);
        acc=V::add(acc,P::spinFrame(identity,V::rotate({0,.01f*float(i&15),0},identity),.014f,20.f));
        P::noteSubstep(e,{-.001f,0,0},{0,0,0},{1,0,0},.0005f,.05f);
        acc=V::add(acc,P::visualPlane(vm,true,{-1,0,0},-1.f,{1.01f,0,0},identity,identity,.028f,.006f,.4f,.03f,.5f,true).offset);}
     const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/1e5;
     std::printf("     host cost (Debug): contactDrive + spinFrame + noteSubstep + visualPlane = %.3f us per hand-substep (%g)\n",us,double(acc[0]));}
    {P::VBodies vb{};vb.count=3;for(unsigned bi=0;bi<3;++bi){auto& b=vb.b[bi];b.guid=bi+1;b.count=12;
        for(unsigned k=0;k<12;++k)b.c[k]={{.1f+.5f*float(bi),.2f+.12f*float(k),0},{.1f+.5f*float(bi),.3f+.12f*float(k),.05f},.06f,1,std::uint8_t(k%6)};P::finishBody(b);}
     float acc=0.f;auto t0=std::chrono::steady_clock::now();
     for(int i=0;i<100000;++i){P::V3 c{.02f+.0000001f*float(i&255),1.2f,0};acc+=P::pushOut(c,c,identity,false,vb,.01f,1.f).depth;}
     const double touching=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/1e5;
     t0=std::chrono::steady_clock::now();
     for(int i=0;i<100000;++i){P::V3 c{3.f,1.2f,.0000001f*float(i&255)};acc+=P::pushOut(c,c,identity,false,vb,.01f,1.f).depth;}
     const double away=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-t0).count()/1e5;
     std::printf("     host cost (Debug): pushOut, 3 bodies of 12 capsules: touching %.3f us, all bodies far %.3f us per hand-substep (%g)\n",touching,away,double(acc));}
    std::printf(fails?"\n%d FAILED\n":"\nALL PASSED\n",fails);
    return fails?1:0;
}
