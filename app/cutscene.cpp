// x3.cutscene/1 implementation — see cutscene.h + docs/design/CUTSCENE_FORMAT.md.
//
// Self-contained: a dependency-free recursive-descent JSON parser (same stance as
// level_loader.cpp — parses well-formed input, fails gracefully on malformed), pure
// spline/overlay evaluation, the deterministic CutscenePlayer, StoryFlags, and the
// --test-cutscene self-test. No window / Vulkan / GLFW / audio types anywhere.

#include "cutscene.h"

#include "asset_root.h"             // assetRoot() — locate the shipped cold_open json
#include "engine/core/x3_log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>                  // std::getenv
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace x3::cut {

// ===========================================================================
// Minimal JSON (recursive descent; objects/arrays/strings/numbers/bool/null)
// ===========================================================================
namespace {

struct JValue {
    enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
    bool        b = false;
    double      num = 0.0;
    std::string str;
    std::vector<JValue> arr;
    std::vector<std::pair<std::string, JValue>> obj;

    const JValue* find(const char* key) const {
        if (t != T::Obj) return nullptr;
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    float fnum(float def = 0.0f) const { return t == T::Num ? (float)num : def; }
};

struct JParser {
    const char* p;
    const char* end;
    bool failed = false;

    explicit JParser(std::string_view s) : p(s.data()), end(s.data() + s.size()) {}

    void skipWs() {
        while (p < end) {
            if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') { ++p; continue; }
            if (*p == '/' && p + 1 < end && p[1] == '/') {           // // comment
                while (p < end && *p != '\n') ++p;
                continue;
            }
            break;
        }
    }
    bool eof() { skipWs(); return p >= end; }

    JValue parseValue() {
        skipWs();
        if (p >= end) { failed = true; return {}; }
        const char c = *p;
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') { JValue v; v.t = JValue::T::Str; v.str = parseString(); return v; }
        if (c == 't' || c == 'f') return parseBool();
        if (c == 'n') { p += 4 <= end - p ? 4 : end - p; return {}; }
        return parseNumber();
    }
    std::string parseString() {
        std::string s;
        if (p >= end || *p != '"') { failed = true; return s; }
        ++p;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case 'u': { // \uXXXX -> keep ASCII subset, others become '?'
                        if (end - p >= 5) {
                            unsigned v = (unsigned)std::strtoul(std::string(p + 1, p + 5).c_str(), nullptr, 16);
                            s += (v < 128) ? (char)v : '?';
                            p += 4;
                        }
                        break;
                    }
                    default: s += *p; break;
                }
                ++p;
            } else {
                s += *p++;
            }
        }
        if (p < end) ++p; else failed = true;
        return s;
    }
    JValue parseNumber() {
        JValue v; v.t = JValue::T::Num;
        char* outEnd = nullptr;
        v.num = std::strtod(p, &outEnd);
        if (outEnd == p) { failed = true; return {}; }
        p = outEnd;
        return v;
    }
    JValue parseBool() {
        JValue v; v.t = JValue::T::Bool;
        if (end - p >= 4 && std::string_view(p, 4) == "true")  { v.b = true;  p += 4; return v; }
        if (end - p >= 5 && std::string_view(p, 5) == "false") { v.b = false; p += 5; return v; }
        failed = true; return {};
    }
    JValue parseArray() {
        JValue v; v.t = JValue::T::Arr;
        ++p; // '['
        skipWs();
        if (p < end && *p == ']') { ++p; return v; }
        while (p < end && !failed) {
            v.arr.push_back(parseValue());
            skipWs();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == ']') { ++p; return v; }
            break;
        }
        failed = true; return v;
    }
    JValue parseObject() {
        JValue v; v.t = JValue::T::Obj;
        ++p; // '{'
        skipWs();
        if (p < end && *p == '}') { ++p; return v; }
        while (p < end && !failed) {
            skipWs();
            if (p >= end || *p != '"') break;
            std::string key = parseString();
            skipWs();
            if (p >= end || *p != ':') break;
            ++p;
            JValue val = parseValue();
            v.obj.emplace_back(std::move(key), std::move(val));
            skipWs();
            if (p < end && *p == ',') { ++p; continue; }
            if (p < end && *p == '}') { ++p; return v; }
            break;
        }
        failed = true; return v;
    }
};

// ---- field readers --------------------------------------------------------
float jf(const JValue* o, const char* key, float def) {
    const JValue* v = o ? o->find(key) : nullptr;
    return v ? v->fnum(def) : def;
}
bool jb(const JValue* o, const char* key, bool def) {
    const JValue* v = o ? o->find(key) : nullptr;
    return (v && v->t == JValue::T::Bool) ? v->b : def;
}
std::string js(const JValue* o, const char* key, const char* def = "") {
    const JValue* v = o ? o->find(key) : nullptr;
    return (v && v->t == JValue::T::Str) ? v->str : std::string(def);
}
Vec3 jv3(const JValue* o, const char* key, Vec3 def = {}) {
    const JValue* v = o ? o->find(key) : nullptr;
    if (!v || v->t != JValue::T::Arr || v->arr.size() < 3) return def;
    return { v->arr[0].fnum(), v->arr[1].fnum(), v->arr[2].fnum() };
}
void jfn(const JValue* o, const char* key, float* out, size_t n) {   // float[n], keep defaults on miss
    const JValue* v = o ? o->find(key) : nullptr;
    if (!v || v->t != JValue::T::Arr) return;
    for (size_t i = 0; i < n && i < v->arr.size(); ++i) out[i] = v->arr[i].fnum(out[(int)i]);
}

constexpr float kPi = 3.14159265358979f;

} // namespace

// ===========================================================================
// Parse
// ===========================================================================
bool parseCutscene(std::string_view jsonText, Cutscene& out, std::vector<std::string>& errors) {
    JParser jp(jsonText);
    JValue root = jp.parseValue();
    if (jp.failed || root.t != JValue::T::Obj) {
        errors.push_back("cutscene: JSON parse failed");
        return false;
    }
    const std::string fmt = js(&root, "format");
    if (fmt != "x3.cutscene/1") {
        errors.push_back("cutscene: missing/unknown format magic (want \"x3.cutscene/1\", got \"" + fmt + "\")");
        return false;
    }

    Cutscene cs;
    cs.name      = js(&root, "name", "unnamed");
    cs.duration  = jf(&root, "duration", 0.0f);
    cs.skippable = jb(&root, "skippable", true);
    cs.skipTo    = jf(&root, "skipTo", -1.0f);

    // ---- camera ----
    if (const JValue* cam = root.find("camera")) {
        if (const JValue* keys = cam->find("keys"); keys && keys->t == JValue::T::Arr) {
            for (const JValue& k : keys->arr) {
                CameraKey ck;
                ck.t    = jf(&k, "t", 0.0f);
                ck.pos  = jv3(&k, "pos");
                ck.look = jv3(&k, "look");
                ck.fov  = jf(&k, "fov", 60.0f);
                ck.roll = jf(&k, "roll", 0.0f);   // dutch angle (deg); absent -> 0 (level)
                ck.cut  = jb(&k, "cut", false);
                cs.camKeys.push_back(ck);
            }
        }
        if (const JValue* sh = cam->find("shakes"); sh && sh->t == JValue::T::Arr) {
            for (const JValue& s : sh->arr) {
                ShakeBurst b;
                b.t = jf(&s, "t", 0.0f); b.dur = jf(&s, "dur", 0.0f);
                b.amp = jf(&s, "amp", 0.0f); b.freq = jf(&s, "freq", 10.0f);
                cs.shakes.push_back(b);
            }
        }
    }

    // ---- actors ----
    if (const JValue* actors = root.find("actors"); actors && actors->t == JValue::T::Arr) {
        for (const JValue& a : actors->arr) {
            Actor ac;
            ac.id    = js(&a, "id");
            ac.model = js(&a, "model");
            ac.size  = jf(&a, "size", 0.0f);
            ac.rotOffsetDeg = jv3(&a, "rotOffsetDeg");
            ac.stretch      = jv3(&a, "stretch", {1, 1, 1});
            jfn(&a, "color",    ac.color, 4);
            jfn(&a, "emissive", ac.emissive, 4);
            // Blob->detailed emissive ramp (Phase 5; all optional, legacy-safe).
            std::copy(ac.emissive, ac.emissive + 4, ac.emissiveFrom);   // default: no ramp (from == to)
            jfn(&a, "emissiveFrom", ac.emissiveFrom, 4);
            ac.emissiveRampAt  = jf(&a, "emissiveRampAt", 0.0f);
            ac.emissiveRampDur = jf(&a, "emissiveRampDur", 0.0f);
            ac.showAt = jf(&a, "showAt", 0.0f);
            ac.hideAt = jf(&a, "hideAt", -1.0f);
            if (const JValue* keys = a.find("keys"); keys && keys->t == JValue::T::Arr) {
                for (const JValue& k : keys->arr) {
                    ActorKey ak;
                    ak.t = jf(&k, "t", 0.0f);
                    ak.pos = jv3(&k, "pos");
                    ak.rotDeg = jv3(&k, "rotDeg");
                    ak.scale = jf(&k, "scale", 1.0f);
                    ac.keys.push_back(ak);
                }
            }
            cs.actors.push_back(std::move(ac));
        }
    }

    // ---- audio ----
    if (const JValue* au = root.find("audio"); au && au->t == JValue::T::Arr) {
        for (const JValue& c : au->arr) {
            AudioCue cue;
            cue.t = jf(&c, "t", 0.0f);
            cue.sound = js(&c, "sound");
            cue.gain = jf(&c, "gain", 1.0f);
            cue.music = jb(&c, "music", false);
            cs.audio.push_back(std::move(cue));
        }
    }

    // ---- fades ----
    if (const JValue* fa = root.find("fades"); fa && fa->t == JValue::T::Arr) {
        for (const JValue& f : fa->arr) {
            Fade fd;
            fd.t = jf(&f, "t", 0.0f); fd.dur = jf(&f, "dur", 0.0f);
            fd.from = jf(&f, "from", 0.0f); fd.to = jf(&f, "to", 0.0f);
            jfn(&f, "color", fd.color, 3);
            cs.fades.push_back(fd);
        }
    }

    // ---- letterbox ----
    if (const JValue* lb = root.find("letterbox")) {
        cs.letterbox.present = true;
        cs.letterbox.inAt   = jf(lb, "inAt", 0.0f);
        cs.letterbox.inDur  = jf(lb, "inDur", 0.5f);
        cs.letterbox.outAt  = jf(lb, "outAt", cs.duration);
        cs.letterbox.outDur = jf(lb, "outDur", 0.5f);
        cs.letterbox.frac   = jf(lb, "frac", 0.11f);
    }

    // ---- titles ----
    if (const JValue* ti = root.find("titles"); ti && ti->t == JValue::T::Arr) {
        for (const JValue& t : ti->arr) {
            TitleCard tc;
            tc.t = jf(&t, "t", 0.0f); tc.dur = jf(&t, "dur", 0.0f);
            tc.text = js(&t, "text");
            tc.font = js(&t, "font", "title");
            tc.sizeFrac = jf(&t, "sizeFrac", 0.07f);
            tc.fadeIn = jf(&t, "fadeIn", 0.5f); tc.fadeOut = jf(&t, "fadeOut", 0.5f);
            jfn(&t, "color", tc.color, 3);
            cs.titles.push_back(std::move(tc));
        }
    }

    // ---- events ----
    if (const JValue* ev = root.find("events"); ev && ev->t == JValue::T::Arr) {
        for (const JValue& e : ev->arr) {
            Event x;
            x.t = jf(&e, "t", 0.0f);
            x.name = js(&e, "name");
            x.endState = jb(&e, "endState", false);
            cs.events.push_back(std::move(x));
        }
    }

    if (!validate(cs, errors)) return false;
    out = std::move(cs);
    return true;
}

bool loadCutsceneFile(const std::string& path, Cutscene& out, std::vector<std::string>& errors) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        errors.push_back("cutscene: cannot open " + path);
        return false;
    }
    std::stringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();
    return parseCutscene(text, out, errors);
}

// ===========================================================================
// Validate
// ===========================================================================
namespace {
bool sortedTimes(const float* prev, float t) { return *prev <= t; }
const char* kKnownFonts[] = { "title", "menu", "news", "mono" };
} // namespace

bool validate(const Cutscene& cs, std::vector<std::string>& errors) {
    const size_t before = errors.size();
    auto err = [&](const std::string& m) { errors.push_back("cutscene '" + cs.name + "': " + m); };

    if (!(cs.duration > 0.0f)) err("duration must be > 0");
    const float tMax = cs.duration + 0.5f;
    auto inRange = [&](float t) { return t >= 0.0f && t <= tMax; };

    // camera
    if (cs.camKeys.size() < 2) err("camera needs >= 2 keys");
    for (size_t i = 0; i < cs.camKeys.size(); ++i) {
        const CameraKey& k = cs.camKeys[i];
        if (!inRange(k.t)) err("camera key t out of range: " + std::to_string(k.t));
        if (k.fov <= 1.0f || k.fov >= 170.0f) err("camera fov out of (1,170): " + std::to_string(k.fov));
        if (i > 0 && !(cs.camKeys[i - 1].t <= k.t)) err("camera keys not sorted by t");
    }
    for (const ShakeBurst& s : cs.shakes) {
        if (!inRange(s.t) || s.dur < 0.0f) err("shake t/dur invalid");
        if (s.amp < 0.0f) err("shake amp < 0");
    }

    // actors
    for (size_t ai = 0; ai < cs.actors.size(); ++ai) {
        const Actor& a = cs.actors[ai];
        if (a.id.empty())    err("actor " + std::to_string(ai) + " has empty id");
        if (a.model.empty()) err("actor '" + a.id + "' has empty model");
        if (a.keys.empty())  err("actor '" + a.id + "' has no keys");
        for (size_t i = 0; i < a.keys.size(); ++i) {
            if (!inRange(a.keys[i].t)) err("actor '" + a.id + "' key t out of range");
            if (i > 0 && !(a.keys[i - 1].t <= a.keys[i].t)) err("actor '" + a.id + "' keys not sorted");
        }
        for (size_t bi = 0; bi < ai; ++bi)
            if (cs.actors[bi].id == a.id) err("duplicate actor id '" + a.id + "'");
    }

    // audio / fades / titles / events
    for (const AudioCue& c : cs.audio) {
        if (c.sound.empty()) err("audio cue with empty sound name");
        if (!inRange(c.t)) err("audio cue t out of range");
    }
    for (const Fade& f : cs.fades)
        if (!inRange(f.t) || f.dur < 0.0f) err("fade t/dur invalid");
    if (cs.letterbox.present && (cs.letterbox.frac < 0.0f || cs.letterbox.frac > 0.45f))
        err("letterbox frac out of [0,0.45]");
    for (const TitleCard& t : cs.titles) {
        if (t.text.empty()) err("title card with empty text");
        if (!inRange(t.t) || t.dur <= 0.0f) err("title card t/dur invalid");
        bool known = false;
        for (const char* f : kKnownFonts) if (t.font == f) { known = true; break; }
        if (!known) err("title card unknown font '" + t.font + "'");
    }
    for (const Event& e : cs.events) {
        if (e.name.empty()) err("event with empty name");
        if (!inRange(e.t)) err("event t out of range");
    }
    if (cs.skipTo >= 0.0f && cs.skipTo > cs.duration + 1e-3f) err("skipTo beyond duration");

    return errors.size() == before;
}

// ===========================================================================
// Evaluation
// ===========================================================================
namespace {

float catmull(float p0, float p1, float p2, float p3, float u) {
    const float u2 = u * u, u3 = u2 * u;
    return 0.5f * ((2.0f * p1) +
                   (-p0 + p2) * u +
                   (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * u3);
}
Vec3 catmull(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d, float u) {
    return { catmull(a.x, b.x, c.x, d.x, u),
             catmull(a.y, b.y, c.y, d.y, u),
             catmull(a.z, b.z, c.z, d.z, u) };
}
float lerpf(float a, float b, float u) { return a + (b - a) * u; }
Vec3 lerp3(const Vec3& a, const Vec3& b, float u) {
    return { lerpf(a.x, b.x, u), lerpf(a.y, b.y, u), lerpf(a.z, b.z, u) };
}

// Find segment i with keys[i].t <= t < keys[i+1].t (clamped), and the local u.
template <typename K>
void segment(const std::vector<K>& keys, float t, size_t& i, float& u) {
    const size_t n = keys.size();
    if (t <= keys.front().t) { i = 0; u = 0.0f; return; }
    if (t >= keys.back().t)  { i = n >= 2 ? n - 2 : 0; u = 1.0f; return; }
    i = 0;
    while (i + 1 < n && keys[i + 1].t <= t) ++i;
    if (i + 1 >= n) { i = n - 2; u = 1.0f; return; }
    const float span = keys[i + 1].t - keys[i].t;
    u = span > 1e-6f ? (t - keys[i].t) / span : 0.0f;
}

} // namespace

CamPose evalCamera(const Cutscene& cs, float t) {
    CamPose out;
    const auto& K = cs.camKeys;
    if (K.empty()) return out;

    // ---- Resolve the SHOT SPAN [lo, hi]: runs of keys delimited by cut flags.
    // Interpolation (CR neighbors included) never crosses a cut boundary, so a
    // "cut": true key is a clean hard cut into a new camera setup.
    size_t lo = 0, hi = K.size() - 1;
    {
        // span start: the last cut key with t <= t (or 0).
        for (size_t i = 1; i < K.size(); ++i)
            if (K[i].cut && K[i].t <= t) lo = i;
        // span end: the key before the NEXT cut after lo.
        for (size_t i = lo + 1; i < K.size(); ++i)
            if (K[i].cut) { hi = i - 1; break; }
    }

    const float kDeg2Rad = kPi / 180.0f;
    if (lo == hi) {
        out.pos = K[lo].pos; out.fov = K[lo].fov;
        out.roll = K[lo].roll * kDeg2Rad;
        float fx = K[lo].look.x - out.pos.x, fy = K[lo].look.y - out.pos.y, fz = K[lo].look.z - out.pos.z;
        const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (len > 1e-5f) { fx /= len; fy /= len; fz /= len; }
        out.yaw = std::atan2(fz, fx);
        out.pitch = std::asin(fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy));
    } else {
        // Segment search + CR within [lo, hi] only (endpoint-clamped neighbors).
        size_t i = lo; float u = 0.0f;
        if (t <= K[lo].t)      { i = lo; u = 0.0f; }
        else if (t >= K[hi].t) { i = hi - 1; u = 1.0f; }
        else {
            while (i + 1 < hi && K[i + 1].t <= t) ++i;
            const float span = K[i + 1].t - K[i].t;
            u = span > 1e-6f ? (t - K[i].t) / span : 0.0f;
        }
        const CameraKey& k0 = K[i > lo ? i - 1 : lo];
        const CameraKey& k1 = K[i];
        const CameraKey& k2 = K[i + 1 <= hi ? i + 1 : hi];
        const CameraKey& k3 = K[i + 2 <= hi ? i + 2 : hi];
        out.pos = catmull(k0.pos, k1.pos, k2.pos, k3.pos, u);
        const Vec3 look = catmull(k0.look, k1.look, k2.look, k3.look, u);
        out.fov = lerpf(k1.fov, k2.fov, u);
        out.roll = lerpf(k1.roll, k2.roll, u) * kDeg2Rad;   // dutch angle eases like fov
        // Derive yaw/pitch (device convention: fwd = (cos p cos y, sin p, cos p sin y)).
        float fx = look.x - out.pos.x, fy = look.y - out.pos.y, fz = look.z - out.pos.z;
        const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (len > 1e-5f) { fx /= len; fy /= len; fz /= len; }
        out.yaw   = std::atan2(fz, fx);
        out.pitch = std::asin(fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy));
    }
    // ---- summed deterministic shake ----
    float sx = 0, sy = 0, sz = 0;
    for (const ShakeBurst& s : cs.shakes) {
        if (s.dur <= 0.0f || t < s.t || t > s.t + s.dur) continue;
        const float decay = 1.0f - (t - s.t) / s.dur;
        const float w = 2.0f * kPi * s.freq;
        sx += s.amp * decay * std::sin(w * t);
        sy += s.amp * decay * std::sin(w * 1.31f * t + 2.1f);
        sz += s.amp * decay * std::sin(w * 0.73f * t + 4.2f);
    }
    out.pos.x += sx; out.pos.y += sy; out.pos.z += sz;
    return out;
}

// Time-evaluate the actor emissive (the blob->detailed reveal ramp, or the static
// emissive). Lerps emissiveFrom -> emissive over [emissiveRampAt, +Dur], holding the
// endpoints outside the window. With emissiveRampDur <= 0 this is just a.emissive
// (legacy: every actor without a ramp keeps its static self-illum at all times).
static void evalActorEmissive(const Actor& a, float t, float out[4]) {
    if (a.emissiveRampDur <= 1e-5f) { std::copy(a.emissive, a.emissive + 4, out); return; }
    float u = (t - a.emissiveRampAt) / a.emissiveRampDur;
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    for (int c = 0; c < 4; ++c) out[c] = lerpf(a.emissiveFrom[c], a.emissive[c], u);
}

ActorPose evalActor(const Cutscene& cs, const Actor& a, float t) {
    ActorPose out;
    evalActorEmissive(a, t, out.emissive);
    if (a.keys.empty()) return out;
    const float hide = a.hideAt < 0.0f ? cs.duration : a.hideAt;
    out.visible = (t >= a.showAt && t <= hide);
    if (a.keys.size() == 1) {
        out.pos = a.keys[0].pos; out.rotDeg = a.keys[0].rotDeg; out.scale = a.keys[0].scale;
        return out;
    }
    size_t i; float u;
    segment(a.keys, t, i, u);
    const size_t n = a.keys.size();
    const ActorKey& k0 = a.keys[i == 0 ? 0 : i - 1];
    const ActorKey& k1 = a.keys[i];
    const ActorKey& k2 = a.keys[i + 1 < n ? i + 1 : n - 1];
    const ActorKey& k3 = a.keys[i + 2 < n ? i + 2 : n - 1];
    out.pos    = catmull(k0.pos, k1.pos, k2.pos, k3.pos, u);
    out.rotDeg = lerp3(k1.rotDeg, k2.rotDeg, u);
    out.scale  = lerpf(k1.scale, k2.scale, u);
    return out;
}

void evalFade(const Cutscene& cs, float t, float outRgba[4]) {
    outRgba[0] = outRgba[1] = outRgba[2] = outRgba[3] = 0.0f;
    const Fade* best = nullptr;
    for (const Fade& f : cs.fades)
        if (f.t <= t && (!best || f.t >= best->t)) best = &f;
    if (!best) return;
    float a;
    if (best->dur <= 1e-5f)          a = best->to;
    else if (t >= best->t + best->dur) a = best->to;
    else                              a = lerpf(best->from, best->to, (t - best->t) / best->dur);
    outRgba[0] = best->color[0]; outRgba[1] = best->color[1]; outRgba[2] = best->color[2];
    outRgba[3] = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}

float evalLetterbox(const Cutscene& cs, float t) {
    const Letterbox& lb = cs.letterbox;
    if (!lb.present) return 0.0f;
    float k = 0.0f;
    if (t >= lb.inAt) k = lb.inDur > 1e-5f ? (t - lb.inAt) / lb.inDur : 1.0f;
    if (k > 1.0f) k = 1.0f;
    if (t >= lb.outAt) {
        const float o = lb.outDur > 1e-5f ? 1.0f - (t - lb.outAt) / lb.outDur : 0.0f;
        if (o < k) k = o;
    }
    if (k < 0.0f) k = 0.0f;
    return lb.frac * k;
}

float evalTitleAlpha(const TitleCard& tc, float t) {
    if (t < tc.t || t > tc.t + tc.dur) return 0.0f;
    float a = 1.0f;
    if (tc.fadeIn > 1e-5f)  a = std::min(a, (t - tc.t) / tc.fadeIn);
    if (tc.fadeOut > 1e-5f) a = std::min(a, (tc.t + tc.dur - t) / tc.fadeOut);
    return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
}

void actorMatrix(const Actor& a, const ActorPose& p, float normScale, float out[16]) {
    auto mul = [](const float m1[16], const float m2[16], float o[16]) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                o[c * 4 + r] = m1[0 * 4 + r] * m2[c * 4 + 0] + m1[1 * 4 + r] * m2[c * 4 + 1] +
                               m1[2 * 4 + r] * m2[c * 4 + 2] + m1[3 * 4 + r] * m2[c * 4 + 3];
    };
    auto rotY = [](float a_, float m[16]) {
        const float c = std::cos(a_), s = std::sin(a_);
        const float r[16] = { c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1 };
        std::copy(r, r + 16, m);
    };
    auto rotX = [](float a_, float m[16]) {
        const float c = std::cos(a_), s = std::sin(a_);
        const float r[16] = { 1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1 };
        std::copy(r, r + 16, m);
    };
    auto rotZ = [](float a_, float m[16]) {
        const float c = std::cos(a_), s = std::sin(a_);
        const float r[16] = { c,s,0,0, -s,c,0,0, 0,0,1,0, 0,0,0,1 };
        std::copy(r, r + 16, m);
    };
    const float d2r = kPi / 180.0f;

    float ry[16], rx[16], rz[16], t0[16], rKey[16];
    rotY(p.rotDeg.x * d2r, ry); rotX(p.rotDeg.y * d2r, rx); rotZ(p.rotDeg.z * d2r, rz);
    mul(ry, rx, t0); mul(t0, rz, rKey);

    float oy[16], ox[16], oz[16], t1[16], rOff[16];
    rotY(a.rotOffsetDeg.x * d2r, oy); rotX(a.rotOffsetDeg.y * d2r, ox); rotZ(a.rotOffsetDeg.z * d2r, oz);
    mul(oy, ox, t1); mul(t1, oz, rOff);

    float rot[16];
    mul(rKey, rOff, rot);

    const float s = p.scale * (normScale > 0.0f ? normScale : 1.0f);
    const float sx = s * a.stretch.x, sy = s * a.stretch.y, sz = s * a.stretch.z;
    float scale[16] = { sx,0,0,0, 0,sy,0,0, 0,0,sz,0, 0,0,0,1 };

    float rs[16];
    mul(rot, scale, rs);
    std::copy(rs, rs + 16, out);
    out[12] = p.pos.x; out[13] = p.pos.y; out[14] = p.pos.z; out[15] = 1.0f;
}

// ===========================================================================
// CutscenePlayer
// ===========================================================================
void CutscenePlayer::rebuildOrder() {
    m_evOrder.resize(m_cs->events.size());
    for (uint32_t i = 0; i < m_evOrder.size(); ++i) m_evOrder[i] = i;
    std::stable_sort(m_evOrder.begin(), m_evOrder.end(), [this](uint32_t a, uint32_t b) {
        return m_cs->events[a].t < m_cs->events[b].t;
    });
    m_evFired.assign(m_cs->events.size(), false);
    m_auFired.assign(m_cs->audio.size(), false);
}

void CutscenePlayer::fireRange(float /*from*/, float to, int mode) {
    // Audio first at each instant would interleave; in practice cue order within a
    // frame is inaudible — fire audio (play mode only), then events in (t, order).
    if (mode == 0 && m_onAudio) {
        for (size_t i = 0; i < m_cs->audio.size(); ++i) {
            if (m_auFired[i]) continue;
            if (m_cs->audio[i].t <= to) { m_auFired[i] = true; m_onAudio(m_cs->audio[i]); }
        }
    } else {
        for (size_t i = 0; i < m_cs->audio.size(); ++i)
            if (!m_auFired[i] && m_cs->audio[i].t <= to) m_auFired[i] = true;   // consumed silently
    }
    for (uint32_t idx : m_evOrder) {
        if (m_evFired[idx]) continue;
        const Event& e = m_cs->events[idx];
        if (e.t > to) continue;
        m_evFired[idx] = true;
        if (mode == 2 && !e.endState) continue;          // skip drops non-endState
        if (m_onEvent) m_onEvent(e, mode != 0);
    }
}

void CutscenePlayer::tick(float dt) {
    if (dt < 0.0f) dt = 0.0f;
    if (done()) return;
    const float from = m_t;
    m_t = std::min(m_t + dt, m_cs->duration);
    fireRange(from, m_t, /*play*/ 0);
}

void CutscenePlayer::seek(float t) {
    t = std::max(0.0f, std::min(t, m_cs->duration));
    if (t >= m_t) {
        const float from = m_t;
        m_t = t;
        fireRange(from, m_t, /*seek*/ 1);
    } else {
        // Backward: rewind fired-state past the new playhead so replay is deterministic.
        m_t = t;
        for (size_t i = 0; i < m_cs->events.size(); ++i)
            if (m_cs->events[i].t > t) m_evFired[i] = false;
        for (size_t i = 0; i < m_cs->audio.size(); ++i)
            if (m_cs->audio[i].t > t) m_auFired[i] = false;
    }
}

void CutscenePlayer::skip() {
    if (!m_cs->skippable || done()) return;
    const float target = (m_t < m_cs->skipTarget() - 1e-4f) ? m_cs->skipTarget()
                                                            : m_cs->duration;
    const float from = m_t;
    m_t = target;
    m_skipped = true;
    fireRange(from, m_t, /*skip*/ 2);
}

uint32_t CutscenePlayer::firedEventCount() const {
    uint32_t n = 0;
    for (bool b : m_evFired) n += b ? 1u : 0u;
    return n;
}
uint32_t CutscenePlayer::firedAudioCount() const {
    uint32_t n = 0;
    for (bool b : m_auFired) n += b ? 1u : 0u;
    return n;
}

// ===========================================================================
// StoryFlags
// ===========================================================================
bool StoryFlags::has(const std::string& flag) const {
    return std::binary_search(m_flags.begin(), m_flags.end(), flag);
}
void StoryFlags::set(const std::string& flag) {
    auto it = std::lower_bound(m_flags.begin(), m_flags.end(), flag);
    if (it == m_flags.end() || *it != flag) m_flags.insert(it, flag);
}
void StoryFlags::clear(const std::string& flag) {
    auto it = std::lower_bound(m_flags.begin(), m_flags.end(), flag);
    if (it != m_flags.end() && *it == flag) m_flags.erase(it);
}
bool StoryFlags::load(const std::string& path) {
    m_flags.clear();
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) set(line);
    }
    return true;
}
bool StoryFlags::save(const std::string& path) const {
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    for (const std::string& s : m_flags) f << s << '\n';
    return (bool)f;
}
std::string defaultStoryFlagsPath() {
    if (const char* la = std::getenv("LOCALAPPDATA"); la && *la)
        return std::string(la) + "/X3Native/story_flags.txt";
    return "story_flags.txt";
}

// ===========================================================================
// GLB POSITION extent (JSON-chunk only; see cutscene.h)
// ===========================================================================
bool glbPositionExtent(const std::string& path, float outMin[3], float outMax[3]) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    uint32_t hdr[3] = {};   // magic, version, length
    f.read(reinterpret_cast<char*>(hdr), 12);
    if (!f || hdr[0] != 0x46546C67u) return false;   // 'glTF'
    uint32_t chunk[2] = {};                          // length, type
    f.read(reinterpret_cast<char*>(chunk), 8);
    if (!f || chunk[1] != 0x4E4F534Au) return false; // 'JSON'
    if (chunk[0] == 0 || chunk[0] > 64u * 1024u * 1024u) return false;
    std::string json(chunk[0], '\0');
    f.read(json.data(), chunk[0]);
    if (!f) return false;

    JParser jp(json);
    JValue root = jp.parseValue();
    if (jp.failed || root.t != JValue::T::Obj) return false;
    const JValue* accessors = root.find("accessors");
    const JValue* meshes    = root.find("meshes");
    if (!accessors || accessors->t != JValue::T::Arr ||
        !meshes || meshes->t != JValue::T::Arr) return false;

    // Collect the accessor indices used as POSITION across all primitives.
    std::vector<size_t> posAcc;
    for (const JValue& m : meshes->arr) {
        const JValue* prims = m.find("primitives");
        if (!prims || prims->t != JValue::T::Arr) continue;
        for (const JValue& p : prims->arr) {
            const JValue* attrs = p.find("attributes");
            const JValue* pos = attrs ? attrs->find("POSITION") : nullptr;
            if (pos && pos->t == JValue::T::Num) posAcc.push_back((size_t)pos->num);
        }
    }
    if (posAcc.empty()) return false;

    bool any = false;
    float mn[3] = { 0, 0, 0 }, mx[3] = { 0, 0, 0 };
    for (size_t idx : posAcc) {
        if (idx >= accessors->arr.size()) continue;
        const JValue& a = accessors->arr[idx];
        const JValue* amin = a.find("min");
        const JValue* amax = a.find("max");
        if (!amin || amin->t != JValue::T::Arr || amin->arr.size() < 3 ||
            !amax || amax->t != JValue::T::Arr || amax->arr.size() < 3) continue;
        for (int c = 0; c < 3; ++c) {
            const float lo = amin->arr[c].fnum(), hi = amax->arr[c].fnum();
            if (!any) { mn[c] = lo; mx[c] = hi; }
            else      { mn[c] = std::min(mn[c], lo); mx[c] = std::max(mx[c], hi); }
        }
        any = true;
    }
    if (!any) return false;
    for (int c = 0; c < 3; ++c) { outMin[c] = mn[c]; outMax[c] = mx[c]; }
    return true;
}

// ===========================================================================
// --test-cutscene self-test
// ===========================================================================
namespace {
int g_pass = 0, g_total = 0;
void check(bool cond, const char* name) {
    ++g_total;
    if (cond) { ++g_pass; x3::logInfo(std::string("[cutscene-test] PASS ") + name); }
    else      {           x3::logError(std::string("[cutscene-test] FAIL ") + name); }
}

const char* kSyntheticJson = R"JSON({
  "format": "x3.cutscene/1",
  "name": "synthetic",
  "duration": 10.0,
  "skippable": true,
  "skipTo": 8.0,
  "camera": {
    "keys": [
      { "t": 0.0, "pos": [0, 0, 0],  "look": [10, 0, 0],  "fov": 60.0 },
      { "t": 5.0, "pos": [0, 5, 0],  "look": [10, 5, 0],  "fov": 40.0 },
      { "t": 10.0, "pos": [0, 5, 10], "look": [10, 5, 10], "fov": 40.0 }
    ],
    "shakes": [ { "t": 6.0, "dur": 1.0, "amp": 0.5, "freq": 12.0 } ]
  },
  "actors": [
    {
      "id": "ship", "model": "builtin:glow", "size": 4.0,
      "showAt": 1.0, "hideAt": 9.0,
      "keys": [
        { "t": 0.0, "pos": [0, 0, -10], "rotDeg": [0, 0, 0],  "scale": 1.0 },
        { "t": 10.0, "pos": [0, 0, 10], "rotDeg": [90, 0, 0], "scale": 2.0 }
      ]
    }
  ],
  "audio": [
    { "t": 2.0, "sound": "alarm", "gain": 0.8 },
    { "t": 9.0, "sound": "boom" }
  ],
  "fades": [
    { "t": 0.0, "dur": 1.0, "from": 1.0, "to": 0.0 },
    { "t": 7.0, "dur": 0.5, "from": 0.0, "to": 1.0 }
  ],
  "letterbox": { "inAt": 0.0, "inDur": 1.0, "outAt": 7.0, "outDur": 1.0, "frac": 0.1 },
  "titles": [
    { "t": 8.0, "dur": 2.0, "text": "TEST CARD", "font": "title", "fadeIn": 0.5, "fadeOut": 0.5 }
  ],
  "events": [
    { "t": 3.0, "name": "midpoint" },
    { "t": 6.5, "name": "fx.impact:ship" },
    { "t": 10.0, "name": "intro_complete", "endState": true }
  ]
})JSON";

bool nearf(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
} // namespace

bool runCutsceneSelfTest() {
    g_pass = g_total = 0;

    // ---- 1) Parse the synthetic document ----
    Cutscene cs;
    std::vector<std::string> errs;
    const bool parsed = parseCutscene(kSyntheticJson, cs, errs);
    for (const auto& e : errs) x3::logError("[cutscene-test] " + e);
    check(parsed, "synthetic JSON parses");
    if (!parsed) return false;
    check(cs.duration == 10.0f && cs.skipTarget() == 8.0f, "duration + skipTo parsed");
    check(cs.camKeys.size() == 3 && cs.shakes.size() == 1, "camera track parsed (3 keys, 1 shake)");
    check(cs.actors.size() == 1 && cs.actors[0].keys.size() == 2, "actor track parsed");
    check(cs.audio.size() == 2 && cs.fades.size() == 2 && cs.titles.size() == 1 &&
          cs.events.size() == 3, "audio/fade/title/event tracks parsed");
    check(cs.letterbox.present && nearf(cs.letterbox.frac, 0.1f), "letterbox parsed");

    // ---- 2) Rejection paths ----
    {
        Cutscene bad; std::vector<std::string> e2;
        check(!parseCutscene("{ \"duration\": 5 }", bad, e2), "rejects missing format magic");
        e2.clear();
        check(!parseCutscene("{ \"format\": \"x3.cutscene/1\", \"duration\": 0 }", bad, e2),
              "rejects duration 0 / missing camera");
        e2.clear();
        std::string unsorted = kSyntheticJson;
        size_t p = unsorted.find("\"t\": 0.0");          // first camera key
        unsorted.replace(p, 8, "\"t\": 8.0");            // keys now 8.0, 5.0, 10.0 — unsorted
        check(!parseCutscene(unsorted, bad, e2), "rejects unsorted camera keys");
        e2.clear();
        std::string badFont = kSyntheticJson;
        p = badFont.find("\"font\": \"title\"");
        badFont.replace(p, 15, "\"font\": \"wingd\"");
        check(!parseCutscene(badFont, bad, e2), "rejects unknown title font");
        e2.clear();
        check(!parseCutscene("not json at all {", bad, e2), "rejects malformed JSON");
    }

    // ---- 3) Camera eval: hits keys, lerps FOV, derives yaw/pitch, shake windows ----
    {
        CamPose c0 = evalCamera(cs, 0.0f);
        check(nearf(c0.pos.x, 0) && nearf(c0.pos.y, 0) && nearf(c0.pos.z, 0), "camera eval hits key 0 pos");
        check(nearf(c0.fov, 60.0f), "camera eval hits key 0 fov");
        check(nearf(c0.yaw, 0.0f) && nearf(c0.pitch, 0.0f), "yaw/pitch derived for +X look");
        CamPose cMid = evalCamera(cs, 2.5f);
        check(nearf(cMid.fov, 50.0f, 0.01f), "fov lerps at segment midpoint");
        check(cMid.pos.y > 0.0f && cMid.pos.y < 5.0f, "pos interpolates inside segment");
        CamPose c5 = evalCamera(cs, 5.0f);
        check(nearf(c5.pos.y, 5.0f, 0.01f) && nearf(c5.fov, 40.0f), "camera eval hits key 1");
        // shake: inactive at 5.5, active at 6.5, gone at 7.5
        CamPose a = evalCamera(cs, 5.5f), b = evalCamera(cs, 6.51f), c = evalCamera(cs, 7.5f);
        auto camNoShake = [&](float t) {
            Cutscene tmp = cs; tmp.shakes.clear(); return evalCamera(tmp, t);
        };
        auto dev = [&](const CamPose& p, float t) {
            CamPose q = camNoShake(t);
            return std::fabs(p.pos.x - q.pos.x) + std::fabs(p.pos.y - q.pos.y) + std::fabs(p.pos.z - q.pos.z);
        };
        check(dev(a, 5.5f) < 1e-4f, "no shake outside burst window");
        check(dev(b, 6.51f) > 1e-3f, "shake active inside burst window");
        check(dev(c, 7.5f) < 1e-4f, "shake decays to zero after burst");
        CamPose b2 = evalCamera(cs, 6.51f);
        check(nearf(b.pos.x, b2.pos.x, 1e-6f) && nearf(b.pos.y, b2.pos.y, 1e-6f), "shake is deterministic");
    }

    // ---- 3b) Hard cuts: interpolation never crosses a "cut" key ----
    {
        Cutscene cc = cs;
        cc.camKeys.clear();
        CameraKey a0; a0.t = 0;  a0.pos = {0, 0, 0};   a0.look = {10, 0, 0};   a0.fov = 60;
        CameraKey a1; a1.t = 5;  a1.pos = {0, 0, -10}; a1.look = {10, 0, -10}; a1.fov = 60;
        CameraKey b0; b0.t = 5;  b0.pos = {100, 0, 0}; b0.look = {100, 0, 10}; b0.fov = 35; b0.cut = true;
        CameraKey b1; b1.t = 10; b1.pos = {100, 5, 0}; b1.look = {100, 5, 10}; b1.fov = 35;
        cc.camKeys = { a0, a1, b0, b1 };
        CamPose pre  = evalCamera(cc, 4.99f);
        CamPose post = evalCamera(cc, 5.0f);
        check(pre.pos.x < 1.0f && nearf(pre.fov, 60.0f), "pre-cut samples stay in shot A");
        check(nearf(post.pos.x, 100.0f) && nearf(post.fov, 35.0f), "cut key jumps cleanly into shot B");
        CamPose mid = evalCamera(cc, 7.5f);
        check(nearf(mid.pos.x, 100.0f, 0.01f) && mid.pos.y > 0.0f && mid.pos.y < 5.0f,
              "shot-B spline ignores shot-A neighbors across the cut");
    }

    // ---- 3c) DUTCH ANGLE / roll: default is level + additive; camBasis matches the
    //          Euler path at roll==0; roll eases between keys exactly like fov ----
    {
        // (a) Every legacy key defaults roll=0 -> pose.roll==0 -> level basis.
        CamPose lvl = evalCamera(cs, 2.5f);
        check(nearf(lvl.roll, 0.0f), "roll defaults to 0 (legacy cutscenes stay level)");
        float f0[3], u0[3];
        camBasis(lvl, f0, u0);
        // fwd must equal the device-convention forward derived from yaw/pitch...
        const float cp = std::cos(lvl.pitch), sp = std::sin(lvl.pitch);
        const float cy = std::cos(lvl.yaw),   sy = std::sin(lvl.yaw);
        check(nearf(f0[0], cp * cy, 1e-5f) && nearf(f0[1], sp, 1e-5f) && nearf(f0[2], cp * sy, 1e-5f),
              "camBasis fwd matches yaw/pitch (Euler path)");
        // ...and at roll==0 up must be world-up projected onto the view plane: since
        // this shot looks level (pitch~0), up ~ (0,1,0) and up.y is the max component.
        check(u0[1] > 0.9f && std::fabs(u0[0]) < 0.1f,
              "camBasis up ~ world-up-projected when roll==0 (pixel-identical to setCamera)");

        // (b) Roll interpolates between keys like fov (single-shot span, no cuts).
        Cutscene rc = cs;
        rc.camKeys.clear();
        CameraKey r0; r0.t = 0;  r0.pos = {0,0,0}; r0.look = {10,0,0}; r0.fov = 60; r0.roll = 0.0f;
        CameraKey r1; r1.t = 10; r1.pos = {0,0,0}; r1.look = {10,0,0}; r1.fov = 60; r1.roll = 10.0f;
        rc.camKeys = { r0, r1 };
        const float d2r = kPi / 180.0f;
        check(nearf(evalCamera(rc, 0.0f).roll, 0.0f), "roll hits key 0 (0 deg)");
        check(nearf(evalCamera(rc, 10.0f).roll, 10.0f * d2r, 1e-4f), "roll hits key 1 (10 deg)");
        check(nearf(evalCamera(rc, 5.0f).roll, 5.0f * d2r, 1e-4f), "roll lerps to 5 deg at midpoint");
        // A non-zero roll must actually bank `up` off world-up (into the +right dir).
        float fR[3], uR[3];
        camBasis(evalCamera(rc, 10.0f), fR, uR);
        check(std::fabs(uR[2]) > 0.1f, "roll banks the up vector off world-up (dutch tilt is real)");
    }

    // ---- 4) Actor eval: visibility window + key interpolation + matrix ----
    {
        const Actor& a = cs.actors[0];
        check(!evalActor(cs, a, 0.5f).visible, "actor hidden before showAt");
        check(evalActor(cs, a, 5.0f).visible,  "actor visible inside window");
        check(!evalActor(cs, a, 9.5f).visible, "actor hidden after hideAt");
        ActorPose p0 = evalActor(cs, a, 0.0f), p1 = evalActor(cs, a, 10.0f), pm = evalActor(cs, a, 5.0f);
        check(nearf(p0.pos.z, -10) && nearf(p1.pos.z, 10), "actor pos hits end keys");
        check(nearf(pm.rotDeg.x, 45.0f, 0.1f) && nearf(pm.scale, 1.5f, 0.01f), "actor rot/scale lerp");
        float m[16];
        actorMatrix(a, pm, 1.0f, m);
        check(nearf(m[12], pm.pos.x) && nearf(m[13], pm.pos.y) && nearf(m[14], pm.pos.z) && nearf(m[15], 1.0f),
              "actorMatrix carries translation");
        ActorPose ident{}; ident.scale = 2.0f; ident.visible = true;
        Actor plain; plain.stretch = {1, 1, 1};
        actorMatrix(plain, ident, 3.0f, m);
        check(nearf(m[0], 6.0f) && nearf(m[5], 6.0f) && nearf(m[10], 6.0f), "actorMatrix scale*norm compose");

        // ---- BLOB->DETAILED emissive ramp (Phase 5): emissiveFrom -> emissive over
        //      [emissiveRampAt, +Dur], holding endpoints outside the window; no ramp
        //      => static emissive at all times. ----
        Actor staticEm; staticEm.keys.push_back({});
        staticEm.emissive[0] = 0.5f; staticEm.emissive[3] = 1.0f;   // no ramp set
        check(nearf(evalActor(cs, staticEm, 0.0f).emissive[0], 0.5f) &&
              nearf(evalActor(cs, staticEm, 99.0f).emissive[0], 0.5f),
              "no-ramp actor keeps static emissive at all t");
        Actor ramp; ramp.keys.push_back({});
        ramp.emissiveFrom[0] = 0.0f;  ramp.emissiveFrom[3] = 1.0f;
        ramp.emissive[0]     = 1.0f;  ramp.emissive[3]     = 1.0f;
        ramp.emissiveRampAt  = 10.0f; ramp.emissiveRampDur = 10.0f;
        check(nearf(evalActor(cs, ramp, 5.0f).emissive[0], 0.0f),  "emissive ramp holds 'from' before window");
        check(nearf(evalActor(cs, ramp, 15.0f).emissive[0], 0.5f), "emissive ramp lerps mid-window");
        check(nearf(evalActor(cs, ramp, 25.0f).emissive[0], 1.0f), "emissive ramp holds 'to' after window");
    }

    // ---- 5) Overlay eval: fades, letterbox, title alpha ----
    {
        float f[4];
        evalFade(cs, 0.0f, f);  check(nearf(f[3], 1.0f), "fade-in starts at alpha 1");
        evalFade(cs, 0.5f, f);  check(nearf(f[3], 0.5f, 0.01f), "fade-in ramps");
        evalFade(cs, 3.0f, f);  check(nearf(f[3], 0.0f), "fade holds target after ramp");
        evalFade(cs, 7.5f, f);  check(nearf(f[3], 1.0f), "later fade takes over (smash to black)");
        check(nearf(evalLetterbox(cs, 0.5f), 0.05f, 0.005f), "letterbox eases in");
        check(nearf(evalLetterbox(cs, 3.0f), 0.1f), "letterbox holds frac");
        check(nearf(evalLetterbox(cs, 8.5f), 0.0f), "letterbox eases out");
        const TitleCard& tc = cs.titles[0];
        check(nearf(evalTitleAlpha(tc, 7.9f), 0.0f), "title inactive before t");
        check(nearf(evalTitleAlpha(tc, 8.25f), 0.5f, 0.01f), "title fades in");
        check(nearf(evalTitleAlpha(tc, 9.0f), 1.0f), "title holds");
        check(nearf(evalTitleAlpha(tc, 9.8f), 0.4f, 0.01f), "title fades out");
    }

    // ---- 6) Player: exactly-once firing in order ----
    {
        CutscenePlayer pl(cs);
        std::vector<std::string> fired;
        std::vector<std::string> sounds;
        pl.onEvent([&](const Event& e, bool seeked) { if (!seeked) fired.push_back(e.name); });
        pl.onAudio([&](const AudioCue& c) { sounds.push_back(c.sound); });
        for (int i = 0; i < 500 && !pl.done(); ++i) pl.tick(0.033f);
        check(pl.done(), "player reaches done");
        check(fired.size() == 3 && fired[0] == "midpoint" && fired[1] == "fx.impact:ship" &&
              fired[2] == "intro_complete", "events fire exactly once, in order");
        check(sounds.size() == 2 && sounds[0] == "alarm" && sounds[1] == "boom",
              "audio cues fire exactly once, in order");
        check(pl.firedEventCount() == 3 && pl.firedAudioCount() == 2, "fired counters agree");
        pl.tick(1.0f);
        check(pl.firedEventCount() == 3, "done is idempotent");
    }

    // ---- 7) Seek semantics (the --cuetime scrub) ----
    {
        CutscenePlayer pl(cs);
        int seekEvents = 0, playEvents = 0, audioFired = 0;
        pl.onEvent([&](const Event&, bool seeked) { seeked ? ++seekEvents : ++playEvents; });
        pl.onAudio([&](const AudioCue&) { ++audioFired; });
        pl.seek(6.6f);
        check(nearf(pl.time(), 6.6f), "seek moves the playhead");
        check(seekEvents == 2 && playEvents == 0, "seek delivers jumped-over events as seeked");
        check(audioFired == 0 && pl.firedAudioCount() == 1, "seek consumes audio silently");
        for (int i = 0; i < 400 && !pl.done(); ++i) pl.tick(0.05f);
        check(playEvents == 1 && audioFired == 1, "post-seek playback fires only later cues");
        // backward seek rewinds fired-state
        pl.seek(0.0f);
        check(pl.firedEventCount() == 0 && pl.firedAudioCount() == 0, "backward seek rewinds fired-state");
        for (int i = 0; i < 400 && !pl.done(); ++i) pl.tick(0.05f);
        check(pl.firedEventCount() == 3, "replay after rewind fires again");
    }

    // ---- 8) Skip semantics ----
    {
        CutscenePlayer pl(cs);
        std::vector<std::string> fired;
        int audioFired = 0;
        pl.onEvent([&](const Event& e, bool) { fired.push_back(e.name); });
        pl.onAudio([&](const AudioCue&) { ++audioFired; });
        pl.tick(1.0f);
        pl.skip();
        check(pl.skipped(), "skip latches");
        check(nearf(pl.time(), 8.0f), "skip jumps to skipTo (title tail still plays)");
        check(fired.empty(), "non-endState events dropped by skip");
        check(audioFired == 0, "audio not fired by skip");
        pl.skip();   // second skip -> duration
        check(pl.done(), "second skip finishes");
        check(fired.size() == 1 && fired[0] == "intro_complete", "endState event STILL fires on skip");
        // non-skippable cutscene ignores skip
        Cutscene ns = cs; ns.skippable = false;
        CutscenePlayer pl2(ns);
        pl2.tick(1.0f); pl2.skip();
        check(!pl2.skipped() && nearf(pl2.time(), 1.0f), "non-skippable ignores skip");
    }

    // ---- 9) StoryFlags round-trip ----
    {
        StoryFlags fl;
        check(!fl.has(kFlagIntroComplete), "flags start empty");
        fl.set(kFlagIntroComplete);
        fl.set("beta_flag");
        fl.set(kFlagIntroComplete);   // dup
        check(fl.has(kFlagIntroComplete) && fl.count() == 2, "set + dedupe");
        const std::string tmp = (std::filesystem::temp_directory_path() / "x3_cutscene_flags_test.txt").string();
        check(fl.save(tmp), "flags save");
        StoryFlags fl2;
        check(fl2.load(tmp), "flags load");
        check(fl2.has(kFlagIntroComplete) && fl2.has("beta_flag") && fl2.count() == 2, "flags round-trip");
        fl2.clear("beta_flag");
        check(!fl2.has("beta_flag") && fl2.count() == 1, "flag clear");
        std::error_code ec; std::filesystem::remove(tmp, ec);
        StoryFlags fl3;
        check(!fl3.load(tmp) && fl3.count() == 0, "missing flags file == empty set");
    }

    // ---- 10) The real shipped cold open parses + carries the canon beats ----
    {
        const std::string path = x3::game::assetRoot() + "/cutscenes/cold_open.cutscene.json";
        Cutscene co;
        std::vector<std::string> e2;
        const bool ok = loadCutsceneFile(path, co, e2);
        for (const auto& e : e2) x3::logError("[cutscene-test] cold_open: " + e);
        check(ok, "cold_open.cutscene.json parses + validates");
        if (ok) {
            check(co.duration >= 45.0f && co.duration <= 90.0f, "cold open duration in 45..90 s");
            check(co.camKeys.size() >= 6, "cold open has a real camera move (>= 6 keys)");
            check(co.actors.size() >= 2, "cold open has Jake's ship + the capital ship");
            bool hasTitle = false, hasSixMonths = false, hasComplete = false;
            for (const auto& t : co.titles) {
                if (t.text == "ESCAPE FROM LAB ZERO" && t.font == "title") hasTitle = true;
                if (t.text == "SIX MONTHS LATER") hasSixMonths = true;
            }
            for (const auto& e : co.events)
                if (e.name == kFlagIntroComplete && e.endState) hasComplete = true;
            check(hasTitle, "cold open carries the ESCAPE FROM LAB ZERO title card (Orbitron)");
            check(hasSixMonths, "cold open carries the SIX MONTHS LATER card");
            check(hasComplete, "cold open fires intro_complete as endState");
            check(co.skippable, "cold open is skippable");
            // Accelerated full run through the player: every event fires, no dupes.
            CutscenePlayer pl(co);
            uint32_t evs = 0;
            pl.onEvent([&](const Event&, bool) { ++evs; });
            for (int i = 0; i < 100000 && !pl.done(); ++i) pl.tick(0.05f);
            check(pl.done(), "cold open accelerated run reaches done");
            check(evs == (uint32_t)co.events.size(), "cold open fires every event exactly once");
            // Camera eval is finite at dense samples (no NaN/Inf out of the spline).
            bool finite = true;
            for (float t = 0.0f; t <= co.duration; t += 0.25f) {
                CamPose c = evalCamera(co, t);
                if (!std::isfinite(c.pos.x + c.pos.y + c.pos.z + c.yaw + c.pitch + c.fov)) finite = false;
            }
            check(finite, "cold open camera eval finite across the whole timeline");
        }
    }

    // ---- 11) GLB POSITION extent probe (ship size casting) ----
    {
        const std::string glb = x3::game::riggedGlbRoot() + "/JakeFighterShip.glb";
        float mn[3], mx[3];
        const bool ok = glbPositionExtent(glb, mn, mx);
        check(ok, "glbPositionExtent reads JakeFighterShip.glb");
        if (ok) {
            const float ex = mx[0] - mn[0], ey = mx[1] - mn[1], ez = mx[2] - mn[2];
            check(ex > 0.0f && ey > 0.0f && ez > 0.0f, "ship extent is positive in all axes");
            x3::logInfo("[cutscene-test] JakeFighterShip extent: " + std::to_string(ex) + " x " +
                        std::to_string(ey) + " x " + std::to_string(ez));
        }
        float dmn[3], dmx[3];
        check(!glbPositionExtent("definitely_missing.glb", dmn, dmx), "extent probe fails gracefully");
    }

    x3::logInfo("[cutscene-test] " + std::to_string(g_pass) + "/" + std::to_string(g_total) + " passed");
    return g_pass == g_total;
}

} // namespace x3::cut
