// root_dump.cpp - baked Spine2 ("Cape_dummy") pose of some sequences relative to the idle one: rotation axis/angle in actor space and origin
//   root_dump <pack> <Body> <seq>...
#include "../src/essence.h"
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 4) { printf("usage\n"); return 2; }
    wchar_t w[512]; mbstowcs(w, argv[1], 512);
    ess::Pack pk; std::string err;
    if (!ess::LoadPack(w, pk, &err)) { printf("load: %s\n", err.c_str()); return 1; }
    ess::Anim* a = ess::FindAnim(pk, argv[2]);
    if (!a) { printf("body?\n"); return 1; }
    auto get = [&](const char* n, ess::Pose& p, int& frames) {
        auto it = a->byName.find(ess::Lower(n)); if (it == a->byName.end()) return false;
        frames = a->seqs[it->second].frames; ess::EvalSeq(*a, a->seqs[it->second], 0.f, p); return true; };
    ess::Pose p0; int f0 = 0;
    if (!get((std::string("wait_1hs_") + ess::Lower(argv[2])).c_str(), p0, f0)) { printf("no idle\n"); return 1; }
    float R0[9]; ess::QuatToMat(&p0.q[0], R0);
    const float R0T[9] = { R0[0], R0[3], R0[6], R0[1], R0[4], R0[7], R0[2], R0[5], R0[8] };
    for (int i = 3; i < argc; ++i) {
        ess::Pose p; int fr = 0;
        if (!get(argv[i], p, fr)) { printf("%s: not found\n", argv[i]); continue; }
        // sample a few frames: min/max angle
        float amin = 1e9f, amax = -1e9f; float axs[3] = { 0, 0, 0 };
        for (int f = 0; f < fr; ++f) {
            ess::Pose q; ess::EvalSeq(*a, a->seqs[a->byName[ess::Lower(argv[i])]], (float)f, q);
            float R[9], D[9]; ess::QuatToMat(&q.q[0], R); ess::Mul33(R, R0T, D);
            const float tr = (D[0] + D[4] + D[8] - 1.f) * 0.5f; const float ang = std::acos(tr > 1.f ? 1.f : (tr < -1.f ? -1.f : tr)) * 57.29578f;
            amin = std::min(amin, ang); amax = std::max(amax, ang);
            if (f == 0) { float x[3] = { D[7] - D[5], D[2] - D[6], D[3] - D[1] }; const float l = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]); if (l > 1e-6f) for (int k = 0; k < 3; ++k) axs[k] = x[k] / l; }
        }
        float R[9], D[9]; ess::QuatToMat(&p.q[0], R); ess::Mul33(R, R0T, D);
        printf("%-26s frames %3d | angle vs idle %.0f..%.0f deg | axis(f0) %.2f %.2f %.2f | origin %.1f %.1f %.1f (idle %.1f %.1f %.1f)\n", argv[i], fr, amin, amax, axs[0], axs[1], axs[2], p.p[0], p.p[1], p.p[2], p0.p[0], p0.p[1], p0.p[2]);
    }
    return 0;
}
