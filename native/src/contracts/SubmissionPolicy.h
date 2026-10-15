#pragma once
#include "Contracts.h"
#include <cmath>

namespace rdrvr {
enum class SubmissionCheck { Valid, WrongMode, MixedFrames, ChangedEpoch,
    WrongExtent, InvalidImage, InvalidPose, InvalidFov, MissingCompletion };
inline bool validPose(const Pose& pose) {
    if (!pose.orientationValid || !pose.positionValid) return false;
    float norm = 0;
    for (auto x : pose.orientation) { if (!std::isfinite(x)) return false; norm += x*x; }
    for (auto x : pose.position) if (!std::isfinite(x)) return false;
    return std::abs(norm - 1.f) < 0.001f;
}
// Structural check only. Caller MUST wait/check the GPU token through its owner.
inline SubmissionCheck checkStereo(const RenderSubmission& s, Epochs current,
                                   const std::array<Extent, 2>& expected) {
    if (s.frame.mode != RenderMode::Stereo) return SubmissionCheck::WrongMode;
    if (!s.frame.renderFrameId) return SubmissionCheck::MixedFrames;
    if (!(s.frame.epochs == current)) return SubmissionCheck::ChangedEpoch;
    for (std::size_t i = 0; i != 2; ++i) {
        const auto& eye = s.eyes[i];
        if (eye.renderFrameId != s.frame.renderFrameId) return SubmissionCheck::MixedFrames;
        if (!(eye.epochs == current)) return SubmissionCheck::ChangedEpoch;
        if (!(eye.extent == expected[i]) || !eye.extent.width || !eye.extent.height)
            return SubmissionCheck::WrongExtent;
        if (!eye.image || !eye.lifetimeToken || !eye.format) return SubmissionCheck::InvalidImage;
        if (!validPose(eye.renderedPose)) return SubmissionCheck::InvalidPose;
        const auto f = eye.renderedFov;
        if (!std::isfinite(f.left) || !std::isfinite(f.right) ||
            !std::isfinite(f.up) || !std::isfinite(f.down) ||
            f.left >= f.right || f.down >= f.up ||
            f.left <= -1.5707963f || f.right >= 1.5707963f ||
            f.down <= -1.5707963f || f.up >= 1.5707963f)
            return SubmissionCheck::InvalidFov;
    }
    if (s.eyes[0].image == s.eyes[1].image && s.eyes[0].arrayLayer == s.eyes[1].arrayLayer)
        return SubmissionCheck::InvalidImage;
    if (s.completion.kind == CompletionKind::None || !s.completion.handle)
        return SubmissionCheck::MissingCompletion;
    return SubmissionCheck::Valid;
}
} // namespace rdrvr
