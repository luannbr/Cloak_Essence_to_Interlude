// wing_test.cpp - skins a packed mantle design at several frames of a sequence and prints the bounding box (sanity check of the wing / ranker designs).
//   wing_test <pack> <key> <design> <seq>
#include "../src/essence.h"
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: wing_test <pack> <key> <design> <seq>\n"); return 2; }
    wchar_t wpath[512]; mbstowcs(wpath, argv[1], 512);
    ess::Pack pk; std::string err;
    if (!ess::LoadPack(wpath, pk, &err)) { printf("LOAD FAILED: %s\n", err.c_str()); return 1; }
    printf("pack: %zu anims, %zu textures, %zu mantles\n", pk.anims.size(), pk.texs.size(), pk.mantles.size());
    ess::Anim* a = ess::FindAnim(pk, argv[2]);
    const ess::Mantle* m = ess::FindMantle(pk, argv[2], atoi(argv[3]));
    if (!a || !m) { printf("not found (anim %p mantle %p)\n", (void*)a, (const void*)m); return 1; }
    auto it = a->byName.find(ess::Lower(argv[4]));
    if (it == a->byName.end()) { printf("sequence '%s' not found; sequences:", argv[4]); for (size_t i = 0; i < a->seqs.size() && i < 12; ++i) printf(" %s", a->seqs[i].name.c_str()); printf("\n"); return 1; }
    const ess::Seq& s = a->seqs[it->second];
    std::vector<int> amap; ess::MapBones(*a, *m, amap);
    int mapped = 0; for (int v : amap) if (v >= 0) ++mapped;
    printf("mantle %s: %d points, %d faces, %zu bones (%d mapped to the animation) | seq %s %d frames %.0f fps\n", m->meshName.c_str(), m->np, m->nf, amap.size(), mapped, s.name.c_str(), s.frames, s.rate);
    for (int k = 0; k < 4; ++k) {
        const float fr = s.frames > 1 ? (float)(s.frames - 1) * (float)k / 3.f : 0.f;
        ess::Pose pose; ess::EvalSeq(*a, s, fr, pose);
        std::vector<float> pos; ess::SkinPoints(*a, *m, amap, pose, pos);
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
        for (int i = 0; i < m->np; ++i) for (int c = 0; c < 3; ++c) { mn[c] = std::min(mn[c], pos[(size_t)i * 3 + c]); mx[c] = std::max(mx[c], pos[(size_t)i * 3 + c]); }
        printf("frame %5.1f: x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f]\n", fr, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    }
    return 0;
}
