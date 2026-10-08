// guide_test.cpp - runs the cloth solver with the guide from the baked cape on the REAL baked animation of the body (idle -> sequence), no synthetic motion.
//   guide_test <essence_capes.bin> <essence_cloth.bin> <Body> <H|R|C> <sequence> <guideK> <out.txt> [dumpTarget]
// The cape root is replaced by the baked Spine2 itself (the engine would give its own; here both are the Essence ones).
#include "../src/clothguide.h"
#include "../src/clothpack.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 8) { printf("usage\n"); return 2; }
    wchar_t w1[512], w2[512]; mbstowcs(w1, argv[1], 512); mbstowcs(w2, argv[2], 512);
    ess::Pack pk; ess::ClothPack cp; std::string err;
    if (!ess::LoadPack(w1, pk, &err)) { printf("pack: %s\n", err.c_str()); return 1; }
    if (!ess::LoadClothPack(w2, cp, &err)) { printf("cloth: %s\n", err.c_str()); return 1; }
    ess::Anim* a = ess::FindAnim(pk, argv[3]); const ess::Mantle* m = ess::FindMantle(pk, argv[3], 0); const cloth::Set* s = ess::FindClothSet(cp, argv[3], argv[4][0]);
    if (!a || !m || !s) { printf("missing anim/mantle/set\n"); return 1; }
    std::vector<int> amap; ess::MapBones(*a, *m, amap);
    std::vector<float> idleL; if (!guide::IdleLocal(*a, *m, amap, idleL)) { printf("no idle\n"); return 1; }
    guide::Map g; guide::Build(*s, *m, amap, idleL, g);
    auto itw = a->byName.find(ess::Lower(std::string("wait_1hs_") + argv[3])); auto its = a->byName.find(ess::Lower(argv[5]));
    if (itw == a->byName.end() || its == a->byName.end()) { printf("sequence not found\n"); return 1; }
    const float gk = (float)atof(argv[6]);
    const int jr = guide::RootMeshBone(amap);
    cloth::Params pr; pr.wind[1] = -9.f;
    cloth::State st; cloth::Pose po;
    const ess::Seq& sq = a->seqs[its->second]; const ess::Seq& sw = a->seqs[itw->second];
    ess::Pose idle; ess::EvalSeq(*a, sw, 0.f, idle);
    float Rref[9]; ess::QuatToMat(&idle.q[0], Rref); const float RrT[9] = { Rref[0], Rref[3], Rref[6], Rref[1], Rref[4], Rref[7], Rref[2], Rref[5], Rref[8] };
    for (int k = 0; k < 3; ++k) po.tb[k] = m->bindT[(size_t)jr * 3 + k];
    FILE* o = fopen(argv[7], "w");
    const float dt = 1.f / 60.f; const float T = 6.f; const int N = (int)(T / dt);
    float peak = 0.f;
    for (int f = 0; f < N; ++f) {
        const float t = f * dt;
        const bool moving = t >= 1.5f;
        const ess::Seq& use = moving ? sq : sw;
        const float fps = use.rate > 1.f ? use.rate : 30.f;
        const float fr = use.frames > 1 ? std::fmod((t - (moving ? 1.5f : 0.f)) * fps, (float)(use.frames - 1)) : 0.f;
        ess::Pose cur; ess::EvalSeq(*a, use, fr, cur);
        float R0[9]; ess::QuatToMat(&cur.q[0], R0); ess::Mul33(R0, RrT, po.Rs);
        for (int k = 0; k < 3; ++k) po.gt[k] = cur.p[k];
        pr.wind[1] = moving ? -20.f : -9.f; pr.guide = nullptr; pr.guideK = 0.f;
        if (gk > 0.f && moving) { guide::Update(*s, *a, *m, amap, idleL, cur, po, g); pr.guide = g.target.data(); pr.guideK = gk; }
        // colliders: spine follows the torso, legs fixed (the real ones come from the engine)
        cloth::CapsuleWorld caps[5];
        { float v[3] = { 0.f, 0.5f, 0.f }, wv[3]; ess::MulVec(po.Rs, v, wv); for (int k = 0; k < 3; ++k) caps[0].a[k] = wv[k] + po.gt[k]; float v2[3] = { 0.f, 0.5f, -7.f }; ess::MulVec(po.Rs, v2, wv); for (int k = 0; k < 3; ++k) caps[0].b[k] = wv[k] + po.gt[k]; caps[0].r = 6.f; caps[0].valid = true; caps[0].backBias = true; }
        const float lx[2] = { -3.5f, 3.5f };
        for (int l = 0; l < 2; ++l) { auto& th = caps[1 + l * 2]; auto& ca = caps[2 + l * 2]; th.a[0] = lx[l]; th.a[1] = 0.f; th.a[2] = 27.f; th.b[0] = lx[l]; th.b[1] = 0.f; th.b[2] = 14.f; ca.a[0] = lx[l]; ca.a[1] = 0.f; ca.a[2] = 14.f; ca.b[0] = lx[l]; ca.b[1] = 0.f; ca.b[2] = 2.f; th.r = 4.f; ca.r = 3.f; th.valid = ca.valid = true; }
        cloth::Step(*s, st, pr, po, caps, 5, dt);
        if (t > 3.f) for (int i = 0; i < s->np; ++i) if (!s->isAnchor[(size_t)i]) { float v = std::sqrt(std::pow(st.p[i * 3] - st.q[i * 3], 2) + std::pow(st.p[i * 3 + 1] - st.q[i * 3 + 1], 2) + std::pow(st.p[i * 3 + 2] - st.q[i * 3 + 2], 2)) * 90.f; peak = std::max(peak, v); }
        if (f % 12 == 0 || f == N - 1) {
            fprintf(o, "frame %d t %.2f\n", f, t);
            for (int i = 0; i < s->np; ++i) fprintf(o, "%.3f %.3f %.3f\n", st.p[i * 3], st.p[i * 3 + 1], st.p[i * 3 + 2]);
        }
    }
    fclose(o);
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < s->np; ++i) for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], st.p[i * 3 + k]); mx[k] = std::max(mx[k], st.p[i * 3 + k]); }
    printf("%s %s guide %.2f: peak particle speed after 3 s %.0f u/s | final actor extents x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f]\n", argv[3], argv[5], gk, peak, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    return 0;
}
