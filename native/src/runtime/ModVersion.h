#pragma once
// The mod version the headset shows (WELCOME panel, DETAIL readout). The kit's copies must agree: README.md, SOURCE_MANIFEST.json,
// tools/build.py, tools/build-and-install.ps1 / .sh, native + layer CMakeLists.txt (tools/release-audit.ps1 checks the first four).
namespace rdrvr::runtime {
inline constexpr const char* modVersion="0.2.0";
}
