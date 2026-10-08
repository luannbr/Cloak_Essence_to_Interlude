// fx_layers.cpp - lists the shine / animation layers of the cloth textures of a pack.   fx_layers <essence_cloth.bin> [substring]
#include "../src/clothpack.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    wchar_t w[512]; mbstowcs(w, argv[1], 512);
    ess::ClothPack cp; std::string err;
    if (!ess::LoadClothPack(w, cp, &err)) { printf("cloth: %s\n", err.c_str()); return 1; }
    for (const auto& kv : cp.fx) {
        if (argc > 2 && kv.first.find(argv[2]) == std::string::npos) continue;
        printf("%s: %d layers\n", kv.first.c_str(), (int)kv.second.size());
        for (const auto& L : kv.second) {
            printf("  layer kind %d:", L.kind);
            for (const auto& x : L.texs) printf(" [%s env %d pan %.2f %.2f]", x.tex >= 0 && x.tex < (int)cp.texs.size() ? cp.texs[(size_t)x.tex].name.c_str() : "?", (int)x.env, x.panU, x.panV);
            printf("\n");
        }
    }
    return 0;
}
