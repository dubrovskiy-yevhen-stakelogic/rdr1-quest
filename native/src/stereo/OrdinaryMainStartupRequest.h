#pragma once
namespace rdrvr::engine {
// Compile-time switch for the ordinary depth replay at MAIN startup; off.
inline constexpr bool ordinaryMainStartupRequested()noexcept{return false;}
}
