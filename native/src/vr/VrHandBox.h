#pragma once
// ARC: the BODY IK hands of this engine frame, published by VrBody hook A
// (aActor::Broadcast(UpdateAttachments), Update N, post-physics workers; one writer at a time - hook A's busy flag) at the end of its arm
// solve, read by arc::Context::startPoint (main thread, UpdateInputs N) so an arc starts on the hand the eyes see, not on the controller
// (the IK clamps the reach). Header-only, lock-free, no engine access; kept apart from VrBody.h so VrArc.h stays
// light (host tests).
#include "VrBodyLink.h"
#include "VrHandPhysMath.h"
#include <array>
#include <cstdint>

namespace rdrvr::vr::bodyik {
// World units of the engine frame (the skeleton globals). Index = the PHYSICAL VR hand (0 left, 1 right).
struct HandBox {
    std::uint64_t serial{},ns{},referenceSpace{};      // hook-A serial, its clock (vr::nowNs), the LOCAL map's XR reference space
    std::array<std::uint8_t,2> ikDriven{};             // 1 = this hand's arm was solved to a live controller target (full weight)
    std::array<std::array<float,3>,2> grip{};          // the point on the IK'd hand that corresponds to the controller grip origin
    std::array<std::array<float,3>,2> wrist{};         // the IK'd wrist (hand bone) position
    std::array<float,2> clampCm{};                     // how far the IK fell short of the target (reach clamp), cm
    float upm{1};
    // HAND PHYSICS (VrHandPhys.h), written only while the row is not OFF. tipLen = the held item's length from the grip (the muzzle of a firearm).
    std::array<float,2> tipLen{},offsetCm{},trigger{}; // tipLen in metres, 0 = an item of unknown length; offsetCm = the visual stop
    std::array<std::uint8_t,2> cls{},touch{};          // handphys::HandClass; touch = the hand's body is held back by a surface this frame
    std::array<float,3> eye{},up{0,1,0};               // the eye midpoint and up of this frame
    std::uint8_t bridged{};                            // a turn bridge moved this frame's plan
};
inline ::rdrvr::vr::body::Seqlock<HandBox> handBox;
inline std::atomic<std::uint64_t> handBoxPublished{};
} // namespace rdrvr::vr::bodyik
