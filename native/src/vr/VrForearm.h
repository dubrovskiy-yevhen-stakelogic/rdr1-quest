#pragma once
// The LEFT forearm of the IK'd body expressed in the left controller GRIP frame (metres, grip axes), published by VrBody after
// its arm solve (main thread) and read by the HUD wrist quad (XR thread): the wrist map then sits on the drawn forearm (which twists
// only by the body's roll share and does not follow wrist flexion) instead of being rigid on the controller.
//   elbow / wrist  bone positions in the grip frame
//   roll           the wristroll bone's rotation in the grip frame (row-major 3x3, columns = bone axes): the forearm skin near the wrist
//                  (twists by the body's roll share); the HUD picks the bone axis perpendicular to the forearm nearest the palm side.
// Header-only, no engine or XR includes.
#include <array>
#include <cstdint>
#include <mutex>

namespace rdrvr::vr::forearm {
struct InGrip {std::array<float,3> elbow{},wrist{};std::array<float,9> roll{1,0,0,0,1,0,0,0,1};bool haveRoll{};std::uint64_t ns{};bool valid{};};
inline std::mutex mutex;inline InGrip latest{};
inline void publish(const InGrip& v) noexcept {std::lock_guard lock(mutex);latest=v;}
inline InGrip snapshot() noexcept {std::lock_guard lock(mutex);return latest;}
}
