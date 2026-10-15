#pragma once
#include <string>
#include <vector>
namespace rdrvr::engine {
void traceConfigure(const char* path);
void traceVerified(const char* path,const char* sha);
std::string traceReport();
#ifdef RDRVR_BENCH_CONTROL
bool traceEnableShaderCapture();
std::vector<unsigned char> traceTakeShader();
#endif
} // namespace rdrvr::engine
