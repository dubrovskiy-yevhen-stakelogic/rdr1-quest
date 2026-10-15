#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cerrno>
namespace rdrvr::engine {
// These are the mod's own labels, not numeric values of the retail enum.
enum class WeaponClass : unsigned { Unknown, Pistol, DualPistol, Rifle, Repeater,
    SniperRifle, Shotgun, Thrown, ThrownExploding, Bow, Melee, Lasso, Turret, Cannon };
inline const char* weaponClassName(WeaponClass kind) {
    constexpr const char* names[]{"UNKNOWN","PISTOL","DUAL_PISTOL","RIFLE","REPEATER",
        "SNIPER_RIFLE","SHOTGUN","THROWN","THROWN_EXPLODING","BOW","MELEE","LASSO","TURRET","CANNON"};
    const auto i=static_cast<unsigned>(kind);return i<std::size(names)?names[i]:names[0];
}
struct WeaponTypeApi {
    bool (*read)(void*,std::uintptr_t,void*,std::size_t){};void* context{};
    std::uintptr_t tree{},baseTypeTable{};
    std::array<std::uintptr_t,13> enumTables{};
    explicit operator bool()const {
        if(!read||!tree||!baseTypeTable)return false;
        for(unsigned i=0;i<enumTables.size();++i){
            if(!enumTables[i])return false;
            for(unsigned j=0;j<i;++j)if(enumTables[i]==enumTables[j])return false;
        }return true;
    }
};
struct WeaponType {std::int16_t id{};WeaponClass kind{};};
class WeaponTypeReader {
    WeaponTypeApi api_;
    template<class T>bool read(std::uintptr_t object,std::size_t offset,T& out)const {
        return object&&object<=UINTPTR_MAX-offset&&object+offset<=UINTPTR_MAX-sizeof(T)&&
            api_.read(api_.context,object+offset,&out,sizeof(T));
    }
public:
    explicit WeaponTypeReader(WeaponTypeApi api):api_(api){}
    // No virtual calls: GetBaseWeaponType can load assets. The exact tree lookup
    // is instead copied with a bounded path, cycle detection and readback.
    bool inspect(std::uintptr_t weapon,WeaponType& out)const noexcept {
        const int saved=errno;struct Restore{int value;~Restore(){errno=value;}} restore{saved};
        try {
            if(!api_)return false;
            std::uintptr_t base{},baseTable{},root{};std::int16_t id{};
            if(!read(weapon,0x28,base)||base%8||!read(base,0,baseTable)||baseTable!=api_.baseTypeTable||
               !read(base,8,id)||!read(api_.tree,0,root))return false;
            struct Node{std::uintptr_t address{},value{},next{};std::int32_t key{};unsigned branch{};};
            std::array<Node,64> path{};unsigned used{};auto address=root;
            std::uintptr_t enumObject{},enumTable{};WeaponClass kind{};
            while(address&&used<path.size()){
                if(address%8)return false;
                for(unsigned i=0;i<used;++i)if(path[i].address==address)return false;
                auto& node=path[used++];node.address=address;
                if(!read(address,8,node.key))return false;
                if(node.key==id){
                    if(!read(address,0,node.value)||node.value%8||!read(node.value,0,enumTable))return false;
                    enumObject=node.value;
                    for(unsigned i=0;i<api_.enumTables.size();++i)
                        if(enumTable==api_.enumTables[i])kind=static_cast<WeaponClass>(i+1);
                    break;
                }
                node.branch=node.key>id?0x18:0x20;
                if(!read(address,node.branch,node.next))return false;
                address=node.next;
            }
            if(kind==WeaponClass::Unknown)return false;
            // A successful observation must still describe the same tree path
            // and active weapon type when returned. It is not a lifetime token.
            std::uintptr_t again{};std::int16_t idAgain{};
            if(!read(weapon,0x28,again)||again!=base||!read(base,0,again)||again!=baseTable||
               !read(base,8,idAgain)||idAgain!=id||!read(api_.tree,0,again)||again!=root||
               !read(enumObject,0,again)||again!=enumTable)return false;
            for(unsigned i=0;i<used;++i){const auto& node=path[i];std::int32_t key{};
                if(!read(node.address,8,key)||key!=node.key||
                   !read(node.address,node.branch,again)||again!=(node.branch?node.next:node.value))return false;
            }
            out={id,kind};return true;
        }catch(...){return false;}
    }
};
}
