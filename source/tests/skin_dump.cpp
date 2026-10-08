// skin_dump.cpp - writes the skinned points of a packed mantle for one sequence frame (text: one "x y z" per point)
//   skin_dump <pack> <key> <design> <seq> <frame> <out.txt>
#include "../src/essence.h"
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 7) { printf("usage\n"); return 2; }
    wchar_t w[512]; mbstowcs(w, argv[1], 512);
    ess::Pack pk; std::string err;
    if (!ess::LoadPack(w, pk, &err)) { printf("load: %s\n", err.c_str()); return 1; }
    ess::Anim* a = ess::FindAnim(pk, argv[2]); const ess::Mantle* m = ess::FindMantle(pk, argv[2], atoi(argv[3]));
    if (!a || !m) { printf("not found\n"); return 1; }
    auto it = a->byName.find(ess::Lower(argv[4])); if (it == a->byName.end()) { printf("seq not found\n"); return 1; }
    std::vector<int> amap; ess::MapBones(*a, *m, amap);
    ess::Pose pose; ess::EvalSeq(*a, a->seqs[it->second], (float)atof(argv[5]), pose);
    std::vector<float> pos; ess::SkinPoints(*a, *m, amap, pose, pos);
    FILE* f = fopen(argv[6], "w");
    for (int i = 0; i < m->np; ++i) fprintf(f, "%.4f %.4f %.4f\n", pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]);
    fclose(f);
    printf("%d points, seq %s frame %s root rot/pos in pose: %.1f %.1f %.1f\n", m->np, argv[4], argv[5], pose.p[0], pose.p[1], pose.p[2]);
    return 0;
}
