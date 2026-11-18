#pragma once
// A seat of a horse-drawn vehicle (sagPlayer+0x5d4 == 3 on sagVehicleType 7: wagon, carriage, stagecoach): VR weapons work there as on a
// horse, except knife and lasso. Other vehicle types (train, car, boat, mine cart), a mounted gun (the mounted weapon context) and a
// control lock stay refused. Holds no engine calls; engine::WeaponContextReader and the VR modules read it.
// Levers (read once, VrHolsters.h startupEnabled): debug.rdrvr.vr.holsters.seat 0 = no weapons in any seat,
// .seatdriver 0 = none in the driver's seat, .seatgrips 0 = an empty grip stays masked from the game in a seat.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rdrvr::vr::vehicleseat {
// sagVehicleType+0x10, in the order of the VehicleType tune enum.
inline constexpr std::int32_t typeStagecoach=7;
inline constexpr std::array<const char*,11> typeNames{"CAR","MOTORCYCLE","BOAT","SUBMARINE","HELICOPTER","GYROCOPTER","PLANE","STAGECOACH","RAIL","BALLOON","PUSHCART"};
inline const char* typeName(std::int32_t type) noexcept {return type>=0&&type<std::int32_t(typeNames.size())?typeNames[std::size_t(type)]:"NONE";}
// SeatDriver / SeatShotgun = the PilotLocation / WeaponLocation flags of the location set; SeatMoving = a move between seats runs.
enum SeatClass : unsigned {SeatNone,SeatDriver,SeatShotgun,SeatPassenger,SeatMoving,SeatClassCount};
inline constexpr std::array<const char*,SeatClassCount> seatNames{"NONE","DRIVER","SHOTGUN","PASSENGER","MOVING"};
inline const char* seatName(unsigned seat) noexcept {return seat<SeatClassCount?seatNames[seat]:"NONE";}

inline std::atomic<bool> weaponsOn{true},driverOn{true},nativeGrips{true};
inline bool eligible(std::int32_t type,unsigned seat) noexcept {
    if(type!=typeStagecoach||!weaponsOn.load(std::memory_order_relaxed))return false;
    if(seat==SeatDriver)return driverOn.load(std::memory_order_relaxed);
    return seat==SeatShotgun||seat==SeatPassenger||seat==SeatMoving;
}

// Published by the holsters poll (main thread), read on any thread. weaponSeatNow: the player sits in an eligible seat and the holsters are on.
inline std::atomic<bool> weaponSeatNow{},seatSwap{};
inline std::atomic<std::int32_t> playerState{-1},vehicleType{-1},seatIndex{-1};
inline std::atomic<unsigned> seatClass{SeatNone};
inline std::atomic<std::uint64_t> seatPolls{},throwsRefused{};
// The player facts that close a seat after the seat itself passed, for the report (bit order of blockNames).
inline constexpr std::array<const char*,9> blockNames{"locked","mountedWeapon","linkedAnimation","blindFiring","cover","duel","gringo","mountingHorse","vehicleTransition"};
inline std::atomic<unsigned> seatBlocks{};
inline void publish(bool weapons,std::int32_t state,std::int32_t type,unsigned seat,std::int32_t index,bool swap) noexcept {
    playerState.store(state,std::memory_order_relaxed);vehicleType.store(type,std::memory_order_relaxed);seatClass.store(seat,std::memory_order_relaxed);
    seatIndex.store(index,std::memory_order_relaxed);seatSwap.store(swap,std::memory_order_relaxed);weaponSeatNow.store(weapons,std::memory_order_relaxed);
    if(state==3)seatPolls.fetch_add(1,std::memory_order_relaxed);
}
} // namespace rdrvr::vr::vehicleseat
