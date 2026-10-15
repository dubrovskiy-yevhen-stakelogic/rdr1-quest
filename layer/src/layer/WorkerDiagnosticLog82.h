#pragma once
#include <cerrno>
#include <cstdarg>
#include <cstdio>

namespace rdr2::layer28 {
using DiagnosticWriter82 = int (*)(int, const char*, const char*);
inline constexpr unsigned DiagnosticMessageCapacity82 = 3072;

#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
inline int writeDiagnostic82(DiagnosticWriter82 writer, int priority, const char* tag, const char* format, ...) noexcept {
 const int incoming = errno;
 if (!writer || !tag || !format) { errno = incoming; return -1; }
 char message[DiagnosticMessageCapacity82];
 va_list arguments;
 va_start(arguments, format);
 const int length = std::vsnprintf(message, sizeof(message), format, arguments);
 va_end(arguments);
 const char* output = length < 0 ? "logTransportError=format" :
  static_cast<unsigned>(length) >= sizeof(message) ? "logTransportError=overflow" : message;
 const int result = writer(priority, tag, output);
 errno = incoming;
 return result;
}
}
