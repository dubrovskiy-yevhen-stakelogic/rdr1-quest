#pragma once
#include <vulkan/vulkan.h>
namespace rdr2::layer28 {
// Cold check of the ACTUAL next-layer dispatch entry, not ppEnabledLayerNames.
// Rejects environment-injected layers below this layer. Known producer-side
// validation remains above it; the next function must belong to system loader.
bool qualifiedTerminator(PFN_vkVoidFunction)noexcept;
}
