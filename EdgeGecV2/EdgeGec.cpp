#define EDGE_GEC_BUILD
#include "EdgeGecApi.h"
#include "../NaturalVoiceSAPIAdapter/EdgeGec.h"
extern "C" int __cdecl HdGecGenerate(uint64_t fileTime, char* output, size_t size) {
    if (!output || size < 82) return 1;
    EdgeGec::Generate(fileTime, output);
    return 0;
}
