// clothpack.h - loader of essence_cloth.bin (tools/build_cloth.py): cloth sets of the standard Essence cloaks + the cloth textures of the official items.
#pragma once
#include "essence.h"
#include "cloth.h"

namespace ess {

struct ClothLook { std::string name; char kind = 'H'; int tex = 0; int itemId = 0; int collarTex = -1, crestTex = -1; std::string family; };
struct Collar {                                                    // rigid collar / shoulder piece (<Body>_<family> skeletal mesh), pinned to Spine2 like the cloth anchors
    std::string body, family; int nv = 0, nt = 0;
    std::vector<float> pos, uv, nrm;                               // nv * 3, nv * 2, nv * 3 (smooth normals of the rest pose)
    std::vector<uint16_t> idx;                                     // nt * 3
};
struct ClothPack {
    std::vector<uint8_t> buf;
    std::vector<Tex> texs;
    std::vector<cloth::Set> sets;
    std::vector<Collar> collars;
    std::vector<ClothLook> looks;
    std::unordered_map<std::string, std::vector<FxLayer>> fx;       // lowercase base texture name -> effect layers (optional FXS1 section)
};

inline bool LoadClothPack(const wchar_t* path, ClothPack& out, std::string* err = nullptr) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) { if (err) *err = "cannot open cloth pack"; return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    out.buf.resize((size_t)sz);
    if (fread(out.buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); if (err) *err = "short read"; return false; }
    fclose(f);
    Reader r(out.buf.data(), out.buf.size());
    char magic[4]; for (int i = 0; i < 4; ++i) magic[i] = (char)r.get<uint8_t>();
    if (memcmp(magic, "ECL2", 4) != 0) { if (err) *err = "bad cloth magic"; return false; }
    const uint32_t nTex = r.get<uint32_t>(), nSets = r.get<uint32_t>(), nCollars = r.get<uint32_t>(), nLooks = r.get<uint32_t>();
    for (uint32_t ti = 0; ti < nTex && r.ok; ++ti) {
        Tex t; t.name = r.cstr(); t.fmt = r.get<uint32_t>(); t.w = r.get<uint32_t>(); t.h = r.get<uint32_t>(); t.levels = (int)r.get<uint32_t>();
        if (t.levels > 12) { if (err) *err = "too many mip levels"; return false; }
        for (int l = 0; l < t.levels; ++l) { t.size[l] = r.get<uint32_t>(); t.data[l] = r.skip(t.size[l]); }
        out.texs.push_back(t);
    }
    for (uint32_t si = 0; si < nSets && r.ok; ++si) {
        cloth::Set s; s.body = r.cstr(); s.kind = r.cstr();
        s.np = (int)r.get<uint32_t>(); s.width = (int)r.get<uint32_t>(); s.nt = (int)r.get<uint32_t>();
        const uint32_t na = r.get<uint32_t>(), nsp = r.get<uint32_t>(), nc = r.get<uint32_t>();
        if (s.np <= 0 || s.np > 256) { if (err) *err = "bad particle count"; return false; }
        s.rest.resize((size_t)s.np * 3); for (auto& v : s.rest) v = r.get<float>();
        s.uv.resize((size_t)s.np * 2); for (auto& v : s.uv) v = r.get<float>();
        s.sens.resize((size_t)s.np); for (auto& v : s.sens) v = r.get<float>();
        s.tris.resize((size_t)s.nt * 3); for (auto& v : s.tris) v = r.get<uint16_t>();
        s.anchors.resize(na); for (auto& v : s.anchors) v = r.get<uint16_t>();
        s.springs.resize(nsp); for (auto& sp : s.springs) { sp.i = r.get<uint16_t>(); sp.j = r.get<uint16_t>(); sp.rest = r.get<float>(); }
        s.caps.resize(nc); for (auto& c : s.caps) { c.a = r.cstr(); c.b = r.cstr(); c.r = r.get<float>(); }
        s.sec1 = r.get<int32_t>();
        s.Finish();
        out.sets.push_back(std::move(s));
    }
    for (uint32_t ci = 0; ci < nCollars && r.ok; ++ci) {
        Collar c; c.body = r.cstr(); c.family = r.cstr(); c.nv = (int)r.get<uint32_t>(); c.nt = (int)r.get<uint32_t>();
        if (c.nv <= 0 || c.nv > 4096 || c.nt <= 0 || c.nt > 8192) { if (err) *err = "bad collar size"; return false; }
        c.pos.resize((size_t)c.nv * 3); c.uv.resize((size_t)c.nv * 2);
        for (int i = 0; i < c.nv; ++i) { for (int k = 0; k < 3; ++k) c.pos[(size_t)i * 3 + k] = r.get<float>(); for (int k = 0; k < 2; ++k) c.uv[(size_t)i * 2 + k] = r.get<float>(); }
        c.idx.resize((size_t)c.nt * 3); for (auto& v : c.idx) { v = r.get<uint16_t>(); if (v >= c.nv) { if (err) *err = "bad collar index"; return false; } }
        c.nrm.assign((size_t)c.nv * 3, 0.f);                               // smooth normals (the lit path rotates them with the torso)
        for (int t = 0; t < c.nt; ++t) {
            const int a = c.idx[(size_t)t * 3], b = c.idx[(size_t)t * 3 + 1], d = c.idx[(size_t)t * 3 + 2];
            const float* A = &c.pos[(size_t)a * 3]; const float* B = &c.pos[(size_t)b * 3]; const float* C = &c.pos[(size_t)d * 3];
            const float u[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] }, v[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
            const float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
            for (int k = 0; k < 3; ++k) { c.nrm[(size_t)a * 3 + k] += n[k]; c.nrm[(size_t)b * 3 + k] += n[k]; c.nrm[(size_t)d * 3 + k] += n[k]; }
        }
        for (int i = 0; i < c.nv; ++i) { float* n = &c.nrm[(size_t)i * 3]; const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); if (l > 1e-9f) { n[0] /= l; n[1] /= l; n[2] /= l; } else { n[0] = 0.f; n[1] = -1.f; n[2] = 0.f; } }
        out.collars.push_back(std::move(c));
    }
    for (uint32_t li = 0; li < nLooks && r.ok; ++li) {
        ClothLook l; l.name = r.cstr(); l.kind = (char)r.get<uint8_t>(); l.tex = (int)r.get<uint32_t>(); l.itemId = (int)r.get<uint32_t>();
        const uint32_t ct = r.get<uint32_t>(), cr = r.get<uint32_t>(); l.family = r.cstr();
        l.collarTex = ct == 0xFFFFFFFFu ? -1 : (int)ct; l.crestTex = cr == 0xFFFFFFFFu ? -1 : (int)cr;
        out.looks.push_back(std::move(l));
    }
    if (!r.ok) { if (err) *err = "truncated cloth pack"; return false; }
    if (r.p + 8 <= r.n && memcmp(out.buf.data() + r.p, "FXS1", 4) == 0) {
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

inline const cloth::Set* FindClothSet(const ClothPack& p, const std::string& body, char kind) {
    const std::string b = Lower(body);
    for (const auto& s : p.sets) if (Lower(s.body) == b && s.kind[0] == kind) return &s;
    return nullptr;
}

inline const Collar* FindCollar(const ClothPack& p, const std::string& body, const std::string& family) {
    const std::string b = Lower(body), f = Lower(family);
    for (const auto& c : p.collars) if (Lower(c.body) == b && Lower(c.family) == f) return &c;
    if (f != "hrm_ad11") for (const auto& c : p.collars) if (Lower(c.body) == b && Lower(c.family) == "hrm_ad11") return &c;      // the body has no such variant: the generic collar
    return nullptr;
}

}  // namespace ess
