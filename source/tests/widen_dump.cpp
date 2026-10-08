// widen_dump.cpp - prints, for every cloth look, the rest-pose half-width of the cloth rows, the collar half-width near the top and the per-row stretch the hook applies.
//   widen_dump <essence_cloth.bin> [Body]
#include "../src/clothwiden.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage\n"); return 2; }
    wchar_t w[512]; mbstowcs(w, argv[1], 512);
    ess::ClothPack cp; std::string err;
    if (!ess::LoadClothPack(w, cp, &err)) { printf("cloth: %s\n", err.c_str()); return 1; }
    const std::string only = argc > 2 ? ess::Lower(argv[2]) : "";
    if (argc > 3) { for (const auto& l : cp.looks) printf("look %-34s kind %c family %-14s item %d tex %d collarTex %d\n", l.name.c_str(), l.kind, l.family.c_str(), l.itemId, l.tex, l.collarTex); return 0; }
    for (const auto& s : cp.sets) {
        if (!only.empty() && ess::Lower(s.body) != only) continue;
        const int rows = s.width > 0 ? s.np / s.width : 0;
        printf("set %s %s: %d particles, width %d, rows %d\n", s.body.c_str(), s.kind.c_str(), s.np, s.width, rows);
        std::vector<float> half((size_t)rows, 0.f), zr((size_t)rows, 0.f), yr((size_t)rows, 0.f);
        for (int r = 0; r < rows; ++r) for (int j = 0; j < s.width; ++j) {
            const int i = r * s.width + j;
            half[(size_t)r] = std::max(half[(size_t)r], std::fabs(s.rest[(size_t)i * 3])); zr[(size_t)r] += s.rest[(size_t)i * 3 + 2] / (float)s.width; yr[(size_t)r] += s.rest[(size_t)i * 3 + 1] / (float)s.width;
        }
        printf("  rows (z, y, half): ");
        for (int r = 0; r < rows; ++r) printf("[%.1f %.1f %.1f] ", zr[(size_t)r], yr[(size_t)r], half[(size_t)r]);
        printf("\n");
        std::vector<std::string> fams;
        for (const auto& l : cp.looks) if (l.kind == s.kind[0] && std::find(fams.begin(), fams.end(), l.family) == fams.end()) fams.push_back(l.family);
        for (const auto& fam : fams) {
            const ess::Collar* c = ess::FindCollar(cp, s.body, fam);
            if (!c) continue;
            float zmaxC = -1e9f, zminC = 1e9f, xmax = 0.f;
            for (int i = 0; i < c->nv; ++i) { zmaxC = std::max(zmaxC, c->pos[(size_t)i * 3 + 2]); zminC = std::min(zminC, c->pos[(size_t)i * 3 + 2]); xmax = std::max(xmax, std::fabs(c->pos[(size_t)i * 3])); }
            std::vector<float> sc; ess::ClothComputeWiden(s, c, 1.f, sc);
            printf("  collar %-10s z %.1f..%.1f, max |x| %.1f | target half-width by row (mean, 0 = unchanged): ", fam.c_str(), zminC, zmaxC, xmax);
            for (int r = 0; r < rows; ++r) { float m = 0.f; for (int j = 0; j < s.width; ++j) m += sc[(size_t)(r * s.width + j)] / (float)s.width; printf("%.2f ", m); }
            printf("\n");
        }
    }
    return 0;
}
