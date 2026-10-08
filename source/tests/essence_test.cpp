// Unit test: load the pack, evaluate a sequence frame, skin the mantle and print a few positions + checksum.
// The Python side (tools/essence_check.py) computes the same values independently from the original ukx.
#include "../src/essence.h"
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: essence_test <pack> <body> <seqname> <frame>\n"); return 2; }
    wchar_t wpath[512]; mbstowcs(wpath, argv[1], 512);
    ess::Pack pk; std::string err;
    if (!ess::LoadPack(wpath, pk, &err)) { printf("LOAD FAILED: %s\n", err.c_str()); return 1; }
    printf("pack: %zu anims, %zu textures, %zu mantles\n", pk.anims.size(), pk.texs.size(), pk.mantles.size());
    ess::Anim* a = ess::FindAnim(pk, argv[2]);
    const ess::Mantle* m = ess::FindMantle(pk, argv[2], 0);
    if (!a || !m) { printf("body not found\n"); return 1; }
    auto it = a->byName.find(ess::Lower(argv[3]));
    if (it == a->byName.end()) { printf("sequence not found\n"); return 1; }
    const ess::Seq& s = a->seqs[it->second];
    std::vector<int> amap; ess::MapBones(*a, *m, amap);
    ess::Pose pose; ess::EvalSeq(*a, s, (float)atof(argv[4]), pose);
    std::vector<float> pos; ess::SkinPoints(*a, *m, amap, pose, pos);
    printf("seq %s frames %d rate %.1f; mantle %s %d points %d wedges %d faces\n", s.name.c_str(), s.frames, s.rate, m->meshName.c_str(), m->np, m->nw, m->nf);
    double sx = 0, sy = 0, sz = 0;
    for (int i = 0; i < m->np; ++i) { sx += pos[i * 3]; sy += pos[i * 3 + 1]; sz += pos[i * 3 + 2]; }
    printf("sum %.4f %.4f %.4f\n", sx, sy, sz);
    for (int i : { 0, 1, 100, 1000, 2000, 2923 }) printf("p%d %.4f %.4f %.4f\n", i, pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]);
    return 0;
}
