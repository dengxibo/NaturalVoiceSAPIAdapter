#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef EDGE_GEC_BUILD
#define EDGE_GEC_API __declspec(dllexport)
#else
#define EDGE_GEC_API __declspec(dllimport)
#endif
// Replaceable LGPL-3.0 component. The adapter uses only this C ABI.
extern "C" EDGE_GEC_API int __cdecl HdGecGenerate(uint64_t fileTime, char* output, size_t size);
