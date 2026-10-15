#pragma once
#include "OrdinarySinglePass.h"
#include <cerrno>
namespace rdr95model {
inline bool modelVisible(const rdr95::OrdinarySinglePass::Description& d,std::uintptr_t expected)noexcept {
    if(!d.modelBounds||!d.modelCull)return true;
    if(!expected||d.modelCull!=expected)return false;
    const int saved=errno;
    const auto result=reinterpret_cast<int(*)(const void*)>(d.modelCull)(d.bounds.data());
    errno=saved;
    return result==1||result==2;
}
}
