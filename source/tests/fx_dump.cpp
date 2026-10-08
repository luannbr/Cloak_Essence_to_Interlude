// fx_dump.cpp - runs one effect and writes the geometry of one moment (for tools/fx_preview.py)
//   fx_dump <pack> <effect> <seconds> <axis> <out.txt> [camera: back|side|top]
#include "../src/fx.h"
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 6) return 2;
    wchar_t w[512]; mbstowcs(w, argv[1], 512);
    fx::Pack pk; std::string err;
    if (!fx::LoadPack(w, pk, &err)) { printf("load failed: %s\n", err.c_str()); return 1; }
    const int ei = fx::FindEffect(pk, argv[2]);
    if (ei < 0) { printf("no such effect\n"); return 1; }
    const float secs = (float)atof(argv[3]); const int axis = atoi(argv[4]);
    const std::string cam = argc > 6 ? argv[6] : "back";
    fx::Instance in; in.Reset(&pk.effects[(size_t)ei], 777u); fx::Warmup(in, 3.f);
    for (float t = 0.f; t < secs; t += 1.f / 30.f) fx::Step(in, 1.f / 30.f);
    fx::Frame fr; fr.axis = axis;                                        // torso frame = identity: x left, y front, z up
    fx::View vw;                                                         // camera behind the character (it looks along +y)
    if (cam == "back") { vw.right[0] = -1.f; vw.right[1] = 0.f; vw.right[2] = 0.f; vw.up[0] = 0.f; vw.up[1] = 0.f; vw.up[2] = 1.f; vw.fwd[0] = 0.f; vw.fwd[1] = 1.f; vw.fwd[2] = 0.f; }
    else if (cam == "side") { vw.right[0] = 0.f; vw.right[1] = 1.f; vw.right[2] = 0.f; vw.up[0] = 0.f; vw.up[1] = 0.f; vw.up[2] = 1.f; vw.fwd[0] = 1.f; vw.fwd[1] = 0.f; vw.fwd[2] = 0.f; }
    else { vw.right[0] = -1.f; vw.right[1] = 0.f; vw.right[2] = 0.f; vw.up[0] = 0.f; vw.up[1] = 1.f; vw.up[2] = 0.f; vw.fwd[0] = 0.f; vw.fwd[1] = 0.f; vw.fwd[2] = -1.f; }
    std::vector<fx::Vertex> vs; std::vector<uint16_t> is; std::vector<fx::Batch> bs;
    vs.clear(); is.clear(); bs.clear();
    fx::Gather(pk, in, fr, vw, vs, is, bs, 20000, 40000);
    FILE* f = fopen(argv[5], "w");
    for (const fx::Batch& b : bs) {
        fprintf(f, "B %d %d %u %u\n", b.tex, b.style, b.nv, b.ntri);
        for (unsigned i = 0; i < b.nv; ++i) { const fx::Vertex& v = vs[b.v0 + i]; fprintf(f, "V %.3f %.3f %.3f %u %.4f %.4f\n", v.x, v.y, v.z, v.col, v.u, v.v); }
        for (unsigned i = 0; i < b.ntri * 3; ++i) fprintf(f, "I %d\n", (int)is[b.i0 + i] - (int)b.v0);
    }
    fclose(f);
    printf("%zu batches, %zu vertices\n", bs.size(), vs.size());
    return 0;
}
