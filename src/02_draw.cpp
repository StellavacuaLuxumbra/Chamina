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
        // CPP_SKIP_PARTICLES isolates the always-on blue dust from the rest of
        // the frame (debugging bloom contamination; same spirit as
        // CPP_SKIP_CLOUD / CPP_SKIP_STARS).
        if (!getenv("CPP_SKIP_PARTICLES"))
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
