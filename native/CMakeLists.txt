cmake_minimum_required(VERSION 3.22)
project(rdrvr VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Configure with the NDK r27c toolchain file, ANDROID_ABI=arm64-v8a, ANDROID_PLATFORM=android-29 and ANDROID_STL=c++_static. Use BUILD_AND_INSTALL to build the complete package.")
endif()
set(RDRVR_BUILD_ID "" CACHE STRING "Lowercase hex identity of the kit sources")
set(OPENXR_LOADER_SO "" CACHE FILEPATH "Khronos OpenXR loader 1.1.43 for arm64-v8a")
set(RDRVR_GENERATED_DIR "" CACHE PATH "Directory written by tools/gamecode.py, tools/hands_assets.py and tools/build.py")
if(NOT RDRVR_BUILD_ID MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "RDRVR_BUILD_ID must be lowercase hex")
endif()
if(NOT EXISTS "${OPENXR_LOADER_SO}")
  message(FATAL_ERROR "OPENXR_LOADER_SO not found: ${OPENXR_LOADER_SO}")
endif()
foreach(required gamecode vr/HandsAssets.cpp WorkerLayerBuildId.h)
  if(NOT EXISTS "${RDRVR_GENERATED_DIR}/${required}")
    message(FATAL_ERROR "Generated source ${required} is missing from RDRVR_GENERATED_DIR")
  endif()
endforeach()
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES IMPORTED_LOCATION "${OPENXR_LOADER_SO}")
add_library(rdrvr SHARED
  src/diagnostics/Bootstrap.cpp
  src/engine/TraceEntry.cpp
  src/engine/TreeTimerRouting.cpp
  src/engine/UnusedDrawLabel.cpp
  src/engine/NativeFramePacing.cpp
  src/engine/NativeEyeResolution.cpp
  src/engine/NativeFramebufferExtent.cpp
  src/diagnostics/VulkanTrace.cpp
  src/diagnostics/SelfMemoryReader.cpp
  src/engine/GameXr.cpp
  src/runtime/GameVulkanBridge.cpp
  src/runtime/VulkanBootstrap.cpp
  src/runtime/GraphicsSettings.cpp
  src/runtime/XrHarness.cpp
  src/runtime/PerfOverlay.cpp
  src/runtime/ControllerActions.cpp
  src/runtime/GameTheater.cpp
  src/runtime/StereoMailbox.cpp
  src/runtime/HdrTransfer.cpp
  src/runtime/StereoTargets.cpp
  src/runtime/VulkanBridge.cpp
  src/harness/SceneRenderer.cpp
  src/engine/CameraAdapter.cpp
  src/engine/SceneCameraBridge.cpp
  src/engine/StereoVisibility.cpp
  src/engine/EyeViewports.cpp
  src/runtime/GameStereoCapture.cpp
  src/runtime/GameHudCapture.cpp
  src/engine/PassResources.cpp
  src/engine/EngineTargets.cpp
  src/engine/CommandResources.cpp
  src/engine/EngineFramebuffers.cpp
  src/engine/EngineStereoPass.cpp
  src/engine/EngineDepthResolve.cpp
  src/engine/EngineTextureScope.cpp
  src/engine/EngineTextureMap.cpp
  src/engine/TextureCallBoundary.cpp
  src/engine/TargetCallBoundary.cpp
  src/engine/VisibilityCameraBridge.cpp
  src/engine/CullViewport.cpp
  src/engine/CullViewportPool.cpp
  src/engine/EngineGlobalState.cpp
  src/engine/RetailSceneDraw.cpp
  src/engine/RetailResources.cpp
  src/engine/HudResources.cpp
  src/engine/EngineHudPass.cpp
  src/engine/EngineHudProducer.cpp
  src/engine/GameFragmentRate.cpp
  src/engine/DynamicUniformDescriptors.cpp
  src/engine/NativePrimaryRaster.cpp
  src/engine/PlannerReplayCapture.cpp
  src/engine/PlannerReplaySnapshot.cpp
  src/engine/PriorityOcclusionSources.cpp
  src/engine/StaticOcclusionCertificate.cpp
  src/engine/ColdOcclusionRegistry.cpp
  src/engine/NativeOcclusionDrawWitness.cpp
  src/engine/NativeOcclusionVertexCapture.cpp
  src/engine/NativeStaticMatrixWitness.cpp
  src/engine/NativeStaticOcclusionSource.cpp
  src/engine/NativeStaticTriangles.cpp
  src/engine/OwnedStaticVertices.cpp
  src/engine/OwnedStaticOccluders.cpp
  src/engine/StaticStereoOcclusion.cpp
  src/engine/StaticOcclusionProjection.cpp
  src/engine/ConservativeStereoRaster.cpp
  src/stereo/GameMainVisibilityWait.cpp
  src/visibility/CurrentNativeWallSources.cpp
  src/engine/ColdRigidDrawBounds.cpp
  src/visibility/NativeEarlyRuntimeBinding.cpp
  src/visibility/WallHintSelector.cpp
  src/visibility/OwnedWallHintWorker.cpp
  src/engine/ConservativeWallCones.cpp
  src/camera/CameraDrawAdapter.cpp
  src/camera/FrozenCameraCreationRegistry.cpp
  src/camera/FrozenCameraPipeline.cpp
  src/camera/CameraUniformOwner.cpp
  src/camera/FrozenCameraSplit.cpp
  src/camera/CommandUniformArena.cpp
  src/camera/CommandUniformSets.cpp
  src/camera/UniformCaptureSink.cpp
  src/camera/IndexedCommandState.cpp
  src/camera/OwnedIndexedReplay.cpp
  src/stereo/NativeCameraRuntime.cpp
  src/stereo/NativeIndexedObservation.cpp
  src/stereo/OrdinaryVtableObserver.cpp
  src/stereo/OrdinaryPacketCapture.cpp
  src/stereo/OrdinaryDepthReplay.cpp
  src/camera/OwnedDepthRequalification.cpp
  src/stereo/StereoReplay.cpp
  src/stereo/StereoReplayAsync.cpp
  src/stereo/GameFoveation.cpp
  src/vr/HandsPass.cpp
  "${RDRVR_GENERATED_DIR}/vr/HandsAssets.cpp"
  src/vr/TaaPass.cpp
  src/vr/VrHud.cpp
)
target_include_directories(rdrvr PRIVATE
  src/stereo src/camera src/visibility src/engine generated "${RDRVR_GENERATED_DIR}" vendor/openxr/include src)
# runtime/ is searched for quoted includes only, so <math.h> never resolves to runtime/Math.h.
target_compile_options(rdrvr PRIVATE "-iquote${CMAKE_CURRENT_SOURCE_DIR}/src/runtime"
  -Wall -Wextra -Werror -fvisibility=hidden -Wno-missing-field-initializers -march=armv8-a+lse
  "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=native" "-ffile-prefix-map=${RDRVR_GENERATED_DIR}=generated"
  "-ffile-prefix-map=${ANDROID_NDK}=ndk")
set_source_files_properties(src/stereo/OrdinaryVtableObserver.cpp PROPERTIES COMPILE_OPTIONS -mbranch-protection=standard)
target_compile_definitions(rdrvr PRIVATE
  RDRVR_BUILD_ID="${RDRVR_BUILD_ID}"
  RDRVR_BENCH_CONTROL=1 RDRVR_CAMERA_BRIDGE=1 RDRVR_FRAGMENT_RATE=1 RDRVR_GAME_SESSION=1 RDRVR_GAME_XR=1
  RDRVR_HEAD_CAMERA=1 RDRVR_MANUAL_THEATER=1 RDRVR_NATIVE_HUD=1 RDRVR_NATIVE_MENUS=1 RDRVR_PASS_RESOURCES=1
  RDRVR_STEREO_BRIDGE=1 RDRVR_TOUCH_CONTROLS=1 RDRVR_TRACE=1 RDRVR_WORLD_PREVIEW=1
  XR_USE_GRAPHICS_API_VULKAN XR_USE_PLATFORM_ANDROID)
target_link_options(rdrvr PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)
target_link_libraries(rdrvr PRIVATE log dl android openxr_loader vulkan atomic m)
