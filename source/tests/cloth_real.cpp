// cloth_real.cpp - runs the cloth on the REAL root motion of an Essence animation (the baked Spine2 track of <Body>_cape_anim) to look for instabilities.
//   cloth_real <essence_capes.bin> <essence_cloth.bin> <Body> <H|R> <sequence> <cycles> <windY> <out.txt>
#include "../src/clothpack.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 9) { printf("usage\n"); return 2; }
    wchar_t w1[512], w2[512]; mbstowcs(w1, argv[1], 512); mbstowcs(w2, argv[2], 512);
    ess::Pack pk; ess::ClothPack cp; std::string err;
    if (!ess::LoadPack(w1, pk, &err)) { printf("pack: %s\n", err.c_str()); return 1; }
    if (!ess::LoadClothPack(w2, cp, &err)) { printf("cloth: %s\n", err.c_str()); return 1; }
    ess::Anim* a = ess::FindAnim(pk, argv[3]); const cloth::Set* s = ess::FindClothSet(cp, argv[3], argv[4][0]);
    if (!a || !s) { printf("anim or set missing\n"); return 1; }
    auto find = [&](const char* n) { auto it = a->byName.find(ess::Lower(n)); return it == a->byName.end() ? -1 : it->second; };
    const int si = find(argv[5]), wi = find((std::string("wait_1hs_") + ess::Lower(argv[3])).c_str());
    if (si < 0 || wi < 0) { printf("sequence missing (%d %d)\n", si, wi); return 1; }
    const int cycles = atoi(argv[6]); const float windY = (float)atof(argv[7]);
    // reference: the idle root orientation
    ess::Pose p0; ess::EvalSeq(*a, a->seqs[wi], 0.f, p0);
    float Rref[9]; ess::QuatToMat(&p0.q[0], Rref);
    const float RrT[9] = { Rref[0], Rref[3], Rref[6], Rref[1], Rref[4], Rref[7], Rref[2], Rref[5], Rref[8] };
    const float tb[3] = { 0.f, 0.5f, 32.4f };
    cloth::Params pr; pr.wind[1] = windY; cloth::State st; cloth::Pose po;
    for (int k = 0; k < 3; ++k) po.tb[k] = tb[k];
    const ess::Seq& sq = a->seqs[si];
    const float fps = sq.rate > 1.f ? sq.rate : 30.f; const float dt = 1.f / 60.f;
    const int N = (int)(cycles * sq.frames / fps / dt);
    FILE* o = fopen(argv[8], "w");
    float worstStretch = 0.f; int worstFrame = -1;
    for (int f = 0; f < N; ++f) {
        const float fr = std::fmod(f * dt * fps, (float)(sq.frames > 1 ? sq.frames - 1 : 1));
        ess::Pose pose; ess::EvalSeq(*a, sq, fr, pose);
        float R0[9]; ess::QuatToMat(&pose.q[0], R0);
        ess::Mul33(R0, RrT, po.Rs);
        po.gt[0] = pose.p[0]; po.gt[1] = pose.p[1]; po.gt[2] = pose.p[2];
        // capsules: torso follows the root; legs fixed
        cloth::CapsuleWorld caps[5]; float dn[3] = { 0.f, 0.f, -7.f }, dd[3];
        ess::MulVec(po.Rs, dn, dd);
        for (int k = 0; k < 3; ++k) { caps[0].a[k] = po.gt[k]; caps[0].b[k] = po.gt[k] + dd[k]; }
        caps[0].r = 6.f; caps[0].valid = true;
        if (argc > 9 && atof(argv[9]) > 0.1) {                                    // thinner torso capsule below the anchors (same rule as the hook)
            float zr = 0.f, dsum = 0.f; int na = 0;
            for (uint16_t ai : s->anchors) {
                const float v[3] = { s->rest[ai * 3] - po.tb[0], s->rest[ai * 3 + 1] - po.tb[1], s->rest[ai * 3 + 2] - po.tb[2] }; float o[3]; cloth::MulVec(po.Rs, v, o);
                const float w[3] = { o[0] + po.gt[0], o[1] + po.gt[1], o[2] + po.gt[2] };
                const float ab[3] = { caps[0].b[0] - caps[0].a[0], caps[0].b[1] - caps[0].a[1], caps[0].b[2] - caps[0].a[2] }, ap[3] = { w[0] - caps[0].a[0], w[1] - caps[0].a[1], w[2] - caps[0].a[2] };
                const float t0 = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / (ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2]); 
                const float d[3] = { ap[0] - ab[0] * t0, ap[1] - ab[1] * t0, ap[2] - ab[2] * t0 };
                dsum += std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); zr += w[2]; ++na;
            }
            const float rt = dsum / na - pr.skin + 0.15f;
            if (rt > caps[0].r * 0.5f && rt < caps[0].r) { caps[0].rTop = rt; caps[0].zRef = zr / na; caps[0].ramp = (float)atof(argv[9]); }
        }
        const float lx[2] = { -3.5f, 3.5f };
        for (int l = 0; l < 2; ++l) {
            cloth::CapsuleWorld& th = caps[1 + l * 2]; cloth::CapsuleWorld& ca = caps[2 + l * 2];
            th.a[0] = lx[l]; th.a[1] = 0.f; th.a[2] = 27.f; th.b[0] = lx[l]; th.b[1] = 0.f; th.b[2] = 14.f; ca.a[0] = lx[l]; ca.a[1] = 0.f; ca.a[2] = 14.f; ca.b[0] = lx[l]; ca.b[1] = 0.f; ca.b[2] = 2.f;
            th.r = 4.f; ca.r = 3.f; th.valid = ca.valid = true;
        }
        cloth::Step(*s, st, pr, po, caps, 5, dt);
        // stretch: worst spring ratio
        for (const cloth::Spring& sp : s->springs) {
            const float* A = &st.p[sp.i * 3]; const float* B = &st.p[sp.j * 3];
            const float d = std::sqrt((A[0] - B[0]) * (A[0] - B[0]) + (A[1] - B[1]) * (A[1] - B[1]) + (A[2] - B[2]) * (A[2] - B[2]));
            const float r = d / sp.rest; if (r > worstStretch) { worstStretch = r; worstFrame = f; }
        }
        if (f % 10 == 0) {
            float ang = std::acos(std::fmax(-1.f, std::fmin(1.f, (po.Rs[0] + po.Rs[4] + po.Rs[8] - 1.f) * 0.5f))) * 57.29578f;
            fprintf(o, "frame %d t %.2f rot %.1f gt %.1f %.1f %.1f\n", f, f * dt, ang, po.gt[0], po.gt[1], po.gt[2]);
            for (int i = 0; i < s->np; ++i) fprintf(o, "%.3f %.3f %.3f\n", st.p[i * 3], st.p[i * 3 + 1], st.p[i * 3 + 2]);
        }
    }
    fclose(o);
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < s->np; ++i) for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], st.p[i * 3 + k]); mx[k] = std::max(mx[k], st.p[i * 3 + k]); }
    printf("%s %s: frames %d (%.0f fps seq of %d frames) | worst spring stretch %.2fx at frame %d | final x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f]\n", argv[5], argv[4], N, fps, sq.frames, worstStretch, worstFrame, mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    return 0;
}
