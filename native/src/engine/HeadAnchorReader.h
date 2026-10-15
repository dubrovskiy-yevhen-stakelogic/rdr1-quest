#pragma once
#include "HeadCameraAnchor.h"
#include "BodyAttachmentReader.h"
#include "NativeAimProbe.h"
#include "WeaponOwnerReader.h"
#include <cstring>
#include <limits>

namespace rdrvr::engine {
struct HeadAnchorApi {
    WeaponOwnerApi player{};
    bool (*head)(const void*,AimNativeVector*){};
    std::uintptr_t reticleTable{};
    BodyAttachmentApi attachments{};
    // engineBase+0x2db79e0 = mvrMoverSimComponent vtable address point (_ZTV20mvrMoverSimComponent 0x2db79d0
    // + 0x10; ctor 0x2892c34 stores [GOT 0x2e0c468]+0x10, R_AARCH64_GLOB_DAT -> 0x2db79d0). 0 = ragdoll state unknown.
    std::uintptr_t moverSimVtable{};
};
// Called after the pinned local PostCameraUpdate reticle call, never from the
// XR/render worker. Unarmed players are eligible: no inventory lookup here.
class HeadAnchorReader {
    HeadAnchorApi api_;
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&
            api_.player.read(api_.player.context,p+offset,&out,sizeof(T));
    }
    struct Identity {
        std::uintptr_t player{},actor{},slots{},manager{},component{},matrix{},health{};
        std::uintptr_t entity{},instance{},creature{};
        std::uint32_t guid{},state{};std::uint16_t count{},generation{},locks{};
        unsigned char active{},lock1{},lock2{};float hp{};
        bool operator==(const Identity&)const=default;
    };
    bool identity(std::uintptr_t reticle,Identity& out)const{
        const auto& api=api_.player;Identity i{};std::uintptr_t table{},back{};
        if(!get(reticle,0,table)||table!=api_.reticleTable||!get(api.playerManager,0,table)||table!=api.playerManagerTable||
           !get(api.playerManager,0xa20,i.player)||!get(i.player,0,table)||table!=api.playerTable||
           !get(i.player,0x5f1,i.active)||i.active!=1||!get(i.player,0x5f4,i.guid)||
           !get(api.actorManagerGlobal,0,i.manager)||!get(api.actorSlotsGlobal,0,i.slots)||
           !get(i.manager,0,i.count)||!i.slots||i.slots%8)return false;
        const auto index=i.guid&0xffffu;
        if(index>=i.count||!get(i.slots,index*16,i.actor)||!i.actor||
           !get(i.slots,index*16+8,i.generation)||i.generation!=(i.guid>>16)||
           !get(reticle,0x10,back)||back!=i.actor||!get(i.actor,0xb8,back)||back!=reticle)return false;
        if(!get(i.actor,0xb0,i.component)||!get(i.component,0x18,i.matrix)||!i.matrix||
           !get(i.actor,0x60,i.health)||!get(i.health,0x20,i.hp)||!std::isfinite(i.hp)||i.hp<=0||
           !get(i.player,0x104,i.locks)||!get(i.player,0x10c,i.lock1)||!get(i.player,0x10d,i.lock2)||
           i.locks||i.lock1||i.lock2||!get(i.player,0x5d4,i.state)||i.state==3||i.state==5||
           !get(i.actor,0x50,i.entity)||!get(i.entity,0x88,i.instance)||!get(i.instance,0x138,i.creature)||!i.creature)return false;
        out=i;return true;
    }
    // Mover facts for the body anchor. Every read is optional (a failed read leaves the fact unknown/false);
    // only the identity/matrix checks above decide whether a sample exists at all.
    void moverFacts(const Identity& i,const runtime::Mat4& body,BodyAnchorFacts& f)const{
        for(unsigned n=0;n<3;++n){f.capsule[n]=body[12+n];f.up[n]=body[4+n];}
        std::uintptr_t mover{},vtable{},attached{};
        if(!get(i.actor,0x88,mover)||!mover||mover>UINTPTR_MAX-0x40)return;
        f.moverMatrix=i.matrix==mover+0x40;
        if(api_.moverSimVtable&&get(mover,0,vtable)&&vtable==api_.moverSimVtable){
            std::uint32_t ragdoll{};
            if(get(mover,0xb8c,ragdoll)){f.simKnown=true;f.ragdoll=ragdoll!=0;}
        }
        std::int16_t suspend{},frozen{};std::uint8_t cover{};std::uint32_t ladder{};
        if(get(mover,0x830,attached))f.attached=attached!=0;
        if(get(mover,0x846,suspend))f.suspended=suspend>0;
        if(get(mover,0x848,frozen))f.frozen=frozen>0;
        if(get(mover,0xaca,cover))f.cover=cover!=0;
        if(get(mover,0xaf0,ladder))f.ladder=ladder!=0;
        std::array<float,3> v{};
        if(get(mover,0x7b0,v)&&std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2])){
            f.velocityKnown=true;for(unsigned n=0;n<3;++n)f.velocity[n]=v[n];}
    }
public:
    explicit HeadAnchorReader(HeadAnchorApi api):api_(api){}
    // `facts` (optional) receives the capsule/mover state; requireHead=false keeps the sample when the native head
    // query declines (body anchor: the head bone only feeds AUTO eye height). The position is then the capsule base.
    bool sample(std::uintptr_t reticle,std::uint64_t now,std::uint64_t revision,HeadCameraAnchor& out,
                BodyAnchorFacts* facts=nullptr,bool requireHead=true)const noexcept{
        const int saved=errno;struct Restore{int saved;~Restore(){errno=saved;}} restore{saved};
        try{
            if(facts)*facts={};
            if(!api_.player.read||!api_.head||!api_.reticleTable||!now||!revision)return false;
            Identity before{},after{};runtime::Mat4 body{},again{};
            if(!identity(reticle,before)||!get(before.matrix,0,body))return false;
            const auto hat=readBodyHat(api_.attachments,api_.player.read,api_.player.context,before.actor);
            AimNativeVector head{};head.v.fill(std::numeric_limits<float>::quiet_NaN());
            // Native false means a body-position fallback, not a head pose.
            bool headValid=api_.head(reinterpret_cast<const void*>(before.actor),&head);
            for(unsigned n=0;n<3;++n)headValid=headValid&&std::isfinite(head.v[n]);
            if((requireHead&&!headValid)||
               !identity(reticle,after)||before!=after||!get(before.matrix,0,again))return false;
            // Matrix34 padding is unspecified; compare only the meaningful lanes.
            for(unsigned c=0;c<4;++c)for(unsigned r=0;r<3;++r){const auto n=c*4+r;
                if(!std::isfinite(body[n])||body[n]!=again[n])return false;}
            HeadCameraAnchor sample{};sample.actorGuid=before.guid;sample.sampledNs=now;sample.presentationRevision=revision;
            sample.bodyCreature=before.creature;
            const auto hatAfter=readBodyHat(api_.attachments,api_.player.read,api_.player.context,after.actor);
            sample.hatNode=hat==hatAfter?hat:0;
            for(unsigned n=0;n<3;++n){sample.position[n]=headValid?head.v[n]:body[12+n];sample.up[n]=body[4+n];}
            const float norm=sample.up[0]*sample.up[0]+sample.up[1]*sample.up[1]+sample.up[2]*sample.up[2];
            if(!std::isfinite(norm)||std::abs(norm-1)>0.005f)return false;
            if(facts){
                BodyAnchorFacts f{};moverFacts(before,body,f);
                f.headValid=headValid;if(headValid)for(unsigned n=0;n<3;++n)f.head[n]=head.v[n];
                f.valid=true;*facts=f;
            }
            out=sample;return true;
        }catch(...){if(facts)*facts={};return false;}
    }
};
}
