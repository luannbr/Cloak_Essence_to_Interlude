// tex_dump.cpp - writes textures of the cloth pack as .dds files; with only the pack: lists the clan cloak looks and their texture indices.
//   tex_dump <essence_cloth.bin> [texIndex out.dds]
#include "../src/clothpack.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    wchar_t w[512]; mbstowcs(w, argv[1], 512);
    ess::ClothPack cp; std::string err;
    if (!ess::LoadClothPack(w, cp, &err)) { printf("cloth: %s\n", err.c_str()); return 1; }
    if (argc < 4) {
        for (const auto& l : cp.looks) if (l.kind == 'C') printf("%-36s item %d tex %d (%s) crest %d (%s)\n", l.name.c_str(), l.itemId, l.tex, cp.texs[(size_t)l.tex].name.c_str(), l.crestTex, l.crestTex >= 0 ? cp.texs[(size_t)l.crestTex].name.c_str() : "-");
        return 0;
    }
    const ess::Tex& t = cp.texs[(size_t)atoi(argv[2])];
    FILE* f = fopen(argv[3], "wb");
    unsigned h[32] = {};
    h[0] = 0x20534444; h[1] = 124; h[2] = 0x1007; h[3] = t.h; h[4] = t.w; h[5] = t.size[0]; h[19] = 32; h[20] = 4; h[21] = t.fmt == 3 ? 0x31545844 : t.fmt == 7 ? 0x33545844 : 0x35545844; h[27] = 0x1000;
    unsigned hdr[32]; memcpy(hdr, h, sizeof hdr);
    fwrite(hdr, 4, 32, f);
    fwrite(t.data[0], 1, t.size[0], f);
    fclose(f);
    printf("%s %ux%u fmt %u size %u\n", t.name.c_str(), t.w, t.h, t.fmt, t.size[0]);
    return 0;
}
