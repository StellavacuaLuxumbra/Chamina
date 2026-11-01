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
    const char* dbg = getenv("CPP_DEBUG_PIXEL");
    int dbgx = -1, dbgy = -1;
    if (dbg) sscanf(dbg, "%d,%d", &dbgx, &dbgy);
    if (dbgx >= 0 && dbgx < w && dbgy >= 0 && dbgy < h)
        fprintf(stderr, "[dbg] pre-bloom @%d,%d = %.3f %.3f %.3f\n", dbgx, dbgy,
                f.color[(size_t)dbgy * w + dbgx][0],
                f.color[(size_t)dbgy * w + dbgx][1],
                f.color[(size_t)dbgy * w + dbgx][2]);
    if (meta.at("bloom").value("enabled", false)) {
        static Img bl;
        bloom(f, meta.at("bloom").at("intensity").get<float>(),
              meta.at("bloom").at("threshold").get<float>(), bl);
        if (dbgx >= 0 && dbgx < w && dbgy >= 0 && dbgy < h) {
            auto& b = bl[(size_t)dbgy * w + dbgx];
            fprintf(stderr, "[dbg] bloom-add @%d,%d = %.3f %.3f %.3f\n", dbgx, dbgy,
                    b[0], b[1], b[2]);
            float best = -1.0f;
            size_t bi = 0;
            for (size_t i = 0; i < bl.size(); i++)
                if (bl[i][2] > best) { best = bl[i][2]; bi = i; }
            fprintf(stderr, "[dbg] max blue in bloom = %.3f @%zu,%zu\n", best,
                    bi % (size_t)w, bi / (size_t)w);
        }
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
