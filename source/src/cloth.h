// Cloth of the Essence standard cloaks (SimulationMesh): a small particle grid pinned to the torso, moved with position based dynamics.
// Everything happens in the ACTOR space of the body mesh (the space of the engine's bone coordinates), like the Essence NClothSimul:
// the anchors (top rows) follow the Spine2 bone, the rest is driven by gravity + a "wind" force that depends on the action (the Essence
// SimulationNotify objects carry such Force vectors, typically (0,0,-50) idle .. (0,-45,-50) running), springs from the authored
// DistAtRest tables, capsule colliders on the body bones and the floor.
#pragma once
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace cloth {

struct Spring { uint16_t i, j; float rest; };
struct Capsule { std::string a, b; float r; };

struct Set {
    std::string body, kind;                      // "MDarkElf", "H" (heavy) or "R" (robe)
    int np = 0, width = 0, nt = 0;
    std::vector<float> rest;                     // np * 3, Essence mesh space
    std::vector<float> uv;                       // np * 2
    std::vector<uint16_t> tris;                  // nt * 3
    std::vector<uint16_t> anchors;
    std::vector<Spring> springs;
    std::vector<Capsule> caps;
    std::vector<float> sens;                     // per particle force sensitivity
    std::vector<uint8_t> isAnchor;               // derived
    std::vector<float> rowW;                     // derived: 0 at the anchors .. 1 from 3 spring hops away (strength of the guide)
    int sec1 = -1;                               // first triangle of the second render section (crest window of the clan cloaks); -1 = none
    void Finish() {
        isAnchor.assign((size_t)np, 0); for (uint16_t a : anchors) if (a < np) isAnchor[a] = 1;
        std::vector<int> hop((size_t)np, 1000); std::vector<int> q;
        for (int i = 0; i < np; ++i) if (isAnchor[(size_t)i]) { hop[(size_t)i] = 0; q.push_back(i); }
        for (size_t h = 0; h < q.size(); ++h) for (const Spring& sp : springs) {
            const int a = sp.i, b = sp.j; const int cur = q[h];
            if (a == cur && hop[(size_t)b] > hop[(size_t)a] + 1) { hop[(size_t)b] = hop[(size_t)a] + 1; q.push_back(b); }
            else if (b == cur && hop[(size_t)a] > hop[(size_t)b] + 1) { hop[(size_t)a] = hop[(size_t)b] + 1; q.push_back(a); }
        }
        rowW.assign((size_t)np, 1.f);
        for (int i = 0; i < np; ++i) { const float u = hop[(size_t)i] >= 1000 ? 1.f : std::min(1.f, (float)hop[(size_t)i] / 3.f); rowW[(size_t)i] = u * u * (3.f - 2.f * u); }
    }
};

struct Params {
    float gravity = 70.f;                        // units/s^2 downwards
    float wind[3] = { 0.f, -10.f, 0.f };         // extra acceleration (actor space), -Y = backwards
    float stiffness = 0.85f;                     // spring stiffness per iteration (Essence "Stiffness": 0.6 .. 0.85)
    float damping = 2.8f;                        // velocity damping per second
    int   iterations = 8;
    float floorZ = 0.6f;                         // terrain collision (actor space z of the soles + margin); <= -1e8 = no floor
    float gdir[3] = { 0.f, 0.f, -1.f };          // direction of gravity in actor space (a rider's actor is pitched / rolled with the mount, so the world 'down' is not actor -z)
    float skin = 0.35f;                          // extra radius around the capsules
    float maxStep = 1.0f;                         // clamp on the displacement of one step
    float jumpAngle = 0.7f, jumpDist = 15.f;     // a pose change per frame bigger than this (rad / units) restarts the cloth at rest
    float widthK = 0.08f;                        // per 1/90 s step: pulls a particle that got narrower than its rest width back out (sideways, in the torso frame)
    const float* guide = nullptr;                // actor-space target position of every particle (np * 3) or null
    float guideK = 0.f;                          // per 1/90 s step: fraction of the way to the target (times the row weight)
    bool  inelastic = true;                      // contacts keep the pre-contact velocity (no launch from collider motion)
    float follow = 0.6f;                         // fraction of the torso's rotation (since the previous frame) that the whole cloth follows
};

struct Pose {                                    // the mapping rest space -> actor space for the pinned particles: x' = Rs * (x - tb) + gt
    float Rs[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    float tb[3] = { 0, 0, 0 };
    float gt[3] = { 0, 0, 0 };
};

struct CapsuleWorld { float a[3], b[3], r; bool valid = false; bool backBias = false;       // backBias: torso capsule, the cloth is always pushed out on the back side
                      float rTop = -1.f, zRef = 0.f, ramp = 0.f; };                          // rTop >= 0: radius at height zRef (the pinned rows), growing to r over `ramp` units below it

struct State {
    bool init = false;
    std::vector<float> p, q;                     // current / previous positions (np * 3)
    float accum = 0.f;
    bool havePrev = false; float Rp[9], gp[3];   // pose of the previous frame (for the frame following)
    int resets = 0;                              // how many times the pinned pose jumped and the cloth restarted at rest (diagnostics)
};

inline void MulVec(const float* R, const float* v, float* o) {
    o[0] = R[0] * v[0] + R[1] * v[1] + R[2] * v[2];
    o[1] = R[3] * v[0] + R[4] * v[1] + R[5] * v[2];
    o[2] = R[6] * v[0] + R[7] * v[1] + R[8] * v[2];
}

inline void PinTo(const Set& s, const Pose& po, State& st) {                // place the anchors (and nothing else)
    for (int i = 0; i < s.np; ++i) if (s.isAnchor[(size_t)i]) {
        float v[3] = { s.rest[(size_t)i * 3] - po.tb[0], s.rest[(size_t)i * 3 + 1] - po.tb[1], s.rest[(size_t)i * 3 + 2] - po.tb[2] }, o[3];
        MulVec(po.Rs, v, o);
        for (int k = 0; k < 3; ++k) st.p[(size_t)i * 3 + k] = o[k] + po.gt[k];
    }
}

inline void Reset(const Set& s, const Pose& po, State& st) {                // whole cloth rigidly attached, at rest
    st.p.assign((size_t)s.np * 3, 0.f);
    for (int i = 0; i < s.np; ++i) {
        float v[3] = { s.rest[(size_t)i * 3] - po.tb[0], s.rest[(size_t)i * 3 + 1] - po.tb[1], s.rest[(size_t)i * 3 + 2] - po.tb[2] }, o[3];
        MulVec(po.Rs, v, o);
        for (int k = 0; k < 3; ++k) st.p[(size_t)i * 3 + k] = o[k] + po.gt[k];
    }
    st.q = st.p; st.accum = 0.f; st.init = true;
}

// Pushes the particle out of the capsule and makes the contact inelastic: the particle keeps the velocity it had before the contact minus the part
// that pointed into the collider.  (Pure position pushing turns a fast moving collider - a kicking leg, the torso rotating at a run - into a huge
// verlet velocity and the cloth is flung away.)
inline void ContactVelocity(float* x, float* q, const float* v0, const float* n) {
    const float vn = v0[0] * n[0] + v0[1] * n[1] + v0[2] * n[2];
    const float k = vn < 0.f ? vn : 0.f;
    for (int c = 0; c < 3; ++c) q[c] = x[c] - (v0[c] - k * n[c]);
}

inline void PushOutCapsule(float* x, float* q, const CapsuleWorld& c, float skin, const float* back, bool inelastic = true) {
    float ab[3] = { c.b[0] - c.a[0], c.b[1] - c.a[1], c.b[2] - c.a[2] };
    float ap[3] = { x[0] - c.a[0], x[1] - c.a[1], x[2] - c.a[2] };
    const float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = l2 > 1e-9f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0.f;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    const float cp[3] = { c.a[0] + ab[0] * t, c.a[1] + ab[1] * t, c.a[2] + ab[2] * t };
    float d[3] = { x[0] - cp[0], x[1] - cp[1], x[2] - cp[2] };
    float rr = c.r;
    if (c.rTop >= 0.f && c.rTop < c.r && c.ramp > 0.1f) { float u = (c.zRef - x[2]) / c.ramp; u = u < 0.f ? 0.f : (u > 1.f ? 1.f : u); u = u * u * (3.f - 2.f * u); rr = c.rTop + (c.r - c.rTop) * u; }
    const float R = rr + skin;
    const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dl >= R) return;
    const float v0[3] = { x[0] - q[0], x[1] - q[1], x[2] - q[2] };
    if (dl < 1e-5f) { d[0] = back[0]; d[1] = back[1]; d[2] = back[2]; } else { d[0] /= dl; d[1] /= dl; d[2] /= dl; }   // exactly on the axis: push backwards
    if (c.backBias) {                                                       // a torso never lets the cloak through to its front: mirror the front half onto the back
        const float k = d[0] * back[0] + d[1] * back[1] + d[2] * back[2];
        if (k < 0.f) { d[0] -= 2.f * k * back[0]; d[1] -= 2.f * k * back[1]; d[2] -= 2.f * k * back[2]; }
    }
    for (int k = 0; k < 3; ++k) x[k] = cp[k] + d[k] * R;
    if (inelastic) ContactVelocity(x, q, v0, d);
}

// the whole cloth follows (a fraction of) the rotation of the torso between two frames: a strong lean / turn does not tear it away from the anchors
inline void FollowFrame(const Set& s, State& st, const Pose& po, float follow) {
    if (st.havePrev && follow > 0.f) {
        // D = Rs * Rp^T  (rotation taking the previous torso frame to the new one)
        const float* A = po.Rs; const float* B = st.Rp;
        float D[9];
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) D[i * 3 + j] = A[i * 3] * B[j * 3] + A[i * 3 + 1] * B[j * 3 + 1] + A[i * 3 + 2] * B[j * 3 + 2];
        const float tr = (D[0] + D[4] + D[8] - 1.f) * 0.5f;
        const float ang = std::acos(tr > 1.f ? 1.f : (tr < -1.f ? -1.f : tr));
        if (ang > 1e-4f) {
            float ax[3] = { D[7] - D[5], D[2] - D[6], D[3] - D[1] };
            const float al = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
            if (al > 1e-6f) {
                ax[0] /= al; ax[1] /= al; ax[2] /= al;
                const float a = ang * follow, c = std::cos(a), sn = std::sin(a), oc = 1.f - c;
                const float F[9] = { c + ax[0] * ax[0] * oc, ax[0] * ax[1] * oc - ax[2] * sn, ax[0] * ax[2] * oc + ax[1] * sn,
                                     ax[1] * ax[0] * oc + ax[2] * sn, c + ax[1] * ax[1] * oc, ax[1] * ax[2] * oc - ax[0] * sn,
                                     ax[2] * ax[0] * oc - ax[1] * sn, ax[2] * ax[1] * oc + ax[0] * sn, c + ax[2] * ax[2] * oc };
                for (int i = 0; i < s.np; ++i) {
                    if (s.isAnchor[(size_t)i]) continue;
                    for (std::vector<float>* arr : { &st.p, &st.q }) {
                        float* x = &(*arr)[(size_t)i * 3]; const float v[3] = { x[0] - po.gt[0], x[1] - po.gt[1], x[2] - po.gt[2] };
                        float o[3]; MulVec(F, v, o);
                        x[0] = o[0] + po.gt[0]; x[1] = o[1] + po.gt[1]; x[2] = o[2] + po.gt[2];
                    }
                }
            }
        }
    }
    memcpy(st.Rp, po.Rs, sizeof st.Rp); st.gp[0] = po.gt[0]; st.gp[1] = po.gt[1]; st.gp[2] = po.gt[2]; st.havePrev = true;
}

// advances the cloth by dt seconds (internally in steps of at most 1/90 s)
inline void Step(const Set& s, State& st, const Params& pr, const Pose& po, const CapsuleWorld* caps, int ncaps, float dt) {
    if (!st.init) { Reset(s, po, st); memcpy(st.Rp, po.Rs, sizeof st.Rp); st.gp[0] = po.gt[0]; st.gp[1] = po.gt[1]; st.gp[2] = po.gt[2]; st.havePrev = true; return; }
    if (dt > 0.1f) dt = 0.1f;
    if (st.havePrev) {                                                    // the pinned pose jumped (equip, the engine rebuilt the skeleton, a teleport): start again from rest instead of whipping the cloth about
        const float* A = po.Rs; const float* Bm = st.Rp;
        float tr = 0.f; for (int i = 0; i < 3; ++i) for (int k = 0; k < 3; ++k) tr += A[i * 3 + k] * Bm[i * 3 + k];
        const float c = (tr - 1.f) * 0.5f, ang = std::acos(c > 1.f ? 1.f : (c < -1.f ? -1.f : c));
        const float dx = po.gt[0] - st.gp[0], dy = po.gt[1] - st.gp[1], dz = po.gt[2] - st.gp[2];
        if (ang > pr.jumpAngle || dx * dx + dy * dy + dz * dz > pr.jumpDist * pr.jumpDist) { ++st.resets; Reset(s, po, st); memcpy(st.Rp, po.Rs, sizeof st.Rp); st.gp[0] = po.gt[0]; st.gp[1] = po.gt[1]; st.gp[2] = po.gt[2]; st.havePrev = true; return; }
    }
    FollowFrame(s, st, po, pr.follow);
    const float bk0[3] = { 0.f, -1.f, 0.f }; float back[3]; MulVec(po.Rs, bk0, back);
    st.accum += dt;
    const float h = 1.f / 90.f;
    int steps = 0;
    while (st.accum >= h && steps < 9) {
        st.accum -= h; ++steps;
        const float damp = std::exp(-pr.damping * h);
        const float acc[3] = { pr.wind[0] + pr.gdir[0] * pr.gravity, pr.wind[1] + pr.gdir[1] * pr.gravity, pr.wind[2] + pr.gdir[2] * pr.gravity };
        for (int i = 0; i < s.np; ++i) {
            if (s.isAnchor[(size_t)i]) continue;
            float* p = &st.p[(size_t)i * 3]; float* q = &st.q[(size_t)i * 3];
            const float k = s.sens.empty() ? 1.f : s.sens[(size_t)i];
            float v[3] = { (p[0] - q[0]) * damp, (p[1] - q[1]) * damp, (p[2] - q[2]) * damp };
            const float vl = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (vl > pr.maxStep) { const float f = pr.maxStep / vl; v[0] *= f; v[1] *= f; v[2] *= f; }
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
            p[0] += v[0] + acc[0] * k * h * h; p[1] += v[1] + acc[1] * k * h * h; p[2] += v[2] + acc[2] * k * h * h;
        }
        PinTo(s, po, st);                                          // the anchors follow the torso (their own "velocity" is not carried)
        if (pr.widthK > 0.f) for (int i = 0; i < s.np; ++i) {                           // keep the cloak as wide as it is cut: gravity must not pinch it to the centre
            if (s.isAnchor[(size_t)i]) continue;
            float* p = &st.p[(size_t)i * 3];
            const float d[3] = { p[0] - po.gt[0], p[1] - po.gt[1], p[2] - po.gt[2] };
            const float vx = po.Rs[0] * d[0] + po.Rs[3] * d[1] + po.Rs[6] * d[2];                // lateral coordinate in the torso frame
            const float rx = s.rest[(size_t)i * 3] - po.tb[0];
            if (std::fabs(vx) < std::fabs(rx) && vx * rx >= 0.f) {
                const float delta = (rx - vx) * pr.widthK * s.rowW[(size_t)i];
                p[0] += po.Rs[0] * delta; p[1] += po.Rs[3] * delta; p[2] += po.Rs[6] * delta;
            }
        }
        if (pr.guide && pr.guideK > 0.f) for (int i = 0; i < s.np; ++i) {                  // guided by the baked cape deformation: drift towards the target
            if (s.isAnchor[(size_t)i]) continue;
            const float k = pr.guideK * s.rowW[(size_t)i];
            float* p = &st.p[(size_t)i * 3]; const float* g = &pr.guide[(size_t)i * 3];
            p[0] += (g[0] - p[0]) * k; p[1] += (g[1] - p[1]) * k; p[2] += (g[2] - p[2]) * k;
        }
        for (int it = 0; it < pr.iterations; ++it) {
            for (const Spring& sp : s.springs) {
                float* a = &st.p[(size_t)sp.i * 3]; float* b = &st.p[(size_t)sp.j * 3];
                float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
                const float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dl < 1e-6f) continue;
                const float diff = (dl - sp.rest) / dl * pr.stiffness;
                const bool fa = s.isAnchor[sp.i] != 0, fb = s.isAnchor[sp.j] != 0;
                if (fa && fb) continue;
                const float wa = fa ? 0.f : (fb ? 1.f : 0.5f), wb = fb ? 0.f : (fa ? 1.f : 0.5f);
                for (int k = 0; k < 3; ++k) { a[k] += d[k] * diff * wa; b[k] -= d[k] * diff * wb; }
            }
            for (int i = 0; i < s.np; ++i) {
                if (s.isAnchor[(size_t)i]) continue;
                float* x = &st.p[(size_t)i * 3];
                for (int c = 0; c < ncaps; ++c) if (caps[c].valid) PushOutCapsule(x, &st.q[(size_t)i * 3], caps[c], pr.skin, back, pr.inelastic);
                if (pr.floorZ > -1e8f && x[2] < pr.floorZ) { float* q = &st.q[(size_t)i * 3]; const float v0[3] = { x[0] - q[0], x[1] - q[1], x[2] - q[2] }, up[3] = { 0.f, 0.f, 1.f }; x[2] = pr.floorZ; if (pr.inelastic) ContactVelocity(x, q, v0, up); }
            }
        }
    }
}

// smooth per-vertex normals from the triangles
inline void Normals(const Set& s, const std::vector<float>& p, std::vector<float>& n) {
    n.assign((size_t)s.np * 3, 0.f);
    for (int t = 0; t < s.nt; ++t) {
        const int a = s.tris[(size_t)t * 3], b = s.tris[(size_t)t * 3 + 1], c = s.tris[(size_t)t * 3 + 2];
        const float* A = &p[(size_t)a * 3]; const float* B = &p[(size_t)b * 3]; const float* C = &p[(size_t)c * 3];
        const float u[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, v[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
        const float f[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        for (int k = 0; k < 3; ++k) { n[(size_t)a * 3 + k] += f[k]; n[(size_t)b * 3 + k] += f[k]; n[(size_t)c * 3 + k] += f[k]; }
    }
    for (int i = 0; i < s.np; ++i) {
        float* v = &n[(size_t)i * 3]; const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-9f) { v[0] /= l; v[1] /= l; v[2] /= l; } else { v[0] = 0.f; v[1] = -1.f; v[2] = 0.f; }
    }
}

}  // namespace cloth
