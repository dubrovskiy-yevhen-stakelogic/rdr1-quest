#pragma once
#include <vulkan/vulkan.h>

namespace rdr2::worker28::retail_allocator {
// The first successful Android ARM64 admission performs full module/file/maps,
// code and GOT checks. Later calls compare callbacks and the retained code/GOT
// bytes through an immutable certificate without syscalls or locks.
// Other platforms always decline custom allocators. The caller
// must still preserve Vulkan allocation-callback lifetime/create-destroy rules.
// No callbacks are invoked and pUserData is not dereferenced. Success retains
// bounded loader references to the one pinned engine and its libc module.
bool qualify(const VkAllocationCallbacks&) noexcept;
}
