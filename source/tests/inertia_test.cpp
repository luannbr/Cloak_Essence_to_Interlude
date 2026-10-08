// Inertialization check: ApplyOffset(tgt, MakeOffset(cur, tgt), 1) == cur  and  weight 0 == tgt.
#include "../src/essence.h"
#include <cstdlib>

static float QDiff(const float* a, const float* b) {                  // angle (deg) between two unit quaternions
    float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]); if (d > 1.f) d = 1.f;
    return 2.f * std::acos(d) * 57.29578f;
}

int main(int argc, char** argv) {
    wchar_t wpath[512]; mbstowcs(wpath, argv[1], 512);
    ess::Pack pk; std::string err;
    if (!ess::LoadPack(wpath, pk, &err)) { printf("LOAD FAILED %s\n", err.c_str()); return 1; }
    ess::Anim* a = ess::FindAnim(pk, "MDarkElf");
    const char* pairs[][2] = { { "walk_1hs_mdarkelf", "wait_1hs_mdarkelf" }, { "run_1hs_mdarkelf", "sit_mdarkelf" }, { "sit_mdarkelf", "stand_mdarkelf" } };
    int bad = 0;
    for (auto& pr : pairs) {
        const ess::Seq& sa = a->seqs[a->byName[pr[0]]]; const ess::Seq& sb = a->seqs[a->byName[pr[1]]];
        ess::Pose cur, tgt, out; ess::EvalSeq(*a, sa, 7.3f, cur); ess::EvalSeq(*a, sb, 0.f, tgt);
        ess::PoseOffset off; ess::MakeOffset(cur, tgt, off);
        float worstQ = 0, worstP = 0, worst0 = 0, mid = 0;
        ess::ApplyOffset(tgt, off, 1.f, out);
        for (int i = 0; i < cur.nb; ++i) { worstQ = std::fmax(worstQ, QDiff(&out.q[i * 4], &cur.q[i * 4])); for (int k = 0; k < 3; ++k) worstP = std::fmax(worstP, std::fabs(out.p[i * 3 + k] - cur.p[i * 3 + k])); }
        ess::ApplyOffset(tgt, off, 0.f, out);
        for (int i = 0; i < tgt.nb; ++i) worst0 = std::fmax(worst0, QDiff(&out.q[i * 4], &tgt.q[i * 4]));
        ess::ApplyOffset(tgt, off, 0.5f, out);
        for (int i = 0; i < tgt.nb; ++i) { const float d1 = QDiff(&out.q[i * 4], &cur.q[i * 4]), d2 = QDiff(&out.q[i * 4], &tgt.q[i * 4]), dt = QDiff(&cur.q[i * 4], &tgt.q[i * 4]); mid = std::fmax(mid, d1 + d2 - dt); }
        printf("%-20s -> %-20s | w=1 vs current: %.4f deg, %.5f units | w=0 vs target: %.4f deg | w=0.5 detour %.3f deg\n", pr[0], pr[1], worstQ, worstP, worst0, mid);
        if (worstQ > 0.1f || worstP > 1e-3f || worst0 > 0.1f) ++bad;
    }
    printf(bad ? "FAILED\n" : "OK\n");
    return bad;
}
