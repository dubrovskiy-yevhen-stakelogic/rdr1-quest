#pragma once
#include "ProjectileLaunchBoundary.h"
namespace rdrvr::engine {
struct ProjectileLaunchSample {
    runtime::Mat4 world{};runtime::Vec3 velocity{},muzzle{},delta{};
    std::uint32_t actorGuid{};int weaponType{};float scalar{};bool nativeFlag{},finalFlag{};
    bool operator==(const ProjectileLaunchSample&)const=default;
};
struct ProjectileLaunchCopy {bool matchesArguments{};std::uint8_t nativeFlags{};};
class ProjectileLaunchReader {
public:
    struct Api {bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};std::uintptr_t projectileTable{};};
private:
    Api api_;
    template<class T>bool get(std::uintptr_t p,std::size_t offset,T& out)const{
        return api_.read&&p&&p<=UINTPTR_MAX-offset&&p+offset<=UINTPTR_MAX-sizeof(T)&&api_.read(api_.context,p+offset,&out,sizeof out);
    }
    static bool finite(const ProjectileLaunchSample& s){
        for(float x:s.world)if(!std::isfinite(x))return false;
        for(const auto& v:{s.velocity,s.muzzle,s.delta})for(float x:v)if(!std::isfinite(x))return false;
        return std::isfinite(s.scalar);
    }
    static void affine(runtime::Mat4& m){m[3]=m[7]=m[11]=0;m[15]=1;}
    bool argument(const ProjectileLaunchCall& call,ProjectileLaunchSample& out)const{
        std::uintptr_t table{};WeaponNativeMatrix matrix{};
        if(!get(reinterpret_cast<std::uintptr_t>(call.projectile),0,table)||table!=api_.projectileTable||
           !get(reinterpret_cast<std::uintptr_t>(call.world),0,matrix)||
           !get(reinterpret_cast<std::uintptr_t>(call.velocity),0,out.velocity)||
           !get(reinterpret_cast<std::uintptr_t>(call.muzzle),0,out.muzzle)||!get(reinterpret_cast<std::uintptr_t>(call.delta),0,out.delta))return false;
        out.world=matrix.values;affine(out.world);out.actorGuid=call.actorGuid;out.weaponType=call.weaponType;
        out.scalar=call.scalar;out.nativeFlag=call.nativeFlag;out.finalFlag=call.finalFlag;return finite(out);
    }
    struct Copied {
        std::uintptr_t table{};runtime::Mat4 world{};runtime::Vec3 velocity{},delta{};
        std::uint32_t guid{};int type{};float scalar{};std::uint8_t flags{};
        bool operator==(const Copied&)const=default;
    };
    bool copied(std::uintptr_t object,Copied& out)const{
        WeaponNativeMatrix matrix{};
        if(!get(object,0,out.table)||out.table!=api_.projectileTable||!get(object,0x20,matrix)||
           !get(object,0x1e0,out.velocity)||!get(object,0x2e0,out.delta)||!get(object,0x2a8,out.guid)||
           !get(object,0x298,out.type)||!get(object,0x2c4,out.scalar)||!get(object,0x2f8,out.flags))return false;
        out.world=matrix.values;affine(out.world);
        ProjectileLaunchSample check{};check.world=out.world;check.velocity=out.velocity;check.delta=out.delta;check.scalar=out.scalar;
        return finite(check);
    }
public:
    explicit ProjectileLaunchReader(Api api):api_(api){}
    bool capture(const ProjectileLaunchCall& call,const LocalWeapon& owner,ProjectileLaunchSample& out)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            if(!api_.projectileTable||!owner.weapon||!owner.actorGuid||call.actorGuid!=owner.actorGuid||call.weaponType!=owner.type.id)return false;
            ProjectileLaunchSample a{},b{};if(!argument(call,a)||!argument(call,b)||a!=b)return false;
            out=a;return true;
        }catch(...){return false;}
    }
    // A successful void Launch return alone does not prove native launch
    // success: InitPhysical can fail internally. Record copied fields and raw
    // native flags without turning either into a hit/ballistics assertion.
    bool completion(const ProjectileLaunchCall& call,const ProjectileLaunchSample& expected,ProjectileLaunchCopy& out)const noexcept{
        const int saved=errno;struct Restore{int n;~Restore(){errno=n;}} restore{saved};
        try{
            if(!api_.projectileTable||!expected.actorGuid)return false;
            Copied a{},b{};const auto object=reinterpret_cast<std::uintptr_t>(call.projectile);
            if(!copied(object,a)||!copied(object,b)||a!=b)return false;
            out={a.guid==expected.actorGuid&&a.type==expected.weaponType&&a.scalar==expected.scalar&&
                 a.world==expected.world&&a.velocity==expected.velocity&&a.delta==expected.delta,a.flags};return true;
        }catch(...){return false;}
    }
};
}
