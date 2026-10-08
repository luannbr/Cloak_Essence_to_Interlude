// fx.h - particle effects of the Essence cloaks (tools/build_fx.py -> essence_fx.bin): pack loader and emitter simulation.  No engine or Direct3D dependency.
//
// The emitters follow the UE2 ParticleEmitter model the Essence client uses (Sprite and Mesh emitters only): particles spawn at a rate, live for a
// lifetime, move with velocity / drag / acceleration, and are scaled / tinted / faded over their life.  Coordinates are the emitter's local space
// (x forward, y right, z up); the hook maps them onto the character's torso.
#pragma once
#include "essence.h"
#include <algorithm>

namespace fx {

enum { kTwoSided = 1, kColorScale = 2, kFadeIn = 4, kFadeOut = 8, kSizeScale = 16, kSpin = 32, kUniform = 64, kRespawn = 128 };
enum { kAutoSpawn = 1, kParticleColor = 2, kRelative = 4, kBlendSub = 8, kRandomSub = 16 };
enum { kRevolve = 1, kRevScale = 2, kAutoReset = 4 };
enum { kRegular = 0, kAlphaBlend = 1, kModulated = 2, kTranslucent = 3, kAlphaModulate = 4, kDarken = 5, kBrighten = 6 };

struct ColKey { float t; uint8_t r, g, b, a; };
struct SizeKey { float t, s; };
struct RevKey { float t, v[3]; };
struct Emitter {
    int kind = 0, draw = 3, flags = 0, flags2 = 0, tex = -1, mesh = -1, tex2 = -1;     // kind 0 sprite, 1 static mesh, 2 vertex-animated mesh; tex2 = a second texture multiplied over tex and panning by (panU, panV) per second
    float panU = 0.f, panV = 0.f;
    float opacity = 1.f; int maxP = 10;
    float lifeMin = 4.f, lifeMax = 4.f, ipps = 0.f, pps = 0.f, delayMin = 0.f, delayMax = 0.f, fadeInEnd = 0.f, fadeOutStart = 0.f;
    float loc[6] = {}, vel[6] = {}, vloss[6] = {}, acc[3] = {}, size[6] = {}, sstart[6] = {}, srate[6] = {}, cmul[6] = {};
    float colorRepeats = 1.f; int usub = 1, vsub = 1, sub0 = 0, sub1 = 0;
    std::vector<ColKey> col; std::vector<SizeKey> sz;
    int locShape = 0, velDir = 0, dirAs = 0, rflags = 0;           // location shape (0 box, 1 sphere, 2 polar), velocity direction (1 inward, 2 outward from the start position), sprite direction (1 = up axis along the velocity), revolution / auto reset flags
    float projN[3] = { 1.f, 0.f, 0.f };
    float polar[6] = {}, sphereR[2] = {}, locOfs[3] = {}, ccw[3] = { 0.5f, 0.5f, 0.5f }, rps[6] = {}, rcen[6] = {}, resetMin = 0.f, resetMax = 0.f, warmup = 0.f;
    std::vector<RevKey> rev;
};
struct Mesh { std::string name; int tex = -1, nv = 0, nt = 0; std::vector<float> v; std::vector<uint16_t> idx; };       // v: x y z u v
struct VMesh { std::string name; int nv = 0, nf = 0, nt = 0; std::vector<uint16_t> idx; std::vector<float> uv, frames; };   // frames: nf x nv x (x y z)
struct Effect { std::string name; std::vector<Emitter> em; };
struct Pack { std::vector<uint8_t> buf; std::vector<ess::Tex> texs; std::vector<Mesh> meshes; std::vector<VMesh> vmeshes; std::vector<Effect> effects; };

inline bool LoadPack(const wchar_t* path, Pack& out, std::string* err = nullptr) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) { if (err) *err = "cannot open effect pack"; return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    out.buf.resize((size_t)sz);
    if (fread(out.buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); if (err) *err = "short read"; return false; }
    fclose(f);
    ess::Reader r(out.buf.data(), out.buf.size());
    char magic[4]; for (int i = 0; i < 4; ++i) magic[i] = (char)r.get<uint8_t>();
    if (memcmp(magic, "EFX3", 4) != 0) { if (err) *err = "bad effect pack magic"; return false; }
    const uint32_t nTex = r.get<uint32_t>(), nMesh = r.get<uint32_t>(), nVMesh = r.get<uint32_t>(), nEff = r.get<uint32_t>();
    for (uint32_t i = 0; i < nTex && r.ok; ++i) {
        ess::Tex t; t.name = r.cstr(); t.fmt = r.get<uint32_t>(); t.w = r.get<uint32_t>(); t.h = r.get<uint32_t>(); t.levels = (int)r.get<uint32_t>();
        if (t.levels > 12) { if (err) *err = "too many mip levels"; return false; }
        for (int l = 0; l < t.levels; ++l) { t.size[l] = r.get<uint32_t>(); t.data[l] = r.skip(t.size[l]); }
        out.texs.push_back(t);
    }
    for (uint32_t i = 0; i < nMesh && r.ok; ++i) {
        Mesh m; m.name = r.cstr(); m.nv = (int)r.get<uint32_t>(); m.nt = (int)r.get<uint32_t>(); m.tex = r.get<int32_t>();
        if (m.nv <= 0 || m.nv > 20000 || m.nt <= 0 || m.nt > 40000) { if (err) *err = "bad effect mesh size"; return false; }
        m.v.resize((size_t)m.nv * 5); for (auto& x : m.v) x = r.get<float>();
        m.idx.resize((size_t)m.nt * 3); for (auto& x : m.idx) { x = r.get<uint16_t>(); if (x >= m.nv) { if (err) *err = "bad effect mesh index"; return false; } }
        out.meshes.push_back(std::move(m));
    }
    for (uint32_t i = 0; i < nVMesh && r.ok; ++i) {
        VMesh m; m.name = r.cstr(); m.nv = (int)r.get<uint32_t>(); m.nf = (int)r.get<uint32_t>(); m.nt = (int)r.get<uint32_t>();
        if (m.nv <= 0 || m.nv > 2000 || m.nf <= 0 || m.nf > 400 || m.nt <= 0 || m.nt > 8000) { if (err) *err = "bad vertex mesh size"; return false; }
        m.idx.resize((size_t)m.nt * 3); for (auto& x : m.idx) { x = r.get<uint16_t>(); if (x >= m.nv) { if (err) *err = "bad vertex mesh index"; return false; } }
        m.uv.resize((size_t)m.nv * 2); for (auto& x : m.uv) x = r.get<float>();
        m.frames.resize((size_t)m.nf * m.nv * 3); for (auto& x : m.frames) x = r.get<float>();
        out.vmeshes.push_back(std::move(m));
    }
    for (uint32_t i = 0; i < nEff && r.ok; ++i) {
        Effect e; e.name = r.cstr(); const uint32_t ne = r.get<uint32_t>();
        for (uint32_t k = 0; k < ne && r.ok; ++k) {
            Emitter m;
            m.kind = r.get<uint8_t>(); m.draw = r.get<uint8_t>(); m.flags = r.get<uint8_t>(); m.flags2 = r.get<uint8_t>();
            m.tex = r.get<int32_t>(); m.mesh = r.get<int32_t>(); m.tex2 = r.get<int32_t>(); m.panU = r.get<float>(); m.panV = r.get<float>();
            m.opacity = r.get<float>(); m.maxP = r.get<int32_t>();
            m.lifeMin = r.get<float>(); m.lifeMax = r.get<float>(); m.ipps = r.get<float>(); m.pps = r.get<float>();
            m.delayMin = r.get<float>(); m.delayMax = r.get<float>(); m.fadeInEnd = r.get<float>(); m.fadeOutStart = r.get<float>();
            for (float* a : { m.loc, m.vel, m.vloss }) for (int j = 0; j < 6; ++j) a[j] = r.get<float>();
            for (int j = 0; j < 3; ++j) m.acc[j] = r.get<float>();
            for (float* a : { m.size, m.sstart, m.srate, m.cmul }) for (int j = 0; j < 6; ++j) a[j] = r.get<float>();
            m.colorRepeats = r.get<float>(); const uint32_t nc = r.get<uint32_t>();
            if (nc > 64) { if (err) *err = "bad colour scale"; return false; }
            for (uint32_t j = 0; j < nc; ++j) { ColKey c; c.t = r.get<float>(); c.r = r.get<uint8_t>(); c.g = r.get<uint8_t>(); c.b = r.get<uint8_t>(); c.a = r.get<uint8_t>(); m.col.push_back(c); }
            m.usub = r.get<int32_t>(); m.vsub = r.get<int32_t>(); m.sub0 = r.get<int32_t>(); m.sub1 = r.get<int32_t>(); const uint32_t ns = r.get<uint32_t>();
            if (ns > 64) { if (err) *err = "bad size scale"; return false; }
            for (uint32_t j = 0; j < ns; ++j) { SizeKey s; s.t = r.get<float>(); s.s = r.get<float>(); m.sz.push_back(s); }
            m.locShape = r.get<uint8_t>(); m.velDir = r.get<uint8_t>(); m.dirAs = r.get<uint8_t>(); m.rflags = r.get<uint8_t>();
            for (int j = 0; j < 3; ++j) m.projN[j] = r.get<float>();
            for (int j = 0; j < 6; ++j) m.polar[j] = r.get<float>();
            for (int j = 0; j < 2; ++j) m.sphereR[j] = r.get<float>();
            for (int j = 0; j < 3; ++j) m.locOfs[j] = r.get<float>();
            for (int j = 0; j < 3; ++j) m.ccw[j] = r.get<float>();
            for (int j = 0; j < 6; ++j) m.rps[j] = r.get<float>();
            for (int j = 0; j < 6; ++j) m.rcen[j] = r.get<float>();
            m.resetMin = r.get<float>(); m.resetMax = r.get<float>(); m.warmup = r.get<float>();
            const uint32_t nr = r.get<uint32_t>();
            if (nr > 64) { if (err) *err = "bad revolution scale"; return false; }
            for (uint32_t j = 0; j < nr; ++j) { RevKey k; k.t = r.get<float>(); for (int c = 0; c < 3; ++c) k.v[c] = r.get<float>(); m.rev.push_back(k); }
            if (m.usub < 1) m.usub = 1;
            if (m.vsub < 1) m.vsub = 1;
            if (m.maxP < 1) m.maxP = 1;
            if (m.maxP > 64) m.maxP = 64;
            if ((m.kind == 1 && (m.mesh < 0 || m.mesh >= (int)out.meshes.size())) || (m.kind == 2 && (m.mesh < 0 || m.mesh >= (int)out.vmeshes.size())) || m.tex >= (int)out.texs.size() || m.tex2 >= (int)out.texs.size()) { if (err) *err = "emitter refers to a missing mesh / texture"; return false; }
            e.em.push_back(std::move(m));
        }
        out.effects.push_back(std::move(e));
    }
    if (!r.ok) { if (err) *err = "truncated effect pack"; return false; }
    return true;
}

inline int FindEffect(const Pack& p, const std::string& name) {
    const std::string n = ess::Lower(name);
    for (size_t i = 0; i < p.effects.size(); ++i) if (ess::Lower(p.effects[i].name) == n) return (int)i;
    return -1;
}

// ---------------------------------------------------------------------------------------------------- simulation
struct Rng {
    uint32_t s = 2463534242u;
    float f() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (float)(s & 0xFFFFFF) / 16777216.f; }
    float range(float a, float b) { return a + (b - a) * f(); }
};
struct Particle {
    bool alive = false;
    float age = 0.f, life = 1.f, delay = 0.f;
    float vel[3] = {}, vloss[3] = {}, pos0[3] = {}, size[3] = {}, spin0[3] = {}, rate[3] = {}, cmul[3] = {}, cen[3] = {}, rrate[3] = {};
    float rnd = 0.f;
};
struct EmState { std::vector<Particle> p; float acc = 0.f; int spawned = 0; float resetT = 0.f, resetAt = 0.f; };
struct Instance {
    const Effect* eff = nullptr; std::vector<EmState> st; Rng rng; float time = 0.f;
    void Reset(const Effect* e, uint32_t seed) { eff = e; st.assign(e ? e->em.size() : 0, EmState()); rng.s = seed ? seed : 2463534242u; time = 0.f; if (e) for (size_t i = 0; i < e->em.size(); ++i) st[i].p.resize((size_t)e->em[i].maxP); }
};

inline void Spawn(const Emitter& e, Particle& q, Rng& rn) {
    q = Particle(); q.alive = true;
    q.life = rn.range(e.lifeMin, e.lifeMax); if (q.life < 0.02f) q.life = 0.02f;
    q.delay = rn.range(e.delayMin, e.delayMax);
    float dirv[3] = { 0.f, 0.f, 0.f };
    if (e.locShape == 1) {                                               // sphere
        float v[3]; float n2;
        do { for (int k = 0; k < 3; ++k) v[k] = rn.range(-1.f, 1.f); n2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; } while (n2 > 1.f || n2 < 1e-4f);
        const float n = std::sqrt(n2), rad = rn.range(e.sphereR[0], e.sphereR[1]);
        for (int k = 0; k < 3; ++k) { dirv[k] = v[k] / n; q.pos0[k] = dirv[k] * rad; }
    } else if (e.locShape == 2) {                                        // polar: pitch above the xy plane (deg), yaw (deg), radius
        const float pitch = rn.range(e.polar[0], e.polar[1]) * 0.0174532925f, yaw = rn.range(e.polar[2], e.polar[3]) * 0.0174532925f, rad = rn.range(e.polar[4], e.polar[5]);
        dirv[0] = std::cos(pitch) * std::cos(yaw); dirv[1] = std::cos(pitch) * std::sin(yaw); dirv[2] = std::sin(pitch);
        for (int k = 0; k < 3; ++k) q.pos0[k] = dirv[k] * rad;
    } else {
        for (int k = 0; k < 3; ++k) q.pos0[k] = rn.range(e.loc[2 * k], e.loc[2 * k + 1]);
        const float n = std::sqrt(q.pos0[0] * q.pos0[0] + q.pos0[1] * q.pos0[1] + q.pos0[2] * q.pos0[2]);
        if (n > 1e-5f) for (int k = 0; k < 3; ++k) dirv[k] = q.pos0[k] / n;
    }
    for (int k = 0; k < 3; ++k) q.pos0[k] += e.locOfs[k];
    for (int k = 0; k < 3; ++k) {
        q.vel[k] = rn.range(e.vel[2 * k], e.vel[2 * k + 1]);
        q.vloss[k] = rn.range(e.vloss[2 * k], e.vloss[2 * k + 1]);
        q.size[k] = rn.range(e.size[2 * k], e.size[2 * k + 1]);
        q.spin0[k] = rn.range(e.sstart[2 * k], e.sstart[2 * k + 1]);
        q.rate[k] = rn.range(e.srate[2 * k], e.srate[2 * k + 1]) * (rn.f() < e.ccw[k] ? -1.f : 1.f);
        q.cmul[k] = rn.range(e.cmul[2 * k], e.cmul[2 * k + 1]);
        q.rrate[k] = rn.range(e.rps[2 * k], e.rps[2 * k + 1]);
        q.cen[k] = rn.range(e.rcen[2 * k], e.rcen[2 * k + 1]);
    }
    if (e.velDir == 1 || e.velDir == 2) {                                // velocity along the line start position <-> owner
        const float sgn = e.velDir == 2 ? 1.f : -1.f;
        for (int k = 0; k < 3; ++k) q.vel[k] = dirv[k] * q.vel[k] * sgn;
    }
    if (e.flags & kUniform) { q.size[1] = q.size[0]; q.size[2] = q.size[0]; }
    if (e.kind != 0) for (int k = 0; k < 3; ++k) q.size[k] = std::fabs(q.size[k]);          // a negative mesh scale would flip the wings upside down in the effect's own axes
    q.rnd = rn.f();
}

inline void Step(Instance& in, float dt) {
    if (!in.eff) return;
    if (dt > 0.1f) dt = 0.1f;
    in.time += dt;
    for (size_t i = 0; i < in.eff->em.size(); ++i) {
        const Emitter& e = in.eff->em[i]; EmState& s = in.st[i];
        int live = 0;
        for (Particle& q : s.p) {
            if (!q.alive) continue;
            if (q.delay > 0.f) { q.delay -= dt; ++live; continue; }
            q.age += dt;
            if (q.age >= q.life) q.alive = false; else ++live;
        }
        float rate = e.ipps;
        if (e.flags2 & kAutoSpawn) rate = (float)e.maxP / (e.lifeMax > 0.05f ? e.lifeMax : 0.05f);
        else if (rate <= 0.f) rate = e.pps;
        if ((e.rflags & kAutoReset) && !(e.flags & kRespawn) && s.spawned >= e.maxP && live == 0) {
            s.resetT += dt; if (s.resetAt <= 0.f) s.resetAt = in.rng.range(e.resetMin, e.resetMax) + 0.001f;
            if (s.resetT >= s.resetAt) { s.spawned = 0; s.resetT = 0.f; s.resetAt = 0.f; s.acc = 0.f; }
        }
        if (rate <= 0.f) continue;
        const bool respawn = (e.flags & kRespawn) != 0;
        s.acc += rate * dt; if (s.acc > (float)e.maxP) s.acc = (float)e.maxP;
        while (s.acc >= 1.f && live < e.maxP && (respawn || s.spawned < e.maxP)) {
            for (Particle& q : s.p) if (!q.alive) { Spawn(e, q, in.rng); ++live; ++s.spawned; break; }
            s.acc -= 1.f;
        }
        if (!respawn && s.spawned >= e.maxP) s.acc = 0.f;
    }
}

// state of one particle for drawing: emitter-local position, per-axis size, rotation (radians), colour, alpha, sprite-sheet frame
struct Draw { float pos[3], vel[3], size[3], rot[3], rgb[3], alpha, t; int frame0, frame1; float frameMix; };

inline float EvalSize(const Emitter& e, float t) {
    if (!(e.flags & kSizeScale) || e.sz.empty()) return 1.f;
    if (t <= e.sz.front().t) return e.sz.front().s;
    for (size_t i = 1; i < e.sz.size(); ++i)
        if (t <= e.sz[i].t) { const float d = e.sz[i].t - e.sz[i - 1].t; const float k = d > 1e-6f ? (t - e.sz[i - 1].t) / d : 1.f; return e.sz[i - 1].s + (e.sz[i].s - e.sz[i - 1].s) * k; }
    return e.sz.back().s;
}
inline void EvalColor(const Emitter& e, float t, float* rgba) {
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 1.f;
    if (!(e.flags & kColorScale) || e.col.empty()) return;
    if (e.colorRepeats > 1.f) { t *= e.colorRepeats; t -= std::floor(t); }
    const ColKey* a = &e.col.front(); const ColKey* b = a;
    if (t >= e.col.back().t) a = b = &e.col.back();
    else for (size_t i = 1; i < e.col.size(); ++i) if (t <= e.col[i].t) { a = &e.col[i - 1]; b = &e.col[i]; break; }
    const float d = b->t - a->t; const float k = d > 1e-6f ? std::min(1.f, std::max(0.f, (t - a->t) / d)) : 0.f;
    rgba[0] = (a->r + (b->r - a->r) * k) / 255.f; rgba[1] = (a->g + (b->g - a->g) * k) / 255.f; rgba[2] = (a->b + (b->b - a->b) * k) / 255.f; rgba[3] = (a->a + (b->a - a->a) * k) / 255.f;
}

inline bool Eval(const Emitter& e, const Particle& q, Draw& d) {
    if (!q.alive || q.delay > 0.f) return false;
    const float t = std::min(1.f, q.age / q.life);
    d.t = t;
    for (int k = 0; k < 3; ++k) {
        const float L = q.vloss[k];
        const float travel = L > 1e-4f ? q.vel[k] * (1.f - std::exp(-L * q.age)) / L : q.vel[k] * q.age;
        d.pos[k] = q.pos0[k] + travel + 0.5f * e.acc[k] * q.age * q.age;
    }
    for (int k = 0; k < 3; ++k) d.vel[k] = q.vel[k] * std::exp(-q.vloss[k] * q.age) + e.acc[k] * q.age;
    if (e.rflags & kRevolve) {                                           // revolve around the centre (rotation about x, y, z)
        float sv[3] = { 1.f, 1.f, 1.f };
        if ((e.rflags & kRevScale) && !e.rev.empty()) {
            if (t <= e.rev.front().t) for (int k = 0; k < 3; ++k) sv[k] = e.rev.front().v[k];
            else if (t >= e.rev.back().t) for (int k = 0; k < 3; ++k) sv[k] = e.rev.back().v[k];
            else for (size_t i = 1; i < e.rev.size(); ++i) if (t <= e.rev[i].t) { const float dd = e.rev[i].t - e.rev[i - 1].t; const float kk = dd > 1e-6f ? (t - e.rev[i - 1].t) / dd : 1.f; for (int c = 0; c < 3; ++c) sv[c] = e.rev[i - 1].v[c] + (e.rev[i].v[c] - e.rev[i - 1].v[c]) * kk; break; }
        }
        const float ax = q.rrate[0] * sv[0] * q.age * 6.2831853f, ay = q.rrate[1] * sv[1] * q.age * 6.2831853f, az = q.rrate[2] * sv[2] * q.age * 6.2831853f;
        float p[3] = { d.pos[0] - q.cen[0], d.pos[1] - q.cen[1], d.pos[2] - q.cen[2] }, o[3];
        float c = std::cos(ax), s = std::sin(ax); o[0] = p[0]; o[1] = c * p[1] - s * p[2]; o[2] = s * p[1] + c * p[2]; p[0] = o[0]; p[1] = o[1]; p[2] = o[2];
        c = std::cos(ay); s = std::sin(ay); o[0] = c * p[0] + s * p[2]; o[1] = p[1]; o[2] = -s * p[0] + c * p[2]; p[0] = o[0]; p[1] = o[1]; p[2] = o[2];
        c = std::cos(az); s = std::sin(az); o[0] = c * p[0] - s * p[1]; o[1] = s * p[0] + c * p[1]; o[2] = p[2];
        for (int k = 0; k < 3; ++k) d.pos[k] = q.cen[k] + o[k];
    }
    const float sc = EvalSize(e, t);
    for (int k = 0; k < 3; ++k) d.size[k] = q.size[k] * sc;
    for (int k = 0; k < 3; ++k) d.rot[k] = (e.flags & kSpin) ? (q.spin0[k] + q.rate[k] * q.age) * 6.2831853f : q.spin0[k] * 6.2831853f;
    float c[4]; EvalColor(e, t, c);
    for (int k = 0; k < 3; ++k) d.rgb[k] = c[k] * q.cmul[k];
    float a = e.opacity * c[3];
    if ((e.flags & kFadeIn) && e.fadeInEnd > 1e-4f && q.age < e.fadeInEnd) a *= q.age / e.fadeInEnd;
    if ((e.flags & kFadeOut) && q.age > e.fadeOutStart) { const float span = q.life - e.fadeOutStart; a *= span > 1e-4f ? std::max(0.f, (q.life - q.age) / span) : 0.f; }
    d.alpha = a;
    const int nf = e.usub * e.vsub;
    d.frame0 = d.frame1 = 0; d.frameMix = 0.f;
    if (nf > 1) {
        if (e.flags2 & kRandomSub) d.frame0 = d.frame1 = std::min(nf - 1, (int)(q.rnd * nf));
        else {
            const int a0 = e.sub0, a1 = e.sub1 > 0 ? e.sub1 : (e.sub0 > 0 ? e.sub0 : 0);
            const float fpos = a0 + (a1 - a0) * t;
            d.frame0 = std::min(nf - 1, std::max(0, (int)fpos));
            d.frame1 = std::min(nf - 1, d.frame0 + 1);
            d.frameMix = (e.flags2 & kBlendSub) ? fpos - std::floor(fpos) : 0.f;
        }
    }
    return a > 0.002f;
}

// run the emitters for `secs` seconds without drawing: effects start in their steady state (UE2 'warm-up')
inline void Warmup(Instance& in, float secs) { for (float t = 0.f; t < secs; t += 0.05f) Step(in, 0.05f); }

// ---------------------------------------------------------------------------------------------------- geometry
struct Vertex { float x, y, z; uint32_t col; float u, v; };
struct Batch { int tex, style; unsigned v0, nv, i0, ntri; int tex2; float panU, panV; };
struct View { float right[3] = { 1.f, 0.f, 0.f }, up[3] = { 0.f, 0.f, 1.f }, fwd[3] = { 0.f, 1.f, 0.f }; };   // camera axes in the actor space
// where the effect sits: origin O and torso frame B (columns: left, front, up) in the actor space; `axis` = how the effect's own axes map onto the torso
struct Frame { float O[3] = {}, B[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; float scale = 1.f, gain = 1.f; int axis = 0; bool wingFlip = true; };   // wingFlip: the vertex-animated wings are modelled facing the other way (x = backwards)

inline void ToTorso(int axis, float lx, float ly, float lz, float* abc) {
    switch (axis) {
        case 1: abc[0] = ly; abc[1] = -lx; abc[2] = lz; break;
        case 2: abc[0] = -ly; abc[1] = -lz; abc[2] = lx; break;
        case 3: abc[0] = -ly; abc[1] = lz; abc[2] = -lx; break;
        default: abc[0] = -ly; abc[1] = lx; abc[2] = lz; break;
    }
}
inline void PointToActor(const Frame& fr, float lx, float ly, float lz, float* o) {
    float v[3]; ToTorso(fr.axis, lx * fr.scale, ly * fr.scale, lz * fr.scale, v);
    for (int k = 0; k < 3; ++k) o[k] = fr.O[k] + fr.B[k * 3] * v[0] + fr.B[k * 3 + 1] * v[1] + fr.B[k * 3 + 2] * v[2];
}
inline void DirToActor(const Frame& fr, float lx, float ly, float lz, float* o) {
    float v[3]; ToTorso(fr.axis, lx, ly, lz, v);
    for (int k = 0; k < 3; ++k) o[k] = fr.B[k * 3] * v[0] + fr.B[k * 3 + 1] * v[1] + fr.B[k * 3 + 2] * v[2];
}
inline uint32_t PackColor(int a, int r, int g, int b) { return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }

// Turns every live particle of the effect into triangles (one batch per emitter).
inline void Gather(const Pack& pk, const Instance& in, const Frame& fr, const View& vw, std::vector<Vertex>& vs, std::vector<uint16_t>& is, std::vector<Batch>& bs, size_t maxV, size_t maxI) {
    if (!in.eff) return;                                                  // appends: the caller clears the arrays (several effects can share them)
    for (size_t ei = 0; ei < in.eff->em.size(); ++ei) {
        const Emitter& e = in.eff->em[ei]; const EmState& st = in.st[ei];
        Batch b = { e.tex, e.draw, (unsigned)vs.size(), 0, (unsigned)is.size(), 0, e.tex2, e.panU, e.panV };
        const bool additive = e.draw == kTranslucent || e.draw == kBrighten || e.draw == kDarken || e.draw == kModulated;
        for (const Particle& q : st.p) {
            Draw d; if (!Eval(e, q, d)) continue;
            const float a = std::min(1.f, d.alpha);
            const float mul = additive ? a * fr.gain : fr.gain;
            const int cr = (int)(std::min(1.f, std::max(0.f, d.rgb[0] * mul)) * 255.f), cg = (int)(std::min(1.f, std::max(0.f, d.rgb[1] * mul)) * 255.f), cb = (int)(std::min(1.f, std::max(0.f, d.rgb[2] * mul)) * 255.f);
            const uint32_t col = PackColor(additive ? 255 : (int)(a * 255.f), cr, cg, cb);
            if (vs.size() + 64 > maxV || is.size() + 1024 > maxI) break;
            if (e.kind == 0) {                                                    // sprite
                const float hx = d.size[0] * 0.5f, hy = d.size[1] * 0.5f;
                const float cs = std::cos(d.rot[0]), sn = std::sin(d.rot[0]);
                float lp[3]; PointToActor(fr, d.pos[0], d.pos[1], d.pos[2], lp);
                float axX[3], axY[3];                                             // the quad's width / height directions (actor space)
                if (e.dirAs == 4) {                                               // lies in the plane perpendicular to the projection normal, spinning about it
                    float n[3] = { e.projN[0], e.projN[1], e.projN[2] };
                    const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (nl < 1e-4f) { n[0] = 1.f; n[1] = n[2] = 0.f; } else { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
                    float ref[3] = { 0.f, 0.f, 1.f }; if (std::fabs(n[2]) > 0.9f) { ref[0] = 0.f; ref[1] = 1.f; ref[2] = 0.f; }
                    float u[3] = { n[1] * ref[2] - n[2] * ref[1], n[2] * ref[0] - n[0] * ref[2], n[0] * ref[1] - n[1] * ref[0] };
                    const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]); for (int k = 0; k < 3; ++k) u[k] /= ul;
                    const float v[3] = { n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0] };
                    float lx[3], ly[3];
                    for (int k = 0; k < 3; ++k) { lx[k] = u[k] * cs + v[k] * sn; ly[k] = -u[k] * sn + v[k] * cs; }
                    DirToActor(fr, lx[0], lx[1], lx[2], axX); DirToActor(fr, ly[0], ly[1], ly[2], axY);
                } else {
                    for (int k = 0; k < 3; ++k) { axX[k] = vw.right[k] * cs + vw.up[k] * sn; axY[k] = -vw.right[k] * sn + vw.up[k] * cs; }
                    if (e.dirAs == 1 || e.dirAs == 2) {                           // stretched along the velocity (still facing the camera)
                        float w[3]; DirToActor(fr, d.vel[0], d.vel[1], d.vel[2], w);
                        const float wf = w[0] * vw.fwd[0] + w[1] * vw.fwd[1] + w[2] * vw.fwd[2];
                        float u[3] = { w[0] - wf * vw.fwd[0], w[1] - wf * vw.fwd[1], w[2] - wf * vw.fwd[2] };
                        const float un = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                        if (un > 1e-3f) {
                            for (int k = 0; k < 3; ++k) u[k] /= un;
                            float r[3] = { u[1] * vw.fwd[2] - u[2] * vw.fwd[1], u[2] * vw.fwd[0] - u[0] * vw.fwd[2], u[0] * vw.fwd[1] - u[1] * vw.fwd[0] };
                            if (r[0] * vw.right[0] + r[1] * vw.right[1] + r[2] * vw.right[2] < 0.f) { r[0] = -r[0]; r[1] = -r[1]; r[2] = -r[2]; }
                            if (e.dirAs == 1) for (int k = 0; k < 3; ++k) { axX[k] = r[k]; axY[k] = u[k]; }
                            else for (int k = 0; k < 3; ++k) { axX[k] = u[k]; axY[k] = -r[k]; }
                        }
                    }
                }
                const float fu = 1.f / (float)e.usub, fv = 1.f / (float)e.vsub; const int fr0 = d.frame0;
                const float u0 = (float)(fr0 % e.usub) * fu, v0 = (float)(fr0 / e.usub) * fv;
                const float cx[4] = { -1.f, 1.f, 1.f, -1.f }, cy[4] = { -1.f, -1.f, 1.f, 1.f }, tu[4] = { 0.f, 1.f, 1.f, 0.f }, tv[4] = { 1.f, 1.f, 0.f, 0.f };
                const unsigned base = (unsigned)vs.size();
                for (int k = 0; k < 4; ++k) {
                    const float px = cx[k] * hx * fr.scale, py = cy[k] * hy * fr.scale;
                    Vertex v; v.x = lp[0] + axX[0] * px + axY[0] * py; v.y = lp[1] + axX[1] * px + axY[1] * py; v.z = lp[2] + axX[2] * px + axY[2] * py; v.col = col; v.u = u0 + tu[k] * fu; v.v = v0 + tv[k] * fv;
                    vs.push_back(v);
                }
                const uint16_t quad[6] = { (uint16_t)base, (uint16_t)(base + 1), (uint16_t)(base + 2), (uint16_t)base, (uint16_t)(base + 2), (uint16_t)(base + 3) };
                for (uint16_t x : quad) is.push_back(x);
                b.ntri += 2;
            } else if (e.kind == 2) {                                             // vertex-animated mesh (wings): one animation cycle per particle life
                const VMesh& m = pk.vmeshes[(size_t)e.mesh];
                const float cx = std::cos(d.rot[0]), sx = std::sin(d.rot[0]), cy = std::cos(d.rot[1]), sy = std::sin(d.rot[1]), cz = std::cos(d.rot[2]), sz = std::sin(d.rot[2]);
                const float R[9] = { cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx, sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx, -sy, cy * sx, cy * cx };
                const float fpos = d.t * (float)(m.nf - 1); const int f0 = std::min(m.nf - 1, (int)fpos), f1 = std::min(m.nf - 1, f0 + 1); const float fm = fpos - (float)f0;
                const unsigned first = (unsigned)vs.size();
                if (vs.size() + (size_t)m.nv + 64 > maxV || is.size() + m.idx.size() + 64 > maxI) break;
                for (int i = 0; i < m.nv; ++i) {
                    const float* a0 = &m.frames[((size_t)f0 * m.nv + i) * 3]; const float* a1 = &m.frames[((size_t)f1 * m.nv + i) * 3];
                    const float p[3] = { (a0[0] + (a1[0] - a0[0]) * fm) * d.size[0], (a0[1] + (a1[1] - a0[1]) * fm) * d.size[1], (a0[2] + (a1[2] - a0[2]) * fm) * d.size[2] };
                    const float lx = R[0] * p[0] + R[1] * p[1] + R[2] * p[2] + d.pos[0], ly = R[3] * p[0] + R[4] * p[1] + R[5] * p[2] + d.pos[1], lz = R[6] * p[0] + R[7] * p[1] + R[8] * p[2] + d.pos[2];
                    float w[3]; if (fr.wingFlip) PointToActor(fr, -lx, -ly, lz, w); else PointToActor(fr, lx, ly, lz, w);
                    Vertex v; v.x = w[0]; v.y = w[1]; v.z = w[2]; v.col = col; v.u = m.uv[(size_t)i * 2]; v.v = m.uv[(size_t)i * 2 + 1];
                    vs.push_back(v);
                }
                for (uint16_t ix : m.idx) is.push_back((uint16_t)(first + ix));
                b.ntri += (unsigned)(m.idx.size() / 3);
            } else {                                                              // mesh particle
                const Mesh& m = pk.meshes[(size_t)e.mesh];
                const float cx = std::cos(d.rot[0]), sx = std::sin(d.rot[0]), cy = std::cos(d.rot[1]), sy = std::sin(d.rot[1]), cz = std::cos(d.rot[2]), sz = std::sin(d.rot[2]);
                const float R[9] = { cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx,          // R = Rz * Ry * Rx
                                     sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx,
                                     -sy,     cy * sx,                cy * cx };
                const unsigned first = (unsigned)vs.size();
                if (vs.size() + (size_t)m.nv + 64 > maxV || is.size() + m.idx.size() + 64 > maxI) break;
                for (int i = 0; i < m.nv; ++i) {
                    const float* mv = &m.v[(size_t)i * 5];
                    const float p[3] = { mv[0] * d.size[0], mv[1] * d.size[1], mv[2] * d.size[2] };
                    const float lx = R[0] * p[0] + R[1] * p[1] + R[2] * p[2] + d.pos[0], ly = R[3] * p[0] + R[4] * p[1] + R[5] * p[2] + d.pos[1], lz = R[6] * p[0] + R[7] * p[1] + R[8] * p[2] + d.pos[2];
                    float w[3]; PointToActor(fr, lx, ly, lz, w);
                    Vertex v; v.x = w[0]; v.y = w[1]; v.z = w[2]; v.col = col; v.u = mv[3]; v.v = mv[4];
                    vs.push_back(v);
                }
                for (uint16_t ix : m.idx) is.push_back((uint16_t)(first + ix));
                b.ntri += (unsigned)(m.idx.size() / 3);
            }
        }
        b.nv = (unsigned)vs.size() - b.v0;
        if (b.ntri) bs.push_back(b);
    }
}

}  // namespace fx
