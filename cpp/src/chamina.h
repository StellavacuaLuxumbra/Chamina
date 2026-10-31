// chamina.h — the whole Chamina engine as a single-header C++ library.
//
// The deliverable software: fonts (stb_truetype), layout, extruded-glyph
// meshes (earcut), animation/camera/backgrounds (ports of js/anim.mjs +
// src/animation.rs), software rasterizer, full audio visualisation and the
// post chain, plus the two content subcommands (gen: LRC→scene JSON;
// audio: track→spectrum envelope). Drop-in, stb-style:
//
//   #define CHAMINA_IMPLEMENTATION      // in exactly one .cpp
//   #include "chamina.h"
//
// Other translation units include the header for the API only. Compile the
// implementation TU with -fopenmp for multithreaded rendering (optional —
// the header also builds single-threaded); on Windows add -lshell32
// (CommandLineToArgvW). The vendored single-header dependencies
// (nlohmann/json.hpp, stb_truetype.h, mapbox/earcut.hpp, stb_image_write.h)
// are found through the include path (cpp/vendor in this repo).
//
// Minimal embedder:
//
//   #define CHAMINA_IMPLEMENTATION
//   #include "chamina.h"
//
//   int main() {
//     std::string scene, env;
//     chamina::read_file("scenes/bird.json", scene);
//     chamina::read_file("target/chamina/audio_env.json", env);
//     char err[512];
//     auto* e = chamina::engine_create(scene.c_str(), env.c_str(), err, sizeof err);
//     if (!e) { fprintf(stderr, "error: %s\n", err); return 1; }
//     std::vector<unsigned char> rgb((size_t)chamina::engine_width(e) *
//                                    chamina::engine_height(e) * 3);
//     chamina::engine_render_rgb(e, 8.0f, rgb.data());    // RGB24, sRGB, post applied
//     chamina::engine_still_png(e, 20.35f, "frame.png");  // or straight to PNG
//     chamina::engine_encode_video(e, 0, 0, "out.mp4");   // 0 frames = to the end
//     chamina::engine_destroy(e);
//   }
//
// Scene JSON contract: whatever js/lrc2scene.mjs / `chamina-cpp gen` emits
// (fields at the root, no meta wrapper). The audio-env JSON is optional;
// without it the spectrum ring/bars/sparks/呼吸 components stay off.
// Not thread-safe across engines: frame buffers are static by design
// (parity with the original single-TU engine).
#ifndef CHAMINA_H_INCLUDE
#define CHAMINA_H_INCLUDE

#include <cstddef>
#include <cstdint>
#include <string>

namespace chamina {

// Opaque engine: parsed scene + fonts + prepared meshes.
struct Engine;

// Build an engine from UTF-8 JSON strings. audio_env_json may be null/empty.
// On failure returns nullptr and fills err (when err_cap > 0).
Engine* engine_create(const char* scene_json, const char* audio_env_json,
                      char* err, int err_cap);
void engine_destroy(Engine*);

// Render frame at time t (seconds) into rgb: width*height*3 bytes, RGB,
// full post chain applied. Never throws; on internal error the buffer is
// zeroed and a message goes to stderr.
void engine_render_rgb(Engine*, float t, unsigned char* rgb);

// Render + encode a PNG at a UTF-8 path. Returns 0 on success, 1 on error.
int engine_still_png(Engine*, float t, const char* png_path);

// Encode frames [from, from+frames) at the scene fps into an mp4 via the
// ffmpeg pipe (bundled installer / CHAMINA_FFMPEG / PATH). frames == 0
// means "until the end". Returns 0 on success, 1 on error. Progress goes
// to stderr; CPP_PROF=1 keeps the per-stage profiling report.
int engine_encode_video(Engine*, unsigned from, unsigned frames,
                        const char* out_path);

int engine_width(const Engine*);
int engine_height(const Engine*);
unsigned engine_fps(const Engine*);
unsigned engine_total_frames(const Engine*);

// Per-frame trace prints (also enabled by env CPP_TRACE=1 at engine_create).
void set_trace(bool on);

// Windows-codepage-proof file read (UTF-8 in, Chinese paths fine).
// Returns false and leaves out empty on failure.
bool read_file(const char* path, std::string& out);

// Windows: re-decode the process command line to UTF-8 and return a new
// argv (valid for the program's lifetime); other platforms pass through.
char** argv_utf8(int argc, char** argv, int* out_argc);

// CLI-shaped content tools — the exe is a thin wrapper around these.
// argv[0] is the subcommand token itself ("gen" / "audio"), exactly as the
// node tools see their own process.argv.slice(1).
int gen_main(int argc, char** argv);    // LRC → scene JSON
int audio_main(int argc, char** argv);  // audio → spectrum envelope JSON

} // namespace chamina

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

// ---------------------------------------------------------------------------
// skydome full-res pixel loop, split for SIMD. The scalar kernel is a verbatim
// transcription of the row loop that used to live in draw_skydome (same op
// order, same casts, same LUT item reads). The AVX2 kernel (defined next to
// the other dispatch kernels, after the grade stage) is lane-for-lane
// identical: atan2f stays a scalar call per lane (lane extraction), sqrt /
// divide / floor run on the vector unit, and LUT / bilinear gathers read the
// same table entries at the same clamped indices. Star-cell gather indices are
// additionally clamped so a pathological lane cannot gather past the tables
// (the scalar body would read wild memory there; gate renders never hit it).
// Gate: still-image byte A/B must stay EQUAL.
// ---------------------------------------------------------------------------
using skydome_kfn = void (*)(float* col, float* dep, int w, int h, int y0, int y1,
                             const float* fwd, const float* rgt, const float* up, float tan_h,
                             float aspect, const float* pal, int need_cloud, int need_stars,
                             const float* cloud, int hw, int hh, const float* cell_tw);

// 1-D LUTs for the two transcendental per-pixel terms (outputs are mixed
// into gradients far below 8-bit significance). Former static locals inside
// draw_skydome, hoisted to file scope so both kernels gather the same tables;
// the build stays lazy exactly as before.
static const float* skydome_lut_pow085() {
    static const float* p = [] {
        static float v[4097];
        for (int i = 0; i <= 4096; i++) v[i] = powf((float)i / 4096, 0.85f);
        return &v[0];
    }();
    return p;
}
static const float* skydome_lut_exp5() {
    static const float* p = [] {
        static float v[2049];
        for (int i = 0; i <= 2048; i++) v[i] = expf(-(float)i / 2048 * 5.0f);
        return &v[0];
    }();
    return p;
}
// Exact body of the lambda that used to live in draw_skydome.
static inline float sky_lut(const float* lutv, int n, float x) {
    float xx = clamp(x, 0.0f, 1.0f) * n;
    int i = (int)xx;
    if (i > n - 1) i = n - 1;
    float fr = xx - i;
    return lutv[i] + (lutv[i + 1] - lutv[i]) * fr;
}

// One full-res skydome pixel, byte-for-byte the former row-loop body of
// draw_skydome. Shared by the scalar kernel and the AVX2 tail.
static inline void skydome_px_scalar(float* col, float* dep, int w, int h, int row, int col_i,
                                     const float* fwd, const float* rgt, const float* up,
                                     float tan_h, float aspect, float yn, const float* pal,
                                     int need_cloud, int need_stars, const float* cloud, int hw,
                                     int hh, const float* cell_tw, const StarTable& st) {
    float xn = 2 * (col_i + 0.5f) / w - 1;
    Vec3 d = (Vec3{fwd[0], fwd[1], fwd[2]} +
              Vec3{rgt[0], rgt[1], rgt[2]} * (xn * tan_h * aspect) +
              Vec3{up[0], up[1], up[2]} * (yn * tan_h))
                 .normalize();
    // gradient (powf → LUT)
    float h01 = clamp(d.y * 0.5f + 0.5f, 0.0f, 1.0f);
    float mixv = sky_lut(skydome_lut_pow085(), 4096, h01);
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = pal[3 + i] + (pal[i] - pal[3 + i]) * mixv;
    // nebula cloud (half-res field) + horizon band (expf → LUT)
    if (need_cloud) {
        float cx = (col_i + 0.5f) * hw / w - 0.5f;
        float cy = (row + 0.5f) * hh / h - 0.5f;
        int x0 = (int)floorf(max(cx, 0.0f));
        int y0 = (int)floorf(max(cy, 0.0f));
        int x1 = min(x0 + 1, hw - 1), y1 = min(y0 + 1, hh - 1);
        float tx = clamp(cx - x0, 0.0f, 1.0f), ty = clamp(cy - y0, 0.0f, 1.0f);
        float cval = (cloud[(size_t)y0 * hw + x0] * (1 - tx) +
                      cloud[(size_t)y0 * hw + x1] * tx) *
                         (1 - ty) +
                     (cloud[(size_t)y1 * hw + x0] * (1 - tx) +
                      cloud[(size_t)y1 * hw + x1] * tx) *
                         ty;
        float band = sky_lut(skydome_lut_exp5(), 2048, fabsf(d.y));
        for (int i = 0; i < 3; i++)
            c[i] += pal[6 + i] * (cval * pal[9] * 0.95f + band * pal[9] * 0.30f);
    }
    if (need_stars) {
        float uv0 = atan2f(d.z, d.x) / (float)M_PI / 2 + 0.5f;
        float uv1 = d.y * 0.5f + 0.5f;
        float sp0 = uv0 * 380, sp1 = uv1 * 190;
        float c0 = floorf(sp0), c1 = floorf(sp1);
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
        for (int i = 0; i < 3; i++) c[i] += add[i] * star * on * tw * pal[10] * 1.7f;
    }
    float rad = clamp(sqrtf(d.x * d.x + d.y * d.y) * 1.15f, 0.0f, 1.0f);
    float sink = 1 - 0.30f * rad * rad;
    size_t oi = ((size_t)row * w + col_i) * 3;
    col[oi] = c[0] * sink;
    col[oi + 1] = c[1] * sink;
    col[oi + 2] = c[2] * sink;
    dep[(size_t)row * w + col_i] = 300.0f;
}

static void skydome_rows_scalar(float* col, float* dep, int w, int h, int y0, int y1,
                                const float* fwd, const float* rgt, const float* up, float tan_h,
                                float aspect, const float* pal, int need_cloud, int need_stars,
                                const float* cloud, int hw, int hh, const float* cell_tw) {
    const StarTable& st = StarTable::get();
    for (int row = y0; row < y1; row++) {
        float py = row + 0.5f;
        float yn = 1 - 2 * py / h;
        for (int col_i = 0; col_i < w; col_i++)
            skydome_px_scalar(col, dep, w, h, row, col_i, fwd, rgt, up, tan_h, aspect, yn, pal,
                              need_cloud, need_stars, cloud, hw, hh, cell_tw, st);
    }
}

// Defined after the grade stage (needs the AVX2 infrastructure further down).
static skydome_kfn skydome_kernel();

// ---------------------------------------------------------------------------
// Half-res nebula prepass (the fbm2 field), split for SIMD the same way as the
// full-res loop: scalar kernel verbatim from the old row loop, AVX2 half
// lane-for-lane (atan2f by lane extraction, fbm2 across lanes — 2 octaves × 4
// lattice-corner gathers with the exact-scalar fallback blended back on the
// rare out-of-domain lane). Tier A: byte gate must stay EQUAL (the fbm table
// hits return the same entries the scalar hash2_lattice reads).
// ---------------------------------------------------------------------------
using cloud_kfn = void (*)(float* cl, int hw, int hh, int y0, int y1, int w, int h,
                           const float* fwd, const float* rgt, const float* up, float tan_h,
                           float aspect, float t, const float* pal);

// One half-res nebula cell, byte-for-byte the old row-loop body of the
// prepass (drift term folded to d18/d09 exactly as the per-pixel expression
// computes it). Shared by the scalar kernel and the AVX2 tail.
static inline void cloud_px_scalar(float* cl, int hw, int w, int row, int x,
                                   const float* fwd, const float* rgt, const float* up,
                                   float tan_h, float aspect, float yn, float d18, float d09) {
    float px = (x + 0.5f) * w / hw;
    float xn = 2 * px / w - 1;
    Vec3 ray = (Vec3{fwd[0], fwd[1], fwd[2]} +
                Vec3{rgt[0], rgt[1], rgt[2]} * (xn * tan_h * aspect) +
                Vec3{up[0], up[1], up[2]} * (yn * tan_h))
                   .normalize();
    float uv0 = atan2f(ray.z, ray.x) / (float)M_PI / 2 + 0.5f;
    float uv1 = ray.y * 0.5f + 0.5f;
    float qx = (uv0 * 7 + d18) * 2;
    float qy = (uv1 * 3.5f + d09) * 2;
    float n = fbm2(qx, qy);
    float kk = clamp((n - 0.40f) / (0.92f - 0.40f), 0.0f, 1.0f);
    cl[(size_t)row * hw + x] = kk * kk * (3 - 2 * kk);
}

static void cloud_rows_scalar(float* cl, int hw, int hh, int y0, int y1, int w, int h,
                              const float* fwd, const float* rgt, const float* up, float tan_h,
                              float aspect, float t, const float* pal) {
    // drift folding: (t*pal[11]) once, then *0.018/*0.009 exactly as the
    // per-pixel expression would compute them
    float tk = t * pal[11];
    const float d18 = tk * 0.018f, d09 = tk * 0.009f;
    for (int row = y0; row < y1; row++) {
        float py = (row + 0.5f) * h / hh;
        float yn = 1 - 2 * py / h;
        for (int x = 0; x < hw; x++)
            cloud_px_scalar(cl, hw, w, row, x, fwd, rgt, up, tan_h, aspect, yn, d18, d09);
    }
}

// Defined next to the other AVX2 kernels, below.
static cloud_kfn cloud_kernel();

static void draw_skydome(Frame& f, const Cam& cam, float t, const float pal[12]) {
    const int w = f.w, h = f.h;

    // The nebula is the expensive part (8 sinf per pixel in fbm) and is pure
    // low frequency: sample it at half resolution and bilinear upsample. The
    // stars stay full res — they are 1-2 px points and would not survive.
    const int hw = max(w / 2, 1), hh = max(h / 2, 1);
    static vector<float> cloud;
    const bool need_cloud = pal[9] > 0.001f && !getenv("CPP_SKIP_CLOUD");
    if (need_cloud) {
        cloud.assign((size_t)hw * hh, 0.0f);
        cloud_kfn cfn = cloud_kernel();
#ifdef _OPENMP
#pragma omp parallel
        {
            int T = omp_get_num_threads(), tk = omp_get_thread_num();
            int y0 = (int)((long long)hh * tk / T), y1 = (int)((long long)hh * (tk + 1) / T);
            if (y0 < y1)
                cfn(cloud.data(), hw, hh, y0, y1, w, h, &cam.fwd.x, &cam.right.x, &cam.up.x,
                    cam.tan_h, cam.aspect, t, pal);
        }
#else
        cfn(cloud.data(), hw, hh, 0, hh, w, h, &cam.fwd.x, &cam.right.x, &cam.up.x, cam.tan_h,
            cam.aspect, t, pal);
#endif
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
    // Full-res pixel loop, row-blocked across threads (both kernels write only
    // their own rows; cloud/cell_tw prepasses above are untouched).
    float* colp = (float*)f.color.data();
    float* depp = f.depth.data();
    skydome_kfn fn = skydome_kernel();
#ifdef _OPENMP
#pragma omp parallel
    {
        int T = omp_get_num_threads(), tk = omp_get_thread_num();
        int y0 = (int)((long long)h * tk / T), y1 = (int)((long long)h * (tk + 1) / T);
        if (y0 < y1)
            fn(colp, depp, w, h, y0, y1, &cam.fwd.x, &cam.right.x, &cam.up.x, cam.tan_h,
               cam.aspect, pal, need_cloud ? 1 : 0, need_stars ? 1 : 0, cloud.data(), hw, hh,
               cell_tw.data());
    }
#else
    fn(colp, depp, w, h, 0, h, &cam.fwd.x, &cam.right.x, &cam.up.x, cam.tan_h, cam.aspect, pal,
       need_cloud ? 1 : 0, need_stars ? 1 : 0, cloud.data(), hw, hh, cell_tw.data());
#endif
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

// ---------------------------------------------------------------------------
// SIMD dispatch: AVX2 kernels selected at runtime, scalar everywhere else.
//
// Toolchain facts (verified in tmp/probe_attr.cpp / probe_fma2.cpp): GCC's
// target_clones needs ifunc, which PE binaries do not support — so each hot
// kernel ships as a scalar/vector pair, the vector half compiled under
// __attribute__((target("avx2"))) and picked once through
// __builtin_cpu_supports into a function pointer. Non-GNU hosts and
// CHAMINA_NO_SIMD=1 (paired-perf switch) land on the scalar path.
//
// Numerics contract (Tier A, byte gate): every lane runs the scalar body's
// exact op sequence — no reassociation, no fma contraction (the scalar
// baseline has no FMA ISA, so the kernels pin -ffp-contract=off), hardware
// sqrt/divide, and LUT gathers that read the same table entries at the same
// clamped indices. Any drift would change render bytes immediately, which
// the still-image A/B gate catches.
// ---------------------------------------------------------------------------
// x86_64 + GNU-family only: PE targets have no ifunc, so there is no
// target_clones — dispatch is a manual function-pointer pick (see below).
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
#define CHAMINA_SIMD_AVX2 1
// immintrin's declarations are macro-guarded on __AVX__ at include time; the
// default build is SSE2 baseline, so include it once inside an AVX2 codegen
// window and restore. The kernels themselves re-mark target("avx2").
#pragma GCC push_options
#pragma GCC target("avx2")
#include <immintrin.h>
#pragma GCC pop_options
#endif

#if CHAMINA_SIMD_AVX2
static inline bool simd_off() {
    static bool v = getenv("CHAMINA_NO_SIMD") != nullptr;
    return v;
}
static inline bool simd_have_avx2() {
    static bool v = [] {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") != 0;
    }();
    return v;
}
static inline bool simd_avx2() { return !simd_off() && simd_have_avx2(); }
#endif

// ---------------------------------------------------------------------------
// ACES tonemap + sRGB encode, full-frame pixel stream. Lane-for-lane
// transcription of tonemap_srgb/srgb_encode_lut/aces_fit: same tables, same
// op order per lane, same (int) truncations. Out-of-band lanes (v<=0, v>=8)
// carry dead gather values that the blends discard; their gather indices are
// clamped so gathers never leave the 4097-entry tables. NaN inputs are
// invalid-scene territory (the byte gate never feeds them).
// ---------------------------------------------------------------------------

// Full-post tonemap over a contiguous pixel range (AoS float3 in and out).
static void tonemap_rows_scalar(const float* src, float* dst, size_t n) {
    for (size_t i = 0; i < n; i++) {
        array<float, 3> c = {src[i * 3], src[i * 3 + 1], src[i * 3 + 2]};
        array<float, 3> o = tonemap_srgb(c);
        dst[i * 3] = o[0];
        dst[i * 3 + 1] = o[1];
        dst[i * 3 + 2] = o[2];
    }
}

using tonemap_kfn = void (*)(const float* src, float* dst, size_t n);

#if CHAMINA_SIMD_AVX2

// srgb_encode_lut across lanes: x = clamp(v,0,1)*4096; the scalar clamps i
// BEFORE taking the fractional part ("if (i > 4095) i = 4095; f = x - i").
__attribute__((target("avx2"), optimize("fp-contract=off")))
static inline __m256 srgb_encode_lanes256(__m256 v, const float* lut) {
    __m256 x = _mm256_min_ps(_mm256_max_ps(v, _mm256_setzero_ps()), _mm256_set1_ps(1.0f)) *
               _mm256_set1_ps(4096.0f);
    __m256i iu = _mm256_cvttps_epi32(x);
    __m256i ic = _mm256_min_epi32(iu, _mm256_set1_epi32(4095));
    __m256 f = x - _mm256_cvtepi32_ps(ic);
    __m256i ib = _mm256_min_epi32(_mm256_add_epi32(ic, _mm256_set1_epi32(1)),
                                  _mm256_set1_epi32(4096));
    __m256 l0 = _mm256_i32gather_ps(lut, ic, 4);
    __m256 l1 = _mm256_i32gather_ps(lut, ib, 4);
    return (l1 - l0) * f + l0;
}

// aces_fit across lanes (mirrors aces_fit above): v<=0 → lut[0]; v>=8 → the
// exact rational; else LUT lerp with f taken from the UNclamped i.
__attribute__((target("avx2"), optimize("fp-contract=off")))
static inline __m256 aces_fit_lanes256(__m256 v, const float* lut) {
    __m256 x = v * _mm256_set1_ps(4096.0f / 8.0f);
    __m256i ix = _mm256_cvttps_epi32(x);
    __m256 f = x - _mm256_cvtepi32_ps(ix);
    // gather indices stay inside [0,4096]; in-band lanes (0<v<8) need no clamp
    __m256i ia = _mm256_min_epi32(_mm256_max_epi32(ix, _mm256_setzero_si256()),
                                  _mm256_set1_epi32(4095));
    __m256i ib = _mm256_min_epi32(_mm256_add_epi32(ia, _mm256_set1_epi32(1)),
                                  _mm256_set1_epi32(4096));
    __m256 l0 = _mm256_i32gather_ps(lut, ia, 4);
    __m256 l1 = _mm256_i32gather_ps(lut, ib, 4);
    __m256 res = (l1 - l0) * f + l0;
    // v >= 8: the exact rational tail, same op order as the scalar branch
    __m256 num = v * (v + _mm256_set1_ps(0.0245786f)) - _mm256_set1_ps(0.000090537f);
    __m256 den = v * (_mm256_set1_ps(0.983729f) * v + _mm256_set1_ps(0.4329510f)) +
                 _mm256_set1_ps(0.238081f);
    res = _mm256_blendv_ps(res, num / den, _mm256_cmp_ps(v, _mm256_set1_ps(8.0f), _CMP_GE_OQ));
    return _mm256_blendv_ps(res, _mm256_set1_ps(lut[0]),
                            _mm256_cmp_ps(v, _mm256_setzero_ps(), _CMP_LE_OQ));
}

__attribute__((target("avx2"), optimize("fp-contract=off")))
static void tonemap_rows_avx2(const float* src, float* dst, size_t n) {
    const float* lutA = aces_lut();
    const float* lutS = srgb_lut();
    const __m256i kstride = _mm256_setr_epi32(0, 3, 6, 9, 12, 15, 18, 21);
    const __m256i kone = _mm256_set1_epi32(1);
    const __m256i ktwo = _mm256_add_epi32(kone, kone);
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i base = _mm256_add_epi32(kstride, _mm256_set1_epi32((int)(i * 3)));
        __m256 c0 = _mm256_i32gather_ps(src, base, 4);
        __m256 c1 = _mm256_i32gather_ps(src, _mm256_add_epi32(base, kone), 4);
        __m256 c2 = _mm256_i32gather_ps(src, _mm256_add_epi32(base, ktwo), 4);
        // fitted[j] = c0*M[j][0] + c1*M[j][1] + c2*M[j][2], mul/add order unchanged
        __m256 f0 = (c0 * _mm256_set1_ps(0.59719f) + c1 * _mm256_set1_ps(0.07600f)) +
                    c2 * _mm256_set1_ps(0.02840f);
        __m256 f1 = (c0 * _mm256_set1_ps(0.35458f) + c1 * _mm256_set1_ps(0.90834f)) +
                    c2 * _mm256_set1_ps(0.13383f);
        __m256 f2 = (c0 * _mm256_set1_ps(0.04823f) + c1 * _mm256_set1_ps(0.01566f)) +
                    c2 * _mm256_set1_ps(0.83777f);
        f0 = aces_fit_lanes256(f0, lutA);
        f1 = aces_fit_lanes256(f1, lutA);
        f2 = aces_fit_lanes256(f2, lutA);
        // v[i] = clamp(f0*MO[i][0] + f1*MO[i][1] + f2*MO[i][2], 0, 1) → srgb LUT
        __m256 v0 = ((f0 * _mm256_set1_ps(1.60475f) + f1 * _mm256_set1_ps(-0.10208f)) +
                     f2 * _mm256_set1_ps(-0.00327f));
        __m256 v1 = ((f0 * _mm256_set1_ps(-0.53108f) + f1 * _mm256_set1_ps(1.10813f)) +
                     f2 * _mm256_set1_ps(-0.07276f));
        __m256 v2 = ((f0 * _mm256_set1_ps(-0.07367f) + f1 * _mm256_set1_ps(-0.00605f)) +
                     f2 * _mm256_set1_ps(1.07602f));
        v0 = srgb_encode_lanes256(v0, lutS);
        v1 = srgb_encode_lanes256(v1, lutS);
        v2 = srgb_encode_lanes256(v2, lutS);
        // interleave (AoS) and store: 8 pixels × 3 floats, contiguous slice
        alignas(32) float tmp[24];
        _mm256_store_ps(tmp + 0, v0);
        _mm256_store_ps(tmp + 8, v1);
        _mm256_store_ps(tmp + 16, v2);
        float* o = dst + i * 3;
        for (int p = 0; p < 8; p++) {
            o[p * 3] = tmp[p];
            o[p * 3 + 1] = tmp[8 + p];
            o[p * 3 + 2] = tmp[16 + p];
        }
    }
    if (i < n) tonemap_rows_scalar(src + i * 3, dst + i * 3, n - i);
}
#endif  // CHAMINA_SIMD_AVX2

static tonemap_kfn tonemap_kernel() {
    static tonemap_kfn fn =
#if CHAMINA_SIMD_AVX2
        simd_avx2() ? &tonemap_rows_avx2 :
#endif
        &tonemap_rows_scalar;
    return fn;
}

// Full-frame tonemap driver. Chunking keeps contiguous 8-pixel lane groups
// per thread and mirrors OpenMP static scheduling, so per-stage profiling
// stays comparable across runs.
static void tonemap_into(const float* src, float* dst, size_t n) {
    if (n == 0) return;
    tonemap_kfn fn = tonemap_kernel();
    size_t blocks = n / 8;  // whole lane groups travel together; tail stays serial
#ifdef _OPENMP
    if (blocks >= 32) {
#pragma omp parallel
        {
            int T = omp_get_num_threads(), tk = omp_get_thread_num();
            size_t b = (blocks * tk) / T, e = (blocks * (tk + 1)) / T;
            if (b < e) fn(src + b * 24, dst + b * 24, (e - b) * 8);
        }
        return;
    }
#endif
    fn(src, dst, n);
}

// ---------------------------------------------------------------------------
// grade (chromatic / vignette / saturation / flash), full frame. The lane
// transcription keeps divides and sqrtfs — vx/y division and sqrt are exact
// in hardware, so the values stay bit-identical to the scalar body.
// ---------------------------------------------------------------------------

// Scalar seed: one pixel, verbatim transcription of the original per-pixel
// body. Shared by the scalar row kernel and the AVX2 tail.
static inline void grade_px(const float* src, int w, int h, int x, int y, float chromatic,
                            float vignette, float saturation, float flash, float* out) {
    float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
    float cx = u - 0.5f, cy = v - 0.5f;
    float r2 = cx * cx + cy * cy;
    float k = chromatic * (0.35f + r2 * 2.4f);
    auto sample = [&](float du, float dv, int ch) {
        int sx = clamp((int)(((u + du) * w)), 0, w - 1);
        int sy = clamp((int)(((v + dv) * h)), 0, h - 1);
        return src[((size_t)sy * w + (size_t)sx) * 3 + ch];
    };
    array<float, 3> col = {sample(cx * k, cy * k, 0), sample(0, 0, 1),
                           sample(-cx * k, -cy * k, 2)};
    float l = 0.2126f * col[0] + 0.7152f * col[1] + 0.0722f * col[2];
    for (auto& c : col) c = l + (c - l) * saturation;
    float rad = clamp(sqrtf(r2) * 1.45f, 0.0f, 1.0f);
    for (auto& c : col) {
        c *= 1 - vignette * rad * rad;
        c += (1 - c) * clamp(flash, 0.0f, 1.0f);
    }
    out[((size_t)y * w + x) * 3] = col[0];
    out[((size_t)y * w + x) * 3 + 1] = col[1];
    out[((size_t)y * w + x) * 3 + 2] = col[2];
}

static void grade_rows_scalar(const float* src, int w, int h, int y0, int y1, float chromatic,
                              float vignette, float saturation, float flash, float* out) {
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < w; x++)
            grade_px(src, w, h, x, y, chromatic, vignette, saturation, flash, out);
}

using grade_kfn = void (*)(const float* src, int w, int h, int y0, int y1, float chromatic,
                           float vignette, float saturation, float flash, float* out);

#if CHAMINA_SIMD_AVX2
__attribute__((target("avx2"), optimize("fp-contract=off")))
static void grade_rows_avx2(const float* src, int w, int h, int y0, int y1, float chromatic,
                            float vignette, float saturation, float flash, float* out) {
    const __m256 half = _mm256_set1_ps(0.5f);
    const __m256i kzero = _mm256_setzero_si256();
    const __m256i kw1 = _mm256_set1_epi32(w - 1);
    const __m256i kh1 = _mm256_set1_epi32(h - 1);
    const __m256i k3 = _mm256_set1_epi32(3);
    const __m256i klanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    for (int y = y0; y < y1; y++) {
        const float v = (y + 0.5f) / h;
        const float cy = v - 0.5f;
        const float cyy = cy * cy;                      // broadcast lane value
        const float cyk_c = cy;                          // dv = cy * k needs cy, not cy²
        const int sy1 = clamp((int)(((v + 0.0f) * h)), 0, h - 1);  // center sample row, per row
        const __m256 vb = _mm256_set1_ps(v);
        const __m256 cyyb = _mm256_set1_ps(cyy);
        const __m256 cyb = _mm256_set1_ps(cyk_c);
        const __m256 chw = _mm256_set1_ps(chromatic);
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            __m256 xf = _mm256_cvtepi32_ps(_mm256_add_epi32(klanes, _mm256_set1_epi32(x)));
            __m256 u = (xf + half) / _mm256_set1_ps((float)w);   // exact per-lane divide
            __m256 cx = u - half;
            __m256 r2 = cx * cx + cyyb;                          // (cx*cx) + (cy*cy)
            __m256 kk = chw * (_mm256_set1_ps(0.35f) + r2 * _mm256_set1_ps(2.4f));
            __m256 duk = cx * kk;                    // du = cx * k
            __m256 dvk = cyb * kk;                   // dv = cy * k
            // sample(cx*k, cy*k, 0)
            __m256 u0 = (u + duk) * _mm256_set1_ps((float)w);
            __m256 v0 = (vb + dvk) * _mm256_set1_ps((float)h);
            // sample(-cx*k, -cy*k, 2): u + (−a) ≡ u − a bitwise
            __m256 u2 = (u - duk) * _mm256_set1_ps((float)w);
            __m256 v2 = (vb - dvk) * _mm256_set1_ps((float)h);
            // center sample (du=dv=0, ch=1): sx per lane, sy hoisted per row
            __m256 uc = u * _mm256_set1_ps((float)w);
            __m256i s0x = _mm256_cvttps_epi32(u0);
            __m256i s0y = _mm256_cvttps_epi32(v0);
            __m256i s2x = _mm256_cvttps_epi32(u2);
            __m256i s2y = _mm256_cvttps_epi32(v2);
            __m256i scx = _mm256_cvttps_epi32(uc);
            s0x = _mm256_min_epi32(_mm256_max_epi32(s0x, kzero), kw1);
            s0y = _mm256_min_epi32(_mm256_max_epi32(s0y, kzero), kh1);
            s2x = _mm256_min_epi32(_mm256_max_epi32(s2x, kzero), kw1);
            s2y = _mm256_min_epi32(_mm256_max_epi32(s2y, kzero), kh1);
            scx = _mm256_min_epi32(_mm256_max_epi32(scx, kzero), kw1);
            __m256i i0 = _mm256_mullo_epi32(_mm256_add_epi32(_mm256_mullo_epi32(s0y, _mm256_set1_epi32(w)), s0x), k3);
            __m256i i2 = _mm256_mullo_epi32(_mm256_add_epi32(_mm256_mullo_epi32(s2y, _mm256_set1_epi32(w)), s2x), k3);
            __m256i i1 = _mm256_mullo_epi32(_mm256_add_epi32(_mm256_mullo_epi32(_mm256_set1_epi32(sy1), _mm256_set1_epi32(w)), scx), k3);
            __m256 col0 = _mm256_i32gather_ps(src, i0, 4);
            __m256 col1 = _mm256_i32gather_ps(src, _mm256_add_epi32(i1, _mm256_set1_epi32(1)), 4);
            __m256 col2 = _mm256_i32gather_ps(src, _mm256_add_epi32(i2, _mm256_set1_epi32(2)), 4);
            __m256 l = (col0 * _mm256_set1_ps(0.2126f) + col1 * _mm256_set1_ps(0.7152f)) +
                       col2 * _mm256_set1_ps(0.0722f);
            const __m256 sat = _mm256_set1_ps(saturation);
            col0 = (col0 - l) * sat + l;
            col1 = (col1 - l) * sat + l;
            col2 = (col2 - l) * sat + l;
            __m256 rad = _mm256_min_ps(
                _mm256_max_ps(_mm256_sqrt_ps(r2) * _mm256_set1_ps(1.45f), _mm256_setzero_ps()),
                _mm256_set1_ps(1.0f));
            // c *= 1 - vignette*rad*rad   (scalar order: (vig*rad)*rad, then 1-…)
            __m256 vig = _mm256_set1_ps(1.0f) - _mm256_set1_ps(vignette) * rad * rad;
            const __m256 fl = _mm256_set1_ps(clamp(flash, 0.0f, 1.0f));
            col0 = col0 * vig;
            col1 = col1 * vig;
            col2 = col2 * vig;
            // c += (1 - c) * flash
            col0 = col0 + (_mm256_set1_ps(1.0f) - col0) * fl;
            col1 = col1 + (_mm256_set1_ps(1.0f) - col1) * fl;
            col2 = col2 + (_mm256_set1_ps(1.0f) - col2) * fl;
            alignas(32) float tmp[24];  // SoA-ish scratch then interleave-copy
            _mm256_store_ps(tmp + 0, col0);
            _mm256_store_ps(tmp + 8, col1);
            _mm256_store_ps(tmp + 16, col2);
            float* o = out + ((size_t)y * w + x) * 3;
            for (int p = 0; p < 8; p++) {
                o[p * 3] = tmp[p];
                o[p * 3 + 1] = tmp[8 + p];
                o[p * 3 + 2] = tmp[16 + p];
            }
        }
        for (int xi = x; xi < w; xi++)
            grade_px(src, w, h, xi, y, chromatic, vignette, saturation, flash, out);
    }
}
#endif

static grade_kfn grade_kernel() {
    static grade_kfn fn =
#if CHAMINA_SIMD_AVX2
        simd_avx2() ? &grade_rows_avx2 :
#endif
        &grade_rows_scalar;
    return fn;
}

static void grade_into(const Img& src, int w, int h, float chromatic, float vignette,
                       float saturation, float flash, Img& out) {
    if ((size_t)w * h > out.size()) out.resize((size_t)w * h);
    if (w <= 0 || h <= 0) return;
    grade_kfn fn = grade_kernel();
#ifdef _OPENMP
#pragma omp parallel
    {
        int T = omp_get_num_threads(), tk = omp_get_thread_num();
        int y0 = (int)((long long)h * tk / T), y1 = (int)((long long)h * (tk + 1) / T);
        if (y0 < y1)
            fn(src.data()[0].data(), w, h, y0, y1, chromatic, vignette, saturation, flash,
               out.data()[0].data());
    }
#else
    fn(src.data()[0].data(), w, h, 0, h, chromatic, vignette, saturation, flash, out.data()[0].data());
#endif
}

// ---------------------------------------------------------------------------
// skydome AVX2 kernel (placed after the grade stage: needs the dispatch
// infrastructure above). Lane-for-lane transcription of skydome_px_scalar:
// per-lane op order unchanged, atan2f as the atan2f_vec256 polynomial
// (Tier B — region-gated; the scalar kernel keeps the exact libm call),
// sqrt / divide / floor on the vector unit, LUT /
// bilinear / star-cell gathers hit the same entries at the same clamped
// indices. Star-cell indices are clamped to the table only for gather safety:
// in-band lanes need no clamp, and the only lanes that would, are precisely
// the ones where the scalar body's unchecked (size_t)(int)c1 * CW + (int)c0
// arithmetic already reads wild memory (d.y → ±1 makes c1 hit 190, i.e. the
// last row of the table plus up to 80 floats — gate renders keep rays off the
// exact pole, same situation as before the kernel existed).
// ---------------------------------------------------------------------------
#if CHAMINA_SIMD_AVX2
// sky_lut across lanes: x = clamp(v,0,1)*n; the scalar clamps i BEFORE taking
// the fractional part ("if (i > n-1) i = n-1; fr = x - i"), like the srgb LUT.
__attribute__((target("avx2"), optimize("fp-contract=off")))
static inline __m256 sky_lut_lanes256(__m256 v, const float* lutv, int n) {
    __m256 x = _mm256_min_ps(_mm256_max_ps(v, _mm256_setzero_ps()), _mm256_set1_ps(1.0f)) *
               _mm256_set1_ps((float)n);
    __m256i iu = _mm256_cvttps_epi32(x);
    __m256i ic = _mm256_min_epi32(iu, _mm256_set1_epi32(n - 1));
    __m256 fr = _mm256_sub_ps(x, _mm256_cvtepi32_ps(ic));
    __m256i ib = _mm256_min_epi32(_mm256_add_epi32(ic, _mm256_set1_epi32(1)), _mm256_set1_epi32(n));
    __m256 l0 = _mm256_i32gather_ps(lutv, ic, 4);
    __m256 l1 = _mm256_i32gather_ps(lutv, ib, 4);
    return _mm256_add_ps(l0, _mm256_mul_ps(_mm256_sub_ps(l1, l0), fr));
}

// Shared lane normalize for the sky geometry rays: l = sqrt(dot) with dot's
// (x²+y²)+z² order kept, and the scalar's l>0 ternary as a blend (dead lanes
// carry 0 exactly like the scalar). Used by both sky kernels; pure ops, so
// bit-identical whichever kernel calls it.
__attribute__((target("avx2"), optimize("fp-contract=off")))
static inline void norm_ray_lanes256(__m256 dx, __m256 dy, __m256 dz, __m256* nx, __m256* ny,
                                     __m256* nz) {
    __m256 zero = _mm256_setzero_ps();
    __m256 d2 = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(dx, dx), _mm256_mul_ps(dy, dy)),
                              _mm256_mul_ps(dz, dz));
    __m256 len = _mm256_sqrt_ps(d2);
    __m256 m = _mm256_cmp_ps(len, zero, _CMP_GT_OQ);
    *nx = _mm256_blendv_ps(zero, _mm256_div_ps(dx, len), m);
    *ny = _mm256_blendv_ps(zero, _mm256_div_ps(dy, len), m);
    *nz = _mm256_blendv_ps(zero, _mm256_div_ps(dz, len), m);
}

// Tier B atan2f: vector polynomial. The scalar kernels keep the exact libm
// call (the CHAMINA_NO_SIMD byte gate proves that reference line unchanged);
// the AVX byte stream DIFFs by design from here on and the region gate ±0.05
// becomes the contract for this helper. Shape: octant swap mask (|y|>|x| takes
// θ ← π/2−θ), x<0 fold (θ ← π−θ), y's sign bitOR as copysign, then double
// halving atan r = 2·atan(r/(1+√(1+r²))) into c ≤ 0.4143 and a 9-term odd
// series evaluated by Horner in c². tmp/atan2_probe.cpp (same order/literals)
// measured max |err| = 2.384e-7 rad over 4e6 samples + all axis folds
// (acceptance < 1e-6); through uv·380/(2π) that is ≤ 1.4e-5 px inside the
// star-cell lookup — a one-cell flip can occur at a cell boundary and stays
// below region significance. The (0,0)-lane blend covers the scalar's ±0 case;
// the (−0,−0)→±π libm corner cannot occur in rendered rays (a normalized
// 3-vector never has both x and z exactly ±0 off the degenerate blend) and is
// accepted as a Tier B domain note.
__attribute__((target("avx2"), optimize("fp-contract=off")))
static inline __m256 atan2f_vec256(__m256 z, __m256 x) {
    const __m256 zero = _mm256_setzero_ps();
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 sign = _mm256_set1_ps(-0.0f);
    __m256 ax = _mm256_andnot_ps(sign, x);
    __m256 ay = _mm256_andnot_ps(sign, z);
    __m256 mx = _mm256_max_ps(ax, ay);
    __m256 mn = _mm256_min_ps(ax, ay);
    // [0,1]; NaN only on an all-zero lane, which the final (0,0) blend resets
    __m256 r0 = _mm256_div_ps(mn, mx);
    __m256 c = _mm256_div_ps(
        r0, _mm256_add_ps(one, _mm256_sqrt_ps(_mm256_add_ps(_mm256_mul_ps(r0, r0), one))));
    __m256 u = _mm256_set1_ps(1.0f / 17.0f);
    __m256 c2 = _mm256_mul_ps(c, c);
    u = _mm256_add_ps(_mm256_mul_ps(u, c2), _mm256_set1_ps(-1.0f / 15.0f));
    u = _mm256_add_ps(_mm256_mul_ps(u, c2), _mm256_set1_ps(1.0f / 13.0f));
    u = _mm256_add_ps(_mm256_mul_ps(u, c2), _mm256_set1_ps(-1.0f / 11.0f));
    u = _mm256_add_ps(_mm256_mul_ps(u, c2), _mm256_set1_ps(1.0f / 9.0f));
    u = _mm256_add_ps(_mm256_mul_ps(u, c2), _mm256_set1_ps(-1.0f / 7.0f));
    u = _mm256_add_ps(_mm256_mul_ps(u, c2), _mm256_set1_ps(1.0f / 5.0f));
    u = _mm256_add_ps(_mm256_mul_ps(u, c2), _mm256_set1_ps(-1.0f / 3.0f));
    u = _mm256_add_ps(_mm256_mul_ps(u, c2), _mm256_set1_ps(1.0f));
    __m256 cu = _mm256_mul_ps(c, u);
    __m256 th = _mm256_add_ps(cu, cu);  // halving undo: 2× exact via + self
    // folds, all exact ±: octant swap, then the x<0 quadrants (θ ≤ π/2 here)
    __m256 tswap = _mm256_sub_ps(_mm256_set1_ps((float)M_PI * 0.5f), th);
    th = _mm256_blendv_ps(th, tswap, _mm256_cmp_ps(ay, ax, _CMP_GT_OQ));
    th = _mm256_blendv_ps(
        th, _mm256_sub_ps(_mm256_set1_ps((float)M_PI), th),
        _mm256_cmp_ps(x, zero, _CMP_LT_OQ));
    // (0,0) lane: scalar atan2(±0,+0) = ±0 — reset the NaN reduction to +0
    th = _mm256_blendv_ps(th, zero, _mm256_cmp_ps(mx, zero, _CMP_EQ_OQ));
    // copysign via y's sign bit (θ ≥ 0, so OR reattaches exactly ±θ)
    return _mm256_or_ps(th, _mm256_and_ps(z, sign));
}

// hash2_lattice across lanes: in-domain integer lanes gather the same table
// item the scalar reads (index identical, clamped only for gather safety);
// out-of-domain / non-integer lanes fall back to exact scalar hash2 via lane
// extraction, blended back. Same if/else as hash2_lattice, bit-identical.
__attribute__((target("avx2"), optimize("fp-contract=off")))
static inline __m256 hash2_lattice_lanes256(__m256 x, __m256 y, const float* tab) {
    const __m256 zero = _mm256_setzero_ps();
    const __m256 k1024 = _mm256_set1_ps(1024.0f), k512 = _mm256_set1_ps(512.0f);
    __m256 fx = _mm256_floor_ps(x), fy = _mm256_floor_ps(y);
    __m256 isint = _mm256_and_ps(_mm256_cmp_ps(x, fx, _CMP_EQ_OQ),
                                 _mm256_cmp_ps(y, fy, _CMP_EQ_OQ));
    __m256 inr = _mm256_and_ps(
        _mm256_cmp_ps(x, zero, _CMP_GE_OQ),
        _mm256_and_ps(_mm256_cmp_ps(x, k1024, _CMP_LT_OQ),
                      _mm256_and_ps(_mm256_cmp_ps(y, zero, _CMP_GE_OQ),
                                    _mm256_cmp_ps(y, k512, _CMP_LT_OQ))));
    __m256 ok = _mm256_and_ps(isint, inr);
    __m256i idx = _mm256_add_epi32(_mm256_mullo_epi32(_mm256_cvttps_epi32(fy),
                                                      _mm256_set1_epi32(1024)),
                                   _mm256_cvttps_epi32(fx));
    __m256i idg = _mm256_min_epi32(_mm256_max_epi32(idx, _mm256_setzero_si256()),
                                   _mm256_set1_epi32(1024 * 512 - 1));
    __m256 tv = _mm256_i32gather_ps(tab, idg, 4);
    __m256 res = tv;
    if (_mm256_movemask_ps(ok) != 0xFF) {
        // rare path: exact scalar fallback per lane, then blend
        alignas(32) float xb[8], yb[8], hb[8];
        _mm256_store_ps(xb, x);
        _mm256_store_ps(yb, y);
        for (int k = 0; k < 8; k++) hb[k] = hash2(xb[k], yb[k]);
        res = _mm256_blendv_ps(_mm256_load_ps(hb), tv, ok);
    }
    return res;
}

// vnoise across lanes: same op order as the scalar (u via 2*fx exact, corners
// computed with (ix, iy), (ix+1, iy), … as floats exactly like the scalar).
__attribute__((target("avx2"), optimize("fp-contract=off")))
static inline __m256 vnoise_lanes256(__m256 x, __m256 y, const float* tab) {
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 three = _mm256_set1_ps(3.0f);
    __m256 ix = _mm256_floor_ps(x), iy = _mm256_floor_ps(y);
    __m256 fx = _mm256_sub_ps(x, ix), fy = _mm256_sub_ps(y, iy);
    __m256 ux = _mm256_mul_ps(_mm256_mul_ps(fx, fx),
                              _mm256_sub_ps(three, _mm256_add_ps(fx, fx)));
    __m256 uy = _mm256_mul_ps(_mm256_mul_ps(fy, fy),
                              _mm256_sub_ps(three, _mm256_add_ps(fy, fy)));
    __m256 ixp = _mm256_add_ps(ix, one), iyp = _mm256_add_ps(iy, one);
    __m256 h00 = hash2_lattice_lanes256(ix, iy, tab);
    __m256 h10 = hash2_lattice_lanes256(ixp, iy, tab);
    __m256 h01 = hash2_lattice_lanes256(ix, iyp, tab);
    __m256 h11 = hash2_lattice_lanes256(ixp, iyp, tab);
    __m256 a = _mm256_add_ps(_mm256_mul_ps(h00, _mm256_sub_ps(one, ux)),
                             _mm256_mul_ps(h10, ux));
    __m256 b = _mm256_add_ps(_mm256_mul_ps(h01, _mm256_sub_ps(one, ux)),
                             _mm256_mul_ps(h11, ux));
    return _mm256_add_ps(_mm256_mul_ps(a, _mm256_sub_ps(one, uy)),
                         _mm256_mul_ps(b, uy));
}

// fbm2 across lanes: two octaves with the scalar's marching (x*2.03+1.7,
// y*2.03+9.2, a halved once); 0.25f is the exact value a reaches after
// a *= 0.5f.
__attribute__((target("avx2"), optimize("fp-contract=off")))
static inline __m256 fbm2_lanes256(__m256 x, __m256 y, const float* tab) {
    const __m256 k050 = _mm256_set1_ps(0.5f), k025 = _mm256_set1_ps(0.25f);
    const __m256 k203 = _mm256_set1_ps(2.03f), k17c = _mm256_set1_ps(1.7f),
                 k92c = _mm256_set1_ps(9.2f);
    __m256 acc = _mm256_mul_ps(k050, vnoise_lanes256(x, y, tab));
    x = _mm256_add_ps(_mm256_mul_ps(x, k203), k17c);
    y = _mm256_add_ps(_mm256_mul_ps(y, k203), k92c);
    return _mm256_add_ps(acc, _mm256_mul_ps(k025, vnoise_lanes256(x, y, tab)));
}

__attribute__((target("avx2"), optimize("fp-contract=off")))
static void skydome_rows_avx2(float* col, float* dep, int w, int h, int y0, int y1,
                              const float* fwd, const float* rgt, const float* up, float tan_h,
                              float aspect, const float* pal, int need_cloud, int need_stars,
                              const float* cloud, int hw, int hh, const float* cell_tw) {
    const StarTable& st = StarTable::get();
    const float* lp085 = skydome_lut_pow085();
    const float* lx5 = skydome_lut_exp5();
    const __m256 half = _mm256_set1_ps(0.5f);
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 zero = _mm256_setzero_ps();
    const __m256i kzero = _mm256_setzero_si256();
    const __m256i klanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256i kone = _mm256_set1_epi32(1);
    const __m256 kfx = _mm256_set1_ps(fwd[0]);
    const __m256 kfy = _mm256_set1_ps(fwd[1]);
    const __m256 kfz = _mm256_set1_ps(fwd[2]);
    const __m256 krx = _mm256_set1_ps(rgt[0]);
    const __m256 kry = _mm256_set1_ps(rgt[1]);
    const __m256 krz = _mm256_set1_ps(rgt[2]);
    const __m256 kux = _mm256_set1_ps(up[0]);
    const __m256 kuy = _mm256_set1_ps(up[1]);
    const __m256 kuz = _mm256_set1_ps(up[2]);
    const __m256 kt = _mm256_set1_ps(tan_h);
    const __m256 ka = _mm256_set1_ps(aspect);
    const __m256 kw = _mm256_set1_ps((float)w);
    const __m256 kchw = _mm256_set1_ps((float)hw);
    const __m256 kpi = _mm256_set1_ps((float)M_PI);
    const __m256 ktwo = _mm256_set1_ps(2.0f);
    const __m256 k380 = _mm256_set1_ps(380.0f);
    const __m256 k190 = _mm256_set1_ps(190.0f);
    const __m256 k006 = _mm256_set1_ps(0.6f);
    const __m256 k016 = _mm256_set1_ps(0.16f);
    const __m256 km16 = _mm256_set1_ps(-0.16f);
    const __m256 k0055 = _mm256_set1_ps(0.055f);
    const __m256 k115 = _mm256_set1_ps(1.15f);
    const __m256 k030 = _mm256_set1_ps(0.30f);
    const __m256 k095 = _mm256_set1_ps(0.95f);
    const __m256 kp9 = _mm256_set1_ps(pal[9]);
    const __m256 kp10b = _mm256_set1_ps(pal[10]);
    const __m256 k17 = _mm256_set1_ps(1.7f);
    const __m256 kpa0 = _mm256_set1_ps(0.78f);
    const __m256 kpa1 = _mm256_set1_ps(0.86f);
    const __m256 kpa2 = one;  // add[2] is 1.0f — same bits, no semantic slip
    const __m256i kc = _mm256_set1_epi32(StarTable::CW);
    const __m256i kn1 = _mm256_set1_epi32(StarTable::CW * StarTable::CH - 1);
    const __m256i khw1 = _mm256_set1_epi32(hw - 1);
    const __m256 p3b0 = _mm256_set1_ps(pal[3]), p0b0 = _mm256_set1_ps(pal[0]);
    const __m256 p3b1 = _mm256_set1_ps(pal[4]), p0b1 = _mm256_set1_ps(pal[1]);
    const __m256 p3b2 = _mm256_set1_ps(pal[5]), p0b2 = _mm256_set1_ps(pal[2]);
    for (int row = y0; row < y1; row++) {
        float py = row + 0.5f;
        float yn = 1 - 2 * py / h;
        // up term hoisted per row — the very scalar product the pixel body
        // would compute per pixel (up* (yn*tan_h), components mul'd after)
        const __m256 kb0 = _mm256_set1_ps(yn * tan_h);
        const __m256 upxb = _mm256_mul_ps(kux, kb0);
        const __m256 upyb = _mm256_mul_ps(kuy, kb0);
        const __m256 upzb = _mm256_mul_ps(kuz, kb0);
        // cloud row invariants: the y side of the bilinear fetch is per-row
        const float cyv = (row + 0.5f) * hh / h - 0.5f;
        const int y0c = (int)floorf(max(cyv, 0.0f));
        const int y1c = min(y0c + 1, hh - 1);
        const float tyv = clamp(cyv - y0c, 0.0f, 1.0f);
        const __m256 tyb = _mm256_set1_ps(tyv);
        const __m256i ky0 = _mm256_set1_epi32(y0c * hw);
        const __m256i ky1 = _mm256_set1_epi32(y1c * hw);
        int x = 0;
        for (; x + 8 <= w; x += 8) {
            __m256i kb = _mm256_add_epi32(klanes, _mm256_set1_epi32(x));
            __m256 xf = _mm256_cvtepi32_ps(kb);
            // xn = 2*(x+0.5)/w - 1  (2*(exact) via + self is bit-exact)
            __m256 xh = _mm256_add_ps(xf, half);
            __m256 xn = _mm256_sub_ps(_mm256_div_ps(_mm256_add_ps(xh, xh), kw), one);
            // d = fwd + right*(xn*tan*aspect) + up*(yn*tan), op order preserved
            __m256 rks = _mm256_mul_ps(_mm256_mul_ps(xn, kt), ka);
            __m256 dx = _mm256_add_ps(_mm256_add_ps(kfx, _mm256_mul_ps(krx, rks)), upxb);
            __m256 dy = _mm256_add_ps(_mm256_add_ps(kfy, _mm256_mul_ps(kry, rks)), upyb);
            __m256 dz = _mm256_add_ps(_mm256_add_ps(kfz, _mm256_mul_ps(krz, rks)), upzb);
            __m256 nx, ny, nz;
            norm_ray_lanes256(dx, dy, dz, &nx, &ny, &nz);
            // gradient: pow085 LUT (the helper's clamp duplicates the h01 clamp
            // idempotently, same as clamping twice in the scalar body)
            __m256 mixv = sky_lut_lanes256(
                _mm256_add_ps(_mm256_mul_ps(ny, half), half), lp085, 4096);
            __m256 c0v = _mm256_add_ps(p3b0, _mm256_mul_ps(_mm256_sub_ps(p0b0, p3b0), mixv));
            __m256 c1v = _mm256_add_ps(p3b1, _mm256_mul_ps(_mm256_sub_ps(p0b1, p3b1), mixv));
            __m256 c2v = _mm256_add_ps(p3b2, _mm256_mul_ps(_mm256_sub_ps(p0b2, p3b2), mixv));
            if (need_cloud) {
                // cx = (x+0.5)*hw/w - 0.5 per lane; x0 floor→int, tx clamped
                __m256 cxv = _mm256_sub_ps(
                    _mm256_div_ps(_mm256_mul_ps(_mm256_add_ps(xf, half), kchw), kw), half);
                __m256i x0i = _mm256_cvttps_epi32(_mm256_floor_ps(_mm256_max_ps(cxv, zero)));
                __m256i x1i = _mm256_min_epi32(_mm256_add_epi32(x0i, kone), khw1);
                __m256 tx = _mm256_min_ps(
                    _mm256_max_ps(_mm256_sub_ps(cxv, _mm256_cvtepi32_ps(x0i)), zero), one);
                // 4 gathers; the bilinear blend keeps the scalar's op order
                __m256 g00 = _mm256_i32gather_ps(cloud, _mm256_add_epi32(ky0, x0i), 4);
                __m256 g01 = _mm256_i32gather_ps(cloud, _mm256_add_epi32(ky0, x1i), 4);
                __m256 g10 = _mm256_i32gather_ps(cloud, _mm256_add_epi32(ky1, x0i), 4);
                __m256 g11 = _mm256_i32gather_ps(cloud, _mm256_add_epi32(ky1, x1i), 4);
                __m256 omtx = _mm256_sub_ps(one, tx);
                __m256 omty = _mm256_sub_ps(one, tyb);
                __m256 cval = _mm256_add_ps(
                    _mm256_mul_ps(
                        _mm256_add_ps(_mm256_mul_ps(g00, omtx), _mm256_mul_ps(g01, tx)), omty),
                    _mm256_mul_ps(
                        _mm256_add_ps(_mm256_mul_ps(g10, omtx), _mm256_mul_ps(g11, tx)), tyb));
                // band = lut(|ny| → exp5 LUT)
                __m256 band = sky_lut_lanes256(_mm256_andnot_ps(_mm256_set1_ps(-0.0f), ny), lx5,
                                               2048);
                __m256 wv = _mm256_add_ps(_mm256_mul_ps(_mm256_mul_ps(cval, kp9), k095),
                                          _mm256_mul_ps(_mm256_mul_ps(band, kp9), k030));
                c0v = _mm256_add_ps(c0v, _mm256_mul_ps(_mm256_set1_ps(pal[6]), wv));
                c1v = _mm256_add_ps(c1v, _mm256_mul_ps(_mm256_set1_ps(pal[7]), wv));
                c2v = _mm256_add_ps(c2v, _mm256_mul_ps(_mm256_set1_ps(pal[8]), wv));
            }
            if (need_stars) {
                // Tier B atan2f: shared vector polynomial (region-gated;
                // scalar path keeps the exact libm call)
                __m256 uv0 = _mm256_add_ps(
                    _mm256_div_ps(_mm256_div_ps(atan2f_vec256(nz, nx), kpi), ktwo), half);
                __m256 uv1 = _mm256_add_ps(_mm256_mul_ps(ny, half), half);
                __m256 sp0 = _mm256_mul_ps(uv0, k380);
                __m256 sp1 = _mm256_mul_ps(uv1, k190);
                __m256 c0f = _mm256_floor_ps(sp0);
                __m256 c1f = _mm256_floor_ps(sp1);
                // cell index = (int)c1 * CW + (int)c0, clamped for gather safety
                __m256i ci = _mm256_add_epi32(
                    _mm256_mullo_epi32(_mm256_cvttps_epi32(c1f), kc),
                    _mm256_cvttps_epi32(c0f));
                __m256i cig = _mm256_min_epi32(_mm256_max_epi32(ci, kzero), kn1);
                __m256 rv = _mm256_i32gather_ps(st.r, cig, 4);
                __m256 j0v = _mm256_i32gather_ps(st.j0, cig, 4);
                __m256 j1v = _mm256_i32gather_ps(st.j1, cig, 4);
                __m256 twv = _mm256_i32gather_ps(cell_tw, cig, 4);
                __m256 f0v = _mm256_sub_ps(_mm256_sub_ps(sp0, c0f), half);
                __m256 f1v = _mm256_sub_ps(_mm256_sub_ps(sp1, c1f), half);
                __m256 dv0 = _mm256_sub_ps(f0v, _mm256_mul_ps(j0v, k006));
                __m256 dv1 = _mm256_sub_ps(f1v, _mm256_mul_ps(j1v, k006));
                __m256 dd = _mm256_sqrt_ps(
                    _mm256_add_ps(_mm256_mul_ps(dv0, dv0), _mm256_mul_ps(dv1, dv1)));
                __m256 kk = _mm256_min_ps(
                    _mm256_max_ps(_mm256_div_ps(_mm256_sub_ps(dd, k016), km16), zero), one);
                // star = kk² * (3 - 2*kk); 2*kk exact via + self
                __m256 stv = _mm256_mul_ps(
                    _mm256_mul_ps(kk, kk),
                    _mm256_sub_ps(_mm256_set1_ps(3.0f), _mm256_add_ps(kk, kk)));
                __m256 onv = _mm256_blendv_ps(zero, one, _mm256_cmp_ps(rv, k0055, _CMP_LE_OQ));
                __m256 k0 = _mm256_mul_ps(kpa0, stv);
                k0 = _mm256_mul_ps(k0, onv);
                k0 = _mm256_mul_ps(k0, twv);
                k0 = _mm256_mul_ps(k0, kp10b);
                k0 = _mm256_mul_ps(k0, k17);
                c0v = _mm256_add_ps(c0v, k0);
                __m256 k1 = _mm256_mul_ps(kpa1, stv);
                k1 = _mm256_mul_ps(k1, onv);
                k1 = _mm256_mul_ps(k1, twv);
                k1 = _mm256_mul_ps(k1, kp10b);
                k1 = _mm256_mul_ps(k1, k17);
                c1v = _mm256_add_ps(c1v, k1);
                __m256 k2 = _mm256_mul_ps(kpa2, stv);
                k2 = _mm256_mul_ps(k2, onv);
                k2 = _mm256_mul_ps(k2, twv);
                k2 = _mm256_mul_ps(k2, kp10b);
                k2 = _mm256_mul_ps(k2, k17);
                c2v = _mm256_add_ps(c2v, k2);
            }
            // sink dim: rad on the *normalized* d, (0.30*rad)*rad order kept
            __m256 rad = _mm256_mul_ps(
                _mm256_sqrt_ps(
                    _mm256_add_ps(_mm256_mul_ps(nx, nx), _mm256_mul_ps(ny, ny))),
                k115);
            rad = _mm256_min_ps(_mm256_max_ps(rad, zero), one);
            __m256 sink = _mm256_sub_ps(one, _mm256_mul_ps(_mm256_mul_ps(k030, rad), rad));
            __m256 e0 = _mm256_mul_ps(c0v, sink);
            __m256 e1 = _mm256_mul_ps(c1v, sink);
            __m256 e2 = _mm256_mul_ps(c2v, sink);
            // interleave (AoS) and store: 8 pixels × 3 floats, contiguous slice
            alignas(32) float tmp[24];
            _mm256_store_ps(tmp + 0, e0);
            _mm256_store_ps(tmp + 8, e1);
            _mm256_store_ps(tmp + 16, e2);
            float* op = col + ((size_t)row * w + x) * 3;
            for (int p = 0; p < 8; p++) {
                op[p * 3] = tmp[p];
                op[p * 3 + 1] = tmp[8 + p];
                op[p * 3 + 2] = tmp[16 + p];
            }
            _mm256_storeu_ps(dep + (size_t)row * w + x, _mm256_set1_ps(300.0f));
        }
        for (int xi = x; xi < w; xi++)
            skydome_px_scalar(col, dep, w, h, row, xi, fwd, rgt, up, tan_h, aspect, yn, pal,
                              need_cloud, need_stars, cloud, hw, hh, cell_tw, st);
    }
}
#endif

static skydome_kfn skydome_kernel() {
    static skydome_kfn fn =
#if CHAMINA_SIMD_AVX2
        simd_avx2() ? &skydome_rows_avx2 :
#endif
        &skydome_rows_scalar;
    return fn;
}

// Half-res nebula prepass, AVX2 half. Per lane: ray (same hoists/shared
// normalize helper), the Tier B atan2f_vec256 polynomial, then fbm2 in lanes —
// 2 octaves ×
// 4 corner gathers with the exact scalar hash2 fallback blended on
// out-of-domain lanes (bird/fuzz scenes never leave the table domain; the
// fallback exists so out-of-range scenes can't silently change values).
__attribute__((target("avx2"), optimize("fp-contract=off")))
static void cloud_rows_avx2(float* cl, int hw, int hh, int y0, int y1, int w, int h,
                            const float* fwd, const float* rgt, const float* up, float tan_h,
                            float aspect, float t, const float* pal) {
    const float* tab = lattice_hash_tab();
    const __m256 half = _mm256_set1_ps(0.5f);
    const __m256 one = _mm256_set1_ps(1.0f);
    const __m256 zero = _mm256_setzero_ps();
    const __m256i klanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256 kfx = _mm256_set1_ps(fwd[0]);
    const __m256 kfy = _mm256_set1_ps(fwd[1]);
    const __m256 kfz = _mm256_set1_ps(fwd[2]);
    const __m256 krx = _mm256_set1_ps(rgt[0]);
    const __m256 kry = _mm256_set1_ps(rgt[1]);
    const __m256 krz = _mm256_set1_ps(rgt[2]);
    const __m256 kux = _mm256_set1_ps(up[0]);
    const __m256 kuy = _mm256_set1_ps(up[1]);
    const __m256 kuz = _mm256_set1_ps(up[2]);
    const __m256 kt = _mm256_set1_ps(tan_h);
    const __m256 ka = _mm256_set1_ps(aspect);
    const __m256 kw = _mm256_set1_ps((float)w);
    const __m256 kchw = _mm256_set1_ps((float)hw);
    const __m256 kpi = _mm256_set1_ps((float)M_PI);
    const __m256 ktwo = _mm256_set1_ps(2.0f);
    const __m256 k7 = _mm256_set1_ps(7.0f);
    const __m256 k35 = _mm256_set1_ps(3.5f);
    const __m256 k040 = _mm256_set1_ps(0.40f);
    // the scalar divides by the compiler-folded (0.92f - 0.40f); same fold here
    const __m256 k052 = _mm256_set1_ps(0.92f - 0.40f);
    // drift: (t*pal[11]) once, then each multiplier separately — bitwise the
    // per-pixel scalar sub-products westill fold
    const float tk = t * pal[11];
    const __m256 kd18 = _mm256_set1_ps(tk * 0.018f);
    const __m256 kd09 = _mm256_set1_ps(tk * 0.009f);
    for (int row = y0; row < y1; row++) {
        float py = (row + 0.5f) * h / hh;
        float yn = 1 - 2 * py / h;
        const __m256 kb0 = _mm256_set1_ps(yn * tan_h);
        const __m256 upxb = _mm256_mul_ps(kux, kb0);
        const __m256 upyb = _mm256_mul_ps(kuy, kb0);
        const __m256 upzb = _mm256_mul_ps(kuz, kb0);
        int x = 0;
        for (; x + 8 <= hw; x += 8) {
            __m256i kb = _mm256_add_epi32(klanes, _mm256_set1_epi32(x));
            __m256 xf = _mm256_cvtepi32_ps(kb);
            // px = (x+0.5)*w/hw; xn = 2*px/w - 1 (2*px exact via + self)
            __m256 px = _mm256_div_ps(_mm256_mul_ps(_mm256_add_ps(xf, half), kw), kchw);
            __m256 xn = _mm256_sub_ps(_mm256_div_ps(_mm256_add_ps(px, px), kw), one);
            __m256 rks = _mm256_mul_ps(_mm256_mul_ps(xn, kt), ka);
            __m256 dx = _mm256_add_ps(_mm256_add_ps(kfx, _mm256_mul_ps(krx, rks)), upxb);
            __m256 dy = _mm256_add_ps(_mm256_add_ps(kfy, _mm256_mul_ps(kry, rks)), upyb);
            __m256 dz = _mm256_add_ps(_mm256_add_ps(kfz, _mm256_mul_ps(krz, rks)), upzb);
            __m256 nx, ny, nz;
            norm_ray_lanes256(dx, dy, dz, &nx, &ny, &nz);
            __m256 uv0 = _mm256_add_ps(
                _mm256_div_ps(_mm256_div_ps(atan2f_vec256(nz, nx), kpi), ktwo), half);
            __m256 uv1 = _mm256_add_ps(_mm256_mul_ps(ny, half), half);
            // qx = (uv0*7 + t*pal[11]*0.018)*2, qy likewise with 3.5/0.009
            __m256 qx = _mm256_mul_ps(_mm256_add_ps(_mm256_mul_ps(uv0, k7), kd18), ktwo);
            __m256 qy = _mm256_mul_ps(_mm256_add_ps(_mm256_mul_ps(uv1, k35), kd09), ktwo);
            __m256 n = fbm2_lanes256(qx, qy, tab);
            __m256 kk = _mm256_min_ps(
                _mm256_max_ps(_mm256_div_ps(_mm256_sub_ps(n, k040), k052), zero), one);
            __m256 stv = _mm256_mul_ps(
                _mm256_mul_ps(kk, kk),
                _mm256_sub_ps(_mm256_set1_ps(3.0f), _mm256_add_ps(kk, kk)));
            _mm256_storeu_ps(cl + (size_t)row * hw + x, stv);
        }
        (void)upxb; (void)upyb; (void)upzb;  // silences unused-warn in odd shapes
        for (int xi = x; xi < hw; xi++)
            cloud_px_scalar(cl, hw, w, row, xi, fwd, rgt, up, tan_h, aspect, yn,
                            tk * 0.018f, tk * 0.009f);
    }
}

static cloud_kfn cloud_kernel() {
    static cloud_kfn fn =
#if CHAMINA_SIMD_AVX2
        simd_avx2() ? &cloud_rows_avx2 :
#endif
        &cloud_rows_scalar;
    return fn;
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
    tonemap_into((const float*)f.color.data(), (float*)srgb.data(), f.color.size());
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
#pragma omp parallel for schedule(static)
    for (long i = 0; i < (long)src.size(); i++) {
        bytes[i * 3] = (uint8_t)(clamp(src[i][0], 0.0f, 1.0f) * 255 + 0.5f);
        bytes[i * 3 + 1] = (uint8_t)(clamp(src[i][1], 0.0f, 1.0f) * 255 + 0.5f);
        bytes[i * 3 + 2] = (uint8_t)(clamp(src[i][2], 0.0f, 1.0f) * 255 + 0.5f);
    }
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
// cuts, the fx rack and the colour palette. Also ported: --layout field /
// --groups auto (js/layout.mjs — serpentine world positions, per-cue camera
// stops, per-line exits, word-group choreography and audio-energy whips).
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
// Math.max(...arr) / Math.min(...arr): a NaN anywhere poisons the whole call
// (unlike std::max_element, which skips comparators against NaN). Anywhere the
// JS folds a possibly-NaN array through the spread, use these.
static double js_max_of(const vector<double>& a) {
    double acc = -numeric_limits<double>::infinity();
    for (double v : a) acc = js_max(acc, v);
    return acc;
}
static double js_min_of(const vector<double>& a) {
    double acc = numeric_limits<double>::infinity();
    for (double v : a) acc = js_min(acc, v);
    return acc;
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
// gen: word grouping (port of js/lrc.js wordGroups / groupCharTimes) — the
// `--groups auto` path. A group always breaks at a space (the space rides
// with the word before it) and each phrase between spaces is cut by a rhythm
// picked from the line/phrase index; the fixed lyric corpus is hand-worded
// instead of guessed.
// ---------------------------------------------------------------------------

static const double SPACE_EM = 0.2959;  // advance of a space in the layout font

// Advance of a line in em, mirroring `text::layout` in Rust. Everything that
// is neither CJK nor a space is counted a whole em — an overestimate only
// ever makes the frame budget more conservative. (The CJK branch adds 1 too,
// so only the space is special.)
static double em_width(const string& text) {
    double em = 0;
    for (size_t i = 0; i < text.size();) {
        size_t l;
        uint32_t cp = utf8_cp(text, i, l);
        i += l;
        em += cp == ' ' ? SPACE_EM : 1.0;
    }
    return em;
}

// js/lrc.js thirds / chunks — the two helpers PHRASE_CUTS composes with.
static vector<int> thirds_cut(int n) {
    int a = (int)js_round(n / 3.0);
    int b = (int)js_round((n - a) / 2.0);
    return {a, b, n - a - b};
}
static vector<int> chunks_cut(int n, int k) {
    vector<int> out;
    int left = n;
    for (int i = k; i > 0; i--) {
        int take = (int)js_max(1.0, js_round(left / (double)i));
        out.push_back((int)js_min((double)take, (double)(left - (i - 1))));
        left -= out.back();
    }
    vector<int> nz;
    for (int x : out)
        if (x > 0) nz.push_back(x);
    return nz;
}

// phrase cut table (js/lrc.js PHRASE_CUTS — fifteen rhythms, index = kind)
typedef vector<int> (*CutFn)(int);
static vector<int> cut_all(int n) { return {n}; }
static vector<int> cut_1_rest(int n) { return n >= 2 ? vector<int>{1, n - 1} : vector<int>{n}; }
static vector<int> cut_2_rest(int n) { return n >= 3 ? vector<int>{2, n - 2} : vector<int>{n}; }
static vector<int> cut_11_rest(int n) { return n >= 4 ? vector<int>{1, 1, n - 2} : vector<int>{n}; }
static vector<int> cut_rest_1(int n) { return n >= 2 ? vector<int>{n - 1, 1} : vector<int>{n}; }
static vector<int> cut_half(int n) {
    // Math.ceil(n/2), Math.floor(n/2)
    return n >= 2 ? vector<int>{(n + 1) / 2, n / 2} : vector<int>{n};
}
static vector<int> cut_thirds(int n) { return n >= 3 ? thirds_cut(n) : vector<int>{n}; }
static vector<int> cut_1_rest_1(int n) { return n >= 4 ? vector<int>{1, n - 2, 1} : vector<int>{n}; }
static vector<int> cut_chunks2(int n) { return chunks_cut(n, 2); }
static vector<int> cut_chunks3(int n) { return chunks_cut(n, 3); }
static vector<int> cut_12_rest(int n) { return n >= 5 ? vector<int>{1, 2, n - 3} : vector<int>{n}; }
static vector<int> cut_rest_21(int n) { return n >= 5 ? vector<int>{n - 3, 2, 1} : vector<int>{n}; }
static vector<int> cut_22_rest(int n) { return n >= 6 ? vector<int>{2, 2, n - 4} : vector<int>{n}; }
static vector<int> cut_31_rest(int n) { return n >= 6 ? vector<int>{3, 1, n - 4} : vector<int>{n}; }
static vector<int> cut_1_mid_rest(int n) {
    if (n >= 3) {
        int mid = (n - 1 + 1) / 2;  // Math.ceil((n-1)/2)
        return {1, mid, n - 1 - mid};
    }
    return {n};
}
static const CutFn PHRASE_CUTS[] = {
    cut_all,     cut_1_rest,  cut_2_rest,   cut_11_rest, cut_rest_1,
    cut_half,    cut_thirds,  cut_1_rest_1, cut_chunks2, cut_chunks3,
    cut_12_rest, cut_rest_21, cut_22_rest,  cut_31_rest, cut_1_mid_rest,
};
static const int PHRASE_CUT_COUNT = (int)(sizeof PHRASE_CUTS / sizeof PHRASE_CUTS[0]);

// Hand-worded segmentation for the fixed lyric corpus (js/lrc.js
// WORD_SPLIT_SPECS): line text → '|'-separated words, the trailing space of a
// gap riding with the word before it so the pieces tile the whole string.
struct SplitSpec {
    const char* text;
    const char* spec;
};
static const SplitSpec WORD_SPLIT_SPECS[] = {
    {"远离那些繁华悲伤 我们从此告别了过往", "远离|那些|繁华|悲伤 |我们|从此|告别了|过往"},
    {"飞鸟张开时光翅膀 经历了无数风雨阻挡", "飞鸟|张开|时光|翅膀 |经历了|无数|风雨|阻挡"},
    {"来到她身旁 谁的目光 停留在那个梦里的夜晚",
     "来到|她|身旁 |谁的|目光 |停留|在|那个|梦里|的|夜晚"},
    {"我们追随风的方向 遥远飞行地老天荒 别彷徨",
     "我们|追随|风的|方向 |遥远|飞行|地老天荒 |别|彷徨"},
    {"在初遇的那地方 我们曾眷恋不已 梦想", "在|初遇|的|那地方 |我们|曾|眷恋|不已 |梦想"},
    {"面对迷惘 也不要放弃了希望", "面对|迷惘 |也|不要|放弃了|希望"},
    {"就像是盛开的花 对阳光不变的那信仰", "就像是|盛开|的|花 |对|阳光|不变的|那|信仰"},
    {"天空里早已留下了 永恒的神话", "天空里|早已|留下了 |永恒的|神话"},
    {"在夏天的操场 静静地眺望 在远方", "在|夏天|的|操场 |静静地|眺望 |在|远方"},
    {"连绵不断 是回忆之间的海洋", "连绵|不断 |是|回忆|之间|的|海洋"},
    {"穿过云端 是谁的天堂 在召唤", "穿过|云端 |是|谁的|天堂 |在|召唤"},
    {"她在寻访 那沉睡千年的答案", "她|在|寻访 |那|沉睡|千年|的|答案"},
    {"在告别的那地方 我们都无法放下的伤感",
     "在|告别|的|那地方 |我们|都|无法|放下|的|伤感"},
    {"挥一挥手留下的是美丽愿望", "挥一挥手|留下的|是|美丽|愿望"},
    {"就像是逆风的帆 对海浪不屈的那坚强", "就像是|逆风|的|帆 |对|海浪|不屈|的|那|坚强"},
    {"秋天的夜晚 静静地回想 在远方", "秋天|的|夜晚 |静静地|回想 |在|远方"},
    {"挥去不散 我们怀念着的故乡", "挥去|不散 |我们|怀念着|的|故乡"},
    {"在我们身旁", "在|我们|身旁"},
    {"她的目光 飞翔在每个梦里的夜晚", "她的|目光 |飞翔|在|每个|梦里|的|夜晚"},
    {"我们追随风的方向 遥远飞行地久天长 不返航",
     "我们|追随|风的|方向 |遥远|飞行|地久天长 |不|返航"},
};
// PHRASE_CUT_OVERRIDES（js/lrc.js）—— `lineIndex:phraseIndex` → 表项：第 0 行
// 的两个短语由手写节拍替代公式节拍，在 word_groups 内联生效（0:0 → 表[3]，
// 0:1 → 表[12]）。

struct WordGroup {
    int i0, i1;   // half-open codepoint range
    double t;     // the time the whole group lands on (its first glyph's stamp)
    int kind;     // phrase-cut index (fallback groups carry -1)
};

static vector<WordGroup> word_groups(const string& text, const vector<double>& char_times,
                                     int seed) {
    if (char_times.empty()) return {};
    // one byte offset per codepoint (spaces break phrases at codepoint level)
    vector<size_t> off;
    for (size_t i = 0; i < text.size();) {
        size_t l;
        utf8_cp(text, i, l);
        off.push_back(i);
        i += l;
    }
    size_t n = min(off.size(), char_times.size());
    if (n == 0) return {};
    vector<WordGroup> out;
    for (const auto& sp : WORD_SPLIT_SPECS) {
        if (text != sp.text) continue;
        const string specs = sp.spec;
        size_t i = 0, pos = 0;
        while (true) {
            size_t bar = specs.find('|', pos);
            string piece = specs.substr(pos, bar == string::npos ? string::npos : bar - pos);
            if (i < n) {
                size_t len = 0;
                for (size_t j = 0; j < piece.size();) {
                    size_t l;
                    utf8_cp(piece, j, l);
                    j += l;
                    len++;
                }
                size_t end = min(i + len, n);
                out.push_back({(int)i, (int)end, char_times[i], seed % PHRASE_CUT_COUNT});
                i = end;
            }
            if (bar == string::npos) break;
            pos = bar + 1;
        }
        // A corpus line that grew past its table entry keeps the remainder on
        // the last group rather than dropping glyphs on the floor.
        if (i < n) out.push_back({(int)i, (int)n, char_times[i], seed % PHRASE_CUT_COUNT});
        return out;
    }
    size_t phrase_start = 0;
    int phrase_index = 0;
    auto flush = [&](size_t phrase_end) {
        size_t len = phrase_end - phrase_start;
        if (len == 0) return;
        int kind = (seed * 5 + phrase_index * 3) % PHRASE_CUT_COUNT;
        // PHRASE_CUT_OVERRIDES — "0:0" → PHRASE_CUTS[3], "0:1" → PHRASE_CUTS[12]
        CutFn cut = PHRASE_CUTS[kind];
        if (seed == 0 && phrase_index == 0) cut = PHRASE_CUTS[3];
        else if (seed == 0 && phrase_index == 1) cut = PHRASE_CUTS[12];
        size_t i = phrase_start;
        for (int piece : cut((int)len)) {
            if (piece <= 0) continue;
            // charTimes[i] can be undefined: the piece sum may run past the
            // line when chunks(...) filters to a longer run. undefined joins
            // later arithmetic as NaN (same as storing NaN directly).
            double t0 = i < char_times.size()
                            ? char_times[i]
                            : numeric_limits<double>::quiet_NaN();
            out.push_back({(int)i, (int)(i + piece), t0, kind});
            i += piece;
        }
        phrase_index += 1;
    };
    // Cut *after* the space, so the gap rides with the word it follows and
    // the groups tile the whole line with no holes.
    for (size_t i = 0; i < n; i++) {
        size_t l;
        uint32_t cp = utf8_cp(text, off[i], l);
        if (cp == ' ') {
            flush(i + 1);
            phrase_start = i + 1;
        }
    }
    flush(n);
    return out;
}

// Compress per-glyph stamps onto their group, then sweep the glyphs inside a
// group 35 ms apart (js/lrc.js groupCharTimes).
static vector<double> group_char_times(const string& text, const vector<double>& ct, int seed) {
    auto gs = word_groups(text, ct, seed);
    if (gs.empty()) return ct;
    vector<double> out = ct;
    for (const auto& g : gs)
        for (int i = g.i0; i < g.i1; i++) {
            // A JS array grows in place on out[i] = v when i == length: piece
            // sums can run past the source stamps, extending the sweep by one
            // entry per group tail (a hole would read undefined → NaN, so fill
            // the vector with NaN the same way).
            if ((size_t)i >= out.size())
                out.resize((size_t)i + 1, numeric_limits<double>::quiet_NaN());
            out[i] = g.t + (i - g.i0) * 0.035;
        }
    return out;
}

// ---------------------------------------------------------------------------
// gen: field layout (port of js/layout.mjs layoutField) — serpentine world
// positions, per-cue camera stops and per-line exits. All double: the output
// is scene JSON, so it must round-trip exactly like node's, unlike the
// renderer's float pipeline.
// ---------------------------------------------------------------------------

using d3 = std::array<double, 3>;

static double clamp_d(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

// js/anim.mjs easeApply — clamp01 first, then the curve (all double).
static double ease_apply_d(const string& name, double x) {
    x = clamp_d(x, 0.0, 1.0);
    if (name == "linear") return x;
    if (name == "ease_in_quad") return x * x;
    if (name == "ease_out_quad") return 1 - (1 - x) * (1 - x);
    if (name == "ease_in_out_cubic") return x < 0.5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2;
    if (name == "back_out") {
        const double C1 = 1.70158, C3 = C1 + 1;
        double u = x - 1;
        return 1 + C3 * u * u * u + C1 * u * u;
    }
    return x * x * (3 - 2 * x);  // smooth (default)
}

// js/anim.mjs evalOrbit — movement easing + sequential stop easing with the
// exact from* chaining (fr/fe/…), all double.
static void eval_orbit_d(const ojson& o, double t, double& radius, double& elevation,
                         d3& target, double& yaw, double& roll) {
    radius = o.at("radius").get<double>();
    elevation = o.at("elevation").get<double>();
    target = {o.at("target")[0].get<double>(), o.at("target")[1].get<double>(),
              o.at("target")[2].get<double>()};
    double yaw_off = 0, roll_off = 0;
    auto m = o.find("movement");
    if (m != o.end() && m->is_object()) {
        double at = m->contains("at") && (*m)["at"].is_number() ? (*m)["at"].get<double>() : 0.0;
        double dur = m->contains("duration") && (*m)["duration"].is_number()
                         ? (*m)["duration"].get<double>()
                         : 1.0;
        double k = ease_apply_d(m->value("ease", string("ease_in_out_cubic")), (t - at) / max(dur, 1e-6));
        auto re = m->find("radius_end");
        if (re != m->end() && re->is_number()) radius = radius + (re->get<double>() - radius) * k;
        auto ee = m->find("elevation_end");
        if (ee != m->end() && ee->is_number())
            elevation = elevation + (ee->get<double>() - elevation) * k;
        auto te = m->find("target_end");
        if (te != m->end() && te->is_array())
            target = {target[0] + ((*te)[0].get<double>() - target[0]) * k,
                      target[1] + ((*te)[1].get<double>() - target[1]) * k,
                      target[2] + ((*te)[2].get<double>() - target[2]) * k};
    }
    auto stops = o.find("stops");
    if (stops != o.end() && stops->is_array()) {
        double fr = radius, fe = elevation, fyaw = yaw_off, froll = roll_off;
        d3 ft = target;
        for (const auto& s : *stops) {
            double at = s.contains("at") && s["at"].is_number() ? s["at"].get<double>() : 0.0;
            if (t < at) break;
            double dur = s.contains("duration") && s["duration"].is_number()
                             ? s["duration"].get<double>()
                             : 1.0;
            double k = ease_apply_d(s.contains("ease") && s["ease"].is_string()
                                        ? s["ease"].get<string>()
                                        : string("ease_in_out_cubic"),
                                    (t - at) / max(dur, 1e-6));
            auto rad = s.find("radius");
            double tr = rad != s.end() && rad->is_number() ? rad->get<double>() : fr;
            auto elv = s.find("elevation");
            double te2 = elv != s.end() && elv->is_number() ? elv->get<double>() : fe;
            d3 tt = ft;
            auto tg = s.find("target");
            if (tg != s.end() && tg->is_array())
                tt = {(*tg)[0].get<double>(), (*tg)[1].get<double>(), (*tg)[2].get<double>()};
            auto yw = s.find("yaw");
            double tyaw = yw != s.end() && yw->is_number() ? yw->get<double>() : fyaw;
            auto rl = s.find("roll");
            double troll = rl != s.end() && rl->is_number() ? rl->get<double>() : froll;
            radius = fr + (tr - fr) * k;
            elevation = fe + (te2 - fe) * k;
            target = {ft[0] + (tt[0] - ft[0]) * k, ft[1] + (tt[1] - ft[1]) * k,
                      ft[2] + (tt[2] - ft[2]) * k};
            yaw_off = fyaw + (tyaw - fyaw) * k;
            roll_off = froll + (troll - froll) * k;
            fr = tr;
            fe = te2;
            ft = tt;
            fyaw = tyaw;
            froll = troll;
        }
    }
    yaw = (o.contains("start_yaw") && o["start_yaw"].is_number() ? o["start_yaw"].get<double>() : 0.0) +
          (o.contains("speed") && o["speed"].is_number() ? o["speed"].get<double>() : 0.0) * t +
          yaw_off;
    roll = roll_off;
}

struct BasisD {
    d3 eye, fwd, right, up;
};

// js/anim.mjs orbitEye + cameraBasis — pinhole basis built the way
// Object3D.lookAt builds it (world up is +Y).
static BasisD camera_basis_d(double radius, double elevation, const d3& target, double yaw) {
    double ce = cos(elevation), se = sin(elevation);
    d3 eye = {target[0] + radius * ce * cos(yaw), target[1] + radius * se,
              target[2] + radius * ce * sin(yaw)};
    double fx = target[0] - eye[0], fy = target[1] - eye[1], fz = target[2] - eye[2];
    double fl = hypot(hypot(fx, fy), fz);
    if (fl == 0) fl = 1;  // Math.hypot(...) || 1
    fx /= fl;
    fy /= fl;
    fz /= fl;
    // right = normalize(cross(forward, worldUp)); cross((x,y,z),(0,1,0)) = (-z,0,x)
    double rx = -fz, ry = 0.0, rz = fx;
    double rl = hypot(hypot(rx, ry), rz);
    if (rl == 0) rl = 1;
    rx /= rl;
    ry /= rl;
    rz /= rl;
    // up' = cross(right, forward)
    d3 up = {ry * fz - rz * fy, rz * fx - rx * fz, rx * fy - ry * fx};
    return {eye, {fx, fy, fz}, {rx, ry, rz}, up};
}

// js/anim.mjs projectNDC — NDC of p in [-1,1] across the frame; false when it
// sits behind the eye (JS null).
static bool project_ndc_d(const BasisD& b, const d3& p, double fov, double aspect, double& x,
                          double& y) {
    double vx = p[0] - b.eye[0], vy = p[1] - b.eye[1], vz = p[2] - b.eye[2];
    double zc = vx * b.fwd[0] + vy * b.fwd[1] + vz * b.fwd[2];
    if (zc <= 1e-6) return false;
    double xc = vx * b.right[0] + vy * b.right[1] + vz * b.right[2];
    double yc = vx * b.up[0] + vy * b.up[1] + vz * b.up[2];
    double th = tan(fov / 2);
    x = (xc / zc) / (th * aspect);
    y = (yc / zc) / th;
    return true;
}

// js/layout.mjs rng — mulberry-style counter, Math.imul arithmetic.
static uint32_t js_imul(uint32_t a, uint32_t b) { return (uint32_t)((uint64_t)a * b); }
static uint32_t to_uint32(double v) {
    double m = fmod(v, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (uint32_t)m;
}
struct JsRng {
    uint32_t s;
    explicit JsRng(double seed) {
        uint32_t u = to_uint32(seed);
        s = u ? u : 1;
    }
    double next() {
        s += 0x6d2b79f5u;  // (s + k) >>> 0
        uint32_t t = s;
        t = js_imul(t ^ (t >> 15), t | 1);
        t = t ^ (uint32_t)(t + js_imul(t ^ (t >> 7), t | 61));
        return (double)(uint32_t)(t ^ (t >> 14)) / 4294967296.0;
    }
};

struct Stage {
    double r_add, sx, sy, el;
};
// Framing presets, cycled by line index. rAdd dollies out (which is what
// creates room for sx); sx/sy are the line's slot in NDC; el tilts the orbit.
static const Stage STAGES[] = {
    {0.0, 0.0, 0.0, 0.0},    // 0 centre close-up
    {0.6, -0.16, 0.14, 0.03},   // 1 slight wide, upper-left
    {0.25, 0.07, -0.2, -0.02},  // 2 near, lower-right
    {0.8, 0.0, 0.24, 0.04},     // 3 high angle, top third
    {0.45, 0.14, -0.08, -0.01}, // 4 near, right
    {0.15, -0.04, -0.22, 0.02}, // 5 low close, bottom third
    {0.65, 0.1, 0.1, -0.03},    // 6 near diagonal
};
static const int STAGE_COUNT = (int)(sizeof STAGES / sizeof STAGES[0]);

// The bank and swing carried by each flight.
static const double FLY_YAW[] = {0.05, -0.07, 0.03, -0.05, 0.06, -0.03, 0.04};
static const double FLY_ROLL[] = {0.03, -0.04, 0.05, -0.02, 0.04, -0.05, 0.02};
static const int FLY_COUNT = (int)(sizeof FLY_YAW / sizeof FLY_YAW[0]);

// The vocabulary of a move; one nonzero axis each, insertion order = the
// order the rng draws kinds in.
struct MoveSpec {
    const char* name;
    double d_roll, d_yaw, d_r, d_e, dx, dy;
};
static const MoveSpec MOVES[] = {
    {"rollL", -1, 0, 0, 0, 0, 0}, {"rollR", 1, 0, 0, 0, 0, 0},
    {"yawL", 0, -1, 0, 0, 0, 0},  {"yawR", 0, 1, 0, 0, 0, 0},
    {"push", 0, 0, -1, 0, 0, 0},  {"pull", 0, 0, 1, 0, 0, 0},
    {"crane", 0, 0, 0, 1, 0, 0},  {"tilt", 0, 0, 0, -1, 0, 0},
    {"down", 0, 0, 0, 0, 0, -1},  {"up", 0, 0, 0, 0, 0, 1},
    {"right", 0, 0, 0, 0, 1, 0},  {"left", 0, 0, 0, 0, -1, 0},
};
static const int MOVE_COUNT = (int)(sizeof MOVES / sizeof MOVES[0]);
static const char* EASE_NAMES[] = {"ease_in_out_cubic", "ease_out_quad", "ease_in_quad",
                                   "smooth"};

struct ChoreoMove {
    string kind;
    double mag = 0, lead = 0, pace = 0;
    string ease;
    int spec_idx = 0;  // row in MOVES (kind's lookup index)
};

// One line's phrase: an optional kick on the beat itself, then one move per
// gap between word groups (js/layout.mjs rollChoreo).
struct Phrase {
    ChoreoMove kick;
    bool has_kick = false;
    vector<ChoreoMove> between;
};

// Roll one move: which axis, how far, which ease, and whether it leads. The
// magnitude band follows the axis: banks 0.05-0.16, swings 0.07-0.20,
// dolly 0.5-1.5, tilt 0.02-0.07, pans 0.06-0.15.
static ChoreoMove roll_move(JsRng& r) {
    int kidx = (int)floor(r.next() * MOVE_COUNT);
    if (kidx >= MOVE_COUNT) kidx = MOVE_COUNT - 1;
    if (kidx < 0) kidx = 0;
    const MoveSpec& spec = MOVES[kidx];
    ChoreoMove m;
    m.kind = spec.name;
    m.spec_idx = kidx;
    if (spec.d_roll != 0)
        m.mag = 0.05 + r.next() * 0.11;
    else if (spec.d_yaw != 0)
        m.mag = 0.07 + r.next() * 0.13;
    else if (spec.d_r != 0)
        m.mag = 0.5 + r.next() * 1.0;
    else if (spec.d_e != 0)
        m.mag = 0.02 + r.next() * 0.05;
    else
        m.mag = 0.06 + r.next() * 0.09;
    m.ease = EASE_NAMES[min((size_t)floor(r.next() * 4), (size_t)3)];
    // > 0 the camera settles before the word lands, < 0 it trails it.
    m.lead = -0.16 + r.next() * 0.42;
    // Fraction of the gap to the next beat the move is allowed to spend.
    m.pace = 0.5 + r.next() * 0.35;
    return m;
}

// One line's phrase: an optional kick on the beat itself, then one move per
// gap between word groups.
static Phrase roll_choreo(int groups, int seed) {
    JsRng r(seed);
    Phrase p;
    if (r.next() < 0.72) {
        p.kick = roll_move(r);
        p.has_kick = true;
    }
    for (int j = 1; j < groups; j++) p.between.push_back(roll_move(r));
    return p;
}

static string signature_of(const Phrase& ch) {
    string s = ch.has_kick ? ch.kick.kind : "-";
    s += ">";
    for (size_t j = 0; j < ch.between.size(); j++) {
        if (j) s += ",";
        s += ch.between[j].kind;
    }
    return s;
}

// Give every line its own phrase; signatures are unique by construction (a
// collision re-rolls that line with a new salt). Line 0 keeps the hand-written
// opening in front of its roll.
static vector<Phrase> assign_choreo(const vector<int>& group_counts) {
    vector<string> used;
    vector<Phrase> out;
    for (size_t i = 0; i < group_counts.size(); i++) {
        int k = group_counts[i];
        bool is_first = i == 0;
        int need = k - 1 > 0 ? k - 1 : 0;
        ChoreoMove prefix_kick;
        vector<ChoreoMove> prefix_between;
        int kept = 0;
        if (is_first) {
            prefix_kick.kind = "down";
            prefix_kick.spec_idx = 8;  // MOVES: down
            prefix_kick.mag = 0.34;
            prefix_kick.ease = "ease_out_quad";
            prefix_kick.lead = -0.12;
            prefix_kick.pace = 0.85;
            ChoreoMove b0;
            b0.kind = "up";
            b0.spec_idx = 9;
            b0.mag = 0.13;
            b0.ease = "smooth";
            b0.lead = 0.1;
            b0.pace = 0.6;
            ChoreoMove b1;
            b1.kind = "right";
            b1.spec_idx = 10;
            b1.mag = 0.31;
            b1.ease = "ease_in_out_cubic";
            b1.lead = 0.14;
            b1.pace = 0.8;
            prefix_between = {b0, b1};
            kept = (int)min(prefix_between.size(), (size_t)need);
        }
        bool placed = false;
        for (int salt = 0; salt < 80 && !placed; salt++) {
            JsRng r(to_uint32((double)i * 7919.0 + (double)salt * 104729.0 + 17.0));
            Phrase ch;
            if (is_first) {
                ch.kick = prefix_kick;
                ch.has_kick = true;
                for (int j = 0; j < kept; j++) ch.between.push_back(prefix_between[j]);
                for (int j = kept; j < need; j++) ch.between.push_back(roll_move(r));
            } else {
                if (r.next() < 0.72) {
                    ch.kick = roll_move(r);
                    ch.has_kick = true;
                }
                for (int j = 0; j < need; j++) ch.between.push_back(roll_move(r));
            }
            string sig = signature_of(ch);
            bool dup = false;
            for (const auto& u : used)
                if (u == sig) dup = true;
            if (!dup) {
                used.push_back(sig);
                out.push_back(move(ch));
                placed = true;
            }
        }
        if (!placed) out.push_back(roll_choreo(k, (int)i + 1));
    }
    return out;
}

// ---------------------------------------------------------------------------
// gen: field layout (port of js/layout.mjs layoutField) — serpentine world
// positions, per-cue camera stops and per-line exits. All double: the output
// is scene JSON, so it must round-trip exactly like node's, unlike the
// renderer's float pipeline.
// ---------------------------------------------------------------------------

// Serpentine grid (GRID + gridPositions): row 0 runs from the middle column to
// the end so line 0 lands on the world origin where the default camera already
// looks; rows 1.. then snake left and right.
static vector<d3> grid_positions(int n) {
    const int cols = 5;
    const double x_step = 30, y_step = 15;
    double half = (cols - 1) / 2.0;
    vector<d3> out;
    int row = 0, col = (int)half, dir = 1;
    for (int i = 0; i < n; i++) {
        out.push_back({(col - half) * x_step, row * y_step, 0.0});
        if (dir > 0) {
            if (col < cols - 1) col += 1;
            else {
                row += 1;
                dir = -1;
            }
        } else if (col > 0) {
            col -= 1;
        } else {
            row += 1;
            dir = 1;
        }
    }
    return out;
}

// Does the line still fit the frame from this framing? (framingFits) A rolled
// picture rotates the line's screen box; inflate the box by the roll's
// axis-aligned extent and test the four corners.
static bool framing_fits(const d3& P, const d3& target, double radius, double elevation,
                         double yaw, double roll, double hw, double hh, double th,
                         double aspect, double fov) {
    BasisD basis = camera_basis_d(radius, elevation, target, yaw);
    double r = roll != 0 && !isnan(roll) ? roll : 0.0;  // Math.cos(roll || 0)
    double c = fabs(cos(r)), s = fabs(sin(r));
    double ew = hw * c + hh * s;
    double eh = hh * c + hw * s;
    const d3 pts[4] = {{P[0] - ew, P[1] - eh, P[2]},
                       {P[0] + ew, P[1] - eh, P[2]},
                       {P[0] - ew, P[1] + eh, P[2]},
                       {P[0] + ew, P[1] + eh, P[2]}};
    for (const d3& pt : pts) {
        double x, y;
        if (!project_ndc_d(basis, pt, fov, aspect, x, y)) return false;
        // 7% of the frame is kept back for the outline rim and the bloom bleed.
        if (fabs(x) > 0.93 || fabs(y) > 0.94) return false;
    }
    return true;
}

// Is the axis-aligned box around P (half extents hw/hh) on screen?
// (boxOnFrame — corners behind the eye are tolerated, a box entirely behind
// the eye is not.)
static bool box_on_frame(double radius, double elevation, const d3& target, double yaw,
                         double roll, const d3& P, double hw, double hh, double fov,
                         double aspect) {
    BasisD basis = camera_basis_d(radius, elevation, target, yaw);
    double r = roll != 0 && !isnan(roll) ? roll : 0.0;
    double c = fabs(cos(r)), s = fabs(sin(r));
    double ew = hw * c + hh * s;
    double eh = hh * c + hw * s;
    bool behind = false, any = false;
    double min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
    const d3 pts[4] = {{P[0] - ew, P[1] - eh, P[2]},
                       {P[0] + ew, P[1] - eh, P[2]},
                       {P[0] - ew, P[1] + eh, P[2]},
                       {P[0] + ew, P[1] + eh, P[2]}};
    for (const d3& pt : pts) {
        double x, y;
        if (!project_ndc_d(basis, pt, fov, aspect, x, y)) {
            behind = true;
            continue;
        }
        any = true;
        min_x = js_min(min_x, x);
        max_x = js_max(max_x, x);
        min_y = js_min(min_y, y);
        max_y = js_max(max_y, y);
    }
    if (!any) return false;   // every corner behind the eye
    if (behind) return true;  // straddling the eye: count it as seen
    return !(max_x < -1 || min_x > 1 || max_y < -1 || min_y > 1);
}

struct FieldLayout {
    vector<d3> positions;
    vector<double> exits;  // NaN = leave the block's own timing (--keep-open)
    ojson stops = ojson::array();
};

// Lay the lines out as a camera path (layoutField). See js/layout.mjs for the
// full narration; formulas are kept line-for-line. Energy loading lives with
// the CLI section (load_energy) — it reads a file, the layout itself is pure.
static FieldLayout layout_field(const vector<LrcLine>& lines, const ojson& camera,
                                double duration, double fov, double aspect, double size,
                                double fit_width, double fade_in, double outro_at,
                                double title_out, bool groups_auto, double whip,
                                const vector<double>* energy,
                                const vector<bool>* whip_force) {
    const int n = (int)lines.size();
    FieldLayout out;
    if (!n) return out;

    vector<double> cues(n);
    for (int i = 0; i < n; i++) cues[i] = lines[i].time;
    out.positions = grid_positions(n);

    // Work from the rig without stops: the stops we emit are built on the
    // orbit + movement, and cameraAnimation never has any yet when this runs.
    ojson rig = camera;
    rig.erase("stops");

    double th = tan(fov / 2);
    auto half_w_at = [&](double r) { return th * aspect * r; };
    auto half_h_at = [&](double r) { return th * r; };

    // The tightest the frame ever gets. The v9 rig stays within ±1.3 of the
    // orbit's own radius at that cue; framingFits vetoes anything past the edge.
    double base_r;
    {
        double r = camera.contains("radius") && camera["radius"].is_number()
                       ? camera["radius"].get<double>()
                       : numeric_limits<double>::quiet_NaN();
        base_r = r != 0 && !isnan(r) ? r : 14.0;  // Number(camera.radius) || 14
    }
    double rmax_cap = js_min(16.0, base_r + 1.3);  // rmax=16 (default)

    // World half-extents of each line, matching how Rust resolves fit_width.
    vector<double> box_w(n), box_h(n), reveal_end(n);
    for (int i = 0; i < n; i++) {
        double em = em_width(lines[i].text);
        double width = em > 0 ? js_min(em * size, fit_width) : 0.0;
        double resolved = em > 0 ? width / em : size;
        box_w[i] = width / 2;
        box_h[i] = resolved * 0.55;  // half the em box + rim/bloom margin
        const vector<double>* ctp = lines[i].has_ct ? &lines[i].ct : nullptr;
        reveal_end[i] = ctp && !ctp->empty()
                            ? js_max_of(*ctp)   // Math.max(...charTimes)
                            : cues[i];
    }

    // Return to the title framing before the outro.
    double tail = isfinite(outro_at) ? outro_at : duration;
    double last_cue = cues[n - 1];
    double back_at = tail - 12;
    if (back_at < last_cue + 0.6) back_at = last_cue + 0.6;
    double back_dur = tail - back_at;

    // Flights in, one per line — computed first, because a line's own phrase
    // may never run into the flight that carries the camera to the next one.
    vector<double> flight_at(n), flight_dur(n);
    for (int i = 0; i < n; i++) {
        double next = i + 1 < n ? cues[i + 1] : duration;
        double gap = js_max(next - cues[i], 0.2);
        double capped = js_min(14.0, gap - 0.4);
        double d = js_max(1.2, js_min(js_min(gap * 0.45, capped), 2.0));  // TRAVEL_MAX
        double start = cues[i] - d;
        // One stop may never begin before the previous cue; the opening card
        // holds still until it is gone.
        if (i == 0 && isfinite(title_out)) start = js_max(start, title_out);
        else if (i > 0) start = js_max(start, cues[i - 1] + 0.8);
        start = js_min(start, cues[i] - 0.4);
        flight_at[i] = start;
        flight_dur[i] = js_max(cues[i] - start, 0.4);
    }

    // Whip rolls on the flights. Only energetic lines earn one, and not all of
    // them; the sign is handed out at emission time so flight whips and word
    // whips share one alternation.
    auto energy_at = [&](int i) -> double {
        double v = energy && i < (int)energy->size() ? (*energy)[i] : 0.5;
        v = clamp_d(v, 0.0, 1.0);
        return isnan(v) ? 0.0 : v;  // clamp(NaN) is NaN, `|| 0` fixes it
    };
    auto force_of = [&](int i) -> bool {
        return whip_force && i < (int)whip_force->size() && (*whip_force)[i];
    };
    vector<double> flight_whip(n, 0.0);
    double whip_sign = 0;
    if (whip > 0) {
        for (int i = 0; i < n; i++) {
            JsRng r(to_uint32((double)i * 2654435761.0 + 101.0));
            double e = energy_at(i);
            bool forced = force_of(i);
            // The r() draw is only consumed when the meter is actually asked
            // (JS && short-circuit) — the stream must stay aligned with node's.
            if ((!forced && e < 0.52) || flight_dur[i] < 1.0 || (!forced && r.next() < 0.22))
                continue;
            flight_whip[i] =
                (M_PI / 2) * (0.8 + 0.2 * r.next()) * js_min(whip, 1.0) * (0.72 + 0.28 * e);
        }
    }

    // Per-word choreography: with groups auto every line is cut into words that
    // land together, each gap between them getting its own camera move.
    vector<vector<WordGroup>> group_lists;
    vector<Phrase> choreo;
    if (groups_auto) {
        for (int i = 0; i < n; i++) {
            vector<WordGroup> g =
                word_groups(lines[i].text, lines[i].has_ct ? lines[i].ct : vector<double>(), i);
            if (g.empty()) g.push_back({0, 1, cues[i], -1});
            group_lists.push_back(move(g));
        }
        vector<int> counts;
        for (const auto& g : group_lists) counts.push_back((int)g.size());
        choreo = assign_choreo(counts);
        // The last glyph of a line now lands with its group, so the exit scan
        // waits for the group rather than for the glyph it used to wait for.
        for (int i = 0; i < n; i++) {
            const WordGroup& g = group_lists[i].back();
            reveal_end[i] = g.t + js_max(0.0, (double)(g.i1 - g.i0 - 1)) * 0.035;
        }
    }

    vector<double> line_ends(n, 0.0);
    for (int i = 0; i < n; i++) {
        const Stage& stage = STAGES[i % STAGE_COUNT];
        double base_radius, base_elevation, base_yaw, base_roll;
        d3 base_target;
        eval_orbit_d(rig, cues[i], base_radius, base_elevation, base_target, base_yaw,
                     base_roll);
        // The zoom budget rides with the orbit: ±1.3 around wherever the long
        // dolly has the camera at this cue.
        double line_base = base_radius;
        double r_lo = js_max(11.6, line_base - 1.3);
        double r_hi = js_max(r_lo + 0.8, js_min(rmax_cap, line_base + 1.3));
        double radius = clamp_d(line_base + stage.r_add, r_lo, r_hi);
        double elevation = clamp_d(base_elevation + stage.el, 0.0, 0.42);
        double fw = half_w_at(radius);
        double fh = half_h_at(radius);
        // Room the frame actually has once the line's half-width and a rim/bloom
        // margin are taken out.
        double room_x = js_max(0.0, 1 - 0.15 - box_w[i] / fw);
        double room_y = js_max(0.0, 1 - box_h[i] / fh - 0.1);
        double sx = clamp_d(stage.sx, -room_x, room_x);
        double sy = clamp_d(stage.sy, -room_y, room_y);

        // Screen basis at the cue with target at the origin — orientation only,
        // which is exactly what an independent slot needs.
        BasisD basis = camera_basis_d(radius, elevation, {0, 0, 0}, base_yaw);
        const d3& P = out.positions[i];
        d3 off = {sx * fw * basis.right[0] + sy * fh * basis.up[0],
                  sx * fw * basis.right[1] + sy * fh * basis.up[1],
                  sx * fw * basis.right[2] + sy * fh * basis.up[2]};
        d3 slot = {P[0] - off[0], P[1] - off[1], P[2] - off[2]};
        double fly_yaw = FLY_YAW[i % FLY_COUNT];
        double fly_roll = FLY_ROLL[i % FLY_COUNT];
        // The slot was solved for the plain orbit; if the banked framing cannot
        // hold the line, fly it level instead of shrinking the shot.
        if (!framing_fits(P, slot, radius, elevation, base_yaw + fly_yaw, fly_roll, box_w[i],
                          box_h[i], th, aspect, fov)) {
            fly_yaw = 0;
            fly_roll = 0;
        }

        // A whipped transition is three stops: bank in while the outgoing
        // framing holds, fly while the bank unwinds past level (back_out
        // overshoots — the whip-crack), then settle level just after the beat.
        double whip_mag = flight_whip[i];
        double flight_settle_end = flight_at[i] + flight_dur[i];
        if (whip_mag != 0) {
            whip_sign = whip_sign == 0 ? 1 : -whip_sign;
            double signed_mag = whip_mag * whip_sign;
            double dA = js_max(0.5, flight_dur[i] * 0.55);
            double dB = js_max(0.45, flight_dur[i] - dA);
            double gap_next = (i + 1 < n ? cues[i + 1] : duration + 10) - cues[i];
            double settle_clamp =
                js_min(js_min(0.55, js_max(0.12, gap_next - 0.88)),
                       back_at - (cues[i] + 0.08) - 0.02);
            {
                ojson s = ojson::object();
                s["at"] = round3(flight_at[i]);
                s["duration"] = round3(dA);
                s["ease"] = "ease_in_quad";
                s["roll"] = round3(signed_mag);
                out.stops.push_back(move(s));
            }
            {
                ojson s = ojson::object();
                s["at"] = round3(flight_at[i] + dA);
                s["duration"] = round3(dB);
                s["ease"] = "back_out";
                s["target"] = ojson::array({round3(slot[0]), round3(slot[1]), round3(slot[2])});
                s["radius"] = round3(radius);
                s["elevation"] = round3(elevation);
                s["yaw"] = round3(fly_yaw);
                s["roll"] = round3(fly_roll);
                out.stops.push_back(move(s));
            }
            if (settle_clamp >= 0.1) {
                ojson s = ojson::object();
                s["at"] = round3(cues[i] + 0.08);
                s["duration"] = round3(settle_clamp);
                s["ease"] = "ease_out_quad";
                s["roll"] = 0;
                out.stops.push_back(move(s));
                flight_settle_end = cues[i] + 0.08 + settle_clamp;
            }
        } else {
            ojson s = ojson::object();
            s["at"] = round3(flight_at[i]);
            s["duration"] = round3(flight_dur[i]);
            s["ease"] = "ease_in_out_cubic";
            s["target"] = ojson::array({round3(slot[0]), round3(slot[1]), round3(slot[2])});
            s["radius"] = round3(radius);
            s["elevation"] = round3(elevation);
            s["yaw"] = round3(fly_yaw);
            s["roll"] = round3(fly_roll);
            out.stops.push_back(move(s));
        }

        double last_end = flight_settle_end;
        if (!groups_auto) {
            line_ends[i] = last_end;
            continue;
        }

        // The phrase: an optional kick on the beat itself, then one move per gap
        // between word groups.
        const vector<WordGroup>& groups_i = group_lists[i];
        double deadline = i + 1 < n ? flight_at[i + 1] : back_at;
        struct PlanItem {
            const ChoreoMove* m;
            double on;
            bool kick;
        };
        vector<PlanItem> plan;
        if (choreo[i].has_kick) plan.push_back({&choreo[i].kick, cues[i], true});
        for (size_t j = 1; j < groups_i.size(); j++)
            plan.push_back({&choreo[i].between[j - 1], groups_i[j].t, false});

        // One in-phrase whip on the widest word gap of a very energetic line —
        // at most one per line, and line 0 is always in (the opening is the
        // pitch). The sign is only spent when the pair is actually placed.
        int whip_gap_idx = -1;
        double whip_gap_scale = 0;
        if (whip > 0 && (energy_at(i) >= 0.78 || i == 0 || force_of(i)) && groups_i.size() > 2) {
            double best = 0;
            for (size_t j = 1; j < groups_i.size(); j++) {
                double gap_len = groups_i[j].t - groups_i[j - 1].t;
                if (gap_len > best) {
                    best = gap_len;
                    whip_gap_idx = (int)j - 1;
                }
            }
            if (best < 1.0) {
                whip_gap_idx = -1;
            } else {
                JsRng r2(to_uint32((double)i * 97.0 + 13.0));
                whip_gap_scale = i == 0 ? (M_PI / 2) * (0.85 + 0.1 * r2.next())
                                        : 0.5 + 0.35 * r2.next();
            }
        }

        double cur_r = radius, cur_e = elevation, cur_yaw = fly_yaw, cur_roll = fly_roll;
        for (int k = 0; k < (int)plan.size(); k++) {
            const ChoreoMove* m = plan[k].m;
            double on = plan[k].on;
            bool kick = plan[k].kick;
            double next_on = k + 1 < (int)plan.size() ? plan[k + 1].on : deadline + 0.35;
            double span = js_max(next_on - on, 0.18);

            // The in-phrase whip: bank in ahead of the word, unwind on it.
            // Falls through to the ordinary move when the gap turns out too
            // tight once the neighbouring stops have claimed their time.
            if (whip_gap_idx >= 0 && !kick && on == groups_i[whip_gap_idx + 1].t) {
                double dA = js_min(0.45, js_max(0.24, span * 0.3));
                double dB = js_min(0.55, js_max(0.28, span * 0.34));
                double at_a = js_max(last_end + 0.02, on - dA - 0.05);
                if (at_a + dA <= on - 0.01 && on + dB <= deadline - 0.02) {
                    whip_sign = whip_sign == 0 ? 1 : -whip_sign;
                    double mag = whip_gap_scale * js_min(whip, 1.0) * whip_sign;
                    {
                        ojson s = ojson::object();
                        s["at"] = round3(at_a);
                        s["duration"] = round3(dA);
                        s["ease"] = "ease_in_quad";
                        s["roll"] = round3(mag);
                        out.stops.push_back(move(s));
                    }
                    {
                        ojson s = ojson::object();
                        s["at"] = round3(on);
                        s["duration"] = round3(dB);
                        s["ease"] = "ease_out_quad";
                        s["roll"] = round3(cur_roll);
                        out.stops.push_back(move(s));
                    }
                    last_end = on + dB;
                    whip_gap_idx = -1;
                    continue;
                }
            }

            double dur = js_max(0.18, js_min(span * m->pace, 1.1));
            double at = kick ? on : on - m->lead - dur;
            at = js_max(at, last_end + 0.02);
            if (at + dur > deadline) dur = deadline - at;
            if (dur < 0.12) continue;

            const MoveSpec& spec = MOVES[m->spec_idx];
            double dx_n = spec.dx * m->mag;
            double dy_n = spec.dy * m->mag;
            // Shrink the move until the line is still comfortably inside the
            // frame at the new framing; six halvings are the budget, a move
            // that still fails is dropped rather than forced.
            double scale = 1.0;
            d3 T = slot;
            double n_r = cur_r, n_e = cur_e, n_y = cur_yaw, n_ro = cur_roll;
            bool ok = false;
            for (int tries = 0; tries < 6; tries++) {
                n_r = clamp_d(cur_r + spec.d_r * m->mag * scale, r_lo, r_hi);
                n_e = clamp_d(cur_e + spec.d_e * m->mag * scale, 0.0, 0.42);
                n_y = clamp_d(cur_yaw + spec.d_yaw * m->mag * scale, -0.26, 0.26);
                n_ro = clamp_d(cur_roll + spec.d_roll * m->mag * scale, -0.24, 0.24);
                double fw2 = half_w_at(n_r);
                double fh2 = half_h_at(n_r);
                // With the eye swung around the line, the pan direction turns too.
                BasisD b2 = camera_basis_d(n_r, n_e, {0, 0, 0}, base_yaw + n_y);
                T = {slot[0] + dx_n * scale * fw2 * b2.right[0] + dy_n * scale * fh2 * b2.up[0],
                     slot[1] + dx_n * scale * fw2 * b2.right[1] + dy_n * scale * fh2 * b2.up[1],
                     slot[2] + dx_n * scale * fw2 * b2.right[2] + dy_n * scale * fh2 * b2.up[2]};
                if (framing_fits(P, T, n_r, n_e, base_yaw + n_y, n_ro, box_w[i], box_h[i], th,
                                 aspect, fov)) {
                    ok = true;
                    break;
                }
                scale *= 0.5;
            }
            if (!ok) continue;
            // A move that ended up nowhere is not worth a stop.
            if (n_r == cur_r && n_e == cur_e && n_y == cur_yaw && n_ro == cur_roll &&
                fabs(T[0] - slot[0]) + fabs(T[1] - slot[1]) + fabs(T[2] - slot[2]) < 1e-3)
                continue;
            {
                ojson s = ojson::object();
                s["at"] = round3(at);
                s["duration"] = round3(dur);
                s["ease"] = m->ease;
                s["target"] = ojson::array({round3(T[0]), round3(T[1]), round3(T[2])});
                s["radius"] = round3(n_r);
                s["elevation"] = round3(n_e);
                s["yaw"] = round3(n_y);
                s["roll"] = round3(n_ro);
                out.stops.push_back(move(s));
            }
            cur_r = n_r;
            cur_e = n_e;
            cur_yaw = n_y;
            cur_roll = n_ro;
            last_end = at + dur;
        }
        line_ends[i] = last_end;
    }

    // Closing return to the title framing; the card is never banked.
    if (back_dur >= 0.4) {
        double br, be, byaw, broll;
        d3 bt;
        eval_orbit_d(rig, js_min(tail, duration), br, be, bt, byaw, broll);
        ojson s = ojson::object();
        s["at"] = round3(back_at);
        s["duration"] = round3(back_dur);
        s["ease"] = "ease_in_out_cubic";
        s["target"] = ojson::array({round3(bt[0]), round3(bt[1]), round3(bt[2])});
        s["radius"] = round3(br);
        s["elevation"] = round3(be);
        s["yaw"] = 0;
        s["roll"] = 0;
        out.stops.push_back(move(s));
    }

    // First moment each line is no longer in the shot: scan the camera path.
    ojson cam = rig;
    cam["stops"] = out.stops;
    for (int i = 0; i < n; i++) {
        const d3& P = out.positions[i];
        double from = js_max(js_max(cues[i] + fade_in + 0.15, reveal_end[i] + 0.15),
                             line_ends[i] + 0.15);
        double exit = numeric_limits<double>::quiet_NaN();
        for (double t = from; t <= duration; t += 0.1) {
            double br, be, byaw, broll;
            d3 bt;
            eval_orbit_d(cam, t, br, be, bt, byaw, broll);
            if (!box_on_frame(br, be, bt, byaw, broll, P, box_w[i], box_h[i], fov, aspect)) {
                exit = round3(t);
                break;
            }
        }
        out.exits.push_back(exit);
    }

    return out;
}


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
    bool groups_auto = false;  // --groups auto: word stamps compress onto groups
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
            // --groups auto: stamps compress onto their word group, then sweep
            // the glyphs inside a group 35 ms apart (groupCharTimes).
            if (o.groups_auto) {
                vector<double> g = group_char_times(line.text, ct, lyric_index);
                ct.clear();
                for (double v : g) ct.push_back(round3(v));
            }
        }
        double last_glyph = has_ct && !ct.empty() ? js_max_of(ct)   // Math.max
                                                  : t;
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

// Per-line audio energy in 0..1 from a precomputed envelope (loadEnergy): the
// mean band energy around each cue (0.8 s before, 1.5 s after), normalised so
// the loudest line is 1 and the quietest 0.
static vector<double> load_energy(const string& path, const vector<LrcLine>& lines) {
    ojson env = ojson::parse(read_file(path));
    if (!env.is_object() || !env.contains("data") || !env["data"].is_array() ||
        env["data"].empty())
        throw runtime_error("--energy-file: " + path + " has no envelope data");
    double fps = 60.0;  // Number(env.fps) || 60
    if (env.contains("fps") && env["fps"].is_number()) {
        double f = env["fps"].get<double>();
        if (f != 0 && !isnan(f)) fps = f;
    }
    double frames = (double)env["data"].size();
    vector<double> raw;
    for (const auto& l : lines) {
        double f0 = js_max(0.0, js_round((l.time - 0.8) * fps));
        double f1 = js_min(frames - 1.0, js_round((l.time + 1.5) * fps));
        double sum = 0;
        double n = 0;
        for (double f = f0; f <= f1; f++) {
            const ojson& row = env["data"][(size_t)f];
            if (!row.is_array() || row.empty()) continue;
            double m = 0;
            for (const auto& v : row) m += v.get<double>();
            sum += m / (double)row.size();
            n += 1;
        }
        raw.push_back(n ? sum / n : 0.0);
    }
    double mn = js_min_of(raw);  // Math.min(...raw)
    double mx = js_max_of(raw);
    double span = mx - mn > 1e-9 ? mx - mn : 1.0;
    vector<double> out;
    for (double v : raw) out.push_back(round3((v - mn) / span));
    return out;
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
        "  field layout (--layout field): --roll-power --force-whip\n"
        "          --energy-file (per-line 0..1 audio energy, js/audio-env.mjs output)\n"
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

        // Where the music is loud. Read before the layout so the roll budget is
        // spent by the track itself; absent the flag no line is ever
        // "energetic" and the camera path is exactly the one v9 laid out.
        // (loadEnergy validates the envelope the same way single layout does.)
        vector<double> energy;
        if (!a.energy_file.empty()) energy = load_energy(a.energy_file, lyric_lines);
        // Lines the author named for the whip treatment no matter the meter.
        vector<bool> whip_force;
        if (!a.force_whip.empty()) {
            for (const auto& l : lyric_lines) {
                bool f = false;
                for (const auto& txt : a.force_whip)
                    if (l.text.find(txt) != string::npos) {
                        f = true;
                        break;
                    }
                whip_force.push_back(f);
            }
        }
        FieldLayout layout;
        if (a.layout == "field") {
            // Same clamping buildLyricTexts applies, so the frame budget is
            // computed against the size the glyphs really get.
            double size_c = clamp_num(a.size.v, 1.5, 0.2, 12);
            double fade_in_c = clamp_num(a.fade_in.v, 0.3, 0, 10);
            double fov = 0.8726646;
            if (tpl.contains("camera") && tpl["camera"].is_object() &&
                tpl["camera"].contains("fov") && tpl["camera"]["fov"].is_number()) {
                double f = tpl["camera"]["fov"].get<double>();
                if (f != 0 && !isnan(f)) fov = f;
            }
            double tw = tpl.contains("width") && tpl["width"].is_number()
                            ? tpl["width"].get<double>()
                            : 0.0;
            if (tw == 0) tw = 1920;
            double th_ = tpl.contains("height") && tpl["height"].is_number()
                             ? tpl["height"].get<double>()
                             : 0.0;
            if (th_ == 0) th_ = 1080;
            double aspect = tw / th_;
            double whip = opt_finite(a.roll_power) ? clamp_d(a.roll_power.v, 0.0, 1.0) : 0.0;
            layout = layout_field(
                lyric_lines, camera_animation, duration, fov, aspect, size_c, fit_width,
                fade_in_c, isfinite(outro_at) ? outro_at : numeric_limits<double>::quiet_NaN(),
                title_out, a.groups == "auto", whip,
                a.energy_file.empty() ? nullptr : &energy,
                a.force_whip.empty() ? nullptr : &whip_force);
            camera_animation["stops"] = layout.stops;
        }

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
        lo.groups_auto = a.groups == "auto";
        vector<ojson> texts = build_lyric_texts(lyric_lines, lo);

        if (a.layout == "field") {
            // The blocks are all in the single slot buildLyricTexts put them in;
            // the layout owns their world position and the moment each one has
            // left frame.
            for (size_t i = 0; i < texts.size(); i++) {
                texts[i]["position"] = ojson::array({layout.positions[i][0],
                                                     layout.positions[i][1],
                                                     layout.positions[i][2]});
                ojson* fade = find_fade(texts[i]);
                if (fade && isfinite(layout.exits[i]))
                    (*fade)["fade_out_at"] = layout.exits[i];
            }
        }

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
    // The bundled installer copy wins; look for it relative to the exe too so
    // dist\chamina-cpp.exe keeps working from any cwd (node resolves the same
    // package through module resolution, independent of cwd).
    for (auto rel : {"js/node_modules/@ffmpeg-installer/win32-x64/ffmpeg.exe"}) {
        if (filesystem::exists(rel)) return norm(rel);
#ifdef _WIN32
        wchar_t exedir[MAX_PATH + 1] = {};
        GetModuleFileNameW(nullptr, exedir, MAX_PATH);
        error_code ec;
        filesystem::path cand =
            filesystem::path(exedir).parent_path() / ".." / filesystem::u8path(rel);
        if (filesystem::exists(cand, ec))
            return norm(cand.lexically_normal().u8string());
#endif
    }
    return "ffmpeg";
}

// ---------------------------------------------------------------------------
// audio: offline spectral envelope (port of node js/audio-env.mjs)
//
//   chamina-cpp audio <audio.(mp3|wav|m4a|...)> [-o out.json] [--fps 60]
//                     [--bands 64] [--lo 40] [--hi 14000]
//
// Output: {"fps":..,"bands":..,"frames":..,"lo":..,"hi":..,"data":[[..]]} in
// compact JSON.stringify form — exactly what the renderer's --audio-env
// reads. Decoding shells out to ffmpeg with the same argument set as the JS
// tool; the analysis reproduces the JS Float32Array rounding chain (f32
// storage, double arithmetic) so the JSON matches node byte for byte.
// ---------------------------------------------------------------------------

// ffmpeg decode → mono f32le @ rate, stdout read back as raw PCM. The 'b'
// matters on the popen fallback: without it CRT text mode mangles 0x0A bytes
// inside floats. ffmpeg's stderr passes through to ours; node captured it
// for its error text, we surface just the exit code.
static wstring utf8_wide(const string& s) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w(n > 0 ? (size_t)n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
#else
    (void)s;
    return wstring();
#endif
}

static vector<float> decode_audio(const string& input, unsigned rate) {
    string bytes;
#ifdef _WIN32
    // popen() runs the command through cmd with an ANSI(=ACP) command line:
    // with ACP 936 the UTF-8 input path fails conversion outright and _popen
    // returns NULL. Spawn ffmpeg directly with UTF-16 arguments and an
    // anonymous pipe for the PCM stream instead — correct on any codepage.
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) throw runtime_error("cannot create pipe");
    // The child inherits every inheritable handle, pipe ends included; our
    // read end must not leak in or EOF tracking breaks.
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);
    HANDLE err_dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE),
                    GetCurrentProcess(), &err_dup, 0, TRUE, DUPLICATE_SAME_ACCESS);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;        // JS decodes with stdio 'ignore'
    si.hStdOutput = wr;
    si.hStdError = err_dup ? err_dup : GetStdHandle(STD_ERROR_HANDLE);
    wstring cmd = L"\"" + utf8_wide(find_ffmpeg()) +
                  L"\" -hide_banner -loglevel error -i \"" + utf8_wide(input) +
                  L"\" -f f32le -acodec pcm_f32le -ac 1 -ar " + to_wstring(rate) +
                  L" pipe:1";
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                             nullptr, &si, &pi);
    if (nul) CloseHandle(nul);
    if (err_dup) CloseHandle(err_dup);
    CloseHandle(wr);  // our copy must go or ReadFile never sees EOF
    if (!ok) {
        CloseHandle(rd);
        throw runtime_error("cannot start ffmpeg");
    }
    char buf[1 << 16];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got > 0) bytes.append(buf, got);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) throw runtime_error("ffmpeg exited " + to_string(code));
#else
    string cmd = "\"\"" + find_ffmpeg() + "\" -hide_banner -loglevel error -i \""
                 + input + "\" -f f32le -acodec pcm_f32le -ac 1 -ar "
                 + to_string(rate) + " pipe:1\"";
    FILE* pipe = popen(cmd.c_str(), "rb");
    if (!pipe) throw runtime_error("cannot start ffmpeg");
    vector<char> buf(1 << 16);
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) bytes.append(buf.data(), n);
    int rc = pclose(pipe);
    if (rc != 0) throw runtime_error("ffmpeg exited " + to_string(rc));
#endif
    bytes.resize(bytes.size() - bytes.size() % 4);  // JS: buf.length >> 2 drops the tail
    vector<float> samples(bytes.size() / 4);
    for (size_t i = 0; i < samples.size(); i++) memcpy(&samples[i], &bytes[i * 4], 4);
    return samples;
}

// In-place iterative radix-2 FFT over re/im of length 2^n. Direct port of the
// JS FFT in js/audio-env.mjs — re/im are Float32Array there, so every
// butterfly store rounds to f32 while the twiddle products stay double; keep
// exactly that mix to reproduce node's bit patterns.
static void fft_env(float* re, float* im, size_t n) {
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            swap(re[i], re[j]);
            swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = (-2.0 * M_PI) / (double)len;
        double wr = cos(ang), wi = sin(ang);
        size_t half = len >> 1;
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < half; k++) {
                size_t a = i + k, l = a + half;
                double tr = (double)re[l] * cr - (double)im[l] * ci;
                double ti = (double)re[l] * ci + (double)im[l] * cr;
                re[l] = (float)((double)re[a] - tr);
                im[l] = (float)((double)im[a] - ti);
                re[a] = (float)((double)re[a] + tr);
                im[a] = (float)((double)im[a] + ti);
                double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

static int audio_main(int argc, char** argv) {
    const char* usage =
        "audio-env\n"
        "chamina-cpp audio <audio-file> [-o out.json] [--fps 60] [--bands 64]\n"
        "                  [--lo 40] [--hi 14000]\n";
    string input, out;
    double fps = 60, bands = 64, lo = 40, hi = 14000;
    try {
        auto next = [&](int& i) -> string {
            i++;
            if (i >= argc) throw runtime_error(string(argv[i - 1]) + " needs a value");
            return argv[i];
        };
        for (int i = 1; i < argc; i++) {
            string a = argv[i];
            if (a == "-o") out = next(i);
            else if (a == "--fps") fps = parse_num(next(i));
            else if (a == "--bands") bands = parse_num(next(i));
            else if (a == "--lo") lo = parse_num(next(i));
            else if (a == "--hi") hi = parse_num(next(i));
            else if (a == "-h" || a == "--help") {
                fputs(usage, stdout);
                return 0;
            } else if (!a.empty() && a[0] == '-')
                throw runtime_error("unknown flag " + a);
            else input = a;  // JS overwrites; the last positional wins
        }
        if (input.empty()) {
            fputs(usage, stdout);  // JS: console.log(USAGE) then exit 1
            return 1;
        }

        const unsigned RATE = 44100;
        vector<float> samples = decode_audio(input, RATE);

        const double hop = (double)RATE / fps;  // samples per frame
        const size_t N = 2048;
        // JS: frames = Math.max(0, Math.floor((samples.length - N) / hop)).
        // A NaN hop would slip past JS's `if (!frames)` guard and emit a
        // garbage file — we fail instead (error path only).
        double frames_d = floor(((double)samples.size() - (double)N) / hop);
        if (!(frames_d >= 1.0))
            throw runtime_error("audio too short (" + to_string(samples.size()) +
                                " samples)");
        long long frames = (long long)frames_d;

        // Log-spaced band edges across [lo, hi].
        size_t B = (size_t)bands;  // fractional bands crash node (RangeError); we truncate
        vector<long long> edges;
        for (size_t b = 0; b <= B; b++)
            edges.push_back(
                (long long)js_round(lo * pow(hi / lo, (double)b / (double)B)));
        double binHz = (double)RATE / (double)N;
        vector<double> bandBin;  // kept double: NaN here must poison the row to nulls
        for (size_t b = 0; b <= B; b++)
            bandBin.push_back(
                js_min((double)(N >> 1), js_max(1.0, js_round((double)edges[b] / binHz))));

        // Hann window, precomputed (Float32Array in JS → f32 rounding).
        vector<float> win(N);
        for (size_t i = 0; i < N; i++)
            win[i] = (float)(0.5 - 0.5 * cos((2.0 * M_PI * (double)i) / (double)(N - 1)));

        string body;
        body.reserve((size_t)frames * (2 + B * 8) + 64);
        body += "{\"fps\":";
        js_dtoa(fps, body);
        body += ",\"bands\":";
        js_dtoa(bands, body);
        body += ",\"frames\":";
        body += to_string(frames);  // Math.floor()'d → integer double
        body += ",\"lo\":";
        js_dtoa(lo, body);
        body += ",\"hi\":";
        js_dtoa(hi, body);
        body += ",\"data\":[";

        vector<float> re(N), im(N);
        const double DB_LO = -60;  // dB window floor
        for (long long f = 0; f < frames; f++) {
            long long off = (long long)js_round((double)f * hop);
            for (size_t i = 0; i < N; i++) {
                // JS reads past the end only in degenerate configs, where
                // undefined→NaN poisons the row to nulls; mirror that.
                double s = (size_t)off + i < samples.size()
                               ? (double)samples[(size_t)off + i] * (double)win[i]
                               : numeric_limits<double>::quiet_NaN();
                re[i] = (float)s;
                im[i] = 0.0f;
            }
            fft_env(re.data(), im.data(), N);
            body += f ? ",[" : "[";
            for (size_t b = 0; b < B; b++) {
                double v;
                double blo = bandBin[b], bhi = js_max(blo + 1.0, bandBin[b + 1]);
                if (isfinite(blo) && isfinite(bhi)) {
                    double sum = 0;
                    for (long long k = (long long)blo; k < (long long)bhi; k++)
                        sum += (double)re[k] * (double)re[k] + (double)im[k] * (double)im[k];
                    // Divide by N/2 (Hann coherent gain) so a full-scale sine
                    // peaks at 0 dB; without this every band of a loud master
                    // saturates the -60..0 window.
                    double mag = sqrt(sum / (bhi - blo)) / ((double)N / 2);
                    double db = 20.0 * log10(mag + 1e-9);
                    v = js_max(0.0, js_min(1.0, (db - DB_LO) / -DB_LO));
                } else {
                    v = numeric_limits<double>::quiet_NaN();
                }
                if (b) body += ',';
                js_dtoa(round3(v), body);
            }
            body += ']';
        }
        body += "]}";

        string out_path = out.empty() ? "target/chamina/audio_env.json" : out;
        {
            error_code ec;
            filesystem::path op = filesystem::u8path(out_path);
            if (op.has_parent_path()) filesystem::create_directories(op.parent_path(), ec);
            ofstream f(op, ios::binary);
            if (!f) throw runtime_error("cannot write " + out_path);
            f.write(body.data(), (streamsize)body.size());
            f.close();
            if (!f) throw runtime_error("cannot write " + out_path);
        }

        printf("audio-env: %s  frames=%lld bands=%zu %sfps\n", out_path.c_str(), frames,
               B, js_num_str(fps).c_str());
        return 0;
    } catch (const exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

// ffmpeg child fed raw RGB frames on stdin. Windows: direct CreateProcessW
// with UTF-16 arguments — popen's ANSI(=ACP) command line breaks non-ASCII
// -o paths (and dies outright when ACP can't decode the UTF-8 bytes); stdout
// is discarded and stderr passes through. Elsewhere: popen with cmd's
// quoting workaround.
struct VideoPipe {
#ifdef _WIN32
    HANDLE wr = nullptr, proc = nullptr, thread = nullptr;
    bool start(unsigned w, unsigned h, unsigned fps, const string& out) {
        SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE rd = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) return false;
        // Same inheritance trap: our write end must not leak into the child,
        // or it never sees EOF and waits forever after the last frame.
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, 0, nullptr);
        HANDLE err_dup = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE),
                        GetCurrentProcess(), &err_dup, 0, TRUE, DUPLICATE_SAME_ACCESS);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = rd;
        si.hStdOutput = nul;
        si.hStdError = err_dup ? err_dup : GetStdHandle(STD_ERROR_HANDLE);
        wstring cmd = L"\"" + utf8_wide(find_ffmpeg()) +
                      L"\" -y -hide_banner -loglevel error -f rawvideo -pix_fmt rgb24 -s " +
                      to_wstring(w) + L"x" + to_wstring(h) + L" -framerate " +
                      to_wstring(fps) + L" -i pipe:0 -c:v libx264 -preset medium "
                      L"-crf 18 -pix_fmt yuv420p -vf scale=trunc(iw/2)*2:trunc(ih/2)*2 "
                      L"-movflags +faststart \"" + utf8_wide(out) + L"\"";
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                 nullptr, &si, &pi);
        if (nul) CloseHandle(nul);
        if (err_dup) CloseHandle(err_dup);
        CloseHandle(rd);
        if (!ok) {
            CloseHandle(wr);
            wr = nullptr;
            return false;
        }
        proc = pi.hProcess;
        thread = pi.hThread;
        return true;
    }
    bool write(const void* p, size_t n) {
        const uint8_t* q = (const uint8_t*)p;
        while (n) {
            DWORD w = 0;
            size_t chunk = n < (size_t)1 << 20 ? n : (size_t)1 << 20;
            if (!WriteFile(wr, q, (DWORD)chunk, &w, nullptr) || w == 0) return false;
            q += w;
            n -= w;
        }
        return true;
    }
    int finish() {
        if (wr) CloseHandle(wr);  // EOF so ffmpeg can finalise the container
        wr = nullptr;
        if (!proc) return -1;
        WaitForSingleObject(proc, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(proc, &code);
        CloseHandle(proc);
        CloseHandle(thread);
        proc = thread = nullptr;
        return (int)code;
    }
#else
    FILE* f = nullptr;
    bool start(unsigned w, unsigned h, unsigned fps, const string& out) {
        // popen shells out through `cmd /c`, which eats the leading quote when
        // a line holds several — wrapping the whole command in one extra quote
        // pair keeps every inner quote intact.
        string cmd = "\"\"" + find_ffmpeg() + "\" -y -hide_banner -loglevel error -f rawvideo "
                     "-pix_fmt rgb24 -s " + to_string(w) + "x" + to_string(h) +
                     " -framerate " + to_string(fps) + " -i pipe:0 -c:v libx264 -preset medium "
                     "-crf 18 -pix_fmt yuv420p -vf scale=trunc(iw/2)*2:trunc(ih/2)*2 "
                     "-movflags +faststart \"" + out + "\"\"";
        f = popen(cmd.c_str(), "wb");
        return f != nullptr;
    }
    bool write(const void* p, size_t n) {
        fwrite(p, 1, n, f);
        return !ferror(f);
    }
    int finish() {
        if (!f) return -1;
        int rc = pclose(f);
        f = nullptr;
        return rc;
    }
#endif
};

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

// ---------------------------------------------------------------------------
// Public API implementation. This section used to be render_main() + main()
// in the single-TU engine: the scene parsing / font / mesh assembly moved
// into engine_create, the still branch into engine_still_png and the video
// branch into engine_encode_video. Everything above this section is the
// untouched engine core (only the file header comment and main() were
// dropped).
// ---------------------------------------------------------------------------
namespace chamina {

struct Engine {
    Engine(const json& s, const json& m) : scene(s), meta(m), world(scene, meta) {}
    // scene must outlive the meshes: PreparedText::anims / char_times point
    // into it (same lifetime rule as the original single-TU render_main).
    json scene;
    json meta;   // root settings (texts erased) — what finish() reads
    World world;
    FontEngine fe;
    int w = 0, h = 0;
    unsigned fps = 0, total = 0;
};

Engine* engine_create(const char* scene_json, const char* audio_env_json,
                      char* err, int err_cap) {
    auto fail = [&](const char* msg) -> Engine* {
        if (err && err_cap > 0) snprintf(err, (size_t)err_cap, "%s", msg);
        return nullptr;
    };
    try {
        json scene = json::parse(scene_json ? scene_json : "");

        // Malformed / minimal scene compat: fill in what older or hand-made
        // scenes omit (defaults mirror gen's own flags) instead of hard-throwing.
        // Valid scenes never hit these paths. One warning, once.
        {
            bool warned = false;
            auto warn_once = [&]() {
                if (!warned) {
                    fprintf(stderr, "[cpp] warn: scene had missing/invalid fields — filled with defaults\n");
                    warned = true;
                }
            };
            if (!scene.contains("texts") || !scene["texts"].is_array()) {
                scene["texts"] = json::array();
                warn_once();
            }
            for (auto& t : scene["texts"]) {
                if (!t.is_object()) {
                    t = json::object();
                    warn_once();
                }
                auto ensure_text = [&](const char* k, const char* d) {
                    if (!t.contains(k) || !t[k].is_string()) {
                        t[k] = d;
                        warn_once();
                    }
                };
                ensure_text("text", "");
                if (!t.contains("position") || !t["position"].is_array() ||
                    t["position"].size() < 3 || !t["position"][0].is_number() ||
                    !t["position"][1].is_number() || !t["position"][2].is_number()) {
                    t["position"] = json::array({0.0, -0.4, 0.09});
                    warn_once();
                }
                if (!t.contains("size") || !t["size"].is_number()) {
                    t["size"] = 1.4;
                    warn_once();
                }
                if (!t.contains("alpha") || !t["alpha"].is_number()) {
                    t["alpha"] = 1.0;
                    warn_once();
                }
                if (!t.contains("animations") || !t["animations"].is_array()) {
                    t["animations"] = json::array();
                    warn_once();
                } else
                    for (auto& an : t["animations"])
                        if (!an.is_object()) {
                            an = json::object();
                            warn_once();
                        }
                if (!t.contains("material") || !t["material"].is_object()) {
                    t["material"] = json{{"color", "#ffffff"}, {"strength", 1.6}};
                    warn_once();
                } else {
                    auto& m = t["material"];
                    if (!m.contains("color") || !m["color"].is_string()) {
                        m["color"] = "#ffffff";
                        warn_once();
                    }
                    if (m.contains("outline") && !m["outline"].is_object()) {
                        m.erase("outline");  // malformed rim → treat as none
                        warn_once();
                    } else if (m.contains("outline") &&
                               (!m["outline"].contains("color") ||
                                !m["outline"]["color"].is_string())) {
                        m.erase("outline");
                        warn_once();
                    }
                }
            }
            if (!scene.contains("backgrounds") || !scene["backgrounds"].is_array())
                scene["backgrounds"] = json::array();
            for (auto& bg : scene["backgrounds"]) {
                if (!bg.is_object()) {
                    bg = json::object();
                    warn_once();
                }
                auto ensure_str = [&](const char* k, const char* d) {
                    if (!bg.contains(k) || !bg[k].is_string()) {
                        bg[k] = d;
                        warn_once();
                    }
                };
                // Neutral sky so a half-filled key stays visible rather than fatal.
                ensure_str("top", "#0a1020");
                ensure_str("bottom", "#05070f");
                ensure_str("accent", "#3a4a66");
                if (!bg.contains("at") || !bg["at"].is_number()) {
                    bg["at"] = 0.0;
                    warn_once();
                }
            }
        }

        // The authored scene carries its settings at the root (no meta wrapper).
        json meta = scene;
        meta.erase("texts");
        {
            bool fps_ok = meta.contains("fps") && meta["fps"].is_number() &&
                          meta["fps"].get<float>() > 0;
            bool dur_ok = meta.contains("duration") && meta["duration"].is_number();
            if (!fps_ok) {
                meta["fps"] = 30;
                fprintf(stderr, "[cpp] warn: scene fps missing/invalid — defaulting to 30\n");
            }
            if (!dur_ok) {
                meta["duration"] = 300;
                fprintf(stderr, "[cpp] warn: scene duration missing — defaulting to 300s\n");
            }
            if (!meta.contains("total_frames"))
                meta["total_frames"] =
                    (unsigned)ceil(meta.at("duration").get<float>() * meta.at("fps").get<float>());
        }
        // Older / minimal scenes may omit whole sections; normalise once so every
        // downstream .at() is safe.
        if (!meta.contains("fx")) meta["fx"] = json{{"enabled", false}};
        if (!meta.contains("bloom")) meta["bloom"] = json{{"enabled", false}};
        if (!meta.contains("dof")) meta["dof"] = json{{"enabled", false}};
        if (!meta.contains("backgrounds")) meta["backgrounds"] = json::array();
        if (!meta.contains("camera"))
            meta["camera"] = json{{"fov", 0.8726646f}, {"near", 0.01f}, {"far", 500.0f}};

        Engine* e = new Engine(scene, meta);

        TRACE = getenv("CPP_TRACE") != nullptr;
        if (audio_env_json && *audio_env_json) {
            json ej = json::parse(audio_env_json);
            e->world.env.fps = ej.value("fps", 60.0f);
            for (const auto& row : ej.at("data")) e->world.env.data.push_back(row.get<vector<float>>());
            e->world.has_env = true;
        }

        // fonts
        bool font_ok = false;
        for (const auto& f : meta.value("font_fallback", json::array())) {
            if (e->fe.open(f.get<string>())) {
                font_ok = true;
                break;
            }
        }
        if (!font_ok) {
            delete e;
            return fail("no usable font");
        }

        float duration = meta.at("total_frames").get<float>() / meta.at("fps").get<float>();
        (void)duration;
        e->world.prepare({});
        // cues/outro/first-lyric are computed in prepare() — they need env-independent
        // info only, fine.

        for (const auto& spec : e->scene.at("texts")) {
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
                uint32_t cp = e->fe.codepoint(text, i, nx);
                i = nx;
                int glyph = e->fe.find_glyph(cp);
                float adv, lsb;
                e->fe.h_metrics(glyph, adv, lsb);
                max_asc = max(max_asc, e->fe.ascender);
                min_desc = min(min_desc, e->fe.descender);
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
                bool empty = it.glyph == 0 || e->fe.glyph_empty(it.glyph);
                PreparedChar ch;
                ch.idx = gi;
                ch.lands_at = 0;
                for (const auto& a : spec.at("animations")) {
                    if (a.value("type", "") == "fade") {
                        float sa = a.value("start_at", 0.0f), stg = a.value("stagger", 0.04f);
                        if (cts && gi < cts->size() && (*cts)[gi].is_number())
                            ch.lands_at = (*cts)[gi].get<float>();
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
                if (!build_glyph_mesh(e->fe, it.glyph, depth, ch.mesh, org_x, org_y)) {
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
            e->world.texts.push_back(move(tx));
        }
        // sparks need hw — rebuild after texts
        {
            uint32_t r = 0;
            for (size_t bi = 0; bi < e->world.texts.size(); bi++) {
                auto& tx = e->world.texts[bi];
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
                    e->world.sparks.p.push_back(sp);
                }
            }
        }

        e->w = meta.at("width").get<int>();
        e->h = meta.at("height").get<int>();
        e->fps = meta.at("fps").get<unsigned>();
        e->total = meta.at("total_frames").get<unsigned>();
        if (err && err_cap > 0) err[0] = '\0';
        return e;
    } catch (const exception& ex) {
        return fail(ex.what());
    } catch (...) {
        return fail("unknown exception");
    }
}

void engine_destroy(Engine* e) { delete e; }

// Shared per-frame tail for the direct-API paths (render_rgb / still_png):
// render + audio-reactive flash + post chain. The flash formula is the
// video branch's, kept verbatim from render_main. encode_video keeps its
// own inline copy because it interleaves per-stage profiling between the
// stages (CPP_PROF), which this helper must not distort.
static void engine_render_frame(Engine* e, float t, vector<uint8_t>& rgb) {
    static Frame fr;
    e->world.render_frame_into(t, fr);
    auto& fx = e->meta.at("fx");
    float flash = 0;
    if (fx.value("enabled", false)) {
        float bands[64], bass, energy;
        e->world.env.sample(t, bands, bass, energy);
        float hit = hit_envelope(e->world.cues, t, fx.value("hit_duration", 0.35f));
        flash = fx.value("flash", 0.0f) * hit * (0.45f + 0.55f * energy);
    }
    finish(fr, e->meta, flash, rgb);  // fills in place, keeps rgb's capacity
}

void engine_render_rgb(Engine* e, float t, unsigned char* rgb) {
    if (!e || !rgb) return;
    size_t n = (size_t)e->w * (size_t)e->h * 3;
    try {
        static vector<uint8_t> buf;  // reused across frames (capacity retained)
        engine_render_frame(e, t, buf);
        if (n && buf.size() >= n) memcpy(rgb, buf.data(), n);
    } catch (const exception& ex) {
        fprintf(stderr, "error: %s\n", ex.what());
        if (n) memset(rgb, 0, n);
    } catch (...) {
        fprintf(stderr, "error: unknown exception\n");
        if (n) memset(rgb, 0, n);
    }
}

int engine_still_png(Engine* e, float t, const char* png_path) {
    if (!e) return 1;
    string png = png_path ? png_path : "";
    try {
        static vector<uint8_t> rgb;
        engine_render_frame(e, t, rgb);
        // stbi_write_png opens through the ANSI codepage and fails on Chinese
        // paths; rasterize to memory and write through a wide path instead.
        vector<uint8_t> png_bytes;
        auto png_sink = [](void* ctx, void* data, int size) {
            auto& buf = *(vector<uint8_t>*)ctx;
            const uint8_t* d = (const uint8_t*)data;
            buf.insert(buf.end(), d, d + size);
        };
        if (!stbi_write_png_to_func(png_sink, &png_bytes, e->w, e->h, 3, rgb.data(), e->w * 3)) {
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
        return 0;
    } catch (const exception& ex) {
        fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "error: unknown exception\n");
        return 1;
    }
}

int engine_encode_video(Engine* e, unsigned from, unsigned frames, const char* out_path) {
    if (!e) return 1;
    string out = out_path && *out_path ? out_path : "cpp_out.mp4";
    try {
        int w = e->w, h = e->h;
        unsigned fps = e->fps, total = e->total;
        unsigned count = frames ? frames : total - from;
        count = min(count, total - from);

        VideoPipe vpipe;
        if (!vpipe.start(w, h, fps, out)) {
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
            e->world.render_frame_into(t, fr);
            auto tf = chrono::steady_clock::now();
            auto& fx = e->meta.at("fx");
            float flash = 0;
            if (fx.value("enabled", false)) {
                float bands[64], bass, energy;
                e->world.env.sample(t, bands, bass, energy);
                float hit = hit_envelope(e->world.cues, t, fx.value("hit_duration", 0.35f));
                flash = fx.value("flash", 0.0f) * hit * (0.45f + 0.55f * energy);
            }
            static vector<uint8_t> rgb;
            finish(fr, e->meta, flash, rgb);
            prof_render += chrono::duration<double>(tf - tr).count();
            prof_finish += chrono::duration<double>(chrono::steady_clock::now() - tf).count();
            if (!vpipe.write(rgb.data(), rgb.size())) {
                // A dead ffmpeg turns the pipe into a silent sink — surface it
                // instead of "done"-ing over a missing file.
                fprintf(stderr, "[cpp] ERROR: ffmpeg pipe broke at frame %u\n", i + 1);
                vpipe.finish();
                return 1;
            }
            if ((i + 1) % 60 == 0 || i + 1 == count) {
                double el = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
                fprintf(stderr, "[cpp] frame %u/%u  %.2f fps  elapsed %.1fs  eta %.0fs\n",
                        i + 1, count, (i + 1) / el, el, el / (i + 1) * (count - i - 1));
            }
        }
        int rc = vpipe.finish();
        double el = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
        if (rc != 0) {
            fprintf(stderr, "[cpp] ERROR: ffmpeg exited %d — output may be missing or bad\n", rc);
            return 1;
        }
        fprintf(stderr, "[cpp] done -> %s  %u frames  %.2f fps\n",
                out.c_str(), count, count / el);
        if (prof)
            fprintf(stderr, "[prof] render %.1fms/f  finish %.1fms/f\n",
                    prof_render / count * 1000, prof_finish / count * 1000);
        return 0;
    } catch (const exception& ex) {
        fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    } catch (...) {
        fprintf(stderr, "error: unknown exception\n");
        return 1;
    }
}

int engine_width(const Engine* e) { return e ? e->w : 0; }
int engine_height(const Engine* e) { return e ? e->h : 0; }
unsigned engine_fps(const Engine* e) { return e ? e->fps : 0; }
unsigned engine_total_frames(const Engine* e) { return e ? e->total : 0; }

void set_trace(bool on) { TRACE = on; }

bool read_file(const char* path, std::string& out) {
    try {
        out = ::read_file(path ? path : "");
        return true;
    } catch (...) {
        out.clear();
        return false;
    }
}

char** argv_utf8(int argc, char** argv, int* out_argc) {
#ifdef _WIN32
    // One decode per program; a second call would invalidate the first
    // result (the storage is reassigned). Callers keep it for the program's
    // lifetime.
    static vector<string> hold;
    static vector<char*> ptrs;
    hold = ::utf8_argv(argc, argv);
    ptrs.clear();
    for (auto& s : hold) ptrs.push_back(&s[0]);
    ptrs.push_back(nullptr);
    if (out_argc) *out_argc = (int)hold.size();
    return ptrs.data();
#else
    if (out_argc) *out_argc = argc;
    return argv;
#endif
}

// CLI-shaped content tools — the exe is a thin wrapper around these.
int gen_main(int argc, char** argv) { return ::gen_main(argc, argv); }
int audio_main(int argc, char** argv) { return ::audio_main(argc, argv); }

} // namespace chamina

#endif // CHAMINA_IMPLEMENTATION

#endif // CHAMINA_H_INCLUDE
