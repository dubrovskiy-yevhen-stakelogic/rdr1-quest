# Third-party notices

- **OpenXR headers** (`native/vendor/openxr/`): Khronos OpenXR SDK 1.1.43,
  Apache License 2.0 or MIT. License: `native/vendor/openxr/LICENSE`.
- **OpenXR Android loader**: Khronos
  `org.khronos.openxr:openxr_loader_for_android:1.1.43`, Apache License 2.0.
  Downloaded and verified at build time.
- **Vulkan-Headers**: Khronos Vulkan-Headers `vulkan-sdk-1.4.357.0`, Apache
  License 2.0 or MIT. Downloaded and verified at build time for the layer
  build; the layer dispatch tables are generated from its registry.
- **VR hand assets** (`assets/vrhands/`): derived from the UltimateXR SDK,
  copyright (c) 2022 VRMADA, MIT License. Source attribution and license are
  beside the assets.
- **FXAA** (`native/src/runtime/shaders/fxaa.frag`): based on NVIDIA FXAA 3.11
  by Timothy Lottes, copyright (c) 2011 NVIDIA Corporation. The notice is kept
  in the shader and in `assets/licenses/NVIDIA-FXAA-3.11.txt`.
- **MiamiVR Quest port**: the theater anchoring in
  `native/src/runtime/XrHarness.cpp` is adapted from the MiamiVR Quest port,
  MIT License (`assets/licenses/MiamiVR-Quest-LICENSE.txt`).
- **LLVM libc++**: linked statically from the Android NDK, Apache License 2.0
  with LLVM exceptions.

Game APKs, Rockstar code and assets, extracted game libraries and signing keys
are not included. Engine verification data is read from the player's own game
package at build time.
