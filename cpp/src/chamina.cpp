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
static float vnoise(float x, float y) {
    float ix = floorf(x), iy = floorf(y);
    float fx = x - ix, fy = y - iy;
    float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    float a = hash2(ix, iy) * (1 - ux) + hash2(ix + 1, iy) * ux;
    float b = hash2(ix, iy + 1) * (1 - ux) + hash2(ix + 1, iy + 1) * ux;
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
    const bool need_cloud = pal[9] > 0.001f;
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

    const bool need_stars = pal[10] > 0.001f;
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
                float r = hash2(c0, c1);
                float f0 = sp0 - c0 - 0.5f, f1 = sp1 - c1 - 0.5f;
                float j0 = hash2(c0 + 7.1f, c1 + 7.1f) - 0.5f;
                float j1 = hash2(c0 + 13.7f, c1 + 13.7f) - 0.5f;
                float dd = sqrtf((f0 - j0 * 0.6f) * (f0 - j0 * 0.6f) +
                                 (f1 - j1 * 0.6f) * (f1 - j1 * 0.6f));
                float kk = clamp((dd - 0.16f) / -0.16f, 0.0f, 1.0f);
                float star = kk * kk * (3 - 2 * kk);
                float tw = 0.55f + 0.45f * sinf(t * 1.7f + r * 40);
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

    Frame render_frame(float t) const {
        Frame f;
        render_frame_into(t, f);
        return f;
    }
};

// ---------------------------------------------------------------------------
// post (port of src/native/post.rs)
// ---------------------------------------------------------------------------

using Img = vector<array<float, 3>>;

static Img blur(const Img& src, int w, int h, float sigma) {
    int radius = (int)ceilf(sigma * 2.5f);
    vector<float> weights(radius + 1);
    for (int i = 0; i <= radius; i++)
        weights[i] = expf(-((float)i * i) / (2 * sigma * sigma));
    float sum = 0;
    for (float v : weights) sum += v;
    sum = sum * 2 - weights[0];
    for (auto& v : weights) v /= sum;
    Img tmp((size_t)w * h), out((size_t)w * h);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc[3] = {0, 0, 0};
            for (int i = 0; i <= radius; i++) {
                auto& a = src[(size_t)y * w + min(x + i, w - 1)];
                auto& b = src[(size_t)y * w + max(x - i, 0)];
                for (int k = 0; k < 3; k++) acc[k] += (a[k] + b[k]) * weights[i];
            }
            tmp[(size_t)y * w + x] = {acc[0], acc[1], acc[2]};
        }
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
    return out;
}

static float bloom_gain_global = 6.0f;

static Img bloom(const Frame& f, float intensity, float threshold) {
    int w = f.w, h = f.h;
    int hw = max(w / 2, 1), hh = max(h / 2, 1);
    Img cur((size_t)hw * hh);
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
            cur[(size_t)y * hw + x] = {acc[0] * 0.25f, acc[1] * 0.25f, acc[2] * 0.25f};
        }
    vector<Img> levels;
    vector<pair<int, int>> sizes;
    int cw = hw, chh = hh;
    for (int i = 0; i < 5; i++) {
        cur = blur(cur, cw, chh, (float)(1 << i));
        levels.push_back(cur);
        sizes.push_back({cw, chh});
        if (cw <= 2 || chh <= 2) break;
        int nw = max(cw / 2, 1), nh = max(chh / 2, 1);
        Img down((size_t)nw * nh);
#pragma omp parallel for schedule(static)
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                float acc[3] = {0, 0, 0};
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++)
                        for (int k = 0; k < 3; k++)
                            acc[k] += cur[min(y * 2 + dy, chh - 1) * (size_t)cw +
                                          min(x * 2 + dx, cw - 1)][k] * 0.25f;
                down[(size_t)y * nw + x] = {acc[0], acc[1], acc[2]};
            }
        cur = down;
        cw = nw;
        chh = nh;
    }
    float weights[5] = {1.0f, 0.8f, 0.6f, 0.4f, 0.2f};
    float k = intensity * bloom_gain_global;
    // Cascade: fold the small mips into their parents (bilinear + add) and
    // touch full resolution exactly once, instead of upsampling all five
    // levels to full res. Same soft result, a fifth of the traffic.
    auto upsample_into = [](Img& dst, int dw, int dh, const Img& src, int sw, int sh,
                            float mul, const Img* add) {
        dst.assign((size_t)dw * dh, {0, 0, 0});
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
                    if (add) o[kk] += (*add)[(size_t)y * dw + x][kk];
                }
            }
        }
    };
    size_t n = levels.size();
    Img acc;
    {
        auto [sw, sh] = sizes[n - 1];
        upsample_into(acc, sw, sh, levels[n - 1], sw, sh,
                      weights[min(n - 1, (size_t)4)] * k, nullptr);
    }
    for (int li = (int)n - 2; li >= 0; li--) {
        auto [dw, dh] = sizes[li];
        auto [sw, sh] = sizes[li + 1];
        Img up;
        upsample_into(up, dw, dh, acc, sw, sh, 1.0f, nullptr);
        // add this level's own (weighted) energy
#pragma omp parallel for schedule(static)
        for (long i = 0; i < (long)up.size(); i++)
            for (int kk = 0; kk < 3; kk++)
                up[i][kk] += levels[li][i][kk] * weights[min((size_t)li, (size_t)4)] * k;
        acc = move(up);
    }
    Img out;
    upsample_into(out, w, h, acc, sizes[0].first, sizes[0].second, 1.0f, nullptr);
    return out;
}

static Img dof(const Img& color, const Frame& f, float focus, float aperture,
               float maxblur) {
    int w = f.w, h = f.h;
    int hw = max(w / 2, 1), hh = max(h / 2, 1);
    vector<array<float, 3>> half((size_t)hw * hh);
    vector<float> coc((size_t)hw * hh);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < hh; y++)
        for (int x = 0; x < hw; x++) {
            float acc[3] = {0, 0, 0}, cacc = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    size_t si = min(y * 2 + dy, h - 1) * (size_t)w + min(x * 2 + dx, w - 1);
                    auto& c = color[si];
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
    Img b1 = blur(half, hw, hh, 1.0f), b2 = blur(half, hw, hh, 4.0f),
        b3 = blur(half, hw, hh, 8.0f);
    float coc_px = (float)w;
    Img out((size_t)w * h);
#pragma omp parallel for schedule(static)
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int hx = min(x / 2, hw - 1), hy = min(y / 2, hh - 1);
            float c01 = clamp(coc[(size_t)hy * hw + hx] * coc_px / 15.0f, 0.0f, 1.0f);
            auto& src = color[(size_t)y * w + x];
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
            auto& o = out[(size_t)y * w + x];
            for (int kk = 0; kk < 3; kk++)
                o[kk] = src[kk] * w0 + b1[i][kk] * w1 + b2[i][kk] * w2 + b3[i][kk] * w3;
        }
    return out;
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

static array<float, 3> tonemap_srgb(const array<float, 3>& c) {
    const float M[3][3] = {{0.59719f, 0.07600f, 0.02840f},
                           {0.35458f, 0.90834f, 0.13383f},
                           {0.04823f, 0.01566f, 0.83777f}};
    const float MO[3][3] = {{1.60475f, -0.10208f, -0.00327f},
                            {-0.53108f, 1.10813f, -0.07276f},
                            {-0.07367f, -0.00605f, 1.07602f}};
    auto fit = [](float v) {
        v = max(v, 0.0f);
        return (v * (v + 0.0245786f) - 0.000090537f) /
               (v * (0.983729f * v + 0.4329510f) + 0.238081f);
    };
    float fitted[3];
    for (int i = 0; i < 3; i++)
        fitted[i] = fit(c[0] * M[i][0] + c[1] * M[i][1] + c[2] * M[i][2]);
    array<float, 3> out;
    for (int i = 0; i < 3; i++) {
        float v = clamp(fitted[0] * MO[i][0] + fitted[1] * MO[i][1] + fitted[2] * MO[i][2],
                        0.0f, 1.0f);
        out[i] = srgb_encode_lut(v);
    }
    return out;
}

static Img grade(const Img& src, int w, int h, float chromatic, float vignette,
                 float saturation, float flash) {
    Img out = src;
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
    return out;
}

static vector<uint8_t> finish(const Frame& f, const json& meta, float flash) {
    int w = f.w, h = f.h;
    Img img = f.color;
    if (meta.at("bloom").value("enabled", false)) {
        Img bl = bloom(f, meta.at("bloom").at("intensity").get<float>(),
                       meta.at("bloom").at("threshold").get<float>());
#pragma omp parallel for schedule(static)
        for (long i = 0; i < (long)img.size(); i++) {
            img[i][0] += bl[i][0];
            img[i][1] += bl[i][1];
            img[i][2] += bl[i][2];
        }
    }
    auto& dof_j = meta.at("dof");
    if (dof_j.value("enabled", false)) {
        float aperture = 0.00017f * (4.0f / max(dof_j.at("aperture_f_stops").get<float>(), 0.1f));
        float maxblur = 0.008f * max(dof_j.at("focal_radius").get<float>(), 0.001f);
        img = dof(img, f, dof_j.at("focal_distance").get<float>(), aperture, maxblur);
    }
    Img srgb((size_t)w * h);
#pragma omp parallel for schedule(static)
    for (long i = 0; i < (long)img.size(); i++) srgb[i] = tonemap_srgb(img[i]);
    if (meta.value("fx", json::object()).value("enabled", false)) {
        auto& fx = meta.at("fx");
        srgb = grade(srgb, w, h, fx.value("chromatic", 0.0f), fx.value("vignette", 0.0f),
                     fx.value("saturation", 1.0f), flash);
    }
    vector<uint8_t> bytes((size_t)w * h * 3);
    for (size_t i = 0; i < srgb.size(); i++)
        for (int k = 0; k < 3; k++)
            bytes[i * 3 + k] = (uint8_t)(clamp(srgb[i][k], 0.0f, 1.0f) * 255 + 0.5f);
    return bytes;
}

static float mat_outline_width(const json& spec) {
    auto& mat = spec.at("material");
    if (mat.value("type", "neon") != "neon") return 0;
    auto it = mat.find("outline");
    if (it == mat.end()) return 0;
    return it->value("width", 0.02f);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

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

int main(int argc, char** argv) {
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
                        "[--from n] [--frames n] [--still t --png f]\n");
        return 1;
    }
    json scene = json::parse(ifstream(input));
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
        json ej = json::parse(ifstream(audio_env_path));
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
        vector<uint8_t> rgb = finish(fr, meta, flash);
        if (!stbi_write_png(png.c_str(), w, h, 3, rgb.data(), w * 3)) {
            fprintf(stderr, "[cpp] ERROR: cannot write %s\n", png.c_str());
            return 1;
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
        vector<uint8_t> rgb = finish(fr, meta, flash);
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
