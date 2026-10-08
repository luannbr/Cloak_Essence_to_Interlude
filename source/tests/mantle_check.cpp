// mantle_check.cpp - validates every packed mantle the way the hook uses it (indices, sections, materials, skinning, normals, vertex fill)
//   mantle_check <pack>
#include "../src/essence.h"
#include <cstdlib>
#include <cmath>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    wchar_t w[512]; mbstowcs(w, argv[1], 512);
    ess::Pack pk; std::string err;
    if (!ess::LoadPack(w, pk, &err)) { printf("load failed: %s\n", err.c_str()); return 1; }
    int bad = 0, n = 0;
    for (const ess::Mantle& m : pk.mantles) {
        ++n; std::string why;
        ess::Anim* a = ess::FindAnim(pk, m.body);
        if (!a) why += "no anim; ";
        if (m.np <= 0 || m.nw <= 0 || m.nf <= 0) why += "empty; ";
        for (int i = 0; i < m.nw && why.empty(); ++i) if (m.wpt[i] < 0 || m.wpt[i] >= m.np) { why += "wedge point out of range; "; break; }
        for (size_t i = 0; i < m.faces.size() && why.empty(); ++i) if (m.faces[i] >= m.nw) { why += "face index >= nw; "; break; }
        for (const auto& s : m.secs) if (s.first < 0 || s.count < 0 || s.first + s.count > m.nf) why += "section out of range; ";
        for (const auto& mt : m.mats) if (mt.tex < 0 || mt.tex >= (int)pk.texs.size()) why += "material texture out of range; ";
        if (m.mats.size() > 8) why += "more than 8 materials; ";
        if (a && why.empty()) {
            std::vector<int> amap; ess::MapBones(*a, m, amap);
            for (size_t si = 0; si < a->seqs.size() && si < 6; ++si) {
                ess::Pose pose; ess::EvalSeq(*a, a->seqs[si], 0.f, pose);
                std::vector<float> pos, nr; ess::SkinPoints(*a, m, amap, pose, pos);
                if ((int)pos.size() < m.np * 3) { why += "skin output too small; "; break; }
                ess::PointNormals(m, pos, nr);
                std::vector<float> vtx((size_t)m.nw * 8); const float off[3] = { 0, 0, 0 };
                ess::FillVertices(m, pos, &nr, 1.f, off, vtx.data());
                for (float v : vtx) if (!std::isfinite(v)) { why += "non-finite vertex; "; break; }
                if (!why.empty()) break;
            }
        }
        if (!why.empty()) { ++bad; printf("BAD %-34s key %-18s design %2d: %s\n", m.meshName.c_str(), m.body.c_str(), m.design, why.c_str()); }
    }
    int nfx = 0;
    for (const auto& kv : pk.fx) {
        ++nfx;
        for (const ess::FxLayer& l : kv.second) {
            if (l.texs.empty() || l.texs.size() > 3) { printf("FX %s: bad texture count %zu\n", kv.first.c_str(), l.texs.size()); ++bad; }
            for (const ess::FxTex& x : l.texs) if (x.tex < 0 || x.tex >= (int)pk.texs.size()) { printf("FX %s: texture index %d out of range\n", kv.first.c_str(), x.tex); ++bad; }
        }
    }
    printf("%d effect programs\n", nfx);
    for (size_t i = 0; i < pk.texs.size(); ++i) {
        const ess::Tex& x = pk.texs[i]; std::string why;
        if (x.fmt != 3 && x.fmt != 7 && x.fmt != 8) why += "format not DXT; ";
        for (int l = 0; l < x.levels; ++l) {
            const unsigned lw = x.w >> l ? x.w >> l : 1, lh = x.h >> l ? x.h >> l : 1;
            const unsigned need = ((lw + 3) / 4) * (x.fmt == 3 ? 8 : 16) * ((lh + 3) / 4);
            if (x.size[l] < need) { why += "level " + std::to_string(l) + " too small; "; break; }
        }
        if (!why.empty() || x.w > 1024) printf("TEX %2zu %-34s fmt %u %ux%u levels %d: %s\n", i, x.name.c_str(), x.fmt, x.w, x.h, x.levels, why.c_str());
    }
    printf("%d mantles checked, %d bad\n", n, bad);
    return bad ? 1 : 0;
}
