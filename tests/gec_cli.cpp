#include "../EdgeGecV2/EdgeGecApi.h"
#include <stdlib.h>
#include <stdio.h>
int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        char token[82];
        if (HdGecGenerate(strtoull(argv[i], nullptr, 10), token, sizeof(token))) return 2;
        puts(token);
    }
}
