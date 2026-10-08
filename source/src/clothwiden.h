// clothwiden.h - draw-only lateral stretch of the cloth so the top of the cloth is as wide as the collar sitting on it.
#pragma once
#include "clothpack.h"

namespace ess {

// The cloth of the cloaks is cut narrower at the top than the collars that sit on it (Legendary Ferios: collar +-7.8, cloth +-4.1), which leaves a step (and a gap to the
// shoulders) where they meet.  Draw-only fix: stretch the cloth sideways, more at the top, so that the top is as wide as the collar and the sides run straight to the hem.
// out[i] = half-width the cloth should have at the height of particle i (0 = leave it alone); ClothApplyWiden turns it into a sideways stretch of the simulated cloth.  The cloth is looked at as a function of the height z (the particles of the clan sets are not ordered in rows).
inline void ClothComputeWiden(const cloth::Set& s, const Collar* col, float amount, std::vector<float>& out) {
    out.assign((size_t)(s.np > 0 ? s.np : 0), 0.f);
    if (s.np <= 0 || !col || amount <= 0.f) return;
    const int n = s.np;
    float ztop = -1e9f, zmin = 1e9f;
    for (int i = 0; i < n; ++i) { const float z = s.rest[(size_t)i * 3 + 2]; ztop = std::max(ztop, z); zmin = std::min(zmin, z); }
    if (ztop - zmin < 1.f) return;
    std::vector<float> env((size_t)n, 0.f);                                   // width of the cloth at the height of each particle
    for (int i = 0; i < n; ++i) {
        const float z = s.rest[(size_t)i * 3 + 2];
        for (int j = 0; j < n; ++j) if (std::fabs(s.rest[(size_t)j * 3 + 2] - z) <= 2.5f) env[(size_t)i] = std::max(env[(size_t)i], std::fabs(s.rest[(size_t)j * 3]));
    }
    float baseTop = 0.f, hem = 0.f;
    for (int i = 0; i < n; ++i) {
        const float z = s.rest[(size_t)i * 3 + 2];
        if (z >= ztop - 0.5f) baseTop = std::max(baseTop, env[(size_t)i]);
        if (z <= zmin + 0.5f) hem = std::max(hem, env[(size_t)i]);
    }
    float wc = 0.f;
    for (int i = 0; i < col->nv; ++i) { const float z = col->pos[(size_t)i * 3 + 2]; if (z > ztop - 6.f && z < ztop + 2.f) wc = std::max(wc, std::fabs(col->pos[(size_t)i * 3])); }
    const float target = wc * 0.95f;
    if (baseTop < 0.5f || target <= baseTop) return;
    const float wt = baseTop + (target - baseTop) * (amount > 1.f ? 1.f : amount);
    for (int i = 0; i < n; ++i) {
        if (env[(size_t)i] < 0.5f) continue;
        const float tt = (ztop - s.rest[(size_t)i * 3 + 2]) / (ztop - zmin);
        const float want = wt + (std::max(hem, wt) - wt) * tt;
        if (want > env[(size_t)i] * 1.002f) out[(size_t)i] = want;
    }
}

// pos = simulated particles (actor space, np * 3), changed in place: every particle with a target is moved sideways (torso frame lateral axis) so that the width of the cloth
// at its height (measured on the simulated cloth, so the pinch of the simulation is corrected too) reaches the target.
inline void ClothApplyWiden(const cloth::Set& s, const std::vector<float>& target, const cloth::Pose& po, float* pos) {
    const int n = s.np;
    if ((int)target.size() != n || n <= 0 || n > 256) return;
    const float lat[3] = { po.Rs[0], po.Rs[3], po.Rs[6] };
    float xl[256], env[256];
    for (int i = 0; i < n; ++i) xl[i] = (pos[(size_t)i * 3] - po.gt[0]) * lat[0] + (pos[(size_t)i * 3 + 1] - po.gt[1]) * lat[1] + (pos[(size_t)i * 3 + 2] - po.gt[2]) * lat[2];
    for (int i = 0; i < n; ++i) {
        env[i] = 0.f;
        if (target[(size_t)i] <= 0.f) continue;
        const float z = s.rest[(size_t)i * 3 + 2];
        for (int j = 0; j < n; ++j) if (std::fabs(s.rest[(size_t)j * 3 + 2] - z) <= 2.5f) env[i] = std::max(env[i], std::fabs(xl[j]));
    }
    for (int i = 0; i < n; ++i) {
        if (target[(size_t)i] <= 0.f) continue;
        const float f = std::min(2.f, std::max(1.f, target[(size_t)i] / std::max(env[i], 0.5f)));
        if (f <= 1.0005f) continue;
        for (int k = 0; k < 3; ++k) pos[(size_t)i * 3 + k] += lat[k] * xl[i] * (f - 1.f);
    }
}

}  // namespace ess
