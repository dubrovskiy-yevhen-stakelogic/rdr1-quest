#pragma once
#include <cmath>
#include <ostream>
namespace rdrvr::diagnostics {
// Raw native call arguments may contain NaN/Inf. JSON has no such numbers;
// preserve unavailable evidence as null instead of corrupting the whole report.
struct JsonNumber {double value;};
inline std::ostream& operator<<(std::ostream& out,JsonNumber number){
    if(std::isfinite(number.value))return out<<number.value;
    return out<<"null";
}
}
