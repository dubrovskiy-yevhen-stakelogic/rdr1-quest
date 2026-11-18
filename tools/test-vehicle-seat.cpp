// Host regression for the vehicle seat rules: the seat read of engine::WeaponContextReader over fake engine memory, the VR weapon gates, the
// manual reload vetoes and the B button. Build without NDEBUG, with native/src, native/src/vr and the tools/gamecode.py output folder on the
// include path, plus a stub sys/system_properties.h. MSVC also needs stubs of clock_gettime and __builtin_return_address.
#include "engine/WeaponContextReader.h"
#include "vr/VrControls.h"
#include "vr/VrReload.h"
#include "vr/VrWeapon.h"
#include <cassert>
#include <cstring>
#include <map>
#include <vector>
using namespace rdrvr::engine;
namespace V=rdrvr::vr;
namespace S=V::vehicleseat;

static std::map<std::uintptr_t,std::vector<unsigned char>> memory;
static bool readFake(void*,std::uintptr_t at,void* out,std::size_t size) {
    auto it=memory.upper_bound(at);if(it==memory.begin())return false;--it;
    const auto offset=at-it->first;if(offset+size>it->second.size())return false;
    std::memcpy(out,it->second.data()+offset,size);return true;
}
static void region(std::uintptr_t at,std::size_t size) {memory[at].assign(size,0);}
template<class T> static void put(std::uintptr_t at,T value) {
    auto it=memory.upper_bound(at);assert(it!=memory.begin());--it;
    assert(at-it->first+sizeof(T)<=it->second.size());std::memcpy(it->second.data()+(at-it->first),&value,sizeof(T));
}
static constexpr std::uintptr_t manager=0x10000,actorManager=0x11000,actorSlots=0x12000,actorCount=0x13000,slotTable=0x14000,gohManager=0x15000,gohSlots=0x16000,
    gohTable=0x17000,player=0x20000,actor=0x30000,health=0x31000,states=0x32000,vehAction=0x33000,behavior=0x34000,contexts=0x35000,duel=0x36000,mounted=0x37000,
    mount=0x38000,aim=0x39000,linked=0x3a000,motion=0x3b000,component=0x3c000,body=0x40000,record=0x41000,vehicle=0x50000,vehicleComp=0x51000,sim=0x52000,
    type=0x53000,set=0x54000,seats=0x55000;
static constexpr std::uintptr_t managerTable=0xaaa0,playerTable=0xaab0;
static constexpr std::uint32_t playerGuid=(7u<<16)|1u,vehicleGuid=(9u<<16)|2u;
static void build() {
    memory.clear();
    for(auto r:{manager,actorManager,actorSlots,actorCount,slotTable,gohManager,gohSlots,gohTable,player,actor,health,states,vehAction,behavior,contexts,duel,mounted,
                mount,aim,linked,motion,component,body,record,vehicle,vehicleComp,sim,type,set,seats})region(r,0x1000);
    put(manager,managerTable);put(manager+0xa20,player);put(player,playerTable);put<std::uint8_t>(player+0x5f1,1);put(player+0x5f4,playerGuid);
    put(actorManager,actorCount);put(actorSlots,slotTable);put<std::uint16_t>(actorCount,16);put(gohManager,gohTable);put<std::uint16_t>(gohTable,16);put(gohSlots,slotTable);
    put(slotTable+16,actor);put<std::uint16_t>(slotTable+16+8,7);put(slotTable+32,vehicle);put<std::uint16_t>(slotTable+32+8,9);
    put(actor+0x60,health);put(health+0x20,100.f);put<std::uint32_t>(player+0x5d4,3);put(player+0x410,states);put(player+0x418,vehAction);
    put(actor+0x38,behavior);put(behavior+0xab8,contexts);put(contexts+0x20,duel);put(contexts+0x28,mounted);put(contexts+0x68,mount);put(contexts+0x18,aim);
    put(contexts+0x50,linked);put(actor+0x88,motion);put(actor+0xb0,component);
    put(actor+0x40,body);put(body+0x238,record);put<std::int32_t>(record+0x14,1);put<std::uint32_t>(record+0x20,1);put(record+0x50,vehicleGuid);
    put(vehicle+0xb0,vehicleComp);put(vehicleComp+0xa38,sim);put(sim+0xac,vehicleGuid);put(sim+0x90,type);put<std::int32_t>(type+0x10,7);
    put(vehicleComp+0xc88,set);put<std::uint16_t>(set+0x20,6);put(set+0x18,seats);
}
static WeaponContext inspect() {
    WeaponOwnerApi api{};api.read=&readFake;api.playerManager=manager;api.actorManagerGlobal=actorManager;api.actorSlotsGlobal=actorSlots;
    api.playerManagerTable=managerTable;api.playerTable=playerTable;api.gohManagerGlobal=gohManager;api.gohSlotsGlobal=gohSlots;
    LocalWeapon owner{};owner.player=player;owner.actor=actor;owner.actorGuid=playerGuid;
    WeaponContext c{};const bool ok=WeaponContextReader(api).inspectPlayer(owner,c);assert(ok);return c;
}
static std::uint32_t mappedB(bool seat) {
    V::controls::Mapper m{};V::controls::MapIn in{};in.ready=true;in.layout=V::controls::Layout::Mounted;in.vehicleSeat=seat;in.now=1'000'000'000ull;
    in.buttons=V::controls::bit::B;return m.apply(in,0u);
}
int main() {
    build();
    { // the driver: PilotLocation of location 1
        put<std::uint8_t>(seats+0x78+0x70,1);auto c=inspect();
        assert(c.ridingVehicle&&c.weaponSeat&&!c.seatSwap&&c.vehicleType==7&&c.seatIndex==1&&c.seatClass==S::SeatDriver&&c.knownLock());
        S::driverOn=false;assert(!inspect().weaponSeat);S::driverOn=true;
        S::weaponsOn=false;assert(!inspect().weaponSeat);S::weaponsOn=true;
    }
    build();
    { // shotgun = WeaponLocation, passenger = neither flag
        put<std::uint8_t>(seats+0x78+0x71,1);auto c=inspect();assert(c.weaponSeat&&c.seatClass==S::SeatShotgun);
        put<std::uint8_t>(seats+0x78+0x71,0);c=inspect();assert(c.weaponSeat&&c.seatClass==S::SeatPassenger);
    }
    { // a move between seats: the signal or the location state; never a transition, always a swap
        put<std::int32_t>(vehAction+0x18,3);put<std::uint8_t>(vehAction+8,1);auto c=inspect();
        assert(c.weaponSeat&&c.seatSwap&&c.seatClass==S::SeatMoving&&!c.vehicleTransition);
        put<std::int32_t>(vehAction+0x18,0);put<std::uint8_t>(vehAction+8,0);put<std::uint32_t>(record+0x20,4);put<std::int32_t>(record+0x14,-1);
        c=inspect();assert(c.weaponSeat&&c.seatSwap&&c.seatClass==S::SeatMoving);
        put<std::uint32_t>(record+0x20,1);put<std::int32_t>(record+0x14,1);
    }
    { // every other vehicle type stays refused, an unreadable chain too, and the context itself stays readable
        for(std::int32_t k:{0,2,8,10}){put<std::int32_t>(type+0x10,k);auto c=inspect();assert(c.ridingVehicle&&!c.weaponSeat&&c.vehicleType==k);}
        put<std::int32_t>(type+0x10,7);
        put(sim+0xac,vehicleGuid+1);assert(!inspect().weaponSeat&&inspect().vehicleType==-1);put(sim+0xac,vehicleGuid);
        put(record+0x50,(8u<<16)|2u);assert(!inspect().weaponSeat);put(record+0x50,vehicleGuid);
        put(body+0x238,std::uintptr_t(0));assert(!inspect().weaponSeat);put(body+0x238,record);
        put<std::uint16_t>(set+0x20,1);auto c=inspect();assert(!c.weaponSeat&&c.seatClass==S::SeatNone&&c.vehicleType==7);put<std::uint16_t>(set+0x20,6);
        put(vehicleComp+0xc88,std::uintptr_t(0));assert(!inspect().weaponSeat);put(vehicleComp+0xc88,set);
    }
    { // on foot / a horse: nothing of this is read
        put<std::uint32_t>(player+0x5d4,5);auto c=inspect();assert(c.riding&&!c.ridingVehicle&&!c.weaponSeat&&!c.seatSwap&&c.vehicleType==-1);
        put<std::uint32_t>(player+0x5d4,2);put<std::int32_t>(vehAction+0x18,3);c=inspect();assert(!c.seatSwap&&!c.ridingVehicle);
        put<std::int32_t>(vehAction+0x18,0);put<std::uint32_t>(player+0x5d4,3);
    }
    { // the VR weapon gates
        namespace W=V::weapon;const W::Config cfg{};WeaponContext c{};c.alive=true;c.ridingVehicle=true;
        const auto kind=WeaponClass::Pistol;
        auto g=W::gates(c,kind,cfg);assert(!g.pose&&!g.force&&!g.aim);
        c.weaponSeat=true;g=W::gates(c,kind,cfg);assert(g.pose==W::attachable(kind,cfg)&&g.force==g.pose&&g.aim==g.pose&&g.pose);
        c.seatSwap=true;g=W::gates(c,kind,cfg);assert(g.pose&&!g.force&&!g.aim);c.seatSwap=false;
        c.mountedWeapon=true;assert(!W::gates(c,kind,cfg).pose);c.mountedWeapon=false;
        c.vehicleTransition=true;assert(!W::gates(c,kind,cfg).pose);
    }
    { // the manual reload
        namespace R=V::reload;R::Context c{};c.valid=true;c.alive=true;c.ridingVehicle=true;
        assert(R::vetoOf(c,1)&R::VetoVehicle);
        c.weaponSeat=true;assert(!R::vetoOf(c,1));
        c.seatSwap=true;assert(R::vetoOf(c,1)&R::VetoTransition);c.seatSwap=false;
        c.vehicleTransition=true;assert(R::vetoOf(c,1)&R::VetoTransition);
    }
    // B on a horse = jump (pad X), B in a weapon seat = the stock reload
    assert(mappedB(false)==V::controls::bit::X);assert(mappedB(true)==V::controls::bit::B);
    return 0;
}
