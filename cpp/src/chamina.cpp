// chamina C++ — the whole engine in one translation unit.
//
// Pipeline: scenes/<name>.json → stb_truetype glyph outlines → earcut
// tessellation (even-odd holes, like the Rust mesher's EvenOdd fill) →
// extrusion → per-frame animation eval (ported from src/animation.rs) →
// software raster + post (ported from src/native) → ffmpeg rawvideo pipe.
//
// Every formula is a line-for-line port of the Rust/JS pair; see the comment
// beside each section for the source file it mirrors.
//
//   g++ -O2 -std=c++17 -fopenmp -I../vendor chamina.cpp -o chamina-cpp.exe
//
//   chamina-cpp scenes/bird.json --audio-env target/chamina/audio_env.json
//       -o out.mp4 [--from N] [--frames N] [--still t --png f]

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

// ---------------------------------------------------------------------------
// prepared scene
// ---------------------------------------------------------------------------

struct PreparedChar {
    size_t idx;
    Vec3 base_position;
    Mesh mesh, hull;
    float lands_at;
};

struct PreparedText {
    vector<PreparedChar> chars;
    // copied spec fields used per frame
    Vec3 position;
    float size, alpha;
    bool neon;
    float color_lin[3];
    float strength;
    bool has_outline;
    float outline_lin[3];
    float outline_strength;
    const json* anims;
    const json* char_times;
    // Block-level visibility window from its fade anims (the only alpha
    // source). Outside [vis_from, vis_to] every char's alpha is 0 — skip the
    // whole block's eval_all. Defaults keep never-fading blocks always on.
    float vis_from = -INFINITY, vis_to = INFINITY;
    float total_width_em = 0;
    float hw = 1;
};

struct Window {
    float start, fade_in, end, fade_out;
    float at(float t) const {
        if (t < start || t > end) return 0;
        float f = 1;
        if (t < start + fade_in) f = (t - start) / fade_in;
        float fo = end - fade_out;
        if (t > fo) f = min(f, (end - t) / fade_out);
        return clamp(f, 0.0f, 1.0f);
    }
};

struct AudioEnv {
    float fps = 60;
    vector<vector<float>> data;
    void sample(float t, float bands[64], float& bass, float& energy) const {
        for (int i = 0; i < 64; i++) bands[i] = 0;
        if (data.empty()) {
            bass = energy = 0;
            return;
        }
        long fi = lrintf(t * fps);
        fi = clamp(fi, 0L, (long)data.size() - 1);
        const auto& row = data[fi];
        float sum = 0;
        for (int i = 0; i < 64; i++) {
            float v = row[min((int)row.size() - 1, (int)((int64_t)i * (int64_t)row.size() / 64))];
            bands[i] = v;
            sum += v;
        }
        bass = 0;
        for (int i = 0; i < 8; i++) bass += bands[i];
        bass /= 8;
        energy = sum / 64;
    }
};

static uint32_t mulberry(uint32_t& state) {
    state += 0x6D2B79F5u;
    uint32_t t = state;
    t = (t ^ (t >> 15)) * (t | 1);
    t ^= t + ((t ^ (t >> 7)) * (t | 61));
    return t;
}
static float mulberry_f(uint32_t& state) { return (float)mulberry(state) / 4294967296.0f; }

// ---------------------------------------------------------------------------
// frame + raster (port of src/native/draw.rs)
// ---------------------------------------------------------------------------

struct Frame {
    int w = 0, h = 0;
    vector<array<float, 3>> color;
    vector<float> depth;

    void clear() {
        fill(color.begin(), color.end(), array<float, 3>{0, 0, 0});
        fill(depth.begin(), depth.end(), INFINITY);
    }
    bool to_px(const Cam& cam, Vec3 p, float& sx, float& sy, float& zc) const {
        Vec3 v = p - cam.eye;
        float z = v.dot(cam.fwd);
        if (z <= cam.near_z) return false;
        float xn = (v.dot(cam.right) / z) / (cam.tan_h * cam.aspect);
        float yn = (v.dot(cam.up) / z) / cam.tan_h;
        sx = (xn + 1) * 0.5f * w;
        sy = (1 - yn) * 0.5f * h;
        zc = z;
        return true;
    }
    // face: 0 both, 1 front only, 2 back only
    void tri(const Cam& cam, Vec3 a, Vec3 b, Vec3 c, const float rgb[3], float alpha, int face) {
        float ax, ay, az, bx, by, bz, cx2, cy2, cz;
        if (!to_px(cam, a, ax, ay, az) || !to_px(cam, b, bx, by, bz) ||
            !to_px(cam, c, cx2, cy2, cz))
            return;
        if (az <= 0 || bz <= 0 || cz <= 0) return;
        if (face != 0) {
            Vec3 n = (b - a).cross(c - a);
            Vec3 centre = (a + b + c) * (1.0f / 3.0f);
            bool back = n.dot(cam.eye - centre) <= 0;
            if ((face == 2) != back) return;
        }
        float area = (bx - ax) * (cy2 - ay) - (by - ay) * (cx2 - ax);
        if (fabsf(area) < 1e-9f) return;
        float inv = 1 / area;
        int x0 = (int)floorf(min(ax, min(bx, cx2)));
        int x1 = (int)ceilf(max(ax, max(bx, cx2)));
        int y0 = (int)floorf(min(ay, min(by, cy2)));
        int y1 = (int)ceilf(max(ay, max(by, cy2)));
        x0 = max(x0, 0);
        y0 = max(y0, 0);
        x1 = min(x1, w - 1);
        y1 = min(y1, h - 1);
        for (int py = y0; py <= y1; py++)
            for (int px = x0; px <= x1; px++) {
                float x = px + 0.5f, y = py + 0.5f;
                float w0 = ((bx - x) * (cy2 - y) - (by - y) * (cx2 - x)) * inv;
                float w1 = ((cx2 - x) * (ay - y) - (cy2 - y) * (ax - x)) * inv;
                float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = az * w0 + bz * w1 + cz * w2;
                size_t i = (size_t)py * w + px;
                if (z >= depth[i]) continue;
                depth[i] = z;
                auto& s = this->color[i];
                s[0] = rgb[0] * alpha + s[0] * (1 - alpha);
                s[1] = rgb[1] * alpha + s[1] * (1 - alpha);
                s[2] = rgb[2] * alpha + s[2] * (1 - alpha);
            }
    }
    // additive disc sprite (fragment: a² × glow, blend ×2 — the GLSL chain)
    void disc(const Cam& cam, Vec3 center, float size_px, const float rgb[3], float glow) {
        float sx, sy, zc;
        if (!to_px(cam, center, sx, sy, zc) || zc <= 0 || size_px <= 0) return;
        float r = size_px * 0.5f;
        int x0 = (int)floorf(sx - r), x1 = (int)ceilf(sx + r);
        int y0 = (int)floorf(sy - r), y1 = (int)ceilf(sy + r);
        x0 = max(x0, 0);
        y0 = max(y0, 0);
        x1 = min(x1, w - 1);
        y1 = min(y1, h - 1);
        if (x0 > x1 || y0 > y1) return;
        size_t ci = (size_t)clamp((int)sy, 0, h - 1) * w + clamp((int)sx, 0, w - 1);
        if (zc >= depth[ci] * 1.15f) return;
        for (int py = y0; py <= y1; py++)
            for (int px = x0; px <= x1; px++) {
                float dx = (px + 0.5f - sx) / size_px;
                float dy = (py + 0.5f - sy) / size_px;
                float d = sqrtf(dx * dx + dy * dy);
                float tt = clamp(1 - 2 * d, 0.0f, 1.0f);
                float a = tt * tt * (3 - 2 * tt);
                float alpha = a * a * glow;
                if (alpha <= 0.004f) continue;
                size_t i = (size_t)py * w + px;
                auto& s = this->color[i];
                s[0] += rgb[0] * alpha * 2.0f;
                s[1] += rgb[1] * alpha * 2.0f;
                s[2] += rgb[2] * alpha * 2.0f;
            }
    }
};

// ---------------------------------------------------------------------------
// skydome + visualisers (port of src/native/draw.rs)
// ---------------------------------------------------------------------------

static float hash2(float x, float y) {
    float s = sinf(x * 127.1f + y * 311.7f) * 43758.5453123f;
    return s - floorf(s);
}

// vnoise evaluates hash2 only at integer lattice corners, revisiting the same
// points every frame. A precomputed table replaces ~16M large-arg sinf calls
// per frame with lookups. Bit-exact: identical formula, cached results. The
// table covers lattice points [0,1024) x [0,512) — enough for t ≤ ~6 min at
// palette speed ≤ 9 — anything else (negative / out of range) falls back to
// the exact hash2.
static const float* lattice_hash_tab() {
    static vector<float> tab = [] {
        vector<float> t((size_t)1024 * 512);
        for (int y = 0; y < 512; y++)
            for (int x = 0; x < 1024; x++)
                t[(size_t)y * 1024 + x] = hash2((float)x, (float)y);
        return t;
    }();
    return tab.data();
}
static float hash2_lattice(float x, float y) {
    if (x >= 0 && y >= 0 && x < 1024 && y < 512 && x == floorf(x) && y == floorf(y))
        return lattice_hash_tab()[(size_t)(int)y * 1024 + (int)x];
    return hash2(x, y);
}static float vnoise(float x, float y) {
    float ix = floorf(x), iy = floorf(y);
    float fx = x - ix, fy = y - iy;
    float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    float a = hash2_lattice(ix, iy) * (1 - ux) + hash2_lattice(ix + 1, iy) * ux;
    float b = hash2_lattice(ix, iy + 1) * (1 - ux) + hash2_lattice(ix + 1, iy + 1) * ux;
    return a * (1 - uy) + b * uy;
}
static float fbm2(float x, float y) {
    float v = 0, a = 0.5f;
    for (int i = 0; i < 2; i++) {
        v += a * vnoise(x, y);
        x = x * 2.03f + 1.7f;
        y = y * 2.03f + 9.2f;
        a *= 0.5f;
    }
    return v;
}

// Per-cell star hash values depend only on the cell index, but the reference
// recomputes three hash2 calls per full-res pixel per frame (~6M large-arg
// sinf). Hoisted into a table, bit-exact (same expressions). 381x191 covers
// c0 = floor(uv0*380) ≤ 380 and c1 = floor(uv1*190) ≤ 190 exactly.
struct StarTable {
    static constexpr int CW = 381, CH = 191;
    float r[CW * CH], j0[CW * CH], j1[CW * CH];
    static const StarTable& get() {
        static const StarTable t = [] {
            StarTable s;
            for (int y = 0; y < CH; y++)
                for (int x = 0; x < CW; x++) {
                    size_t i = (size_t)y * CW + x;
                    s.r[i] = hash2((float)x, (float)y);
                    s.j0[i] = hash2((float)x + 7.1f, (float)y + 7.1f) - 0.5f;
                    s.j1[i] = hash2((float)x + 13.7f, (float)y + 13.7f) - 0.5f;
                }
            return s;
        }();
        return t;
    }
};

[[maybe_unused]] static void skydome_pixel(const Vec3& d, float t, const float pal[12], float col[3]) {
    float h = clamp(d.y * 0.5f + 0.5f, 0.0f, 1.0f);
    float mixv = powf(h, 0.85f);
    for (int i = 0; i < 3; i++) col[i] = pal[3 + i] + (pal[i] - pal[3 + i]) * mixv;
    float uv0 = atan2f(d.z, d.x) / ((float)M_PI * 2) + 0.5f;
    float uv1 = d.y * 0.5f + 0.5f;
    float n = fbm2((uv0 * 7 + t * pal[11] * 0.018f) * 2, (uv1 * 3.5f + t * pal[11] * 0.009f) * 2);
    float k = clamp((n - 0.40f) / (0.92f - 0.40f), 0.0f, 1.0f);
    float cloud = k * k * (3 - 2 * k);
    float band = expf(-fabsf(d.y) * 5);
    for (int i = 0; i < 3; i++)
        col[i] += pal[6 + i] * (cloud * pal[9] * 0.95f + band * pal[9] * 0.30f);
    if (pal[10] > 0.001f) {
        float sp0 = uv0 * 380, sp1 = uv1 * 190;
        float c0 = floorf(sp0), c1 = floorf(sp1);
        float r = hash2(c0, c1);
        float f0 = sp0 - c0 - 0.5f, f1 = sp1 - c1 - 0.5f;
        float j0 = hash2(c0 + 7.1f, c1 + 7.1f) - 0.5f;
        float j1 = hash2(c0 + 13.7f, c1 + 13.7f) - 0.5f;
        float dd = sqrtf((f0 - j0 * 0.6f) * (f0 - j0 * 0.6f) + (f1 - j1 * 0.6f) * (f1 - j1 * 0.6f));
        float kk = clamp((dd - 0.16f) / (0 - 0.16f), 0.0f, 1.0f);
        float star = kk * kk * (3 - 2 * kk);
        float tw = 0.55f + 0.45f * sinf(t * 1.7f + r * 40);
        float on = r <= 0.055f ? 1.0f : 0.0f;
        float add[3] = {0.78f, 0.86f, 1.0f};
        for (int i = 0; i < 3; i++) col[i] += add[i] * star * on * tw * pal[10] * 1.7f;
    }
    float rad = clamp(sqrtf(d.x * d.x + d.y * d.y) * 1.15f, 0.0f, 1.0f);
    float sink = 1 - 0.30f * rad * rad;
    col[0] *= sink;
    col[1] *= sink;
    col[2] *= sink;
}

static void draw_skydome(Frame& f, const Cam& cam, float t, const float pal[12]) {
    const int w = f.w, h = f.h;

    // 1-D LUTs for the two transcendental per-pixel terms (outputs are mixed
    // into gradients far below 8-bit significance).
    static const float* lut_pow085 = [] {
        static float v[4097];
        for (int i = 0; i <= 4096; i++) v[i] = powf((float)i / 4096, 0.85f);
        return v;
    }();
    static const float* lut_exp5 = [] {
        static float v[2049];
        for (int i = 0; i <= 2048; i++) v[i] = expf(-(float)i / 2048 * 5.0f);
        return v;
    }();
    auto lut = [](const float* lutv, int n, float x) {
        float xx = clamp(x, 0.0f, 1.0f) * n;
        int i = (int)xx;
        if (i > n - 1) i = n - 1;
        float fr = xx - i;
        return lutv[i] + (lutv[i + 1] - lutv[i]) * fr;
    };

    // The nebula is the expensive part (8 sinf per pixel in fbm) and is pure
    // low frequency: sample it at half resolution and bilinear upsample. The
    // stars stay full res — they are 1-2 px points and would not survive.
    const int hw = max(w / 2, 1), hh = max(h / 2, 1);
    static vector<float> cloud;
    const bool need_cloud = pal[9] > 0.001f && !getenv("CPP_SKIP_CLOUD");
    if (need_cloud) {
        cloud.assign((size_t)hw * hh, 0.0f);
#pragma omp parallel for schedule(static)
        for (int row = 0; row < hh; row++) {
            float py = (row + 0.5f) * h / hh;
            float yn = 1 - 2 * py / h;
            for (int x = 0; x < hw; x++) {
                float px = (x + 0.5f) * w / hw;
                float xn = 2 * px / w - 1;
                Vec3 ray = (cam.fwd + cam.right * (xn * cam.tan_h * cam.aspect) +
                            cam.up * (yn * cam.tan_h))
                               .normalize();
                float uv0 = atan2f(ray.z, ray.x) / (float)M_PI / 2 + 0.5f;
                float uv1 = ray.y * 0.5f + 0.5f;
                float qx = (uv0 * 7 + t * pal[11] * 0.018f) * 2;
                float qy = (uv1 * 3.5f + t * pal[11] * 0.009f) * 2;
                float n = fbm2(qx, qy);
                float kk = clamp((n - 0.40f) / (0.92f - 0.40f), 0.0f, 1.0f);
                cloud[(size_t)row * hw + x] = kk * kk * (3 - 2 * kk);
            }
        }
    }

    const bool need_stars = pal[10] > 0.001f && !getenv("CPP_SKIP_STARS");
    // Per-cell twinkle is a function of (t, r) and r is per-cell — hoist the
    // sinf out of the full-res pixel loop (2M → 73k calls/frame), bit-exact.
    static vector<float> cell_tw;
    if (need_stars) {
        const StarTable& st = StarTable::get();
        if ((size_t)StarTable::CW * StarTable::CH > cell_tw.size())
            cell_tw.resize((size_t)StarTable::CW * StarTable::CH);
#pragma omp parallel for schedule(static)
        for (long i = 0; i < (long)StarTable::CW * StarTable::CH; i++)
            cell_tw[i] = 0.55f + 0.45f * sinf(t * 1.7f + st.r[i] * 40);
    }
#pragma omp parallel for schedule(static)
    for (int row = 0; row < h; row++) {
        float py = row + 0.5f;
        float yn = 1 - 2 * py / h;
        for (int col_i = 0; col_i < w; col_i++) {
            float xn = 2 * (col_i + 0.5f) / w - 1;
            Vec3 d = (cam.fwd + cam.right * (xn * cam.tan_h * cam.aspect) +
                      cam.up * (yn * cam.tan_h))
                         .normalize();
            // gradient (powf → LUT)
            float h01 = clamp(d.y * 0.5f + 0.5f, 0.0f, 1.0f);
            float mixv = lut(lut_pow085, 4096, h01);
            float col[3];
            for (int i = 0; i < 3; i++)
                col[i] = pal[3 + i] + (pal[i] - pal[3 + i]) * mixv;
            // nebula cloud (half-res field) + horizon band (expf → LUT)
            if (need_cloud) {
                float cx = (col_i + 0.5f) * hw / w - 0.5f;
                float cy = (row + 0.5f) * hh / h - 0.5f;
                int x0 = (int)floorf(max(cx, 0.0f));
                int y0 = (int)floorf(max(cy, 0.0f));
                int x1 = min(x0 + 1, hw - 1), y1 = min(y0 + 1, hh - 1);
                float tx = clamp(cx - x0, 0.0f, 1.0f), ty = clamp(cy - y0, 0.0f, 1.0f);
                float cval = (cloud[(size_t)y0 * hw + x0] * (1 - tx) +
                              cloud[(size_t)y0 * hw + x1] * tx) * (1 - ty) +
                             (cloud[(size_t)y1 * hw + x0] * (1 - tx) +
                              cloud[(size_t)y1 * hw + x1] * tx) * ty;
                float band = lut(lut_exp5, 2048, fabsf(d.y));
                for (int i = 0; i < 3; i++)
                    col[i] += pal[6 + i] * (cval * pal[9] * 0.95f + band * pal[9] * 0.30f);
            }
            if (need_stars) {
                float uv0 = atan2f(d.z, d.x) / (float)M_PI / 2 + 0.5f;
                float uv1 = d.y * 0.5f + 0.5f;
                float sp0 = uv0 * 380, sp1 = uv1 * 190;
                float c0 = floorf(sp0), c1 = floorf(sp1);
                const StarTable& st = StarTable::get();
                size_t ci = (size_t)(int)c1 * StarTable::CW + (int)c0;
                float r = st.r[ci];
                float f0 = sp0 - c0 - 0.5f, f1 = sp1 - c1 - 0.5f;
                float j0 = st.j0[ci];
                float j1 = st.j1[ci];
                float dd = sqrtf((f0 - j0 * 0.6f) * (f0 - j0 * 0.6f) +
                                 (f1 - j1 * 0.6f) * (f1 - j1 * 0.6f));
                float kk = clamp((dd - 0.16f) / -0.16f, 0.0f, 1.0f);
                float star = kk * kk * (3 - 2 * kk);
                float tw = cell_tw[ci];
                float on = r <= 0.055f ? 1.0f : 0.0f;
                float add[3] = {0.78f, 0.86f, 1.0f};
                for (int i = 0; i < 3; i++) col[i] += add[i] * star * on * tw * pal[10] * 1.7f;
            }
            float rad = clamp(sqrtf(d.x * d.x + d.y * d.y) * 1.15f, 0.0f, 1.0f);
            float sink = 1 - 0.30f * rad * rad;
            f.color[(size_t)row * w + col_i] = {col[0] * sink, col[1] * sink, col[2] * sink};
            f.depth[(size_t)row * w + col_i] = 300.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// visualiser state (port of src/native/draw.rs rings/bars/sparks/streaks)
// ---------------------------------------------------------------------------

struct RingSpec {
    Vec3 center;
    float r0, r1, tilt, hue_speed, spin, p0, p1, opacity;
    Window window;
    vector<array<float, 3>> pos;
    vector<float> band, k;
};

static void hsv2rgb(float h, float s, float v, float out[3]) {
    float K[3] = {1.0f, 2.0f / 3, 1.0f / 3};
    for (int i = 0; i < 3; i++) {
        float p = fabsf(fmodf(h + K[i], 1.0f)) * 6 - 3;
        p = clamp(p - 1, 0.0f, 1.0f);
        out[i] = v * (1 - s * p);
    }
}
static void hsl_to_linear(float h, float s, float l, float out[3]) {
    auto f = [&](float n) {
        float k = fmodf(n + h * 12, 12);
        float a = s * min(l, 1 - l);
        float v = l - a * max(-1.0f, min(min(k - 3, 9 - k), 1.0f));
        v = clamp(v, 0.0f, 1.0f);
        return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
    };
    out[0] = f(0);
    out[1] = f(8);
    out[2] = f(4);
}

struct Sparks {
    struct P {
        Vec3 pos;
        float cue;
        float color[3];
        float s0, s1, s2;
    };
    vector<P> p;
};

struct World {
    json scene, meta;
    AudioEnv env;
    bool has_env = false;
    vector<float> cues;
    Window intro{2.0f, 1.6f, 6.0f, 1.3f}, outro{0, 3, 0, 2};
    vector<PreparedText> texts;
    vector<RingSpec> rings;
    struct Bars {
        Vec3 center;
        float r0, tilt, max_len;
        Window window;
    } bars{Vec3{0, 0.8f, -1.4f}, 6.05f, -0.1f, 2.6f, Window{}};
    Sparks sparks;
    struct Quad {
        float c[2], t[2], n[2], len, wid, ring;
    };
    vector<Quad> quads;
    vector<array<float, 3>> cloud; // particle offsets
    vector<array<float, 3>> cloud_extra; // phase/speed/scale packed
    float bloom_gain = 6.0f;

    World(const json& sc, const json& mt) : scene(sc), meta(mt) {}

    float duration() const {
        return meta.at("total_frames").get<float>() / meta.at("fps").get<float>();
    }

    void prepare(const json& env_json) {
        if (env_json.is_object()) {
            env.fps = env_json.value("fps", 60.0f);
            for (const auto& row : env_json.at("data"))
                env.data.push_back(row.get<vector<float>>());
            has_env = true;
        }
        float dur = duration();
        for (const auto& tx : scene.at("texts"))
            for (const auto& a : tx.at("animations")) {
                // Older scenes may omit start_at on a fade; a missing cue is
                // simply not tracked rather than fatal.
                if (a.value("type", "") == "fade") {
                    auto it = a.find("start_at");
                    if (it != a.end() && it->is_number())
                        cues.push_back(it->get<float>());
                }
            }
        sort(cues.begin(), cues.end());
        float outro_start = INFINITY;
        for (const auto& tx : scene.at("texts")) {
            if (tx.contains("char_times")) continue;
            if (!tx.at("animations").empty()) {
                float at = tx.at("animations")[0].value("start_at", 0.0f);
                if (at > 60 && at < outro_start) outro_start = at;
            }
        }
        if (!isfinite(outro_start)) outro_start = 0;
        outro = {outro_start, 3, dur, 2};
        float first_lyric = INFINITY;
        for (float c : cues)
            if (c > 8) {
                first_lyric = c;
                break;
            }
        if (!isfinite(first_lyric)) first_lyric = min(dur * 0.1f, 16.5f);
        intro = {2.0f, 1.6f, max(first_lyric - 0.7f, 6.0f), 1.3f};

        RingSpec outro_ring{Vec3{0, 0.8f, -1.2f}, 4.0f, 5.6f, 0, 0.045f, 0.06f, 0.9f, 2.2f, 0.95f, outro, {}, {}, {}};
        RingSpec intro_ring{Vec3{0, 0.55f, -1.9f}, 2.9f, 4.3f, -0.15f, 0.03f, 0.05f, 0.7f, 1.7f, 0.8f, intro, {}, {}, {}};
        for (auto* rs : {&outro_ring, &intro_ring})
            for (int b = 0; b < 64; b++) {
                float ang = -(float)M_PI / 2 + (float)b / 64 * (float)M_PI * 2;
                for (int k = 0; k < 7; k++) {
                    float r = rs->r0 + (rs->r1 - rs->r0) * k / 6.0f;
                    rs->pos.push_back({cosf(ang) * r, sinf(ang) * r, 0});
                    rs->band.push_back((float)b);
                    rs->k.push_back((float)k);
                }
            }
        rings = {outro_ring, intro_ring};
        bars.window = outro;

        uint32_t rs_ = meta.value("seed", 0u) ^ 0x9E3779B9u;
        for (int i = 0; i < 26; i++) {
            float th = (float)i / 26 * (float)M_PI * 2 + (mulberry_f(rs_) - 0.5f) * 0.35f;
            float rad = 3.6f + mulberry_f(rs_) * 5.2f;
            float len = 1.6f + mulberry_f(rs_) * 2.8f;
            float wid = 0.022f + mulberry_f(rs_) * 0.035f;
            Quad q;
            q.c[0] = cosf(th) * rad;
            q.c[1] = sinf(th) * rad;
            q.t[0] = -sinf(th);
            q.t[1] = cosf(th);
            q.n[0] = cosf(th);
            q.n[1] = sinf(th);
            q.len = len;
            q.wid = wid;
            q.ring = (rad - 3.6f) / 5.2f;
            quads.push_back(q);
        }
        // particle cloud
        uint32_t ps = meta.value("seed", 0u);
        for (int i = 0; i < 2600; i++) {
            cloud.push_back({(mulberry_f(ps) - 0.5f) * 70, mulberry_f(ps) * 70 - 25,
                             (mulberry_f(ps) - 0.5f) * 45 - 20});
            float phase = mulberry_f(ps) * (float)M_PI * 2;
            float speed = 0.25f + mulberry_f(ps) * 1.4f;
            float scale = 0.6f + mulberry_f(ps) * 2.4f;
            cloud_extra.push_back({phase, speed, scale});
        }
    }

    float follow_roll(float cam_roll, float t, float lands_at) const {
        if (cam_roll == 0) return 0;
        float u = t - lands_at;
        if (u < -0.02f || u >= 1.05f) return 0;
        float w = u <= 0.45f ? 1.0f : 1 - smoothstep_s((u - 0.45f) / 0.6f);
        return w > 0.001f ? cam_roll * w : 0;
    }

    // Renders into a caller-owned buffer so the two 30 MB vectors keep their
    // capacity across frames instead of being re-allocated per frame.
    void render_frame_into(float t, Frame& f) const {
        int w = meta.at("width").get<int>(), h = meta.at("height").get<int>();
        float fov = meta.at("camera").at("fov").get<float>();
        float aspect = (float)w / h;
        float near_z = meta.at("camera").value("near", 0.01f);
        float bands[64], bass, energy;
        env.sample(t, bands, bass, energy);
        const json* fx = meta.value("fx", json::object()).value("enabled", false)
                             ? &meta.at("fx")
                             : nullptr;
        const json* orbit = nullptr;
        if (meta.contains("camera_animation") &&
            meta.at("camera_animation").value("type", "") == "orbit")
            orbit = &meta.at("camera_animation");
        Cam cam = orbit
                      ? update_camera(*orbit, t, cues, fx, energy, fov, aspect, near_z)
                      : Cam{Vec3{0, 1, 26}, Vec3{0, 1, 0}, Vec3{0, 0, -1}, Vec3{1, 0, 0},
                            Vec3{0, 1, 0}, 0, tanf(fov * 0.5f), aspect, near_z};

        if (TRACE)
            fprintf(stderr, "[dbg] cam eye=(%.3f,%.3f,%.3f) tgt=(%.3f,%.3f,%.3f) roll=%.3f\n",
                    cam.eye.x, cam.eye.y, cam.eye.z, cam.target.x, cam.target.y,
                    cam.target.z, cam.roll);
        f.w = w;
        f.h = h;
        // The dome overwrites every colour pixel when a background is active,
        // so the colour clear is only needed on a flat-background scene.
        // resize (not assign): once sized, later frames write nothing here.
        float pal[12];
        const bool has_bg = eval_backgrounds(meta.value("backgrounds", json::array()), t, pal);
        if (has_bg)
            f.color.resize((size_t)w * h);
        else
            f.color.assign((size_t)w * h, {0, 0, 0});
        f.depth.assign((size_t)w * h, INFINITY);
        if (has_bg) {
            // sRGB → linear for the shader-space math
            float lin[12];
            auto to_lin = [](float c) {
                return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
            };
            for (int i = 0; i < 9; i++) lin[i] = to_lin(pal[i]);
            lin[9] = pal[9];
            lin[10] = pal[10];
            lin[11] = pal[11];
            double _t_sky = 0;
            if (getenv("CPP_PROF")) _t_sky = chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count();
            draw_skydome(f, cam, t, lin);
            if (getenv("CPP_PROF")) {
                _t_sky = chrono::duration<double>(chrono::steady_clock::now().time_since_epoch()).count() - _t_sky;
                fprintf(stderr, "[prof] skydome %.1fms\n", _t_sky * 1000);
            }
        }

        // text, far to near
        vector<size_t> order(texts.size());
        for (size_t i = 0; i < texts.size(); i++) order[i] = i;
        sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return (texts[b].position - cam.eye).length() >
                   (texts[a].position - cam.eye).length();
        });
        for (size_t bi : order) {
            const auto& tx = texts[bi];
            if (t < tx.vis_from || t > tx.vis_to) continue;
            if (TRACE && !tx.chars.empty()) {
                AnimState st0 = eval_all(*tx.anims, t, tx.chars[0].idx, tx.size, tx.char_times);
                fprintf(stderr,
                        "[dbg] text %zu a0=%.2f pos=(%.1f,%.1f) base0=(%.1f,%.1f) verts=%zu scale=%.2f\n",
                        bi, tx.alpha * st0.alpha, tx.position.x, tx.position.y,
                        tx.chars[0].base_position.x, tx.chars[0].base_position.y,
                        tx.chars[0].mesh.verts.size(), st0.scale);
            }
            for (const auto& ch : tx.chars) {
                AnimState st = eval_all(*tx.anims, t, ch.idx, tx.size, tx.char_times);
                float alpha = tx.alpha * st.alpha;
                if (alpha <= 0.002f || st.scale <= 1e-3f) continue;
                float fr = follow_roll(cam.roll, t, ch.lands_at);
                Quat q = st.rotation * Quat::axis_angle(Vec3{0, 0, 1}, fr);
                float s = tx.size * st.scale;
                Vec3 pos = ch.base_position + st.offset;
                auto xf = [&](const array<float, 3>& v) {
                    return pos + q.mul_vec3(Vec3{v[0] * s, v[1] * s, v[2] * s});
                };
                for (size_t k = 0; k + 2 < ch.mesh.indices.size(); k += 3) {
                    uint32_t i0 = ch.mesh.indices[k], i1 = ch.mesh.indices[k + 1],
                             i2 = ch.mesh.indices[k + 2];
                    f.tri(cam, xf(ch.mesh.verts[i0]), xf(ch.mesh.verts[i1]),
                          xf(ch.mesh.verts[i2]), tx.color_lin, alpha, 0);
                }
                if (tx.has_outline) {
                    for (size_t k = 0; k + 2 < ch.hull.indices.size(); k += 3) {
                        uint32_t i0 = ch.hull.indices[k], i1 = ch.hull.indices[k + 1],
                                 i2 = ch.hull.indices[k + 2];
                        f.tri(cam, xf(ch.hull.verts[i0]), xf(ch.hull.verts[i1]),
                              xf(ch.hull.verts[i2]), tx.outline_lin, alpha, 2);
                    }
                }
            }
        }

        // follow offset + particles + viz
        Vec3 orbit_base{0, 0, 0};
        Vec3 follow{0, 0, 0};
        if (orbit) {
            Rig rig = eval_orbit(*orbit, t);
            orbit_base = Vec3{orbit->at("target")[0].get<float>(),
                              orbit->at("target")[1].get<float>(),
                              orbit->at("target")[2].get<float>()};
            follow = (rig.target - orbit_base) * 0.75f;
        }
        float pcol[3];
        {
            float sr[3];
            hex_srgb("#9fd8ff", sr);
            auto lin = [](float c) {
                return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
            };
            pcol[0] = lin(sr[0]);
            pcol[1] = lin(sr[1]);
            pcol[2] = lin(sr[2]);
        }
        float breath = getenv("CPP_NOVIZ") ? 0.0f : max(intro.at(t), outro.at(t));
        float popacity = 0.9f * (1 + 0.28f * bass * breath);
        for (size_t i = 0; i < cloud.size(); i++) {
            float y = fmodf(cloud[i][1] + t * cloud_extra[i][1] + 45.0f, 70.0f) - 25.0f;
            Vec3 c = follow + Vec3{cloud[i][0], y, cloud[i][2]};
            Vec3 v = c - cam.eye;
            float zc = v.dot(cam.fwd);
            if (zc <= cam.near_z) continue;
            float size = min(cloud_extra[i][2] * (260.0f / max(zc, 0.001f)), 46.0f);
            float tw = 0.45f + 0.55f * sinf(t * 2.1f + cloud_extra[i][0]);
            tw *= smoothstep_s((zc - 2) / 5);
            f.disc(cam, c, size, pcol, tw * popacity);
        }
        if (has_env && !getenv("CPP_NOVIZ")) {
            float intro_f = intro.at(t), outro_f = outro.at(t);
            float fades[2] = {outro_f, intro_f};
            for (int ri = 0; ri < 2; ri++) {
                const RingSpec& rs = rings[ri];
                float fade = fades[ri];
                if (fade <= 0.003f) continue;
                float spin = t * rs.spin, ca = cosf(spin), sa = sinf(spin);
                float ct = cosf(rs.tilt), stl = sinf(rs.tilt);
                for (size_t i = 0; i < rs.pos.size(); i++) {
                    float px = rs.pos[i][0] * ca - rs.pos[i][1] * sa;
                    float py = rs.pos[i][0] * sa + rs.pos[i][1] * ca;
                    Vec3 pw{px, py * ct, py * stl};
                    pw = pw + rs.center;
                    float e = bands[(int)rs.band[i]];
                    float kk = clamp((e * 7 - (rs.k[i] - 0.45f)) / 0.5f, 0.0f, 1.0f);
                    float lit = kk * kk * (3 - 2 * kk);
                    float shim = 0.5f + 0.5f * sinf(t * 1.7f + rs.band[i] * 1.53f);
                    float size = (rs.p0 + (rs.p1 - rs.p0) * lit) * (0.75f + 0.45f * e);
                    Vec3 v = pw - cam.eye;
                    float zc = v.dot(cam.fwd);
                    if (zc <= cam.near_z) continue;
                    float size_px = min(size * (260.0f / max(zc, 0.001f)), 44.0f);
                    float glow = 0.16f + 0.84f * lit * (0.72f + 0.28f * shim);
                    glow *= smoothstep_s((zc - 2) / 5) * fade;
                    float hue = fmodf(rs.band[i] / 64 + t * rs.hue_speed, 1.0f);
                    float vc[3];
                    hsv2rgb(hue, 0.85f, 0.30f + 0.70f * lit, vc);
                    f.disc(cam, pw, size_px, vc, glow * rs.opacity);
                }
            }
            // bars
            float bar_fade = outro_f;
            if (bar_fade > 0.003f) {
                float op = 0.9f * bar_fade;
                float ct = cosf(bars.tilt), stl = sinf(bars.tilt);
                static const float BV[8][3] = {{0, -0.5f, -0.5f}, {1, -0.5f, -0.5f},
                    {1, 0.5f, -0.5f}, {0, 0.5f, -0.5f}, {0, -0.5f, 0.5f}, {1, -0.5f, 0.5f},
                    {1, 0.5f, 0.5f}, {0, 0.5f, 0.5f}};
                static const int BT[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
                    {0, 1, 5}, {0, 5, 4}, {2, 3, 7}, {2, 7, 6}, {1, 2, 6}, {1, 6, 5},
                    {3, 0, 4}, {3, 4, 7}};
                for (int b = 0; b < 64; b++) {
                    float v = bands[b];
                    float ang = -(float)M_PI / 2 + (float)b / 64 * (float)M_PI * 2 + t * 0.05f;
                    float sx = 0.22f + v * bars.max_len;
                    float col[3];
                    hsl_to_linear(fmodf((float)b / 64 + t * 0.02f, 1.0f), 0.85f, 0.22f + 0.5f * v, col);
                    float ca = cosf(ang), sa = sinf(ang);
                    for (auto& tri : BT) {
                        Vec3 wv[3];
                        for (int vi = 0; vi < 3; vi++) {
                            float lx = BV[tri[vi]][0] * sx, ly = BV[tri[vi]][1] * 0.11f;
                            float x = lx * ca - ly * sa, y = lx * sa + ly * ca;
                            wv[vi] = bars.center +
                                     Vec3{x, y * ct, y * stl};
                        }
                        f.tri(cam, wv[0], wv[1], wv[2], col, op, 0);
                    }
                }
            }
            // sparks
            for (const auto& sp : sparks.p) {
                float u = (t - sp.cue) / 1.15f;
                if (u < 0 || u >= 1) continue;
                float e = (1 - u) * (1 - u);
                Vec3 c = sp.pos + Vec3{cosf(sp.s0), sinf(sp.s0), 0} * (sp.s1 * u * 1.5f) +
                         Vec3{0, u * 0.35f, 0};
                Vec3 v = c - cam.eye;
                float zc = v.dot(cam.fwd);
                if (zc <= cam.near_z) continue;
                float size_px = min(sp.s2 * (0.45f + 0.55f * e) * (240.0f / max(zc, 0.001f)), 26.0f);
                f.disc(cam, c, size_px, sp.color, e * e * 0.9f);
            }
        }
        // streaks
        float s_op = clamp((fabsf(cam.roll) - 0.22f) / 0.9f, 0.0f, 1.0f) * 0.85f;
        if (s_op > 0.01f) {
            float rz = cam.roll * 0.92f, cr = cosf(rz), sr = sinf(rz);
            Vec3 origin = follow + Vec3{0, 0, -0.7f};
            for (const auto& qd : quads) {
                float c2x = qd.c[0] * cr - qd.c[1] * sr, c2y = qd.c[0] * sr + qd.c[1] * cr;
                float t2x = qd.t[0] * cr - qd.t[1] * sr, t2y = qd.t[0] * sr + qd.t[1] * cr;
                float n2x = qd.n[0] * cr - qd.n[1] * sr, n2y = qd.n[0] * sr + qd.n[1] * cr;
                Vec3 centre = origin + Vec3{c2x, c2y, 0};
                float col[3] = {0.82f + (0.45f - 0.82f) * qd.ring,
                                0.93f + (0.75f - 0.93f) * qd.ring,
                                1.0f + (1.0f - 1.0f) * qd.ring};
                for (int row = 0; row < f.h; row++) { // coarse bbox via projection
                    (void)row;
                    break;
                }
                // analytic fill: unproject every pixel in a projected bbox
                float xs[4], ys[4];
                int okn = 0;
                float xt[4] = {-1, 1, 1, -1}, yt[4] = {-1, -1, 1, 1};
                for (int vi = 0; vi < 4; vi++) {
                    Vec3 corner = centre +
                                  Vec3{t2x, t2y, 0} * (xt[vi] * qd.len * 0.5f) +
                                  Vec3{n2x, n2y, 0} * (yt[vi] * qd.wid * 0.5f);
                    float sx, sy, zc;
                    if (f.to_px(cam, corner, sx, sy, zc)) {
                        xs[vi] = sx;
                        ys[vi] = sy;
                        okn++;
                    }
                }
                if (okn < 4) continue;
                int x0 = max(0, (int)floorf(min(min(xs[0], xs[1]), min(xs[2], xs[3]))));
                int x1 = min(f.w - 1, (int)ceilf(max(max(xs[0], xs[1]), max(xs[2], xs[3]))));
                int y0 = max(0, (int)floorf(min(min(ys[0], ys[1]), min(ys[2], ys[3]))));
                int y1 = min(f.h - 1, (int)ceilf(max(max(ys[0], ys[1]), max(ys[2], ys[3]))));
                for (int py = y0; py <= y1; py++)
                    for (int px = x0; px <= x1; px++) {
                        float wpx = px + 0.5f, wpy = py + 0.5f;
                        float ndc_x = 2 * wpx / f.w - 1, ndc_y = 1 - 2 * wpy / f.h;
                        Vec3 dir = (cam.fwd + cam.right * (ndc_x * cam.tan_h * cam.aspect) +
                                    cam.up * (ndc_y * cam.tan_h))
                                       .normalize();
                        if (fabsf(dir.z) < 1e-6f) continue;
                        float tt = (centre.z - cam.eye.z) / dir.z;
                        if (tt <= 0) continue;
                        Vec3 p = cam.eye + dir * tt;
                        Vec3 rel = p - centre;
                        float vt = (rel.x * t2x + rel.y * t2y) / (qd.len * 0.5f);
                        float vn = (rel.x * n2x + rel.y * n2y) / (qd.wid * 0.5f);
                        if (fabsf(vt) > 1 || fabsf(vn) > 1) continue;
                        size_t i = (size_t)py * f.w + px;
                        if (tt >= f.depth[i]) continue;
                        float a = (1 - fabsf(vt)) * (1 - fabsf(vt)) * s_op *
                                  (0.3f + 0.5f * qd.ring);
                        auto& s = f.color[i];
                        s[0] += col[0] * a * 1.6f;
                        s[1] += col[1] * a * 1.6f;
                        s[2] += col[2] * a * 1.6f;
                    }
            }
        }
    }

};

// ---------------------------------------------------------------------------
// post (port of src/native/post.rs)
// ---------------------------------------------------------------------------

using Img = vector<array<float, 3>>;

// Vertical gaussian blur into a caller-owned buffer. NOTE on semantics: the
// Rust reference (src/native/post.rs blur) computes a horizontal pass into
// `tmp` that its vertical pass never reads — the effective operation is
// vertical-only, both implementations agree, and the accepted gold-standard
// stills were rendered that way. The dead horizontal pass is skipped here.
// All buffers persist across frames (sizes are constant per scene, so
// resize() only allocates on the first call — was ~50MB of churn/frame).
static void blur_into(const Img& src, int w, int h, float sigma, Img& out) {
    int radius = (int)ceilf(sigma * 2.5f);
    float weights[64];
    if (radius > 63) radius = 63; // max sigma here is 16 -> r=40
    for (int i = 0; i <= radius; i++)
        weights[i] = expf(-((float)i * i) / (2 * sigma * sigma));
    float sum = 0;
    for (int i = 0; i <= radius; i++) sum += weights[i];
    sum = sum * 2 - weights[0];
    for (int i = 0; i <= radius; i++) weights[i] /= sum;
    if ((size_t)w * h > out.size()) out.resize((size_t)w * h);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc[3] = {0, 0, 0};
            for (int i = 0; i <= radius; i++) {
                auto& a = src[min(y + i, h - 1) * (size_t)w + x];
                auto& b = src[max(y - i, 0) * (size_t)w + x];
                for (int k = 0; k < 3; k++) acc[k] += (a[k] + b[k]) * weights[i];
            }
            out[(size_t)y * w + x] = {acc[0], acc[1], acc[2]};
        }
}

static float bloom_gain_global = 6.0f;

static void bloom(const Frame& f, float intensity, float threshold, Img& out_full) {
    int w = f.w, h = f.h;
    int hw = max(w / 2, 1), hh = max(h / 2, 1);
    // All scratch persists across frames (constant sizes per scene). `in`
    // doubles as the downsample target: resize() truncates logically without
    // reallocating, and every consumer only touches [0, size).
    static Img in;
    static Img lv[5];   // blurred mip levels
    static Img fold[5]; // cascade accumulators
    if ((size_t)hw * hh > in.size()) in.resize((size_t)hw * hh);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < hw; x++) {
            float acc[3] = {0, 0, 0};
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    auto& c = f.color[min(y * 2 + dy, h - 1) * (size_t)w + min(x * 2 + dx, w - 1)];
                    float luma = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
                    float kk = clamp((luma - threshold) / 0.01f, 0.0f, 1.0f);
                    float alpha = kk * kk * (3 - 2 * kk);
                    for (int k = 0; k < 3; k++) acc[k] += c[k] * alpha;
                }
            in[(size_t)y * hw + x] = {acc[0] * 0.25f, acc[1] * 0.25f, acc[2] * 0.25f};
        }
    vector<pair<int, int>> sizes;
    int cw = hw, chh = hh;
    for (int i = 0; i < 5; i++) {
        blur_into(in, cw, chh, (float)(1 << i), lv[i]);
        sizes.push_back({cw, chh});
        if (cw <= 2 || chh <= 2) break;
        int nw = max(cw / 2, 1), nh = max(chh / 2, 1);
        in.resize((size_t)nw * nh);
#pragma omp parallel for schedule(static)
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                float acc[3] = {0, 0, 0};
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++)
                        for (int k = 0; k < 3; k++)
                            acc[k] += lv[i][min(y * 2 + dy, chh - 1) * (size_t)cw +
                                           min(x * 2 + dx, cw - 1)][k] * 0.25f;
                in[(size_t)y * nw + x] = {acc[0], acc[1], acc[2]};
            }
        cw = nw;
        chh = nh;
    }
    float weights[5] = {1.0f, 0.8f, 0.6f, 0.4f, 0.2f};
    float k = intensity * bloom_gain_global;
    // Cascade: fold the small mips into their parents (bilinear + add) and
    // touch full resolution exactly once, instead of upsampling all five
    // levels to full res. Same soft result, a fifth of the traffic.
    auto upsample_into = [](Img& dst, int dw, int dh, const Img& src, int sw, int sh,
                            float mul) {
        if ((size_t)dw * dh > dst.size()) dst.resize((size_t)dw * dh);
#pragma omp parallel for schedule(static)
        for (int y = 0; y < dh; y++) {
            float fy = ((float)y + 0.5f) * sh / dh - 0.5f;
            int y0 = (int)floorf(max(fy, 0.0f));
            int y1 = min(y0 + 1, sh - 1);
            float ty = clamp(fy - y0, 0.0f, 1.0f);
            for (int x = 0; x < dw; x++) {
                float fx = ((float)x + 0.5f) * sw / dw - 0.5f;
                int x0 = (int)floorf(max(fx, 0.0f));
                int x1 = min(x0 + 1, sw - 1);
                float tx = clamp(fx - x0, 0.0f, 1.0f);
                auto& a = src[(size_t)y0 * sw + x0];
                auto& b = src[(size_t)y0 * sw + x1];
                auto& c = src[(size_t)y1 * sw + x0];
                auto& d = src[(size_t)y1 * sw + x1];
                auto& o = dst[(size_t)y * dw + x];
                for (int kk = 0; kk < 3; kk++) {
                    float top = a[kk] + (b[kk] - a[kk]) * tx;
                    float bot = c[kk] + (d[kk] - c[kk]) * tx;
                    o[kk] = (top + (bot - top) * ty) * mul;
                }
            }
        }
    };
    size_t n = sizes.size();
    {
        auto [sw, sh] = sizes[n - 1];
        upsample_into(fold[n - 1], sw, sh, lv[n - 1], sw, sh,
                      weights[min(n - 1, (size_t)4)] * k);
    }
    for (int li = (int)n - 2; li >= 0; li--) {
        auto [dw, dh] = sizes[li];
        auto [sw, sh] = sizes[li + 1];
        upsample_into(fold[li], dw, dh, fold[li + 1], sw, sh, 1.0f);
        // add this level's own (weighted) energy
#pragma omp parallel for schedule(static)
        for (long i = 0; i < (long)fold[li].size(); i++)
            for (int kk = 0; kk < 3; kk++)
                fold[li][i][kk] += lv[li][i][kk] * weights[min((size_t)li, (size_t)4)] * k;
    }
    upsample_into(out_full, w, h, fold[0], sizes[0].first, sizes[0].second, 1.0f);
}

static void dof(Frame& f, float focus, float aperture, float maxblur) {
    int w = f.w, h = f.h;
    int hw = max(w / 2, 1), hh = max(h / 2, 1);
    // Persist across frames (constant sizes per scene; was ~70MB churn/frame).
    // The final pass reads f.color[i] and writes f.color[i] — per-pixel
    // read-before-write, safe in place.
    static Img half, b1, b2, b3;
    static vector<float> coc;
    if ((size_t)hw * hh > half.size()) half.resize((size_t)hw * hh);
    if ((size_t)hw * hh > coc.size()) coc.resize((size_t)hw * hh);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < hw; x++) {
            float acc[3] = {0, 0, 0}, cacc = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    size_t si = min(y * 2 + dy, h - 1) * (size_t)w + min(x * 2 + dx, w - 1);
                    auto& c = f.color[si];
                    for (int k = 0; k < 3; k++) acc[k] += c[k] * 0.25f;
                    float zc = f.depth[si];
                    float coc_uv = isfinite(zc)
                                       ? clamp((focus - zc) * aperture, -maxblur, maxblur)
                                       : 0.0f;
                    cacc += fabsf(coc_uv);
                }
            half[(size_t)y * hw + x] = {acc[0], acc[1], acc[2]};
            coc[(size_t)y * hw + x] = cacc * 0.25f;
        }
    blur_into(half, hw, hh, 1.0f, b1);
    blur_into(half, hw, hh, 4.0f, b2);
    blur_into(half, hw, hh, 8.0f, b3);
    float coc_px = (float)w;
#pragma omp parallel for schedule(static)
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int hx = min(x / 2, hw - 1), hy = min(y / 2, hh - 1);
            float c01 = clamp(coc[(size_t)hy * hw + hx] * coc_px / 15.0f, 0.0f, 1.0f);
            auto& src = f.color[(size_t)y * w + x];
            size_t i = (size_t)hy * hw + hx;
            float w0, w1, w2, w3;
            if (c01 < 0.33f) {
                float k = c01 / 0.33f;
                w0 = 1 - k;
                w1 = k;
                w2 = w3 = 0;
            } else if (c01 < 0.66f) {
                float k = (c01 - 0.33f) / 0.33f;
                w0 = w3 = 0;
                w1 = 1 - k;
                w2 = k;
            } else {
                float k = (c01 - 0.66f) / 0.34f;
                w0 = w1 = 0;
                w2 = 1 - k;
                w3 = k;
            }
            auto& o = src;
            float r[3];
            for (int kk = 0; kk < 3; kk++)
                r[kk] = src[kk] * w0 + b1[i][kk] * w1 + b2[i][kk] * w2 + b3[i][kk] * w3;
            for (int kk = 0; kk < 3; kk++) o[kk] = r[kk];
        }
}

// sRGB encode LUT: 4097 entries cover [0,1] finer than the 8-bit output, so
// the LUT is visually exact while replacing two million powf calls a frame.
static const float* srgb_lut() {
    static vector<float> lut = [] {
        vector<float> v(4097);
        for (int i = 0; i <= 4096; i++) {
            float x = (float)i / 4096;
            v[i] = x <= 0.0031308f ? x * 12.92f : 1.055f * powf(x, 1 / 2.4f) - 0.055f;
        }
        return v;
    }();
    return lut.data();
}
static inline float srgb_encode_lut(float v) {
    const float* lut = srgb_lut();
    float x = clamp(v, 0.0f, 1.0f) * 4096.0f;
    int i = (int)x;
    if (i > 4095) i = 4095;
    float f = x - i;
    return lut[i] + (lut[i + 1] - lut[i]) * f;
}

// ACES fit LUT: the rational fit is monotonic on [0,∞); a 4096-step table
// over [0,8] keeps output error ~1e-4 (far below 8-bit significance) and
// replaces ~6M rational evaluations a frame. Above 8 (rare flash/specular
// spikes) the exact expression is kept so the tail stays correct.
static const float* aces_lut() {
    static vector<float> lut = [] {
        vector<float> v(4097);
        for (int i = 0; i <= 4096; i++) {
            float x = (float)i / 4096 * 8.0f;
            v[i] = (x * (x + 0.0245786f) - 0.000090537f) /
                   (x * (0.983729f * x + 0.4329510f) + 0.238081f);
        }
        return v;
    }();
    return lut.data();
}
static inline float aces_fit(float v) {
    const float* lut = aces_lut();
    if (v <= 0.0f) return lut[0]; // matches max(v,0) of the reference fit
    if (v >= 8.0f)
        return (v * (v + 0.0245786f) - 0.000090537f) /
               (v * (0.983729f * v + 0.4329510f) + 0.238081f);
    float x = v * (4096.0f / 8.0f);
    int i = (int)x;
    float f = x - i;
    return lut[i] + (lut[i + 1] - lut[i]) * f;
}

static array<float, 3> tonemap_srgb(const array<float, 3>& c) {
    const float M[3][3] = {{0.59719f, 0.07600f, 0.02840f},
                           {0.35458f, 0.90834f, 0.13383f},
                           {0.04823f, 0.01566f, 0.83777f}};
    const float MO[3][3] = {{1.60475f, -0.10208f, -0.00327f},
                            {-0.53108f, 1.10813f, -0.07276f},
                            {-0.07367f, -0.00605f, 1.07602f}};
    float fitted[3];
    for (int i = 0; i < 3; i++)
        fitted[i] = aces_fit(c[0] * M[i][0] + c[1] * M[i][1] + c[2] * M[i][2]);
    array<float, 3> out;
    for (int i = 0; i < 3; i++) {
        float v = clamp(fitted[0] * MO[i][0] + fitted[1] * MO[i][1] + fitted[2] * MO[i][2],
                        0.0f, 1.0f);
        out[i] = srgb_encode_lut(v);
    }
    return out;
}

static void grade_into(const Img& src, int w, int h, float chromatic, float vignette,
                       float saturation, float flash, Img& out) {
    if ((size_t)w * h > out.size()) out.resize((size_t)w * h);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
            float cx = u - 0.5f, cy = v - 0.5f;
            float r2 = cx * cx + cy * cy;
            float k = chromatic * (0.35f + r2 * 2.4f);
            auto sample = [&](float du, float dv, int ch) {
                int sx = clamp((int)(((u + du) * w)), 0, w - 1);
                int sy = clamp((int)(((v + dv) * h)), 0, h - 1);
                return src[(size_t)sy * w + sx][ch];
            };
            array<float, 3> col = {sample(cx * k, cy * k, 0), sample(0, 0, 1),
                                   sample(-cx * k, -cy * k, 2)};
            float l = 0.2126f * col[0] + 0.7152f * col[1] + 0.0722f * col[2];
            for (auto& c : col) c = l + (c - l) * saturation;
            float rad = clamp(sqrtf(cx * cx + cy * cy) * 1.45f, 0.0f, 1.0f);
            for (auto& c : col) {
                c *= 1 - vignette * rad * rad;
                c += (1 - c) * clamp(flash, 0.0f, 1.0f);
            }
            out[(size_t)y * w + x] = col;
        }
}

// Full post chain, in place on f.color (safe: both paths consume the result
// before the next frame re-renders it; the dome overwrites or the clear
// resets every pixel). All intermediates persist across frames.
static void finish(Frame& f, const json& meta, float flash, vector<uint8_t>& bytes) {
    int w = f.w, h = f.h;
    if (meta.at("bloom").value("enabled", false)) {
        static Img bl;
        bloom(f, meta.at("bloom").at("intensity").get<float>(),
              meta.at("bloom").at("threshold").get<float>(), bl);
#pragma omp parallel for schedule(static)
        for (long i = 0; i < (long)f.color.size(); i++) {
            f.color[i][0] += bl[i][0];
            f.color[i][1] += bl[i][1];
            f.color[i][2] += bl[i][2];
        }
    }
    auto& dof_j = meta.at("dof");
    if (dof_j.value("enabled", false)) {
        float aperture = 0.00017f * (4.0f / max(dof_j.at("aperture_f_stops").get<float>(), 0.1f));
        float maxblur = 0.008f * max(dof_j.at("focal_radius").get<float>(), 0.001f);
        dof(f, dof_j.at("focal_distance").get<float>(), aperture, maxblur);
    }
    static Img srgb;
    if ((size_t)w * h > srgb.size()) srgb.resize((size_t)w * h);
#pragma omp parallel for schedule(static)
    for (long i = 0; i < (long)f.color.size(); i++) srgb[i] = tonemap_srgb(f.color[i]);
    const Img* packed_src = &srgb;
    if (meta.value("fx", json::object()).value("enabled", false)) {
        auto& fx = meta.at("fx");
        static Img graded;
        grade_into(srgb, w, h, fx.value("chromatic", 0.0f), fx.value("vignette", 0.0f),
                   fx.value("saturation", 1.0f), flash, graded);
        packed_src = &graded;
    }
    bytes.resize((size_t)w * h * 3);
    const Img& src = *packed_src;
    for (size_t i = 0; i < src.size(); i++)
        for (int k = 0; k < 3; k++)
            bytes[i * 3 + k] = (uint8_t)(clamp(src[i][k], 0.0f, 1.0f) * 255 + 0.5f);
}

static float mat_outline_width(const json& spec) {
    auto& mat = spec.at("material");
    if (mat.value("type", "neon") != "neon") return 0;
    auto it = mat.find("outline");
    if (it == mat.end()) return 0;
    return it->value("width", 0.02f);
}

// ---------------------------------------------------------------------------
// gen: LRC → scene JSON (port of js/lrc.js + js/lrc2scene.mjs)
//
//   chamina-cpp gen song.lrc -o song.json [flags like node js/lrc2scene.mjs]
//
// Covers the single-layout default path: LRC parsing (incl. {e:...} line tags
// and <mm:ss.xx> word-timed stamps), entrance/fade/jitter/wave text blocks,
// title & outro cards, orbit retune, camera move/shake, background looks and
// cuts, the fx rack and the colour palette. --layout field / --groups auto
// (js/layout.mjs) are not ported yet and are rejected up front.
//
// Output aims at byte parity with node's JSON.stringify(scene, null, 2):
// numbers print through js_dtoa (ECMAScript Number→String rules), keys keep
// JS insertion order (ordered_json), and template floats like "1.0" print
// as "1" the way JS sees them. Known deviations: Math.tan/strtod may differ
// from V8 by 1 ulp (fit_width), and localeCompare ties fall back to codepoint
// order.
// ---------------------------------------------------------------------------

using ojson = nlohmann::ordered_json;

// JS Math.round: half rounds toward +Infinity (floor(x+0.5)); std::round
// rounds half away from zero and would disagree on negatives.
static double js_round(double x) { return floor(x + 0.5); }
static double round3(double v) { return js_round(v * 1000.0) / 1000.0; }

static double js_max(double a, double b) {
    if (isnan(a) || isnan(b)) return numeric_limits<double>::quiet_NaN();
    return a > b ? a : b;
}
static double js_min(double a, double b) {
    if (isnan(a) || isnan(b)) return numeric_limits<double>::quiet_NaN();
    return a < b ? a : b;
}

// One UTF-8 codepoint at i (invalid bytes pass through as themselves).
static uint32_t utf8_cp(const string& s, size_t i, size_t& len) {
    unsigned char b = (unsigned char)s[i];
    len = 1;
    if (b < 0x80) return b;
    int extra = 0;
    uint32_t cp = 0;
    if ((b & 0xE0) == 0xC0) { extra = 1; cp = b & 0x1F; }
    else if ((b & 0xF0) == 0xE0) { extra = 2; cp = b & 0x0F; }
    else if ((b & 0xF8) == 0xF0) { extra = 3; cp = b & 0x07; }
    else return b;
    if (i + extra >= s.size()) return b;
    for (int k = 1; k <= extra; k++) {
        unsigned char c = (unsigned char)s[i + k];
        if ((c & 0xC0) != 0x80) { len = 1; return b; }
        cp = (cp << 6) | (c & 0x3F);
    }
    len = (size_t)extra + 1;
    return cp;
}

// ECMAScript WhiteSpace + LineTerminator (what String.trim strips).
static bool js_is_ws(uint32_t cp) {
    switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
    case 0xA0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return true;
    }
    return cp >= 0x2000 && cp <= 0x200A;
}

static string js_trim(const string& s) {
    size_t b = 0, e = s.size();
    while (b < e) {
        size_t l;
        if (js_is_ws(utf8_cp(s, b, l))) b += l;
        else break;
    }
    size_t end = b, i = b;
    while (i < e) {
        size_t l;
        uint32_t cp = utf8_cp(s, i, l);
        if (!js_is_ws(cp)) end = i + l;
        i += l;
    }
    return s.substr(b, end - b);
}

// Regex-level \s (single codepoint) used by the tag matchers.
static size_t js_ws_at(const string& s, size_t i) {
    size_t l;
    uint32_t cp = utf8_cp(s, i, l);
    return js_is_ws(cp) ? l : 0;
}

static bool js_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static bool js_word(unsigned char c) {  // \w
    return js_alpha(c) || (c >= '0' && c <= '9') || c == '_';
}

static string to_lower_ascii(string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

// JS Number(str): surrounding whitespace ignored, "" → 0, full-string float
// syntax otherwise, else NaN. (strtod also accepts "inf"; JS only "Infinity"
// — divergence confined to garbage input.)
static double parse_num(const string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    if (b == e) return 0.0;
    string t = s.substr(b, e - b);
    const char* c = t.c_str();
    char* end = nullptr;
    double v = strtod(c, &end);
    if (end != c + t.size()) return numeric_limits<double>::quiet_NaN();
    return v;
}

// JS optionals default to `null`; Number.isFinite(null) is false while
// clamp(Number(null)) clamps 0 — both behaviours needed, so model the two
// states instead of one sentinel.
struct OptNum {
    bool is_null;
    double v;
};
static OptNum optn(double v) { return {false, v}; }
static const OptNum OPT_NULL{true, 0.0};
static bool opt_finite(const OptNum& o) { return !o.is_null && isfinite(o.v); }
static bool opt_gt0(const OptNum& o) { return !o.is_null && o.v == o.v && o.v > 0; }
// (args.x || 0)
static double opt_num0(const OptNum& o) {
    return (o.is_null || o.v != o.v || o.v == 0) ? 0.0 : o.v;
}
// JS clamp(): Number(v) — null → 0 (finite!), NaN → fallback.
static double opt_clamp(const OptNum& o, double lo, double hi, double fb) {
    if (o.is_null) return min(hi, max(lo, 0.0));
    if (o.v != o.v) return fb;
    return min(hi, max(lo, o.v));
}
// JS clampNum() in lrc.js has the same Number() semantics.
static double clamp_num(double v, double fb, double lo, double hi) {
    if (v != v) return fb;
    return min(hi, max(lo, v));
}

// CLI options for `gen` — every JS-default `null` number is modelled as
// OPT_NULL so Number.isFinite / clamp(Number(v)) behave exactly like node.
struct GenArgs {
    string input, out;
    string templ = "scenes/gift.json";  // node resolves ROOT/scenes/gift.json
    OptNum size = optn(1.4), y = optn(-0.4), depth = optn(0.09);
    string color = "#ffffff";
    OptNum strength = optn(1.6);
    string outline;
    OptNum outline_width = optn(0.02);
    OptNum bloom = optn(0.06);
    string entrance = "slide";
    string layout = "single", groups = "off";
    OptNum jitter = OPT_NULL;
    OptNum jitter_rot = optn(0), jitter_freq = optn(5);
    bool jitter_hit = false;
    OptNum jitter_hit_dur = optn(0.55);
    OptNum wave = OPT_NULL;
    OptNum wave_freq = optn(1.6), wave_spacing = optn(0.55);
    bool wave_hit = false;
    OptNum wave_hit_dur = optn(2.0);
    OptNum dolly = OPT_NULL, crane = OPT_NULL, move_at = OPT_NULL, move_dur = OPT_NULL;
    OptNum shake = OPT_NULL;
    OptNum shake_roll = optn(0.008), shake_freq = optn(3.5);
    bool shake_hit = false;
    OptNum shake_hit_dur = optn(0.55);
    OptNum fit_width = OPT_NULL;
    OptNum offset = optn(0), fade_in = optn(0.3), fade_out = optn(0.45);
    OptNum stagger = optn(0.04), tail = optn(3);
    OptNum duration = OPT_NULL;
    OptNum yaw_drift = optn(0.6), radius = OPT_NULL;
    bool keep_camera = false, keep_open = false;
    OptNum outro_at = OPT_NULL, outro_out = OPT_NULL;
    string outro_text, outro_subtitle, outro_wish;
    bool has_outro_text = false, has_outro_subtitle = false;
    string bg_look;
    vector<string> bg_cut;
    OptNum bg_fade = optn(1.5);
    bool fx = false;
    OptNum fx_flash = OPT_NULL, fx_punch = OPT_NULL, fx_vignette = OPT_NULL,
           fx_chromatic = OPT_NULL, fx_sat = OPT_NULL;
    string palette;
    OptNum roll_power = optn(0);
    string energy_file;
    vector<string> force_whip;
    OptNum fps = OPT_NULL;
    string title, subtitle;
    OptNum title_in = optn(1.2), title_out = OPT_NULL, title_size = optn(3);
    bool help = false;
};

static bool is_hex6(const string& s) {
    if (s.size() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < 7; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

// The clock regex shared by line tags and glyph stamps:
//   [< (1..3 digits) ':' (1..2 digits) [':'|'.' (1..3 digits)] >]
// matched with real regex backtracking: a bounded digit run that overshoots
// the terminator fails the whole match rather than truncating ("[00:16.1234]"
// is not a time tag).
static size_t digit_run(const string& s, size_t i) {
    size_t n = 0;
    while (i + n < s.size() && s[i + n] >= '0' && s[i + n] <= '9') n++;
    return n;
}
static double digits_val(const string& s, size_t pos, size_t n) {
    long long v = 0;
    for (size_t i = 0; i < n; i++) v = v * 10 + (s[pos + i] - '0');
    return (double)v;
}
static bool match_clock(const string& s, size_t pos, char open, char term,
                        double& sec, size_t& len) {
    if (pos >= s.size() || s[pos] != open) return false;
    size_t run = digit_run(s, pos + 1);
    if (!run) return false;
    size_t mlen = 0;
    for (size_t k = min(run, (size_t)3); k >= 1; k--)
        if (pos + 1 + k < s.size() && s[pos + 1 + k] == ':') { mlen = k; break; }
    if (!mlen) return false;
    double minutes = digits_val(s, pos + 1, mlen);
    size_t i = pos + 1 + mlen + 1;  // past ':'
    size_t run2 = digit_run(s, i);
    if (!run2) return false;
    for (size_t k = min(run2, (size_t)2); k >= 1; k--) {
        size_t j = i + k;
        if (j >= s.size()) continue;
        char c = s[j];
        if (c == term) {
            sec = minutes * 60.0 + digits_val(s, i, k);
            len = j + 1 - pos;
            return true;
        }
        if (c == '.' || c == ':') {
            // The fraction can only be the full digit run after the separator;
            // any shorter prefix ends on a digit, never the terminator.
            size_t fr = digit_run(s, j + 1);
            if (fr >= 1 && fr <= 3 && j + 1 + fr < s.size() && s[j + 1 + fr] == term) {
                double frac = digits_val(s, j + 1, fr) / pow(10.0, (double)fr);
                sec = minutes * 60.0 + digits_val(s, i, k) + frac;
                len = j + 1 + fr + 1 - pos;
                return true;
            }
        }
    }
    return false;
}

struct LrcLine {
    double time = 0;
    string text;
    bool has_tags = false;
    map<string, string> tags;  // keys lowercased at parse, never serialized
    bool has_ct = false;
    vector<double> ct;         // one absolute time per codepoint
};
struct LrcParsed {
    map<string, string> meta;
    double offset_ms = 0;
    vector<LrcLine> lines;
};

static bool match_meta(const string& s, string& key, string& value) {
    // META_RE = /^\[([A-Za-z_][\w#-]*):(.*)\]\s*$/
    if (s.size() < 3 || s[0] != '[') return false;
    if (!(js_alpha((unsigned char)s[1]) || s[1] == '_')) return false;
    size_t i = 2;
    while (i < s.size() && (js_word((unsigned char)s[i]) || s[i] == '#' || s[i] == '-')) i++;
    if (i >= s.size() || s[i] != ':') return false;
    size_t close = s.rfind(']');
    if (close == string::npos || close <= i) return false;
    for (size_t k = close + 1; k < s.size(); k++)
        if (!js_ws_at(s, k)) return false;
    key = s.substr(1, i - 1);
    value = s.substr(i + 1, close - i - 1);
    return true;
}

// TAG_RE — leading {key:value} overrides, key lowercased, value trimmed.
static bool match_tag(const string& s, string& key, string& val, size_t& len) {
    if (s.empty() || s[0] != '{') return false;
    size_t i = 1;
    if (i >= s.size() || !(js_alpha((unsigned char)s[i]) || s[i] == '_')) return false;
    i++;
    while (i < s.size() && (js_word((unsigned char)s[i]) || s[i] == '-')) i++;
    size_t j = i;
    while (j < s.size()) {
        size_t l = js_ws_at(s, j);
        if (!l) break;
        j += l;
    }
    if (j >= s.size() || (s[j] != ':' && s[j] != '=')) return false;
    j++;
    while (j < s.size()) {
        size_t l = js_ws_at(s, j);
        if (!l) break;
        j += l;
    }
    size_t vs = j;
    while (j < s.size() && s[j] != '{' && s[j] != '}') j++;
    if (j >= s.size() || s[j] != '}') return false;
    key = to_lower_ascii(s.substr(1, i - 1));
    val = js_trim(s.substr(vs, j - vs));
    len = j + 1;
    return true;
}

static pair<map<string, string>, string> split_tags(const string& text) {
    map<string, string> tags;
    string rest = text;
    string key, val;
    size_t len;
    while (match_tag(rest, key, val, len)) {
        tags[key] = val;
        rest = rest.substr(len);
    }
    return {move(tags), js_trim(rest)};
}

// splitGlyphTimes — word-timed LRC: <mm:ss.xx> stamps in front of a glyph or a
// run of glyphs; characters inherit the last stamp, untagged text falls back
// to the line cue.
static void split_glyph_times(const string& src, double line_time, string& text,
                              vector<double>& ct, bool& saw) {
    size_t i = 0;
    bool have_at = false;
    double at = 0;
    while (i < src.size()) {
        double stamp;
        size_t slen;
        if (src[i] == '<' && match_clock(src, i, '<', '>', stamp, slen)) {
            at = stamp;
            have_at = true;
            saw = true;
            i += slen;
            continue;
        }
        double when = have_at ? at : line_time;
        if (src[i] == '<') {  // '.' matches the lone '<' as one unit
            text += '<';
            ct.push_back(when);
            i += 1;
            continue;
        }
        while (i < src.size() && src[i] != '<') {
            size_t l;
            utf8_cp(src, i, l);  // one entry per codepoint
            text += src.substr(i, l);
            ct.push_back(when);
            i += l;
        }
    }
}

static LrcParsed parse_lrc(const string& src) {
    LrcParsed p;
    size_t pos = 0;
    while (pos <= src.size()) {
        size_t nl = src.find('\n', pos);
        string raw = src.substr(pos, (nl == string::npos ? src.size() : nl) - pos);
        pos = nl == string::npos ? src.size() + 1 : nl + 1;
        string line = js_trim(raw);
        if (line.empty()) continue;

        // Collect every leading timestamp before deciding what this row is.
        vector<double> times;
        size_t rest_i = 0;
        for (;;) {
            double sec;
            size_t len;
            if (!match_clock(line, rest_i, '[', ']', sec, len)) break;
            times.push_back(sec);
            rest_i += len;
        }
        if (!times.empty()) {
            // Strip a leading run of whitespace/colons (JS /^[\s:]+/), then the
            // {…} overrides.
            string body = line.substr(rest_i);
            size_t lb = 0;
            while (lb < body.size()) {
                size_t l = js_ws_at(body, lb);
                if (!l && body[lb] != ':') break;
                lb += l ? l : 1;
            }
            body = js_trim(body.substr(lb));
            auto [tags, text0] = split_tags(body);
            string text;
            vector<double> ct;
            bool saw = false;
            split_glyph_times(text0, times[0], text, ct, saw);
            for (double t : times) {
                LrcLine e;
                e.time = t;
                e.text = text;
                if (!tags.empty()) {
                    e.has_tags = true;
                    e.tags = tags;
                }
                if (saw) {
                    // A repeated stamp replays the same run on this copy's cue.
                    e.has_ct = true;
                    e.ct = ct;
                    double shift = t - times[0];
                    if (shift != 0)
                        for (auto& v : e.ct) v += shift;
                }
                p.lines.push_back(move(e));
            }
            continue;
        }

        string mk, mv;
        if (match_meta(line, mk, mv)) {
            string key = to_lower_ascii(mk);
            string value = js_trim(mv);
            p.meta[key] = value;
            if (key == "offset") {
                double n = parse_num(value);
                if (isfinite(n)) p.offset_ms = n;
            }
        }
    }

    // JS sorts with `a.time - b.time || a.text.localeCompare(b.text)`; the
    // collation is locale-dependent, so exact ties (same timestamp) fall back
    // to codepoint order here.
    stable_sort(p.lines.begin(), p.lines.end(), [](const LrcLine& a, const LrcLine& b) {
        if (a.time != b.time) return a.time < b.time;
        return a.text < b.text;
    });
    return p;
}

static vector<LrcLine> apply_offset(const LrcParsed& p, bool has_given, double given) {
    // applyOffset: shift = (Number(offsetMs) || 0) / 1000 — a positive
    // [offset:+500] tag displays the lyrics 500 ms earlier.
    double ms = has_given ? given : p.offset_ms;
    double shift = (ms == ms && ms != 0 ? ms : 0.0) / 1000.0;
    vector<LrcLine> out;
    for (const auto& l : p.lines) {
        LrcLine o;
        o.time = l.time - shift;
        o.text = l.text;
        o.has_tags = l.has_tags;
        o.tags = l.tags;
        if (l.has_ct) {
            o.has_ct = true;
            for (double v : l.ct) o.ct.push_back(v - shift);
        }
        out.push_back(move(o));
    }
    return out;
}

static double suggest_duration(const vector<LrcLine>& lines, double tail) {
    double last = lines.empty() ? 0.0 : lines.back().time;
    return js_max(4.0, ceil(last + tail));
}

// ---------------------------------------------------------------------------
// gen: entrances
// ---------------------------------------------------------------------------

static void check_entrance(const string& mode) {
    static const char* ENTRANCES[] = {"none", "slide", "fall", "pop", "overshoot"};
    for (const char* e : ENTRANCES)
        if (mode == e) return;
    throw runtime_error("unknown entrance \"" + mode + "\" (have: none, slide, fall, pop, overshoot)");
}

// The entrance for one line: its `{e:fall}` tag if present, else the
// scene-wide default. A typo'd mode throws — a whole PV line would be off.
static string line_entrance(const LrcLine& line, const string& fallback) {
    if (!line.has_tags) return fallback;
    auto it = line.tags.find("e");
    if (it == line.tags.end() || it->second.empty()) return fallback;
    string mode = to_lower_ascii(js_trim(it->second));
    check_entrance(mode);
    return mode;
}

// ---------------------------------------------------------------------------
// gen: per-line colour palette (hexHsl / hslHex / applyPalette)
// ---------------------------------------------------------------------------

struct Hsl {
    double h, s, l;
};

// #rrggbb → {h, s, l} in 0..1. Plain enough for tinting rims.
static Hsl hex_hsl(const string& hex) {
    string h = hex.empty() ? "#ffffff" : hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    long long n = 0;
    for (size_t i = 0; i < min(h.size(), (size_t)6); i++) {
        char c = h[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;  // parseInt stops at the first invalid character
        n = n * 16 + d;
    }
    double r = ((n >> 16) & 255) / 255.0;
    double g = ((n >> 8) & 255) / 255.0;
    double b = (n & 255) / 255.0;
    double mx = max(r, max(g, b)), mn = min(r, min(g, b));
    double l = (mx + mn) / 2;
    if (mx == mn) return {0, 0, l};
    double d = mx - mn;
    double s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
    double hh;
    if (mx == r) hh = ((g - b) / d + (g < b ? 6 : 0)) / 6;
    else if (mx == g) hh = ((b - r) / d + 2) / 6;
    else hh = ((r - g) / d + 4) / 6;
    return {hh, s, l};
}

static double hcl(double x) { return js_round(min(255.0, max(0.0, x)) * 255.0); }

static double hue2rgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 1.0 / 2) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

static string rgb_hex6(int r, int g, int b) {
    char buf[16];
    snprintf(buf, sizeof buf, "#%06x", (r << 16) | (g << 8) | b);
    return buf;
}

// {h, s, l} in 0..1 → #rrggbb.
static string hsl_hex(double h, double s, double l) {
    double H = fmod(fmod(h, 1.0) + 1.0, 1.0);
    double S = min(1.0, max(0.0, s == s ? s : 0.0));
    double L = min(1.0, max(0.0, l == l ? l : 0.0));
    if (S == 0) {
        int v = (int)hcl(L);
        return rgb_hex6(v, v, v);
    }
    double q = L < 0.5 ? L * (1 + S) : L + S - L * S;
    double p = 2 * L - q;
    int r = (int)hcl(hue2rgb(p, q, H + 1.0 / 3.0));
    int g = (int)hcl(hue2rgb(p, q, H));
    int b = (int)hcl(hue2rgb(p, q, H - 1.0 / 3.0));
    return rgb_hex6(r, g, b);
}

// Cycle a romantic palette across the lyric blocks; the rim follows each
// line's tint, hue-shifted a little and pushed darker and more saturated.
static void apply_palette(vector<ojson>& texts, const string& hexes) {
    vector<string> pal;
    size_t pos = 0;
    while (pos <= hexes.size()) {
        size_t c = hexes.find(',', pos);
        string part = js_trim(hexes.substr(pos, (c == string::npos ? hexes.size() : c) - pos));
        if (is_hex6(part)) pal.push_back(part);
        if (c == string::npos) break;
        pos = c + 1;
    }
    if (pal.empty())
        throw runtime_error("--palette wants comma-separated #rrggbb values");
    for (size_t i = 0; i < texts.size(); i++) {
        const string& c = pal[i % pal.size()];
        texts[i]["material"]["color"] = to_lower_ascii(c);
        if (texts[i]["material"].contains("outline")) {
            Hsl hs = hex_hsl(c);
            texts[i]["material"]["outline"]["color"] =
                hsl_hex(hs.h + 0.055, min(1.0, hs.s * 1.6 + 0.18), hs.l * 0.6);
        }
    }
}

// ---------------------------------------------------------------------------
// gen: frame width budget (visibleWidth)
// ---------------------------------------------------------------------------

// Full width of the text plane the camera can see, in world units — the
// budget for fit_width so a line can never run off frame.
static double visible_width(const ojson& scene, const ojson* cam_anim) {
    static const ojson EMPTY_OBJ = ojson::object();
    const ojson& cam = scene.contains("camera") ? scene["camera"] : EMPTY_OBJ;
    const ojson& anim = cam_anim ? *cam_anim : EMPTY_OBJ;
    double radius = 14.0;
    if (anim.contains("radius") && anim["radius"].is_number()) {
        double r = anim["radius"].get<double>();
        radius = r != 0 ? r : 14.0;  // Number(anim.radius) || 14
    }
    if (anim.contains("movement") && anim["movement"].is_object()) {
        const ojson& mv = anim["movement"];
        if (mv.contains("radius_end") && mv["radius_end"].is_number()) {
            double re = mv["radius_end"].get<double>();
            if (isfinite(re)) radius = min(radius, re);
        }
    }
    double fov = 0.8726646;
    if (cam.contains("fov") && cam["fov"].is_number()) {
        double f = cam["fov"].get<double>();
        fov = f != 0 ? f : 0.8726646;
    }
    double w = 1920, h = 1080;
    if (scene.contains("width") && scene["width"].is_number()) {
        double v = scene["width"].get<double>();
        w = v != 0 ? v : 1920;
    }
    if (scene.contains("height") && scene["height"].is_number()) {
        double v = scene["height"].get<double>();
        h = v != 0 ? v : 1080;
    }
    double aspect = w / h;
    return 2.0 * tan(fov / 2.0) * radius * aspect;
}

// ---------------------------------------------------------------------------
// gen: idle wobble (buildJitter) and karaoke bounce (buildWave)
// ---------------------------------------------------------------------------

static double num_or(const ojson& j, const char* key, double d) {
    double v = j.value(key, d);
    return isfinite(v) ? v : d;
}

// `jitter: 0.03` → one `jitter` animation for line i, phased so neighbouring
// lines never wobble in lockstep. `delay` holds it back until the entrance
// has settled. Returns null when absent or amplitude is zero.
static ojson build_jitter(const ojson* jitter, int i, double t) {
    if (!jitter) return ojson(nullptr);
    const ojson& j = *jitter;
    double a = num_or(j, "amplitude", 0.03);
    double amp[3] = {a, a * 0.8, 0.0};
    if (amp[0] == 0 && amp[1] == 0 && amp[2] == 0) return ojson(nullptr);
    ojson out = ojson::object();
    out["type"] = "jitter";
    out["amplitude"] = ojson::array({round3(amp[0]), round3(amp[1]), round3(amp[2])});
    out["frequency"] = round3(num_or(j, "frequency", 5.0));
    out["rotation"] = round3(num_or(j, "rotation", 0.0));
    out["phase"] = round3(i * 1.7);
    out["start_at"] = round3(t + num_or(j, "delay", 0.4));
    out["stagger"] = round3(num_or(j, "stagger", 0.0));
    out["ramp"] = round3(num_or(j, "ramp", 0.5));
    // Written only when asked for, so scenes without the flag stay identical.
    if (j.value("hit", false)) {
        out["hit"] = true;
        out["hit_duration"] = round3(num_or(j, "hit_duration", 0.55));
    }
    return out;
}

// `wave: 0.2` → the karaoke bounce: each character trails the one before it
// by `spacing` radians. Returns null when absent or amplitude is zero.
static ojson build_wave(const ojson* wave, int i, double t) {
    if (!wave) return ojson(nullptr);
    const ojson& w = *wave;
    double amp = num_or(w, "amplitude", 0.2);
    if (amp == 0 || amp != amp) return ojson(nullptr);
    (void)i;
    ojson out = ojson::object();
    out["type"] = "wave";
    out["amplitude"] = round3(amp);
    out["frequency"] = round3(num_or(w, "frequency", 1.6));
    out["spacing"] = round3(num_or(w, "spacing", 0.55));
    out["start_at"] = round3(t + num_or(w, "delay", 0.0));
    out["ramp"] = round3(num_or(w, "ramp", 0.4));
    if (w.value("hit", false)) {
        out["hit"] = true;
        out["hit_duration"] = round3(num_or(w, "hit_duration", 2.0));
    }
    return out;
}

// ---------------------------------------------------------------------------
// gen: lyric text blocks (buildLyricTexts, single layout)
// ---------------------------------------------------------------------------

struct LyricOpts {
    double size = numeric_limits<double>::quiet_NaN();  // clampNum → 1.5
    double y = numeric_limits<double>::quiet_NaN();     // isfinite ? : -0.4
    double depth = numeric_limits<double>::quiet_NaN(); // clampNum → 0.09
    string color;
    double strength = numeric_limits<double>::quiet_NaN();
    double offset = numeric_limits<double>::quiet_NaN(); // isfinite ? : 0
    double fade_in = numeric_limits<double>::quiet_NaN();
    double fade_out = numeric_limits<double>::quiet_NaN();
    double stagger = numeric_limits<double>::quiet_NaN();
    double total = numeric_limits<double>::quiet_NaN();
    string entrance;
    double fit_width = numeric_limits<double>::quiet_NaN();
    const ojson* jitter = nullptr;
    const ojson* wave = nullptr;
};

static void push_entrance(ojson& anims, const string& entrance, double t, double stagger) {
    // Entrance layers land before the fade in the array (JS unshifts); both
    // share start_at so the line slides/drops in while it fades up.
    if (entrance == "slide") {
        ojson a = ojson::object();
        a["type"] = "slide";
        a["delta"] = ojson::array({-6.0, -2.5, 0.0});
        a["duration"] = 0.7;
        a["stagger"] = stagger * 1.5;
        a["ease"] = "ease_out_quad";
        a["start_at"] = t;
        anims.push_back(move(a));
    } else if (entrance == "fall") {
        ojson a = ojson::object();
        a["type"] = "fall";
        a["duration"] = 0.7;
        a["stagger"] = stagger * 1.5;
        a["height"] = 1.4;
        a["gravity"] = -3.0;
        a["drift"] = 0.0;
        a["wobble"] = 0.0;
        a["start_at"] = t;
        anims.push_back(move(a));
    } else if (entrance == "pop") {
        ojson a = ojson::object();
        a["type"] = "pop";
        a["duration"] = 0.5;
        a["stagger"] = stagger * 1.5;
        a["start_at"] = t;
        a["from"] = 0.0;
        a["ease"] = "back_out";
        anims.push_back(move(a));
    } else if (entrance == "overshoot") {
        // Two layers on purpose: slide contributes position, pop scale.
        // back_out overshoots, so each lands past its rest pose.
        ojson a = ojson::object();
        a["type"] = "slide";
        a["delta"] = ojson::array({-7.0, -3.0, 0.0});
        a["duration"] = 0.75;
        a["stagger"] = stagger * 1.5;
        a["ease"] = "back_out";
        a["start_at"] = t;
        anims.push_back(move(a));
        ojson b = ojson::object();
        b["type"] = "pop";
        b["duration"] = 0.6;
        b["stagger"] = stagger * 1.5;
        b["start_at"] = t;
        b["from"] = 0.0;
        b["ease"] = "back_out";
        anims.push_back(move(b));
    }
}

static vector<ojson> build_lyric_texts(const vector<LrcLine>& lines, const LyricOpts& o) {
    double size = clamp_num(o.size, 1.5, 0.2, 12);
    double y = isfinite(o.y) ? o.y : -0.4;
    double depth = clamp_num(o.depth, 0.09, 0.01, 2);
    string color = o.color.empty() ? "#ffffff" : o.color;
    double strength = clamp_num(o.strength, 1.6, 0, 64);
    double offset = isfinite(o.offset) ? o.offset : 0.0;
    double fade_in = clamp_num(o.fade_in, 0.3, 0, 10);
    double fade_out = clamp_num(o.fade_out, 0.45, 0, 10);
    double stagger = clamp_num(o.stagger, 0.04, 0, 1);
    double total = o.total;  // NaN (JS null) when absent
    string base_entrance = o.entrance.empty() ? "none" : o.entrance;
    check_entrance(base_entrance);
    // World-space width budget; Rust resolves it against real font metrics
    // at export time, so we only say how much of the frame a line may use.
    double fit = isfinite(o.fit_width) && o.fit_width > 0 ? o.fit_width
                                                          : numeric_limits<double>::quiet_NaN();

    vector<ojson> out;
    int lyric_index = 0;
    for (size_t i = 0; i < lines.size(); i++) {
        const LrcLine& line = lines[i];
        if (line.text.empty()) continue;

        string entrance = line_entrance(line, base_entrance);
        double t = round3(line.time + offset);
        // Word-timed lines light up glyph by glyph, so the fade-out waits for
        // the last one. (JS slices charTimes to text.length in UTF-16 units;
        // the array is built one-per-codepoint, so the slice never cuts.)
        vector<double> ct;
        bool has_ct = false;
        if (line.has_ct) {
            has_ct = true;
            for (double v : line.ct) ct.push_back(round3(v + offset));
        }
        double last_glyph = has_ct && !ct.empty() ? *max_element(ct.begin(), ct.end()) : t;
        double next_raw = i + 1 < lines.size() ? lines[i + 1].time + offset : total;
        double next = isfinite(next_raw) ? next_raw : t + 3.0;
        // Never let a line outlive the next cue; always leave room to fade.
        // (opts.hold is never set by the CLI.)
        double out_at = round3(js_max(t + 0.1, js_min(next, t + 1e9) - fade_out));
        if (has_ct) out_at = round3(js_max(out_at, last_glyph + 0.15));

        ojson anims = ojson::array();
        push_entrance(anims, entrance, t, stagger);
        ojson fade = ojson::object();
        fade["type"] = "fade";
        fade["duration"] = fade_in;
        fade["stagger"] = stagger;
        fade["start_at"] = t;
        fade["fade_out_at"] = out_at;
        fade["fade_out"] = fade_out;
        anims.push_back(move(fade));
        ojson jit = build_jitter(o.jitter, lyric_index, t);
        if (!jit.is_null()) anims.push_back(move(jit));
        ojson wav = build_wave(o.wave, lyric_index, t);
        if (!wav.is_null()) anims.push_back(move(wav));

        ojson block = ojson::object();
        block["text"] = line.text;
        block["size"] = size;
        block["depth"] = depth;
        block["position"] = ojson::array({0.0, y, 0.0});
        ojson mat = ojson::object();
        mat["type"] = "neon";
        mat["color"] = color;
        mat["strength"] = strength;
        block["material"] = move(mat);
        block["alpha"] = 1.0;
        block["animations"] = move(anims);
        if (isfinite(fit)) block["fit_width"] = fit;
        // One absolute start time per glyph; it rides on the block because
        // fade, entrance and wobble all fall on the same rhythm.
        if (has_ct) block["char_times"] = ct;
        out.push_back(move(block));
        lyric_index += 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// gen: standalone title/subtitle cards (buildTitleBlock)
// ---------------------------------------------------------------------------

struct TitleOpts {
    string text;
    double size = numeric_limits<double>::quiet_NaN();  // clampNum → 3
    double y = numeric_limits<double>::quiet_NaN();     // isfinite ? : 0.6
    double depth = numeric_limits<double>::quiet_NaN(); // clampNum → 0.12
    string color;
    double strength = numeric_limits<double>::quiet_NaN();
    double start_at = numeric_limits<double>::quiet_NaN();
    double out_at = numeric_limits<double>::quiet_NaN();
    double fade_in = numeric_limits<double>::quiet_NaN();  // clampNum → 0.9
    double fade_out = numeric_limits<double>::quiet_NaN(); // clampNum → 0.9
    double stagger = numeric_limits<double>::quiet_NaN();  // clampNum → 0.03
    string entrance;
    double fit_width = numeric_limits<double>::quiet_NaN();
    string outline;
    double outline_width = numeric_limits<double>::quiet_NaN();
};

static ojson build_title_block(const TitleOpts& o) {
    string text = js_trim(o.text);
    if (text.empty()) return ojson(nullptr);

    double size = clamp_num(o.size, 3, 0.2, 12);
    double y = isfinite(o.y) ? o.y : 0.6;
    double depth = clamp_num(o.depth, 0.12, 0.01, 2);
    string color = o.color.empty() ? "#ffffff" : o.color;
    double strength = clamp_num(o.strength, 1.6, 0, 64);
    double start_at = round3(js_max(0.0, isfinite(o.start_at) ? o.start_at : 0.0));
    double fade_in = clamp_num(o.fade_in, 0.9, 0, 10);
    double fade_out = clamp_num(o.fade_out, 0.9, 0, 10);
    double stagger = clamp_num(o.stagger, 0.03, 0, 1);
    bool has_out = isfinite(o.out_at);
    string entrance = o.entrance.empty() ? "none" : o.entrance;
    check_entrance(entrance);

    ojson anims = ojson::array();
    if (entrance == "slide") {
        ojson a = ojson::object();
        a["type"] = "slide";
        a["delta"] = ojson::array({0.0, -1.6, 0.0});
        a["duration"] = 1.2;
        a["stagger"] = stagger;
        a["ease"] = "ease_out_quad";
        a["start_at"] = start_at;
        anims.push_back(move(a));
    } else if (entrance == "fall") {
        ojson a = ojson::object();
        a["type"] = "fall";
        a["duration"] = 1.2;
        a["stagger"] = stagger;
        a["height"] = 1.8;
        a["gravity"] = -3.0;
        a["drift"] = 0.0;
        a["wobble"] = 0.0;
        a["start_at"] = start_at;
        anims.push_back(move(a));
    } else if (entrance == "pop") {
        ojson a = ojson::object();
        a["type"] = "pop";
        a["duration"] = 0.7;
        a["stagger"] = stagger;
        a["start_at"] = start_at;
        a["from"] = 0.0;
        a["ease"] = "back_out";
        anims.push_back(move(a));
    }
    ojson fade = ojson::object();
    fade["type"] = "fade";
    fade["duration"] = fade_in;
    fade["stagger"] = stagger;
    fade["start_at"] = start_at;
    if (has_out && o.out_at > start_at) {
        fade["fade_out_at"] = round3(o.out_at);
        fade["fade_out"] = fade_out;
    }
    anims.push_back(move(fade));

    ojson block = ojson::object();
    block["text"] = text;
    block["size"] = size;
    block["depth"] = depth;
    block["position"] = ojson::array({0.0, y, 0.0});
    ojson mat = ojson::object();
    mat["type"] = "neon";
    mat["color"] = color;
    mat["strength"] = strength;
    block["material"] = move(mat);
    block["alpha"] = 1.0;
    block["animations"] = move(anims);
    if (isfinite(o.fit_width) && o.fit_width > 0) block["fit_width"] = o.fit_width;
    if (!o.outline.empty()) {
        ojson outline = ojson::object();
        outline["color"] = o.outline;
        double w = isfinite(o.outline_width) && o.outline_width != 0 ? o.outline_width : 0.02;
        outline["width"] = w;
        outline["strength"] = 1.0;
        block["material"]["outline"] = move(outline);
    }
    return block;
}

// ---------------------------------------------------------------------------
// gen: named background looks + cuts (buildBackgrounds / parseCut)
// ---------------------------------------------------------------------------

struct Look {
    const char* top;
    const char* bottom;
    const char* accent;
    double nebula, stars, drift;
};
static const map<string, Look> LOOKS = {
    {"night", {"#01040f", "#05060f", "#2f6dff", 0.55, 0.75, 1.0}},
    {"void", {"#000000", "#02030a", "#1b2c6b", 0.30, 0.95, 0.6}},
    {"nebula", {"#0a0320", "#12062e", "#a24bff", 0.85, 0.65, 1.3}},
    {"aurora", {"#001a26", "#001014", "#00ffb3", 0.75, 0.50, 0.9}},
    {"ember", {"#1a0503", "#0a0201", "#ff6a1e", 0.70, 0.35, 1.1}},
    {"ice", {"#041224", "#02060e", "#8fe3ff", 0.45, 0.80, 0.8}},
    {"dawn", {"#2a0a3a", "#4a1420", "#ff9a3c", 0.60, 0.30, 1.0}},
    {"bloom", {"#1a0226", "#26043a", "#ff3ea5", 0.80, 0.50, 1.2}},
};

struct Cut {
    double at = 0;
    bool has_name = false;
    string name;
    bool has_palette = false;
    ojson palette;
};

static vector<string> js_split(const string& s, char sep) {
    vector<string> out;
    size_t pos = 0;
    while (true) {
        size_t f = s.find(sep, pos);
        if (f == string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, f - pos));
        pos = f + 1;
    }
    return out;
}

// <seconds>:<look> or <seconds>:#top/#bottom/#accent[@n,s,d] → one cut.
static Cut parse_cut(const string& raw) {
    // A trailing ` # …` note is allowed; hexes never have whitespace before #.
    string line = raw;
    for (size_t i = 0; i + 1 < line.size(); i++)
        if (js_ws_at(line, i) && line[i + 1] == '#') {
            line = line.substr(0, i);
            break;
        }
    size_t ci = line.rfind(':');
    if (ci == string::npos)
        throw runtime_error("--bg-cut wants <seconds>:<look>, got " + raw);
    double at = parse_num(line.substr(0, ci));
    if (!isfinite(at))
        throw runtime_error("--bg-cut: bad time in " + raw);
    string spec = line.substr(ci + 1);
    Cut c;
    c.at = at;
    if (spec.find('/') == string::npos) {
        c.has_name = true;
        c.name = spec;
        return c;
    }
    auto parts = js_split(spec, '/');
    if (parts.size() != 3)
        throw runtime_error("--bg-cut palette wants top/bottom/accent, got " + raw);
    // The accent carries the sliders: `#4a6cff@0.55,0.85,1.0`. JS destructures
    // split('@'), so extra '@' components are dropped.
    string accent = parts[2], sliders;
    {
        size_t atpos = parts[2].find('@');
        if (atpos != string::npos) {
            accent = parts[2].substr(0, atpos);
            string rest = parts[2].substr(atpos + 1);
            size_t at2 = rest.find('@');
            sliders = at2 == string::npos ? rest : rest.substr(0, at2);
        }
    }
    for (const string* hex : {&parts[0], &parts[1], &accent})
        if (!is_hex6(*hex))
            throw runtime_error("--bg-cut: \"" + *hex + "\" is not #rrggbb in " + raw);
    c.has_palette = true;
    ojson palette = ojson::object();
    palette["top"] = parts[0];
    palette["bottom"] = parts[1];
    palette["accent"] = accent;
    palette["nebula"] = 0.6;
    palette["stars"] = 0.6;
    palette["drift"] = 1.0;
    if (!sliders.empty()) {
        auto sl = js_split(sliders, ',');
        // RATIO(v, lo, hi, fallback): Number("") is 0 — quirk preserved.
        auto ratio = [](double v, double lo, double hi, double fb) {
            if (!isfinite(v)) return fb;
            return min(hi, max(lo, v));
        };
        double n = sl.size() > 0 ? parse_num(sl[0]) : numeric_limits<double>::quiet_NaN();
        double st = sl.size() > 1 ? parse_num(sl[1]) : numeric_limits<double>::quiet_NaN();
        double d = sl.size() > 2 ? parse_num(sl[2]) : numeric_limits<double>::quiet_NaN();
        palette["nebula"] = ratio(n, 0, 1, 0.6);
        palette["stars"] = ratio(st, 0, 1, 0.6);
        palette["drift"] = ratio(d, 0, 4, 1.0);
    }
    c.palette = move(palette);
    return c;
}

// Turn --bg-look / --bg-cut into the background timeline the renderer
// crossfades through; null when neither flag was given.
static ojson build_backgrounds(const vector<string>& bg_cut, const string& bg_look,
                               double bg_fade) {
    struct Entry {
        double at;
        bool has_name;
        string name;
        bool has_palette;
        ojson palette;
    };
    vector<Entry> cuts;
    if (!bg_look.empty()) cuts.push_back({0.0, true, bg_look, false, {}});
    for (const string& raw : bg_cut) {
        Cut c = parse_cut(raw);
        if (c.has_palette)
            cuts.push_back({c.at, false, "", true, move(c.palette)});
        else
            cuts.push_back({c.at, true, c.name, false, {}});
    }
    if (cuts.empty()) return ojson(nullptr);
    // A first cut with no opener still needs something to cut *from*.
    if (cuts[0].at > 0) cuts.insert(cuts.begin(), {0.0, true, string("night"), false, {}});
    for (const Entry& c : cuts)
        if (!c.has_palette && !LOOKS.count(c.name))
            throw runtime_error("unknown look \"" + c.name +
                                "\" (have: night, void, nebula, aurora, ember, ice, dawn, bloom)");
    stable_sort(cuts.begin(), cuts.end(),
                [](const Entry& a, const Entry& b) { return a.at < b.at; });
    double fade = isfinite(bg_fade) ? min(60.0, max(0.0, bg_fade)) : 1.5;
    ojson arr = ojson::array();
    for (size_t i = 0; i < cuts.size(); i++) {
        const Entry& c = cuts[i];
        ojson e = ojson::object();
        e["at"] = i == 0 ? 0.0 : c.at;
        e["fade"] = i == 0 ? 0.0 : fade;
        if (c.has_palette) {
            for (auto& kv : c.palette.items()) e[kv.key()] = kv.value();
        } else {
            const Look& L = LOOKS.at(c.name);
            e["top"] = L.top;
            e["bottom"] = L.bottom;
            e["accent"] = L.accent;
            e["nebula"] = L.nebula;
            e["stars"] = L.stars;
            e["drift"] = L.drift;
        }
        arr.push_back(move(e));
    }
    return arr;
}

// The grade/distortion rack, or nothing so the scene renders untouched.
static ojson build_fx(const OptNum& fx_chromatic, const OptNum& fx_vignette,
                      const OptNum& fx_sat, const OptNum& fx_flash, const OptNum& fx_punch) {
    ojson fx = ojson::object();
    fx["enabled"] = true;
    fx["chromatic"] = opt_clamp(fx_chromatic, 0, 0.05, 0.0016);
    fx["vignette"] = opt_clamp(fx_vignette, 0, 1, 0.55);
    fx["saturation"] = opt_clamp(fx_sat, 0, 4, 1.12);
    fx["flash"] = opt_clamp(fx_flash, 0, 1, 0.22);
    fx["punch"] = opt_clamp(fx_punch, 0, 5, 0.35);
    fx["hit_duration"] = 0.35;
    return fx;
}

// ---------------------------------------------------------------------------
// gen: camera (tuneOrbit / applyCameraMotion)
// ---------------------------------------------------------------------------

// Swap the template's full-turn orbit for a slow symmetric sway about head-on:
// start yawDrift/2 short of straight-on, cover exactly yawDrift over the scene.
static ojson tune_orbit(const ojson& tpl, double duration, const GenArgs& args) {
    const ojson* base =
        tpl.contains("camera_animation") ? &tpl["camera_animation"] : nullptr;
    bool orbit = base && base->is_object() && base->contains("type") &&
                 base->at("type").is_string() && base->at("type").get<string>() == "orbit";
    ojson anim;
    if (orbit) {
        anim = *base;
    } else {
        anim = ojson::object();
        anim["type"] = "orbit";
        anim["radius"] = 14.0;
        anim["elevation"] = 0.09;
        anim["target"] = ojson::array({0.0, 1.0, 0.0});
    }
    if (opt_finite(args.radius) && args.radius.v > 0) anim["radius"] = args.radius.v;
    double drift = opt_finite(args.yaw_drift) ? args.yaw_drift.v : 0.6;
    anim["start_yaw"] = M_PI / 2 - drift / 2;
    anim["speed"] = drift / js_max(duration, 1e-3);
    return anim;
}

// Layer an eased camera move and/or a hand-held shake onto the orbit; with no
// flags the orbit is left untouched, so regenerated scenes stay identical.
static void apply_camera_motion(ojson& anim, double duration, const GenArgs& args) {
    if (!anim.is_object() || !anim.contains("type") || !anim["type"].is_string() ||
        anim["type"].get<string>() != "orbit")
        return;
    bool has_move = opt_finite(args.dolly) || opt_finite(args.crane);
    if (has_move) {
        ojson move = ojson::object();
        move["ease"] = "ease_in_out_cubic";
        if (opt_finite(args.dolly) && args.dolly.v > 0) move["radius_end"] = args.dolly.v;
        if (opt_finite(args.crane)) move["elevation_end"] = args.crane.v;
        move["at"] = opt_finite(args.move_at) ? args.move_at.v : duration * 0.12;
        move["duration"] = opt_finite(args.move_dur) ? args.move_dur.v : duration * 0.76;
        anim["movement"] = move;
    }
    if (opt_finite(args.shake) && args.shake.v > 0) {
        ojson shake = ojson::object();
        shake["amplitude"] = args.shake.v;
        shake["frequency"] = opt_finite(args.shake_freq) ? args.shake_freq.v : 3.5;
        shake["roll"] = opt_finite(args.shake_roll) ? args.shake_roll.v : 0.008;
        shake["at"] = 0.0;
        shake["until"] = nullptr;
        shake["ramp"] = 0.8;
        // Only written when asked for, so scenes without the flag stay identical.
        if (args.shake_hit) {
            shake["hit"] = true;
            shake["hit_duration"] = opt_finite(args.shake_hit_dur) ? args.shake_hit_dur.v : 0.55;
        }
        anim["shake"] = move(shake);
    }
}

// ---------------------------------------------------------------------------
// gen: JS-flavoured JSON serialisation
// ---------------------------------------------------------------------------

// ECMAScript Number → String, the rules JSON.stringify uses for doubles:
// shortest decimal that round-trips, exponent form only outside 1e-7…1e21.
static void js_dtoa(double v, string& out) {
    if (isnan(v) || isinf(v)) {
        out += "null";  // JSON.stringify coerces nonfinite to null
        return;
    }
    if (v == 0) {
        out += "0";  // also -0 → "0"
        return;
    }
    if (v < 0) {
        out += '-';
        v = -v;
    }
    double a = v;
    char buf[64];
    int p;
    for (p = 1; p <= 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, a);
        if (strtod(buf, nullptr) == a) break;
    }
    if (p > 17) snprintf(buf, sizeof buf, "%.16e", a);
    // Split "d[.ddd]e±XX" into digits and n, where value = 0.digits · 10^n.
    string mant = buf;
    size_t epos = mant.find('e');
    if (epos == string::npos) epos = mant.find('E');
    long E = epos == string::npos ? 0 : strtol(mant.c_str() + epos + 1, nullptr, 10);
    string digits;
    for (size_t i = 0; i < (epos == string::npos ? mant.size() : epos); i++)
        if (buf[i] >= '0' && buf[i] <= '9') digits += buf[i];
    long n = E + 1;
    size_t k = digits.size();
    auto put_zeros = [&](long count) {
        for (long z = 0; z < count; z++) out += '0';
    };
    if (k <= (size_t)max(n, 0L) && n <= 21) {
        out += digits;
        put_zeros((long)n - (long)k);
    } else if (0 < n && n <= 21) {
        out += digits.substr(0, (size_t)n);
        out += '.';
        out += digits.substr((size_t)n);
    } else if (-6 < n && n <= 0) {
        out += "0.";
        put_zeros(-n);
        out += digits;
    } else {
        out += digits.substr(0, 1);
        if (k > 1) {
            out += '.';
            out += digits.substr(1);
        }
        out += 'e';
        long ex = n - 1;
        out += ex < 0 ? '-' : '+';
        char eb[16];
        snprintf(eb, sizeof eb, "%ld", ex < 0 ? -ex : ex);
        out += eb;
    }
}

static string js_num_str(double v) {
    string o;
    js_dtoa(v, o);
    return o;
}

static void js_json_str(const string& s, string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\f': out += "\\f"; break;
        case '\r': out += "\\r"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
}

// JSON.stringify(value, null, 2): 2-space indent, empty arrays/objects collapse,
// non-ASCII stays raw UTF-8, no spaces around brackets.
static void js_json(const ojson& j, string& out, int depth) {
    if (j.is_null()) { out += "null"; return; }
    if (j.is_boolean()) { out += j.get<bool>() ? "true" : "false"; return; }
    if (j.is_number_unsigned()) { out += to_string(j.get<uint64_t>()); return; }
    if (j.is_number_integer()) { out += to_string(j.get<int64_t>()); return; }
    if (j.is_number_float()) { js_dtoa(j.get<double>(), out); return; }
    if (j.is_string()) { js_json_str(j.get<string>(), out); return; }
    if (j.is_array()) {
        if (j.empty()) { out += "[]"; return; }
        out += "[\n";
        for (size_t i = 0; i < j.size(); i++) {
            out.append((size_t)depth + 2, ' ');
            js_json(j[i], out, depth + 2);
            if (i + 1 < j.size()) out += ',';
            out += '\n';
        }
        out.append((size_t)depth, ' ');
        out += ']';
        return;
    }
    if (j.empty()) { out += "{}"; return; }
    out += "{\n";
    size_t i = 0, n = j.size();
    for (auto it = j.begin(); it != j.end(); ++it, ++i) {
        out.append((size_t)depth + 2, ' ');
        js_json_str(it.key(), out);
        out += ": ";
        js_json(it.value(), out, depth + 2);
        if (i + 1 < n) out += ',';
        out += '\n';
    }
    out.append((size_t)depth, ' ');
    out += '}';
}

// JS truthiness for template leaf checks (work_dir || default, bloom?, dof?).
static bool js_truthy(const ojson& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) {
        double d = v.get<double>();
        return d == d && d != 0;
    }
    if (v.is_string()) return !v.get<string>().empty();
    return true;  // arrays/objects are truthy even when empty
}

static ojson* find_fade(ojson& block) {
    if (!block.contains("animations")) return nullptr;
    for (auto& an : block["animations"])
        if (an.is_object() && an.contains("type") && an["type"].is_string() &&
            an["type"].get<string>() == "fade")
            return &an;
    return nullptr;
}

static string read_file(const string& path) {
    // MinGW's ifstream(char*) opens through the ANSI codepage and fails on
    // Chinese paths; going through a filesystem::path built from UTF-8 uses
    // the wide opening path instead.
    filesystem::path p = filesystem::u8path(path);
    ifstream f(p, ios::binary);
    if (!f) throw runtime_error("cannot read " + path);
    f.seekg(0, ios::end);
    size_t n = (size_t)f.tellg();
    f.seekg(0);
    string s(n, '\0');
    f.read(&s[0], (streamsize)n);
    if (f.gcount() >= 0 && (size_t)f.gcount() != n) s.resize((size_t)f.gcount());
    return s;
}

// Single-layout gen never spends the roll budget, but the JS CLI still opens
// and validates the envelope — keep the same error behaviour.
static void check_energy_file(const string& path) {
    string src = read_file(path);
    json ej = json::parse(src);
    if (!ej.contains("data") || !ej["data"].is_array() || ej["data"].empty())
        throw runtime_error("--energy-file: " + path + " has no envelope data");
}

// ---------------------------------------------------------------------------
// gen: CLI (parseArgs + main)
// ---------------------------------------------------------------------------

static void gen_usage() {
    fputs(
        "chamina-cpp gen <song.lrc> [options]\n"
        "  LRC → scene JSON, single layout (port of node js/lrc2scene.mjs)\n"
        "  -o, --out <file>       output scene JSON (default <lrc>.json)\n"
        "  -t, --template <file>  stage to reuse (default scenes/gift.json, cwd-relative)\n"
        "  text:   --size --y --depth --color --strength --outline --outline-width\n"
        "  motion: --entrance --jitter --jitter-rot --jitter-freq --jitter-hit\n"
        "          --jitter-hit-dur --wave --wave-freq --wave-spacing --wave-hit\n"
        "          --wave-hit-dur\n"
        "  camera: --dolly --crane --move-at --move-dur --shake --shake-roll\n"
        "          --shake-freq --shake-hit --shake-hit-dur --yaw-drift --radius\n"
        "          --keep-camera\n"
        "  timing: --offset --fade-in --fade-out --stagger --tail --duration\n"
        "          --fit-width --keep-open\n"
        "  cards:  --title --subtitle --title-in --title-out --title-size\n"
        "          --outro-at --outro-text --outro-subtitle --outro-wish --outro-out\n"
        "  stage:  --bg-look --bg-cut --bg-fade --fx --fx-flash --fx-punch\n"
        "          --fx-vignette --fx-chromatic --fx-sat --bloom --fps --palette\n"
        "  not ported yet (accepted, no-op in single layout): --roll-power\n"
        "          --force-whip --energy-file; --layout field and --groups auto\n"
        "          are rejected\n"
        "  -h, --help\n",
        stderr);
}

static void parse_gen_args(int argc, char** argv, GenArgs& o) {
    auto next = [&](int& i) -> string {
        i++;
        if (i >= argc) throw runtime_error(string(argv[i - 1]) + " needs a value");
        return argv[i];
    };
    // argv[0] is the "gen" token itself (JS parses process.argv.slice(2)).
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        OptNum n = OPT_NULL;
        if (a == "-o" || a == "--out") o.out = next(i);
        else if (a == "-t" || a == "--template") o.templ = next(i);
        else if (a == "--size") { n = optn(parse_num(next(i))); o.size = n; }
        else if (a == "--y") { n = optn(parse_num(next(i))); o.y = n; }
        else if (a == "--depth") { n = optn(parse_num(next(i))); o.depth = n; }
        else if (a == "--color") o.color = next(i);
        else if (a == "--strength") { n = optn(parse_num(next(i))); o.strength = n; }
        else if (a == "--outline") o.outline = next(i);
        else if (a == "--outline-width") { n = optn(parse_num(next(i))); o.outline_width = n; }
        else if (a == "--bloom") { n = optn(parse_num(next(i))); o.bloom = n; }
        else if (a == "--entrance") o.entrance = next(i);
        else if (a == "--layout") o.layout = next(i);
        else if (a == "--groups") o.groups = next(i);
        else if (a == "--jitter") { n = optn(parse_num(next(i))); o.jitter = n; }
        else if (a == "--jitter-rot") { n = optn(parse_num(next(i))); o.jitter_rot = n; }
        else if (a == "--jitter-freq") { n = optn(parse_num(next(i))); o.jitter_freq = n; }
        else if (a == "--jitter-hit") o.jitter_hit = true;
        else if (a == "--jitter-hit-dur") { n = optn(parse_num(next(i))); o.jitter_hit_dur = n; }
        else if (a == "--wave") { n = optn(parse_num(next(i))); o.wave = n; }
        else if (a == "--wave-freq") { n = optn(parse_num(next(i))); o.wave_freq = n; }
        else if (a == "--wave-spacing") { n = optn(parse_num(next(i))); o.wave_spacing = n; }
        else if (a == "--wave-hit") o.wave_hit = true;
        else if (a == "--wave-hit-dur") { n = optn(parse_num(next(i))); o.wave_hit_dur = n; }
        else if (a == "--dolly") { n = optn(parse_num(next(i))); o.dolly = n; }
        else if (a == "--crane") { n = optn(parse_num(next(i))); o.crane = n; }
        else if (a == "--move-at") { n = optn(parse_num(next(i))); o.move_at = n; }
        else if (a == "--move-dur") { n = optn(parse_num(next(i))); o.move_dur = n; }
        else if (a == "--shake") { n = optn(parse_num(next(i))); o.shake = n; }
        else if (a == "--shake-roll") { n = optn(parse_num(next(i))); o.shake_roll = n; }
        else if (a == "--shake-freq") { n = optn(parse_num(next(i))); o.shake_freq = n; }
        else if (a == "--shake-hit") o.shake_hit = true;
        else if (a == "--shake-hit-dur") { n = optn(parse_num(next(i))); o.shake_hit_dur = n; }
        else if (a == "--fit-width") { n = optn(parse_num(next(i))); o.fit_width = n; }
        else if (a == "--offset") { n = optn(parse_num(next(i))); o.offset = n; }
        else if (a == "--fade-in") { n = optn(parse_num(next(i))); o.fade_in = n; }
        else if (a == "--fade-out") { n = optn(parse_num(next(i))); o.fade_out = n; }
        else if (a == "--stagger") { n = optn(parse_num(next(i))); o.stagger = n; }
        else if (a == "--tail") { n = optn(parse_num(next(i))); o.tail = n; }
        else if (a == "--duration") { n = optn(parse_num(next(i))); o.duration = n; }
        else if (a == "--yaw-drift") { n = optn(parse_num(next(i))); o.yaw_drift = n; }
        else if (a == "--radius") { n = optn(parse_num(next(i))); o.radius = n; }
        else if (a == "--keep-camera") o.keep_camera = true;
        else if (a == "--keep-open") o.keep_open = true;
        else if (a == "--outro-at") { n = optn(parse_num(next(i))); o.outro_at = n; }
        else if (a == "--outro-text") { o.outro_text = next(i); o.has_outro_text = true; }
        else if (a == "--outro-subtitle") { o.outro_subtitle = next(i); o.has_outro_subtitle = true; }
        else if (a == "--outro-wish") o.outro_wish = next(i);
        else if (a == "--outro-out") { n = optn(parse_num(next(i))); o.outro_out = n; }
        else if (a == "--bg-look") o.bg_look = next(i);
        else if (a == "--bg-cut") o.bg_cut.push_back(next(i));
        else if (a == "--bg-fade") { n = optn(parse_num(next(i))); o.bg_fade = n; }
        else if (a == "--fx") o.fx = true;
        else if (a == "--fx-flash") { n = optn(parse_num(next(i))); o.fx_flash = n; }
        else if (a == "--fx-punch") { n = optn(parse_num(next(i))); o.fx_punch = n; }
        else if (a == "--fx-vignette") { n = optn(parse_num(next(i))); o.fx_vignette = n; }
        else if (a == "--fx-chromatic") { n = optn(parse_num(next(i))); o.fx_chromatic = n; }
        else if (a == "--fx-sat") { n = optn(parse_num(next(i))); o.fx_sat = n; }
        else if (a == "--palette") o.palette = next(i);
        else if (a == "--roll-power") { n = optn(parse_num(next(i))); o.roll_power = n; }
        else if (a == "--energy-file") o.energy_file = next(i);
        else if (a == "--force-whip") o.force_whip.push_back(next(i));
        else if (a == "--fps") { n = optn(parse_num(next(i))); o.fps = n; }
        else if (a == "--title") o.title = next(i);
        else if (a == "--subtitle") o.subtitle = next(i);
        else if (a == "--title-in") { n = optn(parse_num(next(i))); o.title_in = n; }
        else if (a == "--title-out") { n = optn(parse_num(next(i))); o.title_out = n; }
        else if (a == "--title-size") { n = optn(parse_num(next(i))); o.title_size = n; }
        else if (a == "-h" || a == "--help") o.help = true;
        else if (!a.empty() && a[0] != '-' && o.input.empty()) o.input = a;
        else throw runtime_error("unknown argument " + a);
    }
}

static int gen_main(int argc, char** argv) {
    GenArgs a;
    try {
        parse_gen_args(argc, argv, a);
    } catch (const exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    if (a.help || a.input.empty()) {
        gen_usage();
        return a.help ? 0 : 1;
    }
    try {
        if (a.layout != "single" && a.layout != "field")
            throw runtime_error("--layout must be single or field (got \"" + a.layout + "\")");
        if (a.groups != "off" && a.groups != "auto")
            throw runtime_error("--groups must be off or auto (got \"" + a.groups + "\")");
        if (a.layout == "field" || a.groups == "auto")
            throw runtime_error(
                "chamina-cpp gen: --layout field / --groups auto not ported yet — "
                "use node js/lrc2scene.mjs");

        LrcParsed parsed = parse_lrc(read_file(a.input));
        // The [offset:] tag and --offset both nudge the whole song; the tag
        // runs first so the CLI flag stays an explicit authoring correction.
        double off = opt_num0(a.offset);
        vector<LrcLine> lines;
        for (const auto& l : apply_offset(parsed, false, 0.0)) {
            LrcLine o;
            o.time = l.time + off;
            o.text = l.text;
            o.has_tags = l.has_tags;
            o.tags = l.tags;
            if (l.has_ct) {
                o.has_ct = true;
                for (double v : l.ct) o.ct.push_back(v + off);
            }
            lines.push_back(move(o));
        }
        if (lines.empty())
            throw runtime_error("no timestamped lines in " + a.input);

        ojson tpl = ojson::parse(read_file(a.templ));
        double duration = opt_finite(a.duration)
                              ? a.duration.v
                              : suggest_duration(lines, a.tail.v);

        ojson camera_animation = ojson(nullptr);
        bool cam_absent = false;
        if (a.keep_camera) {
            if (tpl.contains("camera_animation"))
                camera_animation = tpl["camera_animation"];
            else
                cam_absent = true;
        } else {
            camera_animation = tune_orbit(tpl, duration, a);
        }
        if (!cam_absent) apply_camera_motion(camera_animation, duration, a);

        // Keep a margin: outline rim, bloom bleed and DOF softness all push
        // the line outward beyond its advance width.
        double vw = visible_width(tpl, cam_absent ? nullptr : &camera_animation);
        double fit_width = a.fit_width.is_null ? vw * 0.85 : a.fit_width.v;

        // Card times, derived before the layout runs.
        double outro_at = opt_finite(a.outro_at) && a.outro_at.v > 0
                              ? a.outro_at.v
                              : numeric_limits<double>::quiet_NaN();
        double outro_out;
        if (opt_finite(a.outro_out) && a.outro_out.v > 0)
            outro_out = a.outro_out.v;
        else
            outro_out = js_max((isfinite(outro_at) ? outro_at : 0.0) + 3.0, duration - 1.5);
        double first_cue = !lines.empty() ? lines[0].time : duration;
        double title_out;
        if (opt_finite(a.title_out) && a.title_out.v >= 0)
            title_out = a.title_out.v;
        else
            title_out = js_max(a.title_in.v + 2.0, first_cue - 1.2);

        // Only lines that actually render take part in the export.
        vector<LrcLine> lyric_lines;
        for (const auto& l : lines)
            if (!l.text.empty()) lyric_lines.push_back(l);
        if (!a.energy_file.empty()) check_energy_file(a.energy_file);

        ojson jitter_obj = ojson(nullptr);
        if (opt_finite(a.jitter) && a.jitter.v > 0) {
            jitter_obj = ojson::object();
            jitter_obj["amplitude"] = a.jitter.v;
            jitter_obj["rotation"] = a.jitter_rot.v;
            jitter_obj["frequency"] = a.jitter_freq.v;
            jitter_obj["hit"] = a.jitter_hit;
            jitter_obj["hit_duration"] = a.jitter_hit_dur.v;
        }
        ojson wave_obj = ojson(nullptr);
        if (opt_finite(a.wave) && a.wave.v > 0) {
            wave_obj = ojson::object();
            wave_obj["amplitude"] = a.wave.v;
            wave_obj["frequency"] = a.wave_freq.v;
            wave_obj["spacing"] = a.wave_spacing.v;
            wave_obj["hit"] = a.wave_hit;
            wave_obj["hit_duration"] = a.wave_hit_dur.v;
        }

        LyricOpts lo;
        lo.size = a.size.v;
        lo.y = a.y.v;
        lo.depth = a.depth.v;
        lo.color = a.color;
        lo.strength = a.strength.v;
        lo.fade_in = a.fade_in.v;
        lo.fade_out = a.fade_out.v;
        lo.stagger = a.stagger.v;
        lo.entrance = a.entrance;
        lo.total = duration;
        lo.fit_width = fit_width;
        lo.jitter = jitter_obj.is_null() ? nullptr : &jitter_obj;
        lo.wave = wave_obj.is_null() ? nullptr : &wave_obj;
        vector<ojson> texts = build_lyric_texts(lyric_lines, lo);

        if (!a.outline.empty()) {
            for (auto& t : texts) {
                ojson outline = ojson::object();
                outline["color"] = a.outline;
                outline["width"] = a.outline_width.v;
                outline["strength"] = 1.0;
                t["material"]["outline"] = move(outline);
            }
        }
        // Per-line colour cycle; runs after the rim exists so the rim follows
        // each line's own tint. Cards are built later and keep --color.
        if (!a.palette.empty()) apply_palette(texts, a.palette);

        if (a.keep_open && !texts.empty()) {
            ojson* fade = find_fade(texts.back());
            if (fade) {
                fade->erase("fade_out_at");
                fade->erase("fade_out");
            }
        }

        // Outro card. The last cue lands far before the track ends, so the
        // held last line hands the frame over at outro-at - 0.6.
        if (isfinite(outro_at) && !texts.empty()) {
            ojson* fade = find_fade(texts.back());
            if (fade && fade->contains("start_at") && (*fade)["start_at"].is_number()) {
                double hand_off = round3(
                    js_max((*fade)["start_at"].get<double>() + 2.0, outro_at - 0.6));
                double foa = fade->contains("fade_out_at") && (*fade)["fade_out_at"].is_number()
                                 ? (*fade)["fade_out_at"].get<double>()
                                 : numeric_limits<double>::quiet_NaN();
                if (!isfinite(foa) || foa > hand_off) {
                    (*fade)["fade_out"] = a.fade_out.v;
                    (*fade)["fade_out_at"] = hand_off;
                }
            }
        }

        // Opening card; the outro card(s) close the video.
        double title_size = opt_clamp(a.title_size, 0.2, 12, 3);
        string card_entrance = a.entrance == "none" ? "slide" : a.entrance;
        TitleOpts common;
        common.color = a.color;
        common.strength = a.strength.v;
        common.outline = a.outline;
        common.outline_width = a.outline_width.v;
        common.start_at = a.title_in.v;
        common.out_at = title_out;
        // A card with `none` still reads better as a slide.
        common.entrance = card_entrance;
        common.stagger = 0.05;
        vector<ojson> cards;
        auto add_card = [&](const TitleOpts& t) {
            ojson b = build_title_block(t);
            if (!b.is_null()) cards.push_back(move(b));
        };
        {
            TitleOpts t = common;
            t.text = a.title;
            t.size = title_size;
            t.y = 1.2;
            t.fit_width = fit_width * 0.72;
            add_card(t);
        }
        {
            TitleOpts t = common;
            t.text = a.subtitle;
            t.size = title_size / 3.0;
            t.y = -0.6;
            t.strength = a.strength.v * 0.7;
            t.fit_width = fit_width * 0.5;
            t.stagger = 0.03;
            add_card(t);
        }
        if (isfinite(outro_at)) {
            {
                TitleOpts t = common;
                t.text = a.has_outro_text ? a.outro_text : a.title;
                t.size = title_size;
                t.y = 1.2;
                t.fit_width = fit_width * 0.72;
                t.start_at = outro_at;
                t.out_at = outro_out;
                t.fade_out = 1.4;
                add_card(t);
            }
            {
                TitleOpts t = common;
                t.text = a.has_outro_subtitle ? a.outro_subtitle : a.subtitle;
                t.size = title_size / 3.0;
                t.y = -0.6;
                t.strength = a.strength.v * 0.7;
                t.fit_width = fit_width * 0.5;
                t.stagger = 0.03;
                t.start_at = round3(outro_at + 0.35);
                t.out_at = outro_out;
                t.fade_out = 1.4;
                add_card(t);
            }
            if (!a.outro_wish.empty()) {
                TitleOpts t = common;
                t.text = a.outro_wish;
                t.size = title_size / 4.0;
                t.y = -1.7;
                t.strength = a.strength.v * 0.6;
                t.fit_width = fit_width * 0.45;
                t.stagger = 0.05;
                t.start_at = round3(outro_at + 15.0);
                t.out_at = outro_out;
                t.fade_out = 1.4;
                add_card(t);
            }
        }

        ojson scene = tpl;
        scene["duration"] = duration;
        // Sample the same timeline more sparsely; written only when asked for.
        if (opt_finite(a.fps) && a.fps.v > 0) scene["fps"] = a.fps.v;
        {
            bool wd = tpl.contains("work_dir") && js_truthy(tpl["work_dir"]);
            scene["work_dir"] = wd ? tpl["work_dir"] : ojson("target/chamina");
        }
        if (!cam_absent) scene["camera_animation"] = camera_animation;
        ojson bgs = build_backgrounds(a.bg_cut, a.bg_look, a.bg_fade.v);
        if (!bgs.is_null()) scene["backgrounds"] = move(bgs);
        if (a.fx)
            scene["fx"] = build_fx(a.fx_chromatic, a.fx_vignette, a.fx_sat, a.fx_flash,
                                   a.fx_punch);
        if (tpl.contains("bloom") && js_truthy(tpl["bloom"])) {
            ojson bloom = tpl["bloom"];
            // A full lyric line is far more emissive area than the gift card's
            // three glyphs — drop the preset intensity.
            bloom["intensity"] = a.bloom.v;
            scene["bloom"] = move(bloom);
        }
        if (tpl.contains("dof") && js_truthy(tpl["dof"])) {
            ojson dof = tpl["dof"];
            // DOF has to focus on the plane the text sits on; a --radius
            // override would otherwise leave the line permanently defocused.
            if (opt_gt0(a.radius)) dof["focal_distance"] = a.radius.v;
            scene["dof"] = move(dof);
        }
        ojson texts_all = ojson::array();
        for (auto& c : cards) texts_all.push_back(move(c));
        for (auto& t : texts) texts_all.push_back(move(t));
        scene["texts"] = move(texts_all);

        string out_path = a.out;
        if (out_path.empty()) {
            // JS: lrcPath.replace(/\.lrc$/i, '') + '.json'
            out_path = a.input;
            if (out_path.size() >= 4) {
                string tail4 = to_lower_ascii(out_path.substr(out_path.size() - 4));
                if (tail4 == ".lrc") out_path = out_path.substr(0, out_path.size() - 4);
            }
            out_path += ".json";
        }
        {
            error_code ec;
            filesystem::path op = filesystem::u8path(out_path);
            if (op.has_parent_path()) filesystem::create_directories(op.parent_path(), ec);
            string body;
            js_json(scene, body, 0);
            body += '\n';
            ofstream f(op, ios::binary);
            if (!f) throw runtime_error("cannot write " + out_path);
            f.write(body.data(), (streamsize)body.size());
            f.close();
            if (!f) throw runtime_error("cannot write " + out_path);
        }

        printf("%s\n", out_path.c_str());
        double fps_eff = 60.0;
        if (scene.contains("fps") && scene["fps"].is_number()) {
            double fv = scene["fps"].get<double>();
            if (fv != 0) fps_eff = fv;
        }
        double width = 1920, height = 1080;
        if (scene.contains("width") && scene["width"].is_number())
            width = scene["width"].get<double>();
        if (scene.contains("height") && scene["height"].is_number())
            height = scene["height"].get<double>();
        printf("  %d lines  → %ss  → %.0f frames  → %dx%d", (int)texts.size(),
               js_num_str(duration).c_str(), duration * fps_eff, (int)width, (int)height);
        if (!cards.empty())
            printf("  → title card %ss→%ss", js_num_str(a.title_in.v).c_str(),
                   js_num_str(title_out).c_str());
        printf("\n");
        string ti = parsed.meta.count("ti") ? parsed.meta.at("ti") : "";
        string ar = parsed.meta.count("ar") ? parsed.meta.at("ar") : "";
        if (!ti.empty() || !ar.empty())
            printf("  %s%s\n", ti.c_str(), ar.empty() ? "" : (" — " + ar).c_str());
        return 0;
    } catch (const exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

static string find_ffmpeg() {
    // cmd.exe (which popen shells out to) rejects forward slashes in the
    // command position and mangles non-ASCII absolute paths, so candidates
    // are normalised to backslashes — and the bundled installer copy wins.
    auto norm = [](string p) {
        for (auto& c : p)
            if (c == '/') c = '\\';
        return p;
    };
    if (const char* p = getenv("CHAMINA_FFMPEG")) return norm(p);
    for (auto rel : {"js/node_modules/@ffmpeg-installer/win32-x64/ffmpeg.exe"})
        if (filesystem::exists(rel)) return norm(rel);
    return "ffmpeg";
}

#ifdef _WIN32
static vector<string> utf8_argv(int argc, char** argv) {
    vector<string> out;
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc != argc) {
        // Extremely unlikely; keep the raw argv rather than failing to start.
        if (wargv) LocalFree(wargv);
        for (int i = 0; i < argc; i++) out.push_back(argv[i]);
        return out;
    }
    for (int i = 0; i < wargc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        string s(n > 1 ? (size_t)n - 1 : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
        out.push_back(move(s));
    }
    LocalFree(wargv);
    return out;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // cmd.exe hands argv over in the ANSI codepage; re-decode to UTF-8 so
    // Chinese paths and text flags survive into JSON (mirrors how node
    // receives the same bytes as UTF-16 → UTF-8).
    static vector<string> argv_hold;
    static vector<char*> argv_p;
    {
        argv_hold = utf8_argv(argc, argv);
        for (auto& s : argv_hold) argv_p.push_back(&s[0]);
        argv_p.push_back(nullptr);
        argc = (int)argv_hold.size();
        argv = argv_p.data();
    }
#endif
    if (argc >= 2 && string(argv[1]) == "gen")
        return gen_main(argc - 1, argv + 1);
    string input, audio_env_path, out = "cpp_out.mp4", png;
    float still = -1;
    unsigned from = 0, frames = 0;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        auto next = [&]() { return string(argv[++i]); };
        if (a == "--audio-env") audio_env_path = next();
        else if (a == "-o" || a == "--out") out = next();
        else if (a == "--still") still = stof(next());
        else if (a == "--png") png = next();
        else if (a == "--from") from = stoul(next());
        else if (a == "--frames") frames = stoul(next());
        else if (!a.empty() && a[0] != '-') input = a;
    }
    if (input.empty()) {
        fprintf(stderr, "usage: chamina-cpp <scene.json> [--audio-env f] -o out.mp4 "
                        "[--from n] [--frames n] [--still t --png f]\n"
                        "       chamina-cpp gen <song.lrc> -o scene.json [--help]\n");
        return 1;
    }
    // scene/audio-env read through read_file: MinGW's ifstream(char*) opens
    // via the ANSI codepage and fails on Chinese paths.
    json scene = json::parse(read_file(input));
    // The authored scene carries its settings at the root (no meta wrapper).
    json meta = scene;
    meta.erase("texts");
    if (!meta.contains("total_frames"))
        meta["total_frames"] =
            (unsigned)ceil(meta.at("duration").get<float>() * meta.at("fps").get<float>());
    // Older / minimal scenes may omit whole sections; normalise once so every
    // downstream .at() is safe.
    if (!meta.contains("fx")) meta["fx"] = json{{"enabled", false}};
    if (!meta.contains("bloom")) meta["bloom"] = json{{"enabled", false}};
    if (!meta.contains("dof")) meta["dof"] = json{{"enabled", false}};
    if (!meta.contains("backgrounds")) meta["backgrounds"] = json::array();
    if (!meta.contains("camera"))
        meta["camera"] = json{{"fov", 0.8726646f}, {"near", 0.01f}, {"far", 500.0f}};
    World world(scene, meta);
    TRACE = getenv("CPP_TRACE") != nullptr;
    if (!audio_env_path.empty()) {
        json ej = json::parse(read_file(audio_env_path));
        world.env.fps = ej.value("fps", 60.0f);
        for (const auto& row : ej.at("data")) world.env.data.push_back(row.get<vector<float>>());
        world.has_env = true;
    }

    // fonts
    FontEngine fe;
    bool font_ok = false;
    for (const auto& f : meta.value("font_fallback", json::array())) {
        if (fe.open(f.get<string>())) {
            font_ok = true;
            break;
        }
    }
    if (!font_ok) {
        fprintf(stderr, "no usable font\n");
        return 1;
    }

    float duration = meta.at("total_frames").get<float>() / meta.at("fps").get<float>();
    (void)duration;
    world.prepare({});
    // cues/outro/first-lyric are computed in prepare() — they need env-independent
    // info only, fine.

    for (const auto& spec : scene.at("texts")) {
        PreparedText tx;
        tx.position = Vec3{spec.at("position")[0].get<float>(),
                           spec.at("position")[1].get<float>(),
                           spec.at("position")[2].get<float>()};
        tx.size = spec.at("size").get<float>();
        tx.alpha = spec.value("alpha", 1.0f);
        tx.anims = &spec.at("animations");
        tx.char_times = spec.contains("char_times") ? &spec.at("char_times") : nullptr;
        {
            // Fade anims define the block's visible span: first char becomes
            // visible at ~min(start_at), all fade out by max(fade_out_at +
            // fade_out). A ±5s margin absorbs char_times/stagger edges. If any
            // fade lacks fade_out_at the block stays visible indefinitely, so
            // the upper bound is left open. Conservative: worst case we
            // evaluate a block whose chars are all transparent — never the
            // reverse.
            bool has_fade = false, all_bounded = true;
            float lo = INFINITY, hi = -INFINITY;
            for (const auto& a : spec.at("animations")) {
                if (a.value("type", "") != "fade") continue;
                has_fade = true;
                lo = min(lo, a.value("start_at", 0.0f));
                auto fo_it = a.find("fade_out_at");
                if (fo_it != a.end() && fo_it->is_number())
                    hi = max(hi, fo_it->get<float>() + a.value("fade_out", 0.45f));
                else
                    all_bounded = false;
            }
            if (has_fade) {
                tx.vis_from = lo - 5.0f;
                if (all_bounded) tx.vis_to = hi + 5.0f;
            }
        }
        tx.neon = true;
        auto& mat = spec.at("material");
        hex_linear(mat.at("color").get<string>(), tx.color_lin);
        tx.strength = mat.value("strength", 1.0f);
        for (int k = 0; k < 3; k++) tx.color_lin[k] *= tx.strength;
        tx.has_outline = mat.contains("outline");
        if (tx.has_outline) {
            hex_linear(mat.at("outline").at("color").get<string>(), tx.outline_lin);
            tx.outline_strength = mat.at("outline").value("strength", 1.0f);
            for (int k = 0; k < 3; k++) tx.outline_lin[k] *= tx.outline_strength;
        }
        // layout (port of src/text/layout.rs)
        const string& text = spec.at("text").get<string>();
        float cursor = 0, max_asc = -1e30f, min_desc = 1e30f;
        struct Item {
            uint32_t cp;
            int glyph;
            float advance_em, center_x;
        };
        vector<Item> items;
        for (size_t i = 0; i < text.size();) {
            size_t nx;
            uint32_t cp = fe.codepoint(text, i, nx);
            i = nx;
            int glyph = fe.find_glyph(cp);
            float adv, lsb;
            fe.h_metrics(glyph, adv, lsb);
            max_asc = max(max_asc, fe.ascender);
            min_desc = min(min_desc, fe.descender);
            items.push_back({cp, glyph, adv, cursor + adv * 0.5f});
            cursor += adv;
        }
        float total_width = cursor;
        float line_center_y = (max_asc + min_desc) * 0.5f;
        float half = total_width * 0.5f;
        // fit_width (port of export::build)
        float size = tx.size;
        if (spec.contains("fit_width")) {
            float budget = spec.at("fit_width").get<float>();
            float w = total_width * size;
            if (w > budget && budget > 0 && w > 0) size *= budget / w;
        }
        tx.size = size;
        tx.total_width_em = total_width;
        float depth = spec.value("depth", 0.09f);
        const json* cts = tx.char_times;
        for (size_t gi = 0; gi < items.size(); gi++) {
            auto& it = items[gi];
            float cx_local = it.center_x - half;
            float cy_local = -line_center_y;
            bool empty = it.glyph == 0 || fe.glyph_empty(it.glyph);
            PreparedChar ch;
            ch.idx = gi;
            ch.lands_at = 0;
            for (const auto& a : spec.at("animations")) {
                if (a.value("type", "") == "fade") {
                    float sa = a.value("start_at", 0.0f), stg = a.value("stagger", 0.04f);
                    if (cts && gi < cts->size()) ch.lands_at = (*cts)[gi].get<float>();
                    else ch.lands_at = sa + (float)gi * stg;
                }
            }
            if (empty) {
                // no mesh, but the char still occupies the index stream
                tx.chars.push_back(move(ch));
                continue;
            }
            if (TRACE) {
                fprintf(stderr, "[trace] mesh glyph=%d\n", it.glyph);
                fflush(stderr);
            }
            float org_x = 0, org_y = 0;
            if (!build_glyph_mesh(fe, it.glyph, depth, ch.mesh, org_x, org_y)) {
                tx.chars.push_back(move(ch));
                continue;
            }
            // base_position: position + R*((center - adv/2 + origin) * size)
            Vec3 local{(cx_local - it.advance_em * 0.5f + org_x) * size,
                       (cy_local + org_y) * size, 0};
            ch.base_position = tx.position + local;
            if (tx.has_outline) {
                float ow = mat_outline_width(spec);
                ch.hull = smooth_hull(ch.mesh, ow);
            }
            tx.chars.push_back(move(ch));
        }
        tx.hw = clamp(tx.total_width_em * tx.size * 0.5f, 1.0f, 9.0f);
        world.texts.push_back(move(tx));
    }
    // sparks need hw — rebuild after texts
    {
        uint32_t r = 0;
        for (size_t bi = 0; bi < world.texts.size(); bi++) {
            auto& tx = world.texts[bi];
            float cue = INFINITY;
            for (const auto& a : tx.anims->items()) {
                const json& aj = a.value();
                if (aj.value("type", "") == "fade") cue = aj.value("start_at", 0.0f);
            }
            if (!isfinite(cue)) continue;
            r = (uint32_t)(bi * 131 + 7);
            for (int k = 0; k < 14; k++) {
                Sparks::P sp;
                sp.pos = tx.position +
                         Vec3{(mulberry_f(r) * 2 - 1) * tx.hw * 1.05f,
                              (mulberry_f(r) * 2 - 1) * 0.85f, (mulberry_f(r) - 0.5f) * 0.6f};
                sp.cue = cue + mulberry_f(r) * 0.15f;
                for (int c = 0; c < 3; c++) sp.color[c] = tx.color_lin[c];
                sp.s0 = (float)M_PI * (0.15f + 0.7f * mulberry_f(r));
                sp.s1 = 0.5f + mulberry_f(r) * 1.6f;
                sp.s2 = 2.0f + mulberry_f(r) * 4.0f;
                world.sparks.p.push_back(sp);
            }
        }
    }

    int w = meta.at("width").get<int>(), h = meta.at("height").get<int>();
    unsigned fps = meta.at("fps").get<unsigned>();
    unsigned total = meta.at("total_frames").get<unsigned>();
    unsigned count = frames ? frames : total - from;
    count = min(count, total - from);

    if (still >= 0) {
        Frame fr;
        world.render_frame_into(still, fr);
        auto& fx = meta.at("fx");
        float flash = 0;
        if (fx.value("enabled", false)) {
            float bands[64], bass, energy;
            world.env.sample(still, bands, bass, energy);
            float hit = hit_envelope(world.cues, still, fx.value("hit_duration", 0.35f));
            flash = fx.value("flash", 0.0f) * hit * (0.45f + 0.55f * energy);
        }
        static vector<uint8_t> rgb;
        finish(fr, meta, flash, rgb);
        // stbi_write_png opens through the ANSI codepage and fails on Chinese
        // paths; rasterize to memory and write through a wide path instead.
        vector<uint8_t> png_bytes;
        auto png_sink = [](void* ctx, void* data, int size) {
            auto& buf = *(vector<uint8_t>*)ctx;
            const uint8_t* d = (const uint8_t*)data;
            buf.insert(buf.end(), d, d + size);
        };
        if (!stbi_write_png_to_func(png_sink, &png_bytes, w, h, 3, rgb.data(), w * 3)) {
            fprintf(stderr, "[cpp] ERROR: cannot encode %s\n", png.c_str());
            return 1;
        }
        {
            ofstream f(filesystem::u8path(png), ios::binary);
            if (!f || !f.write((const char*)png_bytes.data(), (streamsize)png_bytes.size())) {
                fprintf(stderr, "[cpp] ERROR: cannot write %s\n", png.c_str());
                return 1;
            }
        }
        printf("still t=%g -> %s\n", still, png.c_str());
        return 0;
    }

    // popen shells out through `cmd /c`, which eats the leading quote when a
    // line holds several — wrapping the whole command in one extra quote pair
    // keeps every inner quote intact (verified against cmd's parsing rules).
    string cmd = "\"\"" + find_ffmpeg() + "\" -y -hide_banner -loglevel error -f rawvideo "
                 "-pix_fmt rgb24 -s " + to_string(w) + "x" + to_string(h) +
                 " -framerate " + to_string(fps) + " -i pipe:0 -c:v libx264 -preset medium "
                 "-crf 18 -pix_fmt yuv420p -vf scale=trunc(iw/2)*2:trunc(ih/2)*2 "
                 "-movflags +faststart \"" + out + "\"\"";
    FILE* pipe = popen(cmd.c_str(), "wb");
    if (!pipe) {
        fprintf(stderr, "cannot start ffmpeg\n");
        return 1;
    }
    auto t0 = chrono::steady_clock::now();
    double prof_render = 0, prof_finish = 0;
    bool prof = getenv("CPP_PROF") != nullptr;
    for (unsigned i = 0; i < count; i++) {
        float t = (from + i) / (float)fps;
        auto tr = chrono::steady_clock::now();
        static Frame fr;
        world.render_frame_into(t, fr);
        auto tf = chrono::steady_clock::now();
        auto& fx = meta.at("fx");
        float flash = 0;
        if (fx.value("enabled", false)) {
            float bands[64], bass, energy;
            world.env.sample(t, bands, bass, energy);
            float hit = hit_envelope(world.cues, t, fx.value("hit_duration", 0.35f));
            flash = fx.value("flash", 0.0f) * hit * (0.45f + 0.55f * energy);
        }
        static vector<uint8_t> rgb;
        finish(fr, meta, flash, rgb);
        prof_render += chrono::duration<double>(tf - tr).count();
        prof_finish += chrono::duration<double>(chrono::steady_clock::now() - tf).count();
        fwrite(rgb.data(), 1, rgb.size(), pipe);
        // A dead ffmpeg turns the pipe into a silent sink — surface it instead
        // of "done"-ing over a missing file.
        if (ferror(pipe)) {
            fprintf(stderr, "[cpp] ERROR: ffmpeg pipe broke at frame %u\n", i + 1);
            pclose(pipe);
            return 1;
        }
        if ((i + 1) % 60 == 0 || i + 1 == count) {
            double el = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
            fprintf(stderr, "[cpp] frame %u/%u  %.2f fps  elapsed %.1fs  eta %.0fs\n",
                    i + 1, count, (i + 1) / el, el, el / (i + 1) * (count - i - 1));
        }
    }
    int rc = pclose(pipe);
    double el = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
    if (rc != 0) {
        fprintf(stderr, "[cpp] ERROR: ffmpeg exited %d — output may be missing or bad\n", rc);
        return 1;
    }
    fprintf(stderr, "[cpp] done -> %s  %u frames  %.2f fps\n", out.c_str(), count,
            count / el);
    if (prof)
        fprintf(stderr, "[prof] render %.1fms/f  finish %.1fms/f\n",
                prof_render / count * 1000, prof_finish / count * 1000);
    return 0;
}
