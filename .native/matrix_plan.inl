// 相邻矩阵的分解只在输入改变时计算；每次绘制仍使用实际插值进度。
struct MatrixPlan {
    // mode=0保留原矩阵；1线性；2插值轴角与长度；3轴角恒定。
    // 极分解布局：[su, dsu, au, dau, sv, dsv, av, dav, tx, dtx, ty, dty]。
    // mode=3的角度槽替换为cos/sin常量；mode=1为六个初值和六个差值。
    float v[12]{};
    uint8_t mode = 0;
};
static MatrixPlan PrepareMatrix(const uint16_t* m0h, const uint16_t* m1h) {
    MatrixPlan p;
    float a0 = HalfToFloat(m0h[0]), b0 = HalfToFloat(m0h[1]), c0 = HalfToFloat(m0h[2]), d0 = HalfToFloat(m0h[3]);
    float a1 = HalfToFloat(m1h[0]), b1 = HalfToFloat(m1h[1]), c1 = HalfToFloat(m1h[2]), d1 = HalfToFloat(m1h[3]);
    float tx0 = HalfToFloat(m0h[4]), ty0 = HalfToFloat(m0h[5]);
    float dtx = HalfToFloat(m1h[4]) - tx0, dty = HalfToFloat(m1h[5]) - ty0;
    if (fabsf(dtx) > 420 || fabsf(dty) > 420 || (a0 * d0 - b0 * c0) * (a1 * d1 - b1 * c1) < 0) return p;
    float su0 = sqrtf(a0 * a0 + c0 * c0), su1 = sqrtf(a1 * a1 + c1 * c1);
    float sv0 = sqrtf(b0 * b0 + d0 * d0), sv1 = sqrtf(b1 * b1 + d1 * d1);
    if (su0 > 1e-4f && su1 > 1e-4f && sv0 > 1e-4f && sv1 > 1e-4f) {
        float au0 = atan2f(c0, a0), au1 = atan2f(c1, a1), du = WrapAngleDiff(au1 - au0);
        float av0 = atan2f(d0, b0), av1 = atan2f(d1, b1), dv = WrapAngleDiff(av1 - av0);
        if (fabsf(du) > 2.35f || fabsf(dv) > 2.35f) return p;
        p.mode = 2;
        const float v[] = {su0, su1 - su0, au0, du, sv0, sv1 - sv0, av0, dv, tx0, dtx, ty0, dty};
        memcpy(p.v, v, sizeof(v));
        // 两根轴的角度不变时，三角函数也是常量，不在每帧重复计算。
        if (du == 0 && dv == 0) {
            p.mode = 3;
            p.v[2] = cosf(au0 + du * 0.5f);
            p.v[3] = sinf(au0 + du * 0.5f);
            p.v[6] = cosf(av0 + dv * 0.5f);
            p.v[7] = sinf(av0 + dv * 0.5f);
        }
    } else {
        p.mode = 1;
        for (unsigned k = 0; k < 6; ++k) {
            p.v[k] = HalfToFloat(m0h[k]);
            p.v[6 + k] = HalfToFloat(m1h[k]) - p.v[k];
        }
    }
    return p;
}
// SSE2是Windows x64的基础指令集；按原标量算法处理舍入、下溢和饱和。
static __m128i PackHalf4(__m128 values) {
    const __m128i bits = _mm_castps_si128(values);
    const __m128i sign = _mm_and_si128(_mm_srli_epi32(bits, 16), _mm_set1_epi32(0x8000));
    const __m128i exponent =
        _mm_sub_epi32(_mm_and_si128(_mm_srli_epi32(bits, 23), _mm_set1_epi32(255)), _mm_set1_epi32(112));
    const __m128i rounded =
        _mm_srli_epi32(_mm_add_epi32(_mm_and_si128(bits, _mm_set1_epi32(0x7fffff)), _mm_set1_epi32(0x1000)), 13);
    const __m128i adjusted = _mm_add_epi32(exponent, _mm_srli_epi32(rounded, 10));
    const __m128i raw = _mm_or_si128(_mm_slli_epi32(adjusted, 10), _mm_and_si128(rounded, _mm_set1_epi32(1023)));
    const __m128i saturated = _mm_cmpgt_epi32(adjusted, _mm_set1_epi32(30));
    const __m128i clamped =
        _mm_or_si128(_mm_andnot_si128(saturated, raw), _mm_and_si128(saturated, _mm_set1_epi32(0x7bff)));
    const __m128i valid = _mm_cmpgt_epi32(exponent, _mm_setzero_si128());
    const __m128i half = _mm_or_si128(sign, _mm_and_si128(valid, clamped));
    const __m128i signedHalf = _mm_srai_epi32(_mm_slli_epi32(half, 16), 16);
    return _mm_packs_epi32(signedHalf, _mm_setzero_si128());
}
static void ApplyMatrix(const MatrixPlan& p, float t, uint16_t* out) {
    __m128 values;
    float tx, ty;
    if (p.mode == 1) {
        values = _mm_add_ps(_mm_loadu_ps(p.v), _mm_mul_ps(_mm_loadu_ps(p.v + 6), _mm_set1_ps(t)));
        tx = p.v[4] + p.v[10] * t;
        ty = p.v[5] + p.v[11] * t;
    } else {
        const float su = p.v[0] + p.v[1] * t, sv = p.v[4] + p.v[5] * t;
        float a, b, c, d;
        if (p.mode == 3) {
            a = su * p.v[2];
            c = su * p.v[3];
            b = sv * p.v[6];
            d = sv * p.v[7];
        } else {
            const float au = p.v[2] + p.v[3] * t, av = p.v[6] + p.v[7] * t;
            a = su * cosf(au);
            c = su * sinf(au);
            b = sv * cosf(av);
            d = sv * sinf(av);
        }
        values = _mm_setr_ps(a, b, c, d);
        tx = p.v[8] + p.v[9] * t;
        ty = p.v[10] + p.v[11] * t;
    }
    _mm_storel_epi64(reinterpret_cast<__m128i*>(out), PackHalf4(values));
    const uint32_t translations = static_cast<uint32_t>(_mm_cvtsi128_si32(PackHalf4(_mm_setr_ps(tx, ty, 0, 0))));
    memcpy(out + 4, &translations, 4);
}
