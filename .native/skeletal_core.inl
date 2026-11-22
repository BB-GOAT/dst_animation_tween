struct AnimElement {
    uint32_t layer_hash;   // +0x00
    uint8_t  meta0[12];    // +0x04..+0x0F
    uint32_t symbol_hash;  // +0x10
    uint8_t  meta1[12];    // +0x14..+0x1F
    uint32_t frame_num;    // +0x20
    uint16_t m[6];         // +0x24..+0x2F: 6 IEEE-754 float16 [a, b, c, d, tx, ty]
};
static_assert(sizeof(AnimElement) == 48, "AnimElement must be 48 bytes");

struct AnimFrame {
    float        bbox[6];      // +0x00..+0x17
    AnimElement* elements;     // +0x18
    uint32_t     num_elements; // +0x20
    uint32_t     pad;          // +0x24
};
static_assert(sizeof(AnimFrame) == 40, "AnimFrame must be 40 bytes");

struct Animation {
    uint8_t    pad0[0x08];
    AnimFrame* frames;     // +0x08
    float      framerate;  // +0x10
    uint8_t    pad1[0x24];
    uint32_t   num_frames; // +0x38
    uint32_t   pad2;
    float      duration;   // +0x40
};

// Portable branchless IEEE-754 float16 <-> float32 (runs on 100% of x86-64 CPUs, zero F16C requirement!)
// Exact match with Klei's own half_to_float decoder @ 0x1400F3A8D.
static inline float HalfToFloat(uint16_t h) {
    uint32_t exp_mant = (h & 0x7FFFu);
    uint32_t bits = (h & 0x7C00u) ? ((exp_mant + 0x1C000u) << 13) : 0u;
    bits |= (static_cast<uint32_t>(h & 0x8000u) << 16);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static inline uint16_t FloatToHalf(float f) {
    uint32_t x;
    memcpy(&x, &f, sizeof(x));
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((x >> 23) & 0xFFu) - 112;
    uint32_t mant = x & 0x007FFFFFu;
    if (exp <= 0) return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
    uint32_t rounded = (mant + 0x1000u) >> 13;
    if (rounded >= 0x400u) {
        rounded = 0;
        ++exp;
        if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | rounded);
}

static inline float WrapAngleDiff(float d) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 6.28318530717958647692f;
    if (d > kPi) d -= kTwoPi;
    else if (d < -kPi) d += kTwoPi;
    return d;
}

static inline bool TweenElementMatrix(const uint16_t m0h[6], const uint16_t m1h[6], float t, uint16_t outh[6]) {
    float a0 = HalfToFloat(m0h[0]), b0 = HalfToFloat(m0h[1]);
    float c0 = HalfToFloat(m0h[2]), d0 = HalfToFloat(m0h[3]);
    float tx0 = HalfToFloat(m0h[4]), ty0 = HalfToFloat(m0h[5]);

    float a1 = HalfToFloat(m1h[0]), b1 = HalfToFloat(m1h[1]);
    float c1 = HalfToFloat(m1h[2]), d1 = HalfToFloat(m1h[3]);
    float tx1 = HalfToFloat(m1h[4]), ty1 = HalfToFloat(m1h[5]);

    float dtx = tx1 - tx0;
    float dty = ty1 - ty0;
    // Boss-scaled guard (420px): allows giant boss slams/lunges (Daywalker/Deerclops/Dragonfly)
    // while still blocking cross-screen teleports.
    if (fabsf(dtx) > 420.0f || fabsf(dty) > 420.0f) return false;

    float det0 = a0 * d0 - b0 * c0;
    float det1 = a1 * d1 - b1 * c1;
    if (det0 * det1 < 0.0f) return false;

    float su0 = sqrtf(a0 * a0 + b0 * b0);
    float su1 = sqrtf(a1 * a1 + b1 * b1);
    float sv0 = sqrtf(c0 * c0 + d0 * d0);
    float sv1 = sqrtf(c1 * c1 + d1 * d1);

    float a, b, c, d;
    if (su0 > 1e-4f && su1 > 1e-4f && sv0 > 1e-4f && sv1 > 1e-4f) {
        float au0 = atan2f(b0, a0), au1 = atan2f(b1, a1);
        float du = WrapAngleDiff(au1 - au0);
        float av0 = atan2f(d0, c0), av1 = atan2f(d1, c1);
        float dv = WrapAngleDiff(av1 - av0);
        // Allow fast weapon swings & boss attacks up to ~135 deg/tick (2.35 rad)
        if (fabsf(du) > 2.35f || fabsf(dv) > 2.35f) return false;

        float su = su0 + (su1 - su0) * t, au = au0 + du * t;
        float sv = sv0 + (sv1 - sv0) * t, av = av0 + dv * t;
        a = su * cosf(au); b = su * sinf(au);
        c = sv * cosf(av); d = sv * sinf(av);
    } else {
        a = a0 + (a1 - a0) * t; b = b0 + (b1 - b0) * t;
        c = c0 + (c1 - c0) * t; d = d0 + (d1 - d0) * t;
    }

    outh[0] = FloatToHalf(a);
    outh[1] = FloatToHalf(b);
    outh[2] = FloatToHalf(c);
    outh[3] = FloatToHalf(d);
    outh[4] = FloatToHalf(tx0 + dtx * t);
    outh[5] = FloatToHalf(ty0 + dty * t);
    return true;
}

struct AnimBufferSlot {
    AnimFrame frame{};
    std::vector<AnimElement> elems;
    const AnimFrame* original = nullptr;
};

struct EntityAnimTrack {
    uintptr_t   entity_key = 0;
    const void* anim = nullptr;
    float       last_time = -1.0f;
    float       prev_f = 0.0f;
    float       curr_f = 0.0f;
    int64_t     tick_qpc = 0;
    int64_t     seen_qpc = 0;
    float       tick_period = 1.0f / 30.0f;
    uint8_t     valid = 0;
};

constexpr size_t kTrackSlots = 8192;

struct ThreadLocalAnimState {
    // 一次顶层绘制可能递归绘制多个附属动画；返回过的帧在这轮结束前不能覆盖。
    // unique_ptr保证扩容不移动已有槽位；下一次顶层绘制再复用全部分配。
    std::vector<std::unique_ptr<AnimBufferSlot>> slots;
    size_t ring = 0;
    std::vector<EntityAnimTrack> tracks;
    LONG64 epoch = 0;
    ThreadLocalAnimState() : tracks(kTrackSlots) {}
    AnimBufferSlot& NextSlot() {
        if (ring == slots.size()) slots.push_back(std::make_unique<AnimBufferSlot>());
        return *slots[ring++];
    }
};

thread_local uintptr_t g_tlsLastParentKey = 0;
thread_local ThreadLocalAnimState* g_tlsAnimState = nullptr;

static ThreadLocalAnimState& GetThreadAnimState() {
    if (g_tlsAnimState) return *g_tlsAnimState;
    thread_local ThreadLocalAnimState state;
    g_tlsAnimState = &state;
    return state;
}

static void BeginAnimDraw() { GetThreadAnimState().ring = 0; }

// 绘制读取插值副本，帧序号计算必须使用原始数组中的帧身份。
static const AnimFrame* ResolveFrameIdentity(const AnimFrame* frame) noexcept {
    if (!g_tlsAnimState) return frame;
    for (size_t i = 0; i < g_tlsAnimState->ring; ++i) {
        const auto& slot = *g_tlsAnimState->slots[i];
        if (frame == &slot.frame && slot.original) return slot.original;
    }
    return frame;
}

#include "matrix_plan.inl"
#include "animation_plan.inl"

static const void* InterpGetFrameCore(const void* animPtr, int32_t playMode, float time, uintptr_t key, bool isMainParent, bool frameGeometry = false) {
    if (!animPtr || !g_origAnimGetFrame) {
        return g_origAnimGetFrame ? g_origAnimGetFrame(animPtr, playMode, time) : nullptr;
    }
    if (InterlockedCompareExchange(&g_skeletalTweenEnabled, 0, 0) == 0) {
        return g_origAnimGetFrame(animPtr, playMode, time);
    }

    const auto* anim = static_cast<const Animation*>(animPtr);
    uint32_t nframes = anim->num_frames;
    float fps = anim->framerate;
    if (nframes <= 1 || !anim->frames || !std::isfinite(fps) || !std::isfinite(time) || fps <= 0.0f || fps > 240.0f) {
        return g_origAnimGetFrame(animPtr, playMode, time);
    }

    auto& tls = GetThreadAnimState();
    // 新VM重新启用或手动切换后，不复用上一代实体地址对应的播放历史。
    const LONG64 epoch = InterlockedCompareExchange64(&g_planEpoch, 0, 0);
    if (tls.epoch != epoch) {
        for (auto& track : tls.tracks) track = {};
        tls.epoch = epoch;
        // 顶层入口已刷新父实体；附属动画不能在此清除它。
    }
    if (isMainParent && key != 0) {
        g_tlsLastParentKey = key;
    }
    if (key == 0) {
        return g_origAnimGetFrame(animPtr, playMode, time);
    }

    // Compute continuous floating-point frame coordinate (works for 30fps, 40fps,
    // SetDeltaTimeMultiplier speed scaling, looping, one-shot, and percentage animations).
    float nf = static_cast<float>(nframes);
    float curF = 0.0f;
    if (playMode == 1) {
        float t = (time < 0.0f) ? 0.0f : time;
        curF = fmodf(t * fps, nf);
        if (curF < 0.0f) curF += nf;
    } else if (playMode == 0) {
        float t = (time < 0.0f) ? 0.0f : time;
        curF = t * fps;
        if (curF > nf - 1.0f) curF = nf - 1.0f;
    } else if (playMode == 2) {
        float c = (time < 0.0f) ? 0.0f : ((time > 1.0f) ? 1.0f : time);
        curF = c * (nf - 1.0f);
    } else {
        return g_origAnimGetFrame(animPtr, playMode, time);
    }

    LARGE_INTEGER nowLi{};
    QueryPerformanceCounter(&nowLi);
    int64_t nowQpc = nowLi.QuadPart;

    // 4-probe open-addressing lookup with LRU eviction (0% hash-collision eviction in megabases)
    size_t baseSlot = ((key >> 4) ^ (key >> 15) ^ (key >> 27)) & (kTrackSlots - 1);
    EntityAnimTrack* trPtr = nullptr;
    EntityAnimTrack* lruPtr = &tls.tracks[baseSlot];
    for (size_t p = 0; p < 4; ++p) {
        EntityAnimTrack& cand = tls.tracks[(baseSlot + p) & (kTrackSlots - 1)];
        if (cand.valid && cand.entity_key == key) { trPtr = &cand; break; }
        if (!cand.valid) { trPtr = &cand; break; }
        if (cand.seen_qpc < lruPtr->seen_qpc) lruPtr = &cand;
    }
    if (!trPtr) trPtr = lruPtr;
    EntityAnimTrack& tr = *trPtr;
    tr.seen_qpc = nowQpc;

    auto discreteFrame = [&]() -> const void* {
        uint32_t fi = static_cast<uint32_t>(curF + 0.001f);
        if (fi >= nframes) fi = (playMode == 1) ? (fi % nframes) : (nframes - 1);
        return &anim->frames[fi];
    };

    if (!tr.valid || tr.entity_key != key || tr.anim != animPtr) {
        tr.entity_key = key;
        tr.anim = animPtr;
        tr.last_time = time;
        tr.prev_f = curF;
        tr.curr_f = curF;
        tr.tick_qpc = nowQpc;
        tr.tick_period = 1.0f / 30.0f;
        tr.valid = 1;
        return discreteFrame();
    }

    if (time != tr.last_time) {
        float oldF = tr.curr_f;
        float df = curF - oldF;
        if (playMode == 1 && df < 0.0f) {
            df += nf;
        }
        float elapsed = static_cast<float>(static_cast<double>(nowQpc - tr.tick_qpc) * g_qpcInvFreq);
        bool smoothStep = (df > 0.01f && df <= 4.5f) ||
                          (playMode == 2 && fabsf(df) > 0.01f && fabsf(df) <= 4.5f);
        if (smoothStep) {
            if (elapsed > 0.008f && elapsed < 0.120f) {
                tr.tick_period = tr.tick_period * 0.7f + elapsed * 0.3f;
            }
            tr.prev_f = oldF;
            tr.curr_f = curF;
        } else {
            tr.prev_f = curF;
            tr.curr_f = curF;
        }
        tr.last_time = time;
        tr.tick_qpc = nowQpc;
    }

    float stepF = tr.curr_f - tr.prev_f;
    if (playMode == 1 && stepF < 0.0f) stepF += nf;
    if (fabsf(stepF) < 0.001f) {
        return discreteFrame();
    }

    float dtSinceTick = static_cast<float>(static_cast<double>(nowQpc - tr.tick_qpc) * g_qpcInvFreq);
    float alpha = (tr.tick_period > 1e-4f) ? (dtSinceTick / tr.tick_period) : 1.0f;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    float fNow = tr.prev_f + stepF * alpha;
    uint32_t idx0 = 0, idx1 = 0;
    float frac = 0.0f;

    if (playMode == 1) {
        while (fNow >= nf) fNow -= nf;
        while (fNow < 0.0f) fNow += nf;
        float fl = floorf(fNow);
        idx0 = static_cast<uint32_t>(fl) % nframes;
        idx1 = (idx0 + 1) % nframes;
        frac = fNow - fl;
    } else {
        if (fNow < 0.0f) fNow = 0.0f;
        if (fNow >= nf - 1.0f) return &anim->frames[nframes - 1];
        float fl = floorf(fNow);
        idx0 = static_cast<uint32_t>(fl);
        idx1 = idx0 + 1;
        frac = fNow - fl;
    }

    if (idx0 == idx1 || idx0 >= nframes || idx1 >= nframes) {
        return &anim->frames[idx0 < nframes ? idx0 : 0];
    }

    const AnimFrame& f0 = anim->frames[idx0];
    const AnimFrame& f1 = anim->frames[idx1];
    uint32_t n0 = f0.num_elements, n1 = f1.num_elements;
    if (n0 > 512 || n1 > 512 || (n0 && !f0.elements) || (n1 && !f1.elements)) return g_origAnimGetFrame(animPtr, playMode, time);

    // 只读取本次调用仍有效的资源；缓存中的旧地址从不用于读取。
    const AnimFrame& previous = anim->frames[idx0 ? idx0 - 1 : 0];
    const PairPlan* prepared = nullptr;
    try { prepared = LookupPair(anim, idx0, previous, f0, f1, epoch); }
    catch (...) { return g_origAnimGetFrame(animPtr, playMode, time); }
    if (!prepared) return g_origAnimGetFrame(animPtr, playMode, time);
    const auto& plan = *prepared;
    if (plan.bypass) return g_origAnimGetFrame(animPtr, playMode, time);

    if (frac <= 0.015f) return &f0;
    if (frac >= 0.985f) return &f1;
    if (plan.matrices.empty()) return (frameGeometry || !plan.trajectory) && frac >= 0.5f ? &f1 : &f0;

    AnimBufferSlot& bufSlot = tls.NextSlot();
    const auto& anchor = frac >= 0.5f ? f1 : f0;
    const bool endpointOrder = frameGeometry || !plan.trajectory;
    const uint32_t outputCount = endpointOrder ? anchor.num_elements : static_cast<uint32_t>(plan.matrices.size());
    if (bufSlot.elems.size() < outputCount) bufSlot.elems.resize(outputCount);

    if (endpointOrder) {
        // 普通输出也使用端点的真实元素列表：未配对的出生/消失不制造跨实例运动，
        // 图层、遮挡次序与换片同时切换；只有对应明确的矩阵参与插值。
        // 预生成顶点使用原始帧编号和该帧的元素槽位，不能和另一帧的矩阵列表混用。
        // 几何、贴片、meta及元素次序一起选择端点；每个槽位再应用其对应的插值矩阵。
        bufSlot.original = frameGeometry ? &anchor :
            static_cast<const AnimFrame*>(g_origAnimGetFrame(animPtr, playMode, time));
        if (!bufSlot.original) return nullptr;
        bufSlot.frame = anchor;
        bufSlot.frame.elements = bufSlot.elems.data();
        const auto& order = frac >= 0.5f ? plan.secondOrder : plan.firstOrder;
        if (outputCount) memcpy(bufSlot.elems.data(), anchor.elements, outputCount * sizeof(AnimElement));
        for (uint32_t i = 0; i < outputCount; ++i) {
            const int slot = order[i];
            if (slot >= 0 && plan.matrices[slot].mode) ApplyMatrix(plan.matrices[slot], frac, bufSlot.elems[i].m);
        }
        if (frameGeometry) InterlockedIncrement64(&g_frameGeometryFrames);
    } else {
        // 使用游戏自己的取帧结果保留当前播放时间的帧身份，而非猜测索引。
        bufSlot.original = static_cast<const AnimFrame*>(g_origAnimGetFrame(animPtr, playMode, time));
        if (!bufSlot.original) return nullptr;

        bufSlot.frame = f0;
        bufSlot.frame.elements = bufSlot.elems.data();
        bufSlot.frame.num_elements = outputCount;
        // meta 中含有引擎字符串缓存指针，输出必须从本次有效输入复制。
        for (uint32_t i = 0; i < outputCount; ++i) {
            const auto& ref = plan.sources[i];
            bufSlot.elems[i] = ref.target ? f1.elements[ref.index] : f0.elements[ref.index];
        }

        for (uint32_t i = 0; i < outputCount; ++i) {
            if (plan.matrices[i].mode) ApplyMatrix(plan.matrices[i], frac, bufSlot.elems[i].m);
            if (plan.swaps[i] && frac >= 0.5f && plan.matrices[i].mode) {
                bufSlot.elems[i].frame_num = plan.targetSprite[i];
            }
        }
    }
    const auto tweenedCount = plan.tweened;
    const auto pass2Count = plan.pass2;

    if (tweenedCount > 0) {
        InterlockedIncrement64(&g_animInterpFrames);
        InterlockedAdd64(&g_animElemsTweened, static_cast<LONG64>(tweenedCount));
        if (pass2Count > 0) InterlockedAdd64(&g_animPass2Swaps, static_cast<LONG64>(pass2Count));
    }
    return &bufSlot.frame;
}

extern "C" const void* FastInterpGetFrameMain(const void* animPtr, int32_t playMode, float time, const void* renderCommand) {
    BeginAnimDraw();
    if (!renderCommand) return g_origAnimGetFrame ? g_origAnimGetFrame(animPtr, playMode, time) : nullptr;
    const auto key = ReadGeometryField<uintptr_t>(renderCommand, 8);
    // 即使单帧或关闭补帧，也刷新父实体，避免后续附属通道继承上一个实体。
    g_tlsLastParentKey = key;
    return InterpGetFrameCore(animPtr, playMode, time, key, true, UsesFrameGeometry(renderCommand));
}

extern "C" const void* FastInterpGetFrameUI(const void* animPtr, int32_t playMode, float time, const void* persistentAnimNode) {
    BeginAnimDraw();
    g_tlsLastParentKey = reinterpret_cast<uintptr_t>(persistentAnimNode);
    return InterpGetFrameCore(animPtr, playMode, time, reinterpret_cast<uintptr_t>(persistentAnimNode), true);
}

// 附属绘制记录由每轮渲染快照重建，记录地址不能作为跨帧实例身份。
// 使用父挂接路径、目标符号和该符号下的子项序号；递归期间保存并恢复父路径。
struct OverlayDrawContext { const void* item; const void* node; };
thread_local uintptr_t g_tlsOverlayChildKey = 0;

static uintptr_t OverlayTrackKey(const void* animPtr, const OverlayDrawContext& draw) noexcept {
    if (!g_tlsLastParentKey || !draw.item || !draw.node) return 0;
    const uintptr_t begin = ReadGeometryField<uintptr_t>(draw.node, 0x30);
    const uintptr_t end = ReadGeometryField<uintptr_t>(draw.node, 0x38);
    const uintptr_t item = reinterpret_cast<uintptr_t>(draw.item);
    if (item < begin || item >= end || (item - begin) % 0x120) return 0;
    const uintptr_t index = (item - begin) / 0x120;
    const uint32_t symbol = ReadGeometryField<uint32_t>(draw.node, 0x18);
    uintptr_t key = g_tlsLastParentKey ^ (reinterpret_cast<uintptr_t>(animPtr) * 0x9E3779B97F4A7C15ULL);
    key ^= static_cast<uintptr_t>(symbol) * 0xD6E8FEB86659FD93ULL;
    key ^= (index + 1) * 0xA0761D6478BD642FULL;
    key ^= key >> 32;
    return key | 1ULL;
}

extern "C" const void* FastInterpGetFrameOverlay(const void* animPtr, int32_t playMode, float time,
                                                 const OverlayDrawContext* draw) {
    g_tlsOverlayChildKey = draw ? OverlayTrackKey(animPtr, *draw) : 0;
    return InterpGetFrameCore(animPtr, playMode, time, g_tlsOverlayChildKey, false);
}

class OverlayPathScope {
    uintptr_t parent = g_tlsLastParentKey, child = g_tlsOverlayChildKey;
public:
    OverlayPathScope() noexcept { g_tlsLastParentKey = child; g_tlsOverlayChildKey = 0; }
    ~OverlayPathScope() { g_tlsLastParentKey = parent; g_tlsOverlayChildKey = child; }
};
