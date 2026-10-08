// essence.h - baked Essence mantles: pack loader, pose evaluation, CPU skinning.  No engine or Direct3D dependency (unit-testable).
// Pack layout: see tools/build_capes.py.  Quaternions in the pack are already conjugated: (x, y, z, w).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>

namespace ess {

constexpr int kMaxBones = 64;

struct Bone { std::string name; int parent = -1; float q[4] = { 0, 0, 0, 1 }; float t[3] = { 0, 0, 0 }; };
struct Track { uint8_t flags = 0xFF; float sq[4] = { 0, 0, 0, 1 }; float sp[3] = { 0, 0, 0 }; const uint8_t* q = nullptr; const uint8_t* p = nullptr; };
struct Seq { std::string name; int frames = 1; float rate = 30.f; std::vector<Track> tr; int fname = -1; };
struct Anim {
    std::string body;
    std::vector<Bone> bones;                                    // animation skeleton (names + parents; bind values unused)
    std::vector<Seq> seqs;
    std::unordered_map<std::string, int> byName;                // lowercase sequence name -> index
    std::unordered_map<int, int> byFName;                       // engine FName index -> sequence (filled by the engine glue)
};
struct Tex { std::string name; uint32_t fmt = 0, w = 0, h = 0; int levels = 0; const uint8_t* data[12] = {}; uint32_t size[12] = {}; };
struct Material { int tex = 0, alphaTest = 0, alphaRef = 0, twoSided = 0; };
struct Section { int first = 0, count = 0, mat = 0; };
struct Mantle {
    std::string body, meshName; int design = 0;
    std::vector<Bone> bones;                                    // mesh skeleton with bind pose (local, conjugated quats)
    int np = 0;
    std::vector<float> pts;                                     // np * 3
    std::vector<uint8_t> infBone;                               // np * 4
    std::vector<float> infW;                                    // np * 4
    int nw = 0;
    std::vector<uint16_t> wpt;                                  // wedge -> point
    std::vector<float> wuv;                                     // nw * 2
    int nf = 0;
    std::vector<uint16_t> faces;                                // nf * 3 wedge indices
    std::vector<Section> secs;
    std::vector<Material> mats;
    // derived
    std::vector<float> bindR;                                   // per mesh bone global rotation (9), translation (3) in bindT
    std::vector<float> bindT;
};
struct FxTex { int tex = -1; bool env = false, mask = false; float panU = 0.f, panV = 0.f; };
struct FxLayer { int kind = 0; std::vector<FxTex> texs; };            // kind 0 = additive (shine, glow), 1 = alpha blend through the mask (animated fire in the cloth)
struct Pack {
    std::vector<uint8_t> buf;
    std::unordered_map<std::string, std::vector<FxLayer>> fx;      // lowercase base texture name -> effect layers (optional FXS1 section)
    std::vector<Anim> anims;
    std::vector<Tex> texs;
    std::vector<Mantle> mantles;
};

// ---------------------------------------------------------------------------------------------
// small math (column-vector convention, quaternion = (x, y, z, w))
// ---------------------------------------------------------------------------------------------
inline void QuatToMat(const float* q, float* R) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float n = x * x + y * y + z * z + w * w;
    const float s = n > 0.f ? 2.f / n : 0.f;
    const float xx = x * x * s, yy = y * y * s, zz = z * z * s, xy = x * y * s, xz = x * z * s, yz = y * z * s, wx = w * x * s, wy = w * y * s, wz = w * z * s;
    R[0] = 1 - yy - zz; R[1] = xy - wz;     R[2] = xz + wy;
    R[3] = xy + wz;     R[4] = 1 - xx - zz; R[5] = yz - wx;
    R[6] = xz - wy;     R[7] = yz + wx;     R[8] = 1 - xx - yy;
}
inline void MatToQuat(const float* R, float* q) {                // inverse of QuatToMat (x, y, z, w)
    const float tr = R[0] + R[4] + R[8];
    if (tr > 0.f) { const float s = std::sqrt(tr + 1.f) * 2.f; q[3] = 0.25f * s; q[0] = (R[7] - R[5]) / s; q[1] = (R[2] - R[6]) / s; q[2] = (R[3] - R[1]) / s; }
    else if (R[0] > R[4] && R[0] > R[8]) { const float s = std::sqrt(1.f + R[0] - R[4] - R[8]) * 2.f; q[3] = (R[7] - R[5]) / s; q[0] = 0.25f * s; q[1] = (R[1] + R[3]) / s; q[2] = (R[2] + R[6]) / s; }
    else if (R[4] > R[8]) { const float s = std::sqrt(1.f + R[4] - R[0] - R[8]) * 2.f; q[3] = (R[2] - R[6]) / s; q[0] = (R[1] + R[3]) / s; q[1] = 0.25f * s; q[2] = (R[5] + R[7]) / s; }
    else { const float s = std::sqrt(1.f + R[8] - R[0] - R[4]) * 2.f; q[3] = (R[3] - R[1]) / s; q[0] = (R[2] + R[6]) / s; q[1] = (R[5] + R[7]) / s; q[2] = 0.25f * s; }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 1e-12f) for (int i = 0; i < 4; ++i) q[i] /= n;
}
inline void Mul33(const float* A, const float* B, float* C) {
    float T[9];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) T[i * 3 + j] = A[i * 3] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j];
    memcpy(C, T, sizeof T);
}
inline void MulVec(const float* R, const float* v, float* o) {
    o[0] = R[0] * v[0] + R[1] * v[1] + R[2] * v[2];
    o[1] = R[3] * v[0] + R[4] * v[1] + R[5] * v[2];
    o[2] = R[6] * v[0] + R[7] * v[1] + R[8] * v[2];
}
inline void Nlerp(const float* a, const float* b, float t, float* o) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float s = d < 0.f ? -1.f : 1.f;
    float q[4]; float n = 0.f;
    for (int i = 0; i < 4; ++i) { q[i] = a[i] * (1.f - t) + s * b[i] * t; n += q[i] * q[i]; }
    n = n > 1e-12f ? 1.f / std::sqrt(n) : 1.f;
    for (int i = 0; i < 4; ++i) o[i] = q[i] * n;
}

// local pose of the animation skeleton
struct Pose {
    int nb = 0;
    float q[kMaxBones * 4];
    float p[kMaxBones * 3];
    uint8_t valid[kMaxBones];
};

// ---------------------------------------------------------------------------------------------
// loader
// ---------------------------------------------------------------------------------------------
struct Reader {
    const uint8_t* b; size_t n, p = 0; bool ok = true;
    Reader(const uint8_t* buf, size_t len) : b(buf), n(len) {}
    template <class T> T get() { T v{}; if (p + sizeof(T) > n) { ok = false; return v; } memcpy(&v, b + p, sizeof(T)); p += sizeof(T); return v; }
    std::string cstr() { size_t s = p; while (p < n && b[p]) ++p; if (p >= n) { ok = false; return std::string(); } std::string r(reinterpret_cast<const char*>(b + s), p - s); ++p; return r; }
    const uint8_t* skip(size_t k) { if (p + k > n) { ok = false; return nullptr; } const uint8_t* r = b + p; p += k; return r; }
};

inline std::string Lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a'); return s; }

inline bool LoadPack(const wchar_t* path, Pack& out, std::string* err = nullptr) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) { if (err) *err = "cannot open pack"; return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    out.buf.resize((size_t)sz);
    if (fread(out.buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); if (err) *err = "short read"; return false; }
    fclose(f);
    Reader r(out.buf.data(), out.buf.size());
    char magic[4]; for (int i = 0; i < 4; ++i) magic[i] = (char)r.get<uint8_t>();
    if (memcmp(magic, "ECP1", 4) != 0) { if (err) *err = "bad magic"; return false; }
    const uint32_t nBodies = r.get<uint32_t>(), nTex = r.get<uint32_t>(), nMantles = r.get<uint32_t>();
    for (uint32_t bi = 0; bi < nBodies && r.ok; ++bi) {
        Anim a; a.body = r.cstr();
        const uint32_t nb = r.get<uint32_t>();
        if (nb > (uint32_t)kMaxBones) { if (err) *err = "too many bones"; return false; }
        for (uint32_t i = 0; i < nb; ++i) { Bone b; b.name = r.cstr(); b.parent = r.get<int32_t>(); a.bones.push_back(b); }
        const uint32_t ns = r.get<uint32_t>();
        a.seqs.reserve(ns);
        for (uint32_t si = 0; si < ns && r.ok; ++si) {
            Seq s; s.name = r.cstr(); s.frames = (int)r.get<uint32_t>(); s.rate = r.get<float>();
            s.tr.resize(nb);
            for (uint32_t b = 0; b < nb && r.ok; ++b) {
                Track& t = s.tr[b];
                t.flags = r.get<uint8_t>();
                if (t.flags == 0xFF) continue;
                for (int k = 0; k < 4; ++k) t.sq[k] = r.get<float>();
                for (int k = 0; k < 3; ++k) t.sp[k] = r.get<float>();
                if (t.flags & 1) t.q = r.skip((size_t)s.frames * 8);
                if (t.flags & 2) t.p = r.skip((size_t)s.frames * 12);
            }
            a.byName[s.name] = (int)a.seqs.size();
            a.seqs.push_back(std::move(s));
        }
        out.anims.push_back(std::move(a));
    }
    for (uint32_t ti = 0; ti < nTex && r.ok; ++ti) {
        Tex t; t.name = r.cstr(); t.fmt = r.get<uint32_t>(); t.w = r.get<uint32_t>(); t.h = r.get<uint32_t>(); t.levels = (int)r.get<uint32_t>();
        if (t.levels > 12) { if (err) *err = "too many mip levels"; return false; }
        for (int l = 0; l < t.levels; ++l) { t.size[l] = r.get<uint32_t>(); t.data[l] = r.skip(t.size[l]); }
        out.texs.push_back(t);
    }
    for (uint32_t mi = 0; mi < nMantles && r.ok; ++mi) {
        Mantle m; m.body = r.cstr(); m.design = (int)r.get<uint32_t>(); m.meshName = r.cstr();
        const uint32_t nb = r.get<uint32_t>();
        for (uint32_t i = 0; i < nb; ++i) { Bone b; b.name = r.cstr(); b.parent = r.get<int32_t>(); for (int k = 0; k < 4; ++k) b.q[k] = r.get<float>(); for (int k = 0; k < 3; ++k) b.t[k] = r.get<float>(); m.bones.push_back(b); }
        m.np = (int)r.get<uint32_t>();
        m.pts.resize((size_t)m.np * 3); for (auto& v : m.pts) v = r.get<float>();
        m.infBone.resize((size_t)m.np * 4); m.infW.resize((size_t)m.np * 4);
        for (int i = 0; i < m.np; ++i) { for (int k = 0; k < 4; ++k) m.infBone[(size_t)i * 4 + k] = r.get<uint8_t>(); for (int k = 0; k < 4; ++k) m.infW[(size_t)i * 4 + k] = r.get<float>(); }
        m.nw = (int)r.get<uint32_t>();
        m.wpt.resize(m.nw); m.wuv.resize((size_t)m.nw * 2);
        for (int i = 0; i < m.nw; ++i) { m.wpt[i] = r.get<uint16_t>(); m.wuv[(size_t)i * 2] = r.get<float>(); m.wuv[(size_t)i * 2 + 1] = r.get<float>(); }
        m.nf = (int)r.get<uint32_t>();
        m.faces.resize((size_t)m.nf * 3); for (auto& v : m.faces) v = r.get<uint16_t>();
        const uint32_t nsec = r.get<uint32_t>();
        for (uint32_t i = 0; i < nsec; ++i) { Section s; s.first = (int)r.get<uint32_t>(); s.count = (int)r.get<uint32_t>(); s.mat = (int)r.get<uint32_t>(); m.secs.push_back(s); }
        const uint32_t nmat = r.get<uint32_t>();
        for (uint32_t i = 0; i < nmat; ++i) { Material t; t.tex = (int)r.get<uint32_t>(); t.alphaTest = (int)r.get<uint32_t>(); t.alphaRef = (int)r.get<uint32_t>(); t.twoSided = (int)r.get<uint32_t>(); m.mats.push_back(t); }
        if (!r.ok) break;
        // bind pose in mesh space
        const size_t n = m.bones.size();
        m.bindR.assign(n * 9, 0.f); m.bindT.assign(n * 3, 0.f);
        for (size_t i = 0; i < n; ++i) {
            float R[9]; QuatToMat(m.bones[i].q, R);
            const int p = m.bones[i].parent;
            if (i == 0 || p < 0 || p == (int)i) { memcpy(&m.bindR[i * 9], R, sizeof R); memcpy(&m.bindT[i * 3], m.bones[i].t, 12); }
            else {
                Mul33(&m.bindR[(size_t)p * 9], R, &m.bindR[i * 9]);
                float tt[3]; MulVec(&m.bindR[(size_t)p * 9], m.bones[i].t, tt);
                for (int k = 0; k < 3; ++k) m.bindT[i * 3 + k] = tt[k] + m.bindT[(size_t)p * 3 + k];
            }
        }
        out.mantles.push_back(std::move(m));
    }
    if (!r.ok) { if (err) *err = "truncated pack"; return false; }
    if (r.p + 8 <= r.n && memcmp(out.buf.data() + r.p, "FXS1", 4) == 0) {                     // effect programs (optional)
        r.p += 4;
        const uint32_t nProg = r.get<uint32_t>();
        for (uint32_t pi = 0; pi < nProg && r.ok; ++pi) {
            const std::string key = r.cstr(); const uint32_t nl = r.get<uint8_t>();
            std::vector<FxLayer> layers;
            for (uint32_t li = 0; li < nl && r.ok; ++li) {
                FxLayer l; l.kind = r.get<uint8_t>(); const uint32_t nt = r.get<uint8_t>();
                for (uint32_t ti = 0; ti < nt && r.ok; ++ti) { FxTex x; x.tex = (int)r.get<uint32_t>(); const uint8_t fl = r.get<uint8_t>(); x.env = (fl & 1) != 0; x.mask = (fl & 2) != 0; x.panU = r.get<float>(); x.panV = r.get<float>(); l.texs.push_back(x); }
                layers.push_back(std::move(l));
            }
            if (r.ok) out.fx[Lower(key)] = std::move(layers);
        }
        if (!r.ok) { if (err) *err = "truncated fx section"; return false; }
    }
    return true;
}

inline const Mantle* FindMantle(const Pack& p, const std::string& body, int design) {
    const std::string b = Lower(body);
    for (const auto& m : p.mantles) if (m.design == design && Lower(m.body) == b) return &m;
    return nullptr;
}
inline Anim* FindAnim(Pack& p, const std::string& body) {
    const std::string b = Lower(body);
    for (auto& a : p.anims) if (Lower(a.body) == b) return &a;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// pose evaluation / skinning
// ---------------------------------------------------------------------------------------------
inline void EvalSeq(const Anim& a, const Seq& s, float frame, Pose& out) {
    const int nb = (int)a.bones.size();
    out.nb = nb;
    if (frame < 0.f) frame = 0.f;
    const float maxf = (float)(s.frames > 1 ? s.frames - 1 : 0);
    if (frame > maxf) frame = maxf;
    const int f0 = (int)std::floor(frame);
    const int f1 = f0 + 1 <= (int)maxf ? f0 + 1 : f0;
    const float u = frame - (float)f0;
    for (int b = 0; b < nb; ++b) {
        const Track& t = s.tr[b];
        float* q = &out.q[b * 4]; float* p = &out.p[b * 3];
        if (t.flags == 0xFF) { out.valid[b] = 0; q[0] = q[1] = q[2] = 0.f; q[3] = 1.f; p[0] = p[1] = p[2] = 0.f; continue; }
        out.valid[b] = 1;
        if (t.flags & 1) {
            const int16_t* k = reinterpret_cast<const int16_t*>(t.q);
            float a0[4], a1[4];
            for (int i = 0; i < 4; ++i) {
                int16_t v0, v1; memcpy(&v0, &k[f0 * 4 + i], 2); memcpy(&v1, &k[f1 * 4 + i], 2);
                a0[i] = v0 / 32767.f; a1[i] = v1 / 32767.f;
            }
            Nlerp(a0, a1, u, q);
        } else memcpy(q, t.sq, 16);
        if (t.flags & 2) {
            float p0[3], p1[3];
            memcpy(p0, t.p + (size_t)f0 * 12, 12); memcpy(p1, t.p + (size_t)f1 * 12, 12);
            for (int i = 0; i < 3; ++i) p[i] = p0[i] * (1.f - u) + p1[i] * u;
        } else memcpy(p, t.sp, 12);
    }
}

inline void BlendPose(const Pose& a, const Pose& b, float t, Pose& o) {
    o.nb = b.nb;
    for (int i = 0; i < b.nb; ++i) {
        o.valid[i] = b.valid[i];
        if (a.valid[i] && b.valid[i] && i < a.nb) {
            Nlerp(&a.q[i * 4], &b.q[i * 4], t, &o.q[i * 4]);
            for (int k = 0; k < 3; ++k) o.p[i * 3 + k] = a.p[i * 3 + k] * (1.f - t) + b.p[i * 3 + k] * t;
        } else { memcpy(&o.q[i * 4], &b.q[i * 4], 16); memcpy(&o.p[i * 3], &b.p[i * 3], 12); }
    }
}

// ---- inertialization: a pose change is stored as an offset (current pose relative to the new target) that decays while the new animation keeps playing
inline void QMul(const float* a, const float* b, float* o) {      // Hamilton product a * b (rotation b first, then a)
    float r[4];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    memcpy(o, r, sizeof r);
}
struct PoseOffset { int nb = 0; float q[kMaxBones * 4]; float p[kMaxBones * 3]; };

inline void MakeOffset(const Pose& cur, const Pose& tgt, PoseOffset& o) {          // cur = offset * tgt  (per bone, local space)
    o.nb = tgt.nb;
    for (int i = 0; i < tgt.nb; ++i) {
        float tc[4] = { -tgt.q[i * 4], -tgt.q[i * 4 + 1], -tgt.q[i * 4 + 2], tgt.q[i * 4 + 3] };
        if (i < cur.nb && cur.valid[i] && tgt.valid[i]) {
            QMul(&cur.q[i * 4], tc, &o.q[i * 4]);
            if (o.q[i * 4 + 3] < 0.f) for (int k = 0; k < 4; ++k) o.q[i * 4 + k] = -o.q[i * 4 + k];
            for (int k = 0; k < 3; ++k) o.p[i * 3 + k] = cur.p[i * 3 + k] - tgt.p[i * 3 + k];
        } else { o.q[i * 4] = o.q[i * 4 + 1] = o.q[i * 4 + 2] = 0.f; o.q[i * 4 + 3] = 1.f; o.p[i * 3] = o.p[i * 3 + 1] = o.p[i * 3 + 2] = 0.f; }
    }
}
inline void ApplyOffset(const Pose& tgt, const PoseOffset& o, float w, Pose& out) {     // out = (offset^w) * tgt
    out.nb = tgt.nb;
    const float id[4] = { 0.f, 0.f, 0.f, 1.f };
    for (int i = 0; i < tgt.nb; ++i) {
        out.valid[i] = tgt.valid[i];
        float ow[4]; Nlerp(id, &o.q[i * 4], w, ow);
        QMul(ow, &tgt.q[i * 4], &out.q[i * 4]);
        for (int k = 0; k < 3; ++k) out.p[i * 3 + k] = tgt.p[i * 3 + k] + o.p[i * 3 + k] * w;
    }
}

// mesh bone -> animation bone index (by name, -1 when the animation has no such bone)
inline void MapBones(const Anim& a, const Mantle& m, std::vector<int>& amap) {
    amap.assign(m.bones.size(), -1);
    for (size_t j = 0; j < m.bones.size(); ++j) {
        const std::string n = Lower(m.bones[j].name);
        for (size_t i = 0; i < a.bones.size(); ++i) if (Lower(a.bones[i].name) == n) { amap[j] = (int)i; break; }
    }
}

// skinned point positions (np * 3) for a local pose of the animation skeleton
inline void SkinPoints(const Anim& a, const Mantle& m, const std::vector<int>& amap, const Pose& pose, std::vector<float>& out) {
    const int nb = pose.nb;
    float GR[kMaxBones * 9], GT[kMaxBones * 3];
    for (int i = 0; i < nb; ++i) {
        float R[9]; QuatToMat(&pose.q[i * 4], R);
        const int p = a.bones[i].parent;
        if (i == 0 || p < 0 || p == i) { memcpy(&GR[i * 9], R, sizeof R); memcpy(&GT[i * 3], &pose.p[i * 3], 12); }
        else {
            Mul33(&GR[p * 9], R, &GR[i * 9]);
            float tt[3]; MulVec(&GR[p * 9], &pose.p[i * 3], tt);
            for (int k = 0; k < 3; ++k) GT[i * 3 + k] = tt[k] + GT[p * 3 + k];
        }
    }
    const size_t nm = m.bones.size();
    std::vector<float> SR(nm * 9), ST(nm * 3);
    for (size_t j = 0; j < nm; ++j) {
        const int ai = amap[j];
        const float* Rb = &m.bindR[j * 9]; const float* tb = &m.bindT[j * 3];
        if (ai < 0) { float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; memcpy(&SR[j * 9], I, sizeof I); ST[j * 3] = ST[j * 3 + 1] = ST[j * 3 + 2] = 0.f; continue; }
        float RbT[9] = { Rb[0], Rb[3], Rb[6], Rb[1], Rb[4], Rb[7], Rb[2], Rb[5], Rb[8] };
        float Rs[9]; Mul33(&GR[ai * 9], RbT, Rs);
        float rt[3]; MulVec(Rs, tb, rt);
        memcpy(&SR[j * 9], Rs, sizeof Rs);
        for (int k = 0; k < 3; ++k) ST[j * 3 + k] = GT[ai * 3 + k] - rt[k];
    }
    out.assign((size_t)m.np * 3, 0.f);
    for (int pt = 0; pt < m.np; ++pt) {
        const float* v = &m.pts[(size_t)pt * 3];
        float acc[3] = { 0, 0, 0 };
        for (int k = 0; k < 4; ++k) {
            const float w = m.infW[(size_t)pt * 4 + k];
            if (w <= 0.f) continue;
            const int j = m.infBone[(size_t)pt * 4 + k];
            if ((size_t)j >= nm) continue;
            float o[3]; MulVec(&SR[(size_t)j * 9], v, o);
            for (int c = 0; c < 3; ++c) acc[c] += w * (o[c] + ST[(size_t)j * 3 + c]);
        }
        out[(size_t)pt * 3] = acc[0]; out[(size_t)pt * 3 + 1] = acc[1]; out[(size_t)pt * 3 + 2] = acc[2];
    }
}


// skinned positions of a subset of the mantle points (idx = point indices), out = idx.size() * 3
inline void SkinSubset(const Anim& a, const Mantle& m, const std::vector<int>& amap, const Pose& pose, const std::vector<int>& idx, std::vector<float>& out) {
    const int nb = pose.nb;
    float GR[kMaxBones * 9], GT[kMaxBones * 3];
    for (int i = 0; i < nb; ++i) {
        float R[9]; QuatToMat(&pose.q[i * 4], R);
        const int p = a.bones[i].parent;
        if (i == 0 || p < 0 || p == i) { memcpy(&GR[i * 9], R, sizeof R); memcpy(&GT[i * 3], &pose.p[i * 3], 12); }
        else {
            Mul33(&GR[p * 9], R, &GR[i * 9]);
            float tt[3]; MulVec(&GR[p * 9], &pose.p[i * 3], tt);
            for (int k = 0; k < 3; ++k) GT[i * 3 + k] = tt[k] + GT[p * 3 + k];
        }
    }
    const size_t nm = m.bones.size();
    std::vector<float> SR(nm * 9), ST(nm * 3);
    for (size_t j = 0; j < nm; ++j) {
        const int ai = amap[j];
        const float* Rb = &m.bindR[j * 9]; const float* tb = &m.bindT[j * 3];
        if (ai < 0) { float I[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; memcpy(&SR[j * 9], I, sizeof I); ST[j * 3] = ST[j * 3 + 1] = ST[j * 3 + 2] = 0.f; continue; }
        float RbT[9] = { Rb[0], Rb[3], Rb[6], Rb[1], Rb[4], Rb[7], Rb[2], Rb[5], Rb[8] };
        float Rs[9]; Mul33(&GR[ai * 9], RbT, Rs);
        float rt[3]; MulVec(Rs, tb, rt);
        memcpy(&SR[j * 9], Rs, sizeof Rs);
        for (int k = 0; k < 3; ++k) ST[j * 3 + k] = GT[ai * 3 + k] - rt[k];
    }
    out.assign(idx.size() * 3, 0.f);
    for (size_t n = 0; n < idx.size(); ++n) {
        const int pt = idx[n];
        const float* v = &m.pts[(size_t)pt * 3];
        float acc[3] = { 0, 0, 0 };
        for (int k = 0; k < 4; ++k) {
            const float w = m.infW[(size_t)pt * 4 + k];
            if (w <= 0.f) continue;
            const int j = m.infBone[(size_t)pt * 4 + k];
            if ((size_t)j >= nm) continue;
            float o[3]; MulVec(&SR[(size_t)j * 9], v, o);
            for (int c = 0; c < 3; ++c) acc[c] += w * (o[c] + ST[(size_t)j * 3 + c]);
        }
        out[n * 3] = acc[0]; out[n * 3 + 1] = acc[1]; out[n * 3 + 2] = acc[2];
    }
}

// local coordinates of a skinned position in the (mesh-aligned) frame of the root bone: Rb * Ra^T * (p - ta); equals (rest - tb) for points rigid on the root
inline void RootLocal(const Mantle& m, int jr, const Pose& pose, const float* p, float* out) {
    float Ra[9]; QuatToMat(&pose.q[0], Ra);
    const float d[3] = { p[0] - pose.p[0], p[1] - pose.p[1], p[2] - pose.p[2] };
    float RaTd[3] = { Ra[0] * d[0] + Ra[3] * d[1] + Ra[6] * d[2], Ra[1] * d[0] + Ra[4] * d[1] + Ra[7] * d[2], Ra[2] * d[0] + Ra[5] * d[1] + Ra[8] * d[2] };
    MulVec(&m.bindR[(size_t)jr * 9], RaTd, out);
}

// per-point smooth normals from the skinned positions
inline void PointNormals(const Mantle& m, const std::vector<float>& pos, std::vector<float>& nrm) {
    nrm.assign((size_t)m.np * 3, 0.f);
    for (int f = 0; f < m.nf; ++f) {
        const int pa = m.wpt[m.faces[(size_t)f * 3]], pb = m.wpt[m.faces[(size_t)f * 3 + 1]], pc = m.wpt[m.faces[(size_t)f * 3 + 2]];
        const float* A = &pos[(size_t)pa * 3]; const float* B = &pos[(size_t)pb * 3]; const float* C = &pos[(size_t)pc * 3];
        const float u[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, v[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
        const float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        for (int k = 0; k < 3; ++k) { nrm[(size_t)pa * 3 + k] += n[k]; nrm[(size_t)pb * 3 + k] += n[k]; nrm[(size_t)pc * 3 + k] += n[k]; }
    }
    for (int i = 0; i < m.np; ++i) {
        float* n = &nrm[(size_t)i * 3];
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-12f) { n[0] /= l; n[1] /= l; n[2] /= l; } else { n[0] = 0.f; n[1] = 0.f; n[2] = 1.f; }
    }
}

// vertex buffer data: nw x (pos[3], normal[3], uv[2]) = 32 bytes per wedge
inline void FillVertices(const Mantle& m, const std::vector<float>& pos, const std::vector<float>* nrm, float scale, const float* off, float* out) {
    for (int i = 0; i < m.nw; ++i) {
        const int pt = m.wpt[i];
        float* o = out + (size_t)i * 8;
        for (int k = 0; k < 3; ++k) o[k] = pos[(size_t)pt * 3 + k] * scale + off[k];
        if (nrm) { o[3] = (*nrm)[(size_t)pt * 3]; o[4] = (*nrm)[(size_t)pt * 3 + 1]; o[5] = (*nrm)[(size_t)pt * 3 + 2]; } else { o[3] = 0.f; o[4] = 0.f; o[5] = 1.f; }
        o[6] = m.wuv[(size_t)i * 2]; o[7] = m.wuv[(size_t)i * 2 + 1];
    }
}

}  // namespace ess
