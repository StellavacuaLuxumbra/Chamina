#ifdef CHAMINA_IMPLEMENTATION
#include "json.hpp"
#include "earcut.hpp"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <chrono>
static bool TRACE = false;
#include <tuple>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
// argv arrives in the ANSI codepage via cmd.exe; scene paths and text flags
// are UTF-8 inside the engine, so re-decode once at startup.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

using json = nlohmann::json;
using namespace std;

// ---------------------------------------------------------------------------
// math (port of src/math.rs)
// ---------------------------------------------------------------------------

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    float dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(Vec3 o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float length() const { return sqrtf(dot(*this)); }
    Vec3 normalize() const {
        float l = length();
        return l > 0 ? Vec3{x / l, y / l, z / l} : Vec3{0, 0, 0};
    }
};

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    static Quat identity() { return {}; }
    static Quat axis_angle(Vec3 axis, float angle) {
        float l = axis.length();
        if (l <= 0) return identity();
        Vec3 a{axis.x / l, axis.y / l, axis.z / l};
        float h = angle * 0.5f, s = sinf(h);
        return {a.x * s, a.y * s, a.z * s, cosf(h)};
    }
    // Hamilton product: (a*b) applies b first.
    Quat operator*(Quat o) const {
        return {
            w * o.x + x * o.w + y * o.z - z * o.y,
            w * o.y - x * o.z + y * o.w + z * o.x,
            w * o.z + x * o.y - y * o.x + z * o.w,
            w * o.w - x * o.x - y * o.y - z * o.z};
    }
    Vec3 mul_vec3(Vec3 v) const {
        // v' = q * v * q^-1 (expanded)
        Vec3 q{x, y, z};
        Vec3 t = q.cross(v) * 2.0f;
        return v + t * w + q.cross(t);
    }
};

// Easing (port of src/animation.rs Ease).
static float ease_apply(const string& name, float x) {
    x = clamp(x, 0.0f, 1.0f);
    if (name == "linear") return x;
    if (name == "ease_in_quad") return x * x;
    if (name == "ease_out_quad") return 1 - (1 - x) * (1 - x);
    if (name == "ease_in_out_cubic")
        return x < 0.5f ? 4 * x * x * x : 1 - powf(-2 * x + 2, 3) / 2;
    if (name == "back_out") {
        const float C1 = 1.70158f, C3 = C1 + 1;
        float u = x - 1;
        return 1 + C3 * u * u * u + C1 * u * u;
    }
    return x * x * (3 - 2 * x); // smooth (default)
}
static float smoothstep_s(float x) {
    x = clamp(x, 0.0f, 1.0f);
    return x * x * (3 - 2 * x);
}

// ---------------------------------------------------------------------------
// animation (port of src/animation.rs)
// ---------------------------------------------------------------------------

static const float HARMONIC[3] = {1.618034f, 1.4142135f, 1.7320508f};
static const float AXIS_PHASE[3] = {0.0f, 2.0943951f, 4.1887902f};
static const float CHAR_PHASE = 2.3999632f;

static Vec3 jitter_noise(float t, float w, float p) {
    Vec3 o;
    float arr[3];
    for (int i = 0; i < 3; i++) {
        float ph = p + AXIS_PHASE[i];
        arr[i] = 0.5f * sinf(w * t + ph) + 0.5f * sinf(w * HARMONIC[i] * t + ph * 1.7f);
    }
    return Vec3{arr[0], arr[1], arr[2]};
}

static float hit_weight(float t, float at, float dur) {
    float u = (t - at) / max(dur, 1e-6f);
    if (u < 0 || u > 1) return 0;
    return (1 - u) * (1 - u);
}

struct AnimState {
    Vec3 offset;
    Quat rotation = Quat::identity();
    float alpha = 1, scale = 1;
};

static float num_or(const json& j, const char* key, float d) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return d;
    return it->get<float>();
}

static float char_delay(const json* char_times, size_t idx, float start_at, float stagger) {
    if (char_times && idx < char_times->size()) return (*char_times)[idx].get<float>();
    return start_at + (float)idx * stagger;
}

// Port of eval_anim_timed. `size` is the text's resolved size (em units).
static AnimState eval_anim(const json& a, float t, size_t idx, float size,
                           const json* char_times) {
    AnimState st;
    const string type = a.at("type").get<string>();
    if (type == "fall") {
        float dur = num_or(a, "duration", 0.7f);
        float delay = char_delay(char_times, idx, num_or(a, "start_at", 0), num_or(a, "stagger", 0.04f));
        float u = clamp((t - delay) / max(dur, 1e-6f), 0.0f, 1.0f);
        float height = num_or(a, "height", 1.0f);
        float gravity = num_or(a, "gravity", -3.0f);
        float tt = dur * u;
        float v0 = -(height + 0.5f * gravity * dur * dur) / dur;
        st.offset.y += (height + v0 * tt + 0.5f * gravity * tt * tt) * size;
        if (t > delay) {
            float drift_t = min(t - delay, dur);
            st.offset.x += num_or(a, "drift", 0) * drift_t * size;
        }
    } else if (type == "fade") {
        float dur = num_or(a, "duration", 0.3f);
        float delay = char_delay(char_times, idx, num_or(a, "start_at", 0), num_or(a, "stagger", 0.04f));
        float f = 1.0f;
        if (t < delay) f = 0;
        else if (t < delay + dur) f = (t - delay) / max(dur, 1e-6f);
        auto fo_it = a.find("fade_out_at");
        if (fo_it != a.end() && fo_it->is_number()) {
            float fo = fo_it->get<float>();
            float fade_out = num_or(a, "fade_out", 0.45f);
            if (t > fo) {
                f *= clamp(1 - (t - fo) / max(fade_out, 1e-6f), 0.0f, 1.0f);
                if (t >= fo + fade_out) f = 0;
            }
        }
        st.alpha = clamp(f, 0.0f, 1.0f);
    } else if (type == "spin") {
        float sd = num_or(a, "start_delay", 0);
        if (t >= sd)
            st.rotation = Quat::axis_angle(
                Vec3{num_or(a, "ax", 0), num_or(a, "ay", 0), num_or(a, "az", 1)},
                num_or(a, "speed", 0) * (t - sd));
    } else if (type == "slide") {
        float dur = num_or(a, "duration", 0.7f);
        float delay = char_delay(char_times, idx, num_or(a, "start_at", 0), num_or(a, "stagger", 0.04f));
        float u = (t - delay) / max(dur, 1e-6f);
        float k = (1 - ease_apply(a.value("ease", "smooth"), u)) * size;
        const json& d = a.at("delta");
        st.offset += Vec3{d[0].get<float>(), d[1].get<float>(), d[2].get<float>()} * k;
    } else if (type == "jitter") {
        float delay = char_delay(char_times, idx, num_or(a, "start_at", 0), num_or(a, "stagger", 0));
        if (t >= delay) {
            float p = num_or(a, "phase", 0) + (float)idx * CHAR_PHASE;
            Vec3 n = jitter_noise(t, (float)M_PI * 2 * max(num_or(a, "frequency", 5), 0.0f), p);
            float env;
            if (a.value("hit", false))
                env = hit_weight(t, delay, num_or(a, "hit_duration", 0.55f));
            else
                env = smoothstep_s((t - delay) / max(num_or(a, "ramp", 0.3f), 1e-6f));
            const json& amp = a.at("amplitude");
            float k = size * env;
            st.offset += Vec3{amp[0].get<float>() * n.x, amp[1].get<float>() * n.y,
                              amp.size() > 2 ? amp[2].get<float>() * n.z : 0} * k;
            float rot = num_or(a, "rotation", 0);
            if (rot != 0) {
                float r = rot * env;
                st.rotation = Quat::axis_angle(Vec3{0, 0, 1}, n.x * r) *
                              Quat::axis_angle(Vec3{0, 1, 0}, n.y * r * 0.5f);
            }
        }
    } else if (type == "pop") {
        float dur = num_or(a, "duration", 0.5f);
        float delay = char_delay(char_times, idx, num_or(a, "start_at", 0), num_or(a, "stagger", 0.04f));
        float u = clamp((t - delay) / max(dur, 1e-6f), 0.0f, 1.0f);
        float from = num_or(a, "from", 0.0f);
        st.scale = from + (1 - from) * ease_apply(a.value("ease", "back_out"), u);
    } else if (type == "wave") {
        float at = num_or(a, "start_at", 0);
        if (t >= at) {
            float env;
            if (a.value("hit", false))
                env = hit_weight(t, at, num_or(a, "hit_duration", 2.0f));
            else
                env = smoothstep_s((t - at) / max(num_or(a, "ramp", 0.4f), 1e-6f));
            float phase = (float)M_PI * 2 * num_or(a, "frequency", 1.6f) * (t - at) -
                          (float)idx * num_or(a, "spacing", 0.55f);
            st.offset.y += num_or(a, "amplitude", 0.2f) * env * sinf(phase) * size;
        }
    }
    return st;
}

static AnimState eval_all(const json& anims, float t, size_t idx, float size,
                          const json* char_times) {
    AnimState out;
    for (const auto& a : anims) {
        AnimState st = eval_anim(a, t, idx, size, char_times);
        out.offset = out.offset + st.offset;
        out.rotation = out.rotation * st.rotation;
        out.alpha *= st.alpha;
        out.scale *= st.scale;
    }
    return out;
}

// ---------------------------------------------------------------------------
// camera (port of src/native/mod.rs eval_orbit / update_camera)
// ---------------------------------------------------------------------------

struct Rig {
    float radius, elevation, yaw, roll;
    Vec3 target;
};

static Rig eval_orbit(const json& o, float t) {
    Rig r;
    r.radius = o.at("radius").get<float>();
    r.elevation = o.at("elevation").get<float>();
    r.target = Vec3{o.at("target")[0].get<float>(), o.at("target")[1].get<float>(),
                    o.at("target")[2].get<float>()};
    r.yaw = o.value("start_yaw", 0.0f) + o.value("speed", 0.0f) * t;
    r.roll = 0;
    // Stops carry yaw/roll as OFFSETS on top of the orbit (js: yawOff/rollOff)
    // — they must never replace the orbit's own yaw.
    float yaw_off = 0, roll_off = 0;
    if (auto m = o.find("movement"); m != o.end() && m->is_object()) {
        float k = ease_apply(m->value("ease", "ease_in_out_cubic"),
                             (t - m->value("at", 0.0f)) / max(m->value("duration", 1.0f), 1e-6f));
        auto re = m->find("radius_end");
        if (re != m->end() && re->is_number())
            r.radius += (re->get<float>() - r.radius) * k;
        auto ee = m->find("elevation_end");
        if (ee != m->end() && ee->is_number())
            r.elevation += (ee->get<float>() - r.elevation) * k;
        auto te = m->find("target_end");
        if (te != m->end() && te->is_array())
            r.target = r.target + (Vec3{(*te)[0].get<float>(), (*te)[1].get<float>(),
                                         (*te)[2].get<float>()} -
                                   r.target) * k;
    }
    if (auto stops = o.find("stops"); stops != o.end() && stops->is_array()) {
        for (const auto& s : *stops) {
            float at = s.value("at", 0.0f);
            if (t < at) break;
            float k = ease_apply(s.value("ease", "ease_in_out_cubic"),
                                 (t - at) / max(s.value("duration", 1.0f), 1e-6f));
            auto rad = s.find("radius");
            float tr = rad != s.end() && rad->is_number() ? rad->get<float>() : r.radius;
            auto elv = s.find("elevation");
            float te2 = elv != s.end() && elv->is_number() ? elv->get<float>() : r.elevation;
            Vec3 tt = r.target;
            auto tg = s.find("target");
            if (tg != s.end() && tg->is_array())
                tt = Vec3{(*tg)[0].get<float>(), (*tg)[1].get<float>(), (*tg)[2].get<float>()};
            auto yw = s.find("yaw");
            float tyaw = yw != s.end() && yw->is_number() ? yw->get<float>() : yaw_off;
            auto rl = s.find("roll");
            float troll = rl != s.end() && rl->is_number() ? rl->get<float>() : roll_off;
            r.radius += (tr - r.radius) * k;
            r.elevation += (te2 - r.elevation) * k;
            r.target = r.target + (tt - r.target) * k;
            yaw_off += (tyaw - yaw_off) * k;
            roll_off += (troll - roll_off) * k;
        }
    }
    r.yaw += yaw_off;
    r.roll = roll_off;
    return r;
}

struct Cam {
    Vec3 eye, target, fwd, right, up;
    float roll, tan_h, aspect, near_z;
};

static float hit_envelope(const vector<float>& cues, float t, float dur) {
    float best = 0;
    for (float c : cues) {
        float u = (t - c) / max(dur, 1e-6f);
        if (u >= 0 && u <= 1) best = max(best, (1 - u) * (1 - u));
    }
    return best;
}

static Cam update_camera(const json& o, float t, const vector<float>& cues,
                         const json* fx, float energy, float fov, float aspect,
                         float near_z) {
    Rig rig = eval_orbit(o, t);
    float hit = fx ? hit_envelope(cues, t, fx->value("hit_duration", 0.35f)) : 0.0f;
    float ce = cosf(rig.elevation), se = sinf(rig.elevation);
    Vec3 eye = rig.target + Vec3{rig.radius * ce * cosf(rig.yaw), rig.radius * se,
                                 rig.radius * ce * sinf(rig.yaw)};
    Vec3 target = rig.target;
    float roll = rig.roll;
    if (auto sh = o.find("shake"); sh != o.end() && sh->is_object()) {
        // shakeWeight: window envelope × (hit ? cue hit : 1)
        float at = sh->value("at", 0.0f), ramp = max(sh->value("ramp", 0.6f), 1e-6f);
        float env = 0;
        if (t >= at) {
            auto until = sh->find("until");
            float out_w = 1.0f;
            bool dead = false;
            if (until != sh->end() && until->is_number()) {
                if (t > until->get<float>()) dead = true;
                else out_w = clamp((until->get<float>() - t) / ramp, 0.0f, 1.0f);
            }
            if (!dead) {
                env = clamp((t - at) / ramp, 0.0f, 1.0f);
                env = min(env, out_w);
                if (env > 0 && sh->value("hit", false))
                    env *= hit_envelope(cues, t, sh->value("hit_duration", 0.55f));
            }
        }
        if (env > 0) {
            float amp = sh->value("amplitude", 0.0f) * env;
            Vec3 n = jitter_noise(t, (float)M_PI * 2 * max(sh->value("frequency", 3.5f), 0.0f), 0.7f);
            Vec3 dir = (eye - target).normalize();
            Vec3 right{dir.z, 0, -dir.x};
            right = right.normalize();
            Vec3 up = dir.cross(right);
            Vec3 off = right * (n.x * amp) + up * (n.y * amp) + dir * (n.z * amp * 0.5f);
            eye = eye + off;
            target = target + off;
            roll += n.y * sh->value("roll", 0.008f) * env;
        }
    }
    if (hit > 0 && fx && fx->value("punch", 0.0f) > 0) {
        Vec3 dir = target - eye;
        float step = fx->value("punch", 0.0f) * hit * (0.55f + 0.45f * energy);
        eye = eye + dir.normalize() * step;
    }
    Vec3 fwd = (target - eye).normalize();
    Vec3 right = Vec3{-fwd.z, 0, fwd.x}.normalize();
    Vec3 up = right.cross(fwd);
    // camera.rotateZ: the bank is part of the view basis.
    float cr = cosf(roll), sr = sinf(roll);
    Vec3 right2 = right * cr + up * sr;
    Vec3 up2 = up * cr - right * sr;
    return Cam{eye, target, fwd, right2, up2, roll, tanf(fov * 0.5f), aspect, near_z};
}

// ---------------------------------------------------------------------------
// backgrounds (port of js/anim.mjs::evalBackgrounds + skydome)
// ---------------------------------------------------------------------------

static void hex_linear(const string& hex, float out[3]) {
    auto lin = [](float c) {
        return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    };
    unsigned n = 0xffffff;
    if (hex.size() >= 7 && hex[0] == '#') n = strtoul(hex.substr(1, 6).c_str(), nullptr, 16);
    out[0] = lin(((n >> 16) & 255) / 255.0f);
    out[1] = lin(((n >> 8) & 255) / 255.0f);
    out[2] = lin((n & 255) / 255.0f);
}

static void hex_srgb(const string& hex, float out[3]) {
    unsigned n = 0xffffff;
    if (hex.size() >= 7 && hex[0] == '#') n = strtoul(hex.substr(1, 6).c_str(), nullptr, 16);
    out[0] = ((n >> 16) & 255) / 255.0f;
    out[1] = ((n >> 8) & 255) / 255.0f;
    out[2] = (n & 255) / 255.0f;
}

// Returns sRGB-mixed {top[3], bottom[3], accent[3], nebula, stars, drift}.
static bool eval_backgrounds(const json& keys, float t, float out[12]) {
    if (!keys.is_array() || keys.empty()) return false;
    size_t idx = 0;
    for (size_t k = 0; k < keys.size(); k++)
        if (keys[k].value("at", 0.0f) <= t) idx = k;
    const json& cur = keys[idx];
    float top[3], bottom[3], accent[3];
    hex_srgb(cur.at("top").get<string>(), top);
    hex_srgb(cur.at("bottom").get<string>(), bottom);
    hex_srgb(cur.at("accent").get<string>(), accent);
    float nebula = cur.value("nebula", 0.5f), stars = cur.value("stars", 0.6f),
          drift = cur.value("drift", 1.0f);
    if (idx > 0) {
        float fade = max(cur.value("fade", 0.0f), 0.0f);
        if (fade > 0) {
            float k = smoothstep_s((t - cur.value("at", 0.0f)) / fade);
            const json& prev = keys[idx - 1];
            float p[3][3];
            hex_srgb(prev.at("top").get<string>(), p[0]);
            hex_srgb(prev.at("bottom").get<string>(), p[1]);
            hex_srgb(prev.at("accent").get<string>(), p[2]);
            float* c[3] = {top, bottom, accent};
            for (int i = 0; i < 3; i++)
                for (int j = 0; j < 3; j++) c[i][j] = p[i][j] + (c[i][j] - p[i][j]) * k;
            nebula = prev.value("nebula", 0.5f) + (nebula - prev.value("nebula", 0.5f)) * k;
            stars = prev.value("stars", 0.6f) + (stars - prev.value("stars", 0.6f)) * k;
            drift = prev.value("drift", 1.0f) + (drift - prev.value("drift", 1.0f)) * k;
        }
    }
    memcpy(out, top, 12);
    memcpy(out + 3, bottom, 12);
    memcpy(out + 6, accent, 12);
    out[9] = nebula;
    out[10] = stars;
    out[11] = drift;
    return true;
}

// ---------------------------------------------------------------------------
// font engine (stb_truetype outlines → flatten → earcut → extrude)
// ---------------------------------------------------------------------------

struct FontEngine {
    vector<unsigned char> data;
    stbtt_fontinfo info;
    float upem = 1;
    float ascender = 0, descender = 0; // em units
    bool ok = false;

    bool open(const string& path) {
        ifstream f(path, ios::binary);
        if (!f) return false;
        data = vector<unsigned char>((istreambuf_iterator<char>(f)),
                                     istreambuf_iterator<char>());
        int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);
        if (offset < 0 || !stbtt_InitFont(&info, data.data(), offset)) return false;
        upem = stbtt_ScaleForMappingEmToPixels(&info, 1.0f); // = 1/unitsPerEm
        int asc, desc, lg;
        stbtt_GetFontVMetrics(&info, &asc, &desc, &lg);
        ascender = asc * upem;
        descender = desc * upem;
        ok = true;
        return true;
    }

    // Contour = polyline of on-curve points (quads already flattened).
    struct Contour {
        vector<Vec3> pts; // z unused, y-up EM space
    };

    // Port of the glyph_contours → flatten pipeline. stbtt vertex semantics:
    // vmove = contour start; vline = line TO (x,y); vcurve = quadratic TO
    // (x,y) with control (cx,cy) — and when the NEXT vertex is also a curve,
    // this endpoint is an implied-off-curve point, so the segment ends at the
    // midpoint of the two controls instead. `segments` subdivisions per quad.
    bool glyph_polys(int glyph, vector<Contour>& polys) const {
        stbtt_vertex* verts = nullptr;
        int n = stbtt_GetGlyphShape(const_cast<stbtt_fontinfo*>(&info), glyph, &verts);
        if (n <= 0) return false;
        const int SEG = 10;
        Contour cur;
        Vec3 pen{0, 0, 0};
        auto flush = [&]() {
            if (cur.pts.size() >= 3) polys.push_back(cur);
            cur.pts.clear();
        };
        auto emit = [&](Vec3 p) {
            if (!cur.pts.empty()) {
                Vec3& b = cur.pts.back();
                if (fabsf(b.x - p.x) < 1e-6f && fabsf(b.y - p.y) < 1e-6f) return;
            }
            cur.pts.push_back(p);
        };
        for (int i = 0; i < n; i++) {
            const stbtt_vertex& v = verts[i];
            Vec3 p{v.x * upem, v.y * upem, 0};
            if (v.type == STBTT_vmove) {
                flush();
                pen = p;
                emit(p);
            } else if (v.type == STBTT_vline) {
                emit(p);
                pen = p;
            } else if (v.type == STBTT_vcurve) {
                Vec3 ctrl{v.cx * upem, v.cy * upem, 0};
                Vec3 end = p;
                if (i + 1 < n && verts[i + 1].type == STBTT_vcurve)
                    end = (p + Vec3{verts[i + 1].x * upem, verts[i + 1].y * upem, 0}) * 0.5f;
                for (int s = 1; s <= SEG; s++) {
                    float u = (float)s / SEG;
                    float a = (1 - u) * (1 - u), b = 2 * (1 - u) * u, c2 = u * u;
                    emit(Vec3{pen.x * a + ctrl.x * b + end.x * c2,
                              pen.y * a + ctrl.y * b + end.y * c2, 0});
                }
                pen = end;
            }
        }
        flush();
        stbtt_FreeShape(const_cast<stbtt_fontinfo*>(&info), verts);
        return !polys.empty();
    }

    bool glyph_empty(int glyph) const { return stbtt_IsGlyphEmpty(const_cast<stbtt_fontinfo*>(&info), glyph) == 1; }

    int find_glyph(uint32_t cp) const { return stbtt_FindGlyphIndex(const_cast<stbtt_fontinfo*>(&info), (int)cp); }

    void h_metrics(int glyph, float& advance_em, float& lsb_em) const {
        int a, lsb;
        stbtt_GetGlyphHMetrics(const_cast<stbtt_fontinfo*>(&info), glyph, &a, &lsb);
        advance_em = a * upem;
        lsb_em = lsb * upem;
    }

    uint32_t codepoint(const string& s, size_t i, size_t& next) {
        unsigned char c = s[i];
        uint32_t cp = c;
        if (c < 0x80) next = i + 1;
        else if ((c >> 5) == 0x6) { cp = ((c & 31) << 6) | (s[i + 1] & 63); next = i + 2; }
        else if ((c >> 4) == 0xE) {
            cp = ((c & 15) << 12) | ((s[i + 1] & 63) << 6) | (s[i + 2] & 63);
            next = i + 3;
        } else {
            cp = ((c & 7) << 18) | ((s[i + 1] & 63) << 12) | ((s[i + 2] & 63) << 6) |
                 (s[i + 3] & 63);
            next = i + 4;
        }
        return cp;
    }
};

// Point in polygon (even-odd), for hole nesting.
static bool point_in_poly(const Vec3& p, const vector<Vec3>& poly) {
    bool inside = false;
    size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        if (((poly[i].y > p.y) != (poly[j].y > p.y)) &&
            (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) +
                       poly[i].x))
            inside = !inside;
    }
    return inside;
}

struct Mesh {
    vector<array<float, 3>> verts;
    vector<array<float, 3>> normals;
    vector<uint32_t> indices;
};

// Tessellate + extrude one glyph. Mirrors src/text/mesher.rs::build_char_mesh:
// even-odd fill (earcut with hole groups), caps at z=±h around the bbox
// centre, walls from boundary edges, normals away from the adjacent third
// vertex.
static bool build_glyph_mesh(FontEngine& fe, int glyph, float depth, Mesh& out,
                             float& org_x, float& org_y) {
    vector<FontEngine::Contour> polys;
    if (!fe.glyph_polys(glyph, polys)) return false;

    // bbox centre of the flattened outline (Rust: polyline_bbox centre)
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (auto& c : polys)
        for (auto& p : c.pts) {
            minx = min(minx, p.x);
            maxx = max(maxx, p.x);
            miny = min(miny, p.y);
            maxy = max(maxy, p.y);
        }
    float cx = minx + (maxx - minx) * 0.5f;
    float cy = miny + (maxy - miny) * 0.5f;
    org_x = cx;
    org_y = cy;
    float h = max(depth, 0.001f) * 0.5f;

    // Classify contours even-odd: depth 0 = outer, odd = hole. Each hole is
    // attached to the smallest containing outer.
    size_t nc = polys.size();
    vector<int> cdepth(nc, 0);
    for (size_t i = 0; i < nc; i++) {
        int cnt = 0;
        for (size_t j = 0; j < nc; j++)
            if (j != i && point_in_poly(polys[i].pts[0], polys[j].pts)) cnt++;
        cdepth[i] = cnt;
    }
    // earcut ring groups: outer + its direct holes
    using Point = pair<double, double>;
    using Ring = vector<Point>;
    vector<uint32_t> all_idx;
    float z_front = h, z_back = -h;
    vector<array<float, 3>>& verts = out.verts;
    vector<array<float, 3>>& normals = out.normals;
    vector<bool> used(nc, false);
    for (size_t i = 0; i < nc; i++) {
        if (cdepth[i] % 2 != 0 || used[i]) continue;
        vector<Ring> cut;
        cut.emplace_back();
        for (auto& p : polys[i].pts) cut.back().push_back({p.x - cx, p.y - cy});
        vector<size_t> members{i};
        used[i] = true;
        for (size_t j = 0; j < nc; j++) {
            if (used[j] || cdepth[j] % 2 == 0) continue;
            if (point_in_poly(polys[j].pts[0], polys[i].pts)) {
                // direct hole of this outer (not inside a deeper nested outer)
                bool deeper = false;
                for (size_t k = 0; k < nc; k++)
                    if (k != i && k != j && cdepth[k] % 2 == 0 && cdepth[k] > cdepth[i] &&
                        point_in_poly(polys[j].pts[0], polys[k].pts))
                        deeper = true;
                if (!deeper) {
                    cut.emplace_back();
                    members.push_back(j);
                    used[j] = true;
                }
            }
        }
        for (size_t m = 1; m < members.size(); m++)
            for (auto& p : polys[members[m]].pts)
                cut[m].push_back({p.x - cx, p.y - cy});
        vector<uint32_t> tri = mapbox::earcut<uint32_t>(cut);
        // earcut indices span [0, total pts); remap per ring
        uint32_t base = (uint32_t)verts.size();
        for (size_t m = 0; m < members.size(); m++)
            for (auto& p : polys[members[m]].pts) {
                verts.push_back({p.x - cx, p.y - cy, z_front});
                normals.push_back({0, 0, 1});
            }
        for (size_t k = 0; k < tri.size(); k += 3) {
            all_idx.push_back(base + tri[k]);
            all_idx.push_back(base + tri[k + 1]);
            all_idx.push_back(base + tri[k + 2]);
        }
    }
    if (all_idx.empty()) return false;

    uint32_t nverts = (uint32_t)verts.size();
    // Back cap
    for (size_t i = 0; i < nverts; i++) {
        array<float, 3> v = verts[i];
        v[2] = z_back;
        verts.push_back(v);
        normals.push_back({0, 0, -1});
    }
    for (uint32_t i : all_idx) out.indices.push_back(i);          // front
    for (uint32_t i : all_idx) out.indices.push_back(i + nverts); // back

    // Walls from boundary edges (edges claimed by exactly one cap triangle).
    map<pair<uint32_t, uint32_t>, int> counts;
    map<pair<uint32_t, uint32_t>, uint32_t> third;
    for (size_t k = 0; k < all_idx.size(); k += 3) {
        uint32_t t[3] = {all_idx[k], all_idx[k + 1], all_idx[k + 2]};
        for (int e = 0; e < 3; e++) {
            uint32_t a = t[e], b = t[(e + 1) % 3];
            third.insert({{a, b}, t[(e + 2) % 3]});
            auto key = a <= b ? make_pair(a, b) : make_pair(b, a);
            counts[key]++;
        }
    }
    vector<tuple<uint32_t, uint32_t, uint32_t>> edges;
    for (auto& [key, cnt] : counts)
        if (cnt == 1) {
            auto it = third.find(key);
            uint32_t c = it != third.end() ? it->second : third.at({key.second, key.first});
            edges.push_back({key.first, key.second, c});
        }
    sort(edges.begin(), edges.end());
    uint32_t wall_base = (uint32_t)verts.size();
    for (auto& [a, b, c] : edges) {
        Vec3 pa{verts[a][0], verts[a][1], 0}, pb{verts[b][0], verts[b][1], 0};
        Vec3 pc{verts[c][0], verts[c][1], 0};
        Vec3 e = pb - pa;
        Vec3 n2{-e.y, e.x, 0}; // rot90
        bool flip = n2.dot(pc - pa) > 0;
        Vec3 n = flip ? n2 * -1.0f : n2;
        n = n.normalize();
        // 4 verts: a+,b+,a-,b- ; two triangles; endpoints are the cap vertices
        uint32_t base = (uint32_t)verts.size();
        verts.push_back({verts[a][0], verts[a][1], z_front});
        normals.push_back({n.x, n.y, 0});
        verts.push_back({verts[b][0], verts[b][1], z_front});
        normals.push_back({n.x, n.y, 0});
        verts.push_back({verts[a][0], verts[a][1], z_back});
        normals.push_back({n.x, n.y, 0});
        verts.push_back({verts[b][0], verts[b][1], z_back});
        normals.push_back({n.x, n.y, 0});
        if (!flip) {
            out.indices.insert(out.indices.end(), {base, base + 1, base + 2, base + 1,
                                                   base + 3, base + 2});
        } else {
            out.indices.insert(out.indices.end(), {base, base + 2, base + 1, base + 1,
                                                   base + 2, base + 3});
        }
        (void)c;
    }
    return true;
}

// Average normals at coincident positions and inflate — smooth_hull.
static Mesh smooth_hull(const Mesh& glyph, float width) {
    Mesh hull;
    hull.verts = glyph.verts;
    hull.indices = glyph.indices;
    map<tuple<int, int, int>, array<float, 3>> acc;
    auto key = [&](const array<float, 3>& p) {
        return make_tuple((int)lrintf(p[0] * 8192), (int)lrintf(p[1] * 8192),
                          (int)lrintf(p[2] * 8192));
    };
    for (size_t i = 0; i < glyph.verts.size(); i++) {
        auto& e = acc[key(glyph.verts[i])];
        auto& n = glyph.normals[i];
        e[0] += n[0];
        e[1] += n[1];
        e[2] += n[2];
    }
    hull.normals.resize(glyph.verts.size());
    for (size_t i = 0; i < glyph.verts.size(); i++) {
        auto e = acc[key(glyph.verts[i])];
        float l = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        if (l <= 0) l = 1;
        for (int k = 0; k < 3; k++)
            hull.verts[i][k] += (e[k] / l) * width;
        hull.normals[i] = {e[0] / l, e[1] / l, e[2] / l};
    }
    return hull;
}
