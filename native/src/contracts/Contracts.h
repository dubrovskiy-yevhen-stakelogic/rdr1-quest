#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace rdrvr {
inline constexpr std::uint32_t kContractVersion = 1;
enum class Evidence { Unknown, Static, RuntimeObserved, HookValidated };
struct GameBuildIdentity {
    std::string_view apkSha256, engineSha256, signerSha256;
    std::string_view packageName, versionName, abi;
    std::uint64_t versionCode{};
    Evidence evidence{Evidence::Unknown};
};
enum class GameContext { Unknown, Loading, Menu, Foot, Horse, Vehicle, Aim,
                         Duel, DeadEye, Scripted, Cutscene };
enum class RenderMode { NoOp, Quad, Stereo, Theater };
struct Epochs {
    std::uint64_t tracking{}, referenceSpace{}, resources{};
    bool operator==(const Epochs&) const = default;
};
struct Pose {
    std::array<float, 4> orientation{0, 0, 0, 1};
    std::array<float, 3> position{};
    bool orientationValid{}, positionValid{};
};
struct Fov { float left{}, right{}, up{}, down{}; }; // radians
struct Extent {
    std::uint32_t width{}, height{};
    bool operator==(const Extent&) const = default;
};
struct FrameContext {
    std::uint64_t gameTickId{}, renderFrameId{}, xrFrameId{};
    std::int64_t predictedDisplayTime{}; // XrTime, not wall-clock time
    Epochs epochs{};
    Pose head{};
    RenderMode mode{RenderMode::NoOp};
};
struct ViewState {
    std::array<Pose, 2> eyes{};
    std::array<Fov, 2> fov{};
    std::array<std::array<float, 16>, 2> view{}, projection{};
    float ipdMetres{}, gameUnitsPerMetre{}; // zero means not calibrated
};
struct EyeImage {
    std::uint64_t renderFrameId{}, image{}, lifetimeToken{};
    Epochs epochs{};
    Pose renderedPose{};
    Fov renderedFov{};
    Extent extent{};
    std::int64_t format{};
    std::uint32_t layout{}, queueFamily{}, arrayLayer{};
};
enum class CompletionKind { None, Fence, TimelineSemaphore };
struct GpuCompletion {
    CompletionKind kind{CompletionKind::None};
    std::uint64_t handle{}, value{};
};
struct RenderSubmission {
    FrameContext frame{};
    std::array<EyeImage, 2> eyes{};
    GpuCompletion completion{};
};
struct InputSnapshot {
    std::uint64_t gameTickId{}, held{};
    std::array<float, 4> axes{};
    std::array<Pose, 2> hands{};
    bool focused{}, trackingValid{};
};
struct ActionFrame {
    std::uint64_t gameTickId{}, held{}, pressed{}, released{}, consumed{};
    GameContext owner{GameContext::Unknown};
    bool neutral{true};
};
struct ActionCommand { std::uint64_t gameTickId{}, action{}; GameContext owner{}; };
} // namespace rdrvr
