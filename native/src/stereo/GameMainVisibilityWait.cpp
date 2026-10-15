#include "NativeMainVisibilityRoute.h"
namespace rdrvr::engine {
void gameMainVisibilityWait(MainVisibilityWait original,void* manager,
                           std::uint32_t request,std::uintptr_t returnOffset) {
    experimental::NativeMainVisibilityRoute::wait(original,manager,request,returnOffset);
}
}
