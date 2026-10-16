cmake_minimum_required(VERSION 3.22)
project(rdrvr_worker_layer VERSION 0.1.1 LANGUAGES CXX)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
  message(FATAL_ERROR "Configure with the NDK r27c toolchain file and ANDROID_ABI=arm64-v8a. Use BUILD_AND_INSTALL to build the complete package.")
endif()
set(VULKAN_HEADERS_DIR "" CACHE PATH "Khronos Vulkan-Headers vulkan-sdk-1.4.357.0 (contains include/vulkan)")
set(RDRVR_GENERATED_DIR "" CACHE PATH "Directory written by tools/gamecode.py")
if(NOT EXISTS "${VULKAN_HEADERS_DIR}/include/vulkan/vulkan_core.h")
  message(FATAL_ERROR "VULKAN_HEADERS_DIR does not contain include/vulkan/vulkan_core.h")
endif()
if(NOT EXISTS "${RDRVR_GENERATED_DIR}/gamecode")
  message(FATAL_ERROR "RDRVR_GENERATED_DIR does not contain gamecode")
endif()
# The layer is built with exactly these flags; the NDK toolchain defaults are not used.
set(CMAKE_CXX_FLAGS "")
set(CMAKE_CXX_FLAGS_RELEASE "")
set(CMAKE_SHARED_LINKER_FLAGS "")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")
add_library(VkLayer_RDR2_raw_worker28 SHARED
  src/layer/Layer.cpp
  src/layer/GeneratedWrappers.cpp
  src/layer/ChainQualification.cpp
  src/pool/WorkerPool28.cpp
  src/allocator/RetailAllocatorQualification.cpp
)
target_include_directories(VkLayer_RDR2_raw_worker28 PRIVATE "${VULKAN_HEADERS_DIR}/include" "${RDRVR_GENERATED_DIR}")
target_compile_options(VkLayer_RDR2_raw_worker28 PRIVATE -std=c++17 -O2 -fPIC -march=armv8-a+lse -Wall -Wextra -Werror)
target_compile_definitions(VkLayer_RDR2_raw_worker28 PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
target_link_options(VkLayer_RDR2_raw_worker28 PRIVATE -static-libstdc++ -Wl,-Bsymbolic-functions -Wl,--build-id=sha1
  -Wl,--no-undefined -Wl,-z,max-page-size=16384)
target_link_libraries(VkLayer_RDR2_raw_worker28 PRIVATE dl log)
set_target_properties(VkLayer_RDR2_raw_worker28 PROPERTIES OUTPUT_NAME VkLayer_RDR2_raw_worker28)
