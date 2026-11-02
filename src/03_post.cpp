// ---------------------------------------------------------------------------
// post (port of src/native/post.rs)
// ---------------------------------------------------------------------------

using Img = vector<array<float, 3>>;

// Separable 2D gaussian blur into a caller-owned buffer: horizontal pass into
// a scratch image, then a vertical pass from that scratch. The old port
// inherited a bug from the Rust reference where the vertical pass read `src`
// again, so bloom/DOF only blurred vertically (visible as vertical light
// shafts and colour bleed between lines). Fixed here; all buffers persist
// across frames (sizes are constant per scene, so resize() only allocates on
// the first call).
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
    static Img tmp;
    if ((size_t)w * h > tmp.size()) tmp.resize((size_t)w * h);
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
                auto& a = tmp[min(y + i, h - 1) * (size_t)w + x];
                auto& b = tmp[max(y - i, 0) * (size_t)w + x];
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
    // three.js ACESFilmicToneMapping, in the shader's effective row order:
    // the GLSL mat3(...) constants are columns, so the rows below are the
    // transposes of the vectors written in tonemapping_pars_fragment.glsl.js.
    // The port originally used the columns as rows and dropped the /0.6
    // exposure, which made over-bright warm colours tone-map magenta.
    const float M[3][3] = {{0.59719f, 0.35458f, 0.04823f},
                           {0.07600f, 0.90834f, 0.01566f},
                           {0.02840f, 0.13383f, 0.83777f}};
    const float MO[3][3] = {{1.60475f, -0.53108f, -0.07367f},
                            {-0.10208f, 1.10813f, -0.00605f},
                            {-0.00327f, -0.07276f, 1.07602f}};
    const float exposure = 1.0f / 0.6f; // three.js: toneMappingExposure / 0.6
    float r = c[0] * exposure, g = c[1] * exposure, b = c[2] * exposure;
    float fitted[3];
    for (int i = 0; i < 3; i++)
        fitted[i] = aces_fit(r * M[i][0] + g * M[i][1] + b * M[i][2]);
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
        // three.js ACES: colour *= exposure/0.6, then the input matrix (rows
        // below are the transpose of the GLSL vec3 columns), fit, output
        // matrix. Mirrors tonemap_srgb's op order for the Tier A byte gate.
        __m256 c0 = _mm256_mul_ps(
            _mm256_i32gather_ps(src, base, 4), _mm256_set1_ps(1.0f / 0.6f));
        __m256 c1 = _mm256_mul_ps(
            _mm256_i32gather_ps(src, _mm256_add_epi32(base, kone), 4),
            _mm256_set1_ps(1.0f / 0.6f));
        __m256 c2 = _mm256_mul_ps(
            _mm256_i32gather_ps(src, _mm256_add_epi32(base, ktwo), 4),
            _mm256_set1_ps(1.0f / 0.6f));
        // fitted[j] = c0*M[j][0] + c1*M[j][1] + c2*M[j][2], mul/add order unchanged
        __m256 f0 = (c0 * _mm256_set1_ps(0.59719f) + c1 * _mm256_set1_ps(0.35458f)) +
                    c2 * _mm256_set1_ps(0.04823f);
        __m256 f1 = (c0 * _mm256_set1_ps(0.07600f) + c1 * _mm256_set1_ps(0.90834f)) +
                    c2 * _mm256_set1_ps(0.01566f);
        __m256 f2 = (c0 * _mm256_set1_ps(0.02840f) + c1 * _mm256_set1_ps(0.13383f)) +
                    c2 * _mm256_set1_ps(0.83777f);
        f0 = aces_fit_lanes256(f0, lutA);
        f1 = aces_fit_lanes256(f1, lutA);
        f2 = aces_fit_lanes256(f2, lutA);
        // v[i] = clamp(f0*MO[i][0] + f1*MO[i][1] + f2*MO[i][2], 0, 1) → srgb LUT
        __m256 v0 = ((f0 * _mm256_set1_ps(1.60475f) + f1 * _mm256_set1_ps(-0.53108f)) +
                     f2 * _mm256_set1_ps(-0.07367f));
        __m256 v1 = ((f0 * _mm256_set1_ps(-0.10208f) + f1 * _mm256_set1_ps(1.10813f)) +
                     f2 * _mm256_set1_ps(-0.00605f));
        __m256 v2 = ((f0 * _mm256_set1_ps(-0.00327f) + f1 * _mm256_set1_ps(-0.07276f)) +
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
