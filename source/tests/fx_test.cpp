// fx_test.cpp - loads an effect pack, runs every effect for a few seconds and reports the live particles per emitter
//   fx_test <pack> [seconds]
#include "../src/fx.h"
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    wchar_t w[512]; mbstowcs(w, argv[1], 512);
    fx::Pack pk; std::string err;
    if (!fx::LoadPack(w, pk, &err)) { printf("load failed: %s\n", err.c_str()); return 1; }
    const float secs = argc > 2 ? (float)atof(argv[2]) : 6.f;
    printf("%zu textures, %zu meshes, %zu effects\n", pk.texs.size(), pk.meshes.size(), pk.effects.size());
    int bad = 0;
    for (size_t ei = 0; ei < pk.effects.size(); ++ei) {
        fx::Instance in; in.Reset(&pk.effects[ei], 12345u + (uint32_t)ei);
        printf("== %s (%zu emitters)\n", pk.effects[ei].name.c_str(), pk.effects[ei].em.size());
        for (float t = 0.f; t < secs; t += 1.f / 30.f) fx::Step(in, 1.f / 30.f);
        for (size_t k = 0; k < in.eff->em.size(); ++k) {
            const fx::Emitter& e = in.eff->em[k]; int live = 0, vis = 0; float maxSize = 0.f, maxPos = 0.f, maxA = 0.f;
            for (const fx::Particle& q : in.st[k].p) {
                if (!q.alive) continue; ++live;
                fx::Draw d; if (!fx::Eval(e, q, d)) continue; ++vis;
                for (int j = 0; j < 3; ++j) { if (!std::isfinite(d.pos[j]) || !std::isfinite(d.size[j])) ++bad; maxSize = std::max(maxSize, std::fabs(d.size[j])); maxPos = std::max(maxPos, std::fabs(d.pos[j])); }
                maxA = std::max(maxA, d.alpha);
            }
            printf("   %s tex %d mesh %d draw %d max %d | live %d visible %d | max size %.2f max |pos| %.1f max alpha %.2f\n", e.kind ? "mesh  " : "sprite", e.tex, e.mesh, e.draw, e.maxP, live, vis, maxSize, maxPos, maxA);
        }
    }
    printf("%d non-finite values\n", bad);
    return bad ? 1 : 0;
}
