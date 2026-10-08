// cloth_test.cpp - runs the cloth solver on a synthetic torso motion and writes the particle positions of some frames.
//   cloth_test <essence_cloth.bin> <Body> <H|R> <scenario: idle|walk|run|turn|sit> <out.txt>
#include "../src/clothpack.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 6) { printf("usage\n"); return 2; }
    wchar_t wp[512]; mbstowcs(wp, argv[1], 512);
    ess::ClothPack cp; std::string err;
    if (!ess::LoadClothPack(wp, cp, &err)) { printf("LOAD FAILED: %s\n", err.c_str()); return 1; }
    const cloth::Set* s = ess::FindClothSet(cp, argv[2], argv[3][0]);
    if (!s) { printf("set not found\n"); return 1; }
    printf("pack: %d textures, %d sets, %d looks; set %s %s: %d particles, %d springs, %d anchors\n", (int)cp.texs.size(), (int)cp.sets.size(), (int)cp.looks.size(), s->body.c_str(), s->kind.c_str(), s->np, (int)s->springs.size(), (int)s->anchors.size());
    const std::string sc = argv[4];
    cloth::Params pr; cloth::State st; cloth::Pose po;
    if (argc > 6) pr.follow = (float)atof(argv[6]);
    const bool bias = argc > 7 ? atoi(argv[7]) != 0 : true;
    if (argc > 8) pr.inelastic = atoi(argv[8]) != 0;
    if (argc > 9) pr.maxStep = (float)atof(argv[9]);
    const float tb[3] = { 0.f, 0.f, 32.8f };                                   // Spine2 bind in the Essence mesh space (approx)
    for (int k = 0; k < 3; ++k) { po.tb[k] = tb[k]; po.gt[k] = tb[k]; }
    FILE* o = fopen(argv[5], "w");
    const float dt = 1.f / 60.f; const int N = 60 * 6; float peakSpeed = 0.f; float worstStretch = 0.f, minArea = 1e9f, maxFront = 0.f; int frontMax = 0;
    for (int f = 0; f < N; ++f) {
        const float t = f * dt;
        float yaw = 0.f, pitch = 0.f, bob = 0.f, lat = 0.f, windY = -10.f;
        if (sc == "idle") { pitch = 0.02f * std::sin(t * 1.3f); windY = -9.f; }
        else if (sc == "walk") { yaw = 0.07f * std::sin(t * 2.0f * 3.14159f * 1.0f); bob = 0.4f * std::sin(t * 2.f * 3.14159f * 2.f); windY = -20.f; pitch = 0.06f; }
        else if (sc == "run") { yaw = 0.12f * std::sin(t * 2.0f * 3.14159f * 1.4f); bob = 0.9f * std::sin(t * 2.f * 3.14159f * 2.8f); windY = -36.f; pitch = 0.22f; }
        else if (sc == "turn") { yaw = (t < 3.f) ? 2.5f * t : 7.5f; windY = -10.f; }
        else if (sc == "sit") { pitch = (t < 1.5f) ? 0.f : -1.2f; windY = 0.f; }
        else if (sc == "sit2") { const float k = t < 1.5f ? 0.f : (t < 2.1f ? (t - 1.5f) / 0.6f : 1.f); bob = -12.f * k; pitch = -0.08f * k; windY = 0.f; }
        else if (sc == "run89" || sc == "run89k") {                           // what the engine log shows for run_1hs: Spine2 ~85 deg from idle, origin moved forward/down
            const float k = t < 1.f ? 0.f : (t < 1.3f ? (t - 1.f) / 0.3f : 1.f);
            pitch = -1.5f * k; yaw = 0.1f * k * std::sin(t * 2.f * 3.14159f * 1.4f); bob = (-3.3f + 0.9f * std::sin(t * 2.f * 3.14159f * 2.8f)) * k; lat = 0.f; windY = -36.f * k - 9.f * (1.f - k);
        }
        else if (sc == "lean" || sc == "leanb") {          // strong forward (or backward) lean while running: ramp in 0.35 s, hold with bob + yaw wobble, ramp out at 4.5 s
            const float k = t < 1.f ? 0.f : (t < 1.35f ? (t - 1.f) / 0.35f : (t < 4.5f ? 1.f : (t < 4.85f ? 1.f - (t - 4.5f) / 0.35f : 0.f)));
            pitch = (sc == "lean" ? -1.15f : 1.15f) * k; yaw = 0.15f * k * std::sin(t * 2.f * 3.14159f * 1.4f); bob = 0.9f * k * std::sin(t * 2.f * 3.14159f * 2.8f); windY = -45.f * k - 10.f * (1.f - k);
        }
        pr.wind[1] = windY;
        // Rs: rotation about Z (yaw) then X (pitch), applied to the rest offsets; the spine origin bobs
        const float cy = std::cos(yaw), sy = std::sin(yaw), cp_ = std::cos(pitch), sp_ = std::sin(pitch);
        const float Rz[9] = { cy, -sy, 0, sy, cy, 0, 0, 0, 1 }, Rx[9] = { 1, 0, 0, 0, cp_, -sp_, 0, sp_, cp_ };
        ess::Mul33(Rz, Rx, po.Rs);
        po.gt[0] = tb[0] + lat; po.gt[1] = tb[1] + ((sc == "run89" || sc == "run89k") && t >= 1.f ? 9.f * (t < 1.3f ? (t - 1.f) / 0.3f : 1.f) : 0.f); po.gt[2] = tb[2] + bob;
        // body capsules (approx. Essence MDarkElf bind pose), rotated with the torso for the spine one, fixed for the legs
        cloth::CapsuleWorld caps[6];
        auto spine = [&](float z, float* out) { float v[3] = { 0.f, 0.5f, z - tb[2] }, w[3]; ess::MulVec(po.Rs, v, w); out[0] = w[0] + po.gt[0]; out[1] = w[1] + po.gt[1]; out[2] = w[2] + po.gt[2]; };
        spine(32.f, caps[0].a); spine(25.f, caps[0].b); caps[0].r = 6.f; caps[0].valid = true; caps[0].backBias = bias;
        const float lx[2] = { -3.5f, 3.5f };
        float legTop = 27.f; if (sc == "sit") legTop = 27.f;
        for (int l = 0; l < 2; ++l) {
            cloth::CapsuleWorld& th = caps[1 + l * 2]; cloth::CapsuleWorld& ca = caps[2 + l * 2];
            if ((sc == "sit" || sc == "sit2") && t >= 1.5f) {     // thighs forward (+Y is the front here? the cloak hangs at -Y, so front = +Y)
                th.a[0] = lx[l]; th.a[1] = 0.f; th.a[2] = 24.f; th.b[0] = lx[l]; th.b[1] = 13.f; th.b[2] = 22.f; ca.a[0] = lx[l]; ca.a[1] = 13.f; ca.a[2] = 22.f; ca.b[0] = lx[l]; ca.b[1] = 13.f; ca.b[2] = 6.f;
            } else if (sc == "run89k" && t >= 1.f) {         // running legs: thighs swing +-45 deg, the rear calf folds up behind (what a real run does to the space behind the pelvis)
                const float ph = 2.f * 3.14159f * 1.4f * t + (l ? 3.14159f : 0.f);
                const float s1 = 0.8f * std::sin(ph), fold = 0.5f * (1.f + std::sin(ph - 1.2f)) * 1.6f;
                th.a[0] = lx[l]; th.a[1] = 3.f; th.a[2] = 25.f; th.b[0] = lx[l]; th.b[1] = th.a[1] + 13.f * std::sin(s1); th.b[2] = th.a[2] - 13.f * std::cos(s1);
                ca.a[0] = lx[l]; ca.a[1] = th.b[1]; ca.a[2] = th.b[2]; ca.b[0] = lx[l]; ca.b[1] = ca.a[1] + 12.f * std::sin(s1 - fold); ca.b[2] = ca.a[2] - 12.f * std::cos(s1 - fold);
            } else {
                th.a[0] = lx[l]; th.a[1] = 0.f; th.a[2] = legTop; th.b[0] = lx[l]; th.b[1] = 0.f; th.b[2] = 14.f; ca.a[0] = lx[l]; ca.a[1] = 0.f; ca.a[2] = 14.f; ca.b[0] = lx[l]; ca.b[1] = 0.f; ca.b[2] = 2.f;
            }
            th.r = 4.f; ca.r = 3.f; th.valid = ca.valid = true;
        }
        int ncaps = 5;
        if (sc == "sit2" && t >= 1.5f) { for (int k = 0; k < 3; ++k) { caps[5].a[k] = caps[1].a[k]; caps[5].b[k] = caps[3].a[k]; } caps[5].r = 6.5f; caps[5].valid = true; caps[5].backBias = true; ncaps = 6; }
        cloth::Step(*s, st, pr, po, caps, ncaps, dt);
        {   // metrics
            for (const cloth::Spring& sp : s->springs) {
                const float* A = &st.p[sp.i * 3]; const float* B = &st.p[sp.j * 3];
                const float d = std::sqrt((A[0] - B[0]) * (A[0] - B[0]) + (A[1] - B[1]) * (A[1] - B[1]) + (A[2] - B[2]) * (A[2] - B[2]));
                if (sp.rest > 0.5f && d / sp.rest > worstStretch) worstStretch = d / sp.rest;
            }
            if (t > 1.5f) for (int i = 0; i < s->np; ++i) { if (s->isAnchor[(size_t)i]) continue; const float vx = (st.p[i * 3] - st.q[i * 3]) * 90.f, vy = (st.p[i * 3 + 1] - st.q[i * 3 + 1]) * 90.f, vz = (st.p[i * 3 + 2] - st.q[i * 3 + 2]) * 90.f; peakSpeed = std::max(peakSpeed, std::sqrt(vx * vx + vy * vy + vz * vz)); }
            double ar = 0, ar0 = 0;
            for (int ti = 0; ti < s->nt; ++ti) {
                const int a = s->tris[ti * 3], b = s->tris[ti * 3 + 1], c = s->tris[ti * 3 + 2];
                auto area = [&](const float* P) { const float u[3] = { P[b * 3] - P[a * 3], P[b * 3 + 1] - P[a * 3 + 1], P[b * 3 + 2] - P[a * 3 + 2] }, v[3] = { P[c * 3] - P[a * 3], P[c * 3 + 1] - P[a * 3 + 1], P[c * 3 + 2] - P[a * 3 + 2] };
                    const float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] }; return std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) * 0.5; };
                ar += area(st.p.data()); ar0 += area(s->rest.data());
            }
            if (t > 1.5f) minArea = std::min(minArea, (float)(ar / ar0));
            int front = 0;                                        // particles in front of the torso (body frame y > 2)
            for (int i = 0; i < s->np; ++i) {
                const float v[3] = { st.p[i * 3] - po.gt[0], st.p[i * 3 + 1] - po.gt[1], st.p[i * 3 + 2] - po.gt[2] };
                const float y = po.Rs[1] * v[0] + po.Rs[4] * v[1] + po.Rs[7] * v[2];             // Rs^T * v, y component
                if (y > 2.f) ++front;
            }
            frontMax = std::max(frontMax, front);
        }
        if (f % 18 == 0 || f == N - 1) {
            fprintf(o, "frame %d t %.2f\n", f, t);
            for (int i = 0; i < s->np; ++i) fprintf(o, "%.3f %.3f %.3f\n", st.p[i * 3], st.p[i * 3 + 1], st.p[i * 3 + 2]);
        }
    }
    fclose(o);
    // simple stability stats at the end
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    for (int i = 0; i < s->np; ++i) for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], st.p[i * 3 + k]); mx[k] = std::max(mx[k], st.p[i * 3 + k]); }
    printf("peak particle speed %.0f u/s | worst spring stretch %.2fx | min cloth area vs rest %.0f%% (after 1.5 s) | max particles in front of the torso %d\n", peakSpeed, worstStretch, minArea * 100.f, frontMax);
    {   // the same numbers the hook logs: the cloth in the torso frame at the last frame
        float tmn[3] = { 1e9f, 1e9f, 1e9f }, tmx[3] = { -1e9f, -1e9f, -1e9f };
        for (int i = 0; i < s->np; ++i) {
            const float v[3] = { st.p[i * 3] - po.gt[0], st.p[i * 3 + 1] - po.gt[1], st.p[i * 3 + 2] - po.gt[2] };
            const float b[3] = { po.Rs[0] * v[0] + po.Rs[3] * v[1] + po.Rs[6] * v[2], po.Rs[1] * v[0] + po.Rs[4] * v[1] + po.Rs[7] * v[2], po.Rs[2] * v[0] + po.Rs[5] * v[1] + po.Rs[8] * v[2] };
            for (int k = 0; k < 3; ++k) { tmn[k] = std::min(tmn[k], b[k]); tmx[k] = std::max(tmx[k], b[k]); }
        }
        printf("torso frame x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f]\n", tmn[0], tmx[0], tmn[1], tmx[1], tmn[2], tmx[2]);
    }
    printf("final extents x[%.1f..%.1f] y[%.1f..%.1f] z[%.1f..%.1f]\n", mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]);
    return 0;
}
