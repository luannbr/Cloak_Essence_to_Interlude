// clothguide.h - the cloth follows the deformation that the BAKED cape (design 0 of the same body) shows in the current animation relative to idle.
// The cape bones are driven by the same sequence names as the body, so the cloth moves like the animated mantles (smooth, authored); the spring solver
// (cloth.h) keeps it a cloth: collisions, secondary motion.  Everything is expressed in the root frame of the cape (the Spine2 attachment), so the
// axis-mapping offset between the engine's Spine2 and the Essence bind cancels.
#pragma once
#include "essence.h"
#include "cloth.h"

namespace guide {

constexpr int kK = 6;                                          // cape points blended per cloth particle

struct Map {
    bool ok = false;
    std::vector<int> sub;                                      // cape point indices that are skinned every frame
    std::vector<int> idx;                                      // np * kK : index into `sub`
    std::vector<float> w;                                      // np * kK
    std::vector<float> target;                                 // np * 3 (actor space), filled by Update
};

inline int RootMeshBone(const std::vector<int>& amap) { for (size_t j = 0; j < amap.size(); ++j) if (amap[j] == 0) return (int)j; return 0; }

// local coordinates of every cape point in the idle pose (reference); returns false when the animation has no idle sequence
inline bool IdleLocal(const ess::Anim& a, const ess::Mantle& m, const std::vector<int>& amap, std::vector<float>& idleL) {
    const std::string lb = ess::Lower(a.body); int si = -1;
    for (const char* pre : { "wait_hand_", "wait_1hs_", "wait_" }) { auto it = a.byName.find(std::string(pre) + lb); if (it != a.byName.end()) { si = it->second; break; } }
    if (si < 0) for (size_t i = 0; i < a.seqs.size(); ++i) if (a.seqs[i].name.compare(0, 4, "wait") == 0) { si = (int)i; break; }
    if (si < 0) return false;
    ess::Pose pose; ess::EvalSeq(a, a.seqs[(size_t)si], 0.f, pose);
    std::vector<float> pos; ess::SkinPoints(a, m, amap, pose, pos);
    const int jr = RootMeshBone(amap);
    idleL.assign((size_t)m.np * 3, 0.f);
    for (int k = 0; k < m.np; ++k) ess::RootLocal(m, jr, pose, &pos[(size_t)k * 3], &idleL[(size_t)k * 3]);
    return true;
}

// maps every cloth particle on its nearest cape drape points (by the x/z offset from the Spine2 root in the idle pose)
inline void Build(const cloth::Set& set, const ess::Mantle& m, const std::vector<int>& amap, const std::vector<float>& idleL, Map& g) {
    const int jr = RootMeshBone(amap); const float* tb = &m.bindT[(size_t)jr * 3];
    const int np = set.np;
    std::vector<int> pointOf((size_t)m.np, -1);
    g.sub.clear(); g.idx.assign((size_t)np * kK, 0); g.w.assign((size_t)np * kK, 0.f);
    for (int i = 0; i < np; ++i) {
        const float o[3] = { set.rest[(size_t)i * 3] - tb[0], set.rest[(size_t)i * 3 + 1] - tb[1], set.rest[(size_t)i * 3 + 2] - tb[2] };
        float bd[kK]; int bk[kK]; for (int k = 0; k < kK; ++k) { bd[k] = 1e30f; bk[k] = -1; }
        for (int k = 0; k < m.np; ++k) {
            const float* L = &idleL[(size_t)k * 3];
            if (L[2] > 8.f) continue;                                                     // the collar / shoulder ornaments of the cape are not part of the drape
            const float d2 = (L[0] - o[0]) * (L[0] - o[0]) + (L[2] - o[2]) * (L[2] - o[2]) + 0.1f * (L[1] - o[1]) * (L[1] - o[1]);
            int w = kK - 1; if (d2 >= bd[w]) continue;
            while (w > 0 && d2 < bd[w - 1]) { bd[w] = bd[w - 1]; bk[w] = bk[w - 1]; --w; }
            bd[w] = d2; bk[w] = k;
        }
        float sw = 0.f;
        for (int k = 0; k < kK; ++k) { if (bk[k] < 0) continue; const float w = 1.f / (std::sqrt(bd[k]) + 0.5f); g.w[(size_t)i * kK + k] = w; sw += w; }
        for (int k = 0; k < kK; ++k) {
            if (bk[k] < 0 || sw <= 0.f) { g.idx[(size_t)i * kK + k] = 0; g.w[(size_t)i * kK + k] = 0.f; continue; }
            g.w[(size_t)i * kK + k] /= sw;
            if (pointOf[(size_t)bk[k]] < 0) { pointOf[(size_t)bk[k]] = (int)g.sub.size(); g.sub.push_back(bk[k]); }
            g.idx[(size_t)i * kK + k] = pointOf[(size_t)bk[k]];
        }
    }
    g.target.assign((size_t)np * 3, 0.f); g.ok = !g.sub.empty();
}

// target positions (actor space) of the particles for the cape pose `cur` (the driver's current pose with the engine's Spine2 as root) and the pinned pose po
inline void Update(const cloth::Set& set, const ess::Anim& a, const ess::Mantle& m, const std::vector<int>& amap, const std::vector<float>& idleL, const ess::Pose& cur, const cloth::Pose& po, Map& g) {
    const int jr = RootMeshBone(amap);
    static std::vector<float> sk, dl;
    ess::SkinSubset(a, m, amap, cur, g.sub, sk);
    dl.assign(g.sub.size() * 3, 0.f);
    for (size_t n = 0; n < g.sub.size(); ++n) {
        float L[3]; ess::RootLocal(m, jr, cur, &sk[n * 3], L);
        const float* L0 = &idleL[(size_t)g.sub[n] * 3];
        dl[n * 3] = L[0] - L0[0]; dl[n * 3 + 1] = L[1] - L0[1]; dl[n * 3 + 2] = L[2] - L0[2];
    }
    for (int i = 0; i < set.np; ++i) {
        float acc[3] = { 0.f, 0.f, 0.f };
        for (int k = 0; k < kK; ++k) { const float w = g.w[(size_t)i * kK + k]; if (w <= 0.f) continue; const float* d = &dl[(size_t)g.idx[(size_t)i * kK + k] * 3]; acc[0] += w * d[0]; acc[1] += w * d[1]; acc[2] += w * d[2]; }
        const float v[3] = { set.rest[(size_t)i * 3] - po.tb[0] + acc[0], set.rest[(size_t)i * 3 + 1] - po.tb[1] + acc[1], set.rest[(size_t)i * 3 + 2] - po.tb[2] + acc[2] };
        float o[3]; cloth::MulVec(po.Rs, v, o);
        g.target[(size_t)i * 3] = o[0] + po.gt[0]; g.target[(size_t)i * 3 + 1] = o[1] + po.gt[1]; g.target[(size_t)i * 3 + 2] = o[2] + po.gt[2];
    }
}

}  // namespace guide
