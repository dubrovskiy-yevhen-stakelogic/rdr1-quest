#pragma once
#include "NativeJoinedListOwner.h"
#include "NativeJoinedListProof.h"
namespace rdrvr::experimental {
// Shared by TraceEntry's exact RenderScene scope and GameXr's synchronous
// color consumer. Only joined list/header/record storage, never asset lifetime.
inline rdr95::NativeJoinedListOwner nativeEarlyListOwner;
}
