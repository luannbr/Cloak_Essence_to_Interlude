// shape_dump.cpp - writes the cloth rest pose and a collar of the pack to a text file (plotted by tools/shape_plot.py).
//   shape_dump <essence_cloth.bin> <Body> <H|R|C> <collarFamily> <out.txt>
#include "../src/clothwiden.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 6) { printf("usage\n"); return 2; }
    wchar_t w[512]; mbstowcs(w, argv[1], 512);
    ess::ClothPack cp; std::string err;
    if (!ess::LoadClothPack(w, cp, &err)) { printf("cloth: %s\n", err.c_str()); return 1; }
    const cloth::Set* s = ess::FindClothSet(cp, argv[2], argv[3][0]); const ess::Collar* c = ess::FindCollar(cp, argv[2], argv[4]);
    if (!s) { printf("no set\n"); return 1; }
    FILE* o = fopen(argv[5], "w");
    fprintf(o, "CLOTH %d %d %d\n", s->np, s->width, s->nt);
    for (int i = 0; i < s->np; ++i) fprintf(o, "%.3f %.3f %.3f\n", s->rest[(size_t)i * 3], s->rest[(size_t)i * 3 + 1], s->rest[(size_t)i * 3 + 2]);
    for (int t = 0; t < s->nt; ++t) fprintf(o, "%d %d %d\n", s->tris[(size_t)t * 3], s->tris[(size_t)t * 3 + 1], s->tris[(size_t)t * 3 + 2]);
    if (c) {
        fprintf(o, "COLLAR %d %d\n", c->nv, c->nt);
        for (int i = 0; i < c->nv; ++i) fprintf(o, "%.3f %.3f %.3f\n", c->pos[(size_t)i * 3], c->pos[(size_t)i * 3 + 1], c->pos[(size_t)i * 3 + 2]);
        for (int t = 0; t < c->nt; ++t) fprintf(o, "%d %d %d\n", c->idx[(size_t)t * 3], c->idx[(size_t)t * 3 + 1], c->idx[(size_t)t * 3 + 2]);
    }
    fclose(o);
    printf("ok %d cloth particles, collar %s\n", s->np, c ? "yes" : "no");
    return 0;
}
