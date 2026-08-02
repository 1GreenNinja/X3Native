// Minimal CPU-side GLB primitive reader. See glb_cpu_read.h for the scope and
// why it deliberately does not live in engine/asset.
#include "glb_cpu_read.h"

#include "json_mini.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace x3::game {
namespace {

using jmini::JVal;

const JVal* idx(const JVal& arr, int i) {
    if (arr.t != JVal::Arr || i < 0 || i >= (int)arr.arr.size()) return nullptr;
    return &arr.arr[(size_t)i];
}

struct Mat4 {
    float m[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };   // column-major
};

Mat4 mul(const Mat4& a, const Mat4& b) {          // a * b
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

// glTF node TRS (or an explicit `matrix`) -> column-major 4x4.
Mat4 nodeLocal(const JVal& node) {
    Mat4 out;
    if (const JVal* m = node.get("matrix")) {
        if (m->t == JVal::Arr && m->arr.size() == 16)
            for (int i = 0; i < 16; ++i) out.m[i] = (float)m->arr[(size_t)i].num;
        return out;
    }
    float t[3] = { 0, 0, 0 }, r[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 };
    if (const JVal* v = node.get("translation"))
        for (int i = 0; i < 3 && i < (int)v->arr.size(); ++i) t[i] = (float)v->arr[(size_t)i].num;
    if (const JVal* v = node.get("rotation"))
        for (int i = 0; i < 4 && i < (int)v->arr.size(); ++i) r[i] = (float)v->arr[(size_t)i].num;
    if (const JVal* v = node.get("scale"))
        for (int i = 0; i < 3 && i < (int)v->arr.size(); ++i) s[i] = (float)v->arr[(size_t)i].num;
    const float x = r[0], y = r[1], z = r[2], w = r[3];
    const float rm[9] = {
        1 - 2 * (y * y + z * z),     2 * (x * y + z * w),     2 * (x * z - y * w),
            2 * (x * y - z * w), 1 - 2 * (x * x + z * z),     2 * (y * z + x * w),
            2 * (x * z + y * w),     2 * (y * z - x * w), 1 - 2 * (x * x + y * y)
    };
    for (int c = 0; c < 3; ++c)
        for (int row = 0; row < 3; ++row) out.m[c * 4 + row] = rm[c * 3 + row] * s[c];
    out.m[12] = t[0]; out.m[13] = t[1]; out.m[14] = t[2];
    return out;
}

// One accessor read out as floats (VEC3/VEC2/SCALAR) or as uint32 indices.
struct Reader {
    const JVal* json = nullptr;
    const uint8_t* bin = nullptr;
    size_t binSize = 0;
    std::string err;

    // Returns the base pointer + stride for accessor `a`, or nullptr on any
    // unsupported construct (sparse accessors, external buffers, overrun).
    const uint8_t* view(int accessorIdx, size_t& stride, int& compType, int& numComp, size_t& count) {
        const JVal* accs = json->get("accessors");
        const JVal* a = accs ? idx(*accs, accessorIdx) : nullptr;
        if (!a) { err = "missing accessor"; return nullptr; }
        if (a->get("sparse")) { err = "sparse accessors unsupported"; return nullptr; }
        compType = a->inum("componentType", 0);
        const std::string type = a->sval("type");
        numComp = (type == "VEC4") ? 4 : (type == "VEC3") ? 3 : (type == "VEC2") ? 2 :
                  (type == "SCALAR") ? 1 : 0;
        if (numComp == 0) { err = "accessor type " + type + " unsupported"; return nullptr; }
        count = (size_t)a->inum("count", 0);
        const int bvIdx = a->inum("bufferView", -1);
        if (bvIdx < 0) { err = "accessor without bufferView"; return nullptr; }
        const JVal* bvs = json->get("bufferViews");
        const JVal* bv = bvs ? idx(*bvs, bvIdx) : nullptr;
        if (!bv) { err = "missing bufferView"; return nullptr; }
        if (bv->inum("buffer", 0) != 0) { err = "external buffer unsupported"; return nullptr; }
        const size_t compSize = (compType == 5126 || compType == 5125) ? 4u
                              : (compType == 5123 || compType == 5122) ? 2u
                              : (compType == 5121 || compType == 5120) ? 1u : 0u;
        if (compSize == 0) { err = "componentType unsupported"; return nullptr; }
        const size_t tight = compSize * (size_t)numComp;
        stride = (size_t)bv->inum("byteStride", 0);
        if (stride == 0) stride = tight;
        const size_t base = (size_t)bv->inum("byteOffset", 0) + (size_t)a->inum("byteOffset", 0);
        if (count > 0 && base + stride * (count - 1) + tight > binSize) {
            err = "accessor overruns BIN chunk"; return nullptr;
        }
        return bin + base;
    }

    bool readFloats(int accessorIdx, int wantComp, std::vector<float>& out) {
        size_t stride = 0, count = 0; int ct = 0, nc = 0;
        const uint8_t* p = view(accessorIdx, stride, ct, nc, count);
        if (!p) return false;
        out.assign(count * (size_t)wantComp, 0.0f);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* e = p + stride * i;
            for (int c = 0; c < wantComp; ++c) {
                float v = 0.0f;
                if (c < nc) {
                    if (ct == 5126)      { std::memcpy(&v, e + 4u * (size_t)c, 4); }
                    else if (ct == 5123) { uint16_t u; std::memcpy(&u, e + 2u * (size_t)c, 2); v = (float)u / 65535.0f; }
                    else if (ct == 5121) { v = (float)e[c] / 255.0f; }
                    else { err = "float accessor componentType unsupported"; return false; }
                }
                out[i * (size_t)wantComp + (size_t)c] = v;
            }
        }
        return true;
    }

    bool readIndices(int accessorIdx, std::vector<uint32_t>& out) {
        size_t stride = 0, count = 0; int ct = 0, nc = 0;
        const uint8_t* p = view(accessorIdx, stride, ct, nc, count);
        if (!p || nc != 1) { if (err.empty()) err = "index accessor is not SCALAR"; return false; }
        out.assign(count, 0u);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* e = p + stride * i;
            if (ct == 5125)      { uint32_t u; std::memcpy(&u, e, 4); out[i] = u; }
            else if (ct == 5123) { uint16_t u; std::memcpy(&u, e, 2); out[i] = u; }
            else if (ct == 5121) { out[i] = e[0]; }
            else { err = "index componentType unsupported"; return false; }
        }
        return true;
    }
};

} // namespace

GlbModel readGlbForLod(const std::string& path, uint32_t minTriangles) {
    GlbModel out;

    std::ifstream f(path, std::ios::binary);
    if (!f) { out.error = "cannot open " + path; return out; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 20 || std::memcmp(data.data(), "glTF", 4) != 0) {
        out.error = "not a GLB container"; return out;
    }

    std::string jsonText;
    const uint8_t* bin = nullptr; size_t binSize = 0;
    size_t off = 12;
    while (off + 8 <= data.size()) {
        uint32_t len = 0, type = 0;
        std::memcpy(&len, data.data() + off, 4);
        std::memcpy(&type, data.data() + off + 4, 4);
        if (off + 8 + (size_t)len > data.size()) break;
        if (type == 0x4E4F534Au)      jsonText.assign((const char*)data.data() + off + 8, len);
        else if (type == 0x004E4942u) { bin = data.data() + off + 8; binSize = len; }
        off += 8 + (size_t)len;
    }
    if (jsonText.empty()) { out.error = "no JSON chunk"; return out; }

    jmini::JReader jr(jsonText);
    const JVal root = jr.parse();
    if (!jr.ok || root.t != JVal::Obj) { out.error = "JSON parse failed"; return out; }

    if (const JVal* req = root.get("extensionsRequired")) {
        for (const JVal& e : req->arr) {
            if (e.str.rfind("KHR_draco", 0) == 0 || e.str.rfind("EXT_meshopt", 0) == 0) {
                out.error = "compressed geometry (" + e.str + ") is out of scope"; return out;
            }
        }
    }

    Reader rd; rd.json = &root; rd.bin = bin; rd.binSize = binSize;

    // ---- images (kept encoded; the caller decodes with stb_image) ----
    if (const JVal* imgs = root.get("images")) {
        for (const JVal& im : imgs->arr) {
            GlbImage gi;
            gi.mime = im.sval("mimeType");
            const int bvIdx = im.inum("bufferView", -1);
            const JVal* bvs = root.get("bufferViews");
            const JVal* bv = (bvIdx >= 0 && bvs) ? idx(*bvs, bvIdx) : nullptr;
            if (bv) {
                const size_t o = (size_t)bv->inum("byteOffset", 0);
                const size_t n = (size_t)bv->inum("byteLength", 0);
                if (bin && o + n <= binSize) gi.bytes.assign(bin + o, bin + o + n);
            }
            out.images.push_back(std::move(gi));
        }
    }

    // material index -> base-colour image index
    std::vector<int> matImage;
    if (const JVal* mats = root.get("materials")) {
        for (const JVal& mt : mats->arr) {
            int img = -1;
            if (const JVal* pbr = mt.get("pbrMetallicRoughness"))
                if (const JVal* bct = pbr->get("baseColorTexture")) {
                    const int texIdx = bct->inum("index", -1);
                    const JVal* texs = root.get("textures");
                    const JVal* tx = (texIdx >= 0 && texs) ? idx(*texs, texIdx) : nullptr;
                    if (tx) img = tx->inum("source", -1);
                }
            matImage.push_back(img);
        }
    }

    // ---- node hierarchy -> world transform per mesh instance ----
    const JVal* nodes = root.get("nodes");
    const JVal* meshes = root.get("meshes");
    if (!meshes) { out.error = "no meshes"; return out; }

    struct Inst { int mesh; Mat4 xform; };
    std::vector<Inst> insts;
    if (nodes) {
        // Iterative DFS from the scene roots (or from every node if no scene).
        std::vector<std::pair<int, Mat4>> stack;
        const JVal* scenes = root.get("scenes");
        const JVal* scene = scenes ? idx(*scenes, root.inum("scene", 0)) : nullptr;
        if (scene) {
            if (const JVal* rn = scene->get("nodes"))
                for (const JVal& n : rn->arr) stack.emplace_back((int)n.num, Mat4{});
        }
        if (stack.empty())
            for (int i = 0; i < (int)nodes->arr.size(); ++i) stack.emplace_back(i, Mat4{});

        int guard = 0;
        while (!stack.empty() && guard++ < 100000) {
            const auto [ni, parent] = stack.back();
            stack.pop_back();
            const JVal* n = idx(*nodes, ni);
            if (!n) continue;
            const Mat4 world = mul(parent, nodeLocal(*n));
            const int mi = n->inum("mesh", -1);
            if (mi >= 0) insts.push_back({ mi, world });
            if (const JVal* ch = n->get("children"))
                for (const JVal& c : ch->arr) stack.emplace_back((int)c.num, world);
        }
    } else {
        for (int i = 0; i < (int)meshes->arr.size(); ++i) insts.push_back({ i, Mat4{} });
    }

    // ---- primitives ----
    for (const Inst& in : insts) {
        const JVal* mesh = idx(*meshes, in.mesh);
        if (!mesh) continue;
        const std::string meshName = mesh->sval("name");
        const JVal* prims = mesh->get("primitives");
        if (!prims) continue;
        for (const JVal& pr : prims->arr) {
            if (pr.inum("mode", 4) != 4) continue;            // triangles only
            const JVal* attrs = pr.get("attributes");
            if (!attrs) continue;
            const int posA = attrs->inum("POSITION", -1);
            const int nrmA = attrs->inum("NORMAL", -1);
            const int uvA  = attrs->inum("TEXCOORD_0", -1);
            const int idxA = pr.inum("indices", -1);
            if (posA < 0 || idxA < 0) continue;

            std::vector<float> pos, nrm, uv;
            std::vector<uint32_t> ind;
            if (!rd.readFloats(posA, 3, pos)) { out.error = rd.err; return out; }
            if (!rd.readIndices(idxA, ind))   { out.error = rd.err; return out; }
            if (nrmA >= 0 && !rd.readFloats(nrmA, 3, nrm)) { out.error = rd.err; return out; }
            if (uvA  >= 0 && !rd.readFloats(uvA,  2, uv))  { out.error = rd.err; return out; }
            if (ind.size() / 3 < minTriangles) continue;

            GlbPrimitive gp;
            gp.name = meshName;
            const int matIdx = pr.inum("material", -1);
            gp.baseColorImage = (matIdx >= 0 && matIdx < (int)matImage.size()) ? matImage[(size_t)matIdx] : -1;

            const size_t vcount = pos.size() / 3;
            gp.verts.resize(vcount);
            const float* M = in.xform.m;
            for (size_t v = 0; v < vcount; ++v) {
                const float x = pos[v * 3 + 0], y = pos[v * 3 + 1], z = pos[v * 3 + 2];
                x3::rhi::MeshVertex& mv = gp.verts[v];
                mv.pos[0] = M[0] * x + M[4] * y + M[8]  * z + M[12];
                mv.pos[1] = M[1] * x + M[5] * y + M[9]  * z + M[13];
                mv.pos[2] = M[2] * x + M[6] * y + M[10] * z + M[14];
                if (v * 3 + 2 < nrm.size()) {
                    const float nx = nrm[v * 3 + 0], ny = nrm[v * 3 + 1], nz = nrm[v * 3 + 2];
                    // Normals through the upper-left 3x3 (no non-uniform-scale
                    // inverse-transpose: these assets are uniformly scaled, and a
                    // renormalise is enough for LOD purposes).
                    float tx = M[0] * nx + M[4] * ny + M[8]  * nz;
                    float ty = M[1] * nx + M[5] * ny + M[9]  * nz;
                    float tz = M[2] * nx + M[6] * ny + M[10] * nz;
                    const float l = std::sqrt(tx * tx + ty * ty + tz * tz);
                    if (l > 1e-8f) { tx /= l; ty /= l; tz /= l; }
                    mv.normal[0] = tx; mv.normal[1] = ty; mv.normal[2] = tz;
                } else {
                    mv.normal[1] = 1.0f;
                }
                if (v * 2 + 1 < uv.size()) { mv.uv[0] = uv[v * 2 + 0]; mv.uv[1] = uv[v * 2 + 1]; }
            }
            bool bad = false;
            for (uint32_t i : ind) if (i >= vcount) { bad = true; break; }
            if (bad) continue;
            gp.idx = std::move(ind);
            out.prims.push_back(std::move(gp));
        }
    }

    out.ok = !out.prims.empty();
    if (!out.ok && out.error.empty()) out.error = "no triangle primitives above the size floor";
    return out;
}

} // namespace x3::game
